/*
 * mtx_spec.c: spec routines of the matrix commands of F3DEX 1.x / F3DLX (G_MTX, G_POPMTX, the cached-projection (MVP) update, and the light
 * direction recomputation of the lighting overlay), written from the arithmetic of kit/out/mtx.md and checked against the JIT by rt_test.
 *
 *   spec.f3dex.mtx.mtx      G_MTX, the whole handler (DMA of the Mtx into DMEM 0xae0 already done; the matrix-stack push DMA is described by the
 *                           three cp0 register writes the microcode does, as the framework's mtc0 helper; no RDRAM)
 *   spec.f3dex.mtx.pop      G_POPMTX up to the DMA read of the popped matrix (exit: the SP_RD_LEN write, or the main loop when nothing is popped)
 *   spec.f3dex.mtx.poptail  G_POPMTX after that DMA: the stack pointer store and the MVP recomputation (the matrix is in DMEM 0x3e0 by then)
 *   spec.f3dex.mtx.mvp      the MVP recomputation alone (MVP = modelview * projection), entry = the tail shared by G_POPMTX and G_MTX
 *   spec.f3dex.mtx.light    the light-direction recomputation (lighting overlay, entered with the negative light-pointer word in $1)
 *
 * Matrix convention: row vectors, v' = v * M. A matrix in DMEM at a is 16 halves of integer parts (a + 2*(4*row+col)) followed by 16 halves of
 * fraction parts (a + 0x20 + 2*(4*row+col)); the 32-bit value of an element is (int16_t)int_half * 65536 + frac_half (S15.16).
 */
#include "spec.h"
#include <string.h>

/* deliberate breakage for the mutation check of the harness: build with -DMTX_MUTATE=n (MT_MUT(n, correct, broken)) */
#ifdef MTX_MUTATE
#define MT_MUT(n, ok, bad) ((MTX_MUTATE) == (n) ? (bad) : (ok))
#else
#define MT_MUT(n, ok, bad) (ok)
#endif

/* ---------------------------------------------------------------------------------------------------------------------------------------------
 * 48-bit accumulator helpers (kit/vu_primer.md section 1.1, 1.2): acc is held as an int64 and wrapped to 48 bits where a chain ends
 * ------------------------------------------------------------------------------------------------------------------------------------------- */
static inline int64_t sext48(int64_t v) { return (int64_t)((uint64_t)v << 16) >> 16; }
static inline int16_t clamp16(int64_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }
/* sclamp / uclamp views of the accumulator */
static inline int16_t acc_s(int64_t acc) { return clamp16(sext48(acc) >> 16); }
static inline uint16_t acc_u(int64_t acc)
{
    acc = sext48(acc);
    if (acc >= -(1ll << 31) && acc < (1ll << 31))
        return (uint16_t)acc;
    return acc > 0 ? 0xffff : 0;
}
/* one term of the VMUDL / VMADM / VMADN / VMADH chain: floor(A * B / 65536) for A = (ai:af), B = (bi:bf) */
static inline int64_t mul32_term(int16_t ai, uint16_t af, int16_t bi, uint16_t bf)
{
    return (int64_t)(((uint32_t)af * bf) >> 16) + (int64_t)bi * af + (int64_t)ai * bf + ((int64_t)ai * bi << 16);
}

/* ---------------------------------------------------------------------------------------------------------------------------------------------
 * matrices
 * ------------------------------------------------------------------------------------------------------------------------------------------- */
struct mat { int16_t i[4][4]; uint16_t f[4][4]; };

