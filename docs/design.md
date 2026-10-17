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
  ucode text carries an `F3D` / `F3DEX` / `F3DEX2` family banner (`gfx_ucode.c`; Worms and other ucodes without one are passed through, except Diddy Kong Racing, Jet Force Gemini and Indiana Jones in `GfxMode = hle`, whose microcodes have a task-level front-end: [native-custom.md](native-custom.md)). Those ucodes
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
**the whole 4 KiB at `TASK_UCODE`, whatever `ucode_size` says**. (Games pass 0, 0xf80 or 0x1000 for one text: the boot code loads 0xf80 bytes and the
microcode loads the tail itself. Until 2026-10-16 the hash covered `ucode_size` bytes, so one text had up to three ids: Rocket's F3DEX 2.07 and Donkey Kong 64's
were the same 4 KiB under two ids, 5 of the 58 ids of the survey were such duplicates, and registering a text meant registering it per size; [ucode-survey.md](ucode-survey.md).) rsp-hle's own cache is keyed on the ucode's *address* and says nothing about what is at that address, so it
is not trusted for this. The id is recomputed only when the text changed: four recent texts are kept and compared with `memcmp`
(vectorised in libc) against RDRAM first. The hash itself is a serial multiply chain and has no NEON form; there is a scalar reference only
(`armlet_ucode_hash`).

The first time an id is seen, one line is logged, with the family (`F3D`, `F3DEX`, `F3DEX2`, from the "RSP Gfx ucode ..." banner strings
read from the ucode text and data at run time) and whether a handler exists:

```
arMLEt: gfx ucode 0d3f849c8dd309c4 F3DEX2: RSP Gfx ucode F3DZEX.NoN fifo 2.06H Yoshitaka Yasumoto 1998 Nintendo. | ... [no native handler, LLE]
```

This is computed only in `GfxMode = hle`. With `lle` (the default) a graphics task goes straight to the JIT with no hashing.

Offline companions: [ucode-survey.md](ucode-survey.md) (which texts exist and how close they are), [ucode-coverage.md](ucode-coverage.md) (how many graphics tasks and how much RSP time of 64 games run native today, and the ranked list of what is left) and
[ucode-derive.md](ucode-derive.md) (`tools/derive_build.py`: derives the build record of an unregistered text from the registered builds and lists what it cannot explain; validated against the hand-written records).

## Task-level native path

A graphics task of a ucode that has an `hle_ucode` entry (`src/hle/hle.h`; `GfxMode=hle`, optionally restricted by `GfxHleUcodes`) is run by `armlet_hle_run` (`src/hle/hle_core.c`) instead of
by the JIT; the JIT stays the reference and the fallback. Nothing of this runs in the default `GfxMode=lle`, and it needs the video plugin to be foir behind the coalescing path
(`RdpCoalesce` resolved on), because the commit replays the RDP lists through that path.

**Structure.** A *family* (`struct hle_family`) has a `run(task, ucode)`; a *build* of a microcode is data (`struct hle_ucode`, registered with `HLE_UCODE()` into the section `hle_ucodes`,
found by the ucode id): the layout offsets, flags, the variants within the family (`vtx_variant`, `tri_variant`, `clip_variant`), the family's own constants (`fam_data`), and the DMEM ranges
that are dead at the end of a task. The shared library is the fixed-point arithmetic of the RSP as plain integer functions (`hle_fx.h`; the closed forms and their test,
`tools/hle/prim_test.c`, are Fleet B's: 0 failures over 200 000 trials x 8 lanes against the exact VU instruction sequences, and a mutation check) and the task framework. F3DEX2 is the first
family (`hle_f3dex2.c`): its four builds (Donkey Kong 64, Paper Mario, Ocarina of Time, Pokemon Snap) are tables, and its `run()` is the native front-end described under "The F3DEX2 front-end" below; the interpreter below is its exact reference.
The F3DEX 1.x front-end (`f3dex.c`) is data-driven the same way (since 2026-10-17): a build is `struct f3dex1_build` (`f3dex_bld.h`: the DMEM layout, the handler pcs, the reject mask, the NoN / LX / G_CULLDL
variants), the commands go through the build's own handler tables in DMEM, and nothing tests which build it is ([native-f3dex.md](native-f3dex.md) section 3).

**What a family must produce.** Only what can be observed afterwards: the RDRAM the task writes, the concatenated RDP command stream, the SP / DPC / MI registers, the OSTask words the ucode writes, and
DMEM except the ranges the build's table declares dead. The vector unit, the scalar registers, the accumulator and the flags are not part of it (the next task loads them before use). A family
therefore does not have to imitate the microcode's internal structure (its RDP output buffers, its vertex slot records) except where a later task, or the microcode's own next command, reads it.

**The run.** `armlet_hle_run` saves the RSP state (`prsp_native_save`), zeroes the status-poll counters (the JIT does at the start of a task), switches the fork's sandbox on (SP DMA writes
to RDRAM keep their old bytes, the RDP is not run) and calls the family. The family works on the real DMEM / IMEM / RDRAM and the SP / DPC registers through `hle_dma_read`,
`hle_dma_read`, `hle_dma_write`, `hle_mtc0`, `hle_poll_status` (the fork's own DMA and register code, so the registers, IMEM dirty bits and events come out as the JIT's), and, for a front-end that does not need the
fork's DMA for a write to RDRAM, `hle_rdram_write` (the same effect on the log, the sandbox and the compare at a quarter of the cost: the fork's sandbox allocates an undo record for every DMA row; this one keeps its own, applied when the sandbox goes off). The DMA writes and the DPC
register writes are logged. When the family returns `HLE_OK` the task is complete; `HLE_DECLINE` (`hle_decline(task, "why")`) gives the whole task to the JIT: the state is loaded back and
the RDRAM restored, so nothing is split. At the commit the sandbox is switched off (RDRAM back to its state at the task start), then the log is replayed in order through
`prsp_rdp_defer_dma_write` / `prsp_native_mtc0`, which is exactly the call sequence of a JIT run, so the video plugin and the RDRAM end identical. `SP_PC`, `SP_STATUS` (BROKE | HALT, the
SP interrupt) are what `BREAK` and the run loop leave.

