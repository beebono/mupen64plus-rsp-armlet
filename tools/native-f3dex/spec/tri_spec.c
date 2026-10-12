/*
 * tri_spec.c: a triangle of the F3DEX 1.x family (entry: the triangle code reached from G_TRI1 / G_TRI2), as plain integer arithmetic
 * (docs/ucode-f3dex.md, "Triangles"). Checked against the JIT by tools/native-f3dex/rt_test: DMEM, $23 and the exit pc are compared.
 * Not covered: the clipping overlay (a triangle that needs clipping makes the spec routine decline).
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "spec.h"

extern const uint16_t rsp_reciprocal_rom[1024];

typedef int64_t acc_t;

static inline int16_t sclamp(acc_t a)
{
    acc_t v = a >> 16;
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
}
static inline uint16_t uclamp(acc_t a)
{
    if (a >= -(acc_t)2147483648ll && a < (acc_t)2147483648ll)
        return (uint16_t)a;
    return a < 0 ? 0 : 0xffff;
}
static inline int16_t clamp16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }
static inline int16_t sat_sub(int16_t a, int16_t b) { return clamp16((int32_t)a - b); }

static uint32_t rcp32(int32_t input)
{
    int32_t input_mask = input >> 31, data = input ^ input_mask, result;
    unsigned shift, idx;
    if (input > -32768)
        data -= input_mask;
    if (data == 0)
        return 0x7fffffffu;
    if (input == -32768)
        return 0xffff0000u;
    shift = __builtin_clz(data);
    idx = (((unsigned long long)data << shift) & 0x7FC00000u) >> 22;
    result = rsp_reciprocal_rom[idx];
    result = ((0x10000 | result) << 14) >> (31 - shift);
    return (uint32_t)(result ^ input_mask);
}

/* 32-bit fixed-point pair */
struct fx { int16_t i; uint16_t f; };

/* floor(A * c / 65536) for an unsigned 16-bit c: [VMUDL, VMADM, VMADN] */
static struct fx mul_c(struct fx a, uint16_t c)
{
    acc_t R = (acc_t)(((int64_t)a.f * c) >> 16) + (acc_t)a.i * c;
    struct fx r = { sclamp(R), uclamp(R) };
    return r;
}
/* floor(A * B / 65536), integer half from the full chain, fraction half after the last term ([VMUDL, VMADM, VMADN, VMADH, VMADN 0]) */
static struct fx mul_fx(struct fx a, struct fx b)
{
    acc_t R = (acc_t)(((int64_t)a.f * b.f) >> 16) + (acc_t)a.i * b.f + (acc_t)a.f * b.i + (((acc_t)a.i * b.i) << 16);
    struct fx r = { sclamp(R), uclamp(R) };
    return r;
}
static struct fx sub_fx(struct fx a, struct fx b)      /* VSUBC / VSUB */
{
    struct fx r;
    r.f = (uint16_t)(a.f - b.f);
    r.i = clamp16((int32_t)a.i - b.i - (a.f < b.f));
    return r;
}
static struct fx neg_fx(struct fx a)
{
    struct fx z = { 0, 0 };
    return sub_fx(z, a);
}

static inline acc_t w48(acc_t a) { return (acc_t)((uint64_t)a << 16) >> 16; }
static inline acc_t pv(struct fx a) { return (acc_t)a.i * 65536 + a.f; }
/* [VMUDL, VMADM, VMADN, VMADH] without a final VMADN 0: the fraction is read before the integer x integer term */
static struct fx mul_mid(struct fx a, struct fx b)
{
    acc_t t = (acc_t)(((int64_t)a.f * b.f) >> 16) + (acc_t)a.i * b.f + (acc_t)a.f * b.i;
    acc_t full = t + (((acc_t)a.i * b.i) << 16);
    struct fx r = { sclamp(w48(full)), uclamp(w48(t)) };
    return r;
}
/* x (signed 16) times a 32-bit pair: [VMUDM, VMADH, VMADN 0] */
static struct fx mul_x(int16_t x, struct fx k)
{
    acc_t R = w48((acc_t)x * pv(k));
    struct fx r = { sclamp(R), uclamp(R) };
    return r;
}
/* pair times a signed 16-bit constant: [VMUDN, VMADH, VMADN 0] */
static struct fx mul_s(struct fx a, int16_t c)
{
    acc_t R = w48(pv(a) * c);
    struct fx r = { sclamp(R), uclamp(R) };
    return r;
}
static inline int16_t vabs16(int16_t x) { return x == -32768 ? 32767 : x < 0 ? -x : x; }
static struct fx max32(struct fx a, struct fx b) { return pv(a) >= pv(b) ? a : b; }
static struct fx min32(struct fx a, struct fx b) { return pv(a) < pv(b) ? a : b; }

