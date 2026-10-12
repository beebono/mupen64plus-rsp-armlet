// scenario generators of the "state" spec (state_spec.c): the entry state the display-list loop's dispatch leaves for the small state commands.
// Common: $29 = 0x110, $27 = pointer after the command in the display-list buffer (0x9a0..0xae0, 8-byte steps) with w0 / w1 stored at -8 / -4($27)
// and in $25 / $24; $23 = RDP output pointer (0xce0 + 8k); class 0 (movemem): $22 = 0xae0, $1 = w0 bits 23..16, the DMA'd data at 0xae0.
static unsigned st_u32edge()
{
    switch (rnd() % 8)
    {
    case 0: return 0;
    case 1: return 0xffffffffu;
    case 2: return 0x80000000u;
    case 3: return 1u << (rnd() & 31);
    default: return rnd();
    }
}

static void st_cmd(Machine &m, uint32_t w0, uint32_t w1)
{
    auto &st = m.cpu.get_state();
    unsigned p = 0x9a8 + 8 * (rnd() % 39);
    put32(m.dmem, p - 8, w0);
    put32(m.dmem, p - 4, w1);
    st.sr[25] = w0;
    st.sr[24] = w1;
    st.sr[27] = p;
    st.sr[29] = 0x110;
}

static void st_rand_dmem(Machine &m, unsigned lo, unsigned hi)
{
    for (unsigned a = lo; a < hi; a += 4)
        put32(m.dmem, a, rnd());
}

static void gen_state_geom(Machine &m, const Ucode &u, Scenario &sc)
{
    (void)u; (void)sc;
    put32(m.dmem, 0x114, st_u32edge());
    st_cmd(m, 0xb7000000u | (rnd() & 0xffffff), st_u32edge());
}

static void gen_state_texture(Machine &m, const Ucode &u, Scenario &sc)
{
    (void)u; (void)sc;
    put32(m.dmem, 0x114, rnd());
    put32(m.dmem, 0x120, rnd());
    put32(m.dmem, 0x124, rnd());
    uint32_t w0;
    if (rnd() % 4)
        w0 = 0xbb000000u | (rnd() % 8 << 11) | (rnd() % 8 << 8) | (rnd() % 2) | (rnd() % 2 ? 0 : 0x00ff0000u & rnd());
    else
        w0 = rnd();
    st_cmd(m, w0, rnd() % 4 ? (rnd() & 0xffffu) << 16 | (rnd() & 0xffffu) : st_u32edge());
}

static void gen_state_moveword(Machine &m, const Ucode &u, Scenario &sc)
{
    (void)u; (void)sc;
    st_rand_dmem(m, 0x800, 0x1000);
    static const uint8_t idxs[] = { 0, 2, 4, 6, 8, 10, 12, 14 };
    unsigned idx = (rnd() % 8) ? idxs[rnd() % 8] : (rnd() % 2 ? rnd() & 0xff : (rnd() & 0xfe));
    unsigned off;
    switch (rnd() % 6)
    {
    case 0: off = 0; break;
    case 1: off = 4 * (rnd() % 16); break;               // segment numbers
    case 2: off = 4 + 8 * (rnd() % 4); break;            // clip ratio words
    case 3: off = 4 * (rnd() % 8); break;
    default: off = (rnd() % 3) ? (rnd() & 0xff) : (rnd() & 0xffff); break;
    }
    st_cmd(m, 0xbc000000u | off << 8 | idx, st_u32edge());
}

static void gen_state_som(Machine &m, const Ucode &u, Scenario &sc)
{
    (void)u; (void)sc;
    auto &st = m.cpu.get_state();
    put32(m.dmem, 0x118, rnd() % 4 ? 0xef000000u | (rnd() & 0xffffff) : rnd());
    put32(m.dmem, 0x11c, rnd());
    unsigned sh = rnd() % 4 ? rnd() % 32 : rnd() & 0xff;
    unsigned len = rnd() % 4 ? 1 + rnd() % 8 : (rnd() % 3 ? rnd() % 33 : rnd() & 0xff);
    st_cmd(m, (rnd() % 2 ? 0xb9000000u : 0xba000000u) | sh << 8 | len, rnd() % 3 ? (rnd() & ((1u << (len & 31)) - 1)) << (sh & 31) : st_u32edge());
    st.sr[23] = 0xce0 + 8 * (rnd() % 60);
}

