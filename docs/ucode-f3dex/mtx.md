# Matrix commands of F3DEX 1.x / F3DLX: G_MTX, G_POPMTX, the MVP update, the light-direction recomputation

Reference build F3DEX 1.21 ("banjo121"); pcs below are banjo121 unless a build is named. Spec routines: `tools/native-f3dex/spec/mtx_spec.c`, generators
`mtx_gen.inc`. Run: `SPEC_ONLY=mtx tools/native-f3dex/build_rt_test.sh x86 <out>`, then `rt_test --ucode <uc>/<build> --routine spec.f3dex.mtx`.

Conventions. Matrices are **row-vector** matrices: `v' = v * M`, `M[row][col]`. A matrix in DMEM at address `a` is the N64 `Mtx` layout, **kept as it is**
(the microcode does not convert it): 16 signed integer halves at `a + 2*(4*row+col)`, then 16 unsigned fraction halves at `a + 0x20 + 2*(4*row+col)`;
the element value is `(int16)int * 65536 + frac` as a 32-bit two's-complement number (S15.16). Below `A[r][c]` always means that 32-bit value.

## 0. DMEM map of this area (all builds)

| DMEM | size | meaning |
|---|---|---|
| 0x0ae0 | 64 | the `Mtx` operand DMAed by the command loop before the handler runs (all class-0 commands: DMA of `(w0 & 0x3ff) + 1` bytes from the segment-translated `w1` to 0xae0; for G_MTX the game sends 63, i.e. 64 bytes; DRAM address is forced 8-byte aligned downward by the DMA) |
| 0x03e0 | 64 | current modelview matrix (Mtx layout) |
| 0x0420 | 64 | current projection matrix |
| 0x0460 | 64 | MVP = modelview * projection (the cached matrix the vertex code uses), Mtx layout |
| 0x0134 | 4 | matrix stack pointer: an **RDRAM** address (the stack lives in RDRAM, not DMEM; entries are 64 bytes, grows upward). Init: OSTask `dram_stack` (DMEM 0xfe0) |
| 0x015c | 4 | stack limit: `dram_stack + dram_stack_size` (OSTask 0xfe0 + 0xfe4). Pushing is refused when the pointer equals it |
| 0x0fe0 | 4 | OSTask `dram_stack` (the base the pop compares against; also copied to 0x120 at init, not used by these commands) |
| 0x012c | 4 | light pointer word; **negative = light directions must be recomputed** (section 4) |
| 0x0de0..0x0e2f | 80 | scratch (section 5) |
| 0x01a0..0x02df | | light slots, see section 4 |

The display-list call stack (G_DL / G_ENDDL, 0x6e0..0x71f and 0x2c4..0x2f4) is **not** a matrix stack: it is at DMEM 0x398.. (4-byte DRAM return addresses, depth byte at 0x148,
18 entries, limit test `depth > 68`); it is the display-list-control area. It is not touched by anything here and not covered by my harness runs.

## 1. G_MTX (opcode 0x01, class 0). Handler 0x554 -> main loop 0x09c

Entry state set by the loop/dispatch: `$1 = (w0 >> 16) & 0xff` (flags), `$18 = w0 & 0x3ff`, `$22 = 0xae0`, `$29 = 0x110`, Mtx in DMEM 0xae0.

Flag bits (F3DEX 1.x encoding, differs from F3DEX2): bit 0 = projection (else modelview), bit 1 = load (else multiply), bit 2 = push (modelview only; ignored for
projection). Bits 3..7 are ignored. (These look like gbi.h `G_MTX_PROJECTION 1`, `G_MTX_LOAD 2`, `G_MTX_PUSH 4` of F3DEX 1.x as far as recalled; F3DEX2 uses a different encoding. The code is the authority: it tests `$1&1`, `$1&2`, `$1&4`.)

Effects, in this order:

1. DMEM byte 0x12c (the top byte of the light pointer word) := 0xff. Always, for every flag combination (including projection, load and a refused push).
   The low 24 bits are kept. This makes the word negative.
