/*
 * rdp_spec.c: spec routines for the RDP output side of the F3DEX 1.x / F3DLX microcode (documented in kit/out/rdp.md):
 *   pt      the class-3 pass-through (opcodes 0xc0..0xff): segment translation of SETTIMG / SETZIMG / SETCIMG, the 8 bytes into the output buffer
 *   half1   RDPHALF_1: the word saved at DMEM 0x10c
 *   half2   RDPHALF_2: 8 bytes (saved word, this word) into the output buffer
 *   omode   SETOTHERMODE_H / _L: the cached RDP SET_OTHER_MODES words at DMEM 0x118 / 0x11c, then those 8 bytes into the output buffer
 *   post    the branch after the flush (yield-check or not)
 *   fdec    the three-way decision at the start of the flush (empty / would overflow the FIFO / fits)
 *   ffull   the whole flush for the cases that do not poll DPC_CURRENT unboundedly (empty or FIFO wrap)
 *   full_*  a whole command (pass-through, RDPHALF_2, SETOTHERMODE) including the flush (FIFO-wrap scenarios), F3DLX's DPC_END and the branch on $2
 *   fnow    the flush from the third wait loop on (the buffer fits; DPC_CURRENT = the pointer)
 *   ftail   the tail of the flush: SP DMA write of the buffer, DMEM 0x128, DPC_END (F3DLX: no wait, no DPC_END)
 *   dmaw    the DMA write helper (SP_MEM_ADDR, SP_DRAM_ADDR, SP_WR_LEN)
 *   tri     the state a triangle leaves at the flush call: length of the output buffer from the flags byte
 *   init    the task-start initialisation of the FIFO variables (0x128, 0x150, 0x154) and DPC_START / DPC_END
 * Stretches stop before anything the harness cannot run (the SP DMA is not performed by the standalone JIT: only the register writes are seen).
 * The standalone harness stores raw values for SP_MEM_ADDR..SP_WR_LEN; values are masked here exactly as the JIT does (0x1fff / 0xffffff).
 */
#include "spec.h"

struct rdp_b {
    int dlx;                        /* F3DLX: DPC_END is written by the callers, not by the flush */
    unsigned pt;                    /* class-3 stage ("sra $2,$25,24") */
    unsigned jalpc;                 /* the "jal flush" of the pass-through tail */
    unsigned post;                  /* after the flush returns: F3DLX: the wait + DPC_END write; then "bgtz $2" */
    unsigned h1, h2, omh, oml;      /* handler entries: RDPHALF_1, RDPHALF_2, SETOTHERMODE_H, SETOTHERMODE_L */
    unsigned F;                     /* flush entry */
    unsigned dmaw;                  /* the DMA helper entry that starts a write when $17 > 0 */
    unsigned ie, ix;                /* init: entry (lw $23,40($1)) and exit */
};

static const struct rdp_b B_BANJO = { 0, 0x358, 0x37c, 0x384, 0x348, 0x350, 0x274, 0x27c, 0x128, 0xfd8, 0x758, 0x7c0 };
static const struct rdp_b B_NON   = { 0, 0x358, 0x37c, 0x384, 0x348, 0x350, 0x274, 0x27c, 0x128, 0xfd8, 0x778, 0x7e0 };
static const struct rdp_b B_TS2D  = { 0, 0x350, 0x374, 0x37c, 0x340, 0x348, 0x26c, 0x274, 0x120, 0xfd8, 0x750, 0x7b8 };
static const struct rdp_b B_TS2L  = { 1, 0x348, 0x378, 0x380, 0x338, 0x340, 0x264, 0x26c, 0x120, 0xfd8, 0x778, 0x7e0 };
static const struct rdp_b B_MK64D = { 0, 0x3e0, 0x404, 0x40c, 0x3d0, 0x3d8, 0x324, 0x32c, 0x168, 0x140, 0x7e0, 0x848 };
static const struct rdp_b B_MK64L = { 1, 0x3d8, 0x408, 0x410, 0x3c8, 0x3d0, 0x31c, 0x324, 0x168, 0x140, 0x808, 0x870 };

