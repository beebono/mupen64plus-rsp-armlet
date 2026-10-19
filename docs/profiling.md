# Profiling the graphics microcode

arMLEt runs every graphics task on parallel-rsp's JIT. Before deciding how much of that to replace with native code, the profiler measures
where the time goes: per game and microcode family, which *routines* of the ucode (vertex transform and lighting, triangle setup, display-list
control, RDP command copying, command dispatch, DMA) the JIT time belongs to, how much of a frame it is, and what the same work would cost as
native NEON code. This page describes the profiler, the offline tools and the method, then the results, and ends with the decision table.

What it found (rgdsplus, RK3566, 4x Cortex-A55; scenes and windows below; every number is a mean of at least three alternating runs):

- **The JIT proper takes 1.5-4.5 ms per frame in the regular scenes and 12.9 ms in the Pokemon Snap zoom shot**; that is 26-43 % of the emulation thread by perf and 30-46 % by timing. It runs at about 11.4 ns,
  18 cycles, per executed RSP instruction.
- **Two routines are 69-86 % of it in all seven regular scenes: triangle setup (42-53 %) and vertex transform and lighting (18-43 %).** Dispatch is 3-6 %, DMA 5-11 %, matrices 1-5 %, copying RDP commands 2-7 %,
  display-list control and state commands 0.4-3.4 % each. Mario Kart 64 in its game-select menu is the exception (RDP command copying 28 %, dispatch 9 %, triangle 23 %): a scene with almost no triangles.
- **The three ucode families split the time the same way**: F3DEX2 (Donkey Kong 64, Ocarina of Time, Paper Mario, Snap), F3DEX 1.x (Banjo-Kazooie, Mario Kart 64) and F3D (Super Mario 64) all spend
  more than two thirds of the JIT time in vertex and triangle code; the routines differ per family, the shape does not.
- **Two native routines (vertex, triangle) would save 63-76 % of the JIT proper, 1.1-3.1 ms per frame (9.8 ms in the Snap zoom); the whole display list native 91-95 %, which is 0.2-2.4 ms more** (5-13 % of
  the emulation thread). The native costs are assumptions (a model, not a measurement); the ranking holds within a factor of two on them.
- In the Snap zoom shot 82 % of the emulation thread's time outside the video plugin's callback is the graphics JIT (12.9 of 15.6 ms), audio HLE 4 %, the rest of the core 13 %. The callback, 13.9 ms, is mostly
  waiting (11 ms) for the video plugin's pixel workers, so a faster RSP shortens the emulation thread but the frame time is then bounded by the workers.
- The profiler costs 0.8-4.4 % of the RSP time at level 1 (0.09-0.25 ms per frame; 8 % in the Mario Kart 64 menu, which has little else), and nothing the emulation can see: the RSP state is identical, call by call.
  The profiler build with the profiler off costs nothing measurable.

## How to run it

```sh
scripts/build.sh rg35xxsp --prof                     # levels 1-2 -> out/rg35xxsp-prof/   (a separate .so; the normal module has none of this)
scripts/build.sh rg35xxsp --prof3                    # + the per-instruction counts of level 3 -> out/rg35xxsp-prof3/
tools/equiv/build-tools.sh rg35xxsp; tools/ucodemap/build.sh
SESSION_X=<work dir on the device> PIN=1 tools/session/host.sh <ssh host> <label> 'TAG|ROM|SO|FRAMES|ENV' ...
tools/session/desk.sh 'TAG|ROM|SO|FRAMES|ENV' ...    # the same run on the desktop (x86_64, offscreen, memory-capped scope)
```

A run line is `TAG|ROM|SO|FRAMES|ENV`: `SO` is `armlet`, `armlet-prof` or `armlet-prof3` (the module under test, run behind `rsp-shim.so`),
`FRAMES` the VI frames at which foir takes a screenshot (`FOIR_SHOT_FRAMES`), `ENV` `VAR=value` pairs joined with `+` (see `tools/session/runner.sh`).
The profiler snapshots all its counters when each screenshot file appears, and the shim logs a `MARK` line at the same RSP call, so a
*window* is the difference of two marks. Sessions are one `host.sh` call, under the device's lock (`flock`), about five minutes, with the
clocks pinned (rgdsplus: performance governor; rg35xxsp: `cpu_perf.sh on`) and restored afterwards, a private `HOME` / `XDG_*` / `--configdir`,
`Core[RandomizeInterrupt] = False` and the deterministic pad script of the equivalence harness (`INP_DET_IDLE=1`: no input at all). Nothing
that names a device is in the repository: hosts, work directories and ROM directories come from the environment.

| level (`ARMLET_PROF=`) | what | cost |
|---|---|---|
| 1 | per (task type, ucode id): calls, time, time in the video plugin's `ProcessRdpList` (and `CheckInterrupts`), JIT block lookup and compile time, IMEM invalidations, SP DMA, the RDP lists the ucode submits (calls, bytes, commands by opcode) | see below |
| 1 + `ARMLET_PROF_WALK=1` | a read-only display-list walker at the start of every graphics task (F3D / F3DEX / F3DEX2 tables): commands, vertices, triangles, matrices per opcode | outside the timed region |
| 1 + `ARMLET_PROF_TASKLOG=1` | one line per `DoRspCycles` call: its times, its counters, the walker's counts, the dynamic instructions per class | |
| 2 | the JIT address map: every compiled region with the code offset of each instruction, the VU stubs, the thunks, and the code bytes | |
| 3 | `--prof3` only: how often every instruction of the ucode ran, every control-flow edge, and which display-list command each instruction belonged to | 4-10x slower: counts only |

## Method

**Time.** `ARMLET_PROF=1` wraps `HleForwardTask` (the one place a graphics task leaves rsp-hle) and swaps `ProcessRdpList` / `CheckInterrupts` in the
`RSP_INFO` copy that parallel-rsp gets, so no fork code is needed for the timing. Clock reads are `cntvct_el0` (one `mrs`, 24 MHz on both devices). A `ProcessRdpList`
call is timed in full when its list holds a full sync (the call that waits for the video plugin's workers) and for a random 1 in 8 of the others, scaled (`ARMLET_PROF_CBSAMPLE`; the
per-task log times every call); the profiler's own list parse is timed on every 16th call and scaled. A call to `DoRspCycles` for a graphics task is then

    rsp-hle overhead  +  parallelRSPDoRspCycles  =  JIT proper + RDP callback + CheckInterrupts + the profiler's own parse

