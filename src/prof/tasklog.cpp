/*
 * arMLEt - prof/tasklog.cpp: one line per DoRspCycles call (ARMLET_PROF_TASKLOG=1), the input of the cost-model fit
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "prof_internal.h"

namespace prof
{
void tasklog_header(FILE *f)
{
    fprintf(f, "# key nth phase fresh yield t_total_ns t_fwd_ns t_cb_ns t_parse_ns n_cb t_chk_ns enters t_lookup_ns t_compile_ns hashed inval dma_rd dma_rd_bytes "
               "dma_wr dma_wr_bytes imemdma_chg cmdend rdp_bytes rdp_cmds rdp_tri | w_cmds w_verts w_vtxloads w_tris w_mtx w_culldl w_branchz w_clean | dyn");
    for (int i = 0; i < NCLS; i++)
        fprintf(f, " %s", cls_names[i]);
    fprintf(f, "\n");
}

void tasklog_line(FILE *f, const Key &k, uint64_t t_total, const uint64_t *b, const uint64_t *cb, const WalkTotals *w)
{
    auto d = [&](int i) { return (unsigned long long)(k.c[i] - b[i]); };
    auto dn = [&](int i) { return (unsigned long long)(double(k.c[i] - b[i]) * ns_per_tick); };
    fprintf(f, "%d %llu %d %d %d %.0f %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu |", k.idx,
            (unsigned long long)k.nth_call, opt.next_mark, cur_fresh, cur_yield, double(t_total) * ns_per_tick, dn(C_T_FWD), dn(C_T_CB), dn(C_T_PARSE), d(C_N_CB),
            dn(C_T_CHK), d(C_ENTERS), dn(C_T_LOOKUP), dn(C_T_COMPILE), d(C_HASHW), d(C_INVAL), d(C_DMA_RD), d(C_DMA_RD_BYTES), d(C_DMA_WR),
            d(C_DMA_WR_BYTES), d(C_IMEMDMA_CHG), d(C_CMDEND), d(C_RDP_BYTES), d(C_RDP_CMDS), d(C_RDP_TRI));
    if (w)
        fprintf(f, " %llu %llu %llu %llu %llu %llu %llu %d |", (unsigned long long)w->cmds, (unsigned long long)w->verts, (unsigned long long)w->vtxloads,
                (unsigned long long)w->tris, (unsigned long long)w->mtx, (unsigned long long)w->culldl, (unsigned long long)w->branchz, w->clean());
    else
        fprintf(f, " - - - - - - - - |");
    fprintf(f, " %llu", d(C_DYN));
    for (int i = 0; i < NCLS; i++)
        fprintf(f, " %llu", (unsigned long long)(k.cls[i] - cb[i]));
    fprintf(f, "\n");
}
}  // namespace prof
