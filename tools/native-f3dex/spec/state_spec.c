/*
 * state spec: the small state-changing commands of the F3DEX 1.x / F3DLX microcode (documented in kit/out/state.md).
 *   setgeom / cleargeom   G_SETGEOMETRYMODE / G_CLEARGEOMETRYMODE
 *   texture               G_TEXTURE
 *   moveword              G_MOVEWORD
 *   somL / somH           G_SETOTHERMODE_L / _H, up to (not including) the call of the output-buffer flush
 *   modvtx                G_MODIFYVTX (0xb2 in 1.21 / NoN / 1.23)
 *   perspnorm             G_PERSPNORM (0xb4 in 0.95)
 *   rdphalf1 / rdphalf2   G_RDPHALF_1 / _2 (rdphalf2 up to the flush call, like somL / somH)
 *   movemem               G_MOVEMEM (class 0, after the DMA into 0xae0)
 * Register state set by the dispatch: $25/$24 = w0/w1, $27 = pointer after the command in the display-list buffer (the handlers re-read w0 from DMEM at
 * -8..-5($27)), $29 = 0x110, $22 = 0xae0 and $1 = w0 bits 23..16 for class 0, $23 = RDP output pointer.
 */
#include "spec.h"

/* byte-granular DMEM access with wrap at 4 KiB: what the RSP's lh / lw / sw do for unaligned addresses */
static uint8_t b8(struct armlet_env *e, unsigned a) { return armlet_dm8(&e->rsp, a & 0xfff); }
static uint32_t ldh(struct armlet_env *e, unsigned a) { return (b8(e, a) << 8) | b8(e, a + 1); }
static uint32_t ldw(struct armlet_env *e, unsigned a)
{
    return (uint32_t)b8(e, a) << 24 | (uint32_t)b8(e, a + 1) << 16 | (uint32_t)b8(e, a + 2) << 8 | b8(e, a + 3);
}
static void sth(struct armlet_env *e, unsigned a, uint32_t v)
{
    armlet_st8(&e->rsp, (a + 0) & 0xfff, v >> 8);
    armlet_st8(&e->rsp, (a + 1) & 0xfff, v);
}
static void stw(struct armlet_env *e, unsigned a, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        armlet_st8(&e->rsp, (a + i) & 0xfff, v >> (24 - 8 * i));
}

#define GEOM 0x114
#define SR(n) (e->rsp.sr[n])

/* ---- G_SETGEOMETRYMODE / G_CLEARGEOMETRYMODE: geometry mode word at 0x114 ---- */
static int setgeom(struct armlet_env *e, int exitpc)
{
    stw(e, GEOM, ldw(e, GEOM) | SR(24));
    return exitpc;
}
static int cleargeom(struct armlet_env *e, int exitpc)
{
    stw(e, GEOM, ldw(e, GEOM) & ~SR(24));
    return exitpc;
}

/* ---- G_TEXTURE: w0 -> 0x120, w1 -> 0x124 (raw), geometry bit 1 (G_TEXTURE_ENABLE) := w0 bit 0 ---- */
static int texture(struct armlet_env *e, int exitpc)
{
    stw(e, 0x120, SR(25));
    stw(e, 0x124, SR(24));
    uint32_t h = ldh(e, 0x116);
    sth(e, 0x116, (h & 0xfffd) | ((SR(25) & 1) << 1));
    return exitpc;
}

/* ---- G_MOVEWORD: word w1 stored at table[index] + offset; index = w0 bits 7..0, offset = w0 bits 23..8, table = halfwords at 0x30e + index ---- */
static int moveword(struct armlet_env *e, int exitpc)
{
    unsigned idx = b8(e, SR(27) - 5);
    unsigned off = ldh(e, SR(27) - 7);
    int32_t base = (int16_t)ldh(e, 0x30e + idx);
    stw(e, (unsigned)(base + (int32_t)off) & 0xfff, SR(24));
    return exitpc;
}

/* ---- G_SETOTHERMODE_L / _H: field (shift = w0 bits 15..8, length = w0 bits 7..0) of the word at 0x11c (L) / 0x118 (H) := (old & ~mask) | w1; then
 *      the RDP command (0x118, 0x11c) is stored at the output pointer $23 (the flush call that follows is outside the stretch) ---- */
