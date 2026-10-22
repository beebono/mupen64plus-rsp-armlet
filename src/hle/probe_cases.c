/*
 * arMLEt - probe_cases.c: the canned display lists of the probe (probe.h), after tools/hle/mksyn_ex2.py
 *
 * A case is a block of RDRAM bytes (the display list at offset 0, matrices, vertices, lights, a viewport behind it) for an F3DEX2-family microcode. Everything is made from a seed with a
 * small PRNG of its own (the same bytes on every host). The cases of mksyn_ex2.py are ported without the ones that need another microcode (`load`, `s2`) or that the front-ends decline on
 * purpose (`bad`); `rej` and `bz` are new: they aim at the two things the derivation cannot be sure of from the text alone, the reject mask and the source of G_BRANCH_Z (docs/ucode-derive.md),
 * with inputs that make the two candidate values give different RDP streams every time. The S2DEX 2.x cases (s2rect, s2ldtx, s2bg, s2copy, s2misc) are mksyn_ex2.py's `s2` stage split by
 * command group, without the commands the front-end declines (LDTX_SPRITE, LDTX_RECT_R, the sprite commands) and the undefined opcodes.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <string.h>

#include "probe_cases.h"

/* The case generators stay out of line: inlined into one probe_case_make_*, GCC 15+ -O3 spends minutes completely peeling their short loops. */
#define NOINLINE __attribute__((noinline))

struct rng {
    uint64_t s;
};

static uint32_t rnd(struct rng *r)
{
    r->s ^= r->s >> 12;
    r->s ^= r->s << 25;
    r->s ^= r->s >> 27;
    return (uint32_t)((r->s * 0x2545f4914f6cdd1dull) >> 32);
}
static unsigned randrange(struct rng *r, unsigned n) { return n ? rnd(r) % n : 0; }
static int randint(struct rng *r, int a, int b) { return a + (int)randrange(r, (unsigned)(b - a + 1)); }
static double uniform(struct rng *r, double a, double b) { return a + (b - a) * (rnd(r) / 4294967296.0); }
static double frnd(struct rng *r) { return rnd(r) / 4294967296.0; }
#define CHOICE(r, arr) ((arr)[randrange((r), sizeof(arr) / sizeof((arr)[0]))])

struct dl {
    uint32_t w[512][2];
    unsigned n;
};

static void cmd(struct dl *d, uint32_t w0, uint32_t w1) { d->w[d->n][0] = w0; d->w[d->n][1] = w1; d->n++; }

static void put(struct probe_case *c, unsigned off, const void *p, unsigned n)
{
    if (off + n > PROBE_MEM)
        return;
    memcpy(c->mem + off, p, n);
    if (off + n > c->used)
        c->used = off + n;
}

static void be16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* mtx(vals): the N64 matrix of 16 numbers (row-major): the 16 integer halfwords, then the 16 fractions */
static void mtx(uint8_t *out, const double *v)
{
    unsigned i;
    for (i = 0; i < 16; i++) {
        int64_t fx = (int64_t)floor(v[i] * 65536.0 + 0.5);
        be16(out + 2 * i, (unsigned)((fx >> 16) & 0xffff));
        be16(out + 32 + 2 * i, (unsigned)(fx & 0xffff));
    }
}

static void vtx(uint8_t *out, int x, int y, int z, int s, int t, const uint8_t col[4])
{
    be16(out, (unsigned)x & 0xffff);
    be16(out + 2, (unsigned)y & 0xffff);
    be16(out + 4, (unsigned)z & 0xffff);
    be16(out + 6, 0);
    be16(out + 8, (unsigned)s & 0xffff);
    be16(out + 10, (unsigned)t & 0xffff);
    memcpy(out + 12, col, 4);
}

static void rnd_matrix(struct rng *r, double scale, double *m)
{
    static const double ch[7] = { 0, 0, 1, -1, 2, 0.5, -0.5 };
    unsigned i, k;
    for (i = 0; i < 16; i++)
        m[i] = 0;
    for (i = 0; i < 3; i++)
        for (k = 0; k < 4; k++) {
            unsigned q = randrange(r, 8);
            m[4 * i + k] = (q < 7 ? ch[q] : uniform(r, -2, 2)) * scale;
        }
    m[12] = uniform(r, -4, 4) * scale;
    m[13] = uniform(r, -4, 4) * scale;
    m[14] = uniform(r, -4, 4) * scale;
    m[15] = uniform(r, -2, 6) * scale;
}

static void setup(struct dl *d, struct rng *r, uint32_t base, uint32_t geom)
{
    static const uint32_t pn[4] = { 0xffff, 0x7fff, 0x0100, 0x4000 };
    cmd(d, 0xdb060000, 0);                             /* segment 0 = 0 */
    cmd(d, 0xdb040004, 3);                             /* the clip ratio: x, y */
    cmd(d, 0xdb04000c, 3);
    cmd(d, 0xdb040014, (uint32_t)-3);
    cmd(d, 0xdb04001c, (uint32_t)-3);
    cmd(d, 0xdb0e0000, CHOICE(r, pn));                 /* the perspective normalisation */
    cmd(d, 0xd9ffffff, 0);                             /* clear the geometry mode, then set it */
    cmd(d, 0xd9ffffff, geom);
    cmd(d, 0xdc080008, base + 0x400);                  /* the viewport */
}

static void common(struct probe_case *c, struct dl *d, struct rng *r, uint32_t geom, int trans_z, int tex)
{
    static const uint32_t tx[4] = { 0x80008000, 0xffffffff, 0x20004000, 0x00010001 };
    uint8_t vp[16];
    setup(d, r, c->base, geom);
    be16(vp, 160 * 4); be16(vp + 2, 120 * 4); be16(vp + 4, 0x1ff); be16(vp + 6, 0);
    be16(vp + 8, 160 * 4); be16(vp + 10, 120 * 4); be16(vp + 12, (unsigned)trans_z); be16(vp + 14, 0);
    put(c, 0x400, vp, 16);
    if (tex)
        cmd(d, 0xd7000002 | (randrange(r, 8) << 8), CHOICE(r, tx));
}

static void ident(double *m)
{
    unsigned i;
    for (i = 0; i < 16; i++)
        m[i] = (i % 5) == 0;
}

static void rcol(struct rng *r, uint8_t col[4])
{
    unsigned i;
    for (i = 0; i < 4; i++)
        col[i] = (uint8_t)randrange(r, 256);
}

/* three distinct vertex numbers out of v0 .. v0 + n - 1 */
static void sample3(struct rng *r, unsigned v0, unsigned n, unsigned *a, unsigned *b, unsigned *c)
{
    *a = v0 + randrange(r, n);
    do { *b = v0 + randrange(r, n); } while (*b == *a);
    do { *c = v0 + randrange(r, n); } while (*c == *a || *c == *b);
}

static void tri1(struct dl *d, unsigned a, unsigned b, unsigned c) { cmd(d, (0x05u << 24) | (a * 2 << 16) | (b * 2 << 8) | c * 2, 0); }

NOINLINE static void geometry_cases(struct probe_case *c, struct dl *d, struct rng *r, unsigned kind)
{
    static const uint32_t shades[3] = { 0, 0x4, 0x4 | 0x200000 };
    static const uint32_t culls[5] = { 0, 0, 0x400, 0x200, 0x600 };
    uint32_t shade = CHOICE(r, shades), cull = CHOICE(r, culls), geom;
    unsigned zb = randrange(r, 2), tex = randrange(r, 2) ? 2 : 0, nbatch = kind == PROBE_MIX ? 4 : 3, b, vaddr = 0x600;
    int trans_z;
    double mv[16], pj[16], scale, span0;
    static const double sc_clip[8] = { 1, 1, 0.5, 0.25, 1.0 / 16, 1.0 / 64, 1.0 / 256, 1.0 / 1024 };
    static const double sc_zneg[2] = { 1, 0.25 };
    static const double sc_wild[6] = { 1, 2, 4, 16, 64, 256 };
    static const double sc_mix[3] = { 1, 0.5, 1.0 / 8 };
    static const int tz[3] = { 0x1ff, 0, 0x100 };
    uint8_t buf[1024];
    geom = shade | zb | cull | 0x800000;
    {
        static const double sp0[4] = { 4, 10, 30, 100 };
        span0 = CHOICE(r, sp0);
    }
    trans_z = kind == PROBE_ZNEG ? 0 : CHOICE(r, tz);
    common(c, d, r, geom, trans_z, tex);
    scale = kind == PROBE_CLIP ? CHOICE(r, sc_clip) : kind == PROBE_ZNEG ? CHOICE(r, sc_zneg) : kind == PROBE_WILD ? CHOICE(r, sc_wild) : CHOICE(r, sc_mix);
    ident(mv);
    if (kind == PROBE_ZNEG) {                           /* w = 1 + 0.5 z: points with z in (-1, 0) are in front of the near plane at negative screen z */
        memset(pj, 0, sizeof(pj));
        pj[0] = pj[5] = pj[10] = scale;
        pj[11] = 0.5 * scale;
        pj[15] = scale;
    } else {
        rnd_matrix(r, scale, pj);
        if (kind != PROBE_WILD) {                       /* a w that keeps most of the vertices (about span0 from the origin) inside the guard band: the output is not empty */
            pj[15] = scale * span0 * uniform(r, 1.2, 3.0);
            pj[12] = uniform(r, -0.5, 0.5) * scale * span0;
            pj[13] = uniform(r, -0.5, 0.5) * scale * span0;
            pj[14] = uniform(r, -0.5, 0.5) * scale * span0;
        }
    }
    mtx(buf, mv);
    put(c, 0x500, buf, 64);
    mtx(buf, pj);
    put(c, 0x540, buf, 64);
    cmd(d, 0xda380007, c->base + 0x540);
    cmd(d, 0xda380003, c->base + 0x500);
    for (b = 0; b < nbatch; b++) {
        static const unsigned ns[8] = { 3, 3, 4, 5, 6, 9, 11, 16 };
        static const double sp_z[2] = { 0.5, 1.0 };
        static const double sp_w[3] = { 2000, 10000, 32767 };
        static const int s1[5] = { 1, 2, 5, 20, 100 };
        static const int s2[3] = { 100, 2000, 32768 };
        static const int ends[6] = { -32768, -32767, 32767, 0, 1, -1 };
        static const int zz[4] = { -1, 0, 0, 1 };
        unsigned n = kind == PROBE_MIX ? 1 + randrange(r, 11) : CHOICE(r, ns), v0, i, nt, q;
        double span = kind == PROBE_ZNEG ? CHOICE(r, sp_z) : kind == PROBE_WILD ? CHOICE(r, sp_w) : span0 * uniform(r, 0.5, 1.0), ctr[3];
        uint8_t vs[16 * 16];
        if (kind == PROBE_MIX)
            v0 = randrange(r, 2) ? 32 - n : randrange(r, 32 - n + 1);
        else
            v0 = randrange(r, 32 - n + 1);
        for (i = 0; i < 3; i++)
            ctr[i] = uniform(r, -span, span);
        for (i = 0; i < n; i++) {
            int x, y, z, k;
            uint8_t col[4];
            if (kind == PROBE_ZNEG) {
                x = (int)uniform(r, -8, 8);
                y = (int)uniform(r, -8, 8);
                (void)uniform(r, -1.4, 0.8);
                z = CHOICE(r, zz);
            } else {
                int sp = kind == PROBE_WILD ? CHOICE(r, s2) : (int)(span0 * 0.5) + CHOICE(r, s1) % 3, cc[3];
                for (k = 0; k < 3; k++) {
                    double v = ctr[k] + uniform(r, -sp, sp);
                    int iv = (int)v;
                    cc[k] = iv < -32768 ? -32768 : iv > 32767 ? 32767 : iv;
                }
                x = cc[0]; y = cc[1]; z = cc[2];
                if (kind == PROBE_WILD) {
                    if (frnd(r) < 0.25) x = CHOICE(r, ends);
                    if (frnd(r) < 0.25) y = CHOICE(r, ends);
                    if (frnd(r) < 0.25) z = CHOICE(r, ends);
                }
            }
            rcol(r, col);
            vtx(vs + 16 * i, x, y, z, randint(r, -1024, 1023), randint(r, -1024, 1023), col);
        }
        put(c, vaddr, vs, 16 * n);
        cmd(d, (0x01u << 24) | (n << 12) | ((v0 + n) << 1), c->base + vaddr);
        vaddr += 0x100;
        nt = kind == PROBE_CLIP ? 12 : 2 + randrange(r, 10);
        for (q = 0; q < nt; q++) {
            unsigned a, b2, cc;
            if (n < 3)
                continue;
            sample3(r, v0, n, &a, &b2, &cc);
            if (frnd(r) < 0.3) {
                unsigned a2 = v0 + randrange(r, n), b3 = v0 + randrange(r, n), c3 = v0 + randrange(r, n);
                cmd(d, (0x06u << 24) | (a * 2 << 16) | (b2 * 2 << 8) | cc * 2, (a2 * 2 << 16) | (b3 * 2 << 8) | c3 * 2);
            } else
                tri1(d, a, b2, cc);
        }
    }
    cmd(d, 0xdf000000, 0);
}

