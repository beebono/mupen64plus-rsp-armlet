/*
 * test_s2dex2.c: the S2DEX 2.x closed forms (src/hle/s2dex2_rect.c, s2dex2_bg.c) on random DMEM images, against the microcode. With -DS2RECT_ORACLE=\"file\" (the translation of overlay A that
 * tools/rsp2c/rsp2c.py makes, see docs/testing.md, "Regenerating the microcode oracles") and / or -DS2RECT_VU (the instruction-level kernel the rectangle closed form replaced, tools/hle/oracle/s2dex2_rect_vu.c, linked in)
 * every sprite the closed form takes (it declines what it does not cover: not counted as a failure, but reported) must give the same DMEM, output pointer and outcome (drawn / outside the
 * scissor); likewise -DS2BG_ORACLE for the background routine. Without an oracle the kernels only have to finish without harm.
 *
 * The inputs are random bytes with the structures biased towards realistic values (the render-mode tables of the data segment or random ones, scales that are not 1.0, flips, the COPY
 * cycle bit, scissors that cut or reject the sprite, sizes up to the edge of the 15-bit width) so that the paths are not all "clipped" or "unsupported". A case that does not finish
 * in a second (the background routine loops on its inputs, and random inputs can ask for 2^16 iterations) is skipped on both sides.
 *
 * Local test only. Build and run: tools/hle/build_test_s2dex2.sh [-DARMLET_HLE_MUTATE=n for the mutation check of the rectangle closed form]
 */
#include <inttypes.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hle.h"
#include "s2dex2_fx.h"

#if defined(S2BG_ORACLE) || defined(S2RECT_ORACLE) || defined(S2BGC_ORACLE)
#define VU_FORCE_SCALAR 1
#include "ref_vu.h"
struct rctx {
    struct vu u;
    uint32_t r[32];
    unsigned pc;
    void *t;
    void *ctx;
    int (*seg)(void *ctx, uint32_t *r);
    int (*flush)(void *ctx, uint32_t *r);
};
#define MUT(n, ok, bad) (ok)
#endif
#ifdef S2BG_ORACLE
#include S2BG_ORACLE
#endif
#ifdef S2RECT_ORACLE
#include S2RECT_ORACLE
#endif
#ifdef S2BGC_ORACLE
#ifndef S2BGC_ENTRY                                         /* Kirby 64's S2DEX: the routine after its DMA wait and the loop head it ends at; Worms' (and the other 2.08 builds') is 8 bytes further: S2BGC_ENTRY=0x22c S2BGC_END=0x1ac */
#define S2BGC_ENTRY 0x224
#define S2BGC_END 0x1a4
#endif
static int bgc_seg(struct rctx *x, uint32_t *r) { (void)x; r[24] = (r[24] & 0xffffff) + 0x1000; return 0; }
static int bgc_flush(struct rctx *x, uint32_t *r) { (void)x; if ((int32_t)(r[23] - r[22]) > 0) { r[22] ^= 0xc00; r[23] = r[22] - 800; } return 0; }
#include S2BGC_ORACLE
#endif
#ifdef S2RECT_VU
int s2ref_obj_rect(uint32_t *dm, unsigned *s7p);
#endif

