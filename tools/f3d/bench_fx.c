/*
 * arMLEt - tools/f3d/bench_fx.c: real-data check and timing of the struct-level F3D functions (src/hle/f3d_fx.h) on routine-entry snapshots of a real game
 * (ARMLET_NATIVE_CAPTURE files of the f3d.vtx / f3d.tri routines, written by a verify run; ROM-derived: keep them in test-output/).
 *   bench_fx [-r reps] file.snap...
 * For every snapshot the function is called on the structs read from the entry DMEM, its result is compared with the DMEM the JIT left (the snapshot's "after"), and the calls are timed:
 * the scalar form and the NEON form separately, per call and per vertex / triangle. No JIT, no emulator: builds for aarch64 and runs on a device.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "prsp_native.h"
#include "f3d_fx.h"
#include "f3d_dmem.h"

struct hdr { uint32_t magic, version; char routine[48]; uint64_t ucode; uint32_t entry_pc, exit_pc, n_rows, n_events, flags; };

struct rec {
    int vtx;
    PrspNativeSnap before, after;
};

static double now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }

static int load(const char *path, struct rec *r)
{
    FILE *f = fopen(path, "rb");
    struct hdr h;
    if (!f) return 0;
    if (fread(&h, sizeof h, 1, f) != 1 || fread(&r->before, sizeof r->before, 1, f) != 1) { fclose(f); return 0; }
    for (unsigned i = 0; i < h.n_rows; i++) {
        uint32_t rh[2];
        if (fread(rh, 4, 2, f) != 2 || fseek(f, rh[1], SEEK_CUR)) { fclose(f); return 0; }
    }
    if (fread(&r->after, sizeof r->after, 1, f) != 1) { fclose(f); return 0; }
    fclose(f);
    r->vtx = strstr(h.routine, "vtx") != NULL;
    return 1;
}

int main(int argc, char **argv)
{
    unsigned reps = 200;
    int first = 1;
    if (argc > 2 && !strcmp(argv[1], "-r")) { reps = (unsigned)atoi(argv[2]); first = 3; }
    unsigned nrec = (unsigned)(argc - first), nv = 0, nt = 0;
    struct rec *R = calloc(nrec ? nrec : 1, sizeof *R);
    unsigned n = 0;
    for (int i = first; i < argc; i++)
        if (load(argv[i], &R[n])) n++;
    /* ---- vertices: check, then time ---- */
    struct vcase { struct f3d_vctx c; struct f3d_vin in[16]; unsigned n, a2; int ok; } *V = calloc(n, sizeof *V);
    unsigned vtx_total = 0, vbad = 0, vcheck = 0;
    for (unsigned i = 0; i < n; i++) {
        if (!R[i].vtx) continue;
        const uint32_t *dm = R[i].before.dmem;
        unsigned at = R[i].before.sr[1];
        struct vcase *v = &V[nv];
        v->n = (at >> 4) + 1; v->a2 = at & 15;
        if (v->n + v->a2 > 16 || f3d_load_vctx(&v->c, dm, &f3d_layout_a)) continue;
        for (unsigned k = 0; k < v->n; k++) f3d_load_vin(&v->in[k], dm, R[i].before.sr[22] + 16 * k, 0);
        struct f3d_vtx out[16], outs[16];
        f3d_vtx_xform(&v->c, v->in, v->n, out);
        f3d_vtx_xform_scalar(&v->c, v->in, v->n, outs);
        int bad = memcmp(out, outs, v->n * sizeof out[0]) != 0;
        for (unsigned k = 0; k < v->n; k++) {
            struct f3d_vtx j;
            f3d_load_vtx(&j, R[i].after.dmem, 0x420 + 40 * (v->a2 + k));
            if (memcmp(&j, &out[k], sizeof j)) bad = 1;
        }
        vcheck++;
        vbad += bad;
        vtx_total += v->n;
        nv++;
    }
    printf("vertex calls: %u checked against the JIT's result, %u differ\n", vcheck, vbad);
    if (nv) {
        struct f3d_vtx out[16];
        for (int form = 0; form < 2; form++) {
            double t0 = now_ns();
            for (unsigned r = 0; r < reps; r++)
                for (unsigned i = 0; i < nv; i++)
                    form ? (void)f3d_vtx_xform(&V[i].c, V[i].in, V[i].n, out) : (void)f3d_vtx_xform_scalar(&V[i].c, V[i].in, V[i].n, out);
            double ns = now_ns() - t0;
            printf("  %s: %.0f ns per call, %.1f ns per vertex (%.1f vertices per call)\n", form ? "NEON  " : "scalar", ns / reps / nv, ns / reps / vtx_total, (double)vtx_total / nv);
        }
    }
    /* ---- triangles ---- */
    struct tcase { struct f3d_vtx v[3]; struct f3d_tri_in ti; int ok; } *T = calloc(n, sizeof *T);
    unsigned tcheck = 0, tbad = 0, tdrawn = 0, tother = 0;
    for (unsigned i = 0; i < n; i++) {
        if (R[i].vtx) continue;
        const uint32_t *dm = R[i].before.dmem;
        unsigned k1 = R[i].before.sr[27];
        unsigned flag = fd8(dm, k1 - 4);
        unsigned pv[3] = { 0x420 + 4 * fd8(dm, k1 - 3), 0x420 + 4 * fd8(dm, k1 - 2), 0x420 + 4 * fd8(dm, k1 - 1) };
        int okr = flag <= 2;
        for (int k = 0; k < 3; k++) if (pv[k] < 0x420 || pv[k] + 40 > 0x6a0) okr = 0;
        if (!okr) continue;
        struct tcase *t = &T[nt];
        for (int k = 0; k < 3; k++) { f3d_load_vtx(&t->v[k], dm, pv[k]); t->ti.v[k] = &t->v[k]; }
        t->ti.flat = &t->v[flag];
        t->ti.geom = fd32(dm, 0x114); t->ti.persp = fd16(dm, 0x112); t->ti.tile_byte = fd8(dm, 0x122); t->ti.zclamp = fd8(dm, 0x121);
        t->ti.reject_mask = 0x7070; t->ti.clip_mask = 0x4343; t->ti.vco_carry = R[i].before.flags[0][9] != 0;
        uint8_t cmd[0xc0], cmds[0xc0];
        unsigned len = 0, lens = 0;
        int rc = f3d_tri_setup(&t->ti, cmd, &len), rcs = f3d_tri_setup_scalar(&t->ti, cmds, &lens);
        int bad = rc != rcs || (rc == F3D_TRI_OK && (len != lens || memcmp(cmd, cmds, len)));
        if (rc == F3D_TRI_OK) {
            for (unsigned b = 0; b < len; b++) if (fd8(R[i].after.dmem, 0x9e0 + b) != cmd[b]) bad = 1;
            tdrawn++;
        } else tother++;
        if (bad) { printf("triangle #%u (%s): rc %d scalar %d len %u/%u geom %08x\n", i, argv[first + i], rc, rcs, len, lens, t->ti.geom); if (rc == F3D_TRI_OK && len == lens) for (unsigned b = 0; b < len; b++) if (cmd[b] != cmds[b] || fd8(R[i].after.dmem, 0x9e0 + b) != cmd[b]) { printf("  byte %u: neon %02x scalar %02x jit %02x\n", b, cmd[b], cmds[b], fd8(R[i].after.dmem, 0x9e0 + b)); break; } }
        tcheck++;
        tbad += bad;
        nt++;
    }
    printf("triangle calls: %u checked against the JIT's output buffer (%u drawn, %u rejected / culled / clipped), %u differ\n", tcheck, tdrawn, tother, tbad);
    if (nt) {
        uint8_t cmd[0xc0];
        unsigned len;
        unsigned *idx[2] = { malloc(nt * sizeof(unsigned)), malloc(nt * sizeof(unsigned)) }, cnt[2] = { 0, 0 };
        for (unsigned i = 0; i < nt; i++) {
            int rc0 = f3d_tri_setup_scalar(&T[i].ti, cmd, &len);
            unsigned k = rc0 == F3D_TRI_OK;
            idx[k][cnt[k]++] = i;
        }
        for (int form = 0; form < 2; form++) {
            double ns[2] = { 0, 0 };
            for (int k = 0; k < 2; k++) {
                if (!cnt[k]) continue;
                double t0 = now_ns();
                for (unsigned r = 0; r < reps * 4; r++)
                    for (unsigned j = 0; j < cnt[k]; j++)
                        form ? (void)f3d_tri_setup(&T[idx[k][j]].ti, cmd, &len) : (void)f3d_tri_setup_scalar(&T[idx[k][j]].ti, cmd, &len);
                ns[k] = (now_ns() - t0) / (reps * 4.0) / cnt[k];
            }
            printf("  %s: %.0f ns per drawn triangle (%u), %.0f ns per rejected / culled / clipped (%u), mean %.0f\n", form ? "NEON  " : "scalar", ns[1], cnt[1], ns[0], cnt[0],
                   (ns[1] * cnt[1] + ns[0] * cnt[0]) / nt);
        }
    }
    return vbad || tbad;
}
