# State commands of F3DEX 1.x / F3DLX: G_SETGEOMETRYMODE, G_CLEARGEOMETRYMODE, G_TEXTURE, G_MOVEWORD, G_MOVEMEM, G_SETOTHERMODE_L/H, MODIFYVTX / PERSPNORM / RDPHALF, LOAD_UCODE

Reference build: F3DEX 1.21 `banjo121` (all pcs below are banjo pcs unless a build is named). Spec routines: `tools/native-f3dex/spec/state_spec.c` + `state_gen.inc`
(`spec.f3dex.state.*`); section 10 says what is verified. "Read" means read from the disassembly only, not checked by the harness.

## 0. Entry state the dispatch leaves, and the DMEM words of this area

Display-list loop (banjo 0x060..0x098): `$25` = w0, `$24` = w1, `$27` = DMEM pointer after the command (the command is at `$27-8` (w0) / `$27-4` (w1); big-endian,
so w0 bits 31..24 = byte `-8($27)`, 23..16 = `-7`, 15..8 = `-6`, 7..0 = `-5`), `$29` = 0x110 (a constant base register; never written by the microcode, set by the OS/boot), class = w0 bits 31..30.
Class 0 (opcodes 0x00..0x3f) also: `$22` = 0xae0, DMA of `w0 & 0x3ff` bytes (RD_LEN value; the DMA moves `((len | 7) + 1)` bytes) from the segment-translated `w1` into 0xae0 was issued and
finished before the handler runs, `$1` = `lbu -7($27)` = w0 bits 23..16. Class 2 (0x80..0xbf) handlers are reached through `lh 0x76 + ((opcode << 1) & 0xfe)`; class 0 through `lh 0xc0 + (opcode << 1)`
(0xc4 + ... in 0.95); the class table is at 0xb8 (0xbc in 0.95): class 0 -> 0x390 (banjo; per build: ts2_dex 0x388, ts2_dlx 0x398, 0.95 0x418 / 0x428), class 1 -> 0x09c (nothing), class 2 -> 0x1ac, class 3 (RDP commands 0xc0..0xff) -> 0x358.
The two loop heads: **0x09c** = `mfc0 SP_STATUS`, yield check (bit 7), then falls into **0x0ac** = "next command if `$28 != $27` else refill the buffer". Handlers end with `j 0x09c` (check yield) unless noted.

DMEM words (all banjo; identical in every build unless noted; initial values from `kit/data`):

