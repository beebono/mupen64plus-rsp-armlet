/*
 * arMLEt - tools/native/replay.c: offline harness for native routines
 *
 *   armlet-native-replay [--bench N] [--dmabench BYTES] [--quiet] file.snap...
 *
 * Replays routine-entry snapshots (ARMLET_NATIVE_CAPTURE=<dir> on a verify or capture run writes them): each one is loaded into a private
 * RSP (the fork's JIT, with a private RDRAM that holds only the rows the routine's DMA reads read), and the routine is run natively in the
 * sandbox and by the JIT from the same state, exactly as GfxNative=verify does on a device; mismatches are reported with full context. The
 * state the JIT ends in is also compared with the one recorded when the snapshot was taken. With --bench N the native routine and the
 * JIT's code for the same stretch are timed N times each from the snapshot.
 *
 * Build: scripts/build.sh native --tools -> out/native-tools/armlet-native-replay. Snapshots are ROM-derived: keep them in test-output/.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <inttypes.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "m64p_plugin.h"

#include "armlet.h"
#include "native/native.h"
#include "native/native_internal.h"
#include "hle/hle_internal.h"
#include "prsp_bridge.h"

static unsigned l_mismatch_msgs;
static const char *l_as;            /* --as NAME: run this routine on snapshots taken for another (an oracle-only capture routine) */
static int l_quiet;

static void log_cb(void *ctx, int level, const char *msg)
{
    (void)ctx;
    if (level <= M64MSG_ERROR)
        l_mismatch_msgs++;
    if (!l_quiet || level <= M64MSG_ERROR)
        fprintf(stderr, "%s\n", msg);
}

static void nop(void) {}

static struct {
    unsigned char *rdram;
    uint32_t dmem[1024], imem[1024];
    uint32_t regs[16], mi_intr, sp_pc;
    RSP_INFO info;
} W;

/* the video plugin takes the list at once (as the host core does): without it the microcode's flush waits for the RDP forever on a JIT run (--bench) */
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
}

static int l_oracle;                    /* --oracle: the oracle of a task snapshot is a JIT run from the same state, not the recorded result */
static uint32_t *l_entry;
static void jit_once(void *p)
{
    (void)p;
    W.regs[4] &= ~3u;
    W.sp_pc = *l_entry;
    parallelRSPDoRspCycles(~0u);
}
static unsigned dmab, dmab_bytes = 1024;      /* --dmabench BYTES: time a sync (DPC_END + a DMA write) */

