# The F3DEX 1.x / F3DLX graphics microcode family: semantics

What the microcode computes, as arithmetic, for the five builds that the target games use. This is the input for the per-build parameter tables and the
shared fixed-point library of the task-level native graphics path; it describes **results** (what is stored to DMEM, what goes to the RDP), not instruction
sequences. Everything stated as a closed form was checked against the exact VU instruction sequence it stands for (`tools/native-f3dex/prim_test.c`, random
inputs, 200 000 trials x 8 lanes per primitive, 0 differences). The per-area documents are in `docs/ucode-f3dex/`; section 2 is the map.

| build | banner | ucode id | games |
|---|---|---|---|
| F3DEX 1.21 | `F3DEX 1.21` | `208a567e9aead5ea` | Banjo-Kazooie |
| F3DEX.NoN 1.22 | `F3DEX.NoN 1.22` | `fd5a31ccf6d40b62` | Star Fox 64 |
| F3DEX 1.23 | `F3DEX 1.23` | `047252e810e35d56` | Toy Story 2 (loaded, not used for 3D in the first minutes) |
| F3DLX 1.23 | `F3DLX 1.23` | `7502cfb7098eeffb` | Toy Story 2 |
| F3DEX 0.95 | `F3DEX 0.95` | `34dad67335a54cda` | Mario Kart 64 |
| F3DLX 0.95 | `F3DLX 0.95` | `5f33f7c2be924de0` | Mario Kart 64 (never seen running in the sessions) |

(ids: FNV-1a-64 over the 4 KiB of text, `gfx_ucode.c`.)

## 1. Fixed-point primitives

The RSP vector unit has eight 16-bit lanes and a 48-bit signed accumulator per lane, held as three 16-bit parts (`HI` bits 47..32, `MD` 31..16, `LO` 15..0).
The microcode keeps 32-bit fixed-point numbers as **two vectors**: an integer half (signed 16 bits) and a fraction half (unsigned 16 bits); the value of the pair is
`I * 65536 + F` as a 32-bit two's-complement number (S15.16 when it is a number with 16 fraction bits; the same layout is used for plain 32-bit integers). The
multiply-accumulate instructions are chained to build products of such pairs. Below, `A`, `B` are such 32-bit values `(ai:af)`, `(bi:bf)`; `a*b` is an exact
integer product; `>>` is an arithmetic shift (floor).

### 1.1 The accumulator and the way results leave it

`acc` is a signed 48-bit integer: every sum is taken modulo 2^48 and sign-extended (a sum of the sizes below cannot reach 2^47 for the matrices and coordinates
of real data; the library must still wrap, the JIT does).

Two clamped views are read out of the accumulator by the instructions that produce a result vector:

* `sclamp(acc)` = `acc >> 16` (bits 47..16, 32 bits) saturated to `[-32768, 32767]`. The result of `VMULF VMACF VMUDH VMADH VMUDM VMADM`.
* `uclamp(acc)` = if `acc` is in `[-2^31, 2^31)` then bits 15..0 of `acc`, else `0xffff` for a positive and `0x0000` for a negative accumulator. The result of
  `VMADN VMADL`. (`VMUDN` returns bits 15..0 and `VMUDL` returns `(a*b)>>16` unclamped: both accumulators are small.)
* `VSAR` reads the accumulator parts raw: element selector 8 = bits 47..32, 9 = bits 31..16, 10 = bits 15..0.

Each instruction of a chain returns its result from the accumulator **as it is at that instruction**, so the intermediate results of a chain are
different from the final one (the fraction read after the `H` term is not the fraction read before it). Where only the final values leave the vector unit, only the
final values are needed.

### 1.2 Products

| chain (VU ops) | accumulator after the chain | results |
|---|---|---|
| `VMUDH a,b ; VMADH c,d` (16 x 16 signed, twice) | `(a*b + c*d) << 16` | `sclamp` = saturated 16 bits of `a*b+c*d`; with `VSAR` 9 and 8 the sum as a 32-bit value (`MD`, `HI`) |
| `VMULF a,b` (rounded fractional) | `2*a*b + 0x8000` | `sclamp` = `(2ab + 0x8000) >> 16` saturated (a = b = -32768 gives 32767) |
| `VMACF c,d` | adds `2*c*d` (no rounding term) | `sclamp` |
| `VMUDM x,c ; VMADN 0` (`x` signed 16, `c` unsigned 16) | `x * c` | integer half `(x*c) >> 16` (the `VMUDM` result), fraction half `(x*c) & 0xffff` (the `VMADN` result) |
| `VMUDL f,c ; VMADM i,c ; VMADN 0` (`A` x unsigned fraction `c`) | `floor(A * c / 65536)` | integer half `sclamp(acc)`, fraction half `uclamp(acc)` |
| `VMUDL ; VMADM ; VMADN ; VMADH [; VMADN 0]` (`A` x `B`) | `floor(A * B / 65536)` (exact: the dropped part is only the low 16 bits of `af*bf`) | integer half `sclamp(acc)` (the `VMADH` result); fraction half `uclamp(acc)` read by the **final** `VMADN 0`; the `VMADN` in the middle of the chain returns `uclamp` of the accumulator without the `ai*bi<<16` term, and the `VMADM` returns `sclamp` of the accumulator without the `af*bi + ai*bi<<16` terms |
| `VMUDN A_f,x ; VMADH A_i,x ; VMADN B_f,y ; VMADH B_i,y` (dot of two `A` with signed 16-bit integers) | `A*x + B*y` (exact, 48 bits) | `sclamp(acc)` from the last `VMADH`; `VSAR` 8 / 9 / 10 give bits 47..32 / 31..16 / 15..0 |
| `VMUDN A_f,2 ; VMADH A_i,2 ; VMADN 0` | `2*A` | `sclamp` / `uclamp`: doubling with saturation of the integer half |

