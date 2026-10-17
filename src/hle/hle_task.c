/*
 * arMLEt - hle_task.c: the task-level framework around the native graphics path: capture of task-start snapshots, the JIT as reference
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx_ucode.h"
#include "hle.h"
#include "hle_internal.h"
#include "probe.h"

#ifdef ARMLET_NATIVE
#include "native/native.h"
#include "native/native_internal.h"

struct evbuf {
    PrspNativeEvent *v;
    size_t n, cap;
};

#define POISON_MAX 16
static struct {
    int init, capture, gfx;
    int poison_mask, poison_ring; unsigned n_poison;           /* ARMLET_DMEM_POISON */
    uint16_t poison_lo[POISON_MAX], poison_hi[POISON_MAX];
    uint32_t poison_seed;
    uint64_t cur_id;
    const char *cap_dir;
    unsigned cap_every, cap_max, cap_seen, cap_count;
    int recording;
    PrspNativeSnap *s0, *s1;
    struct evbuf ev;
    uint8_t *rowbuf;                                           /* the RDRAM rows the task's DMA reads read, as they were when read (the task's own writes to the RDP ring may overwrite some of them, as in Cruis'n World) */
    size_t rowbuf_n, rowbuf_cap;
    uint64_t ucode;
    unsigned pc0;
    struct hle_stream st;
    PrspNativeView v;
} H;

/* the rows of one DMA read event, calling put(hdr = { RDRAM address, length }, length, source address, arg) for each */
static void dma_rows(const PrspNativeEvent *e, void (*put)(const uint32_t hdr[2], uint32_t length, uint32_t src, void *arg), void *arg)
{
    uint32_t cache = e->a & ~3u, dram = e->b & ~7u, lenreg = e->c;
    uint32_t length = ((lenreg & 0xfff) + 1 + 7) & ~7u, skip = (lenreg >> 20) & 0xfff;
    unsigned n = (lenreg >> 12) & 0xff, row;
    if (((cache & 0xfff) + length) > 0x1000)
        length = 0x1000 - (cache & 0xfff);
    for (row = 0; row <= n; row++) {
        uint32_t hdr[2] = { dram & 0x7ffffc, length };
        put(hdr, length, dram & 0x7ffffc, arg);
        dram += length + skip;
        cache += length;
    }
}

static void row_count(const uint32_t hdr[2], uint32_t length, uint32_t src, void *arg)
{
    (void)hdr;
    (void)length;
    (void)src;
    (*(uint32_t *)arg)++;
}

static void row_save(const uint32_t hdr[2], uint32_t length, uint32_t src, void *arg)
{
    size_t need = H.rowbuf_n + 8 + length;
    uint32_t j;
    (void)arg;
    if (need > H.rowbuf_cap) {
        H.rowbuf_cap = need * 2;
        H.rowbuf = realloc(H.rowbuf, H.rowbuf_cap);
        if (!H.rowbuf)
            abort();
    }
    memcpy(H.rowbuf + H.rowbuf_n, hdr, 8);
    H.rowbuf_n += 8;
    for (j = 0; j < length; j += 4)
        memcpy(H.rowbuf + H.rowbuf_n + j, (const uint8_t *)H.v.rdram + ((src + j) & 0x7ffffc), 4);
    H.rowbuf_n += length;
}

static void ev_cb(const PrspNativeEvent *e)
{
    if (H.recording && e->kind == PRSP_EV_DMA_RD)
        dma_rows(e, row_save, NULL);
    if (H.ev.n == H.ev.cap) {
        H.ev.cap = H.ev.cap ? H.ev.cap * 2 : 4096;
        H.ev.v = realloc(H.ev.v, H.ev.cap * sizeof(*H.ev.v));
        if (!H.ev.v)
            abort();
    }
    H.ev.v[H.ev.n++] = *e;
    hle_stream_add(&H.st, e, H.v.dmem, H.v.rdram);
}

/* ARMLET_DMEM_POISON=<lo>-<hi>[,...] | mask | all: fills DMEM bytes with a pseudo-random pattern at the end of every graphics task (the JIT's, or the task-level path's), a different
 * pattern each time. "mask" takes the dead ranges of the ucode's table (src/hle), "all" is [0, 0xfc0) (everything but the OSTask header). A range is dead when a run with it poisoned leaves
 * the events, the RDP stream, the pictures and the audio of the unpoisoned run unchanged (docs/design.md, "DMEM masks"). */
