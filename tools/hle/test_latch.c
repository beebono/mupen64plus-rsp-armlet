/*
 * tools/hle/test_latch.c: the latch policy (src/hle/latch.c) without a game
 *
 *   cc -O2 -Isrc/hle -o test_latch tools/hle/test_latch.c src/hle/latch.c && ./test_latch
 *
 * The settings and their parsing, the fingerprint (kinds and bucketed counts), the evidence of a match (1 / (1 + earlier matches of its fingerprint)), the EXACT and NEAR thresholds (evidence
 * and distinct fingerprints), the command kinds a latched build may run, the suspension of an EXACT build at its first mismatch (evidence, fingerprints and kinds reset), its unlatch for good
 * at the strike limit (also with strikes from earlier sessions), the single strike of a NEAR build, the decaying sampling of a latched build. Prints the failed checks and exits 1 when there is one.
 * tools/hle/mutcheck_latch.sh builds it with each deliberate error of latch.c (-DARMLET_HLE_MUTATE=430 .. 437) and expects it to fail.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "latch.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAILED: %s (line %d)\n", what, __LINE__); fails++; } } while (0)

/* a task with `n` commands of kind k and one G_ENDDL; variety v adds one more kind (v + 1) */
static void hist_of(uint32_t h[LATCH_NKINDS], unsigned k, unsigned n, unsigned v)
{
    memset(h, 0, LATCH_NKINDS * sizeof(*h));
    h[0xdf] = 1;
    h[k] += n;
    if (v)
        h[v] += 1;
}

