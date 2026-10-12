/*
 * light_spec.c: the F3DEX 1.x / F3DLX G_VTX handler written as arithmetic: vertex lighting, texgen, fog and the position part that the
 * lit fields share the record with. Checked against the JIT by tools/native-f3dex/rt_test (routine spec.f3dex.light.vtx, every DMEM byte and
 * the exit pc compared). Documentation: kit/out/light.md (section numbers in the comments below refer to it).
 *
 * Stretch: the handler entry (banjo121 0x3a8; per build in the SPEC_REG lines at the end) to the return into the display-list loop (the pc in DMEM 0x106).
 * Everything is per vertex; the microcode processes two vertices per iteration in the two halves of the vector registers (4 lanes each) and the
 * halves never mix, except for the stale v6 of a texgen with no directional light (section 7) and one NoN clip-flag bit (section 9).
 * Calls that need the light-direction recomputation (lighting on, DMEM 0x12c negative) are declined.
 */
#include "spec.h"
#include <string.h>

typedef int64_t i64;

static i64 w48(i64 v) { v &= 0xffffffffffffll; return (v ^ 0x800000000000ll) - 0x800000000000ll; }       /* the accumulator is 48 bits */
static int32_t cl16(i64 v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int32_t)v; }
static int32_t sclamp(i64 acc) { return cl16(acc >> 16); }                                              /* VMUDM VMADM VMADH VMULF VMACF result */
static uint16_t uclamp(i64 acc)                                                                          /* VMADN VMADL result */
{
    if (acc >= -(1ll << 31) && acc < (1ll << 31))
        return (uint16_t)acc;
    return acc < 0 ? 0 : 0xffff;
}
static int32_t vmulf(int a, int b) { return sclamp(w48((i64)2 * a * b + 0x8000)); }

/* 32 x 32 -> floor(A*B/65536): VMUDL VMADM VMADN VMADH. Ri = sclamp(acc), Rf = uclamp(acc before the Ai*Bi term). */
static void mul32(int Ai, unsigned Af, int Bi, unsigned Bf, int *Ri, unsigned *Rf)
{
    i64 acc = ((i64)Af * Bf) >> 16;
    acc = w48(acc + (i64)Ai * Bf);
    acc = w48(acc + (i64)Af * Bi);
    *Rf = uclamp(acc);
    acc = w48(acc + (((i64)Ai * Bi) << 16));
    *Ri = sclamp(acc);
}

/* the reciprocal unit: rsp_reciprocal_rom[i] = ((2^34 / (i + 512)) + 1) >> 8 (i = 1..511), [0] = 0xffff */
static int32_t rcp32(int32_t input)
{
    int32_t mask = input >> 31, data = input ^ mask;
    if (input > -32768)
        data -= mask;
    if (data == 0)
        return 0x7fffffff;
    if (input == -32768)
        return (int32_t)0xffff0000u;
    unsigned shift = (unsigned)__builtin_clz((unsigned)data);
    unsigned idx = (unsigned)((((unsigned long long)(unsigned)data << shift) & 0x7FC00000ull) >> 22);
    int32_t r = idx ? (int32_t)((((1ull << 34) / (idx + 512)) + 1) >> 8) & 0xffff : 0xffff;
    int32_t result = ((0x10000 | r) << 14) >> (31 - shift);
    return result ^ mask;
}

/* 1/X, X = Xi:Xf (S15.16 or integer), one Newton step: R = rcp32(X); D = 2R; T = D*X; E = 2 - T; D*E */
static void recip_refine(int Xi, unsigned Xf, int *Ri, unsigned *Rf)
{
    int32_t in = (int32_t)(((uint32_t)(uint16_t)Xi << 16) | Xf);
    int32_t r = rcp32(in);
    int ri = (int16_t)(r >> 16);
    unsigned rf = (uint16_t)r;
    i64 acc = w48(2 * (i64)rf);
    acc = w48(acc + (((i64)ri * 2) << 16));
    int Di = sclamp(acc);
    unsigned Df = uclamp(acc);
    int Ti;
    unsigned Tf;
    mul32(Di, Df, Xi, Xf, &Ti, &Tf);
    unsigned Ef = (uint16_t)(0u - Tf);
    int Ei = cl16(2 - (i64)Ti - (Tf != 0));
    mul32(Di, Df, Ei, Ef, Ri, Rf);
}

