# G_VTX of F3DEX 1.x / F3DLX: vertex lighting, texture-coordinate generation, fog (and the vertex record they land in)

Scope: the vertex command's handler (banjo121 pc 0x3a8..0x557), the lighting block it branches into (overlay ov2, pc 0x72c..0x7c3), the fog branch and
the texgen branch. The light-direction recomputation (0x7c4..) is documented in `mtx.md`. The position half of the handler (clip-space transform,
clip flags, reciprocal, screen position) is only summarised in section 5.1, with exact formulas, because the lit fields share the record and the harness
compares whole DMEM; its authoritative description is the vertex-pipeline document.

All pcs are F3DEX 1.21 ("banjo121", `kit/dis/banjo121.ov2.s`) unless a build is named; section 9 carries the other builds.

Everything below was checked against the JIT with `tools/native-f3dex/spec/light_spec.c` + `light_gen.inc` (routine `spec.f3dex.light.vtx`, the whole handler
from its entry to the return into the display-list loop, **all of DMEM and the exit pc compared**): 16000 random calls on each of the six builds, 0 differences
(section 11). Things that were only inferred are marked "inferred".

## 0. Summary of the answers

* Lighting is entered when geometry-mode bit 17 (0x20000) is set. Per vertex: `rgb = min(32767, amb<<7 + sum over lights of max(0, (c*D + 128) >> 8)) >> 7`, with
  `D = sat(sat(2*n0*d0 + 2*n1*d1) + 2*n2*d2)` the dot product of the signed-byte normal (input bytes 12..14) with the light's model-space direction byte vector,
  `c` the light's colour byte; the alpha byte is the vertex's own (input byte 15). Details and the exact loop order in 5.2.
* Texgen (bit 18, inside the lighting block only): `s = D_X`, `t = D_Y` dot products with the two lookat direction records, mapped by
  `(D+1)>>1 + 0x4000` (typical game data) or, in general, by the formula of 5.3; linear texgen (bit 19) additionally applies a cubic polynomial with the
  constants at DMEM 0xa2/0xa4/0xa6. The result replaces the vertex's own s,t before the texture scale `(v * scale) >> 16`.
* Fog (bit 16, independent of lighting): `alpha = clamp(sat16((Qz * fm) >> 16) + fo, 0, max)` where `Qz` is the projected z (clip z / clip w, perspective-normalised,
  32-bit S15.16), `fm = s16 [0x390]`, `fo = s16 [0x392]`, `max = s16 [0x394]` (0x00ff). Stored in the colour word's alpha byte (+19 of the record), replacing the
  vertex alpha / the lit alpha.
* Record: +16 colour word (r g b a), +20 s,t (two s16), see section 4.
* Nothing outside the vertex slots (and the halfword DMEM[0x106]) is written. Lighting scratch is inside the slot and overwritten by the position stores (section 8).

## 1. Conventions

Notation as in `vu_primer.md`. `sc(acc) = sat16(acc >> 16)` (the "sclamp"), `uc(acc)` = the "uclamp" (`acc` if it lies in [-2^31, 2^31) taken as its low 16 bits, else
0xffff / 0). `vmulf(a, b) = sc(2*a*b + 0x8000)` (a, b signed 16, so `vmulf(-32768, -32768) = 32767`). `sat16` clamps to [-32768, 32767]. `u8(addr)` / `s8(addr)` / `u16` /
`s16` read DMEM big-endian (addr is a DMEM byte address). All accumulators are 48-bit signed (wrap, never reached by the data below except in the random tests, where
the spec wraps like the JIT). Shifts `>>` of negative numbers are arithmetic (floor).

The handler processes **two vertices per iteration** in the two halves of the vector registers (lanes 0..3 = first vertex "A", lanes 4..7 = second vertex "B"). The halves never
mix, with two exceptions listed in section 7 (a stale register value and, in NoN, a clip-flag bit). Below, `h` = 0 for A and 1 for B; all formulas are per vertex.

## 2. The handler

Entry: banjo pc 0x3a8 (the table entry of opcode 4 in the class-0 table at DMEM 0xc0; per build in section 9). Exit: `jr` to the pc in DMEM[0x106]
(banjo: the main loop 0x09c); the handler first copies DMEM[0xba] (= 0x109c, the main loop address with a flag nibble) to DMEM[0x106]:

