# The Rej microcodes (F3DLX.Rej, F3DFLX.Rej, F3DEX.Rej) on the task-level path

Four texts of the survey ([ucode-survey.md](ucode-survey.md)) are "Rej" builds: **F3DLX.Rej xbus 2.08** (Excitebike 64), **F3DLX.Rej fifo 2.03** and **F3DFLX.Rej fifo 2.03F** (F-Zero X), **F3DEX.Rej fifo 2.06** (the third of
Resident Evil 2's tasks that `G_LOAD_UCODE` it, and the quarter that starts with it). [ucode-f3dex2-dialects.md](ucode-f3dex2-dialects.md) section 4 called them "a different microcode, not a variant" (about 970 of 1000 words differ from
F3DEX2 2.08): true of the instruction stream, which is scheduled differently throughout, but not of what it computes. This page is what the texts do, written from their instructions (`tools/rsp2c/rsp2c.py --straight`, `tools/hle/rej_img.py`,
the oracles below), and how the front-end ([`hle_rej.c`](../src/hle/hle_rej.c), [`rej_fx.c`](../src/hle/rej_fx.c), [`rej_dmem.h`](../src/hle/rej_dmem.h)) runs them. Everything is checked against translations of the microcode itself
([testing.md](testing.md), "Rej").

The short version: a Rej text is the **F3DEX2 skeleton** (command loop, the state commands, `G_MTX`, `G_MOVEWORD`, `G_MOVEMEM`, `G_DL`, `G_LOAD_UCODE`, the RDP output) with a **vertex code that never clips**: a vertex outside the guard band is
*marked*, and a triangle with a marked vertex is dropped ("rejected", hence the name). The vertex record has no clip coordinates (20 bytes), and the triangle code is F3DEX2's without the clipper.

## 1. The builds

| text | id (4 KiB) | `load_key` (0xf48 bytes) | games | head | vtx | tri1 / tri2 | modvtx / culldl / branchz | dma_io | transport | layout |
|---|---|---|---|---|---|---|---|---|---|---|
| F3DLX.Rej **xbus** 2.08 | `8b0a3e3240821457` | `22fd7ac92dbcdc70` | Excitebike 64 | 0x194 | 0x4a4 | 0x74c / 0x750 | 0xc28 / 0xbec / 0xc10 | 0x1c0 | XBUS ring in DMEM | `l_lay_rej_xbus` |
| F3DLX.Rej fifo 2.03 | `7de803fdbd8fb8b0` | `e3a5e6fee8c43368` | F-Zero X (10 % of its tasks) | 0x18c | 0x4a0 | 0x748 / 0x74c | 0xc24 / 0xbe8 / 0xc0c | 0x1b8 | two 592-byte buffers | `l_lay_rej_fifo203` |
| F3DFLX.Rej fifo 2.03F | `c750be26762c28b0` | `f48949acd3c468d8` | F-Zero X (38 %, and the end of the rest) | 0x18c | 0x45c | 0x704 / 0x708 | 0xbd4 / 0xb98 / 0xbbc | 0x1b8 | two 512-byte buffers | `l_lay_rej_flx` |
| F3DEX.Rej fifo 2.06 | `95129dc7d307dd43` | `08322d13b3ad88cf` | Resident Evil 2 | 0x18c | 0x4c4 | 0x740 / 0x744 | 0xc60 / 0xc24 / 0xc48 | 0x1b8 | two 592-byte buffers | `l_lay_rej_fifo206` |

The handlers below 0x170 and 0x1d0 .. 0x258 are F3DEX2's, at F3DEX2 2.08's pcs in the 2.08 text (head 0x194, copy 0x210, sync 0x214, segcopy 0x220, `G_RDPSETOTHERMODE` 0x240, `G_SETSCISSOR` 0x24c, geometry 0x1d8, enddl 0x1ec, half2
0x200, mtx 0x0f0) and at the 2.04 generation's pcs in the others (0x18c, 0x208, ... : [ucode-f3dex2-dialects.md](ucode-f3dex2-dialects.md) section 5). Their jump tables are at 0x36e (0x30e in F3DFLX) and have the opcodes 0x01 .. 0x07 and 0x08 (nothing) in F3DEX2's places.
The low 0x170 bytes of the code are not in the text: the boot code loads them from behind it (the second descriptor of the data segment, `tools/hle/rej_img.py`).

