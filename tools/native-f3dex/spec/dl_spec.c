/*
 * dl_spec.c: the display-list engine of the F3DEX 1.x / F3DLX family as plain C: G_DL, G_ENDDL, G_BRANCH_Z, G_CULLDL, the segment translation, the
 * RDP pass-through (with the SETTIMG / SETZIMG / SETCIMG address translation), G_RDPHALF_1 / _2 and the loop's refill decision
 * (kit/out/dl.md). Checked against the JIT by tools/native-f3dex/rt_test: DMEM, the exit pc, the SP / DPC registers and (dl_gen.inc sets sc.cmp_sr) every GPR.
 *
 * Several stretches change only registers ($24, $26, $28 ...): there the exit pc (taken or not) and the GPR comparison are what the harness sees. The routines
 * model every temporary of their stretch; the "reg_*" routines have no DMEM effect at all.
 */
#include <stdint.h>
#include <string.h>

#include "spec.h"

/* DMEM: segment table 0x160 (16 words), return stack 0x398 (word per entry, depth byte at the build's depth_addr, 4 per entry), RDPHALF_1 word 0x10c,
 * vertex buffer 0x4a0 (0x28 per vertex, clip flags at +0x24, screen z at +0x1c), vertex-address table 0x31e (halfword per vertex index * 2),
 * RDP output write pointer in DRAM 0x128 */
#define SEGTAB 0x160
#define STACK 0x398
#define HALF1 0x10c
#define VTAB 0x31e
#define DRAMPTR 0x128

/* the subroutine at 0x10c (0x104 in 1.23): input and result in $19, scratch $12 (the segment word); 0.95 also $11 (mask 0xb8) and $13 (index * 4) */
static uint32_t do_xlate(struct armlet_env *e, int mk, uint32_t w)
{
    uint32_t *sr = e->rsp.sr;
    const struct armlet_rsp *r = &e->rsp;
    uint32_t idx4 = 4 * ((w >> 24) & 15);
    sr[12] = armlet_dm32(r, SEGTAB + idx4);
    if (mk)
    {
        sr[11] = armlet_dm32(r, 0xb8);                  /* 0x00ffffff */
        sr[13] = idx4;
        sr[19] = sr[12] + (w & sr[11]);
    }
    else
        sr[19] = sr[12] + (w & 0xffffffu);
    return sr[19];
}

/* ---- G_DL ------------------------------------------------------------------------------------------------------------------------------ */
/* $1 = bits 23..16 of w0 (the push flag: 0 = call, nonzero = branch), $24 = w1, $26 = DRAM address after the 320-byte window, $27 = pointer after
 * this command in the DMEM buffer, $28 = end of the valid part of the buffer. */
static int dl_body(struct armlet_env *e, unsigned depth_addr, int mk, unsigned link)
{
    uint32_t *sr = e->rsp.sr;
    const struct armlet_rsp *r = &e->rsp;
    int32_t d = (int8_t)armlet_dm8(r, depth_addr);
    sr[2] = d;                                          /* the delay slot of the test of $1 loads the depth in both cases */
    if (!((int32_t)sr[1] > 0))
    {
        sr[4] = d - 68;
        sr[3] = d + STACK;
        if (d - 68 > 0)
            return 0x09c;                               /* stack full (more than 18 entries): the command is ignored */
        sr[2] = d + 4;
        sr[26] = sr[27] + sr[26] - sr[28];              /* DRAM address of the command after this one */
        armlet_st32(r, STACK + d, sr[26]);
        armlet_st8(r, depth_addr, (uint8_t)(d + 4));
    }
    sr[31] = link;                                      /* the jal of the translation subroutine */
    do_xlate(e, mk, sr[24]);
    sr[26] = sr[19];
    sr[28] = sr[27];                                    /* forces a refill of the buffer from the new address */
    return 0x09c;
}
#define DLF(NAME, DEP, MK, LINK) static int NAME(struct armlet_env *e) { return dl_body(e, DEP, MK, LINK); }
DLF(spec_dl_bk, 0x148, 0, 0x710)
DLF(spec_dl_non, 0x148, 0, 0x730)
DLF(spec_dl_ts2, 0x148, 0, 0x708)
DLF(spec_dl_dlx, 0x148, 0, 0x734)
DLF(spec_dl_mk, 0x110, 1, 0x798)
DLF(spec_dl_mkl, 0x110, 1, 0x7c4)

