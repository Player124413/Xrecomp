/* The host layer alone (XBA-145): links only xbr_host.
 *
 *   host_layer_test core     time, lifecycle, config, storage, pads and audio
 *                            out, no display (SDL's dummy audio driver)
 *   host_layer_test window   a window, a GL texture presented 120 times (or
 *                            for HOST_LAYER_SECONDS), the pads read each frame;
 *                            exits 77 (skipped) when host_window_headless()
 *                            says so (XBR_BUILD_ONLY, no display)
 *   host_layer_test shaders  the console window's picture shaders (XBA-195), skipped the same way
 *   host_layer_test present_shaders  the shared shader module (XBA-203): keys and sources on every host, each
 *                            preset built in every GL context the display offers
 */
#include "host_audio.h"
#include "host_cpus.h"
#include "host_pad.h"
#include "host_time.h"
#include "host_window.h"
#include "present_shaders.h"
#include "framegen_pacer.h"
#include "recomp_cfg.h"
#include "recomp_lifecycle.h"
#include "recomp_storage.h"

#include <SDL.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int s_fail;

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); s_fail = 1; } } while (0)

static int s_events;
static void on_lifecycle(int paused, void *user)
{
    (void)paused;
    (void)user;
    s_events++;
}

static int run_core(void)
{
    /* Host time: monotonic, and a sleep sleeps. */
    uint64_t t0 = xbox_HostNowNs();
    xbox_HostSleepNs(2000000);
    uint64_t t1 = xbox_HostNowNs();
    CHECK(t1 > t0);
    CHECK(t1 - t0 >= 1000000);

    /* Lifecycle: one listener call per pause and per resume. */
    CHECK(recomp_lifecycle_listen(on_lifecycle, NULL) == 0);
    recomp_lifecycle_pause();
    recomp_lifecycle_pause();
    CHECK(recomp_lifecycle_paused());
    recomp_lifecycle_resume();
    CHECK(!recomp_lifecycle_paused());
    CHECK(s_events == 2);

    /* Config: recomp.cfg into the environment. */
    char dir[] = "/tmp/xbr_host_XXXXXX";
    const char *tmp = getenv("TMPDIR");
    char tmpl[512];
    snprintf(tmpl, sizeof tmpl, "%s/xbr_host_XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
    char *base = mkdtemp(tmpl);
    if (!base)
        base = mkdtemp(dir);
    CHECK(base != NULL);
    if (base) {
        char cfg[600];
        snprintf(cfg, sizeof cfg, "%s/recomp.cfg", base);
        FILE *f = fopen(cfg, "w");
        CHECK(f != NULL);
        if (f) {
            fputs("RECOMP_HOST_LAYER_TEST=42\n", f);
            fclose(f);
            unsetenv("RECOMP_HOST_LAYER_TEST");
            CHECK(recomp_cfg_load(cfg) >= 1);
            const char *v = getenv("RECOMP_HOST_LAYER_TEST");
            CHECK(v && !strcmp(v, "42"));
            unlink(cfg);
        }

        /* Storage, console-agnostic: an entry file of our choosing, then none. */
        recomp_storage st;
        char ext[600], gamefile[1200];
        snprintf(ext, sizeof ext, "%s/ext", base);
        unsetenv("RECOMP_GAME_DIR");
        CHECK(recomp_storage_resolve_entry(ext, base, "Game.GBA", &st) == 0);
        snprintf(gamefile, sizeof gamefile, "%s/game.gba", st.game_dir);
        f = fopen(gamefile, "w");
        CHECK(f != NULL);
        if (f)
            fclose(f);
        CHECK(recomp_storage_resolve_entry(ext, base, "Game.GBA", &st) == 1);
        CHECK(strstr(st.xbe, "game.gba") != NULL);
        CHECK(recomp_storage_resolve_entry(ext, base, NULL, &st) == 0);
        CHECK(st.xbe[0] == '\0');
        unlink(gamefile);
        rmdir(st.game_dir);
        rmdir(ext);
        rmdir(st.save_dir);
        rmdir(base);
    }

    /* The shared options (XBA-181): a console's own key wins, then the shared one, then the default. */
    CHECK(host_window_scale_parse("aspect") == HOST_SCALE_ASPECT);
    CHECK(host_window_scale_parse("Integer") == HOST_SCALE_INTEGER);
    CHECK(host_window_scale_parse("stretch") == HOST_SCALE_STRETCH);
    CHECK(host_window_scale_parse("") == -1 && host_window_scale_parse(NULL) == -1);
    CHECK(host_window_scale_parse("fill") == -1);
    CHECK(host_window_filter_parse("nearest") == HOST_FILTER_NEAREST);
    CHECK(host_window_filter_parse("LINEAR") == HOST_FILTER_LINEAR);
    CHECK(host_window_filter_parse("bilinear") == -1);
    unsetenv("RECOMP_SCALE");
    unsetenv("RECOMP_FILTER");
    unsetenv("RECOMP_TEST_SCALE");
    unsetenv("RECOMP_TEST_FILTER");
    CHECK(host_window_env_scale("RECOMP_TEST_SCALE", HOST_SCALE_INTEGER) == HOST_SCALE_INTEGER);
    CHECK(host_window_env_filter("RECOMP_TEST_FILTER", HOST_FILTER_NEAREST) == HOST_FILTER_NEAREST);
    CHECK(host_window_env_filter("RECOMP_TEST_FILTER", HOST_FILTER_LINEAR) == HOST_FILTER_LINEAR);
    setenv("RECOMP_SCALE", "stretch", 1);
    setenv("RECOMP_FILTER", "linear", 1);
    CHECK(host_window_env_scale("RECOMP_TEST_SCALE", HOST_SCALE_ASPECT) == HOST_SCALE_STRETCH);
    CHECK(host_window_env_scale(NULL, HOST_SCALE_ASPECT) == HOST_SCALE_STRETCH);
    CHECK(host_window_env_filter("RECOMP_TEST_FILTER", HOST_FILTER_NEAREST) == HOST_FILTER_LINEAR);
    setenv("RECOMP_TEST_SCALE", "integer", 1);
    setenv("RECOMP_TEST_FILTER", "nearest", 1);
    CHECK(host_window_env_scale("RECOMP_TEST_SCALE", HOST_SCALE_ASPECT) == HOST_SCALE_INTEGER);
    CHECK(host_window_env_filter("RECOMP_TEST_FILTER", HOST_FILTER_LINEAR) == HOST_FILTER_NEAREST);
    setenv("RECOMP_TEST_SCALE", "junk", 1);   /* not a value: the shared key then */
    CHECK(host_window_env_scale("RECOMP_TEST_SCALE", HOST_SCALE_ASPECT) == HOST_SCALE_STRETCH);
    unsetenv("RECOMP_SCALE");
    unsetenv("RECOMP_FILTER");
    unsetenv("RECOMP_TEST_SCALE");
    unsetenv("RECOMP_TEST_FILTER");
    unsetenv("RECOMP_PRESENT_VSYNC");
    CHECK(host_window_env_vsync() == 1);
    setenv("RECOMP_PRESENT_VSYNC", "0", 1);
    CHECK(host_window_env_vsync() == 0);
    setenv("RECOMP_PRESENT_VSYNC", "1", 1);
    CHECK(host_window_env_vsync() == 1);
    unsetenv("RECOMP_PRESENT_VSYNC");
    unsetenv("RECOMP_RUMBLE");
    CHECK(host_pad_rumble_env() == 1);
    setenv("RECOMP_RUMBLE", "0", 1);
    CHECK(host_pad_rumble_env() == 0);
    setenv("RECOMP_RUMBLE", "1", 1);
    CHECK(host_pad_rumble_env() == 1);
    unsetenv("RECOMP_RUMBLE");
    unsetenv("RECOMP_AUDIO_BUF_SAMPLES");
    CHECK(host_audio_env_buffer_samples() == HOST_AUDIO_BUF_SAMPLES_DEFAULT);
    setenv("RECOMP_AUDIO_BUF_SAMPLES", "256", 1);
    CHECK(host_audio_env_buffer_samples() == 256);
    setenv("RECOMP_AUDIO_BUF_SAMPLES", "16", 1);
    CHECK(host_audio_env_buffer_samples() == 128);
    setenv("RECOMP_AUDIO_BUF_SAMPLES", "99999", 1);
    CHECK(host_audio_env_buffer_samples() == 4096);
    unsetenv("RECOMP_AUDIO_BUF_SAMPLES");

    /* Volume: 100 leaves the samples alone, 0 is silence, 50 is a quarter (the square of the fraction). */
    {
        int16_t in[4] = { 32767, -32768, 1000, -1000 }, out[4];
        CHECK(host_audio_volume() == 100);
        host_audio_scale(out, in, 4, 100);
        CHECK(!memcmp(out, in, sizeof in));
        host_audio_scale(out, in, 4, 0);
        CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0);
        host_audio_scale(out, in, 4, 50);
        CHECK(out[0] == 8191 && out[1] == -8192 && out[2] == 250 && out[3] == -250);
        host_audio_set_volume(150);
        CHECK(host_audio_volume() == 100);
        host_audio_set_volume(-5);
        CHECK(host_audio_volume() == 0);
        host_audio_set_volume(100);
    }

    /* No window: no stats, no screenshot. */
    {
        uint32_t fps = 7;
        uint64_t n = 7;
        CHECK(host_window_stats(&fps, &n) == 0 && fps == 0 && n == 0);
        CHECK(host_window_screenshot_bmp("/nonexistent/x.bmp") == -1);
    }

    /* Fast cores and frame generation link (no pacer thread started). */
    (void)recomp_host_cpus_restore;
    CHECK(!framegen_pacer_active());

    /* Pads without a window: none expected on a CI host, but the calls work. */
    if (host_pad_init()) {
        HostPadState ps;
        host_pad_update();
        unsigned mask = host_pad_mask();
        for (int p = 0; p < HOST_PAD_PORTS; p++)
            CHECK(host_pad_read(p, &ps) == !!(mask & (1u << p)));
        CHECK(host_pad_read(7, &ps) == 0);
        printf("pads: mask 0x%x\n", mask);
        host_pad_shutdown();
    } else {
        printf("pads: no SDL game controller subsystem here\n");
    }

    /* Audio out on whatever SDL_AUDIODRIVER says (the test sets dummy). */
    if (host_audio_open(48000, 2)) {
        int16_t silence[2 * 480] = { 0 };
        CHECK(host_audio_rate() > 0);
        CHECK(host_audio_channels() == 2 || host_audio_channels() == 1);
        CHECK(host_audio_queue(silence, 240));   /* 240 frames fit either channel count */
        recomp_lifecycle_pause();     /* the device pauses with the app */
        recomp_lifecycle_resume();
        host_audio_close();
        CHECK(host_audio_rate() == 0);
    } else {
        printf("audio: no device here\n");
    }
    SDL_Quit();

    printf("%s\n", s_fail ? "host_layer core: FAIL" : "host_layer core: ok");
    return s_fail;
}

