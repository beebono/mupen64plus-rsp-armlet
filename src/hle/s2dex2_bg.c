/*
 * arMLEt - s2dex2_bg.c: G_BG_1CYC of S2DEX 2.08 (docs/ucode-s2dex2.md, "Backgrounds") in closed form
 *
 * The background command draws a scaled, clipped and wrapped image as strips of texture rectangles: each strip is a load of image rows into TMEM (SET_TEXTURE_IMAGE, SYNC_LOAD, LOAD_TILE,
 * with the SYNC_TILE / SET_TILE the tile wrap needs) followed by one TEXTURE_RECTANGLE of the frame. The microcode (overlay B, 0x6fc .. 0xdd8) computes the strips with 16-bit vector
 * lanes and a 48-bit accumulator; here the same numbers come out of integer expressions, each the closed form of one instruction chain (the readouts of the accumulator are hle_fx.h's).
 * Only a handful of lanes carry values (lanes 0 and 4 of the vectors are the x and y axes), so this is plain scalar C: no NEON form, nothing to differ from.
 *
 * The microcode keeps its intermediate results in DMEM, in the results area at 0x3d8 .. 0x40f and the scratch words at 0x1c0, and the verification compares the whole DMEM, so those are written
 * too (the same bytes as the microcode leaves, the stale bytes of the RDP commands that it does not write excluded). `A` ... `Z` below name the quantities, with the microcode's units:
 *   uObjBg      imageX imageW frameX frameW  imageY imageH frameY frameH   (halfwords at 0x3b0, a lane each; the x and y groups of four), image pointer at 0x3c0, scaleW / scaleH at 0x3cc / 0x3ce
 *   formats     sizes and frame positions u10.2 / s10.2, image positions u10.5, scales s5.10; the reciprocal of a scale is a 16.16 number
 *
 * Checked against the microcode (its transliteration by tools/rsp2c/rsp2c.py, docs/testing.md, "Regenerating the microcode oracles") on random DMEM images (tools/hle/test_s2dex2.c) and by the replays and verify runs
 * of the games (docs/testing.md).
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>
#include <string.h>

#include "hle_fx.h"
#include "s2dex2_fx.h"
#include "s2dex2_int.h"

static inline int32_t max0(int32_t v) { return v < 0 ? 0 : v; }

enum {
    OBJ = 0x3b0,                            /* the uObjBg: imageX imageW frameX frameW imageY imageH frameY frameH */
    IMAGE_PTR = 0x3c0,                      /* segmented address, replaced by the resolved one */
    TYPE_SIZ = 0x3c6, TYPE_FMT = 0x3c7,     /* texel size and format of the image (bytes) */
    FLIP = 0x3cb,                           /* bit 0: mirrored horizontally */
    SCALE = 0x3cc,                          /* scaleW, scaleH */
    YORIG = 0x3d0,                          /* imageYorig, s20.5 */
    /* the results area */
    R_FRAME_X0 = 0x3d8, R_FRAME_X1 = 0x3da, R_FRAME_Y0 = 0x3dc, R_FRAME_H = 0x3de,
    R_RENDER_BIT = 0x3e0, R_ROWFIT = 0x3e1, R_LINE = 0x3e2, R_ROWSPAN = 0x3e4, R_GSTEP = 0x3e8, R_MSTEP = 0x3ec, R_YORIG = 0x3f0, R_ROFF_X = 0x3f4, R_ROFF_Y = 0x3f6,
    R_CMD_IMAGE = 0x3f8, R_CMD_TILE = 0x3fc, R_Q = 0x400, R_QR = 0x402, R_STEP0 = 0x404, R_STEP1 = 0x408, R_IH4 = 0x40c, R_YSTART = 0x40e,
    SCRATCH = 0x1c0,                        /* the 32-bit scratch word the microcode moves vector lanes through */
    SCISSOR = 0x204,                        /* ulx lrx uly lry (halfwords: 0x204, 0x206, 0x208, 0x20a) */
    TILE_FMT = 0x294, TILE_SIZ = 0x2a4,     /* per format: (mask, scale) halfword pairs; per texel size: a halfword */
    RENDERMODE = 0x268,
    TILE_W1 = 0x264,
};

