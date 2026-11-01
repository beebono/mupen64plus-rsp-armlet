#!/bin/bash
# The mutations of the 1.x Rej closed forms (src/hle/rej1_fx.c, rej_fx.c with REJ_X1, rej1_dmem.h) against the microcode (docs/testing.md "1.x Rej"): each one must make tools/hle/test_rej1.c fail.
#   MUTS="600 601 ..." tools/hle/mutcheck_rej1.sh BUILD DUMP_PREFIX ORACLE_DIR [CASES]      BUILD: 123 | 121 | 123a (build_test_rej1.sh); ORACLE_DIR: the output of gen_rej1_oracles.sh for that dump
H=$(cd "$(dirname "$0")" && pwd)
B=$1; P=$2; O=$3; N=${4:-1500}
for m in ${MUTS:-600 601 602 603 604 605 606 607 608 610 611 612 613 614 615 616 617 627}; do
    out=$(REJ1_BUILD=$B HOST=1 OUT=$O/mut$m REJ_DATA=$P.data REJ1_VTX_ORACLE=$O/rej1_vtx.inc REJ1_TRI_ORACLE=$O/rej1_tri.inc EXTRA=-DARMLET_HLE_MUTATE=$m "$H/build_test_rej1.sh" "$N" 1 2>/dev/null | tail -1)
    case "$out" in *" 0 failures"*) echo "$m NOT caught: $out" ;; *) echo "$m caught: $out" ;; esac
done