static void mat_load(const struct armlet_rsp *r, unsigned a, struct mat *m)
{
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
        {
            m->i[y][x] = (int16_t)armlet_dm16(r, a + 2 * (4 * y + x));
            m->f[y][x] = armlet_dm16(r, a + 0x20 + 2 * (4 * y + x));
        }
}
static void mat_store(const struct armlet_rsp *r, unsigned a, const struct mat *m)
{
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
        {
            armlet_st16(r, a + 2 * (4 * y + x), (uint16_t)m->i[y][x]);
            armlet_st16(r, a + 0x20 + 2 * (4 * y + x), m->f[y][x]);
        }
}
/* R = A * B: R[y][x] = clamp32( sum_k floor(A[y][k] * B[k][x] / 65536) ), the terms summed in the 48-bit accumulator */
static void mat_mul(struct mat *R, const struct mat *A, const struct mat *B)
{
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
        {
            int64_t acc = 0;
            for (int k = 0; k < 4; k++)
                acc += mul32_term(A->i[y][k], A->f[y][k], B->i[k][x], B->f[k][x]);
            if (MT_MUT(2, 0, 1))                 /* mutation 2: floor of the sum instead of the sum of the floors */
            {
                acc = 0;
                int64_t t = 0;
                for (int k = 0; k < 4; k++)
                    t += ((int64_t)((int32_t)((uint32_t)A->i[y][k] << 16 | A->f[y][k])) * (int32_t)((uint32_t)B->i[k][x] << 16 | B->f[k][x]));
                acc = t >> 16;
            }
            R->i[y][x] = acc_s(acc);
            R->f[y][x] = acc_u(acc);
        }
}

/* DMA by its register writes (SP_MEM_ADDR, SP_DRAM_ADDR, then the length register: 2 = read into DMEM, 3 = write from DMEM) */
static void dma_regs(struct armlet_env *e, unsigned lenreg, uint32_t mem, uint32_t dram, uint32_t len)
{
    armlet_mtc0(e, 0, mem & 0x1fff);             /* the cp0 writes mask the addresses (SP_MEM_ADDR 13 bits, SP_DRAM_ADDR 24 bits) */
    armlet_mtc0(e, 1, dram & 0xffffff);
    armlet_mtc0(e, lenreg, len);
}

#define MV_CUR 0x3e0
#define PROJ   0x420
#define MVP    0x460
#define MTXIN  0xae0
#define SCR    0xde0
#define LIGHTP 0x12c
#define STKPTR 0x134
#define STKLIM 0x15c
#define TASKSTK 0xfe0

/* MVP = MV * P (stored at 0x460); the entry the tail of G_MTX / G_POPMTX jumps to */
static void do_mvp(struct armlet_env *e)
{
    struct mat a, b, r;
    mat_load(&e->rsp, MV_CUR, &a);
    mat_load(&e->rsp, PROJ, &b);
    mat_mul(&r, &a, &b);
    mat_store(&e->rsp, MVP, &r);
}

static int spec_mvp(struct armlet_env *e)
{
    do_mvp(e);
    return 0x09c;
}

/* ---------------------------------------------------------------------------------------------------------------------------------------------
 * G_MTX.  $1 = flags (w0 bits 23..16); bit0 projection, bit1 load (else multiply), bit2 push (modelview only); $18 = w0 & 0x3ff (the length of the
 * command's own DMA; used again as the length of the push DMA)
 * ------------------------------------------------------------------------------------------------------------------------------------------- */
static int spec_mtx(struct armlet_env *e)
{
    const struct armlet_rsp *r = &e->rsp;
    uint32_t flags = r->sr[1];
    /* the light pointer word's top byte becomes 0xff (SBV of v31 byte 6) */
    armlet_st8(r, LIGHTP, MT_MUT(18, 0xff, 0x80));
    unsigned cur;
    if (flags & 1)
        cur = PROJ;                              /* projection: no stack */
    else
    {
        cur = MV_CUR;
        if (flags & 4)
        {
            uint32_t ptr = armlet_dm32(r, STKPTR), lim = armlet_dm32(r, STKLIM);
            if (MT_MUT(3, ptr != lim, ptr < lim))
            {
                armlet_st32(r, STKPTR, ptr + 64);
                dma_regs(e, 3, MV_CUR, ptr, r->sr[18]);       /* the current modelview matrix goes to RDRAM[ptr] */
            }
            /* ptr == lim: stack full, nothing is pushed, nothing else changes */
        }
    }
    struct mat in;
    mat_load(r, MTXIN, &in);
    struct mat res;
    if (MT_MUT(16, flags & 2, flags & 4))
        res = in;                                /* load: the bytes of the Mtx as they are (copied in 16-byte units) */
    else
    {
        struct mat c;
        mat_load(r, cur, &c);
        if (MT_MUT(1, 0, 1)) mat_mul(&res, &c, &in); else
        mat_mul(&res, &in, &c);                  /* multiply: new = in * current, also left in the scratch at 0xde0 */
        mat_store(r, SCR, &res);
    }
    if (MT_MUT(16, flags & 2, flags & 4))
        for (unsigned o = 0; o < 64; o += 4)     /* verbatim copy (also keeps the halves exactly as they are) */
            armlet_st32(r, cur + o, armlet_dm32(r, MTXIN + o));
    else
        mat_store(r, cur, &res);
    do_mvp(e);
    return 0x09c;
}

