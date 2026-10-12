/*
 * fxn_tri.c: f3dex_tri_setup / f3dex_tri_setup_noclip (NEON when built for aarch64) against the _scalar forms on random triangles, bit for bit: the return value, *len, and the whole
 * output buffer (it is prefilled with junk: bytes beyond *len, and everything after a non-OK return, must be left alone by both). The scalar form is the reference that was checked against the
 * real microcode (spec.f3dex.fxtri.scalar on the JIT). On other targets both are the scalar function and the test is trivial.
 *   build_fxn.sh fxn_tri host|a64|rg35 ;  run: fxn_tri [triangles (default 10000000)] [seed] [max mismatches before stopping, default 20, 0 = all] [tex]
 * Inputs are chosen to hit the traps on purpose: sx / sy at +-4090, equal and degenerate, full-range differences that saturate, huge dx with tiny dy (the slope limit), clip flags that reject / clip,
 * every geometry-mode combination (shade / texture / z x smooth x cull front / back x clipping) on the three builds, persp 0xffff and random, w <= 0 and extreme 1/w, tiny and extreme s / t (the
 * texture scale hits 1.0 and below it, the integer halves of the maximum tie), colours (equal: zero gradients), z with saturating differences, random tile bytes.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "f3dex_fx.h"

static uint64_t rs = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void)
{
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (uint32_t)(rs >> 16);
}
static int below(int n) { return (int)(rnd() % (unsigned)n); }
static int range(int lo, int hi) { return lo + below(hi - lo + 1); }
static int chance(int pct) { return below(100) < pct; }
static int16_t edge16(void)
{
    static const int16_t v[] = { 0, 1, -1, 2, -2, 0x7fff, -0x8000, 0x7ffe, -0x7fff, 0x4000, -0x4000, 0x00ff, -0x0100, 0x5555, -0x5556, 4090, -4090 };
    return v[below(sizeof v / sizeof v[0])];
}
static uint16_t fracs(void)
{
    static const uint16_t v[] = { 0, 1, 0x7fff, 0x8000, 0x8001, 0xffff, 0xfffe, 0x4000, 0xc000 };
    return chance(40) ? v[below(sizeof v / sizeof v[0])] : (uint16_t)rnd();
}

static const struct f3dex_build *builds[3] = { &f3dex_build_dex, &f3dex_build_non, &f3dex_build_dlx };

static int16_t clampi(int v, int lo, int hi) { return (int16_t)(v < lo ? lo : v > hi ? hi : v); }

/* a triangle's screen positions */
static void rand_pos(int16_t sx[3], int16_t sy[3])
{
    int mode = below(16);
    int cx = range(-4090, 4090), cy = range(-4090, 4090), sz = below(3) == 0 ? range(2, 40) : below(2) ? range(10, 400) : range(100, 3000);
    for (int i = 0; i < 3; i++) {
        switch (mode) {
        case 0: case 1: case 2: case 3: case 4:                                      /* a plausible triangle */
            sx[i] = clampi(cx + range(-sz, sz), -4090, 4090); sy[i] = clampi(cy + range(-sz, sz), -4090, 4090); break;
        case 5: sx[i] = (int16_t)range(-4090, 4090); sy[i] = (int16_t)range(-4090, 4090); break;
        case 6: sx[i] = chance(50) ? 4090 : -4090; sy[i] = chance(50) ? 4090 : -4090; break;
        case 7: sx[i] = (int16_t)range(-4090, 4090); sy[i] = (int16_t)(cy + range(-2, 2)); break;               /* huge dx, tiny dy: the slope limit */
        case 8: sx[i] = (int16_t)rnd(); sy[i] = (int16_t)rnd(); break;                                         /* full range: saturating differences */
        case 9: sx[i] = edge16(); sy[i] = edge16(); break;
        case 10: sx[i] = (int16_t)range(-3, 3); sy[i] = (int16_t)range(-3, 3); break;                           /* tiny: ties, degenerate */
        case 11: sx[i] = (int16_t)(cx + range(-sz, sz)); sy[i] = (int16_t)(chance(70) ? cy : cy + range(-sz, sz)); break;     /* horizontal edges */
        case 12: sx[i] = (int16_t)(chance(70) ? cx : cx + range(-sz, sz)); sy[i] = (int16_t)(cy + range(-sz, sz)); break;     /* vertical edges */
        case 13: sx[i] = (int16_t)(range(-1, 1) * 16000 + range(-8, 8)); sy[i] = (int16_t)(range(-1, 1) * 16000 + range(-8, 8)); break;
        case 14: sx[i] = chance(50) ? 0x7fff : (int16_t)0x8000; sy[i] = (int16_t)(chance(50) ? 0x7fff : (int16_t)0x8000); break;
        default: sx[i] = clampi(cx + range(-sz, sz), -4090, 4090); sy[i] = (int16_t)(range(0, 3) * 4 + cy / 4 * 4); break;     /* multiples of 4 (F3DLX rounded y) */
        }
    }
    if (chance(8)) { int a = below(3), b = below(3); sy[a] = sy[b]; }
    if (chance(5)) { int a = below(3), b = below(3); sx[a] = sx[b]; }
    if (chance(3)) { sx[1] = sx[0]; sy[1] = sy[0]; }
}