static int replay(const char *path, unsigned bench)
{
    FILE *f = fopen(path, "rb");
    struct armlet_snapfile_hdr h;
    static PrspNativeSnap before, after, cur;
    PrspNativeEvent *ev = NULL;
    unsigned i;
    int idx, rc = 0;
    unsigned next_pc = 0;
    int mode = 0;
    uint64_t mm0;
    char buf[4096];

    if (!f) {
        fprintf(stderr, "%s: cannot open\n", path);
        return 1;
    }
    if (fread(&h, sizeof(h), 1, f) != 1 || h.magic != ARMLET_SNAPFILE_MAGIC || fread(&before, sizeof(before), 1, f) != 1) {
        fprintf(stderr, "%s: not a snapshot\n", path);
        fclose(f);
        return 1;
    }
    memset(W.rdram, 0, 0x800000);
    for (i = 0; i < h.n_rows; i++) {
        uint32_t rh[2], j, w;
        if (fread(rh, 4, 2, f) != 2) {
            rc = 1;
            break;
        }
        for (j = 0; j < rh[1]; j += 4) {
            if (fread(&w, 4, 1, f) != 1) {
                rc = 1;
                break;
            }
            memcpy(W.rdram + ((rh[0] + j) & 0x7ffffc), &w, 4);
        }
    }
    if (rc || fread(&after, sizeof(after), 1, f) != 1) {
        fprintf(stderr, "%s: truncated\n", path);
        fclose(f);
        return 1;
    }
    ev = malloc((h.n_events + 1) * sizeof(*ev));
    if (ev && h.n_events)
        (void)!fread(ev, sizeof(*ev), h.n_events, f);
    fclose(f);

    if (!strcmp(h.routine, "hle.task")) {
        const struct hle_ucode *uc = armlet_hle_find(h.ucode);
        struct hle_harness_result res;
        int nd;
        if (!uc) {
            fprintf(stderr, "%s: ucode %016" PRIx64 " has no task-level handler in this build\n", path, h.ucode);
            free(ev);
            return 1;
        }
        before.pc = h.entry_pc;                 /* the capture took the state before the JIT set its pc from SP_PC */
        if (getenv("HLE_POKE")) {               /* HLE_POKE=addr=val,addr=val (hex): DMEM bytes changed before the task runs (the recorded result then no longer applies) */
            const char *q = getenv("HLE_POKE");
            unsigned a, v;
            int n;
            while (sscanf(q, "%x=%x%n", &a, &v, &n) == 2) {
                ((uint8_t *)before.dmem)[(a & 0xfff) ^ 3] = (uint8_t)v;
                q += n;
                if (*q == ',')
                    q++;
            }
        }
        if (getenv("HLE_PATCH")) {              /* HLE_PATCH=addr:hexbytes;addr:hexbytes (hex): RDRAM bytes written before the task runs (tools/f3d/mksyn.py, tools/hle/mksyn_ex2.py: synthetic display lists; use --oracle) */
            const char *q = getenv("HLE_PATCH");
            unsigned a;
            int n;
            while (sscanf(q, "%x:%n", &a, &n) == 1) {
                q += n;
                while (isxdigit((unsigned char)q[0]) && isxdigit((unsigned char)q[1])) {
                    char hx[3] = { q[0], q[1], 0 };
                    W.rdram[(a++ & 0x7fffff) ^ 3] = (uint8_t)strtoul(hx, NULL, 16);
                    q += 2;
                }
                if (*q == ';')
                    q++;
            }
        }
        prsp_native_load(&before);
        prsp_native_resync();
        if (l_oracle) {
            static uint32_t entry;
            entry = h.entry_pc;
            l_entry = &entry;
            nd = armlet_hle_harness_jit(uc, &before, jit_once, NULL, buf, sizeof(buf), &res);
        } else
            nd = armlet_hle_harness(uc, &before, &after, ev, h.n_events, buf, sizeof(buf), &res);
        if (nd < 0) {
            printf("%s: DECLINED %s", path, buf);
            free(ev);
            return 2;
        }
        printf("%s: %s -> %s (%.1f us)\n", path, uc->name, nd ? "MISMATCH native vs JIT" : "native == JIT", res.native_us);
        if (nd)
            printf("%s", buf);
        if (dmab && !nd) {
            printf("  sync (DPC_END + a DMA write of %u bytes to the ring): %.0f ns with the events recorded, %.0f ns not recording\n", dmab_bytes, armlet_hle_dmabench(uc, &before, 20000, dmab_bytes, 1), armlet_hle_dmabench(uc, &before, 20000, dmab_bytes, 0));
        }
        if (bench && !nd) {
            double nn, jn = 0;
            uint8_t *ram0 = malloc(0x800000);       /* the JIT's runs write the RDRAM (lists, and in some games over the display list): every run starts from the same bytes */
            if (ram0)
                memcpy(ram0, W.rdram, 0x800000);
            nn = armlet_hle_bench(uc, &before, bench);
            unsigned k;
            for (k = 0; k < bench + 1 && !getenv("HLE_BENCH_NOJIT"); k++) {      /* (HLE_BENCH_NOJIT=1: some tasks do not run to their end on the JIT outside the plugin) */
                struct timespec a, b;
                prsp_native_load(&before);
                prsp_native_resync();
                if (ram0)
                    memcpy(W.rdram, ram0, 0x800000);
                W.regs[4] &= ~3u;
                W.sp_pc = h.entry_pc;
                clock_gettime(CLOCK_MONOTONIC, &a);
                if (getenv("HLE_HK_DEBUG"))
                    fprintf(stderr, "JIT run %u\n", k);
                parallelRSPDoRspCycles(~0u);
                clock_gettime(CLOCK_MONOTONIC, &b);
                if (k)                  /* the first run compiles */
                    jn += (double)(b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec);
            }
            jn /= bench;
            if (getenv("HLE_BENCH_NOJIT"))
                jn = nn;
            free(ram0);
            printf("  bench: front-end %.1f us, JIT %.1f us (%.2fx)\n", nn / 1000.0, jn / 1000.0, jn / nn);
        }
        free(ev);
        return nd != 0;
    }

    idx = armlet_native_find(l_as ? l_as : h.routine, h.ucode);
    if (idx < 0) {
        fprintf(stderr, "%s: routine %s of ucode %016" PRIx64 " is not in this build\n", path, h.routine, h.ucode);
        free(ev);
        return 1;
    }
    {
        struct armlet_native_settings s;
        memset(&s, 0, sizeof(s));
        s.mode = ARMLET_NATIVE_VERIFY;
        snprintf(s.routines, sizeof(s.routines), "%s", l_as ? l_as : h.routine);
        s.stats = 1;
        s.capture_every = 1;
        armlet_native_start(&s);
    }
    armlet_native_select(h.ucode);
    {
        const struct armlet_routine *rt = armlet_native_routine((unsigned)idx);
        if (rt->run && armlet_native_routine_hash(rt, before.imem) != rt->imem_hash) {
            printf("%s: not applicable: the IMEM of the snapshot is not what %s was made for\n", path, rt->name);
            free(ev);
            return 4;
        }
    }
    prsp_native_load(&before);
    prsp_native_resync();

    mm0 = armlet_native_stats_of(idx)->mismatches;
    l_mismatch_msgs = 0;
    if (armlet_native_call((unsigned)idx, &next_pc, &mode) != PRSP_NATIVE_DONE) {
        printf("%s: DECLINED %s (entry %03x)\n", path, h.routine, h.entry_pc);
        free(ev);
        return 2;
    }
    prsp_native_save(&cur);
    {
        unsigned nd = armlet_native_compare(&cur, &after, (unsigned)idx, buf, sizeof(buf));
        int bad = armlet_native_stats_of(idx)->mismatches != mm0;
        if (nd)
            fprintf(stderr, "%s: the JIT's result now differs from the recorded one (%u differences):\n%s", path, nd, buf);
        printf("%s: %s entry %03x exit %03x -> %s%s\n", path, l_as ? l_as : h.routine, h.entry_pc, next_pc, bad ? "MISMATCH native vs JIT" : "native == JIT",
               nd ? ", recorded result differs" : ", recorded result reproduced");
        rc = bad || nd;
    }
    if (bench && !rc) {
        double nn, jn, un;
        int r = armlet_native_bench((unsigned)idx, &before, next_pc, bench, &nn, &jn, &un);
        if (r)
            printf("  bench failed (%d)\n", r);
        else
            printf("  bench: native %.0f ns/call, JIT %.0f ns/call (%.1fx), %.1f units/call -> native %.1f ns/unit, JIT %.1f ns/unit\n", nn, jn, jn / nn, un,
                   un ? nn / un : 0.0, un ? jn / un : 0.0);
    }
    free(ev);
    return rc;
}

