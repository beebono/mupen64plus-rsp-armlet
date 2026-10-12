/*
 * arMLEt - hle_f3dex2.c: the F3DEX2 family (F3DEX2, F3DEX2.NoN, F3DZEX, F3DZEX.NoN) on the task-level path: the whole-task native front-end, and the builds as DATA
 *
 * run(): the boot code (the initialisation up to the first fetch of the command loop) runs on the interpreter (hle_rsp.cpp), exactly; then a C loop takes over the display list: it
 * reads the commands (in the lean mode straight from RDRAM, with ARMLET_HLE_EXACT=1 through the microcode's 168-byte refills of the input buffer in DMEM), keeps the microcode's own state
 * where the microcode keeps it (DMEM is the home of the matrices, the lights, the vertex records, the geometry mode ...), and does what each command does with the functions of f3dex2_fx.h
 * (struct level, scalar and NEON). The end of the task (G_ENDDL of the outermost list) goes back to the interpreter: the microcode's own exit path flushes the RDP buffer and ends the
 * task. A command the loop does not know, a state it does not support or a situation in which the microcode would wait for the RDP makes the whole task decline (the JIT runs it).
 *
 * What is compared with the interpreter / the JIT: docs/design.md, "What verify compares" (external effects: final DMEM outside the dead ranges, IMEM, the SP / DPC / MI registers, the
 * RDRAM the task wrote, the RDP command stream).
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "f3dex2_dmem.h"
#include "f3dex2_fx.h"
#include "hle.h"

#ifdef ARMLET_NATIVE

#ifdef ARMLET_HLE_MUTATE
#define MUT(n, ok, bad) (ARMLET_HLE_MUTATE == (n) ? (bad) : (ok))
#else
#define MUT(n, ok, bad) (ok)
#endif

/* development: HLE_F3_PROF=1 sums the time of each command kind over all tasks and prints it at exit */
static uint64_t g_pt[40], g_pn[40];
static int g_prof = -1;
static inline uint64_t ticks(void)
{
#if defined(__aarch64__)
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    unsigned lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
#endif
}
static void prof_dump(void)
{
    static const char *nm[] = { "bad", "nop", "dl", "enddl", "texture", "texrect", "half1", "half2", "moveword", "movemem", "omh", "oml", "geom", "copy", "sync", "segcopy", "ef", "ed", "vtx", "mtx", "popmtx", "tri1", "tri2", "modvtx", "culldl", "branchz", "mvp" };
    unsigned k;
    for (k = 0; k < 27; k++)
        if (g_pn[k])
            fprintf(stderr, "PROF %-9s n=%-9llu ticks=%-12llu per=%.1f\n", nm[k], (unsigned long long)g_pn[k], (unsigned long long)g_pt[k], (double)g_pt[k] / g_pn[k]);
}

/* ---- the builds ------------------------------------------------------------------------------------------------------------------------------------------------------------------ */
struct f3dex2_build {
    uint16_t head;                  /* the command loop: mfc0 at (the fetch of the next command) */
    uint16_t copy, sync, segcopy;   /* handlers: copy a command to the RDP buffer / copy only its first word / resolve the segment address, then copy */
    uint16_t ef, ed;                /* G_RDPSETOTHERMODE and G_SETSCISSOR: remember the command, then copy */
    uint16_t tri1, tri2;            /* G_TRI1; G_TRI2 and G_QUAD */
    uint16_t vtx, mtx, geom, enddl, half2, modvtx, culldl, branchz;
    uint16_t mvp;                   /* a handler that concatenates the matrices now (G_SPECIAL_1 of F3DEX.NoN 2.08H), or 0 */
    unsigned zex;                   /* F3DZEX: other lanes of the constant vectors, no mask on the slope fractions */
    uint32_t reject_mask;           /* a triangle whose three clip-flag words have a common bit in this mask is rejected */
};

/* The handlers below 0x170 sit at the same pcs in all the builds: the display list stack, the state words the other handlers read, the other-mode registers */
enum { PC_DL = 0x000, PC_TEXTURE = 0x028, PC_TEXRECT = 0x02c, PC_HALF1 = 0x030, PC_MOVEWORD = 0x038, PC_POPMTX = 0x04c, PC_MOVEMEM = 0x120, PC_OMH = 0x13c, PC_OML = 0x140 };

enum { K_BAD = 0, K_NOP, K_DL, K_ENDDL, K_TEXTURE, K_TEXRECT, K_HALF1, K_HALF2, K_MOVEWORD, K_MOVEMEM, K_OMH, K_OML, K_GEOM, K_COPY, K_SYNC, K_SEGCOPY, K_EF, K_ED, K_VTX, K_MTX, K_POPMTX,
       K_TRI1, K_TRI2, K_MODVTX, K_CULLDL, K_BRANCHZ, K_MVP };

/* DMEM layout (the same in all the builds): jump table at 0x36e by opcode; segments at 0xf8; RDP FIFO pointer 0xf0, FIFO bounds 0xfe8 / 0xfec; the display list stack depth at 222 (x4) and
 * the stack at 0x138; the RDP buffers: two of 344 bytes, s7 the write pointer, s6 the end of the current one (xor 0x208 toggles) */
enum { JUMP_TABLE = 0x36e, SEGMENTS = 0xf8, FIFO_PTR = 0xf0, FIFO_START = 0xfe8, FIFO_END = 0xfec, OUT_BYTES = 344, OUT_TOGGLE = 0x208, DL_DEPTH = 222, DL_STACK = 0x138,
       SIG0 = 0x80, DPC_BUSY_BIT = 0x400, POLL_LIMIT = 30000 };

