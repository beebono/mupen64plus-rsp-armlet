/*
 * arMLEt - validate.c: the background validator (validate.h; docs/design.md, "The background validator and the latch policy")
 *
 * Threading. One validator thread at SCHED_IDLE (nice 19 where that is refused): the kernel gives it a core only when nothing else wants one, so it uses the spare core when foir's
 * workers are saturated and the workers' idle time when they are not, without arMLEt having to see foir's internals, and it never takes time from the emulation thread or the video
 * plugin. The emulation thread never waits for it: the queue is a fixed set of slots (GfxLatchPolicy queue=, default 4), a capture starts only in a free slot, and a slot moves
 * FREE -> ARMED -> CAPTURING -> QUEUED (emulation thread) -> RUNNING -> FREE (validator) through atomics, with a semaphore to wake the thread. The front-ends' own static state is
 * shared: a replay holds the front-end lock, which the emulation thread only tries (hle_core.c: the task then runs on the JIT).
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "armlet.h"
#include "validate.h"

#ifdef ARMLET_NATIVE
#include "hle_internal.h"
#include "native/native.h"
#include "native/native_internal.h"

/* test builds: 444 replays without the front-end lock (the TSan run must report the races it prevents; docs/testing.md) */
#if defined(ARMLET_HLE_MUTATE) && ARMLET_HLE_MUTATE == 444
#define FE_LOCK() ((void)0)
#define FE_UNLOCK() ((void)0)
#else
#define FE_LOCK() armlet_hle_fe_lock()
#define FE_UNLOCK() armlet_hle_fe_unlock()
#endif

#define VAL_ROWS_MAX (2u << 20)                 /* the RDRAM rows a task's DMA read, at most (more: the task is not captured) */
#define VAL_WR_MAX (1u << 20)                   /* the RDRAM it wrote outside the ring, at most */
#define VAL_STREAM_KEEP (1u << 20)              /* RDP stream bytes kept for the near-match tolerance (a longer stream is compared by its hash) */
#define VAL_WEV_MAX 65536u

enum { S_FREE, S_ARMED, S_CAPTURING, S_QUEUED, S_RUNNING };

struct vslot {
    _Atomic int state;
    uint64_t seq;
    struct val_target tg;
    PrspNativeSnap s0, s1;
    uint8_t *rows;                              /* { u32 RDRAM address, u32 length, the bytes } in the order read */
    size_t rows_n, rows_cap;
    PrspNativeEvent *wev;                       /* the JIT's DMA writes */
    size_t n_wev, cap_wev;
    struct hle_stream st;
    struct hle_dx_wr *wr;                       /* the rows written outside the ring, with their bytes at the end of the task in wrb */
    size_t n_wr, cap_wr;
    uint8_t *wrb;
    size_t wrb_n, wrb_cap;
    const char *bad;                            /* the capture failed (too large): dropped */
};

static struct {
    int on;
    pthread_t th;
    sem_t sem;
    _Atomic int stop;
    struct vslot *slots;
    unsigned n;
    struct vslot *armed, *cur;                  /* emulation thread */
    val_result_fn fn;
    PrspNativeView v;
    uint64_t seq;
    /* emulation thread's statistics */
    uint64_t armed_n, captured, dropped_busy, dropped_capture;
    uint64_t capture_ns;
    /* the validator thread's (read after it stopped) */
    uint64_t replayed, matched, mismatched, declined, inconclusive, irrelevant;
    uint64_t replay_cpu_ns, thread_cpu_ns, thread_c0;
    _Atomic uint64_t slot_bytes;
    struct hle_dx dx;
} V;

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline uint64_t cpu_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int reserve(void **p, size_t *cap, size_t need, size_t limit)
{
    size_t c = *cap ? *cap : 4096;
    void *q;
    if (need <= *cap)
        return 0;
    if (need > limit)
        return -1;
    while (c < need)
        c *= 2;
    if (c > limit)
        c = limit;
    q = realloc(*p, c);
    if (!q)
        return -1;
    atomic_fetch_add_explicit(&V.slot_bytes, c - *cap, memory_order_relaxed);
    *p = q;
    *cap = c;
    return 0;
}

/* ---- the emulation thread: arming and capturing ------------------------------------------------------------------------------------------------------------------------------ */
int armlet_valid_running(void) { return V.on; }

