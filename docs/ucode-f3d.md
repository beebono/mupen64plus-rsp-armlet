# Fast3D (F3D) RSP microcode: semantics for the task-level native path

Status: written for the F3D front-end of the task-level native graphics path (`src/hle/f3d.c`, per-build tables). Reference microcode: **Fast3D 2.0D, Super Mario 64 build, ucode id 94c7cfdd2c872523**
(FNV-1a-64 of the text as `gfx_ucode.c` logs it). Section 10 gives the other builds (Pilotwings 64, Wave Race 64, the 2.0H builds) as parameter tables and the semantic differences.

Every statement is tagged where the tag matters: **[verified]** = confirmed by running the real microcode on the fork's JIT (random differential tests against a C model, or the traces of 11), **[read]** =
from the disassembly only. Where a reference implementation exists it is named: `src/native/f3d_vtx.c`, `f3d_tri.c`, `f3d_vu.h` (op-for-op routines, JIT-exact, local reference only) and the models cited in the
sections. The spec is the maths and the layout; where the text and a reference disagree, the JIT is the authority.

Conventions: addresses are hex; `DMEM[a]` is the big-endian byte address; `D8/D16/D32` big-endian DMEM accesses (unaligned allowed, addresses wrap modulo 0x1000); `rd32` reads RDRAM;
`s16/u16/s32` are signed / unsigned integers; `op = w0 >> 24`; `seg(x)` is the segment resolution of 2.4. A *16.16 pair* `(hi : lo)` is the value `hi * 65536 + lo` with `hi` an `s16` and `lo` a `u16`.

Contents: 1 arithmetic of the vector unit; 2 task, command loop, dispatch; 3 the commands (all but G_VTX / G_TRI1); 4 RDP output (flush, FIFO); 5 DMEM layout; 6 G_VTX; 7 lighting and texture
generation, relight; 8 G_TRI1; 9 clipping; 10 per-build differences; 11 DMEM at the end of a task (leftover ranges, live-in ranges, poison results); 12 notes for the shared library and the front-end; 13 evidence and files; 14 the struct-level functions (src/hle/f3d_fx.h): tests and cost.


## 1. Notation and the arithmetic of the vector unit

All quantities are integers; `s16`, `u16` are 16-bit signed / unsigned, `s32` signed 32-bit. A *16.16 value* is a pair `(hi, lo)` with `hi` an `s16` and `lo` a `u16`
standing for the integer `hi * 65536 + lo` (so `lo` is always the unsigned fraction; the value is exact in an `s32` when the pair is consistent). Matrices are 4x4 arrays of
such values stored as 16 integer halfwords followed by the 16 fraction halfwords.

The microcode computes with the RSP's 48-bit accumulator `acc` (signed, wraps modulo 2^48) fed by 16 x 16 products and read through two clamps:

    S16(acc) = clamp_s16(acc >> 16)                                   (arithmetic shift; the "high" word)
    U16(acc) = acc & 0xffff   if  -2^31 <= acc < 2^31,  else 0xffff if acc > 0, else 0     (the "low" word)

The four product forms (a, b are 16-bit; the product is added to `acc`):

    H(a, b) = (s16(a) * s16(b)) << 16        M(a, b) = s16(a) * u16(b)        N(a, b) = u16(a) * s16(b)        L(a, b) = (u16(a) * u16(b)) >> 16

A multiplication of a 32-bit value by a 32-bit value `A = (ah:al)`, `B = (bh:bl)` is the chain `L(al,bl) + M(ah,bl) + N(al,bh) + H(ah,bh)`, which equals
`floor(A * B / 65536)` exactly (A, B as signed 32-bit integers). **The one rule that is easy to miss:** whenever a result's low word is read from a chain
before the last product is added (the microcode reads it with a `VMADN` that precedes the final `VMADH`), `U16` is applied to the sum *without the
`H(ah,bh)` term*: `lo = U16(acc - H(ah,bh))`, `hi = S16(acc)`. It matters only when the sum leaves the 32-bit range between the two reads (matrices with huge entries, W near
zero), and the reference implementation reproduces it everywhere ("lo3" in the code).

`VMULF(a, b) = clamp_s16((2 * s16(a) * s16(b) + 0x8000) >> 16)`. Saturating 16-bit add `sadd`. `rcp32(x)` is the RSP's `VRCP` on a 32-bit input (below).

### 1.1 rcp32 and the reciprocal with one Newton step

    rcp32(x):   m = x >> 31;  d = (x ^ m) - (x > -32768 ? m : 0)            (|x|, with -32768 special)
                if d == 0: return 0x7fffffff;  if x == -32768: return 0xffff0000
                sh = clz32(d);  i = ((d << sh) & 0x7fc00000) >> 22;  r = ((0x10000 | ROM[i]) << 14) >> (31 - sh);  return r ^ m

`ROM` is the RSP's 512-entry reciprocal table (parallel-rsp `rsp_reciprocal_rom`). For a 32-bit `W = (wh:wl)` the microcode's refined reciprocal `R = (rh:rl)` is

    R1 = rcp32(W);  R2 = (2 * R1 saturated to s32, halves saturated separately: hi 0x7fff / 0x8000, lo 0xffff / 0)
    P  = floor(R2 * W / 65536);   ph = S16(P);   pl = U16(P - H(r2h, wh))
    N  = (nh:nl):  nl = (-pl) & 0xffff;  nh = clamp_s16(2 - ph - (pl != 0))
    Q  = floor(R2 * N / 65536);   rh = S16(Q);   rl = U16(Q - H(r2h, nh))

(`R` is about 2^32 / W; the pair is used as a 16.16 value.)

## 2. Command fetch loop and dispatch

### 2.1 Registers / state of the loop (constant meaning across handlers)

| reg | meaning |
|---|---|
| sp = 0x110 | base of the state block (section 5) |
| k0 | RDRAM address of the next command not yet consumed (DL pointer). +8 per command |
| k1 | DMEM pointer to the command being fetched, in the command buffer 0x6a0..0x7df |
| gp | bytes left in the command buffer; -8 per command; refill when <= 0 |
| s7 | DMEM write pointer into the RDP output buffer (starts at 0x9e0) |
| s6 | 0x7e0 = geometry/DMA buffer (class-0 commands) |
| t9, t8 | w0, w1 of the current command |
| v30 / v31 | vector constants loaded from DMEM 0x40 / 0x30 at start (section 5) |

### 2.2 Loop, refill, end of list [verified unless noted]

```
loop (IMEM 0x60):  w0 = D32[k1]; w1 = D32[k1+4]; cls2 = (w0>>29)&6   (= 2*(op>>6));  k0 += 8; k1 += 8; gp -= 8
  if cls2 == 0 (op < 0x40): len = (w0 & 0x1ff)+1;  DMA read  DMEM[0x7e0 .. +len'] <- RDRAM[seg(w1) & 0xffffff & ~7]
        (len' = len rounded UP to a multiple of 8; DMA is only started here, handlers wait for it, see below)
  jump to H = D16[0xbc + cls2]  (class table, 1.3)
command end (IMEM 0xa8):  if SP_STATUS bit7 (SIG0, CPU yield request): yield (2.5)
                          else if gp > 0: goto loop (0x60)       [0xb8 is the same without the yield test]
                          else refill: gp = 320; DMA read DMEM[0x6a0..0x7e0) <- RDRAM[k0] (319+1 = 320 bytes); k1 = 0x6a0; goto 0x58
                                (0x58 waits for DMA idle (SP_DMA_BUSY==0) and falls into the loop)
```
- Command buffer = 320 bytes = 40 commands, always refilled in full from the current DL pointer k0 when gp reaches 0 (also after every G_DL /
  G_ENDDL / G_CULLDL-end, which set gp = 0). Reads beyond the end of a display list are harmless (the list ends with G_ENDDL).
- Command size is always 8 bytes; k0 advances 8 per command whatever the opcode (including no-ops).
- The "does the yield test run" detail: handlers end at 0xa8 (yield test) or 0xb8 (no test). RDP pass-through ops 0xc0..0xe5 end at 0xb8,
  0xe6..0xff at 0xa8 (v0 = op+27 > 0, signed); G_RDPHALF_2 (0xb3) ends at 0xb8; others at 0xa8 or, for the 0x3a8-emitting
  ones (SETOTHERMODE, 0xb1/0xb2), depending on stale register v0 (> 0 -> 0xa8). Only changes WHEN a yield request is noticed. [verified: 0xe1,0xe4 -> b8; 0xf1 -> a8]
- A class-0 handler is entered at 0x3c4 -> `v0 = D16[0xc4 + 2*op]`, waits for the geometry DMA to finish (SP_DMA_BUSY==0), then
  `at = (w0>>16)&0xff` (byte 1), `a2 = at & 15`, jumps. A class-2 handler is entered at 0x1fc -> `v0 = D16[0x76 + 2*(op-0x80)]`,
  `at = w1 & 0xff`, jump. Class 1 (op 0x40..0x7f): jump to 0xa8 directly, NO DMA. Class 3 (op 0xc0..0xff): jump to 0x38c, no DMA.
- Because the class-0 DMA is issued for every op < 0x40 (also no-ops such as G_SPNOOP), DMEM 0x7e0.. receives (len+1 rounded to 8) bytes
  from seg(w1) each time (observable in DMEM only). [read]

### 2.3 Dispatch tables (initial DMEM) [verified: jump targets read at the `jr` for all 256 opcodes]

First level, `D16[0xbc+2*cls]`: cls0 = 0x13c4 (-> IMEM 0x3c4), cls1 = 0x10a8 (-> 0xa8, no-op), cls2 = 0x11fc (-> 0x1fc), cls3 = 0x138c (-> 0x38c).
(Table entries hold IMEM+0x1000; jump = entry & 0xfff.)
Second level: class-0 table at 0xc4 + 2*op (op 0x00..0x3f); class-2 table at 0x76 + 2*(op-0x80). The two tables OVERLAP in DMEM
(class-0 entry for op x is the class-2 entry for op x+0xa7), and they overlap other constants (0x76..0xbb), which is why most of
0x08..0x3f and 0x80..0xaf are undefined.

### 2.4 Segment resolution seg(x) (IMEM 0x11c) [verified for 0xfd/0xfe/0xff and the segment table]

```
seg(x) = D32[0x160 + 4*((x>>24) & 0xf)]  +  (x & 0x00ffffff)        (32-bit add, NO mask afterwards)
```
bits 31..28 of x are ignored. Used for: w1 of class-0 commands (the geometry DMA source), G_DL w1, and the w1 of RDP SETTIMG(0xfd) /
SETZIMG(0xfe) / SETCIMG(0xff). The sum is emitted unmasked into the RDP command (e.g. 0x12345678 with seg2=0x123450 -> 0x468ac8). DMA
addresses are masked `& 0xffffff & ~7` by the DMA unit. G_POPMTX / the matrix stack addresses (DMEM 0x134) are raw RDRAM addresses (no seg).

### 2.5 Task start, yield, resume, task end [verified]

- Entry: IMEM 0x80 `j 0x780; sp = 0x110`. Registers on entry (set by the boot ucode): at = 0xfc0 (OSTask in DMEM). Init (IMEM 0x780..0x86c):
  1. SP_STATUS := 0x2800 (clear SIG1, SIG2). v31 = D128[0x30], v30 = D128[0x40].
  2. if OSTask.flags (D32[0xfc4]) & 1 (yielded task): jump to the resume path (below).
  3. FIFO: D32[0x150] = OSTask.output_buff (D32[0xfe8]), D32[0x154] = D32[0xfec] (treated as the buffer END address). Initial write pointer
     D32[0x128] = v1, chosen so: if DPC_STATUS.xbus (bit0) OR DPC_END < FIFOstart (i.e. `start - END > 0`) OR DPC_CURRENT == 0 OR
     DPC_CURRENT == DPC_END: wait until DPC_STATUS.start_valid (bit10) == 0, clear xbus (DPC_STATUS := 1), DPC_START := DPC_END := FIFO END;
     write ptr = FIFO END (so the first flush wraps to FIFO start). Otherwise (RDP still busy with an old list inside the buffer):
     write ptr = DPC_END (continue appending there). s7 = 0x9e0. [verified: 8 DPC register scenarios]
  4. Add `OSTask.ucode` (D32[0xfd0]) to the overlay descriptor DRAM addresses D32[8], [0x10], [0x18], [0x20] (they are ucode-relative);
     load overlay 0 (IMEM 0x000..0x87: reciprocal routine + loop head, descriptor at 0x08).
  5. k0 = OSTask.data_ptr (D32[0xff0]); initial 320-byte command fetch (as refill, descriptor-less); k1 = 0x6a0, gp = 320.
  6. matrix stack: D32[0x130] = D32[0x134] = OSTask.dram_stack (D32[0xfe0]); D32[0x15c] = that + 640 (limit, 10 entries of 64 bytes).
  7. D32[0x108] = D32[0xff8] (yield_data_ptr). Then `j 0x58`.
- Yield: at 0xa8, if SP_STATUS bit7 (SIG0, set by the CPU) is 1: load overlay 3 (IMEM 0x768, from descriptor D[0x20]), run it: stores gp,k1,k0,s7
  to D32[0x8e4], [0x8e8], [0x8ec], [0x8f0]; DMA-writes DMEM[0..0x8ff] (0x900 bytes) to RDRAM[D32[0x108]]; SP_STATUS := 0x1000 (set SIG1 =
  "yielded"), then falls into the task-end path (SP_STATUS := 0x4000 = set SIG2, `break`). Final SP_STATUS = 0x383 (halt, broke, SIG0|SIG1|SIG2).
  The pending RDP data is already flushed (every command is flushed individually, 3.2); the DL position is exactly "next command" (k0, k1, gp).
- Resume (OSTask.flags bit0 = 1, DMEM[0..0x8ff] already restored by the OS from the yield buffer): reload overlay 0, s7/gp/k1/k0 from
  0x8f0/0x8e4/0x8e8/0x8ec, `j 0xa8`. [verified: yield after 3 of 8 commands + resume reproduces the exact remaining RDP stream]
- Task end: G_ENDDL with an empty DL stack (or an effective ENDDL from CULLDL): load overlay 3, SP_STATUS := 0x4000 (set SIG2), `break`.
  Final SP_STATUS = 0x203 (halt, broke, SIG2). There is no other end condition (no length count). [verified]
- No G_LOAD_UCODE, no G_BRANCH_Z in this build. DPC_END was written by the last flush; no extra RDP command is added at task end.

### 2.6 Complete opcode table (op = w0>>24)

| op | name | action |
|---|---|---|
| 00 | G_SPNOOP | no-op (geometry DMA issued) [verified] |
| 01 | G_MTX | 2.1 |
| 02 | (reserved) | no-op |
| 03 | G_MOVEMEM | 2.3 |
| 04 | G_VTX | (vertex spec) |
| 05 | (reserved) | no-op |
| 06 | G_DL | 2.8 |
| 07, 08, 09 | reserved / G_SPRITE2D_BASE | no-op (geometry DMA issued) [verified] |
| 0a..18 | alias of 0xb1..0xbf | the class-0 table runs into the class-2 table: op 0x0a+k executes handler of op 0xb1+k, after a geometry DMA (so 0x18 = G_TRI1 etc.). Never emitted by games; implement as unsupported [verified targets] |
| 19..3f | undefined | jump to IMEM addresses taken from data bytes (0x17bc,0x17c0,0x1818,0x182c,0x1964,0x1784,0x17a0 for 19..1f; 0x1058 for 20 = restarts the loop and reuses k1=0x6a0; 21.. = 0x0000/0x0029/0xffff/...): crash |
| 40..7f | (class 1) | no-op, no DMA, no side effect [verified] |
| 80..a2 | undefined | jump to the data halfword at 0x76+2*(op-0x80) (0x0001,0x0000,0xffff,0x1774,0x7fff,... ): crash |
| a3..af | undefined/alias | table values equal the class-0 entries: a4,a7,a9,ac,ae,af = no-op; a8,aa,ab,ad jump into the MTX / MOVEMEM / VTX / DL handler without the DMA and parameter setup; a3,a5,a6 re-enter a dispatcher: do not use [verified targets] |
| b0 | (G_BRANCH_Z) | no-op in this build [verified] |
| b1 | G_RDPHALF (emit, no yield test) | emit RDP command (D32[0x10c], w1), 3.9 |
| b2 | G_RDPHALF_CONT (emit) | same, ends with yield test |
| b3 | G_RDPHALF (store) | D32[0x10c] := w1 (no yield test) |
| b4 | G_PERSPNORMALIZE | D16[0x112] := w1 & 0xffff |
| b5 | (G_LINE3D) | no-op in SM64 (WR: quad/line command) |
| b6 | G_CLEARGEOMETRYMODE | D32[0x114] &= ~w1 |
| b7 | G_SETGEOMETRYMODE | D32[0x114] \|= w1 |
| b8 | G_ENDDL | 2.8 |
| b9 | G_SETOTHERMODE_L | 2.6 |
| ba | G_SETOTHERMODE_H | 2.6 |
| bb | G_TEXTURE | 2.5 |
| bc | G_MOVEWORD | 2.4 |
| bd | G_POPMTX | 2.2 |
| be | G_CULLDL | 2.7 |
| bf | G_TRI1 | (triangle spec) |
| c0..fc | RDP commands | copied verbatim, 3.1 (c0..e5: no yield test) |
| fd, fe, ff | SETTIMG, SETZIMG, SETCIMG | copied with w1 := seg(w1) |

Names for 0xb1..0xb5 are by function (the gbi names RDPHALF_1/2/CONT / PERSPNORMALIZE may be assigned differently); the behaviour is what is specified.

## 3. Commands (all verified on the JIT unless tagged [read])

### 3.1 G_MTX (op 0x01): `w0 = 01 PP 00 LL`, w1 = matrix address (segmented)

PP = (w0>>16)&0xff: bit0 PROJECTION (1) / MODELVIEW (0); bit1 LOAD (1) / MUL (0); bit2 PUSH (1). Other bits ignored.
LL: DMA length = (w0&0x1ff)+1 rounded up to 8, normally LL=0x40 -> 72 bytes read (65 -> 72); only the first 64 are used.
Matrix format (RDRAM and DMEM): 64 bytes = integer part 16 x s16 (row-major, row r col c at +8r+2c), then fraction part 16 x u16 (+0x20).
Value = I + F/65536 (s15.16). N = matrix read to DMEM 0x7e0.

```
D8[0x12c] := 0xff                                  (top byte of the light-count word: lights stale, see 3.4)  [any G_MTX incl. projection]
if PP&1:  T = 0x3a0 (projection)            ; PUSH is ignored
else:     T = 0x360 (modelview)
          if PP&4 and D32[0x134] != D32[0x15c]:            ; push, skipped (silently) when the stack is full
               DMA write  RDRAM[D32[0x134] .. +len']  <-  DMEM[0x360 ..]        ; the OLD modelview, len' = (LL+1)->8 = 72 bytes for LL=0x40
               D32[0x134] += 64                               ; (the 8 extra bytes are DMEM 0x3a0..0x3a7, they spill into the next slot)
if PP&2:  DMEM[T..T+64) := N                                  ; load
else:     DMEM[T..T+64) := N (x) DMEM[T]                      ; multiply, N applied first (row-vector convention), result in place
DMEM[0x3e0..0x420) := DMEM[0x360] (x) DMEM[0x3a0]            ; MP = modelview (x) projection, ALWAYS recomputed (also when push was skipped)
```
Scratch side effect: in the MUL case the product N(x)T is also left at DMEM 0xde0 (int rows 0..3 at 0xde0..0xdff, fraction at 0xe00..0xe1f).
Matrix product A (x) B (A = first operand), for i,j,k = 0..3, with u32/s64 arithmetic:
```
acc_ij = sum_k [ (Fb_kj*Fa_ik) >> 16  +  Ib_kj*Fa_ik  +  Fb_kj*Ia_ik  +  ((Ib_kj*Ia_ik) << 16) ]      (Fa,Fb: u16; Ia,Ib: s16; the first term is an unsigned product, floor-shifted)
acc wraps modulo 2^48 (two's complement);  H = acc >> 16 (arithmetic, 32-bit value of acc[47:16])
C.I_ij = clamp_s16(H);   C.F_ij = (-32768 <= H <= 32767) ? (acc & 0xffff) : (H < 0 ? 0x0000 : 0xffff)
```
[verified bit-exact: 3000+1500 random cases over small/huge/identity-like matrices, all 8 PP values, LL = 0x3f/0x40, stack full/not full; mutation (+1 on one term) fails 125/200]
G_MTX never touches D32[0x12c] other than its top byte. DL stack pointer (matrix) = D32[0x134], base D32[0x130], limit D32[0x15c] = base+640.

### 3.2 G_POPMTX (op 0xbd): fields unused

