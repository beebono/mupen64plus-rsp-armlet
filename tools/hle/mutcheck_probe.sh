#!/bin/bash
# The probe against deliberate errors (docs/testing.md, "The probe"). DUMPS: the survey's dump directory. Needs out/native-tools{,-hmutN}/armlet-probe-replay
# (scripts/build.sh native --tools [--hmut=N]).
#   340 reject mask of the derived record flipped (0x7070 <-> 0x70b0)   -> FAIL on every F3DEX2 text
#   341 bz_z inverted                                                    -> FAIL where zex without gen204 (OoT, F3DEX 2.06)
#   342 G_TRI1 pc + 8                                                    -> FAIL everywhere
#   343 zex inverted                                                     -> FAIL on the builds that have it or lack it
#   344 out_bytes - 8 (the RDP buffer size)                              -> FAIL everywhere
#   350 native stream: shade base +1 level   351: an edge bit flipped   (with --tol: 350 passes tolerant, 351 fails)
#   352 vertex colour +1 in DMEM            353: +3                     (with --tol: 352 passes tolerant, 353 fails)
set -e
D=${1:?dump directory}
R=$(cd "$(dirname "$0")/../.." && pwd)
T=()
for id in 1fdeaad6ac0e52f7 0d3f849c8dd309c4 c734403d56034719 d576cfcfed9d9c18 5dce3244420ce715 e857446754069ab5 9875370e17e420ab 0bbec7c325e9cca8 d8bde2727be1cf28 d2d1f9f2e7dfe1ab; do
    for f in "$D"/*/*/dump/$id.*.text; do [ "$(stat -c %s "${f%.text}.data")" = 2048 ] && { T+=("$f"); break; }; done   # (a data image of 2 KiB: the pristine one, not a saved DMEM state)
done
run() { printf '%s\0' "${T[@]}" | xargs -0 "$1" --force "${@:2}" 2>&1 | grep '^PROBE' | sed 's/.*outcome=\([A-Z_]*\).*/\1/' | sort | uniq -c | tr '\n' ' '; echo; }
echo "unmutated:        $(run "$R/out/native-tools/armlet-probe-replay")"
for m in 340 341 342 343 344; do echo "mutation $m:       $(run "$R/out/native-tools-hmut$m/armlet-probe-replay")"; done
for m in 350 351 352 353; do
    echo "mutation $m strict: $(run "$R/out/native-tools-hmut$m/armlet-probe-replay")"
    echo "mutation $m --tol:  $(run "$R/out/native-tools-hmut$m/armlet-probe-replay" --tol)"
done
