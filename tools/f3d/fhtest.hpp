/*
 * arMLEt - tools/f3d/fhtest.hpp: ucode loading, native-routine environment and state comparison for the F3D routine tests.
 *
 * The microcode comes from a profiler dump of a local run (never in the repository): $F3D_UCODE is the path prefix of
 * <id>.text / <id>.imem / <id>.dmem (docs/profiling.md, <out>/prof/ucode/).
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef FHTEST_HPP
#define FHTEST_HPP
#include "fh.hpp"
#include <random>
#include <stdarg.h>

extern "C" {
#include "native.h"
}

extern FH *g_fh;
#ifdef F3D_TRACE
#include "prsp_hooks.h"
// instruction trace: ring of the last executed pcs (the JIT's per-instruction hook; builds with TRACE=1)
static std::vector<unsigned> g_ring(1 << 16);
static unsigned g_ring_n = 0;
static unsigned h_site(unsigned pc, uint32_t) { return pc; }
static void h_hit(unsigned site) { g_ring[g_ring_n++ & 0xffff] = site; }
inline void trace_install() { PrspHooks hk = {}; hk.site = h_site; hk.hit = h_hit; prsp_hooks_install(hk); }
inline void trace_dump(unsigned n)
{
    unsigned total = g_ring_n < 0x10000 ? g_ring_n : 0x10000;
    if (n > total) n = total;
    for (unsigned i = g_ring_n - n; i != g_ring_n; i++) printf(" %x", g_ring[i & 0xffff]);
    printf("\n");
}
#endif
FH *FH::self = nullptr;

struct Ucode
{
    std::vector<uint8_t> text, imem, dmem;
    unsigned off0 = 0;                 // text offset of overlay 0
    unsigned ov_base = 0x768;          // the IMEM window of the overlays (the build's)
    uint32_t d[4][2];                  // descriptors (dram, len/mem word)
    void load(const char *prefix)
    {
        text = FH::readfile(std::string(prefix) + ".text");
        imem = FH::readfile(std::string(prefix) + ".imem");
        dmem = FH::readfile(std::string(prefix) + ".dmem");
        for (int i = 0; i < 4; i++) {
            d[i][0] = rw(dmem, 8 + 8 * i);
            d[i][1] = rw(dmem, 12 + 8 * i);
        }
        // find overlay 0 (IMEM 0 .. its length) in the text
        unsigned len0 = (d[0][1] >> 16) + 1;
        for (unsigned o = 0; o + len0 <= text.size(); o += 4)
            if (!memcmp(&text[o], &imem[0], len0 > 0x40 ? 0x40 : len0)) {
                off0 = o;
                break;
            }
    }
    static uint32_t rw(const std::vector<uint8_t> &v, unsigned a) { return *(const uint32_t *)&v[a]; } // host-order word = big-endian value
    unsigned ov_off(int k) const { return off0 + (d[k][0] - d[0][0]); }
    unsigned ov_len(int k) const { return (d[k][1] >> 16) + 1; }
    // IMEM image with overlay k at 0x768 (k = 0: leave the dumped one)
    void make_imem(uint32_t *imem_out, int k) const
    {
        memcpy(imem_out, imem.data(), 4096);
        if (k) {
            unsigned len = ov_len(k);
            memcpy((uint8_t *)imem_out + ov_base, &text[ov_off(k)], len);
        }
    }
};

// ---- the native routine's environment ---------------------------------------------------------------------------------------------------
extern "C" {
int RSP_MTC0(RSP::CPUState *rsp, unsigned rd, unsigned rt);
int RSP_MFC0(RSP::CPUState *rsp, unsigned rt, unsigned rd);
}

inline void make_env(armlet_env &e, FH &h, unsigned entry)
{
    RSP::CPUState &c = h.st();
    memset(&e, 0, sizeof e);
    e.rsp.sr = c.sr;
    e.rsp.vr = (uint16_t(*)[8])c.cp2.regs[0].e;
    e.rsp.acc = c.cp2.acc.e;
    e.rsp.flags = (uint16_t(*)[16])c.cp2.flags[0].e;
    e.rsp.div_out = &c.cp2.div_out;
    e.rsp.div_in = &c.cp2.div_in;
    e.rsp.dp_flag = &c.cp2.dp_flag;
    e.rsp.dmem = h.dmem;
    e.rsp.imem = h.imem;
    e.entry_pc = entry;
    e.mode = PRSP_NATIVE_MODE_CONTINUE;
}

// ---- comparison ------------------------------------------------------------------------------------------------------------------------
struct Diff
{
    int n = 0;
    std::string text;
    void add(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
    {
        n++;
        if (text.size() > 4000) return;
        char b[256];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(b, sizeof b, fmt, ap);
        va_end(ap);
        text += b;
        text += "\n";
    }
};

struct Masks { uint32_t dead_sr = 0, dead_vr = 0; bool dead_acc = false, dead_flags = false, dead_div = false; bool dead_flag[3] = {false, false, false}; };

inline void compare(const Snap &a, const Snap &b, Diff &d, const Masks &m = Masks())
{
    for (int i = 1; i < 32; i++)
        if (!(m.dead_sr >> i & 1) && a.sr[i] != b.sr[i]) d.add("sr[%d] jit %08x native %08x", i, a.sr[i], b.sr[i]);
    for (int i = 0; i < 32; i++)
        if (!(m.dead_vr >> i & 1) && memcmp(a.vr[i], b.vr[i], 16)) {
            char x[128], y[128];
            for (int k = 0; k < 8; k++) { sprintf(x + 5 * k, "%04x ", a.vr[i][k]); sprintf(y + 5 * k, "%04x ", b.vr[i][k]); }
            d.add("vr[%d] jit %s native %s", i, x, y);
        }
    if (!m.dead_acc && memcmp(a.acc, b.acc, sizeof a.acc)) {
        for (int k = 0; k < 24; k++) if (a.acc[k] != b.acc[k]) d.add("acc[%d] jit %04x native %04x", k, a.acc[k], b.acc[k]);
    }
    for (int f = 0; f < 3; f++)
        if (!m.dead_flags && !m.dead_flag[f])
            if (memcmp(a.flags[f], b.flags[f], 32)) {
                for (int k = 0; k < 16; k++) if (a.flags[f][k] != b.flags[f][k]) d.add("flags[%d][%d] jit %04x native %04x", f, k, a.flags[f][k], b.flags[f][k]);
            }
    if (!m.dead_div) {
        if (a.div_out != b.div_out) d.add("div_out jit %04x native %04x", (uint16_t)a.div_out, (uint16_t)b.div_out);
        if (a.div_in != b.div_in) d.add("div_in jit %04x native %04x", (uint16_t)a.div_in, (uint16_t)b.div_in);
        if (a.dp_flag != b.dp_flag) d.add("dp_flag jit %d native %d", a.dp_flag, b.dp_flag);
    }
    for (int i = 0; i < 1024; i++)
        if (a.dmem[i] != b.dmem[i]) d.add("dmem[%03x] jit %08x native %08x", i * 4, a.dmem[i], b.dmem[i]);
    for (int i = 0; i < 1024; i++)
        if (a.imem[i] != b.imem[i]) d.add("imem[%03x] jit %08x native %08x", i * 4, a.imem[i], b.imem[i]);
    for (int i = 0; i < 16; i++)
        if (a.cr[i] != b.cr[i]) d.add("cr[%d] jit %08x native %08x", i, a.cr[i], b.cr[i]);
    if (a.irq != b.irq) d.add("irq jit %x native %x", a.irq, b.irq);
}

// vectors of the constant registers the ucode keeps in v30 / v31
inline void load_consts(FH &h)
{
    RSP::CPUState &c = h.st();
    for (int i = 0; i < 8; i++) {
        c.cp2.regs[31].e[i] = h.dm16(0x30 + 2 * i);
        c.cp2.regs[30].e[i] = h.dm16(0x40 + 2 * i);
    }
}

// Random generator helpers
struct Rng
{
    std::mt19937_64 g;
    Rng(uint64_t s) : g(s) {}
    uint32_t u32() { return (uint32_t)g(); }
    uint32_t below(uint32_t n) { return (uint32_t)(g() % n); }
    int range(int lo, int hi) { return lo + (int)below((uint32_t)(hi - lo + 1)); }
    bool chance(unsigned pct) { return below(100) < pct; }
    int16_t s16() { return (int16_t)u32(); }
};
#endif
