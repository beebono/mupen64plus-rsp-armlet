/*
 * arMLEt - f3dex.c: the F3DEX 1.x / F3DLX family (F3DEX 1.21, 1.23, 0.95, F3DEX.NoN 1.22, F3DLX 1.23, 0.95) on the task-level native path: a display-list interpreter in C
 *
 * A whole fresh task is run natively: the boot code and the microcode's initialisation (OS code, no vector unit worth replacing) on the RSP interpreter up to the command loop, then the display
 * list by C code over the struct-level functions of f3dex_fx.h, then the end of the task. The microcode's memory layout is kept (DMEM-resident state, docs/ucode-f3dex.md section 3), so what the
 * CPU and the RDP can observe, and DMEM outside the build's dead ranges, equals what the microcode leaves. Anything the code does not implement declines the task (the JIT runs it).
 *
 * The semantics are docs/ucode-f3dex/ (dl, rdp, state, mtx, light, clip) (dl.md: the loop and the task start, rdp.md: the flush, state.md, mtx.md, light.md, clip.md); the numbers of the sections in the comments refer to those.
 * The builds (ucode ids as gfx_ucode.c computes them) are data at the end.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "f3dex_fx.h"
#include "f3dex_dmem.h"

/* test builds only (scripts/build.sh native --tools --hmut=N, tools/hle/mutcheck_fuzz.sh): a deliberate error N, to show that the tests catch it (docs/testing.md) */
#ifdef ARMLET_HLE_MUTATE
#define MUT(n, ok, bad) (ARMLET_HLE_MUTATE == (n) ? (bad) : (ok))
#else
#define MUT(n, ok, bad) (ok)
#endif

#ifdef ARMLET_NATIVE

#include <time.h>
#include <unistd.h>

/* ---- timing of the struct-level functions (F3DEX_PROF=1) and the scalar forms (F3DEX_SCALAR=1), for tools/f3dex/bench ------------------------------------------------------------------ */
static struct {
    int prof, scalar;
    uint64_t vcalls, verts, vns, tcalls[5], tns[5], mcalls, mns, ccalls, cns, rcalls, rns, ovh, tasks, ns_init, ns_flush, nflush, ns_task, ncmds, nsync, ns_sync, ndma, ns_dma, ns_end, nend, bytes_dma;
} P;
#define DT(t0) ({ uint64_t d_ = now_ns() - (t0); d_ > P.ovh ? d_ - P.ovh : 0; })
static inline uint64_t now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
static void prof_init(void)
{
    uint64_t a, b, i;
    P.prof = getenv("F3DEX_PROF") != NULL;
    P.scalar = getenv("F3DEX_SCALAR") != NULL;
    if (P.prof) {                                         /* the cost of a timing pair */
        a = now_ns();
        for (i = 0; i < 1000; i++) b = now_ns();
        P.ovh = (b - a) / 1000;
    }
}
static void prof_report(void)
{
    int i;
    if (!P.prof) return;
    fprintf(stderr, "F3DEXP pid=%d scalar=%d vcalls=%llu verts=%llu vns=%llu", (int)getpid(), P.scalar, (unsigned long long)P.vcalls, (unsigned long long)P.verts, (unsigned long long)P.vns);
    for (i = 0; i < 5; i++)
        fprintf(stderr, " tc%d=%llu tn%d=%llu", i, (unsigned long long)P.tcalls[i], i, (unsigned long long)P.tns[i]);
    fprintf(stderr, " mc=%llu mn=%llu rc=%llu rn=%llu cc=%llu cn=%llu tasks=%llu init=%llu emitn=%llu emitc=%llu task=%llu cmds=%llu syncn=%llu syncc=%llu dman=%llu dmac=%llu dmab=%llu endn=%llu endc=%llu\n",
            (unsigned long long)P.mcalls, (unsigned long long)P.mns, (unsigned long long)P.rcalls, (unsigned long long)P.rns, (unsigned long long)P.ccalls, (unsigned long long)P.cns,
            (unsigned long long)P.tasks, (unsigned long long)P.ns_init, (unsigned long long)P.ns_flush, (unsigned long long)P.nflush, (unsigned long long)P.ns_task, (unsigned long long)P.ncmds,
            (unsigned long long)P.ns_sync, (unsigned long long)P.nsync, (unsigned long long)P.ns_dma, (unsigned long long)P.ndma, (unsigned long long)P.bytes_dma, (unsigned long long)P.ns_end, (unsigned long long)P.nend);
}
static int fx_vtx(const struct f3dex_vctx *c, const struct f3dex_vin *in, unsigned n, struct f3dex_vtx *out)
{
    uint64_t t0 = P.prof ? now_ns() : 0;
    int r = P.scalar ? f3dex_vtx_xform_scalar(c, in, n, out) : f3dex_vtx_xform(c, in, n, out);
    if (P.prof) { P.vns += DT(t0); P.vcalls++; P.verts += n; }
    return r;
}
static int fx_tri(const struct f3dex_tri_in *in, uint8_t *cmd, unsigned *len)
{
    uint64_t t0 = P.prof ? now_ns() : 0;
    int r = P.scalar ? f3dex_tri_setup_scalar(in, cmd, len) : f3dex_tri_setup(in, cmd, len);
    if (P.prof) { P.tns[r] += DT(t0); P.tcalls[r]++; }
    return r;
}
static int fx_tri_noclip(const struct f3dex_tri_in *in, uint8_t *cmd, unsigned *len)
{
    uint64_t t0 = P.prof ? now_ns() : 0;
    int r = P.scalar ? f3dex_tri_setup_noclip_scalar(in, cmd, len) : f3dex_tri_setup_noclip(in, cmd, len);
    if (P.prof) { P.tns[r] += DT(t0); P.tcalls[r]++; }
    return r;
}
static void fx_mtx_mul(const struct f3dex_mtx *a, const struct f3dex_mtx *b, struct f3dex_mtx *o)
{
    uint64_t t0 = P.prof ? now_ns() : 0;
    if (P.scalar) f3dex_mtx_mul_scalar(a, b, o); else f3dex_mtx_mul(a, b, o);
    if (P.prof) { P.mns += DT(t0); P.mcalls++; }
}
static void fx_relight(const struct f3dex_mtx *mv, const int8_t raw[][3], unsigned nrec, int8_t out[][4])
{
    uint64_t t0 = P.prof ? now_ns() : 0;
    if (P.scalar) f3dex_relight_scalar(mv, raw, nrec, out); else f3dex_relight(mv, raw, nrec, out);
    if (P.prof) { P.rns += DT(t0); P.rcalls++; }
}
static void fx_clip(const struct f3dex_vctx *c, const struct f3dex_vtx *const v[3], struct f3dex_clip_out *o)
{
    uint64_t t0 = P.prof ? now_ns() : 0;
    if (P.scalar) f3dex_clip_tri_scalar(c, v, o); else f3dex_clip_tri(c, v, o);
    if (P.prof) { P.cns += DT(t0); P.ccalls++; }
}

