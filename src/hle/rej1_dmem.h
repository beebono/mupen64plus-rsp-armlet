/*
 * arMLEt - rej1_dmem.h: the 1.x Rej G_VTX and triangles on a DMEM image (f3dex.c and tools/hle/test_rej1.c)
 *
 * The records are 20 bytes (rej1_fx.h) from `vbuf`; a vertex number in a command is twice the slot (the microcode turns every one into an address arithmetically: vbuf + 10 n). The DMEM layout is the
 * F3DEX 1.x one of the build (struct f3dex_layout) with the Rej words of struct f3dex_rej1 (f3dex_bld.h). docs/ucode-rej.md section 11.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ARMLET_REJ1_DMEM_H
#define ARMLET_REJ1_DMEM_H

#include <string.h>

#include "f3dex_bld.h"
#include "f3dex_dmem.h"
#include "rej1_fx.h"

#ifdef ARMLET_HLE_MUTATE
#undef MUT
#define MUT(n, ok, bad) (ARMLET_HLE_MUTATE == (n) ? (bad) : (ok))
#else
#ifndef MUT
#define MUT(n, ok, bad) (ok)
#endif
#endif

/* F3DLX.Rej 1.23 (Bust-A-Move 2, Mortal Kombat 4, WCW vs. nWo, ...): the F3DLX 1.23 handlers with the matrices, the viewport, the return stack, the slots and the buffers moved */
static const struct f3dex_layout rej1_lay_123 __attribute__((unused)) = { .jt0 = 0xc0, .jt2 = 0x76, .half1 = 0x10c, .persp = 0x112, .geom = 0x114, .omh = 0x118, .oml = 0x11c, .texture = 0x120,
    .rdp_wp = 0x128, .lights = 0x12c, .mtx_sp = 0x134, .mtx_lim = 0x15c, .depth = 0x148, .fifo = 0x150, .segs = 0x160, .light = 0x1a0, .mm_tab = 0x270, .mw_tab = 0x30e, .vtab = 0x31e,
    .viewport = 0x320, .fog = 0x330, .retstack = 0x336, .mv = 0x380, .proj = 0x3c0, .mvp = 0x400, .vbuf = 0x440, .window = 0x940, .operand = 0xa80, .outbuf = 0xc80,
    .lim = 0x28, .tgbias = 0x38, .vsign = 0x50, .clip_k = 0x76, .planes = 0x98, .tg = 0xa2, .plane_masks = 0xb2 };
static const struct f3dex_rej1 rej1_123 __attribute__((unused)) = { .vbuf = 0x440, .tab = 0xb0, .ymask = 0x4a, .bufsel = 0x15a, .buftog = 0x160 };
/* F3DLX.Rej 1.21 (Air Boarder 64, ISS 64, Mortal Kombat Mythologies, Fighters Destiny, SimCity 2000, ...) and the earlier 1.23 (WCW vs. nWo, Virtual Pro Wrestling 64): the same layout, the triangle code without the
 * load of the top vertex's texture fractions */
static const struct f3dex_rej1 rej1_121 __attribute__((unused)) = { .vbuf = 0x440, .tab = 0xb0, .ymask = 0x4a, .bufsel = 0x15a, .buftog = 0x160, .texf0 = 1, .flat1 = 1 };
static const struct f3dex_rej1 rej1_123a __attribute__((unused)) = { .vbuf = 0x440, .tab = 0xb0, .ymask = 0x4a, .bufsel = 0x15a, .buftog = 0x160, .texf0 = 1 };

static inline void rej1_ld_vtx(const uint32_t *dm, unsigned a, struct rej_vtx *v)
{
    v->sx = (int16_t)xd16(dm, a);
    v->sy = (int16_t)xd16(dm, a + 2);
    v->szi = (int16_t)xd16(dm, a + 4);
    v->szf = xd16(dm, a + 6);
    for (int k = 0; k < 4; k++)
        v->col[k] = xd8(dm, a + 8 + k);
    v->s = (int16_t)xd16(dm, a + 12);
    v->t = (int16_t)xd16(dm, a + 14);
    v->rwi = (int16_t)xd16(dm, a + 16);
    v->rwf = xd16(dm, a + 18);
}
static inline void rej1_st_vtx(uint32_t *dm, unsigned a, const struct rej_vtx *v)
{
    xs16(dm, a, (uint16_t)v->sx);
    xs16(dm, a + 2, (uint16_t)v->sy);
    xs16(dm, a + 4, (uint16_t)v->szi);
    xs16(dm, a + 6, v->szf);
    for (int k = 0; k < 4; k++)
        xs8(dm, a + 8 + k, v->col[k]);
    xs16(dm, a + 12, (uint16_t)v->s);
    xs16(dm, a + 14, (uint16_t)v->t);
    xs16(dm, a + 16, (uint16_t)v->rwi);
    xs16(dm, a + 18, v->rwf);
}

/* G_VTX (w0: v0 * 2 in bits 16..23, the count in bits 10..15; 0 is one vertex) of the input vertices at byte address a of img (RDRAM when rdram). The lights are transformed first when the word
 * at L->lights says they are stale (the lighting overlay's relight). Returns NULL, or why the command declines (the state is left as it was). *marked: some vertex was marked. */
