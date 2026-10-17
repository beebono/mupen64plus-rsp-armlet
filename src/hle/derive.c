/*
 * arMLEt - derive.c: the derivation of a build record from a microcode text, in C
 *
 * A port of tools/derive_build.py (derive_f2.py, derive_f1d.py, derive_lib.py, rsp_isa.py), decision for decision: docs/ucode-derive.md describes the method, "The C port" what is
 * different (no strings, the references come from derive_refs.c). tools/derive_c_check.py runs both on every dump and diffs the records, verdicts and counts. Where the Python code
 * relies on dict or set order (the first maximum, insertion order), this one keeps the order. The Python names are in the comments.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "derive.h"

#define NONE_PC INT_MIN         /* Python's None for a handler pc */
#define MAXSEG 16
#define OP_LOADER 0xdd

enum { C_J, C_BR, C_MEM, C_VMEM, C_ALUI, C_OTHER, C_NOP };
enum { ST_OK, ST_PC, ST_KNOB, ST_PAD, ST_PAT, ST_X };
enum { EV_PC, EV_CF, EV_IMM, EV_MEM };
enum { K_ADDR, K_SIZE, K_MASK };
enum { NEAR_INIT, NEAR_POS, NEAR_SMALL };
enum { XR_INIT, XR_EXTRA, XR_DELETED };
enum { KN_REJECT = 100 };       /* knob name ids: LY_* for the layout, this for the reject mask */

