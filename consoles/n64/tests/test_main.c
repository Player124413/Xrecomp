/* The Nintendo 64 runtime (XBA-146, XBA-174): its pure helpers (pad mapping, stick, colour conversion, save paths,
 * fallback key, frame length, pak names) and headless runs (SDL's dummy drivers) of our homebrew: gfx.z64's picture,
 * the same ROM through "fd:N" (the app shell's documents) and byte-swapped (.v64), saves on disk read back and left
 * alone while unchanged, the app shell's entry, and the errors; save states (XBA-183) saved and loaded through
 * recomp_states.h while the game runs on its own thread, the automatic slot resumed, another game's state refused.
 *
 *   n64_runtime_test [HOMEBREW_DIR]     (the runs skip, the helpers still run, when gfx.z64 is absent) */
#include "host_pad.h"
#include "host_window.h"
#include "host_time.h"
#include "n64_core.h"
#include "n64_runtime.h"
#include "recomp_lifecycle.h"
#include "recomp_states.h"

#include <pthread.h>

#include <SDL.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                               \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

/* The display options (XBA-181): the runtime's own keys win over the shared ones, which win over its defaults;
 * every key accepts both filters and the three scales. */
static void test_display(void)
{
    n64_runtime_opts o;
    int scale = -1, filter = -1;

    n64_runtime_opts_init(&o);
    unsetenv("RECOMP_N64_SCALE");
    unsetenv("RECOMP_N64_FILTER");
    unsetenv("RECOMP_SCALE");
    unsetenv("RECOMP_FILTER");
    n64_runtime_display(&o, &scale, &filter);
    CHECK(scale == HOST_SCALE_ASPECT && filter == HOST_FILTER_LINEAR);
    setenv("RECOMP_SCALE", "stretch", 1);
    setenv("RECOMP_FILTER", "nearest", 1);
    n64_runtime_display(&o, &scale, &filter);
    CHECK(scale == HOST_SCALE_STRETCH && filter == HOST_FILTER_NEAREST);
    setenv("RECOMP_N64_SCALE", "integer", 1);
    setenv("RECOMP_N64_FILTER", "linear", 1);
    n64_runtime_display(&o, &scale, &filter);
    CHECK(scale == HOST_SCALE_INTEGER && filter == HOST_FILTER_LINEAR);
    setenv("RECOMP_N64_FILTER", "nearest", 1);
    n64_runtime_display(NULL, NULL, &filter);
    CHECK(filter == HOST_FILTER_NEAREST);
    o.scale = HOST_SCALE_ASPECT;   /* the caller's choice (a player's --scale) wins over every key */
    o.filter = HOST_FILTER_LINEAR;
    n64_runtime_display(&o, &scale, &filter);
    CHECK(scale == HOST_SCALE_ASPECT && filter == HOST_FILTER_LINEAR);
    unsetenv("RECOMP_N64_SCALE");
    unsetenv("RECOMP_N64_FILTER");
    unsetenv("RECOMP_SCALE");
    unsetenv("RECOMP_FILTER");

    /* The VI's guard band: cropped by default, RECOMP_N64_CROP_EDGES=0 shows it, the caller's choice wins; 8 VI
     * pixels a side whatever the picture's width. */
    n64_runtime_opts_init(&o);
    unsetenv("RECOMP_N64_CROP_EDGES");
    CHECK(n64_runtime_crop_edges(&o) == 1 && n64_runtime_crop_edges(NULL) == 1);
    setenv("RECOMP_N64_CROP_EDGES", "0", 1);
    CHECK(n64_runtime_crop_edges(&o) == 0);
    setenv("RECOMP_N64_CROP_EDGES", "1", 1);
    CHECK(n64_runtime_crop_edges(&o) == 1);
    o.crop_edges = 0;
    CHECK(n64_runtime_crop_edges(&o) == 0);
    unsetenv("RECOMP_N64_CROP_EDGES");
    CHECK(n64_runtime_crop_columns(320) == 4 && n64_runtime_crop_columns(640) == 8 &&
          n64_runtime_crop_columns(16) == 0 && n64_runtime_crop_columns(0) == 0);
}