/* --hookbench N: a synthetic program (a loop of N calls of a `jr ra` subroutine at 0x100, IMEM words below) run on the JIT, without and with the native routine
 * test.hook at 0x100: the cost of one hooked call from the JIT, against the JIT running the two instructions itself. */
static void hookbench(unsigned n)
{
    static const uint32_t prog[] = {
        0x3c100000u | (0u),                 /* lui   s0, hi        (patched) */
        0x36100000u,                        /* ori   s0, s0, lo    (patched) */
        0x0c000040u,                        /* jal   0x100 */
        0x00000000u,                        /* nop */
        0x2210ffffu,                        /* addi  s0, s0, -1 */
        0x1600fffcu,                        /* bne   s0, zero, 0x008 */
        0x00000000u,                        /* nop */
        0x0000000du,                        /* break */
    };
    double t[3];
    unsigned mode;
    int idx;
    uint64_t ids[3] = { 0, 0x0123456789abcdefull, 0x0123456789abcde0ull };
    uint64_t id = ids[1];
    struct armlet_native_settings s;

    memset(W.imem, 0, sizeof(W.imem));
    memcpy(W.imem, prog, sizeof(prog));
    W.imem[0] = prog[0] | (n >> 16);
    W.imem[1] = prog[1] | (n & 0xffff);
    W.imem[0x100 / 4] = 0x03e00008u;
    W.imem[0x104 / 4] = 0;
    memset(&s, 0, sizeof(s));
    s.mode = ARMLET_NATIVE_ON;
    snprintf(s.routines, sizeof(s.routines), "test.hook,test.hookd");
    armlet_native_start(&s);
    idx = armlet_native_find("test.hook", id);
    if (idx < 0) {
        fprintf(stderr, "test.hook is not in this build\n");
        return;
    }
    for (mode = 0; mode < 3; mode++) {
        int rep;
        double best = 1e30;
        armlet_native_select(ids[mode]);
        for (rep = 0; rep < 5; rep++) {
            struct timespec a, b;
            prsp_native_resync();
            W.regs[4] = 0;                  /* SP_STATUS: not halted */
            W.sp_pc = 0;
            clock_gettime(CLOCK_MONOTONIC, &a);
            parallelRSPDoRspCycles(~0u);
            clock_gettime(CLOCK_MONOTONIC, &b);
            if ((double)(b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec) < best)
                best = (double)(b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec);
        }
        t[mode] = best / n;
    }
    printf("hookbench: %u calls; per loop iteration: JIT alone %.1f ns, with the hooked call %.1f ns (+%.1f), with the hook declining %.1f ns (+%.1f)\n", n, t[0], t[1], t[1] - t[0], t[2], t[2] - t[0]);
}

