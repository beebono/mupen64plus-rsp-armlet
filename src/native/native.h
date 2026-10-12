/*
 * arMLEt - native.h: the interface of a native microcode routine (docs/design.md, "Native routines")
 *
 * A native routine replaces a stretch of RSP microcode, from an entry pc to an exit pc, with C / NEON code that works on the RSP state.
 * The JIT hands control to it when it reaches the entry (the content of IMEM there is checked first), and continues at the pc the routine
 * returns. The routine must leave the RSP exactly as the microcode would (the exactness contract below); in verify mode the JIT runs the same
 * stretch from the same state and the two are compared.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ARMLET_NATIVE_H
#define ARMLET_NATIVE_H

#include <stddef.h>
#include <stdint.h>

#include "prsp_native.h"

/* Return value of a routine that does not take the call: the state must be exactly as it was at entry (decide before the first write, or
 * work on copies). The JIT then runs its own code for the routine. */
#define ARMLET_NATIVE_DECLINE (-1)

/* The RSP state. The pointers are into the JIT's CPU state and stay valid for the plugin's life; values are exactly as the JIT keeps them.
 *
 *  sr[1..31]      the GPRs ($1..$31, sr[0] is not maintained: read it as 0, never write it). sr[32] is the JIT's own scratch; do not touch.
 *  vr[n][i]       vector register n, element i (i = 0 is the first halfword in memory order, the most significant element).
 *  acc[0..7]      accumulator bits 47..32 (HI),  acc[8..15]  bits 31..16 (MD),  acc[16..23]  bits 15..0 (LO), lane i of each.
 *  flags[0..2]    VCO, VCC, VCE; 16 lanes of 0 or 0xffff each. Flag bit n (0..7) is lane 8 + n, bit 8 + n is lane n (the order CFC2 returns).
 *                 VCE has only the low byte (lanes 8..15), lanes 0..7 are 0.
 *  div_out/div_in/dp_flag   the reciprocal unit (VRCP/VRSQ result, the pending input half, the double-precision flag).
 *  dmem[w]        word w of DMEM as the big-endian value, held in a host uint32 (so a 32-bit load is dmem[a >> 2]); use the accessors below
 *                 for bytes and halfwords. imem likewise (read only: a routine must not change IMEM except through an SP DMA).
 */
struct armlet_rsp {
    uint32_t *sr;
    uint16_t (*vr)[8];
    uint16_t *acc;
    uint16_t (*flags)[16];
    int16_t *div_out, *div_in;
    int8_t *dp_flag;
    uint32_t *dmem;
    const uint32_t *imem;
};

/* One call. Everything a routine may do to the world beyond the registers and DMEM goes through the env (SP register writes, DMA), because
 * verify mode has to run it in a sandbox. Never touch RDRAM or the RDP directly. */
struct armlet_env {
    struct armlet_rsp rsp;
    unsigned entry_pc;                  /* IMEM byte offset the routine was entered at */
    int mode;                           /* what the instruction that caused it would make the run loop do: PRSP_NATIVE_MODE_CONTINUE unless
                                           an env helper reports otherwise (a DMA into IMEM that changed code, an SP_STATUS write that halts);
                                           the routine returns its exit pc as usual and the framework leaves the run loop with this mode */
    const struct armlet_routine *routine;
    uint32_t units;                     /* a routine adds the vertices / triangles it processed (statistics: native cost per unit) */
};

/* The routine. Returns the pc (IMEM byte offset, word aligned) the RSP continues at, or ARMLET_NATIVE_DECLINE. */
typedef int (*armlet_routine_fn)(struct armlet_env *e);

/* The exactness contract: when the routine returns pc P, every piece of RSP state is what the microcode run from the entry to the first
 * arrival at P leaves, except what the routine's masks declare dead:
 *   GPRs sr[1..31]; vr[0..31]; acc; VCO, VCC, VCE; div_out, div_in, dp_flag; DMEM; IMEM; the SP / DPC registers (the sequence of effectful
 *   writes to them, see PRSP_EV_*, and their final values); the status-poll counters; the RDRAM written by SP DMA and the RDP lists submitted.
 * A routine is bound to a ucode (FNV-1a-64 id of its text, gfx_ucode.c) and to the IMEM content from dep_lo to dep_hi; both are checked when
 * the JIT first reaches the entry after any change of that code, never per call. DMEM data the routine relies on being constant must be read
 * from DMEM, or verified, per call. */
