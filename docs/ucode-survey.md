# Graphics ucode survey: exact hashes against normalised code

Question: could the task-level native path (`src/hle`, one `struct hle_ucode` per exact FNV-1a-64 id, see [design.md](design.md) "Graphics ucode identification" and "Task-level native path"; the
earlier per-routine `armlet_routine` hooks with `dep` / `hash` ranges are the same idea at routine granularity) match builds by *normalised code* instead, so that a build nobody has hand-registered still
gets the native front-end? This is an offline survey over 63 games; nothing here changes the tree. Follow-ups: [ucode-coverage.md](ucode-coverage.md) (task and time shares of the same games, per ucode) and [ucode-derive.md](ucode-derive.md) (the derivation of build records that this page recommends, as a tool).

## Method

- **Corpus.** All 23 games of `test-romset/` and all 40 of `unseen-romset/` (40 files), each run headless on x86_64 for 55 s of wall clock (about 1500 to 5000 graphics
  tasks per game; one game, Gauntlet Legends, produced 726 000 in that time) with the scripted pad of `tools/equiv/inp_det.c` (the title / menu / first seconds of play, not deep game states).
  Every graphics task (OSTask type 1) is seen by `tools/ucode_dump_shim.c`, a pass-through RSP plugin in front of parallel-rsp (LLE; foir is the video plugin) that writes the 4 KiB of ucode text
  (always the whole 0x1000 bytes, whatever `ucode_size` says), the data segment and the IMEM after the first run, once per distinct (text, data) pair, plus the sequence of ucode ids per task.
  `tools/ucode_dump_run.sh` runs one ROM in a memory-capped scope. The dumps are ROM-derived and live in a scratch directory only.
- **Analysis.** `tools/ucode_survey.py RUNSDIR [--handlers]` (`--selftest` is the mutation check of the normaliser). Nothing is written but text. Normalisation levels, applied to the RSP instruction
  words of the 4 KiB text:

  | level | masked |
  |---|---|
  | L0 | nothing (the exact bytes; what the FNV id keys on, except that the id covers only `ucode_size` bytes) |
  | S | printable-ASCII runs (version banners) |
  | L1 | S + `j` / `jal` targets and branch offsets |
  | L2 | L1 + offsets of scalar loads / stores and of `LWC2` / `SWC2` (the DMEM layout) |
  | L3 | L2 + ALU immediates (`addi addiu ori andi xori slti sltiu lui`: DMEM addresses built in registers, constants) |
  | L4 | skeleton: opcode (and funct) only, registers too |

  The selftest flips bits of a masked field and shows the hash is unchanged exactly at the levels that mask it, and that an opcode-bit flip is never hidden. L3 is generous: it also hides real
  constants (clip bit masks, shifts), so an L3 match is "same code shape", not "same behaviour".
- **Two ways to compare.** (1) *Whole-text hash*: one normalised hash of the 4 KiB (what a drop-in replacement of the id would be). (2) *Alignment*: `difflib` over the normalised word sequences
  (matching blocks of at least 4 words, blocks may sit at different addresses), which is the criterion a routine-level matcher has: every routine is found wherever it sits. `cov(A|B)` is the share
  of A's non-zero code words inside a matched block of B; a pair joins a cluster when both directions reach the threshold. 99.5% tolerates about 5 words, 98% about 20.
- **Handlers.** Command handlers are found from the DMEM jump tables in the data segment (F3DEX2: the 256-entry table at 0x26e..0x36e indexed by the signed opcode, targets 0x1000 + IMEM pc; F3DEX 1.x / F3D: every
  run of at least 12 code halfwords in the data). A handler is the code reachable from its entry (branches, `j`, and transitively `jal` callees), minus code that more than half of the handlers reach (command loop, refill, flush).
  The IMEM image of a task is `text[0:0xf80]` at 0x80 and `text[0xf80:0x1000]` at 0 (the microcode loads the tail itself; checked against the IMEM after the run, 94% equal for the F3DEX2 builds, the rest is the exit stub and overlays).

## Result in numbers

Distinct graphics ucodes: **764 (text, data) pairs, 58 FNV ids (today's key), 53 distinct 4 KiB texts.** The five extra ids are the same text hashed over 0xf80 or 0x1000 bytes (34 018 tasks pass `ucode_size` 0xf80,
the rest 0x1000), so identification should always hash the full 4 KiB.

