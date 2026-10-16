/*
 * arMLEt - f3dex2_fx_neon.c: the NEON forms of the F3DEX2 geometry functions (f3dex2_fx.h)
 *
 * The same arithmetic as f3dex2_fx.c, four lanes at a time: the 16-bit halves of the (integer, fraction) pairs are multiplied with the widening multiplies, the sums of the 48-bit accumulator are
 * 64-bit lanes (two int64x2 for four lanes), the saturating readouts are the saturating narrows. tools/hle/test_fx2.c checks every function bit for bit against the scalar form.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifdef __aarch64__
#include <arm_neon.h>
#include <string.h>

#include "f3dex2_fx.h"
#include "f3dex2_fx_int.h"
#include "hle_fx.h"

/* test builds only (-DFX2_NEON_MUTATE=n): a deliberate error in the NEON form only, to show that tools/hle/test_fx2.c catches it */
#ifdef FX2_NEON_MUTATE
#define NMUT(n, ok, bad) (FX2_NEON_MUTATE == (n) ? (bad) : (ok))
#else
#define NMUT(n, ok, bad) (ok)
#endif

#define NI static inline __attribute__((always_inline))

/* HLE_FX2_SCALAR=1: the scalar forms (A/B timing on a device) */
#include <stdlib.h>
static int g_scalar = -1;
NI int use_scalar(void)
{
    if (__builtin_expect(g_scalar < 0, 0))
        g_scalar = getenv("HLE_FX2_SCALAR") != NULL;
    return g_scalar;
}

/* four lanes of 48-bit accumulator in two int64x2 */
typedef struct {
    int64x2_t lo, hi;
} v64;

NI v64 v64_add(v64 a, v64 b)
{
    v64 r = { vaddq_s64(a.lo, b.lo), vaddq_s64(a.hi, b.hi) };
    return r;
}
NI v64 v64_wrap48(v64 a)
{
    v64 r = { vshrq_n_s64(vshlq_n_s64(a.lo, 16), 16), vshrq_n_s64(vshlq_n_s64(a.hi, 16), 16) };
    return NMUT(3, r, a);
}
NI v64 v64_from_s32(int32x4_t v)
{
    v64 r = { vmovl_s32(vget_low_s32(v)), vmovl_high_s32(v) };
    return r;
}
NI v64 v64_addw_s32(v64 a, int32x4_t v)
{
    v64 r = { vaddw_s32(a.lo, vget_low_s32(v)), vaddw_high_s32(a.hi, v) };
    return r;
}
NI v64 v64_shl16_s32(int32x4_t v)
{
    v64 r = { vshll_n_s32(vget_low_s32(v), 16), vshll_high_n_s32(v, 16) };
    return r;
}
NI v64 v64_zero(void)
{
    v64 r = { vdupq_n_s64(0), vdupq_n_s64(0) };
    return r;
}
/* VMUDH...: bits 47..16, saturated to s16 */
NI int16x4_t sclamp4(v64 a)
{
    return vqmovn_s32(vcombine_s32(vqmovn_s64(vshrq_n_s64(a.lo, 16)), vqmovn_s64(vshrq_n_s64(a.hi, 16))));
}
/* low 16 bits, 0xffff / 0 when the accumulator does not fit in 32 bits (the saturation of the narrow gives exactly that) */
NI uint16x4_t uclamp4(v64 a)
{
    return NMUT(4, vmovn_u32(vreinterpretq_u32_s32(vcombine_s32(vqmovn_s64(a.lo), vqmovn_s64(a.hi)))), vmovn_u32(vreinterpretq_u32_s32(vcombine_s32(vmovn_s64(a.lo), vmovn_s64(a.hi)))));
}

