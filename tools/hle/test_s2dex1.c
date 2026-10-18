/*
 * test_s2dex1.c: the S2DEX 1.06 / 1.07 closed forms (s2dex1_rect.c, s2dex1_bg.c, s2dex1_bgcopy.c; docs/ucode-s2dex1.md) on random DMEM images, against the microcode: the rsp2c translations of the three
 * routines (tools/hle/oracle/s1_*_gen.inc, git-ignored; regenerate as described in docs/testing.md, "Regenerating the microcode oracles"). Every case the closed form takes (it declines what it does not cover: reported, not a
 * failure) must give the same DMEM, output pointer and outcome as the translation.
 *
 * Local test only. Build and run: tools/hle/build_test_s2dex1.sh [first argument: extra compiler flags, -DARMLET_HLE_MUTATE=n for the mutation check] [second: the number of cases]
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

static void st8(uint32_t *dm, unsigned a, uint8_t v);
static int bgc_seg(void *x, uint32_t *r) { (void)x; r[19] = (r[24] & 0xffffff) + 0x1000; r[24] &= 0xffffff; return 0; }
static int bgc_flush(void *x, uint32_t *r) { (void)x; if ((int32_t)(r[23] - 0x8a8) > 0) r[23] = 0x588; return 0; }
#include "oracle/s1_rect_gen.inc"
#include "oracle/s1_bg1cyc_gen.inc"
#include "oracle/s1_bgcopy_gen.inc"
#include "oracle/s1_bgcopy107_gen.inc"
#include "oracle/s1_spr_full.inc"
#include "oracle/s1_rect_j.inc"

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

/* the data segment's render-mode tables (0x230 .. 0x24f: position mask, y fraction mask, s bias, flip signs, tile attributes) and render-mode words (0x200: 8, 0x220: 4) */
static const uint16_t tab_real[16] = { 0xfffc, 0, 0, 1, 0xffff, 3, 0xfff0, 0, 1, 0xffff, 8, 0x200, 0xffff, 1, 7, 0xc1f0 };
static const uint32_t w200_real[8] = { 0, 0x00100020, 0x00200040, 0x00300060, 0x0000fff4, 0x00100014, 0x00200034, 0x00300054 };
static const uint32_t w220_real[4] = { 0x0001fffe, 0xfffefffe, 0x00010000, 0 };
static const uint32_t consts[16] = {                    /* v31 .. v28 of S2DEX 1.06 / 1.07 (the same as 2.08's) */
    0x08000040, 0x0002ffff, 0x40000003, 0x0008fff8, 0x01008000, 0xfe000000, 0x10000400, 0x20000200,
    0x3d100001, 0x32000000, 0x27000000, 0x35242628, 0xfffc0400, 0x00200010, 0xfc0003ff, 0x001fffe0 };

static void scissor(uint32_t *dm, int wild)
{
    switch (rnd() % 6) {
    case 0: case 1: st16(dm, 0xc0, 0); st16(dm, 0xc2, 1280); st16(dm, 0xc4, 0); st16(dm, 0xc6, 960); break;
    case 2: case 3: st16(dm, 0xc0, (uint16_t)(rnd() % 600)); st16(dm, 0xc2, (uint16_t)(600 + rnd() % 700)); st16(dm, 0xc4, (uint16_t)(rnd() % 400)); st16(dm, 0xc6, (uint16_t)(400 + rnd() % 600)); break;
    case 4: if (wild) { st16(dm, 0xc0, (uint16_t)(rnd() % 0x4000 - 0x2000)); st16(dm, 0xc2, (uint16_t)(rnd() % 0x4000 - 0x2000)); st16(dm, 0xc4, (uint16_t)(rnd() % 0x4000 - 0x2000)); st16(dm, 0xc6, (uint16_t)(rnd() % 0x4000 - 0x2000)); }
            else { st16(dm, 0xc0, r16()); st16(dm, 0xc2, r16()); st16(dm, 0xc4, r16()); st16(dm, 0xc6, r16()); } break;
    default: st16(dm, 0xc0, (uint16_t)(rnd() % 1280)); st16(dm, 0xc2, (uint16_t)(rnd() % 1280)); st16(dm, 0xc4, (uint16_t)(rnd() % 960)); st16(dm, 0xc6, (uint16_t)(rnd() % 960)); break;
    }
}

