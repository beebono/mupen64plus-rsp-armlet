#!/bin/bash
# Desktop (x86_64, offscreen) runs of the equivalence harness for the F3DEX family, with the pieces this session has built:
#   tools/f3dex/desk.sh RUN...      RUN = TAG|BUILD|ROM|KIND|LAST   (tools/equiv/desk.sh; BUILD may carry +VAR=value environment)
# Env (defaults): ARMLET_SO (the armlet build under test), NATIVE_OUT (dir with ref/ and tools/), CORE_DIR, CONSOLE, FOIR_SO, X (work dir).
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
SP=${SP:?scratch directory (holds the device scripts and ROM paths)}
export CORE_DIR=${CORE_DIR:-$SP/desk/core/projects/unix} CONSOLE=${CONSOLE:-$SP/desk/mupen64plus} FOIR_SO=${FOIR_SO:-$SP/desk/foir/projects/unix/mupen64plus-rdp-foir.so}
export NATIVE_OUT=${NATIVE_OUT:-$R/out/native}
export X=${X:-$R/test-output/f3dex/desk}
exec "$R/tools/equiv/desk.sh" "$@"
