// Differential test of the F3DEX 1.x native routines (src/native/f3dex.c) against the fork's JIT, standalone (no plugin, no RDRAM).
//
//   rt_test --ucode DIR/TAG [--routine SUBSTR] [--cases N] [--seed S] [--verbose]
//
// DIR/TAG.text (>= 0x1440 bytes) and DIR/TAG.data (0x800 bytes) are the ucode text and data as in ROM (big-endian), kept out of git.
// For every registered routine whose ucode id matches the text, N random states are built (scenario generators below), the JIT
// runs the microcode from the routine's entry until it is about to execute one of the routine's exit pcs (a BREAK is planted there), the
// routine runs from a copy of the same state, and the complete architectural state is compared (GPRs, vector registers, accumulator,
// flags, divide unit, DMEM, exit pc). A declined call must leave the state untouched. See tools/native-f3dex/README for the build.
#include "rsp_jit.hpp"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <random>
#include <string>
#include <vector>

extern "C" {
#include "native.h"
}

// the routine registry (section "armlet_routines")
extern "C" const struct armlet_routine *const __start_armlet_routines[] __attribute__((weak));
extern "C" const struct armlet_routine *const __stop_armlet_routines[] __attribute__((weak));

// side-effect helpers the routines may call: not available standalone
extern "C" {
static uint32_t *g_cr;                           // the SP / DPC registers of the machine the routine runs on
void armlet_mtc0(struct armlet_env *, unsigned rd, uint32_t v)
{
    rd &= 15;
    if (rd == 9 || rd == 8)
        v &= 0xfffffff8u;                        // DPC_END (the standalone JIT build does the same)
    if (rd == 8)
        g_cr[8] = g_cr[10] = g_cr[9] = v;
    else
        g_cr[rd] = v;
}
uint32_t armlet_mfc0(struct armlet_env *, unsigned rd, unsigned) { return g_cr[rd & 15]; }
void armlet_dma_read(struct armlet_env *, unsigned, unsigned, uint32_t) { fprintf(stderr, "rt_test: routine called armlet_dma_read\n"); abort(); }
void armlet_dma_write(struct armlet_env *, unsigned, unsigned, uint32_t) { fprintf(stderr, "rt_test: routine called armlet_dma_write\n"); abort(); }
uint64_t armlet_native_imem_hash(const uint32_t *imem, unsigned lo, unsigned hi)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned i = lo; i < hi; i += 4)
        h = (h ^ imem[i >> 2]) * 0x100000001b3ull;
    return h;
}
}

using namespace RSP;

static std::mt19937 rnd;
static bool verbose, oracle_only;
static std::vector<std::pair<unsigned, unsigned>> g_dead;   // DMEM byte ranges [lo, hi) a spec scenario declares dead (leftovers): not compared
static uint32_t g_cmp_sr;
static bool dmem_only;                           // spec.* routines: only DMEM and the exit pc are compared (what a task-level path has to reproduce)
static int stop_pc = -1;
static bool do_bench;
static FILE *out;

static uint32_t be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