/* ---- the builds ------------------------------------------------------------------------------------------------------------------------------------------------------------------ */
struct f3dex_bld {
    const struct f3dex_build *fx;
    uint8_t v095;                       /* F3DEX / F3DLX 0.95: other opcodes, the DL depth byte at 0x110, other CULLDL, no BRANCH_Z / MODIFYVTX / LOAD_UCODE */
};

enum { OV_TEXT = 0, OV_CLIP = 1, OV_LIGHT = 2 };   /* what is in the overlay window: the text image (stubs that load the others), the clipper, the lighting code */

#define STAGE_MAX 4096                  /* bytes staged before a sync */
#define OUTBUF 0xce0
#define WINDOW 0x9a0
#define OPERAND 0xae0
#define RETSTACK 0x398

struct f3dex_task {
    struct hle_task *t;
    const struct f3dex_bld *b;
    uint32_t k0;                        /* RDRAM address of the next command */
    int gp;                             /* bytes left in the command window */
    int ov;
    unsigned ncmds;
    int trace;
    uint32_t wp, fstart, fend;           /* the RDRAM write pointer (DMEM 0x128) and the FIFO bounds, kept here */
    const char *bad;                    /* a reason to decline found where the function could not return one (checked at the head of the loop) */
    uint32_t stage[STAGE_MAX / 4];        /* the staged output as host words holding the big-endian words (the layout of hle_rdram_write) */
    uint32_t pend_ptr, pend_n, vend;      /* the output not yet written (staged in stage[]), and the DPC_END the microcode would have written last */
    int vend_dirty;
    uint32_t junk;                      /* bit k: slot k was loaded by an F3DLX G_VTX with G_CLIPPING off: the microcode's clip flags there are junk, ours are 0 */
    int exact;                          /* issue the DMA reads of the microcode (window, operand buffer): DMEM as the microcode leaves it, the SP register write events */
    const struct f3dex_build *fx;
    unsigned depth;                     /* DMEM address of the DL stack depth byte */
};

static inline void st32u(struct hle_task *t, unsigned a, uint32_t v)       /* unaligned big-endian word, wrapping at 4 KiB */
{
    hle_st8(t, a, (uint8_t)(v >> 24)); hle_st8(t, a + 1, (uint8_t)(v >> 16)); hle_st8(t, a + 2, (uint8_t)(v >> 8)); hle_st8(t, a + 3, (uint8_t)v);
}
static inline uint32_t seg(struct hle_task *t, uint32_t x) { return hle_dm32(t, 0x160 + 4 * ((x >> 24) & 0xf)) + (x & 0xffffff); }

/* ---- the RDP FIFO: the microcode's flush of the output buffer (rdp.md 5, F3DLX: 9) ----------------------------------------------------------------------------------------------- */
/* The output side. The microcode DMAs every command to the FIFO at once and writes DPC_END; only the result is observable (the bytes in RDRAM, the stream the RDP sees, the final DPC and
 * DMEM words), not the chunking, so consecutive chunks that are contiguous in RDRAM are staged in DMEM 0x9a0.. (free in the non-exact mode) and sent by one DMA and one DPC_END write at the
 * next "sync": a FIFO wrap, a full stage, the end of the task. The wrap decision is made per chunk exactly as the microcode does (it depends on the chunk sizes). DPC_END is the value the
 * microcode would have written last (F3DEX: after every chunk; F3DLX: at the publishes W1 / W2 / W3 of rdp.md section 9, so it can lag the write pointer). */

static int sync_out_(struct f3dex_task *f);
static int sync_out(struct f3dex_task *f)
{
    uint64_t t0 = P.prof ? now_ns() : 0;
    int r = sync_out_(f);
    if (P.prof) { P.ns_sync += DT(t0); P.nsync++; }
    return r;
}
static int sync_out_(struct f3dex_task *f)
{
    struct hle_task *t = f->t;
    if (f->pend_n) {
        if (f->trace) fprintf(stderr, "F3DEX sync dma %08x n %u (end %08x dirty %d)\n", f->pend_ptr, f->pend_n, f->vend, f->vend_dirty);
        uint64_t d0 = P.prof ? now_ns() : 0;
        hle_rdram_write(t, f->pend_ptr, f->stage, f->pend_n);
        hle_st32(t, 0x128, f->pend_ptr + f->pend_n);
        if (P.prof) { P.ns_dma += DT(d0); P.ndma++; P.bytes_dma += f->pend_n; }
        f->pend_n = 0;
    }
    if (f->vend_dirty) {
        uint64_t e0 = P.prof ? now_ns() : 0;
        hle_mtc0(t, 9, f->vend);
        if (P.prof) { P.ns_end += DT(e0); P.nend++; }
        f->vend_dirty = 0;
    }
    return HLE_OK;
}

/* F3DLX: DPC_END := the DRAM write pointer (the callers' W1 / W2 / W3) */
static inline void dlx_publish(struct f3dex_task *f)
{
    if (f->fx->dlx) {
        f->vend = f->wp;
        f->vend_dirty = 1;
    }
}

/* the space for a chunk of n bytes (a multiple of 8) of RDP commands: the flush of the microcode's wrap decision. Returns the index of the first stage word, or -1 (declined) */
static int out_reserve(struct f3dex_task *f, unsigned n)
{
    struct hle_task *t = f->t;
    uint32_t ptr = f->wp;
    if ((int32_t)(f->fend - (ptr + n)) < 0) {               /* would run past the FIFO end: back to the start */
        sync_out(f);
        if (*t->cr[11] & 0x400)
            return hle_decline(t, "DPC start valid while wrapping the FIFO"), -1;
        if (*t->cr[10] == f->fstart)
            return hle_decline(t, "RDP at the start of the FIFO while wrapping"), -1;
        hle_mtc0(t, 8, f->fstart);
        ptr = f->fstart;
    } else {
        uint32_t cur = *t->cr[10];
        if (!((int32_t)(ptr - cur) >= 0) && !((int32_t)((ptr + n) - cur) < 0))
            return hle_decline(t, "FIFO full (the RDP would have to catch up)"), -1;
    }
    if (f->pend_n + n > STAGE_MAX)
        sync_out(f);
    if (!f->pend_n)
        f->pend_ptr = ptr;
    f->wp = ptr + n;
    if (!f->fx->dlx) {                                      /* F3DEX: the flush writes DPC_END */
        f->vend = f->wp;
        f->vend_dirty = 1;
    }
    {
        int i = (int)(f->pend_n >> 2);
        f->pend_n += n;
        return i;
    }
}
static int out_chunk(struct f3dex_task *f, const uint8_t *cmd, unsigned n)
{
    unsigned i;
    int w = out_reserve(f, n);
    if (w < 0)
        return HLE_DECLINE;
    for (i = 0; i < n; i += 4) {
        uint32_t x;
        memcpy(&x, cmd + i, 4);
        f->stage[w + (i >> 2)] = __builtin_bswap32(x);       /* (little-endian hosts: the command bytes are big-endian) */
    }
    return HLE_OK;
}