static void test_helpers(void)
{
    char path[256], key[32];
    int8_t x, y;

    /* Xbox layout onto the N64 controller. */
    CHECK(n64_runtime_pad_buttons(HOST_PAD_A, 0, 0, 0, 0) == N64_BTN_A);
    CHECK(n64_runtime_pad_buttons(HOST_PAD_B, 0, 0, 0, 0) == N64_BTN_B);
    CHECK(n64_runtime_pad_buttons(HOST_PAD_X, 0, 0, 0, 0) == N64_BTN_B);
    CHECK(n64_runtime_pad_buttons(HOST_PAD_Y, 0, 0, 0, 0) == N64_BTN_CUP);
    CHECK(n64_runtime_pad_buttons(HOST_PAD_LSHOULDER | HOST_PAD_RSHOULDER, 0, 0, 0, 0) == (N64_BTN_L | N64_BTN_R));
    CHECK(n64_runtime_pad_buttons(0, 0, 0, 30000, 0) == N64_BTN_Z);
    CHECK(n64_runtime_pad_buttons(0, 0, 0, 0, 30000) == N64_BTN_Z);
    CHECK(n64_runtime_pad_buttons(0, 0, 0, 4000, 4000) == 0);
    CHECK(n64_runtime_pad_buttons(HOST_PAD_START | HOST_PAD_BACK, 0, 0, 0, 0) == N64_BTN_START);
    CHECK(n64_runtime_pad_buttons(HOST_PAD_DPAD_UP | HOST_PAD_DPAD_LEFT, 0, 0, 0, 0) == (N64_BTN_DUP | N64_BTN_DLEFT));
    CHECK(n64_runtime_pad_buttons(HOST_PAD_DPAD_DOWN | HOST_PAD_DPAD_RIGHT, 0, 0, 0, 0)
          == (N64_BTN_DDOWN | N64_BTN_DRIGHT));
    CHECK(n64_runtime_pad_buttons(HOST_PAD_DPAD_UP | HOST_PAD_DPAD_DOWN, 0, 0, 0, 0) == 0); /* opposites cancel */
    /* The right stick: the C buttons past half its travel (+y down on SDL = C-down). */
    CHECK(n64_runtime_pad_buttons(0, 0, -30000, 0, 0) == N64_BTN_CUP);
    CHECK(n64_runtime_pad_buttons(0, 0, 30000, 0, 0) == N64_BTN_CDOWN);
    CHECK(n64_runtime_pad_buttons(0, -30000, 0, 0, 0) == N64_BTN_CLEFT);
    CHECK(n64_runtime_pad_buttons(0, 30000, 0, 0, 0) == N64_BTN_CRIGHT);
    CHECK(n64_runtime_pad_buttons(0, 10000, -10000, 0, 0) == 0);
    CHECK(n64_runtime_pad_buttons(HOST_PAD_Y, 0, 30000, 0, 0) == 0); /* C-up and C-down at once */

    /* The left stick: dead zone, range, +y up, the octagon's diagonals. */
    n64_runtime_stick(0, 0, 0, -1, &x, &y);
    CHECK(x == 0 && y == 0);
    n64_runtime_stick(2000, -2000, 0, -1, &x, &y); /* inside 8 % */
    CHECK(x == 0 && y == 0);
    n64_runtime_stick(32767, 0, 0, -1, &x, &y);
    CHECK(x == 80 && y == 0);
    n64_runtime_stick(-32768, 0, 0, -1, &x, &y);
    CHECK(x == -80 && y == 0);
    n64_runtime_stick(0, -32768, 0, -1, &x, &y);
    CHECK(x == 0 && y == 80);
    n64_runtime_stick(0, 32767, 0, -1, &x, &y);
    CHECK(x == 0 && y == -80);
    n64_runtime_stick(23170, -23170, 0, -1, &x, &y); /* full diagonal, up-right */
    CHECK(x >= 64 && x <= 72 && y >= 64 && y <= 72);
    n64_runtime_stick(32767, 32767, 0, -1, &x, &y); /* a square-gated pad's corner: clamped to the range */
    CHECK(x <= 80 && x >= 64 && y >= -80 && y <= -64);
    n64_runtime_stick(16384, 0, 0, 0, &x, &y); /* half travel, no dead zone: half the range */
    CHECK(x == 40 && y == 0);
    n64_runtime_stick(32767, 0, 100, 0, &x, &y);
    CHECK(x == 100);
    n64_runtime_stick(32767, 0, 200, 0, &x, &y);
    CHECK(x == 127);

    /* XRGB to RGBA. */
    uint32_t px[2] = {0x00112233u, 0x00FF8000u};
    uint8_t rgba[8];
    n64_runtime_xrgb_to_rgba(px, rgba, 2);
    CHECK(rgba[0] == 0x11 && rgba[1] == 0x22 && rgba[2] == 0x33 && rgba[3] == 255);
    CHECK(rgba[4] == 0xFF && rgba[5] == 0x80 && rgba[6] == 0x00 && rgba[7] == 255);

    /* Save paths. */
    CHECK(n64_runtime_save_path("/s", "NSME-635A2BFF", N64_SAVE_EEPROM4K, path, sizeof path) == 0
          && !strcmp(path, "/s/n64-NSME-635A2BFF/game.eep"));
    CHECK(n64_runtime_save_path("/s", "K", N64_SAVE_EEPROM16K, path, sizeof path) == 0
          && !strcmp(path, "/s/n64-K/game.eep"));
    CHECK(n64_runtime_save_path("/s", "K", N64_SAVE_SRAM, path, sizeof path) == 0 && !strcmp(path, "/s/n64-K/game.sra"));
    CHECK(n64_runtime_save_path("/s", "K", N64_SAVE_FLASH, path, sizeof path) == 0 && !strcmp(path, "/s/n64-K/game.fla"));
    CHECK(n64_runtime_save_path("/s", "K", N64_SAVE_PAK0, path, sizeof path) == 0 && !strcmp(path, "/s/n64-K/pak1.mpk"));
    CHECK(n64_runtime_save_path("/s", "K", N64_SAVE_PAK3, path, sizeof path) == 0 && !strcmp(path, "/s/n64-K/pak4.mpk"));
    CHECK(n64_runtime_save_path("/s", "K", N64_SAVE_NONE, path, sizeof path) == -1);
    CHECK(n64_runtime_save_path(NULL, "K", N64_SAVE_SRAM, path, sizeof path) == -1);
    CHECK(n64_runtime_save_path("/s", "", N64_SAVE_SRAM, path, sizeof path) == -1);
    CHECK(n64_runtime_save_path("/s", "K", N64_SAVE_SRAM, path, 8) == -1);

    /* The fallback key: recomp_pack_rom_identify's formula, from each byte order. */
    uint8_t h[0x40] = {0x80, 0x37, 0x12, 0x40}, v[0x40];
    h[0x10] = 0x63; h[0x11] = 0x5A; h[0x12] = 0x2B; h[0x13] = 0xFF;
    memcpy(h + 0x3B, "NSME", 4);
    for (int order = 0; order < 3; order++) {
        for (size_t i = 0; i < sizeof h; i++)
            v[i] = order == 0 ? h[i] : order == 1 ? h[i ^ 1] : h[(i & ~(size_t)3) + 3 - (i & 3)];
        CHECK(n64_runtime_fallback_key(v, sizeof v, key, sizeof key) == 0 && !strcmp(key, "NSME-635A2BFF"));
    }
    h[0x3C] = 0;
    CHECK(n64_runtime_fallback_key(h, sizeof h, key, sizeof key) == 0 && !strcmp(key, "N_ME-635A2BFF"));
    CHECK(n64_runtime_fallback_key(h, 0x20, key, sizeof key) == -1);
    h[0] = 0x12;
    CHECK(n64_runtime_fallback_key(h, sizeof h, key, sizeof key) == -1);

    /* Frame length from the cycles run: NTSC / PAL, clamped. */
    CHECK(n64_runtime_frame_ns(93750000ull / 60) / 1000 == 16666);
    CHECK(n64_runtime_frame_ns(93750000ull / 50) / 1000 == 20000);
    CHECK(n64_runtime_frame_ns(0) == 1000000000ull / 75);
    CHECK(n64_runtime_frame_ns(93750000ull) == 1000000000ull / 45);

    /* Pak names. */
    CHECK(n64_runtime_pak_parse("controller") == N64_RT_PAK_CONTROLLER);
    CHECK(n64_runtime_pak_parse("Rumble") == N64_RT_PAK_RUMBLE);
    CHECK(n64_runtime_pak_parse("none") == N64_RT_PAK_NONE);
    CHECK(n64_runtime_pak_parse("transfer") == N64_RT_PAK_DEFAULT);
    CHECK(n64_runtime_pak_parse(NULL) == N64_RT_PAK_DEFAULT);

    /* Guest stuck detector: a healthy game (picture changes now and then, DMAs and audio every frame) never; no
     * progress at all for the limit: once; a fault then no new picture while the controller is still polled: once. */
    {
        n64_runtime_stuck st;
        n64_runtime_stuck_init(&st);
        uint64_t vi = 0, other = 0, faults = 0;
        int hits = 0;
        for (int f = 0; f < 2000; f++) {
            if (f % 40 == 0)
                vi++; /* a static screen for 40 frames at a time */
            other += 3;
            hits += n64_runtime_stuck_check(&st, vi, other, faults, 250) != N64_RT_STUCK_NONE;
        }
        CHECK(hits == 0);
        int got = 0;
        for (int f = 0; f < 600; f++) { /* an exception storm: nothing moves but the fault count */
            int w = n64_runtime_stuck_check(&st, vi, other, faults += 1000, 250);
            if (w != N64_RT_STUCK_NONE) {
                CHECK(f < 250); /* the picture had been still for a while already: the fault reason comes first */
                got++;
            }
        }
        CHECK(got == 1);
        vi++;
        got = 0;
        for (int f = 0; f < 600; f++) { /* a deadlock: nothing moves at all */
            int w = n64_runtime_stuck_check(&st, vi, other, faults, 250);
            if (w != N64_RT_STUCK_NONE) {
                CHECK(w == N64_RT_STUCK_IDLE && f == 250);
                got++;
            }
        }
        CHECK(got == 1);
        vi++; /* it came back */
        CHECK(n64_runtime_stuck_check(&st, vi, other, faults, 250) == N64_RT_STUCK_NONE);
        got = 0;
        faults++; /* the game's crash screen: one fault, the picture stays, SI / audio go on */
        for (int f = 0; f < 600; f++) {
            int w = n64_runtime_stuck_check(&st, vi, other += 19, faults, 250);
            if (w != N64_RT_STUCK_NONE) {
                CHECK(w == N64_RT_STUCK_FAULT);
                got++;
            }
        }
        CHECK(got == 1);
        vi++;
        n64_runtime_stuck_check(&st, vi, other, faults, 250);
        faults++; /* a fault, then the crash screen drawn a few frames later, then that picture stays */
        got = 0;
        for (int f = 0; f < 600; f++) {
            if (f == 5)
                vi++;
            got += n64_runtime_stuck_check(&st, vi, other += 19, faults, 250) == N64_RT_STUCK_FAULT;
        }
        CHECK(got == 1);
        for (int f = 0; f < 600; f++) /* an old fault and a later static screen: not a crash */
            got += n64_runtime_stuck_check(&st, vi += (f == 300), other += 19, faults, 250) != N64_RT_STUCK_NONE;
        CHECK(got == 1);
        CHECK(n64_runtime_stuck_check(&st, vi, other, faults, 0) == N64_RT_STUCK_NONE);
    }

    /* Frame skip: parse, auto (late by more than half a frame or audio low, 3 in a row at most), fixed, off. */
    CHECK(n64_runtime_frameskip_parse("auto") == N64_RT_FRAMESKIP_AUTO);
    CHECK(n64_runtime_frameskip_parse(NULL) == N64_RT_FRAMESKIP_AUTO);
    CHECK(n64_runtime_frameskip_parse("0") == 0 && n64_runtime_frameskip_parse("2") == 2);
    {
        const uint64_t f = 16666666;
        int run = 0, n = 0;
        CHECK(n64_runtime_frameskip(N64_RT_FRAMESKIP_AUTO, 0, f, 0, &run) == 0);
        CHECK(n64_runtime_frameskip(N64_RT_FRAMESKIP_AUTO, (int64_t)f / 4, f, 0, &run) == 0);
        for (int i = 0; i < 8; i++)
            n += n64_runtime_frameskip(N64_RT_FRAMESKIP_AUTO, (int64_t)f, f, 0, &run);
        CHECK(n == 6); /* skip 3, show 1, skip 3, show 1 */
        run = 0;
        CHECK(n64_runtime_frameskip(N64_RT_FRAMESKIP_AUTO, -5000000, f, 1, &run) == 1); /* audio about to run dry */
        CHECK(n64_runtime_frameskip(N64_RT_FRAMESKIP_AUTO, 0, f, 0, &run) == 0 && run == 0);
        n = 0;
        for (int i = 0; i < 9; i++)
            n += n64_runtime_frameskip(2, 0, f, 0, &run);
        CHECK(n == 6); /* fixed 2: two of every three */
        run = 0;
        CHECK(n64_runtime_frameskip(0, (int64_t)f * 10, f, 1, &run) == 0);
        /* Over new pictures only: auto never skips two in a row; fixed N and 0 as above. */
        run = 0;
        n = 0;
        for (int i = 0; i < 8; i++)
            n += n64_runtime_frameskip_picture(N64_RT_FRAMESKIP_AUTO, (int64_t)f, f, 0, &run);
        CHECK(n == 4); /* skip 1, show 1 */
        run = 0;
        CHECK(n64_runtime_frameskip_picture(N64_RT_FRAMESKIP_AUTO, 0, f, 0, &run) == 0 && run == 0);
        CHECK(n64_runtime_frameskip_picture(N64_RT_FRAMESKIP_AUTO, 0, f, 1, &run) == 1 && run == 1);
        CHECK(n64_runtime_frameskip_picture(N64_RT_FRAMESKIP_AUTO, (int64_t)f, f, 1, &run) == 0 && run == 0);
        n = 0;
        for (int i = 0; i < 9; i++)
            n += n64_runtime_frameskip_picture(2, 0, f, 0, &run);
        CHECK(n == 6);
        CHECK(n64_runtime_frameskip_picture(0, (int64_t)f * 10, f, 1, &run) == 0);
    }
}

