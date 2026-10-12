/* Code-generation check for src/native/f3dex_vu.h: a matrix * vector chain (VMUDN / VMADN / VMADH / ...) on a local `struct vu` that is
 * loaded from and stored to the host state. Look at the assembly: the 32 registers must stay in NEON registers (no stack traffic).
 *   aarch64-linux-gnu-gcc -O3 -std=gnu11 -S -o - tools/native-f3dex/vu_asm_example.c | less
 */
#include "../../src/native/f3dex_vu.h"

void vu_example(const struct vu_io *io, unsigned addr)
{
    struct vu u;
    vu_load(&u, io);
    LQV(&u, 4, 0, addr);                    /* vertex */
    VMUDN(&u, 20, 8, 4, 8);                 /* v20 = M.col0 * v4[x] ... as the F3DEX vertex transform does */
    VMADN(&u, 20, 9, 4, 9);
    VMADN(&u, 20, 10, 4, 10);
    VMADN(&u, 20, 11, 4, 11);
    VMADH(&u, 21, 8, 4, 8);
    VMADH(&u, 21, 9, 4, 9);
    VMADH(&u, 21, 10, 4, 10);
    VMADH(&u, 21, 11, 4, 11);
    VRCPH(&u, 22, 3, 21, 3);
    VRCPL(&u, 22, 2, 20, 3);
    VMUDH(&u, 23, 21, 22, 0);
    SDV(&u, 21, 0, addr + 8);
    vu_store_masked(&u, io, ~((1u << 20) | (1u << 21) | (1u << 22) | (1u << 23)), 1, 1, 0);
}