/* ---- G_ENDDL --------------------------------------------------------------------------------------------------------------------------- */
static int enddl_body(struct armlet_env *e, unsigned depth_addr)
{
    uint32_t *sr = e->rsp.sr;
    const struct armlet_rsp *r = &e->rsp;
    int32_t d = (int8_t)armlet_dm8(r, depth_addr) - 4;
    sr[2] = d;
    sr[3] = d + STACK;
    if (d < 0)
        return 0x0bc;                                   /* empty stack: end of task */
    sr[26] = armlet_dm32(r, STACK + d);
    armlet_st8(r, depth_addr, (uint8_t)d);
    sr[28] = sr[27];
    return 0x09c;
}
static int spec_enddl(struct armlet_env *e) { return enddl_body(e, 0x148); }
static int spec_enddl_mk(struct armlet_env *e) { return enddl_body(e, 0x110); }

/* ---- G_BRANCH_Z (F3DEX 1.x only) ------------------------------------------------------------------------------------------------------- */
/* w0 low 12 bits = 2 * vertex index, w1 = the Z to compare with; the Z word is the vertex record's 32-bit screen Z (+0x1c). $25 = w0, $24 = w1.
 * Taken (the branch to the DL in the RDPHALF_1 word) when the signed 32-bit difference (z - w1) is not positive. */
static int branchz_body(struct armlet_env *e, int taken_pc)
{
    uint32_t *sr = e->rsp.sr;
    const struct armlet_rsp *r = &e->rsp;
    uint32_t va = armlet_dm16(r, VTAB + (sr[25] & 0xfffu) /* the lhu's address wraps in DMEM */);
    uint32_t z = armlet_dm32(r, va + 28);
    int32_t d = (int32_t)(z - sr[24]);
    sr[25] = (uint32_t)d;
    sr[24] = armlet_dm32(r, HALF1);
    if (d > 0)
        return 0x09c;
    /* taken: the stretch ends before the jump's target, which is the no-push G_DL tail (translate $24 into $26, force a refill) */
    sr[2] = armlet_dm32(r, 0x114);                      /* the delay slot of the jump (first instruction of G_SETGEOMETRYMODE) */
    return taken_pc;
}
#define BRZ(NAME, TAKEN) static int NAME(struct armlet_env *e) { return branchz_body(e, TAKEN); }
BRZ(spec_brz_708, 0x708)
BRZ(spec_brz_728, 0x728)
BRZ(spec_brz_700, 0x700)
BRZ(spec_brz_72c, 0x72c)

/* ---- G_CULLDL -------------------------------------------------------------------------------------------------------------------------- */
/* F3DEX 1.x: w0 and w1 low 12 bits = 2 * index of the first and the last vertex (inclusive); the AND of the clip flags (+0x24) of the vertices
 * va, va+0x28, ... up to and including vb; culled when (AND & mask) != 0, which continues as G_ENDDL. (vb < va or a non-multiple of 0x28 apart
 * never terminates: the generator does not produce it.) */
static int culldl_body(struct armlet_env *e, unsigned depth_addr, uint32_t mask)
{
    uint32_t *sr = e->rsp.sr;
    const struct armlet_rsp *r = &e->rsp;
    uint32_t va = armlet_dm16(r, VTAB + (sr[25] & 0xfffu)), vb = armlet_dm16(r, VTAB + (sr[24] & 0xfffu));
    uint32_t acc = mask, p = va, fl;
    for (;;)
    {
        fl = (uint32_t)(int32_t)(int16_t)armlet_dm16(r, p + 0x24);
        acc &= fl;
        if (p == vb)
            break;
        p += 40;
    }
    sr[25] = p + 40;                                    /* the increment is in the delay slot of the loop branch */
    sr[24] = vb;
    sr[3] = fl;
    sr[2] = acc;
    if (acc == 0)
    {
        sr[2] = (int8_t)armlet_dm8(r, depth_addr);      /* the delay slot of the test is the first instruction of G_ENDDL */
        return 0x09c;
    }
    return enddl_body(e, depth_addr);
}
static int spec_cull_7070(struct armlet_env *e) { return culldl_body(e, 0x148, 0x7070); }
static int spec_cull_7030(struct armlet_env *e) { return culldl_body(e, 0x148, 0x7030); }

