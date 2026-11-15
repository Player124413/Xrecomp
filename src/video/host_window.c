/* host_window.c -- see host_window.h (XBA-145). */
#include "host_window.h"
#include "host_pad.h"
#include "host_time.h"
#include "recomp_lifecycle.h"

#include <SDL.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#if defined(XBOXRECOMP_LSFG)
/* Frame generation in the console window (XBA-186, docs/technical/framegen.md « Console window »): only in a build
 * with XBOXRECOMP_LSFG (the root CMakeLists defines it on xbr_host), active under RECOMP_LSFG=1. */
#include "framegen_console.h"
#include "framegen_policy.h"
#include "recomp_states.h"
#if defined(XBOXRECOMP_LSFG_BRIDGE)
#include "lsfg_generator.h"
#endif
#endif

/* The few GL entry points the present needs, from SDL: no GL header or GL
 * library at build time, the same code on GLES 3.0 and desktop 3.2 core. */
#ifndef APIENTRY
#define APIENTRY
#endif

#define XGL_TEXTURE_2D          0x0DE1
#define XGL_UNSIGNED_BYTE       0x1401
#define XGL_RGBA                0x1908
#define XGL_RGBA8               0x8058
#define XGL_NEAREST             0x2600
#define XGL_LINEAR              0x2601
#define XGL_PACK_ALIGNMENT      0x0D05
#define XGL_TEXTURE_MAG_FILTER  0x2800
#define XGL_TEXTURE_MIN_FILTER  0x2801
#define XGL_TEXTURE_WRAP_S      0x2802
#define XGL_TEXTURE_WRAP_T      0x2803
#define XGL_CLAMP_TO_EDGE       0x812F
#define XGL_COLOR_BUFFER_BIT    0x4000
#define XGL_TRIANGLES           0x0004
#define XGL_TEXTURE0            0x84C0
#define XGL_TEXTURE1            0x84C1
#define XGL_FRAGMENT_SHADER     0x8B30
#define XGL_VERTEX_SHADER       0x8B31
#define XGL_COMPILE_STATUS      0x8B81
#define XGL_LINK_STATUS         0x8B82
#define XGL_UNPACK_ROW_LENGTH   0x0CF2
#define XGL_UNPACK_ALIGNMENT    0x0CF5
#define XGL_FRAMEBUFFER         0x8D40
#define XGL_SCISSOR_TEST        0x0C11
#define XGL_BLEND               0x0BE2
#define XGL_DEPTH_TEST          0x0B71

