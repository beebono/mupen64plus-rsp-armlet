/*
 * arMLEt - ustats.c: per-ucode task accounting for the coverage recount (docs/ucode-coverage.md).
 *   ARMLET_UCODE_STATS=<file>: the table is rewritten every 128 graphics tasks and at RomClosed, one line per ucode:
 *   U <id> <text4k> <family> <tasks> <native> <declined> <nohandler> <native_ns> <wasted_ns> <jit_ns> <banner>
 *   R <id> <count> <reason>
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gfx_ucode.h"
#include "ustats.h"

#define MAXU 96
#define MAXR 12

struct reason { char why[96]; uint64_t n; };
struct ent {
    uint64_t id, text4k;
    unsigned family;
    char name[160];
    uint64_t tasks, native, declined, nohandler, native_ns, wasted_ns, jit_ns;
    struct reason r[MAXR];
};

bool armlet_ustats_on;
uint64_t armlet_ustats_cb_ns;           /* time inside the video plugin's callback (rdp_defer.c adds to it), subtracted from the task times */
static struct ent l_e[MAXU];
static unsigned l_n, l_cnt;
static struct ent *l_cur;
static const char *l_path;
static uint64_t l_cb0;

uint64_t armlet_ustats_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#ifdef ARMLET_NATIVE
const char *armlet_hle_last_why(void);
#else
static const char *armlet_hle_last_why(void) { return NULL; }
#endif

void armlet_ustats_offer(const struct armlet_ctx *c, const struct armlet_ucode_id *u, bool has_handler)
{
    unsigned i;
    if (!l_path) {
        const char *e = getenv("ARMLET_UCODE_STATS");
        l_path = e && *e ? e : "";
        armlet_ustats_on = *l_path != 0;
    }
    if (!armlet_ustats_on) {
        l_cur = NULL;
        return;
    }
    for (i = 0; i < l_n; i++)
        if (l_e[i].id == u->id)
            break;
    if (i == l_n) {
        if (l_n == MAXU) {
            l_cur = NULL;
            return;
        }
        l_n++;
        memset(&l_e[i], 0, sizeof(l_e[i]));
        l_e[i].id = u->id;
        l_e[i].family = u->family;
        snprintf(l_e[i].name, sizeof(l_e[i].name), "%s", u->name);
        l_e[i].text4k = armlet_ucode_hash(c->rsp.RDRAM, armlet_dmem32(c, ARMLET_TASK_UCODE) & 0x7fffff & ~3u, 0x1000);
    }
    l_cur = &l_e[i];
    l_cb0 = armlet_ustats_cb_ns;
    l_cur->tasks++;
    if (!has_handler)
        l_cur->nohandler++;
    if (!(++l_cnt & 127))
        armlet_ustats_flush();
}

void armlet_ustats_result(bool ok, uint64_t ns)
{
    struct ent *e = l_cur;
    if (!e)
        return;
    ns = ns > armlet_ustats_cb_ns - l_cb0 ? ns - (armlet_ustats_cb_ns - l_cb0) : 0;     /* without the video plugin's time */
    l_cb0 = armlet_ustats_cb_ns;
    if (ok) {
        e->native++;
        e->native_ns += ns;
    } else {
        unsigned i;
        const char *why = armlet_hle_last_why();
        if (!why)
            why = "(no reason given)";
        e->declined++;
        e->wasted_ns += ns;
        for (i = 0; i < MAXR; i++)
            if (!e->r[i].n || !strcmp(e->r[i].why, why))
                break;
        if (i < MAXR) {
            if (!e->r[i].n)
                snprintf(e->r[i].why, sizeof(e->r[i].why), "%s", why);
            e->r[i].n++;
        }
    }
}

void armlet_ustats_jit(uint64_t ns)
{
    if (l_cur)
        l_cur->jit_ns += ns > armlet_ustats_cb_ns - l_cb0 ? ns - (armlet_ustats_cb_ns - l_cb0) : 0;
    l_cur = NULL;
}

void armlet_ustats_flush(void)
{
    FILE *f;
    unsigned i, j;
    if (!armlet_ustats_on || !(f = fopen(l_path, "w")))
        return;
    for (i = 0; i < l_n; i++) {
        const struct ent *e = &l_e[i];
        fprintf(f, "U %016" PRIx64 " %016" PRIx64 " %u %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %s\n", e->id, e->text4k, e->family, e->tasks,
                e->native, e->declined, e->nohandler, e->native_ns, e->wasted_ns, e->jit_ns, e->name[0] ? e->name : "(no banner)");
        for (j = 0; j < MAXR && e->r[j].n; j++)
            fprintf(f, "R %016" PRIx64 " %" PRIu64 " %s\n", e->id, e->r[j].n, e->r[j].why);
    }
    fclose(f);
}
