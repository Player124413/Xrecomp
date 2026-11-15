/* host_window -- the host layer's window: one SDL window with a GL context
 * and a texture presented into it (XBA-145, src/platform/HOST_LAYER.md).
 *
 * Console-agnostic: a console runtime renders into a GL texture of this
 * context (or hands RGBA pixels) and presents it, letterboxed to its aspect.
 * The context is OpenGL ES 3.0 where the host has it (Android, Mesa) and a
 * 3.2 core context otherwise (macOS), current on the thread that opened the
 * window (RECOMP_HOST_GL=core skips GLES, to check the macOS path on Linux);
 * GL entry points come from SDL (host_window_gl_proc), so nothing
 * links libGL or libGLESv3 directly.
 *
 * The Xbox presenter (fb_present_sdl.c, an SDL_Renderer fed from guest
 * memory) is separate and unchanged; this is the window every other console
 * runtime uses.
 *
 * Not thread-safe: open, pump, present and close on the same thread (SDL's
 * main thread on Android and macOS). POSIX hosts. */
#ifndef XBR_HOST_WINDOW_H
#define XBR_HOST_WINDOW_H

#include <stdint.h>

#include "present_shaders.h"   /* HOST_SHADER_*, HostShader (shared with the Xbox presenter, XBA-203) */

