# Indiana Jones and the Infernal Machine's microcode

One build with no version banner (`a6c64e8d82ca6346`; the id is the hash of the first `ucode_size` = 0xec bytes of the text, which are a loader), no data segment, 1682 graphics tasks in the survey's 55 s, every
graphics task of the game. A Fast3D-style microcode (vertex transform with clip flags, an RDP triangle builder, a display list stack, the F3D opcodes 0xb4 .. 0xbf and the RDP commands as they are) with a
command encoding of its own and a code image that changes while the game runs. Written from the disassembly (`tools/native/disasm.py`) and checked against the JIT (docs/native-custom.md) on captured tasks and in the game. The native implementation is src/hle/hle_indy.c (this document's sections 6 and 7 are its specification); sections 1 to 5 describe the microcode.

## 1. A task and its code

OSTask: type 1, `ucode_size` 0xec, `ucode_data_size` 0. The boot code loads the 0xec bytes of the text at IMEM 0: **a loader** (pc 0 .. 0xec) that DMAs the rest of the microcode from the OSTask `ucode`
address (`DMEM[0xfd0]`) to IMEM (the image is 4 KiB), and continues into the command loop at pc 0x2c. The text in RDRAM is longer than the image: behind the 4 KiB it holds
overlay code. The state is kept in DMEM from 0x110 (the pointer block `s2`), the command buffer is at 0x270 .. 0x378 and the RDP output buffer at 0xe80 .. 0xf70 (`s0` is its write pointer).

**The window.** IMEM 0xd60 .. 0x1000 is overwritten while the game runs: the initialisation code at 0xd60 is replaced by the **exit stub** (32 bytes from text offset 0x1fc0: `jal 0xdc` to flush,
`SP_STATUS` write, `break`) before the task ends, and for some scenes a DMA of 0x2a0 bytes puts an **overlay** there (from an address in the game's data, `mem 0x1d60`): the survey's pad meets at least three,
whose code parts are 0x248 bytes long (the last 0x58 bytes of the transfer are not code). hle_indy.c knows the images by hash (`HLE_INDY_OVL=1` prints the hashes of the images the game loads); the overlay manager (pc 0x164) pages them by number, and the ones it implements natively are the clipper (3), the lighting (27), the texture generation (42), the indexed vertex fetch (45) and the exit code; any other image or number (33) declines the task.

## 2. The command loop

pc 0x2c: `w0 = lw 0(s1)`, `w1 = lw 4(s1)`, `class = w0 >> 30`, `s1 += 8`.

| class | meaning |
|---|---|
| 3 (0xc0 .. 0xff) | an RDP command, written at `s0` as it is; a texture rectangle (`w0 >> 25 == 0x72`: 0xe4 / 0xe5) takes the next 8 bytes too (`jal 0x7b0`, 16 bytes in all) |
| 0 (0x00 .. 0x3f) | `jr` through the halfword table at DMEM 0xb6 + 2 (w0 >> 24) |
| 1, 2 (0x40 .. 0xbf) | `jr` through the same memory at `2 (w0 >> 24) - 0x9c`: the F3D opcodes 0xb1 .. 0xbf are the entries of the class-0 opcodes 0x0b .. 0x19 |

An entry is the IMEM address of the handler with bit 12 set. The command buffer is refilled when `s1` reaches 0x378 (pc 0x60 / 0x6c: a DMA of 0x108 bytes from `DMEM[0x5a0]`, the list address, which becomes
the chunk address `DMEM[0x5a4]`). A set `SIG0` (bit 7 of `SP_STATUS`, polled at pc 0x20) takes the yield path at 0x160.

**The flush** (pc 0xdc, when `s0 - 0xf70 > 0`, and at the end): the bytes `[0xe80, s0)` are DMAed to the RDRAM FIFO at the write pointer `lw 8(s2)` (OSTask output buffer: start `lw 0(s2)`, end `lw 4(s2)`); if the
chunk does not fit before the end the code waits for `DPC_STATUS.START_VALID` to clear and for the RDP not to be at the start, writes `DPC_START = start` and wraps; it then writes `DPC_END` and stores the new write pointer.

## 3. The commands (the survey's scripted pad, 1.64 M commands)

| opcode | handler | share | what it does |
|---|---|---|---|
| RDP commands | 0x98 | 40 % | `SETTILE` (0xf5), `TEXRECT` (0xe4), `LOADSYNC` (0xe6), `SETTIMG` (0xfd), `SETCOMBINE` (0xfc), `SETTILESIZE` (0xf2), `LOADBLOCK` (0xf3), ... written to the output buffer as they are |
| 0xba | 0x7dc | 21 % | **set other mode**: a field of the mode word at `0x120 + (w0 >> 16 & 7)` replaced (mask: `0x80000000` shifted arithmetically right by `w0 & 31`, then logically by `w0 >> 8 & 31`, inverted) by w1; the pair of mode words at `s2 + 16` is written as a `SET_OTHER_MODES` command, replacing the one at the end of the output buffer if that is one (0xef) |
| 0xb4 | 0xb40 | 10 % | **two triangles** (a quad): the vertex numbers in the halfwords of w0 and w1; the triangle routine at 0xa90 runs twice |
| 0xbc | 0xa74 | 6 % | `G_MOVEWORD`: `sw w1, 0(s3)`, s3 from w0 |
| 0xb8 | 0x774 | 5 % | **end of list**: pop the stack of (chunk pointer, command pointer) pairs at DMEM 0xfe0 + depth (depth in the byte at 0x142); with an empty stack the end of the task |
| 0x00, 0xb5 | 0x20, 0x68 | 4.7 %, 2.8 % | no operation |
| 0x01 | 0xc90 | 3.6 % | **load vertices (by inspection)**: `(w0 & 0xff) + 1` words from the address in w1 into the vertex buffer at `(w0 >> 8) & 0xfff`; the transfer goes through a stream whose state is in the OSTask words 0xfd4 .. 0xfdc and continues across DMAs (pc 0xd28 does the DMA) |
| 0xbf | 0xb20 | 2.6 % | **one triangle**: vertex lookup (`jal 0xb88`), then the triangle routine |
| 0xb7 / 0xb6 | 0xa64 / 0xa6c | 0.7 % / 0.4 % | geometry mode (register `s5`): set / clear the bits of w1 |
| 0xbb | 0xa7c | 0.4 % | texture: bit 1 of `s5` from w0, w1 kept in the pointer block (`sw s3, 56(s2)`) |
| 0xb9 | 0x7cc | 0.4 % | the same as 0xba on the other mode word |
| 0x06 | 0x750 | 0.2 % | **display list**: push (`DMEM[0x5a4]`, `s1`) and continue at the address in w1 |
| 0x02 | 0xd18 | 0.1 % | `DMEM[0xfdc] = w0 & 0x1ff` and a DMA of 8 bytes from the address in w1 (the stream's chunk size and start) |
| the others | 0x60 / 0x20 | | no operation |

## 4. Vertices and triangles

The vertex code (pc 0x834 ..) has the structure of the Rare microcode's: the position times the 4 x 4 S15.16 matrix (`VMUDN` / `VMADH` chains with the matrix rows in `v8` .. `v15`), the clip flags
against `+- W` (`VCH` / `VCL`, read with `cfc2`, packed into the halfword at +0x24 of the 40-byte vertex record), the reciprocal of W (`VRCP` / `VRCPH`) and the screen position. The triangle code (pc
0xa90 ..) sorts the vertices by Y, computes the area, culls by the geometry mode and builds the RDP triangle command (shade, texture and depth blocks) in the output buffer, up to 176 bytes; triangles with
vertices outside the near / far planes go to the clipper (pc 0x9ec ..), which works on a polygon in a scratch area and calls the triangle code for the fan.

## 5. The end of a task

The outermost `0xb8` (or a set `SIG0`) flushes what is in the output buffer (pc 0xdc), writes `DPC_END`, and runs the exit stub: `SP_STATUS` gets the halt and broke bits and the code executes `break`.

## 6. The arithmetic, as hle_indy.c implements it

**Vertices** (op 0x01 with type 1; pc 0x834). `n = w1 >> 24` (at least one) positions of 8 bytes at DMEM 0x170 are transformed to the 40-byte records at 0x600 (the microcode takes pairs: an odd count
also does the next position, of which only the fog byte (+19) and the clip flags (t3) survive). Per vertex: p = (x, y, z) + the offset vector at 0x128 (16-bit wrapping sums); the clip coordinates
c = (p, 1) M with the S15.16 matrix at 0x5c0 (integer rows, then fraction rows; accumulate in 48 bits: `acc = sum f[j][k] p[j] + (i[j][k] p[j] << 16)`, `acc += f[3][k]`, low = uclamp, high = sclamp(acc + i[3][k] << 16));
the clip flags of F3D (x, y, z against W and against K W, K = DMEM[6]); W' = W persp (DMEM[0x14e]), R = refined 1/W', screen = trans + scale (c R persp) with scale / trans at 0x130 / 0x138 and **only the
lower clamps** (x, y at 0x52, z at 0x56). With geometry mode bit 16 the alpha byte is the fog: z' (32 bits) x (fog[0] : fog[1]) + (fog[2] : fog[3]), x fog[4], integer half limited to [0, fog[4]] (halfwords at 0x160).
These are f3d_vtx_xform (NEON-capable) with `fog_wide`.

**Triangles** (0xbf pc 0xb20, 0xb4 pc 0xb40 = (a, b, c) and (a, c, d)). Skip counter (DMEM 0x58a): above zero the command is skipped (and counted down); otherwise it is set to w0 & 0xff and a drawn or rejected
triangle resets it. Colours: the table at 0xd40 (4 bytes) gives each vertex its colour word (+16) by the bytes of the command (fog first overwrites the entries' alpha from the vertices' alpha); bit 9 of w0: the
next 16 bytes are the texture words of the vertices (+20), bit 11: overlay 42 computes them (position times the matrix at 0xe40, minus the vector at 0xf0, normalised (1/sqrt * 171), scaled by DMEM[0xec], [0xee]).
Setup (pc 0x1a8) is F3D's (f3d_tri_setup, variant F3D_TRIV_INDY): reject when the three flags share a bit of 0x7070, clip when any has a bit of 0x4343; area from the command order; culling only by bit 13
(negative area); the vertex order from a table of three comparisons of the screen Y (DMEM 0x9a .., 0xa1 .., 0xa8 .., parity 0xaf ..); then F3D's shade, texture (scaled by K), z blocks. The deferred list
(DMEM[0x58c]) runs after the decision and before the blocks are built. The clipper (overlay 3) is f3d_clip_tri_seq: the polygon walked in input order, fan (p0, p[i+1], p[i+2]), rounding constant 1.
Lighting (overlay 27, op 0x19 with a non-negative factor): section 3 of hle_indy.c's comment; colours expand 5-6-5 to 32 bits; the scale path multiplies by the eight bytes at 0x158.

## 7. What stays on the JIT

The microcode keeps junk in vector registers between commands and the output depends on it in two cases: a **textured triangle with geometry mode bit 12** (the texture coordinates are not scaled and their
fractions and w come from v9 / v10 / v22 / v23 as the previous triangle left them) and a **quad without texture words** (bit 9 clear: the fourth vertex's st word is v0's bytes 12 .. 15). Those tasks decline
(74 % / 5 % of the declines in a 3700-frame run); the clipper declines when it discards a triangle or fog is on; unknown overlay images (hashes in hle_indy.c) and ops other than the table above decline.
