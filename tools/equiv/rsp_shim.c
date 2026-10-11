/*
 * Test-only RSP shim, after mupen64plus-rdp-foir's tools/prsp/rspt.c: loads the RSP plugin named by $RSPT_TARGET, forwards the
 * plugin API to it and
 *   RSPT_HASH=<file>  appends one line per DoRspCycles call: FNV-1 of DMEM + IMEM and the return value
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
static FILE *lg, *hl;
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

EXPORT unsigned int CALL DoRspCycles(unsigned int c)
{
    struct stat st;
    double a = now(CLOCK_MONOTONIC);
    unsigned r = p_do(c);
    double b = now(CLOCK_MONOTONIC);

    cum_ms += (b - a) * 1e3;
    calls++;
    if (hl && rinfo.DMEM) {
        unsigned long long x = fnv(rinfo.DMEM, 4096, 0xcbf29ce484222325ull);
        x = fnv(rinfo.IMEM, 4096, x);
        x = (x ^ r) * 0x100000001b3ull;
        fprintf(hl, "%llx\n", x);
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
