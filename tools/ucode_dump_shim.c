/*
 * Survey-only RSP shim: forwards to the RSP plugin named by $UDUMP_TARGET and, for every graphics task (OSTask type 1), records
 * the ucode text and data segment from RDRAM (big-endian byte order) once per distinct (text, data) pair into $UDUMP_DIR, plus a
 * per-task sequence log $UDUMP_DIR/seq.txt ("<task#> <texthash> <datahash> <ucode_size> <data_size>") and the IMEM after the first run of each pair.
 * Used by tools/ucode_survey.py (docs/ucode-survey.md). Not part of any build.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "m64p_plugin.h"
#include "m64p_types.h"

static void *h;
static FILE *seq;
static RSP_INFO rinfo;
static const char *dir;
static unsigned long calls, gfx;
static char seen[256][34]; static unsigned nseen;
static m64p_error (*p_start)(m64p_dynlib_handle, void *, void (*)(void *, int, const char *));
static m64p_error (*p_stop)(void);
static m64p_error (*p_ver)(m64p_plugin_type *, int *, int *, const char **, int *);
static unsigned int (*p_do)(unsigned int);
static void (*p_init)(RSP_INFO, unsigned int *);
static void (*p_rc)(void);

static uint64_t fnv(const uint8_t *d, unsigned n)   /* FNV-1a-64 over host-endian 32-bit words, like src/gfx_ucode.c */
{
    uint64_t x = 0xcbf29ce484222325ull; unsigned i;
    for (i = 0; i < n; i += 4) { uint32_t w; memcpy(&w, d + i, 4); x = (x ^ w) * 0x100000001b3ull; }
    return x;
}
static void rd_be(uint8_t *out, const uint8_t *rdram, uint32_t a, unsigned n)   /* RDRAM / DMEM / IMEM are word-swapped on LE hosts */
{
    unsigned i; for (i = 0; i < n; i++) out[i] = rdram[((a + i) & 0x7fffff) ^ 3];
}
static void wr(const char *name, const uint8_t *b, unsigned n)
{
    char p[600]; FILE *f; snprintf(p, sizeof p, "%s/%s", dir, name);
    if ((f = fopen(p, "wb"))) { fwrite(b, 1, n, f); fclose(f); }
}
static uint32_t dm32(uint32_t o) { return *(const uint32_t *)(rinfo.DMEM + o); }

EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle ch, void *ctx, void (*cb)(void *, int, const char *))
{
    const char *t = getenv("UDUMP_TARGET"); dir = getenv("UDUMP_DIR");
    h = t ? dlopen(t, RTLD_NOW | RTLD_LOCAL) : NULL;
    if (!h || !dir) return M64ERR_INPUT_NOT_FOUND;
    p_start = dlsym(h, "PluginStartup"); p_stop = dlsym(h, "PluginShutdown"); p_ver = dlsym(h, "PluginGetVersion");
    p_do = dlsym(h, "DoRspCycles"); p_init = dlsym(h, "InitiateRSP"); p_rc = dlsym(h, "RomClosed");
    { char p[600]; snprintf(p, sizeof p, "%s/seq.txt", dir); seq = fopen(p, "w"); }
    return p_start(ch, ctx, cb);
}
EXPORT m64p_error CALL PluginShutdown(void) { m64p_error e = p_stop(); if (seq) { fclose(seq); seq = NULL; } return e; }
EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type *t, int *v, int *a, const char **n, int *c) {
    if (!p_ver) { if (t) *t = M64PLUGIN_RSP; if (v) *v = 0x010000; if (a) *a = 0x020000; if (n) *n = "udump"; if (c) *c = 0; return M64ERR_SUCCESS; }
    return p_ver(t, v, a, n, c);
}
/* Texts that a G_LOAD_UCODE (0xdd) loads in the middle of a task (Ocarina of Time's S2DEX, Kirby 64's): they are never a task's own text, so find the command in RDRAM
 * (a G_RDPHALF_1 0xe1 with the data address, then 0xdd with the text address and the size in w0), and dump text / data like a task start under the key of the first 0xf48 bytes. */
