/*
 * test_rej1.c: the 1.x Rej closed forms (src/hle/rej1_fx.c, rej_fx.c with REJ_X1, rej1_dmem.h) on random DMEM images. With -DREJ1_VTX_ORACLE=\"file\" / -DREJ1_TRI_ORACLE=\"file\" (translations of the microcode's
 * G_VTX / G_TRI1 / G_TRI2 handlers that tools/rsp2c/rsp2c.py makes from an IMEM image of the text with the lighting overlay resident, tools/hle/gen_rej1_oracles.sh) every command must leave the same DMEM
 * as the microcode (the command window, the operand buffer, the output buffers and the relight scratch excepted) and send the same bytes to the RDP. Without an oracle the closed forms only have to finish.
 *
 * Local test only. Build and run: tools/hle/build_test_rej1.sh
 */
#include <inttypes.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hle.h"
#if defined(REJ1_VTX_ORACLE) || defined(REJ1_TRI_ORACLE)
#define VU_FORCE_SCALAR 1
#include "ref_vu.h"
struct rctx {
    struct vu u;
    uint32_t r[32];
    unsigned pc;
    struct hle_task *t;
};
#endif
#include "rej1_dmem.h"

#ifndef REJ1_VTX_ENTRY                                      /* the pcs of the build under test: G_VTX, G_TRI2, G_TRI1, the command loop's head */
#define REJ1_VTX_ENTRY 0x3ac
#define REJ1_TRI2_ENTRY 0xa70
#define REJ1_TRI1_ENTRY 0x1c0
#define REJ1_HEAD 0x09c
#endif
#define LAY rej1_lay_123
#ifndef RJ
#define RJ rej1_123                                         /* (rej1_121: F3DLX.Rej 1.21 and the earlier 1.23; rej1_lp121 / rej1_lp123: F3DLP.Rej) */
#endif
static const struct f3dex_build FX = { 0x7070, 0, 1, &LAY };
#define TNSLOTS (RJ.lp ? REJ1_NSLOTS_LP : REJ1_NSLOTS)             /* (the slots the cases use: the test's own count, not rej1_nslots, so that a wrong count in the closed forms shows as declines) */

/* the registers the code reaches through MTC0 / MFC0 */
static uint32_t creg[16];
static uint32_t *const crp[16] = { &creg[0], &creg[1], &creg[2], &creg[3], &creg[4], &creg[5], &creg[6], &creg[7], &creg[8], &creg[9], &creg[10], &creg[11], &creg[12], &creg[13], &creg[14], &creg[15] };
int hle_mtc0(struct hle_task *t, unsigned rd, uint32_t value)
{
    (void)t;
    creg[rd & 15] = value;
    if ((rd & 15) == 9 || (rd & 15) == 8)       /* the RDP takes every list at once */
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
    static const int16_t e[] = { 0, 1, 2, 0x7fff, -0x8000, -0x7fff, -1, -2, 0x4000, -0x4000, 0xff, -0x100, 0x100, 0x7f00, -0x7f00, 0x7ffe, 0x7ffc };
    unsigned m = rnd() % 8;
    if (m == 0)
        return e[rnd() % (sizeof(e) / sizeof(e[0]))];
    if (m < 4)
        return (int16_t)((int)(rnd() % 512) - 256);
    return (int16_t)rnd();
}

#define RDRAM_SIZE (8u << 20)
static uint8_t rdram[RDRAM_SIZE];
static struct hle_task T = { .rdram = rdram, .cr = crp };

static uint8_t data_seg[0x800];
static void st16(uint32_t *dm, unsigned a, uint16_t v) { xs16(dm, a, v); }
static void st8(uint32_t *dm, unsigned a, uint8_t v) { xs8(dm, a, v); }
static void st32(uint32_t *dm, unsigned a, uint32_t v) { xs32(dm, a, v); }

