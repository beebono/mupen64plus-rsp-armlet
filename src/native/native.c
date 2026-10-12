/*
 * arMLEt - native.c: the native-routine framework: registry, selection per ucode, the fork's hook callbacks, verify mode, statistics, capture
 *
 * See native.h for the routine interface and the exactness contract, docs/design.md for the design.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "armlet.h"
#include "gfx_ucode.h"
#include "native.h"
#include "native_internal.h"

#define MAX_ROUTINES 64
#define MAX_SEEN 64

/* registration section: the sentinel keeps the section (and so the __start / __stop symbols) alive when no family defines routines */
static const struct armlet_routine *const l_sentinel __attribute__((used, section("armlet_routines"))) = NULL;
extern const struct armlet_routine *const __start_armlet_routines[];
extern const struct armlet_routine *const __stop_armlet_routines[];

struct evbuf {
    PrspNativeEvent *v;
    size_t n, cap;
};

struct rows {
    uint8_t *p;
    size_t n, cap;
    uint32_t count;
};

static struct {
    struct armlet_native_settings set;
    int started;
    const struct armlet_routine *reg[MAX_ROUTINES];
    struct armlet_native_stats stats[MAX_ROUTINES];
    unsigned n_reg;
    unsigned active[MAX_ROUTINES], n_active;    /* indexes into reg */
    uint64_t ucode;                             /* the ucode the selection is for (0: none yet) */
    PrspNativeView view;
    PrspNativeHost host;
    struct evbuf *cur_ev;
    struct evbuf ev_native, ev_jit;
    struct rows rows;
    PrspNativeSnap *s0, *sn, *sj;
    uint64_t seen_hash[MAX_SEEN];
    unsigned n_seen;
    unsigned captured[MAX_ROUTINES];
    uint64_t call_no, mismatch_total, verify_total, tasks;
    struct sigaction wd_old;
    unsigned clock_hz;
} G;

/* ---- time ---- */
static inline uint64_t now_ticks(void)
{
#if defined(__aarch64__)
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static double ticks_to_ns(uint64_t t)
{
#if defined(__aarch64__)
    if (!G.clock_hz) {
        uint64_t f;
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
        G.clock_hz = (unsigned)f;
    }
    return (double)t * 1e9 / G.clock_hz;
#else
    return (double)t;
#endif
}

/* ---- registry ---- */
uint64_t armlet_native_imem_hash(const uint32_t *imem, unsigned lo, unsigned hi)
{
    uint64_t h = 0xcbf29ce484222325ull;
    unsigned i;
    for (i = lo >> 2; i < (hi >> 2) && i < 1024; i++)
        h = (h ^ imem[i]) * 0x100000001b3ull;
    return h;
}

static void registry_build(void)
{
    const struct armlet_routine *const *p;
    if (G.n_reg)
        return;
    for (p = __start_armlet_routines; p < __stop_armlet_routines; p++)
        if (*p && G.n_reg < MAX_ROUTINES)
            G.reg[G.n_reg++] = *p;
}

unsigned armlet_native_count(void) { return G.n_reg; }
const struct armlet_routine *armlet_native_routine(unsigned idx) { return idx < G.n_reg ? G.reg[idx] : NULL; }
struct armlet_native_stats *armlet_native_stats_of(unsigned idx) { return &G.stats[idx]; }

/* allow-list: "f3dex2" (family), "f3dex2.vtx" (routine); "-name" removes; empty list = all. Later items win. */
static int name_matches(const char *item, size_t n, const char *name)
{
    size_t ln = strlen(name);
    if (n == ln && !strncasecmp(item, name, n))
        return 1;
    return n < ln && name[n] == '.' && !strncasecmp(item, name, n);
}

static int allowed_by_list(const char *list, const char *name)
{
    int pass, allowed = strncmp(name, "test.", 5) != 0;     /* test routines only when named */
    for (pass = 0; pass < 2; pass++) {
        const char *s = list;
        while (*s) {
            const char *e;
            size_t n;
            int neg = 0;
            while (*s == ',' || *s == ' ')
                s++;
            if (!*s)
                break;
            e = s;
            while (*e && *e != ',')
                e++;
            if (*s == '-') {
                neg = 1;
                s++;
            }
            n = (size_t)(e - s);
            while (n && s[n - 1] == ' ')
                n--;
            if (pass == 0 && !neg)
                allowed = 0;                /* a positive item: everything not listed is off */
            if (pass == 1 && name_matches(s, n, name))
                allowed = !neg;
            s = e;
        }
    }
    return allowed;
}

unsigned armlet_native_select(uint64_t ucode_id)
{
    uint16_t entries[MAX_ROUTINES], splits[MAX_ROUTINES * 8];
    unsigned i, k, ne = 0, ns = 0;

    G.ucode = ucode_id;
    G.n_active = 0;
    for (i = 0; i < G.n_reg; i++) {
        const struct armlet_routine *r = G.reg[i];
        if (r->ucode != ucode_id || !allowed_by_list(G.set.routines, r->name))
            continue;
        G.active[G.n_active++] = i;
        entries[ne++] = (uint16_t)r->entry_pc;
        for (k = 0; k < r->n_exit_pcs && ns < sizeof(splits) / sizeof(splits[0]); k++)
            splits[ns++] = r->exit_pcs[k];
    }
    prsp_native_set_pcs(entries, ne, splits, ns);
    return G.n_active;
}

/* ---- events ---- */
static void ev_push(struct evbuf *b, const PrspNativeEvent *e)
{
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 256;
        b->v = realloc(b->v, b->cap * sizeof(*b->v));
        if (!b->v)
            abort();
    }
    b->v[b->n++] = *e;
}

