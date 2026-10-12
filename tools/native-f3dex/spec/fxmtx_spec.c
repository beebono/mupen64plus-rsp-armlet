/*
 * fxmtx_spec.c: the struct-level matrix functions (src/hle/f3dex_mtx_fx.c) run through the spec harness against the real microcode on the JIT (rt_test compares all of DMEM and the exit pc):
 *
 *   spec.f3dex.fxmtx.mvp.{scalar,neon}      MVP = modelview x projection (entry: the tail shared by G_MTX and G_POPMTX); f3dex_mtx_mul
 *   spec.f3dex.fxmtx.mtx.{scalar,neon}      the G_MTX handler up to the main loop (as spec.f3dex.mtx.mtx: the light flag, the stack push DMA by its cp0 register writes, load / multiply with
 *                                           the product also left in the scratch 0xde0, then the MVP); the multiply is f3dex_mtx_mul(in, cur, cur), the output aliasing b
 *   spec.f3dex.fxmtx.relight.{scalar,neon}  the light-direction recomputation (lighting overlay 2); f3dex_relight. The fourth byte of every direction (0x1c3, 0x1c7 + 32k) and the scratch
 *                                           0xde0 .. 0xe40 are declared dead by the generator (the function does not model them).
 * (neon = the NEON entry points; relight has no NEON form, f3dex_relight is the scalar form.) Generators: fxmtx_gen.inc.
 */
#include "spec.h"
#include "f3dex_dmem.h"

#define FX_MV 0x3e0
#define FX_PROJ 0x420
#define FX_MVP 0x460
#define FX_MTXIN 0xae0
#define FX_SCR 0xde0

typedef void (*mulfn)(const struct f3dex_mtx *, const struct f3dex_mtx *, struct f3dex_mtx *);

static void do_mvp(struct armlet_env *e, mulfn mul)
{
    uint32_t *dm = e->rsp.dmem;
    struct f3dex_mtx a, b, r;
    f3dex_load_mtx(&a, dm, FX_MV, 0);
    f3dex_load_mtx(&b, dm, FX_PROJ, 0);
    mul(&a, &b, &r);
    f3dex_store_mtx(&r, dm, FX_MVP);
}
static int fxmvp_s(struct armlet_env *e) { do_mvp(e, f3dex_mtx_mul_scalar); return 0x09c; }

static int fxmtx_run(struct armlet_env *e, mulfn mul)
{
    uint32_t *dm = e->rsp.dmem;
    const uint32_t *sr = e->rsp.sr;
    uint32_t flags = sr[1];
    xs8(dm, 0x12c, 0xff);                                       /* the light pointer word's top byte */
    unsigned cur;
    if (flags & 1)
        cur = FX_PROJ;
    else {
        cur = FX_MV;
        if (flags & 4) {
            uint32_t ptr = xd32(dm, 0x134), lim = xd32(dm, 0x15c);
            if (ptr != lim) {
                xs32(dm, 0x134, ptr + 64);
                armlet_mtc0(e, 0, FX_MV & 0x1fff);              /* the push DMA by its register writes */
                armlet_mtc0(e, 1, ptr & 0xffffff);
                armlet_mtc0(e, 3, sr[18]);
            }
        }
    }
    struct f3dex_mtx in, c;
    f3dex_load_mtx(&in, dm, FX_MTXIN, 0);
    if (flags & 2)
        f3dex_store_mtx(&in, dm, cur);                          /* load: the bytes as they are */
    else {
        f3dex_load_mtx(&c, dm, cur, 0);
        mul(&in, &c, &c);                                       /* out aliases b */
        f3dex_store_mtx(&c, dm, cur);
        f3dex_store_mtx(&c, dm, FX_SCR);
    }
    do_mvp(e, mul);
    return 0x09c;
}
static int fxmtx_s(struct armlet_env *e) { return fxmtx_run(e, f3dex_mtx_mul_scalar); }