/* the DMEM of a task at a command: the data segment, the rest random (the vertex slots, the buffers) */
static void base_dmem(uint32_t *dm)
{
    unsigned k;
    for (k = 0; k < 0x1000; k++)
        st8(dm, k, k < 0x800 ? data_seg[k] : (uint8_t)rnd());
    for (k = RJ.vbuf; k < RJ.vbuf + REJ1_VSTRIDE * REJ1_NSLOTS; k++)      /* (F3DLP.Rej: the 64 records and what lies behind them) */
        st8(dm, k, (uint8_t)rnd());
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
    const struct f3dex_layout *L = &LAY;
    unsigned k, lit = rnd() % 2, nrec = rnd() % 5 == 0 ? 0 : 1 + rnd() % 7, mode = rnd() % 8;
    uint32_t g;
    rand_mtx(dm, L->mv, 1);
    rand_mtx(dm, L->mvp, 1);
    if (rnd() % 2) {                                 /* a projection that keeps most of the vertices inside the guard band */
        unsigned sc = 0x0100u << (rnd() % 6), r, c;
        for (r = 0; r < 4; r++)
            for (c = 0; c < 4; c++) {
                st16(dm, L->mvp + 2 * (4 * r + c), 0);
                st16(dm, L->mvp + 0x20 + 2 * (4 * r + c), 0);
            }
        for (r = 0; r < 3; r++)
            st16(dm, L->mvp + 0x20 + 2 * (4 * r + r), (uint16_t)(sc + (rnd() % 4 == 0 ? rnd() % 0x80 : 0)));
        st16(dm, L->mvp + 2 * 15, (uint16_t)(1 + rnd() % 0x40));
        st16(dm, L->mvp + 0x20 + 2 * 15, (uint16_t)rnd());
        st16(dm, L->mvp + 2 * 12, (uint16_t)((int)(rnd() % 9) - 4));
        st16(dm, L->mvp + 2 * 13, (uint16_t)((int)(rnd() % 9) - 4));
        st16(dm, L->mvp + 2 * 14, (uint16_t)((int)(rnd() % 9) - 4));
        if (rnd() % 3 == 0)                          /* w that can be 0 or negative */
            st16(dm, L->mvp + 2 * 15, (uint16_t)((int)(rnd() % 5) - 2));
    }
    if (rnd() % 3 == 0) {                            /* most vertices inside the guard band: x y z a small fraction of the input, w large, a ratio of 2 */
        unsigned r, c;
        for (r = 0; r < 4; r++)
            for (c = 0; c < 4; c++) {
                st16(dm, L->mvp + 2 * (4 * r + c), 0);
                st16(dm, L->mvp + 0x20 + 2 * (4 * r + c), (uint16_t)(r == c && r < 3 ? 0x400 + rnd() % 0x800 : rnd() % 4 == 0 ? rnd() % 0x100 : 0));
            }
        st16(dm, L->mvp + 2 * 15, (uint16_t)(0x100 + rnd() % 0x300));
        st16(dm, L->mvp + 2 * 14, (uint16_t)(rnd() % 0x80));
        mode = 2 + rnd() % 2;
        st16(dm, L->clip_k, 2);
        lit = 2;                                     /* (marker: keep the ratio) */
    }
    if (rnd() % 6 == 0) {                            /* the extreme corner: a tiny w and viewport scales at the ends of the range */
        unsigned r, c;
        for (r = 0; r < 4; r++)
            for (c = 0; c < 4; c++) {
                st16(dm, L->mvp + 2 * (4 * r + c), (uint16_t)(r == c && r < 3 ? 1 : 0));
                st16(dm, L->mvp + 0x20 + 2 * (4 * r + c), 0);
            }
        st16(dm, L->mvp + 2 * 15, 0);
        st16(dm, L->mvp + 0x20 + 2 * 15, (uint16_t)(rnd() % 3 == 0 ? 0 : rnd() % 4 == 0 ? 0x8000 : 1 + rnd() % 0x40));
        mode = 0;
    }
    st16(dm, L->persp, (uint16_t)(rnd() % 4 == 0 ? r16() : rnd() % 2 ? 0xffff : 0xe000 + rnd() % 0x2000));
    for (k = 0; k < 4; k++) {
        st16(dm, L->viewport + 2 * k, (uint16_t)(mode < 2 ? r16() : k == 0 ? 640 : k == 1 ? 480 : k == 2 ? 0x1ff : 0x100));
        st16(dm, L->viewport + 8 + 2 * k, (uint16_t)(mode < 2 ? r16() : k == 0 ? 640 : k == 1 ? 480 : k == 2 ? 0x1ff : 0));
    }
    if (lit == 2)
        lit = rnd() % 2;
    else
        st16(dm, L->clip_k, (uint16_t)(rnd() % 4 == 0 ? r16() : 1 + rnd() % 6));
    st16(dm, L->texture + 4, (uint16_t)(rnd() % 3 == 0 ? r16() : 0xffff - rnd() % 4));
    st16(dm, L->texture + 6, (uint16_t)(rnd() % 3 == 0 ? r16() : 0x8000));
    st16(dm, L->fog, (uint16_t)(rnd() % 3 == 0 ? r16() : 0x100 + rnd() % 0x800));
    st16(dm, L->fog + 2, (uint16_t)(rnd() % 3 == 0 ? r16() : (int)(rnd() % 0x200) - 0x100));
    st16(dm, L->fog + 4, (uint16_t)(rnd() % 4 == 0 ? r16() : 0xff));
    if (rnd() % 4 == 0)
        for (k = 0; k < 3; k++)
            st16(dm, L->tg + 2 * k, (uint16_t)r16());
    g = (rnd() % 2 ? F3DEX_G_ZBUFFER : 0) | (rnd() % 2 ? F3DEX_G_SHADE : 0) | (rnd() % 2 ? F3DEX_G_SMOOTH : 0) | (rnd() % 2 ? F3DEX_G_FOG : 0) | (lit ? F3DEX_G_LIGHTING : 0) |
        (lit && rnd() % 2 ? F3DEX_G_TEXGEN : 0) | (rnd() % 2 ? F3DEX_G_TEXGEN_LINEAR : 0) | (rnd() % 4 == 0 ? F3DEX_G_CLIPPING : 0) | (rnd() % 2 ? F3DEX_G_TEXTURE : 0);
    if ((g & F3DEX_G_TEXGEN) && nrec == 0)
        nrec = 1;
    st32(dm, L->geom, g);
    st32(dm, L->lights, (nrec * 32) | (rnd() % 4 == 0 ? 0x80000000u : 0));
    for (k = 0; k < 8; k++)                          /* the texgen weights, and the colour, raw and transformed direction of each record (bytes +8 .. +15 of a record hold the data segment's constants: 0x208) */
        st8(dm, L->light + k, (uint8_t)rnd());
    for (k = 0; k < 32 * (nrec + 2); k++)
        if ((k & 31) >= 0x10)
            st8(dm, L->light + k, (uint8_t)rnd());
    for (k = 0; k < 8; k++)                          /* (record nrec + 1's transformed direction: what the loop reads past the ambient record) */
        st8(dm, L->light + 32 * (nrec + 2) + k, (uint8_t)rnd());
    for (k = 0; k <= nrec + 1; k++) {                /* the colour bytes and transformed directions as the game has them: the two halves alike */
        unsigned i;
        for (i = 0; i < 4; i++) {
            st8(dm, L->light + 0x14 + 32 * k + i, ((const uint8_t *)dm)[(L->light + 0x10 + 32 * k + i) ^ 3]);
            st8(dm, L->light + 0x24 + 32 * k + i, ((const uint8_t *)dm)[(L->light + 0x20 + 32 * k + i) ^ 3]);
        }
    }
    for (k = 0; k < 16; k++)
        st32(dm, L->segs + 4 * k, rnd() % 4 ? 0 : (rnd() & 0xff000));
    if (getenv("REJ1_IDENT")) {                     /* (debugging the relight: identity model-view, light 0 along x) */
        for (k = 0; k < 16; k++) {
            st16(dm, L->mv + 2 * k, k % 5 == 0 ? 1 : 0);
            st16(dm, L->mv + 0x20 + 2 * k, 0);
        }
        st8(dm, L->light + 0x18, 127); st8(dm, L->light + 0x19, 0); st8(dm, L->light + 0x1a, 0);
    }
    *lit_out = lit;
}

