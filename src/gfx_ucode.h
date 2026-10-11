/*
 * arMLEt - gfx_ucode.h: graphics ucode identification and the table of native handlers
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ARMLET_GFX_UCODE_H
#define ARMLET_GFX_UCODE_H

#include "armlet.h"

enum { ARMLET_FAM_OTHER, ARMLET_FAM_F3D, ARMLET_FAM_F3DEX, ARMLET_FAM_F3DEX2 };

struct armlet_ucode_id {
    uint64_t id;                        /* FNV-1a over the ucode text (ucode_size bytes, at most 4 KiB; 0xf80 when it is 0) as 32-bit RDRAM words */
    unsigned family;                    /* ARMLET_FAM_*, from the version strings in the ucode text / data */
    char name[160];                     /* those strings, e.g. "RSP Gfx ucode F3DEX fifo 2.07 ..." */
};

/* A native handler gets a fresh type-1 task of its ucode and returns nonzero when it has completed it (see docs/design.md). */
typedef int (*armlet_gfx_run_fn)(struct armlet_ctx *c, const struct armlet_ucode_id *u);
struct armlet_gfx_native {
    uint64_t id;
    armlet_gfx_run_fn run;
};

/* Identifies the ucode of the task in DMEM. *is_new tells whether this id was seen for the first time (it was logged then). */
const struct armlet_ucode_id *armlet_gfx_identify(const struct armlet_ctx *c, bool *is_new);
const struct armlet_gfx_native *armlet_gfx_native_find(uint64_t id);
bool armlet_gfx_allowed(const struct armlet_config *cfg, uint64_t id);
void armlet_gfx_reset(void);

/* Scalar reference of the ucode id: FNV-1a-64 over `bytes` of RDRAM at `addr`, one 32-bit host-endian word per step. */
uint64_t armlet_ucode_hash(const unsigned char *rdram, uint32_t addr, uint32_t bytes);

#endif