NOINLINE static void misc_case(struct probe_case *c, struct dl *d, struct rng *r)
{
    static const double sc[3] = { 1, 0.5, 1.0 / 8 };
    static const int tz[2] = { 0x1ff, 0 };
    static const uint32_t thr[4] = { 0, 0x10000, 0x40000000, 0xffff0000 };
    static const unsigned mv_off[7] = { 0x10, 0x14, 0x18, 0x1c, 0x00, 0x04, 0x08 };
    static const uint32_t gm[6] = { 0, 0x200000, 0x400, 0x200, 4, 1 };
    uint32_t geom = 0x4 | 0x200000 | 1 | 0x800000;
    double m[16];
    uint8_t buf[512];
    unsigned n = CHOICE(r, ((const unsigned[]){ 6, 9, 16 })), v0 = randrange(r, 32 - n + 1), i, nk;
    struct dl sub, sub2;

    common(c, d, r, geom, CHOICE(r, tz), 2);
    ident(m);
    mtx(buf, m);
    put(c, 0x500, buf, 64);
    rnd_matrix(r, CHOICE(r, sc), m);
    mtx(buf, m);
    put(c, 0x540, buf, 64);
    rnd_matrix(r, 0.25, m);
    mtx(buf, m);
    put(c, 0x580, buf, 64);
    cmd(d, 0xda380007, c->base + 0x540);
    cmd(d, 0xda380003, c->base + 0x500);
    for (i = 0; i < n; i++) {
        uint8_t col[4];
        rcol(r, col);
        vtx(buf + 16 * i, randint(r, -30, 30), randint(r, -30, 30), randint(r, -30, 30), randint(r, -512, 511), randint(r, -512, 511), col);
    }
    put(c, 0x600, buf, 16 * n);
    cmd(d, (0x01u << 24) | (n << 12) | ((v0 + n) << 1), c->base + 0x600);
    memset(&sub, 0, sizeof(sub));
    memset(&sub2, 0, sizeof(sub2));
    cmd(&sub, 0xf7000000, 0x11111111);
    cmd(&sub, 0xdf000000, 0);
    cmd(&sub2, 0xf7000000, 0x22222222);
    cmd(&sub2, (0x05u << 24) | (v0 * 2 << 16) | ((v0 + 1) * 2 << 8) | (v0 + 2) * 2, 0);
    cmd(&sub2, 0xdf000000, 0);
    {
        unsigned k;
        uint8_t t[64];
        for (k = 0; k < sub.n; k++) { t[8 * k] = (uint8_t)(sub.w[k][0] >> 24); t[8 * k + 1] = (uint8_t)(sub.w[k][0] >> 16); t[8 * k + 2] = (uint8_t)(sub.w[k][0] >> 8); t[8 * k + 3] = (uint8_t)sub.w[k][0];
            t[8 * k + 4] = (uint8_t)(sub.w[k][1] >> 24); t[8 * k + 5] = (uint8_t)(sub.w[k][1] >> 16); t[8 * k + 6] = (uint8_t)(sub.w[k][1] >> 8); t[8 * k + 7] = (uint8_t)sub.w[k][1]; }
        put(c, 0x800, t, sub.n * 8);
        for (k = 0; k < sub2.n; k++) { t[8 * k] = (uint8_t)(sub2.w[k][0] >> 24); t[8 * k + 1] = (uint8_t)(sub2.w[k][0] >> 16); t[8 * k + 2] = (uint8_t)(sub2.w[k][0] >> 8); t[8 * k + 3] = (uint8_t)sub2.w[k][0];
            t[8 * k + 4] = (uint8_t)(sub2.w[k][1] >> 24); t[8 * k + 5] = (uint8_t)(sub2.w[k][1] >> 16); t[8 * k + 6] = (uint8_t)(sub2.w[k][1] >> 8); t[8 * k + 7] = (uint8_t)sub2.w[k][1]; }
        put(c, 0x900, t, sub2.n * 8);
    }
    nk = 8 + randrange(r, 12);
    for (i = 0; i < nk; i++) {
        unsigned k = randrange(r, 12), va = v0 + randrange(r, n), vb = v0 + randrange(r, n);
        switch (k) {
        case 0: {
            unsigned a = va < vb ? va : vb, b = va < vb ? vb : va;
            cmd(d, (0x03u << 24) | (a * 2), b * 2);
            break;
        }
        case 1:
            cmd(d, 0xe1000000, c->base + 0x900);
            cmd(d, (0x04u << 24) | ((va * 5) << 12) | ((vb * 2) & 0xfff), randrange(r, 5) < 4 ? CHOICE(r, thr) : rnd(r));
            break;
        case 2:
            cmd(d, (0x02u << 24) | (CHOICE(r, mv_off) << 16) | (va * 2), rnd(r));
            break;
        case 3: {
            unsigned q[4], j;
            for (j = 0; j < 4; j++)
                q[j] = v0 + randrange(r, n);
            cmd(d, (0x07u << 24) | (q[0] * 2 << 16) | (q[1] * 2 << 8) | q[2] * 2, (q[0] * 2 << 16) | (q[2] * 2 << 8) | q[3] * 2);
            break;
        }
        case 4: {
            static const uint32_t sp1[3] = { 0, 1, 0xff };
            cmd(d, 0xd5000000 | CHOICE(r, sp1), 0);
            break;
        }
        case 5:
            cmd(d, 0xda380001, c->base + 0x580);
            cmd(d, 0xda380000, c->base + 0x500);
            cmd(d, 0xd8380002, 64);
            break;
        case 6:
            cmd(d, 0xde000000, c->base + 0x800);
            break;
        case 7:
            cmd(d, 0xde010000, c->base + 0x800);
            break;
        case 8: {
            static const uint32_t oh[2] = { 0, 0x30000000 };
            cmd(d, 0xe3000a01, CHOICE(r, oh));
            cmd(d, 0xe2001e01, 0x00500000);
            break;
        }
        case 9: {
            static const uint32_t tx[2] = { 0x80008000, 0x40004000 };
            cmd(d, 0xd7000002 | (randrange(r, 8) << 8), CHOICE(r, tx));
            break;
        }
        case 10:
            cmd(d, 0xd9ffffff, CHOICE(r, gm));
            cmd(d, 0xd9000000 | randrange(r, 1u << 24), 0);
            break;
        default:
            tri1(d, va, vb, v0 + randrange(r, n));
            break;
        }
    }
    cmd(d, 0xdf000000, 0);
}

NOINLINE static void lit_case(struct probe_case *c, struct dl *d, struct rng *r)
{
    static const uint32_t tg[3] = { 0, 0x40000, 0x40000 | 0x80000 };
    static const uint32_t fg[2] = { 0, 0x10000 };
    static const int tz[2] = { 0x1ff, 0 };
    static const double sc[3] = { 1, 0.5, 1.0 / 8 };
    static const unsigned ns[5] = { 3, 4, 5, 8, 11 };
    uint32_t gm = 0x4 | 0x200000 | 1 | 0x20000 | CHOICE(r, tg) | CHOICE(r, fg) | 0x800000;
    unsigned nl = 1 + randrange(r, 5), slot, b, vaddr = 0x600;
    double m[16];
    uint8_t buf[512];

    common(c, d, r, gm, CHOICE(r, tz), 2);
    cmd(d, 0xdb020000, nl * 24);                        /* G_MW_NUMLIGHT */
    for (slot = 0; slot < nl + 3; slot++) {
        unsigned ofs = slot < 2 ? 24 * slot : 0x30 + 24 * (slot - 2);
        uint8_t blob[16];
        if (slot >= 2 && slot - 2 == nl)
            ofs = 0x30 + 24 * nl;
        memset(blob, 0, sizeof(blob));
        if (slot >= 2) {
            uint8_t col[4] = { (uint8_t)randrange(r, 256), (uint8_t)randrange(r, 256), (uint8_t)randrange(r, 256), 0 };
            uint8_t dir[4] = { (uint8_t)randrange(r, 256), (uint8_t)randrange(r, 256), (uint8_t)randrange(r, 256), 0 };
            memcpy(blob, col, 4);
            memcpy(blob + 4, col, 4);
            memcpy(blob + 8, dir, 4);
        } else
            blob[8 + (slot == 0 ? 0 : 1)] = 0x7f;
        put(c, 0x1000 + 0x20 * slot, blob, 16);
        cmd(d, 0xdc000000 | (1u << 19) | ((ofs / 8) << 8) | 0x0a, c->base + 0x1000 + 0x20 * slot);
    }
    ident(m);
    mtx(buf, m);
    put(c, 0x500, buf, 64);
    rnd_matrix(r, CHOICE(r, sc), m);
    mtx(buf, m);
    put(c, 0x540, buf, 64);
    cmd(d, 0xda380007, c->base + 0x540);
    cmd(d, 0xda380003, c->base + 0x500);
    for (b = 0; b < 2; b++) {
        unsigned n = CHOICE(r, ns), v0 = randrange(r, 32 - n + 1), i, q;
        uint8_t vs[16 * 16];
        for (i = 0; i < n; i++) {
            int nr[3], k;
            uint8_t col[4];
            for (k = 0; k < 3; k++)
                nr[k] = randint(r, -127, 127);
            col[0] = (uint8_t)nr[0]; col[1] = (uint8_t)nr[1]; col[2] = (uint8_t)nr[2]; col[3] = (uint8_t)randrange(r, 256);
            vtx(vs + 16 * i, randint(r, -200, 200), randint(r, -200, 200), randint(r, -200, 200), randint(r, -1024, 1023), randint(r, -1024, 1023), col);
        }
        put(c, vaddr, vs, 16 * n);
        cmd(d, (0x01u << 24) | (n << 12) | ((v0 + n) << 1), c->base + vaddr);
        vaddr += 0x100;
        for (q = 0; q < 4; q++) {
            unsigned a, b2, cc;
            sample3(r, v0, n, &a, &b2, &cc);
            tri1(d, a, b2, cc);
        }
    }
    cmd(d, 0xdf000000, 0);
}

NOINLINE static void mtx_case(struct probe_case *c, struct dl *d, struct rng *r)
{
    static const uint32_t gm1[2] = { 0, 0x4 };
    static const int tz[2] = { 0x1ff, 0 };
    static const uint16_t sm[5] = { 0, 1, 0xffff, 0x8000, 0x7fff };
    unsigned nm = 2 + randrange(r, 4), k, i;
    uint32_t geom = CHOICE(r, gm1) | randrange(r, 2) | 0x800000;
    uint8_t buf[64], vs[64];
    common(c, d, r, geom, CHOICE(r, tz), 0);
    for (k = 0; k < nm; k++) {
        for (i = 0; i < 64; i++)
            buf[i] = (uint8_t)randrange(r, 256);
        if (randrange(r, 2))                            /* integer halfwords from a small set, fractions random */
            for (i = 0; i < 16; i++) {
                unsigned q = randrange(r, 6);
                be16(buf + 2 * i, q < 5 ? sm[q] : randrange(r, 1u << 16));
            }
        put(c, 0x500 + 0x40 * k, buf, 64);
        cmd(d, 0xda380000 | randrange(r, 8), c->base + 0x500 + 0x40 * k);
    }
    for (i = 0; i < 4; i++) {
        const uint8_t col[4] = { 1, 2, 3, 4 };
        vtx(vs + 16 * i, randint(r, -40, 40), randint(r, -40, 40), randint(r, -40, 40), randint(r, -512, 511), randint(r, -512, 511), col);
    }
    put(c, 0x600, vs, 64);
    cmd(d, (0x01u << 24) | (4u << 12) | (4u << 1), c->base + 0x600);
    cmd(d, (0x05u << 24) | (0 << 16) | (2 << 8) | 4, 0);
    cmd(d, 0xdf000000, 0);
}

/* Triangles that lie entirely beyond one clipping plane, and the display-list culls over them: the reject mask decides whether they are dropped at once, so a wrong mask gives another RDP
 * stream whatever the seed. The projection scales the model coordinates down by 8 (w = 1): a coordinate of +-100 is outside, the others (+-3) inside; in the second variant w is negative. */
