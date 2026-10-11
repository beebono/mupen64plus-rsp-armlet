# arMLEt (mupen64plus-rsp-armlet)

arMLEt (ARM-focused Mid-Level Emulation) is a Mupen64Plus **RSP plugin** for aarch64 retro handhelds. It is rsp-hle and parallel-rsp in one
module, with a place to put native replacements for the slow parts of the graphics microcode:

- **Audio, JPEG, video and boot ucodes** run as HLE, with [mupen64plus-rsp-hle](https://github.com/mupen64plus/mupen64plus-rsp-hle)'s own code (compiled in).
- **Graphics tasks** (and any ucode rsp-hle does not know) run on [parallel-rsp](https://github.com/beebono/parallel-rsp)'s GNU Lightning JIT, as
  low-level emulation. They leave the RDP command stream in RDRAM and call the core's `ProcessRdpList` exactly as before, so a video plugin that
  wants LLE RSP output, such as [foir](../mupen64plus-rdp-foir), works unchanged.
- **A dispatch hook** in front of the JIT (`src/dispatch.c`, `src/gfx_ucode.c`) identifies the graphics ucode of every new task. This is where native
  handlers for a ucode will be plugged in; with no handler registered, nothing differs from the JIT path.

Today arMLEt's output is bit-identical to `mupen64plus-rsp-hle` with `RspFallback = parallel-rsp`
(`DisplayListToGraphicsPlugin = False`, `AudioListToAudioPlugin = False`): the same RSP state after every task, the same frames, the same audio
samples. What it adds is one module instead of two, one initialisation, a ucode id and family log, and the hook. Method and results:
[docs/testing.md](docs/testing.md). Design: [docs/design.md](docs/design.md). Options: [docs/options.md](docs/options.md).

## Using it

```sh
mupen64plus --gfx mupen64plus-rdp-foir.so --rsp mupen64plus-rsp-armlet.so --audio mupen64plus-audio-sdl.so game.z64
```

No other setting is needed. The config section `Rsp-Armlet` has `GfxMode` (`lle`, the default, or `hle`) and `GfxHleUcodes`; with `hle` a ucode
that has a native handler would run natively, and none has yet.

## Building

```sh
git clone --recurse-submodules <url>      # or: git submodule update --init   (not recursive: the fork needs nothing deeper)
scripts/build.sh native                   # -> out/native/mupen64plus-rsp-armlet.so
scripts/build.sh aarch64                  # aarch64-linux-gnu-* cross toolchain (the output needs the build host's glibc)
scripts/build.sh rg35xxsp                 # glibc 2.31 sysroot, host GCC 13 / g++-12 (runs on glibc >= 2.17 devices)
```

CMake and Ninja; the build runs on a copy of the sources (`work/`). The Mupen64Plus API headers are taken from `../mupen64plus-core/src/api`
(`M64P_API_INCLUDE_DIR`). The module exports only the six plugin API functions. `-O3`, generic armv8-a, no LTO. `--prof` builds parallel-rsp's
instruction-mix profiling variant, `--shadow` its shadow-verification variant. `scripts/build.sh ref <target>` builds the reference pairing
that the equivalence tests compare against.

## Layout

| path | content |
|---|---|
| `src/` | the plugin: API entry (`plugin.c`), config (`config.c`), rsp-hle callbacks and the graphics hand-off (`hle_glue.c`), dispatch and ucode identification |
| `ext/` | submodules: `parallel-rsp` (fork, `m64p-aarch64`), `mupen64plus-rsp-hle` (upstream, pinned) |
| `cmake/` | source lists and the drift check against the submodules |
| `scripts/` | `build.sh`, `check-commit.sh` |
| `tools/equiv/` | the equivalence harness (test plugins, device scripts, comparison) |
| `docs/` | design, testing, options |

Licence: GPL-2.0-or-later for arMLEt's own code; the binary is effectively GPL-3.0-or-later, see [NOTICE.md](NOTICE.md).