/* clip compare of the 32-bit X against the 32-bit W (VCH + VCL): bit 0 = "X <= -W" side, bit 1 = "X >= W" side */
static unsigned clipcmp(int Xi, unsigned Xf, int Wi, unsigned Wf)
{
    i64 X = (int32_t)(((uint32_t)(uint16_t)Xi << 16) | Xf), W = (int32_t)(((uint32_t)(uint16_t)Wi << 16) | Wf);
    int opp = (X < 0) != (W < 0);
    unsigned lo = opp ? (X + W <= 0) : (W < 0);
    unsigned hi = opp ? (W < 0) : (X - W >= 0);
    return lo | hi << 1;
}

/* per-build parameters of the handler (kit/out/light.md, "Builds") */
struct build
{
    uint64_t id;
    unsigned tab;         /* DMEM address of the halfword table of the vertex slot addresses, indexed by the command's byte 1 (v0 * 2) */
    unsigned ret;         /* DMEM address of the halfword holding the return pc (low 12 bits) of the handler */
    int non;              /* F3DEX.NoN: no z clip flags in the first compare, the sign of w instead */
    int dlx;              /* F3DLX: G_CLIPPING bit, rounded screen y */
};
static const struct build builds[] = {
    { UC_BANJO121, 0x31e, 0xba, 0, 0 },
    { UC_SF64_NON, 0x31e, 0xba, 1, 0 },
    { UC_TS2_DEX, 0x31e, 0xba, 0, 0 },
    { UC_TS2_DLX, 0x31e, 0xba, 0, 1 },
    { UC_MK64_DEX, 0x31c, 0xbe, 0, 0 },
    { UC_MK64_DLX, 0x31c, 0xbe, 0, 1 },
};
static const struct build *build_of(const struct armlet_env *e)
{
    for (unsigned i = 0; i < sizeof(builds) / sizeof(builds[0]); i++)
        if (builds[i].id == e->routine->ucode)
            return &builds[i];
    return &builds[0];
}

#define D8(a) armlet_dm8(&e->rsp, (a))
#define D16(a) armlet_dm16(&e->rsp, (a))
#define S16(a) ((int16_t)armlet_dm16(&e->rsp, (a)))
#define D32(a) armlet_dm32(&e->rsp, (a))
#define SB(a) ((int8_t)armlet_dm8(&e->rsp, (a)))

struct vtx_out
{
    int Pi[4];            /* clip space position, integer halves (x y z w) */
    int P21[4];           /* the integer halves as stored (v21) and projected */
    unsigned Pf[4];       /* fractions */
    uint32_t colour;      /* the word stored at +16 */
    int16_t st[2];        /* the s,t stored at +20 */
    int scr[3];           /* screen x y z (int halves, x y clamped) */
    unsigned scrzf;       /* screen z fraction */
    int rinv_i;           /* v27 lane: raw integer half of 1/w, stored at +32 */
    unsigned rinv_f;      /* fraction of 1/w, +34 */
    unsigned f1, f2;      /* clip flag groups: first compare (x y z, lo bits 0..2 / hi bits 8..10), second compare */
    int wneg;             /* sign of the integer half of w (NoN) */
    int scry_st;          /* the y stored at +26 (F3DLX: rounded) */
    int scrx_v6;          /* v6 lane 0 after the clamp (stale value for the next pair) */
};

