/*
 * arMLEt - ucode_live.c: the live part of a graphics ucode's text (docs/design.md, "Texts alike")
 *
 * Some games link variables right behind the code, inside the 4 KiB the ucode id covers (Heiwa Pachinko World 64, Super Robot Spirits, Blast Corps): a new id per frame for
 * one build. A text reaches IMEM through the boot ucode (libultra's rspboot: text 0 .. 0xf80 to IMEM 0x80, whatever ucode_size says; any other boot keeps all 4 KiB live)
 * and through the ucode's own overlay DMAs, mostly from descriptors in its data image. With a descriptor the live text is the boot range plus the descriptors' sources.
 * Without one the boot range is cut at the code's end (the furthest address a jump, branch, pc literal or data-image halfword names, then the fall-through up to the next
 * unconditional jump and its delay slot), unless the code reaches the boot range's end (Rogue Squadron, Gauntlet Legends load their tails without a table) or a text offset
 * behind that end appears in the data or the code (Glover): then all 4 KiB stay. Every guess widens the range. Checks: tools/ucode_live_check.c on the survey dumps,
 * RSPT_DEADTEXT in tools/equiv/rsp_shim.c (docs/testing.md, "Live text"); the shim builds this file too.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>

#include "ucode_live.h"

#define BOOT_LEN 0xf80u
#define WIN 0x1000u                                     /* the 4 KiB the ucode id covers; overlay code behind it is no more part of the signature than of the id */

#ifndef ULIVE_MUT
#define ULIVE_MUT 0                                     /* test builds of the shim (tools/equiv, RSPT_DEADTEXT): 1 no fall-through past the furthest target, 2 no overlay sources */
#endif

static inline uint32_t rd32(const unsigned char *rdram, uint32_t a)
{
    return *(const uint32_t *)(rdram + (a & 0x7ffffc));
}

static void add_ov(struct armlet_live *o, uint32_t off, uint32_t im, uint32_t ln)
{
    if (o->nov < ARMLET_LIVE_MAX) {
        o->ov[o->nov].off = off;
        o->ov[o->nov].imem = im;
        o->ov[o->nov++].len = ln;
    }
}

static void add(struct armlet_live *o, uint32_t lo, uint32_t hi)
{
    if (hi > WIN)
        hi = WIN;
    if (o->n == ARMLET_LIVE_MAX) {                      /* (more ranges than the table holds: all of it is live) */
        o->n = 1;
        o->r[0].lo = 0;
        o->r[0].hi = WIN;
    }
    if (o->n == 1 && o->r[0].lo == 0 && o->r[0].hi == WIN)
        return;
    if (lo < hi) {
        o->r[o->n].lo = lo;
        o->r[o->n++].hi = hi;
    }
}

