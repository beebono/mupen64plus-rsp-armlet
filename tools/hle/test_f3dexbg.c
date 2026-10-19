/*
 * test_f3dexbg.c: the lit vertices of F3DEXBG (src/hle/f3dexbg_fx.h) on random inputs: random light slots (point lights, attenuations, positions, any odd count up to 11), normals, flags, colours,
 * matrices, texture generation (plain and linear), positional or not. The vertices go through fx2_vtx_xform (the NEON form on aarch64, run under qemu-aarch64-static on a desktop) and through the
 * scalar form, and fbg_lit_pass() on each; the records must be identical. Then the lit pass itself: the NEON form (aarch64) against the scalar pass of an unmutated copy of f3dexbg_fx.c, on records made
 * directly, with edge-value generators (docs/testing.md, "F3DEXBG lit pass, NEON"). On a host without NEON both are the scalar form. A kernel mutated with -DARMLET_HLE_MUTATE=n (100 .. 114) or the NEON
 * pass with -DFBG_NEON_MUTATE=n must fail the lit-pass comparison; the equivalence with the microcode is docs/testing.md ("Regenerating the microcode oracles") and the replays of docs/testing.md.
 *
 * Local test only. Build and run: tools/hle/build_test_f3dexbg.sh
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "f3dex2_fx.h"
#include "f3dexbg_fx.h"

static uint64_t rs = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (uint32_t)(rs >> 16);
}
static int16_t r16(void)
{
    static const int16_t e[] = { 0, 1, 2, 0x7fff, -0x8000, -0x7fff, -1, -2, 0x4000, -0x4000, 0xff, -0x100 };
    unsigned m = rnd() % 8;
    if (m == 0)
        return e[rnd() % (sizeof(e) / sizeof(e[0]))];
    if (m < 4)
        return (int16_t)(rnd() % 512) - 256;
    return (int16_t)rnd();
}

/* the NEON lit pass against the scalar one, on records made directly (the pass reads the clip coordinates cx / cf and the pad bytes of the record): edge-value generators aimed at each step of the
 * NEON form (a saturating d, D2 of 0 / a power of two / just below one / at the largest, the largest K, extreme colours and directions, n . d overflowing, 1 .. 11 lights, positional or not, vertex counts
 * not a multiple of 4 and over a chunk of 16) */
static int16_t e16(void)
{
    static const int16_t e[] = { 0, 1, -1, 2, 0x7fff, -0x8000, -0x7fff, 0x4000, -0x4000, 0x3fff, 0x8000 - 0x100, 0xff, 0x100, -0x100, 0x7f80, 0x2000 };
    return e[rnd() % (sizeof(e) / sizeof(e[0]))];
}
static int16_t rcol(void)
{
    switch (rnd() % 6) {
    case 0: return 0;
    case 1: return 0x7f80;
    case 2: return (int16_t)((rnd() & 0xff) << 7);
    case 3: return e16();
    case 4: return (int16_t)rnd();
    default: return (int16_t)((rnd() & 0xff) << 7);
    }
}
static int16_t rdir(void)
{
    switch (rnd() % 5) {
    case 0: return (int16_t)(127 * 256);
    case 1: return (int16_t)(-128 * 256);
    case 2: return (int16_t)rnd();                       /* (not a byte << 8: the form is exact for any s16) */
    default: return (int16_t)((int8_t)rnd() * 256);
    }
}
/* a position near the vertex position v: the distance of a few units, a power of two, one under it, or far */
static int16_t near16(int16_t v)
{
    int32_t d;
    switch (rnd() % 8) {
    case 0: return v;
    case 1: d = 1 << (rnd() % 15); break;
    case 2: d = (1 << (rnd() % 15)) - 1; break;
    case 3: d = (int32_t)(rnd() % 9) - 4; break;
    case 4: d = (int32_t)(rnd() % 4096) - 2048; break;
    case 5: d = (int32_t)rnd() % 65536; break;
    default: return r16();
    }
    if (rnd() & 1) d = -d;
    return (int16_t)(v + d);
}

/* (the unmutated scalar pass, a copy of f3dexbg_fx.c with renamed symbols: build_test_f3dexbg.sh) */
void fbg_ref_lit_pass_scalar(const struct fx2_vctx *vc, const struct fbg_ctx *bc, const struct fbg_vin *in, unsigned n, struct fx2_vtx *out);

