/*
 * arMLEt - ref_vu.h (Fleet B's f3dex_vu.h): exact RSP vector-unit operations, here the reference for tools/hle/prim_test.c
 *
 * Every operation reproduces the fork's helpers (ext/parallel-rsp rsp/vfunctions.cpp, rsp/ls.cpp, rsp/cp2.cpp) bit for bit: result
 * register, accumulator, VCO / VCC / VCE, the reciprocal unit and DMEM. tools/native-f3dex/vu_test.cpp proves it (see build_vu_test.sh).
 *
 * Use: vu_load() the state, run the instruction macros with constant register numbers and element selectors, vu_store() (or
 * vu_store_masked() with constant dead masks). Everything is static inline / always_inline, so after inlining a `struct vu` that does
 * not escape lives in registers; build with optimization (correct at any level, but only -O2 or better folds the register numbers and E
 * and keeps the state in registers).
 *
 * Two backends with the same results: NEON (aarch64) and a portable scalar one (plain loops over the 8 lanes, the readable reference).
 * Define VU_FORCE_SCALAR to get the scalar one on aarch64.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ARMLET_F3DEX_VU_H
#define ARMLET_F3DEX_VU_H

#include <stdint.h>
#include <string.h>

#if defined(__ARM_NEON) && defined(__aarch64__) && !defined(VU_FORCE_SCALAR)
#define VU_NEON 1
#include <arm_neon.h>
/* lane i = element i of an RSP vector (element 0 is the first halfword in memory order) */
typedef uint16x8_t vec_t;
#else
#define VU_NEON 0
typedef struct { uint16_t e[8]; } vec_t;
#endif

#define VU_AI static inline __attribute__((always_inline))
#if defined(__clang__)
#define VU_NOUNROLL _Pragma("clang loop unroll(disable)")
#else
#define VU_NOUNROLL _Pragma("GCC unroll 1")
#endif

/* Deliberate breakage for the test's mutation check (-DVU_MUTATE=n): VU_MUT(n, correct, broken). */
#ifdef VU_MUTATE
#define VU_MUT(n, ok, bad) ((VU_MUTATE) == (n) ? (bad) : (ok))
#else
#define VU_MUT(n, ok, bad) (ok)
#endif

/* The state. Flags are lane masks (0 or 0xffff): *_lo = flag bits 0..7 (lane i = bit i), *_hi = bits 8..15 (lane i = bit 8 + i);
 * vce is the 8-bit VCE. Accumulator: bits 47..32, 31..16, 15..0 per lane. */
struct vu {
    vec_t r[32];
    vec_t acc_hi, acc_md, acc_lo;
    vec_t vco_lo, vco_hi, vcc_lo, vcc_hi, vce;
    int16_t div_out, div_in;
    int8_t dp_flag;
    uint32_t *dmem;                     /* from vu_io, for the loads and stores */
};

/* The host's state (the same pointers as struct armlet_rsp in native.h): vr[n][i]; acc[0..7] HI, [8..15] MD, [16..23] LO; flags[0..2] =
 * VCO, VCC, VCE as 16 lanes of 0 / 0xffff where flag bit n (0..7) is lane 8 + n and bit 8 + n is lane n (VCE: lanes 8..15 only);
 * dmem[w] = word w of DMEM as the big-endian value in a host uint32 (byte a is ((uint8_t *)dmem)[(a ^ 3) & 0xfff]). */
struct vu_io {
    uint16_t (*vr)[8];
    uint16_t *acc;
    uint16_t (*flags)[16];
    int16_t *div_out, *div_in;
    int8_t *dp_flag;
    uint32_t *dmem;
};
/* struct vu_io io = VU_IO_INIT(&env->rsp);   (any struct with the armlet_rsp member names) */
#define VU_IO_INIT(r) { (r)->vr, (r)->acc, (r)->flags, (r)->div_out, (r)->div_in, (r)->dp_flag, (r)->dmem }
#define VU_IO_FROM(r) ((struct vu_io)VU_IO_INIT(r))

/* ---------------------------------------------------------------------------------------------------------------------------------
 * Lane access, element selection
 * ------------------------------------------------------------------------------------------------------------------------------- */
#if VU_NEON

#define VU_S(x) vreinterpretq_s16_u16(x)
#define VU_U(x) vreinterpretq_u16_s16(x)

VU_AI vec_t vu_zero(void) { return vdupq_n_u16(0); }
VU_AI vec_t vu_dup(uint16_t x) { return vdupq_n_u16(x); }
VU_AI vec_t vu_ld(const uint16_t *p) { return vld1q_u16(p); }
VU_AI void vu_st(uint16_t *p, vec_t v) { vst1q_u16(p, v); }

VU_AI uint16_t vu_get(vec_t v, int i)
{
    switch (i & 7) {
    case 0: return vgetq_lane_u16(v, 0);
    case 1: return vgetq_lane_u16(v, 1);
    case 2: return vgetq_lane_u16(v, 2);
    case 3: return vgetq_lane_u16(v, 3);
    case 4: return vgetq_lane_u16(v, 4);
    case 5: return vgetq_lane_u16(v, 5);
    case 6: return vgetq_lane_u16(v, 6);
    default: return vgetq_lane_u16(v, 7);
    }
}

VU_AI vec_t vu_set(vec_t v, int i, uint16_t x)
{
    switch (i & 7) {
    case 0: return vsetq_lane_u16(x, v, 0);
    case 1: return vsetq_lane_u16(x, v, 1);
    case 2: return vsetq_lane_u16(x, v, 2);
    case 3: return vsetq_lane_u16(x, v, 3);
    case 4: return vsetq_lane_u16(x, v, 4);
    case 5: return vsetq_lane_u16(x, v, 5);
    case 6: return vsetq_lane_u16(x, v, 6);
    default: return vsetq_lane_u16(x, v, 7);
    }
}

/* vt[E]: 0,1 as is; 2 = (0,0,2,2,4,4,6,6); 3 = (1,1,3,3,5,5,7,7); 4 = (0,0,0,0,4,4,4,4); 5 = (1,1,1,1,5,5,5,5); 6 = (2,2,2,2,6,6,6,6);
 * 7 = (3,3,3,3,7,7,7,7); 8..15 = lane E - 8 broadcast. */
VU_AI vec_t vu_elem_(vec_t v, int e)
{
    switch (e & 15) {
    case 0: case 1: return v;
    case 2: return vtrn1q_u16(v, v);
    case 3: return vtrn2q_u16(v, v);
    case 4: { uint32x4_t t = vreinterpretq_u32_u16(vtrn1q_u16(v, v)); return vreinterpretq_u16_u32(vtrn1q_u32(t, t)); }
    case 5: { uint32x4_t t = vreinterpretq_u32_u16(vtrn2q_u16(v, v)); return vreinterpretq_u16_u32(vtrn1q_u32(t, t)); }
    case 6: { uint32x4_t t = vreinterpretq_u32_u16(vtrn1q_u16(v, v)); return vreinterpretq_u16_u32(vtrn2q_u32(t, t)); }
    case 7: { uint32x4_t t = vreinterpretq_u32_u16(vtrn2q_u16(v, v)); return vreinterpretq_u16_u32(vtrn2q_u32(t, t)); }
    case 8: return vdupq_laneq_u16(v, 0);
    case 9: return vdupq_laneq_u16(v, 1);
    case 10: return vdupq_laneq_u16(v, 2);
    case 11: return vdupq_laneq_u16(v, 3);
    case 12: return vdupq_laneq_u16(v, 4);
    case 13: return vdupq_laneq_u16(v, 5);
    case 14: return vdupq_laneq_u16(v, 6);
    default: return vdupq_laneq_u16(v, 7);
    }
}

#else /* scalar */

VU_AI vec_t vu_zero(void) { vec_t v; memset(&v, 0, sizeof v); return v; }
VU_AI vec_t vu_dup(uint16_t x) { vec_t v; for (int i = 0; i < 8; i++) v.e[i] = x; return v; }
VU_AI vec_t vu_ld(const uint16_t *p) { vec_t v; memcpy(v.e, p, 16); return v; }
VU_AI void vu_st(uint16_t *p, vec_t v) { memcpy(p, v.e, 16); }
VU_AI uint16_t vu_get(vec_t v, int i) { return v.e[i & 7]; }
VU_AI vec_t vu_set(vec_t v, int i, uint16_t x) { v.e[i & 7] = x; return v; }

VU_AI vec_t vu_elem_(vec_t v, int e)
{
    static const uint8_t sel[16][8] = {
        { 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 0, 2, 2, 4, 4, 6, 6 }, { 1, 1, 3, 3, 5, 5, 7, 7 },
        { 0, 0, 0, 0, 4, 4, 4, 4 }, { 1, 1, 1, 1, 5, 5, 5, 5 }, { 2, 2, 2, 2, 6, 6, 6, 6 }, { 3, 3, 3, 3, 7, 7, 7, 7 },
        { 0, 0, 0, 0, 0, 0, 0, 0 }, { 1, 1, 1, 1, 1, 1, 1, 1 }, { 2, 2, 2, 2, 2, 2, 2, 2 }, { 3, 3, 3, 3, 3, 3, 3, 3 },
        { 4, 4, 4, 4, 4, 4, 4, 4 }, { 5, 5, 5, 5, 5, 5, 5, 5 }, { 6, 6, 6, 6, 6, 6, 6, 6 }, { 7, 7, 7, 7, 7, 7, 7, 7 },
    };
    vec_t o;
    for (int i = 0; i < 8; i++) o.e[i] = v.e[sel[e & 15][i]];
    return o;
}

#endif /* backend */

#define vu_elem(v, E) vu_elem_((v), (E))

/* ---------------------------------------------------------------------------------------------------------------------------------
 * load / store of the whole state
 * ------------------------------------------------------------------------------------------------------------------------------- */
#define VU_X32(X) \
    X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) \
    X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31)

VU_AI void vu_load(struct vu *u, const struct vu_io *io)
{
#define X(n) u->r[n] = vu_ld(io->vr[n]);
    VU_X32(X)
#undef X
    u->acc_hi = vu_ld(io->acc);
    u->acc_md = vu_ld(io->acc + 8);
    u->acc_lo = vu_ld(io->acc + 16);
    u->vco_hi = vu_ld(io->flags[0]);
    u->vco_lo = vu_ld(io->flags[0] + 8);
    u->vcc_hi = vu_ld(io->flags[1]);
    u->vcc_lo = vu_ld(io->flags[1] + 8);
    u->vce = vu_ld(io->flags[2] + 8);
    u->div_out = *io->div_out;
    u->div_in = *io->div_in;
    u->dp_flag = *io->dp_flag;
    u->dmem = io->dmem;
}

