/*
 * arMLEt - f3d_dmem.h: filling the f3d_fx.h structs from a DMEM (or RDRAM) image and writing results back
 *
 * Both images are arrays of 32-bit host words that hold the big-endian words (byte a is at ((uint8_t *)img)[a ^ 3]), the layout hle.h's accessors use. The offsets are those of the
 * microcode's builds (docs/ucode-f3d.md 5 and 10.3); the build-dependent ones are in struct f3d_layout.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ARMLET_F3D_DMEM_H
#define ARMLET_F3D_DMEM_H

#include <string.h>

#include "f3d_fx.h"

struct f3d_layout {
    uint16_t mp, mv, viewport, fog, vtx_base, vtx_stride;
    uint8_t flags_variant;              /* F3D_FLAGS_* */
    uint8_t nslots;                     /* the vertex slots (the buffer ends where the command buffer starts) */
};
/* SM64, Pilotwings, Wave Race, Mischief Makers, Tetrisphere */
static const struct f3d_layout f3d_layout_a = { 0x3e0, 0x360, 0x320, 0x330, 0x420, 40, F3D_FLAGS_SM64, 16 };
/* Wave Race: the same with 32 slots (its command buffer is 0x280 higher) */
static const struct f3d_layout f3d_layout_wr = { 0x3e0, 0x360, 0x320, 0x330, 0x420, 40, F3D_FLAGS_SM64, 32 };
/* Pilotwings: the layout of family A with its own clip flag word */
static const struct f3d_layout f3d_layout_pw = { 0x3e0, 0x360, 0x320, 0x330, 0x420, 40, F3D_FLAGS_PW, 16 };
/* the 2.0H re-layout (Glover, SpaceStation, Wipeout) */
static const struct f3d_layout f3d_layout_b = { 0x460, 0x3e0, 0x380, 0x390, 0x4a0, 40, F3D_FLAGS_SM64, 32 };

static inline uint8_t fd8(const uint32_t *m, unsigned a) { return ((const uint8_t *)m)[(a & 0xfff) ^ 3]; }
static inline uint16_t fd16(const uint32_t *m, unsigned a) { return ((const uint16_t *)m)[((a & 0xfff) ^ 2) >> 1]; }
static inline uint32_t fd32(const uint32_t *m, unsigned a) { return m[(a & 0xfff) >> 2]; }
static inline void fs8(uint32_t *m, unsigned a, uint8_t v) { ((uint8_t *)m)[(a & 0xfff) ^ 3] = v; }
static inline void fs16(uint32_t *m, unsigned a, uint16_t v) { ((uint16_t *)m)[((a & 0xfff) ^ 2) >> 1] = v; }
static inline void fs32(uint32_t *m, unsigned a, uint32_t v) { m[(a & 0xfff) >> 2] = v; }
/* the same for RDRAM (23-bit addresses) */
static inline uint16_t fr16(const uint32_t *m, unsigned a) { return ((const uint16_t *)m)[((a & 0x7fffff) ^ 2) >> 1]; }
static inline uint8_t fr8(const uint32_t *m, unsigned a) { return ((const uint8_t *)m)[(a & 0x7fffff) ^ 3]; }

/* a matrix: 16 integer halfwords (row-major, 8 bytes per row) then 16 fractions at +0x20 */
static inline void f3d_load_mtx(struct f3d_mtx *m, const uint32_t *img, unsigned a, int rdram)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            m->i[r][c] = (int16_t)(rdram ? fr16(img, a + 8 * r + 2 * c) : fd16(img, a + 8 * r + 2 * c));
            m->f[r][c] = rdram ? fr16(img, a + 0x20 + 8 * r + 2 * c) : fd16(img, a + 0x20 + 8 * r + 2 * c);
        }
}
static inline void f3d_store_mtx(const struct f3d_mtx *m, uint32_t *dmem, unsigned a)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            fs16(dmem, a + 8 * r + 2 * c, (uint16_t)m->i[r][c]);
            fs16(dmem, a + 0x20 + 8 * r + 2 * c, m->f[r][c]);
        }
}

/* an input vertex at byte address a of an image */
static inline void f3d_load_vin(struct f3d_vin *v, const uint32_t *img, unsigned a, int rdram)
{
    uint16_t (*r16)(const uint32_t *, unsigned) = rdram ? fr16 : fd16;
    uint8_t (*r8)(const uint32_t *, unsigned) = rdram ? fr8 : fd8;
    v->x = (int16_t)r16(img, a);
    v->y = (int16_t)r16(img, a + 2);
    v->z = (int16_t)r16(img, a + 4);
    v->s = (int16_t)r16(img, a + 8);
    v->t = (int16_t)r16(img, a + 10);
    for (int i = 0; i < 4; i++)
        v->c[i] = r8(img, a + 12 + i);
}