struct armlet_routine {
    const char *name;                   /* "f3dex2.vtx": family.routine; allow-lists match the whole name or a leading family */
    uint64_t ucode;                     /* ucode id the routine belongs to */
    unsigned entry_pc;
    unsigned dep_lo, dep_hi;            /* IMEM byte range [lo, hi) hashed; must contain the whole stretch the routine replaces */
    uint64_t imem_hash;                 /* armlet_native_imem_hash(imem, dep_lo, dep_hi); the dev log prints candidates with their hash */
    const uint16_t *exit_pcs;           /* every static pc the stretch can exit at (also region split points); dynamic exits (jr) need not be
                                           listed, but the JIT's run is stopped at the pc the routine returns, so it must be where the microcode
                                           first arrives. Terminated by n_exit_pcs. */
    unsigned n_exit_pcs;
    armlet_routine_fn run;
    /* Live-out masks: what the routine may leave different from the microcode because the code at its exit pcs overwrites it before reading it
     * (justify with tools/native/liveness.py; verify excludes exactly these; ARMLET_NATIVE_POISON=1 fills them with a pattern after the
     * routine so that a wrong claim shows up downstream). All zero = full state equality (the default and the goal). */
    uint32_t dead_sr;                   /* bit n: sr[n] */
    uint32_t dead_vr;                   /* bit n: vr[n] */
    unsigned dead_acc : 1, dead_flags : 1, dead_div : 1;
};

/* Registration: a table entry in section "armlet_routines" (no other file needs to know about it). */
#define ARMLET_ROUTINE(var) \
    static const struct armlet_routine var; \
    static const struct armlet_routine *const var##_ptr __attribute__((used, section("armlet_routines"))) = &var; \
    static const struct armlet_routine var

/* ---- helpers for routines ------------------------------------------------------------------------------------------------------------- */

/* DMEM accessors, byte address a (the RSP's view: big-endian). 16-bit and 32-bit accesses need alignment (the microcode's lh/lw wrap
 * unaligned ones across words; a routine that needs that must decline instead). */
static inline uint8_t armlet_dm8(const struct armlet_rsp *r, unsigned a) { return ((const uint8_t *)r->dmem)[(a ^ 3) & 0xfff]; }
static inline uint16_t armlet_dm16(const struct armlet_rsp *r, unsigned a) { return ((const uint16_t *)r->dmem)[((a ^ 2) & 0xfff) >> 1]; }
static inline uint32_t armlet_dm32(const struct armlet_rsp *r, unsigned a) { return r->dmem[(a & 0xfff) >> 2]; }
static inline void armlet_st8(const struct armlet_rsp *r, unsigned a, uint8_t v) { ((uint8_t *)r->dmem)[(a ^ 3) & 0xfff] = v; }
static inline void armlet_st16(const struct armlet_rsp *r, unsigned a, uint16_t v) { ((uint16_t *)r->dmem)[((a ^ 2) & 0xfff) >> 1] = v; }
static inline void armlet_st32(const struct armlet_rsp *r, unsigned a, uint32_t v) { r->dmem[(a & 0xfff) >> 2] = v; }

/* MTC0 / MFC0 as the microcode does them (same register numbers: 0 SP_MEM_ADDR, 1 SP_DRAM_ADDR, 2 SP_RD_LEN, 3 SP_WR_LEN, 4 SP_STATUS,
 * 8 DPC_START, 9 DPC_END, ...). A write to RD_LEN / WR_LEN runs the DMA (to or from DMEM or IMEM; an IMEM write sets e->mode when it changes
 * code); a write to DPC_END runs the RDP list (deferred / coalesced as for the JIT). Verify mode records instead of acting where it must. */
void armlet_mtc0(struct armlet_env *e, unsigned rd, uint32_t value);
uint32_t armlet_mfc0(struct armlet_env *e, unsigned rd, unsigned rt);
/* The three writes the microcode does for a DMA read / write (SP_MEM_ADDR, SP_DRAM_ADDR, then the length register); `len` is the register
 * value (bytes - 1 | rows << 12 | skip << 20). Same register effects as the JIT. */
void armlet_dma_read(struct armlet_env *e, unsigned mem_addr, unsigned dram_addr, uint32_t len);
void armlet_dma_write(struct armlet_env *e, unsigned mem_addr, unsigned dram_addr, uint32_t len);

uint64_t armlet_native_imem_hash(const uint32_t *imem, unsigned lo, unsigned hi);

/* ---- framework (native.c) ------------------------------------------------------------------------------------------------------------ */
struct armlet_ctx;
struct armlet_ucode_id;
void armlet_native_init(struct armlet_ctx *c);                 /* after parallelRSPInitiateRSP */
void armlet_native_task_begin(struct armlet_ctx *c);           /* before a graphics task goes to the JIT: selects the routines of its ucode */
void armlet_native_reset(void);                                /* RomClosed */

#endif