/* the kernels call the framework only on paths the tests do not take (a DMA, a status poll, a break) */
int hle_mtc0(struct hle_task *t, unsigned rd, uint32_t value) { (void)t; (void)rd; (void)value; return 0; }
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
static uint16_t r16(void)
{
    static const uint16_t e[] = { 0, 1, 2, 0x7fff, 0x8000, 0x8001, 0xffff, 0xfffe, 0x4000, 0xc000, 0x00ff, 0xff00, 0x0100, 0xfffc, 0x0400, 0x0800, 0x1000 };
    unsigned m = rnd() % 8;
    if (m == 0)
        return e[rnd() % (sizeof(e) / sizeof(e[0]))];
    if (m < 4)
        return (uint16_t)(rnd() % 1024);
    return (uint16_t)rnd();
}
static unsigned dm16(const uint32_t *dm, unsigned a) { return ((const uint8_t *)dm)[(a & 0xfff) ^ 3] << 8 | ((const uint8_t *)dm)[((a + 1) & 0xfff) ^ 3]; }
static void st16(uint32_t *dm, unsigned a, uint16_t v) { ((uint16_t *)dm)[(a & 0xfff) >> 1 ^ 1] = v; }
static void st8(uint32_t *dm, unsigned a, uint8_t v) { ((uint8_t *)dm)[(a & 0xfff) ^ 3] = v; }
static void st32(uint32_t *dm, unsigned a, uint32_t v) { dm[(a & 0xfff) >> 2] = v; }

/* a DMEM image with the microcode's constants (what the data segment holds: the vector constants, the render-mode tables) replaced by random values of the right kind */
static void rand_dmem(uint32_t *dm)
{
    unsigned k;
    for (k = 0; k < 1024; k++)
        dm[k] = rnd();
    for (k = 0; k < 8; k++)
        st16(dm, 0x180 + 2 * k, r16());
    st16(dm, 0x244, rnd() & 1 ? 0x248 : 0x250);
    st16(dm, 0x246, rnd() & 1 ? 0x258 : 0x260);
    st32(dm, 0x1f8, rnd());
    st32(dm, 0x1fc, rnd());
    st8(dm, 0x2ae, (uint8_t)(rnd() % 4 ? 0x81 : rnd()));
    st8(dm, 0x2af, (uint8_t)(rnd() % 4 == 0 ? 2 : 0));
    st16(dm, 0x204, 0);
    st16(dm, 0x206, (uint16_t)(rnd() % 1280 + 4));
    st16(dm, 0x208, (uint16_t)(rnd() % 8));
    st16(dm, 0x20a, (uint16_t)(rnd() % 960 + 4));
}

/* the data segment's render-mode tables (0x248 .. 0x267: position mask, y fraction mask, s bias, flip signs, tile attributes) and render-mode words (0x214: 8, 0x234: 4) */
static const uint16_t tab_real[16] = { 0xfffc, 0, 0, 1, 0xffff, 3, 0xfff0, 0, 1, 0xffff, 8, 0x200, 0xffff, 1, 7, 0xc1f0 };
static const uint32_t w214_real[8] = { 0, 0x00100020, 0x00200040, 0x00300060, 0x0000fff4, 0x00100014, 0x00200034, 0x00300054 };
static const uint32_t w234_real[4] = { 0x0001fffe, 0xfffefffe, 0x00010000, 0 };
static const uint32_t bg_consts[16] = {                 /* v31 .. v28 of S2DEX 2.08 */
    0x08000040, 0x0002ffff, 0x40000003, 0x0008fff8, 0x01008000, 0xfe000000, 0x10000400, 0x20000200,
    0x3d100001, 0x32000000, 0x27000000, 0x35242628, 0xfffc0400, 0x00200010, 0xfc0003ff, 0x001fffe0 };

