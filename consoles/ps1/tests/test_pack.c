/* ps1_pack (XBA-168): a PS1 title pack, the runtime's side. SHA-256, the boot EXE of a disc (SYSTEM.CNF, PSX.EXE, a
 * sub-directory, any case; our homebrew disc when it is built), the pack.json checks, the library's table,
 * ps1_pack_load's outcome for a valid / stale (another boot EXE, another key, another shell ABI, another host, a
 * damaged library, a table of another ABI or another EXE, no identity) / missing pack -- the machine runs in every
 * case -- and the discovered code the next Prepare reads (written once, no repeats).
 *
 *   ps1_pack_test <fake_ps1_pack.so> [HOMEBREW_DIR] */
#define _GNU_SOURCE
#include "ps1_core.h"
#include "ps1_pack.h"
#include "ps1_recomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);                                      \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void copy_file(const char *from, const char *to)
{
    FILE *a = fopen(from, "rb"), *b = fopen(to, "wb");
    char buf[65536];
    size_t r;
    CHECK(a && b);
    while (a && b && (r = fread(buf, 1, sizeof buf, a)) > 0)
        fwrite(buf, 1, r, b);
    if (a)
        fclose(a);
    if (b)
        fclose(b);
}

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static char *read_text(const char *path)
{
    static char buf[1 << 16];
    FILE *f = fopen(path, "r");
    size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f)
        fclose(f);
    buf[n] = 0;
    return buf;
}

/* A pack folder dir with lib copied in as libtitle.so and a pack.json; the fields can be spoilt one at a time. */
static void make_pack(const char *dir, const char *lib, const char *key, const char *src_sha, int shell_abi,
                      const char *abi, int spoil_sha)
{
    char so[1200], json[1200], hex[65], text[2048];
    struct stat st;
    mkdir(dir, 0755);
    snprintf(so, sizeof so, "%s/libtitle.so", dir);
    copy_file(lib, so);
    CHECK(ps1_pack_sha256_file(so, hex) == 0 && stat(so, &st) == 0);
    if (spoil_sha)
        hex[0] = hex[0] == '0' ? '1' : '0';
    snprintf(text, sizeof text,
             "{\"format\": 1, \"console\": \"ps1\", \"title_key\": \"%s\", \"title\": \"PACKTEST \\\"x\\\"\",\n"
             " \"source_sha256\": \"%s\", \"boot\": \"GAME.EXE\", \"abi\": \"%s\", \"shell_abi\": %d,\n"
             " \"toolkit\": \"t\", \"prepare\": {\"steps_s\": {\"lift\": 1.0}, \"jobs\": 2, \"lib\": \"nested.so\"},\n"
             " \"lib\": \"libtitle.so\", \"lib_sha256\": \"%s\", \"lib_size\": %lld,\n"
             " \"entry\": \"ps1_recomp_title_table\", \"discovered\": 0}\n",
             key, src_sha, abi, shell_abi, hex, (long long)st.st_size);
    snprintf(json, sizeof json, "%s/pack.json", dir);
    write_text(json, text);
}

static void test_sha256(void)
{
    uint8_t sum[32];
    char hex[65];
    ps1_pack_sha256("abc", 3, sum);
    ps1_pack_hex(sum, hex);
    CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    ps1_pack_sha256("", 0, sum);
    ps1_pack_hex(sum, hex);
    CHECK(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}

/* ---- A cooked ISO 9660 image (2048-byte sectors) of our own ---- */

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
        p[7 - i] = (uint8_t)(v >> (8 * i)); /* both-endian: the big-endian half after */
    }
}

/* A directory record at p: returns its length. */
static int dir_rec(uint8_t *p, const char *name, uint32_t lba, uint32_t size, int dir)
{
    size_t nl = name ? strlen(name) : 1;
    int len = (int)(33 + nl + ((33 + nl) & 1));
    memset(p, 0, (size_t)len);
    p[0] = (uint8_t)len;
    put32(p + 2, lba);
    put32(p + 10, size);
    p[25] = dir ? 2 : 0;
    p[32] = (uint8_t)nl;
    if (name)
        memcpy(p + 33, name, nl);
    return len;
}

