// Exactness test of src/native/f3dex_vu.h against the fork's helpers (ext/parallel-rsp rsp/vfunctions.cpp, ls.cpp, cp2.cpp).
//
//   vu_test [-n TRIALS] [-s SEED] [-o OPNAME[,OPNAME...]] [-v]
//
// Per instruction and per element selector E (0..15) TRIALS random trials (default 20000, or $VU_TEST_N); every trial runs the library op
// on a `struct vu` loaded from a state and the fork's RSP_* helper on the same state, then compares the complete state (32 registers,
// accumulator, VCO / VCC / VCE, div_out / div_in / dp_flag, all 4096 DMEM bytes, the GPRs). Register numbers are template constants: each
// op is instantiated with 8 (vd, vs, vt) triples that cover vd == vs, vd == vt, vs == vt, all equal and the register range ends.
// See build_vu_test.sh for how to build and run it.
#include "state.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <utility>

extern "C" {
#define VU_DECL(n) void RSP_##n(RSP::CPUState *, unsigned vd, unsigned vs, unsigned vt, unsigned e);
VU_DECL(VMULF) VU_DECL(VMULU) VU_DECL(VMUDL) VU_DECL(VMUDM) VU_DECL(VMUDN) VU_DECL(VMUDH) VU_DECL(VMACF) VU_DECL(VMACU)
VU_DECL(VMADL) VU_DECL(VMADM) VU_DECL(VMADN) VU_DECL(VMADH) VU_DECL(VABS) VU_DECL(VADD) VU_DECL(VADDC) VU_DECL(VSUB) VU_DECL(VSUBC)
VU_DECL(VAND) VU_DECL(VNAND) VU_DECL(VOR) VU_DECL(VNOR) VU_DECL(VXOR) VU_DECL(VNXOR) VU_DECL(VEQ) VU_DECL(VNE) VU_DECL(VGE) VU_DECL(VLT)
VU_DECL(VCH) VU_DECL(VCL) VU_DECL(VCR) VU_DECL(VMRG) VU_DECL(VSAR) VU_DECL(RESERVED) VU_DECL(VMOV) VU_DECL(VRCP) VU_DECL(VRCPL)
VU_DECL(VRCPH) VU_DECL(VRSQ) VU_DECL(VRSQL) VU_DECL(VRSQH)
#define LS_DECL(n) void RSP_##n(RSP::CPUState *, unsigned rt, unsigned e, int offset, unsigned base);
LS_DECL(LBV) LS_DECL(LSV) LS_DECL(LLV) LS_DECL(LDV) LS_DECL(LQV) LS_DECL(LPV) LS_DECL(LUV) LS_DECL(LTV)
LS_DECL(SBV) LS_DECL(SSV) LS_DECL(SLV) LS_DECL(SDV) LS_DECL(SQV) LS_DECL(SPV) LS_DECL(SUV) LS_DECL(STV)
void RSP_CFC2(RSP::CPUState *, unsigned rt, unsigned rd);
void RSP_CTC2(RSP::CPUState *, unsigned rt, unsigned rd);
void RSP_MTC2(RSP::CPUState *, unsigned rt, unsigned rd, unsigned element);
void RSP_MFC2(RSP::CPUState *, unsigned rt, unsigned rd, unsigned element);
}

#include "../../src/native/f3dex_vu.h"

namespace {

// ---- random numbers and generators ------------------------------------------------------------------------------------------------
struct Rng {
	uint64_t s[2];
	explicit Rng(uint64_t seed) { s[0] = seed * 0x9e3779b97f4a7c15ull + 1; s[1] = seed ^ 0xd1b54a32d192ed03ull; for (int i = 0; i < 8; i++) next(); }
	uint64_t next() { uint64_t a = s[0], b = s[1]; s[0] = b; a ^= a << 23; s[1] = a ^ b ^ (a >> 17) ^ (b >> 26); return s[1] + b; }
	uint32_t u32() { return (uint32_t)(next() >> 32); }
	uint32_t below(uint32_t n) { return (uint32_t)(((next() >> 32) * (uint64_t)n) >> 32); }
};

const uint16_t kEdge[] = { 0, 1, 2, 3, 0x7fff, 0x8000, 0x8001, 0xffff, 0xfffe, 0xfffd, 0x4000, 0xc000, 0x00ff, 0xff00, 0x0100, 0x7ffe, 0x8002,
                           0x5555, 0xaaaa, 0x0080, 0xff80, 0x2000, 0x1fff, 0xe000 };

uint16_t lane(Rng &r)
{
	uint32_t k = r.below(100);
	if (k < 30) return kEdge[r.below(sizeof kEdge / sizeof kEdge[0])];
	if (k < 55) return (uint16_t)((int)r.below(33) - 16);
	if (k < 72) { uint16_t v = (uint16_t)(r.u32() >> (r.below(16) + 16)); return (r.u32() & 1) ? (uint16_t)-v : v; }   // random bit length
	return (uint16_t)r.u32();
}

const uint8_t kSel[16][8] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 0, 2, 2, 4, 4, 6, 6 }, { 1, 1, 3, 3, 5, 5, 7, 7 },
	{ 0, 0, 0, 0, 4, 4, 4, 4 }, { 1, 1, 1, 1, 5, 5, 5, 5 }, { 2, 2, 2, 2, 6, 6, 6, 6 }, { 3, 3, 3, 3, 7, 7, 7, 7 },
	{ 0, 0, 0, 0, 0, 0, 0, 0 }, { 1, 1, 1, 1, 1, 1, 1, 1 }, { 2, 2, 2, 2, 2, 2, 2, 2 }, { 3, 3, 3, 3, 3, 3, 3, 3 },
	{ 4, 4, 4, 4, 4, 4, 4, 4 }, { 5, 5, 5, 5, 5, 5, 5, 5 }, { 6, 6, 6, 6, 6, 6, 6, 6 }, { 7, 7, 7, 7, 7, 7, 7, 7 },
};

