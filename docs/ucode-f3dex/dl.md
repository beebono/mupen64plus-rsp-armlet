# Display-list engine and task framing of F3DEX 1.x / F3DLX (area "dl")

Covers: the OSTask header and the task start (fresh / resume), the DMEM state variables, the SP DMA helper, the command loop (buffer, refill, dispatch, prefetch DMA,
segment translation, yield test), G_DL, G_ENDDL, G_BRANCH_Z, G_CULLDL, G_SPNOOP, G_LINE3D, unknown opcodes, G_RDPHALF_1 (as the BRANCH_Z operand), the end of the task
and the yield. The flush of the RDP output buffer, the pass-through details, the state commands, the matrix stack, vertices, triangles and lighting belong to other areas
(`rdp.md`, `state.md`, `mtx.md`, `vtx`/`light.md`, `clip.md`); where this document needs a fact from them it says so.

Reference build: F3DEX 1.21 (`banjo121`); every pc below is a banjo121 pc unless a build is named. Builds: `banjo121` F3DEX 1.21, `sf64_non122` F3DEX.NoN 1.22,
`ts2_f3dex123` F3DEX 1.23, `ts2_f3dlx123` F3DLX 1.23, `mk64_f3dex95` F3DEX 0.95, `mk64_f3dlx95` F3DLX 0.95. "1.2x" = the first four.

Status words: **verified** = a spec routine (`tools/native-f3dex/spec/dl_spec.c` + `dl_gen.inc`, `spec.f3dex.dl.*`) agrees with the JIT on DMEM, exit pc, the SP/DPC
registers and all 31 GPRs over 20000 random cases (section 9); **observed** = seen in a whole-task run of the JIT with a scratch driver that emulates RDRAM and the SP DMA
(section 9; the driver is mine, not the repository harness); **read** = taken from the code only.

## 0. Summary for the task-level HLE

* **Task start.** `$1` = 0xfc0 (the OSTask header in DMEM) is an input of the microcode. A fresh start (`flags & 1 == 0`) patches four DMA-table words with `ucode`, sets
  `fifo_start/fifo_end` (0x150/0x154), the RDP write pointer (0x128), `yield_data_ptr` (0x108), the matrix-stack words (0x130/0x134/0x15c), fetches the first 320 bytes of
  the display list and enters the loop without a yield test. A resume (`flags & 1`) touches none of that: it reloads the code head and the four registers `$23,$28,$27,$26`
  from DMEM 0xbf0/0xbe4/0xbe8/0xbec of the restored yield image and enters the loop at the yield test (section 1).
* **Loop.** A 320-byte window of the display list is kept at DMEM 0x9a0..0xadf (`$27` read pointer, `$28` window end = 0xae0, `$26` DRAM address of the byte after the
  window). Every class-0 opcode (0x00..0x3f, including the no-ops) first DMAs `((w0 & 0x3ff) | 7) + 1` bytes from `xlate(w1)` into 0xae0. `xlate(a) = segtab[(a>>24)&15] +
  (a & 0xffffff)` (32-bit add; segment table at 0x160). The yield request (SP_STATUS bit 7) is tested after every command that ends at pc 0x09c (section 3).
* **G_DL** pushes `next-command DRAM address` on a 18-entry return stack at DMEM 0x398 (depth byte 0x148; 0x110 in 0.95), a full stack silently ignores the call;
  no-push is a plain jump. **G_ENDDL** pops, an empty stack ends the task. **G_BRANCH_Z** (1.2x only) jumps to the DL in the RDPHALF_1 word when `(int32)(vtx.z - w1) <= 0`.
  **G_CULLDL** is a conditional G_ENDDL: `(AND of the clip flags of the vertex range) & 0x7070 != 0` (NoN: 0x7030). G_LINE3D (0xb5) is a no-op in 1.2x (0.95: a quad).
* **End of task:** overlay 3 is loaded; SP_STATUS gets SET_SIG2 (task done), BREAK. **Yield:** the same plus a 0xc00-byte write of DMEM 0..0xbff to `yield_data_ptr` and
  SET_SIG1 (yielded) first. No RDP flush happens at either (every RDP command was already flushed when it was produced).

## 1. The OSTask header and the task start

### 1.1 The header at DMEM 0xfc0 (libultra `OSTask_t`, all fields 32-bit)

| DMEM | offset | field | used by the microcode |
|---|---|---|---|
| 0xfc0 | 0x00 | type | no |
| 0xfc4 | 0x04 | flags | **bit 0 (OS_TASK_YIELDED)** selects the resume path; other bits ignored |
| 0xfc8 | 0x08 | ucode_boot | no |
| 0xfcc | 0x0c | ucode_boot_size | no |
| 0xfd0 | 0x10 | ucode | DRAM address of the ucode text: **added to** the DMA-table offsets (fresh start); copied to DMEM 0xbfc by the yield (1.2x, not 0.95); rewritten by G_LOAD_UCODE |
| 0xfd4 | 0x14 | ucode_size | no |
| 0xfd8 | 0x18 | ucode_data | no (the boot code DMAed it to DMEM 0) |
| 0xfdc | 0x1c | ucode_data_size | no |
| 0xfe0 | 0x20 | dram_stack | fresh: copied to DMEM 0x130 and 0x134; POPMTX re-reads 0xfe0 itself as the stack base (`mtx.md`) |
| 0xfe4 | 0x24 | dram_stack_size | fresh: `dram_stack + dram_stack_size` -> DMEM 0x15c (end of the matrix stack) |
| 0xfe8 | 0x28 | output_buff | fresh: DMEM 0x150 (FIFO start); also the test of the RDP state (1.3) |
| 0xfec | 0x2c | output_buff_size | fresh: DMEM 0x154. **An end address, not a size**; also the initial DPC start (1.3) |
| 0xff0 | 0x30 | data_ptr | fresh: DRAM address of the display list; **used as is: no segment translation, no alignment, no use of data_size**. G_LOAD_UCODE rewrites it with the next-command address |
| 0xff4 | 0x34 | data_size | no (the loop ends at ENDDL, never at a length) |
| 0xff8 | 0x38 | yield_data_ptr | fresh: copied to DMEM 0x108; the yield DMAs DMEM 0..0xbff there |
| 0xffc | 0x3c | yield_data_size | no (the yield always writes 0xc00 = 3072 bytes) |

### 1.2 What the boot code (outside the microcode) must have done — observed to be sufficient in the driver

* IMEM 0x80.. <- ucode text (0xf80 bytes in the 1.x builds, 0xf30 in F3DEX 0.95, 0xf60 in F3DLX 0.95); DMEM 0..0x7ff <- ucode_data (the 0x800-byte data segment of `kit/data`);
  header at DMEM 0xfc0..0xfff; `$1 = 0xfc0`; pc = 0x80. No precondition on SP_STATUS (the first write below clears SIG1/SIG2).
* The first two words of the text are `j <init>` and `addi $29,$0,0x110`: **`$29 = 0x110`** is a constant base register for the whole run (set here, never written again).
  `<init>` is 0x738 (banjo), 0x758 (NoN), 0x730 (1.23), 0x758 (DLX 1.23), 0x7c0 (0.95), 0x7e8 (DLX 0.95): inside the overlay window, so the init code is overwritten by the
  first overlay that is loaded and never runs twice. The head DMA (1.3 step 8) overwrites IMEM 0x80..0x87 (the `j`/`addi`) too.
* Everything else (vector registers, other GPRs, DPC registers) is arbitrary. v30 and v31 are loaded by the microcode from DMEM 0x40 and 0x30 (LQV) in both paths.

### 1.3 Fresh start (observed on all six builds; init code read)

In order (banjo pcs 0x738..0x824; the init code is instruction-identical in the 1.x builds, same structure in 0.95):

1. SP_STATUS <- 0x2800 (CLR_SIG1 | CLR_SIG2: clears "yielded" and "task done"). SIG0 (yield request) is **not** touched.
2. v31 <- DMEM[0x30..0x3f], v30 <- DMEM[0x40..0x4f] (constants of the data segment: v30 = `7fff fff8 0008 0040 0020 8000 01cc cccc`, v31 = `0000 0001 0002 ffff 4000 0004 0633 0200` in banjo).
3. If `flags & 1` go to section 1.4. Otherwise:
4. `A = output_buff`, `B = output_buff_size`. DMEM[0x150] = A, DMEM[0x154] = B. `S = B` (the candidate start of the RDP list).
5. The RDP state decides `S`:
   * if DPC_STATUS bit 0 (XBUS_DMEM_DMA) is set: go to 6;
   * `END = DPC_END`; if `A > END` (signed 32-bit `A - END > 0`) go to 6;
   * `CUR = DPC_CURRENT`; if `CUR == 0` or `CUR == END` go to 6;
   * otherwise the RDP is still working on a list that ends at or above `A`: `S = END`, go to 7 (DPC registers untouched).
