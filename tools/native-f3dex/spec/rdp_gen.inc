// scenario generators of rdp_spec.c (the RDP output side: pass-through, RDPHALF, SETOTHERMODE, flush, task-start FIFO setup)
// Per-build numbers (flush entry F, whether the build is F3DLX) and the DMEM variables: 0x10c RDPHALF_1 word, 0x118/0x11c cached SET_OTHER_MODES words,
// 0x128 DRAM output pointer, 0x150/0x154 FIFO start/end, 0x160 segment table, 0xce0 output buffer.
struct RdpP
{
    unsigned F;
    bool dlx;
};
static RdpP rdp_p(const Ucode &u)
{
    switch (u.id)
    {
    case 0x208a567e9aead5eaull: return { 0x128, false };   // F3DEX 1.21
    case 0x047252e810e35d56ull: return { 0x120, false };   // F3DEX 1.23
    case 0x7502cfb7098eeffbull: return { 0x120, true };    // F3DLX 1.23
    case 0x34dad67335a54cdaull: return { 0x168, false };   // F3DEX 0.95
    case 0x5f33f7c2be924de0ull: return { 0x168, true };    // F3DLX 0.95
    default: return { 0x128, false };                      // 1.22 NoN: same layout as 1.21
    }
}

static uint32_t rdp_edge32()
{
    static const uint32_t e[] = { 0, 1, 7, 8, 0x7ffffff8u, 0x7fffffffu, 0x80000000u, 0xfffffff8u, 0xffffffffu, 0x00800000u, 0x007ffff8u, 0x01000000u, 0xff000000u };
    return e[rnd() % (sizeof(e) / sizeof(e[0]))];
}
static uint32_t rdp_w32()
{
    return (rnd() % 4) ? uint32_t(rnd()) : rdp_edge32();
}
static uint32_t rdp_bufptr()
{
    // $23: a multiple of 8 in the output buffer (it is 0xce0 at every command boundary in real use)
    return (rnd() % 3) ? 0xce0 : 0xce0 + 8 * (rnd() % 40);
}

static void rdp_common(Machine &m, Scenario &sc)
{
    auto &st = m.cpu.get_state();
    st.sr[29] = 0x110;
    st.sr[31] = 0x09c;
    sc.imem_overlay = 0;
    // a random segment table and the saved RDPHALF_1 word
    for (unsigned i = 0; i < 16; i++)
        put32(m.dmem, 0x160 + 4 * i, rdp_w32());
    put32(m.dmem, 0x10c, rnd());
    put32(m.dmem, 0x128, rdp_w32() & 0xfffffff8u);
}

static void gen_rdp_pt(Machine &m, const Ucode &u, Scenario &sc)
{
    (void)u;
    auto &st = m.cpu.get_state();
    rdp_common(m, sc);
    unsigned op;
    switch (rnd() % 6)
    {
    case 0: case 1: op = 0xfd + rnd() % 3; break;               // SETTIMG / SETZIMG / SETCIMG
    case 2: op = 0xe4 + rnd() % 2; break;                       // TEXRECT, TEXRECTFLIP
    case 3: op = 0xfc; break;
    default: op = 0xc0 + rnd() % 0x40; break;
    }
    uint32_t w0 = (op << 24) | (rnd() & 0xffffff), w1 = rdp_w32();
    st.sr[25] = w0;
    st.sr[24] = w1;
    st.sr[23] = rdp_bufptr();
}

static void gen_rdp_half1(Machine &m, const Ucode &u, Scenario &sc)
{
    (void)u;
    rdp_common(m, sc);
    m.cpu.get_state().sr[24] = rdp_w32();
    m.cpu.get_state().sr[23] = rdp_bufptr();
}

static void gen_rdp_half2(Machine &m, const Ucode &u, Scenario &sc)
{
    (void)u;
    auto &st = m.cpu.get_state();
    rdp_common(m, sc);
    st.sr[24] = rdp_w32();
    st.sr[23] = rdp_bufptr();
}

static void gen_rdp_om(Machine &m, const Ucode &u, Scenario &sc)
{
    (void)u;
    auto &st = m.cpu.get_state();
    rdp_common(m, sc);
    // the command is in the display-list buffer: w0 = op | shift << 8 | length, w1 = data; $27 points after it
    unsigned shift, len;
    switch (rnd() % 5)
    {
    case 0: shift = rnd() & 0xff; len = rnd() & 0xff; break;
    case 1: shift = 0; len = 32; break;
    case 2: shift = rnd() % 32; len = 1 + rnd() % (32 - shift); break;
    default: shift = rnd() % 32; len = 1 + rnd() % 8; break;
    }
    uint32_t w0 = 0xb9000000u | (shift << 8) | len, w1 = rdp_w32();
    if (rnd() & 1)
        w1 &= ((len >= 32 ? 0xffffffffu : ((1u << len) - 1)) << (shift & 31));       // data inside the field, as the game builds it
    unsigned p = 0x9a8 + 8 * (rnd() % 30);
    put32(m.dmem, p - 8, w0);
    put32(m.dmem, p - 4, w1);
    st.sr[27] = p;
    st.sr[25] = w0;
    st.sr[24] = w1;
    st.sr[23] = rdp_bufptr();
    put32(m.dmem, 0x118, rdp_w32());
    put32(m.dmem, 0x11c, rdp_w32());
}

