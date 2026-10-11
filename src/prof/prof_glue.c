/*
 * arMLEt - prof/prof_glue.c: the C side of the profiler hooks in plugin.c / hle_glue.c
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "armlet.h"
#include "gfx_ucode.h"
#include "prof.h"

_Static_assert((int)ARMLET_FAM_F3D == (int)ARMLET_PROF_FAM_F3D && (int)ARMLET_FAM_F3DEX == (int)ARMLET_PROF_FAM_F3DEX && (int)ARMLET_FAM_F3DEX2 == (int)ARMLET_PROF_FAM_F3DEX2,
               "family values");

void armlet_prof_before(struct armlet_ctx *c)
{
    struct armlet_prof_task t;
    const struct armlet_ucode_id *u;
    bool is_new;

    if (!armlet_prof_level())
        return;
    t.sp_pc = *c->rsp.SP_PC_REG & 0xfff;
    if (armlet_dmem32(c, ARMLET_TASK_UCODE_BOOT_SIZE) > 0x1000) {
        t.type = 0xffff;
        t.flags = t.ucode = t.ucode_size = t.ucode_data = t.ucode_data_size = t.data_ptr = t.data_size = 0;
        t.id = 0;
        t.family = ARMLET_FAM_OTHER;
        t.name = "";
    } else {
        t.type = armlet_dmem32(c, ARMLET_TASK_TYPE);
        t.flags = armlet_dmem32(c, ARMLET_TASK_FLAGS);
        t.ucode = armlet_dmem32(c, ARMLET_TASK_UCODE);
        t.ucode_size = armlet_dmem32(c, ARMLET_TASK_UCODE_SIZE);
        t.ucode_data = armlet_dmem32(c, ARMLET_TASK_UCODE_DATA);
        t.ucode_data_size = armlet_dmem32(c, ARMLET_TASK_UCODE_DATA_SIZE);
        t.data_ptr = armlet_dmem32(c, ARMLET_TASK_DATA_PTR);
        t.data_size = armlet_dmem32(c, ARMLET_TASK_DATA_SIZE);
        u = armlet_gfx_identify(c, &is_new);
        t.id = u->id;
        t.family = u->family;
        t.name = u->name;
    }
    armlet_prof_task_begin(&t);
}

void armlet_prof_after(struct armlet_ctx *c)
{
    (void)c;
    if (armlet_prof_level())
        armlet_prof_task_end();
}
