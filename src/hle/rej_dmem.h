/*
 * arMLEt - rej_dmem.h: the Rej builds' DMEM layout, and the vertex command on a task's DMEM (rej_fx.h's structs from and to DMEM)
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ARMLET_REJ_DMEM_H
#define ARMLET_REJ_DMEM_H

#include "f3dex2_dmem.h"
#include "rej_fx.h"

#ifdef ARMLET_HLE_MUTATE
#undef MUT
#define MUT(n, ok, bad) (ARMLET_HLE_MUTATE == (n) ? (bad) : (ok))
#else
#ifndef MUT
#define MUT(n, ok, bad) (ok)
#endif
#endif

/* The DMEM layout of the Rej builds with the XBUS transport (Excitebike 64): the F3DEX2 state where F3DEX2 keeps it (the matrices, the viewport, the geometry mode, the lights), the display list input buffer
 * ends at 0x9f8 (where the RDP list ring begins), the slot address table has the stride 20, the cull table is at 0x402 (docs/ucode-rej.md, section 1). */
static const struct f2_layout l_lay_rej_xbus __attribute__((unused)) = {
    .mv = 0x00, .p = 0x40, .mvp = 0x80, .persp_norm = 0xdc, .viewport_scale = 0xe0, .viewport_trans = 0xe8, .clip_ratio = 0x186, .const31 = 0x1b0, .const30 = 0x1c0,
    .fog_mul = 0x1e0, .fog_ofs = 0x1e2, .texture = 0x1e4, .tex_scale_s = 0x1e8, .tex_scale_t = 0x1ea, .geom = 0x1ec, .mvp_valid = 0x1d9, .lights_valid = 0x1dc, .lights_size = 0x1dd,
    .light_raw = 0x1f8, .light_base = 0x200, .tg = 0x1d0, .light_scale = 0x1c6, .light_scale_zex = 0x1ce, .vtab = 0x380, .clipmask = 0, .culltab = 0x402, .in = 0x950, .in_end = 0x9f8,
    .jump_table = 0x36e, .mw_max = 14, .geom_def = F2_GEOM_DEF, .movemem_tab = 0x2f0, .moveword_adj = 10238, .desc_light = 0x2e0, .desc_clip = 0x2e8, .xbus = 1, .ring_start = 0x9f8 };

/* A vertex record is 20 bytes, the slot address table is at 0x380 with the stride 20 (slot k at 0x410 + 20 k). The rest of the state is where the F3DEX2 builds keep it (docs/ucode-rej.md, section 1). */
static inline void rej_st_vtx(struct hle_task *t, unsigned s, const struct rej_vtx *v)
{
    uint32_t *w = &t->dmem[(s & 0xfff) >> 2];
    if (!(s & 3)) {
        w[0] = (uint32_t)v->col[0] << 24 | (uint32_t)v->col[1] << 16 | (uint32_t)v->col[2] << 8 | v->col[3];
        w[1] = (uint32_t)(uint16_t)v->s << 16 | (uint16_t)v->t;
        w[2] = (uint32_t)(uint16_t)v->sx << 16 | (uint16_t)v->sy;
        w[3] = (uint32_t)(uint16_t)v->szi << 16 | v->szf;
        w[4] = (uint32_t)(uint16_t)v->rwi << 16 | v->rwf;
        return;
    }
    {   /* (the table only has 4-byte aligned slots; a DMEM image with another table is the caller's problem, but the bytes go where the microcode puts them) */
        unsigned i;
        uint8_t b[20] = { v->col[0], v->col[1], v->col[2], v->col[3], (uint8_t)((uint16_t)v->s >> 8), (uint8_t)v->s, (uint8_t)((uint16_t)v->t >> 8), (uint8_t)v->t,
                          (uint8_t)((uint16_t)v->sx >> 8), (uint8_t)v->sx, (uint8_t)((uint16_t)v->sy >> 8), (uint8_t)v->sy, (uint8_t)((uint16_t)v->szi >> 8), (uint8_t)v->szi,
                          (uint8_t)(v->szf >> 8), (uint8_t)v->szf, (uint8_t)((uint16_t)v->rwi >> 8), (uint8_t)v->rwi, (uint8_t)(v->rwf >> 8), (uint8_t)v->rwf };
        for (i = 0; i < 20; i++)
            hle_st8(t, s + i, b[i]);
    }
}
static inline void rej_ld_vtx(const struct hle_task *t, unsigned s, struct rej_vtx *v)
{
    const uint32_t *w = &t->dmem[(s & 0xfff) >> 2];
    uint32_t c = w[0];
    v->col[0] = (uint8_t)(c >> 24);
    v->col[1] = (uint8_t)(c >> 16);
    v->col[2] = (uint8_t)(c >> 8);
    v->col[3] = (uint8_t)c;
    v->s = (int16_t)(w[1] >> 16);
    v->t = (int16_t)w[1];
    v->sx = (int16_t)(w[2] >> 16);
    v->sy = (int16_t)w[2];
    v->szi = (int16_t)(w[3] >> 16);
    v->szf = (uint16_t)w[3];
    v->rwi = (int16_t)(w[4] >> 16);
    v->rwf = (uint16_t)w[4];
}

