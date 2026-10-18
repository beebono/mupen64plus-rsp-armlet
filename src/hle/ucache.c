/*
 * arMLEt - ucache.c: see ucache.h
 *
 * File layout: a 32-byte header { "ARMLETUC", u32 byte-order mark 0x01020304, u32 format, u32 tool version, u32 entry count, u32 entry size, u32 header crc }, then the entries.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "derive.h"
#include "ucache.h"

/* test builds (cc -DARMLET_HLE_MUTATE=N tools/hle/test_ucache.c, tools/hle/mutcheck_ucache.sh): a deliberate error N that tools/hle/test_ucache.c must catch */
#ifdef ARMLET_HLE_MUTATE
#define UMUT(n, ok, bad) (ARMLET_HLE_MUTATE == (n) ? (bad) : (ok))
#else
#define UMUT(n, ok, bad) (ok)
#endif

struct hdr {
    char magic[8];
    uint32_t bom, format, tool, count, esize, crc;
};

static pthread_mutex_t l_mu = PTHREAD_MUTEX_INITIALIZER;    /* the validator thread stores latch states while the emulation thread looks entries up */

static struct {
    char path[1024];
    struct ucache_entry *e;
    unsigned n;
    uint32_t seq;
    int open;
    uint32_t tool;
} U;

uint32_t ucache_crc32(const void *p, size_t n)
{
    static uint32_t tab[256];
    uint32_t c = 0xffffffffu;
    const uint8_t *b = (const uint8_t *)p;
    size_t i;
    if (!tab[1]) {
        uint32_t k, j;
        for (k = 0; k < 256; k++) {
            uint32_t x = k;
            for (j = 0; j < 8; j++)
                x = x & 1 ? 0xedb88320u ^ (x >> 1) : x >> 1;
            tab[k] = x;
        }
    }
    for (i = 0; i < n; i++)
        c = tab[(c ^ b[i]) & 0xff] ^ (c >> 8);
    return ~c;
}

uint32_t ucache_tool_version(void)
{
    /* the reference data is part of the version: a new registered build, or a regenerated file, changes what a derivation can say */
    uint32_t sig;
    if (!U.tool) {
        const struct derive_ref *last = &derive_refs[derive_n_refs - 1];
        sig = ucache_crc32(derive_words, ((size_t)last->woff + last->n) * 4);
        U.tool = sig ^ (uint32_t)(DERIVE_TOOL_VERSION * 0x9e3779b1u) ^ (uint32_t)(UCACHE_FORMAT * 0x85ebca6bu) ^ derive_gen_version;
        if (!U.tool)
            U.tool = 1;
    }
    return U.tool;
}

static uint32_t entry_crc(const struct ucache_entry *e) { return ucache_crc32(e, offsetof(struct ucache_entry, crc)); }

/* the key: text id, data id and the tier (the loose section is not the strict one) */
static int same_key(const struct ucache_entry *a, uint64_t text_id, uint64_t data_id, unsigned tier)
{
    return a->text_id == text_id && a->data_id == data_id && UMUT(454, a->tier == tier, 1);
}

/* two versions of one key: the newer (seq) wins, but a permanent unlatch and the strikes are kept from either */
static void merge(struct ucache_entry *keep, const struct ucache_entry *other)
{
    int dead = keep->latch == UC_LATCH_DEAD || other->latch == UC_LATCH_DEAD;
    uint8_t strikes = keep->strikes > other->strikes ? keep->strikes : other->strikes;
    if (other->seq > keep->seq)
        *keep = *other;
    if (UMUT(442, dead, 0))
        keep->latch = UC_LATCH_DEAD;
    keep->strikes = strikes;
}

const char *ucache_path(void) { return U.open ? U.path : ""; }