**JIT proper** is the pure RSP part: JIT code, helpers, SP DMA, block lookup. The **RDP callback** is the video plugin's work for the lists the ucode
submits (foir's list parse and setup, and its waits), executed synchronously on the emulation thread inside the RSP call.

**Where in the microcode.** The fork's `prsp_hooks.h` gives the profiler (a) every compiled region with its code address and the code offset of
each instruction (a marker node per instruction in Lightning's stream; the emitted code is unchanged, checked below) and (b) with `PRSP_PROFILE` a counter
call before each instruction. A *site* is a (pc, instruction word) pair, so overlays loaded at the same pc are separate sites. The ucode is a loop
that fetches a display-list command and jumps through a table to a handler; everything an instruction does is charged to the command being handled:

1. **J1**, the first-level dispatch jump, is found while running: it is the register jump that a task with no `G_CULLDL` / `G_BRANCH_Z` (so the
   ucode ran exactly the commands the walker counted) executes exactly once per command, in three tasks in a row.
2. A **context** is the target of J1 and, for ucodes that dispatch twice (F3D and F3DEX 1.x look at the command class first, then the opcode), the target
   of the table-driven jump (a `jr` through a register loaded by `lh` / `lhu`) that follows. Every executed instruction is counted in the current context.
   A first-level target is a *stage* when nearly every entry goes on to a second-level jump after at most 80 instructions; its few instructions are then
   shared among the second-level handlers in proportion to their entries. Shared subroutines are charged to whichever command called them, by construction:
   no static analysis of calls and returns is needed, and the register jumps need none either.
3. A handler is **named by the opcodes it serves**: from the jump table in DMEM when the walker's clean-task counts fit it exactly (F3DEX2 family: the
   halfword at `0x36e + 2 * int8(op)` in the games here), otherwise by matching its number of entries with the walker's count of one opcode or of the group
   of RDP commands from 0xC0 up. Several opcodes with the same count (the halves of a texture rectangle) are marked ambiguous; the category is the majority.
4. Instructions executed about once per command whatever the command (the command fetch and loop) are **dispatch**; writes to the SP DMA registers and the
   DMA-busy polls are **dma**; writes to `CMD_START` / `CMD_END` are **rdp-submit**; code before the first dispatch of a task is **task init**. All else is
   charged to its handler's category: **vertex** (`G_VTX`, `G_MODIFYVTX`), **triangle** (`TRI1`, `TRI2`, `QUAD`, `LINE3D`), **matrix** (`MTX`, `POPMTX`,
   `MOVEMEM`, `MOVEWORD`), **dl** (`DL`, `ENDDL`, `CULLDL`, `BRANCH_Z`), **state** (geometry mode, texture, other modes, RDP halves), **rdp** (the RDP commands
   0xC0 and up that the ucode copies into its output buffer).
5. **Validation:** the executions of J1 must equal the walker's command count, the sum of the handler entries must equal that, and where the table is known
   each handler's entries must equal the walker's count of its opcodes in clean tasks. `unattributed` is the part of the instructions not charged to any
   context (it should be 0).

**Cost.** Instruction counts are not time, so the per-site counts are weighted by the cost of an instruction of each class (vector multiply, multiply-accumulate, add / compare / logic,
divide, vector load / store, scalar ALU / load / store, branch, COP0 and COP2 moves). Two sources, which can check each other:

- `costmodel.py` fits the JIT-proper time of each task of a device to the task's dynamic instruction counts by class (ridge-regularised non-negative least squares towards one cost per instruction,
  the regularisation by leave-one-scene-out error): `t_task = sum_class c_class * n_class + c_dma * dma bytes + c_task`. The counts of task n come from the `--prof3` run, its time from the `--prof` run:
  the emulation is deterministic, the RSP-state hash streams of the two runs are identical, and the walker counts of every joined task are compared.
- `perfmap.py` / `perfcost.py` take the cost of a class from the perf samples: the class's share of the samples over its share of the executions, scaled once to the measured JIT-proper time.

**Cross-check with perf.** On rgdsplus, `perf record -F 997` runs for 14 s over the same frames (`PERF=14`). `perfmap.py` maps each sample of the emulation thread through the region and instruction
offsets to a site, and from there to the routine; samples in a VU stub (a shared out-of-line body) are spread over the sites that execute that VU op, samples in the thunks over the branch sites, those
in the divide / DMA / MTC0 helpers over the sites of that kind, always in proportion to the executions per routine. Block lookup, the video plugin, the core, libc and the kernel are not routine time.

**The hooks in parallel-rsp** (`ext/parallel-rsp/prsp_hooks.h`; with `-DPRSP_HOOKS` a host installs a table of callbacks, all optional, and every call site in the
fork is a `PRSP_HOOK_*` macro that expands to nothing otherwise):

| hook | where | what the profiler does with it |
|---|---|---|
| `region` | `jit_region`, after `jit_emit()` | a marker node (`jit_note`) before each instruction gives its code offset (`jit_address` of the node after emission): the map of level 2 |
| `misc` | `init_vu_stubs`, `init_jit_thunks` | VU stub and thunk addresses and bytes (the code pages are execute-only once committed, so a copy is taken before) |
| `lookup` | `get_jit_block`, on a `blocks[]` miss | ticks in the lookup and in `jit_region` |
| `imem_dma` | `copy_to_imem` | DMA rows into IMEM, and how many of them changed a block |
| counters | `rsp_dma_read` / `rsp_dma_write`, `invalidate_code` | SP DMA operations and bytes, IMEM invalidations: counted inline in a struct the host reads (a hook call per DMA operation cost 1-3 % of the RSP time) |
| `site`, `hit` | `jit_instruction` (`PRSP_PROFILE` only) | the JIT asks for the counter index of an instruction when it compiles it; the counter call runs before the instruction |

The profiler counts what the ucode does by context (see Method) instead of building a static control-flow graph of it: a static graph of this code is incomplete exactly where it matters (the dispatch
is a register jump through a table in DMEM, the subroutines return through registers), whereas the observed edges, calls and returns are exact, and charging an instruction to the command being handled
needs no dominator analysis. The marker nodes do not change the code the JIT emits. Checked three ways: the normal module is byte-identical (md5) with and without the hooks in the tree,
for all three targets; the code images of the JIT (`PRSP_DUMP_JIT`) of the profiler build with and without markers have the same size and differ only in the
move-immediate words that hold absolute addresses (the same 595 words differ between two runs of the same build, ASLR); and the RSP state hash streams of the
profiler runs are identical to those of the normal module (see Testing).


## Results

All numbers are per VI frame. rgdsplus: ROCKNIX, RK3566 (4x Cortex-A55), performance governor (1992 MHz reported, about 1.58 GHz real), foir `devel` at 88d1722 with three
worker threads, the module and the profiler built with the `resources/toolchains/rg35xxsp` toolchain (which runs on both devices); `Core[RandomizeInterrupt] = False`, the deterministic
pad script, no save file. The profiler's own window is the difference of two screenshots' marks.

### Scenes

The pad script is keyed on controller polls and plays only the first minute of a game, so the scenes are menus, cutscenes and the first moments of play. Windows (VI frames):
Super Mario 64, Ocarina of Time, Paper Mario, Banjo-Kazooie, Donkey Kong 64: 1440-2160 (SM64 the intro cutscene with Lakitu, OoT the opening cutscene (the Deku Tree's speech, then Link's house), PM the Mario house exterior and interior, Banjo the title screen with the fly-by, DK64 the file select and the Kremling-bird intro); Mario Kart 64: 1440-2160 is
the game-select menu and the course intro ("menu"), so the race, frames 3000-3700 ("race"), is measured as well; Pokemon Snap: the attract demo with no input at all, 1100-1340,
the zoom shot at the camera (a close-up of the hand with the camera; the frame time there is 31 ms). Earlier numbers in this repository (the first profiling study) were taken
with another pad capture in other scenes and before fork commit e317ef6, so only some rows compare.

### JIT proper per game, against the first study

