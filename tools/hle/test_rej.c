/*
 * test_rej.c: the Rej closed forms (src/hle/rej_fx.c, rej_dmem.h) on random DMEM images. With -DREJ_VTX_ORACLE=\"file\" / -DREJ_TRI_ORACLE=\"file\" (translations of the microcode's G_VTX / G_TRI
 * handlers that tools/rsp2c/rsp2c.py makes from the microcode text, docs/testing.md "Rej") every command must leave the same DMEM, outside the ring the RDP lists are written to, as the
 * microcode. Without an oracle the closed forms only have to finish; the NEON forms (aarch64) are compared with the scalar ones in any case.
 *
 * Local test only. Build and run: tools/hle/build_test_rej.sh
 */
#include <inttypes.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hle.h"
#if defined(REJ_VTX_ORACLE) || defined(REJ_TRI_ORACLE)
#define VU_FORCE_SCALAR 1
#include "ref_vu.h"
struct rctx {
    struct vu u;
    uint32_t r[32];
    unsigned pc;
    struct hle_task *t;
};
#define MUT(n, ok, bad) (ok)
#endif
#include "f3dex2_dmem.h"
#include "rej_dmem.h"

const struct f2_layout *f2L;
#define l_lay l_lay_rej_xbus

/* the translation reaches the framework only through the RDP registers (the flush routine) */
static uint32_t creg[16];
static uint32_t *const crp[16] = { &creg[0], &creg[1], &creg[2], &creg[3], &creg[4], &creg[5], &creg[6], &creg[7], &creg[8], &creg[9], &creg[10], &creg[11], &creg[12], &creg[13], &creg[14], &creg[15] };
int hle_mtc0(struct hle_task *t, unsigned rd, uint32_t value)
{
    (void)t;
    creg[rd & 15] = value;
    if ((rd & 15) == 9)                         /* the RDP takes every list at once */
        creg[10] = value;
    return 0;
}
int hle_poll_status(struct hle_task *t, unsigned rt, uint32_t *value) { (void)t; (void)rt; *value = 0; return 0; }
void hle_break(struct hle_task *t, unsigned end_pc) { (void)t; (void)end_pc; }

static uint64_t rs = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (uint32_t)(rs >> 16);
}
static int16_t r16(void)
{
    static const int16_t e[] = { 0, 1, 2, 0x7fff, -0x8000, -0x7fff, -1, -2, 0x4000, -0x4000, 0xff, -0x100, 0x100, 0x7f00, -0x7f00 };
    unsigned m = rnd() % 8;
    if (m == 0)
        return e[rnd() % (sizeof(e) / sizeof(e[0]))];
    if (m < 4)
        return (int16_t)(rnd() % 512) - 256;
    return (int16_t)rnd();
}

#define RDRAM_SIZE (8u << 20)
static uint8_t rdram[RDRAM_SIZE];
static struct hle_task T = { .rdram = rdram, .cr = crp };

static uint8_t data_seg[0x800];
static void st16(uint32_t *dm, unsigned a, uint16_t v) { hle_u16a *p = (hle_u16a *)dm; p[((a & 0xfff) ^ 2) >> 1] = v; }
static void st8(uint32_t *dm, unsigned a, uint8_t v) { ((uint8_t *)dm)[(a & 0xfff) ^ 3] = v; }
static void st32(uint32_t *dm, unsigned a, uint32_t v) { dm[(a & 0xfff) >> 2] = v; }
static uint16_t ld16(const uint32_t *dm, unsigned a) { return ((const hle_u16a *)dm)[((a & 0xfff) ^ 2) >> 1]; }

/* the DMEM of a task at the first fetch of the command loop: the data segment (the constants, the tables), the state the display lists set up, random */
static void base_dmem(uint32_t *dm)
{
    unsigned k;
    memset(dm, 0, 4096);
    for (k = 0; k < 0x800; k++)
        st8(dm, k, data_seg[k]);
    for (k = 0x400; k < 0x800; k++)                 /* (the data segment's tail is the leftovers of the game's previous task: the vertex area and more) */
        if (k >= 0x400)
            st8(dm, k, (uint8_t)rnd());
    for (k = 0x380; k < 0x400; k++)                 /* the slot address table is the microcode's */
        st8(dm, k, data_seg[k]);
    for (k = 0x400; k < 0x410; k++)
        st8(dm, k, data_seg[k]);
}

