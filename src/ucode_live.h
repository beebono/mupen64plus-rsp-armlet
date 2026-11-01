/*
 * arMLEt - ucode_live.h: the live part of a graphics ucode's text (ucode_live.c)
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ARMLET_UCODE_LIVE_H
#define ARMLET_UCODE_LIVE_H

#include <stdint.h>

#define ARMLET_LIVE_MAX 24

struct armlet_live {
    unsigned n;
    struct { uint32_t lo, hi; } r[ARMLET_LIVE_MAX];     /* text offsets [lo, hi) within the 4 KiB the id covers, sorted and merged */
    uint32_t code_end;                                  /* no overlay table: where the boot range was cut (the code's end); 0 when it was not cut */
    unsigned nov;
    struct { uint32_t off, imem, len; } ov[ARMLET_LIVE_MAX];   /* the overlay descriptors found (tools/ucode_live_check.c maps IMEM back to the text with them) */
};

/* The text bytes a task of the ucode at ta (data image at da, dlen bytes; boot ucode at ba, bsz bytes) can execute: the boot load (text 0 .. 0xf80 to IMEM 0x80) and the sources of the data's
 * overlay descriptors that lie in the 4 KiB; without any descriptor the boot range ends at the code's end, or the whole 4 KiB is live when the code fills it or the boot
 * is not rspboot. rdram is the core's RDRAM (host-order 32-bit words). */
void armlet_ucode_live(const unsigned char *rdram, uint32_t ta, uint32_t da, uint32_t dlen, uint32_t ba, uint32_t bsz, struct armlet_live *o);

/* FNV-1a-64 over the ranges and their words: equal for texts that differ only in words no task can execute */
uint64_t armlet_ucode_live_sig(const unsigned char *rdram, uint32_t ta, const struct armlet_live *l);

#endif