static void poison_parse(const char *e)
{
    while (*e) {
        unsigned lo, hi;
        int n = 0;
        if (!strncmp(e, "mask", 4)) {
            H.poison_mask = 1;
            n = 4;
        } else if (!strncmp(e, "ring", 4)) {
            H.poison_ring = 1;
            n = 4;
        } else if (!strncmp(e, "all", 3)) {
            if (H.n_poison < POISON_MAX) {
                H.poison_lo[H.n_poison] = 0;
                H.poison_hi[H.n_poison++] = 0xfc0;
            }
            n = 3;
        } else {
            if (sscanf(e, "%x-%x%n", &lo, &hi, &n) != 2 || hi <= lo || hi > 0x1000 || H.n_poison >= POISON_MAX)
                break;
            H.poison_lo[H.n_poison] = (uint16_t)lo;
            H.poison_hi[H.n_poison++] = (uint16_t)hi;
        }
        e += n;
        if (*e == ',')
            e++;
    }
}

static void poison_range(unsigned lo, unsigned hi)
{
    unsigned a;
    for (a = lo; a < hi; a++) {
        H.poison_seed ^= H.poison_seed << 13;
        H.poison_seed ^= H.poison_seed >> 17;
        H.poison_seed ^= H.poison_seed << 5;
        ((uint8_t *)H.v.dmem)[(a & 0xfff) ^ 3] = (uint8_t)(H.poison_seed >> 11);
    }
}

void armlet_hle_poison(const struct hle_ucode *uc)
{
    unsigned i;
    if (!H.n_poison && !H.poison_mask && !H.poison_ring)
        return;
    if (H.poison_ring && uc && uc->ring_off) {      /* the RDP output ring in RDRAM: the RDP has taken the lists; nothing may read the bytes any more */
        uint32_t lo = ((const uint32_t *)H.v.dmem)[uc->ring_off >> 2] & 0x7ffff8, hi = ((const uint32_t *)H.v.dmem)[(uc->ring_off >> 2) + 1] & 0x7ffff8, a;
        for (a = lo; a < hi && a < 0x800000; a++) {
            H.poison_seed ^= H.poison_seed << 13;
            H.poison_seed ^= H.poison_seed >> 17;
            H.poison_seed ^= H.poison_seed << 5;
            ((uint8_t *)H.v.rdram)[a ^ 3] = (uint8_t)(H.poison_seed >> 11);
        }
    }
    for (i = 0; i < H.n_poison; i++)
        poison_range(H.poison_lo[i], H.poison_hi[i]);
    if (H.poison_mask && uc)
        for (i = 0; i < uc->n_dead; i++)
            poison_range(uc->dead[i].lo, uc->dead[i].hi);
}

void armlet_hle_init(struct armlet_ctx *c)
{
    const char *e;
    (void)c;
    memset(&H, 0, sizeof(H));
    armlet_probe_init(c);
    H.poison_seed = 0x2545f491u;
    if ((e = getenv("ARMLET_DMEM_POISON")) && *e) {
        poison_parse(e);
        armlet_native_ensure_host();
        prsp_native_view(&H.v);
        H.init = 1;
        armlet_log(M64MSG_INFO, "hle: DMEM poison at the end of every graphics task (%s)", e);
    }
    H.cap_dir = (e = getenv("ARMLET_HLE_CAPTURE")) && *e ? e : NULL;
    if (!H.cap_dir)
        return;
    H.cap_every = (e = getenv("ARMLET_HLE_CAPTURE_EVERY")) && *e ? (unsigned)atoi(e) : 16;
    H.cap_max = (e = getenv("ARMLET_HLE_CAPTURE_MAX")) && *e ? (unsigned)atoi(e) : 16;
    if (!H.cap_every)
        H.cap_every = 1;
    armlet_native_ensure_host();
    prsp_native_view(&H.v);
    H.s0 = H.s0 ? H.s0 : calloc(2, sizeof(*H.s0));
    H.s1 = H.s0 + 1;
    H.capture = 1;
    H.init = 1;
    armlet_log(M64MSG_INFO, "hle: capturing graphics task starts to %s (every %u, at most %u)", H.cap_dir, H.cap_every, H.cap_max);
}

