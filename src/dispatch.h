/*
 * arMLEt - dispatch.h
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ARMLET_DISPATCH_H
#define ARMLET_DISPATCH_H

#include "armlet.h"

/* Called by rsp-hle for every task it does not handle itself; true when a native handler completed the task. */
bool armlet_gfx_try_native(struct armlet_ctx *c);

#endif