static long file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static int ppm_size(const char *path, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    int ok = fscanf(f, "P6 %d %d 255", w, h) == 2;
    fclose(f);
    return ok ? 0 : -1;
}

/* The picture of gfx.z64 (consoles/n64/tests/homebrew/gfx): a red fill rectangle top left, black elsewhere there. */
static int ppm_pixel(const char *path, int x, int y, uint8_t rgb[3])
{
    FILE *f = fopen(path, "rb");
    int w, h;
    if (!f)
        return -1;
    int ok = fscanf(f, "P6 %d %d 255", &w, &h) == 2 && fgetc(f) != EOF && x < w && y < h;
    ok = ok && fseek(f, (long)(y * w + x) * 3, SEEK_CUR) == 0 && fread(rgb, 1, 3, f) == 3;
    fclose(f);
    return ok ? 0 : -1;
}

static void *run_thread(void *arg)
{
    static int rc;
    rc = n64_runtime_run((const n64_runtime_opts *)arg);
    return &rc;
}

static void wait_attached(void)
{
    for (int i = 0; i < 500 && !recomp_states_available(); i++)
        usleep(10 * 1000);
}

static void quit_run(pthread_t th)
{
    SDL_Event quit;
    void *ret = NULL;
    memset(&quit, 0, sizeof quit);
    quit.type = SDL_QUIT;
    CHECK(SDL_PushEvent(&quit) == 1);
    CHECK(pthread_join(th, &ret) == 0 && ret && *(int *)ret == 0);
    CHECK(!recomp_states_available());
}