2. If projection: `cur = 0x420`. Else `cur = 0x3e0`, and if push (bit 2):
   * `ptr = [0x134]`, `lim = [0x15c]`. If `ptr != lim` (equality, not a range test): `[0x134] := ptr + 64`, and the current modelview matrix (64 bytes of 0x3e0, taken
     **before** the new one is computed) is DMAed to RDRAM `ptr`. The DMA is the three cp0 writes `SP_MEM_ADDR := 0x3e0`, `SP_DRAM_ADDR := ptr`, `SP_WR_LEN := $18`
     (= `w0 & 0x3ff`, 63 in a normal game), followed by a wait for the DMA to finish. `SP_DRAM_ADDR` is masked to 24 bits by the cp0 write, no segment translation of `ptr`.
   * If `ptr == lim` (stack full): nothing is pushed, no DMA, **the matrix operation is still carried out** on the current matrix (silent overflow). If `ptr > lim` the
     push happens (only equality stops it).
3. Load (bit 1): `cur[0..63] := DMEM[0xae0..0xb1f]` byte for byte (all 64 bytes, 16-byte units; no arithmetic).
   Multiply (bit 1 clear): `new = In * Cur` with **In on the left** (the matrix of the command), `Cur` on the right; `new` is written to `cur`
   and also left in the scratch 0xde0..0xe1f (same Mtx layout: ints at 0xde0..0xdff, fractions 0xe00..0xe1f).
4. Always (every combination, projection or modelview, load or multiply): `MVP := ModelView(0x3e0) * Projection(0x420)` into 0x460 (64 bytes, all of them rewritten).

### 1.1 The matrix product (used for `In*Cur` and for the MVP)

For rows `r` and columns `c` of `R = X * Y`:

```
acc  = 0                                        // 48-bit accumulator, wraps mod 2^48 (never reached by real data)
for k in 0..3:
    acc += floor(X[r][k] * Y[k][c] / 65536)     // each term floored separately; = (xf*yf >> 16) + xi*yf + xf*yi + ((xi*yi) << 16), xf/yf unsigned, xi/yi signed
R.int  = sclamp(acc)   = clamp16(acc >> 16)
R.frac = uclamp(acc)   = (acc in [-2^31, 2^31)) ? acc & 0xffff : (acc > 0 ? 0xffff : 0)
```
i.e. `R = clamp32(sum_k floor(X[r][k]*Y[k][c]/65536))` stored as halves (the two clamps are consistent: positive overflow gives 0x7fff:0xffff, negative 0x8000:0x0000).
This is the VMUDH(0) / 4 x { VMADL, VMADM, VMADN, VMADH } chain of the subroutine at 0x5fc (vu_primer 1.2, "A x B" row); the subroutine does rows 0,1 then rows 2,3, with the
second operand's row `k` duplicated in both lane halves and the first operand's element `k` broadcast by loading the quadword at `base + 2k`.
Entry convention of the subroutine: `$1` = X address, `$2` = Y address, `$3` = destination, return address in `$31`; it writes only the 64 destination bytes (no scratch).
The first accumulate operand being `In` (G_MTX) or `ModelView` (MVP) was established by the harness (the swapped order fails, mutation 1).

The three call sites: G_MTX multiply (X = 0xae0, Y = `cur`, dest 0xde0), and the tail that is shared by G_MTX (load/multiply) and G_POPMTX: X = 0x3e0, Y = 0x420, dest 0x460
(`$3 = 0x460` set by the caller, the tail itself sets `$1 = 0x3e0`, `$2 = 0x420` at pc 0x5bc and returns to 0x09c through DMEM[0xba] = 0x109c).

## 2. G_POPMTX (opcode 0xbd). Handler 0x204 -> 0x09c

There is **no byte count** operand: w0/w1 are ignored; exactly one 64-byte entry is popped.

1. DMEM byte 0x12c := 0xff (before anything else, so also when nothing is popped).
2. `ptr = [0x134]`, `base = [0xfe0]` (the original OSTask `dram_stack`). If `(int32)(base - ptr) >= 0` (pointer at or below the base): return to 0x09c, nothing else
   (underflow is silently ignored; no clamping, no error).
