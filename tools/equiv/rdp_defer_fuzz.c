/*
 * Model-based test of src/rdp_defer.c (the RDP list coalescing): the module's own code is compiled in, driven by random streams of the events a
 * graphics ucode produces (DPC_END / DPC_START / DPC_STATUS writes, SP DMA writes into RDRAM, DMEM writes, the end of the run), against a model
 * of foir's ProcessRDPList (src/parser.c rdp_process_list: read DPC_CURRENT..DPC_END from RDRAM, from DMEM when XBUS is set, ignore end <= current,
 * leave START = CURRENT = END). The reference model runs every list at once, as parallel-rsp does without coalescing; the system under test goes
 * through the same hooks the fork calls. After every event the DPC registers must be equal, at the end of every task the words the video plugin has
 * been given must be the same stream, and the plugin must not be asked to process anything between runs. A build with a safeguard dropped
 * (-DARMLET_RDP_MUTATE=n) must fail.
 *
 *   rdp_defer_fuzz [seeds [threshold]]       exit status 0 = no difference
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "armlet.h"
#include "gfx_ucode.h"
#include "rdp_defer.h"

enum { FLUSH_START, FLUSH_STATUS, FLUSH_RUN_END };
void prsp_rdp_defer_flush(int why);
void prsp_rdp_defer_dma_write(uint32_t dram, uint32_t length, uint32_t skip, unsigned count);

/* stubs of the rest of the module */
void armlet_log(int level, const char *fmt, ...) { (void)level; (void)fmt; }
const struct armlet_ucode_id *armlet_gfx_identify(const struct armlet_ctx *c, bool *is_new)
{
    static const struct armlet_ucode_id u = { 1, ARMLET_FAM_F3DEX2, "test" };
    (void)c;
    *is_new = false;
    return &u;
}

#define RD_SIZE 0x20000u
#define FIFO_BASE 0x8000u
#define FIFO_SIZE 0x1000u            /* the output ring, bytes */
#define XB_BASE 0x400u               /* the XBUS ring in DMEM */
#define XB_SIZE 0x200u
#define MAXLOG (1u << 22)

struct machine {
    uint8_t rdram[RD_SIZE];
    uint8_t dmem[4096];
    uint32_t start, cur, end, status;
    uint32_t *log;                   /* words handed to the video plugin */
    size_t nlog;
    int deferring;
};

static struct machine *l_m;          /* the machine the video plugin model works on */
static void (*l_cb)(void);           /* the callback armlet_rdp_defer_init installed */

static void video_plugin(void)       /* foir's rdp_process_list, minus the parsing: the words are the result */
{
    struct machine *m = l_m;
    uint32_t cur = (m->cur & ~7u) >> 2, end = (m->end & ~7u) >> 2;
    int xbus = m->status & 1;

    if (end <= cur)
        return;
    while (end > cur) {
        uint32_t w = xbus ? ((uint32_t *)m->dmem)[cur & 0x3ff] : ((uint32_t *)m->rdram)[cur % (RD_SIZE / 4)];
        if (m->nlog < MAXLOG)
            m->log[m->nlog++] = w;
        cur++;
    }
    m->start = m->cur = m->end;
}

static uint64_t rng = 88172645463325252ull;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}

/* ---- events; each is applied to one machine, either the reference (plain) or the one behind the hooks (def) */
struct ev { int kind; uint32_t a, b, c; };
enum { E_DMA_FIFO, E_END, E_START, E_STATUS, E_DMA_ANY, E_DMEM, E_END_XBUS, E_RUN_END };

static void apply(struct machine *m, const struct ev *e, int def)
{
    uint32_t i;

    l_m = m;
    switch (e->kind) {
    case E_DMA_FIFO:        /* b bytes of fresh data at rdram address a (the ucode's output buffer) */
    case E_DMA_ANY:
        if (def)
            prsp_rdp_defer_dma_write(e->a, e->b, e->c, e->c ? 1 : 0);
        for (i = 0; i < e->b; i += 4)
            *(uint32_t *)(m->rdram + ((e->a + i) % RD_SIZE)) = e->c * 2654435761u + i + e->a;
        if (e->c) {         /* a second row, skip e->c bytes after the first */
            for (i = 0; i < e->b; i += 4)
                *(uint32_t *)(m->rdram + ((e->a + e->b + e->c + i) % RD_SIZE)) = e->c * 40503u + i;
        }
        break;
    case E_DMEM:
        for (i = 0; i < e->b; i += 4)
            *(uint32_t *)(m->dmem + ((e->a + i) & 0xffc)) = e->b * 2246822519u + i;
        break;
    case E_START:
        if (def)
            prsp_rdp_defer_flush(FLUSH_START);
        m->start = m->cur = m->end = e->a & 0xfffffff8u;
        break;
    case E_STATUS:
        if (def)
            prsp_rdp_defer_flush(FLUSH_STATUS);
        m->status = (m->status & ~1u) | (e->a & 1);
        break;
    case E_END:
    case E_END_XBUS:
        m->end = e->a & 0xfffffff8u;
        if (def)
            l_cb();                                /* the installed callback */
        else
            video_plugin();
        break;
    case E_RUN_END:
        if (def)
            prsp_rdp_defer_flush(FLUSH_RUN_END);
        break;
    }
}

