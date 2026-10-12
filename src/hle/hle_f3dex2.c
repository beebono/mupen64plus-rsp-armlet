/*
 * arMLEt - hle_f3dex2.c: the F3DEX2 family (F3DEX2, F3DEX2.NoN, F3DZEX, F3DZEX.NoN) on the task-level path: the builds of the family as DATA
 *
 * This is the framework's first family and its reference implementation: its run() is the RSP interpreter (hle_rsp.cpp) over the whole task, which reproduces the JIT's visible
 * results exactly (task-level verify, docs/testing.md). The native front-end for these builds replaces run() later; the table below stays.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>

#include "hle.h"

#ifdef ARMLET_NATIVE

/* DMEM dead at the end of a task: the two RDP output buffers with their slack (0xba8..0xd00 + 0xb0 slack, 0xdb0..0xf08 + 0xb0). The command handlers write the commands there, the flush DMAs
 * [buffer start, write pointer) to the FIFO, and the bytes beyond the pointer are scratch of the triangle setup (it writes its shade, texture and z blocks whether or not the command has
 * them; their values depend on vector registers left by earlier commands) and the stale second words of sync commands, which the RDP ignores (the verify stream zeroes them). At the end of a
 * task everything below the pointer has been flushed.
 * Established by: this analysis and the equivalence runs of docs/testing.md (verify, tools/equiv). To be re-established with ARMLET_DMEM_POISON before a native front-end relies on it. */
static const struct hle_dead_range l_dead_out[] = { { 0xba8, 0xfb8 },
    /* the padding byte of each colour of the light slots (struct Light_t: col[3] pad, colc[3] pad, dir[3] pad; 24 bytes from 0x200): the lighting code stores whole vector lanes and the
     * pad lane holds what the vector registers had, which depends on the commands before; nothing reads the pads. */
    { 0x203, 0x204 }, { 0x207, 0x208 }, { 0x21b, 0x21c }, { 0x21f, 0x220 }, { 0x233, 0x234 }, { 0x237, 0x238 }, { 0x24b, 0x24c }, { 0x24f, 0x250 },
    { 0x263, 0x264 }, { 0x267, 0x268 }, { 0x27b, 0x27c }, { 0x27f, 0x280 }, { 0x293, 0x294 }, { 0x297, 0x298 }, { 0x2ab, 0x2ac }, { 0x2af, 0x2b0 }, { 0x2c3, 0x2c4 },
    { 0x2c7, 0x2c8 } };

static int f3dex2_run(struct hle_task *t, const struct hle_ucode *u)
{
    (void)u;
    return hle_interp_task(t);
}

static const struct hle_family l_fam = { .name = "f3dex2", .run = f3dex2_run };

#define DEAD .ring_off = 0xfe8, .dead = l_dead_out, .n_dead = sizeof(l_dead_out) / sizeof(l_dead_out[0])
HLE_UCODE(l_dk64) = { .id = 0x48c7e653a6adcda8ull, .name = "F3DEX 2.07 (Donkey Kong 64)", .fam = &l_fam, .flags = HLE_F_NEAR_CLIP, DEAD };
HLE_UCODE(l_snap) = { .id = 0xd576cfcfed9d9c18ull, .name = "F3DEX.NoN 2.08H (Pokemon Snap)", .fam = &l_fam, DEAD };
HLE_UCODE(l_pm) = { .id = 0xc734403d56034719ull, .name = "F3DEX 2.08 (Paper Mario)", .fam = &l_fam, .flags = HLE_F_NEAR_CLIP, DEAD };
HLE_UCODE(l_oot) = { .id = 0x0d3f849c8dd309c4ull, .name = "F3DZEX.NoN 2.06H (Ocarina of Time)", .fam = &l_fam, .flags = HLE_F_ZEX, DEAD };

#endif
