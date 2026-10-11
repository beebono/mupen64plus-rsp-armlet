# Options

## Config section `Rsp-Armlet`

Written with its defaults to `mupen64plus.cfg` the first time the plugin starts (when the core saves the configuration).

| Key | Default | Effect |
|---|---|---|
| `Version` | `1.00` | Parameter-set version; a section with another major version is reset to the defaults. |
| `GfxMode` | `lle` | `lle`: every graphics task runs on parallel-rsp's JIT, with no ucode identification (nothing extra is done per task). `hle`: each new graphics task's ucode is identified and logged; a ucode that has a native handler (none yet) would run natively, the rest run as `lle`. |
| `GfxHleUcodes` | empty | Comma-separated 64-bit ucode ids in hex, as logged by `GfxMode = hle`, that may run natively. Empty = every ucode that has a handler. At most 32 ids. |

There is nothing to set for the rest of the pairing: `hle_gfx` and `hle_aud` are always off, as `DisplayListToGraphicsPlugin = False` and
`AudioListToAudioPlugin = False` were for rsp-hle. The video plugin must be one that processes RDP lists from an LLE RSP (foir does).

## Environment

| Variable | Effect |
|---|---|
| `ARMLET_GFX=lle\|hle` | overrides `GfxMode` |
| `ARMLET_UCODES=<ids>` | overrides `GfxHleUcodes` |
| `ARMLET_LOGFILE=<path>` | test aid: messages up to INFO level are also appended to this file and flushed (the console output of a killed run loses its tail) |

## Messages (INFO)

- `arMLEt: ucode <uc_start> data <addr>/<size> -> <handler>`: once per new entry of rsp-hle's ucode cache, naming the HLE handler it got
  (`audio ABI1`, `naudio`, `nead (...)`, `MusyX v1/v2`, `JPEG`, `HVQM2`, ...), or "rsp-hle internal" for the tasks that end in
  `HleForwardTask` or in a plain task-done (graphics, and the nead variants `mats` / `efz` that rsp-hle forwards).
- `arMLEt: gfx ucode <id> <family>: <banner> [<handler state>]`: `GfxMode = hle` only, once per ucode id.

## Build options (`scripts/build.sh`, or CMake)

| Option | Effect |
|---|---|
| `M64P_API_INCLUDE_DIR` | directory with `m64p_types.h` (default `../mupen64plus-core/src/api`) |
| `ARMLET_PRSP_PROFILE` / `build.sh --prof` | parallel-rsp's dynamic instruction-mix profiling variant (`-DPRSP_PROFILE`) |
| `ARMLET_PRSP_SHADOW` / `build.sh --shadow` | parallel-rsp's shadow verification: every task is replayed on a second JIT with the previous code paths (`-DPRSP_SHADOW`) |

The variants build into separate directories (`out/<target>-prof`, `out/<target>-shadow`).