static void rand_vin(uint32_t *dm, unsigned a, int wild)
{
    unsigned k;
    int range = wild ? 0x7fff : 0x1800;
    for (k = 0; k < 3; k++) {
        int16_t v = (int16_t)((int)(rnd() % (2 * range + 1)) - range);
        if (rnd() % 16 == 0)
            v = r16();
        st16(dm, a + 2 * k, (uint16_t)v);
    }
    for (k = 6; k < 16; k++)
        st8(dm, a + k, (uint8_t)rnd());
}

/* the memories of the oracle: DMA reads from RDRAM, DMA writes captured */
static uint8_t cap[0x400];
static unsigned cap_n;
static uint32_t cap_dram;
#if defined(REJ1_VTX_ORACLE) || defined(REJ1_TRI_ORACLE)
static int rj_dma(struct rctx *x, uint32_t *r)
{
    uint32_t *dm = x->u.dmem;
    unsigned mem = r[20] & 0xfff, len = (r[18] & 0xfff) + 1, i;
    if ((int32_t)r[17] > 0) {                       /* a write: the RDP chunk */
        len = (len + 7) & ~7u;
        cap_dram = r[19];
        for (i = 0; i < len && cap_n < sizeof cap; i++)
            cap[cap_n++] = ((uint8_t *)dm)[((mem & ~7u) + i) ^ 3];
        return 0;
    }
    for (i = 0; i < ((len + 7) & ~7u) && (mem & ~7u) + i < 0x1000; i++)
        ((uint8_t *)dm)[((mem & ~7u) + i) ^ 3] = rdram[(((r[19] & 0xffffff) & ~7u) + i) & (RDRAM_SIZE - 1)];
    return 0;
}
static int rj_dma0(struct rctx *x, uint32_t *r) { r[17] = 0; return rj_dma(x, r); }
static int rj_wait(struct rctx *x, uint32_t *r) { (void)x; r[17] = 0; return 0; }

