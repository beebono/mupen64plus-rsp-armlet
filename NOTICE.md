# Licences and credits

arMLEt is a Mupen64Plus RSP plugin that links three bodies of code into one module (`mupen64plus-rsp-armlet.so`).

| component | where | licence |
|---|---|---|
| arMLEt itself (`src/`, `cmake/`, `scripts/`, `tools/`, `docs/`) | this repository | GPL-2.0-or-later |
| mupen64plus-rsp-hle (audio / JPEG / video / boot-code HLE, and the task detection) | `ext/mupen64plus-rsp-hle` (github.com/mupen64plus/mupen64plus-rsp-hle, pinned) | GPL-2.0-or-later |
| parallel-rsp (the Lightning-JIT RSP interpreter, used for graphics tasks) | `ext/parallel-rsp` (github.com/beebono/parallel-rsp, branch `m64p-aarch64`) | MIT **or** LGPL-3.0-or-later, at your choice (see its `LICENSE`, `LICENSE.MIT`, `LICENSE.LESSER`, `CREDITS.txt`) |
| GNU Lightning (JIT code generation library, inside the parallel-rsp submodule) | `ext/parallel-rsp/lightning`, compiled in as a static library | LGPL-3.0-or-later (or GPL-3.0-or-later) |

**The shipped binary is effectively under GPL-3.0-or-later terms.** It contains GPL-2.0-or-later code (rsp-hle and arMLEt) and LGPL-3.0 code
(Lightning, statically linked; parallel-rsp taken under its LGPL option or under MIT). GPL-2.0-*only* cannot be combined with LGPL-3.0, but every
GPL part here is "or later", so the combination as a whole is distributed under GPL version 3 or later (GPLv3 section 5 and LGPLv3 section 4;
the LGPL's relink right is met by the complete corresponding source, which this repository and its two submodules are). The licence texts
are in `LICENSES/` (`COPYING` is the GPLv2 text). Someone who needs a different combination can build a binary without the part they cannot
use, but note that arMLEt's audio comes from rsp-hle, so there is no GPL-free build of the plugin as it stands.

Each source file keeps the licence header of its origin. arMLEt's own files carry `SPDX-License-Identifier: GPL-2.0-or-later`.

## Credits

- **mupen64plus-rsp-hle**: Bobby Smiles, Richard Goedeken, Hacktarux, Azimer and the Mupen64Plus team; see the headers in its sources.
- **parallel-rsp**: Hans-Kristian Arntzen (Themaister); it relies on MarathonMan's CEN64 RSP, Ares and CXD4 (its `CREDITS.txt`). The fork's changes
  for aarch64 are in its history.
- **GNU Lightning**: Paulo Cesar Pereira de Andrade and the Free Software Foundation.
- The reference behaviour that arMLEt is tested against is the same pairing run as two modules: rsp-hle with parallel-rsp as its `RspFallback`.
