/*
 * arMLEt - dbuild.h: a build record derived at run time (derive.c) as the data a front-end runs: struct f3dex2_build + f2_layout + f2_pcs + hle_ucode (the F3DEX2 family and S2DEX 2.x),
 * or struct f3dex1_build + f3dex_layout + hle_ucode (F3DEX 1.x), and the conversion of the registered builds to the derivation's normalised record (the test that derive_refs.c is in step with the tables)
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ARMLET_DBUILD_H
#define ARMLET_DBUILD_H

#include <stdint.h>

#include "derive.h"
#include "hle_f3dex2_int.h"
#include "f3dex_bld.h"

#ifdef ARMLET_NATIVE
struct dbuild {
    struct hle_ucode uc;
    struct f3dex2_build b;
    struct f2_layout lay;
    struct f2_pcs pcs;
    struct f3dex1_build b1;             /* (F3DEX 1.x) */
    struct f3dex_layout lay1;
    uint16_t ext[DERIVE_MAXEXT + 1][2];  /* the dialect commands (S2DEX), ending in { 0xffff, 0 } */
    struct hle_dead_range dead[64];
    char name[112];
    struct derive_rec rec;
    int verdict;
    int primary;                        /* index in derive_refs */
};

/* The registered hle_ucode whose F3DEX2-family or F3DEX 1.x build has this load key (the reference of a derivation), or NULL. */
const struct hle_ucode *dbuild_registered(uint64_t load_key);

/* Fills d from a derivation result of the F3DEX2 family or of F3DEX 1.x (the record's layout, pcs, attributes and reject mask; for F3DEX 1.x a handler pc of 0 means the build has no such command,
 * which the derivation only says when no opcode of the text's tables leads to the handler; the front-end declines it). F3DEX2: the layout, pcs, flags and reject mask of the record; the dead ranges, flags and ring of the primary reference's registered entry (copied,
 * not derived: docs/ucode-derive.md). A record with dialect commands (S2DEX) runs them with the primary reference's handler code (f3dex2_build.ext_cmd): everything it needs of the build is
 * in the record (the handler table, the continuation pcs of G_OBJ_LOADTXTR, the layout). A handler that the primary reference marks as absent (0, 0xffff) may be absent. Returns 0, or nonzero with
 * *why: the record is incomplete (a pc the derivation could not map), or the primary reference is not registered. */
int dbuild_make(struct dbuild *d, uint64_t text_id, const struct derive_result *r, const char **why);

/* The normalised record of a registered build (hle_f3dex2.c, hle_f3dex2_dialects.c, hle_s2dex2.c, f3dex.c): what tools/derive_refs_gen.py reads from the tables. */
void dbuild_rec_of(const struct hle_ucode *u, struct derive_rec *out);

/* nonzero when the two records have the same fields (derive_rec_fields, and the extension table); the first differing field goes to *first */
int dbuild_rec_equal(const struct derive_rec *a, const struct derive_rec *b, const char **first);
#endif

#endif
