/*
 * arMLEt - dbuild.c: see dbuild.h
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "dbuild.h"

#ifdef ARMLET_NATIVE

const struct hle_ucode *dbuild_registered(uint64_t load_key)
{
    const struct hle_ucode *u = NULL;
    while ((u = armlet_hle_next(u)) != NULL)
        if ((u->fam == &f3dex2_fam && u->fam_data && ((const struct f3dex2_build *)u->fam_data)->load_key == load_key) ||
            (u->fam == &f3dex1_fam && u->fam_data && ((const struct f3dex1_build *)u->fam_data)->load_key == load_key && load_key))
            return u;
    return NULL;
}

/* the layout fields in the order of the derivation's lay[] (LY_*) */
#define LAY_FIELDS(X) \
    X(mv) X(p) X(mvp) X(persp_norm) X(viewport_scale) X(viewport_trans) X(clip_ratio) X(const31) X(const30) X(fog_mul) X(fog_ofs) X(texture) X(tex_scale_s) X(tex_scale_t) X(geom) \
    X(mvp_valid) X(lights_valid) X(lights_size) X(light_raw) X(light_base) X(tg) X(light_scale) X(light_scale_zex) X(vtab) X(clipmask) X(culltab) X(in) X(in_end) X(jump_table) \
    X(movemem_tab) X(moveword_adj) X(desc_light) X(desc_clip) X(clip_planes) X(clip_masks) X(out_toggle) X(geom_def) X(mw_max) X(out_bytes) X(pipe_sync) X(no_lit)

#define F1LAY_FIELDS F3DEX_LAY_FIELDS

void dbuild_rec_of(const struct hle_ucode *u, struct derive_rec *r)
{
    const struct f3dex2_build *b = (const struct f3dex2_build *)u->fam_data;
    const struct f2_layout *l;
    unsigned k = 0;
    memset(r, 0, sizeof(*r));
    r->name = u->name;
    if (u->fam == &f3dex1_fam) {
        const struct f3dex1_build *b1 = (const struct f3dex1_build *)u->fam_data;
        const struct f3dex_layout *l1 = f3dex_lay(&b1->fx);
        r->fam = DERIVE_F3DEX1;
#define X(f) r->lay[k++] = l1->f;
        F1LAY_FIELDS(X)
#undef X
        for (k = 0; k < DERIVE_NF1PC; k++)
            r->f1pc[k] = b1->pc[k];
        r->reject_mask = b1->fx.reject_mask;
        r->non = b1->fx.non;
        r->dlx = b1->fx.dlx;
        r->culldl_off = b1->culldl_off;
        r->load_key = b1->load_key;
        return;
    }
    l = b->lay ? b->lay : f3dex2_std_layout();
    r->fam = DERIVE_F3DEX2;
    r->pc[PC_HEAD] = b->head; r->pc[PC_COPY] = b->copy; r->pc[PC_SYNC] = b->sync; r->pc[PC_SEGCOPY] = b->segcopy; r->pc[PC_EF] = b->ef; r->pc[PC_ED] = b->ed;
    r->pc[PC_TRI1] = b->tri1; r->pc[PC_TRI2] = b->tri2; r->pc[PC_VTX] = b->vtx; r->pc[PC_MTX] = b->mtx; r->pc[PC_GEOM] = b->geom; r->pc[PC_ENDDL] = b->enddl;
    r->pc[PC_HALF2] = b->half2; r->pc[PC_MODVTX] = b->modvtx; r->pc[PC_CULLDL] = b->culldl; r->pc[PC_BRANCHZ] = b->branchz; r->pc[PC_MVP] = b->mvp; r->pc[PC_TRI4] = b->tri4;
    r->pc[PC_LDTX_SPR] = b->ldtx_spr; r->pc[PC_LDTX_RECT] = b->ldtx_rect;
    if (b->pcs) {
        r->f2pcs[P2_DL] = b->pcs->dl; r->f2pcs[P2_TEXTURE] = b->pcs->texture; r->f2pcs[P2_TEXRECT] = b->pcs->texrect; r->f2pcs[P2_HALF1] = b->pcs->half1;
        r->f2pcs[P2_MOVEWORD] = b->pcs->moveword; r->f2pcs[P2_POPMTX] = b->pcs->popmtx; r->f2pcs[P2_MOVEMEM] = b->pcs->movemem; r->f2pcs[P2_OMH] = b->pcs->omh; r->f2pcs[P2_OML] = b->pcs->oml;
    } else {
        r->f2pcs[P2_DL] = PC_DL; r->f2pcs[P2_TEXTURE] = PC_TEXTURE; r->f2pcs[P2_TEXRECT] = PC_TEXRECT; r->f2pcs[P2_HALF1] = PC_HALF1; r->f2pcs[P2_MOVEWORD] = PC_MOVEWORD;
        r->f2pcs[P2_POPMTX] = PC_POPMTX; r->f2pcs[P2_MOVEMEM] = PC_MOVEMEM; r->f2pcs[P2_OMH] = PC_OMH; r->f2pcs[P2_OML] = PC_OML;
    }
#define X(f) r->lay[k++] = l->f;
    LAY_FIELDS(X)
#undef X
    r->lay[LY_LIT_VTX] = l->lit_vtx != NULL;
    r->gen204 = (uint8_t)b->gen204;
    r->zex = (uint8_t)b->zex;
    r->bz_z = (uint8_t)b->bz_z;
    r->reject_mask = b->reject_mask;
    r->load_key = b->load_key;
    if (b->ext)
        for (k = 0; b->ext[k][0] != 0xffff && k < DERIVE_MAXEXT; k++) {
            r->ext[r->n_ext].pc = b->ext[k][0];
            r->ext[r->n_ext++].kind = (uint8_t)b->ext[k][1];
        }
}

