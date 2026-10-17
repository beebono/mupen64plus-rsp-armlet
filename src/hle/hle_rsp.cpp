/*
 * arMLEt - hle_rsp.cpp: the RSP interpreter of the task-level native path (src/hle/hle.h)
 *
 * Runs the microcode's own instructions on the real DMEM / IMEM and the JIT's register file: the scalar unit in plain code (memory accesses wrap inside DMEM as the
 * RSP's do, branches have one delay slot), the vector unit and the vector loads and stores through the fork's per-instruction helpers (the JIT's helper-call path,
 * which its inline code is tested against). MFC0 and MTC0 go through the framework (src/hle/hle_core.c), so DMA, RDP lists and the SP_STATUS poll counting happen
 * exactly as for the JIT. Anything the interpreter does not know declines the task. It is the exact reference of the task-level path (docs/design.md), not a fast path.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>

#include "m64p_plugin.h"

#include "rsp_jit.hpp"
#include "rsp_op.hpp"
#include "prsp_native.h"

#include "hle.h"
#include "hle_internal.h"

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
using namespace RSP;

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


static inline uint8_t dm8(const hle_task *t, unsigned a) { return ((const uint8_t *)t->dmem)[(a & 0xfff) ^ 3]; }
static inline void sm8(hle_task *t, unsigned a, uint8_t v) { ((uint8_t *)t->dmem)[(a & 0xfff) ^ 3] = v; }

static int decline(hle_task *t, const char *fmt, unsigned a, unsigned b)
{
	static char buf[8][96];
	static unsigned n;
	char *s = buf[n++ & 7];
	snprintf(s, sizeof(buf[0]), fmt, a, b);
	return hle_decline(t, s);
}

// HLE_RSP_PROFILE=1: executions per pc of the task that just ended go to stderr as "PROF pc count" lines (sum them over a run to see where a task spends its instructions)
static int g_prof_on = -1;
static uint64_t g_prof[1024];
static void prof_dump(void)
{
	for (int i = 0; i < 1024; i++)
		if (g_prof[i])
		{
			fprintf(stderr, "PROF %03x %llu\n", i * 4, (unsigned long long)g_prof[i]);
			g_prof[i] = 0;
		}
}

extern "C" int hle_rsp_run(struct hle_task *t, struct hle_cpu *c)
{
	CPUState &s = t->dx ? *static_cast<CPUState *>(t->dx->cpu) : RSP::cpu.get_state();	// (a detached replay has a register file and vector unit of its own)
	uint32_t *r = c->r;
	int pending = 0, ran = 0;
	unsigned ptarget = 0;
	static int dbg = -1, trace = -1, watch = -2, trace_pc = -2;

	if (g_prof_on < 0)
		g_prof_on = getenv("HLE_RSP_PROFILE") != nullptr;
	if (trace < 0)
		trace = getenv("HLE_RSP_TRACE") != nullptr;
	if (trace_pc == -2)
		trace_pc = getenv("HLE_RSP_TRACE_PC") ? (int)strtoul(getenv("HLE_RSP_TRACE_PC"), nullptr, 16) : -1;
	if (dbg < 0)
	{
		const char *e = getenv("HLE_RSP_DEBUG");
		dbg = e ? atoi(e) : 0;
	}
	if (watch == -2)
	{
		const char *e = getenv("HLE_RSP_WATCH");
		watch = e ? (int)strtoul(e, nullptr, 16) : -1;
	}
	unsigned wlast = watch >= 0 ? ((uint8_t *)t->dmem)[watch ^ 3] : 0;
	const bool dev = g_prof_on || dbg || trace || watch >= 0;
	r[0] = 0;

	for (;;)
	{
		uint32_t w;
		unsigned next;
		bool branch = false, taken = false;
		unsigned target = 0;

		if (!pending && ran && c->pc == c->stop_pc)
			return HLE_STOP;
		if (++c->steps > c->max_steps)
			return decline(t, "interpreter step limit at pc %03x%.0u", c->pc, 0);
		ran = 1;
		w = t->imem[(c->pc >> 2) & 1023];
		if (__builtin_expect(dev, 0))
		{
			if (g_prof_on)
				g_prof[(c->pc >> 2) & 1023]++;
			if (dbg && c->steps <= (unsigned)dbg)
				fprintf(stderr, "%5u %03x %08x\n", c->steps, c->pc, w);
		}
		const unsigned op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, sa = (w >> 6) & 31;
		const int32_t simm = (int16_t)w;
		const uint32_t uimm = w & 0xffff;
		next = (c->pc + 4) & 0xffc;

#define SET(n, v) do { if (n) r[n] = (v); } while (0)
#define BR(cond) do { branch = true; taken = (cond); target = (c->pc + 4 + ((unsigned)simm << 2)) & 0xffc; } while (0)
		if ((w >> 25) == 0x25)
		{
			VUOp f = vu_ops[w & 63];
			if (!f)
				f = RSP_RESERVED;
			f(&s, (w >> 6) & 31, rd, rt, rs & 15);
		}
		else
			switch (op)
			{
			case 0:
				switch (w & 63)
				{
				case 0: SET(rd, r[rt] << sa); break;
				case 2: SET(rd, r[rt] >> sa); break;
				case 3: SET(rd, (uint32_t)((int32_t)r[rt] >> sa)); break;
				case 4: SET(rd, r[rt] << (r[rs] & 31)); break;
				case 6: SET(rd, r[rt] >> (r[rs] & 31)); break;
				case 7: SET(rd, (uint32_t)((int32_t)r[rt] >> (r[rs] & 31))); break;
				case 8: branch = taken = true; target = r[rs] & 0xffc; break;
				case 9: branch = taken = true; target = r[rs] & 0xffc; SET(rd, (c->pc + 8) & 0xffc); break;
				case 13:
					if (pending)
						return decline(t, "BREAK in a delay slot at %03x%.0u", c->pc, 0);
					hle_break(t, next);
					if (g_prof_on)
						prof_dump();
					return HLE_OK;
				case 32: case 33: SET(rd, r[rs] + r[rt]); break;
				case 34: case 35: SET(rd, r[rs] - r[rt]); break;
				case 36: SET(rd, r[rs] & r[rt]); break;
				case 37: SET(rd, r[rs] | r[rt]); break;
				case 38: SET(rd, r[rs] ^ r[rt]); break;
				case 39: SET(rd, ~(r[rs] | r[rt])); break;
				case 42: SET(rd, (int32_t)r[rs] < (int32_t)r[rt]); break;
				case 43: SET(rd, r[rs] < r[rt]); break;
				default: return decline(t, "unsupported SPECIAL instruction %08x at %03x", w, c->pc);
				}
				break;
			case 1:
				switch (rt)
				{
				case 0: BR((int32_t)r[rs] < 0); break;
				case 1: BR((int32_t)r[rs] >= 0); break;
				case 16: { bool tk = (int32_t)r[rs] < 0; SET(31, (c->pc + 8) & 0xffc); BR(tk); break; }
				case 17: { bool tk = (int32_t)r[rs] >= 0; SET(31, (c->pc + 8) & 0xffc); BR(tk); break; }
				default: return decline(t, "unsupported REGIMM instruction %08x at %03x", w, c->pc);
				}
				break;
			case 2: branch = taken = true; target = (w & 0x3ff) << 2; break;
			case 3: branch = taken = true; target = (w & 0x3ff) << 2; SET(31, (c->pc + 8) & 0xffc); break;
			case 4: BR(r[rs] == r[rt]); break;
			case 5: BR(r[rs] != r[rt]); break;
			case 6: BR((int32_t)r[rs] <= 0); break;
			case 7: BR((int32_t)r[rs] > 0); break;
			case 8: case 9: SET(rt, r[rs] + (uint32_t)simm); break;
			case 10: SET(rt, (int32_t)r[rs] < simm); break;
			case 11: SET(rt, r[rs] < (uint32_t)simm); break;
			case 12: SET(rt, r[rs] & uimm); break;
			case 13: SET(rt, r[rs] | uimm); break;
			case 14: SET(rt, r[rs] ^ uimm); break;
			case 15: SET(rt, uimm << 16); break;
			case 16:
				if (rs == 0)
				{
					uint32_t v;
					if ((rd & 15) == 4)
					{
						if (hle_poll_status(t, rt, &v))
							return hle_decline(t, "SP_STATUS polled too often (the JIT would halt)");
					}
					else
						v = *t->cr[rd & 15];
					SET(rt, v);
				}
				else if (rs == 4)
				{
					if (hle_mtc0(t, rd, r[rt]))
						return hle_decline(t, "an SP register write halted the RSP");
				}
				else
					return decline(t, "unsupported COP0 instruction %08x at %03x", w, c->pc);
				break;
			case 18:
				if (rs == 0) RSP_MFC2(&s, rt, rd, (w >> 7) & 15);
				else if (rs == 2) RSP_CFC2(&s, rt, rd);
				else if (rs == 4) RSP_MTC2(&s, rt, rd, (w >> 7) & 15);
				else if (rs == 6) RSP_CTC2(&s, rt, rd);
				else return decline(t, "unsupported COP2 instruction %08x at %03x", w, c->pc);
				break;
			case 32: SET(rt, (uint32_t)(int32_t)(int8_t)dm8(t, r[rs] + simm)); break;
			case 33: { unsigned a = r[rs] + simm; SET(rt, (uint32_t)(int32_t)(int16_t)((dm8(t, a) << 8) | dm8(t, a + 1))); break; }
			case 35:
			{
				unsigned a = r[rs] + simm;
				SET(rt, (uint32_t)((dm8(t, a) << 24) | (dm8(t, a + 1) << 16) | (dm8(t, a + 2) << 8) | dm8(t, a + 3)));
				break;
			}
			case 36: SET(rt, dm8(t, r[rs] + simm)); break;
			case 37: { unsigned a = r[rs] + simm; SET(rt, (dm8(t, a) << 8) | dm8(t, a + 1)); break; }
			case 40: sm8(t, r[rs] + simm, (uint8_t)r[rt]); break;
			case 41: { unsigned a = r[rs] + simm; sm8(t, a, (uint8_t)(r[rt] >> 8)); sm8(t, a + 1, (uint8_t)r[rt]); break; }
			case 43: { unsigned a = r[rs] + simm; for (int k = 0; k < 4; k++) sm8(t, a + k, (uint8_t)(r[rt] >> (24 - 8 * k))); break; }
			case 50:
			{
				LSOp f = lwc2_ops[rd];
				if (!f)
					return decline(t, "unsupported vector load %08x at %03x", w, c->pc);
				f(&s, rt, (w >> 7) & 15, int16_t(w << 9) >> 9, rs);
				break;
			}
			case 58:
			{
				LSOp f = swc2_ops[rd];
				if (!f)
					return decline(t, "unsupported vector store %08x at %03x", w, c->pc);
				f(&s, rt, (w >> 7) & 15, int16_t(w << 9) >> 9, rs);
				break;
			}
			default:
				return decline(t, "unsupported instruction %08x at %03x", w, c->pc);
			}
#undef SET
#undef BR
		r[0] = 0;
		if (__builtin_expect(dev, 0))
		{
			if (watch >= 0)
			{ // HLE_RSP_WATCH=hexaddr: every change of that DMEM byte, with the pc of the instruction that did it
				unsigned b = ((uint8_t *)t->dmem)[watch ^ 3];
				if (b != wlast)
					fprintf(stderr, "WATCH %03x: %02x -> %02x at pc %03x (word %08x)\n", watch, wlast, b, c->pc, w);
				wlast = b;
			}
			if (trace && (trace_pc < 0 || (int)c->pc == trace_pc))
			{ // HLE_RSP_TRACE=1: instruction by instruction: pc, word, the vector register written (else a few scalar registers); HLE_RSP_TRACE_PC=hex: only that pc
				unsigned vr = (w >> 25) == 0x25 ? (w >> 6) & 31 : (op == 50 ? rt : 99);
				fprintf(stderr, "T %03x %08x", c->pc, w);
				if (vr < 32)
				{
					fprintf(stderr, " v%u=[", vr);
					for (unsigned q = 0; q < 8; q++)
						fprintf(stderr, "%04x%c", s.cp2.regs[vr].e[q], q == 7 ? ']' : ' ');
				}
				else
					fprintf(stderr, " r8=%08x r9=%08x r10=%08x r1=%08x r6=%08x r15=%08x r5=%08x r2=%08x r3=%08x r4=%08x", r[8], r[9], r[10], r[1], r[6], r[15], r[5], r[2], r[3], r[4]);
				fprintf(stderr, "\n");
			}
		}
		if (pending)
		{ // the delay slot just ran
			if (branch)
				return decline(t, "branch in a delay slot at %03x%.0u", c->pc, 0);
			c->pc = ptarget;
			pending = 0;
			continue;
		}
		if (branch)
		{
			pending = 1;
			ptarget = taken ? target : ((c->pc + 8) & 0xffc);
		}
		c->pc = next;
	}
}

#ifdef ARMLET_NATIVE
// the detached replay's register file and vector unit (hle_internal.h, struct hle_dx)
extern "C" void *hle_dx_cpu_new(void)
{
	return new (std::nothrow) CPUState();
}

extern "C" void hle_dx_cpu_free(void *cpu)
{
	delete static_cast<CPUState *>(cpu);
}

extern "C" uint32_t *hle_dx_cpu_sr(void *cpu)
{
	return static_cast<CPUState *>(cpu)->sr;
}

extern "C" void hle_dx_cpu_load(void *cpu, const PrspNativeSnap *o, uint32_t *dmem, uint32_t *imem, uint32_t *rdram)
{
	CPUState &s = *static_cast<CPUState *>(cpu);
	s.pc = o->pc;
	s.has_delay_slot = o->has_delay_slot;
	s.branch_target = o->branch_target;
	s.dirty_blocks = o->dirty_blocks;
	memcpy(s.sr, o->sr, sizeof(o->sr));
	for (int i = 0; i < 32; i++)
		memcpy(s.cp2.regs[i].e, o->vr[i], 16);
	for (int i = 0; i < 3; i++)
		memcpy(s.cp2.flags[i].e, o->flags[i], 32);
	memcpy(s.cp2.acc.e, o->acc, 48);
	s.cp2.div_out = o->div_out;
	s.cp2.div_in = o->div_in;
	s.cp2.dp_flag = o->dp_flag;
	s.dmem = dmem;
	s.imem = imem;
	s.rdram = rdram;
}
#endif