/* ---------------------------------------------------------------------------------------------------------------------------------------------
 * G_POPMTX, in two stretches because the pop is a DMA read of the matrix from RDRAM (the standalone JIT stops at the length-register write and the
 * harness has no RDRAM):
 *   pop (entry 0x204): up to the write of SP_RD_LEN (exit pc = that mtc0, in the shared DMA routine) or, when nothing is popped, the main loop
 *   poptail (entry: the jal that waits for the DMA): [0x134] = new pointer ($19), then the MVP recomputation
 * ------------------------------------------------------------------------------------------------------------------------------------------- */
static int pop_pre(struct armlet_env *e, int tail_pc)
{
    const struct armlet_rsp *r = &e->rsp;
    armlet_st8(r, LIGHTP, 0xff);
    uint32_t ptr = armlet_dm32(r, STKPTR);
    uint32_t base = armlet_dm32(r, TASKSTK);
    if (MT_MUT(5, (int32_t)(base - ptr) >= 0, (int32_t)(base - ptr) > 0))
        return 0x09c;                            /* stack empty (or below its base): nothing happens */
    ptr -= MT_MUT(6, 64, 32);
    armlet_mtc0(e, 0, MV_CUR);                   /* DMA read: RDRAM[ptr .. ptr + 63] -> DMEM 0x3e0; the length register is written next (not here) */
    armlet_mtc0(e, 1, ptr & 0xffffff);
    return tail_pc;                              /* the BREAK sits in the delay slot of the DMA routine's return: the pc is the return address (the tail) */
}
static int pop_pre_228(struct armlet_env *e) { return pop_pre(e, 0x228); }
static int pop_pre_220(struct armlet_env *e) { return pop_pre(e, 0x220); }
static int pop_pre_218(struct armlet_env *e) { return pop_pre(e, 0x218); }
static int pop_pre_2d8(struct armlet_env *e) { return pop_pre(e, 0x2d8); }
static int pop_pre_2d0(struct armlet_env *e) { return pop_pre(e, 0x2d0); }
static int spec_poptail(struct armlet_env *e)
{
    armlet_st32(&e->rsp, STKPTR, e->rsp.sr[19]);
    do_mvp(e);
    return 0x09c;
}

/* ---------------------------------------------------------------------------------------------------------------------------------------------
 * Light direction recomputation
 * ------------------------------------------------------------------------------------------------------------------------------------------- */

/* the reciprocal-square-root ROM half of the RSP divide unit (indices 512..1023 of rsp_reciprocal_rom): formula, entry 1 is the saturated one */
static uint16_t rsq_rom[512];
static int rsq_ready;
static uint32_t isqrt64(uint64_t v)
{
    uint64_t lo = 0, hi = 1ull << 32;
    while (hi - lo > 1)
    {
        uint64_t mid = (lo + hi) >> 1;
        if (mid * mid <= v) lo = mid; else hi = mid;
    }
    return (uint32_t)lo;
}
static void rsq_init(void)
{
    for (unsigned j = 0; j < 512; j++)
    {
        uint64_t a = (j + 512) >> (j & 1);
        rsq_rom[j] = j == 1 ? 0xffff : (uint16_t)isqrt64((1ull << 42) / a);
    }
    rsq_ready = 1;
}
#ifdef MTX_SPEC_CHECK_ROM
extern const uint16_t rsp_reciprocal_rom[1024];
#endif