/* a sprite rectangle task: random DMEM with S2DEX 1.x's constants, tables and render-mode words (or random ones), a scissor, a sprite with a scale, flips, formats */
static void rect_dmem(uint32_t *dm)
{
    unsigned k, m;
    int px, py;
    uint16_t scale_pick[2];
    for (k = 0; k < 1024; k++)
        dm[k] = rnd();
    memcpy(dm + 0x170 / 4, consts, sizeof consts);
    m = rnd() % 8;
    for (k = 0; k < 16; k++)
        st16(dm, 0x230 + 2 * k, m == 0 ? r16() : rnd() % 16 == 0 ? r16() : tab_real[k]);
    st16(dm, 0x252, rnd() & 1 ? 0x230 : 0x238);
    st16(dm, 0x254, rnd() & 1 ? 0x240 : 0x248);
    if (rnd() % 32 == 0)
        st16(dm, 0x252, (uint16_t)(rnd() & 0xff8));
    if (rnd() % 32 == 0)
        st16(dm, 0x254, (uint16_t)(rnd() & 0xff8));
    st32(dm, 0x1e8, rnd() % 4 ? w200_real[rnd() % 8] : rnd());
    st32(dm, 0x1ec, rnd() % 4 ? w220_real[rnd() % 4] : rnd());
    st8(dm, 0xd2, (uint8_t)(rnd() % 4 ? 0x81 : rnd() % 3 ? rnd() % 4 : rnd()));
    st8(dm, 0xd3, (uint8_t)(rnd() % 4 == 0 ? 2 : rnd() % 8 == 0 ? rnd() : 0));
    st8(dm, 0xc9, (uint8_t)(rnd() % 3 == 0 ? 0x20 : rnd() % 2 ? 0 : rnd()));
    scissor(dm, 0);
    for (k = 0; k < 24; k++)
        st8(dm, 0x530 + k, (uint8_t)rnd());
    px = (int)(rnd() % 1500) - 150;
    py = (int)(rnd() % 1100) - 100;
    st16(dm, 0x530, rnd() % 16 == 0 ? r16() : (uint16_t)px);
    st16(dm, 0x538, rnd() % 16 == 0 ? r16() : (uint16_t)py);
    for (k = 0; k < 2; k++) {
        static const uint16_t nice[] = { 0x400, 0x400, 0x400, 0x200, 0x100, 0x800, 0x2ab, 0x7ff, 0x3fe, 0x555, 0x155, 0x401, 0x1000, 0x40 };
        scale_pick[k] = rnd() % 3 == 0 ? nice[rnd() % (sizeof(nice) / sizeof(nice[0]))] : rnd() % 16 == 0 ? r16() : rnd() % 4 ? (uint16_t)(0x30 + rnd() % 0x1000) : 0x400;
    }
    st16(dm, 0x532, scale_pick[0]);
    st16(dm, 0x53a, scale_pick[1]);
    for (k = 0; k < 2; k++) {
        unsigned v = rnd() % 8 == 0 ? r16() : rnd() % 3 == 0 ? (1 + rnd() % 64) * 32 : (1 + rnd() % 1024) * 32 + (rnd() % 4 == 0 ? rnd() % 32 : 0);
        st16(dm, k ? 0x53c : 0x534, (uint16_t)v);
    }
    st8(dm, 0x547, (uint8_t)(rnd() % 3 == 0 ? rnd() : rnd() % 4 * 0x10 + (rnd() & 1)));
}


/* the flip table of the data segment (0x2b0: four rows of 8 halfwords) */
static const uint16_t flip_real[32] = { 1, 0, 0, 1, 0, 1, 0, 1,   0, 1, 1, 0, 0, 1, 0, 1,   0, 1, 1, 0, 1, 0, 1, 0,   1, 0, 0, 1, 1, 0, 1, 0 };