/* t = base + (af*bf >> 16) + ai*bf + af*bi; full = t + (ai*bi << 16); each wrapped to 48 bits. b is a scalar pair. */
NI void mm4s(v64 base, int16x4_t ai, uint16x4_t af, int16_t bi, uint16_t bf, v64 *t, v64 *full)
{
    uint32x4_t p1 = NMUT(2, vshrq_n_u32(vmull_n_u16(af, bf), 16), vshrq_n_u32(vmull_n_u16(af, bf), 15));
    int32x4_t p2 = vmulq_n_s32(vmovl_s16(ai), (int32_t)bf);
    int32x4_t p3 = vmulq_n_s32(vreinterpretq_s32_u32(vmovl_u16(af)), (int32_t)bi);
    int32x4_t p4 = vmull_n_s16(ai, bi);
    v64 tt = v64_addw_s32(v64_addw_s32(v64_addw_s32(base, vreinterpretq_s32_u32(p1)), p2), p3);
    tt = v64_wrap48(tt);
    *t = tt;
    *full = v64_wrap48(v64_add(tt, v64_shl16_s32(p4)));
}
/* the same with a vector b */
NI void mm4v(v64 base, int16x4_t ai, uint16x4_t af, int16x4_t bi, uint16x4_t bf, v64 *t, v64 *full)
{
    uint32x4_t p1 = vshrq_n_u32(vmull_u16(af, bf), 16);
    int32x4_t p2 = vmulq_s32(vmovl_s16(ai), vreinterpretq_s32_u32(vmovl_u16(bf)));
    int32x4_t p3 = vmulq_s32(vreinterpretq_s32_u32(vmovl_u16(af)), vmovl_s16(bi));
    int32x4_t p4 = vmull_s16(ai, bi);
    v64 tt = v64_addw_s32(v64_addw_s32(v64_addw_s32(base, vreinterpretq_s32_u32(p1)), p2), p3);
    tt = v64_wrap48(tt);
    *t = tt;
    *full = v64_wrap48(v64_add(tt, v64_shl16_s32(p4)));
}

/* ---- triangle attributes ------------------------------------------------------------------------------------------------------------------------------------------------------ */
/* four lanes of the lane loop: att0/1/2 = the pairs of H, M, L for the lanes; returns st, dX, dY, dE as (int, frac) vectors */
struct pr4 {
    int16x4_t i;
    uint16x4_t f;
};
NI int32x4_t pack32(int16x4_t i, uint16x4_t f)
{
    uint16x4x2_t z = vzip_u16(f, vreinterpret_u16_s16(i));
    return vreinterpretq_s32_u16(vcombine_u16(z.val[0], z.val[1]));
}
NI void unpack32(int32x4_t v, int16x4_t *i, uint16x4_t *f)
{
    uint16x8_t u = vreinterpretq_u16_s32(v);
    uint16x4_t lo = vget_low_u16(u), hi = vget_high_u16(u);
    uint16x4x2_t z = vuzp_u16(lo, hi);
    *f = z.val[0];
    *i = vreinterpret_s16_u16(z.val[1]);
}
/* VSUBC / VSUB on pairs: the integer half saturates */
NI void fsub4(int16x4_t ai, uint16x4_t af, int16x4_t bi, uint16x4_t bf, int16x4_t *ri, uint16x4_t *rf)
{
    uint16x4_t borrow = vclt_u16(af, bf);                                   /* 0xffff where af < bf */
    int32x4_t d = vsubl_s16(ai, bi);
    d = vaddq_s32(d, NMUT(1, vmovl_s16(vreinterpret_s16_u16(borrow)), vdupq_n_s32(0)));      /* - borrow */
    *ri = vqmovn_s32(d);
    *rf = vsub_u16(af, bf);
}

/* The lane loop for NH halves of four lanes (lanes lo0 .. lo0 + 4 NH - 1), the stages of the halves interleaved so that the chains of dependent multiplies of the two overlap (the in-order cores are
 * latency bound). att0/1/2 = the pairs of H, M, L for the lanes; returns st, dX, dY, dE as (int, frac) vectors. */
