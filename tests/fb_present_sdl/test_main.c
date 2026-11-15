/*
 * The Xbox presenter's picture shaders (XBA-203): src/video/fb_present_sdl.c draws its frames through
 * present_shaders.c's presets inside SDL's renderer.
 *
 * Each case runs in a child process (xbox_HostWindowMain opens SDL once per process): it sets the RECOMP_* keys,
 * opens the window (RECOMP_FB_WINDOW=1, 1280x960 at RECOMP_WINDOW_SCALE=2), and its guest thread presents a frame
 * (xbox_FramebufferWindowPresentHost), captures the window as drawn (xbox_FramebufferWindowCapture) and checks the
 * pixels; the child's exit status is the verdict. Every case runs with SDL's opengl renderer (GLSL 1.20) and, when
 * the display offers GLES, its opengles2 one (GLSL ES 1.00, where SDL keeps XRGB textures as swapped RGBA):
 *   none        a grey frame stays grey, the red block red
 *   scanlines   240 scanlines over 960 rows (a 480-line frame shows line pairs): rows 0-1 of every 4 brighter, 2-3
 *               darker, columns alike; the BMP screenshot stays the raw grey
 *   rgbx        the same through an R, G, B, X frame (no swap)
 *   crt         columns 0 / 1 / 2 of every 3 red / green / blue
 *   curve       CRT with curvature 100: the corners black
 *   sharp       a black / white edge in a 300x225 frame (4.27x) blends over fewer columns than the linear plain copy
 *   linear      that plain copy
 *   lcd         no LCD grid for an Xbox: drawn as none, none active
 *   override    RECOMP_XBOX_SHADER=scanlines wins over RECOMP_SHADER=crt
 *   fail        RECOMP_SHADER_TEST_FAIL=crt: the shader does not build, none is drawn, none active
 *   live        none at the start, scanlines set live (xbox_FramebufferWindowSetShader, the in-game menu) drawn
 *               from the window's next turn, then lcd set live: none
 * Skipped (77) with no display or under XBR_BUILD_ONLY. `fb_present_sdl_test <case>` runs one case.
 */
#include "video_player.h"
#include "present_shaders.h"

#include <SDL.h>

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

void *recomp_lookup(unsigned long address) { (void)address; abort(); }
void *recomp_lookup_manual(unsigned long address) { (void)address; abort(); }

enum { FW = 640, FH = 480, SW = 300, SH = 225, WW = 1280, WH = 960 };

static const char *s_case;
static char s_dir[512];
static int s_fail;

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "  FAIL %s: %s (line %d)\n", s_case, #c, __LINE__); s_fail = 1; } } while (0)

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

typedef struct {
    int w, h;
    unsigned char *rgb;
} Ppm;

static int ppm_read(const char *path, Ppm *out)
{
    FILE *f = fopen(path, "rb");
    int max = 0;

    out->rgb = NULL;
    if (!f)
        return 0;
    if (fscanf(f, "P6 %d %d %d", &out->w, &out->h, &max) != 3 || out->w <= 0 || out->h <= 0 || max != 255
            || fgetc(f) == EOF) {
        fclose(f);
        return 0;
    }
    out->rgb = malloc((size_t)out->w * out->h * 3);
    if (!out->rgb || fread(out->rgb, 3, (size_t)out->w * out->h, f) != (size_t)out->w * out->h) {
        free(out->rgb);
        out->rgb = NULL;
        fclose(f);
        return 0;
    }
    fclose(f);
    return 1;
}

static int at(const Ppm *p, int x, int y, int c)
{
    return p->rgb[((size_t)y * p->w + x) * 3 + c];
}

/* The mean of channel c over the rectangle x0..x1 x y0..y1 (exclusive ends). */
static double mean(const Ppm *p, int x0, int y0, int x1, int y1, int c)
{
    double s = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            s += at(p, x, y, c);
    return s / ((double)(x1 - x0) * (y1 - y0));
}

