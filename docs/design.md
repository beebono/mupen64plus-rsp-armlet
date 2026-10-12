# Design

arMLEt is a Mupen64Plus RSP plugin that puts rsp-hle and parallel-rsp in one module and leaves a hook where native graphics handlers will go.
This page describes what is there; [testing.md](testing.md) shows that it behaves like the two-module pairing it replaces.

## Why

On the aarch64 handhelds a part of every frame goes to the RSP running the graphics microcode (F3D, F3DEX, F3DEX2 families) in parallel-rsp's
Lightning JIT, ahead of the RDP plugin ([foir](../../mupen64plus-rdp-foir)). Replacing that with native code (vertex transform and lighting,
clipping, triangle setup written for NEON) would be much cheaper, but a native version of a ucode has to produce exactly the RDP command
stream and RSP state the LLE run produces, or be correct enough to accept. That needs a safe place to plug it in, with the JIT as fallback and
as the bit-exact reference to compare against. arMLEt is that place. With `GfxNative = off` (the default) the plugin is a drop-in for
`mupen64plus-rsp-hle` + `RspFallback = parallel-rsp`; the routine hook is described under [Native routines](#native-routines).

## Flow of one RSP task

```
core: DoRspCycles
  plugin.c            hle_execute(&ctx.hle)                        rsp-hle's task detection (cached per ucode address)
    audio ABI1/2/3, nead, MusyX, JPEG, video, HVQM, cic x105        run as HLE inside rsp-hle's own code, set TASKDONE|BROKE|HALT
    graphics task (type 1), any unknown ucode, nead "mats"/"efz"    rsp-hle calls HleForwardTask()          <- hle_glue.c
      dispatch.c        armlet_gfx_try_native(ctx)                   GfxMode=hle only; false today
      rdp_defer.c       armlet_rdp_defer_task_begin()                 is it a graphics ucode of a known family? then ProcessRdpList is deferred
      hle_glue.c        parallelRSPDoRspCycles(~0u)                  parallel-rsp's JIT runs the ucode
                        (cp0.cpp: SP DMA, the ProcessRdpList callback = rdp_defer.c's, interrupts; the fork's flush hooks)
      rdp_defer.c       armlet_rdp_defer_task_end()                   nothing may be pending
  plugin.c            armlet_log_new_handler()                      logs a ucode's HLE handler once per new cache entry
```

`InitiateRSP` sets `hle_gfx = hle_aud = 0` after `hle_init`. This is the same as the `DisplayListToGraphicsPlugin = False` and
`AudioListToAudioPlugin = False` that the rsp-hle + parallel-rsp setup used: rsp-hle never sends a list to the core's graphics or audio
plugin, so every graphics task takes the `unknown_task` path and ends in `HleForwardTask`. rsp-hle needs no change for this, and
`HleForwardTask` is the single point where a graphics task leaves rsp-hle.

What is shared and what is separate compared with the two-module pairing:

| | rsp-hle + RspFallback | arMLEt |
|---|---|---|
| modules | two `.so`, the fallback `dlopen`ed by rsp-hle | one `.so`; parallel-rsp's `parallelRSP*` entry points are called directly |
| RSP_INFO to parallel-rsp | the core's, unchanged | a copy (`ctx.prsp`), unchanged in the normal module; the profiler build swaps `ProcessRdpList` / `CheckInterrupts` in the copy for timing wrappers, without touching `cp0.cpp` |
| `PluginStartup` / `Shutdown` of the fallback | called, no-ops in parallel-rsp | none |
| `RomClosed` | rsp-hle clears its ucode cache, then the fallback's `RomClosed` | the same, plus the ucode-id cache |
| second `InitiateRSP` in one process | the fallback is unloaded and loaded again (fresh JIT) | the JIT state of the first one stays (the core calls it once per run; a front-end that reuses the process would see a warm JIT) |
| messages | rsp-hle's, unprefixed | rsp-hle's unchanged; arMLEt's own start with `arMLEt: ` |
| config | `Rsp-HLE` (`RspFallback` ...) | `Rsp-Armlet`, see [options.md](options.md) |

## RDP list coalescing

A graphics ucode writes `DPC_END` after every chunk of RDP commands it has DMAed to its output buffer, and parallel-rsp calls the video plugin's
`ProcessRdpList` for each write: 570-1400 calls a frame in Super Mario 64, Banjo-Kazooie and Mario Kart 64, most of them for one 8-byte command, each costing
0.3-0.7 us of fixed work ([profiling.md](profiling.md#the-rdp-callback-cost-per-call)). `src/rdp_defer.c` replaces the callback in the `RSP_INFO` copy parallel-rsp gets with one
that records the range and hands the video plugin the concatenation of several lists in one call. It is exact: the video plugin sees the same bytes
in the same order, the ucode sees the same registers, and nothing that the rest of the machine can observe moves relative to it. Option: `RdpCoalesce`,
threshold `RdpCoalesceBytes` ([options.md](options.md)).

**What the video plugin does with a list** (foir, `rdp_process_list`): it reads the words from `DPC_CURRENT` to `DPC_END` out of RDRAM (out of DMEM when
`DPC_STATUS.XBUS` is set), feeds them to the command parser (a command cut by the end of the list carries over to the next call), and then sets
`DPC_START = DPC_CURRENT = DPC_END`. A list with `end <= current` is ignored, registers untouched. `DPC_CURRENT` is the previous `DPC_END` at every
call (that is what the last call left), so successive lists are contiguous and their concatenation is one list; the parser is a byte stream, so
where it is cut does not matter. This argument is about foir (`RdpCoalesce = auto` is on only when the module behind the core's callback is foir; `on` forces it).

**The deferral** (`defer_cb`, called by `cp0.cpp` after it stored `DPC_END`): in a task of a known graphics family it computes what foir would compute
(`end <= current`: ignore; XBUS: see below), appends `[current, end)` to one pending range `[ps, pe)` and writes `DPC_START = DPC_CURRENT = DPC_END` itself.
The registers the ucode can read are therefore exactly what they are without deferral, at every instant: `MFC0` of `DPC_START`, `DPC_END`,
`DPC_CURRENT` needs no hook, and `DPC_STATUS`, `DPC_CLOCK`, `DPC_BUFBUSY`, `DPC_PIPEBUSY` and `DPC_TMEM` are not touched by foir. Running the pending range
(`run_pending`) sets `DPC_CURRENT = ps`, `DPC_END = pe`, calls the video plugin, and restores the three registers to what they were (equal to what foir leaves
unless the ucode has moved on, which it cannot, see the triggers).

| trigger | action | why it is needed |
|---|---|---|
| `DPC_END` write, range >= `RdpCoalesceBytes` | run the range | bounds the delay; the video plugin's worker threads keep getting work while the ucode runs |
| `DPC_START` write (`cp0.cpp`, `PRSP_RDP_FLUSH_START`) | run the range before the write | a new buffer (FIFO wrap) resets current and end; the pending range was submitted with the old ones |
| `DPC_STATUS` write (`PRSP_RDP_FLUSH_STATUS`) | run the range before the write | it can change XBUS (where the list is read from) and FREEZE / FLUSH |
| `DPC_END` write while `XBUS` is set | run the range, then this list at once | the list is in DMEM, which the ucode overwrites; XBUS lists are never deferred (Mischief Makers uses them for every list; Glover for some) |
| SP DMA write into RDRAM that overlaps the range (`prsp_rdp_defer_dma_write`, every row, wrapping rows count as overlapping) | run the range before the copy | the ucode overwrites command memory that is still pending (the FIFO is a ring) |
| the run loop of the task ends (`parallelRSPDoRspCycles`, `PRSP_RDP_FLUSH_RUN_END`) | run the range before the interrupts are checked and `SP_PC` is stored | halt, break, yield, `SET_INTR`, a poll timeout: the task is over for the core, which then looks at `MI_INTR` (the DP interrupt of a `SYNC_FULL`) and at the frame |
| `armlet_rdp_defer_task_end` | run the range if any | belt and braces after the fork's hook; counted separately, zero in all runs |
| `DPC_END` write with `end <= current` | nothing | foir ignores it too |

What needs no trigger, and why:

- **Reads of `DPC_START` / `DPC_END` / `DPC_CURRENT` / `DPC_STATUS`** (polling for the RDP to drain, buffer-full handling): the registers hold what foir would have left, and
  foir never changes `DPC_STATUS`. A ucode that waits for the RDP to finish sees it finished at once, as without deferral (foir's call is synchronous).
- **`SYNC_FULL` and the DP interrupt**: foir raises `MI_INTR_DP` and calls `CheckInterrupts` while it parses the list that holds the sync. `CheckInterrupts` is a no-op in
  the core's RSP and graphics `INFO`; the core looks at `MI_INTR_DP` after `DoRspCycles` returns (`do_SP_Task`), and `MI_INTR_SP` is only raised by the core after that. Nothing reads
  `MI_INTR` in between (the RSP cannot), so running the sync at the end of the run loop instead of in the middle changes nothing the core can see. The deferred range always runs
  before the run loop's own `CheckInterrupts` call and before `DoRspCycles` returns.
- **`RomClosed`, state saves**: a state is saved by the core between tasks, and nothing is pending between tasks. `RomClosed` forgets a (never present) range.
- **The order of the RDP's work against the ucode's other effects**: the RDP reads and writes RDRAM (textures, the frame buffer, Z); the RSP's DMA reads and writes touch RDRAM too. A deferred
  list sees RDRAM as it is at the flush, so the argument needs that no RSP DMA changes anything the pending lists read or write, and that no RSP DMA read sees what pending lists
  would have written. The DMA-write case that is certain, the output buffer itself, is the overlap trigger. For the rest the module relies on what the ucodes do: it defers only in tasks whose
  ucode text carries an `F3D` / `F3DEX` / `F3DEX2` family banner (`gfx_ucode.c`; Diddy Kong Racing, Jet Force Gemini, Indiana Jones, Worms and other ucodes without one are passed through). Those ucodes
  write RDRAM only for the output buffer, the yield area and a few small blocks (Banjo-Kazooie and Banjo-Tooie write 0x48 to 0x1b0 bytes at low addresses, a few times per task), and read
  display lists, vertices and matrices, not what the RDP rendered. This is an assumption about the ucode, not something the module checks. What backs it: in the survey of
  [testing.md](testing.md) (63 ROMs, `RdpCoalesceBytes` at the default and at its maximum of 1 MiB) all of RDRAM, hashed after every `DoRspCycles`, is identical with and without coalescing (three games whose RDRAM is not deterministic even without it show no difference in everything else).

The fork changes are behind `PRSP_RDP_DEFER` (`ext/parallel-rsp/prsp_rdp_defer.h`, branch `rdp-defer`): four call sites in `rsp/cp0.cpp` and `parallel.cpp`, empty macros when the
define is absent, so the default fork build is byte-identical ([testing.md](testing.md)). The module defines the two hook functions.

## Native routines

Native routines replace stretches of a graphics microcode (a vertex transform, a triangle setup) with C / NEON code that works on the RSP state, while the JIT
keeps running everything around them and stays the reference. The interface, with the exactness contract, is `src/native/native.h`; the framework is
`src/native/native.c`; the hook in the fork is `ext/parallel-rsp/prsp_native.h` (`-DPRSP_NATIVE`, the fork's `native-hook` branch). Nothing of this runs unless
`GfxNative` is `on` or `verify` ([options.md](options.md)).

**The hook (fork).** The JIT enters a block through `blocks[pc]`, and the enter thunk looks that table up inline; only a block that is not compiled yet goes
through `get_jit_block`. The hook lives on that miss path, so a block that has no routine costs nothing:

- the host declares *entry* pcs (the first instruction of a routine) and *split* pcs (entries and static exits). On a miss at an entry pc `get_jit_block` asks the
  host's `lookup(pc, imem)`: does a routine apply to the IMEM content as it is now? If so `blocks[pc]` gets a *stub*, a few instructions that call a thunk
  (`native_thunk`, next to the enter and return thunks), instead of a compiled region. The thunk calls `CPU::native_enter`, which returns the block to continue in:
  the one at the pc the host returns after running the routine, or, if the host declines (or a delay slot is pending: a stub is entered at a block boundary
  only), the JIT's own region for the entry pc, compiled on demand.
- A compiled region never extends across a split pc (the region ends before it and continues through the enter thunk). Without this a `j` or a fall-through
  inside a region would reach the entry without going through `blocks[]`, and the JIT could not be stopped at an exit either.
- The match is keyed on **content**. The stub sits in `blocks[]` like any region, so SP DMA that changes the code removes it (the existing 256-byte-block
  invalidation), and `invalidate_code()` also removes a stub when a changed block overlaps `[dep_lo, dep_hi)` of its match (what the host's decision depended
  on, beyond the region the JIT would compile). The next entry looks up again. The host decides on (ucode id, entry pc, FNV-1a-64 of IMEM
  `[dep_lo, dep_hi)` followed by `[dep2_lo, dep2_hi)`, or of `hash_ranges` when the code is spread over more places): `struct armlet_routine`. A match carries two ranges; a changed
  block that overlaps neither (the overlay window between them) leaves the stub alone.
- The *oracle*: `prsp_native_oracle(entry, stop)` runs the JIT's own code from an entry until it is about to execute `stop`, nested inside the host's `enter`
  (`enter_frame` is re-entrant: it is the C-callable entry of the thunks). Stubs decline meanwhile; the entry pc runs the JIT region, `stop` has a one-instruction
  block that returns `MODE_NATIVE_STOP`. A `CHECK_FLAGS` return (SP DMA into IMEM that changed code) is handled as `run_rsp_task` does: invalidate and go on,
  unless an interrupt or a halt ends the task.
- `prsp_native_record(1)`: `RSP_MTC0` and the DMA code emit an event for every effectful SP register write (DMA read, DMA write with an FNV hash of the bytes
  written to RDRAM, SP_STATUS, semaphore, DPC_START, DPC_END with a hash of the list bytes, DPC_STATUS), whether the JIT or the host caused it.
  `prsp_native_sandbox(1)`: SP DMA writes to RDRAM are done but their old bytes kept (put back by `sandbox(0)`), the RDP is not run (no `ProcessRdpList`, no
  deferral flush). `prsp_native_save/load` copy all RSP state (GPRs, vector registers, accumulator, flags, divide unit, DMEM, IMEM, SP/DPC registers, interrupt flag,
  the status-poll counters, the JIT pc and delay-slot state) to and from a `PrspNativeSnap`.

With `PRSP_NATIVE` undefined the fork compiles to the same object code as before (md5s of every object and the `.so` are identical to the base commit; [testing.md](testing.md)).

**The routine.** `int run(struct armlet_env *e)`: `e->rsp` is a view of the JIT's state (GPRs, vector registers, accumulator, flags, divide unit, DMEM words, IMEM), the
return value is the pc to continue at, or `ARMLET_NATIVE_DECLINE` with the state untouched. The routine never touches RDRAM or the RDP: `armlet_mtc0`, `armlet_mfc0`,
`armlet_dma_read` and `armlet_dma_write` do what the microcode's `mtc0` / `mfc0` sequences do (the fork's own DMA code, the same RDP-list path with its
coalescing), so verify mode can sandbox them. Routines register themselves with `ARMLET_ROUTINE(x) = { ... }` into the section `armlet_routines`; a family adds one
file under `src/native/` and touches nothing shared.

**The exactness contract.** When a routine returns the pc P, the machine is what the microcode run from the entry up to its first arrival at P leaves:

| state | compared |
|---|---|
| GPRs `sr[1..31]`, vector registers `vr[0..31]`, accumulator (HI, MD, LO), VCO / VCC / VCE, `div_out` / `div_in` / `dp_flag` | all, unless the routine declares them dead (below) |
| DMEM, IMEM | all 4 KiB each |
| the SP / DPC registers | final values (the DPC registers are the video plugin's once a list was submitted: compared through the events) |
| the effectful SP register writes | the same sequence: DMA reads (addresses, length register), DMA writes (and the hash of the bytes written to RDRAM), DPC_START / DPC_END (and the hash of the list), SP_STATUS, semaphore |
| the status-poll counters (`MFC0` of SP_STATUS), `SP_STATUS_TIMEOUT`, the interrupt flag, IMEM dirty bits (if neither side did IMEM DMA) | equal |
| the exit pc, no delay slot pending | equal |

A routine may declare live-out masks (`dead_sr`, `dead_vr`, `dead_acc`, `dead_flags`, `dead_div`, and the lane-granular `dead_vr_lanes[n]` / `dead_flag_lanes[k]` /
`dead_acc_lanes[k]`): state that the microcode after the routine's exits never reads (or overwrites before reading), so that the native routine need not compute it. Verify
excludes exactly these; `ARMLET_NATIVE_POISON=1` overwrites them (only the dead lanes) with a pattern after the routine, so that a wrong claim shows up downstream. The
default, and the goal, is full equality.

**How a mask is established, and what is not enough.** A static liveness listing (`tools/native/liveness.py`) cannot establish a mask: an indirect jump has more targets
than any run shows, and partial writes (a load of 8 bytes) make nearly every register look live. A mask that passed verify (which only compares what is not masked) and a
static check has failed the RSP hash-stream equivalence in practice (a first F3D claim diverged at the 580th task). The rule: a claim is **probed**, then **validated**.
Probing: an oracle-only routine (`run = NULL`, below) with `ARMLET_NATIVE_PROBE="sr=0x..,vr=0x..,vl<n>=<lane mask>,acc,vco,vcc,vce,fl<k>=<bit mask>,div"` runs the JIT as always and
poisons the named state after the stretch; the claim holds for a run if the RSP hash stream, state stream, screenshots and audio of `tools/equiv` stay identical to the
reference (`ARMLET_NATIVE_POISON=1` does the same for the declared masks of a native routine). Validated: the masks of a routine are kept only if the POISON runs of all the
games the routine targets, and the verify runs on devices, are identical; and the routine's file says how they were established and over which runs.

**Oracle-only routines** (`run = NULL`): the framework runs the JIT from the entry to `exit_pcs[0]` and does nothing else but what the environment asks: snapshots
(`ARMLET_NATIVE_CAPTURE`), probing (`ARMLET_NATIVE_PROBE`). They are how entry snapshots of a routine are taken before it exists natively, and how its live-out is found.
Their names start with `cap.` and they are selected only when named.

**Verify mode** (`GfxNative=verify`, for tests): at every entry the framework saves the state, runs the routine in the sandbox with recording on, puts the
state and RDRAM back, runs the JIT's code to the routine's exit with recording on (this is the run that stands: the machine always continues from the JIT's
result), saves, and compares. The first differences are reported with their values (`native %s MISMATCH`, the registers, vector lanes, DMEM words, the
first differing SP write); `ARMLET_NATIVE_ABORT=1` aborts at the first one, `ARMLET_NATIVE_CAPTURE=<dir>` writes snapshot files of entries (and of mismatches).
A routine that declines must leave the state as it found it: verify checks that too. If the JIT run does not arrive at the pc the routine returned (a wrong exit)
that is a mismatch as well.

**Offline harness** (`tools/native/replay.c`, `scripts/build.sh <target> --tools` -> `armlet-native-replay`): a snapshot file holds the entry state (`PrspNativeSnap`: RSP state, DMEM,
IMEM), the RDRAM rows the routine's DMA reads read, and the state and effect log the JIT left. The program loads it into a private RSP (the fork's `JIT::CPU`, private RDRAM)
and runs the same verify as the module does, then checks that the JIT's result still equals the recorded one. `--bench N` times the routine and the JIT's code for the same
stretch from the snapshot, N times each. Snapshots come from any run with `ARMLET_NATIVE_CAPTURE=<dir>` and are ROM-derived: keep them in the git-ignored `test-output/`.

**Selection.** Each graphics task identifies its ucode as before (`gfx_ucode.c`); when the id changes, the routines of that ucode that the allow-list admits
(`GfxNativeRoutines`) are selected and their entry and exit pcs given to the fork (which drops its compiled blocks only if the sets changed). A routine named
`test.*` is only selected when the allow-list names it. DMEM data that a routine relies on being constant is the routine's business: it reads it from DMEM per
call, or checks it (ucode id and IMEM content identify the code, not the data segment).

## Profiler

`src/prof/` is compiled only with `-DARMLET_PROFILE` (`scripts/build.sh <target> --prof`, a separate `.so`); the normal module contains none of it and is
byte-identical to a build of the tree without it. It measures where the graphics time goes: per task (type, ucode id), the JIT address map
and, with `--prof3`, how often every instruction of the ucode ran. It watches parallel-rsp through `ext/parallel-rsp/prsp_hooks.h`: `PRSP_HOOK_*`
macros at region compilation (code address, size and the code offset of every instruction), VU stub and thunk creation, block lookup and
compile time, IMEM invalidation, SP DMA, and the per-instruction counter, which expand to nothing unless `PRSP_HOOKS` is defined. Method and
results: [profiling.md](profiling.md).

## Source layout and build

`src/plugin.c` is the six API functions. `config.c` follows rsp-hle's lookup of the core's config API through `dlsym` and keeps the section
version handling. `hle_glue.c` is what rsp-hle's `plugin.c` provided (message and callback forwards, `HleForwardTask`) plus the handler log. `dispatch.c` and
`gfx_ucode.c` are the native-path hook. `log.c` routes messages to the core's debug callback (and to `ARMLET_LOGFILE`).

One CMake target links arMLEt's files, rsp-hle's 13 C files (as upstream's Makefile builds them: `-O3 -DNDEBUG -ffast-math`, hidden visibility,
`-Wall`; no `plugin.c`, no osal) and the 11 parallel-rsp C++ files + `parallel.cpp` (C++14, `-O3 -ffast-math`, the fork's warning flags,
`PARALLEL_INTEGRATION M64P_PLUGIN_API PARALLEL_RSP_LIGHTNING`) with Lightning as a static library. Points that are easy to get wrong:

- `rsp/vu_inline.cpp` is compiled with `-ffixed-x9..x15,x18,x20..x28` on aarch64, the same string as the fork: the VU op bodies the JIT copies into
  its code must not touch an integer register the JIT keeps live. The property is set in the top-level CMakeLists (source file properties are
  directory-scoped).
- No `-flto` (it does not mix with the `-ffixed-*` file) and no `-mcpu` / `-march`: generic armv8-a, as measured for the other plugins.
- Include directories are per language. rsp-hle has headers called `memory.h`, `common.h` and `audio.h`, which would shadow system or parallel-rsp
  headers for the C++ side if they were global.
- `-Wl,--version-script=src/armlet_api_export.ver -Wl,-Bsymbolic -Wl,--exclude-libs,ALL`: the module exports exactly `PluginStartup`,
  `PluginShutdown`, `PluginGetVersion`, `DoRspCycles`, `InitiateRSP`, `RomClosed`. Lightning's `_emit_*` and parallel-rsp's `parallelRSP*` stay private
  (a symbol clash with another plugin, or with the fallback copy of parallel-rsp, cannot happen).
- `cmake/check_sources.cmake` fails the configure step if parallel-rsp's `PARALLEL_RSP_SOURCES` (or its plugin target) or rsp-hle's `SOURCE` list
  differs from `cmake/prsp_sources.cmake` / `hle_sources.cmake`, so a submodule update cannot silently drop a translation unit.

The rsp-hle asserts are compiled out (`-DNDEBUG`), as in upstream's release build. A failed assert would otherwise abort the emulator.

## Graphics ucode identification

`gfx_ucode.c` identifies the microcode of a task by hashing its text: FNV-1a-64, one 32-bit host-endian RDRAM word per step, over
`ucode_size` bytes of `TASK_UCODE` (at most 4 KiB; 0xf80 when the game leaves the field 0, as several do). This is the key the earlier
instruction-profiling work used. rsp-hle's own cache is keyed on the ucode's *address* and says nothing about what is at that address, so it
is not trusted for this. The id is recomputed only when the text changed: four recent texts are kept and compared with `memcmp`
(vectorised in libc) against RDRAM first. The hash itself is a serial multiply chain and has no NEON form; there is a scalar reference only
(`armlet_ucode_hash`).

The first time an id is seen, one line is logged, with the family (`F3D`, `F3DEX`, `F3DEX2`, from the "RSP Gfx ucode ..." banner strings
read from the ucode text and data at run time) and whether a handler exists:

```
arMLEt: gfx ucode 0d3f849c8dd309c4 F3DEX2: RSP Gfx ucode F3DZEX.NoN fifo 2.06H Yoshitaka Yasumoto 1998 Nintendo. | ... [no native handler, LLE]
```

This is computed only in `GfxMode = hle`. With `lle` (the default) a graphics task goes straight to the JIT with no hashing.

## Task-level native path

A graphics task of a ucode that has an `hle_ucode` entry (`src/hle/hle.h`; `GfxMode=hle`, optionally restricted by `GfxHleUcodes`) is run by `armlet_hle_run` (`src/hle/hle_core.c`) instead of
by the JIT; the JIT stays the reference and the fallback. Nothing of this runs in the default `GfxMode=lle`, and it needs the video plugin to be foir behind the coalescing path
(`RdpCoalesce` resolved on), because the commit replays the RDP lists through that path.

**Structure.** A *family* (`struct hle_family`) has a `run(task, ucode)`; a *build* of a microcode is data (`struct hle_ucode`, registered with `HLE_UCODE()` into the section `hle_ucodes`,
found by the ucode id): the layout offsets, flags, the variants within the family (`vtx_variant`, `tri_variant`, `clip_variant`), the family's own constants (`fam_data`), and the DMEM ranges
that are dead at the end of a task. The shared library is the fixed-point arithmetic of the RSP as plain integer functions (`hle_fx.h`; the closed forms and their test,
`tools/hle/prim_test.c`, are Fleet B's: 0 failures over 200 000 trials x 8 lanes against the exact VU instruction sequences, and a mutation check) and the task framework. F3DEX2 is the first
family (`hle_f3dex2.c`): its four builds (Donkey Kong 64, Paper Mario, Ocarina of Time, Pokemon Snap) are tables, and its `run()` is the interpreter below.

**What a family must produce.** Only what can be observed afterwards: the RDRAM the task writes, the concatenated RDP command stream, the SP / DPC / MI registers, the OSTask words the ucode writes, and
DMEM except the ranges the build's table declares dead. The vector unit, the scalar registers, the accumulator and the flags are not part of it (the next task loads them before use). A family
therefore does not have to imitate the microcode's internal structure (its RDP output buffers, its vertex slot records) except where a later task, or the microcode's own next command, reads it.

**The run.** `armlet_hle_run` saves the RSP state (`prsp_native_save`), zeroes the status-poll counters (the JIT does at the start of a task), switches the fork's sandbox on (SP DMA writes
to RDRAM keep their old bytes, the RDP is not run) and calls the family. The family works on the real DMEM / IMEM / RDRAM and the SP / DPC registers through `hle_dma_read`,
`hle_dma_write`, `hle_mtc0`, `hle_poll_status` (the fork's own DMA and register code, so the registers, IMEM dirty bits and events come out as the JIT's). The DMA writes and the DPC
register writes are logged. When the family returns `HLE_OK` the task is complete; `HLE_DECLINE` (`hle_decline(task, "why")`) gives the whole task to the JIT: the state is loaded back and
the RDRAM restored, so nothing is split. At the commit the sandbox is switched off (RDRAM back to its state at the task start), then the log is replayed in order through
`prsp_rdp_defer_dma_write` / `prsp_native_mtc0`, which is exactly the call sequence of a JIT run, so the video plugin and the RDRAM end identical. `SP_PC`, `SP_STATUS` (BROKE | HALT, the
SP interrupt) are what `BREAK` and the run loop leave.

**The interpreter (`hle_rsp.cpp`).** A small RSP interpreter over the real memories: the scalar unit in plain code, the vector unit and the vector loads / stores through the fork's per-instruction
helpers (the JIT's helper-call path; `tools/native/trace.cpp` uses the same ones), MFC0 / MTC0 through the framework. It is the exact reference of the task-level path: a family with no native code
runs on it (that is the F3DEX2 `run()` today, and `ARMLET_HLE_INTERP=1` forces it for any family), `ARMLET_HLE_VERIFY=interp` compares a native front-end with it, and the offline harness replays
captured tasks with it. It reproduces all captured tasks of the four games (570 tasks of Donkey Kong 64, Ocarina of Time, Paper Mario, Pokemon Snap). It is slower than the JIT (about 3x on a Donkey
Kong 64 task on the x86 desktop; on the A55 about 80 ns per interpreted instruction against the JIT's 11 ns) and is not a production path. Earlier work on *hooks* inside the interpreter (native handlers taking over at a pc) and on the JIT running the
stretches between them was dropped in favour of whole-task native front-ends (see below); the hook code is not part of the tree.

**What verify compares** (`ARMLET_HLE_VERIFY=1` or `jit`: the oracle is the JIT; `=interp`: the oracle is the interpreter, and the JIT then runs the task for real; and the offline harness): the
native run in the sandbox, then the state is put back and the oracle runs the task; then, native against oracle: all of DMEM (except the ucode's declared dead ranges) and IMEM, the SP / DPC registers,
MI_INTR (without the DP interrupt bit, which the RDP raises when it processes a full sync), the pc, `SP_STATUS_TIMEOUT`, the IMEM-changed bits (the oracle's must be a subset of the native run's),
the RDRAM written by SP DMA (the bytes of every range either side wrote, except the chunks of the RDP output buffer: those are compared as the stream), the sequence of recorded SP register writes
(DMA reads and writes with the hash of the bytes, status, semaphore, DPC_START / END / STATUS), and **the bytes of the RDP lists as one stream** (FNV-1a-64 and length of all list bytes in order,
with the second word of NO_OP and the sync commands zeroed: the RDP ignores it and the microcode leaves stale scratch there). Not compared: list boundaries, because they are unobservable to foir
(coalescing already relies on it; a family that submits fewer, larger lists names the event kinds to skip in `ev_ignore`); the scalar and vector registers, the accumulator, flags and divide unit
(`ARMLET_HLE_POISON=1` fills them with a pattern after a native task so that a wrong claim shows up in the equivalence runs); the status-poll counters (the JIT zeroes them at every task start).

**DMEM masks.** A build may declare DMEM ranges dead at the end of a task (`hle_dead_range`: scratch the next task overwrites before reading, or never reads). A mask is data in the build's table, cites
how it was established, is applied identically by the verify compare, the poison probe and the `tools/equiv` hash (`armlet_hle_print_masks` feeds the shim), is validated by the equivalence runs
of all the target games on both devices, and never covers RDP lists, the output FIFO state, the DL stack or anything the RDP or a later task reads. **The evidence is per family**: the F3D family
leaves DMEM dead except the OSTask header (Fleet C's poison runs), F3DEX 1.x does not reload DMEM 0x800-0xfbf between tasks; each family's mask needs its own proof.
The probe is `ARMLET_DMEM_POISON=<lo>-<hi>[,...] | mask | all` (`hle_task.c`): at the end of every graphics task, whether the JIT or the task-level path ran it, those DMEM bytes are overwritten with a
pseudo-random pattern (a different one each time); `mask` is the build's dead ranges, `all` is [0, 0xfc0). A range is dead when a run with it poisoned gives the events, the RDP stream, the
pictures and the audio of the unpoisoned run (docs/testing.md).

**Capture and offline replay.** `ARMLET_HLE_CAPTURE=<dir>` (with `_EVERY`, `_MAX`) writes snapshot files of task starts (state, the RDRAM rows the task's DMA reads read, the JIT's end state,
the recorded events and the JIT's RDP stream) for tasks the JIT runs; `armlet-native-replay file.snap...` runs the family on them and compares with the recorded JIT result
(`--bench N` times the front-end and the JIT; `HLE_POKE=addr=val,...` sets DMEM bytes first). Snapshots are ROM-derived: keep them in `test-output/`.

**Environment.** `ARMLET_HLE_VERIFY` (`1`, `jit`, `interp`), `ARMLET_HLE_INTERP`, `ARMLET_HLE_STATS` (counts, decline reasons, time), `ARMLET_HLE_POISON`, `ARMLET_DMEM_POISON`, `ARMLET_HLE_ABORT`,
`ARMLET_HLE_QUIET`, `HLE_RSP_DEBUG=<n>` (the first n interpreted instructions), `HLE_RSP_TRACE` (every interpreted instruction with the vector register it wrote), `HLE_RSP_WATCH=<addr>` (every change of
a DMEM byte, with the pc), `HLE_RSP_PROFILE` (executions per pc, `PROF pc count` lines on stderr at the end of every task).

## Native handler contract (superseded)

`armlet_gfx_try_native` still accepts a task only when `GfxMode = hle`; boot size <= 0x1000 (a task, not a bare RSP program); type 1; fresh (`SP_PC` 0 and the yield flag
clear); its ucode id has a handler (an `hle_ucode` entry, see above) and the id is in `GfxHleUcodes` when that list is not empty. The handler `run` returns non-zero when it completed the
task; returning 0 changes nothing and the JIT runs the task. What a completed task leaves is described in "Task-level native path"; the JIT stays the reference.