/* sectors: 16 PVD, 18 root, 19 sub-directory GAMES, 20 SYSTEM.CNF (when cnf), 21.. the EXE (exe_n bytes) named
 * exe_name in GAMES (in_sub) or in the root. */
static void make_iso(const char *path, const char *cnf, const char *exe_name, int in_sub, const uint8_t *exe,
                     size_t exe_n)
{
    size_t nsec = 21 + (exe_n + 2047) / 2048;
    uint8_t *img = calloc(nsec, 2048);
    uint8_t *pvd = img + 16 * 2048, *root = img + 18 * 2048, *sub = img + 19 * 2048;
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    memcpy(pvd + 8, "PLAYSTATION                     ", 32);
    dir_rec(pvd + 156, NULL, 18, 2048, 1);
    int o = 0, k;
    o += dir_rec(root + o, NULL, 18, 2048, 1); /* "." (name byte 0) */
    k = dir_rec(root + o, NULL, 18, 2048, 1);  /* ".." (name byte 1) */
    root[o + 33] = 1;
    o += k;
    if (cnf)
        o += dir_rec(root + o, "SYSTEM.CNF;1", 20, (uint32_t)strlen(cnf), 0);
    o += dir_rec(root + o, "GAMES", 19, 2048, 1);
    if (!in_sub)
        o += dir_rec(root + o, exe_name, 21, (uint32_t)exe_n, 0);
    int s = 0;
    s += dir_rec(sub + s, NULL, 19, 2048, 1);
    k = dir_rec(sub + s, NULL, 18, 2048, 1);
    sub[s + 33] = 1;
    s += k;
    if (in_sub)
        s += dir_rec(sub + s, exe_name, 21, (uint32_t)exe_n, 0);
    if (cnf)
        memcpy(img + 20 * 2048, cnf, strlen(cnf));
    memcpy(img + 21 * 2048, exe, exe_n);
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f) {
        fwrite(img, 2048, nsec, f);
        fclose(f);
    }
    free(img);
}

static void test_disc_boot(const char *root, const char *homebrew)
{
    char iso[1200], hex[65], want[65], boot[256], why[512];
    uint8_t exe[5000], sum[32];
    for (size_t i = 0; i < sizeof exe; i++)
        exe[i] = (uint8_t)(i * 7 + 3);
    memcpy(exe, "PS-X EXE", 8);
    ps1_pack_sha256(exe, sizeof exe, sum);
    ps1_pack_hex(sum, want);

    /* SYSTEM.CNF names an EXE in a sub-directory, in lower case, with spaces and no "cdrom:" colon. */
    snprintf(iso, sizeof iso, "%s/sub.iso", root);
    make_iso(iso, "boot = cdrom\\games\\game.exe;1\r\nTCB=4\r\n", "GAME.EXE;1", 1, exe, sizeof exe);
    CHECK(ps1_pack_disc_boot_sha256(iso, hex, boot, sizeof boot, why, sizeof why) == 0);
    CHECK(!strcmp(hex, want) && !strcmp(boot, "\\games\\game.exe;1"));
    /* No SYSTEM.CNF: PSX.EXE in the root. */
    snprintf(iso, sizeof iso, "%s/psx.iso", root);
    make_iso(iso, NULL, "PSX.EXE;1", 0, exe, sizeof exe);
    CHECK(ps1_pack_disc_boot_sha256(iso, hex, boot, sizeof boot, why, sizeof why) == 0 && !strcmp(hex, want));
    CHECK(!strcmp(boot, "PSX.EXE"));
    /* SYSTEM.CNF names a file that is not there. */
    snprintf(iso, sizeof iso, "%s/gone.iso", root);
    make_iso(iso, "BOOT=cdrom:\\SLUS_005.94;1\n", "GAME.EXE;1", 0, exe, sizeof exe);
    CHECK(ps1_pack_disc_boot_sha256(iso, hex, boot, sizeof boot, why, sizeof why) == -1 && strstr(why, "SLUS_005.94"));
    /* Not an image at all. */
    snprintf(iso, sizeof iso, "%s/none.iso", root);
    CHECK(ps1_pack_disc_boot_sha256(iso, hex, NULL, 0, why, sizeof why) == -1);

    /* Our homebrew disc (build_homebrew.sh ps1): its boot EXE is overlay.exe, byte for byte. */
    char cue[1200], exe_path[1200];
    snprintf(cue, sizeof cue, "%s/overlay.cue", homebrew ? homebrew : "");
    snprintf(exe_path, sizeof exe_path, "%s/overlay.exe", homebrew ? homebrew : "");
    if (homebrew && access(cue, R_OK) == 0 && access(exe_path, R_OK) == 0) {
        CHECK(ps1_pack_sha256_file(exe_path, want) == 0);
        CHECK(ps1_pack_disc_boot_sha256(cue, hex, boot, sizeof boot, why, sizeof why) == 0 && !strcmp(hex, want));
        printf("ps1_pack: overlay.cue boots %s, sha256 %s\n", boot, hex);
    } else {
        printf("ps1_pack: no homebrew disc (%s), its check skipped\n", cue);
    }
}

