// scenarios of the struct-level matrix functions (spec/fxmtx_spec.c): the mtx_gen.inc generators (same entry state) with a wider matrix distribution -- saturating results, fractions >= 0x8000,
// negative integer parts, sparse / identity-like matrices -- and the dead ranges of the relight (fourth direction byte, scratch).
// (mtx_gen.inc is not included: it has no include guard and is included on its own when all specs are built.)
#include <math.h>

static int16_t fxm_int16_pick()
{
    static const int16_t v[] = { 0x7fff, -0x8000, 0x7ffe, -1, 0, 1, 2, -2, 0x4000, -0x4000, 0x2000 };
    return v[rnd() % (sizeof v / sizeof v[0])];
}
static uint16_t fxm_frac_pick()
{
    static const uint16_t v[] = { 0xffff, 0, 0x8000, 0x7fff, 0x8001, 0x0001, 0xc000, 0x4000 };
    return v[rnd() % (sizeof v / sizeof v[0])];
}

// a random Mtx (16 integer halves then 16 fraction halves) at DMEM address a. kinds: 0 random, 1 small, 2 scaled rotation + translation, 3 edge halves, 4 saturating (integers 0x7fff / 0x8000, fractions
// 0xffff / 0 ...), 5 every fraction >= 0x8000, small signed integers, 6 negative integer parts, 7 sparse (mostly 0 and unit elements), 8 identity + random translation row, 9 zero except one row
static void fxm_put(Machine &m, unsigned a, unsigned kind)
{
    uint16_t hi[16], lo[16];
    switch (kind)
    {
    case 0:
        for (int i = 0; i < 16; i++) { hi[i] = uint16_t(rnd()); lo[i] = uint16_t(rnd()); }
        break;
    case 1:
        for (int i = 0; i < 16; i++) { int32_t x = int32_t(rnd() % 0x60000) - 0x30000; hi[i] = uint16_t(uint32_t(x) >> 16); lo[i] = uint16_t(x); }
        break;
    case 2:
    {
        double ax = (rnd() % 10000) / 10000.0 * 6.2831853, ay = (rnd() % 10000) / 10000.0 * 6.2831853, az = (rnd() % 10000) / 10000.0 * 6.2831853;
        double s = (rnd() % 4) ? 0.25 + (rnd() % 1000) / 500.0 : 1.0;
        double cx = cos(ax), sx = sin(ax), cy = cos(ay), sy = sin(ay), cz = cos(az), sz = sin(az);
        double R[3][3] = { { cy * cz, cy * sz, -sy }, { sx * sy * cz - cx * sz, sx * sy * sz + cx * cz, sx * cy }, { cx * sy * cz + sx * sz, cx * sy * sz - sx * cz, cx * cy } };
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
            {
                double d = y == 3 ? (x == 3 ? 1.0 : (double(int(rnd() % 2000)) - 1000.0)) : (x == 3 ? 0.0 : R[y][x] * s);
                int32_t v = int32_t(floor(d * 65536.0 + 0.5));
                hi[4 * y + x] = uint16_t(uint32_t(v) >> 16);
                lo[4 * y + x] = uint16_t(v);
            }
        break;
    }
    case 3:
        for (int i = 0; i < 16; i++) { hi[i] = rnd_edgy16(); lo[i] = rnd_edgy16(); }
        break;
    case 4:
        for (int i = 0; i < 16; i++) { hi[i] = uint16_t(fxm_int16_pick()); lo[i] = fxm_frac_pick(); }
        break;
    case 5:
        for (int i = 0; i < 16; i++) { hi[i] = uint16_t(int16_t(int(rnd() % 9) - 4)); lo[i] = uint16_t(0x8000 | rnd()); }
        break;
    case 6:
        for (int i = 0; i < 16; i++) { hi[i] = uint16_t(0x8000 | (rnd() % 3 ? (rnd() & 0x7f) | 0x7f80 : rnd())); lo[i] = (rnd() % 4) ? uint16_t(rnd()) : fxm_frac_pick(); }
        break;
    case 7:
        for (int i = 0; i < 16; i++)
        {
            unsigned r = rnd() % 6;
            hi[i] = r == 0 ? 1 : r == 1 ? uint16_t(rnd()) : r == 2 ? 0xffff : 0;
            lo[i] = r == 1 ? uint16_t(rnd()) : r == 2 ? fxm_frac_pick() : 0;
        }
        break;
    case 8:
        for (int i = 0; i < 16; i++) { hi[i] = (i % 5 == 0) ? 1 : 0; lo[i] = 0; }
        if (rnd() % 2) for (int i = 12; i < 15; i++) { hi[i] = uint16_t(rnd()); lo[i] = uint16_t(rnd()); }
        break;
    default:
    {
        for (int i = 0; i < 16; i++) { hi[i] = 0; lo[i] = 0; }
        unsigned row = rnd() % 4;
        for (int i = 0; i < 4; i++) { hi[4 * row + i] = rnd_edgy16(); lo[4 * row + i] = rnd_edgy16(); }
        break;
    }
    }
    for (int i = 0; i < 16; i++)
    {
        put16(m.dmem, a + 2 * i, hi[i]);
        put16(m.dmem, a + 0x20 + 2 * i, lo[i]);
    }
}
static unsigned fxm_kind()
{
    unsigned k = rnd() % 20;
    return k < 2 ? 1 : k < 5 ? 2 : k < 7 ? 0 : k < 9 ? 3 : 4 + rnd() % 6;
}

