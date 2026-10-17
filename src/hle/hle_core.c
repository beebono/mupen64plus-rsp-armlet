/*
 * arMLEt - hle_core.c: the task-level native graphics path (src/hle/hle.h, docs/design.md "Task-level native path")
 *
 * A fresh graphics task of a ucode that has an hle_ucode entry runs here, in a sandbox: the family's front-end works on the real DMEM / IMEM and calls the
 * hle_* functions for everything that touches the world. The sandbox is the fork's (prsp_native_sandbox: SP DMA writes to RDRAM keep their old bytes and the
 * RDP is not run) plus a snapshot of the RSP state (prsp_native_save) taken before. When the front-end declines, or fails to finish, the state is put back
 * and the JIT runs the task. When it completes, the RDRAM is put back and the log of the task's RDRAM writes and RDP lists is replayed in order through the
 * same calls the JIT's run would have made (the coalescing path of rdp_defer.c), so the video plugin and the RDRAM end up exactly as after a JIT run.
 *
 * Verify mode (ARMLET_HLE_VERIFY=1): after the native run the state is put back and the JIT runs the task for real; the two final states are compared
 * (see compare_tasks for exactly what), and the JIT's result stands.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "armlet.h"
#include "gfx_ucode.h"
#include "hle.h"
#include "hle_internal.h"
#include "prsp_bridge.h"
#include "rdp_defer.h"

#ifdef ARMLET_NATIVE
#include "native/native.h"
#include "native/native_internal.h"

#define SP_STATUS_INTR_BREAK 0x40u
#define DPC_STATUS_XBUS 0x1u

#ifdef ARMLET_RDP_DEFER
void prsp_rdp_defer_dma_write(uint32_t dram, uint32_t length, uint32_t skip, unsigned count);
#endif

/* ---- the registry: hle_ucodes section ------------------------------------------------------------------------------------------------------------------------- */
static const struct hle_ucode *const l_sentinel __attribute__((used, section("hle_ucodes"))) = NULL;
extern const struct hle_ucode *const __start_hle_ucodes[];
extern const struct hle_ucode *const __stop_hle_ucodes[];

void armlet_hle_print_masks(FILE *f)
{
    const struct hle_ucode *const *p;
    int first = 1;
    for (p = __start_hle_ucodes; p < __stop_hle_ucodes; p++) {
        unsigned i;
        if (!*p || !(*p)->n_dead)
            continue;
        fprintf(f, "%s%016" PRIx64 ":", first ? "" : ";", (*p)->id);
        for (i = 0; i < (*p)->n_dead; i++)
            fprintf(f, "%s%x-%x", i ? "," : "", (*p)->dead[i].lo, (*p)->dead[i].hi);
        first = 0;
    }
    fprintf(f, "\n");
}

const struct hle_ucode *armlet_hle_next(const struct hle_ucode *prev)
{
    const struct hle_ucode *const *p = __start_hle_ucodes;
    if (prev) {
        while (p < __stop_hle_ucodes && *p != prev)
            p++;
        p++;
    }
    for (; p < __stop_hle_ucodes; p++)
        if (*p)
            return *p;
    return NULL;
}

const struct hle_ucode *armlet_hle_find(uint64_t id)
{
    const struct hle_ucode *const *p;
    for (p = __start_hle_ucodes; p < __stop_hle_ucodes; p++)
        if (*p && (*p)->id == id)
            return *p;
    return NULL;
}

/* two entries with one id would shadow each other (armlet_hle_find takes the first): the ids are the FNV of the whole 4 KiB text, so this is a table error (the same text registered twice) */
static void check_unique_ids(void)
{
    const struct hle_ucode *a = NULL, *b;
    while ((a = armlet_hle_next(a)) != NULL)
        for (b = armlet_hle_next(a); b; b = armlet_hle_next(b))
            if (a->id == b->id)
                armlet_log(M64MSG_ERROR, "hle: two table entries with the ucode id %016" PRIx64 ": \"%s\" and \"%s\"", a->id, a->name, b->name);
}

/* ---- state ------------------------------------------------------------------------------------------------------------------------------------------------------ */
enum { R_DMA_WR = 1, R_MTC0 };
struct rec {
    uint8_t kind, rd;
    uint32_t a, b, c, d;                /* R_DMA_WR: dest (aligned), length (aligned, clamped), skip, rows - 1; R_MTC0: a = value, b = DPC_CURRENT at the time */
    size_t off;                         /* R_DMA_WR: the bytes written, in the arena */
};
struct evbuf {
    PrspNativeEvent *v;
    size_t n, cap;
};
#define MAX_REASONS 48

static struct {
    int init, verify, interp, poison, stats, abort_on_mismatch, quiet, exact;   /* verify: 0 off, 1 the JIT is the oracle, 2 the interpreter */
    PrspNativeView v;
    PrspNativeSnap *s0, *sn, *sj;
    struct rec *recs;
    size_t n_recs, cap_recs;
    uint8_t *arena;
    size_t n_arena, cap_arena;
    const char *why;
    unsigned end_pc;
    int broke;
    int switched;                       /* the last native task took another build over (hle_task.switched) */
    /* verify */
    struct evbuf ev[2];                 /* [0] native, [1] JIT */
    struct hle_stream st[2];
    int cur_ev;
    uint8_t *ram_n;
    /* statistics */
    uint64_t tasks, native, declined, verified, mismatches, ticks;
    struct {
        const char *why;
        uint64_t n;
    } reasons[MAX_REASONS];
} K;

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void *grow(void *p, size_t *cap, size_t need, size_t elem)
{
    if (need <= *cap)
        return p;
    *cap = *cap ? *cap : 1024;
    while (*cap < need)
        *cap *= 2;
    p = realloc(p, *cap * elem);
    if (!p)
        abort();
    return p;
}

static struct rec *rec_new(void)
{
    K.recs = grow(K.recs, &K.cap_recs, K.n_recs + 1, sizeof(*K.recs));
    return &K.recs[K.n_recs++];
}

/* ---- the SP DMA row geometry (as rsp_dma_read / rsp_dma_write in the fork's rsp/cp0.cpp) --------------------------------------------------------------------------- */
int hle_exact(void) { return K.exact; }
/* development (hle_rare.c's HLE_RARE_LOCKSTEP): the commit log as it is, and back to a mark */
void hle_dev_mark(size_t m[2]) { m[0] = K.n_recs; m[1] = K.n_arena; }
void hle_dev_rewind(const size_t m[2]) { K.n_recs = m[0]; K.n_arena = m[1]; }

