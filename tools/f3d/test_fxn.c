/*
 * arMLEt - tools/f3d/test_fxn.c: the NEON forms of the struct-level F3D functions (src/hle/f3d_fx.h) against their scalar forms, on random inputs. No JIT: it builds and runs for aarch64 (qemu or a device);
 * on other targets the "NEON" form is the scalar form and the test is trivial.
 *   test_fxn [-n count] [-s seed] [-m mutation-ignored]
 * The scalar forms are the ones checked against the JIT (test_fx).
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "f3d_fx.h"

static uint64_t rs = 88172645463325252ull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 16); }
static int below(int n) { return (int)(rnd() % (unsigned)n); }
static int range(int lo, int hi) { return lo + below(hi - lo + 1); }
static int chance(int pct) { return below(100) < pct; }

static void rand_mtx(struct f3d_mtx *m, int mode)
{
    for (int j = 0; j < 4; j++)
        for (int k = 0; k < 4; k++) {
            int16_t mi;
            switch (mode) {
            case 0: mi = (int16_t)range(-4, 4); break;
            case 1: mi = (int16_t)range(-300, 300); break;
            case 2: mi = (int16_t)rnd(); break;
            case 3: mi = (j == k) ? 1 : 0; break;
            case 4: mi = (int16_t)range(-20000, 20000); break;
            default: mi = chance(50) ? 0x7fff : (int16_t)0x8000; break;
            }
            uint16_t mf = (uint16_t)rnd();
            if (chance(4)) mf = chance(50) ? 0xffff : 0;
            m->i[j][k] = mi;
            m->f[j][k] = mf;
        }
}

static void rand_ctx(struct f3d_vctx *c, int lit)
{
    rand_mtx(&c->mp, chance(80) ? below(5) : 5);
    if (chance(15))
        for (int j = 0; j < 4; j++) { c->mp.i[j][3] = chance(70) ? 0 : (int16_t)range(-2, 2); c->mp.f[j][3] = (uint16_t)range(0, 300); }
    for (int k = 0; k < 4; k++) {
        c->vscale[k] = chance(50) ? (int16_t)(k < 2 ? (k ? -480 : 640) : 0x1ff) : (int16_t)rnd();
        c->vtrans[k] = chance(50) ? (int16_t)(k < 2 ? (k ? 480 : 640) : 0x1ff) : (int16_t)rnd();
        c->vsign[k] = (k == 1) ? -1 : 1;
    }
    c->lim[0] = 4090; c->lim[1] = -4090; c->lim[2] = 0x7fff; c->lim[3] = 0;
    if (chance(15)) { c->lim[0] = (int16_t)rnd(); c->lim[1] = (int16_t)rnd(); c->lim[2] = (int16_t)rnd(); c->lim[3] = (int16_t)rnd(); }
    c->persp = chance(70) ? 0xffff : (uint16_t)rnd();
    c->flags_variant = chance(40) ? F3D_FLAGS_PW : F3D_FLAGS_SM64;
    c->has_off = chance(30);                     /* Rare's vertex offset */
    for (int k = 0; k < 4; k++) {
        c->off_i[k] = chance(50) ? (int16_t)range(-100, 100) : (int16_t)rnd();
        c->off_f[k] = chance(10) ? 0xffff : (uint16_t)rnd();
    }
    c->fog_wide = chance(25);                    /* Indiana Jones's fog */
    for (int k = 0; k < 5; k++) c->fogw[k] = chance(50) ? (int16_t)range(-8, 8) : (int16_t)rnd();
    if (chance(70)) c->fogw[4] = (int16_t)range(1, 255);
    c->clip_acc = chance(30) ? 1 : 0;
    c->clip_k = chance(75) ? 1 : chance(50) ? (int16_t)range(1, 6) : (int16_t)rnd();
    c->tscale[0] = chance(60) ? 0xffff : (uint16_t)rnd();
    c->tscale[1] = chance(60) ? 0xffff : (uint16_t)rnd();
    c->geom = 0x2204 | (chance(30) ? F3D_G_FOG : 0) | (lit ? F3D_G_LIGHTING : 0) | (chance(30) ? F3D_G_TEXGEN : 0) | (chance(30) ? F3D_G_TEXGEN_LINEAR : 0);
    for (int k = 0; k < 3; k++) c->fog[k] = (int16_t)rnd();
    if (chance(80)) c->fog[2] = chance(80) ? 0xff : (int16_t)(rnd() & 0x3ff);
    c->nslot = lit ? (unsigned)range(1, F3D_MAX_SLOTS > 8 ? 8 : F3D_MAX_SLOTS) : 0;
    if (lit) {
        for (unsigned k = 0; k <= c->nslot + 1; k++)
            for (int i = 0; i < 8; i++) c->lcol[k][i] = (uint8_t)(chance(30) ? 0 : chance(50) ? 0x7f : rnd());
        for (unsigned k = 0; k <= c->nslot; k++)
            for (int i = 0; i < 8; i++) c->ldir[k][i] = (int8_t)(chance(30) ? 0 : chance(50) ? 0x7f : rnd());
        for (int i = 0; i < 8; i++) c->lk[i] = (int8_t)(chance(60) ? 0x80 : rnd());
        for (int i = 0; i < 3; i++) c->tg[i] = chance(50) ? (int16_t)rnd() : (int16_t)range(-3000, 3000);
    }
    for (int p = 0; p < 6; p++) {
        static const uint16_t pm[6] = { 0x40, 0x4000, 0x200, 0x100, 0x2, 0x1 };
        static const int16_t pc[6][4] = { {0,0,1,1}, {0,0,1,-1}, {0,1,0,-1}, {1,0,0,-1}, {0,1,0,1}, {1,0,0,1} };
        c->plane_mask[p] = pm[p];
        for (int k = 0; k < 4; k++) c->plane[p][k] = pc[p][k];
    }
}

