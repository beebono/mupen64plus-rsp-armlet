/*
 * arMLEt - rej1_fx.h: the vertex code of the 1.x Rej microcodes (F3DLX.Rej 1.21 / 1.23) as pure C on plain structs
 *
 * The 1.x Rej texts are F3DLX 1.2x with a vertex stage that does not clip (docs/ucode-rej.md section 11): the F3DEX 1.x transform and lighting (f3dex_fx.h), then the 2.x Rej idea written anew: a
 * vertex outside the guard band is marked by taking the maximum of its screen x and z with the first halfword of a table (0x7fff), and a triangle with a vertex whose x is 0x7fff is dropped. The
 * record has 20 bytes and no clip coordinates; the triangles are rej_fx.h's with REJ_X1.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ARMLET_REJ1_FX_H
#define ARMLET_REJ1_FX_H

#include <stdint.h>

#include "f3dex_fx.h"
#include "rej_fx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* the record of the 1.x texts: sx +0, sy +2, sz +4 (integer) +6 (fraction), r g b a +8, s t +12, 1/w +16 / +18 */
enum { REJ1_VSTRIDE = 20, REJ1_NSLOTS = 64, REJ1_MARK_X = 0x7fff };

/* what the vertex code reads besides the F3DEX 1.x state: the table the screen x and z are raised to (tab[0] for a vertex outside the guard band, tab[1] for one inside: 0x7fff, 0x8000) and the mask of the
 * rounded screen y (v30 lane 5: 0xfffc) */
struct rej1_vparm {
    int16_t tab[2];
    uint16_t ymask;
};

/* n vertices of one G_VTX (1 .. 64); out[i] is the record of in[i] (the pair parity selects the light bytes, as in F3DEX 1.x). Returns nonzero when the state is outside what the function models (as
 * f3dex_vtx_xform_scalar: lighting with too many records, texture generation with no light record). */
int rej1_vtx_xform_scalar(const struct f3dex_vctx *c, const struct rej1_vparm *p, const struct f3dex_vin *in, unsigned n, struct rej_vtx *out);
int rej1_vtx_xform(const struct f3dex_vctx *c, const struct rej1_vparm *p, const struct f3dex_vin *in, unsigned n, struct rej_vtx *out);

#ifdef __cplusplus
}
#endif

#endif