struct Ucode
{
    std::vector<uint8_t> text, data;
    uint64_t id = 0;
    uint32_t main_imem[1024];                    // overlay 0 as loaded by the boot code
    uint32_t imem_ov[4][1024];                   // [k]: main + overlay k
    uint32_t data_dmem[512];                     // the initial DMEM data (0..0x800)
    bool load(const std::string &base)
    {
        auto rd = [](const std::string &p, std::vector<uint8_t> &v) {
            FILE *f = fopen(p.c_str(), "rb");
            if (!f)
                return false;
            fseek(f, 0, SEEK_END);
            v.resize(ftell(f));
            rewind(f);
            if (fread(v.data(), 1, v.size(), f) != v.size())
                v.clear();
            fclose(f);
            return !v.empty();
        };
        if (!rd(base + ".text", text) || !rd(base + ".data", data) || text.size() < 0x1440 || data.size() < 0x800)
            return false;
        uint64_t h = 0xcbf29ce484222325ull;
        for (unsigned i = 0; i < 0x1000; i += 4)
            h = (h ^ be32(&text[i])) * 0x100000001b3ull;
        id = h;
        for (unsigned i = 0; i < 512; i++)
            data_dmem[i] = be32(&data[i * 4]);
        for (unsigned k = 0; k < 4; k++)
        {
            uint8_t im[4096];
            memset(im, 0, sizeof(im));
            for (unsigned k0 = 0; k0 < 2; k0++)          // the boot code's two DMAs (the text, then the head of the code), as the table in the data says
            {
                unsigned off = be32(&data[8 * k0]);
                unsigned sz = (data[8 * k0 + 4] << 8 | data[8 * k0 + 5]) + 1;
                unsigned dst = (data[8 * k0 + 6] << 8 | data[8 * k0 + 7]) & 0xfff;
                memcpy(im + dst, &text[off], sz);
            }
            if (k)
            {
                unsigned off = be32(&data[0x10 + 8 * (k - 1)]);
                unsigned sz = (data[0x14 + 8 * (k - 1)] << 8 | data[0x15 + 8 * (k - 1)]) + 1;
                unsigned dst = (data[0x16 + 8 * (k - 1)] << 8 | data[0x17 + 8 * (k - 1)]) & 0xfff;
                memcpy(im + dst, &text[off], sz);
            }
            for (unsigned i = 0; i < 1024; i++)
                imem_ov[k][i] = be32(&im[i * 4]);
        }
        memcpy(main_imem, imem_ov[0], sizeof(main_imem));
        return true;
    }
};

// One machine = a JIT CPU plus its memories.
struct Machine
{
    JIT::CPU cpu;
    alignas(64) uint32_t dmem[1024];
    alignas(64) uint32_t imem[1024];
    uint32_t cr[16] = {};
    uint32_t irq = 0;
    Machine()
    {
        auto &st = cpu.get_state();
        cpu.set_dmem(dmem);
        cpu.set_imem(imem);
        for (int i = 0; i < 16; i++)
            st.cp0.cr[i] = &cr[i];
        st.cp0.irq = &irq;
        memset(dmem, 0, sizeof(dmem));
        memset(imem, 0, sizeof(imem));
    }
};

struct State
{
    uint32_t sr[33];
    AlignedRSPVector<1> regs[32];
    AlignedRSPVector<2> flags[3];
    AlignedRSPVector<3> acc;
    int16_t div_out, div_in;
    int8_t dp_flag;
    uint32_t dmem[1024];
};

static void capture(Machine &m, State &s)
{
    auto &st = m.cpu.get_state();
    memcpy(s.sr, st.sr, sizeof(s.sr));
    memcpy(s.regs, st.cp2.regs, sizeof(s.regs));
    memcpy(s.flags, st.cp2.flags, sizeof(s.flags));
    memcpy(&s.acc, &st.cp2.acc, sizeof(s.acc));
    s.div_out = st.cp2.div_out;
    s.div_in = st.cp2.div_in;
    s.dp_flag = st.cp2.dp_flag;
    memcpy(s.dmem, m.dmem, sizeof(s.dmem));
}

static void restore(Machine &m, const State &s)
{
    auto &st = m.cpu.get_state();
    memcpy(st.sr, s.sr, sizeof(s.sr));
    memcpy(st.cp2.regs, s.regs, sizeof(s.regs));
    memcpy(st.cp2.flags, s.flags, sizeof(s.flags));
    memcpy(&st.cp2.acc, &s.acc, sizeof(s.acc));
    st.cp2.div_out = s.div_out;
    st.cp2.div_in = s.div_in;
    st.cp2.dp_flag = s.dp_flag;
    memcpy(m.dmem, s.dmem, sizeof(s.dmem));
}

