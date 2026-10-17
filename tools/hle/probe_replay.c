/*
 * arMLEt - tools/hle/probe_replay.c: the probe (src/hle/probe.c) on ucode dumps, without a game
 *
 *   armlet-probe-replay [--cache FILE] [--latch] [--force] [--tol] [--seeds N] [--trace] [--rep N] TEXTFILE...
 *
 * TEXTFILE is a <id>.<datahash>.text dump of tools/ucode_dump_shim.c (.data and .ext next to it). For each one a private RSP is set up as at the start of a graphics task of that
 * microcode (the text in RDRAM and in IMEM at 0x80, the data segment in DMEM, an OSTask, a 128 KiB RDP output ring), and armlet_probe_offer is called the way the dispatch does until
 * the pair is done: derivation, canned tasks on the JIT and on the derived build, compare. One line per file. Build: scripts/build.sh native --tools. ROM-derived input: not in git.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "m64p_plugin.h"

#include "armlet.h"
#include "gfx_ucode.h"
#include "hle/hle_internal.h"
#include "hle/probe.h"
#include "native/native.h"
#include "native/native_internal.h"
#include "prsp_bridge.h"

static int l_trace;

static void log_cb(void *ctx, int level, const char *msg)
{
    (void)ctx;
    if (l_trace || level <= M64MSG_ERROR)
        fprintf(stderr, "%s\n", msg);
}

static void nop(void) {}

static struct {
    unsigned char *rdram;
    uint32_t dmem[1024], imem[1024];
    uint32_t regs[16], mi_intr, sp_pc;
    RSP_INFO info;
    struct armlet_ctx ctx;
} W;

static void rdp_taken(void) { W.regs[10] = W.regs[9]; W.regs[8] = W.regs[9]; W.regs[11] &= ~0x400u; }

static void world_init(void)
{
    RSP_INFO *i = &W.info;
    unsigned cycles = 0;
    W.rdram = calloc(1, 0x800000);
    if (!W.rdram)
        abort();
    memset(i, 0, sizeof(*i));
    i->RDRAM = W.rdram;
    i->DMEM = (unsigned char *)W.dmem;
    i->IMEM = (unsigned char *)W.imem;
    i->MI_INTR_REG = &W.mi_intr;
    i->SP_MEM_ADDR_REG = &W.regs[0];
    i->SP_DRAM_ADDR_REG = &W.regs[1];
    i->SP_RD_LEN_REG = &W.regs[2];
    i->SP_WR_LEN_REG = &W.regs[3];
    i->SP_STATUS_REG = &W.regs[4];
    i->SP_DMA_FULL_REG = &W.regs[5];
    i->SP_DMA_BUSY_REG = &W.regs[6];
    i->SP_SEMAPHORE_REG = &W.regs[7];
    i->DPC_START_REG = &W.regs[8];
    i->DPC_END_REG = &W.regs[9];
    i->DPC_CURRENT_REG = &W.regs[10];
    i->DPC_STATUS_REG = &W.regs[11];
    i->DPC_CLOCK_REG = &W.regs[12];
    i->DPC_BUFBUSY_REG = &W.regs[13];
    i->DPC_PIPEBUSY_REG = &W.regs[14];
    i->DPC_TMEM_REG = &W.regs[15];
    i->SP_PC_REG = &W.sp_pc;
    i->CheckInterrupts = nop;
    i->ProcessDlistList = nop;
    i->ProcessAlistList = nop;
    i->ProcessRdpList = rdp_taken;
    i->ShowCFB = nop;
    parallelRSPInitiateRSP(*i, &cycles);
    W.ctx.rsp = *i;
    W.ctx.prsp = *i;
}

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

static void put_be(uint32_t addr, const uint8_t *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        W.rdram[((addr + (uint32_t)i) & 0x7fffff) ^ 3] = b[i];
}

static const char *outcome_name(int o)
{
    switch (o) {
    case PROBE_OUT_NO_CANDIDATE: return "NO_CANDIDATE";
    case PROBE_OUT_UNSUPPORTED: return "UNSUPPORTED";
    case PROBE_OUT_PASS: return "PASS";
    case PROBE_OUT_PASS_TOL: return "PASS_TOL";
    case PROBE_OUT_FAIL: return "FAIL";
    case PROBE_OUT_INCONCLUSIVE: return "INCONCLUSIVE";
    case PROBE_OUT_PENDING: return "PENDING";
    default: return "NONE";
    }
}

int main(int argc, char **argv)
{
    int i, force = 0, latch = 0, tol = 0, reps = 1;
    unsigned seeds = 0;
    const char *cache = "off";
    int nfail = 0;
    int world = 0;

    armlet_log_set(log_cb, NULL);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--force"))
            force = 1;
        else if (!strcmp(argv[i], "--latch"))
            latch = 1;
        else if (!strcmp(argv[i], "--tol"))
            tol = 1;
        else if (!strcmp(argv[i], "--trace"))
            l_trace = 1;
        else if (!strcmp(argv[i], "--cache") && i + 1 < argc)
            cache = argv[++i];
        else if (!strcmp(argv[i], "--seeds") && i + 1 < argc)
            seeds = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rep") && i + 1 < argc)
            reps = atoi(argv[++i]);
        else {
            char p[1024];
            size_t nt, nd, ne = 0, L = strlen(argv[i]), k;
            uint8_t *t, *d, *e;
            const uint32_t TA = 0x100000, DA = 0x200000, RS = 0x300000, RE = 0x320000, DLA = 0x400000;
            struct armlet_ucode_id u;
            struct probe_summary s;
            int calls, rep;
            const char *stem = strrchr(argv[i], '/');
            stem = stem ? stem + 1 : argv[i];
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
            if (!world) {
                world_init();
                world = 1;
            }
            for (rep = 0; rep < reps; rep++) {
                memset(W.rdram, 0, 0x800000);
                memset(W.dmem, 0, sizeof(W.dmem));
                memset(W.imem, 0, sizeof(W.imem));
                memset(W.regs, 0, sizeof(W.regs));
                W.mi_intr = 0;
                put_be(TA, t, 4096);
                if (e)
                    put_be(TA + 0x1000, e, ne > 0x2000 ? 0x2000 : ne);
                put_be(DA, d, nd > 0x1000 ? 0x1000 : nd);
                for (k = 0; k < nd && k < 0x1000; k++)
                    ((uint8_t *)W.dmem)[k ^ 3] = d[k];                     /* the data segment, where the boot code puts it */
                for (k = 0; k < 0xf80; k++)
                    ((uint8_t *)W.imem)[(0x80 + k) ^ 3] = t[k];             /* the text, where the boot code puts it: IMEM 0x80 .. 0xfff */
                {
                    uint32_t *m = W.dmem;
                    m[0xfc0 >> 2] = 1;
                    m[0xfd0 >> 2] = TA;
                    m[0xfd4 >> 2] = 0xf80;
                    m[0xfd8 >> 2] = DA;
                    m[0xfdc >> 2] = (uint32_t)nd;
                    m[0xfe8 >> 2] = RS;
                    m[0xfec >> 2] = RE;
                    m[0xff0 >> 2] = DLA;
                    m[0xff4 >> 2] = 8;
                }
                put_be(DLA, (const uint8_t[]){ 0xdf, 0, 0, 0, 0, 0, 0, 0 }, 8);
                W.sp_pc = 0x80;
                memset(&W.ctx.cfg, 0, sizeof(W.ctx.cfg));
                W.ctx.cfg.gfx_mode = ARMLET_GFX_HLE;
                W.ctx.cfg.probe = 1;
                W.ctx.cfg.probe_latch = latch;
                W.ctx.cfg.probe_force = force;
                W.ctx.cfg.probe_tol = tol;
                W.ctx.cfg.probe_seeds = seeds;
                snprintf(W.ctx.cfg.ucode_cache, sizeof(W.ctx.cfg.ucode_cache), "%s", cache);
                armlet_hle_init(&W.ctx);
                memset(&u, 0, sizeof(u));
                u.id = armlet_ucode_hash(W.rdram, TA, 0x1000);
                u.family = ARMLET_FAM_F3DEX2;
                for (calls = 0; calls < 400; calls++) {
                    armlet_probe_offer(&W.ctx, &u);
                    if (!armlet_probe_summary(u.id, &s) && s.outcome != PROBE_OUT_PENDING)
                        break;
                }
                if (armlet_probe_summary(u.id, &s)) {
                    printf("PROBE %s outcome=NOT_OFFERED\n", stem);
                    nfail++;
                } else {
                    printf("PROBE %.*s outcome=%s verdict=%s family=%s cache=%s calls=%d tasks=%u equal=%u tol=%u declined=%u differ=%u inconclusive=%u derive_ms=%.2f probe_ms=%.2f rec=%s%s%s latched=%d note=\"%s\"\n",
                           (int)(strlen(stem) - 5), stem, outcome_name(s.outcome), s.verdict == 2 ? "EXACT" : s.verdict == 1 ? "NEAR" : "NONE", s.family == 0 ? "f3dex2" : s.family == 1 ? "f3dex1" : "f3d",
                           s.from_cache ? "hit" : "miss", calls, s.n_tasks, s.n_equal, s.n_tol, s.n_declined, s.n_mismatch, s.n_inconclusive, s.derive_ms, s.probe_ms,
                           s.rec_equal_registered < 0 ? "-" : s.rec_equal_registered ? "equal" : "DIFF", s.rec_equal_registered == 0 && s.first_diff ? ":" : "", s.rec_equal_registered == 0 && s.first_diff ? s.first_diff : "",
                           s.latched, s.note);
                    if (s.outcome == PROBE_OUT_FAIL)
                        nfail++;
                }
                armlet_hle_reset();
            }
            free(t);
            free(d);
            free(e);
        }
    }
    return nfail ? 1 : 0;
}
