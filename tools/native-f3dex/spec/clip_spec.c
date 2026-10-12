/*
 * clip_spec.c: the triangle clipping overlay (overlay 1) of the F3DEX 1.x / F3DLX microcode family as plain integer arithmetic.
 *
 * The stretch: from the overlay entry (the triangle code jumps there when a vertex of the triangle is beyond a clip plane) to the first of
 *   - the hand-over of the first resulting triangle to the triangle code ($1, $2, $3 = vertex addresses, $24 = the fan's re-entry; exit pc = tri),
 *   - the return to the caller ($24 at entry) because the whole polygon is on the outer side of one plane or fewer than 3 vertices are left.
 * "spec.f3dex.clip.main" covers that stretch; "spec.f3dex.clip.fan" covers one step of the final fan (the code the triangle code returns to).
 * DMEM only is compared (rt_test, spec.* routines).
 */
#include <stdint.h>
#include <string.h>

#include "spec.h"

extern const uint16_t rsp_reciprocal_rom[1024];

typedef int64_t acc_t;

static inline acc_t wrap48(acc_t a) { return (acc_t)((uint64_t)a << 16) >> 16; }
static inline int16_t sclamp(acc_t a)
{
    a = wrap48(a);
    acc_t v = a >> 16;
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
}
static inline uint16_t uclamp(acc_t a)
{
    a = wrap48(a);
    if (a >= -(acc_t)2147483648ll && a < (acc_t)2147483648ll)
        return (uint16_t)a;
    return a < 0 ? 0 : 0xffff;
}
static inline int16_t clamp16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }

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

/* the refined reciprocal of a 32-bit S15.16 value (the subroutine at pc 0: rcp32, doubled, one Newton step), docs 1.6 */
static void recip_refine(int32_t w, int16_t *ri, uint16_t *rf)
{
    uint32_t r = rcp32(w);
    int16_t wi = (int16_t)(w >> 16);
    uint16_t wf = (uint16_t)w;
    acc_t a = (acc_t)(uint16_t)r * 2 + (((acc_t)(int16_t)(r >> 16) * 2) << 16);
    int16_t di = sclamp(a);
    uint16_t df = uclamp(a);
    acc_t a1 = (acc_t)(((int64_t)df * wf) >> 16) + (acc_t)di * wf + (acc_t)df * wi;
    uint16_t t_frac = uclamp(a1);
    acc_t a2 = a1 + (((acc_t)di * wi) << 16);
    int16_t t_int = sclamp(a2);
    uint16_t e_frac = (uint16_t)(0 - t_frac);
    int borrow = 0 < t_frac;
    int16_t e_int = clamp16(2 - (int32_t)t_int - borrow);
    acc_t b1 = (acc_t)(((int64_t)df * e_frac) >> 16) + (acc_t)di * e_frac + (acc_t)df * e_int;
    acc_t b2 = b1 + (((acc_t)di * e_int) << 16);
    *ri = sclamp(b2);
    *rf = uclamp(b1);
}

/* VCH + VCL of coordinate k (0..2) against W: bit k = X <= -W, bit 8 + k = X >= W (32-bit values, docs 1.4) */
static unsigned clip_bits(int64_t X, int64_t W, int k)
{
    int opp = (X < 0) != (W < 0);
    int le = opp ? (X + W <= 0) : (W < 0), ge = opp ? (W < 0) : (X - W >= 0);
    return (unsigned)le << k | (unsigned)ge << (8 + k);
}


#ifdef CLIP_COVER
#include <stdio.h>
#include <stdlib.h>
static unsigned cv_plane_new[8], cv_plane_noop[8], cv_plane_disc[8], cv_nnew[16], cv_npoly[16], cv_wneg_new, cv_calls, cv_sf1, cv_sfsat, cv_cfsat, cv_zero, cv_flag_nz;
static void cv_dump(void)
{
    fprintf(stderr, "cover: calls %u\n planes (index by mask order 40,4000,200,100,2,1) new-vertex pairs:", cv_calls);
    for (int i = 0; i < 6; i++) fprintf(stderr, " %u", cv_plane_new[i]);
    fprintf(stderr, "\n  no-op:"); for (int i = 0; i < 6; i++) fprintf(stderr, " %u", cv_plane_noop[i]);
    fprintf(stderr, "\n  discard:"); for (int i = 0; i < 6; i++) fprintf(stderr, " %u", cv_plane_disc[i]);
    fprintf(stderr, "\n  new vertices per call:"); for (int i = 0; i < 14; i++) fprintf(stderr, " %u", cv_nnew[i]);
    fprintf(stderr, "\n  final polygon size:"); for (int i = 0; i < 12; i++) fprintf(stderr, " %u", cv_npoly[i]);
    fprintf(stderr, "\n  weight sf==1 %u, sf saturated(Si!=0) %u, cf saturated %u, X==0 %u\n", cv_sf1, cv_sfsat, cv_cfsat, cv_zero);
}
#define COV(x) x
#else
#define COV(x)
#endif