NOINLINE static void rej_case(struct probe_case *c, struct dl *d, struct rng *r, unsigned seed)
{
    static const uint32_t gm1[2] = { 0, 0x4 };
    double m[16] = { 0.125, 0, 0, 0, 0, 0.125, 0, 0, 0, 0, 0.125, 0, 0, 0, 0, 1 };
    uint8_t buf[64], vs[16 * 32];
    unsigned g, i, nv = 0, v0 = 0;
    uint32_t geom = CHOICE(r, gm1) | 0x800000;
    int neg_w = (seed & 1) != 0;
    common(c, d, r, geom, 0x1ff, 0);
    if (neg_w)
        m[15] = -1;
    {
        double mv[16];
        ident(mv);
        mtx(buf, mv);
        put(c, 0x500, buf, 64);
        mtx(buf, m);
        put(c, 0x540, buf, 64);
    }
    cmd(d, 0xda380007, c->base + 0x540);
    cmd(d, 0xda380003, c->base + 0x500);
    /* groups of 3 vertices: z beyond far, z before near, x right, x left, y top, y bottom, and (both variants) vertices that are inside */
    for (g = 0; g < 7; g++) {
        for (i = 0; i < 3; i++) {
            int x = randint(r, -3, 3), y = randint(r, -3, 3), z = randint(r, -3, 3);
            uint8_t col[4];
            switch (g) {
            case 0: z = 100; break;
            case 1: z = -100; break;
            case 2: x = 100; break;
            case 3: x = -100; break;
            case 4: y = 100; break;
            case 5: y = -100; break;
            default: break;
            }
            rcol(r, col);
            vtx(vs + 16 * nv, x, y, z, 0, 0, col);
            nv++;
        }
    }
    put(c, 0x600, vs, 16 * nv);
    cmd(d, (0x01u << 24) | (nv << 12) | (nv << 1), c->base + 0x600);
    for (g = 0; g < 7; g++) {
        tri1(d, v0 + 3 * g, v0 + 3 * g + 1, v0 + 3 * g + 2);
        cmd(d, (0x05u << 24) | ((v0 + 3 * g + 2) * 2 << 16) | ((v0 + 3 * g + 1) * 2 << 8) | (v0 + 3 * g) * 2, 0);      /* (the other winding) */
    }
    /* G_CULLDL over a group ends the list it is in when the mask says all the group's vertices are outside. Each test is a list of its own (called, so that the end of one does not hide the next):
     * G_CULLDL, then the triangle of the inside group; the triangle shows whether the list ended. */
    for (g = 0; g < 6; g++) {
        uint8_t t[24];
        unsigned k;
        struct dl sub;
        memset(&sub, 0, sizeof(sub));
        cmd(&sub, (0x03u << 24) | ((v0 + 3 * g) * 2), (v0 + 3 * g + 2) * 2);
        tri1(&sub, v0 + 18, v0 + 19, v0 + 20);
        cmd(&sub, 0xdf000000, 0);
        for (k = 0; k < 3; k++) {
            t[8 * k] = (uint8_t)(sub.w[k][0] >> 24); t[8 * k + 1] = (uint8_t)(sub.w[k][0] >> 16); t[8 * k + 2] = (uint8_t)(sub.w[k][0] >> 8); t[8 * k + 3] = (uint8_t)sub.w[k][0];
            t[8 * k + 4] = (uint8_t)(sub.w[k][1] >> 24); t[8 * k + 5] = (uint8_t)(sub.w[k][1] >> 16); t[8 * k + 6] = (uint8_t)(sub.w[k][1] >> 8); t[8 * k + 7] = (uint8_t)sub.w[k][1];
        }
        put(c, 0xa00 + 0x20 * g, t, 24);
        cmd(d, 0xde000000, c->base + 0xa00 + 0x20 * g);
    }
    cmd(d, 0xdf000000, 0);
}

/* G_BRANCH_Z against thresholds between the clip w (an integer, here 1) and the screen z (16.16, here far above): the build either reads the one or the other, and the branch is taken for some
 * of the thresholds with the one only. A triangle behind each branch and a list at the target show which way it went. */
NOINLINE static void bz_case(struct probe_case *c, struct dl *d, struct rng *r, unsigned seed)
{
    static const uint32_t thr[10] = { 0, 1, 2, 5, 0x100, 0x10000, 0x20000, 0x1ff0000, 0x7fffffff, 0xffff0000 };
    static const int tz[2] = { 0x1ff, 0x100 };
    double mv[16], pj[16];
    uint8_t buf[64], vs[16 * 8];
    unsigned i, n = 6, v0 = seed & 1 ? 20 : 4, t;
    uint32_t geom = 0x4 | 0x200000 | 1 | 0x800000;
    struct dl sub;
    uint8_t tb[16];
    common(c, d, r, geom, CHOICE(r, tz), 2);
    ident(mv);
    mtx(buf, mv);
    put(c, 0x500, buf, 64);
    rnd_matrix(r, 0.5, pj);
    pj[15] = 1 + uniform(r, 0, 3);
    mtx(buf, pj);
    put(c, 0x540, buf, 64);
    cmd(d, 0xda380007, c->base + 0x540);
    cmd(d, 0xda380003, c->base + 0x500);
    for (i = 0; i < n; i++) {
        uint8_t col[4];
        rcol(r, col);
        vtx(vs + 16 * i, randint(r, -6, 6), randint(r, -6, 6), randint(r, 0, 12), randint(r, -512, 511), randint(r, -512, 511), col);
    }
    put(c, 0x600, vs, 16 * n);
    cmd(d, (0x01u << 24) | (n << 12) | ((v0 + n) << 1), c->base + 0x600);
    memset(&sub, 0, sizeof(sub));
    cmd(&sub, 0xf7000000, 0x33333333);
    cmd(&sub, 0xdf000000, 0);
    for (t = 0; t < 2; t++) {
        tb[8 * t] = (uint8_t)(sub.w[t][0] >> 24); tb[8 * t + 1] = (uint8_t)(sub.w[t][0] >> 16); tb[8 * t + 2] = (uint8_t)(sub.w[t][0] >> 8); tb[8 * t + 3] = (uint8_t)sub.w[t][0];
        tb[8 * t + 4] = (uint8_t)(sub.w[t][1] >> 24); tb[8 * t + 5] = (uint8_t)(sub.w[t][1] >> 16); tb[8 * t + 6] = (uint8_t)(sub.w[t][1] >> 8); tb[8 * t + 7] = (uint8_t)sub.w[t][1];
    }
    put(c, 0x900, tb, 16);
    for (t = 0; t < 10; t++) {
        unsigned va = v0 + (t % n);
        cmd(d, 0xe1000000, c->base + 0x900);
        cmd(d, (0x04u << 24) | ((va * 5) << 12) | ((va * 2) & 0xfff), thr[t]);
        tri1(d, v0, v0 + 1, v0 + 2);
        cmd(d, 0xd7000002 | ((t & 7) << 8), 0x80008000);      /* (a state change behind the branch: another stream when the branch is not taken) */
    }
    cmd(d, 0xdf000000, 0);
}

/* ---- S2DEX 2.x (after mksyn_ex2.py's s2 stage) ------------------------------------------------------------------------------------------------------------------------------ */
static void be32(uint8_t *p, uint32_t v) { be16(p, v >> 16); be16(p + 2, v & 0xffff); }

static void put_dl(struct probe_case *c, unsigned off, const struct dl *d)
{
    uint8_t t[8 * 16];
    unsigned k;
    for (k = 0; k < d->n && k < 16; k++) {
        be32(t + 8 * k, d->w[k][0]);
        be32(t + 8 * k + 4, d->w[k][1]);
    }
    put(c, off, t, 8 * k);
}

/* a uObjSprite (24 bytes): tame: on the screen, with the scales a game uses */
static void s2_sprite(struct rng *r, uint8_t *o, int tame)
{
    static const unsigned sc[4] = { 0x400, 0x400, 0x200, 0x800 };
    static const unsigned fl[5] = { 0, 0, 1, 0x10, 0x11 };
    unsigned a;
    be16(o, (unsigned)(tame ? randint(r, 0, 1200) : randint(r, -40, 1300)) & 0xffff);
    a = randrange(r, 5);
    be16(o + 2, tame || a < 4 ? CHOICE(r, sc) : 0x100 + randrange(r, 0xf00));
    be16(o + 4, (8 + randrange(r, 56)) * 32);
    be16(o + 6, 0);
    be16(o + 8, (unsigned)(tame ? randint(r, 0, 900) : randint(r, -40, 980)) & 0xffff);
    a = randrange(r, 5);
    be16(o + 10, tame || a < 4 ? CHOICE(r, sc) : 0x100 + randrange(r, 0xf00));
    be16(o + 12, (8 + randrange(r, 56)) * 32);
    be16(o + 14, 0);
    be16(o + 16, randrange(r, 40));
    be16(o + 18, randrange(r, 0x200));
    o[20] = (uint8_t)randrange(r, 5);
    o[21] = (uint8_t)randrange(r, 4);
    o[22] = (uint8_t)randrange(r, 16);
    o[23] = (uint8_t)(tame ? 0 : CHOICE(r, fl));
}

/* a uObjBg (40 bytes); copy: G_BG_COPY's (unscaled) */
static void s2_bg(struct rng *r, uint8_t *o, int copy)
{
    static const unsigned iws[3] = { 0x500, 0x2c0, 0x500 }, ihs[2] = { 0xc0, 0x88 }, fxs[3] = { 0xa4, 0x1c, 0 }, fys[3] = { 0x2e8, 0x2d8, 0 }, fmt[4] = { 0xfff4, 0xfff4, 0x0033, 0 };
    unsigned iw = randrange(r, 4) < 3 ? CHOICE(r, iws) : 0x40 + randrange(r, 0xbc0), ih = randrange(r, 3) < 2 ? CHOICE(r, ihs) : 0x20 + randrange(r, 0x360), sc;
    sc = !copy ? 0x400 : CHOICE(r, ((const unsigned[]){ 0x400, 0x400, 0x53, 0x200 }));
    be16(o, randrange(r, 4) < 3 ? 0 : randrange(r, 0x400));
    be16(o + 2, iw);
    be16(o + 4, randrange(r, 4) < 3 ? CHOICE(r, fxs) : randrange(r, 0x300));
    be16(o + 6, randrange(r, 3) < 2 ? iw : 0x40 + randrange(r, 0xbc0));
    be16(o + 8, randrange(r, 3) < 2 ? 0 : randrange(r, 0x300));
    be16(o + 10, ih);
    be16(o + 12, randrange(r, 4) < 3 ? CHOICE(r, fys) : randrange(r, 0x300));
    be16(o + 14, randrange(r, 3) < 2 ? ih : 0x20 + randrange(r, 0x360));
    be32(o + 16, 0x00100000 + randrange(r, 0x4000) * 8);
    be16(o + 20, CHOICE(r, fmt));
    o[22] = (uint8_t)randrange(r, 5);
    o[23] = (uint8_t)randrange(r, 4);
    o[24] = (uint8_t)randrange(r, 2);
    o[25] = (uint8_t)randrange(r, 16);
    o[26] = 0;
    o[27] = (uint8_t)(randrange(r, 3) == 2);
    be16(o + 28, sc);
    be16(o + 30, sc);
    be32(o + 32, randrange(r, 3) == 0 ? 0 : randrange(r, 2) ? 0x17 : randrange(r, 0x100));
    be16(o + 36, randrange(r, 0x100));
    be16(o + 38, randrange(r, 0x200));
}

/* a uObjTxtr (24 bytes): a texture load (the load bits and their mask: a mask that covers the bits makes the second load of the object a no-op) */
static void s2_txtr(struct rng *r, uint8_t *o)
{
    uint32_t sid = randrange(r, 1u << 16);
    unsigned m = randrange(r, 3);
    be32(o, 0x00001033);
    be32(o + 4, 0x00100000 + randrange(r, 0x1000) * 8);
    be32(o + 8, randrange(r, 0x7ff));
    be32(o + 12, 0x100);
    be32(o + 16, sid);
    be32(o + 20, m == 0 ? 0 : m == 1 ? sid : 0xffffffff);
}

/* the start of every S2DEX case: segment 0 = 0, the scissor (its fields clip the sprites and backgrounds), the render mode */
static void s2_head(struct dl *d, struct rng *r)
{
    cmd(d, 0xdb060000, 0);
    cmd(d, 0xed000000 | (randrange(r, 40) << 14) | randrange(r, 40) * 4, ((1280 - randrange(r, 40) * 4) << 12) | (960 - randrange(r, 40) * 4));
    cmd(d, 0x0b000000 | CHOICE(r, ((const unsigned[]){ 0, 8 })), randrange(r, 1u << 8) | (randrange(r, 4) << 3) | (randrange(r, 8) << 4));
}

