#!/bin/bash
# The probe on the F3DEX 1.x texts against deliberate errors in the derived record (docs/testing.md, "F3DEX 1.x data-driven"). DUMPS: the survey's dump directory. Needs
# out/native-tools{,-hmutN}/armlet-probe-replay (scripts/build.sh native --tools [--hmut=N]). The texts: the seven registered builds the derivation makes EXACT (with --force: derived without
# their own record) and the two unregistered ones (Donald Duck's F3DEX.NoN 1.23, Iggy's Reckin' Balls' F3DEX.NoN 1.22).
#   404 the reject mask's near-plane bits flipped (0x7070 <-> 0x7030)   -> FAIL on all
#   405 the NoN attribute inverted                                       -> FAIL on all
#   406 G_TRI1's pc + 8                                                  -> FAIL on all (the triangle cases decline)
#   407 the vertex table 2 bytes lower (the 0.95 place)                  -> FAIL on all
#   408 the LX attribute inverted                                        -> FAIL on all
#   409 the G_CULLDL form inverted                                       -> FAIL on all
set -e
D=${1:?dump directory}
R=$(cd "$(dirname "$0")/../.." && pwd)
T=()
for id in 208a567e9aead5ea fd5a31ccf6d40b62 047252e810e35d56 7502cfb7098eeffb 19bd8c825b9b3f72 0a7cc6c93363c663 cc09096cb6a48e7b; do
    f=$(find "$D" -name "$id.*.text" | sort | head -1); [ -n "$f" ] && T+=("$f")
done
C=()
for id in efe4b8f339a9ebae dff34c7560eae754; do
    f=$(find "$D" -name "$id.*.text" | sort | head -1); [ -n "$f" ] && C+=("$f")
done
run() {
    { printf '%s\0' "${T[@]}" | timeout 600 xargs -0 "$1" --force; printf '%s\0' "${C[@]}" | timeout 600 xargs -0 "$1"; } 2>&1 | grep '^PROBE' | sed 's/.*outcome=\([A-Z_]*\).*/\1/' | sort | uniq -c | tr '\n' ' '; echo
}
echo "unmutated:   $(run "$R/out/native-tools/armlet-probe-replay")"
for m in 404 405 406 407 408 409; do echo "mutation $m: $(run "$R/out/native-tools-hmut$m/armlet-probe-replay")"; done