NI void attrs_n(const int NH, const struct fx2_pair att[3][8], unsigned lo0, struct fx2_pair R1, struct fx2_pair sl, struct fx2_pair C0, int16_t dyMH, int16_t nyLH, int16_t dxLH, int16_t nxMH,
                struct pr4 *stv, struct pr4 *dX, struct pr4 *dY, struct pr4 *dE)
{
    int16x4_t a0i[2], a1i[2], a2i[2], dLHi[2], dMHi[2], nxi[2], nyi[2];
    uint16x4_t a0f[2], a1f[2], a2f[2], dLHf[2], dMHf[2], nxf[2], nyf[2];
    int32x4_t pvLH[2], pvMH[2], nx32[2], ny32[2];
    v64 a1[2], a2[2], t[2], full[2], t2[2], f2[2], z = v64_zero();
    int h;

    for (h = 0; h < NH; h++) {                          /* the (int, frac) pairs of 4 lanes of each vertex: vld2 de-interleaves them */
        int16x4x2_t v0 = vld2_s16((const int16_t *)&att[0][lo0 + 4 * h]), v1 = vld2_s16((const int16_t *)&att[1][lo0 + 4 * h]), v2 = vld2_s16((const int16_t *)&att[2][lo0 + 4 * h]);
        a0i[h] = v0.val[0];
        a0f[h] = vreinterpret_u16_s16(v0.val[1]);
        a1i[h] = v1.val[0];
        a1f[h] = vreinterpret_u16_s16(v1.val[1]);
        a2i[h] = v2.val[0];
        a2f[h] = vreinterpret_u16_s16(v2.val[1]);
    }
    for (h = 0; h < NH; h++) {
        fsub4(a2i[h], a2f[h], a0i[h], a0f[h], &dLHi[h], &dLHf[h]);
        fsub4(a1i[h], a1f[h], a0i[h], a0f[h], &dMHi[h], &dMHf[h]);
    }
    for (h = 0; h < NH; h++) {
        pvLH[h] = pack32(dLHi[h], dLHf[h]);
        pvMH[h] = pack32(dMHi[h], dMHf[h]);
    }
    for (h = 0; h < NH; h++) {                          /* a1 = pv(dLH) * dyMH + pv(dMH) * nyLH; a2 = pv(dMH) * dxLH + pv(dLH) * nxMH, 48-bit */
        a1[h].lo = vmlal_n_s32(vmull_n_s32(vget_low_s32(pvLH[h]), dyMH), vget_low_s32(pvMH[h]), nyLH);
        a1[h].hi = vmlal_n_s32(vmull_n_s32(vget_high_s32(pvLH[h]), dyMH), vget_high_s32(pvMH[h]), nyLH);
        a2[h].lo = vmlal_n_s32(vmull_n_s32(vget_low_s32(pvMH[h]), dxLH), vget_low_s32(pvLH[h]), nxMH);
        a2[h].hi = vmlal_n_s32(vmull_n_s32(vget_high_s32(pvMH[h]), dxLH), vget_high_s32(pvLH[h]), nxMH);
    }
    for (h = 0; h < NH; h++) {
        a1[h] = v64_wrap48(a1[h]);
        a2[h] = v64_wrap48(a2[h]);
    }
    for (h = 0; h < NH; h++) {                          /* nx = (a1 >> 32, a1 >> 16): the low 32 bits of a1 >> 16 as a pair */
        nx32[h] = vcombine_s32(vmovn_s64(vshrq_n_s64(a1[h].lo, 16)), vmovn_s64(vshrq_n_s64(a1[h].hi, 16)));
        ny32[h] = vcombine_s32(vmovn_s64(vshrq_n_s64(a2[h].lo, 16)), vmovn_s64(vshrq_n_s64(a2[h].hi, 16)));
    }
    for (h = 0; h < NH; h++) {
        unpack32(nx32[h], &nxi[h], &nxf[h]);
        unpack32(ny32[h], &nyi[h], &nyf[h]);
    }
    for (h = 0; h < NH; h++) {
        mm4s(z, nxi[h], nxf[h], R1.i, R1.f, &t[h], &full[h]);
        mm4s(z, nyi[h], nyf[h], R1.i, R1.f, &t2[h], &f2[h]);
    }
    for (h = 0; h < NH; h++) {
        dX[h].i = sclamp4(full[h]);
        dX[h].f = uclamp4(t[h]);
        dY[h].i = sclamp4(f2[h]);
        dY[h].f = uclamp4(t2[h]);
    }
    for (h = 0; h < NH; h++)                            /* dE = dY + dX * DxHDy, on top of the accumulator the dY chain left */
        mm4s(f2[h], dX[h].i, dX[h].f, sl.i, sl.f, &t[h], &full[h]);
    for (h = 0; h < NH; h++) {
        dE[h].i = sclamp4(full[h]);
        dE[h].f = uclamp4(t[h]);
    }
    for (h = 0; h < NH; h++)                            /* st = att0 + dE * C */
        mm4s(v64_from_s32(pack32(a0i[h], a0f[h])), dE[h].i, dE[h].f, C0.i, C0.f, &t[h], &full[h]);
    for (h = 0; h < NH; h++) {
        stv[h].i = sclamp4(full[h]);
        stv[h].f = uclamp4(t[h]);
    }
}

