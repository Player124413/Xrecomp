/* n64_run: runs a Nintendo 64 ROM headless on the core (XBA-169 / 171 / 172).
 *
 *   n64_run ROM [--frames N] [--result] [--ppm OUT.ppm] [--wav OUT.wav] [--tty] [--pad MASK] [--pal | --ntsc]
 *           [--save PATH] [--hash] [--info] [--regs] [--quiet]
 *           [--pack LIB.so [--lockstep] [--missed FILE] [--stats]] [--lockstep-ref]
 *           [--state-save N:FILE] [--state-load FILE] [--run-hash] [--rewind C:R [--rewind-mb M]]
 *           [--cheats FILE] [--peek ADDR] [--perf N]
 *
 * ROM           a .z64 / .v64 / .n64 file (any byte order)
 * --frames N    frames (VI interrupts) to run (default 600); with --result, the most frames to wait
 * --result      read the 512-byte xba_result block of our homebrew (consoles/n64/tests/homebrew/common/xba_result.h,
 *               physical 0x00300000, big-endian) after each frame, stop at the verdict; exit 0 on PASS, 1 on FAIL,
 *               2 on timeout
 * --ppm OUT     write the last frame the VI shows as a binary PPM (black when blank); at the upscaled size when the
 *               Vulkan RDP upscales (RECOMP_N64_RDP=vulkan RECOMP_UPSCALE=2, n64_rdp.h)
 * --fb OUT      write the framebuffer the VI scans, raw (n64_framebuffer: its WIDTH x 480 lines from ORIGIN, no VI
 *               filter), as a binary PPM
 * --wav OUT     write the audio of the run (n64_audio_pull after each frame) as a 48 kHz stereo 16-bit WAV
 * --tty         copy the IS-Viewer debug text (libdragon debugf, osSyncPrintf) to stdout
 * --pad MASK    buttons held on controller 1 for the whole run (N64_BTN_* bits)
 * --pal/--ntsc  force the TV type (default: the ROM's region)
 * --save PATH   cartridge save file: loaded at start if it exists (its size picks the kind), written at exit when the
 *               game wrote it
 * --hash        print a hash of the RDRAM (FNV-1a 64), the instruction and cycle counts
 * --info        print the ROM header and the detected CIC
 * --regs        print the CPU state at the end (PC, COP0 Status / Cause / EPC / BadVAddr, GPRs)
 * --pack LIB    a recompiled title pack (n64_recomp.h: libtitle.so built by n64_tools.recomp /
 *               tools/consoles/n64_recomp_build.py): its lifted code runs natively, the interpreter runs the rest
 * --lockstep    with --pack: an interpreter-only twin runs alongside; after every frame the whole state (CPU, COP0, TLB,
 *               FPU, time, scheduler, MI, RDRAM, RSP memory) and the picture are compared. Prints "LOCKSTEP OK" or
 *               the first difference ("LOCKSTEP FAIL"), exit 1 then
 * --lockstep-ref the interpreter's predecoded blocks against its reference loop (XBA-210): the twin runs step()'s own
 *               loop (n64_set_interp_reference); after every frame the same comparison, then (without --pack) both
 *               machines' whole save states byte for byte (every field, RDRAM, the RSP and the RDP), and the picture
 * --lockstep-cycles N  compare every N CPU cycles instead of every frame (then --frames counts these steps), and
 *               --lockstep-from C runs C cycles before the first comparison (to close in on a divergence). With
 *               --lockstep-ref, --frames still counts frames' worth of cycles (93.75 MHz / 60), run N cycles at a
 *               time: the CPU (n64_cpu_compare) is compared at every stop, the whole machine every 1024 stops and at
 *               the end of each
 * --missed FILE with --pack: append the code the interpreter had to run to FILE (the discovery loop's input)
 * --stats       with --pack: calls, fallbacks, lookups, re-hashes, and the native share of the instructions (packs
 *               built with -DN64_RC_COUNT)
 * --state-save N:FILE  after frame N, write a save state (n64_state_save, XBA-183) to FILE
 * --state-load FILE    load a save state before the first frame (exit 5 when the core refuses it: another ROM,
 *               another version or RDRAM size, not a state)
 * --run-hash    print one hash (FNV-1a 64) of every frame's displayed picture and audio and the final cycle count,
 *               over the frames after --state-save's frame (all the frames without it): a run from a state loaded
 *               with --state-load gives the same hash over the same number of frames (the save state round trip)
 * --rewind C:R  the rewind round trip (XBA-194, recomp_rewind_check): --frames frames with pseudo-random pads, a
 *               capture every C frames as the runtime takes them, R steps back, the picture and every frame after it
 *               must be the first run's; prints "REWIND OK" and the capture cost, exit 6 on a difference.
 *               --rewind-mb M: the ring's cap (default 64)
 * --cheats FILE the cheat list FILE (XBA-197, recomp_cheats.h): its enabled GameShark codes run after every frame
 *               (on both machines with --lockstep)
 * --peek ADDR   after the run, print the 32-bit word at ADDR ("peek 0x80300010 = 0x..."); up to 8 times
 * --perf N      the host performance counters (n64_core.h n64_perf): one frame in N timed stage by stage (1: every
 *               frame), printed at the end ("perf: cpu ... rsp ... rdp ... ms per timed frame"); no effect on the run
 * Prints the frame rate reached on stderr (unless --quiet). Exit 3 on a usage or file error. */
