/*
 * arMLEt - ucache.h: the persistent cache of what the probe learned about a graphics microcode (docs/design.md, "The ucode cache")
 *
 * One file, keyed by (text id, data id) and the tool version; each entry holds the derived record, the verdict, and the probe's result. Entries are checksummed and ignored when bad,
 * a truncated file loses only its tail, writes are atomic (a temporary file and a rename), the number of entries is bounded.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ARMLET_UCACHE_H
#define ARMLET_UCACHE_H

#include <stddef.h>
#include <stdint.h>

#include "derive_int.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCACHE_FORMAT 4           /* 2: the continuation pcs of S2DEX (pc[]) and the dialect command table (ext); 3: F3DEX 1.x records (f1pc, non, dlx, culldl_off; the layout in lay[]); 4: F3D records (f3pc, the masks, f3[]) */
#define UCACHE_MAX_ENTRIES 512          /* oldest-used entries go first when the cache is full */

enum { UC_PROBE_NONE = 0, UC_PROBE_PASS = 1, UC_PROBE_FAIL = 2, UC_PROBE_PASS_TOL = 3, UC_PROBE_UNSUPPORTED = 4 };

/* the stored form: fixed-width fields, a checksum last */
struct ucache_entry {
    uint64_t text_id, data_id;
    uint32_t tool;                      /* ucache_tool_version() at the time */
    uint32_t seq;                       /* use counter (the least recent is evicted) */
    uint8_t verdict, family, probe, flags;
    uint16_t primary;                   /* index in derive_refs */
    uint16_t n_equal, n_tol, n_declined, n_mismatch, n_inconclusive, pad[3];
    int32_t pc[DERIVE_NPC];
    int32_t f2pcs[DERIVE_NF2PCS];
    uint32_t lay[DERIVE_NLAY];
    uint32_t reject_mask;
    uint8_t gen204, zex, bz_z, pad2;
    uint64_t load_key;
    uint16_t ext[DERIVE_MAXEXT][2];     /* S2DEX: { handler pc, kind } */
    uint32_t n_ext, pad4;
    int32_t f1pc[DERIVE_NF1PC];         /* F3DEX 1.x */
    uint8_t non, dlx, culldl_off, pad5;
    uint32_t pad6;
    int32_t f3pc[DERIVE_NF3PC];         /* F3D (the layout in lay[]) */
    uint32_t pad8;
    uint32_t clip_mask, culldl_mask;
    uint8_t f3[F3F_N], pad7;
    uint32_t crc;
    uint32_t pad3;
};
_Static_assert(sizeof(struct ucache_entry) == offsetof(struct ucache_entry, crc) + 8, "ucache_entry has no holes: the checksum covers the bytes before crc");

/* The version of the derivation and of the file format together with a signature of the reference data: an entry made by another version is not used. */
uint32_t ucache_tool_version(void);

/* Opens (and loads) the cache at `path`; NULL or "" disables it (every find misses, store does nothing). Returns the number of entries kept. */
int ucache_open(const char *path);
void ucache_close(void);
const char *ucache_path(void);

const struct ucache_entry *ucache_find(uint64_t text_id, uint64_t data_id);
/* Adds or replaces the entry (key text_id, data_id; crc and tool are set here) and writes the file. Returns 0, or nonzero when the file could not be written (the memory copy is kept). */
int ucache_store(const struct ucache_entry *e);

/* the record of an entry as a derive_rec, and the other way round */
void ucache_from_rec(struct ucache_entry *e, const struct derive_rec *r);
void ucache_to_rec(const struct ucache_entry *e, struct derive_rec *r);

uint32_t ucache_crc32(const void *p, size_t n);

#ifdef __cplusplus
}
#endif

#endif
