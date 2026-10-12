#ifndef TRI_GEN_INC
#define TRI_GEN_INC
// scenarios for spec.f3dex.tri.*: a triangle command's three vertex addresses in $1..$3 and the DMEM state around them
static unsigned tri_gen_mask = 7;               // geometry mode low bits left random (0: header only; 4 shade; 2 texture; 1 z buffer)
static void gen_tri_spec(Machine &m, const Ucode &u, Scenario &sc)
{
    const Layout &L = layout_of(u);
    auto &st = m.cpu.get_state();
    gen_vertices(m, L, rnd() % 3);
    auto vaddr = [&]() { return L.vbuf + 0x28 * (rnd() % 32); };
    st.sr[1] = vaddr();
    st.sr[2] = (rnd() % 16) ? vaddr() : st.sr[1];
    st.sr[3] = (rnd() % 16) ? vaddr() : st.sr[2];
    st.sr[4] = st.sr[1];
    st.sr[29] = 0x110;
    st.sr[23] = L.out + 8 * (rnd() % 48);
    st.sr[24] = (rnd() & 1) ? L.ret_main : L.ret_tri2;
    uint32_t g = rnd();
    if (rnd() % 4) g &= ~0x3000u;
    g = (g & ~7u) | (rnd() & tri_gen_mask);
    put32(m.dmem, L.geom, g);
    put16(m.dmem, 0x112, rnd() % 4 ? 0xffff : uint16_t(rnd()));
    put8(m.dmem, 0x122, rnd());
    sc.exits.push_back(L.ret_main);
    sc.exits.push_back(L.ret_tri2);
    sc.imem_overlay = 0;
    for (int f = 0; f < 3; f++)                   // VCO / VCC / VCE are clear between commands (the code subtracts the VCO carry)
        for (int i = 0; i < 16; i++)
            st.cp2.flags[f].e[i] = 0;
    sc.cmp_sr = 1u << 23;
    sc.dead.push_back({ 0xbe0, 0xc40 });          // scratch of the triangle code (sorted vertex addresses, coefficient temporaries)
}
static struct TriSpecGenInit
{
    TriSpecGenInit() { gens.push_back({ "spec.f3dex.tri.", gen_tri_spec }); }
} tri_spec_gen_init;

#endif
