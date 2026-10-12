/*
 * vtx_spec.c: G_VTX of the F3DEX 1.x / F3DLX family without lighting, as plain integer arithmetic (docs/ucode-f3dex.md, "The vertex command").
 * The routine computes the vertex records in DMEM; it is checked against the JIT by tools/native-f3dex/rt_test (DMEM and exit pc compared).
 */
#include <stdint.h>
#include <string.h>

#include "spec.h"

extern const uint16_t rsp_reciprocal_rom[1024];

typedef int64_t acc_t;

static inline acc_t wrap48(acc_t a) { return (acc_t)((uint64_t)a << 16) >> 16; }
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

/* the reciprocal of W (a 32-bit S15.16 value, (wi:wf)): doubled, one Newton step; docs section 1.6 */
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

/* VCH + VCL: bit k (low byte) and bit 8 + k of the 16-bit mask for the 32-bit values X (coordinate k) and W of its vertex */
static unsigned clip_bits(int64_t X, int64_t W, int k)
{
    int opp = (X < 0) != (W < 0);
    int le = opp ? (X + W <= 0) : (W < 0), ge = opp ? (W < 0) : (X - W >= 0);
    return (unsigned)le << k | (unsigned)ge << (8 + k);
}

struct vk {
    unsigned vtab, retslot;
    int kind;                       /* 0 F3DEX, 1 NoN, 2 F3DLX */
};

static inline int16_t S16(const struct armlet_rsp *R, unsigned a) { return (int16_t)armlet_dm16(R, a); }

/* one vertex, lanes 0..3 (A) or 4..7 (B) of the vector code: all the per-lane results */
struct vres {
    int16_t ci[4]; uint16_t cf[4];  /* clip-space position (x, y, z, w): integer and fraction halves */
    int16_t st[2];
    uint32_t col;
    int16_t sx[4];                  /* screen position int (x, y, z, w-lane) after the clamps */
    uint16_t sz_frac;
    int16_t ri; uint16_t rf;        /* reciprocal of w */
    unsigned c;                     /* the 16-bit VCC mask: bits 0..3 le, 8..11 ge */
    int16_t fogbyte_src;            /* the fog factor lane (z lane), valid when fog is on */
    int w_neg;
    int16_t sy_round;               /* F3DLX: (sy + 2) & ~3 */
};

