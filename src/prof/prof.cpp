/*
 * arMLEt - prof/prof.cpp: the profiler body (ARMLET_PROFILE builds only). See prof.h and docs/profiling.md.
 *
 * Everything here observes: it reads RDRAM / DMEM / IMEM and times parallel-rsp, it never writes emulated state.
 * Counters are 64-bit totals per key (task type, ucode id); a snapshot of all of them is appended to <out>/stats at every mark file
 * and every few seconds, so a window is the difference of two snapshots.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unordered_map>
#include <vector>

#include "prof.h"
#include "prof_internal.h"
#include "prsp_hooks.h"

namespace prof
{
int level;
Opts opt;
Key keys[MAXK];
Key *cur;
const RSP_INFO *info;
RSP_INFO orig;
double ns_per_tick = 1.0;
char outdir[512];

// ------------------------------------------------------------------------------------------------ time
uint64_t ticks_now()
{
#if defined(__aarch64__)
    uint64_t v;
    asm volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return uint64_t(t.tv_sec) * 1000000000ull + uint64_t(t.tv_nsec);
#endif
}
static void time_init()
{
#if defined(__aarch64__)
    uint64_t f;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
    ns_per_tick = f ? 1e9 / double(f) : 1.0;
#else
    ns_per_tick = 1.0;
#endif
}
static double boottime()
{
    timespec t;
    clock_gettime(CLOCK_BOOTTIME, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

// ------------------------------------------------------------------------------------------------ files
FILE *open_out(const char *name, const char *mode)
{
    char p[640];
    snprintf(p, sizeof(p), "%s/%s", outdir, name);
    return fopen(p, mode);
}

static const char *const ctr_names[C_NC] = {
    "enters", "compiled", "t_lookup", "t_compile", "hashed", "inval", "inval_blk", "imemdma_blk", "imemdma_chg",
    "dma_rd", "dma_rd_bytes", "dma_rd_imem", "dma_wr", "dma_wr_bytes",
    "cmdend", "rdp_bytes", "rdp_cmds", "rdp_tri", "rdp_tri_sh", "rdp_tri_tx", "rdp_tri_z", "rdp_rect", "rdp_texrect", "rdp_loads",
    "rdp_settile", "rdp_sync", "rdp_xbus", "n_cb", "t_cb", "t_parse", "n_chk", "t_chk", "n_fwd", "t_fwd", "dyn"};
static_assert(sizeof(ctr_names) / sizeof(ctr_names[0]) == C_NC, "ctr_names");
// counters that are tick sums (printed in ns)
static bool ctr_is_ticks(int i)
{
    return i == C_T_LOOKUP || i == C_T_COMPILE || i == C_T_CB || i == C_T_PARSE || i == C_T_CHK || i == C_T_FWD;
}
const char *const cls_names[NCLS] = {"vmul", "vmac", "vaddsub", "vcmp", "vlogic", "vdiv", "vsar", "vload", "vstore",
                                     "salu", "sload", "sstore", "branch", "cop0", "cop2mov", "other"};

// ------------------------------------------------------------------------------------------------ keys
static FILE *keyf;
static Key *find_key(uint32_t type, uint64_t id, unsigned family, const char *name)
{
    for (int i = 0; i < MAXK; i++)
        if (keys[i].used && keys[i].type == type && keys[i].id == id)
            return &keys[i];
    for (int i = 0; i < MAXK; i++)
        if (!keys[i].used) {
            Key &k = keys[i];
            k.used = true;
            k.idx = i;
            k.type = type;
            k.id = id;
            k.family = family;
            snprintf(k.name, sizeof(k.name), "%s", name ? name : "");
            if (keyf) {
                fprintf(keyf, "K %d %u %016llx %u | %s\n", i, type, (unsigned long long)id, family, k.name);
                fflush(keyf);
            }
            return &k;
        }
    return &keys[MAXK - 1];
}

// ------------------------------------------------------------------------------------------------ snapshots
static FILE *statf, *taskf;
static double last_dump;

static void write_snapshot(const char *label)
{
    if (!statf)
        return;
    uint64_t tk = ticks_now();
    fprintf(statf, "S %s %.0f %.3f\n", label, double(tk) * ns_per_tick, boottime());
    for (int i = 0; i < MAXK; i++) {
        Key &k = keys[i];
        if (!k.used)
            continue;
        fprintf(statf, "k %d %llu %llu %llu %.0f\n", i, (unsigned long long)k.calls, (unsigned long long)k.tasks, (unsigned long long)k.resumes,
                double(k.t_total) * ns_per_tick);
        fprintf(statf, "c %d", i);
        for (int j = 0; j < C_NC; j++) {
            if (ctr_is_ticks(j))
                fprintf(statf, " %s=%.0f", ctr_names[j], double(k.c[j]) * ns_per_tick);
            else
                fprintf(statf, " %s=%llu", ctr_names[j], (unsigned long long)k.c[j]);
        }
        fprintf(statf, "\n");
        bool any = false;
        for (int j = 0; j < 64; j++)
            any |= k.rdp_ops[j] != 0;
        if (any) {
            fprintf(statf, "r %d", i);
            for (int j = 0; j < 64; j++)
                if (k.rdp_ops[j])
                    fprintf(statf, " %02x:%llu", j, (unsigned long long)k.rdp_ops[j]);
            fprintf(statf, "\n");
        }
        for (int t = 0; t < 2; t++) {
            fprintf(statf, "g %d %d", i, t);
            for (int j = 0; j < CBH_BINS; j++)
                fprintf(statf, " %llu:%.0f", (unsigned long long)k.cbh_n[t][j], double(k.cbh_t[t][j]) * ns_per_tick);
            fprintf(statf, "\n");
        }
        for (int b = 0; b < CBH_BINS; b++)
            for (int t = 0; t < 2; t++) {
                bool any2 = false;
                for (int j = 0; j < CBD_BINS; j++)
                    any2 |= k.cbh_d[b][t][j] != 0;
                if (!any2)
                    continue;
                fprintf(statf, "d %d %d %d", i, b, t);
                for (int j = 0; j < CBD_BINS; j++)
                    if (k.cbh_d[b][t][j])
                        fprintf(statf, " %d:%llu", j, (unsigned long long)k.cbh_d[b][t][j]);
                fprintf(statf, "\n");
            }
        walk_print(statf, k);
        if (level >= 3) {
            fprintf(statf, "x %d", i);
            for (int j = 0; j < NCLS; j++)
                fprintf(statf, " %s=%llu", cls_names[j], (unsigned long long)k.cls[j]);
            fprintf(statf, "\n");
        }
    }
    fflush(statf);
    if (taskf)
        fflush(taskf);
    last_dump = boottime();
    if (level >= 3)
        counts_write(label);
}

static void poll_marks()
{
    if (opt.nmarks == 0 || opt.next_mark >= opt.nmarks)
        return;
    struct stat st;
    while (opt.next_mark < opt.nmarks && !stat(opt.marks[opt.next_mark], &st)) {
        char l[32];
        snprintf(l, sizeof(l), "M%d", opt.next_mark);
        opt.next_mark++;
        write_snapshot(l);
    }
}

// ------------------------------------------------------------------------------------------------ the RDP callbacks
// The parse is timed on every 16th call only (and scaled). The call itself is timed in full when the list holds a G_RDPFULLSYNC (one or two per frame, and
// where the waits for the video plugin's workers are) and for a random 1 in cbsample of the others, scaled: a list of one triangle then costs the parse and
// a few counters, no clock read. The sampling is random, so a periodic pattern of the commands cannot alias with it.
static unsigned cb_seq;
static uint32_t cb_rng = 2463534242u;
static inline uint32_t cb_rand()
{
    cb_rng ^= cb_rng << 13;
    cb_rng ^= cb_rng >> 17;
    cb_rng ^= cb_rng << 5;
    return cb_rng;
}
static void cb_process_rdp_list()
{
    Key *k = cur;
    if (!k) {
        orig.ProcessRdpList();
        return;
    }
    RdpList l = {0, 0, false};
    if (opt.parse) {
        if ((++cb_seq & 15) == 0) {
            uint64_t a = ticks_now();
            l = rdp_parse(*k);
            k->c[C_T_PARSE] += (ticks_now() - a) * 16;
        } else {
            l = rdp_parse(*k);
        }
    }
    k->c[C_N_CB]++;
    unsigned scale = 1;
    if (opt.cbsample > 1 && !l.sync) {
        if (cb_rand() & (opt.cbsample - 1)) {
            orig.ProcessRdpList();
            return;
        }
        scale = opt.cbsample;
    }
    uint64_t b = ticks_now();
    orig.ProcessRdpList();
    uint64_t dt = (ticks_now() - b) * scale;
    k->c[C_T_CB] += dt;
    if (opt.cbhist) {
        unsigned bin = 0;
        while (bin < CBH_BINS - 1 && l.bytes > cbh_limit[bin])
            bin++;
        k->cbh_n[l.tris != 0][bin] += scale;
        k->cbh_t[l.tris != 0][bin] += dt;
        uint64_t t1 = dt / scale;
        k->cbh_d[bin][l.tris != 0][t1 ? (63 - __builtin_clzll(t1)) + 1 < CBD_BINS ? (63 - __builtin_clzll(t1)) + 1 : CBD_BINS - 1 : 0] += scale;
    }
}

static void cb_check_interrupts()
{
    Key *k = cur;
    if (!k) {
        orig.CheckInterrupts();
        return;
    }
    uint64_t a = ticks_now();
    orig.CheckInterrupts();
    k->c[C_T_CHK] += ticks_now() - a;
    k->c[C_N_CHK]++;
}

// ------------------------------------------------------------------------------------------------ parallel-rsp hooks
static uint64_t h_now() { return ticks_now(); }
static void h_lookup(uint64_t tk, uint64_t ctk, unsigned hashed)
{
    Key *k = cur;
    if (!k)
        return;
    k->c[C_ENTERS]++;
    k->c[C_T_LOOKUP] += tk;
    k->c[C_T_COMPILE] += ctk;
    k->c[C_HASHW] += hashed;
    if (ctk)
        k->c[C_COMPILED]++;
}
static void h_imem_dma(unsigned blocks, unsigned changed)
{
    Key *k = cur;
    if (!k)
        return;
    k->c[C_IMEMDMA_BLK] += blocks;
    k->c[C_IMEMDMA_CHG] += changed;
}
// SP DMA and IMEM invalidations are counted inline by the fork (prsp_hooks.h); the task adds the difference of the counters
static PrspCounters cnt_before;
static void add_counters(Key &k)
{
    const PrspCounters &c = PrspHookState::counters();
    k.c[C_DMA_RD] += c.dma_rd - cnt_before.dma_rd;
    k.c[C_DMA_RD_BYTES] += c.dma_rd_bytes - cnt_before.dma_rd_bytes;
    k.c[C_DMA_RD_IMEM] += c.dma_rd_imem - cnt_before.dma_rd_imem;
    k.c[C_DMA_WR] += c.dma_wr - cnt_before.dma_wr;
    k.c[C_DMA_WR_BYTES] += c.dma_wr_bytes - cnt_before.dma_wr_bytes;
    k.c[C_INVAL] += c.inval - cnt_before.inval;
    k.c[C_INVAL_BLK] += c.inval_blocks - cnt_before.inval_blocks;
}

// ------------------------------------------------------------------------------------------------ the task
static uint64_t t_begin, t_fwd0;
static uint64_t before[C_NC];
static uint64_t cls_before[NCLS];
static WalkTotals last_walk;
static bool last_walk_valid, last_clean;

static armlet_prof_task cur_task;
bool cur_fresh, cur_yield;

}  // namespace prof

using namespace prof;

static void make_dirs(const char *d)
{
    char p[512];
    snprintf(p, sizeof(p), "%s", d);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            mkdir(p, 0755);
            *s = '/';
        }
    mkdir(p, 0755);
}

extern "C" int armlet_prof_level(void) { return level; }

extern "C" void armlet_prof_init(const RSP_INFO *o, RSP_INFO *prsp)
{
    const char *e = getenv("ARMLET_PROF");
    level = e ? atoi(e) : 0;
    if (level <= 0)
        return;
    const char *od = getenv("ARMLET_PROF_OUT");
    if (!od || !*od) {
        fprintf(stderr, "arMLEt: ARMLET_PROF set without ARMLET_PROF_OUT, profiler off\n");
        level = 0;
        return;
    }
#ifndef PRSP_PROFILE
    if (level >= 3) {
        fprintf(stderr, "arMLEt: ARMLET_PROF=3 needs the --prof3 build (parallel-rsp with PRSP_PROFILE), using level 2\n");
        level = 2;
    }
#endif
    snprintf(outdir, sizeof(outdir), "%s", od);
    make_dirs(outdir);
    time_init();
    orig = *o;
    info = &orig;
    opt.parse = !(getenv("ARMLET_PROF_PARSE") && atoi(getenv("ARMLET_PROF_PARSE")) == 0);
    opt.walk = getenv("ARMLET_PROF_WALK") && atoi(getenv("ARMLET_PROF_WALK"));
    opt.tasklog = getenv("ARMLET_PROF_TASKLOG") && atoi(getenv("ARMLET_PROF_TASKLOG"));
    opt.callbacks = !(getenv("ARMLET_PROF_CB") && atoi(getenv("ARMLET_PROF_CB")) == 0);
    opt.cbhist = getenv("ARMLET_PROF_CBHIST") && atoi(getenv("ARMLET_PROF_CBHIST"));
    {   // default: sample the callback timing 1 in 8, unless the task log wants exact per-task times (or the parse that finds the syncs is off)
        unsigned n = getenv("ARMLET_PROF_CBSAMPLE") ? unsigned(atoi(getenv("ARMLET_PROF_CBSAMPLE"))) : 8;
        if (opt.tasklog || !opt.parse || n < 2)
            n = 1;
        while (n & (n - 1))
            n &= n - 1;                                   // a power of two
        opt.cbsample = n;
    }
    if (const char *m = getenv("ARMLET_PROF_MARKS")) {
        static char buf[2048];
        snprintf(buf, sizeof(buf), "%s", m);
        for (char *p = strtok(buf, ","); p && opt.nmarks < MAXMARKS; p = strtok(NULL, ","))
            opt.marks[opt.nmarks++] = p;
    }
    if (FILE *f = open_out("info", "w")) {
        fprintf(f, "level %d\nparse %d\nwalk %d\ntasklog %d\ncallbacks %d\ncbhist %d\ncbsample %u\nns_per_tick %.6f\n", level, opt.parse, opt.walk, opt.tasklog, opt.callbacks, opt.cbhist, opt.cbsample,
                ns_per_tick);
        for (int i = 0; i < opt.nmarks; i++)
            fprintf(f, "mark %d %s\n", i, opt.marks[i]);
        fclose(f);
    }
    keyf = open_out("keys", "w");
    statf = open_out("stats", "w");
    if (opt.tasklog) {
        taskf = open_out("tasks", "w");
        if (taskf)
            tasklog_header(taskf);
    }

    if (opt.callbacks) {
        prsp->ProcessRdpList = cb_process_rdp_list;
        prsp->CheckInterrupts = cb_check_interrupts;
    }

    PrspHooks h = {};
    h.now = h_now;
    h.lookup = h_lookup;
    h.imem_dma = h_imem_dma;
    if (level >= 2)
        map_install(h);
    if (level >= 3)
        counts_install(h);
    prsp_hooks_install(h);
}

extern "C" void armlet_prof_task_begin(const struct armlet_prof_task *t)
{
    if (!level)
        return;
    Key *k = find_key(t->type, t->id, t->family, t->name);
    cur_task = *t;
    bool fresh = (t->sp_pc & 0xfff) == 0 && !(t->flags & 1);
    cur_fresh = fresh;
    cur_yield = (t->flags & 1) != 0;
    if (fresh)
        k->tasks++;
    else if (t->flags & 1)
        k->resumes++;
    last_walk_valid = false;
    last_clean = false;
    if (opt.walk && t->type == 1 && fresh && k->family)
        last_walk_valid = walk_task(*k, t, last_walk);
    cnt_before = PrspHookState::counters();
    memcpy(before, k->c, sizeof(before));
    memcpy(cls_before, k->cls, sizeof(cls_before));
    if (level >= 3)
        counts_task_begin(*k, last_walk_valid && last_walk.clean(), last_walk_valid ? last_walk.cmds : 0);
    cur = k;
    t_begin = ticks_now();
}

extern "C" void armlet_prof_fwd_begin(void) { t_fwd0 = ticks_now(); }
extern "C" void armlet_prof_fwd_end(void)
{
    uint64_t d = ticks_now() - t_fwd0;
    if (cur) {
        cur->c[C_T_FWD] += d;
        cur->c[C_N_FWD]++;
    }
}

extern "C" void armlet_prof_task_end(void)
{
    if (!level)
        return;
    uint64_t t1 = ticks_now();
    Key *k = cur;
    cur = nullptr;
    if (!k)
        return;
    add_counters(*k);
    rdp_derive(*k);
    k->calls++;
    k->t_total += t1 - t_begin;
    k->nth_call++;
    if (taskf)
        tasklog_line(taskf, *k, t1 - t_begin, before, cls_before, last_walk_valid ? &last_walk : nullptr);
    if (level >= 3)
        counts_task_end(*k);
    k->ntask_seen++;
    if (!k->ucode_dumped && k->type == 1 && k->ntask_seen >= 3) {
        k->ucode_dumped = true;
        ucode_dump(*k, cur_task);
    }
    poll_marks();       // after the call, like the shim's RSPT_MARK: the snapshot and the shim's MARK line cover the same calls
    double now = boottime();
    if (now - last_dump > 15.0)
        write_snapshot("T");
}

extern "C" void armlet_prof_rom_closed(void)
{
    if (!level)
        return;
    write_snapshot("END");
    if (taskf)
        fflush(taskf);
}