/* the microcode's flush of the output buffer at 0xce0 (exact mode: the DMA and DPC_END at once, as the microcode does) */
static int flush_(struct f3dex_task *f, unsigned n)
{
    struct hle_task *t = f->t;
    uint32_t ptr = hle_dm32(t, 0x128), fend = f->fend, fstart = f->fstart;
    if (n == 0)
        return HLE_OK;
    if ((int32_t)(fend - (ptr + n)) < 0) {
        if (*t->cr[11] & 0x400)
            return hle_decline(t, "DPC start valid while wrapping the FIFO");
        if (*t->cr[10] == fstart)
            return hle_decline(t, "RDP at the start of the FIFO while wrapping");
        hle_mtc0(t, 8, fstart);
        ptr = fstart;
    }
    {
        uint32_t cur = *t->cr[10];
        if (!((int32_t)(ptr - cur) >= 0) && !((int32_t)((ptr + n) - cur) < 0))
            return hle_decline(t, "FIFO full (the RDP would have to catch up)");
    }
    hle_dma_write(t, OUTBUF, ptr, n - 1);
    hle_st32(t, 0x128, ptr + n);
    if (!f->fx->dlx)
        hle_mtc0(t, 9, ptr + n);
    return HLE_OK;
}
static int flush(struct f3dex_task *f, unsigned n)
{
    uint64_t t0 = P.prof ? now_ns() : 0;
    int r = flush_(f, n);
    if (P.prof) { P.ns_flush += DT(t0); P.nflush++; }
    return r;
}

/* the bytes of an RDP command, with the F3DLX publishes around it (pass-through: W1 and W2; triangles: W3 before, nothing after) */
static int emit_bytes(struct f3dex_task *f, const uint8_t *cmd, unsigned len, int tri)
{
    unsigned i;
    int r;
    uint64_t t0 = P.prof ? now_ns() : 0;
    if (f->trace) { fprintf(stderr, "F3DEX emit %u:", len); for (i = 0; i < len && i < 16; i++) fprintf(stderr, " %02x", cmd[i]); fprintf(stderr, "\n"); }
    if (f->exact) {
        if (f->fx->dlx) { hle_mtc0(f->t, 9, hle_dm32(f->t, 0x128)); }
        for (i = 0; i < len; i += 4)
            hle_st32(f->t, OUTBUF + i, (uint32_t)cmd[i] << 24 | (uint32_t)cmd[i + 1] << 16 | (uint32_t)cmd[i + 2] << 8 | cmd[i + 3]);
        r = flush(f, len);
        if (r == HLE_OK && !tri && f->fx->dlx)
            hle_mtc0(f->t, 9, hle_dm32(f->t, 0x128));
        return r == HLE_OK ? HLE_OK : HLE_DECLINE;
    }
    dlx_publish(f);                                         /* W1 / W3 */
    r = out_chunk(f, cmd, len);
    if (r != HLE_OK)
        return HLE_DECLINE;
    if (!tri)
        dlx_publish(f);                                     /* W2 */
    if (P.prof) { P.ns_flush += DT(t0); P.nflush++; }
    return HLE_OK;
}
static int emit8(struct f3dex_task *f, uint32_t w0, uint32_t w1)
{
    if (!f->exact && !f->trace) {                           /* the common case: two words straight into the stage */
        int w;
        dlx_publish(f);
        w = out_reserve(f, 8);
        if (w < 0)
            return HLE_DECLINE;
        f->stage[w] = w0;
        f->stage[w + 1] = w1;
        dlx_publish(f);
        return HLE_OK;
    }
    {
        uint8_t b[8] = { (uint8_t)(w0 >> 24), (uint8_t)(w0 >> 16), (uint8_t)(w0 >> 8), (uint8_t)w0, (uint8_t)(w1 >> 24), (uint8_t)(w1 >> 16), (uint8_t)(w1 >> 8), (uint8_t)w1 };
        return emit_bytes(f, b, 8, 0);
    }
}

/* ---- overlays ------------------------------------------------------------------------------------------------------------------------------------------------------------------- */
static void load_overlay(struct f3dex_task *f, int k)           /* the DMA table entry at DMEM 8 + 8k: dram word, length-1 halfword, memory address halfword */
{
    struct hle_task *t = f->t;
    unsigned d = 8 + 8 * k;
    hle_dma_read(t, hle_dm16(t, d + 6), hle_dm32(t, d), hle_dm16(t, d + 4));
}

/* ---- the operand of a class-0 command: the microcode's DMA into 0xae0, or (not exact) the RDRAM itself ----------------------------------------------------------------------- */
struct src { const uint32_t *img; unsigned base; int ram; };
/* The RDP output ring is written by the task's own flushes and the compare treats the bytes the RDP ignores (second words of syncs, slack) as don't care: a display list, vertices, a matrix or a light
 * read from inside it would read those bytes. Not a game's: decline (docs/design.md). */
static inline int ring_hit(const struct f3dex_task *f, uint32_t a, uint32_t n)
{
    a &= 0x7ffff8;
    return a < (f->fend & 0xffffff) && a + n > (f->fstart & 0x7fffff);
}
static struct src fetch(struct f3dex_task *f, uint32_t dram, uint32_t lenreg)
{
    struct src s;
    if (MUT(243, ring_hit(f, dram, ((lenreg & 0x3ff) | 7) + 1), 0))
        f->bad = "a read from inside the RDP output ring";
    if (f->exact) {
        hle_dma_read(f->t, OPERAND, dram, lenreg);
        s.img = f->t->dmem; s.base = OPERAND; s.ram = 0;
    } else {
        s.img = (const uint32_t *)f->t->rdram; s.base = dram & 0x7ffff8; s.ram = 1;
    }
    return s;
}
static inline unsigned dma_bytes(uint32_t w0) { return ((w0 & 0x3ff) | 7) + 1; }

