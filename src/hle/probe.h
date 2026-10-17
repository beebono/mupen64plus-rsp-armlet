/*
 * arMLEt - probe.h: the probe (docs/design.md, "The probe"): a graphics microcode with no table entry is derived (derive.c) and, when the derivation says EXACT or NEAR, its derived build is
 * tried on canned display lists against the JIT while the JIT goes on running the game's own tasks. A pass is remembered in the ucode cache (ucache.h).
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ARMLET_PROBE_H
#define ARMLET_PROBE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "armlet.h"
#include "gfx_ucode.h"
#include "hle.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ARMLET_NATIVE
void armlet_probe_init(struct armlet_ctx *c);       /* InitiateRSP: reads the configuration, opens the cache */
void armlet_probe_reset(void);                      /* RomClosed: forgets the pairs and the latched builds (the cache file stays) */

/* A graphics task of ucode `u` is about to run on the JIT (or natively). When the ucode has no handler (or ARMLET_PROBE_FORCE is on), this derives its record on first sight and runs the probe's
 * canned tasks a few at a time on the following calls. Never changes the task: the RSP state, the JIT's state and the RDRAM are as they were when it returns. */
void armlet_probe_offer(struct armlet_ctx *c, const struct armlet_ucode_id *u);

/* Whether the native handler of `u` may run this task: a build that was latched for one (text, data) pair does not run another data image. Registered builds: always. */
bool armlet_probe_gate(struct armlet_ctx *c, const struct armlet_ucode_id *u);

/* A pair's outcome, for the offline harness and the tests */
enum { PROBE_OUT_NONE, PROBE_OUT_NO_CANDIDATE, PROBE_OUT_UNSUPPORTED, PROBE_OUT_PASS, PROBE_OUT_PASS_TOL, PROBE_OUT_FAIL, PROBE_OUT_INCONCLUSIVE, PROBE_OUT_PENDING };
struct probe_summary {
    int outcome;                    /* PROBE_OUT_* */
    int verdict, family;            /* the derivation's (DERIVE_*) */
    int from_cache;
    int latched;
    int rec_equal_registered;       /* ARMLET_PROBE_FORCE: the derived record equals the registered one (1), differs (0), or not asked (-1) */
    const char *first_diff;         /* the first field that differs, or NULL */
    unsigned n_tasks, n_equal, n_tol, n_declined, n_mismatch, n_inconclusive;
    double derive_ms, probe_ms;
    char note[200];
};
int armlet_probe_summary(uint64_t text_id, struct probe_summary *out);   /* 0: the pair is known */
/* test aid (tools/hle/probe_replay.c --task): the canned tasks with a registered build, no derivation, all at once; returns the number that differed from the JIT (or -1) */
int armlet_probe_run_registered(struct armlet_ctx *c, const struct hle_ucode *u);
#else
static inline void armlet_probe_init(struct armlet_ctx *c) { (void)c; }
static inline void armlet_probe_reset(void) {}
static inline void armlet_probe_offer(struct armlet_ctx *c, const struct armlet_ucode_id *u) { (void)c; (void)u; }
static inline bool armlet_probe_gate(struct armlet_ctx *c, const struct armlet_ucode_id *u) { (void)c; (void)u; return true; }
#endif

#ifdef __cplusplus
}
#endif

#endif