static void host_event(void *user, const PrspNativeEvent *e)
{
    (void)user;
    if (G.cur_ev)
        ev_push(G.cur_ev, e);
}

/* ---- env helpers ---- */
void armlet_mtc0(struct armlet_env *e, unsigned rd, uint32_t value)
{
    int m = prsp_native_mtc0(rd, value);
    if (m != PRSP_NATIVE_MODE_CONTINUE && e->mode == PRSP_NATIVE_MODE_CONTINUE)
        e->mode = m;
}

uint32_t armlet_mfc0(struct armlet_env *e, unsigned rd, unsigned rt)
{
    int m = PRSP_NATIVE_MODE_CONTINUE;
    uint32_t v = prsp_native_mfc0(rd, rt, &m);
    if (m != PRSP_NATIVE_MODE_CONTINUE && e->mode == PRSP_NATIVE_MODE_CONTINUE)
        e->mode = m;
    return v;
}

void armlet_dma_read(struct armlet_env *e, unsigned mem_addr, unsigned dram_addr, uint32_t len)
{
    armlet_mtc0(e, 0, mem_addr);
    armlet_mtc0(e, 1, dram_addr);
    armlet_mtc0(e, 2, len);
}

void armlet_dma_write(struct armlet_env *e, unsigned mem_addr, unsigned dram_addr, uint32_t len)
{
    armlet_mtc0(e, 0, mem_addr);
    armlet_mtc0(e, 1, dram_addr);
    armlet_mtc0(e, 3, len);
}

static void env_init(struct armlet_env *e, const struct armlet_routine *r, unsigned pc)
{
    memset(e, 0, sizeof(*e));
    e->rsp.sr = G.view.sr;
    e->rsp.vr = G.view.vr;
    e->rsp.acc = G.view.acc;
    e->rsp.flags = G.view.flags;
    e->rsp.div_out = G.view.div_out;
    e->rsp.div_in = G.view.div_in;
    e->rsp.dp_flag = G.view.dp_flag;
    e->rsp.dmem = G.view.dmem;
    e->rsp.imem = G.view.imem;
    e->entry_pc = pc;
    e->mode = PRSP_NATIVE_MODE_CONTINUE;
    e->routine = r;
}

/* ---- the fork's callbacks ---- */
static int host_lookup(void *user, unsigned pc, const uint32_t *imem, PrspNativeMatch *m)
{
    unsigned a;
    (void)user;
    for (a = 0; a < G.n_active; a++) {
        const struct armlet_routine *r = G.reg[G.active[a]];
        uint64_t h;
        unsigned i;
        if (r->entry_pc != pc)
            continue;
        h = armlet_native_imem_hash(imem, r->dep_lo, r->dep_hi);
        if (h == r->imem_hash) {
            m->cookie = G.active[a];
            m->dep_lo = r->dep_lo;
            m->dep_hi = r->dep_hi;
            return 1;
        }
        for (i = 0; i < G.n_seen; i++)
            if (G.seen_hash[i] == (h ^ ((uint64_t)G.active[a] << 56)))
                break;
        if (i == G.n_seen && G.n_seen < MAX_SEEN) {
            G.seen_hash[G.n_seen++] = h ^ ((uint64_t)G.active[a] << 56);
            armlet_log(M64MSG_INFO, "native %s: IMEM [%03x,%03x) at the entry %03x hashes to %016" PRIx64 ", the routine expects %016" PRIx64 "; not used",
                       r->name, r->dep_lo, r->dep_hi, pc, h, r->imem_hash);
        }
    }
    return 0;
}

