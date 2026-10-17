/*
 * tools/hle/test_ucache.c: the ucode cache (src/hle/ucache.c) without a game
 *
 *   cc -O2 -Isrc/hle -o test_ucache tools/hle/test_ucache.c src/hle/ucache.c src/hle/derive_refs.c && ./test_ucache
 *
 * Round trip and persistence, replacement of a key, a truncated file, a flipped bit, a damaged header, an entry of another tool version, the bound on the number of entries, an atomic write
 * that fails half way (the old file stands), two processes writing, an unwritable directory, a disabled cache. Prints the failed checks and exits 1 when there is one.
 * tools/hle/mutcheck_ucache.sh builds it with each deliberate error of ucache.c (-DARMLET_HLE_MUTATE=345 .. 349) and expects it to fail.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "derive.h"
#include "ucache.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAILED: %s (line %d)\n", what, __LINE__); fails++; } } while (0)

static struct ucache_entry mk(uint64_t t, uint64_t d, unsigned v)
{
    struct ucache_entry e;
    memset(&e, 0, sizeof(e));
    e.text_id = t;
    e.data_id = d;
    e.verdict = 2;
    e.probe = UC_PROBE_PASS;
    e.pc[0] = (int32_t)v;
    e.lay[3] = v * 3;
    e.reject_mask = 0x70b0;
    e.load_key = 0x1122334455667788ull ^ t;
    return e;
}

static long fsize(const char *p) { struct stat st; return stat(p, &st) ? -1 : (long)st.st_size; }

static void patch(const char *p, long off, unsigned char xor_)
{
    FILE *f = fopen(p, "r+b");
    int c;
    fseek(f, off, SEEK_SET);
    c = fgetc(f);
    fseek(f, off, SEEK_SET);
    fputc(c ^ xor_, f);
    fclose(f);
}

int main(void)
{
    char dir[] = "/tmp/ucache_test_XXXXXX", path[300], cmd[400];
    struct ucache_entry e;
    const struct ucache_entry *f;
    unsigned i;
    long full;

    if (!mkdtemp(dir))
        return 2;
    signal(SIGXFSZ, SIG_IGN);
    snprintf(path, sizeof(path), "%s/armlet-ucode.cache", dir);

    /* round trip, persistence, replacement */
    CHECK(ucache_open(path) == 0, "a new cache is empty");
    e = mk(1, 10, 1);
    CHECK(ucache_store(&e) == 0, "store 1");
    e = mk(2, 20, 2);
    CHECK(ucache_store(&e) == 0, "store 2");
    e = mk(3, 30, 3);
    CHECK(ucache_store(&e) == 0, "store 3");
    f = ucache_find(2, 20);
    CHECK(f && f->pc[0] == 2 && f->lay[3] == 6 && f->reject_mask == 0x70b0 && f->probe == UC_PROBE_PASS, "find returns what was stored");
    CHECK(!ucache_find(2, 21) && !ucache_find(9, 20), "the key is the text id and the data id");
    ucache_close();
    CHECK(ucache_open(path) == 3, "reopened: three entries");
    e = mk(2, 20, 22);
    e.probe = UC_PROBE_FAIL;
    CHECK(ucache_store(&e) == 0, "replace");
    f = ucache_find(2, 20);
    CHECK(f && f->pc[0] == 22 && f->probe == UC_PROBE_FAIL, "the entry was replaced");
    ucache_close();
    CHECK(ucache_open(path) == 3, "still three entries after a replacement");
    full = fsize(path);
    ucache_close();

    /* a flipped bit in an entry: that entry is ignored, the others stand */
    patch(path, 32 + (long)sizeof(struct ucache_entry) + 40, 0x10);
    CHECK(ucache_open(path) == 2, "an entry with a flipped bit is ignored");
    CHECK(ucache_find(1, 10) && !ucache_find(2, 20) && ucache_find(3, 30), "the others of a damaged file stand");
    ucache_close();

    /* a truncated file: the entries before the cut stand */
    snprintf(cmd, sizeof(cmd), "truncate -s %ld '%s'", full - 100, path);
    CHECK(system(cmd) == 0, "truncate");
    /* (the file was damaged in the middle entry above, so rewrite it first) */
    ucache_open("");
    unlink(path);
    ucache_open(path);
    for (i = 1; i <= 5; i++) {
        e = mk(i, i * 10, i);
        ucache_store(&e);
    }
    ucache_close();
    full = fsize(path);
    snprintf(cmd, sizeof(cmd), "truncate -s %ld '%s'", full - (long)sizeof(struct ucache_entry) / 2, path);
    CHECK(system(cmd) == 0, "truncate");
    CHECK(ucache_open(path) == 4, "a file cut in the last entry keeps four");
    CHECK(ucache_find(4, 40) && !ucache_find(5, 50), "the cut entry is gone, the others stand");
    e = mk(7, 70, 7);
    CHECK(ucache_store(&e) == 0, "a store after a truncated load rewrites a good file");
    ucache_close();
    CHECK(ucache_open(path) == 5, "five entries after the rewrite");
    ucache_close();

    /* a damaged header: the whole file is ignored, the next store replaces it */
    patch(path, 2, 0x55);
    CHECK(ucache_open(path) == 0, "a damaged header makes the file empty");
    e = mk(8, 80, 8);
    CHECK(ucache_store(&e) == 0, "store over a damaged file");
    ucache_close();
    CHECK(ucache_open(path) == 1 && ucache_find(8, 80), "the damaged file was replaced");
    ucache_close();

    /* another tool version: the entry is not used (its checksum is right) */
    {
        struct ucache_entry old = mk(11, 110, 11);
        FILE *fp;
        uint32_t crc;
        ucache_open(path);
        e = mk(12, 120, 12);
        ucache_store(&e);
        ucache_close();
        fp = fopen(path, "r+b");
        fseek(fp, 32 + (long)sizeof(struct ucache_entry) * 0, SEEK_SET);
        CHECK(fread(&old, sizeof(old), 1, fp) == 1, "read an entry");
        old.tool ^= 0x1234;
        crc = ucache_crc32(&old, offsetof(struct ucache_entry, crc));
        old.crc = crc;
        fseek(fp, 32, SEEK_SET);
        fwrite(&old, sizeof(old), 1, fp);
        fclose(fp);
        ucache_open(path);
        CHECK(!ucache_find(old.text_id, old.data_id), "an entry of another tool version is not used");
        CHECK(ucache_find(12, 120), "an entry of this tool version is");
        ucache_close();
    }

    /* the bound */
    unlink(path);
    ucache_open(path);
    for (i = 1; i <= UCACHE_MAX_ENTRIES + 100; i++) {
        e = mk(1000 + i, i, i);
        ucache_store(&e);
    }
    ucache_close();
    CHECK(ucache_open(path) == UCACHE_MAX_ENTRIES, "the cache holds at most UCACHE_MAX_ENTRIES entries");
    CHECK(ucache_find(1000 + UCACHE_MAX_ENTRIES + 100, UCACHE_MAX_ENTRIES + 100) && !ucache_find(1001, 1), "the oldest entries went");
    CHECK(fsize(path) <= 32 + (long)UCACHE_MAX_ENTRIES * (long)sizeof(struct ucache_entry), "the file is bounded");
    ucache_close();

    /* an atomic write: one that fails half way leaves the old file */
    unlink(path);
    ucache_open(path);
    for (i = 1; i <= 20; i++) {
        e = mk(i, i, i);
        ucache_store(&e);
    }
    full = fsize(path);
    {
        struct rlimit rl, old;
        getrlimit(RLIMIT_FSIZE, &old);
        rl.rlim_cur = 1200;
        rl.rlim_max = old.rlim_max;
        setrlimit(RLIMIT_FSIZE, &rl);
        e = mk(99, 99, 99);
        CHECK(ucache_store(&e) != 0, "a store that cannot be written reports it");
        setrlimit(RLIMIT_FSIZE, &old);
    }
    ucache_close();
    CHECK(fsize(path) == full, "the old file stands after a write that failed half way");
    CHECK(ucache_open(path) == 20 && !ucache_find(99, 99), "and has its twenty entries");
    {
        char tmp[400];
        snprintf(tmp, sizeof(tmp), "%s.tmp%ld", path, (long)getpid());
        CHECK(fsize(tmp) < 0, "no temporary file is left behind");
    }
    ucache_close();

    /* two processes: the second one's entry is not lost when the first writes later */
    unlink(path);
    ucache_open(path);
    e = mk(1, 1, 1);
    ucache_store(&e);
    {
        pid_t pid = fork();
        if (!pid) {
            struct ucache_entry c = mk(2, 2, 2);
            ucache_open(path);
            ucache_store(&c);
            ucache_close();
            _exit(0);
        }
        waitpid(pid, NULL, 0);
    }
    e = mk(3, 3, 3);
    ucache_store(&e);
    ucache_close();
    CHECK(ucache_open(path) == 3 && ucache_find(1, 1) && ucache_find(2, 2) && ucache_find(3, 3), "entries of two processes are merged");
    ucache_close();

    /* an unwritable directory, and a disabled cache */
    ucache_open("/proc/no/such/dir/armlet.cache");
    e = mk(5, 5, 5);
    CHECK(ucache_store(&e) != 0, "an unwritable path reports an error");
    CHECK(ucache_find(5, 5) != NULL, "the entry stays in memory");
    ucache_close();
    CHECK(ucache_open("") == 0, "an empty path disables the cache");
    CHECK(ucache_store(&e) == 0 && !ucache_find(5, 5), "a disabled cache stores nothing");
    ucache_close();

    {                                                   /* the record of an S2DEX build (the dialect command table, the continuation pcs) survives the entry */
        struct derive_rec r, back;
        struct ucache_entry x;
        unsigned k;
        memset(&r, 0, sizeof(r));
        memset(&x, 0, sizeof(x));
        r.pc[PC_LDTX_SPR] = 0x6e0;
        r.pc[PC_LDTX_RECT] = 0xba0;
        r.n_ext = 10;
        for (k = 0; k < 10; k++) {
            r.ext[k].pc = (int32_t)(0x24 + 8 * k);
            r.ext[k].kind = (uint8_t)(0x40 + k);
        }
        ucache_from_rec(&x, &r);
        ucache_to_rec(&x, &back);
        CHECK(back.n_ext == 10 && back.ext[9].pc == 0x24 + 72 && back.ext[9].kind == 0x49 && back.pc[PC_LDTX_RECT] == 0xba0, "an S2DEX record round-trips through an entry");
    }
    {                                                   /* the record of an F3DEX 1.x build (handler pcs, layout, the three attributes) survives the entry, with its family */
        struct derive_rec r, back;
        struct ucache_entry x;
        unsigned k;
        memset(&r, 0, sizeof(r));
        memset(&x, 0, sizeof(x));
        r.fam = DERIVE_F3DEX1;
        for (k = 0; k < DERIVE_NF1PC; k++)
            r.f1pc[k] = (int32_t)(0x58 + 4 * k);
        for (k = 0; k < DERIVE_NF1LAY; k++)
            r.lay[k] = 0x100 + 2 * k;
        r.non = 1;
        r.dlx = 1;
        r.culldl_off = 1;
        r.reject_mask = 0x7030;
        ucache_from_rec(&x, &r);
        x.family = DERIVE_F3DEX1;
        ucache_to_rec(&x, &back);
        CHECK(back.fam == DERIVE_F3DEX1 && back.f1pc[23] == 0x58 + 92 && back.lay[35] == 0x100 + 70 && back.non == 1 && back.dlx == 1 && back.culldl_off == 1 && back.reject_mask == 0x7030,
              "an F3DEX 1.x record round-trips through an entry");
    }
    {                                                   /* the record of an F3D build (handler pcs, layout, masks, the attributes) survives the entry, with its family */
        struct derive_rec r, back;
        struct ucache_entry x;
        unsigned k;
        memset(&r, 0, sizeof(r));
        memset(&x, 0, sizeof(x));
        r.fam = DERIVE_F3D;
        for (k = 0; k < DERIVE_NF3PC; k++)
            r.f3pc[k] = (int32_t)(0x60 + 4 * k);
        for (k = 0; k < DERIVE_NF3LAY; k++)
            r.lay[k] = 0x200 + 2 * k;
        for (k = 0; k < F3F_N; k++)
            r.f3[k] = (uint8_t)(k + 1);
        r.reject_mask = 0x7030;
        r.clip_mask = 0x4343;
        r.culldl_mask = 0xffff;
        ucache_from_rec(&x, &r);
        x.family = DERIVE_F3D;
        ucache_to_rec(&x, &back);
        CHECK(back.fam == DERIVE_F3D && back.f3pc[21] == 0x60 + 84 && back.lay[33] == 0x200 + 66 && back.f3[F3F_VPTR_MUL] == 7 && back.reject_mask == 0x7030 && back.clip_mask == 0x4343 &&
              back.culldl_mask == 0xffff, "an F3D record round-trips through an entry");
    }

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd))
        fails++;
    printf(fails ? "%d checks FAILED\n" : "all checks passed\n", fails);
    return fails ? 1 : 0;
}