/* host_window_present_rgba_shared's caller: its picture on demand (W x H RGBA, tightly packed), counted. */
static const uint8_t *s_shared_px;
static int s_shared_calls;
static int shared_last(void *user, uint8_t **px, int *w, int *h)
{
    (void)user;
    s_shared_calls++;
    if (!s_shared_px)
        return -1;
    *px = malloc(320 * 240 * 4);
    if (!*px)
        return -1;
    memcpy(*px, s_shared_px, 320 * 240 * 4);
    *w = 320;
    *h = 240;
    return 0;
}

static int run_window(void)
{
    enum { W = 320, H = 240 };
    static uint32_t px[W * H];
    double seconds = getenv("HOST_LAYER_SECONDS") ? atof(getenv("HOST_LAYER_SECONDS")) : 0.0;
    uint64_t start, frames = 0;
    unsigned seen_mask = 0;
    uint32_t seen_buttons = 0;
    HostWindowInfo info;

    if (host_window_headless()) {
        printf("host_layer window: headless (XBR_BUILD_ONLY or no display), skipped\n");
        return 77;
    }
    if (!host_window_open("xbr_host test", 640, 480, 1)) {
        fprintf(stderr, "host_layer window: cannot open a window with a GL context\n");
        return 1;
    }
    start = xbox_HostNowNs();
    for (;;) {
        if (!host_window_pump())
            break;
        /* A moving gradient, with a bar per pressed button. */
        uint32_t buttons = 0;
        for (int p = 0; p < HOST_PAD_PORTS; p++) {
            HostPadState ps;
            if (host_pad_read(p, &ps)) {
                seen_mask |= 1u << p;
                buttons |= ps.buttons;
            }
        }
        seen_buttons |= buttons;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                uint8_t r = (uint8_t)(x + frames), g = (uint8_t)(y * 255 / H), b = 0x40;
                if (y < 16 && (buttons & (1u << (x * 15 / W))))
                    r = g = b = 0xFF;
                px[y * W + x] = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | 0xFF000000u;
            }
        CHECK(host_window_present_rgba(px, W, H, W * 4));
        frames++;
        if (seconds > 0.0) {
            if ((double)(xbox_HostNowNs() - start) / 1e9 >= seconds)
                break;
        } else if (frames >= 120) {
            break;
        }
    }
    /* The same picture drawn again (a console frame without a new picture): one more present, no upload. */
    CHECK(host_window_present_again() == 1);
    frames++;
    host_window_info(&info);
    printf("host_layer window: %llu frames, GL%s %d.%d, %dx%d, vsync %d, pads 0x%x, buttons seen 0x%x\n",
           (unsigned long long)info.frames, info.gles ? " ES" : "", info.gl_major, info.gl_minor, info.drawable_w,
           info.drawable_h, info.vsync, seen_mask, seen_buttons);
    CHECK(info.frames == frames);
    CHECK(host_window_gl_proc("glGetString") != NULL);

    /* Stats and the screenshot of the last picture (XBA-181), from this thread as from any. */
    uint32_t fps = 0;
    uint64_t presented = 0;
    CHECK(host_window_stats(&fps, &presented) == 1);
    CHECK(presented == frames);
    printf("host_layer window: stats %u.%02u fps\n", fps / 100, fps % 100);
    {
        char bmp[512];
        const char *tmp = getenv("TMPDIR");
        snprintf(bmp, sizeof bmp, "%s/xbr_host_shot_%d.bmp", tmp && tmp[0] ? tmp : "/tmp", (int)getpid());
        host_window_set_aspect(4, 3);   /* 320x240 is 4:3 already: same size */
        CHECK(host_window_present_rgba(px, W, H, W * 4));
        CHECK(host_window_screenshot_bmp(bmp) == 0);
        FILE *f = fopen(bmp, "rb");
        unsigned char hdr[54] = { 0 };
        CHECK(f && fread(hdr, 1, sizeof hdr, f) == sizeof hdr);
        if (f)
            fclose(f);
        int32_t bw, bh;
        memcpy(&bw, hdr + 18, 4);
        memcpy(&bh, hdr + 22, 4);
        CHECK(hdr[0] == 'B' && hdr[1] == 'M' && bw == W && bh == H && hdr[28] == 24);
        unlink(bmp);
        host_window_set_aspect(0, 0);

        /* A picture the caller keeps (host_window_present_rgba_shared): the screenshot asks for it, no copy made at
         * present; host_window_keep_shared takes the copy when the caller's pixels go. */
        s_shared_calls = 0;
        s_shared_px = (const uint8_t *)px;
        CHECK(host_window_present_rgba_shared(px, W, H, W * 4, shared_last, NULL));
        CHECK(s_shared_calls == 0);
        CHECK(host_window_screenshot_bmp(bmp) == 0);
        CHECK(s_shared_calls == 1);
        unlink(bmp);
        host_window_keep_shared();
        CHECK(s_shared_calls == 2);
        s_shared_px = NULL; /* gone: the window's copy answers */
        CHECK(host_window_screenshot_bmp(bmp) == 0);
        CHECK(s_shared_calls == 2);
        unlink(bmp);
        CHECK(host_window_present_rgba_shared(px, W, H, W * 4, shared_last, NULL));
        CHECK(host_window_screenshot_bmp(bmp) == -1); /* the caller has none any more */
        CHECK(host_window_present_rgba(px, W, H, W * 4)); /* a kept copy again */
        CHECK(host_window_screenshot_bmp(bmp) == 0);
        CHECK(s_shared_calls == 3);
        unlink(bmp);
    }
    host_window_close();
    CHECK(host_window_stats(&fps, &presented) == 0);
    CHECK(host_window_screenshot_bmp("/tmp/never.bmp") == -1);   /* the picture went with the window */
    SDL_Quit();
    return s_fail;
}