static void rand_mtx(uint32_t *dm, unsigned a, int small)
{
    unsigned k;
    for (k = 0; k < 16; k++) {
        st16(dm, a + 2 * k, (uint16_t)(small ? r16() / 16 : r16()));
        st16(dm, a + 0x20 + 2 * k, (uint16_t)r16());
    }
}

/* the matrices, viewport, projection state, geometry mode and lights a G_VTX finds */
static void vtx_state(uint32_t *dm, unsigned *lit_out)
{
    unsigned k, m, lit, nl, mode = rnd() % 8;
    rand_mtx(dm, 0x00, 1);
    rand_mtx(dm, 0x40, 1);
    rand_mtx(dm, 0x80, 1);
    if (rnd() % 4 == 0)                              /* a plausible projection: identity-ish modelview, perspective in P */
        for (k = 0; k < 16; k++) {
            st16(dm, 0x00 + 2 * k, (k % 5 == 0) ? 1 : 0);
            st16(dm, 0x20 + 2 * k, 0);
        }
    st8(dm, 0x1d9, (uint8_t)(rnd() % 2 ? rnd() | 1 : 0));
    st16(dm, 0xdc, (uint16_t)(rnd() % 4 == 0 ? r16() : rnd() % 2 ? 0xffff : 0xe000 + rnd() % 0x2000));
    st16(dm, 0xde, (uint16_t)r16());
    for (k = 0; k < 4; k++) {
        st16(dm, 0xe0 + 2 * k, (uint16_t)(mode == 0 ? r16() : k == 0 ? 640 : k == 1 ? 480 : k == 2 ? 0x1ff : 0x100));
        st16(dm, 0xe8 + 2 * k, (uint16_t)(mode == 0 ? r16() : k == 0 ? 640 : k == 1 ? 480 : k == 2 ? 0x1ff : 0));
    }
    st16(dm, 0x186, (uint16_t)(rnd() % 3 == 0 ? r16() : 1 + rnd() % 6));
    st16(dm, 0x1e0, (uint16_t)r16());
    st16(dm, 0x1e2, (uint16_t)r16());
    st16(dm, 0x1e8, (uint16_t)(rnd() % 3 == 0 ? r16() : 0xffff - rnd() % 4));
    st16(dm, 0x1ea, (uint16_t)(rnd() % 3 == 0 ? r16() : 0x8000));
    if (rnd() % 4 == 0)
        for (k = 0; k < 3; k++)
            st16(dm, 0x1d0 + 2 * k, (uint16_t)r16());
    lit = rnd() % 2;
    nl = 1 + rnd() % 7;
    st32(dm, 0x1ec, (rnd() % 2 ? 1u : 0) | (rnd() % 2 ? 4u : 0) | (rnd() % 2 ? 0x200000u : 0) | (rnd() % 2 ? 0x10000u : 0) | (lit ? 0x20000u : 0) | (rnd() % 2 ? 0x40000u : 0) | (rnd() % 2 ? 0x80000u : 0));
    st8(dm, 0x1dd, (uint8_t)(24 * nl));
    st8(dm, 0x1dc, (uint8_t)(rnd() % 2 ? rnd() | 1 : 0));
    for (k = 0; k < 24 * 9; k++) {
        st8(dm, 0x1f8 + k, (uint8_t)rnd());
        st8(dm, 0x200 + k, (uint8_t)rnd());
    }
    for (m = 0; m <= nl + 1; m++)                    /* the processed directions as the setup leaves them (copies equal), colours of the game */
        for (k = 0; k < 4; k++)
            st8(dm, 0x200 + 24 * m + 4 + k, ((const uint8_t *)dm)[(0x200 + 24 * m + k) ^ 3]);
    for (k = 0; k < 16; k++)                         /* segment table: mostly 0 */
        st32(dm, 0xf8 + 4 * k, rnd() % 4 ? 0 : (rnd() & 0xff000) );
    *lit_out = lit;
}