/* A machine whose "BIOS" jumps to 0x80010000, where a loop runs: the interpreter runs main-RAM code a table does not
 * cover (a missed record) from the first instructions. */
static ps1_machine *machine(void)
{
    static uint8_t bios[PS1_BIOS_SIZE];
    const uint32_t code[] = {0x3C088001u /* lui t0, 0x8001 */, 0x01000008u /* jr t0 */, 0};
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 4; k++)
            bios[4 * i + k] = (uint8_t)(code[i] >> (8 * k));
    char err[256] = "";
    ps1_machine *m = ps1_create(bios, sizeof bios, NULL, NULL, 0, err, sizeof err);
    CHECK(m != NULL);
    if (m) {
        ps1_poke32(m, 0x80010000u, 0x1000FFFFu); /* b . */
        ps1_poke32(m, 0x80010004u, 0);
    }
    return m;
}

static int count_lines(const char *path, const char *prefix)
{
    FILE *f = fopen(path, "r");
    char line[8192];
    int n = 0;
    while (f && fgets(line, sizeof line, f))
        n += !strncmp(line, prefix, strlen(prefix));
    if (f)
        fclose(f);
    return n;
}

/* P (0x80011300) is reached three times, each right after a new record of code around it (A0 / A1 / A2, many
 * records in between so none is a view into the one before): as a view into each, three lengths, the same bytes.
 * One record (views of other lengths used to be recorded again, and counted as variants). */
static void test_record_once(const char *root)
{
    ps1_machine *m = machine();
    if (!m)
        return;
    const uint32_t A[3] = {0x80011000u, 0x80011100u, 0x80011200u}, P = 0x80011300u;
    const uint32_t JR_RA = 0x03E00008u;
    uint32_t pc = 0x80010000u, filler = 0x80030000u;
#define JAL(t) (0x0C000000u | (((t) >> 2) & 0x3FFFFFFu))
    for (int round = 0; round < 3; round++) {
        ps1_poke32(m, A[round], JR_RA);
        ps1_poke32(m, A[round] + 4, 0);
        ps1_poke32(m, pc, JAL(A[round]));
        ps1_poke32(m, pc + 4, 0);
        ps1_poke32(m, pc + 8, JAL(P));
        ps1_poke32(m, pc + 12, 0);
        pc += 16;
        for (int k = 0; k < 40; k++, filler += 0x800, pc += 8) {
            ps1_poke32(m, filler, JR_RA);
            ps1_poke32(m, filler + 4, 0);
            ps1_poke32(m, pc, JAL(filler));
            ps1_poke32(m, pc + 4, 0);
        }
    }
#undef JAL
    ps1_poke32(m, pc, 0x1000FFFFu);
    ps1_poke32(m, pc + 4, 0);
    ps1_poke32(m, P, JR_RA);
    ps1_poke32(m, P + 4, 0);
    static const ps1_recomp_table t = {PS1_RECOMP_ABI, 0, NULL, "", 0x80010000u};
    CHECK(ps1_attach_recomp(m, &t) == 0);
    ps1_run_cycles(m, 400000);
    char path[1300];
    snprintf(path, sizeof path, "%s/record_once.txt", root);
    CHECK(ps1_recomp_write_missed(m, path) > 120);
    CHECK(count_lines(path, "80011300 ") == 1);
    CHECK(count_lines(path, "80011000 ") == 1 && count_lines(path, "80011200 ") == 1);
    ps1_destroy(m);
}

