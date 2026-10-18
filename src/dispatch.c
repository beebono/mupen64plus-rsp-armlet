/*
 * arMLEt - dispatch.c: decides whether a graphics task runs natively or on parallel-rsp's JIT
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <inttypes.h>

#include "dispatch.h"
#include "gfx_ucode.h"
#include "hle/probe.h"
#include "prsp_bridge.h"
#include "ustats.h"

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
    if (armlet_dmem32(c, ARMLET_TASK_FLAGS) & 1)
        return false;
    if ((*c->rsp.SP_PC_REG & 0xfff) != 0) {
        /* a call that continues a task, after the JIT's idle-poll cut (PRSP_POLL_IDLE: a task that waits for the CPU, Gauntlet Legends): a family that can take it up answers (hle_core.c, wait_pc); no probe,
         * no counting of tasks */
#ifdef ARMLET_NATIVE
        if (prsp_poll_idle_limit() <= 0)
            return false;
        u = armlet_gfx_identify(c, &is_new);
        n = armlet_gfx_native_find(u->id);
        return n && armlet_gfx_allowed(&c->cfg, u->id) && n->run(c, u) != 0;
#else
        return false;
#endif
    }

    u = armlet_gfx_identify(c, &is_new);
    n = armlet_gfx_native_find(u->id);
    if (c->cfg.probe) {
        /* GfxProbe: a ucode that has no handler is derived and probed (src/hle/probe.c); a derived build runs once the validator latched it, for the data image it was derived for,
         * and the tasks the validator samples run on the JIT */
        armlet_probe_offer(c, u);
        if (!armlet_probe_gate(c, u))
            n = NULL;
        else if (!n)
            n = armlet_gfx_native_find(u->id);
    }
    if (is_new)
        armlet_log(M64MSG_INFO, "gfx ucode %016" PRIx64 " %s: %s [%s]", u->id, l_family[u->family], u->name[0] ? u->name : "(no banner)",
                   !n ? "no native handler, LLE" : armlet_gfx_allowed(&c->cfg, u->id) ? "native" : "not in GfxHleUcodes, LLE");
    armlet_ustats_offer(c, u, n != 0);
    if (!n || !armlet_gfx_allowed(&c->cfg, u->id))
        return false;
    if (armlet_ustats_on) {
        uint64_t t0 = armlet_ustats_now();
        bool ok = n->run(c, u) != 0;
        armlet_ustats_result(ok, armlet_ustats_now() - t0);
        if (!ok && c->cfg.probe)
            armlet_probe_declined(c, u);
        return ok;
    }
    if (n->run(c, u) != 0)
        return true;
    if (c->cfg.probe)
        armlet_probe_declined(c, u);
    return false;
}
