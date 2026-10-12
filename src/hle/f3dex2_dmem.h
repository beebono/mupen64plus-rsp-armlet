/*
 * arMLEt - f3dex2_dmem.h: the F3DEX2 structs (f3dex2_fx.h) from and to DMEM
 *
 * DMEM is held as host words that contain the big-endian words: the halfword at an even byte address a is the high half of the word at a & ~3 when a & 2 is clear and the low half
 * otherwise. The adapters below know the microcode's layout (docs/ucode-f3dex2.md); the functions of f3dex2_fx.h know nothing of it.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ARMLET_F3DEX2_DMEM_H
#define ARMLET_F3DEX2_DMEM_H

#include <string.h>

#include "f3dex2_fx.h"
#include "hle.h"
#include "hle_fx.h"

/* the layout: the same in the four builds */
enum {
    F2_MV = 0x00, F2_P = 0x40, F2_MVP = 0x80,                  /* the matrices: 16 integer halfwords, then 16 fractions */
    F2_PERSP_NORM = 0xdc, F2_VIEWPORT_SCALE = 0xe0, F2_VIEWPORT_TRANS = 0xe8,
    F2_CLIP_RATIO = 0x186, F2_CONST31 = 0x1b0, F2_CONST30 = 0x1c0,
    F2_FOG_MUL = 0x1e0, F2_FOG_OFS = 0x1e2, F2_TEXTURE = 0x1e4, F2_TEX_SCALE_S = 0x1e8, F2_TEX_SCALE_T = 0x1ea, F2_GEOM = 0x1ec,
    F2_MVP_VALID = 0x1d9, F2_LIGHTS_VALID = 0x1dc, F2_LIGHTS_SIZE = 0x1dd,
    F2_LIGHT_RAW = 0x1f8, F2_LIGHT_BASE = 0x200, F2_LIGHT_STRIDE = 24,
    F2_VTAB = 0x380, F2_VSLOT0 = 0x420, F2_VSTRIDE = 0x28,
    F2_CLIPMASK = 0x3cc, F2_CULLTAB = 0x3c2,
    F2_IN = 0x920, F2_IN_END = 0x9c8, F2_IN_REFILL = 168
};

static inline uint16_t f2_bswap16(uint16_t v) { return (uint16_t)(v >> 8 | v << 8); }

/* a matrix: 8 words of integer parts, 8 of fractions */
static inline void f2_ld_mtx(const struct hle_task *t, unsigned a, struct fx2_mtx *m)
{
    unsigned k;
    for (k = 0; k < 8; k++) {
        uint32_t wi = t->dmem[((a & 0xfff) >> 2) + k], wf = t->dmem[(((a + 0x20) & 0xfff) >> 2) + k];
        ((int16_t *)m->i)[2 * k] = (int16_t)(wi >> 16);
        ((int16_t *)m->i)[2 * k + 1] = (int16_t)wi;
        ((uint16_t *)m->f)[2 * k] = (uint16_t)(wf >> 16);
        ((uint16_t *)m->f)[2 * k + 1] = (uint16_t)wf;
    }
}
static inline void f2_st_mtx(struct hle_task *t, unsigned a, const struct fx2_mtx *m)
{
    unsigned k;
    for (k = 0; k < 8; k++) {
        t->dmem[((a & 0xfff) >> 2) + k] = (uint32_t)(uint16_t)((const int16_t *)m->i)[2 * k] << 16 | (uint16_t)((const int16_t *)m->i)[2 * k + 1];
        t->dmem[(((a + 0x20) & 0xfff) >> 2) + k] = (uint32_t)((const uint16_t *)m->f)[2 * k] << 16 | ((const uint16_t *)m->f)[2 * k + 1];
    }
}

/* a vertex record (+0 .. +0x27): ten words */
static inline void f2_ld_vtx_lite(const struct hle_task *t, unsigned s, struct fx2_vtx *v)
{
    const uint32_t *w = &t->dmem[(s & 0xfff) >> 2];
    uint32_t c = w[4];
    v->col[0] = (uint8_t)(c >> 24);
    v->col[1] = (uint8_t)(c >> 16);
    v->col[2] = (uint8_t)(c >> 8);
    v->col[3] = (uint8_t)c;
    v->s = (int16_t)(w[5] >> 16);
    v->t = (int16_t)w[5];
    v->sx = (int16_t)(w[6] >> 16);
    v->sy = (int16_t)w[6];
    v->szi = (int16_t)(w[7] >> 16);
    v->szf = (uint16_t)w[7];
    v->rwi = (int16_t)(w[8] >> 16);
    v->rwf = (uint16_t)w[8];
    v->flags = w[9];
}
static inline void f2_ld_vtx(const struct hle_task *t, unsigned s, struct fx2_vtx *v)
{
    const uint32_t *w = &t->dmem[(s & 0xfff) >> 2];
    v->cx[0] = (int16_t)(w[0] >> 16);
    v->cx[1] = (int16_t)w[0];
    v->cx[2] = (int16_t)(w[1] >> 16);
    v->cx[3] = (int16_t)w[1];
    v->cf[0] = (uint16_t)(w[2] >> 16);
    v->cf[1] = (uint16_t)w[2];
    v->cf[2] = (uint16_t)(w[3] >> 16);
    v->cf[3] = (uint16_t)w[3];
    f2_ld_vtx_lite(t, s, v);
}
static inline void f2_st_vtx(struct hle_task *t, unsigned s, const struct fx2_vtx *v)
{
    uint32_t *w = &t->dmem[(s & 0xfff) >> 2];
    w[0] = (uint32_t)(uint16_t)v->cx[0] << 16 | (uint16_t)v->cx[1];
    w[1] = (uint32_t)(uint16_t)v->cx[2] << 16 | (uint16_t)v->cx[3];
    w[2] = (uint32_t)v->cf[0] << 16 | v->cf[1];
    w[3] = (uint32_t)v->cf[2] << 16 | v->cf[3];
    w[4] = (uint32_t)v->col[0] << 24 | (uint32_t)v->col[1] << 16 | (uint32_t)v->col[2] << 8 | v->col[3];
    w[5] = (uint32_t)(uint16_t)v->s << 16 | (uint16_t)v->t;
    w[6] = (uint32_t)(uint16_t)v->sx << 16 | (uint16_t)v->sy;
    w[7] = (uint32_t)(uint16_t)v->szi << 16 | v->szf;
    w[8] = (uint32_t)(uint16_t)v->rwi << 16 | v->rwf;
    w[9] = v->flags;
}