typedef struct {
    void (APIENTRY *GenTextures)(int, unsigned *);
    void (APIENTRY *DeleteTextures)(int, const unsigned *);
    void (APIENTRY *BindTexture)(unsigned, unsigned);
    void (APIENTRY *TexParameteri)(unsigned, unsigned, int);
    void (APIENTRY *TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
    void (APIENTRY *TexSubImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
    void (APIENTRY *PixelStorei)(unsigned, int);
    void (APIENTRY *Viewport)(int, int, int, int);
    void (APIENTRY *ClearColor)(float, float, float, float);
    void (APIENTRY *Clear)(unsigned);
    void (APIENTRY *Disable)(unsigned);
    unsigned (APIENTRY *CreateShader)(unsigned);
    void (APIENTRY *ShaderSource)(unsigned, int, const char *const *, const int *);
    void (APIENTRY *CompileShader)(unsigned);
    void (APIENTRY *GetShaderiv)(unsigned, unsigned, int *);
    void (APIENTRY *GetShaderInfoLog)(unsigned, int, int *, char *);
    void (APIENTRY *DeleteShader)(unsigned);
    unsigned (APIENTRY *CreateProgram)(void);
    void (APIENTRY *AttachShader)(unsigned, unsigned);
    void (APIENTRY *LinkProgram)(unsigned);
    void (APIENTRY *GetProgramiv)(unsigned, unsigned, int *);
    void (APIENTRY *GetProgramInfoLog)(unsigned, int, int *, char *);
    void (APIENTRY *DeleteProgram)(unsigned);
    void (APIENTRY *UseProgram)(unsigned);
    int (APIENTRY *GetUniformLocation)(unsigned, const char *);
    void (APIENTRY *Uniform1i)(int, int);
    void (APIENTRY *Uniform1f)(int, float);
    void (APIENTRY *Uniform2f)(int, float, float);
    void (APIENTRY *GenVertexArrays)(int, unsigned *);
    void (APIENTRY *DeleteVertexArrays)(int, const unsigned *);
    void (APIENTRY *BindVertexArray)(unsigned);
    void (APIENTRY *BindFramebuffer)(unsigned, unsigned);
    void (APIENTRY *ActiveTexture)(unsigned);
    void (APIENTRY *DrawArrays)(unsigned, int, int);
    unsigned (APIENTRY *GetError)(void);
    void (APIENTRY *ReadPixels)(int, int, int, int, unsigned, unsigned, void *);
} HostGl;

static SDL_Window *s_win;
static SDL_GLContext s_ctx;
static HostGl s_gl;
static unsigned s_prog, s_vao, s_tex;
static int s_tex_w, s_tex_h;
static unsigned s_tex_prev;               /* LCD ghosting: the previous picture (s_tex and it swap each upload) */
static int s_tex_prev_w, s_tex_prev_h, s_tex_prev_filter = -1;
static int s_u_tex, s_u_flip;
static int s_gles, s_major, s_minor, s_vsync;
static int s_quit;
static uint64_t s_frames;
static host_window_event_fn s_hook;
static void *s_hook_user;
static int s_scale = HOST_SCALE_ASPECT, s_filter = HOST_FILTER_LINEAR, s_tex_filter = -1;
static int s_aspect_num, s_aspect_den; /* host_window_set_aspect; 0: the picture's own */
static char s_capture[1024];
static int s_drawing_generated; /* the picture being drawn is a generated frame: no capture of it */
#if defined(XBOXRECOMP_LSFG)
static void fg_open(void);  /* frame generation (XBA-186), below present_rgba */
static void fg_close(void);
#endif

/* host_window_stats: written by the presenting thread, read from any. */
static int s_open;
static uint64_t s_frames_pub, s_last_present_ns, s_fps_t0, s_fps_n;
static uint32_t s_fps_centi;

/* host_window_screenshot_bmp: a copy of the last present_rgba picture (RGBA,
 * tightly packed, row 0 at the top) and the shape it was shown at. */
static pthread_mutex_t s_last_mu = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *s_last;
static size_t s_last_cap;
static int s_last_w, s_last_h, s_last_anum, s_last_aden;
static host_window_last_fn s_last_fn; /* host_window_present_rgba_shared: the caller has the picture */
static void *s_last_user;

static int env_set(const char *name)
{
    const char *v = getenv(name);
    return v && v[0];
}

int host_window_headless(void)
{
    const char *drv = getenv("SDL_VIDEODRIVER");

    if (env_set("XBR_BUILD_ONLY"))
        return 1;
    if (drv && (!strcmp(drv, "dummy") || !strcmp(drv, "offscreen")))
        return 1;
#if defined(__linux__) && !defined(__ANDROID__)
    if (!env_set("DISPLAY") && !env_set("WAYLAND_DISPLAY"))
        return 1;
#endif
    return 0;
}

static int load_gl(void)
{
    struct { void **slot; const char *name; } fns[] = {
#define F(n) { (void **)&s_gl.n, "gl" #n }
        F(GenTextures), F(DeleteTextures), F(BindTexture), F(TexParameteri), F(TexImage2D), F(TexSubImage2D),
        F(PixelStorei), F(ReadPixels), F(Viewport), F(ClearColor), F(Clear), F(Disable), F(CreateShader),
        F(ShaderSource),
        F(CompileShader), F(GetShaderiv), F(GetShaderInfoLog), F(DeleteShader), F(CreateProgram),
        F(AttachShader), F(LinkProgram), F(GetProgramiv), F(GetProgramInfoLog), F(DeleteProgram),
        F(UseProgram), F(GetUniformLocation), F(Uniform1i), F(Uniform1f), F(Uniform2f), F(GenVertexArrays),
        F(DeleteVertexArrays), F(BindVertexArray), F(BindFramebuffer), F(ActiveTexture), F(DrawArrays),
        F(GetError),
#undef F
    };
    for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++) {
        *fns[i].slot = SDL_GL_GetProcAddress(fns[i].name);
        if (!*fns[i].slot) {
            fprintf(stderr, "  [HOST] window: no %s in the GL context\n", fns[i].name);
            return 0;
        }
    }
    return 1;
}

static unsigned compile3(unsigned type, const char *head, const char *common, const char *body)
{
    const char *src[3] = { head, common, body };
    unsigned sh = s_gl.CreateShader(type);
    int ok = 0;

    s_gl.ShaderSource(sh, 3, src, NULL);
    s_gl.CompileShader(sh);
    s_gl.GetShaderiv(sh, XGL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        s_gl.GetShaderInfoLog(sh, (int)sizeof log, NULL, log);
        fprintf(stderr, "  [HOST] window: shader: %s\n", log);
        s_gl.DeleteShader(sh);
        return 0;
    }
    return sh;
}

static unsigned compile(unsigned type, const char *head, const char *body)
{
    return compile3(type, head, "", body);
}

/* One triangle over the viewport, no vertex buffer (gl_VertexID 0..2). */
static const char k_vs[] =
    "out vec2 v_uv;\n"
    "uniform float u_flip;\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    v_uv = vec2(p.x, mix(p.y, 1.0 - p.y, u_flip));\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
static const char k_fs[] =
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "out vec4 o_color;\n"
    "void main() { o_color = vec4(texture(u_tex, v_uv).rgb, 1.0); }\n";

static int build_program(void)
{
    const char *head_vs = s_gles ? "#version 300 es\n" : "#version 150\n";
    const char *head_fs = s_gles ? "#version 300 es\nprecision mediump float;\n" : "#version 150\n";
    unsigned vs = compile(XGL_VERTEX_SHADER, head_vs, k_vs);
    unsigned fs = vs ? compile(XGL_FRAGMENT_SHADER, head_fs, k_fs) : 0;
    int ok = 0;

    if (!vs || !fs) {
        if (vs)
            s_gl.DeleteShader(vs);
        return 0;
    }
    s_prog = s_gl.CreateProgram();
    s_gl.AttachShader(s_prog, vs);
    s_gl.AttachShader(s_prog, fs);
    s_gl.LinkProgram(s_prog);
    s_gl.DeleteShader(vs);
    s_gl.DeleteShader(fs);
    s_gl.GetProgramiv(s_prog, XGL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        s_gl.GetProgramInfoLog(s_prog, (int)sizeof log, NULL, log);
        fprintf(stderr, "  [HOST] window: program: %s\n", log);
        s_gl.DeleteProgram(s_prog);
        s_prog = 0;
        return 0;
    }
    s_u_tex = s_gl.GetUniformLocation(s_prog, "u_tex");
    s_u_flip = s_gl.GetUniformLocation(s_prog, "u_flip");
    s_gl.GenVertexArrays(1, &s_vao);
    return 1;
}

/* ── Picture shaders (XBA-195) ──────────────────────────────────────────
 * The presets, their GLSL and uniforms are present_shaders.c's (shared with the Xbox presenter, XBA-203): GLSL ES
 * 3.00 / 1.50 here, over the same full-viewport triangle as the plain present. Built on first use; a failure falls
 * back to none, logged once. */
static HostShader s_shader = { HOST_SHADER_NONE, -1, 0, 0 };
static PresentShaderProg s_sprog[HOST_SHADER_COUNT];

/* The program of the preset to draw with, built on first use; NULL for none or a preset that failed. */
static PresentShaderProg *shader_prog(void)
{
    int p = s_shader.preset;

    if (p <= HOST_SHADER_NONE || p >= HOST_SHADER_COUNT)
        return NULL;
    if (!present_shader_build(&s_sprog[p], s_gles ? PRESENT_GLSL_ES300 : PRESENT_GLSL_150, p, "[HOST] window:"))
        return NULL;
    return &s_sprog[p];
}

static void shader_free(void)
{
    for (int i = 0; i < HOST_SHADER_COUNT; i++)
        present_shader_release(&s_sprog[i], 0);
}

/* ── Live picture changes (the in-game menu) ─────────────────────────────
 * host_window_request_picture may come from any thread (Android's UI thread); the presenting thread takes the
 * request at its next present (live_take), so scale, filter and shader only ever change between two frames. */
static pthread_mutex_t s_live_mu = PTHREAD_MUTEX_INITIALIZER;
static int s_live_pending;               /* atomic: a request waits */
static int s_live_scale = -1, s_live_filter = -1, s_live_has_shader;
static HostShader s_live_shader;

void host_window_request_picture(int scale, int filter, const HostShader *s)
{
    pthread_mutex_lock(&s_live_mu);
    s_live_scale = scale;
    s_live_filter = filter;
    s_live_has_shader = s != NULL;
    if (s)
        s_live_shader = *s;
    pthread_mutex_unlock(&s_live_mu);
    __atomic_store_n(&s_live_pending, 1, __ATOMIC_RELEASE);
}

static void live_take(void)
{
    int scale, filter, has;
    HostShader sh;

    if (!__atomic_load_n(&s_live_pending, __ATOMIC_ACQUIRE))
        return;
    pthread_mutex_lock(&s_live_mu);
    __atomic_store_n(&s_live_pending, 0, __ATOMIC_RELAXED);
    scale = s_live_scale;
    filter = s_live_filter;
    has = s_live_has_shader;
    sh = s_live_shader;
    pthread_mutex_unlock(&s_live_mu);
    host_window_set_scaling(scale < 0 ? s_scale : scale, filter < 0 ? s_filter : filter);
    if (has)
        host_window_set_shader(&sh);
    fprintf(stderr, "  [HOST] window: picture changed live: scale %d, filter %d, shader %s\n", s_scale, s_filter,
            host_window_shader_name(s_shader.preset));
}

/* LCD ghosting wants the previous picture: present_rgba then alternates between two textures. */
static int shader_wants_prev(void)
{
    return s_shader.preset == HOST_SHADER_LCD && s_shader.ghosting > 0 && !s_sprog[HOST_SHADER_LCD].failed;
}

/* The app going to the background and back: on Android these must be seen
 * as they happen, from the event watch, not at the next pump. */
static int SDLCALL lifecycle_watch(void *user, SDL_Event *ev)
{
    (void)user;
    if (ev->type == SDL_APP_WILLENTERBACKGROUND)
        recomp_lifecycle_pause();
    else if (ev->type == SDL_APP_DIDENTERFOREGROUND)
        recomp_lifecycle_resume();
    return 1;
}

static int try_context(const char *title, int w, int h, Uint32 flags, int gles, int major, int minor)
{
    SDL_GL_ResetAttributes();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        gles ? SDL_GL_CONTEXT_PROFILE_ES : SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
    if (!gles)
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

    s_win = SDL_CreateWindow(title ? title : "xrecomp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             w, h, flags);
    if (!s_win)
        return 0;
    s_ctx = SDL_GL_CreateContext(s_win);
    if (!s_ctx) {
        SDL_DestroyWindow(s_win);
        s_win = NULL;
        return 0;
    }
    s_gles = gles;
    s_major = major;
    s_minor = minor;
    return 1;
}

int host_window_open(const char *title, int width, int height, int vsync)
{
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;

    if (s_win)
        return 1;
    if (width <= 0 || height <= 0) {
        width = 1280;
        height = 720;
    }
#ifdef __ANDROID__
    flags |= SDL_WINDOW_FULLSCREEN;
#endif
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "  [HOST] window: SDL video: %s\n", SDL_GetError());
        return 0;
    }
    int ok = 0;
#if !defined(__APPLE__)
    /* RECOMP_HOST_GL=core skips GLES (a check of the macOS path on Linux). */
    const char *want = getenv("RECOMP_HOST_GL");
    if (!want || strcmp(want, "core") != 0)
        ok = try_context(title, width, height, flags, 1, 3, 0);   /* GLES 3.0: Android, Mesa */
#endif
    if (!ok)
        ok = try_context(title, width, height, flags, 0, 3, 2);   /* 3.2 core: macOS, desktop drivers */
    if (!ok) {
        fprintf(stderr, "  [HOST] window: no GL ES 3.0 or 3.2 core context: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
        return 0;
    }
    if (!load_gl() || !build_program()) {
        host_window_close();
        return 0;
    }
    s_vsync = 0;
    if (vsync) {
        if (SDL_GL_SetSwapInterval(-1) == 0 || SDL_GL_SetSwapInterval(1) == 0)
            s_vsync = 1;
    } else {
        SDL_GL_SetSwapInterval(0);
    }
    SDL_AddEventWatch(lifecycle_watch, NULL);
    host_pad_init();
    s_quit = 0;
    s_frames = 0;
    s_fps_t0 = s_fps_n = 0;
    __atomic_store_n(&s_frames_pub, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_fps_centi, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_last_present_ns, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_open, 1, __ATOMIC_RELEASE);
    fprintf(stderr, "  [HOST] window: %dx%d, GL%s %d.%d, vsync %s\n", width, height, s_gles ? " ES" : " core",
            s_major, s_minor, s_vsync ? "on" : "off");
#if defined(XBOXRECOMP_LSFG)
    fg_open();
#endif
    return 1;
}

void host_window_set_event_hook(host_window_event_fn fn, void *user)
{
    s_hook = fn;
    s_hook_user = user;
}

int host_window_pump(void)
{
    SDL_Event ev;

    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            s_quit = 1;
            break;
        case SDL_WINDOWEVENT:
            if (ev.window.event == SDL_WINDOWEVENT_CLOSE)
                s_quit = 1;
            break;
        default:
            break;
        }
        host_pad_handle_event(&ev);
        if (s_hook)
            s_hook(&ev, s_hook_user);
    }
    return !s_quit;
}

