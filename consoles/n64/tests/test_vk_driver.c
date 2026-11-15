/* test_vk_driver.c -- the custom Vulkan driver selection of the N64 RDP (n64_vk_driver.h): the RECOMP_VK_* variables,
 * the trial marker, last_start and <id>.status; and the pipeline cache file (n64_vk_cache.h: RECOMP_VK_CACHE_DIR, load,
 * save, truncated / foreign / corrupt files, MESA_SHADER_CACHE_DIR). Off Android libxba_vkdriver.so does not exist, so every open that gets
 * that far fails as a driver that cannot be opened on a device does: the system-driver path (open_failed). With
 * --wrapper, a fake libxba_vkdriver.so (fake_vkdriver.c, found through LD_LIBRARY_PATH / DYLD_LIBRARY_PATH) opens it:
 * the outcomes the RDP reports afterwards (ok, unsupported, not_loaded). No Vulkan needed. */
#include "n64_vk_cache.h"
#include "n64_vk_driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(c)                                                                                                        \
    do {                                                                                                                \
        if (!(c)) {                                                                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                \
            fails++;                                                                                                    \
        }                                                                                                               \
    } while (0)

static char root[512], drv[600];

static int read_line(const char *name, char *out, size_t n)
{
    char p[700];
    snprintf(p, sizeof p, "%s/%s", root, name);
    FILE *f = fopen(p, "r");
    if (!f)
        return 0;
    if (!fgets(out, (int)n, f))
        out[0] = 0;
    fclose(f);
    out[strcspn(out, "\n")] = 0;
    return 1;
}

static void write_line(const char *name, const char *text)
{
    char p[700];
    snprintf(p, sizeof p, "%s/%s", root, name);
    FILE *f = fopen(p, "w");
    fputs(text, f);
    fclose(f);
}

static void clear(void)
{
    char p[700];
    snprintf(p, sizeof p, "%s/trying", root);
    remove(p);
    snprintf(p, sizeof p, "%s/last_start", root);
    remove(p);
    snprintf(p, sizeof p, "%s/abc123.status", root);
    remove(p);
    snprintf(p, sizeof p, "%s/myid.status", root);
    remove(p);
}

static long file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static void truncate_to(const char *path, long n)
{
    FILE *f = fopen(path, "rb");
    unsigned char buf[4096];
    size_t got = f ? fread(buf, 1, sizeof buf, f) : 0;
    if (f)
        fclose(f);
    f = fopen(path, "wb");
    fwrite(buf, 1, (size_t)n < got ? (size_t)n : got, f);
    fclose(f);
}

/* n64_vk_cache.h: the directory (created, nested), the file's name, a save then a load, and every file that must be
 * ignored rather than handed to the driver. */
