# S2DEX 2.x microcode semantics (as the native front-end reproduces them)

S2DEX is the 2D microcode: sprites (`uObjSprite`), backgrounds (`uObjBg`) and texture loads (`uObjTxtr`) turned into RDP texture rectangles. Builds known (three texts of the 2.x generation, all claimed):

| text | games | key / id | what it is |
|---|---|---|---|
| fifo 2.08 | Worms Armageddon (task start, id `fa171b4489d45ef2`) | | the reference of the closed forms below |
| fifo 2.06 | Kirby 64 (loaded by `G_LOAD_UCODE` in the middle of its F3DEX2 2.04H tasks, id `c9655420d2f5f8cf`); Resident Evil 2 (task start, id `5f33af89e713878b`, 75 % of its graphics tasks) | `9748556461d85ab3` | one text: the handlers from 0x184 on are 8 bytes lower than Worms' ([ucode-f3dex2-dialects.md](ucode-f3dex2-dialects.md) sections 5.6 and 5.7) |
| fifo 2.05 [Safe] | Ocarina of Time (loaded by `G_LOAD_UCODE` in the middle of its F3DZEX tasks) | `af535d1683a57fcb` | the 2.06 text with a pipe sync in front of every flush check, 792-byte output buffers, the handlers behind 0x650 0xc..0x10 higher ([section 7](#7-the-safe-build-s2dex-205-ocarina-of-time)) |

The 1.06 / 1.07 builds of Yoshi's Story and Pokemon Puzzle League are not claimed ([section 6](#6-s2dex-1x)).
The front-end is a dialect of the F3DEX2 one (`src/hle/hle_s2dex2.c`, built on the shared header `hle_f3dex2_int.h`): S2DEX 2.x is built on the F3DEX2 skeleton, so the display list refill, the DL stack, the segment table, the other-mode
commands, the RDP command copies and the flush of the output buffer are the F3DEX2 ones at other pcs and DMEM addresses; the sprite and background commands are its own. Addresses are DMEM byte addresses.

## 1. Layout (2.08)

| | |
|---|---|
| jump table by signed opcode | 0x370 |
| display list input buffer / its end | 0x418 / 0x4c0 (168-byte refills) |
| RDP output buffers | 800 bytes (+ 0xb0 slack) at 0x4c0 and 0x8c0 (`s6 ^= 0xc00`; `s6` starts at 0x7e0, `s7` at 0x4c0); the flush is the F3DEX2 one |
| DL stack, depth, segment table | 0x138, 222, 0xf8 (as F3DEX2) |
| `G_MOVEWORD` table (`lhu v0, 10374(at)`) | 0x386 + index |
| vector constants v31 .. v28 | 0x180, 0x190, 0x1a0, 0x1b0 (`v29` carries the RDP command ids: 0x3d10 .. 0x35 SET_TILE, 0x32 SET_TILE_SIZE, 0x24 TEXTURE_RECTANGLE, 0x27 / 0x26 / 0x28 syncs) |
| other-mode words, render-mode tables | 0x1f8 / 0x1fc; 0x214 (8 words), 0x234 (4 words); table pointers 0x244 (0x248 + 8), 0x246 (0x258 + 8) |
| scissor, decoded | 0xc0 / 0xc4 (the command), halfwords 0x204 .. 0x20a |
| object buffers | 0x3a8 (`uObjTxtr`, 24 bytes; the LDTX_ commands read 48 bytes, the sprite lands at 0x3c0), 0x3b0 (`uObjBg`, 40), 0x3c0 (`uObjSprite`, 24) |
| sync state | bytes 0x2ae (the last load state) and 0x2af (the tile toggle 0 / 2); the loaded-texture words at 0x2b0 + class |
| scratch for vector-to-scalar moves | DMEM 0x1c0 .. (`SSV` / `SLV` then `lw`) |

The layout above is 2.08's; the 2.06 text (Kirby 64, Resident Evil 2) has the same DMEM layout and the handlers from 0x184 on 8 bytes lower (loop head 0x1a4, `G_OBJ_LOADTXTR` 0x550, `G_BG_1CYC` 0xb74, `G_BG_COPY` 0x208, `G_OBJ_RECTANGLE` 0xb80, `G_SETSCISSOR` 0x1d0, overlays at IMEM 0x6c0), and the 2.05 text
(Ocarina of Time) 792-byte output buffers and the handlers behind 0x650 higher by 0xc to 0x10 (loop head 0x1a4, `G_BG_1CYC` 0xb84, `G_OBJ_RECTANGLE` 0xb90, overlays at IMEM 0x6d0; [section 7](#7-the-safe-build-s2dex-205-ocarina-of-time)).

Two code overlays share IMEM 0x6c8 (descriptors at DMEM 0x314 and 0x31c: RDRAM offset from the text, length, IMEM address; the offsets point beyond the 4 KiB the survey saw): **A** (sprites and rectangles, at text + 0x648)
and **B** (backgrounds, at text + 0x1198). Each starts with stubs that load the other and return. The front-end tracks which is resident like the F3DEX2 one tracks its clip / light overlays (`OV_CLIP` = A, `OV_LIGHT` = B).

## 2. Commands

| opcode | handler | what it does |
|---|---|---|
| `G_DL` 0xde, `G_ENDDL` 0xdf, `G_MOVEWORD` 0xdb, `G_RDPHALF_1` 0xe1, `G_TEXRECT` 0xe4 / 0xe5, `G_SETOTHERMODE_H / _L` 0xe3 / 0xe2, RDP commands | 0x054, 0x07c, 0x090, 0x01c, 0x018, 0x0b4 / 0x0b8, 0x140 / 0x144 / 0x150 / 0x170 | as in F3DEX2 |
| `G_SETSCISSOR` 0xed | 0x1d8 | words to 0xc0 / 0xc4, the four fields to 0x208 (w0 & 0xfff), 0x204 (w0 >> 12), 0x20a (w1 & 0xfff), 0x206 (w1 >> 12), then copied |
| `G_RDPHALF_2` 0xf1 | 0x128 | 24 bytes: the remembered scissor (0xc0), the rectangle words (0xd0, 0xd4, 0xd8, w1) |
| `G_OBJ_MOVEMEM` 0xdc | 0x0a4 | the DMA of `(w0 >> 16 & 0xff) + 1` bytes from the segment address of w1 to the halfword at `0x388 + (w0 & 0xfff)` |
| `G_SELECT_DL` 0x04 | 0x024 | halfword 0xd0 = w0; if `(DMEM32[0x2b0 + byte 0xd1] & w1) != DMEM32[0xd4]`: that word becomes `(old & ~w1) | (DMEM32[0xd4] & w1)` and the list is called / branched to the address built at 0xd0 (the `G_DL` code) |
| `G_OBJ_RENDERMODE` 0x0b | 0x0e8 | byte 0x268 = w1; 0x244 = 0x248 + (w1 & 8), 0x246 = 0x258 + (w0 & 8); 0x1fc = DMEM32[0x234 + (w1 & 24) / 2], 0x1f8 = DMEM32[0x214 + (w1 & 112) / 4] |
| `G_OBJ_LOADTXTR` 0x05, `LDTX_SPRITE` 0x06, `LDTX_RECT` 0x07, `LDTX_RECT_R` 0x08 | 0x558 | the object to 0x3a8; when its bits are not in the loaded-texture word yet: (SYNC_TILE when 0x2ae says so), SET_TEXTURE_IMAGE, SET_TILE (tile 7), SYNC_LOAD, the load command; then the halfword at `0x202 + (w0 >> 23)` is the pc the microcode continues at (the loop head, the sprite code at 0x6e8, the rectangle code at 0xba8, negative: the rotated rectangle) |
| `G_OBJ_RECTANGLE` 0x01, `_R` 0xda | overlay A 0xb88 | SET_TILE, SET_TILE_SIZE and TEXTURE_RECTANGLE (+ a sync before them when the tile toggles); `s2dex2_obj_rect` in `s2dex2_rect.c` (section 3b) |
| `G_BG_1CYC` 0x09 | overlay B 0x6f4 | the background as strips of texture rectangles; `s2dex2_obj_bg1cyc` (`s2dex2_bg.c`, section 3a) |
| `G_BG_COPY` 0x0a | 0x210 (the 2.06 and 2.05 builds: 0x208) | the background in the COPY cycle type; `s2dex2_obj_bgcopy` (`s2dex2_bg.c`, section 3c) |
| `G_OBJ_SPRITE` 0x02 / 0xd9 | | not implemented (the front-end declines the task); neither Worms nor Kirby 64 uses them |

## 3. Kernels

`s2dex2_fx.h` declares the three kernels, closed forms in plain C: the sprite rectangle (`s2dex2_rect.c`, section 3b) and the backgrounds (`s2dex2_bg.c`, sections 3a and 3c), with the DMEM access, the 16-bit saturation and the vector
constants in `s2dex2_int.h`. Both reproduce the microcode's DMEM scratch writes, so the compare needs no extra dead ranges beyond the buffers. They decline (`S2_UNSUPPORTED`, nothing written, the task goes to the JIT) what the
derivation does not cover; neither is a transliteration (an earlier instruction-by-instruction rectangle on `tools/hle/ref_vu.h` is kept only as a test oracle, [testing.md](testing.md), "Regenerating the microcode oracles").

### 3a. G_BG_1CYC, `src/hle/s2dex2_bg.c`

The uObjBg (40 bytes at 0x3b0: imageX imageW frameX frameW imageY imageH frameY frameH, image pointer 0x3c0, texel size / format bytes 0x3c6 / 0x3c7, flip 0x3cb, scaleW / scaleH 0x3cc / 0x3ce, imageYorig 0x3d0) becomes strips of
texture rectangles. The routine, in the order the file follows it (sizes and frame positions u10.2, image positions u10.5, scales s5.10; the vector constants v28 .. v31 at 0x180 are folded in, the kernel declines any other):

1. **Frame against image and scissor, per axis.** 1 / scale as a 16.16 number (the reciprocal unit, `1 + (r >> 5)`); the scaled image size less one, in whole pixels; the frame size limited to it (mirrored, the frame moves by what it exceeds);
   cut at the scissor's upper left and lower right (0x204 .. 0x20a). Results at 0x3d8 (first x, last x, first y / 4, height / 4). The image position of the first visible pixel: image position + (cut off the frame) x scale (u10.2 x s5.10 -> u10.5).
2. **Wrap.** The position is reduced modulo the image width (each width moves y down by one pixel, 32) and then the image height (each height moves the origin back); the microcode loops, the kernel divides, and declines where the loop would
   not end (a size of 0 or less).
3. **Tile line and strip height.** The line size in TMEM words H (0x3e2) from the frame width x scale, the render-mode bit (0x268 bit 3), the image width and the per-format table at 0x294; its reciprocal and the texel-size table (0x2a4) give the
   rows that fit; the quotient of the height step by it, with the reciprocal unit's estimate and one correction, and the remainder scaled by scaleH, give the per-strip steps (0x3e8, 0x3ec, 0x400 .. 0x40a), the fractional texture row of the first rectangle, and the first image row, wrapped into the height (0x40e).
4. **Commands.** Three setup commands (SET_TILE twice, SET_TILE_SIZE), then per strip: the image rows are loaded (SET_TEXTURE_IMAGE, SYNC_LOAD, LOAD_TILE, with SYNC_TILE and a second SET_TILE where the strip wraps the image's last row, in one or two
   loads), and one TEXTURE_RECTANGLE (preceded by SYNC_PIPE; s from the wrapped x, t from the remainder for the first strip and 0 after, dsdx / dtdy the scales). The strip loop is the microcode's, row counters and all; the flush check of the
   RDP buffer runs after every rectangle (a callback into the front-end). The bytes of a command the microcode leaves alone (the seven bytes after a SYNC) are not written.

Neither a NEON form nor a table-driven one exists: the work is a few dozen 32-bit operations and two reciprocals per background plus about ten per strip.

### 3b. G_OBJ_RECTANGLE, `src/hle/s2dex2_rect.c`

The uObjSprite (24 bytes at 0x3c0: objX scaleW imageW paddingX objY scaleH imageH paddingY as halfwords, a lane each, then imageStride imageAdrs, imageFmt imageSiz imagePal imageFlags) becomes SET_TILE, SET_TILE_SIZE and
TEXTURE_RECTANGLE. Positions are s10.2, sizes u10.5 (texels x 32), the scale s5.10 (the **texture step per pixel**, 1.0 = 0x400: the reciprocal of the magnification). The microcode computes both axes in the lanes of one vector
(lanes 0 and 4 carry the results); the kernel does them in a loop over a = x, y. The render mode (G_OBJ_RENDERMODE) leaves three words and two table pointers in DMEM that parameterise it: `bias_t` (0x1f8, start offset of the texture),
`trim` (0x1fa, taken off the size), `bias_p` (0x1fc, added to the position); table A (0x248 or 0x250: position mask 0xfffc / 0xffff, y fraction mask 0 / 3, s bias 0 / -16) and table B (0x258 or 0x260: factors +1 / -1 of the
mirrored start and step, the tile attributes of SET_TILE's second word). Per axis:

1. **Screen rectangle.** `tl = sat16(pos + bias_p) & mask`; `r = refined 1/scale` (the reciprocal unit's r, plus its integer half `r >> 16`, the carry into the integer half included: `r + (r >> 16)`, to the saturation of that half);
   `width = floor(u16(sat16(size - trim)) * r / 256) >> 16` (10.2 pixels: size / scale); `br = sat16(tl + width)`.
2. **Scissor.** `tl2 = max(tl, ul)`, `br2 = min(br, lr)` (0x204 ..: ulx lrx uly lry, signed); the part cut off the top left is `cut = (tl >= ul) ? 0 : sat16(tl - ul)` (<= 0). Either axis with `tl2 >= br2`: nothing is drawn (no DMEM written).
3. **Texture.** The step is the scale, or for the mirrored axis (imageFlags bit 0 flips x, bit 4 flips y) `lo16(scale * B1)`. The first texel is `bias_t` or, mirrored, the last: `lo16((bias_t + sat16(size - trim) - 1) * B0)`; plus the s
   bias (`sat16`). The cut moves it by cut x step: 10.2 x s5.10 has 12 fractional bits and u10.5 wants 5, so `P = cut * step`, `first = lo16(start - ceil(P / 128))` (the microcode multiplies by 0xfe00 / 2^16 in the accumulator and
   subtracts the low half; the sum never saturates). t is moved besides by the sub-pixel position of the clipped top edge: `sat16(512 * (tl2_y & y_frac))` x dtdy >> 16.
4. **Commands.** The tile is the byte at 0x2af xor 2 (so 0 / 2 alternately), written back to 0x2af and 0x2ae; a SYNC_TILE (0x28, the following seven bytes stale) is written first and kept when the tile equals the previous state byte 0x2ae
   (signed), else overwritten by SET_TILE. SET_TILE: `0x35`, fmt << 21 | siz << 19 | stride << 9 | adrs; the second word tile << 24 | pal << 20 | the table's attributes (the two halfwords are also moved through the scratch word 0x1c0).
   SET_TILE_SIZE: `0x32000000`; `tile`, `(imageW & 0x7ff8) << 9 | imageH >> 3` less 0x4004. TEXTURE_RECTANGLE: `0x24`, (xl, yl) = (br2) and (xh, yh) = (tl2) 12 bits each, then s, t, dsdx, dtdy. In the COPY cycle type (bit 5 of
   byte 0xc9) the lower right is one pixel (4 quarter pixels) less in both axes and dsdx is multiplied by 4.

**Domain.** The vector constants must be 2.08's (v31 .. v28 at 0x180), the table pointers 8-aligned (LDV), and the width accumulator in [0, 2^31): a scale of 0 or negative, or a sprite wider than 8191 pixels, would saturate the
accumulator, and the stale accumulator the microcode reads again for the mirrored start (a borrow below zero when a zero-width sprite is mirrored, which the scissor test throws out anyway) is only reproduced where the width is
positive; the kernel declines the rest. Worms uses unit scales, no flip, and the unquantised mask; the random test uses every other combination.

**NEON.** None, on purpose: two lanes of eight carry data, the rest is about sixty integer operations, a division-free multiply per axis and one table lookup of the reciprocal. Measured on x86 (`armlet-native-replay --bench`, Worms
snapshots) against the instruction-level kernel it replaced: see [testing.md](testing.md), "F3DEX2 dialects and S2DEX 2.x".

### 3c. G_BG_COPY, `src/hle/s2dex2_bg.c`

The same uObjBg (40 bytes at 0x3b0) drawn in the COPY cycle type: unscaled, so there are no reciprocals and the frame is only cut and wrapped once. (Fields as in 3a; the bytes at 0x3c6 and 0x3c7 are the RDP's format and texel size, in that order: section 3a's `siz` / `fmt` were named the
other way round, and its guard rejected a format 4 (I) background and let a size 4 through; the tables are `TILE_BY_FMT` (5 halfwords at 0x2a4) and `TILE_BY_SIZ` (4 pairs at 0x294).) The routine is in the resident code (no overlay), 0x208 .. 0x54c of Kirby 64's text; its
vector part works on the lanes of the uObjBg (x: imageX imageW frameX frameW, y: the next four) and of the scissor (lanes 2, 3 and 6, 7), and with the vector flags clear on entry (VCO is the carry of VSUB / VADD, which the first VSUB consumes; no other command of S2DEX leaves it set, and a stale vector register
does not matter: the random test randomises them) comes to:

1. **Frame against the scissor, per axis** (F frame position, W frame size, lo / hi the scissor, all s10.2): `cut = max(0, lo - F)`, `ex = max(0, F + W - hi)`, `vis = W - cut - ex`. Either `vis <= 0`: nothing is drawn and nothing written (the microcode leaves at 0x278).
2. **The image position of the first visible pixel**: `t = ((imageX >> 3) & 0xfffc) + cut` in quarter pixels, wrapped once into the image (`t - imageSize` when that is not negative); the y one is `yp` (>> 2 again).
3. **The setup commands**: SET_TILE (format, size, the line from the word at 0x3cc, a byte at 0x3c4), SET_TILE_SIZE, SET_TILE: 24 bytes, words from the object and the constants of the vector register v29.
4. **The strips**: `a0 = (imageH & ~3) - 4 yp` rows (less 4 when the table entry `T[1] * Z >> 16` is positive, at most the visible height `at`), in strips of `s5` (the halfword at 0x3ce) rows; per strip 48 bytes, SYNC_LOAD (one byte, the rest stale), SET_TEXTURE_IMAGE (the width field from the halfword at 0x3d4, the image address resolved through the segment table), LOAD_TILE, SYNC_PIPE
   (one byte) and a TEXTURE_RECTANGLE from the table row (0x274 + 8 times the texel size) and the visible x; the last strip is shortened by the product `V * a0` (V the halfword at 0x3d4), the scratch words at 0x1c0 / 0x1c2 get its halves. When rows are left (the image's last rows wrap), a second pass of 88 bytes
   (two loads and rectangles) and a loop again for the remaining rows (`at -= 4`). Each rectangle is followed by the flush check of the RDP buffer (a callback into the front-end), like 3a.
The domain is a screen: every field of the uObjBg except the image position, and the scissor, within +-0x2000, so that no 16-bit lane of the vector part saturates or wraps; the closed form declines the rest (and a texel size past 3, the constants other than 2.08's). The one-byte commands leave seven stale
bytes: the stream compare of a task that switched builds zeroes the first three of them (design.md, "Build switching").

Checked against the microcode itself (the rsp2c translation of Kirby 64's 0x224 .. 0x54c, `tools/hle/oracle/bgcopy_gen.inc`, git-ignored) on random DMEM images with `tools/hle/test_s2dex2.c` (S2BGC_ORACLE): see [testing.md](testing.md), "Build switching, G_BG_COPY and the S2DEX dialect".

## 4. Dead ranges

0x418 .. 0x890 and 0x8c0 .. 0xc90 (input buffer, the two output buffers with their slack), 0xfc8 .. 0xfcc; the 2.06 and 2.05 builds have the same ranges (the 792-byte buffers and the sync's 8 bytes fit in the slack), and they are in the F3DEX2 2.04H entry too (the task starts with that text). Poison runs: `ARMLET_DMEM_POISON=mask` and `all` on Worms, 900 frames, state / pictures / audio identical.

## 5. What was measured

See [testing.md](testing.md), "F3DEX2 dialects and S2DEX 2.x".

## 6. S2DEX 1.x (not claimed)

The 1.06 (Yoshi's Story, `5b327e0de25d12b9`) and 1.07 (Pokemon Puzzle League, `190d04dfa000cd4b`) builds are of the F3DEX 1.x generation, and the F3DEX2 skeleton above does not carry over: a single-level jump table by signed opcode at
0x3aa (`lb t9, 0x500(k1)`), 320-byte display list chunks into 0x3c0 (the input buffer ends at 0x500), one output buffer at 0x588 (threshold 0x8a8) flushed by a routine at 0xdb0 that waits for the DMA and writes DPC_END *after* it,
DL stack depth at 0xd0 and stack at 0x128, and three code overlays at IMEM 0x740 (descriptors at DMEM 0x3d0, 0x3d8, 0x3e0: text + 0x1008 / 0x1228 / 0x13c0), loaded by stubs in the resident part (e.g. opcode 0x01, `G_BG_1CYC`, at 0x6e8
loads the overlay of descriptor 0x3d0). Yoshi's Story executes nearly every command (`G_BG_1CYC` / `G_BG_COPY` / `G_OBJ_RECTANGLE` / `_SPRITE` with its matrix / `G_OBJ_LOADTXTR` / `G_SELECT_DL` / `G_OBJ_RENDERMODE` ...), Pokemon Puzzle League
the same set at lower rates. The commands are the same ones as in 2.x but other code, so the 2.08 kernels do not apply. The interpreter reproduces all 58 captured Yoshi tasks (so the task-level framework would carry a
front-end), but a front-end needs its own loop, flush and a kernel per command, and these tasks are small (a few microseconds on the JIT, see the Worms measurement), so it was not written. To start one: the dumps of `tools/ucode_dump_run.sh`,
the overlay images from the DMA rows of captured task starts (`ARMLET_HLE_CAPTURE`), and `tools/rsp2c/rsp2c.py` to regenerate a reference of a handler (docs/testing.md, "Regenerating the microcode oracles").

## 7. The Safe build (S2DEX 2.05, Ocarina of Time)

The text `G_LOAD_UCODE`d by Ocarina of Time's F3DZEX (banner "Gfx[Safe] S2DEX fifo 2.05" in its data; `l_b_s2_oot` in `hle_s2dex2.c`) is the 2.06 text with, in the flush routine, `addi t3, zero, 0xe7; sb t3, 0(s7); addi s7, s7, 8` in front of the buffer check at 0x644 (entry 0x640 sets the return address
and falls into it): **every handler that writes to the output buffer is followed by a `G_RDPPIPESYNC`**, one byte written, the other seven what the buffer held (the RDP ignores them; the stream compare zeroes them for a task that switched builds). The output buffers are 792 bytes (`s6` 0x7d8 at the start; the second buffer is 0xc00 higher
as before). Nothing else changes in the semantics: the overlay code A (sprites, rectangles; resident in the text image) and B (backgrounds; text + 0x1198) are word for word the 2.06 code with the branch offsets and jump targets relocated by the 0xc / 0x10 bytes the inserted instructions move the resident code.

The front-end parameterises that instead of copying: `f2_layout.out_bytes` (792) and `f2_layout.pipe_sync`; `after_emit`, which every handler's emission ends in (the generic RDP copies, `G_SETSCISSOR`, `G_RDPHALF_2`, `G_OBJ_LOADTXTR`, the rectangle, and the flush callback the background routines call after every strip), writes the sync first. A command that
writes nothing (a sprite outside the scissor, a texture object whose bits are loaded, a background with nothing visible) jumps to the loop head and has none (mutations 308, 309). The kernels are the 2.08 ones, untouched; the continuation pcs of `G_OBJ_LOADTXTR` are derived from the build's own table (the handler of `G_OBJ_SPRITE` / `G_OBJ_RECTANGLE` + 0x20).
`G_OBJ_SPRITE` and the rotated forms still decline. What Ocarina of Time draws: `G_BG_COPY` only.
