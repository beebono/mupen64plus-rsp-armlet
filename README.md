# arMLEt (mupen64plus-rsp-armlet)

arMLEt (ARM-focused Mid-Level Emulation) is a Mupen64Plus **RSP plugin** for aarch64 retro handhelds. It is rsp-hle and parallel-rsp in one
module, with a place to put native replacements for the slow parts of the graphics microcode:

- **Audio, JPEG, video and boot ucodes** run as HLE, with [mupen64plus-rsp-hle](https://github.com/mupen64plus/mupen64plus-rsp-hle)'s own code (compiled in).
- **Graphics tasks** (and any ucode rsp-hle does not know) run on [parallel-rsp](https://github.com/beebono/parallel-rsp)'s GNU Lightning JIT, as
  low-level emulation. They leave the RDP command stream in RDRAM and call the core's `ProcessRdpList` exactly as before, so a video plugin that
  wants LLE RSP output, such as [foir](../mupen64plus-rdp-foir), works unchanged.
- **Exact coalescing of the video plugin's `ProcessRdpList` calls** (`src/rdp_defer.c`, [design](docs/design.md#rdp-list-coalescing)): a graphics ucode calls it hundreds of
  times a frame, mostly for one command; arMLEt hands foir the same bytes in fewer, larger lists, with every register and interrupt the ucode and the core can see unchanged.
- **Native routines** ([design](docs/design.md#native-routines)): a PC-entry hook in the JIT (`GfxNative = on|verify`, off by default) hands a microcode routine, a vertex transform or a
  triangle setup, to C / NEON code that must leave the RSP exactly as the microcode would. In `verify` mode the JIT runs the same stretch from the same state and everything is compared;
  `armlet-native-replay` does the same offline from captured snapshots.
- **Native graphics** ([design](docs/design.md#task-level-native-path)): a graphics task whose ucode has a task-level front-end runs as C / NEON instead of on the JIT.
  Data-driven front-ends cover F3D, F3DEX 1.x, F3DEX2 (with its dialects, incl. the Rej / LX2 builds) and S2DEX 2.x, plus hand-written ones for
  Diddy Kong Racing / Jet Force Gemini and Indiana Jones. Anything a front-end cannot prove exact declines to the JIT, task by task.
- **Probe and latch** ([design](docs/design.md#the-probe-and-the-ucode-cache)): a ucode with no table entry is derived from its own text, probed against the JIT
  with canned display lists, then validated on real tasks by a background thread; it runs natively only once it has latched, and a mismatch takes it back to the JIT.
  What it learned is kept in `armlet-ucode.cache` in the config directory.

The output is bit-identical to `mupen64plus-rsp-hle` with `RspFallback = parallel-rsp`
(`DisplayListToGraphicsPlugin = False`, `AudioListToAudioPlugin = False`): the same RSP state after every task, the same frames, the same audio
samples (the native paths are held to this by the equivalence gates). Coverage: [docs/ucode-coverage.md](docs/ucode-coverage.md). Method and results:
[docs/testing.md](docs/testing.md). Design: [docs/design.md](docs/design.md). Options: [docs/options.md](docs/options.md).

## Using it

```sh
mupen64plus --gfx mupen64plus-rdp-foir.so --rsp mupen64plus-rsp-armlet.so --audio mupen64plus-audio-sdl.so game.z64
```

No other setting is needed. The config section `Rsp-Armlet` has `GfxMode` (`hle`, the default: native graphics where a front-end exists; `lle`:
everything on the JIT), `GfxHleUcodes` (limit native graphics to some ucode ids) and `GfxProbe` (`on`, the default; `off` disables the probe and latch).
Native graphics needs foir as the video plugin, behind the coalescing path. `RdpCoalesce` (`auto`: on with foir) and `RdpCoalesceBytes` control the list coalescing ([docs/options.md](docs/options.md)).

## Building

```sh
git clone --recurse-submodules <url>      # or: git submodule update --init   (not recursive: the fork needs nothing deeper)
scripts/build.sh native                   # -> out/native/mupen64plus-rsp-armlet.so
scripts/build.sh aarch64                  # aarch64-linux-gnu-* cross toolchain (the output needs the build host's glibc)
scripts/build.sh rg35xxsp                 # glibc 2.31 sysroot, host GCC 13 / g++-12 (runs on glibc >= 2.17 devices)
```

CMake and Ninja; the build runs on a copy of the sources (`work/`). The Mupen64Plus API headers are taken from `../mupen64plus-core/src/api`
(`M64P_API_INCLUDE_DIR`). The module exports only the six plugin API functions. `-O3`, generic armv8-a, no LTO. `--prof` and `--prof3` build
the profiler (`src/prof`, [docs/profiling.md](docs/profiling.md)): levels 1-2, and also the per-instruction counts of level 3; `--shadow`
builds parallel-rsp's shadow-verification variant; `--tools` also builds the native-routine harness. The profiler is not in the normal module (it is a different `.so`). `scripts/build.sh ref <target>`
builds the reference pairing that the equivalence tests compare against.

## Layout

| path | content |
|---|---|
| `src/` | the plugin: API entry (`plugin.c`), config (`config.c`), rsp-hle callbacks and the graphics hand-off (`hle_glue.c`), dispatch and ucode identification, the RDP list coalescing (`rdp_defer.c`) |
| `src/native/` | native microcode routines: the interface (`native.h`), the framework (`native.c`, registry, verify, statistics, capture), the routines themselves (one file per family) |
| `ext/` | submodules: `parallel-rsp` (fork, `m64p-aarch64`), `mupen64plus-rsp-hle` (upstream, pinned) |
| `cmake/` | source lists and the drift check against the submodules |
| `src/prof/` | the profiler: per-task statistics, the JIT address map, per-site counts (`ARMLET_PROFILE` builds only) |
| `scripts/` | `build.sh`, `check-commit.sh` |
| `tools/equiv/` | the equivalence harness (test plugins, device scripts, comparison) |
| `tools/session/` | the profiling sessions (runner, device and desktop drivers) and the analysis scripts (timing tables, cost model, perf mapping, native-cost model) |
| `tools/native/` | the offline harness `replay.c` (snapshots of routine entries, verify, benchmarks), `disasm.py`, `mutcheck.sh` |
| `tools/rsp2c/` | `rsp2c.py` (translates a microcode handler into C on the exact vector-unit library: the oracles of the closed forms), `fuzz_snaps.py` and `replay_each.sh` (perturbed task snapshots) |
| `tools/rare/` | the Diddy Kong Racing / Jet Force Gemini / Indiana Jones tests: synthetic task generators (`synth.py`, `synth_indy.py`), `mutcheck.sh`, `stats.py` ([docs/native-custom.md](docs/native-custom.md)) |
| `tools/ucodemap/` | `ucodemap.py`: attributes the dynamic instructions of a graphics ucode to routines; `rspdis`, its decoder |
| `docs/` | design, testing, options, profiling |

Licence: GPL-2.0-or-later for arMLEt's own code; the binary is effectively GPL-3.0-or-later, see [NOTICE.md](NOTICE.md).