/* host_window_capture: the drawable as drawn, before the swap, as a binary
 * PPM (row 0 at the top). Once per request. */
static void capture(int dw, int dh)
{
    unsigned char *px = malloc((size_t)dw * dh * 4);
    FILE *f = px ? fopen(s_capture, "wb") : NULL;

    if (f) {
        s_gl.PixelStorei(XGL_PACK_ALIGNMENT, 4);
        s_gl.ReadPixels(0, 0, dw, dh, XGL_RGBA, XGL_UNSIGNED_BYTE, px);
        fprintf(f, "P6\n%d %d\n255\n", dw, dh);
        for (int y = dh - 1; y >= 0; y--)
            for (int x = 0; x < dw; x++)
                fwrite(px + ((size_t)y * dw + x) * 4, 1, 3, f);
        fprintf(stderr, "  [HOST] window: %dx%d drawable captured to %s\n", dw, dh, s_capture);
    }
    if (!f || fclose(f) != 0)
        fprintf(stderr, "  [HOST] window: cannot write the capture %s\n", s_capture);
    free(px);
    s_capture[0] = 0;
}

/* host_window_stats: one more present; the rate is taken over a second. */
static void count_frame(void)
{
    uint64_t now = xbox_HostNowNs();

    __atomic_store_n(&s_frames_pub, s_frames, __ATOMIC_RELAXED);
    __atomic_store_n(&s_last_present_ns, now, __ATOMIC_RELAXED);
    if (!s_fps_t0 || now - s_fps_t0 > 4000000000ull) {   /* first frame, or back from a long hold */
        s_fps_t0 = now;
        s_fps_n = 0;
        return;
    }
    s_fps_n++;
    if (now - s_fps_t0 >= 1000000000ull) {
        __atomic_store_n(&s_fps_centi, (uint32_t)(s_fps_n * 100000000000ull / (now - s_fps_t0)), __ATOMIC_RELAXED);
        s_fps_t0 = now;
        s_fps_n = 0;
    }
}

