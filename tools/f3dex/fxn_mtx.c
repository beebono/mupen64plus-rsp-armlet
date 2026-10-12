/*
 * fxn_mtx.c: f3dex_mtx_mul (NEON when built for aarch64) against f3dex_mtx_mul_scalar, bit for bit, and the scalar form against an independent formula (the four-part product of the VMUDL / VMADM /
 * VMADN / VMADH chain, written out); aliasing out == a, out == b, a == b; the traps (saturating results, fractions >= 0x8000, negative integer parts, 48-bit wrap) as explicit cases; spot checks of
 * f3dex_relight against the documented examples (docs/ucode-f3dex/mtx.md 4.2). The differential test against the real microcode is spec/fxmtx_spec.c (rt_test).
 *   build_fxn.sh fxn_mtx host|a64 ;  run: fxn_mtx [matrices (default 2000000)] [seed]
 */
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

/* the independent formula: floor(A*B/65536) = (af*bf >> 16) + ai*bf + af*bi + (ai*bi << 16), summed in a 48-bit accumulator, then the two clamps */
static void ref_mul(const struct f3dex_mtx *a, const struct f3dex_mtx *b, struct f3dex_mtx *out)
{
    struct f3dex_mtx r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            int64_t acc = 0;
            for (int k = 0; k < 4; k++) {
                int64_t ai = a->i[i][k], af = a->f[i][k], bi = b->i[k][j], bf = b->f[k][j];
                acc += ((af * bf) >> 16) + ai * bf + af * bi + ((ai * bi) * 65536);
            }
            acc = (int64_t)((uint64_t)acc << 16) >> 16;        /* 48 bits */
            int64_t hi = acc >> 16;
            r.i[i][j] = (int16_t)(hi > 32767 ? 32767 : hi < -32768 ? -32768 : hi);
            r.f[i][j] = (acc >= -2147483648ll && acc < 2147483648ll) ? (uint16_t)acc : (acc < 0 ? 0 : 0xffff);
        }
    *out = r;
}

static uint16_t edge16(void)
{
    static const uint16_t v[] = { 0, 1, 2, 0x7fff, 0x8000, 0x8001, 0xffff, 0xfffe, 0x4000, 0xc000, 0x00ff, 0xff00, 0x7ffe };
    return v[rnd() % (sizeof v / sizeof v[0])];
}
static void gen(struct f3dex_mtx *m, unsigned kind)
{
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            uint16_t hi, lo;
            switch (kind) {
            case 0: hi = (uint16_t)rnd(); lo = (uint16_t)rnd(); break;                            /* full range */
            case 1: { int32_t x = (int32_t)(rnd() % 0x60000) - 0x30000; hi = (uint16_t)((uint32_t)x >> 16); lo = (uint16_t)x; break; }   /* small */
            case 2: hi = edge16(); lo = edge16(); break;                                           /* edge halves */
            case 3: hi = (rnd() & 1) ? 0x7fff : 0x8000; lo = (rnd() & 1) ? 0xffff : 0; break;     /* saturating */
            case 4: hi = (uint16_t)((int)(rnd() % 9) - 4); lo = (uint16_t)(0x8000 | rnd()); break; /* fractions >= 0x8000 */
            case 5: hi = (uint16_t)(0xff80 | (rnd() & 0x7f)); lo = (uint16_t)rnd(); break;         /* negative integer parts */
            case 6: hi = (rnd() % 3) ? 0 : (uint16_t)rnd(); lo = (rnd() % 3) ? 0 : (uint16_t)rnd(); break;   /* sparse */
            default: hi = (uint16_t)((i == j) ? 1 : 0); lo = 0; if (rnd() & 1) { hi = (uint16_t)rnd(); lo = edge16(); } break;
            }
            m->i[i][j] = (int16_t)hi;
            m->f[i][j] = lo;
        }
}

