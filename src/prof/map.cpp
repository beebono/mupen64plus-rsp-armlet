/*
 * arMLEt - prof/map.cpp: level 2, the JIT address map
 *
 * <out>/map     R <n> <addr> <size> <pc_word> <count> <hash> <bin offset>   one line per compiled region
 *               O <n> <byte offset of instruction 0> ... <of instruction count>   (count + 1 offsets; the last starts the end-of-block /
 *                                                                                  delay-slot / slow-path tail)
 *               W <n> <instruction word 0> ...
 *               S <addr> <size> <name> <bin offset>   VU stubs      T <addr> <size> thunks <bin offset>
 * <out>/jit.bin the code bytes (derived from the games' microcode: never commit)
 * Addresses are the process' own, so they match perf's sample addresses of the same run.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "prof_internal.h"

namespace prof
{
static FILE *mapf, *binf;
static unsigned nregion;
static uint64_t binoff;

static uint64_t put_bin(const void *p, size_t n)
{
    uint64_t o = binoff;
    if (binf)
        fwrite(p, 1, n, binf);
    binoff += n;
    return o;
}

static void h_region(const PrspRegionInfo *r)
{
    if (!mapf)
        return;
    unsigned n = nregion++;
    uint64_t o = put_bin(r->code, r->size);
    fprintf(mapf, "R %u %p %zu %u %u %016llx %llu\nO %u", n, r->code, r->size, r->pc_word, r->count, (unsigned long long)r->hash,
            (unsigned long long)o, n);
    for (unsigned i = 0; i <= r->count; i++)
        fprintf(mapf, " %u", r->offs[i]);
    fprintf(mapf, "\nW %u", n);
    for (unsigned i = 0; i < r->count; i++)
        fprintf(mapf, " %08x", r->instrs[i]);
    fprintf(mapf, "\n");
    fflush(mapf);
    if (binf)
        fflush(binf);
}

static void h_misc(char kind, const void *addr, const void *bytes, size_t size, const char *name)
{
    if (!mapf)
        return;
    uint64_t o = put_bin(bytes, size);
    fprintf(mapf, "%c %p %zu %s %llu\n", kind, addr, size, name, (unsigned long long)o);
    fflush(mapf);
}

void map_install(PrspHooks &h)
{
    mapf = open_out("map", "w");
    binf = open_out("jit.bin", "wb");
    h.region = h_region;
    h.misc = h_misc;
}
}  // namespace prof