static bool diff_state(const State &a, const State &b, std::string &why)
{
    char buf[200];
    if (dmem_only)
    {
        for (int i = 1; i < 32; i++)
            if (((g_cmp_sr >> i) & 1) && a.sr[i] != b.sr[i])
            {
                snprintf(buf, sizeof(buf), "sr[%d] jit %08x spec %08x", i, a.sr[i], b.sr[i]);
                why = buf;
                return true;
            }
        for (int i = 0; i < 1024; i++)
            if (a.dmem[i] != b.dmem[i])
            {
                uint32_t diff = a.dmem[i] ^ b.dmem[i], keep = 0xffffffffu;
                for (auto &d : g_dead)                      // mask the dead bytes (host words hold big-endian words: byte address ^ 3)
                    for (unsigned by = 0; by < 4; by++)
                    {
                        unsigned addr = i * 4 + by;
                        if (addr >= d.first && addr < d.second)
                            keep &= ~(0xffu << (8 * (by ^ 3)));
                    }
                if (!(diff & keep))
                    continue;
                snprintf(buf, sizeof(buf), "dmem[0x%03x] jit %08x spec %08x", i * 4, a.dmem[i], b.dmem[i]);
                why = buf;
                return true;
            }
        return false;
    }
    if (verbose)
    {
        for (int i = 1; i < 32; i++)
            if (a.sr[i] != b.sr[i])
                fprintf(out, "    sr[%d] jit %08x native %08x\n", i, a.sr[i], b.sr[i]);
        for (int i = 0; i < 1024; i++)
            if (a.dmem[i] != b.dmem[i])
                fprintf(out, "    dmem[0x%03x] jit %08x native %08x\n", i * 4, a.dmem[i], b.dmem[i]);
        for (int r = 0; r < 32; r++)
            if (memcmp(a.regs[r].e, b.regs[r].e, 16))
                fprintf(out, "    vr[%d] differs\n", r);
    }
    for (int i = 1; i < 32; i++)
        if (a.sr[i] != b.sr[i])
        {
            snprintf(buf, sizeof(buf), "sr[%d] jit %08x native %08x", i, a.sr[i], b.sr[i]);
            why = buf;
            return true;
        }
    for (int r = 0; r < 32; r++)
        for (int i = 0; i < 8; i++)
            if (a.regs[r].e[i] != b.regs[r].e[i])
            {
                snprintf(buf, sizeof(buf), "vr[%d][%d] jit %04x native %04x", r, i, a.regs[r].e[i], b.regs[r].e[i]);
                why = buf;
                return true;
            }
    for (int i = 0; i < 24; i++)
        if (a.acc.e[i] != b.acc.e[i])
        {
            snprintf(buf, sizeof(buf), "acc[%d] (%s lane %d) jit %04x native %04x", i, i < 8 ? "hi" : i < 16 ? "md" : "lo", i & 7, a.acc.e[i], b.acc.e[i]);
            why = buf;
            return true;
        }
    for (int f = 0; f < 3; f++)
        for (int i = 0; i < 16; i++)
            if (a.flags[f].e[i] != b.flags[f].e[i])
            {
                snprintf(buf, sizeof(buf), "flags[%d][%d] jit %04x native %04x", f, i, a.flags[f].e[i], b.flags[f].e[i]);
                why = buf;
                return true;
            }
    if (a.div_out != b.div_out || a.div_in != b.div_in || a.dp_flag != b.dp_flag)
    {
        snprintf(buf, sizeof(buf), "div out/in/dp jit %04x/%04x/%d native %04x/%04x/%d", uint16_t(a.div_out), uint16_t(a.div_in), a.dp_flag,
                 uint16_t(b.div_out), uint16_t(b.div_in), b.dp_flag);
        why = buf;
        return true;
    }
    for (int i = 0; i < 1024; i++)
        if (a.dmem[i] != b.dmem[i])
        {
            snprintf(buf, sizeof(buf), "dmem[0x%03x] jit %08x native %08x", i * 4, a.dmem[i], b.dmem[i]);
            why = buf;
            return true;
        }
    return false;
}

// ---- scenario generators ------------------------------------------------------------------------------------------------------------------

static uint16_t edge16()
{
    static const uint16_t e[] = { 0, 1, 2, 0x7fff, 0x8000, 0x8001, 0xffff, 0xfffe, 0x4000, 0xc000, 0x0100, 0xff00, 0x00ff, 0x7ffe, 0x2000 };
    return e[rnd() % (sizeof(e) / sizeof(e[0]))];
}
static void put16(uint32_t *dmem, unsigned a, uint16_t v) { ((uint16_t *)dmem)[((a ^ 2) & 0xfff) >> 1] = v; }
static void put32(uint32_t *dmem, unsigned a, uint32_t v) { dmem[(a & 0xfff) >> 2] = v; }
static void put8(uint32_t *dmem, unsigned a, uint8_t v) { ((uint8_t *)dmem)[(a ^ 3) & 0xfff] = v; }
static uint16_t get16(const uint32_t *dmem, unsigned a) { return ((const uint16_t *)dmem)[((a ^ 2) & 0xfff) >> 1]; }

