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
* G_LOAD_UCODE: a re-initialisation, or the text of another F3DEX 1.x build, is followed (section 3, "G_LOAD_UCODE");
* declines (the JIT then runs the whole task): G_LOAD_UCODE of a text that is no F3DEX 1.x build (SpaceStation Silicon Valley and Pokemon Puzzle League load S2DEX 1.x), undefined opcodes, light state outside what the functions model, F3DLX with `G_CLIPPING` off and
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
A derived record (`docs/ucode-derive.md`, "F3DEX 1.x records that run") becomes such a build in `dbuild.c`. G_LOAD_UCODE is followed to another build of this front-end (below); a latched derived build is found by its load key.

G_QUAD (0.95) is two triangles split along the diagonal that is longer in y; the second is skipped when the first was face-culled (the
microcode's cull exit clears `$31`), which `F3DEX_TRI_CULL` against `F3DEX_TRI_DEGEN` encodes.

### G_LOAD_UCODE (2026-10-17, branch `f3dex1-load`; the model is the F3DEX2 front-end's, design.md "Build switching")

**Operands** (read from the handler, `pc[loaduc]`, 0xf8c in the F3DEX 1.2x builds, 0xf80 in F3DLX; the handler lives in the part of the text that stays in IMEM; checked against SpaceStation Silicon Valley's
`af0007ff 8014e320` after `b4000000 8015c770`): `w1` is the text's RDRAM address (the games pass a KSEG0 address: the DMA drops the top bits), `G_RDPHALF_1`'s word (DMEM `half1`, 0x10c) is the data's, `w0 & 0xffff` the
DMA length register of the data (0x7ff: the whole 2048-byte data segment). The handler stores the text address in OSTask.ucode (0xfd0) and the display list position `k0 + k1 - gp` (the next command) in OSTask.data_ptr (0xff0),
copies **0xf18 bytes of text to IMEM 0x80** (the text's first 0xf18 bytes; F3DEX2 copies 0xf48) and the data to **DMEM 0** (F3DEX2: 0x180), waits for the DMA and jumps to IMEM 0x80. The text is 4096 bytes and the boot code puts it
at IMEM 0x80 with the DMA wrapping, so text bytes 0xf80 .. 0xfff lie at IMEM 0 .. 0x7f (the loop head 0x058 is there) and text 0xf18 .. 0xf7f at IMEM 0xf98 .. 0xfff (the DMA routines): the load does not replace them.

**What the front-end does** (`load_ucode` in `f3dex.c`): the arguments are noted for the probe first (`armlet_probe_note_load(text, data, size)`: `size` = `(w0 & 0xfff) + 1` bytes, the image of DMEM 0 here), then
* the same text (4096 bytes equal, at any address): a re-initialisation;
* another text: `loaded_build` looks its load key (FNV of the first 0xf48 bytes) up among the builds the probe latched and the registered ones of this front-end (`f3dex1_fam`); none: decline (`load_note`, below);
* the part of the text that stays in IMEM must be the current one's (text bytes 0xf18 .. 0xfff equal: they are in the 1.2x builds, not in the 0.95 builds or Turok's 1.00, which have no G_LOAD_UCODE anyway); else decline;
* a switch needs the whole data segment (`w0 & 0xfff` = 0x7ff): the builds' data differ in a few constants (the end of the overlay window at 0x396, the text tag at 0x2c8 ..) which the other build's code reads; a shorter image
  (a state no game produces) declines. A re-initialisation takes any length;
* F3DLX vertices loaded with `G_CLIPPING` off (the `junk` bits) and a data image that reaches the vertex buffer: decline;
* then the staged output goes out (`sync_out`: RDRAM, the FIFO write pointer, DPC_END), the registers of the loop head are set as the handler expects them (`t9 = w0`, `t8 = w1`, `k0` = the address after the window, `k1` = the window end
  minus the bytes left in it, `gp` = the window end), and the **interpreter runs the handler and the loaded microcode's initialisation up to its first command fetch** (the new build's `head`). That start-up is what a fresh task start does
  (the flags word of OSTask is clear): it patches the DMA table with the text address, sets the FIFO bounds and the RDP write pointer from OSTask, fetches the first window from data_ptr and enters the loop;
* **the loop's state is made anew from the interpreter's registers and DMEM**, as at the start of a task (`enter`): the display list pointer, the window, `wp` / `fstart` / `fend` from DMEM, the overlay window holds the text
  (`OV_TEXT`), the command kinds from the new build's handler tables (`make_kinds`), the record (`f->b`, `f->L`, `f->fx`) is the new build's. What is carried across is whatever the code does not write: DMEM above the data it
  loaded (OSTask, the matrices and the vertex buffer of a short image, the segment table ..), IMEM outside the text copied, the SP / DPC registers, RDRAM, and in the front-end the count of commands (the poll limit) and the
  `junk` bits. At most 64 loads in a task.

