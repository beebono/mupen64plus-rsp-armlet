/*
 * Test-only RSP shim, after mupen64plus-rdp-foir's tools/prsp/rspt.c: loads the RSP plugin named by $RSPT_TARGET, forwards the
 * plugin API to it and
 *   RSPT_HASH=<file>  appends one line per DoRspCycles call: FNV-1 of DMEM + IMEM and the return value
 *   RSPT_STATE=<file> appends one line per DoRspCycles call with a hash of what the core can see of the RSP and the RDP afterwards: SP_PC, SP_STATUS,
 *                     SP_SEMAPHORE, MI_INTR, DPC_START / END / CURRENT / STATUS (and, with RSPT_RDRAM=1, all of RDRAM: use it with FOIR_THREADS=0, the
 *                     workers of the video plugin write RDRAM asynchronously)
 *   RSPT_MASKS="<ucode id>:<lo>-<hi>,<lo>-<hi>;..."  DMEM byte ranges (hex) that are zeroed before the hash of a graphics task of that ucode: the DMEM that the task-level native path
 *                     declares dead (docs/design.md, "DMEM masks"); the reference runs use the same list, so the hash streams stay comparable. The ucode is identified like
 *                     src/gfx_ucode.c does (FNV-1a over the text words). armlet-native-replay --masks prints the list.
 *   RSPT_LOG=<file>   about once a second "<CLOCK_BOOTTIME s> <calls> <cumulative RSP ms>"
 *   RSPT_MARK=<file>[,<file>...]  at the first call after each file exists (a FOIR_SHOT screenshot, written on the emulation thread,
 *                     so the point is the same for every build): "MARK <calls> <RSP ms> <emu-thread CPU ms> <process CPU ms>" to RSPT_LOG,
 *                     one line per file, in list order (a window is the difference of two lines)
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "m64p_plugin.h"
#include "m64p_types.h"

static void *h;
static FILE *lg, *hl, *sl;
static int hash_rdram;
#define MAX_MASK_UCODES 64
#define MAX_MASK_RANGES 64
static struct {
    unsigned long long id;
    unsigned n, lo[MAX_MASK_RANGES], hi[MAX_MASK_RANGES];
} masks[MAX_MASK_UCODES];
static unsigned n_masks;
static int sticky[MAX_MASK_UCODES];     /* masks stay in force once a graphics task of their ucode has run: the dead bytes it left in DMEM stay there for the tasks after it */
static double cum_ms, last_log;
static unsigned long calls;
#define MAX_MARKS 16
static char *mark_files[MAX_MARKS];
static int n_marks, next_mark;
static RSP_INFO rinfo;
static m64p_error (*p_start)(m64p_dynlib_handle, void *, void (*)(void *, int, const char *));
static m64p_error (*p_stop)(void);
static m64p_error (*p_ver)(m64p_plugin_type *, int *, int *, const char **, int *);
static unsigned int (*p_do)(unsigned int);
static void (*p_init)(RSP_INFO, unsigned int *);
static void (*p_rc)(void);

static unsigned long long fnv(const unsigned char *d, unsigned n, unsigned long long x)
{
    unsigned i;
    for (i = 0; i < n; i++)
        x = (x ^ d[i]) * 0x100000001b3ull;   /* FNV-1a, like rspt.c */
    return x;
}

static unsigned long long fnv64(const unsigned char *d, unsigned long n, unsigned long long x)
{
    unsigned long i;
    for (i = 0; i + 8 <= n; i += 8) {
        unsigned long long w;
        memcpy(&w, d + i, 8);
        x = (x ^ w) * 0x100000001b3ull;
        x ^= x >> 29;
    }
    return x;
}

