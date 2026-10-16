/*
 * test_f3dexbg.c: the lit vertices of F3DEXBG (src/hle/f3dexbg_fx.h) on random inputs: random light slots (point lights, attenuations, positions, any odd count up to 11), normals, flags, colours,
 * matrices, texture generation (plain and linear), positional or not. The vertices go through fx2_vtx_xform (the NEON form on aarch64, run under qemu-aarch64-static on a desktop) and through the
 * scalar form, and fbg_lit_pass() on each; the records must be identical. On a host without NEON both are the scalar form. A mutated kernel (-DARMLET_HLE_MUTATE=n) is not what this test is for: the
 * pass itself is scalar C (see docs/ucode-f3dex2-dialects.md, 2.4); its equivalence with the microcode is docs/testing.md ("Regenerating the microcode oracles") and the replays of docs/testing.md.
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

int main(int argc, char **argv)
{
    unsigned n = argc > 1 ? (unsigned)atoi(argv[1]) : 20000, i, fails = 0;
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
    printf("test_f3dexbg: %u cases, %u failures%s\n", n, fails,
#ifdef __aarch64__
           " (NEON vertex form against scalar)"
#else
           " (no NEON on this host: both are the scalar form)"
#endif
    );
    return fails != 0;
}