int dbuild_rec_equal(const struct derive_rec *a, const struct derive_rec *b, const char **first)
{
    unsigned k;
    if (first)
        *first = NULL;
#define CMP(field, label) if (a->field != b->field) { if (first) *first = label; return 0; }
    for (k = 0; k < DERIVE_NPC; k++)
        CMP(pc[k], "pc")
    for (k = 0; k < DERIVE_NF2PCS; k++)
        CMP(f2pcs[k], "f2pcs")
    for (k = 0; k < DERIVE_NLAY; k++)
        CMP(lay[k], "lay")
    for (k = 0; k < DERIVE_NF1PC; k++)
        CMP(f1pc[k], "f1pc")
    CMP(fam, "fam") CMP(non, "non") CMP(dlx, "dlx") CMP(culldl_off, "culldl_off")
    CMP(gen204, "gen204") CMP(zex, "zex") CMP(bz_z, "bz_z") CMP(reject_mask, "reject_mask") CMP(load_key, "load_key") CMP(n_ext, "n_ext")
#undef CMP
    for (k = 0; k < a->n_ext; k++) {
        unsigned q, found = 0;
        for (q = 0; q < b->n_ext; q++)
            if (a->ext[k].pc == b->ext[q].pc && a->ext[k].kind == b->ext[q].kind)
                found = 1;
        if (!found) {
            if (first)
                *first = "ext";
            return 0;
        }
    }
    return 1;
}

/* the F3DEX 1.x part of dbuild_make */
static int make_f1(struct dbuild *d, uint64_t text_id, const struct derive_result *r, const char **why)
{
    const struct derive_rec *rec = &r->rec, *prec;
    const struct derive_ref *pr;
    const struct hle_ucode *pu;
    unsigned k = 0;
    if (r->primary < 0) {
        *why = "no primary reference";
        return 1;
    }
    pr = &derive_refs[r->primary];
    prec = &derive_recs[pr->rec];
    for (k = 0; k < DERIVE_NF1PC; k++) {
        int v = rec->f1pc[k];
        if (v == 0 && k > F1PC_nop)
            continue;                                   /* (the derivation found no handler the text's tables lead to: the command declines) */
        if (v <= 0 || v >= 0x1000 || (v & 3)) {
            *why = "a handler pc of the record is missing or not an instruction address";
            return 1;
        }
    }
    for (k = 0; k < DERIVE_NF1LAY; k++)
        if (rec->lay[k] >= 0x1000 || (k < DERIVE_NF1KNOB && rec->lay[k] == 0)) {
            *why = "a DMEM address of the record is outside DMEM";
            return 1;
        }
    if (!rec->reject_mask) {
        *why = "no reject mask";
        return 1;
    }
    pu = dbuild_registered(prec->load_key);
    if (!pu || pu->fam != &f3dex1_fam) {
        *why = "the primary reference is not a registered build";
        return 1;
    }
    d->primary = r->primary;
    d->verdict = r->verdict;
    d->rec = *rec;
    k = 0;
#define X(f) d->lay1.f = (uint16_t)rec->lay[k++];
    F1LAY_FIELDS(X)
#undef X
    for (k = 0; k < DERIVE_NF1PC; k++)
        d->b1.pc[k] = (uint16_t)rec->f1pc[k];
    d->b1.fx.reject_mask = (uint16_t)rec->reject_mask;
    d->b1.fx.non = rec->non;
    d->b1.fx.dlx = rec->dlx;
    d->b1.fx.lay = &d->lay1;
    d->b1.culldl_off = rec->culldl_off;
    d->b1.load_key = rec->load_key;
    snprintf(d->name, sizeof(d->name), "derived %s (%016" PRIx64 ", from %s)", derive_verdict_name(r->verdict), text_id, pr->name);
    d->uc.id = text_id;
    d->uc.name = d->name;
    d->uc.fam = &f3dex1_fam;
    d->uc.flags = pu->flags;
    d->uc.fam_data = &d->b1;
    d->uc.ring_off = pu->ring_off;
    d->uc.rdp_cmd_size = pu->rdp_cmd_size;
    d->uc.live_sregs = pu->live_sregs;
    if (pu->n_dead > sizeof(d->dead) / sizeof(d->dead[0])) {
        *why = "the primary reference has more dead ranges than a derived build can hold";
        return 1;
    }
    memcpy(d->dead, pu->dead, pu->n_dead * sizeof(d->dead[0]));
    d->uc.dead = d->dead;
    d->uc.n_dead = pu->n_dead;
    return 0;
}