NOINLINE static void s2_case(struct probe_case *c, struct dl *d, struct rng *r, unsigned kind)
{
    uint8_t o[48];
    unsigned off = 0x400, n, i;
    s2_head(d, r);
    switch (kind) {
    case PROBE_S2RECT:                                  /* sprite rectangles, tame and wild, with render mode changes between them */
        n = 3 + randrange(r, 6);
        for (i = 0; i < n; i++, off += 0x40) {
            s2_sprite(r, o, randrange(r, 2));
            put(c, off, o, 24);
            cmd(d, 0x01000000, c->base + off);
            if (randrange(r, 3) == 0)
                cmd(d, 0x0b000000 | CHOICE(r, ((const unsigned[]){ 0, 8 })), randrange(r, 1u << 8) | (randrange(r, 4) << 3) | (randrange(r, 8) << 4));
        }
        break;
    case PROBE_S2LDTX:                                  /* texture loads (an object loaded twice: the second writes nothing) and LDTX_RECT: the continuation pcs of G_OBJ_LOADTXTR */
        n = 2 + randrange(r, 4);
        for (i = 0; i < n; i++, off += 0x40) {
            s2_txtr(r, o);
            if (i == 0 || randrange(r, 2)) {
                s2_sprite(r, o + 24, randrange(r, 5) < 3);
                put(c, off, o, 48);
                cmd(d, 0x0700002f | CHOICE(r, ((const unsigned[]){ 0, 0x100 })), c->base + off);
            } else {
                put(c, off, o, 24);
                cmd(d, 0x05000017, c->base + off);
                if (randrange(r, 5) < 2)
                    cmd(d, 0x05000017, c->base + off);
            }
        }
        break;
    case PROBE_S2BG:                                    /* backgrounds in the 1-cycle mode (overlay B), a sprite between them (overlay A) */
    case PROBE_S2COPY:                                  /* backgrounds in the COPY mode (the resident code) */
        n = 1 + randrange(r, 3);
        for (i = 0; i < n; i++, off += 0x40) {
            s2_bg(r, o, kind == PROBE_S2COPY);
            put(c, off, o, 40);
            cmd(d, kind == PROBE_S2COPY ? 0x0a000000 : 0x09000000, c->base + off);
            if (randrange(r, 2)) {
                off += 0x40;
                s2_sprite(r, o, 1);
                put(c, off, o, 24);
                cmd(d, 0x01000000, c->base + off);
            }
        }
        break;
    default: {                                          /* the other commands: G_SELECT_DL (taken or not), G_OBJ_MOVEMEM, display list calls and branches, texture rectangles (G_TEXRECT, G_RDPHALF_1 / _2),
                                                         * other-mode, RDP copies, G_SETSCISSOR again */
        struct dl sub;
        uint32_t tgt = c->base + 0xc00;
        memset(&sub, 0, sizeof(sub));
        s2_sprite(r, o, 1);
        put(c, 0xd00, o, 24);
        cmd(&sub, 0x01000000, c->base + 0xd00);
        cmd(&sub, 0xe7000000, 0);
        cmd(&sub, 0xdf000000, 0);
        put_dl(c, 0xc00, &sub);
        n = 6 + randrange(r, 10);
        for (i = 0; i < n; i++) {
            switch (randrange(r, 9)) {
            case 0:                                     /* G_SELECT_DL: the list at 0xc00, called when the flags word of the selector changes (selector = the address' byte 2) */
                cmd(d, 0xe4000000 | (tgt & 0xffff), randrange(r, 2) ? 0 : rnd(r));
                cmd(d, 0x04000000 | ((tgt >> 16) & 0xff), randrange(r, 2) ? 0xffffffff : rnd(r));
                break;
            case 1:
                cmd(d, 0xde000000, tgt);
                break;
            case 2:                                     /* G_OBJ_MOVEMEM: uObjMtx (24 bytes, index 0) or uObjSubMtx (8 bytes, index 2) */
                s2_sprite(r, o, 0);
                put(c, 0xd40, o, 24);
                if (randrange(r, 2))
                    cmd(d, 0xdc170000, c->base + 0xd40);
                else
                    cmd(d, 0xdc070002, c->base + 0xd40);
                break;
            case 3:                                     /* a texture rectangle */
                cmd(d, 0xe4000000 | (randrange(r, 1024) << 12) | randrange(r, 1024), (randrange(r, 8) << 24) | (randrange(r, 1024) << 12) | randrange(r, 1024));
                cmd(d, 0xe1000000, rnd(r));
                cmd(d, 0xf1000000, rnd(r));
                break;
            case 4:
                cmd(d, 0xe3000a01, CHOICE(r, ((const uint32_t[]){ 0, 0x300000, 0x200000 })));
                cmd(d, 0xe2001e01, 0x00500000);
                break;
            case 5:
                cmd(d, 0xfa000000, rnd(r));
                cmd(d, 0xfd100000, 0x00100000 + randrange(r, 0x1000) * 8);
                cmd(d, 0xe6000000, 0);
                break;
            case 6:
                cmd(d, 0xed000000 | (randrange(r, 40) << 14) | randrange(r, 40) * 4, ((1280 - randrange(r, 40) * 4) << 12) | (960 - randrange(r, 40) * 4));
                break;
            case 7:
                s2_sprite(r, o, 1);
                put(c, 0xd80 + 0x20 * (i & 7), o, 24);
                cmd(d, 0x01000000, c->base + 0xd80 + 0x20 * (i & 7));
                break;
            default:
                cmd(d, 0xef000000 | randrange(r, 1u << 24), rnd(r));
                break;
            }
        }
        break;
    }
    }
    cmd(d, 0xdf000000, 0);
}

/* ---- F3DEX 1.x (the same cases in the F3DEX 1.x GBI) ----------------------------------------------------------------------------------------------------------------------- */
enum { G1_ZB = 0x1, G1_SHADE = 0x4, G1_SMOOTH = 0x200, G1_CFRONT = 0x1000, G1_CBACK = 0x2000, G1_FOG = 0x10000, G1_LIGHT = 0x20000, G1_TEXGEN = 0x40000, G1_TEXLIN = 0x80000, G1_CLIP = 0x800000 };

static void f1_vtx(struct dl *d, unsigned n, unsigned v0, uint32_t a) { cmd(d, (0x04u << 24) | (v0 * 2 << 16) | (n << 10) | (16 * n - 1), a); }
static void f1_tri1(struct dl *d, unsigned a, unsigned b, unsigned c) { cmd(d, 0xbf000000, (a * 2 << 16) | (b * 2 << 8) | c * 2); }
static void f1_mtx(struct dl *d, unsigned fl, uint32_t a) { cmd(d, (0x01u << 24) | (fl << 16) | 64, a); }
static void f1_mw(struct dl *d, unsigned idx, unsigned off, uint32_t v) { cmd(d, (0xbcu << 24) | (off << 8) | idx, v); }
static void f1_enddl(struct dl *d) { cmd(d, 0xb8000000, 0); }

static void f1_persp(struct dl *d, const struct probe_f1ops *o, uint32_t v)
{
    if (o->persp)
        cmd(d, (uint32_t)o->persp << 24, v);
    else
        f1_mw(d, 0x0e, 0, v);
}

static void f1_common(struct probe_case *c, struct dl *d, struct rng *r, const struct probe_f1ops *o, uint32_t geom, int trans_z, int tex)
{
    static const uint32_t pn[4] = { 0xffff, 0x7fff, 0x0100, 0x4000 };
    static const uint32_t tx[4] = { 0x80008000, 0xffffffff, 0x20004000, 0x00010001 };
    uint8_t vp[16];
    f1_mw(d, 6, 0, 0);                                  /* segment 0 = 0 */
    f1_mw(d, 4, 0x04, 3);                               /* the clip ratio */
    f1_mw(d, 4, 0x0c, 3);
    f1_mw(d, 4, 0x14, (uint32_t)-3);
    f1_mw(d, 4, 0x1c, (uint32_t)-3);
    f1_persp(d, o, CHOICE(r, pn));
    cmd(d, 0xb6000000, 0xffffffff);                     /* clear the geometry mode, then set it */
    cmd(d, 0xb7000000, geom);
    cmd(d, 0x03800010, c->base + 0x400);                /* the viewport */
    be16(vp, 160 * 4); be16(vp + 2, 120 * 4); be16(vp + 4, 0x1ff); be16(vp + 6, 0);
    be16(vp + 8, 160 * 4); be16(vp + 10, 120 * 4); be16(vp + 12, (unsigned)trans_z); be16(vp + 14, 0);
    put(c, 0x400, vp, 16);
    if (tex)
        cmd(d, 0xbb000001 | (randrange(r, 8) << 8), CHOICE(r, tx));
}

static void f1_culldl(struct dl *d, const struct probe_f1ops *o, unsigned a, unsigned b)
{
    if (o->culldl_off)
        cmd(d, (0xbeu << 24) | (a * 40), (b + 1) * 40);
    else
        cmd(d, (0xbeu << 24) | (a * 2), b * 2);
}

NOINLINE static void f1_geometry(struct probe_case *c, struct dl *d, struct rng *r, unsigned kind, const struct probe_f1ops *o)
{
    static const uint32_t shades[3] = { 0, G1_SHADE, G1_SHADE | G1_SMOOTH };
    static const uint32_t culls[5] = { 0, 0, G1_CBACK, G1_CFRONT, G1_CFRONT | G1_CBACK };
    static const double sc_clip[8] = { 1, 1, 0.5, 0.25, 1.0 / 16, 1.0 / 64, 1.0 / 256, 1.0 / 1024 };
    static const double sc_zneg[2] = { 1, 0.25 };
    static const double sc_wild[6] = { 1, 2, 4, 16, 64, 256 };
    static const double sc_mix[3] = { 1, 0.5, 1.0 / 8 };
    static const int tz[3] = { 0x1ff, 0, 0x100 };
    static const double sp0[4] = { 4, 10, 30, 100 };
    uint32_t geom = CHOICE(r, shades) | randrange(r, 2) * G1_ZB | CHOICE(r, culls) | G1_CLIP;
    unsigned tex = randrange(r, 2), nbatch = kind == PROBE_MIX ? 4 : 3, b, vaddr = 0x600;
    double mv[16], pj[16], scale, span0 = CHOICE(r, sp0);
    uint8_t buf[64];
    f1_common(c, d, r, o, geom, kind == PROBE_ZNEG ? 0 : CHOICE(r, tz), tex);
    scale = kind == PROBE_CLIP ? CHOICE(r, sc_clip) : kind == PROBE_ZNEG ? CHOICE(r, sc_zneg) : kind == PROBE_WILD ? CHOICE(r, sc_wild) : CHOICE(r, sc_mix);
    ident(mv);
    if (kind == PROBE_ZNEG) {                           /* w = 1 + 0.5 z: vertices on both sides of the near plane */
        memset(pj, 0, sizeof(pj));
        pj[0] = pj[5] = pj[10] = scale;
        pj[11] = 0.5 * scale;
        pj[15] = scale;
    } else {
        rnd_matrix(r, scale, pj);
        if (kind != PROBE_WILD) {
            pj[15] = scale * span0 * uniform(r, 1.2, 3.0);
            pj[12] = uniform(r, -0.5, 0.5) * scale * span0;
            pj[13] = uniform(r, -0.5, 0.5) * scale * span0;
            pj[14] = uniform(r, -0.5, 0.5) * scale * span0;
        }
    }
    mtx(buf, mv);
    put(c, 0x500, buf, 64);
    mtx(buf, pj);
    put(c, 0x540, buf, 64);
    f1_mtx(d, 3, c->base + 0x540);                      /* projection, load */
    f1_mtx(d, 2, c->base + 0x500);                      /* model-view, load */
    for (b = 0; b < nbatch; b++) {
        static const unsigned ns[8] = { 3, 3, 4, 5, 6, 9, 11, 16 };
        static const double sp_z[2] = { 0.5, 1.0 };
        static const double sp_w[3] = { 2000, 10000, 32767 };
        static const int s2[3] = { 100, 2000, 32768 };
        static const int ends[6] = { -32768, -32767, 32767, 0, 1, -1 };
        static const int zz[4] = { -1, 0, 0, 1 };
        unsigned n = kind == PROBE_MIX ? 1 + randrange(r, 11) : CHOICE(r, ns), v0 = randrange(r, 32 - n + 1), i, nt, q;
        double span = kind == PROBE_ZNEG ? CHOICE(r, sp_z) : kind == PROBE_WILD ? CHOICE(r, sp_w) : span0 * uniform(r, 0.5, 1.0), ctr[3];
        uint8_t vs[16 * 16];
        for (i = 0; i < 3; i++)
            ctr[i] = uniform(r, -span, span);
        for (i = 0; i < n; i++) {
            int x, y, z, k, cc[3];
            uint8_t col[4];
            if (kind == PROBE_ZNEG) {
                x = (int)uniform(r, -8, 8);
                y = (int)uniform(r, -8, 8);
                z = CHOICE(r, zz) * (int)(1 + randrange(r, 3));
            } else {
                int sp = kind == PROBE_WILD ? CHOICE(r, s2) : (int)(span0 * 0.5) + (int)randrange(r, 3);
                for (k = 0; k < 3; k++) {
                    double v = ctr[k] + uniform(r, -sp, sp);
                    int iv = (int)v;
                    cc[k] = iv < -32768 ? -32768 : iv > 32767 ? 32767 : iv;
                }
                x = cc[0]; y = cc[1]; z = cc[2];
                if (kind == PROBE_WILD) {
                    if (frnd(r) < 0.25) x = CHOICE(r, ends);
                    if (frnd(r) < 0.25) y = CHOICE(r, ends);
                    if (frnd(r) < 0.25) z = CHOICE(r, ends);
                }
            }
            rcol(r, col);
            vtx(vs + 16 * i, x, y, z, randint(r, -1024, 1023), randint(r, -1024, 1023), col);
        }
        put(c, vaddr, vs, 16 * n);
        f1_vtx(d, n, v0, c->base + vaddr);
        vaddr += 0x100;
        nt = kind == PROBE_CLIP ? 12 : 2 + randrange(r, 10);
        for (q = 0; q < nt && n >= 3; q++) {
            unsigned a, b2, cc;
            double u = frnd(r);
            sample3(r, v0, n, &a, &b2, &cc);
            if (u < 0.3) {
                unsigned a2, b3, c3;
                sample3(r, v0, n, &a2, &b3, &c3);
                cmd(d, (0xb1u << 24) | (a * 2 << 16) | (b2 * 2 << 8) | cc * 2, (a2 * 2 << 16) | (b3 * 2 << 8) | c3 * 2);
            } else if (u < 0.45 && o->quad && n >= 4) {
                unsigned v4 = v0 + randrange(r, n);
                cmd(d, (uint32_t)o->quad << 24, (v4 * 2 << 24) | (a * 2 << 16) | (b2 * 2 << 8) | cc * 2);
            } else
                f1_tri1(d, a, b2, cc);
        }
    }
    f1_enddl(d);
}