/* dead_vr_mask: bit n = register n is dead (not written back). dead_acc bits: 1 HI, 2 MD, 4 LO. dead_flags: 1 VCO, 2 VCC, 4 VCE.
 * dead_div: 1 div_out, 2 div_in, 4 dp_flag. All constants in the intended use, so dead state costs nothing after inlining. */
VU_AI void vu_store_masked_ex(const struct vu *u, const struct vu_io *io, uint32_t dead_vr_mask, unsigned dead_acc, unsigned dead_flags,
                              unsigned dead_div)
{
#define X(n) if (!((dead_vr_mask >> (n)) & 1)) vu_st(io->vr[n], u->r[n]);
    VU_X32(X)
#undef X
    if (!(dead_acc & 1)) vu_st(io->acc, u->acc_hi);
    if (!(dead_acc & 2)) vu_st(io->acc + 8, u->acc_md);
    if (!(dead_acc & 4)) vu_st(io->acc + 16, u->acc_lo);
    if (!(dead_flags & 1)) { vu_st(io->flags[0], u->vco_hi); vu_st(io->flags[0] + 8, u->vco_lo); }
    if (!(dead_flags & 2)) { vu_st(io->flags[1], u->vcc_hi); vu_st(io->flags[1] + 8, u->vcc_lo); }
    if (!(dead_flags & 4)) vu_st(io->flags[2] + 8, u->vce);
    if (!(dead_div & 1)) *io->div_out = u->div_out;
    if (!(dead_div & 2)) *io->div_in = u->div_in;
    if (!(dead_div & 4)) *io->dp_flag = u->dp_flag;
}

/* like the dead_* bit fields of struct armlet_routine: nonzero = the whole group is dead */
VU_AI void vu_store_masked(const struct vu *u, const struct vu_io *io, uint32_t dead_vr_mask, unsigned dead_acc, unsigned dead_flags,
                           unsigned dead_div)
{
    vu_store_masked_ex(u, io, dead_vr_mask, dead_acc ? 7 : 0, dead_flags ? 7 : 0, dead_div ? 7 : 0);
}

VU_AI void vu_store(const struct vu *u, const struct vu_io *io) { vu_store_masked_ex(u, io, 0, 0, 0, 0); }

/* ---------------------------------------------------------------------------------------------------------------------------------
 * Arithmetic: scalar backend (the reference: the SSE templates of arch/simd/rsp vXXX.h written lane by lane)
 * ------------------------------------------------------------------------------------------------------------------------------- */
#if !VU_NEON

#define VU_LANES for (int i = 0; i < 8; i++)

VU_AI int vs_carry(uint16_t a, uint16_t b) { return ((uint32_t)a + b) > 0xffffu; }
VU_AI uint16_t vs_mulhi_s(uint16_t a, uint16_t b) { return (uint16_t)(((int32_t)(int16_t)a * (int16_t)b) >> 16); }
VU_AI uint16_t vs_mulhi_u(uint16_t a, uint16_t b) { return (uint16_t)(((uint32_t)a * b) >> 16); }
VU_AI uint16_t vs_sat16(int32_t x) { return x > 32767 ? 0x7fff : x < -32768 ? 0x8000 : (uint16_t)x; }
VU_AI uint16_t vs_sra15(uint16_t x) { return (x & 0x8000) ? 0xffff : 0; }
/* signed saturation of the 32-bit value hi:md */
VU_AI uint16_t vs_sclamp(uint16_t md, uint16_t hi) { return vs_sat16((int32_t)(((uint32_t)hi << 16) | md)); }
/* lo when the accumulator fits a signed 32-bit value, else 0xffff (positive) / 0 (negative) */
VU_AI uint16_t vs_uclamp(uint16_t lo, uint16_t md, uint16_t hi)
{
    uint16_t hn = vs_sra15(hi);
    if (hn == hi && hn == vs_sra15(md)) return lo;
    return hn == 0 ? 0xffff : 0;
}

static inline __attribute__((always_inline)) void vu_k_mulf(struct vu *u, vec_t vs, vec_t vt, int vmulu, vec_t *res)
{
    VU_LANES {
        uint16_t a = vs.e[i], b = vt.e[i];
        uint16_t lo = (uint16_t)(a * b);
        uint16_t sign1 = lo >> 15;
        lo = (uint16_t)(lo + lo);
        uint16_t hi = vs_mulhi_s(a, b);
        uint16_t sign2 = lo >> 15;
        u->acc_lo.e[i] = (uint16_t)(VU_MUT(8, 0x8000, 0) + lo);
        sign1 = (uint16_t)(sign1 + sign2);
        hi = (uint16_t)(hi << 1);
        uint16_t eq = a == b ? 0xffff : 0;
        uint16_t md = (uint16_t)(hi + sign1);
        uint16_t neg = vs_sra15(md);
        u->acc_md.e[i] = md;
        if (vmulu) {
            uint16_t h = (uint16_t)(~eq & neg);
            u->acc_hi.e[i] = h;
            res->e[i] = (uint16_t)(~h & (md | neg));
        } else {
            u->acc_hi.e[i] = (uint16_t)(~eq & neg);
            res->e[i] = (uint16_t)(md + (eq & neg));
        }
    }
}

/* VMUDN / VMADN */
VU_AI vec_t vu_k_madn(struct vu *u, vec_t vs, vec_t vt, int acc)
{
    vec_t res;
    VU_LANES {
        uint16_t a = vs.e[i], b = vt.e[i];
        uint16_t lo = (uint16_t)(a * b);
        uint16_t hi = vs_mulhi_u(a, b);
        if (b & 0x8000) hi = (uint16_t)(hi - a);
        if (acc) {
            int c = vs_carry(u->acc_lo.e[i], lo);
            u->acc_lo.e[i] = (uint16_t)(u->acc_lo.e[i] + lo);
            hi = (uint16_t)(hi + VU_MUT(1, c, 0));
            c = vs_carry(u->acc_md.e[i], hi);
            u->acc_md.e[i] = (uint16_t)(u->acc_md.e[i] + hi);
            u->acc_hi.e[i] = (uint16_t)(u->acc_hi.e[i] + vs_sra15(hi) + c);
            res.e[i] = vs_uclamp(u->acc_lo.e[i], u->acc_md.e[i], u->acc_hi.e[i]);
        } else {
            u->acc_lo.e[i] = lo;
            u->acc_md.e[i] = hi;
            u->acc_hi.e[i] = vs_sra15(hi);
            res.e[i] = lo;
        }
    }
    return res;
}

/* VMUDM / VMADM */
VU_AI vec_t vu_k_madm(struct vu *u, vec_t vs, vec_t vt, int acc)
{
    vec_t res;
    VU_LANES {
        uint16_t a = vs.e[i], b = vt.e[i];
        uint16_t lo = (uint16_t)(a * b);
        uint16_t hi = vs_mulhi_u(a, b);
        if (a & 0x8000) hi = (uint16_t)(hi - b);
        if (acc) {
            int c = vs_carry(u->acc_lo.e[i], lo);
            u->acc_lo.e[i] = (uint16_t)(u->acc_lo.e[i] + lo);
            hi = (uint16_t)(hi + c);
            c = vs_carry(u->acc_md.e[i], hi);
            u->acc_md.e[i] = (uint16_t)(u->acc_md.e[i] + hi);
            u->acc_hi.e[i] = (uint16_t)(u->acc_hi.e[i] + vs_sra15(hi) + c);
            res.e[i] = vs_sclamp(u->acc_md.e[i], u->acc_hi.e[i]);
        } else {
            u->acc_lo.e[i] = lo;
            u->acc_md.e[i] = hi;
            u->acc_hi.e[i] = vs_sra15(hi);
            res.e[i] = hi;
        }
    }
    return res;
}

/* VMUDH / VMADH */
VU_AI vec_t vu_k_madh(struct vu *u, vec_t vs, vec_t vt, int acc)
{
    vec_t res;
    VU_LANES {
        uint16_t a = vs.e[i], b = vt.e[i];
        uint16_t lo = (uint16_t)(a * b);
        uint16_t hi = vs_mulhi_s(a, b);
        if (acc) {
            int c = vs_carry(u->acc_md.e[i], lo);
            u->acc_md.e[i] = (uint16_t)(u->acc_md.e[i] + lo);
            hi = (uint16_t)(hi + c);
            u->acc_hi.e[i] = (uint16_t)(u->acc_hi.e[i] + hi);
        } else {
            u->acc_lo.e[i] = 0;
            u->acc_md.e[i] = lo;
            u->acc_hi.e[i] = hi;
        }
        res.e[i] = vs_sclamp(u->acc_md.e[i], u->acc_hi.e[i]);
    }
    return res;
}

/* VMUDL / VMADL */
VU_AI vec_t vu_k_madl(struct vu *u, vec_t vs, vec_t vt, int acc)
{
    vec_t res;
    VU_LANES {
        uint16_t hi = vs_mulhi_u(vs.e[i], vt.e[i]);
        if (acc) {
            int c = vs_carry(u->acc_lo.e[i], hi);
            u->acc_lo.e[i] = (uint16_t)(u->acc_lo.e[i] + hi);
            int c2 = vs_carry(u->acc_md.e[i], (uint16_t)c);
            u->acc_md.e[i] = (uint16_t)(u->acc_md.e[i] + c);
            u->acc_hi.e[i] = (uint16_t)(u->acc_hi.e[i] + c2);
            res.e[i] = vs_uclamp(u->acc_lo.e[i], u->acc_md.e[i], u->acc_hi.e[i]);
        } else {
            u->acc_lo.e[i] = hi;
            u->acc_md.e[i] = 0;
            u->acc_hi.e[i] = 0;
            res.e[i] = hi;
        }
    }
    return res;
}

/* VMACF / VMACU */
VU_AI vec_t vu_k_macf(struct vu *u, vec_t vs, vec_t vt, int vmacu)
{
    vec_t res;
    VU_LANES {
        uint16_t a = vs.e[i], b = vt.e[i];
        uint16_t lo = (uint16_t)(a * b);
        uint16_t hi = vs_mulhi_s(a, b);
        uint16_t md = (uint16_t)((hi << 1) | (lo >> 15));
        hi = vs_sra15(hi);
        lo = (uint16_t)(lo << 1);
        int c = vs_carry(u->acc_lo.e[i], lo);
        u->acc_lo.e[i] = (uint16_t)(u->acc_lo.e[i] + lo);
        md = (uint16_t)(md + c);
        if (md == 0 && c) hi = (uint16_t)(hi + 1);
        c = vs_carry(u->acc_md.e[i], md);
        u->acc_md.e[i] = (uint16_t)(u->acc_md.e[i] + md);
        u->acc_hi.e[i] = (uint16_t)(u->acc_hi.e[i] + hi + c);
        uint16_t ah = u->acc_hi.e[i], am = u->acc_md.e[i];
        if (vmacu) {
            uint16_t m = (uint16_t)(vs_sra15(am) | am);
            m = (uint16_t)(m & ~vs_sra15(ah));
            res.e[i] = (uint16_t)(((int16_t)ah > 0 ? 0xffff : 0) | m);
        } else {
            res.e[i] = vs_sclamp(am, ah);
        }
    }
    return res;
}