/* ---- matrices (mtx.md) -------------------------------------------------------------------------------------------------------------------------------------------------------- */
static void mvp_recompute(struct hle_task *t)
{
    struct f3dex_mtx mv, pj, mp;
    f3dex_load_mtx(&mv, t->dmem, F3DEX_DM_MV, 0);
    f3dex_load_mtx(&pj, t->dmem, F3DEX_DM_PROJ, 0);
    fx_mtx_mul(&mv, &pj, &mp);
    f3dex_store_mtx(&mp, t->dmem, F3DEX_DM_MVP);
}

static int cmd_mtx(struct f3dex_task *f, uint32_t w0, uint32_t w1)
{
    struct hle_task *t = f->t;
    unsigned fl = (w0 >> 16) & 0xff;
    struct src s;
    struct f3dex_mtx N, T, R;
    unsigned cur = (fl & 1) ? F3DEX_DM_PROJ : F3DEX_DM_MV;
    if (dma_bytes(w0) < 64)
        return hle_decline(t, "G_MTX operand shorter than a matrix");
    s = fetch(f, seg(t, w1), w0 & 0x3ff);
    f3dex_load_mtx(&N, s.img, s.base, s.ram);
    hle_st8(t, 0x12c, 0xff);
    if (!(fl & 1) && (fl & 4)) {
        uint32_t ptr = hle_dm32(t, 0x134), lim = hle_dm32(t, 0x15c);
        if (ptr != lim) {
            if (MUT(244, ring_hit(f, ptr, dma_bytes(w0)), 0))       /* (the output is staged and written later: a write into the ring now would be ordered differently from the microcode's) */
                return hle_decline(t, "matrix push into the RDP output ring");
            hle_st32(t, 0x134, ptr + 64);
            hle_dma_write(t, F3DEX_DM_MV, ptr, w0 & 0x3ff);
        }
    }
    if (fl & 2) {
        R = N;
    } else {
        f3dex_load_mtx(&T, t->dmem, cur, 0);
        fx_mtx_mul(&N, &T, &R);
    }
    f3dex_store_mtx(&R, t->dmem, cur);
    mvp_recompute(t);
    return HLE_OK;
}

static int cmd_popmtx(struct f3dex_task *f)
{
    struct hle_task *t = f->t;
    uint32_t ptr = hle_dm32(t, 0x134), base = hle_dm32(t, 0xfe0);
    hle_st8(t, 0x12c, 0xff);
    if ((int32_t)(base - ptr) >= 0)
        return HLE_OK;
    ptr -= 64;
    hle_dma_read(t, F3DEX_DM_MV, ptr, 63);
    hle_st32(t, 0x134, ptr);
    mvp_recompute(t);
    return HLE_OK;
}

static int cmd_movemem(struct f3dex_task *f, uint32_t w0, uint32_t w1)
{
    struct hle_task *t = f->t;
    unsigned idx = (w0 >> 16) & 0xff, dest, n, i;
    struct src s = fetch(f, seg(t, w1), w0 & 0x3ff);
    if (idx < 0x80 || idx > 0x9e || (idx & 1))
        return hle_decline(t, "G_MOVEMEM index outside the table");
    dest = hle_dm16(t, 0x270 + idx);
    n = 16 - (dest & 15);                               /* LQV + SQV: up to the next 16-byte boundary */
    if (n > dma_bytes(w0))
        return hle_decline(t, "G_MOVEMEM length shorter than the bytes stored");
    for (i = 0; i < n; i++) {
        unsigned a = s.base + i;
        hle_st8(t, dest + i, s.ram ? ((const uint8_t *)s.img)[(a & 0x7fffff) ^ 3] : hle_dm8(t, a));
    }
    return HLE_OK;
}

/* ---- vertices (light.md) ------------------------------------------------------------------------------------------------------------------------------------------------------ */
static int cmd_vtx(struct f3dex_task *f, uint32_t w0, uint32_t w1)
{
    struct hle_task *t = f->t;
    unsigned v0x2 = (w0 >> 16) & 0xff, n = (w0 >> 10) & 0x3f, cnt = n ? n : 1, i;
    struct f3dex_vctx ctx;
    struct f3dex_vin in[32];
    struct f3dex_vtx out[32];
    struct src s;
    uint32_t geom = hle_dm32(t, 0x114);
    if ((v0x2 & 1) || (v0x2 >> 1) + cnt > 32 || dma_bytes(w0) < 16 * cnt)
        return hle_decline(t, "G_VTX slot range or length unusual");
    if (MUT(239, geom & ~F3DEX_G_DEFINED, 0))               /* (bits the microcode's mode word has no meaning for steer its vertex and triangle code in ways the struct-level functions do not model) */
        return hle_decline(t, "geometry mode bits the build does not define");
    s = fetch(f, seg(t, w1), w0 & 0x3ff);               /* the class-0 DMA comes first */
    if (geom & F3DEX_G_LIGHTING) {
        uint32_t l = hle_dm32(t, 0x12c);
        if (f->ov != OV_LIGHT) {
            load_overlay(f, 2);
            f->ov = OV_LIGHT;
        }
        if ((int32_t)l < 0) {                           /* the lights are stale: transform the directions */
            unsigned nrec = (l & 0xfff) >> 5, k;
            int8_t raw[F3DEX_MAX_REC + 1][3], res[F3DEX_MAX_REC + 1][4];
            struct f3dex_mtx mv;
            if ((l & 31) || nrec > F3DEX_MAX_REC)
                return hle_decline(t, "G_VTX: light count unusual");
            for (k = 0; k <= nrec; k++)
                for (i = 0; i < 3; i++)
                    raw[k][i] = (int8_t)hle_dm8(t, 0x1b8 + 32 * k + i);
            f3dex_load_mtx(&mv, t->dmem, F3DEX_DM_MV, 0);
            fx_relight(&mv, raw, nrec, res);
            for (k = 0; k <= nrec; k++)
                for (i = 0; i < 8; i++)
                    hle_st8(t, 0x1c0 + 32 * k + i, (uint8_t)res[k][i & 3]);
            hle_st32(t, 0x12c, l & 0xfff);
        }
    }
    if (f3dex_load_vctx(&ctx, t->dmem, f->fx))
        return hle_decline(t, "G_VTX: light state unusual");
    for (i = 0; i < cnt; i++)
        f3dex_load_vin(&in[i], s.img, s.base + 16 * i, s.ram);
    if (fx_vtx(&ctx, in, cnt, out))
        return hle_decline(t, "G_VTX: state outside what the native code supports");
    for (i = 0; i < cnt; i++) {
        unsigned a = F3DEX_VSLOT((v0x2 >> 1) + i);
        f3dex_store_vtx(&out[i], t->dmem, a);
        if (f->fx->dlx && !(geom & F3DEX_G_CLIPPING)) f->junk |= 1u << ((v0x2 >> 1) + i); else f->junk &= ~(1u << ((v0x2 >> 1) + i));
        if (f->trace && (v0x2 >> 1) + i == 19) fprintf(stderr, "F3DEX slot17 sy %04x (vtx cmd %08x %08x)\n", (uint16_t)out[i].sy, w0, w1);
        if (i & 1)
            hle_st16(t, a + 38, out[i - 1].flags);       /* the pair's word store: the first vertex's flags land in the second one's padding */
    }
    return HLE_OK;
}