static void rand_vtx(struct f3dex_vtx *v, int tmode, int zmode, int cmode)
{
    memset(v, 0, sizeof *v);
    /* w (clip), perspective scaled in the texture preparation */
    switch (below(8)) {
    case 0: v->cx[3] = (int16_t)range(-3, 3); break;
    case 1: v->cx[3] = (int16_t)range(1, 40); break;
    case 2: v->cx[3] = (int16_t)rnd(); break;
    case 3: v->cx[3] = edge16(); break;
    default: v->cx[3] = (int16_t)range(1, 2000); break;
    }
    v->cf[3] = fracs();
    for (int k = 0; k < 3; k++) { v->cx[k] = (int16_t)rnd(); v->cf[k] = (uint16_t)rnd(); }
    for (int i = 0; i < 4; i++)
        v->col[i] = (uint8_t)(cmode == 0 ? rnd() : cmode == 1 ? (chance(50) ? 0 : 255) : cmode == 2 ? 0x80 : (rnd() & 3));
    switch (tmode) {
    case 0: v->s = (int16_t)rnd(); v->t = (int16_t)rnd(); break;
    case 1: v->s = (int16_t)range(-40, 40); v->t = (int16_t)range(-40, 40); break;
    case 2: v->s = edge16(); v->t = edge16(); break;
    case 3: v->s = (int16_t)range(-2000, 2000); v->t = (int16_t)range(-2000, 2000); break;
    case 4: v->s = (int16_t)range(-1, 1); v->t = (int16_t)range(-1, 1); break;
    default: v->s = (int16_t)(range(-512, 512) * 32); v->t = (int16_t)(range(-512, 512) * 32); break;
    }
    /* 1/w */
    switch (below(8)) {
    case 0: v->rwi = (int16_t)rnd(); break;
    case 1: v->rwi = edge16(); break;
    case 2: v->rwi = (int16_t)range(-2, 2); break;
    case 3: v->rwi = 0; break;
    default: v->rwi = (int16_t)range(0, 3); break;
    }
    v->rwf = fracs();
    switch (zmode) {
    case 0: v->sz = (int16_t)rnd(); break;
    case 1: v->sz = chance(50) ? 0x7fff : (int16_t)0x8000; break;
    case 2: v->sz = (int16_t)range(0, 1000); break;
    case 3: v->sz = edge16(); break;
    default: v->sz = (int16_t)range(0, 0x3ff); break;
    }
    v->szf = fracs();
    v->flags = 0;
}

static int tex_mode;            /* argv[4] == "tex": textured, drawn triangles with small s / t and 1/w (the texture-scale maximum ties on its integer halves) */