NI void put_be16x4(uint8_t *p, int16x4_t v)
{
    vst1_u16((uint16_t *)p, vrev32_u16(vreinterpret_u16_s16(v)));          /* the host words of DMEM: the halves of each word swapped */
}

unsigned fx2_tri_attrs_neon(const struct fx2_tri_in *in, const struct fx2_tri_state *st, uint32_t *cmd)
{
    uint32_t g = in->g;
    unsigned shade = g & 4, zb = g & 1, tex = in->tex, o = 32, k;
    struct fx2_pair att[3][8], R1;
    struct pr4 s4[2], x4[2], y4[2], e4[2];
    unsigned need = fx2_tri_attr_prep(in, st, att, &R1);
    unsigned nh = (need & 0x0f ? 1 : 0) | (need & 0xf0 ? 2 : 0);

    if (nh == 3)
        attrs_n(2, att, 0, R1, st->slope[2], st->C0, st->dyMH, st->nyLH, st->dxLH, st->nxMH, s4, x4, y4, e4);
    else if (nh == 1)
        attrs_n(1, att, 0, R1, st->slope[2], st->C0, st->dyMH, st->nyLH, st->dxLH, st->nxMH, &s4[0], &x4[0], &y4[0], &e4[0]);
    else if (nh == 2)
        attrs_n(1, att, 4, R1, st->slope[2], st->C0, st->dyMH, st->nyLH, st->dxLH, st->nxMH, &s4[1], &x4[1], &y4[1], &e4[1]);
    for (k = 0; k < 2; k++) {
        unsigned base;
        if (k == 0) {
            if (!shade)
                continue;
            base = o;
        } else {
            if (!tex)
                continue;
            base = o + (shade ? 64 : 0);
        }
        put_be16x4((uint8_t *)cmd + base, s4[k].i);
        put_be16x4((uint8_t *)cmd + base + 8, x4[k].i);
        put_be16x4((uint8_t *)cmd + base + 16, vreinterpret_s16_u16(s4[k].f));
        put_be16x4((uint8_t *)cmd + base + 24, vreinterpret_s16_u16(x4[k].f));
        put_be16x4((uint8_t *)cmd + base + 32, e4[k].i);
        put_be16x4((uint8_t *)cmd + base + 40, y4[k].i);
        put_be16x4((uint8_t *)cmd + base + 48, vreinterpret_s16_u16(e4[k].f));
        put_be16x4((uint8_t *)cmd + base + 56, vreinterpret_s16_u16(y4[k].f));
    }
    o += (shade ? 64 : 0) + (tex ? 64 : 0);
    if (zb) {                                           /* lane 7 of the second half: z */
        struct fx2_pair dX7 = { x4[1].i[3], x4[1].f[3] }, dY7 = { y4[1].i[3], y4[1].f[3] }, dE7 = { e4[1].i[3], e4[1].f[3] }, st7 = { s4[1].i[3], s4[1].f[3] };
        struct fx2_pair zst, zx, zy, ze;
        hle_acc a;
        uint16_t v10 = (uint16_t)((uint32_t)dE7.f * (uint32_t)(int32_t)(int16_t)st->C0.f);
        a = (hle_acc)dX7.f * 32 + (((hle_acc)dX7.i * 32) << 16);
        zx.i = hle_sclamp(a);
        zx.f = (uint16_t)(dX7.f * 32);
        a = (hle_acc)dY7.f * 32 + (((hle_acc)dY7.i * 32) << 16);
        zy.i = hle_sclamp(a);
        zy.f = (uint16_t)(dY7.f * 32);
        a = (hle_acc)dE7.f * 32 + (((hle_acc)dE7.i * 32) << 16);
        ze.i = hle_sclamp(a);
        ze.f = (uint16_t)(dE7.f * 32);
        a = (hle_acc)(((uint32_t)v10 * 32) >> 16) + (hle_acc)st7.f * 32 + (((hle_acc)st7.i * 32) << 16);
        zst.i = hle_sclamp(a);
        zst.f = hle_uclamp(a - (((hle_acc)st7.i * 32) << 16));      /* before the integer product (see f3dex2_fx.c) */
        {
            uint16_t zv[8] = { (uint16_t)zst.i, zst.f, (uint16_t)zx.i, zx.f, (uint16_t)ze.i, ze.f, (uint16_t)zy.i, zy.f };
            vst1q_u16((uint16_t *)((uint8_t *)cmd + o), vrev32q_u16(vld1q_u16(zv)));
        }
        o += 16;
    }
    return o;
}