/* F3DEX 0.95: w0 & 0x3ff = 40 * first vertex, w1 = 40 * (last vertex + 1) (exclusive), both direct offsets into the vertex buffer at 0x4a0 */
static int spec_cull_mk(struct armlet_env *e)
{
    uint32_t *sr = e->rsp.sr;
    const struct armlet_rsp *r = &e->rsp;
    uint32_t p = sr[25] & 1023u, acc = 0x7070, v;
    do
    {
        v = (uint32_t)(int32_t)(int16_t)armlet_dm16(r, 0x4c4 + p);
        p += 40;
        acc &= v;
    } while (p != sr[24]);
    sr[25] = p;
    sr[3] = v;
    sr[2] = acc;
    if (acc == 0)
    {
        sr[2] = (int8_t)armlet_dm8(r, 0x110);
        return 0x09c;
    }
    return enddl_body(e, 0x110);
}

/* ---- the RDP pass-through ---------------------------------------------------------------------------------------------------------------- */
/* class-3 commands (and the aliases that reach the same code): $25 = w0, $24 = w1, $23 = DMEM write pointer of the output buffer. Opcodes whose
 * signed byte + 3 is not negative (0xfd, 0xfe, 0xff: SETTIMG, SETZIMG, SETCIMG) get the segment translation of w1. The two words are stored at $23
 * (the exit is the call of the flush). The F3DLX builds first write DPC_END = the DRAM write pointer (0x128). */
static int rdpcopy_body(struct armlet_env *e, int mk, int dlx, int translate, unsigned link, unsigned link2)
{
    uint32_t *sr = e->rsp.sr;
    const struct armlet_rsp *r = &e->rsp;
    if (translate)
    {
        int32_t t = ((int32_t)sr[25] >> 24) + 3;
        if (t >= 0)
        {
            sr[31] = link;
            sr[24] = do_xlate(e, mk, sr[24]);
        }
        sr[2] = t + 24;                                 /* the delay slot of the test: later decides between the loop heads 0x09c / 0x0ac */
    }
    if (dlx)
    {
        sr[11] = 0;                                     /* the wait for the DMA to be idle (mfc0 DMA_BUSY) */
        sr[17] = 0;
        sr[31] = link2;
        sr[21] = armlet_dm32(r, DRAMPTR);
        armlet_mtc0(e, 9, sr[21]);
    }
    armlet_st32(r, sr[23], sr[25]);
    armlet_st32(r, sr[23] + 4, sr[24]);
    return 0;
}
static int ret_pc(int pc) { return pc; }
#define RDPC(NAME, MK, DLX, EXIT, LINK, LINK2) static int NAME(struct armlet_env *e) { rdpcopy_body(e, MK, DLX, 1, LINK, LINK2); return ret_pc(EXIT); }
RDPC(spec_rdpc_37c, 0, 0, 0x37c, 0x370, 0)
RDPC(spec_rdpc_374, 0, 0, 0x374, 0x368, 0)
RDPC(spec_rdpc_378, 0, 1, 0x378, 0x360, 0x36c)
RDPC(spec_rdpc_404, 1, 0, 0x404, 0x3f8, 0)
RDPC(spec_rdpc_408, 1, 1, 0x408, 0x3f0, 0x3fc)

/* G_RDPHALF_2: the RDP command (word stored by G_RDPHALF_1, w1) is appended to the output buffer; no translation */
#define HALF2(NAME, MK, DLX, EXIT, LINK2) \
    static int NAME(struct armlet_env *e) { e->rsp.sr[25] = armlet_dm32(&e->rsp, HALF1); rdpcopy_body(e, MK, DLX, 0, 0, LINK2); return ret_pc(EXIT); }
