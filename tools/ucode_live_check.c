/*
 * ucode_live_check: src/ucode_live.c on the dumps of tools/ucode_dump_shim.c (DIR/<text>.<data>.{text,data,ext,imem,boot}; without .boot, rspboot is assumed).
 *   cc -O2 -Isrc -o ucode-live-check tools/ucode_live_check.c src/ucode_live.c && ./ucode-live-check DUMPDIR...
 * Per pair: the live ranges, the code's end, the signature, and the IMEM after the pair's first task traced back to the text: IMEM that only dead text holds is a load
 * the descriptors miss from the dead part of the 4 KiB ("UNSAFE": the signature would leave out live code). Per dump directory and data
 * image: how many ucode ids and how many signatures. Prints hashes and offsets only (the dumps are ROM-derived).
 */
#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ucode_live.h"

#define TA 0x100000u
#define DA 0x300000u
#define BA 0x500000u

static unsigned char *ram;

static long load_be(const char *path, uint32_t at, long max)
{
    FILE *f = fopen(path, "rb");
    unsigned char b[4];
    long n = 0;
    if (!f)
        return -1;
    while (n < max && fread(b, 1, 4, f) == 4) {
        uint32_t w = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
        memcpy(ram + at + n, &w, 4);
        n += 4;
    }
    fclose(f);
    return n;
}

static uint32_t txt(uint32_t off) { uint32_t w; memcpy(&w, ram + TA + off, 4); return w; }

static int live_at(const struct armlet_live *l, uint32_t lo, uint32_t hi)
{
    unsigned k;
    for (k = 0; k < l->n; k++)
        if (lo >= l->r[k].lo && hi <= l->r[k].hi)
            return 1;
    return 0;
}

struct grp { char data[17]; unsigned ids; unsigned nsig; uint64_t sig[1024]; };

int main(int argc, char **argv)
{
    int ai, bad = 0;
    ram = calloc(1, 0x800000);
    for (ai = 1; ai < argc; ai++) {
        DIR *d = opendir(argv[ai]);
        struct dirent *e;
        static struct grp g[64];
        unsigned ng = 0, i;
        memset(g, 0, sizeof(g));
        if (!d) {
            perror(argv[ai]);
            continue;
        }
        while ((e = readdir(d)) != NULL) {
            char base[300], p[600];
            uint32_t imem[1024];
            long dlen, blen;
            struct armlet_live l;
            uint64_t sig;
            unsigned m, unsafe = 0, unknown = 0, k;
            size_t nl = strlen(e->d_name);
            if (nl < 5 || strcmp(e->d_name + nl - 5, ".text"))
                continue;
            snprintf(base, sizeof(base), "%.*s", (int)(nl - 5), e->d_name);
            memset(ram + TA, 0, 0x4000);
            memset(ram + DA, 0, 0x1000);
            snprintf(p, sizeof(p), "%s/%s.text", argv[ai], base);
            if (load_be(p, TA, 0x1000) != 0x1000)
                continue;
            snprintf(p, sizeof(p), "%s/%s.ext", argv[ai], base);
            load_be(p, TA + 0x1000, 0x2000);
            snprintf(p, sizeof(p), "%s/%s.data", argv[ai], base);
            if ((dlen = load_be(p, DA, 0x1000)) <= 0)
                continue;
            snprintf(p, sizeof(p), "%s/%s.boot", argv[ai], base);
            memset(ram + BA, 0, 0x1000);
            if ((blen = load_be(p, BA, 0x1000)) < 0) {   /* (older dumps have no .boot: rspboot's two constants stand for it) */
                uint32_t w[2] = { 0x34011080, 0x34020f7f };
                memcpy(ram + BA, w, sizeof(w));
                blen = sizeof(w);
            }
            armlet_ucode_live(ram, TA, DA, (uint32_t)dlen, BA, (uint32_t)blen, &l);
            sig = armlet_ucode_live_sig(ram, TA, &l);
            snprintf(p, sizeof(p), "%s/%s.imem", argv[ai], base);
            {
                uint32_t su = 0, sk = 0;
                FILE *f = fopen(p, "rb");
                int have = 0;
                if (f) {
                    unsigned char b[4];
                    for (m = 0; m < 1024 && fread(b, 1, 4, f) == 4; m++)
                        imem[m] = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
                    have = m == 1024;
                    fclose(f);
                }
                /* IMEM words the boot load does not explain (zeros are taken as explained): the longest run of them that the text + the RDRAM behind it hold, a live
                 * source preferred; a run of 4+ words found only in dead text is UNSAFE, a word found nowhere is unknown (IMEM below 0x80: the boot code, expected) */
                for (m = 0; have && m < 0x1000;) {
                    uint32_t t, bl = 0, bd = 0, be = 0, n;
                    if (!imem[m / 4] || (m >= 0x80 && txt(m - 0x80) == imem[m / 4])) {
                        m += 4;
                        continue;
                    }
                    for (t = 0; t < 0x3000; t += 4) {
                        for (n = 0; m + 4 * n < 0x1000 && t + 4 * n < 0x3000 && txt(t + 4 * n) == imem[m / 4 + n]; n++)
                            ;
                        if (!n)
                            continue;
                        if (live_at(&l, t, t + 4)) {
                            while (!live_at(&l, t, t + 4 * n))  /* (the part of the run inside the live range: IMEM behind an overlay may still hold an older, longer one) */
                                n--;
                            bl = n > bl ? n : bl;
                        } else if (t < 0x1000)
                            bd = n > bd ? n : bd;
                        else
                            be = n > be ? n : be;
                    }
                    if (bl || (be && be > bd)) {        /* (a source behind the 4 KiB is no more part of the id than of the signature) */
                        m += 4 * (bl ? bl : be);
                        continue;
                    }
                    if (bd >= 4) {
                        if (!unsafe++)
                            su = m;
                        m += 4 * bd;
                        continue;
                    }
                    if (m >= 0x80 && !unknown++)
                        sk = m;
                    m += 4;
                }
                printf("%s %s ranges", argv[ai], base);
                for (k = 0; k < l.n; k++)
                    printf(" %x-%x", l.r[k].lo, l.r[k].hi);
                printf(" ovl %u code_end %x sig %016" PRIx64 "%s", l.nov, l.code_end, sig, have ? "" : " (no imem)");
                if (unsafe)
                    printf(" UNSAFE %u (first imem %03x)", unsafe, su);
                if (unknown)
                    printf(" unknown %u (first imem %03x)", unknown, sk);
                bad += unsafe != 0;
                printf("\n");
            }
            for (i = 0; i < ng; i++)
                if (!strcmp(g[i].data, base + 17))
                    break;
            if (i == ng && ng < 64)
                snprintf(g[ng++].data, sizeof(g[0].data), "%.16s", base + 17);
            if (i < ng) {
                g[i].ids++;
                for (k = 0; k < g[i].nsig && g[i].sig[k] != sig; k++)
                    ;
                if (k == g[i].nsig && k < 1024)
                    g[i].sig[g[i].nsig++] = sig;
            }
        }
        closedir(d);
        for (i = 0; i < ng; i++)
            printf("GROUP %s data %s: %u ids, %u signatures\n", argv[ai], g[i].data, g[i].ids, g[i].nsig);
    }
    printf("TOTAL unsafe pairs: %d\n", bad);
    return bad != 0;
}