VU_AI vec_t vu_k_abs(vec_t vs, vec_t vt, vec_t *acc_lo)
{
    vec_t res;
    VU_LANES {
        uint16_t s = vs.e[i], sign = vs_sra15(s);
        uint16_t vd = s == 0 ? 0 : vt.e[i];
        vd = (uint16_t)(vd ^ sign);
        acc_lo->e[i] = (uint16_t)(vd - sign);
        res.e[i] = vs_sat16((int32_t)(int16_t)vd - (int16_t)sign);
    }
    return res;
}

VU_AI vec_t vu_k_add(vec_t vs, vec_t vt, vec_t carry, vec_t *acc_lo)
{
    vec_t res;
    VU_LANES {
        int c = carry.e[i] ? 1 : 0;
        acc_lo->e[i] = (uint16_t)(vs.e[i] + vt.e[i] + c);
        res.e[i] = vs_sat16((int32_t)(int16_t)vs.e[i] + (int16_t)vt.e[i] + c);
    }
    return res;
}

VU_AI vec_t vu_k_sub(vec_t vs, vec_t vt, vec_t carry, vec_t *acc_lo)
{
    vec_t res;
    VU_LANES {
        int c = carry.e[i] ? 1 : 0;
        int32_t s = (int16_t)vs.e[i], t = (int16_t)vt.e[i];
        int32_t sat_diff = t + c > 32767 ? 32767 : t + c;               /* subs_epi16(vt, carry mask) */
        int32_t unsat_diff = (int16_t)(uint16_t)(t + c);
        acc_lo->e[i] = (uint16_t)(s - unsat_diff);
        int32_t vd = (int16_t)vs_sat16(s - sat_diff);
        int overflow = sat_diff > unsat_diff;                           /* t + c wrapped: adds_epi16(vd, -1) */
        res.e[i] = VU_MUT(4, vs_sat16(vd - overflow), vs_sat16(vd));
    }
    return res;
}

VU_AI vec_t vu_k_ch(struct vu *u, vec_t vs, vec_t vt)
{
    vec_t res;
    VU_LANES {
        uint16_t a = vs.e[i], b = vt.e[i];
        uint16_t sign = vs_sra15((uint16_t)(a ^ b));
        uint16_t sn = (uint16_t)((b ^ sign) - sign);
        uint16_t diff = (uint16_t)(a - sn);
        uint16_t dz = diff == 0 ? 0xffff : 0;
        uint16_t vt_neg = vs_sra15(b);
        uint16_t gtz = (int16_t)diff > 0 ? 0xffff : 0;
        uint16_t gez = (uint16_t)(gtz | dz);
        uint16_t lez = (uint16_t)~gtz;
        uint16_t ge = sign ? vt_neg : gez;
        uint16_t le = sign ? lez : vt_neg;
        uint16_t vce = (diff == sign) ? sign : 0;
        uint16_t ne = ((dz | vce) == 0) ? 0xffff : 0;
        uint16_t sel = sign ? le : ge;
        u->vcc_hi.e[i] = VU_MUT(2, ge, le);
        u->vcc_lo.e[i] = le;
        u->vco_hi.e[i] = ne;
        u->vco_lo.e[i] = sign;
        u->vce.e[i] = vce;
        res.e[i] = sel ? sn : a;
    }
    return res;
}

VU_AI vec_t vu_k_cl(struct vu *u, vec_t vs, vec_t vt)
{
    vec_t res;
    VU_LANES {
        uint16_t a = vs.e[i], b = vt.e[i];
        uint16_t ge = u->vcc_hi.e[i], le = u->vcc_lo.e[i], eq = u->vco_hi.e[i], sign = u->vco_lo.e[i], vce = u->vce.e[i];
        uint16_t sn = (uint16_t)((b ^ sign) - sign);
        uint16_t diff = (uint16_t)(a - sn);
        uint32_t s = (uint32_t)a + b;
        uint16_t ncarry = diff == (s > 0xffff ? 0xffff : s) ? 0xffff : 0;
        uint16_t nvce = vce == 0 ? 0xffff : 0;
        uint16_t dz = diff == 0 ? 0xffff : 0;
        uint16_t le_eq = (uint16_t)((dz & ncarry & nvce) | (vce & (dz | ncarry)));
        uint16_t ge_eq = b <= a ? 0xffff : 0;
        if (sign & ~eq) le = le_eq;
        if (!(sign | eq)) ge = ge_eq;
        uint16_t mux = sign ? le : ge;
        u->vcc_hi.e[i] = ge;
        u->vcc_lo.e[i] = le;
        u->vco_hi.e[i] = u->vco_lo.e[i] = u->vce.e[i] = 0;
        res.e[i] = mux ? sn : a;
    }
    return res;
}

VU_AI vec_t vu_k_cr(struct vu *u, vec_t vs, vec_t vt)
{
    vec_t res;
    VU_LANES {
        uint16_t a = vs.e[i], b = vt.e[i];
        uint16_t sign = vs_sra15((uint16_t)(a ^ b));
        uint16_t le = vs_sra15((uint16_t)((a & sign) + b));
        uint16_t gez = (uint16_t)(a | sign);
        uint16_t ge = (int16_t)b <= (int16_t)gez ? 0xffff : 0;
        uint16_t snt = (uint16_t)(b ^ sign);
        uint16_t mask = sign ? le : ge;
        u->vcc_hi.e[i] = ge;
        u->vcc_lo.e[i] = le;
        u->vco_hi.e[i] = u->vco_lo.e[i] = u->vce.e[i] = 0;
        res.e[i] = mask ? snt : a;
    }
    return res;
}

#endif /* !VU_NEON */

/* ---------------------------------------------------------------------------------------------------------------------------------
 * Arithmetic: NEON backend (the multiply-accumulate algorithms are those of rsp/vu_neon.h, proven equal to the SSE templates)
 * ------------------------------------------------------------------------------------------------------------------------------- */
#if VU_NEON

VU_AI vec_t vn_mulhi_u(vec_t a, vec_t b)
{
    uint32x4_t l = vmull_u16(vget_low_u16(a), vget_low_u16(b));
    uint32x4_t h = vmull_high_u16(a, b);
    return vuzp2q_u16(vreinterpretq_u16_u32(l), vreinterpretq_u16_u32(h));
}

VU_AI vec_t vn_mulhi_s(vec_t a, vec_t b)
{
    int32x4_t l = vmull_s16(vget_low_s16(VU_S(a)), vget_low_s16(VU_S(b)));
    int32x4_t h = vmull_high_s16(VU_S(a), VU_S(b));
    return vuzp2q_u16(vreinterpretq_u16_s32(l), vreinterpretq_u16_s32(h));
}

VU_AI vec_t vn_ltz(vec_t a) { return vcltzq_s16(VU_S(a)); }                 /* sign mask */
VU_AI vec_t vn_sra15(vec_t a) { return VU_U(vshrq_n_s16(VU_S(a), 15)); }

/* signed saturation of hi:md to 16 bits */
VU_AI vec_t vn_sclamp(vec_t md, vec_t hi)
{
    int16x8_t m = VU_S(md), h = VU_S(hi);
    int32x4_t l = vreinterpretq_s32_s16(vzip1q_s16(m, h));
    int32x4_t uu = vreinterpretq_s32_s16(vzip2q_s16(m, h));
    return VU_U(vqmovn_high_s32(vqmovn_s32(l), uu));
}

/* lo if hi == sign of md (the 48-bit value fits 32 bits signed), else 0xffff for a positive and 0 for a negative accumulator */
VU_AI vec_t vn_uclamp(vec_t lo, vec_t md, vec_t hi)
{
    int16x8_t h = VU_S(hi);
    uint16x8_t fits = vceqq_s16(h, vshrq_n_s16(VU_S(md), 15));
    return vbslq_u16(fits, lo, vcgezq_s16(h));
}

/* a + b, carry out of the 16-bit add as a mask */
VU_AI vec_t vn_addc(vec_t a, vec_t b, vec_t *carry)
{
    vec_t s = vaddq_u16(a, b);
    *carry = vcgtq_u16(a, s);
    return s;
}

VU_AI vec_t vu_k_madn(struct vu *u, vec_t vs, vec_t vt, int acc)
{
    vec_t lo = vmulq_u16(vs, vt);
    vec_t hi = vn_mulhi_u(vs, vt);
    hi = vsubq_u16(hi, vandq_u16(vs, vn_ltz(vt)));                  /* unsigned vs * signed vt */
    if (acc) {
        vec_t c;
        u->acc_lo = vn_addc(u->acc_lo, lo, &c);
        hi = vsubq_u16(hi, VU_MUT(1, c, vdupq_n_u16(0)));
        u->acc_md = vn_addc(u->acc_md, hi, &c);
        u->acc_hi = VU_U(vsraq_n_s16(VU_S(u->acc_hi), VU_S(hi), 15));
        u->acc_hi = vsubq_u16(u->acc_hi, c);
        return vn_uclamp(u->acc_lo, u->acc_md, u->acc_hi);
    }
    u->acc_lo = lo;
    u->acc_md = hi;
    u->acc_hi = vn_sra15(hi);
    return lo;
}

VU_AI vec_t vu_k_madm(struct vu *u, vec_t vs, vec_t vt, int acc)
{
    vec_t lo = vmulq_u16(vs, vt);
    vec_t hi = vn_mulhi_u(vs, vt);
    hi = vsubq_u16(hi, vandq_u16(vt, vn_ltz(vs)));                  /* signed vs * unsigned vt */
    if (acc) {
        vec_t c;
        u->acc_lo = vn_addc(u->acc_lo, lo, &c);
        hi = vsubq_u16(hi, c);
        u->acc_md = vn_addc(u->acc_md, hi, &c);
        u->acc_hi = VU_U(vsraq_n_s16(VU_S(u->acc_hi), VU_S(hi), 15));
        u->acc_hi = vsubq_u16(u->acc_hi, c);
        return vn_sclamp(u->acc_md, u->acc_hi);
    }
    u->acc_lo = lo;
    u->acc_md = hi;
    u->acc_hi = vn_sra15(hi);
    return hi;
}