/* ---- triangles (docs/ucode-f3dex.md 5, clip.md) ------------------------------------------------------------------------------------------------------------------------------ */
/* One call of the triangle code: the addresses are twice the vertex indices; flat = the address of the vertex whose colour a flat-shaded triangle uses ($4); *res = the setup result */
static int tri_one(struct f3dex_task *f, unsigned a0, unsigned a1, unsigned a2, unsigned aflat, int *res)
{
    struct hle_task *t = f->t;
    struct f3dex_vtx v[3], vf;
    struct f3dex_tri_in ti;
    uint8_t cmd[0xc0];
    unsigned len = 0, k, ad[3] = { a0, a1, a2 };
    int rc;
    if (MUT(239, hle_dm32(t, 0x114) & ~F3DEX_G_DEFINED, 0))
        return hle_decline(t, "geometry mode bits the build does not define");
    for (k = 0; k < 3; k++) {
        if ((ad[k] & 1) || ad[k] >= 64)
            return hle_decline(t, "G_TRI: vertex index outside the buffer");
        f3dex_load_vtx(&v[k], t->dmem, F3DEX_VSLOT(ad[k] >> 1));
        ti.v[k] = &v[k];
    }
    if ((aflat & 1) || aflat >= 64)
        return hle_decline(t, "G_TRI: vertex index outside the buffer");
    f3dex_load_vtx(&vf, t->dmem, F3DEX_VSLOT(aflat >> 1));
    if (f->fx->dlx && (hle_dm32(t, 0x114) & F3DEX_G_CLIPPING) && (f->junk & ((1u << (a0 >> 1)) | (1u << (a1 >> 1)) | (1u << (a2 >> 1)))))
        return hle_decline(t, "F3DLX triangle with clipping on uses a vertex loaded with clipping off");
    ti.b = f->fx;
    ti.flat = &vf;
    ti.geom = hle_dm32(t, 0x114);
    ti.persp = hle_dm16(t, 0x112);
    ti.tile_byte = hle_dm8(t, 0x122);
    rc = fx_tri(&ti, cmd, &len);
    *res = rc;
    if (f->trace) fprintf(stderr, "F3DEX tri %u %u %u flat %u -> rc %d len %u geom %08x f=%04x %04x %04x\n", a0, a1, a2, aflat, rc, len, ti.geom, v[0].flags, v[1].flags, v[2].flags);
    if (rc == F3DEX_TRI_REJECT || rc == F3DEX_TRI_CULL || rc == F3DEX_TRI_DEGEN)
        return HLE_OK;
    if (rc == F3DEX_TRI_OK)
        return emit_bytes(f, cmd, len, 1);
    {   /* clipping */
        struct f3dex_vctx ctx;
        struct f3dex_clip_out co;
        const struct f3dex_vtx *vp[3] = { &v[0], &v[1], &v[2] };
        unsigned i;
        if (f->ov != OV_CLIP) {
            load_overlay(f, 1);
            f->ov = OV_CLIP;
        }
        f3dex_load_vctx_geom(&ctx, t->dmem, f->fx);
        fx_clip(&ctx, vp, &co);
        for (i = 0; i < co.ntri; i++) {
            struct f3dex_tri_in tj = ti;
            unsigned l2 = 0;
            for (k = 0; k < 3; k++)
                tj.v[k] = co.tri[i][k];
            if (fx_tri_noclip(&tj, cmd, &l2) == F3DEX_TRI_OK && emit_bytes(f, cmd, l2, 1) != HLE_OK)
                return HLE_DECLINE;
        }
    }
    return HLE_OK;
}

/* TRI1: the addresses are the bytes 1..3 of w1 (twice the vertex indices); TRI2: w1's triangle first, then w0's */
static int cmd_tri1(struct f3dex_task *f, uint32_t w1)
{
    int r;
    return tri_one(f, (w1 >> 16) & 0xff, (w1 >> 8) & 0xff, w1 & 0xff, (w1 >> 16) & 0xff, &r);
}
static int cmd_tri2(struct f3dex_task *f, uint32_t w0, uint32_t w1)
{
    int r;
    if (tri_one(f, (w1 >> 16) & 0xff, (w1 >> 8) & 0xff, w1 & 0xff, (w1 >> 16) & 0xff, &r) != HLE_OK)
        return HLE_DECLINE;
    return tri_one(f, (w0 >> 16) & 0xff, (w0 >> 8) & 0xff, w0 & 0xff, (w0 >> 16) & 0xff, &r);
}

/* F3DEX / F3DLX 0.95 G_QUAD (w1 bytes: v4, v1, v2, v3, twice the indices): two triangles split along the diagonal that is longer in y (v1-v3 against v2-v4); the second is skipped when the first was face-culled (the cull exit
 * returns with $31 = 0, which the quad code tests); the flat-shading vertex of both is v1 */
static int cmd_quad(struct f3dex_task *f, uint32_t w1)
{
    struct hle_task *t = f->t;
    unsigned a1 = (w1 >> 16) & 0xff, a2 = (w1 >> 8) & 0xff, a3 = w1 & 0xff, a4 = (w1 >> 24) & 0xff;
    int y[4], r, d13, d24;
    unsigned ad[4] = { a1, a2, a3, a4 }, i;
    for (i = 0; i < 4; i++) {
        if ((ad[i] & 1) || ad[i] >= 64)
            return hle_decline(t, "G_QUAD: vertex index outside the buffer");
        y[i] = (int16_t)hle_dm16(t, F3DEX_VSLOT(ad[i] >> 1) + 26);
    }
    d13 = y[2] - y[0]; if (d13 < 0) d13 = -d13;
    d24 = y[3] - y[1]; if (d24 < 0) d24 = -d24;
    {
        int swap = d13 - d24 > 0;                       /* (the disassembly prints subu's operands swapped: the code computes d13 - d24) */
        if (f->trace) fprintf(stderr, "F3DEX quad y %d %d %d %d d13 %d d24 %d swap %d\n", y[0], y[1], y[2], y[3], d13, d24, swap);
        if (tri_one(f, a1, a2, swap ? a4 : a3, a1, &r) != HLE_OK)
            return HLE_DECLINE;
        if (r == F3DEX_TRI_CULL)
            return HLE_OK;
        return tri_one(f, a3, a4, swap ? a2 : a1, a1, &r);
    }
}

