// Scenario generators of the mtx spec (spec/mtx_spec.c): G_MTX, G_POPMTX, the MVP update and the light-direction recomputation of the lighting overlay.
// Included into rt_test.cpp after rt_gen.inc (put16 / put32 / rnd / edge16 are there).
#include <math.h>

// a random Mtx (S15.16, 16 integer halves then 16 fraction halves) at DMEM address a: kind 0 fully random, 1 small values (the sum of the products
// stays far from saturation), 2 a rotation scaled by a random factor with a translation, 3 edge values
static void mtx_put(Machine &m, unsigned a, unsigned kind)
{
    int32_t v[16];
    switch (kind)
    {
    case 0:
        for (auto &x : v) x = int32_t(rnd());
        break;
    case 1:
        for (auto &x : v) x = int32_t(rnd() % 0x60000) - 0x30000;
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
                v[4 * y + x] = int32_t(floor(d * 65536.0 + 0.5));
            }
        break;
    }
    default:
        for (auto &x : v)
        {
            uint32_t hi = rnd_edgy16(), lo = rnd_edgy16();
            x = int32_t(hi << 16 | lo);
        }
        break;
    }
    for (int i = 0; i < 16; i++)
    {
        put16(m.dmem, a + 2 * i, uint16_t(uint32_t(v[i]) >> 16));
        put16(m.dmem, a + 0x20 + 2 * i, uint16_t(v[i]));
    }
}
static unsigned mtx_kind() { unsigned k = rnd() % 8; return k < 2 ? 1 : k < 5 ? 2 : k < 7 ? 0 : 3; }

static void mtx_put_all(Machine &m)
{
    mtx_put(m, 0xae0, mtx_kind());
    mtx_put(m, 0x3e0, mtx_kind());
    mtx_put(m, 0x420, mtx_kind());
    mtx_put(m, 0x460, mtx_kind());              // the old MVP: overwritten completely
    for (unsigned o = 0xde0; o < 0xe40; o += 2) // scratch: leftovers
        put16(m.dmem, o, uint16_t(rnd()));
}

// G_MTX: the handler 0x554 (banjo) .. the main loop
static void gen_mtx_mtx(Machine &m, const Ucode &u, Scenario &sc)
{
    (void)u;
    auto &st = m.cpu.get_state();
    mtx_put_all(m);
    st.sr[1] = (rnd() % 4) ? (rnd() & 7) : (rnd() & 0xff);               // flags: bit0 projection, bit1 load, bit2 push, the rest ignored
    st.sr[29] = 0x110;
    st.sr[22] = 0xae0;
    st.sr[18] = (rnd() % 4) ? 63 : (rnd() & 0x3ff);                       // w0 & 0x3ff
    uint32_t ptr = (rnd() % 8) ? (0x100000 + 64 * (rnd() % 1000)) : rnd();
    uint32_t lim = ptr + 64 * (rnd() % 6);
    if (rnd() % 5 == 0) lim = ptr;                                        // stack full
    else if (rnd() % 8 == 0) lim = ptr - 64 * (1 + rnd() % 3);            // limit below the pointer: not 'full' (the compare is for equality)
    put32(m.dmem, 0x134, ptr);
    put32(m.dmem, 0x15c, lim);
    put32(m.dmem, 0x12c, rnd());
    sc.exits.push_back(0x09c);
    sc.imem_overlay = 0;
}

// G_POPMTX: the handler 0x204 (banjo) .. the main loop
static void gen_mtx_pop(Machine &m, const Ucode &u, Scenario &sc)
{
    auto &st = m.cpu.get_state();
    mtx_put_all(m);
    st.sr[29] = 0x110;
    uint32_t base = (rnd() % 8) ? (0x100000 + 64 * (rnd() % 1000)) : rnd();
    uint32_t ptr = base + 64 * (rnd() % 5);
    switch (rnd() % 12)
    {
    case 0: ptr = base - 64 * (1 + rnd() % 3); break;                  // below the base
    case 1: ptr = rnd(); break;
    case 2: ptr = base + (rnd() & 0x3f); break;                       // not a multiple of 64
    default: break;
    }
    put32(m.dmem, 0xfe0, base);
    put32(m.dmem, 0x134, ptr);
    put32(m.dmem, 0x12c, rnd());
    sc.exits.push_back(0x09c);
    sc.exits.push_back(u.id == 0x34dad67335a54cdaull || u.id == 0x5f33f7c2be924de0ull ? 0x15c : 0xff4);     // the DMA routine's write of SP_RD_LEN
    sc.imem_overlay = 0;
}

// G_POPMTX after the DMA read has completed: the new stack pointer is in $19, the popped matrix in 0x3e0
static void gen_mtx_poptail(Machine &m, const Ucode &u, Scenario &sc)
{
    (void)u;
    auto &st = m.cpu.get_state();
    mtx_put_all(m);
    st.sr[29] = 0x110;
    st.sr[19] = (rnd() % 8) ? (0x100000 + 64 * (rnd() % 1000)) : rnd();
    put32(m.dmem, 0x134, rnd());
    sc.exits.push_back(0x09c);
    sc.imem_overlay = 0;
}

// the MVP recomputation alone: entered at the tail with $1/$2/$3 not yet set (the entry sets them), $31 as the handler left it
static void gen_mtx_mvp(Machine &m, const Ucode &u, Scenario &sc)
{
    (void)u;
    auto &st = m.cpu.get_state();
    mtx_put_all(m);
    st.sr[29] = 0x110;
    st.sr[3] = 0x460;                          // the popmtx path has set $3 before jumping to the tail; the mtx path sets it itself (see below)
    sc.exits.push_back(0x09c);
    sc.imem_overlay = 0;
}

// the light-direction recomputation: entry with the light-pointer word (negative) in $1, vertex registers v11 / v15 = MVP row 3 as loaded at the start
// of the vertex command
static void gen_mtx_light(Machine &m, const Ucode &u, Scenario &sc)
{
    auto &st = m.cpu.get_state();
    mtx_put(m, 0x3e0, mtx_kind());
    mtx_put(m, 0x460, mtx_kind());
    for (unsigned o = 0xde0; o < 0xe40; o += 2)
        put16(m.dmem, o, uint16_t(rnd()));
    // the lights area: random bytes, directions mostly sane (|d| around 127) with edge bytes, sometimes zero
    for (unsigned o = 0x1a0; o < 0x2e0; o += 4)
        put32(m.dmem, o, rnd());
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
    // v11 / v15: MVP row 3 (ints / fractions), each lane group twice
    for (int l = 0; l < 8; l++)
    {
        st.cp2.regs[11].e[l] = get16(m.dmem, 0x460 + 24 + 2 * (l & 3));
        st.cp2.regs[15].e[l] = get16(m.dmem, 0x460 + 56 + 2 * (l & 3));
    }
    // the exit: the lighting loop (the pc the vertex code reloads from DMEM 0xa0)
    sc.exits.push_back(get16(m.dmem, 0xa0) & 0xfff);
    sc.imem_overlay = 2;
    (void)u;
}

static struct MtxGenInit
{
    MtxGenInit()
    {
        gens.push_back({ "spec.f3dex.mtx.mtx", gen_mtx_mtx });
        gens.push_back({ "spec.f3dex.mtx.pop.", gen_mtx_pop });
        gens.push_back({ "spec.f3dex.mtx.poptail", gen_mtx_poptail });
        gens.push_back({ "spec.f3dex.mtx.mvp", gen_mtx_mvp });
        gens.push_back({ "spec.f3dex.mtx.light", gen_mtx_light });
    }
} mtx_gen_init;