static void cache_tests(const char *base)
{
    char dir[700], path[900], want[900];
    const char *why = "";
    size_t n = 0;
    uint8_t uuid[N64_VK_CACHE_UUID_SIZE], other[N64_VK_CACHE_UUID_SIZE];
    for (int i = 0; i < N64_VK_CACHE_UUID_SIZE; i++) {
        uuid[i] = (uint8_t)(0xA0 + i);
        other[i] = (uint8_t)i;
    }
    uint8_t blob[1000];
    for (size_t i = 0; i < sizeof blob; i++)
        blob[i] = (uint8_t)(i * 7 + 3);

    /* unset or empty: no directory, no file, no Mesa variable */
    unsetenv("RECOMP_VK_CACHE_DIR");
    unsetenv("MESA_SHADER_CACHE_DIR");
    CHECK(!n64_vk_cache_dir(dir, sizeof dir));
    CHECK(!n64_vk_cache_file(path, sizeof path, uuid));
    n64_vk_cache_mesa_env();
    CHECK(getenv("MESA_SHADER_CACHE_DIR") == NULL);
    setenv("RECOMP_VK_CACHE_DIR", "", 1);
    CHECK(!n64_vk_cache_dir(dir, sizeof dir));

    /* nested and missing, without the trailing '/': created, '/' added */
    char set[700];
    snprintf(set, sizeof set, "%s/cache/vk_cache/system", base);
    setenv("RECOMP_VK_CACHE_DIR", set, 1);
    CHECK(n64_vk_cache_dir(dir, sizeof dir));
    snprintf(want, sizeof want, "%s/", set);
    CHECK(!strcmp(dir, want));
    struct stat st;
    CHECK(stat(set, &st) == 0 && S_ISDIR(st.st_mode));
    /* a buffer too small for it: refused, not cut */
    char small[8];
    CHECK(!n64_vk_cache_dir(small, sizeof small));

    CHECK(n64_vk_cache_file(path, sizeof path, uuid));
    snprintf(want, sizeof want, "%s/pipelines-a0a1a2a3a4a5a6a7a8a9aaabacadaeaf.bin", set);
    CHECK(!strcmp(path, want));

    /* no file yet */
    CHECK(n64_vk_cache_load(path, uuid, &n, &why) == NULL && !strcmp(why, "none") && n == 0);

    /* saved, loaded back byte for byte; no temporary file left */
    CHECK(n64_vk_cache_save(path, uuid, blob, sizeof blob) == 0);
    CHECK(file_size(path) == N64_VK_CACHE_HEADER + (long)sizeof blob);
    uint8_t *got = n64_vk_cache_load(path, uuid, &n, &why);
    CHECK(got && n == sizeof blob && !memcmp(got, blob, n) && !strcmp(why, "ok"));
    free(got);
    char tmpf[1000];
    snprintf(tmpf, sizeof tmpf, "%s.tmp%ld", path, (long)getpid());
    CHECK(access(tmpf, F_OK) != 0);

    /* saved again (a larger cache): replaced whole */
    uint8_t big[3000];
    for (size_t i = 0; i < sizeof big; i++)
        big[i] = (uint8_t)(i ^ 0x5A);
    CHECK(n64_vk_cache_save(path, uuid, big, sizeof big) == 0);
    got = n64_vk_cache_load(path, uuid, &n, &why);
    CHECK(got && n == sizeof big && !memcmp(got, big, n));
    free(got);

    /* another device or driver (UUID) */
    CHECK(n64_vk_cache_load(path, other, &n, &why) == NULL && !strcmp(why, "another device or driver"));

    /* truncated inside the payload, inside the header, empty */
    truncate_to(path, N64_VK_CACHE_HEADER + 100);
    CHECK(n64_vk_cache_load(path, uuid, &n, &why) == NULL && !strcmp(why, "truncated") && n == 0);
    truncate_to(path, 20);
    CHECK(n64_vk_cache_load(path, uuid, &n, &why) == NULL && !strcmp(why, "truncated"));
    truncate_to(path, 0);
    CHECK(n64_vk_cache_load(path, uuid, &n, &why) == NULL && !strcmp(why, "truncated"));

    /* a longer file, a flipped payload byte, another format */
    CHECK(n64_vk_cache_save(path, uuid, blob, sizeof blob) == 0);
    FILE *f = fopen(path, "ab");
    fputc(0, f);
    fclose(f);
    CHECK(n64_vk_cache_load(path, uuid, &n, &why) == NULL && !strcmp(why, "not a cache file"));
    CHECK(n64_vk_cache_save(path, uuid, blob, sizeof blob) == 0);
    f = fopen(path, "r+b");
    fseek(f, N64_VK_CACHE_HEADER + 500, SEEK_SET);
    fputc(blob[500] ^ 1, f);
    fclose(f);
    CHECK(n64_vk_cache_load(path, uuid, &n, &why) == NULL && !strcmp(why, "checksum"));
    f = fopen(path, "wb");
    fputs("Granite's own pipeline_cache.bin, or anything else of some length", f);
    fclose(f);
    CHECK(n64_vk_cache_load(path, uuid, &n, &why) == NULL && !strcmp(why, "not a cache file"));
    /* and the next save replaces it */
    CHECK(n64_vk_cache_save(path, uuid, blob, sizeof blob) == 0);
    got = n64_vk_cache_load(path, uuid, &n, &why);
    CHECK(got && n == sizeof blob);
    free(got);

    /* a directory that cannot be written: the save fails, nothing is left */
    char ro[800], ropath[900];
    snprintf(ro, sizeof ro, "%s/cache/ro", base);
    mkdir(ro, 0500);
    snprintf(ropath, sizeof ropath, "%s/pipelines-x.bin", ro);
    if (geteuid() != 0) /* root writes anywhere */
        CHECK(n64_vk_cache_save(ropath, uuid, blob, sizeof blob) != 0);
    CHECK(access(ropath, F_OK) != 0 || geteuid() == 0);
    remove(ropath);
    rmdir(ro);

    /* Mesa's disk cache: <dir>mesa, created; a value already set wins */
    n64_vk_cache_mesa_env();
    snprintf(want, sizeof want, "%s/mesa", set);
    CHECK(getenv("MESA_SHADER_CACHE_DIR") && !strcmp(getenv("MESA_SHADER_CACHE_DIR"), want));
    CHECK(stat(want, &st) == 0 && S_ISDIR(st.st_mode));
    setenv("MESA_SHADER_CACHE_DIR", "/elsewhere", 1);
    n64_vk_cache_mesa_env();
    CHECK(!strcmp(getenv("MESA_SHADER_CACHE_DIR"), "/elsewhere"));
    unsetenv("MESA_SHADER_CACHE_DIR");
    unsetenv("RECOMP_VK_CACHE_DIR");

    remove(path);
    rmdir(want);
    rmdir(set);
    snprintf(want, sizeof want, "%s/cache/vk_cache", base);
    rmdir(want);
    snprintf(want, sizeof want, "%s/cache", base);
    rmdir(want);
}

