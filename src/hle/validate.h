/*
 * arMLEt - validate.h: the background validator (docs/design.md, "The background validator and the latch policy")
 *
 * A graphics task that the JIT runs for a candidate build (a derived build whose probe passed) is captured on the emulation thread: its start state, the RDRAM rows its DMA reads read, as
 * they read them, and its result (the end state, the RDP stream, the RDRAM rows it wrote outside the output ring). A thread of the validator's own, at the lowest priority, replays the task
 * through the candidate build (hle_internal.h, the detached replay) and compares; the outcome goes to the latch policy (probe.c). The emulation thread never waits for it: a capture is only
 * started when a slot of the bounded queue is free.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ARMLET_VALIDATE_H
#define ARMLET_VALIDATE_H

#include <stdint.h>

#include "armlet.h"
#include "hle.h"
#include "latch.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { VAL_MATCH, VAL_MISMATCH, VAL_DECLINED, VAL_INCONCLUSIVE, VAL_IRRELEVANT };

struct val_result {
    int outcome;                        /* VAL_*: equal (tol: within the near-match tolerance); different; the build declined the task; nothing to compare; a loaded build that the task did not load */
    int tol;
    int grade;                          /* a mismatch: LATCH_G_* (latch_grade of the compare's classes for a loose target; STRUCTURAL otherwise) */
    uint64_t fp;                        /* the task's fingerprint (latch_fingerprint) */
    const uint32_t *hist;               /* its command kinds, counted (LATCH_NKINDS) */
    const char *why;                    /* declined / inconclusive: why */
    const char *report;                 /* a mismatch: the first differences */
    double replay_us;
};

/* What a capture validates: the build under validation `cand`, the build the task starts with `run` (cand itself, or the task's own build when cand is a text its G_LOAD_UCODE loads),
 * the near-match tolerance, and the policy's owner, which gets the result (on the validator thread) */
struct val_target {
    void *owner;
    const struct hle_ucode *cand, *run;
    int tol;
    int loose_levels;                   /* a loose build: the colour levels of the NUMERIC grade (0: a strict build, every mismatch STRUCTURAL) */
    int (*dmem_ok)(const struct hle_ucode *u, const uint32_t *dmem, unsigned addr, unsigned na, unsigned nb, int lim);
};
typedef void (*val_result_fn)(void *owner, const struct val_result *r);

#ifdef ARMLET_NATIVE
/* InitiateRSP (GfxProbe on): starts the thread. queue: the number of capture slots. */
int armlet_valid_start(const struct latch_cfg *cfg, val_result_fn fn);
/* RomClosed / PluginShutdown: stops the thread (the replay in progress ends first), frees the slots. The statistics go to the log. */
void armlet_valid_stop(void);
int armlet_valid_running(void);

/* Emulation thread. Whether a capture could start now (a free slot). */
int armlet_valid_room(void);
/* The next JIT run of a fresh graphics task (HleForwardTask) is captured for `t`. Returns 0, or -1 (no free slot: nothing armed). */
int armlet_valid_arm(const struct val_target *t);
/* around the JIT's run of the task (hle_glue.c) */
void armlet_valid_jit_begin(struct armlet_ctx *c);
void armlet_valid_jit_end(struct armlet_ctx *c);

struct val_stats {
    uint64_t armed, captured, dropped_busy, dropped_capture, replayed, matched, mismatched, declined, inconclusive, irrelevant;
    double capture_ms, replay_cpu_ms, thread_cpu_ms;    /* emulation thread's capture time; the validator thread's CPU time in replays and in all */
    uint64_t slot_bytes;                                /* the capture slots' memory at its largest */
};
void armlet_valid_stats(struct val_stats *s);
#else
static inline int armlet_valid_room(void) { return 0; }
static inline void armlet_valid_jit_begin(struct armlet_ctx *c) { (void)c; }
static inline void armlet_valid_jit_end(struct armlet_ctx *c) { (void)c; }
#endif

#ifdef __cplusplus
}
#endif

#endif