/* A 640x480 frame: grey 128, a red block over 0..63 x 0..63; rgbx: bytes R, G, B, X, else 0xXXRRGGBB words. */
static uint32_t *grey_frame(int rgbx)
{
    uint32_t *px = malloc(FW * FH * 4);
    for (int y = 0; y < FH; y++)
        for (int x = 0; x < FW; x++) {
            int red = x < 64 && y < 64;
            uint8_t *b = (uint8_t *)&px[y * FW + x];
            uint8_t r = red ? 255 : 128, g = red ? 0 : 128, bl = red ? 0 : 128;
            if (rgbx) {
                b[0] = r; b[1] = g; b[2] = bl; b[3] = 0;
            } else {
                b[0] = bl; b[1] = g; b[2] = r; b[3] = 0;
            }
        }
    return px;
}

/* Presents the frame until the window has drawn it and written the capture. */
static int shot(const uint32_t *px, int w, int h, uint32_t format, Ppm *out)
{
    char path[600];

    snprintf(path, sizeof path, "%s/shot_%s.ppm", s_dir, s_case);
    unlink(path);
    xbox_FramebufferWindowCapture(path);
    for (int i = 0; i < 200; i++) {
        xbox_FramebufferWindowPresentHost(px, (uint32_t)w, (uint32_t)h, (uint32_t)w * 4u, format);
        sleep_ms(25);
        if (access(path, R_OK) == 0)
            break;
    }
    if (!ppm_read(path, out)) {
        fprintf(stderr, "  FAIL %s: no capture\n", s_case);
        return 0;
    }
    unlink(path);
    if (out->w != WW || out->h != WH) {
        fprintf(stderr, "  FAIL %s: capture %dx%d, not %dx%d\n", s_case, out->w, out->h, WW, WH);
        return 0;
    }
    return 1;
}

/* Grey everywhere outside the red block (none, lcd, fail), and the block red. */
static void check_plain(const Ppm *p)
{
    int bad = 0;
    for (int y = 0; y < WH; y += 3)
        for (int x = (y < 128 ? 128 : 0); x < WW; x += 3)
            for (int c = 0; c < 3; c++)
                bad += abs(at(p, x, y, c) - 128) > 2;
    CHECK(bad == 0);
    CHECK(mean(p, 16, 16, 112, 112, 0) > 240 && mean(p, 16, 16, 112, 112, 2) < 15);
}

/* Scanlines: 4 output rows per scanline (240 over 960), rows 0-1 bright, 2-3 dark; columns alike. */
static void check_scanlines(const Ppm *p)
{
    double r0 = mean(p, 200, 400, 1200, 401, 1), r1 = mean(p, 200, 401, 1200, 402, 1);
    double r2 = mean(p, 200, 402, 1200, 403, 1), r3 = mean(p, 200, 403, 1200, 404, 1);
    printf("  %s: rows %.0f %.0f %.0f %.0f (plain 128)\n", s_case, r0, r1, r2, r3);
    CHECK(r0 > 134 && r1 > 134 && r2 < 100 && r3 < 100);
    CHECK(mean(p, 200, 404, 1200, 405, 1) > 134 && mean(p, 200, 406, 1200, 407, 1) < 100);   /* the next line */
    CHECK(abs((int)mean(p, 400, 200, 401, 800, 1) - (int)mean(p, 401, 200, 402, 800, 1)) <= 2);
    /* The red block keeps its colour (a swapped texture would show blue). */
    CHECK(mean(p, 16, 16, 112, 112, 0) > 150 && mean(p, 16, 16, 112, 112, 2) < 30);
}

/* Over one grey output row of a bright half-line (row 400), column x's channel against the others. */
static int column_lead(const Ppm *p, int phase, int c)
{
    int lead = 0, n = 0;
    for (int x = 600 + phase; x < 900; x += 3, n++) {
        int v = at(p, x, 400, c), o1 = at(p, x, 400, (c + 1) % 3), o2 = at(p, x, 400, (c + 2) % 3);
        lead += v > o1 + 10 && v > o2 + 10;
    }
    return lead == n;
}

