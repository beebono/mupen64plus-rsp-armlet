/*
 * arMLEt - probe.c: the probe (probe.h; docs/design.md, "The probe")
 *
 * First sight of an unknown graphics microcode (a pair of text id and data id): the record is derived from the text (derive.c, a few milliseconds). If the verdict is EXACT or NEAR the derived
 * build is tried, a few canned tasks per call, on the following graphics tasks: at the start of the game's own task the RSP state is saved, the OSTask is pointed at a canned display list
 * written to the tail of the RDP output ring, the build's front-end and the JIT each run the canned task from that state (in the sandbox: no RDP list reaches the video plugin, the RDRAM
 * writes are undone), the end states are compared, and then the state, the registers and the RDRAM are put back. The game's task runs on the JIT as it always did.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include "armlet.h"
#include "probe.h"

#ifdef ARMLET_NATIVE
#include "dbuild.h"
#include "f3dex_bld.h"
#include "f3d_bld.h"
#include "derive.h"
#include "hle.h"
#include "hle_f3dex2_int.h"
#include "hle_internal.h"
#include "native/native.h"
#include "native/native_internal.h"
#include "probe_cases.h"
#include "ucache.h"

#define PAIR_MAX 16
#define MAX_TASKS 128

enum { PS_NEW, PS_PROBING, PS_DONE };

struct pair {
    uint64_t tid, did;
    int state, outcome, latched, from_cache, force, experiment;
    int loaded;                         /* a text that a G_LOAD_UCODE loads in the middle of a task (S2DEX in Kirby 64's and Ocarina of Time's): tid is its load key, and the canned tasks start with it */
    uint32_t ta, da, ds;                /* the text, the data image and its size in the game's RDRAM */
    struct dbuild *b;
    struct derive_result dr;
    unsigned next, total;
    uint8_t kind[MAX_TASKS], seed[MAX_TASKS];
    unsigned n_equal, n_tol, n_declined, n_mismatch, n_inconclusive;
    unsigned done_kind[PROBE_NCASES], need_kind[PROBE_NCASES];
    double derive_ms, probe_ms;
    int f1;                             /* an F3DEX 1.x build: its own encoding of the cases (probe_case_make_f1) with these opcodes */
    struct probe_f1ops f1ops;
    int f3;                             /* an F3D build: its own encoding of the cases (probe_case_make_f3) */
    struct probe_f3ops f3ops;
    int rec_equal;
    const char *first_diff, *incon_why;
    char note[200];
    char first_report[600];
};

static struct {
    int init;
    struct pair pairs[PAIR_MAX];
    unsigned step;                       /* canned tasks per offer */
    int trace, check, force_cache, force_self;
    uint32_t std_sig;
    char why[96];
    struct { uint32_t text, data, len; } note[4];          /* G_LOAD_UCODEs the front-end saw since the last offer (armlet_probe_note_load; a task loads S2DEX and its own text back) */
    unsigned n_note;
} P;

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* test builds (scripts/build.sh native --tools --hmut=N): deliberate errors in the derived record, which the probe must catch (docs/testing.md) */
#ifdef ARMLET_HLE_MUTATE
#define PMUT(n, ok, bad) (ARMLET_HLE_MUTATE == (n) ? (bad) : (ok))
#else
#define PMUT(n, ok, bad) (ok)
#endif

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define SWZ 0
#else
#define SWZ 3
#endif

static void rd_bytes(const struct armlet_ctx *c, uint32_t addr, uint8_t *out, size_t n)
{
    const uint8_t *r = (const uint8_t *)c->rsp.RDRAM;
    size_t i;
    for (i = 0; i < n; i++)
        out[i] = r[((addr + (uint32_t)i) & 0x7fffff) ^ SWZ];
}

void armlet_probe_init(struct armlet_ctx *c)
{
    const char *path = c->cfg.ucode_cache;
    const char *e;
    memset(&P, 0, sizeof(P));
    if (!c->cfg.probe)
        return;
    P.init = 1;
    P.step = (e = getenv("ARMLET_PROBE_STEP")) && *e ? (unsigned)atoi(e) : 4;       /* (0: derive only, no canned task: a test aid) */
    P.trace = (e = getenv("ARMLET_PROBE_TRACE")) && *e && *e != '0';
    P.check = (e = getenv("ARMLET_PROBE_CHECK")) && *e && *e != '0';
    P.force_cache = (e = getenv("ARMLET_PROBE_FORCE_CACHE")) && *e && *e != '0';       /* tests: the forced (registered) ucodes use the cache too */
    P.force_self = (e = getenv("ARMLET_PROBE_FORCE_SELF")) && *e && *e != '0';         /* tests: a forced (registered) ucode is derived with its own record among the references (its record as registered, EXACT) */
    if (!strcmp(path, "off"))
        path = "";
    (void)ucache_open(path);
    armlet_log(M64MSG_INFO, "probe on: unknown graphics ucodes are derived and probed (cache %s%s%s%s)", ucache_path()[0] ? ucache_path() : "off", c->cfg.probe_latch ? ", latching a pass" : "",
               c->cfg.probe_force ? ", also the registered ucodes" : "", c->cfg.probe_tol ? ", near-match tolerance" : "");
}

void armlet_probe_reset(void)
{
    unsigned i;
    for (i = 0; i < PAIR_MAX; i++) {
        if (P.pairs[i].latched && P.pairs[i].b)
            armlet_hle_unregister_dynamic(&P.pairs[i].b->uc);
        free(P.pairs[i].b);
    }
    memset(P.pairs, 0, sizeof(P.pairs));
    ucache_close();
}

/* ---- the pair table ---------------------------------------------------------------------------------------------------------------------------------------------------------- */
static struct pair *find_pair(uint64_t tid, uint64_t did)
{
    unsigned i;
    for (i = 0; i < PAIR_MAX; i++)
        if (P.pairs[i].tid == tid && P.pairs[i].did == did && P.pairs[i].state)
            return &P.pairs[i];
    return NULL;
}

static struct pair *new_pair(uint64_t tid, uint64_t did)
{
    unsigned i;
    for (i = 0; i < PAIR_MAX; i++)
        if (!P.pairs[i].state) {
            memset(&P.pairs[i], 0, sizeof(P.pairs[i]));
            P.pairs[i].tid = tid;
            P.pairs[i].did = did;
            P.pairs[i].state = PS_NEW;
            P.pairs[i].rec_equal = -1;
            return &P.pairs[i];
        }
    return NULL;
}

