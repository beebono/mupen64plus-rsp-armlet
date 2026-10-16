/*
 * arMLEt - hle_glue.c: the hooks rsp-hle's core expects (hle_external.h), and the single point where a graphics task
 * leaves rsp-hle. With hle_gfx = hle_aud = 0 every graphics task and every unknown ucode ends in HleForwardTask().
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdarg.h>
#include <stdio.h>

#include "m64p_types.h"

#include "armlet.h"
#include "dispatch.h"
#include "ustats.h"
#include "hle/hle.h"
#include "prsp_bridge.h"
#include "rdp_defer.h"
#include "hle_external.h"
#include "ucodes.h"

#define MSG(name, level) \
    void name(void *user_defined, const char *message, ...) \
    { \
        va_list ap; \
        va_start(ap, message); \
        armlet_vlog_raw(level, message, ap); \
        va_end(ap); \
    }

MSG(HleVerboseMessage, M64MSG_VERBOSE)
MSG(HleInfoMessage, M64MSG_INFO)
MSG(HleErrorMessage, M64MSG_ERROR)
MSG(HleWarnMessage, M64MSG_WARNING)

#define CTX(u) ((struct armlet_ctx *)(u))

void HleCheckInterrupts(void *user_defined)
{
    if (CTX(user_defined)->rsp.CheckInterrupts)
        CTX(user_defined)->rsp.CheckInterrupts();
}

void HleProcessDlistList(void *user_defined)
{
    if (CTX(user_defined)->rsp.ProcessDlistList)
        CTX(user_defined)->rsp.ProcessDlistList();
}

void HleProcessAlistList(void *user_defined)
{
    if (CTX(user_defined)->rsp.ProcessAlistList)
        CTX(user_defined)->rsp.ProcessAlistList();
}

void HleProcessRdpList(void *user_defined)
{
    if (CTX(user_defined)->rsp.ProcessRdpList)
        CTX(user_defined)->rsp.ProcessRdpList();
}

void HleShowCFB(void *user_defined)
{
    if (CTX(user_defined)->rsp.ShowCFB)
        CTX(user_defined)->rsp.ShowCFB();
}

int HleForwardTask(void *user_defined)
{
    if (armlet_gfx_try_native(CTX(user_defined)))
        return 0;
    if (armlet_hle_task_begin(CTX(user_defined)))
        return 0;
    ARMLET_NATIVE_TASK_BEGIN(CTX(user_defined));
    armlet_rdp_defer_task_begin(CTX(user_defined));
    ARMLET_PROF_FWD_BEGIN();
    if (armlet_ustats_on) {
        uint64_t t0 = armlet_ustats_now();
        parallelRSPDoRspCycles(~0u);
        armlet_ustats_jit(armlet_ustats_now() - t0);
    } else {
        parallelRSPDoRspCycles(~0u);
    }
    armlet_rdp_defer_task_end();
    ARMLET_PROF_FWD_END();
    armlet_hle_task_end(CTX(user_defined));
    return 0;
}

/* Which rsp-hle handler a ucode got, logged once per new cache entry (gate 4 wants to know which audio paths a game used). */
static const struct { ucode_func_t fn; const char *name; } l_handlers[] = {
    { cicx105_ucode, "cic x105 boot" },
    { alist_process_audio, "audio ABI1" },
    { alist_process_audio_ge, "audio ABI1 (GoldenEye)" },
    { alist_process_audio_bc, "audio ABI1 (Blast Corps, Diddy Kong Racing)" },
    { alist_process_naudio, "naudio" },
    { alist_process_naudio_bk, "naudio (Banjo-Kazooie)" },
    { alist_process_naudio_dk, "naudio (Donkey Kong 64)" },
    { alist_process_naudio_mp3, "naudio (mp3: Banjo-Tooie, Jet Force Gemini, Perfect Dark)" },
    { alist_process_naudio_cbfd, "naudio (Conker)" },
    { alist_process_nead_mk, "nead (Mario Kart 64, Wave Race 64 E)" },
    { alist_process_nead_sfj, "nead (Star Fox J)" },
    { alist_process_nead_sf, "nead (Star Fox 64 / Lylat Wars)" },
    { alist_process_nead_fz, "nead (F-Zero X)" },
    { alist_process_nead_wrjb, "nead (Wave Race 64 J RevB)" },
    { alist_process_nead_ys, "nead (Yoshi's Story)" },
    { alist_process_nead_1080, "nead (1080 Snowboarding)" },
    { alist_process_nead_oot, "nead (Ocarina of Time, Majora's Mask J)" },
    { alist_process_nead_mm, "nead (Majora's Mask, Pokemon Stadium 2)" },
    { alist_process_nead_mmb, "nead (Majora's Mask E beta)" },
    { alist_process_nead_ac, "nead (Animal Crossing)" },
    { alist_process_nead_mats, "nead (Mario Artist Talent Studio; forwarded to parallel-rsp)" },
    { alist_process_nead_efz, "nead (F-Zero X Expansion; forwarded to parallel-rsp)" },
    { musyx_v1_task, "MusyX v1" },
    { musyx_v2_task, "MusyX v2" },
    { jpeg_decode_PS0, "JPEG (Pokemon Stadium J)" },
    { jpeg_decode_PS, "JPEG (Ocarina of Time, Pokemon Stadium)" },
    { jpeg_decode_OB, "JPEG (Ogre Battle, Bottom of the 9th)" },
    { resize_bilinear_task, "Resident Evil 2 (resize)" },
    { decode_video_frame_task, "Resident Evil 2 (video)" },
    { fill_video_double_buffer_task, "Resident Evil 2 (fill)" },
    { hvqm2_decode_sp1_task, "HVQM2 sp1" },
    { hvqm2_decode_sp2_task, "HVQM2 sp2" },
};

void armlet_log_new_handler(struct armlet_ctx *c)
{
    const struct cached_ucodes_t *cu = &c->hle.cached_ucodes;
    const struct ucode_info_t *info;
    const char *name = "rsp-hle internal (task done / forwarded to parallel-rsp)";
    unsigned i;

    if (cu->count == c->cached_seen)
        return;
    c->cached_seen = cu->count;
    info = &cu->infos[cu->count - 1];
    for (i = 0; i < sizeof(l_handlers) / sizeof(l_handlers[0]); i++)
        if (l_handlers[i].fn == info->uc_pfunc) {
            name = l_handlers[i].name;
            break;
        }
    armlet_log(M64MSG_INFO, "ucode %08x data %08x/%x -> %s", info->uc_start, info->uc_dstart, info->uc_dsize, name);
}
