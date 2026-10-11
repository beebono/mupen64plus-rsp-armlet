/*
 * arMLEt - prof/counts.cpp: level 3, dynamic counts per site and control-flow edges
 *
 * Needs the --prof3 build (parallel-rsp compiled with PRSP_PROFILE: every JIT-ed instruction calls the hit hook, 4-5x slower than real
 * time, so these runs give counts, never timings). A site is a (pc, instruction word) pair, so ucode overlays loaded at the same pc
 * are separate sites. The JIT asks for the site index when it compiles an instruction and the hit hook counts it:
 *   site count per key, an edge (from site -> to site) for every executed transition (the sites are in execution order, a branch's delay
 *   slot comes between the branch and its target), and totals per instruction class.
 * <out>/sites                idx pc instr class
 * <out>/counts.<label>       C key site count   all tasks      U key site count   "clean" tasks (see walker.cpp)
 *                            E key from to count   edges, all tasks      D key from to count   edges, clean tasks
 *                            Z key site            J1, the first-level dispatch jump (found while running)
 *                            Y key ctx A B         context ctx = J1 went to site A (then, for two-level dispatch, a table-driven jump to site B; else 0)
 *                            N key ctx count       times the context was entered
 *                            T key a b count       J1 dispatches that went from context a to context b
 *                            X key ctx site count  executions of a site charged to a context (context 0: before the first dispatch of a task)
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <unordered_map>
#include <vector>

#include "prof_internal.h"

namespace prof
{
enum { NONE = 0xffffffffu };
static unsigned nsites = 1;                         // site 0 is the sink for overflow
static uint32_t s_pc[MAXSITES], s_instr[MAXSITES];
static uint8_t s_cls[MAXSITES];
static std::unordered_map<uint64_t, unsigned> site_ix;
static std::unordered_map<uint64_t, uint64_t> edges, cedges;   // key << 32 | from << 16 | to; cedges: clean tasks only
static unsigned prev = NONE;
static std::vector<uint64_t> snap;
static bool task_clean;
static unsigned sites_written = 1;
static bool overflow_warned;

// Context counting: the instructions executed between two dispatches are charged to the command just dispatched.
//  - J1, the first-level dispatch jump, is found while running: the register jump (not `jr ra`) that a clean task executed exactly as often as
//    the display-list walker counted commands in it (three tasks in a row). Until then everything is context 0.
//  - A context is (A, B): A the target J1 went to, B the target of the first table-driven jump after it (a `jr` through a register just loaded
//    by lh / lhu: the second dispatch level of F3DEX 1.x and F3D, which first split the commands by class), 0 when there was none. The analysis
//    merges the B level back where it is not a dispatch (see ucodemap.py).
//  Numbered in order of appearance (the last one, CTX - 1, takes the overflow); context 0 is "before the first J1 jump of the task".
enum { CTX = 128, CTXSITES = 4096 };
static uint8_t s_jr[MAXSITES], s_rs[MAXSITES];
struct C3 {
    uint64_t *ctx_cnt;                  // [CTX][CTXSITES]
    uint64_t *trans;                    // [CTX][CTX]: J1 dispatches that went from context a to context b
    unsigned ctx, nctx, pend_jr, pend, pend_table, j1, cand, j1_votes, ctxA, hasB;
    unsigned ctx_a[CTX], ctx_b[CTX];
    uint64_t ctx_n[CTX];                // times the context was entered
    std::unordered_map<uint64_t, unsigned> ctx_id;      // A << 32 | B -> context
};
static C3 c3[MAXK];
static uint64_t task_cmds;

static unsigned ctx_of(C3 &c, unsigned A, unsigned B)
{
    uint64_t k = uint64_t(A) << 32 | B;
    auto it = c.ctx_id.find(k);
    if (it != c.ctx_id.end())
        return it->second;
    if (c.nctx >= CTX)
        return CTX - 1;
    unsigned x = c.nctx++;
    c.ctx_id[k] = x;
    c.ctx_a[x] = A;
    c.ctx_b[x] = B;
    return x;
}

// `jr rs` is table-driven when the code just before it (up to 8 instructions back in the IMEM image, a `jal` may sit in between) loads rs with
// lh / lhu. Decided at the first execution of the site.
static uint8_t s_tbl[MAXSITES];
static bool jr_is_table(unsigned J)
{
    if (!s_tbl[J]) {
        const uint32_t *imem = reinterpret_cast<const uint32_t *>(orig.IMEM);
        s_tbl[J] = 2;
        for (unsigned back = 1; back <= 8 && back * 4 <= s_pc[J]; back++) {
            uint32_t w = imem[(s_pc[J] >> 2) - back];
            unsigned op = w >> 26;
            if ((op == 33 || op == 37) && ((w >> 16) & 31) == s_rs[J]) {
                s_tbl[J] = 1;
                break;
            }
        }
    }
    return s_tbl[J] == 1;
}

enum { VMUL, VMAC, VADDSUB, VCMP, VLOGIC, VDIV, VSAR, VLOAD, VSTORE, SALU, SLOAD, SSTORE, BRANCH, COP0, COP2MOV, OTHER };
static unsigned classify(uint32_t i)
{
    unsigned t = i >> 26;
    if ((i >> 25) == 0x25) {
        unsigned op = i & 63;
        if (op < 8) return op == 2 ? VMAC : VMUL;           // VRNDP reads the accumulator
        if (op < 16) return VMAC;
        if (op < 24) return VADDSUB;
        if (op == 29) return VSAR;
        if (op >= 32 && op < 40) return VCMP;
        if (op >= 40 && op < 48) return VLOGIC;
        if (op >= 48 && op < 55) return VDIV;
        return OTHER;
    }
    switch (t) {
    case 0: { unsigned f = i & 63; return f == 8 || f == 9 ? BRANCH : f == 13 ? OTHER : SALU; }
    case 1: case 2: case 3: case 4: case 5: case 6: case 7: return BRANCH;
    case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 15: return SALU;
    case 16: return COP0;
    case 18: return COP2MOV;
    case 32: case 33: case 35: case 36: case 37: case 39: return SLOAD;
    case 40: case 41: case 43: return SSTORE;
    case 50: return VLOAD;
    case 58: return VSTORE;
    default: return OTHER;
    }
}

static unsigned h_site(unsigned pc, uint32_t instr)
{
    uint64_t key = uint64_t(pc & 0xfff) << 32 | instr;
    auto it = site_ix.find(key);
    if (it != site_ix.end())
        return it->second;
    if (nsites >= MAXSITES) {
        if (!overflow_warned) {
            overflow_warned = true;
            fprintf(stderr, "arMLEt: profiler site table full (%d)\n", MAXSITES);
        }
        return 0;
    }
    unsigned s = nsites++;
    s_pc[s] = pc & 0xfff;
    s_instr[s] = instr;
    s_cls[s] = uint8_t(classify(instr));
    s_jr[s] = (instr >> 26) == 0 && (instr & 63) == 8 && ((instr >> 21) & 31) != 31;
    s_rs[s] = (instr >> 21) & 31;
    site_ix[key] = s;
    return s;
}

static void h_hit(unsigned s)
{
    Key *k = cur;
    if (!k || !k->site_cnt)
        return;
    C3 &c = c3[k->idx];
    if (c.pend == 2) {                  // s is the target of the register jump c.pend_jr (the delay slot came in between)
        c.pend = 0;
        if (c.j1 && c.pend_jr == c.j1) {
            c.ctxA = s;
            c.hasB = 0;
            unsigned x = ctx_of(c, s, 0);
            c.trans[size_t(c.ctx) * CTX + x]++;
            c.ctx_n[x]++;
            c.ctx = x;
        } else if (c.j1 && c.ctxA && !c.hasB && c.pend_table) {
            c.hasB = 1;
            c.ctx = ctx_of(c, c.ctxA, s);
            c.ctx_n[c.ctx]++;
        }
    } else if (c.pend == 1) {
        c.pend = 2;
    }
    if (s_jr[s] && c.pend == 0) {
        c.pend = 1;
        c.pend_jr = s;
        c.pend_table = jr_is_table(s);
    }
    k->site_cnt[s]++;
    if (c.ctx_cnt && s < CTXSITES)
        c.ctx_cnt[size_t(c.ctx) * CTXSITES + s]++;
    k->cls[s_cls[s]]++;
    k->c[C_DYN]++;
    unsigned p = prev;
    if (p != NONE) {
        uint64_t e = uint64_t(k->idx) << 32 | uint64_t(p) << 16 | s;
        edges[e]++;
        if (task_clean)
            cedges[e]++;
    }
    prev = s;
}

void counts_install(PrspHooks &h)
{
    h.site = h_site;
    h.hit = h_hit;
}

void counts_task_begin(Key &k, bool clean, uint64_t cmds)
{
    if (!k.site_cnt) {
        k.site_cnt = static_cast<uint64_t *>(calloc(MAXSITES, sizeof(uint64_t)));
        k.clean_cnt = static_cast<uint64_t *>(calloc(MAXSITES, sizeof(uint64_t)));
    }
    prev = NONE;
    task_clean = clean;
    task_cmds = cmds;
    C3 &c = c3[k.idx];
    if (!c.ctx_cnt) {
        c.ctx_cnt = static_cast<uint64_t *>(calloc(size_t(CTX) * CTXSITES, sizeof(uint64_t)));
        c.trans = static_cast<uint64_t *>(calloc(size_t(CTX) * CTX, sizeof(uint64_t)));
        c.nctx = 1;
    }
    c.ctx = 0;
    c.pend = 0;
    c.ctxA = 0;
    if (clean)
        snap.assign(k.site_cnt, k.site_cnt + nsites);
}

void counts_task_end(Key &k)
{
    if (!task_clean)
        return;
    for (unsigned i = 0; i < snap.size(); i++)
        k.clean_cnt[i] += k.site_cnt[i] - snap[i];
    // sites created during the task
    for (unsigned i = unsigned(snap.size()); i < nsites; i++)
        k.clean_cnt[i] += k.site_cnt[i];
    // J1: the register jump executed exactly once per display-list command of this (clean) task, three tasks in a row
    C3 &c = c3[k.idx];
    if (!c.j1 && task_cmds) {
        unsigned found = 0;
        for (unsigned i = 1; i < nsites && i < snap.size(); i++)
            if (s_jr[i] && k.site_cnt[i] - snap[i] == task_cmds) {
                found = i;
                break;
            }
        if (found && (c.j1_votes == 0 || c.cand == found)) {
            c.cand = found;
            if (++c.j1_votes >= 3)
                c.j1 = found;
        } else {
            c.j1_votes = 0;
            c.cand = 0;
        }
    }
}

void counts_write(const char *label)
{
    if (FILE *f = open_out("sites", "a")) {
        for (unsigned i = sites_written; i < nsites; i++)
            fprintf(f, "%u %u %08x %u\n", i, s_pc[i], s_instr[i], s_cls[i]);
        sites_written = nsites;
        fclose(f);
    }
    char name[64];
    snprintf(name, sizeof(name), "counts.%s", label);
    FILE *f = open_out(name, "w");
    if (!f)
        return;
    for (int i = 0; i < MAXK; i++) {
        Key &k = keys[i];
        if (!k.used || !k.site_cnt)
            continue;
        for (unsigned s = 1; s < nsites; s++) {
            if (k.site_cnt[s])
                fprintf(f, "C %d %u %llu\n", i, s, (unsigned long long)k.site_cnt[s]);
            if (k.clean_cnt[s])
                fprintf(f, "U %d %u %llu\n", i, s, (unsigned long long)k.clean_cnt[s]);
        }
    }
    for (auto &e : edges)
        fprintf(f, "E %u %u %u %llu\n", unsigned(e.first >> 32), unsigned((e.first >> 16) & 0xffff), unsigned(e.first & 0xffff),
                (unsigned long long)e.second);
    for (int i = 0; i < MAXK; i++) {
        C3 &c = c3[i];
        if (!c.ctx_cnt)
            continue;
        fprintf(f, "Z %d %u\n", i, c.j1);
        for (unsigned x = 1; x < c.nctx; x++)
            fprintf(f, "Y %d %u %u %u\nN %d %u %llu\n", i, x, c.ctx_a[x], c.ctx_b[x], i, x, (unsigned long long)c.ctx_n[x]);
        for (unsigned x = 0; x < c.nctx; x++)
            for (unsigned y = 0; y < c.nctx; y++)
                if (c.trans[size_t(x) * CTX + y])
                    fprintf(f, "T %d %u %u %llu\n", i, x, y, (unsigned long long)c.trans[size_t(x) * CTX + y]);
        for (unsigned x = 0; x < c.nctx; x++)
            for (unsigned t = 1; t < nsites && t < CTXSITES; t++)
                if (c.ctx_cnt[size_t(x) * CTXSITES + t])
                    fprintf(f, "X %d %u %u %llu\n", i, x, t, (unsigned long long)c.ctx_cnt[size_t(x) * CTXSITES + t]);
    }
    for (auto &e : cedges)
        fprintf(f, "D %u %u %u %llu\n", unsigned(e.first >> 32), unsigned((e.first >> 16) & 0xffff), unsigned(e.first & 0xffff),
                (unsigned long long)e.second);
    fclose(f);
}
}  // namespace prof
