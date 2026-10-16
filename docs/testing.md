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
| the same with the aarch64 build (NEON vector-unit backend, `scripts/build.sh aarch64 --tools`) under `qemu-aarch64-static` | 157 of 157 (Worms, Dr. Mario, WinBack d2d1), Conker 289 of 289 and 313 of 313, Worms 60 of 60 (the closed forms; the lit pass is scalar, the vertex pipeline under it NEON) |
| `ARMLET_HLE_VERIFY=1` equivalence runs, 600 to 3000 frames | WinBack (d2d1 tasks) PASS 600 f; Dr. Mario PASS 600 f; Conker PASS 1500 f (430 tasks native, 0 differences, 145 lit tasks declined by the transliteration then) and 2500 f with the transliteration enabled (895 tasks, 0 differences); Worms PASS 2000 f (1343 tasks, 0 differences); F-Zero X PASS 3000 f (33dc registered, but 1277 of its 1280 tasks decline: `G_LOAD_UCODE` of the Rej text) |
| `ARMLET_HLE_VERIFY=1`, 3700 frames (the 62 s of the other results), after the fixes below | WinBack 1471 tasks, Paper Mario 3583, Donkey Kong 64 1663, Worms 2879, Dr. Mario 3263, Conker with the transliteration enabled 1215: 0 differences, equivalence PASS each (Conker without it: 486 of 1216 tasks native, the rest lit and declined) |
| the same with the closed forms (S2DEX background, F3DEXBG lighting), 3700 f, `ARMLET_HLE_VERIFY=1` | Worms 2879 of 2880 tasks native, Conker **1215 of 1216 native, 0 declined**, 0 differences, equivalence PASS each (hash, state stream, 6 screenshots, audio identical); the same without verify: PASS each |
| `tools/hle/build_test_s2dex2.sh` (rectangle: NEON against scalar; background: closed form against the microcode, `S2BG_ORACLE`), `tools/hle/build_test_f3dexbg.sh` (the lit pass after the NEON and the scalar vertex forms), under qemu-aarch64 | 20 000 cases each, 0 failures |
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
| 100 .. 120 | the F3DEXBG lighting (`f3dexbg_fx.c`, `f3dexbg_dmem.h`): attenuation nibble, min / max, view-space constants, ambient, lambert masks, non-positional sum, vertex colour, texgen, flag, group, scratch words, light setup stride | all caught by the Conker snapshots (17 to 256 of 313 + 289) except 111 (lane group swapped: the two halves hold equal data in the captured tasks) and 114 (the w lane of the distance: zero in them), caught by the random test (including the point-light constants the old routine's mutants 62 to 64 could not reach) |
| 65, 66 | `G_TRI4` decode, `G_LOAD_UCODE` input buffer | caught (211 snapshots + 74 declined; all 289 declined: every task ends with the command) |

(A mutation that makes the routine loop forever is counted as caught: the replay is killed after 100 s.)

**A bug found by the longer runs, in the shared triangle code.** WinBack (3700 frames, `ARMLET_HLE_VERIFY=1`) had 120 tasks with a different RDP stream: the z block of a triangle read the fraction of the start value after adding the integer product, so a
saturated integer part (0x7fff) took the fraction to 0xffff where the microcode leaves the low bits (`fx2_tri_attrs`, scalar and NEON; the fraction is read before the integer product, as everywhere else). The four original games never hit it. After the fix all 201 captured WinBack tasks are equal.
The F3DEXBG lit routine's longer run (Conker, 3700 frames) showed 60 tasks whose light slot pad bytes beyond the ninth slot differed: the dead list now covers 13 slots.

**Closed forms replace the transliterations.** S2DEX's background (`s2dex2_bg.c`) and F3DEXBG's lit vertices (`f3dexbg_fx.c`) were first transliterations of the microcode (generated, exact, 0.8 to 1.5 times the JIT); they are hand-derived
closed forms now, checked against those transliterations first ("Regenerating the microcode oracles" below) and then against the JIT by the runs above. Not bit-exact corners are declined, not emulated: a background whose wrap loop would not end, an even number of
lights (the microcode's lighting loop never ends on it), more than 11 lights, DMEM vector constants other than S2DEX 2.08's.

## F3DEX2 2.04 and 2.04H (docs/ucode-f3dex2-dialects.md, section 5)

Builds `5dce` (F3DEX 2.04: Buck Bumble, Cruis'n World) and `e857` (2.04H: Super Smash Bros.; Kirby 64 declines in every task, section 5.6). Desktop (x86_64) unless said, `GfxMode=hle`, against the reference pairing, 3700 frames (62 s) unless said.

| check | result |
|---|---|
| equivalence gates, `ref`, `armlet-hle` and `armlet` (default) | Buck Bumble, Cruis'n World, Super Smash Bros., Kirby 64: hash stream, state stream, 6 screenshots, audio identical in both pairs |
| `ARMLET_HLE_VERIFY=1` (and with `ARMLET_HLE_EXACT=1`), 3700 f | Buck Bumble 3711 tasks, Cruis'n World 1663, Super Smash Bros. 3327: 0 differences, 0 declined; equivalence PASS each |
| `ARMLET_HLE_VERIFY=1`, 12000 f | Buck Bumble 11967 tasks, Cruis'n World 5695, Super Smash Bros. 10559: 0 differences |
| share of the graphics tasks of the 3700 frames that ran natively | Buck Bumble 3711 of 3712, Cruis'n World 1663 of 1664, Super Smash Bros. 3327 of 3328 (the first task of a run never is), Kirby 64 0 of 1792 (692 + 11 declines per 1500 frames: section 5.6) |
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

## Regenerating the microcode oracles

The closed forms of S2DEX 2.08 (`s2dex2_bg.c`, `s2dex2_rect.c`) and of F3DEXBG's lit vertices (`f3dexbg_fx.c`) were derived from the microcode and are checked against the microcode itself, besides the replays and verify runs above.
`tools/rsp2c/` is the one directory of this tooling: `rsp2c.py` (the translator, below), `fuzz_snaps.py` (perturbed copies of captured task snapshots) and `replay_each.sh` (one `armlet-native-replay` per snapshot with a time limit, for the perturbed ones).
`tools/rsp2c/rsp2c.py` translates the instructions of a handler (from an IMEM image, one C statement per instruction, on the exact vector-unit library `tools/hle/ref_vu.h`) into a function that does what the RSP interpreter
does. Those translations are ROM-derived and **not** part of the tree (`*_gen.inc`, `gen_*.inc` and `tools/hle/oracle*/` are git-ignored); they are regenerated from an IMEM image taken with `HLE_DUMP_IMEM=<file>` (the front-end writes
IMEM at the end of a task; for S2DEX the overlay that the handler lives in must be resident: A for the sprites, B for the backgrounds; for F3DEXBG the lighting overlay):

```
# S2DEX 2.08 sprite rectangle (Worms Armageddon): overlay A is IMEM [0x6c8, 0xe00); entered after the DMA of the sprite, done at 0x648, outside the scissor at 0x1ac
tools/rsp2c/rsp2c.py --name rect_gen --entry 0xba8 --stop 0x648 --stop 0x1ac --seg s2_ovA.img 0x6c8 0xe00 --out rect_gen.inc
# S2DEX 2.08 background: overlay B is IMEM [0x6c8, 0xe00)
tools/rsp2c/rsp2c.py --name bg1cyc_gen --entry 0x6fc --stop 0x154 --stop 0x1ac --stop 0x64c --stop 0xfc8 --seg s2_ovB.img 0x6c8 0xe00 --out bg1cyc_gen.inc
# F3DEXBG.NoN 2.08 lit G_VTX (Conker): matrix concatenation 0x080, lighting overlay 0x2d8, vertex handler 0x6d8, DMA and wait code 0xfc8
tools/rsp2c/rsp2c.py --name bg_vtx_gen --entry 0x6d8 --stop 0x194 --stop 0x224 --seg mtx.imem 0x080 0x0f4 --seg ov.imem 0x2d8 0x6d8 --seg ov.imem 0x6d8 0x994 --seg ov.imem 0xfc8 0x1000 --out bg_vtx_gen.inc
```

`tools/hle/build_test_s2dex2.sh` takes them as `S2RECT_ORACLE=rect_gen.inc` and `S2BG_ORACLE=bg1cyc_gen.inc`: random DMEM images, the closed form against the translation (the whole DMEM, the output pointer, drawn / outside the
scissor), also under qemu-aarch64 (without `HOST=1`). `S2RECT_VU=1` adds the instruction-by-instruction kernel that `s2dex2_rect.c` replaced as a second oracle (`tools/hle/oracle/s2dex2_rect_vu.c`, kept locally, ignored by git). A mutation
of a closed form that the replays do not catch must be caught by this test (`tools/hle/mutcheck_dialects.sh` with `S2RECT_ORACLE` set does both). The lit-vertex translation was compared with `f3dexbg_fx.c` by one-off harnesses on
captured and mutated command-entry states (results in the table above); they are not kept, the corpora being ROM-derived.