static unsigned lit_cases(unsigned ncases, int verbose)
{
    unsigned i, fails = 0;
    for (i = 0; i < ncases; i++) {
        struct fx2_vctx c;
        struct fbg_ctx bc;
        static struct fbg_vin bin[48];
        static struct fx2_vtx o1[48], o2[48];
        unsigned k, j, g, nv = rnd() % 8 ? 1 + rnd() % 20 : 1 + rnd() % 40, nl = 1 + rnd() % 11;
        int ident = rnd() % 3 != 0;                      /* V = the clip integer: the positions are then aimed at the vertices */

        memset(&c, 0, sizeof c);
        for (k = 0; k < 3; k++)
            c.tg[k] = r16();
        c.sc_s = (uint16_t)rnd();
        c.sc_t = (uint16_t)rnd();
        c.mode = (rnd() % 4 == 0 ? FX2_TEXGEN : 0) | (rnd() % 4 == 0 ? FX2_TEXGEN_LINEAR : 0);
        memset(&bc, 0, sizeof bc);
        bc.top = nl + 1;
        bc.positional = rnd() % 4 != 0;
        for (g = 0; g < 2; g++)
            for (j = 0; j < 4; j++) {
                if (ident) {
                    bc.khi[g][j] = 1;
                    bc.klo[g][j] = 0;
                    bc.zoff[g][j] = rnd() % 4 ? 0 : (int16_t)(r16() / 64);
                } else {
                    bc.khi[g][j] = rnd() % 3 ? 0 : r16();
                    bc.klo[g][j] = rnd() % 2 ? r16() : (int16_t)0x8000;
                    bc.zoff[g][j] = rnd() % 3 ? 0 : r16();
                }
            }
        for (k = 0; k < nv; k++)
            for (j = 0; j < 4; j++) {
                o1[k].cx[j] = rnd() % 4 ? (int16_t)((int32_t)(rnd() % 20001) - 10000) : r16();
                o1[k].cf[j] = (uint16_t)r16();
            }
        for (k = 0; k <= bc.top; k++) {
            const unsigned vi = rnd() % nv;
            static const uint8_t ea[] = { 0, 1, 0x10, 0x0f, 0x80, 0xf0, 0xff, 0x11 };
            bc.s[k].atten = rnd() % 3 ? (uint8_t)rnd() : ea[rnd() % sizeof ea];
            for (g = 0; g < 2; g++) {
                for (j = 0; j < 3; j++) {
                    bc.s[k].d[g][j] = rdir();
                    bc.s[k].col[g][j] = rcol();
                }
                for (j = 0; j < 4; j++)
                    bc.s[k].pos[g][j] = ident ? near16((int16_t)(o1[vi].cx[j] + (j == 2 ? bc.zoff[g][2] : 0))) : r16();
            }
            if (rnd() % 4 == 0) {                                      /* equal groups, as in the game */
                memcpy(bc.s[k].d[1], bc.s[k].d[0], sizeof bc.s[k].d[0]);
                memcpy(bc.s[k].col[1], bc.s[k].col[0], sizeof bc.s[k].col[0]);
                memcpy(bc.s[k].pos[1], bc.s[k].pos[0], sizeof bc.s[k].pos[0]);
            }
        }
        for (k = 0; k < nv; k++) {
            for (j = 0; j < 3; j++)
                bin[k].n[j] = rnd() % 5 == 0 ? (int8_t)(rnd() & 1 ? 127 : -128) : (int8_t)rnd();
            bin[k].flag = (int8_t)(rnd() % 4 ? rnd() & 0x7f : rnd());
            for (j = 0; j < 4; j++)
                bin[k].col[j] = rnd() % 5 == 0 ? (rnd() & 1 ? 0xff : 0) : (uint8_t)rnd();
            for (j = 0; j < 4; j++)
                o1[k].col[j] = (uint8_t)rnd();
            o1[k].s = r16();
            o1[k].t = r16();
        }
        memcpy(o2, o1, sizeof o1);
        fbg_lit_pass(&c, &bc, bin, nv, o1);
        fbg_ref_lit_pass_scalar(&c, &bc, bin, nv, o2);
        if (memcmp(o1, o2, sizeof o1)) {
            if (fails++ < 5 && verbose)
                printf("lit case %u: %u vertices, %u lights, %s: records differ\n", i, nv, nl, bc.positional ? "positional" : "plain");
        }
    }
    return fails;
}