// vt first; vs then often related to the lane of vt[E] it meets (equal, negated, complemented, +-1): VCH / VCL / VCR / VEQ ... corner cases
void gen_pair(Rng &r, uint16_t *vs, uint16_t *vt, int e)
{
	for (int i = 0; i < 8; i++) vt[i] = lane(r);
	for (int i = 0; i < 8; i++) {
		uint16_t t = vt[kSel[e][i]];
		uint32_t k = r.below(100);
		if (k < 12) vs[i] = t;
		else if (k < 24) vs[i] = (uint16_t)-t;
		else if (k < 30) vs[i] = (uint16_t)~t;
		else if (k < 36) vs[i] = (uint16_t)(t + 1);
		else if (k < 42) vs[i] = (uint16_t)(t - 1);
		else vs[i] = lane(r);
	}
}

void gen_acc(Rng &r, uint16_t *acc)
{
	for (int i = 0; i < 8; i++) {
		uint32_t k = r.below(100);
		uint16_t hi, md, lo;
		lo = (k < 35) ? (uint16_t)(r.below(2) ? 0xffff : 0) : lane(r);
		md = (k % 7 < 2) ? (uint16_t)(r.below(2) ? 0xffff : 0) : lane(r);
		if (r.below(100) < 35) hi = (uint16_t)(md & 0x8000 ? 0xffff : 0);      // the accumulator fits 32 bits
		else if (r.below(100) < 40) hi = (uint16_t)(r.below(2) ? 0xffff : 0);
		else hi = lane(r);
		acc[i] = hi; acc[8 + i] = md; acc[16 + i] = lo;
	}
}

void gen_flag(Rng &r, uint16_t *f)    // 8 lanes of 0 / 0xffff
{
	uint32_t k = r.below(100);
	for (int i = 0; i < 8; i++) f[i] = k < 10 ? 0 : k < 20 ? 0xffff : (r.u32() & 1) ? 0xffff : 0;
}

uint32_t gen_addr(Rng &r)
{
	uint32_t k = r.below(100), a;
	if (k < 35) a = r.below(256) * 16;
	else if (k < 50) a = r.below(512) * 8;
	else if (k < 60) a = r.below(2048) * 2;
	else if (k < 78) a = 0xfff - r.below(40);
	else if (k < 84) a = r.below(40);
	else a = r.below(4096);
	uint32_t h = r.below(100);
	if (h < 6) a += 0x1000 * r.below(4);
	else if (h < 8) a += r.u32() & 0xfffff000u;
	return a;
}

// ---- sides --------------------------------------------------------------------------------------------------------------------------
const size_t kDm = 4096, kGuard = 64;

struct Side {
	RSP::CPUState *cpu;
	uint8_t *dm;
	Side() {
		cpu = new RSP::CPUState();
		dm = (uint8_t *)aligned_alloc(64, kDm + kGuard);
		memset(dm, 0, kDm + kGuard);
		cpu->dmem = (uint32_t *)dm;
	}
	vu_io io() {
		vu_io v;
		v.vr = (uint16_t (*)[8])cpu->cp2.regs;
		v.acc = cpu->cp2.acc.e;
		v.flags = (uint16_t (*)[16])cpu->cp2.flags;
		v.div_out = &cpu->cp2.div_out;
		v.div_in = &cpu->cp2.div_in;
		v.dp_flag = &cpu->cp2.dp_flag;
		v.dmem = cpu->dmem;
		return v;
	}
	void copy_from(const Side &o) {
		memcpy(&cpu->cp2, &o.cpu->cp2, sizeof cpu->cp2);
		memcpy(cpu->sr, o.cpu->sr, sizeof cpu->sr);
		memcpy(dm, o.dm, kDm + kGuard);
	}
};

uint16_t *regs(RSP::CPUState *c, int n) { return c->cp2.regs[n].e; }

void gen_flags_all(Rng &r, RSP::CPUState *c)
{
	for (int f = 0; f < 3; f++) {
		uint16_t *p = c->cp2.flags[f].e;
		if (f == 2) { memset(p, 0, 16); gen_flag(r, p + 8); }
		else { gen_flag(r, p); gen_flag(r, p + 8); }
	}
	// the flag registers are mostly related after a VCH / VCL: sometimes make VCO.lo (sign) consistent with random data only
	c->cp2.div_out = (int16_t)lane(r);
	c->cp2.div_in = (int16_t)lane(r);
	uint32_t k = r.below(4);
	c->cp2.dp_flag = k == 0 ? 0 : k == 1 ? 1 : (int8_t)r.u32();
}

// ---- comparison -----------------------------------------------------------------------------------------------------------------------
struct Diff { std::string text; bool any = false; };

