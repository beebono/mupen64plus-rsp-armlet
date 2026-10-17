#!/bin/bash
# A ucode that is a registered build runs natively from its first task with GfxProbe on: no probe, no validator, no latch (docs/testing.md).
#   tools/equiv/check_direct.sh ROM...   (ROM relative to the workspace; the desk.sh environment: CORE_DIR, CONSOLE, FOIR_SO, X)
# Fails when a run's first statistics line (64 tasks) shows fewer than 63 native tasks, or when the log has a validator or latch line.
R=$(cd "$(dirname "$0")/../.." && pwd)
runs=(); i=0
for rom in "$@"; do runs+=("direct.$i|armlet-hle+ARMLET_PROBE=on+ARMLET_UCODE_CACHE=off+ARMLET_HLE_STATS=1|$rom|e2e|900"); i=$((i + 1)); done
"$R/tools/equiv/desk.sh" "${runs[@]}" > /dev/null || exit 2
X=${X:-$R/test-output/equiv/desk}; bad=0; i=0
for rom in "$@"; do
    L=$X/out/direct.$i/armlet.log
    n=$(grep -m1 -o 'hle: 64 tasks, [0-9]* native' "$L" | grep -o '[0-9]* native' | cut -d' ' -f1)
    v=$(grep -c 'validator:\|latch: ucode' "$L")
    if [ "${n:-0}" -ge 63 ] && [ "$v" = 0 ]; then echo "ok    $rom: $n of the first 64 tasks native, validator idle"; else echo "FAIL  $rom: native ${n:-?} of 64, $v validator/latch lines"; bad=1; fi
    i=$((i + 1))
done
exit $bad
