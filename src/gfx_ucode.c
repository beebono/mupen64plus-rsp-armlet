/*
 * arMLEt - gfx_ucode.c
 *
 * The ucode id is a serial multiply chain, so there is no NEON form of it; what makes it cheap is that it is only
 * recomputed when a 4 KiB memcmp (libc, vectorised) of the ucode text against the last copies says the text changed.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <inttypes.h>
#include <string.h>

#include "m64p_types.h"

#include "gfx_ucode.h"
#include "hle/hle.h"

#define RDRAM_MASK 0x7fffff
#define TEXT_MAX 0x1000
#define SLOTS 4
#define SEEN_MAX 64

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define BYTE_SWZ 0
#else
#define BYTE_SWZ 3
#endif

/* the task-level native path (src/hle) answers for the ucodes that have an hle_ucode entry; none other here */
static const struct armlet_gfx_native l_native[] = {
    { 0, NULL }
};

static struct slot {
    bool used;
    uint32_t addr, size;
    unsigned char text[TEXT_MAX];
    struct armlet_ucode_id u;
} l_slots[SLOTS];
static unsigned l_next_slot;
static uint64_t l_seen[SEEN_MAX];
static unsigned l_nseen;

static inline uint32_t rd32(const unsigned char *rdram, uint32_t a)
{
    return *(const uint32_t *)(rdram + (a & RDRAM_MASK & ~3u));
}

uint64_t armlet_ucode_hash(const unsigned char *rdram, uint32_t addr, uint32_t bytes)
{
    uint64_t h = 0xcbf29ce484222325ull;
    uint32_t i;
    for (i = 0; i < bytes; i += 4)
        h = (h ^ rd32(rdram, addr + i)) * 0x100000001b3ull;
    return h;
}