struct ck {
    int kind;                       /* 0 F3DEX (1.21 / 1.23 / 0.95), 1 F3DEX.NoN, 2 F3DLX */
    unsigned tri;                   /* the triangle code's entry for a clipped triangle (the target of the fan's bne) */
};

static inline int16_t S16(const struct armlet_rsp *R, unsigned a) { return (int16_t)armlet_dm16(R, a & 0xfff); }
static inline uint16_t U16(const struct armlet_rsp *R, unsigned a) { return armlet_dm16(R, a & 0xfff); }
static inline void W16(const struct armlet_rsp *R, unsigned a, uint16_t v) { armlet_st16(R, a & 0xfff, v); }

/* a lane-3 sum of four S15.16 lanes with the two-stage saturating pair sums of VADDC / VADD ([2] then [5] element selectors) */
static void lane3_sum(const int16_t vi[4], const uint16_t vf[4], int16_t *oi, uint16_t *of)
{
    uint16_t f1[4]; int16_t i1[4];
    for (int l = 0; l < 4; l++) {
        unsigned p = l & ~1u;
        unsigned s = (unsigned)vf[l] + vf[p];
        f1[l] = (uint16_t)s;
        i1[l] = clamp16((int32_t)vi[l] + vi[p] + (int)(s >> 16));
    }
    unsigned s = (unsigned)f1[3] + f1[1];
    *of = (uint16_t)s;
    *oi = clamp16((int32_t)i1[3] + i1[1] + (int)(s >> 16));
}

/* the new vertex on the edge outside vertex O -- inside vertex Q against the plane with coefficients at DMEM 0x70 + 4 * r6 (r6 = 2 * plane index),
 * stored to the record at `slot` (the clip arithmetic, then the vertex code's tail for a single vertex) */