/* a vertex record (DMEM +0 .. +37) */
static inline void f3d_load_vtx(struct f3d_vtx *v, const uint32_t *dmem, unsigned a)
{
    for (int k = 0; k < 4; k++) {
        v->cx[k] = (int16_t)fd16(dmem, a + 2 * k);
        v->cf[k] = fd16(dmem, a + 8 + 2 * k);
        v->col[k] = fd8(dmem, a + 16 + k);
    }
    v->s = (int16_t)fd16(dmem, a + 20);
    v->t = (int16_t)fd16(dmem, a + 22);
    v->sx = (int16_t)fd16(dmem, a + 24);
    v->sy = (int16_t)fd16(dmem, a + 26);
    v->sz = (int16_t)fd16(dmem, a + 28);
    v->szf = fd16(dmem, a + 30);
    v->rwi = (int16_t)fd16(dmem, a + 32);
    v->rwf = fd16(dmem, a + 34);
    v->flags = fd16(dmem, a + 36);
}
static inline void f3d_store_vtx(const struct f3d_vtx *v, uint32_t *dmem, unsigned a)
{
    for (int k = 0; k < 4; k++) {
        fs16(dmem, a + 2 * k, (uint16_t)v->cx[k]);
        fs16(dmem, a + 8 + 2 * k, v->cf[k]);
        fs8(dmem, a + 16 + k, v->col[k]);
    }
    fs16(dmem, a + 20, (uint16_t)v->s);
    fs16(dmem, a + 22, (uint16_t)v->t);
    fs16(dmem, a + 24, (uint16_t)v->sx);
    fs16(dmem, a + 26, (uint16_t)v->sy);
    fs16(dmem, a + 28, (uint16_t)v->sz);
    fs16(dmem, a + 30, v->szf);
    fs16(dmem, a + 32, (uint16_t)v->rwi);
    fs16(dmem, a + 34, v->rwf);
    fs16(dmem, a + 36, v->flags);
}

/* The G_VTX state from DMEM. Returns nonzero when the lights are in a state the functions do not model (a pending relight (D32[0x12c] < 0), a count that is not a multiple of 32, fewer than one
 * or more than F3D_MAX_SLOTS slots) and lighting is on; without lighting the light words are not read. */
static inline int f3d_load_vctx(struct f3d_vctx *c, const uint32_t *dmem, const struct f3d_layout *lay)
{
    const unsigned SP = 0x110;
    f3d_load_mtx(&c->mp, dmem, lay->mp, 0);
    for (int k = 0; k < 4; k++) {
        c->vscale[k] = (int16_t)fd16(dmem, lay->viewport + 2 * k);
        c->vtrans[k] = (int16_t)fd16(dmem, lay->viewport + 8 + 2 * k);
        c->vsign[k] = (int16_t)fd16(dmem, 0x50 + 2 * k);
        c->lim[k] = (int16_t)fd16(dmem, 0x28 + 2 * k);
    }
    c->persp = fd16(dmem, SP + 2);
    c->clip_k = (int16_t)fd16(dmem, 0x76);
    c->tscale[0] = fd16(dmem, SP + 20);
    c->tscale[1] = fd16(dmem, SP + 22);
    c->geom = fd32(dmem, SP + 4);
    for (int k = 0; k < 3; k++)
        c->fog[k] = (int16_t)fd16(dmem, lay->fog + 2 * k);
    for (int p = 0; p < 6; p++) {
        for (int k = 0; k < 4; k++)
            c->plane[p][k] = (int16_t)fd16(dmem, 0x98 - 8 * p + 2 * k);
        c->plane_mask[p] = fd16(dmem, 0xb2 - 2 * p);
    }
    c->flags_variant = lay->flags_variant;
    c->nslot = 0;
    if (c->geom & F3D_G_LIGHTING) {
        uint32_t l = fd32(dmem, 0x12c);
        if ((int32_t)l < 0 || (l & 31) || (l >> 5) < 1 || (l >> 5) > F3D_MAX_SLOTS)
            return 1;
        c->nslot = l >> 5;
        for (unsigned k = 0; k <= c->nslot + 1; k++)
            for (int i = 0; i < 8; i++)
                c->lcol[k][i] = fd8(dmem, 0x1b0 + 32 * k + i);
        for (unsigned k = 0; k <= c->nslot; k++)
            for (int i = 0; i < 8; i++)
                c->ldir[k][i] = (int8_t)fd8(dmem, 0x1c0 + 32 * k + i);
        for (int i = 0; i < 8; i++)
            c->lk[i] = (int8_t)fd8(dmem, 0x1a0 + i);
        for (int i = 0; i < 3; i++)
            c->tg[i] = (int16_t)fd16(dmem, 0xa2 + 2 * i);
    }
    return 0;
}

#endif