VU_AI vec_t vu_k_madh(struct vu *u, vec_t vs, vec_t vt, int acc)
{
    vec_t lo = vmulq_u16(vs, vt);
    vec_t hi = vn_mulhi_s(vs, vt);
    if (acc) {
        vec_t c;
        u->acc_md = vn_addc(u->acc_md, lo, &c);
        hi = vsubq_u16(hi, c);
        u->acc_hi = vaddq_u16(u->acc_hi, hi);
    } else {
        u->acc_lo = vdupq_n_u16(0);
        u->acc_md = lo;
        u->acc_hi = hi;
    }
    return vn_sclamp(u->acc_md, u->acc_hi);
}

VU_AI vec_t vu_k_madl(struct vu *u, vec_t vs, vec_t vt, int acc)
{
    vec_t hi = vn_mulhi_u(vs, vt);
    if (acc) {
        vec_t c;
        u->acc_lo = vn_addc(u->acc_lo, hi, &c);
        vec_t md = vsubq_u16(u->acc_md, c);                         /* the carry adds 1; md only overflows when it was 0xffff */
        vec_t c2 = vcgtq_u16(u->acc_md, md);
        u->acc_md = md;
        u->acc_hi = vsubq_u16(u->acc_hi, c2);
        return vn_uclamp(u->acc_lo, u->acc_md, u->acc_hi);
    }
    u->acc_lo = hi;
    u->acc_md = vdupq_n_u16(0);
    u->acc_hi = vdupq_n_u16(0);
    return hi;
}

VU_AI vec_t vu_k_macf(struct vu *u, vec_t vs, vec_t vt, int vmacu)
{
    vec_t lo = vmulq_u16(vs, vt);
    vec_t hi = vn_mulhi_s(vs, vt);
    /* (vs * vt) << 1 as a 48-bit value */
    vec_t md = vsraq_n_u16(vshlq_n_u16(hi, 1), lo, 15);
    hi = vn_sra15(hi);
    lo = vshlq_n_u16(lo, 1);
    vec_t c;
    u->acc_lo = vn_addc(u->acc_lo, lo, &c);
    md = vsubq_u16(md, c);
    hi = vsubq_u16(hi, vandq_u16(vceqzq_u16(md), c));               /* md wrapped to zero by the carry: carry on into hi */
    u->acc_md = vn_addc(u->acc_md, md, &c);
    u->acc_hi = vsubq_u16(vaddq_u16(u->acc_hi, hi), c);
    if (vmacu) {
        vec_t hi_neg = vn_ltz(u->acc_hi), md_neg = vn_ltz(u->acc_md);
        vec_t r = vbicq_u16(vorrq_u16(md_neg, u->acc_md), hi_neg);
        return vorrq_u16(vcgtzq_s16(VU_S(u->acc_hi)), r);
    }
    return vn_sclamp(u->acc_md, u->acc_hi);
}

VU_AI void vu_k_mulf(struct vu *u, vec_t vs, vec_t vt, int vmulu, vec_t *res)
{
    vec_t lo = vmulq_u16(vs, vt);
    vec_t hi = vn_mulhi_s(vs, vt);
    vec_t lo2 = vaddq_u16(lo, lo);
    vec_t sign = vsraq_n_u16(vshrq_n_u16(lo, 15), lo2, 15);         /* carry out of the doubling + carry out of adding the round bit */
    u->acc_lo = veorq_u16(lo2, vdupq_n_u16(VU_MUT(8, 0x8000, 0)));
    vec_t md = vaddq_u16(vshlq_n_u16(hi, 1), sign);
    vec_t neg = vn_ltz(md);
    vec_t eq = vceqq_u16(vs, vt);
    u->acc_md = md;
    u->acc_hi = vbicq_u16(neg, eq);
    if (vmulu)
        *res = vbicq_u16(vorrq_u16(md, neg), u->acc_hi);
    else
        *res = vaddq_u16(md, vandq_u16(eq, neg));
}

VU_AI vec_t vu_k_abs(vec_t vs, vec_t vt, vec_t *acc_lo)
{
    vec_t zero = vceqzq_u16(vs);
    vec_t sign = vn_sra15(vs);
    vec_t vd = veorq_u16(vbicq_u16(vt, zero), sign);                /* 0x8000 with a negative vs: acc_lo 0x8000, vd 0x7fff */
    *acc_lo = vsubq_u16(vd, sign);
    return VU_U(vqsubq_s16(VU_S(vd), VU_S(sign)));
}

VU_AI vec_t vu_k_add(vec_t vs, vec_t vt, vec_t carry, vec_t *acc_lo)
{
    *acc_lo = vsubq_u16(vaddq_u16(vs, vt), carry);
    int16x8_t mn = vqsubq_s16(vminq_s16(VU_S(vs), VU_S(vt)), VU_S(carry));
    return VU_U(vqaddq_s16(mn, vmaxq_s16(VU_S(vs), VU_S(vt))));
}

VU_AI vec_t vu_k_sub(vec_t vs, vec_t vt, vec_t carry, vec_t *acc_lo)
{
    vec_t unsat = vsubq_u16(vt, carry);
    int16x8_t sat = vqsubq_s16(VU_S(vt), VU_S(carry));
    *acc_lo = vsubq_u16(vs, unsat);
    int16x8_t vd = vqsubq_s16(VU_S(vs), sat);
    vec_t overflow = vcgtq_s16(sat, VU_S(unsat));
    return VU_U(vqaddq_s16(vd, VU_MUT(4, VU_S(overflow), vdupq_n_s16(0))));
}

VU_AI vec_t vu_k_ch(struct vu *u, vec_t vs, vec_t vt)
{
    vec_t sign = vn_ltz(veorq_u16(vs, vt));
    vec_t sn = vsubq_u16(veorq_u16(vt, sign), sign);
    vec_t diff = vsubq_u16(vs, sn);
    vec_t dz = vceqzq_u16(diff);
    vec_t vt_neg = vn_ltz(vt);
    vec_t gtz = vcgtzq_s16(VU_S(diff));
    vec_t gez = vorrq_u16(gtz, dz);
    vec_t lez = vmvnq_u16(gtz);
    vec_t ge = vbslq_u16(sign, vt_neg, gez);
    vec_t le = vbslq_u16(sign, lez, vt_neg);
    vec_t vce = vandq_u16(vceqq_u16(diff, sign), sign);
    u->vcc_hi = VU_MUT(2, ge, le);
    u->vcc_lo = le;
    u->vco_hi = vmvnq_u16(vorrq_u16(dz, vce));
    u->vco_lo = sign;
    u->vce = vce;
    return vbslq_u16(vbslq_u16(sign, le, ge), sn, vs);
}

VU_AI vec_t vu_k_cl(struct vu *u, vec_t vs, vec_t vt)
{
    vec_t ge = u->vcc_hi, le = u->vcc_lo, eq = u->vco_hi, sign = u->vco_lo, vce = u->vce;
    vec_t sn = vsubq_u16(veorq_u16(vt, sign), sign);
    vec_t diff = vsubq_u16(vs, sn);
    vec_t ncarry = vceqq_u16(diff, vqaddq_u16(vs, vt));
    vec_t nvce = vceqzq_u16(vce);
    vec_t dz = vceqzq_u16(diff);
    vec_t le_eq = vorrq_u16(vandq_u16(vandq_u16(dz, ncarry), nvce), vandq_u16(vce, vorrq_u16(dz, ncarry)));
    vec_t ge_eq = vcleq_u16(vt, vs);
    le = vbslq_u16(vbicq_u16(sign, eq), le_eq, le);
    ge = vbslq_u16(vorrq_u16(sign, eq), ge, ge_eq);
    u->vcc_hi = ge;
    u->vcc_lo = le;
    u->vco_hi = u->vco_lo = u->vce = vdupq_n_u16(0);
    return vbslq_u16(vbslq_u16(sign, le, ge), sn, vs);
}

VU_AI vec_t vu_k_cr(struct vu *u, vec_t vs, vec_t vt)
{
    vec_t sign = vn_sra15(veorq_u16(vs, vt));
    vec_t le = vn_sra15(vaddq_u16(vandq_u16(vs, sign), vt));
    vec_t ge = vcleq_s16(VU_S(vt), VU_S(vorrq_u16(vs, sign)));
    vec_t snt = veorq_u16(vt, sign);
    u->vcc_hi = ge;
    u->vcc_lo = le;
    u->vco_hi = u->vco_lo = u->vce = vdupq_n_u16(0);
    return vbslq_u16(vbslq_u16(sign, le, ge), snt, vs);
}

#endif /* VU_NEON */

/* ---------------------------------------------------------------------------------------------------------------------------------
 * The instructions: NAME(u, vd, vs, vt, E) with constant register numbers vd / vs / vt and element selector E (operand vt[E]).
 * ------------------------------------------------------------------------------------------------------------------------------- */
#define VU_ARGS struct vu *u, int vd, int vs, int vt, int E
#define VU_OPS vec_t a = u->r[vs], b = vu_elem(u->r[vt], E)

VU_AI void vu_vmulf(VU_ARGS) { VU_OPS; vec_t r; vu_k_mulf(u, a, b, 0, &r); u->r[vd] = r; }
VU_AI void vu_vmulu(VU_ARGS) { VU_OPS; vec_t r; vu_k_mulf(u, a, b, 1, &r); u->r[vd] = r; }
VU_AI void vu_vmudl(VU_ARGS) { VU_OPS; u->r[vd] = vu_k_madl(u, a, b, 0); }
VU_AI void vu_vmudm(VU_ARGS) { VU_OPS; u->r[vd] = vu_k_madm(u, a, b, 0); }
VU_AI void vu_vmudn(VU_ARGS) { VU_OPS; u->r[vd] = vu_k_madn(u, a, b, 0); }
VU_AI void vu_vmudh(VU_ARGS) { VU_OPS; u->r[vd] = vu_k_madh(u, a, b, 0); }
VU_AI void vu_vmacf(VU_ARGS) { VU_OPS; u->r[vd] = vu_k_macf(u, a, b, 0); }
VU_AI void vu_vmacu(VU_ARGS) { VU_OPS; u->r[vd] = vu_k_macf(u, a, b, 1); }
VU_AI void vu_vmadl(VU_ARGS) { VU_OPS; u->r[vd] = vu_k_madl(u, a, b, 1); }
VU_AI void vu_vmadm(VU_ARGS) { VU_OPS; u->r[vd] = vu_k_madm(u, a, b, 1); }
VU_AI void vu_vmadn(VU_ARGS) { VU_OPS; u->r[vd] = vu_k_madn(u, a, b, 1); }
VU_AI void vu_vmadh(VU_ARGS) { VU_OPS; u->r[vd] = vu_k_madh(u, a, b, 1); }
VU_AI void vu_vabs(VU_ARGS) { VU_OPS; vec_t lo; u->r[vd] = vu_k_abs(a, b, &lo); u->acc_lo = lo; }
VU_AI void vu_vadd(VU_ARGS)
{
    VU_OPS; vec_t lo;
    vec_t res = vu_k_add(a, b, u->vco_lo, &lo);
    u->vco_lo = u->vco_hi = vu_zero();
    u->acc_lo = lo;
    u->r[vd] = res;
}
VU_AI void vu_vsub(VU_ARGS)
{
    VU_OPS; vec_t lo;
    vec_t res = vu_k_sub(a, b, u->vco_lo, &lo);
    u->vco_lo = u->vco_hi = vu_zero();
    u->acc_lo = lo;
    u->r[vd] = res;
}
VU_AI void vu_vch(VU_ARGS) { VU_OPS; vec_t r = vu_k_ch(u, a, b); u->acc_lo = r; u->r[vd] = r; }
VU_AI void vu_vcl(VU_ARGS) { VU_OPS; vec_t r = vu_k_cl(u, a, b); u->acc_lo = r; u->r[vd] = r; }
VU_AI void vu_vcr(VU_ARGS) { VU_OPS; vec_t r = vu_k_cr(u, a, b); u->acc_lo = r; u->r[vd] = r; }
VU_AI void vu_reserved(VU_ARGS)
{
    VU_OPS;
#if VU_NEON
    u->acc_lo = vaddq_u16(a, b);
#else
    for (int i = 0; i < 8; i++) u->acc_lo.e[i] = (uint16_t)(a.e[i] + b.e[i]);
#endif
    u->r[vd] = vu_zero();
}