int fx2_tri_setup(const struct fx2_tri_in *in, uint32_t *cmd, unsigned *len)
{
    struct fx2_tri_state st;
    int r;
    if (use_scalar())
        return fx2_tri_setup_scalar(in, cmd, len);
    r = fx2_tri_prepare(in, cmd, &st);
    if (r != FX2_TRI_DRAW)
        return r;
    *len = (in->g & 4) || in->tex || (in->g & 1) ? fx2_tri_attrs_neon(in, &st, cmd) : 32;
    return FX2_TRI_DRAW;
}


/* ---- lighting ------------------------------------------------------------------------------------------------------------------------------------------------------------------- */
/* The lit colour is the scalar form: a NEON form (the dot products with the horizontal adds, the colours in lanes) was measured on the A55 at 86 ns a vertex against the scalar form's 62. */
NI void lit_vertex(const struct fx2_vctx *cx, unsigned g, const int8_t nb[3], uint8_t alpha, uint8_t out[4], int texgen, int16_t sgen[2])
{
    fx2_lit_vertex_scalar(cx, g, nb, alpha, out, texgen, sgen);
}

/* ---- vertices ------------------------------------------------------------------------------------------------------------------------------------------------------------------ */
/* a = ((f * k) >> 16) + i * k per lane, k an unsigned 16-bit scalar (the perspective normalisation) */
NI v64 mulk4(int16x4_t i, uint16x4_t f, uint16_t k)
{
    uint32x4_t p1 = vshrq_n_u32(vmull_n_u16(f, k), 16);
    int32x4_t p2 = vmulq_n_s32(vmovl_s16(i), (int32_t)k);
    return v64_addw_s32(v64_from_s32(p2), vreinterpretq_s32_u32(p1));
}

/* the clip-flag bits of the four lanes of X against W (lane 3 of W4): bit k "le" of lane k, bit 8 + k "ge" */
NI unsigned clipbits4(int32x4_t X, int32x4_t W)
{
    static const uint32_t wl[4] = { 1, 2, 4, 8 }, wg[4] = { 0x100, 0x200, 0x400, 0x800 };
    uint32x4_t xneg = vcltzq_s32(X), wneg = vcltzq_s32(W);
    uint32x4_t opp = veorq_u32(xneg, wneg);
    uint32x4_t le_o = vcleq_s32(vaddq_s32(X, W), vdupq_n_s32(NMUT(5, 0, -1)));
    uint32x4_t ge_o = vcgeq_s32(vsubq_s32(X, W), vdupq_n_s32(0));
    uint32x4_t le = vbslq_u32(opp, le_o, wneg), ge = vbslq_u32(opp, wneg, ge_o);
    return vaddvq_u32(vorrq_u32(vandq_u32(le, vld1q_u32(wl)), vandq_u32(ge, vld1q_u32(wg))));
}