static int read_bmp_centre(const char *path, int rgb[3])
{
    FILE *f = fopen(path, "rb");
    uint8_t hdr[54], px[3];
    int32_t w, h;
    uint32_t off, row;

    if (!f)
        return 0;
    if (fread(hdr, 1, 54, f) != 54 || hdr[0] != 'B' || hdr[1] != 'M') {
        fclose(f);
        return 0;
    }
    memcpy(&off, hdr + 10, 4);
    memcpy(&w, hdr + 18, 4);
    memcpy(&h, hdr + 22, 4);
    row = ((uint32_t)w * 3u + 3u) & ~3u;
    if (fseek(f, (long)(off + row * (uint32_t)(h / 2) + (uint32_t)(w / 2) * 3u), SEEK_SET) != 0
            || fread(px, 1, 3, f) != 3) {
        fclose(f);
        return 0;
    }
    fclose(f);
    rgb[0] = px[2];
    rgb[1] = px[1];
    rgb[2] = px[0];
    return w == FW && h == FH;
}

/* Columns of a middle row that are neither black nor white. */
static int edge_columns(const Ppm *p)
{
    int n = 0;
    for (int x = 0; x < WW; x++) {
        int v = at(p, x, WH / 2, 1);
        n += v > 12 && v < 243;
    }
    return n;
}

static int guest_main(void)
{
    Ppm p = { 0 };
    int rgbx = !strcmp(s_case, "rgbx");
    uint32_t *px;

    if (!strcmp(s_case, "sharp") || !strcmp(s_case, "linear")) {
        /* Not 640x480: the presenter filters it linearly (the plain copy), the sharp preset blends itself. */
        px = malloc(SW * SH * 4);
        for (int y = 0; y < SH; y++)
            for (int x = 0; x < SW; x++)
                px[y * SW + x] = x < SW / 2 ? 0x00000000u : 0x00FFFFFFu;
        if (shot(px, SW, SH, XBOX_FB_HOST_BGRX8888, &p)) {
            int n = edge_columns(&p);
            printf("  %s: %d blended columns at the edge\n", s_case, n);
            CHECK(mean(&p, 0, 400, 400, 560, 1) < 3 && mean(&p, 880, 400, WW, 560, 1) > 252);   /* the frame drawn */
            if (!strcmp(s_case, "sharp")) {
                CHECK(xbox_FramebufferWindowShaderActive() == HOST_SHADER_SHARP);
                CHECK(n <= 2);   /* 4.27x: the blend over 1/4 texel, at most a column or two */
            } else
                CHECK(n >= 3);
        } else
            s_fail = 1;
        free(px);
        free(p.rgb);
        return s_fail;
    }
    px = grey_frame(rgbx);
    if (!shot(px, FW, FH, rgbx ? XBOX_FB_HOST_RGBX8888 : XBOX_FB_HOST_BGRX8888, &p)) {
        free(px);
        return 1;
    }
    if (!strcmp(s_case, "live")) {
        /* The in-game menu's live change (xbox_FramebufferWindowSetShader): started without one, scanlines from the
         * window's next turn, then lcd, which an Xbox frame draws as none. */
        CHECK(xbox_FramebufferWindowShaderActive() == HOST_SHADER_NONE);
        check_plain(&p);
        free(p.rgb);
        p.rgb = NULL;
        xbox_FramebufferWindowSetShader(HOST_SHADER_SCANLINES, -1, 0, 0);
        for (int i = 0; i < 200 && xbox_FramebufferWindowShaderActive() != HOST_SHADER_SCANLINES; i++)
            sleep_ms(10);
        CHECK(xbox_FramebufferWindowShaderActive() == HOST_SHADER_SCANLINES);
        if (shot(px, FW, FH, XBOX_FB_HOST_BGRX8888, &p))
            check_scanlines(&p);
        else
            s_fail = 1;
        xbox_FramebufferWindowSetShader(HOST_SHADER_LCD, -1, 0, 50);
        for (int i = 0; i < 200 && xbox_FramebufferWindowShaderActive() != HOST_SHADER_NONE; i++)
            sleep_ms(10);
        CHECK(xbox_FramebufferWindowShaderActive() == HOST_SHADER_NONE);
    } else if (!strcmp(s_case, "none") || !strcmp(s_case, "lcd") || !strcmp(s_case, "fail")) {
        CHECK(xbox_FramebufferWindowShaderActive() == HOST_SHADER_NONE);
        check_plain(&p);
    } else if (!strcmp(s_case, "scanlines") || !strcmp(s_case, "rgbx") || !strcmp(s_case, "override")) {
        CHECK(xbox_FramebufferWindowShaderActive() == HOST_SHADER_SCANLINES);
        check_scanlines(&p);
        if (!strcmp(s_case, "scanlines")) {
            /* The screenshot stays the raw frame. */
            char bmp[600];
            int c[3] = { 0, 0, 0 };
            snprintf(bmp, sizeof bmp, "%s/shot.bmp", s_dir);
            for (int i = 0; i < 3; i++) {   /* the screenshot reads the slot behind the one on screen: fill it */
                xbox_FramebufferWindowPresentHost(px, FW, FH, FW * 4u, XBOX_FB_HOST_BGRX8888);
                sleep_ms(30);
            }
            CHECK(xbox_FramebufferDumpBmp(bmp) == 0);
            CHECK(read_bmp_centre(bmp, c));
            CHECK(c[0] == 128 && c[1] == 128 && c[2] == 128);
            unlink(bmp);
        }
    } else if (!strcmp(s_case, "crt")) {
        CHECK(xbox_FramebufferWindowShaderActive() == HOST_SHADER_CRT);
        /* gl_FragCoord.x 0, 1, 2 of every 3: red, green, blue. */
        CHECK(column_lead(&p, 0, 0) && column_lead(&p, 1, 1) && column_lead(&p, 2, 2));
        CHECK(mean(&p, 200, 402, 1200, 403, 1) < mean(&p, 200, 400, 1200, 401, 1) - 20);   /* its scanlines */
    } else if (!strcmp(s_case, "curve")) {
        CHECK(mean(&p, 0, 0, 8, 8, 0) < 2 && mean(&p, WW - 8, WH - 8, WW, WH, 1) < 2);
        CHECK(mean(&p, 600, 400, 680, 404, 1) > 60);   /* the middle still drawn */
    }
    free(px);
    free(p.rgb);
    return s_fail;
}