static void fxm_put_all(Machine &m)
{
    fxm_put(m, 0xae0, fxm_kind());
    fxm_put(m, 0x3e0, fxm_kind());
    fxm_put(m, 0x420, fxm_kind());
    fxm_put(m, 0x460, fxm_kind());              // the old MVP: overwritten completely
    for (unsigned o = 0xde0; o < 0xe40; o += 2)  // scratch: leftovers
        put16(m.dmem, o, uint16_t(rnd()));
}

static void gen_fxmtx_mtx(Machine &m, const Ucode &u, Scenario &sc)
{
    (void)u;
    auto &st = m.cpu.get_state();
    fxm_put_all(m);
    st.sr[1] = (rnd() % 4) ? (rnd() & 7) : (rnd() & 0xff);               // flags: bit0 projection, bit1 load, bit2 push, the rest ignored
    st.sr[29] = 0x110;
    st.sr[22] = 0xae0;
    st.sr[18] = (rnd() % 4) ? 63 : (rnd() & 0x3ff);                       // w0 & 0x3ff
    uint32_t ptr = (rnd() % 8) ? (0x100000 + 64 * (rnd() % 1000)) : rnd();
    uint32_t lim = ptr + 64 * (rnd() % 6);
    if (rnd() % 5 == 0) lim = ptr;                                        // stack full
    else if (rnd() % 8 == 0) lim = ptr - 64 * (1 + rnd() % 3);
    put32(m.dmem, 0x134, ptr);
    put32(m.dmem, 0x15c, lim);
    put32(m.dmem, 0x12c, rnd());
    sc.exits.push_back(0x09c);
    sc.imem_overlay = 0;
}

static void gen_fxmtx_mvp(Machine &m, const Ucode &u, Scenario &sc)
{
    (void)u;
    auto &st = m.cpu.get_state();
    fxm_put_all(m);
    st.sr[29] = 0x110;
    st.sr[3] = 0x460;
    sc.exits.push_back(0x09c);
    sc.imem_overlay = 0;
}