static void f1_sub(struct probe_case *c, unsigned off, const struct dl *s)
{
    uint8_t t[8 * 16];
    unsigned k;
    for (k = 0; k < s->n && k < 16; k++) {
        t[8 * k] = (uint8_t)(s->w[k][0] >> 24); t[8 * k + 1] = (uint8_t)(s->w[k][0] >> 16); t[8 * k + 2] = (uint8_t)(s->w[k][0] >> 8); t[8 * k + 3] = (uint8_t)s->w[k][0];
        t[8 * k + 4] = (uint8_t)(s->w[k][1] >> 24); t[8 * k + 5] = (uint8_t)(s->w[k][1] >> 16); t[8 * k + 6] = (uint8_t)(s->w[k][1] >> 8); t[8 * k + 7] = (uint8_t)s->w[k][1];
    }
    put(c, off, t, 8 * k);
}

NOINLINE static void f1_misc(struct probe_case *c, struct dl *d, struct rng *r, const struct probe_f1ops *o)
{
    static const double sc[3] = { 1, 0.5, 1.0 / 8 };
    static const int tz[2] = { 0x1ff, 0 };
    static const uint32_t thr[4] = { 0, 0x10000, 0x40000000, 0xffff0000 };
    static const unsigned mv_off[4] = { 0x10, 0x14, 0x18, 0x1c };
    static const uint32_t gm[6] = { 0, G1_SMOOTH, G1_CBACK, G1_CFRONT, G1_SHADE, G1_ZB };
    uint32_t geom = G1_SHADE | G1_SMOOTH | G1_ZB | G1_CLIP;
    double m[16];
    uint8_t buf[512];
    unsigned n = CHOICE(r, ((const unsigned[]){ 6, 9, 16 })), v0 = randrange(r, 32 - n + 1), i, nk;
    struct dl sub, sub2;
    f1_common(c, d, r, o, geom, CHOICE(r, tz), 1);
    ident(m);
    mtx(buf, m);
    put(c, 0x500, buf, 64);
    rnd_matrix(r, CHOICE(r, sc), m);
    mtx(buf, m);
    put(c, 0x540, buf, 64);
    rnd_matrix(r, 0.25, m);
    mtx(buf, m);
    put(c, 0x580, buf, 64);
    f1_mtx(d, 3, c->base + 0x540);
    f1_mtx(d, 2, c->base + 0x500);
    for (i = 0; i < n; i++) {
        uint8_t col[4];
        rcol(r, col);
        vtx(buf + 16 * i, randint(r, -30, 30), randint(r, -30, 30), randint(r, -30, 30), randint(r, -512, 511), randint(r, -512, 511), col);
    }
    put(c, 0x600, buf, 16 * n);
    f1_vtx(d, n, v0, c->base + 0x600);
    memset(&sub, 0, sizeof(sub));
    memset(&sub2, 0, sizeof(sub2));
    cmd(&sub, 0xf7000000, 0x11111111);
    f1_enddl(&sub);
    cmd(&sub2, 0xf7000000, 0x22222222);
    f1_tri1(&sub2, v0, v0 + 1, v0 + 2);
    f1_enddl(&sub2);
    f1_sub(c, 0x800, &sub);
    f1_sub(c, 0x900, &sub2);
    nk = 8 + randrange(r, 12);
    for (i = 0; i < nk; i++) {
        unsigned k = randrange(r, 14), va = v0 + randrange(r, n), vb = v0 + randrange(r, n);
        switch (k) {
        case 0: {                                       /* G_CULLDL in a list of its own (a cull ends that list, not this one), a triangle behind it */
            unsigned a = va < vb ? va : vb, b = va < vb ? vb : va;
            struct dl sub3;
            memset(&sub3, 0, sizeof(sub3));
            f1_culldl(&sub3, o, a, b);
            f1_tri1(&sub3, a, b, v0 + randrange(r, n));
            cmd(&sub3, 0xf7000000, 0xa5a50000 | i);
            f1_enddl(&sub3);
            f1_sub(c, 0xb00 + 0x20 * (i % 16), &sub3);
            cmd(d, 0x06000000, c->base + 0xb00 + 0x20 * (i % 16));
            break;
        }
        case 1:
            if (o->branchz && o->half1) {
                cmd(d, (uint32_t)o->half1 << 24, c->base + 0x900);
                cmd(d, ((uint32_t)o->branchz << 24) | ((va * 5) << 12) | ((va * 2) & 0xfff), randrange(r, 5) < 4 ? CHOICE(r, thr) : rnd(r));
            }
            break;
        case 2:
            if (o->modvtx)
                cmd(d, ((uint32_t)o->modvtx << 24) | (CHOICE(r, mv_off) << 16) | (va * 2), rnd(r));
            break;
        case 3:
            if (o->half1 && o->half2) {
                cmd(d, (uint32_t)o->half1 << 24, 0xe7000000);
                cmd(d, (uint32_t)o->half2 << 24, rnd(r));
            }
            break;
        case 4:
            f1_mtx(d, 4, c->base + 0x580);              /* push, multiply */
            cmd(d, 0xbd000000, 0);                      /* pop */
            f1_mtx(d, 0, c->base + 0x580);              /* multiply */
            break;
        case 5:
            cmd(d, 0x06000000, c->base + 0x800);        /* call */
            break;
        case 6:
            cmd(d, 0x06000000, c->base + 0x900);        /* call a list with a triangle */
            break;
        case 7: {
            static const uint32_t oh[2] = { 0, 0x00300000 };
            cmd(d, 0xba001402, CHOICE(r, oh));          /* other mode H: the cycle type */
            cmd(d, 0xb9000302, 0x00000000);             /* other mode L: the alpha compare */
            break;
        }
        case 8: {
            static const uint32_t tx[2] = { 0x80008000, 0x40004000 };
            cmd(d, 0xbb000001 | (randrange(r, 8) << 8), CHOICE(r, tx));
            break;
        }
        case 9:
            cmd(d, 0xb6000000, CHOICE(r, gm));
            cmd(d, 0xb7000000, CHOICE(r, gm));
            break;
        case 10:
            f1_mw(d, 6, 4 * (1 + randrange(r, 15)), randrange(r, 0x100) * 0x100);      /* a segment other than 0 */
            break;
        case 11:
            f1_mw(d, 8, 0, (uint32_t)(randint(r, 0x100, 0x7fff) << 16) | randrange(r, 0x10000));   /* fog */
            break;
        case 12:
            f1_persp(d, o, randrange(r, 0x10000));
            break;
        default:
            f1_tri1(d, va, vb, v0 + randrange(r, n));
            break;
        }
    }
    if (randrange(r, 2))
        cmd(d, 0x06010000, c->base + 0x900);            /* a jump: the list ends in the other one */
    else
        f1_enddl(d);
}

NOINLINE static void f1_lit(struct probe_case *c, struct dl *d, struct rng *r, const struct probe_f1ops *o)
{
    static const uint32_t tg[3] = { 0, G1_TEXGEN, G1_TEXGEN | G1_TEXLIN };
    static const uint32_t fg[2] = { 0, G1_FOG };
    static const int tz[2] = { 0x1ff, 0 };
    static const double sc[3] = { 1, 0.5, 1.0 / 8 };
    static const unsigned ns[5] = { 3, 4, 5, 8, 11 };
    uint32_t gm = G1_SHADE | G1_SMOOTH | G1_ZB | G1_LIGHT | CHOICE(r, tg) | CHOICE(r, fg) | G1_CLIP;
    unsigned nl = 1 + randrange(r, 5), slot, b, vaddr = 0x600;
    double m[16];
    uint8_t buf[512];
    f1_common(c, d, r, o, gm, CHOICE(r, tz), 1);
    if (gm & G1_FOG)
        f1_mw(d, 8, 0, (uint32_t)(randint(r, 0x100, 0x7fff) << 16) | randrange(r, 0x10000));
    f1_mw(d, 2, 0, 0x80000000u + 32 * (nl + 1));        /* G_MW_NUMLIGHT */
    for (slot = 0; slot < nl + 3; slot++) {             /* look-at Y, look-at X, the lights, the ambient light */
        uint8_t blob[16];
        unsigned idx = slot == 0 ? 0x82 : slot == 1 ? 0x84 : 0x86 + 2 * (slot - 2);
        memset(blob, 0, sizeof(blob));
        if (slot >= 2) {
            uint8_t col[4] = { (uint8_t)randrange(r, 256), (uint8_t)randrange(r, 256), (uint8_t)randrange(r, 256), 0 };
            uint8_t dir[4] = { (uint8_t)randrange(r, 256), (uint8_t)randrange(r, 256), (uint8_t)randrange(r, 256), 0 };
            memcpy(blob, col, 4);
            memcpy(blob + 4, col, 4);
            memcpy(blob + 8, dir, 4);
        } else
            blob[8 + (slot == 0 ? 1 : 0)] = 0x7f;
        put(c, 0x1000 + 0x20 * slot, blob, 16);
        cmd(d, 0x03000010 | (idx << 16), c->base + 0x1000 + 0x20 * slot);
    }
    ident(m);
    mtx(buf, m);
    put(c, 0x500, buf, 64);
    rnd_matrix(r, CHOICE(r, sc), m);
    mtx(buf, m);
    put(c, 0x540, buf, 64);
    f1_mtx(d, 3, c->base + 0x540);
    f1_mtx(d, 2, c->base + 0x500);
    for (b = 0; b < 2; b++) {
        unsigned n = CHOICE(r, ns), v0 = randrange(r, 32 - n + 1), i, q;
        uint8_t vs[16 * 16];
        for (i = 0; i < n; i++) {
            uint8_t col[4];
            col[0] = (uint8_t)randint(r, -127, 127); col[1] = (uint8_t)randint(r, -127, 127); col[2] = (uint8_t)randint(r, -127, 127); col[3] = (uint8_t)randrange(r, 256);
            vtx(vs + 16 * i, randint(r, -200, 200), randint(r, -200, 200), randint(r, -200, 200), randint(r, -1024, 1023), randint(r, -1024, 1023), col);
        }
        put(c, vaddr, vs, 16 * n);
        f1_vtx(d, n, v0, c->base + vaddr);
        vaddr += 0x100;
        for (q = 0; q < 4; q++) {
            unsigned a, b2, cc;
            sample3(r, v0, n, &a, &b2, &cc);
            f1_tri1(d, a, b2, cc);
        }
        if (b == 0)
            f1_mtx(d, 4, c->base + 0x540);              /* a pushed model-view: the light directions are transformed again */
    }
    f1_enddl(d);
}

NOINLINE static void f1_mtxcase(struct probe_case *c, struct dl *d, struct rng *r, const struct probe_f1ops *o)
{
    static const int tz[2] = { 0x1ff, 0 };
    static const uint16_t sm[5] = { 0, 1, 0xffff, 0x8000, 0x7fff };
    unsigned nm = 2 + randrange(r, 4), k, i;
    uint32_t geom = randrange(r, 2) * G1_SHADE | randrange(r, 2) * G1_ZB | G1_CLIP;
    uint8_t buf[64], vs[64];
    f1_common(c, d, r, o, geom, CHOICE(r, tz), 0);
    for (k = 0; k < nm; k++) {
        for (i = 0; i < 64; i++)
            buf[i] = (uint8_t)randrange(r, 256);
        if (randrange(r, 2))
            for (i = 0; i < 16; i++) {
                unsigned q = randrange(r, 6);
                be16(buf + 2 * i, q < 5 ? sm[q] : randrange(r, 1u << 16));
            }
        put(c, 0x500 + 0x40 * k, buf, 64);
        f1_mtx(d, randrange(r, 8), c->base + 0x500 + 0x40 * k);
        if (randrange(r, 4) == 0)
            cmd(d, 0xbd000000, 0);
    }
    for (i = 0; i < 4; i++) {
        const uint8_t col[4] = { 1, 2, 3, 4 };
        vtx(vs + 16 * i, randint(r, -40, 40), randint(r, -40, 40), randint(r, -40, 40), randint(r, -512, 511), randint(r, -512, 511), col);
    }
    put(c, 0x600, vs, 64);
    f1_vtx(d, 4, 0, c->base + 0x600);
    f1_tri1(d, 0, 1, 2);
    f1_enddl(d);
}

/* rej_case in the F3DEX 1.x GBI: groups of three vertices beyond each plane; the near-plane group is rejected by a mask with the near bits (0x7070) and clipped without them (NoN's 0x7030).
 * Group 7 spreads x and y over both sides and puts z far in front (z < -|w|): with w < 0 (the second variant) a coordinate always sets one of its two compare bits, so no x or y bit and (z < w)
 * no z-far bit is common to the three, and only bit 0x40 is: the NoN builds' sign of w (the EX builds': z <= -w), which G_CULLDL tests with the build's mask (0x7030 leaves it out). */
