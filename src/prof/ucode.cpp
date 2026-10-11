/*
 * arMLEt - prof/ucode.cpp: the microcode of a key as the profiler's offline tools need it (<out>/ucode/<id>.*), written once, after the
 * third fresh task of a graphics ucode: text (16 KiB of RDRAM from TASK_UCODE: overlays follow the 4 KiB text), data (TASK_UCODE_DATA),
 * dmem and imem as the RSP left them. Ucode text and data come from the ROM: never commit them.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include <sys/stat.h>

#include "prof_internal.h"

namespace prof
{
static void dump(const char *name, const uint8_t *p, size_t n)
{
    if (FILE *f = open_out(name, "wb")) {
        fwrite(p, 1, n, f);
        fclose(f);
    }
}

// RDRAM words are host-order words of the big-endian stream: write the bytes in memory order (what a host tool reading the RSP's
// words as big-endian needs is each 32-bit word byte-swapped; the python tools do that)
static void dump_rdram(const char *name, uint32_t addr, size_t n)
{
    static uint8_t buf[0x4000];
    if (n > sizeof(buf))
        n = sizeof(buf);
    for (size_t i = 0; i < n; i += 4) {
        uint32_t a = (addr + i) & 0x7ffffc;
        memcpy(buf + i, orig.RDRAM + a, 4);
    }
    dump(name, buf, n);
}

void ucode_dump(Key &k, const armlet_prof_task &t)
{
    char p[640];
    snprintf(p, sizeof(p), "%s/ucode", outdir);
    mkdir(p, 0755);
    char n[96];
    snprintf(n, sizeof(n), "ucode/%016llx.text", (unsigned long long)k.id);
    dump_rdram(n, t.ucode, 0x4000);
    snprintf(n, sizeof(n), "ucode/%016llx.data", (unsigned long long)k.id);
    dump_rdram(n, t.ucode_data, t.ucode_data_size > 0x1000 ? 0x1000 : t.ucode_data_size);
    snprintf(n, sizeof(n), "ucode/%016llx.dmem", (unsigned long long)k.id);
    dump(n, orig.DMEM, 0x1000);
    snprintf(n, sizeof(n), "ucode/%016llx.imem", (unsigned long long)k.id);
    dump(n, orig.IMEM, 0x1000);
}
}  // namespace prof
