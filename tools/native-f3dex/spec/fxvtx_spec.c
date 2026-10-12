/*
 * fxvtx_spec.c: the struct-level vertex function (src/hle/f3dex_vtx_fx.c) run through the spec harness: the adapter reads the G_VTX handler's inputs from DMEM, calls f3dex_vtx_xform(_scalar),
 * writes the vertex records back and the few leftovers the microcode writes (0x106, the copy of A's flags in B), and the harness compares all of DMEM and the exit pc with the JIT running the real
 * microcode (routines spec.f3dex.fxvtx.scalar / .neon; the generator is light_gen.inc's).
 */
#include "spec.h"
#include "f3dex_dmem.h"

struct fxb { uint64_t id; unsigned tab, ret; };
static const struct fxb fxbs[] = {
    { UC_BANJO121, 0x31e, 0xba }, { UC_SF64_NON, 0x31e, 0xba }, { UC_TS2_DEX, 0x31e, 0xba },
    { UC_TS2_DLX, 0x31e, 0xba }, { UC_MK64_DEX, 0x31c, 0xbe }, { UC_MK64_DLX, 0x31c, 0xbe },
};
static const struct fxb *fxb_of(const struct armlet_env *e)
{
    for (unsigned i = 0; i < sizeof fxbs / sizeof fxbs[0]; i++)
        if (fxbs[i].id == e->routine->ucode)
            return &fxbs[i];
    return &fxbs[0];
}

static int fxvtx_run(struct armlet_env *e, int neon)
{
    uint32_t *dm = e->rsp.dmem;
    const uint32_t *sr = e->rsp.sr;
    const struct fxb *fb = fxb_of(e);
    const struct f3dex_build *b = f3dex_build_of_ucode(e->routine->ucode);
    struct f3dex_vctx c;
    if (f3dex_load_vctx(&c, dm, b))
        return ARMLET_NATIVE_DECLINE;
    for (int n = 0; n < 8; n++)
        c.vco_carry |= (e->rsp.flags[0][8 + n] ? 1 : 0) << n;
    unsigned r7 = xd16(dm, fb->tab + sr[1]);
    unsigned n = xd8(dm, sr[27] - 6) >> 2;
    if (n == 0)
        n = 1;
    struct f3dex_vin in[32];
    struct f3dex_vtx out[32];
    for (unsigned i = 0; i < n; i++)
        f3dex_load_vin(&in[i], dm, sr[22] + 16 * i, 0);
    int rc = neon ? f3dex_vtx_xform(&c, in, n, out) : f3dex_vtx_xform_scalar(&c, in, n, out);
    if (rc)
        return ARMLET_NATIVE_DECLINE;
    xs16(dm, 0x106, xd16(dm, fb->ret));
    for (unsigned i = 0; i < n; i++) {
        f3dex_store_vtx(&out[i], dm, r7 + 40 * i);
        if (i & 1)
            xs16(dm, r7 + 40 * i + 38, out[i - 1].flags);       /* the leftover: B's word store also writes A's flags */
    }
    return xd16(dm, 0x106) & 0xfff;
}
static int fxvtx_scalar(struct armlet_env *e) { return fxvtx_run(e, 0); }
SPEC_REG(fxvtx_s_bk, "spec.f3dex.fxvtx.scalar", UC_BANJO121, 0x3a8, 0, fxvtx_scalar);
SPEC_REG(fxvtx_s_non, "spec.f3dex.fxvtx.scalar", UC_SF64_NON, 0x3a8, 0, fxvtx_scalar);
SPEC_REG(fxvtx_s_ts2, "spec.f3dex.fxvtx.scalar", UC_TS2_DEX, 0x3a0, 0, fxvtx_scalar);
SPEC_REG(fxvtx_s_ts2dlx, "spec.f3dex.fxvtx.scalar", UC_TS2_DLX, 0x3b0, 0, fxvtx_scalar);
SPEC_REG(fxvtx_s_mk, "spec.f3dex.fxvtx.scalar", UC_MK64_DEX, 0x430, 0, fxvtx_scalar);
SPEC_REG(fxvtx_s_mkdlx, "spec.f3dex.fxvtx.scalar", UC_MK64_DLX, 0x440, 0, fxvtx_scalar);
#ifdef __ARM_NEON
static int fxvtx_neon(struct armlet_env *e) { return fxvtx_run(e, 1); }
SPEC_REG(fxvtx_n_bk, "spec.f3dex.fxvtx.neon", UC_BANJO121, 0x3a8, 0, fxvtx_neon);
SPEC_REG(fxvtx_n_non, "spec.f3dex.fxvtx.neon", UC_SF64_NON, 0x3a8, 0, fxvtx_neon);
SPEC_REG(fxvtx_n_ts2, "spec.f3dex.fxvtx.neon", UC_TS2_DEX, 0x3a0, 0, fxvtx_neon);
SPEC_REG(fxvtx_n_ts2dlx, "spec.f3dex.fxvtx.neon", UC_TS2_DLX, 0x3b0, 0, fxvtx_neon);
SPEC_REG(fxvtx_n_mk, "spec.f3dex.fxvtx.neon", UC_MK64_DEX, 0x430, 0, fxvtx_neon);
SPEC_REG(fxvtx_n_mkdlx, "spec.f3dex.fxvtx.neon", UC_MK64_DLX, 0x440, 0, fxvtx_neon);
#endif