static void gen_rdp_post(Machine &m, const Ucode &u, Scenario &sc)
{
    (void)u;
    rdp_common(m, sc);
    auto &st = m.cpu.get_state();
    uint32_t v;
    switch (rnd() % 6)
    {
    case 0: v = 0; break;
    case 1: v = 1; break;
    case 2: v = 0xffffffffu; break;
    case 3: v = 0x80000000u; break;
    case 4: v = 0x7fffffffu; break;
    default: v = rnd(); break;
    }
    st.sr[2] = v;
    sc.exits.push_back(0x09c);
    sc.exits.push_back(0x0ac);
}

// state at the flush entry: $23 = buffer pointer, DMEM 0x128 = DRAM pointer, 0x150 / 0x154 = FIFO start / end
static void rdp_flush_state(Machine &m, const Ucode &u, Scenario &sc, int mode)
{
    (void)u;
    auto &st = m.cpu.get_state();
    rdp_common(m, sc);
    int32_t len;
    if (rnd() % 8 == 0)
        len = (rnd() % 2) ? 0 : -8 * int(1 + rnd() % 4);
    else
        len = 8 * (1 + rnd() % 22);
    st.sr[23] = uint32_t(0xce0 + len);
    uint32_t ptr = (rnd() & 1) ? (rnd() & 0x7ffff8u) : (rdp_w32() & 0xfffffff8u);
    uint32_t newend = ptr + uint32_t(len);
    uint32_t fend;
    switch (rnd() % 6)
    {
    case 0: fend = newend; break;
    case 1: fend = newend - 8; break;
    case 2: fend = newend + 8; break;
    case 3: fend = ptr + 8 * (rnd() % 64); break;
    case 4: fend = rdp_w32(); break;
    default: fend = newend + 8 * (rnd() % 4096); break;
    }
    if (mode == 1)      // wrap or empty only: the FIFO end is below the new end
    {
        if (len > 0 && int32_t(fend - newend) >= 0)
            fend = newend - 8 * (1 + rnd() % 64);
    }
    put32(m.dmem, 0x128, ptr);
    put32(m.dmem, 0x154, fend);
    put32(m.dmem, 0x150, (rnd() & 1) ? (rnd() & 0x7ffff8u) : rdp_w32());
}

// a whole command with the flush: the FIFO is full, so the flush takes the wrap path (the other path polls DPC_CURRENT, which the harness cannot arrange)
static void rdp_full_state(Machine &m, Scenario &sc)
{
    auto &st = m.cpu.get_state();
    uint32_t ptr = (rnd() & 1) ? (rnd() & 0x7ffff8u) : (rdp_w32() & 0xfffffff8u);
    put32(m.dmem, 0x128, ptr);
    put32(m.dmem, 0x154, ptr - 8 * (rnd() % 16));       // below ptr + 8 (signed difference)
    put32(m.dmem, 0x150, (rnd() & 1) ? (rnd() & 0x7ffff8u) : rdp_w32());
    st.sr[23] = 0xce0;
    sc.exits.push_back(0x09c);
    sc.exits.push_back(0x0ac);
}
static void gen_rdp_full_pt(Machine &m, const Ucode &u, Scenario &sc) { gen_rdp_pt(m, u, sc); rdp_full_state(m, sc); }
static void gen_rdp_full_h2(Machine &m, const Ucode &u, Scenario &sc) { gen_rdp_half2(m, u, sc); rdp_full_state(m, sc); m.cpu.get_state().sr[2] = 0x1350; }
static void gen_rdp_full_om(Machine &m, const Ucode &u, Scenario &sc) { gen_rdp_om(m, u, sc); rdp_full_state(m, sc); }

static void gen_rdp_fdec(Machine &m, const Ucode &u, Scenario &sc)
{
    rdp_flush_state(m, u, sc, 0);
    RdpP p = rdp_p(u);
    sc.exits.push_back(p.F + 0x24);
    sc.exits.push_back(p.F + 0x40);
    sc.exits.push_back(p.F + (p.dlx ? 0x74 : 0x7c));
}

static void gen_rdp_ffull(Machine &m, const Ucode &u, Scenario &sc)
{
    rdp_flush_state(m, u, sc, 1);
    sc.exits.push_back(0x09c);
}