int armlet_valid_room(void)
{
    unsigned i;
    if (!V.on)
        return 0;
    if (V.armed)
        return 1;
    for (i = 0; i < V.n; i++)
        if (atomic_load_explicit(&V.slots[i].state, memory_order_acquire) == S_FREE)
            return 1;
    return 0;
}

int armlet_valid_arm(const struct val_target *t)
{
    unsigned i;
    if (!V.on)
        return -1;
    if (V.armed) {                                  /* (an armed slot whose task did not reach the JIT: retargeted) */
        V.armed->tg = *t;
        return 0;
    }
    for (i = 0; i < V.n; i++) {
        int f = S_FREE;
        if (atomic_compare_exchange_strong_explicit(&V.slots[i].state, &f, S_ARMED, memory_order_acquire, memory_order_relaxed)) {
            V.slots[i].tg = *t;
            V.armed = &V.slots[i];
            V.armed_n++;
            return 0;
        }
    }
    V.dropped_busy++;
    return -1;
}

static void rows_save(struct vslot *s, const PrspNativeEvent *e)
{
    uint32_t cache = e->a & ~3u, dram = e->b & ~7u, lenreg = e->c;
    uint32_t length = ((lenreg & 0xfff) + 1 + 7) & ~7u, skip = (lenreg >> 20) & 0xfff;
    unsigned n = (lenreg >> 12) & 0xff, row;
    if (((cache & 0xfff) + length) > 0x1000)
        length = 0x1000 - (cache & 0xfff);
    for (row = 0; row <= n && !s->bad; row++, dram += length + skip) {
        uint32_t a = dram & 0x7ffffc, j;
        uint32_t hdr[2] = { a, length };
        if (reserve((void **)&s->rows, &s->rows_cap, s->rows_n + 8 + length, VAL_ROWS_MAX)) {
            s->bad = "the task reads more RDRAM than a capture keeps";
            return;
        }
        memcpy(s->rows + s->rows_n, hdr, 8);
        s->rows_n += 8;
        for (j = 0; j < length; j += 4)
            memcpy(s->rows + s->rows_n + j, (const uint8_t *)V.v.rdram + ((a + j) & 0x7ffffc), 4);
        s->rows_n += length;
    }
}

static void v_ev(const PrspNativeEvent *e)
{
    struct vslot *s = V.cur;
    if (!s || s->bad)
        return;
    switch (e->kind) {
    case PRSP_EV_DMA_RD:
        rows_save(s, e);
        break;
    case PRSP_EV_DMA_WR:
        if (reserve((void **)&s->wev, &s->cap_wev, (s->n_wev + 1) * sizeof(*s->wev), VAL_WEV_MAX * sizeof(*s->wev)))
            s->bad = "too many DMA writes";
        else
            s->wev[s->n_wev++] = *e;
        break;
    case PRSP_EV_CMD_END:
        hle_stream_add(&s->st, e, V.v.dmem, V.v.rdram);
        break;
    default:
        break;
    }
}

void armlet_valid_jit_begin(struct armlet_ctx *c)
{
    struct vslot *s = V.armed;
    uint64_t t0;
    if (!s)
        return;
    V.armed = NULL;
    t0 = now_ns();
    /* the conditions of a native run (armlet_hle_run): the replay could not run the task otherwise; and nobody else listens (a capture or a verify run) */
    if (armlet_event_listener || (*c->rsp.SP_STATUS_REG & (SP_STATUS_HALT | SP_STATUS_BROKE)) || (*c->rsp.SP_PC_REG & 0xfff) != 0 ||
        ((*c->rsp.DPC_STATUS_REG & 1) && !(s->tg.run->flags & HLE_F_XBUS_OK))) {
        V.dropped_capture++;
        atomic_store_explicit(&s->state, S_FREE, memory_order_release);
        return;
    }
    prsp_native_save(&s->s0);
    s->s0.pc = *c->rsp.SP_PC_REG & 0xfff;           /* (the JIT's own pc field is the last task's until its run loop starts: the task starts at SP_PC) */
    s->rows_n = 0;
    s->n_wev = 0;
    s->n_wr = 0;
    s->wrb_n = 0;
    s->bad = NULL;
    hle_stream_reset(&s->st, s->tg.run);
    s->st.keep = VAL_STREAM_KEEP;
    atomic_store_explicit(&s->state, S_CAPTURING, memory_order_relaxed);
    V.cur = s;
    armlet_event_listener = v_ev;
    prsp_native_record(1);
    V.capture_ns += now_ns() - t0;
}

