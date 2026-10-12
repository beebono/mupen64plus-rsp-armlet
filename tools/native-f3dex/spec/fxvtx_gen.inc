// scenarios of the struct-level vertex function: light_gen.inc's, plus the dead ranges of the F3DLX no-G_CLIPPING flags (junk the function does not model: 0)
#include "light_gen.inc"
static void gen_fxvtx(Machine &m, const Ucode &u, Scenario &sc)
{
    gen_light(m, u, sc);
    bool dlx = u.id == 0x7502cfb7098eeffbull || u.id == 0x5f33f7c2be924de0ull;
    if (dlx && !(get16(m.dmem, 0x114) & 0x80))
        for (unsigned k = 0; k < 32; k++)
            sc.dead.push_back({ 0x4a0 + 0x28 * k + 36, 0x4a0 + 0x28 * k + 40 });
}
static struct FxVtxGenInit
{
    FxVtxGenInit() { gens.push_back({ "spec.f3dex.fxvtx.", gen_fxvtx }); }
} fxvtx_gen_init;
