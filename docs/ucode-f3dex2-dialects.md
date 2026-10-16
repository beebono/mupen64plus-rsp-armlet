# F3DEX2 dialects on the task-level path

[ucode-f3dex2.md](ucode-f3dex2.md) describes the four builds the F3DEX2 front-end was written from (Donkey Kong 64, Paper Mario, Ocarina of Time, Pokemon Snap). This page is what was found when
the survey's other F3DEX2 texts ([ucode-survey.md](ucode-survey.md)) were put through the same front-end, which builds are claimed, which are not and why. Each build is a table entry in
`src/hle/hle_f3dex2_dialects.c` (its own translation unit; the types it shares with `hle_f3dex2.c` are in `hle_f3dex2_int.h`); a build that moved the microcode's DMEM state has a `struct f2_layout` of its own (`f3dex2_dmem.h`), a build with other
handler pcs for the commands below 0x170 a `struct f2_pcs`, and commands the standard front-end does not have are run by the build's `ext_cmd` hook (`hle_f3dex2_int.h`, `K_EXT`).

## 1. Claimed

| id | build | games | what is different from the four |
|---|---|---|---|
| `ac310503c83b23ea`, `d2d1f9f2e7dfe1ab` | F3DEX 2.08 | Dr. Mario 64, RR64; WinBack (the main text), 007 TWINE, Hot Wheels, Hydro Thunder, Mario Golf / Tennis, Ogre Battle | only pcs (vertex 0x4f8, triangles 0x7bc / 0x7b4, MODIFYVTX 0xc80, CULLDL 0xc40, BRANCH_Z 0xc68); the two texts have identical code |
| `33dc705a5e4d39ae` | F3DEX 2.03 | F-Zero X (its first text) | pcs again (the old build: vertex 0x4e8, triangles 0x794 / 0x78c, ...). Registered, but F-Zero X's tasks start with this text and `G_LOAD_UCODE` the F3DLX.Rej text in the same task (section 3), so they decline |
| `e80e52925f70b674` | F3DEXBG.NoN 2.08 | Conker's Bad Fur Day | section 2 |
| `5dce3244420ce715` | F3DEX 2.04 | Buck Bumble, Cruis'n World | the generation before 2.05: the triangle code of F3DZEX, an unclamped screen z, another clipper tail, a lighting overlay with a slip; section 5 |
| `e857446754069ab5` | F3DEX 2.04H | Super Smash Bros.; Kirby 64 (every task declines, section 5.6) | 2.04 with `G_SPECIAL_1` (0xd5) added: 24 bytes inserted at 0x1b8, the pcs above it +0x18; section 5 |

## 2. F3DEXBG.NoN 2.08

The code of F3DEX2.NoN 2.08 with the DMEM state moved, a smaller output-buffer threshold, `G_TRI4`, and its own lighting.

**2.1 Layout.** The structures between 0x1d0 and 0x2e0 moved up by 0x60 (the constant vectors, the matrix-valid flag) or 0x68 (fog, the texture words, the geometry mode), everything above by
0x260 / 0x298. `f2_layout` holds them by name; `l_lay_bg` in `hle_f3dex2_dialects.c`:

| | standard | F3DEXBG |
|---|---|---|
| jump table (by signed opcode) | 0x36e | 0x5d6 |
| vertex slot address table | 0x380 | 0x618 |
| display list input buffer / its end | 0x920 / 0x9c8 | 0xbb8 / 0xc60 |
| geometry mode word, texture words, fog | 0x1ec, 0x1e4, 0x1e0 | 0x254, 0x24c, 0x248 |
| MVP-valid byte, lights-valid byte, lights-size byte | 0x1d9, 0x1dc, 0x1dd | 0x239, 0x23c, 0x23f |
| constant vectors v31 / v30, texgen polynomial | 0x1b0 / 0x1c0, 0x1d0 | 0x210 / 0x220, 0x230 |
| clip mask word, cull table, clip plane masks | 0x3cc, 0x3c2, 0x3f8 | 0x664, 0x65a, 0x690 |
| G_MOVEMEM table, G_MOVEWORD index arithmetic (`lhu at, N(v0)`) | 0x2f0, N = 10238 | 0x550, N = 10848 |
| overlay descriptors (light / clip) | 0x410 / 0x418 | 0x6a8 / 0x6b0 |
| light slots (raw / processed) | 24 bytes at 0x1f8 / 0x200 | **48** bytes at 0x268 / 0x278 |