static const struct { uint64_t id; unsigned exitpc; } fxl_exit[] = {
    { UC_BANJO121, 0x72c }, { UC_SF64_NON, 0x74c }, { UC_TS2_DEX, 0x724 }, { UC_TS2_DLX, 0x74c }, { UC_MK64_DEX, 0x7b4 }, { UC_MK64_DLX, 0x7dc },
};
static int fxrelight_run(struct armlet_env *e, void (*rl)(const struct f3dex_mtx *, const int8_t[][3], unsigned, int8_t[][4]))
{
    uint32_t *dm = e->rsp.dmem;
    uint32_t p0 = e->rsp.sr[1] & 0xfff;
    unsigned exitpc = 0;
    for (unsigned i = 0; i < sizeof fxl_exit / sizeof fxl_exit[0]; i++)
        if (fxl_exit[i].id == e->routine->ucode)
            exitpc = fxl_exit[i].exitpc;
    if (!exitpc || (p0 & 31) || (p0 >> 5) > F3DEX_MAX_REC)
        return ARMLET_NATIVE_DECLINE;
    unsigned nrec = p0 >> 5;
    struct f3dex_mtx mv;
    int8_t raw[F3DEX_MAX_REC + 1][3], out[F3DEX_MAX_REC + 1][4];
    f3dex_load_mtx(&mv, dm, FX_MV, 0);
    for (unsigned k = 0; k <= nrec; k++)
        for (int c = 0; c < 3; c++)
            raw[k][c] = (int8_t)xd8(dm, 0x1b8 + 32 * k + c);
    rl(&mv, raw, nrec, out);
    xs32(dm, 0x12c, p0);
    for (unsigned k = 0; k <= nrec; k++)
        for (int c = 0; c < 4; c++) {
            xs8(dm, 0x1c0 + 32 * k + c, (uint8_t)out[k][c]);
            xs8(dm, 0x1c4 + 32 * k + c, (uint8_t)out[k][c]);
        }
    return exitpc;
}
static int fxrelight_s(struct armlet_env *e) { return fxrelight_run(e, f3dex_relight_scalar); }

#define REG6(var, nm, fn, e0, e1, e2, e3, e4, e5) \
    SPEC_REG(var##_bk, nm, UC_BANJO121, e0, 0, fn); \
    SPEC_REG(var##_non, nm, UC_SF64_NON, e1, 0, fn); \
    SPEC_REG(var##_ts2, nm, UC_TS2_DEX, e2, 0, fn); \
    SPEC_REG(var##_ts2dlx, nm, UC_TS2_DLX, e3, 0, fn); \
    SPEC_REG(var##_mk, nm, UC_MK64_DEX, e4, 0, fn); \
    SPEC_REG(var##_mkdlx, nm, UC_MK64_DLX, e5, 0, fn)

REG6(fxmvp_s, "spec.f3dex.fxmtx.mvp.scalar", fxmvp_s, 0x5bc, 0x5dc, 0x5b4, 0x5e0, 0x644, 0x670);
REG6(fxmtx_s, "spec.f3dex.fxmtx.mtx.scalar", fxmtx_s, 0x554, 0x574, 0x54c, 0x578, 0x5dc, 0x608);
REG6(fxrl_s, "spec.f3dex.fxmtx.relight.scalar", fxrelight_s, 0x7c4, 0x7e4, 0x7bc, 0x7e4, 0x84c, 0x874);
#ifdef __ARM_NEON
static int fxmvp_n(struct armlet_env *e) { do_mvp(e, f3dex_mtx_mul); return 0x09c; }
static int fxmtx_n(struct armlet_env *e) { return fxmtx_run(e, f3dex_mtx_mul); }
static int fxrelight_n(struct armlet_env *e) { return fxrelight_run(e, f3dex_relight); }
REG6(fxmvp_n, "spec.f3dex.fxmtx.mvp.neon", fxmvp_n, 0x5bc, 0x5dc, 0x5b4, 0x5e0, 0x644, 0x670);
REG6(fxmtx_n, "spec.f3dex.fxmtx.mtx.neon", fxmtx_n, 0x554, 0x574, 0x54c, 0x578, 0x5dc, 0x608);
REG6(fxrl_n, "spec.f3dex.fxmtx.relight.neon", fxrelight_n, 0x7c4, 0x7e4, 0x7bc, 0x7e4, 0x84c, 0x874);
#endif