enum { OV_CLIP = 0, OV_LIGHT = 1, OV_UNKNOWN = 2 };
enum { DESC_LIGHT = 0x410, DESC_CLIP = 0x418, OV_LO = 0x2d0 };

struct f3 {
    struct hle_task *t;
    const struct f3dex2_build *b;
    uint32_t k0, k1, s6, s7;        /* the microcode's registers */
    unsigned polls;                 /* SP_STATUS polls the microcode would have made (the JIT halts a task at 32767) */
    int ov;                         /* which overlay IMEM holds at [0x2d0, ...) */
    uint8_t kind[256];
    uint32_t w0, w1;
    struct fx2_clip_out co;
    struct fx2_tri_in tin;          /* the constants of the triangle setup */
    uint32_t clip_mask;
    int lean;                       /* the external-effects mode: no DMA the effects do not need (the display list and the vertices are read from RDRAM, the overlays are loaded once at the end) */
    int ov0;                        /* the overlay IMEM held at the start */
    int trace, watch;
    uint16_t wval;
    unsigned ncmd;
    uint32_t total;
};

/* ---- the RDP buffer --------------------------------------------------------------------------------------------------------------------------------------------------------- */
/* The flush of the RDP buffer (the microcode's code after every command that writes to it): the buffer holds s7 - (s6 - 344) bytes; the FIFO pointer in DMEM says where in RDRAM they
 * go; DPC_END is moved up to the end of the chunk written before this one (the RDP is always one chunk behind); when the chunk does not fit before the end of the FIFO the pointer wraps to
 * the start (DPC_START) first. The microcode would wait for the RDP; with the video plugin synchronous there is nothing to wait for, and a state in which it would wait forever (it would
 * hang the JIT too) declines. */
static int flush(struct f3 *f)
{
    struct hle_task *t = f->t;
    uint32_t cur = hle_dm32(t, FIFO_PTR);
    uint32_t n = (f->s7 - f->s6) + OUT_BYTES;
    int32_t room;
    if (*t->cr[6])
        return hle_decline(t, "DMA busy while flushing");
    hle_mtc0(t, 9, cur);
    room = (int32_t)hle_dm32(t, FIFO_END) - (int32_t)(cur + n);
    if (room < 0) {
        if (*t->cr[11] & DPC_BUSY_BIT)
            return hle_decline(t, "DPC busy while wrapping the FIFO");
        cur = hle_dm32(t, FIFO_START);
        if (*t->cr[10] == cur)
            return hle_decline(t, "RDP at the start of the FIFO while wrapping");
        MUT(4, hle_mtc0(t, 8, cur), 0);
    }
    {
        int32_t d = (int32_t)*t->cr[10] - (int32_t)cur;
        if (d > 0 && d - (int32_t)n <= 0)
            return hle_decline(t, "FIFO full (the RDP would have to catch up)");
    }
    hle_st32(t, FIFO_PTR, cur + n);
    f->total += n;
    if (f->lean && !(cur & 7) && !(n & 7))
        hle_rdram_write(t, cur, &t->dmem[(f->s6 - OUT_BYTES) >> 2], n);
    else
        hle_dma_write(t, f->s6 - OUT_BYTES, cur, n - 1);
    f->s6 ^= MUT(3, OUT_TOGGLE, 0x200);
    f->s7 = f->s6 - OUT_BYTES;
    return HLE_OK;
}

/* after a command that wrote to the buffer */
static inline int after_emit(struct f3 *f)
{
    if ((int32_t)(f->s7 - f->s6) > 0)
        return flush(f);
    return HLE_OK;
}

static inline uint32_t seg_addr(const struct hle_task *t, uint32_t w) { return (w & 0xffffff) + hle_dm32(t, SEGMENTS + ((w >> 22) & 0x3c)); }

/* the display list refill that G_DL and G_ENDDL jump into, and the loop's own: the next 168 bytes from k0 */
/* a DMA read into DMEM as the microcode makes it: in the lean mode a plain copy (one row, to DMEM) with the DMA's rounding and clipping, else through the framework */
static void dma_rd(struct f3 *f, unsigned mem, uint32_t dram, uint32_t len)
{
    struct hle_task *t = f->t;
    if (f->lean && !(mem & 0x1000) && !(len >> 12)) {
        unsigned n = ((len & 0xfff) + 1 + 7) & ~7u, m = mem & 0xff8, k;
        if (m + n > 0x1000)
            n = 0x1000 - m;
        for (k = 0; k < n; k += 4)
            t->dmem[(m + k) >> 2] = hle_rd32(t, (dram & 0x7ffff8) + k);
        return;
    }
    hle_dma_read(t, mem, dram, len);
}

static inline void refill(struct f3 *f)
{
    if (!f->lean)
        hle_dma_read(f->t, F2_IN, f->k0, F2_IN_REFILL - 1);
    f->k0 += F2_IN_REFILL;
    f->k1 = (uint32_t)-F2_IN_REFILL;
    f->polls++;
}

/* ---- overlays ------------------------------------------------------------------------------------------------------------------------------------------------------------------- */
/* IMEM [0x2d0, ...) holds the clipping code or the lighting code; each has a descriptor in DMEM (RDRAM address, length register, IMEM address). Which one is resident is found by comparing IMEM with the
 * RDRAM the descriptor points to. */