/* host_window_screenshot_bmp: keeps the picture just shown. */
static void keep_last(const uint8_t *px, int w, int h, int pitch)
{
    if (w <= 0 || h <= 0)
        return;
    size_t need = (size_t)w * h * 4;

    pthread_mutex_lock(&s_last_mu);
    if (need > s_last_cap) {
        uint8_t *n = realloc(s_last, need);
        if (!n) {
            pthread_mutex_unlock(&s_last_mu);
            return;
        }
        s_last = n;
        s_last_cap = need;
    }
    for (int y = 0; y < h; y++)
        memcpy(s_last + (size_t)y * w * 4, px + (size_t)y * pitch, (size_t)w * 4);
    s_last_w = w;
    s_last_h = h;
    s_last_anum = s_aspect_num;
    s_last_aden = s_aspect_den;
    s_last_fn = NULL;
    pthread_mutex_unlock(&s_last_mu);
}

/* host_window_present_rgba_shared: no copy, the caller's `last` gives the picture on demand. */
static void keep_shared(host_window_last_fn fn, void *user, int w, int h)
{
    pthread_mutex_lock(&s_last_mu);
    s_last_fn = fn;
    s_last_user = user;
    s_last_w = w;
    s_last_h = h;
    s_last_anum = s_aspect_num;
    s_last_aden = s_aspect_den;
    pthread_mutex_unlock(&s_last_mu);
}

