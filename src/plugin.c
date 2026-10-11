/*
 * arMLEt (ARM-focused Mid-Level Emulation) RSP plugin - plugin.c
 *
 * rsp-hle runs the audio, JPEG, video and boot ucodes in HLE; every graphics task (and any ucode it does not know) is handed to
 * parallel-rsp's JIT through HleForwardTask() in hle_glue.c, where native graphics handlers will hook in. With
 * DisplayListToGraphicsPlugin and AudioListToAudioPlugin off this is rsp-hle with RspFallback = parallel-rsp in one module.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>

#define M64P_PLUGIN_PROTOTYPES 1
#include "m64p_common.h"
#include "m64p_plugin.h"
#include "m64p_types.h"

#include "armlet.h"
#include "config.h"
#include "gfx_ucode.h"
#include "hle.h"
#include "prsp_bridge.h"

#define PLUGIN_VERSION 0x000100
#define PLUGIN_API_VERSION 0x020000

static struct armlet_ctx l_ctx;
static int l_init;

EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle core, void *context, void (*debug_cb)(void *, int, const char *))
{
    m64p_error e;

    if (l_init)
        return M64ERR_ALREADY_INIT;
    armlet_log_set(debug_cb, context);
    e = armlet_config_startup(core);
    if (e != M64ERR_SUCCESS)
        return e;
    l_init = 1;
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginShutdown(void)
{
    if (!l_init)
        return M64ERR_NOT_INIT;
    armlet_config_shutdown();
    armlet_log_set(NULL, NULL);
    l_init = 0;
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type *type, int *version, int *api, const char **name, int *caps)
{
    if (type)
        *type = M64PLUGIN_RSP;
    if (version)
        *version = PLUGIN_VERSION;
    if (api)
        *api = PLUGIN_API_VERSION;
    if (name)
        *name = "arMLEt (mupen64plus-rsp-armlet)";
    if (caps)
        *caps = 0;
    return M64ERR_SUCCESS;
}

EXPORT unsigned int CALL DoRspCycles(unsigned int cycles)
{
    ARMLET_PROF_BEFORE(&l_ctx);
    hle_execute(&l_ctx.hle);
    ARMLET_PROF_AFTER(&l_ctx);
    armlet_log_new_handler(&l_ctx);
    return cycles;
}

EXPORT void CALL InitiateRSP(RSP_INFO info, unsigned int *cycle_count)
{
    struct armlet_ctx *c = &l_ctx;

    c->rsp = info;
    hle_init(&c->hle, info.RDRAM, info.DMEM, info.IMEM, info.MI_INTR_REG, info.SP_MEM_ADDR_REG, info.SP_DRAM_ADDR_REG,
             info.SP_RD_LEN_REG, info.SP_WR_LEN_REG, info.SP_STATUS_REG, info.SP_DMA_FULL_REG, info.SP_DMA_BUSY_REG,
             info.SP_PC_REG, info.SP_SEMAPHORE_REG, info.DPC_START_REG, info.DPC_END_REG, info.DPC_CURRENT_REG,
             info.DPC_STATUS_REG, info.DPC_CLOCK_REG, info.DPC_BUFBUSY_REG, info.DPC_PIPEBUSY_REG, info.DPC_TMEM_REG, c);
    /* graphics and audio lists never go to the core's plugins: rsp-hle's own audio, parallel-rsp for graphics */
    c->hle.hle_gfx = 0;
    c->hle.hle_aud = 0;

    armlet_config_load(&c->cfg);
    if (c->cfg.gfx_mode == ARMLET_GFX_HLE)
        armlet_log(M64MSG_INFO, "GfxMode=hle: this build has no native graphics handlers yet, graphics run on parallel-rsp");

    c->prsp = info;
    ARMLET_PROF_INIT(c);
    parallelRSPInitiateRSP(c->prsp, cycle_count);
}

EXPORT void CALL RomClosed(void)
{
    l_ctx.hle.cached_ucodes.count = 0;
    l_ctx.cached_seen = 0;
    armlet_gfx_reset();
    ARMLET_PROF_ROMCLOSED();
    parallelRSPRomClosed();
}