static unsigned long nbad;
static void report(const char *what, const struct f3dex_mtx *a, const struct f3dex_mtx *b, const struct f3dex_mtx *got, const struct f3dex_mtx *want)
{
    if (nbad++ < 8) {
        printf("MISMATCH %s\n", what);
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                if (got->i[i][j] != want->i[i][j] || got->f[i][j] != want->f[i][j])
                    printf("  [%d][%d] got %04x:%04x want %04x:%04x\n", i, j, (uint16_t)got->i[i][j], got->f[i][j], (uint16_t)want->i[i][j], want->f[i][j]);
        (void)a; (void)b;
    }
}
static int same(const struct f3dex_mtx *x, const struct f3dex_mtx *y) { return !memcmp(x, y, sizeof *x); }

static struct { unsigned long sat_hi, sat_lo_pos, sat_lo_neg, n; } cov;

static void check(const struct f3dex_mtx *a, const struct f3dex_mtx *b)
{
    struct f3dex_mtx want, s, n, t;
    ref_mul(a, b, &want);
    f3dex_mtx_mul_scalar(a, b, &s);
    if (!same(&s, &want)) report("scalar vs formula", a, b, &s, &want);
    f3dex_mtx_mul(a, b, &n);
    if (!same(&n, &s)) report("neon vs scalar", a, b, &n, &s);
    /* aliasing */
    t = *a; f3dex_mtx_mul(&t, b, &t);
    if (!same(&t, &s)) report("out == a", a, b, &t, &s);
    t = *b; f3dex_mtx_mul(a, &t, &t);
    if (!same(&t, &s)) report("out == b", a, b, &t, &s);
    t = *a; f3dex_mtx_mul_scalar(&t, b, &t);
    if (!same(&t, &s)) report("scalar out == a", a, b, &t, &s);
    t = *b; f3dex_mtx_mul_scalar(a, &t, &t);
    if (!same(&t, &s)) report("scalar out == b", a, b, &t, &s);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            cov.n++;
            cov.sat_hi += s.i[i][j] == 0x7fff && s.f[i][j] == 0xffff;
            cov.sat_lo_neg += s.i[i][j] == -32768 && s.f[i][j] == 0;
        }
}
static void check_self(const struct f3dex_mtx *a)
{
    struct f3dex_mtx s, t = *a;
    f3dex_mtx_mul_scalar(a, a, &s);
    f3dex_mtx_mul(&t, &t, &t);          /* a == b == out */
    if (!same(&t, &s)) report("a == b == out", a, a, &t, &s);
    check(a, a);
}

static void set(struct f3dex_mtx *m, int r, int c, int16_t i, uint16_t f) { m->i[r][c] = i; m->f[r][c] = f; }

static int relight_spot(void)
{
    /* docs/ucode-f3dex/mtx.md 4.2: (127,0,0) with any uniform scale gives 127,0,0; (100,60,-70) gives (94,56,-66) */
    struct f3dex_mtx id;
    memset(&id, 0, sizeof id);
    int bad = 0;
    int8_t raw[2][3] = { { 127, 0, 0 }, { 100, 60, -70 } }, out[2][4], out2[2][4];
    for (int sc = 0; sc < 4; sc++) {
        for (int k = 0; k < 3; k++)
            set(&id, k, k, sc == 0 ? 1 : sc == 1 ? 0 : sc == 2 ? 2 : 0, sc == 1 ? 0x8000 : sc == 3 ? 0x4000 : 0);
        f3dex_relight_scalar(&id, raw, 1, out);
        f3dex_relight(&id, raw, 1, out2);
        if (memcmp(out, out2, sizeof out)) { printf("relight scalar != relight\n"); bad++; }
        if (out[0][0] != 127 || out[0][1] != 0 || out[0][2] != 0) { printf("relight spot 1 (scale %d): %d %d %d\n", sc, out[0][0], out[0][1], out[0][2]); bad++; }
        /* (100,60,-70): the documented bytes, +-1 for a scale whose reciprocal square root differs in the last bit; the scale-1 case is the exact one */
        int e0 = out[1][0] - 94, e1 = out[1][1] - 56, e2 = out[1][2] + 66;
        if (sc == 0 ? (e0 || e1 || e2) : (e0 < -1 || e0 > 1 || e1 < -1 || e1 > 1 || e2 < -1 || e2 > 1)) { printf("relight spot 2 (scale %d): %d %d %d\n", sc, out[1][0], out[1][1], out[1][2]); bad++; }
        if (out[0][3] || out[1][3]) { printf("relight 4th byte not 0\n"); bad++; }
    }
    return bad;
}