/* VRSQH / VRSQL pair on the 32-bit input x: the 32-bit result (vu_primer.md 1.5, VRSQ variant) */
static int32_t vrsq32(int32_t input)
{
    if (!rsq_ready) rsq_init();
    int32_t mask = input >> 31;
    int32_t data = input ^ mask;
    if (input > -32768)
        data -= mask;
    if (data == 0)
        return 0x7fffffff;
    if (input == -32768)
        return (int32_t)0xffff0000u;
    unsigned shift = (unsigned)__builtin_clz((uint32_t)data);
    unsigned idx = (unsigned)((((uint64_t)(uint32_t)data << shift) & 0x7FC00000u) >> 22);
    idx = ((idx | 0x200) & 0x3FE) | MT_MUT(10, shift & 1, 0);
#ifdef MTX_SPEC_CHECK_ROM
    if (rsp_reciprocal_rom[idx] != rsq_rom[idx - 512]) __builtin_trap();
#endif
    int32_t result = rsq_rom[idx - 512];
    result = ((0x10000 | result) << 14) >> ((31 - shift) >> 1);
    return result ^ mask;
}

/* STV / LTV on the register group v8..v15 held as 8 x 8 halves (the fork's definitions, byte-exact) */
struct grp { uint16_t h[8][8]; };
static inline uint8_t grp_byte(const struct grp *g, int r, unsigned i) { return (uint8_t)(g->h[r][i >> 1] >> ((i & 1) ? 0 : 8)); }
static inline void grp_put(struct grp *g, int r, unsigned i, uint8_t v)
{
    uint16_t *p = &g->h[r][i >> 1];
    *p = (i & 1) ? (uint16_t)((*p & 0xff00) | v) : (uint16_t)((*p & 0x00ff) | (v << 8));
}
static void do_stv(const struct armlet_rsp *r, const struct grp *g, unsigned e, unsigned a)
{
    e &= ~1u;
    unsigned element = 16 - e, base = (a & 7) - e;
    a &= ~7u;
    for (int i = 0; i < 8; i++)
    {
        armlet_st8(r, a + (base++ & 0xf), grp_byte(g, i, element++ & 0xf));
        armlet_st8(r, a + (base++ & 0xf), grp_byte(g, i, element++ & 0xf));
    }
}
static void do_ltv(const struct armlet_rsp *r, struct grp *g, unsigned e, unsigned a)
{
    unsigned start = a & ~7u;
    a = start + ((e + (a & 8)) & 0xf);
    unsigned j = e >> 1;
    for (unsigned i = 0; i < 16; j++)
    {
        j &= 7;
        grp_put(g, (int)j, i++, armlet_dm8(r, a++));
        if (a == start + 16) a = start;
        grp_put(g, (int)j, i++, armlet_dm8(r, a++));
        if (a == start + 16) a = start;
    }
}