/* ── Picture shaders (XBA-195) ──────────────────────────────────────────
 * Each preset is drawn through the real present path and read back with host_window_capture (the drawable as drawn).
 * A 160x120 picture in a 640x480 window at whole-pixel scale: every console pixel is 4x4 output pixels, the
 * picture fills the window. */
enum { SW = 160, SH = 120, WW = 640, WH = 480 };

typedef struct {
    int w, h;
    unsigned char *rgb;
} Ppm;

static int ppm_read(const char *path, Ppm *out)
{
    FILE *f = fopen(path, "rb");
    int w = 0, h = 0, max = 0;

    out->rgb = NULL;
    if (!f)
        return 0;
    if (fscanf(f, "P6 %d %d %d", &w, &h, &max) != 3 || w <= 0 || h <= 0 || max != 255 || fgetc(f) == EOF) {
        fclose(f);
        return 0;
    }
    out->rgb = malloc((size_t)w * h * 3);
    if (!out->rgb || fread(out->rgb, 3, (size_t)w * h, f) != (size_t)w * h) {
        free(out->rgb);
        out->rgb = NULL;
        fclose(f);
        return 0;
    }
    fclose(f);
    unlink(path);
    out->w = w;
    out->h = h;
    return 1;
}

static int ppm_at(const Ppm *p, int x, int y, int c)
{
    return p->rgb[((size_t)y * p->w + x) * 3 + c];
}