/* a G_OBJ_SPRITE task: as the rectangle's, plus the object matrix (rotations, scales, mirrors, shears and random numbers), its translation, the flip table */
static void sprite_dmem(uint32_t *dm)
{
    unsigned k;
    int m;
    rect_dmem(dm);
    if (rnd() % 4)                                              /* most sprites are meant to be seen: the screen as the scissor */
        { st16(dm, 0xc0, 0); st16(dm, 0xc2, 1280); st16(dm, 0xc4, 0); st16(dm, 0xc6, 960); }
    for (k = 0; k < 32; k++)
        st16(dm, 0x2b0 + 2 * k, rnd() % 32 ? flip_real[k] : r16());
    switch (rnd() % 6) {
    case 0: case 1: case 2: {                                   /* a rotation and scales */
        double ang = (rnd() % 3600) * 3.14159265358979 / 1800.0, sx = (1 + rnd() % 400) / 100.0 * (rnd() % 5 == 0 ? -1 : 1), sy = (1 + rnd() % 400) / 100.0 * (rnd() % 5 == 0 ? -1 : 1);
        int32_t A = (int32_t)(65536.0 * sx * __builtin_cos(ang)), B = (int32_t)(-65536.0 * sy * __builtin_sin(ang)), C = (int32_t)(65536.0 * sx * __builtin_sin(ang)), D = (int32_t)(65536.0 * sy * __builtin_cos(ang));
        if (rnd() % 4 == 0) { A = (int32_t)(65536.0 * sx); D = (int32_t)(65536.0 * sy); B = C = 0; }
        st32(dm, 0x1d0, (uint32_t)A); st32(dm, 0x1d4, (uint32_t)B); st32(dm, 0x1d8, (uint32_t)C); st32(dm, 0x1dc, (uint32_t)D);
        break;
    }
    case 3: for (k = 0; k < 4; k++) st32(dm, 0x1d0 + 4 * k, (uint32_t)((int32_t)(rnd() % 400000) - 200000)); break;
    default: for (k = 0; k < 4; k++) st32(dm, 0x1d0 + 4 * k, rnd()); break;
    }
    m = (int)(rnd() % 1500) - 150;
    st16(dm, 0x1e0, rnd() % 16 == 0 ? r16() : (uint16_t)m);
    m = (int)(rnd() % 1100) - 100;
    st16(dm, 0x1e2, rnd() % 16 == 0 ? r16() : (uint16_t)m);
    st32(dm, 0x1e4, rnd());
    st8(dm, 0xc9, (uint8_t)rnd());
}

/* the uObjBg and the tables of a background task */
static void bg_obj(uint32_t *dm, int copy)
{
    unsigned k;
    for (k = 0; k < 40; k++)
        st8(dm, 0x520 + k, (uint8_t)rnd());
    for (k = 0; k < 8; k++) {
        unsigned v;
        switch (rnd() % 6) {
        case 0: v = r16(); break;
        case 1: v = rnd() % 0x1400; break;
        case 2: v = (rnd() % 400) * 4; break;
        case 3: v = (rnd() % 0x4000) - 0x2000; break;
        default: v = (rnd() % 0x500); break;
        }
        st16(dm, 0x520 + 2 * k, (uint16_t)v);
    }
    if (copy) {
        st16(dm, 0x522, (uint16_t)(1 + rnd() % 0x500 * 4 / 4));
        st16(dm, 0x52a, (uint16_t)((1 + rnd() % 300) * 4 + (rnd() % 8 == 0 ? rnd() % 4 : 0)));
        st16(dm, 0x526, (uint16_t)(1 + rnd() % 0x500));
        st16(dm, 0x52e, (uint16_t)(1 + rnd() % 0x400));
    }
    st8(dm, 0x536, (uint8_t)(rnd() % 4));
    st8(dm, 0x537, (uint8_t)(rnd() % 4));
    st8(dm, 0x53b, (uint8_t)(rnd() % 3 == 0 ? rnd() : 0));
    st32(dm, 0x530, 0x02000000 | (rnd() & 0xfffff8));
}

static void bgc_dmem(uint32_t *dm)
{
    unsigned k, m;
    for (k = 0; k < 1024; k++)
        dm[k] = rnd();
    memcpy(dm + 0x170 / 4, consts, sizeof consts);
    m = rnd() % 4;
    for (k = 0; k < 16; k++)
        st16(dm, 0x260 + 2 * k, m == 0 ? r16() : m == 1 ? (uint16_t)rnd() : 0);
    bg_obj(dm, 1);
    scissor(dm, 1);
    st8(dm, 0x535, (uint8_t)(rnd() % 3 == 0 ? rnd() : rnd() % 100));
    st8(dm, 0x534, (uint8_t)rnd());
    st16(dm, 0x544, rnd() % 8 == 0 ? r16() : (uint16_t)(rnd() % 400));
    st16(dm, 0x546, (uint16_t)rnd());
    st16(dm, 0x53e, (uint16_t)(1 + rnd() % 64));
    st16(dm, 0x53c, rnd() % 4 ? (uint16_t)(rnd() % 512) : r16());
}