HALF2(spec_half2_37c, 0, 0, 0x37c, 0)
HALF2(spec_half2_374, 0, 0, 0x374, 0)
HALF2(spec_half2_378, 0, 1, 0x378, 0x36c)
HALF2(spec_half2_404, 1, 0, 0x404, 0)
HALF2(spec_half2_408, 1, 1, 0x408, 0x3fc)

/* G_RDPHALF_1: the word is kept for G_BRANCH_Z / G_RDPHALF_2; continues at the loop test that does not look at the yield request (0x0ac) */
static int spec_half1(struct armlet_env *e)
{
    armlet_st32(&e->rsp, HALF1, e->rsp.sr[24]);
    return 0x0ac;
}

/* ---- the loop's refill decision (0x0ac): buffer not exhausted -> next command, else the refill ---------------------------------------------- */
static int spec_refilldec(struct armlet_env *e)
{
    uint32_t *sr = e->rsp.sr;
    sr[25] = armlet_dm32(&e->rsp, sr[27]);              /* the delay slot */
    if (sr[28] != sr[27])
        return 0x064;
    sr[31] = armlet_dm16(&e->rsp, 0x104);
    return 0x0c8;
}

/* ---- register-only stretches (no DMEM effect; the exit pc and, in a register-comparing run, the registers are what is checked) -------------- */
/* the refill (0xc8 .. the call of the DMA helper at 0xdc): DMA of 320 bytes from $26 into the buffer 0x9a0, $26 += 320 */
static int spec_reg_refill(struct armlet_env *e)
{
    uint32_t *sr = e->rsp.sr;
    sr[28] = 0xae0;
    sr[21] = sr[31];
    sr[20] = 0x9a0;
    sr[19] = sr[26];
    sr[26] += 320;
    return 0x0dc;
}
/* the segment translation subroutine alone (generator: $31 = 0x09c): result in $19 */
static int spec_reg_xlate(struct armlet_env *e)
{
    do_xlate(e, 0, e->rsp.sr[19]);
    return (int)(e->rsp.sr[31] & 0xffc);
}
static int spec_reg_xlate_mk(struct armlet_env *e)
{
    do_xlate(e, 1, e->rsp.sr[19]);
    return (int)(e->rsp.sr[31] & 0xffc);
}
/* the head of the loop (0x058 .. the prefetch call at 0x088 / the class dispatch at 0x094) */
static int spec_reg_head(struct armlet_env *e)
{
    uint32_t *sr = e->rsp.sr;
    const struct armlet_rsp *r = &e->rsp;
    sr[11] = 0;                                         /* the wait for the DMA to be idle: mfc0 DMA_BUSY, and the helper's delay slot zeroes $17 */
    sr[17] = 0;
    sr[27] = 0x9a8;
    sr[25] = armlet_dm32(r, 0x9a0);
    sr[24] = armlet_dm32(r, 0x9a4);
    sr[1] = (sr[25] >> 29) & 6;
    sr[18] = sr[25] & 1023u;
    if (sr[1] > 0)
    {
        sr[31] = 0x060;
        sr[2] = (uint32_t)(int32_t)(int16_t)armlet_dm16(r, 0xb8 + sr[1]);
        return 0x094;
    }
    sr[22] = 0xae0;
    sr[31] = 0x088;
    do_xlate(e, 0, sr[24]);
    return 0x088;
}

/* ---- registration ---------------------------------------------------------------------------------------------------------------------- */
#define R(var, nm, uc, entry, exitpc, fn) SPEC_REG(var, nm, uc, entry, exitpc, fn)

R(call_bk, "spec.f3dex.dl.call", UC_BANJO121, 0x6e0, 0x09c, spec_dl_bk);
R(call_non, "spec.f3dex.dl.call", UC_SF64_NON, 0x700, 0x09c, spec_dl_non);
R(call_ts2, "spec.f3dex.dl.call", UC_TS2_DEX, 0x6d8, 0x09c, spec_dl_ts2);
R(call_dlx, "spec.f3dex.dl.call", UC_TS2_DLX, 0x704, 0x09c, spec_dl_dlx);
R(call_mk, "spec.f3dex.dl.call", UC_MK64_DEX, 0x768, 0x09c, spec_dl_mk);
R(call_mkl, "spec.f3dex.dl.call", UC_MK64_DLX, 0x794, 0x09c, spec_dl_mkl);