static int overlay_match(const struct hle_task *t, unsigned desc)
{
    uint32_t dram = hle_dm32(t, desc);
    unsigned reg = hle_dm16(t, desc + 4), imem = hle_dm16(t, desc + 6) & 0xff8, len = ((reg & 0xfff) + 1 + 7) & ~7u;
    if (imem + len > 0x1000 || (dram & 0x7fffff) + len > 0x800000)
        return 0;
    return memcmp((const uint8_t *)t->imem + imem, t->rdram + (dram & 0x7ffff8), len) == 0;
}

static void overlay_load(struct f3 *f, int which)
{
    unsigned desc = which == OV_LIGHT ? DESC_LIGHT : DESC_CLIP;
    struct hle_task *t = f->t;
    if (!f->lean)
        hle_dma_read(t, hle_dm16(t, desc + 6), hle_dm32(t, desc), hle_dm16(t, desc + 4));
    f->ov = which;
}

/* ---- the commands ------------------------------------------------------------------------------------------------------------------------------------------------------------- */
static void do_mtx_mul_mvp(struct f3 *f, uint32_t w0)
{
    struct fx2_mtx mv, p, r;
    f2_ld_mtx(f->t, F2_MV, &mv);
    f2_ld_mtx(f->t, F2_P, &p);
    fx2_mtx_mul(&mv, &p, &r);
    f2_st_mtx(f->t, F2_MVP, &r);
    hle_st8(f->t, F2_MVP_VALID, (uint8_t)w0);
}

static int cmd_vtx(struct f3 *f)
{
    struct hle_task *t = f->t;
    uint32_t w0 = f->w0, w1 = f->w1, a1 = hle_dm16(t, F2_GEOM);
    unsigned n = (w0 >> 12) & 0xff, endslot, first, s4, i, lit = a1 & 2;
    struct fx2_vctx c;
    struct fx2_vin in[34];
    struct fx2_vtx out[34];

    if (n == 0 || n > 32)
        return hle_decline(t, "G_VTX with an unusual count");
    if (lit) {
        unsigned lb = hle_dm8(t, F2_LIGHTS_SIZE);
        if (!(lb >= F2_LIGHT_STRIDE && lb % F2_LIGHT_STRIDE == 0 && lb <= 7 * F2_LIGHT_STRIDE))
            return hle_decline(t, "light count");
        if (f->ov == OV_UNKNOWN)
            return hle_decline(t, "unknown overlay");
        if (!hle_dm8(t, F2_LIGHTS_VALID)) {             /* the lights have changed since the last lit vertex command: the lighting overlay's setup code */
            struct fx2_mtx mv;
            int8_t raw[FX2_LSLOTS][3], o[FX2_LSLOTS][3];
            unsigned nl = lb / F2_LIGHT_STRIDE, m, j;
            f2_ld_mtx(t, F2_MV, &mv);
            for (m = 0; m <= nl + 1; m++)
                for (j = 0; j < 3; j++)
                    raw[m][j] = (int8_t)hle_dm8(t, F2_LIGHT_RAW + F2_LIGHT_STRIDE * m + j);
            fx2_light_setup(&mv, raw, nl, (int16_t)hle_dm16(t, f->b->zex ? 0x1ce : 0x1c6), o);
            hle_st8(t, F2_LIGHTS_VALID, (uint8_t)w0);
            for (m = 0; m <= nl + 1; m++)
                for (j = 0; j < 3; j++) {
                    hle_st8(t, F2_LIGHT_BASE + F2_LIGHT_STRIDE * m + j, (uint8_t)o[m][j]);
                    hle_st8(t, F2_LIGHT_BASE + F2_LIGHT_STRIDE * m + 4 + j, (uint8_t)o[m][j]);
                }
        }
    }
    if (!hle_dm8(t, F2_MVP_VALID))                      /* the MVP matrix is stale: modelview x projection */
        do_mtx_mul_mvp(f, w0);
    if (f->trace)
        fprintf(stderr, "  VTX n=%u geom=%04x lights=%u/%u mvpvalid=%02x ov=%d\n", n, a1, hle_dm8(t, F2_LIGHTS_SIZE), hle_dm8(t, F2_LIGHTS_VALID), hle_dm8(t, F2_MVP_VALID), f->ov);
    endslot = hle_dm16(t, F2_VTAB + (w0 & 0xfff));
    first = hle_dm16(t, F2_VTAB + ((w0 - n * 2) & 0xfff));
    s4 = endslot - n * 16;
    if (f->lean) {
        if (seg_addr(t, w1) & 7)
            return hle_decline(t, "unaligned vertices");
    } else
        hle_dma_read(t, s4, seg_addr(t, w1), n * 16 - 1);
    if (lit && f->ov == OV_CLIP)                        /* the code at 0x2d0 (the clipping overlay's) loads the lighting overlay */
        overlay_load(f, OV_LIGHT);

    f2_ld_vctx(t, &c);
    if (lit)
        f2_ld_lights(t, hle_dm8(t, F2_LIGHTS_SIZE) / F2_LIGHT_STRIDE, &c.L);
    if (f->lean) {
        uint32_t va = seg_addr(t, w1);
        for (i = 0; i < n; i++)
            f2_ld_vin_rdram(t, va + 16 * i, &in[i]);
        if (n & 1)                                      /* an odd count: the pair's second vertex is computed from whatever DMEM holds after the loaded ones; only its clip bits are used */
            f2_ld_vin(t, s4 + 16 * n, &in[n]);
    } else
        for (i = 0; i < n + (n & 1); i++)
            f2_ld_vin(t, s4 + 16 * i, &in[i]);
    fx2_vtx_xform(&c, in, n, out);
    for (i = 0; i < n; i++)
        f2_st_vtx(t, first + F2_VSTRIDE * i, &out[i]);
    return HLE_OK;
}