NOINLINE static void f1_rej(struct probe_case *c, struct dl *d, struct rng *r, unsigned seed, const struct probe_f1ops *o)
{
    double m[16] = { 0.125, 0, 0, 0, 0, 0.125, 0, 0, 0, 0, 0.125, 0, 0, 0, 0, 1 }, mv[16];
    uint8_t buf[64], vs[16 * 32];
    unsigned g, i, nv = 0;
    uint32_t geom = randrange(r, 2) * G1_SHADE | G1_CLIP;
    f1_common(c, d, r, o, geom, 0x1ff, 0);
    if (seed & 1)
        m[15] = -1;
    ident(mv);
    mtx(buf, mv);
    put(c, 0x500, buf, 64);
    mtx(buf, m);
    put(c, 0x540, buf, 64);
    f1_mtx(d, 3, c->base + 0x540);
    f1_mtx(d, 2, c->base + 0x500);
    for (g = 0; g < 8; g++)
        for (i = 0; i < 3; i++) {
            static const int spread[3][3] = { { 100, -100, -100 }, { -100, 100, -100 }, { 100, 100, -100 } };
            int x = randint(r, -3, 3), y = randint(r, -3, 3), z = randint(r, -3, 3);
            uint8_t col[4];
            switch (g) {
            case 0: z = 100; break;
            case 1: z = -100; break;
            case 2: x = 100; break;
            case 3: x = -100; break;
            case 4: y = 100; break;
            case 5: y = -100; break;
            case 7: x = spread[i][0]; y = spread[i][1]; z = spread[i][2]; break;
            default: break;
            }
            rcol(r, col);
            vtx(vs + 16 * nv, x, y, z, 0, 0, col);
            nv++;
        }
    put(c, 0x600, vs, 16 * nv);
    f1_vtx(d, nv, 0, c->base + 0x600);
    for (g = 0; g < 8; g++) {
        f1_tri1(d, 3 * g, 3 * g + 1, 3 * g + 2);
        f1_tri1(d, 3 * g + 2, 3 * g + 1, 3 * g);
    }
    for (g = 0; g < 8; g++) {
        struct dl sub;
        if (g == 6)
            continue;
        memset(&sub, 0, sizeof(sub));
        f1_culldl(&sub, o, 3 * g, 3 * g + 2);
        f1_tri1(&sub, 18, 19, 20);
        cmd(&sub, 0xf7000000, 0x5a5a0000 | g);          /* (a witness in the RDP stream that the list went on: the triangle may be rejected too) */
        f1_enddl(&sub);
        f1_sub(c, 0xa00 + 0x20 * g, &sub);
        cmd(d, 0x06000000, c->base + 0xa00 + 0x20 * g);
    }
    /* The NoN builds keep the sign of w in bit 0x40 of the second vertex of each pair only: the spread vertices again, in the second slots of three pairs, as one triangle. With w < 0 the near
     * bit is the only common one: a mask with it rejects the triangle, NoN's mask gives it to the clipper, which has no near plane. */
    {
        static const int spread[3][3] = { { 100, -100, -100 }, { -100, 100, -100 }, { 100, 100, -100 } };
        uint8_t col[4] = { 200, 100, 50, 255 };
        for (i = 0; i < 6; i++)
            vtx(vs + 16 * i, spread[i / 2][0], spread[i / 2][1], spread[i / 2][2], 0, 0, col);
        put(c, 0x700, vs, 16 * 6);
        f1_vtx(d, 6, 24, c->base + 0x700);
        f1_tri1(d, 25, 27, 29);
        f1_tri1(d, 29, 27, 25);
    }
    f1_enddl(d);
}

/* G_BRANCH_Z (when the build has it) against thresholds around the vertices' screen z */
NOINLINE static void f1_bz(struct probe_case *c, struct dl *d, struct rng *r, unsigned seed, const struct probe_f1ops *o)
{
    static const uint32_t thr[10] = { 0, 1, 2, 5, 0x100, 0x10000, 0x20000, 0x1ff0000, 0x7fffffff, 0xffff0000 };
    static const int tz[2] = { 0x1ff, 0x100 };
    double mv[16], pj[16];
    uint8_t buf[64], vs[16 * 8];
    unsigned i, n = 6, v0 = seed & 1 ? 20 : 4, t;
    struct dl sub;
    f1_common(c, d, r, o, G1_SHADE | G1_SMOOTH | G1_ZB | G1_CLIP, CHOICE(r, tz), 1);
    ident(mv);
    mtx(buf, mv);
    put(c, 0x500, buf, 64);
    rnd_matrix(r, 0.5, pj);
    pj[15] = 1 + uniform(r, 0, 3);
    mtx(buf, pj);
    put(c, 0x540, buf, 64);
    f1_mtx(d, 3, c->base + 0x540);
    f1_mtx(d, 2, c->base + 0x500);
    for (i = 0; i < n; i++) {
        uint8_t col[4];
        rcol(r, col);
        vtx(vs + 16 * i, randint(r, -6, 6), randint(r, -6, 6), randint(r, 0, 12), randint(r, -512, 511), randint(r, -512, 511), col);
    }
    put(c, 0x600, vs, 16 * n);
    f1_vtx(d, n, v0, c->base + 0x600);
    memset(&sub, 0, sizeof(sub));
    cmd(&sub, 0xf7000000, 0x33333333);
    f1_enddl(&sub);
    f1_sub(c, 0x900, &sub);
    for (t = 0; t < 10; t++) {
        unsigned va = v0 + (t % n);
        if (o->branchz && o->half1) {
            cmd(d, (uint32_t)o->half1 << 24, c->base + 0x900);
            cmd(d, ((uint32_t)o->branchz << 24) | ((va * 5) << 12) | ((va * 2) & 0xfff), thr[t]);
        }
        f1_tri1(d, v0, v0 + 1, v0 + 2);
        cmd(d, 0xbb000001 | ((t & 7) << 8), 0x80008000);
    }
    f1_enddl(d);
}

void probe_case_make_f1(struct probe_case *c, unsigned kind, unsigned seed, uint32_t base, const struct probe_f1ops *ops)
{
    struct rng r;
    struct dl d;
    unsigned i;
    uint8_t *p;
    memset(c, 0, sizeof(*c));
    memset(&d, 0, sizeof(d));
    c->base = base;
    r.s = ((uint64_t)seed * 0x9e3779b97f4a7c15ull) ^ ((uint64_t)(kind + 1) * 0xd1b54a32d192ed03ull) ^ 0x8765432112345678ull;
    for (i = 0; i < 8; i++)
        (void)rnd(&r);
    switch (kind) {
    case PROBE_CLIP: case PROBE_ZNEG: case PROBE_TRI: case PROBE_MIX: case PROBE_WILD:
        f1_geometry(c, &d, &r, kind, ops);
        break;
    case PROBE_MISC: f1_misc(c, &d, &r, ops); break;
    case PROBE_LIT: f1_lit(c, &d, &r, ops); break;
    case PROBE_MTX: f1_mtxcase(c, &d, &r, ops); break;
    case PROBE_REJ: f1_rej(c, &d, &r, seed, ops); break;
    default: f1_bz(c, &d, &r, seed, ops); break;
    }
    if (d.n > 126) {
        d.n = 126;
        f1_enddl(&d);
    }
    p = c->mem;
    for (i = 0; i < d.n; i++) {
        p[8 * i] = (uint8_t)(d.w[i][0] >> 24); p[8 * i + 1] = (uint8_t)(d.w[i][0] >> 16); p[8 * i + 2] = (uint8_t)(d.w[i][0] >> 8); p[8 * i + 3] = (uint8_t)d.w[i][0];
        p[8 * i + 4] = (uint8_t)(d.w[i][1] >> 24); p[8 * i + 5] = (uint8_t)(d.w[i][1] >> 16); p[8 * i + 6] = (uint8_t)(d.w[i][1] >> 8); p[8 * i + 7] = (uint8_t)d.w[i][1];
    }
    c->dl_bytes = d.n * 8;
    if (c->dl_bytes > c->used)
        c->used = c->dl_bytes;
}

/* ---- F3D (the same cases in Fast3D's GBI) ------------------------------------------------------------------------------------------------------------------------------------ */
static void f3_vtx(struct dl *d, const struct probe_f3ops *o, unsigned n, unsigned v0, uint32_t a)
{
    if (o->vtx_wr)
        cmd(d, (0x04u << 24) | ((5 * v0) << 16) | (n << 9) | (16 * n - 1), a);
    else
        cmd(d, (0x04u << 24) | ((((n - 1) << 4) | v0) << 16) | (16 * n - 1), a);
}
static unsigned f3_ix(const struct probe_f3ops *o, unsigned i) { return 40 * i / o->vptr_mul; }
static void f3_tri1f(struct dl *d, const struct probe_f3ops *o, unsigned a, unsigned b, unsigned c, unsigned flag)
{
    cmd(d, 0xbf000000, (flag << 24) | (f3_ix(o, a) << 16) | (f3_ix(o, b) << 8) | f3_ix(o, c));
}
static void f3_tri1(struct dl *d, const struct probe_f3ops *o, unsigned a, unsigned b, unsigned c) { f3_tri1f(d, o, a, b, c, 0); }
static void f3_mtx(struct dl *d, unsigned fl, uint32_t a) { cmd(d, (0x01u << 24) | (fl << 16) | 64, a); }
static void f3_mw(struct dl *d, unsigned idx, unsigned off, uint32_t v) { cmd(d, (0xbcu << 24) | (off << 8) | idx, v); }
static void f3_enddl(struct dl *d) { cmd(d, 0xb8000000, 0); }
static void f3_culldl(struct dl *d, unsigned a, unsigned b) { cmd(d, (0xbeu << 24) | (a * 40), (b + 1) * 40); }

static void f3_persp(struct dl *d, const struct probe_f3ops *o, uint32_t v)
{
    if (o->persp)
        cmd(d, (uint32_t)o->persp << 24, v);
    else
        f3_mw(d, 0x0e, 0, v);
}

static void f3_common(struct probe_case *c, struct dl *d, struct rng *r, const struct probe_f3ops *o, uint32_t geom, int trans_z, int tex)
{
    static const uint32_t pn[4] = { 0xffff, 0x7fff, 0x0100, 0x4000 };
    static const uint32_t tx[4] = { 0x80008000, 0xffffffff, 0x20004000, 0x00010001 };
    uint8_t vp[16];
    f3_mw(d, 6, 0, 0);                                  /* segment 0 = 0 */
    f3_mw(d, 4, 0x04, 3);                               /* the clip ratio */
    f3_mw(d, 4, 0x0c, 3);
    f3_mw(d, 4, 0x14, (uint32_t)-3);
    f3_mw(d, 4, 0x1c, (uint32_t)-3);
    f3_persp(d, o, CHOICE(r, pn));
    cmd(d, 0xb6000000, 0xffffffff);                     /* clear the geometry mode, then set it */
    cmd(d, 0xb7000000, geom);
    cmd(d, 0x03800010, c->base + 0x400);                /* the viewport */
    be16(vp, 160 * 4); be16(vp + 2, 120 * 4); be16(vp + 4, 0x1ff); be16(vp + 6, 0);
    be16(vp + 8, 160 * 4); be16(vp + 10, 120 * 4); be16(vp + 12, (unsigned)trans_z); be16(vp + 14, 0);
    put(c, 0x400, vp, 16);
    if (tex)
        cmd(d, 0xbb000001 | (randrange(r, 8) << 8), CHOICE(r, tx));
}

/* a vertex batch's size and first slot: up to 16 per command (the count is a nibble) inside the build's slots */
static unsigned f3_maxn(const struct probe_f3ops *o) { return o->nslots < 16 ? o->nslots : 16; }