/* ---- display-list control (dl.md 4, 5) ----------------------------------------------------------------------------------------------------------------------------------------- */
/* the end of the display list: pops a call, or ends the task. Returns 1 when the task ended, -1 to decline. */
static int enddl(struct f3dex_task *f)
{
    struct hle_task *t = f->t;
    int d = (int8_t)hle_dm8(t, f->depth) - 4;
    if (d < 0)
        return 1;
    f->k0 = hle_dm32(t, RETSTACK + d);
    hle_st8(t, f->depth, (uint8_t)d);
    f->gp = 0;
    return (f->k0 & 7) ? -1 : 0;
}

/* the display-list address of a jump: the window is loaded from the 8-byte aligned address, which is also where the commands are */
static int jump_to(struct f3dex_task *f, uint32_t a)
{
    f->k0 = a;
    f->gp = 0;
    return (a & 7) ? hle_decline(f->t, "display list address not 8-byte aligned") : HLE_OK;
}

static int f3dex_run(struct hle_task *t, const struct hle_ucode *u)
{
    const struct f3dex_bld *b = (const struct f3dex_bld *)u->fam_data;
    struct f3dex_task f;
    struct hle_cpu c;
    int rc;
    static int exact = -1;
    const unsigned half1 = b->v095 ? 0xb3 : 0xb4, half2 = b->v095 ? 0xb2 : 0xb3;

    if (exact < 0) {
        exact = 0;
        prof_init();
    }
    exact = hle_exact();      /* debugging: issue the microcode's DMA reads too (DMEM staging, the SP register write events) */
    memset(&f, 0, sizeof(f));
    f.t = t;
    f.b = b;
    f.fx = b->fx;
    f.exact = exact;
    { static int tr = -1; if (tr < 0) tr = getenv("F3DEX_TRACE") != NULL; f.trace = tr; }
    f.depth = b->v095 ? 0x110 : 0x148;
    t->fam = &f;
    if (hle_dm32(t, 0xfc4) & 1)
        return hle_decline(t, "yielded task");
    if (*t->cr[4] & 0x80)
        return hle_decline(t, "yield request pending");

    /* the boot code and the microcode's initialisation, on the interpreter, up to the command loop head */
    uint64_t tt0 = P.prof ? now_ns() : 0, ti0 = tt0;
    hle_cpu_init(t, &c, t->start_pc, 1u << 16);
    c.stop_pc = 0x058;
    rc = hle_rsp_run(t, &c);
    if (P.prof) P.ns_init += now_ns() - ti0;
    if (rc == HLE_OK)
        return HLE_OK;                                  /* the boot code ended the task (a pending signal): exactly as the microcode */
    if (rc != HLE_STOP)
        return HLE_DECLINE;
    f.k0 = c.r[HLE_REG_K0] - 320;                       /* $26 = the address after the first window */
    f.gp = 320;
    f.ov = OV_TEXT;
    f.wp = hle_dm32(t, 0x128);
    f.fstart = hle_dm32(t, 0x150);
    f.fend = hle_dm32(t, 0x154);
    if (f.wp & 7)
        return hle_decline(t, "RDP write pointer not 8-byte aligned");
    if ((f.k0 & 7) || c.r[27] != WINDOW || c.r[28] != OPERAND || c.r[23] != OUTBUF)
        return hle_decline(t, "unusual state at the command loop");

    for (;;) {
        uint32_t w0, w1;
        unsigned op;
        if (f.bad)
            return hle_decline(t, f.bad);
        if (++f.ncmds > 30000)
            return hle_decline(t, "more commands than the status polls allow");
        w0 = hle_rd32(t, f.k0);
        w1 = hle_rd32(t, f.k0 + 4);
        f.k0 += 8;
        f.gp -= 8;
        op = w0 >> 24;
        if (f.trace) fprintf(stderr, "F3DEX cmd %u: %08x %08x\n", f.ncmds, w0, w1);
        if (op >= 0xc0) {                               /* the RDP commands: copied, the segment address of the image commands resolved */
            if (op >= 0xfd)
                w1 = seg(t, w1);
            if (emit8(&f, w0, w1) != HLE_OK)
                return HLE_DECLINE;
        } else if (op >= 0x80) {
            if (op == half1) {
                hle_st32(t, 0x10c, w1);
            } else if (op == half2) {
                if (emit8(&f, hle_dm32(t, 0x10c), w1) != HLE_OK)
                    return HLE_DECLINE;
            } else switch (op) {
            case 0xaf: if (!b->v095) return hle_decline(t, "G_LOAD_UCODE"); break;
            case 0xb0: if (!b->v095) {                      /* G_BRANCH_Z */
                    unsigned va = hle_dm16(t, 0x31e + (w0 & 0xfff));
                    uint32_t vz = hle_dm32(t, va + 0x1c);
                    if ((int32_t)(vz - w1) <= 0 && jump_to(&f, seg(t, hle_dm32(t, 0x10c))) != HLE_OK)
                        return HLE_DECLINE;
                }
                break;
            case 0xb1:
                if (cmd_tri2(&f, w0, w1) != HLE_OK)
                    return HLE_DECLINE;
                break;
            case 0xb2:                                      /* 1.2x: G_MODIFYVTX */
                {
                    unsigned va = hle_dm16(t, 0x31e + (w0 & 0xfff));
                    st32u(t, ((w0 >> 16) & 0xff) + va, w1);
                }
                break;
            case 0xb4:                                      /* 0.95: G_PERSPNORM */
                if (!b->v095) return hle_decline(t, "unexpected opcode");
                hle_st16(t, 0x112, (uint16_t)w1);
                break;
            case 0xb5:
                if (b->v095 && cmd_quad(&f, w1) != HLE_OK)
                    return HLE_DECLINE;
                break;
            case 0xb6: hle_st32(t, 0x114, hle_dm32(t, 0x114) & ~w1); break;
            case 0xb7: hle_st32(t, 0x114, hle_dm32(t, 0x114) | w1); break;
            case 0xb8: {
                int e = enddl(&f);
                if (e > 0)
                    goto end;
                if (e < 0)
                    return hle_decline(t, "display list address not 8-byte aligned");
                break;
            }
            case 0xb9: case 0xba: {
                unsigned a = op == 0xb9 ? 0x11c : 0x118;
                uint32_t m = (uint32_t)(((1u << (w0 & 31)) - 1) << ((w0 >> 8) & 31));
                hle_st32(t, a, (hle_dm32(t, a) & ~m) | w1);
                if (emit8(&f, hle_dm32(t, 0x118), hle_dm32(t, 0x11c)) != HLE_OK)
                    return HLE_DECLINE;
                break;
            }
            case 0xbb:
                hle_st32(t, 0x120, w0);
                hle_st32(t, 0x124, w1);
                hle_st32(t, 0x114, (hle_dm32(t, 0x114) & ~2u) | ((w0 & 1) << 1));
                break;
            case 0xbc: {
                unsigned ii = w0 & 0xff;
                unsigned off = (w0 >> 8) & 0xffff;
                if ((ii & 1) || ii > (b->v095 ? 0x0c : 0x0e))
                    return hle_decline(t, "G_MOVEWORD index outside the table");
                if (MUT(238, (off & 3) || off > (ii == 10 ? 0x1fc : 0x3c), 0))     /* a game's offsets are words within the group (segments 0 .. 0x3c, clip ratios, the light colours up to the last light) */
                    return hle_decline(t, "G_MOVEWORD offset outside what a game writes");
                st32u(t, (unsigned)(int16_t)hle_dm16(t, 0x30e + ii) + ((w0 >> 8) & 0xffff), w1);
                break;
            }
            case 0xbd: cmd_popmtx(&f); break;
            case 0xbe: {                                    /* G_CULLDL: culled when some clip-flag bit is set in all the vertices of the range */
                uint32_t acc = f.fx->reject_mask;
                unsigned guard = 0;
                if (b->v095) {
                    unsigned p = w0 & 0x3ff;
                    if (w1 <= p || (w1 - p) % 40 || w1 > 40 * 32)
                        return hle_decline(t, "G_CULLDL range unusual");
                    do { if (f.junk >> (p / 40) & 1) return hle_decline(t, "G_CULLDL over F3DLX vertices loaded with clipping off"); acc &= hle_dm16(t, 0x4c4 + p); p += 40; } while (p != w1 && ++guard < 64);
                } else {
                    unsigned pa = hle_dm16(t, 0x31e + (w0 & 0xfff)), pb = hle_dm16(t, 0x31e + (w1 & 0xfff)), p = pa;
                    if (pb < pa || (pb - pa) % 40 || pb > F3DEX_VSLOT(31))
                        return hle_decline(t, "G_CULLDL range unusual");
                    for (;;) {
                        if (f.junk >> ((p - 0x4a0) / 40) & 1)
                            return hle_decline(t, "G_CULLDL over F3DLX vertices loaded with clipping off");
                        acc &= (uint32_t)(int32_t)(int16_t)hle_dm16(t, p + 0x24);
                        if (p == pb)
                            break;
                        p += 40;
                    }
                }
                if (acc != 0) {
                    int e = enddl(&f);
                    if (e > 0)
                        goto end;
                    if (e < 0)
                        return hle_decline(t, "display list address not 8-byte aligned");
                }
                break;
            }
            case 0xbf:
                if (cmd_tri1(&f, w1) != HLE_OK)
                    return HLE_DECLINE;
                break;
            default:
                return hle_decline(t, "unsupported class-2 opcode");
            }
        } else if (op < 0x40) {                         /* class 0: a DMA into the operand buffer is started for every one of them; then the handler */
            /* (the DMA is not issued in the lean mode, and the buffer and the output buffer after it are dead: a length that runs on into the live DMEM past 0xce0 is not a game's) */
            if (MUT(227, dma_bytes(w0) > OUTBUF - OPERAND, 0))
                return hle_decline(t, "class-0 operand longer than the operand buffer");
            switch (op) {
            case 0x00: case 0x02: case 0x05: case 0x07: case 0x08: case 0x09:
                if (f.exact)
                    hle_dma_read(t, OPERAND, seg(t, w1), w0 & 0x3ff);
                break;
            case 0x01: if (cmd_mtx(&f, w0, w1) != HLE_OK) return HLE_DECLINE; break;
            case 0x03: if (cmd_movemem(&f, w0, w1) != HLE_OK) return HLE_DECLINE; break;
            case 0x04: if (cmd_vtx(&f, w0, w1) != HLE_OK) return HLE_DECLINE; break;
            case 0x06: {
                int d = (int8_t)hle_dm8(t, f.depth);
                if (f.exact)
                    hle_dma_read(t, OPERAND, seg(t, w1), w0 & 0x3ff);
                if (((w0 >> 16) & 0xff) == 0) {          /* a call */
                    if (d < 0)
                        return hle_decline(t, "G_DL: negative stack depth");
                    if (d - 68 > 0)
                        break;                          /* the stack is full: the command is ignored */
                    hle_st32(t, RETSTACK + d, f.k0);
                    hle_st8(t, f.depth, (uint8_t)(d + 4));
                }
                if (jump_to(&f, seg(t, w1)) != HLE_OK)
                    return HLE_DECLINE;
                break;
            }
            default:
                return hle_decline(t, "unsupported class-0 opcode");
            }
        }
        /* class 1 (0x40 .. 0x7f): a no-op without a DMA */
        if (f.gp <= 0) {                                /* the window is used up (or a list change): the next 40 commands */
            if (f.k0 & 7)
                return hle_decline(t, "display list address not 8-byte aligned");
            if (f.exact)
                hle_dma_read(t, WINDOW, f.k0, 319);
            f.gp = 320;
            if (MUT(243, ring_hit(&f, f.k0, 320), 0))
                f.bad = "the display list is inside the RDP output ring";
        }
    }

end:
    if (!f.exact)
        sync_out(&f);
    /* the end of the task: the overlay 3 code is loaded and runs: SP_STATUS gets SIG2, BREAK */
    load_overlay(&f, 3);
    hle_mtc0(t, 4, 0x4000);
    hle_break(t, (hle_dm16(t, 0x26) & 0xfff) + 0x1c);          /* DMEM 0x26: the IMEM address of overlay 3 = the window; its BREAK is at +0x18 */
    if (P.prof) { P.ns_task += now_ns() - tt0; P.tasks++; P.ncmds += f.ncmds; }
    prof_report();
    return HLE_OK;
}