/* one vertex; h = 0 for the first vertex of the pair (lanes 0..3), 1 for the second (lanes 4..7); in = DMEM address of its 16 input bytes */
static void do_vertex(struct armlet_env *e, const struct build *bd, int h, unsigned in, int r3, uint32_t L, int *stale6, int vco_carry, struct vtx_out *o)
{
    /* ---- position into clip space: P = [x y z 1] * M, M = 4x4 S15.16 at 0x460 (integers, rows of 4 halves) + 0x480 (fractions) ---- */
    int x[3] = { S16(in), S16(in + 2), S16(in + 4) };
    for (int k = 0; k < 4; k++)
    {
        i64 acc = 0;
        for (int j = 0; j < 3; j++)
        {
            acc = w48(acc + (i64)D16(0x480 + 8 * j + 2 * k) * x[j]);
            acc = w48(acc + (((i64)S16(0x460 + 8 * j + 2 * k) * x[j]) << 16));
        }
        acc = w48(acc + D16(0x480 + 24 + 2 * k));
        o->Pf[k] = uclamp(acc);
        acc = w48(acc + (((i64)S16(0x460 + 24 + 2 * k)) << 16));
        o->Pi[k] = sclamp(acc);
    }
    int clip = !bd->dlx || (r3 & 128);
    for (int k = 0; k < 4; k++)                 /* v21 = v29 + VCO carry; the carry is 0 after VCL, stale (the entry VCO) when F3DLX skips the clip flags */
        o->P21[k] = clip ? o->Pi[k] : cl16((i64)o->Pi[k] + ((vco_carry >> (4 * h + k)) & 1));
    /* ---- clip flags ---- */
    int ratio = S16(0x76);
    int Ri[4];
    unsigned Rf[4];
    for (int k = 0; k < 4; k++)
    {
        Rf[k] = (uint16_t)((i64)o->Pf[k] * ratio);
        Ri[k] = sclamp(w48((i64)o->Pf[k] * ratio + (((i64)o->Pi[k] * ratio) << 16)));
    }
    unsigned f1 = 0, f2 = 0;
    for (int k = 0; k < 3 && clip; k++)
    {
        unsigned a = clipcmp(o->Pi[k], o->Pf[k], o->Pi[3], o->Pf[3]);
        unsigned b = clipcmp(o->Pi[k], o->Pf[k], Ri[3], Rf[3]);
        f1 |= ((bd->non && k == 2) ? 0 : (a & 1)) << k | (a >> 1) << (8 + k);   /* NoN: no "z <= -w" bit in the first compare */
        f2 |= (b & 1) << k | (b >> 1) << (8 + k);
    }
    o->f1 = f1;
    o->f2 = f2;
    o->wneg = o->Pi[3] < 0;
    /* ---- 1/w ---- */
    unsigned pn = D16(0x112);
    i64 acc = ((i64)o->Pf[3] * pn) >> 16;
    acc = w48(acc + (i64)o->Pi[3] * pn);
    int Wi = sclamp(acc);
    unsigned Wf = uclamp(acc);
    int Rinv_i;
    unsigned Rinv_f;
    recip_refine(Wi, Wf, &Rinv_i, &Rinv_f);
    o->rinv_i = Rinv_i;
    o->rinv_f = Rinv_f;
    int Rc_i = Rinv_i >= 0 ? Rinv_i : 0x7fff;
    /* ---- Q = P / w (x y z) ---- */
    int Qi[3];
    unsigned Qf[3];
    for (int k = 0; k < 3; k++)
    {
        int q0i;
        unsigned q0f;
        mul32(o->P21[k], o->Pf[k], Rc_i, Rinv_f, &q0i, &q0f);
        i64 a = ((i64)q0f * pn) >> 16;
        a = w48(a + (i64)q0i * pn);
        Qi[k] = sclamp(a);
        Qf[k] = uclamp(a);
    }
    /* ---- screen position: trans + Q * scale (scale y negated), clamped ---- */
    for (int k = 0; k < 3; k++)
    {
        int sc = cl16((i64)S16(0x380 + 2 * k) * (k == 1 ? -1 : 1));
        int tr = S16(0x388 + 2 * k);
        i64 a = w48(((i64)tr) << 16);
        a = w48(a + (i64)Qf[k] * sc);
        a = w48(a + (((i64)Qi[k] * sc) << 16));
        int v6 = sclamp(a);
        unsigned v7 = uclamp(a);
        if (k < 2)
        {
            if (v6 < S16(0x2a)) v6 = S16(0x2a);
            if (v6 > S16(0x28)) v6 = S16(0x28);
        }
        else
        {
            if (v6 < S16(0x2e)) v6 = S16(0x2e);
            if (v6 > S16(0x2c)) v6 = S16(0x2c);
            o->scrzf = v7;
        }
        o->scr[k] = v6;
    }
    o->scrx_v6 = o->scr[0];
    /* ---- texture coordinates and colour: the vertex's own, or lit / texgen ---- */
    uint32_t w = D32(in + 12);                  /* r g b a, or nx ny nz a */
    int st[2] = { S16(in + 8), S16(in + 10) };
    if (r3 & 2)
    {
        int n[3] = { (int8_t)(w >> 24), (int8_t)(w >> 16), (int8_t)(w >> 8) };
        int amb[4], v27[4];
        for (int j = 0; j < 4; j++)
            amb[j] = D8(0x1d0 + L + 4 * h + j) << 7;
        int v7[4];
        for (int j = 0; j < 4; j++)
        {
            v27[j] = 0;
            v7[j] = amb[j];
        }
        int d6 = *stale6;                       /* v6 lane 0 / 4 at the top of the iteration (the previous dot product) */
        int Dprev = 0, Dcur = 0, col0[4] = { 0, 0, 0, 0 };
        for (int r1 = (int)L;; r1 -= 32)
        {
            for (int j = 0; j < 4; j++)
                v27[j] = cl16((i64)v27[j] + (v7[j] > 0 ? v7[j] : 0));
            int p[3];
            for (int k = 0; k < 3; k++)
                p[k] = vmulf(n[k] << 8, SB(0x1c0 + r1 + 4 * h + k) << 8);
            Dprev = d6;
            Dcur = cl16((i64)cl16((i64)p[0] + p[1]) + p[2]);
            d6 = Dcur;
            for (int j = 0; j < 4; j++)
            {
                col0[j] = D8(0x1b0 + r1 + 4 * h + j) << 7;
                v7[j] = vmulf(col0[j], Dcur);
            }
            if (r1 <= 0)
                break;
        }
        uint32_t c = 0;
        for (int j = 0; j < 3; j++)
            c = c << 8 | (uint32_t)((v27[j] >> 7) & 0xff);
        w = c << 8 | (w & 0xff);
        if (r3 & 4)
        {
            for (int j = 0; j < 2; j++)
            {
                i64 a = w48((i64)2 * col0[j] * Dcur + 0x8000);               /* the last VMULF */
                a = w48(a + (i64)(D8(0x1a0 + 4 * h + j) << 8) * Dprev);      /* VMADN with the previous dot product (unsigned lane x signed) */
                int v = sclamp(a);
                if (r3 & 8)
                {
                    int sq = vmulf(v, v);
                    int cu = vmulf(sq, v);
                    i64 b = w48((i64)2 * cu * S16(0xa2) + 0x8000);
                    b = w48(b + (i64)2 * cu * S16(0xa6));
                    b = w48(b + (i64)2 * v * S16(0xa4));
                    v = sclamp(b);
                }
                st[j] = cl16((i64)v + S16(0x38));
            }
        }
    }
    unsigned sc16[2] = { D16(0x124), D16(0x126) };
    for (int j = 0; j < 2; j++)
        o->st[j] = (int16_t)cl16(((i64)st[j] * (i64)sc16[j]) >> 16);
    /* ---- fog: alpha byte ---- */
    if (r3 & 1)
    {
        int fm = S16(0x390), fo = S16(0x392), hi = S16(0x394);
        i64 a = w48((i64)Qf[2] * fm);
        a = w48(a + (((i64)Qi[2] * fm) << 16));
        int f = sclamp(a);
        f = cl16((i64)f + fo);
        if (f < 0) f = 0;
        if (f > hi) f = hi;
        w = (w & ~0xffu) | ((unsigned)f & 0xff);
    }
    o->colour = w;
}