NOINLINE static void f3_geometry(struct probe_case *c, struct dl *d, struct rng *r, unsigned kind, const struct probe_f3ops *o)
{
    static const uint32_t shades[3] = { 0, G1_SHADE, G1_SHADE | G1_SMOOTH };
    static const uint32_t culls[5] = { 0, 0, G1_CBACK, G1_CFRONT, G1_CFRONT | G1_CBACK };
    static const double sc_clip[8] = { 1, 1, 0.5, 0.25, 1.0 / 16, 1.0 / 64, 1.0 / 256, 1.0 / 1024 };
    static const double sc_zneg[2] = { 1, 0.25 };
    static const double sc_wild[6] = { 1, 2, 4, 16, 64, 256 };
    static const double sc_mix[3] = { 1, 0.5, 1.0 / 8 };
    static const int tz[3] = { 0x1ff, 0, 0x100 };
    static const double sp0[4] = { 4, 10, 30, 100 };
    uint32_t geom = CHOICE(r, shades) | randrange(r, 2) * G1_ZB | CHOICE(r, culls);
    unsigned tex = randrange(r, 2), nbatch = kind == PROBE_MIX ? 4 : 3, b, vaddr = 0x600, mx = f3_maxn(o);
    double mv[16], pj[16], scale, span0 = CHOICE(r, sp0);
    uint8_t buf[64];
    f3_common(c, d, r, o, geom, kind == PROBE_ZNEG ? 0 : CHOICE(r, tz), tex);
    scale = kind == PROBE_CLIP ? CHOICE(r, sc_clip) : kind == PROBE_ZNEG ? CHOICE(r, sc_zneg) : kind == PROBE_WILD ? CHOICE(r, sc_wild) : CHOICE(r, sc_mix);
    ident(mv);
    if (kind == PROBE_ZNEG) {                           /* w = 1 + 0.5 z: vertices on both sides of the near plane */
        memset(pj, 0, sizeof(pj));
        pj[0] = pj[5] = pj[10] = scale;
        pj[11] = 0.5 * scale;
        pj[15] = scale;
    } else {
        rnd_matrix(r, scale, pj);
        if (kind != PROBE_WILD) {
            pj[15] = scale * span0 * uniform(r, 1.2, 3.0);
            pj[12] = uniform(r, -0.5, 0.5) * scale * span0;
            pj[13] = uniform(r, -0.5, 0.5) * scale * span0;
            pj[14] = uniform(r, -0.5, 0.5) * scale * span0;
        }
    }
    mtx(buf, mv);
    put(c, 0x500, buf, 64);
    mtx(buf, pj);
    put(c, 0x540, buf, 64);
    f3_mtx(d, 3, c->base + 0x540);                      /* projection, load */
    f3_mtx(d, 2, c->base + 0x500);                      /* model-view, load */
    for (b = 0; b < nbatch; b++) {
        static const unsigned ns[8] = { 3, 3, 4, 5, 6, 9, 11, 16 };
        static const double sp_z[2] = { 0.5, 1.0 };
        static const double sp_w[3] = { 2000, 10000, 32767 };
        static const int s2[3] = { 100, 2000, 32768 };
        static const int ends[6] = { -32768, -32767, 32767, 0, 1, -1 };
        static const int zz[4] = { -1, 0, 0, 1 };
        unsigned n = kind == PROBE_MIX ? 1 + randrange(r, 11) : CHOICE(r, ns), v0, i, nt, q;
        double span = kind == PROBE_ZNEG ? CHOICE(r, sp_z) : kind == PROBE_WILD ? CHOICE(r, sp_w) : span0 * uniform(r, 0.5, 1.0), ctr[3];
        uint8_t vs[16 * 16];
        if (n > mx)
            n = mx;
        v0 = randrange(r, o->nslots - n + 1);
        if (!o->vtx_wr && v0 > 15)
            v0 = 15;
        for (i = 0; i < 3; i++)
            ctr[i] = uniform(r, -span, span);
        for (i = 0; i < n; i++) {
            int x, y, z, k, cc[3];
            uint8_t col[4];
            if (kind == PROBE_ZNEG) {
                x = (int)uniform(r, -8, 8);
                y = (int)uniform(r, -8, 8);
                z = CHOICE(r, zz) * (int)(1 + randrange(r, 3));
            } else {
                int sp = kind == PROBE_WILD ? CHOICE(r, s2) : (int)(span0 * 0.5) + (int)randrange(r, 3);
                for (k = 0; k < 3; k++) {
                    double v = ctr[k] + uniform(r, -sp, sp);
                    int iv = (int)v;
                    cc[k] = iv < -32768 ? -32768 : iv > 32767 ? 32767 : iv;
                }
                x = cc[0]; y = cc[1]; z = cc[2];
                if (kind == PROBE_WILD) {
                    if (frnd(r) < 0.25) x = CHOICE(r, ends);
                    if (frnd(r) < 0.25) y = CHOICE(r, ends);
                    if (frnd(r) < 0.25) z = CHOICE(r, ends);
                }
            }
            rcol(r, col);
            vtx(vs + 16 * i, x, y, z, randint(r, -1024, 1023), randint(r, -1024, 1023), col);
        }
        put(c, vaddr, vs, 16 * n);
        f3_vtx(d, o, n, v0, c->base + vaddr);
        vaddr += 0x100;
        nt = kind == PROBE_CLIP ? 12 : 2 + randrange(r, 10);
        for (q = 0; q < nt && n >= 3; q++) {
            unsigned a, b2, cc;
            double u = frnd(r);
            sample3(r, v0, n, &a, &b2, &cc);
            if (u < 0.3 && o->quad && n >= 4) {
                unsigned v4 = v0 + randrange(r, n);
                cmd(d, (uint32_t)o->quad << 24, (f3_ix(o, v4) << 24) | (f3_ix(o, a) << 16) | (f3_ix(o, b2) << 8) | f3_ix(o, cc));
            } else if (u < 0.3 && o->tri4 && v0 + n <= 16) {  /* 1 .. 4 triangles: triangle k's vertices are w1's nibbles 2k, 2k + 1 and w0's nibble k */
                unsigned m = 1 + randrange(r, 4), k, a2, b3, c3;
                uint32_t x0 = (uint32_t)o->tri4 << 24, x1 = 0;
                for (k = 0; k < m; k++) {
                    if (k)
                        sample3(r, v0, n, &a2, &b3, &c3);
                    else {
                        a2 = a; b3 = b2; c3 = cc;
                    }
                    x1 |= ((b3 << 4) | a2) << (8 * k);
                    x0 |= c3 << (4 * k);
                }
                cmd(d, x0, x1);
            } else
                f3_tri1f(d, o, a, b2, cc, o->flat ? randrange(r, 3) : 0);
        }
    }
    f3_enddl(d);
}

NOINLINE static void f3_misc(struct probe_case *c, struct dl *d, struct rng *r, const struct probe_f3ops *o)
{
    static const double sc[3] = { 1, 0.5, 1.0 / 8 };
    static const int tz[2] = { 0x1ff, 0 };
    static const uint32_t gm[6] = { 0, G1_SMOOTH, G1_CBACK, G1_CFRONT, G1_SHADE, G1_ZB };
    uint32_t geom = G1_SHADE | G1_SMOOTH | G1_ZB;
    double m[16];
    uint8_t buf[512];
    unsigned n = CHOICE(r, ((const unsigned[]){ 6, 9, 16 })), v0, i, nk;
    struct dl sub, sub2;
    if (n > f3_maxn(o))
        n = f3_maxn(o);
    v0 = randrange(r, o->nslots - n + 1);
    if (!o->vtx_wr && v0 > 15)
        v0 = 15;
    f3_common(c, d, r, o, geom, CHOICE(r, tz), 1);
    ident(m);
    mtx(buf, m);
    put(c, 0x500, buf, 64);
    rnd_matrix(r, CHOICE(r, sc), m);
    mtx(buf, m);
    put(c, 0x540, buf, 64);
    rnd_matrix(r, 0.25, m);
    mtx(buf, m);
    put(c, 0x580, buf, 64);
    f3_mtx(d, 3, c->base + 0x540);
    f3_mtx(d, 2, c->base + 0x500);
    for (i = 0; i < n; i++) {
        uint8_t col[4];
        rcol(r, col);
        vtx(buf + 16 * i, randint(r, -30, 30), randint(r, -30, 30), randint(r, -30, 30), randint(r, -512, 511), randint(r, -512, 511), col);
    }
    put(c, 0x600, buf, 16 * n);
    f3_vtx(d, o, n, v0, c->base + 0x600);
    memset(&sub, 0, sizeof(sub));
    memset(&sub2, 0, sizeof(sub2));
    cmd(&sub, 0xf7000000, 0x11111111);
    f3_enddl(&sub);
    cmd(&sub2, 0xf7000000, 0x22222222);
    f3_tri1(&sub2, o, v0, v0 + 1, v0 + 2);
    f3_enddl(&sub2);
    f1_sub(c, 0x800, &sub);
    f1_sub(c, 0x900, &sub2);
    nk = 8 + randrange(r, 12);
    for (i = 0; i < nk; i++) {
        unsigned k = randrange(r, 14), va = v0 + randrange(r, n), vb = v0 + randrange(r, n);
        switch (k) {
        case 0: {                                       /* G_CULLDL in a list of its own (a cull ends that list, not this one), a triangle and a witness behind it */
            unsigned a = va < vb ? va : vb, b = va < vb ? vb : va;
            struct dl sub3;
            memset(&sub3, 0, sizeof(sub3));
            f3_culldl(&sub3, a, b);
            f3_tri1(&sub3, o, a, b, v0 + randrange(r, n));
            cmd(&sub3, 0xf7000000, 0xa5a50000 | i);
            f3_enddl(&sub3);
            f1_sub(c, 0xb00 + 0x20 * (i % 16), &sub3);
            cmd(d, 0x06000000, c->base + 0xb00 + 0x20 * (i % 16));
            break;
        }
        case 3:
            if (o->half1 && o->half2) {
                cmd(d, (uint32_t)o->half1 << 24, 0xe7000000);
                cmd(d, (uint32_t)o->half2 << 24, rnd(r));
            }
            break;
        case 4:
            f3_mtx(d, 4, c->base + 0x580);              /* push, multiply */
            cmd(d, 0xbd000000, 0);                      /* pop */
            f3_mtx(d, 0, c->base + 0x580);              /* multiply */
            break;
        case 5:
            cmd(d, 0x06000000, c->base + 0x800);        /* call */
            break;
        case 6:
            cmd(d, 0x06000000, c->base + 0x900);        /* call a list with a triangle */
            break;
        case 7: {
            static const uint32_t oh[2] = { 0, 0x00300000 };
            cmd(d, 0xba001402, CHOICE(r, oh));          /* other mode H: the cycle type */
            cmd(d, 0xb9000302, 0x00000000);             /* other mode L: the alpha compare */
            break;
        }
        case 8: {
            static const uint32_t tx[2] = { 0x80008000, 0x40004000 };
            cmd(d, 0xbb000001 | (randrange(r, 8) << 8), CHOICE(r, tx));
            break;
        }
        case 9:
            cmd(d, 0xb6000000, CHOICE(r, gm));
            cmd(d, 0xb7000000, CHOICE(r, gm));
            break;
        case 10:
            f3_mw(d, 6, 4 * (1 + randrange(r, 15)), randrange(r, 0x100) * 0x100);      /* a segment other than 0 */
            break;
        case 11:
            f3_mw(d, 8, 0, (uint32_t)(randint(r, 0x100, 0x7fff) << 16) | randrange(r, 0x10000));   /* fog */
            break;
        case 12:
            f3_persp(d, o, randrange(r, 0x10000));
            break;
        default:
            f3_tri1f(d, o, va, vb, v0 + randrange(r, n), o->flat ? randrange(r, 3) : 0);
            break;
        }
    }
    if (randrange(r, 2))
        cmd(d, 0x06010000, c->base + 0x900);            /* a jump: the list ends in the other one */
    else
        f3_enddl(d);
}

NOINLINE static void f3_lit(struct probe_case *c, struct dl *d, struct rng *r, const struct probe_f3ops *o)
{
    static const uint32_t tg[3] = { 0, G1_TEXGEN, G1_TEXGEN | G1_TEXLIN };
    static const uint32_t fg[2] = { 0, G1_FOG };
    static const int tz[2] = { 0x1ff, 0 };
    static const double sc[3] = { 1, 0.5, 1.0 / 8 };
    static const unsigned ns[5] = { 3, 4, 5, 8, 11 };
    uint32_t gm = G1_SHADE | G1_SMOOTH | G1_ZB | G1_LIGHT | CHOICE(r, tg) | CHOICE(r, fg);
    unsigned nl = 1 + randrange(r, 5), slot, b, vaddr = 0x600;
    double m[16];
    uint8_t buf[512];
    f3_common(c, d, r, o, gm, CHOICE(r, tz), 1);
    if (gm & G1_FOG)
        f3_mw(d, 8, 0, (uint32_t)(randint(r, 0x100, 0x7fff) << 16) | randrange(r, 0x10000));
    f3_mw(d, 2, 0, 0x80000000u + 32 * (nl + 1));        /* G_MW_NUMLIGHT */
    for (slot = 0; slot < nl + 3; slot++) {             /* look-at Y, look-at X, the lights, the ambient light */
        uint8_t blob[16];
        unsigned idx = slot == 0 ? 0x82 : slot == 1 ? 0x84 : 0x86 + 2 * (slot - 2);
        memset(blob, 0, sizeof(blob));
        if (slot >= 2) {
            uint8_t col[4] = { (uint8_t)randrange(r, 256), (uint8_t)randrange(r, 256), (uint8_t)randrange(r, 256), 0 };
            uint8_t dir[4] = { (uint8_t)randrange(r, 256), (uint8_t)randrange(r, 256), (uint8_t)randrange(r, 256), 0 };
            memcpy(blob, col, 4);
            memcpy(blob + 4, col, 4);
            memcpy(blob + 8, dir, 4);
        } else
            blob[8 + (slot == 0 ? 1 : 0)] = 0x7f;
        put(c, 0x1000 + 0x20 * slot, blob, 16);
        cmd(d, 0x03000010 | (idx << 16), c->base + 0x1000 + 0x20 * slot);
    }
    ident(m);
    mtx(buf, m);
    put(c, 0x500, buf, 64);
    rnd_matrix(r, CHOICE(r, sc), m);
    mtx(buf, m);
    put(c, 0x540, buf, 64);
    f3_mtx(d, 3, c->base + 0x540);
    f3_mtx(d, 2, c->base + 0x500);
    for (b = 0; b < 2; b++) {
        unsigned n = CHOICE(r, ns), v0 = randrange(r, o->nslots - n + 1), i, q;
        uint8_t vs[16 * 16];
        if (!o->vtx_wr && v0 > 15)
            v0 = 15;
        for (i = 0; i < n; i++) {
            uint8_t col[4];
            col[0] = (uint8_t)randint(r, -127, 127); col[1] = (uint8_t)randint(r, -127, 127); col[2] = (uint8_t)randint(r, -127, 127); col[3] = (uint8_t)randrange(r, 256);
            vtx(vs + 16 * i, randint(r, -200, 200), randint(r, -200, 200), randint(r, -200, 200), randint(r, -1024, 1023), randint(r, -1024, 1023), col);
        }
        put(c, vaddr, vs, 16 * n);
        f3_vtx(d, o, n, v0, c->base + vaddr);
        vaddr += 0x100;
        for (q = 0; q < 4; q++) {
            unsigned a, b2, cc;
            sample3(r, v0, n, &a, &b2, &cc);
            f3_tri1(d, o, a, b2, cc);
        }
        if (b == 0)
            f3_mtx(d, 4, c->base + 0x540);              /* a pushed model-view: the light directions are transformed again */
        else {                                          /* popped: the builds whose G_POPMTX marks the lights stale transform them again with the popped matrix, the others keep them */
            cmd(d, 0xbd000000, 0);
            f3_vtx(d, o, n, v0, c->base + vaddr - 0x100);
            f3_tri1(d, o, v0, v0 + 1, v0 + 2);
        }
    }
    f3_enddl(d);
}