static int draw(unsigned tex, unsigned prev, int w, int h, int top_down)
{
    int dw = 0, dh = 0, vw, vh;

    if (!s_win || !tex || w <= 0 || h <= 0)
        return 0;
    SDL_GL_GetDrawableSize(s_win, &dw, &dh);
    if (dw <= 0 || dh <= 0)
        return 0;
    /* Letterbox: the largest rectangle of the picture's aspect, or of a
     * whole multiple of its size (HOST_SCALE_INTEGER, when 1x fits). */
    int k = dw / w < dh / h ? dw / w : dh / h;
    /* The shape shown: the picture's own, or host_window_set_aspect's. */
    int64_t aw = w, ah = h;
    if (s_aspect_num > 0 && s_aspect_den > 0) {
        aw = s_aspect_num;
        ah = s_aspect_den;
        k = dh / h;
        if (k >= 1 && (int64_t)h * k * aw / ah > dw)
            k = 0; /* that height is too wide for the window */
    }
    if (s_scale == HOST_SCALE_STRETCH) {
        vw = dw;
        vh = dh;
    } else if (s_scale == HOST_SCALE_INTEGER && k >= 1) {
        vh = h * k;
        vw = s_aspect_num > 0 && s_aspect_den > 0 ? (int)((int64_t)vh * aw / ah) : w * k;
    } else if ((int64_t)dw * ah > (int64_t)dh * aw) {
        vh = dh;
        vw = (int)((int64_t)dh * aw / ah);
    } else {
        vw = dw;
        vh = (int)((int64_t)dw * ah / aw);
    }
    s_gl.BindFramebuffer(XGL_FRAMEBUFFER, 0);
    s_gl.Disable(XGL_SCISSOR_TEST);
    s_gl.Disable(XGL_BLEND);
    s_gl.Disable(XGL_DEPTH_TEST);
    s_gl.Viewport(0, 0, dw, dh);
    s_gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    s_gl.Clear(XGL_COLOR_BUFFER_BIT);
    s_gl.Viewport((dw - vw) / 2, (dh - vh) / 2, vw, vh);
    PresentShaderProg *sp = shader_prog();
    if (sp) {
        /* Console lines: an upscaled picture (XBA-184) holds round(h / 240) output lines per console line. */
        int cells_h = present_shader_console_cells(h);
        PresentShaderParams pp = {
            .src_w = (float)w, .src_h = (float)h, .out_w = (float)vw, .out_h = (float)vh,
            .cells_w = (float)w * (float)cells_h / (float)h, .cells_h = (float)cells_h,
            .flip = top_down, .strength = present_shader_strength(&s_shader), .curvature = s_shader.curvature,
            .ghosting = prev && prev != tex ? s_shader.ghosting : 0,
        };
        if (prev && prev != tex) {
            s_gl.ActiveTexture(XGL_TEXTURE1);
            s_gl.BindTexture(XGL_TEXTURE_2D, prev);
        }
        s_gl.ActiveTexture(XGL_TEXTURE0);
        s_gl.BindTexture(XGL_TEXTURE_2D, tex);
        present_shader_use(sp, &pp);
    } else {
        s_gl.UseProgram(s_prog);
        s_gl.ActiveTexture(XGL_TEXTURE0);
        s_gl.BindTexture(XGL_TEXTURE_2D, tex);
        s_gl.Uniform1i(s_u_tex, 0);
        s_gl.Uniform1f(s_u_flip, top_down ? 1.0f : 0.0f);
    }
    s_gl.BindVertexArray(s_vao);
    s_gl.DrawArrays(XGL_TRIANGLES, 0, 3);
    s_gl.BindVertexArray(0);
    unsigned err = s_gl.GetError();
    if (s_capture[0] && !s_drawing_generated)
        capture(dw, dh);
    SDL_GL_SwapWindow(s_win);
    s_frames++;
    count_frame();
    if (err) {
        fprintf(stderr, "  [HOST] window: GL error 0x%04x at present\n", err);
        return 0;
    }
    return 1;
}

int host_window_present_texture(unsigned tex, int w, int h, int top_down)
{
    live_take();
    return draw(tex, 0, w, h, top_down);
}

/* The last upload_draw's texture pair and size, for host_window_present_again. */
static unsigned s_again_tex, s_again_prev;
static int s_again_w, s_again_h;

/* Uploads RGBA pixels into the window's texture and draws it. */
static int upload_draw(const void *px, int w, int h, int pitch)
{
    int ghost = shader_wants_prev();
    /* Sharp bilinear blends texels itself: it needs linear sampling whatever the filter. */
    int filter = s_shader.preset == HOST_SHADER_SHARP && !s_sprog[HOST_SHADER_SHARP].failed ? HOST_FILTER_LINEAR
                                                                                              : s_filter;

    if (ghost) {
        /* The texture of the previous picture takes this one; the last one becomes the previous. */
        unsigned t = s_tex;
        int tw = s_tex_w, th = s_tex_h, tf = s_tex_filter;
        s_tex = s_tex_prev;
        s_tex_w = s_tex_prev_w;
        s_tex_h = s_tex_prev_h;
        s_tex_filter = s_tex_prev_filter;
        s_tex_prev = t;
        s_tex_prev_w = tw;
        s_tex_prev_h = th;
        s_tex_prev_filter = tf;
    }
    if (!s_tex) {
        s_gl.GenTextures(1, &s_tex);
        s_tex_w = s_tex_h = 0;
        s_tex_filter = -1;
    }
    s_gl.ActiveTexture(XGL_TEXTURE0);
    s_gl.BindTexture(XGL_TEXTURE_2D, s_tex);
    s_gl.PixelStorei(XGL_UNPACK_ALIGNMENT, 4);
    s_gl.PixelStorei(XGL_UNPACK_ROW_LENGTH, pitch / 4);
    if (s_tex_filter != filter) {
        int f = filter == HOST_FILTER_NEAREST ? XGL_NEAREST : XGL_LINEAR;
        s_gl.TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_MIN_FILTER, f);
        s_gl.TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_MAG_FILTER, f);
        s_tex_filter = filter;
    }
    if (w != s_tex_w || h != s_tex_h) {
        s_gl.TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_WRAP_S, XGL_CLAMP_TO_EDGE);
        s_gl.TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_WRAP_T, XGL_CLAMP_TO_EDGE);
        s_gl.TexImage2D(XGL_TEXTURE_2D, 0, (int)XGL_RGBA8, w, h, 0, XGL_RGBA, XGL_UNSIGNED_BYTE, px);
        s_tex_w = w;
        s_tex_h = h;
    } else {
        s_gl.TexSubImage2D(XGL_TEXTURE_2D, 0, 0, 0, w, h, XGL_RGBA, XGL_UNSIGNED_BYTE, px);
    }
    s_gl.PixelStorei(XGL_UNPACK_ROW_LENGTH, 0);
    s_again_tex = s_tex;
    s_again_prev = ghost && s_tex_prev_w == w && s_tex_prev_h == h ? s_tex_prev : 0;
    s_again_w = w;
    s_again_h = h;
    return draw(s_again_tex, s_again_prev, w, h, 1);
}

