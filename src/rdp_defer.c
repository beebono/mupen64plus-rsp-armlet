/*
 * arMLEt - rdp_defer.c: exact coalescing of the ProcessRdpList calls of a graphics ucode
 *
 * A graphics ucode writes DPC_END after every chunk of RDP commands it has DMAed to its output buffer, and parallel-rsp calls the video
 * plugin's ProcessRdpList for each (Banjo-Kazooie: 1210 per frame, most of one command). Here the call is replaced by one that
 * records the range and leaves the registers as the video plugin would (DPC_START = DPC_CURRENT = DPC_END = the new end), so the
 * ucode reads exactly what it reads without deferral, and the plugin gets the whole range in one call when
 *   - the range reaches the threshold,
 *   - the ucode writes DPC_START or DPC_STATUS (prsp_rdp_defer_flush), or DMAs into RDRAM anywhere but at the end of the
 *     pending range (prsp_rdp_defer_dma_write),
 *   - the XBUS bit is set (the list is in DMEM, which the ucode overwrites),
 *   - the ucode stops (prsp_rdp_defer_flush after the run loop, before the interrupts are checked).
 * This depends on foir's ProcessRDPList (src/parser.c rdp_process_list): see docs/design.md for the argument.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gfx_ucode.h"
#include "rdp_defer.h"

/* prsp_rdp_defer.h's PRSP_RDP_FLUSH_* (C++ header): why the fork asks for a flush */
enum { FLUSH_START, FLUSH_STATUS, FLUSH_RUN_END };

/* Test builds only (-DARMLET_RDP_MUTATE=n, docs/testing.md): n deliberately drops one of the safeguards so the gates can be shown to catch it.
 * 1 no flush when the task ends, 2 no flush before a DMA write into RDRAM, 3 no flush at DPC_START / DPC_STATUS writes, 4 XBUS lists are
 * deferred too, 5 DPC_START / CURRENT are not advanced when a list is deferred. */
#ifndef ARMLET_RDP_MUTATE
#define ARMLET_RDP_MUTATE 0
#endif
#define MUTATED(n) (ARMLET_RDP_MUTATE == (n))

#define DPC_STATUS_XBUS 0x1u
#define RDRAM_MASK 0x7fffffu

#ifdef ARMLET_RDP_DEFER

static uint32_t *l_start_reg, *l_cur_reg, *l_end_reg, *l_status_reg;
static void (*l_real)(void);
static uint32_t l_thresh;           /* bytes */
static int l_task_on;               /* the running task is a known graphics ucode */
static uint32_t l_ps, l_pe;         /* the pending range [l_ps, l_pe), bytes; empty when l_ps == l_pe */

static struct {
    uint64_t tasks, tasks_on, calls, deferred, direct, flushes, f_thresh, f_start, f_status, f_run, f_dma, f_xbus, f_end, f_other;
} l_stat;
static int l_stats_log;             /* ARMLET_RDP_STATS=1: the counters go to the log every 256 graphics tasks */

static inline void run_pending(void)
{
    uint32_t s = *l_start_reg, c = *l_cur_reg, e = *l_end_reg;

    *l_cur_reg = l_ps;
    *l_end_reg = l_pe;
    l_ps = l_pe = 0;
    l_stat.flushes++;
    l_real();                       /* the video plugin ends with START = CURRENT = END = the end it was given */
    *l_start_reg = s;
    *l_cur_reg = c;
    *l_end_reg = e;
}

static void defer_cb(void)
{
    uint32_t cur, end;

    l_stat.calls++;
    if (!l_task_on) {               /* not a graphics task of a known family: untouched */
        l_stat.direct++;
        l_real();
        return;
    }
    cur = *l_cur_reg & ~7u;
    end = *l_end_reg & ~7u;
    if (end <= cur)                 /* foir ignores such a list and leaves the registers alone */
        return;
    if ((*l_status_reg & DPC_STATUS_XBUS) && !MUTATED(4)) {     /* the list is in DMEM, which the ucode overwrites: run it now, after what is pending */
        if (l_ps != l_pe) {
            l_stat.f_xbus++;
            run_pending();
        }
        l_stat.direct++;
        l_real();
        return;
    }
    if (l_ps == l_pe)
        l_ps = cur;
    else if (cur != l_pe) {         /* cannot happen (DPC_CURRENT follows the end of the last list); safe anyway */
        l_stat.f_other++;
        run_pending();
        l_ps = cur;
    }
    l_pe = end;
    if (!MUTATED(5))
        *l_start_reg = *l_cur_reg = *l_end_reg;     /* what foir leaves behind */
    l_stat.deferred++;
    if (l_pe - l_ps >= l_thresh) {
        l_stat.f_thresh++;
        run_pending();
    }
}

/* the hooks of the fork's prsp_rdp_defer.h */
void prsp_rdp_defer_flush(int why)
{
    if (MUTATED(1) && why == FLUSH_RUN_END)
        return;
    if (MUTATED(3) && why != FLUSH_RUN_END)
        return;
    if (l_ps != l_pe) {
        if (why == FLUSH_START)
            l_stat.f_start++;
        else if (why == FLUSH_STATUS)
            l_stat.f_status++;
        else
            l_stat.f_run++;
        run_pending();
    }
}

