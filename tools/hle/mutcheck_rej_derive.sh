#!/bin/bash
# The Rej records of the derivation against deliberate errors (docs/testing.md, "Rej as a derivation family"). DUMPS: the survey's dump directory (with the recount's
# Mario Party 3 and NBA Jam 2000 dumps). Needs out/native-tools{,-hmut470,-hmut471}/armlet-probe-replay (scripts/build.sh native --tools [--hmut=N]).
#   470 the derived rej_flags with REJ_YQ flipped (dbuild_make)                              -> FAIL on every Rej build
#   471 the light-setup scale lane of a Rej record swapped (0x1c6 <-> 0x1ce)                  -> FAIL where the build lights (all but F3DEX.Rej 2.06)
set -e
D=${1:?dump directory}
R=$(cd "$(dirname "$0")/../.." && pwd)
pick() { for f in "$D"/*/*/dump/$1*.text; do [ -e "$f" ] && [ "$(stat -c %s "${f%.text}.data")" = 2048 ] && { printf '%s\0' "$f"; return; }; done; }
run() {   # $1 binary, $2 mode: --self (the registered Rej builds with their own record) or --force (the derived ones)
    { for id in 8b0a3e32 7de803fd c750be26 95129dc7; do [ "$2" = --self ] && pick $id; done
      [ "$2" = --force ] && { pick 9332f30c; printf '%s\0' "$D"/*/*/dump/L6f10dc3a*.text; } ; } |
        xargs -0 "$1" --force $([ "$2" = --self ] && echo --self) 2>&1 | grep '^PROBE' | sed 's/.*outcome=\([A-Z_]*\).*/\1/' | sort | uniq -c | tr '\n' ' '; echo
}
for m in "" 470 471; do
    b=$R/out/native-tools${m:+-hmut$m}/armlet-probe-replay
    echo "mutation ${m:-none}: registered Rej texts (their records) $(run "$b" --self)  derived (Mario Party 3, NBA Jam 2000) $(run "$b" --force)"
done