6. wait until DPC_STATUS bit 10 (START_VALID) is clear; DPC_STATUS <- 1 (clear XBUS_DMEM_DMA); DPC_START <- S; DPC_END <- S (so `START = CURRENT = END = S`).
7. DMEM[0x128] <- S (the RDRAM address of the next RDP chunk). `$23 <- 0xce0` (DMEM write pointer of the RDP output buffer).
8. `ucode = task.ucode`. DMEM[0x08], [0x10], [0x18], [0x20] += ucode (the DRAM offsets of head code, overlay 1, 2, 3; entry 0 (text) is not patched). DMA read of the table entry at 0x08
   (IMEM 0 <- ucode + head offset, 0x88 bytes; the table-load routine at 0x0ec waits for it).
9. `$26 <- task.data_ptr`; the refill (section 3.3): DMEM 0x9a0 <- 320 bytes at `$26`; `$28 = 0xae0`, `$27 = 0x9a0`, `$26 += 320` (not waited for here; the loop head waits).
10. DMEM[0x130] = DMEM[0x134] = task.dram_stack; DMEM[0x15c] = task.dram_stack + task.dram_stack_size; DMEM[0x108] = task.yield_data_ptr.
11. Jump to the loop head 0x058 (waits for the DMAs, then reads the first command). **No yield test before the first command.**

Consequences (observed): with `A = output_buff`, `B = output_buff_size` the RDRAM write pointer starts at `S`; in the normal case (`S = B`, the FIFO *end*) the first flush of the task
therefore always overflows the FIFO and restarts it at `A` (`rdp.md` section 0). The six cases of the RDP-state test (banjo; same on every build):

| DPC_STATUS bit0 | START / CURRENT / END before | result |
|---|---|---|
| 0 | -, 0, 0 (never started) | `S = B`; DPC_START = DPC_END = B; DPC_STATUS bit 0 cleared |
| 0 | END below A (e.g. 0x3000, A = 0x40000) | `S = B`, as above |
| 0 | END >= A, CURRENT == END | `S = B` |
| 0 | END >= A, CURRENT == 0 | `S = B` |
| 0 | END >= A, 0 != CURRENT != END (busy), e.g. END 0x41000 or 0x60200 | `S = END`; DPC untouched; DMEM[0x128] = END |
| 1 | any | `S = B` (the bit is cleared) |

Values left in DMEM by the init (diff against the data segment, all six builds, DMEM beyond 0x800 poisoned to check nothing else is touched):

| DMEM | value |
|---|---|
| 0x08, 0x10, 0x18, 0x20 | table offsets + ucode (e.g. banjo `ucode+0xf80`, `+0x1008`, `+0x1228`, `+0x13c0`; 0.95: `+0xf30 +0xfb8 +0x11d8 +0x1370`; DLX 0.95: `+0xf60 +0xfe8 +0x1208 +0x13a0`) |
| 0x108 | yield_data_ptr |
| 0x128 | S |
| 0x130, 0x134 | dram_stack |
| 0x150, 0x154 | output_buff, output_buff_size |
| 0x15c | dram_stack + dram_stack_size |
| 0x9a0..0xadf | first 320 bytes of the display list at data_ptr (a raw DMA: DRAM address & 0xffffff, rounded down to 8) |

Registers at the first arrival at the loop (informational): `$23 = 0xce0`, `$26 = data_ptr + 320`, `$27 = 0x9a8` (after the first command), `$28 = 0xae0`, `$29 = 0x110`.

### 1.4 Resume after a yield (observed on all six builds)

`flags & 1`: the DMEM image the OS loaded is the yield image (section 6.2): DMA table already patched, 0x108/0x128/0x130.. already set. After the status write and the v30/v31 loads of 1.3 (steps 1, 2) the microcode does only:
DMA read of the head code (table entry 0x08, absolute address); `$23 <- DMEM[0xbf0]`, `$28 <- [0xbe4]`, `$27 <- [0xbe8]`, `$26 <- [0xbec]`; then jumps to **0x09c** (the yield test), not to the loop head.
The DPC registers, FIFO pointers and everything else come from the restored DMEM; the header fields output_buff, output_buff_size, data_ptr, dram_stack*, yield_data_ptr are **ignored**.
Observed: a yield after commands M1, M2 and a resume afterwards produce exactly M3, M4, also when the yield came right after a G_DL call (return stack in the image); if SIG0
is still set when the resumed task starts, it yields again at once, before executing a command, and writes the same image.
The OS side (libultra, **not verified here**): `osSpTaskLoad` of a yielded task sets `flags |= OS_TASK_YIELDED`, makes `ucode_data = yield_data_ptr`, size 0xc00 (OS_YIELD_DATA_SIZE), and
for loadable tasks reads the ucode address from the last word of the yield image (hence DMEM 0xbfc), and clears the yield request.

## 2. DMEM state variables and tables

Sizes in bytes. "R" = read by, "W" = written by (banjo pcs; only the writers/readers relevant here are named; `x-ref` = documented in the named file).

### 2.1 0x000..0x0ff and the dispatch tables