static int check_vtx(unsigned count)
{
    int fails = 0;
    for (unsigned it = 0; it < count; it++) {
        struct f3d_vctx c;
        int lit = chance(50);
        rand_ctx(&c, lit);
        unsigned n = (unsigned)range(1, 16);
        struct f3d_vin in[16];
        struct f3d_vtx a[16], b[16];
        int vmode = below(3);
        for (unsigned i = 0; i < n; i++) {
            int16_t v[3];
            for (int k = 0; k < 3; k++)
                v[k] = vmode == 0 ? (int16_t)range(-1000, 1000) : vmode == 1 ? (int16_t)rnd() : (int16_t)range(-40, 40);
            in[i].x = v[0]; in[i].y = v[1]; in[i].z = v[2];
            in[i].s = (int16_t)rnd(); in[i].t = (int16_t)rnd();
            for (int k = 0; k < 4; k++) in[i].c[k] = (uint8_t)rnd();
        }
        memset(a, 0xa5, sizeof a); memset(b, 0xa5, sizeof b);
        int r1 = f3d_vtx_xform_scalar(&c, in, n, a), r2 = f3d_vtx_xform(&c, in, n, b);
        if (r1 != r2 || memcmp(a, b, n * sizeof a[0])) {
            fails++;
            printf("FAIL vtx it=%u n=%u lit=%d K=%d geom=%x\n", it, n, lit, c.clip_k, c.geom);
            for (unsigned i = 0; i < n; i++) if (memcmp(&a[i], &b[i], sizeof a[0])) { printf(" vertex %u differs\n", i); break; }
            if (fails > 4) break;
        }
    }
    printf("vtx: %u cases, %d failed\n", count, fails);
    return fails;
}

static int check_mtx(unsigned count)
{
    int fails = 0;
    for (unsigned it = 0; it < count; it++) {
        struct f3d_mtx x, y, a, b;
        rand_mtx(&x, below(6)); rand_mtx(&y, below(6));
        f3d_mtx_mul_scalar(&x, &y, &a);
        f3d_mtx_mul(&x, &y, &b);
        if (memcmp(&a, &b, sizeof a)) { fails++; printf("FAIL mtx it=%u\n", it); if (fails > 4) break; }
    }
    printf("mtx: %u cases, %d failed\n", count, fails);
    return fails;
}