/* The mean of channel c over row y (or column x when y < 0). */
static double ppm_mean_row(const Ppm *p, int y, int c)
{
    double s = 0;
    for (int x = 0; x < p->w; x++)
        s += ppm_at(p, x, y, c);
    return s / p->w;
}

static double ppm_mean_col(const Ppm *p, int x, int c)
{
    double s = 0;
    for (int y = 0; y < p->h; y++)
        s += ppm_at(p, x, y, c);
    return s / p->h;
}

/* Presents px with preset (strength -1: its default) and captures the window. */
static int shot(const uint32_t *px, int preset, int strength, Ppm *out)
{
    char path[512];
    const char *tmp = getenv("TMPDIR");
    HostShader s = { preset, strength, 0, 0 };

    snprintf(path, sizeof path, "%s/xbr_host_shader_%d.ppm", tmp && tmp[0] ? tmp : "/tmp", (int)getpid());
    host_window_set_shader(&s);
    host_window_pump();
    host_window_capture(path);
    if (!host_window_present_rgba(px, SW, SH, SW * 4))
        return 0;
    return ppm_read(path, out) && out->w == WW && out->h == WH;
}

/* Milliseconds per present of preset at the window's size, vsync off (llvmpipe here: relative cost only). */
static double bench(const uint32_t *px, int preset, int n)
{
    HostShader s = { preset, -1, preset == HOST_SHADER_CRT ? 30 : 0, preset == HOST_SHADER_LCD ? 50 : 0 };

    host_window_set_shader(&s);
    host_window_present_rgba(px, SW, SH, SW * 4);   /* builds the program */
    uint64_t t0 = xbox_HostNowNs();
    for (int i = 0; i < n; i++)
        host_window_present_rgba(px, SW, SH, SW * 4);
    return (double)(xbox_HostNowNs() - t0) / 1e6 / n;
}

/* run_shaders' live change, from a thread of its own as the Android UI thread's would be. */
static void *live_request(void *arg)
{
    HostShader s = { HOST_SHADER_SCANLINES, 80, 0, 0 };
    (void)arg;
    host_window_request_picture(HOST_SCALE_STRETCH, HOST_FILTER_NEAREST, &s);
    return NULL;
}

