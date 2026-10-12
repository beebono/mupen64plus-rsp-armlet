/*
 * fxn_clip.c: plain-C sanity test of the struct-level clipper (src/hle/f3dex_clip_fx.c), no JIT. The differential test against the real microcode is spec/fxclip_spec.c (rt_test).
 *   build_fxn.sh fxn_clip host ;  run: fxn_clip [random triangles (default 200000)] [seed]
 * Checks, for the F3DEX, F3DEX.NoN (no near plane: it is w = 0) and F3DLX builds:
 *  - a triangle inside the frustum comes back unchanged ((v[0], v[1], v[2]), no new vertex); a triangle outside a plane is discarded;
 *  - one vertex beyond each of the six planes gives 2 triangles and 2 new vertices on the plane, two vertices beyond it give 1 triangle and 2 new vertices;
 *  - every returned pointer is an input or a new vertex, every new vertex is used, the orientation of every triangle is that of the input;
 *  - random triangles against an independent double-precision Sutherland-Hodgman with the same plane order and list rules: same polygon size, the vertices (position, colour) within a tolerance;
 *  - F3DLX: the screen y of a new vertex is a multiple of 4;
 *  - (built with FXFLAGS=-DF3DEX_CLIP_COVER) a directed hunt for the weight sf == 0 clamp (docs/ucode-f3dex/clip.md 6.2), which random geometry reaches only rarely: the new vertex must be
 *    the one the clamped weight sf = 1 gives.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "f3dex_fx.h"

static uint64_t rs = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void)
{
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (uint32_t)(rs >> 16);
}

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; if (fails < 40) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

enum { NEAR, FAR, YP, XP, YN, XN };
static const int16_t plane_vec[6][4] = { { 0, 0, 1, 1 }, { 0, 0, 1, -1 }, { 0, 1, 0, -1 }, { 1, 0, 0, -1 }, { 0, 1, 0, 1 }, { 1, 0, 0, 1 } };
static const uint16_t plane_msk[6] = { 0x0040, 0x4000, 0x0200, 0x0100, 0x0002, 0x0001 };

/* identity model-view-projection with w = 100.0: the clip coordinates are the input integers, w = 100 */
static void mkctx(struct f3dex_vctx *c, const struct f3dex_build *b)
{
    memset(c, 0, sizeof *c);
    c->b = b;
    for (int r = 0; r < 3; r++)
        c->mp.i[r][r] = 1;
    c->mp.i[3][3] = 100;
    c->vscale[0] = 640; c->vscale[1] = 480; c->vscale[2] = 0x1ff; c->vscale[3] = 0;
    c->vtrans[0] = 640; c->vtrans[1] = 480; c->vtrans[2] = 0x1ff; c->vtrans[3] = 0;
    c->vsign[0] = 1; c->vsign[1] = -1; c->vsign[2] = 1; c->vsign[3] = 1;
    c->lim[0] = 4090; c->lim[1] = -4090; c->lim[2] = 0x7fff; c->lim[3] = 0;
    c->persp = 0xffff;
    c->clip_k = 1;
    c->tscale[0] = c->tscale[1] = 0x8000;
    c->geom = F3DEX_G_CLIPPING | F3DEX_G_SHADE | F3DEX_G_ZBUFFER;
    c->fog[0] = 256; c->fog[1] = 0; c->fog[2] = 255;
    c->tgbias = 0x4000;
    memcpy(c->plane, plane_vec, sizeof plane_vec);
    memcpy(c->plane_mask, plane_msk, sizeof plane_msk);
    if (b->non) {
        c->plane[NEAR][2] = 0;                      /* near is w = 0 */
    }
}

static void mkvtx(const struct f3dex_vctx *c, const int xyz[3][3], struct f3dex_vtx v[3])
{
    static const uint8_t cols[3][4] = { { 255, 0, 40, 255 }, { 0, 255, 90, 255 }, { 0, 20, 255, 255 } };
    struct f3dex_vin in[3];
    for (int i = 0; i < 3; i++) {
        in[i].x = (int16_t)xyz[i][0]; in[i].y = (int16_t)xyz[i][1]; in[i].z = (int16_t)xyz[i][2];
        in[i].s = (int16_t)(1000 * i); in[i].t = (int16_t)(3000 - 700 * i);
        memcpy(in[i].c, cols[i], 4);
    }
    if (f3dex_vtx_xform_scalar(c, in, 3, v)) { printf("xform declined\n"); exit(2); }
}

