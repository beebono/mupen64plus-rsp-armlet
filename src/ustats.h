/*
 * arMLEt - ustats.h: per-ucode task accounting for the coverage recount (ARMLET_UCODE_STATS=<file>)
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ARMLET_USTATS_H
#define ARMLET_USTATS_H

#include "armlet.h"

struct armlet_ucode_id;

extern bool armlet_ustats_on;
extern uint64_t armlet_ustats_cb_ns;
/* a graphics task of ucode `u` is about to be offered to its native handler (or not); `has_handler`: one exists */
void armlet_ustats_offer(const struct armlet_ctx *c, const struct armlet_ucode_id *u, bool has_handler);
void armlet_ustats_result(bool ok, uint64_t ns);        /* the handler's verdict and the time it took (declined: wasted) */
void armlet_ustats_jit(uint64_t ns);                    /* the JIT ran the task offered last */
uint64_t armlet_ustats_now(void);
void armlet_ustats_flush(void);

#endif