3. Else `ptr -= 64`; DMA read RDRAM[ptr .. ptr+63] -> DMEM 0x3e0 (`SP_MEM_ADDR := 0x3e0`, `SP_DRAM_ADDR := ptr`, `SP_RD_LEN := 63`; wait for completion);
   `[0x134] := ptr`; then the shared MVP tail (section 1, step 4): `MVP := MV(0x3e0) * Proj(0x420)` into 0x460.
   Only the modelview matrix is restored; the projection matrix is untouched. No scratch is written.

## 3. When MVP (0x460) is written, and other writers of 0x460

Written by: every G_MTX (any flags), every G_POPMTX that pops. Not changed by a refused pop. Always the complete 64 bytes.
(Other writers, read from the data tables only, not tested: G_MOVEWORD index 0 writes a word at 0x460 + offset; G_MOVEMEM types 0x98..0x9e write 16 bytes to 0x470/0x480/0x490/0x460
(`gSPForceMatrix`/`InsertMatrix`); those replace the MVP without touching 0x3e0/0x420 and do not set the light flag.)

## 4. Light-direction recomputation (lighting overlay ov2, pc 0x7c4..0x8b0)

### 4.1 The flag word 0x12c

* Set (made negative, top byte 0xff) by G_MTX (all variants) and by G_POPMTX (even a refused pop): section 1 / 2.
* Set by G_MOVEWORD index 2 (`G_MW_NUMLIGHT`): the word is replaced by `w1`; the gbi macro sends `NUML(n) = (n+1)*32 + 0x80000000`. (Moveword table at 0x30e: index 2 -> 0x12c.)
  The microcode adds no flag itself: a positive `w1` would not trigger a recomputation.
* Read in the lighting overlay start (pc 0x72c): `$1 = [0x12c]`; if negative -> recomputation. The overlay is only entered from the vertex loop when lighting is on
  (`[0x114] & 0x20000`, vertex code 0x408).
* Cleared only by the recomputation: `[0x12c] := $1 & 0xfff` (a positive word). Vertices processed without lighting never clear it, so it stays pending until the first lit vertex.
  Its low 12 bits are the **light pointer** `p0` = `(numlights + 1) * 32` in a normal game (the value is used as an offset, see below).

### 4.2 Inputs / outputs

Inputs: `p0 = word & 0xfff`; the modelview matrix at 0x3e0 (the upper-left 3x3 block, plus element [0][3]); MVP row 3 (DMEM 0x460 + 24 and +56, see lane 3 below; these are the
vector registers v11/v15 as the vertex handler loaded them from 0x460 at its start, which the recomputation clobbers and restores by jumping to 0x688 at the end); the light slots'
original directions; constants at DMEM 0x208..0x20d (0xe001 = -8191, 0x1fff = 8191, 0x0004).

Light slots: 32 bytes each at `0x1b0 + p`, `p = 32*slot` (slot 0/1 = the lookat Y / X records written by G_MOVEMEM types 0x82/0x84 via the table at 0x2f0, slot 2.. = lights L0.., then the ambient;
the movemem copies 16 bytes `{col[3], pad, colc[3], pad, dir[3], pad, 4 pad}` to `0x1b0 + p`). The original direction `dir` is at `0x1b8 + p` (3 signed bytes; the 4th, `pad3`, is not used);
the output ("transformed direction") is 8 bytes at `0x1c0 + p`.

Loop: `p = p0; repeat { process(p); if (p <= 0) break; p -= 32; }` - so the slots `p0, p0-32, ..., 32, 0` are all processed (`p0 = 0` processes one slot, slot 0). This includes the lookat
slots and the ambient slot (whose "direction" bytes are whatever are in DMEM); no check on the bytes. (Verified for multiples of 32 only; other values of `p0` untested.)

Per slot `p`, let `d_k = (int8) DMEM[0x1b8 + p + k]`, `k = 0..2`, `dd_k = 128 * d_k` (exact: LPV puts `d<<8`, VMULF by 0x4000 halves it), and let the 3x3 modelview block be
`M[j][k]` (rows `j`, columns `k`; int part `I[j][k]` at 0x3e0 + 8j + 2k, fraction part `F[j][k]` at 0x400 + 8j + 2k). For lanes `j = 0,1,2` the arithmetic is

