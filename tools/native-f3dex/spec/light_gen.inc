#ifndef LIGHT_GEN_INC
#define LIGHT_GEN_INC
// scenario generator of the light spec (spec.f3dex.light.*): a G_VTX call (the whole vertex handler) with random light records and counts, geometry mode (fog, lighting,
// texgen, linear texgen; F3DLX: G_CLIPPING), matrices (random / diagonal-ish with positive w), viewport, texture scale, fog words, perspNorm, clip ratio, input vertices.
// The harness randomises registers and flags; lighting calls never have the recompute bit (0x12c bit 31) set (the spec declines those).
static uint8_t lg_byte_dist()
{
    switch (rnd() % 6)
    {
    case 0: return 0;
    case 1: return 0xff;
    case 2: return 0x80;
    case 3: return 0x7f;
    case 4: return rnd() % 64;
    default: return rnd();
    }
}
static int8_t lg_unit_comp()
{
    // a random direction scaled to +-127, or an edge
    switch (rnd() % 5)
    {
    case 0: return -128;
    case 1: return 127;
    case 2: return 0;
    default: return int8_t(int(rnd() % 255) - 127);
    }
}
static void gen_light(Machine &m, const Ucode &u, Scenario &sc)
{
    auto &st = m.cpu.get_state();
    unsigned v0 = rnd() % 32;
    unsigned n = 1 + rnd() % (32 - v0);
    if (rnd() % 3 == 0) n = 1 + rnd() % 4;
    if (rnd() % 16 == 0) n = 0;                          // n = 0 still handles one vertex
    unsigned nvtx = n ? n : 1;
    if (v0 + nvtx > 32) { v0 = 32 - nvtx; }
    unsigned fam = rnd() % 4;
    // input vertices: pos, flag, st, normal / colour
    for (unsigned i = 0; i < nvtx + 1; i++)
    {
        unsigned a = 0xae0 + 16 * i;
        for (unsigned o = 0; o < 16; o++)
            put8(m.dmem, a + o, rnd());
        for (unsigned o = 0; o < 6; o += 2)
            put16(m.dmem, a + o, (rnd() % 3 == 0) ? rnd_edgy16() : uint16_t(int16_t(int(rnd() % 4000) - 2000)));
        if (rnd() & 1)
            for (unsigned o = 8; o < 12; o += 2)
                put16(m.dmem, a + o, uint16_t(int16_t(int(rnd() % 4096) * 4 - 8192)));
        for (unsigned o = 12; o < 16; o++)
            if (rnd() % 3 == 0) put8(m.dmem, a + o, (rnd() & 1) ? lg_unit_comp() : lg_byte_dist());
    }
    // model-view-projection matrix, viewport
    if (fam == 0)
    {
        for (unsigned o = 0; o < 64; o += 2)
            put16(m.dmem, 0x460 + o, rnd_edgy16());
    }
    else
    {
        for (unsigned o = 0; o < 64; o += 2)
            put16(m.dmem, 0x460 + o, 0);
        for (unsigned i = 0; i < 4; i++)                  // integer rows: mostly diagonal
            put16(m.dmem, 0x460 + 8 * i + 2 * i, (rnd() % 4) ? uint16_t(1 + rnd() % 3) : rnd_edgy16());
        for (unsigned o = 0; o < 64; o += 2)
            if (rnd() % 3 == 0)
                put16(m.dmem, 0x460 + o, uint16_t(rnd() % 3 ? rnd() % 6 : rnd()));
        for (unsigned o = 0; o < 32; o += 2)               // fractions
            if (rnd() % 2)
                put16(m.dmem, 0x480 + o, rnd());
        // w mostly positive and not tiny
        put16(m.dmem, 0x460 + 8 * 3 + 6, uint16_t(rnd() % 16 ? 1 + rnd() % 400 : rnd()));
        put16(m.dmem, 0x480 + 8 * 3 + 6, rnd());
        for (unsigned i = 0; i < 3; i++)
            put16(m.dmem, 0x460 + 8 * i + 6, (rnd() % 3 == 0) ? uint16_t(rnd() % 3) : uint16_t(0));
    }
    for (unsigned o = 0; o < 16; o += 2)
        put16(m.dmem, 0x380 + o, (rnd() % 4) ? uint16_t(rnd() % 3 ? rnd() % 1500 : 0xfffe - rnd() % 1500) : rnd_edgy16());
    put16(m.dmem, 0x380 + 4, (rnd() % 3) ? 0x1ff : rnd());          // z scale / trans like a game's
    put16(m.dmem, 0x388 + 4, (rnd() % 3) ? 0x1ff : rnd());
    put16(m.dmem, 0x112, (rnd() % 3) ? 0xffff : rnd_edgy16());      // perspective normalisation
    put16(m.dmem, 0x76, (rnd() % 2) ? 1 : rnd() % 8);               // clip ratio
    if (rnd() % 20 == 0) put16(m.dmem, 0x76, rnd());
    put32(m.dmem, 0x124, rnd());                                    // texture scale s, t
    if (rnd() % 3 == 0) put32(m.dmem, 0x124, 0x8000u << 16 | 0x8000u);
    // fog: multiplier / offset words
    put16(m.dmem, 0x390, (rnd() % 3) ? uint16_t(int16_t(int(rnd() % 2000) - 400)) : rnd_edgy16());
    put16(m.dmem, 0x392, (rnd() % 3) ? uint16_t(int16_t(int(rnd() % 600) - 200)) : rnd_edgy16());
    if (rnd() % 12 == 0) put16(m.dmem, 0x394, rnd());
    // lights: records of 32 bytes at 0x1b0 (colours 8 bytes, transformed direction 8 bytes at +16); 0x1a0: the texgen constants
    unsigned nl = rnd() % 9;                                      // L = 32 * nl
    for (unsigned o = 0x1a0; o < 0x2e0; o++)
        put8(m.dmem, o, (rnd() % 3) ? lg_byte_dist() : rnd());
    for (unsigned r = 0; r <= 9; r++)                              // transformed directions: duplicated 4 bytes mostly, +-127 range
    {
        unsigned b = 0x1c0 + 32 * r;
        for (unsigned k = 0; k < 4; k++)
        {
            int8_t c = (rnd() % 4) ? int8_t(int(rnd() % 255) - 127) : lg_unit_comp();
            put8(m.dmem, b + k, c);
            put8(m.dmem, b + 4 + k, (rnd() % 8) ? c : (int8_t)rnd());
        }
        if (rnd() % 2)                                            // colours: duplicated (RGB, pad) pair
        {
            unsigned c0 = 0x1b0 + 32 * r;
            for (unsigned k = 0; k < 4; k++)
                put8(m.dmem, c0 + 4 + k, ((const uint8_t *)m.dmem)[(c0 + k) ^ 3]);
        }
    }
    if (rnd() % 2)                                                 // like a game: the lookat Y colour (0,0x80,0), X colour 0, constants 0x80,0,0,0
    {
        for (unsigned k = 0; k < 8; k++)
        {
            put8(m.dmem, 0x1a0 + k, (k & 3) == 0 ? 0x80 : 0);
            put8(m.dmem, 0x1b0 + k, (k & 3) == 1 ? 0x80 : 0);
            put8(m.dmem, 0x1d0 + k, 0);
        }
    }
    if (rnd() % 4 == 0)                                            // small colours: sums seldom saturate
        for (unsigned r = 0; r <= 9; r++)
            for (unsigned k = 0; k < 8; k++)
                put8(m.dmem, 0x1b0 + 32 * r + k, rnd() % 40);
    put32(m.dmem, 0x12c, 32 * nl);
    // geometry mode: lighting mostly on
    uint32_t g = rnd() & 0xfff0ffffu;
    g &= ~0xf0000u;
    if (rnd() % 4) g |= 0x20000;
    if (rnd() % 2) g |= 0x10000;
    if (rnd() % 2) g |= 0x40000;
    if (rnd() % 2) g |= 0x80000;
    if (u.id == 0x7502cfb7098eeffbull || u.id == 0x5f33f7c2be924de0ull)      // F3DLX: bit 23 = G_CLIPPING, mostly on
    {
        g &= ~0x800000u;
        if (rnd() % 5) g |= 0x800000;
    }
    put32(m.dmem, 0x114, g);
    // the command: w0 byte1 = v0 * 2, byte2 = n << 2 | low bits
    st.sr[27] = 0x9a8;
    put8(m.dmem, 0x9a8 - 7, v0 * 2);
    put8(m.dmem, 0x9a8 - 6, (n << 2) | (rnd() & 3));
    st.sr[1] = v0 * 2;
    st.sr[22] = 0xae0;
    st.sr[29] = 0x110;
    st.sr[6] = rnd() & 15;
    sc.items = nvtx;
    sc.exits.push_back(0x09c);
    sc.imem_overlay = 2;
}
static struct LightGenInit
{
    LightGenInit()
    {
        gens.push_back({ "spec.f3dex.light.", gen_light });
    }
} light_gen_init;

#endif