static void new_vertex(const struct armlet_rsp *R, const struct ck *K, unsigned O, unsigned Q, unsigned r6, unsigned slot)
{
    int16_t Qi[4], Oi[4], c[4], nc[4];
    uint16_t Qf[4], Of[4];
    for (int l = 0; l < 4; l++) {
        Qi[l] = S16(R, Q + 2 * l); Qf[l] = U16(R, Q + 8 + 2 * l);
        Oi[l] = S16(R, O + 2 * l); Of[l] = U16(R, O + 8 + 2 * l);
        c[l] = S16(R, 0x70 + 4 * r6 + 2 * l);
        nc[l] = clamp16(-(int32_t)c[l]);
    }
    /* plane value of Q (lane products c * Q as 32-bit pairs) and of Q - O */
    int16_t pi[4], di[4]; uint16_t pf[4], df[4];
    for (int l = 0; l < 4; l++) {
        acc_t aq = (acc_t)Qf[l] * c[l] + (((acc_t)Qi[l] * c[l]) << 16);
        pi[l] = sclamp(aq); pf[l] = uclamp(aq);
        acc_t ax = aq + (acc_t)Of[l] * nc[l] + (((acc_t)Oi[l] * nc[l]) << 16);
        di[l] = sclamp(ax); df[l] = uclamp(ax);
    }
    int16_t Xi; uint16_t Xf;
    lane3_sum(di, df, &Xi, &Xf);                        /* X = D(Q) - D(O) */
    int16_t DQi; uint16_t DQf;
    lane3_sum(pi, pf, &DQi, &DQf);                      /* D(Q) */
    /* first, rough reciprocal of |X| as an unsigned 16-bit fraction cf */
    uint32_t rr = rcp32((int32_t)((uint32_t)(uint16_t)Xi << 16 | Xf));
    int16_t Ri = (int16_t)(rr >> 16); uint16_t Rf = (uint16_t)rr;
    acc_t a2 = (acc_t)Rf * 2 + (((acc_t)Ri * 2) << 16);
    uint16_t v3 = (uint16_t)((acc_t)Rf * 2);
    int16_t v7 = sclamp(a2);
    if (Xi < 0) {
        acc_t a3 = -(acc_t)v3 - ((acc_t)v7 << 16);
        v3 = (uint16_t)(-(acc_t)v3);
        v7 = sclamp(a3);
    }
    uint16_t cf = v7 == 0 ? v3 : 0xffff;
    COV(if (v7 != 0) cv_cfsat++; if (Xi == 0 && Xf == 0) cv_zero++;)
    /* X' = X * cf, its refined reciprocal Inv */
    acc_t ax = (acc_t)(((int64_t)Xf * cf) >> 16) + (acc_t)Xi * cf;
    int16_t Xpi = sclamp(ax); uint16_t Xpf = uclamp(ax);
    int16_t Invi; uint16_t Invf;
    recip_refine((int32_t)((uint32_t)(uint16_t)Xpi << 16 | Xpf), &Invi, &Invf);
    /* s = D(Q) * Inv * cf + 3 (a weight in S15.16), then the 16-bit weights sf (outside vertex) and tf = 65536 - sf (inside vertex) */
    acc_t a1 = (acc_t)(((int64_t)DQf * Invf) >> 16) + (acc_t)DQi * Invf + (acc_t)DQf * Invi;
    uint16_t Tf = uclamp(a1);
    int16_t Ti = sclamp(a1 + (((acc_t)DQi * Invi) << 16));
    acc_t as = 3 + (acc_t)(((int64_t)Tf * cf) >> 16) + (acc_t)Ti * cf;
    int16_t Si = sclamp(as); uint16_t Sf = uclamp(as);
    uint16_t sf = Si == 0 ? Sf : 0xffff;
    COV(if (Si != 0) cv_sfsat++;)
    if (sf == 0) sf = 1;
    COV(if (sf == 1) cv_sf1++;)
    uint16_t tf = (uint16_t)(0x10000 - sf);
    /* the interpolated clip-space position */
    int16_t Ni[4]; uint16_t Nf[4];
    for (int l = 0; l < 4; l++) {
        acc_t a = (acc_t)(((int64_t)Qf[l] * tf) >> 16) + (acc_t)Qi[l] * tf + (acc_t)(((int64_t)Of[l] * sf) >> 16) + (acc_t)Oi[l] * sf;
        Ni[l] = sclamp(a); Nf[l] = uclamp(a);
    }
    /* colour bytes and texture coordinates */
    uint8_t col[4]; int16_t st[2];
    for (int k = 0; k < 6; k++) {
        int32_t vo, vq;
        if (k < 4) { vo = (int32_t)armlet_dm8(R, (O + 16 + k) & 0xfff) << 7; vq = (int32_t)armlet_dm8(R, (Q + 16 + k) & 0xfff) << 7; }
        else { vo = S16(R, O + 16 + 2 * k - 4); vq = S16(R, Q + 16 + 2 * k - 4); }
        int16_t v = sclamp((acc_t)vo * sf + (acc_t)vq * tf);
        if (k < 4) col[k] = (uint8_t)(v >> 7); else st[k - 4] = v;
    }
    uint32_t colw = (uint32_t)col[0] << 24 | (uint32_t)col[1] << 16 | (uint32_t)col[2] << 8 | col[3];

    /* ---- the vertex code's tail for one vertex (pc 0x414 ..): flags, reciprocal of w, screen position ---- */
    uint16_t pn = U16(R, 0x112);
    int16_t geom_hi = S16(R, 0x114);
    int16_t rr0 = S16(R, 0x76);                          /* clip ratio */
    int16_t scale[4], trans[4];
    for (int k = 0; k < 4; k++) {
        int16_t sv = S16(R, 0x380 + 2 * k);
        scale[k] = (k == 1) ? clamp16(-(int32_t)sv) : sv;
        trans[k] = S16(R, 0x388 + 2 * k);
    }
    int64_t W = (int64_t)Ni[3] * 65536 + Nf[3];
    int64_t Wr = ((int64_t)Nf[3] * rr0 + (((int64_t)Ni[3] * rr0) << 16));
    int16_t Wri = sclamp(Wr); uint16_t Wrf = (uint16_t)Wr;
    int64_t Wrv = (int64_t)Wri * 65536 + Wrf;
    unsigned pa = 0, pb = 0;
    for (int k = 0; k < 3; k++) {
        int64_t X = (int64_t)Ni[k] * 65536 + Nf[k];
        pa |= clip_bits(X, W, k);
        pb |= clip_bits(X, Wrv, k);
    }
    unsigned flags;
    if (K->kind == 1)
        flags = (pb & 0x707u) | ((pa & 0x703u) << 4);
    else
        flags = (pb & 0x707u) | ((pa & 0x707u) << 4);
    W16(R, slot + 36, (uint16_t)flags);
    int16_t Qni[4]; uint16_t Qnf[4];
    for (int l = 0; l < 4; l++) {
        acc_t a = (acc_t)(((int64_t)Nf[l] * pn) >> 16) + (acc_t)Ni[l] * pn;
        Qni[l] = sclamp(a); Qnf[l] = uclamp(a);
    }
    int16_t ri; uint16_t rf;
    recip_refine((int32_t)((uint32_t)(uint16_t)Qni[3] << 16 | Qnf[3]), &ri, &rf);
    int16_t rint = ri >= 0 ? ri : 0x7fff;
    int16_t Ti_[4], Mi[4]; uint16_t Tf_[4], Mf[4];
    for (int l = 0; l < 4; l++) {
        acc_t R2 = (acc_t)(((int64_t)Nf[l] * rf) >> 16) + (acc_t)Ni[l] * rf + (acc_t)Nf[l] * rint;
        Mf[l] = uclamp(R2);
        Mi[l] = sclamp(R2 + (((acc_t)Ni[l] * rint) << 16));
        acc_t R4 = (acc_t)(((int64_t)Mf[l] * pn) >> 16) + (acc_t)Mi[l] * pn;
        Ti_[l] = sclamp(R4); Tf_[l] = uclamp(R4);
    }
    int16_t Sc[4]; uint16_t Sz = 0;
    for (int l = 0; l < 4; l++) {
        acc_t A = ((acc_t)trans[l] << 16) + (acc_t)Tf_[l] * scale[l] + (((acc_t)Ti_[l] * scale[l]) << 16);
        int16_t si = sclamp(A);
        if (l == 2) Sz = uclamp(A);
        if (l < 2) { if (si < -4090) si = -4090; if (si > 4090) si = 4090; }
        else if (si < 0) si = 0;               /* z and the w lane: [0, 0x7fff] (the upper bound cannot be exceeded by an s16) */
        Sc[l] = si;
    }
    for (int l = 0; l < 4; l++) {
        W16(R, slot + 2 * l, (uint16_t)Ni[l]);
        W16(R, slot + 8 + 2 * l, Nf[l]);
    }
    uint32_t cw = colw;
    if (geom_hi & 1) {
        int16_t mult = S16(R, 0x390), off = S16(R, 0x392), mx = S16(R, 0x394);
        acc_t F = (acc_t)Tf_[2] * mult + (((acc_t)Ti_[2] * mult) << 16);
        int32_t f = clamp16((int32_t)sclamp(F) + off);
        if (f < 0) f = 0;
        if (f > mx) f = mx;
        cw = (cw & 0xffffff00u) | (uint8_t)f;
    }
    armlet_st32(R, slot + 16, cw);
    W16(R, slot + 20, (uint16_t)st[0]);
    W16(R, slot + 22, (uint16_t)st[1]);
    W16(R, slot + 24, (uint16_t)Sc[0]);
    W16(R, slot + 26, (uint16_t)Sc[1]);
    W16(R, slot + 28, (uint16_t)Sc[2]);
    W16(R, slot + 30, Sz);
    W16(R, slot + 32, (uint16_t)ri);
    W16(R, slot + 34, rf);
    if (K->kind == 2)
        W16(R, slot + 26, (uint16_t)(((int32_t)Sc[1] + 2) & 0xfffc));
}