/* a sprite rectangle task: random DMEM with S2DEX 2.08's constants, tables and render-mode words (or random ones), a scissor, a sprite with a scale, flips, formats */
static void rect_dmem(uint32_t *dm)
{
    unsigned k, m;
    int px, py;
    uint16_t scale_pick[2];
    for (k = 0; k < 1024; k++)
        dm[k] = rnd();
    memcpy(dm + 0x180 / 4, bg_consts, sizeof bg_consts);
    m = rnd() % 8;
    for (k = 0; k < 16; k++)                    /* the tables: the data segment's, with a halfword random now and then; sometimes all random */
        st16(dm, 0x248 + 2 * k, m == 0 ? r16() : rnd() % 16 == 0 ? r16() : tab_real[k]);
    st16(dm, 0x244, rnd() & 1 ? 0x248 : 0x250);
    st16(dm, 0x246, rnd() & 1 ? 0x258 : 0x260);
    if (rnd() % 32 == 0)                        /* any 8-aligned pointer; the tables are random there */
        st16(dm, 0x244, (uint16_t)(rnd() & 0xff8));
    if (rnd() % 32 == 0)
        st16(dm, 0x246, (uint16_t)(rnd() & 0xff8));
    st32(dm, 0x1f8, rnd() % 4 ? w214_real[rnd() % 8] : rnd());
    st32(dm, 0x1fc, rnd() % 4 ? w234_real[rnd() % 4] : rnd());
    st8(dm, 0x2ae, (uint8_t)(rnd() % 4 ? 0x81 : rnd() % 3 ? rnd() % 4 : rnd()));
    st8(dm, 0x2af, (uint8_t)(rnd() % 4 == 0 ? 2 : rnd() % 8 == 0 ? rnd() : 0));
    st8(dm, 0xc9, (uint8_t)(rnd() % 3 == 0 ? 0x20 : rnd() % 2 ? 0 : rnd()));
    /* the scissor: the screen, a window in it, an inverted or random one */
    switch (rnd() % 6) {
    case 0: case 1: st16(dm, 0x204, 0); st16(dm, 0x206, 1280); st16(dm, 0x208, 0); st16(dm, 0x20a, 960); break;
    case 2: case 3: st16(dm, 0x204, (uint16_t)(rnd() % 600)); st16(dm, 0x206, (uint16_t)(600 + rnd() % 700)); st16(dm, 0x208, (uint16_t)(rnd() % 400)); st16(dm, 0x20a, (uint16_t)(400 + rnd() % 600)); break;
    case 4: st16(dm, 0x204, r16()); st16(dm, 0x206, r16()); st16(dm, 0x208, r16()); st16(dm, 0x20a, r16()); break;
    default: st16(dm, 0x204, (uint16_t)(rnd() % 1280)); st16(dm, 0x206, (uint16_t)(rnd() % 1280)); st16(dm, 0x208, (uint16_t)(rnd() % 960)); st16(dm, 0x20a, (uint16_t)(rnd() % 960)); break;
    }
    for (k = 0; k < 24; k++)                    /* the sprite at 0x3c0: objX scaleW imageW paddingX objY scaleH imageH paddingY, imageStride imageAdrs, fmt siz pal flags */
        st8(dm, 0x3c0 + k, (uint8_t)rnd());
    px = (int)(rnd() % 1500) - 150;
    py = (int)(rnd() % 1100) - 100;
    st16(dm, 0x3c0, rnd() % 16 == 0 ? r16() : (uint16_t)px);
    st16(dm, 0x3c8, rnd() % 16 == 0 ? r16() : (uint16_t)py);
    for (k = 0; k < 2; k++) {                   /* scales (s5.10): 1.0, magnifications and reductions, random positive, and sometimes anything */
        static const uint16_t nice[] = { 0x400, 0x400, 0x400, 0x200, 0x100, 0x800, 0x2ab, 0x7ff, 0x3fe, 0x555, 0x155, 0x401, 0x1000, 0x40 };
        scale_pick[k] = rnd() % 3 == 0 ? nice[rnd() % (sizeof(nice) / sizeof(nice[0]))] : rnd() % 16 == 0 ? r16() : rnd() % 4 ? (uint16_t)(0x30 + rnd() % 0x1000) : 0x400;
    }
    st16(dm, 0x3c2, scale_pick[0]);
    st16(dm, 0x3ca, scale_pick[1]);
    for (k = 0; k < 2; k++) {                   /* sizes (u10.5, the 15 bits of 1 .. 1024 texels, mostly the sizes of a sprite) */
        unsigned v = rnd() % 8 == 0 ? r16() : rnd() % 3 == 0 ? (1 + rnd() % 64) * 32 : (1 + rnd() % 1024) * 32 + (rnd() % 4 == 0 ? rnd() % 32 : 0);
        st16(dm, k ? 0x3cc : 0x3c4, (uint16_t)v);
    }
    st8(dm, 0x3d7, (uint8_t)(rnd() % 3 == 0 ? rnd() : rnd() % 4 * 0x10 + (rnd() & 1) ));
}