static void gen_state_modvtx(Machine &m, const Ucode &u, Scenario &sc)
{
    (void)u; (void)sc;
    st_rand_dmem(m, 0x800, 0x1000);
    unsigned vtx = rnd() % 8 ? rnd() % 32 : (rnd() % 3 ? rnd() & 0xff : rnd() & 0x7ff);
    unsigned where = rnd() % 2 ? 0x10 + 4 * (rnd() % 4) : (rnd() & 0xff);
    st_cmd(m, 0xb2000000u | where << 16 | (vtx * 2 & 0xffff), st_u32edge());
}

static void gen_state_perspnorm(Machine &m, const Ucode &u, Scenario &sc)
{
    (void)u; (void)sc;
    put32(m.dmem, 0x110, rnd());
    st_cmd(m, 0xb4000000u | (rnd() & 0xffffff), st_u32edge());
}

static void gen_state_rdphalf(Machine &m, const Ucode &u, Scenario &sc)
{
    (void)u; (void)sc;
    auto &st = m.cpu.get_state();
    put32(m.dmem, 0x10c, rnd());
    st_cmd(m, 0xb3000000u | (rnd() & 0xffffff), st_u32edge());
    st.sr[23] = 0xce0 + 8 * (rnd() % 60);
}

static void gen_state_movemem(Machine &m, const Ucode &u, Scenario &sc)
{
    (void)u; (void)sc;
    auto &st = m.cpu.get_state();
    static const uint8_t idxs[] = { 0x80, 0x82, 0x84, 0x86, 0x88, 0x8a, 0x8c, 0x8e, 0x90, 0x92, 0x94, 0x96, 0x98, 0x9a, 0x9c, 0x9e };
    unsigned idx = (rnd() % 8) ? idxs[rnd() % 16] : (rnd() % 2 ? rnd() & 0xff : rnd() & 0xfe);
    uint32_t w0 = 0x03000000u | idx << 16 | (rnd() % 2 ? 16 : rnd() & 0x3ff);
    st_cmd(m, w0, rnd());
    st_rand_dmem(m, 0xae0, 0xb20);
    st.sr[1] = idx;
    st.sr[22] = 0xae0;
}

static void gen_state_segment(Machine &m, const Ucode &u, Scenario &sc)
{
    (void)u;
    auto &st = m.cpu.get_state();
    st_rand_dmem(m, 0x160, 0x1a0);
    st.sr[19] = rnd();
    st.sr[31] = 0x09c;
    sc.exits.push_back(0x09c);
    sc.cmp_sr = (1u << 19) | (1u << 12);
}

static struct StateGenInit
{
    StateGenInit()
    {
        gens.push_back({ "spec.f3dex.state.setgeom", gen_state_geom });
        gens.push_back({ "spec.f3dex.state.cleargeom", gen_state_geom });
        gens.push_back({ "spec.f3dex.state.texture", gen_state_texture });
        gens.push_back({ "spec.f3dex.state.moveword", gen_state_moveword });
        gens.push_back({ "spec.f3dex.state.som", gen_state_som });
        gens.push_back({ "spec.f3dex.state.modvtx", gen_state_modvtx });
        gens.push_back({ "spec.f3dex.state.perspnorm", gen_state_perspnorm });
        gens.push_back({ "spec.f3dex.state.rdphalf", gen_state_rdphalf });
        gens.push_back({ "spec.f3dex.state.movemem", gen_state_movemem });
        gens.push_back({ "spec.f3dex.state.segment", gen_state_segment });
    }
} state_gen_init;