| addr | size | meaning | written by | read by (banjo pcs) | initial |
|---|---|---|---|---|---|
| 0x10c | 4 | RDPHALF_1 word | RDPHALF_1 | RDPHALF_2 tail (0x354), BRANCH_Z (0x308), LOAD_UCODE (0xfac) | 0 |
| 0x110 | 4 | 1.21..1.23: PERSPNORM word, the vertex code reads its **low half 0x112** (`LSV 1($29)` at 0x668, 0x980). 0.95: byte 0x110 is the DL-stack depth, 0x112 the PERSPNORM halfword | MOVEWORD idx 14 (1.2x) / PERSPNORM (0.95) | vertex 0x668, triangle 0x980 | 0x0000ffff (0.95/others: 0x112 = 0xffff too) |
| 0x114 | 4 | geometry mode | SET/CLEARGEOMETRYMODE, TEXTURE (bit 1) | section 3 | 0 (F3DEX); 0x00800000 (both F3DLX builds) |
| 0x118 | 4 | other-mode word "H" = first word of the RDP SET_OTHER_MODES command | SETOTHERMODE_H | 0x2ac (emit) | 0xef080cff |
| 0x11c | 4 | other-mode word "L" = second word | SETOTHERMODE_L | 0x2b4 (emit) | 0 |
| 0x120 | 4 | TEXTURE w0 (raw) | TEXTURE | triangle: byte 0x122 (`lb 18($29)`, 0xa9c) | 0 |
| 0x124 | 4 | TEXTURE w1 (raw: s scale high half, t scale low half) | TEXTURE | vertex `LLV 5($29)` (0x3c8, 0x3d0) | 0 |
| 0x128 | 4 | RDRAM pointer of the RDP list end (flush bookkeeping, rdp doc); F3DLX writes DPC_END from it before every RDP command (section 7) | flush | 0x12c, 0x19c | 0 |
| 0x12c | 4 | number-of-lights word: bit 31 = "model-space light vectors are stale", bits 11..0 = `(n+1)*32` (n directional lights, plus the ambient record) | MOVEWORD idx 2; MTX (0x558) and POPMTX (0x204) store 0xff in its top byte (`SBV v31,6,28($29)`; v31 lane 3 = 0xffff) | lighting overlay 0x72c (`lw $1,300($0)`), cleared there 0x7c4/0x7c8 | 0x80000040 |
| 0x138 | 8 | G_MV_TXTATT target | MOVEMEM idx 0x96 | nothing found in any overlay (no absolute reader) | `4000 4000 0000 0000` |
| 0x70..0x8f | 32 | clip ratio words: RNX 0x74, RNY 0x7c, RPX 0x84, RPY 0x8c (the other words are constants) | MOVEWORD idx 4 | vertex 0x414 (`LSV v21,0,59($0)` = 0x76 only); clip overlay `LDV v1,0,14($8)` with `$8 = 4*plane` (0x808) | RNX=1, RNY=1, RPX=-1, RPY=-1 (`0000 0001`, `0000 ffff`) |
| 0x160..0x19f | 64 | segment table: 16 words | MOVEWORD idx 6 | address translation 0x10c | 0 |
| 0x1b0 / 0x1d0 | 16 | raw LookAt Y / LookAt X record | MOVEMEM idx 0x82 / 0x84 | lighting recompute (0x810) | defaults `0080 0000 0080 0000 7f00..` / `.. 007f` |
| 0x1f0 + 0x20*k (k = 0..7) | 16 | raw Light k; k = n is the ambient light | MOVEMEM idx 0x86 + 2k; MOVEWORD idx 10 | lighting overlay 0x740, 0x754 | 0 |
| record + 0x10 | 8 | derived (model-space) direction of the record above, written by the lighting recompute | lighting overlay 0x898 | 0x74c (`LPV 56($1)`) | - |
| 0x380 | 16 | viewport: s16 scale[4], s16 trans[4] | MOVEMEM idx 0x80 | vertex prep 0x660 | 0 |
| 0x390 | 4 | fog: s16 multiplier (0x390), s16 offset (0x392); 0x394 = 0x00ff (max clamp, constant) | MOVEWORD idx 8 | vertex 0x4e0 (`LQV v3,0,57($0)`) | `0100 0000` |
| 0x460..0x49f | 64 | model-view-projection matrix cache: 16 s16 integer halves (0x460..0x47f) then 16 u16 fractions (0x480..0x49f) | MOVEMEM idx 0x9e,0x98,0x9a,0x9c (16 bytes each, 0x460,0x470,0x480,0x490); MOVEWORD idx 0 | vertex 0x69c.. | 0 |
| 0x31e + 2*i | 64 | vertex slot address table (`04a0 04c8 ...`, pitch 0x28) | - (constant; 0x31c in 0.95) | MODIFYVTX 0x338, triangle loaders 0x1d0 | |

## 1. Handler entry / exit pcs per build

Names are fixed by the opcode tables (DMEM 0x76.. / 0xc0..) and by the code; the code of all handlers below is instruction-identical in every build (`pcmap` all `=`, checked) except where marked.

| build | SETGEOM (b7) | CLEARGEOM (b6) | TEXTURE (bb) | MOVEWORD (bc) | SOM_L (b9) | SOM_H (ba) | MODIFYVTX (b2) | RDPHALF_1 | RDPHALF_2 | MOVEMEM (03) | segment translate | RDP-tail exit (jal flush) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| banjo121 | 0x310 | 0x320 | 0x250 | 0x238 | 0x27c | 0x274 | 0x334 | 0x348 (b4) | 0x350 (b3) | 0x6d0 | 0x10c | 0x37c |
| sf64_non122 | 0x310 | 0x320 | 0x250 | 0x238 | 0x27c | 0x274 | 0x334 | 0x348 | 0x350 | 0x6f0 | 0x10c | 0x37c |
| ts2_f3dex123 | 0x308 | 0x318 | 0x248 | 0x230 | 0x274 | 0x26c | 0x32c | 0x340 | 0x348 | 0x6c8 | 0x104 | 0x374 |
| ts2_f3dlx123 | 0x300 | 0x310 | 0x240 | 0x228 | 0x26c | 0x264 | 0x324 | 0x338 | 0x340 | 0x6f4 | 0x104 | 0x378 |
| mk64_f3dex95 | 0x3a4 | 0x3b4 | 0x300 | 0x2e8 | 0x32c | 0x324 | - (b2 = RDPHALF_2) | 0x3d0 (b3) | 0x3d8 (b2) | 0x758 (op 03) | 0x10c | 0x404 |
| mk64_f3dlx95 | 0x39c | 0x3ac | 0x2f8 | 0x2e0 | 0x324 | 0x31c | - | 0x3c8 | 0x3d0 | 0x784 | 0x10c | 0x408 |