int main(int argc, char **argv)
{
    char root[] = "/tmp/ps1_pack_testXXXXXX", dir[1200], lib[1300], why[512], json[1300];
    const char *key = "SLUS-00594";
    const char *hex = "d65e9f0484e4074111d165b0443e051f370e0a4689e532017d730ce5b7fd26bd";
    ps1_pack p;

    if (argc < 2) {
        fprintf(stderr, "usage: ps1_pack_test <fake_ps1_pack.so> [HOMEBREW_DIR]\n");
        return 2;
    }
    const char *fake = argv[1];
    CHECK(mkdtemp(root) != NULL);
    test_sha256();
    test_disc_boot(root, argc > 2 ? argv[2] : NULL);
    test_record_once(root);
    CHECK(strlen(ps1_pack_host_abi()) > 0 && strcmp(ps1_pack_host_abi(), "unknown") != 0);

    setenv("FAKE_PACK_SRC_SHA", hex, 1);
    ps1_machine *m = machine();
    if (!m)
        return 1;
    const char *abi = ps1_pack_host_abi();

    /* Valid. */
    snprintf(dir, sizeof dir, "%s/ps1-%s", root, key);
    make_pack(dir, fake, key, hex, 1, abi, 0);
    CHECK(ps1_pack_check(dir, key, hex, 1, lib, sizeof lib, why, sizeof why) == PS1_PACK_LOADED);
    snprintf(json, sizeof json, "%s/libtitle.so", dir);
    CHECK(!strcmp(lib, json));
    CHECK(ps1_pack_load(m, lib, key, hex, &p) == PS1_PACK_LOADED);
    CHECK(p.table && p.table->count == 1 && p.table->entries[0].addr == 0x80020000u);
    p.table->entries[0].fn(m); /* calls ps1_rc_gte_read, resolved against this program */

    /* The discovery loop: the loop at 0x80010000 is not in the table, recorded once with its bytes. */
    ps1_run_cycles(m, 20000);
    snprintf(json, sizeof json, "%s/work/ps1-%s/discovered.txt", root, key);
    CHECK(ps1_pack_save_missed(m, json) == 1); /* directories made */
    CHECK(!strncmp(read_text(json), "80010000 ", 9));
    CHECK(strstr(read_text(json), " FFFF0010000000") != NULL); /* b . ; nop, as in memory */
    CHECK(ps1_pack_save_missed(m, json) == 0);                 /* the exit hook, then the end of the run */
    CHECK(strchr(read_text(json), '\n') == read_text(json) + strlen(read_text(json)) - 1);
    ps1_pack_close(&p);
    CHECK(p.handle == NULL);
    /* shell_abi 0: not checked. */
    CHECK(ps1_pack_check(dir, key, hex, 0, lib, sizeof lib, why, sizeof why) == PS1_PACK_LOADED);

    /* Missing. */
    CHECK(ps1_pack_load(m, NULL, key, hex, &p) == PS1_PACK_NONE && p.handle == NULL);
    CHECK(ps1_pack_load(m, "", key, hex, &p) == PS1_PACK_NONE);
    snprintf(json, sizeof json, "%s/none", root);
    CHECK(ps1_pack_check(json, key, hex, 1, lib, sizeof lib, why, sizeof why) == PS1_PACK_NONE);
    snprintf(json, sizeof json, "%s/none/libtitle.so", root);
    CHECK(ps1_pack_load(m, json, key, hex, &p) == PS1_PACK_STALE && p.handle == NULL);

    /* Stale: another boot EXE (pack.json, then a table that says so), no identity (the disc could not be read),
     * another key, another shell ABI, another host, a damaged library, a table of another ABI. */
    char other[65];
    memcpy(other, hex, 65);
    other[5] = other[5] == 'a' ? 'b' : 'a';
    CHECK(ps1_pack_check(dir, key, other, 1, lib, sizeof lib, why, sizeof why) == PS1_PACK_STALE
          && strstr(why, "another boot EXE"));
    snprintf(json, sizeof json, "%s/libtitle.so", dir);
    CHECK(ps1_pack_load(m, json, key, other, &p) == PS1_PACK_STALE && p.handle == NULL);
    CHECK(ps1_pack_load(m, json, key, "", &p) == PS1_PACK_STALE && p.handle == NULL);
    CHECK(ps1_pack_load(m, json, key, NULL, &p) == PS1_PACK_STALE && p.handle == NULL);
    CHECK(ps1_pack_check(dir, "SLUS-00000", hex, 1, lib, sizeof lib, why, sizeof why) == PS1_PACK_STALE
          && strstr(why, "title_key"));
    CHECK(ps1_pack_check(dir, key, hex, 2, lib, sizeof lib, why, sizeof why) == PS1_PACK_STALE
          && strstr(why, "shell_abi 1, this shell is 2"));

    snprintf(dir, sizeof dir, "%s/abi2", root);
    make_pack(dir, fake, key, hex, 2, abi, 0);
    snprintf(json, sizeof json, "%s/libtitle.so", dir);
    CHECK(ps1_pack_load(m, json, key, hex, &p) == PS1_PACK_STALE); /* the runtime's own shell ABI is 1 */

    snprintf(dir, sizeof dir, "%s/host", root);
    make_pack(dir, fake, key, hex, 1, "mips", 0);
    CHECK(ps1_pack_check(dir, key, hex, 1, lib, sizeof lib, why, sizeof why) == PS1_PACK_STALE && strstr(why, "abi"));

    snprintf(dir, sizeof dir, "%s/damaged", root);
    make_pack(dir, fake, key, hex, 1, abi, 1);
    CHECK(ps1_pack_check(dir, key, hex, 1, lib, sizeof lib, why, sizeof why) == PS1_PACK_STALE
          && strstr(why, "sha256"));
    snprintf(json, sizeof json, "%s/pack.json", dir);
    write_text(json, "{\"format\": 1, \"console\": \"ps1\"");
    CHECK(ps1_pack_check(dir, key, hex, 1, lib, sizeof lib, why, sizeof why) == PS1_PACK_STALE);
    write_text(json, "{\"format\": 1, \"console\": \"ps1\"}");
    CHECK(ps1_pack_check(dir, key, hex, 1, lib, sizeof lib, why, sizeof why) == PS1_PACK_STALE
          && strstr(why, "lacks"));
    snprintf(dir, sizeof dir, "%s/gba", root);
    make_pack(dir, fake, key, hex, 1, abi, 0);
    snprintf(json, sizeof json, "%s/pack.json", dir);
    char *t = strdup(read_text(json));
    char *c = strstr(t, "\"ps1\"");
    memcpy(c, "\"gba\"", 5);
    write_text(json, t);
    free(t);
    CHECK(ps1_pack_check(dir, key, hex, 1, lib, sizeof lib, why, sizeof why) == PS1_PACK_STALE
          && strstr(why, "console gba"));

    snprintf(dir, sizeof dir, "%s/ps1-%s", root, key);
    snprintf(json, sizeof json, "%s/libtitle.so", dir);
    setenv("FAKE_PACK_ABI", "99", 1);
    CHECK(ps1_pack_load(m, json, key, hex, &p) == PS1_PACK_STALE && p.handle == NULL);
    CHECK(ps1_pack_open(json, hex, &p, why, sizeof why) == -1 && strstr(why, "abi 99"));
    unsetenv("FAKE_PACK_ABI");

    /* A bare library (no pack.json): the table's own checks only. */
    snprintf(json, sizeof json, "%s/bare.so", root);
    copy_file(fake, json);
    CHECK(ps1_pack_load(m, json, key, hex, &p) == PS1_PACK_LOADED);
    ps1_pack_close(&p);
    CHECK(ps1_pack_load(m, json, key, other, &p) == PS1_PACK_STALE);

    /* Whatever the pack, the game runs. */
    uint64_t c0 = ps1_cycles(m);
    ps1_run_frame(m);
    ps1_run_frame(m);
    CHECK(ps1_cycles(m) > c0);
    ps1_destroy(m);

    char cmd[1300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0)
        fprintf(stderr, "could not remove %s\n", root);
    if (failures)
        fprintf(stderr, "ps1_pack: %d check(s) failed\n", failures);
    else
        printf("ps1_pack: all checks passed\n");
    return failures ? 1 : 0;
}