static int som(struct armlet_env *e, unsigned wordaddr, int exitpc, int dlx)
{
    uint32_t len = b8(e, SR(27) - 5), shift = b8(e, SR(27) - 6);
    uint32_t mask = (((uint32_t)1 << (len & 31)) - 1) << (shift & 31);
    stw(e, wordaddr, SR(24) | (ldw(e, wordaddr) & ~mask));
    uint32_t hi = ldw(e, 0x118), lo = ldw(e, 0x11c);
    if (dlx)                                    /* F3DLX: the RDP command tail first starts the RDP on what the previous flush wrote: DPC_END := word 0x128 */
        armlet_mtc0(e, 9, ldw(e, 0x128));
    stw(e, SR(23), hi);
    stw(e, SR(23) + 4, lo);
    return exitpc;
}

/* ---- G_MODIFYVTX: w1 stored at vertexaddr[w0 & 0xfff...] + (w0 bits 23..16); the vertex address table is at 0x31e (0x31c in 0.95, which has no MODIFYVTX) ---- */
static int modvtx(struct armlet_env *e, int exitpc)
{
    unsigned where = b8(e, SR(27) - 7);
    unsigned va = ldh(e, 0x31e + SR(25));
    stw(e, (where + va) & 0xfff, SR(24));
    return exitpc;
}

/* ---- G_PERSPNORM (0.95 only): low half of w1 stored at 0x112 ---- */
static int perspnorm(struct armlet_env *e, int exitpc)
{
    sth(e, 0x112, SR(24) & 0xffff);
    return exitpc;
}

/* ---- G_RDPHALF_1: w1 -> 0x10c.   G_RDPHALF_2: RDP command (0x10c, w1) stored at the output pointer ---- */
static int rdphalf1(struct armlet_env *e, int exitpc)
{
    stw(e, 0x10c, SR(24));
    return exitpc;
}
static int rdphalf2(struct armlet_env *e, int exitpc, int dlx)
{
    if (dlx)
        armlet_mtc0(e, 9, ldw(e, 0x128));
    stw(e, SR(23), ldw(e, 0x10c));
    stw(e, SR(23) + 4, SR(24));
    return exitpc;
}

/* ---- G_MOVEMEM (class 0): the 16 bytes at $22 (0xae0, DMA'd data) are stored (SQV) at the address halfword at 0x270 + index ($1 = w0 bits 23..16);
 *      a store that is not 16-byte aligned stops at the next 16-byte boundary ---- */
static int movemem(struct armlet_env *e, int exitpc)
{
    unsigned a = SR(1) + 0x270;
    unsigned dst = (int16_t)ldh(e, a) & 0xfff;
    unsigned src = SR(22) & 0xfff;
    unsigned nld = 16 - (src & 15);
    unsigned nst = 16 - (dst & 15);
    uint8_t reg[16];
    for (unsigned j = 0; j < 16; j++)
        reg[j] = j < nld ? b8(e, src + j) : 0;
    for (unsigned j = 0; j < nst && j < 16; j++)
        armlet_st8(&e->rsp, (dst + j) & 0xfff, reg[j]);
    return exitpc;
}

/* ---- segment translation of a DMA address (the subroutine the MOVEWORD segment table feeds; entered with $19 = address, returns it in $19 and the segment base in $12):
 *      $19 := segtab[(a >> 24) & 15] + (a & mask); segtab = words at 0x160; mask = 0x00ffffff (1.21 .. 1.23: the shifts sll 8 / srl 8; 0.95: the word at 0xb8) ---- */
static int segment(struct armlet_env *e, int mask_from_dmem)
{
    uint32_t a = SR(19);
    uint32_t base = ldw(e, 0x160 + ((a >> 22) & 60));
    uint32_t m = mask_from_dmem ? ldw(e, 0xb8) : 0x00ffffffu;
    SR(12) = base;
    SR(19) = base + (a & m);
    return SR(31);
}
static int w_segment_0(struct armlet_env *e) { return segment(e, 0); }
static int w_segment_1(struct armlet_env *e) { return segment(e, 1); }