static int cmd_mtx(struct f3 *f)
{
    struct hle_task *t = f->t;
    uint32_t w0 = f->w0, w1 = f->w1;
    unsigned at = w0 & 0xfe, len = (w0 >> 16) & 0xff, dst = hle_dm16(t, 0x2f0 + at), k;
    if (!((w0 & 0xfe) < 8 && ((w0 >> 5) & 0x1fff) == 0))
        return hle_decline(t, "G_MTX with an unusual index");
    if ((w0 & 5) == 0) {                                /* push: the current modelview matrix goes to the matrix stack in RDRAM (pointer at 0xf4, which moves up by 64) */
        uint32_t sp = hle_dm32(t, 0xf4);
        hle_dma_write(t, 0, sp, 63);
        hle_st32(t, 0xf4, sp + MUT(14, 64, 32));
    }
    for (k = 0; k < MUT(15, 4, 3); k++)
        hle_st8(t, F2_MVP_VALID + k, 0);                /* the MVP matrix and the lights are stale */
    dma_rd(f, dst, seg_addr(t, w1), len);
    if (!(w0 & 2)) {                                    /* multiply: new x current */
        unsigned d = hle_dm16(t, 0x2f2 + at);
        struct fx2_mtx a, b, r;
        f2_ld_mtx(t, MUT(16, dst, d), &a);
        f2_ld_mtx(t, MUT(16, d, dst), &b);
        fx2_mtx_mul(&a, &b, &r);
        f2_st_mtx(t, d, &r);
    }
    return HLE_OK;
}

static int cmd_popmtx(struct f3 *f)
{
    struct hle_task *t = f->t;
    uint32_t w0 = f->w0, w1 = f->w1, sp = hle_dm32(t, 0xf4), bottom = hle_dm32(t, 0xfe0), ns = sp - w1;
    unsigned at = w0 & 0xfe, k;
    if ((int32_t)(ns - bottom) < 0)
        ns = bottom;
    hle_st32(t, 0xf4, ns);
    if (ns == sp)
        return HLE_OK;
    if (!((w0 & 0xfe) < 8 && ((w0 >> 5) & 0x1fff) == 0))
        return hle_decline(t, "G_POPMTX with an unusual index");
    for (k = 0; k < 4; k++)
        hle_st8(t, F2_MVP_VALID + k, 0);
    dma_rd(f, hle_dm16(t, 0x2f0 + at), ns, (w0 >> 16) & 0xff);
    return HLE_OK;
}

/* the end of the current display list: pops the stack, or (outermost list) hands over to the interpreter, which runs the microcode's own end of the task */
static int do_enddl(struct f3 *f, int *handover)
{
    struct hle_task *t = f->t;
    unsigned at = hle_dm8(t, DL_DEPTH);
    if (at == 0) {
        *handover = 1;
        return HLE_OK;
    }
    at -= 4;
    f->k0 = hle_dm32(t, DL_STACK + at);
    hle_st8(t, DL_DEPTH, (uint8_t)at);
    refill(f);
    return HLE_OK;
}

/* the constants of the triangle setup that the microcode reads from DMEM: once per triangle command */
static void tri_consts(struct f3 *f)
{
    struct hle_task *t = f->t;
    struct fx2_tri_in *in = &f->tin;
    unsigned ca = F2_CULLTAB + hle_dm8(t, F2_GEOM + 2);
    in->g = hle_dm32(t, F2_GEOM);
    in->tex = hle_dm8(t, F2_TEXTURE + 3);
    in->tile = hle_dm8(t, F2_TEXTURE + 2);
    in->reject_mask = f->b->reject_mask;
    in->cull = (uint32_t)hle_dm8(t, ca) << 24 | (uint32_t)hle_dm8(t, ca + 1) << 16 | (uint32_t)hle_dm8(t, ca + 2) << 8 | hle_dm8(t, ca + 3);
    if (f->b->zex) {
        in->rcp_k = hle_dm16(t, F2_CONST31 + 4);
        in->dx_k = hle_dm16(t, F2_CONST31 + 10);
        in->slope_lim = (int16_t)hle_dm16(t, F2_CONST30 + 4);
        in->frac_mask = 0xffff;
    } else {
        in->rcp_k = hle_dm16(t, F2_CONST30 + 14);
        in->dx_k = hle_dm16(t, F2_CONST30 + 4);
        in->slope_lim = (int16_t)hle_dm16(t, F2_CONST30 + 6);
        in->frac_mask = hle_dm16(t, F2_CONST30 + 10);
    }
    f->clip_mask = hle_dm32(t, F2_CLIPMASK);
}

/* the triangle setup for three vertex records; HLE_OK, with the RDP command (if any) in the buffer */
static int tri_setup(struct f3 *f, const struct fx2_vtx *const v[3], uint32_t clip_mask, int *needs_clip)
{
    struct hle_task *t = f->t;
    struct fx2_tri_in *in = &f->tin;
    uint32_t cmd[0xb0 / 4];
    unsigned len = 0;
    int r;

    in->v[0] = v[0];
    in->v[1] = v[1];
    in->v[2] = v[2];
    in->clip_mask = clip_mask;
    r = fx2_tri_setup(in, cmd, &len);
    if (r == FX2_TRI_CLIP) {
        *needs_clip = 1;
        return HLE_OK;
    }
    if (r != FX2_TRI_DRAW)
        return HLE_OK;
    memcpy(&t->dmem[f->s7 >> 2], cmd, len);
    f->s7 += len;
    return after_emit(f);
}

