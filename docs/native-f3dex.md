# The F3DEX 1.x / F3DLX native path

The task-level native path (docs/design.md, "Task-level native path") for the F3DEX 1.x family: F3DEX 1.21 / 1.23 / 0.95, F3DEX.NoN 1.00 / 1.22, F3DLX 1.21 / 1.23 / 0.95 and F3DLX.NoN 1.21 / 1.23
(twelve ucode ids, about twenty games). The microcode's semantics are in [ucode-f3dex.md](ucode-f3dex.md) and [ucode-f3dex/](ucode-f3dex/); this document is about the code that implements them and the evidence
for it.

| file | what |
|---|---|
| `src/hle/f3dex_fx.h` | the interface: plain structs in and out (matrices, input vertices, vertex records, triangle input, clipper output), no DMEM addresses or pcs |
| `src/hle/f3dex_{mtx,vtx,tri,clip}_fx.c`, `f3dex_{vtx,tri}_fx_neon.h` | the functions: a scalar reference (`f3dex_*_scalar`) and, for the hot ones, a NEON form (`f3dex_*`, the scalar form without NEON) |
| `src/hle/f3dex_dmem.h` | filling the structs from a DMEM / RDRAM image and writing the records back |
| `src/hle/f3dex.c` | the family: boot and init on A's RSP interpreter up to the command loop, then a C display-list interpreter over the functions, then the end of the task; the twelve builds as data |
| `tools/native-f3dex/` | the differential harness against the real microcode on the JIT (`rt_test`; the area routines in `spec/`, the adapters `spec/fx*_spec.c` run the struct functions through it) |
| `tools/f3dex/` | NEON-vs-scalar tests (`fxn_*.c`), micro-benchmarks, the desktop and device scripts |

## 1. The functions

