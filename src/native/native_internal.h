/*
 * arMLEt - native_internal.h: the framework's internals shared by native.c and the offline harness (tools/native/replay.c)
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ARMLET_NATIVE_INTERNAL_H
#define ARMLET_NATIVE_INTERNAL_H

#include <stdio.h>

#include "armlet.h"
#include "native.h"

struct armlet_native_stats {
    uint64_t calls, declined, ticks, units;     /* ticks: native time inside the routine (stats on) */
    uint64_t verified, mismatches;
    uint64_t oracle_ticks;                      /* JIT time of the same stretch, verify mode with stats */
};

struct armlet_native_settings {
    int mode;                                   /* ARMLET_NATIVE_* */
    char routines[256];
    int abort_on_mismatch, stats, poison, quiet;
    const char *capture_dir;
    unsigned capture_max, capture_every;
    unsigned watchdog_ms;                       /* verify: abort when the JIT run of a routine takes longer (default 3000) */
};

/* ---- routine registry ---- */
unsigned armlet_native_count(void);
const struct armlet_routine *armlet_native_routine(unsigned idx);
struct armlet_native_stats *armlet_native_stats_of(unsigned idx);

/* settings from the config + ARMLET_NATIVE_* environment; starts the framework (installs the host in the fork when mode != off) */
void armlet_native_settings_load(struct armlet_native_settings *s, const struct armlet_config *cfg);
void armlet_native_start(const struct armlet_native_settings *s);
/* selects the routines of a ucode (allow-lists applied) and tells the fork their entry / exit pcs; returns how many */
unsigned armlet_native_select(uint64_t ucode_id);
void armlet_native_report(int final);

/* One call through the framework as the fork's hook would make it, from the current live state: entry at routine idx.
 * Returns the PRSP_NATIVE_* result like the host's enter(). Used by the harness. */
int armlet_native_call(unsigned idx, unsigned *next_pc, int *mode);

/* Harness support (tools/native/replay.c) */
int armlet_native_find(const char *name, uint64_t ucode);   /* index in the registry, or -1 */
/* Differences between two snapshots (the routine's masks applied, effect logs not compared); the first ones go to out. */
unsigned armlet_native_compare(const PrspNativeSnap *a, const PrspNativeSnap *b, unsigned idx, char *out, size_t outsz);
/* Runs the routine `iters` times from `before` (loaded each time, not timed), then the JIT's stretch to `exit_pc` the same way; ns per call. */
int armlet_native_bench(unsigned idx, const PrspNativeSnap *before, unsigned exit_pc, unsigned iters, double *native_ns, double *jit_ns, double *units);

/* Snapshot files (the offline harness, ARMLET_NATIVE_CAPTURE): */
#define ARMLET_SNAPFILE_MAGIC 0x41534e50u   /* "ASNP" */
struct armlet_snapfile_hdr {
    uint32_t magic, version;
    char routine[48];
    uint64_t ucode;
    uint32_t entry_pc, exit_pc, n_rows, n_events, flags;
};
/* file: hdr, PrspNativeSnap before, n_rows * { u32 addr, u32 len, bytes }, PrspNativeSnap after (the JIT's), n_events * PrspNativeEvent
 * (the JIT's) */

#endif