/* reads the file at U.path, merging its good entries into the table (the one with the higher seq wins for a key) */
static void load_file(void)
{
    FILE *f;
    struct hdr h;
    struct ucache_entry e;
    unsigned i;
    f = fopen(U.path, "rb");
    if (!f)
        return;
    if (fread(&h, sizeof(h), 1, f) != 1 || memcmp(h.magic, "ARMLETUC", 8) || h.bom != 0x01020304u || h.format != UCACHE_FORMAT || h.esize != sizeof(e) ||
        h.crc != ucache_crc32(&h, offsetof(struct hdr, crc))) {
        fclose(f);
        return;
    }
    for (i = 0; i < h.count; i++) {
        unsigned q;
        if (fread(&e, sizeof(e), 1, f) != 1)
            break;                                         /* truncated: the entries before it stand */
        if ((UMUT(345, e.crc != entry_crc(&e), 0)) || UMUT(347, e.tool != ucache_tool_version(), 0))
            continue;                                      /* damaged, or made by another version of the tool */
        for (q = 0; q < U.n; q++)
            if (same_key(&U.e[q], e.text_id, e.data_id, e.tier))
                break;
        if (q < U.n) {
            merge(&U.e[q], &e);
            continue;
        }
        if (U.n < UMUT(346, UCACHE_MAX_ENTRIES, 100000u)) {
            U.e = (struct ucache_entry *)realloc(U.e, (U.n + 1) * sizeof(e));
            if (!U.e) {
                U.n = 0;
                break;
            }
            U.e[U.n++] = e;
        }
        if (e.seq > U.seq)
            U.seq = e.seq;
    }
    fclose(f);
}

int ucache_open(const char *path)
{
    int n;
    ucache_close();
    pthread_mutex_lock(&l_mu);
    memset(&U, 0, sizeof(U));
    if (!path || !*path || strlen(path) >= sizeof(U.path) - 16) {
        pthread_mutex_unlock(&l_mu);
        return 0;
    }
    snprintf(U.path, sizeof(U.path), "%s", path);
    U.open = 1;
    load_file();
    n = (int)U.n;
    pthread_mutex_unlock(&l_mu);
    return n;
}

void ucache_close(void)
{
    pthread_mutex_lock(&l_mu);
    free(U.e);
    memset(&U, 0, sizeof(U));
    pthread_mutex_unlock(&l_mu);
}

int ucache_get_t(uint64_t text_id, uint64_t data_id, unsigned tier, struct ucache_entry *out)
{
    const struct ucache_entry *e;
    int rc = -1;
    pthread_mutex_lock(&l_mu);
    e = ucache_find_t(text_id, data_id, tier);
    if (e) {
        *out = *e;
        rc = 0;
    }
    pthread_mutex_unlock(&l_mu);
    return rc;
}

int ucache_get(uint64_t text_id, uint64_t data_id, struct ucache_entry *out) { return ucache_get_t(text_id, data_id, UC_TIER_STRICT, out); }

const struct ucache_entry *ucache_find_t(uint64_t text_id, uint64_t data_id, unsigned tier)
{
    unsigned i;
    for (i = 0; i < U.n; i++)
        if (same_key(&U.e[i], text_id, data_id, tier))
            return &U.e[i];
    return NULL;
}

const struct ucache_entry *ucache_find(uint64_t text_id, uint64_t data_id) { return ucache_find_t(text_id, data_id, UC_TIER_STRICT); }

static int write_file(void)
{
    char tmp[1100];
    FILE *f;
    struct hdr h;
    unsigned i;
    int ok = 1;
    snprintf(tmp, sizeof(tmp), "%s.tmp%ld", U.path, (long)getpid());
    if (UMUT(349, 0, 1))
        snprintf(tmp, sizeof(tmp), "%s", U.path);              /* (not atomic: the file is rewritten in place) */
    f = fopen(tmp, "wb");
    if (!f && errno == ENOENT) {                           /* the directory of the file does not exist yet */
        char dir[1100];
        char *sl;
        snprintf(dir, sizeof(dir), "%s", U.path);
        sl = strrchr(dir, '/');
        if (sl && sl != dir) {
            *sl = 0;
            if (!mkdir(dir, 0777))
                f = fopen(tmp, "wb");
        }
    }
    if (!f)
        return -1;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "ARMLETUC", 8);
    h.bom = 0x01020304u;
    h.format = UCACHE_FORMAT;
    h.tool = ucache_tool_version();
    h.count = U.n;
    h.esize = sizeof(struct ucache_entry);
    h.crc = ucache_crc32(&h, offsetof(struct hdr, crc));
    ok &= fwrite(&h, sizeof(h), 1, f) == 1;
    for (i = 0; i < U.n && ok; i++) {
        U.e[i].crc = entry_crc(&U.e[i]);              /* (a merge may have changed an entry) */
        ok &= fwrite(&U.e[i], sizeof(U.e[i]), 1, f) == 1;
    }
    ok &= fflush(f) == 0;
    if (ok)
        (void)fsync(fileno(f));
    ok &= fclose(f) == 0;
    if (UMUT(349, 0, 1))
        return ok ? 0 : -1;
    if (!ok || rename(tmp, U.path)) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int store_locked(const struct ucache_entry *src);