/* per-build wrappers: exit pc of the handler (0x09c = display-list loop head after the yield check, 0x0ac = loop head without) or of the flush call of the RDP-command tail */
#define W_EXIT(fn, ex) static int w_##fn##_##ex(struct armlet_env *e) { return fn(e, 0x##ex); }
#define W_SOM(nm, wa, ex, dlx) static int w_##nm##_##ex(struct armlet_env *e) { return som(e, wa, 0x##ex, dlx); }
W_EXIT(setgeom, 09c) W_EXIT(cleargeom, 09c) W_EXIT(texture, 09c) W_EXIT(moveword, 09c) W_EXIT(modvtx, 09c) W_EXIT(perspnorm, 09c)
W_EXIT(rdphalf1, 0ac) W_EXIT(movemem, 09c)
W_SOM(somL, 0x11c, 37c, 0) W_SOM(somH, 0x118, 37c, 0) W_SOM(somL, 0x11c, 374, 0) W_SOM(somH, 0x118, 374, 0) W_SOM(somL, 0x11c, 404, 0) W_SOM(somH, 0x118, 404, 0)
W_SOM(somL, 0x11c, 378, 1) W_SOM(somH, 0x118, 378, 1) W_SOM(somL, 0x11c, 408, 1) W_SOM(somH, 0x118, 408, 1)
#define W_RH2(ex, dlx) static int w_rdphalf2_##ex(struct armlet_env *e) { return rdphalf2(e, 0x##ex, dlx); }
W_RH2(37c, 0) W_RH2(374, 0) W_RH2(404, 0) W_RH2(378, 1) W_RH2(408, 1)

/* SPEC_REG(var, name, ucode, entry, static exit, fn) */
#define R(v, nm, uc, ent, ex, fn) SPEC_REG(v, "spec.f3dex.state." nm, uc, ent, ex, fn)

/* F3DEX 1.21 (banjo121) */
R(bk_sg, "setgeom", UC_BANJO121, 0x310, 0x09c, w_setgeom_09c);
R(bk_cg, "cleargeom", UC_BANJO121, 0x320, 0x09c, w_cleargeom_09c);
R(bk_tx, "texture", UC_BANJO121, 0x250, 0x09c, w_texture_09c);
R(bk_mw, "moveword", UC_BANJO121, 0x238, 0x09c, w_moveword_09c);
R(bk_sl, "somL", UC_BANJO121, 0x27c, 0x37c, w_somL_37c);
R(bk_sh, "somH", UC_BANJO121, 0x274, 0x37c, w_somH_37c);
R(bk_mv, "modvtx", UC_BANJO121, 0x334, 0x09c, w_modvtx_09c);
R(bk_h1, "rdphalf1", UC_BANJO121, 0x348, 0x0ac, w_rdphalf1_0ac);
R(bk_h2, "rdphalf2", UC_BANJO121, 0x350, 0x37c, w_rdphalf2_37c);
R(bk_mm, "movemem", UC_BANJO121, 0x6d0, 0x09c, w_movemem_09c);

R(bk_sj, "segment", UC_BANJO121, 0x10c, 0, w_segment_0);

/* F3DEX.NoN 1.22 (sf64_non122): same code, movemem moved */
R(nn_sg, "setgeom", UC_SF64_NON, 0x310, 0x09c, w_setgeom_09c);
R(nn_cg, "cleargeom", UC_SF64_NON, 0x320, 0x09c, w_cleargeom_09c);
R(nn_tx, "texture", UC_SF64_NON, 0x250, 0x09c, w_texture_09c);
R(nn_mw, "moveword", UC_SF64_NON, 0x238, 0x09c, w_moveword_09c);
R(nn_sl, "somL", UC_SF64_NON, 0x27c, 0x37c, w_somL_37c);
R(nn_sh, "somH", UC_SF64_NON, 0x274, 0x37c, w_somH_37c);
R(nn_mv, "modvtx", UC_SF64_NON, 0x334, 0x09c, w_modvtx_09c);
R(nn_h1, "rdphalf1", UC_SF64_NON, 0x348, 0x0ac, w_rdphalf1_0ac);
R(nn_h2, "rdphalf2", UC_SF64_NON, 0x350, 0x37c, w_rdphalf2_37c);
R(nn_mm, "movemem", UC_SF64_NON, 0x6f0, 0x09c, w_movemem_09c);

R(nn_sj, "segment", UC_SF64_NON, 0x10c, 0, w_segment_0);

/* F3DEX 1.23 (ts2_f3dex123) */
R(t2_sg, "setgeom", UC_TS2_DEX, 0x308, 0x09c, w_setgeom_09c);
R(t2_cg, "cleargeom", UC_TS2_DEX, 0x318, 0x09c, w_cleargeom_09c);
R(t2_tx, "texture", UC_TS2_DEX, 0x248, 0x09c, w_texture_09c);
R(t2_mw, "moveword", UC_TS2_DEX, 0x230, 0x09c, w_moveword_09c);
R(t2_sl, "somL", UC_TS2_DEX, 0x274, 0x374, w_somL_374);
R(t2_sh, "somH", UC_TS2_DEX, 0x26c, 0x374, w_somH_374);
R(t2_mv, "modvtx", UC_TS2_DEX, 0x32c, 0x09c, w_modvtx_09c);
R(t2_h1, "rdphalf1", UC_TS2_DEX, 0x340, 0x0ac, w_rdphalf1_0ac);
R(t2_h2, "rdphalf2", UC_TS2_DEX, 0x348, 0x374, w_rdphalf2_374);
R(t2_mm, "movemem", UC_TS2_DEX, 0x6c8, 0x09c, w_movemem_09c);

