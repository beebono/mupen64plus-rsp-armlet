// scenarios for spec.f3dex.vtx.plain: the vertex handler without lighting (docs/ucode-f3dex.md)
static void gen_vtx_plain(Machine &m, const Ucode &u, Scenario &sc)
{
    auto &st = m.cpu.get_state();
    const bool mk = u.id == 0x34dad67335a54cdaull;
    const unsigned vtab = mk ? 0x31c : 0x31e;
    unsigned v0 = rnd() % 32;
    unsigned n = 1 + rnd() % (32 - v0);
    if (rnd() % 4 == 0) n = 1 + rnd() % 8;
    unsigned nin = n + 1;                       // the code reads one vertex more than the odd count
    for (unsigned i = 0; i < nin && i < 33; i++)
    {
        unsigned a = 0xae0 + 16 * i;
        for (unsigned o = 0; o < 16; o += 2)
            put16(m.dmem, a + o, (rnd() % 8) ? uint16_t(rnd()) : edge16());
        if (rnd() % 3)
            for (unsigned o = 0; o < 6; o += 2)
                put16(m.dmem, a + o, uint16_t(int16_t(int(rnd() % 4000) - 2000)));
    }
    for (unsigned o = 0; o < 64; o += 2)
        put16(m.dmem, 0x460 + o, (rnd() % 4) ? uint16_t(rnd() % 3 ? rnd() % 512 : rnd()) : (rnd() % 8 ? uint16_t(rnd()) : edge16()));
    if (rnd() % 2)                               // a more realistic projection: small integer parts, w from z
        for (unsigned o = 0; o < 32; o += 2)
            put16(m.dmem, 0x460 + o, uint16_t(int16_t(int(rnd() % 7) - 3)));
    for (unsigned o = 0; o < 16; o += 2)
        put16(m.dmem, 0x380 + o, (rnd() % 8) ? uint16_t(int16_t(int(rnd() % 1200) - 600)) : edge16());
    uint32_t g = rnd() & ~0x20000u;               // no lighting
    if (rnd() % 2) g |= 0x10000u; else g &= ~0x10000u;       // fog
    g |= 0x800000u;                               // F3DLX: clip flags on
    put32(m.dmem, 0x114, g);
    put16(m.dmem, 0x112, (rnd() % 4) ? uint16_t(0xffff - rnd() % 64) : uint16_t(rnd()));
    put32(m.dmem, 0x124, rnd());
    put16(m.dmem, 0x390, (rnd() % 4) ? uint16_t(rnd() % 600) : uint16_t(rnd()));
    put16(m.dmem, 0x392, (rnd() % 4) ? uint16_t(int16_t(int(rnd() % 400) - 200)) : uint16_t(rnd()));
    put16(m.dmem, 0x394, (rnd() % 4) ? 0xff : uint16_t(rnd()));
    st.sr[27] = 0x9a8;
    put8(m.dmem, 0x9a8 - 7, v0 * 2);
    put8(m.dmem, 0x9a8 - 6, (n << 2) | (rnd() & 3));
    st.sr[1] = v0 * 2;
    st.sr[22] = 0xae0;
    st.sr[29] = 0x110;
    for (int f = 0; f < 3; f++)
        for (int i = 0; i < 16; i++)
            st.cp2.flags[f].e[i] = 0;
    sc.exits.push_back(0x09c);
    sc.imem_overlay = 2;
    (void)vtab;
}
static struct VtxGenInit
{
    VtxGenInit() { gens.push_back({ "spec.f3dex.vtx.", gen_vtx_plain }); }
} vtx_gen_init;