NOINLINE static void f3_mtxcase(struct probe_case *c, struct dl *d, struct rng *r, const struct probe_f3ops *o)
{
    static const int tz[2] = { 0x1ff, 0 };
    static const uint16_t sm[5] = { 0, 1, 0xffff, 0x8000, 0x7fff };
    unsigned nm = 2 + randrange(r, 4), k, i;
    uint32_t geom = randrange(r, 2) * G1_SHADE | randrange(r, 2) * G1_ZB;
    uint8_t buf[64], vs[64];
    f3_common(c, d, r, o, geom, CHOICE(r, tz), 0);
    for (k = 0; k < nm; k++) {
        for (i = 0; i < 64; i++)
            buf[i] = (uint8_t)randrange(r, 256);
        if (randrange(r, 2))
            for (i = 0; i < 16; i++) {
                unsigned q = randrange(r, 6);
                be16(buf + 2 * i, q < 5 ? sm[q] : randrange(r, 1u << 16));
            }
        put(c, 0x500 + 0x40 * k, buf, 64);
        f3_mtx(d, randrange(r, 8), c->base + 0x500 + 0x40 * k);
        if (randrange(r, 4) == 0)
            cmd(d, 0xbd000000, 0);
    }
    for (i = 0; i < 4; i++) {
        const uint8_t col[4] = { 1, 2, 3, 4 };
        vtx(vs + 16 * i, randint(r, -40, 40), randint(r, -40, 40), randint(r, -40, 40), randint(r, -512, 511), randint(r, -512, 511), col);
    }
    put(c, 0x600, vs, 64);
    f3_vtx(d, o, 4, 0, c->base + 0x600);
    f3_tri1(d, o, 0, 1, 2);
    f3_enddl(d);
}

/* rej_case in Fast3D's GBI: groups of three vertices beyond each plane (the build's 16 slots: two loads), each drawn both ways and culled in a list of its own with an RDP witness behind the cull
 * (a wrong cull mask ends the list before it); the near-plane group is rejected by a mask with the near bits (0x7070) and clipped without them (Pilotwings' 0x7030). The second variant puts w < 0;
 * then the spread vertices, in the second slots of three pairs, have only bit 0x40 in common: Pilotwings' sign of w (SM64's: z <= -w), which G_TRI1 and G_CULLDL test with the build's masks. */
NOINLINE static void f3_rej(struct probe_case *c, struct dl *d, struct rng *r, unsigned seed, const struct probe_f3ops *o)
{
    static const int spread[3][3] = { { 100, -100, -100 }, { -100, 100, -100 }, { 100, 100, -100 } };
    double m[16] = { 0.125, 0, 0, 0, 0, 0.125, 0, 0, 0, 0, 0.125, 0, 0, 0, 0, 1 }, mv[16];
    uint8_t buf[64], vs[16 * 16];
    unsigned g, i, half;
    uint32_t geom = randrange(r, 2) * G1_SHADE;
    f3_common(c, d, r, o, geom, 0x1ff, 0);
    if (seed & 1)
        m[15] = -1;
    ident(mv);
    mtx(buf, mv);
    put(c, 0x500, buf, 64);
    mtx(buf, m);
    put(c, 0x540, buf, 64);
    f3_mtx(d, 3, c->base + 0x540);
    f3_mtx(d, 2, c->base + 0x500);
    for (half = 0; half < 2; half++) {                  /* groups 0 .. 3, then 4 .. 7 */
        unsigned nv = 0;
        for (g = 4 * half; g < 4 * half + 4; g++)
            for (i = 0; i < 3; i++) {
                int x = randint(r, -3, 3), y = randint(r, -3, 3), z = randint(r, -3, 3);
                uint8_t col[4];
                switch (g) {
                case 0: z = 100; break;
                case 1: z = -100; break;
                case 2: x = 100; break;
                case 3: x = -100; break;
                case 4: y = 100; break;
                case 5: y = -100; break;
                case 7: x = spread[i][0]; y = spread[i][1]; z = spread[i][2]; break;
                default: break;
                }
                rcol(r, col);
                vtx(vs + 16 * nv, x, y, z, 0, 0, col);
                nv++;
            }
        put(c, 0x600 + 0x100 * half, vs, 16 * nv);
        f3_vtx(d, o, nv, 0, c->base + 0x600 + 0x100 * half);
        for (g = 0; g < 4; g++) {
            f3_tri1(d, o, 3 * g, 3 * g + 1, 3 * g + 2);
            f3_tri1(d, o, 3 * g + 2, 3 * g + 1, 3 * g);
        }
        for (g = 0; g < 4; g++) {
            struct dl sub;
            if (4 * half + g == 6)
                continue;
            memset(&sub, 0, sizeof(sub));
            f3_culldl(&sub, 3 * g, 3 * g + 2);
            f3_tri1(&sub, o, 6, 7, 8);
            cmd(&sub, 0xf7000000, 0x5a5a0000 | (4 * half + g));     /* (a witness in the RDP stream that the list went on: the triangle may be rejected too) */
            f3_enddl(&sub);
            f1_sub(c, 0xa00 + 0x20 * (4 * half + g), &sub);
            cmd(d, 0x06000000, c->base + 0xa00 + 0x20 * (4 * half + g));
        }
    }
    /* the spread vertices again, in the second slots of three pairs, as one triangle, both ways, and culled */
    {
        uint8_t col[4] = { 200, 100, 50, 255 };
        struct dl sub;
        for (i = 0; i < 6; i++)
            vtx(vs + 16 * i, spread[i / 2][0], spread[i / 2][1], spread[i / 2][2], 0, 0, col);
        put(c, 0x800, vs, 16 * 6);
        f3_vtx(d, o, 6, 2, c->base + 0x800);
        f3_tri1(d, o, 3, 5, 7);
        f3_tri1(d, o, 7, 5, 3);
        memset(&sub, 0, sizeof(sub));
        f3_culldl(&sub, 3, 3);
        f3_culldl(&sub, 5, 5);
        f3_culldl(&sub, 7, 7);
        cmd(&sub, 0xf7000000, 0x5a5a00ff);
        f3_enddl(&sub);
        f1_sub(c, 0xb00, &sub);
        cmd(d, 0x06000000, c->base + 0xb00);
    }
    /* Three vertices just beyond the clip-ratio planes on alternating sides (|coordinate| = 2 w, the ratio 3): with w > 0 they share no compare bit; with w < 0 (the second variant) each has both
     * clip-ratio bits of every axis and no W bit in common, so only a G_CULLDL mask with the clip-ratio bits (SM64's 0xffff, not 0x7070) ends the list before its witness */
    {
        static const int g3[3][3] = { { 16, -16, -16 }, { -16, 16, 16 }, { 16, 16, -16 } };
        uint8_t col[4] = { 10, 20, 30, 255 };
        struct dl sub;
        for (i = 0; i < 3; i++)
            vtx(vs + 16 * i, g3[i][0], g3[i][1], g3[i][2], 0, 0, col);
        put(c, 0x880, vs, 16 * 3);
        f3_vtx(d, o, 3, 9, c->base + 0x880);
        memset(&sub, 0, sizeof(sub));
        f3_culldl(&sub, 9, 11);
        cmd(&sub, 0xf7000000, 0x5a5a00fe);
        f3_enddl(&sub);
        f1_sub(c, 0xb40, &sub);
        cmd(d, 0x06000000, c->base + 0xb40);
    }
    f3_enddl(d);
}

void probe_case_make_f3(struct probe_case *c, unsigned kind, unsigned seed, uint32_t base, const struct probe_f3ops *ops)
{
    struct rng r;
    struct dl d;
    unsigned i;
    uint8_t *p;
    memset(c, 0, sizeof(*c));
    memset(&d, 0, sizeof(d));
    c->base = base;
    r.s = ((uint64_t)seed * 0x9e3779b97f4a7c15ull) ^ ((uint64_t)(kind + 1) * 0xd1b54a32d192ed03ull) ^ 0x5a5aa5a51234fedcull;
    for (i = 0; i < 8; i++)
        (void)rnd(&r);
    switch (kind) {
    case PROBE_CLIP: case PROBE_ZNEG: case PROBE_TRI: case PROBE_MIX: case PROBE_WILD:
        f3_geometry(c, &d, &r, kind, ops);
        break;
    case PROBE_MISC: f3_misc(c, &d, &r, ops); break;
    case PROBE_LIT: f3_lit(c, &d, &r, ops); break;
    case PROBE_MTX: f3_mtxcase(c, &d, &r, ops); break;
    default: f3_rej(c, &d, &r, seed, ops); break;
    }
    if (d.n > 126) {
        d.n = 126;
        f3_enddl(&d);
    }
    p = c->mem;
    for (i = 0; i < d.n; i++) {
        p[8 * i] = (uint8_t)(d.w[i][0] >> 24); p[8 * i + 1] = (uint8_t)(d.w[i][0] >> 16); p[8 * i + 2] = (uint8_t)(d.w[i][0] >> 8); p[8 * i + 3] = (uint8_t)d.w[i][0];
        p[8 * i + 4] = (uint8_t)(d.w[i][1] >> 24); p[8 * i + 5] = (uint8_t)(d.w[i][1] >> 16); p[8 * i + 6] = (uint8_t)(d.w[i][1] >> 8); p[8 * i + 7] = (uint8_t)d.w[i][1];
    }
    c->dl_bytes = d.n * 8;
    if (c->dl_bytes > c->used)
        c->used = c->dl_bytes;
}

const char *const probe_case_names[PROBE_NCASES] = { "clip", "zneg", "tri", "mix", "wild", "misc", "lit", "mtx", "rej", "bz", "s2rect", "s2ldtx", "s2bg", "s2copy", "s2misc" };

void probe_case_make(struct probe_case *c, unsigned kind, unsigned seed, uint32_t base)
{
    struct rng r;
    struct dl d;
    unsigned i;
    uint8_t *p;
    memset(c, 0, sizeof(*c));
    memset(&d, 0, sizeof(d));
    c->base = base;
    r.s = ((uint64_t)seed * 0x9e3779b97f4a7c15ull) ^ ((uint64_t)(kind + 1) * 0xd1b54a32d192ed03ull) ^ 0x1234567887654321ull;
    for (i = 0; i < 8; i++)
        (void)rnd(&r);
    switch (kind) {
    case PROBE_CLIP: case PROBE_ZNEG: case PROBE_TRI: case PROBE_MIX: case PROBE_WILD:
        geometry_cases(c, &d, &r, kind);
        break;
    case PROBE_MISC:
        misc_case(c, &d, &r);
        break;
    case PROBE_LIT:
        lit_case(c, &d, &r);
        break;
    case PROBE_MTX:
        mtx_case(c, &d, &r);
        break;
    case PROBE_REJ:
        rej_case(c, &d, &r, seed);
        break;
    case PROBE_S2RECT: case PROBE_S2LDTX: case PROBE_S2BG: case PROBE_S2COPY: case PROBE_S2MISC:
        s2_case(c, &d, &r, kind);
        break;
    default:
        bz_case(c, &d, &r, seed);
        break;
    }
    if (d.n > 126) {                                    /* (the display list must end before the viewport at 0x400) */
        d.n = 126;
        cmd(&d, 0xdf000000, 0);
    }
    p = c->mem;
    for (i = 0; i < d.n; i++) {
        p[8 * i] = (uint8_t)(d.w[i][0] >> 24); p[8 * i + 1] = (uint8_t)(d.w[i][0] >> 16); p[8 * i + 2] = (uint8_t)(d.w[i][0] >> 8); p[8 * i + 3] = (uint8_t)d.w[i][0];
        p[8 * i + 4] = (uint8_t)(d.w[i][1] >> 24); p[8 * i + 5] = (uint8_t)(d.w[i][1] >> 16); p[8 * i + 6] = (uint8_t)(d.w[i][1] >> 8); p[8 * i + 7] = (uint8_t)d.w[i][1];
    }
    c->dl_bytes = d.n * 8;
    if (c->dl_bytes > c->used)
        c->used = c->dl_bytes;
}