R(t2_sj, "segment", UC_TS2_DEX, 0x104, 0, w_segment_0);

/* F3DLX 1.23 (ts2_f3dlx123) */
R(td_sg, "setgeom", UC_TS2_DLX, 0x300, 0x09c, w_setgeom_09c);
R(td_cg, "cleargeom", UC_TS2_DLX, 0x310, 0x09c, w_cleargeom_09c);
R(td_tx, "texture", UC_TS2_DLX, 0x240, 0x09c, w_texture_09c);
R(td_mw, "moveword", UC_TS2_DLX, 0x228, 0x09c, w_moveword_09c);
R(td_sl, "somL", UC_TS2_DLX, 0x26c, 0x378, w_somL_378);
R(td_sh, "somH", UC_TS2_DLX, 0x264, 0x378, w_somH_378);
R(td_mv, "modvtx", UC_TS2_DLX, 0x324, 0x09c, w_modvtx_09c);
R(td_h1, "rdphalf1", UC_TS2_DLX, 0x338, 0x0ac, w_rdphalf1_0ac);
R(td_h2, "rdphalf2", UC_TS2_DLX, 0x340, 0x378, w_rdphalf2_378);
R(td_mm, "movemem", UC_TS2_DLX, 0x6f4, 0x09c, w_movemem_09c);

R(td_sj, "segment", UC_TS2_DLX, 0x104, 0, w_segment_0);

/* F3DEX 0.95 (mk64_f3dex95): 0xb2 = RDPHALF_2, 0xb3 = RDPHALF_1, 0xb4 = PERSPNORM; no MODIFYVTX */
R(m4_sg, "setgeom", UC_MK64_DEX, 0x3a4, 0x09c, w_setgeom_09c);
R(m4_cg, "cleargeom", UC_MK64_DEX, 0x3b4, 0x09c, w_cleargeom_09c);
R(m4_tx, "texture", UC_MK64_DEX, 0x300, 0x09c, w_texture_09c);
R(m4_mw, "moveword", UC_MK64_DEX, 0x2e8, 0x09c, w_moveword_09c);
R(m4_sl, "somL", UC_MK64_DEX, 0x32c, 0x404, w_somL_404);
R(m4_sh, "somH", UC_MK64_DEX, 0x324, 0x404, w_somH_404);
R(m4_pn, "perspnorm", UC_MK64_DEX, 0x3c8, 0x09c, w_perspnorm_09c);
R(m4_h1, "rdphalf1", UC_MK64_DEX, 0x3d0, 0x0ac, w_rdphalf1_0ac);
R(m4_h2, "rdphalf2", UC_MK64_DEX, 0x3d8, 0x404, w_rdphalf2_404);
R(m4_mm, "movemem", UC_MK64_DEX, 0x758, 0x09c, w_movemem_09c);

R(m4_sj, "segment", UC_MK64_DEX, 0x10c, 0, w_segment_1);

/* F3DLX 0.95 (mk64_f3dlx95) */
R(m5_sg, "setgeom", UC_MK64_DLX, 0x39c, 0x09c, w_setgeom_09c);
R(m5_cg, "cleargeom", UC_MK64_DLX, 0x3ac, 0x09c, w_cleargeom_09c);
R(m5_tx, "texture", UC_MK64_DLX, 0x2f8, 0x09c, w_texture_09c);
R(m5_mw, "moveword", UC_MK64_DLX, 0x2e0, 0x09c, w_moveword_09c);
R(m5_sl, "somL", UC_MK64_DLX, 0x324, 0x408, w_somL_408);
R(m5_sh, "somH", UC_MK64_DLX, 0x31c, 0x408, w_somH_408);
R(m5_pn, "perspnorm", UC_MK64_DLX, 0x3c0, 0x09c, w_perspnorm_09c);
R(m5_h1, "rdphalf1", UC_MK64_DLX, 0x3c8, 0x0ac, w_rdphalf1_0ac);
R(m5_h2, "rdphalf2", UC_MK64_DLX, 0x3d0, 0x408, w_rdphalf2_408);
R(m5_mm, "movemem", UC_MK64_DLX, 0x784, 0x09c, w_movemem_09c);
R(m5_sj, "segment", UC_MK64_DLX, 0x10c, 0, w_segment_1);
