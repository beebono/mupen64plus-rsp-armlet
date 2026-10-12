/*
 * bench_fx2.c: times the scalar and NEON forms of src/hle/f3dex2_fx.h on plausible inputs (aarch64 device or qemu-less desktop for the scalar forms)
 *
 * Local test only. Build: tools/hle/build_test_fx2.sh builds test_fx2; this one: see build_bench_fx2.sh
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "f3dex2_fx.h"
#include "f3dex2_fx_int.h"
#ifndef __aarch64__
unsigned fx2_tri_attrs_neon(const struct fx2_tri_in *in, const struct fx2_tri_state *st, uint32_t *cmd) { return fx2_tri_attrs_scalar(in, st, cmd); }
#endif

static uint64_t rs = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (uint32_t)(rs >> 16);
}
static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

static void plausible_ctx(struct fx2_vctx *c, unsigned mode)
{
    unsigned j, k, m, g;
    memset(c, 0, sizeof(*c));
    for (j = 0; j < 4; j++)
        for (k = 0; k < 4; k++) {
            c->mp.i[j][k] = (int16_t)(j == k ? 1 : 0);
            c->mp.f[j][k] = (uint16_t)(j == k ? 0x1000 : rnd() % 64);
        }
    c->mp.i[3][3] = 1; c->mp.f[3][3] = 0x8000;
    c->mp.i[2][3] = 0; c->mp.f[2][3] = 0xf000;
    c->scale[0] = 640 * 2; c->scale[1] = -480 * 2; c->scale[2] = 0x1ff; c->scale[3] = 0x100;
    c->trans[0] = 640 * 2; c->trans[1] = 480 * 2; c->trans[2] = 0x1ff; c->trans[3] = 0;
    c->pn = 0xfffe; c->ratio = 3; c->sc_s = 0x8000; c->sc_t = 0x8000; c->c31_3 = 0x7f00; c->c31_7 = 0x7fff;
    c->tg[0] = (int16_t)0xc000; c->tg[1] = 0x44d3; c->tg[2] = 0x6cb3;
    c->mode = mode;
    c->L.top = 2;
    for (g = 0; g < 2; g++)
        for (m = 0; m < 3; m++)
            for (k = 0; k < 4; k++) {
                c->L.d[g][m][k] = k < 3 ? (int16_t)((int8_t)rnd() * 256) : 0;
                c->L.c[g][m][k] = k < 3 ? (int16_t)((rnd() & 255) << 7) : 0;
            }
}

static void bench_vtx(unsigned mode, const char *name)
{
    struct fx2_vctx c;
    struct fx2_vin in[34];
    struct fx2_vtx out[34];
    unsigned i, it = 20000, n = 30;
    double t0, ts, tn;
    plausible_ctx(&c, mode);
    for (i = 0; i < 34; i++) {
        in[i].x = (int16_t)(rnd() % 400) - 200; in[i].y = (int16_t)(rnd() % 400) - 200; in[i].z = (int16_t)(rnd() % 400) - 200;
        in[i].s = (int16_t)rnd(); in[i].t = (int16_t)rnd();
        in[i].c[0] = (uint8_t)rnd(); in[i].c[1] = (uint8_t)rnd(); in[i].c[2] = (uint8_t)rnd(); in[i].c[3] = (uint8_t)rnd();
    }
    t0 = now();
    for (i = 0; i < it; i++)
        fx2_vtx_xform_scalar(&c, in, n, out);
    ts = (now() - t0) / it / n;
    t0 = now();
    for (i = 0; i < it; i++)
        fx2_vtx_xform(&c, in, n, out);
    tn = (now() - t0) / it / n;
    printf("vtx %-16s scalar %7.1f ns/vertex   neon %7.1f ns/vertex   (%.2fx)\n", name, ts, tn, ts / tn);
}

static void bench_tri(unsigned g, unsigned tex, const char *name)
{
    struct fx2_vtx v[3];
    struct fx2_tri_in in;
    uint32_t cmd[0xb0 / 4];
    unsigned i, it = 200000, len, k;
    double t0, ts, tn;
    memset(v, 0, sizeof(v));
    for (k = 0; k < 3; k++) {
        v[k].sx = (int16_t)(100 + rnd() % 400); v[k].sy = (int16_t)(100 + rnd() % 400);
        v[k].szi = (int16_t)(rnd() % 1000); v[k].szf = (uint16_t)rnd();
        v[k].rwi = (int16_t)(rnd() % 8); v[k].rwf = (uint16_t)rnd();
        v[k].col[0] = (uint8_t)rnd(); v[k].col[1] = (uint8_t)rnd(); v[k].col[2] = (uint8_t)rnd(); v[k].col[3] = (uint8_t)rnd();
        v[k].s = (int16_t)(rnd() % 4000); v[k].t = (int16_t)(rnd() % 4000);
        in.v[k] = &v[k];
    }
    v[2].sx = 600; v[2].sy = 100;                       /* a clockwise... whichever: the cull table word decides */
    in.g = g; in.tex = tex; in.tile = 0; in.reject_mask = 0x7070; in.clip_mask = 0; in.cull = 0x80000000u;
    in.rcp_k = 0x20; in.dx_k = 0x1000; in.slope_lim = 0x100; in.frac_mask = 0xfff8;
    if (fx2_tri_setup_scalar(&in, cmd, &len) != FX2_TRI_DRAW) {
        in.cull = 0xffff0000u;
        if (fx2_tri_setup_scalar(&in, cmd, &len) != FX2_TRI_DRAW)
            printf("(not drawn)\n");
    }
    t0 = now();
    for (i = 0; i < it; i++) {
        v[0].sx = (int16_t)(100 + (i & 7)); fx2_tri_setup_scalar(&in, cmd, &len);
    }
    ts = (now() - t0) / it;
    t0 = now();
    for (i = 0; i < it; i++) {
        v[0].sx = (int16_t)(100 + (i & 7)); fx2_tri_setup(&in, cmd, &len);
    }
    tn = (now() - t0) / it;
    {
        struct fx2_tri_state st;
        struct fx2_pair att[3][8], R1;
        double tp, ta, tq;
        unsigned dummy = 0;
        t0 = now();
        for (i = 0; i < it; i++) { v[0].sx = (int16_t)(100 + (i & 7)); dummy += fx2_tri_prepare(&in, cmd, &st); }
        tp = (now() - t0) / it;
        t0 = now();
        for (i = 0; i < it; i++) dummy += fx2_tri_attr_prep(&in, &st, att, &R1);
        ta = (now() - t0) / it;
        t0 = now();
        for (i = 0; i < it; i++) dummy += fx2_tri_attrs_neon(&in, &st, cmd);
        tq = (now() - t0) / it;
        printf("    phases: prepare %6.1f  attr_prep %6.1f  attrs(prep+neon) %6.1f ns  (%u)\n", tp, ta, tq, dummy & 1);
    }
    printf("tri %-16s scalar %7.1f ns/tri      neon %7.1f ns/tri      (%.2fx)  len %u\n", name, ts, tn, ts / tn, len);
}