#define RSP(e) (&(e)->rsp)
#define SR(e, n) ((e)->rsp.sr[n])

/* the store of one RDP command into the output buffer at $23 (F3DLX: DPC_END = DMEM[0x128] first) */
static void emit8(struct armlet_env *e, const struct rdp_b *b, uint32_t w0, uint32_t w1)
{
    uint32_t p = SR(e, 23);
    if (b->dlx)
        armlet_mtc0(e, 9, armlet_dm32(RSP(e), 0x128));
    armlet_st32(RSP(e), p, w0);
    armlet_st32(RSP(e), p + 4, w1);
}

static int spec_pt(struct armlet_env *e, const struct rdp_b *b)
{
    uint32_t w0 = SR(e, 25), w1 = SR(e, 24);
    int32_t op = (int32_t)w0 >> 24;
    if (op + 3 >= 0)                /* 0xfd SETTIMG, 0xfe SETZIMG, 0xff SETCIMG: segment-translated address */
        w1 = armlet_dm32(RSP(e), 0x160 + ((w1 >> 24) & 15) * 4) + (w1 & 0xffffff);
    emit8(e, b, w0, w1);
    return b->jalpc;
}
static int spec_half1(struct armlet_env *e, const struct rdp_b *b)
{
    (void)b;
    armlet_st32(RSP(e), 0x10c, SR(e, 24));
    return 0x0ac;
}
static int spec_half2(struct armlet_env *e, const struct rdp_b *b)
{
    emit8(e, b, armlet_dm32(RSP(e), 0x10c), SR(e, 24));
    return b->jalpc;
}
static int spec_om(struct armlet_env *e, const struct rdp_b *b, unsigned addr)
{
    uint32_t len = armlet_dm8(RSP(e), SR(e, 27) - 5);       /* byte 3 of w0 */
    uint32_t sft = armlet_dm8(RSP(e), SR(e, 27) - 6);       /* byte 2 of w0 */
    uint32_t mask = ((1u << (len & 31)) - 1) << (sft & 31);    /* sllv uses the low 5 bits */
    uint32_t old = armlet_dm32(RSP(e), addr);
    armlet_st32(RSP(e), addr, SR(e, 24) | (old & ~mask));
    emit8(e, b, armlet_dm32(RSP(e), 0x118), armlet_dm32(RSP(e), 0x11c));
    return b->jalpc;
}
static int spec_omh(struct armlet_env *e, const struct rdp_b *b) { return spec_om(e, b, 0x118); }
static int spec_oml(struct armlet_env *e, const struct rdp_b *b) { return spec_om(e, b, 0x11c); }
static int spec_post(struct armlet_env *e, const struct rdp_b *b)
{
    if (b->dlx)
        armlet_mtc0(e, 9, armlet_dm32(RSP(e), 0x128));      /* F3DLX: DPC_END = the new DRAM pointer */
    return (int32_t)SR(e, 2) > 0 ? 0x09c : 0x0ac;
}

/* flush, first part: where it goes. exits: F+0x7c / F+0x74 (nothing to flush: jr), F+0x24 (would overflow), F+0x40 (fits; polls DPC_CURRENT) */
static int spec_fdec(struct armlet_env *e, const struct rdp_b *b)
{
    int32_t len = (int32_t)(SR(e, 23) - 0xce0);
    uint32_t ptr = armlet_dm32(RSP(e), 0x128), fend = armlet_dm32(RSP(e), 0x154);
    if (len <= 0)
        return b->F + (b->dlx ? 0x74 : 0x7c);
    return (int32_t)(fend - (ptr + len)) >= 0 ? b->F + 0x40 : b->F + 0x24;
}

