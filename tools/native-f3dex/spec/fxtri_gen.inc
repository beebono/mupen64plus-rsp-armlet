// scenarios of the struct-level triangle setup: tri_gen.inc's, plus the dead range of the output buffer beyond the command (the microcode's coefficient code writes past the length it reserves)
#include "tri_gen.inc"
static void gen_fxtri(Machine &m, const Ucode &u, Scenario &sc)
{
    gen_tri_spec(m, u, sc);
    auto &st = m.cpu.get_state();
    // directed variants: steep edges (slopes beyond the +-0x1cc limit), and small texture coordinates (so that the integer halves of the products tie and the fraction decides the maximum)
    unsigned mode = rnd() % 8;
    if (mode == 0)
    {
        int base = int(rnd() % 1000);
        for (int i = 1; i <= 3; i++)
        {
            unsigned v = st.sr[i] & 0xfff;
            put16(m.dmem, v + 24, uint16_t(int(rnd() % 8000) - 4000));
            put16(m.dmem, v + 26, uint16_t(base + int(rnd() % 4)));
            put16(m.dmem, v + 36, 0);
        }
    }
    else if (mode == 1 || mode == 2)
    {
        for (int i = 1; i <= 3; i++)
        {
            unsigned v = st.sr[i] & 0xfff;
            put16(m.dmem, v + 20, uint16_t(int(rnd() % 80) - 40));
            put16(m.dmem, v + 22, uint16_t(int(rnd() % 80) - 40));
            put16(m.dmem, v + 36, 0);
        }
        put16(m.dmem, 0x114 + 2, (get16(m.dmem, 0x114 + 2) | 2) & ~0x3000);
    }
    unsigned g = get16(m.dmem, 0x114 + 2) & 7;       // low byte of the geometry word (big-endian halfwords: +2 is its low half)
    unsigned len = 0x20 + ((g & 4) ? 0x40 : 0) + ((g & 2) ? 0x40 : 0) + ((g & 1) ? 0x10 : 0);
    sc.dead.push_back({ st.sr[23] + len, st.sr[23] + 0x100 });
}
static struct FxTriGenInit
{
    FxTriGenInit() { gens.push_back({ "spec.f3dex.fxtri.", gen_fxtri }); }
} fxtri_gen_init;
