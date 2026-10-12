// Scenario generators for rt_test.cpp (included there). One per routine family; they fill DMEM / registers in `m` for a call of the
// routine at its entry and say which pcs the oracle must stop at. Layout constants are per ucode (found in the disassembly).
struct Layout
{
    unsigned vbuf;          // vertex buffer in DMEM, 32 entries of 0x28 bytes
    unsigned geom;          // geometry mode word (0x114)
    unsigned out;           // RDP output buffer start
    unsigned ret_main;      // main loop pc (return address of a lone triangle / vertex command)
    unsigned ret_tri2;      // return address after the first triangle of a TRI2
};
static const Layout banjo_layout = { 0x4a0, 0x114, 0xce0, 0x09c, 0x1ec };
static const Layout ts2_layout = { 0x4a0, 0x114, 0xce0, 0x09c, 0x1e4 };
static const Layout ts2dlx_layout = { 0x4a0, 0x114, 0xce0, 0x09c, 0x1dc };
static const Layout mk64_layout = { 0x4a0, 0x114, 0xce0, 0x09c, 0x22c };
static const Layout &layout_of(const Ucode &u)
{
    switch (u.id)
    {
    case 0x047252e810e35d56ull: return ts2_layout;
    case 0x34dad67335a54cdaull: return mk64_layout;
    case 0x7502cfb7098eeffbull: return ts2dlx_layout;
    default: return banjo_layout;           // 1.21 / 1.22
    }
}

static uint16_t rnd_edgy16()
{
    return (rnd() % 8) ? uint16_t(rnd()) : edge16();
}

static void gen_vertices(Machine &m, const Layout &L, unsigned flagmode)
{
    for (unsigned i = 0; i < 32; i++)
    {
        unsigned a = L.vbuf + 0x28 * i;
        for (unsigned o = 0; o < 40; o += 2)
            put16(m.dmem, a + o, rnd_edgy16());
        // screen x, y (10.2) mostly on screen
        auto scr = [&]() -> uint16_t {
            switch (rnd() % 8)
            {
            case 0: return rnd_edgy16();
            case 1: return uint16_t(int16_t(-4090 + int(rnd() % 8181)));
            default: return uint16_t(int16_t(int(rnd() % 1400) - 100));
            }
        };
        put16(m.dmem, a + 24, scr());
        put16(m.dmem, a + 26, scr());
        // clip flags: 0 mostly
        uint16_t fl = 0;
        if (flagmode == 1 && (rnd() % 8 == 0)) fl = uint16_t(rnd()) & 0x7777;
        else if (flagmode == 2) fl = uint16_t(rnd()) & 0x7777;
        put16(m.dmem, a + 36, fl);
    }
}

static void gen_tri(Machine &m, const Ucode &u, Scenario &sc, const Layout &L)
{
    (void)u;
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
    // geometry mode: low 3 bits are the RDP triangle flags; 0x200 smooth shading; 0x1000/0x2000 cull; the rest random
    uint32_t g = rnd() & 0xffffffffu;
    if (rnd() % 4) g &= ~0x3000u;
    if (rnd() % 3 == 0) g |= 4;
    put32(m.dmem, L.geom, g);
    put16(m.dmem, 0x112, rnd_edgy16());
    put8(m.dmem, 0x122, rnd());
    sc.exits.push_back(L.ret_main);
    sc.exits.push_back(L.ret_tri2);
    sc.imem_overlay = 0;
    sc.pre_entry = get16(m.dmem, 0x396) & 0xfff;     // the entry of the whole triangle code (TRI1 / TRI2 jump there)
}

static void gen_vtx(Machine &m, const Ucode &u, Scenario &sc, const Layout &L)
{
    (void)u;
    auto &st = m.cpu.get_state();
    unsigned v0 = rnd() % 32;
    unsigned n = 1 + rnd() % (32 - v0);
    if (rnd() % 4 == 0) n = 1 + rnd() % 8;
    // the input vertices (DMA'd by the ucode before the handler): pos, flag, st, rgba / normal
    for (unsigned i = 0; i < n; i++)
    {
        unsigned a = 0xae0 + 16 * i;
        for (unsigned o = 0; o < 16; o += 2)
            put16(m.dmem, a + o, rnd_edgy16());
        if (rnd() & 1)
            for (unsigned o = 0; o < 6; o += 2)
                put16(m.dmem, a + o, uint16_t(int16_t(int(rnd() % 4000) - 2000)));
    }
    // matrices, viewport, lights, texture scale
    for (unsigned o = 0; o < 64; o += 2)
        put16(m.dmem, 0x460 + o, (rnd() % 4) ? uint16_t(rnd() % 3 ? rnd() % 512 : rnd()) : rnd_edgy16());
    for (unsigned o = 0; o < 16; o += 2)
        put16(m.dmem, 0x380 + o, rnd_edgy16());
    for (unsigned o = 0x1a0; o < 0x2b0; o += 4)
        put32(m.dmem, o, rnd());
    unsigned nl = rnd() % 8;
    uint32_t lp = nl * 32;
    if (rnd() % 24 == 0) lp |= 0x80000000u;
    put32(m.dmem, 0x12c, lp);
    uint32_t g = rnd();
    if (rnd() % 3 == 0) g |= 0x20000; else if (rnd() % 2) g &= ~0x20000u;
    put32(m.dmem, L.geom, g);
    put16(m.dmem, 0x112, rnd_edgy16());
    put32(m.dmem, 0x124, rnd());
    // the command: w0 byte1 = v0 * 2, byte2 = n << 2 | high bits of the length
    st.sr[27] = 0x9a8;
    put8(m.dmem, 0x9a8 - 7, v0 * 2);
    put8(m.dmem, 0x9a8 - 6, (n << 2) | (rnd() & 3));
    st.sr[1] = v0 * 2;
    st.sr[22] = 0xae0;
    st.sr[29] = 0x110;
    st.sr[6] = rnd() & 15;
    sc.items = n;
    sc.exits.push_back(L.ret_main);
    sc.imem_overlay = (rnd() % 8 == 0) ? 1 : 2;           // lighting overlay mostly; the clipping one now and then
}
static void gen_vtx_any(Machine &m, const Ucode &u, Scenario &sc) { gen_vtx(m, u, sc, layout_of(u)); }
static void gen_tri_any(Machine &m, const Ucode &u, Scenario &sc) { gen_tri(m, u, sc, layout_of(u)); }

static struct GenInit
{
    GenInit() { gens.push_back({ "f3dex.tri", gen_tri_any });
        gens.push_back({ "f3dex.vtx", gen_vtx_any }); }
} gen_init;
