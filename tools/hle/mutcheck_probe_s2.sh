#!/bin/bash
# The probe on the S2DEX 2.x texts against deliberate errors in the derived record (docs/testing.md, "The probe"). DUMPS: the survey's dump directory. Needs out/native-tools{,-hmutN}/armlet-probe-replay
# (scripts/build.sh native --tools [--hmut=N]). The texts: Worms' 2.08 and Resident Evil 2's 2.06 (task starts), Kirby 64's 2.06 and Ocarina of Time's 2.05 [Safe] (loaded by G_LOAD_UCODE).
#   400 the continuation pc of LDTX_RECT (G_OBJ_LOADTXTR) + 8          -> FAIL on all four (the s2ldtx cases decline)
#   401 out_bytes + 8 (the RDP buffer size)                             -> FAIL on all four
#   402 pipe_sync inverted                                              -> FAIL on all four (OoT's lacks it, the others gain it)
#   403 the dialect handlers from 0x184 on 8 bytes lower (the 2.06 shift) -> FAIL on all four
set -e
D=${1:?dump directory}
R=$(cd "$(dirname "$0")/../.." && pwd)
T=()
for id in b228360be43c0447 5f33af89e713878b Lf535d1683a57fcb L748556461d85ab3; do
    f=$(find "$D" -name "$id.*.text" | sort | head -1); [ -n "$f" ] && T+=("$f")
done
run() { printf '%s\0' "${T[@]}" | timeout 600 xargs -0 "$1" --force "${@:2}" 2>&1 | grep '^PROBE' | sed 's/.*outcome=\([A-Z_]*\).*/\1/' | sort | uniq -c | tr '\n' ' '; echo; }
echo "unmutated:   $(run "$R/out/native-tools/armlet-probe-replay")"
for m in 400 401 402 403; do echo "mutation $m: $(run "$R/out/native-tools-hmut$m/armlet-probe-replay")"; done