/* the rows of the JIT's DMA writes outside the RDP output ring, with their bytes as the task left them */
static void jit_writes(struct vslot *s)
{
    const struct hle_ucode *u = s->tg.run;
    uint32_t ring_lo = 0, ring_hi = 0;
    size_t k;
    if (u->ring_off) {
        ring_lo = s->s1.dmem[u->ring_off >> 2] & 0x7ffff8;
        ring_hi = s->s1.dmem[(u->ring_off >> 2) + 1] & 0x7ffff8;
    }
    for (k = 0; k < s->n_wev && !s->bad; k++) {
        const PrspNativeEvent *e = &s->wev[k];
        uint32_t length = ((e->c & 0xfff) + 1 + 7) & ~7u, skip = (e->c >> 20) & 0xfff, d = e->b & 0x7ffff8, j;
        unsigned count = (e->c >> 12) & 0xff, row;
        if (((e->a & 0x1ffc & 0xfff) + length) > 0x1000)
            length = 0x1000 - (e->a & 0x1ffc & 0xfff);
        for (row = 0; row <= count && !s->bad; row++, d += length + skip) {
            uint32_t a = d & 0x7ffffc;
            if (u->ring_off && ring_hi > ring_lo && a >= ring_lo && a + length <= ring_hi)
                continue;                           /* (the RDP lists: compared as a stream) */
            if (!u->ring_off && hle_dead_dmem_of(u, e->a & 0xfff))
                continue;
            for (j = 0; j < length; j += 4) {       /* word by word, merged with the range before (a row wraps at the end of RDRAM) */
                uint32_t w = (a + j) & 0x7ffffc;
                struct hle_dx_wr *last = s->n_wr ? &s->wr[s->n_wr - 1] : NULL;
                if (last && last->a + last->n == w) {
                    last->n += 4;
                } else {
                    size_t q;
                    int dup = 0;
                    for (q = 0; q < s->n_wr && !dup; q++)
                        dup = w >= s->wr[q].a && w + 4 <= s->wr[q].a + s->wr[q].n;
                    if (dup)
                        continue;
                    if (reserve((void **)&s->wr, &s->cap_wr, (s->n_wr + 1) * sizeof(*s->wr), 65536 * sizeof(*s->wr))) {
                        s->bad = "too many RDRAM ranges written";
                        break;
                    }
                    s->wr[s->n_wr].a = w;
                    s->wr[s->n_wr++].n = 4;
                }
            }
        }
    }
    for (k = 0; k < s->n_wr && !s->bad; k++) {
        if (reserve((void **)&s->wrb, &s->wrb_cap, s->wrb_n + s->wr[k].n, VAL_WR_MAX)) {
            s->bad = "the task writes more RDRAM than a capture keeps";
            break;
        }
        memcpy(s->wrb + s->wrb_n, (const uint8_t *)V.v.rdram + s->wr[k].a, s->wr[k].n);
        s->wrb_n += s->wr[k].n;
    }
}

void armlet_valid_jit_end(struct armlet_ctx *c)
{
    struct vslot *s = V.cur;
    uint64_t t0;
    (void)c;
    if (!s)
        return;
    t0 = now_ns();
    V.cur = NULL;
    prsp_native_record(0);
    armlet_event_listener = NULL;
    prsp_native_save(&s->s1);
    if (!s->bad && !(s->s1.cr[4] & SP_STATUS_BROKE))
        s->bad = "the JIT did not finish the task in one run";
    if (!s->bad)
        jit_writes(s);
    if (s->bad) {
        V.dropped_capture++;
        atomic_store_explicit(&s->state, S_FREE, memory_order_release);
    } else {
        s->seq = ++V.seq;
        V.captured++;
        if (!(V.captured & 31))
            armlet_log(M64MSG_INFO, "validator: %" PRIu64 " tasks armed, %" PRIu64 " captured, %" PRIu64 " not captured (no free slot), %" PRIu64 " dropped; capture %.1f ms on the emulation thread",
                       V.armed_n, V.captured, V.dropped_busy, V.dropped_capture, (double)V.capture_ns / 1e6);
        atomic_store_explicit(&s->state, S_QUEUED, memory_order_release);
        sem_post(&V.sem);
    }
    V.capture_ns += now_ns() - t0;
}