### 1.1 What differs between the four

| | 2.08 xbus | 2.03 (LX) | 2.03F (FLX) | 2.06 (EX) |
|---|---|---|---|---|
| RDP list | DMEM ring 0x9f8 .. 0xf10 (+ the command's length), `DPC_END` after every command | two buffers, `s6 ^= 0x300`, 592 bytes | two buffers, `s6 ^= 0x350`, 512 bytes | as 2.03 |
| display list buffer | 0x950 .. 0x9f8 | 0x910 .. 0x9b8 | 0x9b0 .. 0xa58 | as 2.03 |
| vertex slots | 64 x 20 bytes from 0x410, table at 0x380 | same | from 0x3b0, table at 0x320 | same as 2.03 |
| cull table | 0x402 | 0x402 | 0x3a2 | 0x402 |
| `G_MOVEMEM` / `G_MOVEWORD` tables | 0x2f0 / 0x2fe | same | 0x290 / 0x29e | same as 2.03 |
| screen y | `(y + 2)` rounded to a whole pixel, mark `0x7ffc` | same | same | **not rounded**, mark `0x7fff` |
| flat shading | colour of the first vertex, each vertex's own alpha | same | colour **and alpha** of the first vertex | as 2.03 |
| lighting | F3DEX2's (section 6.1) | the F3DEX2 loop with unsigned multiplies (6.3) | one light (6.2) | not modelled |

Not different in any of them (the closed forms are shared): the matrix product, the perspective division, the fog byte, the texture coordinate scaling, the triangle setup (section 5) and `G_MODIFYVTX` / `G_CULLDL` / `G_BRANCH_Z` apart from the record offsets.

## 2. Texts that load one another

- **Excitebike 64**: every long task (the ones with a 3D scene) starts with the Rej text and `G_LOAD_UCODE`s a second text near its end: `G_RDPHALF_1` (w1 = data address - 0x180) and `G_LOAD_UCODE` (w0 0xdd0007ff, w1 the text). The second text
  (key `f649a08381045b6b`, 3912 bytes) is **F3DEX.NoN xbus 2.08**: F3DEX2 2.08 with the `.NoN` reject mask 0x70b0, a shorter initialisation (every handler 0x28 lower: vtx 0x4d0, tri 0x794 / 0x78c, modvtx 0xc58, culldl 0xc18, branchz 0xc40) and the XBUS
  ring starting at 0xba8 (`l_b_ex2_208n_xbus`: the F3DEX2 front-end's own vertex, triangle and clipping code runs it; `f2_layout.ring_start`). It was the only reason a tenth of Excitebike's tasks declined ("G_LOAD_UCODE of another ucode").
- **Resident Evil 2**: three of four tasks start with S2DEX 2.06 ([ucode-f3dex2-dialects.md](ucode-f3dex2-dialects.md) section 5.7) and the third of the tasks `G_LOAD_UCODE` the F3DEX.Rej text; a quarter of the tasks start with it.
- **F-Zero X**: a task starts with F3DEX 2.03 (`33dc705a5e4d39ae`, 52 %), F3DFLX.Rej (38 %) or F3DLX.Rej 2.03 (10 %); the F3DEX 2.03 tasks draw their 3D scene and end by loading F3DFLX.Rej (key `f48949acd3c468d8`, the text of the
  second kind of task), which draws the cars (lit, with the reflection table of section 6.2).
  **F3DEX 2.03 is the 2.04 generation** (`l_b_ex2_203`: the pcs of `l_b_ex2_204`, `zex = 1`, `gen204 = 1`). [ucode-f3dex2-dialects.md](ucode-f3dex2-dialects.md) removed it on 2026-10-16 because "the vertex clip-flag high halves were wrong from the
  first `G_VTX`"; that was the entry with `zex` and `gen204` off. With the 2.04 values, 100 seeds of each synthetic case (clip zneg tri mix misc mtx wild) on a captured F-Zero X task are equal to the JIT (section 9).

All of these are `struct f3dex2_build` records of the F3DEX2 front-end ([design.md](design.md#build-switching-g_load_ucode)): the front-end switches builds at the command, the layout (`f2L`), the pcs, the dead ranges follow.

## 3. Memory map where it differs from F3DEX2

(Addresses of the xbus text; the others in section 1.1.)

| | |
|---|---|
| matrices, viewport, perspective norm, clip ratio, fog, texture scales, geometry mode, lights | **as F3DEX2**: MV 0x00, P 0x40, MVP 0x80, 0xdc, viewport 0xe0 / 0xe8, ratio 0x186, fog 0x1e0, scales 0x1e8, geometry 0x1ec, MVP valid 0x1d9, lights valid 0x1dc, size 0x1dd, raw lights 0x1f8, processed 0x200 (24-byte slots) |
| constants | v31 at 0x1b0, v30 at 0x1c0 (`ffff 0004 0008 7f00 fffc 4000 0410 7fff`, `7ffc 1400 1000 0100 fff0 fff8 0010 0020`); lane 6 of v31 is the address of slot 0, lane 0 of v30 the mark `0x7ffc` |
| vertex slot address table | 0x380: 65 halfwords, slot k = 0x410 + 20 k; the record of vertex k is at the table entry k (and a vertex number in a command is the slot x 2 up to 0x7e) |
| input vertices | 16 bytes each (x y z flag s t r g b a / nx ny nz a), DMAed to the end of the slot range rounded down to 8 (`andi s4, s4, 0xff8`) |
| cull table | 0x402: `2222 2200 0022 0000`, by the cull bits of the geometry mode (byte 0x1ee: 0, 2, 4, 6): a bit per sign of the cross product and per triangle of a command (section 5) |
| RDP ring (xbus) | s7 starts at 0x9f8 and wraps to it when it passes 0xf10 (a triangle is at most 0xb0 bytes: 0xfc0) |

## 4. `G_VTX`

Command: `0x01 << 24 | n << 12 | (first + n) * 2`; `1 <= n <= 64`, the end index at most 0x80 and even, the first slot `end/2 - n`. The front-end declines anything else (it is the microcode's table that would be read).
The lights are set up if lit and stale, the MVP matrix computed if stale (both as F3DEX2), then, for each vertex:

1. **P** = the clip coordinates, a 32-bit (integer, fraction) pair per axis: the accumulator chain of F3DEX2 (`fx2_vtx_xform`'s `vtx_one`).
2. **Marked** if one of x, y, z of P is outside +-W2, where W2 is the (saturated, 32-bit) product of P.w and the clip ratio of `G_MV` (the VCH / VCL pair against the scaled w, only the bits of x, y, z: `andi 0x707` / `0x7070`). The ratio is multiplied as a *signed*
   lane: a ratio of 0x8000 or more declines (no game writes one).
3. **Q** = P x the perspective norm; **R0** = `rcp32(Q.w)`; one Newton step, **R1**. Unlike F3DEX2 the integer part of R1 is **not** limited to 0x7fff for a negative w.
4. NDC = P x R1 (the fourth lane takes z in place of w: the fog), x the norm again; the screen position **S** = NDC x viewport scale + translation (lane 3: fog multiplier and offset, +0x7f00 that the readout of the fog byte takes away again).
   **The accumulator starts with `0x7fff * 0x7fff << 16` if the vertex is marked**: every coordinate then saturates at `0x7fff.ffff`, whatever NDC and the viewport are. (With the scales at the ends of the 16-bit range and a saturated NDC the sum can fall back below the
   saturation: the extreme-corner cases of the tests cover it.) With the y rounding (2.08, 2.03): the translation of y is increased by 2 (saturating) and the two low bits of the y integer part are cleared.
5. The record, 20 bytes: `+0` r g b a, `+4` s t (the input's times the `G_TEXTURE` scale >> 16), `+8` sx, `+10` sy, `+12` sz integer part (as computed, no clamp), `+14` its fraction, `+16` 1/w integer part (R1), `+18` fraction. The colour is the input's, or lit (section 6);
   with fog the alpha is `max(S.w integer part, 0x7f00)`'s low byte.

A **marked vertex** therefore has sy = `0x7ffc` (2.08, 2.03) or `0x7fff` (2.06); the triangle code tests `sy >= 0x7ffc` (`slti 32764`).

The odd partner of the vector unit's pairs needs no model: the records have no clip bits, and the partner's stores land on the last vertex's record where the vertex's own stores overwrite them.

## 5. Triangles (`G_TRI1` 0x05, `G_TRI2` 0x06, `G_QUAD` 0x07)

`G_TRI1` is `G_TRI2` with the second word zeroed (the triangle (0, 0, 0): never drawn). **The first word's triangle is drawn first** (F3DEX2 draws the second word's first), each followed by the flush. The vertex bytes are slot x 2; the third is turned into a slot
address arithmetically (`0x410 + 10 b`), the other two through the table: odd or past-the-end numbers decline.

Per triangle (vertices A, B, C in command order):

- **alive**: the y of A, B and C all below `0x7ffc` (signed): `VLT` of the larger of A.y, B.y and `slti` of C.y.
- **cross product** `(A.x - B.x)(A.y - C.y) + (A.x - C.x)(B.y - A.y)` from the saturated 16-bit differences, the sum clamped to 16 bits (`VMUDH`, `VMADH`); `VCH` against 1 makes two bits: negative (bit 1) and positive (bit 9, for the second triangle bits 5 and 13).
- **drawn** iff alive and the cull table word T has the bit of the sign (`T = word at 0x402 + (geometry >> 8 & 0xff)`; no culling `0x2222`, cull front `0x2200` = positive only, cull back `0x0022` = negative only, both 0). A zero cross product is never drawn.
- **the sort** by y: `minAB = A.y < B.y ? A : B`, `maxAB = A.y >= B.y ? A : B`; against C: H = the smaller y of (minAB, C) (a tie goes to C), the other candidate is `mid`; **L = maxAB, M = mid if maxAB.y >= mid.y, else L = mid, M = maxAB**. (F3DEX2 breaks
  the tie of the two larger y values the other way, `M = maxAB`; this changes the winding bit and the slopes of triangles with a horizontal bottom edge. `rej_tri_prepare` has its own sort for it.)
- **setup** as F3DEX2's `fx2_tri_prepare` / `fx2_tri_attrs_*`: the F3DZEX lanes of the constants (reciprocal scale 8 = v31[2], dx scale 0x4000 = v31[5]) but **no clamp on the slopes and no mask on their fractions**; in the texts that round y (2.08, 2.03, 2.03F) **no correction for the fraction of the top vertex's y** (the F3DEX2 setup, and the 2.06 text, add `slope x (y & 3) / 4` to the edges' x; here a record whose y has low bits set, which only a stale slot of an earlier text can hold, gives the plain `x << 14`: mutation 376). The command id is `(g | 0xc8 | tex on) & 0xff`,
  the second byte `0x80` (if the sorted area is negative) | the tile byte; 32 bytes of header, 64 shade, 64 texture, 16 z, as F3DEX2. Colours are the vertices' (flat: the first vertex's; F3DFLX also its alpha).
- the geometry mode must have only defined bits and the `G_TEXTURE` bytes be 0 / 2 and a 6-bit tile, else the task declines (the cull table is indexed by the geometry bits 8 .. 15; the command id has the low byte).

## 6. Lighting

### 6.1 F3DLX.Rej xbus 2.08: F3DEX2's

The setup of the first lit command after the lights changed (`lights_valid` clear, the byte then set to the low byte of w0 *after* the test) and the per-vertex loop are the code of the F3DEX2 lighting overlay ([ucode-f3dex2.md](ucode-f3dex2.md) section 3.4),
so `fx2_light_setup` and `fx2_lit_vertex_scalar` are used as they are (texture generation, linear or not, included).

### 6.2 F3DFLX.Rej 2.03F: one light and a reflection table

The state is at 0x210 (a direction as three s16 lanes), 0x228 (the light's direction as bytes), 0x220 (light colour: the colour bytes twice, 4 bytes apart) and 0x238 (ambient colour, likewise). The number of lights is not read. The first lit
command after a change (`lights_valid` clear; the byte is stored *before* it is tested here) runs the setup of F3DEX2's lights on **two** vectors, the one at 0x210 and the one at 0x228 (bytes << 8): T = d x M(3x3 of the model-view matrix), the sum of the
squares, the reciprocal square root, times the scale at 0x1ce, **stored as s16 lanes** (not bytes) twice, 8 bytes apart, at 0x260 (the first) and 0x270 (the second). (0x250 .. 0x25f is stale register contents and the fourth lane of each copy is unused: dead.)

Per vertex the colour is, per channel: `VMULU` of the ambient byte << 7 and 0x7fff, `VMACU` of the light colour byte << 7 and the lambert term; the lambert term is the **unsigned** dot product of the normal (bytes << 8) and the direction at 0x270 (`VMULU`, `VMACU`, `VMACU`: clamped to
0 .. 0xffff, and then used as a *signed* factor, so a value from 0x8000 is negative); the byte is the result >> 7. The alpha is the input's. With **texture generation** (geometry bit 18) the alpha instead is a table lookup: the high byte of the signed
dot product of the normal and the first direction (0x260) indexes (as a signed number) the 256 bytes at 0x930 - 128 .. 0x930 + 127: F-Zero X loads that table with `G_DMA_IO` (section 7) and uses it for its reflections. (`fl->tab`.)

### 6.3 F3DLX.Rej fifo 2.03

The setup is F3DEX2's, with the scale at 0x1ce. The per-vertex loop is F3DEX2's pairs of slots from the ambient slot down, with the unsigned multiplies: lambert = the unsigned dot product of the normal and the direction (**not** masked to 15 bits),
the running colour `cur` masked to 0x7fff (`VAND`, in the delay slot of the branch to the code for a last single slot, too), then `VMULU cur * 0x7fff`, `VMACU colour(m-1) * lambert(m)`, `VMACU colour(m-2) * lambert(m-1)`, read as an unsigned number; the bytes are
`cur >> 7`; **the alpha is the input's**. The texture coordinates are F3DEX2's, the linear generation with the 2.04 anomaly (`gen204`, [ucode-f3dex2-dialects.md](ucode-f3dex2-dialects.md) section 5.2 item 4).

### 6.4 F3DEX.Rej 2.06

Its lighting is another one again (the setup differs); lit vertices decline (`no_lit`). Resident Evil 2 does not light in the Rej text (500 captured tasks, 8511 of 8512 tasks in 3700 frames).

## 7. The other commands

- **`G_MODIFYVTX` (0x02)**: the word w1 is stored at `(w0 >> 16 & 0xff) - 16 + slot(w0 & 0xfff)`: the offsets of the GBI (0x10 colour, 0x14 st, 0x18 xy, 0x1c z) hit the 20-byte record's +0, +4, +8, +12 (and +16 for 0x20). Other offsets decline.
- **`G_CULLDL` (0x03)**: the display list ends if the y of all the vertices from the slot of w0 to that of w1 is exactly `0x7fff`. Only the 2.06 text marks with 0x7fff; the 2.08 / 2.03 records carry `0x7ffc` (the y is masked), so the command never culls there.
- **`G_BRANCH_Z` (0x04)**: a branch (no stack) to the address `G_RDPHALF_1` left at 0xd8 if the 32-bit word at the record's +12 (the screen z) minus w1 is negative.
- **`G_DMA_IO` (0xd6)**: `(w0 >> 8 & 0xffff) >> 2` is the DMEM address (bit 15 of the halfword: a write to RDRAM; the 13 bits include IMEM), `w0 & 0xff8` the length register, w1 a segment address. F-Zero X reads 256 bytes to 0x8b0 (`w0 = d622c0ff`):
  the reflection table of 6.2. Reads into DMEM outside the microcode's own buffers are done (`f3_dma_rd`); writes to RDRAM, IMEM, the microcode's own buffers and the **vertex slots** (the records are modelled in DMEM with the invariants `G_VTX` gives them; a DMA over them makes records the triangle code was not written for) decline.
- `G_TEXTURE`, `G_MOVEWORD`, `G_MOVEMEM`, `G_MTX`, `G_POPMTX`, `G_DL`, `G_ENDDL`, `G_LOAD_UCODE`, the RDP pass-through and `G_SETOTHERMODE`: the F3DEX2 front-end's, with the layout of the build.

## 8. The RDP list

**XBUS** (Excitebike 64): every command that writes appends at `s7` in the ring and runs the flush routine (0x258): `DPC_END = s7`; if `s7 > 0xf10` wait for START_VALID / END_VALID to clear, `s7 = ring start`, wait for the RDP to leave the start, `DPC_START = DPC_END = ring
start`; then wait while the RDP stands up to 176 bytes ahead of s7. With the video plugin synchronous (it takes every list when `DPC_END` is written, `rdp_defer.c`) none of the waits happens; a state in which one would declines (`rej_flush`). The bytes of the list are
recorded for the replay by `hle_mtc0` (`DPC_STATUS.XBUS`). **fifo**: the F3DEX2 flush with `out_bytes` 592 / 512 and `out_toggle` 0x300 / 0x350.

The dead ranges of the builds (the display list buffer and the RDP output up to 0xfb8, the word the initialisation stores at 0xfc8, the scratch of the lighting) are in `hle_rej.c`; each is proven by `ARMLET_DMEM_POISON` (testing.md).

## 9. Status

See [testing.md](testing.md#rej-f3dlxrej-f3dflxrej-f3dexrej-and-f3dex-203-docsucode-rejmd-2026-10-17) for the evidence. **Claimed**: all four Rej texts (their vertex, triangle, `G_MODIFYVTX`, `G_CULLDL`, `G_BRANCH_Z`, `G_DMA_IO` and lighting as in sections 4 to 8), the F3DEX.NoN xbus 2.08 text Excitebike loads, and F3DEX 2.03 (F-Zero X's first text).
Excitebike 64, F-Zero X and Resident Evil 2 run 100 % of their graphics tasks natively (the first task of a run excepted); the share of graphics RSP time left on the JIT over the 64 games went from 23.4 % to 19.2 % ([ucode-coverage.md](ucode-coverage.md)).

**Stays on the JIT (declines):**
- lit vertices of F3DEX.Rej 2.06 (Resident Evil 2 does not light in that text in 3700 frames; the lighting is another code than the three modelled);
- F3DEX 2.03's lit vertices (the lighting overlay is not in the snapshots, so the case is not testable here; F-Zero X's lit tasks run F3DFLX.Rej);
- a vertex slot or microcode buffer written by `G_DMA_IO`, a `G_DMA_IO` to RDRAM or IMEM, G_VTX counts, end indices, vertex numbers, geometry bits, G_TEXTURE bytes and `G_MOVEWORD` / `G_MOVEMEM` indices no game writes (the same list as the F3DEX2 front-end's "input no game produces");
- XBUS ring states in which the microcode would wait for the RDP (never with the synchronous video plugin).

**Not done: L3DEX 2.08** (WinBack, the line microcode, `cd32d8e5555c1bfc`). It is the same F3DEX2 skeleton with a vertex stage like F3DEX2's (at 0x310) and a triangle command that draws the edges as lines through `G_LINE3D` (pc 0x618, about 270 instructions: its own clip and reciprocal code). It is 0.07 points of the 19.2 left on the JIT (half the tasks of one game, and short ones), so a
third kernel with its oracle, synthetic tasks, mutations and gates was not built; the text stays on the JIT. Nothing found so far makes it unprovable.

## 10. Rej as a derivation family (2026-10-18)

The Rej builds are references of the derivation now ([ucode-derive.md](ucode-derive.md), "Rej as a derivation family"), so a Rej text that no hand-written record covers can be derived, probed and latched. `tools/derive_lib.py` reads
`hle_rej.c`, `rej_dmem.h` and `rej_fx.h` with the other tables (the four Rej builds, F3DEX 2.03 and the F3DEX.NoN xbus 2.08 text Excitebike loads become references: 20 F3DEX2-family references, 35 in all, with their dumps).

**What a Rej record is.** An F3DEX2-family record whose vertex, triangle and cull handlers are in the extension table (`ext`, kinds from `K_REJ_BASE` = 0x60, which the derivation tells from S2DEX's 0x40 ..: `rej_dmem.h`), with the
layout of section 3 (the slots, the cull table, the display list and the output buffers) and the variant bits `f2_layout.rej_flags`. The pcs of the extension table are mapped from the primary reference exactly like S2DEX's (the handler
the reference's jump table sends an opcode to is looked up in the unknown's table); the layout addresses are knobs as before. What is new:

| field | how it is derived |
|---|---|
| `ext` (7 handlers: vertex, `G_TRI1`, `G_TRI2`, `G_MODIFYVTX`, `G_CULLDL`, `G_BRANCH_Z`, `G_DMA_IO`) | the primary reference's table, each pc mapped through the jump tables (S2DEX's mechanism) |
| `rej_flags` (`REJ_YQ`, `REJ_FLAT_A`, `REJ_LIT_FLX`, `REJ_LIT_LX`) | one vote per bit among the Rej references, over the words of the regions the bit owns (YQ: the vertex pipeline and the triangle setup; FLAT_A: the triangle setup; the lightings: the vertex pipeline and the lighting overlay); the references that have the bit and the ones that lack it, the words only the one side explains; no evidence (a tie, or a value no reference shows): the primary reference's bit |
| the composite | a reference of the other class (Rej or not) explains no word of the vertex, triangle, lighting or clip code, and a Rej reference only those whose `rej_flags` agree with the derived bits on the regions each bit owns |
| reference-only words | short runs (under 16 words) of the primary reference's vertex / triangle / lighting code that the unknown lacks, in a region of a bit the primary has and the derived record has not, are "a feature the unknown lacks" (`extra`, NEAR), not "an instruction missing" (`deleted`, NONE): the variant's code |
| `light_scale` | a lane of the constant vector at `const30` (the code names no address): the lane that holds 0x0100, when exactly one does (lane 3 = 0x1c6 in the 2.08 texts, lane 7 = 0x1ce in the 2.03 .. 2.06 ones); an operand that anchors the knob wins |
| `xbus`, `ring_start`, the dead ranges, the ring | **not derived**: the primary reference's (`dbuild_make`); the flush code that depends on them is aligned, so another transport shows as unexplained words (verdict NONE) |
| `no_lit` | the primary reference's (a front-end decision: the lighting of F3DEX.Rej 2.06 is not modelled) |

`dbuild_make` gives such a record the primary reference's `rej_ext_cmd` and the layout with the derived `rej_flags`. The re-entry address of the start-up code (`addi ra, zero, 0x1080` up to the 2.04 generation, `0x1084` behind the `vxor v0` after it) is a
relocation of the derivation, not a layout field (it had made the knob "mvp" ambiguous).

**What the recount's Rej texts derive to** (docs/ucode-derive.md, "Rej as a derivation family", has the numbers): Mario Party 3's F3DLX.Rej fifo 2.08 (`9332f30c`) and the F3DLX.Rej fifo 2.05 text of NBA Jam 2000 and NFL Blitz (`36f10dc3`, loaded by
`G_LOAD_UCODE`) derive **NEAR** from F3DLX.Rej 2.03 with the 2.08 xbus text supplying the lighting words (`rej_flags` = `REJ_YQ`), probe 28 of 28 and latch. ECW's F3DEX.Rej fifo 2.08 (`23cb9e85`) is **NONE** (22 words of the triangle setup that
no reference has: another lane layout of the constant vector; the probe finds 7 of 28 canned tasks different), and Air Boarder 64's and ISS 64's F3DLX.Rej 1.21 (`3c51d836`) is of the **F3DEX 1.x** generation (425 of 1024 words align with F3DEX 1.21, 25 with F3DLX 1.21;
nothing aligns with the F3DEX2 skeleton): the Rej extension exists in the F3DEX2 front-end only.