static void rand_vtx(struct f3d_vtx *v, int mode, int cx, int cy, int tr)
{
    for (int k = 0; k < 4; k++) {
        v->cx[k] = mode == 4 ? (int16_t)range(-4, 4) : (int16_t)rnd();
        v->cf[k] = (uint16_t)rnd();
    }
    if (chance(70)) v->cx[3] = (int16_t)range(0, 8);
    for (int k = 0; k < 4; k++) v->col[k] = (uint8_t)(chance(30) ? 0xff : rnd());
    v->s = (int16_t)rnd(); v->t = (int16_t)rnd();
    if (mode == 0) { v->sx = (int16_t)range(-600, 600); v->sy = (int16_t)range(-600, 600); }
    else if (mode == 1) { v->sx = (int16_t)range(-2000, 2000); v->sy = (int16_t)range(-2000, 2000); }
    else if (mode == 2) { v->sx = (int16_t)rnd(); v->sy = (int16_t)rnd(); }
    else { v->sx = (int16_t)(cx + range(-tr, tr)); v->sy = (int16_t)(cy + range(-tr, tr)); }
    v->sz = (int16_t)rnd(); v->szf = (uint16_t)rnd();
    v->rwi = chance(50) ? (int16_t)range(0, 4) : (int16_t)rnd(); v->rwf = (uint16_t)rnd();
    v->flags = chance(85) ? 0 : (uint16_t)(rnd() & (chance(50) ? 0x7373 : 0xffff));
}

static int check_tri(unsigned count)
{
    int fails = 0, ok = 0;
    for (unsigned it = 0; it < count; it++) {
        struct f3d_vtx v[3];
        int mode = below(5), cx = range(-2000, 2000), cy = range(-2000, 2000), tr = chance(50) ? 3 : chance(50) ? 12 : 40;
        for (int k = 0; k < 3; k++) rand_vtx(&v[k], mode, cx, cy, tr);
        if (chance(10)) v[1].sx = v[0].sx;
        if (chance(10)) v[2].sy = v[0].sy;
        if (chance(5)) v[1] = v[0];
        struct f3d_tri_in ti;
        memset(&ti, 0, sizeof ti);
        for (int k = 0; k < 3; k++) ti.v[k] = &v[k];
        ti.flat = &v[below(3)];
        ti.geom = (uint32_t)(chance(60) ? 4 : 0) | (chance(50) ? 2 : 0) | (chance(50) ? 1 : 0) | (chance(55) ? 0x200 : 0) | (chance(12) ? 0x1000 : 0) | (chance(12) ? 0x2000 : 0) | (chance(10) ? (rnd() & 0xf8) : 0);
        ti.persp = chance(70) ? 0xffff : (uint16_t)rnd();
        ti.tile_byte = chance(60) ? 0 : (uint8_t)rnd();
        ti.zclamp = chance(30) ? 0 : (uint8_t)rnd();
        ti.reject_mask = 0x7070; ti.clip_mask = 0x4343;
        ti.vco_carry = chance(10);
        ti.variant = chance(40) ? F3D_TRIV_SM64 : chance(40) ? F3D_TRIV_WR : chance(50) ? F3D_TRIV_RARE : F3D_TRIV_INDY;
        if (ti.variant == F3D_TRIV_INDY && chance(80)) {     /* Indiana Jones's sort table: any permutation, any parity */
            static const uint8_t perm[6][3] = { {0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0} };
            unsigned q = below(6);
            ti.sort_given = 1;
            memcpy(ti.ord, perm[q], 3);
            ti.par = chance(50);
        }
        uint8_t c1[0xc0], c2[0xc0];
        unsigned l1 = 0, l2 = 0;
        memset(c1, 0x55, sizeof c1); memset(c2, 0x55, sizeof c2);
        int noclip = chance(30);
        int r1 = noclip ? f3d_tri_setup_noclip_scalar(&ti, c1, &l1) : f3d_tri_setup_scalar(&ti, c1, &l1);
        int r2 = noclip ? f3d_tri_setup_noclip(&ti, c2, &l2) : f3d_tri_setup(&ti, c2, &l2);
        if (r1 == F3D_TRI_OK) ok++;
        if (r1 != r2 || (r1 == F3D_TRI_OK && (l1 != l2 || memcmp(c1, c2, l1)))) {
            fails++;
            printf("FAIL tri it=%u rc %d/%d len %u/%u geom=%x\n", it, r1, r2, l1, l2, ti.geom);
            for (unsigned i = 0; i < l1 && i < l2; i++) if (c1[i] != c2[i]) { printf(" first byte difference at %u: %02x %02x\n", i, c1[i], c2[i]); break; }
            if (fails > 4) break;
        }
    }
    printf("tri: %u cases (%d drawn), %d failed\n", count, ok, fails);
    return fails;
}

int main(int argc, char **argv)
{
    unsigned count = 20000;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) count = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) rs ^= (uint64_t)atoi(argv[++i]) * 0x9e3779b97f4a7c15ull;
    }
    int f = check_vtx(count) + check_mtx(count) + check_tri(count);
    printf("%s\n", f ? "FAILED" : "all NEON forms equal the scalar forms");
    return f != 0;
}
