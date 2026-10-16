# Rare's microcode (Diddy Kong Racing, Jet Force Gemini)

Two builds with no version banner: Diddy Kong Racing (id `72a1416f6d8445c3`, text 0xf80 bytes, 1423 graphics tasks in the survey's 55 s) and Jet Force Gemini (`410cfbcf87378a9c`, 1563). Both are
one microcode for the whole game (every graphics task of both games runs it). It is a descendant of Fast3D (the 2.0H builds are its closest relatives in the survey, 77 % of the words) with
a different command encoding and a different data flow: **the display list names its data by DMA**. Vertices, triangles, matrices and RDP command blocks are not in the list, they are fetched
by the command itself from the address in w1, and the RDP lists go through a ring in DMEM (`DPC_STATUS.XBUS`), not through a FIFO in RDRAM. The two builds are 90 % the same code
(`tools/ucode_survey.py`); section 9 lists the differences. Written from the disassembly (`tools/native/disasm.py`, `tools/ucodemap/rspdis`); the native implementation is src/hle/hle_rare.c
(sections 6 to 8 are its specification) and it is checked against the JIT on captured, perturbed and synthetic tasks and in the game (docs/native-custom.md).

## 1. A task

OSTask as usual (type 1, `ucode_size` 0xf80, `ucode_data_size` 0x800, XBUS lists: `output_buff` is not used). The boot code puts text[0:0xf80] at IMEM 0x80 and the data at DMEM 0. The first
instruction (`j 0x7f0`) leads to the initialisation, which

- clears signal bits 1 and 2 (`SP_STATUS = 0x2800`), loads the vector constants `v31` (DMEM 0x30) and `v30` (DMEM 0x40) and sets `DPC_STATUS.XBUS` (`DPC_STATUS = 2`);
- adds the text address to the text offsets of the four overlay descriptors in DMEM (8, 0x10, 0x18, 0x20; below), loads the first one (IMEM 0 .. 0x88 from text 0xee8, the tail of the code,
  which also overwrites the first two words of the text) and sets the DL pointer `k0` to the OSTask data pointer, `s7` (the ring pointer) to 0x1000 and `DPC_START = DPC_END = s7`;
- starts the first refill and falls into the command loop. A yield (OSTask flags bit 0, a resume) takes the code at 0x89c instead, which reloads `gp`, `k1`, `k0`, `s7` from DMEM 0x914 .. 0x920.

| register | |
|---|---|
| `k0` | RDRAM address of the next unfetched command of the list (it is advanced by 8 as each command is fetched) |
| `k1`, `gp` | DMEM address of the next command in the 320-byte command buffer (DKR 0x6d0 .. 0x810, JFG 0x730 .. 0x870) and the bytes left in it; `gp <= 0` after a command: refill (DMA of 320 bytes from `k0`, `gp = 320`) |
| `s7` | the write pointer of the RDP ring in DMEM: DKR [0xa10, 0xe10), JFG [0xa70, 0xe70) |
| `sp` = 0x110 | the state block (below); the display list stack is at 0x1e6 + depth |
| `s6` | the buffer the class-0 commands DMA their data to: DKR 0x810, JFG 0x870 |

**The command loop** (pc 0x60): `w0 = lw 0(k1)`, `w1 = lw 4(k1)`, `class = (w0 >> 30)` (index x2 into a 4-entry table at DMEM 0xbc), advance `k0`, `k1`, `gp`. After a command the code
polls `SP_STATUS` (bit 7 = SIG0, the CPU's request to yield: the yield path saves the registers and the state in the data segment and ends the task) and loops while `gp > 0`.

| class (w0 >> 30) | meaning | handler |
|---|---|---|
| 0 | **data commands** (opcodes 0x00 .. 0x3f): first a DMA of `(w0 & 0x1ff) + 1` bytes from the address in w1 to `s6`, then the handler of the opcode | 0x340 |
| 1 | no operation (opcodes 0x40 .. 0x7f) | loop |
| 2 | **state commands** (0x80 .. 0xbf): handler through a second table | 0x1f8 |
| 3 | **RDP commands** (0xc0 .. 0xff): written to the ring as they are | 0x300 |

The address in w1 of a class-0 command is resolved through the segment table (DMEM 0x160 + 4 x segment, w1 bits 31 .. 24; the mask 0x00ffffff is at DMEM 0xb8) in DKR; JFG resolves the matrix
(op 1) and vertex (op 4) addresses against two base words, DMEM 0x210 and 0x214, and the others through the segment table. Both second-level tables are halfwords at DMEM 0xc4 + 2 op (class 0) and 0x76 + 2 (op & 0x7f)
(class 2); an entry is the IMEM address of the handler with bit 12 set. Class-2 opcode `0xa7 + k` and class-0 opcode `k` use the same entry (the two tables overlap), which is why the
game's `G_RDPHALF`-like commands are 0xb2 / 0xb3 (the entries of class-0 opcodes 0x0b / 0x0c).

**The RDP ring.** Every RDP command (class 3, and what the handlers emit) is written at `s7` after the room check at pc 0x178: with `s4 = s7 + n` and the ring end 0xe10 (JFG 0xe70) a command that
does not fit wraps `s7` to the ring start after waiting for `DPC_STATUS.START_VALID` to clear and for the RDP not to be at the start, and writes `DPC_START = s7`; then the RDP must not be inside
the next `n` bytes (`DPC_CURRENT < s7` or `>= s7 + n`). Then `s7 += n` and `DPC_END = s7` (pc 0x1d0): **every command is a list of its own**. The native code reproduces these loops with the registers as the
framework models them (the video plugin has taken every list when `DPC_END` is written), so nothing waits.

**The state block** (`sp` = 0x110): byte 0 the display list stack depth (x4); halfword 2 the perspective normalisation word; word 4 (0x114) the geometry mode (bit 1 and bit 13 are set by the triangle command
from its flags); words 8 / 12 (0x118, 0x11c) the pair of the RDP `SET_OTHER_MODES` command; words 16 / 20 (0x120, 0x124) the texture words; word -4 (0x10c) the second word of a `G_RDPHALF`;
the lights / viewport table at 0x120 + b1 (op 3 stores 16 bytes at `lh 0x120(b1)`).

**Overlays.** IMEM 0x7d8 .. 0x9f8 (JFG 0x870 .. 0xa90) is a window. At the start it holds the stubs that load the others: `ori s8, 0x10 / 0x20 / 0x18`, then the DMA of the descriptor `s8`
(`lw s3, 0(s8)`: text address, `lh s2, 4(s8)`: length - 1, `lh s4, 6(s8)`: IMEM address with bit 12) and a jump through DMEM 0x100 / 0xa0 / 0xb6. The descriptors (DKR): 0x08 the tail
(text 0xee8, 0x88 bytes, IMEM 0), 0x10 the **clipper** (text 0xf70, 0x220 bytes, IMEM 0x7d8), 0x18 the **exit code** (text 0x1190, 0x68 bytes: saves `gp, k1, k0, s7` to DMEM 0x914 .. 0x920,
DMA-writes the state to the data segment, `SP_STATUS = SET_BROKE`-style halt, `break`), 0x20 an 8-byte restore. The text beyond the 0xf80 bytes that the ucode id covers holds the overlays.

## 2. The commands

(Counts: DKR, the first 700 frames of the survey's scripted pad.)

| opcode | handler (DKR / JFG) | share of commands | what it does |
|---|---|---|---|
| 0x01 (class 0) | 0x358 | 3 % | **load matrix**: `DMEM[0x148] = b1`; the 64 bytes in `s6` (4 x 16: the integer rows, then the fractions) are copied to `0x210 + b1`. The matrices are 4x4 S15.16 in the F3D layout; b1 is the byte offset of the slot, and also the slot the vertex command multiplies by |
| 0x03 (class 0) | 0x45c | 0.8 % | **load vector**: the 16 bytes in `s6` are stored at the address in `DMEM16[0x120 + b1]` (b1 = 0x80: 0x1d0, the vertex offset used by the vertex command) |
| 0x04 (class 0) | 0x46c | 12 % | **vertices**: section 3 |
| 0x05 (class 0) | 0x674 | 10 % | **triangles**: section 4 |
| 0x06 (class 0) | 0x7a4 | 3.6 % | **display list**: b1 = 0: push `k0` (depth < 9), then `k0 = w1` (resolved), `gp = 0` (refill); b1 != 0: branch without the push |
| 0x07 (class 0) | 0x76c | 9 % | **RDP block**: b1 8-byte RDP commands from `s6` are written to the ring (each with its own room check and `DPC_END`): prebuilt RDP lists (triangles of static geometry, texture loads) |
| 0xb1 / 0xb2 / 0xb3 | 0x2f4 / 0x2f8 / 0x2ec | 2 % each | `G_RDPHALF`s: 0xb3 stores w1 in word -4 of the state block; 0xb2 writes the pair (word -4, w1) to the ring (the second half of a texture rectangle, `0xe4` + `0xb3 / 0xb2`) |
| 0xb4 | 0x2e4 | 0.4 % | `DMEM16[0x112] = w1` (perspective normalisation) |
| 0xb6 / 0xb7 | 0x2cc / 0x2bc | 1 % / 2.4 % | geometry mode: clear the bits of w1 / set them (word 0x114) |
| 0xb8 | 0x29c | 3.7 % | **end of list**: pop the display list stack (depth - 4; `k0 = stack[depth]`, `gp = 0`); with an empty stack: the exit code (overlay 0x18) |
| 0xb9 / 0xba | 0x25c / 0x254 | 2.6 % / 10 % | `SETOTHERMODE_L` / `_H`: the field `(1 << len) - 1 << shift` (len = w0 & 0xff, shift = w0 >> 8 & 0xff) of the word at 0x11c / 0x118 replaced by w1, then the pair is written to the ring as `SET_OTHER_MODES` |
| 0xbb | 0x230 | 0.2 % | texture: w0, w1 to 0x120, 0x124; bit 1 of halfword 0x116 = w0 & 1 (texturing on) |
| 0xbc | 0x218 | 9 % | `G_MOVEWORD`: a word (w1) stored at `DMEM16[0x1be + (w0 & 0xff)] + (w0 >> 8 & 0xffff)`: the bases are 0x210 (matrices), 0x144, 0x70, 0x160 (segments), 0x1e0, 0x148, 0x2d0 (the vertex offset vectors) |
| 0xc0 .. 0xff | 0x300 | 20 % | RDP commands as they are; `SETTIMG` / `SETZIMG` / `SETCIMG` (0xfd .. 0xff) first resolve w1 through the segment table. A texture rectangle is `0xe4` (class 3) followed by 0xb3 / 0xb2 |
| the others | loop | | no operation |

## 3. Vertices (op 4)

`b1`: bit 0 = append (start at the slot after the previous vertex command, the previous count is kept in DMEM16 0x14c; otherwise slot 0), bits 1 .. 2 = the misalignment of the data in the DMA buffer
(`s6 + (b1 & 6)`: the DMA aligns the source to 8 bytes), bits 3 .. 7 = the number of **pairs** minus one. The data are 10-byte records: x, y, z (s16) and four bytes (colour or normal and alpha),
two records per pass. The output records are 40 bytes at `0x2d0 + 40 slot`. A pass:

1. the position `(x, y, z, 1)` is multiplied by the matrix at `0x210 + DMEM[0x148]` (S15.16: four vectors of integer parts, four of fractions, element 3 of the vectors `v31[9]` = 1 for the
   translation row), with the offset vector (DMEM 0x2d0 integer part, 0x2d8 fraction part) added when `DMEM32[0x144]` is nonzero; the result `(X, Y, Z, W)` is a pair of vectors (integer, fraction);
2. the clip flags of both vertices: `X, Y, Z` against `+- W` and against `+- W x DMEM16[0x76]` (VCH / VCL, read back with `cfc2`), packed into the halfword at +0x24 of the record;
3. the reciprocal of W (the routine at pc 0x000: VRCPH / VRCPL and one Newton step) and the products with it, scaled by the normalisation halfword at 0x112, give the screen-space values the
   triangle code reads (stored at +0x18 .. +0x23); the colour word (+0x10 .. +0x13; its alpha byte is replaced by a value derived from the vertex, `SBV v4, 5, 19(a3)`) and the position pair
   (+0x00 integer parts, +0x08 fractions of the scaled position) are stored too. The two vertices of a pass are computed together, one in lanes 0 .. 3, the other in lanes 4 .. 7.

## 4. Triangles (op 5)

`b1`: bit 0 sets bit 1 of the geometry mode word (back-face selection), bits 4 .. 7 = the number of triangles minus one. The DMA brought 16-byte records: flags (bit 6: sets bit 13 of the
geometry mode, two-sided), the three vertex numbers (bytes 1 .. 3, x 40 + 0x2d0 = the slot addresses), and three words (the texture coordinates `s, t` of the three vertices, copied to +0x14 of
their records). Per triangle (pc 0x9f8):

- the three clip-flag halfwords (+0x24) are tested: all three outside the same plane (`& 0x7070`) rejects the triangle; any outside a near / far plane (`| & 0x4343`) takes the clipper
  (the overlay at 0x7d8, which clips the polygon against the planes, makes new vertices by interpolation and loops over the fan, calling the triangle code for each);
- the vertices are sorted by screen Y, the area and the facing are computed (cross product of the edge vectors, VMUDH / VMADH / VSAR), and the triangle is culled by the geometry mode (bits 12 / 13);
- the RDP triangle command is built: edge coefficients with the reciprocal of the Y deltas (VRCP, one refinement), then, according to the flags, the shade block (colour gradient from the vertex colours), the texture
  block (S, T, W gradients with the perspective normalisation) and the depth block; the command is `0xc8 | shade << 2 | texture << 1 | z` (8 .. 176 bytes) plus the left / right flag and tile of the state block,
  and goes to the ring through the room check with n = 176 and `DPC_END`.

## 5. Yield and the end of the task

A set `SIG0` makes the next poll jump to the exit code with s8 = 0x18 (the state is saved); the outermost `0xb8` does the same. The exit code ends with `break` (`SP_STATUS` halted, the
interrupt if enabled). The native path does not resume yielded tasks (a task whose OSTask flag bit 0 is set is not fresh).

## 6. The arithmetic, as hle_rare.c implements it

Nothing of the microcode's vector code is executed by the native path. The vertex record, the clipper and the triangle builder are those of Fast3D (docs/ucode-f3d.md), so they are the
struct-level functions of f3d_fx.h (the same ones SM64 and the other F3D builds use, in NEON where the F3D front-end has a NEON form); what is specific to Rare is passed to them as parameters.

**Vertices** (op 4; pc 0x46c / JFG 0x514). `n` records of 10 bytes (x, y, z as s16 and four colour bytes) from the data buffer at `s6 + (b1 & 6)`; DKR: `n = (b1 >> 3) + 1`, first slot 0 or, with
`b1` bit 0, the slot after the previous command (count in DMEM16[0x14c]); JFG: `n = max(b1 >> 3, 1)`, first slot `(w0 >> 8 & 0xff) / 2`. Per vertex: the clip coordinates
`c = (p, 1) M` with the S15.16 matrix of the slot named by DMEM[0x148] (integer rows, then fraction rows: `acc = sum f[j][k] p[j] + (i[j][k] p[j] << 16)` in 48 bits, `acc += f[3][k]`,
low word = unsigned clamp, high word = signed clamp of `acc + i[3][k] << 16`), then, when `DMEM32[0x144]` is nonzero, the **offset vector** (the integer and fraction halves at vertex slot 0, DMEM
0x2d0 / 0x370, `VADDC` + `VADD`: the low words wrap and carry, the high words saturate) is added to the clip coordinates **before** the clip flags and the projection (`has_off`, `off_i`, `off_f` of
`struct f3d_vctx`). Then F3D's: the clip flags against `+-W` and `+-W K` (K = DMEM16[0x76]; the SM64 flag layout), the reciprocal of W with `persp` (DMEM16[0x112]), the screen position from the viewport
(DMEM 0x1d0 scale, +8 translation), the fog byte from DMEM 0x1e0 when geometry bit 16 is set; there is no lighting and no texture generation, the colour bytes are copied through and the `s, t` words
of the record (+20) are the fractions of the offset vector (`SLV v18`), whether or not it was added, until a triangle command writes the real ones. In a pair the second record's word at +38 gets the
first vertex's flags (the microcode's word store).

**Matrices** (op 1). DKR: the 64 bytes to slot `b1` (multiples of 16 up to 0x80). JFG: `b1` = multiply flag (0x80) | right slot << 3 | destination slot; without the flag the destination gets the 64
bytes, with it the product A x B (A the transferred matrix, B the slot) by `f3d_mtx_mul`, **two rows at a time**: the second pair of rows is computed from B as the first pair's store has left it,
which is not a plain product when the destination is the right-hand slot. The vertex command multiplies by the slot named in DMEM[0x148], which both set.

**Triangles** (op 5). Each 16-byte record: flags (bit 6 -> geometry bit 13, per triangle), three vertex numbers (the slot addresses), three texture words copied into the records at +20. The
setup is `f3d_tri_setup` with the **`F3D_TRIV_RARE`** rules: reject mask 0x7070, clip mask 0x4343; culling: geometry bit 13 switches it on and bit 12 selects the front faces (bit 12 alone does nothing);
the vertex order, area, slopes and the shade and z blocks are F3D's; flat shading takes the colour of the first vertex of the command (`flat`); the **texture block is not scaled** (no K, and no magnitude measure:
the gradients of lanes 4 .. 7 are stored as they are); no minimum z slope (`zclamp` 0). A triangle that needs clipping goes to `f3d_clip_tri` (the F3D clipper, the new vertices are stored at
the ring-high address `0xe10` / `0xe70` + 40 i, as the microcode does), then each fan triangle through `f3d_tri_setup_noclip`.

**State commands.** Geometry mode set / clear (0xb7 / 0xb6) on the word at 0x114, `SETOTHERMODE_H / _L` (0xba / 0xb9: `(1 << len) - 1 << shift` of the word replaced by w1 - the microcode ORs w1 into the
cleared field - then the pair is emitted as `SET_OTHER_MODES`), the texture command (w0, w1 at 0x120; bit 1 of the halfword at 0x116 = w0 bit 0), `G_MOVEWORD` (a word to `DMEM16[0x1be + (w0 & 0xff)] + (w0 >> 8)`),
`G_RDPHALF`s, the display list push / pop (stack at 0x1e6 / 0x1e8 + depth, nine levels), the RDP block (each command through its own room check) and the RDP commands (`SETTIMG` / `SETZIMG` / `SETCIMG` resolve w1 through the
segment table: offset masked with DMEM[0xb8], plus the segment word; in JFG `0xfd` first adds an offset from a table of halfwords at 0x320 when DMEM[0x1e7] is set, a command of its own (0x4d4) loads the table, another (0x314) the two bases that matrix and vertex addresses are relative to).

**The ring.** `ring_room` is the microcode's check at pc 0x178: a command of n bytes that does not fit wraps `s7` to the start of the ring (after the waits, which the framework's model never
needs; a state in which the microcode would wait for ever declines) and writes `DPC_START`; `DPC_END` is written after every command (every command is a list of its own; the commit merges them,
section 1). A triangle reserves 176 bytes whatever its length.