#ifdef __cplusplus
extern "C" {
#endif

union SDL_Event;

typedef struct HostWindowInfo {
    int drawable_w, drawable_h;  /* the back buffer, in pixels */
    int gles;                    /* 1: OpenGL ES context, 0: desktop core */
    int gl_major, gl_minor;
    int vsync;                   /* 1 when the swap waits for vsync */
    uint64_t frames;             /* presents so far */
} HostWindowInfo;

/* 1 when no window should be opened: XBR_BUILD_ONLY set (a build check),
 * SDL_VIDEODRIVER dummy or offscreen, or a Linux desktop host with neither
 * DISPLAY nor WAYLAND_DISPLAY. Tests exit 77 (skipped) on it. */
int host_window_headless(void);

/* Opens the window (width x height, resizable; fullscreen on Android) and
 * its GL context. vsync: 1 waits for vsync at the swap (adaptive when the
 * driver has it). Returns 1, or 0 with the reason on stderr. A second open
 * while one is up returns 1. */
int host_window_open(const char *title, int width, int height, int vsync);

/* Handles the pending events: quit, resize, app background / foreground
 * (recomp_lifecycle_pause / resume) and pads (host_pad_handle_event), then
 * the hook, if any. Returns 0 once quit was asked (window closed, SDL_QUIT),
 * 1 otherwise. */
int host_window_pump(void);

/* Called by host_window_pump for every event, after the host layer's own
 * handling. NULL removes it. */
typedef void (*host_window_event_fn)(const union SDL_Event *ev, void *user);
void host_window_set_event_hook(host_window_event_fn fn, void *user);

/* Uploads RGBA8 pixels (row 0 at the top, pitch in bytes) into the
 * window's own texture and presents it. Returns 1, or 0 on a GL error. */
int host_window_present_rgba(const void *px, int w, int h, int pitch);

/* host_window_present_rgba for pixels the caller keeps (the N64 runtime: the Vulkan RDP's mapped scanout): no copy
 * is made for host_window_screenshot_bmp; it asks `last` instead, on its own thread, for the picture shown last: a
 * malloc'd copy, tightly packed RGBA rows, its size; 0, or -1 when there is none any more. The next
 * host_window_present_rgba (a copy kept again) or host_window_keep_shared (the caller's pixels are going: `last` is
 * asked now and the copy kept) ends it. */
typedef int (*host_window_last_fn)(void *user, uint8_t **px, int *w, int *h);
int host_window_present_rgba_shared(const void *px, int w, int h, int pitch, host_window_last_fn last, void *user);
void host_window_keep_shared(void);

/* Draws the picture the last host_window_present_rgba uploaded again (no upload, no copy kept for the screenshot):
 * for a frame whose picture did not change (the N64 runtime: a VI without a new picture). With frame generation
 * running it draws nothing (the pacer shows its FIFO at its own due times and wants distinct pictures only). Returns
 * 1, or 0 when nothing was presented yet or on a GL error. */
int host_window_present_again(void);

/* Frame generation (XBA-186, docs/technical/framegen.md « Console window »): in a build with XBOXRECOMP_LSFG,
 * under RECOMP_LSFG=1, host_window_open starts the pacer (the LSFG bridge when built in, the fake generator under
 * RECOMP_LSFG_FAKE=1; "[LSFG] off: ..." and the usual present when it cannot). host_window_present_rgba then hands
 * each picture to it, distinct pictures only (a 30 fps game presented at 60 Hz shows each one twice), and draws
 * the real and generated frames of its timed FIFO at their due times, during the next presents and during
 * host_window_sleep_until. Fast-forward (recomp_ff_active) draws directly; a state load (recomp_states_loads), the
 * app pausing or resuming and the in-game menu start a fresh history; host_window_screenshot_bmp keeps the last
 * real picture, host_window_capture never captures a generated one. Not used by host_window_present_texture. */

/* Sleeps until CLOCK_MONOTONIC deadline_ns (xbox_HostNowNs), drawing the frame generation FIFO's entries at their
 * due times meanwhile; a plain sleep without frame generation. The runtimes' frame pacing sleeps through it. */
void host_window_sleep_until(uint64_t deadline_ns);

/* 1 while frame generation runs for the window (started and not fallen back). */
int host_window_framegen_active(void);

/* Presents texture `tex` of this context (w x h, its aspect kept). top_down:
 * 1 when its row 0 is the top of the picture (uploaded pixels), 0 when it is
 * the bottom (a texture rendered by GL). Binds framebuffer 0, draws, swaps.
 * Returns 1, or 0 on a GL error. */
int host_window_present_texture(unsigned tex, int w, int h, int top_down);

/* How host_window_present_rgba / _texture fit the picture in the window.
 * HOST_SCALE_ASPECT (the default): the largest rectangle of its aspect;
 * HOST_SCALE_INTEGER: the largest whole multiple of its size that fits (a
 * pixel-art console: every source pixel the same size), aspect when not even
 * 1x fits; HOST_SCALE_STRETCH: the whole window, aspect not kept. filter:
 * how the window's own texture (present_rgba) is sampled, HOST_FILTER_LINEAR
 * (the default) or HOST_FILTER_NEAREST (sharp pixels). May be called before
 * open; kept across open / close. */
enum { HOST_SCALE_ASPECT = 0, HOST_SCALE_INTEGER = 1, HOST_SCALE_STRETCH = 2 };
enum { HOST_FILTER_LINEAR = 0, HOST_FILTER_NEAREST = 1 };
void host_window_set_scaling(int scale, int filter);

/* The shared display options (XBA-181, docs/runtime/environment-variables.md),
 * read the same way by every console runtime and player:
 *   host_window_scale_parse   "aspect" / "integer" / "stretch" -> HOST_SCALE_*
 *   host_window_filter_parse  "nearest" / "linear" -> HOST_FILTER_*
 * (case ignored; -1 for NULL, "" or anything else).
 *   host_window_env_scale     $console_var (e.g. "RECOMP_GBA_SCALE") when it
 *                             parses, else $RECOMP_SCALE, else def
 *   host_window_env_filter    $console_var, else $RECOMP_FILTER, else def
 *   host_window_env_vsync     0 when $RECOMP_PRESENT_VSYNC starts with '0',
 *                             else 1 (the vsync argument of host_window_open)
 * console_var may be NULL. */
int host_window_scale_parse(const char *v);
int host_window_filter_parse(const char *v);
int host_window_env_scale(const char *console_var, int def);
int host_window_env_filter(const char *console_var, int def);
int host_window_env_vsync(void);

/* Picture shaders (XBA-195, docs/runtime/environment-variables.md « Console picture »; sources and keys in
 * present_shaders.c, shared with the Xbox presenter): one fragment shader of our own (GLSL ES 3.0 / GLSL 1.50)
 * between the picture and the window, drawn at the output resolution in the same pass as the scaling, so after an upscaled (XBA-184) or generated (XBA-186) frame and with every scale mode:
 *   HOST_SHADER_NONE       the picture as it is (the default; the plain present, no cost)
 *   HOST_SHADER_SHARP      sharp bilinear: whole-pixel prescale then linear, crisp at any non-integer scale
 *                          (samples the window's texture linearly whatever the filter)
 *   HOST_SHADER_SCANLINES  the lower half of each console line darkened (strength: how much), faded out below two
 *                          output pixels per line
 *   HOST_SHADER_CRT        scanlines + an RGB aperture-grille mask; curvature (barrel + vignette) when set
 *   HOST_SHADER_LCD        a pixel grid (GBA-like, one output pixel per cell edge); ghosting (a blend with the
 *                          previous picture) when set, present_rgba only
 * "Console lines": the picture's height divided by round(height / 240) (an upscaled 960-line PS1 picture has 240
 * lines, a GBA 160). strength / curvature / ghosting 0..100; strength -1: the preset's default (scanlines 50,
 * CRT 60, LCD 50; unused by sharp). A preset whose shader does not compile or link falls back to none (logged
 * once). host_window_screenshot_bmp stays the raw picture; host_window_capture reads the drawable as drawn
 * (shader included). Set before or after open, from the presenting thread; kept across open / close. */
/* HOST_SHADER_* and HostShader: present_shaders.h. */
void host_window_set_shader(const HostShader *s);   /* NULL: none */
/* The preset drawn by the next present: the one set, or none after its shader failed to build. */
int host_window_shader_active(void);
/*   host_window_shader_parse  "none" / "sharp" / "scanlines" / "crt" / "lcd" -> HOST_SHADER_* (case ignored; -1
 *                             for NULL, "" or anything else); host_window_shader_name the other way ("none" for
 *                             an unknown preset)
 *   host_window_env_shader    preset: $console_var (e.g. "RECOMP_GBA_SHADER") when it parses, else $RECOMP_SHADER,
 *                             else def; strength $RECOMP_SHADER_STRENGTH, curvature $RECOMP_SHADER_CURVATURE,
 *                             ghosting $RECOMP_SHADER_GHOSTING (0..100, clamped; unset or not a number: -1 / 0 / 0)
 *   host_window_shader_env    host_window_env_shader + host_window_set_shader, what a console runtime calls with
 *                             host_window_set_scaling */
int host_window_shader_parse(const char *v);
const char *host_window_shader_name(int preset);
void host_window_env_shader(const char *console_var, int def, HostShader *out);
void host_window_shader_env(const char *console_var);

/* A live change of the picture (the in-game menu, docs/android/app-shell.md « In-game menu »), from any thread:
 * scale (HOST_SCALE_*), filter (HOST_FILTER_*) and, when s is not NULL, the shader, taken by the presenting thread
 * at its next present (host_window_present_rgba / _texture / _again), never in the middle of a frame. scale or
 * filter -1 keeps the current one. A later request replaces one not yet taken. */
void host_window_request_picture(int scale, int filter, const HostShader *s);

/* The rate the window presents at, from any thread (the Android companion
 * panel and FPS overlay when no Xbox presenter runs): *fps_centi frames per
 * second x 100 over the last second (0 when nothing was presented for two
 * seconds, e.g. while the game is held), *frames the presents since open.
 * Either pointer may be NULL. Returns 1 while the window is open, else 0
 * (both outputs 0). */
int host_window_stats(uint32_t *fps_centi, uint64_t *frames);

/* Writes the last picture host_window_present_rgba showed, as a 24-bit BMP,
 * from any thread (also while the game is held: a copy of it is kept). It is
 * the picture at its own size, reshaped to host_window_set_aspect's shape
 * when one is set (nearest, never smaller: 256x240 at 4:3 -> 320x240), no
 * letterbox. Returns 0, or -1 when no picture was presented since open or the
 * file cannot be written. */
int host_window_screenshot_bmp(const char *path);

/* The shape the picture is shown at, num:den (4:3 for a PlayStation, whose
 * frame buffer is 256..640 pixels wide whatever the screen), instead of its
 * own w:h. HOST_SCALE_INTEGER then takes whole multiples of its height, the
 * width following the shape. 0 / 0: the picture's own (the default). Kept
 * across open / close. */
void host_window_set_aspect(int num, int den);

/* Writes the next presented picture, the whole drawable as drawn (letterbox
 * included), to ppm_path as a binary PPM, read back before the swap: a
 * screenshot through the real present path (headless checks under Xvfb).
 * One capture per call; NULL or "" cancels. */
void host_window_capture(const char *ppm_path);

/* A GL entry point of the window's context (SDL_GL_GetProcAddress), for a
 * console's renderer. NULL before open. */
void *host_window_gl_proc(const char *name);

void host_window_info(HostWindowInfo *out);

/* The SDL_Window, for what the host layer does not cover (NULL before open). */
void *host_window_sdl(void);

void host_window_close(void);

#ifdef __cplusplus
}
#endif

#endif /* XBR_HOST_WINDOW_H */
