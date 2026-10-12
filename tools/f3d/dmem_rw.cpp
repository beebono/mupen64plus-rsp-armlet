/*
 * dmem_rw.cpp - tools/native/trace.cpp (the instruction-level interpreter of a task-start snapshot) with a byte-granular DMEM footprint added: which DMEM bytes a task reads
 * before it has written them (live-in, "LI"), and which it writes ("WMAY" / "WMUST"), including the exact footprints of all vector loads and stores (from ext/parallel-rsp/rsp/ls.cpp)
 * and the DMEM side of every SP DMA. Output after the usual trace summary: "VER" (the version string found in DMEM), then LI / WMAY / WMUST ranges (hex, [lo, hi)).
 * Used for docs/ucode-f3d.md section 11. Not built by CMake: copy over tools/native/trace.cpp of a work copy (scripts/build.sh native --tools) and build the target armlet-native-trace.
 *   armlet-native-trace -s 0x1000 -n 20000000 --final hle.task-<ucode>-<n>.snap       (snapshots: ARMLET_HLE_CAPTURE=<dir>)
 * The RDP is modelled as the host core does: DPC_CURRENT = DPC_END after each DPC_END write (without it the flush's wait loops never end).
 */
/*
 * arMLEt - tools/native/trace.cpp: instruction-level trace of an RSP stretch from a routine-entry snapshot
 *
 *   armlet-native-trace [-n MAXINSTR] [-s STOPPC] [-q] [--final] file.snap
 *
 * Loads a snapshot (replay.c's files) into a private RSP and interprets the code from the entry pc, one instruction at a time, using the fork's own
 * VU / load-store / CP0 / CP2 helper functions (the semantics of the JIT's helper-call path, which the aarch64 inline code is tested against) and plain C for the
 * scalar part. Prints each instruction with what it changed (GPRs, vector registers as 8 halfwords, accumulator, flags, divide unit), and at the end compares the
 * state with the one the JIT left (the snapshot's "after"), which checks this interpreter too. A development aid for working out what a ucode routine computes.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m64p_plugin.h"

#include "rsp_disasm.hpp"
#include "rsp_jit.hpp"
#include "rsp_op.hpp"
#include "prsp_native.h"

extern "C"
{
	void RSP_VMULF(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VMULU(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VRNDP(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VMULQ(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VMUDL(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VMUDM(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VMUDN(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VMUDH(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VMACF(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VMACU(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VRNDN(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VMACQ(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VMADL(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VMADM(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VMADN(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VMADH(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VADD(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VSUB(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VABS(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VADDC(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VSUBC(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VSAR(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VLT(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VEQ(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VNE(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VGE(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VCL(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VCH(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VCR(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VMRG(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VAND(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VNAND(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VOR(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VNOR(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VXOR(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VNXOR(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VRCP(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VRCPL(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VRCPH(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VMOV(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VRSQ(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VRSQL(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VRSQH(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_VNOP(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
	void RSP_RESERVED(RSP::CPUState *, unsigned, unsigned, unsigned, unsigned);
}

namespace RSP
{
extern JIT::CPU cpu;
}
extern "C" void parallelRSPInitiateRSP(RSP_INFO, unsigned int *);
using namespace RSP;

#define SNAP_MAGIC 0x41534e50u
struct SnapHdr
{
	uint32_t magic, version;
	char routine[48];
	uint64_t ucode;
	uint32_t entry_pc, exit_pc, n_rows, n_events, flags;
};

typedef void (*VUOp)(CPUState *, unsigned, unsigned, unsigned, unsigned);
static const VUOp vu_ops[64] = {
	RSP_VMULF, RSP_VMULU, RSP_VRNDP, RSP_VMULQ, RSP_VMUDL, RSP_VMUDM, RSP_VMUDN, RSP_VMUDH, RSP_VMACF, RSP_VMACU, RSP_VRNDN,
	RSP_VMACQ, RSP_VMADL, RSP_VMADM, RSP_VMADN, RSP_VMADH, RSP_VADD, RSP_VSUB, nullptr, RSP_VABS, RSP_VADDC, RSP_VSUBC,
	nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, RSP_VSAR, nullptr, nullptr, RSP_VLT,
	RSP_VEQ, RSP_VNE, RSP_VGE, RSP_VCL, RSP_VCH, RSP_VCR, RSP_VMRG, RSP_VAND, RSP_VNAND, RSP_VOR, RSP_VNOR,
	RSP_VXOR, RSP_VNXOR, nullptr, nullptr, RSP_VRCP, RSP_VRCPL, RSP_VRCPH, RSP_VMOV, RSP_VRSQ, RSP_VRSQL, RSP_VRSQH,
	RSP_VNOP, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, RSP_VNOP
};
typedef void (*LSOp)(CPUState *, unsigned, unsigned, int, unsigned);
static const LSOp lwc2_ops[32] = { RSP_LBV, RSP_LSV, RSP_LLV, RSP_LDV, RSP_LQV, RSP_LRV, RSP_LPV, RSP_LUV, RSP_LHV, RSP_LFV, nullptr, RSP_LTV };
static const LSOp swc2_ops[32] = { RSP_SBV, RSP_SSV, RSP_SLV, RSP_SDV, RSP_SQV, RSP_SRV, RSP_SPV, RSP_SUV, RSP_SHV, RSP_SFV, RSP_SWV, RSP_STV };

static uint8_t dm8(CPUState &s, unsigned a) { return reinterpret_cast<uint8_t *>(s.dmem)[(a & 0xfff) ^ 3]; }
static void st8(CPUState &s, unsigned a, uint8_t v) { reinterpret_cast<uint8_t *>(s.dmem)[(a & 0xfff) ^ 3] = v; }

static void nop(void) {}
static unsigned char *g_rdram;
static uint32_t g_dmem[1024], g_imem[1024], g_regs[16], g_mi, g_sp_pc;
static RSP_INFO g_info;

static void world()
{
	unsigned cycles = 0;
	g_rdram = static_cast<unsigned char *>(calloc(1, 0x800000));
	memset(&g_info, 0, sizeof(g_info));
	g_info.RDRAM = g_rdram;
	g_info.DMEM = reinterpret_cast<unsigned char *>(g_dmem);
	g_info.IMEM = reinterpret_cast<unsigned char *>(g_imem);
	g_info.MI_INTR_REG = &g_mi;
	unsigned **dst[] = { &g_info.SP_MEM_ADDR_REG, &g_info.SP_DRAM_ADDR_REG, &g_info.SP_RD_LEN_REG, &g_info.SP_WR_LEN_REG, &g_info.SP_STATUS_REG,
		                 &g_info.SP_DMA_FULL_REG, &g_info.SP_DMA_BUSY_REG, &g_info.SP_SEMAPHORE_REG, &g_info.DPC_START_REG, &g_info.DPC_END_REG,
		                 &g_info.DPC_CURRENT_REG, &g_info.DPC_STATUS_REG, &g_info.DPC_CLOCK_REG, &g_info.DPC_BUFBUSY_REG, &g_info.DPC_PIPEBUSY_REG,
		                 &g_info.DPC_TMEM_REG };
	for (int i = 0; i < 16; i++)
		*dst[i] = &g_regs[i];
	g_info.SP_PC_REG = &g_sp_pc;
	g_info.CheckInterrupts = nop;
	g_info.ProcessDlistList = nop;
	g_info.ProcessAlistList = nop;
	g_info.ProcessRdpList = nop;
	g_info.ShowCFB = nop;
	parallelRSPInitiateRSP(g_info, &cycles);
}

// the fork's disassembler prints three-register ALU ops as (rd, rt, rs) and CP0 moves with the CP0 register named like a GPR: print them the usual way
static std::string fixup(const std::string &t)
{
	static const char *gpr[32] = { "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4",
		                           "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra" };
	char addr[16], op[16], a[3][16];
	if (sscanf(t.c_str(), "%15s %15s %15[^,], %15[^,], %15s", addr, op, a[0], a[1], a[2]) == 5 &&
	    (!strcmp(op, "addu") || !strcmp(op, "subu") || !strcmp(op, "and") || !strcmp(op, "or") || !strcmp(op, "xor") || !strcmp(op, "nor") || !strcmp(op, "slt") ||
	     !strcmp(op, "sltu")))
	{
		char b[96];
		snprintf(b, sizeof(b), "%s  %s %s, %s, %s", addr, op, a[0], a[2], a[1]);
		return b;
	}
	if (sscanf(t.c_str(), "%15s %15s %15[^,], %15s", addr, op, a[0], a[1]) == 4 && (!strcmp(op, "mtc0") || !strcmp(op, "mfc0")))
	{
		const bool mt = !strcmp(op, "mtc0");        // mtc0 prints (CP0 register, rt), mfc0 (rt, CP0 register)
		int c = -1;
		for (int i = 0; i < 32; i++)
			if (!strcmp(a[mt ? 0 : 1], gpr[i]))
				c = i;
		char b[96];
		snprintf(b, sizeof(b), "%s  %s %s, c0_%d", addr, op, a[mt ? 1 : 0], c);
		return b;
	}
	return t;
}

static std::string vec_str(const uint16_t *v)
{
	char b[64];
	snprintf(b, sizeof(b), "%04x %04x %04x %04x %04x %04x %04x %04x", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
	return b;
}

static uint16_t flag_bits(const uint16_t *f)
{
	uint16_t r = 0;
	for (int k = 0; k < 8; k++)
		r |= (f[8 + k] ? 1 : 0) << k | (f[k] ? 1 : 0) << (8 + k);
	return r;
}

struct Regs
{
	uint32_t sr[32];
	uint16_t vr[32][8];
	uint16_t acc[24];
	uint16_t fl[3];
	int16_t dout, din;
	int8_t dp;
};

static void grab(CPUState &s, Regs &r)
{
	memcpy(r.sr, s.sr, sizeof(r.sr));
	for (int i = 0; i < 32; i++)
		memcpy(r.vr[i], s.cp2.regs[i].e, 16);
	memcpy(r.acc, s.cp2.acc.e, 48);
	for (int i = 0; i < 3; i++)
		r.fl[i] = flag_bits(s.cp2.flags[i].e);
	r.dout = s.cp2.div_out;
	r.din = s.cp2.div_in;
	r.dp = s.cp2.dp_flag;
}

static void diff(const Regs &a, const Regs &b, std::string &out)
{
	char buf[160];
	for (int i = 1; i < 32; i++)
		if (a.sr[i] != b.sr[i])
		{
			snprintf(buf, sizeof(buf), " %s=%08x", RSP::register_name(i), b.sr[i]);
			out += buf;
		}
	for (int i = 0; i < 32; i++)
		if (memcmp(a.vr[i], b.vr[i], 16))
		{
			snprintf(buf, sizeof(buf), " v%d=[%s]", i, vec_str(b.vr[i]).c_str());
			out += buf;
		}
	static const char *an[3] = { "ACC_HI", "ACC_MD", "ACC_LO" };
	for (int i = 0; i < 3; i++)
		if (memcmp(a.acc + 8 * i, b.acc + 8 * i, 16))
		{
			snprintf(buf, sizeof(buf), " %s=[%s]", an[i], vec_str(b.acc + 8 * i).c_str());
			out += buf;
		}
	static const char *fn[3] = { "VCO", "VCC", "VCE" };
	for (int i = 0; i < 3; i++)
		if (a.fl[i] != b.fl[i])
		{
			snprintf(buf, sizeof(buf), " %s=%04x", fn[i], b.fl[i]);
			out += buf;
		}
	if (a.dout != b.dout || a.din != b.din || a.dp != b.dp)
	{
		snprintf(buf, sizeof(buf), " div(out=%04x in=%04x dp=%d)", uint16_t(b.dout), uint16_t(b.din), b.dp);
		out += buf;
	}
}


// ---- DMEM footprint tracking (dmrw) ------------------------------------------------------------------------------------------------
static uint8_t g_wmust[4096];   // bytes certainly written so far in this task
static uint8_t g_wmay[4096];    // bytes possibly written (over-approximation: every byte an instruction's footprint covers)
static uint8_t g_li[4096];      // bytes read before they were certainly written (live-in)
static uint16_t g_lipc[4096];   // pc of the first such read
static uint32_t g_cur_pc;
static uint32_t g_dma_mem, g_dma_dram;
static long g_rd_count[4096];
static inline void rd1(unsigned a) { a &= 0xfff; g_rd_count[a]++; if (!g_wmust[a] && !g_li[a]) { g_li[a] = 1; g_lipc[a] = g_cur_pc; } }
static inline void wr1(unsigned a) { a &= 0xfff; g_wmust[a] = 1; g_wmay[a] = 1; }
static void rdn(unsigned a, unsigned n) { for (unsigned i = 0; i < n; i++) rd1(a + i); }
static void wrn(unsigned a, unsigned n) { for (unsigned i = 0; i < n; i++) wr1(a + i); }

// the DMEM bytes the vector load / store at word w touches (exact, from the fork's ls.cpp)
static void vec_footprint(uint32_t w, const uint32_t *sr)
{
	const unsigned rs = (w >> 21) & 31, sub = (w >> 11) & 31, e = (w >> 7) & 15;
	const int off = int16_t(w << 9) >> 9;
	const bool store = (w >> 26) == 58;
	static const int shift[16] = { 0, 1, 2, 3, 4, 4, 3, 3, 4, 4, 4, 4, 0, 0, 0, 0 };   // B S L D Q R P U H F W T
	if (sub > 11) return;
	unsigned addr = sr[rs] + (off << shift[sub]);
	addr &= 0xfff;
	auto R = [&](unsigned a) { rd1(a); };
	auto W = [&](unsigned a) { wr1(a); };
	if (!store)
	{
		switch (sub)
		{
		case 0: R(addr); break;                                          // LBV
		case 1: rdn(addr, 2); break;                                     // LSV
		case 2: rdn(addr, 4); break;                                     // LLV
		case 3: { unsigned end = e > 8 ? 16 : e + 8; rdn(addr, end - e); break; }   // LDV
		case 4: { unsigned end = 16 + e - (addr & 15); if (end > 16) end = 16; rdn(addr, end - e); break; }   // LQV
		case 5: { unsigned start = 16 - ((addr & 15) - e); unsigned n = 16 - start; rdn(addr & ~15u, n); break; }   // LRV
		case 6: case 7: { unsigned index = (addr & 7) - e; unsigned b = addr & ~7u; for (unsigned i = 0; i < 8; i++) R(b + ((i + index) & 15)); break; }   // LPV LUV
		case 8: { unsigned index = (addr & 7) - e; unsigned b = addr & ~7u; for (unsigned i = 0; i < 8; i++) R(b + ((index + i * 2) & 15)); break; }   // LHV
		case 9: { unsigned index = (addr & 7) - e; unsigned b = addr & ~7u; for (unsigned i = 0; i < 4; i++) { R(b + ((index + i * 4) & 15)); R(b + ((index + i * 4 + 8) & 15)); } break; }   // LFV
		case 11: { unsigned b = addr & ~7u; for (unsigned i = 0; i < 16; i++) R(b + i); break; }   // LTV
		}
		return;
	}
	switch (sub)
	{
	case 0: W(addr); break;
	case 1: wrn(addr, 2); break;
	case 2: wrn(addr, 4); break;
	case 3: wrn(addr, 8); break;
	case 4: { unsigned end = e + (16 - (addr & 15)); wrn(addr, end - e); break; }   // SQV
	case 5: { unsigned end = e + (addr & 15); wrn(addr & ~15u, end - e); break; }   // SRV
	case 6: case 7: wrn(addr, 8); break;
	case 8: { unsigned b = addr & ~7u, index = addr & 7; for (unsigned i = 0; i < 8; i++) W(b + ((index + i * 2) & 15)); break; }   // SHV
	case 9: { unsigned b = addr & ~7u, base = addr & 7; W(b + base); W(b + base + 4); W(b + ((8 + base) & 15)); W(b + ((12 + base) & 15)); break; }   // SFV
	case 10: { unsigned b = addr & ~7u, index = addr & 7; for (unsigned i = 0; i < 16; i++) W(b + ((index + i) & 15)); break; }   // SWV
	case 11: { unsigned b = addr & ~7u; for (unsigned i = 0; i < 16; i++) W(b + i); break; }   // STV
	}
}

static void scalar_footprint(uint32_t w, const uint32_t *sr)
{
	const unsigned op = w >> 26, rs = (w >> 21) & 31;
	const int32_t simm = int16_t(w);
	unsigned a = sr[rs] + simm;
	switch (op)
	{
	case 32: case 36: rd1(a); break;
	case 33: case 37: rdn(a, 2); break;
	case 35: rdn(a, 4); break;
	case 40: wr1(a); break;
	case 41: wrn(a, 2); break;
	case 43: wrn(a, 4); break;
	}
}

static void dma_footprint(unsigned reg, uint32_t v)
{
	if (reg == 0) { g_dma_mem = v; return; }
	if (reg == 1) { g_dma_dram = v; return; }
	if (reg != 2 && reg != 3) return;
	unsigned length = ((v & 0xfff) + 1 + 7) & ~7u, rows = ((v >> 12) & 0xff) + 1;
	unsigned mem = g_dma_mem;
	if (reg == 2 && (mem & 0x1000)) return;                  // into IMEM
	if (reg == 3 && (mem & 0x1000)) return;                  // from IMEM
	unsigned m = mem & 0xff8;
	for (unsigned r = 0; r < rows; r++)
	{
		if (reg == 2) wrn(m, length); else rdn(m, length);
		m = (m + length) & 0xfff;
	}
}

// executes one instruction at pc; returns false on BREAK. *target set to the branch target when the instruction is a taken branch (else ~0)
static bool exec(CPUState &s, uint32_t pc, uint32_t w, uint32_t *target, bool *is_branch)
{
	const unsigned op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, sa = (w >> 6) & 31;
	const int32_t simm = int16_t(w);
	const uint32_t uimm = w & 0xffff;
	uint32_t *sr = s.sr;
	*target = ~0u;
	*is_branch = false;
	auto set = [&](unsigned r, uint32_t v) { if (r) sr[r] = v; };
	auto br = [&](bool taken) { *is_branch = true; if (taken) *target = (pc + 4 + (simm << 2)) & 0xffc; };
	if ((w >> 25) == 0x25)
	{
		VUOp f = vu_ops[w & 63];
		if (!f)
			f = RSP_RESERVED;
		f(&s, (w >> 6) & 31, rd, rt, rs & 15);
		return true;
	}
	switch (op)
	{
	case 0:
		switch (w & 63)
		{
		case 0: set(rd, sr[rt] << sa); break;
		case 2: set(rd, sr[rt] >> sa); break;
		case 3: set(rd, uint32_t(int32_t(sr[rt]) >> sa)); break;
		case 4: set(rd, sr[rt] << (sr[rs] & 31)); break;
		case 6: set(rd, sr[rt] >> (sr[rs] & 31)); break;
		case 7: set(rd, uint32_t(int32_t(sr[rt]) >> (sr[rs] & 31))); break;
		case 8: *is_branch = true; *target = sr[rs] & 0xffc; break;
		case 9: { uint32_t t = sr[rs] & 0xffc; set(rd, (pc + 8) & 0xffc); *is_branch = true; *target = t; break; }
		case 13: return false;
		case 32: case 33: set(rd, sr[rs] + sr[rt]); break;
		case 34: case 35: set(rd, sr[rs] - sr[rt]); break;
		case 36: set(rd, sr[rs] & sr[rt]); break;
		case 37: set(rd, sr[rs] | sr[rt]); break;
		case 38: set(rd, sr[rs] ^ sr[rt]); break;
		case 39: set(rd, ~(sr[rs] | sr[rt])); break;
		case 42: set(rd, int32_t(sr[rs]) < int32_t(sr[rt])); break;
		case 43: set(rd, sr[rs] < sr[rt]); break;
		default: break;
		}
		break;
	case 1:
		switch (rt)
		{
		case 0: br(int32_t(sr[rs]) < 0); break;
		case 1: br(int32_t(sr[rs]) >= 0); break;
		case 16: { bool t = int32_t(sr[rs]) < 0; set(31, (pc + 8) & 0xffc); br(t); break; }
		case 17: { bool t = int32_t(sr[rs]) >= 0; set(31, (pc + 8) & 0xffc); br(t); break; }
		}
		break;
	case 2: *is_branch = true; *target = (w & 0x3ff) << 2; break;
	case 3: set(31, (pc + 8) & 0xffc); *is_branch = true; *target = (w & 0x3ff) << 2; break;
	case 4: br(sr[rs] == sr[rt]); break;
	case 5: br(sr[rs] != sr[rt]); break;
	case 6: br(int32_t(sr[rs]) <= 0); break;
	case 7: br(int32_t(sr[rs]) > 0); break;
	case 8: case 9: set(rt, sr[rs] + simm); break;
	case 10: set(rt, int32_t(sr[rs]) < simm); break;
	case 11: set(rt, sr[rs] < uint32_t(simm)); break;
	case 12: set(rt, sr[rs] & uimm); break;
	case 13: set(rt, sr[rs] | uimm); break;
	case 14: set(rt, sr[rs] ^ uimm); break;
	case 15: set(rt, uimm << 16); break;
	case 16:
		if (rs == 0)
			RSP_MFC0(&s, rt, rd);
		else if (rs == 4)
			RSP_MTC0(&s, rd, rt);
		break;
	case 18:
		if (rs == 0) RSP_MFC2(&s, rt, rd, (w >> 7) & 15);
		else if (rs == 2) RSP_CFC2(&s, rt, rd);
		else if (rs == 4) RSP_MTC2(&s, rt, rd, (w >> 7) & 15);
		else if (rs == 6) RSP_CTC2(&s, rt, rd);
		break;
	case 32: { int8_t v = int8_t(dm8(s, sr[rs] + simm)); set(rt, v); break; }
	case 33: { unsigned a = sr[rs] + simm; set(rt, int16_t((dm8(s, a) << 8) | dm8(s, a + 1))); break; }
	case 35: { unsigned a = sr[rs] + simm; set(rt, (dm8(s, a) << 24) | (dm8(s, a + 1) << 16) | (dm8(s, a + 2) << 8) | dm8(s, a + 3)); break; }
	case 36: set(rt, dm8(s, sr[rs] + simm)); break;
	case 37: { unsigned a = sr[rs] + simm; set(rt, (dm8(s, a) << 8) | dm8(s, a + 1)); break; }
	case 40: st8(s, sr[rs] + simm, sr[rt]); break;
	case 41: { unsigned a = sr[rs] + simm; st8(s, a, sr[rt] >> 8); st8(s, a + 1, sr[rt]); break; }
	case 43: { unsigned a = sr[rs] + simm; for (int i = 0; i < 4; i++) st8(s, a + i, sr[rt] >> (24 - 8 * i)); break; }
	case 50: { LSOp f = lwc2_ops[rd]; if (f) f(&s, rt, (w >> 7) & 15, int16_t(w << 9) >> 9, rs); break; }
	case 58: { LSOp f = swc2_ops[rd]; if (f) f(&s, rt, (w >> 7) & 15, int16_t(w << 9) >> 9, rs); break; }
	default: break;
	}
	return true;
}

int main(int argc, char **argv)
{
	long maxn = 100000;
	int stop = -1, quiet = 0, final_only = 0;
	const char *path = nullptr;
	for (int a = 1; a < argc; a++)
	{
		if (!strcmp(argv[a], "-n") && a + 1 < argc) maxn = atol(argv[++a]);
		else if (!strcmp(argv[a], "-s") && a + 1 < argc) stop = int(strtol(argv[++a], nullptr, 0));
		else if (!strcmp(argv[a], "-q")) quiet = 1;
		else if (!strcmp(argv[a], "--final")) final_only = 1;
		else path = argv[a];
	}
	if (!path)
	{
		fprintf(stderr, "usage: %s [-n max] [-s stop_pc] [-q] [--final] file.snap\n", argv[0]);
		return 2;
	}
	FILE *f = fopen(path, "rb");
	SnapHdr h;
	static PrspNativeSnap before, after;
	if (!f || fread(&h, sizeof(h), 1, f) != 1 || h.magic != SNAP_MAGIC || fread(&before, sizeof(before), 1, f) != 1)
	{
		fprintf(stderr, "%s: not a snapshot\n", path);
		return 1;
	}
	world();
	for (unsigned i = 0; i < h.n_rows; i++)
	{
		uint32_t rh[2], w;
		if (fread(rh, 4, 2, f) != 2)
			return 1;
		for (uint32_t j = 0; j < rh[1]; j += 4)
		{
			if (fread(&w, 4, 1, f) != 1)
				return 1;
			memcpy(g_rdram + ((rh[0] + j) & 0x7ffffc), &w, 4);
		}
	}
	if (fread(&after, sizeof(after), 1, f) != 1)
		return 1;
	fclose(f);
	prsp_native_load(&before);
	CPUState &s = cpu.get_state();
	if (stop < 0)
		stop = int(h.exit_pc);
	uint32_t pc = h.entry_pc;
	long n = 0;
	bool pending = false;
	uint32_t ptarget = 0;
	Regs prev, cur;
	grab(s, prev);
	bool running = true;
	while (running && n < maxn)
	{
		if (!pending && int(pc) == stop && n > 0)
			break;
		uint32_t w = g_imem[pc >> 2];
		uint32_t target;
		bool isbr;
		g_cur_pc = pc;
		{
			const unsigned op_ = w >> 26;
			if (op_ == 32 || op_ == 33 || op_ == 35 || op_ == 36 || op_ == 37 || op_ == 40 || op_ == 41 || op_ == 43) scalar_footprint(w, s.sr);
			else if (op_ == 50 || op_ == 58) vec_footprint(w, s.sr);
			else if (op_ == 16 && ((w >> 21) & 31) == 4) dma_footprint((w >> 11) & 31, s.sr[(w >> 16) & 31]);
		}
		running = exec(s, pc, w, &target, &isbr);
		if ((w >> 26) == 16 && ((w >> 21) & 31) == 4)
		{
			// the RDP runs synchronously at the DPC_END write (as the host core does): CURRENT = END, START_VALID cleared
			const unsigned cr = (w >> 11) & 31;
			if (cr == 9) { g_regs[10] = g_regs[9]; g_regs[11] &= ~(1u << 10); }
		}
		n++;
		if (!final_only)
		{
			grab(s, cur);
			std::string d;
			diff(prev, cur, d);
			prev = cur;
			if (!quiet || !d.empty())
				printf("%03x %s%s\n", pc, fixup(RSP::disassemble(pc, w)).c_str(), d.c_str());
		}
		if (pending)
		{
			pc = ptarget;
			pending = false;
			continue;
		}
		if (isbr)
		{
			pending = true;
			ptarget = (target == ~0u) ? ((pc + 8) & 0xffc) : target;
			pc = (pc + 4) & 0xffc;
			continue;
		}
		pc = (pc + 4) & 0xffc;
	}
	printf("-- %ld instructions, ended at pc %03x (JIT stop %03x)\n", n, pc, h.exit_pc);
	PrspNativeSnap now;
	prsp_native_save(&now);
	int bad = 0;
	for (int i = 1; i < 32; i++)
		if (now.sr[i] != after.sr[i]) { printf("sr[%d]: trace %08x JIT %08x\n", i, now.sr[i], after.sr[i]); bad++; }
	for (int i = 0; i < 32; i++)
		if (memcmp(now.vr[i], after.vr[i], 16)) { printf("vr[%d]: trace %s JIT %s\n", i, vec_str(now.vr[i]).c_str(), vec_str(after.vr[i]).c_str()); bad++; }
	if (memcmp(now.acc, after.acc, 48)) { printf("acc differs\n"); bad++; }
	if (memcmp(now.flags, after.flags, sizeof(now.flags))) { printf("flags differ\n"); bad++; }
	if (now.div_out != after.div_out || now.div_in != after.div_in || now.dp_flag != after.dp_flag) { printf("div differs\n"); bad++; }
	for (int i = 0; i < 1024; i++)
		if (now.dmem[i] != after.dmem[i]) { if (bad < 20) printf("DMEM[%03x]: trace %08x JIT %08x\n", i * 4, now.dmem[i], after.dmem[i]); bad++; }
	for (int i = 0; i < 16; i++)
		if (now.cr[i] != after.cr[i] && !(i >= 8 && i <= 10)) { printf("cr[%d]: trace %08x JIT %08x\n", i, now.cr[i], after.cr[i]); bad++; }
	printf("%s\n", bad ? "DIFFERS from the JIT's result" : "state equals the JIT's result");
	{
		static const char key[] = "SW Version";
		for (int a = 0; a < 4096 - 40; a++) {
			int k = 0; while (key[k] && dm8(s, a + k) == (uint8_t)key[k]) k++;
			if (!key[k]) { char b[41]; for (int i = 0; i < 40; i++) { uint8_t c = dm8(s, a + i); b[i] = (c >= 32 && c < 127) ? c : '.'; } b[40] = 0; printf("VER %s\n", b); break; }
		}
	}
	{
		auto ranges = [](const uint8_t *m, const char *tag, const uint16_t *pc) {
			for (int i = 0; i < 4096;) {
				if (!m[i]) { i++; continue; }
				int j = i; while (j < 4096 && m[j]) j++;
				if (pc) printf("%s %03x-%03x pc0=%03x\n", tag, i, j, pc[i]); else printf("%s %03x-%03x\n", tag, i, j);
				i = j;
			}
		};
		ranges(g_li, "LI", g_lipc);
		ranges(g_wmay, "WMAY", nullptr);
		ranges(g_wmust, "WMUST", nullptr);
	}
	return bad != 0;
}