/* the fan: one triangle of the final polygon (pc 0x920 ..): list pointer at 0x106, the fan's first vertex at 0xb4 */
static int fan_step(const struct armlet_rsp *R, const struct ck *K, uint32_t ret)
{
    unsigned p = (unsigned)(uint16_t)S16(R, 0x106);
    uint32_t r1 = (uint32_t)(int32_t)S16(R, p + 4);
    W16(R, 0x106, (uint16_t)(p + 2));
    return r1 != 0 ? (int)K->tri : (int)ret;
}

static int clip_main(struct armlet_env *e, const struct ck *K)
{
    const struct armlet_rsp *R = &e->rsp;
    uint32_t *sr = R->sr;
    const uint32_t ret0 = sr[24];
    uint32_t r10 = sr[10], r20 = sr[20];
    uint32_t v13_0 = R->vr[13][0];
    W16(R, 0x158, (uint16_t)sr[31]);
    W16(R, 0xc40, (uint16_t)sr[3]);
    W16(R, 0xc42, (uint16_t)sr[2]);
    W16(R, 0xc44, (uint16_t)sr[1]);
    W16(R, 0xc46, 0);
    COV(unsigned nnew = 0; static int cv_registered;)
    unsigned r7 = 0xdb8, r30 = 0xc40, r6 = 12, r5, r2, r14, r17, r18;
    uint32_t r11, r8;
    for (;;) {
        r5 = r30;
        r30 ^= 20;
    next_plane:
        r11 = (uint32_t)(int32_t)S16(R, 0xa6 + r6);
        if (r6 == 0)
            break;
        r6 -= 2;
        r17 = 0; r18 = 0;
        r2 = r5; r14 = r30 + 2;
        goto emit;
    resume_rewind:
        r2 = r5;
    resume_out:
        r14 = r30 + 2;
    emit:
        r20 = r10;
        W16(R, r14, (uint16_t)r10);
        r14 += 2;
    load:
        r10 = (uint32_t)(int32_t)S16(R, r2);
        r8 = (uint32_t)(int32_t)S16(R, r10 + 36);
        if (r10 != 0) {
            r8 = r11 & r8;
            r2 += 2;
            if (r8 != r18)
                goto emit;
            goto transition;
        }
        r8 = r17 - 2;
        r2 = r5;
        if ((int32_t)r8 > 0)
            goto load;
        if (r8 == 0) {
            COV(cv_plane_noop[(12 - (int)r6 - 2) / 2]++;)
            goto next_plane;
        }
        COV(cv_plane_disc[(12 - (int)r6 - 2) / 2]++; cv_nnew[nnew]++; cv_calls++; if (!cv_registered) { atexit(cv_dump); cv_registered = 1; })
        return (int)ret0;
    transition:
        r18 ^= r11;
        {
            unsigned st = r17;
            r17 += 2;
            r8 = (uint32_t)(int32_t)S16(R, 0x102);
            if (st == 0)
                goto resume_rewind;
            if (st == 2)
                goto resume_out;
            if (st == 4) {
                v13_0 = (uint16_t)r10;
                r10 = r20;
                r20 = (uint32_t)(int32_t)(int16_t)v13_0;
                r14 = r30;
                r8 = (uint32_t)(int32_t)S16(R, 0xf8);
            }
            W16(R, 0x106, (uint16_t)r8);
            r7 += 40;
            COV(nnew++; if (st == 6) cv_plane_new[(12 - (int)r6 - 2) / 2]++;)
            W16(R, r14, (uint16_t)r7);
            W16(R, r14 + 2, 0);
            new_vertex(R, K, r10 & 0xfff, r20 & 0xfff, r6, r7 & 0xfff);
            r10 = (uint32_t)(int32_t)(int16_t)v13_0;
            if (st == 4)
                goto resume_out;
            continue;                           /* resume at the loop top (pc 0x758) */
        }
    }
    /* all planes done: r5 is the final polygon; the fan starts */
    COV({ unsigned n = 0; while (S16(R, r5 + 2 * n)) n++; cv_npoly[n]++; cv_nnew[nnew]++; cv_calls++; if (!cv_registered) { atexit(cv_dump); cv_registered = 1; } })
    W16(R, 0xb4, (uint16_t)S16(R, r5));
    W16(R, 0x106, (uint16_t)r5);
    return fan_step(R, K, ret0);
}