| function | forms | notes |
|---|---|---|
| `f3dex_mtx_mul` | scalar, NEON | `out = a x b`, each term `floor(a*b/65536)`, 48-bit accumulator, integer half clamped to s16, fraction to 0 / 0xffff on overflow. G_MTX multiply and the MVP |
| `f3dex_relight` | scalar | the model-space light directions (transposed 3x3 of the modelview, normalised to ~127 with the reciprocal square root) |
| `f3dex_vtx_xform` | scalar, NEON | G_VTX for 1..32 vertices: clip coordinates, clip flags (reject masks per build; NoN's pair coupling), 1/w, screen position, texture scale, lighting (up to 16 light records), texgen (linear too), fog; F3DLX: rounded y, optional clipping |
| `f3dex_tri_setup`, `_noclip` | scalar, NEON | the decision (reject / clip / face cull / degenerate) and the RDP bytes (header, edges, shade, texture, z); the F3DLX variant (no sub-pixel correction, `mul_mid` slopes) |
| `f3dex_clip_tri` | scalar | Sutherland-Hodgman over the six planes, new vertices completed with the vertex tail, the fan of triangles |

Fixed-point traps the code and the tests carry (docs/ucode-f3dex.md section 1 and 5): `mul_mid` (the fraction read before the integer x integer term) against `mul_fx`; raw VSAR halves; saturating 16-bit
subtracts and 32-bit subtracts with only the integer half saturating; 32-bit min / max; the 48-bit wrap.

## 2. Verification of the functions

| gate | result |
|---|---|
| against the JIT running the real microcode (`rt_test`, scenario generators with edge cases; the whole DMEM, the exit pc and the GPRs the stretch defines compared; leftovers the microcode writes are declared dead per scenario) | 20000 cases x 2 seeds x 6 builds per function: 0 differences (`spec.f3dex.fxvtx`, `fxtri`, `fxmtx`, `fxclip`); the NEON forms too, under qemu-aarch64: 0 differences on the same builds |
| NEON against scalar, plain C, random and edge structs (`tools/f3dex/fxn_*.c`) | vertex 18M (+ 6M on the rg35xxsp toolchain, 1M clang), triangle 27M, matrix 3.1M products x 3 seeds, clipper 6.8M checks: 0 differences; the vertex reciprocal path checked for all 2^32 inputs |
| real captured calls: the front-end over the captured task starts of Banjo, Star Fox 64, Toy Story 2 (F3DLX), Mario Kart 64 and 7 further builds, replayed against the recorded JIT result (`armlet-native-replay`) | 1514 + 450 tasks equal on the desktop; 314 equal on rgdsplus (A55) and on rg35xxsp (A53) with the NEON forms |
| mutation checks (`-DF3DEX_FX_MUTATE=n`, `tools/f3dex/mutcheck.sh`, `build_fxn.sh`) | vertex 22 + 12 (scalar), triangle 20 + 11, matrix and relight 13, clipper 17: every one caught by the differential tests, except those that cannot change a result (listed in the sources: a tie rule that is damped by the reciprocal's 9-bit index, a clamp that is unreachable) |

## 3. The front-end

`f3dex_run` (`src/hle/f3dex.c`): declines a yielded task (OSTask flag bit 0) and a pending yield request, runs the boot code and the microcode's initialisation on the interpreter to the command loop head
(the record's `head` pc, 0x058 in all ten builds; about 30 us per task on an A55), takes the display-list pointer from the interpreter's registers and then executes the commands itself:

* class 0 (MTX, MOVEMEM, VTX, DL), class 2 (the RDPHALF pair, BRANCH_Z, TRI1 / TRI2, MODIFYVTX, geometry mode, SETOTHERMODE, TEXTURE, MOVEWORD, POPMTX, CULLDL, ENDDL; 0.95: PERSPNORM, G_QUAD) and the RDP
  pass-through, each as docs/ucode-f3dex/ describes it;
* the overlays are loaded (DMA) where the microcode would load them (lit vertices: lighting; clipped triangles: the clipper), the end overlay at the end of the task, so IMEM and the SP registers end as the JIT's;
* **the output side**: the microcode flushes every RDP command with its own DMA and DPC_END write. Only the result is observable, so consecutive chunks that are contiguous in RDRAM are staged in DMEM 0x9a0..0xd8f
  and sent with one DMA and one DPC_END write at the next sync (a FIFO wrap, a full stage, the end of the task, and after a texture rectangle's first half: the stream the verify compares is normalised list by list,
  until A parses across lists). The wrap decision is made per chunk exactly as the microcode does. F3DLX's lagging DPC_END (publishes before and after commands, once per triangle) is reproduced as the value the
  microcode would have written last;
* declines (the JIT then runs the whole task): G_LOAD_UCODE (SpaceStation Silicon Valley: about half of its tasks), undefined opcodes, light state outside what the functions model, F3DLX with `G_CLIPPING` off and
  G_CULLDL (the flags are junk there), a vertex or display-list address outside the supported range, more than 30000 commands.

Builds (ids as gfx_ucode.c logs them) and the games they were run with:

| ucode | games |
|---|---|
| F3DEX 1.21 `208a567e9aead5ea` | Banjo-Kazooie, Glover, Bomberman Hero |
| F3DEX.NoN 1.22 `fd5a31ccf6d40b62` | Star Fox 64, Quake, Wetrix |
| F3DEX 1.23 `047252e810e35d56`, `208a567e9aead5ea`, `047252e810e35d56` | Toy Story 2 (2D), Earthworm Jim 3D, Mystical Ninja, SpaceStation Silicon Valley; Quest 64; Pokemon Puzzle League |
| F3DLX 1.23 `7502cfb7098eeffb` | Toy Story 2 (3D), A Bug's Life |
| F3DEX 0.95 `34dad67335a54cda` | Mario Kart 64 |
| F3DLX 0.95 `5f33f7c2be924de0` | (never seen running) |
| F3DLX 1.21 `76c763bc18ad3ab3` | Wipeout 64 |
| F3DLX.NoN 1.21 `0a7cc6c93363c663`, 1.23 `cc09096cb6a48e7b` | Clay Fighter Sculptor's Cut; Iggy's Reckin' Balls, Penny Racers |
| F3DEX.NoN 1.00 `5063f108118bf62a` | Turok |

**A build is data** (since 2026-10-17; `struct f3dex1_build`, `src/hle/f3dex_bld.h`), one record per registered text, and no code of the front-end tests which build it runs:

| field | what | registered values |
|---|---|---|
| `fx.lay` (`struct f3dex_layout`, `f3dex_dmem.h`) | the DMEM layout: 29 addresses the code names (handler tables `jt0` / `jt2`, the state words, the tables of G_MOVEWORD / G_MOVEMEM / the vertex slots, the matrices, the vertex buffer, the window, operand and output buffers, ...) and 7 constants of the data segment's first 0xc0 bytes (screen clamps, texgen bias, sign vector, clip ratio, clip planes and their masks, texgen polynomial) | `f3dex_lay_std` (the 1.2x builds); Turok's F3DEX.NoN 1.00 has its handler tables 4 bytes higher (`jt0` 0xc4); the 0.95 builds also the vertex table at 0x31c and the display-list depth byte at 0x110 |
| `fx.reject_mask`, `fx.non`, `fx.dlx` | the trivial-reject mask; the NoN clip flags; the F3DLX variant (clip flags optional, rounded screen y, its triangle code and DPC_END protocol) | 0x7070 / 0x7030 |
| `pc[24]` | the start-up code's stop (`head`), the loop's return point that the no-op commands lead to (`nop`), and the handler of every command (0: the build has none) | read from each text's tables |
| `culldl_off` | G_CULLDL takes byte offsets into the vertex buffer and leaves the last vertex out (0.95), not vertex numbers | 1 for the 0.95 builds |
| `load_key` | the FNV-1a-64 of the text's first 0xf48 bytes: the record's key for the derivation and the probe | from the dumps (F3DLX 0.95: none) |

The commands are dispatched through the build's own handler tables in DMEM (class 0: `jt0 + 2 op`, ops 0x00 .. 0x09; class 2: `jt2 + 2 (op & 0x7f)`, ops 0xaf .. 0xbf; the other cells of the overlapping tables
are not handler pointers and decline), whose pcs are matched against the record once per task (`make_kinds`): the 0.95 numbering of RDPHALF / PERSPNORM / G_QUAD, Turok's missing G_BRANCH_Z and G_LOAD_UCODE
(no-ops there, as its microcode does; the front-end used to run a G_BRANCH_Z) and a build's missing handlers come out of the data. G_MOVEWORD's table ends where the vertex table begins (8 entries, 7 in 0.95).
Family constants that are not fields: the light records' structure (32 bytes from `light`), G_TEXTURE's tile byte and scales after `texture`, the 40-byte vertex records, the 320-byte window, the DMA table at
DMEM 0 (overlay k at 8 + 8k, the end overlay's IMEM address at 0x26), the OSTask header at 0xfc0, `$29` = 0x110. The dead ranges (`l_dead`) are the family's and are copied, not derived, for a derived build.
A derived record (`docs/ucode-derive.md`, "F3DEX 1.x records that run") becomes such a build in `dbuild.c`. G_LOAD_UCODE declines and is not followed (the front-end does not switch builds; the probe is not told of the loaded text).

G_QUAD (0.95) is two triangles split along the diagonal that is longer in y; the second is skipped when the first was face-culled (the
microcode's cull exit clears `$31`), which `F3DEX_TRI_CULL` against `F3DEX_TRI_DEGEN` encodes.

## 4. DMEM: what a later task reads, and the masks

* Byte-granular tracing of 1500 consecutive tasks of 11 of the ucodes (`tools/f3dex/dmem_rw.cpp`: the interpreter with a read-before-write footprint of every DMEM access, vector loads and DMAs included): the only
  bytes a task reads that it has not written are the OSTask header (0xfc4..0xffc) and, in every build, over-reads whose values never reach an output: the vertex loop's look-ahead past the last input vertex
  (0xb30..0xb78, 0xbd0..0xbe0, 0xc70..0xcd8) and the unused seventh lane of the texture scale inputs (0xc26, 0xc2e). The data segment (0..0x7ff) is loaded by the boot code at every task start. DMEM 0x800..0xfbf is
  **not** reloaded between tasks, but nothing reads it before the same task has written it.
* Poison probe (`ARMLET_DMEM_POISON`): 18 games, 900 frames each, with the task-level path running the tasks: poisoning all of DMEM [0, 0xfc0) after every graphics task leaves the RSP state stream, the six
  screenshots and the audio identical to the reference; poisoning the declared ranges (`mask`) also leaves the hash stream (DMEM masked by the same table) identical, as does the plain native run.
* The ranges `f3dex.c` declares dead (`l_dead`): the clipper's scratch (0xb4, 0x106, 0x158, 0xc40..0xc70, 0xde0..0xfc0), the triangle scratch (0xbe0..0xc40), the command window and operand buffer (0x9a0..0xce0), the
  RDP output buffer and the coefficient scratch beyond the command (0xce0..0xd90), and the fourth byte of the transformed light directions. They are the leftovers the microcode writes and the native code does not.
  The evidence above supports a much larger mask (the whole of [0, 0xfc0)); the narrower list is what the native code needs not to write.

## 5. Results

Task-level replay on the devices (`tools/f3dex/dev_replay.sh`, clocks pinned, `--bench 3` over every 8th captured task; ns per call, NEON / scalar; whole task = native front-end against the JIT):

| game (build) | device | vertex | drawn triangle | rejected | culled | clip (scalar) | matrix | whole task |
|---|---|---|---|---|---|---|---|---|
| Banjo (1.21) | A55 | 158 / 307 | 442 / 1559 | 63 / 73 | 76 / 99 | 2363 | 185 / 236 | 2.05x (1.66x with the scalar functions) |
| Banjo (1.21) | A53 | 158 / 322 | 423 / 1628 | 24 / 35 | 43 / 71 | 2702 | 195 / 244 | 2.05x (1.74x) |
| Star Fox 64 (NoN) | A55 | 186 / 584 | 466 / 1661 | - | 76 / 100 | - | 285 / 334 | 1.19x (1.10x) |
| Star Fox 64 (NoN) | A53 | 187 / 612 | 442 / 1767 | - | 42 / 69 | - | 221 / 366 | 0.93x |
| Mario Kart 64 (0.95) | A55 | 152 / 432 | 466 / 1637 | - | 75 / 96 | - | 242 / 297 | 2.22x (1.46x) |
| Mario Kart 64 (0.95) | A53 | 157 / 454 | 442 / 1749 | - | 41 / 71 | - | 231 / 298 | 2.47x (1.55x) |
| Toy Story 2 (F3DLX) | A55 | 838 / 713 (one task with 44 vertices) | 418 / 1924 | - | - | - | 275 / 315 | |

What the task time is made of (A55, Banjo, per task of 2355 commands): the interpreter init 26 us, the output side 555 us (the DMA of the staged chunks and the DPC_END writes through the framework: about 1 ns per byte
for the DMA on the desktop, and a fixed cost per write; Star Fox's tasks contain hundreds of texture rectangles, each of which ends a list), the functions about 350 us, the rest the command loop and the state in DMEM.
Where the output side dominates (Star Fox on the A53) the native path is no faster than the JIT.

## 6. Reproducing

```
scripts/build.sh native --tools                          # armlet-native-replay, the plugin (desktop)
tools/f3dex/replay_all.sh                                # the captured task starts (ARMLET_HLE_CAPTURE=<dir>, EVERY / MAX) against the recorded JIT result
tools/f3dex/desk.sh RUN...                               # equivalence runs on the desktop (tools/equiv/desk.sh with this tree's pieces)
tools/f3dex/poison_runs.sh TAG ROM                       # ref, native, mask poison, all poison, verify for one game, then compare.py
tools/f3dex/dev_replay.sh DEV WORKDIR eq|bench [STRIDE]  # replay on a device under its lock; bench: F3DEX_PROF=1 (summary: tools/f3dex/prof_sum.py)
tools/f3dex/build_fxn.sh fxn_vtx a64                     # NEON-vs-scalar tests (run under qemu-aarch64-static)
SPEC_ONLY=fxvtx tools/native-f3dex/build_rt_test.sh x86 <dir>; <dir>/rt_test --ucode <ucode>/<build> --routine spec.f3dex.fxvtx --cases 20000
```
Environment of the front-end: `F3DEX_EXACT=1` (the microcode's own DMA reads and one flush per command, DMEM as the microcode leaves it), `F3DEX_TRACE=1`, `F3DEX_PROF=1`, `F3DEX_SCALAR=1` (the scalar functions).