int main(int argc, char **argv)
{
    unsigned long n = argc > 1 ? strtoul(argv[1], 0, 0) : 2000000;
    if (argc > 2) rs ^= (uint64_t)strtoull(argv[2], 0, 0) * 0x2545f4914f6cdd1dull;
    struct f3dex_mtx a, b;

    /* explicit traps */
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    for (int i = 0; i < 4; i++) { set(&a, i, i, 1, 0); set(&b, i, i, 1, 0); }
    check(&a, &b);
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { set(&a, i, j, 0x7fff, 0xffff); set(&b, i, j, 0x7fff, 0xffff); }
    check(&a, &b);                                          /* positive overflow: 0x7fff:0xffff everywhere */
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { set(&a, i, j, 0x7fff, 0xffff); set(&b, i, j, (int16_t)0x8000, 0); }
    check(&a, &b);                                          /* negative overflow: 0x8000:0 */
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { set(&a, i, j, (int16_t)0x8000, 0); set(&b, i, j, (int16_t)0x8000, 0); }
    check(&a, &b);                                          /* (-32768)^2 << 16 wraps in the 48-bit accumulator */
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { set(&a, i, j, 0, 0x8000); set(&b, i, j, 0, 0x8000); }
    check(&a, &b);                                          /* fraction x fraction: 0x8000 * 0x8000 >> 16 = 0x4000 per term, unsigned */
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { set(&a, i, j, -1, 0xffff); set(&b, i, j, -1, 0xffff); }
    check(&a, &b);                                          /* -1/65536 squared: floor of a tiny positive product, and of negative products */
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { set(&a, i, j, -1, 0x0001); set(&b, i, j, 1, 0xffff); }
    check(&a, &b);                                          /* negative integer part with a small fraction: floor, not truncation */
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { set(&a, i, j, (int16_t)(i - 2), (uint16_t)(0x8000 + j)); set(&b, i, j, (int16_t)(j - 2), (uint16_t)(0x9000 + i)); }
    check(&a, &b);
    {   /* one saturating cell among exact ones: the neighbours must be unaffected */
        struct f3dex_mtx m;
        memset(&m, 0, sizeof m);
        for (int i = 0; i < 4; i++) set(&m, i, i, 1, 0);
        set(&m, 0, 0, 0x7fff, 0xffff);
        set(&m, 1, 0, (int16_t)0x8000, 0);
        struct f3dex_mtx mm = m;
        for (int k = 0; k < 4; k++) set(&mm, k, 0, 0x7fff, 0xffff);
        check(&m, &mm);
        check(&mm, &m);
        check_self(&m);
    }
    /* every pair of kinds, then random */
    for (unsigned ka = 0; ka < 8; ka++)
        for (unsigned kb = 0; kb < 8; kb++)
            for (int rep = 0; rep < 2000; rep++) {
                gen(&a, ka); gen(&b, kb);
                check(&a, &b);
                if (rep < 100) check_self(&a);
            }
    for (unsigned long i = 0; i < n; i++) {
        unsigned ka = rnd() % 8, kb = rnd() % 8;
        gen(&a, ka);
        gen(&b, kb);
        check(&a, &b);
        if ((i & 63) == 0) check_self(&a);
    }
    int rb = relight_spot();
    printf("fxn_mtx: %lu random + %u kind-pair matrix products (%lu cells; saturated to 7fff:ffff %lu, to 8000:0000 %lu), %lu mismatches, relight spot checks %d bad%s\n", n, 8 * 8 * 2000, cov.n,
           cov.sat_hi, cov.sat_lo_neg, nbad, rb,
#ifdef __ARM_NEON
           " [NEON]"
#else
           " [scalar fall-back]"
#endif
    );
    return nbad || rb ? 1 : 0;
}
