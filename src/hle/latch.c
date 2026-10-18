/*
 * arMLEt - latch.c: see latch.h
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>

#include "latch.h"

/* test builds (tools/hle/mutcheck_latch.sh): a deliberate error N that tools/hle/test_latch.c must catch */
#ifdef ARMLET_HLE_MUTATE
#define LMUT(n, ok, bad) (ARMLET_HLE_MUTATE == (n) ? (bad) : (ok))
#else
#define LMUT(n, ok, bad) (ok)
#endif

void latch_cfg_default(struct latch_cfg *c)
{
    c->evidence = 24.0;
    c->distinct = 8;
    c->near_mul = 1.75;
    c->strikes = 2;
    c->sample_first = 32;
    c->sample_last = 256;
    c->sample_decay = 64;
    c->queue = 4;
    c->fp_max = 1024;
    c->loose_mul = 2.5;
    c->default_mul = 3.0;
    c->repeat_min = 0.05;
    c->override_mul = 4.0;
    c->resume = 128;
}

int latch_cfg_parse(struct latch_cfg *c, const char *s, const char **bad)
{
    static const char *const keys[] = { "evidence", "distinct", "near", "strikes", "sample", "sample_last", "decay", "queue", "fingerprints", "loose", "default", "repeat", "override", "resume" };
    int rc = 0;
    while (s && *s) {
        size_t n = strcspn(s, "=,"), k;
        double v;
        char *end;
        if (s[n] != '=') {
            if (n && bad && !rc)
                *bad = s;
            rc = n ? -1 : rc;
            s += n + (s[n] == ',');
            continue;
        }
        v = strtod(s + n + 1, &end);
        for (k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
            if (strlen(keys[k]) == n && !strncmp(s, keys[k], n))
                break;
        if (end == s + n + 1 || v < 0 || k == sizeof(keys) / sizeof(keys[0])) {
            if (bad && !rc)
                *bad = s;
            rc = -1;
        } else {
            switch (k) {
            case 0: c->evidence = v; break;
            case 1: c->distinct = (unsigned)v; break;
            case 2: c->near_mul = v < 1 ? 1 : v; break;
            case 3: c->strikes = v < 1 ? 1 : (unsigned)v; break;
            case 4: c->sample_first = v < 1 ? 1 : (unsigned)v; break;
            case 5: c->sample_last = v < 1 ? 1 : (unsigned)v; break;
            case 6: c->sample_decay = v < 1 ? 1 : (unsigned)v; break;
            case 7: c->queue = v < 1 ? 1 : v > 64 ? 64 : (unsigned)v; break;
            case 8: c->fp_max = v < 16 ? 16 : v > 65536 ? 65536 : (unsigned)v; break;
            case 9: c->loose_mul = v < 1 ? 1 : v; break;
            case 10: c->default_mul = v < 1 ? 1 : v; break;
            case 11: c->repeat_min = v > 1 ? 1 : v; break;
            case 12: c->override_mul = v > 0 && v < 1 ? 1 : v; break;
            case 13: c->resume = (unsigned)v; break;
            }
        }
        s = end + strcspn(end, ",");
        if (*s == ',')
            s++;
    }
    if (c->sample_last < c->sample_first)
        c->sample_last = c->sample_first;
    return rc;
}

int latch_init(struct latch_state *s, const struct latch_cfg *c, int near, int loose, unsigned strikes)
{
    unsigned cap = 16;
    memset(s, 0, sizeof(*s));
    s->near = near;
    s->loose = loose;
    s->strikes = strikes;
    s->state = LATCH_CAND;
    while (cap < 2 * c->fp_max)
        cap *= 2;
    s->fp = (uint64_t *)calloc(cap, sizeof(*s->fp));
    s->fpn = (uint32_t *)calloc(cap, sizeof(*s->fpn));
    if (!s->fp || !s->fpn) {
        free(s->fp);
        free(s->fpn);
        s->fp = NULL;
        s->fpn = NULL;
        return -1;
    }
    s->fp_cap = cap;
    return 0;
}

void latch_free(struct latch_state *s)
{
    free(s->fp);
    free(s->fpn);
    s->fp = NULL;
    s->fpn = NULL;
    s->fp_cap = s->fp_n = 0;
}

uint64_t latch_fingerprint(const uint32_t hist[LATCH_NKINDS])
{
    uint64_t h = 0xcbf29ce484222325ull;
    unsigned k;
    for (k = 0; k < LATCH_NKINDS; k++) {
        uint32_t n = hist[k];
        unsigned b = 0;
        if (!n)
            continue;
        while (n && b < 8) {                        /* 1, 2-3, 4-7, ..., 128+ */
            n >>= 1;
            b++;
        }
        b = LMUT(430, b, 1);                        /* (the count left out: every task with the same kinds looks alike) */
        h = (h ^ (k & 0xff)) * 0x100000001b3ull;
        h = (h ^ (k >> 8)) * 0x100000001b3ull;
        h = (h ^ b) * 0x100000001b3ull;
    }
    return h ? h : 1;
}

static unsigned fp_slot(const struct latch_state *s, uint64_t fp)
{
    unsigned i = (unsigned)(fp ^ fp >> 29) & (s->fp_cap - 1);
    while (s->fp[i] && s->fp[i] != fp)
        i = (i + 1) & (s->fp_cap - 1);
    return i;
}

double latch_weight(const struct latch_state *s, const struct latch_cfg *c, uint64_t fp)
{
    unsigned prior;
    double w;
    if (!s->fp)
        return 0;
    prior = s->fpn[fp_slot(s, fp)];
    w = 1.0 / (1.0 + LMUT(431, prior, 0));          /* (431: repeats count fully) */
    return w < LMUT(465, c->repeat_min, 0.0) ? c->repeat_min : w;   /* (465: no floor) */
}

/* the multiplier of a build's thresholds: loose over NEAR (a loose build's probe may have needed the tolerance too) */
static double need_mul(const struct latch_state *s, const struct latch_cfg *c)
{
    return s->deflt ? LMUT(458, c->default_mul, c->loose_mul) : s->loose ? LMUT(450, c->loose_mul, 1.0) : s->near ? LMUT(432, c->near_mul, 1.0) : 1.0;
}
double latch_need_evidence(const struct latch_state *s, const struct latch_cfg *c) { return c->evidence * need_mul(s, c); }
unsigned latch_need_distinct(const struct latch_state *s, const struct latch_cfg *c) { return (unsigned)(c->distinct * need_mul(s, c) + 0.5); }

void latch_never(struct latch_state *s, unsigned k)
{
    if (k < LATCH_NKINDS)
        s->never[k >> 6] |= 1ull << (k & 63);
}

int latch_grade(const struct latch_diff *d)
{
    if (LMUT(452, d->hard, 0))
        return LATCH_G_HARD;
    if (d->numeric && !LMUT(451, d->structural, 0))
        return LATCH_G_NUMERIC;
    return LATCH_G_STRUCTURAL;
}

int latch_match(struct latch_state *s, const struct latch_cfg *c, uint64_t fp, const uint32_t hist[LATCH_NKINDS])
{
    unsigned k, i;
    if (s->state == LATCH_DEAD || !s->fp)
        return LATCH_EV_NONE;
    s->matches++;
    for (k = 0; k < LATCH_NKINDS; k++)
        if (hist[k] && !LMUT(457, s->never[k >> 6] >> (k & 63) & 1, 0))
            s->kinds[k >> 6] |= 1ull << (k & 63);
    if (s->state == LATCH_LATCHED) {
        s->post++;
        return LATCH_EV_NONE;
    }
    s->evidence += latch_weight(s, c, fp);
    i = fp_slot(s, fp);
    if (!s->fp[i]) {
        if (s->fp_n < c->fp_max) {                  /* (a full table: new fingerprints count as new but are not remembered) */
            s->fp[i] = fp;
            s->fpn[i] = 1;
            s->fp_n++;
        }
        s->distinct++;
    } else {
        s->fpn[i]++;
    }
    if ((s->evidence + 1e-9 >= latch_need_evidence(s, c) && s->distinct >= LMUT(433, latch_need_distinct(s, c), 0)) ||
        (c->override_mul > 0 && s->evidence + 1e-9 >= c->evidence * c->override_mul && s->distinct >= LMUT(469, 2u, 1u))) {   /* (the override: variety is not to be had, the evidence is; on the base bar, whatever the tier: variety and the record's certainty are separate questions) */
        s->state = LATCH_LATCHED;
        s->post = 0;
        return LATCH_EV_LATCH;
    }
    return LATCH_EV_NONE;
}

int latch_mismatch(struct latch_state *s, const struct latch_cfg *c, int grade)
{
    if (s->state == LATCH_DEAD)
        return LATCH_EV_NONE;
    s->mismatches++;
    if (grade == LATCH_G_NUMERIC && LMUT(453, s->loose, 1)) {   /* (a strict build never gets this grade; if it did, it is a strike) */
        s->numeric++;
        return LATCH_EV_NONE;
    }
    s->strikes++;
    if (LMUT(434, s->near && !s->loose, 0) || grade == LATCH_G_HARD || s->strikes >= LMUT(435, c->strikes, c->strikes + 1)) {
        s->state = LATCH_DEAD;
        return LATCH_EV_DEAD;
    }
    /* suspended: back to a candidate, the evidence and what it validated start again (which command was wrong is not known) */
    s->state = LATCH_CAND;
    s->evidence = 0;
    s->distinct = 0;
    s->post = 0;
    if (LMUT(436, 1, 0)) {
        memset(s->kinds, 0, sizeof(s->kinds));
        memset(s->fp, 0, s->fp_cap * sizeof(*s->fp));
        memset(s->fpn, 0, s->fp_cap * sizeof(*s->fpn));
        s->fp_n = 0;
    }
    return LATCH_EV_SUSPEND;
}

unsigned latch_sample_interval(const struct latch_state *s, const struct latch_cfg *c)
{
    unsigned iv = c->sample_first, steps = s->post / (c->sample_decay ? c->sample_decay : 1);
    while (steps-- && iv < c->sample_last)
        iv *= LMUT(437, 2, 1);
    return iv > c->sample_last ? c->sample_last : iv;
}
