/*
 * arMLEt - rej_fx.c: the scalar reference of the Rej geometry functions (rej_fx.h)
 *
 * Plain C11 on structs, bit-exact to what the microcode's vector code computes (tools/hle/test_rej.c checks it against translations of the microcode). The arithmetic is hle_fx.h's.
 * docs/ucode-rej.md has the microcode's side.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>
#include <string.h>

#include "f3dex2_fx_int.h"
#include "hle_fx.h"
#include "rej_fx.h"

#ifdef ARMLET_HLE_MUTATE
#define MUT(n, ok, bad) (ARMLET_HLE_MUTATE == (n) ? (bad) : (ok))
#else
#define MUT(n, ok, bad) (ok)
#endif

/* ---- (integer, fraction) pairs: the chains of f3dex2_fx.c ------------------------------------------------------------------------------------------------------------------ */
static inline void mm(struct fx2_pair a, struct fx2_pair b, hle_acc *t, hle_acc *full)
{
    *t = hle_wrap48(((hle_acc)a.f * b.f >> 16) + (hle_acc)a.i * b.f + (hle_acc)a.f * b.i);
    *full = hle_wrap48(*t + (((hle_acc)a.i * b.i) << 16));
}
static inline struct fx2_pair rd(hle_acc t, hle_acc full)
{
    struct fx2_pair r = { hle_sclamp(full), hle_uclamp(t) };
    return r;
}

/* ---- vertices ------------------------------------------------------------------------------------------------------------------------------------------------------------ */
/* From the clip coordinates P (x y z w, 32-bit) on. A vertex outside the guard band (x, y or z beyond w times the clip ratio: the compare of VCH / VCL against the scaled w) is marked: the
 * viewport transformation's accumulator starts with 0x7fff * 0x7fff, so every coordinate saturates, and the screen y (with its low two bits cleared) is 0x7ffc, which the triangle code rejects. */
static void rej_project(const struct fx2_vctx *c, unsigned flags, const struct fx2_pair P[4], struct rej_vtx *o, int16_t *fog)
{
    struct fx2_pair Q[4], R0, T, E, R1, ndc[4], ndc2[4], S[4];
    hle_acc tt, fl;
    unsigned vb = 0, k;
    int32_t Wq;
    int marked;

    {
        hle_acc prod = (hle_acc)P[3].f * c->ratio + (((hle_acc)P[3].i * c->ratio) << 16);
        int32_t W2 = (int32_t)((uint32_t)(uint16_t)hle_sclamp(prod) << 16 | (uint16_t)prod);
        for (k = 0; k < 3; k++) {
            int32_t X = (int32_t)((uint32_t)(uint16_t)P[k].i << 16 | P[k].f);
            vb |= (unsigned)hle_clip_le(X, W2) << k | (unsigned)hle_clip_ge(X, W2) << (8 + k);
        }
    }
    marked = vb != 0;
    for (k = 0; k < 4; k++) {
        hle_acc a = ((hle_acc)P[k].f * c->pn >> 16) + (hle_acc)P[k].i * c->pn;
        Q[k].i = hle_sclamp(a);
        Q[k].f = hle_uclamp(a);
    }
    Wq = (int32_t)((uint32_t)(uint16_t)Q[3].i << 16 | Q[3].f);
    R0.i = (int16_t)(hle_rcp32(Wq) >> 16);
    R0.f = (uint16_t)hle_rcp32(Wq);
    mm(Q[3], R0, &tt, &fl);
    T = rd(tt, fl);
    {
        hle_acc e1 = ((hle_acc)4 << 16) - (hle_acc)4 * T.f;
        hle_acc e2 = e1 - (((hle_acc)4 * T.i) << 16);
        E.i = hle_sclamp(e2);
        E.f = hle_uclamp(e1);
    }
    mm(E, R0, &tt, &fl);
    R1 = rd(tt, fl);
    for (k = 0; k < 4; k++) {
        const struct fx2_pair *p = &P[k == 3 ? 2 : k];                   /* the fourth lane is the fog: z in place of w */
        mm(*p, R1, &tt, &fl);
        ndc[k] = rd(tt, fl);
        {
            hle_acc a = ((hle_acc)ndc[k].f * c->pn >> 16) + (hle_acc)ndc[k].i * c->pn;
            ndc2[k].i = hle_sclamp(a);
            ndc2[k].f = hle_uclamp(a);
        }
    }
    for (k = 0; k < 4; k++) {
        hle_acc acc = ((hle_acc)(k == 1 && (flags & REJ_YQ) ? hle_clamp16((int32_t)c->trans[1] + 2) : c->trans[k]) << 16) + (k == 3 ? ((hle_acc)c->c31_3 << 16) : 0) + (marked ? (((hle_acc)0x7fff * c->c31_7) << 16) : 0);
        hle_acc t1 = hle_wrap48(acc + (hle_acc)ndc2[k].f * c->scale[k]);
        hle_acc f1 = hle_wrap48(t1 + (((hle_acc)ndc2[k].i * c->scale[k]) << 16));
        S[k] = rd(t1, f1);
    }
    o->sx = S[0].i;
    o->sy = (flags & REJ_YQ) ? (int16_t)(S[1].i & ~3) : S[1].i;
    o->szi = S[2].i;
    o->szf = S[2].f;
    o->rwi = R1.i;
    o->rwf = R1.f;
    *fog = S[3].i;
}