/* the refined reciprocal of a 32-bit value (docs/ucode-f3dex.md 1.6) */
static struct fx recip_refine(struct fx W)
{
    int32_t w = (int32_t)((uint32_t)(uint16_t)W.i << 16 | W.f);
    uint32_t r = rcp32(w);
    int16_t ri_ = (int16_t)(r >> 16); uint16_t rf_ = (uint16_t)r;
    acc_t a = (acc_t)rf_ * 2 + (((acc_t)ri_ * 2) << 16);
    struct fx D = { sclamp(a), uclamp(a) };
    acc_t a1 = (acc_t)(((int64_t)D.f * W.f) >> 16) + (acc_t)D.i * W.f + (acc_t)D.f * W.i;
    uint16_t t_frac = uclamp(a1);
    int16_t t_int = sclamp(a1 + (((acc_t)D.i * W.i) << 16));
    struct fx E = { clamp16(2 - (int32_t)t_int - (0 < t_frac)), (uint16_t)(0 - t_frac) };
    return mul_mid(D, E);
}
static inline int16_t S16(const struct armlet_rsp *R, unsigned a) { return (int16_t)armlet_dm16(R, a); }

struct tk {
    uint32_t rej_mask;              /* 0x7070, NoN 0x7030 */
    unsigned exit_pc;
    int dlx;                        /* F3DLX: optional clipping, DPC_END written here, no sub-pixel correction, other coefficient layout order */
};

/* (x, y) of a vertex record: screen position, 10.2 */
static inline int16_t vx(const struct armlet_rsp *R, unsigned v) { return S16(R, v + 0x18); }
static inline int16_t vy(const struct armlet_rsp *R, unsigned v) { return S16(R, v + 0x1a); }

