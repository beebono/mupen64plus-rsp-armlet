/*
 * fxn_vtx.c: f3dex_vtx_xform (NEON when built for aarch64) against f3dex_vtx_xform_scalar on random G_VTX calls, bit for bit: the whole output array, the return value, and that nothing outside out[0 .. n)
 * is written or in[0 .. n) read past (the arrays sit at the end of a page followed by an unmapped page). The scalar form is the reference that was checked against the real microcode
 * (spec.f3dex.light.vtx / spec.f3dex.fxvtx.scalar). On other targets both are the scalar function and the test is trivial.
 *   build_fxn.sh fxn_vtx host|a64|rg35 ;  run: fxn_vtx [vertices (default 6000000)] [seed]
 *   fxn_vtx -recip [part nparts]: the lean reciprocal of the NEON form (vn_recip32) against f3dex_recip_refine for every 32-bit input (part / nparts of the range; ~2 min on a PC for the whole)
 * Inputs are chosen to hit the traps on purpose: matrices with 0x8000 / 0x7fff halves, fractions >= 0x8000, 48-bit wraps; positions at +-32768; w <= 0, w near 0, x == +-w; clip ratios; persp 0xffff and random;
 * viewport and screen-clamp extremes; every geometry-mode combination on the three builds; light counts 0 .. 16; odd and even n; vco_carry; texture scales >= 0x8000; saturating dot products.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "f3dex_fx.h"
#define VN_RECIP_ONLY           /* only the portable reciprocal of the NEON header, for -recip */
#include "f3dex_fx_int.h"
#include "f3dex_vtx_fx_neon.h"

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
    static const int16_t v[] = { 0, 1, -1, 2, -2, 0x7fff, -0x8000, 0x7ffe, -0x7fff, 0x4000, -0x4000, 0x00ff, -0x0100, 0x5555, -0x5556 };
    return v[below(sizeof v / sizeof v[0])];
}
static uint16_t fracs(void)
{
    static const uint16_t v[] = { 0, 1, 0x7fff, 0x8000, 0x8001, 0xffff, 0xfffe, 0x4000, 0xc000 };
    return chance(40) ? v[below(sizeof v / sizeof v[0])] : (uint16_t)rnd();
}

/* matrix modes */
static void rand_mtx(struct f3dex_mtx *m)
{
    int mode = below(13);
    for (int j = 0; j < 4; j++)
        for (int k = 0; k < 4; k++) {
            int16_t mi;
            uint16_t mf = fracs();
            switch (mode) {
            case 0: mi = (int16_t)range(-4, 4); break;                                  /* game-like, small */
            case 1: mi = (int16_t)range(-300, 300); break;
            case 2: mi = (int16_t)rnd(); break;                                         /* full range: wraps */
            case 3: mi = chance(35) ? (int16_t)(j == k) : (int16_t)range(-1, 1); break;
            case 4: mi = (int16_t)range(-20000, 20000); break;
            case 5: mi = chance(50) ? 0x7fff : (int16_t)0x8000; break;                  /* saturating halves */
            case 6: mi = edge16(); break;
            case 7: mi = chance(70) ? 0 : (int16_t)range(-8, 8); if (chance(80)) mf = (uint16_t)(0x8000 | rnd()); break;   /* fractions >= 0x8000 */
            case 8: mi = (int16_t)range(-2000, 2000); break;
            case 9: mi = (int16_t)range(-40, 40); break;
            case 10: mi = (j == k && k < 3) ? 1 : 0; mf = (j == k) ? 0 : (chance(70) ? 0 : fracs()); break;
            case 11: mi = chance(20) ? 0 : (int16_t)range(-3, 3); mf = chance(50) ? 0 : fracs(); break;
            default: {      /* the edge of the 48-bit guard: every column's three rows of the same sign, sum |raw| just below 2^32 (3 x 1431655765 = 2^32 - 1) */
                int64_t raw = 1431655765ll - (chance(30) ? range(0, 70000) : range(0, 20));
                if (chance(30)) raw = 1431655765ll + range(0, 70000);
                int sg = (k & 1) ? -1 : 1;
                if (chance(10)) sg = -sg;
                raw *= sg;
                mi = (int16_t)(raw >> 16); mf = (uint16_t)raw;
                if (j == 3 && chance(60)) { mi = (int16_t)range(-2, 2); mf = (uint16_t)(0x8000 | rnd()); }
                break; }
            }
            m->i[j][k] = mi;
            m->f[j][k] = mf;
        }
    /* column 3 (w): often small / zero / the z coordinate, so that w <= 0, w near 0 and x == +-w occur */
    switch (below(10)) {
    case 0: for (int j = 0; j < 4; j++) { m->i[j][3] = 0; m->f[j][3] = 0; } break;
    case 1: for (int j = 0; j < 4; j++) { m->i[j][3] = 0; m->f[j][3] = (uint16_t)range(0, 40); } break;
    case 2: for (int j = 0; j < 3; j++) { m->i[j][3] = (int16_t)(j == 2); m->f[j][3] = 0; } m->i[3][3] = (int16_t)range(-2, 2); m->f[3][3] = fracs(); break;
    case 3: for (int j = 0; j < 3; j++) { m->i[j][3] = (int16_t)(j == 2 ? -1 : 0); m->f[j][3] = 0; } m->i[3][3] = 0; m->f[3][3] = 0; break;
    case 4: m->i[3][3] = (int16_t)range(1, 5); m->f[3][3] = fracs(); break;
    case 5: m->i[3][3] = (int16_t)range(-5, -1); m->f[3][3] = fracs(); break;
    default: break;
    }
    if (chance(30))
        for (int k = 0; k < 4; k++) { m->i[3][k] = chance(60) ? 0 : (int16_t)range(-3, 3); m->f[3][k] = chance(50) ? 0 : fracs(); }
    if (chance(10) && mode < 4)         /* x == +-w cases: the identity block with w = x */
        for (int j = 0; j < 3; j++)
            for (int k = 0; k < 4; k++) { m->i[j][k] = (int16_t)(j == k || (k == 3 && j == 0)); m->f[j][k] = 0; }
}