/* The fake wrapper opens the driver: the marker is written, n64_vk_driver_done gives the outcome. */
static int wrapper_tests(void)
{
    char line[256], lib[700];
    snprintf(lib, sizeof lib, "%s/vulkan.test.so", drv);
    FILE *f = fopen(lib, "w");
    fputs("x", f);
    fclose(f);
    setenv("RECOMP_VK_DRIVER_DIR", drv, 1);
    setenv("RECOMP_VK_DRIVER_LIB", "vulkan.test.so", 1);
    setenv("RECOMP_VK_DRIVER_ID", "myid", 1);
    static const char *outcomes[] = { "ok", "unsupported", "not_loaded" };
    for (int i = 0; i < 3; i++) {
        void *gipa = n64_vk_driver_open();
        CHECK(gipa != NULL);
        if (!gipa)
            break;
        CHECK(read_line("trying", line, sizeof line) && !strcmp(line, "myid"));
        n64_vk_driver_done(outcomes[i]);
        char want[64];
        snprintf(want, sizeof want, "myid %s", outcomes[i]);
        CHECK(read_line("last_start", line, sizeof line) && !strcmp(line, want));
        CHECK(read_line("myid.status", line, sizeof line) && !strcmp(line, outcomes[i]));
        CHECK(!read_line("trying", line, sizeof line));
        n64_vk_driver_done("ok"); /* once only */
        CHECK(read_line("myid.status", line, sizeof line) && !strcmp(line, outcomes[i]));
    }
    /* another driver's start afterwards: its own status file, myid's stays (the app reads both) */
    setenv("RECOMP_VK_DRIVER_ID", "abc123", 1);
    CHECK(n64_vk_driver_open() != NULL);
    n64_vk_driver_done("unsupported");
    CHECK(read_line("last_start", line, sizeof line) && !strcmp(line, "abc123 unsupported"));
    CHECK(read_line("abc123.status", line, sizeof line) && !strcmp(line, "unsupported"));
    CHECK(read_line("myid.status", line, sizeof line) && !strcmp(line, "not_loaded"));
    /* an id that cannot name a file: last_start only */
    setenv("RECOMP_VK_DRIVER_ID", "../x", 1);
    CHECK(n64_vk_driver_open() != NULL);
    n64_vk_driver_done("ok");
    CHECK(read_line("last_start", line, sizeof line) && !strcmp(line, "../x ok"));
    char p[700];
    snprintf(p, sizeof p, "%s/../x.status", root);
    CHECK(access(p, F_OK) != 0);
    clear();
    remove(lib);
    return 0;
}