/* the clipper (the clipping overlay's code, entered from the triangle code when a vertex has a flag bit of the clip mask) */
static int do_clip(struct f3 *f, const struct fx2_vtx *const v[3])
{
    struct hle_task *t = f->t;
    struct fx2_vctx vc;
    struct fx2_clip_ctx cc;
    struct fx2_clip_out *co = &f->co;
    unsigned p, k;
    int dummy = 0, rc;

    if (f->ov == OV_UNKNOWN)
        return hle_decline(t, "unknown overlay");
    if (f->ov == OV_LIGHT)                              /* the lighting overlay's entry loads the clipping overlay */
        overlay_load(f, OV_CLIP);
    f2_ld_vctx(t, &vc);
    cc.v = &vc;
    for (p = 0; p < 6; p++) {
        unsigned a1 = 20 - 4 * p;
        for (k = 0; k < 4; k++)
            cc.plane[p][k] = (int16_t)hle_dm16(t, 0x180 + 2 * a1 + 2 * k);
        cc.mask[p] = hle_dm32(t, 0x3f8 + a1);
    }
    if (fx2_clip_tri(&cc, v, co))
        return hle_decline(t, "clipper: too many vertices");
    for (k = 0; k < co->ntri; k++) {
        rc = tri_setup(f, co->tri[k], 0, &dummy);
        if (rc != HLE_OK)
            return rc;
    }
    return HLE_OK;
}

/* one triangle of a G_TRI1 / G_TRI2 / G_QUAD word (bytes 1 .. 3 the vertex numbers * 2): the RDP command into the buffer */
static int do_tri(struct f3 *f, uint32_t word)
{
    struct hle_task *t = f->t;
    unsigned sv[3] = { hle_dm16(t, F2_VTAB + ((word >> 16) & 0xff)), hle_dm16(t, F2_VTAB + ((word >> 8) & 0xff)), hle_dm16(t, F2_VTAB + (word & 0xff)) };
    struct fx2_vtx v[3];
    const struct fx2_vtx *vp[3] = { &v[0], &v[1], &v[2] };
    uint32_t fl[3];
    unsigned k;
    int clip = 0, rc;

    for (k = 0; k < 3; k++)
        fl[k] = hle_dm32(t, sv[k] + 0x24);
    if (fl[0] & fl[1] & fl[2] & f->b->reject_mask)
        return HLE_OK;
    for (k = 0; k < 3; k++)
        f2_ld_vtx_lite(t, sv[k], &v[k]);
    if (f->trace) {
        fprintf(stderr, "TRI");
        for (k = 0; k < 3; k++)
            fprintf(stderr, " [%03x s=%04x t=%04x xy=%08x rw=%08x fl=%08x col=%08x z=%08x]", sv[k], (uint16_t)v[k].s, (uint16_t)v[k].t, (uint32_t)(uint16_t)v[k].sx << 16 | (uint16_t)v[k].sy, (uint32_t)(uint16_t)v[k].rwi << 16 | v[k].rwf, v[k].flags,
                    (uint32_t)v[k].col[0] << 24 | (uint32_t)v[k].col[1] << 16 | (uint32_t)v[k].col[2] << 8 | v[k].col[3], (uint32_t)(uint16_t)v[k].szi << 16 | v[k].szf);
        fprintf(stderr, "\n");
    }
    rc = tri_setup(f, vp, f->clip_mask, &clip);
    if (rc != HLE_OK || !clip)
        return rc;
    for (k = 0; k < 3; k++)                             /* the clipper needs the clip coordinates too */
        f2_ld_vtx(t, sv[k], &v[k]);
    return do_clip(f, vp);
}