static void vertex(const struct armlet_rsp *R, const struct vk *K, unsigned in, int lane_half, struct vres *o)
{
    /* inputs */
    int16_t v[3] = { S16(R, in + 0), S16(R, in + 2), S16(R, in + 4) };
    int16_t s = S16(R, in + 8), t = S16(R, in + 10);
    uint16_t pn = armlet_dm16(R, 0x112);
    uint16_t sc_s = armlet_dm16(R, 0x124), sc_t = armlet_dm16(R, 0x126);
    /* viewport: scale with y negated (saturating), translation */
    int16_t scale[4], trans[4];
    for (int k = 0; k < 4; k++) {
        int16_t sv = S16(R, 0x380 + 2 * k);
        scale[k] = (k == 1) ? clamp16(-(int32_t)sv) : sv;
        trans[k] = S16(R, 0x388 + 2 * k);
    }
    (void)lane_half;
    /* clip-space position: sum over the matrix rows */
    int16_t Pi[4]; uint16_t Pf[4];
    for (int k = 0; k < 4; k++) {
        acc_t S = 0;
        for (int j = 0; j < 3; j++) {
            int32_t M = (int32_t)((uint32_t)armlet_dm16(R, 0x460 + 8 * j + 2 * k) << 16 | armlet_dm16(R, 0x480 + 8 * j + 2 * k));
            S += (acc_t)M * v[j];
        }
        int16_t i3 = S16(R, 0x460 + 24 + 2 * k);
        uint16_t f3 = armlet_dm16(R, 0x480 + 24 + 2 * k);
        S += (acc_t)f3 + ((acc_t)i3 << 16);
        S = wrap48(S);
        Pi[k] = sclamp(S);
        Pf[k] = uclamp(wrap48(S - ((acc_t)i3 << 16)));
    }
    for (int k = 0; k < 4; k++) { o->ci[k] = Pi[k]; o->cf[k] = Pf[k]; }
    /* clip flags */
    {
        int64_t W = (int64_t)Pi[3] * 65536 + Pf[3];
        unsigned c = 0;
        for (int k = 0; k < 4; k++)
            c |= clip_bits((int64_t)Pi[k] * 65536 + Pf[k], W, k);
        o->c = c;
        o->w_neg = Pi[3] < 0;
    }
    /* P * perspective norm, the reciprocal of its w */
    int16_t Qi[4]; uint16_t Qf[4];
    for (int k = 0; k < 4; k++) {
        acc_t R1 = (acc_t)(((int64_t)Pf[k] * pn) >> 16) + (acc_t)Pi[k] * pn;
        Qi[k] = sclamp(R1);
        Qf[k] = uclamp(R1);
    }
    int16_t ri; uint16_t rf;
    recip_refine((int32_t)((uint32_t)(uint16_t)Qi[3] << 16 | Qf[3]), &ri, &rf);
    o->ri = ri; o->rf = rf;
    int16_t rint = ri >= 0 ? ri : 0x7fff;
    /* ndc = P * (rint:rf), then * perspective norm */
    int16_t Ni[4], Ti[4]; uint16_t Nf[4], Tf[4];
    for (int k = 0; k < 4; k++) {
        acc_t R2 = (acc_t)(((int64_t)Pf[k] * rf) >> 16) + (acc_t)Pi[k] * rf + (acc_t)Pf[k] * rint;
        acc_t R3 = R2 + (((acc_t)Pi[k] * rint) << 16);
        Ni[k] = sclamp(R3);
        Nf[k] = uclamp(R2);
        acc_t R4 = (acc_t)(((int64_t)Nf[k] * pn) >> 16) + (acc_t)Ni[k] * pn;
        Ti[k] = sclamp(R4);
        Tf[k] = uclamp(R4);
    }
    /* screen position */
    int16_t Sc[4]; uint16_t Sf[4];
    for (int k = 0; k < 4; k++) {
        acc_t A = ((acc_t)trans[k] << 16) + (acc_t)Tf[k] * scale[k] + (((acc_t)Ti[k] * scale[k]) << 16);
        A = wrap48(A);
        int16_t si = sclamp(A);
        uint16_t sf = uclamp(A);
        if (k < 2) { if (si < -4090) si = -4090; if (si > 4090) si = 4090; }
        else { if (si < 0) si = 0; if (si > 0x7fff) si = 0x7fff; }
        Sc[k] = si; Sf[k] = sf;
        o->sx[k] = si;
    }
    o->sz_frac = Sf[2];
    /* fog factor of this vertex: (ndc z * fog multiplier >> 16) + offset, clamped to 0..255 (docs) */
    {
        int16_t mult = S16(R, 0x390), off = S16(R, 0x392);
        acc_t Fa = (acc_t)Tf[2] * 0 + 0; (void)Fa;
        /* the fog source is the scaled ndc (v4:v5 = Ti:Tf before the screen multiply) */
        acc_t F = (acc_t)Tf[2] * mult + (((acc_t)Ti[2] * mult) << 16);
        int32_t f = sclamp(F);
        f = clamp16(f + off);
        if (f < 0) f = 0;
        if (f > S16(R, 0x394)) f = S16(R, 0x394);
        o->fogbyte_src = (int16_t)f;
    }
    o->sy_round = (int16_t)((Sc[1] + 2) & 0xfffc);
    /* texture coordinates */
    o->st[0] = (int16_t)(((int32_t)s * (int32_t)sc_s) >> 16);
    o->st[1] = (int16_t)(((int32_t)t * (int32_t)sc_t) >> 16);
    o->col = armlet_dm32(R, in + 12);
}

