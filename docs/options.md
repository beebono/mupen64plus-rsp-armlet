# Options

## Config section `Rsp-Armlet`

Written with its defaults to `mupen64plus.cfg` the first time the plugin starts (when the core saves the configuration).

| Key | Default | Effect |
|---|---|---|
| `Version` | `1.00` | Parameter-set version; a section with another major version is reset to the defaults. |
| `GfxMode` | `lle` | `lle`: every graphics task runs on parallel-rsp's JIT, with no ucode identification (nothing extra is done per task). `hle`: each new graphics task's ucode is identified and logged; a ucode that has a task-level handler ([design.md](design.md#task-level-native-path)) runs through it (F3DEX2 today: on the interpreter, which is slower than the JIT; a native front-end replaces it later), the rest run as `lle`. |
| `GfxHleUcodes` | empty | Comma-separated 64-bit ucode ids in hex, as logged by `GfxMode = hle`, that may run natively. Empty = every ucode that has a handler. At most 32 ids. |
| `GfxNative` | `off` | Native microcode routines ([design.md](design.md#native-routines)). `off`: nothing is installed in the JIT, the module behaves as before. `on`: the routines of the task's ucode that the allow-list admits run natively where the JIT reaches their entry. `verify`: each call also runs the JIT's code from the same state and compares everything ([design.md](design.md#native-routines)); the JIT's result is the one that stands. |
| `GfxNativeRoutines` | empty | Allow-list: comma-separated routine names (`f3dex2.vtx`) or families (`f3dex2`, the part before the first dot); `-name` excludes; empty = every routine. Routines named `test.*` run only when listed by name. |
| `RdpCoalesce` | `auto` | Exact coalescing of the video plugin's `ProcessRdpList` calls (see [design.md](design.md#rdp-list-coalescing)). `auto`: on when the video plugin is foir (the module behind the core's `ProcessRdpList` callback has "foir" in its file name), `on`: always, `off`: never. Only tasks of a known graphics ucode family (F3D / F3DEX / F3DEX2 and relatives) are affected; XBUS lists and every other task are passed through untouched. |
| `RdpCoalesceBytes` | `4096` | Bytes of pending RDP command list at which the coalesced list is handed to the video plugin (8 to 1048576). |

There is nothing to set for the rest of the pairing: `hle_gfx` and `hle_aud` are always off, as `DisplayListToGraphicsPlugin = False` and
`AudioListToAudioPlugin = False` were for rsp-hle. The video plugin must be one that processes RDP lists from an LLE RSP (foir does).

## Environment

| Variable | Effect |
|---|---|
| `ARMLET_GFX=lle\|hle` | overrides `GfxMode` |
| `ARMLET_UCODES=<ids>` | overrides `GfxHleUcodes` |
| `ARMLET_NATIVE=off\|on\|verify` | overrides `GfxNative` |
| `ARMLET_NATIVE_ROUTINES=<list>` | overrides `GfxNativeRoutines` |
| `ARMLET_NATIVE_ABORT=1` | verify: `abort()` at the first mismatch (after the report) |
| `ARMLET_NATIVE_STATS=1` | per routine, every 128 graphics tasks: calls, declines, units (vertices / triangles) and the native time per call and per unit; in verify also the JIT's time for the same stretch |
| `ARMLET_NATIVE_WATCHDOG_MS=n` | verify: the process ends (exit 3, message `WATCHDOG`) when the JIT run of a routine has not arrived at the routine's exit pc after n ms (default 3000): a wrong exit pc lets the JIT run on, and it cannot be stopped otherwise |
| `ARMLET_NATIVE_PROBE=<list>` | poison, after an oracle-only or native routine, state that the rest of the run might read, to find out whether it does: `sr=<mask>`, `vr=<mask>`, `vl<n>=<lane mask>`, `acc`, `vco`, `vcc`, `vce`, `fl<0..2>=<flag bit mask>`, `div` (comma-separated); see design.md |
| `ARMLET_NATIVE_POISON=1` | `on` mode: after a routine, fill the registers it declares dead with a pattern (shows a wrong live-out claim downstream) |
| `ARMLET_NATIVE_CAPTURE=<dir>` | verify: write routine-entry snapshot files (`<routine>-ok-<call>.snap`, `-mismatch-`) for the offline harness; `ARMLET_NATIVE_CAPTURE_EVERY=n` (default 16) one entry in n, `ARMLET_NATIVE_CAPTURE_MAX=n` (default 32) per routine. ROM-derived: keep in `test-output/` |
| `ARMLET_HLE_VERIFY=1\|jit\|interp` | `GfxMode=hle`: each task-level run is also run by the oracle (the JIT, or the interpreter with `interp`) and the end states, events and RDP streams compared ([design.md](design.md#task-level-native-path)); the JIT's result stands. Mismatches go to the log |
| `ARMLET_HLE_EXACT=1` | `GfxMode=hle`: the exact-events mode: verify also compares the sequence of recorded SP register writes and the IMEM dirty bits, and a front-end that supports it does every SP DMA and IMEM load the microcode does (default: external effects only; [design.md](design.md#task-level-native-path)) |
| `ARMLET_HLE_INTERP=1` | `GfxMode=hle`: run the tasks on the interpreter instead of a family's native front-end |
| `ARMLET_HLE_STATS=1` | the task-level path: tasks, native runs and time, declines and their reasons, verify counts (every 64 graphics tasks) |
| `ARMLET_HLE_POISON=1` | after a task-level run, fill the scalar and vector registers, accumulator and flags with a pattern |
| `ARMLET_HLE_ABORT=1`, `ARMLET_HLE_QUIET=1` | verify: `abort()` at the first mismatch; or do not log the mismatches |
| `ARMLET_HLE_CAPTURE=<dir>` | write snapshot files of the graphics task starts the JIT runs (`hle.task-<ucode>-<n>.snap`) for `armlet-native-replay`; `ARMLET_HLE_CAPTURE_EVERY=n` (16), `_MAX=n` (16). ROM-derived: keep in `test-output/` |
| `ARMLET_DMEM_POISON=<lo>-<hi>\|mask\|all\|ring[,...]` | at the end of every graphics task (whoever ran it) overwrite those DMEM bytes with a pseudo-random pattern: a probe for dead-DMEM ranges; `mask` = the dead ranges of the ucode's table, `all` = `[0, 0xfc0)`, `ring` = the RDP output ring in RDRAM (the bounds from the OSTask, `hle_ucode.ring_off`) |
| `ARMLET_RDP_COALESCE=auto\|on\|off` | overrides `RdpCoalesce` (`0`/`1` also work) |
| `ARMLET_RDP_COALESCE_BYTES=<n>` | overrides `RdpCoalesceBytes` |
| `ARMLET_RDP_STATS=1` | diagnostic: every 256 graphics tasks the coalescing counters (callbacks, deferred, direct, calls to the video plugin and why each flush happened) go to the log |
| `ARMLET_LOGFILE=<path>` | test aid: messages up to INFO level are also appended to this file and flushed (the console output of a killed run loses its tail) |

## Messages (INFO)

- `arMLEt: ucode <uc_start> data <addr>/<size> -> <handler>`: once per new entry of rsp-hle's ucode cache, naming the HLE handler it got
  (`audio ABI1`, `naudio`, `nead (...)`, `MusyX v1/v2`, `JPEG`, `HVQM2`, ...), or "rsp-hle internal" for the tasks that end in
  `HleForwardTask` or in a plain task-done (graphics, and the nead variants `mats` / `efz` that rsp-hle forwards).
- `arMLEt: gfx ucode <id> <family>: <banner> [<handler state>]`: `GfxMode = hle` only, once per ucode id.
- `arMLEt: native routines on|verify (N known ...)`, `arMLEt: gfx ucode <id>: N native routines selected: <names>`: `GfxNative` set, once; once per ucode.
- `arMLEt: native <routine>: IMEM [lo,hi) at the entry hashes to H, the routine expects H'; not used` (INFO, once per hash): the routine's ucode id matched but the code differs.
- `arMLEt: native <routine> MISMATCH (state|exit|declined but changed the state|bad exit pc) ...` (ERROR): verify found a difference, with the first ones listed.
- `arMLEt: RDP list coalescing on|off (RdpCoalesce <setting>, threshold <n> bytes)`: once at start.

## Build options (`scripts/build.sh`, or CMake)

| Option | Effect |
|---|---|
| `M64P_API_INCLUDE_DIR` | directory with `m64p_types.h` (default `../mupen64plus-core/src/api`) |
| `ARMLET_PROFILE` / `build.sh --prof` | the profiler, levels 1 and 2 (`-DARMLET_PROFILE`; parallel-rsp is built with its `PRSP_HOOKS` observation hooks) |
| `ARMLET_PRSP_PROFILE` (with `ARMLET_PROFILE`) / `build.sh --prof3` | also parallel-rsp's per-instruction hook (`-DPRSP_PROFILE`) for the level-3 counts; 4-10x slower, counts only |
| `ARMLET_RDP_DEFER` (default `ON`) | the RDP-list coalescing: the module defines the fork's `prsp_rdp_defer.h` hooks (`-DPRSP_RDP_DEFER` for parallel-rsp, `-DARMLET_RDP_DEFER` for arMLEt). `OFF` builds a module without it (and without the hooks in parallel-rsp) |
| `ARMLET_NATIVE` (default `ON`) | the native-routine framework (`src/native/*.c`, the fork's `prsp_native.h` hook, `-DPRSP_NATIVE`); inert unless `GfxNative` is set. `OFF` builds the module without it |
| `ARMLET_NATIVE_TOOLS` / `build.sh --tools` | also builds `armlet-native-replay`, the offline harness (`out/<target>-tools/`) |
| `ARMLET_NATIVE_MUTATE=n` / `build.sh --nmut=n` | test builds only: a deliberate error `n` (1-8) in the `test.dma` routine, to show that verify catches it ([testing.md](testing.md)) |
| `ARMLET_RDP_MUTATE=n` / `build.sh --mutate=n` | test builds only: coalescing with safeguard `n` (1-5) dropped, to show that the equivalence gates catch it ([testing.md](testing.md)) |
| `ARMLET_PRSP_SHADOW` / `build.sh --shadow` | parallel-rsp's shadow verification: every task is replayed on a second JIT with the previous code paths (`-DPRSP_SHADOW`) |

The variants build into separate directories (`out/<target>-prof`, `out/<target>-prof3`, `out/<target>-shadow`). The normal module is byte-identical
whether or not the profiler's sources exist: nothing of it is compiled in without `ARMLET_PROFILE`.

## Profiler environment (`-prof` / `-prof3` builds only, see [profiling.md](profiling.md))

| Variable | Effect |
|---|---|
| `ARMLET_PROF=1\|2\|3` | 1: per-task statistics; 2: also the JIT address map; 3: also per-site counts (`--prof3` build). Unset or 0: off |
| `ARMLET_PROF_OUT=<dir>` | output directory (required) |
| `ARMLET_PROF_MARKS=<file>[,<file>...]` | write a snapshot of every counter when a file appears (the screenshots of foir's `FOIR_SHOT_FRAMES`) |
| `ARMLET_PROF_WALK=1` | run a read-only display-list walker at the start of every graphics task (needed by level 3) |
| `ARMLET_PROF_TASKLOG=1` | one line per `DoRspCycles` call (the input of the cost model) |
| `ARMLET_PROF_PARSE=0` | do not parse the RDP lists the ucode submits |
| `ARMLET_PROF_CB=0` | do not wrap the core's `ProcessRdpList` / `CheckInterrupts` callbacks |
| `ARMLET_PROF_CBSAMPLE=N` | time the `ProcessRdpList` calls that hold a full sync and a random 1 in N of the others, scaled (default 8; 1 = every call, which the task log (`ARMLET_PROF_TASKLOG=1`) implies) |
| `ARMLET_PROF_CBHIST=1` | histogram the callback time by size of the list, whether it holds a triangle, and duration (a little more overhead per call) |