#define _POSIX_C_SOURCE 200809L /* clock_gettime */
#include "n64_core.h"
#include "n64_rdp.h"
#include "n64_recomp.h"
#include "recomp_cheats.h"
#include "recomp_rewind.h"

#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RESULT_ADDR 0xA0300000u
#define RESULT_MAGIC 0x58424152u
#define RESULT_PASS 0x50415353u
#define RESULT_FAIL 0x4641494Cu

static void on_tty(void *ctx, char c)
{
    (void)ctx;
    fputc(c, stdout);
    if (c == '\n')
        fflush(stdout);
}

static uint8_t *load_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n <= 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    uint8_t *buf = malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

static int write_ppm(const char *path, n64_machine *m, bool raw_fb)
{
    struct n64_rdp *rdp = n64_get_rdp(m);
    int sc = rdp ? n64_rdp_scanout_scale(rdp) : 0; /* the Vulkan RDP's upscaled scanout (XBA-185) */
    if (sc < 1)
        sc = 1;
    int mw = N64_SCREEN_MAX_W * sc, mh = N64_SCREEN_MAX_H * sc;
    uint32_t *px = calloc((size_t)mw * (size_t)mh, sizeof *px);
    if (!px)
        return -1;
    int w = 320, h = 240;
    if (raw_fb ? n64_framebuffer(m, px, N64_SCREEN_MAX_W, 480, &w, &h) != 0 : n64_display(m, px, mw, mh, &w, &h) != 0)
        memset(px, 0, (size_t)mw * (size_t)mh * sizeof *px);
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(px);
        return -1;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint8_t rgb[3] = {(uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i]};
        fwrite(rgb, 1, 3, f);
    }
    free(px);
    return fclose(f);
}

static void put32(FILE *f, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    fwrite(b, 1, 4, f);
}

