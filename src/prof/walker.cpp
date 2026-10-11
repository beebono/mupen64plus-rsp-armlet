/*
 * arMLEt - prof/walker.cpp: a read-only display-list walker (from the earlier rspprof study)
 *
 * At the start of every fresh graphics task the walker follows the display list in RDRAM from TASK_DATA_PTR with the F3D / F3DEX / F3DEX2
 * opcode tables: segment table, G_DL calls and branches with a return stack, G_ENDDL. It counts commands, vertices, triangles, matrices ...
 * G_BRANCH_Z is assumed not taken and G_CULLDL not culling, so a task that contains either is not "clean": the ucode may have run fewer
 * commands than the walk finds. Clean tasks are accumulated separately; they are what the dispatch counts are validated against.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>

#include "prof_internal.h"

namespace prof
{
static inline uint32_t rd32(const uint32_t *rdram, uint32_t a) { return rdram[(a & 0x7FFFFC) >> 2]; }

static void walk_dl(WalkTotals &w, unsigned fam, const uint32_t *rdram, uint32_t addr)
{
    uint32_t seg[16] = {};
    uint32_t stack[64];
    unsigned sp = 0;
    uint64_t n = 0;
    addr &= 0x7FFFFF;
    w.tasks++;
    for (;;) {
        if (++n > 3000000) {
            w.trunc++;
            break;
        }
        uint32_t w0 = rd32(rdram, addr), w1 = rd32(rdram, addr + 4);
        addr += 8;
        unsigned op = w0 >> 24;
        w.ops[op]++;
        w.cmds++;
        bool endd = false;
        if (fam == ARMLET_PROF_FAM_F3DEX2) {
            switch (op) {
            case 0xDF: endd = true; break;
            case 0xDE: {
                w.dl++;
                uint32_t t = (seg[(w1 >> 24) & 15] + (w1 & 0xFFFFFF)) & 0x7FFFFF;
                if (((w0 >> 16) & 0xFF) == 1)
                    w.dlbranch++;
                else if (sp < 64)
                    stack[sp++] = addr;
                if (sp > w.maxdepth)
                    w.maxdepth = sp;
                addr = t;
                break;
            }
            case 0x01: { unsigned nv = (w0 >> 12) & 0xFF; w.verts += nv; w.vtxloads++; if (nv <= 64) w.vhist[nv]++; break; }
            case 0x05: w.tris += 1; break;
            case 0x06: w.tris += 2; break;
            case 0x07: w.tris += 2; break;
            case 0x08: w.lines++; break;
            case 0x03: w.culldl++; break;
            case 0x04: w.branchz++; break;
            case 0xDA: w.mtx++; if ((w0 >> 16) & 1) w.mtxpush++; break;
            case 0xD8: w.pop++; break;
            case 0xDC: w.movemem++; break;
            case 0xDB: if (((w0 >> 16) & 0xFF) == 6) seg[((w0 & 0xFFFF) >> 2) & 15] = w1 & 0xFFFFFF; break;
            default: if (op >= 0xE4 && op != 0xF1) w.rdpcmds++; break;
            }
        } else if (fam == ARMLET_PROF_FAM_F3D || fam == ARMLET_PROF_FAM_F3DEX) {
            switch (op) {
            case 0xB8: endd = true; break;
            case 0x06: {
                w.dl++;
                uint32_t t = (seg[(w1 >> 24) & 15] + (w1 & 0xFFFFFF)) & 0x7FFFFF;
                if (((w0 >> 16) & 0xFF) == 1)
                    w.dlbranch++;
                else if (sp < 64)
                    stack[sp++] = addr;
                if (sp > w.maxdepth)
                    w.maxdepth = sp;
                addr = t;
                break;
            }
            case 0x04: {
                unsigned nv = fam == ARMLET_PROF_FAM_F3D ? ((w0 >> 20) & 0xF) + 1 : ((w0 >> 10) & 0x3F);
                w.verts += nv; w.vtxloads++; if (nv <= 64) w.vhist[nv]++;
                break;
            }
            case 0xBF: w.tris += 1; break;
            case 0xB1: if (fam == ARMLET_PROF_FAM_F3DEX) w.tris += 2; break;
            case 0xB5: w.lines++; break;
            case 0xBE: w.culldl++; break;
            case 0xB0: if (fam == ARMLET_PROF_FAM_F3DEX) w.branchz++; break;
            case 0x01: w.mtx++; if (!((w0 >> 16) & 1)) w.mtxpush++; break;   // F3D: PUSH is the inverted bit 0
            case 0xBD: w.pop++; break;
            case 0x03: w.movemem++; break;
            case 0xBC: if ((w0 & 0xFF) == 6) seg[(((w0 >> 8) & 0xFFFF) >> 2) & 15] = w1 & 0xFFFFFF; break;
            default: if (op >= 0xC0) w.rdpcmds++; break;
            }
        }
        if (endd) {
            if (!sp)
                break;
            addr = stack[--sp];
        }
    }
}

static void add(WalkTotals &a, const WalkTotals &b)
{
    a.tasks += b.tasks; a.cmds += b.cmds; a.verts += b.verts; a.vtxloads += b.vtxloads; a.tris += b.tris; a.lines += b.lines;
    a.mtx += b.mtx; a.mtxpush += b.mtxpush; a.pop += b.pop; a.dl += b.dl; a.dlbranch += b.dlbranch; a.branchz += b.branchz;
    a.culldl += b.culldl; a.movemem += b.movemem; a.trunc += b.trunc; a.rdpcmds += b.rdpcmds;
    if (b.maxdepth > a.maxdepth) a.maxdepth = b.maxdepth;
    for (int i = 0; i < 256; i++) a.ops[i] += b.ops[i];
    for (int i = 0; i < 65; i++) a.vhist[i] += b.vhist[i];
}

bool walk_task(Key &k, const armlet_prof_task *t, WalkTotals &out)
{
    memset(&out, 0, sizeof(out));
    walk_dl(out, k.family, reinterpret_cast<const uint32_t *>(orig.RDRAM), t->data_ptr);
    add(k.w, out);
    if (out.clean())
        add(k.wclean, out);
    return true;
}

static void print_one(FILE *f, const char *tag, int idx, const WalkTotals &w)
{
    fprintf(f, "%s %d tasks=%llu cmds=%llu verts=%llu vtxloads=%llu tris=%llu lines=%llu mtx=%llu mtxpush=%llu pop=%llu dl=%llu dlbranch=%llu "
               "branchz=%llu culldl=%llu movemem=%llu maxdepth=%llu trunc=%llu rdpcmds=%llu\n",
            tag, idx, (unsigned long long)w.tasks, (unsigned long long)w.cmds, (unsigned long long)w.verts, (unsigned long long)w.vtxloads,
            (unsigned long long)w.tris, (unsigned long long)w.lines, (unsigned long long)w.mtx, (unsigned long long)w.mtxpush, (unsigned long long)w.pop,
            (unsigned long long)w.dl, (unsigned long long)w.dlbranch, (unsigned long long)w.branchz, (unsigned long long)w.culldl,
            (unsigned long long)w.movemem, (unsigned long long)w.maxdepth, (unsigned long long)w.trunc, (unsigned long long)w.rdpcmds);
}

void walk_print(FILE *f, const Key &k)
{
    if (!k.w.tasks)
        return;
    print_one(f, "w", k.idx, k.w);
    print_one(f, "u", k.idx, k.wclean);   // clean tasks only
    fprintf(f, "o %d", k.idx);
    for (int j = 0; j < 256; j++)
        if (k.w.ops[j])
            fprintf(f, " %02x:%llu", j, (unsigned long long)k.w.ops[j]);
    fprintf(f, "\nq %d", k.idx);
    for (int j = 0; j < 256; j++)
        if (k.wclean.ops[j])
            fprintf(f, " %02x:%llu", j, (unsigned long long)k.wclean.ops[j]);
    fprintf(f, "\nh %d", k.idx);
    for (int j = 0; j <= 64; j++)
        if (k.w.vhist[j])
            fprintf(f, " %d:%llu", j, (unsigned long long)k.w.vhist[j]);
    fprintf(f, "\n");
}
}  // namespace prof