static int run_loop(struct f3 *f, int *handover)
{
    struct hle_task *t = f->t;
    const struct f3dex2_build *b = f->b;

    for (;;) {
        uint32_t w0, w1, k, e0, e1;
        unsigned kd, at, addr;
        uint64_t t_cmd = 0;
        int rc = HLE_OK;

        if (f->polls > POLL_LIMIT)
            return hle_decline(t, "too many SP_STATUS polls (the JIT would halt)");
        if (f->k1 == 0) {                               /* the loop head found the buffer empty: refill */
            refill(f);
            continue;
        }
        if (f->lean) {
            f->w0 = w0 = hle_rd32(t, f->k0 + f->k1);
            f->w1 = w1 = hle_rd32(t, f->k0 + f->k1 + 4);
        } else {
            f->w0 = w0 = hle_dm32(t, F2_IN_END + f->k1);
            f->w1 = w1 = hle_dm32(t, F2_IN_END + 4 + f->k1);
        }
        f->polls++;
        f->k1 += 8;
        kd = f->kind[w0 >> 24];
        t_cmd = g_prof ? ticks() : 0;
        if (f->trace)
            fprintf(stderr, "CMD %5u k0=%08x op=%02x kind=%u w0=%08x w1=%08x out=%u\n", f->ncmd++, f->k0 + f->k1, w0 >> 24, kd, w0, w1, f->total + (f->s7 - (f->s6 - OUT_BYTES)));
        switch (kd) {
        case K_NOP:
            break;
        case K_DL: {
            unsigned d = hle_dm8(t, DL_DEPTH);
            uint32_t ret = f->k0 + f->k1, target = seg_addr(t, w1);
            if ((int32_t)(w0 << 15) >= 0) {             /* a call: the return address goes on the stack (bit 16 set: a branch) */
                hle_st32(t, DL_STACK + d, ret);
                hle_st8(t, DL_DEPTH, (uint8_t)(d + 4));
            }
            f->k0 = target;
            refill(f);
            break;
        }
        case K_ENDDL:
            rc = do_enddl(f, handover);
            if (*handover)
                return rc;
            break;
        case K_GEOM:
            hle_st32(t, F2_GEOM, (hle_dm32(t, F2_GEOM) & w0) | w1);
            break;
        case K_TEXTURE:
            hle_st32(t, F2_TEXTURE, w0);
            hle_st32(t, F2_TEXTURE + 4, w1);
            break;
        case K_TEXRECT:
            hle_st32(t, 0xd0, w0);
            hle_st32(t, 0xd4, w1);
            break;
        case K_HALF1:
            hle_st32(t, 0xd8, w1);
            break;
        case K_HALF2:
            hle_st32(t, f->s7, hle_dm32(t, 0xd0));
            hle_st32(t, f->s7 + 4, hle_dm32(t, 0xd4));
            hle_st32(t, f->s7 + 8, hle_dm32(t, 0xd8));
            hle_st32(t, f->s7 + 12, w1);
            f->s7 += 16;
            rc = after_emit(f);
            break;
        case K_MOVEWORD:
            addr = (hle_dm8(t, ((w0 >> 16) + 10238) & 0xfff) << 8 | hle_dm8(t, ((w0 >> 16) + 10239) & 0xfff)) + w0;
            hle_st8(t, addr, (uint8_t)(w1 >> 24));
            hle_st8(t, addr + 1, (uint8_t)(w1 >> 16));
            hle_st8(t, addr + 2, (uint8_t)(w1 >> 8));
            hle_st8(t, addr + 3, (uint8_t)w1);
            break;
        case K_MOVEMEM: {
            unsigned dst = hle_dm16(t, 0x2f0 + (w0 & 0xfe)) + (w0 >> 5);
            dma_rd(f, dst, seg_addr(t, w1), (w0 >> 16) & 0xff);
            break;
        }
        case K_OMH:
        case K_OML: {                                   /* the other-mode words at 0xc8 / 0xcc: replace the field (length - 1 in the low bits of w0, shift in bits 8..), then the pair goes to the list */
            unsigned a = kd == K_OMH ? 0xc8 : 0xcc;
            uint32_t mask = (uint32_t)((int32_t)0x80000000 >> (w0 & 31)) >> ((w0 >> 8) & 31);
            hle_st32(t, a, (hle_dm32(t, a) & ~mask) | w1);
            e0 = hle_dm32(t, 0xc8);
            e1 = hle_dm32(t, 0xcc);
            hle_st32(t, f->s7, e0);
            hle_st32(t, f->s7 + 4, e1);
            f->s7 += 8;
            rc = after_emit(f);
            break;
        }
        case K_COPY:
        case K_SYNC:
        case K_SEGCOPY:
        case K_EF:
        case K_ED:
            if (kd == K_SEGCOPY)
                w1 = seg_addr(t, w1);
            else if (kd == K_EF) {
                hle_st32(t, 0xc8, w0);
                hle_st32(t, 0xcc, w1);
            } else if (kd == K_ED) {
                hle_st32(t, 0xc0, w0);
                hle_st32(t, 0xc4, w1);
            }
            if (kd != K_SYNC)
                hle_st32(t, f->s7 + MUT(1, 4, 0), w1);

            hle_st32(t, f->s7, w0);
            f->s7 += 8;
            rc = after_emit(f);
            break;
        case K_VTX:
            rc = cmd_vtx(f);
            break;
        case K_MTX:
            rc = cmd_mtx(f);
            break;
        case K_POPMTX:
            rc = cmd_popmtx(f);
            break;
        case K_TRI1:
            tri_consts(f);
            rc = do_tri(f, w0);
            break;
        case K_TRI2:                                    /* the second triangle first */
            tri_consts(f);
            rc = do_tri(f, w1);
            if (rc == HLE_OK)
                rc = do_tri(f, w0);
            break;
        case K_MVP:
            do_mtx_mul_mvp(f, w0);
            break;
        case K_MODVTX:
            at = ((w0 >> 16) & 0xff) + hle_dm16(t, F2_VTAB + (w0 & 0xfff));       /* (the slot address is the delay slot's lhu) */
            hle_st8(t, at, (uint8_t)(w1 >> 24));
            hle_st8(t, at + 1, (uint8_t)(w1 >> 16));
            hle_st8(t, at + 2, (uint8_t)(w1 >> 8));
            hle_st8(t, at + 3, (uint8_t)w1);
            break;
        case K_CULLDL: {                                /* the display list ends when a clip-flag bit of the reject mask is set in all the vertices of the range */
            unsigned a = hle_dm16(t, F2_VTAB + (w0 & 0xfff)), z = hle_dm16(t, F2_VTAB + (w1 & 0xfff));
            uint32_t m = b->reject_mask;
            for (;;) {
                m &= hle_dm32(t, a + 0x24);
                if (!m)
                    break;
                if (a == z) {
                    rc = do_enddl(f, handover);
                    if (*handover)
                        return rc;
                    break;
                }
                a += 40;
            }
            break;
        }
        case K_BRANCHZ: {
            uint32_t z = hle_dm32(t, hle_dm16(t, F2_VTAB + (w0 & 0xfff)) + 0x1c);
            if ((int32_t)(z - w1) < 0) {                /* a branch (no stack) to the address G_RDPHALF_1 left at 0xd8 */
                f->k0 = seg_addr(t, hle_dm32(t, 0xd8));
                refill(f);
            }
            break;
        }
        default: {
            static char why[8][48];
            static unsigned wn;
            char *w = why[wn++ & 7];
            snprintf(w, sizeof(why[0]), "unsupported command %02x (handler %03x)", w0 >> 24, hle_dm16(t, JUMP_TABLE + 2 * (int)(int8_t)(w0 >> 24)) & 0xfff);
            return hle_decline(t, w);
        }
        }
        if (g_prof) {
            g_pt[kd] += ticks() - t_cmd;
            g_pn[kd]++;
        }
        if (f->watch >= 0 && hle_dm16(t, f->watch) != f->wval) {
            f->wval = hle_dm16(t, f->watch);
            fprintf(stderr, "WATCH %03x = %04x after cmd %u op %02x\n", f->watch, f->wval, f->ncmd - 1, w0 >> 24);
        }
        (void)k;
        if (rc != HLE_OK)
            return rc;
    }
}

