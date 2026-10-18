# S2DEX 1.06 / 1.07 microcode semantics (as the native front-end reproduces them)

The 2D microcode of the first generation, on the skeleton of F3DEX 1.x (a single-level jump table by signed opcode, 320-byte display list windows, one RDP output buffer flushed by a threshold check, the F3DEX 1.x command numbers). Its sprite
rectangle and background code is word for word the code of S2DEX 2.x ([ucode-s2dex2.md](ucode-s2dex2.md)) at other addresses, so the closed forms of that page are shared ([section 3](#3-kernels)); the loop, the other commands and the output
are the F3DEX 1.x front-end's (`f3dex.c`) in its own mode (`hle_s2dex1.h`, included by it). Addresses are DMEM byte addresses, pcs are IMEM addresses.

| text | games | id | G_LOAD_UCODE key |
|---|---|---|---|
| S2DEX 1.06 | Yoshi's Story (task start; every task loads F3DEX.NoN 1.22 `fd5a` and back) | `5b327e0de25d12b9` | `3a88b2c21c8ec254` |
| S2DEX 1.07 (data banner "S2DEX 1.07 / S2DEXD 1.07" in Bangaioh and Magical Tetris Challenge) | Bakuretsu Muteki Bangaioh, Magical Tetris Challenge, Pokemon Puzzle League (task start; PPL's F3DEX 1.23 tasks load it) | `190d04dfa000cd4b` | `9f01e13abd30c872` |

The two texts differ in `G_BG_COPY` only (1.07 has the routine of 2.x, 1.06 an older one: [section 4](#4-g_bg_copy)); the code behind it is 0x2c bytes further on in 1.07 (the helpers at 0xdc0 / 0xde8 / 0xe00 against 0xd94 / 0xdb0 / 0xdc4,
`G_OBJ_LOADTXTR` at 0xcb0 against 0xc84). The data segments are the same (the banner and four cells of the jump table aside: the `G_OBJ_LOADTXTR` handler). "S2DEXD" is no other text: the Bangaioh / Magical Tetris tasks run the same `190d`.

## 1. Layout

| | |
|---|---|
| jump table by signed opcode | 0x3aa (cell of opcode `op` at 0x3aa + 2 * (int8) op: the cells of the opcodes 0x0b .. 0x7f lie under the display list window and are not commands) |
| display list window (320 bytes) | 0x3c0 .. 0x500; the loop head 0x158 reads the next command at 0x500 + k1 (k1 from -320 up to 0, refilled at 0x13c) |
| RDP output buffer | 0x588 (`s7`), flushed when `s7 > 0x8a8` (800 bytes plus the longest command), as one chunk, by the routine at 0xdb0 / 0xdbc (1.07: 0xddc / 0xde8) |
| vector constants v31 .. v28 | 0x170 .. 0x1af (2.x: 0x180); the same values as 2.x's (`s2dex2_int.h`, `k_consts`); `s8` = 0x1b0 (scratch word at 0x1b0) |
| scissor, decoded | halfwords 0xc0 (ulx) 0xc2 (lrx) 0xc4 (uly) 0xc6 (lry) |
| other-mode words | 0xc8 (H), 0xcc (L); G_RDPHALF_0 words 0xd8 / 0xdc, G_RDPHALF_1 word 0xe0 |
| DL stack depth, stack | byte 0xd0, 0x128 (the banner follows at 0x138: four nested calls fit) |
| tile state / toggle | bytes 0xd2 (the last load state; the syncs clear it) and 0xd3 (0 / 2) |
| RDP FIFO pointer, segment table | 0xd4, 0xe8 (16 words) |
| render-mode tables | 0x200 (8 words), 0x220 (4 words), pointer tables 0x230 / 0x238 and 0x240 / 0x248, their pointers 0x252 / 0x254, the byte 0x250; the words G_OBJ_RENDERMODE leaves 0x1e8 / 0x1ec |
| object buffers | 0x518 (`uObjTxtr`, 24 bytes; 48 for the LDTX_ commands, the sprite lands at 0x530), 0x520 (`uObjBg`, 40), 0x530 (`uObjSprite`, 24); the matrices of `G_OBJ_MOVEMEM` at 0x1d0 (MMTX) and 0x1e0 (SUBMTX); the results area of the backgrounds 0x548 .. 0x57f |
| flags words | 0x2a0 + the class (G_SELECT_DL's flags, the loaded-texture words of `G_OBJ_LOADTXTR`: a game's indexes are 0, 4 and 8); the byte at 0x2b0 ... holds the sprite flip table (16 bytes per row) |
| overlay DMA table | 0x2f0 + 8k: RDRAM address (the text's, added by the initialisation), length - 1, IMEM address. k = 0: IMEM 0 (text + 0xf80, 0x138 bytes, the handlers of the lowest pcs, loaded once by the initialisation), k = 1: IMEM 0x238 (text + 0x1b8, 0x730 bytes: sprites, rectangles), k = 2: IMEM 0x238 (text + 0x10b8: backgrounds) |
| tables of the commands | G_MOVEWORD destinations 0x3b6 + index (index 6: the segment table 0xe8, 8: the flags 0x2a0), G_OBJ_MOVEMEM destinations 0x3b8 + (w0 & 0xfff) (0x1d0, 0x1e0, 0xe8, 0x2a0), the continuation of G_OBJ_LOADTXTR at 0x76 + (w0 >> 23) (0x1f8: the loop head, the sprite code 0x258, the rectangle code 0x714, the rotated rectangle 0x9714) |

The overlay in IMEM 0x238 is the sprite / rectangle code (1) or the background code (2); each starts with stubs that load the other (`t3` = 8 or 16, `j 0xd7c`). The front-end tracks the resident one (`f3dex_task.s2ov`) and DMAs the other when a command needs it.

## 2. Commands

The handlers are found by the pc the jump table holds for the opcode (`s2d1_build.pc`, `S2D1_PC_FIELDS`); the opcodes are the GBI's of F3DEX 1.x and S2DEX 1.x.

| opcode | handler | what it does |
|---|---|---|
| `G_BG_1CYC` 0x01 | 0x6e8 (stub; overlay 2) | the uObjBg (40 bytes) to 0x520, the background as strips of texture rectangles: `s2dex1_obj_bg1cyc` (section 3) |
| `G_BG_COPY` 0x02 | 0x968 | the uObjBg to 0x520, the background in the COPY cycle type: `s2dex1_obj_bgcopy106` / `s2dex1_obj_bgcopy` (section 4) |
| `G_OBJ_RECTANGLE` 0x03, `_R` 0xb2 | 0x6f4 (overlay 1) | the uObjSprite (24 bytes) to 0x530, SET_TILE, SET_TILE_SIZE, TEXTURE_RECTANGLE: `s2dex1_obj_rect`; the `_R` form (w0 < 0), whose position and scale come through the sub-matrix: `s2dex1_obj_rect_r` (section 3c) |
| `G_OBJ_SPRITE` 0x04 | 0x238 (overlay 1) | the uObjSprite to 0x530, then the sprite through the object matrix as two textured triangles: `s2dex1_obj_sprite` (section 3b) |
| `G_OBJ_MOVEMEM` 0x05 | 0x1dc | the DMA of the byte 1 of w0 + 1 bytes from the segment address of w1 to the table cell of w0 |
| `G_DL` 0x06 | 0x1b4 | call (byte 1 of w0 zero: the return address is pushed at 0x128 + depth) or branch to the segment address of w1 |
| `G_SELECT_DL` 0xb0 | 0x184 | halfword 0xd8 = w0; when the flags word at 0x2a0 + the byte 0xd9 (the old one) lacks the bits of w1 as 0xdc has them: the word is updated and the list at the word 0xd8 is called / branched to as `G_DL` does |
| `G_OBJ_RENDERMODE` 0xb1 | 0x0c8 | the byte 0x250, the pointers 0x252 / 0x254, the words 0x1e8 / 0x1ec from the tables |
| `G_OBJ_LOADTXTR` 0xc1, `LDTX_SPRITE` 0xc2, `LDTX_RECT` 0xc3, `LDTX_RECT_R` 0xc4 | 0xc84 (1.07: 0xcb0) | the texture object to 0x518 and its load commands as the microcode of 2.x (`s2_loadtxtr`, `hle_s2dex1.h`); then the continuation of the table: nothing, the sprite code (`s2dex1_obj_sprite` without its DMA), the rectangle code (`s2dex1_obj_rect`, or `_r` when the table entry is negative) |
| `G_RDPHALF_0` 0xe4 / 0xe5, `_1` 0xb4, `_2` 0xb3 | 0x000, 0x00c, 0x014 | w0 / w1 to 0xd8 / 0xdc, w1 to 0xe0; `_2` emits the 16 bytes (0xd8, 0xdc, 0xe0, w1): a texture rectangle |
| `G_SETOTHERMODE_L / _H` 0xb9 / 0xba | 0x054 / 0x050 | the word at 0xcc / 0xc8 is merged (mask from the shift in w0 bits 8 .. 12 and the length in w0 bits 0 .. 4, as the microcode computes it, a length of 0 or 32 included) and the pair (0xc8, 0xcc) emitted, **unless the next command of the window is another one of the two**, which emits it (the microcode peeks at the next opcode when the window is not used up) |
| `G_RDPSETOTHERMODE` 0xef | 0x108 | w0 / w1 to 0xc8 / 0xcc, copied |
| `G_SETSCISSOR` 0xed | 0x1f4 | the four fields decoded to 0xc0 .. 0xc6, copied |
| `G_MOVEWORD` 0xbc | 0x0b0 | w1 to (the cell 0x3b6 + index) + (w0 >> 8), mod 0x1000: the front-end takes the segment table and the flags words (index 6 and 8) and declines other destinations |
| `G_ENDDL` 0xb8 | 0x038 | pops the return address (k0 = DMEM 0x128 + depth - 4); depth 0 ends the task |
| `G_LOAD_UCODE` 0xaf | 0xf6c | [section 6](#6-g_load_ucode) |
| RDP syncs 0xe6 .. 0xe9 | 0x220 | the tile state byte 0xd2 := 0, then copied |
| `G_SETTIMG` / `_ZIMG` / `_CIMG` 0xfd .. 0xff | 0x114 | copied, w1 resolved through the segment table |
| the other RDP commands 0xc0 .. 0xfc | 0x224 | copied (8 bytes) |
| 0xf1 | 0x158 | no-op |

## 3. Kernels

The sprite rectangle (`s2dex2_rect.c`) and the background (`s2dex2_bg.c`, `G_BG_1CYC`) are compiled twice: for 2.x, and with `S2_L1` for 1.x as `s2dex1_rect.c` / `s2dex1_bg.c` (functions `s2dex1_obj_rect`, `s2dex1_obj_bg1cyc`). The layout is the only
difference (`s2dex2_int.h`, `L_*`): the object block (the uObjBg 0x3b0, the sprite 0x3c0 ... the results area up to 0x40f) is 0x170 higher in 1.x, the scratch word at 0x1b0 (0x1c0), the scissor at 0xc0 (0x204), the tile state and toggle bytes
0xd2 / 0xd3 (0x2ae / 0x2af), the render-mode words and pointers 0x1e8 / 0x252 / 0x254 / 0x250 (0x1f8 / 0x244 / 0x246 / 0x268), the tile tables by texel size and format 0x280 / 0x290 (0x294 / 0x2a4), the SET_TILE word 0x24c (0x264); the constants are at 0x170
(0x180). The routines were compared instruction by instruction (rect: 0x6f4 .. 0x8ec of 1.06 against 0xb88 .. 0xd80 of Worms' 2.08, backgrounds: overlay 2 against overlay B): the only differences are those addresses (and the stubs and flush calls around
them), nothing else. The decline conditions are those of the 2.x forms (vector constants, 8-aligned table pointers, a width that does not fit, wraps that would not end ...); a case the kernel declines declines the task.

### 3b. G_OBJ_SPRITE, `src/hle/s2dex1_sprite.c`

The sprite code at 0x258 .. 0x64c (entered at 0x238 for `G_OBJ_SPRITE`, at 0x258 for `LDTX_SPRITE` after the load) has no counterpart in the 2.x kernels (S2DEX 2.x declines it). The uObjSprite at 0x530 and the matrix of `G_OBJ_MOVEMEM` (0x1d0: A B C D as s15.16 halfword pairs; 0x1e0: X Y s10.2, BaseScale) become SET_TILE, SET_TILE_SIZE, a SYNC_PIPE (one byte) and two `TEX_TRIANGLE`
commands (0x0a, 96 bytes each: no shade, no z). The code works on vectors that hold one corner per lane; the closed form follows it in the order of its steps (the numbers of the chains are `hle_fx.h`'s):

1. **The corner rectangle** per axis, as `G_OBJ_RECTANGLE`'s width: the reciprocal of the scale (`refined_recip`), `(size - trim)` times it, the low 16 bits of `(that * 0x100) >> 16` as the width `W` in 10.2 pixels. The four corners are `base + row[l] * W` with `base = pos + bias`
   (DMEM 0x1ee) and a row of a table of 4 x 8 halfwords at 0x2b0 (selectors 0 / 1: the row is picked by the sign bits of the matrix bytes 0x1d8 and 0x1dc, `((b1d8 ^ sext(b1dc)) & 0x180) >> 3`); the texture coordinates `(bt + bias_s) + sel * (size - trim)` with `sel = row ^ flip`, the flip bits of `imageFlags` (bit 0: s, bit 4: t).
2. **The matrix**: `x' = (X + c) & mask + x A + y B`, `y' = (Y + c) & mask + x C + y D` in 16.16, the integer half saturated (the position bias `c` is the third lane of the render-mode table, `mask` its first: the rectangle's table). The 16 bytes of the local corners are stored at 0x1b0 (SQV) before the scissor test and
   the four y' over their first half after it.
3. **The scissor**: nothing is drawn unless some x' is >= ulx, some x' < lrx, some y' >= uly and some y' < lry (a bounding test, not a clip). The corners (0, 1) and (2, 3) are then ordered by y (swapped when `y0 >= y1` / `y2 >= y3`), texture coordinates with them.
4. **The tile commands** as the rectangle's (toggle, SYNC_TILE kept when the toggle meets the state byte, SET_TILE, SET_TILE_SIZE), 24 bytes with the SYNC_PIPE.
5. **The triangles** (corners 2, 0, 1 and 0, 1, 3: with the corners ordered above, the four y's are in increasing order for a rotation and scale, the code does not rely on it). A vertex is `[x x x s t 0x7fff y y]`; with `d_ca = c - a`, `d_ba`, `d_ab`, `d_cb` (saturated):
   `N = d_ca * dy_ba + d_ab * dy_ca` per lane (the lane of an x is twice the area), `M = d_ba * dx_ca + d_ca * dx_ab`; the reciprocal of `N_x` (scaled by 1/2 or 1/256 when `|N_hi| < 8` is true or not: the scale `K`, then undone on the gradients) gives the gradients
   `gx = N / area`, `gy = M / area` (x 8 after the scale), the edge slopes come from the refined reciprocals of the three `dy` (a Newton step: `R2 = 2 R`, `E = 2 - R2 dy`, `R2 E`) times `dx`, the start values are `a - dE * (y_a & 3) / 4` with `dE = gx slope_long + gy`. The header's `lft` flag is the sign of the scaled area.
   Every lane the command stores is computed, the fourth halfword of each texture group (lane 6, the y lane) included.

The accumulator's low half is the *result of the last VADD / VAND* (the RSP's logic and add instructions write it): `s2dex1_sprite.c` needs that nowhere, the rotated rectangle does (section 3c). The vector flags are taken to be clear on entry (VCO is consumed by the first VSUB), as for the other kernels; a random-state test shows the stale vector
registers do not matter and the flags do.

### 3c. G_OBJ_RECTANGLE_R and LDTX_RECT_R (`s2dex1_obj_rect_r`, the front of `s2dex2_rect.c`)

The code at 0x8f4 .. 0x95c replaces the first three lines of the rectangle's and joins it at 0x728. Per axis: `P = pos * 2048`, `RR` = the reciprocal of BaseScale (0x1e4 / 0x1e6) refined by the multiply chain (not VADDC / VADD: they differ for a negative reciprocal), `full = P * RR / 65536`; the top left is the integer half of `full` plus the translation
`sat16(X + bias_p) & mask` (the accumulator's low half holds the VAND result, the product's own is gone), the fraction `full & 0xffff` carries into the bottom right, and the scale is `BaseScale * 64 * scale >> 16`. Everything after that is the rectangle's.

The flush check after every strip is a callback into the front-end (`s2c_flush`): the chunk is handed to the FIFO when `s7 > 0x8a8` (the microcode calls 0xdbc after each strip).

## 4. G_BG_COPY

1.07's routine is 2.x's (`s2dex2_obj_bgcopy` compiled with `S2_L1`: `s2dex1_obj_bgcopy`, the 1.07 text 0x968 .. 0xcb0). 1.06's (0x968 .. 0xc80) differs in four places and has its own function, `s2dex1_obj_bgcopy106` (`s2dex1_bgcopy.c`, a copy of the 2.x routine with these changes):

1. a mirrored background's y origin is `(v10 << 2) & 0xffff` (2.x: `(v10 << 2) - 1`);
2. the product that moves the row pointer of the last strip is taken with `MFC2` of the `VMUDN` lane, its low 16 bits sign-extended (2.x: both halves through DMEM, a 32-bit value), and the pointer is not combined with `imageYorig` for the mirrored case;
3. the strip is `SET_TEXTURE_IMAGE`, `SYNC_LOAD` (one byte), the load, `SYNC_PIPE` (one byte), `TEXTURE_RECTANGLE`: 48 bytes (2.x: `SYNC_LOAD` first);
4. the second pass (the rows past the image's last) is 80 bytes: `SET_TEXTURE_IMAGE`, `SYNC_LOAD`, the load (with the tile number 7 in the second word), `SYNC_PIPE`, `SET_TEXTURE_IMAGE`, `SET_TILE`, the load, `SYNC_PIPE`, `TEXTURE_RECTANGLE`; there is no `SET_TILE` in front of the first load.

## 5. The loop, the output and the end of the task

The task starts as an F3DEX 1.x task does: the boot code and the initialisation on the interpreter up to the first fetch (pc 0x158 with k1 = -320, `s7` = 0x588), then `s2_cmd` (`hle_s2dex1.h`) per command. The window is read as the microcode reads it (the DMA, which also leaves
the DMA registers where the microcode does); the commands are assembled in DMEM at `s7`, and when `s7` is above 0x8a8 after a command the bytes from 0x588 go to the FIFO as one chunk (`out_reserve`: the wrap of the FIFO is decided per chunk, as in the microcode's flush at 0xdc4:
the room check, the wait for the RDP, DPC_START; the lean path stages the chunk and leaves the DMA write registers as `hle_dma_write` does). The end of the task (the outermost `G_ENDDL`) is the microcode's own code on the interpreter after the staged chunks have gone to RDRAM: it flushes the buffer, sets SP_STATUS
and breaks.

Declined besides the sprite family ([section 7](#7-declined)): a display list stack deeper than 18 entries (`G_DL`), a `G_MOVEWORD` or `G_SELECT_DL` flags / texture class word outside 0x2a0 .. 0x2ab (the cell of opcode 0x80 ends the range: no game sends it), a `G_OBJ_MOVEMEM` into the jump table, an object read from inside the RDP output ring, a texture object of
more than 0x70 bytes, an opcode whose cell is not a handler of the build.

Dead DMEM at the end of a task (`l_dead_s2d1`): the window 0x3c0 .. 0x500 (the lean run reads the list from RDRAM), the output buffer with its slack 0x588 .. 0x960, and the F3DEX 1.x ranges (the tasks load an F3DEX 1.x text and back; what it leaves in its window, output buffer and
scratch is not kept by the lean F3DEX path) except the two pad bytes at 0x2a3 / 0x2a7, which are S2DEX's flags words.

## 6. G_LOAD_UCODE

Both directions are the F3DEX 1.x front-end's `load_ucode`: the staged output goes to the FIFO, the microcode's own handler (0xf6c in S2DEX: k0 + k1 is the list position, the output buffer is flushed, the text and the data are DMAed in) runs on the interpreter up to the first fetch of the text that was loaded,
and the loop takes that build over (`enter`). Yoshi's Story: S2DEX 1.06 -> F3DEX.NoN 1.22 -> S2DEX 1.06 in every task; Pokemon Puzzle League: F3DEX 1.23 `0472` -> S2DEX 1.07 and the reverse. The builds are found by the key (the first 0xf48 bytes).

The part of the text that stays in IMEM must be the same in the text that loads and the one that is loaded (text 0xf18 .. 0xf7f, IMEM 0xf98 ..). It is, but for two words: the wait for the DMA (IMEM 0xfc8 / 0xfcc) is `bne t3, zero, <itself>` with the status read repeated in the delay slot in S2DEX and `bne t3, zero, <read>` in F3DEX 1.2x. The DMA is
always done in the emulation, so a wait ends at its first read and the two are the same; `tail_differs` leaves those two words out. The comparison used to cover the text up to 0x1000, which includes the overlay code that follows the part copied to IMEM (text 0xf80 ..): that is not IMEM and differs between S2DEX and F3DEX.

## 7. Not implemented

Nothing a game sends. The kernels decline what the 2.x forms decline (domain of the vector part: a scale of 0 or negative that makes a width negative, a case where the background loop would not end, constants other than the standard ones, a table pointer that is not 8-aligned), the front-end the situations of section 5. `G_OBJ_RECTANGLE_R`
and the sprites with a base scale of 0 or negative reciprocal are inside the tested domain (negative base scales included).

## 8. Why the loose tier did not take `190d`

(docs/ucode-coverage.md: "not investigated why the arm does not take it".) `tools/derive_build.py derive` on the 1.07 text: family `f3dex2` (S2DEX 2.x's references explain the rectangle and background code, 1166 of 1944 words aligned: the arm's floor of half is met), primary reference `l_b_s2_kirby`, 291 unexplained words, and the record's layout
fields **conflict**: `lay.in_end` is read as 0x588 at one place (the output buffer) and as 0x500 at another (the end of the window), `lay.desc_clip` as 0x300 and as 0x8. A conflicting constant sets `knob_trouble`, which both arms
(the loose bound and the default-record arm) refuse (`probe.c`: "no conflicting constant"). Even without the conflict the record would run the wrong loop: no `f3dex2_build` field says "single-level jump table at 0x3aa, window 0x3c0, one output buffer at 0x588". The text has a build of its own now and needs no derivation.