0.95 has PERSPNORM as its own command b4 (mk64_f3dex95 0x3c8, mk64_f3dlx95 0x3c0) and no MODIFYVTX, no BRANCH_Z (b0 = nothing), but a QUAD at b5 (not mine). The 1.2x builds have b5 = nothing, b0 = BRANCH_Z (banjo 0x2f8),
b8 ENDDL (0x2d8), bd POPMTX (0x204), be CULLDL (0x2b8), b1 TRI2 (0x1e4), bf TRI1 (0x1bc), af LOAD_UCODE (0xf8c; 0xf80 in both F3DLX builds, absent in 0.95).
Class 0: 01 MTX (banjo 0x554), 03 MOVEMEM, 04 VTX (0x3a8), 06 DL (0x6e0); 00, 02, 05, 07.. = nothing (0x09c). The class-0 table (0xc0) and the class-2 table (0x76) overlap in DMEM, so class-0 opcodes 0x0b..0x1a
are aliases of the class-2 handlers b0..bf (without meaning; not issued by games). Class-2 opcodes 0x80..0xa4 index into constants (undefined).

## 2. Common notes on addressing

* All `lh/lhu/lw/sw` below use the RSP's unaligned behaviour: byte-wise, address mod 0x1000 (checked: the harness feeds odd table indices and wild offsets, the JIT and the byte-wise spec agree).
* `lh` of a table entry sign-extends; every real table value is < 0x8000, so it is irrelevant (a mutation of this was *not* caught by random data for that reason).

## 3. G_SETGEOMETRYMODE (0xb7) / G_CLEARGEOMETRYMODE (0xb6)

w0 = op<<24 (rest ignored), w1 = mask. `geom (DMEM 0x114, u32) |= w1` (SET) / `&= ~w1` (CLEAR). Nothing else; no side effect on lights or matrices; exit 0x09c. No validation of bits (any 32 bits settable).
Verified on all six builds (`setgeom`, `cleargeom`).

Bits of the geometry word as the other code reads them. Readers: vertex prep `lh $3,4($29)` (0x66c) -> `$3` = geom bits 31..16; triangle `lw $13,4($29)` (0x978) = whole word; triangle `lb $5,7($29)` (0xa80) = bits 7..0;
F3DLX `lb $9,5($29)` = bits 23..16 (signed). No other instruction in any of the four overlay images of any build reads 0x114..0x117 (all `($29)` and `($0)` accesses were enumerated).