int armlet_probe_summary(uint64_t text_id, struct probe_summary *o)
{
    unsigned i;
    for (i = 0; i < PAIR_MAX; i++) {
        const struct pair *p = &P.pairs[i];
        if (p->state && p->tid == text_id) {
            memset(o, 0, sizeof(*o));
            o->outcome = p->state == PS_DONE ? p->outcome : PROBE_OUT_PENDING;
            o->verdict = p->dr.verdict;
            o->family = p->dr.family;
            o->from_cache = p->from_cache;
            o->latched = p->latched;
            o->rec_equal_registered = p->rec_equal;
            o->first_diff = p->first_diff;
            o->n_tasks = p->total;
            o->n_equal = p->n_equal; o->n_tol = p->n_tol; o->n_declined = p->n_declined; o->n_mismatch = p->n_mismatch; o->n_inconclusive = p->n_inconclusive;
            o->derive_ms = p->derive_ms;
            o->probe_ms = p->probe_ms;
            snprintf(o->note, sizeof(o->note), "%s", p->note);
            return 0;
        }
    }
    return -1;
}

/* ---- the schedule ------------------------------------------------------------------------------------------------------------------------------------------------------------ */
/* An F3DEX 1.x build's opcodes of the commands whose numbers differ between the builds: the class-2 opcode (0xaf .. 0xbf) whose cell of the build's handler table (in its data image) leads to the
 * record's pc; a record pc no opcode leads to leaves the command out of the cases (the front-end would decline it). The other commands have the GBI's numbers. */
static void f1_ops(struct armlet_ctx *c, struct pair *p)
{
    const struct f3dex1_build *b = (const struct f3dex1_build *)p->b->uc.fam_data;
    const struct f3dex_layout *L = f3dex_lay(&b->fx);
    static uint8_t data[0x1000];
    static const int which[6] = { F1PC_half1, F1PC_half2, F1PC_branchz, F1PC_modvtx, F1PC_persp, F1PC_quad };
    uint8_t *dst[6];
    unsigned k, op, n = p->ds < sizeof(data) ? p->ds : sizeof(data);
    memset(&p->f1ops, 0, sizeof(p->f1ops));
    dst[0] = &p->f1ops.half1; dst[1] = &p->f1ops.half2; dst[2] = &p->f1ops.branchz; dst[3] = &p->f1ops.modvtx; dst[4] = &p->f1ops.persp; dst[5] = &p->f1ops.quad;
    rd_bytes(c, p->da, data, n);
    for (k = 0; k < 6; k++) {
        unsigned pc = b->pc[which[k]];
        for (op = 0xaf; op <= 0xbf && pc; op++) {
            unsigned a = L->jt2 + 2 * (op & 0x7f);
            if (a + 2 <= n && ((data[a] << 8 | data[a + 1]) & 0xffc) == pc) {
                *dst[k] = (uint8_t)op;
                break;
            }
        }
    }
    p->f1ops.culldl_off = b->culldl_off;
    p->f1 = 1;
}

/* An F3D build's encodings: the class-2 opcodes (0xb0 .. 0xbf) whose cells of the build's table (in its data image) lead to the record's pcs of the commands whose numbers differ between the builds
 * (the RDPHALF commands, G_PERSPNORMALIZE, Wave Race's quad, GoldenEye's G_TRI4), and from the record the G_VTX encoding, the triangle index scale and the vertex slots */
static void f3_ops(struct armlet_ctx *c, struct pair *p)
{
    const struct f3d_build *b = (const struct f3d_build *)p->b->uc.fam_data;
    static uint8_t data[0x1000];
    static const int which[5] = { F3PC_half1, F3PC_half2, F3PC_persp, F3PC_quad, F3PC_tri4 };
    uint8_t *dst[5];
    unsigned k, op, n = p->ds < sizeof(data) ? p->ds : sizeof(data);
    memset(&p->f3ops, 0, sizeof(p->f3ops));
    dst[0] = &p->f3ops.half1; dst[1] = &p->f3ops.half2; dst[2] = &p->f3ops.persp; dst[3] = &p->f3ops.quad; dst[4] = &p->f3ops.tri4;
    rd_bytes(c, p->da, data, n);
    for (k = 0; k < 5; k++) {
        unsigned pc = b->pc[which[k]];
        for (op = 0xb0; op <= 0xbf && pc; op++) {
            unsigned a = b->lay->jt2 + 2 * (op - 0x80);
            if (a + 2 <= n && ((data[a] << 8 | data[a + 1]) & 0xfff) == pc) {
                *dst[k] = (uint8_t)op;
                break;
            }
        }
    }
    p->f3ops.vtx_wr = b->vtx_wr;
    p->f3ops.vptr_mul = b->vptr_mul == 8 ? 8 : 4;
    p->f3ops.nslots = (uint8_t)(f3d_nslots(b->lay) > 32 ? 32 : f3d_nslots(b->lay) < 4 ? 4 : f3d_nslots(b->lay));
    p->f3ops.flat = !b->tri_wr;
    p->f3 = 1;
}

static void schedule(struct pair *p, unsigned seeds)
{
    static const unsigned random_kinds[] = { PROBE_CLIP, PROBE_ZNEG, PROBE_TRI, PROBE_MIX, PROBE_WILD, PROBE_MISC, PROBE_LIT, PROBE_MTX };
    static const unsigned s2_kinds[] = { PROBE_S2RECT, PROBE_S2LDTX, PROBE_S2BG, PROBE_S2COPY, PROBE_S2MISC };
    const int s2 = p->b && p->b->uc.fam == &f3dex2_fam && p->b->uc.fam_data && ((const struct f3dex2_build *)p->b->uc.fam_data)->ext;    /* a build with dialect commands (S2DEX 2.x): its own cases; it has no vertex or triangle code */
    const unsigned *kinds = s2 ? s2_kinds : random_kinds, nk = s2 ? 5 : 8;
    unsigned k, s, n = 0;
    for (s = 0; s < (s2 ? 2 * seeds : seeds); s++)
        for (k = 0; k < nk; k++)
            if (n < MAX_TASKS) {
                p->kind[n] = (uint8_t)kinds[k];
                p->seed[n++] = (uint8_t)s;
            }
    for (s = 0; s < 2 && !s2; s++) {                           /* the targeted cases: the reject mask and G_BRANCH_Z (F3DEX 1.x: when the build has it; F3D has none), two variants each */
        p->kind[n] = PROBE_REJ; p->seed[n++] = (uint8_t)s;
        if (!p->f3 && (!p->f1 || p->f1ops.branchz)) {
            p->kind[n] = PROBE_BZ; p->seed[n++] = (uint8_t)s;
        }
    }
    p->total = n;
    memset(p->need_kind, 0, sizeof(p->need_kind));
    for (k = 0; k < n; k++)
        p->need_kind[p->kind[k]]++;
}