struct Scenario
{
    Machine *m;
    unsigned entry;
    std::vector<unsigned> exits;                 // pcs to plant BREAK at
    int imem_overlay = 0;
    uint32_t cmp_sr = 0;                         // spec scenarios: GPRs (bit n = $n) that are compared besides DMEM
    std::vector<std::pair<unsigned, unsigned>> dead;   // spec scenarios: DMEM ranges that are leftovers, not compared
    unsigned pre_entry = 0;                      // nonzero: the JIT first runs from here to the routine's entry, which makes the entry state
    unsigned items = 1;                          // vertices / triangles handled by the call (for the per-item costs)
};

static void load_constants(Machine &m, const Ucode &u)
{
    memcpy(m.dmem, u.data_dmem, sizeof(u.data_dmem));         // 0..0x7ff
    auto &st = m.cpu.get_state();
    // v31 / v30 are loaded from DMEM 0x30 / 0x40 by the init code and stay constant
    for (int i = 0; i < 8; i++)
    {
        st.cp2.regs[31].e[i] = get16(m.dmem, 0x30 + 2 * i);
        st.cp2.regs[30].e[i] = get16(m.dmem, 0x40 + 2 * i);
    }
}

static void randomize_regs(Machine &m)
{
    auto &st = m.cpu.get_state();
    for (int i = 1; i < 32; i++)
        st.sr[i] = (rnd() & 1) ? rnd() : (rnd() & 0xfff);
    for (int r = 0; r < 30; r++)
        for (int i = 0; i < 8; i++)
            st.cp2.regs[r].e[i] = (rnd() % 3) ? uint16_t(rnd()) : edge16();
    for (int f = 0; f < 3; f++)
        for (int i = 0; i < 16; i++)
            st.cp2.flags[f].e[i] = (rnd() & 1) ? 0xffff : 0;
    for (int i = 0; i < 8; i++)
        st.cp2.flags[2].e[i] = 0;
    for (int i = 0; i < 24; i++)
        st.cp2.acc.e[i] = (rnd() % 3) ? uint16_t(rnd()) : edge16();
    st.cp2.div_out = rnd();
    st.cp2.div_in = rnd();
    st.cp2.dp_flag = rnd() & 1;
    st.sr[0] = 0;
    st.sr[32] = 0;
}

typedef void (*gen_fn)(Machine &m, const Ucode &u, Scenario &sc);
static bool run_to_entry(Machine &jit, const armlet_routine *r, Scenario &sc);
struct Gen
{
    const char *prefix;                          // routine name prefix
    gen_fn fn;
};
static std::vector<Gen> gens;

// ---- the test loop -------------------------------------------------------------------------------------------------------------------------