The values were found from the code (the immediates that differ in an alignment against 2.08, `tools/ucode_survey.py` and the tables in the data segment) and confirmed by the replay of 289 captured tasks.

**2.2 Output buffer.** The flush threshold is 16 bytes instead of 344: the two buffers are 320 apart (`s6 ^= 0x140`), `s7 = s6 - 16` after the toggle, the DMA length is `s7 - s6 + 16`. With the start of the task at
`s6 = 0xe50, s7 = 0xe40` the buffers are [0xe40, 0xf00) and [0xf00, 0xfc0) including the 0xb0 of slack of a triangle. Dead at the end of a task (poisoned, the pictures and the audio identical): the input buffer
and the clipper's new vertices 0xbb8 .. 0xe40, the output buffers to 0xfb8, the clipper's polygon lists 0x662 .. 0x6a0, the OSTask word 0xfc8, the pad bytes of the light slots.

**2.3 Triangles.** The reject mask is 0x70b0: the w lane (bit 7) instead of z (bit 6) of the two flag halves. `G_TRI1` (0xa70), `G_TRI2` / `G_QUAD` (0xa68) are the standard ones. **`G_TRI4`** (opcodes 0x10 .. 0x1f, handler 0x994):
four triangles in one command, each vertex number a 5-bit field stored times 2 (a 6-bit mask on `x >> n`):

```
T0 = (w1 >> 24, w1 >> 19, w1 >> 14)    T1 = (w1 >> 9, w1 >> 4, w1 << 1)
T2 = (w0 >> 9,  w0 >> 4,  w0 << 1)     T3 = (w0 >> 22, w0 >> 17, ((w0 >> 12) & 56) | ((w1 >> 29) & 6))     (each & 62)
```

drawn in that order through the usual triangle code (each followed by the flush check).

**2.4 Lighting.** The vertex pipeline (matrix, projection, clip flags, viewport) is the F3DEX2 one: the records a lit command leaves are those of `fx2_vtx_xform` except the colour and, with texture generation, s and t. The lighting stage
is `src/hle/f3dexbg_fx.c` (`fbg_lit_pass`, pure C on structs; the DMEM adapters and the whole command are `f3dexbg_dmem.h`). What the microcode does, per vertex (the pair loop runs two vertices in the two halves of the vector unit):