/* RDP commands written below (the command id is the low six bits of the first byte) */
enum {
    SET_TILE_W1 = 0x27000000,               /* tile 7 */
    SET_TILE_SIZE_W0 = 0x32000000,
    LOAD_TILE_W0 = 0xf4000000,
    CMD_SYNC_PIPE = 0x27, CMD_SYNC_LOAD = 0x26, CMD_SYNC_TILE = 0x28, CMD_TEXRECT = 0x24,
};

/* a 32-bit scratch pair (accumulator readout: integer half sclamp, fraction half uclamp) the way the microcode moves it through DMEM */
static inline uint32_t scratch_pair(uint32_t *dm, hle_acc acc)
{
    uint32_t v = (uint32_t)(uint16_t)hle_sclamp(acc) << 16 | hle_uclamp(acc);
    w32(dm, SCRATCH, v);
    return v;
}

int s2dex2_obj_bg1cyc(uint32_t *dm, uint32_t *s7p, uint32_t *s6p, const struct s2_ctx *c)
{
    unsigned L[8], k, a;                    /* the uObjBg halfwords: [4 * axis + {position, size, frame position, frame size}] */
    uint32_t scale[2];                      /* unsigned scales */
    int32_t recip[2];                       /* 1 / scale, 16.16 */
    int32_t ext[2];                         /* the frame size limited to the (scaled) image size */
    int32_t cut_lo[2], cut_hi[2], vis[2], start[2];
    int32_t x0, y0;                         /* image position of the first visible pixel, u10.5 */
    int flip = m8(dm, FLIP) & 1;
    uint16_t L8[8];                         /* sizes in u10.5 */
    unsigned ulx = m16(dm, SCISSOR), lrx = m16(dm, SCISSOR + 2), uly = m16(dm, SCISSOR + 4), lry = m16(dm, SCISSOR + 6);
    const int16_t clip_lo[2] = { (int16_t)ulx, (int16_t)uly }, clip_hi[2] = { (int16_t)lrx, (int16_t)lry };
    uint32_t s7 = *s7p, s6 = *s6p;

    if (memcmp(dm + 0x180 / 4, k_consts, sizeof k_consts))
        return S2_UNSUPPORTED;

    for (k = 0; k < 8; k++)
        L[k] = m16(dm, OBJ + 2 * k);

    /* ---- the frame against the image and the scissor, per axis ---- */
    {
        int32_t Lp[8], excess_x = 0;
        for (a = 0; a < 2; a++) {
            int16_t sc = (int16_t)m16(dm, SCALE + 2 * a);
            int32_t extent, excess;
            scale[a] = (uint16_t)sc;
            recip[a] = 1 + ((int32_t)hle_rcp32(sc) >> MUT(70, 5, 6));
            /* the scaled image size in frame units, less one, in whole pixels; the frame is cut to it (and, mirrored, moved by what it exceeds it) */
            extent = (int32_t)(((int64_t)recip[a] * L[4 * a + 1]) >> 16) - 1;
            extent &= MUT(71, ~3, ~1);
            excess = max0((int32_t)L[4 * a + 3] - extent);
            ext[a] = sat16((int16_t)L[4 * a + 3] - (int16_t)(uint16_t)excess);
            if (a == 0)
                excess_x = excess;
        }
        for (k = 0; k < 8; k++)
            Lp[k] = (int16_t)L[k];
        if (flip)
            Lp[2] = sat16(Lp[2] + MUT(74, (int16_t)(uint16_t)excess_x, -(int16_t)(uint16_t)excess_x));
        for (a = 0; a < 2; a++) {
            int32_t f = Lp[4 * a + 2];
            /* cut at the scissor's upper left, then at its lower right; what is left is the visible part of the frame */
            cut_lo[a] = max0(sat16(clip_lo[a] - f));
            cut_hi[a] = max0((int16_t)(uint16_t)(f - MUT(75, clip_hi[a], clip_lo[a]) + ext[a]));
            vis[a] = sat16(sat16(ext[a] - cut_lo[a]) - cut_hi[a]);
            start[a] = sat16(f + cut_lo[a]);
        }
        w16(dm, R_FRAME_X0, (uint16_t)start[0]);
        w16(dm, R_FRAME_X1, (uint16_t)sat16(start[0] + vis[0]));
        w16(dm, R_FRAME_Y0, (uint16_t)(start[1] >> 2));
        w16(dm, R_FRAME_H, (uint16_t)(vis[1] >> 2));
        if (flip)
            cut_lo[0] = cut_hi[0];
        for (k = 0; k < 8; k++)
            L8[k] = (uint16_t)(Lp[k] * 8);
        /* the image position of the visible part: image position + (cut off the frame) x scale, u10.2 x s5.10 -> u10.5 */
        x0 = sat16((int32_t)((((int64_t)scale[0] * cut_lo[0] * MUT(76, 512, 256)) >> 16) + (int16_t)L[0]));
        y0 = sat16((int32_t)((((int64_t)scale[1] * cut_lo[1] * 512) >> 16) + (int16_t)L[4]));
    }

    {
        int32_t iw8 = (int16_t)L8[1], ih8 = (int16_t)L8[5], yorig = (int32_t)m32(dm, YORIG);   /* image sizes as s16 of u10.5 */
        int32_t x = x0, y = y0, width_px;
        int32_t ih4 = L[5] >> 2, yw;
        unsigned render_bit, siz, fmt, tmask, tscale, tsize, iw8u = L8[1];
        uint16_t line;

        /* wrap the position into the image: whole image widths of x move y down by one pixel each, whole heights of y shift the origin (the loops of the microcode, as divisions; sizes <= 0 would
         * not terminate (or after 2^31 turns), the JIT can have that) */
        if (x - iw8 >= 0) {
            int32_t n;
            if (iw8 <= 0)
                return S2_UNSUPPORTED;
            n = x / iw8;
            x -= n * iw8;
            y += MUT(72, 32, 31) * n;
            yorig += 32 * n;
        }
        if (y - ih8 >= 0) {
            int32_t n;
            if (ih8 <= 0)
                return S2_UNSUPPORTED;
            n = y / ih8;
            y -= n * ih8;
            yorig -= n * ih8;
        }
        w32(dm, R_YORIG, (uint32_t)yorig);
        w32(dm, R_ROWSPAN, (uint32_t)(y - yorig) << 5);
        /* the visible width in the same units (u10.5), and whether a row of that width still fits in the image row */
        width_px = sat16((int32_t)(((int64_t)scale[0] * vis[0] * 512) >> 16));
        render_bit = (m8(dm, RENDERMODE) >> 3) & 1;
        w8(dm, R_RENDER_BIT, render_bit);
        w8(dm, R_ROWFIT, !((x + width_px + 11) < iw8));
        siz = m8(dm, TYPE_SIZ);
        fmt = m8(dm, TYPE_FMT);
        tsize = m16(dm, TILE_SIZ + 2 * siz);
        tmask = m16(dm, TILE_FMT + 4 * fmt);
        tscale = m16(dm, TILE_FMT + 4 * fmt + 2);

        /* the line size in TMEM words: (frame width x scale + the render-mode bit's 32, limited to the image width, + mask) x format scale, + 1 */
        {
            int32_t z = MUT(77, 32, 16) * (int32_t)render_bit + (int32_t)(((int64_t)scale[0] * (int16_t)L[3]) >> 7);
            unsigned lo = z < (int32_t)iw8u ? (unsigned)z & 0xffff : iw8u;
            unsigned t = (lo + (unsigned)(int16_t)tmask) & 0xffff;
            line = (uint16_t)sat16(MUT(78, 1, 0) + (int32_t)(((int64_t)(int16_t)t * tscale) >> 16));
            w16(dm, R_LINE, line);
        }

        /* the strip height and its remainder: q = (rows per frame-height step) / (rows per TMEM load) by the reciprocal of the load's size (with one correction), then the fractional part */
        {
            int32_t rh = (int32_t)hle_rcp32((int16_t)line);
            int16_t qr = sat16((int32_t)((((int64_t)rh * tsize >> 16) - ((int64_t)render_bit << 16)) >> 16));   /* rows of TMEM lines that fit, less the render-mode bit */
            int32_t Fv = (int32_t)((uint32_t)(uint16_t)qr * 1024), Ay = recip[1];
            int16_t G_hi = hle_sclamp(hle_mac_32x32(Fv, Ay));
            uint16_t G_lo = hle_uclamp(hle_mac_32x32_before_h(Fv, Ay));
            int32_t D = (int32_t)((uint32_t)(y - yorig) << 5);
            int16_t J_hi = hle_sclamp(hle_mac_32x32(D, Ay));
            uint16_t J_lo = hle_uclamp(hle_mac_32x32_before_h(D, Ay)) & 0xfc00;
            int32_t RG = (int32_t)hle_rcp32((int32_t)((uint32_t)(uint16_t)G_hi << 16 | G_lo));
            int32_t J32 = hle_fx(J_hi, J_lo);
            int16_t K_hi = hle_sclamp(hle_mac_32x32(J32, RG));
            uint16_t K_lo = hle_uclamp(hle_mac_32x32_before_h(J32, RG));
            int32_t Nn = 2 * K_hi + (K_lo >> 15), qa = sat16(Nn), qb = sat16(Nn + 1), q;
            int16_t GV_hi = hle_sclamp((int64_t)G_lo * qb + (int64_t)G_hi * qb * 65536);
            uint16_t GV_lo = (uint16_t)hle_uclamp((hle_acc)G_lo * qb);
            int ge = J_hi > GV_hi || (J_hi == GV_hi && MUT(79, J_lo >= GV_lo, J_lo > GV_lo));
            int64_t accq, acc;
            uint16_t gql, rem_lo;
            int32_t rem_hi, rem, S2hi, S2lo, R5lo, R5hi, t0v, yw1, yw2, v8a, x0s, roff_x, roff_y;
            unsigned tfrac;
            int borrow;

            w16(dm, R_GSTEP, (uint16_t)G_hi);
            w16(dm, R_GSTEP + 2, G_lo);
            q = ge ? qb : qa;
            accq = (int64_t)G_lo * q + (int64_t)G_hi * q * 65536;
            gql = (uint16_t)((G_lo * q) & MUT(80, 0xfc00, 0xf800));
            borrow = J_lo < gql;
            rem_lo = (uint16_t)(J_lo - gql);
            rem_hi = sat16(J_hi - hle_sclamp(accq) - borrow);
            rem = (int32_t)((uint32_t)(uint16_t)rem_hi << 16 | rem_lo);
            {
                int64_t S2 = (int64_t)(rem >> MUT(81, 10, 9)) * (int16_t)scale[1];
                S2lo = (uint16_t)S2;
                S2hi = sat16((int32_t)(S2 >> 16));
            }
            acc = (int64_t)G_hi * 65536 + ((G_lo * q) & MUT(73, 0x3ff, 0x3fe)) + G_lo - rem_lo;
            w16(dm, R_MSTEP + 2, hle_uclamp(acc));
            acc -= (int64_t)rem_hi * 65536;
            w16(dm, R_MSTEP, (uint16_t)hle_sclamp(acc));
            tfrac = ((int16_t)S2lo >> 5) & MUT(82, 0x1f, 0x0f);                /* the fractional texture row of the first rectangle */
            {
                int32_t R5 = (int32_t)((((int64_t)S2hi << 16) + S2lo) >> 10);
                R5hi = (int16_t)(R5 >> 16);
                R5lo = (uint16_t)R5;
            }
            w16(dm, R_Q, (uint16_t)sat16(qr - (int16_t)R5lo));
            w16(dm, R_QR, (uint16_t)qr);
            acc = (int64_t)(uint16_t)qr * q + (((int64_t)R5hi << 16) + R5lo) + (yorig >> 5);
            t0v = (int16_t)hle_uclamp(acc);
            yw1 = sat16(t0v - ih4);
            yw2 = sat16(t0v + ih4);
            v8a = t0v >= 0 ? t0v : MUT(83, yw2, t0v);
            yw = yw1 >= 0 ? yw1 : v8a;                          /* the first image row, wrapped into the image height */
            w16(dm, R_YSTART, (uint16_t)yw);
            w16(dm, R_IH4, (uint16_t)ih4);
            x0s = (int16_t)(uint16_t)x;
            roff_x = (((x0s * (int32_t)tscale) >> 16) * MUT(84, 8, 4)) & 0xffff;
            roff_y = ((((int32_t)(int16_t)iw8u * (int32_t)tscale) >> 16) * 8) & 0xffff;
            w16(dm, R_ROFF_X, roff_x);
            w16(dm, R_ROFF_Y, roff_y);
            {
                uint32_t img = c->seg(c->ctx, m32(dm, IMAGE_PTR));
                int64_t acc2 = (int64_t)(uint16_t)roff_x + (int64_t)(uint16_t)yw * (int16_t)roff_y;
                int32_t P0 = (int16_t)m16(dm, R_Q) * (int32_t)roff_y, P1 = (int16_t)qr * (int32_t)roff_y;
                unsigned s0, a3, t3, sco;
                uint32_t img_pos;

                w32(dm, IMAGE_PTR, img);
                img_pos = scratch_pair(dm, acc2) + img;
                w32(dm, R_STEP0, (uint32_t)P0);
                w32(dm, R_STEP1, (uint32_t)P1);
                s0 = (uint16_t)x & tmask;
                sco = flip ? (unsigned)(-(int32_t)s0 - MUT(85, width_px, -width_px)) & 0xffff : s0;     /* the texture S of the rectangles */
                w32(dm, R_CMD_IMAGE, MUT(86, 0xfd100000u, 0xfd180000u) | (uint32_t)((roff_y >> 1) - 1));
                a3 = 0xf5100000u | ((unsigned)line << 9);
                w32(dm, R_CMD_TILE, a3);
                w32(dm, s7, a3);
                w32(dm, s7 + 4, SET_TILE_W1);
                w32(dm, s7 + 8, a3);
                w8(dm, s7 + 9, (siz << 5) | (fmt << 3));
                t3 = m8(dm, 0x3c9);
                w32(dm, s7 + 12, (t3 << 20) | m32(dm, TILE_W1));
                w32(dm, s7 + 16, SET_TILE_SIZE_W0);
                w32(dm, s7 + 20, 0);
                s7 += 24;

                /* ---- the strips ---- */
                {
                    uint32_t rows_left = (uint32_t)(ih4 - (int32_t)(uint16_t)yw), rows = (uint32_t)(int32_t)(int16_t)m16(dm, R_Q), src = img_pos, src_step = m32(dm, R_STEP0);
                    uint32_t pos = m32(dm, R_MSTEP), pos_step = m32(dm, R_GSTEP), frame_rows = m16(dm, R_FRAME_H), x1 = m16(dm, R_FRAME_X0) << 12, x2 = m16(dm, R_FRAME_X1) << 12, y1 = m16(dm, R_FRAME_Y0);
                    uint32_t y2, n, t8;
                    uint32_t T3F8 = m32(dm, R_CMD_IMAGE), T3FC = m32(dm, R_CMD_TILE), base = m32(dm, IMAGE_PTR), tcoord = tfrac;
                    unsigned fl = m8(dm, R_ROWFIT), roff_xs = roff_x, roff_ys = roff_y, ihs = (unsigned)ih4, iter = 0;
                    uint32_t sw_ = scale[0], sh_ = scale[1], Hu = line;

#define EMIT_SYNC_TILE(s4v) do { w8(dm, s7, CMD_SYNC_TILE); s7 += 16; w32(dm, s7 - 8, T3FC | (s4v)); w32(dm, s7 - 4, SET_TILE_W1); } while (0)
#define EMIT_LOAD(s1v, s2v, s3v) do { w32(dm, s7 + 4, (s1v)); s7 += 24; w32(dm, s7 - 24, T3F8); w8(dm, s7 - 16, CMD_SYNC_LOAD); w32(dm, s7 - 8, LOAD_TILE_W0); \
        w16(dm, s7 - 4, (s3v) + MUT(88, 1791, 1790)); w16(dm, s7 - 2, ((s2v) << 2) - MUT(89, 1, 2)); } while (0)

                    n = pos >> 10;
                    for (;;) {
                        while (!((int32_t)n > 0)) {                 /* image rows pass before the first strip, and between strips */
                            if (++iter > 200000)
                                return S2_UNSUPPORTED;
                            rows_left -= rows;
                            src += src_step;
                            if (!((int32_t)rows_left > 0)) {
                                src = base + scratch_pair(dm, (int64_t)roff_ys * (int16_t)(uint16_t)(-rows_left)) + roff_xs;
                                rows_left += ihs;
                            }
                            pos += pos_step;
                            rows = (uint32_t)(int32_t)(int16_t)m16(dm, R_QR);
                            src_step = m32(dm, R_STEP1);
                            tcoord = 0;
                            n = pos >> 10;
                        }
                        if (++iter > 200000)
                            return S2_UNSUPPORTED;
                        frame_rows -= MUT(87, n, n - 1);
                        pos &= 1023;
                        if ((int32_t)frame_rows < 0) {              /* the last strip is cut short */
                            uint32_t pr = scratch_pair(dm, (int64_t)sh_ * (int16_t)(uint16_t)frame_rows), lim = m16(dm, R_QR);
                            rows += MUT(90, 1, 0) + (uint32_t)((int32_t)pr >> 10);
                            n += frame_rows;
                            if (!((int32_t)(rows - lim) < 0))
                                rows = lim;
                        }
                        y2 = y1 + n;
                        {
                            uint32_t t9 = rows + render_bit, s1, s2, s3 = Hu, s4;
                            t8 = rows_left - fl;
                            if (!((int32_t)(t8 - t9) < 0)) {        /* the strip lies inside the rows that are left */
                                EMIT_LOAD(src, t9, s3);
                                rows_left -= rows;
                                src += src_step;
                            } else {                                /* it runs over the image's last row: two loads, or a load from the top */
                                s2 = t9 - rows_left;
                                if ((int32_t)s2 > 0) {
                                    s4 = rows_left;
                                    s1 = base + roff_xs;
                                    if (s4 & 1) {
                                        s4--;
                                        s2 += MUT(91, 1, 0);
                                        s1 -= roff_ys;
                                    }
                                    s4 = (uint32_t)(int32_t)(int16_t)(uint16_t)((uint64_t)(uint16_t)s4 * MUT(92, (int16_t)Hu, 1));
                                    EMIT_SYNC_TILE(s4);
                                    EMIT_LOAD(s1, s2, s3);
                                }
                                if (fl) {
                                    uint32_t pr = scratch_pair(dm, (int64_t)(uint16_t)t8 * (int16_t)roff_ys), t4v, v26, v27_1, v25_0, v25_1;
                                    int odd = (int)(t8 & 1);
                                    s1 = base;
                                    t4v = src + pr;
                                    v26 = (uint16_t)t8;
                                    if (odd) {
                                        t4v -= roff_ys;
                                        s1 -= roff_ys;
                                        v26 = (uint16_t)sat16((int16_t)v26 - 1);
                                    }
                                    s2 = (unsigned)odd + 1;
                                    v27_1 = (uint16_t)(sat16((int16_t)roff_ys - (int16_t)roff_xs) >> 3);
                                    v25_0 = (uint16_t)sat16((int16_t)Hu - (int16_t)v27_1);
                                    v25_1 = (uint16_t)((uint64_t)v26 * (int16_t)Hu);
                                    s3 = (uint32_t)(int32_t)(int16_t)v25_0;
                                    s4 = (uint32_t)(int32_t)(int16_t)(uint16_t)hle_uclamp((int64_t)v26 * (int16_t)Hu + v27_1);
                                    EMIT_SYNC_TILE(s4);
                                    EMIT_LOAD(s1, s2, s3);
                                    s1 = t4v;
                                    s4 = (uint32_t)(int32_t)(int16_t)v25_1;
                                    s3 = (uint32_t)(int32_t)(int16_t)v27_1;
                                    EMIT_SYNC_TILE(s4);
                                    EMIT_LOAD(s1, s2, s3);
                                }
                                rows_left -= rows;
                                if ((int32_t)t8 > 0) {
                                    EMIT_SYNC_TILE(0);
                                    EMIT_LOAD(src, t8, Hu);
                                } else {
                                    s7 += 8;
                                    w32(dm, s7 - 8, T3FC);
                                    w32(dm, s7 - 4, SET_TILE_W1);
                                }
                                src += src_step;
                                if (!((int32_t)rows_left > 0)) {
                                    src = base + roff_xs + scratch_pair(dm, (int64_t)(uint16_t)(-rows_left) * (int16_t)roff_ys);
                                    rows_left += ihs;
                                }
                            }
                        }
                        /* the texture rectangle of the strip */
                        w8(dm, s7, CMD_SYNC_PIPE);
                        w32(dm, s7 + 8, (y2 << 2) | x2);
                        w8(dm, s7 + 8, CMD_TEXRECT);
                        w32(dm, s7 + 12, (y1 << MUT(93, 2, 3)) | x1);
                        w16(dm, s7 + 16, sco);
                        w16(dm, s7 + 18, tcoord);
                        w16(dm, s7 + 20, sw_);
                        w16(dm, s7 + 22, sh_);
                        s7 += 24;
                        if (c->flush(c->ctx, &s7, &s6))
                            return S2_UNSUPPORTED;
                        y1 = y2;
                        if ((int32_t)frame_rows <= 0)
                            break;
                        pos += pos_step;
                        rows = (uint32_t)(int32_t)(int16_t)m16(dm, R_QR);
                        src_step = m32(dm, R_STEP1);
                        tcoord = 0;
                        n = pos >> 10;
                    }
                }
            }
        }
    }
    *s7p = s7;
    *s6p = s6;
    return S2_DONE;
}
