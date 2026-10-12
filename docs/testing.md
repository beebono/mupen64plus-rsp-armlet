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
| `rsp_shim.c` | RSP shim: forwards to `$RSPT_TARGET`; `RSPT_HASH`, `RSPT_LOG` (1 Hz RSP ms), `RSPT_MARK` (a comma list of screenshot files; at the first RSP call after each exists: RSP ms, emulation-thread and process CPU ms, one MARK line per file) |
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

## RDP list coalescing

The coalescing of `ProcessRdpList` calls ([design.md](design.md#rdp-list-coalescing)) must change nothing but speed. It was tested like this; `RdpCoalesce` ON is the default (`auto` with foir),
OFF is `ARMLET_RDP_COALESCE=off` (build `armlet-off` in the device runner). foir `devel` at 88d1722, the rg35xxsp-toolchain builds on both devices.

**Equivalence gates, devices** (3700 frames, 6 screenshots, 3 worker threads; `off.1` and `off.2` agree with each other in every game, so the reference is deterministic; the `on` run's armlet log says "coalescing on"). In addition to the
three gates of the table at the top, a fourth stream, `RSPT_STATE` (`rsp.state`), has one hash per `DoRspCycles` call of `SP_PC`, `SP_STATUS`, `SP_SEMAPHORE`, `MI_INTR` and `DPC_START` / `END` / `CURRENT` / `STATUS`
as the core sees them when the call returns (that is where a late DP interrupt would show).

| device | games | hash stream | state stream | screenshots | audio |
|---|---|---|---|---|---|
| rgdsplus | Banjo-Kazooie, DK64, GoldenEye, Glover, Mystical Ninja, Mario Kart 64, Mischief Makers, Ocarina of Time, Paper Mario, Star Fox 64, SM64, Pokemon Snap | 12 of 12 identical (3,386-7,738 calls) | 12 of 12 identical | 72 of 72 identical | 12 of 12 identical (5.4-11 MB) |
| rg35xxsp | Banjo-Kazooie, DK64, Mario Kart 64, Ocarina of Time, Paper Mario, Star Fox 64, SM64, Pokemon Snap, Toy Story 2 | 9 of 9 identical | 9 of 9 identical | 54 of 54 identical | 9 of 9 identical |

(Mischief Makers uses XBUS for every list, so nothing is coalesced there; it is in the set to cover the pass-through path.)

**Desktop survey, all 63 ROMs of the two sets** (x86_64, foir `NEON=0` with `FOIR_THREADS=0`, 1500 frames, offscreen): OFF against ON, with the per-call state stream and a hash of **all of RDRAM** after every `DoRspCycles`
(`RSPT_RDRAM=1`: foir's output, framebuffer and Z included), hash stream, six screenshots and audio, at the default threshold (4 KiB), at 1 KiB (an earlier default) and at the maximum of 1 MiB (the range
then outlives many FIFO wraps and `DPC_START` writes, which exercises the triggers): **60 of 63 identical on every stream, at all three thresholds.**
Three games, Blast Corps, Quake and Gauntlet Legends, have a baseline that is not deterministic in RDRAM on a loaded machine (OFF against OFF differs at the same calls, by as much as OFF against ON; hash stream, screenshots and
audio are identical in all pairs); with RDRAM left out of the state stream (`RSPT_RDRAM=0`) OFF, OFF again and ON are identical for the three at 4 KiB and at 1 MiB. Six ROMs are not touched by the module at all (all their lists are passed through):
Diddy Kong Racing, Jet Force Gemini, Indiana Jones and Worms Armageddon (no graphics-ucode banner), Mischief Makers and Excitebike 64 (XBUS); Glover and Wipeout 64 have a few XBUS lists. Callbacks per call of the
video plugin at 4 KiB: 3 in the F3DEX2 games (Donkey Kong 64, Ocarina of Time, Paper Mario, Snap, which write 1 KB lists), 15-39 in Banjo-Kazooie, Mario Kart 64, Super Mario 64, GoldenEye, Star Fox 64, Mystical Ninja, up to 172-218 in Iggy's Reckin' Balls and Penny Racers. In the survey no flush was ever caused by
an SP DMA write (`dma 0`), by an XBUS list arriving with something pending, by a `DPC_STATUS` write except in Pokemon Puzzle League (14), Space Station Silicon Valley (234) and Yoshi's Story (1949), or by the safety path (`other 0`, `task-end 0`).

**Model-based test** (`tools/equiv/rdp_defer_fuzz.sh`): `rdp_defer.c` itself, driven through the fork's hooks by random streams of ucode events (chunks DMAed to a ring with `DPC_END`, ring wraps through `DPC_START`,
DMA writes over the ring and elsewhere, DMEM writes, `DPC_STATUS` toggling XBUS, `DPC_END` backwards or unchanged, task boundaries; thresholds 8 bytes to 1 MiB) against a model of foir's `rdp_process_list` that runs every list at once.
After every event the four registers must be equal and at the end of every task the stream of words the video plugin received must be identical. The module: 0 differences in 900 streams (23,463 tasks), and in 1000 streams of a second run.

**Mutation check.** `scripts/build.sh native --mutate=N` builds the module with one safeguard dropped. Against SM64, Banjo-Kazooie and Ocarina of Time (desktop, 1200 frames, RDRAM stream on) and against the model test:

| mutation | caught by the gates (hash, state, shots, audio) | caught by the model test (of 300 streams) |
|---|---|---|
| 1: no flush when the task ends | yes: SM64 hash stream differs from call 123, 5 of 6 screenshots differ, audio differs; Banjo-Kazooie from call 49; Ocarina of Time: 6 of 6 screenshots and audio (the hash stream does not see it) | 237 |
| 5: `DPC_START` / `DPC_CURRENT` not advanced when a list is deferred | yes, as a hang: the emulator never finishes the run (no RSP hash stream at all in Banjo-Kazooie and SM64, 3 of 6 screenshots then a stall in Ocarina of Time, state stream differing from call 2) | 300 |
| 2: no flush before a DMA write that overlaps the pending range | **no**: not even at a 1 MiB threshold, because in these games the ring never wraps over a pending range (every wrap goes through `DPC_START`, which flushes) | 82 |
| 3: no flush at `DPC_START` / `DPC_STATUS` writes | **no**, same reason: the old range is still intact when the late flush happens, and the deferral's own safety path (`DPC_CURRENT` is not the end of the pending range) flushes at the next `DPC_END` | 220 |
| 4: XBUS lists are deferred | **no**: Mischief Makers (every list XBUS) is identical even at 1 MiB, its DMEM ring is not reused before a `DPC_START` | 171 |

So mutations 1 and 5 are caught by the equivalence gates; 2, 3 and 4 are safeguards that no game among the four the mutants were run on needs, and only the model test (which makes the ucode do exactly what they guard against) catches them.

**Fork default build unchanged.** `ext/parallel-rsp` on branch `rdp-defer` against its parent commit 9972158, both built with the rg35xxsp toolchain and the fork's own CMake (the `ref` configuration, `PARALLEL_RSP_M64P_PLUGIN=ON`, no `PRSP_RDP_DEFER`): the `mupen64plus-rsp-parallel.so`
md5 is identical (8263ac13...) and so is every object file (`find -name '*.o'`, md5 each).

### Timing, ON against OFF

Sessions of the method of the profiler (`tools/session/`, windows in VI frames as in [profiling.md](profiling.md): Banjo-Kazooie, SM64, DK64 and Ocarina of Time 1440-2160, Mario Kart 64 race 3000-3700, Pokemon Snap zoom shot with no input 1100-1340),
clocks pinned (rgdsplus performance, rg35xxsp `cpu_perf.sh on`, restored afterwards), three runs of each, alternating `off`, `on` in a session (Mario Kart 64: `ref`, `off`, `on` in a rotating order over three sessions), threshold 4 KiB, foir with three workers.
"RSP" is the time in `DoRspCycles` (the shim; it holds the video plugin's callback, as in all earlier tables), "emu thread" its CPU time, per VI frame. Callbacks per frame from the profiler build, one run each
(`ARMLET_PROF=1`; its count is calls to foir's `ProcessRdpList`: without coalescing every `DPC_END` write, with it the calls that remain). rgdsplus:

| scene | callbacks/frame OFF -> ON | RSP ms/frame OFF | RSP ms/frame ON | delta | emu thread ms/frame OFF | emu thread ms/frame ON | delta |
|---|---|---|---|---|---|---|---|
| Banjo-Kazooie | 1210 -> 14.0 | 6.989 +- 0.040 | 6.417 +- 0.047 | -0.572 (-8.2 %) | 11.482 +- 0.038 | 10.876 +- 0.022 | -0.606 (-5.3 %) |
| Super Mario 64 | 573 -> 6.6 | 4.306 +- 0.020 | 4.057 +- 0.015 | -0.248 (-5.8 %) | 6.978 +- 0.024 | 6.760 +- 0.012 | -0.218 (-3.1 %) |
| Mario Kart 64 race | 1400 -> 12.6 | 5.523 +- 0.070 | 5.126 +- 0.019 | -0.398 (-7.2 %) | 8.331 +- 0.053 | 7.914 +- 0.015 | -0.417 (-5.0 %) |
| Donkey Kong 64 | 192 -> 16.7 | 6.722 +- 0.018 | 6.604 +- 0.026 | -0.118 (-1.8 %) | 11.901 +- 0.069 | 11.801 +- 0.039 | -0.100 (-0.8 %) |
| Ocarina of Time | 91 -> 8.4 | 2.572 +- 0.019 | 2.491 +- 0.019 | -0.081 (-3.2 %) | 4.957 +- 0.008 | 4.823 +- 0.019 | -0.135 (-2.7 %) |
| Pokemon Snap (zoom) | 392 -> 37.2 | 27.446 +- 0.043 | 27.353 +- 0.024 | -0.093 (-0.3 %) | 29.445 +- 0.080 | 29.420 +- 0.141 | -0.026 (-0.1 %) |

rg35xxsp:

| scene | callbacks/frame OFF -> ON | RSP ms/frame OFF | RSP ms/frame ON | delta | emu thread ms/frame OFF | emu thread ms/frame ON | delta |
|---|---|---|---|---|---|---|---|
| Banjo-Kazooie | 1210 -> 14.0 | 7.163 +- 0.069 | 6.949 +- 0.014 | -0.214 (-3.0 %) | 12.164 +- 0.048 | 11.935 +- 0.017 | -0.229 (-1.9 %) |
| Super Mario 64 | 573 -> 6.6 | 4.687 +- 0.034 | 4.584 +- 0.036 | -0.104 (-2.2 %) | 7.794 +- 0.030 | 7.691 +- 0.036 | -0.103 (-1.3 %) |
| Mario Kart 64 race | 1400 -> 12.6 | 5.922 +- 0.013 | 5.740 +- 0.039 | -0.182 (-3.1 %) | 9.113 +- 0.008 | 8.919 +- 0.037 | -0.194 (-2.1 %) |
| Donkey Kong 64 | 192 -> 16.7 | 7.037 +- 0.059 | 7.019 +- 0.049 | -0.018 (-0.3 %) | 12.883 +- 0.045 | 12.857 +- 0.018 | -0.026 (-0.2 %) |
| Ocarina of Time | 91 -> 8.4 | 3.008 +- 0.049 | 3.005 +- 0.063 | -0.004 (-0.1 %) | 5.600 +- 0.047 | 5.624 +- 0.071 | +0.024 (+0.4 %) |
| Pokemon Snap (zoom) | 392 -> 37.2 | 27.770 +- 0.068 | 27.843 +- 0.020 | +0.072 (+0.3 %) | 31.768 +- 0.100 | 31.939 +- 0.076 | +0.171 (+0.5 %) |

The saving is as modelled on the RK3566 for the games with many tiny lists (0.25-0.6 ms a frame) and about half that on the H700. Where the lists are already about 1 KB (the F3DEX2 games) there is little to merge, and in the Snap zoom shot the time is the video plugin's wait for its
workers: no change beyond the noise (the H700's +0.07 / +0.17 ms is 1-2 sigma and not significant). The process CPU time (all threads) falls 0.1-2.7 ms per frame (Banjo-Kazooie 29.71 -> 27.02 ms on the RK3566), presumably because the video plugin's workers are woken less often (not investigated).

Frame time, the Snap zoom shot, foir with `-DFOIR_FRAMELOG` (swap to swap, three runs each, windows as above; lists per frame is foir's own count of `ProcessRDPList` calls):

| device | lists/frame OFF -> ON | p50 OFF / ON ms | p95 OFF / ON ms | p99 OFF / ON ms | frames > 33.3 ms OFF / ON |
|---|---|---|---|---|---|
| rgdsplus | 392 -> 37 | 31.17 / 31.11 | 35.33 / 35.90 | 39.63 / 38.75 | 10.7 % / 13.0 % |
| rg35xxsp | 392 -> 37 | 32.42 / 32.47 | 36.51 / 36.68 | 40.02 / 40.82 | 20.2 % / 20.5 % |

No difference beyond run-to-run spread (p50 +-0.1-0.2 ms, p95 +-0.3-0.8 ms; the RK3566 share over 33.3 ms moves 2.3 points on a spread of 1-1.4). Banjo-Kazooie is vsync-bound (p50 16.6 ms on the RK3566 and 16.8 ms on the H700 in both), tails within the spread (p99 38-42 ms, load hitches in both).

**Threshold.** Banjo-Kazooie window, two runs each per device, RSP ms/frame: rgdsplus off 6.945, 256 B 6.696, 1 KiB 6.483, 4 KiB 6.368, 16 KiB 6.497; rg35xxsp off 7.170, 256 B 6.971, 1 KiB 6.908, 4 KiB 6.910, 16 KiB 7.058.
Snap frame time on rgdsplus (2 runs, p50 / p95): off 31.16 / 36.62; 1 KiB 30.97 / 35.41; 4 KiB 31.34 / 35.63; 16 KiB 31.30 / 35.92: no difference. 4 KiB is the default: best on the RK3566, equal to 1 KiB on the H700, fewer calls than 1 KiB in the heavier lists, with no cost in frame time.

**Mario Kart 64 race against `ref`** (rsp-hle + parallel-rsp as two modules, the pairing arMLEt replaces; three runs each, rotating order). The phase A gap (arMLEt about 1 % slower in a 1500-frame run from boot) did not show in this window:

| device | RSP ms/frame ref | arMLEt OFF | arMLEt ON | OFF - ref | ON - ref | emu thread: OFF - ref / ON - ref |
|---|---|---|---|---|---|---|
| rgdsplus | 5.501 +- 0.005 | 5.523 +- 0.070 | 5.126 +- 0.019 | +0.023 (+0.4 %, inside the spread) | **-0.375 (-6.8 %)** | +0.010 / -0.407 ms |
| rg35xxsp | 5.903 +- 0.026 | 5.922 +- 0.013 | 5.740 +- 0.039 | +0.019 (+0.3 %, inside the spread) | **-0.163 (-2.8 %)** | +0.029 / -0.165 ms |

Coalescing does not explain the old 1 % (it is not there in this window with it off either); it makes arMLEt faster than the reference pairing in this game.

## Profiler checks (docs/profiling.md)

The profiler and the `PRSP_HOOKS` hooks in the fork must not change what the module does. Checks, with the foir `devel` build at 88d1722:

| check | result |
|---|---|
| the normal module of the tree with the hooks and the profiler sources against the tree before them (`git archive` of the old commits, same `scripts/build.sh`) | `.so` md5 identical for `native`, `aarch64` and `rg35xxsp` (the compiled `rsp_jit.cpp` / `cp0.cpp` objects of the rg35xxsp build are byte-identical too); the rg35xxsp `.so` is also the one phase A built |
| the fork's own tests with the hooks in the tree (default build) | `jit_diff` (aarch64 under qemu-aarch64) `div` 26192 runs, `all` 30240, `vu` 1000: 0 mismatches; `dma_diff` 3 seeds x 30000 iterations: 0 mismatches (also compiled with `-DPRSP_HOOKS`, no hooks installed) |
| `PRSP_SHADOW` build (`build.sh rg35xxsp --shadow`) on rgdsplus | SM64 (497 tasks) and DK64 (584 tasks): 0 differences in registers, DMEM/IMEM, CP0, RDP lists (756626 and 166429 lists) and RDRAM DMA writes; 0 stale compiled regions |
| equivalence harness, normal module, desktop (x86_64, 900 frames) | SM64 and OoT: RSP hash stream, 6 screenshots and audio identical to the reference pairing; the reference twice agrees with itself |
| equivalence harness, normal module, rgdsplus, SM64, 3700 frames | 5321 RSP calls, 6 of 6 screenshots, 7.99 MB of audio identical (and ref against ref) |
| JIT code images (`PRSP_DUMP_JIT`) of five runs of SM64 on rgdsplus: normal module, profiler level 1 twice (no marker nodes), level 2 twice (marker nodes) | all 82816 bytes; any two differ only in 595 move-immediate words (1190 between different `.so` files): the absolute addresses, which ASLR moves |
| profiler runs (levels 1, 2, 3) against the normal module | the RSP state hash streams (`RSPT_HASH`) are identical over their common prefix: desktop SM64 (724 calls; normal module, level 1, 2 with walker and task log, 3), rgdsplus SM64 (966 calls; levels 1, 2, 3), and on rgdsplus every `fit` (level 2) / `cnt` (level 3) pair of the profiling sessions (1692 to 5086 calls per game) |

## Native routines: the framework (docs/design.md, "Native routines")

The framework is tested with `test.dma`, a deliberately small routine (`src/native/test_dma.c`): the SP-DMA helper at IMEM 0xfd8 that every F3DEX2 microcode ends with. It has the
properties that matter for the hook, in about ten instructions: a dynamic exit (`jr ra`), SP register writes that run a real DMA (read into DMEM and IMEM, write to RDRAM), and in the
boot sequence a DMA into IMEM that changes code. It saves nothing (a stub call costs more), so it only runs when named. The real routines are listed in their families' sections.

| check | result |
|---|---|
| default fork build unchanged: every object file and the `.so` of the fork's plugin target, built from the base commit (`git archive f067b3a`) and from the tree, same path, same flags | `native` (x86_64), `aarch64` (stock cross toolchain) and `rg35xxsp` (glibc 2.31 sysroot): all 20 object files and the `.so` have identical md5 |
| the module with `GfxNative=off` against the reference pairing (rsp-hle + parallel-rsp): RSP hash stream, state stream, 6 screenshots, audio | DK64, 900 frames: identical on the desktop and on rgdsplus |
| `GfxNative=on`, `test.dma` named | the same, identical on the desktop and on rgdsplus (53422 native calls in 900 frames on rgdsplus; 0 declined) |
| `GfxNative=verify`, `test.dma` named, `ARMLET_NATIVE_ABORT=1` | the same; 53422 calls verified on rgdsplus, 0 mismatches (the module would have aborted) |
| offline harness: `armlet-native-replay` over 32 snapshots captured from DK64 (`ARMLET_NATIVE_CAPTURE_EVERY=50`) | 32 of 32 native == JIT, the recorded JIT result reproduced in all |
| micro-benchmark (`--bench 5000`, desktop) | `test.dma`: native 30 ns, JIT 39 ns per call (the JIT runs about 12 instructions): no gain, as expected |

**The verify machinery catches errors.** `scripts/build.sh native --tools --nmut=N` builds `test.dma` with one deliberate error; `tools/native/mutcheck.sh` replays the 32 snapshots with each of the
eight builds. What reports it, in the order of the first line of the report:

| mutation | caught by | first difference reported |
|---|---|---|
| 1 `t3` (a GPR) ends as 1 | the GPR comparison | `sr[11]: native 00000001, JIT 00000000` |
| 2 another GPR changed | the GPR comparison | `sr[2]: native ..b1, JIT ..b0` |
| 3 a DMEM word changed | the DMEM comparison | `DMEM[7f0]: native 000001ff, JIT 000000ff` |
| 4 DMA length register one short | the log of effectful SP writes | `SP write #0: native kind 2 .. 0000018e, JIT .. 0000018f` |
| 5 exit pc `ra + 4` | the JIT run does not stop where the routine said: it runs on to that pc and the whole state differs | 135 differences, `sr[1]` first |
| 6 the SP_DRAM_ADDR write left out | the SP register comparison and the write log | `SP/DPC register 1: native .., JIT ..` and `SP write #0` |
| 7 the DMA write goes to the wrong RDRAM address | the hash of the bytes written to RDRAM, in the write log (the RDRAM itself is put back by the sandbox) | `SP write #0: native kind 2 .. <hash>, JIT .. <hash>` |
| 8 a vector register changed | the vector register comparison | `vr[17]` with the lanes of both |

All eight are caught in every snapshot where they apply (7: the snapshots whose call is a DMA write).

### Framework additions after the first commit

| check | result |
|---|---|
| default fork build still unchanged (every object file and the `.so` of the plugin target against the base commit, native / aarch64 / rg35xxsp) | identical md5, with the second dependency range and the invalidation change in the tree |
| `armlet-native-replay --dep2test`: a routine with two IMEM dependency ranges (`dep2_lo/hi`) | a changed word between the ranges leaves it in place, a changed word of either range drops it |
| lane masks: `tools/native/mutcheck.sh` mutations 9 and 10 (`dead_vr_lanes[17] = 0x08`, a change of lane 3 / of lane 4 of v17) | 9 is not reported (the lane is declared dead), 10 is reported in 32 of 32 snapshots; mutations 1-8 caught as before (re-run after the watchdog and the `fflush` fix) |
| POISON and probing, DK64, 400 frames, oracle-only `cap.f3dex2.vtx` followed by `ARMLET_NATIVE_PROBE`: `vl20=0xf` (lanes 0-3 of v20), `vl1=1` (lane 0 of v1), `vl30=0x80`, `fl0=0x2` | identical to the reference for v20; diverges for v1, v30 and VCO bit 1 (those are live): a wrong claim is caught by the hash stream |
| 600-frame DK64 equivalence, `GfxNative=verify` and `on` + `ARMLET_NATIVE_POISON=1` with `test.dma` | RSP hash stream, state stream, 6 screenshots and audio identical to the reference |
| the framework alone (no F3DEX2 routine files) builds with `-DARMLET_NATIVE_TOOLS=ON` and passes the replay of 15 `test.dma` snapshots | all equal |
| `armlet-native-replay --hookbench` (desktop x86_64): a loop calling a two-instruction subroutine, with and without the hook | the hook adds about 5 ns per call (aarch64 devices: measured with the F3DEX2 routines) |

## Task-level path: the framework (docs/design.md, "Task-level native path")

The framework (sandbox, commit by replay, verify, capture, the interpreter, the DMEM probe) is tested with the F3DEX2 family as it is today, whose `run()` is the interpreter. It is the reference
implementation: a native front-end must do what it does. Games: Donkey Kong 64, Ocarina of Time, Paper Mario, Pokemon Snap, 700 frames each on the x86 desktop, `GfxMode=hle`, against the reference
pairing (rsp-hle + parallel-rsp), `tools/equiv` hash stream, state stream, 6 screenshots, audio.

| check | result |
|---|---|
| offline replay of the captured task starts (`armlet-native-replay`, 60 + 60 + 330 + 120 tasks), the family on the interpreter against the recorded JIT result: x86 desktop, rgdsplus (A55), rg35xxsp (A53) | 570 of 570 equal on each |
| `ARMLET_HLE_VERIFY=1` (the JIT is the oracle), 4 games | DK64 255, OoT 447, PM 703, Snap 575 tasks compared, 0 differences; equivalence PASS for all four |
| `ARMLET_HLE_VERIFY=interp` (the interpreter is the oracle, the JIT runs for real afterwards), 4 games | the same counts, 0 differences; equivalence PASS |
| `ARMLET_DMEM_POISON=mask` (the build's dead ranges overwritten with a different pseudo-random pattern at the end of every task-level run), 4 games | equivalence PASS: hash stream (masked by the same table), state stream, screenshots, audio identical |
| `ARMLET_DMEM_POISON=all` (DMEM [0, 0xfc0) after every task), 4 games, 700 frames | a first data point only: state stream, 6 screenshots and audio identical in all four; the hash stream differs, as it hashes DMEM. The per-task event lists and gameplay windows on both devices are still to be done for F3DEX2 |
| `tools/hle/build_prim_test.sh` (hle_fx.h against the VU instruction sequences, 200 000 trials x 8 lanes per primitive) | 0 failures; with `HLE_FX_MUTATE=1..7` (a deliberate error in function n) 2 264 697, 703 830, 735, 7 698, 21, 772 760 and 255 517 failures |
| default fork build unchanged: the 20 object files of `build_m64p_plugin.sh` (x86_64), fork HEAD against the tree with the framework's two fork changes | 19 identical; `rsp_disasm.cpp.o` differs (below); the `native_enter` shortcut is inside `PRSP_NATIVE` |

**Fork changes.** `rsp_disasm.cpp`: SB / SH / SW with `rt == 0` were printed as `nop` (the emulation was never affected, only the disassembly: `sw $0, 473($0)` is a real store); `rsp_jit.cpp`: `native_enter`
continues with the block at the new pc if there is one (the cost of a hooked call that returns to compiled code fell from 183 ns to 46 ns).

**The probe finds what is dead.** `ARMLET_DMEM_POISON=all` (DMEM [0, 0xfc0) after every task, whoever ran it) must change nothing the CPU or the RDP can see; where it does, the ranges that matter are
found by bisecting the list (`ARMLET_DMEM_POISON=<lo>-<hi>,...`). The result for a family is the evidence of its mask (design.md "DMEM masks"): events, RDP stream, pictures, audio, on both devices
over gameplay windows.