R(enddl_bk, "spec.f3dex.dl.enddl", UC_BANJO121, 0x2d8, 0x09c, spec_enddl);
R(enddl_non, "spec.f3dex.dl.enddl", UC_SF64_NON, 0x2d8, 0x09c, spec_enddl);
R(enddl_ts2, "spec.f3dex.dl.enddl", UC_TS2_DEX, 0x2d0, 0x09c, spec_enddl);
R(enddl_dlx, "spec.f3dex.dl.enddl", UC_TS2_DLX, 0x2c8, 0x09c, spec_enddl);
R(enddl_mk, "spec.f3dex.dl.enddl", UC_MK64_DEX, 0x384, 0x09c, spec_enddl_mk);
R(enddl_mkl, "spec.f3dex.dl.enddl", UC_MK64_DLX, 0x37c, 0x09c, spec_enddl_mk);

R(brz_bk, "spec.f3dex.dl.branchz", UC_BANJO121, 0x2f8, 0x09c, spec_brz_708);
R(brz_non, "spec.f3dex.dl.branchz", UC_SF64_NON, 0x2f8, 0x09c, spec_brz_728);
R(brz_ts2, "spec.f3dex.dl.branchz", UC_TS2_DEX, 0x2f0, 0x09c, spec_brz_700);
R(brz_dlx, "spec.f3dex.dl.branchz", UC_TS2_DLX, 0x2e8, 0x09c, spec_brz_72c);

R(cull_bk, "spec.f3dex.dl.culldl", UC_BANJO121, 0x2b8, 0x09c, spec_cull_7070);
R(cull_non, "spec.f3dex.dl.culldl", UC_SF64_NON, 0x2b8, 0x09c, spec_cull_7030);
R(cull_ts2, "spec.f3dex.dl.culldl", UC_TS2_DEX, 0x2b0, 0x09c, spec_cull_7070);
R(cull_dlx, "spec.f3dex.dl.culldl", UC_TS2_DLX, 0x2a8, 0x09c, spec_cull_7070);
R(cull_mk, "spec.f3dex.dl.culldl", UC_MK64_DEX, 0x368, 0x09c, spec_cull_mk);
R(cull_mkl, "spec.f3dex.dl.culldl", UC_MK64_DLX, 0x360, 0x09c, spec_cull_mk);

R(rdpc_bk, "spec.f3dex.dl.rdpcopy", UC_BANJO121, 0x358, 0x37c, spec_rdpc_37c);
R(rdpc_non, "spec.f3dex.dl.rdpcopy", UC_SF64_NON, 0x358, 0x37c, spec_rdpc_37c);
R(rdpc_ts2, "spec.f3dex.dl.rdpcopy", UC_TS2_DEX, 0x350, 0x374, spec_rdpc_374);
R(rdpc_dlx, "spec.f3dex.dl.rdpcopy", UC_TS2_DLX, 0x348, 0x378, spec_rdpc_378);
R(rdpc_mk, "spec.f3dex.dl.rdpcopy", UC_MK64_DEX, 0x3e0, 0x404, spec_rdpc_404);
R(rdpc_mkl, "spec.f3dex.dl.rdpcopy", UC_MK64_DLX, 0x3d8, 0x408, spec_rdpc_408);

R(h2_bk, "spec.f3dex.dl.rdphalf2", UC_BANJO121, 0x350, 0x37c, spec_half2_37c);
R(h2_non, "spec.f3dex.dl.rdphalf2", UC_SF64_NON, 0x350, 0x37c, spec_half2_37c);
R(h2_ts2, "spec.f3dex.dl.rdphalf2", UC_TS2_DEX, 0x348, 0x374, spec_half2_374);
R(h2_dlx, "spec.f3dex.dl.rdphalf2", UC_TS2_DLX, 0x340, 0x378, spec_half2_378);
R(h2_mk, "spec.f3dex.dl.rdphalf2", UC_MK64_DEX, 0x3d8, 0x404, spec_half2_404);
R(h2_mkl, "spec.f3dex.dl.rdphalf2", UC_MK64_DLX, 0x3d0, 0x408, spec_half2_408);