int main(int argc, char **argv)
{
    const int wrapper = argc > 1 && !strcmp(argv[1], "--wrapper");
    char line[256];
    const char *tmp = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/n64vkdXXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(root)) {
        perror("mkdtemp");
        return 1;
    }
    char base[520];
    snprintf(base, sizeof base, "%s", root);
    snprintf(root, sizeof root, "%s/vk_drivers", base); /* the marker and last_start live here */
    mkdir(root, 0700);
    snprintf(drv, sizeof drv, "%s/abc123", root);
    mkdir(drv, 0700);
    unsetenv("RECOMP_VK_DRIVER");
    unsetenv("RECOMP_VK_DRIVER_DIR");
    unsetenv("RECOMP_VK_DRIVER_LIB");
    unsetenv("RECOMP_VK_DRIVER_ID");
    unsetenv("RECOMP_VK_HOOK_DIR");

    if (wrapper) {
        wrapper_tests();
        rmdir(drv);
        rmdir(root);
        rmdir(base);
        if (fails) {
            fprintf(stderr, "n64_vk_driver --wrapper: %d failure(s)\n", fails);
            return 1;
        }
        printf("n64_vk_driver --wrapper: ok\n");
        return 0;
    }

    cache_tests(base);

    /* nothing selected: no open, no file */
    CHECK(!n64_vk_driver_selected());
    CHECK(n64_vk_driver_open() == NULL);
    CHECK(!read_line("last_start", line, sizeof line));

    /* selected, forced back to the system driver */
    setenv("RECOMP_VK_DRIVER_DIR", drv, 1); /* no trailing '/': added */
    setenv("RECOMP_VK_DRIVER_LIB", "vulkan.test.so", 1);
    CHECK(n64_vk_driver_selected());
    setenv("RECOMP_VK_DRIVER", "system", 1);
    CHECK(!n64_vk_driver_selected());
    CHECK(n64_vk_driver_open() == NULL);
    CHECK(!read_line("last_start", line, sizeof line));
    unsetenv("RECOMP_VK_DRIVER");

    /* the library is missing: open_failed, id from the directory's name; its own status file too */
    CHECK(n64_vk_driver_open() == NULL);
    CHECK(!strcmp(n64_vk_driver_id(), "abc123"));
    CHECK(read_line("last_start", line, sizeof line) && !strcmp(line, "abc123 open_failed"));
    CHECK(read_line("abc123.status", line, sizeof line) && !strcmp(line, "open_failed"));
    CHECK(!read_line("trying", line, sizeof line));
    clear();

    /* a library file name with a '/' is refused */
    setenv("RECOMP_VK_DRIVER_LIB", "../x.so", 1);
    CHECK(n64_vk_driver_open() == NULL);
    CHECK(read_line("last_start", line, sizeof line) && !strcmp(line, "abc123 open_failed"));
    setenv("RECOMP_VK_DRIVER_LIB", "vulkan.test.so", 1);
    clear();

    /* the library is there, libxba_vkdriver.so is not (desktop): open_failed, the marker written then removed */
    char lib[700];
    snprintf(lib, sizeof lib, "%s/vulkan.test.so", drv);
    FILE *f = fopen(lib, "w");
    fputs("not an ELF", f);
    fclose(f);
    setenv("RECOMP_VK_DRIVER_ID", "myid", 1);
    char slash[700];
    snprintf(slash, sizeof slash, "%s/", drv);
    setenv("RECOMP_VK_DRIVER_DIR", slash, 1); /* with the trailing '/' */
    CHECK(n64_vk_driver_open() == NULL);
    CHECK(!strcmp(n64_vk_driver_id(), "myid"));
    CHECK(read_line("last_start", line, sizeof line) && !strcmp(line, "myid open_failed"));
    CHECK(!read_line("trying", line, sizeof line));
    clear();

    /* a marker for another driver: ignored and removed */
    write_line("trying", "other\n");
    CHECK(n64_vk_driver_open() == NULL);
    CHECK(read_line("last_start", line, sizeof line) && !strcmp(line, "myid open_failed"));
    CHECK(!read_line("trying", line, sizeof line));
    clear();

    /* the marker of a previous start of this driver (it crashed): skipped, the marker stays for the app */
    write_line("trying", "myid\n");
    CHECK(n64_vk_driver_open() == NULL);
    CHECK(read_line("last_start", line, sizeof line) && !strcmp(line, "myid crashed"));
    CHECK(read_line("myid.status", line, sizeof line) && !strcmp(line, "crashed"));
    CHECK(read_line("trying", line, sizeof line) && !strcmp(line, "myid"));
    n64_vk_driver_done("ok"); /* nothing was opened: no effect */
    CHECK(read_line("trying", line, sizeof line) && !strcmp(line, "myid"));
    CHECK(read_line("last_start", line, sizeof line) && !strcmp(line, "myid crashed"));
    clear();

    /* outside a vk_drivers directory: no marker, no last_start */
    char other[700], olib[760];
    snprintf(other, sizeof other, "%s/elsewhere", base);
    mkdir(other, 0700);
    snprintf(olib, sizeof olib, "%s/vulkan.test.so", other);
    f = fopen(olib, "w");
    fputs("x", f);
    fclose(f);
    setenv("RECOMP_VK_DRIVER_DIR", other, 1);
    CHECK(n64_vk_driver_open() == NULL);
    char p2[760];
    snprintf(p2, sizeof p2, "%s/last_start", base);
    CHECK(access(p2, F_OK) != 0);
    snprintf(p2, sizeof p2, "%s/trying", base);
    CHECK(access(p2, F_OK) != 0);
    remove(olib);
    rmdir(other);

    remove(lib);
    rmdir(drv);
    rmdir(root);
    rmdir(base);
    if (fails) {
        fprintf(stderr, "n64_vk_driver: %d failure(s)\n", fails);
        return 1;
    }
    printf("n64_vk_driver: ok\n");
    return 0;
}