void prsp_rdp_defer_dma_write(uint32_t dram, uint32_t length, uint32_t skip, unsigned count)
{
    uint32_t d, r, lo = l_ps & RDRAM_MASK, hi = l_pe & RDRAM_MASK;

    if (l_ps == l_pe || MUTATED(2))
        return;
    /* the rows the DMA writes, as the fork walks them (dest & 0x7ffffc, advancing by length + skip); a row that wraps in RDRAM counts as an overlap */
    d = dram;
    for (r = 0; r <= count; r++, d += length + skip) {
        uint32_t a = d & RDRAM_MASK & ~3u;
        if (hi <= lo || a + length > RDRAM_MASK + 1 || (a < hi && a + length > lo)) {
            l_stat.f_dma++;
            run_pending();
            return;
        }
    }
}

/* auto: the argument above is about foir's ProcessRDPList, so it is on only when that is the module behind the core's callback */
static int video_is_foir(void (*process_rdp_list)(void))
{
    Dl_info di;

    return process_rdp_list && dladdr((void *)process_rdp_list, &di) && di.dli_fname && strstr(di.dli_fname, "foir");
}

void armlet_rdp_defer_init(struct armlet_ctx *c)
{
    l_ps = l_pe = 0;
    l_task_on = 0;
    l_real = NULL;
    c->cfg.rdp_coalesce = c->prsp.ProcessRdpList &&
        (c->cfg.rdp_coalesce_mode == ARMLET_COALESCE_ON ||
         (c->cfg.rdp_coalesce_mode == ARMLET_COALESCE_AUTO && video_is_foir(c->rsp.ProcessRdpList)));
    armlet_log(M64MSG_INFO, "RDP list coalescing %s (RdpCoalesce %s, threshold %u bytes)", c->cfg.rdp_coalesce ? "on" : "off",
               c->cfg.rdp_coalesce_mode == ARMLET_COALESCE_ON ? "on" : c->cfg.rdp_coalesce_mode == ARMLET_COALESCE_OFF ? "off" : "auto",
               c->cfg.rdp_coalesce_bytes);
    if (!c->cfg.rdp_coalesce)
        return;
    l_start_reg = c->prsp.DPC_START_REG;
    l_cur_reg = c->prsp.DPC_CURRENT_REG;
    l_end_reg = c->prsp.DPC_END_REG;
    l_status_reg = c->prsp.DPC_STATUS_REG;
    l_stats_log = getenv("ARMLET_RDP_STATS") != NULL;
    l_real = c->prsp.ProcessRdpList;
    l_thresh = c->cfg.rdp_coalesce_bytes;
    c->prsp.ProcessRdpList = defer_cb;
}

void armlet_rdp_defer_task_begin(const struct armlet_ctx *c)
{
    bool is_new;

    l_task_on = 0;
    if (!l_real)
        return;
    /* a graphics task of a known family (F3D / F3DEX / F3DEX2 and their relatives): a task at all, type 1 */
    if (armlet_dmem32(c, ARMLET_TASK_UCODE_BOOT_SIZE) > 0x1000 || armlet_dmem32(c, ARMLET_TASK_TYPE) != 1)
        return;
    l_task_on = armlet_gfx_identify(c, &is_new)->family != ARMLET_FAM_OTHER;
    l_stat.tasks_on += l_task_on;
}

void armlet_rdp_defer_task_end(void)
{
    if (l_ps != l_pe && !MUTATED(1)) {
        l_stat.f_end++;
        run_pending();
    }
    l_task_on = 0;
    if (l_stats_log && (++l_stat.tasks & 255) == 0)
        armlet_log(M64MSG_INFO, "rdp-defer: tasks %llu (deferring %llu) callbacks %llu deferred %llu direct %llu -> plugin calls %llu; flushes: threshold %llu start %llu status %llu run-end %llu dma %llu xbus %llu task-end %llu other %llu",
                   (unsigned long long)l_stat.tasks, (unsigned long long)l_stat.tasks_on, (unsigned long long)l_stat.calls, (unsigned long long)l_stat.deferred,
                   (unsigned long long)l_stat.direct, (unsigned long long)(l_stat.flushes + l_stat.direct), (unsigned long long)l_stat.f_thresh, (unsigned long long)l_stat.f_start, (unsigned long long)l_stat.f_status,
                   (unsigned long long)l_stat.f_run, (unsigned long long)l_stat.f_dma, (unsigned long long)l_stat.f_xbus, (unsigned long long)l_stat.f_end, (unsigned long long)l_stat.f_other);
}

void armlet_rdp_defer_reset(void)
{
    l_ps = l_pe = 0;
    l_task_on = 0;
}

#else /* !ARMLET_RDP_DEFER: a build without the fork hooks */

void armlet_rdp_defer_init(struct armlet_ctx *c) { c->cfg.rdp_coalesce = 0; }
void armlet_rdp_defer_task_begin(const struct armlet_ctx *c) { (void)c; }
void armlet_rdp_defer_task_end(void) {}
void armlet_rdp_defer_reset(void) {}

#endif
