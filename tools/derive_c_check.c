/*
 * tools/derive_c_check.c: runs the C derivation (src/hle/derive.c) on dump files and prints the result in the form tools/derive_c_check.py prints for the Python tool.
 *
 *   derive_c_check [--family auto|f3dex2|f3dex1|f3d] [--exclude RECNAME]... [--full] [--time] TEXTFILE...
 *
 * TEXTFILE is a <id>.<datahash>.text dump of tools/ucode_dump_shim.c; .data and .ext next to it are read as well. ROM-derived input: nothing of it is written out.
 * Not part of the plugin; see docs/testing.md ("The C derivation").
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/hle/derive.h"

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    long sz;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (uint8_t *)malloc((size_t)sz + 1);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(b);
        return NULL;
    }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

static void field(const char *name, unsigned long long v, void *arg)
{
    (void)arg;
    printf(" %s=%llx", name, v);
}

static int cmp_ext(const void *a, const void *b)
{
    const int32_t *x = (const int32_t *)a, *y = (const int32_t *)b;
    return x[0] != y[0] ? (x[0] < y[0] ? -1 : 1) : x[1] - y[1];
}

int main(int argc, char **argv)
{
    int family = DERIVE_AUTO, full = 0, timing = 0, i;
    uint64_t excl = 0;
    double total = 0;
    int n = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--family") && i + 1 < argc) {
            const char *f = argv[++i];
            family = !strcmp(f, "f3dex2") ? DERIVE_F3DEX2 : !strcmp(f, "f3dex1") ? DERIVE_F3DEX1 : !strcmp(f, "f3d") ? DERIVE_F3D : DERIVE_AUTO;
        } else if (!strcmp(argv[i], "--exclude") && i + 1 < argc) {
            unsigned r;
            const char *nm = argv[++i];
            for (r = 0; r < derive_n_recs; r++)
                if (!strcmp(derive_recs[r].name, nm))
                    excl |= 1ull << r;
        } else if (!strcmp(argv[i], "--full"))
            full = 1;
        else if (!strcmp(argv[i], "--time"))
            timing = 1;
        else {
            char p[1024];
            size_t nt, nd, ne = 0;
            uint8_t *t, *d, *e;
            uint32_t w[1024];
            struct derive_in in;
            static struct derive_result res;
            const char *stem;
            size_t L = strlen(argv[i]);
            unsigned k;
            if (L < 6 || strcmp(argv[i] + L - 5, ".text")) {
                fprintf(stderr, "%s: not a .text file\n", argv[i]);
                return 2;
            }
            t = slurp(argv[i], &nt);
            snprintf(p, sizeof(p), "%.*s.data", (int)(L - 5), argv[i]);
            d = slurp(p, &nd);
            snprintf(p, sizeof(p), "%.*s.ext", (int)(L - 5), argv[i]);
            e = slurp(p, &ne);
            if (!t || !d || nt < 4096) {
                fprintf(stderr, "%s: cannot read\n", argv[i]);
                return 2;
            }
            for (k = 0; k < 1024; k++)
                w[k] = (uint32_t)t[4 * k] << 24 | (uint32_t)t[4 * k + 1] << 16 | (uint32_t)t[4 * k + 2] << 8 | t[4 * k + 3];
            memset(&in, 0, sizeof(in));
            in.text = w;
            in.data = d;
            in.data_len = nd;
            in.ext = e;
            in.ext_len = ne;
            in.family = family;
            in.exclude_recs = excl;
            in.full = full;
            stem = strrchr(argv[i], '/');
            stem = stem ? stem + 1 : argv[i];
            if (derive_run(&in, &res)) {
                printf("%.*s ERROR\n", (int)(strlen(stem) - 5), stem);
                free(t); free(d); free(e);
                continue;
            }
            printf("%.*s family=%s verdict=%s aligned=%u/%u nx=%u x=%u near=%u,%u,%u xr=%u,%u,%u primary=%s contrib=", (int)(strlen(stem) - 5), stem, derive_family_name(res.family),
                   derive_verdict_name(res.verdict), res.aligned_num, res.aligned_den, res.nx, res.n_x, res.n_near[0], res.n_near[1], res.n_near[2], res.n_xr[0], res.n_xr[1], res.n_xr[2],
                   derive_refs[res.primary].name);
            for (k = 0; k < (unsigned)res.n_contrib; k++)
                printf("%s%s:%u", k ? "," : "", derive_refs[res.contrib[k].ref].name, res.contrib[k].n);
            printf(" trouble=%d", res.knob_trouble);
            if (res.family == DERIVE_F3DEX2) {
                int32_t ex[DERIVE_MAXEXT][2];
                derive_rec_fields(&res.rec, field, NULL);
                for (k = 0; k < res.rec.n_ext; k++) {
                    ex[k][0] = res.rec.ext[k].pc;
                    ex[k][1] = res.rec.ext[k].kind;
                }
                qsort(ex, res.rec.n_ext, sizeof(ex[0]), cmp_ext);
                printf(" ext=");
                for (k = 0; k < res.rec.n_ext; k++)
                    printf("%s%x:%x", k ? "," : "", (unsigned)ex[k][0] & 0xffffffffu, (unsigned)ex[k][1]);
            }
            printf("\n");
            if (timing) {
                fprintf(stderr, "%s: %.3f ms\n", stem, res.work_ns / 1e6);
                total += res.work_ns / 1e6;
                n++;
            }
            free(t); free(d); free(e);
        }
    }
    if (timing && n)
        fprintf(stderr, "mean %.3f ms over %d\n", total / n, n);
    return 0;
}
