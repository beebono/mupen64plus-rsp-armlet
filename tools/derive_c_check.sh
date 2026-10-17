#!/bin/bash
# The checks of the C derivation (src/hle/derive.c) against the Python tool, on the survey's dumps (docs/ucode-derive.md "The C port", docs/testing.md):
#   tools/derive_c_check.sh DUMPS [--stems]     (--stems adds every (text, data) pair, ~2 min more)
# 1. src/hle/derive_refs.c is what tools/derive_refs_gen.py writes from the tables and dumps now
# 2. every distinct text, all references, family auto: family, verdict, counts, composite, every field of the record
# 3. leave-one-out: every registered build derived without its own record
# 4. the mutation check of derive_build.py on both implementations
# 5. the production shortcut (the other families skipped when F3DEX2 explains the text) against computing every family, on every (text, data) pair
set -e
D=$(cd "$(dirname "$0")" && pwd)
[ -n "$1" ] || { echo "usage: $0 DUMPS [--stems]" >&2; exit 2; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
python3 -I "$D/derive_refs_gen.py" "$1" "$D/../src/hle/derive_refs.c" --check
cc -O2 -Wall -Wextra -o "$T/derive_c_check" "$D/derive_c_check.c" "$D/../src/hle/derive.c" "$D/../src/hle/derive_refs.c"
for m in --texts --loo --mutants --shortcut $2; do
    python3 -I "$D/derive_c_check.py" "$1" --bin "$T/derive_c_check" $m
done
