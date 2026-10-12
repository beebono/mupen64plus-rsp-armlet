/*
 * bench_vtx.c: timing of f3dex_vtx_xform (NEON on aarch64) against f3dex_vtx_xform_scalar on game-like G_VTX calls: a perspective x model-view matrix, vertices around the origin (most inside the
 * view volume), 16 and 32 vertex calls, unlit / lit (one light, ambient and the two look-at records) / lit with texgen / with fog, on the three builds. Prints ns per vertex and the ratio, after
 * checking that the two forms give the same records.
 *   build_fxn.sh bench_vtx host|a64|rg35 ;  run: bench_vtx [rounds (default 200000)]      (under qemu the times mean nothing)
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "f3dex_fx.h"

static uint64_t rs = 0x1234567887654321ull;
static uint32_t rnd(void)
{
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (uint32_t)(rs >> 16);
}
static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static void set_m(struct f3dex_mtx *m, int j, int k, double v)
{
    long raw = lround(v * 65536.0);
    m->i[j][k] = (int16_t)(raw >> 16);
    m->f[j][k] = (uint16_t)raw;
}

/* model-view (a rotation about y and x, scale 0.25, then the eye 1500 units back) x a 60 degree perspective, 4:3 */
static void game_mvp(struct f3dex_mtx *m)
{
    double a = 0.7, b = 0.3, s = 0.25;
    double R[4][4] = { { cos(a) * s, 0, -sin(a) * s, 0 }, { sin(a) * sin(b) * s, cos(b) * s, cos(a) * sin(b) * s, 0 }, { sin(a) * cos(b) * s, -sin(b) * s, cos(a) * cos(b) * s, 0 }, { 40, -20, -1500 * 0.25, 1 } };
    double f = 1.0 / tan(0.5236), asp = 4.0 / 3.0, zn = 10, zf = 12800;
    double P[4][4] = { { f / asp, 0, 0, 0 }, { 0, f, 0, 0 }, { 0, 0, (zf + zn) / (zn - zf), -1 }, { 0, 0, 2 * zf * zn / (zn - zf), 0 } };
    for (int j = 0; j < 4; j++)
        for (int k = 0; k < 4; k++) {
            double v = 0;
            for (int q = 0; q < 4; q++)
                v += R[j][q] * P[q][k];
            set_m(m, j, k, v);
        }
}

enum { SETS = 64, MAXN = 32 };
struct scenario {
    const char *name;
    const struct f3dex_build *b;
    uint32_t geom;
    unsigned nrec;
};