int main(int argc, char **argv)
{
    unsigned n = argc > 1 ? (unsigned)atoi(argv[1]) : 20000, i, fails = 0, nlit = argc > 2 ? (unsigned)atoi(argv[2]) : 200000, lfails;
    if (argc > 3)
        rs ^= (uint64_t)strtoull(argv[3], NULL, 0) * 0x2545f4914f6cdd1dull;
    for (i = 0; i < n; i++) {
        struct fx2_vctx c;
        struct fbg_ctx bc;
        static struct fx2_vin in[34];
        static struct fbg_vin bin[34];
        static struct fx2_vtx o1[34], o2[34];
        unsigned k, j, g, nv = 1 + rnd() % 32, nl = 1 + 2 * (rnd() % 6);

        memset(&c, 0, sizeof c);
        for (k = 0; k < 4; k++)
            for (j = 0; j < 4; j++) {
                c.mp.i[k][j] = (int16_t)(r16() / 8);
                c.mp.f[k][j] = (uint16_t)r16();
            }
        for (k = 0; k < 4; k++) {
            c.scale[k] = r16();
            c.trans[k] = r16();
        }
        c.pn = (uint16_t)rnd();
        c.ratio = (uint16_t)rnd();
        c.sc_s = (uint16_t)rnd();
        c.sc_t = (uint16_t)rnd();
        c.c31_3 = 0x7f00;
        c.c31_7 = 0x7fff;
        for (k = 0; k < 3; k++)
            c.tg[k] = r16();
        c.mode = (rnd() % 3 ? FX2_TEXGEN : 0) | (rnd() % 4 == 0 ? FX2_TEXGEN_LINEAR : 0) | (rnd() % 4 == 0 ? FX2_FOG : 0);   /* (the standard lighting stays off: the pass does it) */
        c.gen204 = 0;
        memset(&bc, 0, sizeof bc);
        bc.top = nl + 1;
        bc.positional = rnd() % 4 != 0;
        for (k = 0; k <= bc.top; k++) {
            bc.s[k].atten = (uint8_t)(rnd() % 3 ? rnd() : 0);
            for (g = 0; g < 2; g++) {
                for (j = 0; j < 3; j++) {
                    bc.s[k].d[g][j] = (int16_t)((int8_t)rnd() * 256);
                    bc.s[k].col[g][j] = (int16_t)((rnd() & 0xff) << 7);
                }
                for (j = 0; j < 4; j++)
                    bc.s[k].pos[g][j] = r16();
            }
        }
        for (g = 0; g < 2; g++)
            for (j = 0; j < 4; j++) {
                bc.khi[g][j] = rnd() % 4 ? 0 : r16();
                bc.klo[g][j] = r16();
                bc.zoff[g][j] = rnd() % 4 ? 0 : (int16_t)(r16() / 8);
            }
        for (k = 0; k < nv + (nv & 1); k++) {
            in[k].x = r16();
            in[k].y = r16();
            in[k].z = r16();
            in[k].s = r16();
            in[k].t = r16();
            for (j = 0; j < 4; j++)
                in[k].c[j] = (uint8_t)rnd();
            for (j = 0; j < 3; j++)
                bin[k].n[j] = (int8_t)rnd();
            bin[k].flag = (int8_t)(rnd() % 4 ? rnd() & 0x7f : rnd());
            memcpy(bin[k].col, in[k].c, 4);
        }
        fx2_vtx_xform(&c, in, nv, o1);
        fx2_vtx_xform_scalar(&c, in, nv, o2);
        fbg_lit_pass(&c, &bc, bin, nv, o1);
        fbg_lit_pass(&c, &bc, bin, nv, o2);
        if (memcmp(o1, o2, nv * sizeof o1[0]) && fails++ < 5)
            printf("case %u: %u vertices, %u lights: records differ\n", i, nv, nl);
    }
    lfails = lit_cases(nlit, 1);
    printf("test_f3dexbg: lit pass %s against scalar: %u cases, %u failures\n",
#ifdef __aarch64__
           "NEON",
#else
           "(scalar, no NEON on this host)",
#endif
           nlit, lfails);
    fails += lfails;
    printf("test_f3dexbg: %u cases, %u failures%s\n", n, fails,
#ifdef __aarch64__
           " (NEON vertex form against scalar)"
#else
           " (no NEON on this host: both are the scalar form)"
#endif
    );
    return fails != 0;
}