* entry registers (set by the dispatch, read by the handler): `$1` = byte 1 of w0 (= 2*v0, v0 = first vertex slot), `$22` = 0xae0 (input vertices, DMA'd by the dispatch
  loop), `$27` = DMEM address after the command (w0 bytes at `$27-8..-5`), `$29` = 0x110. Command word 0: `0x04 << 24 | 2*v0 << 16 | n << 10 | 16*n`, w1 the vertex address (consumed
  by the dispatch DMA).
* `n` = `u8($27 - 6) >> 2` (bits 15..10 of w0). `n = 0` still handles one vertex (the loop is a do-while with step 2).
* slot pointer `r7 = u16(0x31e + $1)` (banjo; table of 32 halfwords 0x4a0 + 0x28*i); vertex records are 0x28 bytes.
* the geometry mode halfword `r3 = s16(0x114)` (bits 31..16 of the geometry mode): bit0 fog (0x10000), bit1 lighting (0x20000), bit2 texgen (0x40000), bit3 linear
  texgen (0x80000); F3DLX: bit7 = G_CLIPPING (0x800000).
* two helper calls: viewport (0x660: reads 0x380..0x38f, perspNorm `u16(0x112)`, `r3`) and matrix (0x688: loads the 4x4 matrix 0x460..0x49f).

