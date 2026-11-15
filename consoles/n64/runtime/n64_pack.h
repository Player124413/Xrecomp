/* n64_pack -- an N64 title pack, the runtime's side (XBA-174; docs/android/prepare.md « N64 packs », n64_recomp.h
 * for the table it carries). The PS1's ps1_pack.h with a cartridge in place of a disc.
 *
 * tools.prepare builds <packs>/n64-<KEY>/{libtitle.so, pack.json} from the player's ROM (on the device with the
 * embedded toolchain, or on a desktop: tools/prepare/n64.py). The runtime takes the pack when it is the right one and
 * plays the game in the interpreter otherwise -- an N64 game always starts:
 *   - pack.json: format 1, console "n64", title_key = the game's key, source_sha256 = the SHA-256 of the ROM in the
 *     console's big-endian order (a .z64, .v64 and .n64 dump of one cartridge are the same), abi = this host's,
 *     shell_abi = N64_PACK_SHELL_ABI, entry "n64_recomp_title_table", lib a plain name with lib_size bytes and
 *     lib_sha256 (hashed at each start);
 *   - the library: dlopen, the entry's table, its abi (n64_attach_recomp) and its source_sha256 = the ROM's.
 * The runtime holds the whole ROM in memory anyway; it hashes it (n64_pack_rom_sha256) only when a pack is there.
 * And the discovery loop: at exit the code the interpreter had to run (n64_recomp_write_missed: address, hash,
 * bytes) is appended to a text file without repeats (address + hash), which the next Prepare hands to the lifter.
 *
 * No SDL: consoles/n64/tests checks every case (n64_pack). POSIX (dlopen). */
#ifndef N64_PACK_H
#define N64_PACK_H

#include <stddef.h>
#include <stdint.h>

#include "n64_recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The shell ABI an N64 pack is linked against: the app shell's xba_shell_abi (pack_loader.h XBA_SHELL_ABI) and
 * tools/prepare/pack.py SHELL_ABI; bumped together. */
#define N64_PACK_SHELL_ABI 1

enum {
    N64_PACK_LOADED = 0, /* the table is attached */
    N64_PACK_NONE = 1,   /* no pack.json / no library: interpreter */
    N64_PACK_STALE = 2   /* a pack that is not this ROM's, this shell's or this host's, or does not load: interpreter */
};

/* SHA-256. */
void n64_pack_sha256(const void *data, size_t n, uint8_t out[32]);
/* Lower-case hex of a file's SHA-256 into hex (65 bytes). Returns 0, or -1 when it cannot be read. */
int n64_pack_sha256_file(const char *path, char hex[65]);
void n64_pack_hex(const uint8_t sum[32], char hex[65]);
/* The source identity of a ROM in any byte order (z64 / v64 / n64, n64_rom_detect): the SHA-256 of its bytes in the
 * console's big-endian order (as n64_rom_normalize; a trailing part that is not a whole word or pair as it is), the
 * same as tools/prepare/n64.py and n64_tools (n64_recomp_table.source_sha256). rom is not modified. Returns 0, or -1
 * (hex "") when it is not an N64 ROM. */
int n64_pack_rom_sha256(const uint8_t *rom, size_t n, char hex[65]);

/* pack.json's "abi" for this host: "arm64-v8a", "x86_64", "x86", "armeabi-v7a", else "unknown". */
const char *n64_pack_host_abi(void);

/* The checks on <pack_dir>/pack.json for this game (key, the ROM's hex SHA-256), this shell (shell_abi; 0 skips it)
 * and this host. Fills lib_path. Returns N64_PACK_LOADED (all good), N64_PACK_NONE (no pack.json) or N64_PACK_STALE
 * with a short reason in why ("built from another ROM", "shell_abi 2, this shell is 1"). */
int n64_pack_check(const char *pack_dir, const char *key, const char *source_sha256, long long shell_abi,
                   char *lib_path, size_t lib_path_n, char *why, size_t why_n);

typedef struct n64_pack {
    void *handle;
    const n64_recomp_table *table;
} n64_pack;

/* dlopen(lib, RTLD_NOW | RTLD_LOCAL), the table from N64_RECOMP_ENTRY_SYMBOL, its abi and its source_sha256 (= the
 * ROM's; NULL skips that check). Returns 0, or -1 with why (the library is closed again). */
int n64_pack_open(const char *lib, const char *source_sha256, n64_pack *out, char *why, size_t why_n);
void n64_pack_close(n64_pack *p);

/* What n64_runtime_run does with n64_runtime_opts.pack_lib: when the library's folder has a pack.json, n64_pack_check
 * (shell_abi N64_PACK_SHELL_ABI) first; then n64_pack_open and n64_attach_recomp. source_sha256 NULL or "": the ROM's
 * identity is unknown, the pack is refused. Logs one line on stdout: "PACK_LOADED n64 <KEY> (<N> functions)" or
 * "PACK_STALE n64 <KEY> (<why>), running interpreted". pack_lib NULL or "": N64_PACK_NONE, nothing logged. out is
 * closed by the caller (n64_pack_close) after n64_destroy. */
int n64_pack_load(n64_machine *m, const char *pack_lib, const char *key, const char *source_sha256, n64_pack *out);

/* n64_recomp_write_missed(m) into path, its directory made first. Logs "n64: code at <N> address(es) the pack did not
 * cover, <A> new record(s), in <path>" when it added any (it is called at every pause and every few minutes). Returns
 * the number of records added, or -1. */
int n64_pack_save_missed(n64_machine *m, const char *path);

#ifdef __cplusplus
}
#endif

#endif
