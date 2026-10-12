/*
 * arMLEt - test_dma.c: "test.dma", a framework test routine; the SP-DMA helper at IMEM 0xfd8 that every F3DEX2 ucode ends with
 *
 *   fd8  t3 = DMA_FULL; while (t3) t3 = DMA_FULL;  t3 = DMA_FULL      (the JIT's DMA is synchronous: always 0)
 *   fe4  DMA_CACHE = s4
 *   fe8  if (s4 < 0) { DMA_DRAM = t8; DMA_WRITE_LENGTH = s3 } else { DMA_DRAM = t8; DMA_READ_LENGTH = s3 }
 *        jr ra
 *
 * It saves nothing (a stub call costs more than the ten instructions), so it is off unless asked for by name (GfxNativeRoutines=test.dma).
 * Its job is to exercise the whole path on real games: stub, dynamic exit, SP register writes with a real DMA, verify.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "native.h"

#define R_T3 11
#define R_S3 19
#define R_S4 20
#define R_T8 24
#define R_RA 31

/* Test builds only (scripts/build.sh native --nmut=N): one deliberate error each, to show which part of verify catches it.
 *  1 a register: t3 (sr[11]) ends as 1 instead of 0         2 another register: sr[2] is changed
 *  3 DMEM: a word of DMEM is changed                    4 a wrong DMA length register (one byte short)
 *  5 the wrong exit pc (ra + 4)                         6 a missing SP register write (SP_DRAM_ADDR)
 *  7 the DMA write rows go to the wrong RDRAM address   8 a vector register is changed
 *  9, 10  lane masks: the routine claims lane 3 of v17 dead (dead_vr_lanes); 9 changes that lane (verify must not report it), 10 changes lane 4 (it must)
 */
#ifndef ARMLET_NATIVE_MUTATE
#define ARMLET_NATIVE_MUTATE 0
#endif

static int test_dma(struct armlet_env *e)
{
    uint32_t *sr = e->rsp.sr;
    uint32_t full = armlet_mfc0(e, 5, R_T3);
    uint32_t len = sr[R_S3];

    if (full)                           /* the JIT would spin here */
        return ARMLET_NATIVE_DECLINE;
    sr[R_T3] = armlet_mfc0(e, 5, R_T3) + (ARMLET_NATIVE_MUTATE == 1);
    armlet_mtc0(e, 0, sr[R_S4]);
    if (ARMLET_NATIVE_MUTATE != 6)
        armlet_mtc0(e, 1, ARMLET_NATIVE_MUTATE == 7 && (int32_t)sr[R_S4] < 0 ? sr[R_T8] + 8 : sr[R_T8]);
    if (ARMLET_NATIVE_MUTATE == 4)
        len--;
    armlet_mtc0(e, (int32_t)sr[R_S4] < 0 ? 3 : 2, len);
    if (ARMLET_NATIVE_MUTATE == 2)
        sr[2] ^= 1;
    if (ARMLET_NATIVE_MUTATE == 3)
        e->rsp.dmem[0x7f0 >> 2] ^= 0x100;
    if (ARMLET_NATIVE_MUTATE == 8)
        e->rsp.vr[17][3] ^= 0x10;
    if (ARMLET_NATIVE_MUTATE == 9)
        e->rsp.vr[17][3] ^= 0x10;
    if (ARMLET_NATIVE_MUTATE == 10)
        e->rsp.vr[17][4] ^= 0x10;
    e->units++;
    return (int)((sr[R_RA] + (ARMLET_NATIVE_MUTATE == 5 ? 4 : 0)) & 0xffc);
}

#if ARMLET_NATIVE_MUTATE == 9 || ARMLET_NATIVE_MUTATE == 10
#define TEST_LANES .dead_vr_lanes = { [17] = 0x08 },
#else
#define TEST_LANES
#endif

#define TEST_DMA(var, id) \
    ARMLET_ROUTINE(var) = { .name = "test.dma", .ucode = id, .entry_pc = 0xfd8, .dep_lo = 0xfd8, .dep_hi = 0x1000, \
                            .imem_hash = 0x2a7e21705a2bcf3bull, TEST_LANES .run = test_dma }

TEST_DMA(l_dk64, 0x48c7e653a6adcda8ull);
TEST_DMA(l_snap, 0xd576cfcfed9d9c18ull);
TEST_DMA(l_pm, 0xc734403d56034719ull);
TEST_DMA(l_oot, 0x0d3f849c8dd309c4ull);

/* "test.hook": the routine of replay's --hookbench, a synthetic program in which a loop calls a two-instruction subroutine (`jr ra`) at IMEM 0x100. Measures what a
 * call of a native routine costs, from the JIT block that calls it to the one it returns to. */
static int test_hook(struct armlet_env *e)
{
    e->units++;
    return (int)(e->rsp.sr[R_RA] & 0xffc);
}

ARMLET_ROUTINE(l_hook) = { .name = "test.hook", .ucode = 0x0123456789abcdefull, .entry_pc = 0x100, .dep_lo = 0x100, .dep_hi = 0x108,
                           .imem_hash = 0xc84dc27f29a28935ull, .run = test_hook };

/* "test.dep2": replay's --dep2test; the same subroutine at 0x100 with a second dependency range [0x400, 0x408): a change of IMEM between the ranges must not drop it */
static unsigned l_dep2_calls;

static int test_dep2(struct armlet_env *e)
{
    l_dep2_calls++;
    return (int)(e->rsp.sr[R_RA] & 0xffc);
}

unsigned armlet_test_dep2_calls(void)
{
    return l_dep2_calls;
}

ARMLET_ROUTINE(l_dep2) = { .name = "test.dep2", .ucode = 0x0123456789abcdf0ull, .entry_pc = 0x100, .dep_lo = 0x100, .dep_hi = 0x108, .dep2_lo = 0x400, .dep2_hi = 0x408,
                           .imem_hash = 0xeb33598ddd1bf87dull, .run = test_dep2 };