```
accF = sum_k F[j][k] * dd_k                       // unsigned frac x signed, exact in 48 bits (VMUDN/VMADN)
t    = clamp16(accF >> 16)                        // VMADM by 0: sclamp
acc  = 2*t + sum_k 2 * I[j][k] * dd_k             // VMUDM by 2 restarts the accumulator (drops accF's low 16 bits), VMACF x3
N_j  = clamp32(acc)  as (Nh = clamp16(acc>>16), Nl = uclamp(acc))     // ~ 256 * (M[j] . d)  in plain units: row j of M dotted with d
```
That is `N = M * d` as a column vector product, i.e. `d_obj = d * M^T` (the transpose of the modelview 3x3, no inverse, no translation): the direction is rotated back into model space.
(The transposition is done by 4 STV + 4 LTV through the scratch; the net effect on lanes 0..2 of v8..v10 / v12..v14 is that lane `j` of the register holding column `k` contains `M[j][k]`; checked by an in-spec
assertion, `-DMTX_SPEC_SELFCHECK`.)

Lane 3 (the 4th output byte, DMEM 0x1c3) is computed with the same formulas but a pseudo row 3 `[ M[0][3], MVP[3][1], MVP[3][2] ]` (int parts: 0x3e0+6, 0x460+24+2, 0x460+24+4; fraction parts 0x400+6,
0x460+56+2, 0x460+56+4), stale vector registers. It only has to be reproduced for a bit-exact DMEM; nothing in the vertex code consumes the pad byte as far as I can see.

Normalisation (all 32-bit quantities as (hi:lo) pairs, all chains of vu_primer 1.2):