R(h1_bk, "spec.f3dex.dl.rdphalf1", UC_BANJO121, 0x348, 0x0ac, spec_half1);
R(h1_non, "spec.f3dex.dl.rdphalf1", UC_SF64_NON, 0x348, 0x0ac, spec_half1);
R(h1_ts2, "spec.f3dex.dl.rdphalf1", UC_TS2_DEX, 0x340, 0x0ac, spec_half1);
R(h1_dlx, "spec.f3dex.dl.rdphalf1", UC_TS2_DLX, 0x338, 0x0ac, spec_half1);
R(h1_mk, "spec.f3dex.dl.rdphalf1", UC_MK64_DEX, 0x3d0, 0x0ac, spec_half1);
R(h1_mkl, "spec.f3dex.dl.rdphalf1", UC_MK64_DLX, 0x3c8, 0x0ac, spec_half1);

R(rd_bk, "spec.f3dex.dl.refilldec", UC_BANJO121, 0x0ac, 0x064, spec_refilldec);
R(rd_non, "spec.f3dex.dl.refilldec", UC_SF64_NON, 0x0ac, 0x064, spec_refilldec);
R(rd_ts2, "spec.f3dex.dl.refilldec", UC_TS2_DEX, 0x0ac, 0x064, spec_refilldec);
R(rd_dlx, "spec.f3dex.dl.refilldec", UC_TS2_DLX, 0x0ac, 0x064, spec_refilldec);
R(rd_mk, "spec.f3dex.dl.refilldec", UC_MK64_DEX, 0x0ac, 0x064, spec_refilldec);
R(rd_mkl, "spec.f3dex.dl.refilldec", UC_MK64_DLX, 0x0ac, 0x064, spec_refilldec);

/* register-only (banjo / NoN / 1.23 only: the refill of 1.23 DEX is laid out differently and has no call at 0xdc) */
R(rr_bk, "spec.f3dex.dl.reg_refill", UC_BANJO121, 0x0c8, 0x0dc, spec_reg_refill);
R(rr_non, "spec.f3dex.dl.reg_refill", UC_SF64_NON, 0x0c8, 0x0dc, spec_reg_refill);
R(rr_mk, "spec.f3dex.dl.reg_refill", UC_MK64_DEX, 0x0c8, 0x0dc, spec_reg_refill);
R(rr_mkl, "spec.f3dex.dl.reg_refill", UC_MK64_DLX, 0x0c8, 0x0dc, spec_reg_refill);

R(rx_bk, "spec.f3dex.dl.reg_xlate", UC_BANJO121, 0x10c, 0x09c, spec_reg_xlate);
R(rx_non, "spec.f3dex.dl.reg_xlate", UC_SF64_NON, 0x10c, 0x09c, spec_reg_xlate);
R(rx_ts2, "spec.f3dex.dl.reg_xlate", UC_TS2_DEX, 0x104, 0x09c, spec_reg_xlate);
R(rx_dlx, "spec.f3dex.dl.reg_xlate", UC_TS2_DLX, 0x104, 0x09c, spec_reg_xlate);
R(rx_mk, "spec.f3dex.dl.reg_xlate", UC_MK64_DEX, 0x10c, 0x09c, spec_reg_xlate_mk);
R(rx_mkl, "spec.f3dex.dl.reg_xlate", UC_MK64_DLX, 0x10c, 0x09c, spec_reg_xlate_mk);

R(rh_bk, "spec.f3dex.dl.reg_head", UC_BANJO121, 0x058, 0x088, spec_reg_head);
R(rh_non, "spec.f3dex.dl.reg_head", UC_SF64_NON, 0x058, 0x088, spec_reg_head);
R(rh_ts2, "spec.f3dex.dl.reg_head", UC_TS2_DEX, 0x058, 0x088, spec_reg_head);
R(rh_dlx, "spec.f3dex.dl.reg_head", UC_TS2_DLX, 0x058, 0x088, spec_reg_head);