static void rand_vin(uint32_t va, int wild)
{
    uint8_t q[16];
    uint8_t *p = q;
    unsigned kk;
    unsigned k;
    int range = wild ? 0x7fff : 0x1800;
    for (k = 0; k < 3; k++) {
        int16_t v = (int16_t)((int)(rnd() % (2 * range + 1)) - range);
        if (rnd() % 16 == 0)
            v = r16();
        p[2 * k] = (uint8_t)((uint16_t)v >> 8);
        p[2 * k + 1] = (uint8_t)v;
    }
    p[6] = (uint8_t)rnd();
    p[7] = (uint8_t)rnd();
    for (k = 8; k < 16; k++)
        p[k] = (uint8_t)rnd();
    for (kk = 0; kk < 16; kk++)
        rdram[(va + kk) ^ 3] = q[kk];                /* (RDRAM is host words of big-endian words, like DMEM) */
}

/* the memories of the oracle (the DMA hook) */
static int rj_dma(struct rctx *x, uint32_t *r);
static int rj_wait(struct rctx *x, uint32_t *r) { (void)x; (void)r; return 0; }

static sigjmp_buf jb;
static void on_alarm(int s) { (void)s; siglongjmp(jb, 1); }

#ifdef REJ_VTX_ORACLE
#include REJ_VTX_ORACLE
#endif
#ifdef REJ_TRI_ORACLE
#include REJ_TRI_ORACLE
#endif

#if defined(REJ_VTX_ORACLE) || defined(REJ_TRI_ORACLE)
static int rj_dma(struct rctx *x, uint32_t *r)
{
    uint32_t *dm = x->u.dmem;
    unsigned mem = r[20], len = (r[19] & 0xfff) + 1, i;
    uint32_t dram = r[24] & 0xffffff;
    if (mem & 0x1000)
        return 0;
    len = (len + 7) & ~7u;
    mem &= 0xff8;
    dram &= ~7u;
    if ((int32_t)r[20] < 0)
        return 0;
    for (i = 0; i < len && mem + i < 0x1000; i++)
        ((uint8_t *)dm)[(mem + i) ^ 3] = rdram[((dram + i) & (RDRAM_SIZE - 1)) ^ 3];
    return 0;
}
static void oracle_regs(struct rctx *x, uint32_t *dm)
{
    memset(x, 0, sizeof *x);
    x->u.dmem = dm;
    x->t = &T;
    LQV(&x->u, 31, 0, 0x1b0); LQV(&x->u, 30, 0, 0x1c0);
    VXOR(&x->u, 0, 0, 0, 0); VADD(&x->u, 1, 0, 0, 0); VSUB(&x->u, 1, 0, 31, 8);
}
#else
static int rj_dma(struct rctx *x, uint32_t *r) { (void)x; (void)r; return 0; }
#endif

/* the padding bytes of the light slots (docs/ucode-f3dex2.md section 6; the same lanes in the Rej builds) */
static int dead(unsigned d) { return d >= 0x200 && d < 0x200 + 24 * 9 && ((d - 0x200) % 24 == 3 || (d - 0x200) % 24 == 7); }

