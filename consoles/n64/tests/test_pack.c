/* n64_pack (XBA-174): an N64 title pack, the runtime's side. SHA-256, the ROM's source identity (the same for its
 * z64 / v64 / n64 byte orders, a trailing part included; our homebrew gfx.z64 when it is built), the pack.json
 * checks, the library's table, n64_pack_load's outcome for a valid / stale (another ROM, another key, another shell
 * ABI, another host, a damaged library, a table of another ABI or another ROM, no identity) / missing pack -- the
 * machine runs in every case -- and the discovered code the next Prepare reads (written once, no repeats). The
 * dispatcher with a table of its own: one record per code at an address however it is reached (as a view
 * into several earlier records), an entry whose code is not there does not cut the interpreter's blocks, pages no
 * entry's code is in run as chained blocks, and both machines (table / none) stay in the same state.
 *
 *   n64_pack_test <fake_n64_pack.so> [HOMEBREW_DIR] */
#define _GNU_SOURCE
#include "n64_core.h"
#include "n64_internal.h"
#include "n64_pack.h"
#include "n64_recomp.h"

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

static uint8_t *read_all(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = len > 0 ? malloc((size_t)len) : NULL;
    *n = d ? fread(d, 1, (size_t)len, f) : 0;
    fclose(f);
    return d;
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
    CHECK(n64_pack_sha256_file(so, hex) == 0 && stat(so, &st) == 0);
    if (spoil_sha)
        hex[0] = hex[0] == '0' ? '1' : '0';
    snprintf(text, sizeof text,
             "{\"format\": 1, \"console\": \"n64\", \"title_key\": \"%s\", \"title\": \"PACKTEST \\\"x\\\"\",\n"
             " \"source_sha256\": \"%s\", \"rom_order\": \"v64\", \"abi\": \"%s\", \"shell_abi\": %d,\n"
             " \"toolkit\": \"t\", \"prepare\": {\"steps_s\": {\"lift\": 1.0}, \"jobs\": 2, \"lib\": \"nested.so\"},\n"
             " \"lib\": \"libtitle.so\", \"lib_sha256\": \"%s\", \"lib_size\": %lld,\n"
             " \"entry\": \"n64_recomp_title_table\", \"discovered\": 0}\n",
             key, src_sha, abi, shell_abi, hex, (long long)st.st_size);
    snprintf(json, sizeof json, "%s/pack.json", dir);
    write_text(json, text);
}

#define ROM_SIZE 0x10000

/* A ROM in the console's order (header, libdragon's IPL3 marker, some bytes), as consoles/n64/tests/test_io.c's. */
static void make_rom(uint8_t *rom, size_t n)
{
    memset(rom, 0, n);
    static const uint8_t hdr[] = {0x80, 0x37, 0x12, 0x40, 0x00, 0x00, 0x00, 0x0F, 0x80, 0x00, 0x04, 0x00,
                                  0x00, 0x00, 0x14, 0x4C, 0x63, 0x5A, 0x2B, 0xFF, 0x9A, 0xBC, 0xDE, 0xF0};
    memcpy(rom, hdr, sizeof hdr);
    memcpy(rom + 0x20, "XBA PACK TEST       ", 20);
    memcpy(rom + 0x3B, "NSME", 4);
    memcpy(rom + 0x40 + 0x300, " Libdragon IPL3 ", 16);
    for (size_t i = 0x1000; i < n; i++)
        rom[i] = (uint8_t)(i * 13 + (i >> 8));
}

/* be in another byte order (1 v64: pairs swapped, 2 n64: words reversed; a trailing part stays). */
static void reorder(const uint8_t *be, uint8_t *out, size_t n, int order)
{
    memcpy(out, be, n);
    for (size_t i = 0; order == 1 && i + 1 < n; i += 2) {
        out[i] = be[i + 1];
        out[i + 1] = be[i];
    }
    for (size_t i = 0; order == 2 && i + 3 < n; i += 4)
        for (int k = 0; k < 4; k++)
            out[i + k] = be[i + 3 - k];
}