static const struct f3dex_build *builds[3] = { &f3dex_build_dex, &f3dex_build_non, &f3dex_build_dlx };

static void rand_ctx(struct f3dex_vctx *c, int lit)
{
    memset(c, 0, sizeof *c);
    for (unsigned i = 0; i < sizeof *c; i++)           /* junk everywhere, then the fields */
        ((uint8_t *)c)[i] = (uint8_t)rnd();
    c->b = builds[below(3)];
    rand_mtx(&c->mp);
    for (int k = 0; k < 4; k++) {
        int w = below(6);
        c->vscale[k] = w < 2 ? (int16_t)(k < 2 ? (k ? -480 : 640) : 0x1ff) : w == 2 ? edge16() : (int16_t)rnd();
        c->vtrans[k] = w < 2 ? (int16_t)(k < 2 ? (k ? 480 : 640) : 0x1ff) : w == 2 ? edge16() : (int16_t)rnd();
        c->vsign[k] = chance(90) ? (int16_t)(k == 1 ? -1 : 1) : chance(50) ? edge16() : (int16_t)rnd();
    }
    c->lim[0] = 4090; c->lim[1] = -4090; c->lim[2] = 0x7fff; c->lim[3] = 0;
    if (chance(15))
        for (int k = 0; k < 4; k++) c->lim[k] = chance(50) ? edge16() : (int16_t)rnd();
    c->persp = chance(60) ? 0xffff : chance(30) ? (uint16_t)edge16() : (uint16_t)rnd();
    c->clip_k = chance(55) ? 1 : chance(60) ? (int16_t)range(2, 6) : chance(50) ? edge16() : (int16_t)rnd();
    c->tscale[0] = chance(50) ? 0xffff : chance(30) ? (uint16_t)edge16() : (uint16_t)rnd();
    c->tscale[1] = chance(50) ? 0xffff : chance(30) ? (uint16_t)edge16() : (uint16_t)rnd();
    /* geometry mode: other bits random, ours chosen */
    uint32_t g = rnd() & ~(uint32_t)(F3DEX_G_FOG | F3DEX_G_LIGHTING | F3DEX_G_TEXGEN | F3DEX_G_TEXGEN_LINEAR | F3DEX_G_CLIPPING);
    g |= (chance(35) ? F3DEX_G_FOG : 0) | (lit ? F3DEX_G_LIGHTING : 0) | (chance(40) ? F3DEX_G_TEXGEN : 0) | (chance(40) ? F3DEX_G_TEXGEN_LINEAR : 0) | (chance(60) ? F3DEX_G_CLIPPING : 0);
    c->geom = g;
    for (int k = 0; k < 3; k++) c->fog[k] = chance(30) ? edge16() : (int16_t)rnd();
    if (chance(70)) c->fog[2] = chance(70) ? 0xff : (int16_t)(rnd() & 0x3ff);
    if (chance(40)) { c->fog[0] = (int16_t)range(-300, 300); c->fog[1] = (int16_t)range(-300, 300); }
    c->vco_carry = (uint8_t)(chance(50) ? rnd() : chance(50) ? 0xff : 0);
    c->nrec = 0;
    if (lit) {
        c->nrec = chance(10) ? (unsigned)range(0, F3DEX_MAX_REC) : chance(60) ? (unsigned)range(1, 4) : (unsigned)range(1, F3DEX_MAX_REC);
        if (chance(2)) c->nrec = F3DEX_MAX_REC + 1;     /* declined */
        int gl = chance(50);
        for (int k = 0; k < F3DEX_MAX_REC + 2; k++)
            for (int i = 0; i < 8; i++)
                c->lcol[k][i] = (uint8_t)(gl && k < 3 ? (k == 0 ? (i % 4 == 1 ? 0x80 : 0) : k == 1 ? 0 : rnd()) : chance(25) ? 0 : chance(40) ? 0xff : rnd());
        for (int k = 0; k < F3DEX_MAX_REC + 1; k++)
            for (int i = 0; i < 8; i++)
                c->ldir[k][i] = (int8_t)(chance(20) ? 0 : chance(25) ? (chance(50) ? 127 : -128) : chance(30) ? (int8_t)range(-127, 127) : (int8_t)rnd());
        for (int i = 0; i < 8; i++) c->lk[i] = (uint8_t)(chance(50) ? (i % 4 == 0 ? 0x80 : 0) : chance(30) ? 0xff : rnd());
        c->tg[0] = chance(50) ? 0x7fff : chance(30) ? edge16() : (int16_t)rnd();
        c->tg[1] = chance(50) ? 0x571d : chance(30) ? edge16() : (int16_t)rnd();
        c->tg[2] = chance(50) ? 0x3a0c : chance(30) ? edge16() : (int16_t)rnd();
        c->tgbias = chance(70) ? 0x4000 : chance(50) ? edge16() : (int16_t)rnd();
    } else {
        c->tgbias = chance(70) ? 0x4000 : (int16_t)rnd();
    }
    if (chance(5)) c->tgbias = edge16();
}