static unsigned vtx_cases(unsigned n, unsigned *fails_out)
{
    unsigned i, fails = 0, ran = 0, marked = 0, lit_n = 0, odd = 0;
    static uint32_t a[1024], b[1024];
    for (i = 0; i < n; i++) {
        unsigned lit, nv = rnd() % 4 == 0 ? 1 + rnd() % 64 : 1 + rnd() % 8, v0, k, va, wild = rnd() % 4 == 0;
        uint32_t w0, w1;
        const char *why;
        struct fx2_vin in[66];
        base_dmem(a);
        vtx_state(a, &lit);
        v0 = rnd() % (65 - nv);
        w0 = 0x01000000u | nv << 12 | (v0 + nv) * 2;
        va = 0x10000 + 8 * (rnd() % 4096);
        w1 = va;
        if (rnd() % 8 == 0)
            w1 |= (rnd() % 16) << 24;                /* a segment address */
        st32(a, 0x950, w0);
        st32(a, 0x954, w1);
        va = (w1 & 0xffffff) + hle_dm32(&(struct hle_task){ .dmem = a }, 0xf8 + ((w1 >> 22) & 0x3c));
        for (k = 0; k < nv + 1; k++)
            rand_vin(va + 16 * k, wild);
        memcpy(b, a, sizeof a);
        f2L = &l_lay;
        T.dmem = b;
        why = rej_vtx_pre(&T, w0);
        if (why) {
            continue;
        }
        for (k = 0; k < nv; k++)
            f2_ld_vin_rdram(&T, va + 16 * k, &in[k]);
        rej_vtx_run(&T, w0, in);
        ran++;
        lit_n += lit && (ld16(a, 0x1ec) & 2);
        odd += nv & 1;
        for (k = 0; k < nv; k++) {
            struct rej_vtx rv;
            rej_ld_vtx(&T, hle_dm16(&T, F2_VTAB + 2 * (v0 + k)), &rv);
            marked += rv.sy == REJ_MARK_Y;
        }
#ifdef REJ_VTX_ORACLE
        {
            struct rctx x;
            unsigned diff = 0, d;
            oracle_regs(&x, a);
            x.r[25] = w0;
            x.r[24] = w1;
            x.r[27] = (uint32_t)-160;
            x.r[26] = 0x100000;
            x.r[23] = 0x9f8 + 8 * (i % 8);
            if (sigsetjmp(jb, 1)) {
                printf("vtx %u: timeout\n", i);
                continue;
            }
            ualarm(200000, 0);
            rej_vtx_gen(&x, 0x4a4);
            ualarm(0, 0);
            for (d = 0; d < 0x1000; d++)
                if (d < 0x950 && !dead(d) && ((uint8_t *)a)[d ^ 3] != ((uint8_t *)b)[d ^ 3])
                    diff++;
            if (x.pc != 0x194 || diff) {
                if (fails++ < 6) {
                    printf("vtx %u: n=%u first=%u geom=%08x lit=%u pc %x, %u DMEM bytes differ:", i, nv, v0, hle_dm32(&T, 0x1ec), lit, x.pc, diff);
                    for (d = 0, k = 0; d < 0x9f8 && k < 12; d++)
                        if (d < 0x950 && !dead(d) && ((uint8_t *)a)[d ^ 3] != ((uint8_t *)b)[d ^ 3]) {
                            printf(" %03x:%02x/%02x", d, ((uint8_t *)a)[d ^ 3], ((uint8_t *)b)[d ^ 3]);
                            k++;
                        }
                    printf("\n");
                    if (getenv("REJ_DUMP")) {
                        printf("  input[0..1]:");
                        for (k = 0; k < 32; k++)
                            printf(" %02x", rdram[(va + k) ^ 3]);
                        printf("\n  oracle rec0:");
                        for (k = 0; k < 40; k++)
                            printf(" %02x", ((uint8_t *)a)[(0x410 + 20 * v0 + k) ^ 3]);
                        printf("\n  mine   rec0:");
                        for (k = 0; k < 40; k++)
                            printf(" %02x", ((uint8_t *)b)[(0x410 + 20 * v0 + k) ^ 3]);
                        printf("\n");
                    }
                }
            }
        }
#endif
    }
    printf("test_rej: G_VTX: %u cases run, %u lit, %u odd counts, %u marked vertices\n", ran, lit_n, odd, marked);
    *fails_out += fails;
    return ran;
}

/* ---- triangles ------------------------------------------------------------------------------------------------------------------------------------------------------------ */
static void rand_rec(struct rej_vtx *r, int wild)
{
    unsigned k;
    int range = wild ? 0x7fff : 0x500;
    r->sx = (int16_t)((int)(rnd() % (2 * range + 1)) - range);
    r->sy = (int16_t)(((int)(rnd() % (2 * range + 1)) - range) & ~3);
    if (rnd() % 12 == 0)
        r->sy = REJ_MARK_Y;
    if (rnd() % 20 == 0)
        r->sx = r16();
    if (rnd() % 20 == 0)
        r->sy = (int16_t)(r16() & ~3);
    r->szi = (int16_t)(rnd() % 8 == 0 ? r16() : (int)(rnd() % 0x400) - 0x40);
    r->szf = (uint16_t)rnd();
    r->rwi = (int16_t)(rnd() % 4 == 0 ? r16() : (int)(rnd() % 4));
    r->rwf = (uint16_t)rnd();
    for (k = 0; k < 4; k++)
        r->col[k] = (uint8_t)rnd();
    r->s = r16();
    r->t = r16();
}

