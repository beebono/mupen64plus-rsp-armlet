# Native coverage of the graphics tasks: a recount on 64 games

How much of the graphics RSP work does the task-level native path (`GfxMode = hle`, [design.md](design.md#task-level-native-path)) take today, and what is left on the JIT? This is a recount on
`devel` 91d3e2e (the F3DEX2 2.03 entry already unregistered) with a per-ucode counter added for it (`src/ustats.c`, `ARMLET_UCODE_STATS=<file>`; no effect when unset). It replaces the guess that
[ucode-survey.md](ucode-survey.md) made from the dumps (that one counted texts, this one counts tasks and time), and it is the "hot list" that [ucode-derive.md](ucode-derive.md) is run on.

## Method

- **Corpus.** All 23 games of `test-romset/` and all ROMs of `unseen-romset/`: the directory holds **41** files, not the 40 that [ucode-survey.md](ucode-survey.md) counts, so 64 games in all.
- **Run.** One game per run, headless on x86_64 (`SDL_VIDEODRIVER=offscreen`, foir `NEON=0` build behind arMLEt's coalescing path, the scripted pad of `tools/equiv/inp_det.c`), `GfxMode = hle`, 57 s of wall clock, six
  runs at a time in memory-capped scopes (`tools/ucode_coverage_run.sh`). That is 900 to 10 000 graphics tasks per game (title / menu / first seconds of play, as in the survey), 146 944 in all. The counters are written
  every 128 graphics tasks, so a run that is killed loses up to 127 tasks (and the one in flight), which is why a one-ucode game shows `tasks - 1` native.
- **Counters** (`src/ustats.c`, keyed by the ucode id of `gfx_ucode.c`; `tools/ucode_coverage.py` merges ids that differ only by `ucode_size` into one 4 KiB text): per ucode the graphics tasks offered, the native runs, the
  declines with their reason (`hle_decline`'s string, `K.why`), the tasks of a ucode without a handler, and three times: native runs, declined native attempts (wasted), and the JIT's run of the tasks it took.
  **All times leave out the time inside the video plugin's `ProcessRdpList` callback** (`rdp_defer.c` times it): with it a native task of Banjo-Kazooie took 9.7 ms, mostly foir back-pressure at a full sync.
- **"Graphics RSP time on the JIT"** of a game = (JIT time of the tasks the JIT ran + the time wasted in declined native attempts) / (that + native time). It is a wall-clock share on the desktop x86_64 (the native front-end runs
  1.6 to 3.9 times the JIT's speed there; on the A55 the ratio differs, `docs/profiling.md` has the device numbers), so read it as "which ucodes the remaining JIT time belongs to", not as a device prediction.
  The profiler of `docs/profiling.md` was not used: it needs the `-prof` build and a per-ucode split it does not have; the counters above give the split directly.
- **Registered** means an `hle_ucode` entry with this exact id. The F3DEX2 front-end's `G_LOAD_UCODE` handling follows loaded builds by their own key, so S2DEX texts that a task loads are not counted as tasks of their own.

## Result in numbers

- 146 944 graphics tasks: **94 039 native (64.0 %)**, 8 951 declined by a handler that exists, 43 906 of a ucode without a handler.
- Mean over the 64 games: 68.7 % of the tasks native (test-romset 73.0 %, unseen-romset 66.2 %), **34.4 % of the graphics RSP time left on the JIT** (31.9 % test, 35.8 % unseen).
- 38 games run all their graphics tasks native (no JIT time at all); 15 games run none: F-Zero X, GoldenEye 007, Rocket, Yoshi's Story, Banjo-Tooie, Blues Brothers 2000, Chameleon Twist 2, Donald Duck, Excitebike 64,
  Gauntlet Legends, Harvest Moon 64, LEGO Racers, Star Wars Rogue Squadron, Star Wars Racer and Tonic Trouble (Mischief Makers: 1 task of 3200); 22 games keep half or more of their RSP time on the JIT.
- Of the 34.4 points of RSP time left on the JIT, **27.2 points belong to ucodes without a handler** and 7.2 to ucodes with one that declines (XBUS lists, `G_LOAD_UCODE` of an unregistered text, `G_MOVEWORD` offsets,
  a triangle form, ...). The ranking below is by these points.

### Decline reasons (tasks of ucodes that have a handler)

| reason | tasks | games |
|---|---|---|
| not eligible (SP halted, RDP coalescing off or XBUS) | 4275 | 2 |
| G_LOAD_UCODE of another ucode | 1930 | 1 |
| a textured triangle with geometry mode bit 12 | 997 | 1 |
| G_MOVEWORD offset outside what a game writes | 769 | 1 |
| G_LOAD_UCODE | 631 | 2 |
| a command the native loop does not implement | 147 | 1 |
| class-0 operand longer than the operand buffer | 134 | 2 |
| an earlier task with these pointers loaded another ucode | 63 | 1 |
| a triangle that the clipper discards | 5 | 1 |

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
| test | F-Zero X | 2944 | 0 | 0 | 2944 | 100% | F3DEX fifo 2.03 / F3DLX.Rej fifo 2.03 `33dc` 52% -; F3DFLX.Rej fifo 2.03F / L3DEX fifo 2.03 `c750` 38% -; F3DLX.Rej fifo 2.03 / F3DFLX.Rej fifo 2.03F `7de8` 10% - |
| test | Glover | 6144 | 3989 | 1077 | 1077 | 92% | F3DEX 1.21 `208a` 82% P; F3D 2.0H `9d0d` 18% - |
| test | GoldenEye 007 | 1408 | 0 | 0 | 1408 | 100% | F3D 2.0G `538f` 100% - |
| test | Jet Force Gemini | 1536 | 1535 | 0 | 0 | 0% | (no banner) `c2ac` 100% N |
| test | Kirby 64 - The Crystal Shards | 1536 | 1535 | 0 | 0 | 0% | F3DEX fifo 2.04H / L3DEX fifo 2.04H `e857` 100% N |
| test | Legend of Zelda, The - Ocarina of Time | 1280 | 1279 | 0 | 0 | 0% | F3DZEX.NoN fifo 2.06H / S2DEX fifo 2.05 / S2DEX fifo 2.05 [Safe] `0d3f` 100% N |
| test | Mario Kart 64 | 1280 | 1279 | 0 | 0 | 0% | F3DEX 0.95 `34da` 100% N |
| test | Mischief Makers | 3200 | 1 | 3198 | 0 | 100% | F3D 2.0H `4212` 100% P |
| test | Mystical Ninja Starring Goemon | 1792 | 1791 | 0 | 0 | 0% | F3DEX 1.23 `0472` 100% N |
| test | Paper Mario | 3200 | 3199 | 0 | 0 | 0% | F3DEX fifo 2.08 `c734` 100% N |
| test | Pokemon Snap | 1920 | 1919 | 0 | 0 | 0% | F3DEX.NoN fifo 2.08H / L3DEX fifo 2.08H `d576` 100% N |
| test | Rocket - Robot on Wheels | 1536 | 0 | 0 | 1536 | 100% | F3DEX fifo 2.07 `48c7` 100% - |
| test | SpaceStation Silicon Valley | 1920 | 1302 | 617 | 0 | 29% | F3DEX 1.23 `0472` 100% P |
| test | Star Fox 64 | 1536 | 1535 | 0 | 0 | 0% | F3DEX.NoN 1.22 `fd5a` 100% N |
| test | Super Mario 64 | 1536 | 1535 | 0 | 0 | 0% | F3D 2.0D `94c7` 100% N |
| test | Wetrix | 1792 | 1671 | 120 | 0 | 23% | F3DEX.NoN 1.21 `fd5a` 100% P |
| test | Yoshi's Story | 2944 | 0 | 0 | 2944 | 100% | S2DEX 1.06 / F3DEX.NoN 1.23 `5b32` 100% - |
| unseen | 007 - The World Is Not Enough | 1536 | 1535 | 0 | 0 | 0% | F3DEX fifo 2.08 `d2d1` 100% N |
| unseen | Banjo-Tooie | 896 | 0 | 0 | 896 | 100% | F3DEX.NoN fifo 2.08 / F3DEX fifo 2.08 `9875` 100% - |
| unseen | Blues Brothers 2000 | 3328 | 0 | 0 | 3328 | 100% | F3DEX fifo 2.06 `0bbe` 100% - |
| unseen | Bug's Life, A | 2176 | 2175 | 0 | 0 | 0% | F3DLX 1.23 `7502` 100% N |
| unseen | Chameleon Twist 2 | 3072 | 0 | 0 | 3072 | 100% | F3DEX fifo 2.05 `9798` 100% - |
| unseen | Clay Fighter - Sculptor's Cut | 3200 | 3199 | 0 | 0 | 0% | F3DLX.NoN 1.21 `0a7c` 100% N |
| unseen | Conker's Bad Fur Day | 1152 | 1151 | 0 | 0 | 0% | F3DEXBG.NoN fifo 2.08 `e48f` 100% N |
| unseen | Cruis'n World | 1408 | 1407 | 0 | 0 | 0% | F3DEX fifo 2.04 `6635` 100% N |
| unseen | Donald Duck - Goin' Quackers | 1536 | 0 | 0 | 1536 | 100% | F3DEX.NoN 1.23 `efe4` 100% - |
| unseen | Dr. Mario 64 | 2816 | 2815 | 0 | 0 | 0% | F3DEX fifo 2.08 / S2DEX 1.07 `d2d1` 100% N |
| unseen | Earthworm Jim 3D | 3072 | 3071 | 0 | 0 | 0% | F3DEX 1.23 `0472` 100% N |
| unseen | Excitebike 64 | 1408 | 0 | 0 | 1408 | 100% | F3DLX.Rej xbus 2.08 `8b0a` 100% - |
| unseen | Gauntlet Legends | 3072 | 0 | 0 | 3072 | 100% | F3DEX fifo 2.06 `0bbe` 100% - |
| unseen | Harvest Moon 64 | 4992 | 0 | 0 | 4992 | 100% | F3DEX fifo 2.06 `0bbe` 100% - |
| unseen | Hot Wheels - Turbo Racing | 2816 | 2815 | 0 | 0 | 0% | F3DEX fifo 2.08 / L3DEX fifo 2.08 `d2d1` 100% N |
| unseen | Hydro Thunder | 1280 | 1279 | 0 | 0 | 0% | F3DEX fifo 2.08 `d2d1` 100% N |
| unseen | Iggy's Reckin' Balls | 1920 | 1850 | 0 | 69 | 14% | F3DLX.NoN 1.23 `cc09` 96% N; F3DEX.NoN 1.22 `dff3` 4% - |
| unseen | Indiana Jones and the Infernal Machine | 1792 | 642 | 1149 | 0 | 93% | (no banner) `a6c6` 100% P |
| unseen | LEGO Racers | 2432 | 0 | 0 | 2432 | 100% | F3DEX.NoN fifo 2.08 `9875` 100% - |
| unseen | Mario Golf | 1536 | 1535 | 0 | 0 | 0% | F3DEX fifo 2.08 `d2d1` 100% N |
| unseen | Mario Tennis | 1408 | 1407 | 0 | 0 | 0% | F3DEX fifo 2.08 / F3DEX xbus 2.08 `d2d1` 100% N |
| unseen | Ogre Battle 64 - Person of Lordly Caliber | 1536 | 1535 | 0 | 0 | 0% | F3DEX fifo 2.08 / F3DEX.NoN fifo 2.08 `d2d1` 100% N |
| unseen | Penny Racers | 1664 | 1663 | 0 | 0 | 0% | F3DLX.NoN 1.23 `cc09` 100% N |
| unseen | Pilotwings 64 | 1664 | 1663 | 0 | 0 | 0% | F3D 2.0D `3e9b` 100% N |
| unseen | Pokemon Puzzle League | 1920 | 254 | 14 | 1652 | 99% | S2DEX 1.07 `190d` 86% -; F3DEX 1.23 `0472` 14% P |
| unseen | Pokemon Stadium 2 | 1280 | 1279 | 0 | 0 | 0% | F3DEX fifo 2.08 `c734` 100% N |
| unseen | Quake | 1664 | 1663 | 0 | 0 | 0% | F3DEX.NoN 1.21 `fd5a` 100% N |
| unseen | Quest 64 | 2048 | 2047 | 0 | 0 | 0% | F3DEX 1.23 `208a` 100% N |
| unseen | RR64 - Ridge Racer 64 | 2176 | 2175 | 0 | 0 | 0% | F3DEX fifo 2.08 `d2d1` 100% N |
| unseen | Resident Evil 2 | 7808 | 3862 | 1993 | 1952 | 69% | S2DEX fifo 2.06 / F3DEX 1.23 `5f33` 75% P; F3DEX.Rej fifo 2.06 / F3DEX.Rej xbus 2.06 `9512` 25% - |
| unseen | Star Wars - Rogue Squadron | 1280 | 0 | 0 | 1280 | 100% | (no banner) `bfbf` 100% - |
| unseen | Star Wars Episode I - Racer | 2304 | 0 | 0 | 2304 | 100% | F3DEX.NoN fifo 2.08 `9875` 100% - |
| unseen | Super Smash Bros. | 2944 | 2943 | 0 | 0 | 0% | F3DEX fifo 2.04H `e857` 100% N |
| unseen | Tetrisphere | 1152 | 1151 | 0 | 0 | 0% | F3D 2.0H `0b57` 100% N |
| unseen | Tonic Trouble | 1408 | 0 | 0 | 1408 | 100% | F3DEX.NoN fifo 2.07 `d8bd` 100% - |
| unseen | Toy Story 2 - Buzz Lightyear to the Rescue! | 1536 | 1535 | 0 | 0 | 0% | F3DLX 1.23 `7502` 100% N |
| unseen | Turok - Dinosaur Hunter | 1536 | 1535 | 0 | 0 | 0% | F3DEX.NoN 1.00 `5063` 100% N |
| unseen | Wave Race 64 - Kawasaki Jet Ski | 896 | 895 | 0 | 0 | 0% | F3D 2.0D `fb48` 100% N |
| unseen | WinBack - Covert Operations | 2560 | 1280 | 0 | 1279 | 5% | F3DEX fifo 2.08 / L3DEX fifo 2.08 `d2d1` 50% N; L3DEX fifo 2.08 `cd32` 50% - |
| unseen | Wipeout 64 | 9856 | 7026 | 14 | 2815 | 88% | F3DLX 1.21 `76c7` 71% P; L3DEX 1.21 `3c7a` 14% -; F3D 2.0H `968d` 14% - |
| unseen | Worms Armageddon | 2432 | 2431 | 0 | 0 | 0% | S2DEX fifo 2.08 `fa17` 100% N |

## The hot list: what is left, ranked

Non-native ucodes ranked by the mean over all 64 games of the share of a game's graphics RSP time that they keep on the JIT ("mean RSP time on JIT"; a ucode that is the only one of three games contributes 3/64 = 4.7 points).
A text is one row however many ids it has.

| # | ucode (4 KiB text) | family | games | mean RSP time on JIT | mean task share | state | reasons |
|---|---|---|---|---|---|---|---|
| 1 | F3DEX.NoN fifo 2.08 / F3DEX fifo 2.08 `9875370e` | F3DEX2 | 3 | 4.69% | 4.69% | no handler x3 | no build registered |
| 2 | F3DEX fifo 2.06 `0bbec7c3` | F3DEX2 | 3 | 4.69% | 4.69% | no handler x3 | no build registered |
| 3 | F3D 2.0G `538f79d8` | F3D | 1 | 1.56% | 1.56% | no handler x1 | no build registered |
| 4 | F3DEX fifo 2.07 `48c7e653` | F3DEX2 | 1 | 1.56% | 1.56% | no handler x1 | no handler for this id, but its 4 KiB text is a registered build's (the id is keyed over a different ucode_size) (1536) |
| 5 | S2DEX 1.06 / F3DEX.NoN 1.23 `5b327e0d` | F3DEX | 1 | 1.56% | 1.56% | no handler x1 | no build registered |
| 6 | F3DEX fifo 2.05 `97989ee9` | F3DEX2 | 1 | 1.56% | 1.56% | no handler x1 | no build registered |
| 7 | F3DEX.NoN 1.23 `efe4b8f3` | F3DEX | 1 | 1.56% | 1.56% | no handler x1 | no build registered |
| 8 | F3DLX.Rej xbus 2.08 `8b0a3e32` | F3DEX2 | 1 | 1.56% | 1.56% | no handler x1 | no build registered |
| 9 | (no banner) `bfbf5496` | other | 1 | 1.56% | 1.56% | no handler x1 | no build registered |
| 10 | F3DEX.NoN fifo 2.07 `d8bde272` | F3DEX2 | 1 | 1.56% | 1.56% | no handler x1 | no build registered |
| 11 | F3D 2.0H `4212d63f` | F3D | 1 | 1.56% | 1.56% | partly native x1 | not eligible (SP halted, RDP coalescing off or XBUS) (3198) |
| 12 | S2DEX 1.07 `190d04df` | other | 1 | 1.53% | 1.34% | no handler x1 | no build registered |
| 13 | (no banner) `a6c64e8d` | other | 1 | 1.45% | 1.00% | partly native x1 | a textured triangle with geometry mode bit 12 (997); a command the native loop does not implement (147); a triangle that the clipper discards (5) |
| 14 | F3D 2.0D `3e9b9c3a` | F3D | 1 | 1.37% | 0.43% | partly native x1 | G_MOVEWORD offset outside what a game writes (769) |
| 15 | F3DEX 1.21 `208a567e` | F3DEX | 1 | 1.36% | 0.27% | partly native x1 | not eligible (SP halted, RDP coalescing off or XBUS) (1077) |
| 16 | F3D 2.0H `968dec0e` | F3D | 1 | 1.27% | 0.22% | no handler x1 | no build registered |
| 17 | F3DEX fifo 2.03 / F3DLX.Rej fifo 2.03 `33dc705a` | F3DEX2 | 1 | 0.89% | 0.81% | no handler x1 | no build registered |
| 18 | S2DEX fifo 2.06 / F3DEX 1.23 `5f33af89` | F3DEX2 | 1 | 0.63% | 0.40% | partly native x1 | G_LOAD_UCODE of another ucode (1930); an earlier task with these pointers loaded another ucode (63) |
| 19 | F3DFLX.Rej fifo 2.03F / L3DEX fifo 2.03 `c750be26` | F3DEX2 | 1 | 0.57% | 0.59% | no handler x1 | no build registered |
| 20 | F3DEX 1.23 `047252e8` | F3DEX | 2 | 0.47% | 0.51% | partly native x2 | G_LOAD_UCODE (631) |
| 21 | F3DEX.Rej fifo 2.06 / F3DEX.Rej xbus 2.06 `95129dc7` | F3DEX2 | 1 | 0.45% | 0.39% | no handler x1 | no build registered |
| 22 | F3DEX.NoN 1.21 `fd5a31cc` | F3DEX | 1 | 0.37% | 0.11% | partly native x1 | class-0 operand longer than the operand buffer (120) |
| 23 | F3DEX.NoN 1.22 `dff34c75` | F3DEX | 1 | 0.21% | 0.06% | no handler x1 | no build registered |
| 24 | F3DLX.Rej fifo 2.03 / F3DFLX.Rej fifo 2.03F `7de803fd` | F3DEX2 | 1 | 0.10% | 0.16% | no handler x1 | no build registered |
| 25 | F3D 2.0H `9d0d289f` | F3D | 1 | 0.09% | 0.27% | no handler x1 | no build registered |
| 26 | L3DEX 1.21 `3c7a64e0` | other | 1 | 0.08% | 0.22% | no handler x1 | no build registered |
| 27 | L3DEX fifo 2.08 `cd32d8e5` | F3DEX2 | 1 | 0.08% | 0.78% | no handler x1 | no build registered |
| 28 | F3DLX 1.21 `76c763bc` | F3DEX | 1 | 0.03% | 0.00% | partly native x1 | class-0 operand longer than the operand buffer (14) |
| 29 | F3D 2.0D `2f411927` | F3D | 1 | 0.02% | 0.25% | no handler x1 | no build registered |
| 30 | F3D 2.0D `1d12f4b3` | F3D | 1 | 0.00% | 0.03% | no handler x1 | no build registered |

Games per row:

- 9875370e F3DEX.NoN fifo 2.08 / F3DEX fifo 2.08: Banjo-Tooie, LEGO Racers, Star Wars Episode I - Racer
- 0bbec7c3 F3DEX fifo 2.06: Blues Brothers 2000, Gauntlet Legends, Harvest Moon 64
- 538f79d8 F3D 2.0G: GoldenEye 007
- 48c7e653 F3DEX fifo 2.07: Rocket - Robot on Wheels
- 5b327e0d S2DEX 1.06 / F3DEX.NoN 1.23: Yoshi's Story
- 97989ee9 F3DEX fifo 2.05: Chameleon Twist 2
- efe4b8f3 F3DEX.NoN 1.23: Donald Duck - Goin' Quackers
- 8b0a3e32 F3DLX.Rej xbus 2.08: Excitebike 64
- bfbf5496 (no banner): Star Wars - Rogue Squadron
- d8bde272 F3DEX.NoN fifo 2.07: Tonic Trouble
- 4212d63f F3D 2.0H: Mischief Makers
- 190d04df S2DEX 1.07: Pokemon Puzzle League
- a6c64e8d (no banner): Indiana Jones and the Infernal Machine
- 3e9b9c3a F3D 2.0D: Blast Corps
- 208a567e F3DEX 1.21: Glover
- 968dec0e F3D 2.0H: Wipeout 64
- 33dc705a F3DEX fifo 2.03 / F3DLX.Rej fifo 2.03: F-Zero X
- 5f33af89 S2DEX fifo 2.06 / F3DEX 1.23: Resident Evil 2
- c750be26 F3DFLX.Rej fifo 2.03F / L3DEX fifo 2.03: F-Zero X
- 047252e8 F3DEX 1.23: Pokemon Puzzle League, SpaceStation Silicon Valley
- 95129dc7 F3DEX.Rej fifo 2.06 / F3DEX.Rej xbus 2.06: Resident Evil 2
- fd5a31cc F3DEX.NoN 1.21: Wetrix
- dff34c75 F3DEX.NoN 1.22: Iggy's Reckin' Balls
- 7de803fd F3DLX.Rej fifo 2.03 / F3DFLX.Rej fifo 2.03F: F-Zero X
- 9d0d289f F3D 2.0H: Glover
- 3c7a64e0 L3DEX 1.21: Wipeout 64
- cd32d8e5 L3DEX fifo 2.08: WinBack - Covert Operations
- 76c763bc F3DLX 1.21: Wipeout 64
- 2f411927 F3D 2.0D: Blast Corps
- 1d12f4b3 F3D 2.0D: Blast Corps

## What the table says

1. **Most of the rest is "no build registered".** Rows 1 to 10, 12 to 14, 16 and later with the state `no handler`: 27.2 of the 34.4 points. The five largest are F3DEX2 texts that are not far from a registered build:
   F3DEX.NoN 2.08 `9875` (Banjo-Tooie, LEGO Racers, Star Wars Racer: 4.7 points), F3DEX 2.06 `0bbe` (Blues Brothers 2000, Gauntlet Legends, Harvest Moon 64: 4.7), F3DEX 2.07 `48c7` (Rocket), F3DEX.NoN 2.07 `d8bd`
   (Tonic Trouble) and F3DEX 2.05 `9798` (Chameleon Twist 2): together 14.1 points, 12.5 of them as builds [ucode-derive.md](ucode-derive.md) calls EXACT or NEAR.
2. **Rocket's F3DEX 2.07 is the registered Donkey Kong 64 text under another id.** `ucode_size` of Rocket's tasks is 0xf80, Donkey Kong 64's 0x1000, and the id (`gfx_ucode.c`, FNV over `ucode_size` bytes) differs although the 4 KiB
   are identical: row 4 is a pure alias (the survey's recommendation "hash the full 4 KiB" applies; the registered F3DEX2 2.08 and F3DEX 1.x records already carry two to four ids each for that reason). Cost today: 1.6 points.
3. **Registered builds that decline are a front-end question, not a build question** (7.2 points): XBUS (Glover's F3DEX 1.21, Mischief Makers), `G_MOVEWORD` offsets in Blast Corps' Pilotwings-family text, the textured-triangle form of Indiana Jones,
   `G_LOAD_UCODE` of unregistered texts (Resident Evil 2, Pokemon Puzzle League, SpaceStation), a class-0 operand length (Wetrix). No derivation helps them.
4. **The rest is other microcode** (Rej / F3DLX.Rej / F3DFLX.Rej, L3DEX, S2DEX 1.06 / 1.07, F3D 2.0G / 2.0H re-layouts, Star Wars Rogue Squadron, Indiana Jones, F-Zero X's 2.03 and its Rej texts): 10 to 15 points that need kernels, not records.
5. **The first graphics task of a run is not counted native** (one per game; the table shows `tasks - 1` for the one-ucode games): the counters are flushed before the result of the task in flight.

## Reproduce

```
scripts/build.sh native                         # out/native/mupen64plus-rsp-armlet.so with src/ustats.c
for each ROM:  tools/ucode_coverage_run.sh ROM STATS/<set>/<game> 57       # six at a time; env as in the script's header
python3 -I tools/ucode_coverage.py STATS --md   # the tables of this page
```