static int16_t rand_pos(int kind)
{
    switch (kind) {
    case 0: return (int16_t)range(-3, 3);
    case 1: return (int16_t)range(-200, 200);
    case 2: return (int16_t)range(-5000, 5000);
    case 3: return (int16_t)rnd();
    case 4: return chance(50) ? 0x7fff : (int16_t)0x8000;
    case 6: return (int16_t)0x8000;
    case 7: return chance(70) ? (int16_t)0x8000 : (int16_t)0x7fff;
    default: return edge16();
    }
}
static void rand_vin(struct f3dex_vin *v, int kind, int lit)
{
    v->x = rand_pos(kind); v->y = rand_pos(kind); v->z = rand_pos(kind);
    if (chance(15)) { int kk = below(8); v->x = rand_pos(kk); v->y = rand_pos(kk); v->z = rand_pos(kk); }
    v->s = chance(20) ? edge16() : (int16_t)rnd();
    v->t = chance(20) ? edge16() : (int16_t)rnd();
    for (int i = 0; i < 4; i++)
        v->c[i] = (uint8_t)(lit && i < 3 && chance(30) ? (chance(50) ? 0x7f : 0x80) : chance(10) ? 0 : rnd());
}

/* ---- comparison -------------------------------------------------------------------------------------------------------------------------------------------------------------------------- */
#define CMPF(name, a, b) do { if ((a) != (b)) { snprintf(why, wl, "%s: neon %d (0x%x) scalar %d (0x%x)", name, (int)(a), (unsigned)(a) & 0xffffu, (int)(b), (unsigned)(b) & 0xffffu); return 1; } } while (0)
static int cmp_vtx(const struct f3dex_vtx *a, const struct f3dex_vtx *b, char *why, size_t wl)
{
    char nm[32];
    for (int k = 0; k < 4; k++) { snprintf(nm, sizeof nm, "cx[%d]", k); CMPF(nm, a->cx[k], b->cx[k]); }
    for (int k = 0; k < 4; k++) { snprintf(nm, sizeof nm, "cf[%d]", k); CMPF(nm, a->cf[k], b->cf[k]); }
    for (int k = 0; k < 4; k++) { snprintf(nm, sizeof nm, "col[%d]", k); CMPF(nm, a->col[k], b->col[k]); }
    CMPF("s", a->s, b->s); CMPF("t", a->t, b->t);
    CMPF("sx", a->sx, b->sx); CMPF("sy", a->sy, b->sy); CMPF("sz", a->sz, b->sz); CMPF("szf", a->szf, b->szf);
    CMPF("rwi", a->rwi, b->rwi); CMPF("rwf", a->rwf, b->rwf); CMPF("flags", a->flags, b->flags);
    return 0;
}

