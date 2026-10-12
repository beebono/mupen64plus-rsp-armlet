# RDP output side of F3DEX 1.x / F3DLX: output buffer, pass-through, flush, FIFO

Area "rdp". Reference build: F3DEX 1.21 (`banjo121`); all pcs below are banjo121 unless a table says otherwise. Builds: `banjo121` (F3DEX 1.21), `sf64_non122` (NoN 1.22),
`ts2_f3dex123`, `ts2_f3dlx123` (F3DLX 1.23), `mk64_f3dex95`, `mk64_f3dlx95` (F3DLX 0.95). Everything F3DLX-specific is marked "F3DLX:". "Verified" = a spec routine in
`tools/native-f3dex/spec/rdp_spec.c` (+ `rdp_gen.inc`) matched the JIT (DMEM, SP/DPC registers, exit pc) over 20000 random cases on each of two seeds (1, 7) on all six builds, unless the text names fewer; "read" = taken from the code, not run.

## 0. Summary (what the HLE must reproduce)

* Every RDP command the microcode produces is first stored into a DMEM output buffer at **0xce0** (pointer in `$23`), then **flushed at once**: DMA of the buffer to the RDRAM FIFO,
  DRAM pointer `D[0x128]` advanced, `DPC_END` written. The flush runs after **every** pass-through command, after RDPHALF_2, after SETOTHERMODE_H/L and after **every
  non-culled triangle**. So `$23 == 0xce0` at every command boundary, the buffer never holds more than one command (max 0xb0 bytes), and the RDP sees the stream in chunks
  of 8 bytes (pass-through) or 32..176 bytes (triangle).
* The FIFO is a ring in RDRAM `[D[0x150], D[0x154])` (start, end address). A chunk that would not fit before the end makes the writer jump back to the start (DPC_START write).
  At the start of each task the DRAM pointer is set to the FIFO **end**, so the first flush of a task always wraps to the FIFO start (synchronous RDP, sections 5.1 and 8).
* Class-3 commands (opcodes 0xc0..0xff) are copied verbatim (8 bytes) except 0xfd/0xfe/0xff (SETTIMG/SETZIMG/SETCIMG), whose address word goes through the segment table.
  TEXRECT/TEXRECTFLIP (0xe4/0xe5) are *just* the first 8 bytes; the other 8 come from RDPHALF_1 + RDPHALF_2 (section 3).
* F3DLX: the flush does not write `DPC_END`; the callers do it, lazily (section 9). The bytes in RDRAM and the DMEM end state are the same as in F3DEX; only when `DPC_END`
  changes differs, and the last triangle of a task can stay unpublished.

## 1. DMEM variables and registers

DMEM byte addresses; `$29 = 0x110` in all builds (the microcode addresses most of these as `n($29)`). All 32-bit big-endian.

| addr | size | name | meaning |
|---|---|---|---|
| 0x10c | 4 | half1 | the `w1` of the last RDPHALF_1 (`-4($29)`); initial 0 (never cleared) |
| 0x118 | 4 | om_h | cached RDP SET_OTHER_MODES word 0 (opcode 0xef + the "H" bits); data initial 0xef080cff |
| 0x11c | 4 | om_l | cached SET_OTHER_MODES word 1 (the "L" bits); initial 0 |
| 0x128 | 4 | dram_ptr | RDRAM address at which the next flushed chunk is written (`24($29)`). Raw value (not masked) |
| 0x150 | 4 | fifo_start | OSTask.output_buff (`64($29)`) |
| 0x154 | 4 | fifo_end | OSTask.output_buff_size (`68($29)`): despite the name an END ADDRESS, compared with addresses |
| 0x160..0x19f | 16x4 | segment table | base per segment number; used by the pass-through (written by MOVEWORD G_MW_SEGMENT) |
| 0xce0.. | up to 0xb0 | output buffer | RDP command bytes (big-endian words, as the RDP reads them). 0xce0 + 0xb0 = 0xd90 at most |
| 0xfe8, 0xfec | 4, 4 | OSTask.output_buff, output_buff_size | read once by the task start (section 8) |
| 0xbf0 | 4 | yield save of `$23` | `$23` is 0xce0 whenever a yield can happen |