static sigjmp_buf jb;
static void on_alarm(int s) { (void)s; siglongjmp(jb, 1); }

#ifdef REJ1_VTX_ORACLE
#include REJ1_VTX_ORACLE
#endif
#ifdef REJ1_TRI_ORACLE
#include REJ1_TRI_ORACLE
#endif

static void oracle_regs(struct rctx *x, uint32_t *dm, unsigned cmd_at, uint32_t w0, uint32_t w1)
{
    memset(x, 0, sizeof *x);
    x->u.dmem = dm;
    x->t = &T;
    LQV(&x->u, 31, 0, 0x30); LQV(&x->u, 30, 0, 0x40);
    x->r[29] = 0x110;
    x->r[22] = LAY.operand;
    x->r[25] = w0;
    x->r[24] = w1;
    x->r[27] = cmd_at + 8;
    x->r[28] = LAY.operand;                         /* (the window's end) */
    x->r[1] = (w0 >> 16) & 0xff;
    x->r[6] = x->r[1] & 0xf;
    x->r[23] = xd16(dm, RJ.bufsel);
}
#endif

/* DMEM that is not compared: the command window and operand buffer, the output buffers, the relight scratch, the fourth byte of each transformed light direction (a leftover, as in F3DEX 1.x) */
static int dead(unsigned d)
{
    const struct f3dex_layout *L = &LAY;
    if (d >= L->window && d < 0xf40)
        return 1;
    if (d >= 0xf70 && d < 0xfc0)
        return 1;
    if (d >= 0x106 && d < 0x108)                    /* (the return address G_VTX keeps) */
        return 1;
    if (d >= L->light + 0x20 && d < L->light + 0x20 + 32 * (F3DEX_MAX_REC + 1) && ((d - L->light - 0x20) % 32 == 3 || (d - L->light - 0x20) % 32 == 7))
        return 1;
    return 0;
}

static unsigned diffs(const uint32_t *a, const uint32_t *b, unsigned *first, unsigned nfirst, unsigned *nf)
{
    unsigned d, n = 0;
    *nf = 0;
    for (d = 0; d < 0x1000; d++)
        if (!dead(d) && xd8(a, d) != xd8(b, d)) {
            if (*nf < nfirst)
                first[(*nf)++] = d;
            n++;
        }
    return n;
}

