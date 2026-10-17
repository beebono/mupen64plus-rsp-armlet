/*
 * arMLEt - hle_rej.c: the Rej microcodes (F3DLX.Rej, F3DFLX.Rej, F3DEX.Rej; docs/ucode-rej.md) as builds of the F3DEX2 front-end (hle_f3dex2.c)
 *
 * The Rej texts have the F3DEX2 skeleton (the command loop, the state commands, the matrix and G_MOVE* handlers sit at the pcs of F3DEX2 2.08 and use the F3DEX2 DMEM state) and their own vertex,
 * triangle, G_MODIFYVTX, G_CULLDL and G_BRANCH_Z code, which the build's `ext` table hands to the closed forms of rej_fx.c (the commands below are the ext kinds). The Excitebike 64 text writes its RDP
 * lists to a ring in DMEM (DPC_STATUS.XBUS): `f2L->xbus` makes the front-end flush that way (f3_flush_xbus).
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hle_f3dex2_int.h"
#include "rej_dmem.h"

#ifdef ARMLET_NATIVE


enum { K_REJ_VTX = K_EXT, K_REJ_TRI1, K_REJ_TRI2, K_REJ_MODVTX, K_REJ_CULLDL, K_REJ_BRANCHZ };

int f3_flush_xbus(struct f3 *f)
{
    const char *why = rej_flush(f->t, &f->s7);
    return why ? hle_decline(f->t, why) : HLE_OK;
}

static int rej_vtx(struct f3 *f)
{
    struct hle_task *t = f->t;
    uint32_t w0 = f->w0, w1 = f->w1, va = seg_addr(t, w1);
    unsigned n = (w0 >> 12) & 0xff, i;
    const char *why;
    struct fx2_vin in[REJ_NSLOTS];

    if (MUT(224, hle_dm32(t, F2_GEOM) & ~f2L->geom_def, 0))
        return hle_decline(t, "geometry mode bits the build does not define");
    if (MUT(221, ((w0 & 0xfff) > 2 * REJ_NSLOTS || (w0 & 1) || (w0 & 0xfff) < 2 * n || (w0 & 0xf00000)), 0))
        return hle_decline(t, "G_VTX with an unusual end index");
    why = rej_vtx_pre(t, w0);
    if (why)
        return hle_decline(t, why);
    if (f->lean) {
        if (va & 7)
            return hle_decline(t, "unaligned vertices");
        for (i = 0; i < n; i++)
            f2_ld_vin_rdram(t, va + 16 * i, &in[i]);
    } else {
        unsigned dst = rej_vtx_dst(t, w0);
        hle_dma_read(t, dst, va, n * 16 - 1);
        for (i = 0; i < n; i++)
            f2_ld_vin(t, dst + 16 * i, &in[i]);
    }
    rej_vtx_run(t, w0, in);
    return HLE_OK;
}

static int rej_ext_cmd(struct f3 *f, unsigned kind)
{
    struct hle_task *t = f->t;
    uint32_t w0 = f->w0, w1 = f->w1;
    const char *why;
    unsigned at, k;

    switch (kind) {
    case K_REJ_VTX:
        return rej_vtx(f);
    case K_REJ_TRI1:
    case K_REJ_TRI2:
        if (kind == K_REJ_TRI1 && !f->lean)
            hle_st32(t, F2_IN_END + f->k1 - 4, 0);     /* (G_TRI1 zeroes the second word of its command in the input buffer) */
        why = rej_cmd_tri(t, &f->s7, w0, kind == K_REJ_TRI1 ? 0 : w1, kind == K_REJ_TRI2);
        return why ? hle_decline(t, why) : HLE_OK;
    case K_REJ_MODVTX: {                                /* the word w1 goes to the record field (w0 >> 16 & 0xff) - 16 of the vertex w0 & 0xfff */
        int rel = (int)((w0 >> 16) & 0xff) - 16;
        if (MUT(218, rej_vidx_bad(w0 & 0xfff) || rel < 0 || rel > 16 || (rel & 3), 0))
            return hle_decline(t, "G_MODIFYVTX out of range");
        at = (unsigned)rel + hle_dm16(t, F2_VTAB + (w0 & 0xfff));
        hle_st8(t, at, (uint8_t)(w1 >> 24));
        hle_st8(t, at + 1, (uint8_t)(w1 >> 16));
        hle_st8(t, at + 2, (uint8_t)(w1 >> 8));
        hle_st8(t, at + 3, (uint8_t)w1);
        return HLE_OK;
    }
    case K_REJ_CULLDL: {                                /* the display list ends when all the vertices from the slot of w0 to the slot of w1 are marked 0x7fff in y (the records carry 0x7ffc at most: only a G_MODIFYVTX makes one) */
        unsigned a, z;
        if (MUT(222, rej_vidx_bad(w0 & 0xfff) | rej_vidx_bad(w1 & 0xfff), 0))
            return hle_decline(t, "G_CULLDL with an unusual vertex number");
        a = hle_dm16(t, F2_VTAB + (w0 & 0xfff));
        z = hle_dm16(t, F2_VTAB + (w1 & 0xfff));
        for (k = 0;; k++) {
            if (hle_dm16(t, a + 10) != 0x7fff)
                return HLE_OK;
            if (a == z) {
                return f3_enddl(f);
            }
            a += REJ_VSTRIDE;
            if (k > 256)
                return hle_decline(t, "G_CULLDL range");
        }
    }
    case K_REJ_BRANCHZ: {
        unsigned vs;
        if (MUT(223, rej_vidx_bad(w0 & 0xfff), 0))
            return hle_decline(t, "G_BRANCH_Z with an unusual vertex number");
        vs = hle_dm16(t, F2_VTAB + (w0 & 0xfff));
        if ((int32_t)(hle_dm32(t, vs + 12) - w1) < 0)      /* a branch (no stack) to the address G_RDPHALF_1 left at 0xd8 */
            return f3_branch(f, hle_dm32(t, 0xd8));
        return HLE_OK;
    }
    }
    return hle_decline(t, "unsupported Rej command");
}

