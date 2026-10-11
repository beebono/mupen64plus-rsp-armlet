/*
 * Test-only input plugin for the equivalence runs: a pad script keyed on the number of controller polls, not on time, so two
 * runs of a deterministic emulator (Core[RandomizeInterrupt]=False) see the same input at the same emulated moment.
 * INP_DET_IDLE=1 gives an idle pad. No pak (a pak prompt would depend on saves).
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define M64P_PLUGIN_PROTOTYPES 1
#include <stdlib.h>

#include "m64p_common.h"
#include "m64p_plugin.h"
#include "m64p_types.h"

static CONTROL *l_ctl;
static unsigned l_polls;
static int l_idle;

EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle h, void *c, void (*d)(void *, int, const char *))
{
    l_idle = getenv("INP_DET_IDLE") != NULL;
    return M64ERR_SUCCESS;
}
EXPORT m64p_error CALL PluginShutdown(void) { return M64ERR_SUCCESS; }
EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type *t, int *v, int *a, const char **name, int *caps)
{
    if (t) *t = M64PLUGIN_INPUT;
    if (v) *v = 0x010000;
    if (a) *a = 0x020100;
    if (name) *name = "deterministic scripted input (test)";
    if (caps) *caps = 0;
    return M64ERR_SUCCESS;
}
EXPORT int CALL RomOpen(void) { return 1; }
EXPORT void CALL RomClosed(void) {}
EXPORT void CALL InitiateControllers(CONTROL_INFO ci)
{
    l_ctl = ci.Controls;
    l_ctl[0].Present = 1;
    l_ctl[0].RawData = 0;
    l_ctl[0].Plugin = PLUGIN_NONE;
}

/* polls 0-359 idle (boot); to 1559 START and A pulses, 45 polls apart, alternating (title screens, menus, prompts);
 * after that the stick held up with A, B and Z taps (walk around, jump, attack) */
EXPORT void CALL GetKeys(int c, BUTTONS *k)
{
    unsigned n;
    k->Value = 0;
    if (c != 0 || l_idle)
        return;
    n = l_polls++;
    if (n < 360)
        return;
    if (n < 1560) {
        unsigned ph = (n - 360) % 90;
        if (ph < 3)
            k->START_BUTTON = 1;
        else if (ph >= 45 && ph < 48)
            k->A_BUTTON = 1;
        return;
    }
    k->Y_AXIS = 80;
    if (n % 30 < 3)
        k->A_BUTTON = 1;
    if (n % 180 < 3)
        k->B_BUTTON = 1;
    if (n % 240 >= 120 && n % 240 < 125)
        k->Z_TRIG = 1;
}
EXPORT void CALL ControllerCommand(int c, unsigned char *cmd) {}
EXPORT void CALL ReadController(int c, unsigned char *cmd) {}
EXPORT void CALL SDL_KeyDown(int k, int s) {}
EXPORT void CALL SDL_KeyUp(int k, int s) {}
EXPORT void CALL RenderCallback(void) {}