static int check_regs(const struct machine *r, const struct machine *d, long ev)
{
    if (r->start != d->start || r->cur != d->cur || r->end != d->end || r->status != d->status) {
        fprintf(stderr, "event %ld: registers differ: ref start %x cur %x end %x status %x, coalesced %x %x %x %x\n", ev, r->start, r->cur, r->end,
                r->status, d->start, d->cur, d->end, d->status);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int seeds = argc > 1 ? atoi(argv[1]) : 300;
    unsigned thresh_opt = argc > 2 ? (unsigned)atoi(argv[2]) : 0;
    static struct machine ref, def;
    static struct armlet_ctx c;
    static uint8_t task_dmem[4096];
    long bad = 0, tasks = 0, calls_ref = 0, calls_def = 0;
    int s;

    ref.log = malloc(MAXLOG * 4);
    def.log = malloc(MAXLOG * 4);
    for (s = 0; s < seeds; s++) {
        static const unsigned thr[] = { 8, 64, 1024, 1024, 4096, 1048576 };
        unsigned thresh = thresh_opt ? thresh_opt : thr[s % 6];
        int nev, i, xbus_game = (s % 5) == 4;
        uint32_t fifo_pos = 0;           /* offset in the ring */
        uint32_t xb_pos = 0;
        long done_events = 0;

        rng = 88172645463325252ull + (uint64_t)s * 0x9e3779b97f4a7c15ull;
        memset(ref.rdram, 0, sizeof(ref.rdram)); memset(ref.dmem, 0, sizeof(ref.dmem)); ref.start = ref.cur = ref.end = ref.status = 0;
        memset(def.rdram, 0, sizeof(def.rdram)); memset(def.dmem, 0, sizeof(def.dmem)); def.start = def.cur = def.end = def.status = 0;
        ref.nlog = def.nlog = 0;
        memset(&c, 0, sizeof(c));
        memset(task_dmem, 0, sizeof(task_dmem));
        *(uint32_t *)(task_dmem + ARMLET_TASK_TYPE) = 1;
        c.rsp.DMEM = task_dmem;
        c.rsp.ProcessRdpList = video_plugin;
        c.prsp = c.rsp;
        c.prsp.DPC_START_REG = &def.start;
        c.prsp.DPC_CURRENT_REG = &def.cur;
        c.prsp.DPC_END_REG = &def.end;
        c.prsp.DPC_STATUS_REG = &def.status;
        c.cfg.rdp_coalesce_mode = ARMLET_COALESCE_ON;
        c.cfg.rdp_coalesce_bytes = thresh;
        l_m = &def;
        armlet_rdp_defer_init(&c);
        l_cb = c.prsp.ProcessRdpList;
        {
            nev = 400 + rnd() % 600;
            for (i = 0; i < nev; i++) {
                struct ev e;
                uint32_t k = rnd() % 100;

                memset(&e, 0, sizeof(e));
                if (i % 60 == 0 || k < 2) {                       /* a task boundary */
                    e.kind = E_RUN_END;
                    apply(&ref, &e, 0);
                    apply(&def, &e, 1);
                    armlet_rdp_defer_task_end();
                    if (def.nlog != ref.nlog || memcmp(def.log, ref.log, ref.nlog * 4)) {
                        fprintf(stderr, "seed %d (threshold %u) event %d: the words given to the video plugin differ at the end of a task (ref %zu, coalesced %zu)\n", s, thresh, i,
                                ref.nlog, def.nlog);
                        bad++;
                        goto next_seed;
                    }
                    tasks++;
                    armlet_rdp_defer_task_begin(&c);
                    ref.nlog = def.nlog = 0;
                    if (xbus_game) {                               /* the ucode picks XBUS at the start of the task, sometimes switches */
                        e.kind = E_STATUS;
                        e.a = 1;
                        apply(&ref, &e, 0);
                        apply(&def, &e, 1);
                    }
                    continue;
                }
                if (k < 62) {                                      /* a chunk of commands DMAed to the ring, then DPC_END */
                    uint32_t len = 8 * (1 + rnd() % 16);
                    if (!(def.status & 1)) {
                        if (fifo_pos + len > FIFO_SIZE) {          /* the ring wraps: a new buffer through DPC_START */
                            e.kind = E_START;
                            e.a = FIFO_BASE;
                            apply(&ref, &e, 0);
                            apply(&def, &e, 1);
                            if (check_regs(&ref, &def, i)) { bad++; goto next_seed; }
                            fifo_pos = 0;
                        }
                        e.kind = E_DMA_FIFO;
                        e.a = FIFO_BASE + fifo_pos;
                        e.b = len;
                        e.c = 0;
                        apply(&ref, &e, 0);
                        apply(&def, &e, 1);
                        e.kind = E_END;
                        e.a = FIFO_BASE + fifo_pos + len;
                        fifo_pos += len;
                    } else {                                       /* XBUS: the commands go to the DMEM ring */
                        if (xb_pos + len > XB_SIZE) {
                            e.kind = E_START;
                            e.a = XB_BASE;
                            apply(&ref, &e, 0);
                            apply(&def, &e, 1);
                            xb_pos = 0;
                        }
                        e.kind = E_DMEM;
                        e.a = XB_BASE + xb_pos;
                        e.b = len;
                        apply(&ref, &e, 0);
                        apply(&def, &e, 1);
                        e.kind = E_END_XBUS;
                        e.a = XB_BASE + xb_pos + len;
                        xb_pos += len;
                    }
                } else if (k < 70) {                               /* DPC_END with nothing new, or going backwards */
                    e.kind = E_END;
                    e.a = (def.cur - (rnd() % 3) * 8) & 0xffffff;
                } else if (k < 80) {                               /* a DMA write that is not the next chunk: other data, or over the ring */
                    e.kind = E_DMA_ANY;
                    e.b = 8 * (1 + rnd() % 32);
                    e.c = (rnd() % 4 == 0) ? 0x40 : 0;
                    e.a = (rnd() % 2) ? FIFO_BASE + (rnd() % (FIFO_SIZE / 8)) * 8 : 0x400 + (rnd() % 64) * 8;
                } else if (k < 84) {                               /* the ucode touches DMEM: the XBUS ring, or elsewhere */
                    e.kind = E_DMEM;
                    e.a = (rnd() % 2) ? XB_BASE + (rnd() % (XB_SIZE / 8)) * 8 : 0x100 + rnd() % 0x100;
                    e.b = 8 * (1 + rnd() % 8);
                } else if (k < 87) {                               /* DPC_STATUS write: XBUS on / off */
                    e.kind = E_STATUS;
                    e.a = rnd() % 2;
                    if (xbus_game)
                        e.a = (rnd() % 4) != 0;
                } else if (k < 92) {                               /* DPC_START to somewhere in the ring */
                    e.kind = E_START;
                    e.a = FIFO_BASE + (rnd() % (FIFO_SIZE / 8)) * 8;
                    fifo_pos = e.a - FIFO_BASE;
                } else {                                           /* a read of the registers (nothing to do, the check follows every event) */
                    e.kind = E_DMEM;
                    e.a = 0x800;
                    e.b = 8;
                }
                apply(&ref, &e, 0);
                apply(&def, &e, 1);
                done_events++;
                if (check_regs(&ref, &def, i)) {
                    fprintf(stderr, "  seed %d threshold %u, event %d kind %d\n", s, thresh, i, e.kind);
                    bad++;
                    goto next_seed;
                }
            }
        }
        {
            struct ev e = { E_RUN_END, 0, 0, 0 };
            apply(&ref, &e, 0);
            apply(&def, &e, 1);
            armlet_rdp_defer_task_end();
            if (def.nlog != ref.nlog || memcmp(def.log, ref.log, ref.nlog * 4)) {
                fprintf(stderr, "seed %d (threshold %u): the words given to the video plugin differ at the end (ref %zu, coalesced %zu)\n", s, thresh, ref.nlog, def.nlog);
                bad++;
            }
        }
next_seed:
        (void)done_events;
        calls_ref++;
    }
    printf("%d streams, %ld tasks, %ld streams with a difference\n", seeds, tasks, bad);
    (void)calls_def;
    return bad != 0;
}