void diff_u16(Diff &d, const char *what, int idx, const uint16_t *got, const uint16_t *exp, int n)
{
	for (int i = 0; i < n; i++)
		if (got[i] != exp[i]) {
			char b[160];
			snprintf(b, sizeof b, "  %s%d lane %d: lib %04x fork %04x\n", what, idx, i, got[i], exp[i]);
			d.text += b; d.any = true;
		}
}

Diff compare(const Side &lib, const Side &ref, bool sr)
{
	Diff d;
	const RSP::CP2 &a = lib.cpu->cp2, &b = ref.cpu->cp2;
	if (!memcmp(&a, &b, sizeof a) && !memcmp(lib.dm, ref.dm, kDm) && (!sr || !memcmp(lib.cpu->sr, ref.cpu->sr, sizeof lib.cpu->sr))) return d;
	for (int i = 0; i < 32; i++) diff_u16(d, "v", i, a.regs[i].e, b.regs[i].e, 8);
	diff_u16(d, "acc.hi", 0, a.acc.e, b.acc.e, 8);
	diff_u16(d, "acc.md", 0, a.acc.e + 8, b.acc.e + 8, 8);
	diff_u16(d, "acc.lo", 0, a.acc.e + 16, b.acc.e + 16, 8);
	static const char *fn[3] = { "VCO", "VCC", "VCE" };
	for (int f = 0; f < 3; f++) {
		diff_u16(d, fn[f], 0, a.flags[f].e + 8, b.flags[f].e + 8, 8);           // bits 0..7
		diff_u16(d, f == 2 ? "VCE(hi lanes) " : fn[f], 1, a.flags[f].e, b.flags[f].e, 8);   // bits 8..15
	}
	char buf[160];
	if (a.div_out != b.div_out) { snprintf(buf, sizeof buf, "  div_out: lib %04x fork %04x\n", (uint16_t)a.div_out, (uint16_t)b.div_out); d.text += buf; d.any = true; }
	if (a.div_in != b.div_in) { snprintf(buf, sizeof buf, "  div_in: lib %04x fork %04x\n", (uint16_t)a.div_in, (uint16_t)b.div_in); d.text += buf; d.any = true; }
	if (a.dp_flag != b.dp_flag) { snprintf(buf, sizeof buf, "  dp_flag: lib %d fork %d\n", a.dp_flag, b.dp_flag); d.text += buf; d.any = true; }
	int shown = 0;
	for (size_t i = 0; i < kDm + kGuard && shown < 6; i++)
		if (lib.dm[i] != ref.dm[i]) {
			snprintf(buf, sizeof buf, "  dmem host byte %zu (rsp addr %zu): lib %02x fork %02x\n", i, i ^ 3, lib.dm[i], ref.dm[i]);
			d.text += buf; d.any = true; shown++;
		}
	if (sr)
		for (int i = 0; i < 33; i++)
			if (lib.cpu->sr[i] != ref.cpu->sr[i]) {
				snprintf(buf, sizeof buf, "  sr[%d]: lib %08x fork %08x\n", i, lib.cpu->sr[i], ref.cpu->sr[i]);
				d.text += buf; d.any = true;
			}
	if (!d.any) { d.text = "  (memcmp differs in padding?)\n"; d.any = true; }
	return d;
}

std::string dump_pre(const RSP::CP2 &cp2, int vd, int vs, int vt)
{
	std::string t;
	char b[200];
	auto vec = [&](const char *n, const uint16_t *p, int cnt) {
		snprintf(b, sizeof b, "    %-8s", n); t += b;
		for (int i = 0; i < cnt; i++) { snprintf(b, sizeof b, " %04x", p[i]); t += b; }
		t += "\n";
	};
	char n[16];
	snprintf(n, sizeof n, "v%d(vd)", vd); vec(n, cp2.regs[vd].e, 8);
	snprintf(n, sizeof n, "v%d(vs)", vs); vec(n, cp2.regs[vs].e, 8);
	snprintf(n, sizeof n, "v%d(vt)", vt); vec(n, cp2.regs[vt].e, 8);
	vec("acc.hi", cp2.acc.e, 8); vec("acc.md", cp2.acc.e + 8, 8); vec("acc.lo", cp2.acc.e + 16, 8);
	vec("VCO.lo", cp2.flags[0].e + 8, 8); vec("VCO.hi", cp2.flags[0].e, 8);
	vec("VCC.lo", cp2.flags[1].e + 8, 8); vec("VCC.hi", cp2.flags[1].e, 8);
	vec("VCE", cp2.flags[2].e + 8, 8);
	snprintf(b, sizeof b, "    div_out %04x div_in %04x dp_flag %d\n", (uint16_t)cp2.div_out, (uint16_t)cp2.div_in, cp2.dp_flag);
	t += b;
	return t;
}

struct Stats {
	unsigned long trials = 0, bad = 0;
	int printed = 0;
};
bool g_verbose = false;
const int kMaxPrint = 3;