/* ---- the validator thread ------------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ARMLET_VALID_DUMP=<dir> (development): a capture whose replay did not come out equal is written as a task snapshot (tools/native/replay.c reads it: armlet-native-replay) */
static void dump(const struct vslot *s, const char *what)
{
    static unsigned n;
    const char *dir = getenv("ARMLET_VALID_DUMP");
    char path[600];
    struct armlet_snapfile_hdr h;
    uint32_t nrows = 0;
    size_t off;
    FILE *f;
    if (!dir || !*dir || n >= 64)
        return;
    snprintf(path, sizeof(path), "%s/valid-%016" PRIx64 "-%s-%u.snap", dir, s->tg.cand->id, what, n++);
    if (!(f = fopen(path, "wb")))
        return;
    for (off = 0; off + 8 <= s->rows_n; nrows++) {
        uint32_t hdr[2];
        memcpy(hdr, s->rows + off, 8);
        off += 8 + hdr[1];
    }
    memset(&h, 0, sizeof(h));
    h.magic = ARMLET_SNAPFILE_MAGIC;
    h.version = 1;
    snprintf(h.routine, sizeof(h.routine), "hle.task");
    h.ucode = s->tg.run->id;
    h.entry_pc = s->s0.pc;
    h.exit_pc = s->s1.pc;
    h.n_rows = nrows;
    h.n_events = (uint32_t)s->n_wev + 1;
    fwrite(&h, sizeof(h), 1, f);
    fwrite(&s->s0, sizeof(s->s0), 1, f);
    fwrite(s->rows, 1, s->rows_n, f);
    fwrite(&s->s1, sizeof(s->s1), 1, f);
    fwrite(s->wev, sizeof(*s->wev), s->n_wev, f);
    {
        PrspNativeEvent se = { HLE_EV_STREAM, (uint32_t)s->st.len, (uint32_t)(s->st.len >> 32), 0, s->st.hash };
        fwrite(&se, sizeof(se), 1, f);
    }
    fclose(f);
}

