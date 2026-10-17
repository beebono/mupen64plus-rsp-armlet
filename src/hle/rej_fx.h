/*
 * arMLEt - rej_fx.h: the geometry of the "Rej" microcodes (F3DLX.Rej, F3DFLX.Rej, F3DEX.Rej) as pure C on plain structs
 *
 * The Rej builds are F3DEX2-class microcode that rejects instead of clipping (docs/ucode-rej.md): a vertex outside the guard band is not clipped but marked, and every triangle
 * that uses a marked vertex is dropped; the vertex record has 20 bytes and no clip coordinates, the triangle code is the F3DEX2 setup without the clipper. The vertex pipeline up to
 * the clip coordinates and the lighting are the F3DEX2 ones (f3dex2_fx.h); this header has what is different: the projection and the record, the triangle decisions.
 *
 * Every function has a scalar form (rej_*_scalar, the reference, plain C11); on aarch64 the vertex and the triangle forms have a NEON form with identical results where one exists
 * (tools/hle/test_rej.c). tools/hle/test_rej.c also checks the closed forms against the microcode (rsp2c oracles, docs/testing.md, "Rej").
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ARMLET_REJ_FX_H
#define ARMLET_REJ_FX_H

#include <stdint.h>

#include "f3dex2_fx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* a vertex record as G_VTX leaves it in its slot (DMEM +0 .. +0x13) */
struct rej_vtx {
    uint8_t col[4];                                 /* r g b a (+0): lit, with the fog value in a when fog is on */
    int16_t s, t;                                   /* scaled texture coordinates (+4) */
    int16_t sx;                                     /* screen x (+8) */
    int16_t sy;                                     /* screen y with the two low bits cleared (+10): 0x7ffc is the "rejected" mark */
    int16_t szi;                                    /* screen z, integer part as computed (+12) */
    uint16_t szf;                                   /* its fraction (+14) */
    int16_t rwi;                                    /* 1/W integer part (+16) */
    uint16_t rwf;                                   /* fraction (+18) */
};
/* flags of the variants of the Rej texts (f2_layout.rej_flags). REJ_FLAT_A: a flat-shaded triangle takes the alpha of its first vertex too (the others take the alpha of each vertex) */
enum { REJ_YQ = 1, REJ_FLAT_A = 2 };                                /* the screen y is rounded to whole pixels: the viewport's y translation + 2, the two low bits cleared; the mark of a rejected vertex is then 0x7ffc, else 0x7fff (F3DEX.Rej 2.06 does neither) */
enum { REJ_VSTRIDE = 20, REJ_NSLOTS = 64, REJ_MARK_Y = 0x7ffc };                /* (64 vertex slots of 20 bytes: DMEM 0x410 .. 0x910) */

/* n vertices (no pair partner is needed: the records carry no clip bits). out[i] is the record of in[i]. */
void rej_vtx_xform_scalar(const struct fx2_vctx *c, unsigned flags, const struct fx2_vin *in, unsigned n, struct rej_vtx *out);
void rej_vtx_xform(const struct fx2_vctx *c, unsigned flags, const struct fx2_vin *in, unsigned n, struct rej_vtx *out);

/* ---- triangles ----------------------------------------------------------------------------------------------------------------------------------------------------------------- */
struct rej_tri_in {
    const struct rej_vtx *v[3];                     /* the vertices in command order */
    uint32_t g;                                     /* the geometry mode word */
    unsigned tex, tile;                             /* the G_TEXTURE bytes at 0x1e7 / 0x1e6 */
    unsigned pass_neg, pass_pos;                    /* the cull table (DMEM 0x402 + the cull byte of the geometry mode) lets a triangle with a negative / positive cross product through */
    unsigned flags;                                 /* REJ_* */
    uint16_t rcp_k, dx_k;                           /* the constants of the triangle code that come from the vector constants (0x1b0 lanes 2 and 5: 8, 0x4000) */
};
/* The decisions and the RDP triangle command. FX2_TRI_DRAW: *len bytes (a multiple of 8, at most 0xb0), in cmd as the host words of DMEM hold them; FX2_TRI_SKIP: nothing is emitted (a vertex was rejected, the
 * triangle is culled or has no area). */
int rej_tri_setup_scalar(const struct rej_tri_in *in, uint32_t *cmd, unsigned *len);
int rej_tri_setup(const struct rej_tri_in *in, uint32_t *cmd, unsigned *len);
int rej_tri_setup_core(const struct rej_tri_in *in, uint32_t *cmd, unsigned *len, int neon);     /* (the NEON attribute forms where there are any) */

#ifdef __cplusplus
}
#endif

#endif