/* the part of the flush that starts at "addu $23,$18,$19" (F+0x5c): $18 = length in bytes, $19 = DRAM pointer */
static void tail(struct armlet_env *e, const struct rdp_b *b, uint32_t len, uint32_t ptr)
{
    uint32_t newend = len + ptr;
    armlet_mtc0(e, 0, 0xce0);                                  /* SP_MEM_ADDR = 0xce0 (the JIT masks & 0x1fff) */
    armlet_mtc0(e, 1, ptr & 0xffffff);                         /* SP_DRAM_ADDR */
    armlet_mtc0(e, 3, len - 1);                                /* SP_WR_LEN = bytes - 1: the DMA starts here */
    armlet_st32(RSP(e), 0x128, newend);
    if (!b->dlx)
        armlet_mtc0(e, 9, newend);                             /* DPC_END (masked & ~7 by the register) */
    SR(e, 23) = 0xce0;
}
static int spec_ftail(struct armlet_env *e, const struct rdp_b *b)
{
    tail(e, b, SR(e, 18), SR(e, 19));
    return b->F + (b->dlx ? 0x74 : 0x7c);
}

/* the whole flush as C, for scenarios in which it either has nothing to do or wraps (the "fits" case polls DPC_CURRENT until the RDP has passed
 * the destination, which the standalone harness cannot arrange except through fnow) */
static void flush_c(struct armlet_env *e, const struct rdp_b *b)
{
    int32_t len = (int32_t)(SR(e, 23) - 0xce0);
    uint32_t ptr = armlet_dm32(RSP(e), 0x128), fend = armlet_dm32(RSP(e), 0x154), cur;
    if (len <= 0)
    {
        SR(e, 23) = 0xce0;
        return;
    }
    if ((int32_t)(fend - (ptr + len)) < 0)
    {
        uint32_t fstart = armlet_dm32(RSP(e), 0x150);
        while (armlet_mfc0(e, 11, 0) & 0x400)
            ;
        while (armlet_mfc0(e, 10, 0) == fstart)
            ;
        armlet_mtc0(e, 8, fstart);                             /* DPC_START: also DPC_CURRENT and DPC_END */
        ptr = fstart;
    }
    for (;;)
    {
        cur = armlet_mfc0(e, 10, 0);
        if ((int32_t)(ptr - cur) >= 0)
            break;
        if ((int32_t)((ptr + len) - cur) < 0)
            break;
    }
    tail(e, b, len, ptr);
}
static int spec_ffull(struct armlet_env *e, const struct rdp_b *b)
{
    uint32_t ret = SR(e, 31);
    flush_c(e, b);
    return ret;
}

/* a whole command with its flush (the scenarios force the FIFO-wrap path): emit, flush, F3DLX's second DPC_END, then the branch on $2 */
static int full_end(struct armlet_env *e, const struct rdp_b *b, uint32_t r2)
{
    SR(e, 23) += 8;
    flush_c(e, b);
    if (b->dlx)
        armlet_mtc0(e, 9, armlet_dm32(RSP(e), 0x128));
    return (int32_t)r2 > 0 ? 0x09c : 0x0ac;
}
static int spec_full_pt(struct armlet_env *e, const struct rdp_b *b)
{
    uint32_t w0 = SR(e, 25), w1 = SR(e, 24);
    int32_t op = (int32_t)w0 >> 24;
    if (op + 3 >= 0)
        w1 = armlet_dm32(RSP(e), 0x160 + ((w1 >> 24) & 15) * 4) + (w1 & 0xffffff);
    emit8(e, b, w0, w1);
    return full_end(e, b, (uint32_t)(op + 27));
}
static int spec_full_h2(struct armlet_env *e, const struct rdp_b *b)
{
    emit8(e, b, armlet_dm32(RSP(e), 0x10c), SR(e, 24));
    return full_end(e, b, SR(e, 2));
}
static int spec_full_om(struct armlet_env *e, const struct rdp_b *b, unsigned addr)
{
    uint32_t len = armlet_dm8(RSP(e), SR(e, 27) - 5), sft = armlet_dm8(RSP(e), SR(e, 27) - 6);
    uint32_t mask = ((1u << (len & 31)) - 1) << (sft & 31);
    uint32_t keep = armlet_dm32(RSP(e), addr) & ~mask;
    armlet_st32(RSP(e), addr, SR(e, 24) | keep);
    emit8(e, b, armlet_dm32(RSP(e), 0x118), armlet_dm32(RSP(e), 0x11c));
    return full_end(e, b, keep);
}
static int spec_full_omh(struct armlet_env *e, const struct rdp_b *b) { return spec_full_om(e, b, 0x118); }
static int spec_full_oml(struct armlet_env *e, const struct rdp_b *b) { return spec_full_om(e, b, 0x11c); }