static void replay(struct vslot *s)
{
    struct hle_dx *x = &V.dx;
    struct val_result r;
    struct hle_probe_opts opts;
    struct hle_dx_jit j;
    static char rep[1600];
    size_t off;
    unsigned k;
    int rc, any = 0;
    uint64_t c0 = cpu_ns();

    memset(&r, 0, sizeof(r));
    /* the rows, the earliest read last: an address read twice (the task wrote it in between) holds what the task found there first */
    {
        size_t *starts = NULL, n = 0, cap = 0;
        for (off = 0; off + 8 <= s->rows_n;) {
            uint32_t hdr[2];
            memcpy(hdr, s->rows + off, 8);
            if (n == cap) {
                size_t *q = (size_t *)realloc(starts, (cap = cap ? cap * 2 : 256) * sizeof(*starts));
                if (!q)
                    break;
                starts = q;
            }
            starts[n++] = off;
            off += 8 + hdr[1];
        }
        while (n-- > 0) {
            uint32_t hdr[2];
            memcpy(hdr, s->rows + starts[n], 8);
            if (hdr[0] + hdr[1] <= 0x800000)
                memcpy(x->rdram + hdr[0], s->rows + starts[n] + 8, hdr[1]);
        }
        free(starts);
    }
    x->cand = s->tg.cand;
    FE_LOCK();
    rc = armlet_hle_dx_run(x, s->tg.run, &s->s0);
    FE_UNLOCK();
    for (k = 0; k < LATCH_NKINDS && !any; k++)
        any = x->hist[k] != 0;
    r.hist = x->hist;
    r.fp = latch_fingerprint(x->hist);
    if (s->tg.cand != s->tg.run && !any) {
        r.outcome = VAL_IRRELEVANT;                 /* (the task did not load the text under validation) */
    } else if (rc != HLE_OK) {
        r.outcome = x->why && strstr(x->why, "status poll timeout has dropped") ? VAL_INCONCLUSIVE : VAL_DECLINED;
        r.why = x->why;
    } else {
        unsigned nd;
        int tol = 0;
        struct latch_diff df = { 0, 0, 0 };
        memset(&opts, 0, sizeof(opts));
        opts.tol = s->tg.tol;
        opts.loose_levels = s->tg.loose_levels;
        opts.dmem_ok = (s->tg.tol || s->tg.loose_levels) ? s->tg.dmem_ok : NULL;
        j.s1 = &s->s1;
        j.st = &s->st;
        j.wr = s->wr;
        j.n_wr = s->n_wr;
        j.wr_bytes = s->wrb;
        rep[0] = 0;
        nd = armlet_hle_dx_compare(x, s->tg.run, &j, &opts, rep, sizeof(rep), &tol, s->tg.loose_levels ? &df : NULL);
        r.outcome = nd ? VAL_MISMATCH : VAL_MATCH;
        r.tol = tol;
        r.grade = s->tg.loose_levels ? latch_grade(&df) : LATCH_G_STRUCTURAL;
        r.report = rep;
    }
    /* the replay's RDRAM back to zeros (the pages it touched are dropped) */
    madvise(x->rdram, 0x800000, MADV_DONTNEED);
    r.replay_us = (double)(cpu_ns() - c0) / 1000.0;
    V.replay_cpu_ns += cpu_ns() - c0;
    V.replayed++;
    switch (r.outcome) {
    case VAL_MATCH: V.matched++; break;
    case VAL_MISMATCH: V.mismatched++; break;
    case VAL_DECLINED: V.declined++; break;
    case VAL_INCONCLUSIVE: V.inconclusive++; break;
    default: V.irrelevant++; break;
    }
    if (r.outcome == VAL_MISMATCH || r.outcome == VAL_DECLINED)
        dump(s, r.outcome == VAL_MISMATCH ? "differs" : "declined");
    if (V.fn)
        V.fn(s->tg.owner, &r);
    if (!(V.replayed & 31))
        armlet_log(M64MSG_INFO, "validator: %" PRIu64 " replayed: %" PRIu64 " equal, %" PRIu64 " different, %" PRIu64 " declined, %" PRIu64 " inconclusive, %" PRIu64 " without the build; replay %.1f ms CPU, thread %.1f ms CPU, slots %.0f KiB",
                   V.replayed, V.matched, V.mismatched, V.declined, V.inconclusive, V.irrelevant, (double)V.replay_cpu_ns / 1e6, (double)(cpu_ns() - V.thread_c0) / 1e6,
                   (double)atomic_load_explicit(&V.slot_bytes, memory_order_relaxed) / 1024.0);
}

static void lower_priority(void)
{
#ifdef SCHED_IDLE
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    if (pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp) == 0)
        return;
#endif
    (void)setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);   /* (Linux: per thread) */
}

static void *v_main(void *arg)
{
    uint64_t c0 = cpu_ns();
    (void)arg;
    V.thread_c0 = c0;
    lower_priority();
#ifdef __GLIBC__
    pthread_setname_np(pthread_self(), "armlet-valid");
#endif
    for (;;) {
        struct vslot *best = NULL;
        unsigned i;
        while (sem_wait(&V.sem) && errno == EINTR)
            ;
        if (atomic_load_explicit(&V.stop, memory_order_acquire))
            break;
        for (i = 0; i < V.n; i++)
            if (atomic_load_explicit(&V.slots[i].state, memory_order_acquire) == S_QUEUED && (!best || V.slots[i].seq < best->seq))
                best = &V.slots[i];
        if (!best)
            continue;
        atomic_store_explicit(&best->state, S_RUNNING, memory_order_relaxed);
        replay(best);
        atomic_store_explicit(&best->state, S_FREE, memory_order_release);
    }
    V.thread_cpu_ns = cpu_ns() - c0;
    return NULL;
}

