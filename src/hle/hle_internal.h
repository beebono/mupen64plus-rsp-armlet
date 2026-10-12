/*
 * arMLEt - hle_internal.h: what hle_core.c, hle_task.c and the offline harness (tools/native/replay.c) share
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ARMLET_HLE_INTERNAL_H
#define ARMLET_HLE_INTERNAL_H

#include <stdlib.h>

#include "hle.h"
#include "native/native.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The RDP command bytes of a task as one stream (list boundaries are not observable, docs/design.md): FNV-1a-64 and length of the bytes of every list in
 * order, and the first STREAM_KEEP bytes for finding the first difference. */
#define HLE_STREAM_KEEP (32u << 20)
struct hle_stream {
    uint64_t hash, len;
    uint8_t *bytes;
    size_t cap;
};

static inline void hle_stream_reset(struct hle_stream *s)
{
    s->hash = 0xcbf29ce484222325ull;
    s->len = 0;
}

/* The size in bytes of the RDP command that starts with `op` (the low 6 bits of its first byte): triangles 32 + 64 shade + 64 texture + 16 z, texture rectangles 16 (the RSP
 * writes the two half-words behind the command), everything else 8. */
static inline unsigned hle_rdp_cmd_size(unsigned op)
{
    if (op >= 0x08 && op <= 0x0f)
        return 32 + ((op & 4) ? 64 : 0) + ((op & 2) ? 64 : 0) + ((op & 1) ? 16 : 0);
    if (op == 0x24 || op == 0x25)
        return 16;
    return 8;
}
/* The second word of NO_OP and of the sync commands is ignored by the RDP; the microcode leaves whatever the output buffer held there (scratch of earlier commands), so the
 * stream comparison zeroes it (docs/design.md, "What verify compares"). */
static inline int hle_rdp_w1_ignored(unsigned op) { return op == 0x00 || (op >= 0x26 && op <= 0x29); }

/* the list [cur, end) as a CMD_END event reports it (status bit 0 = XBUS: the bytes are in DMEM); dmem / rdram are the host word images */
static inline void hle_stream_add(struct hle_stream *s, const PrspNativeEvent *e, const uint32_t *dmem, const uint32_t *rdram)
{
    uint32_t a, o = 0, cmd_end = 0;
    unsigned ignore_from = 0;
    if (e->kind != PRSP_EV_CMD_END || e->b <= e->a || e->b - e->a > (1u << 20))
        return;
    for (a = e->a; a < e->b; a++, o++) {
        uint8_t byte = e->c & 1 ? ((const uint8_t *)dmem)[(a & 0xfff) ^ 3] : ((const uint8_t *)rdram)[(a & 0x7fffff) ^ 3];
        if (o == cmd_end) {                     /* a command starts */
            unsigned op = byte & 0x3f;
            cmd_end = o + hle_rdp_cmd_size(op);
            ignore_from = hle_rdp_w1_ignored(op) ? o + 4 : ~0u;
        }
        if (o >= ignore_from && o < ignore_from + 4)
            byte = 0;
        s->hash = (s->hash ^ byte) * 0x100000001b3ull;
        if (s->len < HLE_STREAM_KEEP) {
            if (s->len >= s->cap) {
                s->cap = s->cap ? s->cap * 2 : 1u << 16;
                s->bytes = (uint8_t *)realloc(s->bytes, s->cap);
                if (!s->bytes)
                    abort();
            }
            s->bytes[s->len] = byte;
        }
        s->len++;
    }
}

/* a task's snapshot file carries the JIT's stream as one more event after the recorded ones */
#define HLE_EV_STREAM 0x100u    /* a = length low, b = length high, hash = FNV-1a-64 */

/* The offline harness: run the family's front-end from `before` (the RDRAM rows it reads must be loaded), compare with the JIT's end state `after` and recorded
 * events `ev` (the last one the HLE_EV_STREAM). Returns the number of differences (0 = identical), or -1 when the front-end declined (the reason in out). */
struct hle_harness_result {
    int declined;
    double native_us;
};
int armlet_hle_harness(const struct hle_ucode *uc, const PrspNativeSnap *before, const PrspNativeSnap *after, const PrspNativeEvent *ev, size_t n_ev, char *out,
                       size_t outsz, struct hle_harness_result *res);

double armlet_hle_bench(const struct hle_ucode *uc, const PrspNativeSnap *before, unsigned iters);   /* ns per front-end run, or -1 when it declines */

#include <stdio.h>
void armlet_hle_print_masks(FILE *f);    /* the dead DMEM ranges of every registered ucode, for the equivalence shim */

#ifdef __cplusplus
}
#endif

#endif
