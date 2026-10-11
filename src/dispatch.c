/*
 * arMLEt - dispatch.c: decides whether a graphics task runs natively or on parallel-rsp's JIT
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <inttypes.h>

#include "dispatch.h"
#include "gfx_ucode.h"

static const char *const l_family[] = { "other", "F3D", "F3DEX", "F3DEX2" };

bool armlet_gfx_try_native(struct armlet_ctx *c)
{
    const struct armlet_ucode_id *u;
    const struct armlet_gfx_native *n;
    bool is_new;

    if (c->cfg.gfx_mode != ARMLET_GFX_HLE)
        return false;
    /* a graphics task: a task at all (not a bare RSP program), type 1, started fresh and not a yield resume */
    if (armlet_dmem32(c, ARMLET_TASK_UCODE_BOOT_SIZE) > 0x1000 || armlet_dmem32(c, ARMLET_TASK_TYPE) != 1)
        return false;
    if ((*c->rsp.SP_PC_REG & 0xfff) != 0 || (armlet_dmem32(c, ARMLET_TASK_FLAGS) & 1))
        return false;

    u = armlet_gfx_identify(c, &is_new);
    n = armlet_gfx_native_find(u->id);
    if (is_new)
        armlet_log(M64MSG_INFO, "gfx ucode %016" PRIx64 " %s: %s [%s]", u->id, l_family[u->family], u->name[0] ? u->name : "(no banner)",
                   !n ? "no native handler, LLE" : armlet_gfx_allowed(&c->cfg, u->id) ? "native" : "not in GfxHleUcodes, LLE");
    if (!n || !armlet_gfx_allowed(&c->cfg, u->id))
        return false;
    return n->run(c, u) != 0;
}
