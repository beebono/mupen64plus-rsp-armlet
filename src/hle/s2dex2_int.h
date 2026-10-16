/*
 * arMLEt - s2dex2_int.h: what the S2DEX 2.x closed forms (s2dex2_bg.c, s2dex2_rect.c) share: DMEM access, the 16-bit saturation, the microcode's vector constants
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ARMLET_S2DEX2_INT_H
#define ARMLET_S2DEX2_INT_H

#include <stdint.h>

/* test builds only (scripts/build.sh native --tools --hmut=N, tools/hle/test_s2dex2.sh): deliberate errors, to show that the tests catch them (docs/testing.md) */
#ifdef ARMLET_HLE_MUTATE
#define MUT(n, ok, bad) (ARMLET_HLE_MUTATE == (n) ? (bad) : (ok))
#else
#define MUT(n, ok, bad) (ok)
#endif

/* ---- DMEM, big-endian, any byte address (wrapping inside DMEM; the image is the host words that hold the big-endian words, as everywhere in src/hle) ------------------------------ */
static inline unsigned m8(const uint32_t *dm, unsigned a) { return ((const uint8_t *)dm)[(a ^ 3) & 0xfff]; }
static inline unsigned m16(const uint32_t *dm, unsigned a) { return m8(dm, a) << 8 | m8(dm, a + 1); }
static inline uint32_t m32(const uint32_t *dm, unsigned a) { return (uint32_t)m16(dm, a) << 16 | m16(dm, a + 2); }
static inline void w8(uint32_t *dm, unsigned a, unsigned v) { ((uint8_t *)dm)[(a ^ 3) & 0xfff] = (uint8_t)v; }
static inline void w16(uint32_t *dm, unsigned a, unsigned v) { w8(dm, a, v >> 8); w8(dm, a + 1, v); }
static inline void w32(uint32_t *dm, unsigned a, uint32_t v) { w16(dm, a, v >> 16); w16(dm, a + 2, v); }

static inline int32_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }

/* The microcode's vector constants v31 .. v28 (DMEM 0x180 .. 0x1bf, the data segment of every S2DEX 2.08 task). The closed forms have them folded in and decline any other. */
static const uint32_t k_consts[16] = {
    0x08000040, 0x0002ffff, 0x40000003, 0x0008fff8,         /* v31 */
    0x01008000, 0xfe000000, 0x10000400, 0x20000200,         /* v30 */
    0x3d100001, 0x32000000, 0x27000000, 0x35242628,         /* v29: RDP command ids */
    0xfffc0400, 0x00200010, 0xfc0003ff, 0x001fffe0,         /* v28 */
};

#endif