| bit | mask | libultra name (F3DEX gbi, not in the microcode) | tested at (banjo pc) | effect |
|---|---|---|---|---|
| 0 | 0x00000001 | G_ZBUFFER | 0xa80/0xa94/0xab8 | low byte `geom & 0xff` is ORed with 0xc8 and stored as the first byte of the RDP triangle command (0xc8 non-shaded, +1 Z buffer) |
| 1 | 0x00000002 | G_TEXTURE_ENABLE | same | same (+2 = textured); G_TEXTURE copies its "on" flag here (section 4) |
| 2 | 0x00000004 | G_SHADE | same | same (+4 = shaded) |
| 3..7 | 0xf8 | - | same | ORed into the same byte as well: bits 3, 6, 7 are already set in 0xc8; **bits 4 and 5 would change the RDP opcode** (games never set them) |
| 9 | 0x00000200 | G_SHADING_SMOOTH | 0xc84 (`andi $7,$13,512`) | set: each vertex's own colour; clear: flat, the colour of the triangle's first vertex (`$4 = $1`) is used for all three |
| 12 | 0x00001000 | G_CULL_FRONT | 0xa38 (`sll $14,$13,19`, `nor`, `or` with the area sign, `bgez` -> reject) | cull when the area sign is positive (see triangle doc) |
| 13 | 0x00002000 | G_CULL_BACK | 0x9c4 (`sll $15,$13,18`, `and` with area sign, `bltz` -> reject) | cull when the area sign is negative |
| 16 | 0x00010000 | G_FOG | 0x4ac (`andi $12,$3,1`), branch 0x4d8 | vertex: compute the fog alpha from 0x390 and store it as the colour alpha |
| 17 | 0x00020000 | G_LIGHTING | 0x3f0 (`andi $1,$3,2`), branch 0x408 -> lighting overlay | vertex colour from lights instead of vertex colour/normal bytes |
| 18 | 0x00040000 | G_TEXTURE_GEN | lighting overlay 0x778 (`andi $8,$3,4`) | texture coordinates generated from the normal |
| 19 | 0x00080000 | G_TEXTURE_GEN_LINEAR | lighting overlay 0x790 (`andi $8,$3,8`) | linear variant of the above |
| 20 | 0x00100000 | G_LOD | not tested anywhere | no effect |
| 23 | 0x00800000 | G_CLIPPING | **F3DEX: not tested** (always clips). F3DLX: vertex `andi $24,$3,128` (ts2_dlx 0x400, beq 0x420) and triangle `lb $9,5($29)`; `bgez $9` (ts2_dlx 0x960/0x96c) | clear: the vertex clip-flag computation is skipped and the triangle's trivial-reject/clip test is skipped |
| others | | | | no reader |

Per-build pcs of the same reads (vertex fog `andi $12,$3,1` / lighting `andi $1,$3,2` / overlay texgen `andi 4` / `andi 8` / triangle `lw $13` / `sll 18` / `sll 19` / `andi 512` / `lb 7($29)` / `lb 18($29)`):
banjo 0x4ac/0x3f0/0x778/0x790/0x978/0x9c4/0xa38/0xc84/0xa80/0xa9c; non122 0x4cc/0x3f0/0x798/0x7b0/0x998/0x9e4/0xa58/0xca4/0xaa0/0xabc;
ts2_dex 0x4a4/0x3e8/0x770/0x788/0x970/0x9bc/0xa30/0xc7c/0xa78/0xa94; ts2_dlx 0x4bc/0x3f8/0x798/0x7b0/0x9a4/0x9ec/0xa90/0xc54/0xae4/0xb08 (plus G_CLIPPING above);
mk64_dex 0x534/0x478/0x800/0x818/0xa00/0xa4c/0xac0/0xd0c/0xb08/0xb24; mk64_dlx 0x54c/0x488/0x828/0x840/0xa34/0xa7c/0xb20/0xce4/0xb74/0xb98 (G_CLIPPING: vertex 0x490, triangle `lb $9,5($29)` 0x9f0/0x9fc).

## 4. G_TEXTURE (0xbb)

w0 (libultra layout; the handler only tests bit 0): bits 23..16 = 0, bits 13..11 = level, bits 10..8 = tile, **bit 0 = on**; w1 = s scale (bits 31..16, u16) and t scale (bits 15..0, u16).
Exact effect (no arithmetic):
```
DMEM[0x120] = w0 (whole word);  DMEM[0x124] = w1 (whole word)
halfword at 0x116 (= geometry mode bits 15..0) := (that halfword & 0xfffd) | ((w0 & 1) << 1)     // G_TEXTURE_ENABLE (bit 1) := w0 bit 0
```
Nothing is derived. Consumers: triangle code uses the byte at 0x122 = w0 bits 15..8 = `(level << 3) | tile` and ORs it (sign-extended) with the area-sign bit (0x80) into byte 1 of the RDP triangle command (bit 7 = left-major flag from area sign;
a w0 bit 15 would also set it, `lb` sign extension) (0xa9c..0xac0); the vertex code multiplies the vertex s,t by `LLV` of 0x124 (lanes 0,1 for the first and lanes 4,5 for the second vertex of the loop iteration; 0x3c8/0x3d0, then `VMUDM v18,v18,v17[0]` 0x410).
Bits 31..1 of w0 do not matter to the handler (they are only stored, and bits 15..8 are consumed by the triangle code as above). The "on" bit also lives in 0x120 bit 0 (never read there). Edge: since bit 1 of geometry is shared, a later SET/CLEARGEOMETRYMODE of 0x2 toggles texturing without touching 0x120/0x124.
Verified on all six builds (`texture`).

