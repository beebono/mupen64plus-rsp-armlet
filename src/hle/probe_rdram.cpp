/*
 * arMLEt - probe_rdram.cpp: the probe's private RDRAM (probe.c): the JIT's DMA reads and writes go through RSP::cpu's rdram pointer, which this points at a buffer of its own for the length of a
 * canned task. The fork is not changed: the member is the one parallel.cpp sets at InitiateRSP (set_rdram).
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "m64p_plugin.h"

#include "rsp_jit.hpp"

#include "hle_internal.h"

namespace RSP
{
extern JIT::CPU cpu;
extern RSP_INFO rsp;
}

// The DMA code reads and writes through the CPU state's pointer; the sandbox puts the old bytes back through RSP::rsp.RDRAM (prsp_native_sandbox): both have to name the same buffer.
extern "C" void armlet_hle_set_rdram(uint32_t *rdram)
{
	RSP::cpu.set_rdram(rdram);
	RSP::rsp.RDRAM = reinterpret_cast<unsigned char *>(rdram);
}