```
cur = D32[0x134];  if cur > D32[0x15c] - 640 (i.e. stack not empty):
     DMEM[0x360..0x3a0) := RDRAM[cur-64 .. cur)      (exactly 64 bytes, raw address, no segment);  D32[0x134] := cur - 64
     DMEM[0x3e0..0x420) := DMEM[0x360] (x) DMEM[0x3a0]                  (same product as above)
else: nothing (no MP recompute)
```
Does NOT touch D32[0x12c] (lights stay "fresh" although the modelview changed; WR's build sets the stale flag, 5). [verified incl. flag unchanged, 1000 cases]

### 3.3 G_MOVEMEM (op 0x03): `w0 = 03 II 00 LL`, w1 = source (segmented); II = (w0>>16)&0xff

DMA (LL+1 -> rounded to 8) from seg(w1) to 0x7e0, then a raw 16-byte copy `DMEM[dest(II)..] := DMEM[0x7e0..0x7f0)` (LQV/SQV, so a destination
that is not 16-byte aligned receives only the bytes up to the next 16-byte boundary). dest(II) = D16[0x2f0 + (II-0x80)] (halfword table, II even):

| II | dest | what | II | dest | what |
|---|---|---|---|---|---|
| 80 | 0x320 | viewport (vscale s16[4] @0x320, vtrans s16[4] @0x328, raw) | 90 | 0x290 | light 6 slot |
| 82 | 0x1b0 | lookat Y slot | 92 | 0x2b0 | light 7 / ambient slot |
| 84 | 0x1d0 | lookat X slot | 94 | 0x2d0 | light 8 / ambient slot |
| 86 | 0x1f0 | light 1 (L0) slot | 96 | 0x138 | only 8 bytes (0x138..0x13f), no reader found |
| 88 | 0x210 | light 2 | 98 | 0x3f0 | MP int rows 2,3 (G_MV_MATRIX_2) |
| 8a | 0x230 | light 3 | 9a | 0x400 | MP frac rows 0,1 |
| 8c | 0x250 | light 4 | 9c | 0x410 | MP frac rows 2,3 |
| 8e | 0x270 | light 5 (overwrites the version string) | 9e | 0x3e0 | MP int rows 0,1 (G_MV_MATRIX_1) |
| a0 | 0x12c | 4 bytes only: the light-count word | | | |

No conversion happens: a Light_t (col[3],pad,colc[3],pad,dir[3],pad = 16 bytes) is stored as is at the slot base (slot stride 0x20; the second
half of each slot, base+0x10.., is written by the lighting overlay with the transformed direction). Indices outside this table read a
DMEM byte as table entry: undefined. The four MATRIX indices load the final MP matrix directly and do NOT recompute from MV/P and do not
flag lights. MOVEMEM does not set the "lights stale" flag. [verified: all indices, 4 LL values, byte ranges and contents]

### 3.4 G_MOVEWORD (op 0xbc): `w0 = bc OOOO II`: offset O = (w0>>8)&0xffff, index II = w0&0xff (even); value w1

`D32[ tab[II] + O ] := w1` (full 32-bit store, unaligned allowed), tab = D16[0x30e + II]: [verified]

| II | tab | meaning |
|---|---|---|
| 00 | 0x3e0 | G_MW_MATRIX: word into the MP matrix (O selects the word) |
| 02 | 0x12c | G_MW_NUMLIGHT: D32[0x12c] := w1 (normally NUML(n) = 0x80000000 + (n+1)*32) |
| 04 | 0x070 | G_MW_CLIP: clip ratio words, O = 4,0xc,0x14,0x1c (RNX,RNY,RPX,RPY) -> 0x74,0x7c,0x84,0x8c |
| 06 | 0x160 | G_MW_SEGMENT: seg table; O = 4*segment -> D32[0x160+4*seg] := w1 (stored raw, not masked) |
| 08 | 0x330 | G_MW_FOG: D16[0x330] = fm (w1>>16), D16[0x332] = fo (w1&0xffff) |
| 0a | 0x1f0 | G_MW_LIGHTCOL: O = 0x20*(n-1) (+4 for the second colour) -> light colour words at slot 0x1f0+... |
| 0c | 0x420 | G_MW_POINTS: vertex buffer word write, O = vtx*40 + {0x10 rgba, 0x14 st, 0x18 xy, 0x1c z} |
| 0e | 0x000 | G_MW_PERSPNORM index: stores at DMEM[O..], harmless (the real persp norm is op 0xb4) |
| 10 | 0x000 | same (table entry 0) |

The light-count word D32[0x12c] = V: bits 11..0 = 32*(n+1) for n directional lights (SM64 sends NUML(n) = 0x80000000 + 32*(n+1)); the top byte is a
"light directions stale" flag. Slot map: directional light i (0-based) at 0x1f0+32*i, ambient slot at 0x1f0+32*n, lookat X/Y at 0x1d0/0x1b0.
The lighting overlay (ov2, entered from G_VTX when G_LIGHTING is set) does `lw at,0x12c; bltz at` : if the flag is set it writes
D32[0x12c] := V & 0xfff, then for at = V, V-32, ... down to the first value <= 0 transforms the direction at 0x1b8+at by the modelview (rows from
0x360) into the second half of the slot (0x1c0+at), which covers all directional lights and the two lookat slots. [read]
Flag setters: MOVEWORD NUMLIGHT (any value with bit31) and every G_MTX (top byte := 0xff). Cleared by the overlay. NOT touched by POPMTX
(SM64, PW) nor MOVEMEM, so after a POPMTX the cached transformed directions are those of the pre-pop modelview. [verified: setters]

### 3.5 G_TEXTURE (op 0xbb): `w0 = bb 00 (level<<3|tile) on`: byte2 = 0 (bowtie), byte1 = level<<3 | tile, byte0 = on; w1 = (sscale<<16 | tscale)

`D32[0x120] := w0; D32[0x124] := w1; D32[0x114] := (D32[0x114] & ~2) | ((w0&1)<<1)` (geometry-mode bit1 = texture enable). No RDP output.
Other fields are consumed later: byte D8[0x122] = (w0>>8)&0xff (tile/level -> RDP triangle command byte 1), D8[0x121] = (w0>>16)&0xff, w1 =
S/T scale for vertex texture coordinates (vertex spec). [verified: 12 cases]

### 3.6 G_SETOTHERMODE_L (0xb9) / _H (0xba): `w0 = op SS LL` (shift SS = (w0>>8)&0xff, len LL = w0&0xff), value w1 (pre-shifted)

```
W = (op==0xb9) ? &D32[0x11c] (L) : &D32[0x118] (H);   m = ((1 << (LL&31)) - 1) << (SS&31)       (shift counts modulo 32; LL=32 -> m = 0)
*W := (*W & ~m) | w1                                   (w1 is NOT masked by m and NOT shifted)
emit RDP command  (D32[0x118], D32[0x11c])  = (H, L)  to the output buffer and flush (4.2)
```
Initial values: H = 0xef080cff (top byte 0xef = RDP SET_OTHER_MODES), L = 0 (note: +8 holds H, +12 holds L). Result: the two DMEM words are the two words of the RDP command. The pair is emitted after every SETOTHERMODE, either half. [verified: 3000 random incl. LL/SS up to 39]

### 3.7 G_CULLDL (op 0xbe): `w0 = be 00 AAAA` A = start vertex byte offset (vstart*40) & 0x3ff, w1 = end byte offset (vend*40), exclusive

```
a = w0 & 0x3ff;  v = 0xffff
do { v &= D16[0x444 + a];  a += 40; } while (a != w1)        (the AND is executed for the vertex at a, then a advances; so vertices a0, a0+40, .. w1-40)
if v != 0:  behave exactly like G_ENDDL (pop the DL stack, or end the task if the stack is empty)
else:       continue with the next command
```
The tested field is the 16-bit clip-flag halfword of each vertex (vertex base 0x420 + 40*i, halfword at +0x24 = 0x444+40i, written by G_VTX).
All 16 bits are ANDed (mask 0xffff in SM64; PW 0x7030, WR 0x7070): culled iff some flag bit is set in ALL vertices of [vstart, vend).
A range with vend == vstart does not terminate normally (counter runs past); not emitted by games. [verified: 3000 cases incl. exclusive end; PW mask verified]

### 3.8 G_DL (0x06) / G_ENDDL (0xb8)

G_DL: `w0 = 06 PP 0000`, w1 = list address (segmented). The class-0 DMA (also executed) is irrelevant.
```
if PP == 0 (call):   d = (int8)D8[0x110];  if d > 36: command ignored entirely (no branch)  [10 nested calls max]
                     D32[0x336 + d] := k0  (k0 already = address of the NEXT command in the caller; unaligned store, overlaps D16[0x336])
                     D8[0x110] := d + 4
k0 := seg(w1);  gp := 0 (forces refill)           (PP != 0: branch, no push, the target's G_ENDDL returns to the caller of the caller)
```
G_ENDDL: `d = D8[0x110] - 4; if d < 0: task end (2.5) else { k0 := D32[0x336 + d]; D8[0x110] := d; gp := 0 }`.
Stack storage is D32[0x336..0x35e) (10 slots, unaligned words, 0x336 is the 4th halfword of the fog vector, harmless). [verified: nesting, branch, recursion depth 10, ret ptr value]

### 3.9 Remaining commands

- G_SETGEOMETRYMODE (0xb7): `D32[0x114] |= w1`; G_CLEARGEOMETRYMODE (0xb6): `D32[0x114] &= ~w1`. No RDP output. [verified]
  Geometry-mode bits (as consumed by the code): 0x00000001 ZBUFFER, 0x00000002 TEXTURE_ENABLE (also set by G_TEXTURE), 0x00000004 SHADE,
  0x00000200 SHADING_SMOOTH, 0x00001000 CULL_FRONT, 0x00002000 CULL_BACK, 0x00010000 FOG, 0x00020000 LIGHTING, 0x00040000 TEXTURE_GEN,
  0x00080000 TEXTURE_GEN_LINEAR [read]. The low byte (D8[0x117]) | 0xc8 forms the RDP triangle opcode (bits 0,1,2 only meaningful).
- G_PERSPNORMALIZE (0xb4): `D16[0x112] := w1 & 0xffff` [verified]. Initial 0xffff.
- 0xb3: `D32[0x10c] := w1`. 0xb1 / 0xb2: emit RDP command (D32[0x10c], w1) via the output buffer (+flush). SM64 sends texture rectangles as raw RDP ops 0xe4,0xe1,0xf1 which take the pass-through path instead. [verified]
- G_SPNOOP, reserved ops, 0xb0, 0xb5, class 1: no-op. G_LOAD_UCODE, G_BRANCH_Z, G_LINE3D: not present.

## 4. RDP output mechanics

### 4.1 Pass-through (op 0xc0..0xff) [verified]

`out[s7] = (w0, seg-fixed w1)`, 8 bytes, s7 += 8, then flush (4.2). Only w1 of 0xfd/0xfe/0xff is rewritten (seg(w1)); w0 never changed. This
includes 0xe4/0xe1/0xf1 (texrect triple), 0xe7/e8/e9 syncs, 0xef (SET_OTHER_MODES typed by the DL itself - then H/L in DMEM are NOT updated), fill/fog/blend/prim/env colors,
loads, tiles, scissor, combine. Triangles 0xc8..0xcf in a DL would also be copied as single 8-byte units (not used).
G_SETOTHERMODE and 0xb1/0xb2 use the same emit path (0x3a8).

### 4.2 Output buffer and flush (IMEM 0x178) [verified: wrap, fits-exactly, per-command calls, triangle = 1 flush]

Output buffer: DMEM 0x9e0.., s7 = write pointer, reset to 0x9e0 after each flush. One command (8 bytes) or one RDP triangle (<= 0xb0 bytes, written by G_TRI1 starting at 0x9e0 with s7 advanced past it)
is flushed immediately, so the RDP sees one DPC_END update per command / triangle; there is no batching.
State: W = D32[0x128] (FIFO write pointer, RDRAM), S = D32[0x150] (FIFO start), E = D32[0x154] (FIFO end, exclusive, limit for W+n).

```
n = s7 - 0x9e0;  if n <= 0: return (s7 := 0x9e0)
if W + n > E:                       ; does not fit -> wrap
      wait until DPC_STATUS.start_valid (bit10) == 0
      wait until DPC_CURRENT != S        ; RDP has left the start of the buffer
      DPC_START := S;  W := S
loop: c = DPC_CURRENT;  if W >= c: ok  else if W + n < c: ok  else wait (re-read DPC_CURRENT)      ; never write over data the RDP has not consumed
DMA write RDRAM[W ..] <- DMEM[0x9e0 .. 0x9e0+n)      (length register n-1, n multiple of 8)   ; wait for DMA idle
W += n;  D32[0x128] := W;  DPC_END := W;  s7 := 0x9e0
```
FIFO full = the two waits above (busy loops on DPC_STATUS / DPC_CURRENT; the RSP simply stalls, nothing is dropped or signalled). Equality W + n == E fits (W may reach E, the next flush wraps). DPC_START is written ONLY on a wrap (and at init, 1.5); DPC_END after every flush.
The RDP sees list segments [DPC_START/prev END, END). With a host that executes the list synchronously at the DPC_END write (mupen64plus
ProcessRdpList) the waits never block. After wrap, data is written from S again; the video plugin must treat each END write as "process [prev_end, END)".
Initial W is E (or the old DPC_END when the RDP was busy, 1.5), so the very first flush wraps to S: DPC calls are (E,E) at init, then (S,S+8),(S+8,S+16),...
G_TRI1 hand-over: the triangle code leaves s7 > 0x9e0 and calls this routine once (`jal 0x178` at IMEM 0xfcc), then `jr s8` (0xa8). A shaded+zbuffered
triangle is 0x70 bytes (one DPC call (S,S+0x70)). Clipped polygons: each output triangle is its own flush (clip spec).

## 5. DMEM layout (SM64; initial = `.data`, 0x800 bytes; 0x800..0xfff zero except OSTask)

| addr | size | name / content | writer / reader |
|---|---|---|---|
| 0x000 | 8 | boot parameters: D32[4] = 0x0f5f1080 | boot code only |
| 0x008 | 0x20 | overlay descriptors ov0..ov3: (dram_addr, ((len-1)<<16)\|imem): ov0 0xf60 -> IMEM 0x000 len 0x88, ov1 (clip) 0xfe8 -> 0x768 len 0x220, ov2 (light) 0x1208 -> 0x768 len 0x198, ov3 (end) 0x13a0 -> 0x768 len 0x68. dram = ucode text offset, +OSTask.ucode at init | init writes; loader 0xf8/0xfc reads |
| 0x028 | 8 | consts for G_VTX: 0x0ffa f006 7fff 0000 (z clamp 0..0x7fff in elements 3,2) | vtx |
| 0x030 | 16 | v31: 0000 0001 0002 ffff 4000 0004 0633 0200 | all VU code |
| 0x040 | 16 | v30: 7fff fff8 0008 0040 0020 8000 01cc cccc | all VU code |
| 0x050 | 16 | 0001 ffff 0001 0001 0001 ffff 0001 0001 (signs for the vertex lighting helper) | vtx |
| 0x060 | 16 | 0002 x 8 (Newton-Raphson 2.0 for the reciprocal routine at IMEM 0) | recip |
| 0x070 | 0x30 | clip tables (ratio planes, consts): 0x70..0x8f clip-ratio planes (ratio words stored at 0x74,0x7c,0x84,0x8c by MOVEWORD 04), 0x90..0x9f consts | clip overlay |
| 0x0a0 | 0x18 | 0xa0: 0x1774 (ov2 entry, `lh s5,160`), 0xa2: 7fff 571d 3a0c, 0xa8: 0001 0002 0100 0200, 0xb0: 4000 0040, 0xb4: 0000 | lighting / clip |
| 0x0b6 | 2 | 0x1770: return address after loading ov3 (task end) | loop |
| 0x0b8 | 4 | 0x00ffffff segment offset mask | seg() |
| 0x0bc | 8 | first-level class table (2.3) | loop |
| 0x076 | 0x8e | second-level jump tables: class-2 (op 0x80..0xbf) at 0x76+2*(op-0x80), ops 0xb0..0xbf = 0xd6..0xf4; class-0 (op 0x00..0x3f) at 0xc4+2*op (the two overlap); 0xf6..0x103 = data halfwords (code addresses) | loop |
| 0x100 | 4 | 0x1784 (ov1 return after load via stub), 0x17a0 | overlays |
| 0x104 | 2 | 0x1058 = loop restart after refill (`lh ra,260`) | loop |
| 0x106 | 2 | saved return pc (G_VTX stores 0x10a8) | vtx |
| 0x108 | 4 | yield_data_ptr (RDRAM) | init; yield |
| 0x10c | 4 | RDPHALF word (op 0xb3) | 2.9 |
| 0x110 | 1 | DL stack depth (int8, bytes: 0,4,..,40) | DL/ENDDL |
| 0x112 | 2 | persp-normalize (initial 0xffff) | 0xb4; vtx/tri |
| 0x114 | 4 | geometry mode (initial 0) | 0xb6/0xb7/G_TEXTURE |
| 0x118 / 0x11c | 4+4 | other modes H (init 0xef080cff) / L (0) | 2.6 |
| 0x120 / 0x124 | 4+4 | G_TEXTURE w0 / w1 | 2.5; vtx, tri |
| 0x128 | 4 | RDP FIFO write pointer (RDRAM) | flush |
| 0x12c | 4 | light-count word (initial 0x80000040) | 2.4 |
| 0x130 / 0x134 | 4+4 | matrix stack base / current (RDRAM, 64-byte entries) | init; MTX/POPMTX |
| 0x138 | 8 | initial 0x40004000 0 (MOVEMEM 96 target) | - |
| 0x150 / 0x154 | 4+4 | FIFO start / end (OSTask.output_buff, output_buff_size field) | init |
| 0x15c | 4 | matrix stack limit = base+640 | init |
| 0x160 | 0x40 | segment table, 16 words (initial 0) | MOVEWORD 06 |
| 0x1a0 | 0x10 | texgen / lighting consts (0x80000000 0x80000000 0 0) | ov2 |
| 0x1b0 / 0x1d0 | 0x20 each | lookat Y / X slots (16 B raw from MOVEMEM 82/84, +0x10 transformed dir) | MOVEMEM; ov2 |
| 0x1f0 + 0x20*i | 0x20 | light slots i=0..: 0x00 colour(3)+pad, 0x04 colour copy, 0x08 dir(3), 0x10 transformed dir (SPV) | MOVEMEM/LIGHTCOL; ov2 |
| 0x208 | 8 | e001 1fff 0004 0000 (light clamp consts, bytes +0x18..+0x1f of light slot 0; MOVEMEM never overwrites them) | ov2 |
| 0x270 | 0x80 | "RSP SW Version: 2.0D, 04-01-96" and the SGI team string (overwritten by light slots 5.. = 8 lights) | none |
| 0x2f0 | 0x20 | MOVEMEM destination table (II-0x80), then MOVEWORD table at 0x30e (shares bytes with it) | 2.3/2.4 |
| 0x320 | 16 | viewport: vscale s16[4] at 0x320, vtrans s16[4] at 0x328 (raw) | MOVEMEM 80; vtx |
| 0x330 | 8 | fog: fm s16, fo s16, 0x00ff (max), 0 | MOVEWORD 08; vtx |
| 0x336 | 0x28 | DL return stack (10 x 4 bytes, unaligned) | DL/ENDDL |
| 0x360 | 0x40 | modelview matrix (int 0x360, frac 0x380) | MTX/POPMTX |
| 0x3a0 | 0x40 | projection matrix | MTX |
| 0x3e0 | 0x40 | MP = modelview x projection (int 0x3e0, frac 0x400) | MTX/POPMTX/MOVEMEM/MOVEWORD 00 |
| 0x420 | 0x280 | vertex buffer 16 x 40 B (clip halfword at +0x24) | vtx; CULLDL; tri |
| 0x6a0 | 0x140 | command buffer (40 commands) | refill; loop |
| 0x7e0 | 0x200 | geometry DMA buffer (class-0 commands; MTX matrix at 0x7e0..0x828) | class-0 DMA |
| 0x8e4 | 16 | yield save slots gp,k1,k0,s7 (inside the DMA buffer) | ov3 |
| 0x9e0 | to 0xfbf | RDP output buffer (s7); scratch: 0xde0..0xe1f matrix product / tri vertex addrs; 0x940.. clip scratch | flush; tri; MTX |
| 0xfc0 | 0x40 | OSTask: type 0xfc0, flags 0xfc4 (bit0 yielded), ucode 0xfd0, dram_stack 0xfe0, output_buff 0xfe8, output_buff_size(end) 0xfec, data_ptr 0xff0, yield_data_ptr 0xff8 | init |

v31 = D128[0x30] and v30 = D128[0x40] are loaded at init (IMEM 0x788/0x78c) BEFORE the yielded-task test, so a resumed task reloads them
too; they are never written afterwards. [verified: resume test enters through the normal entry]



### 5.1 Scratch that G_VTX, G_TRI1 and the clipper use (SM64 layout; other builds in 10.3.2)

| address | size | content | written by | read by |
|---|---|---|---|---|
| 0x7e0 | n * 16 | the input vertices of the current G_VTX (class-0 DMA); vertex pairs are read 32 bytes at a time, so the loop reads up to 16 bytes past the last pair (stale, see 11.3) | G_VTX DMA | G_VTX |
| 0x8e0 +0, +4, +8 | 3 words | the sorted vertex addresses L, M, H of the last triangle | G_TRI1 | (nothing; leftover) |
| 0x8e0 +64, +66 / +72, +74 | 4 halfwords | the magnitude measure of s and t: high word / low word (8.5) | G_TRI1 | G_TRI1 |
| 0x8e0 +68 / +76 | 2 halfwords | `m2` = (nearest scaled W) / 2, high / low (8.4) | G_TRI1 (texture) | G_TRI1 |
| 0x8f0 +16k +{0, 2, 4, 6}, 0x8f8 +16k +{0, 2, 4, 6} | k = 0, 1, 2 | the texture values s', t', w' (+6 = a junk lane) of the sorted vertex k: high words at 0x8f0.., low words at 0x8f8.. (8.4) | G_TRI1 (texture) | G_TRI1 |
| 0x940, 0x954 | 10 halfwords each | the clipper's polygon buffers (zero terminated lists of vertex addresses) | clipper | clipper |
| 0x9e0 | up to 0xb0 | the RDP triangle assembled by G_TRI1 (and one RDP command at a time for the pass-through commands); flushed immediately | G_TRI1, RDP commands | the flush DMA |
| 0xde0, 0xde4, 0xde8 | 3 words | the three vertex addresses of the current triangle; flat-shading vertex pick | G_TRI1 | G_TRI1 |
| 0xde0 .. 0xe20 (+0xe00 fractions) | 0x40 | the product N x T of a G_MTX multiply (also used as the transpose scratch by the relight) | G_MTX, relight | (nothing; leftover) |
| 0xde0 + 40 s, s = 0..11 | 40 each | the new vertices of the clipper (up to 12), same record layout as the vertex buffer except +38 | clipper | G_TRI1 on the fan triangles |


## 6. G_VTX

Command: `w0 = 0x04000000 | (p << 16) | (16 n - 1)`, `w1` = segment address of `n` vertices, `n = (p >> 4) + 1` (1..16), `v0 = p & 15` (first buffer slot, `v0 + n <= 16`; a larger sum writes past the vertex buffer).
The microcode DMAs the `n * 16` input bytes to DMEM 0x7e0 first (the DMA is outside the routine). Input vertex (16 bytes, big endian): `x y z` (s16), flag (u16, unused),
`s t` (s16, s10.5 texels), then 4 bytes: colour `r g b a`, or with lighting normal `nx ny nz` (s8) and `a`.

Output: slot `v0 + i` of the vertex buffer (DMEM `0x420 + 40 * (v0 + i)`), 40 bytes:

| offset | content |
|---|---|
| +0  | clip X, Y, Z, W integer parts (s16 x 4) |
| +8  | clip X, Y, Z, W fractions (u16 x 4) |
| +16 | colour `r g b a` (lit and fogged as below) |
| +20 | texture s, t after scaling (s16 x 2) |
| +24 | screen x, y, z (s16 x 3, 10.2 pixel units for x, y; z in 0..0x7fff) and +30 the u16 fraction of z |
| +32 | 1/W integer (s16) and +34 fraction (u16) |
| +36 | clip flags (u16) |
| +38 | not set by the routine (the pair store of the second vertex of a pair puts the first vertex's flag halfword there; see 6.6) |

The microcode processes the vertices two at a time (the 8 lanes of a vector register); an odd count still computes the garbage second lane, which is never stored.

### 6.1 Clip coordinates

With `M[j][k] = Mi[j][k] * 65536 + Mf[j][k]` (the MP matrix at DMEM 0x3e0: integer parts at `0x3e0 + 8j + 2k`, fractions at `0x400 + 8j + 2k`; `j` = row = input component x, y, z, 1;
`k` = column = output component X, Y, Z, W), for `k = 0..3`:

    A_k   = x * M[0][k] + y * M[1][k] + z * M[2][k] + Mf[3][k]            (48-bit, wraps; x, y, z the s16 input position; the products are N(Mf, v) + H(Mi, v) chains)
    lo_k  = U16(A_k)                                                      (read before the integer part of row 3 is added)
    hi_k  = S16(A_k + Mi[3][k] * 65536)

`(hi_k, lo_k)` is the 16.16 clip coordinate `(X, Y, Z, W)`; it is stored at +0 (integer parts) and +8 (fractions) and is what the clipper interpolates.

The microcode also forms a copy of the clip coordinates scaled by the clip ratio `K = s16 DMEM[0x76]` (1 unless the game sent G_MOVEWORD G_MW_CLIP, section 3.4):
`(hi2_k, lo2_k) = (hi_k, lo_k) * K` (`lk = lo_k * K` as u16 times s16; `hi2_k = S16(hi_k * K + (lk >> 16))`, `lo2_k = lk & 0xffff`). **The K-scaled copy is used for the clip flags only (6.2); the stored
clip coordinates and the whole projection (6.3) use the unscaled `(hi_k, lo_k)`** [verified for K = 2, 5 and random by the clipper experiments]. For K = 1 the two are identical. (The first-draft
local reference `pair_scalar` projected the scaled copy; the local routine now declines K != 1.)

### 6.2 Clip flags

For `k = 0, 1, 2` (X, Y, Z) compare `X_k = (hi_k, lo_k)` with the 32-bit `W1 = (hi_3, lo_3)` and with `W2 = (hi2_3, lo2_3)` (the K-scaled W of 6.1) by the RSP's VCH (high words) + VCL (low words) pair.
With `Wj` either of them the results are exact 32-bit comparisons (`-Wj` taken without wrap):

    le(X, Wj) = [X <= -Wj]  except 0 when X = 0 and Wj = 0                 ge(X, Wj) = [X >= Wj]

(the same formulas hold for W < 0; the `X = Wj = 0` exception is the VCH/VCL equality behaviour: an all-zero matrix records flags 0x7700, not 0x7777). Let `le2_k, ge2_k` use `W2` and `le1_k, ge1_k` use `W1`.
The flag halfword stored at +36 is

    F = sum over k = 0..2 of ( le2_k << k | le1_k << (4 + k) | ge2_k << (8 + k) | ge1_k << (12 + k) )

| bits | meaning |
|---|---|
| 0, 1, 2 / 8, 9, 10 | x, y, z <= -K*W / >= K*W (guard-band compare) |
| 4, 5, 6 / 12, 13, 14 | x, y, z <= -W / >= W (exact frustum) |

Bits 3, 7, 11, 15 are 0. G_TRI1 uses `F & 0x7070` (the exact frustum, all six sides) to reject a triangle whose three vertices are all outside the same plane, and `F & 0x4343` (x, y in the K guard
band: bits 0, 1, 8, 9; z in the exact frustum: bits 6, 14) to decide that clipping is needed. For K = 1 the two groups are equal (`F = B | B << 4`). Pilotwings 64 differs (section 10).

### 6.3 Reciprocal of W, screen coordinates

    nW   = floor((hi_3 * 65536 + lo_3) * persp / 65536)  as (S16, U16) of  hi_3 * persp + (lo_3 * persp >> 16)         persp = u16 at DMEM 0x112 (0xffff normally)
    R    = reciprocal(nW) of 1.1   -> stored at +32 (rh) and +34 (rl)
    rhc  = rh >= 0 ? rh : 0x7fff
    c'   = floor((hi_k * 65536 + lo_k) * (rhc * 65536 + rl) / 65536)        for k = 0..3  -> (sh_k = S16(c'), sl_k = U16(c' - H(hi_k, rhc)))     (the unscaled clip coordinates of 6.1)
    t_k  = sh_k * persp + ((sl_k * persp) >> 16)                           (a 32-bit value; sh_k signed, the rest unsigned)
    sh2_k = t_k >> 16 (arithmetic),   sl2_k = t_k & 0xffff
    V_k  = vtrans_k * 65536 + vscale_k * (sh2_k * 65536 + sl2_k)           (48-bit)
    screen_k = clamp( S16(V_k), lo_k, hi_k )   with  (lo, hi) = (lim[1], lim[0]) for k = 0, 1 and (lim[3], lim[2]) for k = 2, 3,  lim[i] = s16 at DMEM 0x28 + 2i  (SM64: +-4090 and 0..0x7fff)
    zfrac    = U16(V_2)                                                    stored at +30

with the viewport `vscale_k = clamp_s16(vs_k * c50_k)` where `vs` = the four s16 at DMEM 0x320 (x, y, z, w scale) and `c50 = {1, -1, 1, 1}` the s16 at DMEM 0x50 (this negates the y scale: screen y grows
downwards), `vtrans` = the four s16 at DMEM 0x328. The stored triple is `screen_0..2` (+24, +26, +28), `zfrac` at +30.

### 6.4 Texture coordinates

`s' = (s * ts) >> 16`, `t' = (t * tt) >> 16` (arithmetic shift of the s32 product), `ts, tt` the two u16 at DMEM 0x124, 0x126 (the G_TEXTURE scale words); stored at +20, +22. With texture
generation (7.1) s, t are replaced before this scaling.

### 6.5 Fog (geometry bit 0x10000)

With `f0, f1, f2, f3` the s16 at DMEM 0x330..0x336 (multiplier, offset, maximum): `v = clamp_s16(S16((sh2_2 * 65536 + sl2_2) * f0) + f1)` (z of the normalised coordinates, before the viewport), clamp `v` to `[0, f2]`; the alpha byte
of the stored colour (+19) becomes `v & 0xff`. Without fog the colour is stored unchanged.

### 6.6 Other facts observable in DMEM

* The 16-bit flag halfword is stored at +36 for vertex A of a pair by a halfword store; for vertex B the microcode stores a *word* at +36 whose upper half is B's flags and lower half A's flags, so +38 of vertex B holds
  A's flags (junk), and vertex B's own +38 is written by nothing else. For an odd count the second lane of the last pair is computed and not stored. [verified]
* The first output pair writes +0..+7 of its vertices with scratch before the clip coordinates overwrite them: no net effect.
* The alpha byte replaced by the fog value is stored to DMEM only (+19); the microcode's register copy keeps the original alpha, which later code does not read.

## 7. Lighting, texture generation and relight

### 7.1 Lighting and texture generation (geometry bits 0x20000 lighting, 0x40000 texgen, 0x80000 linear texgen)

With lighting on, the microcode runs the lighting overlay (overlay 2, IMEM 0x774; when another overlay is resident, a stub at 0x768/0x778 first loads it, see 9.1.4) in place of the plain colour copy. The
overlay's first act is to test the light-count word `L = DMEM32[0x12c]`: if it is negative (bit 31 set; G_MTX and G_MOVEWORD NUMLIGHT set it) the *relight* of section 7 runs first, which rewrites the transformed
light directions and clears the flag (`L := L & 0xfff`); afterwards (and when the flag was clear) the per-vertex loop below runs. `L = 32 * (nlights + 1)` for `nlights` directional lights (G_MW_NUMLIGHT).

Light slot `a` (a byte offset multiple of 32) has: colour `col[0..3]` at `0x1b0 + a` (r, g, b, pad; unsigned bytes), the colour repeated at `+4`, the transformed direction at `0x1c0 + a`
(signed bytes `dx dy dz x`, the 4 bytes repeated at `+4`: written by the relight and read here as eight consecutive bytes; the second copy serves the second vertex of the pair). Directional light `i`
(0-based, MOVEMEM index 0x86 + 2i) is slot `a = 64 + 32 i` (colour at 0x1f0 + 32 i), the ambient colour is in the slot after the last light (`a = L`; its colour field `0x1d0 + L`),
slots `a = 32` and `a = 0` (at 0x1d0 / 0x1b0) hold the two look-at vectors (MOVEMEM 0x84 = X, 0x82 = Y) whose colour fields games leave 0.

For the vertex pair (both vertices at once; lanes 0-3 vertex 0, lanes 4-7 vertex 1) with normals `n = (nx, ny, nz)` as signed bytes (the input bytes 12..14, byte 15 = alpha):

    acc  = 0;   c = ambient colour bytes (0x1d0 + L + i, i = 0..7) << 7              (the "current product", 16-bit lanes)
    for a = L, L - 32, ..., down to and including the first a <= 0:        (iterations: L/32 + 1 >= 2 for L >= 32)
        acc_c = sadd(acc_c, max(c, 0))                                                      (the previous iteration's product; first time: the ambient)
        d_xyz = VMULF(n << 8, dirbyte << 8) = 2 * n * dirbyte                               (per component, exact; dirbyte = s8 at 0x1c0 + a + lane)
        D     = sadd(sadd(d_x, d_y), d_z)                                                   (VADD chain; lane 0 for vertex 0, lane 4 for vertex 1)
        c     = VMULF(colbyte << 7, D) = clamp_s16((2 * (colbyte << 7) * D + 0x8000) >> 16)   (colbyte = u8 at 0x1b0 + a + lane)
    (the last iteration's c is not accumulated)
    colour byte = (acc_c >> 7) & 0xff   for r, g, b (arithmetic shift);   alpha = the input alpha byte

where `sadd` is the saturating s16 add. Iterations `a = 32` and `a = 0` (look-at slots) add products whose colour is zero unless a game sets look-at colours. [verified: bit-exact in `src/native/f3d_vtx.c`
(random lights, normals, 0..7 lights)]

Texture generation (bit 0x40000; with lighting on only): after the loop, with `D0` the `D` of the final iteration (`a = 0`, look-at Y slot), `D1` the `D` of the iteration before it (`a = 32`, look-at X slot),
for each vertex `g` and lane `i = 4g + j`, `j = 0` (s) and `1` (t):

    acc_i = 2 * (u8colY_i << 7) * D0 + 0x8000  +  u16(lk_i << 8) * D1           colY_i = byte at 0x1b0 + i, lk_i = signed byte at 0x1a0 + i      (a u16 times s16 product)
    a_i   = clamp_s16(acc_i >> 16)
    linear (bit 0x80000):  x = VMULF(a_i, a_i);  x = VMULF(x, a_i);  acc2 = 2 x c1 + 0x8000 + 2 x c3 + 2 a_i c2;  a_i = clamp_s16(acc2 >> 16)       (c1, c2, c3 = s16 at DMEM 0xa2, 0xa4, 0xa6)
    s, t  = clamp_s16(a_i + 0x4000)                                                        (replace the vertex's own s, t before the texture scale of 6.4)

The bytes at 0x1a0 are constants (0x80 0x80 0x80 0x80 ...: `lk << 8` is 0x8000), not look-at vectors; the look-at vectors enter through `D0` / `D1`. With the usual zero look-at colours the product `colY` is 0 and
`s = t`; an implementation must still compute both lanes as written. [verified by the same tests]

### 7.2 Relight: the light directions are recomputed when the lights-stale flag is set

Scope: the code in overlay 2 that recomputes the cached view-space light directions when the "lights stale" flag is set
(`bltz at, 0x80c` in the lit G_VTX path), its entry and exit conditions, the exact fixed-point arithmetic, and the state it
leaves behind. SM64 build (ucode id 94c7cfdd2c872523) addresses unless stated; Pilotwings 64 and Wave Race 64 in 7.2.5.
The lit vertex loop itself (0x774..0x808) and the stale-flag setters are other parts of the spec.

Tags: [verified] = confirmed by differential run of the C model against the parallel-rsp JIT executing the real microcode;
[read] = from the disassembly only. Everything under "Evidence" is in `relight/` .

Summary in one paragraph: for every light slot `at = W, W-32, ..., down to the first value <= 0` (W = word & 0xfff), the three
signed direction bytes at `0x1b8+at` are multiplied by the 3x3 part of the modelview matrix **in transposed sense**
(`out[l] = sum_c M[l][c] * d[c]`, M[r][c] = s15.16 at 0x360/0x380 + 8r + 2c), normalised with a ROM reciprocal square root, clamped to
[-8191, 8191], and stored as four signed bytes (`>> 6`) at `0x1c0+at`, duplicated into `0x1c4+at`. Then `0x12c := W`, the MP
matrix registers are reloaded and the code jumps back to the lit path entry (0x774), which now sees a non-negative `0x12c`.


#### 7.2.0 Notation

- `D8[a]`, `D16[a]`, `D32[a]`: big-endian DMEM byte / halfword / word; addresses wrap at 4096. `sx8(b)` = signed byte, `s16(x)`/`u16(x)` = signed / unsigned view.
- `floor(x/65536)` is an arithmetic shift right (`>> 16`). `clamp16(x)` = clamp to [-32768, 32767].
- VU accumulator `acc` = 48-bit signed per lane. Readouts: `md(acc) = clamp16(acc >> 16)` (VMULF/VMACF/VMADM/VMADH/VMUDH readout);
  `lo(acc)` = for VMADN: if acc >= 2^31 -> 0xffff, if acc < -2^31 -> 0, else `acc & 0xffff` ("uclamp"). VMUDL/VMUDN/VMUDM return the plain
  accumulator slice (`acc & 0xffff` resp. `(acc >> 16) & 0xffff`) without clamping. No intermediate in this routine ever exceeds 2^47, so the
  48-bit wrap is never reached (the model uses int64 and also wraps explicitly in `relight_ref`). [verified]
- Element selectors (`vt[e]`): e=0/1 same lane; e=3 "1q": lane pair (1,1,3,3,5,5,7,7); e=4,5,6 "0h/1h/2h": lane 0,1,2 of each half (lanes 0..3 use lane
  0/1/2, lanes 4..7 use lane 4/5/6); e=8..15: broadcast lane e-8.
- Constants: v31 = DMEM 0x30 = {0x0000, 0x0001, 0x0002, 0xffff, 0x4000, 0x0004, 0x0633, 0x0200}; v30 = DMEM 0x40 = {0x7fff, 0xfff8, 0x0008, 0x0040, 0x0020, 0x8000, 0x01cc, 0xcccc}.
  The routine uses v31[0]=0, v31[2]=2, v31[4]=0x4000, v30[3]=0x40. (Same in SM64, PW and WR dumps. [verified from the dumps])
- Matrix: `Mi[r][c] = s16(D16[0x360 + 8r + 2c])` (integer part), `Mf[r][c] = u16(D16[0x380 + 8r + 2c])` (fraction), r = 0..2 (row 3 is never read), c = 0..3.
  Value = Mi + Mf/65536 (s15.16).
- Light slot of count value `at`: raw Light_t at `0x1b0+at` (colour +0, colour copy +4, direction +8..+10, pad +11), transformed direction at `0x1c0+at` (8 bytes).
  at = 0: lookat Y slot (0x1b0), at = 32: lookat X (0x1d0), at = 64+32i: light i (0x1f0+32i). The 8 bytes at `0x1c8+at` are not touched (for at=64 they hold the clamp constants at 0x208).

#### 7.2.1 Entry, exit, side effects

##### 7.2.1.1 Entry [read, flow verified]

Lit G_VTX path, SM64 addresses (the lit path is entered by `bne at, zero, 0x774` at 0x5dc, with t7/s0 = the colour/normal words of the vertex pair):

```
0x774 lw   at, 300(zero)          at = D32[0x12c]
0x778 sw   t7, 0(a3)
0x77c sw   s0, 4(a3)
0x780 bltz at, 0x80c              bit 31 of the word: relight (the delay slot LPV v4,0,0(a3) runs in both cases)
0x784 LPV  v4, 0, 0(a3)
```

At 0x80c the relight starts. Inputs it needs:

| input | value |
|---|---|
| `at` (sr[1]) | the light-count word W_full = D32[0x12c] (bit 31 set; the routine does not test it again; only bits 11..0 are used) |
| sp (sr[29]) | 0x110 (used once: `LDV v2,0,31(sp)` = DMEM 0x208) |
| v30, v31 | the standard constants above |
| v11, v15 | only lanes 1,2 of v11 and lanes 5,6 of v15 matter, and only for byte 3 of every output direction (section 7.2.2.5). In the vertex path they are the MP matrix row 3 (int / frac, duplicated halves), loaded by the `jal 0x510` at 0x5a0 |
| DMEM | 0x360..0x397 (modelview rows 0..2), the slot bytes, 0x208..0x20d (clamp constants), 0xa0 (return address halfword) |
| a3, t7, s0, all other scalar and vector registers | not used and not modified (apart from the clobbers listed in 7.2.1.3) |

The routine is NOT entered in any other way: the only caller is the `bltz` above. It runs once per "stale" flag set (the first lit vertex after a G_MTX / G_MOVEWORD NUMLIGHT; the flag setters are in section 3.4).

##### 7.2.1.2 What `jal 0x524` does (0x814, delay slot `addi t0, zero, 864` executes first, so t0 = 0x360 when the routine starts)

The routine at 0x524..0x554 is shared with the vertex path (which enters it at 0x510 with t0 = 0x3e0). It only loads vector registers, nothing is written to DMEM:

```
LDV v8 ,0,0(t0) / LDV v8 ,8,0(t0)   v8  lanes 0..3 = lanes 4..7 = D16[t0+0 .. t0+7]    (4 halfwords, duplicated in both halves)
v9 = 8 bytes at t0+8, v10 = t0+16,  v12 = t0+32, v13 = t0+40, v14 = t0+48   (each duplicated)
jr ra (ra = 0x81c here); the last LDV (v14, high half) is in the delay slot
```

With t0 = 0x360: v8, v9, v10 = modelview integer rows 0, 1, 2 (`Mi[r][0..3]`, lane l and l+4 equal); v12, v13, v14 = fraction rows 0, 1, 2 (`Mf[r][0..3]`). [verified: lane table below]

##### 7.2.1.3 Exit [verified]

After the last slot: `bgtz at, 0x858 ; addi at, at, -32` falls through, then

```
0x8f4 j   0x510
0x8f8 lh  ra, 160(zero)           ra = sign-extended D16[0xa0]  (0x1774 in the SM64 dump)
0x510 addi t0, zero, 992 ; LDV v11,0,3(t0) ; LDV v11,8,3(t0) ; LDV v15,0,7(t0) ; LDV v15,8,7(t0)   (v11 = MP int row 3 @0x3f8, v15 = MP frac row 3 @0x418, duplicated)
0x524 .. 0x554  v8,v9,v10 = MP int rows 0..2 @0x3e0/0x3e8/0x3f0, v12,v13,v14 = MP frac rows @0x400/0x408/0x410 (duplicated); jr ra
```

`jr ra` goes to `ra & 0xfff` = 0x774, the lit path entry again; this time D32[0x12c] >= 0, so the normal lit loop runs, and the delay-slot `LPV v4,0,0(a3)` plus the two `sw t7/s0` stores are simply repeated (idempotent).
So the relight is a prefix of the lit path, not a replacement: the vertex then gets lit with the new directions. [verified: running the whole lit path from 0x774 to its tail 0x5e4 twice,
once with the stale flag (relight in the path) and once with the directions precomputed by the model and the flag cleared, gives identical DMEM and identical live registers
(v17-v21, v27-v29 and all scalar registers except at/t0/ra), 24000 cases with realistic counts W = 32..288, VCO = 0. With W = 0 (a single slot) the texgen outputs v18/v20 would
depend on the stale v6 that the relight overwrites - `VOR v20,v6` at 0x7a0 reads v6 before it is written in the first iteration; SM64 always has W >= 64.]

State at the moment `jr ra` lands at 0x774 (compared against the JIT in every test case [verified]):

| item | value after the relight |
|---|---|
| D32[0x12c] | `W & 0xfff` (bit 31 and bits 30..12 cleared) - see section 7.2.4 for the other writes |
| sr[1] `at` | `W_last - 32` where W_last is the last processed slot (<= 0); for W multiple of 32: -32 |
| sr[8] `t0` | 992 (0x3e0) |
| sr[31] `ra` | `s16(D16[0xa0])` read after all slot writes |
| other sr | unchanged |
| v8, v9, v10, v12, v13, v14 | MP matrix rows reloaded from 0x3e0 / 0x400 (halves duplicated) |
| v11, v15 | MP row 3 int / frac @0x3f8 / 0x418 |
| v2 | lanes 0..3 = D16[0x208..0x20f] (e001 1fff 0004 0000 normally), lanes 4..7 unchanged |
| v3, v4, v5, v6, v7, v26 | values of the last slot (all 8 lanes, `relight_ref` reproduces them): v6 = lo_l, v3 = hi_l (section 7.2.2.3), v5 = sqlo_l, v26 = sqhi_l, v4 = md(N_l) of the normalise step, v7 = (x_l * C2) & 0xffff (the stored value) |
| accumulator | acc_l = u16(x_l) * s16(C2) (non-negative) |
| VCO | 0; VCC low byte = lane mask of the last VLT (x_l < C1 -> 1), VCC high byte 0; VCE unchanged |
| divide unit | div_in = 0, dp_flag = 1 (set by the last `VRSQH ..,v31[8]`), div_out = high word of the last slot's reciprocal square root |

No other vector register (v0, v1, v16..v25, v27..v31) is touched. [verified: all 32 vector registers, accumulator, 3 flag registers, div state compared]

#### 7.2.2 Arithmetic per light slot

##### 7.2.2.1 Slot order and count

```
at = W & 0xfff ; D32[0x12c] := at
loop:  process slot at ;  if (at > 0) { at -= 32; repeat } else { at -= 32; exit }
```

(The `bgtz` tests `at` before the delay-slot `addi`.) For W a multiple of 32: slots W, W-32, ..., 32, 0 (W/32 + 1 slots, highest first; slot 0 = lookat Y is processed last).
For other W the sequence is W, W-32, ... until the first value <= 0 (that value is processed too, with a possibly negative `at`; addresses wrap mod 4096). SM64: W = 32*(n+1) for n lights,
so n lights + 2 lookat slots are covered. [verified incl. odd W]

##### 7.2.2.2 Setup before the loop (0x81c..0x854) [verified]

After `jal 0x524` (section 7.2.1.2), with `R[k]` = v(8+k) lanes 0..7 before the transpose: R0..R2 = `Mi[r][l&3]`, R3 = v11 (entry value), R4..R6 = `Mf[r][l&3]`, R7 = v15 (entry value).

`STV v8,e,k(t0)` for (e,k) = (2,1) (4,2) (12,3) (14,4) with t0 = 0xde0 writes four 16-byte blocks B_n at 0xdf0 + 16n; then four `LTV` read them back
diagonally into v8..v15; `SDV v12,8,2(t0)`, `SDV v13,8,4(t0)`, `SDV v14,8,6(t0)` store the high half of v12..v14 at 0xdf0, 0xe00, 0xe10 and `LDV v12/13/14,0,..` bring it to the low half.
Net effect (lane l = 0..3 is what the loop uses; lanes 4..7 below are leftovers that never reach a stored byte):

```
v8  = { Mi00, Mi10, Mi20, Mi03 | Mi00, Mi01, Mf20, v15[0] }
v9  = { Mi01, Mi11, Mi21, v11[1] | Mi10, Mi11, Mi12, v15[1] }
v10 = { Mi02, Mi12, Mi22, v11[2] | Mf02, Mi21, Mi22, Mi23 }
v12 = { Mf00, Mf10, Mf20, Mf03 | Mf00, Mf10, Mf20, Mf03 }
v13 = { Mf01, Mf11, Mf21, v15[5] | same }
v14 = { Mf02, Mf12, Mf22, v15[6] | same }
```

(`v11`, `v15` are the entry values; lanes 0..3 of v8..v10 are a transpose of the 3x3 integer matrix, v12..v14 of the fraction matrix.) [verified: 20000 random cases at the loop head 0x858]
Memory side effect of this setup: block n (address 0xdf0 + 16n, halfword s = 0..7) = `R[(s + h_n) & 7][s]` with h = {1, 2, 6, 7}, except for n < 3, s < 4 where it is the
transposed fraction matrix `Mf[s][n]` for s<3 and (`Mf03`, `v15[5]`, `v15[6]`) for s = 3 (n = 0,1,2). Code: `relight_fast`. [verified]

Define the coefficient rows used by lane l (l = 0..3):

```
lane l=0,1,2:   F[l][c] = Mf[l][c],   I[l][c] = Mi[l][c]                     c = 0,1,2
lane 3:         F[3] = ( Mf[0][3], u16(v15[5]), u16(v15[6]) ),  I[3] = ( Mi[0][3], s16(v11[1]), s16(v11[2]) )
```

##### 7.2.2.3 One slot, step by step (all lane arithmetic exact unless a saturation is named) [verified]

Let `a = (at + 0x1b8) & 0xfff` and `b_i` (i = 0..7) the byte at LPV address: `b_i = D8[ ((a & ~7) + ((i + (a & 7)) & 15)) & 0xfff ]` (for a multiple of 8: `D8[a+i]`).
Only b0, b1, b2 matter for lanes 0..3 (b4..b6 for lanes 4..7).

```
0x858 LPV  v5 = { b_i << 8 }                         (s16)
0x85c VMULF v5 = v5 * v31[4] (0x4000)               dd_i = (b_i*256*0x4000*2 + 0x8000) >> 16 = 128 * sx8(b_i)   exact (the rounding term never matters)
                                                      dd_c = 128*sx8(b_c), c = 0..2, in [-16384, 16256]
0x860 VMUDN v6  = F[l][0]*dd_0                       acc_l = sum_c u16(F[l][c]) * dd_c        (VMUDN, VMADN, VMADN; broadcast e=4,5,6 = dd_0, dd_1, dd_2)
0x864 VMADN v6 += F[l][1]*dd_1
0x868 VMADN v6 += F[l][2]*dd_2
0x86c VMADM v3  = md(acc + 0)                        t_l = clamp16( floor(S_l / 65536) ),  S_l = sum_c u16(F[l][c]) * dd_c      [SATURATES: |S| >= 2^31 -> 0x7fff / 0x8000]
0x870 VMUDM v6  : acc := t_l * 2                     (the old accumulator is DISCARDED: only t_l = the integer part of the fraction sum survives; v6 is overwritten later)
0x874 VMACF v3 += 2 * I[l][0]*dd_0                   acc = 2*t_l + 2 * sum_c s16(I[l][c]) * dd_c =: T_l   (VMACF: no rounding constant)
0x878 VMACF v3 += 2 * I[l][1]*dd_1
0x87c VMACF v3 += 2 * I[l][2]*dd_2
0x880 VMADN v6  = lo(T_l)                            hi_l = v3 = clamp16(floor(T_l / 65536))   (saturates)
                                                     lo_l = v6 = (-2^31 <= T_l < 2^31) ? (T_l & 0xffff) : (T_l > 0 ? 0xffff : 0)
```

(hi_l : lo_l) is the s15.16 vector component `T_l / 65536`, i.e. `256 * (M.d) / 65536` with `M.d = sum_c M[l][c]*sx8(b_c)`; for a unit light (|d| = 127) and a rotation matrix `T/65536` is about 127/256.
For very large matrices T saturates to (0x7fff : 0xffff) / (0x8000 : 0x0000).

Squared length (lanes 0..2 only; lane 3 and 4..7 are not used):

```
0x884 VMUDL v5  : acc = floor(lo*lo / 65536)                       (u16 * u16)
0x888 VMADM v5  : acc += s16(hi) * u16(lo)
0x88c VMADN v5  : acc += u16(lo) * s16(hi)         v5 = sqlo_l = lo(acc)    <- saturation decided on the 3-term sum, BEFORE hi*hi is added (so it can differ from the final value)
0x890 VMADH v26 : acc += s16(hi) * s16(hi) * 65536   v26 = sqhi_l = clamp16(floor(acc / 65536))
```

i.e. `A3 = floor(lo^2/65536) + 2*hi*lo`, `sqlo = lo(A3)`, `sqhi = clamp16(floor((A3 + hi^2 * 65536) / 65536))`, an approximation of `X^2 / 65536` for X = hi*65536 + lo.

Horizontal sum (only lane 2 of the result is used):

```
0x894 VADDC v7 = v5 + v5[3]   lane 0: s01 = sqlo_0 + sqlo_1 (u16 add, carry c01 -> VCO bit 0)         (lane 2 of this result and v4 below are discarded)
0x898 VADD  v4 = v26 + v26[3] lane 0: h01 = clamp16(sqhi_0 + sqhi_1 + c01)    [saturating add, consumes and clears VCO]
0x89c VADDC v7 = v5 + v7[4]   lane 2: s2 = sqlo_2 + (s01 & 0xffff) (u16 add, carry c2)                  e=4 broadcasts lane 0
0x8a0 VADD  v4 = v26 + v4[4]  lane 2: h2 = clamp16(sqhi_2 + h01 + c2)
```

(Lane 3's square is NOT part of the length: the e=3 add of lane 2 is overwritten by the e=4 add.) Length-squared input of the divide unit: `Lsq = (h2 << 16) | (s2 & 0xffff)` (h2 >= 0, so Lsq >= 0).

Reciprocal square root (fork `rsp_vrcp_vrsq<true>`, double-precision flavour; ROM listed in the appendix):

```
0x8a4 VRSQH v11[0] = div_out(old)  ; div_in := h2 ; dp_flag := 1       (v11[0] garbage, overwritten below)
0x8a8 VRSQL v15[0] = low 16 bits of R ; div_out := R >> 16 ; dp_flag := 0, with input = (div_in << 16) | s2 = Lsq
0x8ac VRSQH v11[0] = div_out (= R >> 16) ; div_in := v31[0] = 0 ; dp_flag := 1

rsq32(input):  mask = input >> 31 ; data = input ^ mask ; if (input > -32768) data -= mask
               if data == 0:          R = 0x7fffffff
               elif input == -32768:  R = 0xffff0000
               else:  shift = clz32(data) ; idx = (((data << shift) & 0x7fc00000) >> 22)
                      idx = ((idx | 0x200) & 0x3fe) | (shift & 1)           (idx in 0x200..0x3ff: only the upper half of the 1024-entry ROM is used)
                      R = ((0x10000 | ROM[idx]) << 14) >> ((31 - shift) >> 1) ;  R ^= mask
```

(Lsq is never negative here, so mask = 0.) R is about `2^? / sqrt(Lsq)` as a 32-bit value; Lsq = 0 (all-zero or vanishing direction) gives R = 0x7fffffff.

Scale (0x8b0..0x8b8) - lane 0 of v15 / v11 only:

```
Kacc = floor(u16(R & 0xffff) * 0x40 / 65536) + s16(R >> 16) * 0x40         (VMUDL v15,v15,v30[3]; VMADM v11,v11,v30[3]; VMADN v15,v31,v31[0])
Khi  = v11[0] = clamp16(floor(Kacc / 65536))         Klo = v15[0] = lo(Kacc)           (Kacc is small: Khi in 0..31, never saturates)
```

Normalise (0x8bc..0x8cc), lanes l = 0..3, e=8 broadcasts lane 0 of v15 / v11:

```
N_l = floor(lo_l * Klo / 65536) + s16(hi_l) * Klo + lo_l * s16(Khi) + s16(hi_l) * s16(Khi) * 65536     (VMUDL, VMADM, VMADN, VMADH)
v7_l = lo(N_l)                                                                                       (0x8cc VMADN +0; saturates to 0xffff / 0 outside s32)
v4_l = md(N_l)                                                                                       (not used again)
```

Clamp and store (0x8d0..0x8e8):

```
C0 = s16(D16[0x208]), C1 = s16(D16[0x20a]), C2 = s16(D16[0x20c])    read here, once per slot, from DMEM as it is at that moment (normally e001 1fff 0004 -> -8191, 8191, 4)
x_l = s16(v7_l) ;  x_l = (x_l >= C0) ? x_l : C0   (VGE v7,v7,v2[0]) ;  x_l = (x_l < C1) ? x_l : C1   (VLT v7,v7,v2[1])
y_l = (u16(x_l) * C2) & 0xffff                                    (VMUDN v7,v7,v2[2]; acc_l = u16(x_l)*C2)
SPV v7,0,56(at):  D8[(at + 0x1c0 + i) & 0xfff] = (s16(y_l) >> 8)  for lane i = 0..7        -> with the standard constants: byte = x_l >> 6, i.e. floor(x/64) in -128..127
lw t0,448(at) ; sw t0,452(at):   D32[(at+0x1c4)&0xfff] := D32[(at+0x1c0)&0xfff]       (bytes 0..3 copied over bytes 4..7; byte-wise wrap, any alignment)
```

So stored bytes are (out0, out1, out2, out3, out0, out1, out2, out3) with `out_l = (s16((u16(x_l)*C2)&0xffff) >> 8)`. The SPV of lanes 4..7 is completely overwritten by the copy.

Numeric meaning [verified against a float model, `sem.c`, 20000 random rotations x random directions per scale]: `out_l = 127 * (M.d)_l / |M.d|` to within about +-2.1 for rotation matrices scaled by 0.5, 1, 2 and 100 (mean error 0.6..0.7), within 6.1 for scale 0.05, and no longer meaningful for scale 0.001 (T loses its precision, |M.d| ~ 0.1); identity matrix: direction (0,0,127) -> (0,0,127,0), (64,64,64) -> (73,73,73,0), (0,-128,0) -> (0,-128,0,0),
(1,0,0) -> (127,0,0,0) (normalised), (0,0,0) -> all zero (R = 0x7fffffff but N = 0).

##### 7.2.2.4 Rounded / saturated values (checklist)

- Truncation (floor): every `>> 16` (`floor(S/65536)` of the fraction chain, `floor(lo^2/65536)` of VMUDL, `floor(u16*u16*0x40/65536)` of the scale, `floor(lo*Klo/65536)` of the normalise, SPV `>> 8`). No other rounding. The only rounding constant is VMULF's 0x8000 in step 0x85c, which never changes the result (the operand is a multiple of 2).
- Saturated: `t_l` (0x86c), `hi_l` / `lo_l` (0x874..0x880), `sqlo` (decided on the 3-term accumulator), `sqhi`, the two VADD's, `v7` (0x8cc), the VGE/VLT clamp to [C0, C1]. All of these are exercised by the tests (counts in section 7.2.3).
- Wrap, not saturation: `v7 = lo(N)` is the plain low 16 bits of N whenever N fits in s32, so an N in [32768, 2^31) WRAPS (e.g. N = 40000 -> s16 -25536 -> clamped to C0 -> byte -128); only an N outside s32 gives 0xffff (= -1 as s16 -> byte -1) or 0. A normalised component never exceeds about 2^13 * (1 + small), so this only matters for degenerate (huge or vanishing) vectors, but it is part of the bit-exact behaviour.

##### 7.2.2.5 Which lanes matter

| lane | role |
|---|---|
| 0, 1, 2 | the transformed x, y, z; also the squared length (lane 2 = sum of squares of lanes 0,1,2) |
| 3 | byte 3 of the stored direction (D8[0x1c3+at] and its copy D8[0x1c7+at]); computed with the same formulas from F[3], I[3] = (Mf03, v15[5], v15[6]), (Mi03, v11[1], v11[2]) (entry leftovers: normally MP row 3 of the vertex path, i.e. MP[3][1], MP[3][2] int/frac) and the same K. In the lit loop (0x784..0x7b4) lane 3 of the light vector only reaches `v3[2]`, which is unused, so it does not influence vertex colours [read] |
| 4..7 | second copy of the loop for the second vertex of the pair; here they use bytes b4..b6 with partly transposed matrix leftovers; their results are overwritten by the word copy and they do not influence lanes 0..3 (the model `relight_fast` ignores them entirely and matches the JIT) [verified] |

#### 7.2.3 C model and verification

Files in `relight/`:

| file | what |
|---|---|
| `relight_model.h` | generated, self-contained: `relight_fast()` (plain integer maths, DMEM + scalar results) and `relight_ref()` (instruction-by-instruction, all registers). Built from `model2.in.h` + `rsq_rom.inc` by `gen.sh` |
| `test_relight.cpp` | random differential test against the JIT (ucode image from `uc/` or `ucv/`, overlay 2 placed from the descriptor) |
| `test_lanes.cpp` | checks the lane table of 7.2.2.2 at the loop head 0x858 against the JIT |
| `sem.c` | float sanity of the semantics (section 7.2.2.3) |
| `mk.sh`, `mk_lanes.sh`, `mut.sh`, `run_all.sh`, `fh.hpp`, `snap.h` | build / mutation / everything; `fh.hpp` is a copy of the fork host from `cmd/tools` |
| `dec.py`, `tv.py` | decoder of the overlay words (vector fields) and the symbolic STV/LTV transpose used to derive the lane table |

`relight_ref(d, word, state)` needs the entry state (v11, v15 and the other registers) in `state`, and returns the complete exit state; `relight_fast(d, word, v11, v15, &res)` only DMEM and `at/t0/ra/exit_pc`.
`relight_fast` is reproduced below (helpers in the appendix); this is the version meant to be ported.

Commands (the JIT objects are the ones already built in `cmd/obj`; no emulator is started, everything is one process per run; each run is wrapped in `systemd-run --user --scope -p MemoryMax=2G`):

```
cd <work dir>/relight
./gen.sh && ./mk.sh && ./mk_lanes.sh                     # build bin/test_relight, bin/test_lanes
CAP="systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 timeout -s KILL 300"
$CAP ./bin/test_relight -n 40000 -s 1 -F -L              # SM64 ucode (default). -F also checks relight_fast, -L the whole-lit-path flow, -t coverage
$CAP ./bin/test_relight -n 40000 -s 1 -F -L -u <work dir>/ucv/3e9b9c3a6ebde1e0      # Pilotwings 64
$CAP ./bin/test_relight -n 40000 -s 1 -F -L -u <work dir>/ucv/fb483893c5b0ad3d      # Wave Race 64
$CAP ./bin/test_lanes -n 20000
./mut.sh 40000                                           # mutation checks
./run_all.sh                                             # everything, log in run_all.log
```

What each case randomises: all 4096 bytes of DMEM (three fill modes), the modelview matrix (8 modes: tiny, +-300, full s16, identity-like, near-singular/zero, 0x7fff/0x8000 with 0xffff fractions, +-1/0 with small fractions, identity with
0xffff fractions), the direction bytes (random, +-127/0, -128/127 only, tiny, sparse), the word (32*(0..8) 70%, 8*(0..100), 4*(0..300), any value <= 0xe00; bits 12..30 random half the time;
a few fully random words with the count masked to <= 0xe00), the clamp constants (15% random), D16[0xa0] (high nibble random, low 12 bits = the lit entry), all 32 vector registers (v11/v15
sometimes forced to 0 / 0xffff lanes), the 32 scalar registers, accumulator, the three flag registers, the divide unit; v30/v31 standard, sp = 0x110. After the run it compares **all 4096 DMEM bytes**, all
32 scalar registers, all 32 vector registers, the 24 accumulator halfwords, VCO/VCC/VCE (48 lane flags), div_in/div_out/dp_flag, and the exit pc, against `relight_ref`; with `-F` the DMEM and at/t0/ra against `relight_fast`.

Results [verified]: SM64, PW and WR dumps, seeds 1, 2, 3, 40000 cases each = **360000 cases, 0 failures** (log: `run_all.log`); +24000 whole-lit-path flow checks per 40000-case run; lane table 20000 cases, 0 failures.
Coverage of the saturation paths (40000-case SM64 run, `-t`): sat_lo high clamp 97385, low clamp 77831, clamp16 high 5.36M, low 65851, rsq of 0: 197840, VGE took the lower bound 1.76M, VLT took the upper bound 2.67M, VADDC carry-out 1.88M; 508573 slots processed.

Mutation checks (a deliberate error is patched into a copy of `relight_model.h`; the test must fail; run `./mut.sh 40000`) [verified: all 8 caught, the first failing case is among the first 17 of seed 1]:

| name | change | caught by |
|---|---|---|
| `rsq_parity` | ROM index parity bit `shift % 2` -> `(shift+1) % 2` | relight_ref vs JIT (acc, div, flags, DMEM) |
| `satlo_threshold` | VMADN uclamp threshold 2^31 -> 2^32 | relight_ref vs JIT (v5/v6 ...) |
| `no_vadd_carry` | VADD ignores the carry from VADDC | relight_ref vs JIT (v4, div) |
| `spv_shift7` | SPV shifts by 7 instead of 8 | relight_ref vs JIT (DMEM 0x1c5..) |
| `fast_unsigned_frac` | fraction read as signed in `relight_fast` | -F cross-check |
| `fast_noclamp_lt` | VLT clamp removed in `relight_fast` | -F cross-check |
| `fast_skip_slot0` | loop stops before slot 0 | -F cross-check |
| `fast_lane3_coef` | lane 3 coefficient v11[1] -> v11[0] | -F cross-check (shows the byte-3 dependence) |

Out of the tested domain (documented, not an error of the model): a count word W with `0xe00 < (W & 0xfff)` makes the SPV / `sw` of a slot cross DMEM address 0xfff -> 0x000; the model wraps byte-wise (as the C helper `ls_SPV` and the hardware),
but the JIT's SPV did not wrap in the one random case that hit it. Real data has W <= 0x120.

##### 7.2.3.1 relight_fast (verified code)

```c
/* ---- DMEM helpers (big-endian bytes, 12 bit wrap) ---- */
static inline uint16_t rl_r16(const uint8_t *d, unsigned a) { return (uint16_t)(d[a & 0xfff] << 8 | d[(a + 1) & 0xfff]); }
static inline void rl_w32(uint8_t *d, unsigned a, uint32_t v) { for (int i = 0; i < 4; i++) d[(a + i) & 0xfff] = (uint8_t)(v >> (24 - 8 * i)); }
static inline uint32_t rl_r32(const uint8_t *d, unsigned a) { uint32_t v = 0; for (int i = 0; i < 4; i++) v = v << 8 | d[(a + i) & 0xfff]; return v; }

#ifndef RL_COV
#define RL_COV(i) ((void)0)        /* coverage hook of the test harness */
#endif
/* ---- saturation helpers of the VU (acc is the 48-bit signed accumulator value) ---- */
static inline int64_t rl_sx48(int64_t x) { return (int64_t)((uint64_t)x << 16) >> 16; }
static inline int rl_clamp16(int64_t x) { if (x > 32767) { RL_COV(2); return 32767; } if (x < -32768) { RL_COV(3); return -32768; } return (int)x; }
static inline uint16_t rl_sat_md(int64_t acc) { return (uint16_t)rl_clamp16(acc >> 16); }       /* VMULF/VMAC*-style readout: clamp(acc >> 16) */
static inline uint16_t rl_sat_lo(int64_t acc)                                                    /* VMADN-style readout: low word, clamped when acc leaves s32 */
{
    if (acc >= 0x80000000LL) { RL_COV(0); return 0xffff; }
    if (acc < -0x80000000LL) { RL_COV(1); return 0; }
    return (uint16_t)acc;
}
/* the reciprocal square root of the divide unit (rsp_vrcp_vrsq<true> of the fork, double-precision flavour): 32-bit signed input */
static inline int32_t rl_rsq32(int32_t input)
{
    int32_t mask = input >> 31, data = input ^ mask, result;
    if (input > -32768) data -= mask;
    if (data == 0) { RL_COV(4); return 0x7fffffff; }
    if (input == -32768) return (int32_t)0xffff0000u;
    unsigned shift = (unsigned)__builtin_clz((unsigned)data);
    unsigned idx = (unsigned)(((uint64_t)(uint32_t)data << shift) & 0x7FC00000u) >> 22;
    idx = ((idx | 0x200) & 0x3FE) | (shift % 2);          /* 0x200..0x3ff */
    int32_t r = RELIGHT_RSQ_ROM[idx - 512];
    result = ((0x10000 | r) << 14) >> ((31 - shift) >> 1);
    return result ^ mask;
}
typedef struct { int32_t at, t0, ra; uint32_t exit_pc; } relight_fast_result;


static void relight_fast(uint8_t *d, uint32_t word, const uint16_t v11[8], const uint16_t v15[8], relight_fast_result *res)
{
    int32_t at = (int32_t)(word & 0xfff);
    rl_w32(d, 0x12c, (uint32_t)at);
    /* modelview 3x3 (+ column 3): Mi[r][c] int part, Mf[r][c] fraction, at 0x360 / 0x380 + 8r + 2c */
    int32_t Mi[3][4]; uint32_t Mf[3][4];
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) { Mi[r][c] = (int16_t)rl_r16(d, 0x360 + 8 * r + 2 * c); Mf[r][c] = rl_r16(d, 0x380 + 8 * r + 2 * c); }
    /* coefficient rows of the four lanes: lane l computes sum_c coefI[l][c] * dd[c] (+ the fraction chain); lane 3 uses leftovers of v11/v15 */
    int32_t coefI[4][3]; uint32_t coefF[4][3];
    for (int l = 0; l < 3; l++) for (int c = 0; c < 3; c++) { coefI[l][c] = Mi[l][c]; coefF[l][c] = Mf[l][c]; }
    coefI[3][0] = Mi[0][3]; coefI[3][1] = (int16_t)v11[1]; coefI[3][2] = (int16_t)v11[2];
    coefF[3][0] = Mf[0][3]; coefF[3][1] = v15[5];          coefF[3][2] = v15[6];
    /* scratch words left by the STV/LTV transpose at 0xdf0..0xe2f (see the spec): R[reg 8..15][lane] before the STVs */
    {
        uint16_t R[8][8];
        for (int l = 0; l < 8; l++) {
            for (int r = 0; r < 3; r++) { R[r][l] = (uint16_t)Mi[r][l & 3]; R[4 + r][l] = (uint16_t)Mf[r][l & 3]; }
            R[3][l] = v11[l]; R[7][l] = v15[l];
        }
        static const int h[4] = {1, 2, 6, 7};
        for (int n = 0; n < 4; n++)
            for (int s = 0; s < 8; s++) {
                uint16_t w = R[(s + h[n]) & 7][s];
                if (n < 3 && s < 4) w = (uint16_t)coefF[s][n];      /* SDV/LDV: column n of the fraction matrix (row s) */
                unsigned a = 0xdf0 + 16 * n + 2 * s;
                d[a] = (uint8_t)(w >> 8); d[a + 1] = (uint8_t)w;
            }
    }
    for (;;) {
        unsigned a = ((unsigned)at + 440) & 0xfff, idx = a & 7, ab = a & ~7u;
        int dd[3];
        for (int c = 0; c < 3; c++) dd[c] = 128 * (int8_t)d[(ab + ((c + idx) & 0xf)) & 0xfff];
        int hi[4]; uint32_t lo[4];
        for (int l = 0; l < 4; l++) {
            int64_t S = 0, I = 0;
            for (int c = 0; c < 3; c++) { S += (int64_t)coefF[l][c] * dd[c]; I += (int64_t)coefI[l][c] * dd[c]; }
            int64_t T = 2 * ((int64_t)rl_clamp16(S >> 16) + I);
            hi[l] = (int16_t)rl_sat_md(T); lo[l] = rl_sat_lo(T);
        }
        /* squared length: lanes 0..2 */
        int64_t A3[3]; uint32_t sq_lo[3]; int sq_hi[3];
        for (int l = 0; l < 3; l++) {
            A3[l] = ((int64_t)lo[l] * lo[l] >> 16) + 2 * (int64_t)hi[l] * (int64_t)lo[l];
            sq_lo[l] = rl_sat_lo(A3[l]);
            sq_hi[l] = (int16_t)rl_sat_md(A3[l] + (int64_t)hi[l] * hi[l] * 65536);
        }
        uint32_t s01 = sq_lo[0] + sq_lo[1]; int c01 = s01 > 0xffff;
        int h01 = rl_clamp16((int64_t)sq_hi[0] + sq_hi[1] + c01);
        uint32_t s2 = sq_lo[2] + (s01 & 0xffff); int c2 = s2 > 0xffff;
        int h2 = rl_clamp16((int64_t)sq_hi[2] + h01 + c2);
        int32_t R = rl_rsq32((int32_t)((uint32_t)(uint16_t)h2 << 16 | (s2 & 0xffff)));
        int64_t Kacc = (((int64_t)(R & 0xffff) * 0x40) >> 16) + (int64_t)(int16_t)(R >> 16) * 0x40;
        int Khi = (int16_t)rl_sat_md(Kacc); uint32_t Klo = rl_sat_lo(Kacc);
        int16_t C0 = (int16_t)rl_r16(d, 0x208), C1 = (int16_t)rl_r16(d, 0x20a), C2 = (int16_t)rl_r16(d, 0x20c);
        uint8_t out[4];
        for (int l = 0; l < 4; l++) {
            int64_t N = (((int64_t)lo[l] * Klo) >> 16) + (int64_t)hi[l] * Klo + (int64_t)lo[l] * Khi + (int64_t)hi[l] * Khi * 65536;
            int16_t x = (int16_t)rl_sat_lo(N);
            if (x < C0) x = C0;
            if (x > C1) x = C1;               /* VLT: x < C1 ? x : C1 */
            out[l] = (uint8_t)((int16_t)(uint16_t)((uint16_t)x * (int64_t)C2) >> 8);
        }
        unsigned p = ((unsigned)at + 448) & 0xfff;
        for (int l = 0; l < 4; l++) { d[(p + l) & 0xfff] = out[l]; }
        for (int l = 4; l < 8; l++) { d[(p + l) & 0xfff] = d[(p + l - 4) & 0xfff]; }
        res->t0 = (int32_t)rl_r32(d, p);
        int more = at > 0; at -= 32;
        if (!more) break;
    }
    res->ra = (int16_t)rl_r16(d, 0xa0);
    res->t0 = 992;
    res->at = at;
    res->exit_pc = (uint32_t)res->ra & 0xfff;
}
```

#### 7.2.4 DMEM bytes the relight writes

All writes [verified by comparing all of DMEM, mutated bytes included]:

| bytes | content | live afterwards? |
|---|---|---|
| 0x12c..0x12f | `W & 0xfff` | live (lit path flag test; later G_MTX / MOVEWORD set the top byte again) |
| `(0x1c0+at)..+7` for each processed slot | `out0 out1 out2 out3 out0 out1 out2 out3` | live: read by LPV v5,0,56(at) in the lit loop (and the texgen/lookat use of slots 0 and 32). Bytes +3 and +7 (lane 3 / 7) only feed `v3[2]`/`v3[6]`, which the dot-product sequence does not use [read] - candidates for a "never matters" mask, but they are written with leftover-dependent values |
| **0xdf0..0xe2f (64 bytes)** | the transpose scratch of 7.2.2.2: four 16-byte blocks written by STV (0xdf0, 0xe00, 0xe10, 0xe20), then 8 bytes at 0xdf0, 0xe00, 0xe10 rewritten by SDV | **scratch**: written before it is read inside the relight (STV -> LTV, SDV -> LDV), and never read by anything else. The same area is the MTX-multiply scratch (0xde0..0xe1f), the triangle vertex pointers (0xde0..0xde8) and the clip vertex slots (0xde0 + 40 s); those users write before they read [per sections 3.1 and 9, not re-proved here]. 0xde0..0xdef itself is NOT written (t0 = 0xde0 is only the base) |

Nothing else changes. In particular the raw light bytes, the modelview matrix, the constants at 0x208, 0xa0 and IMEM are not written; the generic scratch (other than 0xdf0..0xe2f) is untouched.
The scratch bytes at 0xdf0.. depend on the entry values of v11 and v15 (all lanes) - if a dead-DMEM mask removes them the model needs not reproduce them.

#### 7.2.5 Pilotwings 64 (3e9b9c3a6ebde1e0) and Wave Race 64 (fb483893c5b0ad3d)

- The relight code is the same 62-instruction body (0x80c..0x8f8): the instruction words of the whole overlay 2 (102 instructions) are identical except for branch / jump target fields (compared against the disassemblies of the PW and WR overlays: PW differs in the targets of `bltz at`, the two `bgtz` and one `beq`; WR additionally in the overlay-entry `beq zero,zero`, the second `beq`, `j 0x5e4`, `jal 0x524` and `j 0x510`). [read]
  So every DMEM address (0x12c, 0x360/0x380, 0x1b8/0x1c0, 0x208, 0xa0, 0xde0, scratch) and every constant is identical.
- Placement: overlay 2 at IMEM 0x768 (SM64), 0x788 (PW, +0x20), 0x7c8 (WR, +0x60). Relight entry (`bltz` target): 0x80c / 0x82c / 0x86c; lit loop entry (where `jr ra` returns): 0x774 / 0x794 / 0x7d4; so D16[0xa0] = 0x1774 / 0x1794 / 0x17d4 (dumps). `jal 0x524` -> 0x524 (SM64, PW) / 0x584 (WR); `j 0x510` -> 0x510 / 0x510 / 0x570. The shared load routine (0x510..0x554, 18 instructions) is word-identical (PW at 0x510, WR at 0x570). [read]
- v30 / v31 constants (DMEM 0x30 / 0x40) and the clamp constants (0x208) are identical in the three dumps. [verified from the dumps]
- The model passes unchanged on PW and WR: overlay 2 loaded at its descriptor address, entry = base + 0xa4, return address = base + 0xc: 120000 cases, 0 failures. [verified]
- Differences outside the routine (other parts of the spec): the PW dump shows D32[0x12c] = 0xff000040 (flag byte 0xff rather than only bit 31; the relight masks it away anyway); WR sets the stale flag in POPMTX as well (10.1); WR's main text sits 0x60 further, so the lit tail is 0x638 instead of 0x5e4.

#### 7.2.6 What could not be determined / caveats

- Bit-exactness is against the fork's JIT (= its C VU helpers), not against hardware; the VU helpers are the standard ones, and nothing in the routine depends on documented-differing corner cases, but this was not cross-checked on a second emulator.
- The model is exact for any W with `(W & 0xfff) <= 0xe00`, any alignment of `at`, any DMEM content and any matrix; it assumes sp = 0x110 and the standard v30/v31. It does not model wrap across the DMEM end (JIT disagrees there).
- The meaning of byte 3 (leftover-driven) was not traced to any consumer; lit math ignores it [read].
- Instruction timing / the RSP pipeline are not modelled (irrelevant for the result).

#### 7.2.7 Appendix: reciprocal ROM (indices 512..1023 of rsp_reciprocal_rom, i.e. `ROM[idx - 512]` for idx in 0x200..0x3ff)

```c
static const uint16_t RELIGHT_RSQ_ROM[512] = { /* rsp_reciprocal_rom[512..1023], the only half VRSQ/VRSQL index */
    0x6A09, 0xFFFF, 0x6955, 0xFF00, 0x68A1, 0xFE02, 0x67EF, 0xFD06, 0x673E, 0xFC0B, 0x668D, 0xFB12,
    0x65DE, 0xFA1A, 0x6530, 0xF923, 0x6482, 0xF82E, 0x63D6, 0xF73B, 0x632B, 0xF648, 0x6280, 0xF557,
    0x61D7, 0xF467, 0x612E, 0xF379, 0x6087, 0xF28C, 0x5FE0, 0xF1A0, 0x5F3A, 0xF0B6, 0x5E95, 0xEFCD,
    0x5DF1, 0xEEE5, 0x5D4E, 0xEDFF, 0x5CAC, 0xED19, 0x5C0B, 0xEC35, 0x5B6B, 0xEB52, 0x5ACB, 0xEA71,
    0x5A2C, 0xE990, 0x598F, 0xE8B1, 0x58F2, 0xE7D3, 0x5855, 0xE6F6, 0x57BA, 0xE61B, 0x5720, 0xE540,
    0x5686, 0xE467, 0x55ED, 0xE38E, 0x5555, 0xE2B7, 0x54BE, 0xE1E1, 0x5427, 0xE10D, 0x5391, 0xE039,
    0x52FC, 0xDF66, 0x5268, 0xDE94, 0x51D5, 0xDDC4, 0x5142, 0xDCF4, 0x50B0, 0xDC26, 0x501F, 0xDB59,
    0x4F8E, 0xDA8C, 0x4EFE, 0xD9C1, 0x4E6F, 0xD8F7, 0x4DE1, 0xD82D, 0x4D53, 0xD765, 0x4CC6, 0xD69E,
    0x4C3A, 0xD5D7, 0x4BAF, 0xD512, 0x4B24, 0xD44E, 0x4A9A, 0xD38A, 0x4A10, 0xD2C8, 0x4987, 0xD206,
    0x48FF, 0xD146, 0x4878, 0xD086, 0x47F1, 0xCFC7, 0x476B, 0xCF0A, 0x46E5, 0xCE4D, 0x4660, 0xCD91,
    0x45DC, 0xCCD6, 0x4558, 0xCC1B, 0x44D5, 0xCB62, 0x4453, 0xCAA9, 0x43D1, 0xC9F2, 0x434F, 0xC93B,
    0x42CF, 0xC885, 0x424F, 0xC7D0, 0x41CF, 0xC71C, 0x4151, 0xC669, 0x40D2, 0xC5B6, 0x4055, 0xC504,
    0x3FD8, 0xC453, 0x3F5B, 0xC3A3, 0x3EDF, 0xC2F4, 0x3E64, 0xC245, 0x3DE9, 0xC198, 0x3D6E, 0xC0EB,
    0x3CF5, 0xC03F, 0x3C7C, 0xBF93, 0x3C03, 0xBEE9, 0x3B8B, 0xBE3F, 0x3B13, 0xBD96, 0x3A9C, 0xBCED,
    0x3A26, 0xBC46, 0x39B0, 0xBB9F, 0x393A, 0xBAF8, 0x38C5, 0xBA53, 0x3851, 0xB9AE, 0x37DD, 0xB90A,
    0x3769, 0xB867, 0x36F6, 0xB7C5, 0x3684, 0xB723, 0x3612, 0xB681, 0x35A0, 0xB5E1, 0x352F, 0xB541,
    0x34BF, 0xB4A2, 0x344F, 0xB404, 0x33DF, 0xB366, 0x3370, 0xB2C9, 0x3302, 0xB22C, 0x3293, 0xB191,
    0x3226, 0xB0F5, 0x31B9, 0xB05B, 0x314C, 0xAFC1, 0x30DF, 0xAF28, 0x3074, 0xAE8F, 0x3008, 0xADF7,
    0x2F9D, 0xAD60, 0x2F33, 0xACC9, 0x2EC8, 0xAC33, 0x2E5F, 0xAB9E, 0x2DF6, 0xAB09, 0x2D8D, 0xAA75,
    0x2D24, 0xA9E1, 0x2CBC, 0xA94E, 0x2C55, 0xA8BC, 0x2BEE, 0xA82A, 0x2B87, 0xA799, 0x2B21, 0xA708,
    0x2ABB, 0xA678, 0x2A55, 0xA5E8, 0x29F0, 0xA559, 0x298B, 0xA4CB, 0x2927, 0xA43D, 0x28C3, 0xA3B0,
    0x2860, 0xA323, 0x27FD, 0xA297, 0x279A, 0xA20B, 0x2738, 0xA180, 0x26D6, 0xA0F6, 0x2674, 0xA06C,
    0x2613, 0x9FE2, 0x25B2, 0x9F59, 0x2552, 0x9ED1, 0x24F2, 0x9E49, 0x2492, 0x9DC2, 0x2432, 0x9D3B,
    0x23D3, 0x9CB4, 0x2375, 0x9C2F, 0x2317, 0x9BA9, 0x22B9, 0x9B25, 0x225B, 0x9AA0, 0x21FE, 0x9A1C,
    0x21A1, 0x9999, 0x2145, 0x9916, 0x20E8, 0x9894, 0x208D, 0x9812, 0x2031, 0x9791, 0x1FD6, 0x9710,
    0x1F7B, 0x968F, 0x1F21, 0x960F, 0x1EC7, 0x9590, 0x1E6D, 0x9511, 0x1E13, 0x9492, 0x1DBA, 0x9414,
    0x1D61, 0x9397, 0x1D09, 0x931A, 0x1CB1, 0x929D, 0x1C59, 0x9221, 0x1C01, 0x91A5, 0x1BAA, 0x9129,
    0x1B53, 0x90AF, 0x1AFC, 0x9034, 0x1AA6, 0x8FBA, 0x1A50, 0x8F40, 0x19FA, 0x8EC7, 0x19A5, 0x8E4F,
    0x1950, 0x8DD6, 0x18FB, 0x8D5E, 0x18A7, 0x8CE7, 0x1853, 0x8C70, 0x17FF, 0x8BF9, 0x17AB, 0x8B83,
    0x1758, 0x8B0D, 0x1705, 0x8A98, 0x16B2, 0x8A23, 0x1660, 0x89AE, 0x160D, 0x893A, 0x15BC, 0x88C6,
    0x156A, 0x8853, 0x1519, 0x87E0, 0x14C8, 0x876D, 0x1477, 0x86FB, 0x1426, 0x8689, 0x13D6, 0x8618,
    0x1386, 0x85A7, 0x1337, 0x8536, 0x12E7, 0x84C6, 0x1298, 0x8456, 0x1249, 0x83E7, 0x11FB, 0x8377,
    0x11AC, 0x8309, 0x115E, 0x829A, 0x1111, 0x822C, 0x10C3, 0x81BF, 0x1076, 0x8151, 0x1029, 0x80E4,
    0x0FDC, 0x8078, 0x0F8F, 0x800C, 0x0F43, 0x7FA0, 0x0EF7, 0x7F34, 0x0EAB, 0x7EC9, 0x0E60, 0x7E5E,
    0x0E15, 0x7DF4, 0x0DCA, 0x7D8A, 0x0D7F, 0x7D20, 0x0D34, 0x7CB6, 0x0CEA, 0x7C4D, 0x0CA0, 0x7BE5,
    0x0C56, 0x7B7C, 0x0C0C, 0x7B14, 0x0BC3, 0x7AAC, 0x0B7A, 0x7A45, 0x0B31, 0x79DE, 0x0AE8, 0x7977,
    0x0AA0, 0x7911, 0x0A58, 0x78AB, 0x0A10, 0x7845, 0x09C8, 0x77DF, 0x0981, 0x777A, 0x0939, 0x7715,
    0x08F2, 0x76B1, 0x08AB, 0x764D, 0x0865, 0x75E9, 0x081E, 0x7585, 0x07D8, 0x7522, 0x0792, 0x74BF,
    0x074D, 0x745D, 0x0707, 0x73FA, 0x06C2, 0x7398, 0x067D, 0x7337, 0x0638, 0x72D5, 0x05F3, 0x7274,
    0x05AF, 0x7213, 0x056A, 0x71B3, 0x0526, 0x7152, 0x04E2, 0x70F2, 0x049F, 0x7093, 0x045B, 0x7033,
    0x0418, 0x6FD4, 0x03D5, 0x6F76, 0x0392, 0x6F17, 0x0350, 0x6EB9, 0x030D, 0x6E5B, 0x02CB, 0x6DFD,
    0x0289, 0x6DA0, 0x0247, 0x6D43, 0x0206, 0x6CE6, 0x01C4, 0x6C8A, 0x0183, 0x6C2D, 0x0142, 0x6BD1,
    0x0101, 0x6B76, 0x00C0, 0x6B1A, 0x0080, 0x6ABF, 0x0040, 0x6A64,
};
```

## 8. G_TRI1

Command `w0 = 0xbf000000`; the four bytes of `w1`, most significant first, are `flag, 10 i0, 10 i1, 10 i2` (`i` = vertex slot number; `10 i` times 4 is the byte offset in the 40-byte-stride buffer). The three vertex
addresses are `A = 0x420 + 4 b1`, `B = 0x420 + 4 b2`, `C = 0x420 + 4 b3`. They are stored at DMEM 0xde0, 0xde4, 0xde8 (scratch, overwritten by clipping and by MTX); `flatv = DMEM32[0xde0 + 4 flag]` is the vertex address
whose colour is used for flat shading (for `flag` 0, 1, 2 it is A, B, C). The vertex addresses may also point into the clip slots 0xde0.. when the triangle comes from the clipper (section 9).
Notation: `X(P), Y(P)` = the s16 screen x, y at `P + 24, P + 26` (10.2 fixed point pixels); `Z(P)` = `(s16 at P + 28 : u16 at P + 30)`; `CW(P)` = the clip W `(s16 at P + 6 : u16 at P + 14)`; `RW(P)` = `(s16 at P + 32 : u16 at P + 34)`;
`flags(P)` = u16 at `P + 36`; `gm` = geometry word DMEM32[0x114]. All the 16.16 pairs are `(hi:lo)`. Chains `L M N H` are the product forms of section 1; `sub32(a, b)` is the VSUBC/VSUB pair: `lo = a_lo - b_lo` (wrapping), `hi = sat16(a_hi - b_hi - borrow)`
with `borrow = [a_lo < b_lo]` and the borrow applied to `b_hi` *first* (the RSP's VSUB: `sat16(a_hi - sat16(b_hi + borrow))`, minus 1 more when the inner saturation changed the value); `sub16(a, b)` is the same on a single 16-bit lane
(`sat16(a - b)`), and `mul32(P, Q) = floor(P Q / 65536)` with the lo3 rule of section 1 (the low half read before the final H term) unless marked "full".

### 8.1 Trivial reject, clip

    if (flags(A) & flags(B) & flags(C) & 0x7070) != 0:   end of command (nothing is drawn)
    else if ((flags(A) | flags(B) | flags(C)) & 0x4343) != 0:   clip (section 9), which feeds the fan triangles to 8.2 onwards
    else continue

### 8.2 Area, sort, cull

    dyBA = sub16(Y(B), Y(A)) - c1       (c1 = the VCO carry flag of lane 1 at entry: 0 after every instruction sequence of the microcode that ends in a VSUB, which is every path, so 0 in a real run;
                                         a native implementation may require c1 = 0 or read it)
    dxCA = sub16(X(C), X(A))    dyCA = sub16(Y(C), Y(A))    dxAB = sub16(X(A), X(B))
    m1 = dxCA * dyBA    m2 = dxAB * dyCA                       (s32 products)
    area = (ahi : alo):  alo = (m1_lo + m2_lo) mod 2^16;  ahi = sat16(m1_hi + m2_hi + [m1_lo + m2_lo > 0xffff])        (the high word saturates, the low word wraps)

Sort `(P0, P1, P2) = (A, B, C)` by `Y` ascending with the bubble sequence: `repeat { if Y(P1) < Y(P0) swap(P0, P1), par ^= 1; if Y(P2) < Y(P1) swap(P1, P2), par ^= 1, continue; break }`
(strict comparisons: ties keep their order). `H = P0` (top), `M = P1`, `L = P2` (bottom). Cull tests use the area before any negation, and quit the command (no output):

    area == 0                          -> quit
    ahi >= 0  and  gm & 0x1000         -> quit   (front-face culling)
    ahi <  0  and  gm & 0x2000         -> quit   (back-face culling)

If `par` is odd the area is negated: `(ahi : alo) = S16/U16 of -(ahi : alo)` read through the accumulator (`acc = alo * (-1) + (ahi * (-1) << 16)`, `ahi = sclamp(acc)`, `alo = uclamp(acc)`; -0x8000 saturates to 0x7fff).
`flip = [ahi < 0]` after that. `R = (rhi : rlo)` is the refined reciprocal of the area, the reciprocal routine of overlay 0 (section 1.1 with `W = (ahi : alo)`; the same maths as the vertex W reciprocal).
When the command quits early (cull / zero area), the microcode has still stored the three pointers at 0xde0.. and loaded the sorted x, y into vector lanes; nothing reaches the RDP.

### 8.3 The RDP triangle header (32 bytes at DMEM 0x9e0)

    byte 0:   (gm & 0xff) | 0xc8                  (bits 0, 1, 2 of the low geometry byte are the RDP's Z-buffer 1, texture 2, shade 4 coefficient flags; the other low bits of gm should be 0)
    byte 1:   (flip ? 0x80 : 0) | DMEM8[0x122]     (DMEM8[0x122] = level << 3 | tile from G_TEXTURE)
    +2:   YL = Y(L)    +4: YM = Y(M)    +6: YH = Y(H)                (s16, 10.2)

Differences (all `sub16`): `dx21 = X(L) - X(M)`, `dy21 = Y(L) - Y(M)`, `dx10 = X(M) - X(H)`, `dy10 = Y(M) - Y(H)`, `dx20 = X(L) - X(H)`, `dy20 = Y(L) - Y(H)`.
For `e` in `{21, 10, 20}`:

    inv_e   = rcp32(sext32(dy_e))                                (VRCP on the 16-bit value sign-extended; dy = 0 gives 0x7fffffff; dy = -32768 the special case of section 1.1)
    d_e     = (inv_e * 0x20) >> 16 as a pair:  d_hi = sclamp(L(inv_lo, 0x20) + M(inv_hi, 0x20)),  d_lo = uclamp(the same)          (= inv_e / 2048)
    q_e     = dx_e / 4 as 16.16:  (q_hi : q_lo) = (dx_e >> 2 (arithmetic) : (dx_e << 14) & 0xffff)
    u_e     = (uh : ul):  acc = L(d_lo, q_lo) + M(d_hi, q_lo) + N(d_lo, q_hi) + H(d_hi, q_hi);   uh = sclamp(acc), ul = uclamp(acc)               (full, not lo3)
    s_e     = (s0 : s1):  acc = L(ul, 0x4000) + M(uh, 0x4000);   s0 = sclamp(acc), s1 = uclamp(acc)                                              (u / 4)
    slope_e = (c_e : s1_e) with c_e = clamp(s0, -0x1cd, 0x1cc)                (the VCR clamp; the low half s1 is kept unclamped)

    +8:  XL = x(M) / 4         as (X(M) >> 2 : (X(M) << 14) & 0xffff)         +12: DxLDy = slope_21 = (c_21 : s1_21)
    +16: XH = x(H) / 4 - slope_20 * fy                                         +20: DxHDy = slope_20
    +24: XM = x(H) / 4 - slope_10 * fy                                         +28: DxMDy = slope_10

with `fy = (Y(H) << 14) & 0xffff` (the fractional quarter-pixel of the top y in 0.16, as an unsigned 16-bit number) and the correction `t_e = (th : tl)`: `acc = L(s1_e & 0xfff8, fy) + M(c_e, fy)`, `th = sclamp(acc)`, `tl = uclamp(acc)`
(so the *low half of the slope is masked with 0xfff8 here only*); the start x is `sub32((X(H) >> 2 : (X(H) << 14) & 0xffff), t_e)`. [verified: `src/native/f3d_tri.c`]

### 8.4 Texture scalers (when texture, bit 2 of byte 0 of the command)

Let `pn` = u16 at DMEM 0x112. For each of the three vertices `P` (the original order A, B, C; the result does not depend on order): `Wn(P) = (CW(P) * pn) >> 16` as `(hi : lo) = (sclamp, uclamp)` of `L(CW_lo, pn) + M(CW_hi, pn)`.
`Wmin` = the smallest of the three as signed 32-bit numbers; `m2 = Wmin >> 1` (arithmetic shift of the 32-bit value), stored as the halfwords `m2_hi` at DMEM 0x8e0 + 68 and `m2_lo` at 0x8e0 + 76.
For each sorted vertex `P` in `H, M, L` order:

    f_P  = mul32(RW(P), m2)           (lo3 rule)                        (1/W' times min W / 2: about 0.5 for the nearest vertex)
    s'_P = (s16 s at P + 20) * f_P        t'_P = (s16 t at P + 22) * f_P          w'_P = 0x7fff * f_P        each as a pair: acc = M(v, f_lo) + H(v, f_hi), hi = sclamp(acc), lo = uclamp(acc)

The six pairs are written to DMEM scratch (halfwords): vertex k (0 = H, 1 = M, 2 = L), hi words of `s', t', w'` at `0x8f0 + 16k + {0, 2, 4}` and lo words at `0x8f8 + 16k + {0, 2, 4}`. The fourth halfword of each group (offsets +6) is a
by-product of unrelated stale vector lanes (see section 11) and is not used.

### 8.5 Attribute blocks

Vertex attribute vectors (8 lanes; lane value = `(hi : lo)` pair, per sorted vertex `k`): lanes 0-3 colour r, g, b, a = `(byte : 0x8000)` (byte = the u8 at `Pc + 16 + i`, `Pc = P_k` when `gm & 0x200` (smooth) else `flatv`);
lanes 4, 5, 6 = `(s'_k, t'_k, w'_k)` of 8.4 (0x8f0 area; only meaningful with texture on); lane 7 = `Z(P_k)`. Edge vectors: `D10 = V1 - V0`, `D02 = V0 - V2`, `D20 = V2 - V0`, `D01 = V0 - V1` (`sub32` lane-wise), and `v4s_e = sat16(4 * dx_e)` / `sat16(4 * dy_e)`:

    numX = D20 * v4s(dy10) + D01 * v4s(dy20)        (exact: acc = N(D20_lo, c) + H(D20_hi, c) + N(D01_lo, c') + H(D01_hi, c'); numX = (low 16 of acc >> 32 : low 16 of acc >> 16), i.e. acc >> 16 as a 32-bit value, no clamp)
    numY = D10 * v4s(dx20) + D02 * v4s(dx10)        (same form)
    gX   = mul32(numX, R)    gY = mul32(numY, R)                       (lo3 rule)
    gE   = (gY + gX * slope_20) with acc = gY_lo * 1 + (gY_hi * 1 << 16) + L(gX_lo, s1_20) + M(gX_hi, s1_20) + N(gX_lo, c_20); gE_lo = uclamp(acc) (lo3: before the H term); acc += H(gX_hi, c_20); gE_hi = sclamp(acc)
                                                                       (slope_20 = (c_20 : s1_20) with the *unmasked* s1_20)
    t    = (th : tl) with acc = L(gE_lo, fy) + M(gE_hi, fy), th = sclamp, tl = uclamp         init = sub32(V0, t)           (V0 = the lane values of the top vertex H)

Shade block (present when byte 0 bit 2; lanes 0-3, 64 bytes, as 16-bit words, row by row):

    +0:  init_hi[0..3]    +8:  gX_hi[0..3]    +16: init_lo[0..3]    +24: gX_lo[0..3]
    +32: gE_hi[0..3]      +40: gY_hi[0..3]    +48: gE_lo[0..3]      +56: gY_lo[0..3]

Texture block (present when bit 1; 64 bytes, same layout; lanes 4, 5, 6, 7; lane 7's words are meaningless and ignored by the RDP) - the values are scaled by `K`:

    Per lane j = 4..7:  A_j = |gX_hi[j]| * 2048 / 65536   (as a pair: M(|gX_hi|, 2048) read through sclamp / uclamp), B_j = |gY_hi[j]| * 2048 / 65536,
                        C_j = (cmag_j * 2048) >> 16 with cmag_j the magnitude measure below (pair (ch : cl) from the scratch 0x8e0 + 64 + 2 jj (hi) and 0x8e0 + 72 + 2 jj (lo), jj = j - 4):
                              s: the larger |s'| of the three vertices (hi word, with its lo word); t: the same for t'; w: m2 (the stored halfwords at +68 / +76); lane 7: stale
                        X_j = 2 A_j + B_j + C_j:  acc = N(A_lo, 2) + H(A_hi, 2) + N(B_lo, 1) + H(B_hi, 1) + N(C_lo, 1);  X_lo = uclamp(acc) (before C_hi);  acc += H(C_hi, 1);  X_hi = sclamp(acc)
                    (the magnitude measure for s: of |s'_0|, |s'_1|, |s'_2| (the hi words, saturating abs) take the one with the largest hi word (ties: the earlier one) and its lo word)
    Xmax = max(X_4, X_5, X_6) as signed 32-bit numbers      S = (Sh : Sl) = L(Xmax_lo, 64) + M(Xmax_hi, 64)    F = rcp32(S as a signed 32-bit number)
    K = 2 F: acc = N(F_lo, 2); K_lo = low 16 bits of acc; K_hi = sclamp(acc + H(F_hi, 2));  if K_hi >= 1 (signed) then K = 1.0 = (1 : 0)
    block words (lanes 4..7): init = mul32(init, K), dX = mul32(gX, K), dY = mul32(gY, K), dE = mul32(gE, K)       (lo3 rule; rows as above: +0 init_hi, +8 dX_hi, +16 init_lo, +24 dX_lo, +32 dE_hi, +40 dY_hi, +48 dE_lo, +56 dY_lo)

Z block (present when bit 0; 16 bytes), lane 7 of the *unscaled* values: each value `v` is first multiplied by 32: `v32 = (sclamp(acc), uclamp(acc))` with `acc = N(v_lo, 0x20) + H(v_hi, 0x20)`:

    zi  = v32(Z(H))   dzdx = v32(gX[7])   dzde = v32(gE[7])   dzdy = v32(gY[7])          (gX[7], gY[7], gE[7] are lane 7 of 8.5)
    if c = DMEM8[0x121] != 0:  dzdy is replaced by +-c.0 when its magnitude is below c: with lim = c * 65536 (as the 32-bit value), dzdy := (dzdy >= 0 ? lim : -lim) when -lim < dzdy < lim, else unchanged
                              (a minimum magnitude of the z gradient in y: VCH + VCL against -c with a zero low word; a zero gradient becomes +c) [verified: exhaustive over the high word -400..400, four low words, five c]
    zinit = sub32(zi, t_z)     with t_z = (th : tl), acc = L(dzde_lo, fy) + M(dzde_hi, fy)
    words: +0 zinit_hi, +2 zinit_lo, +4 dzdx_hi, +6 dzdx_lo, +8 dzde_hi, +10 dzde_lo, +12 dzdy_hi, +14 dzdy_lo

The command is the header followed by the shade, texture and z blocks that are enabled (in that order); its length is `32 + 64 s + 64 t + 16 z` bytes.

### 8.6 Flush to the RDP FIFO

As in section 4.2: the bytes `[0x9e0, 0x9e0 + len)` are DMA-written to RDRAM at the FIFO write pointer `W = DMEM32[0x128]`, `W += len`, `DPC_END := W`; if `W + len` would pass the FIFO end `DMEM32[0x154]`, the wrap
sequence of 4.2 runs first. Before it the microcode waits for the RDP not to overtake its writes (4.2).

### 8.7 State the command leaves

DMEM: 0xde0, 0xde4, 0xde8 = the three vertex addresses; `0x8e0 + 0 .. 8` = the sorted addresses `L, M, H`; the texture scratch of 8.4 and `0x8e0 + 64..79` (the magnitude measures; with texture off, the area products instead);
DMEM 0x128 (write pointer) advanced; the unflushed part of the output buffer beyond `len` holds gradient values that were computed and not used (e.g. the shade rows when only texture is on are overwritten, a z-only
triangle leaves the gradient rows beyond byte 16). Not an RDP-visible effect.
Registers at the command loop: `at = H`, `v1 = L`, `a0 = flatv`, `a1 = byte 0 (sign extended)`, `a2 = byte 1`, `s3 = old write pointer`, `s7 = 0x9e0`, `s8 = 0xa8`; vector registers: only v2[0] (= `fy`), v5[7], v6[7], v7, v8, v13, v14, v15
carry anything the next command reads, and then only through DMEM scratch stores that are leftovers (section 11).

## 9. Clipping (overlay 1) and the hand-off to triangle setup

Scope: G_TRI1 clip decision, the clip overlay (IMEM 0x768..0x987, SM64 build), the new-vertex arithmetic,
the polygon-to-triangle fan, and the state left behind. All addresses are for the SM64 build unless stated.
Reference implementation of everything below (bit-exact against the JIT, see "Evidence"):
the clipper model (section 13) (functions `model_clip`, `new_vertex`, `tail`).

Tags: [verified] = confirmed by differential experiment (model vs. the parallel-rsp JIT running the real
microcode, tens of thousands of random cases, K = 1, 2, random; random flag patterns; polygons of 3..9
vertices); [read] = from the disassembly only.


### 9.0 Notation

- Clip coordinate lane k (k = 0..3 = X, Y, Z, W) of a vertex record V at DMEM address `a`:
  `V_k = 65536 * hi + lo` with `hi = s16 at a+2k`, `lo = u16 at a+8+2k` (16.16, raw value is an s32).
- `floor` is mathematical floor; `sat16(x)` clamps to [-32768, 32767].
- `mulh(A, B) = floor(A*B / 65536)` for s32 A, B (B = a u16 factor such as pn or R16 is taken unsigned) (the VU sequence VMUDL/VMADM/VMADN/VMADH computes exactly this
  sum of partial products in a 48-bit accumulator). It is exact while the result fits in s32; outside that range
  the two halves of the result saturate independently (hi: sat16 of acc>>16; lo: 0 or 0xffff by sign, "uclamp").
  The model implements this; a native path should do the same or decline when |result| >= 2^31. [verified]
- `recip(X)` = the reciprocal routine at IMEM 0 (overlay 0, also used by G_VTX): ROM estimate of 1/X, doubled,
  then one Newton step R' = R*(2 - R*X) in 16.16. Input and output are 32-bit (hi:lo). [verified; same as
  `recip_w` in src/native/f3d_vu.h]
- Vertex buffer: record `i` at `0x420 + 40*i` (16 records, 0x420..0x69f). Record layout (all big-endian):

| off | size | field |
|---|---|---|
| +0 | 4 x s16 | clip X, Y, Z, W integer parts |
| +8 | 4 x u16 | clip X, Y, Z, W fractions |
| +16 | 4 x u8 | colour R, G, B, A (or lit colour; alpha replaced by fog when G_FOG) |
| +20 | 2 x s16 | s, t (already scaled by the texture scale) |
| +24 | 3 x s16 | screen x, y, z |
| +30 | u16 | screen z fraction |
| +32 | u16, u16 | 1/W' integer, fraction (W' = W * persp-norm) |
| +36 | u16 | clip flags F |
| +38 | 2 B | padding (G_VTX writes scratch there for the second vertex of a pair; the clipper never writes it) |

### 9.1 When clipping happens

#### 9.1.1 Clip flags F (written by G_VTX, and by the clipper for new vertices) [verified]

Let `K = s16 at DMEM[0x76]` (clip ratio, default 1; gSPClipRatio = G_MOVEWORD index 4, see 9.2.1), X_k the raw s32
of lane k = 0,1,2 (x, y, z) and W the raw s32 of lane 3. Define two comparison widths:

- `W1 = W`
- `W2 = (hi2, lo2)` with `lk = lo(W) * K` (u16 times s16, a signed 32-bit product), `hi2 = sat16(hi(W)*K + floor(lk/65536))`,
  `lo2 = lk mod 65536` (the "K-scaled copy" of W; for K=1 it is W itself). Note only the integer half saturates.

For `k = 0,1,2` and `Wj in {W1, W2}`: `le_j,k = [X_k <= -Wj]`, `ge_j,k = [X_k >= Wj]` (exact 32-bit comparison, -Wj
taken without wrap), with one exception: `le` is 0 when both X_k = 0 and Wj = 0 (VCH/VCL behaviour; seen with an all-zero matrix, where
G_VTX records flags 0x7700, not 0x7777).

```
F = sum over k=0..2 of ( le2_k << k  |  le1_k << (4+k)  |  ge2_k << (8+k)  |  ge1_k << (12+k) )
```

| bits | meaning |
|---|---|
| 0,1,2 / 8,9,10 | x,y,z <= -K*W  /  x,y,z >= K*W   (guard-band compare, "2") |
| 4,5,6 / 12,13,14 | x,y,z <= -W  /  x,y,z >= W   (exact frustum, "1") |

Bit 3, 7, 11, 15 are always 0. Only these bits are used by the clipper and the triangle test: x,y use the "2"
compare (bits 0,1,8,9), z uses the "1" compare (bits 6, 14).

#### 9.1.2 Decision in G_TRI1 (IMEM 0x998..0x9c0) [verified, 12k random triangles, including random flags]

With `F0, F1, F2` the flags of the three vertices (any order):

```
if ((F0 & F1 & F2) & 0x7070) != 0        -> trivial reject: nothing is drawn, jump to command loop 0xa8
else if ((F0 | F1 | F2) & 0x4343) != 0   -> clip (branch to 0x768; branch delay slot 0x9c4 is a harmless vector load)
else                                      -> trivial accept: continue at 0x9c4 with the original triangle
```

0x7070 = the six exact-frustum bits 4,5,6,12,13,14 (a triangle wholly outside one side of any of x, y, z against
+-W). 0x4343 = bits 0,1,8,9 (x, y outside +-K*W) and 6, 14 (z outside +-W). So with K > 1 a vertex between W and K*W
in x or y needs no clipping (guard band), z never has a guard band. With K = 1 reject and clip bits coincide in
x,y,z, so clipping happens exactly for triangles that cross a plane and are not rejected.

Tri prologue (0x20c..0x24c): the three index bytes at -3,-2,-1(k1) are `idx*10`; vertex address = `0x420 + 4*byte`
(registers at, v0, v1 = first, second, third vertex). They are also stored to DMEM[0xde0],[0xde4],[0xde8], and
`a0 = DMEM[0xde0 + 4*flagbyte]` (flagbyte = byte -4(k1) = the flat-shading vertex selector) is read before the
clipper runs; a0 is not touched by the clipper, so flat shading of the pieces uses the colour of that original
vertex [read].

#### 9.1.3 Entry state of the overlay

Registers: `at`, `v0`, `v1` = vertex addresses (first, second, third). Everything else is rebuilt. RA is not needed.
Resident requirements: overlay 0 (reciprocal) at IMEM 0..0x87; vector constants v30, v31 loaded (DMEM[0x40..0x4f],
[0x30..0x3f]); DMEM tables below; `sp = 0x110`.

DMEM data the clipper reads (SM64 values, all set at task start; none is written by the clipper) [verified]:

| DMEM | content |
|---|---|
| 0xa6 + a2, a2=2..12 | flag mask per plane: 0xa8:0x0001, 0xaa:0x0002, 0xac:0x0100, 0xae:0x0200, 0xb0:0x4000, 0xb2:0x0040 |
| 0x70 + 8*j | 6 plane coefficient vectors, 4 x s16 (cx,cy,cz,cw): see 9.2.1 |
| 0xf6..0xfc | event jump table: 0x17bc (rewind), 0x17c0 (restart output), 0x1818 (enter handler), 0x182c (leave handler); the machine of 9.2.2 |
| 0x60..0x6f | eight x 0x0002 (constant 2.0 for the Newton step in the reciprocal routine, loaded into v23) |
| 0xa0 / 0x100 / 0x102 | 0x1774 (reload-lighting return) / 0x1784 (clip entry after load) / 0x17a0 (plane-loop head) |
| 0xfe | 0x1964 = fan loop address (loaded into s8 at the end, 3) |
| 0x28..0x2f | screen clamps: x,y in [0xf006, 0x0ffa] = [-4090, 4090]; z in [0, 0x7fff] |
| 0x50..0x5f | 1,-1,1,1,1,-1,1,1 (viewport sign vector; y scale is negated) |
| 0x112 / 0x114 | persp-norm u16 pn / geometry mode word (fog = bit 16, i.e. high half bit 0) |
| 0x320 / 0x328 / 0x330 | viewport scale / translate (4 x s16 each) / fog (mult f0, offset f1, max f2) |

#### 9.1.4 Loading the overlay [verified]

Overlays 1, 2, 3 all live at IMEM 0x768. Descriptors (8 bytes each) at DMEM 0x08 (ov0), 0x10 (ov1), 0x18 (ov2),
0x20 (ov3): `{ u32 RDRAM address (relocated at task start by the ucode text base), u16 length-1, u16 IMEM address
with bit 12 set }`. SM64: ov0 text+0xf60, len 0x88, IMEM 0x000; **ov1 (clip) text+0xfe8, length 0x220 (544 bytes), IMEM 0x768**;
ov2 (lighting) text+0x1208, 0x198 bytes; ov3 (task end / yield) text+0x13a0, 0x68 bytes.

Who is resident when G_TRI1 branches to 0x768:

| resident | behaviour at 0x768 |
|---|---|
| ov1 | `beq -> 0x784` (delay slot stores RA to DMEM[0x158], never read): clip starts at once |
| ov2 (normal after a lit G_VTX) | stub: `s8 = 0x10; s5 = DMEM[0x100] = 0x784; DMA ov1 -> IMEM 0x768; wait for DMA idle; jr s5` (0xfc..0x118). at/v0/v1 survive. |
| ov3 (only before the first overlay swap) | the end-of-task code runs: the task halts. Cannot happen after any lit G_VTX. [verified: the JIT reaches `break` at 0x780] |

Afterwards ov1 stays resident until a lit G_VTX (0x5dc `bne -> 0x774`) lands in ov1's stub at 0x778
(`s8 = 0x18; s5 = DMEM[0xa0] = 0x774;` same DMA code) which loads ov2 (408 bytes) and resumes at 0x774. Each swap
waits for the DMA to finish (SP_DMA_BUSY poll); there is no other side effect. [verified]

### 9.2 The clip algorithm

#### 9.2.1 Planes, in processing order [verified]

Polygon is clipped against 6 planes, in this order (a2 is the loop counter, 12 down to 2, step 2; the mask index is
a2, the coefficient index is a2-2):

| # | a2 | flag mask m | "outside" means | coeff at | c = (cx,cy,cz,cw) | signed distance d(V) = c . V | inside iff |
|---|---|---|---|---|---|---|---|
| 1 | 12 | 0x0040 | z <= -W | 0x98 | (0,0,1,1) | z + W | d > 0 |
| 2 | 10 | 0x4000 | z >= W | 0x90 | (0,0,1,-1) | z - W | d < 0 |
| 3 | 8 | 0x0200 | y >= K*W | 0x88 | (0,1,0,-K3) | y - K3*W | d < 0 |
| 4 | 6 | 0x0100 | x >= K*W | 0x80 | (1,0,0,-K4) | x - K4*W | d < 0 |
| 5 | 4 | 0x0002 | y <= -K*W | 0x78 | (0,1,0,K5) | y + K5*W | d > 0 |
| 6 | 2 | 0x0001 | x <= -K*W | 0x70 | (1,0,0,K6) | x + K6*W | d > 0 |

The W coefficients are the halfwords at 0x8e, 0x86, 0x7e, 0x76 (K3, K4, K5, K6). gSPClipRatio(r) is four
G_MOVEWORD (index 4) commands with offsets 0x1c, 0x14, 0x0c, 0x04 and data -r, -r, r, r: each stores the 32-bit
data word at DMEM[0x70 + offset] (high half = cz, low half = cw). [verified with the JIT]. The vertex flags
(9.1.1) use only DMEM[0x76] for all four x/y compares; this dump has r = 1. Coefficients are used as s16; -c is
clamped to +32767 for c = -32768. If the K_i differ from each other the flags and the plane equations disagree.

"Inside" of a vertex for a plane is decided only by the stored flags: inside iff `(F & m) == 0`, not by d.
Because the flags use "<=" / ">=", a vertex with d = 0 is "outside". [verified]

#### 9.2.2 Polygon representation and per-plane list operation

A polygon is a list of 16-bit vertex addresses terminated by a 0 halfword, in a 10-halfword buffer: buffer A at
DMEM 0x940, buffer B at 0x954. Initial list (written at 0x784) is **reversed**: `[third, second, first, 0]` at 0x940.
Input buffer = the buffer holding the current polygon, output = the other one. The buffers swap only when a plane
actually cuts. Max polygon = 3 + 6 = 9 vertices + terminator = exactly 10 halfwords. [verified]

Let the current list be V_0..V_{n-1}, `out_i = ((F(V_i) & m) != 0)`:

```
all out_i = 0   -> plane is a no-op; list unchanged, buffers NOT swapped.                       [verified]
all out_i = 1   -> ABORT: the whole triangle is discarded; jump 0x980 -> 0xa8. New vertices already
                   made stay in DMEM, nothing is drawn.                                         [verified]
otherwise:  o = smallest i with out_i = 1                       (first outside vertex in list order)
            j = first index after o (cyclic, i.e. o+1, o+2, ... mod n) with out_j = 0   (entering vertex)
            k = first index after j (cyclic) with out_k = 1                              (leaving vertex)
            new list = [ N(P=V_{j-1}, Q=V_j) , V_j, V_{j+1}, ... , V_{k-1} , N(P=V_k, Q=V_{k-1}) , 0 ]
```

(`N(P, Q)` = new vertex, 2.3; P is always the outside vertex and Q the inside one; indices mod n.) The entering
intersection gets the lower new-vertex slot. For a convex polygon (one outside run) this is plain Sutherland-Hodgman
with the entering point first. If the outside vertices are not one cyclic run (only possible with inconsistent
flags or rounding), only the first run after `o` is processed; the other vertices are dropped. [verified: the
microcode's state machine (s1 event counter, s2 expected state) was modelled literally and gives the same lists
as these rules on all 1016 inside/outside patterns of n = 3..9 (a script), and the machine matched the JIT on
40000 random direct-mode polygons with random flag bits incl. non-convex ones]

New-vertex slots: `a3` starts at 0xdb8, `a3 += 40` before each creation: slot s lives at `0xde0 + 40*s`, s = 0..11;
two per cutting plane, so at most 12 (0xde0..0xfbf). There is no bounds check and none is needed
(6 planes x 2). The scheme reuses slots for every triangle (a3 is reset at 0x794). Original vertex records are
never modified. [verified]

#### 9.2.3 New vertex N(P, Q) on plane c

Inputs: P (outside), Q (inside) records, plane c. Outputs below are exact (steps S1..S6 are fixed-point; "raw"
values are s32 16.16 numbers; all 32-bit sums are exact when they do not overflow, else per-half saturation as in 0).

```
S1  dP = c . P ,  dQ = c . Q                (lane products c_k*V_k accumulated in 48 bits, each lane result
                                              split into (hi=sat16(acc>>16), lo=uclamp(acc)); the 4 lanes are summed
                                              pairwise (lane0+1, lane2+3) then together with VADDC/VADD: lo wraps with
                                              carry, hi saturates)
    D  = c . (Q - P)  ( = dQ - dP, computed independently as sum c_k*(Q_k - P_k) with the same split-add rules )
S2  R  = rcp32(D)            ROM reciprocal estimate of the 32-bit raw D (sign-aware; D = 0 gives 0x7fffffff)
    R2 = ( hi = sat16(floor(2R / 65536)) , lo = (2 * lo16(R)) mod 65536 )   # NOT the lo half of the saturated 2R
    if hi16(D) < 0 : R2 = 32-bit two's-complement negate of R2 (hi saturating)
    R16 = (R2.hi == 0) ? R2.lo : 0xffff          # ~ min(0xffff, 2^32/|D_raw|) = 1/|D| as a 0.16 fraction
S3  X  = mulh(D, R16)        ( ~ +-1.0 in 16.16 : the correction factor D * R16 )
S4  Rx = recip(X)            ( ~ +-1.0, Newton-refined, 16.16 )
S5  W1 = mulh(dQ, Rx)        ( ~ dQ )
    u  = mulh(W1, R16) + 3   ( ~ dQ/D * 65536; the "+3" is the accumulator bias from VMUDL v31[3]*v31[13] )
S6  wP = (hi16(u) == 0) ? lo16(u) : 0xffff ;   if wP == 0 : wP = 1          # 0.16 weight of the OUTSIDE vertex
    wQ = 65536 - wP                                                         # in [1, 65535]
```

So `wP ~ dQ / (dQ - dP)` = the usual t, quantised to 16 bits, never 0 and never 1.0 (the new vertex never coincides
with P or Q). If hi16(u) != 0 (u > 1, or u negative from a flag/plane mismatch) wP = 0xffff. [verified: every new-vertex field,
so `wP` included, bit-exact over tens of thousands of new vertices with both signs of D; saturated and D = 0 inputs also matched.]

Interpolation (N = new vertex; all lanes k = 0..3 of position, then attributes). With `Vi` = hi halves (s16),
`Vf` = lo halves (u16):

```
N_raw_k = Qi_k*wQ + floor(Qf_k*wQ / 65536) + Pi_k*wP + floor(Pf_k*wP / 65536)       # 16.16, each half of the
                                                                                    # product floored SEPARATELY
(Ni_k, Nf_k) = ( sat16(N_raw_k >> 16), N_raw_k & 0xffff )          # no saturation can occur: wP + wQ = 65536
colour byte c = floor( (cP*wP + cQ*wQ) / 65536 )          for R, G, B, A  (cP, cQ = u8)
s,t           = floor( (sP*wP + sQ*wQ) / 65536 )          signed floor, s16, s and t independently
```

Note the position formula differs from `floor((P_raw*wP + Q_raw*wQ)/65536)` in the rounding of the fractional
parts (floor of each fractional product, then the integer products added). [verified]

Colour/ST come through 16-bit lanes (colour bytes are loaded as `byte<<7`, results stored `>>7`); this is exactly
the formulas above. [verified]

#### 9.2.4 Completing the record (the vertex tail, same code as G_VTX's per-vertex tail at 0x5e8)

The clipper does **not** re-run lighting, the matrix, or the texture scale. It enters the G_VTX tail with the
interpolated clip coordinates N and the interpolated colour/st:

```
+0..7    X,Y,Z,W integer parts = Ni_k            (NOT multiplied by K: the K-scaled copy is only used for flags) [verified]
+8..15   fractions = Nf_k
+16..19  colour bytes (9.2.3); if fog is on (geometry mode bit 16) byte A is replaced by the fog byte below
+20..23  s, t (9.2.3)
+24..29  screen x, y, z    }  see 9.2.5
+30      screen z fraction  }
+32..35  1/W' hi, lo       }
+36      flags F (9.1.1), computed from N and K
+38..39  not written
```

#### 9.2.5 Screen coordinates, 1/W, fog, flags (recomputed for the new vertex) [verified, field by field]

With `pn = u16 at DMEM[0x112]`, `Wn = mulh(N_W, pn)` taken as (hi = sat16(...), lo) i.e. `floor(W*pn/65536)` for the
32-bit W of N and pn as an UNSIGNED 0.16 factor (pn = 0xffff is ~1.0):

```
(rh, rl) = recip(Wn)                     # stored at +32 (hi), +34 (lo)
rhc      = (rh >= 0) ? rh : 0x7fff       # signed hi half; negative -> +0x7fff
for k = 0..3 (x, y, z, w lanes):
    S_k  = mulh(N_k, (rhc : rl))         # N * (1/Wn)
    S'_k = mulh(S_k, pn)                 # times pn again (pn^2 overall with the reciprocal of Wn)
    v_k  = (vtrans_k << 16) + vscale_k * S'_k      # vscale_k: viewport scale, y negated via the sign vector at 0x50
    screen_k = clamp( sat16(v_k >> 16), lo_k, hi_k )   # x,y: [-4090, 4090]; z: [0, 0x7fff]
    zfrac    = uclamp-low16 of v_2                      # stored at +30
```

`vscale_k = sat16(DMEM[0x320+2k] * DMEM[0x50+2k])` (the sign vector), `vtrans_k = DMEM[0x328+2k]`; screen x, y, z
at +24, +26, +28 (lane 3 is overwritten at +30 by zfrac).

Fog (geometry mode bit 16): `f = sat16(floor(Sz_raw * f0 / 65536) + f1)`, `f = max(f, 0)`, `f = min(f, f2)`;
colour alpha byte := `f & 0xff`, with `Sz_raw = S'_2` (the z/w lane just computed), f0,f1,f2 = DMEM[0x330],[0x332],[0x334]. [verified]

The same tail yields F (6.2) from N (and the K-scaled W copy).

### 9.3 From polygon to triangles

After the 6 planes (also reached when the last cutting plane was plane 6 or a plane left the polygon unchanged),
code at 0x954 [verified]:

```
DMEM[0xb4]  = poly[0]            # fan pivot (survives the triangle code's register use)
DMEM[0x106] = a1                 # cursor = address of poly[0] (a1 = base of the final buffer)
s8 = DMEM[0xfe] = 0x964          # return address for the triangle code (replaces the command-loop address)
loop (0x964):  t0 = DMEM[0x106]; v1 = DMEM[0xb4]; v0 = poly[i+1]; at = poly[i+2]; DMEM[0x106] = t0 + 2
               if at != 0:  go to 0x9c4 (triangle setup, skipping cull/clip test); on return (jr s8) repeat
               else:        j 0xa8   (command loop)
```

Triangle i (i = 0..n-3) submitted to the normal setup is `(at, v0, v1) = (poly[i+2], poly[i+1], poly[0])`, so
poly[0] (the entering intersection `N_enter`) is the common vertex of the fan and orientation is preserved (the
list is reversed relative to the original `(first, second, third)`, and so is the fan). `n - 2` triangles,
at most 7. [verified: every stop at 0x9c4 matched, incl. register order]

Each triangle is processed by exactly the unclipped path from 0x9c4 on: y sort/swap of vertices, area/orientation
(`cull front/back`), edge/shade/texture/z coefficient computation, writing the RDP command(s) to the output
buffer at s7, and the end-of-triangle `jal 0x178` (RDP FIFO flush check) before `jr s8`. No clip or flag test is
repeated for the fan triangles. A triangle whose area is zero or which is culled by facing is dropped individually.

State after the clip (observed at 0xa8, [verified]):

- Changed DMEM besides the triangle path's own scratch (0x8e2..0x92d) and RDP output: the new vertex slots (bytes
  +0..+37 of each used slot, 0xde0 + 40*s), polygon buffers 0x940..0x963 (alternating), DMEM[0xb4] (pivot),
  [0x106] (cursor; final value a1 + 2*(n-1), n = polygon size), [0x158] (stale RA, never read). Measured as the union
  of changed bytes over 6000 clipped triangles. Vertex records 0x420..0x69f, constants and tables are untouched.
- The three DMEM words [0xde0],[0xde4],[0xde8] written by the tri prologue (the original vertex addresses) are
  destroyed by slot 0 (they were already consumed, 1.2).
- Overlay 1 remains resident at 0x768. GPRs at 0xa8 after a drawn polygon: s8 = 0x1964, `at` = 0 (the terminator),
  v0/v1 = the last fan triangle's vertices; a1/a2/a3 are the triangle path's leftovers (not a stable value).
  After an ABORT: a1 = input buffer base, s8 = output buffer base (0x940/0x954), a2 = plane counter - 2 of the
  aborting plane, `at` = last vertex tested. k0/k1/gp (command pointers/count) and s7 (RDP out pointer) are updated
  only by the triangle path / command loop as usual. The vector registers hold the last triangle's scratch values
  (stale lanes are read by the next triangle, as noted in src/native/f3d.h). [verified]

### 9.4 Differences in the triangle path for a clipped triangle

1. Entry at 0x9c4 skips the flag tests (reject/clip) and `s8` holds 0x964 instead of the command-loop address, so the
   triangle returns to the fan loop; the end-of-triangle FIFO check (0x178) still runs after every triangle. [verified]
2. Vertex addresses at/v0/v1 may be in the clip slots 0xde0..0xfbf, not only in the 16-record buffer. Triangle code
   reads only the fixed record fields (+16 colour, +20 st, +24/26/28 screen, +30 zfrac), so this is transparent; screen
   x,y of new vertices are clamped to +-4090 like any vertex.
3. Flat shading (`geometry mode & 0x200 == 0`) takes the colour from `a0`, the original flag vertex chosen in the
   prologue (9.1.2), for all pieces. [read]
4. Fog alpha and colour are interpolated attributes of the new vertex, not recomputed from lighting. Lit colour is
   interpolated in the already-lit domain. [verified]
5. The new vertices' flags (+36) are never examined again after the last plane they were created in (the fan skips
   the flag tests), so pieces are drawn even if rounding put a new vertex fractionally outside its own plane. [read]

### 9.5 Per-build differences

| build | clip overlay | notes |
|---|---|---|
| SM64 (this dump) | IMEM 0x768, 544 bytes, poly buffers 0x940/0x954, exit `j 0xa8` | as above |
| Pilotwings 64 | identical code, loaded at 0x788 (+0x20) | **vertex tail differs** (0x5f0..0x630, 3+4 extra instructions): the z "<= -W" compare of the "1" group (bits 2/6 of the cc1 word) is cleared and replaced by the sign bit of the clip W (`W_hi < 0`), cull mask is 0x7030 (no z-le bit) instead of 0x7070, G_CULLDL mask 0x7030. The sign bit of the FIRST vertex of each vertex pair is routed into the second vertex's bit 2 instead of its own bit 6 (shift bug: or'ed into the group shifted by 16), so only second-of-pair vertices (and only bit 6) carry "W < 0" as flag 0x0040. Clip masks/table order are the same; the near plane's coefficient vector (DMEM 0x98) is data and not available. [read] |
| Wave Race 64 | identical algorithm, loaded at 0x7c8 (+0x60) | poly buffers at 0xbc0/0xbd4 (+0x280); the overlay saves nothing at entry but the triangle routine stores `s8` in DMEM[0xa6c] (0xa04) and the clipper's exits are `lw s8,0xa6c; jr s8` (abort and end of fan) instead of `j 0xa8`, because WR's TRI1 also serves its quad path (TRI called twice via `jal 0x9e8`). New vertex area base `a3 = 0xdb8` is unchanged although the whole data layout moved, so slots 0xde0.. may overlap WR's RDP output buffer (0xc60..) when it is more than 0x180 bytes full. Tail, masks (0x7070 / 0x4343), planes: same. No flat-shading branch in WR's setup. [read] |
| Glover | none | the supplied Glover image contains no vector compare ops (VCH/VCL/cfc2), no triangle setup (no 0xc8 command build), no overlay at 0x768, and sprite-like RDP command emitters (SetTImg, SetTile, LoadTLUT): it is a 2D/sprite microcode, not the F3D 3D path. Nothing to compare; clipping of Glover's 3D lists would be in a different image. [read] |

### 9.6 Evidence

- the clipper model and its test (built against the JIT harness; a working file of the spec's author, not in the repository):
  - realistic mode: G_VTX through the JIT with random matrices/vertices (K = 1, 2, 1..6, random s16), then G_TRI1 with
    ov1 resident; per case the model predicts (a) which of reject/clip/accept happens, (b) the final polygon list,
    (c) all new vertex bytes 0..39 of every used slot, (d) the exact sequence of (at, v0, v1) at 0x9c4.
    Runs: 3000..40000 iterations per configuration, 0 failures; 765..1500 clipped triangles per 3000, polygons of
    3..9 vertices, up to 12 new vertices.
  - direct mode (`-p`): random 3..8-vertex polygons with random flag bits, starting at 0x7a8, all planes.
  - loader mode (`-o 2`): ov2 resident, overlay descriptors/RDRAM text set up: 1138 of 1138 clipped cases loaded
    ov1 exactly (IMEM 0x768..0x987 == text image) and continued correctly.
  - flag formula (9.1.1): 36k..48k vertices per K setting, 0 mismatches. Reject/clip/accept (9.1.2): 12k triangles, 0.
  - mutation checks (each run against the JIT, 1500+ cases): bias +3 -> +4, joint instead of per-half floor of the
    position products, wQ = 0xffff - wP, round-to-nearest colour: each fails within the first cases; dropping the
    `wP >= 1` clamp fails only in direct mode over 20000 polygons (wP = 0 is rare); storing the K-scaled
    copy (the bug found in pair_scalar) fails for every K != 1.
- Cases not exercised: the lit-vertex path (clipper is independent of it), PW/WR/Glover images (code only).

## 10. Differences between builds

This section has the command-layer differences (10.1) and then, for G_VTX / G_TRI1, the parameter tables, the semantic differences and the groups of builds (10.2 to 10.9).

Scope: only the vertex command (G_VTX), the triangle command (G_TRI1, including its cull/clip decision and the RDP triangle bytes) and what they
read. Command-layer differences (MTX, MOVEMEM, DL stack, ...) are listed only where they change how VTX/TRI1 are fed or where their output goes.
"SM64" below means the reference semantics of sections 1 to 9.

Evidence tags: [verified N] = differential test on the JIT, see 0.2, N scenes, 0 unexplained differences, with the stated knobs; [model] = a closed-form
model of the difference was applied to the SM64 output and matched the other build exactly; [read] = disassembly only.

### 10.1 Differences between builds (command layer only)

| item | SM64 (94c7...) | Pilotwings 64 (3e9b...) | Wave Race 64 (fb48...) | Glover (9d0d..., "2.0H, 02-12-97") |
|---|---|---|---|---|
| version string | 2.0D 04-01-96 | same | same | 2.0H, but this dump is the Sprite2D variant (no MTX/VTX/TRI1) |
| IMEM base of overlays | 0x768 | 0x788 (main text 0x20 bytes longer: clip code partly resident) | 0x7c8 | n/a |
| loop head / refill / dispatch | 0x60 / 0xd4 / 0x9c | same | 0x64 / 0xc8 / 0x90 | 0x10c / 0x190 / 0x148 |
| class-0 dispatcher | 0x3c4 | 0x3c4 | 0x424 | 0xed0 |
| class-2 dispatcher | 0x1fc | 0x1fc | 0x1ec | 0x308 (table base 0x74) |
| RDP pass-through | 0x38c | 0x38c | 0x3ec | 0xe90 |
| flush | 0x178 | 0x178 | 0x168 | 0x288 (different code) [read] |
| MTX/MOVEMEM/VTX/DL | 3dc/558/568/734 | 3dc/558/568/754 | 43c/5b8/5c8/788 | none / none / none / ee8 |
| 0xb1..0xbf | 380 384 378 370 -- 358 348 328 2cc 2c4 2a0 288 250 30c 20c | same addresses | 3e0 3e4 3d8 3d0 1fc 3b8 3a8 388 32c 324 300 2e8 2b0 36c 278 | e84 e88 e7c -- e64 e54 e34 df4 dec dc8 db0 (bd 340 sprite draw, be 318 scale/flip, bf nop) |
| command buffer / geometry buf / output buf | 0x6a0 / 0x7e0 / 0x9e0 | same | 0x920 / 0xa60 / 0xc60 | 0x6a0 / 0x7e0 / 0x9e0.. |
| buffer end test | gp counts bytes down (gp -= 8) | same | gp = buffer END address; loop until k1 == gp; k0 += 320 per refill | as SM64 |
| G_DL push value | k0 | k0 | k0 + k1 - gp (same address of next command) | k0, depth byte at 0x15a |
| yield save | gp,k1,k0,s7 at 0x8e4.., 0x900 bytes | same | at 0xb64.., 0xb80 bytes | 0x8e4.. |
| G_POPMTX flag | none | none | stores 0xff to D8[0x12c] (lights stale) | - |
| CULLDL mask | 0xffff | 0x7030 | 0x7070 | - |
| triangle vertex index scale (w1 bytes) | x4 (10*i) | x4 | x8 (5*i) | - |
| clip-flag layout / reject masks | 0x7070 reject, 0x4343 needs clip | 0x7030 reject; extra bit from W sign (mfc2 bit 15 -> bit 2 / bit 6) | 0x7070 / 0x4343 | - |
| op 0xb5 | no-op | no-op | quad/line command: 4 vertex bytes -> two triangles through the TRI1 core | no-op |
| MOVEMEM/MOVEWORD/geometry/TEXTURE/OTHERMODE/ENDDL/segment/FIFO flush semantics | as above | identical [verified] | identical except as listed [verified] | OTHERMODE/GEOMODE/TEXTURE/MOVEWORD/DL/ENDDL as above [read] |

Harness re-runs: PW (task end/yield break at 0x7a0 because ov3 sits at 0x788): E2 segments/pass-through, E3 MTX (500), E3b POPMTX (300), E4 MOVEMEM,
E5 MOVEWORD, E6 DL stack, E7 task end + yield, E9 OTHERMODE/TEXTURE/geometry give output identical to SM64; only E10 differs (mask 0x7030 passes 3000, SM64's 0xffff fails).
WR (loop heads 0x64/0x60, command end 0x9c/0xac, task end 0xbc, init stop 0x8c8): E2, E3 MTX (400), E3b POPMTX (200, with the stale flag 0xff written
by POPMTX), E4/E5, E6 (return pointer value D32[0x336] = 0x200008 identical, recursion depth limit 10 identical), E9, E10 (mask 0x7070 passes 3000) all pass.
GL: [read] only; the GL dump is the Sprite2D microcode (op 0x06 DL handler at 0xee8 with the depth byte at D8[0x15a], MTX/MOVEMEM/VTX/POPMTX/CULLDL/TRI1 entries are no-ops; 0xbd/0xbe are sprite ops).

### 10.2 What was compared, and with what

#### 10.2.1 Builds (nine F3D images)

The Glover dump `9d0d289ffc583535` is the Sprite2D variant (no MTX/VTX/TRI1, as 10.1 says). Glover's ROM also holds a real F3D 2.0H
image; all 2.0H ROMs of the sets contain their microcode uncompressed, so the images were cut from the ROMs (z64, big endian):
text = 16 KiB from the first word (`j X; addi sp,zero,0x110`), data = 0x800 bytes whose DMEM[4..0x2f] describe the load: D32[4] = ((len-1)<<16)|0x1080
(text[0..len) -> IMEM 0x80), D32[8]/[12] ov0 (text offset, ((len-1)<<16)|0x1000), then ov1..ov3 as (offset, ((len-1)<<16)|imem). The cut images of
SM64, PW and WR reproduce the profiler dumps (IMEM identical outside the overlay window).

| label | game / version string | ROM, text offset, data offset | ucode family | note |
|---|---|---|---|---|
| sm64 | Super Mario 64, 2.0D 04-01-96 | Super Mario 64.z64 0xe6330 / 0xf4ac0 | A | reference |
| pw | Pilotwings 64, 2.0D 04-01-96 (id 3e9b..) | Pilotwings 64.z64 0x479e0 / 0x51370 | A | overlays at 0x788 |
| pwb | Pilotwings 64, second image in the same ROM | 0x465b0 / 0x50b70 | A | overlays at 0x790, XBUS (DMEM) RDP transport |
| wr | Wave Race 64, 2.0D 04-01-96 (id fb48..) | Wave Race 64 - Kawasaki Jet Ski.z64 0x8cb80 / 0xa8b10 | WR | |
| mm | Mischief Makers, 2.0H 02-12-97 | Mischief Makers.z64 0xbb6b0 / 0xef610 | A | XBUS (DMEM) RDP transport |
| tt | Tetrisphere, 2.0H 02-12-97 | Tetrisphere.z64 0xb8780 / 0xcc920 | A | |
| gl | Glover (its F3D image), 2.0H 02-12-97 | Glover.z64 0xbd710 / 0xf3640 | B | |
| ss | SpaceStation Silicon Valley, 2.0H | SpaceStation Silicon Valley.z64 0x2a8f0 / 0x38e30 | B | code = gl with other addresses |
| wo | Wipeout 64, 2.0H | Wipeout 64.z64 0x6550 / 0x88bd0 | B+ | custom additions, see 4.5 |

Families: A = SM64 layout (buffers 0x6a0/0x7e0/0x9e0, 16 vertex slots at 0x420); WR = same code body, buffers moved by +0x280; B = 2.0H re-layout
(buffers 0x9a0/0xae0/0xce0, 32 vertex slots at 0x4a0 addressed through a pointer table). Cut images: `w3/h20/<label>.{text,data,imem,dmem}`
(`mkpseudo.py`; labels there: sm64v pwv pwb wrv mm2h tt2h gl2h ss2h wo2h), disassemblies `w3/h20/dis-<label>/`.

#### 10.2.2 Method

Static: aligned instruction diffs (branch targets masked) of each image against sm64 (family A) or wr (family B): `w3/h20/d_*.txt`, `dw_*.txt`.
Dynamic: `w3/x/xb.cpp` (build: `sh w3/x/mk.sh`; needs the fork JIT objects, see 10.8). It boots each image from reset on the JIT
(OSTask in DMEM 0xfc0, entry IMEM 0x80, so the build's own init code sets all state) and runs a complete display list, the same abstract scene on two
builds: G_CLEARGEOMETRYMODE/G_SETGEOMETRYMODE, G_MOVEWORD (clip ratio words, fog, numlights, persp), G_TEXTURE, G_MOVEMEM viewport/lights/lookat, G_MTX
(MV and P, MP computed by the microcode), G_VTX (n 3..16 vertices, v0 random), 3 x G_TRI1, two RDP syncs, G_ENDDL. Compared: the 40-byte vertex records
(vertex buffer of the build) and the byte stream the RDP receives. Scenes: 80 % plausible cameras (60 % identity modelview, perspective on 70 %),
20 % wild matrices; random geometry mode (shade, texture, z, smooth, cull front/back, fog, lighting, texgen, linear), 0..7 lights, clip ratio 1..6.
Sanity: self-compare (sm64 vs sm64, gl vs gl, ...) identical; a mutated VMUDN in the test image gave 100/100 vertex differences.
Clip scenes are included unless a knob says otherwise (knob NOCLIP skips scenes in which either build's flags request clipping).

### 10.3 Build parameter tables (a)

#### 10.3.1 Code addresses (IMEM, bytes)

| item | sm64 | pw | pwb | wr | mm | tt | gl | ss | wo |
|---|---|---|---|---|---|---|---|---|---|
| command loop head / return-to-loop (idle) pc | 60 / a8 | 60 / a8 | 60 / a8 | 60 / 9c | 60 / a8 | 60 / a8 | 60 / 9c | 60 / 9c | 60 / 9c |
| first-level class table (D16) / class-0 table / class-2 table | bc / c4 / 76 | bc / c4 / 76 | bc / c4 / 76 | bc / c4 / 76 | bc / c4 / 74 | bc / c4 / 74 | b8 / c0 / 76 | b8 / c0 / 76 | b8 / c0 / 76 |
| class-0 dispatcher / class-2 dispatcher | 3c4 / 1fc | 3c4 / 1fc | 3c8 / 1f8 | 424 / 1ec | 3c4 / 1f8 | 3c0 / 1fc | 390 / 1ac | 388 / 1a4 | 3a0 / 1a4 |
| G_VTX handler (op 4, class-0 table) | 568 | 568 | 56c | 5c8 | 568 | 564 | 3a8 | 3a0 | 3b8 |
| G_TRI1 handler (op bf, class-2 table) | 20c | 20c | 208 | 278 | 208 | 20c | 1bc | 1b4 | 1b4 |
| triangle tail (first cull-flag load `lh t3,36(v1)`) | 998 | 9b8 | 9b0 | 9e8 | 988 | 988 | 940 | 938 | 96c |
| overlay window base (ov1 clip, ov2 light, ov3 end) | 768 | 788 | 790 | 7c8 | 768 | 768 | 720 | 718 | 748 |
| overlay lengths ov1 / ov2 / ov3 (bytes) | 220/198/68 | same | same | same | same | same | 220/198/70 | same | same |
| clip entry (= window base) / lighting entry (base+0xc) | 768/774 | 788/794 | 790/79c | 7c8/7d4 | 768/774 | 768/774 | 720/72c | 718/724 | 748/754 |
| RDP flush routine (called after each triangle) | 178 | 178 | 1d0 (room check 178) | 168 | 1d0 (room check 178) | 178 | 128 | 120 | 128 |
| RDP transport | RDRAM FIFO | FIFO | DMEM XBUS ring | FIFO | DMEM XBUS ring | FIFO | FIFO | FIFO | FIFO, lazy (4.5) |
| G_CULLDL flag mask (D16 immediate, not VTX/TRI) | ffff | 7030 | 7030 | 7070 | 7070 | 7070 | 7070 | 7070 | 7070 |

#### 10.3.2 DMEM (bytes). Unchanged in ALL nine builds: state block base sp = 0x110 (persp word +0/+2, geometry mode +4 = 0x114, other modes 0x118/0x11c,
G_TEXTURE words 0x120/0x124 with level byte 0x121 and tile byte 0x122, FIFO write pointer 0x128, light-count word 0x12c, matrix stack 0x130/0x134,
FIFO start/end 0x150/0x154, segment table 0x160), lookat 0x1b0 (Y) / 0x1d0 (X), light slots 0x1f0 + 0x20*i, clip ratio words 0x74/0x7c/0x84/0x8c
(c76 = D16[0x76]), limits 0x28 (0ffa f006 7fff 0000), v31 = 0x30, v30 = 0x40, sign vector 0x50, constant 2.0 at 0x60, clip plane vectors 0x70..0x9f,
plane flag masks 0xa8..0xb2 (0001 0002 0100 0200 4000 0040), vertex record layout (40 bytes, fields as in section 6), matrix format.

| item | A: sm64 pw pwb mm tt | WR | B: gl ss wo |
|---|---|---|---|
| command buffer (k1 start), 320 bytes | 0x6a0 | 0x920 | 0x9a0 |
| geometry/vertex DMA buffer (s6) | 0x7e0 | 0xa60 | 0xae0 |
| RDP output buffer start (s7) | 0x9e0 | 0xc60 | 0xce0 |
| vertex buffer, stride 40 | 0x420, 16 slots | 0x420, 16 slots | 0x4a0, 32 slots; slot pointer = D16[0x31e + 2*slot] (0x4a0 + 40*slot) |
| viewport (vscale s16[4], vtrans s16[4]) | 0x320 | 0x320 | 0x380 |
| modelview (int rows, frac at +0x20) / projection / MP | 0x360 / 0x3a0 / 0x3e0 | same | 0x3e0 / 0x420 / 0x460 |
| fog vector (fm, fo, max 0x00ff, 0) | 0x330 | 0x330 | 0x390 |
| matrix stack limit D32[0x15c] | base + 0x280 | same | base + 0x400 |
| triangle scratch block (t0) | 0x8e0 | 0xb60 | 0xbe0 (also the 3 saved vertex pointers, halfwords) |
| triangle vertex-pointer save (flat-shading vertex pick) | words 0xde0,0xde4,0xde8 | none in TRI1 (the quad path saves at 0xa60..) | halfwords 0xbe0,0xbe2,0xbe4 (sort input) |
| clip polygon buffers (s8) / new vertex area | 0x940, 0x954 / 0xdb8 | 0xbc0, 0xbd4 / 0xdb8 | 0xc40, 0xc54 / 0xdb8 |
| sort tables (B only, 4 x 7 bytes) | - | - | 0x360 (v1), 0x367 (v0), 0x36e (at), 0x375 (negate) |
| G_DL depth byte | 0x110 (mm, tt: 0x15a) | 0x110 | 0x148 |
| persp-norm | op 0xb4 stores D16[0x112] (sm64 pw pwb wr) | same | none: G_MOVEWORD index 0x0e, offset 0 stores D32[0x110] (mm tt gl ss wo; op 0xb4 = RDPHALF store at 0x10c) |
| FIFO pointers 0x128/0x150/0x154 | used (mm, pwb: zero; FIFO end only at 0x140, output is the DMEM ring) | used | used |
| initial geometry mode D32[0x114] | 0 | 0 | 0 (wo: 0x00800000) |
| near plane vector D16[0x98..0x9f] | (0,0,1,1) (pw, pwb: (0,0,0,1)) | (0,0,1,1) | (0,0,1,1) |

Probe evidence (state landing addresses): `w3/x/probe_all.txt` (single-command display lists, DMEM diff).

#### 10.3.3 Command encodings seen by VTX / TRI1 (w0 w1, big endian)

| | sm64 pw pwb mm tt | wr | gl ss wo |
|---|---|---|---|
| G_VTX w0 | `04 BB LLLL`: BB = ((n-1)<<4) + v0, LLLL = 16n-1 (DMA length-1 = w0 & 0x1ff) | 0x04000000 + ((5*v0)<<16) + (n<<9) + (16n-1) | 0x04000000 + ((2*v0)<<16) + (n<<10) + (16n-1); DMA length-1 = w0 & 0x3ff |
| G_VTX w1 | segmented address of n*16 input bytes | same | same |
| G_TRI1 w0 / w1 | bf000000 / (flag<<24) + ((10*i0)<<16) + ((10*i1)<<8) + 10*i2 | bf000000 / ((5*i0)<<16) + ((5*i1)<<8) + 5*i2 (top byte unused) | bf000000 / ((2*i0)<<16) + ((2*i1)<<8) + 2*i2 (top byte ignored) |
| vertex pointer of index byte b | 0x420 + 4*b | 0x420 + 8*b | D16[0x31e + b] |
| G_PERSPNORMALIZE | w0 = b4000000, w1 = norm | same | w0 = bc00000e (MOVEWORD idx 0e), w1 = norm (mm, tt too) |

[verified]: every row was used to drive the builds in the differential runs, outputs identical to the SM64 encoding of the same scene (except the listed semantic differences).

### 10.4 Unchanged: what was tested equal (so a table of addresses is enough)

All of the following are bit-identical between sm64 and every other build once the knobs of section 10.5 are respected:
- Vertex records +0..+35 (clip coordinates hi/lo, colour/lit/fog, scaled s,t, screen x,y,z and z fraction, 1/W): lit with 0..7 lights, texgen and
  linear texgen, fog, clip ratio c76 = 1..6, odd and even n, v0 up to 16-n, persp norm varied, plausible and wild matrices.
- The G_TRI1 decision (reject by AND of flags, clip request by OR of flags, zero area, cull front, cull back), the sort, the area negation, the RDP
  triangle header (cmd 0xc8|gm&7, flip/tile byte, YL/YM/YH, XL..DxMDy), shade, texture, z blocks, for non-clipped and clipped triangles
  (the clip overlay is the same code in all nine images; only its scratch addresses and its return differ, see 4.1/4.3).

### 10.5 Semantic differences (b), as maths

Notation as in sections 6 to 8: A, B = first and second vertex of each pair the G_VTX loop processes (pairs start at the first vertex of the
command, so A = batch index 0, 2, 4, ...; B = 1, 3, ...; for odd n the last vertex is an A with a discarded B), W_A, W_B = clip W integer parts (+6),
F = the 16-bit flag word at +36; "B receives a copy of F_A at +38" is unchanged. y = signed s16 at +26. `pw` stands for pw and pwb.

#### 10.5.1 pw, pwb: different clip-flag word, reject mask, near plane

Let c1, c2 be the VCC read-outs of the two compare groups (against W, against c76*W), bit k = le of lane k, bit 8+k = ge, lanes 0..2 = A (x,y,z),
4..6 = B. sm64 builds t0 = ((c1&0x707)<<4) | ((c1&0x7070)<<16) | (c2&0x707) | ((c2&0x7070)<<12), F_A = t0 & 0xffff, F_B = t0 >> 16.
pw builds t0 = ((c1&0x703)<<4) | (((c1&0x7030) | (sA<<2) | (sB<<6))<<16) | (c2&0x707) | ((c2&0x7070)<<12) with sA = [W_A < 0], sB = [W_B < 0].
Equivalently, with F the sm64 word:

    F_A' = F_A & ~0x0040
    F_B' = (F_B & ~0x0040) | (sB ? 0x0040 : 0) | (sA ? 0x0004 : 0)         (sA lands in B's bit 2, a shift slip in the microcode)

So the z "le -W" flag (bit 6, the near-plane flag) is replaced by "W < 0", and only second-of-pair vertices carry it; A vertices never request near clipping.
Bit 6 of F_A' is always 0. [model; vertex records identical incl. +38 in every pw/pwb run, several thousand scenes; code 0x5f0..0x630]
G_TRI1: reject test `F0&F1&F2 & mask` with mask 0x7030 (sm64 0x7070, i.e. no "all three z <= -W" reject); clip request mask stays 0x4343 [read: andi at tail
+0x18; confirmed by the flag model plus 0 rdp differences on the 351 non-clip scenes of the 3000, with the clip decision evaluated on each build's own flags].
Clip overlay: identical code; its near-plane coefficient vector at D16[0x98..0x9f] is (cx,cy,cz,cw) = (0,0,0,1) instead of (0,0,1,1): the plane is
"W > 0", not "Z + W > 0" (inside tests still use the flags only). [verified 600 scenes with W<0 vertices: pw vs pw with 0x9c patched to sm64's value
differ in 136; with W>0 only: 0 of 600 differ, i.e. the vector is only reached through the W<0 flags]
Everything else (non-clip path, lit path, texgen, tail) identical. pwb differs from pw only in addresses and in the XBUS transport (4.4).

#### 10.5.2 wr: three G_TRI1 differences, same vertex maths, relocated buffers

Let g = the geometry-mode word, a1 = sext8(g & 0xff) | 0xc8 (cmd byte), s = (g & 7).
1. No front-face culling: sm64 discards when area_hi >= 0 and g&0x1000 (cull front) or area_hi < 0 and g&0x2000 (cull back). wr implements only the
   second (back) test; bit 0x1000 has no effect. [verified: 3000 scenes with g&0x1000 cleared: 0 differences; cull front the only difference left (shade and smooth forced): 58/600 differ]
2. No flat shading: wr always takes the three vertex colours (sm64 with g&0x200 set); the flag byte does not exist; the bit 0x200 has no effect.
   [verified: wr with 0x200 cleared == sm64 with 0x200 set, 600 scenes; with the flag byte random]
3. The 64-byte shade block is emitted iff s != 0 (sm64: iff g&4). Header cmd byte is still 0xc8|s. Triangle byte lengths by s = 0..7:
   sm64 32,48,96,112,96,112,160,176; wr 32,112,160,176,96,112,160,176. (So a texture or z triangle without G_SHADE carries a shade block the cmd byte does
   not announce; games always set G_SHADE.) [verified: per-s lengths and bytes, 2 scenes x 8 values, plus the random runs]
With these three neutralised (g |= 0x204, g &= ~0x1000) wr == sm64 on 3000 scenes (1858 drew), clip scenes included (2649 had a clip request, ZCLIP runs 1080).
Other wr facts: the TRI1 core also serves op 0xb5 (quad/line: 4 vertex bytes, 2 triangles via the same tail, [read]); the clip overlay returns through
`lw s8,0xa6c; jr s8` (the tail stores s8 there at 0xa04); G_POPMTX sets the light-stale flag (10.1); command loop `gp` = end pointer.

#### 10.5.3 mm, tt (2.0H, family A): no difference in VTX/TRI1 maths

Code of 0x568..0x724 and 0x988..0xfd4 is instruction-identical to sm64 (only the addresses of the return slot D16[0x104], DL depth byte and the flush/transport
differ). The only observable change feeding them: persp-norm is set by G_MOVEWORD index 0x0e (op 0xb4 is RDPHALF). [verified 3000 scenes each, 0 differences,
persp varied through the right command]. mm additionally uses the XBUS transport and a room check before each triangle (4.4).

#### 10.5.4 gl, ss, wo (2.0H, family B): common differences

These three share the following; gl and ss are the same code (4 blocks of address differences), results identical to each other on all scenes.
1. Addressing/layout as in 1.1..1.3: 32 vertex slots via the pointer table, other command encodings, relocated buffers, 10-bit DMA length. The G_VTX
   loop counts t1 = n = byte2>>2 down by 2 per pair; semantics of the pairs are unchanged. [verified: slots 16..31 behave like 0..15 (500 + 200 scenes, vertices
   and triangles)]
2. Flat shading source: with g&0x200 clear the colour comes from the FIRST vertex of the command (pointer `at`, before sorting); the flag byte of w1 is not
   read. This is sm64 with flag = 0 always. [verified: gl == sm64(flag 0) on flat scenes with random flag bytes, 800 scenes; sm64(flag 1) differs in 319]
3. Vertex sort tie rule (the only change in the order/parity). Let A,B,C be the three pointers in command order, y_A,y_B,y_C their y.
   sm64: repeat { if y_A < y_B swap(A,B), flip ^= 1; if y_B < y_C { swap(B,C), flip ^= 1, restart } else stop }  -> non-increasing y (the register `at` ends with the largest y;
   only the comparison direction matters here, 8.2 names the roles H/M/L), a stable sort. 2.0H: idx = 4*[y_B<y_C] + 2*[y_A<y_B] + [y_C<y_A]; (first, second, third; negate) from the tables (A=0,B=2,C=4 as byte offsets):

        idx 0: A B C 0 | 1: A B C 0 | 2: B C A 0 | 3: B A C 1 | 4: C A B 0 | 5: A C B 1 | 6: C B A 1 | 7 impossible

   Identical to sm64 for every y triple except y_A == y_C != y_B: if y_A = y_C < y_B sm64 gives (B,A,C) with flip 1, 2.0H gives (B,C,A) with flip 0;
   if y_A = y_C > y_B sm64 gives (A,C,B) flip 1, 2.0H (C,A,B) flip 0 (enumerated over y in {0,1,2}^3: exactly those 6 triples differ). The flip flag is
   the area negation (and with it the RDP flip bit and the edge roles), the order changes which vertex is H/M/L and so XL/XM and the attribute bases.
   [verified: 79 of 79 unclipped scenes with such a triangle differ, 0 differences in 3000 scenes once those are skipped; scenes whose clip output has no
   tied y (YL==YM or YM==YH) are identical in 2078 z-clipping scenes and 323 more clip scenes, so the clip code itself is unchanged]
4. Lighting overlay: same code; only the modelview address it reads for the light-direction transform is 0x3e0 (family A 0x360). [read; lit scenes verified]
5. Shade block rule, cull front, culling, flip/tile byte, z, texture: as sm64 (unlike wr). [verified]

#### 10.5.5 wo only (on top of 10.5.4)

1. Geometry bit 0x00800000 (D32[0x114], "clipping"; initial value of the word is 0x00800000): when CLEAR, G_VTX skips the compare/flag code (0x42c..0x464) and
   stores the stale register t0 (a return-address leftover such as 0x109c, or 0x0460 for the first pair) as the flag word; G_TRI1 (`lb t1,5(sp); bgez`) skips
   the reject and clip-request tests entirely, so nothing is rejected or clipped by flags. When set, flags are the sm64 ones. [verified for the VTX flag junk and
   that sm64-vs-wo triangle counts differ without the bit and match with it; reject/clip skip: read]
2. Screen y is rounded to whole pixels after the viewport clamp: y' = (y + 2) & 0xfffc (s16 arithmetic, round half up, multiple of 4 in the 10.2
   format); stored at +26 for both vertices of a pair; x, z, z fraction unchanged. [model; 1200 scenes incl. wild, 0 vertex differences]
3. Triangle setup assumes y is a multiple of 4: the sub-scanline correction f = (yH & 3) << 14 is dropped (XH = XM = x_H/4 exactly, the attribute fraction words stay at the 0x8000 colour
   bias). With snapped y this equals sm64's formula; with the snap patched out (unsnapped y) it does not (observed: XH == XM, no gE*f term). [verified: with y aligned
   on the sm64 side (input nudged until all screen y % 4 == 0) wo == sm64 in ~700 scenes incl. fog/lit/texgen/z/tex/shade/flat/cull combinations, apart from
   3.4 items 2 and 3 (ties 11/11 scenes differ, random flag 126/344 differ)]
4. Lazy submission: each triangle is handed to the RDP (DPC_END write) when the next triangle starts or when a later RDP command flushes; the last triangle
   of a list stays pending until then (DL = tri,tri,tri,ENDDL delivers 2 triangles; with a following sync command all 3). Bytes unchanged. [verified]
5. Clipping with the bit set: new vertices come from the same vertex code, so their y is snapped too: clipped output differs from sm64 in y (not compared
   bit-exactly, see 10.7).

### 10.6 Other facts that change what a native path must do per build

4.1 clip overlay: identical algorithm in all nine; per-build constants: window base, scratch (10.3.2), near plane vector (pw), return path (wr: `lw s8,0xa6c; jr s8`;
gl/ss/wo: `jr t8` / `jalr s8` using the return address the TRI1 handler keeps in t8; sm64 and family A: `j 0xa8`). The clipper re-enters the vertex code (j 0x5e8 / equivalent) for
every new vertex, so the vertex differences of 10.5.1 and 10.5.5 also apply to clipped geometry.
4.2 pw flag semantics also apply to G_CULLDL (mask 0x7030) and to vertices produced by the clipper.
4.3 wr/gl/ss/wo keep the clipper's new-vertex area at 0xdb8 although their RDP output buffer starts at 0xc60/0xce0: it overlaps once more than 0x158 (wr) /
0xd8 (B) bytes are pending in the output buffer (a single 0xb0-byte triangle does not reach it; same remark as in 9.5).
4.4 XBUS builds (mm, pwb): the init sets DPC_STATUS xbus; D32[0x128]/[0x150]/[0x154] are 0 (FIFO end only at 0x140); triangles are appended at s7 in the DMEM
ring starting at 0x9e0 and each triangle is its own RDP call with DPC_START/END = DMEM addresses (observed (0x9e0,0xa80), (0xa80,0xb20), ... for 0xa0-byte
triangles, after an initial (0x1000,0x1000)); `jal 0x178` before each triangle tests for 176 bytes of room (limit 0xde0). The byte stream is the same as the FIFO builds.
RDRAM FIFO builds issue (S,S+n) calls from the FIFO start after the initial (END,END) call, as in 4.2.
4.5 wo is the only build whose output depends on a geometry bit that sm64 never sets (0x00800000) and on y snapping; wo's tri code is a rewrite (software
pipelined, header bytes stored with `sb`, DPC wait loop `mfc0 a3,a2`), numerically identical to the sm64 formulas for aligned y.
4.6 The RDP command opcode byte and the flip/tile byte are the same in all builds: 0xc8 | (g & 7), (flip<<7) | sext8(D8[0x122]).

### 10.7 (c) Same function with different addresses, and different functions

| group | members | relation to sm64 |
|---|---|---|
| identical function, other addresses/transport | tt, mm, (pwb vs pw) | G_VTX and G_TRI1 instruction-identical to sm64; persp command differs (10.5.3); mm/pwb: XBUS transport |
| same function + parameter tables | wr | same vertex code; 3 triangle-rule differences (10.5.2); relocated buffers (10.3.2); encodings (10.3.3) |
| same function + data differences | pw, pwb | flag word / reject mask / near plane vector (10.5.1); everything else identical |
| same function as each other | gl == ss | restructured addressing (pointer table, 32 slots), flat source and tie rule differ (10.5.4); math identical |
| different function (extended) | wo | gl + clip bit gating + y snap + lazy flush (10.5.5) |

Minimum parameter set per build for a generic native path: the rows of 10.3.1/1.2/1.3 (entry pcs, buffer addresses, encodings), plus these flags:
`flags_variant` {sm64 | pw (10.5.1)}, `cull_front` {yes | no(wr)}, `flat_shading` {yes, vertex chosen by flag byte | no (wr) | first vertex (B)},
`shade_block_when` {g&4 | g&7 (wr)}, `tie_rule` {bubble | table (B)}, `y_snap` {no | always (wo)}, `clip_gate_bit` {none | 0x800000 (wo)},
`near_plane_vec`, `reject_mask` {0x7070 | 0x7030}, `persp_cmd` {0xb4 | movew 0x0e}, `transport` {FIFO | XBUS | FIFO lazy (wo)}.

### 10.8 Reproduction and files

- Cut images / disassemblies: `w3/h20/`; aligned diffs `d_<label>.txt` (vs sm64v), `dw_<label>.txt` (vs wrv), `d_gl_wo.txt`.
- Harness: `w3/x/xb.cpp` (+ `fh.hpp`, `fhtest.hpp`, `snap.h`, copied from `cmd/tools`, with the XBUS read added to the RDP callback);
  link with the objects built by `make -C armlet-f3d-wt/tools/f3d OBJDIR=<copy of test-output/f3d/obj> BIN=<dir>` (everything except the f3d*.o). Run
  `H20=.../w3/h20 ./xb REF TEST [count] [seed]` with env knobs: PLAUS, TIGHT (1: small, 2: tiny coordinates), LIT, FOG, TEXGEN, C76, NL MAX, GMOR/GMAND,
  TRIGM, FLAG0 (all flag bytes 0), REFFLAG, NOTIE (1 skip any y tie, 2 skip y_A==y_C!=y_B ties, 3 only those), NOOUTTIE (skip scenes whose reference output
  has a tied triangle), NOCLIP, ALIGNY (nudge input y so screen y % 4 == 0 on the reference), MODEL=pwflags|wosnap, PATCHA/PATCHB (IMEM word edits),
  DMPA/DMPB (DMEM data edits), SHIFT (load test build's vertices at slot +SHIFT), NOSYNC, RAW (print DPC calls), STATS, DUMPV, FULLRDP/RDPN, `-probe` mode
  (layout probes into a directory, analysed by `probe_an.py`). The ROM -> cut image step: `w3/h20/mkpseudo.py`.
- Typical commands: pw `MODEL=pwflags NOCLIP=1 ./xb sm64v pwv 3000 71`; wr `GMOR=0x204 GMAND=0x1000 ./xb sm64v wrv 3000 71`; gl/ss
  `FLAG0=1 NOOUTTIE=1 NOTIE=2 ./xb sm64v gl2h 3000 71`; tt/mm `./xb sm64v tt2h 3000 71`; wo `ALIGNY=1 IDENTMV=1 GMOR=0x800000 FLAG0=1 NOTIE=2 NOCLIP=1 TIGHT=2 ./xb sm64v wo2h 1500 5`.

### 10.9 (d) Still unknown / not compared

- Clipped triangles of wo (new vertices snapped, bit set) and the wo behaviour of the vertex flag junk beyond the first pairs; wo's exact lazy-flush release rule
  (which later commands release the pending triangle).
- Anything not visible in vertex records + RDP bytes: DMEM scratch and stale vector lanes the next command reads (the stale-lane quirks of `f3d_tri.c`)
  were not compared build against build; scratch addresses are in 10.3.2 but contents unverified. Same for the accumulator/flags/divide state at exit.
- XBUS builds (mm, pwb): ring wrap at 0xde0 and the room check were not exercised (needs > 0x400 bytes of triangles in one list); DPC call chunking at
  buffer-full is unverified. Likewise FIFO wrap per build (only sm64's is in 4.2).
- 2.0H builds with n > 16 per call (only the slot shift 16 was tested), more than 7 lights, G_MW_POINTS vertex writes, direct MP loads through MOVEMEM matrix indices
  in family B, G_CULLDL in family B.
- Which of the two Pilotwings images is used by which task; the second and third data segments of SSSV (0x36eb0, 0x37e70 with 2.0H strings) are other
  microcodes (not analysed). The Sprite2D variant is outside this document.
- The clip-plane vector (0,0,0,1) of pw is verified only through its effect (136/600 differences), not through an independent model of the clipper.
- wr's quad op (0xb5) and its use of the TRI1 core ([read]); the G_VTX `ret` slot and the yield save areas (not VTX/TRI1).

## 11. DMEM at the end of a task: what is leftover, what later tasks read

A graphics task that runs to its end leaves DMEM in the state the microcode computed. The task-level path may leave a *different* state in the ranges nothing reads afterwards ("dead DMEM ranges").
This section gives the facts for the F3D family that decide those ranges: what a task writes, what a task reads before it has written it (its *live-in* DMEM), and the result of poisoning
DMEM at the end of every task in whole-game runs. Everything here is measured on the real microcode (the JIT's behaviour); nothing is assumed from the code layout.
**The evidence is per microcode family.** It holds for the F3D builds measured below (data segment DMA'd to 0..0x7ff at every task start, every byte above written before it is read). It says nothing about other
families: for example the F3DEX 1.x builds do *not* reload DMEM 0x800-0xfbf between tasks, so stale vertex slots survive into the next task there (reported by the F3DEX 1.x analysis), and the same experiment must be run for each family.

### 11.1 Method

* **Read/write sets.** `tools/f3d/dmem_rw.cpp` is `tools/native/trace.cpp` (instruction-level interpreter of a task-start snapshot, `ARMLET_HLE_CAPTURE`) with a byte-granular footprint of every DMEM access
  added: scalar loads and stores, the exact byte footprints of all vector loads and stores (derived from the fork's `rsp/ls.cpp`), and the DMEM side of every SP DMA (class-0 geometry DMAs, the matrix push, the RDP flush,
  the yield save). *Live-in* (LI) = a byte read before the task has certainly written it. *Written* (W) = every byte any store or DMA-in touches. The task is interpreted from its boot code (pc 0) to its end,
  with the RDP modelled as the host core does (DPC_CURRENT = DPC_END after each DPC_END write). Every traced task ended at the same pc as the JIT did. Tasks are consecutive tasks of a 900-frame
  attract-mode run of each game (no controller input), up to 400 per game; `tools/f3d/dmem_rw_agg.py` takes the union per build.
* **Poison.** `ARMLET_DMEM_POISON=<lo>-<hi>[,...]` (a private patch of `hle_task.c`, not in the repository) overwrites the listed DMEM bytes with a pseudo-random pattern at the end of every JIT-run graphics task.
  Compared against the unpoisoned run of the same game and the same 1200 frames (`ARMLET_HLE_CAPTURE` of every task, `EVERY=1`): the **complete event list of each task** (every SP DMA write with the hash of the bytes written,
  every DPC_START / DPC_END write with the hash of the command bytes, SP_STATUS and semaphore writes, plus the hash of the whole RDP command stream), the screenshots, the audio and the RSP state stream of
  `tools/equiv` (`tools/f3d/hle_events_cmp.py` compares the event lists of two capture directories). (The RSP *hash* stream hashes DMEM and must differ.)

### 11.2 Result: nothing but the OSTask header is read from a previous task, and poisoning all of DMEM changes nothing

Poisoning **DMEM [0x000, 0xfc0)** (everything except the OSTask header) after every graphics task, 1200 frames per game, on the x86 desktop build (JIT-only, `ARMLET_NATIVE` off):

| game (ucode) | tasks compared | events compared | identical event lists | screenshots | audio | RSP state stream |
|---|---|---|---|---|---|---|
| Super Mario 64 (94c7cfdd..) | 488 | 1 945 765 | 488 | 6/6 | identical | identical |
| Pilotwings 64 (3e9b9c3a..) | 600 | 4 426 824 | 600 | 6/6 | identical | identical |
| Wave Race 64 (fb483893..) | 354 | 2 779 390 | 354 | 6/6 | identical | identical |
| Mischief Makers (4212d63f.., 2.0H, XBUS) | 600 | 1 037 411 | 600 | 6/6 | identical | identical |
| Tetrisphere (88fa3c0a.., 2.0H) | 592 | 1 323 845 | 592 | 6/6 | identical | identical |
| Wipeout 64 (19bd8c82.., 968dec0e..) | 600 | 160 719 | 600 | 6/6 | identical | identical |
| Glover (208a567e.., 9d0d289f..) | 600 | 51 024 | 600 | 6/6 | identical | identical |
| SpaceStation Silicon Valley (047252e8..) | 600 | 931 028 | 600 | 6/6 | identical | identical |

0 differences everywhere. Limits of this evidence: x86 desktop only, attract-mode / title windows without input, the pseudo-random pattern is one fixed sequence; the proof required for a mask in the repository is the
same experiment with the final mechanism (tools/equiv, poison at task end, both devices, gameplay windows).

### 11.3 What each F3D build writes (W) and reads before writing (LI), union over the traced tasks

LI always also contains the OSTask header fields the boot code and the microcode read: 0xfc4 (flags), 0xfd0 (ucode), 0xfd8..0xfe3 (ucode_data/size words), 0xfe8..0xff3 (output_buff, size, data_ptr), 0xff8 (yield_data_ptr)
(written by the CPU before each task; the exact words differ slightly per boot/ucode). The table lists only the other bytes.

| build | tasks | W: bytes written (union) | LI: DMEM read before written (other than OSTask) |
|---|---|---|---|
| sm64 94c7cfdd (2.0D) | 368 | 0x000-0x8ec, 0x8f0-0x926, 0x928-0x92e, 0x940-0x94e, 0x954-0x962, 0x9e0-0xa90, 0xde0-0xf1e | 0x8f0-0x8f8, 0x926-0x928, 0x92e-0x930 |
| pw 3e9b9c3a (2.0D) | 400 | same as sm64 (0x940-0x950 for the polygon buffer) | same as sm64 |
| wr fb483893 (2.0D) | 253 | 0x000-0xd10, 0xde0-0xf1e, 0xf20-0xf6e | 0xad0-0xae8, 0xaf0-0xaf8, 0xb40-0xb58, 0xba6-0xba8, 0xbae-0xbb0, 0xbe0-0xbe8, 0xbf0-0xc18 |
| mm 4212d63f (2.0H family A) | 400 | 0x000-0x8ec, 0x8f0-0x926, 0x928-0x92e, 0x9e0-0xe20 | 0x830-0x838, 0x8f0-0x8f8, 0x926-0x928, 0x92e-0x930 |
| tt 88fa3c0a (2.0H family A) | 400 | as sm64 | 0x8f0-0x8f8, 0x926-0x928, 0x92e-0x930 |
| 208a567e (Glover F3D, 2.0H family B) | 335 | 0x000-0x950, 0x9a0-0xcc0, 0xce0-0xd80, 0xde0-0xf1e (and the OSTask words 0xfc4-0xfc8) | 0xcc0-0xcc8, 0xcd0-0xcd8 |
| 047252e8 (SpaceStation, family B, the F3D part) | 166 | 0x000-0xd90, 0xde0-0xf1e, 0xfc4-0xfc8 | 0xb60-0xb98, 0xc26-0xc30, 0xc80-0xc98, 0xcf0-0xcf8 |
| 19bd8c82 (Wipeout 64) | 334 | 0x000-0x800, 0x9a0-0xb28, 0xce0-0xce8, 0xfc4-0xfc8 | none |
| tasks of other microcode that ended at pc 0x1cc (047252e8 x234, 968dec0e x66, 9d0d289f x65) | 365 | 0x000-0x828 (Sprite2D) or 0x000-0x800 and 0x9a0-0xaf8 | none |

(The "also OSTask" writes of the family B / Wipeout / SpaceStation builds are `0xfc4` flag words written by those microcodes at their end; the 2.0D builds and the family A 2.0H builds never write 0xfc0 and up.)

For SM64 the three live-in scratch ranges are *stale-lane* reads: 0x8f0-0x8f8 is read at IMEM 0x690 by the G_VTX loop's prefetch of the next vertex pair past the last pair of a 16-vertex load (the junk lanes of a pair that is
never stored), 0x926-0x928 and 0x92e-0x930 are lane 3 of the texture-scale temporaries read at 0xe58 / 0xe60 (written by no task, i.e. whatever DMEM held before; they feed only the unused fourth lane of the
magnitude measure, `v5[7]`/`v6[7]`). Every one of them reaches only dead lanes (section 8.7), and the poison runs above show that.

### 11.4 Dead DMEM ranges for the task-level path (candidates, per build)

* **All builds in 11.2: [0x000, 0xfc0)** is dead at the end of a task (poison-tested above): a following graphics task of the same microcode reloads 0..0x7ff with the ucode data at its start, and writes every byte of the
  rest that it reads (11.3: the only exceptions are the stale-lane bytes, which influence nothing). The OSTask header [0xfc0, 0x1000) is not dead (the CPU writes it, the microcode reads it).
  The microcode families that write 0xfc4.. (B, Wipeout, SpaceStation, Glover) need those words kept.
* **Narrower tiers**, if a finer table is wanted (bytes never written by the task keep their old value anyway, so a mask over them costs nothing): the written union W of 11.3. Within it the *scratch that is
  certainly never read by anything but the command that wrote it* (high confidence even without the whole-DMEM result): the RDP output buffer and clip scratch [0x9e0, 0xfc0) (SM64 layout; family B 0x9a0/0xce0..; WR 0xc60..),
  the triangle scratch [0x8e0, 0x930), the clip polygon buffers [0x940, 0x968), the geometry/vertex DMA buffer [0x7e0, 0x8e0), the matrix-product scratch [0xde0, 0xe20), the 40-byte clip vertex slots [0xde0, 0xfc0),
  the vertex +38/+39 junk of every slot. State that games could in principle expect to survive a task (matrix stack contents, light slots, segment table) is below 0x800 and is reloaded by the next task's data DMA.
* **Never dead**: OSTask [0xfc0, 0x1000); the RDRAM the microcode writes (matrix stack pushes), the RDP command bytes, SP/DPC registers, MI_INTR.

### 11.5 What the path must still do with the live-in scratch bytes

Nothing to the RDP-visible result: they only feed dead lanes. A bit-exact DMEM comparison *without* a mask would need the previous task's leftover in those bytes; with the mask the verify never reads them.
The native front-end must not read them as input.

### 11.6 Not covered

* Gameplay windows with input (SM64 levels, Wave Race races): the traced and poisoned runs are attract / title windows. Poison runs on the final mechanism must cover them.
* The CPU reading RSP DMEM (no F3D game does through the OS interface; not checked in game code).
* A task that yields (SP_STATUS SIG0 set by the CPU): the microcode saves DMEM[0..0x8ff] to RDRAM itself and the OS restores it; the task-level path declines such tasks.
* Device (A53 / A55) runs of the poison experiment.


## 12. Notes for the shared library and the front-end

### 12.1 Where F3D differs from F3DEX / F3DEX2 (only the F3D side is verified; statements about F3DEX / F3DEX2 are quoted from the public descriptions of those microcodes and marked [unchecked])

* **Dispatch.** Two-level: the first level is the class `op >> 6` (class table at DMEM 0xbc), the second level an opcode table (class 0: ops 0x00-0x3f at 0xc4, which carry a geometry DMA of `(w0 & 0x1ff) + 1` bytes from `seg(w1)` into 0x7e0
  *before* the handler runs; class 2: ops 0x80-0xbf at 0x76; class 3: ops 0xc0-0xff are copied to the RDP; class 1 is a no-op). Opcode numbering is the F3D one (G_VTX 0x04, G_TRI1 0xbf, G_MTX 0x01, G_MOVEMEM 0x03,
  G_POPMTX 0xbd, G_CULLDL 0xbe, G_DL 0x06, G_ENDDL 0xb8, G_TEXTURE 0xbb, G_MOVEWORD 0xbc, G_SETOTHERMODE_L/H 0xb9/0xba, geometry mode 0xb7/0xb6, G_PERSPNORMALIZE 0xb4).
* **Flushing.** Every RDP command and every triangle is DMA-flushed to the RDP FIFO on its own with one DPC_END write each (no batching of a display list into one list); DPC_START is written only on FIFO wrap and at init.
  The write pointer (D32[0x128]) must match after the task. A native path that submits one coalesced list must reproduce D32[0x128], the final DPC registers and the concatenated command stream (the coordinator's
  verify rule: stream bytes + final DPC / FIFO / SP / MI state, list boundaries, per-chunk DMA events and poll counts excluded).
* **Vertices.** 40-byte records: clip coordinates as int+frac (`+0`, `+8`), colour `+16`, st `+20`, screen xyz `+24` with the z fraction `+30`, 1/W `+32`, clip flags `+36`. The vertex is
  transformed *two at a time* (8 lanes); the screen z comes out of the same `vtrans + vscale * (clip * 1/W)` chain as x and y, clamped to [0, 0x7fff] (limits at 0x28), together with a 16-bit z fraction at +30.
  The clip flags are 6 bits x 2 copies (6.2), with the guard-band copy using the clip ratio K (DMEM[0x76]).
* **Lighting.** Per-vertex loop over light *slots* with the direction already transformed into model space by the **relight** step (overlay 2, 7.2): a lazy flag in the light-count word (bit 31 of D32[0x12c], set by every
  G_MTX and G_MOVEWORD NUMLIGHT) triggers it at the next lit G_VTX. (F3DEX2 keeps the light directions transformed by a different mechanism [unchecked].)
* **Triangles.** One command (G_TRI1), vertices addressed by `10 * slot` bytes (`0x420 + 4 b`), culling bits 0x1000 (front) / 0x2000 (back) of the geometry word (F3DEX2 uses other bit positions [unchecked]), reject mask 0x7070 / clip mask 0x4343
  on the 16-bit flag word, an explicit **clipper in an IMEM overlay** (six planes with the K guard band in x / y; section 9); the area is negated for an odd number of sort swaps; the whole RDP triangle
  (header + shade + texture + z) assembled in DMEM and flushed once per triangle. No `G_TRI2`, no `G_QUAD`.
* **Reciprocal.** The reciprocal routine in overlay 0 (IMEM 0x00-0x87) is the one-Newton-step refinement of 1.1; it is used for W (per vertex), the triangle area (per triangle), the three edge slopes
  (single `rcp32`, no refinement) and the texture scale.
* **Setup arithmetic that is easy to get wrong** (all in 8): the `lo3` product rule (1), the edge slope's `0xfff8` mask in the start-x correction only, the unmasked `s1_20` in the major-edge gradient, the area high word
  saturating while its low word wraps, the VCO carry of lane 1 entering the first difference, the K scale of the texture block only, and the z block multiplied by 32 *after* the (unscaled) gradients.

### 12.2 Notes for a native implementation

(from the command-layer experiments) To match DMEM bit for bit also reproduce: the geometry DMA into 0x7e0 for every op below 0x40, the 0xde0 scratch after a G_MTX multiply, the k1 / gp / s7 state, D32[0x108..0x15c],
the 0x336 stack words, and the yield save area. To match RDRAM and the RDP list reproduce: the matrix stack pushes (72 bytes for LL = 0x40), the FIFO bytes and the (START, END) call sequence. The path declines on a
yield request, on clipping until the clipper is written (the ~10% of SM64 vertex calls that relight are not a reason to decline once 7.2 is implemented), on a FIFO wrap whose wait path would block, and on 32767 polls.
Do not rely on class-0 opcodes >= 0x0a, class-2 opcodes < 0xb0, or MOVEMEM / MOVEWORD indices outside the tables: undefined.

### 12.3 Parameters a generic F3D front-end needs per build

From 10.3: entry and layout addresses (buffers, matrices, viewport, vertex base and slot count, scratch), command encodings (G_VTX byte layout, G_TRI1 index scale, persp-norm command), and the flags
`flags_variant`, `cull_front`, `flat_shading`, `shade_block_when`, `tie_rule`, `y_snap`, `clip_gate_bit`, `near_plane_vec`, `reject_mask`, `persp_cmd`, `transport` (10.9).


## 13. Evidence, reference implementations and how to reproduce

Reference implementations (op-for-op, JIT-exact, local to the F3D worktree and not part of the repository's shared code): `src/native/f3d_vtx.c` (+ `f3d_vtx_fast.h`, `f3d_vtx_neon.h`), `src/native/f3d_tri.c`,
`src/native/f3d_vu.h` (accumulator, clamps, `rcp32`, `recip_w`, `recip_fast`, `clip_flags`). Test harnesses and models (all in `tools/f3d/` unless noted): `test_vtx.cpp` (random differential test of G_VTX against the JIT),
`test_tri.cpp` (G_TRI1, ~20 000 scenarios per seed with a RDP list, FIFO and DMA states), `test_recip.cpp`, `dmem_rw.cpp` (section 11), the Makefile / `mk.sh` / `env.sh` for private object directories. The mutation checks of those tests
are listed in the tests' headers. Sections 2 to 5 (command layer), 9 (clipper) and 10 (builds) are backed by separate harnesses of the same kind whose sources are working files of the spec's author, not in the repository:
the command layer (`E1..E14`: dispatch of all 256 opcodes, MTX / POPMTX against a C++ model, MOVEMEM, MOVEWORD, DL stack, task end / yield / resume, FIFO wrap and init, SETOTHERMODE, CULLDL), the clipper
(`clip-model-test.cpp`: 3000 to 40000 random cases per configuration, K = 1, 2, random, polygons of 3 to 9 vertices, loader path), and the build comparison (`xb.cpp`: the same abstract display list on two builds of the JIT,
3000 scenes per pair). Their results are in the sections as [verified N] tags.

The microcode images used: SM64, PW and WR from the profiler dumps (`ARMLET_PROFILE` ucode dump); the 2.0H images were cut from the ROMs (offsets in 10.2). ROMs and ucode text are not in the repository.

Open items (the section that owns each): wo clipped triangles and lazy flush release rule (10.5); XBUS ring wrap per build and FIFO wrap per build other than SM64 (10.6, 10.9); the 2.0H G_CULLDL and n > 16 loads (10.9); the relight of PW / WR (7.2);
the gameplay-window and device poison runs (11.6).



## 14. The struct-level functions (src/hle/f3d_fx.h)

The semantics of sections 6 to 9 as pure C functions on plain structs, for the F3D front-end (a hook that replaces the microcode's code at a pc, or a whole-task front-end): no DMEM addresses, no pcs, no vector
registers. `f3d_dmem.h` fills the structs from a DMEM or RDRAM image (host words holding big-endian values) and writes vertex records and matrices back. All arithmetic uses `hle_fx.h` (the closed forms of the VU chains) and the
local `f3d_recip_refine` (the same value as `hle_recip_refine`, fewer operations). Every function exists as `f3d_*_scalar` (the reference; plain C11) and `f3d_*` (NEON on aarch64, the scalar code elsewhere).

| function | computes | NEON form |
|---|---|---|
| `f3d_mtx_mul[_scalar](a, b, out)` | the G_MTX / G_POPMTX product (3.1) | yes |
| `f3d_vtx_xform[_scalar](ctx, in, n, out)` | n vertices: clip coordinates, flags (with the clip ratio K), 1/W, screen, texture scale, lighting, texgen, linear texgen, fog (6, 7.1) | yes (two vertices per call of the kernel, lighting vectorised) |
| `f3d_relight[_scalar](mv, raw, nslot, clamp, out)` | the light directions transformed by the model-view matrix (7.2); byte 3 of a slot is not computed (0) | scalar form only (a few slots per matrix change) |
| `f3d_tri_setup[_noclip][_scalar](in, cmd, len)` | the reject / clip / cull decision and the RDP triangle bytes (8) | yes (the attribute gradients and the stores of the blocks, four lanes per vector; the header and the texture scale stay scalar) |
| `f3d_clip_tri[_scalar](ctx, v, out)` | the clipper: new vertices and the fan (9) | scalar form only (about 8% of SM64 triangles) |

Not in the functions (the caller's): the DMA, the RDP flush and FIFO (4.2), the DMEM scratch that holds stale-lane leftovers (11), the microcode's pairing of vertices (invisible: vertex i uses lane half `i & 1` of the
light byte rows, which hold the same bytes after a relight), the lights-stale flag and `D32[0x12c]` bookkeeping, the persp-norm command per build. The functions return the clip decision (`F3D_TRI_CLIP`) rather than calling the clipper;
the fan triangles go through `f3d_tri_setup_noclip` (no flag tests), the first one with the VCO carry the clipper leaves (always 0 in the measured runs).

### 14.1 Tests (all in tools/f3d, ucode image from a profiler dump; `mutfx.sh` builds a mutated copy)

* `test_fx MODE` runs the JIT on the real microcode and compares the struct functions' scalar form with what the microcode left (vertex records bytes +0..+37, the RDP bytes, the matrices, the light bytes, the clipper's new-vertex slots).
  Results (x86, seeds 1..5, 7, 11, 21; counts are scenarios): **vtx** 103 000 (1..16 vertices each; lit with 1..6 lights, texgen, linear texgen, fog, K = 1..6 and random, tiny-W matrices), **mtx** 43 000, **relight** 64 000
  (matrices from tiny to saturating, 0..16 slots, random clamp constants), **tri** 100 000 (about 55 000 drawn: all of shade / texture / z, flat and smooth, culls, ties, saturating gradients, minimum z slope), **clip** 90 000 (about 25 000 clipped
  triangles, 42 000 fan triangles, up to 9-vertex polygons, K != 1, fog): 0 differences. The comparison of the clip test is the concatenated RDP bytes of the whole fan plus every new vertex record.
* Mutation checks (each a deliberate model error; the test must fail): vtx 1 fog byte, 2 texgen centre, 3 the K-scaled copy projected (the first draft's error), 4 low word of the projection product taken after the high product (needs
  the tiny-W scenarios), 5 K not applied to the flags, 6 viewport sign ignored, 7 second vertex of a pair uses the first one's light bytes; mtx 8 rounded low product; relight 1 carry dropped, 2 byte shifted by 7;
  tri 2 area not negated, 3 flip bit, 4 low word taken late, 5 slope low word not masked, 6 z shifted by 4, 7 sort ties, 8 no minimum z slope, 9 farthest w; clip 1 weight bias 4, 2 weight complement off by one, 3 fan order, 4 plane order,
  5 rounded colour interpolation. All caught.
* `test_fxn` (no JIT; aarch64 under qemu or on a device) compares each NEON form with its scalar form on random inputs: vtx 20 000, mtx 20 000, tri 20 000 (12 000 drawn): 0 differences; mutations of the NEON code (screen +1, unsigned fraction treated
  as signed in the product, VSUB saturation quirk missing, low word taken late in two places, the z lane skipped when only z is on) all caught.
* `bench_fx` reads routine-entry snapshots of real SM64 calls (`ARMLET_NATIVE_CAPTURE`: 500 vertex calls, 200 triangle calls of which 150 are drawn), runs both forms and compares with the JIT's final DMEM:
  **0 differences** on x86, on aarch64 under qemu, and on both devices. (This check found a bug the random test missed: the NEON triangle skipped the z lane of a z-only triangle; the random test passed because the
  uninitialised array the NEON path read was the stack frame the scalar call had just written.)

### 14.2 Cost (ns, `bench_fx` on the SM64 snapshots; the timing excludes filling the structs from DMEM; JIT = the JIT's code for the same stretch, `armlet-native-replay --bench 3000`)

| | rgdsplus (A55, performance governor, ~1.58 GHz effective) | rg35xxsp (A53, `cpu_perf.sh on`, 1512 MHz) |
|---|---|---|
| G_VTX, JIT, per vertex | 633 / 678 / 641 / 640 (four calls of 14-16 vertices) | 685 / 734 / 688 / 688 |
| G_VTX, the op-for-op routine (NEON, local reference), per vertex | 182 - 190 | 193 - 202 |
| G_VTX, struct scalar, per vertex | 481 - 488 | 498 - 502 |
| G_VTX, struct NEON, per vertex | 237 - 238 | 238 - 246 |
| G_TRI1, JIT, per drawn triangle | 4920 - 5020 | 5250 - 5310 |
| G_TRI1, JIT, per culled / rejected triangle | 670 - 690 | 720 |
| G_TRI1, the op-for-op routine (scalar, local reference), per drawn triangle | 3190 - 3330 | 3370 - 3420 |
| G_TRI1, struct scalar, per drawn triangle | 1696 - 1712 | 1854 - 1871 |
| G_TRI1, struct NEON, per drawn triangle | 1092 - 1098 | 1129 - 1152 |
| G_TRI1, struct (either form), per rejected / culled triangle | 76 - 77 | 88 - 91 |
Per call the vertex rows are 12 vertices on average. Three repeats per device, alternated forms inside one process, 100 repetitions each; the clocks pinned as in the repository's device rules and restored afterwards.
Struct NEON is 2.7x (A55) / 2.8x (A53) faster than the JIT per vertex, 4.5x / 4.6x per drawn triangle and 9x per rejected or culled triangle; the op-for-op vertex routine is about 25% faster than the struct NEON
form (it loads and stores DMEM with vector instructions and keeps no per-vertex struct), the struct NEON triangle is 3x faster than the op-for-op triangle routine. Not measured: filling the structs from DMEM and writing the
results back (a hook pays it; a whole-task front-end working on the structs does not), the clipper and relight (scalar, rare).