| addr | size | meaning | notes |
|---|---|---|---|
| 0x00..0x27 | 5 x 8 | DMA table, entries `{u32 dram offset from ucode, u16 length-1, u16 imem address (bit 12 = IMEM)}` for: 0x00 text (0xf7f -> IMEM 0x80, used by the boot code only), 0x08 head code (0x87 -> IMEM 0), 0x10 overlay 1 (clipping), 0x18 overlay 2 (lighting), 0x20 overlay 3 (end / yield) | entries 0x08..0x20 get `+ucode` at the fresh start; overlay 1..3 all go to the same window (banjo 0x720; per build below) |
| 0x26 | 2 | = imem address of overlay 3 (`0x1720`): `jr` target of the yield (the low 12 bits of the table entry 0x20) | read at pc 0x0a8 |
| 0x30, 0x40 | 16, 16 | v31 / v30 constants (init loads them) | |
| 0xa0 | 2 | entry pc of the lighting overlay (0x172c) after its load | read by the stub at 0x734 |
| 0xb6 | 2 | **end entry** of overlay 3 (`0x1728`): where ENDDL-with-empty-stack continues | read at 0x0bc |
| 0xb8..0xbf (0xbc..0xc3 in 0.95) | 4 x 2 | **class table**, index `(w0 >> 29) & 6`: class 0 (op 0x00..0x3f) -> 0x390 (class-0 dispatcher), class 1 (0x40..0x7f) -> 0x09c, class 2 (0x80..0xbf) -> 0x1ac (class-2 dispatcher), class 3 (0xc0..0xff) -> 0x358 | 0.95 has the 24-bit mask 0x00ffffff at 0xb8 (used by its translation routine) |
| 0xba | 2 | = 0x109c, the loop head: other code uses it as the return pc constant (`lh $24,186($0)`) | read-only |
| 0x76..0xf5 | 64 x 2 | **class-2 opcode table**, index `(w0 >> 23) & 0xfe` (= 2*(op & 0x7f)) | overlaps the first cells with constants and the class table |
| 0xc0..0x13f (0xc4.. in 0.95) | 64 x 2 | **class-0 opcode table**, index `2*op` | overlaps the class-2 table for ops 0..0x1a (aliases) and state variables for ops 0x20..0x3f |
| 0x100 | 2 | entry pc of the clipping overlay (0x173c) after its load | read by the stub at 0x728 |
| 0x102 | 2 | continuation pc (0x1758) read by the clipping overlay (0x7cc) | constant |
| 0x104 | 2 | loop head pc after a refill (0x1058) | constant, loaded into `$31` at 0x0b8 |
| 0x106 | 2 | continuation pc of the vertex / matrix code (the VTX handler stores 0x09c; the code `jr`s to it at the end of the vertex loop) | `x-ref vtx`, `clip.md` |
| 0x108 | 4 | yield_data_ptr copy | W init (0x820); R yield (0x75c) |
| 0x10c | 4 | RDPHALF_1 word (the `w1` of the last G_RDPHALF_1) | W 0x34c; R G_RDPHALF_2 (0x354), **G_BRANCH_Z (0x308)**, LOAD_UCODE (0xfac); never cleared |
| 0x110 | 4 | 1.2x: 0x112 = G_PERSPNORM scale (halfword, data 0xffff). **0.95: byte 0x110 = the DL return-stack depth** | `x-ref state.md` |
| 0x114 | 4 | geometry mode | `x-ref state.md` |
| 0x118, 0x11c | 4, 4 | cached RDP SET_OTHER_MODES words | `x-ref state.md`, `rdp.md` |
| 0x120, 0x124 | 4, 4 | G_TEXTURE w0 / w1 | `x-ref state.md` |
| 0x128 | 4 | RDRAM address where the next flushed RDP chunk goes | W init (7b8), flush; R flush; F3DLX also R before every RDP command (DPC_END kick) |
| 0x12c | 4 | lights word (bit 31 = stale model-space light vectors, low bits `(n+1)*32`) | `x-ref state.md`, `mtx.md` |
| 0x130 | 4 | copy of dram_stack | W init; **no reader found in any overlay of banjo** |
| 0x134 | 4 | matrix stack pointer in RDRAM (starts at dram_stack, +64 per push) | W init; `x-ref mtx.md` |
| 0x138 | 8 | G_MV_TXTATT target | no reader found (`state.md`) |
| **0x148** | 1 | **DL return-stack depth** (signed byte, 4 per entry, 0..72); 1.2x. (0.95: 0x110) | W 0x2ec (pop), 0x704 (push); R 0x2d8, 0x6e4 |
| 0x150, 0x154 | 4, 4 | RDP FIFO start / end address (OSTask.output_buff / output_buff_size) | W init; R flush |
| 0x158 | 2 | scratch of the clipping overlay (low half of `$31` at its entry) | `clip.md` |
| 0x15c | 4 | end of the matrix stack in RDRAM | W init; R MTX (0x578) |
| 0x160..0x19f | 16 x 4 | **segment table**: segtab[n] = base of segment n | W G_MOVEWORD index 6 at `0x160 + offset` (the offset field is not range-checked: it can write any DMEM word); R every translation |
| 0x1a0..0x2af | | LookAt / lights | `x-ref state.md`, `light.md` |
| 0x2b0..0x2ef | 64 | banner `RSP Gfx ucode F3DEX  ... 1.21 Yoshitaka Yasumoto Nintendo.` | |
| 0x2f0..0x30d, 0x30e..0x31d | | G_MOVEMEM / G_MOVEWORD destination tables (halfword per index) | `x-ref state.md` |
| 0x31e..0x35d | 32 x 2 | vertex slot address table: `0x4a0 + 0x28*i` | R CULLDL, BRANCH_Z, MODIFYVTX, triangles (0x31c in 0.95) |
| 0x380 | 16 | viewport | `x-ref state.md` |
| 0x390 | 4 | fog | `x-ref state.md` |
| **0x398..0x3df** | 18 x 4 | **DL return stack** (entry `d/4` at `0x398 + d`, d = depth before the push) | W push; R pop. Entry 17 at 0x3dc is the last |
| 0x3e0 / 0x420 / 0x460 | 64 each | modelview / projection / MVP matrices | `x-ref mtx.md` |
| 0x4a0..0x99f | 32 x 0x28 | vertex buffer (+0x1c = screen Z, +0x24 = clip flags) | `x-ref vtx`, `clip.md` |
| **0x9a0..0xadf** | 320 | **display-list window** | W refill (and nothing else) |
| **0xae0..0xcdf** | up to 512 | **memory-operand buffer** (prefetch DMA target); also scratch of triangle code (0xbe0..) and of the yield (0xbe4..0xbfc) | R only by the class-0 handlers MTX, VTX, MOVEMEM (through `$22`) |
| 0xce0.. | up to 0xb0 | RDP output buffer | `x-ref rdp.md` |
| 0xfc0..0xfff | 64 | OSTask header | 1.1 |

DMEM bytes that the loop writes only as leftovers: 0xae0.. after every class-0 command (the prefetch of `((w0&0x3ff)|7)+1` bytes; nothing but MTX/VTX/MOVEMEM reads it, and each of
those fetches its own operand first); 0xbe4..0xbfc written by the yield only into the image it writes out (and the yield ends the task).

### 2.2 The overlay window per build

| build | text DMA | head DMA | window (ov1/2/3 loaded here) | ov3 yield entry (DMEM 0x26) | ov3 end entry (DMEM 0xb6) | overlay sizes (1/2/3) |
|---|---|---|---|---|---|---|
| banjo121 | 0xf80 B | `+0xf80`, 0x88 B | 0x720 | 0x720 | 0x728 | 0x220 / 0x198 / 0x70 |
| sf64_non122 | 0xf80 | `+0xf80` | 0x740 | 0x740 | 0x748 | same |
| ts2_f3dex123 | 0xf80 | `+0xf80` | 0x718 | 0x718 | 0x720 | same |
| ts2_f3dlx123 | 0xf80 | `+0xf80` | 0x740 | 0x740 | 0x748 | same |
| mk64_f3dex95 | 0xf30 | `+0xf30` | 0x7a8 | 0x7a8 | 0x7b0 | 0x220 / 0x198 / 0x68 |
| mk64_f3dlx95 | 0xf60 | `+0xf60` | 0x7d0 | 0x7d0 | 0x7d8 | same |

## 3. The command loop

### 3.1 Registers

`$25` = w0, `$24` = w1 of the current command, `$27` = DMEM address after it (command at `$27-8`), `$28` = end of the valid window (0xae0; equal to `$27` right after a jump of the
DL pointer), `$26` = DRAM address of the byte after the window, `$22` = 0xae0 (class 0), `$1` = class*2 (loop) / DL push flag (class-0 dispatcher), `$29` = 0x110, `$23` = RDP output pointer, `$30`,
`$21`, `$19`, `$20`, `$18`, `$17`, `$11` = DMA helper arguments / scratch. The loop's own state that survives a command is `$23, $26, $27, $28, $29`.

### 3.2 The SP DMA helper and the wait routine (IMEM 0xfc4..0xffc; 0.95: 0x12c..0x164)

* `wait` (0xfc4, 0.95: 0x12c): spin while SP_DMA_BUSY (cp0 6) != 0; returns with `$17 = 0` (the delay slot of its `jr` is the first instruction of the read entry below).
* `dma_read` (0xfd4): `$17 = 0`, falls into `dma` (0xfd8). `dma_write` = `$17 = 1; jal dma`.
* `dma` (0xfd8): spin while SP_DMA_FULL (cp0 5); SP_MEM_ADDR <- `$20`; SP_DRAM_ADDR <- `$19`; then SP_RD_LEN <- `$18` if `$17 <= 0`, else SP_WR_LEN <- `$18`; returns. The DMA is **not** waited for.
* Hardware / JIT semantics (`ext/parallel-rsp/rsp/cp0.cpp`): MEM_ADDR `& 0x1fff` (bit 12 selects IMEM; low 2 bits dropped), DRAM_ADDR `& 0xffffff` (low 3 bits dropped), bytes per row
  `((len & 0xfff) | 7) + 1`, rows `((len >> 12) & 0xff) + 1`, skip `len >> 20`; the row is clipped at the end of DMEM/IMEM; RDRAM access `& 0x7ffffc`.
  So `RD_LEN = n - 1` moves `n` bytes rounded up to 8.

### 3.3 Loop head, refill, termination (observed; verified: `refilldec`, `reg_refill`, `reg_head`)

```
0x058  wait();  p = 0x9a0                                   // $27
0x060  w0 = DMEM32[p]; w1 = DMEM32[p+4]; p += 8;  cls = (w0 >> 29) & 6;  len = w0 & 0x3ff
       if cls == 0: prefetch(): $22 = 0xae0; a = xlate(w1); DMA_read(mem 0xae0, dram a, lenreg = len)      // 0x07c..0x08c
0x090  jump DMEM16[0xb8 + cls] & 0xffc       // class table: 0x390, 0x09c, 0x1ac, 0x358
0x09c  if (SP_STATUS & 0x80) goto yield (section 6)         // the yield test, see below which commands skip it
0x0ac  if p != $28 { w0 = DMEM32[p]; goto 0x064 }           // next command; delay slot loads w0 in both cases
       // window exhausted:
0x0c8  $28 = 0xae0;  DMA_read(mem 0x9a0, dram $26, lenreg 319);  $26 += 320;  p = 0x9a0;  goto 0x058 (via DMEM16[0x104])
```

* The window is **320 bytes = 40 commands**; the DMA address is `$26` as is (a 32-bit register; the DMA drops bits above 23 and the low 3). The pointer `$26` advances by 320 per refill.
* The loop **ends only through G_ENDDL with an empty return stack** (0x2e0 -> 0x0bc), a G_CULLDL that culls with an empty stack, or G_LOAD_UCODE. Neither `data_size` nor the window
  content bounds it; a display list without ENDDL runs on through RDRAM.