typedef struct {
    const char *name, *shader, *xbox_shader, *extra_k, *extra_v;
} Case;

static const Case k_cases[] = {
    { "none", NULL, NULL, NULL, NULL },
    { "scanlines", "scanlines", NULL, NULL, NULL },
    { "rgbx", "scanlines", NULL, NULL, NULL },
    { "crt", "crt", NULL, NULL, NULL },
    { "curve", "crt", NULL, "RECOMP_SHADER_CURVATURE", "100" },
    { "sharp", "sharp", NULL, NULL, NULL },
    { "linear", NULL, NULL, NULL, NULL },
    { "lcd", "lcd", NULL, NULL, NULL },
    { "override", "crt", "scanlines", NULL, NULL },
    { "fail", "crt", NULL, "RECOMP_SHADER_TEST_FAIL", "crt" },
    { "live", NULL, NULL, NULL, NULL },
};

/* Runs fn in a child under a time limit; its exit status (-1: killed or crashed). */
static int run_child(int (*fn)(const void *), const void *arg, int timeout_s)
{
    pid_t pid = fork();
    int st = 0;

    if (pid < 0)
        return -1;
    if (pid == 0)
        _exit(fn(arg));
    for (int i = 0; i < timeout_s * 20; i++) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid)
            return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        sleep_ms(50);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    fprintf(stderr, "  child %d timed out\n", (int)pid);
    return -1;
}

