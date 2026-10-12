// scenario generators of the dl spec (dl_spec.c): states the display-list engine can really be in at the entry of each handler (see rt_gen.inc for the helpers)
struct DlB
{
    unsigned depth;         // DMEM byte holding the return-stack depth (4 per entry)
    unsigned taken;         // G_BRANCH_Z: the pc of the taken branch (the translation of the RDPHALF_1 word), 0 = no G_BRANCH_Z
    bool mk;                // F3DEX 0.95 / F3DLX 0.95
};
static const DlB dl_banjo = { 0x148, 0x708, false };
static const DlB dl_non = { 0x148, 0x728, false };
static const DlB dl_ts2 = { 0x148, 0x700, false };
static const DlB dl_dlx = { 0x148, 0x72c, false };
static const DlB dl_mk = { 0x110, 0, true };
static const DlB &dl_of(const Ucode &u)
{
    switch (u.id)
    {
    case 0xfd5a31ccf6d40b62ull: return dl_non;
    case 0x047252e810e35d56ull: return dl_ts2;
    case 0x7502cfb7098eeffbull: return dl_dlx;
    case 0x34dad67335a54cdaull:
    case 0x5f33f7c2be924de0ull: return dl_mk;
    default: return dl_banjo;
    }
}

static uint32_t dl_rnd32()
{
    switch (rnd() % 6)
    {
    case 0: return 0;
    case 1: return rnd() & 0x7ffff8;
    case 2: return 0x80000000u | (rnd() & 0x7ffff8);
    case 3: return rnd() & 0xff000000u;
    default: return rnd();
    }
}

// 16 segment base words
static void dl_segtab(Machine &m)
{
    for (unsigned i = 0; i < 16; i++)
        put32(m.dmem, 0x160 + 4 * i, dl_rnd32());
}

// a depth byte: a multiple of 4 in the range the microcode keeps it in, now and then outside it (full stack, signed byte)
static uint8_t dl_depth()
{
    switch (rnd() % 10)
    {
    case 0: return 72;
    case 1: return 0x7c;
    case 2: return 0xfc;
    case 3: return 0xe0;
    case 4: return 0x80;
    default: return uint8_t(4 * (rnd() % 18));
    }
}

static void dl_stackfill(Machine &m)
{
    for (unsigned a = 0x380; a < 0x420; a += 4)
        put32(m.dmem, a, rnd());
}

// the buffer state at a command: $27 after the command, $28 the end of the valid buffer (0xae0), or $27 itself when a G_DL has just forced a refill
static void dl_buf(Machine &m)
{
    auto &st = m.cpu.get_state();
    st.sr[27] = 0x9a8 + 8 * (rnd() % 0x29);
    st.sr[28] = (rnd() % 8 == 0) ? st.sr[27] : 0xae0;
    st.sr[26] = dl_rnd32() & ~7u;
    if (rnd() % 4 == 0) st.sr[26] = rnd();
}

static void gen_dl_call(Machine &m, const Ucode &u, Scenario &sc)
{
    auto &st = m.cpu.get_state();
    const DlB &B = dl_of(u);
    dl_segtab(m);
    dl_stackfill(m);
    dl_buf(m);
    put8(m.dmem, B.depth, dl_depth());
    st.sr[1] = (rnd() % 3 == 0) ? 1 + rnd() % 255 : 0;
    st.sr[24] = (rnd() % 4 == 0) ? rnd() : ((rnd() & 0x0f000000u) | (rnd() & 0xffffff));
    st.sr[29] = 0x110;
    sc.exits.push_back(0x09c);
    sc.imem_overlay = 0;
    sc.cmp_sr = 0xfffffffeu;                // every GPR is compared as well (the stretches are short enough to model all their temporaries)
}

static void gen_dl_enddl(Machine &m, const Ucode &u, Scenario &sc)
{
    auto &st = m.cpu.get_state();
    const DlB &B = dl_of(u);
    dl_stackfill(m);
    dl_buf(m);
    put8(m.dmem, B.depth, dl_depth() & ((rnd() % 5) ? 0xff : 0x00));
    st.sr[29] = 0x110;
    sc.exits.push_back(0x09c);
    sc.exits.push_back(0x0bc);
    sc.imem_overlay = 0;
    sc.cmp_sr = 0xfffffffeu;                // every GPR is compared as well (the stretches are short enough to model all their temporaries)
}

