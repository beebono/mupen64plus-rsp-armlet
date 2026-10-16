/*
 * test_fx2.c: the NEON forms of src/hle/f3dex2_fx.h against the scalar forms, bit for bit, on random inputs (aarch64 only; under qemu-aarch64-static on a desktop).
 *
 * Local test only. Build and run: tools/hle/build_test_fx2.sh [-DFX2_NEON_MUTATE=n for the mutation check]
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "f3dex2_fx.h"
#include "f3dex2_fx_int.h"

static uint64_t rs = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (uint32_t)(rs >> 16);
}
static uint16_t r16(void)
{
    static const uint16_t e[] = { 0, 1, 2, 0x7fff, 0x8000, 0x8001, 0xffff, 0xfffe, 0x4000, 0xc000, 0x00ff, 0xff00, 0x0100, 0xfffc };
    unsigned m = rnd() % 8;
    if (m == 0)
        return e[rnd() % (sizeof(e) / sizeof(e[0]))];
    if (m < 4)
        return (uint16_t)(rnd() % 256) - 128;
    return (uint16_t)rnd();
}

#ifdef __aarch64__
static unsigned fails;

static void rand_vtx(struct fx2_vtx *v)
{
    unsigned k;
    for (k = 0; k < 4; k++) {
        v->cx[k] = (int16_t)r16();
        v->cf[k] = r16();
        v->col[k] = (uint8_t)rnd();
    }
    v->s = (int16_t)r16();
    v->t = (int16_t)r16();
    v->sx = (int16_t)(rnd() % 3 ? (int)(rnd() % 1280) - 100 : (int16_t)r16());
    v->sy = (int16_t)(rnd() % 3 ? (int)(rnd() % 960) - 100 : (int16_t)r16());
    v->szi = (int16_t)(rnd() % 2 ? (int)(rnd() % 1024) : (int16_t)r16());
    v->szf = r16();
    v->rwi = (int16_t)(rnd() % 2 ? (int)(rnd() % 64) : (int16_t)r16());
    v->rwf = r16();
    v->flags = rnd() % 4 ? 0 : rnd();
}

static void test_tri(unsigned n)
{
    unsigned i, nd = 0, ndraw = 0;
    for (i = 0; i < n; i++) {
        struct fx2_vtx v[3];
        struct fx2_tri_in in;
        uint32_t c1[0xb0 / 4], c2[0xb0 / 4];
        unsigned l1 = 0, l2 = 0, k;
        int r1, r2;
        for (k = 0; k < 3; k++) {
            rand_vtx(&v[k]);
            in.v[k] = &v[k];
        }
        in.flat = rnd() % 4 == 0 ? &v[rnd() % 3] : NULL;
        in.g = (rnd() & 0x200000) | (rnd() & 5) | (rnd() % 8 == 0 ? 0 : 0);
        in.tex = rnd() % 3 ? 2 : 0;
        in.tile = (rnd() & 7) << 0;
        in.reject_mask = 0x7070;
        in.clip_mask = 0;
        in.cull = rnd() % 2 ? 0x80000000u : (uint32_t)(int32_t)-(int32_t)(rnd() % 64);
        in.rcp_k = rnd() % 2 ? 0x20 : 8;
        in.dx_k = rnd() % 2 ? 0x1000 : 0x4000;
        in.slope_lim = rnd() % 2 ? 0x100 : 0x1cc;
        in.frac_mask = rnd() % 2 ? 0xfff8 : 0xffff;
        memset(c1, 0xaa, sizeof(c1));
        memset(c2, 0xaa, sizeof(c2));
        r1 = fx2_tri_setup_scalar(&in, c1, &l1);
        r2 = fx2_tri_setup(&in, c2, &l2);
        if (r1 == FX2_TRI_DRAW)
            ndraw++;
        if (r1 != r2 || (r1 == FX2_TRI_DRAW && (l1 != l2 || memcmp(c1, c2, l1)))) {
            if (nd++ < 4) {
                unsigned q;
                printf("tri %u: scalar r=%d len=%u, neon r=%d len=%u\n", i, r1, l1, r2, l2);
                for (q = 0; q < l1 && q < l2; q++)
                    if (((uint8_t *)c1)[q] != ((uint8_t *)c2)[q]) {
                        printf("  first difference at byte %u: %02x vs %02x (g=%08x tex=%u)\n", q, ((uint8_t *)c1)[q], ((uint8_t *)c2)[q], in.g, in.tex);
                        break;
                    }
            }
        }
    }
    printf("tri: %u cases (%u drawn), %u differences\n", n, ndraw, nd);
    fails += nd;
}

static void test_vtx(unsigned n)
{
    unsigned i, nd = 0;
    for (i = 0; i < n; i++) {
        struct fx2_vctx c;
        struct fx2_vin in[34];
        struct fx2_vtx o1[34], o2[34];
        unsigned nv = 1 + rnd() % 32, k, j;
        memset(o1, 0x55, sizeof(o1));
        memset(o2, 0x55, sizeof(o2));
        for (j = 0; j < 4; j++)
            for (k = 0; k < 4; k++) {
                c.mp.i[j][k] = (int16_t)r16();
                c.mp.f[j][k] = r16();
            }
        for (k = 0; k < 4; k++) {
            c.scale[k] = (int16_t)r16();
            c.trans[k] = (int16_t)r16();
        }
        c.pn = r16();
        c.ratio = r16();
        c.sc_s = r16();
        c.sc_t = r16();
        c.c31_3 = 0x7f00;
        c.c31_7 = 0x7fff;
        for (k = 0; k < 3; k++)
            c.tg[k] = (int16_t)r16();
        c.mode = rnd() % 16;
        c.gen204 = rnd() % 2;
        if (c.mode & FX2_LIGHTING) {
            unsigned nl = 1 + rnd() % 7, g, m;
            c.L.top = nl + 1;
            for (g = 0; g < 2; g++)
                for (m = 0; m <= c.L.top; m++)
                    for (k = 0; k < 4; k++) {
                        c.L.d[g][m][k] = k < 3 ? (int16_t)((int8_t)rnd() * 256) : (int16_t)r16();     /* the pad lane is garbage: the code must not depend on it */
                        c.L.c[g][m][k] = k < 3 ? (int16_t)((rnd() & 255) << 7) : (int16_t)r16();
                    }
        }
        for (j = 0; j < nv + 1; j++) {
            in[j].x = (int16_t)(rnd() % 3 ? (int)(rnd() % 2000) - 1000 : (int16_t)r16());
            in[j].y = (int16_t)(rnd() % 3 ? (int)(rnd() % 2000) - 1000 : (int16_t)r16());
            in[j].z = (int16_t)(rnd() % 3 ? (int)(rnd() % 2000) - 1000 : (int16_t)r16());
            in[j].s = (int16_t)r16();
            in[j].t = (int16_t)r16();
            for (k = 0; k < 4; k++)
                in[j].c[k] = (uint8_t)rnd();
        }
        if (rnd() % 2) {                                /* sane matrices: most of the cases are not all saturated */
            for (j = 0; j < 4; j++)
                for (k = 0; k < 4; k++) {
                    c.mp.i[j][k] = (int16_t)((int)(rnd() % 8) - 4);
                    c.mp.f[j][k] = r16();
                }
            c.pn = 0xffff - (rnd() % 100);
        }
        fx2_vtx_xform_scalar(&c, in, nv, o1);
        fx2_vtx_xform(&c, in, nv, o2);
        if (memcmp(o1, o2, nv * sizeof(o1[0]))) {
            if (nd++ < 4) {
                for (j = 0; j < nv; j++)
                    if (memcmp(&o1[j], &o2[j], sizeof(o1[0]))) {
                        const uint16_t *a = (const uint16_t *)&o1[j], *b = (const uint16_t *)&o2[j];
                        printf("vtx case %u vertex %u/%u mode %x differs:\n", i, j, nv, c.mode);
                        for (k = 0; k < sizeof(o1[0]) / 2; k++)
                            if (a[k] != b[k])
                                printf("  halfword %u: scalar %04x neon %04x\n", k, a[k], b[k]);
                        break;
                    }
            }
        }
    }
    printf("vtx: %u cases, %u differences\n", n, nd);
    fails += nd;
}

