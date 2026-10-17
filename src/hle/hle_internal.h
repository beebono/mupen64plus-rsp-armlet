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
    uint64_t hash_r;                            /* the same stream with bytes 1 .. 7 of NO_OP and the syncs zeroed (the relaxed comparison of a task that switched builds, hle_task.switched) */
    uint8_t *bytes;
    size_t cap;
    unsigned (*size)(unsigned first_byte);      /* the ucode's own sizing of a command, or NULL */
    uint32_t cmd_pos, cmd_end, ignore_from;     /* the command being parsed, carried over from one list to the next (a command split over lists normalises as one) */
};

static inline void hle_stream_reset(struct hle_stream *s, const struct hle_ucode *uc)
{
    s->size = uc ? uc->rdp_cmd_size : NULL;
    s->hash = s->hash_r = 0xcbf29ce484222325ull;
    s->len = 0;
    s->cmd_pos = s->cmd_end = 0;
    s->ignore_from = ~0u;
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
 * stream comparison zeroes it (docs/design.md, "What verify compares"). Where a one-byte store writes the command (S2DEX), the three bytes behind it in the first word are scratch as well; they are
 * the same in a native run and the JIT's unless the task switched builds (a G_LOAD_UCODE in the middle: the buffers of the build it loaded lie over DMEM that the build before left, in ranges it
 * declares dead and does not keep exactly: the clipper's new vertices, the scratch of the triangle setup); the comparison of such a task zeroes them too (hash_r). */
static inline int hle_rdp_w1_ignored(unsigned op) { return op == 0x00 || (op >= 0x26 && op <= 0x29); }

/* the list [cur, end) as a CMD_END event reports it (status bit 0 = XBUS: the bytes are in DMEM); dmem / rdram are the host word images */
static inline void hle_stream_add(struct hle_stream *s, const PrspNativeEvent *e, const uint32_t *dmem, const uint32_t *rdram)
{
    uint32_t a;
    if (e->kind != PRSP_EV_CMD_END || e->b <= e->a || e->b - e->a > (1u << 20))
        return;
    for (a = e->a; a < e->b; a++) {
        uint8_t byte = e->c & 1 ? ((const uint8_t *)dmem)[(a & 0xfff) ^ 3] : ((const uint8_t *)rdram)[(a & 0x7fffff) ^ 3];
        if (s->cmd_pos == s->cmd_end) {         /* a command starts */
            unsigned op = byte & 0x3f;
            s->cmd_pos = 0;
            s->cmd_end = s->size ? s->size(byte) : hle_rdp_cmd_size(op);
            s->ignore_from = hle_rdp_w1_ignored(op) ? 4 : ~0u;
        }
        {
            uint8_t rb = s->ignore_from != ~0u && s->cmd_pos >= 1 ? 0 : byte;
            if (s->cmd_pos >= s->ignore_from && s->cmd_pos < s->ignore_from + 4)
                byte = 0;
            s->hash_r = (s->hash_r ^ rb) * 0x100000001b3ull;
        }
        s->cmd_pos++;
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

/* like armlet_hle_harness, but the oracle is a JIT run from the same state, made here by jit(ctx) (the state `before` is loaded, SP_PC set, parallelRSPDoRspCycles called), not a recorded
 * result: for snapshots that were changed after the capture (tools/rsp2c/fuzz_snaps.py) */
int armlet_hle_harness_jit(const struct hle_ucode *uc, const PrspNativeSnap *before, void (*jit)(void *), void *jctx, char *out, size_t outsz, struct hle_harness_result *res);

/* The probe (probe.c): a pair run of a build's front-end and the JIT on a canned task, compared. */
struct hle_probe_opts {
    int tol;                                    /* near-match tolerance for the RDP stream: shade coefficients only, 2 levels / 1/128 level per pixel (hle_core.c, stream_tolerable) */
    /* a DMEM byte (address addr, native value na, JIT value nb) that the tolerance admits: nonzero; dmem is the native end state */
    int (*dmem_ok)(const struct hle_ucode *u, const uint32_t *dmem, unsigned addr, unsigned na, unsigned nb);
};
enum { HLE_PROBE_EQUAL = 0, HLE_PROBE_DECLINED, HLE_PROBE_MISMATCH, HLE_PROBE_INCONCLUSIVE, HLE_PROBE_TOLERANT };
struct hle_probe_out {
    int status;                                 /* HLE_PROBE_*: equal; the front-end declined (why); differences (report); the JIT did not finish; equal within the tolerance */
    const char *why;
    char report[2048];
    unsigned n_diff, n_events;
    double native_us, jit_us;
    uint64_t stream_len;
};
int armlet_hle_probe_pair(const struct hle_ucode *uc, const PrspNativeSnap *before, const struct hle_probe_opts *opts, uint32_t win_lo, uint32_t win_hi, uint32_t win2_lo, uint32_t win2_hi,
                          struct hle_probe_out *out);   /* the RDRAM compared: the output ring [win_lo, win_hi) and the matrix stack [win2_lo, win2_hi) */
int armlet_hle_register_dynamic(const struct hle_ucode *u);     /* a build derived at run time: found by armlet_hle_find and armlet_hle_next after the registered ones */
void armlet_hle_unregister_dynamic(const struct hle_ucode *u);

double armlet_hle_dmabench(const struct hle_ucode *uc, const PrspNativeSnap *before, unsigned iters, unsigned bytes, int record);
double armlet_hle_bench(const struct hle_ucode *uc, const PrspNativeSnap *before, unsigned iters);   /* ns per front-end run, or -1 when it declines */

#include <stdio.h>
void armlet_hle_print_masks(FILE *f);    /* the dead DMEM ranges of every registered ucode, for the equivalence shim */

#ifdef __cplusplus
}
#endif

#endif