/* the near-match tolerance's DMEM part: the colour word of a vertex record (+0x10 .. +0x13 of the slot the table at lay.vtab names) may differ by 2 levels */
static int near_dmem_ok(const struct hle_ucode *u, const uint32_t *dmem, unsigned addr, unsigned na, unsigned nb)
{
    const struct f3dex2_build *b;
    unsigned i;
    int d = (int)na - (int)nb;
    if (u->fam != &f3dex2_fam || d > 2 || d < -2 || !u->fam_data)
        return 0;
    b = (const struct f3dex2_build *)u->fam_data;
    if (!b->lay)
        return 0;
    for (i = 0; i < 32; i++) {
        unsigned a = (b->lay->vtab + 2 * i) & 0xfff;
        unsigned slot = ((const hle_u16a *)dmem)[(a ^ 2) >> 1] & 0xfff;
        if (addr - (slot + 0x10) < 4)
            return 1;
    }
    return 0;
}

#ifdef ARMLET_HLE_MUTATE
/* test builds: the colour of the first vertex slot in the native end state is changed by 1 (352: within the tolerance) or 3 (353: outside it) */
static void perturb_dmem(const struct hle_ucode *u, uint32_t *dmem)
{
    const struct f3dex2_build *b = (const struct f3dex2_build *)u->fam_data;
    unsigned slot = ((const hle_u16a *)dmem)[(b->lay->vtab ^ 2) >> 1] & 0xfff;
    ((uint8_t *)dmem)[(slot + 0x10) ^ 3] += ARMLET_HLE_MUTATE == 352 ? 1 : 3;
}
#endif

/* ---- one canned task -------------------------------------------------------------------------------------------------------------------------------------------------------- */
static PrspNativeSnap g_real, g_pb;

/* The canned task's RDRAM: an 8 MiB anonymous mapping of its own (pages that nobody writes cost nothing), into which the ucode's text, its overlays and its data segment are copied from the
 * game's RDRAM at the addresses the OSTask names; the ring, the matrix stack and the canned data go to addresses that do not collide with them. The game's RDRAM is read, never written. */
static uint32_t *g_priv;