/* Save states (XBA-183): gfx.z64 plays paced on its own thread; slot 1 saved (with a 4:3 thumbnail of its picture)
 * and loaded as the in-game menu does, slot 2 while the game is held, the quick slot; fast-forward toggled; the
 * automatic slot saved when the window closes and resumed at the next start (RECOMP_STATE_RESUME=1); the slot copied
 * into another game's folder (hello.z64) refused there. */
/* The performance line's text (n64_runtime_perf_format): the fields in their order, the pack and the core variants. */
static void test_perf_format(void)
{
    n64_runtime_perf p;
    char line[1024];
    memset(&p, 0, sizeof p);
    p.seconds = 10.0;
    p.frames = 150;
    p.host_frames = 150;
    p.vi_hz = 60.0;
    p.cpu_ms = 7.9;
    p.rsp_ms = 4.1;
    p.rdp_ms = 5.2;
    p.rdp_wait_ms = 3.0;
    p.sched_ms = 0.3;
    p.vi_ms = 1.8;
    p.present_ms = 0.9;
    p.worst_ms = 41.2;
    p.over_ms = 16.7;
    p.over_1 = 120;
    p.over_2 = 30;
    p.rsp_instr = 1.2e6;
    p.rdp_cmds = 3400;
    p.rdp_syncs = 2;
    p.pack = 1;
    p.native_pct = 83.4;
    p.interp_instr = 3100;
    p.rehashes = 12;
    p.slow_lookups = 40;
    p.rdp = "vulkan";
    p.upscale = 2;
    p.cpu = 6;
    p.cpu_khz = 1900800;
    n64_runtime_perf_format(&p, line, sizeof line);
    p.rdp_queue_ms = 1.0;
    p.rdp_sync_ms = 2.5;
    p.rdp_scanout_ms = 0.5;
    n64_runtime_perf_format(&p, line, sizeof line);
    CHECK(strcmp(line, "speed 25 % (15.0 of 60.0 VI/s); cpu 7.9 rsp 4.1 rdp 5.2 (queue 1.0, sync 2.5, scanout 0.5) "
                       "sched 0.3 vi 1.8 present "
                       "0.9 audio 0.0 idle 0.0 other 0.0 ms/frame; worst 41 ms, 120 > 16.7 ms, 30 > 33.4 ms of 150 "
                       "frames; rsp 1.20 M instr, rdp 3400 cmds, 2.0 full syncs /frame; pack 83 % native, 3.1 k "
                       "interp instr/frame, 12 rehashes, 40 slow lookups; rdp vulkan, upscale 2x, rewind off; core 6 @ "
                       "1.90 GHz") == 0);
    if (failures)
        printf("perf line: %s\n", line);
    p.pack = 0;
    p.rdp = "software";
    p.rdp_threads = 2;
    p.upscale = 1;
    p.rewind = 1;
    p.cpu_khz = 0;
    n64_runtime_perf_format(&p, line, sizeof line);
    CHECK(strstr(line, "; no pack; rdp software, 2 threads, upscale 1x, rewind on; core 6") != NULL);
    CHECK(strstr(line, "GHz") == NULL);
    p.cpu = -1;
    n64_runtime_perf_format(&p, line, sizeof line);
    CHECK(strstr(line, "; core ?") != NULL);

    /* The detail line: the pictures against the VIs, the worst frame's own breakdown. */
    memset(&p, 0, sizeof p);
    p.seconds = 10.0;
    p.frames = 500;
    p.pictures = 167;
    p.unchanged = 333;
    p.vi_origin_changes = 167;
    p.scanouts = 167;
    p.queue_waits = 40;
    p.stalled_compiles = 2;
    p.stalled_compile_ms = 70.4;
    p.worst_ms = 40.2;
    p.worst_core_ms = 30.1;
    p.worst_vi_ms = 7.2;
    p.worst_present_ms = 1.8;
    p.worst_audio_ms = 0.1;
    p.worst_other_ms = 1.0;
    p.worst_queue_ms = 9.0;
    p.worst_sync_ms = 8.0;
    p.worst_scanout_ms = 3.0;
    p.worst_deadline_ms = 0.2;
    p.scanouts_early = 160;
    p.dp_deferred = 160;
    p.dp_now = 7;
    p.dp_early = 1;
    p.dp_waits = 12;
    p.dp_wait_ms = 3.5;
    n64_runtime_perf_detail_format(&p, line, sizeof line);
    CHECK(strcmp(line, "500 VIs, 167 pictures (16.7/s), 333 unchanged, 0 skipped; 167 VI_ORIGIN changes, 167 scanouts "
                       "(160 early), 40 full rings; DP 160 deferred (1 early, 0 backoffs), 7 at once, 12 waited 3.5 "
                       "ms (0.02 ms each); 2 stalled compiles (70 ms); worst 40 ms: core 30.1 vi 7.2 present 1.8 "
                       "audio 0.1 other 1.0 (rdp queue 9.0, sync 8.0, deadline 0.2, scanout 3.0)") == 0);
    if (failures)
        printf("detail line: %s\n", line);
}