/* a G_BG_COPY task: the constants of S2DEX 2.08, the tables of the data segment at 0x274 (or random ones), a uObjBg of realistic and wild values, a scissor that cuts or rejects it */
static const uint16_t bgc_tab_real[16] = { 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 };
static void bgc_dmem(uint32_t *dm)
{
    unsigned k, m;
    for (k = 0; k < 1024; k++)
        dm[k] = rnd();
    memcpy(dm + 0x180 / 4, bg_consts, sizeof bg_consts);
    m = rnd() % 4;
    for (k = 0; k < 16; k++)
        st16(dm, 0x274 + 2 * k, m == 0 ? r16() : m == 1 ? (uint16_t)rnd() : bgc_tab_real[k]);
    for (k = 0; k < 40; k++)
        st8(dm, 0x3b0 + k, (uint8_t)rnd());
    for (k = 0; k < 8; k++) {                   /* imageX imageW frameX frameW, imageY imageH frameY frameH */
        unsigned v;
        switch (rnd() % 6) {
        case 0: v = r16(); break;
        case 1: v = rnd() % 0x1400; break;
        case 2: v = (rnd() % 400) * 4; break;
        case 3: v = (rnd() % 0x4000) - 0x2000; break;       /* the whole domain of the closed form, and its edges */
        default: v = (rnd() % 0x500); break;
        }
        st16(dm, 0x3b0 + 2 * k, (uint16_t)v);
    }
    st16(dm, 0x3b2, (uint16_t)(1 + rnd() % 0x500 * 4 / 4));
    st16(dm, 0x3ba, (uint16_t)((1 + rnd() % 300) * 4 + (rnd() % 8 == 0 ? rnd() % 4 : 0)));
    st16(dm, 0x3b6, (uint16_t)(1 + rnd() % 0x500));
    st16(dm, 0x3be, (uint16_t)(1 + rnd() % 0x400));
    switch (rnd() % 5) {
    case 0: case 1: st16(dm, 0x204, 0); st16(dm, 0x206, 1280); st16(dm, 0x208, 0); st16(dm, 0x20a, 960); break;
    case 2: st16(dm, 0x204, (uint16_t)(rnd() % 600)); st16(dm, 0x206, (uint16_t)(600 + rnd() % 700)); st16(dm, 0x208, (uint16_t)(rnd() % 400)); st16(dm, 0x20a, (uint16_t)(400 + rnd() % 600)); break;
    case 3: st16(dm, 0x204, (uint16_t)(rnd() % 0x4000 - 0x2000)); st16(dm, 0x206, (uint16_t)(rnd() % 0x4000 - 0x2000)); st16(dm, 0x208, (uint16_t)(rnd() % 0x4000 - 0x2000)); st16(dm, 0x20a, (uint16_t)(rnd() % 0x4000 - 0x2000)); break;
    default: st16(dm, 0x204, r16()); st16(dm, 0x206, r16()); st16(dm, 0x208, r16()); st16(dm, 0x20a, r16()); break;
    }
    st8(dm, 0x3c6, (uint8_t)(rnd() % 4)); st8(dm, 0x3c7, (uint8_t)(rnd() % 4));
    st8(dm, 0x3cb, (uint8_t)(rnd() % 3 == 0 ? rnd() : 0));
    st8(dm, 0x3c5, (uint8_t)(rnd() % 3 == 0 ? rnd() : rnd() % 100));
    st8(dm, 0x3c4, (uint8_t)rnd());
    st16(dm, 0x3d4, rnd() % 8 == 0 ? r16() : (uint16_t)(rnd() % 400));
    st16(dm, 0x3d6, (uint16_t)rnd());
    st16(dm, 0x3ce, (uint16_t)(1 + rnd() % 64));
    st16(dm, 0x3cc, rnd() % 4 ? (uint16_t)(rnd() % 512) : r16());
    st32(dm, 0x3c0, 0x02000000 | (rnd() & 0xfffff8));
}