## 5. G_MOVEWORD (0xbc)

w0 = op<<24 | offset<<8 | index (index = byte 7..0, offset = **16 bits 23..8**), w1 = data. Handler (read from DMEM `-5($27)` = index, `-7($27)` as halfword = offset):
```
base = sext16( halfword at DMEM[0x30e + index] );      // table of bases, index is a byte address (even for valid commands)
DMEM[(base + offset) & 0xfff .. +3] = w1;              // one 32-bit store of the whole w1, unaligned-capable; exit 0x09c
```
Valid indices (table values at 0x30e.., identical in all builds; 0.95 lacks idx 14):

| idx | libultra G_MW_ | base | what the stored word means / who reads it |
|---|---|---|---|
| 0x00 | MATRIX | 0x460 | word `offset` of the MVP cache (offset = 4*word, 0..0x3c; fractions from 0x20): modifies the matrix in place (gSPInsertMatrix). Nothing else. |
| 0x02 | NUMLIGHT | 0x12c | `w1 = 0x80000000 + (n+1)*32` from libultra (NUML); the microcode stores it verbatim: bit 31 = dirty ("recompute light directions", cleared by the lighting overlay at its next use), bits 11..0 = byte count of the light loop: loop `$1 = (n+1)*32, ... , 32` visits ambient (slot n) then lights n-1..0; **light record k is at 0x1f0 + 0x20*k, ambient at 0x1f0 + 0x20*n**. Max n = 7 (records up to 0x2d0). |
| 0x04 | CLIP | 0x70 | offset 4 / 0xc / 0x14 / 0x1c = RNX / RNY / RPX / RPY (libultra: ratio, ratio, -ratio, -ratio) -> words 0x74 / 0x7c / 0x84 / 0x8c. The vertex code reads **only the low half of 0x74 (0x76)** (as the scale of w for both axes); the clip overlay reads all four (via `LDV 14($8)`, `$8 = 4*plane`). |
| 0x06 | SEGMENT | 0x160 | offset = 4 * segment number (0..15): `segtab[seg] = w1`, **stored raw, no masking** (libultra passes the physical address). See the translate formula below. |
| 0x08 | FOG | 0x390 | w1 = (fog multiplier s16 << 16) | fog offset s16; offset 0 only is meaningful (0x394 holds the constant 0x00ff clamp, a nonzero offset would overwrite constants). |
| 0x0a | LIGHTCOL | 0x1f0 | (libultra offsets, inferred) offset = 0x20*n (+0 colour word, +4 colour-copy word) of light n: writes the RGBA words of record n directly (without MOVEMEM). Does **not** set the dirty bit (colours do not need it). |
| 0x0c | FORCEMTX | 0x4a0 | dummy base (= vertex slot 0): w1 (0x10000 from libultra) lands in the first word of vertex 0 and has no consumer. In 0.95 (no idx 14) idx 14 would index the vertex-address table the same way. |
| 0x0e | PERSPNORM | 0x110 | w1 (u16 in the low half) is the perspective normalisation factor; the **word** at 0x110 is written, the vertex/triangle code reads the low half 0x112. 1.21 / NoN / 1.23 only. In 0.95 it is its own command (b4: `halfword 0x112 := w1 & 0xffff`). |
| other | | table garbage | the index byte is not range-checked: any byte value reads a table entry from 0x30e + idx (0x31e.. is the vertex table, higher = other DMEM) and stores there. |

Segment translation (`jal 0x10c`, also used by the class-0 DMA prologue, DL, BRANCH_Z, MTX, VTX ... before their DMA): input `$19 = a` (an address word from a command):
```
$19 = segtab[(a >> 24) & 15] + (a & 0x00ffffff)        // 32-bit add, segtab = 16 words at 0x160 (bits 31..28 of a ignored)
```
0.95 computes `(a & DMEM_word[0xb8])` for the offset part (that word is 0x00ffffff); all builds also leave the segment base in `$12`. Verified (`segment`, GPR `$19`/`$12` compared).
Edge cases: segment 0 is not special (a stale/zero entry means address = offset, i.e. physical); no overflow check; unaligned results are passed on to the DMA unit.
Verified on all six builds (`moveword`; random index incl. invalid and odd values, random offset incl. 16-bit wrap).

