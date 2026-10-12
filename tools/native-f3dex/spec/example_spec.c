/* example: G_SETGEOMETRYMODE / G_CLEARGEOMETRYMODE (F3DEX 1.21: handlers at 0x310 / 0x320, exit to the display-list loop at 0x09c) */
#include "spec.h"

/* the geometry mode word is at DMEM 0x114; $24 holds the second command word */
static int spec_setgeom(struct armlet_env *e)
{
    uint32_t w1 = e->rsp.sr[24];
    armlet_st32(&e->rsp, 0x114, armlet_dm32(&e->rsp, 0x114) | w1);
    return 0x09c;
}
static int spec_cleargeom(struct armlet_env *e)
{
    uint32_t w1 = e->rsp.sr[24];
    armlet_st32(&e->rsp, 0x114, armlet_dm32(&e->rsp, 0x114) & ~w1);
    return 0x09c;
}
SPEC_REG(spec_setgeom_bk, "spec.f3dex.example.setgeom", UC_BANJO121, 0x310, 0, spec_setgeom);
SPEC_REG(spec_cleargeom_bk, "spec.f3dex.example.cleargeom", UC_BANJO121, 0x320, 0, spec_cleargeom);