static sigjmp_buf jb;
static void on_alarm(int s) { (void)s; siglongjmp(jb, 1); }

static uint32_t k_seg(void *ctx, uint32_t a) { (void)ctx; return (a & 0xffffff) + 0x1000; }
static int k_flush(void *ctx, uint32_t *s7, uint32_t *s6) { (void)ctx; (void)s6; if ((int32_t)(*s7 - 0x8a8) > 0) *s7 = 0x588; return 0; }

static void vu_init(struct rctx *x, uint32_t *dm)
{
    memset(x, 0, sizeof *x);
    x->u.dmem = dm;
    LQV(&x->u, 31, 0, 0x170); LQV(&x->u, 30, 0, 0x180); LQV(&x->u, 29, 0, 0x190); LQV(&x->u, 28, 0, 0x1a0);
    VXOR(&x->u, 0, 0, 0, 0); VADD(&x->u, 1, 0, 29, 9);
    x->r[30] = 0x1b0;
}

int main(int argc, char **argv)
{
    unsigned n = argc > 1 ? (unsigned)atoi(argv[1]) : 100000, i, fails = 0, skipped = 0, cnt[6][3];
    static uint32_t a[1024], b[1024];
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    memset(cnt, 0, sizeof cnt);
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);
    for (i = 0; i < n; i++) {
        int kind = (int)(rnd() % 6), ra;
        if (getenv("S2T_ONLY")) kind = atoi(getenv("S2T_ONLY"));
        unsigned s7i = 0x588 + 8 * (rnd() % 40), s7 = s7i, s6 = 0;
        struct s2_ctx c = { b, k_seg, k_flush };
        struct rctx x;
        uint32_t o[1024];
        if (kind == 0) {
            rect_dmem(a);
            memcpy(b, a, sizeof a);
            ra = s2dex1_obj_rect(b, &s7);
            cnt[0][ra == S2_DONE ? 0 : ra == S2_CLIPPED ? 1 : 2]++;
            if (ra == S2_UNSUPPORTED) {
                if (memcmp(a, b, sizeof a) && fails++ < 5)
                    printf("rect %u: declined, but DMEM written\n", i);
                continue;
            }
            memcpy(o, a, sizeof o);
            vu_init(&x, o);
            LDV(&x.u, 4, 0, 0x1e8);
            LDV(&x.u, 3, 0, dm16(o, 0x252));
            x.r[20] = 0x530;
            x.r[23] = s7i;
            x.r[25] = 0x0300ffff;                                      /* t9 >= 0: not the rotated form */
            rect1_gen(&x, 0x714);
            {
                int orc = x.pc == 0xdb0 ? S2_DONE : x.pc == 0x158 ? S2_CLIPPED : -1;
                if (orc != ra || (ra == S2_DONE && x.r[23] != s7) || (ra == S2_CLIPPED ? memcmp(o, a, sizeof o) : memcmp(o, b, sizeof o))) {
                    if (fails++ < 5)
                        printf("rect %u: closed form %d, microcode %d (pc %x), s7 %x / %x, dmem %s\n", i, ra, orc, x.pc, s7, x.r[23], memcmp(o, ra == S2_CLIPPED ? a : b, sizeof o) ? "differs" : "equal");
                }
            }
        } else if (kind == 5) {
            rect_dmem(a);
            st16(a, 0x1e4, rnd() % 4 == 0 ? (uint16_t)(-(0x100 + (int)(rnd() % 0x700))) : rnd() % 8 ? (uint16_t)(0x100 + rnd() % 0x700) : r16());     /* (a negative base scale: the front's reciprocal is the multiply chain's, not the VADDC one) */
            st16(a, 0x1e6, rnd() % 4 == 0 ? (uint16_t)(-(0x100 + (int)(rnd() % 0x700))) : rnd() % 8 ? (uint16_t)(0x100 + rnd() % 0x700) : r16());
            if (rnd() % 4 == 0) { st16(a, 0x532, (uint16_t)-(int16_t)dm16(a, 0x532)); st16(a, 0x53a, (uint16_t)-(int16_t)dm16(a, 0x53a)); }
            st16(a, 0x1e0, r16()); st16(a, 0x1e2, r16());
            memcpy(b, a, sizeof a);
            if (getenv("S2T_DBGR")) {
                struct rctx y;
                uint32_t oo[1024];
                unsigned ax;
                memcpy(oo, a, sizeof oo);
                vu_init(&y, oo);
                LDV(&y.u, 4, 0, 0x1e8);
                LDV(&y.u, 3, 0, dm16(oo, 0x252));
                y.r[20] = 0x530; y.r[23] = s7i; y.r[25] = 0xb2000000;
                rect_j(&y, 0x714);
                for (ax = 0; ax < 2; ax++) {
                    const int pos = (int16_t)dm16(a, 0x530 + 8 * ax), sc = (int16_t)dm16(a, 0x532 + 8 * ax), bs = (int16_t)dm16(a, 0x1e4 + 2 * ax);
                    const int32_t P = pos * 2048;
                    const uint32_t rcp = ((uint32_t(*)(int32_t))0, 0);
                    (void)rcp; (void)sc; (void)bs; (void)P;
                    printf("case %u axis %d: oracle v5 %04x v6 %04x v2[scale] %04x (pc %x)\n", i, ax, ((uint16_t *)&y.u.r[5])[4 * ax], ((uint16_t *)&y.u.r[6])[4 * ax], ((uint16_t *)&y.u.r[2])[1 + 4 * ax], y.pc);
                }
            }
            ra = s2dex1_obj_rect_r(b, &s7);
            cnt[5][ra == S2_DONE ? 0 : ra == S2_CLIPPED ? 1 : 2]++;
            if (ra == S2_UNSUPPORTED) {
                if (memcmp(a, b, sizeof a) && fails++ < 5)
                    printf("rect_r %u: declined, but DMEM written\n", i);
                continue;
            }
            memcpy(o, a, sizeof o);
            vu_init(&x, o);
            LDV(&x.u, 4, 0, 0x1e8);
            LDV(&x.u, 3, 0, dm16(o, 0x252));
            x.r[20] = 0x530;
            x.r[23] = s7i;
            x.r[25] = 0xb2000000;                                      /* t9 < 0: the rotated form */
            rect1_gen(&x, 0x714);
            {
                int orc = x.pc == 0xdb0 ? S2_DONE : x.pc == 0x158 ? S2_CLIPPED : -1;
                if (orc != ra || (ra == S2_DONE && x.r[23] != s7) || (ra == S2_CLIPPED ? memcmp(o, a, sizeof o) : memcmp(o, b, sizeof o))) {
                    if (fails++ < 8) {
                        unsigned q, nd = 0, first = 0;
                        for (q = 0; q < 4096; q++) if (((uint8_t *)o)[q] != ((uint8_t *)b)[q]) { if (!nd++) first = q ^ 3; }
                        printf("rect_r %u: closed form %d, microcode %d (pc %x), s7 %x / %x, %u bytes differ, the first at %03x\n", i, ra, orc, x.pc, s7, x.r[23], nd, first);
                        for (q = 0; q < 4096; q++) if (((uint8_t *)o)[q] != ((uint8_t *)b)[q]) printf("   %03x: closed form %02x microcode %02x\n", q ^ 3, ((uint8_t *)b)[q], ((uint8_t *)o)[q]);
                        printf("   pos %04x %04x scale %04x %04x size %04x %04x bs %04x %04x X %04x Y %04x flags %02x; sprite words:", dm16(a, 0x530), dm16(a, 0x538), dm16(a, 0x532), dm16(a, 0x53a), dm16(a, 0x534), dm16(a, 0x53c), dm16(a, 0x1e4), dm16(a, 0x1e6), dm16(a, 0x1e0), dm16(a, 0x1e2), ((uint8_t *)a)[0x547 ^ 3]);
                        printf("\n   scissor %04x %04x %04x %04x rm %08x %08x ptrA %04x\n", dm16(a, 0xc0), dm16(a, 0xc2), dm16(a, 0xc4), dm16(a, 0xc6), a[0x1e8 / 4], a[0x1ec / 4], dm16(a, 0x252));
                    }
                }
            }
        } else if (kind == 4) {
            sprite_dmem(a);
            memcpy(b, a, sizeof a);
            ra = s2dex1_obj_sprite(b, &s7);
            cnt[4][ra == S2_DONE ? 0 : ra == S2_CLIPPED ? 1 : 2]++;
            if (ra == S2_UNSUPPORTED) {
                if (memcmp(a, b, sizeof a) && fails++ < 5)
                    printf("sprite %u: declined, but DMEM written\n", i);
                continue;
            }
            memcpy(o, a, sizeof o);
            vu_init(&x, o);
            if (getenv("S2T_STALE")) {                         /* the vector registers and the flags the earlier commands left: the microcode must not depend on them (the flags: it does, S2T_STALE=2 shows where) */
                unsigned vi, q;
                for (vi = 2; vi < 27; vi++) { uint16_t *e = (uint16_t *)&x.u.r[vi]; for (q = 0; q < 8; q++) e[q] = (uint16_t)rnd(); }
                if (atoi(getenv("S2T_STALE")) > 1) {
                    uint16_t *e = (uint16_t *)&x.u.vco_lo; for (q = 0; q < 8; q++) { e[q] = rnd() & 1 ? 0xffff : 0; }
                    e = (uint16_t *)&x.u.vco_hi; for (q = 0; q < 8; q++) e[q] = rnd() & 1 ? 0xffff : 0;
                    e = (uint16_t *)&x.u.vcc_lo; for (q = 0; q < 8; q++) e[q] = rnd() & 1 ? 0xffff : 0;
                    e = (uint16_t *)&x.u.vcc_hi; for (q = 0; q < 8; q++) e[q] = rnd() & 1 ? 0xffff : 0;
                }
            }
            LDV(&x.u, 4, 0, 0x1e8);
            LDV(&x.u, 3, 0, dm16(o, 0x252));
            x.r[20] = 0x530;
            x.r[23] = s7i;
            x.r[25] = 0x04000000;
            spr_full(&x, 0x258);
            {
                int orc = x.pc == 0xdb0 ? S2_DONE : x.pc == 0x158 ? S2_CLIPPED : -1;
                if (orc != ra || (ra == S2_DONE && x.r[23] != s7) || memcmp(o, b, sizeof o)) {
                    if (getenv("S2T_DUMP")) {
                        FILE *df = fopen(getenv("S2T_DUMP"), "ab");
                        uint32_t hd[4] = { 4, 0, 0, s7i };
                        if (df) { fwrite(hd, 4, 4, df); fwrite(a, 4, 1024, df); fclose(df); }
                    }
                    if (fails++ < 8) {
                        unsigned q, nd = 0, first = 0;
                        for (q = 0; q < 4096; q++) if (((uint8_t *)o)[q] != ((uint8_t *)b)[q]) { if (!nd++) first = q ^ 3; }
                        printf("sprite %u: closed form %d, microcode %d (pc %x), s7 %x / %x, %u bytes differ, the first at %03x (closed form %02x, microcode %02x)\n", i, ra, orc, x.pc, s7, x.r[23], nd, first, ((uint8_t *)b)[first ^ 3], ((uint8_t *)o)[first ^ 3]);
                    }
                }
            }
        } else if (kind == 1) {
            unsigned k;
            for (k = 0; k < 1024; k++)
                a[k] = rnd();
            memcpy(a + 0x170 / 4, consts, sizeof consts);
            bg_obj(a, 0);
            scissor(a, 1);
            st16(a, 0x520, (uint16_t)(r16() | 0x400));
            st16(a, 0x522, r16());
            st16(a, 0x524, (uint16_t)(r16() | 0x80));
            st16(a, 0x528, (uint16_t)(r16() | 0x40));
            st16(a, 0x52e, (uint16_t)(r16() & 0xfff));
            memcpy(b, a, sizeof a);
            if (sigsetjmp(jb, 1)) {
                skipped++;
                continue;
            }
            ualarm(150000, 0);
            ra = s2dex1_obj_bg1cyc(b, &s7, &s6, &c);
            ualarm(0, 0);
            cnt[1][ra == S2_DONE ? 0 : 2]++;
            if (ra == S2_UNSUPPORTED)                          /* (it may have written results before it found the case it does not cover: the task is declined as a whole) */
                continue;
            memcpy(o, a, sizeof o);
            vu_init(&x, o);
            x.r[20] = 0x520; x.r[23] = s7i; x.r[1] = 0x548;
            LLV(&x.u, 2, 4, 0xc0); LLV(&x.u, 2, 12, 0xc4);
            x.ctx = o; x.seg = bgc_seg; x.flush = bgc_flush;
            if (sigsetjmp(jb, 1)) {
                skipped++;
                continue;
            }
            ualarm(150000, 0);
            bg1cyc1_gen(&x, 0x27c);
            ualarm(0, 0);
            if (x.pc != 0x158 || memcmp(o, b, sizeof o) || x.r[23] != s7) {
                if (fails++ < 5)
                    printf("bg1cyc %u: microcode pc %x, dmem %s, s7 %x / %x\n", i, x.pc, memcmp(o, b, sizeof o) ? "differs" : "equal", x.r[23], s7);
            }
        } else {
            int v107 = kind == 3;
            bgc_dmem(a);
            memcpy(b, a, sizeof a);
            if (sigsetjmp(jb, 1)) {
                skipped++;
                continue;
            }
            ualarm(150000, 0);
            ra = v107 ? s2dex1_obj_bgcopy(b, &s7, &s6, &c) : s2dex1_obj_bgcopy106(b, &s7, &s6, &c);
            ualarm(0, 0);
            if (ra == S2_UNSUPPORTED) {
                cnt[kind][2]++;
                if (memcmp(a, b, sizeof a) && fails++ < 5)
                    printf("bgcopy %u: declined, but DMEM written\n", i);
                continue;
            }
            cnt[kind][s7 == s7i ? 1 : 0]++;
            memcpy(o, a, sizeof o);
            vu_init(&x, o);
            { unsigned vi, q; for (vi = 2; vi < 14; vi++) { uint16_t *e = (uint16_t *)&x.u.r[vi]; for (q = 0; q < 8; q++) e[q] = (uint16_t)rnd(); } }      /* stale vector state: must not matter */
            { uint16_t *e = (uint16_t *)&x.u.acc_md; unsigned q; for (q = 0; q < 8; q++) e[q] = (uint16_t)rnd(); e = (uint16_t *)&x.u.acc_hi; for (q = 0; q < 8; q++) e[q] = (uint16_t)rnd(); }
            x.r[20] = 0x520; x.r[23] = s7i;
            LLV(&x.u, 2, 4, 0xc0); LLV(&x.u, 2, 12, 0xc4);
            x.ctx = o; x.seg = bgc_seg; x.flush = bgc_flush;
            if (sigsetjmp(jb, 1)) {
                skipped++;
                continue;
            }
            ualarm(150000, 0);
            if (v107)
                bgcopy107_gen(&x, 0x984);
            else
                bgcopy1_gen(&x, 0x984);
            ualarm(0, 0);
            if (x.pc != 0x158 || memcmp(o, b, sizeof o) || x.r[23] != s7) {
                if (fails++ < 5)
                    printf("bgcopy%s %u: microcode pc %x, dmem %s, s7 %x / %x\n", v107 ? "107" : "106", i, x.pc, memcmp(o, b, sizeof o) ? "differs" : "equal", x.r[23], s7);
            }
        }
    }
    printf("test_s2dex1: rotated rectangles %u drawn / %u outside the scissor / %u declined\n", cnt[5][0], cnt[5][1], cnt[5][2]);
    printf("test_s2dex1: sprites %u drawn / %u outside the scissor / %u declined\n", cnt[4][0], cnt[4][1], cnt[4][2]);
    printf("test_s2dex1: %u cases; rectangles %u drawn / %u outside the scissor / %u declined; G_BG_1CYC %u done / %u declined; G_BG_COPY 1.06 %u drawn / %u nothing visible / %u declined, 1.07 %u / %u / %u; %u skipped (timeout), %u failures\n",
           n, cnt[0][0], cnt[0][1], cnt[0][2], cnt[1][0], cnt[1][2], cnt[2][0], cnt[2][1], cnt[2][2], cnt[3][0], cnt[3][1], cnt[3][2], skipped, fails);
    return fails != 0;
}