static void store_vertex(const struct armlet_rsp *R, const struct vk *K, unsigned slot, const struct vres *o, int fog)
{
    for (int k = 0; k < 4; k++) {
        armlet_st16(R, slot + 2 * k, (uint16_t)o->ci[k]);
        armlet_st16(R, slot + 8 + 2 * k, o->cf[k]);
    }
    uint32_t col = o->col;
    if (fog)
        col = (col & 0xffffff00u) | (uint8_t)o->fogbyte_src;
    armlet_st32(R, slot + 16, col);
    armlet_st16(R, slot + 20, (uint16_t)o->st[0]);
    armlet_st16(R, slot + 22, (uint16_t)o->st[1]);
    for (int k = 0; k < 3; k++)
        armlet_st16(R, slot + 24 + 2 * k, (uint16_t)o->sx[k]);
    armlet_st16(R, slot + 30, o->sz_frac);
    armlet_st16(R, slot + 32, (uint16_t)o->ri);
    armlet_st16(R, slot + 34, o->rf);
    if (K->kind == 2)
        armlet_st16(R, slot + 26, (uint16_t)o->sy_round);
}

static int spec_vtx(struct armlet_env *e, const struct vk *K)
{
    const struct armlet_rsp *R = &e->rsp;
    unsigned ret = armlet_dm16(R, K->retslot);
    armlet_st16(R, 0x106, (uint16_t)ret);
    unsigned n = armlet_dm8(R, e->rsp.sr[27] - 6) >> 2;
    unsigned in = e->rsp.sr[22];
    unsigned slot = armlet_dm16(R, K->vtab + e->rsp.sr[1]);
    uint32_t geom_hi = (uint32_t)(int32_t)(int16_t)armlet_dm16(R, 0x114);
    int fog = geom_hi & 1;
    int clipflags = K->kind != 2 || (geom_hi & 128);
    int r9 = (int)n;
    for (;;) {
        struct vres a, b;
        vertex(R, K, in, 0, &a);
        vertex(R, K, in + 16, 1, &b);
        /* flags words of this pair (A: low halfword, B: high halfword of r8) */
        unsigned ca = a.c, cb;
        {   /* the two vertices share the VCC read: lanes 0..3 are A, lanes 4..7 are B */
            unsigned c = (a.c & 0x0f0f) | ((b.c & 0x0f0f) << 4);        /* bit k: lane k; bit 8+k: ge of lane k */
            ca = c; cb = c;
        }
        uint32_t r8 = 0;
        if (clipflags) {
            unsigned c = ca;
            uint32_t r13, r14;
            if (K->kind == 1) {
                uint32_t wa = a.w_neg ? 4 : 0, wb = b.w_neg ? 0x40 : 0;
                r14 = wa | wb;
                r13 = (((c & 0x0703u) << 4) & 0xffffu) | (((c & 0x7030u) | r14) << 16);
                r8 = (c & 0x0707u) | ((c & 0x7070u) << 12) | r13;
            } else {
                r13 = ((c & 0x0707u) << 4) | ((c & 0x7070u) << 16);
                r8 = (c & 0x0707u) | ((c & 0x7070u) << 12) | r13;
            }
        } else {
            /* F3DLX with clip flags off: r13 / r14 keep their previous values; not modelled (the generator keeps the bit set) */
            return -1;
        }
        r9 -= 2;
        armlet_st16(R, slot + 36, (uint16_t)r8);
        store_vertex(R, K, slot, &a, fog);
        if (r9 < 0)
            break;
        store_vertex(R, K, slot + 40, &b, fog);
        armlet_st32(R, slot + 76, r8);
        slot += 80;
        in += 32;
        if (r9 <= 0)
            break;
    }
    return (int)(ret & 0xfff);
}

#define SPEC_VTX(nm, uc, entry, vtab, retslot, kind) \
    static const struct vk vk_##nm = { vtab, retslot, kind }; \
    static int spec_vtx_##nm(struct armlet_env *e) { return spec_vtx(e, &vk_##nm); } \
    SPEC_REG(spec_reg_vtx_##nm, "spec.f3dex.vtx.plain", uc, entry, 0, spec_vtx_##nm)

SPEC_VTX(bk, UC_BANJO121, 0x3a8, 0x31e, 0xba, 0);
SPEC_VTX(ts2, UC_TS2_DEX, 0x3a0, 0x31e, 0xba, 0);
SPEC_VTX(mk64, UC_MK64_DEX, 0x430, 0x31c, 0xbe, 0);
SPEC_VTX(sf64, UC_SF64_NON, 0x3a8, 0x31e, 0xba, 1);
SPEC_VTX(ts2dlx, UC_TS2_DLX, 0x3b0, 0x31e, 0xba, 2);