## 6. G_MOVEMEM (class 0, opcode 0x03) and the DMA data

w0 = 0x03 << 24 | index << 16 | length (length = 10 bits used = DMA RD_LEN value; libultra gDma1p: sizeof of the struct), w1 = RDRAM address (segment-translated by the loop). Before the handler: DMEM[0xae0..] = first `((length|7)+1)` bytes at the address (24 bytes for a 16-byte struct, so bytes 16..23 are DMA'd but not copied). The handler (0x6d0):
```
reg = 16 bytes at DMEM[0xae0] (LQV);  dst = sext16(halfword at DMEM[0x270 + index]);  store reg bytes to dst .. next 16-byte boundary (SQV)
```
i.e. a **raw copy of the first 16 bytes (or less for an unaligned destination) of the DMA data**, no byte-swap/scale/flag. Exit 0x09c. Stale bytes of an earlier class-0 DMA are copied when `length` < 16.

| index (w0 bits 23..16) | libultra | dst | bytes stored | struct |
|---|---|---|---|---|
| 0x80 | G_MV_VIEWPORT | 0x380 | 16 | Vp: s16 vscale[4] (x, y, z, 0), s16 vtrans[4] — **stored unmodified**. The sign flip is applied by the vertex prep each time (0x660..0x684: `v0 = vscale * (1,-1,1,1)` using the constant row at DMEM 0x50, `VMUDH`; vtrans is used as is) |
| 0x82 | G_MV_LOOKATY | 0x1b0 | 16 | LookAt/Light record |
| 0x84 | G_MV_LOOKATX | 0x1d0 | 16 | same |
| 0x86 + 2k, k=0..7 | G_MV_L0..L7 | 0x1f0 + 0x20*k | 16 | Light: u8 col[3], pad, u8 colc[3], pad, s8 dir[3], pad, 4 pad bytes |
| 0x96 | G_MV_TXTATT | 0x138 | **8** (destination not 16-aligned: SQV stops at 0x140) | no reader |
| 0x98, 0x9a, 0x9c, 0x9e | G_MV_MATRIX_2,3,4,1 | 0x470, 0x480, 0x490, 0x460 | 16 each | the four 16-byte quarters of an `Mtx` (ints then fractions) = the MVP cache; gSPForceMatrix issues four of these then MOVEWORD idx 0x0c |

Recomputation: **none set by MOVEMEM.** The lighting overlay recomputes the model-space directions of all records (Y, X lookat and every light, `$1 = 0, 32, ..., (n+1)*32`; input dir at record + 8, output 8 bytes (dir, pad, dir, pad) at record + 0x10; transform by the matrix at 0x3e0 — read from the overlay, inferred) only when 0x12c bit 31 is set; that bit is set by MOVEWORD NUMLIGHT, MTX (0x558) and POPMTX (0x204). A game changing a light with MOVEMEM alone after the first use keeps the old derived direction until NUMLIGHT/MTX/POPMTX (colour reads go to the raw record at once).
Unknown index: dst = halfword at 0x270+index (0 for most) -> 16 bytes written at 0x000 (the DMA table) etc.
Verified on all six builds (`movemem`; valid and random indices; the 8-byte TXTATT case and the mutation "always 16 bytes" are caught).

## 7. G_SETOTHERMODE_L (0xb9) / G_SETOTHERMODE_H (0xba)

w0 = op<<24 | shift<<8 | length (**length, not length-1**; shift = bits 15..8, length = bits 7..0, both used mod 32 by the shifts), w1 = data. Target word `W` = DMEM 0x11c (L) or 0x118 (H):
```
mask = ((1 << (length & 31)) - 1) << (shift & 31)        // 32-bit, wraps; length&31 == 0 -> mask 0 (pure OR); shifts beyond 31 drop bits
W    = w1 | (W & ~mask)                                   // w1 is NOT masked: its bits outside the field are ORed in
then emit RDP command: DMEM[$23 .. $23+3] = DMEM[0x118] (new value if H), DMEM[$23+4 .. +7] = DMEM[0x11c]   (big-endian words)
```
The emitted 8 bytes are the RDP SET_OTHER_MODES command: first word = DMEM 0x118 (high byte is the RDP opcode, initially 0xef; initial word 0xef080cff), second word = DMEM 0x11c; **both words are emitted every time**, for L as well as for H, and also if nothing changed.
Fields (libultra G_MDSFT_*, positions = RDP bit positions of the respective word): H: alpha dither 4, rgb dither 6, combkey 8, text conv 9, text filt 12, text lut 14, text lod 16, text detail 17, text persp 19, cycle type 20, pipeline 23; L: alpha compare 0, z source 2, render mode 3 (29 bits), blender 16 (these come from libultra, not from the microcode). A field with shift+length > 24 in H clobbers the opcode byte 0xef.
After the stores the stretch continues at the RDP-command tail (`j 0x374`: `jal 0x128` flush, see rdp doc): `$23 += 8` happens in the delay slot of the flush call (`$23` is reset to 0xce0 by the flush). The *next* pc is **0x09c if `$2 > 0` (signed) else 0x0ac**, with `$2 = old W & ~mask` (the leftover of the field arithmetic): the yield check is skipped when that value is <= 0. (For RDPHALF_2 `$2` = the handler address from the table, positive: always 0x09c; for class 3 RDP commands `$2 = opcode+3 (+24 for 0xfd..0xff)`.)
F3DLX (1.23 and 0.95) tails differ: the tail begins with a DMA-idle wait (`jal 0xfc4`) and `mtc0 DPC_END, DMEM[0x128]` (restart the RDP at the pointer of the previous flush) **before** the two stores, then the flush (`jal 0x120` / 0x12c) and again DMA-idle wait + `DPC_END := DMEM[0x128]` after. That first `mtc0` is inside the verified stretch (spec calls `armlet_mtc0(e, 9, DMEM[0x128])` for F3DLX).
Edge: DMEM 0x118/0x11c are the only state; the triangle/vertex code never reads them.
Verified (`somL`, `somH`, all six builds, stretch ends at the flush call: banjo/NoN 0x37c, ts2_dex 0x374, ts2_dlx 0x378, mk64_dex 0x404, mk64_dlx 0x408; random shift/length incl. >31, random w1, random DMEM words, output pointer 0xce0..0xed0).

## 8. Other commands of class 0 / 2

* **G_SPNOOP (0x00)** and every unassigned opcode: handler 0x09c (class 1 = 0x40..0x7f too). Class 0 still issues the DMA first (harmless).
* **0xb2**: 1.21, NoN, 1.23 (dex and dlx): **G_MODIFYVTX**. w0 = op<<24 | where<<16 | (vtx*2), w1 = value: `DMEM[(where + vaddr) & 0xfff] = w1` (32-bit store), `vaddr` = the halfword at `0x31e + w0` (the full w0 is the base register: `(0x31e + w0) & 0xfff`, so the low 12 bits of w0 = 2*vertex index select the entry of the vertex-address table; with vertex < 32 the entry is 0x4a0 + 0x28*vtx). libultra offsets: where 0x10 = RGBA, 0x14 = ST, 0x18 = XY screen, 0x1c = Z screen. No clip-flag recomputation, no marking of the vertex. Exit 0x09c. Verified (`modvtx`) on banjo, NoN, ts2_dex, ts2_dlx; 0.95 has no such handler: **0xb2 = RDPHALF_2** there.
* **G_RDPHALF_1** (1.2x: 0xb4; 0.95: 0xb3): `DMEM[0x10c] = w1`; exit **0x0ac** (no yield check). Consumers of 0x10c: RDPHALF_2, BRANCH_Z (0x308 `lw $24,-4($29)` = branch target), LOAD_UCODE (data address).
* **G_RDPHALF_2** (1.2x: 0xb3; 0.95: 0xb2): emits an RDP command (first word `DMEM[0x10c]`, second word `w1`; no segment translation) via the same tail as SET_OTHERMODE (the rdp doc owns this); next pc 0x09c. Verified (`rdphalf1`, `rdphalf2`, as far as the flush call).
* **G_PERSPNORM (0.95 only, 0xb4)**: `halfword DMEM[0x112] = w1 & 0xffff`; exit 0x09c (verified `perspnorm`). In 1.2x it is MOVEWORD idx 14.
* **G_LOAD_UCODE (0xaf; class-0 alias 0x0a), 1.2x only, banjo 0xf8c** (both F3DLX builds from 0xf80, where a DMA-idle wait and `DPC_END := DMEM[0x128]` precede the same sequence). Read, not verified (DMA): it loads a new microcode and jumps into it:
  `DMEM[0xfd0] = w1` (OSTask ucode), `DMEM[0xff0] = $26 + $27 - $28` (RDRAM address of the next command: the display-list continuation), DMA 0xf18 bytes from RDRAM `w1` (physical, **no segment translation**) to IMEM 0x080, then DMA `(w0 & 0xffff)` as RD_LEN value from RDRAM `DMEM[0x10c]` (RDPHALF_1) to DMEM 0, then `$1 = 0xfc0` and jump to IMEM 0x080 after waiting for the DMA. An HLE should decline/treat as end of task.
* Not state commands, listed so that nothing falls through: b0 BRANCH_Z, b1 TRI2, b8 ENDDL, bd POPMTX, be CULLDL, bf TRI1, class 0: 01 MTX, 04 VTX, 06 DL.

## 9. Build differences (summary)

* Handler code is identical in all builds (pcs shift: section 1). NoN: only a constant at 0x2c0 (CULLDL) differs (not mine).
* 0.95: PERSPNORM is a command, no MODIFYVTX/BRANCH_Z/LOAD_UCODE, QUAD at b5; class tables at 0xbc / 0xc4; the vertex-address table is at 0x31c; DL stack depth byte at 0x110 (instead of 0x148); segment translation masks with the DMEM word 0xb8; MOVEWORD idx 14 does not exist.
* F3DLX (both): initial geometry word 0x00800000 (G_CLIPPING set), bit 23 gates the vertex clip flags and the triangle clip test; RDP-command tail begins with DPC_END := DMEM[0x128]; LOAD_UCODE starts with the same.
* Initial DMEM: identical for 0x100..0x13f except 0x114 above; 0.95 `0xb8 = 0x00ffffff`.

## 10. Verification

Harness: `SPEC_ONLY=state tools/native-f3dex/build_rt_test.sh x86 <out>`; `rt_test --ucode kit/uc/<build> --routine spec.f3dex.state --cases 20000` passed with 0 differences for **every** build (banjo121, sf64_non122, ts2_f3dex123, ts2_f3dlx123, mk64_f3dex95, mk64_f3dlx95) and every routine registered (11 per build: setgeom, cleargeom, texture, moveword, somL, somH, modvtx (not 0.95) / perspnorm (0.95 only), rdphalf1, rdphalf2, movemem, segment). DMEM (all 4 KiB), exit pc and (for `segment`) `$12/$19`, plus the DPC registers (F3DLX DPC_END write) are compared. Scenario generators build the dispatch's entry state (section 0), use the real data tables as DMEM base, random operands including invalid ones.
Mutation checks (caught): SOM length mask `&31`->`&15`; MOVEMEM always 16 bytes; MODIFYVTX base uses only the low byte of w0; segment index mask; TEXTURE on-bit position. Not caught (irrelevant for real data): sign extension of the MOVEWORD/MOVEMEM table entries.
Not verified (read only): all consumer lists in sections 0/3/4/6 (which code reads which DMEM word), the meaning of libultra field names, the lighting-record layout (inferred from the lighting overlay's address arithmetic: raw record 16 bytes + derived 16 bytes per 0x20 slot), LOAD_UCODE (DMA), the flush routine behind the RDP-command tail (0x128: rdp doc), the TXTATT consumer (none found).

Open questions: (1) who reads 0x138 (TXTATT) — none found statically; (2) the three non-RNX clip words and the lookat-Y derived vector at 0x1a0 are only reached by overlay code with computed bases (not enumerated per case); (3) the exact RDP-visible meaning of initial other-mode 0xef080cff (inferred: persp texture on, bilerp bits set, dithers off); (4) which model-space matrix (0x3e0) the light recompute uses is inferred from the call set-up `$8 = 992` in the overlay.
