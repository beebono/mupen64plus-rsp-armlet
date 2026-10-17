/*
 * arMLEt - probe_cases.h: the canned display lists of the probe (probe_cases.c)
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ARMLET_PROBE_CASES_H
#define ARMLET_PROBE_CASES_H

#include <stdint.h>

enum { PROBE_CLIP, PROBE_ZNEG, PROBE_TRI, PROBE_MIX, PROBE_WILD, PROBE_MISC, PROBE_LIT, PROBE_MTX, PROBE_REJ, PROBE_BZ, PROBE_NCASES };
extern const char *const probe_case_names[PROBE_NCASES];

enum { PROBE_MEM = 0x1200 };            /* the bytes behind the base address a case uses */

struct probe_case {
    uint32_t base;                      /* the RDRAM address of mem[0] (the display list) */
    uint32_t used;                      /* bytes of mem that hold the case (the rest is zero) */
    uint32_t dl_bytes;
    uint8_t mem[PROBE_MEM];             /* big-endian bytes of RDRAM */
};

/* case `kind` (PROBE_*) for `seed`, laid out at RDRAM address `base`; deterministic */
void probe_case_make(struct probe_case *c, unsigned kind, unsigned seed, uint32_t base);

#endif