/* the flush from "mfc0 $23,$10" (F+0x40) on, the case that the buffer fits: the third wait loop, then the tail. Entered with DPC_START / CURRENT / END = $19 & ~7
 * (the generator's pre-run executes the DPC_START write of the wrap path), which is the state a synchronous RDP leaves */
static int spec_fnow(struct armlet_env *e, const struct rdp_b *b)
{
    uint32_t len = SR(e, 18), ptr = SR(e, 19), cur;
    for (;;)
    {
        cur = armlet_mfc0(e, 10, 0);
        if ((int32_t)(ptr - cur) >= 0)
            break;
        if ((int32_t)((ptr + len) - cur) < 0)
            break;
    }
    tail(e, b, len, ptr);
    return b->F + (b->dlx ? 0x74 : 0x7c);
}

/* the DMA write helper: $20 = DMEM address, $19 = DRAM address, $18 = length register, $17 > 0 */
static int spec_dmaw(struct armlet_env *e, const struct rdp_b *b)
{
    (void)b;
    armlet_mtc0(e, 0, SR(e, 20) & 0x1fff);
    armlet_mtc0(e, 1, SR(e, 19) & 0xffffff);
    armlet_mtc0(e, 3, SR(e, 18));
    return SR(e, 31);
}

/* triangle -> flush: the buffer length from the flags byte, then the decision as in fdec */
static int spec_tri(struct armlet_env *e, const struct rdp_b *b)
{
    uint32_t b0 = armlet_dm8(RSP(e), 0xce0);
    uint32_t exp = 32 + ((b0 & 4) ? 64 : 0) + ((b0 & 2) ? 64 : 0) + ((b0 & 1) ? 16 : 0);
    int bad = SR(e, 23) - 0xce0 != exp;
    if (b->dlx && armlet_mfc0(e, 9, 0) != (armlet_dm32(RSP(e), 0x128) & ~7u))
        bad = 1;
    if (bad)
        armlet_st32(RSP(e), 0x000, 0xdeadbeef);
    return spec_fdec(e, b);
}

/* task start */
static int spec_init(struct armlet_env *e, const struct rdp_b *b)
{
    uint32_t t = SR(e, 1);
    uint32_t fstart = armlet_dm32(RSP(e), t + 40), fend = armlet_dm32(RSP(e), t + 44), ptr;
    int keep = 0;
    armlet_st32(RSP(e), 0x150, fstart);
    armlet_st32(RSP(e), 0x154, fend);
    if (!(armlet_mfc0(e, 11, 0) & 1))
    {
        uint32_t end = armlet_mfc0(e, 9, 0), cur = armlet_mfc0(e, 10, 0);
        if (!((int32_t)(fstart - end) > 0) && cur != 0 && cur != end)
        {
            keep = 1;
            ptr = end;
        }
    }
    if (!keep)
    {
        while (armlet_mfc0(e, 11, 0) & 0x400)
            ;
        armlet_mtc0(e, 11, 0);          /* the write of 1 (clear XBUS) leaves 0 in the JIT's DPC_STATUS when it was 0; the harness helper stores the value */
        armlet_mtc0(e, 8, fend);
        armlet_mtc0(e, 9, fend);
        ptr = fend;
    }
    armlet_st32(RSP(e), 0x128, ptr);
    SR(e, 23) = 0xce0;
    return b->ix;
}

#define WRAP(fn, tag) static int fn##_##tag(struct armlet_env *e) { return fn(e, &B_##tag); }
#define ALL(fn) WRAP(fn, BANJO) WRAP(fn, NON) WRAP(fn, TS2D) WRAP(fn, TS2L) WRAP(fn, MK64D) WRAP(fn, MK64L)
ALL(spec_pt) ALL(spec_half1) ALL(spec_half2) ALL(spec_omh) ALL(spec_oml) ALL(spec_post) ALL(spec_fdec) ALL(spec_ftail) ALL(spec_ffull)
ALL(spec_dmaw) ALL(spec_tri) ALL(spec_init) ALL(spec_fnow) ALL(spec_full_pt) ALL(spec_full_h2) ALL(spec_full_omh) ALL(spec_full_oml)