## 7. Registers that outlive a task

The next task of Jet Force Gemini reads `a0` (register 4) as the previous task left it (the pointer to the triangle record being processed, `0x2d0 + 40 k` of the last vertex handled), which is why the build carries
`HLE_F_REGS_LIVE`, `live_sregs = 1 << 4`: the front-end keeps `a0` as the microcode would (triangle command: the first corner's address; the display list command: `d - 36`) and the verify run compares it with the JIT's.
The vector registers are never live. Diddy Kong Racing has no live register.

## 8. What stays on the JIT

The task declines (nothing has been written; the JIT runs it) when: a command is unknown, or a handler needs more bytes than the command's DMA transferred; the vertex slots reach into the command buffer;
a triangle names a vertex outside the vertex buffer; the matrix slot is unusual; the RDP ring would make the microcode wait (`START_VALID` while wrapping, the RDP in the way); the clipper's new vertices
would run past the end of DMEM (JFG's clipper in a crowded scene); the task is a resumed or yielding one; more commands than the status polls allow (`POLL_LIMIT`, a runaway list). The outermost end of the list
hands over to the interpreter, which runs the microcode's own exit overlay. DMEM dead at the end of a task (command buffer, data buffer, ring, three scratch words of the clipper): `l_dead_dkr` /
`l_dead_jfg`.

## 9. What differs from Fast3D

The commands DMA their own data (the display list does not hold vertices or matrices, and a vertex command writes its records to the slots from 0 or from the end of the previous command:
triangles name them by slot number), there is no lighting code and no light commands (the colour bytes are copied through), matrices are loaded into slots rather than pushed and multiplied by the
microcode (the unreferenced multiply routine at pc 0x388 is never entered), a texture rectangle is the class-3 command followed by the `G_RDPHALF` pair, and the lists go through the XBUS ring.

## 10. Jet Force Gemini against Diddy Kong Racing

Same structure, shifted addresses (command buffer 0x730, data buffer 0x870, ring [0xa70, 0xe70), overlay window 0x870 .. 0xa90, descriptors at text 0x1008 / 0x1228 and the tail at 0xf80),
the class-0 address of op 1 / op 4 against DMEM 0x210 / 0x214 instead of the segment table, and a different class-0 prologue (0x80 .. 0xc4). JFG sends most of its geometry as prebuilt RDP
blocks (op 7: 10 % of its commands, ops 4 and 5: 25 of 141 000 in the survey's pad), so the vertex and triangle code is hardly exercised by the scripted pad there; the synthetic tasks of tools/rare/synth.py and the mutation checks of docs/native-custom.md reach it, and
Diddy Kong Racing's captured tasks run the same code.