static unsigned vtx_cases(unsigned n, unsigned *fails_out)
{
    unsigned i, fails = 0, ran = 0, nmarked = 0, lit_n = 0, decl = 0, nvm = 0, nvt = 0;
    static uint32_t a[1024], b[1024];
    for (i = 0; i < n; i++) {
        unsigned lit, nv = rnd() % 4 == 0 ? 1 + rnd() % 32 : 1 + rnd() % 8, v0, k, cmd_at = LAY.window + 8 * (rnd() % 40), wild = rnd() % 4 == 0;
        uint32_t w0, w1 = 0x01000000u + 16 * (rnd() % 0x1000), l0;
        const char *why;
        int marked = 0;
        base_dmem(a);
        vtx_state(a, &lit);
        l0 = xd32(a, LAY.lights);
        v0 = rnd() % (TNSLOTS + 1 - nv);
        w0 = 0x04000000u | (v0 * 2) << 16 | (nv == 1 && rnd() % 8 == 0 ? 0 : nv) << 10 | (16 * nv - 1);
        st32(a, cmd_at, w0);
        st32(a, cmd_at + 4, w1);
        for (k = 0; k < 32; k++)
            rand_vin(a, LAY.operand + 16 * k, wild);
        memcpy(b, a, sizeof a);
        why = rej1_vtx_dmem(b, &FX, &RJ, w0, b, LAY.operand, 0, &marked);
        if (!why)
            for (k = 0; k < (nv ? nv : 1); k++)
                nvm += xd16(b, RJ.vbuf + rej1_rec(&RJ) * (v0 + k)) == REJ1_MARK_X, nvt++;
        if (why) {
            decl++;
            continue;
        }
        ran++;
        lit_n += lit;
        nmarked += marked;
#ifdef REJ1_VTX_ORACLE
        {
            struct rctx x;
            unsigned diff, first[12], nf, q;
            oracle_regs(&x, a, cmd_at, w0, w1);
            if (sigsetjmp(jb, 1)) {
                printf("vtx %u: timeout\n", i);
                fails++;
                continue;
            }
            ualarm(200000, 0);
            rej1_vtx_gen(&x, REJ1_VTX_ENTRY);
            ualarm(0, 0);
            diff = diffs(a, b, first, 12, &nf);
            if (x.pc != REJ1_HEAD || diff) {
                if (fails++ < 8) {
                    printf("vtx %u: n=%u first=%u geom=%08x lights %08x pc %x, %u DMEM bytes differ:", i, nv, v0, xd32(a, LAY.geom), l0, x.pc, diff);
                    for (q = 0; q < nf; q++)
                        printf(" %03x:%02x/%02x", first[q], xd8(a, first[q]), xd8(b, first[q]));
                    printf("\n");
                }
            }
        }
#endif
    }
    printf("test_rej1: G_VTX: %u cases run (%u lit, %u with a marked vertex; %u of %u vertices marked), %u declined\n", ran, lit_n, nmarked, nvm, nvt, decl);
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
        r->sx = REJ1_MARK_X;
    if (rnd() % 20 == 0)
        r->sx = r16();
    if (rnd() % 20 == 0)
        r->sy = (int16_t)(r16() & ~3);
    if (rnd() % 4 == 0)                                     /* records G_VTX does not leave: y low bits (a stale slot) */
        r->sy |= (int16_t)(rnd() & 3);
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
    struct rej_vtx prev;
    for (k = 0; k < TNSLOTS; k++) {
        struct rej_vtx r;
        rand_rec(&r, wild);
        if (k && rnd() % 6 == 0) {                          /* near-duplicates: horizontal / vertical / zero edges */
            r.sx = prev.sx;
            if (rnd() % 2)
                r.sy = prev.sy;
        }
        rej1_st_vtx(dm, RJ.vbuf + rej1_rec(&RJ) * k, &r, RJ.lp);
        prev = r;
    }
    g = (rnd() % 2 ? F3DEX_G_ZBUFFER : 0) | (rnd() % 2 ? F3DEX_G_SHADE : 0) | (rnd() % 2 ? F3DEX_G_TEXTURE : 0) | (rnd() % 2 ? F3DEX_G_SMOOTH : 0) | (rnd() % 3 == 0 ? F3DEX_G_CULL_BACK : 0) |
        (rnd() % 3 == 0 ? F3DEX_G_CULL_FRONT : 0) | (rnd() % 2 ? F3DEX_G_FOG : 0) | (rnd() % 2 ? F3DEX_G_LIGHTING : 0);
    st32(dm, LAY.geom, g);
    st8(dm, LAY.texture + 2, (uint8_t)rnd());
    st16(dm, RJ.bufsel, rnd() % 2 ? 0xc80 : 0xc80 ^ RJ.buftog);
}