#if defined(XBOXRECOMP_LSFG)
/* ── Frame generation (XBA-186) ─────────────────────────────────────────
 * present_rgba hands each picture to framegen_console (distinct frames to the pacer, repeats dropped) and draws
 * what its timed FIFO has due; host_window_sleep_until keeps drawing FIFO entries at their due times while the
 * runtime waits for its next frame. Fast-forward and a fallen-back pacer draw directly; a state load, the app
 * pausing or resuming and the in-game menu reset the history. */
static int s_fg;                 /* framegen_console running for this window */
static int s_fg_off_logged;      /* the pacer fell back: logged once */
static unsigned s_fg_loads;      /* recomp_states_loads() at the last frame */
static uint64_t s_fg_log_at;     /* the next statistics line */
static uint64_t s_fg_drawn, s_fg_drawn_gen;

static void fg_lifecycle(int paused, void *user)
{
    (void)paused;
    (void)user;
    fgc_reset();
}

static void fg_log(const char *when)
{
    uint32_t real = 0, shown = 0, gen_us = 0;
    FgcCounters c;
    FramegenPacerCounters p;

    framegen_pacer_stats(&real, &shown, &gen_us);
    fgc_counters(&c);
    framegen_pacer_counters(&p);
    fprintf(stderr,
            "[LSFG] console%s: real %.2f / shown %.2f fps, generation %.2f ms; pictures %llu (distinct %llu, repeats "
            "%llu, direct %llu, downscaled %llu), generated %llu, window drew %llu (%llu generated), late %llu, FIFO "
            "full %llu, skipped pairs %llu, resets %llu%s\n",
            when, real / 100.0, shown / 100.0, gen_us / 1000.0, (unsigned long long)c.frames,
            (unsigned long long)c.distinct, (unsigned long long)c.repeats, (unsigned long long)c.direct,
            (unsigned long long)c.downscaled, (unsigned long long)p.generated, (unsigned long long)s_fg_drawn,
            (unsigned long long)s_fg_drawn_gen, (unsigned long long)c.late, (unsigned long long)c.fifo_full,
            (unsigned long long)p.skipped, (unsigned long long)c.resets, c.passthrough ? ", pass-through" : "");
}

static void fg_open(void)
{
    static int listening;
    SDL_DisplayMode mode;
    int hz = 0, idx;

    s_fg = 0;
    if (!framegen_policy_enabled())
        return;
#if defined(XBOXRECOMP_LSFG_BRIDGE)
    lsfg_bridge_register();
#endif
    idx = SDL_GetWindowDisplayIndex(s_win);
    if (idx >= 0 && SDL_GetCurrentDisplayMode(idx, &mode) == 0)
        hz = mode.refresh_rate;
    if (fgc_start_env(hz) < 0)
        return;
    s_fg = 1;
    s_fg_off_logged = 0;
    s_fg_loads = recomp_states_loads();
    s_fg_log_at = xbox_HostNowNs() + 5000000000ull;
    s_fg_drawn = s_fg_drawn_gen = 0;
    if (!listening)
        listening = recomp_lifecycle_listen(fg_lifecycle, NULL) == 0;
}

static void fg_close(void)
{
    if (!s_fg)
        return;
    fg_log(" at close");
    fgc_stop();
    s_fg = 0;
}

/* Draws the newest FIFO entry due at now, if any. */
static void fg_service(uint64_t now)
{
    FgcShown e;

    if (now >= s_fg_log_at) {
        s_fg_log_at = now + 5000000000ull;
        fg_log("");
    }
    if (!fgc_take(now, &e))
        return;
    s_drawing_generated = e.generated;
    upload_draw(e.px, e.w, e.h, e.w * 4);
    s_drawing_generated = 0;
    s_fg_drawn++;
    s_fg_drawn_gen += (uint64_t)e.generated;
}

/* 1 when the picture was taken by frame generation (nothing to draw now), 0 to draw it as usual. */
static int fg_present(const void *px, int w, int h, int pitch)
{
    unsigned flags = 0, loads;
    uint64_t now = xbox_HostNowNs();

    if (!s_fg)
        return 0;
    if (recomp_ff_active())
        flags |= FGC_BYPASS;
    loads = recomp_states_loads();
    if (loads != s_fg_loads) {
        s_fg_loads = loads;
        flags |= FGC_RESET;
    }
    int r = fgc_frame(px, w, h, pitch, now, flags);
    if (!framegen_pacer_active() && !s_fg_off_logged) {
        s_fg_off_logged = 1;
        fprintf(stderr, "  [HOST] window: frame generation off (%s); normal present\n", framegen_pacer_off_reason());
    }
    if (r == FGC_DIRECT)
        return 0;
    fg_service(now);
    return 1;
}
#endif /* XBOXRECOMP_LSFG */

int host_window_present_rgba(const void *px, int w, int h, int pitch)
{
    if (!s_win || !px || w <= 0 || h <= 0 || pitch < w * 4)
        return 0;
    keep_last(px, w, h, pitch);   /* the real picture, for host_window_screenshot_bmp */
    live_take();
#if defined(XBOXRECOMP_LSFG)
    if (fg_present(px, w, h, pitch))
        return 1;
#endif
    return upload_draw(px, w, h, pitch);
}

int host_window_present_rgba_shared(const void *px, int w, int h, int pitch, host_window_last_fn last, void *user)
{
    if (!s_win || !px || w <= 0 || h <= 0 || pitch < w * 4)
        return 0;
    if (last)
        keep_shared(last, user, w, h);
    else
        keep_last(px, w, h, pitch);
#if defined(XBOXRECOMP_LSFG)
    if (fg_present(px, w, h, pitch))
        return 1;
#endif
    return upload_draw(px, w, h, pitch);
}