/* hle_rdram_write: the RDRAM writes of a front-end that does not go through the fork's DMA (its sandbox cannot see them) keep their own undo log */
static struct undo {
    uint32_t addr, n;
    size_t off;
} *g_undo;
static size_t g_n_undo, g_cap_undo, g_n_uarena, g_cap_uarena;
static uint8_t *g_uarena;

static void undo_apply(void)
{
    size_t i;
    for (i = g_n_undo; i-- > 0;)
        memcpy((uint8_t *)K.v.rdram + g_undo[i].addr, g_uarena + g_undo[i].off, g_undo[i].n);
    g_n_undo = g_n_uarena = 0;
}
/* the sandbox off: the fork's undo log, then ours */
static void sandbox_off(void)
{
    prsp_native_sandbox(0);
    undo_apply();
}

void hle_rdram_write(struct hle_task *t, uint32_t dram, const uint32_t *words, uint32_t nbytes)
{
    struct undo *u;
    struct rec *r;
    size_t off;
    dram &= 0x7ffff8;
    g_undo = grow(g_undo, &g_cap_undo, g_n_undo + 1, sizeof(*g_undo));
    g_uarena = grow(g_uarena, &g_cap_uarena, g_n_uarena + nbytes, 1);
    u = &g_undo[g_n_undo++];
    u->addr = dram;
    u->n = nbytes;
    u->off = g_n_uarena;
    memcpy(g_uarena + g_n_uarena, t->rdram + dram, nbytes);
    g_n_uarena += nbytes;
    memcpy(t->rdram + dram, words, nbytes);
    r = rec_new();
    r->kind = R_DMA_WR;
    r->a = dram;
    r->b = nbytes;
    r->c = 0;
    r->d = 0;
    off = K.n_arena;
    K.arena = (uint8_t *)grow(K.arena, &K.cap_arena, off + nbytes, 1);
    r->off = off;
    memcpy(K.arena + off, words, nbytes);
    K.n_arena = off + nbytes;
}

const char *armlet_hle_last_why(void) { return K.why; }

int hle_decline(struct hle_task *t, const char *why)
{
    (void)t;
    if (!K.why)
        K.why = why;
    return HLE_DECLINE;
}

/* An SP DMA of one row to or from DMEM without the fork's register-write chain when nothing records events (verify, capture): the same bytes, the same sandbox and log, the same register values
 * afterwards (rsp_dma_read / rsp_dma_write in the fork's rsp/cp0.cpp: DMA_CACHE = mem + length, DMA_DRAM = dram + length + skip, the length register 0xff8). The write is the expensive one: the
 * fork's sandbox allocates an undo record per row; hle_rdram_write keeps one arena. Rows that wrap, straddle or leave RDRAM take the fork's path. */
void hle_dma_read(struct hle_task *t, unsigned mem_addr, unsigned dram_addr, uint32_t len)
{
    if (!armlet_event_listener && !(len >> 12) && !(mem_addr & 0x1000)) {
        uint32_t length = ((len & 0xfff) + 1 + 7) & ~7u, skip = (len >> 20) & 0xfff;
        uint32_t cache = (mem_addr & 0x1fff) & ~3u, dram = (dram_addr & 0xffffff) & ~7u;
        if ((cache & 0xfff) + length > 0x1000)
            length = 0x1000 - (cache & 0xfff);
        if (length && (dram & 0x7ffffc) + length <= 0x800000) {
            memcpy(t->dmem + (cache >> 2), t->rdram + (dram & 0x7ffffc), length);
            *t->cr[0] = cache + length;
            *t->cr[1] = dram + length + skip;
            *t->cr[2] = 0xff8;
            return;
        }
    }
    hle_mtc0(t, 0, mem_addr);
    hle_mtc0(t, 1, dram_addr);
    hle_mtc0(t, 2, len);
}

void hle_dma_write(struct hle_task *t, unsigned mem_addr, unsigned dram_addr, uint32_t len)
{
    if (!armlet_event_listener && t->log && !(len >> 12) && !(mem_addr & 0x1000)) {
        uint32_t length = ((len & 0xfff) + 1 + 7) & ~7u, skip = (len >> 20) & 0xfff;
        uint32_t cache = (mem_addr & 0x1fff) & ~3u, dram = (dram_addr & 0xffffff) & ~7u;
        if ((cache & 0xfff) + length > 0x1000)
            length = 0x1000 - (cache & 0xfff);
        if (length && (dram & 0x7ffffc) + length <= 0x800000) {
            hle_rdram_write(t, dram & 0x7ffffc, &t->dmem[cache >> 2], length);
            *t->cr[0] = cache + length;
            *t->cr[1] = dram + length + skip;
            *t->cr[3] = 0xff8;
            return;
        }
    }
    hle_mtc0(t, 0, mem_addr);
    hle_mtc0(t, 1, dram_addr);
    hle_mtc0(t, 3, len);
}

/* the rows a DMA write put into RDRAM go to the log, with what the defer hook needs to know */
static void log_dma_write(struct hle_task *t, uint32_t len, uint32_t cache, uint32_t dram)
{
    uint32_t length = ((len & 0xfff) + 1 + 7) & ~7u, skip = (len >> 20) & 0xfff, d;
    unsigned count = (len >> 12) & 0xff, i;
    struct rec *r;
    size_t off;

    if (((cache & 0xfff) + length) > 0x1000)
        length = 0x1000 - (cache & 0xfff);
    r = rec_new();
    r->kind = R_DMA_WR;
    r->a = dram;
    r->b = length;
    r->c = skip;
    r->d = count;
    off = K.n_arena;
    K.arena = (uint8_t *)grow(K.arena, &K.cap_arena, off + (size_t)length * (count + 1), 1);
    r->off = off;
    for (i = 0, d = dram; i <= count; i++, d += length + skip) {
        uint32_t a = d & 0x7ffffc;
        if (a + length <= 0x800000) {
            memcpy(K.arena + off, t->rdram + a, length);
            off += length;
        } else {
            uint32_t j;
            for (j = 0; j < length; j += 4, off += 4)
                memcpy(K.arena + off, t->rdram + ((d + j) & 0x7ffffc), 4);
        }
    }
    K.n_arena = off;
}