// ---- the VU ops -----------------------------------------------------------------------------------------------------------------------
using VuFn = void (*)(struct vu *);
using VuOracle = void (*)(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
constexpr int NC = 8;
constexpr int Cd[NC] = { 2, 31, 6, 8, 12, 0, 4, 14 };
constexpr int Cs[NC] = { 9, 31, 11, 8, 13, 30, 18, 20 };   // & 7: 1 7 3 0 5 6 2 4 (every destination element of VMOV / VRCP...)
constexpr int Ct[NC] = { 17, 0, 6, 8, 13, 30, 19, 14 };

struct VuOpEntry {
	const char *name;
	VuOracle ora;
	VuFn tab[16][NC];
};

template <class Op, int E, int... C>
void fill_c(VuFn *row, std::integer_sequence<int, C...>) { ((row[C] = &Op::template f<Cd[C], Cs[C], Ct[C], E>), ...); }
template <class Op, int... E>
void fill_e(VuFn (*t)[NC], std::integer_sequence<int, E...>) { (fill_c<Op, E>(t[E], std::make_integer_sequence<int, NC>{}), ...); }

#define VUOP(NAME) struct Op_##NAME { template <int D, int S, int T, int E> static void f(struct vu *u) { NAME(u, D, S, T, E); } };
VUOP(VMULF) VUOP(VMULU) VUOP(VMUDL) VUOP(VMUDM) VUOP(VMUDN) VUOP(VMUDH) VUOP(VMACF) VUOP(VMACU) VUOP(VMADL) VUOP(VMADM) VUOP(VMADN)
VUOP(VMADH) VUOP(VABS) VUOP(VADD) VUOP(VADDC) VUOP(VSUB) VUOP(VSUBC) VUOP(VAND) VUOP(VNAND) VUOP(VOR) VUOP(VNOR) VUOP(VXOR) VUOP(VNXOR)
VUOP(VEQ) VUOP(VNE) VUOP(VGE) VUOP(VLT) VUOP(VCH) VUOP(VCL) VUOP(VCR) VUOP(VMRG) VUOP(VSAR) VUOP(VRESERVED)
// (u, vd, de, vt, E): S is the vs field of the instruction
#define VUOP_DE(NAME) struct Op_##NAME { template <int D, int S, int T, int E> static void f(struct vu *u) { NAME(u, D, S, T, E); } };
VUOP_DE(VMOV) VUOP_DE(VRCP) VUOP_DE(VRCPL) VUOP_DE(VRCPH) VUOP_DE(VRSQ) VUOP_DE(VRSQL) VUOP_DE(VRSQH)

std::vector<VuOpEntry *> g_vu;
template <class Op> void reg_vu(const char *name, VuOracle o)
{
	auto *e = new VuOpEntry{ name, o, {} };
	fill_e<Op>(e->tab, std::make_integer_sequence<int, 16>{});
	g_vu.push_back(e);
}

// the load -> op -> store harness, out of line so each instantiation stays small
__attribute__((noinline)) void run_lib(Side &s, VuFn f)
{
	struct vu u;
	vu_io io = s.io();
	vu_load(&u, &io);
	f(&u);
	vu_store(&u, &io);
}

void gen_vu_state(Rng &r, Side &ref, int e, int vd, int vs, int vt, bool full)
{
	RSP::CPUState *c = ref.cpu;
	if (full)
		for (int i = 0; i < 32; i++) for (int k = 0; k < 8; k++) regs(c, i)[k] = lane(r);
	for (int k = 0; k < 8; k++) regs(c, vd)[k] = lane(r);
	gen_pair(r, regs(c, vs), regs(c, vt), e);
	if (vs == vt)
		for (int k = 0; k < 8; k++) regs(c, vt)[k] = regs(c, vs)[k];       // identical register (gen_pair wrote vt last)
	gen_acc(r, c->cp2.acc.e);
	gen_flags_all(r, c);
}

bool op_selected(const std::vector<std::string> &filt, const char *name)
{
	if (filt.empty()) return true;
	for (auto &f : filt) if (f == name) return true;
	return false;
}

void run_vu(Rng &r, unsigned long n, const std::vector<std::string> &filt, unsigned long &total_trials, unsigned long &total_bad, int &nops)
{
	Side ref, lib;
	for (auto *op : g_vu) {
		if (!op_selected(filt, op->name)) continue;
		Stats st;
		nops++;
		for (int e = 0; e < 16; e++)
			for (unsigned long t = 0; t < n; t++) {
				int c = (int)(t % NC);
				int vd = Cd[c], vs = Cs[c], vt = Ct[c];
				bool deop = op->ora == RSP_VMOV || op->ora == RSP_VRCP || op->ora == RSP_VRCPL || op->ora == RSP_VRCPH || op->ora == RSP_VRSQ ||
				            op->ora == RSP_VRSQL || op->ora == RSP_VRSQH;
				gen_vu_state(r, ref, e, vd, deop ? vd : vs, vt, (t & 7) == 0);
				lib.copy_from(ref);
				RSP::CP2 saved;
				memcpy(&saved, &ref.cpu->cp2, sizeof saved);
				run_lib(lib, op->tab[e][c]);
				op->ora(ref.cpu, vd, vs, vt, e);
				st.trials++;
				Diff d = compare(lib, ref, false);
				if (d.any) {
					st.bad++;
					if (st.printed++ < kMaxPrint) {
						printf("MISMATCH %s E=%d vd=%d vs=%d vt=%d\n%s", op->name, e, vd, vs, vt, d.text.c_str());
						printf("  state before:\n%s", dump_pre(saved, vd, vs, vt).c_str());
					}
				}
			}
		printf("%-9s %9lu trials  %lu mismatches\n", op->name, st.trials, st.bad);
		total_trials += st.trials; total_bad += st.bad;
	}
}

// All 65536 single-precision inputs and a spread of double-precision ones through the reciprocal unit.
void run_recip_sweep(Rng &r, unsigned long &total_trials, unsigned long &total_bad)
{
	Side ref, lib;
	unsigned long bad = 0, trials = 0;
	struct { const char *n; VuFn f; VuOracle o; } ops[] = {
		{ "VRCP", &Op_VRCP::f<3, 5, 9, 0>, RSP_VRCP }, { "VRCPL", &Op_VRCPL::f<3, 5, 9, 0>, RSP_VRCPL },
		{ "VRSQ", &Op_VRSQ::f<3, 5, 9, 0>, RSP_VRSQ }, { "VRSQL", &Op_VRSQL::f<3, 5, 9, 0>, RSP_VRSQL },
		{ "VRCPH", &Op_VRCPH::f<3, 5, 9, 0>, RSP_VRCPH }, { "VRSQH", &Op_VRSQH::f<3, 5, 9, 0>, RSP_VRSQH },
	};
	for (auto &op : ops)
		for (uint32_t x = 0; x < 65536; x++) {
			gen_vu_state(r, ref, 0, 3, 5, 9, false);
			regs(ref.cpu, 9)[0] = (uint16_t)x;
			if (x & 1) ref.cpu->cp2.dp_flag = 1;
			lib.copy_from(ref);
			run_lib(lib, op.f);
			op.o(ref.cpu, 3, 5, 9, 0);
			trials++;
			Diff d = compare(lib, ref, false);
			if (d.any && bad++ < 3) printf("MISMATCH recip sweep %s x=%04x\n%s", op.n, x, d.text.c_str());
		}
	printf("%-9s %9lu trials  %lu mismatches   (all 65536 inputs of vt[0] for each of VRCP VRCPL VRSQ VRSQL VRCPH VRSQH)\n", "recip", trials, bad);
	total_trials += trials; total_bad += bad;
}

// ---- loads / stores -----------------------------------------------------------------------------------------------------------------
using LsFn = void (*)(struct vu *, unsigned);
using LsOracle = void (*)(RSP::CPUState *, unsigned, unsigned, int, unsigned);
constexpr int NR = 5;
constexpr int Rt[NR] = { 0, 5, 13, 24, 31 };               // LTV / STV groups 0, 0, 8, 24, 24

struct LsOpEntry {
	const char *name;
	LsOracle ora;
	bool store;
	LsFn tab[16][NR];
};

template <class Op, int E, int... C>
void fill_lc(LsFn *row, std::integer_sequence<int, C...>) { ((row[C] = &Op::template f<Rt[C], E>), ...); }
template <class Op, int... E>
void fill_le(LsFn (*t)[NR], std::integer_sequence<int, E...>) { (fill_lc<Op, E>(t[E], std::make_integer_sequence<int, NR>{}), ...); }

#define LSOP(NAME) struct LOp_##NAME { template <int R, int E> static void f(struct vu *u, unsigned a) { NAME(u, R, E, a); } };
LSOP(LBV) LSOP(LSV) LSOP(LLV) LSOP(LDV) LSOP(LQV) LSOP(LPV) LSOP(LUV) LSOP(LTV)
LSOP(SBV) LSOP(SSV) LSOP(SLV) LSOP(SDV) LSOP(SQV) LSOP(SPV) LSOP(SUV) LSOP(STV)

std::vector<LsOpEntry *> g_ls;
template <class Op> void reg_ls(const char *name, LsOracle o, bool store)
{
	auto *e = new LsOpEntry{ name, o, store, {} };
	fill_le<Op>(e->tab, std::make_integer_sequence<int, 16>{});
	g_ls.push_back(e);
}

__attribute__((noinline)) void run_lib_ls(Side &s, LsFn f, unsigned a)
{
	struct vu u;
	vu_io io = s.io();
	vu_load(&u, &io);
	f(&u, a);
	vu_store(&u, &io);
}

void gen_dmem_window(Rng &r, Side &ref, uint32_t a, bool full)
{
	uint8_t *d = ref.dm;
	if (full)
		for (size_t i = 0; i < kDm; i += 8) { uint64_t v = r.next(); memcpy(d + i, &v, 8); }
	// a window around the address (wrapping) with edge-heavy bytes, so stray writes and misplaced bytes show
	for (int k = -20; k < 52; k++) {
		uint32_t p = (a + (uint32_t)k) & 0xfff;
		uint32_t q = r.below(10);
		d[p ^ 3] = q == 0 ? 0 : q == 1 ? 0xff : q == 2 ? 0x80 : q == 3 ? 0x7f : (uint8_t)r.u32();
	}
}

// the fork's SLV / STV byte paths write past the end of DMEM instead of wrapping: fold what landed in the guard bytes back
void fold_guard(Side &ref, const Side &ref2)
{
	for (size_t g = 0; g < kGuard; g++) {
		uint8_t b0 = ref.dm[kDm + g], b1 = ref2.dm[kDm + g];
		if (b0 != 0 || b1 != 0xff) {
			uint8_t v = b0 != 0 ? b0 : b1;
			size_t logical = (kDm + g) ^ 3;
			ref.dm[(logical & 0xfff) ^ 3] = v;
		}
		ref.dm[kDm + g] = 0;
	}
}

void run_ls(Rng &r, unsigned long n, const std::vector<std::string> &filt, unsigned long &total_trials, unsigned long &total_bad, int &nops)
{
	Side ref, lib, ref2;
	for (auto *op : g_ls) {
		if (!op_selected(filt, op->name)) continue;
		Stats st;
		nops++;
		for (int e = 0; e < 16; e++)
			for (unsigned long t = 0; t < n; t++) {
				int c = (int)(t % NR);
				int rt = Rt[c];
				uint32_t a = gen_addr(r);
				bool full = (t & 255) == 0;
				if (full)
					for (int i = 0; i < 32; i++) for (int k = 0; k < 8; k++) regs(ref.cpu, i)[k] = lane(r);
				for (int k = 0; k < 8; k++) regs(ref.cpu, rt)[k] = lane(r);
				if (op->ora == RSP_LTV || op->ora == RSP_STV)
					for (int i = 0; i < 8; i++) for (int k = 0; k < 8; k++) regs(ref.cpu, (rt & ~7) + i)[k] = lane(r);
				gen_flags_all(r, ref.cpu);
				gen_acc(r, ref.cpu->cp2.acc.e);
				gen_dmem_window(r, ref, a, full);
				memset(ref.dm + kDm, 0, kGuard);
				lib.copy_from(ref);
				RSP::CP2 saved; memcpy(&saved, &ref.cpu->cp2, sizeof saved);
				run_lib_ls(lib, op->tab[e][c], a);
				ref.cpu->sr[1] = a;
				if (op->store) {
					ref2.copy_from(ref);
					memset(ref2.dm + kDm, 0xff, kGuard);
					op->ora(ref.cpu, rt, e, 0, 1);
					op->ora(ref2.cpu, rt, e, 0, 1);
					fold_guard(ref, ref2);
				} else {
					op->ora(ref.cpu, rt, e, 0, 1);
				}
				st.trials++;
				lib.cpu->sr[1] = a;
				Diff d = compare(lib, ref, true);
				if (d.any) {
					st.bad++;
					if (st.printed++ < kMaxPrint) {
						printf("MISMATCH %s E=%d rt=%d addr=%08x\n%s", op->name, e, rt, a, d.text.c_str());
						printf("  state before:\n%s", dump_pre(saved, rt, rt, rt).c_str());
					}
				}
			}
		printf("%-9s %9lu trials  %lu mismatches\n", op->name, st.trials, st.bad);
		total_trials += st.trials; total_bad += st.bad;
	}
}

// ---- MFC2 / MTC2 / CFC2 / CTC2 ---------------------------------------------------------------------------------------------------
struct MoveOp { template <int V, int E> static void mfc2(struct vu *u, uint32_t *g) { MFC2(u, g, V, E); }
                template <int V, int E> static void mtc2(struct vu *u, uint32_t g) { MTC2(u, g, V, E); }
                template <int W> static void cfc2(struct vu *u, uint32_t *g) { CFC2(u, g, W); }
                template <int W> static void ctc2(struct vu *u, uint32_t g) { CTC2(u, g, W); } };

template <int V, int... E> void mfc_tab(void (**t)(struct vu *, uint32_t *), std::integer_sequence<int, E...>) { ((t[E] = &MoveOp::mfc2<V, E>), ...); }
template <int V, int... E> void mtc_tab(void (**t)(struct vu *, uint32_t), std::integer_sequence<int, E...>) { ((t[E] = &MoveOp::mtc2<V, E>), ...); }

void run_moves(Rng &r, unsigned long n, const std::vector<std::string> &filt, unsigned long &total_trials, unsigned long &total_bad, int &nops)
{
	Side ref, lib;
	void (*mfc[2][16])(struct vu *, uint32_t *);
	void (*mtc[2][16])(struct vu *, uint32_t);
	mfc_tab<7>(mfc[0], std::make_integer_sequence<int, 16>{});
	mfc_tab<30>(mfc[1], std::make_integer_sequence<int, 16>{});
	mtc_tab<2>(mtc[0], std::make_integer_sequence<int, 16>{});
	mtc_tab<29>(mtc[1], std::make_integer_sequence<int, 16>{});
	void (*cfc[4])(struct vu *, uint32_t *) = { &MoveOp::cfc2<0>, &MoveOp::cfc2<1>, &MoveOp::cfc2<2>, &MoveOp::cfc2<3> };
	void (*ctc[4])(struct vu *, uint32_t) = { &MoveOp::ctc2<0>, &MoveOp::ctc2<1>, &MoveOp::ctc2<2>, &MoveOp::ctc2<3> };
	const int vregs[2] = { 7, 30 }, vdregs[2] = { 2, 29 };

	auto prep = [&](bool full) {
		if (full) for (int i = 0; i < 32; i++) for (int k = 0; k < 8; k++) regs(ref.cpu, i)[k] = lane(r);
		for (int k = 0; k < 8; k++) { regs(ref.cpu, 7)[k] = lane(r); regs(ref.cpu, 30)[k] = lane(r); regs(ref.cpu, 2)[k] = lane(r); regs(ref.cpu, 29)[k] = lane(r); }
		gen_flags_all(r, ref.cpu);
		gen_acc(r, ref.cpu->cp2.acc.e);
		for (int i = 1; i < 32; i++) ref.cpu->sr[i] = r.u32();
		if (r.below(3) == 0) ref.cpu->sr[3] = lane(r);
		lib.copy_from(ref);
	};
	auto one = [&](const char *name, int e, Stats &st, const std::string &desc) {
		st.trials++;
		Diff d = compare(lib, ref, true);
		if (d.any) {
			st.bad++;
			if (st.printed++ < kMaxPrint) printf("MISMATCH %s E=%d %s\n%s", name, e, desc.c_str(), d.text.c_str());
		}
	};
	struct Row { const char *name; Stats st; } rows[4] = { { "MFC2", {} }, { "MTC2", {} }, { "CFC2", {} }, { "CTC2", {} } };
	for (int e = 0; e < 16; e++)
		for (unsigned long t = 0; t < n; t++) {
			int k = (int)(t & 1);
			if (op_selected(filt, "MFC2")) {
				prep((t & 63) == 0);
				uint32_t g = 0xdeadbeef;
				{
					struct vu u; vu_io io = lib.io(); vu_load(&u, &io); mfc[k][e](&u, &g); vu_store(&u, &io);
					lib.cpu->sr[3] = g;
				}
				RSP_MFC2(ref.cpu, 3, vregs[k], e);
				one("MFC2", e, rows[0].st, "");
			}
			if (op_selected(filt, "MTC2")) {
				prep((t & 63) == 0);
				{
					struct vu u; vu_io io = lib.io(); vu_load(&u, &io); mtc[k][e](&u, lib.cpu->sr[5]); vu_store(&u, &io);
				}
				RSP_MTC2(ref.cpu, 5, vdregs[k], e);
				one("MTC2", e, rows[1].st, "");
			}
			if (e < 4 && op_selected(filt, "CFC2")) {
				prep((t & 63) == 0);
				uint32_t g = 0xdeadbeef;
				{
					struct vu u; vu_io io = lib.io(); vu_load(&u, &io); cfc[e](&u, &g); vu_store(&u, &io);
					lib.cpu->sr[3] = g;
				}
				RSP_CFC2(ref.cpu, 3, (unsigned)e | (r.below(8) << 2));
				one("CFC2", e, rows[2].st, "");
			}
			if (e < 4 && op_selected(filt, "CTC2")) {
				prep((t & 63) == 0);
				{
					struct vu u; vu_io io = lib.io(); vu_load(&u, &io); ctc[e](&u, lib.cpu->sr[5]); vu_store(&u, &io);
				}
				RSP_CTC2(ref.cpu, 5, (unsigned)e | (r.below(8) << 2));
				one("CTC2", e, rows[3].st, "");
			}
		}
	for (auto &row : rows) {
		if (!op_selected(filt, row.name)) continue;
		printf("%-9s %9lu trials  %lu mismatches\n", row.name, row.st.trials, row.st.bad);
		total_trials += row.st.trials; total_bad += row.st.bad; nops++;
	}
}

// ---- vu_load / vu_store round trip and the masked store --------------------------------------------------------------------------------
template <uint32_t DV, unsigned DA, unsigned DF, unsigned DD>
bool masked_case(Rng &r)
{
	Side src, dst;
	for (int i = 0; i < 32; i++) for (int k = 0; k < 8; k++) regs(src.cpu, i)[k] = lane(r);
	gen_acc(r, src.cpu->cp2.acc.e);
	gen_flags_all(r, src.cpu);
	for (int i = 0; i < 32; i++) for (int k = 0; k < 8; k++) regs(dst.cpu, i)[k] = lane(r);
	gen_acc(r, dst.cpu->cp2.acc.e);
	gen_flags_all(r, dst.cpu);
	Side orig; orig.copy_from(dst);
	struct vu u;
	vu_io is = src.io(), id = dst.io();
	vu_load(&u, &is);
	vu_store_masked_ex(&u, &id, DV, DA, DF, DD);
	// expected: dead parts keep their old content, the rest is the source's
	bool ok = true;
	for (int i = 0; i < 32; i++)
		if (memcmp(regs(dst.cpu, i), regs((DV >> i) & 1 ? orig.cpu : src.cpu, i), 16)) ok = false;
	for (int p = 0; p < 3; p++)
		if (memcmp(dst.cpu->cp2.acc.e + 8 * p, ((DA >> p) & 1 ? orig.cpu : src.cpu)->cp2.acc.e + 8 * p, 16)) ok = false;
	for (int f = 0; f < 3; f++) {
		const RSP::CPUState *s = (DF >> f) & 1 ? orig.cpu : src.cpu;
		if (memcmp(dst.cpu->cp2.flags[f].e + 8, s->cp2.flags[f].e + 8, 16)) ok = false;
		if (f < 2 && memcmp(dst.cpu->cp2.flags[f].e, s->cp2.flags[f].e, 16)) ok = false;
		if (f == 2 && memcmp(dst.cpu->cp2.flags[f].e, orig.cpu->cp2.flags[f].e, 16)) ok = false;   // lanes 0..7 of VCE are never written
	}
	if (dst.cpu->cp2.div_out != ((DD & 1) ? orig.cpu : src.cpu)->cp2.div_out) ok = false;
	if (dst.cpu->cp2.div_in != ((DD & 2) ? orig.cpu : src.cpu)->cp2.div_in) ok = false;
	if (dst.cpu->cp2.dp_flag != ((DD & 4) ? orig.cpu : src.cpu)->cp2.dp_flag) ok = false;
	return ok;
}

void run_store_tests(Rng &r, unsigned long &total_trials, unsigned long &total_bad, int &nops)
{
	unsigned long bad = 0, trials = 0;
	for (int i = 0; i < 200; i++) {
		// full store must reproduce the state exactly
		Side a, b;
		for (int k = 0; k < 32; k++) for (int j = 0; j < 8; j++) regs(a.cpu, k)[j] = lane(r);
		gen_acc(r, a.cpu->cp2.acc.e);
		gen_flags_all(r, a.cpu);
		b.copy_from(a);
		struct vu u; vu_io ia = a.io();
		// scribble over the target first so a missing write shows
		memset(b.cpu->cp2.regs, 0x5a, sizeof b.cpu->cp2.regs);
		memset(b.cpu->cp2.acc.e, 0x5a, sizeof b.cpu->cp2.acc.e);
		memset(b.cpu->cp2.flags[0].e, 0x5a, 32);
		memset(b.cpu->cp2.flags[1].e, 0x5a, 32);
		memset(b.cpu->cp2.flags[2].e + 8, 0x5a, 16);                  // lanes 0..7 of VCE are never written
		b.cpu->cp2.div_out = b.cpu->cp2.div_in = 0x5a5a; b.cpu->cp2.dp_flag = 0x5a;
		vu_io ib = b.io();
		vu_load(&u, &ia); vu_store(&u, &ib);
		Side c; c.copy_from(a);
		trials++;
		Diff d = compare(b, c, false);
		if (d.any && bad++ < 3) printf("MISMATCH vu_load/vu_store\n%s", d.text.c_str());
		bool ok = masked_case<0xAAAAAAAAu, 5, 2, 3>(r) && masked_case<0x0000FFFFu, 7, 0, 0>(r) && masked_case<0xFFFFFFFFu, 0, 7, 4>(r) &&
		          masked_case<0x80000001u, 2, 5, 1>(r) && masked_case<0x00F00F00u, 4, 3, 6>(r);
		trials++;
		if (!ok && bad++ < 3) printf("MISMATCH vu_store_masked_ex\n");
	}
	printf("%-9s %9lu trials  %lu mismatches\n", "store", trials, bad);
	total_trials += trials; total_bad += bad; nops++;
}

void register_all()
{
#define R(NAME) reg_vu<Op_##NAME>(#NAME, RSP_##NAME);
	R(VMULF) R(VMULU) R(VMUDL) R(VMUDM) R(VMUDN) R(VMUDH) R(VMACF) R(VMACU) R(VMADL) R(VMADM) R(VMADN) R(VMADH) R(VABS) R(VADD) R(VADDC)
	R(VSUB) R(VSUBC) R(VAND) R(VNAND) R(VOR) R(VNOR) R(VXOR) R(VNXOR) R(VEQ) R(VNE) R(VGE) R(VLT) R(VCH) R(VCL) R(VCR) R(VMRG) R(VSAR)
	R(VMOV) R(VRCP) R(VRCPL) R(VRCPH) R(VRSQ) R(VRSQL) R(VRSQH)
	reg_vu<Op_VRESERVED>("VRESERVED", RSP_RESERVED);
#undef R
#define L(NAME, ST) reg_ls<LOp_##NAME>(#NAME, RSP_##NAME, ST);
	L(LBV, false) L(LSV, false) L(LLV, false) L(LDV, false) L(LQV, false) L(LPV, false) L(LUV, false) L(LTV, false)
	L(SBV, true) L(SSV, true) L(SLV, true) L(SDV, true) L(SQV, true) L(SPV, true) L(SUV, true) L(STV, true)
#undef L
}

} // namespace