static unsigned classify(const struct f3dex2_build *b, unsigned h)
{
    if (h == b->head || h == (unsigned)b->head - 4)
        return K_NOP;
    if (h == b->copy)
        return K_COPY;
    if (h == b->sync)
        return K_SYNC;
    if (h == b->segcopy)
        return K_SEGCOPY;
    if (h == b->ef)
        return K_EF;
    if (h == b->ed)
        return K_ED;
    if (h == b->tri1)
        return K_TRI1;
    if (h == b->tri2)
        return K_TRI2;
    if (h == b->vtx)
        return K_VTX;
    if (h == b->mtx)
        return K_MTX;
    if (h == b->geom)
        return K_GEOM;
    if (h == b->enddl)
        return K_ENDDL;
    if (h == b->half2)
        return K_HALF2;
    if (h == b->modvtx)
        return K_MODVTX;
    if (h == b->culldl)
        return K_CULLDL;
    if (h == b->branchz)
        return K_BRANCHZ;
    if (b->mvp && h == b->mvp)
        return K_MVP;
    switch (h) {
    case PC_DL: return K_DL;
    case PC_TEXTURE: return K_TEXTURE;
    case PC_TEXRECT: return K_TEXRECT;
    case PC_HALF1: return K_HALF1;
    case PC_MOVEWORD: return K_MOVEWORD;
    case PC_POPMTX: return K_POPMTX;
    case PC_MOVEMEM: return K_MOVEMEM;
    case PC_OMH: return K_OMH;
    case PC_OML: return K_OML;
    }
    return K_BAD;
}

static int f3dex2_run(struct hle_task *t, const struct hle_ucode *u)
{
    const struct f3dex2_build *b = (const struct f3dex2_build *)u->fam_data;
    struct hle_cpu c;
    struct f3 f;
    unsigned op;
    int handover = 0, rc;

    if (g_prof < 0) {
        g_prof = getenv("HLE_F3_PROF") != NULL;
        if (g_prof)
            atexit(prof_dump);
    }
    hle_cpu_init(t, &c, t->start_pc, 1u << 22);
    c.stop_pc = b->head + 4;                            /* the first fetch has polled SP_STATUS already */
    rc = hle_rsp_run(t, &c);
    if (rc != HLE_STOP)
        return rc;                                      /* the task ended before the loop, or declined */
    if (*t->cr[4] & SIG0)
        return hle_decline(t, "the task yields");
    memset(&f, 0, sizeof(f));
    f.t = t;
    f.b = b;
    f.k0 = c.r[HLE_REG_K0];
    f.k1 = c.r[HLE_REG_K1];
    f.s6 = c.r[HLE_REG_S6];
    f.s7 = c.r[HLE_REG_S7];
    f.polls = 1;
    f.trace = getenv("HLE_F3_TRACE") != NULL;
    f.watch = getenv("HLE_F3_WATCH") ? (int)strtoul(getenv("HLE_F3_WATCH"), NULL, 16) : -1;
    f.wval = f.watch >= 0 ? hle_dm16(t, f.watch) : 0;
    f.total = 0;
    f.lean = !hle_exact();
    f.ov = overlay_match(t, DESC_CLIP) ? OV_CLIP : overlay_match(t, DESC_LIGHT) ? OV_LIGHT : OV_UNKNOWN;
    f.ov0 = f.ov;
    for (op = 0; op < 256; op++)
        f.kind[op] = (uint8_t)classify(b, hle_dm16(t, JUMP_TABLE + 2 * (int)(int8_t)op) & 0xfff);
    /* the loop head starts at the instruction after the poll: lw t9 */
    rc = run_loop(&f, &handover);
    if (rc != HLE_OK)
        return rc;
    if (getenv("HLE_DUMP_IMEM")) {
        FILE *fp = fopen(getenv("HLE_DUMP_IMEM"), "wb");
        if (fp) {
            fwrite(t->imem, 4, 1024, fp);
            fclose(fp);
        }
    }
    if (f.lean && f.ov != f.ov0 && f.ov != OV_UNKNOWN) {   /* the overlay the microcode would have left in IMEM */
        unsigned desc = f.ov == OV_LIGHT ? DESC_LIGHT : DESC_CLIP;
        hle_dma_read(t, hle_dm16(t, desc + 6), hle_dm32(t, desc), hle_dm16(t, desc + 4));
    }
    /* the outermost G_ENDDL: the microcode's own end. The registers as the loop head leaves them for the handler. */
    c.r[HLE_REG_K0] = f.k0;
    c.r[HLE_REG_K1] = f.k1;
    c.r[HLE_REG_S6] = f.s6;
    c.r[HLE_REG_S7] = f.s7;
    c.r[HLE_REG_AT] = 0;
    c.r[HLE_REG_T9] = f.w0;
    c.r[HLE_REG_T8] = f.w1;
    c.r[HLE_REG_T4] = (uint32_t)((int32_t)f.w0 >> 24);
    c.r[HLE_REG_T3] = b->enddl;
    c.pc = b->enddl;
    c.stop_pc = 0x1000;

    if (g_prof) {
        static unsigned runs;
        if (++runs % 400 == 0)
            prof_dump();
    }
    return hle_rsp_run(t, &c);
}