void armlet_hle_reset(void)
{
    armlet_probe_reset();
    armlet_hle_core_reset();
    if (H.recording) {
        prsp_native_record(0);
        armlet_event_listener = NULL;
        H.recording = 0;
    }
}

bool armlet_hle_task_begin(struct armlet_ctx *c)
{
    const struct armlet_ucode_id *u;
    bool is_new;
    if (!H.init)
        return false;
    if (armlet_dmem32(c, ARMLET_TASK_UCODE_BOOT_SIZE) > 0x1000 || armlet_dmem32(c, ARMLET_TASK_TYPE) != 1)
        return false;
    if ((*c->rsp.SP_PC_REG & 0xfff) != 0 || (armlet_dmem32(c, ARMLET_TASK_FLAGS) & 1))
        return false;
    u = armlet_gfx_identify(c, &is_new);
    if (u->family == ARMLET_FAM_OTHER && !armlet_hle_find(u->id))      /* (S2DEX and the other families without a banner match have a table entry) */
        return false;
    H.cur_id = u->id;
    H.gfx = 1;
    if (H.capture && H.cap_count < H.cap_max && (H.cap_seen++ % H.cap_every) == 0) {
        H.ucode = u->id;
        H.pc0 = *c->rsp.SP_PC_REG & 0xfff;
        prsp_native_save(H.s0);
        H.ev.n = 0;
        H.rowbuf_n = 0;
        hle_stream_reset(&H.st, armlet_hle_find(u->id));
        armlet_event_listener = ev_cb;
        prsp_native_record(1);
        H.recording = 1;
    }
    return false;
}

void armlet_hle_task_end(struct armlet_ctx *c)
{
    char path[512];
    FILE *f;
    struct armlet_snapfile_hdr h;
    uint32_t nrows;
    size_t i;
    (void)c;
    if (!H.gfx)
        return;
    H.gfx = 0;
    if (!H.recording) {
        armlet_hle_poison(armlet_hle_find(H.cur_id));
        return;
    }
    prsp_native_record(0);
    armlet_event_listener = NULL;
    H.recording = 0;
    prsp_native_save(H.s1);
    snprintf(path, sizeof(path), "%s/hle.task-%016" PRIx64 "-%u.snap", H.cap_dir, H.ucode, H.cap_count);
    f = fopen(path, "wb");
    if (!f) {
        armlet_hle_poison(armlet_hle_find(H.cur_id));
        return;
    }
    memset(&h, 0, sizeof(h));
    h.magic = ARMLET_SNAPFILE_MAGIC;
    h.version = 1;
    snprintf(h.routine, sizeof(h.routine), "hle.task");
    h.ucode = H.ucode;
    h.entry_pc = H.pc0;
    h.exit_pc = H.s1->pc;
    nrows = 0;
    for (i = 0; i < H.ev.n; i++)
        if (H.ev.v[i].kind == PRSP_EV_DMA_RD)
            dma_rows(&H.ev.v[i], row_count, &nrows);
    h.n_rows = nrows;
    {
        PrspNativeEvent se = { HLE_EV_STREAM, (uint32_t)H.st.len, (uint32_t)(H.st.len >> 32), 0, H.st.hash };
        ev_cb(&se);
    }
    h.n_events = (uint32_t)H.ev.n;
    fwrite(&h, sizeof(h), 1, f);
    fwrite(H.s0, sizeof(*H.s0), 1, f);
    fwrite(H.rowbuf, 1, H.rowbuf_n, f);
    fwrite(H.s1, sizeof(*H.s1), 1, f);
    fwrite(H.ev.v, sizeof(PrspNativeEvent), H.ev.n, f);
    fclose(f);
    H.cap_count++;
    armlet_hle_poison(armlet_hle_find(H.cur_id));
}

#else
void armlet_hle_init(struct armlet_ctx *c) { (void)c; }
bool armlet_hle_task_begin(struct armlet_ctx *c) { (void)c; return false; }
void armlet_hle_task_end(struct armlet_ctx *c) { (void)c; }
void armlet_hle_reset(void) {}
void armlet_hle_poison(const struct hle_ucode *uc) { (void)uc; }
#endif
