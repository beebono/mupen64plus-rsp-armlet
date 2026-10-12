#ifndef CLIP_GEN_INC
#define CLIP_GEN_INC
// scenarios for spec.f3dex.clip.*: the clipping overlay (overlay 1) entered from the triangle code with three transformed vertices of the buffer at 0x4a0
// (docs/ucode-f3dex.md "Clipping"). The entry conditions of the triangle code hold: the AND of the three flag words has none of 0x7070 (the 0x7030 for
// F3DEX.NoN), the OR has one of 0x4343. The vertices carry realistic clip-space positions (x y z w as S15.16) and clip flags consistent with them.
struct ClipLay
{
    unsigned entry;     // overlay entry (pc the triangle code jumps to)
    unsigned tri;       // the triangle code entered with a clipped triangle
    unsigned ret2;      // second possible return address ($24) of the caller (TRI2's)
    unsigned rej;       // reject mask of the AND of the flags
    int kind;           // 0 F3DEX, 1 NoN, 2 F3DLX
    unsigned fan;       // pc of the fan step (the triangle code returns there)
};
static ClipLay clip_lay_of(const Ucode &u)
{
    switch (u.id)
    {
    case 0xfd5a31ccf6d40b62ull: return { 0x740, 0x98c, 0x1ec, 0x7030, 1, 0x940 };       // F3DEX.NoN 1.22
    case 0x047252e810e35d56ull: return { 0x718, 0x964, 0x1e4, 0x7070, 0, 0x918 };       // F3DEX 1.23
    case 0x34dad67335a54cdaull: return { 0x7a8, 0x9f4, 0x22c, 0x7070, 0, 0x9a8 };       // F3DEX 0.95
    case 0x7502cfb7098eeffbull: return { 0x740, 0x994, 0x1dc, 0x7070, 2, 0x940 };       // F3DLX 1.23
    case 0x5f33f7c2be924de0ull: return { 0x7d0, 0xa24, 0x224, 0x7070, 2, 0x9d0 };       // F3DLX 0.95
    default: return { 0x720, 0x96c, 0x1ec, 0x7070, 0, 0x920 };                           // F3DEX 1.21
    }
}

// the flag word of the vertex tail (pc 0x414..): pa = compare against w, pb = against w * clip ratio, both for x, y, z
static unsigned clip_gen_bits(int64_t X, int64_t W, int k)
{
    int opp = (X < 0) != (W < 0);
    int le = opp ? (X + W <= 0) : (W < 0), ge = opp ? (W < 0) : (X - W >= 0);
    return (unsigned)le << k | (unsigned)ge << (8 + k);
}
static unsigned clip_gen_flags(const int32_t p[4], int ratio, int kind, bool pair_b)
{
    int64_t W = p[3];
    int64_t Wr = ((int64_t)W * ratio);
    // Wr as the vertex tail forms it: the S15.16 product saturated in the integer half
    int64_t wri = Wr >> 16;
    if (wri > 32767) Wr = (int64_t)32767 * 65536 + 0; // only the integer half is clamped, the fraction is the low 16 bits
    if (wri < -32768) Wr = (int64_t)-32768 * 65536;
    unsigned pa = 0, pb = 0;
    for (int k = 0; k < 3; k++)
    {
        pa |= clip_gen_bits(p[k], W, k);
        pb |= clip_gen_bits(p[k], Wr, k);
    }
    unsigned f;
    if (kind == 1)
    {
        if (!pair_b)
            f = (pb & 0x707u) | ((pa & 0x703u) << 4);
        else
        {
            f = (pb & 0x707u) | ((pa & 0x7030u));
            if (p[3] < 0) f |= 0x40;
            if (rnd() & 1) f |= 4;
        }
    }
    else
        f = (pb & 0x707u) | ((pa & 0x707u) << 4);
    return f;
}

static int32_t clip_gen_coord(int32_t w, int mode)
{
    // a coordinate relative to w: mostly inside, or beyond, or near the plane
    int64_t v;
    switch (mode)
    {
    case 0: v = (int64_t)w * (int)(rnd() % 2001 - 1000) / 1000; break;           // |x| <= w
    case 1: v = (int64_t)w * (int)(rnd() % 3001 - 1500) / 1000; break;           // up to 1.5 w
    case 2: v = (int64_t)w * (int)(rnd() % 201 - 100) / 100 + (int)(rnd() % 5) - 2; break;   // around +-w and 0
    case 3: v = (rnd() & 1) ? w : -(int64_t)w; v += (int)(rnd() % 3) - 1; break;
    default: v = (int32_t)rnd(); break;
    }
    if (v > 0x7fffffffll) v = 0x7fffffffll;
    if (v < -0x80000000ll) v = -0x80000000ll;
    return (int32_t)v;
}

