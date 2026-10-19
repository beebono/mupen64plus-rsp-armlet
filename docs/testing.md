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
| rg35xxsp (`resources/toolchains/rg35xxsp`, also via its `build.sh armlet`) | the six | **GLIBC_2.17**, GLIBCXX_3.4.29 (the device's libstdc++ is GCC 12's) |

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

**Framework changes of the second round** (external-effects compare, the output ring as a mask, cross-list stream parsing, the cheaper log, the staleness check). Evidence:

| check | result |
|---|---|
| the index of this change set exported to a scratch directory (`git checkout-index`, the submodules copied) builds (`scripts/build.sh native --tools`, `native`) and replays 25 captured tasks of each of the four games (the family runs on the interpreter) | 100 of 100 equal, in the default mode and with `ARMLET_HLE_EXACT=1`; the captured tasks of another family are reported as "no task-level handler in this build" |
| desktop equivalence, 4 games x 700 frames, the exported module with `ARMLET_HLE_VERIFY=interp` | PASS for all four (hash, state, screenshots, audio), 255 / 447 / 703 / 575 tasks compared, 0 differences |
| `armlet-native-replay --dmabench BYTES` (the cost of one sync: a DPC_END write and a DMA write of BYTES to the ring, in the sandbox), x86 desktop, 1024 bytes | events recorded (verify, capture): 1067 -> 946 ns; not recording: 208 -> 70 ns |
| the same on rgdsplus (A55), 344 / 1024 bytes | recording: 3.3 / 7.9 -> 2.8 / 7.0 us; not recording: 1.28 / 2.91 -> 0.56 / 1.95 us |
| the same on rg35xxsp (A53) | recording: 3.2 / 7.9 -> 2.9 / 6.7 us; not recording: 1.38 / 2.73 -> 0.53 / 0.96 us |
| the harness runs every task a second time with nothing recording (the short DMA paths) and compares the end state and the commit log | a deliberate error in the short path (the wrong source words) is reported on every task with a DMA write |

Where the sync time went: the fork's sandbox allocated a `std::vector` for the old bytes of every DMA row, the commit log copied the written bytes one word at a time, and the commit replay copied them back one word at a time; with events recorded (verify, capture) the fork also hashes every DMA write and every list, which is the 7 us per KB and stays. The log is built when a task runs
natively (the commit needs it); nothing is hashed or formatted per DMA / per list unless `ARMLET_HLE_VERIFY` or `ARMLET_HLE_CAPTURE` is on.

**Fork changes.** `rsp_disasm.cpp`: SB / SH / SW with `rt == 0` were printed as `nop` (the emulation was never affected, only the disassembly: `sw $0, 473($0)` is a real store); `rsp_jit.cpp`: `native_enter`
continues with the block at the new pc if there is one (the cost of a hooked call that returns to compiled code fell from 183 ns to 46 ns).

**The probe finds what is dead.** `ARMLET_DMEM_POISON=all` (DMEM [0, 0xfc0) after every task, whoever ran it) must change nothing the CPU or the RDP can see; where it does, the ranges that matter are
found by bisecting the list (`ARMLET_DMEM_POISON=<lo>-<hi>,...`). The result for a family is the evidence of its mask (design.md "DMEM masks"): events, RDP stream, pictures, audio, on both devices
over gameplay windows.

## F3DEX2 dialects, G_LOAD_UCODE and S2DEX 2.x (docs/design.md, "Dialects of the F3DEX2 front-end")

Builds: F3DEX 2.08 `ac31` (Dr. Mario 64) and `d2d1` (WinBack), F3DEX 2.03 `33dc` (F-Zero X), F3DEXBG.NoN 2.08 (Conker's Bad Fur Day), S2DEX fifo 2.08 (Worms Armageddon). Desktop (x86_64), `GfxMode=hle`, against the reference pairing.
Task snapshots: `ARMLET_HLE_CAPTURE` (every n-th task start of a JIT run; `ARMLET_UCODES=0000000000000001` keeps the native path out of the run), replayed by `armlet-native-replay`.

| check | result |
|---|---|
| offline replay of captured task starts, x86_64 | Dr. Mario 64 (ac31) 56 of 56, WinBack (d2d1) 41 of 41, Conker 289 of 289 and 313 of 313 (about 75 % of them with lit vertices, native by default now), Worms 60 of 60; the four original builds (DK64) 60 of 60 after the layout refactor |
| the same with the aarch64 build (NEON vector-unit backend, `scripts/build.sh aarch64 --tools`) under `qemu-aarch64-static` | 157 of 157 (Worms, Dr. Mario, WinBack d2d1), Conker 289 of 289 and 313 of 313, Worms 60 of 60 (the closed forms; the lit pass was scalar then, the vertex pipeline under it NEON; the lit pass is NEON now: section "F3DEXBG lit pass, NEON") |
| `ARMLET_HLE_VERIFY=1` equivalence runs, 600 to 3000 frames | WinBack (d2d1 tasks) PASS 600 f; Dr. Mario PASS 600 f; Conker PASS 1500 f (430 tasks native, 0 differences, 145 lit tasks declined by the transliteration then) and 2500 f with the transliteration enabled (895 tasks, 0 differences); Worms PASS 2000 f (1343 tasks, 0 differences); F-Zero X PASS 3000 f (33dc was registered then, since removed (docs/ucode-f3dex2-dialects.md); but 1277 of its 1280 tasks decline: `G_LOAD_UCODE` of the Rej text) |
| `ARMLET_HLE_VERIFY=1`, 3700 frames (the 62 s of the other results), after the fixes below | WinBack 1471 tasks, Paper Mario 3583, Donkey Kong 64 1663, Worms 2879, Dr. Mario 3263, Conker with the transliteration enabled 1215: 0 differences, equivalence PASS each (Conker without it: 486 of 1216 tasks native, the rest lit and declined) |
| the same with the closed forms (S2DEX background, F3DEXBG lighting), 3700 f, `ARMLET_HLE_VERIFY=1` | Worms 2879 of 2880 tasks native, Conker **1215 of 1216 native, 0 declined**, 0 differences, equivalence PASS each (hash, state stream, 6 screenshots, audio identical); the same without verify: PASS each |
| `tools/hle/build_test_s2dex2.sh` (rectangle: NEON against scalar; background: closed form against the microcode, `S2BG_ORACLE`), `tools/hle/build_test_f3dexbg.sh` (the lit pass after the NEON and the scalar vertex forms; the NEON lit pass against the scalar one, section "F3DEXBG lit pass, NEON"), under qemu-aarch64 | 20 000 cases each, 0 failures |
| closed forms against the microcode's transliteration on the whole DMEM (one-off differential harnesses of the development, since removed; "Regenerating the microcode oracles" below for what is kept) | F3DEXBG lit commands: 12 676 captured (odd and even counts, 1 to 11 lights, positional or not, texgen) + 300 000 mutated, 0 differences; S2DEX backgrounds: 1 002 captured + 100 000 mutated (random object, scale, flip, formats, tables, scissor, segment table), 0 differences, 99.7 % of the lines of `s2dex2_bg.c` and all of `f3dexbg_fx.c` executed (a model decline where the microcode's loop would not end, sizes of 0 or less, is not a difference) |
| production runs (no verify), 1500 f and (Worms, Conker, WinBack, Dr. Mario) 3700 f | Worms, Conker, WinBack, Dr. Mario: hash stream, state stream, 6 screenshots and audio identical to the reference |
| `ARMLET_DMEM_POISON=mask` and `all`, 900 f | `mask`: Worms, WinBack, Dr. Mario, Conker PASS (hash included). `all`: state stream, pictures and audio identical (the hash stream differs by design) in Worms, Dr. Mario, WinBack |
| `tools/hle/build_test_s2dex2.sh` with `S2RECT_ORACLE` (rsp2c translation of overlay A), `S2RECT_VU=1` (the old instruction-level kernel) and `S2BG_ORACLE`: random DMEM images, the closed forms against the microcode | 500 000 cases on x86_64 (125 785 rectangles drawn, 226 967 outside the scissor, 21 859 declined, 45 765 backgrounds), 150 000 under qemu-aarch64 (37 876 drawn), 0 failures on both oracles for the rectangle |
| x86 front-end time against the JIT (`armlet-native-replay --bench`) | ac31 4.0 to 4.3 times faster, d2d1 3.0 to 3.5, Conker unlit 2.6 to 3.3, **Conker lit 3.4 to 3.9** (the transliteration: 0.8), **Worms 1.6 to 3.4** (the transliteration: 0.96 to 1.5; tasks of 3 to 11 us, one trivial task 0.85); the lit command alone takes about 125 ns per vertex |
| the rectangle closed form against the instruction-level kernel it replaced (x86_64, scalar vector-unit backend; `--bench 400` over the 60 Worms snapshots, 3 alternating runs) | front-end time of the 60 tasks 364 us (old) to 298 us (new), i.e. 2.64 times the JIT's speed becomes 3.22; one call of the kernel alone (a 32 x 32 unscaled sprite, copy of the DMEM image subtracted) 158 ns to 28 ns. No NEON timing: there is no device access in this work; the old kernel's NEON backend is the same dataflow on the 8-lane vector, the closed form has ~60 integer operations |

**Mutation check** (`tools/hle/mutcheck_dialects.sh`, deliberate error N, replay of the captured tasks of the game; "caught" = a mismatch, a decline the unmutated build does not have, or a hang):

