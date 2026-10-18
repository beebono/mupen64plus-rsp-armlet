# Native coverage of the graphics tasks: a recount on 64 games

How much of the graphics RSP work does the task-level native path (`GfxMode = hle`, [design.md](design.md#task-level-native-path)) take today, and what is left on the JIT? This is a recount on
`devel` 91d3e2e (the F3DEX2 2.03 entry already unregistered) with a per-ucode counter added for it (`src/ustats.c`, `ARMLET_UCODE_STATS=<file>`; no effect when unset). It replaces the guess that
[ucode-survey.md](ucode-survey.md) made from the dumps (that one counted texts, this one counts tasks and time), and it is the "hot list" that [ucode-derive.md](ucode-derive.md) is run on. **It was run four times**: before the ucode id fix and the registration of the four derived builds (`devel` 91d3e2e) and after (2026-10-16), after the Rej front-end, and with the shipped defaults on a1d7c0e ([Result in numbers](#result-in-numbers) has all four); the tables below are the second.

## Method

- **Corpus.** All 23 games of `test-romset/` and all ROMs of `unseen-romset/`: the directory holds **41** files, not the 40 that [ucode-survey.md](ucode-survey.md) counts, so 64 games in all.
- **Run.** One game per run, headless on x86_64 (`SDL_VIDEODRIVER=offscreen`, foir `NEON=0` build behind arMLEt's coalescing path, the scripted pad of `tools/equiv/inp_det.c`), `GfxMode = hle`, 57 s of wall clock, six
  runs at a time in memory-capped scopes (`tools/ucode_coverage_run.sh`). That is 900 to 10 000 graphics tasks per game (title / menu / first seconds of play, as in the survey), 146 944 in the first recount and 146 560 in the second. The counters are written
  every 128 graphics tasks, so a run that is killed loses up to 127 tasks (and the one in flight), which is why a one-ucode game shows `tasks - 1` native.
- **Counters** (`src/ustats.c`, keyed by the ucode id of `gfx_ucode.c`; `tools/ucode_coverage.py` merges ids that differ only by `ucode_size` into one 4 KiB text: only the first recount needed it, since 2026-10-16 the id is the hash of the whole 4 KiB): per ucode the graphics tasks offered, the native runs, the
  declines with their reason (`hle_decline`'s string, `K.why`), the tasks of a ucode without a handler, and three times: native runs, declined native attempts (wasted), and the JIT's run of the tasks it took.
  **All times leave out the time inside the video plugin's `ProcessRdpList` callback** (`rdp_defer.c` times it): with it a native task of Banjo-Kazooie took 9.7 ms, mostly foir back-pressure at a full sync.
- **"Graphics RSP time on the JIT"** of a game = (JIT time of the tasks the JIT ran + the time wasted in declined native attempts) / (that + native time). It is a wall-clock share on the desktop x86_64 (the native front-end runs
  1.6 to 3.9 times the JIT's speed there; on the A55 the ratio differs, `docs/profiling.md` has the device numbers), so read it as "which ucodes the remaining JIT time belongs to", not as a device prediction.
  The profiler of `docs/profiling.md` was not used: it needs the `-prof` build and a per-ucode split it does not have; the counters above give the split directly.
- **Registered** means an `hle_ucode` entry with this exact id. The F3DEX2 front-end's `G_LOAD_UCODE` handling follows loaded builds by their own key, so S2DEX texts that a task loads are not counted as tasks of their own.

## Result in numbers

**Update 2026-10-18, after XBUS / Rej / the data signature (four passes, the "after" of the 2026-10-17 night recount).** Same corpus, method and machine as the "Method" section (112 of 113 games counted; 57 s per game, six runs at a time, x86_64 scalar foir, a private `armlet-ucode.cache` per game and mode), the module rebuilt from a copy of the `devel` tree with the uncommitted work of the day (`scripts/build.sh native --tools`). What the tree gained since the night recount: the `clip204` split, the F3D XBUS flag (Mischief Makers, War Gods), XBUS for the F3DEX2 xbus texts, StarCraft's S2DEX xbus dialect, Glover's XBUS-start tasks, Rej as a derivation family, the static data signature as the pair key, replay-noted loads, the latch policy's repeat floor (`repeat=0.05`) and evidence override (`override=4`), and the loose tier's default-record arm. Passes: (1) `ARMLET_PROBE=on`, fresh cache; (2) `on`, the cache of pass 1; (3) `loose`, fresh cache (no cache of the previous recount was reused, Glover's included); (4) `loose`, the cache of pass 3. Mischief Makers was checked on its log before the batch: `gfx ucode 4212d63f6e483348 F3D ... [native]`, 63 of 64 tasks native, no "not eligible" decline in any of the 452 logs. Raw output: `test-output/coverage-2026-10-18/` (`pass1`..`pass4`, `cache/on` and `cache/loose` = the caches after the first launch, `cache/on2` and `cache/loose2` = after the second, `md1..4.txt` / `txt1..4.txt` = `tools/ucode_coverage.py --md` / plain), git-ignored.

| 112 games | 2026-10-17 night: 1 `on`, first | 2 `on`, second | 3 `loose`, first | 4 `loose`, second | **today: 1 `on`, first** | **2 `on`, second** | **3 `loose`, first** | **4 `loose`, second** |
|---|---|---|---|---|---|---|---|---|
| graphics tasks offered | 263 040 | 264 064 | 263 808 | 263 936 | 263 936 | 263 936 | 264 064 | 263 936 |
| native | 205 446 (78.1 %) | 208 720 (79.0 %) | 208 445 (79.0 %) | 211 011 (79.9 %) | **220 067 (83.4 %)** | **224 315 (85.0 %)** | **222 573 (84.3 %)** | **226 780 (85.9 %)** |
| declined by a handler that exists | 17 278 | 17 453 | 17 406 | 17 440 | 9 177 | 8 999 | 9 191 | 8 926 |
| of a ucode without a handler | 40 219 | 37 798 | 37 859 | 35 391 | 34 592 | 30 528 | 32 199 | 28 135 |
| mean per-game share of tasks native | 78.5 % (test 85.6, unseen 88.4, not-coded 66.6) | 79.6 % (85.6, 89.3, 68.5) | 79.2 % (85.6, 88.4, 68.4) | 80.4 % (85.6, 89.3, 70.3) | **83.3 %** (90.7, 88.5, 75.3) | **85.3 %** (90.7, 89.3, 79.2) | **84.1 %** (90.7, 88.5, 77.2) | **86.1 %** (90.7, 89.3, 81.1) |
| mean share of graphics RSP time left on the JIT | 24.7 % (test 20.6, unseen 13.8, not-coded 36.1) | 24.3 % (20.8, 12.5, 36.0) | 24.2 % (20.6, 13.9, 34.6) | 23.6 % (20.5, 12.5, 34.5) | **19.5 %** (12.9, 13.9, 27.3) | **18.0 %** (12.8, 12.9, 24.8) | **18.8 %** (12.8, 13.9, 25.8) | **17.4 %** (13.0, 12.9, 23.2) |
| games all native (at most 1 task off) | 76 | 76 | 76 | 76 | 79 | 79 | 79 | 79 |
| games (almost) none native (under 2 % of the tasks) | 16 | 16 | 15 | 15 | 10 | 10 | 9 | 9 |
| games with half or more of their RSP time on the JIT | 27 | 27 | 26 | 26 | 22 | 20 | 21 | 19 |

The 64 games of the earlier recounts (test + unseen): native 84.2 / 85.0 / 84.3 / 85.0 % before, **87.3 / 87.9 / 87.3 / 87.9 %** today; mean JIT share 16.2 / 15.5 / 16.3 / 15.4 % before, **13.6 / 12.9 / 13.5 / 12.9 %** today; all native 49 before, 50 today; (almost) none native 4 before, 3 today (Yoshi's Story, Gauntlet Legends, Star Wars Rogue Squadron). The `not-coded` set alone (116 864 to 117 120 tasks): native 70.5 / 71.6 / 72.4 / 73.6 % before, **78.5 / 81.3 / 80.5 / 83.4 %** today; all native 27 before, 29 today; (almost) none native 12 / 12 / 11 / 11 before, **7 / 7 / 6 / 6** today. The three more all-native games are Mischief Makers (3199 of 3200 tasks), War Gods (4351 of 4352) and StarCraft 64 (1279 of 1280); the six that left the (almost) none native list in pass 1 are those three plus Ms. Pac-Man, Lode Runner 3-D and Twisted Edge (Killer Instinct Gold leaves it only under `loose`, as before). Run-to-run noise as in the previous recount: task totals differ by up to 0.4 % (today 263 936 / 263 936 / 264 064 / 263 936, the night 263 040 to 264 064), the per-set JIT shares by 0.1 to 0.3 points between identical configurations (test 12.9 / 12.8 / 12.8 / 13.0 in the four passes).

**Per game, against the 2026-10-17 night tables.** Native share / share of graphics RSP time on the JIT, the night recount, then today (bold); the four columns are the four passes. 10 of the 112 games changed by more than the noise (2 points of native share or 3 points of JIT share in a pass): the first ten rows below; the last three rows are expected movers that did not move. Every other game is within noise of the night tables (AeroGauge 58.2 to 60.0 % native in the first launches, its strict latch came at a slightly different moment, 96.2 % in both second launches; Donald Duck, Iggy's Reckin' Balls, Neon Genesis Evangelion, International Superstar Soccer 64: native share identical, JIT share within 2 points; SpaceStation Silicon Valley: native share identical at 56.8 %, JIT share 69 / 72 / 69 / 70 % before and 69 / 67 / 70 / 71 % today).

| game | set | tasks (night -> today, pass 1) | pass 1: `on`, first | pass 2: `on`, second | pass 3: `loose`, first | pass 4: `loose`, second |
|---|---|---|---|---|---|---|
| Mischief Makers | test | 3200 -> 3200 | 0.0% / 100% -> **100.0% / 0%** | 0.0% / 100% -> **100.0% / 0%** | 0.0% / 100% -> **100.0% / 0%** | 0.0% / 100% -> **100.0% / 0%** |
| War Gods | not-coded | 4352 -> 4352 | 0.0% / 100% -> **100.0% / 0%** | 0.0% / 100% -> **100.0% / 0%** | 0.0% / 100% -> **100.0% / 0%** | 0.0% / 100% -> **100.0% / 0%** |
| Glover | test | 6016 -> 6144 | 64.7% / 92% -> **82.5% / 15%** | 64.9% / 92% -> **82.4% / 14%** | 64.7% / 91% -> **82.5% / 14%** | 64.7% / 91% -> **82.4% / 15%** |
| Ms. Pac-Man - Maze Madness | not-coded | 3072 -> 3072 | 0.0% / 100% -> **64.4% / 66%** | 0.0% / 100% -> **97.3% / 6%** | 0.0% / 100% -> **64.4% / 65%** | 0.0% / 100% -> **97.3% / 6%** |
| Lode Runner 3-D | not-coded | 1536 -> 1536 | 0.0% / 100% -> **59.5% / 15%** | 0.0% / 100% -> **96.7% / 8%** | 0.0% / 100% -> **59.5% / 15%** | 0.0% / 100% -> **96.8% / 8%** |
| Twisted Edge - Extreme Snowboarding | not-coded | 1536 -> 1536 | 0.0% / 100% -> **84.2% / 15%** | 0.0% / 100% -> **96.7% / 12%** | 0.0% / 100% -> **84.2% / 14%** | 0.0% / 100% -> **96.7% / 12%** |
| StarCraft 64 | not-coded | 1280 -> 1280 | 0.0% / 100% -> **99.9% / 0%** | 0.0% / 100% -> **99.9% / 0%** | 0.0% / 100% -> **99.9% / 0%** | 0.0% / 100% -> **99.9% / 0%** |
| NFL Blitz | not-coded | 1536 -> 1536 | 39.1% / 99% -> **63.2% / 68%** | 56.2% / 99% -> **92.6% / 22%** | 39.1% / 99% -> **63.2% / 67%** | 56.2% / 99% -> **92.6% / 22%** |
| Chameleon Twist 2 | unseen | 3072 -> 3072 | 62.9% / 44% -> **67.6% / 50%** | 97.3% / 1% -> **97.3% / 17%** | 62.9% / 49% -> **67.6% / 48%** | 97.3% / 1% -> **97.3% / 16%** |
| Mario Party 3 | not-coded | 5632 -> 5760 | 87.4% / 40% -> **74.8% / 52%** | 87.3% / 39% -> **74.8% / 52%** | 87.3% / 39% -> **74.7% / 51%** | 87.3% / 40% -> **76.1% / 52%** |
| Command & Conquer | not-coded | 2048 -> 2048 | 0.0% / 100% -> **0.0% / 100%** | 0.0% / 100% -> **0.0% / 100%** | 0.0% / 100% -> **0.0% / 100%** | 0.0% / 100% -> **0.0% / 100%** |
| NBA Jam 2000 | not-coded | 1408 -> 1536 | 10.9% / 100% -> **10.0% / 100%** | 48.5% / 99% -> **48.5% / 99%** | 10.0% / 100% -> **10.0% / 100%** | 48.5% / 99% -> **48.5% / 99%** |
| Killer Instinct Gold | not-coded | 2688 -> 2688 | 0.0% / 100% -> **0.0% / 100%** | 0.0% / 100% -> **0.0% / 100%** | 88.7% / 27% -> **88.8% / 28%** | 88.7% / 28% -> **88.8% / 27%** |

What each mover is (the reasons are the counters' and the logs'):

- **Mischief Makers, War Gods** (F3D 2.0H `4212`, XBUS transport): 0 % to **100 %** native in all four passes (3199 of 3200 and 4351 of 4352; the first task of a run is the JIT's), JIT share 100 % to 0 %. The 7548 `not eligible (SP halted, RDP coalescing off or XBUS)` declines of the night table are gone.
- **StarCraft 64** (S2DEX xbus 2.08 / S2DEX 1.07 `3ee6`): a registered build now (log: `[native]`, no probe), 0 % to **99.9 %**, JIT share 100 % to 0 %.
- **Glover**: 64.7 % to **82.5 %** native, JIT share 92 % to 14 or 15 %: F3DEX 1.21 `208a` runs 5066 of 5067 tasks (the night run declined 1060 of them `not eligible`, the XBUS-start tasks), the remaining 17.5 % is F3D 2.0H `9d0d` (1077 tasks, NONE `225 of 1024` words, not admitted to the loose tier, 32 ms of JIT time).
- **Ms. Pac-Man, Lode Runner 3-D, Twisted Edge, NFL Blitz** (all latched probe builds): 0 % to **64.4 / 59.5 / 84.2 %** native in the first launch (the build latches after 1020 / 584 / 192 equal tasks, at 17.2 / 11.7 / 6.5 s of emulated time) and to **97.3 / 96.7 / 96.7 %** in the second (cached latch), JIT share 100 % to 66 / 15 / 15 % (first) and 6 / 8 / 12 % (second). Ms. Pac-Man's `a986` and Twisted Edge's `f8d3` derive EXACT now and Lode Runner's `cbe9` derives NEAR, where the night run said NONE (`901 / 900 of 1154` words) or had derived EXACT without a probe result (Twisted Edge, 16 probe starts, one per data id: today one data id, the probe passes 28 of 28). NFL Blitz 39.1 to **63.2 %** and 56.2 to **92.6 %** (second launch), JIT share 99 to 68 and 99 to 22 %: both its texts latch (`9798` after 258 tasks, the loaded Rej text `36f10dc3` after 206 equal tasks at 26.2 s); 189 + 63 tasks stay declined in the first launch (`G_LOAD_UCODE of another ucode`, `an earlier task with these pointers loaded another ucode`).
- **Chameleon Twist 2**: native 62.9 to **67.6 %** (first launch; the latch comes after 919 equal tasks at 15.6 s), 97.3 % in the second launch as before.
- **Command & Conquer: unchanged, still 0 % in all four passes** (100 % of its RSP time on the JIT). `65ad` now derives NEAR (xbus record `l_b_ex2_208n_xbus`; it was NONE `899 of 1154`) and the probe passes 28 of 28, but the real-task validation never gets anywhere: under `on` the validator replayed 1792 of 1792 captured tasks as **declined** (0 equal, 0 different; the log does not give the reason), so the candidate stays a candidate; under `loose` (pass 3) it is **unlatched for good** (`a real task differs (strike 1)`, 68 DMEM bytes differ, the first at `DMEM[418..423]`), the cache keeps the DEAD entry (pass 4 reuses it). The loaded S2DEX xbus text `b2b01e17` is admitted to the loose tier (`0 unexplained, 7 missing words`, 1423 of 1612 aligned), the probe passes 30 of 30, and it stays a candidate. The cause of the declined replays was not investigated.
- **NBA Jam 2000: unchanged** (10.9 to 10.0 % first launch, 48.5 % second, JIT share 99 to 100 %). Its strict `9798` latches (606 equal tasks, 21.1 s), but the loaded Rej text `36f10dc3` (derives NEAR, probe 28 of 28) is still a candidate after 57 s in all four passes (no latch line; NFL Blitz's latches at 26.2 s), so the 721 tasks `an earlier task with these pointers loaded another ucode` stay on the JIT: that is the 99 % JIT share.
- **Killer Instinct Gold** (`loose` only): unchanged, 88.7 to 88.8 % native, JIT 27 / 28 to 28 / 27 % (latch after 221 equal tasks, evidence 60.77, 37 distinct, 7.35 s of emulated time); under `on` it stays at 0 %.

**Worse than the night tables (beyond the noise).**

- **Mario Party 3: native 87.4 % to 74.8 %, JIT share 40 % to 52 %** (pass 1; 87.3 to 74.8 / 74.7 / 76.1 % and 39 / 39 / 40 to 52 / 51 / 52 % in the others; the task total grows by 128, 5632 to 5760, which is noise). Cause: the text it loads, `9332f30c` (F3DLX.Rej fifo 2.08 / S2DEX fifo 2.08, 733 tasks), derived "not admitted" (NONE) before and derives **NEAR** today (`l_b_rej_203`, the Rej family); the probe passes 28 of 28, the candidate validates real tasks (the last `validator:` line of pass 1 has 704 captured and replayed, 704 equal, 0 different), but it is **not latched within 57 s in any of the four passes** (the cached candidate of pass 2 and 4 is not reused as a latch). While it validates, 717 tasks of the registered `d2d1` decline with `(no reason given)` (3890 tasks, 3172 native; before 3889 of 3890), which cost almost nothing (5 ms of JIT time and 0.1 ms wasted for the 717), and the JIT time of the 733 tasks of `9332` itself rises from 176 / 166 ms (night, pass 1 / 2) to 297 / 296 ms (today). A single-game rerun (57 s, two runs at once) confirms the probe as the cause: `ARMLET_PROBE=off`: `d2d1` 3889 of 3890 native, `9332` JIT 158 ms; `on`: `d2d1` 3173 of 3890 native, 716 declined `(no reason given)`, `9332` JIT 266 ms. The reason of the 717 declines (probably the tasks that load the S2DEX text while its candidate is armed for capture: 717 is close to the 733 tasks of `9332`) and why the JIT runs slower with the validator armed were **not investigated**.
- **Chameleon Twist 2, second launch: JIT share 1 % to 17 % (16 % in pass 4) with the native share unchanged at 97.3 %.** The 78 tasks before the cached latch applies (no handler) cost 87 ms of JIT time today against 5 ms for 81 tasks in the night run; the first launch is 44 to 50 % (990 tasks, 395 ms, against 1138 tasks and 323 ms), with 4.7 points more tasks native. The same 48 to 78 start tasks cost 87 to 107 ms in the other second launches (Ms. Pac-Man 87 ms, Lode Runner 104, Twisted Edge 107; they had no cached latch to compare with in the night run). Not investigated (a cold JIT start, or the probe's cache path running on the first tasks).

No other game got worse; the totals (4 passes) improved by 5.3 to 6.0 points of native share and 5.2 to 6.3 points of JIT share. **Decline reasons**, pass 1, over all 112 games: 9177 tasks (night 17 278): the status poll time-out (3068, Gauntlet Legends), `an earlier task with these pointers loaded another ucode` (1902, 4 games), geometry bit 12 (997, Indiana Jones), `S2DEX LDTX_SPRITE / LDTX_RECT_R` (813, Neon Genesis Evangelion), `G_MOVEWORD offset` (769, Blast Corps), `(no reason given)` (717, Mario Party 3, new), `G_LOAD_UCODE of another ucode` (599, 4 games), unimplemented command (147), `class-0 operand longer than the operand buffer` (134, Wetrix and Wipeout 64), `a command kind the latched build has not run in a validated task` (24, 6 games), 5 + 1 + 1 others. `not eligible (SP halted, RDP coalescing off or XBUS)`, 8608 tasks in the night table, is gone.

**What is left on the JIT** (the hot list below, pass 1, mean over the 112 games of the share of a game's graphics RSP time): S2DEX 1.07 / S2DEXD 1.07 `190d` (Bangaioh, Magical Tetris, Pokemon Puzzle League) 2.67 points, `9798` (the F3DEX 2.05 text of Chameleon Twist 2, NBA Jam 2000, NFL Blitz: the first launch before the latch, the loaded-Rej-text declines of NBA Jam) 1.94, Armorines `7847` 0.89, Command & Conquer `65ad` 0.89, Killer Instinct Gold `bf9b` 0.89 (not under `loose`), Majora's Mask `1c72` 0.89, Perfect Dark `eb2c` 0.89, Yoshi's Story `5b32` 0.89, Rogue Squadron `bfbf` 0.89, Gauntlet Legends `0bbe` 0.89, F3DLX 1.21 `76c7` 0.85, Indiana Jones `a6c6` 0.83, ECW `23cb` 0.82, Blast Corps `3e9b` 0.79, then the rest below 0.7 points each. The four XBUS rows of the night list (F3D 2.0H `4212`, Lode Runner `cbe9`, Ms. Pac-Man `a986`, StarCraft `3ee6`) are gone or below 0.6 points; `9332f30c` (Mario Party 3) is new at 0.45 points and `d2d1` (the 717 declines) at 0.01.

**`loose` against `on`.** The loose tier **admitted two pairs** in 113 games (the strict bound of 16 unexplained plus missing words): **Killer Instinct Gold's F3D 2.0D `bf9b`** (`0 unexplained, 3 missing words, 912 of 1024`; probe 26 of 26 equal; latched after 221 equal tasks, evidence 60.77, 37 distinct, 7.35 s; native 0 to **88.7 %**, JIT share 100 to 28 %) and **Command & Conquer's loaded S2DEX xbus text `b2b01e17`** (`0 unexplained, 7 missing words`; probe 30 of 30 equal; a candidate, never latched in the 57 s, so nothing changes in the game). The default-record arm (beyond the bound, the family's record as derived) probed six more texts: Armorines `7847` (49 + 3; **FAIL**, 22 of 28 differ, 6 equal), ECW `23cb` (22 + 21; **FAIL**, 7 of 28 differ, 3 declined), Perfect Dark `eb2c` (45 + 27; **FAIL**, the build declined 26 of 26), Blast Corps `2f41` (**FAIL**, 26 of 26 differ) and `1d12` (**FAIL**, 26 of 26 declined), and Wipeout 64 `3c7a` (L3DEX 1.21; 511 + 38; **PASS 28 of 28**, a candidate, not latched in 57 s, as in the night recount). Latches under `loose`: Killer Instinct Gold's `bf9b` only (plus the strict ones that `on` has too: AeroGauge, Donald Duck, NBA Jam 2000, NFL Blitz, Chameleon Twist 2, Ms. Pac-Man, Lode Runner, Twisted Edge, NFL Blitz's Rej text). **Numeric mismatches: 0** (every cache entry, `numeric` and `mismatches` = 0 except Command & Conquer's `65ad`, which has 1 mismatch of the real-task kind, 0 numeric); no loose build was ever latched with a nonzero count. In pass 4 the Killer Instinct Gold latch is again not reused (the same derivation, probe and latch after 221 tasks, as before: an unclean end makes a loose latch a candidate again). The share difference: pass 3 against pass 1: test 12.9 to 12.8, unseen 13.9 to 13.9, **not-coded 27.3 to 25.8** (-1.5 points); pass 4 against pass 2: 12.8 to 13.0, 12.9 to 12.9, **24.8 to 23.2** (-1.6); all 112 games 19.5 to 18.8 and 18.0 to 17.4. All of the not-coded difference is Killer Instinct Gold (JIT share 100 % to 28 %). Native share: not-coded 75.3 to 77.2 % and 79.2 to 81.1 %, all 112 games 83.3 to 84.1 % and 85.3 to 86.1 %. No loose-tier pair was probed or latched in the test or unseen sets except the Blast Corps (FAIL) and Wipeout 64 (candidate) arms above.

**Probe results of the strict tier** (same in `on` and `loose`; the full table is "Every ucode seen in the not-coded set" below): latched strictly: `efe4` (AeroGauge 473 equal tasks at 18.9 s, Donald Duck 33 tasks at 0.9 s), `9798` (NBA Jam 2000 606, NFL Blitz 258, Chameleon Twist 2 919), `a986` Ms. Pac-Man (1020), `cbe9` Lode Runner (584, NEAR), `f8d3` Twisted Edge (192), NFL Blitz's Rej text `36f1` (206, NEAR). Candidates not latched in 57 s: Mario Party 3 `9332` (NEAR, 28 of 28), NBA Jam 2000's `36f1`, Iggy's `dff3` (NEAR, 28 of 28, 4 % of its tasks), Command & Conquer `65ad` (see above) and `b2b0`. The failed probe: Majora's Mask `1c72` (NEAR, 25 of 28, 3 differ, cached as failed). Cached latches were reused in every second launch (`latched in an earlier session`).

**Crashed, hung or timed out.** Nothing crashed or hung: no abort, assert, segfault or `terminate called` line in any of the 452 logs, and no `RSP Error`; every run ended on the 57 s `timeout -s KILL`, by design. The only `RSP Warning` besides the config-version notice of every run is Command & Conquer's `latch: ... a real task differs (strike 1): UNLATCHED for good` (pass 3). One game left no counter file again: **40 Winks** (all four passes): the log shows 3120 video frames and one graphics ucode (F3DEX.NoN fifo 2.08 `9875`, `[native]`) but fewer than 128 graphics tasks, and the counters are written every 128 tasks (112 games counted, 113 run).

**Update 2026-10-17 (night), after phase 3: the loose tier's default-record arm (passes 5 and 6).** Same corpus, method and machine as the "before phase 3" block below; the module was rebuilt from the tree with the default-record arm (a NONE derivation beyond the 16-word bound is tried with the family's record as derived when the family explains at least half of the image, latch at 3x evidence; the log banner reads `default record x 3.00`). Pass 5 = `ARMLET_PROBE=loose`, fresh cache (`cache/loose3/`); pass 6 = `loose`, the cache of pass 5. Raw output: `test-output/coverage-2026-10-17/pass5`, `pass6`.

| 112 games | 3: `loose`, first (before) | 5: `loose`, first (after) | 4: `loose`, second (before) | 6: `loose`, second (after) |
|---|---|---|---|---|
| graphics tasks offered | 263 808 | 263 936 | 263 936 | 263 808 |
| native | 208 445 (79.0 %) | 208 565 (79.0 %) | 211 011 (79.9 %) | 210 880 (79.9 %) |
| declined by a handler | 17 406 | 17 414 | 17 440 | 17 443 |
| no handler | 37 859 | 37 859 | 35 391 | 35 391 |
| mean per-game share of tasks native | 79.2 % | 79.2 % | 80.4 % | 80.4 % |
| mean share of graphics RSP time on the JIT | 24.2 % (test 20.6, unseen 13.9, not-coded 34.6) | 24.2 % (20.5, 14.0, 34.6) | 23.6 % (20.5, 12.5, 34.5) | 23.6 % (20.6, 12.5, 34.5) |
| games all native / (almost) none native / half or more on the JIT | 76 / 15 / 26 | 76 / 15 / 27 | 76 / 15 / 26 | 76 / 15 / 26 |

**The arm changed nothing in the numbers.** The JIT share difference per set, pass 3 to pass 5 (test 20.6 to 20.5, unseen 13.9 to 14.0, not-coded 34.6 to 34.6) and pass 4 to pass 6 (20.5 to 20.6, 12.5 to 12.5, 34.5 to 34.5), is run-to-run noise (0.1 point); the "no handler" task counts are identical; no game changed its native share (Killer Instinct Gold stays at 88.7 %, from the strict-bound loose admission of before). The 64-game figures are 84.3 % native / 16.3 % JIT (pass 5) and 85.0 % / 15.4 % (pass 6). The one count that moved, "half or more on the JIT" 26 to 27 in pass 5, is Chameleon Twist 2 at the 50 % border (JIT share 49.4 % in pass 3, 51.5 % in pass 5; the strict latch comes at a slightly different moment of the first launch), not an effect of the arm. All the probes of the arm, per text (**no pair was latched**; numeric mismatches: 0, since nothing latched; in pass 6 nothing was reused, every failed, inconclusive or candidate result was probed again):

| game | text id | data ids tried | aligned | probe | state | native share (pass 5 / pass 6) |
|---|---|---|---|---|---|---|
| Blast Corps | F3D 2.0D `2f411927a2df64a8` | 1 | 885 of 1024 (32 unexplained, 29 missing), f3d record | FAIL, 26 of 26 canned tasks differ | refused | 54.8 % / 54.8 % (the game's other text is unchanged) |
| Blast Corps | F3D 2.0D `1d12f4b31150a7ba` | 1 | 518 of 1024 (507 + 34), f3d record | FAIL, the build declined 26 of 26 canned tasks | refused | same |
| Wipeout 64 | L3DEX 1.21 `3c7a64e0b92bd7f1` | 1 | 525 of 1024 (511 + 38), f3dex1 record | **PASS 28 of 28 equal** | **candidate, not latched**: 1376 of 1376 real tasks replayed equal by the validator, but the latch (3x evidence; `unexplained words in the other region; no kind gate covers them`) did not come in 57 s; stored `open`, 0 matches, 0 numeric | 71.2 % / 71.3 % (as before: this text is 14 % of the game's tasks) |
| Armorines | F3DEX.NoN/L3DEX fifo 2.05 `7847866d95f40933` | 1 | 941 of 1154 (49 + 3), f3dex2 record | FAIL, 22 of 28 differ (first difference: case `clip/0`) | refused | 0 % / 0 % |
| Command & Conquer | F3DEX.NoN/L3DEX xbus 2.06 `65ad6e1a2bf9e9ff` | 1 | 899 of 1154 (49 + 45), f3dex2 record | inconclusive, 0 of 28 conclusive (the RDP reads its lists from DMEM, XBUS) | no build can be validated | 0 % / 0 % |
| Ms. Pac-Man - Maze Madness | F3DEX xbus 2.08 `a9864f29d85ed52c` | 16 | 901 of 1154 (48 + 45), f3dex2 record | 10 inconclusive (XBUS), 6 with no result in 57 s | no build can be validated | 0 % / 0 % |
| Lode Runner 3-D | F3DEX xbus 2.07 `cbe9a9f21ad75f27` | 5 | 900 of 1154 (48 + 45), f3dex2 record | 4 inconclusive (XBUS), 1 with no result | no build can be validated | 0 % / 0 % |
| StarCraft 64 | S2DEX xbus 2.08 `3ee636569037de5b` | 2 | 1277 of 1942 (108 + 43), f3dex2 record, 30 canned tasks | inconclusive, 0 of 30 conclusive (XBUS) | no build can be validated | 0 % / 0 % |
| Perfect Dark | (no banner) `eb2cc8ed91b299e1` | 1 | 914 of 1024 (45 + 27), f3d record | FAIL, the build declined 26 of 26 canned tasks | refused | 0 % / 0 % |

Not admitted by the arm (family explains under half of the image, or the family is not covered): F3DLX.Rej 1.21 `3c51` (425 of 1024), F3DEX.Rej fifo 2.08 `23cb` (392 of 1024), F3DLX.Rej/S2DEX fifo 2.08 `9332` (713 of 1482), Wipeout 64's F3D 2.0H `968d` (244 of 1024), and, though the family explains 60 % of the image and it was expected as a candidate, **S2DEX 1.07 / S2DEXD 1.07 `190d`** (Bangaioh, Magical Tetris: 1166 of 1944 aligned; `not admitted to the loose tier`, "f3dex2 family"; not investigated why the arm does not take it). So of the expected candidates, `65ad`, `7847`, `a986`, `cbe9`, `3ee6` and `eb2c` were admitted, `190d` was not, and `9332`, `3c51` and `23cb` stayed refused as expected. Blast Corps' `2f41` and `1d12` and Wipeout 64's `3c7a` were also admitted. The pass 6 results equal pass 5's (failures are re-probed). **What stops the arm:** the XBUS ucodes (Command & Conquer, Ms. Pac-Man, Lode Runner 3-D, StarCraft 64) cannot be probed (the canned tasks are inconclusive), the games with 5 and 16 data ids spend the probe on data ids that change (below), and the four FAILs are real differences or declines. The only pass is a candidate with 1376 clean replays and no latch.

**Crashes, hangs.** None: no abort, assert or segfault line in the 224 logs of passes 5 and 6; all runs ended on the 57 s kill; 40 Winks again left no counter file (fewer than 128 graphics tasks).

**Twisted Edge, F3DEX.NoN fifo 2.06 `f8d3b2039771caa8`: why the probe never finishes.** The log shows 16 data ids (16 probe starts, no result, in all of the passes 1 to 6); a dump run of 40 s with the survey shim (`tools/ucode_dump_run.sh`, the module behind `udump.so`) saw **1082 graphics tasks and 1082 distinct data ids** (every task a new one), all with `ucode_size` 0x1000 and **`ucode_data_size` 0x800**. The probe hashes all 0x800 bytes of the data segment as RDRAM holds them; in 40 tasks 114 bytes differ, all within **0x422 .. 0x57f** of the segment (a region the game rewrites for each task, in 4-byte steps from 0x422 on). So each task is a new (text, data) pair, none sees the 7 graphics tasks (4 canned tasks each) its probe needs before the next pair arrives, and the pair table fills with the first 16. A hash over the part of the data the microcode reads, not the game's scratch, would make it one pair (not done).

**Update 2026-10-17 (night), "before phase 3": a recount on 112 games with the `clip204` split in (Chameleon Twist 2 derives EXACT) and the opt-in loose tier, four passes.** `devel` f1c3c70 plus the uncommitted work of the day (the tree copied, `scripts/build.sh native --tools`), `GfxMode = hle`, x86_64 scalar foir, 57 s per game, six runs at a time, a private fixed `armlet-ucode.cache` per game and mode. Three sets: `test-romset/` (23), `unseen-romset/` (41) and the new **`not-coded-romset/`** (49 games the native code never saw; 48 left counters, see below). The passes: (1) `ARMLET_PROBE=on`, fresh cache; (2) `on`, the cache of pass 1; (3) `ARMLET_PROBE=loose`, fresh cache; (4) `loose`, the cache of pass 3. 112 of 113 games wrote a counter file in every pass. Raw output: `test-output/coverage-2026-10-17/` (git-ignored).

| 112 games | 1: `on`, first | 2: `on`, second | 3: `loose`, first | 4: `loose`, second |
|---|---|---|---|---|
| graphics tasks offered | 263 040 | 264 064 | 263 808 | 263 936 |
| native | 205 446 (78.1 %) | 208 720 (79.0 %) | 208 445 (79.0 %) | 211 011 (79.9 %) |
| declined by a handler that exists | 17 278 | 17 453 | 17 406 | 17 440 |
| of a ucode without a handler | 40 219 | 37 798 | 37 859 | 35 391 |
| mean per-game share of tasks native | 78.5 % (test 85.6, unseen 88.4, not-coded 66.6) | 79.6 % (85.6, 89.3, 68.5) | 79.2 % (85.6, 88.4, 68.4) | 80.4 % (85.6, 89.3, 70.3) |
| mean share of graphics RSP time left on the JIT | **24.7 %** (test 20.6, unseen 13.8, not-coded 36.1) | **24.3 %** (20.8, 12.5, 36.0) | **24.2 %** (20.6, 13.9, 34.6) | **23.6 %** (20.5, 12.5, 34.5) |
| games that run all their graphics tasks native (at most 1 task off) | 76 | 76 | 76 | 76 |
| games that run (almost) none native (under 2 % of the tasks) | 16 | 16 | 15 | 15 |
| games with half or more of their RSP time on the JIT | 27 | 27 | 26 | 26 |

The same four passes restricted to the 64 games of the earlier recounts (test + unseen), against the evening recount (first launch 84.5 % native, mean JIT share 16.8 %; second launch 84.5 %, 16.9 %): pass 1 **84.2 %** native (123 081 of 146 176), mean JIT share **16.2 %**, 49 games all native, 4 (almost) none native; pass 2 85.0 %, 15.5 %; pass 3 84.3 %, 16.3 %; pass 4 85.0 %, 15.4 %. (The (almost) none native four are Yoshi's Story, Star Wars Rogue Squadron, Mischief Makers and Gauntlet Legends, which the evening list did not name: 2 of 3072 tasks native. Chameleon Twist 2 left the list.) The `not-coded` set alone: 116 864 to 117 120 tasks, 70.5 / 71.6 / 72.4 / 73.6 % native, mean JIT share 36.1 / 36.0 / 34.6 / 34.5 %; 27 of its 48 counted games are all native and 11 to 12 are (almost) none native (Armorines, Bangaioh, Command & Conquer, Killer Instinct Gold in passes 1 and 2, Majora's Mask, Lode Runner 3-D, Magical Tetris Challenge, Ms. Pac-Man, Perfect Dark, StarCraft 64, Twisted Edge, War Gods). Run-to-run noise: the task totals are wall-clock-limited and differ by up to 0.4 %; the per-set JIT shares move by 0.1 to 0.3 points between identical configurations (the test set 20.6 / 20.8 / 20.6 / 20.5).

**Against the evening table (per game, 64 games).** Only three games changed by 2 points of native share or 3 points of JIT share in any pass: **Chameleon Twist 2** (F3DEX 2.05 `9798`, strict EXACT with `clip204`): 0 % native (100 % of its RSP time on the JIT) in the evening, now **62.9 % native first launch** (it latches after about 20 s; JIT 44 % in pass 1, 49 % in pass 3) and **97.3 % second launch** (JIT 1 %, both `on` and `loose`): native, as intended. **Donald Duck** 94.7 % to 96.9 % in the second launches (JIT 22 % to 13 %, the cached latch of `efe4`; first launch 94.3 %). **Wetrix** (F3DEX.NoN 1.21 `fd5a`) went the other way: 100 % native (0 % JIT) to **93.2 % native, 22 to 24 % of its RSP time on the JIT** in all four passes, because 120 of its 1792 tasks decline with `class-0 operand longer than the operand buffer` (a reason that did not appear in the evening table; the only other game with it is Wipeout 64, 14 tasks of `76c7`). The wall-clock run reached a later part of the game than the evening one (1792 tasks against 2432 at 3.5 ms each), so this may be a section the evening run never got to rather than a regression of the code; not checked. SpaceStation Silicon Valley: native share unchanged (56.8 %), JIT share 73 % to 69 % (70 % in pass 4). Every other game of the two sets is within noise of the evening table.

**`loose` against `on`.** The loose tier admitted **one** pair in 113 games, all four passes: **Killer Instinct Gold's F3D 2.0D `bf9b120728efbf0a`** (data `3a828d411896ded0`; "0 unexplained, 3 missing words, 912 of 1024 aligned": the default-record arm is not involved, it is inside the word bound). The probe passed it (26 of 26 canned tasks equal, `derived NONE (loose tier)`, primary `l_mm`), the log says `unexplained words in the other region; no kind gate covers them`, and it **latched after 223 equal tasks (evidence 60.14, 38 distinct)**; native share **0 % (`on`) to 88.7 %** (2385 of 2688 tasks; the rest are the tasks before the latch), JIT share 100 % to 27 to 28 %. Numeric mismatches: **0 stored** (the cache entry written at the latch has `numeric` 0 and `mismatches` 0); the entry's counters after the latch are not observable here, because the 57 s run is killed (the INFO line with the final counts is written at shutdown) and the cache is stored at the latch, not afterwards. In pass 4 the loose latch was **not** reused: Killer Instinct Gold derives, probes (26 of 26) and latches again after 223 equal tasks exactly as in pass 3, because an unclean end with a latched loose build makes it a candidate again (design.md, "The loose tier"; the recount kills the run). So the second-launch loose figures equal the first-launch ones for this pair; the strict latches (AeroGauge `efe4`, NBA Jam 2000 and NFL Blitz `9798`, Chameleon Twist 2, Donald Duck) were reused ("latched in an earlier session"). Every other NONE text, in all three sets, was **not admitted** (`not admitted to the loose tier` in the pass-3 logs; the bound is 16 unexplained plus missing words). The share difference: pass 3 against pass 1: test 20.6 to 20.6, unseen 13.8 to 13.9, not-coded **36.1 to 34.6** (-1.5 points); pass 4 against pass 2: 20.8 to 20.5, 12.5 to 12.5, **36.0 to 34.5**; all 112 games 24.7 to 24.2 and 24.3 to 23.6. All of the not-coded difference is Killer Instinct Gold (JIT share 100 % to 27 %). No loose-tier pair was probed or latched in the test or unseen sets. Chameleon Twist 2's `9798` derives EXACT (strict) in both modes, as the same text does in NBA Jam 2000 and NFL Blitz; it is not a loose candidate.

**What the other probes did (all four passes, same in `on` and `loose`).** Derived EXACT and passed the probe, latched strictly: F3DEX.NoN 1.23 `efe4` (AeroGauge: 28 of 28 equal, latched after 496 equal tasks, 58 % native first launch, 96.2 % second) and F3DEX fifo 2.05 / F3DLX.Rej fifo 2.05 `9798` (NBA Jam 2000, NFL Blitz: 27 of 28 equal, 1 declined; latched after 606 and 267 equal tasks; 11 % and 39 % native first launch, 49 % and 56 % second: the rest declines `a command kind the latched build has not run in a validated task`, 593 and 617 tasks, and 100 % / 99 % of their RSP time stays on the JIT). The failed probe: Majora's Mask's F3DZEX.NoN 2.08I `1c72` (derived NEAR, 25 of 28 equal, 3 differ: FAIL, cached as failed). Derived EXACT with **no probe result at all**: Twisted Edge's F3DEX.NoN fifo 2.06 `f8d3` (0 of 1536 tasks native in every pass): the probe started 16 times in 57 s, once per data id (`probing with 28 canned tasks`, 16 distinct data ids), and never reported; probably the data hash of this game's tasks changes from task to task, so no pair gets to finish (not investigated). That is the one open anomaly of the run.

**Crashed, hung or timed out.** Nothing crashed or hung (no abort, assert or segfault line in any of the 452 logs; every run ended on the 57 s `timeout -s KILL`, by design). One game left no counter file: **40 Winks** (all four passes): the log shows 3120 video frames and one graphics ucode (F3DEX.NoN fifo 2.08 `9875`, native), but fewer than 128 graphics tasks were offered, and the counters are written every 128 tasks, so it is not in the numbers (112 games counted, 113 run). The not-coded directory holds 49 files, the other sets 23 and 41.

**Update 2026-10-17 (evening), the official recount with the shipped defaults: `GfxMode = hle`, `GfxProbe = on`, `devel` a1d7c0e (GoldenEye 007 registered, Donald Duck and the probe's latching in), same corpus and method (57 s per game, six runs at a time, x86_64 scalar foir), a private fresh `armlet-ucode.cache` per game.** The games table, the decline reasons and the hot list below are this recount's **first launch** (empty cache; the probe validates in the background and latches during the 57 s). It was run a second time with each game's cache kept from the first run (**second launch**, a cached latch starts native at once):

| | 2026-10-17 (46537eb, probe off) | first launch (a1d7c0e) | second launch (cached latch) |
|---|---|---|---|
| graphics tasks offered (64 games) | 147 072 | 141 893 | 139 845 |
| native | 119 353 (81.2 %) | **119 890 (84.5 %)** | 118 166 (84.5 %) |
| declined by a handler that exists | 10 027 | 10 080 | 9 720 |
| of a ucode without a handler | 17 634 | 11 864 | 11 901 |
| mean per-game share of tasks native | 83.5 % | **86.5 %** | 86.5 % |
| mean share of graphics RSP time left on the JIT | 19.2 % | **16.8 %** (test 19.8, unseen 15.2) | 16.9 % (test 19.9, unseen 15.2) |
| games that run all their graphics tasks native | n/a | 50 | 50 |
| games that run (almost) none native | 6 | 4: Yoshi's Story, Chameleon Twist 2, Star Wars Rogue Squadron, Mischief Makers (1 of 3200) | the same |
| games with half or more of their RSP time on the JIT | n/a | 11 | 11 |

The two launches agree within the run-to-run noise of a wall-clock-limited count (the task totals differ because offline replay jobs shared the machine with both passes, so the wall-clock-limited runs got a little less CPU; the JIT share differs by 0.1 point). GoldenEye 007 (1407 of 1408) and Donald Duck (2060 of 2176 first launch, 1347 of 1408 second) are native now; Iggy's Reckin' Balls' two texts run 1850 of 1920 (the F3DEX.NoN 1.22 `dff3` stays with no handler). The per-game cache file was written for 11 games in the first pass (Blast Corps, Glover, SpaceStation, Yoshi's Story, Chameleon Twist 2, Donald Duck, Iggy, Pokemon Puzzle League, Rogue Squadron, WinBack, Wipeout 64). What is left of the 16.8 points: Mischief Makers' F3D 2.0H XBUS tasks, Gauntlet Legends' status poll time-out, Blast Corps' F3D 2.0D `3e9b` / `2f41`, Glover (XBUS, F3D 2.0H `9d0d`), Wipeout 64 (F3D 2.0H `968d`, L3DEX 1.21), Pokemon Puzzle League and Yoshi's Story (S2DEX 1.06 / 1.07), Chameleon Twist 2's F3DEX 2.05 `9798`, Rogue Squadron, Indiana Jones (geometry bit 12) and SpaceStation. The updates below are the earlier recounts, kept as history.

**Update 2026-10-17, after the Rej front-end ([ucode-rej.md](ucode-rej.md)): F3DLX.Rej xbus 2.08 and the F3DEX.NoN xbus text it loads (Excitebike 64), F3DLX.Rej 2.03, F3DFLX.Rej 2.03F and F3DEX 2.03 (F-Zero X), F3DEX.Rej 2.06 (Resident Evil 2); `native-rej` on `devel` 46537eb, same corpus and method, 57 s per game, three runs at a time.** The tables below are this recount; the previous one (the 2026-10-16 update) is the "before":

| | before (46537eb) | after |
|---|---|---|
| graphics tasks offered (64 games) | 146 560 | 147 072 |
| native | 110 580 (75.5 %) | **119 353 (81.2 %)** |
| declined by a handler that exists | 12 003 | 10 027 |
| of a ucode without a handler | 23 921 | 17 634 |
| mean per-game share of tasks native | 79.6 % | **83.5 %** |
| mean share of graphics RSP time left on the JIT | **23.4 %** (test 27.6, unseen 21.1) | **19.2 %** |
| games that run (almost) none native | 8 | 6: GoldenEye 007, Yoshi's Story, Chameleon Twist 2, Donald Duck, Star Wars Rogue Squadron, Mischief Makers (1 of 3200) |

The run-time probe ([ucode-derive.md](ucode-derive.md), `GfxProbe`, off by default) is not in these numbers: it derives records for unregistered F3DEX2-family texts at run time and registers them dynamically; the figures above count registered builds only, so every game counted native here is native with the probe off.

Excitebike 64, F-Zero X and Resident Evil 2 went from 0 / 0 / 68 % to 100 % native (the first task of each run is counted with the JIT: `tasks - 1`); the 4.2 points that left the JIT are theirs (F-Zero X 100 % of its RSP time, Excitebike 64 100 %, Resident Evil 2's quarter). What is left of the 19.2 points
is S2DEX 1.0x, the F3D 2.0G / 2.0H re-layouts, the XBUS transport of two F3D-class texts, the status poll time-out of Gauntlet Legends, Star Wars Rogue Squadron's own microcode, and the line microcodes (L3DEX 1.21 in Wipeout 64, **L3DEX 2.08 in WinBack: 0.07 points**, rows 16 and 17 of the hot list).
`G_LOAD_UCODE of another ucode` (1930 tasks in Excitebike 64 before) and `an earlier task with these pointers loaded another ucode` (63) are gone from the decline reasons.

**Update 2026-10-16, after the ucode id fix and the registration of the four derived builds ([ucode-derive.md](ucode-derive.md#registered-2026-10-16)); `devel` 22b2d64 plus that work, same corpus and method, 57 s per game.** The first recount (devel 91d3e2e) is kept as "before":

| | before (91d3e2e) | after |
|---|---|---|
| graphics tasks offered (64 games) | 146 944 | 146 560 |
| native | 94 039 (64.0 %) | **110 580 (75.5 %)** |
| declined by a handler that exists | 8 951 | 12 003 |
| of a ucode without a handler | 43 906 | 23 921 |
| mean per-game share of tasks native | 68.7 % (test 73.0, unseen 66.2) | **79.6 %** (test 77.4, unseen 80.8) |
| mean share of graphics RSP time left on the JIT | **34.4 %** (test 31.9, unseen 35.8) | **23.4 %** (test 27.6, unseen 21.1) |
| games that run all their graphics tasks native | 38 | 45 |
| games that run (almost) none native | 15 | 8: F-Zero X, GoldenEye 007, Yoshi's Story, Chameleon Twist 2, Donald Duck, Excitebike 64, Star Wars Rogue Squadron, Mischief Makers (1 of 3200) |
| games with half or more of their RSP time on the JIT | 22 | 15 |

The seven games of the new builds (Banjo-Tooie, LEGO Racers, Star Wars Racer, Blues Brothers 2000, Harvest Moon 64, Tonic Trouble, Rocket) went from 0 to 100 % native; Gauntlet Legends stays on the JIT (2 of 3072 tasks native: 3068 are declined because the JIT's
status poll time-out has dropped, [ucode-derive.md](ucode-derive.md#gauntlet-legends)). The estimate of the derive page, 21.9 % if all three games of the 2.06 row ran as verified, is 23.4 % with Gauntlet Legends staying behind (its 1.56 points are the 0bbe row).
The task counts differ a little between the two recounts because the runs are timed by the wall clock (57 s), not by frames.

Of the 23.4 points left on the JIT, **14.7 points belong to ucodes without a handler** (was 27.2) and **8.8 to ucodes with one that declines** (was 7.2: XBUS lists, `G_LOAD_UCODE` of unregistered texts, `G_MOVEWORD` offsets, a triangle form, and now Gauntlet Legends'
1.6). Next candidates by the derive page: Chameleon Twist 2's F3DEX 2.05 `9798` and Donald Duck's F3DEX.NoN 1.23 `efe4` (NONE by the tool, 0 differences in its verify experiment), then the kernels (Rej / L3DEX / S2DEX 1.x / F3D 2.0G).

**F3DEX 1.x records that run (2026-10-17, [ucode-derive.md](ucode-derive.md)).** With the F3DEX 1.x front-end data-driven and its records derived, Donald Duck's `efe4` derives **EXACT** and passes the probe;
latched it ran 1727 of 1728 tasks natively in 3700 frames (4351 of 4352 in 9000) with 0 differences: its row above (1.56 %, 1536 tasks, "no handler") would move to native as soon as the latch policy admits a pass.
Iggy's Reckin' Balls' F3DEX.NoN 1.22 `dff3` (0.24 %) derives NEAR and passes too, but adds nothing: every one of its tasks sets the geometry bit 0x4000 that its 14 extra words implement, which the front-end
declines. Inside the 64-ROM set that is all the F3DEX 1.x derivation finds (the registered texts were native already); its value is the retail F3DEX 1.x texts outside the set: a build that differs from a
registered one in relocated handlers, moved DMEM state, the version rewrite of 1.23 or guarded extra code now gets a record that runs.

**F3D records that run, GoldenEye registered (2026-10-17, [ucode-derive.md](ucode-derive.md#f3d-records-that-run-2026-10-17)).** With the F3D front-end data-driven every unregistered F3D text of the
set still derives NONE: Blast Corps' `2f41` (0.02 %) has an output transport the front-end lacks (the lists are collected in RDRAM, not handed to the RDP: its probe, forced, differs in all 26 canned tasks),
`1d12` (0.00 %) and the 2.0H texts of Wipeout 64 `968d` (1.22 %) and Glover `9d0d` (0.06 %) are other code (the latter two the Sprite2D microcode). **GoldenEye 007's 2.0G `538f` (row 1, 1.56 %)**
derives with every handler of Pilotwings' family except its four-triangle command (op 0xb1, nearly every task): forced through the probe it passes the 26 canned tasks (they cannot use a command the
record has no handler for) and, latched, ran 87 of 1600 tasks natively. Registered by hand with that command added to the front-end (ucode-f3d.md 15) it runs **1663 of 1664 tasks** natively in 3700
frames with 0 differences (`ARMLET_HLE_VERIFY=1`), identical to the reference: its 1.56 points move from "no handler" to native, the mean JIT share of the 64 games from 23.4 % to about 21.8 %
(not recounted). A retail 2.0G text that differs from GoldenEye's in the ways the derivation knows now gets a record that runs, G_TRI4 included.

**The first recount (91d3e2e), as it was:**

- 146 944 graphics tasks: **94 039 native (64.0 %)**, 8 951 declined by a handler that exists, 43 906 of a ucode without a handler.
- Mean over the 64 games: 68.7 % of the tasks native (test-romset 73.0 %, unseen-romset 66.2 %), **34.4 % of the graphics RSP time left on the JIT** (31.9 % test, 35.8 % unseen).
- 38 games ran all their graphics tasks native (no JIT time at all); 15 ran none: F-Zero X, GoldenEye 007, Rocket, Yoshi's Story, Banjo-Tooie, Blues Brothers 2000, Chameleon Twist 2, Donald Duck, Excitebike 64,
  Gauntlet Legends, Harvest Moon 64, LEGO Racers, Star Wars Rogue Squadron, Star Wars Racer and Tonic Trouble (Mischief Makers: 1 task of 3200); 22 games kept half or more of their RSP time on the JIT.
- Of the 34.4 points of RSP time left on the JIT, **27.2 points belonged to ucodes without a handler** and 7.2 to ucodes with one that declines.

The tables of the games, the decline reasons and the hot list below are the **2026-10-17 (evening)** first-launch counts.

### Decline reasons (tasks of ucodes that have a handler)

(History: the 2026-10-17 evening recount. The reasons of the 2026-10-18 recount are in its update block above; `not eligible` is gone from them.)

| reason | tasks | games |
|---|---|---|
| not eligible (SP halted, RDP coalescing off or XBUS) | 4258 | 2 |
| the JIT's status poll timeout has dropped (an earlier task waited for the CPU) | 3068 | 1 |
| a textured triangle with geometry mode bit 12 | 997 | 1 |
| G_MOVEWORD offset outside what a game writes | 769 | 1 |
| an earlier task with these pointers loaded another ucode | 441 | 1 |
| G_LOAD_UCODE of another ucode | 387 | 1 |
| a command the native loop does not implement | 147 | 1 |
| (no reason given) | 7 | 1 |
| a triangle that the clipper discards | 5 | 1 |
| too many SP_STATUS polls (the JIT would halt) | 1 | 1 |

`not eligible` is the check at the start of a task (SP halted, no coalescing path, or the RDP list in DMEM / XBUS without `HLE_F_XBUS_OK`): Glover's F3DEX 1.21 (1060 tasks, a third of its tasks use the XBUS transport)
and Mischief Makers' F3D 2.0H (3198, all of them). The `G_LOAD_UCODE` ones (`an earlier task with these pointers loaded another ucode`, `G_LOAD_UCODE of another ucode`) are SpaceStation Silicon Valley and Pokemon Puzzle League (F3DEX 1.23 loading an S2DEX 1.07 that has no handler); Resident Evil 2 no longer declines.

## The games

Columns: graphics tasks offered, native, declined by a handler, of a ucode without a handler, the share of the game's graphics RSP time that stayed on the JIT, and the native share of the second launch. The last column lists the ucodes of the game (banner, first four hex digits of the 4 KiB text hash, share of the game's tasks, state: N native, P partly native, D handler that always declines, `-` no handler). The 64 games of `test-romset/` and `unseen-romset/`, **the 2026-10-18 recount, pass 1** (`ARMLET_PROBE=on`, first launch; the "native share: first / second launch" column is passes 1 and 2); the tables of the earlier recounts are replaced.

| set | game | gfx tasks | native | declined | no handler | graphics RSP time on the JIT (first launch) | native share: first / second launch (`on`) | ucodes (task share, state; first launch) |
|---|---|---|---|---|---|---|---|---|
| test | Banjo-Kazooie | 1280 | 1279 | 0 | 0 | 0% | 100% / 100% | F3DEX 1.21 `208a` 100% N |
| test | Blast Corps | 2816 | 1544 | 769 | 502 | 90% | 55% / 55% | F3D 2.0D `3e9b` 82% P; F3D 2.0D `2f41` 16% -; F3D 2.0D `1d12` 2% - |
| test | Bomberman Hero | 2560 | 2559 | 0 | 0 | 0% | 100% / 100% | F3DEX 1.21 `208a` 100% N |
| test | Buck Bumble | 3328 | 3327 | 0 | 0 | 0% | 100% / 100% | F3DEX fifo 2.04 `6635` 100% N |
| test | Diddy Kong Racing | 1408 | 1407 | 0 | 0 | 0% | 100% / 100% | (no banner) `909f` 100% N |
| test | Donkey Kong 64 | 1408 | 1407 | 0 | 0 | 0% | 100% / 100% | F3DEX fifo 2.07 / L3DEX fifo 2.07 `48c7` 100% N |
| test | F-Zero X | 2944 | 2943 | 0 | 0 | 0% | 100% / 100% | F3DEX fifo 2.03 / F3DLX.Rej fifo 2.03 `33dc` 52% N; F3DFLX.Rej fifo 2.03F / L3DEX fifo 2.03 `c750` 38% N; F3DLX.Rej fifo 2.03 / F3DFLX.Rej fifo 2.03F `7de8` 10% N |
| test | Glover | 6144 | 5066 | 0 | 1077 | 15% | 82% / 82% | F3DEX 1.21 `208a` 82% N; F3D 2.0H `9d0d` 18% - |
| test | GoldenEye 007 | 1408 | 1407 | 0 | 0 | 0% | 100% / 100% | F3D 2.0G `538f` 100% N |
| test | Jet Force Gemini | 1536 | 1535 | 0 | 0 | 0% | 100% / 100% | (no banner) `c2ac` 100% N |
| test | Kirby 64 - The Crystal Shards | 1536 | 1535 | 0 | 0 | 0% | 100% / 100% | F3DEX fifo 2.04H / L3DEX fifo 2.04H `e857` 100% N |
| test | Legend of Zelda, The - Ocarina of Time | 1280 | 1279 | 0 | 0 | 0% | 100% / 100% | F3DZEX.NoN fifo 2.06H / S2DEX fifo 2.05 / S2DEX fifo 2.05 [Safe] `0d3f` 100% N |
| test | Mario Kart 64 | 1280 | 1279 | 0 | 0 | 0% | 100% / 100% | F3DEX 0.95 `34da` 100% N |
| test | Mischief Makers | 3200 | 3199 | 0 | 0 | 0% | 100% / 100% | F3D 2.0H `4212` 100% N |
| test | Mystical Ninja Starring Goemon | 1792 | 1791 | 0 | 0 | 0% | 100% / 100% | F3DEX 1.23 `0472` 100% N |
| test | Paper Mario | 3200 | 3199 | 0 | 0 | 0% | 100% / 100% | F3DEX fifo 2.08 `c734` 100% N |
| test | Pokemon Snap | 1920 | 1919 | 0 | 0 | 0% | 100% / 100% | F3DEX.NoN fifo 2.08H / L3DEX fifo 2.08H `d576` 100% N |
| test | Rocket - Robot on Wheels | 1536 | 1535 | 0 | 0 | 0% | 100% / 100% | F3DEX fifo 2.07 `48c7` 100% N |
| test | SpaceStation Silicon Valley | 1920 | 1091 | 828 | 0 | 69% | 57% / 57% | F3DEX 1.23 `0472` 100% P |
| test | Star Fox 64 | 1536 | 1535 | 0 | 0 | 0% | 100% / 100% | F3DEX.NoN 1.22 `fd5a` 100% N |
| test | Super Mario 64 | 1536 | 1535 | 0 | 0 | 0% | 100% / 100% | F3D 2.0D `94c7` 100% N |
| test | Wetrix | 1792 | 1671 | 120 | 0 | 23% | 93% / 93% | F3DEX.NoN 1.21 `fd5a` 100% P |
| test | Yoshi's Story | 2944 | 0 | 0 | 2944 | 100% | 0% / 0% | S2DEX 1.06 / F3DEX.NoN 1.23 `5b32` 100% - |
| unseen | 007 - The World Is Not Enough | 1536 | 1535 | 0 | 0 | 0% | 100% / 100% | F3DEX fifo 2.08 `d2d1` 100% N |
| unseen | Banjo-Tooie | 896 | 895 | 0 | 0 | 0% | 100% / 100% | F3DEX.NoN fifo 2.08 / F3DEX fifo 2.08 `9875` 100% N |
| unseen | Blues Brothers 2000 | 3328 | 3327 | 0 | 0 | 0% | 100% / 100% | F3DEX fifo 2.06 `0bbe` 100% N |
| unseen | Bug's Life, A | 2176 | 2175 | 0 | 0 | 0% | 100% / 100% | F3DLX 1.23 `7502` 100% N |
| unseen | Chameleon Twist 2 | 3072 | 2078 | 3 | 990 | 50% | 68% / 97% | F3DEX fifo 2.05 `9798` 100% P |
| unseen | Clay Fighter - Sculptor's Cut | 3200 | 3199 | 0 | 0 | 0% | 100% / 100% | F3DLX.NoN 1.21 `0a7c` 100% N |
| unseen | Conker's Bad Fur Day | 1152 | 1151 | 0 | 0 | 0% | 100% / 100% | F3DEXBG.NoN fifo 2.08 `e48f` 100% N |
| unseen | Cruis'n World | 1408 | 1407 | 0 | 0 | 0% | 100% / 100% | F3DEX fifo 2.04 `6635` 100% N |
| unseen | Donald Duck - Goin' Quackers | 1536 | 1449 | 0 | 86 | 22% | 94% / 97% | F3DEX.NoN 1.23 `efe4` 100% P |
| unseen | Dr. Mario 64 | 2816 | 2815 | 0 | 0 | 0% | 100% / 100% | F3DEX fifo 2.08 / S2DEX 1.07 `d2d1` 100% N |
| unseen | Earthworm Jim 3D | 3072 | 3071 | 0 | 0 | 0% | 100% / 100% | F3DEX 1.23 `0472` 100% N |
| unseen | Excitebike 64 | 1408 | 1407 | 0 | 0 | 0% | 100% / 100% | F3DLX.Rej xbus 2.08 `8b0a` 100% N |
| unseen | Gauntlet Legends | 3072 | 2 | 3069 | 0 | 100% | 0% / 0% | F3DEX fifo 2.06 `0bbe` 100% P |
| unseen | Harvest Moon 64 | 4992 | 4991 | 0 | 0 | 0% | 100% / 100% | F3DEX fifo 2.06 `0bbe` 100% N |
| unseen | Hot Wheels - Turbo Racing | 2816 | 2815 | 0 | 0 | 0% | 100% / 100% | F3DEX fifo 2.08 / L3DEX fifo 2.08 `d2d1` 100% N |
| unseen | Hydro Thunder | 1280 | 1279 | 0 | 0 | 0% | 100% / 100% | F3DEX fifo 2.08 `d2d1` 100% N |
| unseen | Iggy's Reckin' Balls | 1920 | 1850 | 0 | 69 | 17% | 96% / 96% | F3DLX.NoN 1.23 `cc09` 96% N; F3DEX.NoN 1.22 `dff3` 4% - |
| unseen | Indiana Jones and the Infernal Machine | 1792 | 642 | 1149 | 0 | 93% | 36% / 36% | (no banner) `a6c6` 100% P |
| unseen | LEGO Racers | 2432 | 2431 | 0 | 0 | 0% | 100% / 100% | F3DEX.NoN fifo 2.08 `9875` 100% N |
| unseen | Mario Golf | 1536 | 1535 | 0 | 0 | 0% | 100% / 100% | F3DEX fifo 2.08 `d2d1` 100% N |
| unseen | Mario Tennis | 1408 | 1407 | 0 | 0 | 0% | 100% / 100% | F3DEX fifo 2.08 / F3DEX xbus 2.08 `d2d1` 100% N |
| unseen | Ogre Battle 64 - Person of Lordly Caliber | 1536 | 1535 | 0 | 0 | 0% | 100% / 100% | F3DEX fifo 2.08 / F3DEX.NoN fifo 2.08 `d2d1` 100% N |
| unseen | Penny Racers | 1664 | 1663 | 0 | 0 | 0% | 100% / 100% | F3DLX.NoN 1.23 `cc09` 100% N |
| unseen | Pilotwings 64 | 1664 | 1663 | 0 | 0 | 0% | 100% / 100% | F3D 2.0D `3e9b` 100% N |
| unseen | Pokemon Puzzle League | 1920 | 268 | 0 | 1652 | 99% | 14% / 14% | S2DEX 1.07 `190d` 86% -; F3DEX 1.23 `0472` 14% N |
| unseen | Pokemon Stadium 2 | 1280 | 1279 | 0 | 0 | 0% | 100% / 100% | F3DEX fifo 2.08 `c734` 100% N |
| unseen | Quake | 1664 | 1663 | 0 | 0 | 0% | 100% / 100% | F3DEX.NoN 1.21 `fd5a` 100% N |
| unseen | Quest 64 | 2048 | 2047 | 0 | 0 | 0% | 100% / 100% | F3DEX 1.23 `208a` 100% N |
| unseen | RR64 - Ridge Racer 64 | 2176 | 2175 | 0 | 0 | 0% | 100% / 100% | F3DEX fifo 2.08 `d2d1` 100% N |
| unseen | Resident Evil 2 | 7808 | 7807 | 0 | 0 | 0% | 100% / 100% | S2DEX fifo 2.06 / F3DEX 1.23 `5f33` 75% N; F3DEX.Rej fifo 2.06 / F3DEX.Rej xbus 2.06 `9512` 25% N |
| unseen | Star Wars - Rogue Squadron | 1280 | 0 | 0 | 1280 | 100% | 0% / 0% | (no banner) `bfbf` 100% - |
| unseen | Star Wars Episode I - Racer | 2304 | 2303 | 0 | 0 | 0% | 100% / 100% | F3DEX.NoN fifo 2.08 `9875` 100% N |
| unseen | Super Smash Bros. | 2944 | 2943 | 0 | 0 | 0% | 100% / 100% | F3DEX fifo 2.04H `e857` 100% N |
| unseen | Tetrisphere | 1152 | 1151 | 0 | 0 | 0% | 100% / 100% | F3D 2.0H `0b57` 100% N |
| unseen | Tonic Trouble | 1408 | 1407 | 0 | 0 | 0% | 100% / 100% | F3DEX.NoN fifo 2.07 `d8bd` 100% N |
| unseen | Toy Story 2 - Buzz Lightyear to the Rescue! | 1536 | 1535 | 0 | 0 | 0% | 100% / 100% | F3DLX 1.23 `7502` 100% N |
| unseen | Turok - Dinosaur Hunter | 1536 | 1535 | 0 | 0 | 0% | 100% / 100% | F3DEX.NoN 1.00 `5063` 100% N |
| unseen | Wave Race 64 - Kawasaki Jet Ski | 896 | 895 | 0 | 0 | 0% | 100% / 100% | F3D 2.0D `fb48` 100% N |
| unseen | WinBack - Covert Operations | 2560 | 1280 | 0 | 1279 | 5% | 50% / 50% | F3DEX fifo 2.08 / L3DEX fifo 2.08 `d2d1` 50% N; L3DEX fifo 2.08 `cd32` 50% - |
| unseen | Wipeout 64 | 9856 | 7026 | 14 | 2815 | 84% | 71% / 71% | F3DLX 1.21 `76c7` 71% P; L3DEX 1.21 `3c7a` 14% -; F3D 2.0H `968d` 14% - |
| unseen | Worms Armageddon | 2432 | 2431 | 0 | 0 | 0% | 100% / 100% | S2DEX fifo 2.08 `fa17` 100% N |

## The games of the not-coded set (2026-10-18 recount)

The 48 counted games of `not-coded-romset/` (40 Winks has no counter file, see the update). First-launch counts of pass 1 (`ARMLET_PROBE=on`); the two further columns give the native share of the first / second launch of the `on` passes (1, 2) and of the `loose` passes (3, 4). Columns as in the table above. A ucode shown `N` with a `P` is partly native (declines, or a latch earned during the run); `-` has no handler. The 2026-10-17 night table of this set is replaced.

| game | gfx tasks | native | declined | no handler | graphics RSP time on the JIT (first launch) | native share: first / second launch (`on`) | native share: first / second launch (`loose`) | ucodes (task share, state; first launch) |
|---|---|---|---|---|---|---|---|---|
| 1080 Snowboarding | 2432 | 2431 | 0 | 0 | 0% | 100% / 100% | 100% / 100% | F3DEX 1.23 `0472` 100% N |
| AeroGauge | 1280 | 768 | 7 | 505 | 22% | 60% / 96% | 60% / 96% | F3DEX.NoN 1.23 `efe4` 100% P |
| Aidyn Chronicles - The First Mage | 1408 | 1407 | 0 | 0 | 0% | 100% / 100% | 100% / 100% | F3DEX fifo 2.08 `d2d1` 100% N |
| Air Boarder 64 | 1536 | 182 | 688 | 665 | 100% | 12% / 12% | 12% / 12% | F3DLX 1.21 `76c7` 57% P; F3DLX.Rej 1.21 `3c51` 43% - |
| Akumajou Dracula Mokushiroku Gaiden - Legend of Cornell | 1792 | 1791 | 0 | 0 | 0% | 100% / 100% | 100% / 100% | F3DEX fifo 2.06 `0bbe` 100% N |
| Armorines - Project S.W.A.R.M. | 1024 | 0 | 0 | 1024 | 100% | 0% / 0% | 0% / 0% | F3DEX.NoN fifo 2.05 / L3DEX fifo 2.05 `7847` 100% - |
| Army Men - Sarge's Heroes 2 | 3200 | 3199 | 0 | 0 | 0% | 100% / 100% | 100% / 100% | F3DEX fifo 2.07 / L3DEX fifo 2.07 `48c7` 100% N |
| Bakuretsu Muteki Bangaioh | 2944 | 0 | 0 | 2944 | 100% | 0% / 0% | 0% / 0% | S2DEX 1.07 / S2DEXD 1.07 `190d` 100% - |
| Beetle Adventure Racing! | 2816 | 2815 | 0 | 0 | 0% | 100% / 100% | 100% / 100% | F3DEX.NoN fifo 2.08 / F3DEX.NoN xbus 2.08 `9875` 100% N |
| Command & Conquer | 2048 | 0 | 0 | 2048 | 100% | 0% / 0% | 0% / 0% | F3DEX.NoN xbus 2.06 / L3DEX xbus 2.06 `65ad` 100% - |
| Custom Robo V2 | 6272 | 6271 | 0 | 0 | 0% | 100% / 100% | 100% / 100% | F3DEX fifo 2.08 / L3DEX fifo 2.08 `d2d1` 100% N |
| Dance Dance Revolution - Disney Dancing Museum | 1664 | 1663 | 0 | 0 | 0% | 100% / 100% | 100% / 100% | F3DEX fifo 2.06 `0bbe` 100% N |
| Duck Dodgers Starring Daffy Duck | 3200 | 3199 | 0 | 0 | 0% | 100% / 100% | 100% / 100% | F3DEX.NoN fifo 2.08 `9875` 100% N |
| ECW Hardcore Revolution | 1664 | 571 | 0 | 1093 | 91% | 34% / 34% | 34% / 34% | F3DEX.Rej fifo 2.08 `23cb` 66% -; F3DEX fifo 2.08 / F3DEX.Rej fifo 2.08 `d2d1` 34% N |
| F1 Racing Championship | 1152 | 1151 | 0 | 0 | 0% | 100% / 100% | 100% / 100% | F3DEX fifo 2.04 / L3DEX fifo 2.05 `6635` 100% N |
| Forsaken 64 | 1536 | 1535 | 0 | 0 | 0% | 100% / 100% | 100% / 100% | F3DEX 1.23 `0472` 100% N |
| Fushigi no Dungeon - Fuurai no Shiren 2 - Oni Shuurai! Shiren Jou! | 3712 | 3711 | 0 | 0 | 0% | 100% / 100% | 100% / 100% | F3DEX fifo 2.08 `d2d1` 100% N |
| G.A.S.P!! Fighters' NEXTream | 2688 | 2687 | 0 | 0 | 0% | 100% / 100% | 100% / 100% | F3DEX 1.23 `208a` 100% N |
| Gex 64 - Enter the Gecko | 1408 | 1407 | 0 | 0 | 0% | 100% / 100% | 100% / 100% | F3DEX 1.23 `0472` 100% N |
| Hexen | 1536 | 1535 | 0 | 0 | 0% | 100% / 100% | 100% / 100% | F3DEX 1.21 `208a` 100% N |
| Hybrid Heaven | 1536 | 1535 | 0 | 0 | 0% | 100% / 100% | 100% / 100% | F3DEX fifo 2.06 `0bbe` 100% N |
| International Superstar Soccer 64 | 5376 | 3583 | 0 | 1792 | 72% | 67% / 67% | 67% / 67% | F3DEX 1.21 `208a` 67% N; F3DLX.Rej 1.21 `3c51` 33% - |
| John Romero's Daikatana | 2944 | 2943 | 0 | 0 | 0% | 100% / 100% | 100% / 100% | F3DEX fifo 2.08 `d2d1` 100% N |
| Killer Instinct Gold | 2688 | 0 | 0 | 2688 | 100% | 0% / 0% | 89% / 89% | F3D 2.0D `bf9b` 100% - |
| Kiratto Kaiketsu! 64 Tanteidan | 3328 | 3327 | 0 | 0 | 0% | 100% / 100% | 100% / 100% | F3DEX 1.23 `208a` 100% N |
| Legend of Zelda, The - Majora's Mask | 1536 | 0 | 0 | 1536 | 100% | 0% / 0% | 0% / 0% | F3DZEX.NoN fifo 2.08I / S2DEX fifo 2.08 `1c72` 100% - |
| Lode Runner 3-D | 1536 | 914 | 1 | 620 | 15% | 60% / 97% | 60% / 97% | F3DEX xbus 2.07 `cbe9` 100% P |
| Magical Tetris Challenge | 2688 | 0 | 0 | 2688 | 100% | 0% / 0% | 0% / 0% | S2DEX 1.07 / S2DEXD 1.07 `190d` 100% - |
| Mario Party 3 | 5760 | 4309 | 717 | 733 | 52% | 75% / 75% | 75% / 76% | F3DEX fifo 2.08 / F3DEX.NoN fifo 2.08 `d2d1` 68% P; F3DEX.NoN fifo 2.08 / F3DLX.Rej fifo 2.08 `9875` 20% N; F3DLX.Rej fifo 2.08 / S2DEX fifo 2.08 `9332` 13% - |
| Mega Man 64 | 1536 | 1535 | 0 | 0 | 0% | 100% / 100% | 100% / 100% | F3DEX fifo 2.08 / L3DEX fifo 2.08 `d2d1` 100% N |
| Mission - Impossible | 1920 | 1919 | 0 | 0 | 0% | 100% / 100% | 100% / 100% | F3DEX.NoN 1.21 `fd5a` 100% N |
| Ms. Pac-Man - Maze Madness | 3072 | 1979 | 3 | 1089 | 66% | 64% / 97% | 64% / 97% | F3DEX xbus 2.08 `a986` 100% P |
| NBA Jam 2000 | 1536 | 153 | 741 | 641 | 100% | 10% / 49% | 10% / 49% | F3DEX fifo 2.05 / F3DLX.Rej fifo 2.05 `9798` 100% P |
| NFL Blitz | 1536 | 971 | 252 | 312 | 68% | 63% / 93% | 63% / 93% | F3DEX fifo 2.05 / F3DLX.Rej fifo 2.05 `9798` 100% P |
| Neon Genesis Evangelion | 6784 | 5970 | 813 | 0 | 10% | 88% / 88% | 88% / 88% | F3DEX fifo 2.08 / L3DEX fifo 2.08 `d2d1` 60% N; S2DEX fifo 2.08 `fa17` 40% P |
| Nightmare Creatures | 1408 | 1407 | 0 | 0 | 0% | 100% / 100% | 100% / 100% | F3DEX fifo 2.06 `0bbe` 100% N |
| Off Road Challenge | 1536 | 1535 | 0 | 0 | 0% | 100% / 100% | 100% / 100% | F3DEX.NoN 1.21 `fd5a` 100% N |
| Paperboy | 2304 | 2303 | 0 | 0 | 0% | 100% / 100% | 100% / 100% | F3DEX.NoN fifo 2.08 `9875` 100% N |
| Perfect Dark | 1280 | 0 | 0 | 1280 | 100% | 0% / 0% | 0% / 0% | (no banner) `eb2c` 100% - |
| Racing Simulation 2 | 1792 | 1791 | 0 | 0 | 0% | 100% / 100% | 100% / 100% | F3DEX fifo 2.04 / L3DEX fifo 2.05 `6635` 100% N |
| Robotron 64 | 2048 | 2046 | 1 | 0 | 0% | 100% / 100% | 100% / 100% | F3DEX 1.21 `208a` 100% P |
| Shadow Man | 3328 | 3327 | 0 | 0 | 0% | 100% / 100% | 100% / 100% | F3DEX fifo 2.08 / L3DEX fifo 2.08 `d2d1` 100% N |
| Snowboard Kids | 2560 | 2559 | 0 | 0 | 0% | 100% / 100% | 100% / 100% | F3DLX 1.23 `76c7` 100% N |
| StarCraft 64 | 1280 | 1279 | 0 | 0 | 0% | 100% / 100% | 100% / 100% | S2DEX xbus 2.08 / S2DEX 1.07 `3ee6` 100% N |
| Top Gear Rally | 1280 | 1279 | 0 | 0 | 0% | 100% / 100% | 100% / 100% | F3DEX.NoN 1.21 `fd5a` 100% N |
| Twisted Edge - Extreme Snowboarding | 1536 | 1293 | 2 | 240 | 15% | 84% / 97% | 84% / 97% | F3DEX.NoN fifo 2.06 `f8d3` 100% P |
| Vigilante 8 | 3328 | 3327 | 0 | 0 | 0% | 100% / 100% | 100% / 100% | F3DEX fifo 2.06 `0bbe` 100% N |
| War Gods | 4352 | 4351 | 0 | 0 | 0% | 100% / 100% | 100% / 100% | F3D 2.0H `4212` 100% N |
| 40 Winks | n/a (under 128: no counter file) | | | | | | | F3DEX.NoN fifo 2.08 `9875` registered, N (log line only) |

### Every ucode seen in the not-coded set

One row per 4 KiB text hash (the id of the log's `gfx ucode` line, or of a `probe: ... (loaded by G_LOAD_UCODE ...)` line for a text that a task loads; banner of the first game that uses it). "Derive verdict" is the tool's verdict on the text (EXACT / NEAR / NONE, with the aligned-words figure for NONE); "loose tier" is the pass-3 admission; "probe" the canned-task result; "latched" whether the validator latched the pair during the 57 s run. Registered rows are ucodes with a native handler: no probe is run. 2026-10-18 recount; rows whose verdict changed since the night table are bold.

| ucode (banner) | id (text hash) | family | games | derive verdict | loose tier (pass 3) | probe | latched, first launch (pass 1) | latched, loose first launch (pass 3) | second launch (pass 2 / pass 4) |
|---|---|---|---|---|---|---|---|---|---|
| F3D 2.0D | `bf9b120728efbf0a` | F3D | Killer Instinct Gold | NONE (f3d family, 912 of 1024 words aligned; 0 unexplained, 3 missing) | admitted | `on`: none; `loose`: PASS (26/26 equal) |  | **yes**, after 221 equal tasks (evidence 60.77, 37 distinct, 7.35 s) | - / not reused: derived, probed and latched again identically |
| F3DLX.Rej 1.21 | `3c51d836d337efab` | F3DEX | Air Boarder 64, International Superstar Soccer 64 | NONE (f3dex1 family, 425 of 1024 words aligned) | not admitted | none |  |  |  |
| (loaded text, banner not logged) | `578ac8cc405d9e81` | F3DEX | Air Boarder 64 | NONE (f3dex1 family, 425 of 1024 words aligned) | not admitted | none |  |  |  |
| F3DEX.NoN 1.23 | `efe4b8f339a9ebae` | F3DEX | AeroGauge, Donald Duck (unseen set) | EXACT | | PASS (28/28 equal) | yes (AeroGauge after 473 equal tasks, 18.9 s) | yes | cached latch used / cached latch used |
| F3DZEX.NoN fifo 2.08I / S2DEX fifo 2.08 | `1c724a38139c0669` | F3DEX2 | Legend of Zelda, The - Majora's Mask | NEAR | | FAIL (25/28 equal, 3 differ) |  |  | cached as failed |
| F3DEX.Rej fifo 2.08 | `23cb9e8557a7c74e` | F3DEX2 | ECW Hardcore Revolution | NONE (f3dex2 family, 786 of 1024 words aligned; 22 unexplained, 21 missing) | beyond the bound: default-record arm | `on`: none; `loose`: **FAIL** (18 equal, 3 declined, 7 differ of 28) |  |  | - / probed again, FAIL again |
| F3DEX.NoN xbus 2.06 / L3DEX xbus 2.06 | `65ad6e1a2bf9e9ff` | F3DEX2 | Command & Conquer | **NEAR** (was NONE, 899 of 1154), xbus record | | PASS (28/28 equal) | candidate, **never latched**: 1792 of 1792 validator replays declined | **unlatched for good** (strike 1, a real task differs, 68 DMEM bytes) | candidate again / the dead entry |
| (loaded text, S2DEX xbus) | `b2b01e17177e5881` | F3DEX2 | Command & Conquer | NONE (f3dex2 family, 1423 of 1612 words aligned; 0 unexplained, 7 missing) | admitted | `on`: none; `loose`: PASS (30/30 equal) |  | candidate, not latched | - / cached |
| F3DEX.NoN fifo 2.05 / L3DEX fifo 2.05 | `7847866d95f40933` | F3DEX2 | Armorines - Project S.W.A.R.M. | NONE (f3dex2 family, 941 of 1154 words aligned; 49 unexplained, 3 missing) | beyond the bound: default-record arm | `on`: none; `loose`: **FAIL** (6 equal, 22 differ of 28) |  |  | - / probed again, FAIL again |
| F3DLX.Rej fifo 2.08 / S2DEX fifo 2.08 | `9332f30c0d47063c` | F3DEX2 | Mario Party 3 | **NEAR** (was NONE, 713 of 1482), Rej record `l_b_rej_203` | | PASS (28/28 equal) | candidate, not latched (704 of 704 replayed tasks equal at the last log line) | candidate, not latched | candidate, not latched / the same |
| F3DEX fifo 2.05 / F3DLX.Rej fifo 2.05 | `97989ee9c10ef7ad` | F3DEX2 | NBA Jam 2000, NFL Blitz, Chameleon Twist 2 (unseen set) | EXACT | | PASS (27/28 equal, 1 declined) | yes (NBA Jam 606 tasks, 21.1 s; NFL Blitz 258, 9.3 s; Chameleon Twist 2 919, 15.6 s) | yes (identical) | cached latch used / cached latch used |
| (loaded text, Rej, `l_b_rej_203`) | `36f10dc3aeaafe57` | F3DEX2 | NBA Jam 2000, NFL Blitz | NEAR | | PASS (28/28 equal) | NFL Blitz: yes, after 206 equal tasks (26.2 s, 0 graphics tasks of the pair); NBA Jam: candidate, not latched | the same | NFL Blitz: cached latch used; NBA Jam: candidate / candidate |
| F3DEX xbus 2.08 | `a9864f29d85ed52c` | F3DEX2 | Ms. Pac-Man - Maze Madness | **EXACT** (was NONE, 901 of 1154), xbus record | | PASS (27/28 equal, 1 declined) | **yes**, after 1020 equal tasks (17.2 s) | yes | cached latch used / cached latch used |
| F3DEX xbus 2.07 | `cbe9a9f21ad75f27` | F3DEX2 | Lode Runner 3-D | **NEAR** (was NONE, 900 of 1154), xbus record | | PASS (27/28 equal, 1 declined) | **yes**, after 584 equal tasks (11.7 s) | yes | cached latch used / cached latch used |
| F3DEX.NoN fifo 2.06 | `f8d3b2039771caa8` | F3DEX2 | Twisted Edge - Extreme Snowboarding | EXACT | | **PASS (28/28 equal)** (the night run had no result: 16 probe starts, one per data id; now one data id) | **yes**, after 192 equal tasks (6.5 s) | yes | cached latch used / cached latch used |
| S2DEX 1.07 / S2DEXD 1.07 | `190d04dfa000cd4b` | other | Bakuretsu Muteki Bangaioh, Magical Tetris Challenge (Pokemon Puzzle League, unseen set) | NONE (f3dex2 family, 1166 of 1944 words aligned) | not admitted | none |  |  |  |
| (no banner) | `eb2cc8ed91b299e1` | other | Perfect Dark | NONE (f3d family, 914 of 1024 words aligned; 45 unexplained, 27 missing) | beyond the bound: default-record arm | `on`: none; `loose`: **FAIL** (the build declined 26 of 26) |  |  | - / probed again, FAIL again |
| F3D 2.0H | `4212d63f6e483348` | F3D | War Gods (Mischief Makers, test set) | registered (no probe) |  |  |  |  |  |
| S2DEX xbus 2.08 / S2DEX 1.07 | `3ee636569037de5b` | other | StarCraft 64 | registered (no probe; was NONE, 1277 of 1942) |  |  |  |  |  |
| F3DEX 1.23 | `047252e810e35d56` | F3DEX | 1080 Snowboarding, Forsaken 64, Gex 64 - Enter the Gecko | registered (no probe) |  |  |  |  |  |
| F3DEX 1.23 | `208a567e9aead5ea` | F3DEX | G.A.S.P!! Fighters' NEXTream, Hexen, International Superstar Soccer 64, Kiratto Kaiketsu! 64 Tanteidan, Robotron 64 | registered (no probe) |  |  |  |  |  |
| F3DLX 1.21 | `76c763bc18ad3ab3` | F3DEX | Air Boarder 64, Snowboard Kids | registered (no probe) |  |  |  |  |  |
| F3DEX.NoN 1.21 | `fd5a31ccf6d40b62` | F3DEX | Mission - Impossible, Off Road Challenge, Top Gear Rally | registered (no probe) |  |  |  |  |  |
| F3DEX fifo 2.06 | `0bbec7c325e9cca8` | F3DEX2 | Akumajou Dracula Mokushiroku Gaiden - Legend of Cornell, Dance Dance Revolution - Disney Dancing Museum, Hybrid Heaven, Nightmare Creatures, Vigilante 8 | registered (no probe) |  |  |  |  |  |
| F3DEX fifo 2.07 / L3DEX fifo 2.07 | `48c7e653a6adcda8` | F3DEX2 | Army Men - Sarge's Heroes 2 | registered (no probe) |  |  |  |  |  |
| F3DEX fifo 2.04 / L3DEX fifo 2.05 | `6635562b2dde31ae` | F3DEX2 | F1 Racing Championship, Racing Simulation 2 | registered (no probe) |  |  |  |  |  |
| F3DEX.NoN fifo 2.08 | `9875370e17e420ab` | F3DEX2 | 40 Winks, Beetle Adventure Racing!, Duck Dodgers Starring Daffy Duck, Mario Party 3, Paperboy | registered (no probe) |  |  |  |  |  |
| F3DEX fifo 2.08 | `d2d1f9f2e7dfe1ab` | F3DEX2 | Aidyn Chronicles - The First Mage, Custom Robo V2, ECW Hardcore Revolution, Fushigi no Dungeon - Fuurai no Shiren 2 - Oni Shuurai! Shiren Jou!, John Romero's Daikatana, Mario Party 3, Mega Man 64, Neon Genesis Evangelion, Shadow Man | registered (no probe) |  |  |  |  |  |
| S2DEX fifo 2.08 | `fa171b4489d45ef2` | other | Neon Genesis Evangelion | registered (no probe) |  |  |  |  |  |

Derive verdicts read the same in `on` and `loose` except for the texts the loose tier admits or tries with the default record (`bf9b`, `b2b0`, `7847`, `23cb`, `eb2c`; under `on` they are NONE with no probe). Every NONE row without a latch is "no handler": the JIT runs it. `eb2c` is Perfect Dark's (no banner; family f3d, 914 of 1024 words aligned); `1c72` is Majora's Mask's F3DZEX; `65ad`, `a986`, `cbe9` and `b2b0` are XBUS builds. Air Boarder 64's registered F3DLX 1.21 `76c7` runs 182 of 871 tasks (11 tasks `G_LOAD_UCODE of another ucode`, 677 `an earlier task with these pointers loaded another ucode`: the F3DLX.Rej 1.21 `3c51` it loads has no handler); Neon Genesis Evangelion's S2DEX `fa17` runs 1899 of 2713 (`S2DEX LDTX_SPRITE / LDTX_RECT_R`, 813). War Gods' F3D 2.0H `4212` runs 4351 of 4352 now (the XBUS flag; it was 1 of 4352 `not eligible`).

## The hot list after the 2026-10-18 recount (112 games, pass 1)

Non-native ucodes ranked as below, over all 112 games, first launch with the shipped defaults (`GfxProbe=on`), the module of the 2026-10-18 update. The "no handler for this id, but its 4 KiB text is a registered build's" reason on rows 2, 18, 20, 23 and 25 is the script's guess for texts that a latch made native somewhere (`9798`, `a986`, `efe4`, `cbe9`, `f8d3`): they are latched probe builds, not registered ones, and their tasks are the first launch's tasks before the latch. Under `loose` row 5 (`bf9b1207`, Killer Instinct Gold) drops out; everything else is as listed. Pass 4 (`loose`, second launch) has the lowest mean JIT share, 17.4 %.

| # | ucode (4 KiB text) | family | games | mean RSP time on JIT | mean task share | state | reasons |
|---|---|---|---|---|---|---|---|
| 1 | S2DEX 1.07 / S2DEXD 1.07 `190d04df` | other | 3 | 2.67% | 2.55% | no handler x3 | no build registered |
| 2 | F3DEX fifo 2.05 / F3DLX.Rej fifo 2.05 `97989ee9` | F3DEX2 | 3 | 1.94% | 1.42% | partly native x3 | no handler for this id, but its 4 KiB text is a registered build's (the id is keyed over a different ucode_size) (1943); an earlier task with these pointers loaded another ucode (784); G_LOAD_UCODE of another ucode (201) |
| 3 | F3DEX.NoN fifo 2.05 / L3DEX fifo 2.05 `7847866d` | F3DEX2 | 1 | 0.89% | 0.89% | no handler x1 | no build registered |
| 4 | F3DEX.NoN xbus 2.06 / L3DEX xbus 2.06 `65ad6e1a` | F3DEX2 | 1 | 0.89% | 0.89% | no handler x1 | no build registered |
| 5 | F3D 2.0D `bf9b1207` | F3D | 1 | 0.89% | 0.89% | no handler x1 | no build registered |
| 6 | F3DZEX.NoN fifo 2.08I / S2DEX fifo 2.08 `1c724a38` | F3DEX2 | 1 | 0.89% | 0.89% | no handler x1 | no build registered |
| 7 | (no banner) `eb2cc8ed` | other | 1 | 0.89% | 0.89% | no handler x1 | no build registered |
| 8 | S2DEX 1.06 / F3DEX.NoN 1.23 `5b327e0d` | F3DEX | 1 | 0.89% | 0.89% | no handler x1 | no build registered |
| 9 | (no banner) `bfbf5496` | other | 1 | 0.89% | 0.89% | no handler x1 | no build registered |
| 10 | F3DEX fifo 2.06 `0bbec7c3` | F3DEX2 | 1 | 0.89% | 0.89% | partly native x1 | the JIT's status poll timeout has dropped (an earlier task waited for the CPU) (3068); too many SP_STATUS polls (the JIT would halt) (1) |
| 11 | F3DLX 1.21 `76c763bc` | F3DEX | 2 | 0.85% | 0.40% | partly native x2 | an earlier task with these pointers loaded another ucode (677); class-0 operand longer than the operand buffer (14); G_LOAD_UCODE of another ucode (11) |
| 12 | (no banner) `a6c64e8d` | other | 1 | 0.83% | 0.57% | partly native x1 | a textured triangle with geometry mode bit 12 (997); a command the native loop does not implement (147); a triangle that the clipper discards (5) |
| 13 | F3DEX.Rej fifo 2.08 `23cb9e85` | F3DEX2 | 1 | 0.82% | 0.59% | no handler x1 | no build registered |
| 14 | F3D 2.0D `3e9b9c3a` | F3D | 1 | 0.79% | 0.24% | partly native x1 | G_MOVEWORD offset outside what a game writes (769) |
| 15 | F3DLX.Rej 1.21 `3c51d836` | F3DEX | 2 | 0.70% | 0.68% | no handler x2 | no build registered |
| 16 | F3D 2.0H `968dec0e` | F3D | 1 | 0.68% | 0.13% | no handler x1 | no build registered |
| 17 | F3DEX 1.23 `047252e8` | F3DEX | 1 | 0.62% | 0.39% | partly native x1 | an earlier task with these pointers loaded another ucode (441); G_LOAD_UCODE of another ucode (387) |
| 18 | F3DEX xbus 2.08 `a9864f29` | F3DEX2 | 1 | 0.59% | 0.32% | partly native x1 | no handler for this id, but its 4 KiB text is a registered build's (the id is keyed over a different ucode_size) (1089); a command kind the latched build has not run in a validated task (3) |
| 19 | F3DLX.Rej fifo 2.08 / S2DEX fifo 2.08 `9332f30c` | F3DEX2 | 1 | 0.45% | 0.11% | no handler x1 | no build registered |
| 20 | F3DEX.NoN 1.23 `efe4b8f3` | F3DEX | 2 | 0.40% | 0.41% | partly native x2 | no handler for this id, but its 4 KiB text is a registered build's (the id is keyed over a different ucode_size) (591); a command kind the latched build has not run in a validated task (7) |
| 21 | F3DEX.NoN 1.21 `fd5a31cc` | F3DEX | 1 | 0.21% | 0.06% | partly native x1 | class-0 operand longer than the operand buffer (120) |
| 22 | F3DEX.NoN 1.22 `dff34c75` | F3DEX | 1 | 0.15% | 0.03% | no handler x1 | no build registered |
| 23 | F3DEX xbus 2.07 `cbe9a9f2` | F3DEX2 | 1 | 0.14% | 0.36% | partly native x1 | no handler for this id, but its 4 KiB text is a registered build's (the id is keyed over a different ucode_size) (620); a command kind the latched build has not run in a validated task (1) |
| 24 | F3D 2.0H `9d0d289f` | F3D | 1 | 0.13% | 0.16% | no handler x1 | no build registered |
| 25 | F3DEX.NoN fifo 2.06 `f8d3b203` | F3DEX2 | 1 | 0.13% | 0.14% | partly native x1 | no handler for this id, but its 4 KiB text is a registered build's (the id is keyed over a different ucode_size) (240); a command kind the latched build has not run in a validated task (2) |
| 26 | S2DEX fifo 2.08 `fa171b44` | other | 1 | 0.09% | 0.11% | partly native x1 | S2DEX LDTX_SPRITE / LDTX_RECT_R (813) |
| 27 | L3DEX 1.21 `3c7a64e0` | other | 1 | 0.05% | 0.13% | no handler x1 | no build registered |
| 28 | L3DEX fifo 2.08 `cd32d8e5` | F3DEX2 | 1 | 0.04% | 0.45% | no handler x1 | no build registered |
| 29 | F3D 2.0D `2f411927` | F3D | 1 | 0.01% | 0.14% | no handler x1 | no build registered |
| 30 | F3DEX fifo 2.08 / F3DEX.NoN fifo 2.08 `d2d1f9f2` | F3DEX2 | 1 | 0.01% | 0.11% | partly native x1 | (no reason given) (717) |
| 31 | F3D 2.0D `1d12f4b3` | F3D | 1 | 0.00% | 0.02% | no handler x1 | no build registered |
| 32 | F3DEX 1.21 `208a567e` | F3DEX | 1 | 0.00% | 0.00% | partly native x1 | yield request pending (1) |

Games per row:

- 190d04df S2DEX 1.07 / S2DEXD 1.07: Bakuretsu Muteki Bangaioh, Magical Tetris Challenge, Pokemon Puzzle League
- 97989ee9 F3DEX fifo 2.05 / F3DLX.Rej fifo 2.05: Chameleon Twist 2, NBA Jam 2000, NFL Blitz
- 7847866d F3DEX.NoN fifo 2.05 / L3DEX fifo 2.05: Armorines - Project S.W.A.R.M.
- 65ad6e1a F3DEX.NoN xbus 2.06 / L3DEX xbus 2.06: Command & Conquer
- bf9b1207 F3D 2.0D: Killer Instinct Gold
- 1c724a38 F3DZEX.NoN fifo 2.08I / S2DEX fifo 2.08: Legend of Zelda, The - Majora's Mask
- eb2cc8ed (no banner): Perfect Dark
- 5b327e0d S2DEX 1.06 / F3DEX.NoN 1.23: Yoshi's Story
- bfbf5496 (no banner): Star Wars - Rogue Squadron
- 0bbec7c3 F3DEX fifo 2.06: Gauntlet Legends
- 76c763bc F3DLX 1.21: Air Boarder 64, Wipeout 64
- a6c64e8d (no banner): Indiana Jones and the Infernal Machine
- 23cb9e85 F3DEX.Rej fifo 2.08: ECW Hardcore Revolution
- 3e9b9c3a F3D 2.0D: Blast Corps
- 3c51d836 F3DLX.Rej 1.21: Air Boarder 64, International Superstar Soccer 64
- 968dec0e F3D 2.0H: Wipeout 64
- 047252e8 F3DEX 1.23: SpaceStation Silicon Valley
- a9864f29 F3DEX xbus 2.08: Ms. Pac-Man - Maze Madness
- 9332f30c F3DLX.Rej fifo 2.08 / S2DEX fifo 2.08: Mario Party 3
- efe4b8f3 F3DEX.NoN 1.23: AeroGauge, Donald Duck - Goin' Quackers
- fd5a31cc F3DEX.NoN 1.21: Wetrix
- dff34c75 F3DEX.NoN 1.22: Iggy's Reckin' Balls
- cbe9a9f2 F3DEX xbus 2.07: Lode Runner 3-D
- 9d0d289f F3D 2.0H: Glover
- f8d3b203 F3DEX.NoN fifo 2.06: Twisted Edge - Extreme Snowboarding
- fa171b44 S2DEX fifo 2.08: Neon Genesis Evangelion
- 3c7a64e0 L3DEX 1.21: Wipeout 64
- cd32d8e5 L3DEX fifo 2.08: WinBack - Covert Operations
- 2f411927 F3D 2.0D: Blast Corps
- d2d1f9f2 F3DEX fifo 2.08 / F3DEX.NoN fifo 2.08: Mario Party 3
- 1d12f4b3 F3D 2.0D: Blast Corps
- 208a567e F3DEX 1.21: Robotron 64

(The script's own listing: `python3 tools/ucode_coverage.py test-output/coverage-2026-10-18/pass1`.)

## The hot list after the 2026-10-17 night recount (history)

Non-native ucodes ranked as below, over all 112 games, first launch with the shipped defaults (`GfxProbe=on`). The "no handler for this id, but its 4 KiB text is a registered build's" reason on rows 2 and 24 is the script's guess for texts that a latch made native somewhere (`9798`, `efe4`): they are latched probe builds, not registered ones. Under `loose` row 6 (`bf9b1207`, Killer Instinct Gold) drops out; everything else is as listed. Pass 4 (`loose`, second launch) has the lowest mean JIT share, 23.6 %.

|---|---|---|---|---|---|---|---|
| 1 | S2DEX 1.07 / S2DEXD 1.07 `190d04df` | other | 3 | 2.67% | 2.55% | no handler x3 | no build registered |
| 2 | F3DEX fifo 2.05 / F3DLX.Rej fifo 2.05 `97989ee9` | F3DEX2 | 3 | 2.17% | 1.67% | partly native x3 | no handler for this id, but its 4 KiB text is a registered build's (the id is keyed over a different ucode_size) (2116); a command kind the latched build has not run in a validated task (1212) |
| 3 | F3D 2.0H `4212d63f` | F3D | 2 | 1.79% | 1.79% | partly native x2 | not eligible (SP halted, RDP coalescing off or XBUS) (7548) |
| 4 | F3DEX.NoN fifo 2.05 / L3DEX fifo 2.05 `7847866d` | F3DEX2 | 1 | 0.89% | 0.89% | no handler x1 | no build registered |
| 5 | F3DEX.NoN xbus 2.06 / L3DEX xbus 2.06 `65ad6e1a` | F3DEX2 | 1 | 0.89% | 0.89% | no handler x1 | no build registered |
| 6 | F3D 2.0D `bf9b1207` | F3D | 1 | 0.89% | 0.89% | no handler x1 | no build registered |
| 7 | F3DZEX.NoN fifo 2.08I / S2DEX fifo 2.08 `1c724a38` | F3DEX2 | 1 | 0.89% | 0.89% | no handler x1 | no build registered |
| 8 | F3DEX xbus 2.07 `cbe9a9f2` | F3DEX2 | 1 | 0.89% | 0.89% | no handler x1 | no build registered |
| 9 | F3DEX xbus 2.08 `a9864f29` | F3DEX2 | 1 | 0.89% | 0.89% | no handler x1 | no build registered |
| 10 | (no banner) `eb2cc8ed` | other | 1 | 0.89% | 0.89% | no handler x1 | no build registered |
| 11 | S2DEX xbus 2.08 / S2DEX 1.07 `3ee63656` | other | 1 | 0.89% | 0.89% | no handler x1 | no build registered |
| 12 | F3DEX.NoN fifo 2.06 `f8d3b203` | F3DEX2 | 1 | 0.89% | 0.89% | no handler x1 | no build registered |
| 13 | S2DEX 1.06 / F3DEX.NoN 1.23 `5b327e0d` | F3DEX | 1 | 0.89% | 0.89% | no handler x1 | no build registered |
| 14 | (no banner) `bfbf5496` | other | 1 | 0.89% | 0.89% | no handler x1 | no build registered |
| 15 | F3DEX fifo 2.06 `0bbec7c3` | F3DEX2 | 1 | 0.89% | 0.89% | partly native x1 | the JIT's status poll timeout has dropped (an earlier task waited for the CPU) (3068); too many SP_STATUS polls (the JIT would halt) (1) |
| 16 | F3DLX 1.21 `76c763bc` | F3DEX | 2 | 0.86% | 0.40% | partly native x2 | an earlier task with these pointers loaded another ucode (677); class-0 operand longer than the operand buffer (14); G_LOAD_UCODE of another ucode (11) |
| 17 | (no banner) `a6c64e8d` | other | 1 | 0.83% | 0.57% | partly native x1 | a textured triangle with geometry mode bit 12 (997); a command the native loop does not implement (147); a triangle that the clipper discards (5) |
| 18 | F3DEX.Rej fifo 2.08 `23cb9e85` | F3DEX2 | 1 | 0.82% | 0.59% | no handler x1 | no build registered |
| 19 | F3D 2.0D `3e9b9c3a` | F3D | 1 | 0.78% | 0.24% | partly native x1 | G_MOVEWORD offset outside what a game writes (769) |
| 20 | F3DEX 1.21 `208a567e` | F3DEX | 2 | 0.77% | 0.16% | partly native x2 | not eligible (SP halted, RDP coalescing off or XBUS) (1060); yield request pending (1) |
| 21 | F3DLX.Rej 1.21 `3c51d836` | F3DEX | 2 | 0.70% | 0.68% | no handler x2 | no build registered |
| 22 | F3D 2.0H `968dec0e` | F3D | 1 | 0.67% | 0.13% | no handler x1 | no build registered |
| 23 | F3DEX 1.23 `047252e8` | F3DEX | 1 | 0.62% | 0.39% | partly native x1 | an earlier task with these pointers loaded another ucode (441); G_LOAD_UCODE of another ucode (387) |
| 24 | F3DEX.NoN 1.23 `efe4b8f3` | F3DEX | 2 | 0.37% | 0.42% | partly native x2 | no handler for this id, but its 4 KiB text is a registered build's (the id is keyed over a different ucode_size) (613); a command kind the latched build has not run in a validated task (7) |
| 25 | F3DLX.Rej fifo 2.08 / S2DEX fifo 2.08 `9332f30c` | F3DEX2 | 1 | 0.36% | 0.11% | no handler x1 | no build registered |
| 26 | F3DEX.NoN 1.21 `fd5a31cc` | F3DEX | 1 | 0.21% | 0.06% | partly native x1 | class-0 operand longer than the operand buffer (120) |
| 27 | F3DEX.NoN 1.22 `dff34c75` | F3DEX | 1 | 0.17% | 0.03% | no handler x1 | no build registered |
| 28 | S2DEX fifo 2.08 `fa171b44` | other | 1 | 0.10% | 0.11% | partly native x1 | S2DEX LDTX_SPRITE / LDTX_RECT_R (813) |
| 29 | L3DEX 1.21 `3c7a64e0` | other | 1 | 0.06% | 0.13% | no handler x1 | no build registered |
| 30 | F3D 2.0H `9d0d289f` | F3D | 1 | 0.05% | 0.16% | no handler x1 | no build registered |
| 31 | L3DEX fifo 2.08 `cd32d8e5` | F3DEX2 | 1 | 0.04% | 0.45% | no handler x1 | no build registered |
| 32 | F3D 2.0D `2f411927` | F3D | 1 | 0.01% | 0.14% | no handler x1 | no build registered |
| 33 | F3D 2.0D `1d12f4b3` | F3D | 1 | 0.00% | 0.02% | no handler x1 | no build registered |

(The "games per hot ucode" list of the script: `python3 tools/ucode_coverage.py test-output/coverage-2026-10-17/pass1`.)

## The hot list of the 2026-10-17 (evening) recount (history)

Non-native ucodes ranked by the mean over all 64 games of the share of a game's graphics RSP time that they keep on the JIT ("mean RSP time on JIT"; a ucode that is the only one of three games contributes 3/64 = 4.7 points).
A text is one row however many ids it has.

| # | ucode (4 KiB text) | family | games | mean RSP time on JIT | mean task share | state | reasons |
|---|---|---|---|---|---|---|---|
| 1 | S2DEX 1.06 / F3DEX.NoN 1.23 `5b327e0d` | F3DEX | 1 | 1.56% | 1.56% | no handler x1 | no build registered |
| 2 | F3DEX fifo 2.05 `97989ee9` | F3DEX2 | 1 | 1.56% | 1.56% | no handler x1 | no build registered |
| 3 | (no banner) `bfbf5496` | other | 1 | 1.56% | 1.56% | no handler x1 | no build registered |
| 4 | F3D 2.0H `4212d63f` | F3D | 1 | 1.56% | 1.56% | partly native x1 | not eligible (SP halted, RDP coalescing off or XBUS) (3198) |
| 5 | F3DEX fifo 2.06 `0bbec7c3` | F3DEX2 | 1 | 1.56% | 1.56% | partly native x1 | the JIT's status poll timeout has dropped (an earlier task waited for the CPU) (3068); too many SP_STATUS polls (the JIT would halt) (1) |
| 6 | S2DEX 1.07 `190d04df` | other | 1 | 1.55% | 1.34% | no handler x1 | no build registered |
| 7 | (no banner) `a6c64e8d` | other | 1 | 1.47% | 1.00% | partly native x1 | a textured triangle with geometry mode bit 12 (997); a command the native loop does not implement (147); a triangle that the clipper discards (5) |
| 8 | F3D 2.0D `3e9b9c3a` | F3D | 1 | 1.39% | 0.43% | partly native x1 | G_MOVEWORD offset outside what a game writes (769) |
| 9 | F3DEX 1.21 `208a567e` | F3DEX | 1 | 1.33% | 0.28% | partly native x1 | not eligible (SP halted, RDP coalescing off or XBUS) (1060) |
| 10 | F3D 2.0H `968dec0e` | F3D | 1 | 1.25% | 0.22% | no handler x1 | no build registered |
| 11 | F3DEX 1.23 `047252e8` | F3DEX | 1 | 1.14% | 0.67% | partly native x1 | an earlier task with these pointers loaded another ucode (441); G_LOAD_UCODE of another ucode (387) |
| 12 | F3DEX.NoN 1.23 `efe4b8f3` | F3DEX | 1 | 0.34% | 0.08% | partly native x1 | no handler for this id, but its 4 KiB text is a registered build's (the id is keyed over a different ucode_size) (108); (no reason given) (7) |
| 13 | F3DEX.NoN 1.22 `dff34c75` | F3DEX | 1 | 0.28% | 0.06% | no handler x1 | no build registered |
| 14 | F3D 2.0H `9d0d289f` | F3D | 1 | 0.10% | 0.28% | no handler x1 | no build registered |
| 15 | L3DEX 1.21 `3c7a64e0` | other | 1 | 0.08% | 0.22% | no handler x1 | no build registered |
| 16 | L3DEX fifo 2.08 `cd32d8e5` | F3DEX2 | 1 | 0.05% | 0.78% | no handler x1 | no build registered |
| 17 | F3D 2.0D `2f411927` | F3D | 1 | 0.02% | 0.25% | no handler x1 | no build registered |
| 18 | F3D 2.0D `1d12f4b3` | F3D | 1 | 0.00% | 0.03% | no handler x1 | no build registered |

Games per row:

- 5b327e0d S2DEX 1.06 / F3DEX.NoN 1.23: Yoshi's Story
- 97989ee9 F3DEX fifo 2.05: Chameleon Twist 2
- bfbf5496 (no banner): Star Wars - Rogue Squadron
- 4212d63f F3D 2.0H: Mischief Makers
- 0bbec7c3 F3DEX fifo 2.06: Gauntlet Legends
- 190d04df S2DEX 1.07: Pokemon Puzzle League
- a6c64e8d (no banner): Indiana Jones and the Infernal Machine
- 3e9b9c3a F3D 2.0D: Blast Corps
- 208a567e F3DEX 1.21: Glover
- 968dec0e F3D 2.0H: Wipeout 64
- 047252e8 F3DEX 1.23: SpaceStation Silicon Valley
- efe4b8f3 F3DEX.NoN 1.23: Donald Duck - Goin' Quackers
- dff34c75 F3DEX.NoN 1.22: Iggy's Reckin' Balls
- 9d0d289f F3D 2.0H: Glover
- 3c7a64e0 L3DEX 1.21: Wipeout 64
- cd32d8e5 L3DEX fifo 2.08: WinBack - Covert Operations
- 2f411927 F3D 2.0D: Blast Corps
- 1d12f4b3 F3D 2.0D: Blast Corps

## S2DEX 1.x registered: `190d` and `5b32` (2026-10-18; docs/ucode-s2dex1.md)

57 s desktop runs as above, `devel` 9859bd7 plus the working tree. Before = this file's 2026-10-18 recount (no build registered, every task on the JIT); after = tasks native / tasks, JIT share of the graphics RSP time.

| game | ucodes | before (native) | after (native) | JIT share after |
|---|---|---|---|---|
| Bakuretsu Muteki Bangaioh | S2DEX 1.07 `190d` | 0% | 2943 / 2944 (100%) | 0% |
| Magical Tetris Challenge | `190d` | 0% | 2687 / 2688 (100%) | 0% |
| Pokemon Puzzle League | `190d` + F3DEX 1.23 `0472` | 0% on `190d` (86% of the tasks) | 1651 + 268 / 1920 (100%) | 0% |
| Yoshi's Story | S2DEX 1.06 + F3DEX.NoN `5b32` | 0% | 2815 / 2816 (100%) | 0% |

Dr. Mario 64 (`d2d1`, F3DEX fifo 2.08 loading S2DEX 1.07) was already native and stays so; SpaceStation (F3DEX 1.23, 828 declines "G_LOAD_UCODE of another ucode") is unchanged and does not load an S2DEX 1.x text. The two builds leave the hot list; the loose tier's refusal of `190d` is explained in ucode-s2dex1.md section 8.
