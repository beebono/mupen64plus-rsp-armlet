#!/bin/bash
# The probe on the F3D texts against deliberate errors in the derived record (docs/testing.md, "F3D data-driven"). DUMPS: the survey's dump directory. Needs
# out/native-tools{,-hmutN}/armlet-probe-replay (scripts/build.sh native --tools [--hmut=N]). The texts: the five registered builds, derived with their own record among the references (--self:
# EXACT; their leave-one-out derivations are NONE, docs/ucode-derive.md "F3D records that run"), so every mutation meets the build it was made for.
#   410 the reject mask's near-plane bits flipped (0x7070 <-> 0x7030)   -> FAIL
#   411 the clip flag word's variant inverted (Pilotwings <-> SM64)      -> FAIL
#   412 G_TRI1's pc + 8                                                  -> FAIL (the triangle commands decline)
#   413 the class-2 table 2 lower (the 2.0H place)                       -> FAIL (the opcodes lead to other commands)
#   414 the triangle index scale 4 <-> 8                                 -> FAIL
#   415 the G_CULLDL mask 0xffff <-> 0x7070                              -> FAIL
#   416 G_POPMTX's stale flag inverted                                   -> FAIL
#   417 the G_MOVEWORD table 2 lower                                    -> FAIL
#   418 Wave Race's triangle code inverted                               -> FAIL
#   419 the loop form inverted                                           -> FAIL (every task declines)
set -e
D=${1:?dump directory}
R=$(cd "$(dirname "$0")/../.." && pwd)
T=()
for id in 94c7cfdd2c872523 3e9b9c3a6ebde1e0 fb483893c5b0ad3d 4212d63f6e483348 88fa3c0aa5315997; do
    f=$(find "$D" -name "$id.*.text" | sort | head -1); [ -n "$f" ] && T+=("$f")
done
run() {
    printf '%s\0' "${T[@]}" | timeout 600 xargs -0 "$1" --self 2>&1 | grep '^PROBE' | sed 's/^PROBE \(........\).*outcome=\([A-Z_]*\).*/\1:\2/' | tr '\n' ' '; echo
}
echo "unmutated:   $(run "$R/out/native-tools/armlet-probe-replay")"
for m in 410 411 412 413 414 415 416 417 418 419; do echo "mutation $m: $(run "$R/out/native-tools-hmut$m/armlet-probe-replay")"; done