static double now(clockid_t c)
{
    struct timespec t;
    clock_gettime(c, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle ch, void *ctx, void (*cb)(void *, int, const char *))
{
    const char *t = getenv("RSPT_TARGET"), *l = getenv("RSPT_LOG"), *hf = getenv("RSPT_HASH");
    h = t ? dlopen(t, RTLD_NOW | RTLD_LOCAL) : NULL;
    if (!h)
        return M64ERR_INPUT_NOT_FOUND;
    p_start = dlsym(h, "PluginStartup");
    p_stop = dlsym(h, "PluginShutdown");
    p_ver = dlsym(h, "PluginGetVersion");
    p_do = dlsym(h, "DoRspCycles");
    p_init = dlsym(h, "InitiateRSP");
    p_rc = dlsym(h, "RomClosed");
    if (l)
        lg = fopen(l, "w");
    if (hf)
        hl = fopen(hf, "w");
    if (getenv("RSPT_STATE"))
        sl = fopen(getenv("RSPT_STATE"), "w");
    if (getenv("RSPT_MASKS")) {
        char *m = strdup(getenv("RSPT_MASKS")), *save = NULL, *e;
        for (e = strtok_r(m, ";", &save); e && n_masks < MAX_MASK_UCODES; e = strtok_r(NULL, ";", &save)) {
            char *colon = strchr(e, ':'), *r, *save2 = NULL;
            if (!colon)
                continue;
            masks[n_masks].id = strtoull(e, NULL, 16);
            for (r = strtok_r(colon + 1, ",", &save2); r && masks[n_masks].n < MAX_MASK_RANGES; r = strtok_r(NULL, ",", &save2)) {
                unsigned lo, hi;
                if (sscanf(r, "%x-%x", &lo, &hi) == 2) {
                    masks[n_masks].lo[masks[n_masks].n] = lo;
                    masks[n_masks].hi[masks[n_masks].n++] = hi;
                }
            }
            n_masks++;
        }
    }
    hash_rdram = getenv("RSPT_RDRAM") && *getenv("RSPT_RDRAM") == '1';
    if (getenv("RSPT_MARK")) {
        char *m = strdup(getenv("RSPT_MARK")), *save = NULL, *p;
        for (p = strtok_r(m, ",", &save); p && n_marks < MAX_MARKS; p = strtok_r(NULL, ",", &save))
            mark_files[n_marks++] = p;
    }
    return p_start(ch, ctx, cb);
}

EXPORT m64p_error CALL PluginShutdown(void)
{
    m64p_error e = p_stop();
    if (lg) { fclose(lg); lg = NULL; }
    if (hl) { fclose(hl); hl = NULL; }
    if (sl) { fclose(sl); sl = NULL; }
    return e;
}

EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type *t, int *v, int *a, const char **n, int *c)
{
    if (!p_ver) {
        if (t) *t = M64PLUGIN_RSP;
        if (a) *a = 0x020000;
        return M64ERR_SUCCESS;
    }
    return p_ver(t, v, a, n, c);
}

/* a graphics task (OSTask type 1): identify its ucode as src/gfx_ucode.c does and put the ranges it declares dead in force from this call on. The OSTask header is read before the task runs: the
 * microcode may use it as scratch (Indiana Jones's does; ARMLET_DMEM_POISON=fc0-fd0 changes it), and then the task would not be recognised afterwards. */
static void mask_identify(void)
{
    if (n_masks && rinfo.DMEM && *(const unsigned *)(rinfo.DMEM + 0xfc0) == 1 && *(const unsigned *)(rinfo.DMEM + 0xfcc) <= 0x1000) {
        unsigned addr = *(const unsigned *)(rinfo.DMEM + 0xfd0) & 0x7fffff & ~3u, size = *(const unsigned *)(rinfo.DMEM + 0xfd4), i, k;
        unsigned long long id = 0xcbf29ce484222325ull;
        if (!size)
            size = 0xf80;
        if (size > 0x1000)
            size = 0x1000;
        size = (size + 3) & ~3u;
        for (i = 0; i < size; i += 4)
            id = (id ^ *(const unsigned *)(rinfo.RDRAM + ((addr + i) & 0x7ffffc))) * 0x100000001b3ull;
        for (k = 0; k < n_masks; k++)
            if (masks[k].id == id)
                sticky[k] = 1;
    }
}

