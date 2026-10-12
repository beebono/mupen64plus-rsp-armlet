/*
 * fxtri_spec.c: the struct-level triangle setup (src/hle/f3dex_tri_fx.c) through the spec harness: the adapter reads the triangle code's inputs from DMEM (three vertex records, geometry mode,
 * perspective normalisation, tile byte), calls f3dex_tri_setup(_scalar), writes the RDP bytes at $23 and advances it, and returns the exit pc; the harness compares all of DMEM (but for the
 * output-buffer bytes beyond the command and the triangle scratch: dead ranges of the generator), $23, and the exit pc with the JIT running the real microcode.
 * F3DLX: the DPC_END write that precedes the coefficients is the front-end's, the adapter does it (armlet_mtc0).
 * Routines spec.f3dex.fxtri.scalar / .neon; the generator is tri_gen.inc's.
 */
#include "spec.h"
#include "f3dex_dmem.h"

struct fxt { uint64_t id; unsigned exit_pc; };
static const struct fxt fxts[] = {
    { UC_BANJO121, 0xf10 }, { UC_SF64_NON, 0xf30 }, { UC_TS2_DEX, 0xf08 }, { UC_TS2_DLX, 0x120 }, { UC_MK64_DEX, 0xf98 }, { UC_MK64_DLX, 0x168 },
};

static int fxtri_run(struct armlet_env *e, int neon)
{
    uint32_t *dm = e->rsp.dmem;
    uint32_t *sr = e->rsp.sr;
    const struct f3dex_build *b = f3dex_build_of_ucode(e->routine->ucode);
    unsigned exit_pc = 0;
    for (unsigned i = 0; i < sizeof fxts / sizeof fxts[0]; i++)
        if (fxts[i].id == e->routine->ucode)
            exit_pc = fxts[i].exit_pc;
    struct f3dex_vtx v[3], flat;
    for (int i = 0; i < 3; i++)
        f3dex_load_vtx(&v[i], dm, sr[1 + i] & 0xfff);
    f3dex_load_vtx(&flat, dm, sr[4] & 0xfff);
    struct f3dex_tri_in in;
    in.b = b;
    for (int i = 0; i < 3; i++)
        in.v[i] = &v[i];
    in.flat = &flat;
    in.geom = xd32(dm, 0x114);
    in.persp = xd16(dm, 0x112);
    in.tile_byte = xd8(dm, 0x122);
    uint8_t cmd[0xc0];
    unsigned len = 0;
    int rc = neon ? f3dex_tri_setup(&in, cmd, &len) : f3dex_tri_setup_scalar(&in, cmd, &len);
    if (rc == F3DEX_TRI_CLIP)
        return ARMLET_NATIVE_DECLINE;
    if (rc != F3DEX_TRI_OK)
        return (int)(sr[24] & 0xfff);
    if (b->dlx)
        armlet_mtc0(e, 9, xd32(dm, 0x128));
    for (unsigned i = 0; i < len; i++)
        xs8(dm, sr[23] + i, cmd[i]);
    sr[23] += len;
    return (int)exit_pc;
}
static int fxtri_scalar(struct armlet_env *e) { return fxtri_run(e, 0); }
#define FXT_REG(nm, id, entry, ex, fn) SPEC_REG(nm, "spec.f3dex.fxtri." #fn, id, entry, ex, fxtri_##fn)
FXT_REG(fxtri_s_bk, UC_BANJO121, 0x940, 0xf10, scalar);
FXT_REG(fxtri_s_non, UC_SF64_NON, 0x960, 0xf30, scalar);
FXT_REG(fxtri_s_ts2, UC_TS2_DEX, 0x938, 0xf08, scalar);
FXT_REG(fxtri_s_ts2dlx, UC_TS2_DLX, 0x960, 0x120, scalar);
FXT_REG(fxtri_s_mk, UC_MK64_DEX, 0x9c8, 0xf98, scalar);
FXT_REG(fxtri_s_mkdlx, UC_MK64_DLX, 0x9f0, 0x168, scalar);
#ifdef __ARM_NEON
static int fxtri_neon(struct armlet_env *e) { return fxtri_run(e, 1); }
FXT_REG(fxtri_n_bk, UC_BANJO121, 0x940, 0xf10, neon);
FXT_REG(fxtri_n_non, UC_SF64_NON, 0x960, 0xf30, neon);
FXT_REG(fxtri_n_ts2, UC_TS2_DEX, 0x938, 0xf08, neon);
FXT_REG(fxtri_n_ts2dlx, UC_TS2_DLX, 0x960, 0x120, neon);
FXT_REG(fxtri_n_mk, UC_MK64_DEX, 0x9c8, 0xf98, neon);
FXT_REG(fxtri_n_mkdlx, UC_MK64_DLX, 0x9f0, 0x168, neon);
#endif