static int light_core(struct armlet_env *e, int exit_pc)
{
    const struct armlet_rsp *r = &e->rsp;
    uint32_t p0 = r->sr[1] & 0xfff;
    armlet_st32(r, LIGHTP, p0);                  /* flag cleared: the pointer without its top bits */

    /* v8..v15: modelview rows 0..2 (ints v8..v10, fractions v12..v14, each row twice: lanes 0..3 and 4..7); v11 / v15 are the MVP row 3 (ints /
     * fractions, as the vertex handler loaded them from 0x460: they are not touched here before the transposition) */
    struct grp g;
    for (int rr = 0; rr < 8; rr++)
        for (int l = 0; l < 8; l++)
        {
            unsigned a;
            if (rr < 3) a = MV_CUR + 8 * rr + 2 * (l & 3);                 /* v8..v10 */
            else if (rr == 3) a = MVP + 24 + 2 * (l & 3);                  /* v11 */
            else if (rr < 7) a = MV_CUR + 0x20 + 8 * (rr - 4) + 2 * (l & 3); /* v12..v14 */
            else a = MT_MUT(17, MVP + 0x20 + 24, MVP + 0x20 + 16) + 2 * (l & 3);                        /* v15 */
            g.h[rr][l] = armlet_dm16(r, a);
        }
    /* the transposition through the scratch at 0xde0 */
    do_stv(r, &g, 2, SCR + 16);
    do_stv(r, &g, 4, SCR + 32);
    do_stv(r, &g, 12, SCR + 48);
    do_stv(r, &g, 14, SCR + 64);
    do_ltv(r, &g, 14, SCR + 16);
    do_ltv(r, &g, 12, SCR + 32);
    do_ltv(r, &g, 4, SCR + 48);
    do_ltv(r, &g, 2, SCR + 64);
    for (int rr = 4; rr < 7; rr++)               /* SDV rr,8 -> scratch; LDV rr,0 <- scratch: lanes 4..7 into lanes 0..3 of v12..v14 */
    {
        unsigned a = SCR + 16 * (rr - 3);
        for (int l = 4; l < 8; l++)
            armlet_st16(r, a + 2 * (l - 4), g.h[rr][l]);
        for (int l = 0; l < 4; l++)
            g.h[rr][l] = MT_MUT(12, armlet_dm16(r, a + 2 * l), g.h[rr][l]);
    }
#ifdef MTX_SPEC_SELFCHECK
    /* the closed form of the register contents after the transposition (kit/out/mtx.md): lanes 0..2 of v8..v10 / v12..v14 are the columns of the
     * 3x3 block of the modelview; lane 3 takes M[0][3] and the MVP row 3 elements 1, 2 */
    for (int j = 0; j < 3; j++)
        for (int k = 0; k < 3; k++)
            if (g.h[k][j] != armlet_dm16(r, MV_CUR + 8 * j + 2 * k) || g.h[4 + k][j] != armlet_dm16(r, MV_CUR + 0x20 + 8 * j + 2 * k))
                __builtin_trap();
    if (g.h[0][3] != armlet_dm16(r, MV_CUR + 6) || g.h[4][3] != armlet_dm16(r, MV_CUR + 0x20 + 6) ||
        g.h[1][3] != armlet_dm16(r, MVP + 24 + 2) || g.h[2][3] != armlet_dm16(r, MVP + 24 + 4) ||
        g.h[5][3] != armlet_dm16(r, MVP + 0x20 + 24 + 2) || g.h[6][3] != armlet_dm16(r, MVP + 0x20 + 24 + 4))
        __builtin_trap();
#endif
    /* constants of the clamp: v2 lanes 0..2 */
    int16_t c_lo = (int16_t)armlet_dm16(r, 0x208), c_hi = (int16_t)armlet_dm16(r, 0x20a);
    uint16_t c_mul = armlet_dm16(r, 0x20c);

    int32_t p = (int32_t)p0;
    for (;;)
    {
        int16_t dd[3];
        for (int k = 0; k < 3; k++)
            dd[k] = (int16_t)((int8_t)armlet_dm8(r, 0x1b8 + p + k) * MT_MUT(7, 128, 127));   /* LPV + VMULF by 0x4000: exactly 128 * d */
        int16_t Nh[4], Qh[4];
        uint16_t Nl[4], Ql[4];
        for (int j = 0; j < 4; j++)
        {
            /* N = 256 * (M row j . d) as a 32-bit pair, via the fractions, the clipped integer part of that, then the integer parts */
            int64_t acc = 0;
            for (int k = 0; k < 3; k++)
                acc += (int64_t)g.h[4 + k][j] * dd[k];                       /* VMUDN / VMADN: unsigned fraction x signed */
            int16_t v3 = acc_s(acc);                                          /* VMADM by zero: sclamp */
            acc = MT_MUT(8, 0, acc) + (int64_t)v3 * 2;                       /* VMUDM by 2: restarts the accumulator */
            for (int k = 0; k < 3; k++)
                acc += 2 * (int64_t)(int16_t)g.h[k == 0 ? 0 : k == 1 ? 1 : 2][j] * dd[k];   /* VMACF */
            Nh[j] = acc_s(acc);
            Nl[j] = acc_u(acc);
            /* Q = floor(N * N / 65536); the fraction half is read before the high x high term */
            int64_t a2 = (int64_t)(((uint32_t)Nl[j] * Nl[j]) >> 16) + (int64_t)Nh[j] * Nl[j] + (int64_t)Nl[j] * Nh[j];
            Ql[j] = acc_u(a2);
            a2 += (int64_t)Nh[j] * Nh[j] << 16;
            Qh[j] = acc_s(a2);
        }
        /* S = Q0 + Q1 + Q2 in lane 2: first pass adds the neighbour lane, second adds lane 0 of the first pass (carry into the saturating add) */
        uint16_t lo1[4];
        int16_t hi1[4];
        for (int j = 0; j < 4; j++)
        {
            uint32_t s = (uint32_t)Ql[j] + Ql[j | 1];
            lo1[j] = (uint16_t)s;
            hi1[j] = MT_MUT(9, clamp16((int64_t)Qh[j] + Qh[j | 1] + (s >> 16)), (int16_t)((int64_t)Qh[j] + Qh[j | 1] + (s >> 16)));
        }
        uint32_t s2 = (uint32_t)Ql[2] + lo1[0];
        uint16_t Slo = (uint16_t)s2;
        int16_t Shi = clamp16((int64_t)Qh[2] + hi1[0] + (s2 >> 16));
        int32_t R = vrsq32((int32_t)(((uint32_t)(uint16_t)Shi << 16) | Slo));
        /* T = floor(R * 64 / 65536) as a pair */
        int64_t at = (int64_t)(((uint32_t)(uint16_t)R * 0x40) >> 16) + (int64_t)(int16_t)(R >> 16) * 0x40;
        int16_t Ti = acc_s(at);
        uint16_t Tf = acc_u(at);
        uint8_t out[8];
        for (int j = 0; j < 4; j++)
        {
            /* U = floor(N * T / 65536); only the fraction half is used (its low 16 bits) */
            int64_t au = (int64_t)(((uint32_t)Nl[j] * Tf) >> 16) + (int64_t)Nh[j] * Tf + (int64_t)Nl[j] * Ti + ((int64_t)Nh[j] * Ti << 16);
            int16_t v = (int16_t)acc_u(au);
            if (v < c_lo) v = c_lo;
            if (v > c_hi) v = MT_MUT(14, c_hi, c_hi - 64);
            uint16_t m = (uint16_t)((uint16_t)v * c_mul);
            out[j] = (uint8_t)((int16_t)m >> 8);
            out[j + 4] = MT_MUT(11, out[j], 0);
        }
        /* the lanes 4..7 are overwritten by the copy of the first word, so only lanes 0..3 matter */
        for (int j = 0; j < 8; j++)
            armlet_st8(r, 0x1c0 + p + j, out[j]);
        if (MT_MUT(13, p <= 0, p < 0))
            break;
        p -= 32;
    }
    return exit_pc;
}