EXPORT unsigned int CALL DoRspCycles(unsigned int c)
{
    struct stat st;
    double a;
    unsigned r;
    if (hl)
        mask_identify();
    a = now(CLOCK_MONOTONIC);
    r = p_do(c);
    double b = now(CLOCK_MONOTONIC);

    cum_ms += (b - a) * 1e3;
    calls++;
    if (getenv("RSPT_DUMP") && rinfo.DMEM) {       /* development: RSPT_DUMP=<prefix> RSPT_DUMP_CALLS=<first>-<last>: DMEM and IMEM after those calls (1-based) */
        unsigned lo = 0, hi = 0;
        if (getenv("RSPT_DUMP_CALLS"))
            sscanf(getenv("RSPT_DUMP_CALLS"), "%u-%u", &lo, &hi);
        if (calls >= lo && calls <= hi) {
            char nm[512];
            FILE *f;
            snprintf(nm, sizeof(nm), "%s.%lu.bin", getenv("RSPT_DUMP"), calls);
            f = fopen(nm, "wb");
            if (f) {
                fwrite(rinfo.DMEM, 1, 4096, f);
                fwrite(rinfo.IMEM, 1, 4096, f);
                if (getenv("RSPT_DUMP_RDRAM"))
                    fwrite(rinfo.RDRAM, 1, 0x800000, f);
                fclose(f);
            }
        }
    }
    if (hl && rinfo.DMEM) {
        unsigned char dm[4096];
        const unsigned char *dmem = rinfo.DMEM;
        unsigned long long x;
        {
            unsigned k, j, i;
            int any = 0;
            for (k = 0; k < n_masks; k++)
                any |= sticky[k];
            if (any) {
                memcpy(dm, rinfo.DMEM, 4096);
                for (k = 0; k < n_masks; k++)
                    if (sticky[k])
                        for (j = 0; j < masks[k].n; j++)
                            for (i = masks[k].lo[j]; i < masks[k].hi[j] && i < 4096; i++)
                                dm[i ^ 3] = 0;
                dmem = dm;
            }
        }
        x = fnv(dmem, 4096, 0xcbf29ce484222325ull);
        x = fnv(rinfo.IMEM, 4096, x);
        x = (x ^ r) * 0x100000001b3ull;
        fprintf(hl, "%llx\n", x);
    }
    if (sl && rinfo.DMEM) {
        unsigned v[9] = { *rinfo.SP_PC_REG, *rinfo.SP_STATUS_REG, *rinfo.SP_SEMAPHORE_REG, *rinfo.MI_INTR_REG, *rinfo.DPC_START_REG,
                          *rinfo.DPC_END_REG, *rinfo.DPC_CURRENT_REG, *rinfo.DPC_STATUS_REG, r };
        unsigned long long x = fnv((const unsigned char *)v, sizeof(v), 0xcbf29ce484222325ull);
        if (hash_rdram)
            x = fnv64(rinfo.RDRAM, 0x800000, x);
        fprintf(sl, "%llx\n", x);
    }
    if (lg && b - last_log >= 1.0) {
        last_log = b;
        fprintf(lg, "%.3f %lu %.3f\n", now(CLOCK_BOOTTIME), calls, cum_ms);
        fflush(lg);
    }
    while (next_mark < n_marks && !stat(mark_files[next_mark], &st)) {
        if (lg) {
            fprintf(lg, "MARK %lu %.3f %.3f %.3f\n", calls, cum_ms, now(CLOCK_THREAD_CPUTIME_ID) * 1e3, now(CLOCK_PROCESS_CPUTIME_ID) * 1e3);
            fflush(lg);
        }
        next_mark++;
    }
    return r;
}

EXPORT void CALL InitiateRSP(RSP_INFO i, unsigned int *cc)
{
    rinfo = i;
    p_init(i, cc);
}

EXPORT void CALL RomClosed(void)
{
    p_rc();
}