static void scan_loads(void)
{
    uint32_t a;
    for (a = 0; a + 16 <= 0x800000; a += 8) {
        uint8_t m[16]; uint32_t w1, w2, w0b, ta, da, sz;
        if (rinfo.RDRAM[(a ^ 3)] != 0xe1 || rinfo.RDRAM[((a + 8) ^ 3)] != 0xdd) continue;
        rd_be(m, rinfo.RDRAM, a, 16);
        w1 = m[4] << 24 | m[5] << 16 | m[6] << 8 | m[7];
        w0b = m[8] << 24 | m[9] << 16 | m[10] << 8 | m[11];
        w2 = m[12] << 24 | m[13] << 16 | m[14] << 8 | m[15];
        ta = w2 & 0x7fffff & ~7u; da = w1 & 0x7fffff & ~7u; sz = (w0b & 0xfff) + 1;
        if (!ta || !da || sz < 0x40 || sz > 0x800 || ta > 0x7fe000 || da > 0x7ff000) continue;
        {
            uint8_t tb[0x1000], db[0x1000 + 0x180], xb[0x2000], im[0x1000]; char key[40], n[100]; uint64_t th, dh = 0xcbf29ce484222325ull; unsigned i;
            rd_be(tb, rinfo.RDRAM, ta, 0x1000);
            th = fnv(rinfo.RDRAM + ta, 0xf48);
            memset(db, 0, 0x180); rd_be(db + 0x180, rinfo.RDRAM, da, sz);
            for (i = 0; i < sz; i++) dh = (dh ^ db[0x180 + i]) * 0x100000001b3ull;
            snprintf(key, sizeof key, "L%015llx.%016llx", (unsigned long long)th & 0xfffffffffffffffull, (unsigned long long)dh);
            for (i = 0; i < nseen; i++) if (!strcmp(seen[i], key)) break;
            if (i < nseen || nseen >= 256) continue;
            strcpy(seen[nseen++], key);
            snprintf(n, sizeof n, "%s.text", key); wr(n, tb, 0x1000);
            snprintf(n, sizeof n, "%s.data", key); wr(n, db, 0x180 + sz);
            rd_be(xb, rinfo.RDRAM, ta + 0x1000, sizeof xb); snprintf(n, sizeof n, "%s.ext", key); wr(n, xb, sizeof xb);
            memset(im, 0, sizeof im); snprintf(n, sizeof n, "%s.imem", key); wr(n, im, sizeof im);
            { char lp[600]; FILE *lf; snprintf(lp, sizeof lp, "%s/loads.txt", dir); if ((lf = fopen(lp, "a"))) { fprintf(lf, "%s %x %x %llx\n", key, 0xf48, 0x180 + sz, (unsigned long long)th); fclose(lf); } }
        }
    }
}
EXPORT unsigned int CALL DoRspCycles(unsigned int c)
{
    int isgfx = rinfo.DMEM && dm32(0xfc0) == 1;
    char key[40] = ""; unsigned done = 0;
    calls++;
    if (isgfx) {
        uint32_t ua = dm32(0xfd0) & 0x7fffff & ~3u, us = dm32(0xfd4), da = dm32(0xfd8) & 0x7fffff & ~3u, ds = dm32(0xfdc);
        uint8_t tb[0x1000], db[0x1000]; uint64_t th, dh; unsigned i;
        if (!us) us = 0xf80;
        if (us > 0x1000) us = 0x1000;
        us = (us + 3) & ~3u;
        if (!ds || ds > 0x1000) ds = ds ? 0x1000 : 0x800;
        ds = (ds + 3) & ~3u;
        th = fnv(rinfo.RDRAM + ua, us);
        rd_be(tb, rinfo.RDRAM, ua, 0x1000); rd_be(db, rinfo.RDRAM, da, ds);   /* the whole 4 KiB: the last 0x80 bytes are code the microcode loads itself when ucode_size says 0xf80 */
        { uint32_t x = 0; (void)x; }
        { uint64_t x = 0xcbf29ce484222325ull; for (i = 0; i < ds; i++) x = (x ^ db[i]) * 0x100000001b3ull; dh = x; }
        snprintf(key, sizeof key, "%016llx.%016llx", (unsigned long long)th, (unsigned long long)dh);
        gfx++;
        if (seq) { fprintf(seq, "%lu %s %x %x %x\n", gfx, key, us, ds, dm32(0xfc4)); fflush(seq); }
        for (i = 0; i < nseen; i++) if (!strcmp(seen[i], key)) break;
        if (i == nseen && nseen < 256) {
            char n[100]; strcpy(seen[nseen++], key);
            snprintf(n, sizeof n, "%s.text", key); wr(n, tb, 0x1000);
            snprintf(n, sizeof n, "%s.data", key); wr(n, db, ds);
            { uint8_t xb[0x2000]; rd_be(xb, rinfo.RDRAM, ua + 0x1000, sizeof xb); snprintf(n, sizeof n, "%s.ext", key); wr(n, xb, sizeof xb); }   /* RDRAM behind the 4 KiB: the overlays the data descriptors point at (tools/derive_build.py) */
            done = 1;
        }
    }
    {
        unsigned r = p_do(c);
        if (done) {
            uint8_t im[0x1000]; char n[100];
            rd_be(im, rinfo.IMEM, 0, 0x1000);
            snprintf(n, sizeof n, "%s.imem", key); wr(n, im, 0x1000);
        }
        if (isgfx && (gfx < 8 || (gfx & 31) == 0) && getenv("UDUMP_LOADS")) scan_loads();
        return r;
    }
}
EXPORT void CALL InitiateRSP(RSP_INFO i, unsigned int *cc) { rinfo = i; p_init(i, cc); }
EXPORT void CALL RomClosed(void) { p_rc(); }
