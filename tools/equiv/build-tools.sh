#!/bin/bash
# Builds the three test plugins of the equivalence runs for a target: out/<target>/tools/{rsp-shim,input-det,audio-adump}.so
#   tools/equiv/build-tools.sh native|aarch64|rg35xxsp
# audio-adump.so is mupen64plus-rdp-foir's tools/prsp/adump.c (FOIR_TOOLS, default ../mupen64plus-rdp-foir/tools/prsp).
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
API=${M64P_API_INCLUDE_DIR:-$R/../mupen64plus-core/src/api}
RG35=${RG35_TOOLCHAIN:-$R/../toolchains/rg35xxsp}
FT=${FOIR_TOOLS:-$R/../mupen64plus-rdp-foir/tools/prsp}
case $1 in
native)   CC=cc ;;
aarch64)  CC=aarch64-linux-gnu-gcc ;;
rg35xxsp) CC=$RG35/bin/rg35-gcc ;;
*) echo "usage: $0 native|aarch64|rg35xxsp" >&2; exit 2 ;;
esac
O=$R/out/$1/tools; mkdir -p "$O"
here=$(dirname "$0")
$CC -O2 -shared -fPIC -I"$API" -o "$O/rsp-shim.so" "$here/rsp_shim.c" -ldl
$CC -O2 -shared -fPIC -I"$API" -o "$O/input-det.so" "$here/inp_det.c"
$CC -O2 -shared -fPIC -I"$API" -o "$O/audio-adump.so" "$FT/adump.c"
ls -l "$O"