static unsigned tri_cases(unsigned n, unsigned *fails_out)
{
    unsigned i, fails = 0, ran = 0, drawn = 0, decl = 0, k;
    static uint32_t a[1024], b[1024];
    for (i = 0; i < n; i++) {
        unsigned two = rnd() % 4 != 0, cmd_at = LAY.window + 8 * (rnd() % 40), j, len = 0, idx[6];
        uint32_t w0, w1, chunk[0x160 / 4];
        const char *why = NULL;
        base_dmem(a);
        tri_state(a);
        for (k = 0; k < 6; k++)
            idx[k] = 2 * (rnd() % TNSLOTS);
        if (rnd() % 8 == 0)
            idx[1] = idx[0];
        if (rnd() % 8 == 0)
            idx[2] = idx[0];
        if (two) {
            w0 = 0xb1000000u | idx[0] << 16 | idx[1] << 8 | idx[2];
            w1 = (rnd() & 0xff) << 24 | idx[3] << 16 | idx[4] << 8 | idx[5];
        } else {
            w0 = 0xbf000000u | (rnd() & 0xffffff);
            w1 = (rnd() & 0xff) << 24 | idx[0] << 16 | idx[1] << 8 | idx[2];
        }
        if (rnd() % 24 == 0)
            w1 ^= (rnd() & 0xff) << (8 * (rnd() % 3));       /* (an odd or out-of-range vertex number: declined) */
        st32(a, cmd_at, w0);
        st32(a, cmd_at + 4, w1);
        st32(a, LAY.rdp_wp, 0x200000);
        st32(a, LAY.fifo, 0x100000);
        st32(a, LAY.fifo + 4, 0x300000);
        memcpy(b, a, sizeof a);
        /* the closed forms: w1's triangle, then (TRI2) w0's, one chunk */
        uint16_t frac[16] = { 0 };
        for (j = 0; j < (two ? 2u : 1u) && !why; j++) {
            uint32_t ww = j == 0 ? w1 : w0, cmd[0xb0 / 4];
            unsigned l = 0;
            int r = rej1_tri_dmem(b, &FX, &RJ, (ww >> 16) & 0xff, (ww >> 8) & 0xff, ww & 0xff, ((RJ.flat1 ? w1 : ww) >> 16) & 0xff, frac, cmd, &l, &why);
            if (r == FX2_TRI_DRAW) {
                memcpy((uint8_t *)chunk + len, cmd, l);
                len += l;
            }
        }
        if (why) {
            decl++;
            continue;
        }
        ran++;
        drawn += len != 0;
        if (len) {                                      /* the flush: the chunk from the selected buffer, the pointer, the other buffer */
            unsigned buf = xd16(b, RJ.bufsel);
            for (k = 0; k < len / 4; k++)
                xs32(b, buf + 4 * k, chunk[k]);
            xs32(b, LAY.rdp_wp, xd32(b, LAY.rdp_wp) + len);
            xs16(b, RJ.bufsel, (uint16_t)(buf ^ RJ.buftog));
        }
#ifdef REJ1_TRI_ORACLE
        {
            struct rctx x;
            unsigned diff, first[12], nf, q, bad_out;
            for (k = 0; k < 16; k++)
                creg[k] = 0;
            creg[8] = creg[9] = creg[10] = 0x200000;
            oracle_regs(&x, a, cmd_at, w0, w1);
            cap_n = 0;
            if (sigsetjmp(jb, 1)) {
                printf("tri %u: timeout\n", i);
                fails++;
                continue;
            }
            ualarm(200000, 0);
            rej1_tri_gen(&x, two ? REJ1_TRI2_ENTRY : REJ1_TRI1_ENTRY);
            ualarm(0, 0);
            diff = diffs(a, b, first, 12, &nf);
            bad_out = cap_n != len || memcmp(cap, (const uint8_t *)chunk, 0) != 0;
            for (k = 0; k < len && !bad_out; k++)
                if (cap[k] != ((const uint8_t *)chunk)[(k) ^ 3])
                    bad_out = 1;
            if (x.pc != REJ1_HEAD || diff || bad_out) {
                if (fails++ < 8) {
                    printf("tri %u: %s w0 %08x w1 %08x geom %08x pc %x, out %u / %u bytes%s, %u DMEM bytes differ:", i, two ? "TRI2" : "TRI1", w0, w1, xd32(a, LAY.geom), x.pc, cap_n, len,
                           bad_out ? " DIFFER" : "", diff);
                    for (q = 0; q < nf; q++)
                        printf(" %03x:%02x/%02x", first[q], xd8(a, first[q]), xd8(b, first[q]));
                    printf("\n");
                    if (getenv("REJ1_DUMP") && bad_out) {
                        printf("  oracle:");
                        for (q = 0; q < cap_n; q++)
                            printf("%s%02x", q % 8 ? "" : " ", cap[q]);
                        printf("\n  mine:  ");
                        for (q = 0; q < len; q++)
                            printf("%s%02x", q % 8 ? "" : " ", ((const uint8_t *)chunk)[q ^ 3]);
                        printf("\n");
                        for (q = 0; q < (two ? 6u : 3u); q++) {
                            uint32_t ww = q < 3 ? w1 : w0;
                            unsigned bb = (ww >> (16 - 8 * (q % 3))) & 0xff;
                            struct rej_vtx rv;
                            rej1_ld_vtx(a, RJ.vbuf + rej1_rec(&RJ) / 2 * bb, &rv, RJ.lp);
                            printf("  v%u(%u): col %02x%02x%02x%02x st %d %d xy %d %d z %d.%04x rw %d.%04x\n", q, bb / 2, rv.col[0], rv.col[1], rv.col[2], rv.col[3], rv.s, rv.t, rv.sx, rv.sy, rv.szi, rv.szf,
                                   rv.rwi, rv.rwf);
                        }
                    }
                }
            }
        }
#endif
    }
    printf("test_rej1: G_TRI: %u cases run (%u drew), %u declined\n", ran, drawn, decl);
    *fails_out += fails;
    return ran;
}