int main(void)
{
    struct latch_cfg c;
    struct latch_state s;
    uint32_t h[LATCH_NKINDS], h2[LATCH_NKINDS];
    const char *bad = NULL;
    unsigned i, ev;
    double sum;

    /* settings */
    latch_cfg_default(&c);
    CHECK(c.evidence == 24 && c.distinct == 8 && c.near_mul == 1.75 && c.strikes == 2 && c.sample_first == 32 && c.sample_last == 256 && c.sample_decay == 64 && c.queue == 4 && c.loose_mul == 2.5, "the documented defaults");
    CHECK(c.default_mul == 3 && c.repeat_min == 0.05 && c.override_mul == 4 && latch_cfg_parse(&c, "loose=3,default=5,repeat=0.1,override=6", &bad) == 0 && c.loose_mul == 3 && c.default_mul == 5 &&
          c.repeat_min == 0.1 && c.override_mul == 6, "the loose, default and override multipliers and the repeat floor are keys");
    latch_cfg_default(&c);
    CHECK(latch_cfg_parse(&c, "evidence=30,distinct=10,near=2,strikes=3,sample=4,sample_last=128,decay=32,queue=6,fingerprints=512", &bad) == 0 && c.evidence == 30 && c.distinct == 10 &&
          c.near_mul == 2 && c.strikes == 3 && c.sample_first == 4 && c.sample_last == 128 && c.sample_decay == 32 && c.queue == 6 && c.fp_max == 512, "every key is read");
    CHECK(latch_cfg_parse(&c, "evidence=12,bogus=3,distinct=4", &bad) != 0 && bad && !strncmp(bad, "bogus", 5) && c.evidence == 12 && c.distinct == 4, "an unknown key is reported, the others read");
    latch_cfg_default(&c);

    /* the fingerprint */
    hist_of(h, 0x01, 5, 0);
    hist_of(h2, 0x01, 5, 0);
    CHECK(latch_fingerprint(h) == latch_fingerprint(h2), "the same task, the same fingerprint");
    hist_of(h2, 0x01, 6, 0);
    CHECK(latch_fingerprint(h) == latch_fingerprint(h2), "a count in the same bucket (4-7), the same fingerprint");
    hist_of(h2, 0x01, 9, 0);
    CHECK(latch_fingerprint(h) != latch_fingerprint(h2), "a count in another bucket (8-15), another fingerprint");
    hist_of(h2, 0x01, 5, 0x05);
    CHECK(latch_fingerprint(h) != latch_fingerprint(h2), "another kind, another fingerprint");
    hist_of(h2, 0x01, 5, LATCH_K_CLIP);
    CHECK(latch_fingerprint(h) != latch_fingerprint(h2), "a risky path (clipping), another fingerprint");
    hist_of(h, 0x01, 300, 0);
    hist_of(h2, 0x01, 3000, 0);
    CHECK(latch_fingerprint(h) == latch_fingerprint(h2), "counts of 128 and more are one bucket");

    /* evidence: repeats count little */
    CHECK(latch_init(&s, &c, 0, 0, 0) == 0, "init");
    hist_of(h, 0x01, 5, 0);
    for (i = 0, sum = 0; i < 10; i++) {
        CHECK(fabs(latch_weight(&s, &c, latch_fingerprint(h)) - 1.0 / (1 + i)) < 1e-12, "the weight of the n-th repeat is 1 / n");
        sum += 1.0 / (1 + i);
        CHECK(latch_match(&s, &c, latch_fingerprint(h), h) == LATCH_EV_NONE, "no latch on repeats");
    }
    CHECK(fabs(s.evidence - sum) < 1e-9 && s.distinct == 1 && s.matches == 10, "ten repeats: the harmonic sum, one distinct");
    for (i = 0; i < 2000; i++)
        latch_match(&s, &c, latch_fingerprint(h), h);
    CHECK(s.state == LATCH_CAND, "one fingerprint never latches, however often");
    CHECK(fabs(latch_weight(&s, &c, latch_fingerprint(h)) - 0.05) < 1e-12 && s.evidence > 96, "a repeat is worth at least the floor (0.05): 2000 repeats carry the evidence past the bar and past the override, one fingerprint still never latches");
    latch_free(&s);
    /* the override: 4 times the evidence latches with 2 distinct fingerprints (a loaded text's uniform tasks) */
    latch_init(&s, &c, 0, 0, 0);
    hist_of(h, 0x01, 5, 0);
    hist_of(h2, 0x01, 5, 0x10);
    for (i = 0; i < 1000 && s.state == LATCH_CAND; i++)
        latch_match(&s, &c, latch_fingerprint(i & 1 ? h : h2), i & 1 ? h : h2);
    CHECK(s.state == LATCH_LATCHED && s.distinct == 2 && s.evidence + 1e-9 >= 96 && s.evidence < 97, "override: two fingerprints latch once the evidence reaches 4 x 24");
    latch_free(&s);
    c.override_mul = 0;
    latch_init(&s, &c, 0, 0, 0);
    for (i = 0; i < 1000; i++)
        latch_match(&s, &c, latch_fingerprint(i & 1 ? h : h2), i & 1 ? h : h2);
    CHECK(s.state == LATCH_CAND, "override=0: never");
    latch_free(&s);
    latch_cfg_default(&c);
    c.repeat_min = 0;
    latch_init(&s, &c, 0, 0, 0);
    for (i = 0; i < 2000; i++)
        latch_match(&s, &c, latch_fingerprint(h), h);
    CHECK(s.evidence < 9, "repeat=0: the harmonic sum alone (about 8.2 after 2000)");
    latch_free(&s);
    latch_cfg_default(&c);

    /* EXACT: the evidence and the distinct fingerprints, both */
    latch_init(&s, &c, 0, 0, 0);
    for (i = 0, ev = LATCH_EV_NONE; i < 23; i++) {
        hist_of(h, 0x01, 1u << (i % 8), 0x10 + i / 8);       /* 23 distinct tasks */
        ev = (unsigned)latch_match(&s, &c, latch_fingerprint(h), h);
    }
    CHECK(ev == LATCH_EV_NONE && s.state == LATCH_CAND && s.distinct == 23 && fabs(s.evidence - 23) < 1e-9, "23 distinct tasks: evidence 23, not latched");
    hist_of(h, 0x01, 1, 0x40);
    CHECK(latch_match(&s, &c, latch_fingerprint(h), h) == LATCH_EV_LATCH && s.state == LATCH_LATCHED, "the 24th distinct one latches");
    CHECK(latch_kind_ok(s.kinds, 0x01) && latch_kind_ok(s.kinds, 0x10) && latch_kind_ok(s.kinds, 0x40) && latch_kind_ok(s.kinds, 0xdf), "the kinds of the validated tasks are allowed");
    CHECK(!latch_kind_ok(s.kinds, 0x05) && !latch_kind_ok(s.kinds, LATCH_K_CLIP) && !latch_kind_ok(s.kinds, LATCH_NKINDS + 3), "the others are not");
    latch_free(&s);

    latch_init(&s, &c, 0, 0, 0);                                /* 7 fingerprints with plenty of repeats: the evidence is there, the variety is not */
    for (i = 0; i < 7 * 400; i++) {
        hist_of(h, 0x01, 1u << (i % 7), 0);
        latch_match(&s, &c, latch_fingerprint(h), h);
    }
    CHECK(s.evidence >= 24 && s.distinct == 7 && s.state == LATCH_CAND, "7 distinct: not latched however much evidence");
    hist_of(h, 0x01, 1u << 7, 0);
    CHECK(latch_match(&s, &c, latch_fingerprint(h), h) == LATCH_EV_LATCH, "the 8th distinct fingerprint latches it");
    latch_free(&s);

    /* NEAR: 1.75 times both (42 evidence, 14 distinct) */
    latch_init(&s, &c, 1, 0, 0);
    CHECK(fabs(latch_need_evidence(&s, &c) - 42) < 1e-9 && latch_need_distinct(&s, &c) == 14, "NEAR thresholds");
    for (i = 0; i < 41; i++) {
        hist_of(h, 0x02, 1u << (i % 8), 0x10 + i / 8);
        latch_match(&s, &c, latch_fingerprint(h), h);
    }
    CHECK(s.state == LATCH_CAND && fabs(s.evidence - 41) < 1e-9, "NEAR: 41 is not enough");
    hist_of(h, 0x02, 1, 0x60);
    CHECK(latch_match(&s, &c, latch_fingerprint(h), h) == LATCH_EV_LATCH, "NEAR: 42 distinct latches");
    latch_free(&s);

    /* EXACT: the first mismatch suspends and resets; the second unlatches for good */
    latch_init(&s, &c, 0, 0, 0);
    for (i = 0; i < 24; i++) {
        hist_of(h, 0x03, 1u << (i % 8), 0x10 + i / 8);
        latch_match(&s, &c, latch_fingerprint(h), h);
    }
    CHECK(s.state == LATCH_LATCHED, "latched");
    CHECK(latch_mismatch(&s, &c, LATCH_G_STRUCTURAL) == LATCH_EV_SUSPEND && s.state == LATCH_CAND && s.strikes == 1, "first mismatch: suspended");
    CHECK(s.evidence == 0 && s.distinct == 0 && !latch_kind_ok(s.kinds, 0x03) && !latch_kind_ok(s.kinds, 0x10), "suspended: evidence, distinct and kinds reset");
    hist_of(h, 0x03, 1, 0x10);
    CHECK(fabs(latch_weight(&s, &c, latch_fingerprint(h)) - 1.0) < 1e-12, "suspended: the fingerprints are forgotten too");
    for (i = 0; i < 24; i++) {
        hist_of(h, 0x03, 1u << (i % 8), 0x10 + i / 8);
        latch_match(&s, &c, latch_fingerprint(h), h);
    }
    CHECK(s.state == LATCH_LATCHED, "relatched after new evidence");
    CHECK(latch_mismatch(&s, &c, LATCH_G_STRUCTURAL) == LATCH_EV_DEAD && s.state == LATCH_DEAD, "second mismatch: unlatched for good");
    hist_of(h, 0x03, 1, 0x70);
    CHECK(latch_match(&s, &c, latch_fingerprint(h), h) == LATCH_EV_NONE && s.state == LATCH_DEAD, "nothing relatches a dead build");
    CHECK(latch_mismatch(&s, &c, LATCH_G_STRUCTURAL) == LATCH_EV_NONE, "nor counts against it");
    latch_free(&s);

    latch_init(&s, &c, 0, 0, 1);                             /* one strike from an earlier session */
    CHECK(latch_mismatch(&s, &c, LATCH_G_STRUCTURAL) == LATCH_EV_DEAD, "a strike from an earlier session counts: the next mismatch unlatches for good");
    latch_free(&s);

    c.strikes = 3;
    latch_init(&s, &c, 0, 0, 0);
    CHECK(latch_mismatch(&s, &c, LATCH_G_STRUCTURAL) == LATCH_EV_SUSPEND && latch_mismatch(&s, &c, LATCH_G_STRUCTURAL) == LATCH_EV_SUSPEND && latch_mismatch(&s, &c, LATCH_G_STRUCTURAL) == LATCH_EV_DEAD, "strikes=3: the third unlatches");
    latch_free(&s);
    c.strikes = 2;

    /* NEAR: one mismatch, for good */
    latch_init(&s, &c, 1, 0, 0);
    CHECK(latch_mismatch(&s, &c, LATCH_G_STRUCTURAL) == LATCH_EV_DEAD && s.state == LATCH_DEAD, "NEAR: a single mismatch unlatches for good");
    latch_free(&s);

    /* the decaying sampling of a latched build */
    latch_init(&s, &c, 0, 0, 0);
    for (i = 0; i < 24; i++) {
        hist_of(h, 0x04, 1u << (i % 8), 0x10 + i / 8);
        latch_match(&s, &c, latch_fingerprint(h), h);
    }
    CHECK(latch_sample_interval(&s, &c) == 32, "one in 32 at first");
    hist_of(h, 0x04, 1, 0x10);
    for (i = 0; i < 64; i++)
        latch_match(&s, &c, latch_fingerprint(h), h);
    CHECK(s.post == 64 && latch_sample_interval(&s, &c) == 64, "one in 64 after 64 matches");
    for (i = 0; i < 64 * 2; i++)
        latch_match(&s, &c, latch_fingerprint(h), h);
    CHECK(latch_sample_interval(&s, &c) == 256, "one in 256 after 192");
    for (i = 0; i < 64 * 10; i++)
        latch_match(&s, &c, latch_fingerprint(h), h);
    CHECK(latch_sample_interval(&s, &c) == 256, "never below one in 256");
    hist_of(h, 0x04, 1, 0x7e);
    latch_match(&s, &c, latch_fingerprint(h), h);
    CHECK(latch_kind_ok(s.kinds, 0x7e), "a latched build's matches still add kinds");
    latch_mismatch(&s, &c, LATCH_G_STRUCTURAL);
    CHECK(latch_sample_interval(&s, &c) == 32, "suspended and relatched: the sampling starts again at one in 32");
    latch_free(&s);

    /* a loose build (docs/design.md, "The loose tier"): 2.5 times both (60 evidence, 20 distinct); a NUMERIC mismatch is counted and nothing more, a STRUCTURAL one is a strike as for EXACT,
     * a HARD one unlatches for good at once; a strict build is never told NUMERIC, and if it were, that would be a strike */
    latch_init(&s, &c, 0, 1, 0);
    CHECK(s.loose && fabs(latch_need_evidence(&s, &c) - 60) < 1e-9 && latch_need_distinct(&s, &c) == 20, "loose thresholds");
    for (i = 0; i < 59; i++) {
        hist_of(h, 0x06, 1u << (i % 8), 0x10 + i / 8);
        latch_match(&s, &c, latch_fingerprint(h), h);
    }
    CHECK(s.state == LATCH_CAND, "loose: 59 is not enough");
    hist_of(h, 0x06, 1, 0x60);
    CHECK(latch_match(&s, &c, latch_fingerprint(h), h) == LATCH_EV_LATCH, "loose: 60 distinct latches");
    CHECK(latch_mismatch(&s, &c, LATCH_G_NUMERIC) == LATCH_EV_NONE && s.state == LATCH_LATCHED && s.strikes == 0 && s.numeric == 1 && s.mismatches == 1, "loose: a NUMERIC mismatch is counted, not a strike");
    CHECK(latch_mismatch(&s, &c, LATCH_G_NUMERIC) == LATCH_EV_NONE && latch_mismatch(&s, &c, LATCH_G_NUMERIC) == LATCH_EV_NONE && s.numeric == 3 && s.state == LATCH_LATCHED, "loose: however many");
    CHECK(latch_mismatch(&s, &c, LATCH_G_STRUCTURAL) == LATCH_EV_SUSPEND && s.state == LATCH_CAND && s.strikes == 1, "loose: a STRUCTURAL mismatch suspends like EXACT");
    for (i = 0; i < 60; i++) {
        hist_of(h, 0x06, 1u << (i % 8), 0x10 + i / 8);
        latch_match(&s, &c, latch_fingerprint(h), h);
    }
    CHECK(s.state == LATCH_LATCHED, "loose: relatched");
    CHECK(latch_mismatch(&s, &c, LATCH_G_STRUCTURAL) == LATCH_EV_DEAD && s.state == LATCH_DEAD, "loose: the second STRUCTURAL strike unlatches for good");
    latch_free(&s);
    latch_init(&s, &c, 0, 1, 0);
    CHECK(latch_mismatch(&s, &c, LATCH_G_HARD) == LATCH_EV_DEAD && s.state == LATCH_DEAD, "loose: a HARD mismatch unlatches for good at once");
    latch_free(&s);
    latch_init(&s, &c, 0, 1, 0);
    s.deflt = 1;
    CHECK(fabs(latch_need_evidence(&s, &c) - 72) < 1e-9 && latch_need_distinct(&s, &c) == 24, "default record: 3 times both (72, 24)");
    latch_free(&s);
    latch_init(&s, &c, 1, 1, 0);
    CHECK(fabs(latch_need_evidence(&s, &c) - 60) < 1e-9, "loose over NEAR: the loose multiplier");
    CHECK(latch_mismatch(&s, &c, LATCH_G_STRUCTURAL) == LATCH_EV_SUSPEND, "loose over NEAR: NEAR's one-strike rule does not apply");
    latch_free(&s);
    latch_init(&s, &c, 0, 0, 0);
    CHECK(latch_mismatch(&s, &c, LATCH_G_NUMERIC) == LATCH_EV_SUSPEND && s.strikes == 1 && s.numeric == 0, "strict: a NUMERIC grade is a strike all the same");
    CHECK(latch_mismatch(&s, &c, LATCH_G_HARD) == LATCH_EV_DEAD, "strict: a HARD grade unlatches for good");
    latch_free(&s);
    /* the region gate: a never kind is not added by a match, latched or not, and survives a suspension's reset */
    latch_init(&s, &c, 0, 1, 0);
    latch_never(&s, LATCH_K_LIT);
    latch_never(&s, LATCH_K_CLIP);
    for (i = 0; i < 60; i++) {
        hist_of(h, 0x07, 1u << (i % 8), 0x10 + i / 8);
        h[LATCH_K_LIT] = 1;
        h[LATCH_K_CLIP] = 2;
        latch_match(&s, &c, latch_fingerprint(h), h);
    }
    CHECK(s.state == LATCH_LATCHED && latch_kind_ok(s.kinds, 0x07) && latch_kind_ok(s.kinds, 0x10), "never: latched, the other kinds admitted");
    CHECK(!latch_kind_ok(s.kinds, LATCH_K_LIT) && !latch_kind_ok(s.kinds, LATCH_K_CLIP), "never: lit and clipped tasks are not admitted though every match had them");
    latch_mismatch(&s, &c, LATCH_G_STRUCTURAL);
    hist_of(h, 0x07, 1, 0x10);
    h[LATCH_K_LIT] = 1;
    latch_match(&s, &c, latch_fingerprint(h), h);
    CHECK(!latch_kind_ok(s.kinds, LATCH_K_LIT) && latch_kind_ok(s.kinds, 0x07), "never: kept across a suspension");
    latch_free(&s);
    {   /* the grade of a compare's classes */
        struct latch_diff d = { 0, 0, 0 };
        CHECK(latch_grade(&d) == LATCH_G_STRUCTURAL, "nothing: structural (never asked)");
        d.numeric = 1;
        CHECK(latch_grade(&d) == LATCH_G_NUMERIC, "numeric only: NUMERIC");
        d.structural = 1;
        CHECK(latch_grade(&d) == LATCH_G_STRUCTURAL, "numeric and structural: STRUCTURAL");
        d.hard = 1;
        CHECK(latch_grade(&d) == LATCH_G_HARD, "hard with anything: HARD");
        d.numeric = d.structural = 0;
        CHECK(latch_grade(&d) == LATCH_G_HARD, "hard alone: HARD");
    }

    /* the bound on remembered fingerprints */
    c.fp_max = 16;
    latch_init(&s, &c, 0, 0, 0);
    for (i = 0; i < 100; i++) {
        hist_of(h, 0x05, 1, 0x10 + i);
        latch_match(&s, &c, latch_fingerprint(h), h);
    }
    CHECK(s.fp_n == 16 && s.distinct == 24 && s.state == LATCH_LATCHED, "a full table counts new fingerprints as new, remembers at most fp_max (and the build latched at 24)");
    latch_free(&s);

    if (!fails)
        printf("all checks passed\n");
    return fails ? 1 : 0;
}
