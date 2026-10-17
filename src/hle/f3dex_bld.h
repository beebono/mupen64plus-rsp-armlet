/*
 * arMLEt - f3dex_bld.h: the build record of the F3DEX 1.x / F3DLX front-end (f3dex.c), which dbuild.c also fills from a derived record (docs/native-f3dex.md section 3, docs/ucode-derive.md)
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ARMLET_F3DEX_BLD_H
#define ARMLET_F3DEX_BLD_H

#include <stdint.h>

#include "f3dex_dmem.h"
#include "f3dex_fx.h"
#include "hle.h"

/* the handler pcs of a build, in the order of f3dex1_build.pc[] (and of tools/derive_f1d.py's F1_PC_FIELDS). head: where the start-up code reaches the command loop (the interpreter stops there);
 * nop: the loop's return point, which the no-op commands lead to (a class-0 one after its operand DMA) */
#define F3DEX_PC_FIELDS(X) \
    X(head) X(nop) X(mtx) X(movemem) X(vtx) X(dl) X(loaduc) X(branchz) X(tri2) X(modvtx) X(half2) X(half1) X(persp) X(quad) X(clrgeom) X(setgeom) X(enddl) X(oml) X(omh) X(texture) \
    X(moveword) X(popmtx) X(culldl) X(tri1)
enum {
#define X(f) F1PC_##f,
    F3DEX_PC_FIELDS(X)
#undef X
    F1_NPC
};

struct f3dex1_build {
    struct f3dex_build fx;              /* reject mask, NoN, LX, the DMEM layout */
    uint16_t pc[F1_NPC];                /* by F1PC_*; 0: the build has no such command */
    uint8_t culldl_off;                 /* G_CULLDL's operands are byte offsets into the vertex buffer and the last vertex is not tested (the 0.95 builds), not vertex numbers */
    uint64_t load_key;                  /* FNV-1a-64 of the text's first 0xf48 bytes (the derivation's record key; 0: no dump of the text) */
};

#ifdef ARMLET_NATIVE
extern const struct hle_family f3dex1_fam;
#endif

#endif