/* one routine per (build, stretch): SPEC_REG's shape with per-build constants */
#define REG(tag, UC, nm, fn, entry, exitpc, var) \
    static const uint16_t var##_##tag##_ex[] = { (exitpc) }; \
    ARMLET_ROUTINE(var##_##tag) = { .name = "spec.f3dex.rdp." nm, .ucode = (UC), .entry_pc = (entry), .dep_lo = (entry), .dep_hi = (entry) + 4, \
                                    .imem_hash = 0, .exit_pcs = var##_##tag##_ex, .n_exit_pcs = (exitpc) ? 1 : 0, .run = fn##_##tag };

/* the numbers repeat the table B_* above (initialisers of static structs are not constant expressions for the registration) */
#define REGB(tag, UC, PT, JAL, POST, H1, H2, OMH, OML, F, DMAW, IE, IX, TAILX) \
    REG(tag, UC, "pt.class3", spec_pt, PT, JAL, r_pt) \
    REG(tag, UC, "half1.rdphalf1", spec_half1, H1, 0x0ac, r_h1) \
    REG(tag, UC, "half2.rdphalf2", spec_half2, H2, JAL, r_h2) \
    REG(tag, UC, "omode.seth", spec_omh, OMH, JAL, r_omh) \
    REG(tag, UC, "omode.setl", spec_oml, OML, JAL, r_oml) \
    REG(tag, UC, "post.branch", spec_post, POST, 0, r_post) \
    REG(tag, UC, "fdec.decide", spec_fdec, F, 0, r_fdec) \
    REG(tag, UC, "ffull.flush", spec_ffull, F, 0, r_ffull) \
    REG(tag, UC, "ftail.dma", spec_ftail, (F) + 0x5c, (F) + TAILX, r_ftail) \
    REG(tag, UC, "fnow.fits", spec_fnow, (F) + 0x40, (F) + TAILX, r_fnow) \
    REG(tag, UC, "dmaw.helper", spec_dmaw, DMAW, 0, r_dmaw) \
    REG(tag, UC, "tri.tolen", spec_tri, F, 0, r_tri) \
    REG(tag, UC, "init.fifo", spec_init, IE, IX, r_init) \
    REG(tag, UC, "full_pt.class3", spec_full_pt, PT, 0, r_fpt) \
    REG(tag, UC, "full_h2.rdphalf2", spec_full_h2, H2, 0, r_fh2) \
    REG(tag, UC, "full_om.seth", spec_full_omh, OMH, 0, r_fomh) \
    REG(tag, UC, "full_om.setl", spec_full_oml, OML, 0, r_foml)

REGB(BANJO, UC_BANJO121, 0x358, 0x37c, 0x384, 0x348, 0x350, 0x274, 0x27c, 0x128, 0xfd8, 0x758, 0x7c0, 0x7c)
REGB(NON,   UC_SF64_NON, 0x358, 0x37c, 0x384, 0x348, 0x350, 0x274, 0x27c, 0x128, 0xfd8, 0x778, 0x7e0, 0x7c)
REGB(TS2D,  UC_TS2_DEX,  0x350, 0x374, 0x37c, 0x340, 0x348, 0x26c, 0x274, 0x120, 0xfd8, 0x750, 0x7b8, 0x7c)
REGB(TS2L,  UC_TS2_DLX,  0x348, 0x378, 0x380, 0x338, 0x340, 0x264, 0x26c, 0x120, 0xfd8, 0x778, 0x7e0, 0x74)
REGB(MK64D, UC_MK64_DEX, 0x3e0, 0x404, 0x40c, 0x3d0, 0x3d8, 0x324, 0x32c, 0x168, 0x140, 0x7e0, 0x848, 0x7c)
REGB(MK64L, UC_MK64_DLX, 0x3d8, 0x408, 0x410, 0x3c8, 0x3d0, 0x31c, 0x324, 0x168, 0x140, 0x808, 0x870, 0x74)
