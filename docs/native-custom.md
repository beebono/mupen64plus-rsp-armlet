# Custom microcodes on the task-level native path: Diddy Kong Racing, Jet Force Gemini, Indiana Jones

Three games run a microcode of their own (no banner, no relative among the registered builds: [ucode-survey.md](ucode-survey.md)). Their command sets and arithmetic are written down in
[ucode-rare.md](ucode-rare.md) (Diddy Kong Racing and Jet Force Gemini) and [ucode-indy.md](ucode-indy.md) (Indiana Jones and the Infernal Machine). This page is about how they run natively, what
was tested and what is left on the JIT.

## Resident Evil 2 is not a custom microcode

The survey counts eight texts for Resident Evil 2 and calls six of them custom. They are not graphics microcodes: the 31 + 5 tasks with `ucode_size` 0x1000 and a 0x80-byte or 4-byte data segment are
the game's **video** (HVQM-style decode), **resize** and **fill** programs, which rsp-hle already runs as HLE (`ucode ... -> Resident Evil 2 (video)`, `(resize)`, `(fill)` in the log; they never reach
`HleForwardTask`). The two graphics texts, 99.6 % of its graphics tasks, are **S2DEX 2.06** (`5f33af89e713878b`, banner "S2DEX fifo 2.06", 75 %) and **F3DEX.Rej 2.06** (`95129dc7d307dd43`,
"F3DEX.Rej fifo / xbus 2.06", 25 %): standard families. Its S2DEX text is Kirby 64's (S2DEX 2.06) and is registered in `hle_s2dex2.c` (task-start entry `l_s2_re2`; [ucode-f3dex2-dialects.md](ucode-f3dex2-dialects.md) section 5.7); the F3DEX.Rej text and the six custom texts are not claimed.

## How they run