int dbuild_make(struct dbuild *d, uint64_t text_id, const struct derive_result *r, const char **why)
{
    const struct derive_rec *rec = &r->rec;
    const struct hle_ucode *pu;
    const struct f3dex2_build *pb;
    unsigned k = 0;
    static const int need[] = { PC_HEAD, PC_COPY, PC_SYNC, PC_SEGCOPY, PC_EF, PC_ED, PC_TRI1, PC_TRI2, PC_VTX, PC_MTX, PC_GEOM, PC_ENDDL, PC_HALF2, PC_MODVTX, PC_CULLDL, PC_BRANCHZ };
    const struct derive_ref *pr;
    const struct derive_rec *prec;

    memset(d, 0, sizeof(*d));
    if (r->family == DERIVE_F3DEX1)
        return make_f1(d, text_id, r, why);
    if (r->family != DERIVE_F3DEX2) {
        *why = "not of the F3DEX2 family or F3DEX 1.x";
        return 1;
    }
    if (r->primary < 0) {
        *why = "no primary reference";
        return 1;
    }
    pr = &derive_refs[r->primary];
    prec = &derive_recs[pr->rec];
    /* a pc is a handler's address; 0 / 0xffff (no such handler) only where the primary reference has the same marker (conv_pc keeps them: the derived record has them exactly there) */
#define ABSENT(v, rv) (((v) == 0 || (v) == 0xffff) && (v) == (rv))
    for (k = 0; k < sizeof(need) / sizeof(need[0]); k++)
        if (!ABSENT(rec->pc[need[k]], prec->pc[need[k]]) && (rec->pc[need[k]] <= 0 || rec->pc[need[k]] >= 0x1000 || (rec->pc[need[k]] & 3))) {
            *why = "a handler pc of the record is missing or not an instruction address";
            return 1;
        }
    for (k = 0; k < DERIVE_NF2PCS; k++)
        if (!ABSENT(rec->f2pcs[k], prec->f2pcs[k]) && (rec->f2pcs[k] < 0 || rec->f2pcs[k] >= 0x1000 || (rec->f2pcs[k] & 3))) {
            *why = "a command pc of the record is not an instruction address";
            return 1;
        }
#undef ABSENT
    if (!rec->reject_mask && rec->pc[PC_TRI1]) {
        *why = "no reject mask";
        return 1;
    }
    if (rec->n_ext > DERIVE_MAXEXT) {
        *why = "too many dialect commands";
        return 1;
    }
    for (k = 0; k < rec->n_ext; k++)
        if (rec->ext[k].pc <= 0 || rec->ext[k].pc >= 0x1000 || (rec->ext[k].pc & 3) || rec->ext[k].kind < K_EXT) {
            *why = "a dialect command's pc of the record is missing or not an instruction address";
            return 1;
        }
    if (rec->n_ext && (rec->pc[PC_LDTX_SPR] <= 0 || rec->pc[PC_LDTX_SPR] >= 0x1000 || (rec->pc[PC_LDTX_SPR] & 3) || rec->pc[PC_LDTX_RECT] <= 0 || rec->pc[PC_LDTX_RECT] >= 0x1000 || (rec->pc[PC_LDTX_RECT] & 3))) {
        *why = "a continuation pc of G_OBJ_LOADTXTR is missing";
        return 1;
    }
    pu = dbuild_registered(prec->load_key);
    if (!pu) {
        *why = "the primary reference is not a registered build";
        return 1;
    }
    pb = (const struct f3dex2_build *)pu->fam_data;
    if (rec->n_ext && !pb->ext_cmd) {
        *why = "dialect commands, but the primary reference has no code for them";
        return 1;
    }
    d->primary = r->primary;
    d->verdict = r->verdict;
    d->rec = *rec;

    d->pcs.dl = (uint16_t)rec->f2pcs[P2_DL]; d->pcs.texture = (uint16_t)rec->f2pcs[P2_TEXTURE]; d->pcs.texrect = (uint16_t)rec->f2pcs[P2_TEXRECT]; d->pcs.half1 = (uint16_t)rec->f2pcs[P2_HALF1];
    d->pcs.moveword = (uint16_t)rec->f2pcs[P2_MOVEWORD]; d->pcs.popmtx = (uint16_t)rec->f2pcs[P2_POPMTX]; d->pcs.movemem = (uint16_t)rec->f2pcs[P2_MOVEMEM]; d->pcs.omh = (uint16_t)rec->f2pcs[P2_OMH];
    d->pcs.oml = (uint16_t)rec->f2pcs[P2_OML];
    k = 0;
#define X(f) d->lay.f = (__typeof__(d->lay.f))rec->lay[k++];
    LAY_FIELDS(X)
#undef X
    if (rec->lay[LY_LIT_VTX])
        d->lay.lit_vtx = (pb->lay ? pb->lay : f3dex2_std_layout())->lit_vtx;

    d->b.head = (uint16_t)rec->pc[PC_HEAD]; d->b.copy = (uint16_t)rec->pc[PC_COPY]; d->b.sync = (uint16_t)rec->pc[PC_SYNC]; d->b.segcopy = (uint16_t)rec->pc[PC_SEGCOPY];
    d->b.ef = (uint16_t)rec->pc[PC_EF]; d->b.ed = (uint16_t)rec->pc[PC_ED]; d->b.tri1 = (uint16_t)rec->pc[PC_TRI1]; d->b.tri2 = (uint16_t)rec->pc[PC_TRI2];
    d->b.vtx = (uint16_t)rec->pc[PC_VTX]; d->b.mtx = (uint16_t)rec->pc[PC_MTX]; d->b.geom = (uint16_t)rec->pc[PC_GEOM]; d->b.enddl = (uint16_t)rec->pc[PC_ENDDL];
    d->b.half2 = (uint16_t)rec->pc[PC_HALF2]; d->b.modvtx = (uint16_t)rec->pc[PC_MODVTX]; d->b.culldl = (uint16_t)rec->pc[PC_CULLDL]; d->b.branchz = (uint16_t)rec->pc[PC_BRANCHZ];
    d->b.mvp = rec->pc[PC_MVP] > 0 ? (uint16_t)rec->pc[PC_MVP] : 0;
    d->b.tri4 = rec->pc[PC_TRI4] > 0 ? (uint16_t)rec->pc[PC_TRI4] : 0;
    d->b.gen204 = rec->gen204;
    d->b.zex = rec->zex;
    d->b.bz_z = rec->bz_z;
    d->b.reject_mask = rec->reject_mask;
    d->b.load_key = rec->load_key;
    d->b.lay = &d->lay;
    d->b.pcs = &d->pcs;
    d->b.ldtx_spr = rec->pc[PC_LDTX_SPR] > 0 ? (uint16_t)rec->pc[PC_LDTX_SPR] : 0;
    d->b.ldtx_rect = rec->pc[PC_LDTX_RECT] > 0 ? (uint16_t)rec->pc[PC_LDTX_RECT] : 0;
    if (rec->n_ext) {
        for (k = 0; k < rec->n_ext; k++) {
            d->ext[k][0] = (uint16_t)rec->ext[k].pc;
            d->ext[k][1] = rec->ext[k].kind;
        }
        d->ext[k][0] = 0xffff;
        d->ext[k][1] = 0;
        d->b.ext = (const uint16_t (*)[2])d->ext;
        d->b.ext_cmd = pb->ext_cmd;
    } else {
        d->b.ext = NULL;
        d->b.ext_cmd = NULL;
    }

    snprintf(d->name, sizeof(d->name), "derived %s (%016" PRIx64 ", from %s)", derive_verdict_name(r->verdict), text_id, pr->name);
    d->uc.id = text_id;
    d->uc.name = d->name;
    d->uc.fam = &f3dex2_fam;
    d->uc.flags = pu->flags;
    d->uc.fam_data = &d->b;
    d->uc.ring_off = pu->ring_off;
    d->uc.rdp_cmd_size = pu->rdp_cmd_size;
    d->uc.live_sregs = pu->live_sregs;
    if (pu->n_dead > sizeof(d->dead) / sizeof(d->dead[0])) {
        *why = "the primary reference has more dead ranges than a derived build can hold";
        return 1;
    }
    memcpy(d->dead, pu->dead, pu->n_dead * sizeof(d->dead[0]));
    d->uc.dead = d->dead;
    d->uc.n_dead = pu->n_dead;
    return 0;
}

#endif