static uint32_t *priv_rdram(void)
{
    if (!g_priv) {
        void *m = mmap(NULL, 0x800000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        g_priv = m == MAP_FAILED ? NULL : (uint32_t *)m;
    }
    return g_priv;
}

static void priv_put(uint32_t addr, const uint8_t *in, size_t n)
{
    uint8_t *r = (uint8_t *)g_priv;
    size_t i;
    for (i = 0; i < n; i++)
        r[((addr + (uint32_t)i) & 0x7fffff) ^ SWZ] = in[i];
}

/* returns HLE_PROBE_*; fills out. The state is as it was when this was called, whatever happens. */
static int run_canned(struct armlet_ctx *c, struct pair *p, unsigned kind, unsigned seed, struct hle_probe_out *out)
{
    static struct probe_case cs;
    static uint8_t buf[0x3000];
    static uint8_t *guard;                              /* ARMLET_PROBE_CHECK=1 (tests): a copy of the game's RDRAM, to show that a canned task does not change it */
    struct hle_probe_opts opts;
    uint32_t ta, da, ds, base, lo, hi, sp_pc;
    int st;

    memset(out, 0, sizeof(*out));
    if (!priv_rdram()) {
        out->status = HLE_PROBE_INCONCLUSIVE;
        out->why = "no memory for the canned tasks' RDRAM";
        return out->status;
    }
    prsp_native_save(&g_real);
    if (P.check) {
        if (!guard)
            guard = (uint8_t *)malloc(0x800000);
        if (guard)
            memcpy(guard, c->rsp.RDRAM, 0x800000);
    }
    if (g_real.cr[11] & 1) {
        out->status = HLE_PROBE_INCONCLUSIVE;
        out->why = "the RDP reads its lists from DMEM (XBUS)";
        return out->status;
    }
    if (g_real.sp_status_timeout != 0x7fff) {
        out->status = HLE_PROBE_INCONCLUSIVE;
        out->why = "the JIT's status poll time-out has dropped";
        return out->status;
    }
    ta = p->ta;
    da = p->da;
    ds = p->ds;
    /* the ring, the canned data and the matrix stack: three blocks at the top of RDRAM, clear of the text with its overlays (3 x 4 KiB) and of the data */
    lo = 0x7a0000;
    hi = 0x7c0000;
    base = 0x7d0000;
    if ((ta + 0x3000 > lo && ta < 0x7e0000) || (da + ds > lo && da < 0x7e0000)) {
        lo = 0x300000;
        hi = 0x320000;
        base = 0x330000;
        if ((ta + 0x3000 > lo && ta < 0x340000) || (da + ds > lo && da < 0x340000)) {
            out->status = HLE_PROBE_INCONCLUSIVE;
            out->why = "the ucode's text or data lies where the canned tasks' ring would be";
            return out->status;
        }
    }
    /* the ucode's text, its overlays behind it and its data, as the game's RDRAM has them now */
    rd_bytes(c, ta, buf, 0x3000);
    priv_put(ta, buf, 0x3000);
    rd_bytes(c, da, buf, ds);
    if (p->loaded)
        memset(buf, 0, 0x180);                          /* (DMEM below 0x180: zeros, as the derivation saw it) */
    priv_put(da, buf, ds);
    if (p->f3)
        probe_case_make_f3(&cs, kind, seed, base, &p->f3ops);
    else if (p->f1)
        probe_case_make_f1(&cs, kind, seed, base, &p->f1ops);
    else
        probe_case_make(&cs, kind, seed, base);
    priv_put(base, cs.mem, PROBE_MEM);

    memcpy(&g_pb, &g_real, sizeof(g_pb));
    g_pb.dmem[0xfe0 >> 2] = base + 0x1200;              /* OSTask.dram_stack: the matrix stack of G_MTX with push, behind the case's data */
    g_pb.dmem[0xfe4 >> 2] = 0x400;
    g_pb.dmem[0xfe8 >> 2] = lo;                         /* OSTask.output_buff, output_buff_size: the ring */
    g_pb.dmem[0xfec >> 2] = hi;
    g_pb.dmem[0xff0 >> 2] = base;                       /* OSTask.data_ptr, data_size */
    g_pb.dmem[0xff4 >> 2] = cs.dl_bytes;
    g_pb.dmem[0xff8 >> 2] = base + 0x1600;              /* the yield area (the canned tasks do not yield) */
    g_pb.dmem[0xfc4 >> 2] = 0;                          /* flags: no yield */
    g_pb.dmem[0xfd0 >> 2] = ta;                         /* OSTask.ucode, ucode_data, ucode_data_size: the game's task's own, or the loaded text with the data image its G_LOAD_UCODE named */
    g_pb.dmem[0xfd8 >> 2] = da;
    g_pb.dmem[0xfdc >> 2] = ds;
    g_pb.cr[4] &= ~3u;                                  /* SP_STATUS: not halted, not broken */
    g_pb.cr[2] = g_pb.cr[3] = 0xff8;                    /* the DMA length registers, as any earlier task leaves them (the lean front-end writes the ring without a DMA and leaves them as they are) */
    g_pb.pc = *c->rsp.SP_PC_REG & 0xfff;                /* the pc the task starts at (the front-end reads the JIT's pc field, the JIT SP_PC) */
    g_pb.has_delay_slot = 0;
    g_pb.branch_target = 0;
    g_pb.sp_status_timeout = 0x7fff;
    memset(g_pb.mfc0_count, 0, sizeof(g_pb.mfc0_count));
    memset(&opts, 0, sizeof(opts));
    opts.tol = c->cfg.probe_tol;
    opts.dmem_ok = c->cfg.probe_tol ? near_dmem_ok : NULL;
#ifdef ARMLET_HLE_MUTATE
    if (ARMLET_HLE_MUTATE == 352 || ARMLET_HLE_MUTATE == 353)
        opts.perturb = perturb_dmem;
#endif

    sp_pc = *c->rsp.SP_PC_REG;
    st = armlet_hle_probe_pair(&p->b->uc, &g_pb, &opts, lo, hi, base + 0x1200, base + 0x1600, g_priv, out);
    prsp_native_load(&g_real);                          /* the state, the registers, MI_INTR and the status poll bookkeeping as they were; the JIT drops what no longer matches IMEM */
    prsp_native_resync();
    *c->rsp.SP_PC_REG = sp_pc;
    if (P.check && guard && memcmp(guard, c->rsp.RDRAM, 0x800000))
        armlet_log(M64MSG_ERROR, "probe: a canned task changed the game's RDRAM");
    if (P.trace) {                                      /* (development: the restore must give back every byte) */
        static PrspNativeSnap chk;
        prsp_native_save(&chk);
        if (memcmp(chk.dmem, g_real.dmem, sizeof(chk.dmem)) || memcmp(chk.imem, g_real.imem, sizeof(chk.imem)) || memcmp(chk.cr, g_real.cr, sizeof(chk.cr)) || chk.irq != g_real.irq)
            armlet_log(M64MSG_ERROR, "probe: the state after a canned task differs from the one before it");
    }
    /* (what the canned task wrote to the private RDRAM was undone by the sandbox; the next task rewrites the text, data and case it needs) */
    return st;
}

static void finish(struct armlet_ctx *c, struct pair *p);

static void probe_steps(struct armlet_ctx *c, struct pair *p, unsigned max)
{
    unsigned n;
    uint64_t t0 = now_ns();
    if (!p->b)
        return;
    for (n = 0; n < max && p->next < p->total; n++) {
        struct hle_probe_out o;
        unsigned i = p->next++, kind = p->kind[i];
        int st = run_canned(c, p, kind, p->seed[i], &o);
        switch (st) {
        case HLE_PROBE_EQUAL:
            p->n_equal++;
            p->done_kind[kind]++;
            break;
        case HLE_PROBE_TOLERANT:
            p->n_tol++;
            p->done_kind[kind]++;
            break;
        case HLE_PROBE_DECLINED:
            if (o.why && strstr(o.why, "status poll timeout has dropped")) {
                p->n_inconclusive++;
                p->incon_why = o.why;
            }
            else {
                p->n_declined++;
                if (P.trace)
                    armlet_log(M64MSG_INFO, "probe: %016" PRIx64 " %s/%u declined: %s", p->tid, probe_case_names[kind], p->seed[i], o.why ? o.why : "?");
            }
            break;
        case HLE_PROBE_MISMATCH:
            p->n_mismatch++;
            if (!p->first_report[0])
                snprintf(p->first_report, sizeof(p->first_report), "%s/%u:\n%.480s", probe_case_names[kind], p->seed[i], o.report);
            break;
        default:
            p->n_inconclusive++;
            p->incon_why = o.why;
            break;
        }
        if (P.trace && st == HLE_PROBE_MISMATCH)
            armlet_log(M64MSG_INFO, "probe: %016" PRIx64 " %s/%u:\n%.700s", p->tid, probe_case_names[kind], p->seed[i], o.report);
        if (P.trace)
            armlet_log(M64MSG_INFO, "probe: %016" PRIx64 " %s/%u -> %s (native %.0f us, JIT %.0f us, %" PRIu64 " stream bytes)", p->tid, probe_case_names[kind], p->seed[i],
                       st == HLE_PROBE_EQUAL ? "equal" : st == HLE_PROBE_TOLERANT ? "equal within the tolerance" : st == HLE_PROBE_DECLINED ? "declined" : st == HLE_PROBE_MISMATCH ? "DIFFERS" : "inconclusive",
                       o.native_us, o.jit_us, o.stream_len);
    }
    p->probe_ms += (double)(now_ns() - t0) / 1e6;
    if (p->next >= p->total)
        finish(c, p);
}

/* the pass rule (docs/design.md, "The probe"): no task differs; most of the conclusive ones completed natively; every case kind completed at least once; the two targeted cases completed in every variant */
static int evaluate(struct pair *p, char *why, size_t whysz)
{
    unsigned conclusive = p->n_equal + p->n_tol + p->n_declined + p->n_mismatch, done = p->n_equal + p->n_tol, k;
    if (p->n_mismatch) {
        snprintf(why, whysz, "%u of the canned tasks differ from the JIT", p->n_mismatch);
        return PROBE_OUT_FAIL;
    }
    if (conclusive * 10 < p->total * 6) {
        snprintf(why, whysz, "only %u of %u canned tasks were conclusive (%s)", conclusive, p->total, p->incon_why ? p->incon_why : "the JIT or the ring did not allow the rest");
        return PROBE_OUT_INCONCLUSIVE;
    }
    if (done * 4 < conclusive * 3) {
        snprintf(why, whysz, "the build declined %u of %u canned tasks", p->n_declined, conclusive);
        return PROBE_OUT_FAIL;
    }
    for (k = 0; k < PROBE_NCASES; k++) {
        unsigned need = (k == PROBE_REJ || k == PROBE_BZ) ? p->need_kind[k] : 1;
        if (p->need_kind[k] && p->done_kind[k] < need) {
            snprintf(why, whysz, "no canned task of the case \"%s\" completed natively", probe_case_names[k]);
            return PROBE_OUT_FAIL;
        }
    }
    snprintf(why, whysz, "%u canned tasks equal%s", done, p->n_tol ? " (some within the tolerance)" : "");
    return p->n_tol ? PROBE_OUT_PASS_TOL : PROBE_OUT_PASS;
}

static void store_entry(struct pair *p, int probe_result)
{
    struct ucache_entry e;
    memset(&e, 0, sizeof(e));
    e.text_id = p->tid;
    e.data_id = p->did;
    e.verdict = (uint8_t)p->dr.verdict;
    e.family = (uint8_t)p->dr.family;
    e.probe = (uint8_t)probe_result;
    e.primary = (uint16_t)p->dr.primary;
    e.n_equal = (uint16_t)p->n_equal; e.n_tol = (uint16_t)p->n_tol; e.n_declined = (uint16_t)p->n_declined; e.n_mismatch = (uint16_t)p->n_mismatch; e.n_inconclusive = (uint16_t)p->n_inconclusive;
    ucache_from_rec(&e, &p->dr.rec);
    (void)ucache_store(&e);
}

static void latch(struct armlet_ctx *c, struct pair *p)
{
    if (!c->cfg.probe_latch || !p->b || p->latched)
        return;
    if (armlet_hle_register_dynamic(&p->b->uc) == 0) {
        p->latched = 1;
        armlet_log(M64MSG_INFO, "probe: ucode %016" PRIx64 " latched as \"%s\"", p->tid, p->b->name);
    }
}

static void finish(struct armlet_ctx *c, struct pair *p)
{
    char why[200];
    int out = evaluate(p, why, sizeof(why)), pr;
    p->state = PS_DONE;
    p->outcome = out;
    snprintf(p->note, sizeof(p->note), "%s", why);
    armlet_log(M64MSG_INFO, "probe: ucode %016" PRIx64 " data %016" PRIx64 ": %u canned tasks: %u equal, %u within the tolerance, %u declined, %u differ, %u inconclusive -> %s (%s; %.1f ms)", p->tid, p->did,
               p->total, p->n_equal, p->n_tol, p->n_declined, p->n_mismatch, p->n_inconclusive, out == PROBE_OUT_PASS ? "PASS" : out == PROBE_OUT_PASS_TOL ? "PASS (tolerant)" : out == PROBE_OUT_FAIL ? "FAIL" : "inconclusive",
               why, p->probe_ms);
    if (out == PROBE_OUT_FAIL && p->first_report[0])
        armlet_log(M64MSG_INFO, "probe: first difference, case %s", p->first_report);
    pr = out == PROBE_OUT_PASS ? UC_PROBE_PASS : out == PROBE_OUT_PASS_TOL ? UC_PROBE_PASS_TOL : out == PROBE_OUT_FAIL ? UC_PROBE_FAIL : UC_PROBE_NONE;
    if ((!p->force || P.force_cache) && !p->experiment && out != PROBE_OUT_INCONCLUSIVE)
        store_entry(p, pr);
    if (out == PROBE_OUT_PASS || out == PROBE_OUT_PASS_TOL)
        latch(c, p);
    else if (!p->latched) {
        free(p->b);
        p->b = NULL;
    }
}

/* ---- first sight --------------------------------------------------------------------------------------------------------------------------------------------------------------- */
static void start_probe(struct armlet_ctx *c, struct pair *p, const char *src)
{
    unsigned seeds = c->cfg.probe_seeds ? c->cfg.probe_seeds : 3;
    if (p->b && p->b->uc.fam == &f3dex1_fam)
        f1_ops(c, p);
    else if (p->b && p->b->uc.fam == &f3d_fam)
        f3_ops(c, p);
    schedule(p, seeds);
    p->state = PS_PROBING;
    armlet_log(M64MSG_INFO, "probe: ucode %016" PRIx64 " data %016" PRIx64 ": derived %s (%s, primary %s, %.1f ms), probing with %u canned tasks, %u per graphics task", p->tid, p->did,
               derive_verdict_name(p->dr.verdict), src, derive_refs[p->dr.primary].name, p->derive_ms, p->total, P.step);
}

static void first_sight(struct armlet_ctx *c, struct pair *p, const struct armlet_ucode_id *u, uint64_t did, uint32_t da, unsigned ds)
{
    uint32_t text[1024], ta = p->ta, i;
    static uint8_t data[0x1000], ext[0x2000];
    struct derive_in in;
    const struct ucache_entry *ce = ucache_find(p->tid, did);
    uint64_t t0;
    const char *why = NULL;

    (void)u;
    for (i = 0; i < 1024; i++)
        text[i] = *(const uint32_t *)((const uint8_t *)c->rsp.RDRAM + ((ta + 4 * i) & 0x7ffffc));
    p->dr.family = DERIVE_F3DEX2;
    if (ce && (!p->force || P.force_cache)) {
        /* seen before (this tool version, these references): the verdict and the probe's result are in the cache */
        p->from_cache = 1;
        p->dr.verdict = ce->verdict;
        p->dr.family = ce->family;
        p->dr.primary = ce->primary;
        ucache_to_rec(ce, &p->dr.rec);
        p->n_equal = ce->n_equal; p->n_tol = ce->n_tol; p->n_declined = ce->n_declined; p->n_mismatch = ce->n_mismatch; p->n_inconclusive = ce->n_inconclusive;
        p->total = (unsigned)(ce->n_equal + ce->n_tol + ce->n_declined + ce->n_mismatch + ce->n_inconclusive);
        if (ce->verdict == DERIVE_NONE) {
            p->state = PS_DONE;
            p->outcome = PROBE_OUT_NO_CANDIDATE;
            snprintf(p->note, sizeof(p->note), "cached: the derivation says %s (%s)", derive_verdict_name(ce->verdict), derive_family_name(ce->family));
            armlet_log(M64MSG_INFO, "probe: ucode %016" PRIx64 " data %016" PRIx64 ": %s", p->tid, p->did, p->note);
            return;
        }
        if (ce->probe == UC_PROBE_FAIL || ce->probe == UC_PROBE_UNSUPPORTED) {
            p->state = PS_DONE;
            p->outcome = ce->probe == UC_PROBE_FAIL ? PROBE_OUT_FAIL : PROBE_OUT_UNSUPPORTED;
            snprintf(p->note, sizeof(p->note), "cached: the probe %s", ce->probe == UC_PROBE_FAIL ? "failed" : "found the build unsupported");
            armlet_log(M64MSG_INFO, "probe: ucode %016" PRIx64 " data %016" PRIx64 ": %s", p->tid, p->did, p->note);
            return;
        }
        if (p->force) {
            const struct hle_ucode *reg = dbuild_registered(p->dr.rec.load_key);
            if (reg) {
                struct derive_rec rr;
                dbuild_rec_of(reg, &rr);
                p->rec_equal = dbuild_rec_equal(&p->dr.rec, &rr, &p->first_diff);
            }
        }
        {
            struct dbuild *b = (struct dbuild *)calloc(1, sizeof(*b));
            if (!b)
                return;
            if (dbuild_make(b, p->tid, &p->dr, &why)) {
                free(b);
                p->state = PS_DONE;
                p->outcome = PROBE_OUT_UNSUPPORTED;
                snprintf(p->note, sizeof(p->note), "cached record unusable: %s", why);
                return;
            }
            p->b = b;
        }
        if (ce->probe == UC_PROBE_PASS || ce->probe == UC_PROBE_PASS_TOL) {
            p->state = PS_DONE;
            p->outcome = ce->probe == UC_PROBE_PASS ? PROBE_OUT_PASS : PROBE_OUT_PASS_TOL;
            snprintf(p->note, sizeof(p->note), "cached: the probe passed");
            armlet_log(M64MSG_INFO, "probe: ucode %016" PRIx64 " data %016" PRIx64 ": cached, derived %s, probe passed", p->tid, p->did, derive_verdict_name(ce->verdict));
            latch(c, p);
            return;
        }
        start_probe(c, p, "from the cache");
        return;
    }

    rd_bytes(c, da, data, ds);
    if (p->loaded)
        memset(data, 0, 0x180);                         /* a loaded text: DMEM below 0x180 is not loaded; zeros, as in the canned tasks */
    {
        size_t el = 0x2000;
        if (ta + 0x3000 > 0x800000)
            el = ta + 0x1000 < 0x800000 ? 0x800000 - (ta + 0x1000) : 0;
        rd_bytes(c, ta + 0x1000, ext, el);
        memset(&in, 0, sizeof(in));
        in.text = text;
        in.data = data;
        in.data_len = ds;
        in.ext = ext;
        in.ext_len = el;
        in.family = DERIVE_AUTO;
        if (p->force && !P.force_self) {                /* leave the registered build's own record out of the references: its record must come out as derived */
            uint64_t lk = derive_fnv_words(text, 0xf48 / 4);
            unsigned r;
            for (r = 0; r < derive_n_recs; r++)
                if (derive_recs[r].load_key == lk)
                    in.exclude_recs |= 1ull << r;
        }
        t0 = now_ns();
        if (derive_run(&in, &p->dr)) {
            p->state = PS_DONE;
            p->outcome = PROBE_OUT_NO_CANDIDATE;
            snprintf(p->note, sizeof(p->note), "the derivation failed (no memory?)");
            return;
        }
        p->derive_ms = (double)(now_ns() - t0) / 1e6;
    }
    if (p->dr.verdict == DERIVE_NONE && c->cfg.probe_none_x && p->dr.n_x + p->dr.n_xr[2] <= c->cfg.probe_none_x) {
        p->experiment = 1;                              /* an experiment (ARMLET_PROBE_NONE_X): the record is as derived, the verdict stays NONE, nothing is cached */
        armlet_log(M64MSG_INFO, "probe: ucode %016" PRIx64 ": the derivation says NONE (%u unexplained, %u missing words); probing it anyway (ARMLET_PROBE_NONE_X)", p->tid, p->dr.n_x, p->dr.n_xr[2]);
    } else if (p->dr.verdict == DERIVE_NONE) {
        p->state = PS_DONE;
        p->outcome = PROBE_OUT_NO_CANDIDATE;
        snprintf(p->note, sizeof(p->note), "the derivation says %s (%s family, %u of %u words aligned)", derive_verdict_name(p->dr.verdict), derive_family_name(p->dr.family), p->dr.aligned_num, p->dr.aligned_den);
        armlet_log(M64MSG_INFO, "probe: ucode %016" PRIx64 " data %016" PRIx64 ": %s (%.1f ms); no probe", p->tid, p->did, p->note, p->derive_ms);
        if (!p->force || P.force_cache) {
            p->dr.primary = p->dr.primary < 0 ? 0 : p->dr.primary;
            store_entry(p, UC_PROBE_NONE);
        }
        return;
    }
    {
        struct dbuild *b = (struct dbuild *)calloc(1, sizeof(*b));
        if (!b)
            return;
        /* test builds: deliberate errors in the record (the probe has to fail) */
        p->dr.rec.reject_mask = PMUT(340, p->dr.rec.reject_mask, p->dr.rec.reject_mask ^ 0xc0);
        p->dr.rec.bz_z = (uint8_t)PMUT(341, p->dr.rec.bz_z, !p->dr.rec.bz_z);
        p->dr.rec.pc[PC_TRI1] = PMUT(342, p->dr.rec.pc[PC_TRI1], p->dr.rec.pc[PC_TRI1] + 8);
        p->dr.rec.zex = (uint8_t)PMUT(343, p->dr.rec.zex, !p->dr.rec.zex);
        p->dr.rec.lay[LY_OUT_BYTES] = PMUT(344, p->dr.rec.lay[LY_OUT_BYTES], p->dr.rec.lay[LY_OUT_BYTES] - 8);
        /* (S2DEX: the continuation pc of LDTX_RECT, the output buffer size, the pipe sync of the Safe build, the handlers from 0x184 on 8 bytes lower as in the 2.06 text) */
        p->dr.rec.pc[PC_LDTX_RECT] = PMUT(400, p->dr.rec.pc[PC_LDTX_RECT], p->dr.rec.pc[PC_LDTX_RECT] + 8);
        p->dr.rec.lay[LY_OUT_BYTES] = PMUT(401, p->dr.rec.lay[LY_OUT_BYTES], p->dr.rec.lay[LY_OUT_BYTES] + 8);
        p->dr.rec.lay[LY_PIPE_SYNC] = PMUT(402, p->dr.rec.lay[LY_PIPE_SYNC], !p->dr.rec.lay[LY_PIPE_SYNC]);
        for (i = 0; i < p->dr.rec.n_ext; i++)
            if (p->dr.rec.ext[i].pc >= 0x184)
                p->dr.rec.ext[i].pc = PMUT(403, p->dr.rec.ext[i].pc, p->dr.rec.ext[i].pc - 8);
        if (p->dr.family == DERIVE_F3DEX1) {          /* (F3DEX 1.x: the near-plane bits of the reject mask, the NoN and LX attributes, G_TRI1's pc + 8, the vertex table 2 lower (the 0.95 place), the G_CULLDL form) */
            p->dr.rec.reject_mask = PMUT(404, p->dr.rec.reject_mask, p->dr.rec.reject_mask ^ 0x40);
            p->dr.rec.non = (uint8_t)PMUT(405, p->dr.rec.non, !p->dr.rec.non);
            p->dr.rec.f1pc[F1PC_tri1] = PMUT(406, p->dr.rec.f1pc[F1PC_tri1], p->dr.rec.f1pc[F1PC_tri1] + 8);
            p->dr.rec.lay[18] = PMUT(407, p->dr.rec.lay[18], p->dr.rec.lay[18] - 2);
            p->dr.rec.dlx = (uint8_t)PMUT(408, p->dr.rec.dlx, !p->dr.rec.dlx);
            p->dr.rec.culldl_off = (uint8_t)PMUT(409, p->dr.rec.culldl_off, !p->dr.rec.culldl_off);
        }
        if (p->dr.family == DERIVE_F3D) {             /* (F3D: the near-plane bits of the reject mask, Pilotwings' clip flag word, G_TRI1's pc + 8, the class-2 table 2 lower (the 2.0H place), the triangle index
                                                         scale, the G_CULLDL mask, the stale flag of G_POPMTX, the G_MOVEWORD table 2 lower, Wave Race's triangle code, the loop form) */
            p->dr.rec.reject_mask = PMUT(410, p->dr.rec.reject_mask, p->dr.rec.reject_mask ^ 0x40);
            p->dr.rec.f3[F3F_FV] = (uint8_t)PMUT(411, p->dr.rec.f3[F3F_FV], !p->dr.rec.f3[F3F_FV]);
            p->dr.rec.f3pc[F3PC_tri1] = PMUT(412, p->dr.rec.f3pc[F3PC_tri1], p->dr.rec.f3pc[F3PC_tri1] + 8);
            p->dr.rec.lay[DERIVE_NF3KNOB + 1] = PMUT(413, p->dr.rec.lay[DERIVE_NF3KNOB + 1], p->dr.rec.lay[DERIVE_NF3KNOB + 1] - 2);
            p->dr.rec.f3[F3F_VPTR_MUL] = (uint8_t)PMUT(414, p->dr.rec.f3[F3F_VPTR_MUL], p->dr.rec.f3[F3F_VPTR_MUL] ^ 12);
            p->dr.rec.culldl_mask = PMUT(415, p->dr.rec.culldl_mask, p->dr.rec.culldl_mask ^ 0x8f8f);
            p->dr.rec.f3[F3F_POPMTX_STALE] = (uint8_t)PMUT(416, p->dr.rec.f3[F3F_POPMTX_STALE], !p->dr.rec.f3[F3F_POPMTX_STALE]);
            p->dr.rec.lay[14] = PMUT(417, p->dr.rec.lay[14], p->dr.rec.lay[14] - 2);
            p->dr.rec.f3[F3F_TRI_WR] = (uint8_t)PMUT(418, p->dr.rec.f3[F3F_TRI_WR], !p->dr.rec.f3[F3F_TRI_WR]);
            p->dr.rec.f3[F3F_GP_END] = (uint8_t)PMUT(419, p->dr.rec.f3[F3F_GP_END], !p->dr.rec.f3[F3F_GP_END]);
            if (p->dr.rec.lay[DERIVE_NF3KNOB + 2])      /* (G_TRI4's table one byte off: GoldenEye) */
                p->dr.rec.lay[DERIVE_NF3KNOB + 2] = PMUT(420, p->dr.rec.lay[DERIVE_NF3KNOB + 2], p->dr.rec.lay[DERIVE_NF3KNOB + 2] + 1);
        }
        if (p->force) {
            const struct hle_ucode *reg = dbuild_registered(p->dr.rec.load_key);
            if (reg) {
                struct derive_rec rr;
                dbuild_rec_of(reg, &rr);
                p->rec_equal = dbuild_rec_equal(&p->dr.rec, &rr, &p->first_diff);
                armlet_log(M64MSG_INFO, "probe: ucode %016" PRIx64 ": the derived record %s the registered one (%s)%s%s", p->tid, p->rec_equal ? "equals" : "DIFFERS from", reg->name,
                           p->rec_equal ? "" : ", first field: ", p->rec_equal ? "" : p->first_diff);
            }
        }
        if (dbuild_make(b, p->tid, &p->dr, &why)) {
            free(b);
            p->state = PS_DONE;
            p->outcome = PROBE_OUT_UNSUPPORTED;
            snprintf(p->note, sizeof(p->note), "derived %s, but no build can be made of it: %s", derive_verdict_name(p->dr.verdict), why);
            armlet_log(M64MSG_INFO, "probe: ucode %016" PRIx64 " data %016" PRIx64 ": %s; no probe", p->tid, p->did, p->note);
            if ((!p->force || P.force_cache) && !p->experiment)
                store_entry(p, UC_PROBE_UNSUPPORTED);
            return;
        }
        p->b = b;
    }
    start_probe(c, p, "derived");
}

void armlet_probe_note_load(uint32_t text, uint32_t data, uint32_t len)
{
    unsigned i;
    if (!P.init)
        return;
    text &= 0x7ffff8;
    data &= 0x7ffff8;
    for (i = 0; i < P.n_note; i++)
        if (P.note[i].text == text && P.note[i].data == data)
            return;
    if (P.n_note < sizeof(P.note) / sizeof(P.note[0])) {
        P.note[P.n_note].text = text;
        P.note[P.n_note].data = data;
        P.note[P.n_note++].len = len;
    }
}

/* A text loaded by G_LOAD_UCODE (armlet_probe_note_load): a pair of its own, keyed by its load key (the identity loaded_build looks a build up by) and its data image. Registered: only with ARMLET_PROBE_FORCE.
 * Returns 1 when it derived a new pair (enough work for this graphics task). */
static int offer_loaded(struct armlet_ctx *c, uint32_t ta, uint32_t da, uint32_t ds)
{
    uint64_t key, did;
    struct pair *p;
    int registered;
    if (!ds || ds > 0x1000)
        ds = ds ? 0x1000 : 0x800;
    ds = (ds + 3) & ~3u;
    if (ta + 0x1000 > 0x800000 || da + ds > 0x800000)
        return 0;
    key = armlet_ucode_hash((const unsigned char *)c->rsp.RDRAM, ta, 0xf48);
    registered = dbuild_registered(key) != NULL;
    if (registered && !c->cfg.probe_force)
        return 0;
    {
        unsigned i;
        for (i = 0; i < PAIR_MAX; i++)                  /* one pair per loaded text: its data image is not what names it (the bytes in front of the loaded part change from task to task in Kirby 64), and a latched loaded build is not bound to one */
            if (P.pairs[i].state && P.pairs[i].loaded && P.pairs[i].tid == key)
                return 0;
    }
    did = armlet_ucode_hash((const unsigned char *)c->rsp.RDRAM, da + 0x180, ds > 0x180 ? ds - 0x180 : 0);   /* (the part the command loads) */
    p = new_pair(key, did);
    if (!p)
        return 0;
    p->force = registered;
    p->loaded = 1;
    p->ta = ta;
    p->da = da;
    p->ds = ds;
    armlet_log(M64MSG_INFO, "probe: ucode %016" PRIx64 " (loaded by G_LOAD_UCODE from %08x, data %08x)", key, ta, da);
    first_sight(c, p, NULL, did, da, ds);
    return 1;
}

void armlet_probe_offer(struct armlet_ctx *c, const struct armlet_ucode_id *u)
{
    uint32_t da, ds;
    uint64_t did;
    struct pair *p;
    int registered;
    unsigned i;

    if (!P.init)
        return;
    while (P.n_note) {                                  /* (one derivation per graphics task; the notes of the same texts come again with the next tasks) */
        P.n_note--;
        if (offer_loaded(c, P.note[P.n_note].text, P.note[P.n_note].data, P.note[P.n_note].len)) {
            P.n_note = 0;
            return;
        }
    }
    for (i = 0; i < PAIR_MAX; i++)                      /* the loaded texts' canned tasks run on whatever graphics task comes, before the task's own */
        if (P.pairs[i].loaded && P.pairs[i].state == PS_PROBING && P.step) {
            probe_steps(c, &P.pairs[i], P.step);
            return;
        }
    registered = armlet_hle_find(u->id) != NULL;
    if (registered && !c->cfg.probe_force)
        return;
    ds = armlet_dmem32(c, ARMLET_TASK_UCODE_DATA_SIZE);
    if (!ds || ds > 0x1000)
        ds = ds ? 0x1000 : 0x800;
    ds = (ds + 3) & ~3u;
    da = armlet_dmem32(c, ARMLET_TASK_UCODE_DATA) & 0x7ffffc;
    if (da + ds > 0x800000)
        return;
    did = armlet_ucode_hash((const unsigned char *)c->rsp.RDRAM, da, ds);
    p = find_pair(u->id, did);
    if (!p) {
        p = new_pair(u->id, did);
        if (!p)
            return;                                     /* (more pairs than the table holds: the rest are not probed) */
        p->force = registered;
        p->ta = armlet_dmem32(c, ARMLET_TASK_UCODE) & 0x7ffffc;
        p->da = da;
        p->ds = ds;
        first_sight(c, p, u, did, da, ds);
        return;                                         /* (the first canned tasks run on the next graphics task: the derivation took long enough for this one) */
    }
    if (p->state == PS_PROBING && P.step)
        probe_steps(c, p, P.step);
}

int armlet_probe_run_registered(struct armlet_ctx *c, const struct hle_ucode *u)
{
    static struct pair p;
    struct dbuild *b = (struct dbuild *)calloc(1, sizeof(*b));
    unsigned i, bad = 0;
    if (!b)
        return -1;
    b->uc = *u;
    memset(&p, 0, sizeof(p));
    p.b = b;
    p.tid = u->id;
    p.ta = armlet_dmem32(c, ARMLET_TASK_UCODE) & 0x7ffffc;
    p.da = armlet_dmem32(c, ARMLET_TASK_UCODE_DATA) & 0x7ffffc;
    p.ds = armlet_dmem32(c, ARMLET_TASK_UCODE_DATA_SIZE);
    if (!p.ds || p.ds > 0x1000)
        p.ds = p.ds ? 0x1000 : 0x800;
    if (u->fam == &f3dex1_fam)
        f1_ops(c, &p);
    else if (u->fam == &f3d_fam)
        f3_ops(c, &p);
    schedule(&p, c->cfg.probe_seeds ? c->cfg.probe_seeds : 3);
    for (i = 0; i < p.total; i++) {
        struct hle_probe_out o;
        int st = run_canned(c, &p, p.kind[i], p.seed[i], &o);
        if (st == HLE_PROBE_MISMATCH)
            bad++;
    }
    free(b);
    return (int)bad;
}

bool armlet_probe_gate(struct armlet_ctx *c, const struct armlet_ucode_id *u)
{
    unsigned i;
    uint32_t da, ds;
    uint64_t did;
    if (!P.init)
        return true;
    for (i = 0; i < PAIR_MAX; i++)
        if (P.pairs[i].latched && P.pairs[i].tid == u->id)
            break;
    if (i == PAIR_MAX)
        return true;
    ds = armlet_dmem32(c, ARMLET_TASK_UCODE_DATA_SIZE);
    if (!ds || ds > 0x1000)
        ds = ds ? 0x1000 : 0x800;
    ds = (ds + 3) & ~3u;
    da = armlet_dmem32(c, ARMLET_TASK_UCODE_DATA) & 0x7ffffc;
    if (da + ds > 0x800000)
        return false;
    did = armlet_ucode_hash((const unsigned char *)c->rsp.RDRAM, da, ds);
    return did == P.pairs[i].did;
}

#endif