int ucache_store(const struct ucache_entry *src)
{
    int rc;
    pthread_mutex_lock(&l_mu);
    rc = store_locked(src);
    pthread_mutex_unlock(&l_mu);
    return rc;
}

static int store_locked(const struct ucache_entry *src)
{
    struct ucache_entry e = *src;
    unsigned i, q;
    if (!U.open)
        return 0;
    load_file();                                           /* another process may have added entries since: merge before writing */
    q = U.n;
    e.tool = ucache_tool_version();
    e.seq = ++U.seq;
    for (i = 0; i < U.n; i++)
        if (same_key(&U.e[i], e.text_id, e.data_id, e.tier)) {
            q = i;
            break;
        }
    if (q < U.n) {                                         /* (an unlatch for good stays, whoever stores later) */
        if (UMUT(442, U.e[q].latch == UC_LATCH_DEAD, 0))
            e.latch = UC_LATCH_DEAD;
        if (U.e[q].strikes > e.strikes)
            e.strikes = U.e[q].strikes;
    }
    e.crc = entry_crc(&e);
    if (q == U.n) {
        if (U.n == UMUT(346, UCACHE_MAX_ENTRIES, 100000u)) {                   /* full: the least recently used entry goes */
            unsigned old = 0;
            for (i = 1; i < U.n; i++)
                if (U.e[i].seq < U.e[old].seq)
                    old = i;
            q = old;
        } else {
            U.e = (struct ucache_entry *)realloc(U.e, (U.n + 1) * sizeof(e));
            if (!U.e) {
                U.n = 0;
                return -1;
            }
            U.n++;
        }
    }
    U.e[q] = e;
    return write_file();
}

void ucache_from_rec(struct ucache_entry *e, const struct derive_rec *r)
{
    memcpy(e->pc, r->pc, sizeof(e->pc));
    memcpy(e->f2pcs, r->f2pcs, sizeof(e->f2pcs));
    memcpy(e->lay, r->lay, sizeof(e->lay));
    e->reject_mask = r->reject_mask;
    e->rej_flags = r->rej_flags;
    e->gen204 = r->gen204;
    e->clip204 = r->clip204;
    e->zex = r->zex;
    e->bz_z = r->bz_z;
    e->load_key = r->load_key;
    e->n_ext = r->n_ext < DERIVE_MAXEXT ? r->n_ext : DERIVE_MAXEXT;
    for (unsigned k = 0; k < e->n_ext; k++) {
        e->ext[k][0] = (uint16_t)r->ext[k].pc;
        e->ext[k][1] = r->ext[k].kind;
    }
    memcpy(e->f1pc, r->f1pc, sizeof(e->f1pc));
    e->non = r->non;
    e->dlx = r->dlx;
    e->culldl_off = r->culldl_off;
    memcpy(e->f3pc, r->f3pc, sizeof(e->f3pc));
    e->clip_mask = r->clip_mask;
    e->culldl_mask = r->culldl_mask;
    memcpy(e->f3, r->f3, sizeof(e->f3));
}

void ucache_to_rec(const struct ucache_entry *e, struct derive_rec *r)
{
    memset(r, 0, sizeof(*r));
    r->name = "(cached)";
    r->fam = e->family;
    memcpy(r->pc, e->pc, sizeof(r->pc));
    memcpy(r->f2pcs, e->f2pcs, sizeof(r->f2pcs));
    memcpy(r->lay, e->lay, sizeof(r->lay));
    r->reject_mask = e->reject_mask;
    r->rej_flags = e->rej_flags;
    r->gen204 = e->gen204;
    r->clip204 = UMUT(460, e->clip204, 0);
    r->zex = e->zex;
    r->bz_z = e->bz_z;
    r->load_key = e->load_key;
    r->n_ext = (uint8_t)(e->n_ext < DERIVE_MAXEXT ? e->n_ext : DERIVE_MAXEXT);
    for (unsigned k = 0; k < r->n_ext; k++) {
        r->ext[k].pc = e->ext[k][0];
        r->ext[k].kind = (uint8_t)e->ext[k][1];
    }
    memcpy(r->f1pc, e->f1pc, sizeof(r->f1pc));
    r->non = e->non;
    r->dlx = e->dlx;
    r->culldl_off = e->culldl_off;
    memcpy(r->f3pc, e->f3pc, sizeof(r->f3pc));
    r->clip_mask = e->clip_mask;
    r->culldl_mask = e->culldl_mask;
    memcpy(r->f3, e->f3, sizeof(r->f3));
}