- **Inputs.** The normal is (x, y) from the halfword table at 0x510 (`G_MOVEMEM` 14, indexed by the vertex's buffer slot) and z from the vertex's pad byte 7; pad byte 6 negative means "not lit" (the colour stays); the vertex colour is the base
  colour the lit result is multiplied by (the alpha is kept). Light slots are 48 bytes: raw direction at +0, attenuation byte at +4, the setup's normalised direction at +16 (twice), position (x y z w, s16, twice) at +24, colour at +40 (twice).
  The setup (first lit command after the lights changed) is the F3DEX2 one (`fx2_light_setup`) with that stride; `lights_size` (halfword at 0x23e) is 48 per light plus the ambient slot, and the loop below needs an odd number of lights (an even one never ends on the microcode).
- **View position.** The clip coordinates (integer and fraction, z first moved by the constant at 0x1f0) times per-axis 32-bit constants (0x1d0 high, 0x1e0 low halves), the integer half of the product: the eye-space position the projection came from.
- **Lights.** Start from the ambient colour (slot top = lights + 1). The top light (direction of slot top, colour of slot top - 1) is lambert-weighted with no attenuation, the lambert term not masked to 15 bits. The others go in pairs (slots m, m - 1;
  direction and position of the slot, colour of the slot below): d = view position - light position, D2 = sum of the squares as a saturating 32-bit sum (four lanes, w included), R = reciprocal(D2), F = R x (a / 16) (the attenuation byte: high nibble integer,
  low nibble sixteenths), colour' = min(colour x F, colour). Positional lighting (geometry mode bit 6): colour = colour x 0x7fff + colour' x lambert for each light of the pair; without it the attenuated colours are just added.
- **Result.** colour x vertex colour (rounded), byte = lane >> 7. Texture generation: s = 0x4000 + (n . lookat0) / 2, t likewise with lookat1 (slots 0 and 1), or the F3DEX2 polynomial with the linear bit.
- **Scratch the microcode leaves** (reproduced, the verification compares them): the MVP integer rows 0 and 1 twice at 0x1b0 / 0x1c0, the last pair's clip-coordinate fractions (halves swapped) at 0x200, its input colours at 0x258.

It reproduces all 289 + 313 captured Conker tasks, 300 000 randomised commands (lights, positions, attenuations, normals, flags, geometry bits, counts, matrices) against the microcode's transliteration, and the verify runs
of docs/testing.md. On x86 the lit tasks run 3.4 to 3.9 times faster than the JIT (the transliteration was 0.8 times), so lit vertices are native by default.

## 3. `G_LOAD_UCODE` (0xdd) inside a task

The command's handler (pc 0xfac in every F3DEX2 build) is the microcode's way of loading another ucode text and data and continuing in it: the exit stub (from the descriptor at 0x2e0) flushes the output
buffer, stores the display list position in the OSTask header, DMAs the new text to IMEM 0x80 and the new data to DMEM 0x180 (the address of the data is the previous `G_RDPHALF_1` word), and jumps to the new text's
initialisation, which finds the rest of the display list. Rare's Conker (BG) ends *every* task this way, loading its own text again (a re-initialisation of the DMEM state); F-Zero X loads the F3DLX.Rej text.

The front-end hands the command to the interpreter (the stub and the initialisation of the loaded text, about 400 instructions) when the text loaded is identical to the text the task started with (`memcmp` of the 0xf48
bytes in RDRAM), and continues the native loop at the loop head the new initialisation reaches (the jump table, the overlay in IMEM and the registers are re-read). Any other text declines the task: it would run the
rest of the task on the interpreter, three times slower than the JIT.

## 4. Not claimed

| build | games | why |
|---|---|---|
| F3DLX.Rej 2.08 `f67036e9d2a701c3`, 2.03 `7de803fdbd8fb8b0`, F3DFLX.Rej 2.03F `c750be26762c28b0` | Excitebike 64; F-Zero X | a different microcode, not a variant: 20-byte vertex records (no clip coordinates: the "Rej" builds reject instead of clip), another triangle code that splits a triangle crossing the near plane in two, another lighting code (directional only), other boot and DMEM layout. About 970 of 1000 instruction words differ from F3DEX2 2.08 (`tools/ucode_survey.py`). F3DFLX has its own jump table layout. |
| L3DEX 2.08 `cd32d8e5555c1bfc` | WinBack (every second task) | the line microcode: `G_LINE3D` and a triangle code that draws lines; 763 words differ from F3DEX2 2.08. |

## 5. F3DEX2 2.04 and 2.04H

The two texts of the survey's "F3DEX2 2.04 / 2.04H" cluster ([ucode-survey.md](ucode-survey.md): 96 % against the registered builds, "20 to 40 real words" in the vertex and triangle code). 2.04 is the older generation: the text is the standard F3DEX2
skeleton (same DMEM layout as Donkey Kong 64: `l_lay_std`, the 344-byte output buffers, the loop head at 0x18c, the handlers below 0x170 at the standard pcs) and its handlers sit at the pcs of the 2.03 table (vertex 0x4e8, `G_TRI1` 0x794, `G_TRI2` 0x78c, ...). 2.04H
(Kirby 64, Super Smash Bros.) is the same code plus one handler: `G_SPECIAL_1` (0xd5) at 0x1b8, the "concatenate the matrices now and set the valid byte to the low byte of w0" command that F3DEX.NoN 2.08H (Pokemon Snap) has at 0x1c0 (the front-end's `K_MVP`); everything from 0x1b8 on
moves up by 0x18 (copy 0x220, sync 0x224, segcopy 0x230, `G_RDPSETOTHERMODE` 0x250, `G_SETSCISSOR` 0x25c, geometry mode 0x1e8, `G_ENDDL` 0x1fc, `G_RDPHALF_2` 0x210, vertex 0x500, triangles 0x7ac / 0x7a4, `G_CULLDL` 0xc2c, `G_BRANCH_Z` 0xc54, `G_MODIFYVTX` 0xc6c). The two builds are
table entries (`l_b_ex2_204`, `l_b_ex2_204h` in `hle_f3dex2_dialects.c`); the pcs are read off the jump table in the data segment, the differences below off the instruction streams (`tools/native/disasm.py`, an alignment of the L3-normalised words like `tools/ucode_survey.py`'s).

**5.1 What is the same as which build.** 2.04 against Donkey Kong 64 (2.07): 34 blocks differ. Against Ocarina of Time's F3DZEX.NoN 2.06H only 20 do, and none of them in the triangle code. The triangle code, its constants (the data's vector constants at
0x1b0 / 0x1c0 are F3DZEX's, not 2.07's), the slope handling (no mask on the fractions), the polynomial of the lighting at 0x1ce, the lighting overlay and the clipper's six-plane loop are F3DZEX's. So the front-end's existing `zex` parameter set (the constants are read from the data) carries the triangle
setup; only the reject mask is the standard 0x7070. The 2.03 text (F-Zero X) is 2.04 with a few lane choices of the vertex code permuted; its entry (`l_b_ex2_203`) is left as it is: the only 2.03 task of F-Zero X that is native-eligible has no triangle, and its triangle constants would need `zex` (not verified, the task
that matters declines: section 3).

**5.2 What differs from 2.05 and later, found and how it is handled.** Each is a field of the builds' `gen204` flag; all of them are small closed-form changes inside the existing functions, none needs new kernels.

| # | where | 2.05+ (what the front-end did) | 2.04 / 2.04H | handled in |
|---|---|---|---|---|
| 1 | vertex loop's stores (0x5f4 ... 0x73c; `G_VTX` and the new vertices of the clipper) | the screen z's integer part is `max(z, 0)` (`VGE v3, v25, v0`; `SSV v3`) | `SDV v25` stores the unclamped integer part together with x and y; the fraction is the same. A vertex behind the z = 0 plane of the viewport (negative `z / w * scale + trans`) keeps a negative z in its record, so the triangle's z block, `G_BRANCH_Z` and the clipper see it | `fx2_vctx.gen204` (`vtx_project`; the NEON vertex form: `NMUT(6)`) |
| 2 | the clipper, intersection of an edge with a plane (0x394 ...) | the sign of the doubled reciprocal is that of `(int | 1)` of the distance difference's integer part (`VOR v29, v11, v1` first): never 0 | `VABS v29, v11, v25[3]` on the integer part itself: a distance difference below 1.0 (integer part 0) gives 0, the reciprocal's scale is 0, `t` becomes the minimum (1) | `fx2_clip_ctx.sign0` |
| 3 | the clipper, the fan (0x4b4 ...) | triangles (L[k], L[k+1], L[n-1]), k = 0 .. n-3 | (L[0], L[n-2-k], L[n-1-k]), k = 0 .. n-3: the first vertex of the polygon is the pivot and the last triangle comes first. The order of the vertices within a triangle decides the flat colour, the winding and the ties of the sort | `fx2_clip_ctx.fan_l0` |
| 4 | the lighting overlay, linear texture generation (overlay offset 0x1d0) | `VMUDH v21, v1, v31[13]` restarts the accumulator at 0x4000 and writes v21 | the same instruction writes **v22**, which holds x: the following `VMACF v22, v22, c1` then multiplies 0x4000 (not x) with c1, so the linear term of the polynomial is `2 * 0x4000 * c1` | `fx2_texgen_linear` (`gen204`) |
| 5 | triangle setup | F3DEX2 constants (rcp 0x20, dx 0x1000, limit 0x100, fraction mask) | F3DZEX's lanes (8, 0x4000, 0x1cc, none), read from the data | `zex` |

Found in the same work, **not 2.04-specific** (they were in the front-end for every F3DEX2 build; the captured games never reached them, the synthetic tasks of section 5.5 did): the flat-shading colour of the triangles the clipper makes comes from the vertex the *original* triangle command started with (the microcode's `a0`, set at
the triangle entry, is not set again on the clipper's way in), not from the first vertex of the fan triangle (`fx2_tri_in.flat`); and an odd vertex count that ends at the last vertex slot (31) computes its phantom partner from the first bytes of the display list's current chunk at 0x920, which the lean mode does not keep in DMEM
(`cmd_vtx` reads them from RDRAM: `k0 - 168`).

**5.3 Dead ranges.** The standard table (`F3DEX2_DEAD_RANGES`). Cruis'n World's RDP ring (RDRAM 0x353380 ...) overlaps the last 0x120 bytes of its ucode data (0x352ca0 + 0x800): the lists a task writes there come back as data at the start of the next task, into DMEM [0x6e0, 0x800) (vertex slots 17 to 22, written
before they are read), and the lists differ from the JIT's in the stale second words of the sync commands. Those bytes of the next task's DMEM differ too, so [0x6e0, 0x800) is dead for `5dce` (the hash stream of the equivalence run is identical with it; `ARMLET_DMEM_POISON=mask` and `ring` PASS in Cruis'n World and Buck Bumble). Super Smash Bros.
and Buck Bumble do not overlap.

**5.4 The identity hash does not cover the lighting overlay.** The ucode id is the FNV of the first 4 KiB; the lighting overlay is stored behind it (`desc_light` points at text + 0x1188, 0x208 bytes), so it is neither hashed nor in the survey's dumps. The 2.04 overlay was read from the task snapshots (the overlay DMA's rows) and compared with Ocarina of
Time's and Donkey Kong 64's (one instruction: difference 4 above, and the light scale lane of 2.07).

**5.5 Tests.** Captured tasks (`ARMLET_HLE_CAPTURE`): 150 + 337 (every task of the first 900 frames) + 150 of Buck Bumble, Cruis'n World and Super Smash Bros. equal on x86_64, aarch64 NEON and aarch64 scalar. Synthetic tasks (`tools/hle/mksyn_ex2.py`: random matrices of scales down to 1/1024, vertices in batches, triangles
crossing the clip planes with short edges, negative screen z, lit vertices with 1 to 5 lights / look-at / fog / both texture generations, and G_CULLDL, G_BRANCH_Z, G_MODIFYVTX, G_QUAD, G_SPECIAL_1, G_POPMTX, calls and branches), the JIT run from the same state (`--oracle`, `tools/hle/synth_ex2.sh`). Mutations 210 .. 216 (`tools/hle/mutcheck_204.sh`): each of the differences above,
the phantom vertex, the flat colour and the valid byte of the matrix concatenation (`G_SPECIAL_1`), all caught ([testing.md](testing.md), "F3DEX2 2.04 and 2.04H").

**5.6 Kirby 64.** Its tasks start with the 2.04H text (claimed) and every one declines: each ends with `G_LOAD_UCODE` of a different text, which is S2DEX (the survey saw only the first text of a task). The loaded text (3912 bytes, its data, 1664 bytes, goes to DMEM 0x180) is the code of the registered S2DEX 2.08 build
(Worms): an alignment against it differs in the initialisation (one vector instruction), a nop, and four immediates, and its handlers from 0x184 on sit 8 bytes lower (loop head 0x1a4, `G_OBJ_LOADTXTR` 0x550, `G_BG_1CYC` 0xb74, `G_BG_COPY` 0x208, `G_OBJ_RECTANGLE` 0xb80, `G_SETSCISSOR` 0x1d0). Its display lists contain `G_BG_1CYC`,
`G_BG_COPY` (the S2DEX front-end declines it: [ucode-s2dex2.md](ucode-s2dex2.md)), `G_OBJ_RENDERMODE` and `G_OBJ_LOADTXTR`. What it would take: the native loop switching to another build in the middle of a task (the S2DEX layout, pcs, overlays and dead ranges, and the union of the two builds' dead ranges in the entry the
task starts with, since the equivalence shim masks by the first text), a build record for the shifted pcs, and `G_BG_COPY`. The rest of a task on the interpreter is three times slower than the JIT, so the task declines as a whole. Kirby is therefore 0 % native. To keep the registration from costing time (the native work before the command would be thrown away every task), the front-end remembers the text and data pointers of a task that declined at
such a command and declines the next tasks with the same pair at once (every 64th runs again): 692 of 703 Kirby tasks in the first 1500 frames decline before the boot code runs. F-Zero X's tasks do the same.