static int run_shaders(void)
{
    static uint32_t flat[SW * SH];
    Ppm none = { 0 }, p = { 0 };
    char bmp[512];
    const char *tmp = getenv("TMPDIR");

    /* Parsing and the environment, before any window. */
    CHECK(host_window_shader_parse("none") == HOST_SHADER_NONE);
    CHECK(host_window_shader_parse("Sharp") == HOST_SHADER_SHARP);
    CHECK(host_window_shader_parse("SCANLINES") == HOST_SHADER_SCANLINES);
    CHECK(host_window_shader_parse("crt") == HOST_SHADER_CRT);
    CHECK(host_window_shader_parse("lcd") == HOST_SHADER_LCD);
    CHECK(host_window_shader_parse("xbr") == -1 && host_window_shader_parse("") == -1);
    CHECK(host_window_shader_parse(NULL) == -1);
    CHECK(!strcmp(host_window_shader_name(HOST_SHADER_CRT), "crt") && !strcmp(host_window_shader_name(99), "none"));
    {
        HostShader s;
        unsetenv("RECOMP_SHADER");
        unsetenv("RECOMP_TEST_SHADER");
        unsetenv("RECOMP_SHADER_STRENGTH");
        unsetenv("RECOMP_SHADER_CURVATURE");
        unsetenv("RECOMP_SHADER_GHOSTING");
        host_window_env_shader("RECOMP_TEST_SHADER", HOST_SHADER_NONE, &s);
        CHECK(s.preset == HOST_SHADER_NONE && s.strength == -1 && s.curvature == 0 && s.ghosting == 0);
        setenv("RECOMP_SHADER", "scanlines", 1);
        setenv("RECOMP_SHADER_STRENGTH", "250", 1);
        setenv("RECOMP_SHADER_CURVATURE", "30", 1);
        setenv("RECOMP_SHADER_GHOSTING", "x", 1);
        host_window_env_shader("RECOMP_TEST_SHADER", HOST_SHADER_NONE, &s);
        CHECK(s.preset == HOST_SHADER_SCANLINES && s.strength == 100 && s.curvature == 30 && s.ghosting == 0);
        setenv("RECOMP_TEST_SHADER", "lcd", 1);
        host_window_env_shader("RECOMP_TEST_SHADER", HOST_SHADER_NONE, &s);
        CHECK(s.preset == HOST_SHADER_LCD);
        host_window_env_shader(NULL, HOST_SHADER_NONE, &s);
        CHECK(s.preset == HOST_SHADER_SCANLINES);
        setenv("RECOMP_SHADER", "bogus", 1);
        host_window_env_shader(NULL, HOST_SHADER_SHARP, &s);
        CHECK(s.preset == HOST_SHADER_SHARP);
        unsetenv("RECOMP_SHADER");
        unsetenv("RECOMP_TEST_SHADER");
        unsetenv("RECOMP_SHADER_STRENGTH");
        unsetenv("RECOMP_SHADER_CURVATURE");
        unsetenv("RECOMP_SHADER_GHOSTING");
    }

    if (host_window_headless()) {
        printf("host_layer shaders: headless (XBR_BUILD_ONLY or no display), skipped\n");
        return s_fail ? 1 : 77;
    }
    host_window_set_scaling(HOST_SCALE_INTEGER, HOST_FILTER_NEAREST);
    if (!host_window_open("xbr_host shaders", WW, WH, 0)) {
        fprintf(stderr, "host_layer shaders: cannot open a window with a GL context\n");
        return 1;
    }
    HostWindowInfo info;
    host_window_info(&info);
    if (info.drawable_w != WW || info.drawable_h != WH) {
        printf("host_layer shaders: drawable %dx%d, not %dx%d; skipped\n", info.drawable_w, info.drawable_h, WW, WH);
        host_window_close();
        SDL_Quit();
        return s_fail ? 1 : 77;
    }
    for (int i = 0; i < SW * SH; i++)
        flat[i] = 0xFF808080u;   /* mid grey */

    /* none: the picture as it is. */
    CHECK(shot(flat, HOST_SHADER_NONE, -1, &none));
    if (none.rgb) {
        CHECK(ppm_at(&none, 0, 0, 0) == 128 && ppm_at(&none, WW - 1, WH - 1, 1) == 128);
        CHECK(ppm_mean_row(&none, 2, 0) == ppm_mean_row(&none, 3, 0));
    }

    /* scanlines: each console line is 4 rows; rows 2-3 darker than none, rows 0-1 no darker. */
    CHECK(shot(flat, HOST_SHADER_SCANLINES, -1, &p));
    CHECK(host_window_shader_active() == HOST_SHADER_SCANLINES);
    if (p.rgb && none.rgb) {
        for (int line = 0; line < SH; line += 37) {
            double r0 = ppm_mean_row(&p, line * 4, 1), r3 = ppm_mean_row(&p, line * 4 + 3, 1);
            CHECK(r0 >= ppm_mean_row(&none, line * 4, 1));
            CHECK(r3 < 0.8 * ppm_mean_row(&none, line * 4 + 3, 1));
            CHECK(r0 > r3 + 30);
        }
        printf("host_layer shaders: scanlines rows 0..3 %.0f %.0f %.0f %.0f (none 128)\n", ppm_mean_row(&p, 0, 1),
               ppm_mean_row(&p, 1, 1), ppm_mean_row(&p, 2, 1), ppm_mean_row(&p, 3, 1));
        /* Columns all alike: no horizontal effect. */
        CHECK(ppm_mean_col(&p, 0, 1) == ppm_mean_col(&p, 1, 1));
    }
    free(p.rgb);
    p.rgb = NULL;

    /* Strength 0: the picture as none draws it. */
    CHECK(shot(flat, HOST_SHADER_SCANLINES, 0, &p));
    if (p.rgb && none.rgb)
        CHECK(!memcmp(p.rgb, none.rgb, (size_t)WW * WH * 3));
    free(p.rgb);
    p.rgb = NULL;

    /* CRT: scanlines plus the aperture grille, red / green / blue stripes one pixel wide. */
    CHECK(shot(flat, HOST_SHADER_CRT, -1, &p));
    if (p.rgb) {
        CHECK(ppm_mean_row(&p, 0, 1) > ppm_mean_row(&p, 3, 1) + 20);
        CHECK(ppm_mean_col(&p, 0, 0) > ppm_mean_col(&p, 0, 1) + 10);   /* column 0: red */
        CHECK(ppm_mean_col(&p, 1, 1) > ppm_mean_col(&p, 1, 2) + 10);   /* column 1: green */
        CHECK(ppm_mean_col(&p, 2, 2) > ppm_mean_col(&p, 2, 0) + 10);   /* column 2: blue */
    }
    free(p.rgb);
    p.rgb = NULL;
    {
        /* Curvature: the corners go black, the centre stays lit. */
        HostShader s = { HOST_SHADER_CRT, -1, 100, 0 };
        char path[512];
        snprintf(path, sizeof path, "%s/xbr_host_shader_c_%d.ppm", tmp && tmp[0] ? tmp : "/tmp", (int)getpid());
        host_window_set_shader(&s);
        host_window_capture(path);
        CHECK(host_window_present_rgba(flat, SW, SH, SW * 4));
        CHECK(ppm_read(path, &p));
        if (p.rgb) {
            CHECK(ppm_at(&p, 1, 1, 0) == 0 && ppm_at(&p, WW - 2, WH - 2, 1) == 0);
            CHECK(ppm_at(&p, WW / 2, WH / 2, 0) > 64);
        }
        free(p.rgb);
        p.rgb = NULL;
    }

    /* LCD: the last column and row of every 4x4 cell darker, the inside as none. */
    CHECK(shot(flat, HOST_SHADER_LCD, -1, &p));
    if (p.rgb && none.rgb) {
        CHECK(ppm_at(&p, 1, 1, 0) == ppm_at(&none, 1, 1, 0));
        CHECK(ppm_at(&p, 3, 1, 0) < ppm_at(&none, 3, 1, 0) - 30);     /* the cell's right edge */
        CHECK(ppm_at(&p, 1, 3, 0) < ppm_at(&none, 1, 3, 0) - 30);     /* its bottom edge */
        CHECK(ppm_at(&p, 401, 205, 1) == ppm_at(&none, 401, 205, 1));
        CHECK(ppm_at(&p, 403, 205, 1) < ppm_at(&none, 403, 205, 1) - 30);
    }
    free(p.rgb);
    p.rgb = NULL;
    {
        /* Ghosting: after a white picture, a black one shows part of the white one. */
        static uint32_t white[SW * SH], black[SW * SH];
        HostShader s = { HOST_SHADER_LCD, 0, 0, 100 };
        char path[512];
        for (int i = 0; i < SW * SH; i++) {
            white[i] = 0xFFFFFFFFu;
            black[i] = 0xFF000000u;
        }
        snprintf(path, sizeof path, "%s/xbr_host_shader_g_%d.ppm", tmp && tmp[0] ? tmp : "/tmp", (int)getpid());
        host_window_set_shader(&s);
        CHECK(host_window_present_rgba(white, SW, SH, SW * 4));
        host_window_capture(path);
        CHECK(host_window_present_rgba(black, SW, SH, SW * 4));
        CHECK(ppm_read(path, &p));
        if (p.rgb)
            CHECK(ppm_at(&p, WW / 2, WH / 2, 0) > 100 && ppm_at(&p, WW / 2, WH / 2, 0) < 155);
        free(p.rgb);
        p.rgb = NULL;
        host_window_capture(path);
        CHECK(host_window_present_rgba(black, SW, SH, SW * 4));   /* black after black: black */
        CHECK(ppm_read(path, &p));
        if (p.rgb)
            CHECK(ppm_at(&p, WW / 2, WH / 2, 0) == 0);
        free(p.rgb);
        p.rgb = NULL;
    }

    /* Sharp bilinear at a non-integer scale (stretch, linear: a 150-wide picture in 640 columns is 4.27x): the
     * black / white edge blends over fewer columns than plain linear. */
    {
        static uint32_t e2[150 * SH];
        int n_none = 0, n_sharp = 0;
        char path[512];
        for (int y = 0; y < SH; y++)
            for (int x = 0; x < 150; x++)
                e2[y * 150 + x] = x < 75 ? 0xFF000000u : 0xFFFFFFFFu;
        snprintf(path, sizeof path, "%s/xbr_host_shader_s_%d.ppm", tmp && tmp[0] ? tmp : "/tmp", (int)getpid());
        host_window_set_scaling(HOST_SCALE_STRETCH, HOST_FILTER_LINEAR);
        for (int pass = 0; pass < 2; pass++) {
            HostShader s = { pass ? HOST_SHADER_SHARP : HOST_SHADER_NONE, -1, 0, 0 };
            host_window_set_shader(&s);
            host_window_capture(path);
            CHECK(host_window_present_rgba(e2, 150, SH, 150 * 4));
            CHECK(ppm_read(path, &p));
            if (p.rgb) {
                int n = 0;
                for (int x = 0; x < p.w; x++) {
                    int v = ppm_at(&p, x, WH / 2, 1);
                    n += v > 8 && v < 247;
                }
                if (pass)
                    n_sharp = n;
                else
                    n_none = n;
                CHECK(ppm_at(&p, 10, WH / 2, 1) < 8 && ppm_at(&p, WW - 10, WH / 2, 1) > 247);
            }
            free(p.rgb);
            p.rgb = NULL;
        }
        printf("host_layer shaders: edge blend columns: linear %d, sharp bilinear %d\n", n_none, n_sharp);
        CHECK(n_sharp <= 2 && n_sharp < n_none);   /* at most the one output pixel straddling the edge */
        /* The other scale modes and an upscaled-sized picture draw too. */
        host_window_set_scaling(HOST_SCALE_ASPECT, HOST_FILTER_LINEAR);
        CHECK(host_window_present_rgba(e2, 150, SH, 150 * 4));
    }

    /* The screenshot stays the raw picture, whatever the shader. */
    {
        HostShader s = { HOST_SHADER_CRT, 100, 100, 0 };
        unsigned char hdr[54], pix[3];
        host_window_set_scaling(HOST_SCALE_INTEGER, HOST_FILTER_NEAREST);
        host_window_set_shader(&s);
        CHECK(host_window_present_rgba(flat, SW, SH, SW * 4));
        snprintf(bmp, sizeof bmp, "%s/xbr_host_shader_%d.bmp", tmp && tmp[0] ? tmp : "/tmp", (int)getpid());
        CHECK(host_window_screenshot_bmp(bmp) == 0);
        FILE *f = fopen(bmp, "rb");
        CHECK(f && fread(hdr, 1, sizeof hdr, f) == sizeof hdr && fread(pix, 1, 3, f) == 3);
        if (f)
            fclose(f);
        int32_t bw, bh;
        memcpy(&bw, hdr + 18, 4);
        memcpy(&bh, hdr + 22, 4);
        CHECK(bw == SW && bh == SH && pix[0] == 128 && pix[1] == 128 && pix[2] == 128);
        unlink(bmp);
    }

    /* A live change (the in-game menu, host_window_request_picture): from another thread, taken by the next
     * present only; -1 / NULL keep what is there. */
    {
        pthread_t th;
        host_window_set_scaling(HOST_SCALE_ASPECT, HOST_FILTER_LINEAR);
        host_window_set_shader(NULL);
        CHECK(host_window_present_rgba(flat, SW, SH, SW * 4));
        CHECK(pthread_create(&th, NULL, live_request, NULL) == 0);
        pthread_join(th, NULL);
        CHECK(host_window_shader_active() == HOST_SHADER_NONE);   /* not before the next present */
        CHECK(host_window_present_rgba(flat, SW, SH, SW * 4));
        CHECK(host_window_shader_active() == HOST_SHADER_SCANLINES);
        host_window_request_picture(-1, -1, NULL);
        CHECK(host_window_present_rgba(flat, SW, SH, SW * 4));
        CHECK(host_window_shader_active() == HOST_SHADER_SCANLINES);
        host_window_set_shader(NULL);
    }

    /* A shader that does not build: none, logged once, presents still work. */
    setenv("RECOMP_SHADER_TEST_FAIL", "lcd", 1);
    host_window_close();
    CHECK(host_window_open("xbr_host shaders", WW, WH, 0));
    CHECK(shot(flat, HOST_SHADER_LCD, -1, &p));
    CHECK(host_window_shader_active() == HOST_SHADER_NONE);
    if (p.rgb && none.rgb)
        CHECK(!memcmp(p.rgb, none.rgb, (size_t)WW * WH * 3));
    free(p.rgb);
    p.rgb = NULL;
    unsetenv("RECOMP_SHADER_TEST_FAIL");

    /* Cost: milliseconds per present, vsync off, in a fresh window of HOST_LAYER_BENCH_SIZE (WxH, default 640x480)
     * for HOST_LAYER_BENCH presents (default 30); on Mesa's llvmpipe a relative cost only. */
    {
        int bw = WW, bh = WH;
        const char *sz = getenv("HOST_LAYER_BENCH_SIZE");
        int bench_n = getenv("HOST_LAYER_BENCH") ? atoi(getenv("HOST_LAYER_BENCH")) : 30;
        if (sz && sscanf(sz, "%dx%d", &bw, &bh) != 2) {
            bw = WW;
            bh = WH;
        }
        host_window_close();
        host_window_set_scaling(HOST_SCALE_ASPECT, HOST_FILTER_LINEAR);
        CHECK(host_window_open("xbr_host shaders", bw, bh, 0));
        host_window_info(&info);
        printf("host_layer shaders: cost at %dx%d (GL%s %d.%d), ms per present of a %dx%d picture:",
               info.drawable_w, info.drawable_h, info.gles ? " ES" : "", info.gl_major, info.gl_minor, SW, SH);
        for (int i = 0; i < HOST_SHADER_COUNT; i++)
            printf(" %s %.3f", host_window_shader_name(i), bench(flat, i, bench_n));
        printf("\n");
    }
    host_window_set_shader(NULL);
    host_window_close();
    free(none.rgb);
    SDL_Quit();
    printf("%s\n", s_fail ? "host_layer shaders: FAIL" : "host_layer shaders: ok");
    return s_fail;
}