static void gen_rdp_ftail(Machine &m, const Ucode &u, Scenario &sc)
{
    (void)u;
    auto &st = m.cpu.get_state();
    rdp_common(m, sc);
    st.sr[18] = (rnd() % 6) ? 8 * (1 + rnd() % 22) : rdp_w32();
    st.sr[19] = (rnd() & 1) ? (rnd() & 0x7ffff8u) : rdp_w32();
    st.sr[23] = rnd();
    st.sr[21] = 0x09c;
}

static void gen_rdp_fnow(Machine &m, const Ucode &u, Scenario &sc)
{
    auto &st = m.cpu.get_state();
    rdp_common(m, sc);
    st.sr[18] = (rnd() % 6) ? 8 * (1 + rnd() % 22) : rdp_w32();
    st.sr[19] = (rnd() & 1) ? (rnd() & 0x7ffff8u) : rdp_w32();
    st.sr[21] = 0x09c;
    sc.pre_entry = rdp_p(u).F + 0x3c;       // DPC_START = $19: DPC_CURRENT = DPC_END = $19 & ~7, as a synchronous RDP leaves them
}

static void gen_rdp_dmaw(Machine &m, const Ucode &u, Scenario &sc)
{
    (void)u;
    auto &st = m.cpu.get_state();
    rdp_common(m, sc);
    st.sr[17] = 1 + (rnd() % 100);
    st.sr[18] = rdp_w32();
    st.sr[19] = rdp_w32();
    st.sr[20] = rdp_w32();
    sc.exits.push_back(0x09c);
}

static void gen_rdp_tri(Machine &m, const Ucode &u, Scenario &sc)
{
    gen_tri_any(m, u, sc);          // the triangle generator of rt_gen.inc: the JIT runs from the triangle code to the flush (this routine's entry)
    auto &st = m.cpu.get_state();
    st.sr[23] = 0xce0;              // the buffer is empty at every command boundary in real use; the length check needs the start
    put32(m.dmem, 0x128, rdp_w32() & 0xfffffff8u);
    put32(m.dmem, 0x154, rdp_w32());
    put32(m.dmem, 0x150, rdp_w32());
    RdpP p = rdp_p(u);
    sc.exits.push_back(p.F + 0x24);
    sc.exits.push_back(p.F + 0x40);
    sc.exits.push_back(p.F + (p.dlx ? 0x74 : 0x7c));
}

static void gen_rdp_init(Machine &m, const Ucode &u, Scenario &sc)
{
    (void)u;
    auto &st = m.cpu.get_state();
    sc.imem_overlay = 0;
    st.sr[1] = 0xfc0;
    st.sr[29] = 0x110;
    st.sr[23] = rnd();
    // OSTask: output_buff (0xfe8) and output_buff_size (0xfec: the end address of the FIFO)
    uint32_t fs = (rnd() & 0x7ffff8u), fe = fs + 8 * (1 + rnd() % 8192);
    if (rnd() % 8 == 0) fe = rdp_w32();
    put32(m.dmem, 0xfe8, fs);
    put32(m.dmem, 0xfec, fe);
    // the DPC registers are random in the harness: DPC_END above or below the FIFO start decides the path; make both likely
    // (register values come from the harness: nothing to set here)
}

static struct RdpGenInit
{
    RdpGenInit()
    {
        gens.push_back({ "spec.f3dex.rdp.pt.", gen_rdp_pt });
        gens.push_back({ "spec.f3dex.rdp.half1.", gen_rdp_half1 });
        gens.push_back({ "spec.f3dex.rdp.half2.", gen_rdp_half2 });
        gens.push_back({ "spec.f3dex.rdp.omode.", gen_rdp_om });
        gens.push_back({ "spec.f3dex.rdp.post.", gen_rdp_post });
        gens.push_back({ "spec.f3dex.rdp.fdec.", gen_rdp_fdec });
        gens.push_back({ "spec.f3dex.rdp.ffull.", gen_rdp_ffull });
        gens.push_back({ "spec.f3dex.rdp.ftail.", gen_rdp_ftail });
        gens.push_back({ "spec.f3dex.rdp.fnow.", gen_rdp_fnow });
        gens.push_back({ "spec.f3dex.rdp.full_pt.", gen_rdp_full_pt });
        gens.push_back({ "spec.f3dex.rdp.full_h2.", gen_rdp_full_h2 });
        gens.push_back({ "spec.f3dex.rdp.full_om.", gen_rdp_full_om });
        gens.push_back({ "spec.f3dex.rdp.dmaw.", gen_rdp_dmaw });
        gens.push_back({ "spec.f3dex.rdp.tri.", gen_rdp_tri });
        gens.push_back({ "spec.f3dex.rdp.init.", gen_rdp_init });
    }
} rdp_gen_init;