/* P = the clip coordinates: (Mi[3] << 16) + Mf[3] + sum over j < 3 of Mf[j] v[j] + (Mi[j] v[j]) << 16, per column k (the four lanes) */
NI void vtx_P4(const struct fx2_mtx *mp, const int16_t v[3], int16x4_t *Pi, uint16x4_t *Pf)
{
    v64 acc = v64_from_s32(pack32(vld1_s16(mp->i[3]), vld1_u16(mp->f[3])));
    v64 last = v64_zero();
    unsigned j;
    for (j = 0; j < 3; j++) {
        int16x4_t mi = vld1_s16(mp->i[j]);
        uint16x4_t mf = vld1_u16(mp->f[j]);
        int32x4_t pf = vmulq_n_s32(vreinterpretq_s32_u32(vmovl_u16(mf)), (int32_t)v[j]);
        int32x4_t pi = vmull_n_s16(mi, v[j]);
        last = v64_shl16_s32(pi);
        acc = v64_wrap48(v64_add(v64_addw_s32(acc, pf), last));
    }
    *Pi = sclamp4(acc);
    {   /* uclamp(wrap48(acc - last)) */
        v64 d = { vsubq_s64(acc.lo, last.lo), vsubq_s64(acc.hi, last.hi) };
        *Pf = uclamp4(v64_wrap48(d));
    }
}

/* The vertices in stages, so that the serial chains of the scalar reciprocal (rcp, three multiplies) of different vertices overlap: stage 1 the matrix product, the clip flags and Q = P * pn per vertex,
 * stage 2 the reciprocal of Q's w, stage 3 T = Q3 R0 and E = 4 - 4T, stage 4 R1 = E R0, stage 5 the projection and the viewport (NEON again). The results are those of vtx_project in f3dex2_fx.c. */
#define XFORM_MAX 34

