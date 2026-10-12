/*
 * arMLEt - rdp_defer.h: exact coalescing of the ProcessRdpList calls of a graphics ucode (docs/design.md)
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ARMLET_RDP_DEFER_H
#define ARMLET_RDP_DEFER_H

#include "armlet.h"

/* Installs the deferring callback in ctx->prsp (the RSP_INFO copy parallel-rsp gets); call after the profiler's wrapping, before
 * parallelRSPInitiateRSP. Does nothing when the option resolved to off. */
void armlet_rdp_defer_init(struct armlet_ctx *c);
/* Before a task is forwarded to parallel-rsp: lists are deferred only in the tasks of a known graphics ucode family. */
void armlet_rdp_defer_task_begin(const struct armlet_ctx *c);
/* After it: nothing may be pending (the fork's hook ran the rest before the interrupts were checked); runs it if something is. */
void armlet_rdp_defer_task_end(void);
void armlet_rdp_defer_reset(void);

#endif
