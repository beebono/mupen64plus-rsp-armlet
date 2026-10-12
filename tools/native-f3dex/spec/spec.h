/*
 * spec.h: helpers for "spec" routines: C functions that compute what one stretch of the microcode (a command handler) leaves in DMEM, written from
 * the arithmetic of docs/ucode-f3dex.md and checked against the JIT by tools/native-f3dex/rt_test (spec.* routines compare DMEM and the exit pc only).
 *
 * A spec routine is an ordinary native routine (src/native/native.h): int run(struct armlet_env *e) returns the pc to continue at; it reads and writes DMEM
 * through armlet_dm8/16/32 and armlet_st8/16/32 (e->rsp), reads the GPRs the dispatch set up from e->rsp.sr[], and need not touch the vector registers.
 * Register the same function for every build whose handler it describes (entry / exit pcs differ per build: tools/native-f3dex/kit/pcmap).
 */
#ifndef SPEC_H
#define SPEC_H
#include <stdint.h>
#include "native.h"

#define UC_BANJO121   0x208a567e9aead5eaull     /* F3DEX 1.21 (Banjo-Kazooie) */
#define UC_SF64_NON   0xfd5a31ccf6d40b62ull     /* F3DEX.NoN 1.22 (Star Fox 64) */
#define UC_TS2_DEX    0x047252e810e35d56ull     /* F3DEX 1.23 (Toy Story 2) */
#define UC_TS2_DLX    0x7502cfb7098eeffbull     /* F3DLX 1.23 (Toy Story 2) */
#define UC_MK64_DEX   0x34dad67335a54cdaull     /* F3DEX 0.95 (Mario Kart 64) */
#define UC_MK64_DLX   0x5f33f7c2be924de0ull     /* F3DLX 0.95 */

/* SPEC_REG(variable, "spec.f3dex.<area>.<name>", UC_xxx, entry_pc, static_exit_pc or 0, run_function): a spec routine for one build. */
#define SPEC_REG(var, nm, uc, entry, exitpc, fn) \
    static const uint16_t var##_ex[] = { (exitpc) }; \
    ARMLET_ROUTINE(var) = { .name = nm, .ucode = (uc), .entry_pc = (entry), .dep_lo = (entry), .dep_hi = (entry) + 4, .imem_hash = 0, \
                            .exit_pcs = var##_ex, .n_exit_pcs = (exitpc) ? 1 : 0, .run = fn }

#endif