static const struct hle_family l_fam = { .name = "f3dex", .run = f3dex_run };

/* ---- the builds ---------------------------------------------------------------------------------------------------------------------------------------------------------------- */
static const struct f3dex_bld l_dex = { &f3dex_build_dex, 0 };
static const struct f3dex_bld l_non = { &f3dex_build_non, 0 };
static const struct f3dex_bld l_dlx = { &f3dex_build_dlx, 0 };
static const struct f3dex_bld l_dlxn = { &f3dex_build_dlx_non, 0 };
static const struct f3dex_bld l_dex095 = { &f3dex_build_dex, 1 };
static const struct f3dex_bld l_dlx095 = { &f3dex_build_dlx, 1 };

/* DMEM dead at the end of a task: scratch and leftovers (docs/ucode-f3dex.md section 8). CANDIDATES until the poison probe has established them for this family (ARMLET_DMEM_POISON); each
 * entry says what writes it. The window / operand / output buffer are only dead because the native code does not issue the microcode's DMA reads (F3DEX_EXACT=1 does). */
static const struct hle_dead_range l_dead[] = {
    { 0x0b4, 0x0b6 },       /* the clipper's fan pivot */
    { 0x106, 0x108 },       /* the return address G_VTX and the clipper keep, the fan pointer */
    { 0x158, 0x15a },       /* the clipper's stale return address */
    { 0x9a0, 0xce0 },       /* the command window and the operand buffer */
    { 0xbe0, 0xc40 },       /* the triangle scratch (sorted vertex addresses, perspective-scaled s/t/w, texture scale inputs) */
    { 0xc40, 0xc70 },       /* the clipper's polygon lists */
    { 0xce0, 0xd90 },       /* the RDP output buffer and the triangle code's coefficient scratch beyond the command */
    { 0xde0, 0xfc0 },       /* (the vertex clip flags of every slot follow: junk after an F3DLX G_VTX with clipping off, internal to a task otherwise) */       /* the clipper's new vertices, the matrix product scratch (0xde0..0xe1f), the light-recompute scratch (0xdf0..0xe2f) */
    /* the clip flags (+0x24, +0x26) of the 32 vertex slots */
    { 0x4c4, 0x4c8 },
    { 0x4ec, 0x4f0 },
    { 0x514, 0x518 },
    { 0x53c, 0x540 },
    { 0x564, 0x568 },
    { 0x58c, 0x590 },
    { 0x5b4, 0x5b8 },
    { 0x5dc, 0x5e0 },
    { 0x604, 0x608 },
    { 0x62c, 0x630 },
    { 0x654, 0x658 },
    { 0x67c, 0x680 },
    { 0x6a4, 0x6a8 },
    { 0x6cc, 0x6d0 },
    { 0x6f4, 0x6f8 },
    { 0x71c, 0x720 },
    { 0x744, 0x748 },
    { 0x76c, 0x770 },
    { 0x794, 0x798 },
    { 0x7bc, 0x7c0 },
    { 0x7e4, 0x7e8 },
    { 0x80c, 0x810 },
    { 0x834, 0x838 },
    { 0x85c, 0x860 },
    { 0x884, 0x888 },
    { 0x8ac, 0x8b0 },
    { 0x8d4, 0x8d8 },
    { 0x8fc, 0x900 },
    { 0x924, 0x928 },
    { 0x94c, 0x950 },
    { 0x974, 0x978 },
    { 0x99c, 0x9a0 },
    /* the fourth byte of each transformed light direction (and its copy), records 0..9 (the shim of tools/equiv takes 64 ranges per ucode): a leftover of the vector registers the relight reads (mtx.md 4.2), never read */
    { 0x1c3, 0x1c4 }, { 0x1c7, 0x1c8 }, { 0x1e3, 0x1e4 }, { 0x1e7, 0x1e8 }, { 0x203, 0x204 }, { 0x207, 0x208 },
    { 0x223, 0x224 }, { 0x227, 0x228 }, { 0x243, 0x244 }, { 0x247, 0x248 }, { 0x263, 0x264 }, { 0x267, 0x268 },
    { 0x283, 0x284 }, { 0x287, 0x288 }, { 0x2a3, 0x2a4 }, { 0x2a7, 0x2a8 }, { 0x2c3, 0x2c4 }, { 0x2c7, 0x2c8 },
    { 0x2e3, 0x2e4 }, { 0x2e7, 0x2e8 },
};