| game | ucode | tasks/frame | gfx RSP ms/frame | = rsp-hle overhead | + JIT proper | + RDP callback | + CheckInterrupts | profiler parse (overhead) | block lookup + compile (inside JIT proper) | audio HLE ms/frame | JIT proper per task (us) | earlier study: JIT proper / RDP cb | JIT proper change |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Pokemon Snap | F3DEX2 | 1.00 | 26.83 +- 0.03 | 0.00 | 12.85 +- 0.20 | 13.88 +- 0.21 | 0.00 | 0.09 | 0.59 | 0.65 | 12849 | 0.17 / 2.23 | +12.68 (+7458 %) |
| Banjo-Kazooie | F3DEX | 0.43 | 6.74 +- 0.03 | 0.00 | 4.52 +- 0.01 | 2.13 +- 0.03 | 0.00 | 0.09 | 0.07 | 0.36 | 10408 | 5.18 / 3.64 | -0.66 (-13 %) |
| Donkey Kong 64 | F3DEX2 | 0.43 | 6.43 +- 0.02 | 0.00 | 3.64 +- 0.02 | 2.75 +- 0.01 | 0.00 | 0.04 | 0.05 | 0.37 | 8400 | 4.51 / 5.46 | -0.87 (-19 %) |
| Mario Kart 64 (menu) | F3DEX | 0.28 | 1.15 +- 0.01 | 0.00 | 0.45 +- 0.00 | 0.67 +- 0.01 | 0.00 | 0.03 | 0.02 | 0.48 | 1601 | - | - |
| Mario Kart 64 (race) | F3DEX | 0.50 | 5.26 +- 0.04 | 0.00 | 3.36 +- 0.03 | 1.80 +- 0.01 | 0.00 | 0.10 | 0.03 | 0.47 | 6716 | 3.66 / 2.61 | -0.30 (-8 %) |
| Ocarina of Time | F3DEX2 | 0.33 | 2.56 +- 0.01 | 0.00 | 1.46 +- 0.01 | 1.08 +- 0.01 | 0.00 | 0.02 | 0.07 | 0.05 | 4381 | 2.07 / 1.89 | -0.61 (-29 %) |
| Paper Mario | F3DEX2 | 0.99 | 4.28 +- 0.01 | 0.00 | 2.59 +- 0.01 | 1.64 +- 0.01 | 0.00 | 0.04 | 0.03 | 0.49 | 2624 | 2.80 / 3.33 | -0.21 (-7 %) |
| Super Mario 64 | F3D | 0.50 | 4.11 +- 0.03 | 0.00 | 3.21 +- 0.02 | 0.86 +- 0.01 | 0.00 | 0.04 | 0.11 | 0.27 | 6420 | 3.44 / 2.37 | -0.23 (-7 %) |

"Earlier study" is the table of the first study (2026-10-09; Q1 of its README). Differences are scene and fork changes (`e317ef6`: IMEM invalidation skipped for unchanged DMA, inlined
divides): Banjo-Kazooie -13 %, Donkey Kong 64 -19 %, Super Mario 64 -7 %, Paper Mario -7 %, Mario Kart 64 race -8 %, Ocarina of Time -29 % (another scene: the earlier capture
was in the village, now it is a cutscene). The RDP callback column changes a lot because foir changed too (the callbacks of Super Mario 64 were 2.37 ms, now 0.86).
The rsp-hle overhead of a graphics task (the part of `DoRspCycles` outside `parallelRSPDoRspCycles`) is below 5 us per task and shows as 0.00. Block lookup and
compilation inside the JIT proper: 0.02-0.11 ms per frame in steady state (0.58 in the Snap window, where new code is still compiled); the rest of "JIT proper" is JIT
code, helpers and SP DMA.

### Profiler overhead

| game | n | RSP ms/frame normal build | RSP ms/frame profiler (level 1) | delta | delta % of RSP | emu-thread CPU ms/frame normal -> profiler | delta | parse share of the delta |
|---|---|---|---|---|---|---|---|---|
| Pokemon Snap | 3 + 3 | 27.414 +- 0.107 | 27.622 +- 0.030 | +0.208 | +0.8 % | 29.482 -> 29.447 | -0.035 | 44 % |
| Banjo-Kazooie | 3 + 3 | 6.976 +- 0.025 | 7.141 +- 0.029 | +0.165 | +2.4 % | 11.455 -> 11.619 | +0.164 | 53 % |
| Donkey Kong 64 | 3 + 3 | 6.732 +- 0.037 | 6.847 +- 0.020 | +0.115 | +1.7 % | 11.956 -> 12.003 | +0.047 | 32 % |
| Mario Kart 64 (menu) | 3 + 3 | 1.612 +- 0.015 | 1.741 +- 0.003 | +0.129 | +8.0 % | 5.442 -> 5.509 | +0.067 | 27 % |
| Mario Kart 64 (race) | 3 + 3 | 5.604 +- 0.074 | 5.849 +- 0.037 | +0.245 | +4.4 % | 8.403 -> 8.702 | +0.299 | 39 % |
| Ocarina of Time | 3 + 3 | 2.563 +- 0.010 | 2.654 +- 0.009 | +0.090 | +3.5 % | 4.915 -> 5.005 | +0.090 | 19 % |
| Paper Mario | 3 + 3 | 4.757 +- 0.021 | 4.965 +- 0.041 | +0.209 | +4.4 % | 8.011 -> 8.057 | +0.045 | 17 % |
| Super Mario 64 | 3 + 3 | 4.292 +- 0.012 | 4.442 +- 0.030 | +0.149 | +3.5 % | 6.966 -> 7.107 | +0.141 | 28 % |

Where it comes from (Banjo-Kazooie and Super Mario 64, the two games that submit the most RDP lists, two rounds each; the normal module, level 1 without the callback
wrappers, with the wrappers but no list parse, and the full level 1):

| game | normal module RSP ms/frame | level 1, callbacks not wrapped (task times, DMA / lookup hooks) | + callback wrappers, no list parse | + list parse (the full level 1) |
|---|---|---|---|---|
| Banjo-Kazooie | 6.958 +- 0.056 | 6.977 (+0.020, +0.3 %) | 7.098 (+0.140, +2.0 %) | 7.146 (+0.188, +2.7 %) |
| Super Mario 64 | 4.297 +- 0.004 | 4.375 (+0.077, +1.8 %) | 4.458 (+0.161, +3.7 %) | 4.473 (+0.175, +4.1 %) |


The target was 3 %. It is met by Pokemon Snap (0.8 %), Donkey Kong 64 (1.7 %) and Banjo-Kazooie (2.4 %), and missed by Ocarina of Time and Super Mario 64 (3.5 %), the Mario Kart 64 race and Paper Mario
(4.4 %) and the Mario Kart 64 menu (8 %, but 0.13 ms: the RSP time there is 1.6 ms). In absolute terms 0.09-0.25 ms per frame. The decomposition says where the remainder is: the callback wrappers (about 100-150 ns per call:
the count, the list parse, a sampled clock read), the parse (25-40 ns per call), and in Super Mario 64 1.8 % at the task level, probably the hooks of its 160 block lookups (two clock reads each) and 50 IMEM DMA rows (a 256-byte compare each) per frame; not investigated.
Counting SP DMA inline in the fork instead of through a hook per operation took 3 % off Banjo-Kazooie (a hook call cost 100 ns, 1900 times per frame).
`ARMLET_PROF_CB=0` removes the wrappers (task-level times only, the JIT proper then includes the callback), `ARMLET_PROF_PARSE=0` the parse. The RDP callback times of the tables are scaled from a sample (1 in 8
lists plus every list with a full sync) and agree with timing every call to 1.3 % in a desktop check; the per-task logs time every call.

### Where the JIT time goes: routines