| N | where | Worms / Conker snapshots |
|---|---|---|
| 30, 31 | rectangle closed form: the shift and the offset of the render-mode bit 0x20 | not caught by Worms (it never sets the bit); caught by the random test against the microcode (2472 and 2491 failures in 20 000 cases) |
| 32, 33, 34 | rectangle closed form: tile toggle, sync advance, the SET_TILE field shift | caught (17 snapshots each) |
| 150 .. 166 | rectangle closed form (`s2dex2_rect.c`): the reciprocal refinement, the position bias and mask, the trim, the width shift, the scissor cut and its sign, the mirrored first texel and step, the flip flag of y, the bias of s, the cut's rounding, the sub-pixel part of t, the SET_TILE_SIZE field mask | Worms snapshots catch 150, 151, 153, 154, 155, 157, 160, 161, 162, 164, 165 (6 to 17 of 60); 152 (scissor boundary), 156 (rounding of the cut), 158 (y flip flag), 159 (SET_TILE_SIZE field mask), 163 (position mask), 166 (trim) not (unscaled, unflipped, inside the screen), caught by the random test against the microcode (41 to 4286 failures in 20 000 cases each; `mutcheck_dialects.sh` with `S2RECT_ORACLE` runs both) |
| 35, 36, 37, 38, 39, 44 | LOADTXTR, RENDERMODE table, RDPHALF_2 order, SETSCISSOR decode | caught (17, 17, 2, 60, 6, 60 snapshots) |
| 70 .. 93 | the background routine (`s2dex2_bg.c`): reciprocal shift, extent mask, loop step, flip, scissor, scale, line size, quotient compare / mask, strip commands, ... | 70, 75, 78, 80, 81, 84, 86 .. 93 caught by Worms snapshots (16 to 59 of 60); 71, 72, 73, 74, 76, 77, 79, 82, 83, 85 not (Worms uses unit scales, no flip, no scissor cut), caught by the random test against the microcode (49 to 1105 failures in 6000 cases each; 79 also by the 1 002 captured backgrounds) |
| 100 .. 121 | the F3DEXBG lighting (`f3dexbg_fx.c`, `f3dexbg_dmem.h`; 121: the phantom partner of an odd count at the last slot, read from the display list chunk 8 bytes off): attenuation nibble, min / max, view-space constants, ambient, lambert masks, non-positional sum, vertex colour, texgen, flag, group, scratch words, light setup stride | all caught by the Conker snapshots (17 to 256 of 313 + 289) except 111 (lane group swapped: the two halves hold equal data in the captured tasks) and 114 (the w lane of the distance: zero in them), caught by the random test (including the point-light constants the old routine's mutants 62 to 64 could not reach) |
| 65, 66 | `G_TRI4` decode, `G_LOAD_UCODE` input buffer | caught (211 snapshots + 74 declined; all 289 declined: every task ends with the command) |

(A mutation that makes the routine loop forever is counted as caught: the replay is killed after 100 s.)

**A bug found by the longer runs, in the shared triangle code.** WinBack (3700 frames, `ARMLET_HLE_VERIFY=1`) had 120 tasks with a different RDP stream: the z block of a triangle read the fraction of the start value after adding the integer product, so a
saturated integer part (0x7fff) took the fraction to 0xffff where the microcode leaves the low bits (`fx2_tri_attrs`, scalar and NEON; the fraction is read before the integer product, as everywhere else). The four original games never hit it. After the fix all 201 captured WinBack tasks are equal.
The F3DEXBG lit routine's longer run (Conker, 3700 frames) showed 60 tasks whose light slot pad bytes beyond the ninth slot differed: the dead list now covers 13 slots.

**Closed forms replace the transliterations.** S2DEX's background (`s2dex2_bg.c`) and F3DEXBG's lit vertices (`f3dexbg_fx.c`) were first transliterations of the microcode (generated, exact, 0.8 to 1.5 times the JIT); they are hand-derived
closed forms now, checked against those transliterations first ("Regenerating the microcode oracles" below) and then against the JIT by the runs above. Not bit-exact corners are declined, not emulated: a background whose wrap loop would not end, an even number of
lights (the microcode's lighting loop never ends on it), more than 11 lights, DMEM vector constants other than S2DEX 2.08's.

## F3DEX2 2.04 and 2.04H (docs/ucode-f3dex2-dialects.md, section 5)

Builds `5dce` (F3DEX 2.04: Buck Bumble, Cruis'n World) and `e857` (2.04H: Super Smash Bros.; Kirby 64 loads S2DEX in every task: it declined then, section 5.6 and the next section). Desktop (x86_64) unless said, `GfxMode=hle`, against the reference pairing, 3700 frames (62 s) unless said.

| check | result |
|---|---|
| equivalence gates, `ref`, `armlet-hle` and `armlet` (default) | Buck Bumble, Cruis'n World, Super Smash Bros., Kirby 64: hash stream, state stream, 6 screenshots, audio identical in both pairs |
| `ARMLET_HLE_VERIFY=1` (and with `ARMLET_HLE_EXACT=1`), 3700 f | Buck Bumble 3711 tasks, Cruis'n World 1663, Super Smash Bros. 3327: 0 differences, 0 declined; equivalence PASS each |
| `ARMLET_HLE_VERIFY=1`, 12000 f | Buck Bumble 11967 tasks, Cruis'n World 5695, Super Smash Bros. 10559: 0 differences |
| share of the graphics tasks of the 3700 frames that ran natively | Buck Bumble 3711 of 3712, Cruis'n World 1663 of 1664, Super Smash Bros. 3327 of 3328 (the first task of a run never is), Kirby 64 0 of 1792 then (692 + 11 declines per 1500 frames), 1791 of 1792 with the build switching (next section) |
| `ARMLET_HLE_POISON=1`, `ARMLET_DMEM_POISON=mask`, `=all`, `=ring` | `POISON=1` and `mask` PASS (hash included) in all three; `all`: state, pictures and audio identical (hash differs by design); `ring` PASS in Buck Bumble and Cruis'n World; in Super Smash Bros. the hash differs (first at call 1165) **with the JIT alone** (`ARMLET_UCODES=1`), state, pictures and audio identical: the game reads its ring (not the front-end) |
| regression, 3700 f, `ref` against `armlet-hle` | Paper Mario, Dr. Mario 64, Banjo-Kazooie, Conker's Bad Fur Day, Worms Armageddon, Super Mario 64: identical (hash, state, screenshots, audio) |
| captured task starts, recorded JIT result: x86_64, aarch64 under qemu NEON, aarch64 under qemu scalar (`scripts/build.sh aarch64 --tools [--scalar]`) | Buck Bumble 150 of 150, Cruis'n World 337 of 337 (every task of the first 900 frames), Super Smash Bros. 150 of 150, Donkey Kong 64 55 of 55; the same on all three |
| synthetic tasks (`tools/hle/synth_ex2.sh`, `mksyn_ex2.py`: clip, zneg, tri, mix, misc, lit), JIT run from the same state (`--oracle`), 400 to 1000 seeds per case | x86_64: 100 % equal on a Buck Bumble, a Cruis'n World and a Super Smash Bros. task (lit and clipping ones as bases) and on Donkey Kong 64 (before the fixes below: 16 to 25 % of the clip, zneg, tri and mix seeds differed on every build, 30 % of the lit seeds on 2.04H); aarch64 NEON and scalar under qemu: 600 to 1000 seeds per case, 0 differences |
| `tools/hle/build_test_fx2.sh` (NEON forms against scalar, with the new `gen204` and `flat` inputs), qemu-aarch64 | 200 000 triangles, 20 000 vertex batches, 50 000 matrices: 0 differences; with `-DFX2_NEON_MUTATE=6` (the NEON z clamp): 4584 of 10 000 batches differ |
| mutations 210 .. 216 (`tools/hle/mutcheck_204.sh`) | 210 (clipper sign): synthetic only, 336 of 400 tasks; 211 (fan): 15 captured, 1242 synthetic; 212 (unclamped z): 1 captured (Cruis'n World), 2215 synthetic; 213 (phantom vertex of the last slot): 20 captured, 52 synthetic; 214 (flat colour of clipped triangles): synthetic only, 288; 215 (texgen polynomial): synthetic only, 114; 216 (the valid byte the matrix concatenation sets): 275 captured, 2260 synthetic. All caught |
| x86 front-end time against the JIT (`armlet-native-replay --bench 300`) | Buck Bumble 3.2 to 5.1 times, Cruis'n World 1.9 to 3.6, Super Smash Bros. 3.9 to 5.0 (tasks of 25 to 600 us of JIT time) |
| builds: `scripts/build.sh native`, `aarch64`, `rg35xxsp` (`RG35_TOOLCHAIN`), `--tools` | succeed; no new warning in `src/` or `tools/` (the fork's own `-Wparentheses` / `-Wunused-function` ones are unchanged); the rg35xxsp module needs GLIBC_2.17 |

**What the captured tasks did and did not show.** Buck Bumble's first replay had one task with a different RDP stream (the fan of a clipped quad; mutation 211), Cruis'n World's one (a clipped triangle with a negative screen z; mutation 212) and Super Smash Bros.'s 20 tasks differed
in the flags of the last vertex slot (213). The clipper's reciprocal sign (210), the texture generation polynomial (215) and the flat colour of clipped triangles (214) are not reached by the captured tasks at all, and the first synthetic tasks (`clip`, `lit`) found them.
Both general bugs (flat colour, phantom vertex) were in the front-end for every F3DEX2 build; Donkey Kong 64's 300 seeds per case are equal after them as well.

**A capture bug found on the way.** `ARMLET_HLE_CAPTURE` wrote the RDRAM rows a task's DMA reads read as they were at the *end* of the task. Cruis'n World's RDP ring overlaps its ucode data (section 5.3 of the dialects page), so a task's own flush
overwrote the rows of the next task's start: replays of its small tasks differed in 0x6e0 .. 0x800 and 0x7c5. The rows are copied when the DMA event comes now (`hle_task.c`, `row_save`).

**Replay harness: `HLE_PATCH`.** `armlet-native-replay` writes `HLE_PATCH=addr:hex;addr:hex` into RDRAM before the task (and `HLE_POKE` into DMEM): the synthetic display lists of `tools/hle/mksyn_ex2.py` (and `tools/f3d/mksyn.py`, whose runtime side this is) live at 0x300000; use `--oracle`. The snapshot supplies the
microcode, its overlays (a base that never loaded the lighting overlay has none in its RDRAM image: the lit cases need a base with lit vertices, and a clipped one) and the start state.

## Fuzzing the front-ends (input no game produces)

`tools/rsp2c/fuzz_snaps.py` writes perturbed copies of captured task snapshots (bytes of the display list, vertex and matrix rows flipped, words replaced by words of other commands); `armlet-native-replay --oracle` runs the front-end and
the JIT from the same perturbed state, so every difference is a front-end error (`tools/rsp2c/replay_each.sh` runs one replay per file in a memory-capped scope with a time limit). `tools/rsp2c/fuzz_cmds.py` aims at one command (an opcode and
the field of it to replace), `tools/rsp2c/ddmin_snap.py` reduces a mismatching file to the few bytes that matter (mostly one to three), and `HLE_F3_TRACE` / `F3DEX_TRACE` / `F3D_TRACE` (commands, vertices with their clip flags, triangle decisions),
`ARMLET_HLE_EXACT=1 HLE_EV_DUMP=1` (the first differing SP register write with the events around it), `ARMLET_HLE_INTERP=1 HLE_RSP_WATCH=<addr> HLE_RSP_TRACE=1` (who writes a DMEM byte, counted in command loop heads) find the cause. Every mismatch was
classified as: **(a)** a front-end or table bug reachable by a list a game can write: fixed; **(b)** a state only a self-modifying or illegal list reaches (the display list overwrites the ucode's own buffers, a vertex number past the table, the list in the
RDP output ring): detected where the command is handled and declined to the JIT, table in [design.md](design.md#task-level-native-path) ("Input no game produces"); **(c)** a fuzzer artefact: none was found (the perturbation only touches rows that
go to DMEM above the data segment, not the microcode's text; an early targeted run that perturbed the text rows is the reason `fuzz_cmds.py` uses the same row selection).

**The extent before and after** (Donkey Kong 64, Ocarina of Time, Buck Bumble, Paper Mario, Banjo-Kazooie, Super Mario 64, Conker, Worms, Super Smash Bros., and Cruis'n World / Kirby 64 as the other 2.04 / 2.04H builds; 11 sets of perturbed copies of the
captured tasks with different seeds, 810 to 4000 per game and set, plus display-list-only and vertex-only sets, about 300 000 perturbed tasks in all, of which 154 000 in the table; devel at e499bad against this branch; `--oracle`, x86_64):

| game (tasks) | before: equal / mismatch / declined / hung | after: equal / mismatch / declined / hung |
|---|---|---|
| Donkey Kong 64 (19 104) | 6480 / 475 / 12 120 / 28 | 5991 / 0 / 13 113 / 0 |
| Ocarina of Time (19 250) | 3988 / 286 / 14 964 / 9 | 3709 / 0 / 15 541 / 0 |
| Buck Bumble (15 600) | 3911 / 9 / 11 679 / 1 | 3710 / 0 / 11 890 / 0 |
| Paper Mario (14 637) | 10 611 / 62 / 3962 / 1 | 10 307 / 0 / 4330 / 0 |
| Banjo-Kazooie (10 350) | 845 / 347 / 9155 / 3 | 759 / 0 / 9590 / 1 |
| Super Mario 64 (10 373) | 1628 / 142 / 8222 / 40 | 1076 / 0 / 9280 / 17 |
| Conker (18 490) | 8855 / 238 / 9327 / 66 | 7471 / 0 / 11 010 / 9 |
| Worms Armageddon (10 431) | 7682 / 69 / 2680 / 0 | 7524 / 0 / 2907 / 0 |
| Super Smash Bros. (14 700) | 3832 / 29 / 10 834 / 4 | 3653 / 0 / 11 047 / 0 |
| Cruis'n World (10 784) | 1522 / 511 / 8723 / 28 | 1225 / 0 / 9559 / 0 |
| Kirby 64 (10 440) | 442 / 1 / 9997 / 0 | 393 / 0 / 10 047 / 0 |
| Pokemon Snap, Dr. Mario 64, Mario Golf, Hydro Thunder, RR64, Star Fox 64, Mario Kart 64, Wave Race, Pilotwings (1512 each) | 4 to 18 mismatches in 6 of them | 0, except one Pokemon Snap task (below) |

The first set alone (825 per game, seed 1) is the run the task started from: Donkey Kong 64 120 / 23 / 682 declined before, 99 / 0 / 726 after; Ocarina of Time 124 / 12 / 687, 109 / 0 / 716; Buck Bumble (the 2.04 captures) 220 / 0 and
Cruis'n World 134 / 52 (of 1011) before, 206 / 0 and 108 / 0 after. The decline rate of the perturbed tasks rises by 5 to 10 points; the tasks that were "equal" before and decline now were equal by chance (a perturbed list that did not reach the state).
**Real play does not decline more:** the 3700-frame gates (`ref` against `armlet-hle`, built from devel and from this branch, 9 games above) are identical in hash, state, screenshots and audio, and the native share of the graphics tasks is the same to the
task (`ARMLET_HLE_STATS`): Donkey Kong 64 1663 of 1664, Ocarina of Time 544 of 1472 (the `G_LOAD_UCODE` declines of section 3 of the dialects page), Buck Bumble 3711 of 3712, Paper Mario 3583 of 3584, Banjo-Kazooie 1535 of 1536, Super Mario 64 1727 of 1728, Conker 1215 of 1216,
Worms 2879 of 2880, Super Smash Bros. 3327 of 3328. The captured task starts of the 19 sets of captures (the 9 games above, Cruis'n World and the 9 others) replay equal with 0 declined (Ocarina of Time 27 declined, as before); the same 88 of them under qemu-aarch64 (NEON) are equal; 360 perturbed
ones under qemu: 6 equal, 354 declined, 0 mismatches. The targeted sets (one opcode, one field, 49 000 for the F3DEX2 builds and 28 000 for F3DEX / F3D / S2DEX / F3DEXBG): 0 mismatches.

**Mutations** (`tools/hle/mutcheck_fuzz.sh`, `FILES=<the perturbed snapshots that differed before the fixes, 2175 of them> SYNTH=<a Donkey Kong 64 and an Ocarina of Time snapshot>`): each decline or fix has a number (`MUT(n, ok, bad)`, `scripts/build.sh native --tools --hmut=n`; the
table in design.md) and the 27 builds with one of them switched off each make some perturbed or synthetic task differ from the JIT (94: 64 files, 217: 588, 218: 88, 219: 16, 221: 33, 222: 5 + 2 synthetic, 223: 1 synthetic task, 224: 15, 225: 25, 226: 1, 227: 306, 228: 26, 229: 23, 230: 2, 231: 34,
232: 1, 233: 4, 235: 36, 236: 6, 237: 1, 238: 1, 239: 1, 242: 1, 243: 1, 244: 1, 245: 25 + 3 synthetic, 246: 21 synthetic tasks). 220, a guard for `G_MOVEWORD` stores into the buffers, was dominated by the offset bound (235) and was removed.
The synthetic tasks (`tools/hle/mksyn_ex2.py`) have two new cases: `wild` (coordinates over the whole 16-bit range, matrices of scale up to 256, some coordinates at the ends of the range: 400 seeds equal on Donkey Kong 64 and Ocarina of Time) and `bad` (vertex numbers, indices and offsets no game writes: before the fixes 232 of 400 seeds differed on Donkey Kong 64 and 230 on Ocarina of Time, all 400 decline now); the old `tex` bit in the geometry mode of
the `clip` / `zneg` / `tri` / `mix` cases (bit 1 means nothing to F3DEX2) is gone. Ocarina of Time's `misc` case was 90 % equal before (`G_BRANCH_Z`, mutation 246): 400 of 400 now, all cases.

**Not resolved:** one perturbed Pokemon Snap task (F3DEX.NoN 2.08H, `snap2_Q`, a `G_GEOMETRYMODE` clear that leaves the lighting on): after a lit `G_VTX` that loads the lighting overlay the microcode refills its display list from the overlay's source address
(the DL stack word holds the overlay descriptor's RDRAM address) and the task runs on differently; the lit synthetic tasks of the same build are equal and no captured Snap task reaches the state. 1 in 1512.

## Diddy Kong Racing, Jet Force Gemini and Indiana Jones (docs/native-custom.md)

The three custom microcodes run closed forms (`hle_rare.c`, `hle_indy.c`); the page [native-custom.md](native-custom.md) has the whole test record. In short, on the final tree (desktop, x86_64):

| test | result |
|---|---|
| 3700-frame gates (`ref` twice for the three; `hle` and default against `ref`), SM64, Mischief Makers, Resident Evil 2, Star Fox 64, Mario Kart 64, Banjo-Kazooie, Worms, Conker, Dr. Mario 64, WinBack as well | identical in every pair: hash stream, state stream, 6 screenshots, audio |
| `ARMLET_HLE_VERIFY=1`, 3700 frames (9000 for the three custom games) | 0 differences: Diddy Kong Racing 3903 tasks, Jet Force Gemini 4415, Indiana Jones 2445 (of 5184; the rest declines by design), and the other families' tasks of the games above |
| captured task starts, recorded JIT result: x86_64, aarch64 under qemu NEON, aarch64 under qemu scalar (`scripts/build.sh aarch64 --tools [--scalar]`) | 120 Rare tasks equal; 509 Indiana Jones tasks 201 equal, 308 declined; the same on all three |
| synthetic tasks (`tools/rare/synth.py`, `synth_indy.py`), JIT run from the same state (`--oracle`), same three builds | Diddy Kong Racing 200 of 200 equal; Jet Force Gemini 97 equal, 103 declined; Indiana Jones 193 equal, 107 declined; 0 differences |
| perturbed tasks (`tools/rsp2c/fuzz_snaps.py`, `replay_each.sh`), `--oracle` | Rare 945 tasks: 285 equal, 0 mismatches; Indiana Jones 2000: 153 equal, 0 mismatches (samples of both under qemu NEON and scalar: 0) |
| mutations (`tools/rare/mutcheck.sh`): hmut 40-60 (Rare) and 170-201 (Indiana Jones), gmut 27-41 and 43-46 (F3D functions) | all 72 caught (gmut 30, a NEON form, on aarch64 under qemu) |
| `tools/f3d/test_fxn.c` (NEON against scalar on random vertices, matrices, triangles; now with the Indiana Jones variant, the sort table and the wide fog), aarch64 under qemu | 20 000 cases each, 0 differences |

**Dead ranges of Indiana Jones** (`l_dead` in `hle_indy.c`: 0x16a - 0x16c, 0x270 - 0x378, 0x560 - 0x58a, 0x590 - 0x596, 0xf80 - 0xfd0), 3700 frames, each range alone with
`ARMLET_DMEM_POISON=<lo>-<hi>`, all five together, `=mask` and `=mask,ring`: equivalence PASS for every one (hash, state, pictures, audio identical); `=all` leaves state stream, pictures and audio
identical (the hash stream differs by design); `ARMLET_HLE_POISON=1` (the registers; `live_sregs` are left alone) PASS. The range 0xf80 - 0xfd0 ends in the OSTask header (0xfc0 - 0xfd0), which the
microcode uses as scratch; `rsp_shim.c` took the task's ucode from that header after the call, so the first version of this test failed on the hash stream; the shim identifies the task before the call
now (it reads the header the core wrote). Without 0xfc0 - 0xfd0 in the list the native result differs from the JIT's in those 16 bytes, so the range stays. Diddy Kong Racing and Jet Force Gemini:
`=mask,ring` PASS.

## Build switching, G_BG_COPY and the S2DEX dialect (docs/design.md, "Build switching"; docs/ucode-s2dex2.md section 3c)

The F3DEX2 front-end follows a `G_LOAD_UCODE` to another registered build and back (Kirby 64: F3DEX2 2.04H -> S2DEX 2.08 -> 2.04H, in every task), and S2DEX `G_BG_COPY` is a closed form. Desktop (x86_64) unless said, `GfxMode=hle` against the reference pairing.

| check | result |
|---|---|
| native share of Kirby 64's graphics tasks (`ARMLET_HLE_STATS`) | 3700 frames: 1791 of 1792 (was 0 of 1792; the first task of a run never is), 0 declined; 12 000 frames: 5887 of 5888 |
| captured task starts of Kirby 64 (90, first 1500 frames), recorded JIT result: x86_64, aarch64 NEON and scalar under qemu | 90 of 90 equal on all three, 0 declined (before: 90 declined; 65 of the 90 contain a `G_BG_COPY`, one set of parameters) |
| x86 front-end time against the JIT (`armlet-native-replay --bench 200`, the 90 tasks) | 7.8 ms against 25.6 ms: 3.28 times the JIT's speed (1.3 times for the smallest task, 3.8 times for one of 1.2 ms of JIT time) |
| equivalence gates, 3700 frames, `ref` against `armlet-hle` (`GfxMode=hle`) and `armlet` (default): hash stream, state stream, 6 screenshots, audio | identical, both pairs: Kirby 64, Donkey Kong 64, Ocarina of Time, Paper Mario, Banjo-Kazooie, Super Mario 64, Conker, Worms, Super Smash Bros., Buck Bumble, F-Zero X |
| Kirby 64, 3700 frames, variants | `ARMLET_HLE_VERIFY=1`: 1791 tasks compared, 0 differences; `+ ARMLET_HLE_EXACT=1`: 1791, 0; `ARMLET_HLE_POISON=1`, `ARMLET_DMEM_POISON=mask`, `=ring`: PASS (hash included); `=all`: state, pictures and audio identical, hash differs by design (as in every build) |
| `ARMLET_HLE_VERIFY=1`, 12 000 frames | Kirby 64 5887 tasks, Super Smash Bros. 10 559, Buck Bumble 11 967, Cruis'n World 5695, Conker 4927, Ocarina of Time 586: 0 differences |
| share of the graphics tasks that ran natively, 3700 frames, before -> after | Kirby 64 0 -> 1791 of 1792; Ocarina of Time 544 of 1472 -> 544 (S2DEX 2.06: not registered, 918 + 9 declines); F-Zero X 1 of 1920 -> 1 (F3DLX.Rej: not a build); Conker 1215 of 1216, Worms 2879 of 2880, Super Smash Bros. 3327 of 3328, Buck Bumble 3711 of 3712: unchanged |
| synthetic tasks crossing a load (`tools/hle/mksyn_ex2.py load`: triangles, `G_RDPHALF_1` + `G_LOAD_UCODE` to S2DEX, scissor / render mode / sprites / backgrounds in 1-cycle and COPY mode / texture loads / display list selection / undefined opcodes, `G_LOAD_UCODE` back, triangles), JIT run from the same state, 400 seeds | x86_64 309 equal, 0 differ, 91 declined (the poll limit of loops the random objects make, undefined opcodes); aarch64 NEON and scalar under qemu (300 seeds): 262 equal, 0 differ, 38 declined, the same |
| synthetic tasks with fully random matrices (`mksyn_ex2.py mtx`, 300 seeds), x86, NEON, scalar | 299 equal, 0 differ, 1 declined on all three |
| perturbed copies of the captured Kirby 64 tasks (`tools/rsp2c/fuzz_snaps.py`, `FUZZ_DONOR`: 9 seeds x 30 copies of each of the 90 tasks, 24 300 files), `--oracle`, x86 | 2232 equal, 0 mismatches, 22 068 declined (undefined opcodes, indices and vertex numbers outside what a game writes, the poll limit), 0 hangs |
| aimed perturbations (`fuzz_cmds.py`: the opcodes 01 04 05 06 07 08 09 0a 0b dd e1 ed x the fields w0lo, w0mid, w1lo, 8 copies of each of the tasks that contain the opcode: 36 sets, 21 480 files), `--oracle`, x86 | 14 902 equal, 0 mismatches, 6578 declined, 0 hangs |
| the same, 5 sets under qemu NEON and scalar | identical to x86 (equal / declined counts) |
| older perturbed sets of other games (Worms x4, Conker x4, Super Smash Bros. x4, Buck Bumble x4, Cruis'n World x4: 53 000 files) with this build | 15 958 equal, 0 mismatches (6 hangs of the JIT, as before) |
| G_BG_COPY closed form against the microcode (`tools/hle/test_s2dex2.c`, `S2BGC_ORACLE=tools/hle/oracle/bgcopy_gen.inc`, the rsp2c translation of Kirby 64's 0x224 .. 0x54c; random DMEM images with random stale vector registers) | 1 500 000 cases, 0 failures (56 930 backgrounds drawn, 210 857 outside the scissor, the rest declined by the domain); 600 000 cases (22 833 drawn) against the translation of Worms' build (`S2BGC_ENTRY=0x22c S2BGC_END=0x1ac`), 0 failures; 100 000 cases under qemu-aarch64, 0 failures |
| S2DEX `G_BG_1CYC` against its oracle after the format / texel size guard was corrected (`S2BG_ORACLE`, `S2RECT_ORACLE` regenerated from Worms' end-of-task IMEM) | 4 differences in 200 000 cases, all "the translation stops at 0xfc8" (the microcode waits for a DMA that the closed form does not model); the unchanged devel code gives 2 of the same kind |
| builds: `scripts/build.sh native`, `aarch64`, `rg35xxsp` (`RG35_TOOLCHAIN`), `--tools`, `--tools --scalar` | succeed; no new warning in `src/` or `tools/`; the rg35xxsp module needs GLIBC_2.17 |

**Mutations** (`tools/hle/mutcheck_switch.sh`; deliberate error N, `scripts/build.sh native --tools --hmut=N`; caught = more snapshots mismatch or are declined than with the unmutated build): 240 (a display list address that is not 8-aligned), 241 / 247 (the texture object longer than its
buffer / a class byte that stores outside the state words), 249 (the poll count reset at a load), 250 (the load key ignored), 252 / 253 / 256 / 257 (the layout, the classification, the end pc, the overlay not taken over by `f3_bind`), 254 (the text address not updated: a second load back is taken for the same text), 255 (the stop pc of the old build),
258 (the continuation pcs of Kirby 64's `G_OBJ_LOADTXTR`), 259 (`G_BRANCH_Z` of 2.04 / 2.04H) are caught by the 10 050 captured and perturbed Kirby 64 snapshots (`--oracle`: 240 by 1, 241 by 5, 247 by 2, 249 by 1, 257 by 2173, 259 by 10 mismatches, the others by declines); 251 (the last display list chunk
put in the input buffer) only by the recorded results (40 of the 90 tasks differ in the stale bytes behind a sync; with `--oracle` the relaxed stream compare of a task that switched ignores them), 248 (the "no such command" marker 0xfff of the S2DEX tables) by the synthetic tasks (48 of 400 seeds differ). All 40 mutations of `G_BG_COPY` (260 .. 299)
are caught: 25 by the 90 captured tasks (65 of them mismatch), 15 (262 .. 266, 271, 272, 275, 279, 283, 290 .. 292, 294, 299) only by the random test against the microcode (3 to 3826 failures in 100 000 cases).

**What fuzzing the switching tasks found**, all outside the games' lists and all fixed or declined: `G_BRANCH_Z` of F3DEX2 2.04 / 2.04H compared the clip w (259); the S2DEX builds' "no such command" markers (0xfff) equalled the 0x0fff entries of the jump table (248); the SP_STATUS poll count was reset at a reload,
so a task that spins on the JIT's 32767-poll limit after a load ran on in the front-end (249); a display list address that is not 8-aligned (240); a `G_OBJ_LOADTXTR` whose object is longer than its buffer or whose class byte stores over the jump table (241, 247); the stale bytes behind a one-byte sync (the 7 bytes the
microcode leaves in its output buffer) came from the display list chunk in the input buffer, from the clipper's new vertices and from the triangle setup's scratch: the first is put in DMEM at a switch (251), the others are not kept by the front-end and are zeroed in the stream compare of a task that switched (`hash_r`).
**G_SELECT_DL over the jump table (mutation 315).** Kirby 64 `load` seed 904 of `mksyn_ex2.py` (devel a355089) differed from the JIT: the first `G_SELECT_DL` of the random S2DEX stage had byte 0 of w0 = 0xbe, so its flags word is DMEM 0x2b0 + 0xbe = 0x36e, which covers the jump table entries of opcodes 0xff and 0 (table at 0x370); the second one called the zeros at RDRAM 0xc0000. The microcode stores the word and so opcode 0 now jumps to the loop head (0x004: no output) instead of the sync handler (0x144, which writes a no-op word); the front-end had classified the opcodes once from the table and went on writing a no-op RDP word per zero command (87 160 bytes against the JIT's 1 504). It is not the SP_STATUS poll limit (10 773 polls) nor the end of the list; the interpreter (`ARMLET_HLE_INTERP=1`) and the JIT agree. A game's flags words are the first few (sids), so a flags word over the cached state is declined (`f3_store_unmodelled`, mutation 315; the narrower check of mutation 311, the S2DEX 2.06 / 2.05 branch's, is subsumed). `tools/hle/mutcheck_seldl.sh <Kirby 64 task>.snap` (`mksyn_seldl.py`, the reduced task: load S2DEX, the select, a call of zeros) shows the decline with the fix, the difference with `--hmut=315`, and AT 0x10 native and equal in both. Kirby 64 `load`, 2000 seeds: 0 differences with the fix (seed 904 the only one without it; 379 declined against 377), 1000 seeds under qemu-aarch64 0 differences; perturbed Kirby 64 (3 x 30 copies, `fuzz_cmds` 04 / 0b / e1 / dd) and Worms (3 x 30, 0b / e1) with `FUZZ_DONOR`: 0 mismatches; captured sets 1327 equal, 27 declined (Ocarina of Time); 3700-frame gates (`ref` against `armlet-hle`, `armlet` and `ARMLET_HLE_VERIFY=1`): Kirby 64, Worms, Donkey Kong 64 identical. After merging with the S2DEX 2.06 / 2.05 branch (one check, 315, replaces that branch's narrower 311 (length 3) and 312 (length 4); `f3_store_unmodelled` takes the addresses from the build's layout (`f2_layout`: input buffer, jump table, overlay descriptors), which Worms 2.08, Kirby 64 / Resident Evil 2 2.06 and Ocarina of Time 2.05 [Safe] (792-byte output buffers) share, so one check serves all): `mutcheck_seldl.sh` caught; `mutcheck_s206.sh` 300 .. 304, 308, 309, 310, 315 all caught (315 by the same 36 synthetic seeds as 311 was); captured sets 1459 equal / 0 mismatches (x86), Kirby 64, Ocarina of Time, Resident Evil 2 starts, Worms, Super Smash Bros., Conker, Donkey Kong 64 equal under qemu NEON and scalar; synthetic 1000 seeds each: Kirby 64 `load` 609 equal, Ocarina of Time `load` 604, Resident Evil 2 `s2` 609, 0 differ; perturbed copies (`fuzz_snaps`, `fuzz_cmds` with `FUZZ_DONOR`: Kirby 64, Ocarina of Time, Resident Evil 2, Worms) 7869 + 1596 equal, 0 mismatches (one perturbed Resident Evil 2 task does not finish on the JIT, with or without the check); 3700-frame gates (`ref` against `armlet-hle` and `armlet`): Kirby 64, Ocarina of Time, Resident Evil 2, Worms, F-Zero X (all three of its texts are LLE now), Donkey Kong 64 identical.

The oracle (`--oracle`) needs the code of the overlays and the other microcode in the snapshot's RDRAM; a task that did not load them has zeros there, which the JIT executes and the front-end (that takes an overlay for what its build says) does not: `FUZZ_DONOR=<snapshot>` adds those rows to a perturbed set (`tools/rsp2c/make_donor.py` builds the donor from a task and the static rows of the others).

## S2DEX 2.06 and 2.05 [Safe] (docs/ucode-s2dex2.md section 7; docs/ucode-f3dex2-dialects.md section 5.7)

Resident Evil 2's S2DEX start (its text is Kirby 64's: 2.06) and Ocarina of Time's loaded text (2.05 [Safe]: a pipe sync in front of every flush check, 792-byte output buffers). Desktop (x86_64) unless said, `GfxMode=hle` against the reference pairing.

| check | result |
|---|---|
| native share of the graphics tasks (`ARMLET_HLE_STATS`), 3700 frames, before -> after | Ocarina of Time 544 of 1472 -> 1471 (the first task of a run never is); Resident Evil 2 0 -> 4267 of 6464 (66 %; the 2196 declines are the tasks that `G_LOAD_UCODE` the F3DEX.Rej text, one in three of its S2DEX tasks, plus the first). 12 000 frames: Ocarina of Time 4159 of 4160, Resident Evil 2 12 544 of 18 880 |
| commands the two games run in S2DEX (all tasks of 12 000 frames, a counter per build and opcode) | Ocarina of Time: `G_BG_COPY` 526, `G_SETOTHERMODE_L` 526, `G_RDPHALF_1`, `G_LOAD_UCODE` (back), `G_DL`, syncs. Resident Evil 2: `G_BG_COPY` 11 741, `G_SETSCISSOR`, `G_MOVEWORD`, `G_RDPHALF_1`, `G_LOAD_UCODE`, the other-mode, combiner, fill and image commands and syncs. No sprite, rectangle, texture-load or 1-cycle background command in either |
| equivalence gates, 3700 frames, `ref` against `armlet-hle` and `armlet` (default): hash stream, state stream, 6 screenshots, audio | identical, both pairs: Ocarina of Time, Resident Evil 2, and (regression) Kirby 64, Worms Armageddon, Conker, Donkey Kong 64, Paper Mario, Super Mario 64, Super Smash Bros. |
| the same with `ARMLET_HLE_VERIFY=1`, `+ ARMLET_HLE_EXACT=1`, `ARMLET_HLE_POISON=1`, `ARMLET_DMEM_POISON=mask`, `=ring` (Ocarina of Time and Resident Evil 2) | identical hash, state, pictures and audio; `=all`: state, pictures and audio identical, hash differs by design. The S2DEX dead ranges are the 2.06 ones (the 792-byte buffers and the sync's eight bytes fit in the slack): no new range |
| `ARMLET_HLE_VERIFY=1`, 12 000 frames | Ocarina of Time 4159 tasks compared, 0 differences; Resident Evil 2 12 544, 0 |
| captured task starts, recorded JIT result: x86_64, aarch64 NEON and scalar under qemu | Ocarina of Time 55 (28 with the load) equal, 0 declined (the 27 that declined at the load before); Resident Evil 2's S2DEX starts 86 equal (a set of the first 30 tasks: 20 equal, 10 declined at the F3DEX.Rej load); all other sets (32 sets, 1440 snapshots) equal, 0 declined; identical on the three |
| synthetic tasks (`tools/hle/mksyn_ex2.py`: `load` on Ocarina of Time's rows, F3DZEX -> S2DEX 2.05 -> F3DZEX; `s2`, new, a task that starts with S2DEX, on Resident Evil 2's; `load` on Kirby 64's; scissor, render mode, sprites, 1-cycle and COPY backgrounds, texture loads including `LDTX_RECT` and an object loaded twice, two `G_SELECT_DL` that rewrite a jump table entry, undefined opcodes), JIT run from the same state, `tools/hle/synth_ex2.sh` | x86, 1000 seeds each: 604 / 609 / 609 equal, 0 differ, the rest declined (loops the random objects make, the declines of the build), 1 seed of the two `load` sets does not finish on the JIT; aarch64 NEON and scalar under qemu, 400 seeds each: 239 / 244 / 241 equal, 0 differ, the same declines (identical on both) |
| perturbed copies of the captured tasks (`tools/rsp2c/fuzz_snaps.py`, `FUZZ_DONOR` with the overlay rows: Ocarina of Time's 28 tasks with the load x 30 copies x 3 seeds = 2520 files, Resident Evil 2's 86 x 15 x 3 = 3870), `--oracle`, x86 | Ocarina of Time 187 equal / 0 mismatches / 2333 declined (a perturbed background object or list nearly always declines); Resident Evil 2 2860 equal / 0 / 1008 declined; 2 perturbed tasks of Resident Evil 2 do not finish on the JIT (both before and after) |
| aimed perturbations (`fuzz_cmds.py`: Resident Evil 2: `G_BG_COPY` 0a, `G_SETSCISSOR` ed, `G_RDPHALF_1` e1, `G_MOVEWORD` db, ff, f9, fc, e3, e2; Ocarina of Time: 0a, dd, e1, ef, e7, de; x w0lo, w0mid, w1lo, 8 copies per task: 45 sets, 20 232 files), `--oracle`, x86 | 14 643 equal, 0 mismatches, 5589 declined, 0 hangs |
| the same fuzz sets, 5 of them, under qemu NEON and scalar | identical to x86 (equal / declined counts), 0 mismatches |
| `G_BG_COPY` and `G_OBJ_RECTANGLE` closed forms against the 2.05 microcode (`tools/hle/test_s2dex2.c`; oracles regenerated from the text and overlay A / B of Ocarina of Time: `S2OUT=792 S2SYNC=1 S2RECT_ENTRY=0xbb0 S2RECT_DONE=0x640 S2RECT_CLIP=0x1a4 S2BG_ENTRY=0x704 S2BG_END=0x1a4`, recipes below), x86, 300 000 cases | 11 517 backgrounds drawn, 56 951 rectangles drawn, 0 failures except the 4 `G_BG_1CYC` cases that the translation cuts at the DMA wait of 0xfc8 (as for 2.08); the translation of the background-copy routine is, comment names apart, the Kirby 64 one; under qemu aarch64 60 000 cases, 2 of the same 4 |
| the same against the 2.06 oracles (Kirby 64's `G_BG_COPY`, Worms' rectangle and background) and Worms' `G_BG_COPY` | 200 000 cases each, 0 failures besides the same 4 |
| builds: `scripts/build.sh native`, `aarch64`, `rg35xxsp` (`RG35_TOOLCHAIN`), `--tools`, `--tools --scalar` | succeed; no warning in `src/` or `tools/`; the rg35xxsp module needs GLIBC_2.17 |
| x86 front-end time against the JIT (`armlet-native-replay --bench 200`, summed over the tasks) | Ocarina of Time's 28 tasks with the load 7.5 ms against 27.2 ms (3.63 times the JIT's speed; the JIT could not be compared before, the tasks declined), all 55: 11.6 against 40.0 ms (3.45); Resident Evil 2's 86 S2DEX tasks 0.6 against 0.6 ms (they are four to eight microseconds each: a scissor, a few other-mode commands and one background); Kirby 64 (unchanged) 10.9 against 32.7 ms (3.00) |

**Mutations** (`tools/hle/mutcheck_s206.sh`; `scripts/build.sh native --tools --hmut=N`; caught = more captured tasks mismatch or decline, or more synthetic seeds differ or decline, than with the unmutated build; the 27 captured Ocarina of Time tasks with the load, the 86 Resident Evil 2 tasks, one perturbed one,
200 + 400 synthetic seeds): 300 (the sync byte 0xe6) caught by 27 captured tasks and 117 seeds, 301 (the sync does not advance the pointer) 27 / 117, 302 (800-byte buffers) 27 / 18, 303 (no sync in the layout) 27 / 117, 304 (the rectangle's `G_OBJ_LOADTXTR` continuation pc off) by 200 more declined seeds, 308 (a sync after a rectangle outside the scissor) 34 seeds, 309 (a sync after
an object that is loaded already) 15, 310 (the entry of an undefined opcode taken from the display list buffer) by the perturbed Resident Evil 2 task (the front-end ran an undefined opcode 0x69 as `G_RDPHALF_1`, the microcode took its handler from the list), 315 (a `G_SELECT_DL` that rewrites the jump table; was 311) 36 seeds. 306 (any decline at a load marks the pointers) by the share:
Resident Evil 2 4267 -> 69 of 6464 graphics tasks. The mutants 258 (the `G_OBJ_LOADTXTR` continuation pcs, now derived from the build's table) and 250 .. 257 of the build switching are still caught by the 90 Kirby 64 tasks (`tools/hle/mutcheck_switch.sh`; 249 needs the perturbed sets, as before).

**What it found.** The equivalence shim (`tools/equiv/rsp_shim.c`) holds the masks of 32 ucodes at most: the table of `armlet-native-replay --masks` is at 34 entries with these two builds, so Resident Evil 2's S2DEX masks were dropped and its hash stream differed at the fifth task (now 64). Fuzzing found an S2DEX gap that is older than this work and
is closed by 310: an undefined opcode 0x54 .. 0x7f has its jump table entry (0x370 + 2 x opcode) in the display list buffer (0x418 ..), so its handler is whatever the list holds. Synthetic tasks with a `G_SELECT_DL` pair found 315's case (then 311). The first synthetic runs also produced two harness artefacts that are not front-end errors: a background object with a
random `imageLoad` field makes the microcode write a LOAD_TILE whose id byte is arbitrary (0xcf, 0xe4); the RDP parses it as a triangle or a texture rectangle and swallows the syncs behind it, whose stale bytes the relaxed compare of a task that switched builds (`hash_r`) can then not zero. The generator keeps the field to the two values a game writes
(`0xfff4`, `0x0033`). With the old generator, `load` seed 904 of Kirby 64 (devel a355089) ended the JIT after 1504 bytes while the front-end ran 10 773 commands: a `G_SELECT_DL` whose flags word lay over the jump table, now declined (mutation 315, section "G_SELECT_DL over the jump table" above).

## Rej: F3DLX.Rej, F3DFLX.Rej, F3DEX.Rej and F3DEX 2.03 (docs/ucode-rej.md; 2026-10-17)

Builds `8b0a` (F3DLX.Rej xbus 2.08, Excitebike 64) with the F3DEX.NoN xbus 2.08 text it loads (`f649`), `7de8` (F3DLX.Rej fifo 2.03), `c750` (F3DFLX.Rej fifo 2.03F), `33dc` (F3DEX fifo 2.03) of F-Zero X and `9512` (F3DEX.Rej fifo 2.06, Resident Evil 2).
Desktop (x86_64) unless said, `GfxMode=hle`, against the reference pairing, 3700 frames (62 s) unless said. **The closed forms are checked against the microcode itself**: `tools/hle/gen_rej_oracles.sh` makes rsp2c translations of the G_VTX and G_TRI handlers
of each text from its dump (the oracles are ROM-derived: git-ignored, in test-output/), `tools/hle/test_rej.c` (`tools/hle/build_test_rej.sh`) runs random DMEM images through the oracle and through the front-end's functions (`rej_vtx_xform`, `rej_tri_setup`,
the lighting) and compares every DMEM byte, the RDP output and the state; the rsp2c translations are oracles only, never production code.

| check | result |
|---|---|
| `test_rej` against the rsp2c oracles, x86 scalar, 3000 cases of each of the four Rej texts (G_VTX with 1 .. 64 vertices, odd counts, marked vertices, lit; G_TRI1 / G_TRI2 / G_QUAD with random records, wrapped rings, culling, flat and smooth) | 0 failures (xbus, fifo203, fifo206, flx) |
| the same under qemu-aarch64: NEON forms, and with `SCALAR=1` the scalar forms (3000 cases of each text) | 0 failures |
| NEON against scalar, 160 000 random vertex and triangle cases per text (`REJ_ONLY=form`, qemu) | identical (xbus, flx; 80 000 for the others) |
| captured tasks (`ARMLET_HLE_CAPTURE`, 3700 f), `armlet-native-replay`: Excitebike 64 / F-Zero X / Resident Evil 2 | 173 / 320 / 500 snapshots: all equal, 0 declined |
| synthetic tasks (`tools/hle/mksyn_rej.py`, `synth_rej.sh`; a captured task of the text supplies the microcode and the start state, `MKSYN_BASE=0x200000` for F-Zero X: the default base lies in its RDP ring; cases `vtx lit wild misc ring bad load`), 100 seeds each (44 for `bad`), x86 | 0 differ in all four Rej texts (the declines: `misc` 11 to 40 % by the `G_DMA_IO` to 0x8b0, which is over the vertex slots of every text but F3DFLX, `lit` of F3DFLX 56 % by its light descriptors, `bad` all but a few kinds) (`ring`: Excitebike's output ring wrapping; `load`: the G_LOAD_UCODE of the F3DEX.NoN text, 60 seeds equal; `bad`: commands no game writes, every seed declines by the guard it aims at) |
| synthetic, 30 seeds each, qemu-aarch64 NEON and scalar | 0 differ |
| perturbed copies of the captured tasks (`tools/rsp2c/fuzz_snaps.py` with `FUZZ_DONOR`, `--oracle`), x86 | 3 sets of 20 copies x 3 seeds of every 6th / 25th / 12th task of Excitebike 64 / Resident Evil 2 / F-Zero X: 813 equal, 0 mismatches after the fix of mutation 376 (before it: one mismatch in about 1500, see below), the rest declined (undefined opcodes, indices outside what a game writes, the poll limit), 0 hangs; qemu NEON and scalar, 90 files per game: 0 mismatches |
| captured tasks under qemu-aarch64, every second snapshot, NEON and scalar | Excitebike 64 87, F-Zero X 160, Resident Evil 2 250: all equal, 0 declined (both) |
| equivalence gates, `ref` against `armlet-hle`, `armlet` and `ARMLET_HLE_VERIFY=1`, 3700 f | Excitebike 64 (1663 of 1664 tasks native), Resident Evil 2 (8575 of 8576), F-Zero X (3391 of 3392): hash stream, state stream, 6 screenshots, audio identical; the verify runs 0 differences, 0 declined |
| `ARMLET_HLE_VERIFY=1`, 12000 f | Excitebike 64 5823 tasks, Resident Evil 2 25 151, F-Zero X 11 455: 0 differences; equivalence PASS each |
| `ARMLET_HLE_POISON=1` and `ARMLET_DMEM_POISON=mask`, 3700 f | the three games identical to `ref` (the dead ranges of `hle_rej.c` are dead) |
| regression gates (`ref`, `armlet-hle`, `armlet`, verify), 3700 f | Donkey Kong 64, Ocarina of Time, Paper Mario, Banjo-Kazooie, Conker, Kirby 64, Worms, Banjo-Tooie, Resident Evil 2: identical |
| x86 front-end time against the JIT (`armlet-native-replay --bench 60`, 3 runs) | Excitebike 64 13 us against 21 us (1.6 times); F-Zero X: F3DEX 2.03 3.1 to 3.2 times (241 us against 750), F3DLX.Rej 3.1 to 3.2, F3DFLX.Rej 2.8 to 3.0; Resident Evil 2 1.0 to 1.2 (tasks of 3 us: the fixed cost of the task dominates) |

**Mutations 360 .. 376 and 377 .. 396** (`tools/hle/mutcheck_rej.sh`: closed forms and NEON forms against the oracles; the front-end against the captured and the synthetic tasks, a mismatch, a decline the unmutated build does not have or the other way round counts):

| # | the deliberate error | caught by |
|---|---|---|
| 377 | the mark of a vertex from the x, y, z bits only (`& 0x0707`) widened / narrowed | oracle |
| 378 | marked vertices do not saturate (the extreme corners of the viewport) | oracle (the "extreme corner" vertex mode) |
| 379, 380 | y translation + 1 instead of + 2; the mask ~1 instead of ~3 | oracle |
| 381 | fog byte limit 0x7e00 | oracle |
| 382 | a negative guard-band ratio is not declined | oracle (ratio >= 0x8000) |
| 383, 384 | `sy >= 0x7ffd`; the sort tie of F3DEX2 | oracle |
| 388, 389, 390 | geometry bits, G_TEXTURE bytes and odd vertex numbers not declined in the triangle command | oracle (bad bits, bytes, numbers) |
| 396, 360, 361 | F3DFLX's flat alpha; the cull bits swapped; the two triangles of a command in the other order | oracle |
| 366, 367 | the ring wraps one command late / to the wrong address | oracle (ring) |
| 368 .. 371 | the same errors in the NEON form (mark mask, mark term, y translation, y mask) | oracle under qemu |
| 373, 374 | F3DLX.Rej 2.03's unsigned lighting: the lambert's multiplier, the mask of `cur` | oracle (fifo203) |
| 376 | the y fraction of the top vertex corrected in the rounding builds | oracle (records with y low bits) |
| 385, 386, 387, 375 | G_DMA_IO to RDRAM / into IMEM / over the microcode's own buffers / over the vertex slots not declined | synthetic `bad` (seeds 7, 18, 29, 40 of 11 kinds x 8 destinations), `misc` |
| 392, 393, 394, 395 | G_VTX end index, G_MODIFYVTX offset, G_CULLDL and G_BRANCH_Z vertex numbers not declined | synthetic `bad` |
| 362, 363, 364 | G_MODIFYVTX offset base 16 -> 0; G_BRANCH_Z reads the wrong record field; G_CULLDL compares 0x7ffc | synthetic `misc` |
| 391 | the geometry-mode guard of G_VTX | **not caught: an equivalent mutant.** The command that sets the mode (`G_GEOMETRYMODE`, `hle_f3dex2.c`) declines undefined bits before any vertex is seen; the guard in `rej_vtx` would only matter for a start state that has them (a snapshot taken after a text that does not clear them), which no captured task has |

**What the tests found** (all fixed before the commit):
- the signed guard-band ratio: the microcode multiplies the ratio as a signed lane, so 0x8000 and up is declined (382);
- the sort tie, the tile byte and the cull word differ from F3DEX2's (384, 360), and the second triangle of a command is not drawn first;
- 64 vertex slots of 20 bytes: the arrays of the F3DEX2 front-end held 32;
- F-Zero X's F3DFLX lighting setup normalises two vectors, stores the valid byte before testing it and the alpha is a table lookup for texture generation; F3DLX.Rej 2.03's `cur` is masked in the branch delay slot too (373, 374);
- a fuzzed F-Zero X task started with vertex slots of the previous (F3DEX 2.03) text: records with y low bits set, which the YQ builds' triangle code does not correct for (the F3DEX2 setup does): 376, and `test_rej` now makes such records;
- a synthetic `G_DMA_IO` over the vertex slots: the front-end took the junk records as its own (the slots are modelled in DMEM) and applied its own invariants: declined (375).

**F3DEX 2.03.** Registered as the 2.04 build's pcs with `zex = 1`, `gen204 = 1`. Synthetic tasks (`mksyn_ex2.py`, a captured `33dc` task as base, 100 seeds): clip zneg tri mix misc mtx 0 differences, wild 96 of 96 equal; `lit` cannot be tested (the lighting overlay is not in the snapshots, the JIT does not finish).

## F3DEXBG lit pass, NEON (2026-10-19)

`fbg_lit_pass` is NEON on aarch64 (`f3dexbg_fx_neon.h`, four vertices per vector; bounds in the file header); `fbg_lit_pass_scalar` is the reference. Conker, Tavern scene (rgdsplus), devel + this change.

| check | result |
|---|---|
| `tools/hle/build_test_f3dexbg.sh [CFLAGS] [vertex cases] [lit cases] [seed]`: random and edge-value records (saturating d, a distance of 0 / 2^k / 2^k - 1 / far, atten 0 / 0x0f / 0x10 / 0xff, colours 0 / 0x7f80 / any s16, directions +-128 << 8 and any s16, normals +-128, 1 .. 11 lights, positional or not, 1 .. 40 vertices, texgen, flag byte of either sign, equal or different lane groups), NEON `fbg_lit_pass` against the scalar pass of an unmutated copy of `f3dexbg_fx.c` (symbols renamed), whole records compared | qemu-aarch64: 12 000 000 + 12 000 000 cases; rgdsplus (A55, native): 30 000 000 cases; 0 failures |
| mutation of the NEON form only (`-DFBG_NEON_MUTATE=n`: reciprocal shift, falloff clamp, distance carry and cap, lambert rounding and clamp, the `macf` parity bit, fraction mask, D2 = 0, the product term, view-space shift, lane pattern, the partial last block, saturating add, the second lambert mask) | 1 .. 14 and 16 all caught (3 to 12 754 failures in 20 000 cases; 5 (lambert rounding) and 9 (D2 = 0) are the rarest) |
| `-DARMLET_HLE_MUTATE=100 .. 114` (both passes mutated; the reference is the unmutated copy) | all caught, 63 to 19 868 failures in 20 000 cases (111 lane group, 112 top-light mask and 114 w lane of the distance included) |
| `scripts/build.sh aarch64 --tools`, `armlet-native-replay` under qemu-aarch64 (and the x86_64 scalar build), 206 Conker tasks captured on the rgdsplus (`ARMLET_HLE_CAPTURE`, every 2nd task of the Tavern scene, JIT only) + 86 of earlier captures (`test-output/f3dex2-204/snap/conker`, `reg4-wt/snap/f204/conker`) | 292 of 292 equal on both; the same 206 with the build before the change equal |
| `tools/hle/mutcheck_dialects.sh` with `TARGET=aarch64 RUNNER="qemu-aarch64-static -L /usr/aarch64-linux-gnu"`, `MUTS` 100 .. 120, the 292 snapshots | caught: 100 .. 110, 113, 115 .. 120 (160 to 206 snapshots); not caught: 111 (the two lane groups are equal in the captured tasks), 112 (no top-light lambert term above 0x7fff in this scene: the x86 scalar build misses it on the same snapshots, the earlier 289 + 313 caught it), 114 (w lane zero); those three are caught by the random test above |
| `ARMLET_HLE_VERIFY=1` on the rgdsplus, Conker slot-1 state, 40 s (191 tasks compared), new and old build | identical: 1 task (96) differs in 1 byte (DMEM 0xbb4, native 0x0f, JIT 0x0e) with both builds. **Cause (2026-10-19):** 0xbb4 is the clip-flag word of vertex slot 31; the task's last lit `G_VTX` loads 5 vertices from slot 27, so the pair's phantom partner comes from the input buffer's first bytes (the display list chunk, `k0 - 168` in RDRAM), which `fbg_cmd_vtx` read from stale DMEM (F3DEX2's `cmd_vtx` had this since 2.04). Fixed (`dl` argument); mutation **121** (chunk address 8 off) is caught by the captured task (snapshot 95 of a 285-task capture, every task) and by the synthetic case `bgph` (`synth_ex2.sh REPLAY CONKER-SNAP 100 bgph`: 91 of 100 seeds differ before the fix, 98 equal after; seeds 6 and 94 differ in one colour byte of a lit vertex for another reason, not looked into: random lights/vertices also differ in even-count loads). After: 285 + 206 Conker snapshots equal; VERIFY on the rgdsplus from the slot-1 state 90 s (831 tasks) and from a cold boot 70 s (1087 tasks): 0 differences |

## Regenerating the microcode oracles

The closed forms of S2DEX 2.08 (`s2dex2_bg.c`, `s2dex2_rect.c`) and of F3DEXBG's lit vertices (`f3dexbg_fx.c`) were derived from the microcode and are checked against the microcode itself, besides the replays and verify runs above.
`tools/rsp2c/` is the one directory of this tooling: `rsp2c.py` (the translator, below), `fuzz_snaps.py` / `fuzz_cmds.py` / `ddmin_snap.py` (perturbed copies of captured task snapshots, the same aimed at one command, and their reduction; the section above) and `replay_each.sh` (one `armlet-native-replay` per snapshot with a time limit, for the perturbed ones).
`tools/rsp2c/rsp2c.py` translates the instructions of a handler (from an IMEM image, one C statement per instruction, on the exact vector-unit library `tools/hle/ref_vu.h`) into a function that does what the RSP interpreter
does. Those translations are ROM-derived and **not** part of the tree (`*_gen.inc`, `gen_*.inc` and `tools/hle/oracle*/` are git-ignored); they are regenerated from an IMEM image taken with `HLE_DUMP_IMEM=<file>` (the front-end writes
IMEM at the end of a task; for S2DEX the overlay that the handler lives in must be resident: A for the sprites, B for the backgrounds; for F3DEXBG the lighting overlay):

```
# S2DEX 2.08 sprite rectangle (Worms Armageddon): overlay A is IMEM [0x6c8, 0xe00); entered after the DMA of the sprite, done at 0x648, outside the scissor at 0x1ac
tools/rsp2c/rsp2c.py --name rect_gen --entry 0xba8 --stop 0x648 --stop 0x1ac --seg s2_ovA.img 0x6c8 0xe00 --out rect_gen.inc
# S2DEX 2.08 background: overlay B is IMEM [0x6c8, 0xe00)
tools/rsp2c/rsp2c.py --name bg1cyc_gen --entry 0x6fc --stop 0x154 --stop 0x1ac --stop 0x64c --stop 0xfc8 --seg s2_ovB.img 0x6c8 0xe00 --out bg1cyc_gen.inc
# S2DEX 2.05 [Safe] (Ocarina of Time): IMEM image = the text at 0x80 (3912 bytes) with the low part (RDRAM text + 0xf80, 152 bytes, at IMEM 0; the row at text + 0x1018, 384 bytes: its bytes 0xa0 .. 0x180 at the same IMEM addresses); overlay B image = the same with the overlay copied to 0x6d0 (RDRAM text + 0x1198, 0x728 bytes; RSPT_DUMP_RDRAM=1 RSPT_DUMP_CALLS=<n> RSPT_DUMP=<prefix> of tools/equiv/rsp_shim.c dumps RDRAM)
tools/rsp2c/rsp2c.py --name bgcopy_gen --entry 0x224 --stop 0x154 --stop 0x1a4 --ext "0x154:bgc_seg(x, r)" --ext "0x644:bgc_flush(x, r)" --seg oot_ovA.img 0x208 0x550 --out bgcopy_gen_oot.inc
tools/rsp2c/rsp2c.py --name rect_gen --entry 0xbb0 --stop 0x640 --stop 0x1a4 --seg oot_ovA.img 0x6d0 0xe00 --out rect_gen_oot.inc
tools/rsp2c/rsp2c.py --name bg1cyc_gen --entry 0x704 --stop 0x154 --stop 0x1a4 --stop 0x644 --stop 0xfc8 --seg oot_ovB.img 0x6d0 0xe00 --out bg1cyc_gen_oot.inc
# F3DEXBG.NoN 2.08 lit G_VTX (Conker): matrix concatenation 0x080, lighting overlay 0x2d8, vertex handler 0x6d8, DMA and wait code 0xfc8
tools/rsp2c/rsp2c.py --name bg_vtx_gen --entry 0x6d8 --stop 0x194 --stop 0x224 --seg mtx.imem 0x080 0x0f4 --seg ov.imem 0x2d8 0x6d8 --seg ov.imem 0x6d8 0x994 --seg ov.imem 0xfc8 0x1000 --out bg_vtx_gen.inc
```

`tools/hle/build_test_s2dex2.sh` (S2OUT / S2SYNC / S2RECT_ENTRY / S2RECT_DONE / S2RECT_CLIP / S2BG_ENTRY / S2BG_END say which build the oracles are of; the defaults are Worms') takes them as `S2RECT_ORACLE=rect_gen.inc` and `S2BG_ORACLE=bg1cyc_gen.inc`: random DMEM images, the closed form against the translation (the whole DMEM, the output pointer, drawn / outside the
scissor), also under qemu-aarch64 (without `HOST=1`). `S2RECT_VU=1` adds the instruction-by-instruction kernel that `s2dex2_rect.c` replaced as a second oracle (`tools/hle/oracle/s2dex2_rect_vu.c`, kept locally, ignored by git). A mutation
of a closed form that the replays do not catch must be caught by this test (`tools/hle/mutcheck_dialects.sh` with `S2RECT_ORACLE` set does both). The lit-vertex translation was compared with `f3dexbg_fx.c` by one-off harnesses on
captured and mutated command-entry states (results in the table above); they are not kept, the corpora being ROM-derived.

## Ucode id over the whole 4 KiB, three derived F3DEX2 builds, the status poll time-out (2026-10-16)

`ucode-derive.md` ("Registered") has the story; this is the evidence. Desktop x86_64, `GfxMode=hle`, against the reference pairing, devel 22b2d64 plus this branch.

| check | result |
|---|---|
| ids: every registered id recomputed from the dumps (FNV of the 4 KiB); collisions | 10 ids changed, 3 duplicate entries merged (Dr. Mario 64 / RR64 into `d2d1`, Pokemon Puzzle League into `0472`, Quest 64 into `208a`); `hle_core.c` logs an error for two entries with one id (checked with a deliberate duplicate); Rocket has Donkey Kong 64's id |
| 3700-frame gates, `ref` against `armlet-hle` and `armlet` (hash, state, 6 screenshots, audio) | identical: Banjo-Tooie, LEGO Racers, Star Wars Racer, Blues Brothers 2000, Harvest Moon 64, Tonic Trouble, Rocket, Gauntlet Legends, and (regression) Donkey Kong 64, Ocarina of Time, Paper Mario, Pokemon Snap, Banjo-Kazooie, Super Mario 64, Conker, Worms, Kirby 64, Super Smash Bros., Buck Bumble, Resident Evil 2, Diddy Kong Racing, Indiana Jones, F-Zero X |
| the seven new games also with `ARMLET_HLE_VERIFY=1`, `+ ARMLET_HLE_EXACT=1`, `ARMLET_HLE_POISON=1`, `ARMLET_DMEM_POISON=mask`, `=ring`, 3700 f | identical to the reference (hash included); `=all`: state, pictures, audio identical, hash differs by design. No dead range added or removed |
| native share, 3700 f | all but the first task of each of the seven games; Gauntlet Legends 2 of 3392 |
| `ARMLET_HLE_VERIFY=1`, 12 000 f | 0 differences: Banjo-Tooie 5247 tasks, LEGO Racers 9535, Star Wars Racer 7743, Blues Brothers 12 031, Harvest Moon 18 047, Tonic Trouble 9087, Rocket 5887 |
| Gauntlet Legends, verify | before the time-out rule 209 native, 207 differences; after: 2 native, 0 differences. Mutation 316 (rule off): 7 differences in 700 frames |
| captured task replays (`armlet-native-replay`), x86 | 2165 equal, 0 mismatches, 308 declined (Indiana Jones by design): 411 snapshots of the new games, the earlier f204 / rare / indy sets (snapshots of the ten renamed builds had their header ids rewritten) |
| the same, qemu-aarch64, NEON and scalar, a sample of 533 | 533 equal each, 0 mismatches |
| synthetic tasks (`synth_ex2.sh`, 1000 seeds per case: clip zneg tri mix misc lit wild mtx; `bad` declines by design) on bases of `9875` (Banjo-Tooie, LEGO Racers, Star Wars Racer), `0bbe` (Blues Brothers, Harvest Moon), `d8bd` and Rocket; lit bases need the overlay rows (`addrows`: text + 8 KiB behind it from the dump added to the snapshot) | 0 differences everywhere; qemu NEON and scalar, 200 seeds per case on Blues Brothers and Tonic Trouble bases: 0 differences. **Before `bz_z`: `misc` 149 of 1000 differed on the 2.06 base** |
| `fuzz_snaps.py` with `FUZZ_DONOR` (the base with overlay rows), 9 seeds x 30 copies per task, `--oracle` | about 126 000 perturbed tasks over the seven sets: 0 mismatches, 0 hung (equal 600 to 2200 of 2400 per seed, the rest declined) |
| `fuzz_cmds.py`, opcodes 01 02 03 04 05 06 07 d9 da x w0lo / w0mid / w1lo, 8 copies | 25 000 equal-or-declined, 0 mismatches |
| mutation checks of the new code | 316 (time-out rule): Gauntlet verify differs again; 317 (`bz_z`): `misc` 36 of 300; 318 / 319 / 320 (reject masks of the three records): `clip`/`tri` 43 / 33 of 300 (318, 319), `clip` / `zneg` / `wild` 3 / 3 / 13 of 1000 (320) |
| `tools/derive_check.sh` (leave-one-out, now with the three records and `bz_z`) | no unexplained mismatch; mutation check caught |
| builds | native, aarch64, rg35xxsp (glibc 2.17) clean; `--tools` native and aarch64 (NEON and scalar) |
| recount, 64 games | mean JIT share 34.4 % to 23.4 %, `ucode-coverage.md` |

A lesson for the harness: `armlet-native-replay --oracle` on a synthetic task whose overlay is missing from the snapshot can make the JIT loop for ever; always run it under `timeout` (`synth_ex2.sh` and `replay_each.sh` do).

## The C derivation, the probe and the ucode cache (2026-10-17; docs/design.md "The probe and the ucode cache", docs/ucode-derive.md "The C port")

Desktop x86_64, `GfxMode=hle`, against the reference pairing, branch `ucode-probe` on devel 46537eb. Dumps: the survey's (64 games, 797 (text, data) pairs, ROM-derived, not in git).

| check | result |
|---|---|
| C derivation against `tools/derive_build.py` (`tools/derive_c_check.sh DUMPS`): the 54 distinct texts, the 786 (text, data) pairs with the production shortcut, the 28 leave-one-out derivations, the mutation check | 0 differences in family, verdict, counts, composite and every field of every record; ASan + UBSan + leaks clean |
| `derive_refs.c` against the tables (`armlet-probe-replay --sync`) and against the dumps (`derive_refs_gen.py --check`) | 14 F3DEX2 references in step; file up to date |
| `tools/hle/mutcheck_ucache.sh` (`test_ucache.c`: round trip, replacement, flipped bit, truncation, damaged header, other tool version, 512-entry bound, a write that fails half way, two processes, unwritable path, disabled) | passes; 345 (entry checksum), 346 (bound), 347 (tool version), 349 (in-place write) each fail it. The test found a real bug on the way: a store after another process's write overwrote that process's entry |
| the probe on dumps, no game (`armlet-probe-replay --force`, every distinct text): F3DEX2 texts | 10 of 10 registered texts PASS (28 canned tasks, 27 or 28 equal, 0 differ), the derived record equal to the registered one in all; S2DEX texts (Kirby's, RE2's, Worms') derived EXACT / NEAR, no build can be made (unsupported); Conker (BG), Chameleon Twist 2's 2.05, F-Zero X's 2.03, the Rej / LX2 texts: NONE, not probed; F3DEX 1.x / F3D texts: classified |
| in the game, `ARMLET_PROBE=on ARMLET_PROBE_FORCE=1 ARMLET_PROBE_CHECK=1`, 1800 frames, all 27 games whose ucode is an F3DEX2-family text | PASS and derived record == registered, 22 games: Donkey Kong 64, Rocket, Ocarina of Time, Paper Mario, Pokemon Stadium 2, Pokemon Snap, Buck Bumble, Cruis'n World, Kirby 64, Super Smash Bros., 007 TWINE, Hot Wheels, Hydro Thunder, Mario Golf, Mario Tennis, Ogre Battle 64, WinBack, Banjo-Tooie, LEGO Racers, Star Wars Racer, Harvest Moon 64, Tonic Trouble; **Gauntlet Legends: inconclusive** (20 of 28 canned tasks: the JIT's status poll time-out has dropped, as in the 2026-10-16 finding); Chameleon Twist 2, Conker: NONE; Resident Evil 2, Worms: S2DEX, unsupported. The game's RDRAM never changes (`ARMLET_PROBE_CHECK`: 0 errors) |
| `ARMLET_PROBE_NONE_X=10` on Chameleon Twist 2's F3DEX 2.05 (NONE: 2 unexplained, 3 missing words) | the probe **FAILs** it: 4 of 28 canned tasks differ (clip/0: the clipper's output ring, `SP_MEM_ADDR`, the RDP stream from byte 2). Its 3839 real tasks verify clean ([ucode-derive.md](ucode-derive.md)), the canned clip cases do not: the NONE is right. It was not latched |
| 3700-frame gates, `armlet-hle` (probe off) against `ref` | identical (hash, state, 6 pictures, audio): Donkey Kong 64, Ocarina of Time, Kirby 64 |
| 3700-frame gates, `armlet-hle` + `ARMLET_PROBE=on ARMLET_PROBE_FORCE=1 ARMLET_PROBE_LATCH=1 ARMLET_HLE_VERIFY=1 ARMLET_PROBE_CHECK=1` (the derived leave-one-out build replaces the registered one after its probe passed), against `ref` | identical, `ARMLET_HLE_VERIFY` **0 differences** in the tasks compared: Donkey Kong 64 (1663), Ocarina of Time (1471), Kirby 64 (1791), Paper Mario (3583), Pokemon Snap (2111), Mario Golf (1791), Buck Bumble (703), Banjo-Tooie (1151), Harvest Moon 64 (5567), Tonic Trouble (1599); the derived primary references differ from the hand-written ones' (Donkey Kong 64 from the .NoN 2.07, Buck Bumble from the 2.04H ...) and the results are the same |
| Chameleon Twist 2 with `ARMLET_PROBE_NONE_X=10 ARMLET_PROBE_LATCH=1 ARMLET_HLE_VERIFY=1` (`EXTRA_MASKS` for its dead ranges), 3700 f | the probe fails, nothing is latched, the run is identical to `ref` |
| a first version of the probe that wrote its data into the tail of the game's RDP output ring | Donkey Kong 64's audio diverged from call 84 (the sandbox's undo wrote the private buffer's bytes into the game's RDRAM): found by the gates, fixed by pointing `RSP::rsp.RDRAM` at the private RDRAM too, shown by `ARMLET_PROBE_CHECK` |
| `armlet-probe-replay --task SNAP` (a captured Donkey Kong 64 task run by the JIT before and after the canned tasks of the registered build) | the same end state, RDRAM, registers (4 snapshots, 2 rounds) |
| mutation checks (`tools/hle/mutcheck_probe.sh DUMPS`; `scripts/build.sh native --tools --hmut=N`), 10 registered texts, `--force` | unmutated: 10 PASS. **340** (reject mask flipped): 10 FAIL. **341** (`bz_z` inverted): 2 FAIL (Ocarina of Time and F3DEX 2.06, the two builds with zex and without gen204: where it matters), 8 PASS as they must. **342** (`G_TRI1` pc + 8), **343** (zex), **344** (RDP buffer size - 8): 10 FAIL each. Near-match tolerance: **350** (shade base + 1 level in the native stream) 10 FAIL strict, 10 PASS tolerant; **351** (an edge bit) 10 FAIL either way; **352** (vertex colour + 1) 9 FAIL strict, 9 PASS tolerant (the tenth, Buck Bumble's, has the colour word in its dead range 0x418 .. 0x890); **353** (+ 3) fails either way |
| probe time | desktop x86_64: derivation 1.2 to 3.3 ms (Ocarina of Time: more references explain it), 28 canned tasks 4.5 to 6.5 ms offline, 5 to 19 ms in a game (JIT compiles included). `qemu-aarch64` (about 10 times slower than the desktop, so a pessimistic proxy for an A53): 12.5 to 20 ms and 72 to 75 ms. The canned tasks are spread over 7 graphics tasks (4 per task) |
| builds | native, aarch64, rg35xxsp (glibc 2.17) clean, `--tools` native and aarch64; the module exports the six API symbols |

Not done here: the background validator (probe pass + real-task matches), the latch policy and the device timing (the next steps of the plan); the aarch64 probe on a device (only the qemu proxy); F3DEX 1.x (Donald Duck's `efe4b8f3`, NONE) and F3D are classified, not probed.

## S2DEX 2.x data-driven, its derivation and probe (2026-10-17; docs/ucode-s2dex2.md section 8, docs/ucode-derive.md "S2DEX records that run", docs/design.md "The probe and the ucode cache")

Desktop x86_64 unless noted, branch `s2dex-datadriven` on devel abdca70. The front-end's `G_OBJ_LOADTXTR` continuation pcs moved into the record (`ldtx_spr` / `ldtx_rect`, the same values as before); `dbuild_make` builds S2DEX records;
the probe has five S2DEX cases and probes the texts a `G_LOAD_UCODE` loads (keyed by the load key; `loaded_build` takes a latched build first).

| check | result |
|---|---|
| leave-one-out (`tools/derive_check.sh`), 77 fields | the three S2DEX records equal to the hand-written ones (Worms 2.08 EXACT from 2.06, Kirby 64 / Resident Evil 2 2.06 NEAR from 2.08, Ocarina of Time 2.05 [Safe] EXACT from 2.06); no unexplained mismatch anywhere; mutation check caught |
| `tools/derive_c_check.sh` (54 texts, 28 leave-one-out, 6 mutants, 786 pairs) | 0 differences; `derive_refs.c` regenerated and up to date; `armlet-probe-replay --sync` 14 references in step |
| `tools/hle/mutcheck_ucache.sh` (with a new S2DEX record round trip) | passes; 345, 346, 347, 349 caught |
| the probe on the dumps, `--force` (`armlet-probe-replay`), the four S2DEX texts (Worms, Resident Evil 2 task starts; Kirby 64, Ocarina of Time loaded) | 4 PASS, 30 canned tasks each, 30 equal, 0 declined, the derived record equal to the registered one in all four (before: "no build can be made") |
| the same, the 10 F3DEX2 texts of `mutcheck_probe.sh` | 10 PASS, records equal; mutations 340 .. 353 as before (340, 342, 343, 344 10 FAIL; 341 2 FAIL; 350 / 352 pass tolerant, 351 / 353 fail) |
| **probe mutations** (`tools/hle/mutcheck_probe_s2.sh DUMPS`; `--hmut=N`) | unmutated 4 PASS. **400** (LDTX_RECT continuation + 8): 4 FAIL (every `s2ldtx` case declines: "no canned task of the case completed"). **401** (`out_bytes` + 8): 4 FAIL (4 to 30 tasks differ). **402** (`pipe_sync` inverted): 4 FAIL (30 differ). **403** (the dialect handlers from 0x184 on 8 lower): 4 FAIL (30 declined) |
| 3700-frame gates, `ref` against `armlet-hle` and `armlet` (default) | identical (hash, state, 6 pictures, audio): Worms, Kirby 64, Ocarina of Time, Resident Evil 2, Yoshi's Story and Pokemon Puzzle League (S2DEX 1.x, JIT), Donkey Kong 64, Paper Mario, Banjo-Tooie, F-Zero X, Excitebike 64 |
| `ARMLET_HLE_VERIFY=1`, 3700 and 9000 frames, against `ref` | identical; 0 differences in 2879 / 7487 (Worms), 1791 / 4415 (Kirby 64), 1471 / 3199 (Ocarina of Time), 8575 / 19199 (Resident Evil 2) tasks |
| `ARMLET_PROBE=on ARMLET_PROBE_FORCE=1 ARMLET_PROBE_LATCH=1 ARMLET_HLE_VERIFY=1 ARMLET_PROBE_CHECK=1`, 3700 frames, against `ref` | identical, verify 0 differences. Worms: derived EXACT (from the 2.06 reference), record equal, probe PASS 30/30, latched. Resident Evil 2: NEAR (from 2.08), equal, PASS, latched (its loaded F3DEX.Rej: NONE, no probe). Kirby 64: the task's 2.04H and both loaded texts (S2DEX 2.06, NEAR from 2.08; the 2.04H text loaded back) equal, PASS, latched; the latched S2DEX build ran 6720 loads. Ocarina of Time: F3DZEX, its reloaded text and the loaded S2DEX 2.05 [Safe] (EXACT from 2.06) equal, PASS, latched (509 loads on the latched S2DEX build) |
| captured task starts, recorded JIT result, x86 and qemu-aarch64 NEON / scalar | Kirby 64 90, Ocarina of Time 55, Worms 61, Resident Evil 2 S2DEX starts 86 and first tasks 40, Donkey Kong 64 55: all equal on the three, 0 declined; the other captured sets on x86 equal (Worms' sets were captured under the old ucode id `b228360be43c0447` and are replayed with the header's id set to `fa171b4489d45ef2`; Conker, Blast Corps and a few others are under old ids too and were not replayed) |
| synthetic (`synth_ex2.sh`): Ocarina of Time `load`, Resident Evil 2 `s2`, Kirby 64 `load` | x86, 1000 seeds each: 604 / 609 / 609 equal, 0 differ (as before); qemu NEON and scalar, 400 each: 239 / 244 / 241 equal, 0 differ |
| perturbed copies (`fuzz_snaps.py`, seed 1: Ocarina of Time 840 with its donor, Resident Evil 2 1290, Kirby 64 900, Worms 915), `--oracle`, x86 and qemu NEON / scalar | 54 / 950 / 75 / 657 equal, 0 mismatches, 0 hangs, identical on the three |
| `build_test_s2dex2.sh` against the rsp2c oracles (2.08 rectangle, background, copy: 200 000 cases x86, 20 000 qemu; 2.05 [Safe]: 100 000 x86) | 0 failures except the known `G_BG_1CYC` cases the translation cuts at the DMA wait 0xfc8 (4 and 3) |
| S2DEX front-end mutations | `mutcheck_s206.sh` 300 .. 304, 308, 309 caught, 310 caught with the perturbed Resident Evil 2 task; `mutcheck_switch.sh` 250 .. 258 caught on the 90 Kirby 64 tasks (258: the continuation pcs replaced by Worms' values, 38 declined); `mutcheck_seldl.sh` 315 caught; `mutcheck_dialects.sh` 37, 38, 39, 44 caught on the captured sets, 35 and 36 on 216 earlier Worms snapshots |
| builds | native, aarch64, rg35xxsp (glibc <= 2.17) clean |

## F3DEX 1.x data-driven, its derivation and probe (2026-10-17; docs/native-f3dex.md section 3, docs/ucode-derive.md "F3DEX 1.x records that run")

Desktop x86_64 unless noted, branch `f3dex1-datadriven` on devel bc582ed. The F3DEX 1.x front-end runs from the build record (`struct f3dex1_build`: layout, handler pcs, reject mask, NoN / LX / G_CULLDL
form) and dispatches through the build's own handler tables; the derivation gives that record; `dbuild_make` builds it; the probe has F3DEX 1.x cases.

| check | result |
|---|---|
| captured task starts (Banjo-Kazooie 45, Mario Kart 64 28, Star Fox 64 28), recorded JIT result: x86, qemu-aarch64 NEON and scalar | 101 of 101 equal on the three |
| perturbed sets (`f3dex2-204/fzfix`), `--oracle`, the new front-end against the previous one (devel) | identical per snapshot: `bk` 810 (63 equal, 747 declined), `bk_L` 830, `bk_M` (the 3000 before the time limit); `mk2_Q`: the 175 that finish in the time limit (most of its tasks run the JIT for minutes); qemu NEON and scalar on `bk`: identical to x86 |
| front-end guard mutations on the perturbed BK sets (`--hmut=N`) | 227: 183 differ, 238: 1, 239: 1 (caught); 243 / 244 (the RDP ring guards): no F3DEX 1.x set reaches them (as before; their evidence is other families' sets) |
| leave-one-out (`tools/derive_check.sh`), 65 fields of F3DEX 1.x | 8 of 9 records equal (7 EXACT: 1.21, NoN 1.22, 1.23, F3DLX 1.23, 1.21, F3DLX.NoN 1.21, 1.23; Turok's 1.00 NONE, 12 unexplained, record equal); F3DEX 0.95: NONE, 4 expected differences (G_PERSPNORM, G_QUAD, its CULLDL and the form: in no other reference); no unexplained mismatch; mutation check: the 8 F3DEX2 edits and the 5 new F3DEX 1.x edits all change the result |
| `tools/derive_c_check.sh` | `derive_refs.c` up to date (generator version 2); 54 texts, 28 leave-one-out, 11 mutants, 786 pairs: 0 differences; ASan + UBSan over the 59 distinct texts clean; mean 2.7 ms per text, all families |
| `armlet-probe-replay --sync` | 23 references (14 F3DEX2 family, 9 F3DEX 1.x) in step with the tables |
| the probe on the dumps, `--force` (every distinct text) | F3DEX 1.x: the 7 EXACT texts (9 ids) PASS, 28 of 28 canned tasks equal, derived record equal to the registered one; 0.95 and Turok NONE (not probed; `--none-x 80`: Turok PASS 26 of 26, 0.95 FAIL: every case declines without its G_PERSPNORM); F3DEX2 family unchanged (14 PASS, records equal) |
| the unregistered candidates on the dumps | Donald Duck's F3DEX.NoN 1.23 `efe4`: EXACT, PASS 28 / 28; Iggy's Reckin' Balls' F3DEX.NoN 1.22 `dff3`: NEAR (14 words guarded by geometry bit 0x4000), PASS 28 / 28; both through the cache (format 3) on a second run, latched |
| **probe mutations** (`tools/hle/mutcheck_probe_f1.sh DUMPS`; `--hmut=N`), the 7 registered EXACT texts (`--force`) and the 2 candidates | unmutated 9 PASS. **404** (the reject mask's near bits flipped): 9 FAIL. **405** (NoN inverted), **406** (G_TRI1 pc + 8), **407** (vertex table 2 lower), **408** (LX inverted), **409** (G_CULLDL form inverted): 9 FAIL each. 404 needed the case of a triangle of three second-of-pair vertices with only bit 0x40 in common (the NoN builds' sign of w): culls alone do not tell the NoN mask from the full one |
| the existing probe mutations | `mutcheck_probe.sh` (340 .. 353) and `mutcheck_probe_s2.sh` (400 .. 403) as before; `mutcheck_ucache.sh` passes (with an F3DEX 1.x record round trip), 345 .. 349 caught |
| 3700-frame gates, `ref` against `armlet-hle` (`hle`), `armlet` (`def`), `ARMLET_HLE_VERIFY=1` (`ver`) and `ARMLET_PROBE=on ARMLET_PROBE_FORCE=1 ARMLET_PROBE_LATCH=1 ARMLET_HLE_VERIFY=1 ARMLET_PROBE_CHECK=1` (`probe`) | **97 of 97 identical** (hash, state, 6 pictures, audio): Banjo-Kazooie, Glover, Bomberman Hero, Quest 64, Star Fox 64, Wetrix, Quake, Toy Story 2, Mystical Ninja, SpaceStation Silicon Valley, Pokemon Puzzle League, Earthworm Jim 3D, Mario Kart 64, Wipeout 64, Clay Fighter SC, Iggy's Reckin' Balls, Penny Racers, Turok, A Bug's Life, Yoshi's Story (5 runs each), Donald Duck (hle / def / ver / probe without force), and Donkey Kong 64, Ocarina of Time, Paper Mario, Kirby 64, Worms Armageddon, Excitebike 64 (hle, def) |
| verify in those runs | 0 differences everywhere (e.g. Wipeout 64 7927 tasks, Clay Fighter 3583, Earthworm Jim 3D 3455, Bomberman Hero 2943, A Bug's Life 2495); natively run share as before (Glover 4267 of 5376: the XBUS tasks; SpaceStation 1384 of 2368: G_LOAD_UCODE; Pokemon Puzzle League 241 of 256) |
| the probe in those runs | every registered F3DEX 1.x text the game runs derived EXACT, record equal to the registered one, PASS 28 / 28, latched, then verified (0 differences); Mario Kart 64 (0.95) and Turok: NONE, not probed. **Donald Duck**: `efe4` EXACT, PASS, latched: 1727 of 1728 tasks native, 0 differences, identical to `ref`. **Iggy's Reckin' Balls**: `dff3` NEAR, PASS, latched, but all 62 of its tasks decline: the game sets geometry bit 0x4000, the feature the 14 guarded words implement |
| 9000 frames, `ver` (Banjo-Kazooie, Mario Kart 64, Toy Story 2, Turok) and Donald Duck's latched build | identical to `ref`; 0 differences in 3967 / 4159 / 4159 / 4415 tasks, Donald Duck 4351 of 4352 native, 0 differences |
| builds | native, aarch64, rg35xxsp (glibc <= 2.17) clean |

## F3D data-driven, its derivation and probe; GoldenEye registered (2026-10-17; docs/ucode-f3d.md section 15, docs/ucode-derive.md "F3D records that run")

Desktop x86_64 unless noted, branch `f3d-datadriven` on devel 206ebe7. The F3D front-end runs from the build record (`struct f3d_build`: layout, handler pcs, masks, attributes) and dispatches through
the build's own handler tables; the derivation gives that record; `dbuild_make` builds it; the probe has F3D cases; GoldenEye 007's 2.0G text is registered with its four-triangle command.

| check | result |
|---|---|
| captured task starts (SM64 41, Pilotwings 24, Wave Race 24), recorded JIT result: x86, qemu-aarch64 NEON and scalar | 89 of 89 equal on the three |
| perturbed sets (`f3dex2-204/fzfix`), `--oracle`, the new front-end against the previous one (devel) | identical per snapshot: `sm64` 802 (102 equal, 700 declined; 18 hit the time limit on both), `pw2_Q` 1512 (47 equal, 1465 declined), `wr2_Q` 1512 (155 equal, 1357 declined); the final build (with G_TRI4, GoldenEye registered) the same per snapshot on all three; qemu-aarch64 NEON and scalar (the build before G_TRI4): `pw2_Q`, `wr2_Q` identical to x86 (1512 each), `sm64` identical on the 802 that finish on both; the final build: the 143 below identical on x86, NEON, scalar |
| front-end guard mutations on the 143 F3D files of the perturbed sets that once differed (`--hmut=N`) | 228: 31 differ, 229: 26, 236: 6, 238: 1, 242: 1 (caught); 243 / 244 (the RDP ring guards): none, as on devel (their evidence is other families' sets) |
| leave-one-out (`tools/derive_check.sh`), 70 fields of F3D | every verdict NONE (each build has code no other reference has), records equal for SM64, Pilotwings, Mischief Makers, Tetrisphere; Wave Race 2 expected (G_TRI1, quad) + the index scale that follows, GoldenEye 2 expected (G_TRI4, its table); no unexplained mismatch; mutation check: the six F3D edits of SM64 all change the result |
| `tools/derive_c_check.sh` | `derive_refs.c` up to date (generator version 3); 54 texts, 29 leave-one-out, 17 mutants, 786 pairs: 0 differences; ASan + UBSan over the 62 distinct texts clean |
| `armlet-probe-replay --sync` | 29 references (F3DEX2 family, F3DEX 1.x, F3D) in step with the tables |
| the probe on the dumps, `--self` (each registered text with its own record among the references) | the six F3D texts EXACT, record equal, PASS 26 / 26 |
| the probe on the dumps, `--force --none-x 200` (leave-one-out records, NONE) | SM64, Pilotwings, Mischief Makers, Tetrisphere: record equal, PASS 26 / 26; GoldenEye: record equal but for G_TRI4, PASS 26 / 26 (the cases cannot use a command the record lacks); Wave Race: FAIL, 26 of 26 decline (no G_TRI1) |
| **probe mutations** (`tools/hle/mutcheck_probe_f3.sh DUMPS`; `--hmut=N`), the six texts `--self` | unmutated 6 PASS. **410** (the reject mask's near bits): 4 FAIL, Pilotwings and GoldenEye PASS (an equivalent mutant there: their bit 0x40 is the sign of w of second-of-pair vertices only, and a triangle with it in all three is clipped to nothing by the plane w > 0). **411** (clip flag word variant), **412** (G_TRI1 pc + 8), **413** (class-2 table 2 lower), **414** (index scale 4 <-> 8), **415** (G_CULLDL mask), **417** (G_MOVEWORD table 2 lower), **418** (Wave Race's triangle code), **419** (loop form): 6 FAIL each. **416** (G_POPMTX's stale flag): 5 FAIL, GoldenEye PASS (no G_POPMTX). **420** (G_TRI4's table one byte off): GoldenEye FAIL, the others PASS. 415 needed the case of three vertices beyond the clip-ratio planes with w < 0 (only clip-ratio bits in common), 416 the matrix pop and second draw in `lit`. (The G_DL depth byte's place was tried first as 417: unobservable while the calls are balanced, which the derivation reads from two moved instructions anyway) |
| the existing probe mutations | `mutcheck_probe.sh` (340 .. 353), `mutcheck_probe_s2.sh` (400 .. 403), `mutcheck_probe_f1.sh` (404 .. 409) as before; `mutcheck_ucache.sh` passes (with an F3D record round trip, format 4), 345 .. 349 caught |
| 3700-frame gates, `ref` against `armlet-hle` (`hle`), `armlet` (`def`), `ARMLET_HLE_VERIFY=1` (`ver`), `probe` (`ARMLET_PROBE=on ARMLET_PROBE_FORCE=1 ARMLET_PROBE_LATCH=1 ARMLET_HLE_VERIFY=1 ARMLET_PROBE_CHECK=1 ARMLET_PROBE_NONE_X=130`: the leave-one-out records), `probes` (the same with `ARMLET_PROBE_FORCE_SELF=1`: the registered records derived and latched), `probeu` / `probeux` (unforced; `ARMLET_PROBE_NONE_X=900`) | **all identical** (hash, state, 6 pictures, audio): Super Mario 64, Pilotwings 64, Blast Corps, Wave Race 64, Mischief Makers, Tetrisphere (hle, def, ver, probe, probes), GoldenEye 007 (registered: hle, def, ver, probes, `ARMLET_HLE_POISON=1`, `ARMLET_DMEM_POISON=mask` and `=ring`; before: hle, def, probeu, probeux), Wipeout 64, Glover, Donald Duck (hle, def, probeu), Banjo-Kazooie (hle, def, ver), Donkey Kong 64, Ocarina of Time, Paper Mario, Kirby 64, Worms Armageddon, Excitebike 64 (hle, def). Donald Duck against a reference made in this session (the F3DEX 1.x session's reference differs from today's from hash 684 on, in every variant and in a fresh reference too: not the plugin) |
| verify in those runs | 0 differences everywhere; native share: SM64 1727 of 1728, Pilotwings 1983 of 1984, Wave Race 1087 of 1088, Tetrisphere 1279 of 1280, Blast Corps 1647 of 2624 (its other two texts and G_MOVEWORD, as before), Mischief Makers 1 of 3648 (XBUS, as before), **GoldenEye 1663 of 1664** (0 before), Banjo-Kazooie 1535 of 1536 |
| the probe in those runs | `probe`: the registered F3D texts the game runs derive NONE without their own record (record equal: SM64, Pilotwings, Tetrisphere, Mischief Makers), PASS 26 / 26 and latched (verified, 0 differences); Wave Race FAIL (declines), not latched; Mischief Makers inconclusive (the game's RDP reads XBUS); Blast Corps' `2f41` (NONE, 32 words) **FAIL, 26 of 26 differ** (its RDRAM-collecting output flush), not latched; `1d12`: no family. `probes`: every registered text EXACT, record equal, PASS, latched (Wave Race: 1023 of 1024 tasks then natively by the derived build), verified. Unregistered: GoldenEye's `538f` (before the registration) NONE, no probe; with `ARMLET_PROBE_NONE_X` PASS 26 / 26, latched, but **87 of 1600 tasks native** (the rest declined at G_TRI4); Wipeout's `968d` and Glover's `9d0d` (Sprite2D) NONE, no probe |
| final build (G_TRI4, GoldenEye registered): `ver` (and `probes`) again on SM64, Pilotwings, Wave Race, Tetrisphere, Mischief Makers, Blast Corps | all identical; `ver`: SM64 1727 of 1728 native, Pilotwings 1919 of 1920, Wave Race 1023 of 1024, Tetrisphere 1279 of 1280, Mischief Makers 1 of 3648, Blast Corps 1647 of 2624, 0 differences; `probes`: records equal, latched, 0 differences (the task counts of a game differ a little from run to run) |
| 9000 frames, `ver` (SM64, Pilotwings, Wave Race, Tetrisphere, GoldenEye) | all five identical to `ref` (hash / state / 6 shots / audio; 13300 / 13540 / 11609 / 11854 / 7978 hash entries), 0 verify differences |
| builds | native, aarch64, rg35xxsp (glibc <= 2.17) clean |

## The background validator and the latch policy (2026-10-17; docs/design.md "The background validator and the latch policy")

Desktop x86_64 unless noted, branch `ucode-latch` on devel da1c0e6. `GfxMode=hle`, `ARMLET_PROBE=on`; forced games (`ARMLET_PROBE_FORCE=1`) run as if their text were unknown.

| check | result |
|---|---|
| `tools/hle/mutcheck_latch.sh` (`test_latch.c`: settings and parsing, fingerprint buckets, 1 / (1 + n) evidence, EXACT 24 / 8 and NEAR 42 / 14 thresholds, kinds, suspend + reset, strikes incl. earlier sessions, NEAR single strike, decay 8 -> 256, fingerprint bound) | passes; 430 .. 437 each caught (1 to 12 checks fail) |
| `tools/hle/mutcheck_ucache.sh` (format 5: latch state round trip, an unlatch for good kept over a later store, another process and a merge) | passes; 345, 346, 347, 349, **442** caught |
| 3700-frame gates against `ref`, `ARMLET_HLE_VERIFY=1 ARMLET_PROBE_CHECK=1`, validator on: Donkey Kong 64, Ocarina of Time, Kirby 64, Banjo-Tooie (forced), Super Mario 64 (forced, `ARMLET_PROBE_FORCE_SELF=1`: its leave-one-out derivation is NONE), Donald Duck (unregistered) | **all identical** (hash, state, 6 shots, audio), verify **0 differences**; Donald Duck needs its derived build's dead ranges as `EXTRA_MASKS` (logged as `latch: masks`; without them the hash differs from call 1820 on dead DMEM, the state stays identical) |
| time to latch (desktop; tasks = graphics tasks of the pair, all of them captured) | Donald Duck 48 tasks 1.2 s (16 distinct), SM64 111 / 5.5 s (12), Banjo-Tooie 148 / 7.4 s (8), DK64 248 / 8.2 s (8), OoT 374 / 9.3 s (NEAR: 42.1, 15). **Kirby 64: not latched** (1795 equal tasks, 3 fingerprints in its intro). OoT's loaded S2DEX and its reloaded own text: 228 / 234 equal, not latched in 3700 frames |
| no false mismatch | 0 mismatches over every replay of the runs above (several thousand tasks), and 0 declines but the gate's |
| corrupted record (test builds, the record changed after the probe passed), **without** verify | **443** (zex / NoN / clip flag word): DK64 and Donald Duck caught before latching (2 strikes, unlatched for good), output identical to `ref`; **SM64: latched after 110 equal tasks, the first mismatch 41 samples later (vertex clip flags of some vertices), output differs from RSP call 1594 until the unlatch** (the limit in design.md). **440** (reject mask near bit): DK64 latched, caught after latching (2 strikes), output identical; Donald Duck: never shows (equivalent in 3700 frames). **441**: never shows in DK64 / Donald Duck, identical |
| gate and compare mutations (DK64, 3700 f) | **438** (gate ignored): 0 gate declines against 3; **439** (replay compare of SP_STATUS wrong): every replay differs, suspended, unlatched for good |
| second launch from the cache (`ARMLET_UCODE_CACHE` fixed; DK64 with `ARMLET_PROBE_FORCE_CACHE=1`, Donald Duck) | the second run starts "latched in an earlier session", native from its first task; both runs identical to `ref` |
| `GfxProbe` off: `armlet-hle` (DK64, SM64, OoT) and `armlet` (DK64) against `ref` | identical; offline replays of 6034 captured tasks (`reg4-wt`, `s2dd`, `rej`, `indy`, `rare`) give per file the same outcome as devel da1c0e6 |
| ThreadSanitizer build (`-fsanitize=thread`, `setarch -R`, libtsan preloaded), DK64 150 s and OoT 200 s with `ARMLET_HLE_VERIFY=1`, RomClosed reached | no report; test build **444** (replay without the front-end lock): TSan reports the race in `f3dex2_run`; with the emulation thread's timed wait (Mario Party 3, `ARMLET_PROBE=on`, 100 s): no report, test build **491** (a task that finds the lock taken runs without it): TSan reports the race in `f3dex2_run` |
| builds | native, aarch64, rg35xxsp (glibc <= 2.17) clean |

**Cost (desktop).** Capture on the emulation thread 3.5 us per task; replay 0.33 ms (DK64) / 0.83 ms (SM64) CPU per task on the validator thread; 0 samples dropped for want of a slot; capture slots 656 KiB at most (plus the replay's 8 MiB RDRAM mapping, pages dropped after each replay).

**Devices** (rg35xxsp build on both, clocks pinned, `time` runs to frame 2400, frame times from the swap returns after frame 600; medians of the runs; `off` = probe off (DK64, SM64) or `ARMLET_PROBE_LATCH=1` (Donald Duck), `warm` = validator with a cache that says latched (native, gate, sampling from 1/8), `cold` = first run, empty cache):

| device / game | derive, probe | time to latch | off p95 / p99, >36 ms | warm p95 / p99, >36 ms | process CPU off / warm / cold (s) |
|---|---|---|---|---|---|
| rg35xxsp DK64 | 10.5, 37.3 ms | 248 tasks, 8.4 s | 30.6 / 44.0, 36 | 31.6 / 44.9, 53 | 51.4 / 54.3 / 56.4 |
| rg35xxsp SM64 | 26.4, 38.1 ms | 110, 5.8 s | 22.1 / 27.5, 4 | 21.6 / 28.1, 5 | 40.5 / 43.1 / 46.1 |
| rg35xxsp Donald Duck | 26.6, 41.7 ms | 48, 1.4 s | 26.6 / 34.7, 15 | 26.6 / 37.4, 22 | 60.3 / 61.7 / 62.9 |
| rgdsplus DK64 | 8.3, 35.5 ms | 248, 8.3 s | 30.5 / 40.9, 28 | 30.3 / 42.9, 37 | 45.4 / 48.1 / 49.9 |
| rgdsplus SM64 | 22.5, 38.3 ms | 110, 5.7 s | 28.2 / 30.9, 6 | 28.5 / 33.9, 7 | 35.5 / 38.1 / 40.7 |
| rgdsplus Donald Duck | 21.8, 44.3 ms | 48, 1.3 s | 28.8 / 36.9, 25 | 28.3 / 34.2, 18 | 54.0 / 55.3 / 56.4 |

p50 is 16.6-16.9 ms everywhere. On the A53 a replay costs 2.2 ms (DK64) to 5.6 ms (SM64, A55) of validator-thread CPU and a capture 0.1 ms on the emulation thread. The warm cost is the sampling: with sampling off (`ARMLET_LATCH_POLICY=sample=1000000,...`, rgdsplus SM64) the latched build costs what the registered one does (process 35.6 against 35.4 s, RSP time +1.3 %, p99 30.0 against 30.9 ms); with it, about 100 sampled tasks in 40 s run on the JIT (emulation thread +0.45 s, validator 0.54 s, the rest on other threads) and DK64 shows more frames over 36 ms. Raw data: test-output/latch (git-ignored).

### Second round: data-dependent kinds, direct matches, sampling from 1/32, session marker (2026-10-17)

| check | result |
|---|---|
| unit tests | `mutcheck_latch.sh` (sampling now 1/32 .. 1/256) and `mutcheck_ucache.sh` pass, mutations caught as before |
| **direct matches** (`tools/equiv/check_direct.sh`, `GfxProbe=on`, no `ARMLET_PROBE_FORCE`): Kirby 64, DK64, OoT, SM64 | 63 of the first 64 tasks native (the 64th is counted before it runs), no validator or latch line: registered builds run at once; 3700-frame runs of the four identical to `ref` |
| 3700-frame gates with the validator, verify on (same six games and options as above) | all identical, verify 0 differences |
| time to latch, before -> after the data kinds (desktop) | DK64 248 -> **39** tasks (8.2 -> 1.3 s), OoT 374 -> 233 (9.3 -> 5.1 s), Banjo-Tooie 148 -> 102 (7.4 -> 5.1 s), SM64 111 -> 104 (5.5 -> 5.2 s), Donald Duck 48 -> 33 (1.2 -> 0.9 s), Kirby 64 (forced) still not; the kinds add fingerprints, the gate then declines 2 to 5 tasks per game on situations first met after the latch |
| **443 on SM64, without verify** | now **identical to `ref`**: the latched build declines the first task with a vertex behind the eye (`LATCH_K_WNEG`, not validated), the JIT runs it, the replay differs, 2 strikes, unlatched for good |
| 440 / 441 / 443 on DK64 and Donald Duck, without verify | all identical to `ref`; 440: DK64 latched, caught, unlatched; 443: both caught and unlatched; 440 / 441 on Donald Duck and 441 on DK64 never show |
| cache second launch (Donald Duck) | the second run starts latched, both identical |
| unclean end | DK64 with 443 and `strikes=1000000`, killed while suspended: the next launch logs "suspended when the last session ended without closing the ROM; unlatched for good" and runs identical to `ref` (JIT). The same with a clean close (SIGINT, RomClosed): the next launch continues the pair as a candidate |
| `GfxProbe` off (DK64, SM64 `armlet-hle`, OoT `armlet`) | identical |
| TSan (DK64, OoT with verify) | no report |
| builds | native, aarch64, rg35xxsp clean |

**rg35xxsp DK64, sampling from 1/32** (4 off / 4 warm alternating, 1 cold; same method as above): off p95 30.8 / p99 44.0 ms, **35.5** frames > 36 ms, process CPU 51.4 s; warm p95 30.9 / p99 44.2,
**41.5** frames > 36 ms, 52.7 s; cold 48 frames, 53.1 s, latched after 39 tasks in 1.3 s. With 1/8 it was 53 frames and 54.3 s warm.

## F3DEX 1.x G_LOAD_UCODE (2026-10-17; docs/native-f3dex.md section 3, "G_LOAD_UCODE"; branch `f3dex1-load`)

The F3DEX 1.x front-end follows a `G_LOAD_UCODE` to the same text (re-initialisation) or another registered / latched F3DEX 1.x build: the interpreter runs the handler and the loaded microcode's initialisation, `enter` rebuilds the loop state. Desktop x86_64 unless said; JIT as the oracle (`--oracle`).

| check | result |
|---|---|
| synthetic tasks, `tools/hle/mksyn_ex1.py` (`load`: A -> A, B, B A, B B, A B, A A with data images of 0x7ff or shorter; `call`: the same inside a called list; `xload`: S2DEX 1.x; texts and data from the dumps, `tools/hle/synth_ex1.sh`), 1000 seeds per case, 15 (A, B) cells: 121 -> 123 / 122n / lx123 / 0.95, 122n -> 121 / 123, 123 -> 121 / lx123 / 122n, F3DLX 1.23 -> 123 / F3DLX.NoN 1.21, F3DLX.NoN 1.21 -> 1.23 / lx123, 1.23 -> 1.21, 0.95 -> 121 | **0 differ in all 45 runs.** The base builds' snapshots are captures of Banjo-Kazooie, Star Fox 64, SpaceStation Silicon Valley, Pokemon Puzzle League, Toy Story 2, Clay Fighter, Iggy's Reckin' Balls, Mario Kart 64 (with the overlay code of the other tasks added, `make_donor.py`). 739 of 1000 equal, 261 declined for the F3DEX 1.2x builds (a switch with a data image shorter than 0x800: mutation 470), 630 to 639 for F3DLX (clipping off and junk vertices, light state), 295 for 0.95 as the target (its IMEM tail differs: declined); `xload` 100 of 100 declined; the 0.95 base has no G_LOAD_UCODE (no-op there) |
| the same, `MKSYN1_FUZZ=1` (bytes of the list, vertices and matrices flipped, command words swapped), 1000 seeds `load` + 1000 `call` (121 -> 123) | 479 + 633 equal, 0 differ |
| qemu-aarch64 NEON and scalar (`out/aarch64-tools`, `-scalar`), 250 seeds `load`, `call`, `xload` | 178 equal, 0 differ, 72 declined (x86: the same counts); `xload` 250 declined |
| perturbed copies of the captured tasks (`fuzz_snaps.py` + `fuzz_cmds.py` for af, b4, 04, bf, 01, bc, `FUZZ_DONOR`; Banjo-Kazooie, Mario Kart 64, Star Fox 64, SpaceStation, Pokemon Puzzle League, Toy Story 2, Clay Fighter, Iggy; 3 seeds), `--oracle` | 10 638 equal, 1 mismatch, 13 413 declined, 135 hung (the JIT). The mismatch (Toy Story 2 `ts2.2` task 22, F3DLX.NoN data DMEM 0x18 / 0x118) is the same with the Oct 15 binary (before the data-driven record and this branch): not a regression; analysed afterwards, see "G_MODIFYVTX outside the vertex slots" below |
| captured task starts (Banjo-Kazooie 45, Mario Kart 64 4, Star Fox 64 4, Pokemon Puzzle League 60, Toy Story 2 60, Clay Fighter 30, Iggy 30, SpaceStation 60: 57 decline at the S2DEX 1.x load as before) | all equal |
| 3700-frame gates, `ref` against `armlet-hle`, `armlet`, `ARMLET_HLE_VERIFY=1` | **identical** (hash, state, 6 pictures, audio) and 0 differences in verify: Pokemon Puzzle League, SpaceStation Silicon Valley, Banjo-Kazooie, Mario Kart 64, Star Fox 64, Donald Duck, Glover, Donkey Kong 64, Ocarina of Time, Kirby 64 |
| probe run (`ARMLET_PROBE=on ARMLET_PROBE_FORCE=1 ARMLET_PROBE_LATCH=1 ARMLET_HLE_VERIFY=1`) on SpaceStation | `probe: ucode 5c420d85... (loaded by G_LOAD_UCODE from 0014e320, data 0015c770)` then `the derivation says NONE (f3d family, 239 of 1024 words aligned); no probe`: the loaded S2DEX 1.x text is noted and derived (verdict NONE: no family of the derivation covers it); output identical to `ref`, 1093 tasks verified, 0 differences. Pokemon Puzzle League: its S2DEX 1.x (`190d04df`) runs as a task of its own in 3700 frames, no load in an F3DEX task: NONE (f3dex2 family, 1166 of 1484 aligned) |
| SpaceStation native share (3700 frames, `ARMLET_HLE_STATS`) and the cost of the decline memory | 1093 of 2368 tasks native with the memory (before this branch 1384): the memory (`load_known`) skips the pair's tasks once more than half of them decline at the load (517 declined at the load, 694 at once), which also skips non-loading ones (every 64th runs). **Measured in time** (`F3DEX_NO_LOADMEM=1` switches it off in the same build: loading tasks run natively up to the load and decline late, 293 more non-loading tasks run natively), `armlet-hle`, total RSP ms of the 5905 calls to frame 3700, alternating, 6 repeats on the desktop: memory 5221 5114 4517 4527 4497 4502 (median 4522), no memory 4933 4592 4552 5389 4523 4731 (median 4662); the quiet runs 4497 .. 4527 against 4523 .. 4592. rg35xxsp (H700, clocks pinned 1512 MHz and restored, own dev dir, removed afterwards), 4 alternating repeats of 3700 frames, RSP ms: memory 12 373 11 962 12 116 12 009 (median 12 063), no memory 12 002 12 036 12 043 12 070 (median 12 040). **Equal within the noise (0 to 3 % in the memory's favour on the desktop, 0.2 % against it on the device): the rule stays.** Pokemon Puzzle League is not affected (no load in its F3DEX tasks in 3700 frames: 255 of 256 tasks native, 0 declined, with or without the memory) |
| builds | native, aarch64, rg35xxsp (glibc <= 2.17) clean, no new warning in `src/` or `tools/` |

**Mutations** (`tools/hle/mutcheck_f1load.sh`, `--hmut=N`; caught = more seeds differ or decline than unmutated, 300 to 400 seeds of `load` on Banjo-Kazooie): **460** (the display list position handed to the new microcode is 8 bytes on): 91 differ; **461** (the loop keeps the old record after a switch): 275 declined (against 103); **462** (the command does nothing): 292 differ; **464** (the IMEM tail guard, 121 -> 0.95): 66 differ; **467** (the load key ignored: no build found): 275 declined; **468** (the current text address not updated: a load back is taken for a re-initialisation): 139 declined; **470** (a switch with a partial data image): 37 differ. **463** (the probe is not told): not seen by seeds (the replay has no probe); the SpaceStation probe run lacks the `loaded by G_LOAD_UCODE` line. **466** (the decline memory skips every pair; the mutation is the rule's own switch, `F3DEX_NO_LOADMEM` the opposite one): not seen by the replay (one process per snapshot); SpaceStation 1500 frames: 17 native tasks of 704 against 258 of 704. (The IMEM tail and junk guards: 465 is a plain decline with no observable effect on the tasks of the tests, so it has no number.)

**F3DEX 1.x G_LOAD_UCODE under the latch policy (the merge of `ucode-latch` and `f3dex1-load`).** The load is the `LATCH_K_LOAD` risky path of the host build (`hle_cmd`), a derived build is a switch target only when `hle_build_usable` (latched; in a replay under validation: not unlatched for good), the switch rebinds the gate (`hle_bind`, so the new build's record gates the rest of the task), a replay under validation leaves the decline memory and the probe's load note alone, and the probe's host rule accepts F3DEX 1.x hosts (`host_build`: registered or latched). Checked on the merged tree: the synthetic `load` / `call` sets (15 cells, 1000 seeds each) 0 differ with the counts above, qemu NEON and scalar 0 differ, the mutation scripts of both branches as documented, 3700-frame gates identical to `ref` (`armlet-hle`, `armlet`) on 16 games, the validator without `ARMLET_PROBE_FORCE` on Super Mario 64, Donkey Kong 64, SpaceStation Silicon Valley and Donald Duck (Donald Duck with its latched build's dead ranges as `EXTRA_MASKS`), `ARMLET_PROBE_FORCE=1` with `ARMLET_HLE_VERIFY=1` on Donkey Kong 64, Donald Duck, SpaceStation, Banjo-Kazooie (latched, 0 verify differences) and the captured replays (6038 native == JIT, 0 differ).

**What the first runs found.** A switch with a data image shorter than 0x800 is not a state the front-end knows: the builds' data differ in constants (0x396 / 0x397: the end of the overlay window, 0x2cf: the text tag ...) which the other build's code reads, so the JIT's result depended on bytes of the old build's data (12 of 40 early seeds differed; all of them with 0x1ff .. 0x27f); declined now (470). The JIT hangs on lit vertices when the snapshot's RDRAM has no lighting overlay: the generator patches the build's text and overlays (`MKSYN1_A_ADDR`).

## G_MODIFYVTX outside the vertex slots (F3DEX 1.x; the Toy Story 2 perturbed-task mismatch, 2026-10-17; branch `ts2-perturbed`)

Toy Story 2 (F3DLX 1.23) task 22 of `fuzz_snaps.py` seed 2 differed from the JIT (DMEM 0x18 and 0x118, RDP stream from command 320). `ddmin_snap.py` (against the base *with* the donor rows, which `fuzz_snaps.py` adds before perturbing) reduced it to one byte: a `G_MODIFYVTX` (b2) of the game's own list got slot offset 0x7800 in w0. The microcode reads the slot's address at `vtab + (w0 & 0xfff)` = DMEM 0xb1e, in the operand buffer where the last `G_VTX` staged its records (the halfword at +0x3e of the fourth); the lean mode does not stage them, so the front-end read the start state's 0 and stored w1 at 0 + field 0x18 (the othermode word) where the microcode stored into the staged halfword. `ARMLET_HLE_EXACT=1` (which stages the records) is equal to the JIT; only the lean mode differs. The F3DEX2 front-end already declined this (`G_MODIFYVTX out of range`, mutation 218); the F3DEX 1.x one stored with `st32u` unchecked. Fix: the same decline (slot offset past 0x3e or odd, field past the 0x28-byte record or not a word offset), mutation 471; `G_BRANCH_Z` got the slot-offset half of it (mutation 472).

`tools/hle/mutcheck_modvtx.sh <Toy Story 2 task>.snap` (`mksyn_modvtx.py`: a G_VTX of 4 nonzero records, the modify, a setothermode): declined with the fix, MISMATCH with `--hmut=471`, slot 0 / field 0x14 native and equal in both; the same for `G_BRANCH_Z` (`FIELD 0xb0`, w1 = 0 takes the branch to a list with another othermode write): declined, MISMATCH with `--hmut=472`, slot 0 equal.

| check | result |
|---|---|
| perturbed copies + aimed `fuzz_cmds` (b2 w0lo / w0mid / w1lo, b0, 01, 04; 3 seeds x 20 copies; Toy Story 2, A Bug's Life, Iggy, Pokemon Puzzle League, SpaceStation; Clay Fighter seeds 1-2 partly; `FUZZ_DONOR`, `--oracle`, x86) | 13 484 equal, **0 mismatches**, 25 242 declined, 227 hung (the JIT does not finish; as before) |
| captured F3DEX 1.x tasks (313 snapshots), fix against `--hmut=471` | 234 equal, 0 mismatches, 57 declined both; same counts |
| 3700-frame gates `ref` against `armlet-hle` / `ARMLET_HLE_VERIFY=1` (and `armlet` for Toy Story 2) | Toy Story 2 (hash, state, 6 pictures, audio identical; verify 1535 tasks, 0 differences), A Bug's Life, Clay Fighter, Banjo-Kazooie, Mario Kart 64: all identical |

`G_BRANCH_Z` reads a slot address the same way: with w1 = 0 the synthetic one goes the other way in the lean mode (it did not with w1 = 0x7fffffff, which is why it was first missed), now declined too.

## The loose tier (2026-10-17; docs/design.md "The loose tier")

Desktop x86_64, `GfxMode=hle`. The tier is opt-in (`GfxProbe = loose`); `on` stays the default, and the first row shows it unchanged.

| check | result |
|---|---|
| `tools/hle/mutcheck_latch.sh` (`test_latch.c`: the `loose` key and default 2.5, loose thresholds 60 / 20, NUMERIC counted and never a strike, STRUCTURAL a strike as EXACT, HARD unlatches at once, loose over NEAR, a strict build's NUMERIC is a strike, `latch_grade`) | passes; 430 .. 437 as before, **450 .. 453** caught (4 / 1 / 2 / 1 checks) |
| `tools/hle/mutcheck_ucache.sh` (format 6: the tier in the key, `n_x` and `numeric` round trip, a loose store replaces the loose entry only, a strict lookup never sees the loose latch) | passes; 345 .. 349, 442 as before, **454** caught (6 checks) |
| `armlet-probe-replay --force` on the 10 registered F3DEX2 texts of `mutcheck_probe.sh`, strict and `--tol` (the compare's bounds became parameters) | 10 PASS strict, 10 PASS tolerant, records equal: as before |
| **strict tier unchanged**: `tools/equiv/check_direct.sh` (DK64, Ocarina of Time, SM64, Kirby 64, `GfxProbe=on`); 3700-frame gates against `ref` with `ARMLET_PROBE=on ARMLET_HLE_VERIFY=1`: DK64 forced, Donald Duck (unregistered `efe4`, the strict latch) | 63 of 64 native, validator idle, all four; DK64 LATCHED after 39 tasks (evidence 24.4, 16 distinct) in 1.3 s, Donald Duck after 33 in 0.9 s: the strict thresholds; both **identical** to `ref` (hash 3385 / 9672, state, 6 shots, audio), verify 0 differences |
| Chameleon Twist 2 (F3DEX 2.05 `9798`: NONE, 2 unexplained + 3 missing words, both in the clip overlay), `ARMLET_PROBE=loose`, 900 frames | **admitted** (the 0.9 aligned-fraction floor first tried refused it at 944 / 1154: lowered to the derivation's 0.5); the probe **FAILS 4 of 28 canned tasks**, all `clip`: the stream is 2800 bytes against the JIT's 2464 (another number of clipped triangles), DMEM 0xf2 .. 0xf3 and the DPC registers differ: structural, cached as failed; the game's own tasks never reach that path (it verified clean in 3839 tasks before). The right outcome; the fix was strict-tier: the `clip204` flag (the section below), after which it derives EXACT and latches strictly |
| Blast Corps F3D 2.0D `2f41` (NONE, 32 + 29 words), `ARMLET_PROBE=loose`: unmutated against `--hmut=456` (the admission bound ignored) | unmutated: *not admitted to the loose tier*; **456**: admitted, the probe fails **26 of 26** canned tasks: the bound is the first net and the probe the second |
| Donkey Kong 64 forced through the loose tier (`ARMLET_PROBE=loose ARMLET_PROBE_FORCE=1 ARMLET_PROBE_LOOSE_FORCE=1 ARMLET_PROBE_FORCE_CACHE=1 ARMLET_HLE_VERIFY=1`), 3700 frames against `ref`, cold and then from the cache | derived EXACT, "(loose tier)", probe 27 equal / 1 declined; **LATCHED after 310 equal tasks (evidence 60.94, 28 distinct) in 10.5 s** (strict: 24 / 8); 1258 of 1280 tasks native; validator 352 replayed, 352 equal; verify 0 differences; **identical** to `ref` (hash 3385, state, 6 shots, audio). Second launch: the cache says latched but the session marker is set (the e2e run is killed at its frame count, not closed): "a loose build latched when the last session ended uncleanly; validating it again" (a strict build would start native at once; a suspended strict one would be unlatched for good), LATCHED again after 268 tasks in 8.9 s, 1339 of 1344 native, 288 / 288 equal, identical to `ref` |

| **the region gate** (`test_latch.c`: never kinds not added by matches, kept across a suspension; mutation **457**) | passes, 457 caught (2 checks) |
| DK64 forced through the loose tier with `ARMLET_PROBE_LOOSE_FORCE=clip,light` (pretend the clip and light regions have unexplained words), 3700 frames vs `ref` | "unexplained words in the clip light regions; the tasks of those kinds stay on the JIT"; LATCHED after 310 tasks as above; **314 of 1344 tasks native, 1029 declined** at the gate (DK64 lights or clips in nearly every task: a region gate on a 3D game's clipper leaves little for the native path, which is why it is a safety, not a plan); verify 0 differences, **identical** to `ref` |

| **the default-record arm** (`test_latch.c`: the `default` key and 3.0, thresholds 72 / 24; mutation **458** a default-record build needs only the loose evidence) | passes, 458 caught (1 check) |
| the arm on the far-NONE texts of the set, `ARMLET_PROBE=loose`, 900 .. 1500 frames: Blast Corps F3D 2.0D `2f41` (32 + 29 words, 86 % aligned), Wipeout 64 L3DEX 1.21 `3c7a` (511 + 38, 51 %) and F3D 2.0H `968d` (24 %), Rogue Squadron `bfbf` (18 %) | `2f41`: tried as the F3D record as derived, the probe fails **26 of 26**; **`3c7a`: tried as the F3DEX 1.21 record as derived, the probe passes 28 of 28** (the unexplained words are the line code, "in the other region; no kind gate covers them"), a candidate; `968d` and `bfbf`: below the family floor, not admitted |
| mutation **459** (the arm's family floor ignored), Rogue Squadron | admitted; `dbuild_make` refuses the record ("a handler pc of the record is missing or not an instruction address"), cached as unsupported: the third net after the bound and the probe |
| Wipeout 64, `ARMLET_PROBE=loose ARMLET_HLE_VERIFY=1`, 3700 frames vs `ref`, cold and then from the cache (with the build's `latch: masks` as `EXTRA_MASKS`); then at `default=3` (the shipped value) and `ARMLET_LATCH_POLICY=default=2` | the L3DEX candidate: **1568 replayed, 1568 equal**, 0 numeric, verify 0 differences, every run **identical** to `ref` (hash 14768, state, 6 shots, audio); **not latched** at 4x, 3x or 2x. `ARMLET_LATCH_TRACE=1` (1800 frames, 812 validated tasks) shows why: every task has the **same fingerprint, the empty one** (`cbf29ce484222325`, no command counted): Wipeout's L3DEX tasks are near-empty lists (0.08 % of its RSP time in the hot list), one fingerprint never latches whatever the multiplier (the "scenes that vary little" rule), and there is nothing to gain from them anyway. The multiplier was set to 3 on the general argument (the protection is the probe, the grading and the three nets; the multiplier buys variety), not on this text |

| **loads noted from replays** (NBA Jam 2000, found by the three-set recount: its latched 2.05 build declined 2326 of 3072 tasks "at the gate", every replay declined with `G_LOAD_UCODE of another ucode`, and the loaded text was never derived because only the replay reaches the load): `ARMLET_LATCH_TRACE=1` 5000 frames before and after | before: no `loaded by G_LOAD_UCODE` line; after: the loaded text `36f10dc3` (F3DLX.Rej 2.05) is derived (NONE, 391 of 1024 aligned: Rej is no derivation family yet, so the tasks still decline). Regression, forced, 3700 frames vs `ref`: Ocarina of Time (3 loaded notes, S2DEX and its own text back, LATCHED after 233) and DK64 (LATCHED after 39) **identical**, verify 0 differences |

The cost of the tier on the desktop is the derivation (2 ms per pair) and, for an admitted pair, what a NEAR pair costs plus the higher evidence (310 captured tasks against DK64's 248 strict).
No pair of the 64-ROM set runs on it today (design.md "What it is for"); the recount with `GfxProbe = loose` against `on` is what decides whether it becomes the default.

## `clip204`: the clipper's reciprocal flag (2026-10-17; docs/ucode-f3dex2-dialects.md section 7)

| check | result |
|---|---|
| `tools/derive_check.sh DUMPS` (leave-one-out, every registered record field for field, `clip204` included: 78 fields compared) | every row as before (`none`; `l_b_bg` unchanged) |
| `tools/derive_c_check.sh DUMPS` (C vs Python, `clip204` in the compared fields; `derive_refs.c` regenerated, `--check` passes) | 54 texts, 29 loo, 17 mutants, 786 shortcut: 0 differ |
| `tools/derive_build.py derive DUMPS 97989ee9c10ef7ad.1f45` (Chameleon Twist 2's F3DEX 2.05) | was NONE (2 unexplained, 3 missing); **EXACT**, 0 unexplained, `gen204 = 0 clip204 = 1 zex = 1`; votes `gen204` (15, 0), `clip204` (0, 2), `zex` (0, 19) |
| `armlet-probe-replay --force`, the 10 texts of `mutcheck_probe.sh` | 10 PASS, records equal; Chameleon Twist 2's text: PASS, 28 canned tasks, 27 equal, 1 declined (was: 4 of 28 differ) |
| `tools/hle/test_ucache.c` (`gen204 / clip204 / zex` round trip; the flag is in the entry's former `pad5`), `mutcheck_ucache.sh` | passes; mutation **460** (`clip204` not read back from an entry) caught (2 checks) |
| Chameleon Twist 2, `ARMLET_PROBE=on ARMLET_HLE_VERIFY=1 ARMLET_HLE_STATS=1`, fresh cache, 3700 frames vs `ref` | derived EXACT, probe PASS, LATCHED after 1066 equal tasks in 18.6 s; verify 2301 tasks, 0 with differences; hash, state, 6 shots and audio identical |
| Donkey Kong 64 forced (`ARMLET_PROBE=on ARMLET_PROBE_FORCE=1 ARMLET_HLE_VERIFY=1`), 3700 frames vs `ref` | identical (hash, state, 6 shots, audio) |

## XBUS, part 1: the F3D builds' eligibility flag (2026-10-18; docs/ucode-f3d.md 4.4)

The F3D front-end has had the XBUS transport since the 2.0H work (`xbus_emit`: the ring at `s7`, the room check, the wrap, `DPC_END` per command; `ucode-f3d.md` 4.4, harness-identical at 600 frames), but the registered Mischief Makers entry never carried `HLE_F_XBUS_OK`, so `armlet_hle_run` refused every task that starts with `DPC_STATUS.XBUS` set as "not eligible": 3198 of 3200 tasks in every coverage recount. The three-set recount found War Gods running the same text (`4212d63f`) with the same 4350 declines. The flag was added (`F3D_UCODE_R(l_f3d_mm, ..., .flags = HLE_F_XBUS_OK)`).

| check | result |
|---|---|
| Mischief Makers, `ARMLET_PROBE=on ARMLET_HLE_VERIFY=1`, 3700 frames vs `ref` | **3647 of 3648 tasks native, 0 declined** (no `XBUS ring:` decline: the wrap and the RDP-not-caught-up cases did not occur), verify 0 differences, **identical** (hash 7245, state, 6 shots, audio) |
| War Gods (not-coded set), the same | **5823 of 5824 native, 0 declined**, verify 0 differences, **identical** (hash 6998) |

Part 2 (the F3DEX2 xbus texts of the not-coded set: Ms. Pac-Man 2.08, Lode Runner 3-D 2.07, Command & Conquer 2.06) is a derivation feature: their ~48 words of difference to the fifo builds are the init, the flush routine and the exit tail of Excitebike 64's loaded `f649` text, which the front-end already runs (`f3_flush_xbus`) but which is not a derivation reference.

## The pair key: the static data signature (2026-10-18; docs/design.md "The probe and the ucode cache", "The pair key")

Twisted Edge (not-coded set) derives EXACT but never got a probe result in six recount passes: its `ucode_data` buffer is a saved DMEM image rewritten every task (1082 whole-image hashes in 1082 tasks, 114 bytes at 0x422 .. 0x57f), so every task was a new (text, data) pair and the 16-pair table filled with the first 16. The pair key is now the signature of the bytes the derivation reads (`data_sig`: the jump table's own opcode range and the overlay descriptors for F3DEX2, the two handler tables for F3DEX 1.x and F3D).

| check | result |
|---|---|
| first try, jump-table window ±0x100 around the table | still 16 pairs: the window overlapped the rewritten region (the halfwords of opcodes 0x20 .. 0x7f lie over other DMEM state); narrowed to -128 .. 0x1f |
| Twisted Edge, `ARMLET_PROBE=on ARMLET_HLE_VERIFY=1`, 3700 frames vs `ref`, cold | **1 derivation, 1 pair**, EXACT (from `l_b_ex2_206`), probe 28 of 28, **LATCHED after 192 tasks (8 distinct) in 6.5 s**, 1469 of 1472 native, verify 0 differences, **identical** (hash 1692, state, 6 shots, audio) |
| second launch from the cache | "latched in an earlier session", 1661 of 1664 native, identical |
| regressions: DK64 forced, Donald Duck (`efe4`), 3700 frames vs `ref` | one pair each, latched after 39 / 33 tasks, identical, verify 0 differences |
| Gauntlet Legends (the other game with many data images, 256) | its text is registered, so no pair is involved: 3388 of 3392 tasks decline with the status-poll time-out (ucode-derive.md "Gauntlet Legends"), unchanged; identical to `ref` |

## Rej as a derivation family (2026-10-18; docs/ucode-rej.md section 10, docs/ucode-derive.md "Rej as a derivation family")

The Rej builds, F-Zero X's F3DEX 2.03 and the F3DEX.NoN xbus 2.08 text are references of the derivation (35 references, `armlet-probe-replay --sync`: 0 out of step), the Rej extension table and `rej_flags` are derived, and `dbuild_make` builds a derived Rej record (the primary's `rej_ext_cmd`, the derived bits, the primary's transport).
Two small changes in `probe.c` were needed to probe a Rej build at all: the canned tasks were the S2DEX ones for any build with an extension table (`ext[0][1] < DERIVE_K_REJ` now), and a build whose lighting is not modelled (`no_lit`: F3DEX.Rej 2.06, ECW's text) is not required to complete the "lit" case.

| check | result |
|---|---|
| `tools/derive_check.sh` (selftest, leave-one-out of 35 builds, mutation check) | passes; every earlier row field for field as before (79 fields), six new rows, no UNEXPLAINED mismatch |
| `tools/derive_c_check.sh` (refs `--check`; C against Python: 61 texts, 35 leave-one-out, 17 mutants, 848 pairs with the shortcut) | 0 differences |
| `mutcheck_probe.sh` texts (10), `--force` | 10 PASS, records equal |
| the four registered Rej texts, `--force --self` (derived with their own record among the references) | 4 PASS, records equal (28 / 28 / 26 / 25 canned tasks equal) |
| the four, `--force` leave-one-out with `--none-x 1000` | 4 FAIL: each differs from its neighbours in what the other references cannot show (the LX / FLAT_A / FLX lighting bits, F3DFLX's other layout, the XBUS transport); without `--none-x`: NO_CANDIDATE (NONE) |
| Mario Party 3 `9332f30c`, NBA Jam 2000's loaded `36f10dc3`, `--force` | NEAR, 28 of 28 equal, PASS, latched |
| ECW `23cb9e85` | NONE (22 words); `--none-x 200`: 7 of 28 differ (RDP stream, byte 15 of the first triangle) |
| mutation 470 (`rej_flags` ^ `REJ_YQ` in `dbuild_make`), 471 (the light scale lane 0x1c6 <-> 0x1ce): `tools/hle/mutcheck_rej_derive.sh DUMPS` | 470: 4 + 2 of 4 + 2 FAIL (all 28 tasks differ); 471: 3 of 4 registered builds and both derived FAIL (the fourth, 2.06, lights nothing) |

End to end on the desktop (`ARMLET_PROBE=on ARMLET_HLE_VERIFY=1 ARMLET_HLE_STATS=1`, fresh cache, vs `ref`; hash, state, 6 shots and audio identical in all):

| game | frames | result |
|---|---|---|
| Air Boarder 64 | 3700 | `3c51d836` and its loaded `578ac8cc`: NONE, F3DEX 1.x family, 425 of 1024 words aligned, no probe; 182 native, the rest declined (unchanged) |
| ECW Hardcore Revolution | 3700 | `23cb9e85`: NONE (f3dex2, 786 of 1024 words aligned), no probe; 511 tasks native (the F3DEX.NoN 2.08 text), verify 0 differences |
| Excitebike 64, F-Zero X (registered Rej builds) | 3700 | 1663 of 1664 and 3391 of 3392 native, verify 0 differences |
| NBA Jam 2000 | 5000 | `97989ee9` (F3DEX 2.05) derived EXACT, probe 27 / 28, **latched** after 606 tasks; the text it loads, `36f10dc3`, "loaded by G_LOAD_UCODE", derived NEAR (`l_b_rej_203`), probe 28 of 28 equal, a NEAR candidate; its 2390 validated tasks are all equal, but with 2 distinct fingerprints (a NEAR build needs 14 of the policy's 8), so it did not latch in 5000 frames and the tasks that load it stay on the JIT |

## XBUS, part 2: the F3DEX2 xbus texts derived (2026-10-18; docs/design.md "XBUS")

`xbus` and `ring_start` are fields of the F3DEX2 record (a flag voted over the start-up, flush and exit code; a knob of the xbus references, else read from the start-up's `addi s7`), `dbuild_make` gives an xbus build `HLE_F_XBUS_OK` and no RDRAM ring, Excitebike 64's loaded `f649` text is a reference (the Rej work made `hle_rej.c`'s builds references), and `run_canned` no longer refuses a canned task because the game's `DPC_STATUS.XBUS` is set when the build is an XBUS one (every task of an XBUS game starts so; that refusal was why every xbus pair was "inconclusive" in the recount).

| check | result |
|---|---|
| `tools/derive_check.sh`, `tools/derive_c_check.sh` (the record has 44 layout fields now), `derive_refs_gen.py --check` | pass; 61 texts, 35 leave-one-out, 17 mutants, 848 pairs: 0 differences |
| the three not-coded xbus texts, `derive_build.py derive` | Ms. Pac-Man F3DEX xbus 2.08 `a986`: **EXACT**; Lode Runner 3-D F3DEX xbus 2.07 `cbe9`: **NEAR** (init 1); Command & Conquer F3DEX.NoN/L3DEX xbus 2.06 `65ad`: **NEAR** (init 1); all from `l_b_ex2_208n_xbus`, 0 unexplained, `xbus = 1`, `ring_start = 0xba8`, flag evidence xbus (0, 27) |
| Ms. Pac-Man, `ARMLET_PROBE=on ARMLET_HLE_VERIFY=1`, 3700 frames vs `ref` | probe 27 equal / 1 declined, **LATCHED after 1021 tasks**, **2428 of 2432 native**, verify 0 differences, **identical** (hash 3383, state, 6 shots, audio) |
| Lode Runner 3-D, the same | NEAR, probe 27 / 1, **LATCHED after 584 tasks** (43.8 evidence, 14 distinct), **1150 of 1152 native**, 0 differences, **identical** (hash 4593) |
| Command & Conquer, the same | NEAR, probe 28 / 0, a candidate (not latched in 3700 frames: its tasks load a second text `b2b0…`, 1276 of 1612 aligned, NONE: the S2DEX xbus 2.06 it switches to, which has no transport in the S2DEX front-end yet), 0 differences, **identical** (hash 5803) |
| Excitebike 64 (the registered xbus builds), regression | 1663 of 1664 native, identical |

## XBUS, part 3: S2DEX xbus 2.08 (StarCraft 64; 2026-10-18; docs/ucode-s2dex2.md section 9)

`l_b_s2_xbus` (`3ee636569037de5b`) is registered by hand: `f2_layout.xbus` with `ring_start` 0x4c0, `ring_end` 0xee0, `ring_gap` 224 (new layout fields; the Rej and F3DEX2.NoN layouts set 0xf10 / 176, a derived build's 0 means those). The S2DEX front-end's `after_emit` flushes through `f3_flush_xbus`.

| check | result |
|---|---|
| StarCraft 64, `ARMLET_PROBE=on ARMLET_HLE_VERIFY=1 ARMLET_HLE_STATS=1`, 3700 frames vs `ref` | registered (no probe, no latch), 0 declined, verify 1535 compared / 0 differences, **identical** (hash 1448, state 1490, 6 shots, audio) |
| regressions, the same | Worms Armageddon (S2DEX fifo 2.08, hash 5572), Kirby 64 (5324 common), Excitebike 64 (1451), Command & Conquer (5803): **identical**, verify 0 differences |
| captured tasks (4 StarCraft snapshots, `ARMLET_HLE_CAPTURE`, `ARMLET_UCODES=0000000000000001`), `armlet-native-replay` | 4 of 4 native == JIT |
| synthetic ring wrap: `tools/hle/synth_ex2.sh REPLAY SNAP 12 s2ring` (`mksyn_ex2.py s2ring`: 60 .. 119 sprite rectangles, more than the 0x4c0 .. 0xee0 ring holds) | 12 of 12 equal |
| mutations 366 (`rej_flush` wraps 8 bytes late: `ring_end + 8`), 367 (the wrapped pointer `ring_start + 8`), same synthetic run (`tools/hle/mutcheck_s2xbus.sh`) | both differ in 10 of 12 seeds (caught); in the game itself (3700 frames, verify) both pass with 0 differences: its tasks never reach the end of the ring. Not observable: `ring_gap` (the room check; the synchronous video plugin has taken every list, so the state that it guards declines, as in the Rej builds) |
| `tools/derive_check.sh DUMPS` | passes; every existing row unchanged but two composites: `l_b_s2_208` (also needs `l_b_s2_xbus` instead of `l_b_bg`; EXACT as before) and `l_b_ex2_208n_xbus` (primary `l_b_ex2_208`, also needs `l_b_s2_xbus`: 7 unexplained instead of 18, NONE as before); new row `l_b_s2_xbus`: NONE (51 unexplained words, as for every record derived without its own text), `unconfirmed 1` (`lay.out_toggle`), `expected 1` (`lay.out_bytes`) |
| `tools/derive_c_check.sh DUMPS` | 848 derivations, 17 mutants: 0 differences |
| `armlet-probe-replay --force` on the 10 texts of `mutcheck_probe.sh` | 10 PASS, records equal to the build before (`rec=equal`) |
| derivation of the loaded text of Command & Conquer, `b2b01e17177e5881` ("S2DEX xbus 2.06") | NONE with 0 unexplained words (1423 of 1612 aligned): 7 + 320 words of the reference's overlays without counterpart (docs/ucode-s2dex2.md section 9); no build, no probe, its tasks stay on the JIT |

## Waits for the CPU: the F3DEX2 front-end cuts and continues (`PRSP_POLL_IDLE=16`; 2026-10-18; docs/design.md "Waits for the CPU")

Gauntlet Legends (F3DEX fifo 2.06 `0bbe`): `G_DL` to itself, the JIT's idle cut, the calls that continue the task. Desktop (x86_64), `ref` and `armlet` both with `PRSP_POLL_IDLE=16`. Runs through `tools/equiv/desk.sh` (`EQUIV_TMO` for the slow ones).

| check | result |
|---|---|
| gate, 3700 f (`ref` against `armlet`: hash, state, 6 screenshots, audio) | identical: 1 228 230 calls, 6 of 6 screenshots, audio 5 525 504 bytes; 3583 of 3584 tasks native (was 207), 1 193 958 of 1 194 703 continued calls native (745 declined: SIG0, a yield request), 1 047 476 of them unchanged waits, 146 487 runs ended at a cut |
| `ARMLET_HLE_VERIFY=1`, 3700 f | 1 158 144 calls compared (3519 tasks, 1 154 625 continued), 0 differences; the cut compares the registers the loop carries (`F3_CUT_SREGS`), the input buffer and the unflushed RDP output (without the words the RDP ignores), the DMA registers, pc, SP_STATUS, DPC, MI |
| `ARMLET_HLE_VERIFY=1 ARMLET_HLE_EXACT=1`, 400 f | 30 055 calls, 0 differences (the exact-events compare too: an unchanged wait runs on the interpreter) |
| poison of what a cut does not keep, 3700 f: `ARMLET_HLE_POISON=1 HLE_POISON_SEL=s=6027e7f0,v=3ffffffc,x=d` at every native cut | identical to `ref` (hash, state, shots, audio): a0 .. a3, t0 .. t2, t5 .. t7, s0 .. s2, s5, sp, s8, v2 .. v29, the accumulator, the divide unit, VCC and VCE are dead at a cut. Poisoning VCO (`x=2`, all flags) changes the output at call 94 700: it is 0 at the end of every command (`vadd`, `vsub`) and stays so |
| mutations (`scripts/build.sh native --hmut=N`, `PRSP_POLL_IDLE=16`, `mut.sh`-style runs of 1200 f against `ref`) | **481** (the unchanged test says unchanged for a patched list): hash differs from call 21, 6 of 6 screenshots; **485** (s7 + 8 at the cut): hash from call 28; **486** (a viewport word at the cut): hash from call 11, 3 of 6 screenshots; **487** (the cut's pc + 4): state differs from call 11; **482** (`at` wrong at the cut) and **484** (SP_WR_LEN wrong): the gate does not see them (registers the next call overwrites); verify does: 23 509 and 949 of 25 100 calls differ in 300 f; **483** (the stale command's iteration left out): equivalent, the interpreter that goes on is as exact (0 differences with verify and `ARMLET_HLE_EXACT=1`, only slower) |
| the other 63 ROMs of the test and unseen sets, `ref` against `armlet`, 600 f each, `PRSP_POLL_IDLE=16` and default | 62 identical (hash, state, 6 shots, audio) in each; Donald Duck's hash differs at call 906 in both (state and pictures identical) and in the build before this change (`e10de8d`), with the option on or off: not from this |
| captured task starts (`ARMLET_HLE_CAPTURE`, ten games: DK64, Paper Mario, Pokemon Snap, Ocarina of Time, Blues Brothers 2000, Harvest Moon 64, Dr. Mario 64, Banjo-Tooie, Conker, Gauntlet Legends), `armlet-native-replay` | 369 snapshots: the build before and this build without the option: 47 declined (Gauntlet's), all equal; this build with `PRSP_POLL_IDLE=16`: 0 declined, all equal (the 47 are cuts); hmut 484 / 485 / 486: 47 of 48 Gauntlet snapshots mismatch (481 / 482 are in the continued calls, which the capture does not take) |
| synthetic tasks (`tools/hle/synth_ex2.sh`, 60 seeds of each case on a Blues Brothers 2000 snapshot) | identical for the build before and this build: clip, zneg, tri, mix, misc 60 of 60 equal, wild 57 equal / 3 declined, bad 60 declined; `lit` fails in both (the snapshot has no lighting overlay: 47 differ, 13 hung) |
| RSP ms per 1200 f (`time` runs, the shim's, video callbacks included), three runs each | `ref` 2376 / 2374 / 2366, `armlet` before 2400 / 2434 / 2439, this build 2292 / 2313 / 2340; option off, this build 2309 / 2346 / 2728 (an outlier), the build before 2426 / 2375 / 2476. One unchanged wait: JIT 0.9 us, front-end 0.04 us; a continued call that runs commands: 240 us on both sides (the RDP lists the call hands to the video plugin) |
| builds | native, aarch64 and their `--tools` clean; rg35xxsp not built here |

Development switches: `HLE_CUT_VR=1` prints the vector registers, flags, accumulator and divide unit that differ at each cut in verify (not part of the result), `HLE_CUT_SREG_ALL=1` reports every scalar register that differs, `HLE_POISON_SEL` `x=8` is VCC and VCE only. Mutation numbers 481 to 487 are this feature's.


## S2DEX 1.x (2026-10-18; docs/ucode-s2dex1.md)

- Kernels: `tools/hle/build_test_s2dex1.sh` and `test_s2dex1` compare each kernel (rect, rotated rect, BG 1-cycle, BG_COPY 1.06 / 1.07, sprite) with rsp2c translations of the microcode (`tools/hle/oracle/s1_*_gen.inc`, ROM-derived, git-ignored) on random inputs including stale vector registers; 0 differences.
- Replay: `HLE_PATCH` completes the tail (0xf18..0xf80) of a loaded text that a snapshot only holds in part (`mkpatch.py`-style: swap the host-word rows first); Yoshi's snapshots need it.
- tools/equiv (verify and the 3700-frame gate) for Yoshi, Bangaioh, MTC, PPL: hash, state, screenshots identical to `ref`; the other test and unseen games rerun on the final build, all PASS.
- Mutations 500-504, 510-517, 520-534, 540-561 (`tools/hle/mutcheck_s2dex1.sh`): kernel ones by the random test, front-end ones by replay; the guards that a game's values never reach by perturbed snapshots: 522, 523, 534 caught; 521, 529, 532, 533 (defensive guards, no perturbation of the 180 reaches them) NOT caught yet (`FUZZ=<dir>`, `tools/rsp2c/fuzz_snaps.py`, replayed with `--oracle`). The fuzz found the stale op cell after a store into the flags words (`s2_cells_changed`, MUT 534).
- Assumption shared with the other kernels: the vector flags are clear on entry (random flags make the microcode's result depend on VCO).

## Regressions of the 2026-10-18 recount (Mario Party 3, Chameleon Twist 2, Command & Conquer; 2026-10-18)

Desktop x86_64, 57 s wall-clock runs (`tools/ucode_coverage_run.sh`), the module before and after:

| | before | after |
|---|---|---|
| Mario Party 3 `d2d1` (`GfxProbe=on`) | 3170 of 3890 native, 719 declined `(no reason given)`, 6.3 ms JIT | 3884 of 3890 native, 5 declined `the validator held the front-end lock`, 0.6 ms JIT (`ARMLET_PROBE=off`: 3889) |
| Chameleon Twist 2, second launch | 2611 of 2688 native, 72 tasks without handler, 86 ms JIT | 3044 of 3072 native, 24 without handler, 30 ms JIT (sampling off: 0 and 5 ms) |
| Command & Conquer `65ad`, `loose`, 140 s | 0 native, unlatched for good | latched after 868 equal tasks, 3229 of 5888 native (its loaded `b2b0` latches after 1552 equal tasks at 64 s); `on`: 0 native |

- **No reason given.** The task that finds the front-end lock taken (a replay of the loaded Rej text of `9332` in progress: the replay is armed at the end of that task and the next task of the registered `d2d1` follows at once) counted its decline but did not set `K.why`, so the per-ucode statistics (`ustats.c`) printed `(no reason given)`. It sets it now, and waits for the lock for up to 1 ms (`FE_WAIT_NS`) before it declines. Cost: the wait is the replay's remaining time (about 0.2 ms per collision; Mario Party 3 spends 134 ms of emulation-thread time in 57 s on it, the JIT ran the 717 tasks in 6 ms). The proper fix (the front-ends' statics thread-local, no lock) was not attempted. The 9332 tasks' own JIT time (297 against 176 ms) is the capture: a task armed for the validator runs with its DMA events recorded.
- **Chameleon Twist 2.** The cached latch applies at the first task. The 72 to 78 tasks are the **sampling** of a latched build (one in 32, halving every 64 matches), which starts again at its first rate in every session; they run on the JIT by design (1.2 ms each). `GfxLatchPolicy resume=128` (new) credits the matches of the earlier sessions up to that many towards the decay, so the second launch starts at one in 128. `resume=0` is the old behaviour.
- **Command & Conquer `65ad`.** The validator replays declined with `G_LOAD_UCODE of another ucode` (every task loads the S2DEX xbus text `b2b0`; under `on` that text is NONE and never admitted, so a replay cannot take the switch; the validator did not log the reason, it does now: `validator: a replayed task was declined: ...`). Under `loose` `b2b0` is a candidate and the replay does take the switch, ends in `b2b0`'s layout, and the compare used `65ad`'s dead DMEM only: 68 bytes in `DMEM[418..]`, which `b2b0` declares dead (`418-f90`). The end-state compare accepts the dead bytes of the build the task ended in as well (`hle_dx.bound`) when it switched. The gate's masks have to say the same: `EXTRA_MASKS` for `65ad` with `418-f90` added (without it the hash stream of a task that ended in `b2b0` differs from the reference at the first native switched task, state, screenshots and audio identical).
- **Gates** (desktop, 3700 frames, `ref` against the module, hash / state / 6 screenshots / audio): Super Mario 64, Ocarina of Time, Kirby 64, Donkey Kong 64, Banjo-Kazooie (`armlet-hle`), Mario Party 3 and Chameleon Twist 2 (`ARMLET_PROBE=on`), Command & Conquer (`loose`, masks as above): identical; `ARMLET_HLE_VERIFY=1` with Super Mario 64, Kirby 64 and Mario Party 3 (`ARMLET_PROBE=on`): identical.
- **Mutations** (`--hmut=N`): 490 (no wait: the old decline) brings back 629 `d2d1` declines; 491 (a task that finds the lock taken runs without it): TSan reports the race in `f3dex2_run`, the unmutated build none; 492 (the dead bytes of the build the task ended in are not accepted) brings back `a real task differs (strike 1): UNLATCHED for good`. `tools/hle/test_latch.c` has the `resume` key (`mutcheck_latch.sh`: all mutations caught).

## F3DZEX.NoN fifo 2.08I: `bz_w` (Majora's Mask; 2026-10-19; docs/ucode-f3dex2-dialects.md section 8)

| check | result |
|---|---|
| `tools/derive_check.sh DUMPS` (82 fields compared) | every registered record unchanged (`bz_w = 0`); `l_b_mm` and `l_b_pm` derive each other with `bz_w` 1 / 0, `none` |
| `tools/derive_c_check.sh DUMPS` (`derive_refs.c` regenerated, `--check` passes) | 62 texts, 37 loo, 17 mutants, 849 shortcut: 0 differ |
| offline probe (`armlet-probe-replay --force`, Majora's Mask `1c72`) | 28 of 28 equal (was 25: `bz/0`, `bz/1`, `misc/1`); `tools/hle/mutcheck_probe.sh` with MM added (`probe_replay.c` now starts a task at `sp_pc = 0`: with 0x80 every task was taken for an idle-poll resume and declined): 11 PASS unmutated; 340: 11 FAIL; 341: 2 FAIL; **354** (`bz_w` inverted): 10 FAIL, 1 PASS (Ocarina of Time: `bz_w` already read the clip w there); with `bz_w = 0` MM is 25 of 28 (`rec=DIFF:bz_w`) |
| `tools/hle/mutcheck_ucache.sh` (`bz_w` in the round trip; `UCACHE_FORMAT` 8) | passes; 345 .. 349, 442, 454, 460, **461** (`bz_w` not read back) caught |
| synthetic tasks (`synth_ex2.sh`, 300 seeds per case, base: a captured MM task) | clip / zneg / tri / mix / misc 300 of 300 equal; wild 290 (10 declined), mtx 298 (2 declined); `lit` needs the overlay rows (not run: hangs without them). **321** (`bz_w` ignored by the front-end): `misc` 37 of 300 differ, `clip` 0 |
| equivalence, desktop (`desk.sh`, `ref` against `armlet`), Majora's Mask 6000 frames (the intro cutscene, Link on Epona, from frame ~3000) | hash 8463 / 8463, state 8471, 6 of 6 screenshots, audio identical; also with `ARMLET_HLE_VERIFY=1` and `ARMLET_HLE_POISON=1`: identical. Native share (`ARMLET_HLE_STATS`): 2431 of 2432 graphics tasks at the last print (the first task of a run never is), 0 declined |
| same, 3700 frames | Ocarina of Time (hash 4841, state 4834) and Paper Mario (7254, 7245): identical, screenshots 6 of 6, audio identical |