static uint64_t mix64(uint64_t x)
{
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

uint64_t derive_fnv_words(const uint32_t *w, unsigned nwords)
{
    uint64_t x = 0xcbf29ce484222325ull;
    unsigned i;
    for (i = 0; i < nwords; i++)
        x = (x ^ w[i]) * 0x100000001b3ull;
    return x;
}

const char *derive_verdict_name(int v) { return v == DERIVE_EXACT ? "EXACT" : v == DERIVE_NEAR ? "NEAR" : "NONE"; }
const char *derive_family_name(int f) { return f == DERIVE_F3DEX2 ? "f3dex2" : f == DERIVE_F3DEX1 ? "f3dex1" : f == DERIVE_F3D ? "f3d" : "?"; }

/* ---- the instruction decoder (rsp_isa.py: only what the derivation looks at) -------------------------------------------------------------------------------------------- */
static int sx16(uint32_t v) { return (int)(int16_t)(uint16_t)v; }

static int cls_of(uint32_t w)
{
    unsigned op = w >> 26, rt = (w >> 16) & 31;
    if (!w)
        return C_NOP;
    if (op == 2 || op == 3)
        return C_J;
    if ((op >= 4 && op <= 7) || (op == 1 && (rt == 0 || rt == 1 || rt == 16 || rt == 17)))
        return C_BR;
    switch (op) {
    case 0x20: case 0x21: case 0x23: case 0x24: case 0x25: case 0x27: case 0x28: case 0x29: case 0x2b:
        return C_MEM;
    case 0x32: case 0x3a:
        return C_VMEM;
    default:
        break;
    }
    if (op >= 8 && op <= 0x0f)
        return C_ALUI;
    return C_OTHER;
}

static int vmem_sub_defined(unsigned op, unsigned sub)
{
    if (op == 0x32)
        return sub <= 9 || sub == 0xb;                  /* LV */
    return sub <= 0xb;                                  /* SV */
}

static int vscale(unsigned sub)
{
    static const uint8_t t[12] = { 1, 2, 4, 8, 16, 16, 8, 8, 16, 16, 16, 16 };
    return sub < 12 ? t[sub] : 1;
}

/* the SPECIAL-class names that matter: jr, jalr, break */
static int is_special(uint32_t w, unsigned f) { return (w >> 26) == 0 && (w & 0x3f) == f; }
static int is_jr(uint32_t w) { return is_special(w, 8); }
static int is_jalr(uint32_t w) { return is_special(w, 9); }
static int is_break(uint32_t w) { return is_special(w, 0x0d); }

/* the shape of a word (tools/derive_refs_gen.py shape()): hash, class, rs == 0, init-eligible, operand */
struct wshape {
    uint64_t h;
    uint8_t cls, rs0, init;
    uint32_t opd;
};

static void wshape_of(uint32_t w, struct wshape *s)
{
    int c = cls_of(w);
    uint32_t m;
    unsigned op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31;
    s->cls = (uint8_t)c;
    s->rs0 = 0;
    s->opd = 0;
    s->init = 0;
    switch (c) {
    case C_NOP:
        m = 0;
        break;
    case C_J:
        m = w & 0xfc000000u;
        s->opd = w & 0x3ffffff;
        break;
    case C_BR:
    case C_MEM:
    case C_ALUI:
        m = w & 0xffff0000u;
        s->opd = w & 0xffff;
        s->rs0 = rs == 0;
        break;
    case C_VMEM: {
        unsigned sub = (w >> 11) & 31;
        int off7 = (int)(w & 0x7f);
        m = w & ~0x7fu;
        if (!vmem_sub_defined(op, sub))
            m = (m & ~0xfc000000u) | (0x32u << 26);
        if (off7 & 0x40)
            off7 -= 0x80;
        s->opd = (uint32_t)(off7 * vscale(sub)) & 0xffff;
        s->rs0 = rs == 0;
        break;
    }
    default:
        m = w;
        break;
    }
    if (c == C_ALUI)
        s->init = 1;
    else if (c == C_OTHER) {
        int ex = is_jr(w) || is_jalr(w) || is_break(w) || (op == 0x10 && (rs == 0 || rs == 4));
        s->init = !ex;
    }
    (void)rt;
    s->h = mix64(mix64(m) ^ (uint64_t)c);
}

/* ---- images ------------------------------------------------------------------------------------------------------------------------------------------------------------ */
struct dimg {
    int n;
    uint32_t *w;                /* the real words (the unknown only) */
    uint32_t *sid;              /* shape id */
    uint32_t *opd;
    uint8_t *cls, *flg;         /* flg: bit 0 rs == 0, bit 1 init-eligible */
    uint16_t *pc;
    uint8_t *seg;
    int nsegs;
    struct derive_seg segs[MAXSEG];
    uint32_t active;            /* bit k: segment k is code of the build (all ones: all); Image.active */
};

static void img_free(struct dimg *im)
{
    free(im->w);
    free(im->sid);
    free(im->opd);
    free(im->cls);
    free(im->flg);
    free(im->pc);
    free(im->seg);
    memset(im, 0, sizeof(*im));
}

static int img_alloc(struct dimg *im, int n, int with_w)
{
    memset(im, 0, sizeof(*im));
    im->n = n;
    im->w = with_w ? (uint32_t *)calloc((size_t)n, 4) : NULL;
    im->sid = (uint32_t *)calloc((size_t)n, 4);
    im->opd = (uint32_t *)calloc((size_t)n, 4);
    im->cls = (uint8_t *)calloc((size_t)n, 1);
    im->flg = (uint8_t *)calloc((size_t)n, 1);
    im->pc = (uint16_t *)calloc((size_t)n, 2);
    im->seg = (uint8_t *)calloc((size_t)n, 1);
    im->active = ~0u;
    return (!im->sid || !im->opd || !im->cls || !im->flg || !im->pc || !im->seg || (with_w && !im->w)) ? -1 : 0;
}

static int shape_find(uint64_t h);
static int shape_find_j(uint32_t w)
{
    struct wshape s;
    wshape_of(w, &s);
    return shape_find(s.h);
}

/* a reference's image, from derive_refs.c */
static int img_ref(struct dimg *im, const struct derive_ref *r)
{
    int i, s;
    if (img_alloc(im, r->n, 0))
        return -1;
    im->nsegs = r->nsegs;
    for (s = 0; s < r->nsegs; s++) {
        im->segs[s] = r->segs[s];
        for (i = 0; i < r->segs[s].n; i++) {
            im->seg[r->segs[s].first + i] = (uint8_t)s;
            im->pc[r->segs[s].first + i] = s == 0 ? (uint16_t)((0x80 + 4 * i) & 0xfff) : (uint16_t)(r->segs[s].base + 4 * i);
        }
    }
    {
        int jsid[2];
        jsid[0] = shape_find_j(0x08000000u);
        jsid[1] = shape_find_j(0x0c000000u);
        for (i = 0; i < r->n; i++) {
            uint32_t v = derive_words[r->woff + (unsigned)i];
            if (v & 0x80000000u) {                          /* a jump: bit 26 says jal, the low 26 bits are the target field */
                im->sid[i] = (uint32_t)jsid[(v >> 26) & 1];
                im->opd[i] = v & 0x3ffffff;
                im->cls[i] = C_J;
                im->flg[i] = 0;
            } else {
                uint8_t inf = derive_shape_info[v >> 16];
                im->sid[i] = v >> 16;
                im->opd[i] = v & 0xffff;
                im->cls[i] = inf & 7;
                im->flg[i] = (uint8_t)(((inf >> 3) & 1) | (((inf >> 4) & 1) << 1));
            }
        }
    }
    return 0;
}

static int shape_find(uint64_t h)
{
    unsigned lo = 0, hi = derive_n_shapes;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (derive_shape_hash[mid] < h)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < derive_n_shapes && derive_shape_hash[lo] == h ? (int)lo : -1;
}

/* shape ids of words the references do not have: derive_n_shapes + a local number */
struct unkmap {
    uint64_t *h;
    uint32_t *id;
    size_t cap;
    uint32_t next;
};

static uint32_t unk_id(struct unkmap *m, uint64_t h)
{
    size_t k = (size_t)(h * 0x9e3779b97f4a7c15ull >> 20) & (m->cap - 1);
    while (m->id[k]) {
        if (m->h[k] == h)
            return m->id[k] - 1 + derive_n_shapes;
        k = (k + 1) & (m->cap - 1);
    }
    m->h[k] = h;
    m->id[k] = ++m->next;
    return m->next - 1 + derive_n_shapes;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static unsigned u16d(const uint8_t *d, size_t len, long a) { return a >= 0 && (size_t)a + 2 <= len ? (unsigned)(d[a] << 8 | d[a + 1]) : 0; }

struct ovd {
    unsigned off, imem, ln;
};

static int ovd_cmp(const void *a, const void *b)
{
    const struct ovd *x = (const struct ovd *)a, *y = (const struct ovd *)b;
    if (x->off != y->off)
        return x->off < y->off ? -1 : 1;
    if (x->imem != y->imem)
        return x->imem < y->imem ? -1 : 1;
    return x->ln < y->ln ? -1 : x->ln > y->ln;
}

/* L.find_descriptors + F2.build_image: overlay descriptors of the data segment; the text + the segments whose bytes lie behind it */
static int img_unknown(struct dimg *im, const struct derive_in *in, struct unkmap *um)
{
    struct ovd ov[64];
    int nov = 0, k, i, nw;
    size_t a, lim;
    uint32_t *words;
    int total;

    lim = in->data_len < 0xe00 ? in->data_len : 0xe00;
    for (a = 0; lim >= 4 && a < lim - 4; a += 4) {
        uint32_t o, b;
        unsigned imem;
        if (a + 8 > in->data_len)
            break;
        o = be32(in->data + a);
        b = be32(in->data + a + 4);
        if ((b & 0xf000) == 0x1000 && (b >> 16) >= 0x1f && (b >> 16) <= 0x7ff && o <= 0x3000 && (o & 7) == 0) {
            imem = b & 0xfff;
            if (imem >= 0x100 && imem <= 0x700 && (imem & 7) == 0) {
                unsigned ln = (b >> 16) + 1, off = o, im2 = imem;
                if (off >= 0x1000) {
                } else if (off + ln > 0x1000) {
                    im2 = imem + (0x1000 - off);
                    ln = off + ln - 0x1000;
                    off = 0x1000;
                } else
                    continue;
                for (k = 0; k < nov; k++)
                    if (ov[k].off == off && ov[k].imem == im2 && ov[k].ln == ln)
                        break;
                if (k == nov && nov < 64) {
                    ov[nov].off = off;
                    ov[nov].imem = im2;
                    ov[nov].ln = ln;
                    nov++;
                }
            }
        }
    }
    qsort(ov, (size_t)nov, sizeof(ov[0]), ovd_cmp);
    /* count the words */
    total = 1024;
    nw = 0;
    for (k = 0; k < nov && nw < MAXSEG - 1; k++) {
        if (ov[k].off < 0x1000 || (size_t)(ov[k].off - 0x1000) + ov[k].ln > in->ext_len)
            continue;
        total += (int)(ov[k].ln / 4);
        nw++;
    }
    if (img_alloc(im, total, 1))
        return -1;
    words = im->w;
    memcpy(words, in->text, 1024 * 4);
    im->segs[0].base = 0x80;
    im->segs[0].first = 0;
    im->segs[0].n = 1024;
    im->segs[0].off = 0;
    im->nsegs = 1;
    for (i = 0; i < 1024; i++)
        im->pc[i] = (uint16_t)((0x80 + 4 * i) & 0xfff);
    nw = 1024;
    for (k = 0; k < nov && im->nsegs < MAXSEG; k++) {
        unsigned ln, j;
        if (ov[k].off < 0x1000 || (size_t)(ov[k].off - 0x1000) + ov[k].ln > in->ext_len)
            continue;
        ln = ov[k].ln / 4;
        im->segs[im->nsegs].base = (uint16_t)ov[k].imem;
        im->segs[im->nsegs].first = (uint16_t)nw;
        im->segs[im->nsegs].n = (uint16_t)ln;
        im->segs[im->nsegs].off = (uint16_t)ov[k].off;
        for (j = 0; j < ln; j++) {
            words[nw] = be32(in->ext + (ov[k].off - 0x1000) + 4 * j);
            im->pc[nw] = (uint16_t)(ov[k].imem + 4 * j);
            im->seg[nw] = (uint8_t)im->nsegs;
            nw++;
        }
        im->nsegs++;
    }
    im->n = nw;
    for (i = 0; i < im->n; i++) {
        struct wshape s;
        int id;
        wshape_of(im->w[i], &s);
        id = shape_find(s.h);
        im->sid[i] = id >= 0 ? (uint32_t)id : unk_id(um, s.h);
        im->opd[i] = s.opd;
        im->cls[i] = s.cls;
        im->flg[i] = (uint8_t)(s.rs0 | (s.init << 1));
    }
    return 0;
}

/* Image.resolve: the word indices an IMEM address can mean from code in segment cur_seg; returns the count (<= MAXSEG) */
static int img_resolve(const struct dimg *im, unsigned pc, int cur_seg, int *out)
{
    int n = 0, k;
    pc &= 0xfff;
    if (cur_seg) {
        const struct derive_seg *s = &im->segs[cur_seg];
        if (s->base <= pc && pc < (unsigned)s->base + 4u * s->n) {
            out[0] = s->first + (int)((pc - s->base) / 4);
            return 1;
        }
    }
    out[n++] = (int)(((pc - 0x80) & 0xfff) / 4);
    for (k = 1; k < im->nsegs; k++) {
        const struct derive_seg *s = &im->segs[k];
        if (!(im->active >> k & 1))
            continue;
        if (s->base <= pc && pc < (unsigned)s->base + 4u * s->n)
            out[n++] = s->first + (int)((pc - s->base) / 4);
    }
    return n;
}

/* ---- the alignment: difflib.SequenceMatcher(None, a, b, autojunk=False).get_matching_blocks() and L.align_keys ---------------------------------------------------------- */
struct blk {
    int i, j, k;
};

/* the positions of every key in b, ascending, grouped by key (b2j of difflib; a nop's key is negative and unique, it can match nothing and is left out) */
struct csr {
    int nkeys;
    int *start;                 /* nkeys + 1 */
    int *pos;
};

static void csr_free(struct csr *c)
{
    free(c->start);
    free(c->pos);
    memset(c, 0, sizeof(*c));
}

static int csr_build(struct csr *c, int nkeys, const int64_t *b, int lb)
{
    int i, k;
    c->nkeys = nkeys;
    c->start = (int *)calloc((size_t)nkeys + 2, sizeof(int));
    c->pos = (int *)malloc((size_t)(lb + 1) * sizeof(int));
    if (!c->start || !c->pos) {
        csr_free(c);
        return -1;
    }
    for (i = 0; i < lb; i++)
        if (b[i] >= 0 && b[i] < nkeys)
            c->start[b[i] + 1]++;
    for (k = 0; k < nkeys; k++)
        c->start[k + 1] += c->start[k];
    {
        int *fill = (int *)malloc((size_t)nkeys * sizeof(int));
        if (!fill) {
            csr_free(c);
            return -1;
        }
        memcpy(fill, c->start, (size_t)nkeys * sizeof(int));
        for (i = 0; i < lb; i++)
            if (b[i] >= 0 && b[i] < nkeys)
                c->pos[fill[b[i]]++] = i;
        free(fill);
    }
    return 0;
}

struct dctx {
    int nkeys;
    struct csr b2j;             /* of the unknown's keys: the b2j of the first pass of every pairing */
};

struct sm {
    const int64_t *a;
    int la, lb;
    const struct csr *b2j;
    int *prev, *cur;            /* j2len arrays (lb + 1) */
    int *tprev, *tcur;          /* touched lists */
};

static void sm_longest(struct sm *m, int alo, int ahi, int blo, int bhi, int *bi, int *bj, int *bk)
{
    int besti = alo, bestj = blo, bestsize = 0, i, nprev = 0, ncur, t;
    int *prev = m->prev, *cur = m->cur, *tprev = m->tprev, *tcur = m->tcur;
    const struct csr *c = m->b2j;
    for (i = alo; i < ahi; i++) {
        int r = 0, re = 0, key = (int)m->a[i];
        ncur = 0;
        if (key >= 0 && key < c->nkeys) {
            r = c->start[key];
            re = c->start[key + 1];
            if (blo > 0 && r < re && c->pos[r] < blo) {                 /* the first position at or after blo */
                int lo = r, hi = re;
                while (lo < hi) {
                    int mid = (lo + hi) / 2;
                    if (c->pos[mid] < blo)
                        lo = mid + 1;
                    else
                        hi = mid;
                }
                r = lo;
            }
        }
        for (; r < re; r++) {
            int j = c->pos[r], k;
            if (j >= bhi)
                break;
            k = (j > 0 ? prev[j - 1] : 0) + 1;
            cur[j] = k;
            tcur[ncur++] = j;
            if (k > bestsize) {
                besti = i - k + 1;
                bestj = j - k + 1;
                bestsize = k;
            }
        }
        for (t = 0; t < nprev; t++)
            prev[tprev[t]] = 0;
        { int *x = prev; prev = cur; cur = x; x = tprev; tprev = tcur; tcur = x; }
        nprev = ncur;
    }
    for (t = 0; t < nprev; t++)
        prev[tprev[t]] = 0;
    *bi = besti;
    *bj = bestj;
    *bk = bestsize;
}

static int blk_cmp(const void *a, const void *b)
{
    const struct blk *x = (const struct blk *)a, *y = (const struct blk *)b;
    if (x->i != y->i)
        return x->i - y->i;
    if (x->j != y->j)
        return x->j - y->j;
    return x->k - y->k;
}

/* the matching blocks with the adjacent ones merged (and no sentinel); returns the count, blocks malloc'ed. b2j is of b (the caller built it) */
static int sm_blocks(const int64_t *a, int la, int lb, const struct csr *b2j, struct blk **out)
{
    struct sm m;
    int (*stack)[4];
    int sp = 0, n = 0, i, nm = 0;
    struct blk *blocks, *merged;
    int ok = 1;

    memset(&m, 0, sizeof(m));
    m.a = a;
    m.la = la;
    m.lb = lb;
    m.b2j = b2j;
    m.prev = (int *)calloc((size_t)lb + 2, sizeof(int));
    m.cur = (int *)calloc((size_t)lb + 2, sizeof(int));
    m.tprev = (int *)malloc((size_t)(lb + 2) * sizeof(int));
    m.tcur = (int *)malloc((size_t)(lb + 2) * sizeof(int));
    stack = (int (*)[4])malloc((size_t)(la + lb + 4) * sizeof(*stack));
    blocks = (struct blk *)malloc((size_t)(la + lb + 4) * sizeof(struct blk));
    merged = (struct blk *)malloc((size_t)(la + lb + 4) * sizeof(struct blk));
    if (!m.prev || !m.cur || !m.tprev || !m.tcur || !stack || !blocks || !merged)
        ok = 0;
    if (ok) {
        stack[sp][0] = 0; stack[sp][1] = la; stack[sp][2] = 0; stack[sp][3] = lb; sp++;
        while (sp) {
            int alo, ahi, blo, bhi, bi, bj, bk;
            sp--;
            alo = stack[sp][0]; ahi = stack[sp][1]; blo = stack[sp][2]; bhi = stack[sp][3];
            if (alo >= ahi || blo >= bhi)
                continue;
            sm_longest(&m, alo, ahi, blo, bhi, &bi, &bj, &bk);
            if (bk) {
                blocks[n].i = bi; blocks[n].j = bj; blocks[n].k = bk; n++;
                if (alo < bi && blo < bj) {
                    stack[sp][0] = alo; stack[sp][1] = bi; stack[sp][2] = blo; stack[sp][3] = bj; sp++;
                }
                if (bi + bk < ahi && bj + bk < bhi) {
                    stack[sp][0] = bi + bk; stack[sp][1] = ahi; stack[sp][2] = bj + bk; stack[sp][3] = bhi; sp++;
                }
            }
        }
        qsort(blocks, (size_t)n, sizeof(struct blk), blk_cmp);
        {
            int i1 = 0, j1 = 0, k1 = 0;
            for (i = 0; i < n; i++) {
                if (i1 + k1 == blocks[i].i && j1 + k1 == blocks[i].j)
                    k1 += blocks[i].k;
                else {
                    if (k1) {
                        merged[nm].i = i1; merged[nm].j = j1; merged[nm].k = k1; nm++;
                    }
                    i1 = blocks[i].i; j1 = blocks[i].j; k1 = blocks[i].k;
                }
            }
            if (k1) {
                merged[nm].i = i1; merged[nm].j = j1; merged[nm].k = k1; nm++;
            }
        }
    }
    free(m.prev); free(m.cur); free(m.tprev); free(m.tcur); free(stack); free(blocks);
    if (!ok) {
        free(merged);
        return -1;
    }
    *out = merged;
    return nm;
}

/* L.align_keys: pr[i] = j for the aligned words (a = the reference, b = the unknown), -1 elsewhere. first_b2j: the b2j of the whole of kb, shared by the pairings of one derivation */
static int align_keys(const int64_t *ka, int la, const int64_t *kb, int lb, int minblock, int nkeys, const struct csr *first_b2j, int *pr)
{
    int *ia = (int *)malloc((size_t)(la + 1) * sizeof(int)), *ib = (int *)malloc((size_t)(lb + 1) * sizeof(int));
    int64_t *a = (int64_t *)malloc((size_t)(la + 1) * sizeof(int64_t)), *b = (int64_t *)malloc((size_t)(lb + 1) * sizeof(int64_t));
    uint8_t *ub = (uint8_t *)calloc((size_t)lb + 1, 1);
    int first = 1, i, na, nbb, rc = 0;
    if (!ia || !ib || !a || !b || !ub) {
        free(ia); free(ib); free(a); free(b); free(ub);
        return -1;
    }
    for (i = 0; i < la; i++)
        pr[i] = -1;
    na = la;
    nbb = lb;
    for (i = 0; i < la; i++)
        ia[i] = i;
    for (i = 0; i < lb; i++)
        ib[i] = i;
    while (na && nbb) {
        struct blk *bl = NULL;
        struct csr local;
        const struct csr *c = first_b2j;
        int nb, got = 0, q, k;
        for (i = 0; i < na; i++)
            a[i] = ka[ia[i]];
        for (i = 0; i < nbb; i++)
            b[i] = kb[ib[i]];
        memset(&local, 0, sizeof(local));
        if (!first) {
            if (csr_build(&local, nkeys, b, nbb)) {
                rc = -1;
                break;
            }
            c = &local;
        }
        nb = sm_blocks(a, na, nbb, c, &bl);
        csr_free(&local);
        if (nb < 0) {
            rc = -1;
            break;
        }
        for (q = 0; q < nb; q++) {
            if (bl[q].k < (first ? 1 : minblock))
                continue;
            for (k = 0; k < bl[q].k; k++) {
                pr[ia[bl[q].i + k]] = ib[bl[q].j + k];
                ub[ib[bl[q].j + k]] = 1;
            }
            got += bl[q].k;
        }
        free(bl);
        first = 0;
        if (!got)
            break;
        { int n2 = 0; for (i = 0; i < la; i++) if (pr[i] < 0) ia[n2++] = i; na = n2; }
        { int n2 = 0; for (i = 0; i < lb; i++) if (!ub[i]) ib[n2++] = i; nbb = n2; }
    }
    free(ia); free(ib); free(a); free(b); free(ub);
    return rc;
}

/* ---- knobs ------------------------------------------------------------------------------------------------------------------------------------------------------------- */
struct knob {
    int name;
    uint32_t val;
    int kind;
};
#define MAXKNOB 64

static int knobs_of(const struct derive_rec *r, struct knob *ks)
{
    int n = 0, k;
    if (r->fam == DERIVE_F3DEX2) {
        for (k = 0; k < DERIVE_NLAY_ADDR; k++) {
            uint32_t v = r->lay[k];
            if (v >= 0x10 && k != LY_MOVEWORD_ADJ && v) {
                ks[n].name = k; ks[n].val = v; ks[n].kind = K_ADDR; n++;
            }
        }
        if (r->lay[LY_MOVEWORD_ADJ]) {
            ks[n].name = LY_MOVEWORD_ADJ; ks[n].val = r->lay[LY_MOVEWORD_ADJ]; ks[n].kind = K_ADDR; n++;
        }
        if (r->lay[LY_OUT_BYTES]) {
            ks[n].name = LY_OUT_BYTES; ks[n].val = r->lay[LY_OUT_BYTES]; ks[n].kind = K_SIZE; n++;
        }
        if (r->reject_mask) {
            ks[n].name = KN_REJECT; ks[n].val = r->reject_mask; ks[n].kind = K_MASK; n++;
        }
    } else {
        for (k = 0; k < r->n_knobs; k++) {
            ks[n].name = k; ks[n].val = r->knobs[k].val; ks[n].kind = r->knobs[k].kind == 0 ? K_ADDR : K_MASK; n++;
        }
    }
    return n;
}

static unsigned size_enc(unsigned v, int enc)
{
    switch (enc) {
    case 0: return v & 0xfff;
    case 1: return (0u - v) & 0xfff;
    case 2: return (v - 1) & 0xfff;
    default: return (1u - v) & 0xfff;
    }
}

static unsigned inv_size(int enc, unsigned imm)
{
    imm &= 0xfff;
    switch (enc) {
    case 0: return imm;
    case 1: return (0u - imm) & 0xfff;
    case 2: return (imm + 1) & 0xfff;
    default: return (1u - imm) & 0xfff;
    }
}

/* ---- the pairing of one reference with the unknown --------------------------------------------------------------------------------------------------------------------- */
struct cand {
    int k, enc;                 /* knob index; encoding (-1 '=') */
    uint32_t v;
};
struct ev {
    uint8_t kind;
    int i, j;
    uint32_t vr, vu;
    int c0, nc;                 /* candidates in the pool */
};
struct pairing {
    int ref;                    /* index in derive_refs */
    const struct derive_rec *rec;
    struct dimg R;
    int *pr, *pu;
    int n_pr;
    uint8_t *st;                /* per unknown word */
    uint8_t *xr;                /* per reference word */
    int *hunk;                  /* per reference word: the unaligned run of the unknown that replaces it (index of its first word), or -1 */
    int n_x, n_xr;
    struct knob ks[MAXKNOB];
    int nks;
    int derived[MAXKNOB];       /* 0/1 */
    uint32_t dval[MAXKNOB];
    int conflict[MAXKNOB], ambig[MAXKNOB];
    int anchor_n[MAXKNOB], same_n[MAXKNOB];
    int any_trouble;
};

static void pairing_free(struct pairing *P)
{
    img_free(&P->R);
    free(P->pr); free(P->pu); free(P->st); free(P->xr); free(P->hunk);
    memset(P, 0, sizeof(*P));
}

/* U words 3 of the S2DEX 2.05 [Safe] pattern: addi rX, zero, 0xe7 ; sb rX, 0(s7) ; addi s7, s7, 8 */
static int pattern_pipe_sync(const uint32_t *w)
{
    uint32_t a = w[0], b = w[1], c = w[2];
    return (a >> 26) == 8 && ((a >> 21) & 31) == 0 && (a & 0xffff) == 0xe7 && (b >> 26) == 0x28 && ((b >> 21) & 31) == 23 && ((b >> 16) & 31) == ((a >> 16) & 31) && (b & 0xffff) == 0 &&
           (c >> 26) == 8 && ((c >> 21) & 31) == 23 && ((c >> 16) & 31) == 23 && (c & 0xffff) == 8;
}

static int j_tgt(uint32_t opd) { return (int)((opd << 2) & 0xfff); }

static int pc_ok(const struct pairing *P, const struct dimg *U, unsigned vr, unsigned vu, int seg_r, int seg_u)
{
    int ur[MAXSEG + 1], rr[MAXSEG + 1], nu, nr, a, b;
    nu = img_resolve(U, vu & 0xfff, seg_u, ur);
    nr = img_resolve(&P->R, vr & 0xfff, seg_r, rr);
    for (a = 0; a < nr; a++) {
        int x = P->pr[rr[a]];
        if (x < 0)
            continue;
        for (b = 0; b < nu; b++)
            if (ur[b] == x)
                return 1;
    }
    return 0;
}

/* the hunk of reference words that sit between the same anchors as the unaligned run [j0, j1] of the unknown (lo, hi: their reference indices); returns 0 when there is none */
static int hunk_bounds(const struct pairing *P, const struct dimg *U, int j0, int j1, int *lo, int *hi, int runlen)
{
    int k;
    *lo = *hi = -1;
    for (k = j0 - 1; k >= 0; k--)
        if (P->pu[k] >= 0 && U->seg[k] == U->seg[j0]) {
            *lo = P->pu[k];
            break;
        }
    for (k = j1 + 1; k < U->n; k++)
        if (P->pu[k] >= 0 && U->seg[k] == U->seg[j1]) {
            *hi = P->pu[k];
            break;
        }
    if (*lo < 0 || *hi < 0 || *hi <= *lo || *hi - *lo > 4 * runlen + 8)
        return 0;
    return 1;
}

static int pairing_make(struct pairing *P, int ref, const struct dimg *U, int nkeys, const struct csr *ub2j)
{
    const struct derive_ref *dr = &derive_refs[ref];
    const struct dimg *R;
    int i, j, k;
    int64_t *ka, *kb;
    struct ev *ev = NULL;
    int nev = 0;
    struct cand *cp = NULL;
    int ncp = 0, capcp = 0;
    struct { uint32_t v; uint8_t kind; } *same_ops = NULL;
    int nsame = 0;

    memset(P, 0, sizeof(*P));
    P->ref = ref;
    P->rec = &derive_recs[dr->rec];
    if (img_ref(&P->R, dr))
        return -1;
    R = &P->R;
    P->pr = (int *)malloc((size_t)(R->n + 1) * sizeof(int));
    P->pu = (int *)malloc((size_t)(U->n + 1) * sizeof(int));
    P->st = (uint8_t *)calloc((size_t)U->n + 1, 1);
    P->xr = (uint8_t *)calloc((size_t)R->n + 1, 1);
    P->hunk = (int *)malloc((size_t)(R->n + 1) * sizeof(int));
    ka = (int64_t *)malloc((size_t)(R->n + 1) * sizeof(int64_t));
    kb = (int64_t *)malloc((size_t)(U->n + 1) * sizeof(int64_t));
    ev = (struct ev *)malloc((size_t)(R->n + 1) * sizeof(struct ev));
    same_ops = malloc((size_t)(R->n + 1) * sizeof(*same_ops));
    if (!P->pr || !P->pu || !P->st || !P->xr || !P->hunk || !ka || !kb || !ev || !same_ops)
        goto fail;
    for (i = 0; i < R->n; i++)
        ka[i] = R->cls[i] == C_NOP ? -2 * (int64_t)(i + 1) : (int64_t)R->sid[i];
    for (j = 0; j < U->n; j++)
        kb[j] = U->cls[j] == C_NOP ? -2 * (int64_t)(j + 1) - 1 : (int64_t)U->sid[j];
    if (align_keys(ka, R->n, kb, U->n, 3, nkeys, ub2j, P->pr))
        goto fail;
    for (j = 0; j < U->n; j++)
        P->pu[j] = -1;
    for (i = 0; i < R->n; i++) {
        P->hunk[i] = -1;
        if (P->pr[i] >= 0) {
            P->pu[P->pr[i]] = i;
            P->n_pr++;
        }
    }
    /* _collect */
    for (i = 0; i < R->n; i++) {
        uint32_t vr, vu;
        int c;
        j = P->pr[i];
        if (j < 0)
            continue;
        c = R->cls[i];
        if (R->opd[i] == U->opd[j]) {
            if (c == C_MEM || c == C_VMEM) {
                same_ops[nsame].v = R->opd[i];
                same_ops[nsame++].kind = 0;
            } else if (c == C_ALUI) {
                same_ops[nsame].v = R->opd[i];
                same_ops[nsame++].kind = 1;
            }
            continue;
        }
        ev[nev].i = i;
        ev[nev].j = j;
        ev[nev].c0 = ev[nev].nc = 0;
        if (c == C_J || c == C_BR) {
            unsigned tr, tu;
            if (c == C_J) {
                tr = (unsigned)j_tgt(R->opd[i]);
                tu = (unsigned)j_tgt(U->opd[j]);
            } else {
                tr = (R->pc[i] + 4u + 4u * (unsigned)sx16(R->opd[i])) & 0xfff;
                tu = (U->pc[j] + 4u + 4u * (unsigned)sx16(U->opd[j])) & 0xfff;
            }
            ev[nev].kind = pc_ok(P, U, tr, tu, R->seg[i], U->seg[j]) ? EV_PC : EV_CF;
            ev[nev].vr = tr;
            ev[nev].vu = tu;
        } else if (c == C_ALUI) {
            int lit;
            vr = R->opd[i];
            vu = U->opd[j];
            lit = (R->flg[i] & 1) && (vr & 3) == 0 && (vu & 3) == 0 && (vr >> 12) == (vu >> 12) && (vr >> 12) != 0;
            ev[nev].kind = lit && pc_ok(P, U, vr, vu, R->seg[i], U->seg[j]) ? EV_PC : EV_IMM;
            ev[nev].vr = vr;
            ev[nev].vu = vu;
        } else if (c == C_MEM || c == C_VMEM) {
            ev[nev].kind = EV_MEM;
            ev[nev].vr = R->opd[i] & 0xffff;
            ev[nev].vu = U->opd[j] & 0xffff;
        } else
            continue;                                   /* ("other": the shapes are whole words there, so aligned words are equal) */
        nev++;
    }
    /* _knobs */
    P->nks = knobs_of(P->rec, P->ks);
#ifdef DERIVE_DEBUG
    for (i = 0; i < nev; i++)
        if (ev[i].j == 604)
            fprintf(stderr, "EVDBG kind %d i %d j %d vr %x vu %x\n", ev[i].kind, ev[i].i, ev[i].j, ev[i].vr, ev[i].vu);
#endif
    for (i = 0; i < nev; i++) {
        if (ev[i].kind != EV_IMM && ev[i].kind != EV_MEM)
            continue;
        for (k = 0; k < P->nks; k++) {
            const struct knob *kn = &P->ks[k];
            uint32_t vr = ev[i].vr, vu = ev[i].vu;
            int enc;
            if (kn->kind == K_ADDR) {
                if (vr == kn->val || (vr == (kn->val | 0x1000) && (vu >> 12) == (vr >> 12))) {
                    if (ncp + 1 > capcp) {
                        capcp = capcp ? capcp * 2 : 256;
                        cp = (struct cand *)realloc(cp, (size_t)capcp * sizeof(*cp));
                        if (!cp)
                            goto fail;
                    }
                    if (!ev[i].nc)
                        ev[i].c0 = ncp;
                    cp[ncp].k = k; cp[ncp].enc = -1; cp[ncp].v = vr == kn->val ? vu : (vu & 0xfff);
                    ncp++; ev[i].nc++;
                }
            } else if (kn->kind == K_MASK) {
                if (ev[i].kind == EV_IMM && vr == kn->val) {
                    if (ncp + 1 > capcp) {
                        capcp = capcp ? capcp * 2 : 256;
                        cp = (struct cand *)realloc(cp, (size_t)capcp * sizeof(*cp));
                        if (!cp)
                            goto fail;
                    }
                    if (!ev[i].nc)
                        ev[i].c0 = ncp;
                    cp[ncp].k = k; cp[ncp].enc = -1; cp[ncp].v = vu;
                    ncp++; ev[i].nc++;
                }
            } else {
                for (enc = 0; enc < 4; enc++)
                    if ((vr & 0xfff) == size_enc(kn->val, enc)) {
                        if (ncp + 1 > capcp) {
                            capcp = capcp ? capcp * 2 : 256;
                            cp = (struct cand *)realloc(cp, (size_t)capcp * sizeof(*cp));
                            if (!cp)
                                goto fail;
                        }
                        if (!ev[i].nc)
                            ev[i].c0 = ncp;
                        cp[ncp].k = k; cp[ncp].enc = enc; cp[ncp].v = inv_size(enc, vu);
                        ncp++; ev[i].nc++;
                    }
            }
        }
    }
    for (k = 0; k < P->nks; k++) {
        const struct knob *kn = &P->ks[k];
        int n = 0;
        for (i = 0; i < nsame; i++) {
            uint32_t v = same_ops[i].v;
            if (kn->kind == K_ADDR && v == kn->val)
                n++;
            else if (kn->kind == K_SIZE && ((v & 0xfff) == size_enc(kn->val, 0) || (v & 0xfff) == size_enc(kn->val, 1) || (v & 0xfff) == size_enc(kn->val, 2) || (v & 0xfff) == size_enc(kn->val, 3)))
                n++;
            else if (kn->kind == K_MASK && same_ops[i].kind == 1 && v == kn->val && v >= 0x100 &&
                     !(v == 0xffff || v == 0x7fff || v == 0x8000 || v == 0xfff || v == 0x1000 || v == 0x3fff || v == 0x7ff || v == 0xfff0))
                n++;
        }
        P->same_n[k] = n;
    }
    {
        /* votes[name][value] in insertion order */
        struct { int k; int nv; uint32_t v[64]; int cnt[64]; int total; } vot[MAXKNOB];
        int nvot = 0;
        memset(vot, 0, sizeof(vot));
        for (i = 0; i < nev; i++) {
            int q, w;
            if ((ev[i].kind != EV_IMM && ev[i].kind != EV_MEM) || ev[i].nc != 1)
                continue;
            k = cp[ev[i].c0].k;
            for (q = 0; q < nvot; q++)
                if (vot[q].k == k)
                    break;
            if (q == nvot) {
                vot[q].k = k;
                nvot++;
            }
            for (w = 0; w < vot[q].nv; w++)
                if (vot[q].v[w] == cp[ev[i].c0].v)
                    break;
            if (w == vot[q].nv && vot[q].nv < 64) {
                vot[q].v[w] = cp[ev[i].c0].v;
                vot[q].nv++;
            }
            if (w < 64) {
                vot[q].cnt[w]++;
                vot[q].total++;
            }
            if (w == 64)
                vot[q].total++;
        }
        for (i = 0; i < nvot; i++) {
            int best = 0, w;
            uint32_t bv;
            k = vot[i].k;
            for (w = 1; w < vot[i].nv; w++)
                if (vot[i].cnt[w] > vot[i].cnt[best])
                    best = w;
            bv = vot[i].v[best];
            if (vot[i].nv > 1)
                P->conflict[k] = 1;
            if (P->same_n[k] && vot[i].total <= 1 && bv != P->ks[k].val) {
                P->ambig[k] = 1;
                continue;
            }
            P->derived[k] = 1;
            P->dval[k] = bv;
        }
    }
    for (i = 0; i < nev; i++) {
        int q;
        if (ev[i].kind != EV_IMM && ev[i].kind != EV_MEM)
            continue;
        for (q = 0; q < ev[i].nc; q++) {
            const struct cand *c = &cp[ev[i].c0 + q];
            if (P->derived[c->k] && P->dval[c->k] == c->v)
                P->anchor_n[c->k]++;
        }
    }
    for (k = 0; k < P->nks; k++)
        if (P->conflict[k] || P->ambig[k])
            P->any_trouble = 1;
    /* _classify */
    for (i = 0; i < nev; i++) {
        j = ev[i].j;
        if (ev[i].kind == EV_PC)
            P->st[j] = ST_PC;
        else if (ev[i].kind == EV_CF)
            P->st[j] = ST_X;
        else {
            int q, ok = 0;
            for (q = 0; q < ev[i].nc; q++) {
                const struct cand *c = &cp[ev[i].c0 + q];
                if ((P->derived[c->k] ? P->dval[c->k] == c->v : 1)) {
                    ok = 1;
                    break;
                }
            }
            P->st[j] = ok ? ST_KNOB : ST_X;
        }
    }
    {
        /* runs of unaligned words of the unknown */
        int run0 = -1;
        for (j = 0; j <= U->n; j++) {
            int un = j < U->n && P->pu[j] < 0;
            if (un && run0 >= 0 && U->seg[j] == U->seg[run0])
                continue;
            if (run0 >= 0) {
                int j1 = j - 1, len = j1 - run0 + 1, allnop = 1, lo, hi, hb, pat = 0;
                for (k = run0; k <= j1; k++)
                    if (U->cls[k] != C_NOP)
                        allnop = 0;
                hb = hunk_bounds(P, U, run0, j1, &lo, &hi, len);
                if (allnop) {
                    int rnop = 1;
                    if (hb)
                        for (k = lo + 1; k < hi; k++)
                            if (P->pr[k] < 0 && R->cls[k] != C_NOP)
                                rnop = 0;
                    for (k = run0; k <= j1; k++)
                        P->st[k] = rnop ? ST_PAD : ST_X;
                } else {
                    pat = len == 3 && pattern_pipe_sync(&U->w[run0]);
                    for (k = run0; k <= j1; k++) {
                        if (pat)
                            P->st[k] = ST_PAT;
                        else if (U->cls[k] == C_NOP && len <= 2)
                            P->st[k] = ST_PAD;
                        else
                            P->st[k] = ST_X;
                    }
                }
                if (hb)
                    for (k = lo + 1; k < hi; k++)
                        if (P->pr[k] < 0)
                            P->hunk[k] = run0;
                run0 = -1;
            }
            if (un)
                run0 = j;
        }
    }
    /* the unaligned tail of an overlay segment is slack (a descriptor may declare more bytes than the code has) */
    for (k = 1; k < U->nsegs; k++) {
        int first = U->segs[k].first, n = U->segs[k].n, last = first - 1, x;
        for (x = first; x < first + n; x++)
            if (P->pu[x] >= 0)
                last = x;
        for (x = last + 1; x < first + n; x++)
            P->st[x] = ST_PAD;
    }
    for (j = 0; j < U->n; j++)
        if (P->st[j] == ST_X)
            P->n_x++;
    for (i = 0; i < R->n; i++) {
        if (P->pr[i] < 0 && R->cls[i] != C_NOP) {
            int sg = R->seg[i];
            if (sg) {
                int first = R->segs[sg].first, n = R->segs[sg].n, last = first - 1, x;
                for (x = first; x < first + n; x++)
                    if (P->pr[x] >= 0)
                        last = x;
                if (i > last)
                    continue;
            }
            P->xr[i] = 1;
            P->n_xr++;
        }
    }
    free(ka); free(kb); free(ev); free(same_ops); free(cp);
    return 0;
fail:
    free(ka); free(kb); free(ev); free(same_ops); free(cp);
    pairing_free(P);
    return -1;
}

#ifdef DERIVE_DEBUG
#define DBG_PAIRING(P, U) dbg_pairing(P, U)
static void dbg_pairing(const struct pairing *P, const struct dimg *U)
{
    const char *nm = "oPKdtX";
    int j, i;
    fprintf(stderr, "PRIM %s n_pr %d n_x %d n_xr %d\n", derive_refs[P->ref].name, P->n_pr, P->n_x, P->n_xr);
    for (i = 0; i < P->R.n; i++)
        if (P->pr[i] >= 0)
            fprintf(stderr, "PR %d %d\n", i, P->pr[i]);
    for (i = 0; i < P->nks; i++)
        fprintf(stderr, "KN %d name %d val %x kind %d derived %d dval %x conflict %d ambig %d same %d anchor %d\n", i, P->ks[i].name, P->ks[i].val, P->ks[i].kind, P->derived[i], P->dval[i], P->conflict[i], P->ambig[i], P->same_n[i], P->anchor_n[i]);
    for (j = 0; j < U->n; j++)
        if (P->st[j] == ST_X || P->st[j] == ST_KNOB)
            fprintf(stderr, "ST %d %c\n", j, nm[P->st[j]]);
    for (i = 0; i < P->R.n; i++)
        if (P->xr[i])
            fprintf(stderr, "XR %d\n", i);
}
#else
#define DBG_PAIRING(P, U) ((void)0)
#endif

/* ---- reach (derive_f2.reach), the unknown only ------------------------------------------------------------------------------------------------------------------------ */
static int is_ra_lit(uint32_t w)
{
    unsigned op = w >> 26;
    return (op == 8 || op == 9 || op == 0x0d) && ((w >> 21) & 31) == 0 && ((w >> 16) & 31) == 31 && (w & 0xf000);
}

/* seen: n bytes, pre-set (1) for the words of `block`; on return the words reached are 1 and the block words 0 */
static void reach(const struct dimg *im, const int *entries, int nent, const int *stop, int nstop, const uint8_t *block, uint8_t *seen)
{
    int *wi, *wf, nw = 0, e, cap = 4 * im->n + 64, r;
    int rs[MAXSEG + 1];
    if (block)
        memcpy(seen, block, (size_t)im->n);
    else
        memset(seen, 0, (size_t)im->n);
    wi = (int *)malloc((size_t)cap * sizeof(int));
    wf = (int *)malloc((size_t)cap * sizeof(int));
    if (!wi || !wf) {
        free(wi); free(wf);
        return;
    }
#define PUSH(idx, fst) do { if (nw < cap) { wi[nw] = (idx); wf[nw] = (fst); nw++; } } while (0)
    for (e = 0; e < nent; e++) {
        int n = img_resolve(im, (unsigned)entries[e], 0, rs);
        for (r = 0; r < n; r++)
            PUSH(rs[r], 1);
    }
    while (nw) {
        int i, first;
        nw--;
        i = wi[nw];
        first = wf[nw];
        while (i >= 0 && i < im->n && !seen[i]) {
            uint32_t w;
            int c, s, nxt, after, st;
            if (!first) {
                int isstop = 0;
                for (st = 0; st < nstop; st++)
                    if (im->pc[i] == stop[st])
                        isstop = 1;
                if (isstop && im->seg[i] == 0)
                    break;
            }
            first = 0;
            seen[i] = 1;
            w = im->w[i];
            c = im->cls[i];
            s = im->seg[i];
            nxt = (i + 1 < im->n && im->seg[i + 1] == s) ? i + 1 : -1;
            if (c == C_ALUI && is_ra_lit(w)) {
                int n = img_resolve(im, w & 0xfff, s, rs);
                for (r = 0; r < n; r++)
                    PUSH(rs[r], 0);
            }
            if (c == C_J || (c == C_OTHER && (is_jr(w) || is_jalr(w))) || c == C_BR) {
                if (nxt >= 0) {
                    seen[nxt] = 1;
                    if (im->cls[nxt] == C_ALUI && is_ra_lit(im->w[nxt])) {
                        int n = img_resolve(im, im->w[nxt] & 0xfff, s, rs);
                        for (r = 0; r < n; r++)
                            PUSH(rs[r], 0);
                    }
                }
                after = (i + 2 < im->n && im->seg[i + 2] == s) ? i + 2 : -1;
                if (c == C_J) {
                    int n = img_resolve(im, (unsigned)j_tgt(w & 0x3ffffff), s, rs);
                    for (r = 0; r < n; r++)
                        PUSH(rs[r], 0);
                    if ((w >> 26) == 3 && after >= 0) {
                        i = after;
                        continue;
                    }
                    break;
                }
                if (c == C_BR) {
                    int n = img_resolve(im, (im->pc[i] + 4u + 4u * (unsigned)sx16(w)) & 0xfff, s, rs);
                    for (r = 0; r < n; r++)
                        PUSH(rs[r], 0);
                    if (after >= 0) {
                        i = after;
                        continue;
                    }
                    break;
                }
                if (is_jalr(w) && after >= 0) {
                    i = after;
                    continue;
                }
                break;
            }
            if (is_break(w))
                break;
            if (nxt < 0)
                break;
            i = nxt;
        }
    }
#undef PUSH
    if (block)
        for (e = 0; e < im->n; e++)
            if (block[e])
                seen[e] = 0;
    free(wi);
    free(wf);
}

/* ---- the tables and the pcs --------------------------------------------------------------------------------------------------------------------------------------------- */
static int valid_pc(int v) { return v != NONE_PC && v != 0 && v != 0xffff; }

static void read_table_u(const uint8_t *data, size_t len, unsigned jt, int *tab)
{
    int op;
    for (op = 0; op < 256; op++) {
        unsigned v = u16d(data, len, (long)jt + 2L * (op > 127 ? op - 256 : op));
        tab[op] = (v & 0xf000) == 0x1000 ? (int)(v & 0xfff) : NONE_PC;
    }
}

static void read_table_r(const struct derive_ref *r, int *tab)
{
    int op;
    for (op = 0; op < 256; op++) {
        int v = derive_jt[r->jt + op];
        tab[op] = v < 0 ? NONE_PC : v;
    }
}

/* F2.map_pc: the IMEM address in the unknown of the instruction at pc in the reference */
static int map_pc(const struct pairing *P, const struct dimg *U, int pc)
{
    int rr[MAXSEG + 1], n, a, t;
    if (pc == 0 || pc == 0xffff)
        return pc;
    n = img_resolve(&P->R, (unsigned)pc & 0xfff, 0, rr);
    for (a = 0; a < n; a++) {
        int ia = rr[a];
        if (ia + 6 <= P->R.n) {
            int hits = 0, hit = -1;
            for (t = 0; t + 6 <= U->n; t++) {
                int q;
                if (U->seg[t] != 0)
                    continue;
                for (q = 0; q < 6; q++)
                    if (U->sid[t + q] != P->R.sid[ia + q])
                        break;
                if (q == 6) {
                    hits++;
                    hit = t;
                }
            }
            if (hits == 1)
                return U->pc[hit];
        }
        if (P->pr[ia] >= 0)
            return U->pc[P->pr[ia]];
    }
    return -1;
}

struct conv {
    const struct pairing *P;
    const struct dimg *U;
    int tabR[256], tabU[256];
};

/* map_pcs_by_table's conv(pc) */
static int conv_pc(const struct conv *cv, int pc)
{
    int votes_v[256], votes_n[256], nv = 0, op, k, best;
    if (pc == 0 || pc == 0xffff)
        return pc;
    for (op = 0; op < 256; op++) {
        int v;
        if (!valid_pc(cv->tabR[op]) || cv->tabR[op] != pc)
            continue;
        v = cv->tabU[op];
        if (!valid_pc(v))
            continue;
        for (k = 0; k < nv; k++)
            if (votes_v[k] == v)
                break;
        if (k == nv) {
            votes_v[nv] = v;
            votes_n[nv] = 0;
            nv++;
        }
        votes_n[k]++;
    }
    if (nv) {
        best = 0;
        for (k = 1; k < nv; k++)
            if (votes_n[k] > votes_n[best])
                best = k;
        return votes_v[best];
    }
    return map_pc(cv->P, cv->U, pc);
}

/* ---- the F3DEX2 / S2DEX derivation (derive_f2.derive) ------------------------------------------------------------------------------------------------------------------- */
struct merged {
    int has[256];
    uint32_t val[256];
};

static int knob_idx(const struct pairing *P, int name)
{
    int k;
    for (k = 0; k < P->nks; k++)
        if (P->ks[k].name == name)
            return k;
    return -1;
}

/* merge_knobs(order) */
static void merge_knobs(struct pairing **order, int norder, struct merged *m)
{
    int names[512], nn = 0, a, idx, i;
    memset(m, 0, sizeof(*m));
    for (a = 0; a < norder; a++)
        for (i = 0; i < order[a]->nks; i++) {
            int nm = order[a]->ks[i].name, q;
            for (q = 0; q < nn; q++)
                if (names[q] == nm)
                    break;
            if (q == nn && nn < 512)
                names[nn++] = nm;
        }
    for (a = 0; a < nn; a++) {
        int name = names[a];
        for (idx = 0; idx < norder; idx++) {
            const struct pairing *P = order[idx];
            int k = knob_idx(P, name);
            if (k >= 0 && P->derived[k] && !P->conflict[k] && (idx == 0 || P->anchor_n[k] >= 2)) {
                m->has[name] = 1;
                m->val[name] = P->dval[k];
                break;
            }
            if (k >= 0 && P->same_n[k] >= 2) {
                m->has[name] = 1;
                m->val[name] = P->ks[k].val;
                break;
            }
        }
    }
}

static void set_active_u(struct dimg *im, const uint8_t *data, size_t len, const uint32_t *lay)
{
    uint32_t act = 1;
    int which, k;
    for (which = 0; which < 2; which++) {
        unsigned a = lay[which == 0 ? LY_DESC_LIGHT : LY_DESC_CLIP];
        if (a && a + 8 <= len) {
            unsigned off = be32(data + a), b = be32(data + a + 4);
            for (k = 1; k < im->nsegs; k++)
                if (im->segs[k].base == (b & 0xfff) && im->segs[k].off == off)
                    act |= 1u << k;
        }
    }
    for (k = 1; k < im->nsegs; k++)
        if (im->segs[k].off == 0x1000)
            act |= 1u << k;
    im->active = act;
}

static void overlay_words(const struct dimg *im, const uint8_t *data, size_t len, const uint32_t *lay, int which, uint8_t *out)
{
    unsigned a = lay[which];
    memset(out, 0, (size_t)im->n);
    if (!a || a + 8 > len)
        return;
    {
        unsigned off = be32(data + a), b = be32(data + a + 4), ln = ((b >> 16) & 0xfff) + 1, imem = b & 0xfff, x;
        if (off < 0x1000) {
            for (x = off / 4; x < (off + ln) / 4 && x < 1024; x++)
                out[x] = 1;
        } else {
            int k;
            for (k = 1; k < im->nsegs; k++)
                if (im->segs[k].base == imem && im->segs[k].off == off) {
                    for (x = 0; x < im->segs[k].n; x++)
                        out[im->segs[k].first + x] = 1;
                    break;
                }
        }
    }
}

struct f2work {
    int verdict;
    int n_x, nx_total;
    unsigned n_near[3], n_xr[3];
    int primary;
    int n_contrib;
    int contrib[DERIVE_MAXREFS];
    unsigned contrib_n[DERIVE_MAXREFS];
    struct derive_rec rec;
    int trouble;
    unsigned aligned_num;
};

static int cmp_pair_order(const struct pairing *a, const struct pairing *b)
{
    if (a->n_x != b->n_x)
        return a->n_x < b->n_x ? -1 : 1;
    if (a->n_xr != b->n_xr)
        return a->n_xr < b->n_xr ? -1 : 1;
    if (a->n_pr != b->n_pr)
        return a->n_pr > b->n_pr ? -1 : 1;                  /* -len(pr) ascending */
    return 0;
}

/* stable insertion sort of the pairings by (len x_set, len xr, -len pr) */
static void sort_pairings(struct pairing **o, int n)
{
    int i, j;
    for (i = 1; i < n; i++) {
        struct pairing *x = o[i];
        for (j = i - 1; j >= 0 && cmp_pair_order(o[j], x) > 0; j--)
            o[j + 1] = o[j];
        o[j + 1] = x;
    }
}

static int refs_of(const struct derive_in *in, int fam, int *list)
{
    unsigned r;
    int n = 0;
    for (r = 0; r < derive_n_refs; r++) {
        const struct derive_rec *rec = &derive_recs[derive_refs[r].rec];
        if (rec->fam != fam || (in->exclude_recs >> derive_refs[r].rec & 1))
            continue;
        if (n < DERIVE_MAXREFS)
            list[n++] = (int)r;
    }
    return n;
}

static int derive_f2(const struct derive_in *in, struct dimg *U, const struct dctx *X, const int *refs, int nrefs, struct f2work *out)
{
    struct pairing *prs = (struct pairing *)calloc((size_t)nrefs, sizeof(struct pairing));
    struct pairing *order[DERIVE_MAXREFS];
    struct merged mg;
    struct derive_rec *rec = &out->rec;
    struct pairing *prim;
    uint8_t *expl[DERIVE_MAXREFS];
    uint8_t *cur = NULL, *gen_own = NULL, *zex_own = NULL, *tmp = NULL;
    uint8_t *sets[10], *xrs[2] = { NULL, NULL };
    int tab[256], tabR[256], i, k, j, rc = -1, ndone = 0, head;
    struct conv cv;
    const uint32_t *lay;
    int nex = 0;

    memset(expl, 0, sizeof(expl));
    memset(sets, 0, sizeof(sets));
    if (!prs)
        return -1;
    for (i = 0; i < nrefs; i++) {
        if (pairing_make(&prs[i], refs[i], U, X->nkeys, &X->b2j))
            goto done;
        ndone++;
        order[i] = &prs[i];
    }
    sort_pairings(order, nrefs);
    prim = order[0];
    out->primary = prim->ref;
    out->aligned_num = (unsigned)prim->n_pr;
    DBG_PAIRING(prim, U);
    for (i = 0; i < nrefs; i++) {
        expl[i] = (uint8_t *)malloc((size_t)U->n);
        if (!expl[i])
            goto done;
        for (j = 0; j < U->n; j++)
            expl[i][j] = order[i]->st[j] != ST_X;
    }
    memset(rec, 0, sizeof(*rec));
    rec->name = "(derived)";
    rec->fam = DERIVE_F3DEX2;
    merge_knobs(order, nrefs, &mg);
    /* derived_layout */
    memcpy(rec->lay, prim->rec->lay, sizeof(rec->lay));
    for (k = 0; k < prim->nks; k++) {
        if (prim->ks[k].name >= 64)
            continue;
        if (mg.has[prim->ks[k].name])
            rec->lay[prim->ks[k].name] = mg.val[prim->ks[k].name];
        else if (prim->derived[k])
            rec->lay[prim->ks[k].name] = prim->dval[k];
    }
    rec->reject_mask = mg.has[KN_REJECT] ? mg.val[KN_REJECT] : prim->rec->reject_mask;
    lay = rec->lay;
    out->trouble = prim->any_trouble;
    read_table_u(in->data, in->data_len, lay[LY_JUMP_TABLE], tab);
    read_table_r(&derive_refs[prim->ref], tabR);
    memset(&cv, 0, sizeof(cv));
    cv.P = prim;
    cv.U = U;
    memcpy(cv.tabU, tab, sizeof(tab));
    memcpy(cv.tabR, tabR, sizeof(tabR));
    for (k = 0; k < DERIVE_NPC; k++)
        rec->pc[k] = conv_pc(&cv, prim->rec->pc[k]);
    {
        int known[64], nk = 0;
        static const int mvp_tri4[2] = { PC_MVP, PC_TRI4 };
        static const int ops[2] = { 0xd5, 0x10 };
        for (k = 0; k < DERIVE_NPC; k++)
            if (valid_pc(rec->pc[k]))
                known[nk++] = rec->pc[k];
        for (k = 0; k < DERIVE_NF2PCS; k++)
            if (valid_pc(prim->rec->f2pcs[k]) && nk < 64)
                known[nk++] = prim->rec->f2pcs[k];
        for (i = 0; i < 2 && !prim->rec->n_ext; i++) {
            int fld = mvp_tri4[i], op = ops[i];
            if (prim->rec->pc[fld] == 0) {
                int inknown = 0;
                if (valid_pc(tab[op]))
                    for (k = 0; k < nk; k++)
                        if (known[k] == tab[op])
                            inknown = 1;
                if (valid_pc(tab[op]) && tab[op] != rec->pc[PC_HEAD] && !inknown && (!valid_pc(tabR[op]) || tabR[op] == prim->rec->pc[PC_HEAD]))
                    rec->pc[fld] = tab[op];
            } else if (rec->pc[fld] == rec->pc[PC_HEAD])
                rec->pc[fld] = 0;
        }
    }
    for (k = 0; k < DERIVE_NF2PCS; k++)
        rec->f2pcs[k] = conv_pc(&cv, prim->rec->f2pcs[k]);
    rec->n_ext = prim->rec->n_ext;
    for (k = 0; k < prim->rec->n_ext; k++) {
        rec->ext[k].pc = conv_pc(&cv, prim->rec->ext[k].pc);
        rec->ext[k].kind = prim->rec->ext[k].kind;
    }
    rec->load_key = derive_fnv_words(in->text, 0xf48 / 4);
    {
        int npat = 0;
        for (j = 0; j < U->n; j++)
            if (prim->st[j] == ST_PAT)
                npat++;
        rec->lay[LY_PIPE_SYNC] = npat ? 1 : 0;
    }
    head = rec->pc[PC_HEAD];
    /* the code regions by role (reach_sets) */
    set_active_u(U, in->data, in->data_len, rec->lay);
    {
        static const struct { int idx; int op; } roles[4] = { { 0, 0x01 }, { 1, 0x05 }, { 2, 0x06 }, { 3, 0x04 } };
        /* sets[0..3]: vtx tri1 tri2 branchz (the other roles of the Python tool's reach_sets are for its per-handler report) */
        uint8_t *clip = (uint8_t *)malloc((size_t)U->n), *light = (uint8_t *)malloc((size_t)U->n), *blk = (uint8_t *)malloc((size_t)U->n);
        int stop[3], r;
        uint8_t *lightset = NULL, *bzset = NULL;
        if (!clip || !light || !blk) {
            free(clip); free(light); free(blk);
            goto done;
        }
        overlay_words(U, in->data, in->data_len, rec->lay, LY_DESC_CLIP, clip);
        overlay_words(U, in->data, in->data_len, rec->lay, LY_DESC_LIGHT, light);
        for (j = 0; j < U->n; j++)
            blk[j] = clip[j] | light[j];
        stop[0] = tab[OP_LOADER] != NONE_PC && tab[OP_LOADER] != 0 ? tab[OP_LOADER] : 0xfac;
        stop[1] = head;
        stop[2] = head - 4;
        for (r = 0; r < 4; r++) {
            sets[roles[r].idx] = (uint8_t *)calloc((size_t)U->n, 1);
            if (!sets[roles[r].idx])
                goto done;
            if (valid_pc(tab[roles[r].op])) {
                int e = tab[roles[r].op];
                reach(U, &e, 1, stop, 3, blk, sets[roles[r].idx]);
            }
        }
        /* flags: gen_own = vtx | clip; zex_own = (tri1 | tri2) - gen_own - light */
        gen_own = (uint8_t *)malloc((size_t)U->n);
        zex_own = (uint8_t *)malloc((size_t)U->n);
        if (!gen_own || !zex_own) {
            free(clip); free(light); free(blk);
            goto done;
        }
        for (j = 0; j < U->n; j++) {
            gen_own[j] = sets[0][j] | clip[j];
            zex_own[j] = (uint8_t)((sets[1][j] | sets[2][j]) && !gen_own[j] && !light[j]);
        }
        lightset = light;
        bzset = sets[3];
        for (r = 0; r < 2; r++) {                              /* zex first, then gen204 */
            const uint8_t *own = r == 0 ? zex_own : gen_own;
            int ev1 = 0, ev0 = 0, v;
            for (j = 0; j < U->n; j++) {
                int in1 = 0, in0 = 0;
                for (i = 0; i < nrefs; i++) {
                    int fv = r == 0 ? derive_recs[derive_refs[order[i]->ref].rec].zex : derive_recs[derive_refs[order[i]->ref].rec].gen204;
                    if (expl[i][j] && own[j]) {
                        if (fv)
                            in1 = 1;
                        else
                            in0 = 1;
                    }
                }
                if (in1 && !in0)
                    ev1++;
                if (in0 && !in1)
                    ev0++;
            }
            if (ev1 == ev0)
                v = r == 0 ? prim->rec->zex : prim->rec->gen204;
            else
                v = ev1 > ev0;
            if (r == 0)
                rec->zex = (uint8_t)v;
            else
                rec->gen204 = (uint8_t)v;
        }
        /* the composite */
        cur = (uint8_t *)malloc((size_t)U->n);
        tmp = (uint8_t *)malloc((size_t)U->n);
        if (!cur || !tmp) {
            free(clip); free(light); free(blk);
            goto done;
        }
        for (j = 0; j < U->n; j++)
            cur[j] = prim->st[j] == ST_X;
        {
            int pool[DERIVE_MAXREFS], npool = 0;
            uint8_t *okby[DERIVE_MAXREFS];
            memset(okby, 0, sizeof(okby));
            for (i = 1; i < nrefs; i++) {
                const struct derive_rec *rr = &derive_recs[derive_refs[order[i]->ref].rec];
                okby[i] = (uint8_t *)malloc((size_t)U->n);
                if (!okby[i]) {
                    int q;
                    for (q = 1; q < nrefs; q++)
                        free(okby[q]);
                    free(clip); free(light); free(blk);
                    goto done;
                }
                for (j = 0; j < U->n; j++) {
                    int usable = 1;
                    if (zex_own[j] && rr->zex != rec->zex)
                        usable = 0;
                    if (gen_own[j] && rr->gen204 != rec->gen204)
                        usable = 0;
                    okby[i][j] = expl[i][j] && usable;
                }
                pool[npool++] = i;
            }
            for (;;) {
                int ncur = 0, bestq = -1, bestn = -1, q;
                for (j = 0; j < U->n; j++)
                    ncur += cur[j];
                if (!ncur || !npool)
                    break;
                for (q = 0; q < npool; q++) {
                    int n = 0;
                    for (j = 0; j < U->n; j++)
                        n += cur[j] & okby[pool[q]][j];
                    if (n > bestn) {
                        bestn = n;
                        bestq = q;
                    }
                }
                if (bestn == 0)
                    break;
                out->contrib[out->n_contrib] = order[pool[bestq]]->ref;
                out->contrib_n[out->n_contrib++] = (unsigned)bestn;
                for (j = 0; j < U->n; j++)
                    if (okby[pool[bestq]][j])
                        cur[j] = 0;
                for (q = bestq; q + 1 < npool; q++)
                    pool[q] = pool[q + 1];
                npool--;
            }
            for (i = 1; i < nrefs; i++)
                free(okby[i]);
        }
        /* apply_rules: the branchz source */
        {
            int src_lh6 = 0, src_lw28 = 0, srcw = 0;       /* the last matching word decides */
            int want_lh;
            int src = 0;                                     /* 0 none, 1 lh 6, 2 lw 28 */
            for (j = 0; j < U->n; j++)
                if (bzset[j] && U->cls[j] == C_MEM) {
                    uint32_t w = U->w[j];
                    unsigned op = w >> 26;
                    int imm = sx16(w);
                    if (op == 0x21 && imm == 6)
                        src = 1;
                    if (op == 0x23 && imm == 28)
                        src = 2;
                }
            (void)src_lh6; (void)src_lw28; (void)srcw;
            rec->bz_z = (uint8_t)(rec->zex && !rec->gen204 && src == 2);
            want_lh = rec->zex && !rec->gen204 && !rec->bz_z;
            for (j = 0; j < U->n; j++)
                if (cur[j] && bzset[j] && U->cls[j] == C_MEM) {
                    uint32_t w = U->w[j];
                    unsigned op = w >> 26;
                    int imm = sx16(w);
                    if (want_lh ? (op == 0x21 && imm == 6) : (op == 0x23 && imm == 28))
                        cur[j] = 0;
                }
        }
        /* classify_near */
        {
            int guard = 0, nlx = 0, a, b, wa = -1;
            uint8_t *near = (uint8_t *)calloc((size_t)U->n, 1);
            if (!near) {
                free(clip); free(light); free(blk);
                goto done;
            }
            for (j = 0; j < U->n; j++)
                if (lightset[j]) {
                    if (wa >= 0) {
                        a = wa;
                        b = j;
                        if (U->cls[a] == C_ALUI && (U->w[a] >> 26) == 0x0c && (U->w[a] & 0xffff) == 0x40 && U->cls[b] == C_BR && (U->w[b] >> 26) == 4 &&
                            ((U->w[b] >> 21) & 31) == ((U->w[a] >> 16) & 31) && ((U->w[b] >> 16) & 31) == 0)
                            guard = 1;
                    }
                    wa = j;
                }
            for (j = 0; j < U->n; j++)
                if (cur[j] && lightset[j])
                    nlx++;
            for (j = 0; j < U->n; j++) {
                if (!cur[j])
                    continue;
                if (U->pc[j] >= 0x80 && (int)U->pc[j] < head && U->seg[j] == 0 && (U->flg[j] >> 1 & 1) && (U->cls[j] == C_OTHER || U->cls[j] == C_ALUI)) {
                    near[j] = 1 + NEAR_INIT;
                } else if (lightset[j] && guard) {
                    near[j] = 1 + NEAR_POS;
                } else if (lightset[j] && nlx <= 16) {
                    near[j] = 1 + NEAR_SMALL;
                }
            }
            for (j = 0; j < U->n; j++)
                if (near[j]) {
                    out->n_near[near[j] - 1]++;
                    cur[j] = 0;
                }
            free(near);
        }
        for (j = 0; j < U->n; j++)
            out->n_x += cur[j];
        /* ref_sets: the reference's optional handlers (G_SPECIAL_1, G_TRI4) that the unknown has no command for */
        {
            const struct derive_ref *pr = &derive_refs[prim->ref];
            int role;
            for (role = 0; role < 2; role++) {
                int fld = role == 0 ? PC_MVP : PC_TRI4;
                if (prim->rec->pc[fld] && rec->pc[fld] == 0) {
                    uint8_t *s = (uint8_t *)calloc((size_t)prim->R.n, 1);
                    unsigned q;
                    if (!s) {
                        free(clip); free(light); free(blk);
                        goto done;
                    }
                    for (q = 0; q < pr->set_n[role]; q++)
                        s[derive_sets[pr->set_off[role] + q]] = 1;
                    xrs[role] = s;
                }
            }
        }
        /* xr_residual */
        {
            const struct dimg *R = &prim->R;
            int nrr = 0;
            uint8_t *cat = (uint8_t *)calloc((size_t)R->n, 1);       /* 0 none, 1 init, 2 extra, 3 deleted */
            int i0;
            if (!cat) {
                free(clip); free(light); free(blk);
                goto done;
            }
            for (i0 = 0; i0 < R->n; i0++) {
                int run, role, skip = 0;
                if (!prim->xr[i0])
                    continue;
                run = prim->hunk[i0];
                if (run >= 0) {
                    /* all words of the replacing run explained: the run is [run, ...) of unaligned words in one segment */
                    int e = run, bad = 0;
                    while (e < U->n && prim->pu[e] < 0 && U->seg[e] == U->seg[run]) {
                        if (cur[e])
                            bad = 1;
                        e++;
                    }
                    if (!bad)
                        continue;
                }
                for (role = 0; role < 2; role++)
                    if (xrs[role] && xrs[role][i0])
                        skip = 1;
                if (skip)
                    continue;
                if (R->pc[i0] >= 0x80 && (int)R->pc[i0] < prim->rec->pc[PC_HEAD] && R->seg[i0] == 0 && (R->flg[i0] >> 1 & 1) && (R->cls[i0] == C_OTHER || R->cls[i0] == C_ALUI))
                    cat[i0] = 1;
                else
                    cat[i0] = 2;
                nrr++;
            }
            {
                int s0 = 0;
                while (s0 < R->n) {
                    if (cat[s0] == 2) {
                        int e = s0;
                        while (e < R->n && cat[e] == 2)
                            e++;
                        if (e - s0 < 16)
                            for (i0 = s0; i0 < e; i0++)
                                cat[i0] = 3;
                        s0 = e;
                    } else
                        s0++;
                }
            }
            for (i0 = 0; i0 < R->n; i0++)
                if (cat[i0])
                    out->n_xr[cat[i0] - 1]++;
            free(cat);
        }
        free(clip); free(light); free(blk);
    }
    {
        unsigned nnear = out->n_near[0] + out->n_near[1] + out->n_near[2];
        unsigned nxr = out->n_xr[0] + out->n_xr[1] + out->n_xr[2];
        out->nx_total = (int)(out->n_x + nnear + nxr);
        if (out->n_x || out->n_xr[XR_DELETED])
            out->verdict = DERIVE_NONE;
        else if (nnear || nxr)
            out->verdict = DERIVE_NEAR;
        else
            out->verdict = DERIVE_EXACT;
        if (prim->any_trouble)
            out->verdict = DERIVE_NONE;
    }
    rc = 0;
done:
    for (i = 0; i < DERIVE_MAXREFS; i++)
        free(expl[i]);
    for (i = 0; i < 10; i++)
        free(sets[i]);
    free(xrs[0]); free(xrs[1]);
    free(cur); free(tmp); free(gen_own); free(zex_own);
    for (i = 0; i < ndone; i++)
        pairing_free(&prs[i]);
    free(prs);
    (void)nex;
    return rc;
}

/* ---- the variant families (derive_f1d.derive_variant): only what decides the family and the verdict --------------------------------------------------------------------- */
static int derive_variant(const struct derive_in *in, struct dimg *U, const struct dctx *X, const int *refs, int nrefs, struct f2work *out)
{
    struct pairing *prs = (struct pairing *)calloc((size_t)nrefs, sizeof(struct pairing));
    struct pairing *order[DERIVE_MAXREFS];
    uint8_t *cur = NULL, *expl[DERIVE_MAXREFS];
    int i, j, ndone = 0, rc = -1;
    struct pairing *prim;
    (void)in;
    memset(expl, 0, sizeof(expl));
    if (!prs)
        return -1;
    for (i = 0; i < nrefs; i++) {
        if (pairing_make(&prs[i], refs[i], U, X->nkeys, &X->b2j))
            goto done;
        ndone++;
        order[i] = &prs[i];
    }
    sort_pairings(order, nrefs);
    prim = order[0];
    out->primary = prim->ref;
    out->aligned_num = (unsigned)prim->n_pr;
    DBG_PAIRING(prim, U);
    for (i = 0; i < nrefs; i++) {
        expl[i] = (uint8_t *)malloc((size_t)U->n);
        if (!expl[i])
            goto done;
        for (j = 0; j < U->n; j++)
            expl[i][j] = order[i]->st[j] != ST_X;
    }
    cur = (uint8_t *)malloc((size_t)U->n);
    if (!cur)
        goto done;
    for (j = 0; j < U->n; j++)
        cur[j] = prim->st[j] == ST_X;
    {
        int pool[DERIVE_MAXREFS], npool = 0;
        for (i = 1; i < nrefs; i++)
            pool[npool++] = i;
        for (;;) {
            int ncur = 0, bestq = -1, bestn = -1, q;
            for (j = 0; j < U->n; j++)
                ncur += cur[j];
            if (!ncur || !npool)
                break;
            for (q = 0; q < npool; q++) {
                int n = 0;
                for (j = 0; j < U->n; j++)
                    n += cur[j] & expl[pool[q]][j];
                if (n > bestn) {
                    bestn = n;
                    bestq = q;
                }
            }
            if (bestn == 0)
                break;
            out->contrib[out->n_contrib] = order[pool[bestq]]->ref;
            out->contrib_n[out->n_contrib++] = (unsigned)bestn;
            for (j = 0; j < U->n; j++)
                if (expl[pool[bestq]][j])
                    cur[j] = 0;
            for (q = bestq; q + 1 < npool; q++)
                pool[q] = pool[q + 1];
            npool--;
        }
    }
    for (j = 0; j < U->n; j++)
        out->n_x += cur[j];
    {
        /* xr = {i: extra for i in prim.xr if i not in hunk_of_r or any(j in res.x for j in hunk_of_r[i])}; runs under 16 words are deleted */
        const struct dimg *R = &prim->R;
        uint8_t *cat = (uint8_t *)calloc((size_t)R->n, 1);
        int i0, nxr = 0;
        if (!cat)
            goto done;
        for (i0 = 0; i0 < R->n; i0++) {
            int run;
            if (!prim->xr[i0])
                continue;
            run = prim->hunk[i0];
            if (run >= 0) {
                int e = run, bad = 0;
                while (e < U->n && prim->pu[e] < 0 && U->seg[e] == U->seg[run]) {
                    if (cur[e])
                        bad = 1;
                    e++;
                }
                if (!bad)
                    continue;
            }
            cat[i0] = 2;
        }
        {
            int s0 = 0;
            while (s0 < R->n) {
                if (cat[s0] == 2) {
                    int e = s0;
                    while (e < R->n && cat[e] == 2)
                        e++;
                    if (e - s0 < 16)
                        for (i0 = s0; i0 < e; i0++)
                            cat[i0] = 3;
                    s0 = e;
                } else
                    s0++;
            }
        }
        for (i0 = 0; i0 < R->n; i0++)
            if (cat[i0]) {
                out->n_xr[cat[i0] - 1]++;
                nxr++;
            }
        free(cat);
        out->nx_total = (int)(out->n_x + (unsigned)nxr);
        out->verdict = (out->n_x || out->n_xr[XR_DELETED]) ? DERIVE_NONE : nxr ? DERIVE_NEAR : DERIVE_EXACT;
    }
    rc = 0;
done:
    for (i = 0; i < DERIVE_MAXREFS; i++)
        free(expl[i]);
    free(cur);
    for (i = 0; i < ndone; i++)
        pairing_free(&prs[i]);
    free(prs);
    return rc;
}

/* ---- the driver ------------------------------------------------------------------------------------------------------------------------------------------------------- */
static unsigned long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}

/* int(round(aligned, 2) * 100): Python's round of the double num / den to 2 places (correctly rounded decimal) */
static int round2(unsigned num, unsigned den)
{
    char buf[32];
    double a = den ? (double)num / (double)den : 0.0;
    int v = 0;
    snprintf(buf, sizeof(buf), "%.2f", a);
    { const char *p = buf; for (; *p; p++) if (*p >= '0' && *p <= '9') v = v * 10 + (*p - '0'); }
    return v;
}

int derive_run(const struct derive_in *in, struct derive_result *out)
{
    struct dimg U;
    struct dctx X;
    struct unkmap um;
    int fam, best = -1, rc = 0, refs[DERIVE_MAXREFS], nrefs;
    struct f2work *w[3];
    unsigned long long t0 = now_ns();
    int nfam = 0;
    unsigned udone[3] = { 0, 0, 0 };

    memset(out, 0, sizeof(*out));
    memset(w, 0, sizeof(w));
    if (!in->text || !in->data)
        return -1;
    um.cap = 32768;
    um.h = (uint64_t *)calloc(um.cap, sizeof(uint64_t));
    um.id = (uint32_t *)calloc(um.cap, sizeof(uint32_t));
    um.next = 0;
    if (!um.h || !um.id) {
        free(um.h); free(um.id);
        return -1;
    }
    if (img_unknown(&U, in, &um)) {
        free(um.h); free(um.id);
        return -1;
    }
    memset(&X, 0, sizeof(X));
    X.nkeys = (int)derive_n_shapes + (int)um.next + 1;
    {
        int64_t *kb = (int64_t *)malloc((size_t)(U.n + 1) * sizeof(int64_t));
        int j;
        if (!kb) {
            img_free(&U);
            free(um.h); free(um.id);
            return -1;
        }
        for (j = 0; j < U.n; j++)
            kb[j] = U.cls[j] == C_NOP ? -2 * (int64_t)(j + 1) - 1 : (int64_t)U.sid[j];
        if (csr_build(&X.b2j, X.nkeys, kb, U.n)) {
            free(kb);
            img_free(&U);
            free(um.h); free(um.id);
            return -1;
        }
        free(kb);
    }
    for (fam = 0; fam < 3; fam++) {
        if (in->family != DERIVE_AUTO && in->family != fam)
            continue;
        if (!in->full && in->family == DERIVE_AUTO && fam > 0 && w[0] && w[0]->aligned_num * 100u >= 60u * (unsigned)U.n && w[0]->verdict != DERIVE_NONE)
            continue;                                   /* the F3DEX2 family explains the text; the other families cannot beat it */
        nrefs = refs_of(in, fam, refs);
        if (!nrefs)
            continue;
        w[fam] = (struct f2work *)calloc(1, sizeof(struct f2work));
        if (!w[fam]) {
            rc = -1;
            break;
        }
        U.active = ~0u;
        if (fam == DERIVE_F3DEX2)
            rc = derive_f2(in, &U, &X, refs, nrefs, w[fam]);
        else
            rc = derive_variant(in, &U, &X, refs, nrefs, w[fam]);
        if (rc)
            break;
        udone[fam] = 1;
        nfam++;
        out->fam_aligned[fam][0] = w[fam]->aligned_num;
        out->fam_aligned[fam][1] = (unsigned)U.n;
        out->fam_nx[fam] = (unsigned)w[fam]->nx_total;
        out->fam_verdict[fam] = w[fam]->verdict;
    }
    if (!rc) {
        for (fam = 0; fam < 3; fam++) {
            if (!udone[fam])
                continue;
            if (best < 0) {
                best = fam;
                continue;
            }
            {
                int ra = round2(w[fam]->aligned_num, (unsigned)U.n), rb = round2(w[best]->aligned_num, (unsigned)U.n);
                if (ra > rb || (ra == rb && w[fam]->nx_total < w[best]->nx_total))
                    best = fam;
            }
        }
        if (best < 0)
            rc = -1;
    }
    if (!rc) {
        const struct f2work *b = w[best];
        int i;
        out->family = best;
        out->verdict = b->verdict;
        out->aligned_num = b->aligned_num;
        out->aligned_den = (unsigned)U.n;
        out->nx = (unsigned)b->nx_total;
        out->n_x = (unsigned)b->n_x;
        for (i = 0; i < 3; i++) {
            out->n_near[i] = b->n_near[i];
            out->n_xr[i] = b->n_xr[i];
        }
        out->primary = b->primary;
        out->n_contrib = b->n_contrib;
        for (i = 0; i < b->n_contrib; i++) {
            out->contrib[i].ref = b->contrib[i];
            out->contrib[i].n = b->contrib_n[i];
        }
        out->knob_trouble = b->trouble;
        if (best == DERIVE_F3DEX2)
            out->rec = b->rec;
    }
    for (fam = 0; fam < 3; fam++)
        free(w[fam]);
    (void)nfam;
    csr_free(&X.b2j);
    img_free(&U);
    free(um.h);
    free(um.id);
    out->work_ns = now_ns() - t0;
    return rc;
}

/* ---- field names ----------------------------------------------------------------------------------------------------------------------------------------------------- */
static const char *const pc_names[DERIVE_NPC] = { "head", "copy", "sync", "segcopy", "ef", "ed", "tri1", "tri2", "vtx", "mtx", "geom", "enddl", "half2", "modvtx", "culldl", "branchz", "mvp", "tri4", "ldtx_spr", "ldtx_rect" };
static const char *const f2pcs_names[DERIVE_NF2PCS] = { "dl", "texture", "texrect", "half1", "moveword", "popmtx", "movemem", "omh", "oml" };
static const char *const lay_names[DERIVE_NLAY] = { "mv", "p", "mvp", "persp_norm", "viewport_scale", "viewport_trans", "clip_ratio", "const31", "const30", "fog_mul", "fog_ofs", "texture",
    "tex_scale_s", "tex_scale_t", "geom", "mvp_valid", "lights_valid", "lights_size", "light_raw", "light_base", "tg", "light_scale", "light_scale_zex", "vtab", "clipmask", "culltab", "in",
    "in_end", "jump_table", "movemem_tab", "moveword_adj", "desc_light", "desc_clip", "clip_planes", "clip_masks", "out_toggle", "geom_def", "mw_max", "out_bytes", "pipe_sync", "no_lit", "lit_vtx" };

void derive_rec_fields(const struct derive_rec *r, void (*f)(const char *name, unsigned long long v, void *arg), void *arg)
{
    char nm[64];
    int k;
    for (k = 0; k < DERIVE_NPC; k++) {
        snprintf(nm, sizeof(nm), "pc.%s", pc_names[k]);
        f(nm, (unsigned long long)(long long)r->pc[k] & 0xffffffffull, arg);
    }
    for (k = 0; k < DERIVE_NF2PCS; k++) {
        snprintf(nm, sizeof(nm), "f2pcs.%s", f2pcs_names[k]);
        f(nm, (unsigned long long)(long long)r->f2pcs[k] & 0xffffffffull, arg);
    }
    for (k = 0; k < DERIVE_NLAY; k++) {
        snprintf(nm, sizeof(nm), "lay.%s", lay_names[k]);
        f(nm, r->lay[k], arg);
    }
    f("gen204", r->gen204, arg);
    f("zex", r->zex, arg);
    f("bz_z", r->bz_z, arg);
    f("reject_mask", r->reject_mask, arg);
    f("load_key", r->load_key, arg);
}