/* ── The shared shader module (present_shaders.c, XBA-203) ──────────────
 * Its pure half on every host: names, keys, strengths, console / Xbox lines, the sources of each dialect. With a
 * display, every preset built in each GL context the host can make: GLES 3.0 (GLSL ES 3.00 and 1.00, the console
 * window and SDL's opengles2 renderer), a 3.2 core context (GLSL 1.50) and a compatibility one (GLSL 1.20, SDL's
 * opengl renderer). */
static int src_has(PresentGlsl d, int preset, const char *needle)
{
    const char *src[4];
    int n = 0;

    present_shader_fs_src(d, preset, NULL, src, &n);
    for (int i = 0; i < n; i++)
        if (strstr(src[i], needle))
            return 1;
    return 0;
}

/* Builds every preset in a fresh context of that kind; -1 when the context cannot be made here. */
static int build_all(const char *what, int es, int major, int minor, int profile, PresentGlsl d1, int d2)
{
    SDL_Window *w;
    SDL_GLContext ctx;
    int built = 0;

    SDL_GL_ResetAttributes();
    if (profile)
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, profile);
    if (es)
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    if (major) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
    }
    w = SDL_CreateWindow(what, 0, 0, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    ctx = w ? SDL_GL_CreateContext(w) : NULL;
    if (!ctx) {
        printf("host_layer present_shaders: no %s context here (%s), skipped\n", what, SDL_GetError());
        if (w)
            SDL_DestroyWindow(w);
        return -1;
    }
    SDL_GL_MakeCurrent(w, ctx);
    for (int k = 0; k < 2; k++) {
        int d = k ? d2 : (int)d1;
        if (d < 0)
            continue;
        for (int p = HOST_SHADER_SHARP; p < HOST_SHADER_COUNT; p++) {
            PresentShaderProg sp = { 0 };
            int ok = present_shader_build(&sp, (PresentGlsl)d, p, "[TEST]");
            if (!ok)
                fprintf(stderr, "FAIL: %s dialect %d preset %s does not build\n", what, d, present_shader_name(p));
            CHECK(ok && sp.prog && sp.u_tex >= 0);   /* unused uniforms are optimised out (-1) */
            /* The legacy vertex shader carries the crop and the legacy fragment header the swap. */
            CHECK((d == PRESENT_GLSL_ES100 || d == PRESENT_GLSL_120) == (sp.u_rect >= 0));
            built += ok;
            present_shader_release(&sp, 0);
        }
    }
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(w);
    printf("host_layer present_shaders: %s: %d programs built\n", what, built);
    return built;
}

