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
	unsigned dump_lo = 0, dump_hi = 0;
	int show_stores = 0;
	const char *path = nullptr;
	for (int a = 1; a < argc; a++)
	{
		if (!strcmp(argv[a], "-n") && a + 1 < argc) maxn = atol(argv[++a]);
		else if (!strcmp(argv[a], "-s") && a + 1 < argc) stop = int(strtol(argv[++a], nullptr, 0));
		else if (!strcmp(argv[a], "-q")) quiet = 1;
		else if (!strcmp(argv[a], "--final")) final_only = 1;
		else if (!strcmp(argv[a], "--stores")) show_stores = 1;
		else if (!strcmp(argv[a], "--dump") && a + 2 < argc) { dump_lo = unsigned(strtoul(argv[++a], nullptr, 0)); dump_hi = unsigned(strtoul(argv[++a], nullptr, 0)); }
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
		uint32_t before_dmem[1024];
		if (show_stores)
			memcpy(before_dmem, s.dmem, sizeof(before_dmem));
		running = exec(s, pc, w, &target, &isbr);
		n++;
		if (!final_only)
		{
			grab(s, cur);
			std::string d;
			diff(prev, cur, d);
			prev = cur;
			if (show_stores)
			{
				for (unsigned a = 0; a < 4096; a++)
					if (reinterpret_cast<const uint8_t *>(before_dmem)[a ^ 3] != reinterpret_cast<const uint8_t *>(s.dmem)[a ^ 3])
					{
						unsigned b = a;
						while (b + 1 < 4096 && reinterpret_cast<const uint8_t *>(before_dmem)[(b + 1) ^ 3] != reinterpret_cast<const uint8_t *>(s.dmem)[(b + 1) ^ 3])
							b++;
						char sb[64];
						snprintf(sb, sizeof(sb), " ST[%03x..%03x]", a, b);
						d += sb;
						a = b;
					}
			}
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
	for (unsigned a = dump_lo & ~15u; a < dump_hi; a += 16)
		printf("DMEM %03x: %08x %08x %08x %08x\n", a, now.dmem[a >> 2], now.dmem[(a >> 2) + 1], now.dmem[(a >> 2) + 2], now.dmem[(a >> 2) + 3]);
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
	return bad != 0;
}