int main(int argc, char **argv)
{
	unsigned long n = 20000;
	uint64_t seed = 1;
	std::vector<std::string> filt;
	if (const char *en = getenv("VU_TEST_N")) n = strtoul(en, nullptr, 0);
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "-n" && i + 1 < argc) n = strtoul(argv[++i], nullptr, 0);
		else if (a == "-s" && i + 1 < argc) seed = strtoull(argv[++i], nullptr, 0);
		else if (a == "-v") g_verbose = true;
		else if (a == "-o" && i + 1 < argc) {
			std::string l = argv[++i];
			size_t p = 0;
			while (p <= l.size()) { size_t q = l.find(',', p); if (q == std::string::npos) q = l.size(); filt.push_back(l.substr(p, q - p)); p = q + 1; }
		} else { fprintf(stderr, "usage: vu_test [-n TRIALS] [-s SEED] [-o OP[,OP...]] [-v]\n"); return 2; }
	}
	register_all();
	printf("f3dex_vu backend: %s%s, %lu trials per op and E, seed %llu\n", VU_NEON ? "NEON" : "scalar",
#ifdef VU_MUTATE
	       " (MUTATED: -DVU_MUTATE)",
#else
	       "",
#endif
	       n, (unsigned long long)seed);
	Rng r(seed);
	unsigned long trials = 0, bad = 0;
	int nops = 0;
	run_vu(r, n, filt, trials, bad, nops);
	if (filt.empty() || op_selected(filt, "recip")) run_recip_sweep(r, trials, bad);
	run_ls(r, n, filt, trials, bad, nops);
	run_moves(r, n, filt, trials, bad, nops);
	if (filt.empty() || op_selected(filt, "store")) run_store_tests(r, trials, bad, nops);
	printf("TOTAL: %d ops, %lu trials, %lu mismatches -> %s\n", nops, trials, bad, bad ? "FAIL" : "PASS");
	return bad ? 1 : 0;
}