static void bench_mtx(void)
{
    struct fx2_mtx a, b, r;
    unsigned i, j, k, it = 200000;
    double t0, ts, tn;
    for (j = 0; j < 4; j++)
        for (k = 0; k < 4; k++) {
            a.i[j][k] = (int16_t)(rnd() % 4); a.f[j][k] = (uint16_t)rnd();
            b.i[j][k] = (int16_t)(rnd() % 4); b.f[j][k] = (uint16_t)rnd();
        }
    t0 = now();
    for (i = 0; i < it; i++) { a.f[0][0] = (uint16_t)i; fx2_mtx_mul_scalar(&a, &b, &r); }
    ts = (now() - t0) / it;
    t0 = now();
    for (i = 0; i < it; i++) { a.f[0][0] = (uint16_t)i; fx2_mtx_mul(&a, &b, &r); }
    tn = (now() - t0) / it;
    printf("mtx mul           scalar %7.1f ns/mul      neon %7.1f ns/mul      (%.2fx)\n", ts, tn, ts / tn);
}

int main(void)
{
    bench_vtx(0, "plain");
    bench_vtx(FX2_FOG, "fog");
    bench_vtx(FX2_LIGHTING, "lit");
    bench_vtx(FX2_LIGHTING | FX2_TEXGEN | FX2_FOG, "lit texgen fog");
    bench_tri(4 | 0x200000, 0, "shade");
    bench_tri(4 | 0x200000, 2, "shade tex");
    bench_tri(4 | 0x200000 | 1, 2, "shade tex z");
    bench_tri(1, 0, "z only");
    bench_tri(0, 0, "fill");
    bench_mtx();
    return 0;
}