static double px(const struct f3dex_vtx *v, int k) { return (double)v->cx[k] + v->cf[k] / 65536.0; }

static double area(const struct f3dex_vtx *a, const struct f3dex_vtx *b, const struct f3dex_vtx *d)
{
    double ax = px(a, 0) / px(a, 3), ay = px(a, 1) / px(a, 3), bx = px(b, 0) / px(b, 3), by = px(b, 1) / px(b, 3), dx = px(d, 0) / px(d, 3), dy = px(d, 1) / px(d, 3);
    return (bx - ax) * (dy - ay) - (by - ay) * (dx - ax);
}

/* the structural checks every result must pass; returns the polygon P[0 .. ntri + 2) of the fan */
static void check_structure(const struct f3dex_clip_out *o, const struct f3dex_vtx *in[3], const char *what, int all_new_used)
{
    int used[F3DEX_CLIP_MAX_NEW] = { 0 };
    CHECK(o->ntri <= 7 && o->nnew <= F3DEX_CLIP_MAX_NEW, "%s: ntri %u nnew %u", what, o->ntri, o->nnew);
    for (unsigned k = 0; k < o->ntri; k++)
        for (int i = 0; i < 3; i++) {
            const struct f3dex_vtx *p = o->tri[k][i];
            int ok = p == in[0] || p == in[1] || p == in[2];
            for (unsigned j = 0; j < o->nnew; j++)
                if (p == &o->nv[j]) { ok = 1; used[j] = 1; }
            CHECK(ok, "%s: triangle %u pointer %d is not an input or a new vertex", what, k, i);
        }
    for (unsigned j = 0; j < o->nnew; j++)
        CHECK(used[j] || o->ntri == 0 || !all_new_used, "%s: new vertex %u unused", what, j);   /* with several planes an earlier plane's vertex may be cut away later */
    /* the fan shares P[0] and chains P[k+1]: tri[k] = (P[k+2], P[k+1], P[0]) */
    for (unsigned k = 0; k + 1 < o->ntri; k++)
        CHECK(o->tri[k][2] == o->tri[k + 1][2] && o->tri[k][0] == o->tri[k + 1][1], "%s: fan chain broken at %u", what, k);
}

/* ---- single-plane cuts --------------------------------------------------------------------------------------------------------------------------------------------------------- */
static void test_planes(const struct f3dex_build *b, const char *bn)
{
    struct f3dex_vctx c;
    mkctx(&c, b);
    const int base[3][3] = { { -30, -20, 0 }, { 30, -20, 0 }, { 0, 30, 0 } };
    /* the inside triangle */
    {
        struct f3dex_vtx v[3];
        mkvtx(&c, base, v);
        const struct f3dex_vtx *in[3] = { &v[0], &v[1], &v[2] };
        struct f3dex_clip_out o;
        f3dex_clip_tri_scalar(&c, in, &o);
        CHECK(o.ntri == 1 && o.nnew == 0 && o.tri[0][0] == in[0] && o.tri[0][1] == in[1] && o.tri[0][2] == in[2], "%s: inside triangle: ntri %u nnew %u", bn, o.ntri, o.nnew);
        f3dex_clip_tri(&c, in, &o);
        CHECK(o.ntri == 1 && o.nnew == 0, "%s: f3dex_clip_tri", bn);
    }
    for (int p = 0; p < 6; p++) {
        if (b->non && p == NEAR)
            continue;
        for (int nout = 1; nout <= 3; nout++)
            for (int first = 0; first < 3; first++) {
                int xyz[3][3];
                memcpy(xyz, base, sizeof xyz);
                for (int s = 0; s < nout; s++) {
                    int i = (first + s) % 3;
                    switch (p) {
                    case NEAR: xyz[i][2] = -150 - 10 * s; break;
                    case FAR: xyz[i][2] = 150 + 10 * s; break;
                    case YP: xyz[i][1] = 150 + 10 * s; break;
                    case XP: xyz[i][0] = 150 + 10 * s; break;
                    case YN: xyz[i][1] = -150 - 10 * s; break;
                    case XN: xyz[i][0] = -150 - 10 * s; break;
                    }
                }
                struct f3dex_vtx v[3];
                mkvtx(&c, xyz, v);
                const struct f3dex_vtx *in[3] = { &v[0], &v[1], &v[2] };
                struct f3dex_clip_out o;
                f3dex_clip_tri_scalar(&c, in, &o);
                char what[64];
                snprintf(what, sizeof what, "%s plane %d nout %d first %d", bn, p, nout, first);
                check_structure(&o, in, what, 1);
                if (nout == 3) {
                    CHECK(o.ntri == 0 && o.nnew == 0, "%s: discard: ntri %u nnew %u", what, o.ntri, o.nnew);
                    continue;
                }
                CHECK(o.nnew == 2 && o.ntri == (nout == 1 ? 2u : 1u), "%s: ntri %u nnew %u", what, o.ntri, o.nnew);
                double a0 = area(&v[0], &v[1], &v[2]);
                CHECK(fabs(a0) > 1e-9, "%s: degenerate input", what);
                for (unsigned k = 0; k < o.ntri; k++)
                    CHECK(area(o.tri[k][0], o.tri[k][1], o.tri[k][2]) * a0 > 0, "%s: triangle %u orientation (%g vs %g)", what, k, area(o.tri[k][0], o.tri[k][1], o.tri[k][2]), a0);
                for (unsigned j = 0; j < o.nnew; j++) {
                    double d = 0;
                    for (int k = 0; k < 4; k++)
                        d += c.plane[p][k] * px(&o.nv[j], k);
                    CHECK(fabs(d) < 0.05, "%s: new vertex %u is %g off the plane", what, j, d);
                    if (b->dlx)
                        CHECK((o.nv[j].sy & 3) == 0, "%s: F3DLX screen y %d", what, o.nv[j].sy);
                }
            }
    }
}

