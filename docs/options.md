# Options

## Config section `Rsp-Armlet`

Written with its defaults to `mupen64plus.cfg` the first time the plugin starts (when the core saves the configuration).

| Key | Default | Effect |
|---|---|---|
| `Version` | `1.00` | Parameter-set version; a section with another major version is reset to the defaults. |
| `GfxMode` | `hle` | `hle` (default since 2026-10-17): each new graphics task's ucode is identified and logged; a ucode that has a task-level handler ([design.md](design.md#task-level-native-path): F3D, F3DEX 1.x, F3DEX2 and its dialects incl. Rej, S2DEX 2.x, Rare, Indiana Jones) runs natively, the rest on parallel-rsp's JIT. `lle`: every graphics task runs on the JIT, with no ucode identification (nothing extra is done per task). |
| `GfxHleUcodes` | empty | Comma-separated 64-bit ucode ids in hex, as logged by `GfxMode = hle`, that may run natively. Empty = every ucode that has a handler. At most 32 ids. The id is the FNV-1a-64 of the whole 4 KiB of ucode text; **ids logged before 2026-10-16 were taken over `ucode_size` bytes and differ from today's for ten builds** (Wipeout 64 `19bd8c825b9b3f72` -> `76c763bc18ad3ab3`, Jet Force Gemini `410cfbcf87378a9c` -> `c2aca403f282af1b`, Buck Bumble / Cruis'n World `5dce3244420ce715` -> `6635562b2dde31ae`, Diddy Kong Racing `72a1416f6d8445c3` -> `909f82e8f8da6456`, Tetrisphere `88fa3c0aa5315997` -> `0b574ecb06d75978`, Worms Armageddon's S2DEX `b228360be43c0447` -> `fa171b4489d45ef2`, Conker `e80e52925f70b674` -> `e48f5dc9bed2fe3d`, and three texts that were registered under two ids each: Dr. Mario 64 / RR64 `ac310503c83b23ea` -> `d2d1f9f2e7dfe1ab` (WinBack's), Pokemon Puzzle League `1de5ed1ca5f278ab` -> `047252e810e35d56`, Quest 64 `9ac3b68bf66e164f` -> `208a567e9aead5ea`); an old id in `GfxHleUcodes` no longer matches. |
| `GfxNative` | `off` | Native microcode routines ([design.md](design.md#native-routines)). `off`: nothing is installed in the JIT, the module behaves as before. `on`: the routines of the task's ucode that the allow-list admits run natively where the JIT reaches their entry. `verify`: each call also runs the JIT's code from the same state and compares everything ([design.md](design.md#native-routines)); the JIT's result is the one that stands. |
| `GfxNativeRoutines` | empty | Allow-list: comma-separated routine names (`f3dex2.vtx`) or families (`f3dex2`, the part before the first dot); `-name` excludes; empty = every routine. Routines named `test.*` run only when listed by name. |
| `GfxProbe` | `on` | `on` (default since 2026-10-17; `off` to disable): a graphics ucode with no table entry is derived and probed ([design.md](design.md#the-probe-and-the-ucode-cache)); needs `GfxMode = hle`. A build whose probe passed is validated on real tasks by a background thread and runs natively once it latches ([design.md](design.md#the-background-validator-and-the-latch-policy)). `loose`: `on`, and the loose tier ([design.md](design.md#the-loose-tier)): a NONE derivation with at most `GfxProbeLooseWords` unexplained + missing words is probed too (as strictly) and, latched, keeps running through differences inside `GfxProbeLooseShade` in shade coefficients and vertex colours (counted, never a strike); geometry differences strike as for an EXACT build, a task that ends otherwise unlatches for good at once. A NONE derivation beyond the bound is tried too with the family's record as it was derived (the default-record arm, `default=3` evidence; `ARMLET_PROBE_DEFAULT=0` disables it). Its cache entries live in a section of their own. |
| `GfxProbeNear` | `strict` | `tolerant`: the probe of a NEAR derivation admits a shade-coefficient / vertex-colour difference of two levels (design.md). |
| `GfxProbeLooseWords` | `16` | `GfxProbe = loose`: unexplained plus missing words a NONE derivation may have and still be admitted to the loose tier (0 to 4096). |
| `GfxProbeLooseShade` | `8` | `GfxProbe = loose`: colour levels a latched loose build's shade coefficients (and `levels / 256` per pixel step in a slope) and vertex colours may differ from the JIT's by and count as a numeric difference (2 to 255). |
| `GfxUcodeCache` | empty | path of the probe's cache file (also the latch state: a build latched in an earlier session runs natively at once, one unlatched for good is never tried again); empty: `armlet-ucode.cache` in the core's config directory; `off`: none. |
| `GfxLatchPolicy` | empty | the latch policy's numbers, `key=value` separated by commas; empty = all defaults: `evidence=24` (evidence an EXACT build needs; a validated task adds 1 / (1 + earlier matches of its fingerprint)), `distinct=8` (distinct task fingerprints it needs), `near=1.75` (a NEAR build needs this times both), `strikes=2` (an EXACT build's mismatch that makes this many strikes, this session and earlier ones, unlatches it for good; the first suspends it), `sample=32` (a latched build: one task in 32 is still validated ...), `sample_last=256` (... halving the rate every `decay` matches down to one in 256), `decay=64`, `queue=4` (capture slots: tasks waiting for the validator at most), `fingerprints=1024` (fingerprints remembered per build), `loose=2.5` (a loose-tier build needs this times both), `default=3` (a loose-tier default-record build), `repeat=0.05` (the least evidence a repeat of a known fingerprint adds; `1 / (1 + n)` above it), `override=4` (this many times the base `evidence`, whatever the tier, latches with 2 distinct fingerprints or more, whatever the distinct bar; 0 = never). An unknown key is reported (WARNING) and the rest read. |
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
| `ARMLET_HLE_EXACT=1` | `GfxMode=hle`: the exact-events mode: the front-end does every SP DMA and IMEM load the microcode does, and verify also compares the sequence of recorded SP register writes and the IMEM dirty bits (default: the lean mode, external effects only; [design.md](design.md#task-level-native-path)) |
| `HLE_FX2_SCALAR=1` | aarch64: the F3DEX2 front-end uses the scalar forms of its geometry functions instead of the NEON ones (A/B timing) |
| `ARMLET_HLE_INTERP=1` | `GfxMode=hle`: run the tasks on the interpreter instead of a family's native front-end |
| `PRSP_POLL_IDLE=N` | the JIT (parallel-rsp, also the `ref` pairing): cut a task at N polls of SP_STATUS in a row with the scalar registers unchanged (a wait for the CPU) instead of lowering the poll time-out from 0x7fff to 16 for good after the first time-out, which slices every later task at its 16th poll. Default 16; 0 restores the original policy. Gauntlet Legends: with `GfxMode=hle` the F3DEX2 front-end cuts its tasks where the JIT does (a `G_DL` to itself) and takes up the calls that continue them, so the whole game runs natively ([design.md](design.md), "Waits for the CPU"); with the option at 0 a task that waits declines as before. Changes the JIT's call boundaries (the RSP hash stream) of a game that waits for the CPU; no other game |
| `ARMLET_HLE_STATS=1` | the task-level path: tasks, native runs and time, declines and their reasons, verify counts (every 64 graphics tasks) |
| `ARMLET_UCODE_STATS=<file>` | `GfxMode=hle`: per-ucode counters of the graphics tasks (tasks, native runs, declines with their reasons, JIT time, tasks of ucodes without a handler; the video plugin's callback time left out), rewritten every 128 graphics tasks and at `RomClosed`: the input of `tools/ucode_coverage.py` ([ucode-coverage.md](ucode-coverage.md)) |
| `ARMLET_HLE_POISON=1` | after a task-level run, fill the scalar and vector registers, accumulator and flags with a pattern |
| `ARMLET_HLE_ABORT=1`, `ARMLET_HLE_QUIET=1` | verify: `abort()` at the first mismatch; or do not log the mismatches |
| `ARMLET_HLE_CAPTURE=<dir>` | write snapshot files of the graphics task starts the JIT runs (`hle.task-<ucode>-<n>.snap`) for `armlet-native-replay`; `ARMLET_HLE_CAPTURE_EVERY=n` (16), `_MAX=n` (16). ROM-derived: keep in `test-output/` |
| `ARMLET_DMEM_POISON=<lo>-<hi>\|mask\|all\|ring[,...]` | at the end of every graphics task (whoever ran it) overwrite those DMEM bytes with a pseudo-random pattern: a probe for dead-DMEM ranges; `mask` = the dead ranges of the ucode's table, `all` = `[0, 0xfc0)`, `ring` = the RDP output ring in RDRAM (the bounds from the OSTask, `hle_ucode.ring_off`) |
| `ARMLET_PROBE=off\|on\|loose` | overrides `GfxProbe` |
| `ARMLET_PROBE_NEAR=strict\|tolerant`, `ARMLET_UCODE_CACHE=<path>\|off` | override `GfxProbeNear`, `GfxUcodeCache` |
| `ARMLET_PROBE_LOOSE_WORDS=n`, `ARMLET_PROBE_LOOSE_SHADE=n` | override `GfxProbeLooseWords`, `GfxProbeLooseShade` |
| `ARMLET_PROBE_DEFAULT=0` | `GfxProbe = loose`: turns the default-record arm off (a NONE derivation beyond the word bound is then not probed; on by default with `loose`) |
| `ARMLET_PROBE_LOOSE_FORCE=1\|clip,light` | testing (`GfxProbe = loose`): every derived pair is treated as a loose-tier one whatever its verdict (the loose latch path on a text that derives EXACT; with `ARMLET_PROBE_FORCE`, a registered one); a region list also pretends those regions have unexplained words (the region gate: clipped / lit tasks stay on the JIT) |
| `ARMLET_LATCH_POLICY=<key=value,...>` | overrides `GfxLatchPolicy` |
| `ARMLET_PROBE_LATCH=1` | testing (the latch policy's override): a pair whose probe passed is latched at once, every command kind, never sampled, and no validator thread runs. The default path is the background validator ([design.md](design.md#the-background-validator-and-the-latch-policy)) |
| `ARMLET_PROBE_FORCE=1` | testing: probe the registered ucodes too (leave-one-out derivation, the derived record is compared with the registered one) and treat them as unknown: a forced ucode's tasks run on the JIT until its derived build latches, then on the derived build (for the data image it was derived for), and a G_LOAD_UCODE of a forced text whose derived build has not latched declines, as for an unknown text. `ARMLET_PROBE_FORCE_CACHE=1` lets forced pairs use the cache (the second-launch test) |
| `ARMLET_LATCH_TRACE=1` | one line per validated task (equal / differs / declined / inconclusive / without the build, its fingerprint, the replay's CPU time) |
| `ARMLET_VALID_DUMP=<dir>` | development: a captured task whose replay differed or declined is written as a task snapshot (`armlet-native-replay` reads it), at most 64 |
| `ARMLET_PROBE_SEEDS=n` (3), `ARMLET_PROBE_STEP=n` (4; 0: derive only), `ARMLET_PROBE_TRACE=1` (one line per canned task), `ARMLET_PROBE_CHECK=1` (compare the game's whole RDRAM before and after every canned task: an error is logged if it changed), `ARMLET_PROBE_NONE_X=n` (probe a NONE derivation with at most n unexplained + missing words; never cached), `ARMLET_PROBE_FORCE_SELF=1` (with `ARMLET_PROBE_FORCE`: derive a registered ucode with its own record among the references, so the probe and the latch test the build made of its registered record) | |
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
- `arMLEt: loose tier: a NONE derivation with at most N unexplained + missing words is probed; ...`: `GfxProbe = loose`, once. `arMLEt: probe: ucode <id> data <id>: the derivation says NONE (...): admitted to the loose tier` / `... not admitted to the loose tier`; `arMLEt: latch: ucode <id> data <id>: a real task differs within the loose tolerance (<n> so far): kept` (INFO); WARNING `... a real task differs (a hard failure: the task ended otherwise) (strike n): UNLATCHED for good`.
- `arMLEt: latch policy: evidence ..., sampling 1/8 .. 1/256 ...`: `GfxProbe = on`, once. `arMLEt: latch: ucode <id> data <id> "<build>": a candidate: validating real tasks` (or `latched in an earlier session`), `... LATCHED after <n> equal tasks (evidence <e>, <d> distinct) in <s> s, <t> graphics tasks of the pair`; WARNING `... a real task differs (strike <n>): suspended, validating again` / `UNLATCHED for good`, with the first differences; `arMLEt: latch: masks <id>:<lo-hi>,...` (the build's dead DMEM ranges, in the form of `tools/equiv`'s `EXTRA_MASKS`). `arMLEt: validator: ...`: every 256 captures and replays, and at the end of the ROM: tasks armed, captured, not captured for want of a slot, dropped, replayed by outcome, the capture time on the emulation thread, the replays' and the thread's CPU time, the capture slots' memory.

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