static void rej_vtx_one(const struct fx2_vctx *c, unsigned flags, const struct fx2_vin *vi, unsigned idx, struct rej_vtx *o)
{
    int16_t v[3] = { vi->x, vi->y, vi->z };
    int16_t s = vi->s, tt_ = vi->t, fog = 0;
    uint8_t col[4] = { vi->c[0], vi->c[1], vi->c[2], vi->c[3] };
    struct fx2_pair P[4];
    unsigned k, j;

    if (c->mode & FX2_LIGHTING) {
        int8_t nb[3] = { (int8_t)vi->c[0], (int8_t)vi->c[1], (int8_t)vi->c[2] };
        uint8_t lo[4];
        int16_t sg[2] = { 0, 0 };
        fx2_lit_vertex_scalar(c, (idx & 1) ? 0 : 1, nb, vi->c[3], lo, (c->mode & FX2_TEXGEN) != 0, sg);
        memcpy(col, lo, 4);
        if (c->mode & FX2_TEXGEN) {
            s = sg[0];
            tt_ = sg[1];
        }
    }
    for (k = 0; k < 4; k++) {
        hle_acc acc = hle_wrap48(((hle_acc)c->mp.i[3][k] << 16) + c->mp.f[3][k]);
        hle_acc last = 0;
        for (j = 0; j < 3; j++) {
            last = ((hle_acc)c->mp.i[j][k] * v[j]) << 16;
            acc = hle_wrap48(acc + (hle_acc)c->mp.f[j][k] * v[j] + last);
        }
        P[k].i = hle_sclamp(acc);
        P[k].f = hle_uclamp(hle_wrap48(acc - last));
    }
    rej_project(c, flags, P, o, &fog);
    if (c->mode & FX2_FOG)
        col[3] = (uint8_t)(fog < 0x7f00 ? 0x7f00 : fog);   /* the byte is the low byte of max(S3.i, 0x7f00) */
    memcpy(o->col, col, 4);
    o->s = (int16_t)(((int32_t)s * c->sc_s) >> 16);
    o->t = (int16_t)(((int32_t)tt_ * c->sc_t) >> 16);
}

void rej_vtx_xform_scalar(const struct fx2_vctx *c, unsigned flags, const struct fx2_vin *in, unsigned n, struct rej_vtx *out)
{
    unsigned i;
    for (i = 0; i < n; i++)
        rej_vtx_one(c, flags, &in[i], i, &out[i]);
}

#ifndef __aarch64__
void rej_vtx_xform(const struct fx2_vctx *c, unsigned flags, const struct fx2_vin *in, unsigned n, struct rej_vtx *out)
{
    rej_vtx_xform_scalar(c, flags, in, n, out);
}
#endif

/* ---- triangles ------------------------------------------------------------------------------------------------------------------------------------------------------------ */
/* The triangle code is the F3DEX2 one without the clipper (f3dex2_fx.c): the same slopes, header and shade, texture and z blocks (the attribute part is fx2_tri_attrs_*), with F3DZEX's lanes of the
 * constant vectors and no clamp on the slopes, behind other decisions: a vertex whose y is 0x7ffc or more is a rejected one (rej_project) and takes the triangle with it; the cull table has a bit per
 * sign of the cross product; and the sort of the three vertices breaks the tie of the two larger y values the other way (the larger of the pair that was compared with the third is the middle one). */
