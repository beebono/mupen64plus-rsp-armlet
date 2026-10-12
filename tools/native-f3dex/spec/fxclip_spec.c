/*
 * fxclip_spec.c: the struct-level clipper (src/hle/f3dex_clip_fx.c) run through the spec harness against the real microcode on the JIT.
 *
 * spec.f3dex.fxclip.main.scalar: the stretch of clip_spec.c's spec.f3dex.clip.main (overlay entry -> the first triangle's hand-over, or the caller's return when the polygon is discarded).
 *   The adapter reads the three vertices ($1, $2, $3) and the plane / viewport / fog state from DMEM, calls f3dex_clip_tri_scalar, and writes back what the microcode leaves and nothing else reads
 *   (clip.md section 8): the new vertex records (0xde0 + 0x28 j, bytes 0 .. 0x25), the final polygon as the zero-terminated list the fan walks (in the buffer the ping-pong of the planes leaves it in:
 *   0xc40, or 0xc54 after an odd number of cut planes), 0xb4 = P[0], 0x106 = the fan pointer after the first step, and $1, $2, $3 of the first triangle (compared via Scenario.cmp_sr).
 *   The list is rebuilt from the function's triangles (P[0] = tri[0][2], P[1] = tri[0][1], P[k+2] = tri[k][0]), so every triangle of the fan is checked, not only the first. What the microcode
 *   also leaves is dead (the generator declares it): the scratch buffer and the scan copies, 0x158, and (for a discarded polygon) 0xb4, 0x106 and the lists.
 * spec.f3dex.fxclip.fan.scalar: one step of the fan from one triangle to the next (clip_spec.c's spec.f3dex.clip.fan). The adapter takes the polygon from the list in DMEM, makes the triangles with the
 *   function's fan (f3dex_clip_fan_scalar, the one f3dex_clip_tri uses) and hands over triangle k, k = (the list pointer at 0x106 - the list start) / 2. The generator builds real states: it runs the
 *   microcode through the first triangle and the following fan steps and picks one of the states a fan step is entered in (the last one returns to the caller).
 */
#include "spec.h"
#include "f3dex_dmem.h"

void f3dex_clip_fan_scalar(const struct f3dex_vtx *const *P, unsigned n, struct f3dex_clip_out *o);

struct fxcb { uint64_t id; unsigned tri; };
static const struct fxcb fxcbs[] = {
    { UC_BANJO121, 0x96c }, { UC_SF64_NON, 0x98c }, { UC_TS2_DEX, 0x964 }, { UC_TS2_DLX, 0x994 }, { UC_MK64_DEX, 0x9f4 }, { UC_MK64_DLX, 0xa24 },
};
static unsigned fxc_tri(const struct armlet_env *e)
{
    for (unsigned i = 0; i < sizeof fxcbs / sizeof fxcbs[0]; i++)
        if (fxcbs[i].id == e->routine->ucode)
            return fxcbs[i].tri;
    return fxcbs[0].tri;
}

#define NEWV(j) (0xde0u + 0x28u * (j))

static int fxclip_main(struct armlet_env *e)
{
    uint32_t *dm = e->rsp.dmem;
    uint32_t *sr = e->rsp.sr;
    const struct f3dex_build *b = f3dex_build_of_ucode(e->routine->ucode);
    struct f3dex_vctx c;
    (void)f3dex_load_vctx(&c, dm, b);           /* the light words are not used by the clipper */
    unsigned a[3] = { sr[1] & 0xfff, sr[2] & 0xfff, sr[3] & 0xfff };
    struct f3dex_vtx vv[3];
    for (int i = 0; i < 3; i++)
        f3dex_load_vtx(&vv[i], dm, a[i]);
    const struct f3dex_vtx *in[3] = { &vv[0], &vv[1], &vv[2] };
    struct f3dex_clip_out o;
    f3dex_clip_tri_scalar(&c, in, &o);
    for (unsigned j = 0; j < o.nnew; j++)
        f3dex_store_vtx(&o.nv[j], dm, NEWV(j));
    if (o.ntri == 0)
        return (int)sr[24];                     /* discarded: back to the caller */
    unsigned addr[7][3];
    for (unsigned k = 0; k < o.ntri; k++)
        for (int i = 0; i < 3; i++) {
            const struct f3dex_vtx *p = o.tri[k][i];
            addr[k][i] = p >= o.nv && p < o.nv + F3DEX_CLIP_MAX_NEW ? NEWV((unsigned)(p - o.nv)) : a[p - vv];
        }
    /* the final polygon P: ($1, $2, $3) = (P[k+2], P[k+1], P[0]) */
    unsigned n = o.ntri + 2, base = (o.nnew / 2) & 1 ? 0xc54 : 0xc40;
    unsigned P[9];
    P[0] = addr[0][2];
    P[1] = addr[0][1];
    for (unsigned k = 0; k < o.ntri; k++)
        P[k + 2] = addr[k][0];
    for (unsigned i = 0; i < n; i++)
        xs16(dm, base + 2 * i, (uint16_t)P[i]);
    xs16(dm, base + 2 * n, 0);
    xs16(dm, 0xb4, (uint16_t)P[0]);
    xs16(dm, 0x106, (uint16_t)(base + 2));
    sr[1] = addr[0][0];
    sr[2] = addr[0][1];
    sr[3] = addr[0][2];
    return (int)fxc_tri(e);
}

static int fxclip_fan(struct armlet_env *e)
{
    uint32_t *dm = e->rsp.dmem;
    uint32_t *sr = e->rsp.sr;
    unsigned p = xd16(dm, 0x106), base = p >= 0xc54 ? 0xc54 : 0xc40;
    struct f3dex_vtx vv[10];
    const struct f3dex_vtx *P[10];
    unsigned addr[10], n = 0;
    while (n < 10 && xd16(dm, base + 2 * n)) {
        addr[n] = xd16(dm, base + 2 * n) & 0xfff;
        f3dex_load_vtx(&vv[n], dm, addr[n]);
        P[n] = &vv[n];
        n++;
    }
    struct f3dex_clip_out o;
    f3dex_clip_fan_scalar(P, n, &o);
    unsigned k = (p - base) / 2;
    xs16(dm, 0x106, (uint16_t)(p + 2));
    if (k >= o.ntri)
        return (int)sr[30];                     /* P[k+2] == 0: the fan is done, back to the caller */
    sr[1] = addr[o.tri[k][0] - vv];
    sr[2] = addr[o.tri[k][1] - vv];
    sr[3] = addr[o.tri[k][2] - vv];
    return (int)fxc_tri(e);
}

#define FXC_REG(nm, uc, entry, fanpc, tri) \
    SPEC_REG(fxclip_m_##nm, "spec.f3dex.fxclip.main.scalar", uc, entry, tri, fxclip_main); \
    SPEC_REG(fxclip_f_##nm, "spec.f3dex.fxclip.fan.scalar", uc, fanpc, tri, fxclip_fan)

FXC_REG(bk, UC_BANJO121, 0x720, 0x920, 0x96c);
FXC_REG(ts2, UC_TS2_DEX, 0x718, 0x918, 0x964);
FXC_REG(mk64, UC_MK64_DEX, 0x7a8, 0x9a8, 0x9f4);
FXC_REG(sf64, UC_SF64_NON, 0x740, 0x940, 0x98c);
FXC_REG(ts2dlx, UC_TS2_DLX, 0x740, 0x940, 0x994);
FXC_REG(mk64dlx, UC_MK64_DLX, 0x7d0, 0x9d0, 0xa24);