static int run_routine(const armlet_routine *r, const Ucode &u, gen_fn gen, unsigned cases)
{
    Machine jit, nat;
    unsigned declined = 0, bad = 0, ok = 0, skipped = 0;
    std::vector<unsigned> exit_hist;
    for (unsigned c = 0; c < cases; c++)
    {
        Scenario sc{ &jit, r->entry_pc, {}, 0 };
        memset(jit.dmem, 0, sizeof(jit.dmem));
        memset(&jit.cpu.get_state().cp2, 0, sizeof(jit.cpu.get_state().cp2));
        load_constants(jit, u);
        randomize_regs(jit);
        gen(jit, u, sc);
        g_dead = sc.dead;
        g_cmp_sr = sc.cmp_sr;
        if (sc.pre_entry == r->entry_pc)
            sc.pre_entry = 0;
        memcpy(jit.imem, u.imem_ov[sc.imem_overlay], sizeof(jit.imem));
        for (int i = 0; i < 16; i++)
            jit.cr[i] = nat.cr[i] = rnd();
        jit.cr[4] = nat.cr[4] = jit.cr[5] = nat.cr[5] = jit.cr[6] = nat.cr[6] = jit.cr[11] = nat.cr[11] = 0;
        if (sc.pre_entry)
        {
            if (!run_to_entry(jit, r, sc))
            {
                skipped++;
                memcpy(jit.imem, u.imem_ov[sc.imem_overlay], sizeof(jit.imem));
                continue;
            }
            memcpy(jit.imem, u.imem_ov[sc.imem_overlay], sizeof(jit.imem));
            nat.cr[9] = jit.cr[9]; for (int i = 0; i < 16; i++) nat.cr[i] = jit.cr[i];
        }
        State snap, after_jit, after_nat;
        capture(jit, snap);

        // native from a copy of the snapshot
        memcpy(nat.imem, jit.imem, sizeof(nat.imem));
        restore(nat, snap);
        struct armlet_env env;
        memset(&env, 0, sizeof(env));
        auto &ns = nat.cpu.get_state();
        env.rsp.sr = ns.sr;
        env.rsp.vr = reinterpret_cast<uint16_t(*)[8]>(&ns.cp2.regs[0]);
        env.rsp.acc = ns.cp2.acc.e;
        env.rsp.flags = reinterpret_cast<uint16_t(*)[16]>(&ns.cp2.flags[0]);
        env.rsp.div_out = &ns.cp2.div_out;
        env.rsp.div_in = &ns.cp2.div_in;
        env.rsp.dp_flag = &ns.cp2.dp_flag;
        env.rsp.dmem = nat.dmem;
        env.rsp.imem = nat.imem;
        env.entry_pc = r->entry_pc;
        g_cr = nat.cr;
        env.mode = 1;
        env.routine = r;
        int pc = oracle_only ? 0 : r->run(&env);
        capture(nat, after_nat);
        std::string why;
        if (!oracle_only && pc == ARMLET_NATIVE_DECLINE)
        {
            declined++;
            if (diff_state(snap, after_nat, why))
            {
                fprintf(out, "case %u: DECLINE changed the state: %s\n", c, why.c_str());
                bad++;
            }
            continue;
        }
        // oracle: BREAK at every exit pc, run from the entry
        for (unsigned i = 0; i < r->n_exit_pcs; i++)
            sc.exits.push_back(r->exit_pcs[i]);
        for (unsigned x : sc.exits)
            jit.imem[x >> 2] = 0x0000000d;
        if (stop_pc >= 0)
            jit.imem[stop_pc >> 2] = 0x0000000d;
        {
            auto &st = jit.cpu.get_state();
            st.pc = r->entry_pc;
            st.has_delay_slot = 0;
            st.branch_target = 0;
            st.dirty_blocks = 0xffffffffu;
            jit.cpu.invalidate_imem();
            jit.cr[CP0_REGISTER_SP_STATUS] = 0;
            ReturnMode rm = jit.cpu.run();
            if (rm != MODE_BREAK)
            {
                fprintf(out, "case %u: oracle ended with mode %d\n", c, int(rm));
                bad++;
                continue;
            }
        }
        capture(jit, after_jit);
        unsigned jit_pc = jit.cpu.get_state().pc;
        if (oracle_only)
        {
            if (stop_pc >= 0)
            {
                auto &js = jit.cpu.get_state();
                if (const char *vd = getenv("VDUMP"))
                    for (const char *q = vd; *q;)
                    {
                        int r = atoi(q);
                        fprintf(out, "  v%d:", r);
                        for (int i = 0; i < 8; i++) fprintf(out, " %04x", js.cp2.regs[r].e[i]);
                        fprintf(out, "\n");
                        while (*q && *q != ',') q++;
                        if (*q == ',') q++;
                    }
                fprintf(out, "case %u stop pc %03x: r7=%u r9=%d r10=%d r11=%d r12=%d r17=%d r15=%08x\n", c, jit_pc, js.sr[7], (int)js.sr[9], (int)js.sr[10], (int)js.sr[11], (int)js.sr[12], (int)js.sr[17], js.sr[15]);
            }
            exit_hist.push_back(jit_pc);
            ok++;
            continue;
        }
        if (jit_pc != unsigned(pc) && jit_pc != unsigned(pc) + 4 && jit_pc + 4 != unsigned(pc))
        {
            fprintf(out, "case %u: exit pc jit %03x native %03x\n", c, jit_pc, unsigned(pc));
            if (verbose)
                for (int i = 1; i < 32; i++)
                    fprintf(out, "    jit sr[%d]=%08x snap %08x\n", i, after_jit.sr[i], snap.sr[i]);
            bad++;
            continue;
        }
        jit.cr[4] = nat.cr[4];                   // the planted BREAK set BROKE | HALT
        if (memcmp(jit.cr, nat.cr, sizeof(jit.cr)))
        {
            fprintf(out, "case %u: SP/DPC registers differ\n", c);
            for (int i = 0; i < 16; i++)
                if (jit.cr[i] != nat.cr[i])
                    fprintf(out, "    cr[%d] jit %08x native %08x\n", i, jit.cr[i], nat.cr[i]);
            bad++;
            continue;
        }
        if (diff_state(after_jit, after_nat, why))
        {
            fprintf(out, "case %u: native differs: %s (exit %03x)\n", c, why.c_str(), unsigned(pc));
            bad++;
            continue;
        }
        ok++;
        exit_hist.push_back(unsigned(pc));
    }
    fprintf(out, "%s: %u cases: %u equal, %u declined, %u BAD%s\n", r->name, cases, ok, declined, bad,
            skipped ? (std::string(", ") + std::to_string(skipped) + " ended before the entry").c_str() : "");
    if (verbose)
    {
        std::vector<std::pair<unsigned, unsigned>> h;
        for (unsigned p : exit_hist)
        {
            bool f = false;
            for (auto &e : h)
                if (e.first == p) { e.second++; f = true; }
            if (!f) h.push_back({ p, 1 });
        }
        for (auto &e : h)
            fprintf(out, "   exit %03x: %u\n", e.first, e.second);
    }
    return bad != 0;
}