static void rand_tri(struct f3dex_vtx v[3], struct f3dex_tri_in *in, const struct f3dex_build **bp, int *clip)
{
    int16_t sx[3], sy[3];
    rand_pos(sx, sy);
    int tmode = below(7), zmode = below(5), cmode = below(5);
    if (tex_mode) {
        tmode = chance(60) ? 4 : chance(50) ? 1 : 3;
        if (chance(60)) {               /* tiny triangles: a large 1/area makes the gradients (and the texture scale) large */
            int cx = range(-100, 100), cy = range(-100, 100), r = below(3) ? 3 : 12;
            for (int i = 0; i < 3; i++) { sx[i] = (int16_t)(cx + range(-r, r)); sy[i] = (int16_t)(cy + range(-r, r)); }
        }
    }
    for (int i = 0; i < 3; i++) {
        rand_vtx(&v[i], tmode, zmode, cmode);
        v[i].sx = sx[i]; v[i].sy = sy[i];
    }
    if (chance(15)) { int a = below(3), b = below(3); v[a].col[0] = v[b].col[0]; v[a].col[1] = v[b].col[1]; v[a].col[2] = v[b].col[2]; v[a].col[3] = v[b].col[3]; }
    if (chance(10)) { for (int i = 1; i < 3; i++) { v[i].s = v[0].s; v[i].t = v[0].t; v[i].rwi = v[0].rwi; v[i].rwf = v[0].rwf; } }
    if (chance(10)) { for (int i = 1; i < 3; i++) { v[i].sz = v[0].sz; v[i].szf = v[0].szf; } }
    if (chance(10)) { for (int i = 1; i < 3; i++) { v[i].cx[3] = v[0].cx[3]; v[i].cf[3] = v[0].cf[3]; } }
    /* clip flags */
    switch (below(12)) {
    case 0: for (int i = 0; i < 3; i++) v[i].flags = (uint16_t)rnd(); break;
    case 1: { uint16_t m = (uint16_t)(1u << (4 + below(3)) | (chance(50) ? 1u << (12 + below(3)) : 0)); for (int i = 0; i < 3; i++) v[i].flags = (uint16_t)(m | (rnd() & 0x8f8f)); } break;   /* reject masks */
    case 2: v[below(3)].flags = (uint16_t)(chance(50) ? 0x4343 & rnd() : 1u << below(16)); break;                                                       /* clip */
    case 3: { uint16_t m = (uint16_t)(chance(50) ? 0x7030 : 0x7070); for (int i = 0; i < 3; i++) v[i].flags = (uint16_t)(m & (chance(70) ? 0xffff : rnd())); } break;
    default: break;
    }
    const struct f3dex_build *b = builds[below(3)];
    uint32_t g = rnd() & ~(uint32_t)(F3DEX_G_CULL_FRONT | F3DEX_G_CULL_BACK | F3DEX_G_SMOOTH | F3DEX_G_CLIPPING | 7);
    g |= (uint32_t)below(8);
    if (chance(70)) g |= F3DEX_G_SMOOTH;
    if (chance(15)) g |= F3DEX_G_CULL_BACK;
    if (chance(15)) g |= F3DEX_G_CULL_FRONT;
    if (chance(60)) g |= F3DEX_G_CLIPPING;
    if (chance(50)) g = (g & ~0x00ffffffu) | (g & 0xe0000u) | (g & 0x7) | (g & F3DEX_G_SMOOTH) | (g & (F3DEX_G_CULL_FRONT | F3DEX_G_CULL_BACK));
    in->b = b;
    in->v[0] = &v[0]; in->v[1] = &v[1]; in->v[2] = &v[2];
    in->flat = &v[chance(70) ? 0 : below(3)];
    in->geom = g;
    in->persp = chance(55) ? 0xffff : chance(30) ? (uint16_t)edge16() : (uint16_t)rnd();
    in->tile_byte = (uint8_t)(chance(30) ? 0 : rnd());
    *bp = b;
    *clip = chance(75);
    if (tex_mode) {
        in->geom = (in->geom & ~(uint32_t)(F3DEX_G_CULL_FRONT | F3DEX_G_CULL_BACK | 7)) | 2u | (chance(50) ? 1u : 0) | (chance(50) ? 4u : 0);
        for (int i = 0; i < 3; i++) {
            v[i].flags = 0;
            v[i].rwi = (int16_t)range(0, 2); v[i].cx[3] = (int16_t)range(1, 3000);
            if (chance(50)) v[i].rwf = (uint16_t)range(0, 4000);
        }
        if (chance(60)) { int16_t s = (int16_t)range(-2, 2); v[1].s = v[2].s = v[0].s = s; }
        if (chance(40)) { for (int i = 1; i < 3; i++) { v[i].rwi = v[0].rwi; v[i].cx[3] = v[0].cx[3]; v[i].cf[3] = v[0].cf[3]; if (chance(70)) v[i].rwf = v[0].rwf; } }
    }
}

#define BUF 0xd0
static const char *rcname[5] = { "OK", "REJECT", "CLIP", "CULL", "DEGEN" };

static void dump_tri(const struct f3dex_tri_in *in)
{
    printf("build dlx=%d non=%d  geom 0x%08x persp 0x%04x tile 0x%02x flat v%d\n", in->b->dlx, in->b->non, (unsigned)in->geom, in->persp, in->tile_byte, (int)(in->flat - in->v[0]));
    for (int i = 0; i < 3; i++) {
        const struct f3dex_vtx *v = in->v[i];
        printf("  v%d sx %d sy %d sz %d szf 0x%04x  s %d t %d  w %d:0x%04x  rw %d:0x%04x  col %02x%02x%02x%02x flags 0x%04x\n", i, v->sx, v->sy, v->sz, v->szf, v->s, v->t, v->cx[3], v->cf[3], v->rwi, v->rwf,
               v->col[0], v->col[1], v->col[2], v->col[3], v->flags);
    }
}

