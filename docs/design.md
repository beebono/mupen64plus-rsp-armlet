# Design

arMLEt is a Mupen64Plus RSP plugin that puts rsp-hle and parallel-rsp in one module and leaves a hook where native graphics handlers will go.
This page describes what is there; [testing.md](testing.md) shows that it behaves like the two-module pairing it replaces.

## Why

On the aarch64 handhelds a part of every frame goes to the RSP running the graphics microcode (F3D, F3DEX, F3DEX2 families) in parallel-rsp's
Lightning JIT, ahead of the RDP plugin ([foir](../../mupen64plus-rdp-foir)). Replacing that with native code (vertex transform and lighting,
clipping, triangle setup written for NEON) would be much cheaper, but a native version of a ucode has to produce exactly the RDP command
stream and RSP state the LLE run produces, or be correct enough to accept. That needs a safe place to plug it in, with the JIT as fallback and
as the bit-exact reference to compare against. arMLEt is that place. Nothing native exists yet; the plugin as it stands is a drop-in for
`mupen64plus-rsp-hle` + `RspFallback = parallel-rsp`.

## Flow of one RSP task

```
core: DoRspCycles
  plugin.c            hle_execute(&ctx.hle)                        rsp-hle's task detection (cached per ucode address)
    audio ABI1/2/3, nead, MusyX, JPEG, video, HVQM, cic x105        run as HLE inside rsp-hle's own code, set TASKDONE|BROKE|HALT
    graphics task (type 1), any unknown ucode, nead "mats"/"efz"    rsp-hle calls HleForwardTask()          <- hle_glue.c
      dispatch.c        armlet_gfx_try_native(ctx)                   GfxMode=hle only; false today
      hle_glue.c        parallelRSPDoRspCycles(~0u)                  parallel-rsp's JIT runs the ucode
                        (cp0.cpp: SP DMA, the core's ProcessRdpList callback, interrupts)
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

## Native handler contract

`armlet_gfx_try_native` accepts a task only when all of these hold: `GfxMode = hle`; boot size <= 0x1000 (a task, not a bare RSP program);
type 1; fresh (`SP_PC` 0 and the yield flag clear); its ucode id has a handler in `l_native[]` (`gfx_ucode.c`); and the id is in `GfxHleUcodes`
when that list is not empty. The table is empty today. A handler `int run(struct armlet_ctx *c, const struct armlet_ucode_id *u)` that returns
non-zero has completed the task and must leave the machine as the LLE run would:

1. write the RDP commands to RDRAM where the ucode would (the output buffer or the FIFO) and set `DPC_START`, `DPC_CURRENT`, `DPC_END`;
2. call `ProcessRdpList` (the callback in `c->rsp`) for them, in the order and with the batching the ucode uses;
3. set `SP_STATUS` `TASKDONE | BROKE | HALT`;
4. raise `MI_INTR_SP` when `INTR_ON_BREAK` is set and `MI_INTR_DP`, then call `CheckInterrupts`;
5. leave DMEM / IMEM and the RSP registers either as the ucode would, or in a state the next task cannot observe (the next task reloads
   both; only a yield, which the dispatcher already refuses, or a game that reads them back, could tell).

A handler returning 0 must not have changed anything; the task then runs on the JIT. The JIT stays the reference: any handler is judged by
the `FOIR_SHOT` and RDP-stream comparison against it, per game, before it is enabled by default.