void fx2_vtx_xform(const struct fx2_vctx *c, const struct fx2_vin *in, unsigned n, struct fx2_vtx *out)
{
    unsigned vcc_a[XFORM_MAX], vcc_b[XFORM_MAX], i, nn = n + (n & 1);
    int16x4_t Pi[XFORM_MAX];
    uint16x4_t Pf[XFORM_MAX];
    int16_t Qi3[XFORM_MAX], T_i[XFORM_MAX], R1i[XFORM_MAX];
    uint16_t Qf3[XFORM_MAX], T_f[XFORM_MAX], R1f[XFORM_MAX];
    uint32_t R0[XFORM_MAX];
    uint16x4_t Pf_dummy;
    (void)Pf_dummy;

    if (use_scalar()) {
        fx2_vtx_xform_scalar(c, in, n, out);
        return;
    }
    {   /* stage 1 */
        const int16_t sc[4] = { c->scale[0], c->scale[1], c->scale[2], c->scale[3] };
        (void)sc;
    }
    for (i = 0; i < nn; i++) {
        const struct fx2_vin *vi = &in[i];
        int16_t v[3] = { vi->x, vi->y, vi->z };
        int32x4_t X, W;
        int16_t P3i;
        uint16_t P3f, P2f;
        hle_acc prod;
        int32_t W2;
        int32x4_t X2;
        vtx_P4(&c->mp, v, &Pi[i], &Pf[i]);
        X = pack32(Pi[i], Pf[i]);
        W = vdupq_laneq_s32(X, 3);
        P3i = vget_lane_s16(Pi[i], 3);
        P3f = vget_lane_u16(Pf[i], 3);
        P2f = vget_lane_u16(Pf[i], 2);
        prod = (hle_acc)P3f * c->ratio + (((hle_acc)P3i * c->ratio) << 16);
        W2 = (int32_t)((uint32_t)(uint16_t)hle_sclamp(prod) << 16 | (uint16_t)prod);
        X2 = vsetq_lane_s32((int32_t)((uint32_t)(uint16_t)P3i << 16 | P2f), X, 3);
        vcc_a[i] = clipbits4(X, W);
        vcc_b[i] = clipbits4(X2, vdupq_n_s32(W2));
        if (i < n) {
            v64 Q = mulk4(Pi[i], Pf[i], c->pn);
            int64_t q3 = vgetq_lane_s64(Q.hi, 1);             /* lane 3 only */
            Qi3[i] = hle_sclamp(q3);
            Qf3[i] = hle_uclamp(q3);
            R0[i] = hle_rcp32((int32_t)((uint32_t)(uint16_t)Qi3[i] << 16 | Qf3[i]));
        }
    }
#pragma GCC unroll 4
    for (i = 0; i < n; i++) {                           /* T = Q3 * R0; E = 4 - 4 T */
        int16_t R0i = (int16_t)(R0[i] >> 16);
        uint16_t R0f = (uint16_t)R0[i];
        hle_acc tt = hle_wrap48(((hle_acc)Qf3[i] * R0f >> 16) + (hle_acc)Qi3[i] * R0f + (hle_acc)Qf3[i] * R0i);
        hle_acc fu = hle_wrap48(tt + (((hle_acc)Qi3[i] * R0i) << 16));
        T_i[i] = hle_sclamp(fu);
        T_f[i] = hle_uclamp(tt);
    }
#pragma GCC unroll 4
    for (i = 0; i < n; i++) {                           /* R1 = E * R0 */
        int16_t R0i = (int16_t)(R0[i] >> 16), Ei;
        uint16_t R0f = (uint16_t)R0[i], Ef;
        hle_acc e1 = ((hle_acc)4 << 16) - (hle_acc)4 * T_f[i];
        hle_acc e2 = e1 - (((hle_acc)4 * T_i[i]) << 16);
        hle_acc t2, f2;
        Ei = hle_sclamp(e2);
        Ef = hle_uclamp(e1);
        t2 = hle_wrap48(((hle_acc)Ef * R0f >> 16) + (hle_acc)Ei * R0f + (hle_acc)Ef * R0i);
        f2 = hle_wrap48(t2 + (((hle_acc)Ei * R0i) << 16));
        R1i[i] = hle_sclamp(f2);
        R1f[i] = hle_uclamp(t2);
    }
    {
        int16_t sc[4] = { c->scale[0], c->scale[1], c->scale[2], c->scale[3] };
        int32_t base32[4] = { c->trans[0], c->trans[1], c->trans[2], c->trans[3] + (int32_t)c->c31_3 };
        v64 b = v64_shl16_s32(vld1q_s32(base32)), z = v64_zero();
        int16x4_t scale = vld1_s16(sc);
        for (i = 0; i < n; i++) {                       /* stage 5 and the rest of the record */
            const struct fx2_vin *vi = &in[i];
            struct fx2_vtx *o = &out[i];
            int16_t s = vi->s, tt = vi->t, fog;
            uint8_t col[4] = { vi->c[0], vi->c[1], vi->c[2], vi->c[3] };
            int16_t rint = vget_lane_s16(Pi[i], 3) >= 0 ? R1i[i] : 0x7fff;
            v64 t, fl, n2, t1, f1;
            int16x4_t ndci, nni, Si;
            uint16x4_t ndcf, nni_f, Sf;
            int32x4_t pf;
            if ((c->mode & FX2_LIGHTING)) {
                int8_t nb[3] = { (int8_t)vi->c[0], (int8_t)vi->c[1], (int8_t)vi->c[2] };
                uint8_t lo[4];
                int16_t sg[2] = { 0, 0 };
                lit_vertex(c, (i & 1) ? 0 : 1, nb, vi->c[3], lo, (c->mode & FX2_TEXGEN) != 0, sg);
                memcpy(col, lo, 4);
                if (c->mode & FX2_TEXGEN) {
                    s = sg[0];
                    tt = sg[1];
                }
            }
            mm4s(z, Pi[i], Pf[i], rint, R1f[i], &t, &fl);
            ndci = sclamp4(fl);
            ndcf = uclamp4(t);
            n2 = mulk4(ndci, ndcf, c->pn);
            {
                int16x4_t ni = sclamp4(n2);
                uint16x4_t nf = uclamp4(n2);
                nni = vcopy_lane_s16(ni, 3, ni, 2);
                nni_f = vcopy_lane_u16(nf, 3, nf, 2);
            }
            pf = vmulq_s32(vreinterpretq_s32_u32(vmovl_u16(nni_f)), vmovl_s16(scale));
            t1 = v64_wrap48(v64_addw_s32(b, pf));
            f1 = v64_wrap48(v64_add(t1, v64_shl16_s32(vmull_s16(nni, scale))));
            Si = sclamp4(f1);
            Sf = uclamp4(t1);
            vst1_s16(o->cx, Pi[i]);
            vst1_u16(o->cf, Pf[i]);
            o->sx = vget_lane_s16(Si, 0);
            o->sy = vget_lane_s16(Si, 1);
            {
                int16_t z2 = vget_lane_s16(Si, 2);
                o->szi = z2 < 0 ? 0 : z2;
            }
            o->szf = vget_lane_u16(Sf, 2);
            o->rwi = R1i[i];
            o->rwf = R1f[i];
            fog = vget_lane_s16(Si, 3);
            if (c->mode & FX2_FOG)
                col[3] = (uint8_t)(fog < 0x7f00 ? 0x7f00 : fog);
            memcpy(o->col, col, 4);
            o->s = (int16_t)(((int32_t)s * c->sc_s) >> 16);
            o->t = (int16_t)(((int32_t)tt * c->sc_t) >> 16);
            (void)Sf;
        }
    }
    for (i = 0; i < n; i++) {
        unsigned pa = i & ~1u, pb = pa + 1;
        unsigned ca = (vcc_a[pa] & 0x0f0f) | ((vcc_a[pb] & 0x0f0f) << 4);
        unsigned cb = (vcc_b[pa] & 0x0f0f) | ((vcc_b[pb] & 0x0f0f) << 4);
        out[i].flags = (i & 1) ? (uint32_t)(cb << 16 | ca) : (uint32_t)(((cb << 4) & 0xffff) << 16 | ((ca << 4) & 0xffff));
    }
}