static void gen_dl_branchz(Machine &m, const Ucode &u, Scenario &sc)
{
    auto &st = m.cpu.get_state();
    const DlB &B = dl_of(u);
    dl_segtab(m);
    dl_buf(m);
    put32(m.dmem, 0x10c, dl_rnd32());
    for (unsigned i = 0; i < 32; i++)
    {
        uint32_t z;
        switch (rnd() % 6)
        {
        case 0: z = 0x7fffffffu; break;
        case 1: z = 0x80000000u; break;
        case 2: z = rnd() & 0x03ff0000u; break;
        default: z = rnd();
        }
        put32(m.dmem, 0x4a0 + 40 * i + 28, z);
    }
    unsigned v = rnd() % 32;
    uint32_t z = m.dmem[(0x4a0 + 40 * v + 28) >> 2];
    uint32_t w1;
    switch (rnd() % 8)
    {
    case 0: w1 = z; break;
    case 1: w1 = z + 1; break;
    case 2: w1 = z - 1; break;
    case 3: w1 = 0x80000000u; break;
    case 4: w1 = 0x7fffffffu; break;
    case 5: w1 = z + (rnd() & 0xffff) - 0x8000; break;
    default: w1 = rnd();
    }
    st.sr[25] = 0xb0000000u | ((v * 5) << 12) | (v * 2);
    if (rnd() % 8 == 0) st.sr[25] ^= (rnd() & 0xfff000u);          // the bits 12..23 are not looked at
    st.sr[24] = w1;
    st.sr[29] = 0x110;
    sc.exits.push_back(0x09c);
    sc.exits.push_back(B.taken);
    sc.imem_overlay = 0;
    sc.cmp_sr = 0xfffffffeu;                // every GPR is compared as well (the stretches are short enough to model all their temporaries)
}

static void gen_dl_culldl(Machine &m, const Ucode &u, Scenario &sc)
{
    auto &st = m.cpu.get_state();
    const DlB &B = dl_of(u);
    dl_stackfill(m);
    dl_buf(m);
    put8(m.dmem, B.depth, dl_depth() & ((rnd() % 5) ? 0xff : 0x00));
    unsigned i = rnd() % 32, j = i + rnd() % (32 - i);
    if (rnd() % 3 == 0)
    {
        j = i + rnd() % 3;
        if (j > 31) j = 31;
    }
    if (B.mk)
    {
        i = rnd() % 26;                     // direct byte offsets: 40 * i must stay below 1024
        j = i + 1 + rnd() % (32 - i);
    }
    uint32_t common = rnd() & 0xf7f7u;
    unsigned mode = rnd() % 4;
    for (unsigned k = 0; k < 32; k++)
    {
        uint16_t f;
        if (mode == 0) f = uint16_t(common | (rnd() & 0xffff));
        else if (mode == 1) f = uint16_t(rnd() & 0xffff & (rnd() | rnd()));
        else if (mode == 2) f = uint16_t((rnd() & 0x7070) | 0x7070 * (rnd() % 3 != 0));
        else f = uint16_t(rnd());
        put16(m.dmem, 0x4a0 + 40 * k + 36, f);
    }
    if (B.mk)
    {
        st.sr[25] = 0xbe000000u | (40 * i) | ((rnd() % 4 == 0) ? (rnd() & 0xfffc00u) : 0);
        st.sr[24] = 40 * j;
    }
    else
    {
        st.sr[25] = 0xbe000000u | (i * 2) | ((rnd() % 4 == 0) ? (rnd() & 0xfff000u) : 0);
        st.sr[24] = (rnd() % 4 == 0) ? (j * 2) | (rnd() & 0xfffff000u) : j * 2;
    }
    st.sr[29] = 0x110;
    sc.exits.push_back(0x09c);
    sc.exits.push_back(0x0bc);
    sc.imem_overlay = 0;
    sc.cmp_sr = 0xfffffffeu;                // every GPR is compared as well (the stretches are short enough to model all their temporaries)
}

static void dl_rdp_regs(Machine &m, const Ucode &u, Scenario &sc, bool half2)
{
    (void)u;
    auto &st = m.cpu.get_state();
    dl_segtab(m);
    uint32_t op;
    switch (rnd() % 8)
    {
    case 0: op = 0xfd + rnd() % 3; break;               // SETTIMG / SETZIMG / SETCIMG
    case 1: op = 0xa4 + rnd() % 0x1c; break;            // the aliases that reach this code
    case 2: op = rnd() % 256; break;
    default: op = 0xc0 + rnd() % 0x40;
    }
    st.sr[25] = (op << 24) | (rnd() & 0xffffff);
    st.sr[24] = (rnd() % 3) ? dl_rnd32() | (rnd() & 0x0f000000u) : rnd();
    st.sr[23] = 0xce0 + 8 * (rnd() % 0x30);
    for (unsigned a = 0xce0; a < 0xe80; a += 4) put32(m.dmem, a, rnd());
    put32(m.dmem, 0x10c, rnd());
    put32(m.dmem, 0x128, dl_rnd32() & ~7u);
    st.sr[29] = 0x110;
    (void)half2;
    sc.imem_overlay = 0;
    sc.cmp_sr = 0xfffffffeu;                // every GPR is compared as well (the stretches are short enough to model all their temporaries)
}
static void gen_dl_rdpcopy(Machine &m, const Ucode &u, Scenario &sc) { dl_rdp_regs(m, u, sc, false); }
static void gen_dl_rdphalf2(Machine &m, const Ucode &u, Scenario &sc) { dl_rdp_regs(m, u, sc, true); }