The unsigned/signed roles of the operands matter: `VMUDL` is unsigned x unsigned >> 16, `VMUDM`/`VMADM` signed `vs` x unsigned `vt`, `VMUDN`/`VMADN`
unsigned `vs` x signed `vt`, `VMUDH`/`VMADH` signed x signed, shifted left by 16 into the accumulator.

(`prim_test.c` T1-T5, T11.)

### 1.3 32-bit add, subtract, compare, select

* `VSUBC f1,f2 ; VSUB i1,i2` is a 32-bit subtract: `frac = f1 - f2 (mod 2^16)`, `borrow = f1 < f2`, `int = clamp16(i1 - i2 - borrow)`; the saturation is on the
  integer half only (the fraction half is not adjusted). `VADDC ; VADD` likewise with a carry: `int = clamp16(i1 + i2 + carry)`. (T6)
* `VSUBC ; VLT ; VMRG` and `VSUBC ; VGE ; VMRG` are the 32-bit **min** and **max** of `A` and a broadcast `B`: the integer result is `min`/`max` by the full
  32-bit comparison, the fraction result is the fraction of the chosen value (ties choose `A` for `VGE`, `A` for `VLT` too: the result equals `B` in that case). (T10)
* `VABS x,x` is `|x|` with `-32768 -> 32767`.
* The element selector of the third operand broadcasts: `[0]`/`[1]` the vector itself, `[2]`/`[3]` lanes (0,0,2,2,4,4,6,6) / (1,1,3,3,5,5,7,7), `[4..7]` the
  lane `e-4` of each half (lanes 0..3 take lane `e-4` of the first half, lanes 4..7 lane `e-4+4`), `[8..15]` lane `e-8` to all lanes.

### 1.4 The clip-flag pair

`VCH` on the integer halves followed by `VCL` on the fractions, both with the vertex's `w` (broadcast within each half: `[7]` = lane 3 for lanes 0..3, lane 7 for
lanes 4..7), set `VCC` so that for a lane with the 32-bit value `X` (the coordinate) and `W`:

* "opposite signs" means `X < 0` differs from `W < 0` (the sign of a 32-bit value is the sign of its integer half; zero is positive);
* bit `k` of `VCC` (low byte, lane k): `opposite ? (X + W <= 0) : (W < 0)`;
* bit `8+k` (high byte): `opposite ? (W < 0) : (X - W >= 0)`.

So for `W >= 0` bit `k` is `X <= -W` and bit `8+k` is `X >= W`. The two differ from the plain comparisons only for `W <= 0` (a vertex behind the eye, or exactly 0), where the
documented forms apply. `CFC2 VCC` reads the 16 bits. (T7)

### 1.5 The reciprocal

`VRCPH x_i ; VRCPL x_f ; VRCPH` computes `rcp32(X)` for a 32-bit `X` held as integer half / fraction half: the divide unit takes the 32-bit input, and the result's high and
low halves come out in two destination elements. With `input = X` (signed 32 bit):

```
mask = input >> 31;  data = input ^ mask;  if (input > -32768) data -= mask;
if data == 0:      result = 0x7fffffff
elif input == -32768: result = 0xffff0000
else:  shift = clz32(data);  idx = (((uint64_t)data << shift) & 0x7FC00000) >> 22;
       r = rsp_reciprocal_rom[idx];            // 1024 x 16-bit table, ext/parallel-rsp/rsp/reciprocal.cpp
       result = ((0x10000 | r) << 14) >> (31 - shift);
       result ^= mask
```

(`VRSQ` differs in the index and the shift: `idx = ((idx | 0x200) & 0x3FE) | (shift & 1)`, `result = ((0x10000|r) << 14) >> ((31-shift)>>1)`; the F3DEX 1.x builds
use `VRCP` only.) The 16-bit forms (`VRCP`/`VRSQ` single precision) sign-extend the 16-bit input instead of taking the double-precision input. (T8)

### 1.6 The refined reciprocal (perspective division, triangle area)

The vertex and triangle code compute `1/x` of a 32-bit `X` (in `v29:v28`, i.e. integer half / fraction half) with one Newton step of the S15.16 chains above:

```
R = rcp32(X)                    // 32 bit, integer half Ri, fraction Rf
D = 2*R                         // [VMUDN ; VMADH ; VMADN 0] with the constant 2: Di = sclamp(2*Rf + (2*Ri << 16)), Df = uclamp(same)
T = floor(D * X / 65536)        // the full 32x32 chain; Ti = sclamp(acc); the fraction Tf = uclamp of the accumulator BEFORE the ai*bi term (the VMADN of the chain)
E = 2.0 - T                     // VSUBC / VSUB: Ef = (0 - Tf) mod 2^16; borrow = (Tf != 0); Ei = clamp16(2 - Ti - borrow)
result = floor(D * E / 65536)   // the same chain: integer half sclamp(acc), fraction half uclamp of the accumulator before the ai*bi term
```

It is the sequence of the vertex loop (the reciprocal of `w` for lanes 3 and 7) and of the triangle code (the reciprocal of the area, lane 3). `recip_refine` in
`prim_test.c` is the closed form; T9 compares it with the instruction sequence for random 32-bit `X`.

## 2. Map of this specification