Share of the graphics JIT time per routine (%), for the windows above. Three views of the same thing: the executed instructions counted per routine; those counts weighted with the
cost of an instruction of each class (the perf-calibrated costs, see below); and, in bold, the share of the perf samples of the emulation thread that fall into the routine
(a VU stub's samples are spread over the sites that execute that VU op, in proportion to their executions; the next section says how well the views agree).

| game | basis | vertex | triangle | matrix | dl | state | rdp | dispatch | dma | rdp-submit | task init |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Pokemon Snap | instruction counts | 28.4 | 51.1 | 4.1 | 1.2 | 1.7 | 2.9 | 8.0 | 2.5 | 0.0 | 0.0 |
| | cost-weighted (perf-calibrated class costs) | 32.1 | 50.4 | 3.7 | 0.6 | 0.8 | 1.4 | 5.5 | 5.3 | 0.1 | 0.0 |
| | **perf samples** | **32.2** | **50.5** | **4.2** | **1.1** | **1.3** | **1.7** | **3.7** | **5.3** | **0.1** | **0.1** |
| Banjo-Kazooie | instruction counts | 19.8 | 51.3 | 5.4 | 3.7 | 3.0 | 8.6 | 5.6 | 2.3 | 0.3 | 0.0 |
| | cost-weighted (perf-calibrated class costs) | 23.1 | 54.1 | 5.2 | 1.9 | 1.8 | 5.0 | 3.1 | 5.1 | 0.7 | 0.0 |
| | **perf samples** | **21.5** | **52.6** | **5.2** | **2.5** | **2.1** | **4.6** | **3.2** | **7.9** | **0.3** | **0.1** |
| Donkey Kong 64 | instruction counts | 39.3 | 43.9 | 1.7 | 1.0 | 1.3 | 2.3 | 7.9 | 2.5 | 0.1 | 0.0 |
| | cost-weighted (perf-calibrated class costs) | 42.9 | 43.7 | 1.1 | 0.5 | 0.6 | 1.0 | 5.1 | 4.9 | 0.1 | 0.0 |
| | **perf samples** | **43.1** | **42.4** | **1.6** | **0.7** | **1.0** | **1.6** | **3.5** | **5.9** | **0.1** | **0.2** |
| Mario Kart 64 (race) | instruction counts | 15.8 | 45.8 | 2.6 | 5.5 | 5.4 | 14.1 | 7.0 | 3.3 | 0.5 | 0.0 |
| | cost-weighted (perf-calibrated class costs) | 18.6 | 52.0 | 2.6 | 2.9 | 3.2 | 8.4 | 3.9 | 7.3 | 1.0 | 0.0 |
| | **perf samples** | **18.0** | **50.5** | **2.8** | **3.4** | **3.0** | **7.2** | **3.2** | **11.0** | **0.6** | **0.3** |
| Ocarina of Time | instruction counts | 29.2 | 53.1 | 1.0 | 1.1 | 1.5 | 3.2 | 8.2 | 2.5 | 0.1 | 0.0 |
| | cost-weighted (perf-calibrated class costs) | 32.0 | 54.2 | 0.6 | 0.5 | 0.7 | 1.4 | 5.4 | 5.0 | 0.2 | 0.0 |
| | **perf samples** | **32.7** | **51.7** | **1.0** | **1.1** | **1.8** | **1.8** | **4.4** | **5.3** | **0.1** | **0.2** |
| Paper Mario | instruction counts | 27.9 | 47.9 | 1.2 | 2.5 | 2.5 | 3.6 | 10.8 | 3.4 | 0.1 | 0.0 |
| | cost-weighted (perf-calibrated class costs) | 31.1 | 49.9 | 0.6 | 1.2 | 1.2 | 1.6 | 7.2 | 6.9 | 0.2 | 0.0 |
| | **perf samples** | **32.2** | **48.7** | **1.2** | **1.5** | **1.5** | **2.1** | **5.8** | **6.4** | **0.1** | **0.5** |
| Super Mario 64 | instruction counts | 30.1 | 44.3 | 4.8 | 2.6 | 0.5 | 6.1 | 9.7 | 1.7 | 0.2 | 0.0 |
| | cost-weighted (perf-calibrated class costs) | 35.7 | 43.8 | 4.9 | 1.3 | 0.3 | 3.5 | 6.1 | 4.0 | 0.4 | 0.0 |
| | **perf samples** | **36.0** | **42.8** | **5.0** | **1.4** | **0.4** | **3.0** | **5.1** | **6.0** | **0.2** | **0.1** |

Mario Kart 64 in its game-select menu has no perf run; by the cost-weighted counts (percent: vertex 8.3, triangle 23.3, matrix 2.5, dl 2.4, state 6.8, rdp 28.4, dispatch 9.2, dma 16.1) it is the exception, with little vertex and triangle work and much RDP command copying and dispatch.

Reading it: *vertex* is `G_VTX` and `G_MODIFYVTX` (load, transform, light, texgen, clip codes), *triangle* the triangle commands (cull, clip, edge, shade, texture and Z coefficients,
output), *matrix* `G_MTX`, `G_POPMTX`, `G_MOVEMEM`, `G_MOVEWORD`, *dl* `G_DL`, `G_ENDDL`, `G_CULLDL`, `G_BRANCH_Z`, *state* geometry mode, texture, other modes and the RDP halves, *rdp* the RDP commands
the ucode copies into its output buffer, *dispatch* the command fetch and the loop, *dma* the SP DMA triggers and the DMA-busy polls (the cost of the copy itself is in
the `dma` row only as far as it is in the helpers; libc's `memcpy` samples are not attributed, so the perf share of `dma` is a lower bound). `rdp-submit` is the writes to
`CMD_START` / `CMD_END` (the callback is not in the JIT time), `task init` the code before the first command of a task.
Nothing is left unattributed or unmapped in any window except as shown in the tables below.

Validation (the map must reproduce what the display-list walker counted):

| game | J1 (dispatch jump) executions | walker commands, all tasks | owner entries | clean tasks | handler check |
|---|---|---|---|---|---|
| Pokemon Snap (zoom shot) | 1813823 | 1968550 (-7.86 %) | 1813823 | no clean task in the window | table in DMEM (all tasks (the walker assumes no skipped commands)), 10 of 22 handlers: entries == walker count; off: 7d4 18580 vs 18720, 228 338609 vs 383210, 13c 80592 vs 89040 |
| Banjo-Kazooie | 1496138 | 1496138 (0.00 %) | 1496138 | 313 of 313 tasks clean: 1496138 == 1496138 | entry counts matched to walker opcode counts for 15 of 15 owners (>= 0.1 % of commands) |
| Donkey Kong 64 | 1584145 | 2356674 (-32.78 %) | 1584145 | 134 of 312 tasks clean: 531256 == 531256 | table in DMEM (clean tasks), 19 of 19 handlers: entries == walker count |
| Mario Kart 64 (menu) | 451233 | 451233 (0.00 %) | 451233 | 201 of 201 tasks clean: 451233 == 451233 | entry counts matched to walker opcode counts for 14 of 16 owners (>= 0.1 % of commands) |
| Mario Kart 64 (race) | 1342438 | 1342438 (0.00 %) | 1342438 | 350 of 350 tasks clean: 1342438 == 1342438 | entry counts matched to walker opcode counts for 14 of 16 owners (>= 0.1 % of commands) |
| Ocarina of Time | 624832 | 626512 (-0.27 %) | 624832 | no clean task in the window | table in DMEM (all tasks (the walker assumes no skipped commands)), 18 of 23 handlers: entries == walker count; off: fac 240 vs 480, 238 4269 vs 4509, 000 18909 vs 19149 |
| Paper Mario | 1693657 | 1693657 (0.00 %) | 1693657 | 712 of 712 tasks clean: 1693657 == 1693657 | table in DMEM (clean tasks), 20 of 20 handlers: entries == walker count |
| Super Mario 64 | 1185831 | 1185831 (0.00 %) | 1185831 | 360 of 360 tasks clean: 1185831 == 1185831 | entry counts matched to walker opcode counts for 13 of 15 owners (>= 0.1 % of commands) |

Handler detail, by instructions per entry (one entry is one display-list command; the percentages are the cost-weighted share of the JIT time):

**Donkey Kong 64**, window M0..M1, 224544037 instructions, ucode `48c7e653a6adcda8`

| owner | category | share of JIT time | instr | entries | instr per entry |
|---|---|---|---|---|---|
| VTX [4f0] | vertex | 43.6 % | 39.7 % | 104953 | 849 |
| TRI2/QUAD [7ac] | triangle | 34.8 % | 34.0 % | 252608 | 303 |
| TRI1 [7b4] | triangle | 10.6 % | 10.7 % | 238158 | 100 |
| dispatch (command fetch and loop, shared by every command) | dispatch | 5.1 % | 7.9 % |  |  |
| MTX [0f0] | matrix | 1.6 % | 1.8 % | 75880 | 54 |
| SETTILE/LOADBLOCK/SETTILESIZE/SETCOMBINE+5 [208] | rdp | 1.0 % | 1.3 % | 266119 | 11 |
| RDPPIPESYNC/RDPLOADSYNC/RDPTILESYNC/NOOP+1 [20c] | rdp | 0.9 % | 1.1 % | 248173 | 10 |
| SETOTHERMODE_H [13c] | state | 0.5 % | 0.9 % | 86139 | 24 |
| DL [000] | dl | 0.5 % | 0.7 % | 39901 | 37 |
| ENDDL [1e4] | dl | 0.4 % | 0.4 % | 33117 | 27 |

**Banjo-Kazooie**, window M0..M1, 295283385 instructions, ucode `208a567e9aead5ea`

| owner | category | share of JIT time | instr | entries | instr per entry |
|---|---|---|---|---|---|
| TRI2 [1ac.1e4] | triangle | 52.6 % | 49.4 % | 312458 | 467 |
| VTX [390.3a8] | vertex | 23.2 % | 19.8 % | 83862 | 698 |
| SETTILE/SETTIMG/RDPPIPESYNC/LOADBLOCK+14 [358] | rdp | 7.6 % | 9.8 % | 528128 | 55 |
| MTX [390.554] | matrix | 3.3 % | 3.3 % | 44306 | 221 |
| dispatch (command fetch and loop, shared by every command) | dispatch | 3.1 % | 5.6 % |  |  |
| TRI1 [1ac.1bc] | triangle | 2.5 % | 2.4 % | 23375 | 301 |
| POPMTX [1ac.204] | matrix | 2.2 % | 2.1 % | 34401 | 183 |
| DL [390.6e0] | dl | 1.5 % | 2.2 % | 98285 | 67 |
| ENDDL [1ac.2d8] | dl | 1.5 % | 1.9 % | 98598 | 57 |
| SETOTHERMODE_H [1ac.274] | state | 1.1 % | 1.5 % | 67506 | 64 |

**Super Mario 64**, window M0..M1, 220398368 instructions, ucode `94c7cfdd2c872523`

| owner | category | share of JIT time | instr | entries | instr per entry |
|---|---|---|---|---|---|
| TRI1 [1fc.20c] | triangle | 44.7 % | 44.7 % | 573486 | 172 |
| VTX [3c4.568] | vertex | 35.9 % | 30.2 % | 85515 | 778 |
| dispatch (command fetch and loop, shared by every command) | dispatch | 6.1 % | 9.7 % |  |  |
| SETTILE/RDPPIPESYNC/SETTIMG/RDPLOADSYNC+12 [38c] | rdp | 5.6 % | 7.0 % | 291291 | 53 |
| MTX [3c4.3dc] | matrix | 4.6 % | 4.2 % | 36155 | 258 |
| DL [3c4.734] | dl | 1.1 % | 1.6 % | 58715 | 61 |
| ENDDL [1fc.328] | dl | 1.1 % | 1.3 % | 58968 | 50 |
| MOVEMEM [3c4.558] | matrix | 0.4 % | 0.5 % | 31474 | 37 |
| SETOTHERMODE_L [1fc.2cc] | state | 0.1 % | 0.2 % | 5427 | 73 |
| POPMTX [1fc.250] | matrix | 0.1 % | 0.1 % | 1498 | 185 |

**Mario Kart 64 (race)**, window M0..M1, 210855508 instructions, ucode `34dad67335a54cda`

| owner | category | share of JIT time | instr | entries | instr per entry |
|---|---|---|---|---|---|
| TRI2 [1ec.224] | triangle | 49.0 % | 42.6 % | 119168 | 754 |
| VTX [418.430] | vertex | 18.8 % | 15.9 % | 64302 | 521 |
| SETTILE/RDPPIPESYNC/SETTIMG/RDPLOADSYNC+16 [3e0] | rdp | 12.7 % | 16.1 % | 633987 | 53 |
| TRI1 [1ec.1fc] | triangle | 4.1 % | 3.6 % | 29606 | 260 |
| dispatch (command fetch and loop, shared by every command) | dispatch | 3.9 % | 7.0 % |  |  |
| MTX [418.5dc] | matrix | 2.6 % | 2.5 % | 28916 | 184 |
| SETOTHERMODE_H [1ec.324] | state | 2.4 % | 3.3 % | 108139 | 64 |
| DL [418.768] | dl | 2.4 % | 3.4 % | 102165 | 71 |
| ENDDL [1ec.384] | dl | 2.1 % | 2.7 % | 102515 | 55 |
| SETOTHERMODE_L [1ec.32c] | state | 1.4 % | 1.8 % | 58753 | 66 |



### Cost model and the perf cross-check

**The regression does not separate the classes.** Fitting the time of each task to its dynamic instruction counts by class (`costmodel.py`, 2725 tasks of eight scenes,
ridge-regularised non-negative least squares towards one cost per instruction, the regularisation chosen by leave-one-scene-out error) gives, pooled, R2 0.98 and an rmse of 9 %
of the mean task, and per scene R2 0.97-1.00 in the scenes where the mix of work varies from task to task (Snap and Ocarina of Time have nearly identical tasks: no variation to fit). But the
classes are collinear (every task has the same mixture of vector multiplies, loads and scalar code, scaled by its number of vertices and triangles), so the individual class costs it
returns are not identified; the cross-validation prefers a model close to one cost for every instruction, about 11.4 ns:

```
== pooled over 8 games: 2725 tasks, R2 0.981, rmse 558 us (8.9 % of the mean task), total error +0.64 %
   regularisation 0.3 (leave-one-game-out error by lambda: 0: 801 us, 0.003: 701 us, 0.01: 668 us, 0.03: 638 us, 0.1: 617 us, 0.3: 614 us, 1: 618 us, 3: 620 us, 10: 620 us, 100: 618 us); prior 11.40 ns per instruction
   ns per instruction: vmul+vmac 11.65, vaddsub+vcmp+vlogic+vsar 11.94, vdiv 14.72, vload+vstore 12.09, salu+branch 10.95, sload+sstore 10.85, cop0+cop2mov 10.90, dma_bytes 0.00, task 0.00
   pooled model on snap  : predicted 12133 us vs measured 12833 us per task (-5.5 %)
   pooled model on banjo : predicted 10775 us vs measured 10467 us per task (+2.9 %)
   pooled model on dk64  : predicted 8344 us vs measured 8375 us per task (-0.4 %)
   pooled model on mk64  : predicted 1927 us vs measured 1646 us per task (+17.1 %)
   pooled model on mk64r : predicted 6859 us vs measured 7059 us per task (-2.8 %)
   pooled model on oot   : predicted 4076 us vs measured 4387 us per task (-7.1 %)
   pooled model on pm    : predicted 2801 us vs measured 2658 us per task (+5.4 %)
   pooled model on sm64  : predicted 6986 us vs measured 6495 us per task (+7.6 %)
```

(The pooled regression is off by +17 % in the Mario Kart 64 menu, +7.6 % in Super Mario 64, -7.1 % in Ocarina of Time: a mix of work it cannot tell from its average.)

**The perf samples do separate them.** The share of the samples of a class over its share of the executions is its cost relative to the average; pooled over the 14 perf runs, and scaled
so that the total is the measured JIT proper time:

```
relative cost of an executed instruction by class, pooled over 14 perf runs (1 = the average):
   vmac 2.38, cop0 2.11, vcmp 1.56, vmul 1.44, vaddsub 1.31, vdiv 1.27, sload 0.88, cop2mov 0.69, sstore 0.67, branch 0.60, vload 0.59, vlogic 0.56, vstore 0.48, vsar 0.39, salu 0.26
scaled to the measured JIT proper time: 11.40 ns per instruction on average
   ns per instruction: vmul 16.7, vmac 27.5, vaddsub 15.2, vcmp 18.0, vlogic 6.4, vdiv 14.6, vsar 4.5, vload 6.9, vstore 5.5, salu 3.0, sload 10.2, sstore 7.7, branch 7.0, cop0 24.4, cop2mov 8.0, other 11.6
   pooled over 2725 tasks: R2 0.979, rmse 588 us (9.4 % of the mean task)
   (the uniform cost per instruction: R2 0.979, rmse 577 us)
```

| game | tasks | measured JIT proper per task (us) | predicted, perf-calibrated class costs | error | predicted, one cost for every instruction | error |
|---|---|---|---|---|---|---|
| Pokemon Snap | 240 | 12833 | 12191 | -5.0 % | 12006 | -6.4 % |
| Banjo-Kazooie | 313 | 10467 | 10424 | -0.4 % | 10757 | +2.8 % |
| Donkey Kong 64 | 310 | 8375 | 8913 | +6.4 % | 8201 | -2.1 % |
| Mario Kart 64 (menu) | 200 | 1646 | 1597 | -3.0 % | 1979 | +20.2 % |
| Mario Kart 64 (race) | 350 | 7059 | 6608 | -6.4 % | 6869 | -2.7 % |
| Ocarina of Time | 240 | 4387 | 4340 | -1.1 % | 4010 | -8.6 % |
| Paper Mario | 712 | 2658 | 2910 | +9.5 % | 2766 | +4.1 % |
| Super Mario 64 | 360 | 6495 | 6496 | +0.0 % | 6981 | +7.5 % |

(An instruction of the multiply-accumulate family `VMADx` is a call into a 25-instruction stub, 2.4x the average; a scalar ALU instruction, which the JIT flushes to memory after every
instruction, is 0.26x. `cop0` is the `MTC0` / `MFC0` helpers.) These costs, determined from the samples alone, predict the time of every task of every scene, with no fitting to the task times
(only the one scale factor), to within 10 %, better than one cost for every instruction in the scenes whose mix differs most (Mario Kart 64 in the menu, Ocarina of Time, Super Mario 64).

**Agreement between the views.** The total variation distance between the routine shares of two views is half the sum of the absolute differences of the shares (0 = identical, 1 = disjoint):

| game | perf runs | emulation-thread samples | JIT code + stubs + helpers, share of the thread (perf) | JIT proper share of the thread (timing) | perf vs the regression (total variation distance) | perf vs plain instruction counts |
|---|---|---|---|---|---|---|
| Pokemon Snap | 2 | 27432 | 43.2 % | 43.6 % | 0.063 | 0.068 |
| Banjo-Kazooie | 2 | 21480 | 38.3 % | 39.5 % | 0.078 | 0.087 |
| Donkey Kong 64 | 2 | 19738 | 27.0 % | 30.4 % | 0.070 | 0.074 |
| Mario Kart 64 (race) | 2 | 9875 | 35.7 % | 40.0 % | 0.140 | 0.153 |
| Ocarina of Time | 2 | 8728 | 26.3 % | 29.7 % | 0.067 | 0.071 |
| Paper Mario | 2 | 14077 | 30.1 % | 32.4 % | 0.076 | 0.087 |
| Super Mario 64 | 2 | 11562 | 38.9 % | 46.1 % | 0.098 | 0.105 |

Perf puts 2-6 points more on vertex and 3-8 on DMA, and 2-5 points less on dispatch and 1-7 less on RDP command copying than the instruction counts do (vector code is dearer, scalar code
cheaper than average). It agrees with them on the ranking of the top routines in six scenes; in Donkey Kong 64 vertex and triangle swap (43.1 against 42.4 % by perf, 39.3 against 43.9 % by instruction counts). The largest
disagreement is the Mario Kart 64 race (0.14: its DMA is 11 % of the perf samples, 3 % of the instructions).
The JIT proper share of the emulation thread from perf (code, stubs, thunks and helpers) is 0.4-7 points below that from timing (largest in Super Mario 64); perf has no attribution for libc's `memcpy` of the SP DMA, which the timing includes.


### The RDP callback: cost per call

Banjo-Kazooie, Mario Kart 64 and Super Mario 64 submit hundreds of RDP lists per frame, mostly of one 8-byte command. Every call to `ProcessRdpList` enters the video plugin, which parses the
list, and for a triangle sets it up and queues it for its worker threads. Distribution of the call durations by kind of list (session C, level 1 with `ARMLET_PROF_CBHIST=1`, every call
timed; one timer tick is 41.7 ns, a duration bin is a factor of two):

| game | callbacks/frame | callback ms/frame | one non-triangle command (<= 8 B): calls/frame, median, long-call share | lists with a triangle | other lists |
|---|---|---|---|---|---|
| Banjo-Kazooie | 1210 | 2.11 | 898 / 333-667 ns / 32 % | 312 / 1333-2667 ns / 3 % | - |
| Donkey Kong 64 | 192 | 2.74 | 1 / 1333-2667 ns / 0 % | 183 / 2667-5333 ns / 49 % | 8 / 10667-21333 ns / 83 % |
| Ocarina of Time | 91 | 1.09 | 2 / 333-667 ns / 8 % | 82 / 2667-5333 ns / 7 % | 7 / 10667-21333 ns / 82 % |
| Paper Mario | 176 | 1.64 | 2 / 667-1333 ns / 3 % | 162 / 2667-5333 ns / 6 % | 12 / 10667-21333 ns / 78 % |
| Super Mario 64 | 573 | 0.87 | 421 / 333-667 ns / 35 % | 152 / 1333-2667 ns / 3 % | - |
| Mario Kart 64 (menu) | 526 | 0.66 | 510 / 167-333 ns / 27 % | 16 / 1333-2667 ns / 3 % | - |
| Mario Kart 64 (race) | 1400 | 1.83 | 1153 / 333-667 ns / 12 % | 247 / 1333-2667 ns / 24 % | - |
| Pokemon Snap | 392 | 13.86 | 2 / 1333-2667 ns / 100 % | 372 / 2667-5333 ns / 4 % | 18 / 10667-21333 ns / 98 % |

A call with one non-triangle command takes **0.33-0.67 us** in the median (0.17-0.33 in the Mario Kart 64 menu), which includes the profiler's two clock reads (about 40 ns): that is the
fixed cost of a call. A list with a triangle takes 1.3-2.7 us when it comes alone, 2.7-5.3 us in the games whose lists hold more than one triangle. About a third of the time of the
tiny calls in Banjo-Kazooie and Super Mario 64 (and nearly all of it in Snap, where it is 11 ms per frame) is a few calls per frame that take more than 43 us: the `G_RDPFULLSYNC` that waits for the worker threads.

What coalescing could save (ms per frame; the calls per frame are from the table above, the fixed cost 0.29-0.63 us (the median of a one-command call, 0.33-0.67 us, less the 40 ns of the profiler's clock reads), and every call that carries less than 1 KiB of list is merged into its successor
until 1 KiB or the end of the task; the per-triangle work stays):

| game | calls/frame now | coalesced | saving ms/frame (fixed cost 0.29-0.63 us per call) | share of the callback time | share of the emulation thread |
|---|---|---|---|---|---|
| Banjo-Kazooie | 1210 | 56 | 0.33-0.73 | 16-34 % | 2.9-6.3 % |
| Super Mario 64 | 573 | 26 | 0.16-0.34 | 18-40 % | 2.3-4.9 % |
| Donkey Kong 64 | 192 | 69 | 0.04-0.08 | 1-3 % | 0.3-0.6 % |
| Paper Mario | 176 | 68 | 0.03-0.07 | 2-4 % | 0.4-0.9 % |
| Ocarina of Time | 91 | 34 | 0.02-0.04 | 2-3 % | 0.3-0.7 % |
| Mario Kart 64 (race) | 1400 | 51 | 0.39-0.85 | 21-47 % | 4.7-10.1 % |
| Pokemon Snap | 392 | 142 | 0.07-0.16 | 1-1 % | 0.2-0.5 % |
| Mario Kart 64 (menu) | 526 | 7 | 0.15-0.33 | 23-50 % | 2.8-6.0 % |

That is a cheap win in exactly the three games named in the plan: 0.2-0.9 ms per frame (2-10 % of the emulation thread, 16-47 % of the callback time) for a few lines in front of the callback, with no
microcode knowledge: delay `ProcessRdpList` until the list reaches 1 KiB, the task ends, or the ucode reads `DPC_CURRENT` / `DPC_STATUS`, the FIFO is nearly full, or it raises a sync. It has
to keep the RDP's view of memory right (a texture load must reach the plugin before the triangle that uses it, which delaying within one task does).

**Implemented since** ([design.md](design.md#rdp-list-coalescing), results in [testing.md](testing.md#rdp-list-coalescing)). The polling worry does not arise: foir's call is synchronous and leaves `DPC_START = DPC_CURRENT = DPC_END`,
so arMLEt writes those registers itself when it defers a list and the ucode reads what it always read. The saving measured on the RK3566 is 0.4-0.6 ms of RSP time per frame in Banjo-Kazooie, Mario Kart 64 (race) and Super Mario 64 (0.25), at a threshold of 4 KiB; the model above
(0.2-0.9 ms) was right for the RK3566 and about twice too high for the H700.


### The emulation thread in the Pokemon Snap zoom shot

Attract demo, no input, VI frames 1100-1340, the zoom at the camera. Three runs of the normal module and three of the profiler. The thread's CPU time is the shim's
`CLOCK_THREAD_CPUTIME_ID` at the two marks; the split comes from the profiler's per-task-type times.

| item | ms/frame | share of the emulation thread |
|---|---|---|
| emulation thread CPU (shim, normal module) | 29.48 | 100 % |
| of which in DoRspCycles (shim) | 27.41 | 93 % |
| - graphics tasks: JIT proper | 12.85 | 44 % |
| - graphics tasks: the video plugin's RDP callback (foir: list parse, setup, waiting) | 13.88 | 47 % |
| - graphics tasks: rsp-hle overhead + CheckInterrupts | 0.00 | 0 % |
| - audio HLE (type 2 tasks) | 0.65 | 2 % |
| - other tasks and non-tasks | 0.00 | 0 % |
| the rest of the emulation thread (core: CPU, devices, VI, and the profiler-free shim) | 2.07 | 7 % |

The emulation thread has 15.6 ms of work that is not the video plugin's callback: 12.9 of it (82 %) is the graphics JIT, 0.65 audio HLE (rsp-hle's own code, already HLE), 2.0 the rest of the core
(the CPU interpreter / dynarec, the devices, the VI). A perf run over the same frames, whole thread, agrees: 44 % of its samples in the JIT (regions 24 %, VU stubs 16 %, thunks, helpers),
38 % in foir, 6.5 % in the core's dynarec, 3.5 % libc, about 1.5 % audio HLE.

**What arMLEt can remove** is the 12.9 ms, down to about 0.7 ms (0.4-1.2) for the whole display list native, 3.0 ms (2.7-3.7) with only vertex and triangle native; the 29.5 ms thread becomes
about 17-20 ms. **What that does to the frame time is bounded by the video plugin, not by the RSP:** foir's own log of the window says the callback is 14.4 ms, of which 11.4 ms is *waiting at
SYNC_FULL* for its three pixel worker threads, which use 78 ms of CPU per frame (26 ms per worker on a four-core device that also runs the emulation thread); the frame takes 31 ms. The emulation thread's
busy time without the wait is 18 ms (29.5 - 11.4). With a native RSP the lists reach the workers earlier, the wait grows by what the JIT used to take, and the frame time falls towards the workers' 26 ms, not towards 18.
So the likely gain in this scene is a few milliseconds per frame (31 to somewhere around 26-28), not twelve. That estimate is not a measurement (no native code exists). It applies to this heavy shot;
in the other scenes the callback is short (0.9-2.7 ms) and the emulation thread is the bottleneck by a wide margin, so there the full JIT saving counts.


### Spot check on the rg35xxsp (H700, 4x Cortex-A53 at 1512 MHz)

The same builds (they run on both devices), pinned clocks (`cpu_perf.sh on`), the same windows, three alternating runs each:

| game | ucode | tasks/frame | gfx RSP ms/frame | = rsp-hle overhead | + JIT proper | + RDP callback | + CheckInterrupts | profiler parse (overhead) | block lookup + compile (inside JIT proper) | audio HLE ms/frame | JIT proper per task (us) | earlier study: JIT proper / RDP cb | JIT proper change |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Super Mario 64 | F3D | 0.50 | 4.44 +- 0.02 | 0.00 | 3.28 +- 0.01 | 1.13 +- 0.01 | 0.00 | 0.04 | 0.11 | 0.32 | 6550 | 3.44 / 2.37 | -0.16 (-5 %) |
| Donkey Kong 64 | F3DEX2 | 0.43 | 6.76 +- 0.06 | 0.00 | 3.68 +- 0.01 | 3.05 +- 0.05 | 0.00 | 0.03 | 0.05 | 0.39 | 8492 | 4.51 / 5.46 | -0.83 (-18 %) |

| game | n | RSP ms/frame normal build | RSP ms/frame profiler (level 1) | delta | delta % of RSP | emu-thread CPU ms/frame normal -> profiler | delta | parse share of the delta |
|---|---|---|---|---|---|---|---|---|
| Super Mario 64 | 3 + 3 | 4.693 +- 0.028 | 4.904 +- 0.017 | +0.211 | +4.5 % | 7.801 -> 7.924 | +0.124 | 17 % |
| Donkey Kong 64 | 3 + 3 | 7.082 +- 0.043 | 7.238 +- 0.057 | +0.156 | +2.2 % | 12.911 -> 13.028 | +0.117 | 18 % |

The JIT proper of an in-order A53 at 1512 MHz is the same as the A55's (Super Mario 64 3.28 against 3.21 ms, Donkey Kong 64 3.68 against 3.64); the instruction counts, the walker counts and the routine shares are
the same (the ucode does the same work), and so are the RSP state hash streams of the profiler runs. The video plugin's callback is 0.2-0.3 ms slower. The profiler's overhead is 4.5 / 2.2 % there.
A fit run (session F) of Super Mario 64 on the A53 gives 6610 us per task against 6495 on the A55 and 11.6 ns per instruction against 11.4. No perf on that device; no other scene.


### What native code would save, and the decision

A model, not a measurement: nothing native exists. Per game it takes the measured JIT proper, its routine shares (the perf samples), the display-list workload per frame from the
walker (vertices, triangles submitted and sent to the RDP, matrices, RDP commands, other commands) and an assumed cost in cycles per event for native NEON code (`hlemodel.py`; 1.58 GHz).
Cycles per event, low / mid / high: vertex with transform, lighting and texgen 70 / 110 / 180; triangle submitted (fetch, cull) 25 / 40 / 70; triangle sent to the RDP (clip, edge setup,
shade, texture and Z coefficients, output) 250 / 400 / 700; matrix command 150 / 300 / 600; RDP command copied 8 / 12 / 25; state command 8 / 12 / 25; display-list command 10 / 15 / 30; dispatch
6 per command (4 / 12); 10 k per task (5 k / 30 k); and 80 / 150 / 300 ns hook overhead for every call of a native routine that the JIT code around it enters.

- **top-N routines**: the N handlers with the largest net saving (JIT share minus native cost minus hooks) are intercepted by PC (`get_jit_block` keyed on region hash and pc, a separate plan);
  the rest of the ucode, the dispatch loop and the DMA stay on the JIT.
- **whole DL**: the task runs natively (`armlet_gfx_try_native`), nothing of it on the JIT.

Milliseconds per frame **saved** on the emulation thread (mid parameters; low-cost / high-cost parameters in brackets):

(routine shares: perf samples)

| game | JIT proper now | RDP cb | share: vertex / triangle / matrix / rdp / state / dl / dispatch | native whole DL (low / mid / high) | saving top-1 (routines) | top-2 | top-3 | whole DL |
|---|---|---|---|---|---|---|---|---|
| Pokemon Snap | 12.85 | 13.88 | 32% / 50% / 4% / 2% / 1% / 1% / 4% | 0.43 / 0.69 / 1.23 | 6.06 (5.69..6.24) | 9.81 (9.17..10.13) | 10.21 (9.43..10.60) | 12.16 (11.62..12.42) |
| Banjo-Kazooie | 4.52 | 2.13 | 21% / 53% / 5% / 5% / 2% / 2% / 3% | 0.14 / 0.23 / 0.41 | 2.21 (2.06..2.28) | 3.08 (2.86..3.19) | 3.28 (3.02..3.40) | 4.30 (4.12..4.38) |
| Donkey Kong 64 | 3.64 | 2.75 | 43% / 42% / 2% / 2% / 1% / 1% / 4% | 0.21 / 0.34 / 0.59 | 1.39 (1.27..1.46) | 2.63 (2.24..2.83) | 2.65 (2.24..2.86) | 3.30 (3.05..3.43) |
| Mario Kart 64 (race) | 3.36 | 1.80 | 18% / 51% / 3% / 7% / 3% / 3% / 3% | 0.10 / 0.16 / 0.28 | 1.59 (1.51..1.63) | 2.13 (1.99..2.20) | 2.23 (2.06..2.36) | 3.20 (3.08..3.26) |
| Ocarina of Time | 1.46 | 1.08 | 33% / 52% / 1% / 2% / 2% / 1% / 4% | 0.07 / 0.11 / 0.19 | 0.67 (0.60..0.70) | 1.11 (1.02..1.16) | 1.12 (1.02..1.17) | 1.35 (1.27..1.39) |
| Paper Mario | 2.59 | 1.64 | 32% / 49% / 1% / 2% / 2% / 1% / 6% | 0.14 / 0.23 / 0.42 | 1.10 (0.97..1.17) | 1.85 (1.65..1.95) | 1.85 (1.65..1.96) | 2.36 (2.18..2.45) |
| Super Mario 64 | 3.21 | 0.86 | 36% / 43% / 5% / 3% / 0% / 1% / 5% | 0.11 / 0.18 / 0.32 | 1.20 (1.03..1.27) | 2.25 (2.01..2.36) | 2.37 (2.10..2.51) | 3.03 (2.90..3.10) |


Native cost of the whole display list (mid): 0.1-0.7 ms per frame, 11-21x faster than the JIT proper; even with every parameter doubled it stays below 1.3 ms, so the ranking of the strategies
does not depend on them.

**Reading it.**

- Two native routines per ucode family, vertex and triangle, remove 63-76 % of the JIT proper (1.1-3.1 ms per frame, 9.8 ms in the Snap zoom shot) in every scene measured
  except the Mario Kart 64 menu; the whole display list native removes 91-95 %. The two routines are **67-82 % of what the whole-DL strategy saves**; the rest (dispatch, the DMA triggers, matrices, RDP
  command copying, state and display-list commands) adds 0.2-2.4 ms, 5-13 % of the emulation thread.
- The third routine is worth a small amount (matrices in Banjo-Kazooie and Super Mario 64, RDP command copying in the Mario Kart 64 race) or nothing: below about 5 % of the JIT time a routine does not
  pay for its hook calls.
- Triangle setup is the largest single routine in six of the seven scenes (42-53 %; vertex transform is 43 % against 42 % in Donkey Kong 64) and is the one to do first; vertex transform is 18-43 %. F3DEX2 (Donkey Kong 64, Ocarina of Time, Paper Mario, Snap) shares one table of
  handlers, F3DEX 1.x (Banjo-Kazooie, Mario Kart 64) and F3D (Super Mario 64) dispatch in two levels; each family needs its own two routines (the vertex and triangle code differs), the shape of the work does not.
- Mario Kart 64 is the one ucode where something else counts: its F3DEX 0.95 copies every RDP command through a loop of its own (14 % of the instructions, 7 % of the time in the race) and the DMA share is 11 %.

**Recommendation (confidence: high on where the time goes, medium on what native code would save, because of the assumed native cost).**

1. Do the cheap thing first: coalesce the `ProcessRdpList` calls (0.2-0.9 ms per frame in Super Mario 64, Banjo-Kazooie and the Mario Kart 64 race; section above). It needs no microcode knowledge.
2. **HLE a few hot routines, not the whole display list, as the first native step**: triangle setup and vertex transform per ucode family, through the PC-entry hook. They are about three quarters of the JIT time
   for two routines per family, and a routine boundary gives something the whole-DL route cannot have: the JIT is still there to be the oracle. Run both on the same call, compare DMEM and the registers
   (the way the fork's shadow build compares whole tasks), and every native routine is verified call by call, not frame by frame through the RDP output. The cost is the contract: the native routine must leave DMEM (the
   vertex buffer, clip codes, the output buffer pointer) exactly as the ucode would, because JIT code continues from it.
3. Grow that mechanism toward "as much as possible" routine by routine only where the table says it pays (matrices, then RDP command copying, then the small handlers, then dispatch) and decide the whole-DL
   rewrite after the first two routines exist and their measured native cost replaces the model's. A whole-DL native path buys the last 5-13 % of the emulation thread, needs every command of the family implemented
   and can only be checked through the RDP output and the task-end state.
4. In scenes like the Snap zoom shot the frame time will not follow the emulation thread (see above): the video plugin's workers are the next limit.


## What was not measured

- Native code: nothing native exists. The native cost parameters are cycle counts per event, assumed (the earlier study's, re-based on NEON vertex code), not measured; the table brackets them
  with low and high sets (2x apart). The hook overhead of a PC-entry interception (80-300 ns per native call) is an assumption too.
- Scenes: the pad script plays about a minute of each game, so menus, cutscenes and the first moments of play; the heaviest 3D scenes of the games are not in the windows. The routine shares are
  stable across scenes (the table), the milliseconds are not. Ocarina of Time, Super Mario 64 and Snap have no or few "clean" tasks at the validation level (see the validation table).
- Games: eight windows of seven games on one device (the spot check on the second has two); Star Fox 64 and Toy Story 2 of the earlier study, and the 39 unseen ROMs, are not profiled. F3DLX, F3DEX2 variants with
  other opcodes (`.Rej`, `.NoN` besides the one measured) and customised ucodes such as Conker's are not covered.
- The video plugin's side of the Snap zoom shot (whether the frame rate rises when the RSP gets faster) is not measured; only the emulation thread is.
- The perf shares exclude libc's `memcpy` (the SP DMA copies) and under-count `dma`; perf on the rg35xxsp was not tried.
- The walker is exact only for tasks without `G_CULLDL` / `G_BRANCH_Z`; in Pokemon Snap and Ocarina of Time no task of the window is clean, and in Donkey Kong 64 134 of 312, so there the handler check
  is on the clean tasks or against an upper bound (the dispatch count is still exact: the profiler finds the dispatch jump with clean tasks and counts it in all).
- The coalescing of `ProcessRdpList` calls is estimated from the call-duration distribution, not tried.
- The task times of the fit runs (the regression of the cost model) and the perf runs come from profiler builds that counted SP DMA through a hook call per operation, which cost up to 3 % of the RSP time in
  Banjo-Kazooie (the timing tables come from the final build); the perf-calibrated costs are scaled to the JIT proper of those runs.
- What the profiler perturbs: its own overhead (the table above) and, at level 3, the speed (4-10x slower); the RSP state is not perturbed (hash streams).

