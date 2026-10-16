/*
 * arMLEt - s2dex2_fx.h: the vector-unit parts of S2DEX 2.x (docs/ucode-s2dex2.md) as kernels over DMEM
 *
 * The sprite and background commands of S2DEX compute with the vector unit on a handful of 16-bit fields and write RDP commands next to the microcode's own scratch words in DMEM.
 * These kernels do the same from the object structure the front-end has put in DMEM (the DMA is the front-end's), as closed forms in plain C (s2dex2_rect.c, s2dex2_bg.c). The DMEM
 * image is the host words that hold the big-endian words, as everywhere in src/hle.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ARMLET_S2DEX2_FX_H
#define ARMLET_S2DEX2_FX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { S2_DONE = 0, S2_CLIPPED = 1, S2_UNSUPPORTED = 2 };

/* G_OBJ_RECTANGLE: the 24-byte uObjSprite is at DMEM 0x3c0. The commands (SET_TILE, SET_TILE_SIZE, TEXTURE_RECTANGLE, possibly a sync before them) are written at *s7 and *s7 is advanced
 * past them; returns S2_DONE, S2_CLIPPED when the sprite is outside the scissor (nothing written, DMEM untouched), or S2_UNSUPPORTED (nothing written) when the DMEM constants are not
 * those of S2DEX 2.08 or the sprite is outside what the closed form covers (s2dex2_rect.c). */
int s2dex2_obj_rect(uint32_t *dmem, unsigned *s7);

/* G_BG_1CYC: the 40-byte uObjBg is at DMEM 0x3b0 (the DMA is the front-end's). The command writes RDP commands at *s7 up to the end of the output buffer *s6 and calls back for the two pieces of
 * the microcode that are not its own: seg(ctx, a) = the segmented address a resolved, and flush(ctx, &s7, &s6) = the flush check of the RDP output buffer for the commands written up to *s7, which may
 * move *s6 / *s7 (nonzero: give up). Returns S2_DONE at the end of the command, S2_UNSUPPORTED when it would not finish or the DMEM constants are not those of S2DEX 2.08. */
struct s2_ctx {
    void *ctx;
    uint32_t (*seg)(void *ctx, uint32_t a);
    int (*flush)(void *ctx, uint32_t *s7, uint32_t *s6);
};
int s2dex2_obj_bg1cyc(uint32_t *dmem, uint32_t *s7, uint32_t *s6, const struct s2_ctx *c);

/* G_BG_COPY (the background in the COPY cycle type, unscaled): the same object at DMEM 0x3b0, the same callbacks; returns S2_DONE, or S2_UNSUPPORTED (nothing written) when the DMEM constants are not those of S2DEX 2.08
 * or the object is outside what the closed form covers (s2dex2_bg.c). */
int s2dex2_obj_bgcopy(uint32_t *dmem, uint32_t *s7, uint32_t *s6, const struct s2_ctx *c);

#ifdef __cplusplus
}
#endif

#endif