static inline const char *rej1_vtx_dmem(uint32_t *dm, const struct f3dex_build *fx, const struct f3dex_rej1 *r, uint32_t w0, const uint32_t *img, unsigned a, int rdram, int *marked)
{
    const struct f3dex_layout *L = f3dex_lay(fx);
    unsigned v0x2 = (w0 >> 16) & 0xff, n = (w0 >> 10) & 0x3f, cnt = n ? n : 1, i;
    uint32_t geom = xd32(dm, L->geom);
    struct f3dex_vctx c;
    struct rej1_vparm p;
    struct f3dex_vin in[REJ1_NSLOTS];
    struct rej_vtx out[REJ1_NSLOTS];
    if (MUT(609, (v0x2 & 1) || (v0x2 >> 1) + cnt > REJ1_NSLOTS, 0))
        return "G_VTX slot range unusual";
    if (geom & ~F3DEX_G_DEFINED)
        return "geometry mode bits the build does not define";
    if (geom & F3DEX_G_LIGHTING) {
        uint32_t l = xd32(dm, L->lights);
        if ((int32_t)l < 0) {                                   /* stale: the relight */
            unsigned nrec = (l & 0xfff) >> 5, k;
            int8_t raw[F3DEX_MAX_REC + 1][3], res[F3DEX_MAX_REC + 1][4];
            struct f3dex_mtx mv;
            if ((l & 31) || nrec > F3DEX_MAX_REC)
                return "G_VTX: light count unusual";
            for (k = 0; k <= nrec; k++)
                for (i = 0; i < 3; i++)
                    raw[k][i] = (int8_t)xd8(dm, L->light + 0x18 + 32 * k + i);
            f3dex_load_mtx(&mv, dm, L->mv, 0);
            f3dex_relight(&mv, raw, nrec, res);
            for (k = 0; k <= nrec; k++)
                for (i = 0; i < 8; i++)
                    xs8(dm, L->light + 0x20 + 32 * k + i, (uint8_t)res[k][i & 3]);
            xs32(dm, L->lights, l & 0xfff);
        }
    }
    if (f3dex_load_vctx(&c, dm, fx))
        return "G_VTX: light state unusual";
    p.tab[0] = (int16_t)xd16(dm, r->tab);
    p.tab[1] = (int16_t)xd16(dm, r->tab + 2);
    p.ymask = xd16(dm, r->ymask);
    for (i = 0; i < cnt; i++)
        f3dex_load_vin(&in[i], img, a + 16 * i, rdram);
    if (rej1_vtx_xform(&c, &p, in, cnt, out))
        return "G_VTX: state outside what the native code supports";
    *marked = 0;
    for (i = 0; i < cnt; i++) {
        rej1_st_vtx(dm, r->vbuf + REJ1_VSTRIDE * ((v0x2 >> 1) + i), &out[i]);
        *marked |= out[i].sx == REJ1_MARK_X;
    }
    return NULL;
}

/* One triangle of a G_TRI1 / G_TRI2 (the vertex bytes as the command has them: twice the slots; bflat: the one whose colour a flat-shaded triangle takes, its first, or with flat1 the first of the first
 * triangle of the command); frac: the colour fractions of the middle and low vertex (zeros for the first triangle of a command), updated
 * when the triangle is drawn (rej_tri_in.x1_frac). Returns FX2_TRI_DRAW (cmd, *len: the RDP command, host words of big-endian words), FX2_TRI_SKIP (marked, culled,
 * no area), or -1: declined (*why). The decisions: a vertex whose x is 0x7fff takes the triangle with it; G_CULL_BACK (0x2000) draws only a negative cross product, else any nonzero one (the code has no
 * test of G_CULL_FRONT). The command's id is the low byte of the geometry mode | 0xcc (always shaded), its tile byte DMEM 0x122. */
static inline int rej1_tri_dmem(const uint32_t *dm, const struct f3dex_build *fx, const struct f3dex_rej1 *r, unsigned b0, unsigned b1, unsigned b2, unsigned bflat, uint16_t frac[8], uint32_t *cmd,
                                unsigned *len, const char **why)
{
    const struct f3dex_layout *L = f3dex_lay(fx);
    uint32_t g = xd32(dm, L->geom);
    unsigned bs[3] = { b0, b1, b2 }, k;
    struct rej_vtx v[3], vf;
    struct rej_tri_in ti;
    for (k = 0; k < 3; k++) {
        if (MUT(610, (bs[k] & 1) || bs[k] >= 2 * REJ1_NSLOTS, 0)) {
            *why = "G_TRI: vertex number outside the slots";
            return -1;
        }
        rej1_ld_vtx(dm, r->vbuf + 10u * bs[k], &v[k]);
        ti.v[k] = &v[k];
    }
    if (g & ~F3DEX_G_DEFINED) {
        *why = "geometry mode bits the build does not define";
        return -1;
    }
    rej1_ld_vtx(dm, r->vbuf + 10u * bflat, &vf);         /* (a number the triangles of the command have: in the slots) */
    ti.x1_flat = &vf;
    ti.g = g | 4;
    ti.tex = g & 2;
    ti.tile = xd8(dm, L->texture + 2);
    ti.pass_neg = 1;
    ti.pass_pos = !(g & MUT(611, F3DEX_G_CULL_BACK, F3DEX_G_CULL_FRONT));
    ti.flags = REJ_YQ | REJ_X1 | (r->texf0 ? REJ_X1T0 : 0);
    ti.rcp_k = 8;
    ti.dx_k = 0x4000;
    memcpy(ti.x1_frac, frac, sizeof ti.x1_frac);
    ti.x1_next = frac;
    return rej_tri_setup(&ti, cmd, len);
}

#endif