static int clip_fan(struct armlet_env *e, const struct ck *K)
{
    return fan_step(&e->rsp, K, e->rsp.sr[30]);
}

/* clip.main: overlay entry -> first triangle (exit = the triangle code's entry) or the caller's return; clip.fan: pc 0x920 + (build offset), the step the
 * triangle code returns to */
#define SPEC_CLIP(nm, uc, entry, trip, kindv, fanpc) \
    static const struct ck ck_##nm = { kindv, trip }; \
    static int spec_clip_##nm(struct armlet_env *e) { return clip_main(e, &ck_##nm); } \
    static int spec_clipfan_##nm(struct armlet_env *e) { return clip_fan(e, &ck_##nm); } \
    SPEC_REG(spec_reg_clip_##nm, "spec.f3dex.clip.main", uc, entry, trip, spec_clip_##nm); \
    SPEC_REG(spec_reg_clipfan_##nm, "spec.f3dex.clip.fan", uc, fanpc, trip, spec_clipfan_##nm)

SPEC_CLIP(bk, UC_BANJO121, 0x720, 0x96c, 0, 0x920);
SPEC_CLIP(ts2, UC_TS2_DEX, 0x718, 0x964, 0, 0x918);
SPEC_CLIP(mk64, UC_MK64_DEX, 0x7a8, 0x9f4, 0, 0x9a8);
SPEC_CLIP(sf64, UC_SF64_NON, 0x740, 0x98c, 1, 0x940);
SPEC_CLIP(ts2dlx, UC_TS2_DLX, 0x740, 0x994, 2, 0x940);
SPEC_CLIP(mk64dlx, UC_MK64_DLX, 0x7d0, 0xa24, 2, 0x9d0);