#include <time.h>
static double now_ns()
{
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e9 + t.tv_nsec;
}

// Native vs JIT cost per call over a set of random scenarios (no comparison here: run the equality test for that).
static void bench_routine(const armlet_routine *r, const Ucode &u, gen_fn gen, unsigned K, double seconds)
{
    Machine jit, nat;
    struct Sc { State st; std::vector<unsigned> exits; int ov; int pc; unsigned items; };
    std::vector<Sc> scs;
    for (unsigned guard = 0; scs.size() < K && guard < K * 20; guard++)
    {
        Scenario sc{ &jit, r->entry_pc, {}, 0 };
        memset(jit.dmem, 0, sizeof(jit.dmem));
        memset(&jit.cpu.get_state().cp2, 0, sizeof(jit.cpu.get_state().cp2));
        load_constants(jit, u);
        randomize_regs(jit);
        gen(jit, u, sc);
        if (sc.pre_entry == r->entry_pc)
            sc.pre_entry = 0;
        for (int i = 0; i < 16; i++)
            jit.cr[i] = nat.cr[i] = 0;
        memcpy(jit.imem, u.imem_ov[sc.imem_overlay], sizeof(jit.imem));
        if (sc.pre_entry)
        {
            bool ok = run_to_entry(jit, r, sc);
            memcpy(jit.imem, u.imem_ov[sc.imem_overlay], sizeof(jit.imem));
            if (!ok)
                continue;
            for (int i = 0; i < 16; i++) nat.cr[i] = jit.cr[i];
        }
        Sc s;
        capture(jit, s.st);
        memcpy(nat.imem, u.imem_ov[sc.imem_overlay], sizeof(nat.imem));
        restore(nat, s.st);
        struct armlet_env env;
        memset(&env, 0, sizeof(env));
        auto &ns = nat.cpu.get_state();
        env.rsp.sr = ns.sr; env.rsp.vr = reinterpret_cast<uint16_t(*)[8]>(&ns.cp2.regs[0]); env.rsp.acc = ns.cp2.acc.e;
        env.rsp.flags = reinterpret_cast<uint16_t(*)[16]>(&ns.cp2.flags[0]); env.rsp.div_out = &ns.cp2.div_out; env.rsp.div_in = &ns.cp2.div_in;
        env.rsp.dp_flag = &ns.cp2.dp_flag; env.rsp.dmem = nat.dmem; env.rsp.imem = nat.imem; env.entry_pc = r->entry_pc; env.mode = 1; env.routine = r;
        g_cr = nat.cr;
        int pc = r->run(&env);
        if (pc == ARMLET_NATIVE_DECLINE)
            continue;
        s.pc = pc;
        s.ov = sc.imem_overlay;
        s.items = sc.items;
        s.exits = sc.exits;
        for (unsigned i = 0; i < r->n_exit_pcs; i++)
            s.exits.push_back(r->exit_pcs[i]);
        scs.push_back(s);
    }
    if (scs.empty())
        return;
    // groups by exit pc
    std::vector<unsigned> keys;
    for (auto &s : scs)
    {
        bool f = false;
        for (unsigned k : keys) f |= (k == unsigned(s.pc));
        if (!f) keys.push_back(s.pc);
    }
    keys.push_back(~0u);                         // all
    for (unsigned key : keys)
    {
        std::vector<Sc *> sel;
        for (auto &s : scs)
            if (key == ~0u || unsigned(s.pc) == key) sel.push_back(&s);
        double ns_nat = 0, ns_jit = 0, ns_rest = 0;
        // restore-only loop
        auto timed = [&](int mode) {
            unsigned reps = 1;
            for (;;)
            {
                double t0 = now_ns();
                for (unsigned rep = 0; rep < reps; rep++)
                    for (Sc *s : sel)
                    {
                        if (mode == 0)
                        {
                            restore(nat, s->st);
                        }
                        else if (mode == 1)
                        {
                            restore(nat, s->st);
                            struct armlet_env env;
                            memset(&env, 0, sizeof(env));
                            auto &ns = nat.cpu.get_state();
                            env.rsp.sr = ns.sr; env.rsp.vr = reinterpret_cast<uint16_t(*)[8]>(&ns.cp2.regs[0]); env.rsp.acc = ns.cp2.acc.e;
                            env.rsp.flags = reinterpret_cast<uint16_t(*)[16]>(&ns.cp2.flags[0]); env.rsp.div_out = &ns.cp2.div_out; env.rsp.div_in = &ns.cp2.div_in;
                            env.rsp.dp_flag = &ns.cp2.dp_flag; env.rsp.dmem = nat.dmem; env.rsp.imem = nat.imem; env.entry_pc = r->entry_pc; env.mode = 1; env.routine = r;
                            g_cr = nat.cr;
                            volatile int rv = r->run(&env);
                            (void)rv;
                        }
                        else
                        {   // modes 2, 3
                            restore(jit, s->st);
                            memcpy(jit.imem, u.imem_ov[s->ov], sizeof(jit.imem));
                            if (mode == 3)
                                jit.imem[r->entry_pc >> 2] = 0x0000000d;     // a call that stops at once: the cost of run() and the register dump
                            else
                                for (unsigned x : s->exits) jit.imem[x >> 2] = 0x0000000d;
                            auto &st = jit.cpu.get_state();
                            st.pc = r->entry_pc; st.has_delay_slot = 0; st.branch_target = 0;
                            jit.cr[4] = 0;
                            jit.cpu.run();
                        }
                    }
                double t1 = now_ns();
                if (t1 - t0 > seconds * 1e9 || reps > (1u << 22))
                    return (t1 - t0) / (double(reps) * sel.size());
                reps *= 2;
            }
        };
        // jit: plant the breaks once so the compiled blocks are reused
        memcpy(jit.imem, u.imem_ov[sel[0]->ov], sizeof(jit.imem));
        ns_rest = timed(0);
        ns_nat = timed(1) - ns_rest;
        if (getenv("RT_NOJIT"))
        {
            fprintf(out, "  %s exit 0x%03x: native %.0f ns/call\n", r->name, key == ~0u ? 0u : key, ns_nat);
            continue;
        }
        // the JIT restore copies also IMEM (4 KB) each call; subtract the same plus that
        {
            double t0 = now_ns();
            unsigned reps = 20000;
            for (unsigned i = 0; i < reps; i++)
            {
                restore(jit, sel[i % sel.size()]->st);
                memcpy(jit.imem, u.imem_ov[sel[0]->ov], sizeof(jit.imem));
                for (unsigned x : sel[0]->exits) jit.imem[x >> 2] = 0x0000000d;
            }
            double rest_j = (now_ns() - t0) / reps;
            ns_jit = timed(2) - rest_j;
            double null_call = timed(3) - rest_j;
            ns_jit -= null_call;
        }
        double items = 0;
        for (Sc *s : sel) items += s->items;
        items /= sel.size();
        fprintf(out, "  %s exit %s: %zu scenarios, %.1f items/call  native %.0f ns/call (%.1f ns/item)   JIT %.0f ns/call (%.1f ns/item)   x%.1f\n", r->name,
                key == ~0u ? "all" : (std::string("0x") + [&]{ char b[16]; snprintf(b, sizeof b, "%03x", key); return std::string(b); }()).c_str(),
                sel.size(), items, ns_nat, ns_nat / items, ns_jit, ns_jit / items, ns_jit / ns_nat);
    }
}


