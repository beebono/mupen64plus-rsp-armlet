/*
 * arMLEt - probe_cases.h: the canned display lists of the probe (probe_cases.c)
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ARMLET_PROBE_CASES_H
#define ARMLET_PROBE_CASES_H

#include <stdint.h>

enum { PROBE_CLIP, PROBE_ZNEG, PROBE_TRI, PROBE_MIX, PROBE_WILD, PROBE_MISC, PROBE_LIT, PROBE_MTX, PROBE_REJ, PROBE_BZ,
       PROBE_S2RECT, PROBE_S2LDTX, PROBE_S2BG, PROBE_S2COPY, PROBE_S2MISC,     /* S2DEX 2.x */
       PROBE_NCASES };
extern const char *const probe_case_names[PROBE_NCASES];

enum { PROBE_MEM = 0x1200 };            /* the bytes behind the base address a case uses */

struct probe_case {
    uint32_t base;                      /* the RDRAM address of mem[0] (the display list) */
    uint32_t used;                      /* bytes of mem that hold the case (the rest is zero) */
    uint32_t dl_bytes;
    uint8_t mem[PROBE_MEM];             /* big-endian bytes of RDRAM */
};

/* case `kind` (PROBE_*) for `seed`, laid out at RDRAM address `base`; deterministic */
void probe_case_make(struct probe_case *c, unsigned kind, unsigned seed, uint32_t base);

/* The same cases for an F3DEX 1.x build (the GBI of F3DEX 1.x: G_VTX 0x04, G_TRI1 0xbf, G_TRI2 0xb1, ...). The commands whose opcode differs between the builds are given by the caller
 * (probe.c: the opcode whose handler-table cell leads to the record's pc; 0: the build has no such command, the case leaves it out), and the form of G_CULLDL by the record. */
struct probe_f1ops {
    uint8_t half1, half2, branchz, modvtx, persp, quad;
    uint8_t culldl_off;
};
void probe_case_make_f1(struct probe_case *c, unsigned kind, unsigned seed, uint32_t base, const struct probe_f1ops *ops);

/* The same cases for an F3D build (Fast3D's GBI: G_VTX 0x04 with the count and the first slot in byte 1, G_TRI1 0xbf with the slot offsets scaled and the flat-shading vertex in w1's top byte,
 * G_CULLDL over byte offsets ...). The build's encodings are given by the caller (probe.c, from the record): the opcodes whose number differs (the class-2 opcode whose handler-table cell leads to
 * the record's pc; 0: none), Wave Race's G_VTX encoding, the triangle index scale, the vertex slots. */
struct probe_f3ops {
    uint8_t half1, half2, persp, quad;
    uint8_t vtx_wr;                     /* G_VTX: 04 | 5 v0 | n << 9 | 16 n - 1 */
    uint8_t vptr_mul;                   /* G_TRI1's index byte is 40 i / vptr_mul */
    uint8_t nslots;                     /* the vertex slots (16 or 32) */
    uint8_t flat;                       /* the flag byte of G_TRI1 selects the flat-shading vertex (0 .. 2) */
};
void probe_case_make_f3(struct probe_case *c, unsigned kind, unsigned seed, uint32_t base, const struct probe_f3ops *ops);

#endif