/* ---- matrices ------------------------------------------------------------------------------------------------------------------------------------------------------------------- */
void fx2_mtx_mul(const struct fx2_mtx *a, const struct fx2_mtx *b, struct fx2_mtx *out)
{
    struct fx2_mtx r;
    unsigned i, k;
    if (use_scalar()) {
        fx2_mtx_mul_scalar(a, b, out);
        return;
    }
    for (i = 0; i < 4; i++) {                           /* row i of the result: the four columns are the lanes */
        v64 acc = v64_zero(), last = v64_zero();
        for (k = 0; k < 4; k++) {
            int16x4_t bi = vld1_s16(b->i[k]);
            uint16x4_t bf = vld1_u16(b->f[k]);
            uint16_t af = a->f[i][k];
            int16_t ai = a->i[i][k];
            uint32x4_t p1 = vshrq_n_u32(vmull_n_u16(bf, af), 16);
            int32x4_t p2 = vmulq_n_s32(vmovl_s16(bi), (int32_t)af);
            int32x4_t p3 = vmulq_n_s32(vreinterpretq_s32_u32(vmovl_u16(bf)), (int32_t)ai);
            last = v64_shl16_s32(vmull_n_s16(bi, ai));
            acc = v64_addw_s32(v64_addw_s32(v64_addw_s32(acc, vreinterpretq_s32_u32(p1)), p2), p3);
            acc = v64_wrap48(v64_add(acc, last));
        }
        vst1_s16(r.i[i], sclamp4(acc));
        if (i < 2) {                                    /* rows 0 and 1: the fraction is read after the last term, rows 2 and 3 before its integer product */
            vst1_u16(r.f[i], uclamp4(acc));
        } else {
            v64 d = { vsubq_s64(acc.lo, last.lo), vsubq_s64(acc.hi, last.hi) };
            vst1_u16(r.f[i], uclamp4(v64_wrap48(d)));
        }
    }
    *out = r;
}

#endif