#if VU_NEON
#define VU_BIN(NAME, EXPR) VU_AI void NAME(VU_ARGS) { VU_OPS; vec_t r = (EXPR); u->acc_lo = r; u->r[vd] = r; }
VU_BIN(vu_vand, vandq_u16(a, b))
VU_BIN(vu_vnand, vmvnq_u16(vandq_u16(a, b)))
VU_BIN(vu_vor, vorrq_u16(a, b))
VU_BIN(vu_vnor, vmvnq_u16(vorrq_u16(a, b)))
VU_BIN(vu_vxor, veorq_u16(a, b))
VU_BIN(vu_vnxor, vmvnq_u16(veorq_u16(a, b)))

VU_AI void vu_vaddc(VU_ARGS)
{
    VU_OPS;
    vec_t s = vaddq_u16(a, b);
    u->vco_lo = vcltq_u16(s, a);
    u->vco_hi = vdupq_n_u16(0);
    u->acc_lo = s;
    u->r[vd] = s;
}
VU_AI void vu_vsubc(VU_ARGS)
{
    VU_OPS;
    vec_t s = vsubq_u16(a, b);
    u->vco_lo = vcltq_u16(a, b);
    u->vco_hi = vmvnq_u16(vceqq_u16(a, b));
    u->acc_lo = s;
    u->r[vd] = s;
}
/* compare family: le = the new VCC lo */
#define VU_CMP_TAIL \
    u->vcc_hi = vdupq_n_u16(0); u->vcc_lo = le; u->vco_hi = u->vco_lo = vdupq_n_u16(0); \
    vec_t r = vbslq_u16(le, a, b); u->acc_lo = r; u->r[vd] = r;
VU_AI void vu_veq(VU_ARGS) { VU_OPS; vec_t le = vbicq_u16(vceqq_u16(a, b), u->vco_hi); VU_CMP_TAIL }
VU_AI void vu_vne(VU_ARGS)
{
    VU_OPS; vec_t equal = vceqq_u16(a, b);
    vec_t le = vorrq_u16(vandq_u16(u->vco_hi, equal), vmvnq_u16(equal));
    VU_CMP_TAIL
}
VU_AI void vu_vge(VU_ARGS)
{
    VU_OPS; vec_t equal = vceqq_u16(a, b);
    vec_t le = vorrq_u16(vcgtq_s16(VU_S(a), VU_S(b)), vbicq_u16(equal, vandq_u16(u->vco_hi, u->vco_lo)));
    VU_CMP_TAIL
}
VU_AI void vu_vlt(VU_ARGS)
{
    VU_OPS; vec_t equal = vceqq_u16(a, b);
    vec_t le = vorrq_u16(vcltq_s16(VU_S(a), VU_S(b)), vandq_u16(vandq_u16(u->vco_hi, equal), u->vco_lo));
    VU_CMP_TAIL
}
VU_AI void vu_vmrg(VU_ARGS)
{
    VU_OPS;
    vec_t r = vbslq_u16(u->vcc_lo, a, b);
    u->vco_lo = u->vco_hi = vdupq_n_u16(0);
    u->acc_lo = r;
    u->r[vd] = r;
}
#else
#define VU_BIN(NAME, EXPR) \
    VU_AI void NAME(VU_ARGS) { VU_OPS; vec_t r; for (int i = 0; i < 8; i++) { uint16_t x = a.e[i], y = b.e[i]; r.e[i] = (uint16_t)(EXPR); } \
                               u->acc_lo = r; u->r[vd] = r; }
VU_BIN(vu_vand, x & y)
VU_BIN(vu_vnand, ~(x & y))
VU_BIN(vu_vor, x | y)
VU_BIN(vu_vnor, ~(x | y))
VU_BIN(vu_vxor, x ^ y)
VU_BIN(vu_vnxor, ~(x ^ y))

VU_AI void vu_vaddc(VU_ARGS)
{
    VU_OPS; vec_t r;
    for (int i = 0; i < 8; i++) {
        r.e[i] = (uint16_t)(a.e[i] + b.e[i]);
        u->vco_lo.e[i] = vs_carry(a.e[i], b.e[i]) ? 0xffff : 0;
        u->vco_hi.e[i] = 0;
    }
    u->acc_lo = r; u->r[vd] = r;
}
VU_AI void vu_vsubc(VU_ARGS)
{
    VU_OPS; vec_t r;
    for (int i = 0; i < 8; i++) {
        r.e[i] = (uint16_t)(a.e[i] - b.e[i]);
        u->vco_lo.e[i] = a.e[i] < b.e[i] ? 0xffff : 0;
        u->vco_hi.e[i] = a.e[i] != b.e[i] ? 0xffff : 0;
    }
    u->acc_lo = r; u->r[vd] = r;
}
/* compare family: VEQ 0, VNE 1, VGE 2, VLT 3 */
VU_AI void vu_cmp(struct vu *u, int vd, int vs, int vt, int E, int kind)
{
    VU_OPS; vec_t r, le;
    for (int i = 0; i < 8; i++) {
        uint16_t x = a.e[i], y = b.e[i], eq = u->vco_hi.e[i], sign = u->vco_lo.e[i];
        int equal = x == y, l;
        switch (kind) {
        case 0: l = !eq && equal; break;
        case 1: l = (eq && equal) || !equal; break;
        case 2: l = (int16_t)x > (int16_t)y || (equal && !(eq && sign)); break;
        default: l = (int16_t)x < (int16_t)y || (equal && eq && sign); break;
        }
        le.e[i] = l ? 0xffff : 0;
        r.e[i] = l ? x : y;
    }
    u->vcc_hi = vu_zero();
    u->vcc_lo = le;
    u->vco_hi = u->vco_lo = vu_zero();
    u->acc_lo = r;
    u->r[vd] = r;
}
VU_AI void vu_veq(VU_ARGS) { vu_cmp(u, vd, vs, vt, E, 0); }
VU_AI void vu_vne(VU_ARGS) { vu_cmp(u, vd, vs, vt, E, 1); }
VU_AI void vu_vge(VU_ARGS) { vu_cmp(u, vd, vs, vt, E, 2); }
VU_AI void vu_vlt(VU_ARGS) { vu_cmp(u, vd, vs, vt, E, 3); }
VU_AI void vu_vmrg(VU_ARGS)
{
    VU_OPS; vec_t r;
    for (int i = 0; i < 8; i++) r.e[i] = u->vcc_lo.e[i] ? a.e[i] : b.e[i];
    u->vco_lo = u->vco_hi = vu_zero();
    u->acc_lo = r;
    u->r[vd] = r;
}
#endif

/* VSAR: E 8 = HI, 9 = MD, 10 = LO, else zero (the accumulator is not changed) */
VU_AI void vu_vsar(VU_ARGS)
{
    (void)vs; (void)vt;
    u->r[vd] = E == 8 ? u->acc_hi : E == 9 ? u->acc_md : E == 10 ? u->acc_lo : vu_zero();
}

/* ---- reciprocal unit: (u, vd, de, vt, E); the element of vt is lane E & 7 (not the broadcast one), the result lands in vd[de] ---- */
extern const uint16_t rsp_reciprocal_rom[1024];

VU_AI int32_t vu_recip(int32_t input, int rsq)
{
    int32_t input_mask = input >> 31, data = input ^ input_mask, result;
    if (input > -32768) data -= input_mask;
    if (data == 0) return 0x7fffffff;
    if (input == -32768) return (int32_t)0xffff0000u;
    unsigned shift = (unsigned)__builtin_clz((unsigned)data);
    unsigned idx = (unsigned)((((unsigned long long)(unsigned)data << shift) & 0x7FC00000ull) >> 22);
    if (rsq) {
        idx = ((idx | 0x200) & 0x3FE) | (shift % 2);
        result = rsp_reciprocal_rom[idx];
        result = ((0x10000 | result) << 14) >> ((31 - shift) >> 1);
    } else {
        idx = VU_MUT(3, idx, idx ^ 1);
        result = rsp_reciprocal_rom[idx];
        result = ((0x10000 | result) << 14) >> (31 - shift);
    }
    return result ^ input_mask;
}

/* VRCP / VRCPL / VRSQ / VRSQL: dpin = use div_in as the high half (the L variants when dp_flag & 1) */
VU_AI void vu_div(struct vu *u, int vd, int de, int vt, int E, int rsq, int lvariant)
{
    u->acc_lo = vu_elem(u->r[vt], E);
    int dp = lvariant ? (u->dp_flag & 1) : 0;
    u->dp_flag = 0;
    int16_t v = (int16_t)vu_get(u->r[vt], E & 7);
    uint32_t in = dp ? (((uint32_t)(uint16_t)u->div_in << 16) | (uint16_t)v) : (uint32_t)(int32_t)v;
    int32_t result = vu_recip((int32_t)in, rsq);
    u->div_out = (int16_t)(result >> 16);
    u->r[vd] = vu_set(u->r[vd], de, (uint16_t)result);
}
VU_AI void vu_divh(struct vu *u, int vd, int de, int vt, int E)
{
    u->acc_lo = vu_elem(u->r[vt], E);
    u->dp_flag = 1;
    u->div_in = (int16_t)vu_get(u->r[vt], E & 7);
    u->r[vd] = vu_set(u->r[vd], de, (uint16_t)u->div_out);
}
/* VMOV vd[de] = vt[E] lane de (acc_lo = vt[E]) */
VU_AI void vu_vmov(struct vu *u, int vd, int de, int vt, int E)
{
    vec_t b = vu_elem(u->r[vt], E);
    u->acc_lo = b;
    u->r[vd] = vu_set(u->r[vd], de, vu_get(b, de));
}