/* ---- registration: entry pcs per build (kit/pcmap) --------------------------------------------------------------------------------------------- */
static int light_72c(struct armlet_env *e) { return light_core(e, 0x72c); }
static int light_74c(struct armlet_env *e) { return light_core(e, 0x74c); }
static int light_724(struct armlet_env *e) { return light_core(e, 0x724); }
static int light_7b4(struct armlet_env *e) { return light_core(e, 0x7b4); }
static int light_7dc(struct armlet_env *e) { return light_core(e, 0x7dc); }

SPEC_REG(spec_mtx_bk, "spec.f3dex.mtx.mtx.banjo121", UC_BANJO121, 0x554, 0, spec_mtx);
SPEC_REG(spec_mtx_non, "spec.f3dex.mtx.mtx.sf64_non", UC_SF64_NON, 0x574, 0, spec_mtx);
SPEC_REG(spec_mtx_ts2, "spec.f3dex.mtx.mtx.ts2_dex", UC_TS2_DEX, 0x54c, 0, spec_mtx);
SPEC_REG(spec_mtx_dlx, "spec.f3dex.mtx.mtx.ts2_dlx", UC_TS2_DLX, 0x578, 0, spec_mtx);
SPEC_REG(spec_mtx_mk, "spec.f3dex.mtx.mtx.mk64_dex", UC_MK64_DEX, 0x5dc, 0, spec_mtx);
SPEC_REG(spec_mtx_mkx, "spec.f3dex.mtx.mtx.mk64_dlx", UC_MK64_DLX, 0x608, 0, spec_mtx);