int armlet_valid_start(const struct latch_cfg *cfg, val_result_fn fn)
{
    void *m;
    if (V.on)
        return 0;
    memset(&V, 0, sizeof(V));
    V.n = cfg->queue ? cfg->queue : 4;
    V.slots = (struct vslot *)calloc(V.n, sizeof(*V.slots));
    V.dx.cpu = hle_dx_cpu_new();
    V.dx.dmem = (uint32_t *)calloc(1024, 4);
    V.dx.imem = (uint32_t *)calloc(1024, 4);
    m = mmap(NULL, 0x800000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    V.dx.rdram = m == MAP_FAILED ? NULL : (uint8_t *)m;
    if (!V.slots || !V.dx.cpu || !V.dx.dmem || !V.dx.imem || !V.dx.rdram || sem_init(&V.sem, 0, 0)) {
        armlet_valid_stop();
        armlet_log(M64MSG_WARNING, "validator: no memory; derived builds will not latch");
        return -1;
    }
    atomic_store(&V.slot_bytes, (uint64_t)V.n * sizeof(*V.slots));
    V.fn = fn;
    armlet_native_ensure_host();
    prsp_native_view(&V.v);
    armlet_hle_core_init();                         /* (its settings are read once, before the thread can look at them) */
    armlet_hle_fe_enable(1);
    if (pthread_create(&V.th, NULL, v_main, NULL)) {
        armlet_hle_fe_enable(0);
        sem_destroy(&V.sem);
        armlet_valid_stop();
        armlet_log(M64MSG_WARNING, "validator: no thread; derived builds will not latch");
        return -1;
    }
    V.on = 1;
    return 0;
}

void armlet_valid_stats(struct val_stats *o)
{
    memset(o, 0, sizeof(*o));
    o->armed = V.armed_n;
    o->captured = V.captured;
    o->dropped_busy = V.dropped_busy;
    o->dropped_capture = V.dropped_capture;
    o->replayed = V.replayed;
    o->matched = V.matched;
    o->mismatched = V.mismatched;
    o->declined = V.declined;
    o->inconclusive = V.inconclusive;
    o->irrelevant = V.irrelevant;
    o->capture_ms = (double)V.capture_ns / 1e6;
    o->replay_cpu_ms = (double)V.replay_cpu_ns / 1e6;
    o->thread_cpu_ms = (double)V.thread_cpu_ns / 1e6;
    o->slot_bytes = atomic_load(&V.slot_bytes);
}

void armlet_valid_stop(void)
{
    unsigned i;
    if (V.on) {
        struct val_stats st;
        if (V.cur) {                                /* (RomClosed in the middle of a task: the capture is dropped) */
            prsp_native_record(0);
            armlet_event_listener = NULL;
            V.cur = NULL;
        }
        atomic_store_explicit(&V.stop, 1, memory_order_release);
        sem_post(&V.sem);
        pthread_join(V.th, NULL);
        armlet_hle_fe_enable(0);
        sem_destroy(&V.sem);
        V.on = 0;
        armlet_valid_stats(&st);
        armlet_log(M64MSG_INFO, "validator: %" PRIu64 " tasks armed, %" PRIu64 " captured, %" PRIu64 " not captured (no free slot), %" PRIu64 " dropped (capture); %" PRIu64 " replayed: %" PRIu64
                   " equal, %" PRIu64 " different, %" PRIu64 " declined, %" PRIu64 " inconclusive, %" PRIu64 " without the build; capture %.1f ms on the emulation thread, replay %.1f ms CPU (thread %.1f ms), slots %.0f KiB at most",
                   st.armed, st.captured, st.dropped_busy, st.dropped_capture, st.replayed, st.matched, st.mismatched, st.declined, st.inconclusive, st.irrelevant, st.capture_ms, st.replay_cpu_ms,
                   st.thread_cpu_ms, (double)st.slot_bytes / 1024.0);
    }
    for (i = 0; V.slots && i < V.n; i++) {
        free(V.slots[i].rows);
        free(V.slots[i].wev);
        free(V.slots[i].st.bytes);
        free(V.slots[i].wr);
        free(V.slots[i].wrb);
    }
    free(V.slots);
    free(V.dx.dmem);
    free(V.dx.imem);
    free(V.dx.st.bytes);
    free(V.dx.wr);
    if (V.dx.cpu)
        hle_dx_cpu_free(V.dx.cpu);
    if (V.dx.rdram)
        munmap(V.dx.rdram, 0x800000);
    memset(&V, 0, sizeof(V));
}
#endif