// the light-direction recomputation: as gen_mtx_light (entry with the negative light-pointer word in $1, v11 / v15 = MVP row 3 as the vertex handler loaded them), the modelview from the wider kinds
// (rotations dominate: a zero or saturated direction is the interesting minority), the dead ranges
static void gen_fxmtx_relight(Machine &m, const Ucode &u, Scenario &sc)
{
    auto &st = m.cpu.get_state();
    unsigned kmv = (rnd() % 3) ? 2 : fxm_kind();
    fxm_put(m, 0x3e0, kmv);
    fxm_put(m, 0x460, fxm_kind());
    for (unsigned o = 0xde0; o < 0xe40; o += 2)
        put16(m.dmem, o, uint16_t(rnd()));
    for (unsigned o = 0x1a0; o < 0x2e0; o += 4)
        put32(m.dmem, o, rnd());
    // the clamp constants (-8191, 8191, 4 at 0x208 .. 0x20d) are the ucode's own, not random: the function has them built in (mtx_gen.inc randomises them, its spec reads them)
    for (unsigned o = 0x208; o < 0x210; o += 4)
        put32(m.dmem, o, u.data_dmem[o >> 2]);
    if (get16(m.dmem, 0x208) != 0xe001 || get16(m.dmem, 0x20a) != 0x1fff || get16(m.dmem, 0x20c) != 4)
        fprintf(stderr, "fxmtx: unexpected clamp constants in the ucode data\n");
    unsigned n = rnd() % 8;
    uint32_t p0 = (n + 1) * 32;
    if (rnd() % 10 == 0) p0 = 32 * (rnd() % 9);
    for (unsigned s = 0; s <= 9; s++)
    {
        unsigned a = 0x1b8 + 32 * s;
        int mode = rnd() % 8;
        for (unsigned k = 0; k < 3; k++)
        {
            int8_t d;
            switch (mode)
            {
            case 0: d = 0; break;
            case 1: d = (rnd() & 1) ? -128 : 127; break;
            case 2: d = int8_t(rnd() % 5) - 2; break;
            default: d = int8_t(int(rnd() % 255) - 127); break;
            }
            put8(m.dmem, a + k, uint8_t(d));
        }
        if (mode == 3)                           // a unit vector
        {
            double x = (rnd() % 2001) / 1000.0 - 1, y = (rnd() % 2001) / 1000.0 - 1, z = (rnd() % 2001) / 1000.0 - 1;
            double l = sqrt(x * x + y * y + z * z);
            if (l > 0.01)
                for (unsigned k = 0; k < 3; k++)
                    put8(m.dmem, a + k, uint8_t(int8_t(lround(127 * (k == 0 ? x : k == 1 ? y : z) / l))));
        }
    }
    uint32_t word = (rnd() % 3 == 0) ? (0xff000000u | p0) : (rnd() | 0x80000000u);
    word = (word & ~0xfffu) | p0;
    put32(m.dmem, 0x12c, word);
    st.sr[1] = word;
    st.sr[29] = 0x110;
    for (int l = 0; l < 8; l++)
    {
        st.cp2.regs[11].e[l] = get16(m.dmem, 0x460 + 24 + 2 * (l & 3));
        st.cp2.regs[15].e[l] = get16(m.dmem, 0x460 + 56 + 2 * (l & 3));
    }
    sc.exits.push_back(get16(m.dmem, 0xa0) & 0xfff);
    sc.imem_overlay = 2;
    // dead: the scratch of the transposition, the fourth byte of every direction (word 0x1c0 + 32k: bytes 3 and 7)
    sc.dead.push_back({ 0xde0, 0xe40 });
    for (unsigned k = 0; k <= p0 / 32; k++)
    {
        sc.dead.push_back({ 0x1c3 + 32 * k, 0x1c4 + 32 * k });
        sc.dead.push_back({ 0x1c7 + 32 * k, 0x1c8 + 32 * k });
    }
}

static struct FxMtxGenInit
{
    FxMtxGenInit()
    {
        gens.push_back({ "spec.f3dex.fxmtx.mtx.", gen_fxmtx_mtx });
        gens.push_back({ "spec.f3dex.fxmtx.mvp.", gen_fxmtx_mvp });
        gens.push_back({ "spec.f3dex.fxmtx.relight.", gen_fxmtx_relight });
    }
} fxmtx_gen_init;
