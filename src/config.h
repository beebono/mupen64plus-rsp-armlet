/*
 * arMLEt - config.h: the "Rsp-Armlet" section of the core's configuration
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ARMLET_CONFIG_H
#define ARMLET_CONFIG_H

#include "armlet.h"

#define ARMLET_CONFIG_SECTION "Rsp-Armlet"

m64p_error armlet_config_startup(m64p_dynlib_handle core);
void armlet_config_load(struct armlet_config *cfg);   /* section values, then the ARMLET_* environment overrides */
void armlet_config_shutdown(void);
const char *armlet_config_user_path(void);           /* the core's user config directory (the cache of the probe lives there); "" when unknown */

#endif
