/*
 * arMLEt - derive.h: the derivation of a build record from a graphics microcode's text, in C (docs/ucode-derive.md, "The C port"; the Python tool is tools/derive_build.py)
 *
 * An unknown text (+ the overlays behind it, + the data segment's jump table) is aligned with the references of derive_refs.c (the registered builds); the build record, a verdict
 * and the unexplained differences come out. Pure computation: no RDRAM, no RSP state, no globals other than the lazily built shape lookup (none in this file).
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ARMLET_DERIVE_H
#define ARMLET_DERIVE_H

#include <stddef.h>
#include <stdint.h>

#include "derive_int.h"

#ifdef __cplusplus
extern "C" {
#endif

/* bump when the derivation's decisions change (the cache is keyed with it) */
#define DERIVE_TOOL_VERSION 2

enum { DERIVE_NONE = 0, DERIVE_NEAR = 1, DERIVE_EXACT = 2 };
enum { DERIVE_F3DEX2 = 0, DERIVE_F3DEX1 = 1, DERIVE_F3D = 2, DERIVE_AUTO = -1 };
enum { DERIVE_MAXREFS = 32 };

struct derive_in {
    const uint32_t *text;           /* 1024 words: the 4 KiB at OSTask.ucode, as numbers (the big-endian words of the dump files) */
    const uint8_t *data;            /* the data segment, big-endian bytes (the DMEM image: jump table, constants, overlay descriptors) */
    size_t data_len;
    const uint8_t *ext;             /* the RDRAM behind the text: ext[0] is the byte at text + 0x1000 (the overlays of the lighting / S2DEX); NULL: none */
    size_t ext_len;
    int family;                     /* DERIVE_AUTO or a family: compute only that one */
    uint64_t exclude_recs;          /* bit r: the record r of derive_recs is not a reference (leave-one-out validation); 0 for production */
    int full;                       /* nonzero: always compute every family (the test); else the F3DEX2 family alone when it explains the text well (>= 0.95 aligned) */
};

struct derive_result {
    int family;                     /* of the best explanation */
    int verdict;                    /* DERIVE_NONE / NEAR / EXACT (a statement about the code: docs/ucode-derive.md "Verdicts") */
    unsigned aligned_num, aligned_den;
    unsigned nx;                    /* the family's residual count that decides between families */
    unsigned n_x;                   /* unexplained words */
    unsigned n_near[4];             /* near-class words: init, light-positional, light-small, geom-guarded (F3DEX 1.x) */
    unsigned n_xr[3];               /* reference-only words: init, extra, deleted */
    int primary;                    /* index in derive_refs */
    int n_contrib;
    struct {
        int ref;
        unsigned n;
    } contrib[DERIVE_MAXREFS];
    int knob_trouble;               /* a constant read differently at different places (the verdict is NONE) */
    struct derive_rec rec;          /* DERIVE_F3DEX2 and DERIVE_F3DEX1: the derived record (name "(derived)"; F3DEX2: load_key, pcs, layout, flags, reject mask, extension table; F3DEX 1.x: derive_int.h) */
    unsigned fam_aligned[3][2];     /* per family tried: aligned words, image words (0 / 0 when not tried) */
    unsigned fam_nx[3];
    int fam_verdict[3];
    unsigned long long work_ns;     /* time spent (CLOCK_MONOTONIC), for the log */
};

/* 0 on success; nonzero on bad input or no memory. */
int derive_run(const struct derive_in *in, struct derive_result *out);

/* the ucode id / load key hash of the first `nwords` words (FNV-1a-64, one word per step: gfx_ucode.c's armlet_ucode_hash over numbers) */
uint64_t derive_fnv_words(const uint32_t *w, unsigned nwords);

/* the record's fields by the Python tool's names ("pc.head", "lay.mv", "reject_mask" ...), for the comparison tests: calls f(name, value, arg) for each */
void derive_rec_fields(const struct derive_rec *r, void (*f)(const char *name, unsigned long long v, void *arg), void *arg);

const char *derive_verdict_name(int v);
const char *derive_family_name(int f);

#ifdef __cplusplus
}
#endif

#endif