static void test_mtx(unsigned n)
{
    unsigned i, nd = 0;
    for (i = 0; i < n; i++) {
        struct fx2_mtx a, b, r1, r2;
        unsigned j, k, small = rnd() % 2;
        for (j = 0; j < 4; j++)
            for (k = 0; k < 4; k++) {
                a.i[j][k] = small ? (int16_t)((int)(rnd() % 16) - 8) : (int16_t)r16();
                a.f[j][k] = r16();
                b.i[j][k] = small ? (int16_t)((int)(rnd() % 16) - 8) : (int16_t)r16();
                b.f[j][k] = r16();
            }
        fx2_mtx_mul_scalar(&a, &b, &r1);
        fx2_mtx_mul(&a, &b, &r2);
        if (memcmp(&r1, &r2, sizeof(r1)) && nd++ < 4)
            printf("mtx case %u differs\n", i);
    }
    printf("mtx: %u cases, %u differences\n", n, nd);
    fails += nd;
}

int main(int argc, char **argv)
{
    unsigned n = argc > 1 ? (unsigned)atoi(argv[1]) : 200000;
    test_tri(n);
    test_vtx(n / 10);
    test_mtx(n / 4);
    printf(fails ? "FAIL\n" : "ALL EQUAL\n");
    return fails != 0;
}
#else
int main(void)
{
    printf("aarch64 only\n");
    return 0;
}
#endif