/* printable runs of >= 10 characters that look like a version banner, blanks squeezed, joined with " | " */
static void scan_strings(const unsigned char *rdram, uint32_t a, uint32_t bytes, char *out, size_t outsz)
{
    static const char *const keys[] = { "RSP", "ucode", "F3D", "Gfx", "Version", "SW", "Nintendo", "fifo" };
    size_t o = strlen(out);
    char run[128];
    unsigned n = 0, i, k;

    for (i = 0; i <= bytes; i++) {
        unsigned ch = i < bytes ? rdram[((a + i) & RDRAM_MASK) ^ BYTE_SWZ] : 0;
        if (i < bytes && ch >= 0x20 && ch < 0x7f) {
            if (n < sizeof(run) - 1)
                run[n++] = (char)ch;
            continue;
        }
        if (n >= 10) {
            run[n] = 0;
            for (k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
                if (strstr(run, keys[k]))
                    break;
            if (k < sizeof(keys) / sizeof(keys[0])) {
                char sq[128];
                unsigned m = 0, j;
                for (j = 0; j < n && m < sizeof(sq) - 1; j++)
                    if (!(run[j] == ' ' && m && sq[m - 1] == ' '))
                        sq[m++] = run[j];
                sq[m] = 0;
                if (o + m + 4 < outsz) {
                    if (o) {
                        out[o++] = ' ';
                        out[o++] = '|';
                        out[o++] = ' ';
                    }
                    memcpy(out + o, sq, m + 1);
                    o += m;
                }
            }
        }
        n = 0;
    }
}

/* "RSP Gfx ucode F3DEX fifo 2.07 ...": 2.x is the F3DEX2 family (F3DZEX, F3DLX2, L3DEX2), 1.x the first F3DEX / F3DLX generation */
static unsigned family_of(const char *name)
{
    const char *g = strstr(name, "fifo ");
    if (!g)
        g = strstr(name, "xbus ");
    if (g && (strstr(name, "F3D") || strstr(name, "L3D"))) {
        const char *v = g + 5;
        while (*v == ' ')
            v++;
        if (*v == '2')
            return ARMLET_FAM_F3DEX2;
        if (*v == '1')
            return ARMLET_FAM_F3DEX;
    }
    if (strstr(name, "F3DEX2") || strstr(name, "F3DZEX") || strstr(name, "F3DLX2"))
        return ARMLET_FAM_F3DEX2;
    if (strstr(name, "F3DEX") || strstr(name, "F3DLX") || strstr(name, "F3DLP"))
        return ARMLET_FAM_F3DEX;
    if (strstr(name, "F3D") || strstr(name, "SW Version"))
        return ARMLET_FAM_F3D;
    return ARMLET_FAM_OTHER;
}

static bool seen_add(uint64_t id)
{
    unsigned i;
    for (i = 0; i < l_nseen; i++)
        if (l_seen[i] == id)
            return false;
    if (l_nseen < SEEN_MAX)
        l_seen[l_nseen++] = id;
    return true;
}

const struct armlet_ucode_id *armlet_gfx_identify(const struct armlet_ctx *c, bool *is_new)
{
    const unsigned char *rdram = c->rsp.RDRAM;
    uint32_t addr = armlet_dmem32(c, ARMLET_TASK_UCODE) & RDRAM_MASK & ~3u;
    uint32_t size = armlet_dmem32(c, ARMLET_TASK_UCODE_SIZE);
    uint32_t data = armlet_dmem32(c, ARMLET_TASK_UCODE_DATA);
    uint32_t dsize = armlet_dmem32(c, ARMLET_TASK_UCODE_DATA_SIZE);
    unsigned char wrapped[TEXT_MAX];
    const unsigned char *text;
    struct slot *s = NULL;
    unsigned i;

    /* many games leave ucode_size 0 (their boot code loads 0xf80 bytes regardless) */
    if (!size)
        size = 0xf80;
    if (size > TEXT_MAX)
        size = TEXT_MAX;
    size = (size + 3) & ~3u;
    if (addr + size <= RDRAM_MASK + 1) {
        text = rdram + addr;
    } else {
        for (i = 0; i < size; i += 4)
            memcpy(wrapped + i, rdram + ((addr + i) & RDRAM_MASK), 4);
        text = wrapped;
    }

    for (i = 0; i < SLOTS; i++)
        if (l_slots[i].used && l_slots[i].addr == addr && l_slots[i].size == size) {
            if (!memcmp(l_slots[i].text, text, size)) {
                *is_new = false;
                return &l_slots[i].u;
            }
            s = &l_slots[i];                    /* same place, new text */
            break;
        }
    if (!s) {
        s = &l_slots[l_next_slot];
        l_next_slot = (l_next_slot + 1) % SLOTS;
    }

    s->used = true;
    s->addr = addr;
    s->size = size;
    memcpy(s->text, text, size);
    s->u.id = armlet_ucode_hash(rdram, addr, size);
    s->u.name[0] = 0;
    scan_strings(rdram, addr, size, s->u.name, sizeof(s->u.name));
    scan_strings(rdram, data, dsize > 0x800 ? 0x800 : dsize, s->u.name, sizeof(s->u.name));
    s->u.family = family_of(s->u.name);
    *is_new = seen_add(s->u.id);
    return &s->u;
}

const struct armlet_gfx_native *armlet_gfx_native_find(uint64_t id)
{
    const struct armlet_gfx_native *n;
#ifdef ARMLET_NATIVE
    static struct armlet_gfx_native hle;
    if (armlet_hle_find(id)) {
        hle.id = id;
        hle.run = armlet_hle_run;
        return &hle;
    }
#endif
    for (n = l_native; n->run; n++)
        if (n->id == id)
            return n;
    return NULL;
}

bool armlet_gfx_allowed(const struct armlet_config *cfg, uint64_t id)
{
    unsigned i;
    if (!cfg->n_allow)
        return true;
    for (i = 0; i < cfg->n_allow; i++)
        if (cfg->allow[i] == id)
            return true;
    return false;
}

void armlet_gfx_reset(void)
{
    memset(l_slots, 0, sizeof(l_slots));
    l_next_slot = 0;
    l_nseen = 0;
}