static int case_child(const void *arg)
{
    const Case *c = arg;

    s_case = c->name;
    s_fail = 0;   /* the parent's verdict so far is not this case's */
    setenv("RECOMP_FB_WINDOW", "1", 1);
    setenv("RECOMP_WINDOW_SCALE", "2", 1);
    setenv("RECOMP_PRESENT_VSYNC", "0", 1);
    unsetenv("RECOMP_HEADLESS");
    unsetenv("RECOMP_LSFG");
    unsetenv("RECOMP_ASPECT");
    unsetenv("RECOMP_SHADER_STRENGTH");
    if (c->shader)
        setenv("RECOMP_SHADER", c->shader, 1);
    else
        unsetenv("RECOMP_SHADER");
    if (c->xbox_shader)
        setenv("RECOMP_XBOX_SHADER", c->xbox_shader, 1);
    else
        unsetenv("RECOMP_XBOX_SHADER");
    if (c->extra_k)
        setenv(c->extra_k, c->extra_v, 1);
    return xbox_HostWindowMain(guest_main);
}

/* 0 when SDL can make a renderer of that name here. */
static int probe_child(const void *arg)
{
    const char *name = arg;
    SDL_Window *w;
    SDL_Renderer *r;
    SDL_RendererInfo ri;
    int ok = 0;

    setenv("SDL_RENDER_DRIVER", name, 1);   /* the environment wins over a hint: the parent may have set it */
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
        return 1;
    w = SDL_CreateWindow("probe", 0, 0, 64, 64, SDL_WINDOW_HIDDEN);
    r = w ? SDL_CreateRenderer(w, -1, 0) : NULL;
    if (r && SDL_GetRendererInfo(r, &ri) == 0 && !strcmp(ri.name, name))
        ok = 1;
    else
        printf("fb_present_sdl: %s renderer: %s\n", name, r ? ri.name : SDL_GetError());
    if (r)
        SDL_DestroyRenderer(r);
    if (w)
        SDL_DestroyWindow(w);
    SDL_Quit();
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    static const char *const k_renderers[] = { "opengl", "opengles2" };
    const char *tmp = getenv("TMPDIR");
    int ran = 0;

    setvbuf(stdout, NULL, _IONBF, 0);   /* nothing buffered is copied into the children */

    if (getenv("XBR_BUILD_ONLY") && getenv("XBR_BUILD_ONLY")[0]) {
        printf("fb_present_sdl: XBR_BUILD_ONLY, skipped\n");
        return 77;
    }
#if defined(__linux__)
    if (!(getenv("DISPLAY") && getenv("DISPLAY")[0]) && !(getenv("WAYLAND_DISPLAY") && getenv("WAYLAND_DISPLAY")[0])) {
        printf("fb_present_sdl: no display, skipped\n");
        return 77;
    }
#endif
    snprintf(s_dir, sizeof s_dir, "%s/xbr_fbsdl_%d", tmp && tmp[0] ? tmp : "/tmp", (int)getpid());
    if (mkdir(s_dir, 0700) != 0 && errno != EEXIST) {
        perror(s_dir);
        return 1;
    }
    for (size_t r = 0; r < sizeof k_renderers / sizeof k_renderers[0]; r++) {
        if (run_child(probe_child, k_renderers[r], 30) != 0) {
            printf("fb_present_sdl: no %s renderer here, its cases skipped\n", k_renderers[r]);
            continue;
        }
        setenv("SDL_RENDER_DRIVER", k_renderers[r], 1);
        for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
            if (argc > 1 && strcmp(argv[1], k_cases[i].name))   /* fb_present_sdl_test [case] */
                continue;
            int rc = run_child(case_child, &k_cases[i], 60);
            printf("fb_present_sdl %-9s %-10s %s\n", k_renderers[r], k_cases[i].name, rc == 0 ? "PASS" : "FAIL");
            if (rc != 0)
                s_fail = 1;
        }
        ran = 1;
    }
    rmdir(s_dir);
    if (!ran) {
        printf("fb_present_sdl: no GL renderer, skipped\n");
        return 77;
    }
    return s_fail;
}
