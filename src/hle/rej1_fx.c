/*
 * arMLEt - rej1_fx.c: the scalar reference of the 1.x Rej vertex code (rej1_fx.h)
 *
 * Bit-exact to the microcode's vector code (tools/hle/test_rej1.c checks it against translations of the microcode); docs/ucode-rej.md section 11 has the microcode's side. The arithmetic is hle_fx.h's.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>
#include <string.h>

#include "f3dex_fx_int.h"
#include "hle_fx.h"
#include "rej1_fx.h"

#ifdef ARMLET_HLE_MUTATE
#define MUT(n, ok, bad) (ARMLET_HLE_MUTATE == (n) ? (bad) : (ok))
#else
#define MUT(n, ok, bad) (ok)
#endif

static inline int16_t max16(int16_t a, int16_t b) { return a > b ? a : b; }

/* h = 0 / 1: the first / second vertex of a pair */
static void vtx_one(const struct f3dex_vctx *c, const struct rej1_vparm *p, const struct f3dex_vin *in, unsigned h, struct rej_vtx *o)
{
    const int16_t px[3] = { in->x, in->y, in->z };
    int16_t Pi[4], T;
    uint16_t Pf[4];
    unsigned k, out = 0;
    int32_t W, Wk;
    struct hle_pair R, N2[3];
    uint8_t col[4];
    int16_t st[2] = { in->s, in->t };

    /* clip coordinates as F3DEX 1.x: the fraction is read before the integer part of row 3 is added */
    for (k = 0; k < 4; k++) {
        hle_acc acc = 0;
        for (unsigned j = 0; j < 3; j++)
            acc += (hle_acc)hle_fx(c->mp.i[j][k], c->mp.f[j][k]) * px[j];
        acc = hle_wrap48(acc + c->mp.f[3][k]);
        Pf[k] = hle_uclamp(acc);
        Pi[k] = hle_sclamp(hle_wrap48(acc + ((hle_acc)c->mp.i[3][k] << 16)));
    }
    /* the guard band: x and y against w with only its integer half times the clip ratio (VMUDH; the fraction is w's own), z against w and only on the far side */
    W = hle_fx(Pi[3], Pf[3]);
    Wk = hle_fx(hle_clamp16((int32_t)Pi[3] * MUT(600, c->clip_k, 1)), Pf[3]);
    for (k = 0; k < 2; k++) {
        int32_t X = hle_fx(Pi[k], Pf[k]);
        out |= (unsigned)hle_clip_le(X, Wk) | (unsigned)hle_clip_ge(X, Wk);
    }
    out |= (unsigned)hle_clip_ge(hle_fx(Pi[2], Pf[2]), W) | (unsigned)MUT(601, 0, hle_clip_le(hle_fx(Pi[2], Pf[2]), W));
    T = out ? p->tab[0] : p->tab[1];
    /* 1/w of w times the perspective normalisation (no clamp of a negative result), the position times it and the normalisation again, the viewport */
    R = f3dex_recip_refine(hle_pair32(hle_pair_acc(hle_mac_32xu16(W, c->persp))));
    for (k = 0; k < 3; k++) {
        int32_t P = hle_fx(Pi[k], Pf[k]), Rv = hle_fx(R.i, R.f);
        hle_acc a = hle_mac_32x32(P, Rv);
        int32_t N = hle_fx(hle_sclamp(a), hle_uclamp(hle_mac_32x32_before_h(P, Rv)));
        hle_acc b = hle_mac_32xu16(N, c->persp);
        N2[k].i = hle_sclamp(b);
        N2[k].f = hle_uclamp(b);
    }
    for (k = 0; k < 3; k++) {
        int16_t sc = hle_clamp16((int32_t)c->vscale[k] * c->vsign[k]);
        hle_acc v = hle_wrap48(((hle_acc)c->vtrans[k] << 16) + (hle_acc)hle_pair32(N2[k]) * sc);
        int16_t s = hle_sclamp(v);
        if (k == 0)
            o->sx = max16(s, T);
        else if (k == 1)
            o->sy = (int16_t)(hle_clamp16((int32_t)s + MUT(602, 2, 1)) & MUT(603, p->ymask, 0xfff8));        /* (rounded before the mark: the y of a marked vertex is its own) */
        else {
            o->szi = MUT(604, max16(s, T), s);
            o->szf = hle_uclamp(v);
        }
    }
    o->rwi = R.i;
    o->rwf = R.f;
    memcpy(col, in->c, 4);
    if (c->geom & F3DEX_G_LIGHTING)
        f3dex_lit(c, in, h, col, st);
    if (c->geom & F3DEX_G_FOG)
        col[3] = f3dex_fog_byte(c, N2[2].i, N2[2].f);
    memcpy(o->col, col, 4);
    o->s = hle_sclamp((hle_acc)st[0] * c->tscale[0]);
    o->t = hle_sclamp((hle_acc)st[1] * c->tscale[1]);
}

int rej1_vtx_xform_scalar(const struct f3dex_vctx *c, const struct rej1_vparm *p, const struct f3dex_vin *in, unsigned n, struct rej_vtx *out)
{
    unsigned i;
    if (n == 0 || n > REJ1_NSLOTS)
        return 1;
    if ((c->geom & F3DEX_G_LIGHTING) && (c->nrec > F3DEX_MAX_REC || ((c->geom & F3DEX_G_TEXGEN) && c->nrec == 0)))
        return 1;
    for (i = 0; i < n; i++)
        vtx_one(c, p, &in[i], MUT(605, i & 1, 0), &out[i]);
    return 0;
}

int rej1_vtx_xform(const struct f3dex_vctx *c, const struct rej1_vparm *p, const struct f3dex_vin *in, unsigned n, struct rej_vtx *out)
{
    return rej1_vtx_xform_scalar(c, p, in, n, out);
}