int main(int argc, char **argv)
{
    unsigned rounds = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 200000;
    static struct f3dex_vin in[SETS][MAXN];
    static struct f3dex_vtx outa[MAXN], outb[MAXN];
    struct f3dex_vctx c;
    memset(&c, 0, sizeof c);
    game_mvp(&c.mp);
    c.vscale[0] = 640; c.vscale[1] = -480; c.vscale[2] = 0x1ff; c.vscale[3] = 0;
    c.vtrans[0] = 640; c.vtrans[1] = 480; c.vtrans[2] = 0x1ff; c.vtrans[3] = 0;
    c.vsign[0] = 1; c.vsign[1] = -1; c.vsign[2] = 1; c.vsign[3] = 1;
    c.lim[0] = 4090; c.lim[1] = -4090; c.lim[2] = 0x7fff; c.lim[3] = 0;
    c.persp = 0xffff;
    c.clip_k = 2;
    c.tscale[0] = c.tscale[1] = 0x8000;
    c.fog[0] = 0x100; c.fog[1] = 0x20; c.fog[2] = 0xff;
    c.tgbias = 0x4000;
    c.tg[0] = 0x7fff; c.tg[1] = 0x571d; c.tg[2] = 0x3a0c;
    for (int i = 0; i < 8; i++) c.lk[i] = i % 4 == 0 ? 0x80 : 0;
    for (int k = 0; k < F3DEX_MAX_REC + 2; k++)
        for (int i = 0; i < 8; i++) c.lcol[k][i] = k == 0 ? (i % 4 == 1 ? 0x80 : 0) : k == 1 ? 0 : (uint8_t)(0x60 + 8 * (i % 4));
    for (int k = 0; k < F3DEX_MAX_REC + 1; k++)
        for (int i = 0; i < 8; i++) c.ldir[k][i] = (int8_t)((i % 4 == 3) ? 0 : (i % 4 == k % 3 ? 100 : -30 + 10 * k));
    for (int s = 0; s < SETS; s++)
        for (int i = 0; i < MAXN; i++) {
            struct f3dex_vin *v = &in[s][i];
            v->x = (int16_t)((int)(rnd() % 700) - 350);
            v->y = (int16_t)((int)(rnd() % 700) - 350);
            v->z = (int16_t)((int)(rnd() % 700) - 350);
            if (rnd() % 20 == 0) v->x = (int16_t)rnd();
            v->s = (int16_t)(rnd() % 2048 - 512);
            v->t = (int16_t)(rnd() % 2048 - 512);
            for (int q = 0; q < 4; q++) v->c[q] = (uint8_t)rnd();
        }
    const struct scenario sc[] = {
        { "unlit", &f3dex_build_dex, F3DEX_G_ZBUFFER | F3DEX_G_SHADE | F3DEX_G_CLIPPING, 0 },
        { "unlit+fog", &f3dex_build_dex, F3DEX_G_ZBUFFER | F3DEX_G_SHADE | F3DEX_G_FOG, 0 },
        { "unlit NoN", &f3dex_build_non, F3DEX_G_ZBUFFER | F3DEX_G_SHADE, 0 },
        { "unlit dlx", &f3dex_build_dlx, F3DEX_G_ZBUFFER | F3DEX_G_SHADE | F3DEX_G_CLIPPING, 0 },
        { "lit nrec=3", &f3dex_build_dex, F3DEX_G_ZBUFFER | F3DEX_G_LIGHTING | F3DEX_G_SHADE, 3 },
        { "lit nrec=4 fog", &f3dex_build_dex, F3DEX_G_ZBUFFER | F3DEX_G_LIGHTING | F3DEX_G_SHADE | F3DEX_G_FOG, 4 },
        { "lit nrec=3 texgen", &f3dex_build_dex, F3DEX_G_ZBUFFER | F3DEX_G_LIGHTING | F3DEX_G_TEXGEN | F3DEX_G_SHADE, 3 },
        { "lit nrec=3 texgen lin", &f3dex_build_dex, F3DEX_G_ZBUFFER | F3DEX_G_LIGHTING | F3DEX_G_TEXGEN | F3DEX_G_TEXGEN_LINEAR | F3DEX_G_SHADE, 3 },
    };
    static const unsigned ns[2] = { 16, 32 };
    printf("%-24s %4s %12s %12s %8s\n", "scenario", "n", "scalar ns/v", "neon ns/v", "ratio");
    volatile unsigned sink = 0;
    for (unsigned s = 0; s < sizeof sc / sizeof sc[0]; s++)
        for (int ni = 0; ni < 2; ni++) {
            unsigned n = ns[ni];
            c.b = sc[s].b;
            c.geom = sc[s].geom;
            c.nrec = sc[s].nrec;
            for (int set = 0; set < SETS; set++) {
                f3dex_vtx_xform_scalar(&c, in[set], n, outa);
                f3dex_vtx_xform(&c, in[set], n, outb);
                if (memcmp(outa, outb, n * sizeof outa[0])) { printf("MISMATCH in %s n=%u set %d\n", sc[s].name, n, set); return 1; }
            }
            double t0 = now();
            for (unsigned r = 0; r < rounds; r++) {
                f3dex_vtx_xform_scalar(&c, in[r % SETS], n, outa);
                sink += outa[r % n].sx;
            }
            double t1 = now();
            for (unsigned r = 0; r < rounds; r++) {
                f3dex_vtx_xform(&c, in[r % SETS], n, outb);
                sink += outb[r % n].sx;
            }
            double t2 = now();
            double a = (t1 - t0) / rounds / n * 1e9, b = (t2 - t1) / rounds / n * 1e9;
            printf("%-24s %4u %12.1f %12.1f %8.2f\n", sc[s].name, n, a, b, a / b);
        }
    return (int)(sink == 0xdeadbeef);
}