#define VMULF(u, vd, vs, vt, E) vu_vmulf((u), (vd), (vs), (vt), (E))
#define VMULU(u, vd, vs, vt, E) vu_vmulu((u), (vd), (vs), (vt), (E))
#define VMUDL(u, vd, vs, vt, E) vu_vmudl((u), (vd), (vs), (vt), (E))
#define VMUDM(u, vd, vs, vt, E) vu_vmudm((u), (vd), (vs), (vt), (E))
#define VMUDN(u, vd, vs, vt, E) vu_vmudn((u), (vd), (vs), (vt), (E))
#define VMUDH(u, vd, vs, vt, E) vu_vmudh((u), (vd), (vs), (vt), (E))
#define VMACF(u, vd, vs, vt, E) vu_vmacf((u), (vd), (vs), (vt), (E))
#define VMACU(u, vd, vs, vt, E) vu_vmacu((u), (vd), (vs), (vt), (E))
#define VMADL(u, vd, vs, vt, E) vu_vmadl((u), (vd), (vs), (vt), (E))
#define VMADM(u, vd, vs, vt, E) vu_vmadm((u), (vd), (vs), (vt), (E))
#define VMADN(u, vd, vs, vt, E) vu_vmadn((u), (vd), (vs), (vt), (E))
#define VMADH(u, vd, vs, vt, E) vu_vmadh((u), (vd), (vs), (vt), (E))
#define VABS(u, vd, vs, vt, E) vu_vabs((u), (vd), (vs), (vt), (E))
#define VADD(u, vd, vs, vt, E) vu_vadd((u), (vd), (vs), (vt), (E))
#define VADDC(u, vd, vs, vt, E) vu_vaddc((u), (vd), (vs), (vt), (E))
#define VSUB(u, vd, vs, vt, E) vu_vsub((u), (vd), (vs), (vt), (E))
#define VSUBC(u, vd, vs, vt, E) vu_vsubc((u), (vd), (vs), (vt), (E))
#define VAND(u, vd, vs, vt, E) vu_vand((u), (vd), (vs), (vt), (E))
#define VNAND(u, vd, vs, vt, E) vu_vnand((u), (vd), (vs), (vt), (E))
#define VOR(u, vd, vs, vt, E) vu_vor((u), (vd), (vs), (vt), (E))
#define VNOR(u, vd, vs, vt, E) vu_vnor((u), (vd), (vs), (vt), (E))
#define VXOR(u, vd, vs, vt, E) vu_vxor((u), (vd), (vs), (vt), (E))
#define VNXOR(u, vd, vs, vt, E) vu_vnxor((u), (vd), (vs), (vt), (E))
#define VEQ(u, vd, vs, vt, E) vu_veq((u), (vd), (vs), (vt), (E))
#define VNE(u, vd, vs, vt, E) vu_vne((u), (vd), (vs), (vt), (E))
#define VGE(u, vd, vs, vt, E) vu_vge((u), (vd), (vs), (vt), (E))
#define VLT(u, vd, vs, vt, E) vu_vlt((u), (vd), (vs), (vt), (E))
#define VCH(u, vd, vs, vt, E) vu_vch((u), (vd), (vs), (vt), (E))
#define VCL(u, vd, vs, vt, E) vu_vcl((u), (vd), (vs), (vt), (E))
#define VCR(u, vd, vs, vt, E) vu_vcr((u), (vd), (vs), (vt), (E))
#define VMRG(u, vd, vs, vt, E) vu_vmrg((u), (vd), (vs), (vt), (E))
#define VSAR(u, vd, vs, vt, E) vu_vsar((u), (vd), (vs), (vt), (E))
#define VRESERVED(u, vd, vs, vt, E) vu_reserved((u), (vd), (vs), (vt), (E))   /* what the fork does for the unassigned VU opcodes */
#define VNOP(u) ((void)0)
/* (u, vd, de, vt, E): de = destination element 0..7 (the vs field & 7 of the instruction) */
#define VMOV(u, vd, de, vt, E) vu_vmov((u), (vd), (de) & 7, (vt), (E))
#define VRCP(u, vd, de, vt, E) vu_div((u), (vd), (de) & 7, (vt), (E), 0, 0)
#define VRCPL(u, vd, de, vt, E) vu_div((u), (vd), (de) & 7, (vt), (E), 0, 1)
#define VRSQ(u, vd, de, vt, E) vu_div((u), (vd), (de) & 7, (vt), (E), 1, 0)
#define VRSQL(u, vd, de, vt, E) vu_div((u), (vd), (de) & 7, (vt), (E), 1, 1)
#define VRCPH(u, vd, de, vt, E) vu_divh((u), (vd), (de) & 7, (vt), (E))
#define VRSQH(u, vd, de, vt, E) vu_divh((u), (vd), (de) & 7, (vt), (E))

/* ---------------------------------------------------------------------------------------------------------------------------------
 * MFC2 / MTC2 / CFC2 / CTC2
 * ------------------------------------------------------------------------------------------------------------------------------- */
VU_AI int32_t vu_mfc2(const struct vu *u, int vs, int E)
{
    unsigned lo = (unsigned)E >> 1;
    if (E & 1) {
        unsigned hi = (((unsigned)E + 1) >> 1) & 7;
        uint16_t high = (uint16_t)(vu_get(u->r[vs], (int)lo) << 8);
        uint8_t low = (uint8_t)(vu_get(u->r[vs], (int)hi) >> 8);
        return (int16_t)(high | low);
    }
    return (int16_t)vu_get(u->r[vs], (int)lo);
}

VU_AI void vu_mtc2(struct vu *u, uint32_t gpr, int vd, int E)
{
    uint16_t v = (uint16_t)gpr;
    int i = E >> 1;
    if (E & 1) {
        u->r[vd] = vu_set(u->r[vd], i, (uint16_t)((vu_get(u->r[vd], i) & VU_MUT(7, 0xff00, 0xffff)) | (v >> 8)));
        if (E != 0xf) u->r[vd] = vu_set(u->r[vd], i + 1, (uint16_t)((vu_get(u->r[vd], i + 1) & 0xff) | (uint16_t)(v << 8)));
    } else {
        u->r[vd] = vu_set(u->r[vd], i, v);
    }
}

/* the flag as the 16-bit register: bits 0..7 from the lo lanes, bits 8..15 from the hi lanes */
VU_AI unsigned vu_lanes_to_bits(vec_t m)
{
#if VU_NEON
    static const uint16_t w[8] = { 1, 2, 4, 8, 16, 32, 64, 128 };
    return vaddvq_u16(vandq_u16(m, vld1q_u16(w)));
#else
    unsigned b = 0;
    for (int i = 0; i < 8; i++) b |= (m.e[i] ? 1u : 0u) << i;
    return b;
#endif
}

VU_AI vec_t vu_bits_to_lanes(unsigned bits)
{
#if VU_NEON
    static const uint16_t w[8] = { 1, 2, 4, 8, 16, 32, 64, 128 };
    return vtstq_u16(vdupq_n_u16((uint16_t)bits), vld1q_u16(w));
#else
    vec_t m;
    for (int i = 0; i < 8; i++) m.e[i] = (bits >> i) & 1 ? 0xffff : 0;
    return m;
#endif
}

/* which: 0 VCO, 1 VCC, 2 VCE (3 = VCE as in the fork); result sign extended from 16 bits */
VU_AI int32_t vu_cfc2(const struct vu *u, int which)
{
    unsigned w = (unsigned)which & 3;
    unsigned bits;
    if (w == 0) bits = vu_lanes_to_bits(u->vco_lo) | (vu_lanes_to_bits(u->vco_hi) << 8);
    else if (w == 1) bits = vu_lanes_to_bits(u->vcc_lo) | (vu_lanes_to_bits(u->vcc_hi) << 8);
    else bits = vu_lanes_to_bits(u->vce);
    return (int16_t)bits;
}

VU_AI void vu_ctc2(struct vu *u, uint32_t gpr, int which)
{
    unsigned v = gpr & 0xffff, w = (unsigned)which & 3;
    if (w == 0) { u->vco_lo = vu_bits_to_lanes(v & 0xff); u->vco_hi = vu_bits_to_lanes(v >> 8); }
    else if (w == 1) { u->vcc_lo = vu_bits_to_lanes(v & 0xff); u->vcc_hi = vu_bits_to_lanes(v >> 8); }
    else u->vce = vu_bits_to_lanes(v & 0xff);
}

/* MFC2(u, &gpr, vs, E): *gpr = sign extended 16-bit; CFC2(u, &gpr, which). The destination may be a uint32_t or int32_t. */
#define MFC2(u, gprp, vs, E) (*(gprp) = (__typeof__(*(gprp)))vu_mfc2((u), (vs), (E)))
#define MTC2(u, gpr, vd, E) vu_mtc2((u), (uint32_t)(gpr), (vd), (E))
#define CFC2(u, gprp, which) (*(gprp) = (__typeof__(*(gprp)))vu_cfc2((u), (which)))
#define CTC2(u, gpr, which) vu_ctc2((u), (uint32_t)(gpr), (which))

/* ---------------------------------------------------------------------------------------------------------------------------------
 * DMEM access and the vector loads / stores. The address is the final byte address (any 32-bit value; it is masked to DMEM).
 * ------------------------------------------------------------------------------------------------------------------------------- */
typedef uint16_t __attribute__((may_alias)) vu_u16a;
typedef uint32_t __attribute__((may_alias)) vu_u32a;

VU_AI uint8_t vu_m8(const uint32_t *dm, unsigned a) { return ((const uint8_t *)dm)[(a ^ 3) & 0xfff]; }
VU_AI uint16_t vu_m16(const uint32_t *dm, unsigned a) { return ((const vu_u16a *)dm)[((a ^ 2) & 0xfff) >> 1]; }
VU_AI void vu_w8(uint32_t *dm, unsigned a, uint8_t v) { ((uint8_t *)dm)[(a ^ 3) & 0xfff] = v; }
VU_AI void vu_w16(uint32_t *dm, unsigned a, uint16_t v) { ((vu_u16a *)dm)[((a ^ 2) & 0xfff) >> 1] = v; }