static void test_sha256(void)
{
    uint8_t sum[32];
    char hex[65];
    n64_pack_sha256("abc", 3, sum);
    n64_pack_hex(sum, hex);
    CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    n64_pack_sha256("", 0, sum);
    n64_pack_hex(sum, hex);
    CHECK(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"; /* FIPS 180-2: two blocks */
    n64_pack_sha256(two, strlen(two), sum);
    n64_pack_hex(sum, hex);
    CHECK(!strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    char *mil = malloc(1000000); /* a million 'a': whole blocks straight from the input, then the tail */
    memset(mil, 'a', 1000000);
    n64_pack_sha256(mil, 1000000, sum);
    n64_pack_hex(sum, hex);
    CHECK(!strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    free(mil);
    const char *c = getenv("N64_PACK_SHA_C"); /* the ARMv8 instructions, or plain C: both must give these */
    printf("n64_pack: SHA-256 known answers checked (%s)\n", c && *c == '1' ? "plain C forced" : "fastest path");
}

/* The identity of each byte order is the big-endian ROM's SHA-256, over pieces larger than the 64 KB buffer and with
 * a trailing part that is not a whole word (the same rule as n64_rom_normalize and tools/prepare/n64.py). */
static void test_rom_identity(const char *homebrew)
{
    const size_t sizes[2] = {0x30000, 0x30003};
    for (int s = 0; s < 2; s++) {
        size_t n = sizes[s];
        uint8_t *be = malloc(n), *o = malloc(n), sum[32];
        char want[65], hex[65];
        make_rom(be, n);
        n64_pack_sha256(be, n, sum);
        n64_pack_hex(sum, want);
        for (int order = 0; order < 3; order++) {
            reorder(be, o, n, order);
            CHECK(n64_pack_rom_sha256(o, n, hex) == 0 && !strcmp(hex, want));
            /* n64_rom_normalize gives back the same bytes: the core runs what was hashed */
            CHECK(n64_rom_normalize(o, n) == 0 && !memcmp(o, be, n));
        }
        free(be);
        free(o);
    }
    uint8_t junk[64] = {1, 2, 3, 4};
    char hex[65];
    CHECK(n64_pack_rom_sha256(junk, sizeof junk, hex) == -1 && hex[0] == 0);

    char path[1200];
    snprintf(path, sizeof path, "%s/gfx.z64", homebrew ? homebrew : "");
    size_t n = 0;
    uint8_t *rom = homebrew ? read_all(path, &n) : NULL;
    if (rom) {
        char want[65], got[65];
        CHECK(n64_pack_sha256_file(path, want) == 0); /* a .z64: the file's own SHA-256 */
        uint8_t *o = malloc(n);
        for (int order = 0; order < 3; order++) {
            reorder(rom, o, n, order);
            CHECK(n64_pack_rom_sha256(o, n, got) == 0 && !strcmp(got, want));
        }
        printf("n64_pack: gfx.z64 identity %s (z64, v64, n64)\n", want);
        free(o);
        free(rom);
    } else {
        printf("n64_pack: no homebrew ROM (%s), its check skipped\n", path);
    }
}

/* A machine whose CPU is at the table's boot PC (0x80000400), in kernel mode with interrupts off, on a loop there:
 * the interpreter runs RDRAM code the table does not cover (a missed record) from the first instructions. */
static n64_machine *machine(void)
{
    static uint8_t rom[ROM_SIZE];
    make_rom(rom, sizeof rom);
    char err[256] = "";
    n64_machine *m = n64_create(rom, sizeof rom, NULL, err, sizeof err);
    CHECK(m != NULL);
    if (m) {
        m->cpu.cop0[C0_STATUS] = 0x34000000u;
        n64_cpu_status_changed(m);
        n64_poke32(m, 0x80000400u, 0x08000100u); /* j 0x80000400 */
        n64_poke32(m, 0x80000404u, 0);
        m->cpu.pc = 0xFFFFFFFF80000400ull;
        m->cpu.npc = m->cpu.pc + 4;
    }
    return m;
}

/* ---- the dispatcher with a table built here ---- */

#define NOP 0u
#define JR_RA 0x03E00008u
#define J(t) (0x08000000u | (((t) >> 2) & 0x3FFFFFFu))
#define JAL(t) (0x0C000000u | (((t) >> 2) & 0x3FFFFFFu))
#define B_SELF 0x1000FFFFu /* beq zero, zero, . */
#define ADDIU_T0 0x25080001u /* addiu t0, t0, 1 */

static void never_called(n64_machine *m)
{
    (void)m;
    failures++;
    fprintf(stderr, "a lifted function that may not run ran\n");
}

static n64_machine *bare_machine(void)
{
    static uint8_t rom[ROM_SIZE];
    make_rom(rom, sizeof rom);
    char err[256] = "";
    n64_machine *m = n64_create(rom, sizeof rom, NULL, err, sizeof err);
    CHECK(m != NULL);
    if (m) {
        m->cpu.cop0[C0_STATUS] = 0x34000000u;
        n64_cpu_status_changed(m);
        m->cpu.pc = 0xFFFFFFFF80000400ull;
        m->cpu.npc = m->cpu.pc + 4;
    }
    return m;
}

/* An entry over n words at addr as memory holds them now (hash ok) or not (spoilt). */
static n64_recomp_entry entry_at(n64_machine *m, uint32_t addr, uint32_t n, int spoil)
{
    uint32_t w[64];
    for (uint32_t i = 0; i < n; i++)
        w[i] = n64_peek32(m, addr + 4 * i);
    n64_recomp_entry e = {addr, 4 * n, n64_recomp_hash(w, n) ^ (spoil ? 1u : 0u), never_called};
    return e;
}

static int count_lines(const char *path, const char *prefix, size_t *len)
{
    FILE *f = fopen(path, "r");
    char line[8192];
    int n = 0;
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, prefix, strlen(prefix))) {
            n++;
            if (len)
                *len = strcspn(line, "\n");
        }
    if (f)
        fclose(f);
    return n;
}

/* P (0x80001300) is reached three times, each time right after a new record of code around it (A0 / A1 / A2: one
 * 2 KB window each, many records in between so none is a view into the one before): as a view into each, of three
 * lengths, the same bytes. One record. */
static void test_record_once(const char *root)
{
    n64_machine *m = bare_machine();
    if (!m)
        return;
    const uint32_t A[3] = {0x80001000u, 0x80001100u, 0x80001200u}, P = 0x80001300u;
    uint32_t pc = 0x80000400u, filler = 0x80010000u;
    for (int round = 0; round < 3; round++) {
        n64_poke32(m, A[round], JR_RA);
        n64_poke32(m, A[round] + 4, NOP);
        n64_poke32(m, pc, JAL(A[round]));
        n64_poke32(m, pc + 4, NOP);
        n64_poke32(m, pc + 8, JAL(P));
        n64_poke32(m, pc + 12, NOP);
        pc += 16;
        for (int k = 0; k < 40; k++, filler += 0x800, pc += 8) { /* fresh records, 2 KB apart */
            n64_poke32(m, filler, JR_RA);
            n64_poke32(m, filler + 4, NOP);
            n64_poke32(m, pc, JAL(filler));
            n64_poke32(m, pc + 4, NOP);
        }
    }
    n64_poke32(m, pc, B_SELF);
    n64_poke32(m, pc + 4, NOP);
    n64_poke32(m, P, JR_RA);
    n64_poke32(m, P + 4, NOP);
    /* an entry of A's page that never runs: that page is dispatched block by block (a record at each start) */
    static n64_recomp_entry e[1];
    e[0] = entry_at(m, 0x80001700u, 2, 0);
    static const n64_recomp_table t = {N64_RECOMP_ABI, 1, e, "", 0x80000400u};
    CHECK(n64_attach_recomp(m, &t) == 0);
    n64_run_cycles(m, 400000);
    CHECK(m->cpu.pc == (uint64_t)(int64_t)(int32_t)pc || m->cpu.pc == (uint64_t)(int64_t)(int32_t)(pc + 4));
    char path[1300];
    snprintf(path, sizeof path, "%s/record_once.txt", root);
    CHECK(n64_recomp_write_missed(m, path) > 120);
    size_t len = 0;
    CHECK(count_lines(path, "80001300 ", &len) == 1);
    CHECK(len == 8 + 1 + 16 + 1 + 16 * 8); /* a view: its first 16 words */
    for (int round = 0; round < 3; round++) {
        char pre[16];
        snprintf(pre, sizeof pre, "%08X ", A[round]);
        CHECK(count_lines(path, pre, &len) == 1 && len == 8 + 1 + 16 + 1 + 4096);
    }
    uint32_t addrs[8];
    size_t total = n64_recomp_missed(m, addrs, 8);
    int seen = 0; /* the boot PC, A0, the return point, P, ... in the order they were met */
    for (int i = 0; i < 8; i++)
        seen |= (addrs[i] == 0x80000400u) | (addrs[i] == A[0]) << 1 | (addrs[i] == P) << 2;
    CHECK(total > 120 && seen == 7);
    CHECK(n64_recomp_write_missed(m, path) == 0); /* saved again (a pause, then the exit): nothing new */
    n64_destroy(m);
}

/* A loop of 12 instructions at base, run on a machine with a table and on one without. spoil: an entry whose code is
 * not there starts inside the loop (it must not cut the block); with_valid: an entry elsewhere in the page whose code
 * is there (else the page holds no entry's code: chained blocks). Returns the instructions per dispatcher block. */
static double loop_run(uint32_t base, int spoil, int with_valid)
{
    n64_machine *m = bare_machine(), *ref = bare_machine();
    if (!m || !ref)
        return 0;
    n64_machine *both[2] = {m, ref};
    for (int k = 0; k < 2; k++) {
        n64_machine *x = both[k];
        n64_poke32(x, 0x80000400u, J(base));
        n64_poke32(x, 0x80000404u, NOP);
        for (int i = 0; i < 10; i++)
            n64_poke32(x, base + 4 * i, ADDIU_T0);
        n64_poke32(x, base + 40, J(base));
        n64_poke32(x, base + 44, NOP);
        x->cpu.pc = 0xFFFFFFFF80000400ull;
        x->cpu.npc = x->cpu.pc + 4;
    }
    static n64_recomp_entry e[2];
    uint32_t n = 0;
    if (spoil)
        e[n++] = entry_at(m, base + 16, 2, 1);
    if (with_valid)
        e[n++] = entry_at(m, base + 0x800, 2, 0);
    static n64_recomp_table t;
    t = (n64_recomp_table){N64_RECOMP_ABI, n, e, "", 0x80000400u};
    CHECK(n64_attach_recomp(m, &t) == 0);
    n64_run_cycles(m, 3000000);
    n64_run_cycles(ref, 3000000);
    char why[256] = "";
    int diff = n64_recomp_compare(m, ref, why, sizeof why);
    CHECK(diff == 0);
    if (diff)
        fprintf(stderr, "loop at %08X: %s\n", base, why);
    n64_recomp_stats st;
    n64_recomp_get_stats(m, &st);
    n64_destroy(m);
    n64_destroy(ref);
    return st.blocks ? (double)st.interp_ram / (double)st.blocks : 0;
}

static void test_dispatch_blocks(void)
{
    double cut = loop_run(0x80003000u, 1, 1), chained = loop_run(0x80004000u, 0, 0), junk = loop_run(0x80005000u, 1, 0);
    printf("n64_pack: instructions per dispatched block: %.1f (an entry not there inside the loop), %.1f (no entry "
           "in the page), %.1f (only an entry not there)\n", cut, chained, junk);
    CHECK(cut >= 11.0);     /* one block per turn: the spoilt entry does not cut it */
    CHECK(chained >= 100.0); /* chained blocks: back to the dispatcher only at events */
    CHECK(junk >= 100.0);    /* an entry whose code is not there leaves the page free */
}

int main(int argc, char **argv)
{
    char root[] = "/tmp/n64_pack_testXXXXXX", dir[1200], lib[1300], why[512], json[1300];
    const char *key = "NSME-635A2BFF";
    const char *hex = "d65e9f0484e4074111d165b0443e051f370e0a4689e532017d730ce5b7fd26bd";
    n64_pack p;

    if (argc < 2) {
        fprintf(stderr, "usage: n64_pack_test <fake_n64_pack.so> [HOMEBREW_DIR]\n");
        return 2;
    }
    const char *fake = argv[1];
    CHECK(mkdtemp(root) != NULL);
    test_sha256();
    test_rom_identity(argc > 2 ? argv[2] : NULL);
    test_record_once(root);
    test_dispatch_blocks();
    CHECK(strlen(n64_pack_host_abi()) > 0 && strcmp(n64_pack_host_abi(), "unknown") != 0);

    setenv("FAKE_PACK_SRC_SHA", hex, 1);
    n64_machine *m = machine();
    if (!m)
        return 1;
    const char *abi = n64_pack_host_abi();

    /* Valid. */
    snprintf(dir, sizeof dir, "%s/n64-%s", root, key);
    make_pack(dir, fake, key, hex, 1, abi, 0);
    CHECK(n64_pack_check(dir, key, hex, 1, lib, sizeof lib, why, sizeof why) == N64_PACK_LOADED);
    snprintf(json, sizeof json, "%s/libtitle.so", dir);
    CHECK(!strcmp(lib, json));
    CHECK(n64_pack_load(m, lib, key, hex, &p) == N64_PACK_LOADED);
    CHECK(p.table && p.table->count == 1 && p.table->entries[0].addr == 0x80100000u);
    p.table->entries[0].fn(m); /* calls n64_rc_read, resolved against this program */

    /* The discovery loop: the loop at 0x80000400 is not in the table, recorded once with its bytes. */
    n64_run_cycles(m, 20000);
    snprintf(json, sizeof json, "%s/work/n64-%s/discovered.txt", root, key);
    CHECK(n64_pack_save_missed(m, json) == 1); /* directories made */
    CHECK(!strncmp(read_text(json), "80000400 ", 9));
    CHECK(strstr(read_text(json), " 0800010000000000") != NULL); /* j . ; nop, big-endian as in memory */
    CHECK(n64_pack_save_missed(m, json) == 0);                   /* the exit hook, then the end of the run */
    CHECK(strchr(read_text(json), '\n') == read_text(json) + strlen(read_text(json)) - 1);
    n64_pack_close(&p);
    CHECK(p.handle == NULL);
    /* shell_abi 0: not checked. */
    CHECK(n64_pack_check(dir, key, hex, 0, lib, sizeof lib, why, sizeof why) == N64_PACK_LOADED);

    /* Missing. */
    CHECK(n64_pack_load(m, NULL, key, hex, &p) == N64_PACK_NONE && p.handle == NULL);
    CHECK(n64_pack_load(m, "", key, hex, &p) == N64_PACK_NONE);
    snprintf(json, sizeof json, "%s/none", root);
    CHECK(n64_pack_check(json, key, hex, 1, lib, sizeof lib, why, sizeof why) == N64_PACK_NONE);
    snprintf(json, sizeof json, "%s/none/libtitle.so", root);
    CHECK(n64_pack_load(m, json, key, hex, &p) == N64_PACK_STALE && p.handle == NULL);

    /* Stale: another ROM (pack.json, then a table that says so), no identity, another key, another shell ABI,
     * another host, a damaged library, a table of another ABI. */
    char other[65];
    memcpy(other, hex, 65);
    other[5] = other[5] == 'a' ? 'b' : 'a';
    CHECK(n64_pack_check(dir, key, other, 1, lib, sizeof lib, why, sizeof why) == N64_PACK_STALE
          && strstr(why, "another ROM"));
    snprintf(json, sizeof json, "%s/libtitle.so", dir);
    CHECK(n64_pack_load(m, json, key, other, &p) == N64_PACK_STALE && p.handle == NULL);
    CHECK(n64_pack_load(m, json, key, "", &p) == N64_PACK_STALE && p.handle == NULL);
    CHECK(n64_pack_load(m, json, key, NULL, &p) == N64_PACK_STALE && p.handle == NULL);
    CHECK(n64_pack_check(dir, "NSME-00000000", hex, 1, lib, sizeof lib, why, sizeof why) == N64_PACK_STALE
          && strstr(why, "title_key"));
    CHECK(n64_pack_check(dir, key, hex, 2, lib, sizeof lib, why, sizeof why) == N64_PACK_STALE
          && strstr(why, "shell_abi 1, this shell is 2"));

    snprintf(dir, sizeof dir, "%s/abi2", root);
    make_pack(dir, fake, key, hex, 2, abi, 0);
    snprintf(json, sizeof json, "%s/libtitle.so", dir);
    CHECK(n64_pack_load(m, json, key, hex, &p) == N64_PACK_STALE); /* the runtime's own shell ABI is 1 */

    snprintf(dir, sizeof dir, "%s/host", root);
    make_pack(dir, fake, key, hex, 1, "mips", 0);
    CHECK(n64_pack_check(dir, key, hex, 1, lib, sizeof lib, why, sizeof why) == N64_PACK_STALE && strstr(why, "abi"));

    snprintf(dir, sizeof dir, "%s/damaged", root);
    make_pack(dir, fake, key, hex, 1, abi, 1);
    CHECK(n64_pack_check(dir, key, hex, 1, lib, sizeof lib, why, sizeof why) == N64_PACK_STALE
          && strstr(why, "sha256"));
    snprintf(json, sizeof json, "%s/pack.json", dir);
    write_text(json, "{\"format\": 1, \"console\": \"n64\"");
    CHECK(n64_pack_check(dir, key, hex, 1, lib, sizeof lib, why, sizeof why) == N64_PACK_STALE);
    write_text(json, "{\"format\": 1, \"console\": \"n64\"}");
    CHECK(n64_pack_check(dir, key, hex, 1, lib, sizeof lib, why, sizeof why) == N64_PACK_STALE
          && strstr(why, "lacks"));
    snprintf(dir, sizeof dir, "%s/ps1", root);
    make_pack(dir, fake, key, hex, 1, abi, 0);
    snprintf(json, sizeof json, "%s/pack.json", dir);
    char *t = strdup(read_text(json));
    char *c = strstr(t, "\"n64\"");
    memcpy(c, "\"ps1\"", 5);
    write_text(json, t);
    free(t);
    CHECK(n64_pack_check(dir, key, hex, 1, lib, sizeof lib, why, sizeof why) == N64_PACK_STALE
          && strstr(why, "console ps1"));

    snprintf(dir, sizeof dir, "%s/n64-%s", root, key);
    snprintf(json, sizeof json, "%s/libtitle.so", dir);
    setenv("FAKE_PACK_ABI", "99", 1);
    CHECK(n64_pack_load(m, json, key, hex, &p) == N64_PACK_STALE && p.handle == NULL);
    CHECK(n64_pack_open(json, hex, &p, why, sizeof why) == -1 && strstr(why, "abi 99"));
    unsetenv("FAKE_PACK_ABI");

    /* A bare library (no pack.json): the table's own checks only. */
    snprintf(json, sizeof json, "%s/bare.so", root);
    copy_file(fake, json);
    CHECK(n64_pack_load(m, json, key, hex, &p) == N64_PACK_LOADED);
    n64_pack_close(&p);
    CHECK(n64_pack_load(m, json, key, other, &p) == N64_PACK_STALE);

    /* Whatever the pack, the game runs. */
    uint64_t c0 = n64_cycles(m);
    n64_run_cycles(m, 100000);
    CHECK(n64_cycles(m) > c0);
    n64_destroy(m);

    char cmd[1300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0)
        fprintf(stderr, "could not remove %s\n", root);
    if (failures)
        fprintf(stderr, "n64_pack: %d check(s) failed\n", failures);
    else
        printf("n64_pack: all checks passed\n");
    return failures ? 1 : 0;
}