static void gen_clip_main(Machine &m, const Ucode &u, Scenario &sc)
{
    const ClipLay L = clip_lay_of(u);
    auto &st = m.cpu.get_state();
    // the vertex buffer: random contents, then the three vertices
    for (unsigned o = 0; o < 32 * 0x28; o += 2)
        put16(m.dmem, 0x4a0 + o, rnd_edgy16());
    // the lists and the new-vertex area: stale data (zero most of the time, as after the previous run, else random)
    int stale = rnd() % 4;
    for (unsigned o = 0xc40; o < 0xc68; o += 2)
        put16(m.dmem, o, stale == 0 ? uint16_t(rnd()) : (stale == 1 ? uint16_t(0x4a0 + 0x28 * (rnd() % 32)) : 0));
    for (unsigned o = 0xdb8; o < 0xfc0; o += 2)
        put16(m.dmem, o, (rnd() % 3) ? 0 : uint16_t(rnd()));
    put16(m.dmem, 0xb4, rnd());
    put16(m.dmem, 0x106, rnd());
    put16(m.dmem, 0x158, rnd());
    // state the vertex tail reads
    int ratio = 1;
    switch (rnd() % 8) { case 0: ratio = 2; break; case 1: ratio = 3; break; case 2: ratio = int(rnd() % 8); break; case 3: ratio = int16_t(rnd()); break; default: break; }
    // the four clip ratio words (RNX 0x74, RNY 0x7c, RPX 0x84, RPY 0x8c; only the low halves 0x76, 0x7e, 0x86, 0x8e matter): the w coefficients of the planes
    put16(m.dmem, 0x76, uint16_t(ratio));
    put16(m.dmem, 0x7e, uint16_t(ratio));
    put16(m.dmem, 0x86, uint16_t(-ratio));
    put16(m.dmem, 0x8e, uint16_t(-ratio));
    if (rnd() % 10 == 0)
        put16(m.dmem, 0x7e + 8 * (rnd() % 3), uint16_t(rnd() % 4 ? int(rnd() % 6) - 3 : int(rnd())));
    put16(m.dmem, 0x112, (rnd() % 3) ? uint16_t(0xffff - rnd() % 64) : uint16_t(rnd()));
    uint32_t g = rnd();
    if (rnd() % 2) g |= 0x10000u; else g &= ~0x10000u;          // fog
    if (rnd() % 2) g |= 0x20000u; else g &= ~0x20000u;
    g |= 0x800000u;
    put32(m.dmem, 0x114, g);
    for (unsigned o = 0; o < 16; o += 2)
        put16(m.dmem, 0x380 + o, (rnd() % 8) ? uint16_t(int16_t(int(rnd() % 1200) - 600)) : edge16());
    put16(m.dmem, 0x390, (rnd() % 4) ? uint16_t(rnd() % 600) : uint16_t(rnd()));
    put16(m.dmem, 0x392, (rnd() % 4) ? uint16_t(int16_t(int(rnd() % 400) - 200)) : uint16_t(rnd()));
    put16(m.dmem, 0x394, (rnd() % 4) ? 0xff : uint16_t(rnd()));

    // the three vertices
    unsigned slot[3];
    for (int i = 0; i < 3; i++)
        slot[i] = 0x4a0 + 0x28 * (rnd() % 32);
    if (rnd() % 24 == 0) slot[1] = slot[0];
    if (rnd() % 24 == 0) slot[2] = slot[(rnd() & 1)];
    int cmode = rnd() % 5;
    for (int attempt = 0; attempt < 400; attempt++)
    {
        int32_t pos[3][4];
        bool wneg[3];
        int wk = rnd() % 8;
        int32_t wbase = 1 + (int32_t)(rnd() >> (4 + rnd() % 20));
        for (int i = 0; i < 3; i++)
        {
            int32_t w = (int32_t)(wbase + (int32_t)(rnd() >> (4 + rnd() % 22)));
            if (wk == 0 && (rnd() % 3 == 0)) w = -w;                 // behind the eye
            else if (wk == 1 && (rnd() % 5 == 0)) w = 0;
            else if (wk == 2) w = (int32_t)rnd();
            wneg[i] = w < 0;
            pos[i][3] = w;
            int ww = w < 0 ? -w : w;
            int mode = cmode == 4 ? int(rnd() % 5) : cmode;
            for (int k = 0; k < 3; k++)
                pos[i][k] = clip_gen_coord(ww, mode);
            if (rnd() % 16 == 0) pos[i][rnd() % 3] = (int32_t)rnd();
        }
        unsigned fl[3];
        bool pb[3] = { bool(rnd() & 1), bool(rnd() & 1), bool(rnd() & 1) };
        bool bad = rnd() % 10 == 0;                                    // flags not matching the positions
        for (int i = 0; i < 3; i++)
            fl[i] = bad ? (rnd() & 0x7777u) : clip_gen_flags(pos[i], ratio, L.kind, pb[i]);
        unsigned a = fl[0] & fl[1] & fl[2], o = fl[0] | fl[1] | fl[2];
        if ((a & L.rej) || !(o & 0x4343))
            continue;
        for (int i = 0; i < 3; i++)
        {
            unsigned s = slot[i];
            // equal slots must agree: later vertices overwrite earlier ones
            for (int k = 0; k < 4; k++)
            {
                put16(m.dmem, s + 2 * k, uint16_t(uint32_t(pos[i][k]) >> 16));
                put16(m.dmem, s + 8 + 2 * k, uint16_t(pos[i][k]));
            }
            put16(m.dmem, s + 36, uint16_t(fl[i]));
        }
        goto placed;
    }
    // no luck: a triangle that crosses the x plane
    {
        int32_t w = 0x10000 * 5;
        int32_t p[3][4] = { { -w * 2, 0, 0, w }, { w / 2, w / 3, 0, w }, { w / 4, -w / 3, 0, w } };
        for (int i = 0; i < 3; i++)
        {
            unsigned s = slot[i];
            for (int k = 0; k < 4; k++)
            {
                put16(m.dmem, s + 2 * k, uint16_t(uint32_t(p[i][k]) >> 16));
                put16(m.dmem, s + 8 + 2 * k, uint16_t(p[i][k]));
            }
            put16(m.dmem, s + 36, uint16_t(clip_gen_flags(p[i], ratio, L.kind, false)));
        }
        put16(m.dmem, 0x76, 1); put16(m.dmem, 0x7e, 1); put16(m.dmem, 0x86, 0xffff); put16(m.dmem, 0x8e, 0xffff);
    }
placed:
    st.sr[1] = slot[0];
    st.sr[2] = slot[1];
    st.sr[3] = slot[2];
    st.sr[24] = (rnd() & 1) ? 0x09c : L.ret2;
    st.sr[29] = 0x110;
    sc.exits.push_back(0x09c);
    sc.exits.push_back(L.ret2);
    sc.imem_overlay = 1;
}