**Tasks that decline at a load** (S2DEX 1.x, an unregistered text, a tail that differs) are remembered as in the F3DEX2 front-end (`load_known` / `load_note`: text and data pointers of the task, tasks run / tasks that declined,
more than half: the next ones decline at the start, every 64th runs; mutation 466). Without the rule a task that declines at its load has done its native work for nothing; with it the non-loading tasks of a mostly loading pair are skipped as well (SpaceStation: 1093 instead of 1384 native tasks). The total RSP time with and without the rule was measured on the desktop and the rg35xxsp and is equal within the noise (docs/testing.md, "F3DEX 1.x G_LOAD_UCODE"), so the rule stays; `F3DEX_NO_LOADMEM=1` switches it off (a measurement aid).

**Next step, F3DEX 1.x -> S2DEX 1.x (not implemented).** SpaceStation Silicon Valley (57 of 60 captured tasks) and Pokemon Puzzle League load the S2DEX 1.x text (`af0007ff`, 4096 bytes, key 5c420d85..., data 2048 bytes) in the
middle of a task and go on in it; they decline at the load (what follows the load in the list was not examined here). What it would need:
1. **A model of S2DEX 1.x**, which is not in this tree (the S2DEX work is 2.x: `ucode-s2dex2.md`). The data image the games load has the same skeleton as the F3DEX 1.x data (class table at 0xb8, handler table at 0xc0, state
   words at 0x10c ..), so the loop, the window refill, the DMA of class-0 operands and the end of the task may be shared, but the handlers (object rectangles, backgrounds, the object matrix and texture loads, `G_RDPHALF`
   forms, its own `G_MOVEWORD` / `G_MOVEMEM` tables) are new: a spec (`docs/ucode-s2dex1.md`: disassemble the dumped texts `L...` of `tools/ucode_dump_run.sh`), closed forms with an oracle (the S2DEX 2.x ones are the pattern,
   `tools/hle/test_s2dex2.c`), a build record.
2. **A front-end that holds two kinds of record in one loop**, or a hand-over between the two front-ends: the F3DEX2 front-end does it by `f3_bind` over one `struct f3` (S2DEX 2.x is a record of the same family); here the
   second kind is another family (`f3dex1_fam` has `struct f3dex1_build`, no extension hook like `ext_cmd`). Simplest: S2DEX 1.x as `f3dex1_build` plus an `ext` command hook (kinds beyond the F3DEX set, run on the shared
   `struct f3dex_task`), the way the F3DEX2 front-end hosts S2DEX 2.x; `classify` would take `K1_EXT` kinds, and `load_ucode` would already do the switch: the text is looked up by load key, the interpreter runs the loaded text's
   initialisation to its `head`, `enter` rebuilds the state. Nothing in the switch itself is F3DEX-specific except the tail guard and the `size == 0x800` rule, whose reasons (shared IMEM tail, constants of the data) have to be
   re-established for S2DEX 1.x (its data differs from F3DEX 1.x's everywhere).
3. **The state S2DEX 1.x shares with F3DEX 1.x across the load**: what the data image does not overwrite. SSV loads a 0x800-byte image, so DMEM 0 .. 0x7ff is the S2DEX data and only 0x800 .. 0xfbf and OSTask are carried;
   the matrices and vertices of the F3DEX task are gone (a game that goes back to F3DEX in the same task loads it again with `G_RDPHALF_1` + `G_LOAD_UCODE`; that load, S2DEX 1.x -> F3DEX 1.x, is the same code with the other build current).
   The verify compare has to be told that the task switched (`hle_task.switched`: the output buffers of the two microcodes overlay each other and the scratch behind one-byte commands differs, as for S2DEX 2.x).
4. **The probe**: the loaded text is noted already (this branch); deriving an S2DEX 1.x record needs the derivation to know the family (`derive_f1d.py` / `derive.c` handle F3DEX 1.x, F3D, F3DEX2 and S2DEX 2.x).
5. **Tests**: a `load` synthetic case for the pair (the generator `tools/hle/mksyn_ex1.py` already loads a text from files: `MKSYN1_X_TEXT` / `MKSYN1_X_DATA` is the S2DEX 1.x image), perturbed copies of the SSV / PPL captures with
   `FUZZ_DONOR`, the 3700-frame gates on SSV and PPL, mutation checks as in `tools/hle/mutcheck_f1load.sh`.


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