/* ---- the independent double-precision clipper ---------------------------------------------------------------------------------------------------------------------------------- */
struct dv { double p[4], col[4], s, t; };
static int d_out(int p, const struct dv *v)
{
    const double *q = v->p;
    switch (p) {
    case NEAR: return q[2] <= -q[3];
    case FAR: return q[2] >= q[3];
    case YP: return q[1] >= q[3];
    case XP: return q[0] >= q[3];
    case YN: return q[1] <= -q[3];
    default: return q[0] <= -q[3];
    }
}
static double d_val(int p, const struct dv *v)
{
    double d = 0;
    for (int k = 0; k < 4; k++)
        d += plane_vec[p][k] * v->p[k];
    return d;
}
static struct dv d_new(int p, const struct dv *O, const struct dv *Q)
{
    double dO = d_val(p, O), dQ = d_val(p, Q), t = -dO / (dQ - dO);
    struct dv n;
    for (int k = 0; k < 4; k++) { n.p[k] = O->p[k] + t * (Q->p[k] - O->p[k]); n.col[k] = O->col[k] + t * (Q->col[k] - O->col[k]); }
    n.s = O->s + t * (Q->s - O->s);
    n.t = O->t + t * (Q->t - O->t);
    return n;
}
static unsigned d_clip(const struct f3dex_build *b, const struct dv in[3], struct dv P[16], unsigned *nnew)
{
    struct dv L[16], M[16];
    unsigned n = 3;
    L[0] = in[2]; L[1] = in[1]; L[2] = in[0];
    *nnew = 0;
    for (int p = 0; p < 6; p++) {
        if (b->non && p == NEAR)
            continue;
        int out[16], nout = 0, first = -1;
        for (unsigned i = 0; i < n; i++) {
            out[i] = d_out(p, &L[i]);
            nout += out[i];
            if (out[i] && first < 0) first = (int)i;
        }
        if (nout == 0) continue;
        if (nout == (int)n) return 0;
        unsigned q = ((unsigned)first + 1) % n;
        while (out[q]) q = (q + 1) % n;
        unsigned r = (q + 1) % n;
        while (!out[r]) r = (r + 1) % n;
        unsigned m = 0;
        M[m++] = d_new(p, &L[(q + n - 1) % n], &L[q]);
        for (unsigned i = q; i != r; i = (i + 1) % n) M[m++] = L[i];
        M[m++] = d_new(p, &L[r], &L[(r + n - 1) % n]);
        *nnew += 2;
        memcpy(L, M, m * sizeof L[0]);
        n = m;
    }
    memcpy(P, L, n * sizeof L[0]);
    return n;
}

static struct dv to_dv(const struct f3dex_vtx *v)
{
    struct dv d;
    for (int k = 0; k < 4; k++) { d.p[k] = px(v, k); d.col[k] = v->col[k]; }
    d.s = v->s;
    d.t = v->t;
    return d;
}

static double max_pos_err, max_col_err, max_rel_err;
static unsigned compared, skipped, mism;