**The interpreter (`hle_rsp.cpp`).** A small RSP interpreter over the real memories: the scalar unit in plain code, the vector unit and the vector loads / stores through the fork's per-instruction
helpers (the JIT's helper-call path; `tools/native/trace.cpp` uses the same ones), MFC0 / MTC0 through the framework. It is the exact reference of the task-level path: a family with no native code
runs on it (`ARMLET_HLE_INTERP=1` forces it for any family), `ARMLET_HLE_VERIFY=interp` compares a native front-end with it, the offline harness replays captured tasks with it, and the F3DEX2 front-end uses it for the
boot code (about 140 instructions up to the first fetch of the command loop) and for the end of the task (about 70). It reproduces all captured tasks of the four games (570 tasks of Donkey Kong 64, Ocarina of Time, Paper Mario, Pokemon Snap). It is slower than the JIT (about 3x on a Donkey
Kong 64 task on the x86 desktop; on the A55 about 80 ns per interpreted instruction against the JIT's 11 ns) and is not a production path. Earlier work on *hooks* inside the interpreter (native handlers taking over at a pc) and on the JIT running the
stretches between them was dropped in favour of whole-task native front-ends (see below); the hook code is not part of the tree.

**What verify compares** (`ARMLET_HLE_VERIFY=1` or `jit`: the oracle is the JIT; `=interp`: the oracle is the interpreter, and the JIT then runs the task for real; and the offline harness): the
native run in the sandbox, then the state is put back and the oracle runs the task; then native against oracle. Two modes, the same for every family (nothing is set per ucode):

- **External effects (the default; the measure is the result, not what happens in between).** All of DMEM except the ucode's declared dead ranges, IMEM, the SP / DPC registers, MI_INTR (without the DP
  interrupt bit, which the RDP raises when it processes a full sync), the pc, `SP_STATUS_TIMEOUT`, the RDRAM the task wrote by SP DMA (the bytes of every range either side wrote, except the RDP output
  ring: below), and **the bytes of the RDP lists as one stream** (FNV-1a-64 and length of all list bytes in order, parsed across list boundaries like foir's parser, with the second word of NO_OP and the sync
  commands zeroed: the RDP ignores it and the microcode leaves stale scratch there). Not compared: the order and number of the intermediate SP register writes, the DMA chunking, the poll counts, the IMEM
  dirty bits, list boundaries (unobservable to foir; coalescing already relies on it), the scalar and vector registers, the accumulator, flags and divide unit (`ARMLET_HLE_POISON=1` fills them with a pattern
  after a native task so that a wrong claim shows up in the equivalence runs), the status-poll counters (the JIT zeroes them at every task start).
- **Exact events (`ARMLET_HLE_EXACT=1`).** The above plus the sequence of recorded SP register writes (DMA reads and writes with the hash of the bytes, status, semaphore, DPC_START / END / STATUS) and the
  IMEM dirty bits (the oracle's must be a subset). A front-end that claims to do what the microcode does, DMA for DMA, can be held to it, and in this mode it has to be: a front-end checks `hle_exact()` and
  does every DMA and every IMEM load the microcode does (the F3DEX2 front-end does; its replay of the 582 captured tasks is equal in both modes on the desktop and on both devices).

**The RDP output ring is a mask in RDRAM.** The ucode's OSTask names the ring (`hle_ucode.ring_off`: the DMEM address of the two words, start and end; 0xfe8 on F3DEX2). The RDP lists are written there and
taken by the video plugin; nothing reads the ring afterwards, and what a native run leaves in it differs from the JIT's in the stale second words of the sync commands (the stream compare zeroes them) and in
whatever scratch a chunk starts or ends in. The compare skips the DMA chunks inside the ring (their bytes are compared through the stream); `ARMLET_DMEM_POISON=ring` overwrites the whole ring with a pseudo-random pattern after
every graphics task (the lists have been taken by then) as the proof, like the poison of a DMEM mask.

**DMEM masks.** A build may declare DMEM ranges dead at the end of a task (`hle_dead_range`: scratch the next task overwrites before reading, or never reads). A mask is data in the build's table, cites
how it was established, is applied identically by the verify compare, the poison probe and the `tools/equiv` hash (`armlet_hle_print_masks` feeds the shim), is validated by the equivalence runs
of all the target games on both devices, and never covers RDP lists, the output FIFO state, the DL stack or anything the RDP or a later task reads. **The evidence is per family**: the F3D family
leaves DMEM dead except the OSTask header (Fleet C's poison runs), F3DEX 1.x does not reload DMEM 0x800-0xfbf between tasks; each family's mask needs its own proof.
The probe is `ARMLET_DMEM_POISON=<lo>-<hi> | mask | all | ring` (a comma separated list of any of them; `hle_task.c`): at the end of every graphics task, whether the JIT or the task-level path ran it, those DMEM bytes are overwritten with a
pseudo-random pattern (a different one each time); `mask` is the build's dead ranges, `all` is [0, 0xfc0). A range is dead when a run with it poisoned gives the events, the RDP stream, the
pictures and the audio of the unpoisoned run (docs/testing.md).

**Capture and offline replay.** `ARMLET_HLE_CAPTURE=<dir>` (with `_EVERY`, `_MAX`) writes snapshot files of task starts (state, the RDRAM rows the task's DMA reads read, the JIT's end state,
the recorded events and the JIT's RDP stream) for tasks the JIT runs; `armlet-native-replay file.snap...` runs the family on them and compares with the recorded JIT result (the rows of RDRAM that the task's DMA reads read are copied when the DMA happens: the task's own flush can overwrite them; `HLE_PATCH=addr:hex;...` writes RDRAM before the run, for the synthetic tasks of `tools/hle/mksyn_ex2.py`, docs/testing.md)
(`--bench N` times the front-end and the JIT; `HLE_POKE=addr=val,...` sets DMEM bytes first). Snapshots are ROM-derived: keep them in `test-output/`.

**The F3DEX2 front-end** (`hle_f3dex2.c`, `f3dex2_fx.[ch]`, `f3dex2_fx_neon.c`, `f3dex2_dmem.h`; the microcode's behaviour is written down in [ucode-f3dex2.md](ucode-f3dex2.md)). `run()` lets the interpreter run the
microcode's boot code up to the first fetch of the command loop, takes over there with a C loop over the display list, and hands the end of the task (the outermost `G_ENDDL`) back to the interpreter, whose
exit code flushes the RDP buffer and ends the task. Anything the loop does not know (`G_LOAD_UCODE` of a text that is no registered build, `G_DMA_IO`, a yield, a vertex count over 32, an unknown overlay, more than 30 000 SP_STATUS polls, a state in
which the microcode would wait for the RDP) declines the whole task. DMEM stays the home of the microcode's state (the matrices, the lights, the vertex records, the geometry mode, the display list stack, the segment
table, the RDP output buffers), so the interpreter's exit code finds what it expects and the compare can check the state itself. The commands are plain C; the geometry is a set of functions over structs that
know nothing of DMEM addresses or pcs (`f3dex2_fx.h`, after Fleet C's `f3d_fx.h`): `fx2_mtx_mul`, `fx2_light_setup`, `fx2_vtx_xform` (n vertices: lighting, texture generation, fog, the clip flags with the pair
merging and the phantom partner of an odd count), `fx2_tri_setup` (the decisions and the RDP triangle command), `fx2_clip_tri` (the six-plane clipper with the new vertices completed by the vertex
pipeline); `f3dex2_dmem.h` fills the structs from DMEM and writes them back. Each has a scalar form (`_scalar`, plain C11, the reference, checked against the interpreter running the microcode on every captured
task and in the equivalence runs) and on aarch64 a NEON form with identical results (`tools/hle/test_fx2.c`: 400 000 random triangles, 40 000 vertex batches and 100 000 matrix products bit for bit, with deliberate
errors in the NEON forms, `-DFX2_NEON_MUTATE=n`, caught). `HLE_FX2_SCALAR=1` selects the scalar forms at run time (A/B timing on a device).

The **lean mode** (default; `ARMLET_HLE_EXACT=1` turns it off) does only the SP DMA and IMEM loads that the external effects need: the display list is read from RDRAM instead of through the 168-byte refills, the input
vertices from RDRAM instead of through a DMA to the slots (the 40-byte records written over them cover the range), the matrices, lights and viewport are copied, the RDP buffer is written to the ring with
`hle_rdram_write`, the lighting and clipping overlays are tracked and the one that the microcode would have left is loaded once at the end. The decisions that the microcode makes per chunk (where the ring wraps, whether the RDP
would have to catch up) are still made chunk by chunk, so DPC_START / END and the FIFO pointer end where the microcode's do. The ranges that this leaves different from the JIT's are the F3DEX2 table's
(`f3dex2_dead_out`, below).

**The F3DEX2 table of dead ranges** (cited where it is declared): the display list input buffer 0x920 .. 0x9c8 (the last refill), the clipper's polygon lists 0x3ca .. 0x408 and new vertices 0x9c8 .. 0xba8, the RDP output buffers
and their slack up to 0xfb8, the padding byte of each colour / direction of the light slots, and the word at 0xfc8 (OSTask.ucode_boot: the initialisation stores register a0 there, `sw a0, 0xfc8`, which holds what the
previous task left in it; found because the equivalence run of a native-run Ocarina of Time diverged in its RSP hash from the second graphics task on). The microcode reloads DMEM from the ucode data at the start of every task, so
the whole [0, 0xfc0) is dead across tasks (`ARMLET_DMEM_POISON=all` leaves the state stream, the pictures and the audio of the games identical); the table is deliberately smaller than that so that the compare keeps checking
the state a native front-end computes and the interpreter does not need to reproduce.

**Input no game produces: what the front-ends decline.** The lean mode and the closed forms model the microcode on the display lists, vertices and matrices a game writes. A perturbed task (`tools/rsp2c/fuzz_snaps.py`,
docs/testing.md, "Fuzzing the front-ends") reaches states the microcode handles in its own way, which the front-ends do not reproduce; each of these is detected where the command is handled (a compare or two) and the whole task
goes to the JIT. A mutation number (`MUT(n, ...)`, `scripts/build.sh native --tools --hmut=n`) switches each one off for the check:

| decline | why it cannot be modelled | n |
|---|---|---|
| a vertex number past the 32 slots or odd (`G_TRI1/2/4`, `G_QUAD`, `G_MODIFYVTX`, `G_CULLDL`, `G_BRANCH_Z`); a `G_VTX` whose end index is past 32 / odd / below 2n / has bits 20 .. 23 set; a `G_MODIFYVTX` field past the record | the table lookup reads the clipper's constants and stores go anywhere | 217, 218, 221, 222, 223 |
| `G_MOVEMEM` / `G_OBJ_MOVEMEM` to IMEM (bit 12), over the display list buffer, the jump table, the overlay descriptors, the segment table, the display list stack or the FIFO pointer; an index past the table; `G_MOVEWORD` with an index past the table or an offset that is not a word within its group (segments up to 0x3c, lights up to 0x1fc) | stores the front-end does not keep (the lean mode reads the list from RDRAM; the opcode classification is cached from the jump table) | 219, 231, 235, 237, 238 |
| `G_SELECT_DL` whose flags word (DMEM `0x2b0` + byte 0 of w0) lies over the jump table, the overlay descriptors, the input buffer, the segment table or the FIFO pointer (`f3_store_unmodelled`); a game's are the first few words | the microcode stores the word where it is told: over opcode 0's jump table entry it turns the no-op from the sync handler into the loop head, and the opcode classification the loop cached from the table is stale (the front-end then wrote a no-op word per zero command, the JIT nothing) | 315 |
| an opcode whose jump table entry is 0 (which is `G_DL`'s pc) or the sync handler and is not `G_DL` / a sync (Conker's 0x09 .. 0x0f) | undefined opcodes: the sync copy leaves a stale second word | 225, 226 |
| a geometry mode bit the build gives no meaning to; a texture on byte other than 0 / 2 or a tile byte past 6 bits; a guard-band ratio with bit 15; `G_MTX` with a length other than 64 bytes (F3D / F3DEX: shorter than a matrix) | the cull table is indexed by bits 8 .. 15, the command id takes the low byte, the ratio is a signed lane, a short DMA leaves the rest of the matrix as the geometry buffer was | 224, 230, 232, 233, 229, 239 |
| a class-0 operand longer than the buffer's dead range (F3D 0x150 bytes at SM64's layout, F3DEX 0x200) | the DMA runs on into live DMEM | 227, 236 |
| a triangle or `G_CULLDL` over the partner slot of an odd `G_VTX` (F3D, SM64 flag word) | the microcode's pair store writes the flags of a vertex it computed from stale input | 242 |
| a display list, vertices, matrix or light read from inside the RDP output ring; a matrix push into it (staged output) | the compare treats the bytes the RDP ignores (second words of syncs, slack) as don't care, a read gives them meaning | 243, 244 |
| S2DEX `G_BG_1CYC` with a format past 4 or a texel size past 3, `G_BG_COPY` with a texel size past 3 | the table lookups read other DMEM | 94 (1CYC; its roles of the two bytes were swapped before) |
| S2DEX `G_OBJ_LOADTXTR` with an object longer than its buffer or a tmem class byte that stores outside the two state words | the DMA / the store runs over the display list buffer and the jump table | 241, 247 |
| an S2DEX opcode of 0x54 .. 0x7f (undefined): its jump table entry lies in the display list buffer (the table is at 0x370, the buffer at 0x418) | the microcode reads the entry from the list's own bytes when the command comes; the front-end classified it at the build's start | 310 |
| a display list address that is not 8-aligned (`G_DL`, `G_BRANCH_Z`) | the refill DMA rounds down; the lean mode reads from the address | 240 |
| `G_BRANCH_Z` of F3DEX2 2.04 / 2.04H compares the screen z (it compared the clip w, as F3DZEX does) | a vertex whose clip w and screen z differ | 259 |
| `G_BRANCH_Z` of F3DEX 2.06 (`0bbe`) compares the screen z although the build has the F3DZEX triangle code (`bz_z`; derived from `zex` alone it compared the clip w) | a vertex whose clip w and screen z differ; the synthetic `misc` case found it (149 of 1000 seeds) | 317 |
| any task once the JIT's SP_STATUS poll timeout has dropped from 0x7fff to 16 (parallel-rsp does that for good after the first task that polled for the CPU; Gauntlet Legends' display list branches to itself until the CPU patches it) | the JIT then halts every task at its 16th poll of a register and resumes it in the next call: the end state of the call is part of a task, and the front-ends run whole tasks (`hle_core.c`, `native_run`; all families) | 316 |

Found by the same fuzzing and fixed (they are reachable by lists a game can write): the F3D triangle's `f3d_tri_in.sort_given` was never set (stack garbage decided whether the vertices were sorted by Indiana Jones' table: Wave Race and Pilotwings
captures differed from the JIT in 5 to 9 of 24 tasks, nondeterministically; mutation 228), and the build constants of two F3DEX2 builds were wrong: Ocarina of Time's and Pokemon Snap's reject mask is 0x70b0 (the w lane), not 0x7030 (a triangle or `G_CULLDL`
whose vertices share only bit 7 was drawn / not ended; 245), and F3DZEX's `G_BRANCH_Z` compares the integer part of the clip w (`lh 6(vertex)`), not the screen z (246).

**Build switching (`G_LOAD_UCODE`).** The build record of the F3DEX2 front-end (`struct f3dex2_build`: the layout, the pcs of the handlers, the extension table, the overlay descriptors, the dead ranges) is a property of the
*command loop*, not of the task: `f3_bind` (`hle_f3dex2.c`) sets it from the state of the microcode at a first command fetch, at the start of the task and after every `G_LOAD_UCODE`. What the microcode does (derived from its code; the
stub at IMEM 0 is overlay 0, the same code that ends a task): it flushes the RDP buffer, saves the position in the display list (OSTask.data_ptr, DMEM 0xff0) and the text address (0xfd0, which the new microcode adds to its overlay
descriptors), copies 3912 bytes of text to IMEM 0x80 and the data segment (the word of the `G_RDPHALF_1` before it, DMEM 0xd8, plus 0x180; the size is w0's low twelve bits) to DMEM 0x180 .., and starts the new text at 0x80, which
initialises itself up to its first fetch. The front-end hands the command to the interpreter at 0xfac (the lean mode first puts the command's two words and the last 168-byte chunk of the list in the input buffer: the output buffers of the loaded
microcode lie over it and the bytes behind its one-byte syncs are what it held), runs the loaded text's initialisation up to its first fetch (`head + 4` of the *new* build) and then calls `f3_bind` for the new build: the layout `f2L`, the opcode
classification read from the jump table the data load put in DMEM, the overlay IMEM holds (S2DEX's initial text image has its overlay A resident), the pc a `G_ENDDL` hands over at, and `uc0`, the address of the running text. What is carried across is
what that code does not write: DMEM below 0x180 (the F3DEX2 matrices and viewport, the RDP state words 0xc0 .. 0xd8, the FIFO pointer, the segment table, the display list stack and its depth, OSTask) and above the data, IMEM outside the text, the
SP / DPC registers, RDRAM, and the count of SP_STATUS polls (the JIT halts a task at 32767 of them whichever microcode polls: `polls` goes on, plus the new text's first fetch). The new build is found by the FNV-1a-64 of the 3912 bytes the command
copies (`f3dex2_build.load_key`; the ucode ids of the task starts are over `ucode_size` bytes and say nothing about a loaded text, which the DMA also copies only in part); the keys of the registered builds were taken from captured task
starts (`tools/hle/` `armlet-native-replay` prints one in `HLE_F3_TRACE` for a text it does not know). A build that is not registered, or not of this front-end, declines the task as before.
A task that switched builds is compared in two ways that differ from a plain one: the DMEM ranges that are dead are the *union* of the builds' (the table of the build a task starts with: the equivalence shim masks by it; Kirby 64's entry, the 2.04H
text it shares with Super Smash Bros., carries S2DEX's two output buffers), and the RDP stream is compared with the three scratch bytes behind a one-byte sync zeroed as well (`hle_stream.hash_r`, `hle_task.switched`): the loaded build's buffers lie over DMEM
that the build before it left in ranges it declares dead and does not keep exactly (the clipper's new vertices, the scratch of the triangle setup), and the RDP ignores those bytes.

**Environment.** `ARMLET_HLE_VERIFY` (`1`, `jit`, `interp`), `ARMLET_HLE_EXACT` (the exact-events compare and the DMA-for-DMA front-end), `ARMLET_HLE_INTERP`, `ARMLET_HLE_STATS` (counts, decline reasons, time), `ARMLET_HLE_POISON`,
`ARMLET_DMEM_POISON`, `ARMLET_HLE_ABORT`, `ARMLET_HLE_QUIET`, `HLE_FX2_SCALAR` (the scalar forms), `HLE_F3_PROF` (ticks per command kind, on stderr), `HLE_F3_TRACE` (every command and triangle), `HLE_F3_WATCH=<addr>` (a DMEM halfword,
after every command that changes it), `HLE_DUMP_IMEM=<file>`, `HLE_DUMP_STREAM=<file>` (the native side's RDP stream), `HLE_DMEM_ALL` (list every differing DMEM byte), `HLE_RSP_TRIPC=<pc>` (the interpreter prints the three vertex slots at a triangle entry), `HLE_RSP_DEBUG=<n>` (the first n interpreted instructions), `HLE_RSP_TRACE` (every interpreted instruction with the vector register it wrote), `HLE_RSP_WATCH=<addr>` (every change of
a DMEM byte, with the pc), `HLE_RSP_PROFILE` (executions per pc, `PROF pc count` lines on stderr at the end of every task).

**Dialects of the F3DEX2 front-end** (`hle_f3dex2_dialects.c`, `hle_s2dex2.c`, `hle_rej.c` (the Rej texts: extension commands for the vertex and triangle stage, `rej_fx.c` / `rej_dmem.h`, [ucode-rej.md](ucode-rej.md)), sharing `hle_f3dex2_int.h` with `hle_f3dex2.c`; the microcode semantics and the builds are in [ucode-f3dex2-dialects.md](ucode-f3dex2-dialects.md) and [ucode-s2dex2.md](ucode-s2dex2.md)). Builds
that share the F3DEX2 skeleton (the display list refill, the DL stack, the RDP command copies, the flush) but differ in details are table entries of the same front-end, parameterised instead of copied:

- **`struct f2_layout`** (`f3dex2_dmem.h`): the DMEM addresses the front-end uses, by name; the standard instance is what the four original builds share, a dialect that moved its state has one of its own, selected per task
  (`f2L`, set by `f3dex2_run` from `f3dex2_build.lay`). The code reads `F2_GEOM` etc. through it, so the standard path costs a pointer load, not a copy of the code. The output buffer size (`out_bytes`: 344, 800 for S2DEX 2.06 / 2.08, 792 for
  Ocarina of Time's S2DEX 2.05, F3DEXBG's 16) and `pipe_sync` (a flush entry that writes a `G_RDPPIPESYNC` in front of the buffer check: that S2DEX 2.05 [Safe]; `after_emit` in `hle_f3dex2_int.h` does it for every handler that emits) are fields of it.
- **`struct f2_pcs`** and **`ext` / `ext_cmd`** (`f3dex2_build`): handlers below 0x170 at other pcs (S2DEX), and dialect commands (kinds from `K_EXT`) run by a hook that sees the whole per-task state (`struct f3`).
  The hook does not test which build it runs: everything that differs between the S2DEX 2.08 / 2.06 / 2.05 texts is in the record, the handler table (`ext`: the 2.06 text's handlers from 0x184 on 8 bytes lower), the continuation pcs of
  `G_OBJ_LOADTXTR` (`ldtx_spr`, `ldtx_rect`: where the table at DMEM 0x202 sends LDTX_SPRITE / LDTX_RECT), the layout (792-byte buffers and `pipe_sync` of the 2.05 [Safe] text, the overlay descriptors). So a derived S2DEX record runs with
  the primary reference's hook (`dbuild_make`). The DMEM addresses of the S2DEX state (object buffers, render-mode tables, scissor fields) are the same in every 2.x text and stay constants of the front-end and the kernels: a text that moved
  one does not align with the references at that instruction and its derivation is not EXACT / NEAR.
  `G_TRI4` (F3DEXBG) is a command of the loop itself.
- **`G_LOAD_UCODE`** (handler 0xfac in every build) switches the build the loop works with, in the middle of the task: see [Build switching](#build-switching-g_load_ucode) below. The same text is a re-initialisation (Conker ends every task this way);
  the text of another registered build of this front-end is taken over (Kirby 64: F3DEX2 2.04H -> S2DEX 2.06 and back, in every task; Ocarina of Time: F3DZEX -> S2DEX 2.05 [Safe] and back, in every task that draws its
  background); an unregistered text declines the task (F-Zero X's F3DLX.Rej and Resident Evil 2's F3DEX.Rej were such until the Rej front-end, [ucode-rej.md](ucode-rej.md); Excitebike 64's F3DEX.NoN xbus 2.08 likewise). The front-end remembers the text / data pointers of the tasks that declined at such a command
  with the number of tasks that ran with them: when more than half of them declined, the next tasks with the same pair decline at once (every 64th runs again), else they run and the minority declines late (Resident Evil 2: 4267 of 6464
  graphics tasks of 3700 frames are native; with the rule "one decline marks the pair" it was 69, because the pair is the same for the tasks that load and those that do not; mutation 306).
- **`gen204`** (`f3dex2_build`): F3DEX2 2.04 / 2.04H, the generation before 2.05 (Buck Bumble, Cruis'n World, Super Smash Bros., Kirby 64): an unclamped screen z, another clipper reciprocal sign and fan, a slip in the linear texture generation, and `G_BRANCH_Z`
  that compares the screen z (the F3DZEX builds compare the integer part of the clip w). One flag, three fields of the geometry structs (`fx2_vctx.gen204`, `fx2_clip_ctx.sign0` / `fan_l0`); the triangle setup is the existing `zex` parameter set
  ([ucode-f3dex2-dialects.md](ucode-f3dex2-dialects.md) section 5).
- **Overlays.** The S2DEX 2.x code overlays take the slots of the F3DEX2 clip / light overlays (`OV_CLIP` = A, `OV_LIGHT` = B); the end-of-task fix-up of the resident overlay is the same.
- **Kernels.** Nothing in the production path is a transliteration of the microcode any more. The three routines that once were (S2DEX's sprite rectangle and background, F3DEXBG's lit vertices; `tools/rsp2c/rsp2c.py` made or drafted
  them, one statement per instruction on the exact vector-unit library `tools/hle/ref_vu.h`) are closed forms: S2DEX `G_OBJ_RECTANGLE` in `s2dex2_rect.c`, `G_BG_1CYC` and `G_BG_COPY` in `s2dex2_bg.c` (sections 3a, 3b and 3c of
  [ucode-s2dex2.md](ucode-s2dex2.md); shared DMEM access, saturation and constants in `s2dex2_int.h`) and the lighting stage of F3DEXBG in `f3dexbg_fx.[ch]` (a pass over the records of the standard vertex pipeline; DMEM adapters
  and the whole command in `f3dexbg_dmem.h`; [ucode-f3dex2-dialects.md](ucode-f3dex2-dialects.md) 2.4). All plain scalar C: the S2DEX ones work on two lanes (x and y) of a vector, and a NEON form that loads eight to use two
  does not pay (the rectangle is about sixty integer operations; on x86 the closed form takes 0.4 times the old instruction-level kernel's time, section 3b); the lighting is a few 32-bit chains per vertex and light, and the vertex
  pipeline under it is the NEON one. On x86 the front-end runs at 1.6 to 3.4 times the JIT's speed (Worms tasks) and 3.4 to 3.9 (Conker's lit tasks), so lit vertices are native by default. A closed form declines (nothing written, the task
  goes to the JIT) what its derivation does not cover: another set of vector constants, a scale of 0 or negative, a loop that would not end. `tools/rsp2c/rsp2c.py` stays as the generator of the references the closed forms are checked
  against ([testing.md](testing.md), "Regenerating the microcode oracles"); the generated files are not part of the tree.
- **Mutation numbers** (`MUT(n, ...)`, `scripts/build.sh native --tools --hmut=N`, `tools/hle/mutcheck_dialects.sh`): 30 to 39 and 44 the S2DEX front-end and the rectangle's tile/command part, 150 to 166 the rectangle closed form, 65 / 66 `G_TRI4` / `G_LOAD_UCODE`, 70 to 93 the background routine, 100 to 120 the F3DEXBG lighting, 40 to 60 the Rare front-end (`hle_rare.c`), 170 to 201 the Indiana Jones front-end (`hle_indy.c`), 210 to 216 the F3DEX2 2.04 / 2.04H differences and the two general fixes found with them (`f3dex2_fx.c`, `hle_f3dex2.c`; `tools/hle/mutcheck_204.sh`), 240 .. 259 the build switching and the declines found by fuzzing it (`hle_f3dex2.c`, `hle_s2dex2.c`), 260 to 299 `G_BG_COPY` (`s2dex2_bg.c`; `tools/hle/mutcheck_switch.sh`), 300 to 310 the S2DEX 2.05 [Safe] build, the load rule and a decline (`tools/hle/mutcheck_s206.sh`: 300 the sync byte, 301 its length, 302 the buffer size, 303 the sync flag of the layout, 304 the rectangle's continuation pc of `G_OBJ_LOADTXTR`, 306 the load rule, 308 / 309 a sync after a command that wrote nothing, 310 the undefined-opcode decline; 305 and 307 are free), 315 the `G_SELECT_DL` flags-word decline (`tools/hle/mutcheck_seldl.sh`; it replaced 311 and 312 when the two were merged; the branch `cmd-handoff` uses 300 to 314, so new numbers start at 316), 340 to 353 the probe on F3DEX2 records, 400 to 403 the probe on S2DEX records (`tools/hle/mutcheck_probe_s2.sh`: 400 the continuation pc of LDTX_RECT, 401 the output buffer size, 402 the pipe sync, 403 the 2.06 handler shift); the numbers are only live in the code of the family that runs, so a number may be used by two families (`tools/rare/mutcheck.sh` runs the Rare and Indiana Jones ones). `--gmut=N` is `F3D_FX_MUTATE` in the F3D struct-level functions (27 to 46 are the Rare and Indiana Jones paths).

## The probe and the ucode cache

Off by default (`GfxProbe = off`); it needs `GfxMode = hle`. When it is on, a graphics task of a ucode that has no table entry makes arMLEt **derive** a build record for it from its text and **probe** the derived build against the JIT, while the JIT keeps running the game's real tasks (the "ubershader"). A pass is remembered in a file. What the probe does *not* do is decide to run the build for real: that is the latch policy (a validator on real tasks, the next step of the plan); `ARMLET_PROBE_LATCH=1` latches a pass at once, for tests.

**The derivation in C** (`derive.c`, `derive_int.h`, `derive_refs.c`; the method is [ucode-derive.md](ucode-derive.md), the C port is described there). First sight of a (text id, data id) pair: the 4 KiB text, the 8 KiB of RDRAM behind it (the lighting / S2DEX overlays) and the data segment are aligned with the references and the record comes out, with a verdict EXACT / NEAR / NONE; 1.2 to 1.6 ms for the F3DEX2 family on the x86 desktop (all families: 3 to 5 ms). Records come out for the F3DEX2 family, S2DEX 2.x and F3DEX 1.x; F3D is classified only. The references are `derive_refs.c`, generated by `tools/derive_refs_gen.py` from the C tables and the dumps of the registered builds. **What it embeds:** no microcode text. Per instruction word of a reference a 16-bit index into a table of 64-bit one-way hashes of the instruction's *shape* (its word with the immediate, jump target or offset masked out; the whole word for instructions without such a field) and the 16-bit immediate; the build records; the jump table of each reference's data (handler pcs); the word indices of the handlers a reference has and a build may lack (`G_SPECIAL_1`, `G_TRI4`); the overlay geometry. A text can be *compared* with that, not recovered from it.

**The probe** (`probe.c`, `probe_cases.c`, `dbuild.c`; the pair run is `armlet_hle_probe_pair` in `hle_core.c`). A derived F3DEX2 record becomes a `struct hle_ucode` + `f3dex2_build` + `f2_layout` + `f2_pcs` (`dbuild_make`: the dead ranges, flags and ring of the *primary reference's* registered entry are copied, not derived; an S2DEX record's dialect commands run on the primary reference's `ext_cmd`, its table and continuation pcs are the record's); a derived F3DEX 1.x record a `struct hle_ucode` + `f3dex1_build` + `f3dex_layout` (a handler pc may be 0 where no opcode of the text's own tables leads to the reference's handler: the command declines). On the next graphics tasks of that pair (4 canned tasks per task; `ARMLET_PROBE_STEP`) one canned task is run per step:

1. The RSP state is saved (`prsp_native_save`: DMEM, IMEM, registers, vector unit, SP/DPC registers, MI_INTR, the status-poll bookkeeping, `dirty_blocks`).
2. The canned task's world is a **private RDRAM**: an 8 MiB anonymous mapping of the probe's own (pages nobody writes cost nothing). The ucode's text with the 8 KiB behind it and its data segment are copied into it from the game's RDRAM at the addresses the OSTask names; the RDP ring (128 KiB), the canned data (`probe_cases.c`, 0x1200 bytes) and the matrix stack go to clear addresses near the top. The game's RDRAM is read, never written (`ARMLET_PROBE_CHECK=1` compares all of it before and after every canned task). For the JIT's DMA both `RSP::cpu`'s rdram pointer and `RSP::rsp.RDRAM` (which the sandbox's undo uses) are pointed at it for the length of the task (`probe_rdram.cpp`; the fork is unchanged). *The first version put the data into the tail of the game's RDP output ring and the undo wrote zeros over the game's RDRAM: Donkey Kong 64's audio diverged. The private RDRAM is why that cannot happen.*
3. The OSTask in the saved DMEM is patched (display list, ring, matrix stack, no yield flag), the build's front-end runs the task in the sandbox (`native_run`), the state is put back, the **JIT** runs the same task from the same state, also in the sandbox (the RDP is not run, the writes are undone; the DPC registers are advanced as the video plugin does, `rdp_emul`), and the end states are compared as `ARMLET_HLE_VERIFY` does (DMEM except the build's dead ranges, IMEM, the SP/DPC registers, MI_INTR, the pc, the RDP list bytes as a stream, and the RDRAM written by DMA in the ring, the matrix stack and any other range the native run wrote).
4. State, registers and `SP_PC` are put back (`prsp_native_load`, `prsp_native_resync`): the game's task then runs on the JIT as it would have. A canned task is *inconclusive* (not counted either way) when the XBUS is set, the JIT's status poll time-out has dropped (Gauntlet Legends: `native_run` declines then, too) or the JIT does not end the task.

**The JIT's block cache.** The canned tasks run the game's own microcode text, so the blocks the JIT compiles for them are the blocks it compiles for the game's tasks (it keys them on the IMEM contents); the probe adds their compilation time (a few ms in all) earlier and, for overlay variants the game never loads, some blocks it would not have. `prsp_native_resync` drops the blocks and stubs that no longer match the restored IMEM. Evidence that nothing leaks: the state, hash, pictures and audio of the 3700-frame gates are identical with the probe on (testing.md).

**The cases** (`probe_cases.c`, ported from `tools/hle/mksyn_ex2.py` and seeded with a PRNG of its own): `clip zneg tri mix wild misc lit mtx` (three seeds each) and two targeted cases of two variants each: `rej` (triangles entirely beyond one clipping plane, and `G_CULLDL` over them, each in a list of its own: the reject mask decides whether the list ends, so a wrong mask changes the RDP stream whatever the seed; the second variant has a negative w) and `bz` (`G_BRANCH_Z` against thresholds between the clip w and the screen z: a wrong `bz_z` changes the branch). 28 tasks, about 5 ms on the desktop. An F3DEX 1.x build gets the same eight random kinds and `rej` / `bz` encoded in the F3DEX 1.x GBI (`probe_case_make_f1`): the commands whose opcodes differ between the
builds (RDPHALF, G_BRANCH_Z, G_MODIFYVTX, G_PERSPNORM, G_QUAD) take the opcode whose cell of the build's handler table leads to the record's pc, the others the GBI's numbers, G_CULLDL the record's form; its `rej`
puts a witness RDP command behind every cull, and a triangle of three second-of-pair vertices with only bit 0x40 in common (NoN builds put the sign of w there): the NoN mask (0x7030) and the full one tell
apart only there; `bz` is left out for a build without G_BRANCH_Z (28 or 26 tasks, 5 to 20 ms). An S2DEX 2.x build (a record with dialect commands) gets its own five cases instead, six seeds each (30 tasks, about 3.5 ms), `mksyn_ex2.py`'s `s2` stage split by command group:
`s2rect` (sprite rectangles, tame and wild, render-mode changes), `s2ldtx` (texture loads, an object loaded twice, and an `LDTX_RECT` in every case: a wrong continuation pc declines them all), `s2bg` (`G_BG_1CYC`, overlay B, with sprites between: the
overlay switches), `s2copy` (`G_BG_COPY`) and `s2misc` (`G_SELECT_DL` taken and not, `G_OBJ_MOVEMEM`, display list calls, `G_TEXRECT` + `G_RDPHALF_1 / _2`, other-mode, RDP copies, `G_SETSCISSOR`); each starts with segment 0, a scissor and a render mode.
The backgrounds write 300 to 1500 bytes, so the flush of the output buffer (its size, the [Safe] sync in front of it) is in every seed. **Pass:** no task differs; at least 60 % of the tasks were conclusive; at least 75 % of the conclusive ones completed natively (a build that declines the rest is not the build); every case completed natively at least once, `rej` and `bz` in every variant.

**Near-match tolerance.** None by default (`GfxProbeNear = strict`): a NEAR verdict is held to the same equality as an EXACT one. `tolerant` (`ARMLET_PROBE_NEAR=tolerant`) admits two things and nothing else, for NEAR derivations that differ in colour only: in the RDP stream, the **shade block** of shaded triangles (commands 0x0c .. 0x0f, bytes 32 .. 95) may differ by at most 2 colour levels in a channel's base value and 1/128 of a level per pixel step in a slope, the streams having the same length and every other byte equal (no command word, edge, texture or depth coefficient); in DMEM, the **colour word of a vertex record** (+0x10 .. +0x13 of the slots the build's vertex table names) by at most 2. A pass that needed the tolerance is `PASS (tolerant)`, cached as such. Geometry and wrong colours are never admitted. (Tested with mutations 350 .. 353: the perturbation inside the bounds passes tolerant and fails strict; outside, it fails both.)

**First sight, in order:** cache lookup (hit with a pass: the build is made from the cached record at once, no canned task; hit with a failure or NONE: nothing more) - derivation (verdict NONE, or F3D: logged, cached, no probe) - build (a record that cannot be made: unsupported, cached) - probe. Up to 16 pairs at a time. `ARMLET_PROBE_FORCE=1` probes the registered ucodes too, with their own record left out of the references (leave-one-out) and compares the derived record with the registered one; with `ARMLET_PROBE_LATCH=1` the derived build replaces the registered entry for the data image it was probed with (`armlet_probe_gate`). **Loaded texts.** A text that a `G_LOAD_UCODE` loads in the middle of a task (S2DEX in Kirby 64's and Ocarina of Time's tasks) is never a task's own ucode. The front-end reports every load of another text (`armlet_probe_note_load`: the text address and the
`G_RDPHALF_1` address, which is the image of DMEM 0, the command filling DMEM 0x180 .. from it); the next offer makes a pair of it keyed by its **load key** (the FNV of the 0xf48 bytes the command copies, the key `loaded_build` looks builds up by) and the data image, derives
it with that image, and runs its canned tasks as tasks that start with that text (OSTask.ucode / ucode_data pointed at them). Its canned tasks run on whatever graphics task comes. A latched loaded build is registered with the load key as its id and
`loaded_build` takes a latched build before the registered ones; it is not bound to the data image (`armlet_probe_gate` only sees task starts). Registered loaded texts are probed only with `ARMLET_PROBE_FORCE`.
`ARMLET_PROBE_NONE_X=n` probes a NONE derivation of the F3DEX2 family or F3DEX 1.x that has at most n unexplained plus missing words (an experiment, never cached).

**The cache** (`ucache.c`): one file, `armlet-ucode.cache` in the core's user config directory (`ConfigGetUserConfigPath`, so `--configdir` is respected; `$XDG_CONFIG_HOME/mupen64plus` or `~/.config/mupen64plus` when the core does not offer it), or the path of `GfxUcodeCache` / `ARMLET_UCODE_CACHE` (`off` for none). Entries are keyed by (text id, data id) and carry the tool version (the derivation's version, the file format and a checksum of the reference data: a new registered build or a changed derivation retires every entry), the derived record (format 3: F3DEX 1.x records; format 2: with an S2DEX record's dialect table), the verdict and the probe's counts and result. Each entry has a CRC-32 and a bad one is ignored; a truncated file keeps the entries before the cut; a damaged header makes the file empty; at most 512 entries (the oldest stored go first); writes go to a temporary file that is renamed over the cache, after merging in what another process wrote since the load.

## Native handler contract (superseded)

`armlet_gfx_try_native` still accepts a task only when `GfxMode = hle`; boot size <= 0x1000 (a task, not a bare RSP program); type 1; fresh (`SP_PC` 0 and the yield flag
clear); its ucode id has a handler (an `hle_ucode` entry, see above) and the id is in `GfxHleUcodes` when that list is not empty. The handler `run` returns non-zero when it completed the
task; returning 0 changes nothing and the JIT runs the task. What a completed task leaves is described in "Task-level native path"; the JIT stays the reference.
