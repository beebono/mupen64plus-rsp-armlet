/*
 * arMLEt - prof/rdp.cpp: counts the RDP commands of a list the ucode submits (CMD_END write -> ProcessRdpList)
 *
 * The list is [DPC_CURRENT, DPC_END) in RDRAM, or in DMEM when DPC_STATUS says XBUS. Command lengths follow from the opcode, so the walk is
 * a dependent chain over a table; there is no useful NEON form (the profiler's cost is in the clock reads and the call, not here).
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "prof_internal.h"

namespace prof
{
// bytes per RDP command by opcode (word 0 bits 29..24); triangles 8..15, texture rectangles 0x24 / 0x25 are longer than the default 8
static const uint8_t cmd_len[64] = {
    8, 8, 8, 8, 8, 8, 8, 8,  32, 48, 96, 112, 96, 112, 160, 176,  8, 8, 8, 8, 8, 8, 8, 8,  8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 16, 16, 8, 8,  8, 8, 8, 8, 8, 8, 8, 8,  8, 8, 8, 8, 8, 8, 8, 8,  8, 8, 8, 8, 8, 8, 8, 8};

const unsigned cbh_limit[CBH_BINS - 1] = {8, 16, 32, 64, 128, 256, 512, 1024};

RdpList rdp_parse(Key &k)
{
    const bool xbus = (*orig.DPC_STATUS_REG & 1) != 0;
    const uint32_t mask = xbus ? 0xfff : 0x7fffff;
    uint32_t cur = *orig.DPC_CURRENT_REG & mask, end = *orig.DPC_END_REG & mask;
    if (end <= cur)
        return RdpList{0, 0, false};
    k.c[C_CMDEND]++;
    if (xbus)
        k.c[C_RDP_XBUS]++;
    k.c[C_RDP_BYTES] += end - cur;
    const uint32_t *base = reinterpret_cast<const uint32_t *>(xbus ? orig.DMEM : orig.RDRAM);
    uint64_t cmds = 0;
    unsigned tris = 0;
    bool sync = false;
    const unsigned bytes = end - cur;
    while (cur < end) {
        unsigned op = (base[cur >> 2] >> 24) & 0x3f;
        k.rdp_ops[op]++;
        tris += (op >> 3) == 1;
        sync |= op == 0x29;
        cmds++;
        cur += cmd_len[op];
    }
    k.c[C_RDP_CMDS] += cmds;
    return RdpList{bytes, tris, sync};
}

// the per-class counters are sums over the histogram, refreshed when a task ends and at every snapshot
void rdp_derive(Key &k)
{
    const uint64_t *o = k.rdp_ops;
    uint64_t tri = 0, sh = 0, tx = 0, z = 0;
    for (unsigned op = 8; op < 16; op++) {
        tri += o[op];
        if (op & 4) sh += o[op];
        if (op & 2) tx += o[op];
        if (op & 1) z += o[op];
    }
    k.c[C_RDP_TRI] = tri;
    k.c[C_RDP_TRI_SH] = sh;
    k.c[C_RDP_TRI_TX] = tx;
    k.c[C_RDP_TRI_Z] = z;
    k.c[C_RDP_RECT] = o[0x36];
    k.c[C_RDP_TEXRECT] = o[0x24] + o[0x25];
    k.c[C_RDP_LOADS] = o[0x30] + o[0x33] + o[0x34];
    k.c[C_RDP_SETTILE] = o[0x35];
    k.c[C_RDP_SYNC] = o[0x26] + o[0x27] + o[0x28] + o[0x29];
}
}  // namespace prof
