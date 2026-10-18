# Deriving a build record from a microcode text (offline)

The task-level path claims a graphics ucode by its exact id, and a build is a hand-written table (`struct f3dex2_build` + `f2_layout` + `f2_pcs` + the extension table, `f3dex_build`, `f3d_build`; [design.md](design.md#task-level-native-path)).
[ucode-survey.md](ucode-survey.md) showed that most unregistered texts are a registered build with a few dozen relocated pcs and a handful of constants; [ucode-coverage.md](ucode-coverage.md) says which of them cost the
most RSP time. `tools/derive_build.py` does what the person did by hand for the dialects ([ucode-f3dex2-dialects.md](ucode-f3dex2-dialects.md)): it aligns an unknown text with the registered builds, **derives the record**, and
lists **every difference it cannot explain**. It is pure offline analysis of the dumps that `tools/ucode_dump_shim.c` writes; nothing in `src/hle` is changed or registered by it. The four candidates that this page ended with
were registered by hand from its records afterwards ([Registered](#registered-2026-10-16): three new builds, one alias, and what registering them found).

## Input

`tools/ucode_dump_run.sh` (`UDUMP_LOADS=1` for the texts a `G_LOAD_UCODE` loads) leaves per (text, data) pair of a game's graphics tasks, in a scratch directory (ROM-derived, never in git):

| file | content |
|---|---|
| `<id>.<datahash>.text` | the 4 KiB of RDRAM at `OSTask.ucode`, big-endian (word i is at IMEM `(0x80 + 4 i) & 0xfff`: the boot code loads `ucode_size` bytes to 0x80, the microcode loads the tail to 0 itself) |
| `.data` | the data segment (the DMEM image: jump table, constants, overlay descriptors) |
| `.ext` | **new:** the 8 KiB of RDRAM behind the text. The lighting overlay of F3DEX2 sits at text + 0x1188, outside the 4 KiB the survey looked at (`desc_light` in the data points there), S2DEX's second overlay at + 0x1198, the exit code's tail at + 0x1000 |
| `.imem` | IMEM after the first run |

The registered builds come from the C sources themselves (`tools/derive_lib.py` reads `hle_f3dex2.c`, `hle_f3dex2_dialects.c`, `hle_s2dex2.c`, `f3dex.c`, `f3d.c` and their headers: the structs, `MUT(n, ok, bad)` resolved to `ok`, the enums,
the `HLE_UCODE` entries), so the hand-written tables are the one source of truth and the validation below compares with exactly what is compiled.

## Method

1. **Image.** The 4 KiB text plus the overlay segments that the data's descriptors name (`(text offset, length register << 16 | 0x1000 | IMEM address)` pairs; found by a scan, then only those that the build's own layout
   (`desc_light`, `desc_clip`) names stay "active"). A descriptor that declares more bytes than the code has (Paper Mario's two both say 0x4a8) leaves slack at the end of its segment, which counts as padding.
2. **Alignment.** Every instruction word gets a shape (`tools/rsp_isa.py`: opcode and registers, with the jump / branch target, the load / store offset and the ALU immediate left out; vector instructions and everything else
   exactly). `difflib` aligns the shapes of the two images, then again the leftovers of both sides (their order kept) until no block of 3 matches, so moved blocks (the F3DEX2 2.08 builds) stay aligned. Nops are never aligned (a lone nop would
   match any other); padding is judged from the words around it.
3. **Classification of every difference between two aligned words** (the shapes are equal, so it is one of these fields):
   - **pc**: a jump or branch whose target is the aligned counterpart of the reference's target, or an `addi rX, zero, imm` pc literal (`ra` return addresses, `0x1000 | pc` overlay window addresses) that is. Control flow is
     checked, not assumed: a retargeted branch is unexplained.
   - **knob**: an operand (load / store offset, ALU immediate) that holds the value of a **field of the reference's record**, and then the unknown's operand is that field's new value: layout addresses (`mv` ... `desc_clip`; direct), the output buffer
     size (`out_bytes`: `+v`, `-v`, `v - 1`, `1 - v`, modulo 0x1000, because DMEM wraps and the code adds 0x2000 to some of them), the reject mask (`andi` / `addiu`). **An address or constant that no field of the record names is unexplained
     whatever it looks like**: if a build moved something the record cannot express, the front-end cannot run it. A knob needs consistent evidence: two different new values, or one instruction that says "moved" against two that use the old value, make the
     verdict NONE (a deliberate error like that is mutation 3 and 5 below). A knob that no aligned instruction uses (twice, when unchanged) is **unconfirmed** and kept from the reference; the report names them.
   - **pad**: nops, and overlay slack. **pattern**: the one known insertion, `addi t, zero, 0xe7 ; sb t, 0(s7) ; addi s7, s7, 8` = a `G_RDPPIPESYNC` in front of the flush check (S2DEX 2.05 [Safe]); it sets `pipe_sync`.
   - everything else, and every unaligned word of the unknown, is **X**.
4. **Reference words the unknown lacks** (unaligned words of the reference with no counterpart): a run of 16 or more is a feature the unknown does not have (the reference's positional-lighting path, a handler such as `G_SPECIAL_1`);
   a shorter run is "an instruction missing" and counts like X; inside the start-up code it is `init` (below). A reference word that an unaligned run of the unknown replaces is not counted twice.
5. **Handlers and pcs.** The unknown's jump table is read at the derived `jump_table` address (indexed by the signed opcode). For every pc the reference's record names (`head`, `copy`, ..., the `f2_pcs` and extension-table pcs of S2DEX) the opcodes that
   lead to it in the reference's table are looked up in the unknown's table (a majority vote; the loop head is the target of the no-op opcodes), so the S2DEX shifted handlers need no alignment. A handler the reference lacks (`G_SPECIAL_1`, `G_TRI4`)
   is taken when the unknown's table leads that opcode to code that is not the head and the reference's leads it to the head.
6. **Several references.** The reference with the fewest unexplained words is the *primary*; the words it leaves unexplained are offered to the others (greedily: most words first). A word may be explained by a reference only if that reference has
   the **flag value that owns the word**: `zex` owns the triangle-setup code (reachable from `G_TRI1` / `G_TRI2`, not from `G_VTX`, not the clip or light overlays), `gen204` the vertex pipeline (`G_VTX`) and the clipper but for its reciprocal, `clip204` the clipper's reciprocal (the vector computation words of the clip overlay). Without this rule a text could be
   "explained" by mixing words of a F3DZEX build and a plain one that no build has (mutation 2 below).
7. **Flags** (`gen204`, `clip204`, `zex`): the value whose references explain words in the owned code that the references with the other value do not. F3D has attributes of this kind too (`xbus`, `vtx_wr`, ...): there the vote is
   taken only over pairs of references that differ in that attribute alone, and not at all when there is no such pair or the margin is under 4 : 1 (the tool says so). F3DEX 1.x decides its attributes otherwise (below).
   Rules that the tool knows about the front-end: `G_BRANCH_Z` reads the clip w (`lh 6(v)`) for F3DZEX without `gen204`, else the screen z (`lw 28(v)`), as `hle_f3dex2.c` does; `clip204` is voted from the clip overlay's vector words (`vrcph` / `vabs` of the intersection's scale) alone, `gen204` from the rest of the vertex pipeline and the clipper (its new-vertex stores, the fan), so a text can have the 2.04 reciprocal under 2.06 stores (F3DEX 2.05, Chameleon Twist 2); the loop is checked against all registered builds.
8. **The rest of the record**: `load_key` is the FNV of the first 0xf48 bytes (the tool reproduces Donkey Kong 64's `0x4d947651fb1d5494` and the removed F-Zero X entry's `0xd02467acf66d7396`); the dead ranges, `ring_off` and the informational `HLE_UCODE.flags`
   are copied from the primary reference and are **not evidence**: a dead range needs the poison proof of [testing.md](testing.md) (`ARMLET_DMEM_POISON`) whatever the tool says.

### Verdicts

- **EXACT**: no unexplained word; every difference to the references is a relocation (pc, DMEM address), a derived constant, padding or the pipe-sync pattern, and no reference code is missing. The record is the build's record.
- **NEAR**: no unexplained word, but at least one of the near-class categories below. They are the ones that the near-match tolerance agreed for near ucode builds can admit (slightly relaxed exactness: a tiny tint or dither yes, wrong colours or geometry no); the probe of the next phase has to confirm them with the build's own text:
  - `init`: a vector or scalar ALU instruction (no store, no control flow, no `mtc0`) in the start-up code in front of the first command fetch (pc 0x80 up to the loop head). The interpreter runs that code for real and the front-end re-reads
    the registers and DMEM it leaves; the differences seen are `vor` for `vxor` (OoT's `v0`), one `vadd v1, v0, v0` more or less (the later builds clear the carry of a following `vsub`).
  - `light-positional`: the lighting overlay of a build with the positional-light variant, found by its guard (`andi rX, rY, 0x40 ; beq rX, zero`: geometry bit 0x400000), which the front-end declines (`geom_def` lacks the bit); the unaligned words of that overlay are
    counted as near, not the other overlays. Paper Mario's 169 words.
  - `light-small`: at most 16 unexplained words in the lighting overlay (a lane, a rounding): only vertex colours and texture coordinates can change. (No text seen so far needs it.)
  - reference code of 16 or more words that the unknown lacks (a feature the unknown does not have: the unknown can do less than the reference, and the front-end declines what it cannot do).
- **NONE**: anything else, in particular any unexplained word in the vertex, triangle, clip, matrix or command code, a conflicting constant, a short run of missing instructions, and a family score under 50 % aligned.

A verdict is a statement about the *code*. NONE does not mean "wrong": Chameleon Twist 2's F3DEX 2.05 was NONE until `clip204` was split from `gen204` (2.04's pcs and F3DZEX's triangle code, the clipper's reciprocal of the 2.04 flavour under the 2.06 stores and fan: two words that no reference combined with the rest, and three instructions missing); it derives EXACT now (`gen204 = 0, zex = 1, clip204 = 1`) and verified clean in 3839 tasks (below).
It means that a person or the probe has to look.

## Validation: leave-one-out against the hand-written records

Every registered build is derived **without its own table** (all texts of its record are removed from the reference set) and the derived record is compared with the hand-written one, field by field (F3DEX2, S2DEX and Rej: 79 fields each since the Rej work (74 before the S2DEX pcs, then 77, 78, now `lay.rej_flags`), from `pc.*`, `f2pcs.*`, `lay.*`,
`gen204`, `zex`, `reject_mask`, `load_key` and the extension table; F3DEX 1.x: 65 since its records run (below; the rows here are the current ones); F3D: 70 since its records run, "F3D records that run" below). `tools/derive_check.sh DUMPS` runs it (and the mutation check) and fails on a mismatch that is not in the table of understood ones below.

| family | build (hand record) | ids | primary reference | also needs | verdict | unexplained | near / reference-only | fields compared | fields that differ from the hand record |
|---|---|---|---|---|---|---|---|---|---|
| f3dex2 | l_b_dk64 | 1fdeaad6 | l_b_ex2_207n | - | EXACT | 0 | - / 0 | 79 | none |
| f3dex2 | l_b_snap | d576cfcf | l_b_ex2_208 | l_b_ex2_204h | EXACT | 0 | - / 0 | 79 | none |
| f3dex2 | l_b_pm | c734403d | l_b_ex2_208 | l_b_bg, l_b_ex2_203 | NEAR | 0 | {'light-positional': 168} / 0 | 79 | none |
| f3dex2 | l_b_oot | 0d3f849c | l_b_ex2_206 | - | NEAR | 0 | {'init': 1} / 1 | 79 | none |
| f3dex2 | l_b_ex2_208 | d2d1f9f2 | l_b_ex2_208n | - | EXACT | 0 | - / 0 | 79 | none |
| f3dex2 | l_b_ex2_204 | 5dce3244 | l_b_ex2_204h | - | EXACT | 0 | - / 0 | 79 | none |
| f3dex2 | l_b_ex2_204h | e8574467 | l_b_ex2_204 | l_b_snap | EXACT | 0 | - / 0 | 79 | none |
| f3dex2 | l_b_bg | e80e5292 | l_b_pm | l_b_ex2_203, l_b_ex2_208, l_b_rej_flx | NONE | 194 | {'init': 1} / 200 | 79 | unconfirmed 12, expected 4 |
| f3dex2 | l_b_ex2_208n | 9875370e | l_b_ex2_208 | - | EXACT | 0 | - / 0 | 79 | none |
| f3dex2 | l_b_ex2_206 | 0bbec7c3 | l_b_oot | l_b_dk64 | NEAR | 0 | - / 1 | 79 | none |
| f3dex2 | l_b_ex2_207n | d8bde272 | l_b_dk64 | - | EXACT | 0 | - / 0 | 79 | none |
| f3dex2 | l_b_s2_kirby | 5f33af89 | l_b_s2_208 | - | NEAR | 0 | - / 1 | 79 | none |
| f3dex2 | l_b_s2_oot | Lf535d16 | l_b_s2_kirby | l_b_oot | EXACT | 0 | - / 0 | 79 | none |
| f3dex2 | l_b_s2_208 | b228360b | l_b_s2_kirby | l_b_s2_xbus | EXACT | 0 | - / 0 | 81 | none |
| f3dex2 | l_b_s2_xbus | 3ee63656 | l_b_s2_208 | l_b_ex2_208n_xbus, l_b_oot, l_b_rej_flx, l_b_rej_xbus | NONE | 51 | {'init': 1} / 11 | 81 | unconfirmed 1, expected 1 |
| f3dex2 | l_b_ex2_208n_xbus | L649a083 | l_b_ex2_208 | l_b_s2_xbus | NONE | 7 | - / 19 | 81 | unconfirmed 1, expected 1 |
| f3dex2 | l_b_ex2_203 | 33dc705a | l_b_ex2_204 | l_b_bg | NONE | 32 | - / 36 | 79 | none |
| f3dex2 | l_b_rej_203 | 7de803fd | l_b_rej_flx | l_b_rej_206, l_b_rej_xbus | NONE | 27 | - / 35 | 79 | unconfirmed 2, expected 1 |
| f3dex2 | l_b_rej_206 | 95129dc7 | l_b_rej_203 | l_b_rej_xbus | NONE | 153 | - / 150 | 79 | unconfirmed 1, expected 2 |
| f3dex2 | l_b_rej_flx | c750be26 | l_b_rej_203 | - | NONE | 50 | {'init': 1, 'light-small': 1} / 61 | 79 | unconfirmed 2, expected 1 |
| f3dex2 | l_b_rej_xbus | 8b0a3e32 | l_b_rej_203 | l_b_ex2_208n_xbus | NONE | 88 | {'init': 2, 'light-small': 12} / 87 | 79 | unconfirmed 1, expected 1 |
| f3dex1 | l_b_121 | 208a567e | l_b_123 | l_b_non122 | EXACT | 0 | - / 0 | 65 | none |
| f3dex1 | l_b_non122 | fd5a31cc | l_b_121 | l_b_non100 | EXACT | 0 | - / 0 | 65 | none |
| f3dex1 | l_b_123 | 047252e8 | l_b_121 | l_b_lx123 | EXACT | 0 | - / 0 | 65 | none |
| f3dex1 | l_b_lx123 | 7502cfb7 | l_b_lx121 | l_b_lxn123 | EXACT | 0 | - / 0 | 65 | none |
| f3dex1 | l_b_095 | 34dad673 | l_b_non100 | l_b_121 | NONE | 32 | - / 28 | 65 | expected 4 |
| f3dex1 | l_b_lx121 | 19bd8c82 | l_b_lx123 | l_b_lxn121 | EXACT | 0 | - / 0 | 65 | none |
| f3dex1 | l_b_lxn121 | 0a7cc6c9 | l_b_lxn123 | l_b_lx121 | EXACT | 0 | - / 0 | 65 | none |
| f3dex1 | l_b_lxn123 | cc09096c | l_b_lx123 | l_b_lxn121 | EXACT | 0 | - / 0 | 65 | none |
| f3dex1 | l_b_non100 | 5063f108 | l_b_non122 | l_b_095 | NONE | 12 | - / 22 | 65 | none |
| f3d | l_sm64 | 94c7cfdd | l_tt | l_pw | NONE | 0 | - / 5 | 70 | none |
| f3d | l_pw | 3e9b9c3a | l_sm64 | l_ge | NONE | 0 | - / 6 | 70 | none |
| f3d | l_wr | fb483893 | l_sm64 | l_tt | NONE | 80 | - / 47 | 70 | expected 2, noted 1 |
| f3d | l_mm | 4212d63f | l_tt | - | NONE | 48 | - / 50 | 70 | none |
| f3d | l_tt | 88fa3c0a | l_sm64 | l_mm | NONE | 2 | - / 2 | 70 | none |
| f3d | l_ge | 538f79d8 | l_pw | l_tt | NONE | 17 | - / 16 | 70 | expected 2 |

Mismatches, with the class the tool gives them (**unconfirmed**: the derived value is the reference's and the report says no aligned instruction confirms it; **noted**: the tool said it could not decide the attribute; **expected**: understood and
listed in `EXPECTED` of `derive_build.py`; **UNEXPLAINED** would fail the check, and there is none):

| build | field | hand | derived | class | why |
|---|---|---|---|---|---|
| l_b_bg | lay.fog_mul | 0x248 | 0x1e0 | unconfirmed | unconfirmed: no aligned instruction anchors it, kept from the primary reference (reported as such) |
| l_b_bg | lay.fog_ofs | 0x24a | 0x1e2 | unconfirmed | unconfirmed: no aligned instruction anchors it, kept from the primary reference (reported as such) |
| l_b_bg | lay.texture | 0x24c | 0x1e4 | unconfirmed | unconfirmed: no aligned instruction anchors it, kept from the primary reference (reported as such) |
| l_b_bg | lay.tex_scale_s | 0x250 | 0x1e8 | unconfirmed | unconfirmed: no aligned instruction anchors it, kept from the primary reference (reported as such) |
| l_b_bg | lay.tex_scale_t | 0x252 | 0x1ea | unconfirmed | unconfirmed: no aligned instruction anchors it, kept from the primary reference (reported as such) |
| l_b_bg | lay.lights_size | 0x23f | 0x1dd | unconfirmed | unconfirmed: no aligned instruction anchors it, kept from the primary reference (reported as such) |
| l_b_bg | lay.light_raw | 0x268 | 0x1f8 | unconfirmed | unconfirmed: no aligned instruction anchors it, kept from the primary reference (reported as such) |
| l_b_bg | lay.light_base | 0x278 | 0x200 | unconfirmed | unconfirmed: no aligned instruction anchors it, kept from the primary reference (reported as such) |
| l_b_bg | lay.light_scale | 0x226 | 0x1c6 | unconfirmed | unconfirmed: no aligned instruction anchors it, kept from the primary reference (reported as such) |
| l_b_bg | lay.light_scale_zex | 0x22e | 0x1ce | unconfirmed | unconfirmed: no aligned instruction anchors it, kept from the primary reference (reported as such) |
| l_b_bg | lay.movemem_tab | 0x550 | 0x2f0 | unconfirmed | unconfirmed: no aligned instruction anchors it, kept from the primary reference (reported as such) |
| l_b_bg | lay.moveword_adj | 0x2a60 | 0x27fe | unconfirmed | unconfirmed: no aligned instruction anchors it, kept from the primary reference (reported as such) |
| l_b_bg | lay.geom_def | 0xff0605 | 0xbf0605 | expected | a front-end decision (which geometry-mode bits the build gives a meaning to): BG's extra bit 0x400000 is the positional-lighting bit, implemented by its own kernel; not visible as an operand |
| l_b_bg | lay.mw_max | 0x10 | 0xe | expected | the size of the G_MOVEWORD table (16 in BG): a bound in a compare no other build's code shows |
| l_b_bg | lay.no_lit | 0x1 | 0x0 | expected | BG's lighting is another kernel (f3dexbg_fx): the tool reports the lighting overlay as different (X) but cannot name the kernel |
| l_b_bg | lay.lit_vtx | 0x1 | 0x0 | expected | the same |
| l_b_s2_xbus | lay.out_toggle | 0x0 | 0xc00 | unconfirmed | unconfirmed: no aligned instruction anchors it, kept from the primary reference (reported as such) |
| l_b_s2_xbus | lay.out_bytes | 0x0 | 0x10 | expected | an XBUS build has no output buffer (hand value 0): the words that carry Worms' buffer size are replaced by the ring code (init, the flush routine at 0x648), and the tool reads an unrelated immediate there as a size (16) |
| l_b_ex2_208n_xbus | lay.out_toggle | 0x0 | 0x208 | unconfirmed | unconfirmed: no aligned instruction anchors it, kept from the primary reference (reported as such) |
| l_b_ex2_208n_xbus | lay.out_bytes | 0x0 | 0x10 | expected | an XBUS build has no RDP output buffers (its list is a ring in DMEM, the layout's xbus / ring_start): the buffer sizes that its flush code does not name are 0 in the hand record; the transport is not derived (the XBUS work); verdict NONE |
| l_b_rej_203 | lay.movemem_tab | 0x2f0 | 0x290 | unconfirmed | unconfirmed: no aligned instruction anchors it, kept from the primary reference (reported as such) |
| l_b_rej_203 | lay.moveword_adj | 0x27fe | 0x279e | unconfirmed | unconfirmed: no aligned instruction anchors it, kept from the primary reference (reported as such) |
| l_b_rej_203 | lay.rej_flags | 0x9 | 0x3 | expected | REJ_LIT_LX (the unsigned multiply-accumulate lighting) is in no other reference, so no remaining reference can show it; REJ_FLAT_A is flx's alone, and flx is the primary reference (no discriminating evidence: the primary's bit) |
| l_b_rej_206 | lay.light_scale | 0x1c6 | 0x1ce | unconfirmed | unconfirmed: no aligned instruction anchors it, kept from the primary reference (reported as such) |
| l_b_rej_206 | lay.no_lit | 0x1 | 0x0 | expected | F3DEX.Rej 2.06's lighting is not modelled (lit vertex commands decline): a front-end decision no operand shows |
| l_b_rej_206 | lay.rej_flags | 0x0 | 0x1 | expected | every other Rej reference rounds y (REJ_YQ): none shows the text that does not, so the bit is the primary reference's |
| l_b_rej_flx | lay.movemem_tab | 0x290 | 0x2f0 | unconfirmed | unconfirmed: no aligned instruction anchors it, kept from the primary reference (reported as such) |
| l_b_rej_flx | lay.moveword_adj | 0x279e | 0x27fe | unconfirmed | unconfirmed: no aligned instruction anchors it, kept from the primary reference (reported as such) |
| l_b_rej_flx | lay.rej_flags | 0x7 | 0x9 | expected | REJ_FLAT_A and REJ_LIT_FLX are in no other reference (this build is the only one with them), REJ_LIT_LX is the primary reference's: bits that no remaining reference can show |
| l_b_rej_xbus | lay.out_toggle | 0x0 | 0x300 | unconfirmed | unconfirmed: no aligned instruction anchors it, kept from the primary reference (reported as such) |
| l_b_rej_xbus | lay.out_bytes | 0x0 | 0x10 | expected | the same: Excitebike 64's Rej text is an XBUS build, derived here from the fifo Rej texts (the flush code is unexplained words); xbus / ring_start are the primary reference's, not derived |
| l_b_095 | pc.persp | 0x3c8 | 0x0 | expected | F3DEX 0.95's G_PERSPNORM handler is in no other reference (F3DLX 0.95, the only other build with it, has no dump): no handler to map it from; the front-end of the derived build declines the command. Verdict NONE (32 unexplained words) |
| l_b_095 | pc.quad | 0x244 | 0x0 | expected | the same for G_QUAD |
| l_b_095 | pc.culldl | 0x368 | 0x0 | expected | the same for the 0.95 form of G_CULLDL (byte offsets, the last vertex not tested): its code differs from the 1.2x handler |
| l_b_095 | flag.culldl_off | 0x1 | 0x0 | expected | follows from the handler: without a G_CULLDL pc the attribute is the primary reference's (and the command declines) |
| l_wr | pc.tri1 | 0x278 | 0x0 | expected | Wave Race's G_TRI1 entry is a call into a routine it shares with its quad command (`jal 0x288`): no other reference's handler maps onto it; the derived build declines G_TRI1. Verdict NONE (80 unexplained words) |
| l_wr | pc.quad | 0x1fc | 0x0 | expected | Wave Race's quad command is in no other reference: no handler to map it from (the derived build declines the command) |
| l_wr | flag.vptr_mul | 0x8 | 0x4 | noted | noted: the tool says it could not decide it |
| l_ge | lay.tri4_tab | 0x2d0 | 0x0 | expected | follows from the handler: without a G_TRI4 pc there is no table to read |
| l_ge | pc.tri4 | 0x20c | 0x0 | expected | GoldenEye's G_TRI4 (2.0G) is in no other reference: no handler to map it from (the derived build declines the command). Verdict NONE (its 17 words unexplained) |

What this shows:

- **F3DEX2 and S2DEX: 798 of 814 field comparisons identical; all 16 differences are Conker's F3DEXBG** (see below). The other ten records, among them every dialect the survey asked about, are reproduced field for field with their own table removed:
  - **The reject masks.** Ocarina of Time's F3DZEX.NoN 2.06H and Pokemon Snap's F3DEX.NoN 2.08H derive to `0x70b0`, Donkey Kong 64, Paper Mario and the 2.08 / 2.04 builds to `0x7070`, F3DEXBG to `0x70b0`, from the immediates of the two places that read the mask (`andi t3, a1, mask` in `G_TRI1`,
    `addiu at, zero, mask` in `G_CULLDL`) against a reference with the other value. The error that `hle_f3dex2.c` once had (`0x7030`) is read as `0x7030` if the immediate says so (mutation 3), and a text whose two sites disagree is NONE.
  - **The 2.04 `gen204` differences.** `5dce` derives from 2.04H and the other way round with `gen204 = zex = 1`; Ocarina of Time (zex without gen204) is derived from Donkey Kong 64 (neither flag) plus the 2.04 pair (both), which supply the 21 words that differ from Donkey Kong 64: the F3DZEX lane choices of the triangle setup (19 words that only the zex references explain) and the lighting overlay's lanes, while the
    words of the vertex pipeline and clipper that are gen204 (`SDV` for `SSV` stores of the screen z, `vabs` / `vrcph` in the clipper's reciprocal, the fan, `vmudh v22` for `v21` in the linear texture generation: 20 words that only the non-gen204 references explain) stay Donkey Kong 64's. Both flags come out right although no build
    with exactly that combination is in the reference set; the one word left, `lh 6(v)` in `G_BRANCH_Z`, is the front-end's own rule (step 7).
  - **S2DEX.** Kirby 64's / Resident Evil 2's text derives from Worms' 2.08 with the handlers from 0x184 on 8 bytes lower (`head` 0x1a4 and six of the ten extension pcs: `G_OBJ_LOADTXTR` 0x550, `G_BG_1CYC` 0xb74, `G_BG_COPY` 0x208, `G_OBJ_RECTANGLE` 0xb80, `G_OBJ_SPRITE` 0x6c0, `G_SETSCISSOR` 0x1d0; `copy` 0x140 .. `ef` 0x170, `omh` / `oml` and the other four stay), Ocarina of Time's 2.05 [Safe] from Kirby's
    text with `out_bytes = 792` (read from `addiu s3, t3, 792`, `addi s7, s6, -792`, `addiu t4, t3, 791`, `addi s4, s6, -8984` ...), `pipe_sync = 1` (the three inserted words) and the pcs behind 0x650 shifted by 0xc / 0x10 (`G_BG_1CYC` 0xb84, `G_OBJ_RECTANGLE` 0xb90, `G_OBJ_SPRITE` 0x6d0, the
    `G_OBJ_LOADTXTR` continuation pcs live in the front-end, not in the record). The overlay descriptors (`desc_clip` 0x314, `desc_light` 0x31c) are found in the data and the two overlays are aligned (second one from the `.ext` dump).
  - **Paper Mario** derives (NEAR) with `light-positional` for 169 words of its lighting overlay: Paper Mario's is the Kawasedo 2.08 with the positional-light variant ("F3DEX fifo 2.08 Yoshitaka Yasumoto/Kawasedo 1999"), the other 2.08 texts have the 130-word overlay. This is the reason the hand
    record needed the geometry-bit decline; the tool shows where it comes from.
- **Conker's F3DEXBG** moved the DMEM state. Seventeen of the moved layout fields are derived correctly from the aligned code (`jump_table` 0x5d6, `in` / `in_end` 0xbb8 / 0xc60, `vtab`, `geom` 0x254, `tg`, `const31` / `const30`, `mvp_valid`, `lights_valid`, `clipmask`, `culltab`, `clip_masks`, `desc_light`, `desc_clip`, `out_toggle` 0x140, `out_bytes` 16),
  with `reject_mask = 0x70b0`, `G_TRI4` (a handler the references lack) and every pc. Twelve moved fields (the fog, texture and light-slot words, the light scales, `movemem_tab`, `moveword_adj`) are read only by BG's own vertex and lighting code, which is in no other build and so not aligned: the tool keeps the reference's address and lists them as unconfirmed.
  `geom_def` / `mw_max` / `no_lit` / `lit_vtx` are decisions of the front-end (a bit set, a table size, a kernel) that no operand shows. Verdict NONE (202 unexplained words: the lighting overlay and part of the vertex code), which is right: BG needs its own kernel.
- **F3DEX 1.x** (since 2026-10-17, "F3DEX 1.x records that run" below): the whole record of the front-end, 65 fields (the layout, the 24 handler pcs, the reject mask, NoN, LX, the G_CULLDL form, the load key).
  Eight of the nine texts come out equal field by field, seven of them EXACT; F3DEX 0.95 is NONE with three handlers no other reference has (its G_PERSPNORM, G_QUAD and the 0.95 G_CULLDL: "expected"), Turok's
  1.00 NONE (12 unexplained words) with a record that is still equal. The attributes the old tool could not decide by pairs of references (`non` for 1.21 / 1.23 / F3DLX.NoN, `v095`) are read from the text or
  became data: `v095` is gone (its opcodes, depth byte and vertex table are the layout and the handler pcs).
- **F3D** (since 2026-10-17, "F3D records that run" below): the whole record of the front-end, 70 fields (the layout, the 23 handler pcs, the three masks, seven attributes, the load key). The six
  references are six different texts (no two builds share code the way the F3DEX 1.x versions do), so every leave-one-out verdict is NONE, but the records come out equal field by field for SM64, Pilotwings,
  Mischief Makers and Tetrisphere; Wave Race's G_TRI1 entry and quad command and GoldenEye's G_TRI4 are in no other reference ("expected"). The attributes the old pair votes could not decide (`xbus`,
  `vtx_wr`, `tri_wr`, `gp_end`, the loop head, `half_shift`, `quad`) are read from the text or became table cells.

## The mutation check

`derive_build.py mutate DUMPS` takes Donkey Kong 64's text (against the other builds) and applies deliberate errors; each must change the result:

```
unmutated Donkey Kong 64 against the other builds: NEAR, 0 unexplained, derived reject_mask 0x7070
  vertex pipeline: opcode of the instruction at 0x5e0 changed                    -> NONE  unexplained   1 near {} caught
  triangle setup: element selector of the instruction at 0x94c changed           -> NONE  unexplained   1 near {} caught
  G_TRI1 reject mask 0x7070 -> 0x7050                                            -> NONE  unexplained   0 near {} caught
  a branch of G_GEOMETRYMODE retargeted by 8 bytes                               -> NONE  unexplained   1 near {} caught
  one load of the geometry mode word moved from 0x1ec to 0x1f0 (the other uses stay) -> NONE  unexplained   0 near {} caught
  a use of the matrix stack pointer (0xf4, no layout field) moved to 0xf0        -> NONE  unexplained   1 near {} caught
  an instruction in the vertex loop replaced by a nop                            -> NONE  unexplained   1 near {} caught
  jump table entry of G_VTX 0x4f0 -> 0x4f8                                       -> derived vtx 0x4f8 caught
```

(1: an opcode in the vertex pipeline; 2: an element selector in the triangle setup that another build (F3DZEX) has, but with the other flag value; 3: the mask of `G_TRI1` alone; 4: a branch retargeted; 5: one of the uses of the geometry mode word moved: the record would have to name it twice;
6: an address that is no field; 7: a real instruction replaced by a nop; 8: a table entry.)  `selftest` runs the decoder and alignment on synthetic words without any dump.

## F-Zero X's F3DEX 2.03 is not exact

The text that was unregistered because its vertex clip flags were wrong (`33dc705a5e4d39ae`; section 1 of [ucode-f3dex2-dialects.md](ucode-f3dex2-dialects.md)) derives, against the registered builds, to: verdict **NONE**, 32 unexplained words and 36 words of the
reference (2.04) that it lacks: in the vertex pipeline (`llv v22` and the `vmudl` / `vmadm` / `vmudn` / `vmadh` chains with other element selectors: 5a0 .. 5fc, 6e8, 6f0, 700, 708, 764 .. 77c: the clip-flag lanes "permuted" that the dialects page names) and in the lighting overlay (3d4 .. 460).
The pcs are exactly the removed entry's (`head 0x18c`, `tri1 0x794`, `tri2 0x78c`, `vtx 0x4e8`, `modvtx 0xc54`, `culldl 0xc14`, `branchz 0xc3c`, `load_key 0xd02467acf66d7396`) and the code of the other handlers is the 2.04's, but the tool sets `zex = gen204 = 1` where the old entry had both off
(the dialects page says its triangle constants "would need `zex`"): a point in favour of the tool, and a reason not to re-register the old record.

```
ucode 33dc705a5e4d39ae.3aa37f16eca9a30c  RSP Gfx ucode F3DEX       fifo 2.03  Yoshitaka Yasumoto 1998 Nintendo. | RSP Gfx ucode F3DLX.Rej   f
games (tasks in the survey run): F-Zero X (1492)
family scores (family, aligned fraction, residual words, verdict): [('f3dex2', 0.779, 68, 'NONE'), ('f3dex1', 0.032, 1905, 'NONE'), ('f3d', 0.021, 1916, 'NONE')]
best family f3dex2; primary reference l_b_ex2_204; also explained by {'l_b_bg': 6}
VERDICT NONE
  unexplained words: 32; near-class words: -; reference-only words: {'deleted': 36}
...
```

## The hot list

`derive_build.py hot DUMPS STATS` derives every non-native text of the coverage recount ([ucode-coverage.md](ucode-coverage.md)) and prints the table below (details per ucode with `--no-details` off). "Derivation: best family" is the family
with the highest aligned fraction; rows with the state "registered (declines for other reasons)" have an `hle_ucode` already and are listed only when their *text* is not a registered one.

| # | ucode (text) | family | games | RSP time on JIT (mean over 64 games) | task share | build handler today | derivation: best family | verdict | unexplained words | near / reference-only | primary + composite |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | F3DEX.NoN fifo 2.08 / F3DEX fifo 2.08 `9875370e` | F3DEX2 | 3 | 4.69% | 4.69% | none | f3dex2 | EXACT | 0 | - | l_b_ex2_208 |
| 2 | F3DEX fifo 2.06 `0bbec7c3` | F3DEX2 | 3 | 4.69% | 4.69% | none | f3dex2 | NEAR | 0 | init 1 | l_b_oot + l_b_dk64 |
| 3 | F3D 2.0G `538f79d8` | F3D | 1 | 1.56% | 1.56% | none | f3d | NONE | 17 | deleted 16 | l_pw + l_tt |
| 4 | F3DEX fifo 2.07 `48c7e653` | F3DEX2 | 1 | 1.56% | 1.56% | none | - | SAME TEXT as registered l_b_dk64 | - | - | - |
| 5 | S2DEX 1.06 / F3DEX.NoN 1.23 `5b327e0d` | F3DEX | 1 | 1.56% | 1.56% | none | f3dex2 | NONE | 339 | init 29, deleted 153, init 10, extra 17 | l_b_s2_kirby + l_b_oot, l_b_rej_flx, l_b_rej_xbus, l_b_snap |
| 6 | F3DEX fifo 2.05 `97989ee9` | F3DEX2 | 1 | 1.56% | 1.56% | none | f3dex2 | NONE | 2 | init 1, deleted 3 | l_b_oot + l_b_ex2_204 |
| 7 | F3DEX.NoN 1.23 `efe4b8f3` | F3DEX | 1 | 1.56% | 1.56% | none | f3dex1 | NONE | 0 | deleted 2 | l_non + l_dex:047252 |
| 8 | F3DLX.Rej xbus 2.08 `8b0a3e32` | F3DEX2 | 1 | 1.56% | 1.56% | none | - | SAME TEXT as registered l_b_rej_xbus | - | - | - |
| 9 | (no banner) `bfbf5496` | other | 1 | 1.56% | 1.56% | none | f3dex1 | NONE | 851 | deleted 281, extra 453 | l_dex:047252 + l_dlx:7502cf, l_non:5063f1 |
| 10 | F3DEX.NoN fifo 2.07 `d8bde272` | F3DEX2 | 1 | 1.56% | 1.56% | none | f3dex2 | EXACT | 0 | - | l_b_dk64 |
| 11 | F3D 2.0H `4212d63f` | F3D | 1 | 1.56% | 1.56% | registered (declines for other reasons) | - | SAME TEXT as registered l_mm | - | - | - |
| 12 | S2DEX 1.07 `190d04df` | other | 1 | 1.53% | 1.34% | none | f3dex2 | NONE | 297 | init 29, deleted 133, init 10 | l_b_s2_kirby + l_b_oot, l_b_pm, l_b_rej_flx, l_b_rej_xbus, l_b_snap |
| 13 | (no banner) `a6c64e8d` | other | 1 | 1.45% | 1.00% | registered (declines for other reasons) | f3dex1 | NONE | 862 | deleted 237, extra 511 | l_dex:047252 + l_dlx:7502cf |
| 14 | F3D 2.0D `3e9b9c3a` | F3D | 1 | 1.37% | 0.43% | registered (declines for other reasons) | - | SAME TEXT as registered l_pw | - | - | - |
| 15 | F3DEX 1.21 `208a567e` | F3DEX | 1 | 1.36% | 0.27% | registered (declines for other reasons) | - | SAME TEXT as registered l_dex | - | - | - |
| 16 | F3D 2.0H `968dec0e` | F3D | 1 | 1.27% | 0.22% | none | f3d | NONE | 783 | deleted 65, extra 630 | l_tt + l_wr |
| 17 | F3DEX fifo 2.03 / F3DLX.Rej fifo 2.03 `33dc705a` | F3DEX2 | 1 | 0.89% | 0.81% | none | - | SAME TEXT as registered l_b_ex2_203 | - | - | - |
| 18 | S2DEX fifo 2.06 / F3DEX 1.23 `5f33af89` | F3DEX2 | 1 | 0.63% | 0.40% | registered (declines for other reasons) | - | SAME TEXT as registered l_b_s2_kirby | - | - | - |
| 19 | F3DFLX.Rej fifo 2.03F / L3DEX fifo 2.03 `c750be26` | F3DEX2 | 1 | 0.57% | 0.59% | none | - | SAME TEXT as registered l_b_rej_flx | - | - | - |
| 20 | F3DEX 1.23 `047252e8` | F3DEX | 2 | 0.47% | 0.51% | registered (declines for other reasons) | - | SAME TEXT as registered l_dex:047252 | - | - | - |
| 21 | F3DEX.Rej fifo 2.06 / F3DEX.Rej xbus 2.06 `95129dc7` | F3DEX2 | 1 | 0.45% | 0.39% | none | - | SAME TEXT as registered l_b_rej_206 | - | - | - |
| 22 | F3DEX.NoN 1.21 `fd5a31cc` | F3DEX | 1 | 0.37% | 0.11% | registered (declines for other reasons) | - | SAME TEXT as registered l_non | - | - | - |
| 23 | F3DEX.NoN 1.22 `dff34c75` | F3DEX | 1 | 0.21% | 0.06% | none | f3dex1 | NONE | 14 | - | l_non |
| 24 | F3DLX.Rej fifo 2.03 / F3DFLX.Rej fifo 2.03F `7de803fd` | F3DEX2 | 1 | 0.10% | 0.16% | none | - | SAME TEXT as registered l_b_rej_203 | - | - | - |
| 25 | F3D 2.0H `9d0d289f` | F3D | 1 | 0.09% | 0.27% | none | f3d | NONE | 789 | deleted 59, extra 631 | l_mm + l_tt |
| 26 | L3DEX 1.21 `3c7a64e0` | other | 1 | 0.08% | 0.22% | none | f3dex1 | NONE | 535 | deleted 38, extra 360 | l_dex + l_non:5063f1 |
| 27 | L3DEX fifo 2.08 `cd32d8e5` | F3DEX2 | 1 | 0.08% | 0.78% | none | f3dex2 | NONE | 327 | init 1, light-small 4, init 3, deleted 49, extra 554 | l_b_oot + l_b_pm, l_b_s2_208 |
| 28 | F3DLX 1.21 `76c763bc` | F3DEX | 1 | 0.03% | 0.00% | registered (declines for other reasons) | - | SAME TEXT as registered l_dlx | - | - | - |
| 29 | F3D 2.0D `2f411927` | F3D | 1 | 0.02% | 0.25% | none | f3d | NONE | 33 | deleted 43 | l_sm64 + l_mm |
| 30 | F3D 2.0D `1d12f4b3` | F3D | 1 | 0.00% | 0.03% | none | f3d | NONE | 507 | deleted 34, extra 384 | l_sm64 + l_wr |

Reading the verdicts:

- **Rows 1, 2, 4, 10: EXACT or NEAR F3DEX2 builds, 12.5 points of the 34.4 left on the JIT** (mean over 64 games; the three games of row 1 and the three of row 2 are the largest single items). Row 4 is the registered Donkey Kong 64 text under another id (Rocket's `ucode_size` is 0xf80).
  Row 1 is **EXACT against the registered 2.08**: same code, `reject_mask 0x70b0` (NoN), pcs of the 2.08 builds. Row 10 is **EXACT against Donkey Kong 64's 2.07** with the NoN mask. Row 2 (2.06) is NEAR: one `init` word, with the F3DZEX triangle code (`zex = 1`).
- **Rows 6 and 7** were close: Chameleon Twist 2's 2.05 (2 unexplained words in the clipper, 3 missing: EXACT since `clip204`) and Donald Duck's F3DEX.NoN 1.23 (no unexplained word, 2 instructions missing against the 1.22 / 1.21 builds). Both verify clean in the experiment below.
- **Everything else is another microcode** (the 18.8 points that remain after rows 1, 2, 4, 6, 7 and 10 are 7.2 points of registered builds that decline in the front-end and 11.6 of the microcode below): S2DEX 1.06 / 1.07 (a different generation than 2.x; 300 to 340 words), the Rej / F3DLX.Rej / F3DFLX.Rej / L3DEX texts (500 to 700 words differ: 20-byte vertex records, other triangle code), F3D 2.0G (GoldenEye: 17 + 16 words against
  the 2.0D family) and the 2.0H re-layouts (Glover, Wipeout: 780 words), Star Wars Rogue Squadron and Indiana Jones (no family), F-Zero X's 2.03 (above), the F3DEX.NoN 1.22 of Iggy's Reckin' Balls (14 words). Their 11.6 points need kernels, not records.
- (2026-10-17) The F3D rows as the F3D derivation sees them now ("F3D records that run" below): row 3 (GoldenEye's 2.0G) NONE, 17 unexplained words, 16 reference-only, l_pw + l_tt: its four-triangle
  command, which is now a command of the front-end, and the text is registered; row 29 (`2f41`) NONE, 32 / deleted 29, l_mm + l_sm64 (an output flush that keeps the lists in RDRAM); rows 16, 25 and 30 as
  before (other code: the Sprite2D microcode, and `1d12` at 51 %).

### Do the EXACT / NEAR claims hold? An experiment on the desktop

The candidates were registered in a **scratch copy** of the tree (never in git: `derive_build.py emit` writes the C of the `f3dex2_build` records, with the standard F3DEX2 dead ranges, and an alias line for F3DEX.NoN 1.23 in `f3dex.c`), built, and every game that
runs one of the texts was run for 70 s with `GfxMode = hle` and `ARMLET_HLE_VERIFY=1` (each native task also run by the JIT; the external-effects compare of [design.md](design.md): RDRAM written by DMA, the RDP stream, SP / DPC registers, DMEM except the dead ranges, IMEM):

| text | tool verdict | game | tasks compared | with differences | other declines |
|---|---|---|---|---|---|
| F3DEX.NoN 2.08 `9875` | EXACT | LEGO Racers | 3135 | **0** | 0 |
| | | Banjo-Tooie | 1343 | **0** | 0 |
| | | Star Wars Episode I Racer | 2687 | **0** | 0 |
| F3DEX 2.06 `0bbe` | NEAR | Blues Brothers 2000 | 4159 | **0** | 0 |
| | | Harvest Moon 64 | 6207 | **0** | 0 |
| | | Gauntlet Legends | 209 | **207** | 3502 (`too many SP_STATUS polls`) |
| F3DEX.NoN 2.07 `d8bd` | EXACT | Tonic Trouble | 1663 | **0** | 0 |
| F3DEX 2.07 `1fde` (DK64 text, another id) | EXACT | Rocket | 1919 | **0** | 0 |
| F3DEX 2.05 `9798` | EXACT (was NONE: 2 words, 3 missing; the verify column is from before the split) | Chameleon Twist 2 | 3839 | **0** | 0 |
| F3DEX.NoN 1.23 `efe4` (record `l_non`) | NONE | Donald Duck | 1983 | **0** | 0 |

Nine of ten games verify clean (26 935 tasks without a difference). **Gauntlet Legends fails with the same text that verifies in two other games**: its tasks differ in the RDP state words (DMEM 0xc0 .. 0xe8) and in IMEM 0 .. 0xc at the end, and 3502 of its tasks poll the RSP status more than the 30 000 times
the front-end allows. It passes a 0xc00-byte data segment (the survey noted 257 data images and 529 607 tasks in 55 s) and it was first read as a property of the (text, data) pair, not of the build record. **That was wrong**: the cause is outside the text and the data
([Registered](#registered-2026-10-16), "Gauntlet Legends"). The NONE rows verifying clean says that NONE is "not proven", not "wrong".

### Records

The four EXACT / NEAR F3DEX2 records of the table (scratch only, the form the tables use; `F3DEX2_DEAD` is the standard dead-range set and still needs the poison runs of [testing.md](testing.md) for any text that is registered for real):

```c

/* 9875370e17e420ab RSP Gfx ucode F3DEX.NoN   fifo 2.08  Yoshitaka Yas: EXACT, primary l_b_ex2_208 + []; ids ['9875370e17e420ab'] */
static const struct f3dex2_build l_b_c9875370e = {
    .head = 0x194, .copy = 0x210, .sync = 0x214, .segcopy = 0x220, .ef = 0x240, .ed = 0x24c, .tri1 = 0x7bc, .tri2 = 0x7b4, .vtx = 0x4f8, .mtx = 0x0f0, .geom = 0x1d8, .enddl = 0x1ec, .half2 = 0x200, .modvtx = 0xc80, .culldl = 0xc40, .branchz = 0xc68, .reject_mask = 0x70b0, .load_key = 0x6f36ef1549b31211ull };
HLE_UCODE(l_c9875370e_0) = { .id = 0x9875370e17e420abull, .name = "candidate c9875370e", .fam = &f3dex2_fam, .fam_data = &l_b_c9875370e, F3DEX2_DEAD };

/* 0bbec7c325e9cca8 RSP Gfx ucode F3DEX       fifo 2.06  Yoshitaka Yas: NEAR, primary l_b_oot + ['l_b_dk64']; ids ['0bbec7c325e9cca8', 'bc95641309d14ff7'] */
static const struct f3dex2_build l_b_c0bbec7c3 = {
    .head = 0x18c, .copy = 0x208, .sync = 0x20c, .segcopy = 0x218, .ef = 0x238, .ed = 0x244, .tri1 = 0x7b4, .tri2 = 0x7ac, .vtx = 0x4f0, .mtx = 0x0f0, .geom = 0x1d0, .enddl = 0x1e4, .half2 = 0x1f8, .modvtx = 0xc74, .culldl = 0xc34, .branchz = 0xc5c, .zex = 1, .reject_mask = 0x7070, .load_key = 0x549ebb163e63b994ull };
HLE_UCODE(l_c0bbec7c3_0) = { .id = 0x0bbec7c325e9cca8ull, .name = "candidate c0bbec7c3", .fam = &f3dex2_fam, .fam_data = &l_b_c0bbec7c3, F3DEX2_DEAD };
HLE_UCODE(l_c0bbec7c3_1) = { .id = 0xbc95641309d14ff7ull, .name = "candidate c0bbec7c3", .fam = &f3dex2_fam, .fam_data = &l_b_c0bbec7c3, F3DEX2_DEAD };

/* d8bde2727be1cf28 RSP Gfx ucode F3DEX.NoN   fifo 2.07  Yoshitaka Yas: EXACT, primary l_b_dk64 + []; ids ['d8bde2727be1cf28'] */
static const struct f3dex2_build l_b_cd8bde272 = {
    .head = 0x18c, .copy = 0x208, .sync = 0x20c, .segcopy = 0x218, .ef = 0x238, .ed = 0x244, .tri1 = 0x7b4, .tri2 = 0x7ac, .vtx = 0x4f0, .mtx = 0x0f0, .geom = 0x1d0, .enddl = 0x1e4, .half2 = 0x1f8, .modvtx = 0xc78, .culldl = 0xc38, .branchz = 0xc60, .reject_mask = 0x70b0, .load_key = 0xa8d17cd78a5b4e14ull };
HLE_UCODE(l_cd8bde272_0) = { .id = 0xd8bde2727be1cf28ull, .name = "candidate cd8bde272", .fam = &f3dex2_fam, .fam_data = &l_b_cd8bde272, F3DEX2_DEAD };

/* 1fdeaad6ac0e52f7 RSP Gfx ucode F3DEX       fifo 2.07  Yoshitaka Yas: EXACT, primary l_b_dk64 + []; ids ['1fdeaad6ac0e52f7', '48c7e653a6adcda8'] */
static const struct f3dex2_build l_b_c1fdeaad6 = {
    .head = 0x18c, .copy = 0x208, .sync = 0x20c, .segcopy = 0x218, .ef = 0x238, .ed = 0x244, .tri1 = 0x7b4, .tri2 = 0x7ac, .vtx = 0x4f0, .mtx = 0x0f0, .geom = 0x1d0, .enddl = 0x1e4, .half2 = 0x1f8, .modvtx = 0xc78, .culldl = 0xc38, .branchz = 0xc60, .reject_mask = 0x7070, .load_key = 0x4d947651fb1d5494ull };
HLE_UCODE(l_c1fdeaad6_0) = { .id = 0x1fdeaad6ac0e52f7ull, .name = "candidate c1fdeaad6", .fam = &f3dex2_fam, .fam_data = &l_b_c1fdeaad6, F3DEX2_DEAD };

/* 97989ee9c10ef7ad RSP Gfx ucode F3DEX       fifo 2.05  Yoshitaka Yas: NONE, primary l_b_oot + ['l_b_ex2_204']; ids ['97989ee9c10ef7ad'] */
static const struct f3dex2_build l_b_c97989ee9 = {
    .head = 0x18c, .copy = 0x208, .sync = 0x20c, .segcopy = 0x218, .ef = 0x238, .ed = 0x244, .tri1 = 0x7ac, .tri2 = 0x7a4, .vtx = 0x4e8, .mtx = 0x0f0, .geom = 0x1d0, .enddl = 0x1e4, .half2 = 0x1f8, .modvtx = 0xc6c, .culldl = 0xc2c, .branchz = 0xc54, .zex = 1, .reject_mask = 0x7070, .load_key = 0x9a8102d791623c41ull };
HLE_UCODE(l_c97989ee9_0) = { .id = 0x97989ee9c10ef7adull, .name = "candidate c97989ee9", .fam = &f3dex2_fam, .fam_data = &l_b_c97989ee9, F3DEX2_DEAD };

```

The estimate of what these would add: the four rows are 12.5 of the 34.4 points of RSP time that are left on the JIT (mean over 64 games), i.e. the mean game would move from 34.4 % to **21.9 %** of its graphics RSP time on the JIT, if all three games of rows 1 and 2 ran as the verified ones do
(Gauntlet Legends, 1.6 of the 4.7 points of row 2, does not: 10.9 points sure). The two NONE rows that verify clean (6, 7) would add 3.1 more once the probe has confirmed them; of the remaining 18.8 points 7.2 are registered builds that decline in the front-end and 11.6 other microcode.

## What the tool cannot do

- It sees the **code**, not the inputs: a layout that no aligned instruction reads (Conker's moved fog / texture words) stays unconfirmed; an attribute that no pair of references isolates is not decided by votes
  (F3DEX 1.x and F3D read theirs from signatures in the text instead); two knobs that share a value in every reference cannot be told apart without the handler (F3D's G_CULLDL mask is read from its handler).
- **It cannot prove that a rewritten routine computes the same thing** (Paper Mario's lighting loop; the one rewrite between the F3DEX 1.x versions, a call through a saved `ra` against a tail jump, is a pattern it knows): that is what the probe of the next phase is for, with the near-match tolerance. The tool's NEAR / NONE list is the work list of the probe.
- The dead ranges (`F3DEX2_DEAD` and the S2DEX / BG variants) are copied, not derived.
- Other microcode (Rej, L3DEX, S2DEX 1.x, Rare, Indiana Jones, Resident Evil 2's custom texts) is reported as "no family" (aligned fraction under 50 %) or NONE.

## Reproduce

```
tools/ucode_dump_run.sh ...                      # as in ucode-survey.md "Reproduce"; UDUMP_LOADS=1 also dumps the texts a G_LOAD_UCODE loads; 64 games, DUMPS/<set>/<game>/dump/
tools/derive_check.sh DUMPS                      # self-test, leave-one-out validation (fails on an unexplained mismatch), mutation check; about 10 s
python3 -I tools/derive_build.py derive DUMPS 33dc705a5e4d39ae          # one text: family, record, handler table, unexplained words
python3 -I tools/derive_build.py hot DUMPS STATS                        # STATS: the directory of tools/ucode_coverage_run.sh
python3 -I tools/derive_build.py emit DUMPS cand.c KEY...               # the C of the records, for a scratch build
```

## The C port (2026-10-17)

`src/hle/derive.c` is `derive_f2.py` / `derive_f1d.py` / `derive_lib.py` / `rsp_isa.py` in C, decision for decision (including the first-maximum and insertion-order choices of the Python dicts; one difference of the Python tool was removed: `derive_f2.derive` left the primary reference restricted to its own overlays (`set_active`), so a derivation depended on the ones before it; it resets them now). The F3DEX2 family, S2DEX 2.x and (since 2026-10-17) F3DEX 1.x and F3D give records and verdicts. The runtime needs the references without the microcode texts: `derive_refs.c`, written by `tools/derive_refs_gen.py`. **What is embedded:** per instruction word of each reference image (text + overlay segments) a 16-bit index into a table of one-way 64-bit hashes of the word's *shape* (the word with its immediate, branch offset or load/store offset masked, hashed; the whole word hashed for instructions without such a field) and the 16-bit immediate (a jump keeps its 26-bit target field); the build records (pcs, layout, flags, masks, extension table, load key); each reference's jump table (256 handler pcs); the word indices of the reference-only handlers (`G_SPECIAL_1`, `G_TRI4`) and the segment geometry. Shape equality was checked equal to the Python tool's `wkey` equality over every word of every reference at generation time. `tools/derive_refs_gen.py DUMPS src/hle/derive_refs.c --check` fails when a registered build changed and the file is stale (the cache's tool version contains a checksum of the data, so a stale file also retires old cache entries).

Equivalence (`tools/derive_c_check.sh DUMPS`; `derive_c_check.py` runs both tools and diffs, field by field: family, verdict, aligned and residual counts, primary reference, composite, near and reference-only categories, knob trouble, every field of the record, the extension table):

| check | result |
|---|---|
| every distinct text of the 64-game dumps (54 non-loaded texts), all references, family auto | 0 differences |
| every (text, data) pair of the dumps (786) with the production shortcut (the other families are skipped when F3DEX2 explains >= 60 % of the words with a verdict other than NONE) against all families | 0 differences |
| leave-one-out of every registered build (28: F3DEX2 14, F3DEX 1.x 9, F3D 5) | 0 differences |
| the mutation check of `derive_build.py mutate` (6 edits of Donkey Kong 64's text and jump table), both implementations | equal, each changes the result |
| ASan + UBSan + leak check over the 54 texts | clean |

Cost (`derive_c_check --time`, x86_64 desktop, `-O2`): F3DEX2 family only 1.2 to 1.6 ms, all three families 4 to 5 ms per text (the first pass of every alignment shares one position table of the unknown, and the longest-match search starts at the first position inside the range).

## Registered (2026-10-16)

The four candidates are in `src/hle/hle_f3dex2_dialects.c` (the records were taken from the tool's output; the jump tables of the five dumps and the three `load_key` values were re-read from the dumps by a second script, which agree field for field), after the ucode id
change that the same session made: the id is the FNV-1a-64 of the whole 4 KiB, so the pairs of ids in the table above are one id each now ([design.md](design.md#graphics-ucode-identification); `docs/options.md` lists the ids that changed).

| text | id | name in the table | verdict of the tool | native share, 3700 frames | `ARMLET_HLE_VERIFY=1`, 12 000 frames |
|---|---|---|---|---|---|
| F3DEX.NoN 2.08 | `9875370e17e420ab` | Banjo-Tooie, LEGO Racers, Star Wars Episode I Racer | EXACT | 1151 of 1152 / 2815 of 2816 / 2687 of 2688 | 5247 of 5248 / 9535 of 9536 / 7743 of 7744 tasks compared, 0 differences |
| F3DEX 2.06 | `0bbec7c325e9cca8` | Blues Brothers 2000, Harvest Moon 64 (Gauntlet Legends: below) | NEAR | 3711 of 3712 / 5567 of 5568 | 12 031 of 12 032 / 18 047 of 18 048, 0 differences |
| F3DEX.NoN 2.07 | `d8bde2727be1cf28` | Tonic Trouble | EXACT | 1599 of 1600 | 9087 of 9088, 0 differences |
| F3DEX 2.07 | `48c7e653a6adcda8` | (Donkey Kong 64's entry) Rocket | alias | 1727 of 1728 | 5887 of 5888, 0 differences |

(The first task of a run is never native: the counters are flushed before the result of the task in flight.) Rocket needed no entry: with the full-4 KiB id its tasks have Donkey Kong 64's id, and `ARMLET_HLE_STATS` counts them under that entry.

**What registering found.**

1. **`G_BRANCH_Z` of F3DEX 2.06 reads the screen z, not the clip w** (a derivation error; the tool's "flags" said clip w). The text of Blues Brothers 2000 has Ocarina of Time's triangle code (`zex`) but `lw 28(vertex)` in the branch (`lhu t9, 896(t9); lw t9, 28(t9)` at 0xc5c), not
   the `lh 6(vertex)` of the F3DZEX builds. The front-end derived the source from `zex && !gen204`, which no 3700-frame game run notices (149 of 1000 seeds of the synthetic `misc` case differed, nothing else did). `f3dex2_build.bz_z` says "zex, but the branch compares the screen z"; mutation 317 switches it
   off (36 of 300 `misc` seeds differ). `tools/derive_f2.py` now reads the branch instruction from the text and derives `bz_z` (the leave-one-out validation compares it: 75 fields now, `l_b_ex2_206` derived without its own table gives `bz_z = 1`).
2. **The records' reject masks** (`0x70b0` for the `.NoN` texts, `0x7070` for the 2.06) are read from the immediates of the two places that use them; mutations 318 / 319 / 320 (the other value in the 2.08.NoN / 2.07.NoN / 2.06 record) are caught by the synthetic `clip`, `tri` and `wild` cases
   (43 and 33 of 300 seeds on a 2.08.NoN and a 2.07.NoN base; 3, 3 and 13 of 1000 `clip`, `zneg`, `wild` seeds for the 2.06, which is the least observable one: the near-clipping masks differ in two bits).
3. **Dead ranges.** The three new builds use the standard F3DEX2 table (`F3DEX2_DEAD`, the 21 ranges of [design.md](design.md)). `ARMLET_HLE_POISON=1`, `ARMLET_DMEM_POISON=mask` and `=ring` (hash included) and `=all` (state, pictures and audio identical, the hash stream differs by design) on all seven games, 3700
   frames each: no change in any stream, so no range was added or removed. (The ranges are not proven *minimal* by this, only sufficient, as for the other builds.)
4. **The tool and the registry.** `tools/derive_lib.py` maps a registered id to a loaded text (`L...` dump) only through its load key now; a table entry that holds the id of a loaded-only text (S2DEX 2.06 / 2.05 [Safe]) is the FNV of the 4 KiB that follow the load address, which is imaged differently from a task start and made the validation report six pcs
   off by four. `hle_core.c` logs an error when two entries have one id.

### Gauntlet Legends

Gauntlet Legends is the same text as Blues Brothers 2000 and Harvest Moon 64 and is not a different data image problem. Two of its graphics tasks run natively (the first two) and match the JIT; every later task declined (3118 of 3392 in 3700 frames: "too many SP_STATUS polls") or, before
the fix, ran natively and differed (207 of 209).

- Its display lists contain a `G_DL` branch to itself (`de010000 e0211b58` at 0x211b58): the microcode spins on it until the CPU patches the list. Those tasks are the 3118 declines (30 000 commands, the JIT halts at 32 767 polls): the front-end rightly gives them to the JIT.
  parallel-rsp's `run_rsp_task` cuts such a task with its status poll time-out and leaves the RSP running ("graphics ucode idles at 0xfc8" waiting for the CPU, `parallel.cpp`). The 0xc00-byte data segments are not the cause: the first 700 frames pass 0x800 bytes and
  still differ; the 0xc00 images come later in the game (`seq.txt` of the survey run: 735 842 tasks with 0x800 bytes, 541 268 with 0xc00, 1541 more with the OS yield flag set, which are never offered to a front-end) and are the saved DMEM states that the game hands the microcode (255 distinct images).
- **The time-out is the cause.** After the first poll time-out parallel-rsp sets `SP_STATUS_TIMEOUT` to 16 for good ("from now on, wait 16 times, not 0x7fff"), and the JIT then halts a task at its 16th `mfc0 SP_STATUS` into one register (one per command at the loop head) and finishes it in the next call. The
  end state of a call is a part of a task; the 207 "differences" were the native run of the whole task against the JIT's first sixteen commands (DMEM 0xc4 .. 0xeb: the last scissor, texture rectangle and `G_RDPHALF` words that the JIT had not reached yet; IMEM 0 .. 0xc). The front-ends' poll limit (30 000, for 0x7fff) does not
  model this, and it did not matter for any other game because none of them polls for the CPU.
- **The fix is general, not per (text, data)**: `native_run` (`hle_core.c`, all families) declines a task when the snapshot of the JIT state has `sp_status_timeout != 0x7fff` ("the JIT's status poll timeout has dropped (an earlier task waited for the CPU)"). Gauntlet Legends: 3700 frames, `ARMLET_HLE_VERIFY=1`: 2 tasks
  native, 0 differences, 3389 declined (before: 209 native, 207 differences); hash, state, screenshots and audio identical to the reference; the mutant (mutation 316) shows the 7 differences again in 700 frames. Nothing else changed: the other 22 games of the regression set were identical before and after.
  The cost is that Gauntlet Legends stays on the JIT (its mean native share was 0 before, too).


## With the native Rej builds (merge of ucode-probe and native-rej, 2026-10-17)

`derive_refs.c` is generated from the hand-written tables that `tools/derive_lib.py` reads back. Until 2026-10-18 the F3DEX 2.03 build and the Rej builds (F3DLX.Rej, F3DFLX.Rej, F3DEX.Rej, the XBUS F3DLX.Rej), registered in `hle_rej.c`, were **not** among them (different microcodes: 20-byte vertex records, other triangle code; hot list rows 8, 17, 19, 21, 24); they are references now, with a Rej extension of the derivation: "Rej as a derivation family" below. The static ids of `hle_rej.c` and the dynamic registry cannot collide: a derived build is found after the registered ones and `armlet_hle_find` takes the first, so a registered text is never re-derived.

## S2DEX records that run (2026-10-17)

The S2DEX 2.x front-end is data-driven now ([ucode-s2dex2.md](ucode-s2dex2.md) section 8): the record has two more pc fields, `ldtx_spr` and `ldtx_rect` (`PC_FIELDS` of `derive_f2.py`, `PC_LDTX_SPR` / `PC_LDTX_RECT` in `derive_int.h`; 0 for the
other builds), the pcs where `G_OBJ_LOADTXTR` goes on for LDTX_SPRITE and LDTX_RECT. They are mapped like any handler pc that is not a jump table target: the instruction of the reference at that pc, found in the unknown text (`map_pc` / `conv_pc`,
a unique run of six instruction shapes or the aligned word; the overlay A code lies at text + IMEM pc - 0x80 like the resident code). The leave-one-out validation compares 77 fields now; the three S2DEX records come out equal to the hand-written ones
field by field (2.08 from the 2.06 reference: `ldtx` 0x6e0 -> 0x6e8 ...; 2.06 from 2.08: NEAR, one reference-only word; 2.05 [Safe] from 2.06: EXACT, `out_bytes` 792 and `pipe_sync` from the knob / pattern evidence as before). `dbuild_make` builds a
runnable S2DEX build from such a record (the dialect commands run on the primary reference's `ext_cmd`; a handler the primary reference marks absent, 0 or 0xffff, may be absent; no reject mask is needed without `G_TRI1`), and the ucode cache stores the
dialect table (format 2).

**An S2DEX xbus record by hand (2026-10-18).** StarCraft 64's S2DEX xbus 2.08 (`3ee636569037de5b`) derived NONE (108 unexplained words, 48 of them the transport's: another start-up, the flush routine at 0x648 with the ring at 0x4c0 .. 0xee0, buffer knobs that encode differently)
and is registered by hand now (`l_b_s2_xbus`, [ucode-s2dex2.md](ucode-s2dex2.md) section 9), so it is a reference for the other xbus derivations. The start-up's `addi s7, zero, ring_start` is searched from 0x400 up (was 0x800: StarCraft's ring starts at 0x4c0). The ring's end and the room
margin (`ring_end` 0xee0 / 0xf10, `ring_gap` 224 / 176) are layout fields of the front-end, not of the derivation record: a derived xbus build takes them from its primary reference when that one has the transport (`dbuild_make`), else the Rej / F3DEX2.NoN defaults (0), so the
S2DEX xbus family derived from `l_b_s2_xbus` is right (the flush routine's constants `addi t3, s7, -3808` and `addi t3, t3, -224` are in the aligned words, but nothing checks them against the primary's: a text whose flush differs from its primary's has unexplained words).

**The data image of a loaded text.** `tools/ucode_dump_shim.c` writes a `L...` dump's `.data` as 0x180 zero bytes followed by RDRAM from the `G_RDPHALF_1` address; that address is the image of DMEM **0** (`G_LOAD_UCODE` fills DMEM 0x180 .. from
address + 0x180), so the dump's bytes sit 0x180 too high. `derive_lib.Ucode`, `derive_c_check.c` and `armlet-probe-replay` drop the first 0x180 bytes of a loaded dump (the shim is unchanged, so the existing dumps stay valid). Before, the jump table
and the overlay descriptors of the Kirby 64 and Ocarina of Time S2DEX references were read from the wrong bytes; the derivations came out right anyway (the pcs were mapped by alignment, not by the table), and nothing else in the leave-one-out,
the 54 texts or the 786 pairs changed (`derive_c_check.sh`: 0 differences). At run time the probe reads the image the same way (`armlet_probe_note_load`).

Results (`tools/derive_check.sh`, `tools/derive_c_check.sh`, `armlet-probe-replay --sync`): leave-one-out no unexplained mismatch, 77 fields; C against Python 54 texts / 28 leave-one-out / 6 mutants / 786 pairs, 0 differences;
`derive_refs.c` regenerated, 14 F3DEX2-family references in step with the tables.

## F3DEX 1.x records that run (2026-10-17)

The F3DEX 1.x front-end is data-driven now ([native-f3dex.md](native-f3dex.md) section 3: `struct f3dex1_build`, one record per registered text), and `derive_f1d.derive_f1` (`derive.c` `derive_f1`, decision for
decision) derives all of it. The record's fields, in the tool's names: `lay.*` (`F1_LAY_FIELDS`: 29 addresses and 7 constants), `pc.*` (`F1_PC_FIELDS`: `head`, `nop` and the 22 handlers), `reject_mask`,
`flag.non`, `flag.dlx`, `flag.culldl_off`, `load_key`.

**How each part is derived.**

- **The layout and the reject mask** are knobs as in F3DEX2 (step 3). F3DEX 1.x addresses its state words relative to `$29` = 0x110 (`lw at, 4(sp)` is the geometry word 0x114), so the pairing adds 0x110 to the
  offset of a load / store with base `$29` for a reference of this family (the C references carry a "relative to `$29`" bit in their shape info, generator version 2). Turok's class tables at 0xc4 come out of
  its dispatch code (`lh v0, 0xc4(v0)`). The 7 constants (clamps, texgen bias, sign vector, clip ratio, planes and masks, texgen polynomial) live in the data segment, no operand names them: copied from
  the primary reference. `head`, the pc where the start-up code reaches the loop, is copied too: it lies in the head code that the start-up code DMAs to IMEM 0 from the text, which the image the tool aligns
  has only for the 0xf80-byte texts (the last 0x80 bytes of the 4 KiB).
- **The handler pcs** by alignment (`map_pc`, from the best reference that has the handler and whose mapped pc the unknown's own handler tables lead an opcode of the GBI to; `jt0` / `jt2` the derived ones).
  Mapping by opcode, as F3DEX2 does, would give the wrong handler for the 0.95 numbering. A handler no reference has (0.95's G_PERSPNORM and G_QUAD) is 0: the derived build declines the command.
- **LX**: the words that only references of one value explain (the F3DLX triangle code and flush: about 330 words against at most 9 in every leave-one-out); undecided under 8 words or a 4 : 1 margin (NONE).
- **NoN** is read from the text: the NoN vertex code masks the near-plane bit off one vertex (`andi rX, rY, 0x703`) and puts the signs of w into the flags (`andi rX, rX, 0x8000 ; srl rX, rX, 13` and `srl 9`).
  Both signatures make it NoN, neither (with the 0x707 mask) not; one without the other is undecided (NONE): the mutant "0x707 -> 0x703 without the NoN code" is caught by that. The old pair votes could not decide
  it when the leave-one-out removed the build's pair (1.21 and 1.23 are one record there).
- **The G_CULLDL form**: the index form looks the two vertex numbers up in the vertex table (`lhu rX, vtab(rY)` in the handler's first eight words); otherwise byte offsets.
- **The primary reference** is the one that leaves the fewest words unexplained after the composite, counting its own words the unknown lacks (a NoN reference has 8 words of w-sign code a non-NoN text lacks,
  which the old choice by unexplained words alone left "deleted").
- **Two understood differences.** (1) The display-list window refill of 1.21 / 1.22 returns through a saved `ra` (`add s5, zero, ra ; ... jal 0xfd4 ; ... jr s5`) where 1.23 tail-jumps (`j 0xfd4`); the only
  difference is the value left in `$21`, the overlay entry register, which the code sets before it reads it. The tool recognises both directions (within 12 words of the alignment anchor, the same target) and
  counts them as explained: 1.21 against 1.23 is EXACT now. (2) Code guarded by a geometry-mode bit that the front-end declines (`lw rA, geom ; andi rA, rA, BIT ; beq rA, zero, END`, END the first aligned word
  after the run, BIT outside `F3DEX_G_DEFINED`): near-class `geom-guarded` (NEAR). Iggy's Reckin' Balls' F3DEX.NoN 1.22 `dff3` has 14 such words in its vertex loop (bit 0x4000: a test of the light records).

**Results.** Leave-one-out (`tools/derive_check.sh`): the table above, 8 of 9 records equal in all 65 fields (7 EXACT), F3DEX 0.95 with 4 expected differences (the three handlers and the CULLDL form that
follows from one). The unregistered texts: Donald Duck's F3DEX.NoN 1.23 `efe4` **EXACT** (from 1.23 with NoN's code by composite; its pcs are those its tables name), Iggy's Reckin' Balls' NoN 1.22 `dff3`
**NEAR** (geom-guarded 14). Mutation check (`derive_build.py mutate`): five F3DEX 1.x edits of 1.21's text and data, each changes the result (reject mask 0x7050 in G_CULLDL: conflict, NONE; the
near-plane mask without the NoN code: NoN undecided, NONE; one `$29`-relative geometry load moved: conflict; the refill's `jr s5` -> `jr s4`: unexplained; the handler table cell of G_TRI1: another pc).
C against Python (`tools/derive_c_check.sh`): 54 texts, 28 leave-one-out, 11 mutants, 786 pairs, 0 differences; ASan + UBSan over the 59 distinct texts clean; `derive_refs.c` regenerated (23 references
in step with the tables, `armlet-probe-replay --sync`). The probe of these records: [testing.md](testing.md), "F3DEX 1.x data-driven".

## F3D records that run (2026-10-17)

The F3D front-end is data-driven now ([ucode-f3d.md](ucode-f3d.md) section 15: `struct f3d_build`, `f3d_bld.h`), and `tools/derive_f3d.py` (`derive.c` `f3_derive`, decision for decision) derives all
of it. The record's fields, in the tool's names: `lay.*` (`F3_LAY_FIELDS`: 26 addresses the code names, the two dispatch tables and G_TRI4's table, 7 constants), `pc.*` (`F3_PC_FIELDS`: `head`, `nop`, `brk`
and 20 commands), `reject_mask`, `clip_mask`, `culldl_mask`, `flag.flags_variant`, `flag.vtx_wr`, `flag.gp_end`, `flag.tri_wr`, `flag.popmtx_stale`, `flag.xbus`, `flag.vptr_mul`, `load_key`.

**How each part is derived.** No two references differ in one attribute alone, so nothing is decided by pair votes; what alignment cannot show is read from the unknown's own text (lesson of F3DEX 1.x: two
signatures where a half-change is possible; any signature that does not decide makes the verdict NONE).

- **The layout and the reject / clip masks** are knobs as in F3DEX 1.x (F3D also addresses its state relative to `$29` = 0x110: `lb v0, 74(sp)` is Tetrisphere's G_DL depth byte 0x15a). G_VTX's return slot
  `vret` (0x106 in 2.0D and 2.0G, 0x104 in 2.0H) is a knob too: it is the one dead range that moves between the builds. The geometry buffer comes from `addi s6, zero, X` in the loop head code (overlay 0, read
  through the DMA table: the head code is at text + 0xf50 .. 0xf88 depending on the build, not where the 4 KiB image puts IMEM 0).
- **The tables** from the two dispatchers (`andi v0, v0, 0x1fe ; lh v0, jt0(v0)` and `andi v0, v0, 0xfe ; lh v0, jt2(v0)`): 0xc4 and 0x76 (0x74 in 2.0H). A knob would not do: 0x76 is also the clip ratio's address.
- **The handler pcs** by alignment (`map_pc`, from the best reference that has the handler), accepted only when the unknown's own tables lead a GBI opcode there (class 0: 0x00 .. 0x09, class 2: 0xb0 .. 0xbf)
  and no other field took it: SM64's G_PERSPNORMALIZE handler maps onto the 2.0H builds' RDPHALF store, which they have instead (their perspective norm is G_MOVEWORD 0x0e). `head` is the target of the loop's
  back-branch 16 bytes after `nop` (`bgtz gp, head`, or Wave Race's `bne gp, k1, head`); `brk` is the BREAK of overlay 3 (read from the RDRAM behind the text through the DMA table).
- **The attributes**: the loop form (`gp_end`: the back-branch on `gp`/`k1` and no `addi gp, gp, -8` in the head code), Wave Race's G_VTX encoding (`lbu rX, -6(k1) ; srl rX, rX, 1` instead of `srl at, at, 4`),
  the index scale (three `sll rX, rX, s` before three `addi rX, rX, vtx_base` in G_TRI1's handler or the routine it calls: 4 or 8), Wave Race's triangle code (neither `andi .., 0x1000` (front-face cull) nor
  `andi .., 0x200` (the flat-shading choice); one without the other is undecided), Pilotwings' clip flag word (the F3DEX.NoN signature: `andi 0x703` and the w-sign shifts), G_POPMTX's stale flag (a byte store to
  the light count word among its first words), XBUS (`addi rA, zero, 2 ; mtc0 rA, DPC_STATUS` and DPC_START pointed at the output buffer in DMEM; the FIFO builds clear the bit instead), the G_CULLDL
  mask (`ori v0, zero, MASK` in its handler; SM64's is 0xffff, the reject mask 0x7070), G_TRI4's table (`lbu rX, TAB(rX)` in its handler).

**Results.** Leave-one-out (`tools/derive_check.sh`, table above): every F3D verdict is NONE (each build has code no other has: Wave Race 80 words, Mischief Makers' XBUS flush 48, GoldenEye's G_TRI4 17,
Tetrisphere 2, Pilotwings and SM64 only reference-only words), and the records are equal in all 70 fields for SM64, Pilotwings, Mischief Makers and Tetrisphere; Wave Race's G_TRI1 / quad and GoldenEye's
G_TRI4 are "expected" (no other reference has the handler; the derived build declines those commands). With its own record among the references every registered text derives EXACT with its record
(`armlet-probe-replay --self`). Mutation check: six F3D edits of SM64's text and data, each changes the result (G_CULLDL's `ori` mask: derived 0x7070; the near-plane mask without Pilotwings' code:
undecided, NONE; one `$29`-relative geometry load moved: conflict; one of G_TRI1's shifts: undecided; the front-face cull test alone removed: undecided; the table cell of G_TRI1: another pc). C against
Python (`tools/derive_c_check.sh`): 54 texts, 29 leave-one-out, 17 mutants, 786 pairs, 0 differences; `derive_refs.c` regenerated (generator version 3, tool version 3), 29 references in step with the tables.

**The unregistered F3D texts** (all NONE): Blast Corps' 2.0D `2f41` (32 unexplained words: an output flush that accumulates the lists in RDRAM without handing them to the RDP, a transport the front-end
does not have), its `1d12` (51 % aligned: other code) and the 2.0H texts of Wipeout 64 (`968d`) and Glover (`9d0d`), which are the Sprite2D microcode (24 and 22 % aligned). GoldenEye 007's 2.0G `538f`
derives with Pilotwings' clip flag word and masks, the 2.0H RDPHALF numbering and depth byte and every handler but G_TRI4, its main triangle command: as a derived build it would decline nearly every
task, so it is registered by hand instead, with G_TRI4 as a new command of the front-end ([ucode-f3d.md](ucode-f3d.md) 15, testing.md "F3D data-driven").


## Rej as a derivation family (2026-10-18)

The Rej builds ([ucode-rej.md](ucode-rej.md); `hle_rej.c`, `rej_dmem.h`) are references now, so an unknown Rej text is aligned with them (and the rest of the family) and gets a record that `dbuild_make` can build; before, every Rej text outside the four registered ones derived NONE with ~40 % of its words aligned.

**References.** `tools/derive_lib.py` reads `hle_rej.c`, `rej_dmem.h` and `rej_fx.h` too (the `__attribute__((unused))` of the layouts, the designated-initialiser macro `REJ_FIFO_FRONT` and the `REJ_*` bits are understood), so six records join the F3DEX2 family: the four Rej builds (`l_b_rej_xbus`, `l_b_rej_203`,
`l_b_rej_flx`, `l_b_rej_206`), F-Zero X's F3DEX 2.03 (`l_b_ex2_203`) and the F3DEX.NoN xbus 2.08 text Excitebike 64 loads (`l_b_ex2_208n_xbus`): 20 F3DEX2-family references, 35 in all (`armlet-probe-replay --sync`: 0 out of step). `derive_refs.c` was regenerated (`--check` passes).
The Rej extension kinds start at `K_REJ_BASE` 0x60 (S2DEX's at `K_EXT` 0x40), which is how the derivation tells the two dialects apart (`DERIVE_K_REJ`, a static assertion in `dbuild.c`).

**What is derived** (`derive_f2.py` and `derive.c`, decision for decision): the extension table's seven pcs (the primary reference's table mapped through the jump tables, as S2DEX's), the layout (knobs as before), the new field `lay.rej_flags` (`derive_rec.rej_flags`, `ucache_entry.rej_flags` in the old padding),
and the Rej-specific rules of [ucode-rej.md](ucode-rej.md) section 10: one vote per bit of `rej_flags` over the words of the regions it owns; a reference of the other class explains no word of the vertex, triangle, lighting and clip code, and a Rej reference only the ones whose bits agree; the short runs of the primary
reference's variant code that the unknown lacks are "a feature the unknown lacks" (`extra`, so NEAR), not "an instruction missing" (`deleted`, NONE); the light-setup scale is the lane of the constant vector at `const30` that holds 0x0100 (when no operand anchors it). One more rule, for the pair of re-entry addresses of the
start-up code (`ra` = 0x1080 up to the 2.04 generation, 0x1084 after): a relocation, not the knob "mvp" that it had been read as (that ambiguity alone made Mario Party 3's and NBA Jam's texts NONE). `xbus`, `ring_start`, the dead ranges and the ring are **copied from the primary reference** in `dbuild_make`
and not derived (the flush code that depends on them is aligned, so another transport is unexplained words and NONE); the XBUS derivation is separate work. `DERIVE_TOOL_VERSION` 5.

**The leave-one-out table** above has the six new builds; all verdicts NONE (every Rej text differs from its neighbours in variant code, no record comes out EXACT from the others), and the new mismatches are the understood ones: `rej_flags` bits that no remaining reference shows (203's LX lighting, flx's FLAT_A / LIT_FLX, 206's missing rounding), `no_lit`
of 2.06, `light_scale` of 2.06 (lane 7; the hand record's 0x1c6 is a copy, the build lights nothing), and for the two XBUS builds `out_bytes` / `out_toggle` (no output buffers). Unconfirmed: the F3DFLX layout's `movemem_tab` / `moveword_adj` in both directions, `light_scale` / `out_toggle` as before.
**Every earlier row is field for field as before** (79 fields now with `lay.rej_flags`); what the new references changed in those rows (composite and counts only): Paper Mario's light-positional words 169 -> 168 (`l_b_ex2_203` explains one word), Conker's F3DEXBG 202 -> 194 unexplained, `init` 2 -> 1, reference-only 203 -> 200 (also needs `l_b_ex2_203`,
`l_b_rej_flx` besides `l_b_ex2_208`), Kirby's S2DEX 2.08 also needs `l_b_rej_xbus` instead of `l_b_bg`; verdicts unchanged. In the hot list the five registered Rej texts (rows 8, 17, 19, 21, 24) are "SAME TEXT as registered" now, and three S2DEX / L3DEX rows lose 4 or 5 unexplained words to the Rej references
(rows 5: 344 -> 339, 12: 302 -> 297, 27: 331 -> 327 with 4 `light-small`).

**The recount's Rej texts** (dumps of the not-coded set; `armlet-probe-replay`, no `--force` needed):

| text | games | family | verdict | unexplained / reference-only | primary + composite | `rej_flags` | probe |
|---|---|---|---|---|---|---|---|
| `9332f30c` F3DLX.Rej fifo 2.08 | Mario Party 3 | f3dex2 (497 of 1024 words align; the rest is S2DEX's overlay) | **NEAR** | 0 / `extra` 10 | `l_b_rej_203` + `l_b_rej_xbus` (67 words) | `REJ_YQ` | 28 of 28 equal, latched |
| `36f10dc3` F3DLX.Rej fifo 2.05 (loaded by `G_LOAD_UCODE`) | NBA Jam 2000, NFL Blitz | f3dex2 (732) | **NEAR** | 0 / `extra` 10 | `l_b_rej_203` + `l_b_rej_xbus` (52) | `REJ_YQ` | 28 of 28 equal, latched |
| `23cb9e85` F3DEX.Rej fifo 2.08 | ECW Hardcore Revolution | f3dex2 (786 of 1024) | NONE | 22 (triangle setup 20, vertex 2) / `deleted` 21 | `l_b_rej_206` | 0 | not admitted; with `ARMLET_PROBE_NONE_X` 7 of 28 canned tasks differ |
| `3c51d836` F3DLX.Rej 1.21 | Air Boarder 64, ISS 64 | **f3dex1** (425 of 1024 with F3DEX 1.21, 25 with F3DLX 1.21; f3dex2 0.025) | NONE | 585 / 50 + 437 | `l_b_121` + `l_b_lx121` | - | no candidate |

Mario Party 3's and NBA Jam's two texts differ from F3DLX.Rej 2.03 in the lighting only (the F3DEX2 loop of the 2.08 xbus text, scale in lane 3 or 7 of the constant vector: `rej_flags` loses `REJ_LIT_LX`, the 10 words of 2.03's unsigned-multiply loop are the `extra`), and ECW's text is F3DEX.Rej 2.06
with the 2.08 generation's lane layout in the triangle setup (20 words with other element selectors that no reference has: `v30[15]` where 2.06 has another lane); Air Boarder's is Rej in the F3DEX 1.x generation (no F3DEX2 skeleton: the command loop, the state commands and the matrix code align with F3DEX 1.21): the Rej extension exists in the F3DEX2 front-end only, so it stays on the JIT.
