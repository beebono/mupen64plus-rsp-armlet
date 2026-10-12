// scenarios of the struct-level clipper: clip_gen.inc's entry states of the real microcode (the overlay entry), plus
//  - main: the generator first runs the real microcode on the JIT from the entry to learn how the stretch ends (first triangle or the caller's return) and which list buffer holds the polygon, to
//    declare the leftovers dead (the scratch buffer, the scan copies behind the final list's terminator, 0x158; for a discarded polygon also the lists, 0xb4, 0x106) and to compare $1 $2 $3 only
//    when a triangle is handed over;
//  - fan: states real fan re-entries have (the microcode runs through the first triangle and some fan steps; one of the states a step is entered in is picked, the last one returns to the caller).
#include "clip_gen.inc"

// runs the JIT from `from` to the first of `stops` (BREAK planted for the run) and returns that pc (~0 if it did not stop at one)
static unsigned fxclip_run(Machine &m, const Ucode &u, int ov, unsigned from, const std::vector<unsigned> &stops)
{
    memcpy(m.imem, u.imem_ov[ov], sizeof(m.imem));
    for (unsigned x : stops)
        m.imem[x >> 2] = 0x0000000d;
    auto &st = m.cpu.get_state();
    st.pc = from; st.has_delay_slot = 0; st.branch_target = 0; st.dirty_blocks = 0xffffffffu;
    m.cpu.invalidate_imem();
    m.cr[4] = 0;
    ReturnMode rm = m.cpu.run();
    unsigned pc = rm == MODE_BREAK ? st.pc - 4 : ~0u;
    memcpy(m.imem, u.imem_ov[ov], sizeof(m.imem));
    m.cpu.invalidate_imem();
    m.cr[4] = 0;
    return pc;
}

static void gen_fxclip_main(Machine &m, const Ucode &u, Scenario &sc)
{
    const ClipLay L = clip_lay_of(u);
    static State s0, s1;
    Scenario tmp{ &m, L.entry, {}, 0 };
    gen_clip_main(m, u, tmp);
    sc.exits = tmp.exits;
    sc.imem_overlay = tmp.imem_overlay;
    capture(m, s0);
    unsigned pc = fxclip_run(m, u, sc.imem_overlay, L.entry, { L.tri, 0x09c, L.ret2 });
    capture(m, s1);
    restore(m, s0);
    sc.dead.push_back({ 0x158, 0x15a });
    if (pc == L.tri)
    {
        sc.cmp_sr = 0xe;                                            // $1 $2 $3 of the first triangle
        unsigned base = get16(s1.dmem, 0x106) - 2, n = 0;
        while (n < 10 && get16(s1.dmem, base + 2 * n)) n++;
        sc.dead.push_back({ 0xc40, base });
        sc.dead.push_back({ base + 2 * (n + 1), 0xc68 });
    }
    else
    {
        sc.dead.push_back({ 0xc40, 0xc68 });
        sc.dead.push_back({ 0xb4, 0xb6 });
        sc.dead.push_back({ 0x106, 0x108 });
    }
}

static void gen_fxclip_fan(Machine &m, const Ucode &u, Scenario &sc)
{
    const ClipLay L = clip_lay_of(u);
    static State snaps[16];
    for (int attempt = 0; attempt < 500; attempt++)
    {
        Scenario tmp{ &m, L.entry, {}, 0 };
        gen_clip_main(m, u, tmp);
        sc.exits = tmp.exits;
        sc.imem_overlay = tmp.imem_overlay;
        unsigned pc = fxclip_run(m, u, sc.imem_overlay, L.entry, { L.tri, 0x09c, L.ret2 });
        if (pc != L.tri)
            continue;
        unsigned ns = 0;
        bool last_is_tri = false;
        for (;;)
        {
            capture(m, snaps[ns++]);                                // the state the fan step is entered in
            pc = fxclip_run(m, u, sc.imem_overlay, L.fan, { L.tri, 0x09c, L.ret2 });
            last_is_tri = pc == L.tri;
            if (!last_is_tri || ns >= 12)
                break;
        }
        if (pc != L.tri && pc != 0x09c && pc != L.ret2)
            continue;
        unsigned pick = rnd() % ns;                                 // the last snapshot's step returns to the caller
        restore(m, snaps[pick]);
        sc.cmp_sr = (pick + 1 < ns || last_is_tri) ? 0xe : 0;
        return;
    }
    fprintf(stderr, "fxclip fan generator: no state\n");
    abort();
}

static struct FxClipGenInit
{
    FxClipGenInit()
    {
        gens.push_back({ "spec.f3dex.fxclip.main", gen_fxclip_main });
        gens.push_back({ "spec.f3dex.fxclip.fan", gen_fxclip_fan });
    }
} fxclip_gen_init;