Registers: `$23` output pointer (DMEM address, 0xce0 at every command boundary; set to 0xce0 by the task start and by the last instruction of every flush); `$25/$24` = `w0/w1`
of the current command; `$27` points after the command in the DL buffer; `$21` holds the flush's return address (`$31` at entry); `$31` link; `$2` is the "next" selector described
in section 2.4; `$17`, `$18`, `$19`, `$20`, `$11` are the DMA helper registers (section 6).

cp0 numbers (as printed by the disassembly): 0 SP_MEM_ADDR, 1 SP_DRAM_ADDR, 2 SP_RD_LEN, 3 SP_WR_LEN, 4 SP_STATUS, 5 SP_DMA_FULL, 6 SP_DMA_BUSY, 8 DPC_START, 9 DPC_END,
10 DPC_CURRENT, 11 DPC_STATUS (bit 0 XBUS_DMEM_DMA, bit 10 START_VALID).

Register semantics in the fork's JIT (`ext/parallel-rsp/rsp/cp0.cpp`), which is what the task-level HLE has to match:
SP_MEM_ADDR write: `v & 0x1fff`; SP_DRAM_ADDR write: `v & 0xffffff`; SP_WR_LEN write stores `v` and performs the DMA at once (DMEM -> RDRAM, rows = `((v>>12)&0xff)+1`,
row length `((v&0xfff)+1)` rounded up to 8, skip `v>>20`); afterwards (read, not run in the harness) SP_MEM_ADDR/SP_DRAM_ADDR have advanced by the bytes moved and SP_WR_LEN = 0xff8.
DPC_START write: `START = CURRENT = END = v & ~7`. DPC_END write: `END = v & ~7`, then the video plugin's `ProcessRdpList` runs inside the write. DPC_CURRENT writes are ignored.
The video plugin (foir `parser.c:376`, and arMLEt's `rdp_defer.c`) leaves `START = CURRENT = END = end` after a list, and ignores a list with `end <= current`.

## 2. The class-3 pass-through (opcodes 0xc0..0xff)

### 2.1 Dispatch
First-level table at DMEM 0xb8 (banjo; 0xbc on mk64) indexed by `(w0>>29)&6`: class 3 -> pc **0x358**. Entered with `$25 = w0`, `$24 = w1`, `$23` = buffer pointer.
(Class 2, opcodes 0x80..0xbf, has per-opcode entries; the ones that emit RDP commands are RDPHALF_2 and SETOTHERMODE_H/L, sections 3 and 4.)

### 2.2 What it does (verified, all six builds: spec `pt.class3`)
```
op = (int32)w0 >> 24                       // 0xc0..0xff => -64..-1
if (op + 3 >= 0)                           // 0xfd SETTIMG, 0xfe SETZIMG, 0xff SETCIMG
    w1 = D32[0x160 + ((w1 >> 24) & 15) * 4] + (w1 & 0x00ffffff)      // 32-bit wrap; bits 31..28 of w1 ignored
store32(D, $23, w0); store32(D, $23 + 4, w1);   // F3DLX: preceded by DPC_END = D32[0x128] (section 9)
$23 += 8; flush(); then the exit selector (2.4)
```
* Everything else (SETTILE, LOADBLOCK, LOADTILE, LOADTLUT, SETTILESIZE, SETSCISSOR, SETCOMBINE, SETOTHERMODE(0xef), colour commands, FILLRECT, SYNC_*, SETKEY*, SETCONVERT,
  SETPRIMDEPTH, raw RDP triangles 0xc8.., 0xc0 no-op) is copied bit for bit: `w0` unmodified (including its top two opcode bits = 11, which the RDP ignores), `w1` unmodified.
  No state is kept in DMEM for any of them (a raw 0xef does **not** update the cache at 0x118/0x11c).
* Segment translation: segment number = `(w1>>24)&0xf`; result = `seg[n] + (w1 & 0xffffff)`, a plain 32-bit add (no masking of the sum; the RDP uses the low 24 bits). On
  mk64 the routine masks with the DMEM word 0xb8 (= 0x00ffffff), identical. `w0` of SETTIMG/SETCIMG (format, size, width) is untouched.
* SYNC_FULL (0xe9) has no special handling here; the RDP raises the DP interrupt when it executes it (after the flush's `DPC_END`).
* An 8-byte DMA and `DPC_END` advance by 8 per command, also for TEXRECT (so the RDP gets a 16-byte command in two lists; foir's parser keeps the partial command).

### 2.3 Entry/exit pcs
Entry = stage pc of the table in section 10. The segment routine (`jal` at 0x368, routine 0x10c..0x124) is the only subroutine. Join point 0x374 (stores) is shared with RDPHALF_2 and SETOTHERMODE.

### 2.4 After the flush: yield check or not (`$2`)
The flush returns to the instruction after the `jal` (0x384 `bgtz $2`): `$2 > 0` (signed) -> **0x09c** (display-list loop *with* the SP_STATUS SIG0 "yield requested" test),
else -> **0x0ac** (loop without that test). `$2` at that point:
* class 3: `$2 = op + 27` (computed as `op + 3` then `+ 24`, both for the translated and the verbatim case). So opcodes 0xc0..0xe5 skip the yield test, 0xe6..0xff do the test.
* RDPHALF_2: `$2` = the handler address from the dispatch (positive) -> always 0x09c. RDPHALF_1 jumps to 0x0ac directly (no flush).
* SETOTHERMODE_H/L: `$2 = old_word & ~mask` (section 4), so the test is done only if that value is positive as a signed 32-bit number.
The test only matters when the CPU sets SIG0 (yield request); a task-level HLE that never yields can ignore the difference. Verified in `full_pt`, `full_h2`, `full_om` and `post`.

## 3. RDPHALF_1 / RDPHALF_2 and the texture rectangle

Opcodes: banjo, NoN, ts2 (both): RDPHALF_1 = **0xb4**, RDPHALF_2 = **0xb3** (and 0xb2 is MODIFYVTX). mk64 (both, from the handler tables): RDPHALF_1 = **0xb3**, RDPHALF_2 = **0xb2**, and
0xb4 is a different command (`D16[0x112] = w1 & 0xffff`, perspective-norm word). (The numbers follow from the DMEM tables at 0x76 + 2*(op&0x7f); the games' own display lists were not checked.)

A texture rectangle is three display-list commands: `TEXRECT(e4|e5) w0,w1` (class 3, copied: first 8 bytes of the RDP command, flushed alone), `RDPHALF_1 (w1 = s<<16|t)`, `RDPHALF_2 (w1 = dsdx<<16|dtdy)`.
* RDPHALF_1: `D32[0x10c] = w1`; jump to 0x0ac. No output, no flush. `w0` ignored. (Verified `half1`.)
* RDPHALF_2: stores `(D32[0x10c], w1)` as the next 8 bytes (`w0` ignored; the saved word is stale or 0 if no RDPHALF_1 preceded), flushes, `$2` positive -> 0x09c. (Verified `half2`, `full_h2`.)
So the RDP stream is `e4.. w1 | s,t | dsdx,dtdy`, 16 bytes, in two chunks. There is no RDPHALF_CONT in these builds.

## 4. SETOTHERMODE_H (0xba) / SETOTHERMODE_L (0xb9): a pass-through producer

`w0 = op<<24 | shift<<8 | len` (byte 2 = shift, byte 3 = len, read from the DL buffer at `$27-6` / `$27-5`), `w1 = data`. Banjo entry 0xba -> 0x274, 0xb9 -> 0x27c.
```
a   = (op == 0xba) ? 0x118 : 0x11c
mask = ((1u << (len & 31)) - 1) << (shift & 31)        // sllv: shift counts use the low 5 bits; len == 32 gives mask 0 (nothing cleared)
keep = D32[a] & ~mask
D32[a] = w1 | keep                                      // w1 is NOT masked to the field
emit8(D32[0x118], D32[0x11c])                           // the whole cached SET_OTHER_MODES command (0xef08.... normally)
flush(); $2 = keep (signed test, 2.4)
```
Verified (`omode.*`, `full_om.*`). The cached words (0x118/0x11c) are read by nothing else in the RDP path; G_RDPSETOTHERMODE (raw 0xef) bypasses them.

## 5. The flush (banjo pc 0x128), F3DEX builds

Entry with `$23` = buffer pointer, return address `$31` (saved to `$21`; the triangle code arranges `$31 = $24` so the flush returns straight to the triangle's caller).
There is no "buffer fill" threshold: the flush is called after every command and sends whatever is in the buffer (anything with `len > 0`).
Let `len = $23 - 0xce0` (signed), `ptr = D32[0x128]`, `fend = D32[0x154]`, `fstart = D32[0x150]`.

```
flush():
  if (len <= 0) goto done                         // nothing: no DMA, no DPC write
  if ((int32)(fend - (ptr + len)) < 0) {          // would run past the FIFO end (equal is OK)
      while (DPC_STATUS & 0x400) ;                // START_VALID: previous DPC_START not yet taken
      while (DPC_CURRENT == fstart) ;             // 32-bit raw compare
      DPC_START = fstart;                         // HW: new list start; fork: START = CURRENT = END = fstart & ~7
      ptr = fstart                                // the fit is NOT re-checked
  }
  for (;;) {                                      // do not overwrite what the RDP has not read
      cur = DPC_CURRENT;
      if ((int32)(ptr - cur) >= 0) break;         // RDP is behind the write position
      if ((int32)((ptr + len) - cur) < 0) break;  // RDP is beyond the end of the write
  }                                               // else cur in (ptr, ptr+len]: poll again
  newend = ptr + len
  while (SP_DMA_FULL) ;                           // helper 0xfd8
  SP_MEM_ADDR = 0xce0; SP_DRAM_ADDR = ptr; SP_WR_LEN = len - 1;      // in this order; the DMA starts with the last write
  while (SP_DMA_BUSY) ;                           // helper 0xfc4
  D32[0x128] = newend                             // stored just before the wait, i.e. also before DPC_END
  DPC_END = newend                                // F3DLX: NOT here, see 9
done:
  $23 = 0xce0; return $21
```
The decision part (empty / wrap / fits -> pcs 0x1a4 / 0x14c / 0x168) is verified (`fdec`, 3 exits all hit, including the equality boundary), the wrap path from the entry to the
return (`ffull`: DPC_START write, cr[8..10], DMA registers, DMEM 0x128, DPC_END), the fits path from the third loop on (`fnow`, entered with DPC_CURRENT = DPC_END = ptr & ~7), the tail alone
with arbitrary `$18/$19` including addresses above 24 bits (`ftail`: SP_MEM_ADDR = 0xce0, SP_DRAM_ADDR = ptr & 0xffffff, SP_WR_LEN = len - 1, 0x128, DPC_END), the DMA write helper (`dmaw`).
The DMA itself is not performed by the standalone harness (only the register writes are compared); the transfer is DMEM[0xce0 .. 0xce0+len) -> RDRAM[ptr & 0xffffff & ~7 ..], read from `rsp_dma_write`.

Notes. No "+8/-8" adjustments exist in these builds. `len` is always a multiple of 8 in practice (8 for pass-through, 32 + 64a + 64b + 16c for triangles); DPC_END is `newend & ~7` by the register.
`dram_ptr` and the DPC registers are 32-bit raw (no 24-bit masking in the microcode); the DMA masks the address. A FIFO smaller than one chunk is not detected (the chunk is written past `fend`).

### 5.1 Which loops can spin with a synchronous RDP (DPC_CURRENT = DPC_END after each list, DMA instantaneous) — read
Assumptions, all true in this stack: (S1) `SP_DMA_FULL/BUSY` are never set (DMA completes inside the SP_WR_LEN write); (S2) `ProcessRdpList` runs inside the DPC_END write and leaves CURRENT = END;
(S3) DPC_STATUS bit 10 is never set by RSP-side writes (only the CPU-side register write path sets it); (S4) the DPC_START write sets CURRENT = END = START.
* START_VALID loop: never spins (S3). DMA_FULL / DMA_BUSY loops: never (S1).
* `while (DPC_CURRENT == fstart)`: spins forever only if CURRENT == fifo_start when the wrap is taken. CURRENT is the end of the last list, which is > its start >= fifo_start, or fifo_end after the
  task start; so only for a degenerate FIFO (`fifo_end & ~7 == fifo_start`).
* The third loop: after a wrap CURRENT = ptr & ~7 (<= ptr); without a wrap CURRENT = END = the last `newend` = ptr (F3DLX: DPC_END was brought to ptr right before). `ptr - cur >= 0` -> exits at once. It would spin only if the RDP
  position were inside (ptr, ptr+len], impossible when CURRENT <= END = ptr.
So with the synchronous RDP the flush is straight-line: [wrap if needed] -> DMA -> DPC_END. (The harness cannot set DPC_CURRENT, so the polling loops were run only through `fnow`/`ffull`, where they pass at once.)

## 6. DMA helpers (banjo 0xfc4..0xffc; mk64: wait at 0x12c, start at 0x13c/0x140)

* **0xfc4**: `while (SP_DMA_BUSY != 0) ; return with $17 = 0` (the `$17=0` is the delay slot of the `jr`, which is also the instruction at 0xfd4). Clobbers `$11`.
* **0xfd4**: `$17 = 0`, then falls into 0xfd8 (a DMA **read** helper).
* **0xfd8**: `while (SP_DMA_FULL != 0) ;  SP_MEM_ADDR = $20;  SP_DRAM_ADDR = $19;  if ($17 > 0 (signed)) SP_WR_LEN = $18 else SP_RD_LEN = $18;  return`. Clobbers `$11`.
  `$20` = DMEM address (bit 12 = IMEM), `$19` = DRAM address, `$18` = the length register value **already minus one** (bytes-1, rows<<12, skip<<20). No masking or +/-1 in the helper; the
  register write masks (0x1fff / 0xffffff) and the DMA rounds the length up to 8. The flush calls it with `$20 = 0xce0`, `$19 = ptr`, `$18 = len-1`, `$17 = 1` (set in the `jal`'s delay slot).
Verified for the write case (`dmaw`: `$17 > 0`); the read case cannot run in the harness (the JIT returns to its caller on SP_RD_LEN).

## 7. Triangles and the output buffer

The triangle code (other area) writes into the buffer starting at `$23` (= 0xce0) and advances `$23` as it reserves:
| group | bytes | present when (flags = DMEM[0x117], low byte of the geometry word 0x114) |
|---|---|---|
| edge/header (header byte 0 = `0xc8 | flags`, byte 1, 15 more halfwords; all 32 bytes are written) | 0x20 | always (`$23 += 0x20` at banjo pc 0xb94) |
| shade coefficients | 0x40 | flags bit 2 |
| texture coefficients | 0x40 | flags bit 1 |
| z coefficients | 0x10 | flags bit 0 |
Order edge, shade, texture, z. Total `len = 0x20 + 0x40*(f&4?1:0) + 0x40*(f&2?1:0) + 0x10*(f&1?1:0)` = the RDP triangle command length for command byte `0xc8|f` (verified at the flush
entry, `tri.tolen`, 6 builds, triangles produced by the real triangle code; mutation: a wrong group size is caught). A culled triangle writes nothing and does not flush. After the last group
the code calls the flush (banjo `jal 0x128` at 0xf10, once per triangle; F3DEX 1.23 0xf08, 0.95 0xf98, NoN 0xf30), with `$31` set to the return pc.
Leftover bytes: the previous command's bytes remain in the buffer beyond the new length; nothing reads them (the DMA only reads `len` fresh bytes; the yield DMA saves DMEM 0..0xbff only).
F3DLX: the same sizes; five exits into the flush: section 9.

## 8. Task start (banjo pcs 0x758..0x7bc; `$1 = 0xfc0`, the OSTask header; not run again after the overlays replace the window)

```
if (OSTask.flags & 1) -> resume path (0x82c, restores $23/$26..$28 from DMEM 0xbf0..; not examined further)
D32[0x150] = fifo_start = D32[0xfe8];  D32[0x154] = fifo_end = D32[0xfec]
keep = !(DPC_STATUS & 1) && !((int32)(fifo_start - DPC_END) > 0) && DPC_CURRENT != 0 && DPC_CURRENT != DPC_END     // RDP busy with a list in this FIFO
if (keep) ptr = DPC_END
else { while (DPC_STATUS & 0x400) ; DPC_STATUS = 1 (clear XBUS); DPC_START = fifo_end; DPC_END = fifo_end; ptr = fifo_end }
D32[0x128] = ptr;  $23 = 0xce0
```
Synchronous RDP: DPC_CURRENT == DPC_END always, so `keep` is false and every task starts with `DPC_START = DPC_END = fifo_end`, `ptr = fifo_end`, and the first flush wraps to `fifo_start`.
Verified (`init.fifo`, 6 builds, random DPC registers): the stores to 0x150/0x154/0x128, the DPC_START/DPC_END writes and the DPC_STATUS write (the harness stores the written value; the fork clears bit 0, same result for status 0).
Not exercised: the sub-conditions `DPC_CURRENT == DPC_END` and `DPC_CURRENT == 0` (random registers never hit them; a mutation of the first was not caught).
The end of the task has **no flush and no DPC_END write** (ov3: wait DMA idle, SP_STATUS write, break); every DL ends in SYNC_FULL whose pass-through flush publishes everything. A yield saves DMEM 0..0xbff.

## 9. F3DLX differences (ts2_f3dlx123 pcs; mk64_f3dlx95 in section 10)

The F3DLX flush (ts2dlx 0x120..0x198) is the F3DEX flush **without the wait for DMA idle and without `DPC_END`**; it ends with the DMA start, `D32[0x128] = newend`, `$23 = 0xce0`
(verified: `ffull`/`ftail`: DLX leaves cr[9] untouched; mutating it breaks the test). It uses `$6` instead of `$20` as temporary. `DPC_END = D32[0x128]` is written by the callers, after a wait for DMA idle (`jal 0xfc4`; the `lw`
of 0x128 sits in the delay slot, harmless):
* **pass-through / RDPHALF_2 / SETOTHERMODE** (ts2dlx join point 0x364): **W1** before the store: wait idle, `DPC_END = D32[0x128]` (publishes everything already DMAed, e.g. the previous triangle);
  store the 8 bytes; flush; **W2** after it (0x380): wait idle, `DPC_END = D32[0x128]` (the new pointer); then `bgtz $2`. Verified: `pt.class3`, `full_*` (W1 and W2), `post`.
* **triangle**: **W3** once, after the culling decisions and before the first coefficient store (0xaa8..0xac0): wait idle (poll SP_DMA_BUSY), `DPC_END = D32[0x128]`; then build, then `j 0x120` (flush, no DPC_END). Culled
  triangles do none of it. Verified at the flush entry (`tri.tolen`: `DPC_END == D32[0x128] & ~7` on every non-culled triangle, mutation caught). The exits into the flush are 0xc34 (no flags), 0xed8 (no z), 0xee0 (z group added in the delay slot), 0xee8 (no texture, no z), 0xf3c (z group), resulting in the same lengths as section 7.
* Consequence: after a triangle `DPC_END` lags `D32[0x128]` by that triangle until the next command's W1/W3. If a task ends right after a triangle (no SYNC_FULL) the last triangle's bytes are in RDRAM but never
  published. In RDRAM, DMEM and `D[0x128]` the end state is the same as F3DEX; `DPC_START/CURRENT/END` differ at the end of such a task.
* F3DLX 1.23 only: G_LOAD_UCODE (op 0xaf, 0xf80) also does wait idle + `DPC_END = D32[0x128]` before reloading (read). 0.95 has no such handler (0xaf is a no-op there; the F3DEX 1.21/1.23 handler at 0xf8c has no DPC_END).
* Before the first W the register state after the task start is `DPC_START = DPC_END = fifo_end`, `D[0x128] = fifo_end`: the first W1/W3 writes `DPC_END = fifo_end` again (harmless: the video plugin ignores `end <= current`).

## 10. Per-build pcs

| item | banjo121 | sf64_non122 | ts2_f3dex123 | ts2_f3dlx123 | mk64_f3dex95 | mk64_f3dlx95 |
|---|---|---|---|---|---|---|
| class-3 stage (`sra $2,$25,24`) | 0x358 | 0x358 | 0x350 | 0x348 | 0x3e0 | 0x3d8 |
| segment routine | 0x10c | 0x10c | 0x104 | 0x104 | 0x10c | 0x10c |
| `jal flush` of the common tail | 0x37c | 0x37c | 0x374 | 0x378 | 0x404 | 0x408 |
| F3DLX W1 / W2 (wait + DPC_END) | - | - | - | 0x364 / 0x380 | - | 0x3f4 / 0x410 |
| `bgtz $2` | 0x384 | 0x384 | 0x37c | 0x38c | 0x40c | 0x41c |
| RDPHALF_1 / RDPHALF_2 entry | 0x348 / 0x350 | same | 0x340 / 0x348 | 0x338 / 0x340 | 0x3d0 / 0x3d8 | 0x3c8 / 0x3d0 |
| RDPHALF opcodes | b4 / b3 | b4 / b3 | b4 / b3 | b4 / b3 | **b3 / b2** | **b3 / b2** |
| SETOTHERMODE_H (0xba) / _L (0xb9) | 0x274 / 0x27c | same | 0x26c / 0x274 | 0x264 / 0x26c | 0x324 / 0x32c | 0x31c / 0x324 |
| flush entry F | 0x128 | 0x128 | 0x120 | 0x120 | 0x168 | 0x168 |
| flush: wrap block / third loop / DMA setup | F+0x24 / F+0x40 / F+0x5c | same | same | same | same | same |
| flush: `jr $21` | F+0x7c | F+0x7c | F+0x7c | **F+0x74** | F+0x7c | **F+0x74** |
| DMA helpers: wait idle / start | 0xfc4 / 0xfd8 (0xfd4 = `$17=0` first) | same | same | same | 0x12c / 0x140 (0x13c) | same |
| triangle call of the flush | 0xf10 | 0xf30 | 0xf08 | 5 exits (9) | 0xf98 | 5 exits |
| triangle W3 (DLX) | - | - | - | 0xaa8..0xac0 | - | 0xb38..0xb50 |
| task start (entry..exit) | 0x758..0x7c0 | 0x778..0x7e0 | 0x750..0x7b8 | 0x778..0x7e0 | 0x7e0..0x848 | 0x808..0x870 |
The flush, DMA helpers, pass-through, halves and SETOTHERMODE code is instruction-for-instruction identical (modulo branch targets) in banjo121, sf64_non122, ts2_f3dex123 and mk64_f3dex95
(sf64_non122 diffed textually against banjo121; the other two by pcmap `=` over these ranges, and the same specs pass on them). Differences are only F3DLX (section 9), the mk64 opcode numbers and the segment mask word.
All of this code lies outside the overlay window except the task start, which is only valid with overlay 0 (the image the boot code loads).

## 11. Edge cases

* `$23 <= 0xce0` at the flush (nothing emitted): returns without any DMA or DPC write. Not reachable except via the tail of a command that stored nothing.
* FIFO smaller than a chunk, or `fifo_end` below `ptr`: wrap taken, `ptr = fifo_start`, no second check, the DMA may write beyond `fifo_end`.
* `ptr` (D[0x128]) not a multiple of 8: DMA address floors to 8, DPC_END is `newend & ~7`; the third-loop compare `ptr - (ptr & ~7) >= 0` still passes.
* SETOTHERMODE with `len == 0` or `len == 32`: mask 0 (nothing cleared, `w1` ORed in whole); `len > 32` or `shift > 31` use the low 5 bits.
* The segment translation of SETTIMG uses `w1` bits 27..24; bits 31..28 (e.g. 0x80 from a KSEG0 address) are discarded, an unset segment (table entry 0) yields `offset` alone.
* RDPHALF_2 without RDPHALF_1: stale `D32[0x10c]`.
* Raw RDP triangle opcodes 0xc8..0xcf in a DL are copied as 8 bytes only (the RDP would then misparse): no special handling.
* `D32[0x128]` after a wrap: `newend = fifo_start + len`; the word at 0x128 is **not** wrapped back when it equals `fifo_end` exactly (a chunk ending at `fifo_end` is allowed; the next one wraps).

## 12. What was verified and what was not

Harness: `SPEC_ONLY=rdp tools/native-f3dex/build_rt_test.sh x86 <dir>`; `rt_test --ucode kit/uc/<build> --routine spec.f3dex.rdp --cases N`. The standalone JIT performs no DMA and no RDP list: DMA register writes and DPC
registers are compared (the SP_MEM/DRAM/WR_LEN values as written), the DMA content and the RDP list are not. DPC_CURRENT, DPC_END, DPC_START in the harness start random; DMA_FULL/BUSY and DPC_STATUS read 0.
* All six builds (20000 cases per routine on seeds 1 and 7, 17 routines, 0 differences): `pt.class3`, `half1.rdphalf1`, `half2.rdphalf2`, `omode.seth/setl`, `post.branch`, `fdec.decide`, `ffull.flush`, `fnow.fits`, `ftail.dma`, `dmaw.helper`,
  `tri.tolen` (about 40% of the generated triangles reach the flush; the rest are culled), `init.fifo`, `full_pt.class3`, `full_h2.rdphalf2`, `full_om.seth/setl` (whole command + flush [wrap path] + F3DLX's W2 + the `$2` branch).
* Mutation checks (a deliberate error in the spec, caught): tri group sizes (shade/tex/z), segment mask, SETOTHERMODE shift mask, DMA length `-1`, DPC_END value, F3DLX W1/W2/flush-tail variants, decision boundary
  `>= 0`, `$2` offset (op+26 caught by the boundary opcode 0xe5), RDPHALF source word. **Not caught** (not exercised): the `init` sub-conditions DPC_CURRENT==DPC_END / ==0, the second condition of the third wait loop
  (only reachable with CURRENT inside the write range, which a synchronous RDP never produces), the wrap with equality `fend == newend` inside `ffull` (covered by `fdec`).
* Read only: the real DMA transfer and the final SP register values after it, the polling loops with non-trivial register values, the resume-from-yield path, F3DLX 1.23's G_LOAD_UCODE DPC_END, the behaviour on a
  real asynchronous RDP, the opcode numbers as used by the games' display lists (mk64).

## 13. Open questions

1. mk64 (F3DEX 0.95): RDPHALF_1/2 are 0xb3/0xb2 by the ucode's tables, 0xb4 stores a halfword at 0x112. Not checked against an MK64 display list that draws a texture rectangle.
2. The init `keep` condition (a continuing RDP list) is unreachable under the synchronous model but its two sub-conditions are untested by the harness.
3. F3DLX: whether any game's DL ends a task right after a triangle (final `DPC_END` lag). The HLE should reproduce the lag exactly or at least the final `DPC_START/CURRENT/END`.
4. Video plugins other than foir: `ProcessRdpList` leaving CURRENT == END is assumed (S2); with a plugin that does not, the third loop and the init `keep` path would behave differently.