/* ---- verify ---- */
struct report {
    char buf[4096];
    size_t n;
    unsigned count;
};

static void rep(struct report *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void rep(struct report *r, const char *fmt, ...)
{
    va_list ap;
    int k;
    r->count++;
    if (r->n + 200 > sizeof(r->buf))
        return;
    va_start(ap, fmt);
    k = vsnprintf(r->buf + r->n, sizeof(r->buf) - r->n, fmt, ap);
    va_end(ap);
    if (k > 0)
        r->n += (size_t)k;
}

static void vec_str(char *out, const uint16_t *v)
{
    sprintf(out, "%04x %04x %04x %04x %04x %04x %04x %04x", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
}

/* a = native, b = the JIT's. flags: bit0 CMD_END seen (DPC registers belong to the video plugin), bit1 the oracle ran invalidate_code */
static void compare_snaps(struct report *r, const PrspNativeSnap *a, const PrspNativeSnap *b, const struct armlet_routine *rt,
                          const struct evbuf *ea, const struct evbuf *eb, int cmd_end, int invalidated)
{
    unsigned i, k;
    char x[80], y[80];

    for (i = 1; i < 32; i++)
        if (a->sr[i] != b->sr[i] && !(rt->dead_sr >> i & 1))
            rep(r, "  sr[%u]: native %08x, JIT %08x\n", i, a->sr[i], b->sr[i]);
    for (i = 0; i < 32; i++)
        if (memcmp(a->vr[i], b->vr[i], 16) && !(rt->dead_vr >> i & 1)) {
            vec_str(x, a->vr[i]);
            vec_str(y, b->vr[i]);
            rep(r, "  vr[%u]:\n    native %s\n    JIT    %s\n", i, x, y);
        }
    if (!rt->dead_acc)
        for (i = 0; i < 3; i++)
            if (memcmp(a->acc + 8 * i, b->acc + 8 * i, 16)) {
                vec_str(x, a->acc + 8 * i);
                vec_str(y, b->acc + 8 * i);
                rep(r, "  acc %s:\n    native %s\n    JIT    %s\n", i == 0 ? "HI" : i == 1 ? "MD" : "LO", x, y);
            }
    if (!rt->dead_flags)
        for (i = 0; i < 3; i++)
            if (memcmp(a->flags[i], b->flags[i], 32)) {
                uint16_t fa = 0, fb = 0;
                for (k = 0; k < 8; k++) {
                    fa |= (a->flags[i][8 + k] ? 1 : 0) << k | (a->flags[i][k] ? 1 : 0) << (8 + k);
                    fb |= (b->flags[i][8 + k] ? 1 : 0) << k | (b->flags[i][k] ? 1 : 0) << (8 + k);
                }
                rep(r, "  %s: native %04x, JIT %04x (lanes %s)\n", i == 0 ? "VCO" : i == 1 ? "VCC" : "VCE", fa, fb,
                    fa == fb ? "differ although the bit masks agree" : "differ");
            }
    if (!rt->dead_div && (a->div_out != b->div_out || a->div_in != b->div_in || a->dp_flag != b->dp_flag))
        rep(r, "  divide unit: native out %04x in %04x dp %d, JIT out %04x in %04x dp %d\n", (uint16_t)a->div_out, (uint16_t)a->div_in,
            a->dp_flag, (uint16_t)b->div_out, (uint16_t)b->div_in, b->dp_flag);
    for (i = 0; i < 1024; i++)
        if (a->dmem[i] != b->dmem[i]) {
            if (r->count < 40)
                rep(r, "  DMEM[%03x]: native %08x, JIT %08x\n", i * 4, a->dmem[i], b->dmem[i]);
            else
                r->count++;
        }
    for (i = 0; i < 1024; i++)
        if (a->imem[i] != b->imem[i])
            rep(r, "  IMEM[%03x]: native %08x, JIT %08x\n", i * 4, a->imem[i], b->imem[i]);
    for (i = 0; i < 16; i++) {
        if (cmd_end && i >= 8 && i <= 10)
            continue;
        if (a->cr[i] != b->cr[i])
            rep(r, "  SP/DPC register %u: native %08x, JIT %08x\n", i, a->cr[i], b->cr[i]);
    }
    if ((a->irq & (cmd_end ? 1u : ~0u)) != (b->irq & (cmd_end ? 1u : ~0u)))
        rep(r, "  MI_INTR: native %08x, JIT %08x\n", a->irq, b->irq);
    if (!invalidated && a->dirty_blocks != b->dirty_blocks)
        rep(r, "  IMEM dirty blocks: native %08x, JIT %08x\n", a->dirty_blocks, b->dirty_blocks);
    for (i = 0; i < 32; i++)
        if (a->mfc0_count[i] != b->mfc0_count[i])
            rep(r, "  status-poll counter %u: native %d, JIT %d\n", i, a->mfc0_count[i], b->mfc0_count[i]);
    if (a->sp_status_timeout != b->sp_status_timeout)
        rep(r, "  SP_STATUS_TIMEOUT: native %d, JIT %d\n", a->sp_status_timeout, b->sp_status_timeout);
    if (a->has_delay_slot != b->has_delay_slot)
        rep(r, "  delay slot pending: native %u, JIT %u (the exit pc is a delay slot?)\n", a->has_delay_slot, b->has_delay_slot);

    if (ea->n != eb->n)
        rep(r, "  effectful SP register writes: native %zu, JIT %zu\n", ea->n, eb->n);
    for (i = 0; i < ea->n && i < eb->n; i++)
        if (memcmp(&ea->v[i], &eb->v[i], sizeof(ea->v[i]))) {
            rep(r, "  SP write #%u: native kind %u %08x %08x %08x %016" PRIx64 "\n              JIT    kind %u %08x %08x %08x %016" PRIx64 "\n", i,
                ea->v[i].kind, ea->v[i].a, ea->v[i].b, ea->v[i].c, ea->v[i].hash, eb->v[i].kind, eb->v[i].a, eb->v[i].b, eb->v[i].c,
                eb->v[i].hash);
            break;
        }
}

static int events_have_cmd_end(const struct evbuf *e)
{
    size_t i;
    for (i = 0; i < e->n; i++)
        if (e->v[i].kind == PRSP_EV_CMD_END)
            return 1;
    return 0;
}

static void mismatch(const struct armlet_routine *rt, unsigned pc, int exit_pc, struct report *r, const char *what)
{
    G.mismatch_total++;
    armlet_log(M64MSG_ERROR, "native %s MISMATCH (%s) call %" PRIu64 ", entry %03x, native exit %03x, %u differences; first ones:\n%s", rt->name,
               what, G.call_no, pc, exit_pc, r->count, r->buf);
    if (G.set.abort_on_mismatch)
        abort();
}

/* The RDRAM the routine's DMA reads read, as { u32 addr, u32 len, bytes } rows, for a snapshot file (the same arithmetic as the fork's
 * rsp_dma_read). Called while RDRAM is as the routine saw it. */
static void rows_add(struct rows *r, uint32_t src, uint32_t length)
{
    uint32_t hdr[2] = { src, length }, j;
    size_t need = r->n + 8 + length;
    if (need > r->cap) {
        r->cap = need * 2;
        r->p = realloc(r->p, r->cap);
        if (!r->p)
            abort();
    }
    memcpy(r->p + r->n, hdr, 8);
    r->n += 8;
    for (j = 0; j < length; j += 4) {
        uint32_t w = G.view.rdram[((src + j) & 0x7ffffc) >> 2];
        memcpy(r->p + r->n, &w, 4);
        r->n += 4;
    }
    r->count++;
}

static void rows_collect(struct rows *r, const struct evbuf *ev)
{
    size_t i;
    r->n = 0;
    r->count = 0;
    for (i = 0; i < ev->n; i++) {
        uint32_t cache, dram, lenreg, length, skip;
        unsigned count, row;
        if (ev->v[i].kind != PRSP_EV_DMA_RD)
            continue;
        cache = ev->v[i].a & ~3u;
        dram = ev->v[i].b & ~7u;
        lenreg = ev->v[i].c;
        length = ((lenreg & 0xfff) + 1 + 7) & ~7u;
        skip = (lenreg >> 20) & 0xfff;
        count = (lenreg >> 12) & 0xff;
        if (((cache & 0xfff) + length) > 0x1000)
            length = 0x1000 - (cache & 0xfff);
        for (row = 0; row <= count; row++) {
            rows_add(r, dram & 0x7ffffc, length);
            dram += length + skip;
            cache += length;
        }
    }
}

static int snapfile_write(const struct armlet_routine *rt, unsigned pc, unsigned exit_pc, const PrspNativeSnap *before, const struct rows *rows,
                          const PrspNativeSnap *after, const struct evbuf *jit_ev, const char *tag)
{
    char path[512];
    FILE *f;
    struct armlet_snapfile_hdr h;
    snprintf(path, sizeof(path), "%s/%s-%s-%" PRIu64 ".snap", G.set.capture_dir, rt->name, tag, G.call_no);
    f = fopen(path, "wb");
    if (!f)
        return -1;
    memset(&h, 0, sizeof(h));
    h.magic = ARMLET_SNAPFILE_MAGIC;
    h.version = 1;
    snprintf(h.routine, sizeof(h.routine), "%s", rt->name);
    h.ucode = rt->ucode;
    h.entry_pc = pc;
    h.exit_pc = exit_pc;
    h.n_rows = rows->count;
    h.n_events = (uint32_t)jit_ev->n;
    fwrite(&h, sizeof(h), 1, f);
    fwrite(before, sizeof(*before), 1, f);
    fwrite(rows->p, 1, rows->n, f);
    fwrite(after, sizeof(*after), 1, f);
    fwrite(jit_ev->v, sizeof(PrspNativeEvent), jit_ev->n, f);
    fclose(f);
    return 0;
}

static void poison(const struct armlet_routine *r)
{
    unsigned i, k;
    for (i = 1; i < 32; i++)
        if (r->dead_sr >> i & 1)
            G.view.sr[i] = 0xdead0000u | i;
    for (i = 0; i < 32; i++)
        if (r->dead_vr >> i & 1)
            for (k = 0; k < 8; k++)
                G.view.vr[i][k] = (uint16_t)(0xbad0 + i);
    if (r->dead_acc)
        memset(G.view.acc, 0xa5, 48);
    if (r->dead_flags)
        for (i = 0; i < 3; i++)
            memset(G.view.flags[i], 0x5a, 32);
    if (r->dead_div) {
        *G.view.div_out = 0x5a5a;
        *G.view.div_in = 0x5a5a;
        *G.view.dp_flag = 1;
    }
}

/* The JIT run of a routine that returned a wrong exit pc may never get there (it runs the ucode on, and a ucode waiting for the CPU spins). Nothing can
 * stop JIT code from outside but a signal, and what the run did to RDRAM and the RDP cannot be undone, so the watchdog reports and ends the process. */
static char l_wd_msg[256];

static void wd_handler(int sig)
{
    (void)sig;
    (void)!write(2, l_wd_msg, strlen(l_wd_msg));
    _exit(3);
}

static void wd_arm(const struct armlet_routine *rt, unsigned pc, int npc)
{
    struct itimerval it;
    struct sigaction sa;
    unsigned ms = G.set.watchdog_ms ? G.set.watchdog_ms : 3000;
    snprintf(l_wd_msg, sizeof(l_wd_msg), "arMLEt: native %s WATCHDOG: the JIT, run from the entry %03x, did not arrive at the exit pc %03x within %u ms; aborting\n", rt->name, pc, npc, ms);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = wd_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, &G.wd_old);
    memset(&it, 0, sizeof(it));
    it.it_value.tv_sec = ms / 1000;
    it.it_value.tv_usec = (ms % 1000) * 1000;
    setitimer(ITIMER_REAL, &it, NULL);
}

static void wd_disarm(void)
{
    struct itimerval it;
    memset(&it, 0, sizeof(it));
    setitimer(ITIMER_REAL, &it, NULL);
    sigaction(SIGALRM, &G.wd_old, NULL);
}

/* Runs the routine natively in a sandbox, then the JIT from the same state, and compares. The JIT's result is the one that stands. */
static int enter_verify(unsigned idx, unsigned pc, unsigned *next_pc, int *mode)
{
    const struct armlet_routine *rt = G.reg[idx];
    struct armlet_native_stats *st = &G.stats[idx];
    struct armlet_env env;
    struct report rp;
    int npc, orc, cmd_end, invalidated;
    uint64_t t0 = 0, t1 = 0;
    int do_capture = 0;

    prsp_native_save(G.s0);
    G.ev_native.n = G.ev_jit.n = 0;
    G.cur_ev = &G.ev_native;
    prsp_native_record(1);
    prsp_native_sandbox(1);
    env_init(&env, rt, pc);
    if (G.set.stats)
        t0 = now_ticks();
    npc = rt->run(&env);
    if (G.set.stats)
        t1 = now_ticks();
    prsp_native_sandbox(0);
    prsp_native_record(0);
    G.cur_ev = NULL;
    st->verified++;
    G.verify_total++;
    st->units += env.units;

    if (G.set.capture_dir && st->verified % G.set.capture_every == 1 && G.captured[idx] < G.set.capture_max)
        do_capture = 1;

    memset(&rp, 0, sizeof(rp));
    if (npc < 0) {
        PrspNativeSnap *sx = G.sn;
        prsp_native_save(sx);
        st->declined++;
        compare_snaps(&rp, sx, G.s0, rt, &G.ev_native, &G.ev_jit, 0, 0);
        if (G.ev_native.n)
            rep(&rp, "  declined after %zu effectful SP register writes\n", G.ev_native.n);
        prsp_native_load(G.s0);
        if (rp.count) {
            st->mismatches++;
            mismatch(rt, pc, -1, &rp, "declined but changed the state");
        }
        return PRSP_NATIVE_DECLINE;
    }
    if (npc & 3 || npc > 0xffc || (unsigned)npc == pc) {
        prsp_native_load(G.s0);
        rep(&rp, "  the routine returned the pc %d\n", npc);
        st->mismatches++;
        mismatch(rt, pc, npc, &rp, "bad exit pc");
        return PRSP_NATIVE_DECLINE;
    }
    st->calls++;
    prsp_native_save(G.sn);
    if (G.set.stats)
        st->ticks += t1 - t0;
    prsp_native_load(G.s0);

    if (do_capture) {
        G.captured[idx]++;
        rows_collect(&G.rows, &G.ev_native);   /* the sandbox put the RDRAM back: this is what the routine read */
    }

    G.cur_ev = &G.ev_jit;
    prsp_native_record(1);
    if (G.set.stats)
        t0 = now_ticks();
    wd_arm(rt, pc, npc);
    orc = prsp_native_oracle(pc, (unsigned)npc);
    wd_disarm();
    if (G.set.stats)
        st->oracle_ticks += now_ticks() - t0;
    prsp_native_record(0);
    G.cur_ev = NULL;
    *next_pc = *G.view.pc;
    *mode = PRSP_NATIVE_MODE_CONTINUE;

    if (orc != 0) {
        rep(&rp, "  the JIT's run from the entry %03x did not arrive at %03x: it ended with mode %d at pc %03x\n", pc, npc, orc, *G.view.pc);
        st->mismatches++;
        mismatch(rt, pc, npc, &rp, "exit");
        *mode = orc > 0 ? orc : PRSP_NATIVE_MODE_CONTINUE;
        return PRSP_NATIVE_DONE;
    }
    prsp_native_save(G.sj);
    cmd_end = events_have_cmd_end(&G.ev_jit) || events_have_cmd_end(&G.ev_native);
    invalidated = 1;    /* the oracle may have run invalidate_code(); the dirty bits are only compared when neither side had IMEM DMA */
    {
        size_t i;
        for (i = 0; i < G.ev_jit.n; i++)
            if (G.ev_jit.v[i].kind == PRSP_EV_DMA_RD && (G.ev_jit.v[i].a & 0x1000))
                break;
        invalidated = i < G.ev_jit.n;
    }
    compare_snaps(&rp, G.sn, G.sj, rt, &G.ev_native, &G.ev_jit, cmd_end, invalidated);
    if (rp.count) {
        st->mismatches++;
        mismatch(rt, pc, npc, &rp, "state");
        if (G.set.capture_dir) {
            if (!do_capture)
                rows_collect(&G.rows, &G.ev_native);   /* RDRAM has moved on by now; good enough for a failure report */
            snapfile_write(rt, pc, (unsigned)npc, G.s0, &G.rows, G.sj, &G.ev_jit, "mismatch");
        }
    } else if (do_capture) {
        snapfile_write(rt, pc, (unsigned)npc, G.s0, &G.rows, G.sj, &G.ev_jit, "ok");
    }
    return PRSP_NATIVE_DONE;
}

static int enter_native(unsigned idx, unsigned pc, unsigned *next_pc, int *mode)
{
    const struct armlet_routine *rt = G.reg[idx];
    struct armlet_native_stats *st = &G.stats[idx];
    struct armlet_env env;
    uint64_t t0 = 0;
    int npc;

    env_init(&env, rt, pc);
    if (G.set.stats)
        t0 = now_ticks();
    npc = rt->run(&env);
    if (G.set.stats)
        st->ticks += now_ticks() - t0;
    if (npc < 0) {
        st->declined++;
        return PRSP_NATIVE_DECLINE;
    }
    st->calls++;
    st->units += env.units;
    if (G.set.poison)
        poison(rt);
    *next_pc = (unsigned)npc & 0xffc;
    *mode = env.mode;
    return PRSP_NATIVE_DONE;
}

int armlet_native_call(unsigned idx, unsigned *next_pc, int *mode)
{
    unsigned pc = G.reg[idx]->entry_pc;
    G.call_no++;
    return G.set.mode == ARMLET_NATIVE_VERIFY ? enter_verify(idx, pc, next_pc, mode) : enter_native(idx, pc, next_pc, mode);
}

static int host_enter(void *user, unsigned cookie, unsigned pc, unsigned *next_pc, int *mode)
{
    (void)user;
    (void)pc;
    return armlet_native_call(cookie, next_pc, mode);
}

/* ---- harness support ---- */
int armlet_native_find(const char *name, uint64_t ucode)
{
    unsigned i;
    registry_build();
    for (i = 0; i < G.n_reg; i++)
        if (!strcmp(G.reg[i]->name, name) && G.reg[i]->ucode == ucode)
            return (int)i;
    return -1;
}

unsigned armlet_native_compare(const PrspNativeSnap *a, const PrspNativeSnap *b, unsigned idx, char *out, size_t outsz)
{
    struct report rp;
    struct evbuf none = { NULL, 0, 0 };
    memset(&rp, 0, sizeof(rp));
    compare_snaps(&rp, a, b, G.reg[idx], &none, &none, 1, 1);
    snprintf(out, outsz, "%s", rp.buf);
    return rp.count;
}

int armlet_native_bench(unsigned idx, const PrspNativeSnap *before, unsigned exit_pc, unsigned iters, double *native_ns, double *jit_ns, double *units)
{
    const struct armlet_routine *rt = G.reg[idx];
    struct armlet_env env;
    uint64_t tn = 0, tj = 0, t0;
    uint64_t un = 0;
    unsigned i;

    for (i = 0; i < iters; i++) {
        prsp_native_load(before);
        env_init(&env, rt, rt->entry_pc);
        t0 = now_ticks();
        if (rt->run(&env) < 0)
            return -1;
        tn += now_ticks() - t0;
        un += env.units;
    }
    for (i = 0; i < iters; i++) {
        prsp_native_load(before);
        t0 = now_ticks();
        if (prsp_native_oracle(rt->entry_pc, exit_pc) != 0)
            return -2;
        tj += now_ticks() - t0;
    }
    *native_ns = ticks_to_ns(tn) / iters;
    *jit_ns = ticks_to_ns(tj) / iters;
    *units = (double)un / iters;
    return 0;
}

/* ---- setup ---- */
void armlet_native_settings_load(struct armlet_native_settings *s, const struct armlet_config *cfg)
{
    const char *e;
    memset(s, 0, sizeof(*s));
    s->mode = cfg->native_mode;
    snprintf(s->routines, sizeof(s->routines), "%s", cfg->native_routines);
    s->abort_on_mismatch = (e = getenv("ARMLET_NATIVE_ABORT")) && *e && *e != '0';
    s->stats = (e = getenv("ARMLET_NATIVE_STATS")) && *e && *e != '0';
    s->poison = (e = getenv("ARMLET_NATIVE_POISON")) && *e && *e != '0';
    s->capture_dir = (e = getenv("ARMLET_NATIVE_CAPTURE")) && *e ? e : NULL;
    s->capture_max = (e = getenv("ARMLET_NATIVE_CAPTURE_MAX")) && *e ? (unsigned)atoi(e) : 32;
    s->capture_every = (e = getenv("ARMLET_NATIVE_CAPTURE_EVERY")) && *e ? (unsigned)atoi(e) : 16;
    if (!s->capture_every)
        s->capture_every = 1;
    s->watchdog_ms = (e = getenv("ARMLET_NATIVE_WATCHDOG_MS")) && *e ? (unsigned)atoi(e) : 0;
}

void armlet_native_start(const struct armlet_native_settings *s)
{
    G.set = *s;
    registry_build();
    if (G.set.mode == ARMLET_NATIVE_OFF || G.started)
        return;
    G.s0 = calloc(3, sizeof(*G.s0));
    G.sn = G.s0 + 1;
    G.sj = G.s0 + 2;
    if (!G.s0)
        abort();
    prsp_native_view(&G.view);
    G.host.lookup = host_lookup;
    G.host.enter = host_enter;
    G.host.event = host_event;
    prsp_native_install(&G.host);
    G.started = 1;
    armlet_log(M64MSG_INFO, "native routines %s (%u known%s%s)", G.set.mode == ARMLET_NATIVE_VERIFY ? "on, verified against the JIT" : "on", G.n_reg,
               G.set.routines[0] ? ", allow-list " : "", G.set.routines);
}

void armlet_native_init(struct armlet_ctx *c)
{
    struct armlet_native_settings s;
    armlet_native_settings_load(&s, &c->cfg);
    armlet_native_start(&s);
}

void armlet_native_task_begin(struct armlet_ctx *c)
{
    const struct armlet_ucode_id *u;
    bool is_new;
    if (!G.started)
        return;
    if (armlet_dmem32(c, ARMLET_TASK_UCODE_BOOT_SIZE) > 0x1000 || armlet_dmem32(c, ARMLET_TASK_TYPE) != 1) {
        if (G.ucode)
            armlet_native_select(0);
        return;
    }
    if (G.set.stats && !(++G.tasks & 127))
        armlet_native_report(0);
    u = armlet_gfx_identify(c, &is_new);
    if (u->id != G.ucode) {
        unsigned n = armlet_native_select(u->id);
        if (n || is_new) {
            unsigned i;
            char names[256] = "";
            for (i = 0; i < G.n_active; i++) {
                size_t l = strlen(names);
                snprintf(names + l, sizeof(names) - l, "%s%s", l ? " " : "", G.reg[G.active[i]]->name);
            }
            armlet_log(M64MSG_INFO, "gfx ucode %016" PRIx64 ": %u native routines selected%s%s", u->id, n, n ? ": " : "", names);
        }
    }
}

void armlet_native_report(int final)
{
    unsigned i;
    if (!G.started || !G.set.stats)
        return;
    for (i = 0; i < G.n_reg; i++) {
        const struct armlet_native_stats *s = &G.stats[i];
        if (!s->calls && !s->declined)
            continue;
        armlet_log(M64MSG_INFO, "native %s: %" PRIu64 " calls, %" PRIu64 " declined, %" PRIu64 " units, native %.1f ns/call %.1f ns/unit%s", G.reg[i]->name, s->calls,
                   s->declined, s->units, s->calls ? ticks_to_ns(s->ticks) / s->calls : 0.0, s->units ? ticks_to_ns(s->ticks) / s->units : 0.0,
                   s->verified ? "" : "");
        if (s->verified)
            armlet_log(M64MSG_INFO, "native %s: verified %" PRIu64 " calls, %" PRIu64 " mismatches, JIT %.1f ns/call", G.reg[i]->name, s->verified, s->mismatches,
                       s->calls ? ticks_to_ns(s->oracle_ticks) / s->calls : 0.0);
    }
    (void)final;
}

void armlet_native_reset(void)
{
    if (!G.started)
        return;
    armlet_native_report(1);
    G.ucode = 0;
    G.n_active = 0;
    prsp_native_set_pcs(NULL, 0, NULL, 0);
}