static int spec_tri(struct armlet_env *e, const struct tk *K)
{
    const struct armlet_rsp *R = &e->rsp;
    uint32_t *sr = e->rsp.sr;
    unsigned v[3] = { sr[1] & 0xfff, sr[2] & 0xfff, sr[3] & 0xfff };
    unsigned ret = sr[24] & 0xfff;
    unsigned f0 = (uint16_t)armlet_dm16(R, v[0] + 0x24), f1 = (uint16_t)armlet_dm16(R, v[1] + 0x24), f2 = (uint16_t)armlet_dm16(R, v[2] + 0x24);
    uint32_t g = armlet_dm32(R, 0x114);

    /* trivially rejected: all three vertices outside the same clip plane */
    int clip_on = !K->dlx || (g & 0x800000);              /* F3DLX tests the clip flags only with G_CLIPPING set */
    if (clip_on && (f0 & f1 & f2 & K->rej_mask))
        return (int)ret;
    /* needs the clipping overlay */
    if (clip_on && ((f0 | f1 | f2) & 0x4343))
        return ARMLET_NATIVE_DECLINE;

    int16_t x[3], y[3];
    for (int i = 0; i < 3; i++) { x[i] = vx(R, v[i]); y[i] = vy(R, v[i]); }

    /* the signed area (twice) as the sum of two products of saturated differences */
    int16_t dx20 = sat_sub(x[2], x[0]), dy10 = sat_sub(y[1], y[0]), dx01 = sat_sub(x[0], x[1]), dy20 = sat_sub(y[2], y[0]);
    int64_t cross64 = (int64_t)dx20 * dy10 + (int64_t)dx01 * dy20;
    int32_t cross = (int32_t)cross64;                           /* the 48-bit accumulator keeps 32 bits of the sum in the VSAR readouts */
    int32_t r17 = cross64 > 32767 ? 32767 : cross64 < -32768 ? -32768 : (int32_t)cross64;     /* sign / zero test value */
    if ((g & 0x2000) && r17 < 0)                                 /* G_CULL_BACK */
        return (int)ret;

    /* scratch: the three vertex addresses, halfwords at 0xbe0 */
    armlet_st16(R, 0xbe0, (uint16_t)v[0]);
    armlet_st16(R, 0xbe2, (uint16_t)v[1]);
    armlet_st16(R, 0xbe4, (uint16_t)v[2]);
    int idx = 4 * (y[2] < y[1]) + 2 * (y[1] < y[0]) + (y[0] < y[2]);
    unsigned t3 = armlet_dm8(R, 0x360 + idx), t2 = armlet_dm8(R, 0x367 + idx), t1 = armlet_dm8(R, 0x36e + idx);
    int parity = armlet_dm8(R, 0x375 + idx) > 0;
    unsigned H = armlet_dm16(R, 0xbe0 + t1), M = armlet_dm16(R, 0xbe0 + t2), L = armlet_dm16(R, 0xbe0 + t3);
    sr[1] = H; sr[2] = M; sr[3] = L;

    struct fx area = { (int16_t)(cross >> 16), (uint16_t)cross };
    if (parity)
        area = neg_fx(area);
    if (r17 == 0)                                                /* degenerate */
        return (int)ret;
    if ((g & 0x1000) && r17 >= 0)                                /* G_CULL_FRONT */
        return (int)ret;

    if (K->dlx)                                                  /* F3DLX: the RDP is told about the data flushed so far */
        armlet_mtc0(e, 9, armlet_dm32(R, 0x128));

    /* ---- the edge coefficients ---- */
    int16_t xH = vx(R, H), yH = vy(R, H), xM = vx(R, M), yM = vy(R, M), xL = vx(R, L), yL = vy(R, L);
    int16_t dxMH = sat_sub(xM, xH), dyMH = sat_sub(yM, yH), dxLH = sat_sub(xL, xH), dyLH = sat_sub(yL, yH), dxLM = sat_sub(xL, xM), dyLM = sat_sub(yL, yM);
    unsigned out = sr[23];
    uint8_t b0 = (uint8_t)(((g & 0xff) | 0xc8)), b1 = (uint8_t)(armlet_dm8(R, 0x122) | (area.i < 0 ? 0x80 : 0));
    armlet_st8(R, out + 0, b0);
    armlet_st8(R, out + 1, b1);
    armlet_st16(R, out + 2, (uint16_t)yL);
    armlet_st16(R, out + 4, (uint16_t)yM);
    armlet_st16(R, out + 6, (uint16_t)yH);

    /* reciprocal of each dy (16-bit input), scaled by 32/65536; delta x in 16.16 pixels (x / 4) */
    int16_t dy[3] = { dyLM, dyMH, dyLH }, dx[3] = { dxLM, dxMH, dxLH };
    struct fx slope[3], slope_m[3];
    uint16_t c_sub = (uint16_t)(((uint32_t)(uint16_t)yH * 0x4000u) & 0xffff);
    for (int k = 0; k < 3; k++) {
        uint32_t rc = rcp32((int32_t)dy[k]);
        struct fx A = { (int16_t)(rc >> 16), (uint16_t)rc };
        A = mul_c(A, armlet_dm16(R, 0x48));                       /* v30[4] = 0x0020 */
        struct fx B = { (int16_t)(((int32_t)dx[k] * 0x4000) >> 16), (uint16_t)((int32_t)dx[k] * 0x4000) };
        struct fx S = K->dlx ? mul_mid(A, B) : mul_fx(A, B);       /* F3DLX has no final VMADN 0: the fraction is read before the integer term */
        S = mul_c(S, armlet_dm16(R, 0x38));                       /* v31[4] = 0x4000: divide by 4 */
        slope[k] = S;
        /* the integer half is limited to +-0x1cc (VCR with v30[6]) */
        int16_t lim = (int16_t)armlet_dm16(R, 0x4c);
        int16_t si = S.i;
        if (si > lim) si = lim;
        if (si < ~lim) si = ~lim;
        slope[k].i = si;
        struct fx Sm = { si, (uint16_t)(S.f & armlet_dm16(R, 0x42)) };   /* the fraction with the low 3 bits cleared (v30[1] = 0xfff8) */
        slope_m[k] = mul_c(Sm, c_sub);
    }
    struct fx xMf = { (int16_t)(((int32_t)xM * 0x4000) >> 16), (uint16_t)((int32_t)xM * 0x4000) };
    struct fx xHf = { (int16_t)(((int32_t)xH * 0x4000) >> 16), (uint16_t)((int32_t)xH * 0x4000) };
    struct fx XM = sub_fx(xHf, slope_m[1]), XH = sub_fx(xHf, slope_m[2]);
    if (K->dlx) { XM = xHf; XH = xHf; }                          /* F3DLX: no sub-pixel correction */
    /* XL: the middle vertex x as it is; DxLDy, XH, DxHDy, XM, DxMDy */
    armlet_st16(R, out + 8, (uint16_t)xMf.i);
    armlet_st16(R, out + 10, xMf.f);
    armlet_st16(R, out + 12, (uint16_t)slope[0].i);
    armlet_st16(R, out + 14, slope[0].f);
    armlet_st16(R, out + 16, (uint16_t)XH.i);
    armlet_st16(R, out + 18, XH.f);
    armlet_st16(R, out + 20, (uint16_t)slope[2].i);
    armlet_st16(R, out + 22, slope[2].f);
    armlet_st16(R, out + 24, (uint16_t)XM.i);
    armlet_st16(R, out + 26, XM.f);
    armlet_st16(R, out + 28, (uint16_t)slope[1].i);
    armlet_st16(R, out + 30, slope[1].f);
    out += 32;
    sr[23] = out;
    unsigned flags = g & 7;              /* 4 shade, 2 texture, 1 z buffer */
    if (!flags)
        return (int)K->exit_pc;

    /* ---- attributes: lanes 0..3 colour, 4..6 s t w, 7 z; per vertex H M L as 32-bit pairs ---- */
    unsigned vH = H, vM = M, vL = L;
    struct fx att[3][8];
    unsigned vs3[3] = { vH, vM, vL };
    unsigned smooth = g & 0x200;
    unsigned flat = sr[4] & 0xfff;
    /* texture preparation: scratch at 0xbf0 / 0xc00 / 0xc10 (int halves) and +8 (fractions) */
    if (flags & 2) {
        struct fx w[3];
        for (int i = 0; i < 3; i++) {       /* w of the three vertices as unsorted, scaled by the halfword at 0x112 (perspective normalisation) */
            struct fx wr = { S16(R, v[i] + 6), (uint16_t)armlet_dm16(R, v[i] + 0xe) };
            w[i] = mul_c(wr, (uint16_t)armlet_dm16(R, 0x112));
        }
        struct fx mw = min32(min32(w[0], w[1]), w[2]);
        struct fx B = mul_c(mw, 0x8000);
        armlet_st16(R, 0xc24, (uint16_t)B.i);
        armlet_st16(R, 0xc2c, B.f);
        static const unsigned base[3] = { 0xbf0, 0xc00, 0xc10 };
        int16_t pi[3][4]; uint16_t pf[3][4];
        for (int k = 0; k < 3; k++) {
            unsigned vv = vs3[k];
            struct fx A = { S16(R, vv + 0x20), (uint16_t)armlet_dm16(R, vv + 0x22) };
            struct fx kk = mul_mid(A, B);
            int16_t x[4] = { S16(R, vv + 0x14), S16(R, vv + 0x16), 0x7fff, 0 };       /* lane 3: register leftover, replaced by z below */
            for (int j = 0; j < 4; j++) {
                struct fx pr = mul_x(x[j], kk);
                pi[k][j] = pr.i; pf[k][j] = pr.f;
                if (j < 3) {
                    armlet_st16(R, base[k] + 2 * j, (uint16_t)pr.i);
                    armlet_st16(R, base[k] + 8 + 2 * j, pr.f);
                }
            }
        }
        int16_t mxi[2]; uint16_t mxf[2];
        for (int j = 0; j < 2; j++) {
            struct fx cH = { vabs16(pi[0][j]), pf[0][j] }, cM = { vabs16(pi[1][j]), pf[1][j] }, cL = { vabs16(pi[2][j]), pf[2][j] };
            struct fx t1 = cH.i >= cL.i ? cH : cL;
            struct fx t2 = t1.i >= cM.i ? t1 : cM;
            mxi[j] = t2.i; mxf[j] = t2.f;
        }
        armlet_st16(R, 0xc20, (uint16_t)mxi[0]); armlet_st16(R, 0xc22, (uint16_t)mxi[1]);
        armlet_st16(R, 0xc28, mxf[0]); armlet_st16(R, 0xc2a, mxf[1]);
    }
    for (int k = 0; k < 3; k++) {
        unsigned vv = vs3[k];
        for (int l = 0; l < 4; l++) {
            att[k][l].i = (int16_t)armlet_dm8(R, (smooth ? vv : flat) + 0x10 + l);
            att[k][l].i &= 0xff;
            att[k][l].f = 0x8000;
        }
        static const unsigned base[3] = { 0xbf0, 0xc00, 0xc10 };
        for (int l = 4; l < 7; l++) {
            att[k][l].i = S16(R, base[k] + 2 * (l - 4));
            att[k][l].f = (uint16_t)armlet_dm16(R, base[k] + 8 + 2 * (l - 4));
        }
        att[k][7].i = S16(R, vv + 0x1c);
        att[k][7].f = (uint16_t)armlet_dm16(R, vv + 0x1e);
    }

    /* ---- coefficients, 8 lanes ---- */
    int16_t A4 = clamp16(4 * (int32_t)dyMH), B4 = clamp16(4 * (int32_t)dyLH), C4 = clamp16(4 * (int32_t)dxLH), D4 = clamp16(4 * (int32_t)dxMH);
    struct fx Rr = recip_refine(area);
    struct fx st[8], dX[8], dY[8], dE[8];
    for (int l = 0; l < 8; l++) {
        struct fx aH = att[0][l], aM = att[1][l], aL = att[2][l];
        struct fx dLH = sub_fx(aL, aH), dHM = sub_fx(aH, aM), dMH = sub_fx(aM, aH), dHL = sub_fx(aH, aL);
        acc_t acc1 = w48(pv(dLH) * A4 + pv(dHM) * B4);
        struct fx nx = { (int16_t)(acc1 >> 32), (uint16_t)(acc1 >> 16) };
        acc_t acc2 = w48(pv(dMH) * C4 + pv(dHL) * D4);
        struct fx ny = { (int16_t)(acc2 >> 32), (uint16_t)(acc2 >> 16) };
        dX[l] = mul_mid(nx, Rr);
        dY[l] = mul_mid(ny, Rr);
        /* DaDe = DaDy + DaDx * DxHDy (the clamped integer half, the full fraction) */
        struct fx sl = slope[2];
        acc_t ta = pv(dY[l]) + (acc_t)(((int64_t)dX[l].f * sl.f) >> 16) + (acc_t)dX[l].i * sl.f + (acc_t)dX[l].f * sl.i;
        acc_t tfull = ta + (((acc_t)dX[l].i * sl.i) << 16);
        dE[l].i = sclamp(w48(tfull)); dE[l].f = uclamp(w48(ta));
        struct fx cs = mul_c(dE[l], c_sub);
        st[l] = K->dlx ? aH : sub_fx(aH, cs);
    }
    unsigned o = sr[23];
    for (int l = 0; l < 4; l++) {          /* written whether or not the shade bit is set */
        armlet_st16(R, o + 8 + 2 * l, (uint16_t)dX[l].i);
        armlet_st16(R, o + 24 + 2 * l, dX[l].f);
        armlet_st16(R, o + 40 + 2 * l, (uint16_t)dY[l].i);
        armlet_st16(R, o + 56 + 2 * l, dY[l].f);
        armlet_st16(R, o + 32 + 2 * l, (uint16_t)dE[l].i);
        armlet_st16(R, o + 48 + 2 * l, dE[l].f);
    }
    if (flags & 4 || K->dlx) {          /* F3DLX stores the start values whether or not the shade bit is set */
        for (int l = 0; l < 4; l++) {
            armlet_st16(R, o + 2 * l, (uint16_t)st[l].i);
            armlet_st16(R, o + 16 + 2 * l, st[l].f);
        }
    }
    if (flags & 4)
        o += 64;
    if (flags & 2) {
        /* the scale: 1 / max(2|DsDx|, |DsDy|, ...) clamped to 1.0 */
        struct fx P[8];
        for (int l = 4; l < 8; l++) {
            struct fx n = { S16(R, 0xc20 + 2 * (l - 4)), (uint16_t)armlet_dm16(R, 0xc28 + 2 * (l - 4)) };
            struct fx E = mul_c(n, 0x800);
            acc_t tot = 2 * ((acc_t)vabs16(dX[l].i) * 0x800) + (acc_t)vabs16(dY[l].i) * 0x800 + E.f + (((acc_t)E.i) << 16);
            acc_t part = tot - (((acc_t)E.i) << 16);
            P[l].i = sclamp(w48(tot)); P[l].f = uclamp(w48(part));
        }
        struct fx M4 = max32(max32(P[4], P[5]), max32(P[6], P[5]));
        struct fx Ms = mul_c(M4, 0x40);
        uint32_t rr = rcp32((int32_t)((uint32_t)(uint16_t)Ms.i << 16 | Ms.f));
        int16_t Ri = (int16_t)(rr >> 16); uint16_t Rf = (uint16_t)rr;
        acc_t da = (acc_t)Rf * 2 + (((acc_t)Ri * 2) << 16);
        int16_t di = sclamp(da); uint16_t df = (uint16_t)((acc_t)Rf * 2);
        struct fx S = di < 1 ? (struct fx){ di, df } : (struct fx){ 1, 0 };
        for (int l = 4; l < 8; l++) {
            struct fx a = mul_mid(st[l], S), b = mul_mid(dX[l], S), c = mul_mid(dY[l], S), d = mul_mid(dE[l], S);
            int j = l - 4;
            armlet_st16(R, o + 2 * j, (uint16_t)a.i);        armlet_st16(R, o + 16 + 2 * j, a.f);
            armlet_st16(R, o + 8 + 2 * j, (uint16_t)b.i);    armlet_st16(R, o + 24 + 2 * j, b.f);
            armlet_st16(R, o + 40 + 2 * j, (uint16_t)c.i);   armlet_st16(R, o + 56 + 2 * j, c.f);
            armlet_st16(R, o + 32 + 2 * j, (uint16_t)d.i);   armlet_st16(R, o + 48 + 2 * j, d.f);
        }
        o += 64;
    }
    if ((flags & 1) || (K->dlx && (flags & 2))) {      /* F3DLX writes the z block after a texture block even without the z bit */
        struct fx zx = mul_s(dX[7], 0x20), zy = mul_s(dY[7], 0x20), ze = mul_s(dE[7], 0x20), zh = mul_s(att[0][7], 0x20);
        struct fx zs = K->dlx ? zh : sub_fx(zh, mul_c(ze, c_sub));
        armlet_st16(R, o + 0, (uint16_t)zs.i);  armlet_st16(R, o + 2, zs.f);
        armlet_st16(R, o + 4, (uint16_t)zx.i);  armlet_st16(R, o + 6, zx.f);
        armlet_st16(R, o + 8, (uint16_t)ze.i);  armlet_st16(R, o + 10, ze.f);
        armlet_st16(R, o + 12, (uint16_t)zy.i); armlet_st16(R, o + 14, zy.f);
        if (flags & 1)
            o += 16;
    }
    sr[23] = o;
    return (int)K->exit_pc;
}

#define SPEC_TRI(nm, uc, entry, exitpc, rej, dlxf) \
    static const struct tk tk_##nm = { rej, exitpc, dlxf }; \
    static int spec_tri_##nm(struct armlet_env *e) { return spec_tri(e, &tk_##nm); } \
    SPEC_REG(spec_reg_tri_##nm, "spec.f3dex.tri.setup", uc, entry, exitpc, spec_tri_##nm)

SPEC_TRI(bk, UC_BANJO121, 0x940, 0xf10, 0x7070, 0);
SPEC_TRI(ts2, UC_TS2_DEX, 0x938, 0xf08, 0x7070, 0);
SPEC_TRI(mk64, UC_MK64_DEX, 0x9c8, 0xf98, 0x7070, 0);
SPEC_TRI(sf64, UC_SF64_NON, 0x960, 0xf30, 0x7030, 0);
SPEC_TRI(ts2dlx, UC_TS2_DLX, 0x960, 0x120, 0x7070, 1);
SPEC_TRI(mk64dlx, UC_MK64_DLX, 0x9f0, 0x168, 0x7070, 1);
