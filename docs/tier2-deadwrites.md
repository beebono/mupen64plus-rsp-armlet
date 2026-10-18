# Dead VU work in graphics microcode: is a register-allocating JIT tier worth it?

A ucode-agnostic second JIT tier for graphics tasks would keep the RSP's vector state (the 32 vector registers, the three accumulator slices, VCO / VCC / VCE)
in NEON registers across a region and skip the outputs nobody reads, instead of compiling each VU op on its own as the fork does now
(`ldr q0, ldr q1, shuffle, body, str q0`, with the body loading and storing the accumulator and flags itself; ext/parallel-rsp's README). This page measures,
before anything is built, how much such a tier could remove: **what fraction of the VU outputs is dead, and how much of the VU state traffic and the VU
instructions disappear with in-register state.** It is static analysis of microcode text; nothing in the plugin or the fork is changed.

Short answer: in the vertex and triangle code of the F3DEX2 family about **a third of all VU outputs are dead**, and keeping VU state in registers **within a
basic block** removes **75-80 % of the VU state loads and stores** and **about 30 % of the instructions the JIT emits for VU ops**, before any dead output is
dropped. Looking across blocks adds little (1-3 points of dead outputs in the vertex and triangle code). That points to a block-local allocator as the
first step, and to a gain smaller than the 2-4x guessed before this measurement: on the order of 1.3-1.6x on the VU-heavy code from instruction count
alone, more only if the store-to-load forwarding on the accumulator chains (removed with the traffic) costs more than its instruction count, which only a device
run can show.

## Tool

`tools/native/deadwrites.py` (with a `--selftest`) takes a 4 KiB IMEM image (`tools/ucode_dump_shim.c`'s `.imem`, or the profiler's `ucode/<id>.imem`),
and `--patch` puts another overlay in place from a dump's `.ext`. Its docstring has the full definitions; in short:

- **Semantics** are the fork's (`ext/parallel-rsp/rsp/vfunctions.cpp`, which the NEON bodies are checked against): VADD / VSUB / logic / select / compares /
  VCH / VCL / VCR / VMOV / VRCP* write ACC_LO only; the multiplies write all three slices and the multiply-accumulates also read them; VSAR reads one slice and
  writes no accumulator; VLT / VEQ / VNE / VGE read VCO, write VCC, clear VCO and leave VCE alone; VMRG clears VCO. A partial write (vector loads, MTC2, VMOV,
  VRCP*) is a read and a write.
- **Two region models.** `bb`: everything is live at the end of every basic block (a tier that analyses nothing across blocks; the lower bound). `global`: liveness
  along the static control flow, with every register jump (the command dispatch, returns), `break` and every COP0 access (the SP_STATUS poll time-out can stop
  the JIT there; DMA can change IMEM) an exit at which all state is live. That is sound for any region a tier could compile between those exits (the upper
  bound for a tier that does not look through register jumps), and it needs no register-jump targets, unlike `liveness.py`'s claims about routine exits.
- **Overlays.** `--window LO-HI` makes a jump into an overlay window from outside it an exit, so code outside the windows can be analysed in any image and each
  overlay in the image that holds it. (F3DEX2's vertex loop branches straight into the lighting overlay; without the window that edge would be analysed against
  the clipping overlay's code.)
- **Traffic.** Per block, `now` counts one load per vector input and one store per output (less the reload the fork already skips when vs / vt is the previous
  op's vd); `alloc` counts one load per item read before it is written in the block and one store per item written (`alloc-live`: only those live at the end).
- `--counts PROFDIR/counts.LABEL:KEY` weights each instruction by a level-3 profiler run (`sites` + `C` lines); `--list LO-HI` prints every instruction with its
  dead outputs, which is how the results below were checked by hand.

## Data

The repository holds no microcode (the derivation keeps only shape hashes, and the dumps are ROM-derived), and no profiler run was available, so the numbers here
come from the public reassemblable disassembly [Mr-Wiseguy/f3dex2](https://github.com/Mr-Wiseguy/f3dex2) (commit `bd31393`), assembled with
[armips](https://github.com/Kingcom/armips) (`62adab4`): all 20 builds it knows (F3DEX2 2.04 to 2.08H in fifo / NoN / xbus forms, 2.08 with point lighting,
F3DZEX 2.06H / 2.08I / 2.08J), every one matching its retail MD5 for code and data. `tools/native/deadwrites_f3dex2.py` makes the steady-state IMEM
images (main code at 0x080, overlay 1 at 0x000, the clipping or lighting overlay at 0x2d8) and names the ranges from the assembler's symbols. Counts are
**static** (each instruction once); the profiling page's per-routine split (vertex 18-43 %, triangle 42-53 % of the JIT time in the scenes it measured,
[profiling.md](profiling.md)) is what says the `vtx` and `tri` rows are the ones that matter.

The builds differ very little: the rows of 2.08 (Banjo-Tooie) stand for all of them except the lighting overlay, which is larger in the point-lighting and
F3DZEX builds (167 VU ops against 75).

## Results

F3DEX2 2.08, static. "dead out": dead VU outputs (vd, slices, flags, divide unit) over all outputs; "vd / acc / flags": dead share of those outputs alone (global);
"clamp": multiply / multiply-accumulate ops whose vd is dead while the accumulator is live (the clamp to vd can go); "acc-o": the reverse (the slice computation
can go); "traffic": allocated VU state loads + stores as a share of now, per block; "run": mean VU ops per block that has any; "pmax" / "<=20": distinct VU items
a block touches, the most and the share of blocks with at most 20 (the host registers an allocation needs); "instr": the instruction estimate below.

| range | VU ops | dead out bb | dead out global | vd | acc | flags | clamp | acc-o | traffic | run | pmax | <= 20 | instr now -> alloc |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| vtx (G_VTX) | 64 | 32.2 % | 33.5 % | 32.8 % | 35.5 % | 27.3 % | 37.8 % | 21.6 % | 19.7 % | 9.1 | 30 | 86 % | 1163 -> 814 (-30 %) |
| tri (G_TRI1 / TRI2 / QUAD) | 147 | 32.4 % | 35.3 % | 25.2 % | 39.7 % | 33.3 % | 42.7 % | 24.4 % | 25.3 % | 12.2 | 34 | 75 % | 2212 -> 1545 (-30 %) |
| clipping overlay | 60 | 34.4 % | 37.2 % | 21.7 % | 43.9 % | 38.9 % | 33.3 % | 30.6 % | 16.5 % | 20.0 | 25 | 33 % | 997 -> 674 (-32 %) |
| lighting overlay | 75 | 28.5 % | 41.4 % | 37.3 % | 44.0 % | 38.9 % | 60.5 % | 30.2 % | 36.0 % | 5.4 | 18 | 100 % | 1322 -> 1037 (-22 %) |
| overlay 1 (command handlers) | 7 | 14.3 % | 17.9 % | 28.6 % | 14.3 % | - | 28.6 % | 14.3 % | 40.3 % | 3.5 | 10 | 100 % | 169 -> 135 (-20 %) |

Over all 20 builds: vtx 33.5 %, tri 35.3 %, clipping 37.2 %, lighting 41.4 % of the outputs dead (global); the spread between builds is under 0.2 points
except in the lighting overlay (bb 28.5-34.8 %). No VU instruction is dead as a whole (every op has at least one live output): the dead work is inside
the ops, not whole ops.

**What the dead outputs are.** The pattern behind the numbers, seen in the listing: a matrix row is `vmudn / vmadh / vmadn / vmadh ...` into a scratch register
(`v29` in F3DEX2) whose clamped value is never read, only the accumulator (that is the "clamp" column, 38-43 % of the multiplies in vertex and triangle code
and 60 % in lighting); the last op of a chain, or a `vmadn vX, v0, v0[0]` that reads the accumulator out, leaves slices that the next `vmud*` overwrites
("acc-o", 22-31 %); nearly every VADD / logic / compare writes an ACC_LO that nobody reads, and VCO / VCC writes that the next compare overwrites.

**Traffic and instructions.** The current code generation does 7-8 VU state loads or stores per VU op in these routines (a multiply-accumulate loads vs, vt
and three slices and stores vd and three slices). With the state in registers inside a block, 75-80 % of them go in vertex and triangle code and 83-85 % in
the clipping overlay. In instructions (`--bodies`: the fork's bodies built with its own flags by aarch64-linux-gnu-g++ 13 at `-O3`, e.g. VMADN 23 instructions of which 4
load or store accumulator slices, VMADH 14, VCL 30, VAND 2, all without the RET; plus the JIT's loads of vs / vt, the element shuffle, the BL / RET of a body over 8 instructions, and the
store of vd) that is **30 % fewer instructions for the VU ops of the vertex and triangle code** (32 % clipping, 22-26 % lighting), counting only the traffic
and the call overhead: dropping the dead clamps and slice computations comes on top (their instruction share is not split out here).

**Across blocks.** The global model finds only 1.3 to 2.9 points more dead outputs than the block model in vertex, triangle and clipping code; the lighting overlay is the
exception (28.5 to 41.4 %: its blocks are short, 5.4 VU ops). Vertex and triangle blocks are long (9-12 VU ops on average; 80 % of the VU ops are in blocks of
8 or more), so a block-local allocator gets nearly everything there is.

**Register pressure.** A quarter of the triangle blocks touch more than 20 distinct VU items and the largest 34-35, against 32 NEON registers of which the op
bodies use some as scratch: an allocator needs spilling, not a fixed map.

## What this says about the tier

- Worth building as a **block-local** allocator first: VU state in NEON registers within a basic block, loads of what the block reads first, stores of what is live
  at its end, bodies always inlined, then dead-output elimination per op (clamp, slices, flags) from the liveness inside the block. Superblocks buy little for
  vertex and triangle code.
- The expected gain is **modest**: about 30 % of the VU instructions from the traffic alone, perhaps 35-45 % with dead outputs dropped, in code where VU ops are most
  but not all of the time. That is roughly 1.3-1.6x on the vertex and triangle JIT time, well below what the task-level front-ends get (11-21x) and below the 2-4x
  guessed before this measurement. The open question is the store-to-load forwarding: every multiply-accumulate in a chain loads the slices the previous op has just
  stored, on the critical path of an in-order A55, and the profiler's 27.5 ns per multiply-accumulate on the RK3566 ([profiling.md](profiling.md): about 43 cycles at its real 1.58 GHz for about 30 instructions,
  measured before some of the fork's later changes) suggests stalls beyond
  the instruction count. That, not this page, decides whether the tier is worth it.
- The cheapest way to find out: hand-allocate one hot block (a vertex transform chain), run it through `armlet-native-replay --bench` against the JIT's code for the
  same stretch on the device, and compare with the 30 % instruction estimate. If the time drops much more than the instruction count, forwarding stalls dominate and the
  tier is worth more than this page estimates.

## Limits

- **One family, static.** F3DEX2 only (the family with a public reassemblable disassembly); F3D, F3DEX 1.x, S2DEX and the custom ucodes left on the JIT (the hot list of
  [ucode-coverage.md](ucode-coverage.md)) are not measured. Static counts weight every instruction once; `--counts` would weight them by a real run.
- **Conservative in places.** Vector loads are partial writes, which keeps earlier writes alive; every COP0 access and register jump is a full exit; jumps into overlay
  windows are exits. The dead shares are lower bounds for each model.
- **The instruction estimate is first order.** It counts instructions, not cycles; it leaves out VMOV / VSAR / VRCP* (inline JIT sequences) and the scalar code around the VU
  ops; it assumes every item in a block gets a register (the pressure column says some blocks will spill).
- **Side finding:** `tools/native/liveness.py` lets VLT / VEQ / VNE / VGE define VCE, which the fork's semantics do not, so a compare between a VCE write and a VCL could
  make it report VCE dead at an exit where it is live. Its masks are only kept after the POISON runs (design.md), so nothing shipped depends on it, but the decode should be
  fixed before it is used for new masks.

## Reproduce

```
git clone https://github.com/Kingcom/armips && cmake -S armips -B armips/build -DCMAKE_BUILD_TYPE=Release && cmake --build armips/build
git clone https://github.com/Mr-Wiseguy/f3dex2 && make -C f3dex2 ok ARMIPS=$PWD/armips/build/armips BUILD_DIR=$PWD/f3dex2-build
# the fork's VU bodies as assembly, with the flags of ext/parallel-rsp/CMakeLists.txt (-ffixed-x9 .. -ffixed-x28 as listed there)
(cd ext/parallel-rsp && aarch64-linux-gnu-g++ -O3 -std=c++14 -DPARALLEL_INTEGRATION $(sed -n 56p CMakeLists.txt | grep -o '\-ffixed-x[0-9]*') \
    -I. -Iarch/simd/rsp -Irsp -S rsp/vu_inline.cpp -o ../../vu_inline.s)
python3 -I tools/native/deadwrites.py --selftest
python3 -I tools/native/deadwrites_f3dex2.py f3dex2-build --bodies vu_inline.s          # the tables above, a few seconds
# on a dump of any ucode (ROM-derived: keep it out of git), statically or weighted by a level-3 profiler run:
python3 -I tools/native/deadwrites.py DUMPS/<id>.<datahash>.imem --ranges vtx=0x4f8-0x7b4,tri=0x7b4-0xc40 [--counts PROFDIR/counts.M3:KEY]
```