// Runs the JIT from sc.pre_entry to the routine's entry (a BREAK planted there, and at every exit the routine or the generator knows);
// false when the run ended elsewhere (the triangle was culled before the entry, ...), true with the machine at the entry otherwise.
static bool run_to_entry(Machine &jit, const armlet_routine *r, Scenario &sc)
{
    std::vector<unsigned> stops = sc.exits;
    for (unsigned i = 0; i < r->n_exit_pcs; i++)
        stops.push_back(r->exit_pcs[i]);
    uint32_t save = jit.imem[r->entry_pc >> 2];
    for (unsigned x : stops)
        jit.imem[x >> 2] = 0x0000000d;
    jit.imem[r->entry_pc >> 2] = 0x0000000d;
    auto &st = jit.cpu.get_state();
    st.pc = sc.pre_entry; st.has_delay_slot = 0; st.branch_target = 0; st.dirty_blocks = 0xffffffffu;
    jit.cpu.invalidate_imem();
    jit.cr[4] = 0;
    ReturnMode rm = jit.cpu.run();
    bool ok = rm == MODE_BREAK && st.pc == r->entry_pc + 4;
    // IMEM is restored by the caller (the BREAKs were planted for this run only)
    (void)save;
    jit.cpu.invalidate_imem();
    st.pc = r->entry_pc; st.has_delay_slot = 0; st.branch_target = 0;
    jit.cr[4] = 0;
    return ok;
}