static void test_states(const char *dir, const char *hb)
{
    char rom[600], rom2[600], slot[800], other[800], cmd[1700];
    snprintf(rom, sizeof rom, "%s/gfx.z64", hb);
    snprintf(rom2, sizeof rom2, "%s/hello.z64", hb);
    recomp_states_detach();
    CHECK(recomp_states_save(1) == RECOMP_STATE_ENOGAME);
    n64_runtime_opts o;
    n64_runtime_opts_init(&o);
    o.rom = rom;
    o.save_dir = dir;
    o.title_key = "STAT-00000001";
    o.headless = 1;
    pthread_t th;
    CHECK(pthread_create(&th, NULL, run_thread, &o) == 0);
    wait_attached();
    CHECK(recomp_states_available() && !strcmp(recomp_states_console(), "n64"));
    usleep(600 * 1000);
    recomp_state_meta meta;
    static uint16_t thumb[RECOMP_STATE_THUMB_W * RECOMP_STATE_THUMB_H];
    CHECK(recomp_states_info(1, &meta, NULL) == RECOMP_STATE_EEMPTY);
    CHECK(recomp_states_save(1) == RECOMP_STATE_OK);
    CHECK(recomp_states_info(1, &meta, thumb) == RECOMP_STATE_OK);
    CHECK(!strcmp(meta.console, "n64") && !strcmp(meta.key, "STAT-00000001") && meta.frames > 0);
    CHECK(meta.thumb_w == 160 && meta.thumb_h == 120);
    int lit = 0;
    for (int i = 0; i < meta.thumb_w * meta.thumb_h; i++)
        lit += thumb[i] != 0;
    CHECK(lit > 100); /* gfx.z64's picture, not black */
    snprintf(slot, sizeof slot, "%s/n64-STAT-00000001/state1.st", dir);
    CHECK(file_size(slot) > 100000);
    recomp_ff_set(1);
    usleep(150 * 1000);
    recomp_ff_set(0);
    CHECK(recomp_states_load(1) == RECOMP_STATE_OK);
    recomp_lifecycle_pause_for(RECOMP_HOLD_MENU);
    CHECK(recomp_states_save(2) == RECOMP_STATE_OK && recomp_states_load(2) == RECOMP_STATE_OK);
    recomp_lifecycle_resume_for(RECOMP_HOLD_MENU);
    CHECK(recomp_states_quick_slot() == 2 && recomp_states_quick_load() == RECOMP_STATE_OK);
    usleep(50 * 1000);
    quit_run(th);

    /* The automatic slot: saved when the window closes, loaded before the first frame of the next run. */
    char autost[800];
    snprintf(autost, sizeof autost, "%s/n64-STAT-00000001/auto.st", dir);
    setenv("RECOMP_STATE_RESUME", "1", 1);
    CHECK(pthread_create(&th, NULL, run_thread, &o) == 0);
    wait_attached();
    usleep(300 * 1000);
    quit_run(th);
    CHECK(file_size(autost) > 100000);
    recomp_state_event ev0, ev;
    recomp_states_last_event(&ev0);
    CHECK(pthread_create(&th, NULL, run_thread, &o) == 0);
    wait_attached();
    usleep(100 * 1000);
    recomp_states_last_event(&ev);
    CHECK(ev.seq != ev0.seq && ev.kind == RECOMP_STATE_EV_RESUMED && ev.rc == RECOMP_STATE_OK);
    quit_run(th);
    unsetenv("RECOMP_STATE_RESUME");

    if (access(rom2, R_OK) != 0)
        return;
    snprintf(other, sizeof other, "%s/n64-OTHR-00000002", dir);
    mkdir(other, 0755);
    snprintf(other, sizeof other, "%s/n64-OTHR-00000002/state3.st", dir);
    snprintf(cmd, sizeof cmd, "cp '%s' '%s'", slot, other);
    CHECK(system(cmd) == 0);
    n64_runtime_opts_init(&o);
    o.rom = rom2;
    o.save_dir = dir;
    o.title_key = "OTHR-00000002";
    o.headless = 1;
    CHECK(pthread_create(&th, NULL, run_thread, &o) == 0);
    wait_attached();
    CHECK(recomp_states_info(3, &meta, NULL) == RECOMP_STATE_OK);
    CHECK(recomp_states_load(3) == RECOMP_STATE_EGAME);
    quit_run(th);
}