/* the commands the front-end does not know by pc: { handler pc, kind } */
static const uint16_t l_ext_xbus[][2] = { { 0x4a4, K_REJ_VTX }, { 0x74c, K_REJ_TRI1 }, { 0x750, K_REJ_TRI2 }, { 0xc28, K_REJ_MODVTX }, { 0xbec, K_REJ_CULLDL }, { 0xc10, K_REJ_BRANCHZ }, { 0xffff, 0 } };

/* Excitebike 64: F3DLX.Rej xbus 2.08 */
static const struct f3dex2_build l_b_rej_xbus = {
    .head = 0x194, .copy = 0x210, .sync = 0x214, .segcopy = 0x220, .ef = 0x240, .ed = 0x24c,
    .mtx = 0x0f0, .geom = 0x1d8, .enddl = 0x1ec, .half2 = 0x200, .load_key = 0x22fd7ac92dbcdc70ull,
    .lay = &l_lay_rej_xbus, .ext = l_ext_xbus, .ext_cmd = rej_ext_cmd };

/* The text Excitebike 64 loads in the middle of its tasks (G_LOAD_UCODE; key f649a083..., 3912 bytes): F3DEX2.NoN 2.08 with the XBUS transport. Its code is the registered F3DEX2.NoN 2.08's (Hydro Thunder's text, the .NoN reject mask
 * 0x70b0) with a shorter initialisation, so the handlers sit 0x28 lower, and the ring begins at 0xba8; the DMEM state is the standard one. The front-end's own vertex, triangle and clipping code runs it. */
static const struct f2_layout l_lay_ex2_xbus = {
    .mv = 0x00, .p = 0x40, .mvp = 0x80, .persp_norm = 0xdc, .viewport_scale = 0xe0, .viewport_trans = 0xe8, .clip_ratio = 0x186, .const31 = 0x1b0, .const30 = 0x1c0,
    .fog_mul = 0x1e0, .fog_ofs = 0x1e2, .texture = 0x1e4, .tex_scale_s = 0x1e8, .tex_scale_t = 0x1ea, .geom = 0x1ec, .mvp_valid = 0x1d9, .lights_valid = 0x1dc, .lights_size = 0x1dd,
    .light_raw = 0x1f8, .light_base = 0x200, .tg = 0x1d0, .light_scale = 0x1c6, .light_scale_zex = 0x1ce, .vtab = 0x380, .clipmask = 0x3cc, .culltab = 0x3c2, .in = 0x920, .in_end = 0x9c8,
    .jump_table = 0x36e, .mw_max = 14, .geom_def = F2_GEOM_DEF, .movemem_tab = 0x2f0, .moveword_adj = 10238, .desc_light = 0x410, .desc_clip = 0x418, .clip_planes = 0x180, .clip_masks = 0x3f8,
    .xbus = 1, .ring_start = 0xba8 };
static const struct f3dex2_build l_b_ex2_208n_xbus = {
    .head = 0x194, .copy = 0x210, .sync = 0x214, .segcopy = 0x220, .ef = 0x240, .ed = 0x24c, .tri1 = 0x794, .tri2 = 0x78c, .vtx = 0x4d0,
    .mtx = 0x0f0, .geom = 0x1d8, .enddl = 0x1ec, .half2 = 0x200, .modvtx = 0xc58, .culldl = 0xc18, .branchz = 0xc40, .reject_mask = 0x70b0, .lay = &l_lay_ex2_xbus, .load_key = 0xf649a08381045b6bull };
static const struct hle_dead_range l_dead_ex2_xbus[] = { F3DEX2_DEAD_RANGES };
HLE_UCODE(l_ex2_xbus_f649) = { .id = 0xf649a08381045b6bull, .name = "F3DEX.NoN xbus 2.08 (Excitebike 64, loaded by G_LOAD_UCODE)", .fam = &f3dex2_fam, .flags = HLE_F_XBUS_OK, .fam_data = &l_b_ex2_208n_xbus,
    .dead = l_dead_ex2_xbus, .n_dead = F3DEX2_N_DEAD_OUT };

/* dead at the end of a task: the display list input buffer (the lean mode reads the list from RDRAM), the RDP list ring beyond the last list (scratch the triangle code writes past the end of its command, and the
 * garbage the vertex loop's pipeline stores ahead of the output pointer), the padding bytes of the light slots; and, because the tasks switch to the F3DEX2.NoN text above in the middle and end in it, that
 * text's dead ranges too (the clipper's polygon lists, the new vertices; the equivalence shim masks by the ucode a task starts with) */
static const struct hle_dead_range l_dead_rej_xbus[] = { F3DEX2_DEAD_RANGES };
HLE_UCODE(l_rej_8b0a) = { .id = 0x8b0a3e3240821457ull, .name = "F3DLX.Rej xbus 2.08 (Excitebike 64)", .fam = &f3dex2_fam, .flags = HLE_F_XBUS_OK, .fam_data = &l_b_rej_xbus,
    .dead = l_dead_rej_xbus, .n_dead = F3DEX2_N_DEAD_OUT };

#endif