extern unsigned armlet_test_dep2_calls(void);

/* --dep2test: a routine with two dependency ranges is dropped only when IMEM changes inside one of them */
static int dep2test(void)
{
    static const uint32_t prog[] = { 0x0c000040u, 0, 0x0000000du };    /* jal 0x100; nop; break */
    uint64_t id = 0x0123456789abcdf0ull;
    struct armlet_native_settings s;
    unsigned c0, step, fails = 0;
    static const struct { unsigned addr; int expect_kept; const char *what; } steps[] = {
        { 0x200, 1, "a word between the ranges" }, { 0x108, 1, "a word after the first range, same block" }, { 0x400, 0, "a word of the second range" },
        { 0x104, 0, "the first range" } };

    memset(&s, 0, sizeof(s));
    s.mode = ARMLET_NATIVE_ON;
    snprintf(s.routines, sizeof(s.routines), "test.dep2");
    armlet_native_start(&s);
    armlet_native_select(id);
    for (step = 0; step < 4; step++) {
        memset(W.imem, 0, sizeof(W.imem));
        memcpy(W.imem, prog, sizeof(prog));
        W.imem[0x100 / 4] = 0x03e00008u;
        prsp_native_resync();
        W.regs[4] = 0;
        W.sp_pc = 0;
        parallelRSPDoRspCycles(~0u);        /* first run: the stub is installed, the routine is called */
        c0 = armlet_test_dep2_calls();
        W.imem[steps[step].addr / 4] ^= 0x0000a5a5u;    /* a harmless change (a nop turns into an andi $0 ... does nothing to the flow) */
        prsp_native_resync();
        W.regs[4] = 0;
        W.sp_pc = 0;
        parallelRSPDoRspCycles(~0u);
        {
            int called = armlet_test_dep2_calls() > c0;
            printf("dep2test: change of %s: routine %s (expected %s)\n", steps[step].what, called ? "still called" : "dropped", steps[step].expect_kept ? "kept" : "dropped");
            if (called != steps[step].expect_kept)
                fails++;
        }
    }
    printf("dep2test: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}

int main(int argc, char **argv)
{
    unsigned bench = 0, hb = 0, d2 = 0;
    int a, rc = 0, n = 0, declined = 0, na = 0;

    armlet_log_set(log_cb, NULL);
    for (a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "--bench") && a + 1 < argc)
            bench = (unsigned)atoi(argv[++a]);
        else if (!strcmp(argv[a], "--dmabench") && a + 1 < argc) {
            dmab = 1;
            dmab_bytes = (unsigned)atoi(argv[++a]);
        } else if (!strcmp(argv[a], "--dep2test"))
            d2 = 1;
        else if (!strcmp(argv[a], "--hookbench") && a + 1 < argc)
            hb = (unsigned)atoi(argv[++a]);
        else if (!strcmp(argv[a], "--quiet"))
            l_quiet = 1;
        else if (!strcmp(argv[a], "--oracle"))
            l_oracle = 1;
        else if (!strcmp(argv[a], "--masks")) {
            /* the DMEM ranges that the task-level path declares dead, per ucode, in the form tools/equiv/rsp_shim.c reads (RSPT_MASKS): "<id>:<lo>-<hi>,<lo>-<hi>;..." */
            armlet_hle_print_masks(stdout);
            return 0;
        }
        else if (!strcmp(argv[a], "--as") && a + 1 < argc)
            l_as = argv[++a];
        else
            break;
    }
    if (a >= argc && !hb && !d2) {
        fprintf(stderr, "usage: %s [--bench N] [--hookbench N] [--as ROUTINE] [--quiet] file.snap...\n", argv[0]);
        return 2;
    }
    world_init();
    if (hb) {
        hookbench(hb);
        return 0;
    }
    if (d2)
        return dep2test();
    for (; a < argc; a++) {
        /* one child per snapshot: each starts from the same fresh world, and a watchdog abort (the JIT never reaching a wrong exit pc) ends only its own */
        int r, status = 0;
        pid_t pid;
        fflush(NULL);
        pid = fork();
        if (pid == 0) {
            int cr = replay(argv[a], bench);
            fflush(NULL);                   /* _exit does not */
            _exit(cr);
        }
        waitpid(pid, &status, 0);
        r = WIFEXITED(status) ? WEXITSTATUS(status) : 3;
        if (r == 3)
            printf("%s: ABORTED (watchdog or crash)\n", argv[a]);
        n++;
        if (r == 4)
            na++;
        else if (r == 2)
            declined++;
        else if (r)
            rc = 1;
    }
    printf("%d snapshots, %d declined, %d not applicable, %s\n", n, declined, na, rc ? "FAILED" : "all equal");
    return rc;
}
