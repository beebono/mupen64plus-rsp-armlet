/*
 * The parallel-rsp entry points (parallel.cpp, extern "C"), linked into this module. The fork's own plugin entry
 * (m64p_plugin_entry.cpp) is not used: armlet owns the Mupen64Plus plugin API.
 */
#ifndef ARMLET_PRSP_BRIDGE_H
#define ARMLET_PRSP_BRIDGE_H

#include "m64p_plugin.h"

unsigned int parallelRSPDoRspCycles(unsigned int cycles);
void parallelRSPRomClosed(void);
void parallelRSPInitiateRSP(RSP_INFO info, unsigned int *cycle_count);

#endif
