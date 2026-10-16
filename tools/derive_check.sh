#!/bin/bash
# The checks of tools/derive_build.py in one go (docs/ucode-derive.md, "Validation"): the synthetic self-test, the leave-one-out derivation of every registered build against its
# hand-written record, and the mutation check.  tools/derive_check.sh DUMPS     (DUMPS: the survey's dump directory, see docs/ucode-survey.md "Reproduce"; ROM-derived, not in git)
set -e
D=$(cd "$(dirname "$0")" && pwd)
echo "== self-test"; python3 -I "$D/derive_build.py" selftest
[ -n "$1" ] || { echo "(no dump directory: validate and mutate skipped)"; exit 0; }
echo "== leave-one-out validation"; python3 -I "$D/derive_build.py" validate "$1"
echo "== mutation check"; python3 -I "$D/derive_build.py" mutate "$1"
