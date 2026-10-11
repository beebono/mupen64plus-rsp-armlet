/*
 * arMLEt - prof/prof.h: the profiler (ARMLET_PROFILE builds only). See docs/profiling.md.
 *
 *   ARMLET_PROF=1  per task (task type, ucode id): calls, time, time inside the RDP callback, RDP list parse, JIT lookup / compile,
 *                  IMEM invalidations, DMA; optionally a display-list walk (ARMLET_PROF_WALK=1) and a task log (ARMLET_PROF_TASKLOG=1)
 *   ARMLET_PROF=2  + the JIT address map (regions with per-instruction code offsets, VU stubs, thunks) and the code bytes
 *   ARMLET_PROF=3  + dynamic counts per (pc, instruction word) site and control-flow edges (needs the --prof3 build: -DPRSP_PROFILE)
 *   ARMLET_PROF_OUT=<dir> (required), ARMLET_PROF_MARKS=<file>[,<file>...] (a snapshot of all counters when a file appears)
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ARMLET_PROF_H
#define ARMLET_PROF_H

#include <stdint.h>

#include "m64p_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

/* the same values as ARMLET_FAM_* in gfx_ucode.h (checked in prof_glue.c) */
enum { ARMLET_PROF_FAM_OTHER, ARMLET_PROF_FAM_F3D, ARMLET_PROF_FAM_F3DEX, ARMLET_PROF_FAM_F3DEX2 };

struct armlet_prof_task {
    uint32_t type;                      /* OSTask type; 0xffff for a bare RSP program (boot size > 0x1000) */
    uint32_t flags, ucode, ucode_size, ucode_data, ucode_data_size, data_ptr, data_size, sp_pc;
    uint64_t id;                        /* ucode id (gfx_ucode.h); 0 for a non-task */
    unsigned family;                    /* ARMLET_FAM_* */
    const char *name;
};

/* orig: the core's RSP_INFO; prsp: the copy for parallel-rsp, whose ProcessRdpList / CheckInterrupts are replaced */
void armlet_prof_init(const RSP_INFO *orig, RSP_INFO *prsp);
int armlet_prof_level(void);
void armlet_prof_task_begin(const struct armlet_prof_task *t);
void armlet_prof_task_end(void);
void armlet_prof_fwd_begin(void);
void armlet_prof_fwd_end(void);
void armlet_prof_rom_closed(void);

#ifdef __cplusplus
}
#endif
#endif
