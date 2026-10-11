# Testing

arMLEt must behave exactly like the pairing it replaces: **rsp-hle with `RspFallback` = parallel-rsp** (`DisplayListToGraphicsPlugin = False`,
`AudioListToAudioPlugin = False`). It is tested against that pairing, built from the same two submodule commits with the same toolchain, run
on the same emulator, video plugin and ROM, and compared on what is observable.

## What is compared

| gate | what | how |
|---|---|---|
| 1 | determinism baseline | the reference twice (`ref.1`, `ref.2`) must agree with itself on everything below; otherwise a difference between reference and arMLEt means nothing |
| 2 | RSP state | one line per `DoRspCycles` call: FNV hash of DMEM + IMEM and the return value (`RSPT_HASH` of `tools/equiv/rsp_shim.c`, the same stream as foir's `tools/prsp/rspt.c`); the streams must be identical over their common prefix |
| 3 | frames | foir's `FOIR_SHOT_DIR` / `FOIR_SHOT_FRAMES` screenshots at six fixed frames; the md5s must be identical |
| 4 | audio | every AI DMA buffer as the core hands it to the audio plugin (foir's `tools/prsp/adump.c`); bit-identical over the common prefix |
| 5 | module | `nm -D` shows exactly the six plugin API symbols; the rg35xxsp build needs at most glibc 2.17 |
| 6 | timing | RSP time per frame and emulation-thread CPU time per frame, ref against arMLEt, clocks pinned, builds alternated, 3+ repeats |

Every run uses `Core[RandomizeInterrupt] = False`, foir with its defaults (`FrameSkip = 0`, 3 worker threads), a fresh config and **no save file** (a
game that finds one boots differently), a private `HOME` / `XDG_*` / `--configdir`, and a scripted pad (`tools/equiv/inp_det.c`) that is keyed on
the number of controller polls, not on time: idle for 360 polls, START and A pulses until poll 1560, then the stick held up with A, B and Z taps.
A run ends after the screenshot of frame 3700 (62 s of emulated time at 60 Hz). The streams of two runs differ in length by a few calls
because the run is killed; they are compared over the common prefix.

## Harness (`tools/equiv/`)

| file | role |
|---|---|
| `build-tools.sh` | builds the test plugins (`rsp-shim.so`, `input-det.so`, `audio-adump.so`) for native / aarch64 / rg35xxsp |
| `rsp_shim.c` | RSP shim: forwards to `$RSPT_TARGET`; `RSPT_HASH`, `RSPT_LOG` (1 Hz RSP ms), `RSPT_MARK` (at the first RSP call after a screenshot file exists: RSP ms, emulation-thread and process CPU ms) |
| `inp_det.c` | the deterministic pad script |
| `device/runner.sh` | runs a list of `TAG|BUILD|ROM|KIND|LAST` lines; `ref`, `armlet`, `armlet-hle` (arMLEt with `ARMLET_GFX=hle`, exercising the ucode-id path); `e2e` or `time` runs |
| `device/rgdsplus.sh` | ROCKNIX flavour: Wayland env, private HOME, performance governor when `PIN` is set |
| `device/rg35xxsp_*.sh` | spruce flavour: hands over from PyUI, runs the standalone mupen64plus with spruce's libraries, restores `lastgame` and PyUI; `cpu_perf.sh` for the clocks |
| `host.sh` | one session under the device's lock: deploy (md5-checked), run, fetch to `test-output/equiv/<dev>/` (git-ignored), clean the device |
| `desk.sh` | the same runs on the desktop (x86_64, `SDL_VIDEODRIVER=offscreen`, memory-capped `systemd-run` scope) |
| `compare.py` | gates 1-4 per pair of runs, and the timing table |

```sh
scripts/build.sh rg35xxsp; scripts/build.sh ref rg35xxsp; tools/equiv/build-tools.sh rg35xxsp
EQUIV_X=<work dir on the device> tools/equiv/host.sh <ssh host> label 'sm64.ref.1|ref|Super Mario 64.z64|e2e|3700' 'sm64.armlet.1|armlet|Super Mario 64.z64|e2e|3700' ...
tools/equiv/compare.py test-output/equiv/<dev> sm64.ref.1 sm64.ref.2  sm64.ref.1 sm64.armlet.1
```

The rg35xxsp build (glibc <= 2.17 symbols) runs on both devices, so the same binaries were used on both. foir is the `devel` build at 3e58ce0.
foir's `tools/prsp/prsp_common.sh` also has an `armlet` mode for its own `prsp_*.sh` scripts.

**The harness catches errors.** Two deliberately wrong arMLEt builds, each compared against the reference on SM64 (desktop, 900 frames):
`DoRspCycles` returning `cycles + 1` differs from the first RSP call; skipping one graphics task in 50 differs from call 272 of 1207 (the six
screenshots were still identical, so the screenshot gate alone would not have caught it; the hash stream does).

## Results

Games and the ucode paths each exercised (from the `ucode ... ->` lines arMLEt logs, identical on both devices):

| game | audio / other ucode | graphics ucode |
|---|---|---|
| Super Mario 64 | audio ABI1 | F3D 2.0D |
| Ocarina of Time | nead (OoT), **JPEG** (`jpeg_decode_PS`), cic x105 | F3DEX2 (F3DZEX.NoN 2.06H + S2DEX) |
| Mario Kart 64 | nead (MK64) | F3DEX 0.95 |
| Banjo-Kazooie | naudio (BK) | F3DEX 1.21 |
| Donkey Kong 64 | naudio (DK64), cic x105 | F3DEX2 |
| Paper Mario | naudio | F3DEX2 2.08 |
| Star Fox 64 | nead (SF) | F3DEX.NoN 1.22 |
| Pokemon Snap | audio ABI1 | F3DEX2 2.08H |
| Hydro Thunder | **MusyX v1** | F3DEX2 2.08 |
| Indiana Jones and the Infernal Machine | **MusyX v2** | other (no banner) |

Hydro Thunder and Indiana Jones are not on the devices; their ROMs were copied to the device's private work directory for these runs and
removed afterwards. Handlers that no device run exercised: audio ABI1 GoldenEye / Blast Corps, naudio mp3 / Conker, nead F-Zero X / Yoshi's Story /
Majora's Mask, HVQM2 sp1 and the Resident Evil 2 video ucodes. A one-off desktop survey (all 62 ROMs of the two ROM sets, armlet with `GfxMode = hle`,
1200 frames each) reached those, but without comparing against the reference; their code is rsp-hle's, compiled the same way as in the reference.
Nothing reached nead Star Fox J / Wave Race J / 1080 / Majora's Mask beta / Animal Crossing / `mats` / `efz` or HVQM2 sp2.

### Gates 1-4, both devices

Ten games, 3700 frames (62 s emulated), per device: `ref.1`, `ref.2` and `armlet.1` for each, plus `armlet-hle.1` for SM64, OoT and BK.

| | rgdsplus (RK3566, A55) | rg35xxsp (H700, A53) |
|---|---|---|
| gate 1: `ref.1` vs `ref.2` (hash, 6 screenshots, audio) | 10 of 10 identical | 10 of 10 identical |
| gate 2: `ref.1` vs `armlet.1` RSP hash stream | 10 of 10 identical, 2,900-7,300 calls per game | 10 of 10 identical |
| gate 3: screenshots at frames 616, 1232, 1848, 2464, 3080, 3700 | 60 of 60 identical | 60 of 60 identical |
| gate 4: AI audio stream, 5-8 MB per game | 10 of 10 identical, none silent | 10 of 10 identical |
| `armlet` vs `armlet-hle` (ucode-id path on) | 3 of 3 identical | 3 of 3 identical |

The number of RSP calls in 3700 frames is the same on both devices for seven of the ten games and differs for MK64, BK and Indiana Jones
(presumably the two core builds); every comparison is within one device.

Desktop (x86_64, `NEON=0` foir, offscreen, 2 GB scope): SM64, OoT, Hydro Thunder and Indiana Jones, 1800 frames, `ref` twice, `armlet` and
`armlet-hle`: all identical on all three gates.

### Gate 5

`scripts/build.sh` prints the exports and the symbol-version floor of every build:

| target | exports | needs |
|---|---|---|
| native x86_64 | the six API functions | GLIBC_2.34, GLIBCXX_3.4.29 |
| aarch64 (Ubuntu 24.04 cross toolchain) | the six | GLIBC_2.38 (for rgdsplus' glibc 2.44; not run on a device, the rg35xxsp build was used on both) |
| rg35xxsp (`toolchains/rg35xxsp`, also via its `build.sh armlet`) | the six | **GLIBC_2.17**, GLIBCXX_3.4.29 (the device's libstdc++ is GCC 12's) |

### Gate 6: timing

`time` runs end at the first RSP call after the frame-1500 screenshot, so both builds have run exactly the same RSP calls (checked: the call
counts below agree) and the totals are directly comparable; they include the boot. "RSP" is the time inside `DoRspCycles` as the shim
measures it (audio HLE included), "emu thread" the CPU time of the emulation thread at that point, which also holds foir's list parsing.
Clocks pinned (rgdsplus: performance governor, which it reports as 1992 MHz and runs at about 1.58 GHz; rg35xxsp: `cpu_perf.sh on`, 1512 MHz);
six runs per game in the order ref, armlet, armlet, ref, ref, armlet. Pass: |delta RSP| <= max(0.03 ms, 2 sigma of the ref repeats) and the same for the emulation thread.

rgdsplus:

| game | frames | calls (ref / armlet) | RSP ms/frame ref | RSP ms/frame armlet | delta | limit | emu-thread ms/frame ref -> armlet | process ms/frame ref -> armlet | parity |
|---|---|---|---|---|---|---|---|---|---|
| bk | 1500 | 1203 / 1203 | 4.146 +- 0.004 (n=3) | 4.137 +- 0.024 (n=3) | -0.009 | 0.030 | 8.225 -> 8.238 (+0.012, limit 0.030) | 19.332 -> 19.308 | PASS |
| dk64 | 1500 | 1306 / 1306 | 3.710 +- 0.011 (n=3) | 3.697 +- 0.011 (n=3) | -0.013 | 0.030 | 8.330 -> 8.316 (-0.014, limit 0.038) | 17.876 -> 17.795 | PASS |
| mk64 | 1500 | 1985 / 1985 | 3.198 +- 0.007 (n=3) | 3.231 +- 0.026 (n=3) | +0.033 | 0.030 | 5.829 -> 5.836 (+0.007, limit 0.061) | 14.215 -> 14.346 | FAIL |
| oot | 1500 | 2037 / 2037 | 3.200 +- 0.019 (n=3) | 3.193 +- 0.007 (n=3) | -0.007 | 0.037 | 7.439 -> 7.425 (-0.014, limit 0.030) | 16.063 -> 16.035 | PASS |
| pm | 1500 | 2879 / 2879 | 1.847 +- 0.015 (n=3) | 1.834 +- 0.009 (n=3) | -0.013 | 0.030 | 5.354 -> 5.310 (-0.044, limit 0.067) | 10.733 -> 10.674 | PASS |
| sm64 | 1500 | 2055 / 2055 | 4.265 +- 0.034 (n=3) | 4.297 +- 0.033 (n=3) | +0.033 | 0.067 | 8.039 -> 8.061 (+0.022, limit 0.059) | 19.479 -> 19.538 | PASS |
| mk64 (2nd batch) | 1500 | 1985 / 1985 | 3.193 +- 0.008 (n=3) | 3.241 +- 0.020 (n=3) | +0.048 | 0.030 | 5.843 -> 5.856 (+0.014, limit 0.034) | 14.213 -> 14.365 | FAIL |

rg35xxsp:

| game | frames | calls (ref / armlet) | RSP ms/frame ref | RSP ms/frame armlet | delta | limit | emu-thread ms/frame ref -> armlet | process ms/frame ref -> armlet | parity |
|---|---|---|---|---|---|---|---|---|---|
| bk | 1500 | 1203 / 1203 | 4.478 +- 0.010 (n=3) | 4.505 +- 0.025 (n=3) | +0.026 | 0.030 | 9.036 -> 9.059 (+0.023, limit 0.030) | 21.677 -> 21.689 | PASS |
| dk64 | 1500 | 1306 / 1306 | 4.087 +- 0.021 (n=3) | 4.109 +- 0.026 (n=3) | +0.022 | 0.042 | 9.398 -> 9.413 (+0.015, limit 0.035) | 19.829 -> 19.868 | PASS |
| mk64 | 1500 | 1985 / 1985 | 3.648 +- 0.010 (n=3) | 3.690 +- 0.014 (n=3) | +0.042 | 0.030 | 6.392 -> 6.436 (+0.043, limit 0.030) | 16.161 -> 16.248 | FAIL |
| oot | 1500 | 2037 / 2037 | 3.617 +- 0.028 (n=3) | 3.626 +- 0.003 (n=3) | +0.009 | 0.055 | 8.368 -> 8.374 (+0.006, limit 0.060) | 18.026 -> 18.083 | PASS |
| pm | 1500 | 2879 / 2879 | 2.186 +- 0.017 (n=3) | 2.194 +- 0.009 (n=3) | +0.007 | 0.034 | 6.021 -> 6.040 (+0.019, limit 0.111) | 12.433 -> 12.462 | PASS |
| sm64 | 1500 | 2055 / 2055 | 4.623 +- 0.003 (n=3) | 4.641 +- 0.008 (n=3) | +0.018 | 0.030 | 8.756 -> 8.762 (+0.006, limit 0.030) | 21.448 -> 21.429 | PASS |
| mk64 (2nd batch) | 1500 | 1985 / 1985 | 3.664 +- 0.026 (n=3) | 3.693 +- 0.011 (n=3) | +0.029 | 0.053 | 6.405 -> 6.429 (+0.024, limit 0.030) | 16.181 -> 16.254 | PASS |

**Result: parity for five of six games on both devices; Mario Kart 64 is about 1 % slower in arMLEt on both.** Over two batches (6 + 6 runs) its RSP time is
+0.040 ms/frame (+1.3 %) on rgdsplus and +0.036 ms/frame (+1.0 %) on rg35xxsp, above the 0.03 ms limit in three of the four batches (the fourth, rg35xxsp
second batch, passes on its wider spread). Its emulation-thread time is +0.007 / +0.014 (rgdsplus) and +0.043 / +0.024 ms/frame (rg35xxsp) in the two
batches. The cause is not known and was not investigated; candidates, untested: code layout of the one-module build (the reference has rsp-hle and
parallel-rsp in two objects), or the nead audio HLE that MK64 runs every frame. On the A53 every delta is positive (+0.007 to +0.042 ms/frame, 0.2-1.3 %); on the A55 they
are -0.013 to +0.033 ms/frame.