static void gen_dl_rdphalf1(Machine &m, const Ucode &u, Scenario &sc)
{
    (void)u;
    auto &st = m.cpu.get_state();
    put32(m.dmem, 0x10c, rnd());
    st.sr[24] = rnd();
    st.sr[29] = 0x110;
    sc.imem_overlay = 0;
    sc.cmp_sr = 0xfffffffeu;                // every GPR is compared as well (the stretches are short enough to model all their temporaries)
}

static void gen_dl_refilldec(Machine &m, const Ucode &u, Scenario &sc)
{
    (void)u;
    auto &st = m.cpu.get_state();
    st.sr[27] = 0x9a8 + 8 * (rnd() % 0x29);
    st.sr[28] = (rnd() % 3 == 0) ? st.sr[27] : 0xae0;
    for (unsigned a = 0x9a0; a < 0xae0; a += 4) put32(m.dmem, a, rnd());
    put16(m.dmem, 0x104, 0x1058);
    st.sr[29] = 0x110;
    sc.exits.push_back(0x064);
    sc.exits.push_back(0x0c8);
    sc.imem_overlay = 0;
    sc.cmp_sr = 0xfffffffeu;                // every GPR is compared as well (the stretches are short enough to model all their temporaries)
}

static void gen_dl_reg_refill(Machine &m, const Ucode &u, Scenario &sc)
{
    (void)u;
    auto &st = m.cpu.get_state();
    st.sr[26] = dl_rnd32();
    st.sr[31] = (rnd() % 2) ? 0x058 : (rnd() & 0xffc);
    st.sr[29] = 0x110;
    sc.imem_overlay = 0;
    sc.cmp_sr = 0xfffffffeu;                // every GPR is compared as well (the stretches are short enough to model all their temporaries)
}

static void gen_dl_reg_xlate(Machine &m, const Ucode &u, Scenario &sc)
{
    (void)u;
    auto &st = m.cpu.get_state();
    dl_segtab(m);
    st.sr[19] = (rnd() % 3) ? dl_rnd32() | (rnd() & 0xff000000u) : rnd();
    st.sr[31] = 0x09c;
    st.sr[29] = 0x110;
    sc.imem_overlay = 0;
    sc.cmp_sr = 0xfffffffeu;                // every GPR is compared as well (the stretches are short enough to model all their temporaries)
}

static void gen_dl_reg_head(Machine &m, const Ucode &u, Scenario &sc)
{
    (void)u;
    auto &st = m.cpu.get_state();
    dl_segtab(m);
    // first command of a window: a random class (0..3), opcode, lengths
    unsigned cls = rnd() % 4;
    uint32_t w0 = (cls << 30) | (rnd() & 0x3fffffffu), w1 = (rnd() % 3) ? dl_rnd32() | (rnd() & 0x0f000000u) : rnd();
    put32(m.dmem, 0x9a0, w0);
    put32(m.dmem, 0x9a4, w1);
    for (unsigned a = 0x9a8; a < 0xae0; a += 4) put32(m.dmem, a, rnd());
    st.sr[29] = 0x110;
    sc.exits.push_back(0x094);
    sc.imem_overlay = 0;
    sc.cmp_sr = 0xfffffffeu;                // every GPR is compared as well (the stretches are short enough to model all their temporaries)
}

static struct DlGenInit
{
    DlGenInit()
    {
        gens.push_back({ "spec.f3dex.dl.call", gen_dl_call });
        gens.push_back({ "spec.f3dex.dl.enddl", gen_dl_enddl });
        gens.push_back({ "spec.f3dex.dl.branchz", gen_dl_branchz });
        gens.push_back({ "spec.f3dex.dl.culldl", gen_dl_culldl });
        gens.push_back({ "spec.f3dex.dl.rdpcopy", gen_dl_rdpcopy });
        gens.push_back({ "spec.f3dex.dl.rdphalf2", gen_dl_rdphalf2 });
        gens.push_back({ "spec.f3dex.dl.rdphalf1", gen_dl_rdphalf1 });
        gens.push_back({ "spec.f3dex.dl.refilldec", gen_dl_refilldec });
        gens.push_back({ "spec.f3dex.dl.reg_refill", gen_dl_reg_refill });
        gens.push_back({ "spec.f3dex.dl.reg_xlate", gen_dl_reg_xlate });
        gens.push_back({ "spec.f3dex.dl.reg_head", gen_dl_reg_head });
    }
} dl_gen_init;