Banner families (the ucode ID string in text / data; "other" has none): F3D "SW Version 2.0D / 2.0G / 2.0H", F3DEX 1.x (F3DEX, F3DLX, .NoN, 0.95 .. 1.23), F3DEX2 (F3DEX / F3DLX / F3DZEX / L3DEX / F3DEXBG, fifo and xbus, .NoN, .Rej, 2.03 .. 2.08),
S2DEX 1.06 / 1.07 / 2.06 / 2.08, and 10 texts without a banner (Rare: Diddy Kong Racing, Jet Force Gemini; Resident Evil 2 with 6 custom texts; Indiana Jones; Wipeout 64's `L3DEX 1.21`).

**Whole-text classes** (ids / distinct classes by level; each row counts the 58 ids, so L0 already merges the five size duplicates):

| family | ids | L0 | S | L1 | L2 | L3 | L4 |
|---|---|---|---|---|---|---|---|
| F3D | 10 | 10 | 10 | 10 | 10 | 10 | 10 |
| F3DEX 1.x | 13 | 11 | 11 | 11 | 11 | 11 | 11 |
| F3DEX2 | 21 | 18 | 18 | 18 | 18 | 16 | 15 |
| S2DEX | 4 | 4 | 4 | 4 | 4 | 4 | 4 |
| other | 10 | 10 | 10 | 10 | 9 | 9 | 9 |
| **all** | **58** | **53** | **53** | **53** | **52** | **50** | **49** |

A normalised hash of the whole text merges almost nothing (53 to 50 even with every immediate masked, 49 as a bare skeleton): two builds of one family are the same code with some blocks moved and a few
words changed, and any such move changes the sequence. Whole-text normalised hashing is not worth building.

**Alignment clusters** (same 53 texts; number of clusters at L1 / L2 / L3): T = 99.5%: 47 / 46 / 45; T = 98%: 33 / 32 / 32; T = 95%: 28 / 25 / 25. The L3 clusters at 98%:

| cluster | builds | games |
|---|---|---|
| F3DEX2 2.07 (1fde, d8bd .NoN), 2.08 (ac31, c734), .NoN 2.08 (9875), .NoN 2.08H (d576) | 6 | 18: 007, Banjo-Tooie, Donkey Kong 64, Dr. Mario 64, Hot Wheels, Hydro Thunder, LEGO Racers, Mario Golf / Tennis, Ogre Battle, Paper Mario, Pokemon Snap, Pokemon Stadium 2, RR64, Rocket, Star Wars Racer, Tonic Trouble, WinBack (partly) |
| F3DEX 1.21, 1.23, .NoN 1.22 (two), .NoN 1.23 | 5 | 13: Banjo-Kazooie, Bomberman Hero, Donald Duck, Earthworm Jim 3D, Glover, Iggy's, Mystical Ninja, Pokemon Puzzle League, Quake, Quest 64, SpaceStation, Star Fox 64, Wetrix |
| F3DLX 1.21 / 1.23, .NoN | 4 | 6: A Bug's Life, Clay Fighter, Iggy's, Penny Racers, Toy Story 2, Wipeout 64 |
| F3DEX2 2.05 / 2.06 / F3DZEX.NoN 2.06H | 3 | 5: Blues Brothers, Chameleon Twist 2, Gauntlet Legends, Harvest Moon 64, Ocarina of Time |
| F3D 2.0D (3e9b, 94c7) + 2.0H (88fa) | 3 | 4: Pilotwings 64, Super Mario 64, Tetrisphere, Blast Corps |
| F3DEX2 2.04 / 2.04H | 2 | 4: Buck Bumble, Cruis'n World, Kirby 64, Smash Bros. |
| S2DEX 2.06 / 2.08 | 2 | Resident Evil 2, Worms |
| Resident Evil 2 custom | 3 + 2 | Resident Evil 2 |

Left alone (their closest relative covers under 98%): F3D 2.0D 1d12 / 2f41 (Blast Corps), 2.0D fb48 (Wave Race), 2.0G (GoldenEye, 98%), 2.0H 4212 / 968d / 9d0d (Mischief Makers, Wipeout 64, Glover), F3DEX 0.95 / 1.00 (Mario Kart, Turok),
F3DEX2 2.03 and the Rej / LX / FLX / BG / L3DEX builds (F-Zero X, Resident Evil 2, Excitebike, Conker, WinBack), S2DEX 1.06 / 1.07, Rare's two, Indiana Jones, Wipeout's L3DEX.

**Generalisation.** For each game, which share of its tasks runs a ucode that is covered, and how many games are covered completely (every ucode that is at least 1% of the game's tasks). "Known" is, in the first
table, the texts the 23 test-romset games use; in the others the 22 exact ids registered in `src/hle` today (20 of them occur in the survey, as 18 texts).

| tier (a ucode is covered when a known text matches at the tier) | unseen games vs test-romset: mean share of tasks / games of 40 | all 63 games vs registered ids: mean share / games of 63 | unseen only vs registered: mean share / games of 40 |
|---|---|---|---|
| exact 4 KiB text | 17.9% / 7 | 46.8% / 26 | 34.6% / 12 |
| L1, every word matched (blocks may move) | 17.9% / 7 | 46.8% / 26 | 34.6% / 12 |
| L2, every word matched | 17.9% / 7 | 46.8% / 26 | 34.6% / 12 |
| L3, every word matched | 49.1% / 19 | 66.6% / 38 | 65.8% / 24 |
| L1 at 99.5% | 56.6% / 22 | 71.4% / 41 | 73.3% / 27 |
| L3 at 99.5% | 56.6% / 22 | 71.4% / 41 | 73.3% / 27 |
| L3 at 98% | 64.2% / 25 | 74.6% / 44 | 78.4% / 30 |
| L3 at 95% | 68.8% / 27 | 82.8% / 49 | 83.4% / 32 |

(Over the 23 test-romset games alone the registered ids already cover 14 completely, 68.0% of tasks; at L3 98% the same 14; at 95% 17.) Reading: masking jump targets and load / store offsets alone adds nothing
over the exact id; **the gain comes from the ALU immediates (L3)**, i.e. the DMEM addresses that the builds put in registers and a few constants. With them, the registered 22 ids would cover 38 of 63 games completely
instead of 26 at no tolerance, 41 at 99.5%, 44 at 98%.

Games that stay uncovered even at L3 98% (best coverage by a registered build in brackets): Diddy Kong Racing (79%), Jet Force Gemini (76%), Yoshi's Story (3%, S2DEX), Pokemon Puzzle League (3%, S2DEX 85% of tasks), Excitebike 64 (23%, F3DLX.Rej xbus),
Indiana Jones (6%), Resident Evil 2 (18 to 32%), Worms (18%, S2DEX), Wipeout 64 (45% and 18%), WinBack (45%, L3DEX), F-Zero X (93% / 30% / 31%: 2.03 and the Rej builds), Glover (17%, F3D 2.0H 9d0d), Conker (93%, BG),
and at 96 to 98% Buck Bumble, Cruis'n World, Kirby 64, Smash Bros. (F3DEX2 2.04 / 2.04H, about 30 to 40 real words from 2.05+), GoldenEye (2.0G, 98%), Blast Corps (F3D 2.0D 2f41, 96%).

## What differs between near builds

Closest partner of each build, aligned at L3, for the 40 pairs with at least 90% coverage; the raw words in the aligned blocks that differ, split by the field that differs (pairs with the maximum in brackets):
**812 jump / branch fields, 189 load / store offsets, 101 ALU immediates, and 478 words with no counterpart** (a genuine code change) in about 41 000 aligned words. Per pair that is 10 to 55 relocated jumps (mostly 15 to 30) (zero for some
2.03 / 2.06 / 2.07 pairs whose code did not move), no load / store offset difference inside the F3DEX2 2.0x and F3DEX 1.x dialects (9 to 17 for F3D 2.0D against 2.0H, F3DEX 0.95 against 1.00, S2DEX 1.06 against 1.07; 56 for Conker's BG),
0 to 5 ALU immediates (9 for Conker, 16 for the Rare pair), and 0 to 3 unmatched words inside a dialect (12 to 55 between dialects: Conker 12, S2DEX 1.06 / 1.07 19 to 31, Rare's DKR / JFG 51 to 55). Inside a dialect a build is the code of its neighbour plus a table of a few dozen jump pcs and a handful of constants.

**Handlers** (reference = the build with the highest mean coverage; "unmatched" = instructions of the handler without a counterpart in the other build; ≤ 4 means a handler a front-end could reuse after checking the differing words):

| group | handlers / instructions | share of (handler, build) instructions with 0 / ≤ 4 / ≤ 16 unmatched at L2 | at L3 |
|---|---|---|---|
| F3DEX2, the 6-build 2.07 / 2.08 cluster | 32 / 2678 | 30% / 100% / 100% | 84% / 100% / 100% |
| F3DEX 1.x, EX + NoN cluster (5) | 25 / 1055 | 71% / 92% / 100% | 92% / 92% / 100% |
| F3DEX 1.x, LX cluster (4) | 25 / 1081 | 62% / 89% / 100% | 89% / 89% / 100% |
| F3D, 2.0D + 2.0H cluster (3) | 29 / 942 | 76% / 100% / 100% | 100% / 100% / 100% |
| F3DEX2, 2.05 / 2.06 / ZEX cluster (3) | 46 / 5160 | 26% / 100% / 100% | 61% / 100% / 100% |
| F3DEX2, whole family (18) | 33 / 2682 | 13% / 46% / 57% | 37% / 50% / 60% |
| F3DEX 1.x, whole family (11) | 25 / 1055 | 50% / 68% / 84% | 63% / 68% / 84% |
| F3D, whole family (10) | 29 / 942 | 37% / 54% / 61% | 48% / 54% / 61% |

Inside the F3DEX2 cluster every handler is identical to L3 in all five partners, and at L2 (immediates kept) the only handlers that differ are `G_VTX` (694 instructions; median 3 words) and `G_TRI1` / `G_TRI2`
(452 each; median 2) plus a few handlers of 8 to 66 instructions (CULLDL, SETTIMG ...) in one or two partners, i.e. those differences are ALU immediates; everything else (DL, MTX, MOVEMEM, MOVEWORD, SETOTHERMODE, RDP appenders, ENDDL ...) is byte-identical modulo pcs and DMEM offsets. In the F3DEX 1.x
clusters the 427-instruction shared triangle code (pc 0x940 / 0x968) is identical to L3 in all partners and the 168-instruction stretch at 0x3a8 differs by about 8 real instructions in two of the four partners. Across dialects it is the vertex and
triangle code that differ for real (F3DEX2 whole family: `G_VTX` / `G_TRI1` / `G_TRI2` have a median of 23 to 27 unmatched instructions, the near-clipping builds against .NoN, LX, Rej and BG; the other handlers have a median of 0 to 2 unmatched instructions); in F3D the five largest handlers (211, 114, 97, 65, 52 instructions) differ by 13 to 17 instructions in 6 of the 9 other builds
(identical in the 2.0D / 2.0H cluster), the small ones are identical.

## What would break, or needs care

- **The data segment is not part of the identity.** The same text arrives with many data images: 764 (text, data) pairs over 53 texts. Buck Bumble changes one word (data 0x6e0) every few tasks (31 images), Gauntlet Legends has 257
  (it also passes a 0xc00-byte data segment, 529 607 tasks), Banjo-Tooie / LEGO / Star Wars Racer 120 images differing from 0x420 (the part of the F3DEX2 data image that is DMEM state: matrices, lights, viewport), Resident Evil 2 24. A
  front-end must read the jump table, layout constants and descriptors from the task's data at run time (or verify them against the build's expectation) and must not hash the data.
- **Several ucodes alternate between tasks.** Not swaps inside a task (not observable from here) but a different text at consecutive graphics tasks: Blast Corps (3 F3D builds, 35 changes per 100 tasks), Glover (F3DEX 1.21 and F3D 2.0H,
  35 per 100), Wipeout 64 (F3D 2.0H, F3DLX 1.21, L3DEX: 57 per 100), Resident Evil 2 (8 texts, 50 per 100, mostly two alternating), WinBack (100 per 100: strict alternation of two). Ucode selection must be per task and cheap (the existing four-slot recent-text cache
  does that); a per-game decision is wrong for these 6 games. F-Zero X (3 texts), Iggy's and Pokemon Puzzle League switch rarely (0.1 to 0.2 per 100).
- **Self-modification and overlays.** The text itself is never altered in the runs (IMEM after the first task equals the expected image in 94% of the words: the other 6% is the exit stub at 0 and the overlay window at 0xfd0..0xfff;
  Ocarina of Time 82%: its lighting / clip overlays, other overlays are loaded from the 4 KiB itself). Overlay code is DMAed from the text region, so it is covered by a hash of the full 4 KiB, not of `ucode_size` bytes. Pokemon Puzzle League's S2DEX 1.07 (5%) and the Rare / Indiana Jones
  texts are loaded elsewhere (Indiana Jones' text sits at IMEM 0; the others rotate like the standard boot): the layout is a property of the build too. A `G_LOAD_UCODE` (0xdd) inside a task, which swaps the ucode mid-task, cannot be seen by this survey (it
  records the ucode at task start); the F3DEX2 front-end follows it to a registered build ([design.md](design.md#build-switching-g_load_ucode); the games that do it: [ucode-f3dex2-dialects.md](ucode-f3dex2-dialects.md) 5.7), the others decline such tasks.
- **L3 hides constants.** An L3 match proves the instruction shape, not the behaviour: the 101 differing ALU immediates in the closest pairs include flag masks and shifts the native code must take from the build. A normalised
  match must therefore produce the build's parameter record (pcs, layout offsets, constants) from the aligned words and use the existing verify mode (`ARMLET_HLE_VERIFY`) and `tools/equiv` per newly admitted build; it cannot be a blind admit.
- **Custom and non-F3D families need their own kernels** whatever the matching: S2DEX (4 builds, 3 games), Rare's DKR / JFG (one family of 2, 93% close, their own dispatch), Resident Evil 2 (a 2+3 cluster family), Indiana Jones, Wipeout's L3DEX, and the Rej / LX / BG
  F3DEX2 dialects (no registered build is within 30% of the Rej ones).
- **Limits of the survey.** 55 s per game with a scripted pad: later-game ucodes are missed (Smash Bros. shows only 2.04H, Zelda only F3DZEX.NoN, Jet Force Gemini and DKR one text each). Counting is by 4 KiB text; ids differing only by `ucode_size` are one text.
  Handler extents come from static control flow plus the jump tables and have no oracle (indirect `jr` other than the dispatch are not followed); the alignment numbers are position-independent but treat reordered blocks as equal.
  "Games covered" is a property of the ucode text, not of the front-end's other conditions (declines, near-clipping, vertex counts).

## Recommendation

Normalised **identification** pays only when it is done per routine / per handler and parameterised; a normalised whole-text hash does not (53 texts become 50). The numbers: against the 22 ids registered today, the exact id covers 26 of the 63 games completely (46.8% of a game's tasks on average);
matching at L3 with the aligned-word criterion adds 12 games (38 of 63, 66.6%), and tolerating up to about 20 real words (L3 at 98%) 18 games (44 of 63, 74.6%); on the 40 unseen games alone the step is 12 to 24 (every word matched) to 30 (98%).
Those are the 5 clusters of F3DEX 1.x / F3DEX2 / F3D whose members differ only by relocated jumps, DMEM offsets and a handful of constants, i.e. the families the front-ends already exist for. The step from "claim by id" to "claim a build whose text matches a known build's normalised handlers and
whose differing words all fall in the masked fields" is therefore worthwhile for the F3DEX2 2.07 / 2.08 cluster (18 games), the F3DEX 1.x EX / NoN and LX clusters (13 and 6 games), the F3DEX2 2.05 / 2.06 / ZEX cluster (5) and the F3D 2.0D / 2.0H cluster (4), if three conditions hold: (1) the build record
(`struct hle_ucode`: layout, pcs, variants, constants) is **derived** from the aligned words and the data image of the new build instead of written by hand, and the derivation declines on any word outside the permitted fields; (2) a newly seen
build passes `ARMLET_HLE_VERIFY` and the `tools/equiv` hash stream on at least one game before being admitted to a list, exactly like a hand-registered one (so the games actually gained, 12 to 18 more than today, depend on how many of them run clean); (3) identification
hashes the full 4 KiB and ignores the data image.
Not worth it: whole-text hashing; matching across dialects (near-clip against .NoN, LX against EX2, 2.04 against 2.05: the vertex / triangle code differs by 20 to 40 instructions); the S2DEX, Rej / LX2, Rare, Resident Evil 2 and Indiana Jones texts (about 12 games),
which need their own kernels in any case. The F3DEX2 2.04 / 2.04H pair (Buck Bumble, Cruis'n World, Kirby, Smash) is the best next hand-written build: it is the largest group at 96% that exact matching and L3 both miss. (Done since: [ucode-f3dex2-dialects.md](ucode-f3dex2-dialects.md) section 5. The "20 to 40 real words" were five differences in
the vertex stores, the clipper and the lighting overlay, which is not in the 4 KiB the survey dumped, plus the triangle constants of the F3DZEX generation.)

## Reproduce

```
# needs a built x86_64 mupen64plus-core, console, foir (NEON=0) and the parallel-rsp plugin (out/native/ref); see tools/equiv/desk.sh for the pieces
cc -O2 -shared -fPIC -I../mupen64plus-core/src/api -o udump.so tools/ucode_dump_shim.c -ldl
UD_SO=udump.so CORE_DIR=... CONSOLE=... FOIR_SO=... PRSP_SO=out/native/ref/mupen64plus-rsp-parallel.so INPUT_SO=out/native/tools/input-det.so \
  tools/ucode_dump_run.sh "../test-romset/Super Mario 64.z64" runs/test-romset/"Super Mario 64" 55      # once per ROM, 6 at a time
python3 -I tools/ucode_survey.py --selftest
python3 -I tools/ucode_survey.py runs --handlers      # all tables above, about 2 min
```