static void test_random(const struct f3dex_build *b, const char *bn, unsigned count)
{
    struct f3dex_vctx c;
    mkctx(&c, b);
    for (unsigned it = 0; it < count; it++) {
        int xyz[3][3], range = (rnd() % 4 == 0) ? 120 : (rnd() % 2 ? 250 : 1500);
        int amb = 0;
        for (int i = 0; i < 3; i++)
            for (int k = 0; k < 3; k++) {
                xyz[i][k] = (int)(rnd() % (2 * range + 1)) - range;
                if (abs(abs(xyz[i][k]) - 100) < 2) amb = 1;
            }
        if (amb) { skipped++; continue; }
        struct f3dex_vtx v[3];
        mkvtx(&c, xyz, v);
        const struct f3dex_vtx *in[3] = { &v[0], &v[1], &v[2] };
        struct f3dex_clip_out o;
        f3dex_clip_tri_scalar(&c, in, &o);
        char what[64];
        snprintf(what, sizeof what, "%s random %u", bn, it);
        check_structure(&o, in, what, 0);
        struct dv din[3] = { to_dv(&v[0]), to_dv(&v[1]), to_dv(&v[2]) }, P[16];
        unsigned nnew;
        unsigned n = d_clip(b, din, P, &nnew);
        if (n == 0) {
            if (o.ntri != 0) mism++;                 /* borderline: see below */
            continue;
        }
        /* a corner case: a new vertex exactly on a later plane (borderline flags) may legitimately differ */
        if (o.ntri + 2 != n || o.nnew != nnew) {
            mism++;
            if (getenv("FXN_DBG")) printf("%s: polygon size %u (nnew %u) vs %u (nnew %u)\n", what, o.ntri + 2, o.nnew, n, nnew);
            continue;
        }
        compared++;
        double ext = 0;                              /* the weights are 16-bit fractions with a rough reciprocal: the tolerance is relative to the size of the input */
        for (int a = 0; a < 3; a++)
            for (int k = 0; k < 3; k++)
                ext = fmax(ext, abs(xyz[a][k]));
        const struct f3dex_vtx *Q[9];
        Q[0] = o.tri[0][2]; Q[1] = o.tri[0][1];
        for (unsigned k = 0; k < o.ntri; k++) Q[k + 2] = o.tri[k][0];
        for (unsigned i = 0; i < n; i++) {
            struct dv g = to_dv(Q[i]);
            double e = 0, ce = 0;
            for (int k = 0; k < 4; k++) {
                e = fmax(e, fabs(g.p[k] - P[i].p[k]));
                ce = fmax(ce, fabs(g.col[k] - P[i].col[k]));
            }
            ce = fmax(ce, fmax(fabs(g.s - P[i].s), fabs(g.t - P[i].t)) / 8);
            max_pos_err = fmax(max_pos_err, e);
            max_rel_err = fmax(max_rel_err, e / ext);
            max_col_err = fmax(max_col_err, ce);
            CHECK(e < 0.05 * ext + 0.2 && ce < 7.0, "%s vertex %u: position err %g colour err %g (input %d %d %d / %d %d %d / %d %d %d)", what, i, e, ce,
                  xyz[0][0], xyz[0][1], xyz[0][2], xyz[1][0], xyz[1][1], xyz[1][2], xyz[2][0], xyz[2][1], xyz[2][2]);
        }
    }
}

#ifdef F3DEX_CLIP_COVER
struct f3dex_clip_cover {
    unsigned calls, plane_new[6], plane_noop[6], plane_disc[6], nnew[16], npoly[16], cf_sat, x_zero, sf_sat, sf1, sf0, xneg;
};
extern struct f3dex_clip_cover f3dex_clip_cov;

/* The weight clamp "sf == 0 -> 1" needs 3 + floor(T * cf) == 0: a tiny negative plane value of the inside vertex (inconsistent flags). Hunt random edges (O flagged outside the near plane, Q inside) with the
 * triangle (O, Q, Q): both new vertices lie on the edge (O, Q) and the expected position is computed here from sf = 1, tf = 65535. */
