/*
 * prim_test.c: checks src/hle/hle_fx.h (the closed-form fixed-point primitives of the task-level native path) against the exact VU instruction sequences
 * they stand for (ref_vu.h, the exact VU library; itself fuzzed against the fork's RSP_* helpers). Written by Fleet B as tools/native-f3dex/prim_test.c
 * against its own definitions (docs/ucode-f3dex.md, section 1); here it is the differential test of the shared library, run for every family.
 *
 * Local test only. Build and run: tools/hle/build_prim_test.sh
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ref_vu.h"
#include "hle_fx.h"

extern const uint16_t rsp_reciprocal_rom[1024];

/* ---- the primitives, as plain integer arithmetic ---------------------------------------------------------------------------------- */

typedef hle_acc acc_t;
#define sclamp hle_sclamp
#define uclamp hle_uclamp
static inline int64_t fx(int16_t i, uint16_t f) { return (int64_t)i * 65536 + f; }
static inline acc_t mac_fxfx(int16_t ai, uint16_t af, int16_t bi, uint16_t bf) { return hle_mac_32x32(hle_fx(ai, af), hle_fx(bi, bf)); }
static inline acc_t mac_fxc(int16_t ai, uint16_t af, uint16_t c) { return hle_mac_32xu16(hle_fx(ai, af), c); }
static inline int64_t mul_s16u16(int16_t x, uint16_t c) { return hle_mul_s16u16(x, c); }
static inline acc_t dot2(int16_t ai, uint16_t af, int16_t x, int16_t bi, uint16_t bf, int16_t y) { return hle_dot32x16(hle_fx(ai, af), x, hle_fx(bi, bf), y); }
static uint32_t rcp32(int32_t input) { return hle_rcp32(input); }
static void recip_refine(int32_t w, int16_t *ri, uint16_t *rf) { struct hle_pair p = hle_recip_refine(w); *ri = p.i; *rf = p.f; }

/* ---- tests ------------------------------------------------------------------------------------------------------------------------- */

static uint32_t rs = 12345;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static uint16_t r16(void)
{
    static const uint16_t e[] = { 0, 1, 2, 0x7fff, 0x8000, 0x8001, 0xffff, 0xfffe, 0x4000, 0xc000, 0x00ff, 0xff00 };
    return (rnd() % 4) ? (uint16_t)rnd() : e[rnd() % (sizeof(e) / sizeof(e[0]))];
}
static void fill(struct vu *u, int n)
{
    int r, i;
    memset(u, 0, sizeof(*u));
    for (r = 0; r < n; r++) {
        uint16_t t[8];
        for (i = 0; i < 8; i++) t[i] = r16();
        u->r[r] = vu_ld(t);
    }
}
static uint16_t lane(struct vu *u, int r, int i) { return vu_get(u->r[r], i); }

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { if (fails++ < 20) { printf("FAIL " __VA_ARGS__); printf("\n"); } } } while (0)

