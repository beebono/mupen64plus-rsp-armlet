# F3DEX2 family microcode semantics (as the native front-end reproduces them)

What the four F3DEX2 builds that arMLEt knows (Donkey Kong 64 "F3DEX 2.07", Paper Mario "F3DEX 2.08", Ocarina of Time "F3DZEX.NoN 2.06H", Pokemon Snap "F3DEX.NoN 2.08H") do with a
display list, written from the microcode's own instructions (the interpreter, `src/hle/hle_rsp.cpp`, runs them; `tools/native/disasm.py` disassembles a DMEM / IMEM image) and checked by running
the front-end (`src/hle/hle_f3dex2.c`, `f3dex2_fx.c`) against the JIT on every captured task. The arithmetic is in [ucode-f3dex.md](ucode-f3dex.md) section 1 (Fleet B's closed forms of the
vector unit's multiply-accumulate chains, `src/hle/hle_fx.h`). Addresses are DMEM byte addresses; "(x4)" means the value is a byte offset into a stack of words.

## 1. A task

The CPU puts the OSTask header at DMEM 0xfc0 (type, flags, boot, ucode, data pointers, ..., output buffer `0xfe8` / size `0xfec`, data pointer of the display list at 0xff0 ...), starts the
RSP at pc 0 and the boot code in IMEM loads the microcode's text and data. The microcode's initialisation (about 140 instructions) sets up the registers and ends with the first fetch of the
command loop:

| register | |
|---|---|
| `k0` | RDRAM address of the next 168-byte chunk of the display list (after the chunk in the input buffer) |
| `k1` | negative offset of the next command in the input buffer: the command is at DMEM `0x9c8 + k1` (the buffer is 0x920 .. 0x9c8); `k1 == 0`: refill |
| `s7`, `s6` | the RDP output buffer: commands are appended at `s7`; `s6` is the end of the current one (two buffers of 344 bytes at 0xba8 .. 0xd00 and 0xdb0 .. 0xf08, `s6 ^= 0x208` toggles) |

The loop head (pc `0x18c` DK64 / OoT, `0x194` PM / Snap) is `mfc0 at, SP_STATUS` (the poll; `at & 0x80` = SIG0, the CPU asks the microcode to yield), `lw t9, 0x9c8(k1)` (w0),
`beq k1, zero, refill`, `sra t4, t9, 24` (the opcode), `lhu t3, 0x36e + 2 t4` (the handler pc from the jump table, indexed by the *signed* opcode), `lw t8, 0x9cc(k1)` (w1), `jr t3` with `k1 += 8`.
The refill (pc 0x170) DMAs 168 bytes from `k0` to 0x920, `k0 += 168`, `k1 = -168`. Handlers that started a DMA go through the wait (pc 0x188: `jal 0xfc8` whose delay slot is the loop head)
before the next command; the others jump to the head directly.

**Flush.** After a command that wrote to the output buffer (`s7 - s6 > 0` means the buffer overflowed its 344 bytes: there are 0xb0 bytes of slack for a triangle) the routine at 0x25c
(DK64) DMAs `[s6 - 344, s7)` to the RDRAM ring at the FIFO pointer (DMEM 0xf0), first writing `DPC_END = FIFO pointer` (the RDP is always one chunk behind), and if the chunk does not fit before
the end of the ring (DMEM 0xfec) it waits for the RDP, writes `DPC_START = ring start (DMEM 0xfe8)` and wraps. It then advances the FIFO pointer, toggles `s6` and sets
`s7 = s6 - 344`. The front-end declines the whole task when the microcode would have to wait for the RDP (FIFO full, DPC busy while wrapping).

**The end.** `G_ENDDL` of the outermost list (or the yield bit) jumps to 0xfac, which loads the exit code from the descriptor at DMEM 0x2e0 into IMEM 0 and runs it: flush what is in the
buffer (`DPC_END`), `SP_STATUS = SET_SIG... | BROKE`, `break`. The front-end hands this to the interpreter (about 70 instructions).

## 2. The commands

The handler pc of an opcode is in the jump table (DMEM 0x36e + 2 int8(op)); a handler pc is the same in the four builds for the commands below 0x170, and differs by build above.

| opcode | handler (DK64 / OoT) | what it does |
|---|---|---|
| `G_DL` 0xde | 0x000 | w1 resolved through the segment table (DMEM 0xf8 + (w1 >> 22 & 0x3c), + w1 & 0xffffff). A call (w0 bit 16 clear) pushes the address of the next command (`k0 + k1`) on the stack (DMEM 0x138 + depth, depth at DMEM 222, x4); a branch does not. Then `k0 = target`, refill |
| `G_ENDDL` 0xdf | 0x1e4 | depth 0: the end of the task. Else pop, refill |
| `G_GEOMETRYMODE` 0xd9 | 0x1d0 | DMEM word 0x1ec = (word & w0) \| w1 |
| `G_TEXTURE` 0xd7 | 0x028 | w0 to 0x1e4, w1 to 0x1e8 (scales s, t; byte 0x1e6 tile, 0x1e7 on / level) |
| `G_MOVEWORD` 0xdb | 0x038 | a word at `table16[0x300 + (w0 >> 16 & 0xfe)] + (w0 & 0xffff)` = w1 (the number of lights, the clip ratio, the fog, the segments ...) |
| `G_MOVEMEM` 0xdc | 0x120 | DMA of `w0 >> 16 & 0xff` + 1 bytes from the segment address of w1 to `table16[0x2f0 + (w0 & 0xfe)] + (w0 >> 5)`: 2 modelview matrix (0x00 or the scratch 0x9d0), 6 projection (0x40), 8 viewport (0xe0), 10 lights (0x1f0 + ofs), 12 vertex, 14 MVP |
| `G_MTX` 0xda | 0x0f0 | flags in w0: bit 0 no push, bit 1 load, bit 2 projection. Push: the 64 bytes at DMEM 0 go to the matrix stack in RDRAM (pointer 0xf4, += 64). The flags 0x1d9 (MVP valid) .. 0x1dc (lights valid) are cleared. The matrix is DMAed to the destination (load) or to the scratch 0x9d0 and multiplied: new x current |
| `G_POPMTX` 0xd8 | 0x04c | the stack pointer 0xf4 -= w1 (not below the bottom, DMEM 0xfe0); if it changed: flags cleared, the matrix at the new pointer is DMAed to the modelview matrix |
| `G_VTX` 0x01 | 0x4f0 / 0x510 / 0x780 | section 3 |
| `G_MODIFYVTX` 0x02 | 0xc78 / 0xc98 / 0xf08 | a word (w1) stored at `w0 >> 16 & 0xff` + the slot address of vertex `w0 & 0xfff` (table 0x380) |
| `G_CULLDL` 0x03 | 0xc38 | the display list ends (as `G_ENDDL`) when one bit of the build's reject mask (0x7070; 0x70b0 in Ocarina of Time's F3DZEX.NoN 2.06H and Pokemon Snap's F3DEX.NoN 2.08H, the w lane instead of z; read from the microcode: `addiu r1, r0, mask`) is set in the flags (+0x24) of all vertices from the slot of `w0` to the slot of `w1` |
| `G_BRANCH_Z` 0x04 | 0xc60 | if the z of vertex w0 (the word at +0x1c; F3DZEX: the integer part of its clip w, the halfword at +6, `lh`) is less than w1: a branch (no stack) to the address `G_RDPHALF_1` left at 0xd8 |
| `G_TRI1` 0x05 / `G_TRI2` 0x06 / `G_QUAD` 0x07 | 0x7b4 / 0x7ac | section 4; TRI2 does the triangle of w1 first, then w0; vertex bytes (w >> 16, >> 8, & 0xff) are vertex numbers x 2, the slot address from the table at 0x380 |
| `G_SPECIAL_1` 0xd5 (Snap only) | 0x1c0 | concatenates MV x P into the MVP matrix now and sets the valid flag to the low byte of w0 |
| `G_RDPHALF_1` 0xe1 | 0x030 | w1 to 0xd8 |
| `G_TEXRECT` 0xe4 / 0xe5 | 0x02c | w0, w1 to 0xd0 / 0xd4 |
| `G_RDPHALF_2` 0xf1 | 0x1f8 | appends the four words 0xd0, 0xd4, 0xd8, w1 (a 16-byte texture rectangle) to the output buffer |
| `G_SETOTHERMODE_H` 0xe3 / `_L` 0xe2 | 0x13c / 0x140 | a field of the word 0xc8 / 0xcc replaced (mask `0x80000000 >> (w0 & 31) >> (w0 >> 8 & 31)`), then the pair (0xc8, 0xcc) appended as `SET_OTHER_MODES` |
| `G_RDPSETOTHERMODE` 0xef | 0x238 | w0, w1 to 0xc8, 0xcc, appended |
| `G_SETSCISSOR` 0xed | 0x244 | w0, w1 to 0xc0, 0xc4, appended |
| `G_SETTIMG` / `G_SETZIMG` / `G_SETCIMG` 0xfd .. 0xff | 0x218 | the segment address resolved, appended |
| the other RDP commands | 0x208 | appended (2 words); the syncs (0xe6 .. 0xe9, 0x00) 0x20c: only the first word is written, the second is stale scratch |
| 0x08 `G_LINE3D`, 0xd3, 0xd4, 0xd0, 0xd2, 0xe0 | the loop head | nothing |
| `G_LOAD_UCODE` 0xdd | 0xfac | loads a text (3912 bytes to IMEM 0x80, the data segment to DMEM 0x180) and starts it; the front-end hands it to the interpreter and takes over the build that was loaded when it registered ([design.md](design.md#build-switching-g_load_ucode)), else declines the task |
| `G_DMA_IO` 0xd6 | 0x1b8 | not implemented (the front-end declines the task) |

## 3. Vertices

`G_VTX`: n = w0 >> 12 & 0xff, the first slot = table[0x380 + (w0 - 2n) & 0xfff], one past the last = table[0x380 + w0 & 0xfff]. Before: the lights are set up if they are lit
and stale (flag 0x1dc, below), the MVP matrix (0x80 / 0xa0) is recomputed when its flag 0x1d9 is clear (MV x P, the flag set to the low byte of w0). The 16-byte input vertices
(x y z, flag, s t, rgba or nx ny nz a) are DMAed to the end of the slot range, and each is transformed; a *pair* of vertices shares the vector registers, so the clip-flag words of
the two are merged (3.3), and an odd count computes a phantom pair partner from whatever DMEM holds after the last input vertex (only its clip bits are used).

**3.1 The record** (40 bytes at 0x420 + 0x28 n): +0 clip x y z w (integer parts), +8 their fractions, +0x10 colour r g b a (lit, or the fog value in a), +0x14 s t (scaled by the
G_TEXTURE scales: `s * scale >> 16`), +0x18 screen x y, +0x1c screen z (integer part, negative set to 0), +0x1e its fraction, +0x20 / +0x22 1/w (integer, fraction), +0x24 the clip flags.

**3.2 The arithmetic.** P = the 32-bit clip coordinates: the accumulator chain `(Mi[3] << 16) + Mf[3] + sum_j Mf[j] v[j] + (Mi[j] v[j]) << 16` over the rows of the MVP matrix (the integer part
is the saturated accumulator, the fraction is read *before* the last integer product). Q = P x the perspective norm (DMEM 0xdc). R = the reciprocal of Q's w: `rcp32` (the divide unit's ROM
table, `hle_fx.h`), T = Q3 R, E = 4 - 4 T, R1 = E R (one Newton step). The ndc = P x (R1 integer part limited to 0x7fff when w < 0, R1 fraction) x the norm again; the screen position =
ndc x the viewport scale + the translation (DMEM 0xe0 / 0xe8; the y scale negated). The fourth lane of the viewport (DMEM 0x1e0 / 0x1e2) is the fog multiplier / offset, applied to z in
that lane; the fog value in the alpha byte is `max(that lane, 0x7f00)`'s low byte.

**3.3 The clip flags.** Each of x y z w against w (and, in a second compare, against `w x` the guard-band ratio (DMEM 0x186; the fourth lane compared as w's integer part with z's
fraction)): per lane `le` (bit k) and `ge` (bit 8 + k), with the sign cases of `VCH` / `VCL` (`hle_clip_le` / `hle_clip_ge`). A pair merges its two vertices' bits (`bits & 0x0f0f`, the second
shifted by 4); the first vertex of the pair keeps them shifted by 4 more. The word at +0x24 is `(second compare) << 16 | (first compare)`. The triangle code tests it against the build's reject mask and
against the clip mask (DMEM 0x3cc).

**3.4 Lights** (geometry mode bit 17, `G_LIGHTING`). The light data are at 0x200 + 24 m: `n` lights = DMEM byte 0x1dd / 24; slots 0 and 1 are the two look-at directions (for texture generation). The
raw directions (bytes at 0x1f8 + 24 m .. +2, from `G_MOVEMEM` 10) are *set up* by the first lit vertex command after the lights or a matrix changed (flag 0x1dc cleared by `G_MTX`, `G_POPMTX`, the number
of lights): each direction is transformed by the upper 3x3 of the modelview matrix, normalised with the reciprocal square root and scaled by the halfword at 0x1c6 (0x1ce in F3DZEX), and stored as
signed bytes at 0x200 + 24 m + 0 .. 2 and again at + 4 .. 6 (slot m takes the direction of slot m - 1, hence slots 0 .. n + 1 are written). The lit colour of a vertex: starting at the ambient
slot (m = n + 1) two slots at a time, `cur = cur x 0x7fff` (`VMULF`) plus colour(m - 1) x lambert(direction m) plus colour(m - 2) x lambert(direction m - 1), the lambert term
`(normal . direction)` read unsigned and masked with 0x7fff; the byte is `lane >> 7`. Texture generation (bit 18): s, t = 0x4000 + (normal . look-at 0 / 1) / 2, or with bit 19 (linear)
the polynomial `0x4000 + 2 x c1 + 2 x^2 (x k + x c2)` of `x = 0x4000 + c0 + d / 2` with c0 c1 c2 the halfwords at 0x1d0 .. 0x1d4 and k = 0x7fff.

**3.5 The overlays.** The lighting code and the clipping code occupy the same IMEM window ([0x2d0, 0x4d8) / [0x2d0, 0x4f0) in DK64 and OoT, shifted by 8 in Paper Mario and 0x20 in Snap); each has a
descriptor in DMEM (0x410 lighting, 0x418 clipping: RDRAM address, length register, IMEM address). A lit `G_VTX` with the clipping code resident, and a triangle that needs clipping with the lighting
code resident, DMA the other one in. IMEM at the end of a task holds whichever was loaded last.

## 4. Triangles

The triangle code takes three vertex slots (A, B, C in command order) and:

1. rejects the triangle when its three flag words have a bit in common in the build's reject mask (0x7070; 0x70b0 in the two NoN builds above); the clipper runs when `(fa | fb | fc) & DMEM[0x3cc]` (section 5);
2. computes the signed area of the *unsorted* vertices `(Ax-Bx)(Ay-Cy) + (Ax-Cx)(By-Ay)` from saturated 16-bit differences, clamps the sum to 16 bits, and culls with the cull table word (DMEM 0x3c2 + byte
   0x1ee, the geometry mode's cull bits): `word + area >= 0` or `area == 0` draws nothing;
3. sorts the vertices by screen y (H, M, L: min / max networks with the ties as `VLT` / `VGE` leave them), computes the edge slopes (DxLDy, DxMDy, DxHDy: `rcp32` of each dy, scaled, times dx, the integer
   part clamped to +-0x100 (+-0x1cc in F3DZEX)) and the start x of the edges, and writes the 32-byte header (opcode `0xc8 | shade 4 | texture | z-buffer 1`, the tile, the three y, the M.x << 14, the slopes
   and starts). F3DZEX has other lanes of the constant vectors in this code (dx scaled by 0x4000 instead of 0x1000, the reciprocal by 8 instead of 0x20, no mask on the slope fraction);
4. when shading, texturing or z-buffering: the attributes of H M L in 8 lanes (r g b a, s t w, z; the flat colour is vertex A's unless smooth shading), 1/area with a Newton step, and per lane the
   gradients dX (per x), dY (per y), dE (along the edge) and the value at the start of the edge (`att[H] + dE x C`, C the fractional y of H): the shade block (64 bytes), the texture block (64)
   and the z block (16). The texture lanes use s t scaled by the *smallest* 1/w of the three vertices.

## 5. The clipper

The clip code (DMEM 0x3cc masks the flags that start it; the lists are at 0x3d0 and 0x3e4, the new vertices at 0x9c8 + 40 k, at most 12) clips the triangle against six planes in the order the
table at 0x1a8, 0x1a0, 0x198, 0x190, 0x188, 0x180 gives (x y z w coefficients; with the flag mask of each at 0x40c .. 0x3f8): the near / far planes and, in F3DEX2, the guard band x = +-3w, y = +-3w
(+-w in the NoN builds; the coefficients are data). For each plane the polygon's vertex list is walked once: the edge from the previous vertex to this one crosses the plane when the "outside" flag
(the vertex's flags & the plane's mask) differs; a crossing adds the intersection, an inside vertex is kept; fewer than three vertices left ends the clip (nothing is drawn). The intersection of
inside -> outside: the dot products of both ends with the plane (the 48-bit products summed over the four lanes with the carrying 32-bit adds), `rcp32` of their difference (doubled, sign made
positive; its low half rescales the difference; the reciprocal of that product with a Newton step; times the inside distance; rescaled by the low half again), the fraction t clamped to
[1, 0xffff] as a 16-bit number, and the new clip coordinates `out x t + in x (1 - t)`, the colour bytes (as `byte << 7`) and the texture coordinates interpolated the same way; the new
vertex then goes through the vertex pipeline from its clip coordinates (3.2 and 3.3, fog not recomputed). The remaining polygon (L[0] .. L[m - 1]) is drawn as the fan (L[k], L[k + 1], L[m - 1]),
k = 0 .. m - 3, each through the triangle code with the clip mask cleared (the cull test still applies).

## 6. DMEM at the end of a task

The microcode reloads DMEM at the start of every task, so nothing in [0, 0xfc0) is read by a later task (`ARMLET_DMEM_POISON=all` over gameplay windows, docs/testing.md). The table of dead ranges
the verify compare skips is smaller on purpose (it lists only what the native front-end does not reproduce, so that the compare keeps checking the rest): the input buffer (0x920 .. 0x9c8, a native
run reads the list from RDRAM), the clipper's lists (0x3ca .. 0x408) and new vertices (0x9c8 ..), the two RDP output buffers and their slack (.. 0xfb8), the padding bytes of the light slots, and the
word at 0xfc8 where the initialisation stores a register (`sw a0, 0xfc8`) that holds whatever the previous task left in it.