typedef uint16_t __attribute__((may_alias)) u16a;
static inline void put16(uint32_t *p, unsigned o, uint16_t v) { ((u16a *)p)[(o >> 1) ^ 1] = v; }
static inline void put32(uint32_t *p, unsigned o, uint32_t v) { p[o >> 2] = v; }
static inline struct fx2_pair fxs(int32_t v)
{
    struct fx2_pair r = { (int16_t)(v >> 16), (uint16_t)v };
    return r;
}

struct vrec {
    int16_t x, y;
    const struct fx2_vtx *v;
};

static int rej_tri_prepare(const struct rej_tri_in *in, const struct fx2_vtx *fv, uint32_t *cmd, struct fx2_tri_state *st)
{
    const struct fx2_vtx *va = &fv[0], *vb = &fv[1], *vc = &fv[2];
    struct vrec A = { va->sx, va->sy, va }, B = { vb->sx, vb->sy, vb }, C = { vc->sx, vc->sy, vc };
    struct vrec minAB, maxAB, mid, H, M, L;
    int16_t cross16;
    uint32_t g = in->g;
    int16_t dxMH, dyMH, dxLH, dyLH, dxLM, dyLM, nxMH, nyLH;
    int16_t dy[3], dx[3];
    struct fx2_pair slope_m[3], XH = { 0, 0 }, XM = { 0, 0 };
    uint32_t cfrac;
    int32_t xHf, Cv;
    unsigned k;

    for (k = 0; k < 3; k++)
        if (fv[k].sy >= MUT(346, REJ_MARK_Y, REJ_MARK_Y + 1))
            return FX2_TRI_SKIP;
    {
        int32_t sum = (int32_t)hle_clamp16((int32_t)A.x - B.x) * hle_clamp16((int32_t)A.y - C.y) + (int32_t)hle_clamp16((int32_t)A.x - C.x) * hle_clamp16((int32_t)B.y - A.y);
        cross16 = hle_clamp16(sum);
    }
    if (cross16 == 0 || (cross16 < 0 ? !in->pass_neg : !in->pass_pos))
        return FX2_TRI_SKIP;

    minAB = A.y < B.y ? A : B;
    maxAB = A.y >= B.y ? A : B;
    {
        int cmp = minAB.y >= C.y, ge;
        mid = cmp ? minAB : C;
        H = cmp ? C : minAB;
        ge = maxAB.y >= mid.y;
        M = MUT(347, ge, !ge) ? mid : maxAB;
        L = ge ? maxAB : mid;
    }
    dxMH = hle_clamp16((int32_t)M.x - H.x);
    dyMH = hle_clamp16((int32_t)M.y - H.y);
    dxLH = hle_clamp16((int32_t)L.x - H.x);
    dyLH = hle_clamp16((int32_t)L.y - H.y);
    dxLM = hle_clamp16((int32_t)L.x - M.x);
    dyLM = hle_clamp16((int32_t)L.y - M.y);
    nxMH = hle_clamp16((int32_t)H.x - M.x);
    nyLH = hle_clamp16((int32_t)H.y - L.y);
    st->area = (int32_t)dyMH * dxLH + (int32_t)dyLH * nxMH;
    dy[0] = dyLM; dy[1] = dyMH; dy[2] = dyLH;      /* DxLDy, DxMDy, DxHDy */
    dx[0] = dxLM; dx[1] = dxMH; dx[2] = dxLH;
    cfrac = ((uint32_t)(uint16_t)H.y * 0x4000u) & 0xffff;
    xHf = (int32_t)H.x * 0x4000;
    Cv = -(int32_t)cfrac;
    st->C0.i = (int16_t)(Cv >> 16);
    st->C0.f = (uint16_t)Cv;
    for (k = 0; k < 3; k++) {
        int32_t Rr = (int32_t)hle_rcp32((int32_t)dy[k]);
        hle_acc a = hle_mac_32xu16(Rr, in->rcp_k);                       /* VMUDL VMADM VMADN with v31[10] = 8 */
        struct fx2_pair A32 = { hle_sclamp(a), hle_uclamp(a) };
        struct fx2_pair dxs = fxs(dx[k] * (int32_t)in->dx_k);                  /* VMUDM / VMADN 0 with v31[13] = 0x4000 */
        hle_acc tt, full;
        struct fx2_pair S;
        mm(dxs, A32, &tt, &full);
        S = rd(tt, full);
        st->slope[k] = S;
        slope_m[k] = S;
    }
    for (k = 1; k < 3; k++) {
        hle_acc tt = (hle_acc)xHf + ((hle_acc)slope_m[k].f * st->C0.f >> 16) + (hle_acc)slope_m[k].i * st->C0.f + (hle_acc)slope_m[k].f * st->C0.i;
        struct fx2_pair r = rd(tt, tt + (((hle_acc)slope_m[k].i * st->C0.i) << 16));
        if (k == 1)
            XM = r;
        else
            XH = r;
    }
    put16(cmd, 0, (uint16_t)(((((g | 0xc8) | in->tex) & 0xff) << 8) | (((st->area < 0 ? 0x80 : 0) | in->tile) & 0xff)));
    put16(cmd, 2, (uint16_t)L.y);
    put16(cmd, 4, (uint16_t)M.y);
    put16(cmd, 6, (uint16_t)H.y);
    put32(cmd, 8, (uint32_t)((int32_t)M.x << 14));
    put16(cmd, 12, (uint16_t)st->slope[0].i);
    put16(cmd, 14, st->slope[0].f);
    put16(cmd, 16, (uint16_t)XH.i);
    put16(cmd, 18, XH.f);
    put16(cmd, 20, (uint16_t)st->slope[2].i);
    put16(cmd, 22, st->slope[2].f);
    put16(cmd, 24, (uint16_t)XM.i);
    put16(cmd, 26, XM.f);
    put16(cmd, 28, (uint16_t)st->slope[1].i);
    put16(cmd, 30, st->slope[1].f);
    st->H = H.v;
    st->M = M.v;
    st->L = L.v;
    st->dyMH = dyMH;
    st->nyLH = nyLH;
    st->dxLH = dxLH;
    st->nxMH = nxMH;
    return FX2_TRI_DRAW;
}

