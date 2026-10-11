/*
 * arMLEt - prof/prof_internal.h: shared state of the profiler's translation units
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ARMLET_PROF_INTERNAL_H
#define ARMLET_PROF_INTERNAL_H

#include <stdint.h>
#include <stdio.h>

#include "prof.h"
#include "prsp_hooks.h"

namespace prof
{
enum Ctr {
    C_ENTERS,       // get_jit_block misses (blocks[] empty -> rsp_enter): analyse + hash + lookup (+ compile)
    C_COMPILED,
    C_T_LOOKUP,     // ticks inside get_jit_block on a miss (contains C_T_COMPILE)
    C_T_COMPILE,
    C_HASHW,        // words hashed
    C_INVAL,        // invalidate_code with something dirty
    C_INVAL_BLK,    // 256-byte blocks cleared
    C_IMEMDMA_BLK,  // IMEM DMA: 256-byte blocks covered
    C_IMEMDMA_CHG,  // ... whose bytes changed
    C_DMA_RD, C_DMA_RD_BYTES, C_DMA_RD_IMEM, C_DMA_WR, C_DMA_WR_BYTES,
    C_CMDEND,       // RDP list submissions with a non-empty list
    C_RDP_BYTES, C_RDP_CMDS, C_RDP_TRI, C_RDP_TRI_SH, C_RDP_TRI_TX, C_RDP_TRI_Z, C_RDP_RECT, C_RDP_TEXRECT, C_RDP_LOADS, C_RDP_SETTILE,
    C_RDP_SYNC, C_RDP_XBUS,
    C_N_CB,         // ProcessRdpList calls
    C_T_CB,         // ticks inside the core's ProcessRdpList
    C_T_PARSE,      // ticks in the profiler's own list parse (overhead, not RSP time)
    C_N_CHK, C_T_CHK,  // CheckInterrupts
    C_N_FWD, C_T_FWD,  // parallelRSPDoRspCycles calls from HleForwardTask / ticks inside (contains CB, PARSE, CHK)
    C_DYN,          // dynamic RSP instructions (level 3)
    C_NC
};
enum { NCLS = 16, MAXK = 16, MAXMARKS = 8, MAXSITES = 1 << 16, CBH_BINS = 9, CBD_BINS = 24 };
// list size bins of the callback histogram: <= 8, 16, 32, 64, 128, 256, 512, 1024 bytes, more
extern const unsigned cbh_limit[CBH_BINS - 1];
extern const char *const cls_names[NCLS];

// display-list walker totals (one task, or the sum over a key)
struct WalkTotals {
    uint64_t tasks, cmds, verts, vtxloads, tris, lines, mtx, mtxpush, pop, dl, dlbranch, branchz, culldl, movemem, maxdepth, trunc, rdpcmds;
    uint64_t ops[256];
    uint64_t vhist[65];
    bool clean() const { return culldl == 0 && branchz == 0 && trunc == 0; }
};

struct Key {
    bool used;
    int idx;
    uint32_t type;
    uint64_t id;
    unsigned family;
    char name[160];
    uint64_t calls, tasks, resumes, t_total;
    uint64_t nth_call, ntask_seen;
    bool ucode_dumped;
    uint64_t c[C_NC];
    uint64_t rdp_ops[64];
    uint64_t cbh_d[CBH_BINS][2][CBD_BINS];             // callback calls by list size, triangle flag and duration (log2 of the ticks)
    uint64_t cbh_n[2][CBH_BINS], cbh_t[2][CBH_BINS];   // callback calls and ticks by size of the list (bytes) and whether it holds a triangle
    uint64_t cls[NCLS];
    WalkTotals w;            // all walked tasks
    WalkTotals wclean;       // walked tasks without CULLDL / BRANCH_Z (their command stream is what the ucode ran)
    uint64_t *site_cnt;      // level 3: [MAXSITES]
    uint64_t *clean_cnt;     // level 3: the same over the clean tasks
};

struct Opts {
    bool parse, walk, tasklog, callbacks, cbhist;
    unsigned cbsample;      // 1: time every ProcessRdpList call; N (a power of two): time the calls with a full sync and a random 1 in N of the others, scaled by N
    int nmarks, next_mark;
    const char *marks[MAXMARKS];
};

extern int level;
extern Opts opt;
extern Key keys[MAXK];
extern Key *cur;
extern RSP_INFO orig;
extern double ns_per_tick;
extern char outdir[512];
extern bool cur_fresh, cur_yield;   // the current call is a fresh task start / a resume after a yield

uint64_t ticks_now();
FILE *open_out(const char *name, const char *mode);

// rdp.cpp: counts the commands of the list [DPC_CURRENT, DPC_END) the ucode is submitting
struct RdpList { unsigned bytes; unsigned tris; bool sync; };   // sync: the list holds a G_RDPFULLSYNC (the call that waits for the plugin's workers)
RdpList rdp_parse(Key &k);
void rdp_derive(Key &k);
// walker.cpp
bool walk_task(Key &k, const armlet_prof_task *t, WalkTotals &out);
void walk_print(FILE *f, const Key &k);
// tasklog.cpp
void tasklog_header(FILE *f);
void tasklog_line(FILE *f, const Key &k, uint64_t t_total_ticks, const uint64_t *before, const uint64_t *cls_before, const WalkTotals *w);
// map.cpp (level 2) and counts.cpp (level 3): install their hooks into `h`
void map_install(PrspHooks &h);
void counts_install(PrspHooks &h);
void counts_task_begin(Key &k, bool clean, uint64_t cmds);
void counts_task_end(Key &k);
void counts_write(const char *label);
// ucode.cpp
void ucode_dump(Key &k, const armlet_prof_task &t);
}  // namespace prof

#endif