/* The shared picture as a kept copy (the caller's pixels are about to go). */
void host_window_keep_shared(void)
{
    pthread_mutex_lock(&s_last_mu);
    host_window_last_fn fn = s_last_fn;
    void *user = s_last_user;
    int anum = s_last_anum, aden = s_last_aden;
    s_last_fn = NULL;
    pthread_mutex_unlock(&s_last_mu);
    uint8_t *px = NULL;
    int w = 0, h = 0;
    if (!fn || fn(user, &px, &w, &h) != 0 || !px) {
        free(px);
        return;
    }
    int keep_num = s_aspect_num, keep_den = s_aspect_den;
    s_aspect_num = anum; /* the shape it was shown at */
    s_aspect_den = aden;
    keep_last(px, w, h, w * 4);
    s_aspect_num = keep_num;
    s_aspect_den = keep_den;
    free(px);
}

int host_window_present_again(void)
{
    if (!s_win || !s_again_tex)
        return 0;
    live_take();
#if defined(XBOXRECOMP_LSFG)
    if (s_fg && framegen_pacer_active() && !recomp_ff_active())
        return 1; /* the pacer draws its FIFO at its due times (host_window_sleep_until): nothing new to give it */
#endif
    return draw(s_again_tex, s_again_prev, s_again_w, s_again_h, 1);
}

void host_window_sleep_until(uint64_t deadline_ns)
{
    uint64_t now;

#if defined(XBOXRECOMP_LSFG)
    if (s_fg && s_win) {
        for (;;) {
            now = xbox_HostNowNs();
            fg_service(now);
            now = xbox_HostNowNs();
            if (now >= deadline_ns)
                return;
            uint64_t due = fgc_next_due(), until = deadline_ns;
            if (due && due < until)
                until = due > now + 1000000ull ? due - 1000000ull : now; /* within a millisecond counts as due */
            if (until > now)
                fgc_wait(until);
        }
    }
#endif
    while ((now = xbox_HostNowNs()) < deadline_ns) {
        uint64_t d = deadline_ns - now;
        struct timespec ts = {(time_t)(d / 1000000000ull), (long)(d % 1000000000ull)};
        nanosleep(&ts, NULL);
    }
}

int host_window_framegen_active(void)
{
#if defined(XBOXRECOMP_LSFG)
    return s_fg && framegen_pacer_active();
#else
    return 0;
#endif
}

void host_window_set_scaling(int scale, int filter)
{
    s_scale = scale == HOST_SCALE_INTEGER || scale == HOST_SCALE_STRETCH ? scale : HOST_SCALE_ASPECT;
    s_filter = filter == HOST_FILTER_NEAREST ? HOST_FILTER_NEAREST : HOST_FILTER_LINEAR;
}

void host_window_set_aspect(int num, int den)
{
    s_aspect_num = num > 0 && den > 0 ? num : 0;
    s_aspect_den = num > 0 && den > 0 ? den : 0;
}

int host_window_scale_parse(const char *v)
{
    if (!v)
        return -1;
    if (!strcasecmp(v, "aspect"))
        return HOST_SCALE_ASPECT;
    if (!strcasecmp(v, "integer"))
        return HOST_SCALE_INTEGER;
    if (!strcasecmp(v, "stretch"))
        return HOST_SCALE_STRETCH;
    return -1;
}

int host_window_filter_parse(const char *v)
{
    if (!v)
        return -1;
    if (!strcasecmp(v, "nearest"))
        return HOST_FILTER_NEAREST;
    if (!strcasecmp(v, "linear"))
        return HOST_FILTER_LINEAR;
    return -1;
}

int host_window_env_scale(const char *console_var, int def)
{
    int v = console_var ? host_window_scale_parse(getenv(console_var)) : -1;

    if (v < 0)
        v = host_window_scale_parse(getenv("RECOMP_SCALE"));
    return v < 0 ? def : v;
}

int host_window_env_filter(const char *console_var, int def)
{
    int v = console_var ? host_window_filter_parse(getenv(console_var)) : -1;

    if (v < 0)
        v = host_window_filter_parse(getenv("RECOMP_FILTER"));
    return v < 0 ? def : v;
}

int host_window_env_vsync(void)
{
    const char *v = getenv("RECOMP_PRESENT_VSYNC");

    return !(v && v[0] == '0');
}

void host_window_set_shader(const HostShader *s)
{
    s_shader = present_shader_clamp(s);
}

int host_window_shader_active(void)
{
    int p = s_shader.preset;

    return p > HOST_SHADER_NONE && p < HOST_SHADER_COUNT && !s_sprog[p].failed ? p : HOST_SHADER_NONE;
}

int host_window_shader_parse(const char *v)
{
    return present_shader_parse(v);
}

const char *host_window_shader_name(int preset)
{
    return present_shader_name(preset);
}

void host_window_env_shader(const char *console_var, int def, HostShader *out)
{
    present_shader_env(console_var, def, out);
}

void host_window_shader_env(const char *console_var)
{
    HostShader s;

    host_window_env_shader(console_var, HOST_SHADER_NONE, &s);
    host_window_set_shader(&s);
    if (s.preset != HOST_SHADER_NONE)
        fprintf(stderr, "  [HOST] window: picture shader %s requested (strength %d, curvature %d, ghosting %d)\n",
                host_window_shader_name(s.preset), s.strength, s.curvature, s.ghosting);
}