VU_AI uint8_t vu_bget(const uint16_t *arr, unsigned i) { return (i & 1) ? (uint8_t)(arr[i >> 1] & 0xff) : (uint8_t)(arr[i >> 1] >> 8); }
VU_AI void vu_bput(uint16_t *arr, unsigned i, uint8_t b)
{
    const unsigned n = i >> 1;
    if (i & 1) arr[n] = (uint16_t)((arr[n] & 0xff00) | b);
    else arr[n] = (uint16_t)((arr[n] & 0xff) | ((uint16_t)b << 8));
}

/* Generic versions on a register held as uint16_t[8] (element order): the fork's ls.cpp. Stores wrap at the end of DMEM (the fork's SLV
 * and STV byte paths write past it instead, a bug that cannot matter on the hardware). */
VU_AI void vu_g_lbv(uint16_t *r, const uint32_t *dm, unsigned e, unsigned a) { vu_bput(r, e, vu_m8(dm, a)); }
VU_AI void vu_g_sbv(const uint16_t *r, uint32_t *dm, unsigned e, unsigned a) { vu_w8(dm, a, vu_bget(r, e)); }

VU_AI void vu_g_lsv(uint16_t *r, const uint32_t *dm, unsigned e, unsigned a)
{
    const unsigned end = (e > 14) ? 16 : (e + 2);
    if (((a | e) & 1) == 0) { r[e >> 1] = vu_m16(dm, a); return; }
    for (unsigned i = e; i < end; i++) vu_bput(r, i & 0xf, vu_m8(dm, a++));
}
VU_AI void vu_g_ssv(const uint16_t *r, uint32_t *dm, unsigned e, unsigned a)
{
    if (((a | e) & 1) == 0) { vu_w16(dm, a, r[e >> 1]); return; }
    uint8_t v0 = vu_bget(r, e), v1 = vu_bget(r, (e + 1) & 0xf);
    vu_w8(dm, a, v0);
    vu_w8(dm, a + 1, v1);
}
VU_AI void vu_g_llv(uint16_t *r, const uint32_t *dm, unsigned e, unsigned a)
{
    const unsigned end = (e > 12) ? 16 : (e + 4);
    if (((a | e) & 1) == 0) {
        for (unsigned i = e; i < end; i += 2) r[i >> 1] = vu_m16(dm, a + i - e);
        return;
    }
    for (unsigned i = e; i < end; i++) vu_bput(r, i & 0xf, vu_m8(dm, a++));
}
VU_AI void vu_g_slv(const uint16_t *r, uint32_t *dm, unsigned e, unsigned a)
{
    if (((a | e) & 1) == 0) {
        vu_w16(dm, a, r[e >> 1]);
        vu_w16(dm, a + 2, r[((e >> 1) + 1) & 7]);
        return;
    }
    for (unsigned i = e; i < e + 4; i++) vu_w8(dm, a++, vu_bget(r, i & 0xf));
}
VU_AI void vu_g_ldv(uint16_t *r, const uint32_t *dm, unsigned e, unsigned a)
{
    const unsigned end = (e > 8) ? 16 : (e + 8);
    if (((a | e) & 1) == 0) {
        for (unsigned i = e; i < end; i += 2) r[i >> 1] = vu_m16(dm, a + i - e);
        return;
    }
    for (unsigned i = e; i < end; i++) vu_bput(r, i & 0xf, vu_m8(dm, a++));
}
VU_AI void vu_g_sdv(const uint16_t *r, uint32_t *dm, unsigned e, unsigned a)
{
    if ((e > 8) || (e & 1) || (a & 1)) {
        for (unsigned i = 0; i < 8; i++) vu_w8(dm, a + i, vu_bget(r, (e + i) & 0xf));
    } else {
        e >>= 1;
        for (unsigned i = 0; i < 4; i++) vu_w16(dm, a + 2 * i, r[e + i]);
    }
}
VU_AI void vu_g_lqv(uint16_t *r, const uint32_t *dm, unsigned e, unsigned a)
{
    unsigned end = 16 + e - (a & 0xf);
    if (end > 16) end = 16;
    if (((a | e) & 1) == 0) {
        for (unsigned i = e; i < end; i += 2) r[i >> 1] = vu_m16(dm, a + i - e);
        return;
    }
    for (unsigned i = e; i < end; i++) vu_bput(r, i & 0xf, vu_m8(dm, a++));
}
VU_AI void vu_g_sqv(const uint16_t *r, uint32_t *dm, unsigned e, unsigned a)
{
    const unsigned end = e + (16 - (a & 15));
    if (((a | e) & 1) == 0) {
        for (unsigned i = e; i < end; i += 2) vu_w16(dm, a + i - e, r[(i >> 1) & 7]);
        return;
    }
    for (unsigned i = e; i < end; i++) vu_w8(dm, a++, vu_bget(r, i & 15));
}
/* LPV / LUV: bytes of the 16-byte window at (a & ~7), rotated by (a & 7) - e, into the high bits (shift 8) / bit 14..7 (shift 7) */
VU_AI void vu_g_lpv(uint16_t *r, const uint32_t *dm, unsigned e, unsigned a, unsigned shift)
{
    const unsigned index = (a & 7) - e;
    a &= ~7u;
    for (unsigned i = 0; i < 8; i++) r[i] = (uint16_t)(vu_m8(dm, a + ((i + index) & 0xf)) << shift);
}
VU_AI void vu_g_spv(const uint16_t *r, uint32_t *dm, unsigned e, unsigned a, int uv)
{
    for (unsigned i = e; i < e + 8; i++) {
        unsigned shift = ((i & 0xf) < 8) ? 8 : 7;
        if (uv) shift = ((i & 0xf) < 8) ? 7 : 8;
        shift = VU_MUT(6, shift, 15 - shift);
        vu_w8(dm, a++, (uint8_t)((int16_t)r[i & 7] >> shift));
    }
}
VU_AI void vu_g_ltv(uint16_t (*regs)[8], const uint32_t *dm, unsigned e, unsigned a)
{
    const unsigned start = a & ~7u;
    a = start + ((e + (a & 8)) & 0xf);
    unsigned j = e >> 1;
    VU_NOUNROLL
    for (unsigned i = 0; i < 16; j++) {
        j &= 7;
        vu_bput(regs[j], i++, vu_m8(dm, a++));
        if (a == start + 16) a = start;
        vu_bput(regs[j], i++, vu_m8(dm, a++));
        if (a == start + 16) a = start;
    }
}
VU_AI void vu_g_stv(uint16_t (*regs)[8], uint32_t *dm, unsigned e, unsigned a)
{
    e &= ~1u;
    unsigned element = 16 - e;
    unsigned base = (a & 7) - e;
    a &= ~7u;
    VU_NOUNROLL
    for (unsigned i = 0; i < 8; i++) {
        vu_w8(dm, a + (base++ & 0xf), vu_bget(regs[i], element++ & 0xf));
        vu_w8(dm, a + (base++ & 0xf), vu_bget(regs[i], element++ & 0xf));
    }
}

/* The generic (unaligned, odd element) paths are out of line and cold, so the hot path stays small and free of stack traffic. They take
 * and return the register by value. */
#define VU_COLD_LD(NAME, FN) \
    static __attribute__((noinline, cold)) vec_t NAME(vec_t v, uint32_t *dm, unsigned e, unsigned a) \
    { uint16_t t[8]; vu_st(t, v); FN(t, dm, e, a); return vu_ld(t); }
#define VU_COLD_ST(NAME, FN) \
    static __attribute__((noinline, cold)) void NAME(vec_t v, uint32_t *dm, unsigned e, unsigned a) \
    { uint16_t t[8]; vu_st(t, v); FN(t, dm, e, a); }
VU_COLD_LD(vu_c_lsv, vu_g_lsv) VU_COLD_LD(vu_c_llv, vu_g_llv) VU_COLD_LD(vu_c_ldv, vu_g_ldv) VU_COLD_LD(vu_c_lqv, vu_g_lqv)
VU_COLD_ST(vu_c_ssv, vu_g_ssv) VU_COLD_ST(vu_c_slv, vu_g_slv) VU_COLD_ST(vu_c_sdv, vu_g_sdv) VU_COLD_ST(vu_c_sqv, vu_g_sqv)
#define VU_GLD(u, rt, fn, ...) do { uint16_t t_[8]; vu_st(t_, (u)->r[rt]); fn(t_, (u)->dmem, __VA_ARGS__); (u)->r[rt] = vu_ld(t_); } while (0)
#define VU_GST(u, rt, fn, ...) do { uint16_t t_[8]; vu_st(t_, (u)->r[rt]); fn(t_, (u)->dmem, __VA_ARGS__); } while (0)

#if VU_NEON
/* 4 / 8 / 16 bytes at an aligned DMEM address as lanes in RSP order: the host holds each big-endian word byte-reversed, so a
 * 32-bit-wise halfword swap fixes the order */
VU_AI uint16x8_t vu_ld16a(const struct vu *u, unsigned a) { return vrev32q_u16(vld1q_u16((const uint16_t *)((const uint8_t *)u->dmem + (a & 0xff0)))); }
VU_AI uint16x4_t vu_ld8a(const struct vu *u, unsigned a) { return vrev32_u16(vld1_u16((const uint16_t *)((const uint8_t *)u->dmem + (a & 0xff8)))); }
VU_AI void vu_st16a(struct vu *u, unsigned a, uint16x8_t v) { vst1q_u16((uint16_t *)((uint8_t *)u->dmem + (a & 0xff0)), vrev32q_u16(v)); }
VU_AI void vu_st8a(struct vu *u, unsigned a, uint16x4_t v) { vst1_u16((uint16_t *)((uint8_t *)u->dmem + (a & 0xff8)), vrev32_u16(v)); }
/* 8 bytes at an 8-aligned address in memory order */
VU_AI uint8x8_t vu_ldb8a(const struct vu *u, unsigned a) { return vrev32_u8(vld1_u8((const uint8_t *)u->dmem + (a & 0xff8))); }
VU_AI void vu_stb8a(struct vu *u, unsigned a, uint8x8_t v) { vst1_u8((uint8_t *)u->dmem + (a & 0xff8), vrev32_u8(v)); }
#endif