int hle_mtc0(struct hle_task *t, unsigned rd, uint32_t value)
{
    int mode;
    uint32_t cur = *t->cr[10], cache = *t->cr[0] & 0x1ffc, dram = *t->cr[1] & 0x7ffff8;
    rd &= 15;
    mode = prsp_native_mtc0(rd, value);
    if (rd == 3 && t->log) {
        log_dma_write(t, value, cache, dram);
    } else if ((rd == 8 || rd == 9 || rd == 11) && t->log) {
        struct rec *r = rec_new();
        r->kind = R_MTC0;
        r->rd = (uint8_t)rd;
        r->a = value;
        r->b = cur;
        r->c = 0;
        if (rd == 9 && (*t->cr[11] & DPC_STATUS_XBUS) && (value & ~7u) > (cur & ~7u) && (value & ~7u) - (cur & ~7u) <= 0x1000) {
            /* an XBUS list lives in DMEM, which the microcode goes on to overwrite: keep its bytes for the replay */
            uint32_t n = (value & ~7u) - (cur & ~7u), i;
            size_t off = K.n_arena;
            K.arena = (uint8_t *)grow(K.arena, &K.cap_arena, off + n, 1);
            for (i = 0; i < n; i++)
                K.arena[off + i] = ((const uint8_t *)t->dmem)[(((cur & ~7u) + i) & 0xfff) ^ 3];
            r->c = n;
            r->off = off;
            K.n_arena = off + n;
        }
        if (rd == 9) {
            /* what the video plugin leaves behind after it took a list (rdp_defer.c: START = CURRENT = END = the new end); a list with end <= current is ignored */
            uint32_t c8 = cur & ~7u, e8 = value & ~7u;
            if (e8 > c8)
                *t->cr[8] = *t->cr[10] = *t->cr[9] = e8;
        }
    }
    return rd == 4 && mode != PRSP_NATIVE_MODE_CONTINUE;      /* a DMA into IMEM answers CHECK_FLAGS (the JIT drops code): not an end */
}

int hle_poll_status(struct hle_task *t, unsigned rt, uint32_t *value)
{
    int mode = PRSP_NATIVE_MODE_CONTINUE;
    (void)t;
    *value = prsp_native_mfc0(4, rt, &mode);
    return mode != PRSP_NATIVE_MODE_CONTINUE;
}

void hle_cpu_init(struct hle_task *t, struct hle_cpu *c, unsigned pc, unsigned max_steps)
{
    (void)t;
    c->r = K.v.sr;
    c->pc = pc & 0xffc;
    c->steps = 0;
    c->max_steps = max_steps;
    c->stop_pc = 0x1000;
}

void hle_break(struct hle_task *t, unsigned end_pc)
{
    *t->cr[4] |= SP_STATUS_BROKE | SP_STATUS_HALT;
    if (*t->cr[4] & SP_STATUS_INTR_BREAK)
        *t->irq |= 1;
    K.end_pc = end_pc & 0xffc;
    K.broke = 1;
}

/* a whole task on the interpreter, from the task's start pc */
int hle_interp_task(struct hle_task *t)
{
    struct hle_cpu c;
    hle_cpu_init(t, &c, t->start_pc, 1u << 24);
    return hle_rsp_run(t, &c);
}

/* ---- events (verify) ------------------------------------------------------------------------------------------------------------------------------------------------ */
static void ev_cb(const PrspNativeEvent *e)
{
    struct evbuf *b = &K.ev[K.cur_ev];
    b->v = grow(b->v, &b->cap, b->n + 1, sizeof(*b->v));
    b->v[b->n++] = *e;
    hle_stream_add(&K.st[K.cur_ev], e, K.v.dmem, K.v.rdram);
}


static void side_reset(int i, const struct hle_ucode *uc)
{
    K.ev[i].n = 0;
    hle_stream_reset(&K.st[i], uc);
}

/* ---- the comparison (verify) ---------------------------------------------------------------------------------------------------------------------------------------- */
struct report {
    char buf[8192];
    size_t n;
    unsigned count;
};