int main(int argc, char **argv)
{
    unsigned n = argc > 1 ? (unsigned)atoi(argv[1]) : 2000, fails = 0;
    const char *dp = getenv("REJ_DATA");
    FILE *fp = dp ? fopen(dp, "rb") : NULL;
    if (!fp || fread(data_seg, 1, sizeof data_seg, fp) != sizeof data_seg) {
        fprintf(stderr, "REJ_DATA=<data segment dump of the text> needed\n");
        return 2;
    }
    fclose(fp);
    if (argc > 2)
        rs += (uint64_t)strtoull(argv[2], NULL, 0) * 0x2545f4914f6cdd1dull;
#if defined(REJ1_VTX_ORACLE) || defined(REJ1_TRI_ORACLE)
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_alarm;
        sigaction(SIGALRM, &sa, NULL);
    }
#endif
    if (!getenv("REJ_ONLY") || !strcmp(getenv("REJ_ONLY"), "vtx"))
        vtx_cases(n, &fails);
    if (!getenv("REJ_ONLY") || !strcmp(getenv("REJ_ONLY"), "tri"))
        tri_cases(n, &fails);
    printf("test_rej1: %u failures; oracles:%s%s\n", fails,
#ifdef REJ1_VTX_ORACLE
           " vtx",
#else
           "",
#endif
#ifdef REJ1_TRI_ORACLE
           " tri"
#else
           ""
#endif
    );
    return fails != 0;
}