// the fan step: the pointer at 0x106 into a zero-terminated polygon list, the fan's first vertex at 0xb4, the caller's return in $30
static void gen_clip_fan(Machine &m, const Ucode &u, Scenario &sc)
{
    const ClipLay L = clip_lay_of(u);
    auto &st = m.cpu.get_state();
    for (unsigned o = 0xc40; o < 0xc68; o += 2)
        put16(m.dmem, o, 0);
    unsigned n = rnd() % 11;                                   // entries in the list
    unsigned base = (rnd() & 1) ? 0xc40 : 0xc54;
    for (unsigned i = 0; i < n && i < 10; i++)
        put16(m.dmem, base + 2 * i, uint16_t(0x4a0 + 0x28 * (rnd() % 32)));
    if (rnd() % 8 == 0 && n) put16(m.dmem, base + 2 * (rnd() % n), 0);     // a hole
    unsigned k = (n && (rnd() % 4)) ? rnd() % n : 0;
    put16(m.dmem, 0x106, uint16_t(base + 2 * k));
    put16(m.dmem, 0xb4, uint16_t(0x4a0 + 0x28 * (rnd() % 32)));
    st.sr[30] = (rnd() & 1) ? 0x09c : L.ret2;
    st.sr[24] = L.fan;
    st.sr[29] = 0x110;
    sc.exits.push_back(0x09c);
    sc.exits.push_back(L.ret2);
    sc.imem_overlay = 1;
}

static struct ClipGenInit
{
    ClipGenInit()
    {
        gens.push_back({ "spec.f3dex.clip.main", gen_clip_main });
        gens.push_back({ "spec.f3dex.clip.fan", gen_clip_fan });
    }
} clip_gen_init;
#endif