* The refill is entered from 0x0ac only; the DL jump commands make it happen by setting `$28 = $27` (so `p == $28` at the next 0x0ac).
* **Verified** (`refilldec`): the decision `$28 != $27` -> 0x064 (with `$25 = DMEM32[$27]`), else `$31 = DMEM16[0x104]` and 0x0c8. (`reg_refill`: the register effects of 0x0c8..0x0dc;
  `reg_head`: `$25, $24, $1, $18, $22, $27, $31, $19` after the head, both for class 0 (exit 0x088, before the prefetch DMA) and for the other classes (exit 0x094, before the dispatch `jr`).)
* Which commands reach the yield test at **0x09c** and which go straight to **0x0ac**: observed (`rdpyield` on all builds) — after every handler that ends with `j 0x09c`; RDPHALF_1 (op b4; 0.95:
  b3) and the RDP pass-through opcodes **0xc0..0xe5** end at 0x0ac (no yield test); the pass-through opcodes **0xe6..0xff** end at 0x09c (the choice is the sign of `op + 27` computed in the delay slot of the
  pass-through; verified in `rdpcopy`, register `$2`). The first command after the init is also executed without a test.

### 3.4 Segment translation (verified: `reg_xlate`, `rdpcopy`, `call`; subroutine at 0x10c, 0x104 in 1.23)

```
xlate(a) = DMEM32[0x160 + 4*((a >> 24) & 15)] + (a & 0xffffff)         // 32-bit wrap-around; bits 31..28 of a ignored
```
Scratch: `$12` = the segment word (and, in 0.95, `$11` = DMEM32[0xb8] = 0x00ffffff, `$13` = index*4; there `a & DMEM32[0xb8]`). The result is **not** masked: a base above 16 MB or a sum that carries into bit 24
reaches the DMA, which drops bits 24..31 and bits 0..2. A 0x80xxxxxx (K0) address works because segment 0 (table word 0x160, initially 0) adds nothing.
Segment table writes: G_MOVEWORD index 6, `segtab[offset/4] = w1` (stored raw; `state.md`).

### 3.5 The prefetch DMA of class 0 (observed on all builds; the formula is the code)