static void tri_state(uint32_t *dm)
{
    unsigned k, wild = rnd() % 5 == 0;
    uint32_t g;
    for (k = 0; k < 65; k++) {
        struct rej_vtx r;
        rand_rec(&r, wild);
        if (rnd() % 6 == 0 && k)                    /* near-duplicates: shared positions, so that edges are horizontal / vertical / zero */
            r.sx = (int16_t)(hle_dm16(&(struct hle_task){ .dmem = dm }, 0x410 + 20 * (k - 1) + 8)), r.sy = (int16_t)(hle_dm16(&(struct hle_task){ .dmem = dm }, 0x410 + 20 * (k - 1) + 10));
        if (k < 64)
            rej_st_vtx(&(struct hle_task){ .dmem = dm }, 0x410 + 20 * k, &r);
    }
    g = (rnd() % 2 ? 1u : 0) | (rnd() % 2 ? 4u : 0) | (rnd() % 2 ? 0x200000u : 0) | (rnd() % 4 == 0 ? 0x200u : 0) | (rnd() % 4 == 0 ? 0x400u : 0) | (rnd() % 2 ? 0x10000u : 0) | (rnd() % 2 ? 0x20000u : 0) | (rnd() % 4 == 0 ? 0x800000u : 0);
    st32(dm, 0x1ec, g);
    st8(dm, 0x1e6, (uint8_t)(rnd() % 64));
    st8(dm, 0x1e7, (uint8_t)(rnd() % 2 ? 2 : 0));
}

