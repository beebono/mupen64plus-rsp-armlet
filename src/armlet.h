/*
 * arMLEt (ARM-focused Mid-Level Emulation RSP plugin) - armlet.h
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ARMLET_H
#define ARMLET_H

#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>

#include "m64p_types.h"
#include "m64p_plugin.h"
#include "hle_internal.h"
#include "hle/latch.h"

#define ARMLET_MAX_ALLOW 32

enum { ARMLET_GFX_LLE, ARMLET_GFX_HLE };
enum { ARMLET_NATIVE_OFF, ARMLET_NATIVE_ON, ARMLET_NATIVE_VERIFY };
enum { ARMLET_COALESCE_OFF, ARMLET_COALESCE_ON, ARMLET_COALESCE_AUTO };
#define ARMLET_COALESCE_BYTES_DEFAULT 4096

struct armlet_config {
    int gfx_mode;                       /* ARMLET_GFX_* */
    unsigned n_allow;                   /* 0 = every ucode with a native handler */
    uint64_t allow[ARMLET_MAX_ALLOW];
    int rdp_coalesce_mode;              /* ARMLET_COALESCE_*, as configured */
    int rdp_coalesce;                   /* resolved at InitiateRSP: coalescing is active (auto: the video plugin is foir) */
    unsigned rdp_coalesce_bytes;        /* flush threshold, bytes of pending RDP list */
    int native_mode;                    /* ARMLET_NATIVE_*: native microcode routines (src/native, docs/design.md) */
    char native_routines[256];          /* allow-list: comma-separated routine names / family prefixes; "-x" excludes; empty = all */
    int probe;                          /* GfxProbe: derive and probe the build record of a graphics ucode that has no table entry (src/hle/probe.c, docs/design.md "The probe"); needs GfxMode=hle */
    int probe_latch;                    /* ARMLET_PROBE_LATCH=1 (tests): a ucode whose probe passed is latched at once, every command kind, never sampled (the latch policy's override) */
    int probe_force;                    /* ARMLET_PROBE_FORCE=1: probe the registered ucodes too, with their own record left out of the references (testing) */
    int probe_tol;                      /* GfxProbeNear = tolerant: the near-match tolerance of the probe (docs/design.md); default strict */
    unsigned probe_none_x;              /* ARMLET_PROBE_NONE_X=n (experiment): also probe a NONE derivation (any family) with at most n unexplained plus missing words; its result is not cached */
    unsigned probe_seeds;               /* ARMLET_PROBE_SEEDS: canned tasks per random case (default 3) */
    char ucode_cache[512];              /* GfxUcodeCache: path of the cache file; empty = armlet-ucode.cache in the core's config directory; "off" = none */
    struct latch_cfg latch;             /* GfxLatchPolicy / ARMLET_LATCH_POLICY: the latch policy's numbers (src/hle/latch.h) */
};

struct armlet_ctx {
    struct hle_t hle;                   /* rsp-hle state; hle.user_defined points back here */
    RSP_INFO rsp;                       /* as given by the core */
    RSP_INFO prsp;                      /* the copy handed to parallel-rsp */
    struct armlet_config cfg;
    int cached_seen;                    /* hle.cached_ucodes.count after the last task, to log new handlers */
};

/* DMEM task header (the OSTask), DMEM words are host-endian */
enum {
    ARMLET_TASK_TYPE = 0xfc0,
    ARMLET_TASK_FLAGS = 0xfc4,
    ARMLET_TASK_UCODE_BOOT_SIZE = 0xfcc,
    ARMLET_TASK_UCODE = 0xfd0,
    ARMLET_TASK_UCODE_SIZE = 0xfd4,
    ARMLET_TASK_UCODE_DATA = 0xfd8,
    ARMLET_TASK_UCODE_DATA_SIZE = 0xfdc,
    ARMLET_TASK_DATA_PTR = 0xff0,
    ARMLET_TASK_DATA_SIZE = 0xff4
};

static inline uint32_t armlet_dmem32(const struct armlet_ctx *c, unsigned off)
{
    return *(const uint32_t *)(c->rsp.DMEM + (off & 0xffc));
}

/* log.c */
void armlet_log_set(void (*cb)(void *, int, const char *), void *ctx);
void armlet_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void armlet_vlog_raw(int level, const char *fmt, va_list ap);   /* no "arMLEt: " prefix */

/* hle_glue.c */
void armlet_log_new_handler(struct armlet_ctx *c);


/* native microcode routines (src/native, docs/design.md): compiled out without ARMLET_NATIVE */
#ifdef ARMLET_NATIVE
void armlet_native_init(struct armlet_ctx *c);
void armlet_native_task_begin(struct armlet_ctx *c);
void armlet_native_reset(void);
#define ARMLET_NATIVE_INIT(c) armlet_native_init(c)
#define ARMLET_NATIVE_TASK_BEGIN(c) armlet_native_task_begin(c)
#define ARMLET_NATIVE_RESET() armlet_native_reset()
#else
#define ARMLET_NATIVE_INIT(c) ((void)0)
#define ARMLET_NATIVE_TASK_BEGIN(c) ((void)0)
#define ARMLET_NATIVE_RESET() ((void)0)
#endif

/* profiler hooks (ARMLET_PROFILE builds only, see docs/profiling.md): nothing in the normal build */
#ifdef ARMLET_PROFILE
#include "prof/prof.h"
void armlet_prof_before(struct armlet_ctx *c);
void armlet_prof_after(struct armlet_ctx *c);
#define ARMLET_PROF_INIT(c) armlet_prof_init(&(c)->rsp, &(c)->prsp)
#define ARMLET_PROF_BEFORE(c) armlet_prof_before(c)
#define ARMLET_PROF_AFTER(c) armlet_prof_after(c)
#define ARMLET_PROF_FWD_BEGIN() armlet_prof_fwd_begin()
#define ARMLET_PROF_FWD_END() armlet_prof_fwd_end()
#define ARMLET_PROF_ROMCLOSED() armlet_prof_rom_closed()
#else
#define ARMLET_PROF_INIT(c) ((void)0)
#define ARMLET_PROF_BEFORE(c) ((void)0)
#define ARMLET_PROF_AFTER(c) ((void)0)
#define ARMLET_PROF_FWD_BEGIN() ((void)0)
#define ARMLET_PROF_FWD_END() ((void)0)
#define ARMLET_PROF_ROMCLOSED() ((void)0)
#endif

#endif