static int run_present_shaders(void)
{
    HostShader s = { HOST_SHADER_CRT, -1, 0, 0 }, c;
    const char *src[4];
    int n = -1;

    CHECK(present_shader_parse("CRT") == HOST_SHADER_CRT && present_shader_parse("lcd") == HOST_SHADER_LCD);
    CHECK(present_shader_parse("") == -1 && present_shader_parse(NULL) == -1 && present_shader_parse("xbr") == -1);
    CHECK(!strcmp(present_shader_name(HOST_SHADER_SHARP), "sharp") && !strcmp(present_shader_name(-1), "none"));
    CHECK(present_shader_strength(&s) == 60);
    s.preset = HOST_SHADER_SCANLINES;
    CHECK(present_shader_strength(&s) == 50);
    s.strength = 150;
    CHECK(present_shader_strength(&s) == 100);
    s.preset = HOST_SHADER_NONE;
    CHECK(present_shader_strength(&s) == 0);
    s = (HostShader){ 9, 30, 30, 30 };
    c = present_shader_clamp(&s);
    CHECK(c.preset == HOST_SHADER_NONE);
    s = (HostShader){ HOST_SHADER_CRT, -7, 300, -2 };
    c = present_shader_clamp(&s);
    CHECK(c.preset == HOST_SHADER_CRT && c.strength == -1 && c.curvature == 100 && c.ghosting == 0);
    c = present_shader_clamp(NULL);
    CHECK(c.preset == HOST_SHADER_NONE && c.strength == -1);

    /* Keys: the console's variable, then RECOMP_SHADER. */
    setenv("RECOMP_SHADER", "scanlines", 1);
    setenv("RECOMP_XBOX_SHADER", "sharp", 1);
    setenv("RECOMP_SHADER_STRENGTH", "35", 1);
    setenv("RECOMP_SHADER_CURVATURE", "x", 1);
    present_shader_env("RECOMP_XBOX_SHADER", HOST_SHADER_NONE, &c);
    CHECK(c.preset == HOST_SHADER_SHARP && c.strength == 35 && c.curvature == 0 && c.ghosting == 0);
    setenv("RECOMP_XBOX_SHADER", "bogus", 1);
    present_shader_env("RECOMP_XBOX_SHADER", HOST_SHADER_NONE, &c);
    CHECK(c.preset == HOST_SHADER_SCANLINES);
    unsetenv("RECOMP_SHADER");
    present_shader_env(NULL, HOST_SHADER_CRT, &c);
    CHECK(c.preset == HOST_SHADER_CRT);
    unsetenv("RECOMP_XBOX_SHADER");
    unsetenv("RECOMP_SHADER_STRENGTH");
    unsetenv("RECOMP_SHADER_CURVATURE");

    /* Lines: a console picture's, upscaled or not; an Xbox frame's, line pairs below 720. */
    CHECK(present_shader_console_cells(160) == 160 && present_shader_console_cells(240) == 240);
    CHECK(present_shader_console_cells(960) == 240 && present_shader_console_cells(480) == 240);
    CHECK(present_shader_console_cells(0) == 1);
    CHECK(present_shader_xbox_cells(480) == 240 && present_shader_xbox_cells(1440) == 240);
    CHECK(present_shader_xbox_cells(576) == 288 && present_shader_xbox_cells(720) == 720);
    CHECK(present_shader_xbox_cells(1920) == 240 && present_shader_xbox_cells(2160) == 720);   /* 4 x 480, 3 x 720 */
    CHECK(present_shader_xbox_cells(1080) == 1080);   /* none of the three: each line */
    CHECK(present_shader_xbox_cells(0) == 1);

    /* Sources: none has no fragment shader; each dialect its header; the bodies shared. */
    present_shader_fs_src(PRESENT_GLSL_ES300, HOST_SHADER_NONE, NULL, src, &n);
    CHECK(n == 0);
    present_shader_fs_src(PRESENT_GLSL_ES100, HOST_SHADER_CRT, NULL, src, &n);
    CHECK(n == 4 && !strncmp(src[0], "#version 100\n", 13));
    present_shader_vs_src(PRESENT_GLSL_120, src, &n);
    CHECK(n == 2 && !strcmp(src[0], "#version 120\n") && strstr(src[1], "a_pos") && strstr(src[1], "u_rect"));
    present_shader_vs_src(PRESENT_GLSL_ES300, src, &n);
    CHECK(n == 2 && strstr(src[1], "gl_VertexID") && !strstr(src[1], "a_pos"));
    CHECK(src_has(PRESENT_GLSL_150, HOST_SHADER_LCD, "#version 150") && src_has(PRESENT_GLSL_150, HOST_SHADER_LCD, "u_ghost"));
    CHECK(src_has(PRESENT_GLSL_ES100, HOST_SHADER_SCANLINES, "texture2D") && src_has(PRESENT_GLSL_120, HOST_SHADER_SHARP, "u_swap"));
    CHECK(!src_has(PRESENT_GLSL_ES300, HOST_SHADER_SCANLINES, "texture2D"));
    present_shader_fs_src(PRESENT_GLSL_ES300, HOST_SHADER_CRT, "not glsl;\n", src, &n);
    CHECK(n == 3 && !strcmp(src[2], "not glsl;\n"));

    if (host_window_headless()) {
        printf("host_layer present_shaders: no display, GL builds skipped; %s\n", s_fail ? "FAIL" : "ok");
        return s_fail;
    }
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("host_layer present_shaders: SDL video unavailable (%s), GL builds skipped\n", SDL_GetError());
        return s_fail;
    }
    {
        int es3 = build_all("GLES 3.0", 1, 3, 0, 0, PRESENT_GLSL_ES300, PRESENT_GLSL_ES100);
        int es2 = build_all("GLES 2.0", 1, 2, 0, 0, PRESENT_GLSL_ES100, -1);
        int core = build_all("GL 3.2 core", 0, 3, 2, SDL_GL_CONTEXT_PROFILE_CORE, PRESENT_GLSL_150, -1);
        int compat = build_all("GL compatibility", 0, 0, 0, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY, PRESENT_GLSL_120, -1);
        CHECK(es3 != 0 && es2 != 0 && core != 0 && compat != 0);   /* -1 (not here) is fine, 0 built is not */
        CHECK(es3 > 0 || es2 > 0 || core > 0 || compat > 0);
    }
    SDL_Quit();
    printf("%s\n", s_fail ? "host_layer present_shaders: FAIL" : "host_layer present_shaders: ok");
    return s_fail;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "core";

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!strcmp(mode, "core"))
        return run_core();
    if (!strcmp(mode, "window"))
        return run_window();
    if (!strcmp(mode, "shaders"))
        return run_shaders();
    if (!strcmp(mode, "present_shaders"))
        return run_present_shaders();
    fprintf(stderr, "usage: %s core|window|shaders|present_shaders\n", argv[0]);
    return 2;
}