static void put16(FILE *f, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    fwrite(b, 1, 2, f);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static uint32_t cheat_peek32(void *m, uint32_t addr)
{
    return n64_peek32(m, addr);
}

static void cheat_poke32(void *m, uint32_t addr, uint32_t v)
{
    n64_poke32(m, addr, v);
}

static int usage(void)
{
    fprintf(stderr, "usage: n64_run ROM [--frames N] [--result] [--ppm OUT] [--fb OUT] [--wav OUT] [--tty] [--pad MASK]\n"
                    "               [--pal | --ntsc] [--save PATH] [--hash] [--info] [--regs] [--quiet]\n"
                    "               [--pack LIB.so [--lockstep] [--missed FILE] [--stats]] [--lockstep-ref]\n"
                    "               [--state-save N:FILE] [--state-load FILE] [--run-hash] [--cheats FILE]\n"
                    "               [--peek ADDR] [--perf N]\n");
    return 3;
}

static n64_save_kind kind_of_size(size_t n)
{
    switch (n) {
    case 512: return N64_SAVE_EEPROM4K;
    case 2048: return N64_SAVE_EEPROM16K;
    case 0x8000: return N64_SAVE_SRAM;
    case 0x20000: return N64_SAVE_FLASH;
    default: return N64_SAVE_NONE;
    }
}

static uint64_t fnv64(const uint8_t *p, size_t n, uint64_t h)
{
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

/* The picture n64_display shows (at the upscaled size with the Vulkan RDP's scanout), into h. */
static uint64_t picture_hash(n64_machine *m, uint64_t h)
{
    static uint32_t *px;
    static size_t cap;
    struct n64_rdp *rdp = n64_get_rdp(m);
    int sc = rdp ? n64_rdp_scanout_scale(rdp) : 0;
    if (sc < 1)
        sc = 1;
    int mw = N64_SCREEN_MAX_W * sc, mh = N64_SCREEN_MAX_H * sc;
    if (cap < (size_t)mw * (size_t)mh) {
        free(px);
        cap = (size_t)mw * (size_t)mh;
        px = malloc(cap * sizeof *px);
        if (!px) {
            cap = 0;
            return h;
        }
    }
    int w = 0, h2 = 0;
    if (n64_display(m, px, mw, mh, &w, &h2) == 0)
        h = fnv64((const uint8_t *)px, (size_t)w * (size_t)h2 * 4, h);
    h = fnv64((const uint8_t *)&w, sizeof w, h);
    return fnv64((const uint8_t *)&h2, sizeof h2, h);
}

static uint64_t display_hash(n64_machine *m)
{
    static uint32_t px[N64_SCREEN_MAX_W * N64_SCREEN_MAX_H];
    int w = 0, h = 0;
    if (n64_display(m, px, N64_SCREEN_MAX_W, N64_SCREEN_MAX_H, &w, &h) != 0)
        return 0;
    uint64_t hv = 0xCBF29CE484222325ull;
    for (int i = 0; i < w * h; i++) {
        hv ^= px[i];
        hv *= 0x100000001B3ull;
    }
    return hv ^ ((uint64_t)w << 32 | (uint32_t)h);
}


/* Rewind round trip (--rewind, XBA-194): pseudo-random pads that change every few frames, the same for a frame. */
static uint32_t rw_rand(long f)
{
    uint32_t x = (uint32_t)(f / 6 + 1) * 2654435761u;
    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    return x;
}

static uint16_t rw_pad_held;

static void rw_input(void *m, long f)
{
    uint32_t r = rw_rand(f);
    n64_set_controller(m, 0, 1, (uint16_t)(r | rw_pad_held), (int)((r >> 16) & 0x7F) - 64,
                       (int)((r >> 23) & 0x7F) - 64);
}

static void rw_run(void *m)
{
    n64_run_frame(m);
}

static uint64_t rw_picture(void *m)
{
    return picture_hash(m, 0xCBF29CE484222325ull);
}

static uint64_t rw_audio(void *m)
{
    int16_t buf[2048 * 2];
    size_t n;
    uint64_t hv = 0xCBF29CE484222325ull;
    while ((n = n64_audio_pull(m, buf, 2048)) > 0)
        hv = fnv64((const uint8_t *)buf, n * 4, hv);
    return hv;
}

static size_t rw_size(void *m)
{
    return n64_state_size(m);
}

static size_t rw_save(uint8_t *out, size_t cap, void *m)
{
    return n64_state_save(m, out, cap);
}

static int rw_load(const uint8_t *in, size_t n, void *m)
{
    if (n64_state_load(m, in, n) != N64_STATE_OK)
        return -1;
    rw_audio(m); /* what the state had queued was heard already */
    return 0;
}

/* --lockstep-ref: both machines' save states, byte for byte (the first difference's offset in the blob). */
static int state_compare(n64_machine *a, n64_machine *b, char *why, size_t n)
{
    size_t ca = n64_state_size(a), cb = n64_state_size(b);
    uint8_t *sa = malloc(ca), *sb = malloc(cb);
    size_t la = sa ? n64_state_save(a, sa, ca) : 0, lb = sb ? n64_state_save(b, sb, cb) : 0;
    int d = 0;
    if (!la || !lb) {
        d = 1;
        snprintf(why, n, "save state failed");
    } else if (la != lb) {
        d = 1;
        snprintf(why, n, "save state size %zu vs %zu", la, lb);
    } else if (memcmp(sa, sb, la)) {
        size_t i = 0;
        while (sa[i] == sb[i])
            i++;
        d = 1;
        snprintf(why, n, "save state byte %zu: 0x%02X vs 0x%02X", i, sa[i], sb[i]);
    }
    free(sa);
    free(sb);
    return d;
}

int main(int argc, char **argv)
{
    const char *rom_path = NULL, *ppm = NULL, *fb_ppm = NULL, *wav = NULL, *save = NULL, *pack = NULL, *missed = NULL;
    long frames = 600;
    bool result = false, quiet = false, hash = false, tty = false, info = false, regs = false;
    bool lockstep = false, lockref = false, stats = false, run_hash = false;
    int perf_every = 0;
    const char *cheats_path = NULL;
    uint32_t peeks[8];
    int npeek = 0;
    const char *state_save = NULL, *state_load = NULL;
    long state_at = 0;
    long long lock_cycles = 0, lock_from = 0;
    uint16_t pad = 0;
    int rewind_every = 0, rewind_back = 0;
    double rewind_mb = RECOMP_REWIND_MB_DEFAULT;
    n64_options opts = {.force_pal = -1};

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool more = i + 1 < argc;
        if (!strcmp(a, "--frames") && more)
            frames = strtol(argv[++i], NULL, 0);
        else if (!strcmp(a, "--result"))
            result = true;
        else if (!strcmp(a, "--ppm") && more)
            ppm = argv[++i];
        else if (!strcmp(a, "--fb") && more)
            fb_ppm = argv[++i];
        else if (!strcmp(a, "--wav") && more)
            wav = argv[++i];
        else if (!strcmp(a, "--tty"))
            tty = true;
        else if (!strcmp(a, "--pad") && more)
            pad = (uint16_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--pal"))
            opts.force_pal = 1;
        else if (!strcmp(a, "--ntsc"))
            opts.force_pal = 0;
        else if (!strcmp(a, "--save") && more)
            save = argv[++i];
        else if (!strcmp(a, "--hash"))
            hash = true;
        else if (!strcmp(a, "--info"))
            info = true;
        else if (!strcmp(a, "--regs"))
            regs = true;
        else if (!strcmp(a, "--rewind") && more && strchr(argv[i + 1], ':')) {
            rewind_every = (int)strtol(argv[++i], NULL, 10);
            rewind_back = (int)strtol(strchr(argv[i], ':') + 1, NULL, 10);
        }
        else if (!strcmp(a, "--rewind-mb") && more)
            rewind_mb = strtod(argv[++i], NULL);
        else if (!strcmp(a, "--cheats") && more)
            cheats_path = argv[++i];
        else if (!strcmp(a, "--peek") && more && npeek < 8)
            peeks[npeek++] = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--quiet"))
            quiet = true;
        else if (!strcmp(a, "--pack") && more)
            pack = argv[++i];
        else if (!strcmp(a, "--missed") && more)
            missed = argv[++i];
        else if (!strcmp(a, "--lockstep"))
            lockstep = true;
        else if (!strcmp(a, "--lockstep-ref"))
            lockref = true;
        else if (!strcmp(a, "--lockstep-cycles") && more)
            lock_cycles = strtoll(argv[++i], NULL, 0);
        else if (!strcmp(a, "--lockstep-from") && more)
            lock_from = strtoll(argv[++i], NULL, 0);
        else if (!strcmp(a, "--stats"))
            stats = true;
        else if (!strcmp(a, "--perf") && more)
            perf_every = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(a, "--run-hash"))
            run_hash = true;
        else if (!strcmp(a, "--state-load") && more)
            state_load = argv[++i];
        else if (!strcmp(a, "--state-save") && more && strchr(argv[i + 1], ':')) {
            state_at = strtol(argv[++i], NULL, 10);
            state_save = strchr(argv[i], ':') + 1;
        }
        else if (a[0] != '-' && !rom_path)
            rom_path = a;
        else
            return usage();
    }
    if (!rom_path || frames <= 0)
        return usage();

    size_t rom_size = 0;
    uint8_t *rom = load_file(rom_path, &rom_size);
    if (!rom) {
        fprintf(stderr, "n64_run: cannot read %s\n", rom_path);
        return 3;
    }
    size_t save_size = 0;
    uint8_t *save_data = save ? load_file(save, &save_size) : NULL;
    if (save_data)
        opts.save_kind = kind_of_size(save_size);
    char err[256] = "";
    n64_machine *m = n64_create(rom, rom_size, &opts, err, sizeof err);
    n64_machine *twin = NULL;
    bool want_twin = (pack && lockstep) || lockref;
    if (m && want_twin)
        twin = n64_create(rom, rom_size, &opts, err, sizeof err);
    if (twin && lockref)
        n64_set_interp_reference(twin, 1);
    free(rom);
    if (!m || (want_twin && !twin)) {
        fprintf(stderr, "n64_run: %s\n", err);
        free(save_data);
        n64_destroy(m);
        return 3;
    }
    if (save_data) {
        if (n64_save_load(m, opts.save_kind, save_data, save_size) != 0)
            fprintf(stderr, "n64_run: %s: not a save image (%zu bytes)\n", save, save_size);
        if (twin)
            n64_save_load(twin, opts.save_kind, save_data, save_size);
        free(save_data);
    }
    void *lib = NULL;
    if (pack) {
        lib = dlopen(pack, RTLD_NOW | RTLD_LOCAL);
        n64_recomp_title_table_fn get = lib ? (n64_recomp_title_table_fn)dlsym(lib, N64_RECOMP_ENTRY_SYMBOL) : NULL;
        const n64_recomp_table *t = get ? get() : NULL;
        if (!t || n64_attach_recomp(m, t) != 0) {
            fprintf(stderr, "n64_run: %s: %s\n", pack, !lib ? dlerror() : !t ? "no n64_recomp_title_table" : "ABI");
            n64_destroy(m);
            n64_destroy(twin);
            return 3;
        }
        if (!quiet)
            fprintf(stderr, "n64_run: pack %s: %u functions, boot 0x%08X\n", pack, t->count, t->boot_pc);
    }
    if (info) {
        const n64_rom_info *ri = n64_info(m);
        printf("rom: \"%s\" code %s version %u region '%c' (%s) crc %08X %08X boot 0x%08X cic %s (ipl3 crc32 %08X)\n",
               ri->title, ri->game_code, ri->version, ri->region ? ri->region : '?', ri->pal ? "PAL" : "NTSC",
               ri->crc1, ri->crc2, ri->boot_address, n64_cic_name(ri->cic), ri->ipl3_crc);
    }
    if (tty)
        n64_set_tty(m, on_tty, NULL);
    n64_set_controller(m, 0, 1, pad, 0, 0);
    if (twin)
        n64_set_controller(twin, 0, 1, pad, 0, 0);
    bool lock_bad = false;

    if (state_load) {
        size_t n = 0;
        uint8_t *st = load_file(state_load, &n);
        int src = st ? n64_state_load(m, st, n) : N64_STATE_ECORRUPT;
        if (src == N64_STATE_OK && twin)
            src = n64_state_load(twin, st, n);
        free(st);
        if (src != N64_STATE_OK) {
            printf("STATE REFUSED %s: %s\n", state_load,
                   src == N64_STATE_EGAME ? "another ROM" : src == N64_STATE_EVERSION ? "another version"
                                                                                       : "not a state");
            n64_destroy(m);
            n64_destroy(twin);
            return 5;
        }
        if (!quiet)
            fprintf(stderr, "n64_run: state %s loaded (%zu bytes, frame %llu)\n", state_load, n,
                    (unsigned long long)n64_frames(m));
    }
    if (rewind_every > 0) {
        rw_pad_held = pad;
        recomp_rewind_machine mc = {m, rw_input, rw_run, rw_picture, rw_audio, rw_size, rw_save, rw_load};
        int r = recomp_rewind_check("n64", &mc, frames, rewind_every, rewind_back > 0 ? rewind_back : 1,
                                    (size_t)((rewind_mb > 0 ? rewind_mb : 1) * 1048576.0));
        n64_destroy(m);
        n64_destroy(twin);
        return r == 0 ? 0 : r == 1 ? 6 : 3;
    }
    uint64_t rh = 0xCBF29CE484222325ull;
    long rh_frames = 0;

    FILE *wf = NULL;
    uint32_t wav_frames = 0;
    if (wav) {
        wf = fopen(wav, "wb");
        if (!wf) {
            fprintf(stderr, "n64_run: cannot write %s\n", wav);
            n64_destroy(m);
            return 3;
        }
        fwrite("RIFF\0\0\0\0WAVEfmt ", 1, 16, wf);
        put32(wf, 16);
        put16(wf, 1);
        put16(wf, 2);
        put32(wf, N64_AUDIO_RATE);
        put32(wf, N64_AUDIO_RATE * 4);
        put16(wf, 4);
        put16(wf, 16);
        fwrite("data\0\0\0\0", 1, 8, wf);
    }

    int rc = result ? 2 : 0;
    if (perf_every > 0)
        n64_perf_enable(m, perf_every);
    double t0 = now_s();
    long f;
    if (twin && lock_from > 0) {
        n64_run_cycles(m, (uint64_t)lock_from);
        n64_run_cycles(twin, (uint64_t)lock_from);
    }
    recomp_cheats cheats, cheats_twin;
    recomp_cheats_init(&cheats, RECOMP_CHEAT_N64);
    recomp_cheats_init(&cheats_twin, RECOMP_CHEAT_N64);
    if (cheats_path) {
        recomp_cheats_load(&cheats, cheats_path);
        if (twin)
            recomp_cheats_load(&cheats_twin, cheats_path);
    }
    recomp_cheat_bus cbus = {m, NULL, NULL, cheat_peek32, cheat_poke32, 1, NULL, 0};
    recomp_cheat_bus cbus_twin = {twin, NULL, NULL, cheat_peek32, cheat_poke32, 1, NULL, 0};
    long lock_stops = 0;
    for (f = 0; f < frames; f++) {
        if (twin && lockref && lock_cycles > 0) {
            uint64_t end = n64_cycles(m) + 93750000u / 60; /* N64_CPU_HZ */
            char why[160] = "";
            while (n64_cycles(m) < end) {
                uint64_t pa = n64_pc(m);
                n64_run_cycles(m, (uint64_t)lock_cycles);
                n64_run_cycles(twin, (uint64_t)lock_cycles);
                bool full = ++lock_stops % 1024 == 0 || n64_cycles(m) >= end;
                int d = n64_cpu_compare(m, twin, why, sizeof why);
                if (!d && full) {
                    d = n64_recomp_compare(m, twin, why, sizeof why);
                    if (!d && !pack)
                        d = state_compare(m, twin, why, sizeof why);
                }
                if (d) {
                    printf("LOCKSTEP FAIL at frame %ld, cycle %llu: %s (blocks vs reference interpreter; pc before "
                           "0x%016llX, after 0x%016llX vs 0x%016llX)\n", f + 1, (unsigned long long)n64_cycles(m), why,
                           (unsigned long long)pa, (unsigned long long)n64_pc(m), (unsigned long long)n64_pc(twin));
                    lock_bad = true;
                    break;
                }
            }
            if (lock_bad) {
                f++;
                break;
            }
            continue;
        }
        if (twin && lock_cycles > 0)
            n64_run_cycles(m, (uint64_t)lock_cycles);
        else
            n64_run_frame(m);
        if (cheats.n)
            recomp_cheats_apply(&cheats, &cbus);
        if (twin) {
            if (lock_cycles > 0)
                n64_run_cycles(twin, (uint64_t)lock_cycles);
            else
                n64_run_frame(twin);
            if (cheats_twin.n)
                recomp_cheats_apply(&cheats_twin, &cbus_twin);
            char why[160] = "";
            int d = n64_recomp_compare(m, twin, why, sizeof why);
            if (!d && lockref && !pack)
                d = state_compare(m, twin, why, sizeof why);
            if (!d && display_hash(m) != display_hash(twin)) {
                d = 1;
                snprintf(why, sizeof why, "the picture");
            }
            if (d) {
                printf("LOCKSTEP FAIL at frame %ld: %s (%s; pc 0x%016llX vs 0x%016llX, "
                       "instructions %llu vs %llu, cycles %llu vs %llu)\n",
                       f + 1, why, pack ? "recompiled vs interpreter" : "blocks vs reference interpreter", (unsigned long long)n64_pc(m), (unsigned long long)n64_pc(twin),
                       (unsigned long long)n64_instructions(m), (unsigned long long)n64_instructions(twin),
                       (unsigned long long)n64_cycles(m), (unsigned long long)n64_cycles(twin));
                lock_bad = true;
                f++;
                break;
            }
        }
        int counted = run_hash && (!state_save || f + 1 > state_at);
        if (counted) {
            rh = picture_hash(m, rh);
            rh_frames++;
        }
        if (wf || run_hash) {
            int16_t buf[2048 * 2];
            size_t n;
            while ((n = n64_audio_pull(m, buf, 2048)) > 0) {
                if (counted)
                    rh = fnv64((const uint8_t *)buf, n * 4, rh);
                if (!wf)
                    continue;
                for (size_t i = 0; i < n * 2; i++)
                    put16(wf, (uint16_t)buf[i]);
                wav_frames += (uint32_t)n;
            }
        }
        if (state_save && f + 1 == state_at) {
            size_t cap = n64_state_size(m), n = 0;
            uint8_t *st = malloc(cap);
            FILE *sf = st ? fopen(state_save, "wb") : NULL;
            if (!st || !(n = n64_state_save(m, st, cap)) || !sf || fwrite(st, 1, n, sf) != n || fclose(sf) != 0) {
                fprintf(stderr, "n64_run: cannot write the state %s\n", state_save);
                return 3;
            }
            free(st);
            if (!quiet)
                fprintf(stderr, "n64_run: state written to %s after frame %ld (%zu bytes)\n", state_save, f + 1, n);
        }
        if (result && n64_peek32(m, RESULT_ADDR) == RESULT_MAGIC) {
            uint32_t st = n64_peek32(m, RESULT_ADDR + 4);
            if (st == RESULT_PASS || st == RESULT_FAIL) {
                rc = st == RESULT_PASS ? 0 : 1;
                f++;
                break;
            }
        }
    }
    double dt = now_s() - t0;
    if (run_hash) {
        uint64_t cyc = n64_cycles(m);
        rh = fnv64((const uint8_t *)&cyc, sizeof cyc, rh);
        printf("run-hash %016llx frames %ld\n", (unsigned long long)rh, rh_frames);
    }
    if (twin && !lock_bad)
        printf("LOCKSTEP OK (%ld frames, the whole state identical after each)\n", f);
    if (pack && missed) {
        int n = n64_recomp_write_missed(m, missed);
        if (n < 0)
            fprintf(stderr, "n64_run: cannot write %s\n", missed);
        else if (!quiet)
            fprintf(stderr, "n64_run: %d new missed records -> %s\n", n, missed);
    }
    if (perf_every > 0) {
        n64_perf p;
        n64_perf_get(m, &p);
        double tf = p.timed ? 1e6 * (double)p.timed : 1.0, nf = p.frames ? (double)p.frames : 1.0;
        double other = (double)p.frame_ns - (double)p.cpu_ns - (double)p.rsp_ns - (double)p.rdp_ns;
        printf("perf: cpu %.2f rsp %.2f rdp %.2f sched %.2f (frame %.2f) ms per timed frame (%llu of %llu), "
               "rdp wait %.2f ms/frame (%llu waits; queue %.2f, scanout %.2f ms/frame); %.0f RSP instructions, "
               "%.0f RDP commands, %.2f full syncs per frame; %llu stalled compiles (%.0f ms)\n",
               (double)p.cpu_ns / tf, (double)p.rsp_ns / tf, (double)p.rdp_ns / tf, other / tf,
               (double)p.frame_ns / tf, (unsigned long long)p.timed, (unsigned long long)p.frames,
               (double)p.rdp_wait_ns / 1e6 / nf, (unsigned long long)p.rdp_waits, (double)p.rdp_queue_ns / 1e6 / nf,
               (double)p.rdp_scanout_ns / 1e6 / nf, (double)p.rsp_instructions / nf, (double)p.rdp_commands / nf,
               (double)p.rdp_full_syncs / nf, (unsigned long long)p.rdp_stalled_compiles,
               (double)p.rdp_stalled_compile_ns / 1e6);
        printf("perf: DP %llu deferred, %llu at once, %llu early, %llu backoffs; %llu deadline waits %.1f ms; %llu "
               "early scanouts of %llu scanouts (%llu started)\n",
               (unsigned long long)p.dp_deferred, (unsigned long long)p.dp_now, (unsigned long long)p.dp_early,
               (unsigned long long)p.dp_backoffs, (unsigned long long)p.rdp_dp_waits, (double)p.rdp_dp_wait_ns / 1e6,
               (unsigned long long)p.rdp_prefetch_hits, (unsigned long long)p.rdp_scanouts,
               (unsigned long long)p.rdp_prefetches);
    }
    if (pack && stats) {
        n64_recomp_stats st;
        n64_recomp_get_stats(m, &st);
        uint64_t total = n64_instructions(m);
        printf("recomp: %llu calls, %llu interpreted RDRAM instructions (%llu fallbacks, %llu blocks), %llu slow "
               "lookups, %llu re-hashes (%llu stale), %llu interrupts\n",
               (unsigned long long)st.calls, (unsigned long long)st.interp_ram, (unsigned long long)st.fallbacks,
               (unsigned long long)st.blocks, (unsigned long long)st.slow_lookups, (unsigned long long)st.rehashes, (unsigned long long)st.stale,
               (unsigned long long)st.interrupts);
        if (st.native)
            printf("recomp: %.2f%% native (%llu of %llu instructions)\n", total ? 100.0 * st.native / total : 0.0,
                   (unsigned long long)st.native, (unsigned long long)total);
    }

    if (result) {
        uint32_t magic = n64_peek32(m, RESULT_ADDR), st = n64_peek32(m, RESULT_ADDR + 4);
        uint32_t id = n64_peek32(m, RESULT_ADDR + 8), detail = n64_peek32(m, RESULT_ADDR + 12);
        uint32_t count = n64_peek32(m, RESULT_ADDR + 20);
        char fourcc[5] = {(char)(id >> 24), (char)(id >> 16), (char)(id >> 8), (char)id, 0};
        for (int i = 0; i < 4; i++)
            if (fourcc[i] < 32 || fourcc[i] > 126)
                fourcc[i] = '?';
        printf("result: %s test=%s detail=%u checksum=%08X count=%u frames=%u stage=%u (%ld frames run)\n",
               magic != RESULT_MAGIC ? "NOBLOCK" : st == RESULT_PASS ? "PASS" : st == RESULT_FAIL ? "FAIL" : "TIMEOUT",
               fourcc, detail, n64_peek32(m, RESULT_ADDR + 16), count, n64_peek32(m, RESULT_ADDR + 24),
               n64_peek32(m, RESULT_ADDR + 28), f);
        if (count > 120)
            count = 120;
        for (uint32_t i = 0; i < count && (st == RESULT_FAIL || !quiet); i++)
            printf("  table[%u] = 0x%08X\n", i, n64_peek32(m, RESULT_ADDR + 32 + 4 * i));
        if (magic != RESULT_MAGIC || (st != RESULT_PASS && st != RESULT_FAIL))
            printf("  pc 0x%016llX, %llu instructions\n", (unsigned long long)n64_pc(m),
                   (unsigned long long)n64_instructions(m));
    }
    if (ppm && write_ppm(ppm, m, false) != 0)
        fprintf(stderr, "n64_run: cannot write %s\n", ppm);
    if (fb_ppm && write_ppm(fb_ppm, m, true) != 0)
        fprintf(stderr, "n64_run: cannot write %s\n", fb_ppm);
    if (wf) {
        fseek(wf, 4, SEEK_SET);
        put32(wf, 36 + wav_frames * 4);
        fseek(wf, 40, SEEK_SET);
        put32(wf, wav_frames * 4);
        fclose(wf);
    }
    if (save && n64_save_take_dirty(m, n64_save_kind_detected(m))) {
        size_t n = 0;
        const uint8_t *d = n64_save_data(m, n64_save_kind_detected(m), &n);
        FILE *sf = d ? fopen(save, "wb") : NULL;
        if (!sf || fwrite(d, 1, n, sf) != n)
            fprintf(stderr, "n64_run: cannot write %s\n", save);
        if (sf)
            fclose(sf);
    }
    if (regs) {
        printf("pc %016llX status %08llX cause %08llX epc %016llX badvaddr %016llX count %08llX\n",
               (unsigned long long)n64_pc(m), (unsigned long long)n64_cop0(m, 12), (unsigned long long)n64_cop0(m, 13),
               (unsigned long long)n64_cop0(m, 14), (unsigned long long)n64_cop0(m, 8),
               (unsigned long long)n64_cop0(m, 9));
        for (int r = 0; r < 32; r += 4)
            printf("r%-2d %016llX %016llX %016llX %016llX\n", r, (unsigned long long)n64_reg(m, r),
                   (unsigned long long)n64_reg(m, r + 1), (unsigned long long)n64_reg(m, r + 2),
                   (unsigned long long)n64_reg(m, r + 3));
    }
    if (hash) {
        uint64_t h = 0xCBF29CE484222325ull;
        const uint8_t *p = n64_rdram(m);
        for (uint32_t i = 0; i < n64_rdram_size(m); i++) {
            h ^= p[i];
            h *= 0x100000001B3ull;
        }
        printf("hash: rdram=%016llx instructions=%llu cycles=%llu\n", (unsigned long long)h,
               (unsigned long long)n64_instructions(m), (unsigned long long)n64_cycles(m));
    }
    if (!quiet)
        fprintf(stderr, "n64_run: %ld frames in %.2f s (%.1f fps, %.1f MIPS, %.1f MHz emulated)\n", f, dt,
                dt > 0 ? f / dt : 0.0, dt > 0 ? (double)n64_instructions(m) / dt / 1e6 : 0.0,
                dt > 0 ? (double)n64_cycles(m) / dt / 1e6 : 0.0);
    for (int i = 0; i < npeek; i++)
        printf("peek 0x%08X = 0x%08X\n", peeks[i], n64_peek32(m, peeks[i]));
    recomp_cheats_free(&cheats);
    recomp_cheats_free(&cheats_twin);
    n64_destroy(m);
    n64_destroy(twin);
    /* lib stays loaded until exit (no dlclose: nothing may call into it after the machines are gone anyway) */
    (void)lib;
    return lock_bad ? 1 : rc;
}