Before the handler of any opcode 0x00..0x3f runs: DMA read of `((w0 & 0x3ff) | 7) + 1` bytes (`RD_LEN = w0 & 0x3ff`; an 8-byte minimum, always a multiple of 8) from
`xlate(w1)` to DMEM 0xae0, completed (wait at 0x398) before the handler starts. Also for no-ops, G_SPNOOP and for G_DL (8 bytes from the callee's address, `len` = 0). libultra puts the byte count there
(MTX: 0x40, so 72 bytes are moved; VTX: n*16-1). Nothing is read from RDRAM beyond what the DMA moves. The classes 1, 2, 3 do no prefetch.

### 3.6 Dispatch (tables read from the data segments; the class-0 dispatcher at 0x390 additionally sets `$1 = w0 bits 23..16`, `$6 = $1 & 15` and waits for the DMA)

* class 0: `pc = DMEM16[0xc0 + 2*op] & 0xffc` (0.95: base 0xc4)
* class 1: `pc = 0x09c` (nothing happens, **no prefetch**)
* class 2: `pc = DMEM16[0x76 + 2*(op & 0x7f)] & 0xffc` (the class-2 dispatcher at 0x1ac loads `$1` with the last byte of w0)
* class 3: `pc = 0x358` (RDP pass-through)

The class-0 table (64 halfwords from 0xc0) and the class-2 table (64 halfwords from 0x76) overlap. A cell that is not a handler pointer holds a constant or a state variable; jumping through it
goes to arbitrary code (an IMEM address made of the halfword's low 12 bits). In the grids below a cell shown as `=xxxx` is such a **non-pointer cell** with its raw value (`undefined behaviour`; observed
op 0x20: the JIT runs into code that leaves the run; op 0x80: execution goes through the vector code at pc 0 and re-enters the loop at 0x060 without passing 0x09c, consistent with `$31` still being the link of the loop head's wait call).
Cells with a pc are handlers. Games only issue the real opcodes; the aliases (class-0 ops 0x0a.. = class-2 handlers 0xaf.., class-2 ops 0xa5.. = class-0 handlers) are accidents of the table overlap.

All 256 opcodes, per build (row = high nibble, column = low nibble; `=xxxx` = non-pointer cell):

```
banjo121   (class table 0xb8, class-0 table 0xc0, class-2 table 0x76)
       +0    +1    +2    +3    +4    +5    +6    +7    +8    +9    +a    +b    +c    +d    +e    +f 
0x00  09c   554   09c   6d0   3a8   09c   6e0   09c   09c   09c   f8c   2f8   1e4   334   350   348 
0x10  09c   320   310   2d8   27c   274   250   238   204   2b8   1bc  =1774 =1778 =17d0 =17e4 =1920
0x20 =173c =1758 =1058 =0000 =0000 =0000 =0000 =0000 =0000 =ffff =0000 =0000 =ef08 =0cff =0000 =0000
0x30 =0000 =0000 =0000 =0000 =0000 =0000 =8000 =0040 =0000 =0000 =0000 =0000 =4000 =4000 =0000 =0000
0x40  09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c 
0x50  09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c 
0x60  09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c 
0x70  09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c 
0x80 =0001 =0000 =0001 =0000 =0001 =0001 =0000 =0000 =ffff =0000 =0001 =0000 =ffff =0000 =0000 =0001
0x90 =ffff =0000 =0000 =0001 =0001 =172c =7fff =571d =3a0c =0001 =0002 =0100 =0200 =4000 =0040 =0000
0xa0 =1728  390   09c   1ac   358   09c   554   09c   6d0   3a8   09c   6e0   09c   09c   09c   f8c 
0xb0  2f8   1e4   334   350   348   09c   320   310   2d8   27c   274   250   238   204   2b8   1bc 
0xc0  358   358   358   358   358   358   358   358   358   358   358   358   358   358   358   358 
0xd0  358   358   358   358   358   358   358   358   358   358   358   358   358   358   358   358 
0xe0  358   358   358   358   358   358   358   358   358   358   358   358   358   358   358   358 
0xf0  358   358   358   358   358   358   358   358   358   358   358   358   358   358   358   358 
```

```
sf64_non122   (class table 0xb8, class-0 table 0xc0, class-2 table 0x76)
       +0    +1    +2    +3    +4    +5    +6    +7    +8    +9    +a    +b    +c    +d    +e    +f 
0x00  09c   574   09c   6f0   3a8   09c   700   09c   09c   09c   f8c   2f8   1e4   334   350   348 
0x10  09c   320   310   2d8   27c   274   250   238   204   2b8   1bc  =1794 =1798 =17f0 =1804 =1940
0x20 =175c =1778 =1058 =0000 =0000 =0000 =0000 =0000 =0000 =ffff =0000 =0000 =ef08 =0cff =0000 =0000
0x30 =0000 =0000 =0000 =0000 =0000 =0000 =8000 =0040 =0000 =0000 =0000 =0000 =4000 =4000 =0000 =0000
0x40  09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c 
0x50  09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c 
0x60  09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c 
0x70  09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c 
0x80 =0001 =0000 =0001 =0000 =0001 =0001 =0000 =0000 =ffff =0000 =0001 =0000 =ffff =0000 =0000 =0001
0x90 =ffff =0000 =0000 =0000 =0001 =174c =7fff =571d =3a0c =0001 =0002 =0100 =0200 =4000 =0040 =0000
0xa0 =1748  390   09c   1ac   358   09c   574   09c   6f0   3a8   09c   700   09c   09c   09c   f8c 
0xb0  2f8   1e4   334   350   348   09c   320   310   2d8   27c   274   250   238   204   2b8   1bc 
0xc0  358   358   358   358   358   358   358   358   358   358   358   358   358   358   358   358 
0xd0  358   358   358   358   358   358   358   358   358   358   358   358   358   358   358   358 
0xe0  358   358   358   358   358   358   358   358   358   358   358   358   358   358   358   358 
0xf0  358   358   358   358   358   358   358   358   358   358   358   358   358   358   358   358 
```

```
ts2_f3dex123   (class table 0xb8, class-0 table 0xc0, class-2 table 0x76)
       +0    +1    +2    +3    +4    +5    +6    +7    +8    +9    +a    +b    +c    +d    +e    +f 
0x00  09c   54c   09c   6c8   3a0   09c   6d8   09c   09c   09c   f8c   2f0   1dc   32c   348   340 
0x10  09c   318   308   2d0   274   26c   248   230   1fc   2b0   1b4  =176c =1770 =17c8 =17dc =1918
0x20 =1734 =1750 =1058 =0000 =0000 =0000 =0000 =0000 =0000 =ffff =0000 =0000 =ef08 =0cff =0000 =0000
0x30 =0000 =0000 =0000 =0000 =0000 =0000 =8000 =0040 =0000 =0000 =0000 =0000 =4000 =4000 =0000 =0000
0x40  09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c 
0x50  09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c 
0x60  09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c 
0x70  09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c 
0x80 =0001 =0000 =0001 =0000 =0001 =0001 =0000 =0000 =ffff =0000 =0001 =0000 =ffff =0000 =0000 =0001
0x90 =ffff =0000 =0000 =0001 =0001 =1724 =7fff =571d =3a0c =0001 =0002 =0100 =0200 =4000 =0040 =0000
0xa0 =1720  388   09c   1a4   350   09c   54c   09c   6c8   3a0   09c   6d8   09c   09c   09c   f8c 
0xb0  2f0   1dc   32c   348   340   09c   318   308   2d0   274   26c   248   230   1fc   2b0   1b4 
0xc0  350   350   350   350   350   350   350   350   350   350   350   350   350   350   350   350 
0xd0  350   350   350   350   350   350   350   350   350   350   350   350   350   350   350   350 
0xe0  350   350   350   350   350   350   350   350   350   350   350   350   350   350   350   350 
0xf0  350   350   350   350   350   350   350   350   350   350   350   350   350   350   350   350 
```

```
ts2_f3dlx123   (class table 0xb8, class-0 table 0xc0, class-2 table 0x76)
       +0    +1    +2    +3    +4    +5    +6    +7    +8    +9    +a    +b    +c    +d    +e    +f 
0x00  09c   578   09c   6f4   3b0   09c   704   09c   09c   09c   f80   2e8   1d4   324   340   338 
0x10  09c   310   300   2c8   26c   264   240   228   1f4   2a8   1ac  =1794 =1798 =17f0 =1804 =1940
0x20 =175c =1778 =1058 =0000 =0000 =0000 =0000 =0000 =0000 =ffff =0080 =0000 =ef08 =0cff =0000 =0000
0x30 =0000 =0000 =0000 =0000 =0000 =0000 =8000 =0040 =0000 =0000 =0000 =0000 =4000 =4000 =0000 =0000
0x40  09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c 
0x50  09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c 
0x60  09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c 
0x70  09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c 
0x80 =0001 =0000 =0001 =0000 =0001 =0001 =0000 =0000 =ffff =0000 =0001 =0000 =ffff =0000 =0000 =0001
0x90 =ffff =0000 =0000 =0001 =0001 =174c =7fff =571d =3a0c =0001 =0002 =0100 =0200 =4000 =0040 =0000
0xa0 =1748  398   09c   19c   348   09c   578   09c   6f4   3b0   09c   704   09c   09c   09c   f80 
0xb0  2e8   1d4   324   340   338   09c   310   300   2c8   26c   264   240   228   1f4   2a8   1ac 
0xc0  348   348   348   348   348   348   348   348   348   348   348   348   348   348   348   348 
0xd0  348   348   348   348   348   348   348   348   348   348   348   348   348   348   348   348 
0xe0  348   348   348   348   348   348   348   348   348   348   348   348   348   348   348   348 
0xf0  348   348   348   348   348   348   348   348   348   348   348   348   348   348   348   348 
```

```
mk64_f3dex95   (class table 0xbc, class-0 table 0xc4, class-2 table 0x76)
       +0    +1    +2    +3    +4    +5    +6    +7    +8    +9    +a    +b    +c    +d    +e    +f 
0x00  09c   5dc   09c   758   430   09c   768   09c   09c   09c   224   3d8   3d0   3c8   244   3b4 
0x10  3a4   384   32c   324   300   2e8   2b4   368   1fc  =17fc =1800 =1858 =186c =19a8 =17c4 =17e0
0x20 =1058 =0000 =0000 =0000 =0000 =0000 =0000 =ffff =0000 =0000 =ef08 =0cff =0000 =0000 =0000 =0000
0x30 =0000 =0000 =0000 =0000 =8000 =0040 =0000 =0000 =0000 =0000 =4000 =4000 =0000 =0000 =0000 =0000
0x40  09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c 
0x50  09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c 
0x60  09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c 
0x70  09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c 
0x80 =0001 =0000 =0001 =0000 =0001 =0001 =0000 =0000 =ffff =0000 =0001 =0000 =ffff =0000 =0000 =0001
0x90 =ffff =0000 =0000 =0001 =0001 =17b4 =7fff =571d =3a0c =0001 =0002 =0100 =0200 =4000 =0040 =0000
0xa0 =17b0 =00ff =ffff  418   09c   1ec   3e0   09c   5dc   09c   758   430   09c   768   09c   09c 
0xb0  09c   224   3d8   3d0   3c8   244   3b4   3a4   384   32c   324   300   2e8   2b4   368   1fc 
0xc0  3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0 
0xd0  3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0 
0xe0  3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0 
0xf0  3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0   3e0 
```

```
mk64_f3dlx95   (class table 0xbc, class-0 table 0xc4, class-2 table 0x76)
       +0    +1    +2    +3    +4    +5    +6    +7    +8    +9    +a    +b    +c    +d    +e    +f 
0x00  09c   608   09c   784   440   09c   794   09c   09c   09c   21c   3d0   3c8   3c0   23c   3ac 
0x10  39c   37c   324   31c   2f8   2e0   2ac   360   1f4  =1824 =1828 =1880 =1894 =19d0 =17ec =1808
0x20 =1058 =0000 =0000 =0000 =0000 =0000 =0000 =ffff =0080 =0000 =ef08 =0cff =0000 =0000 =0000 =0000
0x30 =0000 =0000 =0000 =0000 =8000 =0040 =0000 =0000 =0000 =0000 =4000 =4000 =0000 =0000 =0000 =0000
0x40  09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c 
0x50  09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c 
0x60  09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c 
0x70  09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c   09c 
0x80 =0001 =0000 =0001 =0000 =0001 =0001 =0000 =0000 =ffff =0000 =0001 =0000 =ffff =0000 =0000 =0001
0x90 =ffff =0000 =0000 =0001 =0001 =17dc =7fff =571d =3a0c =0001 =0002 =0100 =0200 =4000 =0040 =0000
0xa0 =17d8 =00ff =ffff  428   09c   1e4   3d8   09c   608   09c   784   440   09c   794   09c   09c 
0xb0  09c   21c   3d0   3c8   3c0   23c   3ac   39c   37c   324   31c   2f8   2e0   2ac   360   1f4 
0xc0  3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8 
0xd0  3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8 
0xe0  3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8 
0xf0  3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8   3d8 
```


Handlers and the opcodes that reach them (first the "real" opcode, second the alias):

| handler (opcodes) | 1.21 banjo121 | NoN 1.22 sf64_non122 | 1.23 ts2_f3dex123 | DLX 1.23 ts2_f3dlx123 | 0.95 mk64_f3dex95 | DLX 0.95 mk64_f3dlx95 |
|---|---|---|---|---|---|---|
| MTX | 554 (01 a6) | 574 (01 a6) | 54c (01 a6) | 578 (01 a6) | 5dc (01 a8) | 608 (01 a8) |
| MOVEMEM | 6d0 (03 a8) | 6f0 (03 a8) | 6c8 (03 a8) | 6f4 (03 a8) | 758 (03 aa) | 784 (03 aa) |
| VTX | 3a8 (04 a9) | 3a8 (04 a9) | 3a0 (04 a9) | 3b0 (04 a9) | 430 (04 ab) | 440 (04 ab) |
| DL | 6e0 (06 ab) | 700 (06 ab) | 6d8 (06 ab) | 704 (06 ab) | 768 (06 ad) | 794 (06 ad) |
| LOAD_UCODE | f8c (0a af) | f8c (0a af) | f8c (0a af) | f80 (0a af) | - | - |
| BRANCH_Z | 2f8 (0b b0) | 2f8 (0b b0) | 2f0 (0b b0) | 2e8 (0b b0) | - | - |
| TRI2 | 1e4 (0c b1) | 1e4 (0c b1) | 1dc (0c b1) | 1d4 (0c b1) | 224 (0a b1) | 21c (0a b1) |
| MODIFYVTX | 334 (0d b2) | 334 (0d b2) | 32c (0d b2) | 324 (0d b2) | - | - |
| RDPHALF_2 | 350 (0e b3) | 350 (0e b3) | 348 (0e b3) | 340 (0e b3) | 3d8 (0b b2) | 3d0 (0b b2) |
| RDPHALF_1 | 348 (0f b4) | 348 (0f b4) | 340 (0f b4) | 338 (0f b4) | 3d0 (0c b3) | 3c8 (0c b3) |
| CLEARGEOM | 320 (11 b6) | 320 (11 b6) | 318 (11 b6) | 310 (11 b6) | 3b4 (0f b6) | 3ac (0f b6) |
| SETGEOM | 310 (12 b7) | 310 (12 b7) | 308 (12 b7) | 300 (12 b7) | 3a4 (10 b7) | 39c (10 b7) |
| ENDDL | 2d8 (13 b8) | 2d8 (13 b8) | 2d0 (13 b8) | 2c8 (13 b8) | 384 (11 b8) | 37c (11 b8) |
| SETOTHERMODE_L | 27c (14 b9) | 27c (14 b9) | 274 (14 b9) | 26c (14 b9) | 32c (12 b9) | 324 (12 b9) |
| SETOTHERMODE_H | 274 (15 ba) | 274 (15 ba) | 26c (15 ba) | 264 (15 ba) | 324 (13 ba) | 31c (13 ba) |
| TEXTURE | 250 (16 bb) | 250 (16 bb) | 248 (16 bb) | 240 (16 bb) | 300 (14 bb) | 2f8 (14 bb) |
| MOVEWORD | 238 (17 bc) | 238 (17 bc) | 230 (17 bc) | 228 (17 bc) | 2e8 (15 bc) | 2e0 (15 bc) |
| POPMTX | 204 (18 bd) | 204 (18 bd) | 1fc (18 bd) | 1f4 (18 bd) | 2b4 (16 bd) | 2ac (16 bd) |
| CULLDL | 2b8 (19 be) | 2b8 (19 be) | 2b0 (19 be) | 2a8 (19 be) | 368 (17 be) | 360 (17 be) |
| TRI1 | 1bc (1a bf) | 1bc (1a bf) | 1b4 (1a bf) | 1ac (1a bf) | 1fc (18 bf) | 1f4 (18 bf) |
| RDP | 358 | 358 | 350 | 348 | 3e0 | 3d8 |
| PERSPNORM | - | - | - | - | 3c8 (0d b4) | 3c0 (0d b4) |
| QUAD | - | - | - | - | 244 (0e b5) | 23c (0e b5) |
| redispatch0 | 390 | 390 | 388 | 398 | 418 | 428 |
| redispatch2 | 1ac | 1ac | 1a4 | 19c | 1ec | 1e4 |
| nop | 09c | 09c | 09c | 09c | 09c | 09c |

Role notes: class-2 opcodes of the 1.2x builds: af LOAD_UCODE, b0 BRANCH_Z, b1 TRI2, b2 MODIFYVTX, b3 RDPHALF_2, b4 RDPHALF_1, b5 (nothing), b6 CLEARGEOMETRYMODE, b7 SETGEOMETRYMODE, b8 ENDDL, b9
SETOTHERMODE_L, ba SETOTHERMODE_H, bb TEXTURE, bc MOVEWORD, bd POPMTX, be CULLDL, bf TRI1. 0.95: b0 nothing, b1 TRI2, b2 RDPHALF_2, b3 RDPHALF_1, b4 PERSPNORMALIZE (`halfword 0x112 <- w1`), b5 QUAD (four-vertex
primitive of two triangles, read only), b6..bf as above, **no** MODIFYVTX, BRANCH_Z, LOAD_UCODE. Class 0: 00 SPNOOP, 01 MTX, 03 MOVEMEM, 04 VTX, 06 DL; 02, 05, 07, 08, 09, 0x10 are no-ops in the 1.2x
(with the prefetch DMA). Banjo opcode 0xa3 jumps to the class-2 dispatcher again, 0xa1 to the class-0 dispatcher, 0xa4 to the pass-through (a raw copy of the command, observed).

## 4. G_DL, G_ENDDL

### 4.1 G_DL (class 0, op 0x06; banjo 0x6e0; verified `call` on all six builds)

Entry state: `$1` = w0 bits 23..16 (G_DL_PUSH = 0, G_DL_NOPUSH = 1; any nonzero value branches), `$24` = w1 (segmented address), `$26/$27/$28` as in 3.1 with `$28 = 0xae0` (the prefetch DMA of 8
bytes at the callee was done and is not used).

```
d = (int8) DMEM8[depth_addr]                   // 0x148 (0.95: 0x110); also loaded into $2 in the delay slot of the first test, in both cases
if ($1 == 0) {                                  // push
    if (d - 68 > 0) return 0x09c                // stack full: the whole command is ignored (no call, no branch)
    ret = $26 + $27 - $28                       // DRAM address of the next command (= $26 - (0xae0 - $27))
    DMEM32[0x398 + d] = ret;   DMEM8[depth_addr] = d + 4
}
$26 = xlate(w1);   $28 = $27                    // the next 0x0ac sees p == end and refills from $26
return 0x09c
```
* The return stack holds **18** entries (depth 0..68; the 19th nested call is dropped, **observed**: the callee body is skipped, the caller continues with the command after the DL).
* `depth_addr` is read as a *signed* byte: a stack word at `0x398 + d` with `d` in -128..-1 is written below the stack (not reachable in a sane run); no bound on `d < 0`.
* No-push is a jump: the stack is unchanged. The yield test follows (0x09c) before the callee's first command is fetched.
* The new window starts at `align8(xlate(w1) & 0xffffff)`: **observed** that a call to an address with low bits 4 executes the commands from the 8-byte-aligned address below it, while `$26` keeps the unaligned value (the
  return address stored by a later call is computed relative to it). DL addresses are 8-aligned in practice.
* DMEM bytes written: the stack word and the depth byte; scratch 0xae0..0xae7 (the prefetch). Registers: `$2, $3, $4` (push path), `$12, $19, $31` (translation), `$26`, `$28`.

### 4.2 G_ENDDL (class 2, op 0xb8; banjo 0x2d8; verified `enddl` on all six builds)

```
d = (int8) DMEM8[depth_addr] - 4
if (d < 0) goto end_of_task (0x0bc)             // empty stack: section 6.1 (no yield test)
$26 = DMEM32[0x398 + d];  DMEM8[depth_addr] = d;  $28 = $27;  goto 0x09c
```
The popped value is the DRAM address stored by G_DL. Like every DL jump it empties the window (`$28 = $27`); the refill at 0x0ac loads 320 bytes from the popped address. Registers: `$2 = d`, `$3 = d + 0x398`.

## 5. G_BRANCH_Z, G_CULLDL, G_SPNOOP, G_LINE3D, unknown opcodes

### 5.1 G_BRANCH_Z (1.21, NoN, 1.23, DLX 1.23; op 0xb0; banjo 0x2f8; verified `branchz` on those four; **absent in 0.95**: op 0xb0 is a no-op)

`w0 = 0xb0 << 24 | (vtx*5) << 12 | vtx*2` (libultra), `w1 = Z`. Only the low 12 bits of w0 matter (bits 12..23 are ignored):
```
va = DMEM16[0x31e + (w0 & 0xfff)]                // vertex record address: 0x4a0 + 0x28*(w0 & 0xfff)/2 for a vertex index < 32
vz = DMEM32[va + 0x1c]                           // the screen Z word of the vertex record: s16 integer part at +0x1c (clamped 0..0x7fff) , u16 fraction at +0x1e
d  = (int32)(vz - w1)                            // 32-bit wrap-around subtraction, then a signed test
if (d > 0) continue at 0x09c                     // z strictly greater than w1: nothing happens
else       $24 = DMEM32[0x10c];  $26 = xlate($24);  $28 = $27;  goto 0x09c       // branch (jump, no push) to the DL given by the preceding G_RDPHALF_1
```
Edge cases: vz == w1 branches; a wrapped difference (vz = 0x7fffffff, w1 = 0x80000000: d = -1) branches, (0x80000000 vs 0x7fffffff: d = +1) does not (observed); the RDPHALF_1 word is whatever the
last G_RDPHALF_1 left (initially 0, never cleared: the branch then goes to `xlate(0)`); no stack push; the vertex index is not range-checked (indices >= 32 read the following table words).
The vertex Z is the value the vertex command stored; a vertex that was never loaded has stale bytes. Registers at the exit: `$25 = d`, `$24 = DMEM[0x10c]`, `$2 = DMEM32[0x114]` (the delay slot of the jump).
The verified stretch ends at the jump target; the taken-branch tail is the same code as the no-push G_DL (translate `$24`, `$26 = result`, `$28 = $27`, `j 0x09c`) at pc 0x708 (NoN 0x728, 1.23 0x700, DLX 1.23 0x72c), covered by `call`.
End-to-end (observed, all four builds): RDPHALF_1(B), BRANCH_Z(v, w1), M_A, ENDDL with B = M_B, ENDDL: M_B is emitted only when vz <= w1, M_A only when vz > w1.

### 5.2 G_CULLDL (op 0xbe; verified `culldl` on all six builds)

1.2x (banjo 0x2b8; NoN 0x2b8; 1.23 0x2b0; DLX 0x2a8): `w0` low 12 bits = 2*first vertex index, `w1` low 12 bits = 2*last vertex index (libultra `gSPCullDisplayList(vstart, vend)`):
```
va = DMEM16[0x31e + (w0 & 0xfff)];  vb = DMEM16[0x31e + (w1 & 0xfff)]
acc = MASK;  p = va
loop:  acc &= DMEM16[p + 0x24];  if (p == vb) break;  p += 0x28            // INCLUSIVE of vb
if (acc == 0) continue at 0x09c                       // not culled ($2 = depth byte, see below)
else behave exactly as G_ENDDL (section 4.2): pop and continue, or end the task if the stack is empty
```
`MASK` = **0x7070** (1.21, 1.23, DLX 1.23, both 0.95 builds), **0x7030** for NoN (bit 6 is not tested). Bits 4,5,6 and 12,13,14 of the clip flags (halfword at vertex +0x24) are, as I read the vertex code (0x420..0x478, not
verified by me; `clip.md`/the vertex area define them), the x/y/z <= -w (low byte of VCC << 4) and x/y/z >= w (high byte) outcomes of the comparison against `w` itself: the display list is culled when **some** such bit is set in the
flags of **all** the vertices of the range (all outside the same plane). Hazard: if `vb` is not reached by stepping +0x28 from `va` (vb < va, or a table pointer of another build), the loop never ends and the RSP hangs; the games never issue this.
Edge: a range of one vertex culls when that vertex has any of the bits. A culled DL with a non-empty stack pops (observed: the callee returns to the caller); culled with an empty stack it jumps to 0x0bc, the end of the task (verified up to the jump). The
register `$2` after a non-culled exit is `lb 0x148` (the delay slot of the test is the first instruction of G_ENDDL).

**0.95** (mk64_f3dex95 0x368, mk64_f3dlx95 0x360): different encoding: `p = w0 & 0x3ff` is the byte offset of the first vertex in the vertex buffer (`40 * index`), `w1` is the byte offset **one past** the last
vertex (exclusive, 32-bit compared as is): the loop `v = lh(0x4c4 + p); p += 40; acc &= v` runs until `p == w1` (at least once; a `w1` that is not `p + 40k` hangs). Mask 0x7070. The rest as above.
Observed end-to-end on all six builds (flags 0x10/0x10/0x10 -> culled; no common bit, or only bit 0 -> not culled; bit 6 culls except in NoN).

### 5.3 G_SPNOOP (op 0x00), G_LINE3D (0xb5), reserved and unknown opcodes

* **G_SPNOOP** (class 0, handler 0x09c): does nothing, but is class 0: the prefetch DMA of `w1` (as an address) happens. Observed: no output, 2 loop visits for `NOOP, marker`.
* **G_LINE3D (0xb5)**: 1.21, NoN, 1.23, DLX 1.23: table cell 0xe0 = 0x109c = no-op, no prefetch (class 2 does none). Observed (banjo: 2 visits, no output). 0.95: not a line but the QUAD handler (read).
* Class 0 reserved/unused opcodes 0x02, 0x05, 0x07, 0x08, 0x09 (and 0x10): no-ops with the prefetch (observed on banjo). Class 1 (0x40..0x7f): silently skipped, no prefetch (observed).
* Class-0 opcodes 0x0a..0x1a are aliases of the class-2 handlers (e.g. 0x0a runs LOAD_UCODE with garbage and hangs/leaves; 0x0b = BRANCH_Z with the prefetch). Class-2 opcodes 0xa5..0xaf alias the class-0
  handlers *without* the prefetch (stale 0xae0) — e.g. 0xa6 = MTX, 0xab = DL.
* Class-2 opcodes 0x80..0xa4 and class-0 opcodes 0x1b..0x3f: non-pointer cells, undefined (see 3.6). Class 3: every opcode 0xc0..0xff is an RDP command, copied by the pass-through (op 0xa4 is copied too).
  RDP commands outside the real set are still copied and sent to the RDP.
* G_RDPHALF_1 (op b4; 0.95: b3; handler 0x348, verified `rdphalf1`): `DMEM32[0x10c] = w1`, continues at 0x0ac (no yield test). G_RDPHALF_2 (b3; 0.95: b2; 0x350; verified `rdphalf2`): appends the 8-byte
  RDP command `(DMEM32[0x10c], w1)` at `$23` (no translation) and calls the flush; continues at 0x09c.
* The RDP pass-through (`rdp.md`): `op = (int32)w0 >> 24; if (op + 3 >= 0) w1 = xlate(w1)` (0xfd..0xff: SETTIMG, SETZIMG, SETCIMG); `DMEM32[$23] = w0; DMEM32[$23+4] = w1`; flush; verified here as
  `rdpcopy` (DMEM words, `$2 = op + 27`, `$19/$12/$31` of the translation) on all six builds, including F3DLX's `DPC_END <- DMEM32[0x128]` write before the stores.

## 6. End of task and yield

### 6.1 End of task (G_ENDDL, empty stack; observed on all six builds, code read)

`0x2e0 bltz -> 0x0bc`: `$21 <- DMEM16[0xb6]` (overlay 3's end entry), then the overlay-load routine with table entry 0x20: DMA read of overlay 3 (112 B = 0x70, 0.95: 104 B) from `ucode + 0x13c0` to the window,
wait for the DMA, `jr $21`. Overlay 3 at the end entry: wait for the DMA to be idle; **SP_STATUS <- 0x4000 (SET_SIG2: TASKDONE)**; `break` (the RSP halts: SP_STATUS HALT | BROKE, interrupt if INTR_ON_BREAK
is set). Final SP_STATUS observed: 0x203 (+ whatever bits the OS had set, SIG0 is left alone). Nothing else: no RDP flush, no DPC_END write (the last one was written by the last flush), no DMA out.
The next pc is the one after the break; the OS reloads the ucode for the next task.

### 6.4 Where the RDP flush / DPC_END are involved (described in `rdp.md`)

The flush (0x128; 0x120 in 1.23) is called after each produced RDP chunk: from the pass-through tail (0x37c; RDPHALF_2 and SETOTHERMODE_L/H join it at 0x374) and from the triangle tail (0xf10). It is **not** called by G_ENDDL, the yield, or the
end sequence. F3DEX flushes then writes DPC_END in the flush itself (0x1a0). F3DLX writes DPC_END from DMEM 0x128 *before and after* the flush in the pass-through (DLX 1.23: 0x36c/0x388) in the triangle code (0xac0) and in LOAD_UCODE (0xf88).

### 6.2 Yield (observed on all six builds; verified register effects of the test only by the driver)

At 0x09c: `if (SP_STATUS & 0x80)` (SIG0, "yield request") -> `$21 <- DMEM16[0x26]` and the overlay-load routine with table entry 0x20 (the same overlay 3, now entered at its **yield entry**):

1. `$2 = 0x1000 (SET_SIG1)`; `$6 = DMEM32[0xfd0]` (task.ucode; not in 0.95).
2. DMEM32[0xbe4] = `$28`, [0xbe8] = `$27`, [0xbec] = `$26`, [0xbf0] = `$23` (= 0xce0 at a command boundary), [0xbfc] = `$6` (1.2x; 0.95 does not write 0xbfc).
3. DMA **write** of DMEM 0..0xbff (**3072 bytes**) to DRAM `yield_data_ptr` (= DMEM32[0x108], used raw: `& 0xffffff`, aligned down to 8); wait for it.
4. SP_STATUS <- 0x1000 (SET_SIG1: YIELDED) (in the delay slot of the jump to 0x0bc), then the end sequence of 6.1 (reload of overlay 3, SP_STATUS <- SET_SIG2, `break`).

Final SP_STATUS observed: 0x383 = HALT | BROKE | SIG0 (still set) | SIG1 | SIG2. The saved image contains the patched DMA table, the whole state of section 2, the window (0x9a0), and the return stack; the
RDP output buffer (0xce0..) is not saved (empty), nor is DMEM 0xc00..0xfff. The RDP FIFO needs no flush: observed that all earlier commands are already in RDRAM when the yield happens (the pass-through and every producer
flush at once), and the DPC registers are as the last flush left them.
Which commands yield: after a handler that ends at 0x09c (section 3.3); a *pending* request is not noticed by G_RDPHALF_1, the RDP opcodes 0xc0..0xe5, nor by a G_ENDDL that ends the task.
The yield bit is **not cleared** by the microcode (the OS does it); a resumed task with the bit still set yields again at once (observed).

### 6.3 G_LOAD_UCODE (0xaf; banjo 0xf8c; read only)

Mentioned because it also leaves the loop: `DMEM32[0xfd0] = w1` (new ucode address), `DMEM32[0xff0] = next-command DRAM address` (the same pointer arithmetic as G_DL's push), DMA of the new text
(IMEM 0x1080 <- w1, 0xf18 bytes) and of the new data (DMEM 0 <- DMEM32[0x10c], `w0 & 0xffff` + 1 bytes), `$1 = 0xfc0`, jump to IMEM 0x80 = the init of the new microcode, which finds the same header
with the updated `ucode` / `data_ptr` fields. F3DLX first writes DPC_END from DMEM 0x128 (0xf80). Not tested; the HLE should refuse a task that reaches it.

## 7. Differences between the builds (everything not listed here is `=` in the pcmap and was read)

| item | 1.21 | NoN 1.22 | 1.23 | DLX 1.23 | 0.95 | DLX 0.95 |
|---|---|---|---|---|---|---|
| init code / loop / G_DL / G_ENDDL | reference | identical | identical (pcs -8) | identical | same structure, different pcs | same |
| refill (0xc8..0xe8) | `$21`-return | same | **different layout**: no `$21`; the DMA helper is entered by `j` (so the helper returns to `$31` = the caller); the same result | same as 1.23 | `$21` | `$21` |
| DMA helper / wait | IMEM 0xfc4 / 0xfd4 / 0xfd8 | same | same | same | in the text: 0x12c / 0x13c / 0x140 | same |
| translation routine | 0x10c: `(a<<8)>>8` | same | 0x104 | 0x104 | 0x10c: `a & DMEM32[0xb8]` (a longer routine, same result) | same |
| DL return stack depth byte | 0x148 | 0x148 | 0x148 | 0x148 | **0x110** | **0x110** |
| class table / class-0 table | 0xb8 / 0xc0 | same | same | same | **0xbc / 0xc4** | same |
| G_BRANCH_Z (b0) | yes | yes | yes | yes | **no** (nop) | no |
| G_CULLDL | inclusive, 2*index, mask 0x7070 | mask **0x7030** | 0x7070 | 0x7070 | exclusive, 40*index, 0x7070 | same |
| G_LOAD_UCODE (af) | 0xf8c | 0xf8c | 0xf8c | 0xf80 (DPC_END kick first) | no | no |
| yield saves the `ucode` word at 0xbfc | yes | yes | yes | yes | **no** | no |
| RDP pass-through DPC_END | in the flush | in the flush | in the flush | **before and after** the flush | in the flush | before and after |
| G_LINE3D (b5) | nop | nop | nop | nop | quad (read) | quad |
| overlay window / ov3 entries | 2.2 | | | | | |

## 8. Side effects and what later code depends on

* DMEM that the display-list engine writes and later code *does* depend on: 0x9a0..0xadf (window), `$26/$27/$28` (registers; saved by the yield), the return stack 0x398.. with its depth byte, 0x10c (RDPHALF_1 word; read by
  BRANCH_Z / RDPHALF_2 / LOAD_UCODE), the segment table (written by MOVEWORD), 0x108/0x128/0x130/0x134/0x150/0x154/0x15c (init).
* Pure scratch of the engine: 0xae0.. after the handler has used it (prefetch), the stack words above the current depth (stale entries stay), the `$2/$3/$4/$12/$13/$19/$31` temporaries.
* The prefetch DMA reads RDRAM that the DL author may not have mapped (for G_DL: the first 8 bytes of the callee; for no-ops: `w1` taken as an address); a task-level HLE can skip it for opcodes whose handler does not use 0xae0 (everything except
  MTX, VTX, MOVEMEM) if it also reproduces 0xae0.. as the microcode would have left it *only where something reads it* (nothing does, see section 2.1).

## 9. Verification

### 9.1 Harness (`tools/native-f3dex/rt_test`, spec routines in `spec/dl_spec.c`, generators in `spec/dl_gen.inc`)

DMEM, exit pc, SP/DPC registers and **all GPRs** ($1..$31, via `sc.cmp_sr`) compared; 20000 random cases per routine and build, 0 differences everywhere (5000 on a first run):

| routine (`spec.f3dex.dl.`) | stretch (banjo) | builds |
|---|---|---|
| `call` | G_DL 0x6e0 -> 0x09c (push, no-push, full stack, signed depth bytes, segment table random, `$28 == $27` states) | all 6 |
| `enddl` | 0x2d8 -> {0x09c, 0x0bc} | all 6 |
| `branchz` | 0x2f8 -> {0x09c, taken label 0x708 / 0x728 / 0x700 / 0x72c} (Z edge values, overflowing differences, the bits 12..23 of w0 randomised) | 1.21, NoN, 1.23, DLX 1.23 |
| `culldl` | 0x2b8 -> {0x09c, 0x0bc} incl. the pop; flag patterns with and without common bits | all 6 (0.95: separate encoding) |
| `rdpcopy` | 0x358 -> 0x37c: pass-through with the SETTIMG family translation (op byte sweep, segment table random) | all 6 |
| `rdphalf1`, `rdphalf2` | 0x348 -> 0x0ac, 0x350 -> 0x37c | all 6 |
| `refilldec` | 0x0ac -> {0x064, 0x0c8} | all 6 |
| `reg_refill`, `reg_xlate`, `reg_head` | register-only stretches (0x0c8 -> 0x0dc; 0x10c -> `$31`; 0x058 -> {0x088, 0x094}) | refill: 1.21, NoN, 0.95, DLX 0.95; xlate: all 6; head: the four 1.x builds |

The stretches stop before every SP DMA (`jal 0xfd4` etc. cannot run in the harness: no RDRAM), so the DMA calls themselves, the init, the yield and the end of the task are **not** in the harness.
Mutation checks (each applied to a scratch copy of `dl_spec.c`, banjo, 3000 cases; the harness reports BAD): stack-full test `>` -> `>=` (52 BAD), BRANCH_Z `d > 0` -> `d >= 0` (510), translation condition
`t >= 0` -> `t > 0` (161), segment index mask 15 -> 7 (363), CULLDL mask 0x7070 -> 0x7071 (24), ENDDL depth write `d` -> `d+1` (1660), return address `+$27+$26-$28` -> `+$27+$26` (1617), `$28 = $27 + 8` (2612),
head `$18` mask (1482), refill `+= 312` (3000), CULLDL `$25` without the delay-slot increment (3000).

### 9.2 Whole-task runs on the JIT with a scratch driver (observed)

The driver loads the real text/data of each build, emulates RDRAM and the SP DMA (the semantics of `cp0.cpp`) by trapping the DMA helper, boots the microcode as in 1.2, and records every DMA, the SP_STATUS and DPC
registers and the DMEM. Run on all six builds: init (DMEM diff above), the six RDP-state cases, 70-command display list (refill addresses 0x100000, +0x140; prefetch DMAs), nested G_DL (3 and 20 levels; the
19th nested call is ignored), segment translation (segments 3, 4, K0 address, bits 31..28), BRANCH_Z (7 Z cases), CULLDL (6 flag cases), yield at loop visits 1, 3, 5 (and after a G_DL), resume with and without a
still-pending request, the set of RDP opcodes that reach the yield test, a number of reserved/alias opcodes, an unaligned DL address. All matched the statements above.
**Not tested anywhere:** G_LOAD_UCODE; the undefined-opcode cells beyond the few listed; XBUS-mode FIFOs beyond the init decision; behaviour of the DMA helper with the real timing; RDRAM larger than 8 MB; the libultra
conventions of 1.4 (flags handling, `osSpTaskYield`, SIG bits' mapping to the OS names).

## 10. Open questions

* The OS side of the yield (which SP_STATUS bits the OS clears, how it builds the resumed task header) is quoted from libultra from memory, not from a ROM; the microcode side is complete.
* DMEM 0x130 has no reader; 0x138.. likewise (state doc). Possibly dead data.
* The meaning of the 0x7070 clip-flag bits is defined by the vertex code (`clip.md`); this document only states which bits CULLDL tests.
* Undefined-opcode cells: a game that issues one (a corrupted display list) gets arbitrary behaviour; the HLE should abort the task-level path for such opcodes (everything in the grids that is `=xxxx`).