static int cmp_lo(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* an IMEM pc a halfword or an immediate may stand for (0x1000 | pc is an overlay window address) */
static inline void pc_cand(uint32_t v, uint32_t *top)
{
    if (v < 0x2000 && !(v & 3) && (v & 0xfff) >= 0x80 && (v & 0xfff) > *top)
        *top = v & 0xfff;
}

/* the boot code is libultra's rspboot (a few hundred bytes that DMA text 0 .. 0xf80 to IMEM 0x80: SP_MEM_ADDR 0x1080, length 0xf7f); Indiana Jones names its 4 KiB text
 * as the boot, which then runs from IMEM 0 */
static int rspboot(const unsigned char *rdram, uint32_t ba, uint32_t bsz)
{
    uint32_t i, w, f = 0;
    if (!bsz || bsz > 0x400)
        return 0;
    for (i = 0; i < bsz; i += 4) {
        w = rd32(rdram, ba + i);
        if (((w >> 26) == 8 || (w >> 26) == 9 || (w >> 26) == 13) && !((w >> 21) & 31))
            f |= (w & 0xffff) == 0x1080 ? 1 : (w & 0xffff) == 0xf7f ? 2 : 0;
    }
    return f == 3;
}

/* the code's end in the boot range (text offset), for a text without overlay table */
static uint32_t code_end(const unsigned char *rdram, uint32_t ta, uint32_t da, uint32_t dlen)
{
    uint32_t top = 0x80, i, w, pc, end;
    for (i = 0; i < BOOT_LEN / 4; i++) {
        unsigned op, rs, rt;
        int32_t imm;
        w = rd32(rdram, ta + 4 * i);
        pc = 0x80 + 4 * i;
        op = w >> 26;
        rs = (w >> 21) & 31;
        rt = (w >> 16) & 31;
        imm = (int16_t)(w & 0xffff);
        if (op == 2 || op == 3)                         /* j, jal */
            pc_cand((w & 0x3ffffff) << 2 & 0xfff, &top);
        else if ((op >= 4 && op <= 7) || (op == 1 && (rt & 0xe) == 0))         /* beq bne blez bgtz; bltz bgez bltzal bgezal */
            pc_cand((pc + 4 + ((uint32_t)imm << 2)) & 0xfff, &top);
        else if ((op == 8 || op == 9 || op == 13) && rs == 0)          /* addi / addiu / ori rX, zero, pc: return addresses, overlay entries */
            pc_cand(w & 0xffff, &top);
    }
    for (i = 0; i + 4 <= dlen && i < 0x1000; i += 4) {   /* the jump tables (and every constant that looks like a pc) */
        w = rd32(rdram, da + i);
        pc_cand(w >> 16, &top);
        pc_cand(w & 0xffff, &top);
    }
    if (ULIVE_MUT == 1)
        return top - 0x80 + 4;
    for (i = (top - 0x80) / 4; i < BOOT_LEN / 4; i++) {
        unsigned op, f;
        w = rd32(rdram, ta + 4 * i);
        op = w >> 26;
        f = w & 63;
        if (op == 2 || (op == 0 && (f == 8 || f == 13)) || (op == 4 && ((w >> 21) & 31) == ((w >> 16) & 31)))   /* j, jr, break, beq rX, rX */
            break;
    }
    if (i == BOOT_LEN / 4)
        return BOOT_LEN;
    end = 4 * i + 8 <= BOOT_LEN ? 4 * i + 8 : BOOT_LEN;
    /* a text offset behind the end in the data or an immediate of the code: an overlay of its own text without a descriptor (Glover's 2.0H text loads text 0xf70 .. to IMEM
     * 0x150, its code has 0xf90): all of it stays */
    for (i = 0; i < end / 4; i++) {
        w = rd32(rdram, ta + 4 * i);
        if (((w >> 26) == 8 || (w >> 26) == 9 || (w >> 26) == 13) && (w & 0xffff) >= end && (w & 0xffff) < WIN && !(w & 7))
            return BOOT_LEN;
    }
    for (i = 0; i + 4 <= dlen && i < 0x1000; i += 4) {
        w = rd32(rdram, da + i);
        if (((w >> 16) >= end && (w >> 16) < WIN && !((w >> 16) & 7)) || ((w & 0xffff) >= end && (w & 0xffff) < WIN && !(w & 7)))
            return BOOT_LEN;
    }
    return end;
}

void armlet_ucode_live(const unsigned char *rdram, uint32_t ta, uint32_t da, uint32_t dlen, uint32_t ba, uint32_t bsz, struct armlet_live *o)
{
    uint32_t a, k, j;
    unsigned nov = 0;
    o->n = o->nov = 0;
    o->code_end = 0;
    ta &= 0x7ffffc;
    da &= 0x7ffffc;
    if (dlen > 0x1000)
        dlen = 0x1000;
    if (!rspboot(rdram, ba & 0x7ffffc, bsz)) {
        add(o, 0, WIN);                                 /* (another boot: where the text goes is not known) */
        return;
    }
    /* overlay descriptors (text offset, length - 1 << 16 | 0x1000 | IMEM address; a DMA moves multiples of 8 bytes) anywhere in the image: Fast3D's and F3DEX 1.x's table at DMEM 0 (its first entry the
     * resident code), F3DEX2's (the exit code's at IMEM 0, its tail at text + 0x1000), S2DEX's. Looser than derive.c's img_unknown: a false one only widens the range */
    for (a = 0; a + 8 <= dlen; a += 4) {
        uint32_t off = rd32(rdram, da + a), b = rd32(rdram, da + a + 4), ln = (b >> 16) + 1, im = b & 0xfff;
        if ((b & 0xf000) != 0x1000 || (ln & 7) || ln > 0x1000 || ln < 8 || (off & 7) || (im & 7) || off >= 0x3000 || im + ln > 0x1000)
            continue;
        if (ULIVE_MUT != 2)
            add(o, off, off + ln);
        add_ov(o, off, im, ln);
        nov++;
    }
    if (nov)
        add(o, 0, BOOT_LEN);
    else if ((o->code_end = code_end(rdram, ta, da, dlen)) < BOOT_LEN)
        add(o, 0, o->code_end);
    else
        add(o, 0, WIN);                                 /* (code up to the boot range's end and no descriptor: the tail may be loaded some other way, Rogue Squadron's is) */
    qsort(o->r, o->n, sizeof(o->r[0]), cmp_lo);
    for (k = j = 0; k < o->n; k++) {
        if (j && o->r[k].lo <= o->r[j - 1].hi) {
            if (o->r[k].hi > o->r[j - 1].hi)
                o->r[j - 1].hi = o->r[k].hi;
        } else
            o->r[j++] = o->r[k];
    }
    o->n = j;
}

uint64_t armlet_ucode_live_sig(const unsigned char *rdram, uint32_t ta, const struct armlet_live *l)
{
    uint64_t x = 0xcbf29ce484222325ull;
    unsigned k;
    uint32_t a;
    ta &= 0x7ffffc;
    for (k = 0; k < l->n; k++) {
        x = (x ^ l->r[k].lo) * 0x100000001b3ull;
        x = (x ^ l->r[k].hi) * 0x100000001b3ull;
        for (a = l->r[k].lo & ~3u; a < l->r[k].hi; a += 4)
            x = (x ^ rd32(rdram, ta + a)) * 0x100000001b3ull;
    }
    return x;
}