VU_AI void vu_lbv(struct vu *u, int rt, int E, unsigned a)
{
    uint16_t o = vu_get(u->r[rt], E >> 1);
    uint8_t b = vu_m8(u->dmem, a);
    u->r[rt] = vu_set(u->r[rt], E >> 1, (E & 1) ? (uint16_t)((o & 0xff00) | b) : (uint16_t)((o & 0xff) | ((uint16_t)b << 8)));
}
VU_AI void vu_sbv(struct vu *u, int rt, int E, unsigned a)
{
    uint16_t o = vu_get(u->r[rt], E >> 1);
    vu_w8(u->dmem, a, (E & 1) ? (uint8_t)o : (uint8_t)(o >> 8));
}
VU_AI void vu_lsv(struct vu *u, int rt, int E, unsigned a)
{
    if (((a | (unsigned)E) & 1) == 0) { u->r[rt] = vu_set(u->r[rt], E >> 1, vu_m16(u->dmem, a)); return; }
    u->r[rt] = vu_c_lsv(u->r[rt], u->dmem, (unsigned)E, a);
}
VU_AI void vu_ssv(struct vu *u, int rt, int E, unsigned a)
{
    if (((a | (unsigned)E) & 1) == 0) { vu_w16(u->dmem, a, vu_get(u->r[rt], E >> 1)); return; }
    vu_c_ssv(u->r[rt], u->dmem, (unsigned)E, a);
}
VU_AI void vu_llv(struct vu *u, int rt, int E, unsigned a)
{
    if (((a | (unsigned)E) & 1) == 0) {
        vec_t v = u->r[rt];
        v = vu_set(v, E >> 1, vu_m16(u->dmem, a));
        if (E <= 12) v = vu_set(v, (E >> 1) + 1, vu_m16(u->dmem, a + 2));
        u->r[rt] = v;
        return;
    }
    u->r[rt] = vu_c_llv(u->r[rt], u->dmem, (unsigned)E, a);
}
VU_AI void vu_slv(struct vu *u, int rt, int E, unsigned a)
{
    if (((a | (unsigned)E) & 1) == 0) {
        vu_w16(u->dmem, a, vu_get(u->r[rt], E >> 1));
        vu_w16(u->dmem, a + 2, vu_get(u->r[rt], (E >> 1) + 1));
        return;
    }
    vu_c_slv(u->r[rt], u->dmem, (unsigned)E, a);
}
VU_AI void vu_ldv(struct vu *u, int rt, int E, unsigned a)
{
#if VU_NEON
    if (!(a & 7) && (E == 0 || E == 8)) {
        uint16x4_t n = vu_ld8a(u, a);
        u->r[rt] = E == 0 ? vcombine_u16(n, vget_high_u16(u->r[rt])) : vcombine_u16(vget_low_u16(u->r[rt]), n);
        return;
    }
#endif
    if (((a | (unsigned)E) & 1) == 0) {
        vec_t v = u->r[rt];
        for (int k = 0; k < 4 && (E >> 1) + k < 8; k++) v = vu_set(v, (E >> 1) + k, vu_m16(u->dmem, a + 2 * k));
        u->r[rt] = v;
        return;
    }
    u->r[rt] = vu_c_ldv(u->r[rt], u->dmem, (unsigned)E, a);
}
VU_AI void vu_sdv(struct vu *u, int rt, int E, unsigned a)
{
#if VU_NEON
    if (!(a & 7) && (E == 0 || E == 8)) {
        vu_st8a(u, a, E == 0 ? vget_low_u16(u->r[rt]) : vget_high_u16(u->r[rt]));
        return;
    }
#endif
    if (!(E > 8) && !(E & 1) && !(a & 1)) {
        for (int k = 0; k < 4; k++) vu_w16(u->dmem, a + 2 * k, vu_get(u->r[rt], (E >> 1) + k));
        return;
    }
    vu_c_sdv(u->r[rt], u->dmem, (unsigned)E, a);
}
VU_AI void vu_lqv(struct vu *u, int rt, int E, unsigned a)
{
#if VU_NEON
    if (E == 0 && !(a & 15)) { u->r[rt] = vu_ld16a(u, a); return; }
#else
    if (E == 0 && !(a & 15)) {
        vec_t v;
        for (int k = 0; k < 8; k++) v.e[k] = vu_m16(u->dmem, a + 2 * k);
        u->r[rt] = v;
        return;
    }
#endif
    u->r[rt] = vu_c_lqv(u->r[rt], u->dmem, (unsigned)E, a);
}
VU_AI void vu_sqv(struct vu *u, int rt, int E, unsigned a)
{
    a &= 0xfff;
#if VU_NEON
    if (E == 0 && !(a & 15)) { vu_st16a(u, a, u->r[rt]); return; }
#else
    if (E == 0 && !(a & 15)) {
        for (int k = 0; k < 8; k++) vu_w16(u->dmem, a + 2 * k, u->r[rt].e[k]);
        return;
    }
#endif
    vu_c_sqv(u->r[rt], u->dmem, (unsigned)E, a);
}

/* LPV / LUV / SPV / SUV */
#if VU_NEON
VU_AI vec_t vu_lpv_(struct vu *u, int E, unsigned a, int shift7)
{
    unsigned a8 = a & 0xff8;
    uint8x16_t win = vcombine_u8(vu_ldb8a(u, a8), vu_ldb8a(u, (a8 + 8) & 0xfff));
    static const uint8_t iota[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    uint8x8_t idx = vand_u8(vadd_u8(vld1_u8(iota), vdup_n_u8((uint8_t)((a & 7) - (unsigned)E))), vdup_n_u8(15));
    uint8x8_t bytes = vqtbl1_u8(win, idx);
    return shift7 ? vshll_n_u8(bytes, 7) : vshll_n_u8(bytes, 8);
}
VU_AI uint16x8_t vu_rot_(uint16x8_t v, int k)
{
    switch (k & 7) {
    case 0: return v;
    case 1: return vextq_u16(v, v, 1);
    case 2: return vextq_u16(v, v, 2);
    case 3: return vextq_u16(v, v, 3);
    case 4: return vextq_u16(v, v, 4);
    case 5: return vextq_u16(v, v, 5);
    case 6: return vextq_u16(v, v, 6);
    default: return vextq_u16(v, v, 7);
    }
}
VU_AI void vu_spv_(struct vu *u, int rt, int E, unsigned a, int uv)
{
    int16_t sh[8];
    for (int j = 0; j < 8; j++) {
        unsigned i = (unsigned)(E + j);
        int s = ((i & 0xf) < 8) ? 8 : 7;
        if (uv) s = ((i & 0xf) < 8) ? 7 : 8;
        sh[j] = (int16_t)-VU_MUT(6, s, 15 - s);
    }
    int16x8_t v = vshlq_s16(VU_S(vu_rot_(u->r[rt], E & 7)), vld1q_s16(sh));
    uint8x8_t bytes = vmovn_u16(VU_U(v));
    if (!(a & 7)) { vu_stb8a(u, a, bytes); return; }
    uint8_t t[8];
    vst1_u8(t, bytes);
    for (int j = 0; j < 8; j++) vu_w8(u->dmem, a + (unsigned)j, t[j]);
}
#endif

VU_AI void vu_lpv(struct vu *u, int rt, int E, unsigned a)
{
    a &= 0xfff;
#if VU_NEON
    u->r[rt] = vu_lpv_(u, E, a, VU_MUT(5, 0, 1));
#else
    VU_GLD(u, rt, vu_g_lpv, E, a, VU_MUT(5, 8, 7));
#endif
}
VU_AI void vu_luv(struct vu *u, int rt, int E, unsigned a)
{
    a &= 0xfff;
#if VU_NEON
    u->r[rt] = vu_lpv_(u, E, a, 1);
#else
    VU_GLD(u, rt, vu_g_lpv, E, a, 7);
#endif
}
VU_AI void vu_spv(struct vu *u, int rt, int E, unsigned a)
{
    a &= 0xfff;
#if VU_NEON
    vu_spv_(u, rt, E, a, 0);
#else
    VU_GST(u, rt, vu_g_spv, E, a, 0);
#endif
}
VU_AI void vu_suv(struct vu *u, int rt, int E, unsigned a)
{
    a &= 0xfff;
#if VU_NEON
    vu_spv_(u, rt, E, a, 1);
#else
    VU_GST(u, rt, vu_g_spv, E, a, 1);
#endif
}

/* LTV / STV work on the 8 registers rt & ~7 .. | 7 */
VU_AI void vu_ltv(struct vu *u, int rt, int E, unsigned a)
{
    uint16_t t[8][8];
    int b = rt & ~7;
    for (int i = 0; i < 8; i++) vu_st(t[i], u->r[b + i]);
    vu_g_ltv(t, u->dmem, (unsigned)E, a & 0xfff);
    for (int i = 0; i < 8; i++) u->r[b + i] = vu_ld(t[i]);
}
VU_AI void vu_stv(struct vu *u, int rt, int E, unsigned a)
{
    uint16_t t[8][8];
    int b = rt & ~7;
    for (int i = 0; i < 8; i++) vu_st(t[i], u->r[b + i]);
    vu_g_stv(t, u->dmem, (unsigned)E, a & 0xfff);
}

/* LxV(u, rt, E, addr) / SxV(u, rt, E, addr): rt, E constants; addr the effective byte address */
#define LBV(u, rt, E, addr) vu_lbv((u), (rt), (E), (unsigned)(addr))
#define LSV(u, rt, E, addr) vu_lsv((u), (rt), (E), (unsigned)(addr) & 0xfff)
#define LLV(u, rt, E, addr) vu_llv((u), (rt), (E), (unsigned)(addr) & 0xfff)
#define LDV(u, rt, E, addr) vu_ldv((u), (rt), (E), (unsigned)(addr) & 0xfff)
#define LQV(u, rt, E, addr) vu_lqv((u), (rt), (E), (unsigned)(addr) & 0xfff)
#define LPV(u, rt, E, addr) vu_lpv((u), (rt), (E), (unsigned)(addr))
#define LUV(u, rt, E, addr) vu_luv((u), (rt), (E), (unsigned)(addr))
#define LTV(u, rt, E, addr) vu_ltv((u), (rt), (E), (unsigned)(addr))
#define SBV(u, rt, E, addr) vu_sbv((u), (rt), (E), (unsigned)(addr))
#define SSV(u, rt, E, addr) vu_ssv((u), (rt), (E), (unsigned)(addr) & 0xfff)
#define SLV(u, rt, E, addr) vu_slv((u), (rt), (E), (unsigned)(addr) & 0xfff)
#define SDV(u, rt, E, addr) vu_sdv((u), (rt), (E), (unsigned)(addr) & 0xfff)
#define SQV(u, rt, E, addr) vu_sqv((u), (rt), (E), (unsigned)(addr))
#define SPV(u, rt, E, addr) vu_spv((u), (rt), (E), (unsigned)(addr))
#define SUV(u, rt, E, addr) vu_suv((u), (rt), (E), (unsigned)(addr))
#define STV(u, rt, E, addr) vu_stv((u), (rt), (E), (unsigned)(addr))

#endif /* ARMLET_F3DEX_VU_H */
