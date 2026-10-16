/*
 * arMLEt - hle_f3dex2_int.h: what the F3DEX2 front-end (hle_f3dex2.c) shares with its dialects (hle_f3dex2_dialects.c, hle_s2dex2.c): the build tables' type, the command kinds, the
 * task state and the few helpers the dialects' handlers call
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ARMLET_HLE_F3DEX2_INT_H
#define ARMLET_HLE_F3DEX2_INT_H

#include <stdint.h>

#include "f3dex2_dmem.h"
#include "f3dex2_fx.h"
#include "hle.h"

/* test builds only (scripts/build.sh native --tools --hmut=N, tools/hle/mutcheck.sh): deliberate errors, to show that the tests catch them (docs/testing.md) */
#ifdef ARMLET_HLE_MUTATE
#define MUT(n, ok, bad) (ARMLET_HLE_MUTATE == (n) ? (bad) : (ok))
#else
#define MUT(n, ok, bad) (ok)
#endif

struct f3;
struct f2_pcs;
struct f3dex2_build {
    uint16_t head;                  /* the command loop: mfc0 at (the fetch of the next command) */
    uint16_t copy, sync, segcopy;   /* handlers: copy a command to the RDP buffer / copy only its first word / resolve the segment address, then copy */
    uint16_t ef, ed;                /* G_RDPSETOTHERMODE and G_SETSCISSOR: remember the command, then copy */
    uint16_t tri1, tri2;            /* G_TRI1; G_TRI2 and G_QUAD */
    uint16_t vtx, mtx, geom, enddl, half2, modvtx, culldl, branchz;
    uint16_t mvp;                   /* a handler that concatenates the matrices now (G_SPECIAL_1 of F3DEX.NoN 2.08H), or 0 */
    unsigned zex;                   /* F3DZEX: other lanes of the constant vectors, no mask on the slope fractions */
    uint32_t reject_mask;           /* a triangle whose three clip-flag words have a common bit in this mask is rejected */
    uint16_t tri4;                  /* G_TRI4 (F3DEXBG): four triangles in one command, or 0 */
    const struct f2_layout *lay;    /* the DMEM layout when it is not the standard one */
    const struct f2_pcs *pcs;       /* the handlers of the commands below 0x170 when they are not at the standard pcs (S2DEX), or NULL */
    const uint16_t (*ext)[2];       /* dialect commands: { handler pc, kind (>= K_EXT) } pairs ending in { 0xffff, 0 }, run by ext_cmd */
    int (*ext_cmd)(struct f3 *f, unsigned kind);
};

/* The handlers below 0x170 sit at the same pcs in all the builds: the display list stack, the state words the other handlers read, the other-mode registers */
enum { PC_DL = 0x000, PC_TEXTURE = 0x028, PC_TEXRECT = 0x02c, PC_HALF1 = 0x030, PC_MOVEWORD = 0x038, PC_POPMTX = 0x04c, PC_MOVEMEM = 0x120, PC_OMH = 0x13c, PC_OML = 0x140 };

/* the handler pcs of the commands the standard builds share (PC_*), for a build that has them elsewhere; 0xffff: the build has no such command */
struct f2_pcs {
    uint16_t dl, texture, texrect, half1, moveword, popmtx, movemem, omh, oml;
};
enum { K_EXT = 0x40 };                                  /* kinds from here on belong to a dialect (ext_cmd) */

enum { K_BAD = 0, K_NOP, K_DL, K_ENDDL, K_TEXTURE, K_TEXRECT, K_HALF1, K_HALF2, K_MOVEWORD, K_MOVEMEM, K_OMH, K_OML, K_GEOM, K_COPY, K_SYNC, K_SEGCOPY, K_EF, K_ED, K_VTX, K_MTX, K_POPMTX,
       K_TRI1, K_TRI2, K_MODVTX, K_CULLDL, K_BRANCHZ, K_MVP, K_TRI4, K_LOADUCODE };

/* DMEM layout (the same in all the builds): jump table at 0x36e by opcode; segments at 0xf8; RDP FIFO pointer 0xf0, FIFO bounds 0xfe8 / 0xfec; the display list stack depth at 222 (x4) and
 * the stack at 0x138; the RDP buffers: two of 344 bytes, s7 the write pointer, s6 the end of the current one (xor 0x208 toggles) */
enum { SEGMENTS = 0xf8, FIFO_PTR = 0xf0, FIFO_START = 0xfe8, FIFO_END = 0xfec, DL_DEPTH = 222, DL_STACK = 0x138,
       SIG0 = 0x80, DPC_BUSY_BIT = 0x400, POLL_LIMIT = 30000 };

enum { OV_CLIP = 0, OV_LIGHT = 1, OV_UNKNOWN = 2 };
#define JUMP_TABLE (f2L->jump_table)
#define OUT_BYTES (f2L->out_bytes)
#define OUT_TOGGLE (f2L->out_toggle)
#define DESC_LIGHT (f2L->desc_light)
#define DESC_CLIP (f2L->desc_clip)

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
    uint32_t uc0;                   /* OSTask.ucode at the start (the text this run is for) */
    unsigned ho_pc;                 /* where the interpreter takes over when the loop hands the task back: the G_ENDDL handler, or 0xfac (G_LOAD_UCODE) */
    uint16_t wval;
    unsigned ncmd;
    uint32_t total;
};

/* ---- shared with the dialects ------------------------------------------------------------------------------------------------------------------------------------------------ */
extern const struct hle_family f3dex2_fam;

/* the dead ranges of the standard builds (the dialects that keep the standard buffers use them too) */
extern const struct hle_dead_range f3dex2_dead_out[];
enum { F3DEX2_N_DEAD_OUT = 21 };
#define F3DEX2_DEAD .ring_off = 0xfe8, .dead = f3dex2_dead_out, .n_dead = F3DEX2_N_DEAD_OUT

int f3_flush(struct f3 *f);
void f3_dma_rd(struct f3 *f, unsigned mem, uint32_t dram, uint32_t len);
void f3_overlay_load(struct f3 *f, int which);
void f3_do_dl(struct f3 *f, uint32_t w0, uint32_t w1);

/* after a command that wrote to the buffer */
static inline int after_emit(struct f3 *f)
{
    if ((int32_t)(f->s7 - f->s6) > 0)
        return f3_flush(f);
    return HLE_OK;
}

static inline uint32_t seg_addr(const struct hle_task *t, uint32_t w) { return (w & 0xffffff) + hle_dm32(t, SEGMENTS + ((w >> 22) & 0x3c)); }

#endif