static size_t gsize(size_t bytes) { return (((bytes + 4095) / 4096) + 1) * 4096 + 4096; }
static uint8_t *guarded(size_t bytes, void **base)
{
    /* a mapping whose last `bytes` bytes end at a page boundary followed by an unmapped page */
    size_t pg = 4096, total = ((bytes + pg - 1) / pg + 1) * pg;
    uint8_t *m = mmap(NULL, total + pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) { perror("mmap"); exit(2); }
    mprotect(m + total, pg, PROT_NONE);
    *base = m;
    return m + total - bytes;
}

static int recip_test(unsigned part, unsigned nparts)
{
    uint64_t lo = (1ull << 32) * part / nparts, hi = (1ull << 32) * (part + 1) / nparts, bad = 0;
    for (uint64_t u = lo; u < hi; u++) {
        int32_t W = (int32_t)(uint32_t)u;
        int32_t a = hle_pair32(f3dex_recip_refine(W)), b = vn_recip32(W);
        if (a != b && bad++ < 5)
            printf("recip mismatch: W=%d (0x%08x): reference 0x%08x lean 0x%08x\n", W, (unsigned)W, (unsigned)a, (unsigned)b);
        if ((u & 0xfffff) == 0x12345) {            /* the reference against the plain hle_recip_refine as a spot check */
            struct hle_pair p = hle_recip_refine(W);
            if (hle_pair32(p) != a && bad++ < 5)
                printf("f3dex_recip_refine != hle_recip_refine at W=%d\n", W);
        }
    }
    printf("recip part %u/%u: W in [%lld, %lld): %llu differences\n", part, nparts, (long long)(int32_t)(uint32_t)lo, (long long)(int32_t)(uint32_t)hi, (unsigned long long)bad);
    return bad != 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "-recip"))
        return recip_test(argc > 3 ? (unsigned)atoi(argv[2]) : 0, argc > 3 ? (unsigned)atoi(argv[3]) : 1);
    unsigned long long target = argc > 1 ? strtoull(argv[1], NULL, 0) : 6000000ull;
    if (argc > 2) rs ^= strtoull(argv[2], NULL, 0) * 0x2545f4914f6cdd1dull;
    for (int i = 0; i < 8; i++) rnd();
    unsigned long long verts = 0, calls = 0, bad = 0, badverts = 0, declined = 0;
    unsigned long long by_build[3] = { 0, 0, 0 }, lit_v = 0, tex_v = 0, lin_v = 0, fog_v = 0, noclip_v = 0, odd_calls = 0, nrec_calls[F3DEX_MAX_REC + 2] = { 0 };
    unsigned long long flag_nz = 0, wneg = 0;
    char why[200], first[400] = "";
    struct f3dex_vtx *ref = malloc(40 * sizeof *ref), *sent = malloc(40 * sizeof *sent);
    struct f3dex_vin *inb = malloc(40 * sizeof *inb);
    memset(sent, 0xa5, 40 * sizeof *sent);
    while (verts < target) {
        struct f3dex_vctx c;
        int lit = chance(45);
        rand_ctx(&c, lit);
        unsigned n = chance(25) ? 32 : chance(25) ? 16 : (unsigned)range(1, 32);
        if (chance(1)) n = chance(50) ? 0 : 33;
        /* inputs and outputs at the end of a guarded page */
        void *bi, *bo;
        struct f3dex_vin *in = (struct f3dex_vin *)guarded((n ? n : 1) * sizeof(struct f3dex_vin), &bi);
        struct f3dex_vtx *out = (struct f3dex_vtx *)guarded((n ? n : 1) * sizeof(struct f3dex_vtx), &bo);
        int kind = below(8);
        for (unsigned i = 0; i < n; i++)
            rand_vin(&in[i], chance(30) ? below(8) : kind, lit);
        memcpy(inb, in, n * sizeof *in);
        memset(ref, 0xa5, 40 * sizeof *ref);
        for (unsigned i = 0; i < n; i++)
            memset(&out[i], 0xa5, sizeof out[i]);
        int r0 = f3dex_vtx_xform_scalar(&c, in, n, ref);
        int r1 = f3dex_vtx_xform(&c, in, n, out);
        calls++;
        int fail = 0;
        why[0] = 0;
        if (r0 != r1) {
            fail = 1;
            snprintf(why, sizeof why, "return value: neon %d scalar %d", r1, r0);
        }
        for (unsigned i = 0; i < n && !fail; i++)
            if (cmp_vtx(&out[i], &ref[i], why, sizeof why)) {
                fail = 1;
                char t[200];
                snprintf(t, sizeof t, "vertex %u of %u: %s", i, n, why);
                strcpy(why, t);
            }
        for (unsigned i = n; i < 33 && !fail; i++)      /* nothing past n is touched (the reference writes none either) */
            if (memcmp(&ref[i], &sent[0], sizeof ref[i]) != 0) { fail = 1; snprintf(why, sizeof why, "reference wrote out[%u]", i); }
        if (memcmp(inb, in, n * sizeof *in) != 0) { fail = 1; snprintf(why, sizeof why, "input modified"); }
        if (r0) declined++;
        else {
            verts += n;
            by_build[c.b == &f3dex_build_dex ? 0 : c.b == &f3dex_build_non ? 1 : 2] += n;
            if (c.geom & F3DEX_G_LIGHTING) { lit_v += n; nrec_calls[c.nrec]++; if (c.geom & F3DEX_G_TEXGEN) tex_v += n; if ((c.geom & F3DEX_G_TEXGEN) && (c.geom & F3DEX_G_TEXGEN_LINEAR)) lin_v += n; }
            if (c.geom & F3DEX_G_FOG) fog_v += n;
            if (c.b->dlx && !(c.geom & F3DEX_G_CLIPPING)) noclip_v += n;
            if (n & 1) odd_calls++;
            for (unsigned i = 0; i < n; i++) { flag_nz += ref[i].flags != 0; wneg += (ref[i].cx[3] < 0); }
        }
        if (fail) {
            bad++;
            if (!first[0]) {
                snprintf(first, sizeof first, "call %llu: build %s%s n=%u geom=0x%x lit=%d nrec=%u clip_k=%d persp=0x%x: %s", calls,
                         c.b == &f3dex_build_dex ? "dex" : c.b == &f3dex_build_non ? "non" : "dlx", "", n, c.geom, !!(c.geom & F3DEX_G_LIGHTING), c.nrec, c.clip_k, c.persp, why);
            }
        }
        munmap(bi, gsize((n ? n : 1) * sizeof(struct f3dex_vin))); munmap(bo, gsize((n ? n : 1) * sizeof(struct f3dex_vtx)));
    }
    printf("fxn_vtx: %llu calls, %llu vertices (dex %llu, non %llu, dlx %llu; lit %llu, texgen %llu, linear %llu, fog %llu, dlx-no-clipping %llu), %llu odd-n calls, %llu declined calls (return 1)\n",
           calls, verts, by_build[0], by_build[1], by_build[2], lit_v, tex_v, lin_v, fog_v, noclip_v, odd_calls, declined);
    printf("  coverage: %llu vertices with nonzero clip flags, %llu with w < 0; calls per light-record count:", flag_nz, wneg);
    for (unsigned i = 0; i <= F3DEX_MAX_REC; i++) printf(" %llu", nrec_calls[i]);
    printf("\n");
    (void)badverts;
    if (bad) {
        printf("FAIL: %llu of %llu calls differ; first: %s\n", bad, calls, first);
        return 1;
    }
    printf("OK: neon == scalar, 0 differences\n");
    return 0;
}