static void rej_tri_in_f2(const struct rej_tri_in *in, struct fx2_vtx fv[3], struct fx2_tri_in *ti)
{
    unsigned k;
    memset(fv, 0, 3 * sizeof fv[0]);
    for (k = 0; k < 3; k++) {
        const struct rej_vtx *r = in->v[k];
        memcpy(fv[k].col, r->col, 4);
        fv[k].s = r->s;
        fv[k].t = r->t;
        fv[k].sx = r->sx;
        fv[k].sy = r->sy;
        fv[k].szi = r->szi;
        fv[k].szf = r->szf;
        fv[k].rwi = r->rwi;
        fv[k].rwf = r->rwf;
    }
    if ((in->flags & REJ_FLAT_A) && !(in->g & 0x200000))     /* (flat shading: the colour of the first vertex, with F3DFLX its alpha too) */
        for (k = 1; k < 3; k++)
            fv[k].col[3] = fv[0].col[3];
    memset(ti, 0, sizeof *ti);
    for (k = 0; k < 3; k++)
        ti->v[k] = &fv[k];
    ti->g = in->g;
    ti->tex = in->tex;
    ti->tile = in->tile;
}

int rej_tri_setup_core(const struct rej_tri_in *in, uint32_t *cmd, unsigned *len, int neon)
{
    struct fx2_vtx fv[3];
    struct fx2_tri_in ti;
    struct fx2_tri_state st;
    int r;
    rej_tri_in_f2(in, fv, &ti);
    r = rej_tri_prepare(in, fv, cmd, &st);
    if (r != FX2_TRI_DRAW)
        return r;
    if (!((in->g & 4) || in->tex || (in->g & 1)))
        *len = 32;
#ifdef __aarch64__
    else if (neon)
        *len = fx2_tri_attrs_neon(&ti, &st, cmd);
#endif
    else
        *len = fx2_tri_attrs_scalar(&ti, &st, cmd);
    (void)neon;
    return FX2_TRI_DRAW;
}

int rej_tri_setup_scalar(const struct rej_tri_in *in, uint32_t *cmd, unsigned *len)
{
    return rej_tri_setup_core(in, cmd, len, 0);
}

#ifndef __aarch64__
int rej_tri_setup(const struct rej_tri_in *in, uint32_t *cmd, unsigned *len)
{
    return rej_tri_setup_core(in, cmd, len, 0);
}
#endif
