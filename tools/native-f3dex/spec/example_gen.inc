// scenario generators of the example spec: a generator fills DMEM and the registers for one call (see rt_gen.inc for the helpers) and lists the pcs the
// oracle must stop at (sc.exits: the handler's dynamic exit pcs).
static void gen_example_geom(Machine &m, const Ucode &u, Scenario &sc)
{
    (void)u;
    auto &st = m.cpu.get_state();
    put32(m.dmem, 0x114, rnd());
    st.sr[24] = rnd();
    st.sr[29] = 0x110;
    sc.exits.push_back(0x09c);
    sc.imem_overlay = 0;
}
static struct ExampleGenInit
{
    ExampleGenInit()
    {
        gens.push_back({ "spec.f3dex.example.", gen_example_geom });
    }
} example_gen_init;