#define U(var, uid, nm, bld) HLE_UCODE(var) = { .id = (uid), .name = (nm), .fam = &l_fam, .fam_data = &(bld), .dead = l_dead, .n_dead = sizeof(l_dead) / sizeof(l_dead[0]), .ring_off = 0xfe8 }
U(l_bk, 0x208a567e9aead5eaull, "F3DEX 1.21 (Banjo-Kazooie, Glover, Bomberman Hero)", l_dex);
U(l_sf, 0xfd5a31ccf6d40b62ull, "F3DEX.NoN 1.22 (Star Fox 64, Quake, Wetrix)", l_non);
U(l_ts_ex, 0x047252e810e35d56ull, "F3DEX 1.23 (Toy Story 2, Earthworm Jim 3D, Mystical Ninja, SpaceStation)", l_dex);
U(l_ts_lx, 0x7502cfb7098eeffbull, "F3DLX 1.23 (Toy Story 2, A Bug's Life)", l_dlx);
U(l_mk_ex, 0x34dad67335a54cdaull, "F3DEX 0.95 (Mario Kart 64)", l_dex095);
U(l_mk_lx, 0x5f33f7c2be924de0ull, "F3DLX 0.95", l_dlx095);
U(l_wo, 0x19bd8c825b9b3f72ull, "F3DLX 1.21 (Wipeout 64)", l_dlx);
U(l_cf, 0x0a7cc6c93363c663ull, "F3DLX.NoN 1.21 (Clay Fighter SC)", l_dlxn);
U(l_ig, 0xcc09096cb6a48e7bull, "F3DLX.NoN 1.23 (Iggy's Reckin' Balls, Penny Racers)", l_dlxn);
U(l_pp, 0x1de5ed1ca5f278abull, "F3DEX 1.23 (Pokemon Puzzle League)", l_dex);
U(l_q6, 0x9ac3b68bf66e164full, "F3DEX 1.23 (Quest 64)", l_dex);
U(l_tu, 0x5063f108118bf62aull, "F3DEX.NoN 1.00 (Turok)", l_non);

#endif
