# Triangle clipping overlay (overlay 1) of F3DEX 1.x / F3DLX

Reference build: F3DEX 1.21 (`banjo121`); pcs below are banjo's, other builds in section 9. Everything in sections 1-7 was checked against the JIT with the spec
harness (section 10): `tools/native-f3dex/spec/clip_spec.c` + `clip_gen.inc` reproduce the complete DMEM and exit pc of the stretch on all six builds.

**Disassembler caveat (affects reading this overlay).** The kit's disassembler prints any instruction whose destination/`rt` is `$0` as `nop`. Two of the "nops" in the
overlay are real stores: pc 0x748 is `sh $0, 0xc46($0)` (list terminator at entry) and pc 0x7f0 is `sh $0, 2($14)` (terminator after each new vertex). Other builds: same
words at the shifted pcs (section 9). Without them the polygon lists have no terminator.

## 1. Where it sits, what it does

The triangle code (pc 0x940 on banjo) is entered with `$1,$2,$3` = DMEM addresses of three transformed vertices (records of 0x28 bytes in the buffer at 0x4a0), `$24` = return pc
(0x09c display-list loop, or TRI2's 0x1ec), `$29` = 0x110. It first runs the two flag tests (F3DEX; F3DLX only if geometry-mode bit 23 `G_CLIPPING` is set):

* `F_and = flags(v1) & flags(v2) & flags(v3)`; if `F_and & 0x7070` (NoN: `& 0x7030`) the triangle is wholly outside one plane: dropped (jump to 0xf1c, outside this document).
* `F_or = flags(v1) | flags(v2) | flags(v3)`; if `F_or & 0x4343` the **clipping overlay** is entered at 0x720 (`bne` at 0x968). Otherwise the triangle is drawn at 0x96c.

`flags(v)` is the halfword at vertex + 0x24 (section 3). The overlay clips the triangle against six planes (a Sutherland-Hodgman pass per plane over a list of vertex
addresses), creating new vertex records for the crossing points, then hands the resulting convex polygon to the triangle code as a **fan of triangles**, one call per
triangle. The triangle code returns into the fan loop (`$24 = 0x920`), and after the last triangle the overlay returns to the original caller.

Exits of the stretch: (a) "first triangle": pc 0x96c (the triangle code proper), with the fan state in DMEM; (b) return to the caller (`$24` at entry): no triangle (polygon
entirely outside one plane, or fewer than 3 vertices left).

## 2. DMEM used

| addr | size | meaning |
|---|---|---|
| 0x70..0x9f | 6 x 8 | plane coefficient vectors `(cx,cy,cz,cw)` as 4 x s16; index `p = r6/2` (see section 4). Constants except the four `cw` words 0x76, 0x7e, 0x86, 0x8e = low halves of the clip-ratio words RNX 0x74, RNY 0x7c, RPX 0x84, RPY 0x8c (MOVEWORD idx 4; libultra: r, r, -r, -r; initial 1, 1, -1, -1) |
| 0xa6..0xb2 | 7 x 2 | plane masks, read at `0xa6 + r6` for `r6 = 12,10,..,2`: 0x0040, 0x4000, 0x0200, 0x0100, 0x0002, 0x0001 (at 0xb2, 0xb0, 0xae, 0xac, 0xaa, 0xa8) |
| 0xb4 | 2 | the fan's first vertex (written when the fan starts) |
| 0xf6..0xfe | 5 x 2 | code addresses of the state handlers: 0x774, 0x778, 0x7d0, 0x7e4 and the fan re-entry 0x920 (`$24` for the triangle code); constants |
| 0x100..0x104 | 3 x 2 | 0x73c, 0x758, 0x058 (constants; 0x102 = the loop top is used as a resume address, section 5) |
| 0x106 | 2 | vertex-code "return slot": the overlay stores the pc to resume at after the vertex tail (0x778 or 0x758); the fan reuses it as the list read pointer |
| 0x112 | 2 | perspective normalisation `pn` (u16), read by the vertex tail |
| 0x114 | 4 | geometry mode; the high half (`lh` at 0x114) is read: bit 0 = G_FOG |
| 0x158 | 2 | low 16 bits of `$31` at entry (never read back; in practice 0x1940 = DMEM[0x396]) |
| 0x28..0x2f | 8 | clamp constants of the screen position: 4090, -4090, 0x7fff, 0 |
| 0x380..0x38f | 16 | viewport: scale x,y,z,w (s16) then translation x,y,z,w |
| 0x390..0x395 | 6 | fog: multiplier, offset, maximum (s16 each) |
| 0x4a0 + 0x28 k | 32 x 0x28 | transformed vertex buffer (k < 32) |
| 0xc40 / 0xc54 | 2 x 20 | the two polygon lists (ping-pong): zero-terminated arrays of u16 vertex addresses, up to 9 entries + terminator |
| 0xdb8 | - | base of the new vertices: the j-th new vertex (j = 1..12) is at `0xdb8 + 0x28 j`, i.e. 0xde0 .. 0xf98 (ends at 0xfc0, the OSTask header) |

Vertex record (0x28 bytes; written by the vertex code, completed for new vertices by the overlay):

| off | size | field |
|---|---|---|
| 0x00 | 4 x s16 | clip-space x, y, z, w, integer halves |
| 0x08 | 4 x u16 | same, fraction halves (position = int * 65536 + frac, S15.16) |
| 0x10 | 4 x u8 | colour r,g,b,a (lit colour, or the game's colour; with G_FOG the a byte is replaced by the fog factor) |
| 0x14 | 2 x s16 | texture coordinates s, t (already scaled) |
| 0x18 | s16 | screen x (viewport transformed, clamped to +-4090) |
| 0x1a | s16 | screen y (same; F3DLX: `(y + 2) & ~3`) |
| 0x1c | s16 | screen z integer (clamped to 0..0x7fff) |
| 0x1e | u16 | screen z fraction |
| 0x20 | s16 | reciprocal of w' (= w * pn), integer half (see 7.2) |
| 0x22 | u16 | same, fraction half |
| 0x24 | u16 | clip flags (section 3) |
| 0x26 | 2 | padding: not written by the clip path |

## 3. Clip flags (halfword at +0x24)

Computed by the vertex code for transformed vertices and by the same code (section 7) for new vertices. With `W = w` and `Wr = sat32(w * ratio)` (ratio = s16 at 0x76) and the
32-bit comparison pair of the primer 1.4 (`le_k = X_k <= -W`, `ge_k = X_k >= W`, with the documented behaviour for W <= 0):

| bit | meaning | | bit | meaning |
|---|---|---|---|---|
| 0x0001 | x <= -Wr (left, scaled) | | 0x0100 | x >= Wr (right, scaled) |
| 0x0002 | y <= -Wr (bottom, scaled) | | 0x0200 | y >= Wr (top, scaled) |
| 0x0004 | z <= -Wr (unused by the overlay) | | 0x0400 | z >= Wr (unused) |
| 0x0010 | x <= -W | | 0x1000 | x >= W |
| 0x0020 | y <= -W | | 0x2000 | y >= W |
| 0x0040 | z <= -W (**near**) | | 0x4000 | z >= W (**far**) |

`flags(A) = (Bpair & 0x707) | ((Apair & 0x707) << 4)` where `Apair`/`Bpair` are the VCC values against `W` / `Wr` (bits 0..2 and 8..10 = x,y,z). The trivial-reject mask 0x7070 is the
unscaled set; the clip trigger 0x4343 is exactly the six plane bits: scaled for x and y, unscaled for z. With ratio 1 scaled = unscaled.

**F3DEX.NoN** (sf64): the flags of the vertex in lanes 0..3 of a vertex pair (the new vertex is always such a vertex) are `(Bpair & 0x707) | ((Apair & 0x703) << 4)`: bit 0x40 is never
set for it. For the vertex in lanes 4..7 bit 0x40 is `w < 0` (and its bit 0x04 additionally gets `w < 0` of the pair mate). Reject mask 0x7030. Plane 0x0040 is `w = 0` (section 4).
**F3DLX**: the flags are as in F3DEX; the clip test is skipped altogether when geometry-mode bit 23 is clear.

## 4. Planes

Plane `p` (processed in this order; `r6` is the doubled index after the decrement, coefficient vector at `0x70 + 4*r6`):

| order | mask | r6 | vector at | coefficients (cx,cy,cz,cw) | D(P) = c . P | meaning |
|---|---|---|---|---|---|---|
| 1 | 0x0040 | 10 | 0x98 | (0,0,1,1) [NoN: (0,0,0,1)] | z + w [NoN: w] | near |
| 2 | 0x4000 | 8 | 0x90 | (0,0,1,-1) | z - w | far |
| 3 | 0x0200 | 6 | 0x88 | (0,1,0,RPY) = (0,1,0,-r) | y - r w | top |
| 4 | 0x0100 | 4 | 0x80 | (1,0,0,RPX) = (1,0,0,-r) | x - r w | right |
| 5 | 0x0002 | 2 | 0x78 | (0,1,0,RNY) = (0,1,0,r) | y + r w | bottom |
| 6 | 0x0001 | 0 | 0x70 | (1,0,0,RNX) = (1,0,0,r) | x + r w | left |

`D` is evaluated on the 32-bit clip-space coordinates (product of the s16 coefficient with an S15.16 value, summed over the four lanes with the saturating lane-pair adds of 6.1).
**The clip ratio enters the plane equations** (the guard-band planes are x = +-r w, y = +-r w with the ratio words as `cw`), as well as the flag computation (only 0x76 there, for
both axes). "Inside the plane" is decided from the **flags**, not from `D`: vertex `v` is inside plane `p` iff `flags(v) & mask_p == 0`. For consistent flags this is `D >= 0` (for
W >= 0); a vertex with W <= 0 gets its flags from the documented VCH/VCL forms (for W < 0 every coordinate has its `le` or its `ge` bit set), nothing special-cases it: the same
arithmetic runs and the weights saturate (6.3) when `D` has the wrong sign.

## 5. Control flow as data

State: current list `L` (u16 addresses, 0-terminated, base `$5`), other buffer `O` (`$30`), mask `m` (`$11`), plane counter `$6`, next new-vertex pointer `$7` (initially 0xdb8).

```
entry: [0x158] = $31;  [0xc40] = v3; [0xc42] = v2; [0xc44] = v1; [0xc46] = 0   (the list is v3, v2, v1: reversed)
       $7 = 0xdb8; $30 = 0xc40; $6 = 12
for each plane (r6 = 12, 10, .., 2):                       // 6 planes
    L = current list (n entries), side(v) = flags(v) & m  (0 = inside)
    1. no vertex inside                      -> RETURN to caller ($24 at entry)       (polygon discarded; nothing more written)
    2. no vertex outside                     -> plane is a no-op (list unchanged, buffers not swapped)
    3. otherwise
         o = smallest index with side != 0                     (first outside vertex in list order)
         q = first inside index after o, cyclically            (scan o+1.., wrapping to index 0)
         r = first outside index after q, cyclically
         N1 = new vertex on the edge (O = L[q-1], Q = L[q])         // L[q-1] cyclic, outside
         N2 = new vertex on the edge (O = L[r],   Q = L[r-1])       // L[r-1] inside
         new list = [N1, L[q], L[q+1], .., L[r-1], N2, 0]           // cyclic run of inside vertices, n' = 2 + (r - q) mod n
         swap the buffers
fan: list P = final list (p entries)
     [0xb4] = P[0]; [0x106] = &P[0]
     loop: $3 = P[0]; $2 = P[k+1]; $1 = P[k+2]; [0x106] += 2
           if $1 == 0 -> return to caller via $30 (= original $24)     (p < 3, or finished)
           else $24 = 0x920 (DMEM[0xfe]); call the triangle code at 0x96c; it returns (jr $24) to the loop with k+1
```

Notes: (i) for convex input the inside run is unique so this is the usual Sutherland-Hodgman result; for a non-convex list (cannot occur for a triangle, but flags can be inconsistent)
only the first run after the first outside vertex is kept. (ii) Vertex order: output triangles are `($1,$2,$3) = (P[k+2], P[k+1], P[0])`, so for an unclipped list `(v3,v2,v1)` the call is `($1,$2,$3) = (v1,v2,v3)`:
orientation is preserved. (iii) A polygon has at most 3 + 6 = 9 entries (each plane adds at most one), 7 triangles, and at most 12 new vertices (two per plane) fit in 0xde0..0xfbf. (iv) The new vertices
of plane p are classified by plane p+1.. with their own flags (computed in 7), so later planes see them. (v) `$4` (the flat-shading source vertex of the triangle code, `LUV .. ($4)` at 0xc94) is untouched by the overlay:
a clipped flat-shaded triangle takes its colour from the original first vertex of the triangle, not from a new one.

State machine of the real code ($17 = 0,2,4,6; `$18` = look-for side, 0 or the mask): S0 scan for the first inside vertex (outside vertices pass), found -> rewind; S1 scan from index 0 for the first outside vertex,
found -> restart the output at index 1; S2 scan for the next inside vertex (wrapping once), found -> create N1 into slot 0 of the new list (the two vertices are swapped so that `$10` = outside, `$20` = inside, `v13[0]` keeps the inside one),
resume at 0x778; S3 copy inside vertices, wrapping once at the terminator, until an outside vertex -> create N2 behind the last entry, resume at the loop top 0x758. S0 hitting the terminator -> `jr $24` (case 1); S1 hitting the terminator -> next plane
without swapping (case 2). The resume addresses are stored in DMEM[0x106] (0x778 after N1, 0x758 after N2) because the vertex tail returns through `lh $8,0x106; jr $8`.

## 6. New vertex arithmetic (pcs 0x7f4..0x8f0)

Inputs: `O` = outside vertex address (`$10`), `Q` = inside vertex address (`$20`), plane vector `c` (4 x s16, lanes x y z w), `r6`. All sums below are the 48-bit accumulator chains of the primer; `sclamp`, `uclamp` as there.
Per lane `l` = 0..3 (only these lanes are used; the stored position is lanes 0..3):

### 6.1 Plane values

```
aq_l  = Qf_l * c_l + ((Qi_l * c_l) << 16)                  (VMUDN, VMADH)       (Qi:Qf = integer:fraction halves at Q+0..7 / Q+8..15)
pQ_l  = (sclamp(aq_l), uclamp(aq_l))                       (32-bit pair per lane)
ax_l  = aq_l + Of_l * (-c_l) + ((Oi_l * (-c_l)) << 16)     (-c_l = sat16(-c_l))
dX_l  = (sclamp(ax_l), uclamp(ax_l))                       (c_l * (Q_l - O_l))
```
`lane3_sum(v)` of four pairs: stage 1 `t_l = v_l + v_{l&~1}` (VADDC on the fractions with carry, VADD on the integers `clamp16(i + i' + carry)`), stage 2 `s = t_3 + t_1` likewise.
`X = lane3_sum(dX) = D(Q) - D(O)`, `DQ = lane3_sum(pQ) = D(Q)` (32-bit values `Xi:Xf`, `DQi:DQf`).

### 6.2 Rough reciprocal and weight

```
R  = rcp32((int32)(Xi << 16 | Xf)) ; Ri = R >> 16, Rf = R & 0xffff                          (primer 1.5)
v3 = (2 * Rf) & 0xffff ;  v7 = sclamp(2 * Rf + ((2 * Ri) << 16))
if Xi < 0:  v7 = sclamp(-v3 - (v7 << 16)) ;  v3 = (-v3) & 0xffff                          (negate: rcp32 of a negative input is ~rcp32(|x|))
cf = (v7 == 0) ? v3 : 0xffff                       // |1/X| as an unsigned 16-bit fraction (0xffff when |X| < 1.0 or the doubling overflows)
X' = floor(X * cf / 65536)       = (Xf*cf >> 16) + Xi*cf ; X'i = sclamp, X'f = uclamp           (VMUDL, VMADM, VMADN)
Inv = recip_refine((int32) X')                     // primer 1.6, = subroutine at pc 0; (Invi:Invf)
T   = D(Q) * Inv :   a1 = (DQf*Invf >> 16) + DQi*Invf + DQf*Invi ;  Tf = uclamp(a1) ;  Ti = sclamp(a1 + ((DQi*Invi) << 16))   // fraction read BEFORE the high term
as  = 3 + (Tf * cf >> 16) + Ti * cf ;   Si = sclamp(as), Sf = uclamp(as)                       // the 3 is (0xffff*4)>>16 from VMUDL v28,v31,v31[13] (rounding bias)
sf  = (Si != 0) ? 0xffff : Sf ;   if sf == 0: sf = 1
tf  = 65536 - sf                                                                              // both in 1..65535
```
`s = D(Q)/(D(Q) - D(O))` is the weight of the outside vertex, `t = 1 - s = -D(O)/(D(Q)-D(O))` of the inside vertex (`N = O + t (Q - O)`). When `|X| < 1.0`, when `Si != 0`
(`s` outside [0,1): inconsistent flags, ratio planes, `D(Q) < 0`) or when `X = 0` the weights saturate / take the values the formulas give; no branch handles them.

### 6.3 Interpolation

```
N_l (x y z w, lanes 0..3):  a = (Qf_l*tf >> 16) + Qi_l*tf + (Of_l*sf >> 16) + Oi_l*sf ;   Ni_l = sclamp(a), Nf_l = uclamp(a)      (S15.16, per-term truncation)
colour byte k (0..3):       v = sclamp(((O.byte_k << 7) * sf) + ((Q.byte_k << 7) * tf)) ;   byte_k = (v >> 7) & 0xff            = (O.k*sf + Q.k*tf) >> 16
s, t (halfwords at +0x14, +0x16):  v = sclamp(O.s * sf + Q.s * tf)  = (O.s*sf + Q.s*tf) >> 16 (arithmetic)
```
Only these are interpolated (clip position, colour word, s/t). Normals are not separate (a vertex with lighting already holds its final colour at +0x10). The colour word is taken from the freshly stored bytes.

## 7. The vertex tail applied to the new vertex (pcs 0x414..0x54c, entered with `$9 = 1`; the second-vertex half is skipped by `bltz $9`)

Not part of this overlay's own text but its effect on the record is part of the stretch. With `N` = (Ni:Nf) from 6.3, `pn` = u16[0x112], ratio = s16[0x76], viewport from 0x380:

1. Setup (`jal 0x660`): `v0 = viewport scale * (1,-1,1,1)` (y negated with saturation), `v1 = translation`, `geom = s16[0x114]`.
2. Flags: `Wr = N_w * ratio` (S15.16 product, integer half saturated: `Wri = sclamp`, `Wrf = low16`); `Apair` = (x,y,z of N vs `N_w`), `Bpair` = (x,y,z of N vs `Wr`) with the 32-bit VCH/VCL pair; `flags = (Bpair & 0x707) | ((Apair & 0x707) << 4)` (NoN: `Apair & 0x703`). Stored at +0x24.
3. `Q = N * pn`: `a = (Nf*pn >> 16) + Ni*pn`, `Qi = sclamp`, `Qf = uclamp` per lane. `(ri, rf) = recip_refine(Qi_3:Qf_3)` (the refined reciprocal of `w*pn`); stored at +0x20 (`ri`) and +0x22 (`rf`).
   `rint = ri >= 0 ? ri : 0x7fff`.
4. `M = N * (rint:rf)`: `a = (Nf*rf >> 16) + Ni*rf + Nf*rint` -> `Mf = uclamp(a)`; `Mi = sclamp(a + ((Ni*rint) << 16))` (fraction before the high term); then `T = M * pn` (as 3); then
   screen `A = (trans << 16) + Tf*scale + ((Ti*scale) << 16)`: `Sc = sclamp(A)`, `Sz = uclamp(A)` (lane 2); clamp lanes x,y to [-4090, 4090] and z (and the w lane) to [0, 0x7fff].
   Stored: +0x18 `Sc_x`, +0x1a `Sc_y`, +0x1c `Sc_z`, +0x1e `Sz` (F3DLX: +0x1a = `(Sc_y + 2) & 0xfffc`).
5. Fog (geometry-mode bit 16 = `geom & 1`): `f = clamp16(sclamp(Tf_z*mult + ((Ti_z*mult) << 16)) + off)`, `f = max(f, 0)`, `f = min(f, s16[0x394])`; the colour alpha byte (+0x13) becomes `f & 0xff`.
6. Position (+0x00 int x4, +0x08 frac x4) = `N`; colour word at +0x10; s,t at +0x14/+0x16.

The second vertex of the vertex code's pair is not written (the stores for lanes 4..7 are branched over).

## 8. DMEM effects

Written by the stretch, and all of it (checked by whole-DMEM comparison):

| addr | what | later use |
|---|---|---|
| 0x158 | `$31` & 0xffff | dead |
| 0xc40..0xc66 | both lists: the initial 3-entry list, the new lists, and the "scan copy" (below) | the final list is read by the fan; the rest dead |
| 0xde0 + 0x28 (j-1), j = 1..number of new vertices | new vertex records, bytes 0x00..0x25 (0x26/0x27 untouched) | read by the triangle code during the fan; dead afterwards |
| 0x106 | resume pc (0x778 / 0x758), then the fan list pointer (ends at `&P[k_last+1] + 2`, see below) | dead after the fan (the next vertex command re-initialises it) |
| 0xb4 | `P[0]` | read by the fan; dead afterwards |

* Scan copy: every plane iteration begins by writing a stale register (`$10`, the last vertex handled: 0 = the terminator after a no-op plane, `L[q]` (the first inside vertex, via `v13[0]`) after a plane that created N2; the
  incoming `$10` for plane 1) at `O[1]`, then copies the scanned input vertices behind it until the state changes (a no-op plane copies the whole input list to `O[2..n+1]`; a discarding plane copies up to the terminator). Because the other buffer is only
  ever written in full before it becomes current (list, N1, run, N2, 0), and reads never go behind a terminator, all of this is dead. The harness compares it anyway and the spec reproduces it, including the stale register.
* Terminators: `[0xc46] = 0` at entry (3-entry list), and `[$14 + 2] = 0` after each stored new vertex (this is the terminator of the finished list when the new vertex is N2; after N1 it is overwritten by the copy of the run).
* The fan: `[0x106]` is the list pointer `p` (initially `&P[0]`); each step reads `P[p/2+1]`, `P[p/2+2]`, writes `[0x106] = p + 2` (also on the final step where `P[p/2+2] == 0`, after which the stretch returns to the caller).
* Registers handed to the triangle code: `$1,$2,$3` as above, `$24 = 0x1920` (jr uses the low 12 bits: 0x920), `$30` = original `$24`, `$4` unchanged, `$29 = 0x110`, `$23` unchanged. The triangle code must preserve `$30` (it does: it never writes it). The vertex tail
  clobbers `$3,$8,$9,$12..$16`, the clip code `$5..$7,$10,$11,$14,$17,$18,$20`; `$31` is overwritten by the sub-calls and is 0x944 when returning to the caller via `jalr $30`.

## 9. Per build

The overlay text is the same instruction for instruction in all six builds (pcmap: every pc 0x720..0x93f maps `=`), only the pcs and a few DMEM constants differ. `off` = shift relative to banjo.

| build | off | entry | first-triangle exit | fan re-entry | tail entry | setup `jal` | TRI2 return | reject mask | kind |
|---|---|---|---|---|---|---|---|---|---|
| banjo121 (F3DEX 1.21) | 0 | 0x720 | 0x96c | 0x920 | 0x414 | 0x660 | 0x1ec | 0x7070 | F3DEX |
| ts2_f3dex123 (1.23) | -8 | 0x718 | 0x964 | 0x918 | 0x40c | 0x658 | 0x1e4 | 0x7070 | F3DEX |
| mk64_f3dex95 (0.95) | +0x88 | 0x7a8 | 0x9f4 | 0x9a8 | 0x49c | 0x6e8 | 0x22c | 0x7070 | F3DEX |
| sf64_non122 (NoN 1.22) | +0x20 | 0x740 | 0x98c | 0x940 | 0x414 | 0x680 | 0x1ec | 0x7030 | NoN |
| ts2_f3dlx123 (F3DLX 1.23) | +0x20 | 0x740 | 0x994 | 0x940 | 0x424 | 0x684 | 0x1dc | 0x7070 | F3DLX |
| mk64_f3dlx95 (F3DLX 0.95) | +0xb0 | 0x7d0 | 0xa24 | 0x9d0 | 0x4b4 | 0x714 | 0x224 | 0x7070 | F3DLX |

The `sh $0` words ("nop" in the disassembly): terminator-at-entry at banjo 0x748, ts2_dex 0x740, mk64_dex 0x7d0, sf64/ts2_dlx 0x768, mk64_dlx 0x7f8; terminator-after-new-vertex at 0x7f0, 0x7e8, 0x878, 0x810, 0x810, 0x8a0.
Differences in the processed data:
* NoN: plane 1 vector (0x98) is (0,0,0,1) instead of (0,0,1,1): the "near" plane is w = 0; flags of the new vertex as in section 3 (`Apair & 0x703`); reject mask 0x7030 (entry condition unchanged, 0x4343).
* F3DLX: new vertex +0x1a is `(sy + 2) & 0xfffc`; the whole clipping path is skipped unless geometry-mode bit 23 is set (tested by `lb 5($29)`, `bgez`, in the triangle code, F3DLX only); the F3DLX triangle code is entirely different (its entry is the first-triangle exit column).
* The data words 0xf6..0x104 are shifted with the code (section 2 values + off); 0x70..0xb2 are the same in all builds except the NoN vector.

## 10. Verification

Harness: `spec.f3dex.clip.main` (entry = overlay entry; exits: the first-triangle pc, 0x09c, the TRI2 return) and `spec.f3dex.clip.fan` (entry = fan re-entry; exits: first-triangle pc and the caller returns), DMEM and exit pc compared, 5000-6000 cases per build and seed:

| build | clip.main | clip.fan |
|---|---|---|
| banjo121 | 6000 / 6000 equal, seeds 1 and 5 (and 5000 x seeds 1, 7 earlier) | same |
| ts2_f3dex123 | 6000 / 6000, seeds 1, 5 | same |
| mk64_f3dex95 | 6000 / 6000, seeds 1, 5 | same |
| sf64_non122 | 6000 / 6000, seeds 1, 5 | same |
| ts2_f3dlx123 | 6000 / 6000, seeds 1, 5 | same |
| mk64_f3dlx95 | 6000 / 6000, seeds 1, 5 | same |

(The triangle code itself is never run: the stretch ends where it would start; the generator therefore does not test what the triangle code does with the new vertices.)

Generator (`clip_gen.inc`): three vertices (random slots among 32; occasionally equal slots) with S15.16 clip coordinates: w mostly positive of varying magnitude, sometimes negative or zero or fully random, x,y,z mostly a fraction of w
(inside, up to 1.5 w, around +-w and exactly on it) with outliers; flags computed from the positions with the vertex tail's rules (10% random flags instead); entry conditions of the triangle code enforced (reject mask, 0x4343); ratio words 1 (half
of the cases), 2, 3, 0..7, any s16, sometimes with unequal words; fog on/off, random viewport, fog and normalisation words; stale list/new-vertex memory (zero, random or vertex addresses); `$24` = 0x09c or the TRI2 return. Coverage of
the model over 5000 banjo cases (first version of the generator, before the unequal ratio words were added): new-vertex pairs per plane (order of section 4) 2961/2802/1482/1276/1244/1095, no-op 1970/2074/3060/3202/3136/3199, discard 69/55/334/64/98/86, final polygon sizes 3: 889, 4: 2244, 5: 759, 6: 319, 7: 74, 8: 7, 9: 2, new vertices per call 0..12
(all 12 reached), `cf` saturated in 4558 vertices, `X = 0` in 64, weight `sf == 1` in 50.

Mutation checks (each makes `clip.main` fail on banjo, many hundreds of cases of 1000 unless noted): `+3` -> `+2` in the weight bias; flags against `w` instead of `w*ratio`; `cf` without saturation; reciprocal int +1 in the record; `0xb4` -> `0xb6`;
terminator at `+4` instead of `+2` (hangs the spec: detected); NoN flag mask `0x703` -> `0x707` (caught only on sf64, as it must be). **Not caught:** `sf == 0 -> 1` replaced by `-> 2` (the case `Si == 0 && Sf == 0` did not occur in ~10^5 new vertices:
needs `Ti*cf + (Tf*cf>>16) == -3`, i.e. a tiny `cf`); the spec implements it as the code does but it is untested.

## 11. Open points

* The values of `$10`/`$20` at entry (stale, game-state dependent) only reach dead list slots; the spec takes them from the registers.
* Not enumerated: what the triangle code reads from the new vertex records and which of the clip-space position bytes (+0..+0xd) it needs; those are dead for all purposes except what the triangle documentation lists.
* Flags of a vertex with `w < 0` and the exact guard-band behaviour (`Si != 0`) are covered by the generator but not interpreted beyond "the arithmetic runs, the weights saturate".
* NoN `bit 2` of the second vertex of a pair receiving the first vertex's `w < 0` is read from the code (and verified through the new-vertex flags for lanes 0..3 only); the lane 4..7 combination is not exercised by this overlay.
