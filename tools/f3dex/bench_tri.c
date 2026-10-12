/*
 * bench_tri.c: timing of f3dex_tri_setup (NEON when built for aarch64) against f3dex_tri_setup_scalar on plausible data: a 6x6 mesh of vertices in a 32-vertex buffer (screen positions, colours,
 * texture coordinates, 1/w, z), triangles from the mesh cells; drawn triangles with geometry-mode flags 0 / 1 / 2 / 3 / 4 / 5 / 6 / 7 (flat and smooth), rejected, back-face culled, degenerate, and a mix
 * (60% drawn with flags 7 / 5 / 6 / 4, 20% rejected, 20% culled) as a scene would have.
 *   build_fxn.sh bench_tri host|a64|rg35 ;  run: bench_tri [passes (default 200)]       output: ns per triangle (best pass), the NEON : scalar ratio
 * It also checks that both forms return the same bytes for every triangle it times.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "f3dex_fx.h"

#define NT 4096
static uint64_t rs = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void)
{
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (uint32_t)(rs >> 16);
}
static int range(int lo, int hi) { return lo + (int)(rnd() % (unsigned)(hi - lo + 1)); }

static struct f3dex_vtx mesh[36];
static struct f3dex_tri_in tris[NT];
static uint8_t out[0xd0] __attribute__((aligned(16)));

static void make_mesh(int cell, int offx, int offy)
{
    for (int j = 0; j < 6; j++)
        for (int i = 0; i < 6; i++) {
            struct f3dex_vtx *v = &mesh[j * 6 + i];
            memset(v, 0, sizeof *v);
            v->sx = (int16_t)(offx + i * cell + range(-cell / 4, cell / 4));
            v->sy = (int16_t)(offy + j * cell + range(-cell / 4, cell / 4));
            int w = range(2, 60);
            v->cx[3] = (int16_t)w; v->cf[3] = (uint16_t)rnd();
            v->rwi = 0; v->rwf = (uint16_t)(0xffff / w);
            v->sz = (int16_t)range(0, 0x7000); v->szf = (uint16_t)rnd();
            v->s = (int16_t)(i * 32 * 8 + range(-20, 20)); v->t = (int16_t)(j * 32 * 8 + range(-20, 20));
            for (int c = 0; c < 4; c++) v->col[c] = (uint8_t)rnd();
        }
}

enum { K_DRAW, K_REJECT, K_CULL, K_DEGEN };
/* triangle n of a table: one cell of the mesh `m`, of the given kind and geometry mode */
static void make_tri(struct f3dex_tri_in *t, const struct f3dex_vtx *m, const struct f3dex_build *b, int kind, unsigned flags, int smooth)
{
    int i = (int)(rnd() % 5), j = (int)(rnd() % 5);
    const struct f3dex_vtx *a = &m[j * 6 + i], *bb = &m[j * 6 + i + 1], *c = &m[(j + 1) * 6 + i], *d = &m[(j + 1) * 6 + i + 1];
    memset(t, 0, sizeof *t);
    t->b = b;
    const struct f3dex_vtx *v0 = a, *v1 = bb, *v2 = c;
    if (rnd() & 1) { v0 = bb; v1 = d; v2 = c; }
    if (kind == K_DEGEN) v2 = v1;
    t->v[0] = v0; t->v[1] = v1; t->v[2] = v2;
    t->flat = v0;
    t->geom = flags | (smooth ? F3DEX_G_SMOOTH : 0) | F3DEX_G_CLIPPING | (kind == K_CULL ? F3DEX_G_CULL_BACK : 0);
    t->persp = 0xffff;
    t->tile_byte = 0x01;
    if (kind == K_CULL) {                                               /* the winding that is back-facing */
        uint8_t tmp[0xd0];
        unsigned l;
        if (f3dex_tri_setup_scalar(t, tmp, &l) != F3DEX_TRI_CULL) { t->v[1] = v2; t->v[2] = v1; }
    }
}
static void make_tris(const struct f3dex_build *b, int kind, unsigned flags, int smooth)
{
    for (int n = 0; n < NT; n++)
        make_tri(&tris[n], mesh, b, kind, flags, smooth);
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

typedef int (*fn_t)(const struct f3dex_tri_in *, uint8_t *, unsigned *);
static double run(fn_t f, int passes, unsigned long *sum)
{
    double best = 1e30;
    unsigned long s = 0;
    for (int p = 0; p < passes; p++) {
        double t0 = now();
        for (int n = 0; n < NT; n++) {
            unsigned len = 0;
            int r = f(&tris[n], out, &len);
            s += (unsigned long)r + len + out[len ? len - 1 : 0];
        }
        double t = (now() - t0) / NT;
        if (t < best) best = t;
    }
    *sum = s;
    return best;
}

static void report(const char *name, int passes)
{
    unsigned long s1, s2;
    int ok = 0, nreject = 0;
    /* equality of the bytes (a full compare on every triangle) */
    uint8_t o2[0xd0] __attribute__((aligned(16)));
    for (int n = 0; n < NT; n++) {
        unsigned l1 = 0, l2 = 0;
        memset(out, 0xa5, sizeof out); memset(o2, 0xa5, sizeof o2);
        int r1 = f3dex_tri_setup(&tris[n], out, &l1), r2 = f3dex_tri_setup_scalar(&tris[n], o2, &l2);
        if (r1 != r2 || (r1 == F3DEX_TRI_OK && l1 != l2) || memcmp(out, o2, sizeof out)) { printf("MISMATCH in %s triangle %d\n", name, n); exit(1); }
        ok += r1 == F3DEX_TRI_OK; nreject += r1 != F3DEX_TRI_OK;
    }
    double tn = run(f3dex_tri_setup, passes, &s1), ts = run(f3dex_tri_setup_scalar, passes, &s2);
    printf("%-28s drawn %4d/%d  neon %7.1f ns  scalar %7.1f ns  ratio %.2f%s\n", name, ok, NT, tn, ts, ts / tn, s1 != s2 ? "  (CHECKSUM DIFFERS)" : "");
}

int main(int argc, char **argv)
{
    int passes = argc > 1 ? atoi(argv[1]) : 200;
    const struct f3dex_build *builds[2] = { &f3dex_build_dex, &f3dex_build_dlx };
    const char *bn[2] = { "dex", "dlx" };
    static struct f3dex_vtx rej[36];
    printf("f3dex_tri_setup, per triangle (best of %d passes of %d)\n", passes, NT);
    for (int bi = 0; bi < 2; bi++) {
        const struct f3dex_build *b = builds[bi];
        char nm[64];
        make_mesh(40, -100, -100);
        static const unsigned fl[] = { 0, 1, 4, 5, 6, 7, 2, 3 };
        for (unsigned k = 0; k < sizeof fl / sizeof fl[0]; k++)
            for (int sm = 0; sm < 2; sm++) {
                if ((fl[k] & 4) == 0 && sm) continue;
                make_tris(b, K_DRAW, fl[k], sm);
                snprintf(nm, sizeof nm, "%s drawn flags %u%s", bn[bi], fl[k], (fl[k] & 4) ? (sm ? " smooth" : " flat") : "");
                report(nm, passes);
            }
        memcpy(rej, mesh, sizeof rej);                                  /* a vertex buffer whose vertices are all outside one plane */
        for (int k = 0; k < 36; k++) rej[k].flags = 0x0010;
        for (int n = 0; n < NT; n++) make_tri(&tris[n], rej, b, K_DRAW, 7, 1);
        snprintf(nm, sizeof nm, "%s rejected", bn[bi]);
        report(nm, passes);
        make_tris(b, K_CULL, 7, 1);
        snprintf(nm, sizeof nm, "%s back-face culled", bn[bi]);
        report(nm, passes);
        make_tris(b, K_DEGEN, 7, 1);
        snprintf(nm, sizeof nm, "%s degenerate", bn[bi]);
        report(nm, passes);
        /* a scene: 60% drawn (flags 7 / 5 / 6 / 4), 20% rejected, 20% culled */
        static const unsigned mixfl[10] = { 7, 7, 7, 5, 5, 6, 4, 7, 5, 7 };
        for (int n = 0; n < NT; n++) {
            int k = n % 10;
            if (k < 6) make_tri(&tris[n], mesh, b, K_DRAW, mixfl[(n / 10) % 10], 1);
            else if (k < 8) make_tri(&tris[n], rej, b, K_DRAW, 7, 1);
            else make_tri(&tris[n], mesh, b, K_CULL, 7, 1);
        }
        snprintf(nm, sizeof nm, "%s scene mix 60/20/20", bn[bi]);
        report(nm, passes);
    }
    return 0;
}
