/*
 * arMLEt - s2dex2_rect.c: G_OBJ_RECTANGLE of S2DEX 2.08 (docs/ucode-s2dex2.md, "Sprite rectangles") in closed form
 *
 * A sprite (uObjSprite, 24 bytes at DMEM 0x3c0) becomes SET_TILE + SET_TILE_SIZE + TEXTURE_RECTANGLE. What the microcode does with its vector registers (overlay A, 0xba8 .. 0xd7c) is
 * the same arithmetic on two axes at once; here it is written per axis a (0 = x, 1 = y), each step the closed form of the instructions that make it:
 *
 *   input       pos scale size                  s10.2 position, s5.10 texture step per pixel (1.0 = 0x400, the reciprocal of the magnification), u10.5 size in texels
 *   render mode (DMEM, left by G_OBJ_RENDERMODE) bias_s bias_p trim, the position mask, y_frac, bias_t, flip signs and tile attributes
 *   1. screen   top left tl = (pos + bias_p) & mask; width = size / scale in 10.2 pixels (the reciprocal unit, refined as the microcode does); bottom right br = tl + width
 *   2. scissor  the rectangle is cut to the scissor (0x204 ..): tl2 = max(tl, ul), br2 = min(br, lr); empty (tl2 >= br2 on either axis): nothing is drawn
 *   3. texture  the first texel (s, t) and the steps (dsdx, dtdy): the sprite's own, or mirrored (flip flags: the first texel is the last, the step negated); the part cut off the
 *               top left (tl - ul, negative) moves the first texel by cut * step in 10.2 x s5.10 -> u10.5; y also by the sub-pixel position of the cut edge
 *   4. commands the tile (toggling between two, with a SYNC_TILE kept when the toggle meets the load state), the tile size from the image size, the rectangle; in COPY cycle
 *               mode the rectangle is one pixel smaller and dsdx four times larger
 *
 * The microcode keeps 16-bit lanes and a 48-bit accumulator; the expressions below are exact for the values it can produce on its valid domain. Outside it the kernel declines
 * (S2_UNSUPPORTED, nothing written): the microcode's vector constants are not S2DEX 2.08's, a table pointer that is not 8-aligned (LDV), or a width that does not fit 15 bits
 * (a scale of 0 or negative, or one that makes the sprite wider than 8191 pixels: the accumulator saturates there). Nothing is written either when the sprite is outside the
 * scissor (S2_CLIPPED).
 *
 * Only two lanes carry the result (0 and 4), so this is plain scalar C: NEON would load eight lanes to use two, and the whole kernel is some sixty integer operations.
 * It is checked against the microcode (tools/rsp2c.py's translation of overlay A, docs/testing.md, "Regenerating the microcode oracles") on random DMEM images (tools/hle/test_s2dex2.c) and by the
 * replays and verify runs of the games (docs/testing.md).
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>
#include <string.h>

#include "hle_fx.h"
#include "s2dex2_fx.h"
#include "s2dex2_int.h"

enum {
    OBJ = 0x3c0,                            /* the uObjSprite: objX scaleW imageW paddingX  objY scaleH imageH paddingY (halfwords; the x and y groups of four), then ... */
    O_STRIDE = 0x3d0, O_ADRS = 0x3d2,       /* ... imageStride imageAdrs (TMEM words), imageFmt imageSiz imagePal imageFlags (bytes) */
    O_FMT = 0x3d4, O_SIZ = 0x3d5, O_PAL = 0x3d6, O_FLAGS = 0x3d7,
    RM_WORDS = 0x1f8,                       /* the render-mode words: bias_s trim | bias_p (unused) */
    RM_PTR_A = 0x244, RM_PTR_B = 0x246,     /* pointers to the two 8-byte render-mode tables (0x248 / 0x250 and 0x258 / 0x260) */
    SCISSOR = 0x204,                        /* ulx lrx uly lry */
    CYCLE_BYTE = 0xc9,                      /* bit 5: the cycle type is COPY */
    TILE_STATE = 0x2ae, TILE_TOGGLE = 0x2af,/* the tile the last load / rectangle used (bytes) and the 0 / 2 toggle */
    SCRATCH = 0x1c0,                        /* the word the microcode moves two table halfwords through */
};

enum { FLIP_S = 0x01, FLIP_T = 0x10 };      /* imageFlags */
enum { CMD_SYNC_TILE = 0x28, CMD_SET_TILE = 0x35, CMD_TEXRECT = 0x24, SET_TILE_SIZE_W0 = 0x32000000 };