static void test_runs(const char *hb)
{
    char dir[] = "/tmp/n64_runtime_test.XXXXXX", rom[600], v64[400], shot[300], eep[400], pak[400], spec[32],
         cmd[3000];
    uint8_t rgb[3];
    int w = 0, h = 0;

    snprintf(rom, sizeof rom, "%s/gfx.z64", hb);
    if (access(rom, R_OK) != 0) {
        printf("runs skipped: no %s (tools/consoles/build_homebrew.sh n64)\n", rom);
        return;
    }
    if (!mkdtemp(dir)) {
        CHECK(0);
        return;
    }
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    CHECK(SDL_Init(SDL_INIT_EVENTS) == 0);

    /* 1: gfx.z64: its 320-wide picture, the red rectangle at (40, 30); no save file made for a save never written. */
    n64_runtime_opts o;
    n64_runtime_opts_init(&o);
    o.rom = rom;
    o.save_dir = dir;
    o.title_key = "TEST-00000001";
    o.frames = 120;
    o.unpaced = 1;
    snprintf(shot, sizeof shot, "%s/last.ppm", dir);
    o.screenshot = shot;
    CHECK(n64_runtime_run(&o) == 0);
    CHECK(ppm_size(shot, &w, &h) == 0 && w == 320 && h >= 224 && h <= 240);
    CHECK(ppm_pixel(shot, 40, 30, rgb) == 0 && rgb[0] > 200 && rgb[1] < 40 && rgb[2] < 40);
    /* The whole run's performance figures: every frame counted, the core timed, the stages add up to the wall time
     * (other: what no stage covers, a small part). */
    n64_runtime_perf pf;
    n64_runtime_last_perf(&pf);
    double wall_ms = pf.frames ? pf.seconds * 1e3 / (double)pf.frames : 0.0;
    double staged = pf.cpu_ms + pf.rsp_ms + pf.rdp_ms + pf.sched_ms + pf.vi_ms + pf.present_ms + pf.audio_ms +
                    pf.idle_ms;
    printf("perf of run 1: %.4f ms/frame, staged %.4f (cpu %.4f rsp %.4f rdp %.4f sched %.4f vi %.4f audio %.4f "
           "other %.4f), %llu host frames, rdp %s\n",
           wall_ms, staged, pf.cpu_ms, pf.rsp_ms, pf.rdp_ms, pf.sched_ms, pf.vi_ms, pf.audio_ms, pf.other_ms,
           (unsigned long long)pf.host_frames, pf.rdp ? pf.rdp : "?");
    CHECK(pf.frames == 120 && pf.host_frames >= 118 && pf.host_frames <= 120);
    CHECK(pf.cpu_ms > 0 && pf.vi_ms > 0 && pf.rdp && pf.vi_hz > 49 && pf.vi_hz < 61);
    CHECK(staged <= wall_ms * 1.001 && staged >= wall_ms * 0.9);
    CHECK(n64_runtime_save_path(dir, "TEST-00000001", N64_SAVE_PAK0, pak, sizeof pak) == 0);
    CHECK(file_size(pak) == -1);
    CHECK(n64_runtime_save_path(dir, "TEST-00000001", N64_SAVE_EEPROM4K, eep, sizeof eep) == 0);
    CHECK(file_size(eep) == -1);

    /* 2: saves on disk (an EEPROM and a controller pak) are loaded and, unchanged, not written again (same bytes, no
     * .tmp left); two RDP threads, a rumble pak, two players. Through "fd:N" with the descriptor's offset elsewhere:
     * read from 0, left open. */
    snprintf(cmd, sizeof cmd, "mkdir -p '%s/n64-TEST-00000001' && head -c 512 /dev/urandom > '%s' && "
                              "head -c 32768 /dev/urandom > '%s' && cp '%s' '%s.ref' && cp '%s' '%s.ref'",
             dir, eep, pak, eep, eep, pak, pak);
    CHECK(system(cmd) == 0);
    int fd = open(rom, O_RDONLY);
    CHECK(fd >= 0 && lseek(fd, 77, SEEK_SET) == 77);
    snprintf(spec, sizeof spec, "fd:%d", fd);
    o.rom = spec;
    o.rdp_threads = 2;
    o.pak = N64_RT_PAK_RUMBLE;
    o.players = 2;
    o.frames = 60;
    CHECK(n64_runtime_run(&o) == 0);
    CHECK(fcntl(fd, F_GETFD) != -1);
    close(fd);
    CHECK(ppm_pixel(shot, 40, 30, rgb) == 0 && rgb[0] > 200 && rgb[1] < 40 && rgb[2] < 40);
    snprintf(cmd, sizeof cmd, "cmp -s '%s' '%s.ref' && cmp -s '%s' '%s.ref'", eep, eep, pak, pak);
    CHECK(system(cmd) == 0);
    char tmp[420];
    snprintf(tmp, sizeof tmp, "%s.tmp", eep);
    CHECK(file_size(tmp) == -1);

    /* 3: the same ROM byte-swapped (.v64): the same picture. */
    snprintf(v64, sizeof v64, "%s/gfx.v64", dir);
    snprintf(cmd, sizeof cmd, "dd if='%s' of='%s' conv=swab status=none", rom, v64);
    CHECK(system(cmd) == 0);
    n64_runtime_opts_init(&o);
    o.rom = v64;
    o.frames = 120;
    o.unpaced = 1;
    o.screenshot = shot;
    CHECK(n64_runtime_run(&o) == 0);
    CHECK(ppm_size(shot, &w, &h) == 0 && w == 320);
    CHECK(ppm_pixel(shot, 40, 30, rgb) == 0 && rgb[0] > 200 && rgb[1] < 40 && rgb[2] < 40);

    /* 4: errors: no ROM, a missing file, a bad fd, a file that is no ROM. */
    n64_runtime_opts_init(&o);
    CHECK(n64_runtime_run(&o) == -1);
    o.rom = "/nonexistent/x.z64";
    CHECK(n64_runtime_run(&o) == -1);
    o.rom = "fd:-1";
    CHECK(n64_runtime_run(&o) == -1);
    o.rom = shot;
    CHECK(n64_runtime_run(&o) == -1);

    /* 5: the app shell's entry, bounded by RECOMP_N64_FRAMES, saves in n64_runtime_set_save_dir's dir (those of 2
     * are read there, and left alone); a pack's library is ignored. */
    n64_runtime_set_save_dir(dir);
    setenv("RECOMP_N64_FRAMES", "3", 1);
    CHECK(n64_runtime_start(rom, "libtitle.so", "TEST-00000001") == 0);
    unsetenv("RECOMP_N64_FRAMES");
    n64_runtime_set_save_dir(NULL);
    snprintf(cmd, sizeof cmd, "cmp -s '%s' '%s.ref' && cmp -s '%s' '%s.ref'", eep, eep, pak, pak);
    CHECK(system(cmd) == 0);

    /* 6: save states (XBA-183). */
    test_states(dir, hb);
    SDL_Quit();

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "note: could not remove %s\n", dir);
}

int main(int argc, char **argv)
{
    const char *v = n64_runtime_version();

    if (!v || strncmp(v, "n64_runtime ", 12) != 0) {
        fprintf(stderr, "FAIL: version \"%s\"\n", v ? v : "(null)");
        return 1;
    }
    if (xbox_HostNowNs() == 0) { /* the host layer is linked in */
        fprintf(stderr, "FAIL: no host clock\n");
        return 1;
    }
    test_helpers();
    test_display();
    test_perf_format();
    if (argc >= 2)
        test_runs(argv[1]);
    else
        printf("runs skipped: no homebrew directory argument\n");
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("%s: ok\n", v);
    return 0;
}