/* G_VTX, the part before the vertices: the lights set up when they changed (lit vertices), the MVP matrix when it is stale. Returns NULL, or the reason the task declines. */
static inline const char *rej_vtx_pre(struct hle_task *t, uint32_t w0)
{
    unsigned n = (w0 >> 12) & 0xff, lit = hle_dm16(t, F2_GEOM) & 2;
    if (n == 0 || n > REJ_NSLOTS)
        return "G_VTX with an unusual count";
    if (MUT(345, hle_dm16(t, F2_CLIP_RATIO) & 0x8000, 0))      /* the guard-band ratio multiplies as a signed lane in the microcode; the ratios of the GBI are 1 .. 6 */
        return "guard-band ratio out of range";
    if (lit) {
        unsigned lb = hle_dm8(t, F2_LIGHTS_SIZE);
        if (!(lb >= F2_LIGHT_STRIDE && lb % F2_LIGHT_STRIDE == 0 && lb <= 7 * F2_LIGHT_STRIDE))
            return "unusual light count";
        if (!hle_dm8(t, F2_LIGHTS_VALID)) {             /* the lights have changed since the last lit vertex command: the setup normalises the directions (as F3DEX2's) */
            struct fx2_mtx mv;
            int8_t raw[FX2_LSLOTS][3], o[FX2_LSLOTS][3];
            unsigned nl = lb / F2_LIGHT_STRIDE, m, j;
            f2_ld_mtx(t, F2_MV, &mv);
            for (m = 0; m <= nl + 1; m++)
                for (j = 0; j < 3; j++)
                    raw[m][j] = (int8_t)hle_dm8(t, F2_LIGHT_RAW + F2_LIGHT_STRIDE * m + j);
            fx2_light_setup(&mv, raw, nl, (int16_t)hle_dm16(t, f2L->light_scale), o);
            hle_st8(t, F2_LIGHTS_VALID, (uint8_t)w0);
            for (m = 0; m <= nl + 1; m++)
                for (j = 0; j < 3; j++) {
                    hle_st8(t, F2_LIGHT_BASE + F2_LIGHT_STRIDE * m + j, (uint8_t)o[m][j]);
                    hle_st8(t, F2_LIGHT_BASE + F2_LIGHT_STRIDE * m + 4 + j, (uint8_t)o[m][j]);
                }
        }
    }
    if (!hle_dm8(t, F2_MVP_VALID)) {
        struct fx2_mtx mv, p, r;
        f2_ld_mtx(t, F2_MV, &mv);
        f2_ld_mtx(t, F2_P, &p);
        fx2_mtx_mul(&mv, &p, &r);
        f2_st_mtx(t, F2_MVP, &r);
        hle_st8(t, F2_MVP_VALID, (uint8_t)w0);
    }
    return NULL;
}

/* where the microcode's DMA puts the n input vertices of a G_VTX: the end of the slot range, rounded down to 8 */
static inline unsigned rej_vtx_dst(const struct hle_task *t, uint32_t w0)
{
    return (hle_dm16(t, F2_VTAB + (w0 & 0xfff)) & 0xff8u) - ((w0 >> 12) & 0xff) * 16;
}

/* G_VTX, the vertices: in[] holds the n input vertices (from RDRAM or from DMEM where the DMA left them); the records go to the slots from the first one of the command, 20 bytes apart */
static inline void rej_vtx_run(struct hle_task *t, uint32_t w0, const struct fx2_vin *in)
{
    unsigned n = (w0 >> 12) & 0xff, first = hle_dm16(t, F2_VTAB + ((w0 - n * 2) & 0xfff)), i;
    struct fx2_vctx c;
    struct rej_vtx out[REJ_NSLOTS + 2];
    f2_ld_vctx(t, &c);
    if (c.mode & FX2_LIGHTING)
        f2_ld_lights(t, hle_dm8(t, F2_LIGHTS_SIZE) / F2_LIGHT_STRIDE, &c.L);
    rej_vtx_xform(&c, in, n, out);
    for (i = 0; i < n; i++)
        rej_st_vtx(t, first + REJ_VSTRIDE * i, &out[i]);
}