int host_window_stats(uint32_t *fps_centi, uint64_t *frames)
{
    uint32_t fps = 0;
    uint64_t n = 0;
    int open = __atomic_load_n(&s_open, __ATOMIC_ACQUIRE);

    if (open) {
        uint64_t last = __atomic_load_n(&s_last_present_ns, __ATOMIC_RELAXED);
        n = __atomic_load_n(&s_frames_pub, __ATOMIC_RELAXED);
        if (last && xbox_HostNowNs() - last < 2000000000ull)
            fps = __atomic_load_n(&s_fps_centi, __ATOMIC_RELAXED);
    }
    if (fps_centi)
        *fps_centi = fps;
    if (frames)
        *frames = n;
    return open;
}

int host_window_screenshot_bmp(const char *path)
{
    uint8_t *px = NULL, hdr[54], *line;
    int w, h, anum, aden;

    if (!path || !path[0])
        return -1;
    pthread_mutex_lock(&s_last_mu);
    host_window_last_fn fn = s_last_fn;
    void *user = s_last_user;
    w = s_last_w;
    h = s_last_h;
    anum = s_last_anum;
    aden = s_last_aden;
    if (!fn && s_last && w > 0 && h > 0 && (px = malloc((size_t)w * h * 4)))
        memcpy(px, s_last, (size_t)w * h * 4);
    pthread_mutex_unlock(&s_last_mu);
    if (fn && fn(user, &px, &w, &h) != 0) { /* the caller's picture (host_window_present_rgba_shared) */
        free(px);
        px = NULL;
    }
    if (!px || w <= 0 || h <= 0) {
        free(px);
        return -1;
    }

    /* The shape it was shown at: the height kept and the width following, or the width kept when that would
     * make it narrower than it is. */
    int ow = w, oh = h;
    if (anum > 0 && aden > 0) {
        ow = (int)(((int64_t)h * anum + aden / 2) / aden);
        if (ow < w) {
            ow = w;
            oh = (int)(((int64_t)w * aden + anum / 2) / anum);
        }
    }
    uint32_t row = ((uint32_t)ow * 3u + 3u) & ~3u, img = row * (uint32_t)oh, total = 54u + img;
    uint32_t uw = (uint32_t)ow, uh = (uint32_t)oh;
    FILE *f = fopen(path, "wb");
    line = calloc(1, row);
    if (!f || !line) {
        if (f)
            fclose(f);
        free(line);
        free(px);
        return -1;
    }
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B';
    hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &uw, 4);
    memcpy(hdr + 22, &uh, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    int ok = fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr;
    for (int y = oh - 1; ok && y >= 0; y--) {   /* bottom-up */
        const uint8_t *src = px + (size_t)((int64_t)y * h / oh) * w * 4;
        for (int x = 0; x < ow; x++) {
            const uint8_t *p = src + (size_t)((int64_t)x * w / ow) * 4;
            line[x * 3 + 0] = p[2];
            line[x * 3 + 1] = p[1];
            line[x * 3 + 2] = p[0];
        }
        ok = fwrite(line, 1, row, f) == row;
    }
    free(line);
    free(px);
    if (fclose(f) != 0 || !ok)
        return -1;
    fprintf(stderr, "  [HOST] window: screenshot %dx%d (from %dx%d) to %s\n", ow, oh, w, h, path);
    return 0;
}

void host_window_capture(const char *ppm_path)
{
    snprintf(s_capture, sizeof s_capture, "%s", ppm_path ? ppm_path : "");
}

void *host_window_gl_proc(const char *name)
{
    return s_ctx ? SDL_GL_GetProcAddress(name) : NULL;
}

void host_window_info(HostWindowInfo *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof *out);
    if (s_win)
        SDL_GL_GetDrawableSize(s_win, &out->drawable_w, &out->drawable_h);
    out->gles = s_gles;
    out->gl_major = s_major;
    out->gl_minor = s_minor;
    out->vsync = s_vsync;
    out->frames = s_frames;
}

void *host_window_sdl(void)
{
    return s_win;
}

void host_window_close(void)
{
    if (!s_win)
        return;
#if defined(XBOXRECOMP_LSFG)
    fg_close(); /* the pacer's thread stops (and the bridge closes) before the GL context goes */
#endif
    __atomic_store_n(&s_open, 0, __ATOMIC_RELEASE);
    pthread_mutex_lock(&s_last_mu);
    free(s_last);
    s_last = NULL;
    s_last_cap = 0;
    s_last_w = s_last_h = 0;
    s_last_fn = NULL;
    pthread_mutex_unlock(&s_last_mu);
    SDL_DelEventWatch(lifecycle_watch, NULL);
    host_pad_shutdown();
    if (s_ctx) {
        if (s_gl.DeleteTextures && s_tex)
            s_gl.DeleteTextures(1, &s_tex);
        if (s_gl.DeleteTextures && s_tex_prev)
            s_gl.DeleteTextures(1, &s_tex_prev);
        shader_free();
        if (s_gl.DeleteVertexArrays && s_vao)
            s_gl.DeleteVertexArrays(1, &s_vao);
        if (s_gl.DeleteProgram && s_prog)
            s_gl.DeleteProgram(s_prog);
        SDL_GL_DeleteContext(s_ctx);
    }
    SDL_DestroyWindow(s_win);
    s_win = NULL;
    s_ctx = NULL;
    s_tex = s_vao = s_prog = 0;
    s_tex_w = s_tex_h = 0;
    s_tex_filter = -1;
    s_tex_prev = 0;
    s_tex_prev_w = s_tex_prev_h = 0;
    s_again_tex = s_again_prev = 0;
    s_tex_prev_filter = -1;
    memset(s_sprog, 0, sizeof s_sprog);   /* a context that could not be made current: built again next time */
    memset(&s_gl, 0, sizeof s_gl);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
}