How the pieces were checked: every area has **spec routines** (`tools/native-f3dex/spec/<area>_spec.c`, plain C written from the arithmetic in the documents, registered as
`spec.f3dex.<area>.*`) that `tools/native-f3dex/rt_test` runs next to the microcode on the JIT, from the same random but valid DMEM/register state, comparing the whole DMEM,
the exit pc and (where the area has them) the SP/DPC registers and GPRs. The generators (`<area>_gen.inc`) build states the microcode can really be in at the entry. "Verified"
below means: 2000..20000 random cases per routine per build with 0 differences, and at least one deliberate error in the routine caught. The harness has no RDRAM, so a stretch that
issues an SP DMA is split before the DMA (the DMA is then described by its register arguments); those DMA effects are *read*, not run. "Pcs" in the area documents are IMEM
pcs of F3DEX 1.21 unless a build is named; the "reading kit" they refer to (`kit/dis`, `kit/data`, `kit/uc`) is the disassembly and data-segment dumps generated from the ucode
images by the dev tooling; it is not committed (it is Nintendo's code).

| area | document | spec routines | builds | verified |
|---|---|---|---|---|
| primitives (fixed point, clip-flag pair, reciprocal) | section 1 here | `tools/native-f3dex/prim_test.c` | VU library | 0 differences |
| display-list engine, task start/end, yield, dispatch, G_DL/ENDDL/BRANCH_Z/CULLDL | [ucode-f3dex/dl.md](ucode-f3dex/dl.md) | `spec.f3dex.dl.*` | all 6 | yes (a few stretches by a scratch whole-task driver only: see the document) |
| RDP output buffer, pass-through, flush, FIFO | [ucode-f3dex/rdp.md](ucode-f3dex/rdp.md) | `spec.f3dex.rdp.*` | all 6 | yes; the flush's polling loops and the DMA data are read, not run |
| state commands (geometry mode, TEXTURE, MOVEWORD, MOVEMEM, SETOTHERMODE, segments) | [ucode-f3dex/state.md](ucode-f3dex/state.md) | `spec.f3dex.state.*` | all 6 | yes |
| matrices, MVP, light-direction recompute | [ucode-f3dex/mtx.md](ucode-f3dex/mtx.md) | `spec.f3dex.mtx.*` | all 6 | yes; RDRAM side of push/pop read only |
| G_VTX: transform, clip flags, projection, lighting, texgen, fog | [ucode-f3dex/light.md](ucode-f3dex/light.md) | `spec.f3dex.light.vtx` (also `spec.f3dex.vtx.plain`: unlit subset) | all 6 | yes (recompute of light directions is mtx.md's, declined here) |
| clipping overlay | [ucode-f3dex/clip.md](ucode-f3dex/clip.md) | `spec.f3dex.clip.*` | all 6 | yes (triangle code not run in the stretch) |
| triangle setup (header, edges, shade, texture, z), culling, F3DLX variant | section 5 here | `spec.f3dex.tri.setup` | all 6 | yes (clipped triangles are declined: clip.md) |

Spec routines that decline a state (`ARMLET_NATIVE_DECLINE`) say so in their documents: it is the "needs the clipping overlay" case (triangles), a light recompute pending (vertices),
a DMA-dependent stretch, or undefined opcode cells.

## 3. Memory map (F3DEX 1.2x; deviations of F3DEX 0.95 and F3DLX are in section 6)

DMEM, byte addresses. `$29 = 0x110` is a constant base register of the whole run. The data segment (DMEM 0..0x7ff) is reloaded by the boot code for every task; 0x800..0xfbf
is not (leftovers of the previous task survive there, see section 8).

| address | size | contents | written by / read by | document |
|---|---|---|---|---|
| 0x000..0x027 | 5x8 | DMA table: ucode text, head code, overlays 1..3 | task start | dl |
| 0x030 / 0x040 | 16 / 16 | constant vectors v31 / v30 (loaded with LQV at start): v31 = {0, 1, 2, -1, 0x4000, 4, 0x633, 0x200}, v30 = {0x7fff, 0xfff8, 8, 0x40, 0x20, 0x8000, 0x1cc, 0xcccc} (the same in all six builds) | constant | here |
| 0x050..0x06f | 32 | more constants; 0x060..0x06f = eight halfwords 2 (the "2.0" of the refined reciprocal) | constant | here, light |
| 0x070..0x09f | 48 | clip plane coefficients; clip ratio words RNX 0x74, RNY 0x7c, RPX 0x84, RPY 0x8c | MOVEWORD (clip ratio) | clip, state |
| 0x0a0, 0x0b6, 0x0b8.., 0x0c0.. | | overlay entry pcs, class table (0xb8; 0xbc in 0.95), class-0 table (0xc0; 0xc4), class-2 table (0x76) | constant | dl |
| 0x106..0x10f | | clip resume pc / fan pointer; RDPHALF_1 word 0x10c; yield data pointer 0x108 | clip, state, task start | clip, state, dl |
| 0x110..0x113 | 4 | 1.2x: PERSPNORM word (the vertex and triangle code read the halfword at 0x112, 0xffff by default); 0.95: 0x110 is the DL stack depth byte | MOVEWORD / PERSPNORM | state |
| 0x114 | 4 | **geometry mode** (bit 0 z-buffer, 1 texture, 2 shade, 9 smooth shading, 12 cull front, 13 cull back, 16 fog, 17 lighting, 18 texgen, 19 linear texgen; F3DLX: bit 23 `G_CLIPPING`, default 0x00800000) | SET/CLEARGEOMETRYMODE, TEXTURE | state |
| 0x118 / 0x11c | 4 / 4 | SET_OTHER_MODES words (initial 0xef080cff / 0) | SETOTHERMODE_H/L | state, rdp |
| 0x120 / 0x124 | 4 / 4 | TEXTURE command words (byte 0x122 = level/tile goes into every triangle header; 0x124 = s/t scale used by the vertex code) | TEXTURE | state |
| 0x128 | 4 | RDRAM address of the end of the RDP data flushed so far | flush | rdp |
| 0x12c | 4 | number-of-lights word, bit 31 = light directions stale | MOVEWORD, MTX, POPMTX | mtx, state |
| 0x130..0x15f | | matrix stack pointer 0x134 and limit 0x15c (RDRAM addresses), DL return-stack depth byte 0x148, FIFO start/end 0x150/0x154 | task start | dl, mtx, rdp |
| 0x160..0x19f | 16x4 | segment table | MOVEWORD (segment) | state |
| 0x1a0..0x2df | | light records: raw 16 bytes + derived (model space) 16 bytes per 0x20 slot; lights from 0x1f0 | MOVEMEM, light recompute | mtx, light |
| 0x31e.. | 32x2 | vertex slot address table (0x31c in 0.95) | constant | state |
| 0x360..0x37c | 4x7 | triangle sort tables (section 5.3) | constant | here |
| 0x380 | 16 | viewport: scale[4], translate[4] (s16) | MOVEMEM | state, light |
| 0x390 | 6 | fog multiplier, offset, maximum (0x00ff) | MOVEWORD | state, light |
| 0x398.. | 18x4 | display-list return stack (RDRAM addresses) | G_DL | dl |
| 0x3e0 / 0x420 / 0x460 | 64 each | current modelview, projection, MVP = modelview x projection (Mtx layout: 16 integer halves then 16 fractions) | MTX, POPMTX, MOVEMEM | mtx |
| 0x4a0..0x99f | 32x0x28 | **transformed vertex buffer**, 40-byte records (light.md section 4) | G_VTX | light |
| 0x9a0..0xadf | 320 | display-list window; `$27` read pointer, `$28` end | loop | dl |
| 0xae0.. | up to 0x1b0 | operand of the class-0 command (matrix, vertices, light data), DMAed by the loop before the handler | loop | dl |
| 0xb4, 0xc40..0xc66 | | clip scratch (polygon lists) | clip overlay | clip |
| 0xbe0..0xc3f | | triangle scratch: sorted vertex addresses (0xbe0), perspective-scaled s/t/w per vertex (0xbf0/0xc00/0xc10, fractions +8), texture scale inputs (0xc20/0xc28, 0xc24/0xc2c) | triangle code | section 5 |
| 0xce0..0xd8f | 0xb0 | **RDP output buffer** (`$23` = write pointer, 0xce0 between commands) | pass-through, triangles, SETOTHERMODE | rdp |
| 0xde0..0xe2f | | new vertices of the clipper (0x28 each, up to 12) / matrix-multiply and light-recompute scratch | clip, mtx | clip, mtx |
| 0xfc0..0xfff | 64 | OSTask header | OS | dl |

## 4. Commands, dispatch, per-build opcodes

The loop reads a command (`w0`, `w1`) from the window at 0x9a0 and dispatches on `w0 >> 24` in two levels: class `(op >> 6)` through the class table (0: op 0x00..0x3f via the class-0 table indexed by
`2*op`; 1: 0x40..0x7f nothing; 2: 0x80..0xbf via the class-2 table; 3: 0xc0..0xff RDP pass-through). Class-0 commands first DMA `((w0 & 0x3ff) | 7) + 1` bytes from the
segment-translated `w1` to 0xae0. The class-0 and class-2 tables overlap in DMEM, so a few "opcodes" are aliases and some cells are not handler pointers (undefined: refuse the task in
an HLE). Full 256-entry grids per build and the dispatch registers are in [dl.md](ucode-f3dex/dl.md) section 3.6.

| op | F3DEX 1.21 / NoN / 1.23 / F3DLX | F3DEX 0.95 / F3DLX 0.95 | document |
|---|---|---|---|
| 0x00 | G_SPNOOP | same | dl |
| 0x01 | G_MTX | same | mtx |
| 0x03 | G_MOVEMEM | same | state |
| 0x04 | G_VTX | same | light |
| 0x06 | G_DL | same | dl |
| 0xaf | G_LOAD_UCODE (handler 0xf8c / 0xf80 in F3DLX, in the IMEM tail that stays: native-f3dex.md section 3 has the operands and what it does; run by the task-level front-end on the interpreter) | absent | state |
| 0xb0 | G_BRANCH_Z | no-op | dl |
| 0xb1 | G_TRI2 (two triangles) | same | section 5 |
| 0xb2 | G_MODIFYVTX | G_RDPHALF_2 | state, rdp |
| 0xb3 | G_RDPHALF_2 | G_RDPHALF_1 | rdp |
| 0xb4 | G_RDPHALF_1 | G_PERSPNORM | rdp, state |
| 0xb5 | no-op (G_LINE3D) | G_QUAD (four-vertex handler) | dl |
| 0xb6 / 0xb7 | G_CLEARGEOMETRYMODE / G_SETGEOMETRYMODE | same | state |
| 0xb8 | G_ENDDL | same | dl |
| 0xb9 / 0xba | G_SETOTHERMODE_L / _H | same | state, rdp |
| 0xbb | G_TEXTURE | same | state |
| 0xbc | G_MOVEWORD | same | state |
| 0xbd | G_POPMTX | same | mtx |
| 0xbe | G_CULLDL | same (excludes the last vertex) | dl |
| 0xbf | G_TRI1 | same | section 5 |
| 0xc0..0xff | RDP pass-through (0xfd..0xff: segment-translated address; 0xe4/0xe5 texture rectangle with RDPHALF) | same | rdp |

(The 0.95 numbering of RDPHALF/QUAD/PERSPNORM comes from the microcode's own tables, not from a Mario Kart display list.)

## 5. Triangles (G_TRI1 / G_TRI2): culling, setup, coefficients

Entry: the TRI1/TRI2 handlers load the three vertex addresses (DMEM addresses of vertex-buffer slots) into `$1, $2, $3` (V0, V1, V2 as given in the command), `$4 = V0` (the flat-shading
vertex), and jump to the shared triangle code through the halfword at DMEM 0x396 (entry pc 0x940 in F3DEX 1.21, 0x938 in 1.23, 0x960 in NoN and F3DLX 1.23, 0x9c8 in F3DEX 0.95, 0x9f0 in F3DLX 0.95).
`$23` is the RDP output pointer, `$24` the return pc (0x09c, or the TRI2 return 0x1ec/0x1e4/0x1dc/0x22c/...), `$29 = 0x110`. TRI2 calls the code twice. The code ends by calling the flush (`jal` to
0x128 in F3DEX 1.21 and NoN, 0x120 in F3DEX 1.23, 0x168 in F3DEX 0.95, after setting `$31` to the return pc; F3DLX: `j` to 0x120, 0x168 in F3DLX 0.95), which hands the bytes at 0xce0..`$23` to the RDP (rdp.md section 7). A culled or rejected triangle returns to `$24` without writing anything and without a flush.
All of this section is verified by `spec.f3dex.tri.setup` on all six builds (roughly 40% of the random cases run to the flush; the rest are early exits, or need the clipper and are declined), and the flags below were
exercised in every combination. **Marked "F3DLX:" are the places where the F3DLX triangle code differs; everything else is common.**

### 5.1 Inputs and notation

* Vertex record (40 bytes at the slot address `v`): s16 clip-space position at +0 (x, y, z, w integer halves) and fractions at +8; +0x10 colour bytes R G B A (or the lit colour); +0x14 s16 s, t;
  +0x18 s16 screen x, +0x1a s16 screen y (10.2 fixed point; F3DLX: y was rounded to a multiple of 4 by the vertex code); +0x1c / +0x1e screen z as s16 integer / u16 fraction; +0x20 / +0x22 the
  reciprocal of w as integer / fraction; +0x24 u16 clip flags (light.md section 4, clip.md section 3).
* `g` = geometry mode word (DMEM 0x114); `flags = g & 7` (bit 2 shade, bit 1 texture, bit 0 z-buffer); `smooth = g & 0x200`; `pn` = u16 at DMEM 0x112 (perspective normalisation, 0xffff by default);
  `tile = s8` at DMEM 0x122.
* `sat(a - b)` = 16-bit saturating subtract. `pair(i, f) = i*65536 + f` (i signed 16, f unsigned 16). `mul_c`, `mul_fx`, `mul_mid`, `mul_x`, `mul_s` below are the chains of section 1.2 in the order named by the
  `tri_spec.c` helpers: `mul_c(A, c) = floor(A*c/65536)` for an unsigned 16-bit `c` ([VMUDL, VMADM, VMADN]); `mul_fx(A, B)` = `floor(A*B/65536)` with the fraction read after the integer term ([...VMADH, VMADN 0]);
  `mul_mid(A, B)` = the same product with the **fraction read before the integer x integer term** (no final VMADN: `uclamp` of the accumulator without `ai*bi<<16`, integer half `sclamp` of the whole); `mul_x(x, K)` = `x * pair(K)` as a
  48-bit accumulator with `sclamp`/`uclamp` readouts; `mul_s(A, c)` = `A * c` for a signed 16-bit `c`. The difference between `mul_fx` and `mul_mid` is visible only when a result saturates (then the fraction is 0xffff / 0 or the plain low bits).
* The refined reciprocal `recip_refine` is section 1.6.

### 5.2 Early rejection, area, culling

1. `f_i = u16[v_i + 0x24]`. With the clip test enabled (F3DEX: always; **F3DLX: only if `g & 0x800000`**): if `f0 & f1 & f2 & M != 0` the triangle is rejected (`M = 0x7070`, F3DEX.NoN `0x7030`); else if `(f0|f1|f2) & 0x4343 != 0` the clipping overlay runs (clip.md), after
   which the clipped triangles re-enter this code at the fan re-entry pc.
2. Screen coordinates `(x_i, y_i) = (s16[v_i+0x18], s16[v_i+0x1a])`. `cross = (sat(x2-x0) * sat(y1-y0) + sat(x0-x1) * sat(y2-y0))` (exact 48-bit sum of two 16x16 products; the area pair below is its low 32 bits, wrapped);
   `r17 = clamp16(cross)` is the sign/zero test value.
3. Back-face cull: `(g & 0x2000) && r17 < 0` -> return (no output). Front-face cull happens after the sort: `r17 == 0` (degenerate) -> return; `(g & 0x1000) && r17 >= 0` -> return.
4. F3DLX only: here (after the culls) the code waits for SP DMA idle and writes **DPC_END := DMEM[0x128]** (the RDP is told about everything flushed so far).

### 5.3 Vertex order and the area

`idx = 4*(y2 < y1) + 2*(y1 < y0) + (y0 < y2)` (signed 16-bit compares of the screen y's). The tables at 0x360 (identical in all six builds) give the vertices as H (smallest y), M, L (largest y) and a parity:

| idx | H | M | L | parity |
|---|---|---|---|---|
| 0 | V0 | V1 | V2 | 0 |
| 1 | V0 | V1 | V2 | 0 |
| 2 | V1 | V2 | V0 | 0 |
| 3 | V1 | V0 | V2 | 1 |
| 4 | V2 | V0 | V1 | 0 |
| 5 | V0 | V2 | V1 | 1 |
| 6 | V2 | V1 | V0 | 1 |

(idx 0 and 1: the triangle is already in order; idx 7 cannot occur.) After the sort `$1, $2, $3 = H, M, L`, and the three addresses are also left as halfwords at 0xbe0 (V0, V1, V2 order, scratch).
`area = pair(cross >> 16, cross & 0xffff)`, negated (32-bit subtract with the integer half saturated: `neg(a) = sub(0, a)`) when the parity is 1. The sign of `area.i` becomes bit 7 of header byte 1.
`R = recip_refine(area)` (section 1.6; lane 3 of the vector unit) is the common factor of all derivative coefficients below.

### 5.4 Header and edge coefficients (32 bytes at `$23`, big-endian; `$23 += 32`)

With `(xH, yH)`, `(xM, yM)`, `(xL, yL)` the sorted screen coordinates:

| offset | field |
|---|---|
| +0 | byte `(g & 0xff) | 0xc8` (0xc8 fill, 0xcc shade, 0xca texture, 0xc9 z, combinations) |
| +1 | byte `tile | (area.i < 0 ? 0x80 : 0)` |
| +2, +4, +6 | s16 `yL`, `yM`, `yH` |
| +8 / +10 | XL = `pair(xM*0x4000)`: the middle vertex's x (10.2 screen units times 2^14 = pixels in 16.16) |
| +12 / +14 | DxLDy = slope of edge L-M (integer half limited to +-0x1cc, fraction as computed) |
| +16 / +18 | XH |
| +20 / +22 | DxHDy = slope of edge L-H (limited) |
| +24 / +26 | XM |
| +28 / +30 | DxMDy = slope of edge M-H (limited) |

Slopes. For each edge `k` with `(dx, dy)`: LM `(sat(xL-xM), sat(yL-yM))`, MH `(sat(xM-xH), sat(yM-yH))`, LH `(sat(xL-xH), sat(yL-yH))`:
`rc = rcp32(dy)` (sign-extended 16-bit input, section 1.5); `A = mul_c(rc, 0x20)`; `B = pair(dx*0x4000)` (the 32-bit product: dx in 16.16 pixels);
`S = mul_fx(A, B)` (**F3DLX: `mul_mid`**); `S = mul_c(S, 0x4000)` (divide by 4); the integer half is limited to `[~0x1cc, 0x1cc]` (the clamp keeps the fraction as is). `dy = 0` gives `rcp32 = 0x7fffffff` (and the limit applies).

Starting x. `c_sub = (u16(yH) * 0x4000) & 0xffff` (the fractional pixel row of the top vertex, as a multiplier). F3DEX: `XH = xH16 - mul_c(Sm_LH, c_sub)` and `XM = xH16 - mul_c(Sm_MH, c_sub)`, where `xH16 = pair(xH*0x4000)`
and `Sm = (S.i limited, S.f & 0xfff8)`: **sub-pixel correction of the start x to the first scanline.** **F3DLX: no correction: `XH = XM = xH16`.** `XL = xM16 = pair(xM*0x4000)` in both.

### 5.5 Attribute coefficients (shade, texture, z): 8 lanes

Everything below is computed for eight lanes at once by the microcode; the lanes are independent except for the broadcasts named. Lane meaning: lanes 0..3 = R, G, B, A; lanes 4..6 = s, t, w (perspective-scaled);
lane 7 = z. Only the lanes that are stored matter (shade 0..3, texture 4..7, z 7); the others hold register leftovers and are never observable.

**Attribute values `a_H, a_M, a_L`** (32-bit pairs per lane, for the sorted H, M, L):
* lanes 0..3: integer half = the colour byte (offset +0x10 + lane) of the vertex, fraction 0x8000. Smooth shading: each vertex its own bytes; **flat shading (`smooth == 0`): all three from vertex `$4`** (= V0 of the command; for a clipped triangle still the original V0).
* lane 7: z of the vertex, `pair(s16[v+0x1c], u16[v+0x1e])`.
* lanes 4..6: written by the texture preparation (below) to scratch and read back; stale (previous triangle) if the texture bit is clear, where they are not used. (F3DLX prepares them whether or not the texture bit is set.)

**Texture preparation** (F3DEX: texture bit set; F3DLX: always). For the three vertices in their *command* order `V0, V1, V2`: `w_i = mul_c(pair(w_int, w_frac), pn)` with `w_int = s16[v+6]`, `w_frac = u16[v+0xe]` (`w * pn / 65536`, rounded down);
`mw = min(w0, w1, w2)` (signed 32-bit pair compare); `B = mul_c(mw, 0x8000)` (half of it), stored as s16/u16 at 0xc24 / 0xc2c. For each of H, M, L in order: `K = mul_mid(pair(s16[v+0x20], u16[v+0x22]), B)` (the vertex's `1/w` times `B`) and the three lane values
`x = {s16[v+0x14], s16[v+0x16], 0x7fff}` give `pair_x = mul_x(x, K)` (lane 4 = s, 5 = t, 6 = w; written as int halves to 0xbf0 / 0xc00 / 0xc10 and fractions to +8). Then the largest absolute values of s and t over the three vertices
(`max` of `|int|` taken on the integer half only, ties to the earlier of H, L, M in that comparison order; the fraction of the chosen one, **not** negated) are stored as `(maxS, maxT)` ints at 0xc20 and fractions at 0xc28.
(The halfwords for lane 3 of each stored pair are register leftovers: 0xbf6, 0xbfe, 0xc06, 0xc0e, 0xc16, 0xc1e; never read.)

**Deltas** (per lane l; `A = clamp16(4*sat(yM-yH))`, `B = clamp16(4*sat(yL-yH))`, `C = clamp16(4*sat(xL-xH))`, `D = clamp16(4*sat(xM-xH))`):
```
nx = ((aL - aH) * A + (aH - aM) * B) >> 16        // exact 48-bit accumulator, taken as the raw bits 47..16 (int = bits 47..32 not clamped)
ny = ((aM - aH) * C + (aH - aL) * D) >> 16
DaDx = mul_mid(nx, R)        DaDy = mul_mid(ny, R)
DaDe = DaDy + DaDx * DxHDy   // exactly: acc = pair(DaDy) + (DaDx.f * sl.f >> 16) + DaDx.i * sl.f + DaDx.f * sl.i + (DaDx.i * sl.i << 16), sl = the limited slope of the edge L-H
                             // int = sclamp(acc), fraction = uclamp(acc without the last term)
start = aH - mul_c(DaDe, c_sub)      // F3DLX: start = aH (no correction)
```
(`aX - aY` are 32-bit subtracts with the integer half saturated.)

**Shade (flags & 4):** 64 bytes at `$23`: +0 start ints (lanes 0..3, s16 each), +8 DaDx ints, +16 start fractions, +24 DaDx fractions, +32 DaDe ints, +40 DaDy ints, +48 DaDe fractions, +56 DaDy fractions;
`$23 += 64`. The delta words (+8, +24, +32, +40, +48, +56) are written before the flag test, so they exist as leftovers when no shade is output. **F3DLX:** the start values (+0, +16) are written whether or not the shade bit is set.

**Texture (flags & 2):** 64 bytes at the then-current `$23`, the same layout with lanes 4..7 (s, t, w, z), each coefficient multiplied by the scale `S`:
```
for lane l = 4..7:  T_l = 2 * (|DaDx.i| * 0x800) + |DaDy.i| * 0x800 + mul_c(pair(maxlane_l), 0x800)        // maxlane_4 = (maxS int, frac), _5 = (maxT int, frac), _6 = (B int, frac) from 0xc24/0xc2c, _7 = unused leftovers
                    P_l = (sclamp(T_l), low 16 bits of T_l with the last term excluded)
Mx = max(P_4, P_5, P_6)  (signed 32-bit)          Ms = mul_c(Mx, 0x40)         r = rcp32(Ms)
D = 2*r: Di = sclamp(2*r.f + (2*r.i << 16)), Df = (2*r.f) & 0xffff
S = (Di < 1) ? (Di, Df) : (1, 0)                   // 2/(max) limited to 1.0
out = mul_mid(coefficient, S)  for start, DaDx, DaDy, DaDe  (lanes 4..7)
```
`$23 += 64`. **F3DLX:** the start value is the uncorrected `aH`; and after the texture block the z block (below) is written at the new `$23` even when the z bit is clear (the pointer advances only if it is set).

**Z (flags & 1):** 16 bytes: `+0/+2` z start (int, frac), `+4/+6` DzDx, `+8/+10` DzDe, `+12/+14` DzDy, all lane 7, each scaled by 32: `v' = mul_s(v, 0x20)` for `aH(z)`, `DaDx`, `DaDy`, `DaDe`; start `= aH' - mul_c(DaDe', c_sub)`
(**F3DLX: `aH'`**). `$23 += 16`.

If `flags == 0` nothing follows the 32-byte header. The routine then jumps to the flush. Verified: `spec.f3dex.tri.setup`, all six builds, 20000 cases x 2 seeds each, 0 differences; mutations of the start correction, the texture scale limit, the z scale, the |DaDy| weight, one `A` coefficient, the DPC_END write, the optional clip test, the `mul_mid`/`mul_fx` choice for the F3DLX slope, the sub-pixel removal of F3DLX and the F3DLX trailing z block were each caught.

### 5.6 What is left in DMEM

Besides the output bytes: the sorted vertex addresses (0xbe0..0xbe5), the texture scratch 0xbf0..0xc2f (see 5.5), the output buffer words beyond the final `$23` (shade deltas when only z/texture is output etc.), and register-leftover halfwords where noted. None of it is read by a later command except the scratch
written and read inside the same triangle. (To be established by probing before any of it is masked: section 8.)

## 6. Differences between the builds (summary; details in the area documents)

| topic | F3DEX 1.21 | NoN 1.22 | F3DEX 1.23 | F3DLX 1.23 | F3DEX 0.95 | F3DLX 0.95 |
|---|---|---|---|---|---|---|
| clip reject mask | 0x7070 | **0x7030**; flags and plane 1 differ (clip.md section 9, light.md section 9) | 0x7070 | 0x7070, only with `G_CLIPPING` (bit 23) | 0x7070 | as DLX 1.23 |
| vertex record | 40 bytes | same | same | y at +0x1a rounded `(y+2)&~3` | same as 1.21 | as DLX |
| triangle code | common | common (mask) | common | **F3DLX variant** (section 5: no sub-pixel correction, DPC_END, z block, optional clip) | common | F3DLX variant |
| flush | `jal 0x128` | same | `jal 0x120` | `j 0x120`, different protocol (rdp.md section 9) | `jal 0x168` | `j 0x168`, F3DLX protocol |
| text split / overlay window | 0x720 | 0x740 | 0x718 | 0x740 | 0x7a8 | 0x7d0 |
| opcodes | 1.x numbering | same | same | same | renumbered RDPHALF/MODIFYVTX/PERSPNORM/QUAD; no BRANCH_Z, no LOAD_UCODE | same |
| vertex slot table / DL depth byte | 0x31e / 0x148 | same | same | same | 0x31c / 0x110 | same |
| ucode id | 208a567e9aead5ea | fd5a31ccf6d40b62 | 047252e810e35d56 | 7502cfb7098eeffb | 34dad67335a54cda | 5f33f7c2be924de0 |

All other handler code is identical instruction for instruction across the six builds (pcmap), only the pcs and a few DMEM constants differ; the per-area documents list the pcs.

Four registered builds came after this table (F3DLX 1.21 `76c763bc`, F3DLX.NoN 1.21 `0a7cc6c9` and 1.23 `cc09096c`, F3DEX.NoN 1.00 `5063f108` of Turok, whose class tables lie at 0xbc / 0xc4 as in 0.95 but
with the 1.2x opcodes and no G_BRANCH_Z / G_LOAD_UCODE), and the versions differ in a few words that do not change the semantics (1.21 / 1.22 return from the display-list window refill through a saved
`ra`, 1.23 tail-jumps to the DMA routine). The front-end holds every difference above as data of the build's record ([native-f3dex.md](native-f3dex.md) section 3), and the derivation reads it from a
text ([ucode-derive.md](ucode-derive.md), "F3DEX 1.x records that run").

## 7. Facts that matter to an HLE front-end

* **State carried between commands in registers** (not in DMEM): `$23` (always 0xce0 at command boundaries), `$26/$27/$28` (DL window), `$29`; the vertex code depends on stale vector registers for `L == 0` texgen and for the second pair (light.md section 10); the clipper reads stale `$10/$20`
  (dead list slots only); F3DLX clipping off leaves stale flag halfwords in new vertices (light.md section 10). An HLE that wants bit-exact DMEM has to model or mask those; the outputs that reach the RDP do not depend on them.
* **RDP flush per command.** Every RDP-producing command flushes at once (no threshold); `$23` is 0xce0 at command boundaries. F3DLX protocol: DPC_END is written by the callers (rdp.md section 9).
* **Yield and end of task**: the end overlay writes SP_STATUS; a yield saves DMEM 0..0xbff (3072 bytes) to the OSTask `yield_data_ptr` (dl.md section 6).
* **G_LOAD_UCODE** (read from the code, then run on the interpreter and the JIT by the synthetic tasks of `tools/hle/mksyn_ex1.py`): `w1` = text address, `G_RDPHALF_1`'s word (DMEM 0x10c) = data address, `w0 & 0xffff` = the data DMA length register. The handler saves the next command's address (`k0 + k1 - gp`) in OSTask.data_ptr (0xff0) and the text address in OSTask.ucode (0xfd0), DMAs the first 0xf18 bytes of the text to IMEM 0x80 and the data to DMEM 0 (F3DEX2: 0xf48 bytes and 0x180), waits and jumps to IMEM 0x80: the new microcode starts as a fresh task at the saved display list position. IMEM 0 .. 0x7f and 0xf98 .. 0xfff (text bytes 0xf80 .. 0xfff and 0xf18 .. 0xf7f: the loop head and the DMA routines) stay.
* **Undefined behaviour**: undefined opcode cells, DL stack overflow beyond 18 entries (silently dropped call), matrix stack pointer at the limit (push refused, matrix still applied): an HLE should refuse or reproduce the documented behaviour.

## 8. Leftover DMEM: mask candidates (NOT established)

Per the project rule, a mask is allowed only after (a) probing (poisoning the bytes and showing nothing changes), (b) validating with POISON and `tools/equiv` hash streams on all target games and (c) `verify` on the devices. The list below is only what the
documents' reading says is **written and not read again**; none of it has been probed in a game yet. DMEM 0..0xbff is also what a yield saves and restores (so a difference there would reach the saved image, but only where the bytes are dead).

| range | what | evidence | risk |
|---|---|---|---|
| 0xce0..0xd8f beyond the data to be flushed | RDP output buffer leftovers | rdp.md section 7: only `len` fresh bytes are DMAed; the yield saves only 0..0xbff | low |
| 0xbe0..0xc3f | triangle scratch (5.6); inside it 0xbe0..0xbff is within the yield image | written before read in every triangle | low; 0xbe4..0xbf0 are rewritten by the yield itself |
| 0xbf6, 0xbfe, 0xc06, 0xc0e, 0xc16, 0xc1e, 0xc20..0xc23, 0xc28..0xc2b (non-textured triangles) | register-leftover halfwords | values come from stale vector registers; the next textured triangle rewrites what it reads | none (cannot be reproduced anyway) |
| 0xc40..0xc66, 0xb4, 0x106, 0x158 | clipper lists and bookkeeping | clip.md section 8: dead after the fan | low |
| 0xde0..0xe2f | clipper new vertices (needed until the fan finishes), matrix multiply product, light-recompute scratch | mtx.md section 5, clip.md section 8 | assumes the RDP buffer (0xce0..0xd8f) is empty when the matrix scratch is written (0xde0 is above it) |
| vertex records, bytes +0x26..+0x27 and B's copy of A's clip flags | vertex-pair leftover | light.md section 8 | low |
| 0x130, 0x138 | no reader found in any overlay | state.md, dl.md | low (unconfirmed) |
| 0xae0.. after the handler | command operand | dl.md section 8 | none |
| stale entries above the DL stack depth | | dl.md section 8 | none |
| 0x800..0xfbf across tasks | not reloaded by the boot code | to be checked: vertex slots 21..31 live in 0x800+ and survive into the next task, where no valid display list reads a slot before filling it | to probe |

The disassembly printed some stores of zero as `nop` (SB/SH/SW with `$0` as the source): the three occurrences relevant here are in the overlay stubs and the clipper (`sw $0, 0xfc4($0)` in the window at 0x754 of F3DEX 1.21, `sh $0, 0xc46($0)`
at the clipper entry and `sh $0, 2($14)` after each new vertex, with equivalents at shifted pcs in the other builds); the 0x940..0xf30 triangle code is not affected.

## 9. Status and what is not verified

* Verified: everything marked in section 2 (289 spec-routine runs over the six builds, 2000 cases each, 0 BAD in the last full run; more cases and seeds in the area documents).
* Not verified by the spec routines: pieces that involve SP DMA or RDRAM (the harness has none; G_LOAD_UCODE is covered by the replay tests of docs/testing.md, "F3DEX 1.x G_LOAD_UCODE"): the RDRAM side of matrix push/pop, the flush's DMA data and polling loops, the prefetch DMA's RDRAM reads; the yield/resume path other than by the scratch driver in dl.md;
  F3DLX 0.95 behaviour that was only read from the shared code (`mk64_f3dlx95` passes the same spec routines but was never seen running a game); MK64's RDPHALF/PERSPNORM opcode numbers (tables, not a Mario Kart list).
* Facts taken from SDK knowledge rather than from the microcode: the names of geometry-mode bits, the fog parameter convention, the LookAt colours (light.md section 11).
* The leftover-DMEM table in section 8 is a candidate list, not a mask list.