static const struct hle_family l_fam = { .name = "f3dex2", .run = f3dex2_run };

/* head, copy, sync, segcopy, ef, ed, tri1, tri2, vtx, mtx, geom, enddl, half2, modvtx, culldl, branchz, zex, reject mask */
static const struct f3dex2_build l_b_dk64 = { 0x18c, 0x208, 0x20c, 0x218, 0x238, 0x244, 0x7b4, 0x7ac, 0x4f0, 0x0f0, 0x1d0, 0x1e4, 0x1f8, 0xc78, 0xc38, 0xc60, 0, 0, 0x7070 };
static const struct f3dex2_build l_b_oot = { 0x18c, 0x208, 0x20c, 0x218, 0x238, 0x244, 0x7b4, 0x7ac, 0x4f0, 0x0f0, 0x1d0, 0x1e4, 0x1f8, 0xc74, 0xc34, 0xc5c, 0, 1, 0x7030 };
static const struct f3dex2_build l_b_pm = { 0x194, 0x210, 0x214, 0x220, 0x240, 0x24c, 0xa44, 0xa3c, 0x780, 0x0f0, 0x1d8, 0x1ec, 0x200, 0xf08, 0xec8, 0xef0, 0, 0, 0x7070 };
static const struct f3dex2_build l_b_snap = { 0x194, 0x228, 0x22c, 0x238, 0x258, 0x264, 0x7d4, 0x7cc, 0x510, 0x0f0, 0x1f0, 0x204, 0x218, 0xc98, 0xc58, 0xc80, 0x1c0, 0, 0x7030 };

/* DMEM dead at the end of a task (docs/design.md, "The F3DEX2 table of dead ranges"; docs/testing.md has the evidence): the display list input buffer (0x920 .. 0x9c8: the microcode's last refill;
 * a lean run reads the list from RDRAM), the clipper's polygon lists (0x3ca .. 0x408) and new vertices (12 records from 0x9c8; the clipper makes them anew for every triangle), the two RDP output buffers
 * with their slack (0xba8..0xd00 + 0xb0 slack, 0xdb0..0xf08 + 0xb0): the command handlers write the commands there, the flush DMAs [buffer start, write pointer) to the FIFO, and the bytes beyond the
 * pointer are scratch of the triangle setup (it writes its shade, texture and z blocks whether or not the command has them; their values depend on vector registers left by earlier commands) and the
 * stale second words of sync commands, which the RDP ignores (the verify stream zeroes them). At the end of a task everything below the pointer has been flushed. */
static const struct hle_dead_range l_dead_out[] = { { 0x3ca, 0x408 }, { 0x920, 0xfb8 },
    /* the word at 0xfc8 (OSTask.ucode_boot): the initialisation stores register a0 there (sw a0, 0xfc8), which holds whatever the previous task left in it; nothing reads it */
    { 0xfc8, 0xfcc },
    /* the padding byte of each colour of the light slots (struct Light_t: col[3] pad, colc[3] pad, dir[3] pad; 24 bytes from 0x200): the lighting code stores whole vector lanes and the
     * pad lane holds what the vector registers had, which depends on the commands before; nothing reads the pads. */
    { 0x203, 0x204 }, { 0x207, 0x208 }, { 0x21b, 0x21c }, { 0x21f, 0x220 }, { 0x233, 0x234 }, { 0x237, 0x238 }, { 0x24b, 0x24c }, { 0x24f, 0x250 },
    { 0x263, 0x264 }, { 0x267, 0x268 }, { 0x27b, 0x27c }, { 0x27f, 0x280 }, { 0x293, 0x294 }, { 0x297, 0x298 }, { 0x2ab, 0x2ac }, { 0x2af, 0x2b0 }, { 0x2c3, 0x2c4 },
    { 0x2c7, 0x2c8 } };

#define DEAD .ring_off = 0xfe8, .dead = l_dead_out, .n_dead = sizeof(l_dead_out) / sizeof(l_dead_out[0])
HLE_UCODE(l_dk64) = { .id = 0x48c7e653a6adcda8ull, .name = "F3DEX 2.07 (Donkey Kong 64)", .fam = &l_fam, .flags = HLE_F_NEAR_CLIP, .fam_data = &l_b_dk64, DEAD };
HLE_UCODE(l_snap) = { .id = 0xd576cfcfed9d9c18ull, .name = "F3DEX.NoN 2.08H (Pokemon Snap)", .fam = &l_fam, .fam_data = &l_b_snap, DEAD };
HLE_UCODE(l_pm) = { .id = 0xc734403d56034719ull, .name = "F3DEX 2.08 (Paper Mario)", .fam = &l_fam, .flags = HLE_F_NEAR_CLIP, .fam_data = &l_b_pm, DEAD };
HLE_UCODE(l_oot) = { .id = 0x0d3f849c8dd309c4ull, .name = "F3DZEX.NoN 2.06H (Ocarina of Time)", .fam = &l_fam, .flags = HLE_F_ZEX, .fam_data = &l_b_oot, DEAD };

#endif