static int spec_vtx(struct armlet_env *e)
{
    const struct build *bd = build_of(e);
    uint32_t *sr = e->rsp.sr;
    int r3 = (int16_t)D16(0x114);
    uint32_t L = D32(0x12c);
    if ((r3 & 2) && (int32_t)L < 0)
        return ARMLET_NATIVE_DECLINE;           /* the light-direction recomputation (0x7c4..) is documented separately */
    unsigned r22 = sr[22];
    unsigned r7 = armlet_dm16(&e->rsp, bd->tab + sr[1]);
    int r9 = (int)(armlet_dm8(&e->rsp, sr[27] - 6) >> 2);
    int stale6[2] = { (int16_t)e->rsp.vr[6][0], (int16_t)e->rsp.vr[6][4] };
    int clip = !bd->dlx || (r3 & 128);
    /* VCO carry bits (lane n = bit n) at the entry: only the first pair of an unlit F3DLX call without G_CLIPPING sees them (VGE of the light loop and VCL clear VCO) */
    int vco = 0;
    for (int n = 0; n < 8; n++)
        vco |= (e->rsp.flags[0][8 + n] ? 1 : 0) << n;
    if (r3 & 2)
        vco = 0;
    uint32_t r8 = 0x460;                        /* GPR $8 (the matrix address left by the matrix load): the F3DLX no-clip flags accumulate in it */
    uint32_t r13 = sr[13], r14 = sr[14];
    armlet_st16(&e->rsp, 0x106, D16(bd->ret));
    for (;;)
    {
        struct vtx_out a, b;
        int s0 = stale6[0], s1 = stale6[1];
        do_vertex(e, bd, 0, r22, r3, L, &s0, vco, &a);
        do_vertex(e, bd, 1, r22 + 16, r3, L, &s1, vco, &b);
        vco = 0;
        struct vtx_out *vo[2] = { &a, &b };
        uint32_t fw;                            /* the flag word r8: low half the first vertex's, high half the second's */
        if (clip)
        {
            unsigned fa = a.f2 | a.f1 << 4, fb = b.f2 | b.f1 << 4;
            if (bd->non)
            {
                fb |= (unsigned)a.wneg << 2 | (unsigned)b.wneg << 6;
            }
            fw = fb << 16 | fa;
        }
        else
        {
            if (r3 & 2)                         /* the lit block leaves $8 = (texgen ? r3 & 8 : 0) */
                r8 = (r3 & 4) ? (uint32_t)(r3 & 8) : 0;
            r14 = (r14 & 0x7070) << 12;         /* $14 is updated in place: only the first pair sees the entry value */
            r8 = r14 | r8;
            r8 |= r13;
            fw = r8;
        }
        r9 -= 2;
        for (int h = 0; h < 2; h++)
        {
            if (h == 1 && r9 < 0)
                break;
            struct vtx_out *o = vo[h];
            unsigned r = r7 + 40 * h;
            for (int k = 0; k < 4; k++)
            {
                armlet_st16(&e->rsp, r + 2 * k, o->P21[k]);
                armlet_st16(&e->rsp, r + 8 + 2 * k, o->Pf[k]);
            }
            armlet_st32(&e->rsp, r + 16, o->colour);
            armlet_st16(&e->rsp, r + 20, o->st[0]);
            armlet_st16(&e->rsp, r + 22, o->st[1]);
            armlet_st16(&e->rsp, r + 24, o->scr[0]);
            armlet_st16(&e->rsp, r + 26, bd->dlx ? ((o->scr[1] + 2) & 0xfffc) : o->scr[1]);
            armlet_st16(&e->rsp, r + 28, o->scr[2]);
            armlet_st16(&e->rsp, r + 30, o->scrzf);
            armlet_st16(&e->rsp, r + 32, o->rinv_i);
            armlet_st16(&e->rsp, r + 34, o->rinv_f);
            if (h == 0)
                armlet_st16(&e->rsp, r + 36, fw & 0xffff);
            else
                armlet_st32(&e->rsp, r + 36, fw);
        }
        stale6[0] = a.scrx_v6;
        stale6[1] = b.scrx_v6;
        r22 += 32;
        r7 += 80;
        r8 = (uint32_t)(int32_t)(int16_t)D16(0x106);      /* the delay slot of the loop-back branch reloads $8 */
        if (r9 <= 0)
            break;
    }
    return D16(0x106) & 0xfff;
}
SPEC_REG(spec_vtx_bk, "spec.f3dex.light.vtx", UC_BANJO121, 0x3a8, 0, spec_vtx);
SPEC_REG(spec_vtx_non, "spec.f3dex.light.vtx", UC_SF64_NON, 0x3a8, 0, spec_vtx);
SPEC_REG(spec_vtx_ts2, "spec.f3dex.light.vtx", UC_TS2_DEX, 0x3a0, 0, spec_vtx);
SPEC_REG(spec_vtx_ts2dlx, "spec.f3dex.light.vtx", UC_TS2_DLX, 0x3b0, 0, spec_vtx);
SPEC_REG(spec_vtx_mk, "spec.f3dex.light.vtx", UC_MK64_DEX, 0x430, 0, spec_vtx);
SPEC_REG(spec_vtx_mkdlx, "spec.f3dex.light.vtx", UC_MK64_DLX, 0x440, 0, spec_vtx);