```
Q_j  = floor(N_j * N_j / 65536)       // VMUDL,VMADM,VMADN,VMADH; Ql = uclamp of the accumulator BEFORE the VMADH (no hi*hi term), Qh = sclamp after it
pass 1 (lane j):  s = Ql[j] + Ql[j|1];  lo1[j] = s & 0xffff;  hi1[j] = clamp16(Qh[j] + Qh[j|1] + (s >> 16))      // VADDC / VADD: saturating signed add of the integer halves
pass 2 (lane 2):  s = Ql[2] + lo1[0];   Slo = s & 0xffff;     Shi = clamp16(Qh[2] + hi1[0] + (s >> 16))
S    = (int32)(Shi << 16 | Slo)        // = Q0 + Q1 + Q2 in S15.16-like units (lane 3 only feeds pass 1 of lane 2, which pass 2 discards)
R    = VRSQ(S)                         // VRSQH/VRSQL pair, 32-bit double-precision input
T    = floor(R * 64 / 65536)           // pair (Ti = clamp16, Tf = uclamp): acc = (Rlo*64 >> 16) + Rhi*64
U_j  = floor(N_j * T / 65536)          // 32x32 chain; only the fraction half is used: u = (int16) uclamp(acc_final)
v_j  = min(max(u, -8191), 8191)        // signed compares against the constants at DMEM 0x208/0x20a
byte_j = (int16)((v_j * 4) & 0xffff) >> 8         // = floor(v_j / 64); constants 4 at 0x20c
```
VRSQ (this is the fork's `rsp_vrcp_vrsq<true>`, vu_primer 1.5 variant): `input = S` (signed 32); `mask = input >> 31; data = input ^ mask; if (input > -32768) data -= mask;`
`data == 0 -> R = 0x7fffffff`; `input == -32768 -> R = 0xffff0000`; else `shift = clz32(data); idx = ((data << shift) & 0x7FC00000) >> 22; idx = ((idx | 0x200) & 0x3FE) | (shift & 1);`
`R = (((0x10000 | rom[idx]) << 14) >> ((31 - shift) >> 1)) ^ mask`. The half of the ROM used (indices 512..1023, call it `rsq[j] = rom[512+j]`) is
`rsq[j] = isqrt((1<<42) / ((j + 512) >> (j & 1))) & 0xffff`, except `rsq[1] = 0xffff` (checked equal to `rsp_reciprocal_rom` in all indices hit by the tests and offline for all 512).
R's high half is written to element 0 of v11 and its low half to element 0 of v15 by VRSQH/VRSQL (the following chain reads them there); the dp flag/`div_in` state at entry is irrelevant (set by VRSQH).

Precision/scale: the result is the direction scaled so that its length is about **127** (`byte ~ 127 * N_j / |N|`, e.g. `d = (127,0,0)` with any uniform scale gives 127,0,0; `(100,60,-70)` gives `(94,56,-66)`);
for a (near) zero vector `S` is 0 or tiny and R saturates (0x7fffffff / 0x7fffc000...), giving garbage-but-deterministic bytes (e.g. `d=(1,0,0)` scaled by 0.5 gives 63): the spec reproduces them exactly.

Output stores, per slot: `SPV` of the 8 lanes to `0x1c0+p .. +7`, then the word at `0x1c0+p` is copied to `0x1c4+p`. Net result: bytes 0..3 = `byte_0..byte_3` (3 direction bytes + the lane-3 byte), bytes 4..7 = copy of bytes 0..3.
(Lanes 4..7 are computed from garbage but fully overwritten; they cannot influence lanes 0..3.)

After the loop: jump to 0x688 (reload v8..v15 from MVP), return to the lighting loop start (pc 0x72c) via `[0xa0]` (= 0x172c); the lit vertex is then processed from the top with the flag cleared.

### 4.3 Register/memory side effects relevant to bit-exactness

* DMEM written: `[0x12c]` (word := p0), the 8-byte output of each processed slot at 0x1c0+p, and the scratch below. Nothing else.
* Registers: v1..v15, accumulator, flags, div unit clobbered, but the lighting code restarts from its top with v8..v15 reloaded; `$1`, `$8`, `$31` changed.

## 5. Scratch written (nothing reads it afterwards)

| addr | writer | content |
|---|---|---|
| 0xde0..0xe1f | G_MTX multiply | the product `In*Cur` in Mtx layout (a copy of what goes to `cur`). Not read again. |
| 0xdf0..0xe2f | light recomputation | 32 halves: the STV skew of the group v8..v15 (modelview rows 0..2 ints v8..v10 / fractions v12..v14, MVP row 3 in v11/v15, each row stored twice), then partially overwritten: 0xdf0..0xdf7 = [F00,F10,F20,F03], 0xe00..0xe07 = [F01,F11,F21,MVPf31], 0xe10..0xe17 = [F02,F12,F22,MVPf32] (fractions of the modelview columns, the transposed block). 0xde0..0xdef not written. |
| (none) | G_POPMTX, MVP tail, load | - |

0xde0 is 0x100 above the start of the RDP output buffer (0xce0); the buffer is flushed by the producers and I assume it is empty between commands (not verified).

## 6. Differences between the builds

The handlers, the multiply subroutine, the light block and all DMEM offsets above are **instruction-for-instruction identical** in all six builds (pcmap "=" over the whole range; compared by diffing the
disassembly with branch targets removed). Only the pcs move:

| build | G_MTX | G_POPMTX | pop tail (after DMA) | MVP tail | light entry | lighting loop restart (= exit) | DMA-read write of SP_RD_LEN |
|---|---|---|---|---|---|---|---|
| banjo121 | 0x554 | 0x204 | 0x228 | 0x5bc | 0x7c4 | 0x72c | 0xff4 |
| sf64_non122 | 0x574 | 0x204 | 0x228 | 0x5dc | 0x7e4 | 0x74c | 0xff4 |
| ts2_f3dex123 | 0x54c | 0x1fc | 0x220 | 0x5b4 | 0x7bc | 0x724 | 0xff4 |
| ts2_f3dlx123 | 0x578 | 0x1f4 | 0x218 | 0x5e0 | 0x7e4 | 0x74c | 0xff4 |
| mk64_f3dex95 | 0x5dc | 0x2b4 | 0x2d8 | 0x644 | 0x84c | 0x7b4 | 0x15c |
| mk64_f3dlx95 | 0x608 | 0x2ac | 0x2d0 | 0x670 | 0x874 | 0x7dc | 0x15c |

In every build the main loop is 0x09c; mk64 reads its return address from DMEM 0xbe instead of 0xba (both 0x109c). The ov2 light code sits at the pc given in the table (the block ends with the jump to the
`0x688` equivalent). The stack and flag offsets (0x134, 0x15c, 0xfe0, 0x12c), scratch addresses and constants (0x208..) are identical; the init code of every build sets 0x134 and 0x15c the same way. NoN and F3DLX do not differ here.

## 7. Verification

Harness (`rt_test`, DMEM and SP/DPC registers compared against the JIT, x86 build): 8000 random cases per routine, seed 11, plus 6000 (seed 7) earlier, **0 differences on all six builds**
for `spec.f3dex.mtx.{mtx,pop,poptail,mvp,light}`; also 3000 cases with `-DMTX_SPEC_SELFCHECK -DMTX_SPEC_CHECK_ROM` (closed-form register contents after the transposition; the VRSQ ROM formula against the fork's table
at every index used) on banjo121, mk64_f3dlx95 and ts2_f3dex123.

Scenarios: `mtx`: all 8 flag combinations plus random high bits, random 64-byte Mtx at 0xae0 (kinds: random, small, scaled rotation + translation, edge values), random stack pointer/limit incl. full (ptr == lim),
limit below the pointer, random `$18`, random 0x12c; the push DMA is checked through the cp0 register writes (SP_MEM_ADDR/DRAM_ADDR/WR_LEN) which the harness compares. `pop`: pointer above, equal, below the base, unaligned, random; compared up to the
write of SP_RD_LEN (the standalone JIT stops there; there is no RDRAM), so the **DMA read itself and the DMEM 0x3e0 it fills are not tested** - the HLE must copy 64 bytes from RDRAM `ptr-64` into 0x3e0. `poptail`: the
rest (pointer store, MVP). `mvp`: MV*P alone. `light`: random p0 (mostly `(n+1)*32`, n=0..7, sometimes any multiple of 32 up to 0x100), random light/lookat bytes (random, 0, edge, unit vectors), rotation/random/edge modelview
matrices, `v11/v15` set from MVP row 3 of DMEM as the vertex handler would, flag word with random upper bits.

Mutation checks (banjo121, 1500 cases, mutants selectable with `-DMTX_MUTATE=n`): 1 operand order of the multiply, 2 floor of the sum instead of sum of floors, 3 push refused for `ptr < lim` instead of `!=`, 5 pop underflow `> 0` instead of `>= 0`,
6 pop step 32, 7 direction scale 127, 8 no accumulator restart, 9 no saturation in the S add, 10 VRSQ index parity dropped, 11 lanes 4..7 not copied, 12 scratch round trip skipped, 13 loop runs one slot too far,
14 wrong upper clamp, 16 load/push flags swapped, 17 wrong MVP row 3 source, 18 wrong flag byte: **all detected** (3 first slipped through when the generator never had a limit below the pointer; generator extended, then detected).

Not verified: other values of `p0` (not multiples of 32), the DMA read of POPMTX and the data movement of the push (only its register writes), execution on the aarch64 builds, bit encoding names against the original gbi.h (derived from the code),
G_MOVEWORD/G_MOVEMEM writers of 0x460 and 0x12c (read from tables only), whether the pad byte 0x1c3 is consumed anywhere.

## 8. Open questions

* The pad byte at 0x1c3/0x1c7 of a light slot depends on MVP row 3 elements (stale vector registers); reproduced exactly but its consumer (if any) is unknown.
* Lookat slots 0/1 and the ambient slot are transformed too (meaningless for the ambient); harmless but they are DMEM writes the HLE must reproduce.
* G_MOVEWORD NUMLIGHT is the only way a plain light-pointer update re-arms the recomputation; the effect of a positive `w1` (never produced by gbi.h) is "no recomputation".