static void rep(struct report *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void rep(struct report *r, const char *fmt, ...)
{
    va_list ap;
    int k;
    r->count++;
    if (r->n + 240 > sizeof(r->buf))
        return;
    va_start(ap, fmt);
    k = vsnprintf(r->buf + r->n, sizeof(r->buf) - r->n, fmt, ap);
    va_end(ap);
    if (k > 0)
        r->n += (size_t)k;
}

static int dead_dmem(const struct hle_ucode *u, unsigned byte)
{
    unsigned i;
    for (i = 0; i < u->n_dead; i++)
        if (byte >= u->dead[i].lo && byte < u->dead[i].hi)
            return 1;
    return 0;
}

static uint32_t l_ring_lo, l_ring_hi;          /* the output ring of the task being verified */
static int in_ring(const struct hle_ucode *u, uint32_t dram, uint32_t span, unsigned count)
{
    uint32_t end = dram + span * (count + 1);
    return u->ring_off && l_ring_hi > l_ring_lo && dram >= l_ring_lo && end <= l_ring_hi;
}

/* The final state of the native run (a) against the JIT's (b) (docs/design.md, "What verify compares"). The standard mode compares the EXTERNAL EFFECTS: DMEM (except the ucode's dead
 * ranges, hle_dead_range), IMEM, the SP and DPC registers, MI_INTR, the pc, SP_STATUS_TIMEOUT, the RDRAM the task wrote by DMA (the bytes of every range either side wrote, except the RDP
 * output ring, hle_ucode.ring_off), and the concatenated bytes of the RDP lists (not their boundaries). The order and number of the intermediate SP register writes, the DMA chunking, the
 * poll counts and the IMEM dirty bits are NOT compared. The exact-events mode (ARMLET_HLE_EXACT=1) adds the recorded SP register writes in order and the dirty bits, for a front-end that
 * claims to do what the microcode does, DMA for DMA. NOT compared in either: the scalar and vector registers, the accumulator, the flags and the divide unit (the next task loads them
 * before use; ARMLET_HLE_POISON=1 fills them after a native task to prove it), the status-poll counters (the JIT zeroes them at the start of every task). */
static void compare_tasks(struct report *r, const struct hle_ucode *u, const PrspNativeSnap *a, const PrspNativeSnap *b, int with_ram)
{
    unsigned i, nd = 0;
    const uint8_t *da = (const uint8_t *)a->dmem, *db = (const uint8_t *)b->dmem;
    const struct evbuf *ea = &K.ev[0], *eb = &K.ev[1];

    l_ring_lo = l_ring_hi = 0;
    if (u->ring_off) {
        l_ring_lo = b->dmem[u->ring_off >> 2] & 0x7ffff8;
        l_ring_hi = b->dmem[(u->ring_off >> 2) + 1] & 0x7ffff8;
    }
    for (i = 0; i < 4096; i++) {
        unsigned addr = i ^ 3;
        if (da[i] != db[i] && !dead_dmem(u, addr)) {
            if (nd++ < (getenv("HLE_DMEM_ALL") ? 100000u : 24u))
                rep(r, "  DMEM[%03x]: native %02x, JIT %02x\n", addr, da[i], db[i]);
        }
    }
    if (nd > 24)
        rep(r, "  ... %u DMEM bytes differ in all\n", nd);
    for (i = 0; i < 1024; i++)
        if (a->imem[i] != b->imem[i])
            rep(r, "  IMEM[%03x]: native %08x, JIT %08x\n", i * 4, a->imem[i], b->imem[i]);
    for (i = 0; i < 16; i++)
        if (a->cr[i] != b->cr[i])
            rep(r, "  SP/DPC register %u: native %08x, JIT %08x\n", i, a->cr[i], b->cr[i]);
    if ((a->irq ^ b->irq) & ~0x20u)             /* the DP interrupt is the RDP's: raised when it processes a full sync */
        rep(r, "  MI_INTR: native %08x, JIT %08x\n", a->irq, b->irq);
    if (a->pc != b->pc)
        rep(r, "  pc: native %03x, JIT %03x\n", a->pc, b->pc);
    if (a->sp_status_timeout != b->sp_status_timeout)
        rep(r, "  SP_STATUS_TIMEOUT: native %d, JIT %d\n", a->sp_status_timeout, b->sp_status_timeout);
    if (u->flags & HLE_F_REGS_LIVE) {           /* the scalar registers a later task reads are part of the result */
        unsigned k;
        for (k = 1; k < 32; k++)
            if ((u->live_sregs >> k & 1) && a->sr[k] != b->sr[k])
                rep(r, "  scalar register %u: native %08x, JIT %08x\n", k, a->sr[k], b->sr[k]);
    }
    if (K.exact && (b->dirty_blocks & ~a->dirty_blocks))
        rep(r, "  IMEM dirty blocks: native %08x, JIT %08x (the JIT's must be a subset)\n", a->dirty_blocks, b->dirty_blocks);

    if (getenv("HLE_DUMP_STREAM")) {         /* development: the native side's stream, to compare with the interpreter's (ARMLET_HLE_INTERP=1) run */
        FILE *fp = fopen(getenv("HLE_DUMP_STREAM"), "wb");
        if (fp) {
            fwrite(K.st[0].bytes, 1, K.st[0].len < K.st[0].cap ? K.st[0].len : K.st[0].cap, fp);
            fclose(fp);
        }
    }
    if (getenv("HLE_DUMP_STREAM_JIT")) {     /* development: the JIT side's stream */
        FILE *fp = fopen(getenv("HLE_DUMP_STREAM_JIT"), "wb");
        if (fp) {
            fwrite(K.st[1].bytes, 1, K.st[1].len < K.st[1].cap ? K.st[1].len : K.st[1].cap, fp);
            fclose(fp);
        }
    }
    /* the RDP lists, as one byte stream */
    if (K.st[0].len != K.st[1].len || (K.st[0].hash != K.st[1].hash && !(K.switched && K.st[0].hash_r == K.st[1].hash_r))) {
        uint64_t k, n = K.st[0].len < K.st[1].len ? K.st[0].len : K.st[1].len;
        rep(r, "  RDP command stream: native %" PRIu64 " bytes %016" PRIx64 ", JIT %" PRIu64 " bytes %016" PRIx64 "\n", K.st[0].len, K.st[0].hash, K.st[1].len, K.st[1].hash);
        if (n > HLE_STREAM_KEEP)
            n = HLE_STREAM_KEEP;
        for (k = 0; K.st[1].bytes && k < n; k++)
            if (K.st[0].bytes[k] != K.st[1].bytes[k]) {
                rep(r, "    first difference at byte %" PRIu64 " (command %" PRIu64 ", +%u): native %02x, JIT %02x\n", k, k / 8, (unsigned)(k & 7), K.st[0].bytes[k],
                    K.st[1].bytes[k]);
                break;
            }
    }

    /* the recorded SP register writes (DMA reads and writes, status, DPC): the DMA writes carry the hash of the bytes written (exact-events mode only) */
    if (K.exact) {
        size_t ia = 0, ib = 0, k = 0;
        PrspNativeEvent ea_cmp, eb_cmp;
        for (;;) {
            if (ia >= ea->n || ib >= eb->n)
                break;
            {   /* a DMA write from a dead range (the RDP buffers): the bytes it carries are compared through the RDP stream, not by this hash */
                PrspNativeEvent x = ea->v[ia], y = eb->v[ib];
                if (x.kind == PRSP_EV_DMA_WR && dead_dmem(u, x.a & 0xfff))
                    x.hash = 0;
                if (y.kind == PRSP_EV_DMA_WR && dead_dmem(u, y.a & 0xfff))
                    y.hash = 0;
                if (x.kind == PRSP_EV_CMD_END)              /* the list bytes: through the stream (second words of syncs are not compared) */
                    x.hash = 0;
                if (y.kind == PRSP_EV_CMD_END)
                    y.hash = 0;
                ea_cmp = x;
                eb_cmp = y;
            }
            if (memcmp(&ea_cmp, &eb_cmp, sizeof(ea_cmp))) {
                rep(r, "  SP register write #%zu: native kind %u %08x %08x %08x %016" PRIx64 "\n                       JIT    kind %u %08x %08x %08x %016" PRIx64 "\n", k,
                    ea->v[ia].kind, ea->v[ia].a, ea->v[ia].b, ea->v[ia].c, ea->v[ia].hash, eb->v[ib].kind, eb->v[ib].a, eb->v[ib].b, eb->v[ib].c, eb->v[ib].hash);
                if (getenv("HLE_EV_DUMP")) {        /* development: the events around the first difference on both sides */
                    size_t q;
                    for (q = ia > 8 ? ia - 8 : 0; q < ia + 4 && q < ea->n; q++)
                        fprintf(stderr, "    native[%zu] kind %u %08x %08x %08x\n", q, ea->v[q].kind, ea->v[q].a, ea->v[q].b, ea->v[q].c);
                    for (q = ib > 8 ? ib - 8 : 0; q < ib + 4 && q < eb->n; q++)
                        fprintf(stderr, "    JIT   [%zu] kind %u %08x %08x %08x\n", q, eb->v[q].kind, eb->v[q].a, eb->v[q].b, eb->v[q].c);
                }
                break;
            }
            ia++;
            ib++;
            k++;
        }
        if ((ia < ea->n) != (ib < eb->n))
            rep(r, "  SP register writes: one side has more (native %zu, JIT %zu in all)\n", ea->n, eb->n);
    }

    /* the RDRAM written by DMA: every range either side wrote, native copy against the live RDRAM after the JIT run */
    if (with_ram) {
        int side;
        for (side = 0; side < 2; side++) {
            const struct evbuf *e = &K.ev[side];
            size_t k;
            for (k = 0; k < e->n; k++) {
                uint32_t lenreg, length, skip, dram, d;
                unsigned count, row;
                if (e->v[k].kind != PRSP_EV_DMA_WR)
                    continue;
                lenreg = e->v[k].c;
                length = ((lenreg & 0xfff) + 1 + 7) & ~7u;
                skip = (lenreg >> 20) & 0xfff;
                count = (lenreg >> 12) & 0xff;
                if (((e->v[k].a & 0x1ffc & 0xfff) + length) > 0x1000)
                    length = 0x1000 - (e->v[k].a & 0x1ffc & 0xfff);
                dram = e->v[k].b & 0x7ffff8;
                /* A chunk of the RDP output buffer (a DMA write from dead DMEM) is not compared byte for byte: the second words of NO_OP and the sync commands in it are stale scratch that the RDP ignores,
                 * a chunk can start in the middle of a command, and the ring is overwritten by later chunks. The list the RDP takes from the chunks (read from RDRAM when its end is announced) is compared
                 * as a stream, with those words zeroed (hle_stream_add); the events compare where and how much was written. */
                if (in_ring(u, dram, length + skip, count) || (!u->ring_off && dead_dmem(u, e->v[k].a & 0xfff)))
                    continue;
                for (row = 0, d = dram; row <= count; row++, d += length + skip) {
                    uint32_t j;
                    for (j = 0; j < length; j += 4) {
                        uint32_t addr = (d + j) & 0x7ffffc;
                        if (memcmp(K.ram_n + addr, (const uint8_t *)K.v.rdram + addr, 4)) {
                            rep(r, "  RDRAM[%06x] written by DMA %s (event %zu: dmem %03x dram %06x len %08x): native %08x, JIT %08x\n", addr, side ? "(JIT)" : "(native)", k, e->v[k].a, e->v[k].b, e->v[k].c, *(uint32_t *)(K.ram_n + addr),
                                *(const uint32_t *)((const uint8_t *)K.v.rdram + addr));
                            goto next_range;
                        }
                    }
                }
            next_range:;
            }
        }
    }
}

/* ---- running a task --------------------------------------------------------------------------------------------------------------------------------------------------- */
static void poison_regs(uint32_t live)
{
    unsigned i, k;
    /* development: HLE_POISON_SEL="s=<hex mask of scalar registers>,v=<hex mask of vector registers>,x=<1 accumulator | 2 flags | 4 divide unit>" poisons only these (default: all) */
    uint32_t sm = ~live, vm = ~0u, xm = 7;
    const char *e = getenv("HLE_POISON_SEL");
    if (e) {
        sm = vm = xm = 0;
        for (; *e; e++) {
            if (e[1] == '=') {
                unsigned long v = strtoul(e + 2, NULL, 16);
                if (*e == 's') sm = (uint32_t)v;
                else if (*e == 'v') vm = (uint32_t)v;
                else if (*e == 'x') xm = (uint32_t)v;
            }
        }
    }
    for (i = 1; i < 32; i++)
        if (sm >> i & 1)
            K.v.sr[i] = 0xdead0000u | i;
    for (i = 0; i < 32; i++)
        if (vm >> i & 1)
            for (k = 0; k < 8; k++)
                K.v.vr[i][k] = (uint16_t)(0xdead ^ (i << 8) ^ k);
    if (xm & 1)
        for (i = 0; i < 24; i++)
            K.v.acc[i] = (uint16_t)(0xbeef ^ i);
    if (xm & 2)
        for (i = 0; i < 3; i++)
            for (k = 0; k < 16; k++)
                K.v.flags[i][k] = (k ^ i) & 1 ? 0xffff : 0;
    if (xm & 4) {
        *K.v.div_out = (int16_t)0xdead;
        *K.v.div_in = (int16_t)0xbeef;
        *K.v.dp_flag = 1;
    }
}

static void replay(struct armlet_ctx *c)
{
    size_t i;
    (void)c;
    for (i = 0; i < K.n_recs; i++) {
        const struct rec *r = &K.recs[i];
        if (r->kind == R_DMA_WR) {
            uint32_t d = r->a, j;
            unsigned row;
            size_t off = r->off;
#ifdef ARMLET_RDP_DEFER
            prsp_rdp_defer_dma_write(r->a, r->b, r->c, r->d);
#endif
            for (row = 0; row <= r->d; row++, d += r->b + r->c) {
                if ((d & 0x7ffffc) + r->b <= 0x800000) {
                    memcpy((uint8_t *)K.v.rdram + (d & 0x7ffffc), K.arena + off, r->b);
                    off += r->b;
                } else
                    for (j = 0; j < r->b; j += 4, off += 4)
                        memcpy((uint8_t *)K.v.rdram + ((d + j) & 0x7ffffc), K.arena + off, 4);
            }
        } else {
            uint8_t keep[0x1000];
            uint32_t i2, base = r->b & ~7u, endv = r->a, total;
            size_t j = i;
            if (r->rd == 9)
                *K.v.cr[10] = r->b;     /* DPC_CURRENT as it was when the microcode wrote DPC_END */
            if (r->c) {
                /* An XBUS list: its bytes go back into DMEM for the video plugin, then DMEM as it is. Lists that follow each other in the ring (the microcode hands over every
                 * command on its own) are taken as one: the video plugin sees the same bytes in the same order, and a command cut by the end of a list carries over anyway. */
                total = r->c;
                for (i2 = 0; i2 < r->c; i2++) {
                    uint8_t *p = (uint8_t *)K.v.dmem + (((base + i2) & 0xfff) ^ 3);
                    keep[i2] = *p;
                    *p = K.arena[r->off + i2];
                }
                while (j + 1 < K.n_recs) {
                    const struct rec *q = &K.recs[j + 1];
                    if (q->kind != R_MTC0 || q->rd != 9 || !q->c || (q->b & ~7u) != (endv & ~7u) || ((q->b & ~7u) < base) || (q->b & ~7u) + q->c - base > sizeof(keep) || (q->b & ~7u) + q->c > 0x1000)
                        break;
                    for (i2 = 0; i2 < q->c; i2++) {
                        unsigned o = (q->b & ~7u) - base + i2;
                        uint8_t *p = (uint8_t *)K.v.dmem + ((((q->b & ~7u) + i2) & 0xfff) ^ 3);
                        keep[o] = *p;
                        *p = K.arena[q->off + i2];
                    }
                    total = (q->b & ~7u) + q->c - base;
                    endv = q->a;
                    j++;
                }
                prsp_native_mtc0(r->rd, endv);
                for (i2 = total; i2-- > 0;)
                    ((uint8_t *)K.v.dmem)[((base + i2) & 0xfff) ^ 3] = keep[i2];
                i = j;
                continue;
            }
            prsp_native_mtc0(r->rd, r->a);
        }
    }
}

static void count_reason(const char *why)
{
    int i;
    for (i = 0; i < MAX_REASONS; i++) {
        if (K.reasons[i].why == why || (K.reasons[i].why && !strcmp(K.reasons[i].why, why))) {
            K.reasons[i].n++;
            return;
        }
        if (!K.reasons[i].why) {
            K.reasons[i].why = why;
            K.reasons[i].n = 1;
            return;
        }
    }
}

static void report(int final)
{
    int i;
    if (!K.stats || !K.tasks)
        return;
    armlet_log(M64MSG_INFO, "hle: %" PRIu64 " tasks, %" PRIu64 " native (%.1f us each), %" PRIu64 " declined%s", K.tasks, K.native,
               K.native ? (double)K.ticks / 1000.0 / (double)K.native : 0.0, K.declined, final ? " (final)" : "");
    if (K.verify)
        armlet_log(M64MSG_INFO, "hle: verify: %" PRIu64 " tasks compared, %" PRIu64 " with differences", K.verified, K.mismatches);
    for (i = 0; i < MAX_REASONS && K.reasons[i].why; i++)
        armlet_log(M64MSG_INFO, "hle:   declined %" PRIu64 "x: %s", K.reasons[i].n, K.reasons[i].why);
}

static void init_once(void)
{
    const char *e;
    if (K.init)
        return;
    K.init = 1;
    check_unique_ids();
    armlet_native_ensure_host();
    prsp_native_view(&K.v);
    K.s0 = calloc(3, sizeof(*K.s0));
    if (!K.s0)
        abort();
    K.sn = K.s0 + 1;
    K.sj = K.s0 + 2;
    e = getenv("ARMLET_HLE_VERIFY");
    K.verify = !e || !*e || *e == '0' ? 0 : !strcmp(e, "interp") ? 2 : 1;
    K.interp = (e = getenv("ARMLET_HLE_INTERP")) && *e && *e != '0';
    K.poison = (e = getenv("ARMLET_HLE_POISON")) && *e && *e != '0';
    K.stats = (e = getenv("ARMLET_HLE_STATS")) && *e && *e != '0';
    K.abort_on_mismatch = (e = getenv("ARMLET_HLE_ABORT")) && *e && *e != '0';
    K.quiet = (e = getenv("ARMLET_HLE_QUIET")) && *e && *e != '0';
    K.exact = (e = getenv("ARMLET_HLE_EXACT")) && *e && *e != '0';
    armlet_log(M64MSG_INFO, "hle: task-level native graphics path%s%s%s", K.verify == 1 ? ", verified against the JIT" : K.verify == 2 ? ", verified against the interpreter" : "",
               K.interp ? ", on the interpreter" : "", K.poison ? ", poisoning the vector state" : "");
}

static void jit_run(struct armlet_ctx *c)
{
    ARMLET_NATIVE_TASK_BEGIN(c);
    armlet_rdp_defer_task_begin(c);
    parallelRSPDoRspCycles(~0u);
    armlet_rdp_defer_task_end();
}

/* test builds only (scripts/build.sh native --hmut=N): a deliberate error N, to show that the tests catch it (docs/testing.md) */
#ifdef ARMLET_HLE_MUTATE
#define MUT(n, ok, bad) (ARMLET_HLE_MUTATE == (n) ? (bad) : (ok))
#else
#define MUT(n, ok, bad) (ok)
#endif

/* parallel-rsp starts with SP_STATUS_TIMEOUT 0x7fff: a task that polls SP_STATUS that often is halted (it waits for the CPU). After the first such halt it is 16 for good ("from now on, wait 16 times"),
 * and the JIT then halts every task at its 16th poll of a register (one per command), resuming it in the next call: the end state of the first call is a part of the task, which no front-end that runs
 * the whole task models (Gauntlet Legends: a display list that spins on itself until the CPU patches it, 207 of its 209 other tasks differed). The poll limits of the front-ends are for 0x7fff. */
enum { PRSP_TIMEOUT_INIT = 0x7fff };

/* The native run in the sandbox, from the state as it is. HLE_OK: the task is complete, the sandbox is still on (the caller commits or compares, then sandbox(0));
 * else the state is restored and the sandbox is off. */
static int native_run(struct armlet_ctx *c, const struct hle_ucode *uc, int record, int interp)
{
    struct hle_task t;
    int rc;

    memset(&t, 0, sizeof(t));
    t.dmem = K.v.dmem;
    t.imem = K.v.imem;
    t.rdram = (uint8_t *)K.v.rdram;
    t.cr = K.v.cr;
    t.irq = K.v.irq;
    t.uc = uc;
    t.ctx = c;
    t.start_pc = c ? (*c->rsp.SP_PC_REG & 0xfff) : *K.v.pc;
    t.log = 1;
    K.n_recs = K.n_arena = 0;
    g_n_undo = g_n_uarena = 0;
    K.why = NULL;
    K.broke = 0;
    prsp_native_save(K.s0);
    if (MUT(316, K.s0->sp_status_timeout != PRSP_TIMEOUT_INIT, 0)) {
        K.why = "the JIT's status poll timeout has dropped (an earlier task waited for the CPU)";
        return HLE_DECLINE;
    }
    memset(K.v.mfc0_count, 0, 32 * sizeof(short));        /* the JIT does this at the start of every task */
    if (record) {
        side_reset(0, uc);
        K.cur_ev = 0;
        armlet_event_listener = ev_cb;
        prsp_native_record(1);
    }
    prsp_native_sandbox(1);
    rc = interp || K.interp ? hle_interp_task(&t) : uc->fam->run(&t, uc);
    K.switched = t.switched;
    if (rc == HLE_OK && !K.broke) {
        K.why = "the family returned without ending the task";
        rc = HLE_DECLINE;
    }
    if (rc != HLE_OK) {
        sandbox_off();
        if (record) {
            prsp_native_record(0);
            armlet_event_listener = NULL;
        }
        prsp_native_load(K.s0);
        prsp_native_resync();                       /* (the JIT may have compiled code for IMEM contents the task went through) */
        return rc;
    }
    if (K.poison)
        poison_regs(uc->flags & HLE_F_REGS_LIVE ? uc->live_sregs : 0);
    *K.v.pc = K.end_pc;
    return HLE_OK;
}

int armlet_hle_run(struct armlet_ctx *c, const struct armlet_ucode_id *u)
{
    const struct hle_ucode *uc = armlet_hle_find(u->id);
    uint64_t t0;

    if (!uc || !uc->fam || !uc->fam->run)
        return 0;
    init_once();
    K.tasks++;
    if (!(K.tasks & 63))
        report(0);
    /* the JIT's own preconditions (run_rsp_task), and the one thing the replay relies on: the video plugin is foir, behind the coalescing path */
    if ((*c->rsp.SP_STATUS_REG & (SP_STATUS_HALT | SP_STATUS_BROKE)) || !c->cfg.rdp_coalesce || ((*c->rsp.DPC_STATUS_REG & DPC_STATUS_XBUS) && !(uc->flags & HLE_F_XBUS_OK))) {
        K.declined++;
        K.why = "not eligible (SP halted, RDP coalescing off or XBUS)";
        count_reason(K.why);
        return 0;
    }

    t0 = now_ns();
    if (native_run(c, uc, K.verify != 0, 0) != HLE_OK) {
        K.declined++;
        count_reason(K.why ? K.why : "(no reason given)");
        return 0;
    }

    if (K.verify) {
        struct report r;
        PrspNativeSnap *sn = K.sn, *sj = K.sj;
        int irc = HLE_OK;
        prsp_native_save(sn);
        if (!K.ram_n)
            K.ram_n = (uint8_t *)malloc(0x800000);
        if (!K.ram_n)
            abort();
        memcpy(K.ram_n, K.v.rdram, 0x800000);
        sandbox_off();
        prsp_native_load(K.s0);
        prsp_native_resync();
        side_reset(1, uc);
        K.cur_ev = 1;
        if (K.verify == 2) {
            /* the oracle is the interpreter: it runs the task in the sandbox (the RDRAM it leaves is compared), then the state goes back and the JIT runs the task for real, unrecorded */
            struct hle_task t2;
            memset(&t2, 0, sizeof(t2));
            t2.dmem = K.v.dmem;
            t2.imem = K.v.imem;
            t2.rdram = (uint8_t *)K.v.rdram;
            t2.cr = K.v.cr;
            t2.irq = K.v.irq;
            t2.uc = uc;
            t2.ctx = c;
            t2.start_pc = *c->rsp.SP_PC_REG & 0xfff;
            t2.log = 1;
            K.n_recs = K.n_arena = 0;
            K.broke = 0;
            memset(K.v.mfc0_count, 0, 32 * sizeof(short));
            prsp_native_sandbox(1);
            irc = hle_interp_task(&t2);
            if (irc == HLE_OK && !K.broke)
                irc = HLE_DECLINE;
            *K.v.pc = K.end_pc;
        } else {
            jit_run(c);
        }
        prsp_native_record(0);
        armlet_event_listener = NULL;
        prsp_native_save(sj);
        r.n = 0;
        r.count = 0;
        if (irc == HLE_OK)
            compare_tasks(&r, uc, sn, sj, 1);
        if (K.verify == 2) {
            sandbox_off();
            prsp_native_load(K.s0);
            prsp_native_resync();
            jit_run(c);
        }
        if (irc != HLE_OK) {
            count_reason("the interpreter oracle declined");
        } else {
            K.verified++;
            if (r.count) {
                K.mismatches++;
                if (!K.quiet)
                    armlet_log(M64MSG_ERROR, "hle: task %" PRIu64 " (%s): native and %s differ (%u):\n%.*s", K.tasks, uc->name, K.verify == 2 ? "interpreter" : "JIT", r.count, (int)r.n, r.buf);
                if (K.abort_on_mismatch)
                    abort();
            }
        }
        K.native++;
        K.ticks += now_ns() - t0;
        armlet_hle_poison(uc);
        return 1;
    }

    /* commit: the RDRAM goes back to the start of the task and the log is replayed in order through the calls the JIT's run makes */
    sandbox_off();
    armlet_rdp_defer_task_begin(c);
    replay(c);
    armlet_rdp_defer_task_end();
    *c->rsp.SP_PC_REG = 0x04001000 | (K.end_pc & 0xffc);
    K.native++;
    K.ticks += now_ns() - t0;
    armlet_hle_poison(uc);
    return 1;
}

/* ns per `sync` (a DPC_END write and a DMA write of `bytes` from DMEM to the ring) as a front-end issues it, in the sandbox, with the events recorded or not (--dmabench of the offline harness) */
double armlet_hle_dmabench(const struct hle_ucode *uc, const PrspNativeSnap *before, unsigned iters, unsigned bytes, int record)
{
    struct hle_task t;
    unsigned i;
    uint64_t t0;
    init_once();
    K.verify = 0;
    prsp_native_load(before);
    memset(&t, 0, sizeof(t));
    t.dmem = K.v.dmem;
    t.imem = K.v.imem;
    t.rdram = (uint8_t *)K.v.rdram;
    t.cr = K.v.cr;
    t.irq = K.v.irq;
    t.uc = uc;
    t.log = 1;
    K.n_recs = K.n_arena = 0;
    g_n_undo = g_n_uarena = 0;
    if (record) {
        side_reset(0, uc);
        K.cur_ev = 0;
        armlet_event_listener = ev_cb;
        prsp_native_record(1);
    }
    prsp_native_sandbox(1);
    t0 = now_ns();
    for (i = 0; i < iters; i++) {
        uint32_t cur = 0x100000 + (i & 63) * 0x400;
        if (!(i & 255)) {
            K.n_recs = K.n_arena = 0;
            sandbox_off();
            prsp_native_sandbox(1);
        }
        *K.v.cr[10] = *K.v.cr[8] = *K.v.cr[9] = cur;
        hle_mtc0(&t, 9, cur);
        hle_dma_write(&t, 0xba8, cur, bytes - 1);
    }
    t0 = now_ns() - t0;
    sandbox_off();
    if (record) {
        prsp_native_record(0);
        armlet_event_listener = NULL;
    }
    return (double)t0 / iters;
}

/* ns per run of the front-end from `before`, state loaded each time (the load is not timed) */
double armlet_hle_bench(const struct hle_ucode *uc, const PrspNativeSnap *before, unsigned iters)
{
    unsigned i;
    uint64_t total = 0;
    init_once();
    K.verify = 0;
    for (i = 0; i < iters; i++) {
        uint64_t t0;
        prsp_native_load(before);
        prsp_native_resync();
        t0 = now_ns();
        if (native_run(NULL, uc, 0, 0) != HLE_OK)
            return -1.0;
        sandbox_off();
        total += now_ns() - t0;
    }
    return (double)total / iters;
}

/* a digest of the commit log (records and the bytes of the DMA writes) */
static uint64_t log_digest(void)
{
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i;
    for (i = 0; i < K.n_recs; i++) {
        const struct rec *r = &K.recs[i];
        uint32_t v[7] = { r->kind, r->rd, r->a, r->b, r->c, r->d, 0 };
        unsigned k;
        for (k = 0; k < 7; k++)
            h = (h ^ v[k]) * 0x100000001b3ull;
    }
    for (i = 0; i < K.n_arena; i++)
        h = (h ^ K.arena[i]) * 0x100000001b3ull;
    return h;
}

int armlet_hle_harness(const struct hle_ucode *uc, const PrspNativeSnap *before, const PrspNativeSnap *after, const PrspNativeEvent *ev, size_t n_ev, char *out,
                       size_t outsz, struct hle_harness_result *res)
{
    struct report r;
    uint64_t t0;
    size_t i;
    int rc;

    init_once();
    K.verify = 0;
    prsp_native_load(before);
    t0 = now_ns();
    rc = native_run(NULL, uc, 1, 0);
    if (res) {
        res->declined = rc != HLE_OK;
        res->native_us = (double)(now_ns() - t0) / 1000.0;
    }
    r.n = 0;
    r.count = 0;
    if (rc != HLE_OK) {
        snprintf(out, outsz, "declined: %s\n", K.why ? K.why : "(no reason)");
        return -1;
    }
    prsp_native_save(K.sn);
    sandbox_off();
    prsp_native_record(0);
    armlet_event_listener = NULL;
    {   /* the same run again with nothing recording (the fast DMA paths of hle_dma_read / hle_dma_write): the same end state and the same commit log */
        uint64_t d0 = log_digest();
        PrspNativeSnap *sj = K.sj;
        prsp_native_load(before);
        if (native_run(NULL, uc, 0, 0) != HLE_OK) {
            r.count++;
            rep(&r, "  the run without recording declined: %s\n", K.why ? K.why : "?");
        } else {
            prsp_native_save(sj);
            sandbox_off();
            if (memcmp(sj->dmem, K.sn->dmem, sizeof(sj->dmem)) || memcmp(sj->imem, K.sn->imem, sizeof(sj->imem)) || memcmp(sj->cr, K.sn->cr, sizeof(sj->cr)) || sj->irq != K.sn->irq || sj->pc != K.sn->pc)
                rep(&r, "  the run without recording ends in another state (DMEM / IMEM / registers)\n");
            if (log_digest() != d0)
                rep(&r, "  the run without recording leaves another commit log\n");
        }
    }
    side_reset(1, uc);
    K.ev[1].n = 0;
    for (i = 0; i < n_ev; i++) {
        if (ev[i].kind == HLE_EV_STREAM) {
            K.st[1].len = ev[i].a | (uint64_t)ev[i].b << 32;
            K.st[1].hash = ev[i].hash;
            continue;
        }
        K.ev[1].v = (PrspNativeEvent *)grow(K.ev[1].v, &K.ev[1].cap, K.ev[1].n + 1, sizeof(*K.ev[1].v));
        K.ev[1].v[K.ev[1].n++] = ev[i];
    }
    compare_tasks(&r, uc, K.sn, after, 0);
    snprintf(out, outsz, "%.*s", (int)r.n, r.buf);
    return (int)r.count;
}

int armlet_hle_harness_jit(const struct hle_ucode *uc, const PrspNativeSnap *before, void (*jit)(void *), void *jctx, char *out, size_t outsz, struct hle_harness_result *res)
{
    struct report r;
    uint64_t t0;
    int rc;

    init_once();
    K.verify = 0;
    prsp_native_load(before);
    t0 = now_ns();
    rc = native_run(NULL, uc, 1, 0);
    if (res) {
        res->declined = rc != HLE_OK;
        res->native_us = (double)(now_ns() - t0) / 1000.0;
    }
    r.n = 0;
    r.count = 0;
    if (rc != HLE_OK) {
        snprintf(out, outsz, "declined: %s\n", K.why ? K.why : "(no reason)");
        return -1;
    }
    prsp_native_save(K.sn);
    if (!K.ram_n)
        K.ram_n = (uint8_t *)malloc(0x800000);
    if (!K.ram_n)
        abort();
    memcpy(K.ram_n, K.v.rdram, 0x800000);
    sandbox_off();
    prsp_native_record(0);
    armlet_event_listener = NULL;
    prsp_native_load(before);
    prsp_native_resync();
    side_reset(1, uc);
    K.cur_ev = 1;
    armlet_event_listener = ev_cb;
    prsp_native_record(1);
    jit(jctx);
    prsp_native_record(0);
    armlet_event_listener = NULL;
    prsp_native_save(K.sj);
    compare_tasks(&r, uc, K.sn, K.sj, 1);
    snprintf(out, outsz, "%.*s", (int)r.n, r.buf);
    return (int)r.count;
}

void armlet_hle_core_reset(void)
{
    report(1);
    K.tasks = K.native = K.declined = K.verified = K.mismatches = K.ticks = 0;
    memset(K.reasons, 0, sizeof(K.reasons));
}

#else
const struct hle_ucode *armlet_hle_find(uint64_t id) { (void)id; return NULL; }
const struct hle_ucode *armlet_hle_next(const struct hle_ucode *prev) { (void)prev; return NULL; }
int armlet_hle_run(struct armlet_ctx *c, const struct armlet_ucode_id *u) { (void)c; (void)u; return 0; }
void armlet_hle_core_reset(void) {}
#endif