int main(int argc, char **argv)
{
    unsigned long n = argc > 1 ? strtoul(argv[1], 0, 0) : 10000000ul;
    if (argc > 2)
        rs ^= (uint64_t)strtoull(argv[2], 0, 0) * 0x2545f4914f6cdd1dull;
    long maxmis = argc > 3 ? strtol(argv[3], 0, 0) : 20;      /* stop after this many mismatches (0: count them all) */
    tex_mode = argc > 4 && !strcmp(argv[4], "tex");
    for (int i = 0; i < 8; i++) rnd();
    unsigned long cnt[5] = { 0 }, flagcnt[8] = { 0 }, nclip = 0, nsm = 0, bld[3] = { 0 }, mism = 0;
    unsigned long nstr = 0;                                     /* OK triangles with flags 7 */
    for (unsigned long it = 0; it < n; it++) {
        struct f3dex_vtx v[3];
        struct f3dex_tri_in in;
        const struct f3dex_build *b;
        int clip;
        rand_tri(v, &in, &b, &clip);
        uint8_t ca[BUF], cb[BUF];
        uint8_t fill = (uint8_t)rnd();
        for (int i = 0; i < BUF; i++) ca[i] = cb[i] = (uint8_t)(fill + i * 7);
        unsigned la = 0x1234, lb = 0x1234;
        int ra = clip ? f3dex_tri_setup(&in, ca, &la) : f3dex_tri_setup_noclip(&in, ca, &la);
        int rb = clip ? f3dex_tri_setup_scalar(&in, cb, &lb) : f3dex_tri_setup_noclip_scalar(&in, cb, &lb);
        int bad = 0;
        if (ra != rb) { printf("MISMATCH #%lu: return neon %d scalar %d\n", it, ra, rb); bad = 1; }
        else if (ra == F3DEX_TRI_OK && la != lb) { printf("MISMATCH #%lu: len neon %u scalar %u\n", it, la, lb); bad = 1; }
        else if (memcmp(ca, cb, BUF)) {
            int k = 0;
            while (ca[k] == cb[k]) k++;
            printf("MISMATCH #%lu: byte %d (0x%x): neon 0x%02x scalar 0x%02x  (len %u, flags %u, ret %d)\n", it, k, k, ca[k], cb[k], lb, (unsigned)(in.geom & 7), rb);
            bad = 1;
        }
        if (bad) {
            mism++;
            if (mism <= 3) dump_tri(&in);
            if (mism <= 3) {
                printf("  neon  :"); for (int i = 0; i < 0xb0; i++) printf("%s%02x", i % 8 == 0 ? " " : "", ca[i]); printf("\n");
                printf("  scalar:"); for (int i = 0; i < 0xb0; i++) printf("%s%02x", i % 8 == 0 ? " " : "", cb[i]); printf("\n");
            }
            if (maxmis && (long)mism >= maxmis) break;
        }
        if (rb >= 0 && rb <= 4) cnt[rb]++;
        if (rb == F3DEX_TRI_OK) { flagcnt[in.geom & 7]++; nsm += (in.geom & F3DEX_G_SMOOTH) != 0; bld[in.b->dlx ? 2 : in.b->non ? 1 : 0]++; }
        nclip += clip;
        if (rb == F3DEX_TRI_OK && (in.geom & 7) == 7) nstr++;
    }
    printf("triangles %lu (clip tests on %lu): OK %lu REJECT %lu CLIP %lu CULL %lu DEGEN %lu\n", n, nclip, cnt[0], cnt[1], cnt[2], cnt[3], cnt[4]);
    printf("drawn by flags (geom & 7):");
    for (int i = 0; i < 8; i++) printf(" %d:%lu", i, flagcnt[i]);
    printf("\ndrawn smooth %lu, by build dex %lu non %lu dlx %lu\n", nsm, bld[0], bld[1], bld[2]);
    (void)rcname;
    if (mism) { printf("FAIL: %lu mismatches\n", mism); return 1; }
    printf("PASS: neon == scalar on all %lu triangles\n", n);
    return 0;
}
