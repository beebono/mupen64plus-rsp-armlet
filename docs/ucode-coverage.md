# Native coverage of the graphics tasks: a recount on 64 games

How much of the graphics RSP work does the task-level native path (`GfxMode = hle`, [design.md](design.md#task-level-native-path)) take today, and what is left on the JIT? This is a recount on
`devel` 91d3e2e (the F3DEX2 2.03 entry already unregistered) with a per-ucode counter added for it (`src/ustats.c`, `ARMLET_UCODE_STATS=<file>`; no effect when unset). It replaces the guess that
[ucode-survey.md](ucode-survey.md) made from the dumps (that one counted texts, this one counts tasks and time), and it is the "hot list" that [ucode-derive.md](ucode-derive.md) is run on. **It was run three times**: before the ucode id fix and the registration of the four derived builds (`devel` 91d3e2e) and after (2026-10-16, [Result in numbers](#result-in-numbers) has both); the tables below are the second.

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

**The first recount (91d3e2e), as it was:**

- 146 944 graphics tasks: **94 039 native (64.0 %)**, 8 951 declined by a handler that exists, 43 906 of a ucode without a handler.
- Mean over the 64 games: 68.7 % of the tasks native (test-romset 73.0 %, unseen-romset 66.2 %), **34.4 % of the graphics RSP time left on the JIT** (31.9 % test, 35.8 % unseen).
- 38 games ran all their graphics tasks native (no JIT time at all); 15 ran none: F-Zero X, GoldenEye 007, Rocket, Yoshi's Story, Banjo-Tooie, Blues Brothers 2000, Chameleon Twist 2, Donald Duck, Excitebike 64,
  Gauntlet Legends, Harvest Moon 64, LEGO Racers, Star Wars Rogue Squadron, Star Wars Racer and Tonic Trouble (Mischief Makers: 1 task of 3200); 22 games kept half or more of their RSP time on the JIT.
- Of the 34.4 points of RSP time left on the JIT, **27.2 points belonged to ucodes without a handler** and 7.2 to ucodes with one that declines.

The tables of the games and of the hot list below are the **2026-10-17** counts; the decline reasons are the 2026-10-17 ones too.

### Decline reasons (tasks of ucodes that have a handler)

| reason | tasks | games |
|---|---|---|
| not eligible (SP halted, RDP coalescing off or XBUS) | 4275 | 2 |
| the JIT's status poll timeout has dropped (an earlier task waited for the CPU) | 3068 | 1 |
| a textured triangle with geometry mode bit 12 | 997 | 1 |
| G_MOVEWORD offset outside what a game writes | 769 | 1 |
| G_LOAD_UCODE | 631 | 2 |
| a command the native loop does not implement | 147 | 1 |
| class-0 operand longer than the operand buffer | 134 | 2 |
| a triangle that the clipper discards | 5 | 1 |
| too many SP_STATUS polls (the JIT would halt) | 1 | 1 |

`not eligible` is the check at the start of a task (SP halted, no coalescing path, or the RDP list in DMEM / XBUS without `HLE_F_XBUS_OK`): Glover's F3DEX 1.21 (1077 tasks, a third of its tasks use the XBUS transport)
and Mischief Makers' F3D 2.0H (3198, all of them). The `G_LOAD_UCODE` ones are Resident Evil 2 (an F3DEX.Rej text that no build covers), SpaceStation and Pokemon Puzzle League.

## The games

Columns: graphics tasks offered, native, declined by a handler, of a ucode without a handler, and the share of the game's graphics RSP time that stayed on the JIT. The last column lists the ucodes of the game (banner, first
four hex digits of the 4 KiB text hash, share of the game's tasks, state: N native, P partly native, D handler that always declines, `-` no handler).

| set | game | gfx tasks | native | declined | no handler | graphics RSP time on the JIT | ucodes (task share, state) |
|---|---|---|---|---|---|---|---|
| test | Banjo-Kazooie | 1408 | 1407 | 0 | 0 | 0% | F3DEX 1.21 `208a` 100% N |
| test | Blast Corps | 2816 | 1544 | 769 | 502 | 89% | F3D 2.0D `3e9b` 82% P; F3D 2.0D `2f41` 16% -; F3D 2.0D `1d12` 2% - |
| test | Bomberman Hero | 2560 | 2559 | 0 | 0 | 0% | F3DEX 1.21 `208a` 100% N |
| test | Buck Bumble | 3328 | 3327 | 0 | 0 | 0% | F3DEX fifo 2.04 `6635` 100% N |
| test | Diddy Kong Racing | 1408 | 1407 | 0 | 0 | 0% | (no banner) `909f` 100% N |
| test | Donkey Kong 64 | 1408 | 1407 | 0 | 0 | 0% | F3DEX fifo 2.07 / L3DEX fifo 2.07 `48c7` 100% N |
| test | F-Zero X | 2944 | 2943 | 0 | 0 | 0% | F3DEX fifo 2.03 / F3DLX.Rej fifo 2.03 `33dc` 52% N; F3DFLX.Rej fifo 2.03F / L3DEX fifo 2.03 `c750` 38% N; F3DLX.Rej fifo 2.03 / F3DFLX.Rej fifo 2.03F `7de8` 10% N |
| test | Glover | 6144 | 3989 | 1077 | 1077 | 94% | F3DEX 1.21 `208a` 82% P; F3D 2.0H `9d0d` 18% - |
| test | GoldenEye 007 | 1408 | 0 | 0 | 1408 | 100% | F3D 2.0G `538f` 100% - |
| test | Jet Force Gemini | 1536 | 1535 | 0 | 0 | 0% | (no banner) `c2ac` 100% N |
| test | Kirby 64 - The Crystal Shards | 1536 | 1535 | 0 | 0 | 0% | F3DEX fifo 2.04H / L3DEX fifo 2.04H `e857` 100% N |
| test | Legend of Zelda, The - Ocarina of Time | 1280 | 1279 | 0 | 0 | 0% | F3DZEX.NoN fifo 2.06H / S2DEX fifo 2.05 / S2DEX fifo 2.05 [Safe] `0d3f` 100% N |
| test | Mario Kart 64 | 1280 | 1279 | 0 | 0 | 0% | F3DEX 0.95 `34da` 100% N |
| test | Mischief Makers | 3200 | 1 | 3198 | 0 | 100% | F3D 2.0H `4212` 100% P |
| test | Mystical Ninja Starring Goemon | 1792 | 1791 | 0 | 0 | 0% | F3DEX 1.23 `0472` 100% N |
| test | Paper Mario | 3200 | 3199 | 0 | 0 | 0% | F3DEX fifo 2.08 `c734` 100% N |
| test | Pokemon Snap | 1920 | 1919 | 0 | 0 | 0% | F3DEX.NoN fifo 2.08H / L3DEX fifo 2.08H `d576` 100% N |
| test | Rocket - Robot on Wheels | 1536 | 1535 | 0 | 0 | 0% | F3DEX fifo 2.07 `48c7` 100% N |
| test | SpaceStation Silicon Valley | 1920 | 1302 | 617 | 0 | 27% | F3DEX 1.23 `0472` 100% P |
| test | Star Fox 64 | 1536 | 1535 | 0 | 0 | 0% | F3DEX.NoN 1.22 `fd5a` 100% N |
| test | Super Mario 64 | 1536 | 1535 | 0 | 0 | 0% | F3D 2.0D `94c7` 100% N |
| test | Wetrix | 1792 | 1671 | 120 | 0 | 23% | F3DEX.NoN 1.21 `fd5a` 100% P |
| test | Yoshi's Story | 2944 | 0 | 0 | 2944 | 100% | S2DEX 1.06 / F3DEX.NoN 1.23 `5b32` 100% - |
| unseen | 007 - The World Is Not Enough | 1536 | 1535 | 0 | 0 | 0% | F3DEX fifo 2.08 `d2d1` 100% N |
| unseen | Banjo-Tooie | 896 | 895 | 0 | 0 | 0% | F3DEX.NoN fifo 2.08 / F3DEX fifo 2.08 `9875` 100% N |
| unseen | Blues Brothers 2000 | 3328 | 3327 | 0 | 0 | 0% | F3DEX fifo 2.06 `0bbe` 100% N |
| unseen | Bug's Life, A | 2176 | 2175 | 0 | 0 | 0% | F3DLX 1.23 `7502` 100% N |
| unseen | Chameleon Twist 2 | 3072 | 0 | 0 | 3072 | 100% | F3DEX fifo 2.05 `9798` 100% - |
| unseen | Clay Fighter - Sculptor's Cut | 3200 | 3199 | 0 | 0 | 0% | F3DLX.NoN 1.21 `0a7c` 100% N |
| unseen | Conker's Bad Fur Day | 1152 | 1151 | 0 | 0 | 0% | F3DEXBG.NoN fifo 2.08 `e48f` 100% N |
| unseen | Cruis'n World | 1408 | 1407 | 0 | 0 | 0% | F3DEX fifo 2.04 `6635` 100% N |
| unseen | Donald Duck - Goin' Quackers | 1536 | 0 | 0 | 1536 | 100% | F3DEX.NoN 1.23 `efe4` 100% - |
| unseen | Dr. Mario 64 | 2816 | 2815 | 0 | 0 | 0% | F3DEX fifo 2.08 / S2DEX 1.07 `d2d1` 100% N |
| unseen | Earthworm Jim 3D | 3072 | 3071 | 0 | 0 | 0% | F3DEX 1.23 `0472` 100% N |
| unseen | Excitebike 64 | 1408 | 1407 | 0 | 0 | 0% | F3DLX.Rej xbus 2.08 `8b0a` 100% N |
| unseen | Gauntlet Legends | 3072 | 2 | 3069 | 0 | 100% | F3DEX fifo 2.06 `0bbe` 100% P |
| unseen | Harvest Moon 64 | 4992 | 4991 | 0 | 0 | 0% | F3DEX fifo 2.06 `0bbe` 100% N |
| unseen | Hot Wheels - Turbo Racing | 2816 | 2815 | 0 | 0 | 0% | F3DEX fifo 2.08 / L3DEX fifo 2.08 `d2d1` 100% N |
| unseen | Hydro Thunder | 1280 | 1279 | 0 | 0 | 0% | F3DEX fifo 2.08 `d2d1` 100% N |
| unseen | Iggy's Reckin' Balls | 1920 | 1850 | 0 | 69 | 16% | F3DLX.NoN 1.23 `cc09` 96% N; F3DEX.NoN 1.22 `dff3` 4% - |
| unseen | Indiana Jones and the Infernal Machine | 1792 | 642 | 1149 | 0 | 93% | (no banner) `a6c6` 100% P |
| unseen | LEGO Racers | 2432 | 2431 | 0 | 0 | 0% | F3DEX.NoN fifo 2.08 `9875` 100% N |
| unseen | Mario Golf | 1536 | 1535 | 0 | 0 | 0% | F3DEX fifo 2.08 `d2d1` 100% N |
| unseen | Mario Tennis | 1408 | 1407 | 0 | 0 | 0% | F3DEX fifo 2.08 / F3DEX xbus 2.08 `d2d1` 100% N |
| unseen | Ogre Battle 64 - Person of Lordly Caliber | 1536 | 1535 | 0 | 0 | 0% | F3DEX fifo 2.08 / F3DEX.NoN fifo 2.08 `d2d1` 100% N |
| unseen | Penny Racers | 1664 | 1663 | 0 | 0 | 0% | F3DLX.NoN 1.23 `cc09` 100% N |
| unseen | Pilotwings 64 | 1664 | 1663 | 0 | 0 | 0% | F3D 2.0D `3e9b` 100% N |
| unseen | Pokemon Puzzle League | 1920 | 254 | 14 | 1652 | 100% | S2DEX 1.07 `190d` 86% -; F3DEX 1.23 `0472` 14% P |
| unseen | Pokemon Stadium 2 | 1280 | 1279 | 0 | 0 | 0% | F3DEX fifo 2.08 `c734` 100% N |
| unseen | Quake | 1664 | 1663 | 0 | 0 | 0% | F3DEX.NoN 1.21 `fd5a` 100% N |
| unseen | Quest 64 | 2048 | 2047 | 0 | 0 | 0% | F3DEX 1.23 `208a` 100% N |
| unseen | RR64 - Ridge Racer 64 | 2176 | 2175 | 0 | 0 | 0% | F3DEX fifo 2.08 `d2d1` 100% N |
| unseen | Resident Evil 2 | 7808 | 7807 | 0 | 0 | 0% | S2DEX fifo 2.06 / F3DEX 1.23 `5f33` 75% N; F3DEX.Rej fifo 2.06 / F3DEX.Rej xbus 2.06 `9512` 25% N |
| unseen | Star Wars - Rogue Squadron | 1280 | 0 | 0 | 1280 | 100% | (no banner) `bfbf` 100% - |
| unseen | Star Wars Episode I - Racer | 2304 | 2303 | 0 | 0 | 0% | F3DEX.NoN fifo 2.08 `9875` 100% N |
| unseen | Super Smash Bros. | 2944 | 2943 | 0 | 0 | 0% | F3DEX fifo 2.04H `e857` 100% N |
| unseen | Tetrisphere | 1152 | 1151 | 0 | 0 | 0% | F3D 2.0H `0b57` 100% N |
| unseen | Tonic Trouble | 1408 | 1407 | 0 | 0 | 0% | F3DEX.NoN fifo 2.07 `d8bd` 100% N |
| unseen | Toy Story 2 - Buzz Lightyear to the Rescue! | 1536 | 1535 | 0 | 0 | 0% | F3DLX 1.23 `7502` 100% N |
| unseen | Turok - Dinosaur Hunter | 1536 | 1535 | 0 | 0 | 0% | F3DEX.NoN 1.00 `5063` 100% N |
| unseen | Wave Race 64 - Kawasaki Jet Ski | 896 | 895 | 0 | 0 | 0% | F3D 2.0D `fb48` 100% N |
| unseen | WinBack - Covert Operations | 2560 | 1280 | 0 | 1279 | 4% | F3DEX fifo 2.08 / L3DEX fifo 2.08 `d2d1` 50% N; L3DEX fifo 2.08 `cd32` 50% - |
| unseen | Wipeout 64 | 9856 | 7026 | 14 | 2815 | 85% | F3DLX 1.21 `76c7` 71% P; L3DEX 1.21 `3c7a` 14% -; F3D 2.0H `968d` 14% - |
| unseen | Worms Armageddon | 2560 | 2559 | 0 | 0 | 0% | S2DEX fifo 2.08 `fa17` 100% N |

## The hot list: what is left, ranked

Non-native ucodes ranked by the mean over all 64 games of the share of a game's graphics RSP time that they keep on the JIT ("mean RSP time on JIT"; a ucode that is the only one of three games contributes 3/64 = 4.7 points).
A text is one row however many ids it has.

| # | ucode (4 KiB text) | family | games | mean RSP time on JIT | mean task share | state | reasons |
|---|---|---|---|---|---|---|---|
| 1 | F3D 2.0G `538f79d8` | F3D | 1 | 1.56% | 1.56% | no handler x1 | no build registered |
| 2 | S2DEX 1.06 / F3DEX.NoN 1.23 `5b327e0d` | F3DEX | 1 | 1.56% | 1.56% | no handler x1 | no build registered |
| 3 | F3DEX fifo 2.05 `97989ee9` | F3DEX2 | 1 | 1.56% | 1.56% | no handler x1 | no build registered |
| 4 | F3DEX.NoN 1.23 `efe4b8f3` | F3DEX | 1 | 1.56% | 1.56% | no handler x1 | no build registered |
| 5 | (no banner) `bfbf5496` | other | 1 | 1.56% | 1.56% | no handler x1 | no build registered |
| 6 | F3D 2.0H `4212d63f` | F3D | 1 | 1.56% | 1.56% | partly native x1 | not eligible (SP halted, RDP coalescing off or XBUS) (3198) |
| 7 | F3DEX fifo 2.06 `0bbec7c3` | F3DEX2 | 1 | 1.56% | 1.56% | partly native x1 | the JIT's status poll timeout has dropped (an earlier task waited for the CPU) (3068); too many SP_STATUS polls (the JIT would halt) (1) |
| 8 | S2DEX 1.07 `190d04df` | other | 1 | 1.54% | 1.34% | no handler x1 | no build registered |
| 9 | (no banner) `a6c64e8d` | other | 1 | 1.46% | 1.00% | partly native x1 | a textured triangle with geometry mode bit 12 (997); a command the native loop does not implement (147); a triangle that the clipper discards (5) |
| 10 | F3DEX 1.21 `208a567e` | F3DEX | 1 | 1.41% | 0.27% | partly native x1 | not eligible (SP halted, RDP coalescing off or XBUS) (1077) |
| 11 | F3D 2.0D `3e9b9c3a` | F3D | 1 | 1.38% | 0.43% | partly native x1 | G_MOVEWORD offset outside what a game writes (769) |
| 12 | F3D 2.0H `968dec0e` | F3D | 1 | 1.22% | 0.22% | no handler x1 | no build registered |
| 13 | F3DEX 1.23 `047252e8` | F3DEX | 2 | 0.43% | 0.51% | partly native x2 | G_LOAD_UCODE (631) |
| 14 | F3DEX.NoN 1.21 `fd5a31cc` | F3DEX | 1 | 0.37% | 0.11% | partly native x1 | class-0 operand longer than the operand buffer (120) |
| 15 | F3DEX.NoN 1.22 `dff34c75` | F3DEX | 1 | 0.24% | 0.06% | no handler x1 | no build registered |
| 16 | L3DEX 1.21 `3c7a64e0` | other | 1 | 0.08% | 0.22% | no handler x1 | no build registered |
| 17 | L3DEX fifo 2.08 `cd32d8e5` | F3DEX2 | 1 | 0.07% | 0.78% | no handler x1 | no build registered |
| 18 | F3D 2.0H `9d0d289f` | F3D | 1 | 0.06% | 0.27% | no handler x1 | no build registered |
| 19 | F3DLX 1.21 `76c763bc` | F3DEX | 1 | 0.03% | 0.00% | partly native x1 | class-0 operand longer than the operand buffer (14) |
| 20 | F3D 2.0D `2f411927` | F3D | 1 | 0.02% | 0.25% | no handler x1 | no build registered |
| 21 | F3D 2.0D `1d12f4b3` | F3D | 1 | 0.00% | 0.03% | no handler x1 | no build registered |

Games per row:

- 538f79d8 F3D 2.0G: GoldenEye 007
- 5b327e0d S2DEX 1.06 / F3DEX.NoN 1.23: Yoshi's Story
- 97989ee9 F3DEX fifo 2.05: Chameleon Twist 2
- efe4b8f3 F3DEX.NoN 1.23: Donald Duck - Goin' Quackers
- bfbf5496 (no banner): Star Wars - Rogue Squadron
- 4212d63f F3D 2.0H: Mischief Makers
- 0bbec7c3 F3DEX fifo 2.06: Gauntlet Legends
- 190d04df S2DEX 1.07: Pokemon Puzzle League
- a6c64e8d (no banner): Indiana Jones and the Infernal Machine
- 208a567e F3DEX 1.21: Glover
- 3e9b9c3a F3D 2.0D: Blast Corps
- 968dec0e F3D 2.0H: Wipeout 64
- 047252e8 F3DEX 1.23: Pokemon Puzzle League, SpaceStation Silicon Valley
- fd5a31cc F3DEX.NoN 1.21: Wetrix
- dff34c75 F3DEX.NoN 1.22: Iggy's Reckin' Balls
- 3c7a64e0 L3DEX 1.21: Wipeout 64
- cd32d8e5 L3DEX fifo 2.08: WinBack - Covert Operations
- 9d0d289f F3D 2.0H: Glover
- 76c763bc F3DLX 1.21: Wipeout 64
- 2f411927 F3D 2.0D: Blast Corps
- 1d12f4b3 F3D 2.0D: Blast Corps