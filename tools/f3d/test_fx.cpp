/*
 * arMLEt - tools/f3d/test_fx.cpp: random differential tests of the struct-level F3D functions (src/hle/f3d_fx.h) against the JIT running the real microcode.
 *   test_fx MODE [-n count] [-s seed] [-v]       MODE = vtx | mtx | relight | tri | clip     ($F3D_UCODE = path prefix of the ucode dump, F3D_BUILD = pw for the Pilotwings build)
 * Each case sets up DMEM / RDRAM at random, lets the JIT run the command (the oracle), fills the structs from the same initial DMEM through f3d_dmem.h, calls the function (the scalar form, and the
 * NEON form on aarch64 when -N is given: that build is run under qemu) and compares the bytes the microcode leaves.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include "fhtest.hpp"

FH *g_fh;
extern "C" {
uint64_t armlet_native_imem_hash(const uint32_t *imem, unsigned lo, unsigned hi) { uint64_t h = 0xcbf29ce484222325ull; for (unsigned i = lo >> 2; i < (hi >> 2) && i < 1024; i++) h = (h ^ imem[i]) * 0x100000001b3ull; return h; }
void armlet_mtc0(struct armlet_env *e, unsigned rd, uint32_t value) { g_fh->st().sr[32] = value; int m = RSP_MTC0(&g_fh->st(), rd, 32); if (m != RSP::MODE_CONTINUE) e->mode = m; }
uint32_t armlet_mfc0(struct armlet_env *e, unsigned rd, unsigned rt) { (void)e; RSP_MFC0(&g_fh->st(), 32, rd); return g_fh->st().sr[32]; }
void armlet_dma_read(struct armlet_env *e, unsigned mem, unsigned dram, uint32_t len) { armlet_mtc0(e, 0, mem); armlet_mtc0(e, 1, dram); armlet_mtc0(e, 2, len); }
void armlet_dma_write(struct armlet_env *e, unsigned mem, unsigned dram, uint32_t len) { armlet_mtc0(e, 0, mem); armlet_mtc0(e, 1, dram); armlet_mtc0(e, 3, len); }
}
extern "C" {
#include "f3d_fx.h"
#include "f3d_dmem.h"
}

static int g_neon = 0;
static const f3d_layout *g_lay = &f3d_layout_a;      // F3D_BUILD=pw: Pilotwings (overlays at 0x788, its clip flag word, reject mask 0x7030)
static unsigned g_reject = 0x7070;
static unsigned g_off = 0;                           // the build's triangle tail is this many bytes further than SM64's (PW 0x20, WR 0x54)
static unsigned g_ovd = 0;                           // the overlay window is this many bytes further than SM64's (PW 0x20, WR 0x60)
// the build's addresses and encodings (src/hle/f3d.c): command buffer, geometry DMA buffer, RDP output buffer, loop head, G_VTX and G_TRI1 handler, the pc the commands return to
static unsigned g_cmd = 0x6a0, g_geom = 0x7e0, g_outb = 0x9e0, g_loop = 0x60, g_vpc = 0x568, g_tpc = 0x20c, g_ret = 0xa8;
static int g_wr = 0;                                 // Wave Race: its vertex / triangle encodings (the index scaled by 5), no flat shading, the other triangle rules
static unsigned g_imul = 10, g_vmul = 4;             // G_TRI1 index byte = g_imul * slot, vertex pointer = 0x420 + g_vmul * byte
static uint32_t vtx_cmd(int n, int v0) { return g_wr ? 0x04000000u | ((5 * v0) << 16) | (n << 9) | (16 * n - 1) : (0x04u << 24) | ((((n - 1) << 4) | v0) << 16) | (n * 16 - 1); }
static void loop_regs(RSP::CPUState &c) { c.sr[29] = 0x110; c.sr[27] = g_cmd; c.sr[28] = g_wr ? g_cmd + 8 : 8; c.sr[23] = g_outb; }
static int mut_check = 0;

static void randomize_regs(FH &h, Rng &g)
{
    RSP::CPUState &c = h.st();
    for (int i = 0; i < 33; i++) c.sr[i] = g.u32();
    c.sr[0] = 0;
    for (int i = 0; i < 32; i++) for (int k = 0; k < 8; k++) c.cp2.regs[i].e[k] = g.u32();
    for (int f = 0; f < 3; f++) for (int k = 0; k < 16; k++) c.cp2.flags[f].e[k] = g.chance(50) ? 0xffff : 0;
    for (int k = 0; k < 8; k++) c.cp2.flags[2].e[k] = 0;
    for (int k = 0; k < 24; k++) c.cp2.acc.e[k] = g.u32();
    c.cp2.div_in = g.u32(); c.cp2.div_out = g.u32(); c.cp2.dp_flag = g.below(2);
}

static void rand_matrix(FH &h, Rng &g, unsigned base, int mode)
{
    for (int j = 0; j < 4; j++)
        for (int k = 0; k < 4; k++) {
            int16_t mi; uint16_t mf = g.u32();
            switch (mode) {
            case 0: mi = (int16_t)g.range(-4, 4); break;
            case 1: mi = (int16_t)g.range(-300, 300); break;
            case 2: mi = g.s16(); break;
            default: mi = (j == k) ? 1 : 0; break;
            }
            if (mode == 3 && g.chance(30)) mi = g.range(-8, 8);
            if (g.chance(3)) mf = g.chance(50) ? 0xffff : 0;
            h.s16(base + 8 * j + 2 * k, mi);
            h.s16(base + 0x20 + 8 * j + 2 * k, mf);
        }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
static void setup_vtx_state(FH &h, Rng &g, int lit)
{
    rand_matrix(h, g, 0x3e0, g.below(4));
    if (g.chance(15))                                                   // a tiny W column: large clip / W ratios
        for (int j = 0; j < 4; j++) { h.s16(0x3e0 + 8 * j + 6, g.chance(70) ? 0 : g.range(-2, 2)); h.s16(0x400 + 8 * j + 6, g.range(0, 300)); }
    for (int k = 0; k < 4; k++) {
        h.s16(0x320 + 2 * k, g.chance(50) ? (k < 2 ? (k ? -120 * 4 : 160 * 4) : 0x1ff) : g.s16());
        h.s16(0x328 + 2 * k, g.chance(50) ? (k < 2 ? (k ? 120 * 4 : 160 * 4) : 0x1ff) : g.s16());
    }
    h.s16(0x112, g.chance(70) ? 0xffff : g.u32());
    uint32_t gm = 0x2204 | (g.chance(30) ? 0x10000 : 0) | (lit ? 0x20000 : 0) | (g.chance(30) ? 0x40000 : 0) | (g.chance(30) ? 0x80000 : 0);
    if (g.chance(20)) gm ^= g.u32() & 0x3f3f;
    h.s32(0x114, gm);
    h.s32(0x124, g.chance(60) ? 0xffffffff : g.u32());
    h.s16(0x330, g.s16()); h.s16(0x332, g.s16()); h.s16(0x334, g.chance(80) ? 0xff : g.u32() & 0x3ff); h.s16(0x336, g.u32());
    if (g.chance(25)) h.s16(0x76, g.chance(50) ? g.range(1, 6) : g.u32());     // the clip ratio
    if (lit) {
        h.s32(0x12c, 32 * g.range(1, 6));
        for (int i = 0; i < 0x120; i++) h.s8(0x1a0 + i, g.chance(30) ? 0 : g.chance(50) ? 0x7f : g.u32());
        if (g.chance(60)) for (int i = 0; i < 4; i++) h.s16(0xa2 + 2 * i, g.chance(50) ? g.s16() : g.range(-3000, 3000));
    }
}

static int test_vtx(FH &h, Rng &g, Ucode &u, int count, int verbose)
{
    int fails = 0, ran = 0, skipped = 0;
    for (int it = 0; it < count; it++) {
        u.make_imem(h.imem, 2);
        memcpy(h.dmem, u.dmem.data(), 4096);
        RSP::CPUState &c = h.st();
        randomize_regs(h, g);
        int lit = g.chance(50);
        setup_vtx_state(h, g, lit);
        int n = g.range(1, 16), v0 = g.range(0, 16 - n);
        unsigned vb = 0x100000;
        int vmode = g.below(3);
        for (int i = 0; i < n; i++) {
            unsigned b = vb + 16 * i;
            for (int k = 0; k < 3; k++) {
                int16_t v = vmode == 0 ? (int16_t)g.range(-1000, 1000) : vmode == 1 ? (int16_t)g.range(-32768, 32767) : (int16_t)g.range(-40, 40);
                h.r16(b + 2 * k, (uint16_t)v);
            }
            h.r16(b + 6, g.u32());
            h.r16(b + 8, g.u32()); h.r16(b + 10, g.u32());
            h.r32(b + 12, g.u32());
        }
        h.s32(g_cmd, vtx_cmd(n, v0));
        h.s32(g_cmd + 4, vb);
        loop_regs(c);
        load_consts(h);
        for (int i = 0; i < 16; i++) h.s32(0x160 + 4 * i, 0);
        h.run(g_loop, g_vpc);
        Snap s0, sj;
        h.save(s0);
        if (h.run_any(g_vpc, {g_ret}) != g_ret) { printf("it=%d JIT did not return\n", it); return 3; }
        h.save(sj);
        // the struct-level run from s0's DMEM
        f3d_vctx ctx;
        if (f3d_load_vctx(&ctx, s0.dmem, g_lay)) { skipped++; continue; }
        f3d_vin in[16];
        f3d_vtx out[16];
        for (int i = 0; i < n; i++) f3d_load_vin(&in[i], s0.dmem, g_geom + 16 * i, 0);
        int rc = g_neon ? f3d_vtx_xform(&ctx, in, n, out) : f3d_vtx_xform_scalar(&ctx, in, n, out);
        if (rc) { skipped++; continue; }
        ran++;
        int bad = 0;
        for (int i = 0; i < n; i++) {
            unsigned a = 0x420 + 40 * (v0 + i);
            f3d_vtx j;
            f3d_load_vtx(&j, sj.dmem, a);
            uint8_t x[38], y[38];
            // serialise both through a scratch DMEM image
            static uint32_t tmp[1024];
            memset(tmp, 0, sizeof tmp);
            f3d_store_vtx(&out[i], tmp, 0);
            f3d_store_vtx(&j, tmp, 64);
            memcpy(x, (uint8_t *)tmp, 0); (void)x; (void)y;
            for (int o = 0; o < 38; o++) {
                if (fd8(tmp, o) != fd8(tmp, 64 + o)) {
                    // the byte offset o of vertex i differs
                    if (!bad++ && verbose) fprintf(stderr, "it=%d v%d byte +%d: jit %02x struct %02x\n", it, i, o, fd8(tmp, 64 + o), fd8(tmp, o));
                    else bad++;
                }
            }
        }
        if (bad) {
            fails++;
            printf("FAIL it=%d n=%d v0=%d lit=%d gm=%08x K=%d nslot=%u (%d bytes differ)\n", it, n, v0, lit, ctx.geom, ctx.clip_k, ctx.nslot, bad);
            if (fails > 4) break;
        }
    }
    printf("vtx: ran %d skipped %d failed %d\n", ran, skipped, fails);
    return fails != 0;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
static int test_mtx(FH &h, Rng &g, Ucode &u, int count, int verbose)
{
    int fails = 0, ran = 0;
    for (int it = 0; it < count; it++) {
        u.make_imem(h.imem, 2);
        memcpy(h.dmem, u.dmem.data(), 4096);
        RSP::CPUState &c = h.st();
        randomize_regs(h, g);
        int mode = g.below(4);
        rand_matrix(h, g, 0x360, mode);
        rand_matrix(h, g, 0x3a0, g.below(4));
        rand_matrix(h, g, 0x3e0, g.below(4));
        unsigned pp = g.below(8), ll = g.chance(80) ? 0x40 : 0x3f;
        // the matrix N in RDRAM at 0x100000
        unsigned nb = 0x100000;
        rand_matrix(h, g, g_geom, g.below(4));             // build in DMEM, copy to RDRAM
        for (int i = 0; i < 16; i++) { h.r16(nb + 2 * i, h.dm16(g_geom + 2 * i)); h.r16(nb + 0x20 + 2 * i, h.dm16(g_geom + 0x20 + 2 * i)); }
        unsigned stack = 0x200000;
        h.s32(0x130, stack); h.s32(0x134, g.chance(15) ? stack + 640 : stack); h.s32(0x15c, stack + 640);
        h.s32(g_cmd, (0x01u << 24) | (pp << 16) | ll);
        h.s32(g_cmd + 4, nb);
        loop_regs(c);
        load_consts(h);
        for (int i = 0; i < 16; i++) h.s32(0x160 + 4 * i, 0);
        Snap s0, sj;
        h.save(s0);
        if (h.run_any(g_loop, {g_ret}) != g_ret) { printf("it=%d JIT did not return\n", it); return 3; }
        h.save(sj);
        f3d_mtx N, mv, pj, mp, T;
        f3d_load_mtx(&N, s0.dmem, nb, 1 ? 0 : 0);
        (void)N;
        // N from the RDRAM copy
        f3d_load_mtx(&N, (const uint32_t *)h.rdram, nb, 1);
        f3d_load_mtx(&mv, s0.dmem, 0x360, 0);
        f3d_load_mtx(&pj, s0.dmem, 0x3a0, 0);
        auto mul = [&](const f3d_mtx *a, const f3d_mtx *b, f3d_mtx *o) { if (g_neon) f3d_mtx_mul(a, b, o); else f3d_mtx_mul_scalar(a, b, o); };
        if (pp & 1) { if (pp & 2) T = N; else mul(&N, &pj, &T); pj = T; }
        else { if (pp & 2) T = N; else mul(&N, &mv, &T); mv = T; }
        mul(&mv, &pj, &mp);
        f3d_mtx rmv, rpj, rmp;
        f3d_load_mtx(&rmv, sj.dmem, 0x360, 0);
        f3d_load_mtx(&rpj, sj.dmem, 0x3a0, 0);
        f3d_load_mtx(&rmp, sj.dmem, 0x3e0, 0);
        ran++;
        int bad = memcmp(&mv, &rmv, sizeof mv) + memcmp(&pj, &rpj, sizeof pj) + memcmp(&mp, &rmp, sizeof mp);
        if (bad) {
            fails++;
            printf("FAIL it=%d pp=%u ll=%x mode=%d\n", it, pp, ll, mode);
            if (fails > 4) break;
        }
    }
    printf("mtx: ran %d failed %d\n", ran, fails);
    return fails != 0;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
static int test_relight(FH &h, Rng &g, Ucode &u, int count, int verbose)
{
    int fails = 0, ran = 0;
    for (int it = 0; it < count; it++) {
        u.make_imem(h.imem, 2);
        memcpy(h.dmem, u.dmem.data(), 4096);
        RSP::CPUState &c = h.st();
        randomize_regs(h, g);
        int mmode = g.below(8);
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++) {
                int16_t mi; uint16_t mf = g.u32();
                switch (mmode) {
                case 0: mi = (int16_t)g.range(-1, 1); break;
                case 1: mi = (int16_t)g.range(-300, 300); break;
                case 2: mi = g.s16(); break;
                case 3: mi = (j == k) ? 1 : 0; mf = g.chance(50) ? 0xffff : 0; break;
                case 4: mi = 0; mf = g.chance(60) ? 0 : (uint16_t)g.range(0, 40); break;
                case 5: mi = g.chance(50) ? 0x7fff : (int16_t)0x8000; mf = 0xffff; break;
                case 6: mi = (j == k) ? 1 : (int16_t)g.range(-1, 1) * (g.chance(30)); mf = g.u32() & (g.chance(50) ? 0xffff : 0xf); break;
                default: mi = (j == k) ? 0 : 0; mf = (j == k) ? (uint16_t)(0x4000 + g.range(-300, 300)) : (uint16_t)g.range(-20, 20); break;
                }
                h.s16(0x360 + 8 * j + 2 * k, mi);
                h.s16(0x380 + 8 * j + 2 * k, mf);
            }
        unsigned ns = g.chance(70) ? g.range(1, 8) : g.range(0, 16);
        unsigned W = 32 * ns;
        h.s32(0x12c, 0x80000000u | W | (g.chance(30) ? (g.u32() & 0x7ffff000u) : 0));
        int dmode = g.below(5);
        for (unsigned s = 0; s <= ns; s++)
            for (int i = 0; i < 16; i++) {
                int b;
                switch (dmode) {
                case 0: b = g.u32() & 0xff; break;
                case 1: b = g.chance(50) ? 127 : g.chance(50) ? 0x80 : 0; break;
                case 2: b = (int)g.range(-3, 3) & 0xff; break;
                case 3: b = g.chance(70) ? 0 : (g.u32() & 0xff); break;
                default: b = (int)g.range(-127, 127) & 0xff; break;
                }
                h.s8(0x1b0 + 32 * s + i, b);
            }
        if (g.chance(15)) { h.s16(0x208, g.u32()); h.s16(0x20a, g.u32()); h.s16(0x20c, g.u32()); }
        c.sr[1] = h.dm32(0x12c); c.sr[29] = 0x110;
        load_consts(h);
        Snap s0, sj;
        h.save(s0);
        if (h.run_any(0x80c + g_ovd, {0x774 + g_ovd}) != 0x774 + g_ovd) { printf("it=%d JIT did not return\n", it); return 3; }
        h.save(sj);
        f3d_mtx mv;
        f3d_load_mtx(&mv, s0.dmem, 0x360, 0);
        int8_t raw[17][3], out[17][4];
        for (unsigned s = 0; s <= ns; s++)
            for (int i = 0; i < 3; i++) raw[s][i] = (int8_t)fd8(s0.dmem, 0x1b8 + 32 * s + i);
        int16_t cl[3] = { (int16_t)fd16(s0.dmem, 0x208), (int16_t)fd16(s0.dmem, 0x20a), (int16_t)fd16(s0.dmem, 0x20c) };
        if (g_neon) f3d_relight(&mv, raw, ns, cl, out); else f3d_relight_scalar(&mv, raw, ns, cl, out);
        ran++;
        int bad = 0;
        for (unsigned s = 0; s <= ns; s++)
            for (int i = 0; i < 3; i++) {
                int jb = (int8_t)fd8(sj.dmem, 0x1c0 + 32 * s + i);
                if (jb != out[s][i]) { if (!bad && verbose) fprintf(stderr, "it=%d slot %u byte %d: jit %d struct %d\n", it, s, i, jb, out[s][i]); bad++; }
            }
        if (bad) {
            fails++;
            printf("FAIL it=%d ns=%u mmode=%d dmode=%d (%d bytes)\n", it, ns, mmode, dmode, bad);
            if (fails > 4) break;
        }
    }
    printf("relight: ran %d failed %d\n", ran, fails);
    return fails != 0;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
static const unsigned RD_WIN = 0x200000;

static void randomize_vtx_state_tri(FH &h, Rng &g, int mxmode)
{
    rand_matrix(h, g, 0x3e0, mxmode);
    for (int k = 0; k < 4; k++) {
        h.s16(0x320 + 2 * k, g.chance(70) ? (k < 2 ? (k ? -120 * 4 : 160 * 4) : 0x1ff) : g.s16());
        h.s16(0x328 + 2 * k, g.chance(70) ? (k < 2 ? (k ? 120 * 4 : 160 * 4) : 0x1ff) : g.s16());
    }
    h.s16(0x112, g.chance(70) ? 0xffff : g.u32());
    h.s32(0x124, g.chance(60) ? 0xffffffff : g.u32());
    h.s16(0x330, g.s16()); h.s16(0x332, g.s16()); h.s16(0x334, g.chance(80) ? 0xff : g.u32() & 0x3ff); h.s16(0x336, g.u32());
}

// a triangle scenario: a real G_VTX fills the vertex buffer, the records are fuzzed, and a G_TRI1 command is set up; the state at the handler entry (0x20c) is in h
struct TriScn { int n; unsigned i0, i1, i2, flag; };
static int make_tri_scenario(FH &h, Rng &g, Ucode &u, TriScn &sc, int natural = 0)
{
    u.make_imem(h.imem, 1);
    memcpy(h.dmem, u.dmem.data(), 4096);
    RSP::CPUState &c = h.st();
    randomize_regs(h, g);
    randomize_vtx_state_tri(h, g, natural ? (g.chance(70) ? 1 : g.chance(50) ? 0 : 3) : g.below(4));
    if (natural && g.chance(40)) h.s16(0x76, g.range(1, 4));
    h.s32(0x114, 0x2204);
    int n = g.chance(80) ? g.range(8, 16) : g.range(3, 16);
    unsigned vb = 0x100000;
    int vmode = g.below(3);
    for (int i = 0; i < n; i++) {
        unsigned b = vb + 16 * i;
        for (int k = 0; k < 3; k++) {
            int16_t v = vmode == 0 ? (int16_t)g.range(-1000, 1000) : vmode == 1 ? (int16_t)g.range(-32768, 32767) : (int16_t)g.range(-40, 40);
            h.r16(b + 2 * k, (uint16_t)v);
        }
        h.r16(b + 6, g.u32());
        h.r16(b + 8, g.u32()); h.r16(b + 10, g.u32());
        h.r32(b + 12, g.u32());
    }
    h.s32(g_cmd, vtx_cmd(n, 0));
    h.s32(g_cmd + 4, vb);
    loop_regs(c);
    load_consts(h);
    for (int i = 0; i < 16; i++) h.s32(0x160 + 4 * i, 0);
    h.run(g_loop, g_vpc);
    if (h.run_any(g_vpc, {g_ret}) != g_ret) return 3;
    int fz = g.below(10);
    int fuzz = natural ? 0 : fz < 2 ? 0 : fz < 5 ? 1 : fz < 6 ? 2 : fz < 7 ? 3 : 4;
    const int flatattr = natural ? -1 : g.chance(20) ? (g.chance(40) ? 0 : g.chance(50) ? 2 : 200) : -1;
    const uint32_t abase_c = g.u32(); const int abase_s = g.range(-30000, 30000), abase_t = g.range(-30000, 30000);
    int tcx = g.range(-2000, 2000), tcy = g.range(-2000, 2000), tr = g.chance(50) ? 3 : g.chance(50) ? 12 : 40;
    unsigned clipkeep = natural ? 2 : g.chance(85) ? 1 : 0;
    for (int i = 0; i < (natural ? 0 : 16); i++) {
        unsigned b = 0x420 + 40 * i;
        if (fuzz == 1 || fuzz == 3) {
            int range = g.chance(50) ? 600 : g.chance(50) ? 2000 : g.chance(60) ? 8000 : 32768;
            h.s16(b + 24, (uint16_t)g.range(-range, range - 1));
            h.s16(b + 26, (uint16_t)g.range(-range, range - 1));
            if (g.chance(50)) h.s16(b + 28, g.u32());
            if (g.chance(30)) h.s16(b + 30, g.u32());
        }
        if (fuzz == 4) {
            h.s16(b + 24, (uint16_t)(tcx + g.range(-tr, tr)));
            h.s16(b + 26, (uint16_t)(tcy + g.range(-tr, tr)));
            h.s16(b + 28, g.u32()); h.s16(b + 30, g.u32());
            h.s32(b + 16, g.chance(30) ? 0xff00ff00u : g.u32());
            h.s32(b + 20, g.u32());
            h.s16(b + 6, g.range(0, 8)); h.s16(b + 14, g.u32());
            if (g.chance(50)) { h.s16(b + 32, g.u32()); h.s16(b + 34, g.u32()); }
        }
        if (fuzz == 2 || fuzz == 3) {
            h.s16(b + 6, g.chance(70) ? g.range(0, 8) : g.u32());
            h.s16(b + 14, g.u32());
            if (g.chance(50)) h.s32(b + 16, g.u32());
            h.s32(b + 20, g.u32());
        }
        if (flatattr >= 0) {
            uint32_t cc = abase_c;
            for (int k = 0; k < 4; k++) { int v = (int)((cc >> (8 * k)) & 0xff) + (flatattr ? g.range(-flatattr, flatattr) : 0); if (v < 0) v = 0; if (v > 255) v = 255; cc = (cc & ~(0xffu << (8 * k))) | ((uint32_t)v << (8 * k)); }
            h.s32(b + 16, cc);
            h.s16(b + 20, (uint16_t)(abase_s + (flatattr ? g.range(-flatattr, flatattr) : 0)));
            h.s16(b + 22, (uint16_t)(abase_t + (flatattr ? g.range(-flatattr, flatattr) : 0)));
        }
        if (clipkeep) h.s16(b + 36, g.chance(10) ? (g.u32() & 0x3030) : 0);
        else if (fuzz) h.s16(b + 36, g.u32() & (g.chance(50) ? 0x7373 : 0xffff));
        if (i >= 1 && g.chance(4)) { h.s32(b + 24, h.dm32(0x420 + 40 * (i - 1) + 24)); }
        if (i >= 1 && g.chance(5)) { h.s16(b + 26, h.dm16(0x420 + 40 * (i - 1) + 26)); }
    }
    unsigned i0 = g.below(n), i1 = g.below(n), i2 = g.below(n);
    if (g.chance(90)) while (i1 == i0) i1 = g.below(n);
    if (g.chance(90)) while (i2 == i0 || i2 == i1) i2 = g.below(n);
    if (g.chance(3)) i1 = i0;
    if (g.chance(3)) i2 = i1;
    unsigned flag = g.chance(80) ? g.below(3) : g.u32() & 0xff;
    h.s32(g_cmd, 0xBF000000u);
    h.s32(g_cmd + 4, (flag << 24) | ((i0 * g_imul & 0xff) << 16) | ((i1 * g_imul & 0xff) << 8) | (i2 * g_imul & 0xff));
    uint32_t gm = 0;
    if (g.chance(60)) gm |= 4;
    if (g.chance(50)) gm |= 2;
    if (g.chance(50)) gm |= 1;
    if (g.chance(55)) gm |= 0x200;
    if (g.chance(12)) gm |= 0x1000;
    if (g.chance(12)) gm |= 0x2000;
    if (g.chance(30)) gm |= 0x10000;
    if (g.chance(10)) gm |= g.u32() & 0xf8;
    if (g.chance(10)) gm |= g.u32() & 0x8cf8;
    h.s32(0x114, gm);
    h.s8(0x121, g.chance(30) ? 0 : g.u32());
    h.s8(0x122, g.chance(60) ? 0 : g.u32());
    h.s16(0x112, g.chance(70) ? 0xffff : g.u32());
    // a roomy FIFO that nothing waits on
    unsigned fstart = RD_WIN + 8 * g.below(1000), fend = fstart + 0x1f000;
    h.s32(0x128, fstart); h.s32(0x150, fstart); h.s32(0x154, fend);
    h.dpc[0] = fstart; h.dpc[1] = fstart; h.dpc[2] = fstart; h.dpc[3] = 0;
    loop_regs(c);
    for (int i = 1; i < 32; i++) if (g.chance(25) && i != 29 && i != 28 && i != 27 && i != 23 && i != 22) c.sr[i] = g.u32();
    loop_regs(c);
    if (g.chance(85)) for (int k = 0; k < 16; k++) c.cp2.flags[0].e[k] = 0;
    else for (int k = 0; k < 16; k++) c.cp2.flags[0].e[k] = g.chance(50) ? 0xffff : 0;
    load_consts(h);
    h.run(g_loop, g_tpc);
    memset(h.rdram + (RD_WIN >> 2), 0, 0x40000);
    h.rdp_log.clear(); h.rdp_calls = 0;
    sc = { n, i0, i1, i2, flag };
    return 0;
}

static int test_tri(FH &h, Rng &g, Ucode &u, int count, int verbose)
{
    int fails = 0, ran = 0, nclip = 0, nrej = 0, ncull = 0, nok = 0, badv = 0;
    for (int it = 0; it < count; it++) {
        TriScn sc;
        if (make_tri_scenario(h, g, u, sc)) { printf("it=%d vtx setup stopped\n", it); return 3; }
        if (getenv("F3D_TRI1")) {                       // one recorded triangle (f3d.c's F3D_TRACE_AT output): its vertex records and state replace the random ones
            if (it) break;
            FILE *tf = fopen(getenv("F3D_TRI1"), "r");
            char ln[512];
            unsigned gm = 0, pe = 0, z1 = 0, t2 = 0, fl = 0, ix[3] = { 0, 0, 0 };
            unsigned char rec[3][40];
            int nv = 0;
            while (tf && fgets(ln, sizeof ln, tf)) {
                if (!strncmp(ln, "TRIV", 4) && nv < 3) { for (int b = 0; b < 40; b++) { unsigned x; sscanf(ln + 6 + 2 * b, "%2x", &x); rec[nv][b] = x; } nv++; }
                else if (!strncmp(ln, "TRIS", 4)) sscanf(ln, "TRIS geom=%x persp=%x z121=%x t122=%x flag=%u idx=%u,%u,%u", &gm, &pe, &z1, &t2, &fl, &ix[0], &ix[1], &ix[2]);
            }
            if (tf) fclose(tf);
            if (nv != 3) { printf("F3D_TRI1: no records\n"); return 3; }
            for (int k = 0; k < 3; k++) for (int b = 0; b < 40; b++) h.s8(0x420 + g_vmul * ix[k] + b, rec[k][b]);
            h.s32(0x114, gm); h.s16(0x112, pe); h.s8(0x121, z1); h.s8(0x122, t2);
            h.s32(g_cmd + 4, (fl << 24) | (ix[0] << 16) | (ix[1] << 8) | ix[2]);
            sc.i0 = ix[0] / g_imul; sc.i1 = ix[1] / g_imul; sc.i2 = ix[2] / g_imul; sc.flag = fl;
        }
        Snap s0, sj;
        h.save(s0);
        if (h.run_any(g_tpc, {g_ret}) != g_ret) { printf("it=%d JIT stopped\n", it); h.load(s0); continue; }
        h.save(sj);
        unsigned pv[3] = { 0x420 + g_vmul * ((sc.i0 * g_imul) & 0xff), 0x420 + g_vmul * ((sc.i1 * g_imul) & 0xff), 0x420 + g_vmul * ((sc.i2 * g_imul) & 0xff) };
        bool inrange = true;
        for (int k = 0; k < 3; k++) if (pv[k] < 0x420 || pv[k] + 40 > 0x6a0) inrange = false;
        if (!inrange || sc.flag > 2) { badv++; continue; }
        f3d_vtx v[3];
        for (int k = 0; k < 3; k++) f3d_load_vtx(&v[k], s0.dmem, pv[k]);
        f3d_tri_in ti;
        for (int k = 0; k < 3; k++) ti.v[k] = &v[k];
        ti.flat = &v[sc.flag];
        ti.geom = fd32(s0.dmem, 0x114);
        ti.persp = fd16(s0.dmem, 0x112);
        ti.tile_byte = fd8(s0.dmem, 0x122);
        ti.zclamp = fd8(s0.dmem, 0x121);
        ti.reject_mask = g_reject; ti.clip_mask = 0x4343; ti.variant = g_wr ? F3D_TRIV_WR : F3D_TRIV_SM64;
        ti.vco_carry = s0.flags[0][9] != 0;
        uint8_t cmd[0xc0];
        unsigned len = 0;
        int rc = g_neon ? f3d_tri_setup(&ti, cmd, &len) : f3d_tri_setup_scalar(&ti, cmd, &len);
        ran++;
        if (rc == F3D_TRI_CLIP) { nclip++; continue; }
        int bad = 0;
        if (rc == F3D_TRI_REJECT || rc == F3D_TRI_CULL) {
            if (rc == F3D_TRI_REJECT) nrej++; else ncull++;
            if (!h.rdp_log.empty()) bad = 1000;
        } else {
            nok++;
            std::vector<uint8_t> jl = h.rdp_log;                       // host-order words
            if (getenv("F3D_TRI1")) { for (unsigned i = 0; i < len; i += 4) { uint32_t w; memcpy(&w, &jl[i], 4); printf("%3u jit %08x struct %02x%02x%02x%02x\n", i, w, cmd[i], cmd[i+1], cmd[i+2], cmd[i+3]); } }
            if (jl.size() != len) bad = 2000 + (int)jl.size();
            else for (unsigned i = 0; i < len; i += 4) {
                uint32_t w; memcpy(&w, &jl[i], 4);
                uint32_t b = (uint32_t)cmd[i] << 24 | cmd[i + 1] << 16 | cmd[i + 2] << 8 | cmd[i + 3];
                if (w != b) { if (!bad && verbose) fprintf(stderr, "it=%d byte %u: jit %08x struct %08x\n", it, i, w, b); bad++; }
            }
        }
        if (bad) {
            fails++;
            printf("FAIL it=%d rc=%d len=%u (%d)\n", it, rc, len, bad);
            if (fails > 4) break;
        }
    }
    printf("tri: ran %d (ok %d reject %d cull %d clip %d) skipped %d failed %d\n", ran, nok, nrej, ncull, nclip, badv, fails);
    return fails != 0;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
static int test_clip(FH &h, Rng &g, Ucode &u, int count, int verbose)
{
    int fails = 0, ran = 0, nfan = 0, nabort = 0, nnoclip = 0, c1set = 0;
    for (int it = 0; it < count; it++) {
        TriScn sc;
        if (make_tri_scenario(h, g, u, sc, 1)) { printf("it=%d vtx setup stopped\n", it); return 3; }
        // lighting and culling off for the clip scenarios half of the time: more triangles survive the facing test
        if (g.chance(50)) h.s32(0x114, h.dm32(0x114) & ~0x3000u);
        Snap s0;
        h.save(s0);
        unsigned pv[3] = { 0x420 + g_vmul * ((sc.i0 * g_imul) & 0xff), 0x420 + g_vmul * ((sc.i1 * g_imul) & 0xff), 0x420 + g_vmul * ((sc.i2 * g_imul) & 0xff) };
        bool inrange = true;
        for (int k = 0; k < 3; k++) if (pv[k] < 0x420 || pv[k] + 40 > 0x6a0) inrange = false;
        if (!inrange || sc.flag > 2) { continue; }
        // the decision first (the JIT run needs to know whether the clipper is entered: 0x9c4 is the delay slot of the branch into it)
        f3d_vctx ctx0;
        {
            f3d_vtx v0[3];
            for (int k = 0; k < 3; k++) f3d_load_vtx(&v0[k], s0.dmem, pv[k]);
            f3d_tri_in t0;
            for (int k = 0; k < 3; k++) t0.v[k] = &v0[k];
            t0.flat = &v0[sc.flag]; t0.geom = fd32(s0.dmem, 0x114); t0.persp = fd16(s0.dmem, 0x112); t0.tile_byte = 0; t0.zclamp = 0;
            t0.reject_mask = g_reject; t0.clip_mask = 0x4343; t0.vco_carry = 0; t0.variant = g_wr ? F3D_TRIV_WR : F3D_TRIV_SM64;
            uint8_t cmd0[0xc0]; unsigned l0 = 0;
            (void)ctx0;
            sc.n = f3d_tri_setup_scalar(&t0, cmd0, &l0) == F3D_TRI_CLIP;     // reuse the field: 1 = clip expected
        }
        int r;
        int c1_first = 0;
        if (sc.n) {
            r = h.run_any(g_tpc, {0x9c4 + g_off});               // the break lands in the delay slot of the jump into the clipper: r is the jump target (0x764)
            if (r == 0x764 + g_ovd) r = h.run_any(0x764 + g_ovd, {0x9c4 + g_off, g_ret});
            if (r == 0x9c4 + g_off) {
                c1_first = h.st().cp2.flags[0].e[9] != 0;
                r = h.run_any(0x9c4 + g_off, {g_ret});
            }
        } else
            r = h.run_any(g_tpc, {g_ret});
        if (r != g_ret) {
            printf("it=%d JIT stopped at %x (clip expected %d) pc=%x sr: at=%x v0=%x v1=%x s8=%x\n", it, r, sc.n, h.st().pc, h.st().sr[1], h.st().sr[2], h.st().sr[3], h.st().sr[30]);
            h.load(s0); auto o = h.trace(0x20c, {0x768, 0x784, 0x9c4, 0x954, 0x964, 0x980, 0x998, 0x9bc, 0x9c0, 0x760, 0x734}, 0xa8); for (auto q : o) printf(" %x", q); printf("\n"); continue; }
        Snap sj;
        h.save(sj);
        f3d_vctx ctx;
        if (f3d_load_vctx(&ctx, s0.dmem, g_lay)) { continue; }
        f3d_vtx v[3];
        for (int k = 0; k < 3; k++) f3d_load_vtx(&v[k], s0.dmem, pv[k]);
        f3d_tri_in ti;
        for (int k = 0; k < 3; k++) ti.v[k] = &v[k];
        ti.flat = &v[sc.flag];
        ti.geom = fd32(s0.dmem, 0x114);
        ti.persp = fd16(s0.dmem, 0x112);
        ti.tile_byte = fd8(s0.dmem, 0x122);
        ti.zclamp = fd8(s0.dmem, 0x121);
        ti.reject_mask = g_reject; ti.clip_mask = 0x4343; ti.variant = g_wr ? F3D_TRIV_WR : F3D_TRIV_SM64;
        ti.vco_carry = s0.flags[0][9] != 0;
        uint8_t cmd[0xc0];
        unsigned len = 0;
        int rc = f3d_tri_setup_scalar(&ti, cmd, &len);
        std::vector<uint8_t> mine;
        f3d_clip_out co;
        co.ntri = co.nnew = 0;
        if (rc == F3D_TRI_CLIP) {
            const f3d_vtx *vp[3] = { &v[0], &v[1], &v[2] };
            if (g_neon) f3d_clip_tri(&ctx, vp, &co); else f3d_clip_tri_scalar(&ctx, vp, &co);
            if (!co.ntri) nabort++;
            for (unsigned t = 0; t < co.ntri; t++) {
                f3d_tri_in tj = ti;
                for (int k = 0; k < 3; k++) tj.v[k] = co.tri[t][k];
                tj.vco_carry = t == 0 ? c1_first : 0;
                unsigned l2 = 0;
                int rc2 = f3d_tri_setup_noclip_scalar(&tj, cmd, &l2);
                if (rc2 == F3D_TRI_OK) mine.insert(mine.end(), cmd, cmd + l2);
            }
            nfan += co.ntri;
        } else {
            nnoclip++;
            if (rc == F3D_TRI_OK) mine.assign(cmd, cmd + len);
        }
        ran++;
        if (c1_first) c1set++;
        int bad = 0;
        std::vector<uint8_t> jl = h.rdp_log;
        if (jl.size() != mine.size()) bad = 5000 + (int)jl.size();
        else for (unsigned i = 0; i < mine.size(); i += 4) {
            uint32_t w; memcpy(&w, &jl[i], 4);
            uint32_t b = (uint32_t)mine[i] << 24 | mine[i + 1] << 16 | mine[i + 2] << 8 | mine[i + 3];
            if (w != b) { if (!bad && verbose) fprintf(stderr, "it=%d byte %u: jit %08x struct %08x\n", it, i, w, b); bad++; }
        }
        // the new vertex records
        for (unsigned s = 0; s < co.nnew && s < 12 && (!bad || verbose); s++) {
            static uint32_t tmp[1024];
            memset(tmp, 0, sizeof tmp);
            f3d_store_vtx(&co.nv[s], tmp, 0);
            for (int o = 0; o < 38; o++)
                if (fd8(tmp, o) != fd8(sj.dmem, 0xde0 + 40 * s + o)) { if (verbose) fprintf(stderr, "it=%d new vertex %u byte +%d: jit %02x struct %02x\n", it, s, o, fd8(sj.dmem, 0xde0 + 40 * s + o), fd8(tmp, o)); bad += 100; }
        }
        if (bad) {
            fails++;
            printf("FAIL it=%d rc=%d ntri=%u nnew=%u (%d)\n", it, rc, co.ntri, co.nnew, bad);
            if (fails > 4) break;
        }
    }
    printf("clip: ran %d (fan triangles %d, discarded %d, unclipped %d, entry carry set %d) failed %d\n", ran, nfan, nabort, nnoclip, c1set, fails);
    return fails != 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: test_fx vtx|mtx|relight|tri|clip [-n count] [-s seed] [-N] [-v]\n"); return 2; }
    const char *mode = argv[1];
    int count = 1000, verbose = 0;
    unsigned seed = 1;
    optind = 2;
    for (int c; (c = getopt(argc, argv, "n:s:vNm:")) != -1;)
        switch (c) {
        case 'n': count = atoi(optarg); break;
        case 's': seed = atoi(optarg); break;
        case 'v': verbose = 1; break;
        case 'N': g_neon = 1; break;
        case 'm': mut_check = atoi(optarg); break;
        }
    const char *up = getenv("F3D_UCODE");
    if (!up) { fprintf(stderr, "set F3D_UCODE\n"); return 2; }
    FH h;
    g_fh = &h;
    Ucode u;
    u.load(up);
    const char *bld = getenv("F3D_BUILD");           // the build's parameters (src/hle/f3d.c): the default is SM64
    if (bld && !strcmp(bld, "pw")) { g_lay = &f3d_layout_pw; g_reject = 0x7030; u.ov_base = 0x788; g_off = 0x20; g_ovd = 0x20; }
    if (bld && !strcmp(bld, "wr")) {
        g_lay = &f3d_layout_wr; g_wr = 1; g_imul = 5; g_vmul = 8; u.ov_base = 0x7c8; g_ovd = 0x60; g_off = 0x54;
        g_cmd = 0x920; g_geom = 0xa60; g_outb = 0xc60; g_vpc = 0x5c8; g_tpc = 0x278; g_ret = 0x9c;
    }
    if (getenv("F3D_REJECT")) g_reject = strtoul(getenv("F3D_REJECT"), 0, 0);   // a deliberate error: the mutation check of the reject mask
    Rng g(seed);
    if (!strcmp(mode, "vtx")) return test_vtx(h, g, u, count, verbose);
    if (!strcmp(mode, "mtx")) return test_mtx(h, g, u, count, verbose);
    if (!strcmp(mode, "relight")) return test_relight(h, g, u, count, verbose);
    if (!strcmp(mode, "tri")) return test_tri(h, g, u, count, verbose);
    if (!strcmp(mode, "clip")) return test_clip(h, g, u, count, verbose);
    fprintf(stderr, "unknown mode %s\n", mode);
    return 2;
}