int main(void)
{
    int trial, i;
    for (trial = 0; trial < 200000; trial++) {
        struct vu u;
        fill(&u, 30);
        /* v0 = A int, v1 = A frac, v2 = B int, v3 = B frac, v4 = c */
        for (i = 0; i < 8; i++) {
            int16_t ai = (int16_t)lane(&u, 0, i), bi = (int16_t)lane(&u, 2, i);
            uint16_t af = lane(&u, 1, i), bf = lane(&u, 3, i), c = lane(&u, 4, i);
            (void)c;
            (void)ai; (void)bi; (void)af; (void)bf;
        }
        /* T1: full 32x32: v10 = UL result, v11 = M, v12 = N (before H), v13 = H, v14 = N after H */
        {
            struct vu u1 = u;
            u1.r[20] = u1.r[21] = vu_zero();
            VMUDL(&u1, 10, 1, 3, 0);
            VMADM(&u1, 11, 0, 3, 0);
            VMADN(&u1, 12, 1, 2, 0);
            VMADH(&u1, 13, 0, 2, 0);
            VMADN(&u1, 14, 20, 21, 8);          /* v20 = v21 = 0: adds nothing */
            for (i = 0; i < 8; i++) {
                int16_t ai = (int16_t)lane(&u, 0, i), bi = (int16_t)lane(&u, 2, i);
                uint16_t af = lane(&u, 1, i), bf = lane(&u, 3, i);
                acc_t R = mac_fxfx(ai, af, bi, bf);
                acc_t R_beforeH = R - (((acc_t)ai * bi) << 16);
                acc_t R_beforeN = (acc_t)(((int64_t)af * bf) >> 16) + (acc_t)ai * bf;
                CHECK(lane(&u1, 13, i) == (uint16_t)sclamp(R), "T1 H lane %d", i);
                CHECK(lane(&u1, 14, i) == uclamp(R), "T1 N-after lane %d", i);
                CHECK(lane(&u1, 12, i) == uclamp(R_beforeH), "T1 N-before lane %d", i);
                CHECK(lane(&u1, 11, i) == (uint16_t)sclamp(R_beforeN), "T1 M lane %d", i);
                CHECK(lane(&u1, 10, i) == (uint16_t)(((int64_t)af * bf) >> 16), "T1 UL lane %d", i);
            }
        }
        /* T2: S15.16 x unsigned fraction c (v4 lane 0 broadcast) */
        {
            struct vu u1 = u;
            u1.r[20] = u1.r[21] = vu_zero();
            VMUDL(&u1, 10, 1, 4, 8);
            VMADM(&u1, 11, 0, 4, 8);
            VMADN(&u1, 12, 20, 21, 8);
            for (i = 0; i < 8; i++) {
                int16_t ai = (int16_t)lane(&u, 0, i);
                uint16_t af = lane(&u, 1, i), c = lane(&u, 4, 0);
                acc_t R = mac_fxc(ai, af, c);
                CHECK(lane(&u1, 11, i) == (uint16_t)sclamp(R), "T2 int lane %d", i);
                CHECK(lane(&u1, 12, i) == uclamp(R), "T2 frac lane %d", i);
            }
        }
        /* T3: signed 16 x unsigned 16 -> 32 bit */
        {
            struct vu u1 = u;
            u1.r[20] = u1.r[21] = vu_zero();
            VMUDM(&u1, 10, 0, 4, 9);
            VMADN(&u1, 11, 20, 21, 8);
            for (i = 0; i < 8; i++) {
                int16_t x = (int16_t)lane(&u, 0, i);
                uint16_t c = lane(&u, 4, 1);
                int64_t p = mul_s16u16(x, c);
                CHECK(lane(&u1, 10, i) == (uint16_t)(p >> 16), "T3 int lane %d", i);
                CHECK(lane(&u1, 11, i) == (uint16_t)p, "T3 frac lane %d", i);
            }
        }
        /* T4: A*x + B*y, extracted with VSAR (E = 8: bits 47..32, 9: bits 31..16, 10: bits 15..0) */
        {
            struct vu u1 = u;
            VMUDN(&u1, 10, 1, 4, 10);           /* A frac * x    (x = v4 lane 2) */
            VMADH(&u1, 10, 0, 4, 10);           /* A int * x << 16 */
            VMADN(&u1, 10, 3, 4, 12);           /* B frac * y    (y = v4 lane 4) */
            VMADH(&u1, 10, 2, 4, 12);           /* B int * y << 16 */
            VSAR(&u1, 11, 0, 0, 8);
            VSAR(&u1, 12, 0, 0, 9);
            VSAR(&u1, 13, 0, 0, 10);
            for (i = 0; i < 8; i++) {
                int16_t ai = (int16_t)lane(&u, 0, i), bi = (int16_t)lane(&u, 2, i), x = (int16_t)lane(&u, 4, 2), y = (int16_t)lane(&u, 4, 4);
                uint16_t af = lane(&u, 1, i), bf = lane(&u, 3, i);
                acc_t R = dot2(ai, af, x, bi, bf, y);
                CHECK(lane(&u1, 11, i) == (uint16_t)(R >> 32), "T4 hi lane %d", i);
                CHECK(lane(&u1, 12, i) == (uint16_t)(R >> 16), "T4 md lane %d", i);
                CHECK(lane(&u1, 13, i) == (uint16_t)R, "T4 lo lane %d", i);
                CHECK(lane(&u1, 10, i) == (uint16_t)sclamp(R), "T4 H result lane %d", i);
            }
        }
        /* T5: [VMUDH, VMADH]: the exact sum of two 16x16 products (area), VSAR gives it as 32 bits */
        {
            struct vu u1 = u;
            VMUDH(&u1, 10, 0, 2, 9);            /* v0 * v2[1] */
            VMADH(&u1, 10, 1, 3, 9);            /* + v1 * v3[1] */
            VSAR(&u1, 11, 0, 0, 8);
            VSAR(&u1, 12, 0, 0, 9);
            for (i = 0; i < 8; i++) {
                int32_t s = (int32_t)(int16_t)lane(&u, 0, i) * (int16_t)lane(&u, 2, 1) + (int32_t)(int16_t)lane(&u, 1, i) * (int16_t)lane(&u, 3, 1);
                int64_t sum = (int64_t)(int16_t)lane(&u, 0, i) * (int16_t)lane(&u, 2, 1) + (int64_t)(int16_t)lane(&u, 1, i) * (int16_t)lane(&u, 3, 1);
                (void)s;
                CHECK(hle_dot16((int16_t)lane(&u, 0, i), (int16_t)lane(&u, 2, 1), (int16_t)lane(&u, 1, i), (int16_t)lane(&u, 3, 1)) == (int32_t)sum, "T5 dot16 lane %d", i);
                CHECK(hle_dot16_acc((int16_t)lane(&u, 0, i), (int16_t)lane(&u, 2, 1), (int16_t)lane(&u, 1, i), (int16_t)lane(&u, 3, 1)) == (sum << 16), "T5 dot16_acc lane %d", i);
                CHECK(lane(&u1, 12, i) == (uint16_t)sum && lane(&u1, 11, i) == (uint16_t)(sum >> 16), "T5 sum lane %d", i);
                CHECK(lane(&u1, 10, i) == (uint16_t)sclamp(sum << 16), "T5 result lane %d", i);
            }
        }
        /* T6: 32-bit subtract and add on (int:frac) pairs: VSUBC / VSUB and VADDC / VADD */
        {
            struct vu u1 = u;
            VSUBC(&u1, 10, 1, 3, 0);            /* frac: v1 - v3 */
            VSUB(&u1, 11, 0, 2, 0);             /* int:  v0 - v2 - borrow, saturated */
            VADDC(&u1, 12, 1, 3, 0);
            VADD(&u1, 13, 0, 2, 0);
            for (i = 0; i < 8; i++) {
                int16_t ai = (int16_t)lane(&u, 0, i), bi = (int16_t)lane(&u, 2, i);
                uint16_t af = lane(&u, 1, i), bf = lane(&u, 3, i);
                struct hle_pair s = hle_sub32(hle_fx(ai, af), hle_fx(bi, bf)), d = hle_add32(hle_fx(ai, af), hle_fx(bi, bf));
                CHECK(lane(&u1, 10, i) == s.f, "T6 subc lane %d", i);
                CHECK(lane(&u1, 11, i) == (uint16_t)s.i, "T6 sub lane %d", i);
                CHECK(lane(&u1, 12, i) == d.f, "T6 addc lane %d", i);
                CHECK(lane(&u1, 13, i) == (uint16_t)d.i, "T6 add lane %d", i);
            }
        }
        /* T7: the clip-flag pair: VCH on the integer halves, VCL on the fractions, then CFC2 of VCC.
         * X = (v0:v1) S15.16 x, y, z, w of two vertices in lanes 0..2 / 4..6, W = (v0:v1)[3], [7] (broadcast by element 7 = 3h).
         * VCC low byte, lane k: X <= -W; VCC high byte, lane k: X >= W. */
        {
            struct vu u1 = u;
            uint32_t c;
            VCH(&u1, 10, 0, 0, 7);
            VCL(&u1, 10, 1, 1, 7);
            CFC2(&u1, &c, 1);
            for (i = 0; i < 8; i++) {
                int j = i < 4 ? 3 : 7;
                int64_t X = fx((int16_t)lane(&u, 0, i), lane(&u, 1, i)), W = fx((int16_t)lane(&u, 0, j), lane(&u, 1, j));
                int le = hle_clip_le((int32_t)X, (int32_t)W), ge = hle_clip_ge((int32_t)X, (int32_t)W);
                CHECK(((c >> i) & 1) == (unsigned)le, "T7 le lane %d X=%" PRId64 " W=%" PRId64, i, X, W);
                CHECK(((c >> (8 + i)) & 1) == (unsigned)ge, "T7 ge lane %d X=%" PRId64 " W=%" PRId64, i, X, W);
            }
        }
        /* T8: the reciprocal of a 32-bit value: VRCPH (input half), VRCPL (low half, computes), VRCPH (reads the high half of the result) */
        {
            struct vu u1 = u;
            u1.r[20] = vu_zero();
            VRCPH(&u1, 10, 3, 0, 11);           /* v10[3] <- stale div_out; div_in = v0[3] */
            VRCPL(&u1, 11, 3, 1, 11);           /* v11[3] <- low half of rcp32(v0[3]:v1[3]) */
            VRCPH(&u1, 10, 3, 0, 15);           /* v10[3] <- high half of the result; div_in = v0[7] */
            VRCPL(&u1, 11, 7, 1, 15);           /* v11[7] <- low half of rcp32(v0[7]:v1[7]) */
            VRCPH(&u1, 10, 7, 20, 8);           /* v10[7] <- high half */
            {
                int32_t w3 = (int32_t)((uint32_t)lane(&u, 0, 3) << 16 | lane(&u, 1, 3)), w7 = (int32_t)((uint32_t)lane(&u, 0, 7) << 16 | lane(&u, 1, 7));
                uint32_t r3 = rcp32(w3), r7 = rcp32(w7);
                CHECK(lane(&u1, 10, 3) == (uint16_t)(r3 >> 16) && lane(&u1, 11, 3) == (uint16_t)r3, "T8 lane 3 w=%08x", (unsigned)w3);
                CHECK(lane(&u1, 10, 7) == (uint16_t)(r7 >> 16) && lane(&u1, 11, 7) == (uint16_t)r7, "T8 lane 7 w=%08x", (unsigned)w7);
            }
        }

        /* T10: min / max of 32-bit values with the borrow / compare / merge triple: VSUBC (frac), VLT or VGE (int), VMRG (frac).
         * a = (v0:v1) lane i, b = (v2:v3) lane 1 (broadcast); result int in v10, frac in v11 */
        {
            struct vu u1 = u;
            VSUBC(&u1, 12, 1, 3, 9);
            VLT(&u1, 10, 0, 2, 9);
            VMRG(&u1, 11, 1, 3, 9);
            struct vu u2 = u;
            VSUBC(&u2, 12, 1, 3, 9);
            VGE(&u2, 10, 0, 2, 9);
            VMRG(&u2, 11, 1, 3, 9);
            for (i = 0; i < 8; i++) {
                int64_t a = fx((int16_t)lane(&u, 0, i), lane(&u, 1, i)), b = fx((int16_t)lane(&u, 2, 1), lane(&u, 3, 1));
                int64_t m = hle_min32((int32_t)a, (int32_t)b);
                int64_t M = hle_max32((int32_t)a, (int32_t)b);
                CHECK(lane(&u1, 10, i) == (uint16_t)(m >> 16) && lane(&u1, 11, i) == (uint16_t)m, "T10 min lane %d a=%" PRId64 " b=%" PRId64, i, a, b);
                CHECK(lane(&u2, 10, i) == (uint16_t)(M >> 16) && lane(&u2, 11, i) == (uint16_t)M, "T10 max lane %d", i);
            }
        }
        /* T11: VMULF / VMACF: signed fractional multiply, rounded: (2ab + 0x8000) >> 16 saturated; VMACF accumulates 2ab without the rounding term */
        {
            struct vu u1 = u;
            VMULF(&u1, 10, 0, 2, 0);
            VMACF(&u1, 11, 1, 3, 0);
            for (i = 0; i < 8; i++) {
                int64_t a = (int16_t)lane(&u, 0, i), b = (int16_t)lane(&u, 2, i), c = (int16_t)lane(&u, 1, i), d = (int16_t)lane(&u, 3, i);
                acc_t A1 = hle_mulf((int16_t)a, (int16_t)b), A2 = hle_macf(A1, (int16_t)c, (int16_t)d);
                CHECK(lane(&u1, 10, i) == (uint16_t)sclamp(A1), "T11 mulf lane %d", i);
                CHECK(lane(&u1, 11, i) == (uint16_t)sclamp(A2), "T11 macf lane %d", i);
            }
        }
        /* T12: VABS with itself: |a|, 0x8000 -> 0x7fff */
        {
            struct vu u1 = u;
            VABS(&u1, 10, 0, 0, 0);
            for (i = 0; i < 8; i++) {
                int16_t a = (int16_t)lane(&u, 0, i);
                CHECK(lane(&u1, 10, i) == (uint16_t)hle_abs16(a), "T12 lane %d", i);
            }
        }
        /* T13: VRSQH / VRSQL / VRSQH, the same way as T8 */
        {
            struct vu u1 = u;
            u1.r[20] = vu_zero();
            VRSQH(&u1, 10, 3, 0, 11);
            VRSQL(&u1, 11, 3, 1, 11);
            VRSQH(&u1, 10, 3, 0, 15);
            VRSQL(&u1, 11, 7, 1, 15);
            VRSQH(&u1, 10, 7, 20, 8);
            {
                int32_t w3 = (int32_t)((uint32_t)lane(&u, 0, 3) << 16 | lane(&u, 1, 3)), w7 = (int32_t)((uint32_t)lane(&u, 0, 7) << 16 | lane(&u, 1, 7));
                uint32_t r3 = hle_rsq32(w3), r7 = hle_rsq32(w7);
                CHECK(lane(&u1, 10, 3) == (uint16_t)(r3 >> 16) && lane(&u1, 11, 3) == (uint16_t)r3, "T13 lane 3 w=%08x", (unsigned)w3);
                CHECK(lane(&u1, 10, 7) == (uint16_t)(r7 >> 16) && lane(&u1, 11, 7) == (uint16_t)r7, "T13 lane 7 w=%08x", (unsigned)w7);
            }
        }
        /* T9: the refined reciprocal (vertex w, triangle area): R = rcp32(W); D = 2R; E = 2.0 - D*W; result D*E, all S15.16 chains.
         * Inputs (v28 = frac, v29 = int) in lanes 3 and 7; the result (v27:v26) in the same lanes. */
        {
            struct vu u1 = u;
            uint16_t two[8] = { 0, 0, 0, 2, 0, 0, 0, 2 }, z[8] = { 0 }, k2[8] = { 0, 1, 2, 4, 8, 0, 0, 0 };
            u1.r[31] = vu_ld(z);                /* v31 = constants: lane 0 = 0 (zero multiplier), lane 2 = 2 */
            u1.r[31] = vu_ld(k2);
            u1.r[31] = vu_set(u1.r[31], 0, 0);
            u1.r[23] = vu_ld(two);              /* LQV v23: 2 in the integer half (the microcode loads it from DMEM) */
            /* the subroutine, in VU instructions */
            VRCPH(&u1, 27, 3, 29, 11); VRCPL(&u1, 26, 3, 28, 11); VRCPH(&u1, 27, 3, 29, 15); VRCPL(&u1, 26, 7, 28, 15); VRCPH(&u1, 27, 7, 31, 8);
            VMUDN(&u1, 26, 26, 31, 10); VMADH(&u1, 27, 27, 31, 10); VMADN(&u1, 26, 31, 31, 8);
            VXOR(&u1, 22, 31, 31, 0);
            VMUDL(&u1, 24, 26, 28, 0); VMADM(&u1, 24, 27, 28, 0); VMADN(&u1, 24, 26, 29, 0); VMADH(&u1, 25, 27, 29, 0);
            VSUBC(&u1, 24, 22, 24, 0); VSUB(&u1, 25, 23, 25, 0);
            VMUDL(&u1, 22, 26, 24, 0); VMADM(&u1, 23, 27, 24, 0); VMADN(&u1, 26, 26, 25, 0); VMADH(&u1, 27, 27, 25, 0);
            for (i = 3; i <= 7; i += 4) {
                int32_t w = (int32_t)((uint32_t)lane(&u, 29, i) << 16 | lane(&u, 28, i));
                int16_t ri; uint16_t rf;
                recip_refine(w, &ri, &rf);
                CHECK(lane(&u1, 27, i) == (uint16_t)ri && lane(&u1, 26, i) == rf, "T9 lane %d w=%08x: chain %04x:%04x model %04x:%04x", i, (unsigned)w,
                      lane(&u1, 27, i), lane(&u1, 26, i), (uint16_t)ri, rf);
            }
        }
    }
    printf("prim_test: %d failures\n", fails);
    return fails != 0;
}
