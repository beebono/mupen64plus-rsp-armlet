#!/bin/bash
# stamp.sh [check TARGET]: the source hash of the test plugins (rsp_shim.c, inp_det.c, mupen64plus-rdp-foir's adump.c). "stamp.sh" prints it (build-tools.sh stores it in out/<target>/tools/STAMP);
# "stamp.sh check TOOLSDIR" (a directory holding the built plugins and their STAMP) exits 1 with a message when that build is older than the sources, which makes every native run look like a hash difference (host.sh and desk.sh call it).
R=$(cd "$(dirname "$0")/../.." && pwd)
FT=${FOIR_TOOLS:-$R/../mupen64plus-rdp-foir/tools/prsp}
h=$(cat "$R/tools/equiv/rsp_shim.c" "$R/tools/equiv/inp_det.c" "$FT/adump.c" | sha256sum | cut -c1-16)
if [ "$1" = check ]; then
    s=$(cat "$2/STAMP" 2>/dev/null)
    if [ "$s" != "$h" ]; then
        echo "stale test plugins in $2 (stamp '$s', sources '$h'): run tools/equiv/build-tools.sh <target>" >&2
        exit 1
    fi
else
    echo "$h"
fi