static void test_sf0(const struct f3dex_build *b, const char *bn, unsigned tries)
{
    struct f3dex_vctx c;
    mkctx(&c, b);
    unsigned found = 0;
    for (unsigned it = 0; it < tries && found < 6; it++) {
        struct f3dex_vtx v[3];
        memset(v, 0, sizeof v);
        for (int i = 0; i < 2; i++) {
            int sh = rnd() % 3 ? 16 : (int)(rnd() % 17) + 8;
            for (int k = 0; k < 4; k++) {
                int32_t val = (int32_t)(rnd() << 16 ^ rnd()) >> (rnd() % 20 + (32 - sh) % 12);
                v[i].cx[k] = (int16_t)(val >> 16);
                v[i].cf[k] = (uint16_t)val;
                v[i].col[k] = (uint8_t)rnd();
            }
            v[i].s = (int16_t)rnd(); v[i].t = (int16_t)rnd();
        }
        if (rnd() % 2) {                            /* a tiny negative D(Q): Q.z + Q.w small negative */
            v[1].cx[2] = (int16_t)(rnd() % 3 - 1); v[1].cf[2] = (uint16_t)rnd();
            int32_t w = ((int32_t)v[1].cx[3] << 16 | v[1].cf[3]);
            int32_t z = -w - (int32_t)(rnd() % 8);
            v[1].cx[2] = (int16_t)(z >> 16); v[1].cf[2] = (uint16_t)z;
        }
        v[0].flags = 0x40;                          /* O outside the near plane (flag-decided), Q inside */
        v[1].flags = 0;
        v[2] = v[1];
        const struct f3dex_vtx *in[3] = { &v[0], &v[1], &v[2] };
        struct f3dex_clip_out o;
        unsigned before = f3dex_clip_cov.sf0;
        f3dex_clip_tri_scalar(&c, in, &o);
        if (f3dex_clip_cov.sf0 == before || o.nnew != 2)      /* only the two vertices of the first plane */
            continue;
        found++;
        /* the clamped weights: sf = 1 for O, tf = 65535 for Q */
        for (unsigned j = 0; j < 2; j++)
            for (int k = 0; k < 4; k++) {
                int64_t Q32 = (int32_t)((uint32_t)(uint16_t)v[1].cx[k] << 16 | v[1].cf[k]), O32 = (int32_t)((uint32_t)(uint16_t)v[0].cx[k] << 16 | v[0].cf[k]);
                int64_t a = ((Q32 * 65535) >> 16) + ((O32 * 1) >> 16);
                int16_t ni = (int16_t)(a >> 16 > 32767 ? 32767 : a >> 16 < -32768 ? -32768 : a >> 16);
                uint16_t nf = (a >= -2147483648ll && a < 2147483648ll) ? (uint16_t)a : (a < 0 ? 0 : 0xffff);
                CHECK(o.nv[j].cx[k] == ni && o.nv[j].cf[k] == nf, "%s: sf==0 vertex %u lane %d: got %04x:%04x expected %04x:%04x", bn, j, k, (uint16_t)o.nv[j].cx[k], o.nv[j].cf[k], (uint16_t)ni, nf);
            }
    }
    CHECK(found >= 3, "%s: sf == 0 reached only %u times in %u tries", bn, found, tries);
    printf("%s: sf == 0 clamp: %u directed cases\n", bn, found);
}
#endif

int main(int argc, char **argv)
{
    unsigned count = argc > 1 ? (unsigned)atoi(argv[1]) : 200000;
    if (argc > 2) rs ^= (uint64_t)atoi(argv[2]) * 0x2545f4914f6cdd1dull;
    const struct { const struct f3dex_build *b; const char *n; } bs[] = { { &f3dex_build_dex, "F3DEX" }, { &f3dex_build_non, "F3DEX.NoN" }, { &f3dex_build_dlx, "F3DLX" } };
    for (int i = 0; i < 3; i++) {
        test_planes(bs[i].b, bs[i].n);
        test_random(bs[i].b, bs[i].n, count);
        printf("%s: %u random triangles compared (%u skipped near a plane), max position error %.4f (%.2e of the input extent), max colour/texture error %.3f\n", bs[i].n, compared, skipped, max_pos_err, max_rel_err, max_col_err);
        CHECK(mism * 1000 <= count, "%s: %u of %u random triangles clipped to a different polygon size than the double-precision clipper", bs[i].n, mism, count);
        if (mism) printf("%s: %u polygons differ in size from the double-precision clipper (new vertices on a later plane's boundary)\n", bs[i].n, mism);
        compared = skipped = mism = 0;
        max_rel_err = max_pos_err = max_col_err = 0;
#ifdef F3DEX_CLIP_COVER
        test_sf0(bs[i].b, bs[i].n, 4000000);
#endif
    }
#ifndef F3DEX_CLIP_COVER
    printf("(sf == 0 clamp hunt skipped: build with FXFLAGS=-DF3DEX_CLIP_COVER)\n");
#endif
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