Loop over pairs (A at `$22`, B at `$22+16`; `r22 += 32` per pair; `r7 += 80` per pair; B's slot is `r7 + 40`):
```
r9 = n
do {  process A and B (both, in the registers)
      r9 -= 2
      store A's record
      if (r9 < 0) break          // odd count (or n = 0): B is not stored
      store B's record
} while (r9 > 0)
```
(The number of vertices stored is n, or 1 for n = 0. v0 + n must be <= 32.)

If lighting is on, each pair goes through the lighting block (0x72c); with another overlay resident (ov0 / ov1 / ov3) the same pc holds a stub that DMAs the lighting overlay
in (DMA table entry 0x18: text offset 0x1228, 0x198 bytes to IMEM 0x720) and re-enters 0x72c; the DMA is not part of the arithmetic and was not covered by the harness.
If the lighting word at 0x12c is negative the block first runs the light-direction recomputation (`mtx.md` section 4) and restarts; the spec declines such calls.

## 3. DMEM read by the handler (banjo; same addresses in all builds unless section 9 says so)

| address | size | meaning |
|---|---|---|
| 0x30..0x3f | 8 x s16 | `v31` constants: (0, 1, 2, -1, 0x4000, 4, 0x0633, 0x0200). Used: 0, 1, 2 and 0x4000 (texgen bias, `s16(0x38)`) |
| 0x28..0x2f | 4 x s16 | screen clamp: +0x28 = 4090 (x,y max), +0x2a = -4090 (x,y min), +0x2c = 0x7fff (z max), +0x2e = 0 (z min) |
| 0x40 | s16 | `v30[0]` = 0x7fff (reciprocal int clamp) |
| 0x50 | 8 x s16 | (1,-1,1,1, 1,-1,1,1): sign vector applied to the viewport scale |
| 0x60 | 8 x s16 | all 2: the constant 2 of the reciprocal refinement |
| 0x76 | s16 | clip ratio (flag guard factor) |
| 0xa2, 0xa4, 0xa6 | s16 | linear-texgen polynomial constants 0x7fff, 0x571d, 0x3a0c (0xa0 is the overlay return pc, not a constant of this area) |
| 0xba | u16 | return-pc halfword copied to 0x106 |
| 0x112 | u16 | perspective normalisation `pn` (0xffff = 1.0) |
| 0x114 | u32 | geometry mode |
| 0x124, 0x126 | u16, u16 | texture scale s, t (G_TEXTURE) |
| 0x12c | u32 | lighting word: bit31 recompute pending (see 2), bits 11..0 `L = (nlights+1)*32` (nlights directional lights) |
| 0x1a0..0x1a7 | 8 bytes | texgen coefficient bytes: A uses 0x1a0..0x1a3, B uses 0x1a4..0x1a7. Data-segment constant 0x80,0,0,0,0x80,0,0,0 (no handler seen writing it) |
| 0x1b0 + 32*i | 32 bytes | light record i (i = 0: LookAt Y, 1: LookAt X, 2..: lights, then the ambient record). +0..7 colour (A: +0..3 = r,g,b,pad; B: +4..7, the duplicate), +8..15 rest of the game's 16-byte struct (direction, raw), **+16..23 model-space direction** (A: +16..18 = dx,dy,dz s8; B: +20..22; written duplicated by the recompute), +24..31 unused |
| 0x380..0x38f | 4 x s16 + 4 x s16 | viewport scale (x,y,z,w), translation (x,y,z,w) |
| 0x390, 0x392, 0x394 | s16 x 3 | fog multiplier, fog offset, fog maximum (0x00ff). MOVEWORD writes (multiplier << 16 \| offset) at 0x390 |
| 0x31e + 2*v0 | u16 table | vertex slot addresses |
| 0x460..0x47f, 0x480..0x49f | 16 x s16, 16 x u16 | MVP matrix: integer halves (row r at 0x460 + 8*r), fraction halves (row r at 0x480 + 8*r) |
| 0xae0 + 16*i | 16 bytes | input vertex i |

Input vertex (16 bytes): `x,y,z` s16 (+0,+2,+4), flag/pad (+6, ignored), `s,t` s16 (+8,+10), `r,g,b,a` bytes (+12..15) or, with lighting, `nx,ny,nz,a` (signed bytes for n; the alpha byte is always byte 15).

## 4. The vertex record (40 bytes at `r7` for A, `r7 + 40` for B)

| offset | size | content | produced by |
|---|---|---|---|
| +0 | 4 x s16 | clip-space x,y,z,w, integer halves | 5.1 |
| +8 | 4 x u16 | their fractions | 5.1 |
| +16 | 4 bytes | colour word: r, g, b, a (lit: lit rgb + original alpha; fog: a = fog factor) | 5.2, 5.5 |
| +20 | 2 x s16 | s, t (texture-scaled; texgen replaces the vertex's own) | 5.3, 5.4 |
| +24 | s16 | screen x (clamped to +-4090) | 5.1 |
| +26 | s16 | screen y (clamped to +-4090); **F3DLX: (y + 2) & ~3** | 5.1 |
| +28 | s16 | screen z integer (clamped to 0..0x7fff) | 5.1 |
| +30 | u16 | screen z fraction | 5.1 |
| +32 | s16 | 1/w integer half (raw: may be negative) | 5.1 |
| +34 | u16 | 1/w fraction | 5.1 |
| +36 | u16 | clip flags | 5.1 |
| +38 | u16 | A: not written. B: a copy of A's clip flags (leftover of `sw $8,76($7)`), see 8 | 5.1 |

(Screen w, +30 before the z fraction overwrites it, is stored and overwritten: not part of the result.)

## 5. Per-vertex arithmetic

### 5.1 Position, clip flags, 1/w, projection, screen (summary; exact)

Matrix `M[r][k]` (r, k = 0..3) = `s16(0x460 + 8r + 2k) * 65536 + u16(0x480 + 8r + 2k)`. Input `x[0..2]` = s16 position.
```
for k in 0..3:                                   // MAC chain, exact 48-bit
    acc = 0
    for j in 0..2: acc += u16(0x480+8j+2k) * x[j];  acc += (s16(0x460+8j+2k) * x[j]) << 16
    acc += u16(0x498 + 2k);   Pf[k] = uc(acc)    // fraction read BEFORE the last integer term
    acc += s16(0x478 + 2k) << 16;   Pi[k] = sc(acc)
// (Pi[k]:Pf[k]) is the clip-space coordinate P[k] in S15.16 (clamps only on overflow)
```
Clip flags (the pair of compare chains `VCH/VCL`; `vu_primer.md` 1.4): with `X = P[k]` (k = 0,1,2), `W = P[3]`, `W2 = ratio * P[3]` where
`ratio = s16(0x76)`, `W2i = sc(acc), W2f = low16(acc)`, `acc = Pf[3]*ratio + (Pi[3]*ratio << 16)`:
```
cmp(X, W): opp = (X < 0) != (W < 0);  lo = opp ? (X + W <= 0) : (W < 0);  hi = opp ? (W < 0) : (X - W >= 0)
flags = sum_k<3 ( lo2_k << k | lo1_k << (4+k) | hi2_k << (8+k) | hi1_k << (12+k) )    // 1 = cmp(X, W), 2 = cmp(X, W2)
```
(`x`, `y`, `z` bits; with W >= 0 bit `lo` is X <= -W, `hi` is X >= W.) The record gets this halfword at +36; B's record gets `flags_B` at +36 and `flags_A` at +38.
The stored clip-space integer halves are `Pi` and fractions `Pf` unchanged.

Reciprocal: `pn = u16(0x112)`; `acc = ((Pf[3]*pn) >> 16) + Pi[3]*pn`; `Wi = sc(acc)`, `Wf = uc(acc)`; `R = recip_refine(Wi:Wf)` as in `vu_primer.md` 1.5/1.6 with the ROM
`rom[0] = 0xffff, rom[i] = ((2^34 / (i + 512)) + 1) >> 8` (i = 1..511) (checked against `reciprocal.cpp`); the stored `+32 = Ri` (raw), `+34 = Rf`. `Rc_i = Ri >= 0 ? Ri : 0x7fff`.

Projection (k = 0,1,2): `q0 = mul32(Pi[k]:Pf[k], Rc_i:Rf)` (floor(P * Rc / 65536), `vu_primer.md` 1.2), then `acc = ((q0f*pn) >> 16) + q0i*pn`; `Qi[k] = sc(acc)`, `Qf[k] = uc(acc)`.
`Qz = Qi[2]:Qf[2]` is the quantity the fog uses.

Screen: `scale_k = sat16(s16(0x380+2k) * (k == 1 ? -1 : 1))`, `trans_k = s16(0x388+2k)`; `acc = trans<<16 + Qf*scale + (Qi*scale << 16)`; `v6 = sc(acc)`, `v7 = uc(acc)`;
x, y (k = 0, 1): `clamp(v6, s16(0x2a), s16(0x28))`; z: `clamp(v6, s16(0x2e), s16(0x2c))`; z fraction `v7`.

### 5.2 Lighting (bit 17 of the geometry mode)

Inputs: `n[0..2] = s8(in+12..14)`, `L = [0x12c]` (non-negative; `L = (nlights+1)*32`), `h`.
```
// lanes: j = 0,1,2 are r,g,b, j = 3 the pad byte (unused)
for j: acc[j] = 0;  carry[j] = u8(0x1d0 + L + 4h + j) << 7            // the ambient record (the record after the last light)
d_prev = stale_v6                                                   // see section 7: matters only when L == 0 and texgen is on
for r1 = L; ; r1 -= 32:                                             // r1 = L, L-32, ..., 0   (L/32 + 1 iterations)
    for j: acc[j] = sat16(acc[j] + max(carry[j], 0))                // adds the previous iteration's contribution (the ambient in the first)
    for k<3: p[k] = vmulf(n[k] << 8, s8(0x1c0 + r1 + 4h + k) << 8)  // = 2*n[k]*d[k], except (-128,-128) -> 32767
    D = sat16(sat16(p[0] + p[1]) + p[2]);   d_x = d_prev;  d_prev = D
    for j: c0[j] = u8(0x1b0 + r1 + 4h + j) << 7;  carry[j] = vmulf(c0[j], D)   // = (u8(colour)*D + 128) >> 8
    if (r1 <= 0) break
rgb[j] = (acc[j] >> 7) & 0xff   (j = 0..2)                          // acc is in 0..32767 here: byte = acc >> 7, 0..255
colour word = rgb[0] << 24 | rgb[1] << 16 | rgb[2] << 8 | u8(in + 15)
```
Notes:
* The contribution of the last record processed (`r1 = 0`) is **not added** to the colour: record 0 is the LookAt-Y record (its dot product and colour feed texgen, 5.3).
  The LookAt-X record (record 1, `r1 = 32`) is added like a light (its colour is zero in games). The directional lights are records 2..L/32; the ambient colour is read from record
  `L/32 + 1`, i.e. `0x1d0 + L`.
* Each add saturates at 32767; since all terms are >= 0 the result equals `min(32767, ambient + sum of max(0, term))`. Negative dot products (and so their colour product) are clamped to 0 per light.
* The normal is **not** transformed here: the light (and lookat) directions are already in model space (the recompute of `mtx.md`), and are used as unit vectors scaled by 127 (s8). A normal of
  length 127 against a unit light gives D ~ 32258 at most; D saturates at 32767.
* `vmulf(c0, D)` with `c0 = byte << 7` equals `(byte*D + 128) >> 8` (floor): `(2*(byte*128)*D + 0x8000) >> 16`.
* Vertex B reads the second halves (+4) of the 8-byte colour / direction fields; the game's data duplicates them. The harness randomises both halves independently.
* The original vertex colour bytes (12..14) are not read at all in the lit case (they are the normal); only byte 15 (alpha) survives, unless fog replaces it (5.5).

### 5.3 Texture-coordinate generation (bit 18, only inside the lighting block; bit 19 = linear variant)

After the light loop, with `D_Y` = the last `D` (record 0), `d_x` = the dot product of the iteration before (record 1, LookAt X; `stale_v6` if `L == 0`), `c0[j]` the colour bytes<<7 of record 0
(0x1b0 + 4h + j), `K[j] = u8(0x1a0 + 4h + j) << 8` (unsigned):
```
for j in 0,1:                                  // j = 0: s, j = 1: t
    acc = 2*c0[j]*D_Y + 0x8000                 // the accumulator left by the last VMULF of the loop
    acc += K[j] * d_x                          // VMADN: unsigned lane * signed dot product
    v = sc(acc)
    if (bit 19) {                              // linear texgen
        sq = vmulf(v, v);  cu = vmulf(sq, v)
        acc2 = 2*cu*s16(0xa2) + 0x8000 + 2*cu*s16(0xa6) + 2*v*s16(0xa4)
        v = sc(acc2)                           // ~ v*0.680 + cu*1.4535 in s15 units
    }
    st[j] = sat16(v + s16(0x38))               // + 0x4000
```
With the game's LookAt records (record 0 = LookAt Y with colour (0, 0x80, 0), record 1 = LookAt X with colour 0) and the constant bytes 0x80,0,0,0 this is
`s = sat((D_X + 1) >> 1) + 0x4000`, `t = ((D_Y + 1) >> 1) + 0x4000` (both then within 0..0x7fff, saturating at 32767): `0x1a0` makes lane 0 scale `d_x` by 0x8000 and the LookAt-Y colour byte 0x80 makes
lane 1 scale `D_Y` the same way. Other data give the general formula above (the harness randomises records, colours and the 0x1a0 bytes).
The 0xa2/0xa4/0xa6 constants, the 0x4000 and the bytes at 0x1a0 are read from DMEM at every call by the microcode (data-segment constants; the spec reads them from DMEM too).

If texgen is on but lighting is off, nothing happens (the texgen code lives in the lighting block); s,t are the vertex's.

### 5.4 Texture scale

`s_stored = sat16((s * u16(0x124)) >> 16)`, `t_stored = sat16((t * u16(0x126)) >> 16)` (signed s,t times the unsigned scale, floor of the 32-bit product / 65536; `VMUDM`). `s,t` = the vertex's own
`s16(in + 8), s16(in + 10)` or the texgen result. Stored at +20 / +22.

### 5.5 Fog (bit 16, any lighting state)

```
f = sc(Qf[2]*fm + (Qi[2]*fm << 16))      // fm = s16(0x390): floor(Qz * fm / 65536), saturated
f = sat16(f + s16(0x392))
f = clamp(f, 0, s16(0x394))              // 0x394 = 0x00ff: a signed compare with that halfword
alpha byte = f & 0xff                    // replaces byte 3 of the colour word (lit or not)
```
`Qz` is the projected z of 5.1 (after the second perspNorm multiply). The fm/fo pair is `gSPFogPosition`'s `(128000/(max-min), (500-min)*256/(max-min))` by SDK convention (inferred, not read from the microcode). B's byte is inserted
through a temporary word at the vertex slot +24 (overwritten afterwards).

### 5.6 What the stores are, per vertex

The values of sections 4/5 are stored (A first; B only when `r9 >= 0` after the decrement): +0..7 and +8..15 (clip space), +16 colour word (with the fog byte already inserted), +20 s,t,
+24..31 screen (x, y, z int; the z fraction goes to +30), +32/+34 1/w, +36 flags (A: halfword; B: halfword at +36 and A's flags at +38).

## 6. Counts and positions

`r7` starts at `u16(0x31e + 2*v0)`; the pair loop advances `r7` by 80 and the input by 32. The reads of the *next* pair's input vertices (`LDV` at the end of each iteration) may run past the
last vertex: they are reads only. For odd n the lit/fog/texgen computation still runs for the B lane on whatever the input buffer holds (bytes after the last vertex); the B record is not written.

## 7. State the result depends on besides DMEM and the command

* **Stale `v6` (texgen with L == 0 only):** the light loop has one iteration when `L = 0`; the texgen `d_x` is then the value `v6` lane 0 (A) / lane 4 (B) held on entry to the block: at the first pair the entry value of
  `v6` (vector register 6 at the handler's entry), at later pairs the clamped screen x (`+24`) of the previous pair's vertex A / B (`v6` after the previous pair's screen clamp). The spec models both (the harness randomises `v6`).
  With `L >= 32` there is no dependence. A game with only an ambient light and texgen is the only way to see this.
* **F3DLX without G_CLIPPING (bit 23 clear):** the clip-flag computation is skipped. The record's integer position halves become `sat16(P_int + VCO carry)` for the **first pair of an unlit call** (the VCO at the entry: in the normal path VCL clears it, in the lit block the first compare instruction of the light loop clears it), and the clip-flag halfwords are `GPR`-derived junk: `$8` (loaded with 0x460 by the matrix load, then `0x109c` after each pair, or the lit block's `(texgen ? r3 & 8 : 0)`) `| ($14 & 0x7070) << 12 | $13`,
  with `$13` and `$14` the entry values and `$14` updated in place (`$14 = ($14 & 0x7070) << 12` per pair). The spec reproduces all of it (it needs the GPRs at the entry); a C reimplementation can treat +36/+38 as don't care in that mode (not verified that the triangle code ignores them).
* NoN only: clip-flag cross term (section 9).
* Everything else (the other vector registers, accumulator, VCC, VCE, `$2..$7`, `$9..`) is overwritten before use or does not matter (checked by the 16000-call equality including random registers).

## 8. DMEM written (everything) and what is scratch

* `0x106` (halfword) := DMEM[0xba] (same value at every call).
* For every processed vertex: its 40-byte slot, all fields of section 4 (+38..39 of A is not written; +38..39 of B holds the copy of A's flags and is the only leftover).
* Scratch: lit block writes the two normal words to slot A `+0..7` (then A's `+0..7` are overwritten by the clip-space integer halves; B's lit colour is only in a register); with fog B's colour goes through slot A `+24..27` (then overwritten by the screen
  words, +24 is overwritten even when n is odd). Lit colours and texgen leave **no** DMEM residue.
* The input buffer 0xae0.. and the light records are not modified.

## 9. Builds

Entry and lit-block pcs (the pcmap shifts; code between is identical modulo branch targets in the lit block for every build):

| build | handler entry | lit block | back into the vertex loop | slot table | ret halfword |
|---|---|---|---|---|---|
| F3DEX 1.21 (banjo121) | 0x3a8 | 0x72c..0x7c3 | 0x410 | 0x31e | 0xba |
| F3DEX.NoN 1.22 (sf64_non122) | 0x3a8 | 0x74c..0x7e3 | 0x410 | 0x31e | 0xba |
| F3DEX 1.23 (ts2_f3dex123) | 0x3a0 | 0x724..0x7bb | 0x408 | 0x31e | 0xba |
| F3DLX 1.23 (ts2_f3dlx123) | 0x3b0 | 0x74c..0x7e3 | 0x41c | 0x31e | 0xba |
| F3DEX 0.95 (mk64_f3dex95) | 0x430 | 0x7b4..0x84b | 0x498 | 0x31c | 0xbe |
| F3DLX 0.95 (mk64_f3dlx95) | 0x440 | 0x7dc..0x873 | 0x4ac | 0x31c | 0xbe |

(`mk64`: the slot table sits two bytes earlier because of the build's data layout; `$1 = 2*v0` indexes it the same way. The texgen constants 0xa2/0xa4/0xa6, the 0x1a0 bytes, the fog words, the viewport, matrix and light addresses are the same in all builds.)

* **F3DEX 1.21, 1.23, 0.95:** same arithmetic; verified with the same spec.
* **F3DEX.NoN 1.22:** differs only in the clip flags: the first compare has no `X <= -W` bit for z (the mask 0x707 -> 0x703, 0x7070 -> 0x7030) and instead ORs the sign of `W`'s integer half (`mfc2` of lanes 3 / 7): for the pair,
  `flags_B |= (Wi_A < 0) << 2 | (Wi_B < 0) << 6` (the A-sign bit lands in **B's** flag halfword, bit 2 — an apparent quirk of the code that the harness confirms). A's flag bit 6 is never set. Lighting, texgen, fog are identical.
* **F3DLX 1.23 / 0.95:** (a) G_CLIPPING (geometry bit 23): when clear, the clip flags are skipped (section 7); (b) the screen y at +26 is `(y_clamped + 2) & 0xfffc` (A and B), x, z unchanged;
  the colour word store is `sw $15,16($7)` as before. Lighting, texgen, fog, the lit block are identical (`pcmap` all `=`). The "extra halfword" of the F3DLX record is not in the vertex handler's stores (the record is still 0x28 bytes: the rounded y replaces +26).
* **F3DEX 0.95:** the handler is the same as 1.21 (no differences found in the vertex code).

## 10. Edge cases

* `n = 0` handles one vertex; `n` odd: B computed, not stored.
* Negative `D` clamps to 0 per light; dot sums saturate twice (`p0 + p1` then `+ p2`); (-128,-128) normal/direction components give 32767 not 32768.
* `L` must be a multiple of 32 and < 0x2d8 for valid reads (records up to 0x2d0); `L` with bit 31 set takes the recompute path (not covered); other values of `L` (non-multiples of 32) were not explored (the loop then walks off the record grid).
* Texgen without lighting is a no-op; fog does not need lighting; fog replaces the lit alpha.
* No lighting: bytes 12..15 are copied to +16..19 unchanged (fog may replace byte 15).
* Overflow behaviour of the position MAC chain with extreme matrices (clamps of the fraction when the sum before the last integer term leaves [-2^31, 2^31)) is reproduced bit for bit by the formulas of 5.1 (random full-range matrices in the tests).
* `W <= 0`: the reciprocal of a non-positive w gives a raw (possibly negative) `+32`; `Rc_i` is forced to 0x7fff when negative, so the projected coordinates saturate.

## 11. Verification

Harness: `tools/native-f3dex/rt_test`, routine `spec.f3dex.light.vtx` (registered for all six builds, entry at the handler entry, exit pc 0x09c), `spec/light_spec.c` (the arithmetic of this document in scalar C, per vertex) and
`spec/light_gen.inc` (generator: v0, n = 0..32-v0, random vertices with edge values, random/edgy/identity-like matrices with positive w, random viewport / perspNorm / clip ratio / texture scale / fog words,
1..9 light records with random, game-like (LookAt Y colour (0,0x80,0), X zero, 0x1a0 = 0x80,0,0,0) or small colours, light counts 0..8, all combinations of the geometry bits (fog, lighting, texgen, linear), F3DLX clipping bit mostly on).
Random registers / flags / accumulator at entry (the harness). DMEM (entire) and exit pc compared.

* 8000 calls x 2 seeds (11, 12) x 6 builds on the final files: 0 differences (earlier seeds 1, 2, 3, 7 with 6000-8000 calls: 0 differences too).
* Mutation checks (1500 calls each; every mutation is caught): banjo121: fog offset dropped (218 failing calls), fog max replaced by 255 (64), texgen bias 0x3fff (479), linear coefficient 0xa4 -> 0xa2 (279),
  dot-product saturation between the terms removed (219), negative-dot clamp removed (915), previous-dot term of texgen replaced by the current one (533), VMULF rounding term removed (375), clip-flag halves swapped (409),
  A-flags copy at B+38 dropped (1189), reciprocal refinement `Ef` perturbed (39), alpha restore removed (535), reciprocal-int clamp removed (1180), stale-v6 of the texgen dropped (34), odd-count loop exit changed (676);
  F3DLX: rounded-y mask 0xfffc -> 0xfff8 (1205), lit-block `$8` value dropped (31); NoN: sign-bit position changed (845).
* Not verified: the DMA overlay swap path (lighting stub with another overlay resident), the recompute path (declined), `L` not a multiple of 32, `v0 + n > 32`, whether the F3DLX triangle code uses the flags in no-clip mode,
  the SDK-convention statements (fog parameters, LookAt colours) beyond what the data segment shows.