/* ---- the RDP list ring (XBUS) --------------------------------------------------------------------------------------------------------------------------------------------- */
enum { REJ_RING_LIMIT = 0xf10 };

/* The flush routine (0x258 / 0x25c), run after every command that wrote to the ring: DPC_END = the output pointer; when the pointer passed 0xf10 the ring wraps to its start (after waiting for START_VALID
 * / END_VALID to clear and for the RDP to leave the start); then the routine waits while the RDP stands within 176 bytes ahead of the pointer. With the video plugin synchronous (it has taken every
 * list when DPC_END is written) the waits never happen, and a state in which they would (it would hang the JIT too) declines. */
static inline const char *rej_flush(struct hle_task *t, uint32_t *s7)
{
    int32_t d;
    int wrap = (int32_t)(*s7 - REJ_RING_LIMIT) > 0;
    hle_mtc0(t, 9, *s7);
    if (wrap) {
        if (*t->cr[11] & 0x600)
            return "DPC busy while wrapping the ring";
        *s7 = f2L->ring_start;
        if (*t->cr[10] == *s7)
            return "RDP at the start of the ring while wrapping";
        hle_mtc0(t, 8, *s7);
        hle_mtc0(t, 9, *s7);
    }
    d = (int32_t)(*t->cr[10] - *s7);
    if (d > 0 && d - 176 <= 0)
        return "the RDP stands just ahead of the ring pointer";
    return NULL;
}

/* ---- triangles ---------------------------------------------------------------------------------------------------------------------------------------------------------------- */
/* A vertex number in a triangle command is the slot x 2 (the third one is turned into a slot address arithmetically, the other two through the table): even and within the 64 slots */
static inline int rej_vidx_bad(uint32_t b) { return (b & 0x81) != 0; }

/* G_TRI1 (two == 0: the second word is zero: its triangle is the degenerate (0, 0, 0)), G_TRI2 and G_QUAD (the first word's triangle first, then the second's): the RDP commands are appended at *s7 and
 * flushed one by one. Returns NULL or the reason the task declines. */
static inline const char *rej_cmd_tri(struct hle_task *t, uint32_t *s7, uint32_t w0, uint32_t w1, int two)
{
    uint32_t g = hle_dm32(t, F2_GEOM);
    unsigned ca = F2_CULLTAB + hle_dm8(t, F2_GEOM + 2), T = hle_dm16(t, ca), k, nt = two ? 2 : 1;
    struct rej_tri_in ti;
    uint32_t word[2] = { w0, w1 };
    if (MUT(224, g & ~f2L->geom_def, 0))
        return "geometry mode bits the build does not define";
    if (MUT(233, (hle_dm8(t, F2_TEXTURE + 3) & 0xfd) || (hle_dm8(t, F2_TEXTURE + 2) & 0xc0), 0))
        return "G_TEXTURE with an unusual on / tile byte";
    memset(&ti, 0, sizeof ti);
    ti.g = g;
    ti.tex = hle_dm8(t, F2_TEXTURE + 3);
    ti.tile = hle_dm8(t, F2_TEXTURE + 2);
    ti.rcp_k = hle_dm16(t, F2_CONST31 + 4);
    ti.dx_k = hle_dm16(t, F2_CONST31 + 10);
    for (k = 0; k < nt; k++) {
        uint32_t w = word[k];
        unsigned sv[3], j;
        struct rej_vtx rv[3];
        uint32_t cmd[0xb0 / 4];
        unsigned len = 0;
        if (MUT(217, rej_vidx_bad((w >> 16) & 0xff) | rej_vidx_bad((w >> 8) & 0xff) | rej_vidx_bad(w & 0xff), 0))
            return "vertex number out of range";
        sv[0] = hle_dm16(t, F2_VTAB + ((w >> 16) & 0xff));
        sv[1] = hle_dm16(t, F2_VTAB + ((w >> 8) & 0xff));
        sv[2] = hle_dm16(t, F2_VTAB + (w & 0xff));
        for (j = 0; j < 3; j++) {
            rej_ld_vtx(t, sv[j], &rv[j]);
            ti.v[j] = &rv[j];
        }
        ti.pass_neg = (T >> (1 + 4 * k)) & 1;
        ti.pass_pos = (T >> (9 + 4 * k)) & 1;
        if (rej_tri_setup(&ti, cmd, &len) == FX2_TRI_DRAW) {
            const char *why;
            memcpy(&t->dmem[*s7 >> 2], cmd, len);
            *s7 += len;
            why = rej_flush(t, s7);
            if (why)
                return why;
        }
    }
    return NULL;
}

#endif