static unsigned tri_cases(unsigned n, unsigned *fails_out)
{
    unsigned i, fails = 0, ran = 0, drawn = 0, skipped_decl = 0, wraps = 0, k;
    static uint32_t a[1024], b[1024];
    for (i = 0; i < n; i++) {
        unsigned two = rnd() % 4 != 0, quad = two && rnd() % 4 == 0, j;
        uint32_t w0, w1, s7, s7m, cm[16], ci[16];
        uint32_t idx[6];
        const char *why;
        base_dmem(a);
        tri_state(a);
        for (k = 0; k < 6; k++)
            idx[k] = 2 * (rnd() % 64);
        if (rnd() % 8 == 0)
            idx[1] = idx[0];
        if (rnd() % 8 == 0)
            idx[2] = idx[0];
        w0 = (two ? (quad ? 7u : 6u) : 5u) << 24 | idx[0] << 16 | idx[1] << 8 | idx[2];
        w1 = two ? (quad ? idx[0] << 16 | idx[2] << 8 | idx[3] : idx[3] << 16 | idx[4] << 8 | idx[5]) : 0;
        if (rnd() % 16 == 0)
            w0 ^= (rnd() & 0xff) << (8 * (rnd() % 3));             /* (a vertex number out of range: both sides decline or draw as the microcode does) */
        st32(a, 0x950, w0);
        st32(a, 0x954, w1);
        s7 = 0x9f8 + 8 * (rnd() % 164);
        if (rnd() % 6 == 0)
            s7 = 0xf10 - 8 * (rnd() % 20);
        memcpy(b, a, sizeof a);
        f2L = &l_lay;
        T.dmem = b;
        for (j = 0; j < 16; j++)
            creg[j] = 0;
        creg[8] = creg[9] = creg[10] = s7;
        memcpy(ci, creg, sizeof ci);
        s7m = s7;
        why = rej_cmd_tri(&T, &s7m, w0, w1, two);
        memcpy(cm, creg, sizeof cm);
        if (why) {
            skipped_decl++;
            continue;
        }
        ran++;
        drawn += s7m != s7;
        wraps += s7m < s7;
#ifdef REJ_TRI_ORACLE
        {
            struct rctx x;
            unsigned diff = 0, d;
            memcpy(creg, ci, sizeof ci);
            oracle_regs(&x, a);
            x.r[25] = w0;
            x.r[24] = w1;
            x.r[27] = (uint32_t)-160;
            x.r[26] = 0x100000;
            x.r[23] = s7;
            if (sigsetjmp(jb, 1)) {
                printf("tri %u: timeout\n", i);
                continue;
            }
            ualarm(200000, 0);
            rej_tri_gen(&x, two ? 0x750 : 0x74c);
            ualarm(0, 0);
            for (d = 0; d < 0x1000; d++) {
                int in_ring = d >= f2L->ring_start && d < 0xfc0;
                uint32_t lo = s7m < s7 ? f2L->ring_start : s7, hi = s7m;
                if (in_ring ? (d >= lo && d < hi + 0 && ((uint8_t *)a)[d ^ 3] != ((uint8_t *)b)[d ^ 3]) : (d < 0x950 && ((uint8_t *)a)[d ^ 3] != ((uint8_t *)b)[d ^ 3]))
                    diff++;
            }
            if (x.pc != 0x194 || diff || x.r[23] != s7m || creg[8] != cm[8] || creg[9] != cm[9] || creg[10] != cm[10]) {
                if (fails++ < 6) {
                    printf("tri %u: %s w0 %08x w1 %08x geom %08x s7 %x -> %x / %x pc %x DPC %x %x %x / %x %x %x, %u DMEM bytes differ:", i, two ? (quad ? "QUAD" : "TRI2") : "TRI1", w0, w1, hle_dm32(&T, 0x1ec), s7, x.r[23], s7m, x.pc,
                           creg[8], creg[9], creg[10], cm[8], cm[9], cm[10], diff);
                    for (d = 0, k = 0; d < 0xfc0 && k < 12; d++) {
                        int in_ring = d >= f2L->ring_start;
                        uint32_t lo = s7m < s7 ? f2L->ring_start : s7;
                        if ((in_ring ? (d >= lo && d < s7m) : d < 0x950) && ((uint8_t *)a)[d ^ 3] != ((uint8_t *)b)[d ^ 3]) {
                            printf(" %03x:%02x/%02x", d, ((uint8_t *)a)[d ^ 3], ((uint8_t *)b)[d ^ 3]);
                            k++;
                        }
                    }
                    printf("\n");
                    if (getenv("REJ_DUMP")) {
                        unsigned q;
                        for (q = 0; q < (two ? 6u : 3u); q++) {
                            unsigned idxv = q < 3 ? (w0 >> (16 - 8 * q)) & 0xff : (w1 >> (16 - 8 * (q - 3))) & 0xff;
                            struct rej_vtx rv;
                            rej_ld_vtx(&T, hle_dm16(&T, 0x380 + idxv), &rv);
                            printf("  v%u(idx %u): col %02x%02x%02x%02x st %d %d xy %d %d z %d.%04x rw %d.%04x\n", q, idxv / 2, rv.col[0], rv.col[1], rv.col[2], rv.col[3], rv.s, rv.t, rv.sx, rv.sy, rv.szi, rv.szf, rv.rwi, rv.rwf);
                        }
                        printf("  tex %02x tile %02x T %04x\n", hle_dm8(&T, 0x1e7), hle_dm8(&T, 0x1e6), hle_dm16(&T, 0x402 + hle_dm8(&T, 0x1ee)));
                        printf("  oracle out:");
                        for (q = 0; q < 160 && s7 + q < 0xfc0; q++)
                            printf(" %02x", ((uint8_t *)a)[(s7 + q) ^ 3]);
                        printf("\n  mine   out:");
                        for (q = 0; q < 160 && s7 + q < 0xfc0; q++)
                            printf(" %02x", ((uint8_t *)b)[(s7 + q) ^ 3]);
                        printf("\n");
                    }
                }
            }
        }
#endif
    }
    printf("test_rej: G_TRI: %u cases run (%u drew, %u wrapped the ring), %u declined\n", ran, drawn, wraps, skipped_decl);
    *fails_out += fails;
    return ran;
}

int main(int argc, char **argv)
{
    unsigned n = argc > 1 ? (unsigned)atoi(argv[1]) : 2000, fails = 0;
    const char *dp = getenv("REJ_DATA");
    FILE *fp = dp ? fopen(dp, "rb") : NULL;
    struct sigaction sa;
    if (!fp || fread(data_seg, 1, sizeof data_seg, fp) != sizeof data_seg) {
        fprintf(stderr, "REJ_DATA=<data segment dump of a Rej text> needed\n");
        return 2;
    }
    fclose(fp);
    if (argc > 2)
        rs += (uint64_t)strtoull(argv[2], NULL, 0) * 0x2545f4914f6cdd1dull;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);
    if (!getenv("REJ_ONLY") || !strcmp(getenv("REJ_ONLY"), "vtx"))
        vtx_cases(n, &fails);
    if (!getenv("REJ_ONLY") || !strcmp(getenv("REJ_ONLY"), "tri"))
        tri_cases(n, &fails);
    printf("test_rej: %u failures; oracles:%s%s\n", fails,
#ifdef REJ_VTX_ORACLE
           " vtx",
#else
           "",
#endif
#ifdef REJ_TRI_ORACLE
           " tri"
#else
           ""
#endif
    );
    return fails != 0;
}