/* an input vertex as the DMA left it */
static inline void f2_ld_vin(const struct hle_task *t, unsigned a, struct fx2_vin *v)
{
    v->x = (int16_t)hle_dm16(t, a);
    v->y = (int16_t)hle_dm16(t, a + 2);
    v->z = (int16_t)hle_dm16(t, a + 4);
    v->s = (int16_t)hle_dm16(t, a + 8);
    v->t = (int16_t)hle_dm16(t, a + 10);
    {
        uint32_t c = hle_dm32(t, a + 12);
        v->c[0] = (uint8_t)(c >> 24);
        v->c[1] = (uint8_t)(c >> 16);
        v->c[2] = (uint8_t)(c >> 8);
        v->c[3] = (uint8_t)c;
    }
}

/* an input vertex from RDRAM (big-endian words) */
static inline void f2_ld_vin_rdram(const struct hle_task *t, uint32_t a, struct fx2_vin *v)
{
    uint32_t w0 = hle_rd32(t, a), w1 = hle_rd32(t, a + 4), w2 = hle_rd32(t, a + 8), w3 = hle_rd32(t, a + 12);
    v->x = (int16_t)(w0 >> 16);
    v->y = (int16_t)w0;
    v->z = (int16_t)(w1 >> 16);
    v->s = (int16_t)(w2 >> 16);
    v->t = (int16_t)w2;
    v->c[0] = (uint8_t)(w3 >> 24);
    v->c[1] = (uint8_t)(w3 >> 16);
    v->c[2] = (uint8_t)(w3 >> 8);
    v->c[3] = (uint8_t)w3;
}

/* the lights as the lit-vertex code reads them: the slots at 0x200 + 24 m, m = 0 .. nlights + 1 */
static inline void f2_ld_lights(const struct hle_task *t, unsigned nlights, struct fx2_lights *L)
{
    unsigned g, m, k;
    L->top = nlights + 1;
    for (g = 0; g < 2; g++)
        for (m = 0; m <= L->top; m++) {
            unsigned S = F2_LIGHT_BASE + F2_LIGHT_STRIDE * m;
            for (k = 0; k < 3; k++) {
                L->d[g][m][k] = (int16_t)((int8_t)hle_dm8(t, S + (g ? 4 : 0) + k) * 256);
                L->c[g][m][k] = (int16_t)(hle_dm8(t, S + (g ? 12 : 8) + k) << 7);
            }
            L->d[g][m][3] = 0;
            L->c[g][m][3] = 0;
        }
}

/* the vertex state */
static inline void f2_ld_vctx(const struct hle_task *t, struct fx2_vctx *c)
{
    unsigned k;
    f2_ld_mtx(t, F2_MVP, &c->mp);
    for (k = 0; k < 4; k++) {
        c->scale[k] = (int16_t)hle_dm16(t, F2_VIEWPORT_SCALE + 2 * k);
        c->trans[k] = (int16_t)hle_dm16(t, F2_VIEWPORT_TRANS + 2 * k);
    }
    c->scale[1] = hle_clamp16(-(int32_t)c->scale[1]);
    c->scale[3] = (int16_t)hle_dm16(t, F2_FOG_MUL);      /* the fog multiplier and offset (G_MV_FOG) take the fourth lane of the viewport scale and translation */
    c->trans[3] = (int16_t)hle_dm16(t, F2_FOG_OFS);
    c->pn = hle_dm16(t, F2_PERSP_NORM);
    c->ratio = hle_dm16(t, F2_CLIP_RATIO);
    c->sc_s = hle_dm16(t, F2_TEX_SCALE_S);
    c->sc_t = hle_dm16(t, F2_TEX_SCALE_T);
    c->c31_3 = hle_dm16(t, F2_CONST31 + 6);
    c->c31_7 = (int16_t)hle_dm16(t, F2_CONST31 + 14);
    for (k = 0; k < 3; k++)
        c->tg[k] = (int16_t)hle_dm16(t, 0x1d0 + 2 * k);
    c->mode = hle_dm16(t, F2_GEOM) & 15u;
}

#endif