SPEC_REG(spec_pop_bk, "spec.f3dex.mtx.pop.banjo121", UC_BANJO121, 0x204, 0, pop_pre_228);
SPEC_REG(spec_pop_non, "spec.f3dex.mtx.pop.sf64_non", UC_SF64_NON, 0x204, 0, pop_pre_228);
SPEC_REG(spec_pop_ts2, "spec.f3dex.mtx.pop.ts2_dex", UC_TS2_DEX, 0x1fc, 0, pop_pre_220);
SPEC_REG(spec_pop_dlx, "spec.f3dex.mtx.pop.ts2_dlx", UC_TS2_DLX, 0x1f4, 0, pop_pre_218);
SPEC_REG(spec_pop_mk, "spec.f3dex.mtx.pop.mk64_dex", UC_MK64_DEX, 0x2b4, 0, pop_pre_2d8);
SPEC_REG(spec_pop_mkx, "spec.f3dex.mtx.pop.mk64_dlx", UC_MK64_DLX, 0x2ac, 0, pop_pre_2d0);

SPEC_REG(spec_poptail_bk, "spec.f3dex.mtx.poptail.banjo121", UC_BANJO121, 0x228, 0, spec_poptail);
SPEC_REG(spec_poptail_non, "spec.f3dex.mtx.poptail.sf64_non", UC_SF64_NON, 0x228, 0, spec_poptail);
SPEC_REG(spec_poptail_ts2, "spec.f3dex.mtx.poptail.ts2_dex", UC_TS2_DEX, 0x220, 0, spec_poptail);
SPEC_REG(spec_poptail_dlx, "spec.f3dex.mtx.poptail.ts2_dlx", UC_TS2_DLX, 0x218, 0, spec_poptail);
SPEC_REG(spec_poptail_mk, "spec.f3dex.mtx.poptail.mk64_dex", UC_MK64_DEX, 0x2d8, 0, spec_poptail);
SPEC_REG(spec_poptail_mkx, "spec.f3dex.mtx.poptail.mk64_dlx", UC_MK64_DLX, 0x2d0, 0, spec_poptail);

SPEC_REG(spec_mvp_bk, "spec.f3dex.mtx.mvp.banjo121", UC_BANJO121, 0x5bc, 0, spec_mvp);
SPEC_REG(spec_mvp_non, "spec.f3dex.mtx.mvp.sf64_non", UC_SF64_NON, 0x5dc, 0, spec_mvp);
SPEC_REG(spec_mvp_ts2, "spec.f3dex.mtx.mvp.ts2_dex", UC_TS2_DEX, 0x5b4, 0, spec_mvp);
SPEC_REG(spec_mvp_dlx, "spec.f3dex.mtx.mvp.ts2_dlx", UC_TS2_DLX, 0x5e0, 0, spec_mvp);
SPEC_REG(spec_mvp_mk, "spec.f3dex.mtx.mvp.mk64_dex", UC_MK64_DEX, 0x644, 0, spec_mvp);
SPEC_REG(spec_mvp_mkx, "spec.f3dex.mtx.mvp.mk64_dlx", UC_MK64_DLX, 0x670, 0, spec_mvp);

SPEC_REG(spec_light_bk, "spec.f3dex.mtx.light.banjo121", UC_BANJO121, 0x7c4, 0, light_72c);
SPEC_REG(spec_light_non, "spec.f3dex.mtx.light.sf64_non", UC_SF64_NON, 0x7e4, 0, light_74c);
SPEC_REG(spec_light_ts2, "spec.f3dex.mtx.light.ts2_dex", UC_TS2_DEX, 0x7bc, 0, light_724);
SPEC_REG(spec_light_dlx, "spec.f3dex.mtx.light.ts2_dlx", UC_TS2_DLX, 0x7e4, 0, light_74c);
SPEC_REG(spec_light_mk, "spec.f3dex.mtx.light.mk64_dex", UC_MK64_DEX, 0x84c, 0, light_7b4);
SPEC_REG(spec_light_mkx, "spec.f3dex.mtx.light.mk64_dlx", UC_MK64_DLX, 0x874, 0, light_7dc);