/* The 1/scale the microcode computes with: the reciprocal unit's 32-bit result r, plus its integer half (r + (r >> 16), the carry into the integer half included, which saturates) */
static inline int32_t refined_recip(int16_t scale)
{
    uint32_t r = hle_rcp32(scale), lo = r & 0xffff, hi = r >> 16;
    uint32_t l = lo + hi;
    int32_t h = sat16((int16_t)hi + (int32_t)(l >> 16));
    return (int32_t)((uint32_t)h << 16 | (l & 0xffff));
}

int s2dex2_obj_rect(uint32_t *dm, unsigned *s7p)
{
    unsigned s7 = *s7p, a;
    unsigned ptr_a = m16(dm, RM_PTR_A), ptr_b = m16(dm, RM_PTR_B);
    /* the render mode: the words G_OBJ_RENDERMODE chose (0x1f8: bias_t = start offset of the texture, trim = taken off the size; 0x1fc: bias_p added to the positions) and the two tables */
    const int bias_t = (int16_t)m16(dm, RM_WORDS), trim = (int16_t)m16(dm, RM_WORDS + 2), bias_p = (int16_t)m16(dm, RM_WORDS + 4);
    int mask, y_frac, bias_s, sign_flip_start, sign_flip_step;
    unsigned tile_attr;
    int tl[2], br[2], tl2[2], br2[2], cut[2], first[2], step[2];
    unsigned flags = m8(dm, O_FLAGS);

    if (memcmp(dm + 0x180 / 4, k_consts, sizeof k_consts) || ((ptr_a | ptr_b) & 7))
        return S2_UNSUPPORTED;
    mask = (int16_t)m16(dm, ptr_a);                         /* position mask: 0xfffc whole pixels, 0xffff the quarter pixels */
    y_frac = (int16_t)m16(dm, ptr_a + 2);                   /* the bits of the cut edge's y that move t (the sub-pixel position) */
    bias_s = (int16_t)m16(dm, ptr_a + 4);                   /* added to s: half a texel less when filtering */
    sign_flip_start = (int16_t)m16(dm, ptr_b);              /* the factor of the mirrored first texel and of the mirrored step: +1 / -1, one each way round */
    sign_flip_step = (int16_t)m16(dm, ptr_b + 2);
    tile_attr = m32(dm, ptr_b + 4);                         /* SET_TILE's second word below the tile number: clamp / mask / shift */

    for (a = 0; a < 2; a++) {
        const unsigned o = OBJ + 8 * a;
        const int pos = (int16_t)m16(dm, o), scale = (int16_t)m16(dm, o + 2), size = (int16_t)m16(dm, o + 4);
        const int ul = (int16_t)m16(dm, SCISSOR + 4 * a), lr = (int16_t)m16(dm, SCISSOR + 2 + 4 * a);
        const int flip = (flags & (a ? MUT(158, FLIP_T, FLIP_T << 1) : FLIP_S)) != 0;
        const int32_t recip = MUT(150, refined_recip(scale), (int32_t)hle_rcp32(scale));
        const unsigned sz = (uint16_t)sat16(size - MUT(166, trim, 0));              /* the size less the trim, as the unsigned lane value */
        int64_t scaled = ((int64_t)sz * recip) >> 8;                   /* size / scale: 16 fractional bits of 10.2 pixels */
        int32_t width, start;
        int ds;

        /* 1. */
        tl[a] = (int16_t)(sat16(pos + MUT(151, bias_p, -bias_p)) & MUT(163, mask & 0xffff, 0xffff));
        if (scaled < 0 || scaled >= 0x80000000ll)
            return S2_UNSUPPORTED;
        width = (int32_t)(scaled >> MUT(161, 16, 17));
        br[a] = sat16(tl[a] + width);
        /* 2. the cut at the scissor: tl2, br2 and what was cut off the top left (<= 0) */
        tl2[a] = MUT(164, tl[a] >= ul ? tl[a] : ul, tl[a]);
        br2[a] = MUT(162, br[a] < lr ? br[a] : lr, br[a]);
        cut[a] = tl[a] >= ul ? 0 : sat16(MUT(153, tl[a] - ul, ul - tl[a]));
        /* 3. the first texel and the step. The mirrored first texel is the last: bias_t + size - trim - 1, negated if the table says so (the borrow below 0 would wrap, but only a
         * zero-width sprite, which the scissor test below throws out, gets there) */
        ds = flip ? (int16_t)((unsigned)scale * (unsigned)MUT(155, sign_flip_step, sign_flip_start)) : scale;
        start = bias_t;
        if (flip) {
            unsigned last = ((unsigned)bias_t + sz) & 0xffff;
            last = (last - MUT(154, 1, 0)) & 0xffff;
            start = (int16_t)(last * (unsigned)sign_flip_start);
        }
        start = sat16(start + MUT(160, bias_s, 0));
        step[a] = ds;
        {
            /* cut * step: 10.2 x s5.10 has 12 fractional bits, u10.5 wants 5; the microcode's multiply by 0xfe00 / 2^16 and the subtraction of the low half amount to rounding the
             * quotient up: start - ceil(P / 128) */
            int32_t P = cut[a] * ds;
            first[a] = (int16_t)(start - ((P + MUT(156, 127, 0)) >> 7));
        }
    }

    /* 2. empty after the cut: nothing is drawn */
    if (MUT(152, tl2[0] >= br2[0] || tl2[1] >= br2[1], tl2[0] > br2[0] || tl2[1] > br2[1]))
        return S2_CLIPPED;

    {
        /* t also moves with the sub-pixel position of the cut edge: y_frac bits of its y, in 1/4 pixel, times 2^9 times dtdy, 16 fractional bits (x has none) */
        int32_t frac = sat16((int16_t)((unsigned)MUT(165, tl2[1], tl[1]) & (unsigned)y_frac) * MUT(157, 512, 256));
        first[1] = sat16(first[1] - (int32_t)(((int64_t)step[1] * (uint16_t)frac) >> 16));
    }

    /* 4. the commands. The tile alternates between 0 and 2; a SYNC_TILE precedes SET_TILE when the tile equals the state byte (the load code leaves it, 0x80 + ...), and the state byte follows */
    {
        const int copy = (m8(dm, CYCLE_BYTE) & 0x20) != 0;
        const unsigned tile = m8(dm, TILE_TOGGLE) ^ MUT(32, 2u, 1u);
        const uint32_t state = (uint32_t)(int32_t)(int8_t)m8(dm, TILE_STATE);
        unsigned imageW = m16(dm, OBJ + 4), imageH = m16(dm, OBJ + 12);
        uint32_t w0, w1;

        w8(dm, TILE_TOGGLE, tile);
        w8(dm, TILE_STATE, tile);
        w8(dm, s7, CMD_SYNC_TILE);                                           /* the second word of the sync is whatever the buffer held */
        if (MUT(33, tile == state, tile != state))
            s7 += 8;
        w32(dm, SCRATCH, tile_attr);                                         /* (the microcode's store of the two table halfwords) */

        /* SET_TILE: format, texel size, line (TMEM words), address; palette and the table's attributes */
        w0 = (uint32_t)m8(dm, O_FMT) << MUT(34, 21, 20) | (uint32_t)m8(dm, O_SIZ) << 19 | (uint32_t)m16(dm, O_STRIDE) << 9 | m16(dm, O_ADRS);
        w32(dm, s7, CMD_SET_TILE << 24 | (w0 & 0xffffff));
        w1 = (uint32_t)m8(dm, O_PAL) << 20 | tile_attr;
        w32(dm, s7 + 4, tile << 24 | (w1 & 0xffffff));
        /* SET_TILE_SIZE: from the image size, (w - 1, h - 1) in the format the rectangle's tile uses */
        w32(dm, s7 + 8, SET_TILE_SIZE_W0);
        w1 = ((imageW & MUT(159, 0x7ff8u, 0x7ff0u)) << 9 | imageH >> 3) - 16388u;
        w32(dm, s7 + 12, tile << 24 | (w1 & 0xffffff));
        /* TEXTURE_RECTANGLE: the corners (10.2), the tile, s t dsdx dtdy; in COPY mode the lower right is inclusive (one pixel in) and dsdx is four times larger */
        w0 = (uint32_t)(br2[0] & 0xfff) << 12 | (uint32_t)(br2[1] & 0xfff);
        if (copy)
            w0 -= MUT(31, 16388u, 16387u);
        w32(dm, s7 + 16, CMD_TEXRECT << 24 | (w0 & 0xffffff));
        w1 = (uint32_t)(tl2[0] & 0xfff) << 12 | (uint32_t)(tl2[1] & 0xfff);
        w32(dm, s7 + 20, tile << 24 | w1);
        w16(dm, s7 + 24, (unsigned)first[0]);
        w16(dm, s7 + 26, (unsigned)first[1]);
        w16(dm, s7 + 28, (unsigned)step[0] << (copy ? MUT(30, 2, 3) : 0));
        w16(dm, s7 + 30, (unsigned)step[1]);
        s7 += 32;
    }
    *s7p = s7;
    return S2_DONE;
}
