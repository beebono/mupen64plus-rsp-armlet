#!/bin/bash
# Builds and runs rdp_defer_fuzz.c (the model-based test of src/rdp_defer.c) natively: the real module must show no difference, and each of the
# deliberately broken variants (-DARMLET_RDP_MUTATE=1..5) must be caught.
#   tools/equiv/rdp_defer_fuzz.sh [streams]       (default 300; the normal build is also run with 1000)
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
API=${M64P_API_INCLUDE_DIR:-$R/../mupen64plus-core-dento/src/api}
N=${1:-300}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cc_() { cc -O1 -g -Wall -Wextra -Wno-unused-parameter -DARMLET_RDP_DEFER "$@" -I"$R/src" -I"$R/ext/mupen64plus-rsp-hle/src" -I"$API" \
        "$R/tools/equiv/rdp_defer_fuzz.c" "$R/src/rdp_defer.c" -ldl; }
cc_ -o "$T/good"
echo "module:    $("$T/good" $((N * 3)) 2>&1 | tail -n 1)"
rc=0
for n in 1 2 3 4 5; do
    cc_ -DARMLET_RDP_MUTATE=$n -o "$T/m$n"
    out=$(timeout 120 "$T/m$n" "$N" 2>&1 | tail -n 1)
    echo "mutant $n:  $out"
    case $out in *" 0 streams with a difference") rc=1; echo "  NOT CAUGHT" ;; esac
done
"$T/good" $((N * 3)) > /dev/null || rc=1
exit $rc