static sigjmp_buf jb;
static void on_alarm(int s) { (void)s; siglongjmp(jb, 1); }

static uint32_t bg_seg(void *ctx, uint32_t a) { (void)ctx; return (a & 0xffffff) + 0x1000; }
static int bg_flush(void *ctx, uint32_t *s7, uint32_t *s6)
{
    (void)ctx;
    if ((int32_t)(*s7 - *s6) > 0) {
        *s6 ^= 0xc00;
        *s7 = *s6 - 800;
    }
    return 0;
}
#ifdef S2BG_ORACLE
static int or_seg(void *ctx, uint32_t *r) { (void)ctx; r[24] = (r[24] & 0xffffff) + 0x1000; return 0; }
static int or_flush(void *ctx, uint32_t *r) { (void)ctx; if ((int32_t)(r[23] - r[22]) > 0) { r[22] ^= 0xc00; r[23] = r[22] - 800; } return 0; }
#endif

int main(int argc, char **argv)
{
    unsigned n = argc > 1 ? (unsigned)atoi(argv[1]) : 100000, i, fails = 0, skipped = 0, done[4] = { 0, 0, 0, 0 }, bgc_n[3] = { 0, 0, 0 };
    static uint32_t a[1024], b[1024];
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);
    for (i = 0; i < n; i++) {
        int kind = (int)(rnd() % 4 == 0), ra, rb;
#ifdef S2BGC_ORACLE
        if (rnd() % 8 >= 6) kind = 2;
#endif
        unsigned sa_ = 0x4c0 + 8 * (rnd() % 8), sb_ = sa_;
        rand_dmem(a);
        memcpy(b, a, sizeof a);
        if (kind == 0) {
            rect_dmem(a);
            memcpy(b, a, sizeof a);
            sb_ = sa_;
            ra = s2dex2_obj_rect(a, &sa_);
            done[ra == S2_DONE ? 0 : ra == S2_CLIPPED ? 1 : 3]++;
            if (ra == S2_UNSUPPORTED && memcmp(a, b, sizeof a)) {
                if (fails++ < 5)
                    printf("rect %u: declined, but DMEM written\n", i);
            } else if (ra != S2_UNSUPPORTED) {
                if (ra == S2_CLIPPED && memcmp(a, b, sizeof a)) {
                    if (fails++ < 5)
                        printf("rect %u: outside the scissor, but DMEM written\n", i);
                }
#ifdef S2RECT_ORACLE
                {
                    struct rctx x;
                    uint32_t o[1024];
                    int orc;
                    memcpy(o, b, sizeof o);
                    memset(&x, 0, sizeof x);
                    x.u.dmem = o;
                    LQV(&x.u, 31, 0, 0x180); LQV(&x.u, 30, 0, 0x190); LQV(&x.u, 29, 0, 0x1a0); LQV(&x.u, 28, 0, 0x1b0);
                    VXOR(&x.u, 0, 0, 0, 0); VADD(&x.u, 1, 0, 0, 0); VSUB(&x.u, 1, 0, 31, 11);
                    LDV(&x.u, 4, 0, 0x1f8);
                    LDV(&x.u, 3, 0, dm16(o, 0x244));   /* the pointer in the microcode's lhu t3 */
                    x.r[20] = 0x3c0;
                    x.r[30] = 0x1c0;
                    x.r[23] = sb_;
                    rect_gen(&x, 0xba8);
                    orc = x.pc == 0x648 ? S2_DONE : x.pc == 0x1ac ? S2_CLIPPED : -1;
                    if (orc != ra || (ra == S2_DONE && x.r[23] != sa_) || (orc == S2_CLIPPED ? memcmp(o, b, sizeof o) : memcmp(o, a, sizeof o))) {
                        if (fails++ < 5)
                            printf("rect %u: closed form %d, microcode %d (pc %x), s7 %x / %x, dmem %s\n", i, ra, orc, x.pc, sa_, x.r[23], memcmp(o, ra == S2_CLIPPED ? b : a, sizeof o) ? "differs" : "equal");
                    }
                }
#endif
#ifdef S2RECT_VU
                {
                    uint32_t o[1024];
                    unsigned so = sb_;
                    int orc;
                    memcpy(o, b, sizeof o);
                    orc = s2ref_obj_rect(o, &so);
                    if (orc != ra || (ra == S2_DONE && so != sa_) || memcmp(o, ra == S2_CLIPPED ? b : a, sizeof o)) {
                        if (fails++ < 5)
                            printf("rect %u: closed form %d, vu kernel %d, dmem %s\n", i, ra, orc, memcmp(o, ra == S2_CLIPPED ? b : a, sizeof o) ? "differs" : "equal");
                    }
                }
#endif
            }
        } else if (kind == 2) {
#ifdef S2BGC_ORACLE
            struct s2_ctx c = { b, bg_seg, bg_flush };
            unsigned s7 = 0x4c0 + 8 * (rnd() % 8), s6 = 0x4c0 + 800, s7i = s7;
            bgc_dmem(a);
            memcpy(b, a, sizeof a);
            if (sigsetjmp(jb, 1)) {
                skipped++;
                continue;
            }
            ualarm(150000, 0);
            ra = s2dex2_obj_bgcopy(b, &s7, &s6, &c);
            ualarm(0, 0);
            if (ra == S2_UNSUPPORTED) {
                done[3]++;
                bgc_n[2]++;
                if (memcmp(a, b, sizeof a) && fails++ < 5)
                    printf("bgcopy %u: declined, but DMEM written\n", i);
            } else {
                struct rctx x;
                uint32_t r1[32], o[1024];
                unsigned vi;
                memset(&x, 0, sizeof x);
                memset(r1, 0, sizeof r1);
                memcpy(o, a, sizeof o);
                r1[20] = 0x3b0; r1[30] = 0x1c0; r1[23] = s7i; r1[22] = 0x4c0 + 800;
                x.u.dmem = o;
                LQV(&x.u, 31, 0, 0x180); LQV(&x.u, 30, 0, 0x190); LQV(&x.u, 29, 0, 0x1a0); LQV(&x.u, 28, 0, 0x1b0);
                VXOR(&x.u, 0, 0, 0, 0); VSUB(&x.u, 1, 0, 31, 11);
                for (vi = 2; vi < 14; vi++) { uint16_t *e = (uint16_t *)&x.u.r[vi]; unsigned q; for (q = 0; q < 8; q++) e[q] = (uint16_t)rnd(); }      /* stale vector state: must not matter */
                { uint16_t *e = (uint16_t *)&x.u.acc_md; unsigned q; for (q = 0; q < 8; q++) e[q] = (uint16_t)rnd(); e = (uint16_t *)&x.u.acc_hi; for (q = 0; q < 8; q++) e[q] = (uint16_t)rnd(); }
                memcpy(x.r, r1, sizeof r1);
                LLV(&x.u, 2, 4, x.r[30] + 68); LLV(&x.u, 2, 12, x.r[30] + 72);
                if (sigsetjmp(jb, 1)) {
                    skipped++;
                    continue;
                }
                ualarm(150000, 0);
                bgcopy_gen(&x, S2BGC_ENTRY);
                ualarm(0, 0);
                bgc_n[ra == S2_DONE && s7 == s7i ? 1 : 0]++;
                if (x.pc != S2BGC_END || memcmp(o, b, sizeof o) || x.r[23] != s7 || x.r[22] != s6) {
                    if (fails++ < 5)
                        printf("bgcopy %u: microcode pc %x, dmem %s, s7 %x / %x\n", i, x.pc, memcmp(o, b, sizeof o) ? "differs" : "equal", x.r[23], s7);
                }
            }
#else
            (void)0;
#endif
        } else {
            struct s2_ctx c = { b, bg_seg, bg_flush };
            unsigned k, s7 = 0x4c0, s6 = 0x4c0 + 800;
            for (k = 0; k < 16; k++)                    /* the microcode's vector constants (the kernel declines any others) */
                a[0x180 / 4 + k] = bg_consts[k];
            for (k = 0; k < 40; k++)
                st8(a, 0x3b0 + k, (uint8_t)rnd());
            st16(a, 0x3b0, (uint16_t)(r16() | 0x400));
            st16(a, 0x3b2, r16());
            st16(a, 0x3b4, (uint16_t)(r16() | 0x80));
            st16(a, 0x3b8, (uint16_t)(r16() | 0x40));
            st16(a, 0x3be, (uint16_t)(r16() & 0xfff));
            memcpy(b, a, sizeof a);
            if (sigsetjmp(jb, 1)) {
                skipped++;
                continue;
            }
            ualarm(150000, 0);
            ra = s2dex2_obj_bg1cyc(b, &s7, &s6, &c);
            ualarm(0, 0);
            done[2] += ra == S2_DONE;
#ifdef S2BG_ORACLE
            if (ra == S2_DONE) {
                struct rctx x;
                uint32_t r1[32];
                memset(&x, 0, sizeof x);
                memset(r1, 0, sizeof r1);
                r1[20] = 0x3b0;
                r1[30] = 0x1c0;
                r1[23] = 0x4c0;
                r1[22] = 0x4c0 + 800;
                x.u.dmem = a;
                LQV(&x.u, 31, 0, 0x180); LQV(&x.u, 30, 0, 0x190); LQV(&x.u, 29, 0, 0x1a0); LQV(&x.u, 28, 0, 0x1b0);
                VXOR(&x.u, 0, 0, 0, 0); VADD(&x.u, 1, 0, 0, 0); VSUB(&x.u, 1, 0, 31, 11);
                memcpy(x.r, r1, sizeof r1);
                x.ctx = a;
                x.seg = or_seg;
                x.flush = or_flush;
                if (sigsetjmp(jb, 1)) {
                    skipped++;
                    continue;
                }
                ualarm(150000, 0);
                bg1cyc_gen(&x, 0x6fc);
                ualarm(0, 0);
                if (x.pc != 0x1ac || memcmp(a, b, sizeof a) || x.r[23] != s7 || x.r[22] != s6) {
                    if (fails++ < 5)
                        printf("bg %u: microcode pc %x, dmem %s, s7 %x / %x\n", i, x.pc, memcmp(a, b, sizeof a) ? "differs" : "equal", x.r[23], s7);
                }
            }
#endif
        }
    }
#ifdef S2BGC_ORACLE
    printf("test_s2dex2: G_BG_COPY %u drawn / %u nothing visible / %u declined\n", bgc_n[0], bgc_n[1], bgc_n[2]);
#endif
    printf("test_s2dex2: %u cases, rectangles %u drawn / %u outside the scissor / %u declined, %u backgrounds done, %u skipped (timeout), %u failures; oracles:%s%s%s\n", n, done[0], done[1], done[3], done[2], skipped, fails,
#ifdef S2RECT_ORACLE
           " rect-microcode",
#else
           "",
#endif
#ifdef S2RECT_VU
           " rect-vu-kernel",
#else
           "",
#endif
#ifdef S2BG_ORACLE
           " bg-microcode"
#else
           ""
#endif
#ifdef S2BGC_ORACLE
           " bgcopy-microcode"
#endif
    );
    return fails != 0;
}