// scenario generators live in rt_gen.inc (per routine family)
#include "rt_gen.inc"
#include "spec_gens.inc"

int main(int argc, char **argv)
{
    std::string base, sub;
    unsigned cases = 2000, seed = 1;
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        if (a == "--ucode" && i + 1 < argc) base = argv[++i];
        else if (a == "--routine" && i + 1 < argc) sub = argv[++i];
        else if (a == "--cases" && i + 1 < argc) cases = atoi(argv[++i]);
        else if (a == "--seed" && i + 1 < argc) seed = atoi(argv[++i]);
        else if (a == "--verbose") verbose = true;
        else if (a == "--oracle") oracle_only = true;
        else if (a == "--bench") do_bench = true;
        else if (a == "--stop" && i + 1 < argc) stop_pc = strtol(argv[++i], 0, 16);
    }
    if (base.empty())
    {
        fprintf(stderr, "usage: rt_test --ucode DIR/TAG [--routine SUBSTR] [--cases N] [--seed S] [--verbose]\n");
        return 2;
    }
    out = fdopen(dup(1), "w");           // BREAK makes the JIT print its register file on stdout
    if (!freopen("/dev/null", "w", stdout)) {}
    rnd.seed(seed);
    static Ucode u;
    if (!u.load(base))
    {
        fprintf(out, "cannot load %s.text/.data\n", base.c_str());
        return 2;
    }
    fprintf(out, "ucode id %016llx\n", (unsigned long long)u.id);
    int rc = 0;
    unsigned n = 0;
    for (auto p = __start_armlet_routines; p && p != __stop_armlet_routines; p++)
    {
        const armlet_routine *r = *p;
        dmem_only = !strncmp(r->name, "spec.", 5);
        if (r->ucode != u.id || (!sub.empty() && !strstr(r->name, sub.c_str())))
            continue;
        gen_fn g = nullptr;
        for (auto &x : gens)
            if (!strncmp(r->name, x.prefix, strlen(x.prefix)))
                g = x.fn;
        if (!g)
        {
            fprintf(out, "%s: no scenario generator\n", r->name);
            rc = 1;
            continue;
        }
        n++;
        if (do_bench)
            bench_routine(r, u, g, 64, 0.3);
        else
            rc |= run_routine(r, u, g, cases);
    }
    if (!n)
        fprintf(out, "no routine for this ucode id\n");
    return rc;
}