Closed forms, written by hand from the disassembly (`tools/native/disasm.py`), like the other families. Each microcode is a Fast3D descendant whose vertex record, clipper and triangle builder are
F3D's, so the arithmetic lives in the shared struct-level functions of `f3d_fx.h` (`f3d_vtx_xform`, `f3d_tri_setup`, `f3d_clip_tri`, in NEON where the F3D front-end has a NEON form) with the
differences of the three microcodes passed as parameters; what is their own is the data flow around them (DMA'd commands, the RDP lists, the state in DMEM), which the front-ends reproduce.

| front-end | microcodes | the F3D functions with |
|---|---|---|
| `src/hle/hle_rare.c` (one parameterised implementation) | Diddy Kong Racing `72a1416f6d8445c3`, Jet Force Gemini `410cfbcf87378a9c` | `has_off` (the vertex offset vector), `F3D_TRIV_RARE` (cull bits, unscaled texture block, no minimum z slope), `f3d_clip_tri` |
| `src/hle/hle_indy.c` | Indiana Jones `a6c64e8d82ca6346` | `fog_wide` (the 32-bit fog), `clip_acc` 1, `F3D_TRIV_INDY` (cull bit 13 only, own colour at every vertex, a triangle of area zero is rejected, not culled), `sort_given` (the vertex order from the microcode's table), `f3d_clip_tri_seq` (its clipper walks the polygon in input order) |

Both front-ends run the boot code and the initialisation (OS code, the overlay manager's first load) on the interpreter (`hle_rsp_run`, the exact reference of the framework) up to the command loop, then
take the display list from RDRAM in a C loop (the lean mode; `ARMLET_HLE_EXACT=1` goes through the microcode's own refills and DMAs), keep the microcode's state where it keeps it in DMEM, and hand the
outermost end of the list back to the interpreter (the exit code). A command, an overlay image or a state the front-end does not know declines the whole task before anything is written (the JIT runs it).

**What the framework needed.** `HLE_F_XBUS_OK` (hle.h): the two Rare builds put the RDP lists in a ring in DMEM and leave `DPC_STATUS.XBUS` set, so every task after the first used to be refused
("not eligible"); the commit now keeps the bytes of every XBUS list when `DPC_END` is written and replays the lists, consecutive ones as one, with the bytes put back into DMEM for the video plugin's
read (`hle_core.c`, `replay`). The XBUS lists of Mischief Makers, Glover, Excitebike would need the same. `HLE_F_REGS_LIVE` with `live_sregs`: the next task of Jet Force Gemini and of Indiana Jones reads
some scalar registers the previous one leaves (`ARMLET_HLE_POISON=1` changes the hash stream of Jet Force Gemini from the tenth call and of Indiana Jones from the 222nd), so verify and the harness compare
those too and the poison leaves them alone; the vector unit is never live. `rdp_defer.c` treats a banner-less ucode with a task-level handler as known in `GfxMode = hle`, so the RDP lists of Indiana
Jones are coalesced. `hle_task.c` captures and poisons tasks of registered banner-less ucodes. The harness has `--oracle` (a JIT run from the same state is the oracle, instead of the recorded result:
for perturbed and synthetic snapshots) and `armlet_hle_harness_jit`.

## What runs natively

| game | native in `GfxMode = hle` (3700 frames) | what stays on the JIT |
|---|---|---|
| Diddy Kong Racing | all 1663 tasks (and 3903 of 3904 in 9000 frames; the last task of a run is not counted) | nothing in the scripted run; see ucode-rare.md section 8 for what would decline |
| Jet Force Gemini | all 1791 tasks (4415 of 4416 in 9000 frames) | the same |
| Indiana Jones and the Infernal Machine | 746 of 1984 tasks (38 %; 2445 of 5184 in 9000 frames, 47 %) | by design: a textured triangle with geometry mode bit 12 (1085 tasks in 3700 frames), commands the loop does not implement (147), a triangle the clipper discards (5), clipping with fog, overlay images that were not seen (later scenes: 1322 more in 9000 frames) and the corners of ucode-indy.md section 7 |

Indiana Jones's microcode keeps junk in vector registers between commands and the output depends on it in two cases; those tasks decline instead of guessing, and the front-end is exact for the rest.
Resident Evil 2: see above, nothing here.

## Tests

Desktop, x86_64 (`tools/equiv/desk.sh`, foir `NEON=0`, offscreen, 2 GB scope; at most four runs at a time). The numbers are those of the final tree.

* **Equivalence gates**, 3700 frames (62 s) per game: `ref` twice (gate 1: Diddy Kong Racing, Jet Force Gemini, Indiana Jones identical), `armlet` with `GfxMode = hle` and in the default mode against
  `ref` (gates 2-4: hash stream, state stream, 6 screenshots, audio) - **all identical** for Diddy Kong Racing (3388 hash calls), Jet Force Gemini (3627), Indiana Jones (5564); and for the games that
  share the code that changed (`f3d_fx.h`, `f3d_vtx_fx.c`, `f3d_tri_fx.c`, `f3d_clip_fx.c`: `F3D_TRIV_INDY`, the sort order, wide fog, `f3d_clip_tri_seq`) or the framework: Super Mario 64 (F3D),
  Mischief Makers, Resident Evil 2, Star Fox 64 (F3DEX), Mario Kart 64 (F3DEX), Banjo-Kazooie (F3DEX), Worms Armageddon, Conker's Bad Fur Day, Dr. Mario 64 and WinBack (the F3DEX2 families):
  `hle` and default identical in all, `ARMLET_HLE_VERIFY=1` identical and **0 differences** (Star Fox 64 1791 tasks compared, Mario Kart 64 1471, Banjo-Kazooie 1535, Worms 2879, Conker 1215,
  Dr. Mario 64 3263, WinBack 1471).
* **`ARMLET_HLE_VERIFY=1`** (every native task is compared with a JIT run of the same task: all of DMEM outside the dead ranges, IMEM, the SP / DPC registers, the live scalar registers, the RDRAM the task
  writes, the RDP list bytes as one stream), 3700 and 9000 frames: Diddy Kong Racing 1663 / 3903 tasks compared, Jet Force Gemini 1791 / 4415, Indiana Jones 746 / 2445, **0 differences**.
* **Offline replay of captured tasks** (`ARMLET_UCODES=1 ARMLET_HLE_CAPTURE=<dir>` takes the task starts with the JIT as the reference, 40 per game of the first two and 509 of Indiana Jones;
  `armlet-native-replay`): the 120 Rare tasks all equal to the JIT's recorded result, the 509 Indiana Jones tasks 201 equal and 308 declined (by design), **on x86_64, on aarch64 under qemu with the
  NEON backend and on aarch64 under qemu scalar (`--scalar`, `-U__ARM_NEON`)**: 0 differences.
* **Synthetic tasks** (`tools/rare/synth.py` for the Rare pair, `tools/rare/synth_indy.py`): random scenes (matrices, viewports, vertex loads of every kind, every triangle flag combination, clipping,
  fog, offsets, texture and other-mode state, nested lists, a deferred list, small FIFOs that wrap, the colour tables, lights and environment mapping of Indiana Jones) run by the front-end and by the
  JIT from the same state (`--oracle`): Diddy Kong Racing 200 of 200 equal, Jet Force Gemini 97 equal and 103 declined (the clipper's room, and vertex records that the task did not make), Indiana Jones 193 equal
  and 107 declined; on x86_64 and on aarch64 under qemu with NEON and scalar: **0 differences**.
* **Perturbed tasks** (`tools/rsp2c/fuzz_snaps.py`): bytes of the rows the DMA reads (display list, vertices, triangles, matrices) flipped at random in four densities, words overwritten with words of other
  rows; the front-end and the JIT run from the same perturbed state. Rare: 225 + 720 snapshots, 285 equal, 0 mismatches, the rest declined or a JIT that hangs. Indiana Jones: 2000 snapshots (20 each of 100
  captured tasks), 153 equal, 0 mismatches, 1846 declined, 1 JIT hang. Under qemu a third / an eighth of those on NEON and scalar: 0 mismatches. Most perturbed tasks decline (a corrupted list jumps to an
  address that is no entry, polls the status too often, names a vertex the task did not make, ...): that is the declines doing their job.
* **Mutation checks** (`tools/rare/mutcheck.sh`): 72 deliberate errors, each built (`scripts/build.sh native --tools --hmut=N` for MUT(n, ...) in hle_rare.c (40-60) and hle_indy.c (170-201),
  `--gmut=N` for F3D_FX_MUTATE in the F3D functions: 27-30 the Rare vertex offset and triangle rules, 31 and 43-46 the wide fog, 32-36 `F3D_TRIV_INDY` and the sort table, 37-41 `f3d_clip_tri_seq`)
  and replayed against the captured and the synthetic tasks; a mutation is caught by a mismatch with the JIT, a crash, or a decline that the unmutated build does not have. **All 72 caught**: 71 on x86_64
  (31 of them are caught by the captured tasks, 35 more by the synthetic ones, 5 by a crash or by declines the unmutated build does not have; some by a single synthetic task), `--gmut=30` (the NEON form of the offset's flags) on aarch64 under qemu (9 mismatches). Mutations that no test could catch
  were equivalent: a version of 43 that read the low word of the fog multiplication after the last product changes nothing in 100 000 random fog states (the clamp hides it), and a plane-separation
  test by value in `f3d_clip_tri_seq` is the same as by truth value while the planes' masks are single bits; both were replaced by mutations of code that matters.
* **Poison** (`ARMLET_DMEM_POISON`, `ARMLET_HLE_POISON`): see "Dead ranges" in [testing.md](testing.md).

### What the tests found

The perturbed and synthetic runs and the poison runs found four differences in the code as first written (and one precaution); all are declines or fixes now:

* Diddy Kong Racing, perturbed: a triangle whose clipper input is a vertex record this task did not make (stale or inconsistent flags and coordinates) clips differently from the microcode's clipper.
  The front-end remembers which slots a vertex command of this task wrote (`vb_wr`, cleared when a matrix, vector or `G_MOVEWORD` overwrites the record) and declines a clipped triangle that uses another.
* Indiana Jones, synthetic: a deferred list that changes the geometry mode, the tile or a vertex record between the decision for a triangle and the building of its blocks (the game's deferred lists only set
  up the RDP), and a triangle without its shade or texture block (the microcode leaves rows from its vector registers behind the command; the game draws only shaded, textured triangles).
* Precaution: the JIT halts a task at 32767 status polls (one per command and one per triangle in Diddy Kong Racing); the front-end counted the two separately and now adds them.
* The test shim `tools/equiv/rsp_shim.c` took the ucode of a task from the OSTask header **after** the task ran; Indiana Jones's microcode uses the header (DMEM 0xfc0 .. 0xfd0) as scratch, so with that
  range poisoned the hash stream was not masked. It identifies the task before the call now.

## Timing (x86_64 desktop, indication only)

`armlet-native-replay --bench` on captured tasks (scalar x86 build, JIT time / native time): Diddy Kong Racing 2.4 to 3.4x, Indiana Jones tasks 2.7 to 3.0x. The aarch64 devices were not measured; the
expectation from the F3D / F3DEX measurements (docs/ucode-f3d.md) is a gain of the same order or larger there, since the JIT's vector operations are helper calls on both architectures;
`GfxMode = hle` is opt-in until it is measured.

## Reproduce

```
# captured task starts (JIT as the reference), 40 per game; the Indiana Jones set was taken with ARMLET_HLE_CAPTURE_EVERY=3 ARMLET_HLE_CAPTURE_MAX=600
ARMLET_UCODES=1 ARMLET_HLE_CAPTURE=test-output/rare/snap tools/equiv/desk.sh 'dkr|armlet-hle|test-romset/Diddy Kong Racing.z64|e2e|1500' ...
scripts/build.sh native --tools; scripts/build.sh aarch64 --tools; scripts/build.sh aarch64 --tools --scalar
out/native-tools/armlet-native-replay --quiet test-output/rare/snap/*.snap test-output/indy/snap/*.snap
qemu-aarch64-static -L /usr/aarch64-linux-gnu out/aarch64-tools/armlet-native-replay --quiet test-output/rare/snap/*.snap     # and aarch64-tools-scalar
# synthetic tasks (the template is a captured task start of the game; SYN_TEXT / INDY_TEXT the 0x3000 bytes of the ucode text as the game's RDRAM holds it)
SYN_TEXT=dkr.text python3 tools/rare/synth.py dkr test-output/rare/syn2 200 7 test-output/rare/snap/hle.task-72a1416f6d8445c3-3.snap
SYN_TEXT=jfg.text python3 tools/rare/synth.py jfg test-output/rare/synj2 200 7 test-output/rare/snap/hle.task-410cfbcf87378a9c-3.snap
INDY_TEXT=indy.text python3 tools/rare/synth_indy.py test-output/rare/syni 300 1 test-output/indy/snap/hle.task-a6c64e8d82ca6346-100.snap
out/native-tools/armlet-native-replay --oracle --quiet test-output/rare/syn2/*.snap          # synj2, syni
# perturbed tasks
python3 tools/rsp2c/fuzz_snaps.py test-output/rare/fuzz2 8 1 test-output/rare/snap/*.snap
REPLAY_ARGS=--oracle tools/rsp2c/replay_each.sh out/native-tools/armlet-native-replay test-output/rare/fuzz2/*.snap
# mutations
tools/rare/mutcheck.sh test-output/rare/snap test-output/indy/snap test-output/rare/syn2 test-output/rare/synj2 test-output/rare/syni
```

(The ucode text dumps are taken from the RDRAM of a run; the ucode survey's `tools/ucode_dump_run.sh` does it, docs/ucode-survey.md.)

Environment (development): `HLE_RARE_LOCKSTEP=1` (with `ARMLET_HLE_EXACT=1`: every command is run by the C code and by the interpreter, DMEM / registers / DPC compared; `HLE_LS_DUMP=<addr>`),
`HLE_RARE_TRACE`, `HLE_RARE_STATS`, `HLE_RARE_PROF`, `HLE_INDY_HYBRID=1 ARMLET_HLE_EXACT=1 HLE_INDY_LOCKSTEP=1` (the same for Indiana Jones), `HLE_INDY_TRACE`, `HLE_INDY_OVL=1` (prints the hashes of
the overlay images), `HLE_DUMP_STREAM` / `HLE_DUMP_STREAM_JIT` (the RDP stream of the native side / the JIT side of a replay), `HLE_POISON_SEL`, `HLE_EV_DUMP`, `HLE_RSP_TRACE_PC`.

## A new microcode of this kind

`tools/rsp2c/rsp2c.py` translates a handler's instructions into C on the exact vector-unit library (docs/testing.md, "Regenerating the microcode oracles"); an earlier version of this front-end ran a
translation of the whole microcode (3000 instructions per game) as the production path and was replaced by the closed forms once they matched it; the translations stay a development oracle (ROM-derived,
git-ignored), not a production path.
