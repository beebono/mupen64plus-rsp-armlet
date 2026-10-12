/*
 * arMLEt - tools/f3d/snap.h: a copy of the RSP state at a routine entry or exit, and the record the replay tool reads.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef F3D_SNAP_H
#define F3D_SNAP_H
#include <stdint.h>

typedef struct Snap
{
    uint32_t sr[33];
    uint16_t vr[32][8];
    uint16_t acc[24];
    uint16_t flags[3][16];
    int16_t div_out, div_in;
    int8_t dp_flag;
    uint32_t dmem[1024], imem[1024];
    uint32_t cr[16];
    uint32_t irq, pc;
} Snap;

/* replay file: "F3DS" magic, u32 version, u32 routine id (1 vtx, 2 tri), u32 count, then count x { Snap before, Snap after (the JIT's), i32 exit pc } */
#define F3D_SNAP_MAGIC 0x53443346u
#endif
