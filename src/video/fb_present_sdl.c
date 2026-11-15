/**
 * Show the guest framebuffer in a window: the POSIX half of fb_present.c.
 *
 * fb_present.c is Win32 GDI and runs its window on a thread of its own. That
 * cannot work on macOS, where AppKit -- and so SDL's video and event pump --
 * only runs on the process main thread, and the guest would otherwise own
 * that thread for the whole run. So the shape is inverted here:
 *
 *   process main thread   SDL window, the one SDL event loop, the presents
 *   "guest-main" pthread  the boot sequence and the recompiled entry point
 *
 * A title opts in by handing its boot sequence to xbox_HostWindowMain()
 * from main(). With RECOMP_FB_WINDOW set (the same switch as the Win32
 * window) it opens the window, starts the guest on a pthread with at least
 * the main thread's stack and, on macOS, its QoS, and pumps events and
 * presents until the guest returns or the window is closed. Without it, or
 * with RECOMP_HEADLESS=1, no display, or SDL video failing, the guest runs
 * on the calling thread exactly as before.
 *
 * Frames come from the executor's flip (xbox_FramebufferWindowPresent, on
 * the executor's thread). The flip copies the finished surface into a free
 * slot of three and publishes it with an index swap under a mutex held for a
 * few instructions, so the executor never waits for the window and the
 * window never reads guest memory the rasteriser may be writing. A guest
 * surface is in the GDI window's format: 640x480, bpp from pitch / 640,
 * 2 = R5G6B5 (X1R5G5B5 when the executor says so,
 * xbox_FramebufferWindowSetFormat), 4 = X8R8G8B8. A GPU back end may instead
 * hand over its own pixels at their own size (xbox_FramebufferWindowPresentHost:
 * an internal resolution above 640x480, RECOMP_GPU_SCALE); each slot carries
 * its frame's width, height, pitch and pixel format, the texture follows
 * them, and the renderer's logical size stays 640x480, so letterboxing and
 * the perf overlay do not change. Before the first flip the window reads
 * the AvSetDisplayMode surface directly, as the GDI window does, for a title
 * that never flips.
 *
 * Aspect modes (aspect_policy.h, RECOMP_ASPECT): each published frame is
 * classed when it is published (or when it reaches the frame generation
 * pacer) as NORMAL (4:3), WIDE (stretched to 16:9: a title's anamorphic 3D
 * frame) or ZOOM (a letterboxed movie's picture rows shown at 16:9); the
 * slot carries its class. The main loop switches the logical size between
 * 640x480 and 1280x720 when the class of what it shows changes, and
 * screenshots take the shape shown.
 *
 * Closing the window ends the process: the window owns the main thread, and
 * lifted code cannot be stopped mid-flight. atexit handlers still run, except
 * on Android, where recomp_quit() flushes the logs and _exit()s instead
 * (recomp_quit.h). Also on Android, a guest that returns leaves the window
 * up on its last frame until the player quits (the in-game menu, or Back
 * twice where nothing above SDL takes Back): an app does not vanish on its
 * own, and the lifecycle keeps working (XBA-52).
 *
 * Frame generation (built with XBOXRECOMP_LSFG, run with RECOMP_LSFG=1,
 * docs/technical/framegen.md): both present paths hand their frame to the
 * pacer (src/framegen) instead of a slot; the pacer's sink queues real and
 * generated frames with their due times in a FIFO of four, and the main loop
 * sleeps until the next event or due time and shows each entry when it is
 * due. Headless, the pacer still runs and its sink only counts. When the
 * pacer falls back (framegen_pacer_active() == 0) the slots take over again.
 *
 * Knobs: RECOMP_PRESENT_VSYNC=0 presents without waiting for the display;
 * RECOMP_WINDOW_SCALE=<n> sizes the window (default 2);
 * RECOMP_FULLSCREEN=1 starts in borderless desktop fullscreen.
 */
#if !defined(_WIN32)

#include "frame_times.h"   /* the frame-time log (XBA-29) */
#include "display_clock.h" /* the display's refresh grid (XBA-136) */
#ifdef __ANDROID__
#include "display_vsync_android.h"
#endif
#include <SDL.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include "platform/recomp_lifecycle.h"
#include "platform/recomp_quit.h"
#include "video_player.h"
#include "perf_overlay.h"
#include "aspect_policy.h"
#include "present_shaders.h"  /* picture shaders, shared with host_window (XBA-203) */
#include "pvideo_posix.h"     /* the PVIDEO overlay's scan-out */
#include "keyboard_sdl.h"     /* src/input: keys into the pad stand-in */
#include "xinput_xbox.h"
#if defined(XBOXRECOMP_HAVE_GPU)
#include "nv2a_gl.h"          /* src/gpu: RECOMP_GPU=1 */
#endif
#if defined(XBOXRECOMP_LSFG)
#include "framegen_pacer.h"   /* src/framegen: RECOMP_LSFG=1 */
#include "framegen_policy.h"
#if defined(XBOXRECOMP_LSFG_BRIDGE)
#include "lsfg_generator.h"
#endif
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define FB_W 640u
#define FB_H 480u

/* Back twice within this many ms quits (Android, XBA-24). */
#define BACK_QUIT_MS 2000u
/* User-facing text, named by its key (spec XBA-24). */
#define TEXT_QUIT_PROMPT "Press Back again to quit"
/* The title ended on its own (a test title like the stub; a game does not):
 * the screen stays as it was, so say it there and not only in logcat. */
#define TEXT_GUEST_DONE "The title finished (code %d). Press Back for the menu (Quit)."

/* ── Handoff: three slots, one each for writer, reader and the ready frame ── */

/* A frame: w x h pixels, rows pitch bytes apart (top row first), in SDL
 * pixel format `format` (RGB565, RGB555, XRGB8888 or XBGR8888). General on
 * purpose: whatever produces frames for the window -- the flip, a GPU back
 * end's own read-back, a frame pacer -- fills one the same way. */
struct fb_slot {
    uint8_t *px;
    size_t   cap;
    uint32_t w, h, pitch;
    uint32_t format;            /* SDL_PIXELFORMAT_* */
    uint64_t aspect;            /* aspect_pack(): class, ZOOM rows */
    uint64_t seq;               /* s_ready_seq it was published as */
    uint64_t pub_ns;            /* when it was published (XBA-136: flip -> present) */
};

/* What the window takes from SDL, and gives back on every way out. */
#define FBWIN_SDL_SUBSYSTEMS \
    (SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER)

static struct fb_slot  s_slot[3];
static int             s_back = 0, s_ready = 1, s_front = 2;
static uint64_t        s_ready_seq;          /* frames published */
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int    s_running;            /* window up: flips copy */
static Uint32          s_frame_event;        /* wakes the main loop */
static volatile int    s_wake_pending;
static volatile int    s_in_present;         /* executor inside Present */
static volatile uint32_t s_fb_va, s_fb_pitch; /* AvSetDisplayMode surface */
static volatile uint32_t s_fb_x555;           /* xbox_FramebufferWindowSetFormat */
/* One presenter at a time: the executor's flips and the overlay scan-out's
 * frames (pvideo_posix.c) both fill the back slot, class frames and feed
 * the pacer, all of which assume a single producer. */
static pthread_mutex_t s_present_mu = PTHREAD_MUTEX_INITIALIZER;
static int             s_overlay_publishing;  /* under s_present_mu */

/* Counted at the flip, read once a second for the title bar. */
static uint64_t s_flips;
static uint32_t s_frame_draws;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t now_ms(void)
{
    return now_ns() / 1000000ull;
}

/* ── Aspect modes (aspect_policy.h) ───────────────────────────────────── */

/* A frame's class and ZOOM crop in one word, so the reader never sees one
 * frame's class with another's rows: class | y0 << 8 | rows << 32. */
static uint64_t aspect_pack(int cls, uint32_t y0, uint32_t rows)
{
    return (uint64_t)(uint32_t)cls | (uint64_t)y0 << 8 | (uint64_t)rows << 32;
}
static int      aspect_cls(uint64_t a)  { return (int)(a & 0xFF); }
static uint32_t aspect_y0(uint64_t a)   { return (uint32_t)(a >> 8) & 0xFFFFFFu; }
static uint32_t aspect_rows(uint64_t a) { return (uint32_t)(a >> 32); }

static AspectPolicy      s_aspect;           /* executor thread only */
static int               s_aspect_ready;
static volatile int      s_aspect_title_wide; /* xbox_FramebufferWindowSetWide */
static volatile uint32_t s_aspect_tris;       /* xbox_FramebufferWindowFrameTris */
static uint64_t          s_aspect_now;        /* the last frame's, for the FIFO */

void xbox_FramebufferWindowSetWide(int on)
{
    __atomic_store_n(&s_aspect_title_wide, on != 0, __ATOMIC_RELAXED);
}

void xbox_FramebufferWindowFrameTris(uint32_t tris)
{
    __atomic_store_n(&s_aspect_tris, tris, __ATOMIC_RELAXED);
}

/* The class of a frame about to be shown, on the presenting thread; also
 * kept as the current one for the frame generation sink. Class changes are
 * logged. */
static uint64_t aspect_classify(const uint8_t *px, uint32_t w, uint32_t h, uint32_t pitch,
                                uint32_t format)
{
    static int last = -1;
    int layout = SDL_BYTESPERPIXEL(format) == 4 ? ASPECT_PX_32
               : format == SDL_PIXELFORMAT_RGB555 ? ASPECT_PX_555 : ASPECT_PX_565;
    /* An overlay frame is a movie picture: no triangles, whatever the
     * title's last flip drew. */
    uint32_t tris = s_overlay_publishing ? 0u : __atomic_load_n(&s_aspect_tris, __ATOMIC_RELAXED);
    int cls;
    uint64_t a;

    if (!s_aspect_ready) {
        aspect_policy_init_from_env(&s_aspect);
        s_aspect_ready = 1;
        fprintf(stderr, "  [FBWIN] aspect %s (RECOMP_ASPECT), 3D at %u triangles, zoom %s\n",
                s_aspect.setting == ASPECT_SETTING_4_3 ? "4:3"
                : s_aspect.setting == ASPECT_SETTING_16_9 ? "16:9" : "auto",
                s_aspect.min_tris, s_aspect.zoom_on ? "on" : "off");
    }
    if (s_aspect.title_wide != __atomic_load_n(&s_aspect_title_wide, __ATOMIC_RELAXED)) {
        aspect_policy_set_title_wide(&s_aspect, !s_aspect.title_wide);
        fprintf(stderr, "  [FBWIN] aspect: the title %s wide 3D frames\n",
                s_aspect.title_wide ? "draws" : "no longer draws");
    }
    cls = aspect_policy_frame(&s_aspect, tris, px, w, h, pitch, layout);
    a = cls == ASPECT_ZOOM ? aspect_pack(cls, s_aspect.zoom_y0, s_aspect.zoom_h)
                           : aspect_pack(cls, 0, 0);
    if (cls != last) {
        if (cls == ASPECT_ZOOM)
            fprintf(stderr, "  [FBWIN] aspect %s -> ZOOM (%ux%u, rows %u-%u, %u triangles)\n",
                    last < 0 ? "-" : aspect_class_name(last), w, h, s_aspect.zoom_y0,
                    s_aspect.zoom_y0 + s_aspect.zoom_h - 1, tris);
        else
            fprintf(stderr, "  [FBWIN] aspect %s -> %s (%ux%u, %u triangles)\n",
                    last < 0 ? "-" : aspect_class_name(last), aspect_class_name(cls), w, h,
                    tris);
        last = cls;
    }
    __atomic_store_n(&s_aspect_now, a, __ATOMIC_RELAXED);
    return a;
}

/* Wake the main loop, once per batch of frames it has not looked at. */
static void wake_main_loop(void)
{
    if (!__atomic_exchange_n(&s_wake_pending, 1, __ATOMIC_ACQ_REL)) {
        SDL_Event ev;
        memset(&ev, 0, sizeof ev);
        ev.type = s_frame_event;
        /* Not queued (full, or filtered): clear the latch, or no frame
         * would wake the loop again before its timeout. */
        if (SDL_PushEvent(&ev) <= 0)
            __atomic_store_n(&s_wake_pending, 0, __ATOMIC_RELEASE);
    }
}

/* ── Frame generation: pacer, timed FIFO ──────────────────────────────── */

#if defined(XBOXRECOMP_LSFG)
/* An entry: w x h packed BGRX (SDL XRGB8888), due on screen at due_ns
 * (CLOCK_MONOTONIC). */
struct fg_entry {
    uint8_t *px;
    size_t   cap;
    uint32_t w, h;
    uint64_t due_ns;
    uint64_t aspect;            /* the class when it reached the sink */
};

#define FG_FIFO 4
static struct fg_entry s_fg_fifo[FG_FIFO];   /* ring, s_fg_head the oldest */
static int             s_fg_head, s_fg_count;
static struct fg_entry s_fg_front;           /* shown by the main loop */
static pthread_mutex_t s_fg_lock = PTHREAD_MUTEX_INITIALIZER;
static int             s_fg_on;              /* the pacer runs */
static int             s_fg_window;          /* ... and feeds the window */
static volatile int    s_fg_stop_req;        /* xbox_FramegenRequestStop asked */
static volatile int    s_fg_wait;            /* waits for the lock verdict (xbox_FramegenRelease) */
static uint64_t        s_fg_full_drops;      /* pushed out of a full FIFO */
static uint64_t        s_fg_late_skips;      /* due, but a newer one was too */
static uint64_t        s_fg_presented;       /* FIFO frames the window showed */

/* Frames go to the pacer rather than the slots. */
static int fg_feeding(void)
{
    /* Not once a stop is asked (XBA-96): the window loop then waits for the
     * presents in flight (s_in_present) before it stops the pacer, so no
     * submit can reach a pacer being torn down. seq_cst on both sides: a
     * present either sees the request or is counted in s_in_present. */
    return s_fg_on && !__atomic_load_n(&s_fg_stop_req, __ATOMIC_SEQ_CST)
        && !__atomic_load_n(&s_fg_wait, __ATOMIC_ACQUIRE) && framegen_pacer_active();
}

/* The window's sink, on the pacer thread: copy into the FIFO and wake the
 * loop. A full FIFO loses its oldest entry. */
static void fg_sink_window(const uint8_t *px, uint32_t w, uint32_t h, uint32_t bpp,
                           uint64_t due_ns)
{
    size_t need = (size_t)w * h * 4;
    struct fg_entry *e;

    if (bpp != 4)
        return;
    pthread_mutex_lock(&s_fg_lock);
    if (s_fg_count == FG_FIFO) {
        s_fg_head = (s_fg_head + 1) % FG_FIFO;
        s_fg_count--;
        s_fg_full_drops++;
    }
    e = &s_fg_fifo[(s_fg_head + s_fg_count) % FG_FIFO];
    if (e->cap < need) {
        uint8_t *n = (uint8_t *)realloc(e->px, need);
        if (!n) {
            pthread_mutex_unlock(&s_fg_lock);
            return;
        }
        e->px = n;
        e->cap = need;
    }
    memcpy(e->px, px, need);
    e->w = w;
    e->h = h;
    e->due_ns = due_ns;
    /* Real and generated frames alike: the class of the latest real frame
     * (a generated frame sits between two of the same scene). */
    e->aspect = __atomic_load_n(&s_aspect_now, __ATOMIC_RELAXED);
    s_fg_count++;
    pthread_mutex_unlock(&s_fg_lock);
    wake_main_loop();
}

static void fg_log_stats(void)
{
    FramegenPacerCounters c;
    uint32_t real, shown, gen;

    framegen_pacer_counters(&c);
    framegen_pacer_stats(&real, &shown, &gen);
    fprintf(stderr, "[LSFG] real %u.%02u fps, shown %u.%02u fps, generation %.2f ms; "
            "frames: %llu real in, %llu generated, shown %llu real + %llu generated, "
            "dropped %llu input / %llu real / %llu generated, skipped %llu, failed %llu, "
            "over budget %llu",
            real / 100, real % 100, shown / 100, shown % 100, gen / 1000.0,
            (unsigned long long)c.submitted, (unsigned long long)c.generated,
            (unsigned long long)c.shown_real, (unsigned long long)c.shown_generated,
            (unsigned long long)c.dropped_input, (unsigned long long)c.dropped_real,
            (unsigned long long)c.dropped_generated, (unsigned long long)c.skipped,
            (unsigned long long)c.failed, (unsigned long long)c.over_budget);
    if (s_fg_window) {
        pthread_mutex_lock(&s_fg_lock);
        fprintf(stderr, "; window presented %llu, FIFO full %llu, late %llu",
                (unsigned long long)s_fg_presented, (unsigned long long)s_fg_full_drops,
                (unsigned long long)s_fg_late_skips);
        pthread_mutex_unlock(&s_fg_lock);
    }
    fputc('\n', stderr);
}

/* Headless sink: nothing to show; the pacer's counters are the figures,
 * logged every 5 s of due time and at exit. */
static void fg_sink_headless(const uint8_t *px, uint32_t w, uint32_t h, uint32_t bpp,
                             uint64_t due_ns)
{
    static uint64_t next;
    (void)px; (void)w; (void)h; (void)bpp;
    if (!next)
        next = due_ns + 5000000000ull;
    else if (due_ns >= next) {
        next = due_ns + 5000000000ull;
        fg_log_stats();
    }
}

static void fg_atexit(void)
{
    if (s_fg_on)
        fg_log_stats();
}

static void fg_fifo_clear(void)
{
    pthread_mutex_lock(&s_fg_lock);
    s_fg_head = s_fg_count = 0;
    pthread_mutex_unlock(&s_fg_lock);
}

/* The executor found the title wants every vblank (XBA-96, frame_lock.h):
 * frame generation stops at the window loop's next turn, and frames go to
 * the window as without it. Any thread. */
void xbox_FramegenRequestStop(const char *why)
{
    if (s_fg_on && !__atomic_exchange_n(&s_fg_stop_req, 1, __ATOMIC_SEQ_CST))
        fprintf(stderr, "[LSFG] frame generation off: %s\n", why ? why : "asked");
}

/* WP4: the executor judged the title free to run below its ask, or locked to
 * a rate frame generation's cap does not divide (frame_lock_gate): frames go
 * to the pacer from the next present. Until then they go to the window as
 * without frame generation, and the cap was not lowered. Any thread. */
void xbox_FramegenRelease(const char *why)
{
    if (s_fg_on && __atomic_exchange_n(&s_fg_wait, 0, __ATOMIC_ACQ_REL)) {
        framegen_pacer_reset();     /* the first frame primes the generator */
        fprintf(stderr, "[LSFG] frame generation on: %s\n", why ? why : "asked");
    }
}

/* Lifecycle pause / resume: nothing from before the pause is blended with,
 * or shown after, what comes after it. */
static void fg_lifecycle(int paused, void *user)
{
    (void)paused; (void)user;
    framegen_pacer_reset();
    fg_fifo_clear();
}

/* The display's refresh rate for the cap rule (framegen_policy.h), unless
 * RECOMP_LSFG_DISPLAY_HZ says otherwise. Only a rate below 60 Hz is passed
 * on: at 120 Hz the cap rule would ask for 60 real fps, and a title that
 * runs at 30 (most do) would then be over the pacer's budget and past its
 * skip threshold every other frame; 60 shown fps sit evenly on a 120 Hz
 * display. Before the guest starts: the executor reads the cap at its
 * first flip. */
static void fg_report_display_rate(SDL_Window *win)
{
    SDL_DisplayMode mode;
    int idx = SDL_GetWindowDisplayIndex(win);
    const char *e = getenv("RECOMP_LSFG_DISPLAY_HZ");

    if (e && *e)
        return;
    if (idx >= 0 && SDL_GetCurrentDisplayMode(idx, &mode) == 0
            && mode.refresh_rate >= 24 && mode.refresh_rate <= 480) {
        if (mode.refresh_rate < 60) {
            char hz[16];
            snprintf(hz, sizeof hz, "%d", mode.refresh_rate);
            setenv("RECOMP_LSFG_DISPLAY_HZ", hz, 1);
        }
        fprintf(stderr, "[LSFG] display %d Hz; pacing for %d Hz (RECOMP_LSFG_DISPLAY_HZ)\n",
                mode.refresh_rate, mode.refresh_rate < 60 ? mode.refresh_rate : 60);
    }
}

/* Under RECOMP_LSFG=1: register the bridge (when built) and start the
 * pacer. window = 0: headless, the sink only counts. Under RECOMP_PACING=auto
 * (the default) the pacer starts idle: the frames reach it only once the
 * executor has judged the title (nv2a_pb_exec_framegen_waits, WP4). */
extern int nv2a_pb_exec_framegen_waits(void);
static void fg_start(int window)
{
    static FramegenSink sink;

    if (s_fg_on || !framegen_policy_enabled())
        return;
#if defined(XBOXRECOMP_LSFG_BRIDGE)
    lsfg_bridge_register();
#endif
    sink.show = window ? fg_sink_window : fg_sink_headless;
    if (framegen_pacer_start(&sink) < 0)
        return;
    s_fg_window = window;
    if (nv2a_pb_exec_framegen_waits()) {
        __atomic_store_n(&s_fg_wait, 1, __ATOMIC_RELEASE);
        fprintf(stderr, "[LSFG] frame generation waits for the title's lock verdict (25 to 50 s after its "
                        "first frame): frames shown as they come until then\n");
    }
    s_fg_on = 1;
    recomp_lifecycle_listen(fg_lifecycle, NULL);
    atexit(fg_atexit);
}

/* Main loop: the newest entry that is due (older due ones are skipped),
 * swapped into s_fg_front; 1 when there is one. *next_due: the next
 * entry's due time, 0 when none is left. */
static int fg_take_due(uint64_t now, uint64_t *next_due)
{
    int got = 0;

    pthread_mutex_lock(&s_fg_lock);
    /* Within a millisecond counts as due: the loop sleeps in whole ms. */
    while (s_fg_count && s_fg_fifo[s_fg_head].due_ns <= now + 1000000ull) {
        struct fg_entry t = s_fg_front;
        if (got)
            s_fg_late_skips++;
        s_fg_front = s_fg_fifo[s_fg_head];
        s_fg_fifo[s_fg_head] = t;
        s_fg_head = (s_fg_head + 1) % FG_FIFO;
        s_fg_count--;
        got = 1;
    }
    if (got)
        s_fg_presented++;
    *next_due = s_fg_count ? s_fg_fifo[s_fg_head].due_ns : 0;
    pthread_mutex_unlock(&s_fg_lock);
    return got;
}
#endif /* XBOXRECOMP_LSFG */

/* The display's refresh rate for RECOMP_PACING=auto (nv2a_pb_exec.c,
 * frame_pacing.h), which bounds a title that presents IMMEDIATE by it:
 * RECOMP_DISPLAY_HZ, unless already set. Before the guest starts: the
 * executor reads it once, at its first flip. */
static int report_display_hz(SDL_Window *win)
{
    SDL_DisplayMode mode;
    int idx = SDL_GetWindowDisplayIndex(win);
    const char *e = getenv("RECOMP_DISPLAY_HZ");
    char hz[16];

    if (e && *e)
        return atoi(e);
    if (idx < 0 || SDL_GetCurrentDisplayMode(idx, &mode) != 0
            || mode.refresh_rate < 24 || mode.refresh_rate > 480)
        return 0;
    snprintf(hz, sizeof hz, "%d", mode.refresh_rate);
    setenv("RECOMP_DISPLAY_HZ", hz, 1);
    return mode.refresh_rate;
}

/* XBA-136: the display's refresh grid for RECOMP_PACING=auto's flips in
 * phase with the panel (frame_pacing_align_slot in the executor). Android:
 * AChoreographer on its own thread (display_vsync_android.c). Elsewhere:
 * the return of a vsynced SDL_RenderPresent that blocked (it waited for the
 * refresh), on this thread; presents that do not block give no grid, and
 * none is published. Only when the executor uses it (the switches of
 * pacing_auto, and RECOMP_PACING_ALIGN not 0). Both renderers present
 * here, so the GPU back end and the CPU rasteriser are aligned alike. */
extern void nv2a_pb_exec_display_clock(uint64_t vsync_ns, uint64_t period_ns, uint32_t gen);
extern void nv2a_pb_exec_present_latency(uint64_t lat_ns);
static DisplayClock s_dclk;
static int s_dclk_present;              /* the desktop source is on */
static int s_dclk_wanted;
static int64_t s_present_lat_ns;        /* flip -> present call, mean */

extern int nv2a_pb_exec_pacing_auto_wanted(void);
static int display_align_wanted(void)
{
    const char *a = getenv("RECOMP_PACING_ALIGN");
    return nv2a_pb_exec_pacing_auto_wanted() && !(a && *a == '0' && !strchr(a, '.'));
}

static void display_sync_start(int hz, int vsync)
{
    uint64_t nominal = hz > 0 ? 1000000000ull / (uint64_t)hz : 0;

    s_dclk_wanted = display_align_wanted();
    if (!s_dclk_wanted)
        return;
#ifdef __ANDROID__
    (void)vsync;
    if (display_vsync_android_start(nominal, nv2a_pb_exec_display_clock))
        fprintf(stderr, "  [FBWIN] display clock: Choreographer (%s)\n",
                display_vsync_android_source());
    else
        fprintf(stderr, "  [FBWIN] display clock: no Choreographer; flips not aligned\n");
#else
    if (!vsync) {
        fprintf(stderr, "  [FBWIN] display clock: none (RECOMP_PRESENT_VSYNC=0)\n");
        return;
    }
    display_clock_init(&s_dclk, nominal);
    s_dclk_present = 1;
    fprintf(stderr, "  [FBWIN] display clock: the returns of vsynced presents that block\n");
#endif
}

/* Around each present of a new frame: its flip -> present time, and on the
 * desktop the present's return when it blocked for at least 2 ms. */
static void display_sync_present(uint64_t pub_ns, uint64_t t_call, uint64_t t_ret)
{
    if (!s_dclk_wanted)
        return;
    if (pub_ns && t_call > pub_ns && t_call - pub_ns < 250000000ull) {
        int64_t l = (int64_t)(t_call - pub_ns);
        s_present_lat_ns = s_present_lat_ns ? s_present_lat_ns + (l - s_present_lat_ns) / 16 : l;
        nv2a_pb_exec_present_latency((uint64_t)s_present_lat_ns);
    }
    if (s_dclk_present && t_ret - t_call >= 2000000ull) {
        uint64_t ref, per;
        uint32_t gen = s_dclk.gen;
        int was = s_dclk.locked;
        display_clock_sample(&s_dclk, t_ret, 0);
        if (display_clock_get(&s_dclk, &ref, &per))
            nv2a_pb_exec_display_clock(ref, per, s_dclk.gen);
        else if (was || gen != s_dclk.gen)
            nv2a_pb_exec_display_clock(0, 0, s_dclk.gen);
    }
}

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    s_fb_va = fb_va;
    if (pitch)
        s_fb_pitch = pitch;
}

/* ponytail: the SDL window shows the surface unscaled; 2x2 anti-aliased
 * surfaces (#162) come through at twice the height until it honours this. */
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }

/* SET_SURFACE_FORMAT colour format of what the next presents hold in guest
 * memory: 1 and 2 (X1R5G5B5) are shown as 555, anything else 16-bit as
 * R5G6B5. */
void xbox_FramebufferWindowSetFormat(uint32_t color_format)
{
    s_fb_x555 = color_format == 1 || color_format == 2;
}

/* The window is opened by xbox_HostWindowMain, on the main thread. */
void xbox_FramebufferWindowStart(void) {}

/* SDL pixel format of a guest surface of bpp bytes per pixel. */
static uint32_t guest_format(uint32_t bpp, uint32_t x555)
{
    return bpp == 4 ? SDL_PIXELFORMAT_RGB888        /* = XRGB8888 */
         : x555     ? SDL_PIXELFORMAT_RGB555
                    : SDL_PIXELFORMAT_RGB565;
}

/* The back slot with room for h rows of pitch bytes, or NULL. Only the
 * presenting thread touches s_back. */
static struct fb_slot *back_slot(uint32_t pitch, uint32_t h)
{
    struct fb_slot *s = &s_slot[s_back];
    size_t need = (size_t)pitch * h;

    if (s->cap < need) {
        uint8_t *n = (uint8_t *)realloc(s->px, need);
        if (!n)
            return NULL;
        s->px = n;
        s->cap = need;
    }
    return s;
}

/* The back slot is filled: make it the ready one. An unshown ready frame is
 * overwritten (dropped), never waited for. */
static void publish(uint32_t w, uint32_t h, uint32_t pitch, uint32_t format)
{
    s_slot[s_back].w = w;              /* only this thread touches s_back */
    s_slot[s_back].h = h;
    s_slot[s_back].pitch = pitch;
    s_slot[s_back].format = format;
    s_slot[s_back].aspect = aspect_classify(s_slot[s_back].px, w, h, pitch, format);
    pthread_mutex_lock(&s_lock);
    {
        int t = s_ready;
        s_ready = s_back;
        s_back = t;
        s_ready_seq++;
        s_slot[s_ready].seq = s_ready_seq;
        s_slot[s_ready].pub_ns = now_ns();
    }
    pthread_mutex_unlock(&s_lock);

    wake_main_loop();
}

/* Copy w x h pixels (rows src_pitch bytes apart, row_bytes of them used)
 * into the back slot and publish it. */
static void present_pixels(const uint8_t *src, uint32_t w, uint32_t h,
                           uint32_t src_pitch, uint32_t row_bytes, uint32_t format)
{
    struct fb_slot *s = back_slot(row_bytes, h);
    uint32_t y;

    if (!s)
        return;
    if (src_pitch == row_bytes)
        memcpy(s->px, src, (size_t)row_bytes * h);
    else
        for (y = 0; y < h; y++)
            memcpy(s->px + (size_t)y * row_bytes, src + (size_t)y * src_pitch, row_bytes);
    publish(w, h, row_bytes, format);
}

static void present_copy(uint32_t fb_va, uint32_t pitch)
{
    uint32_t bpp = pitch / FB_W;

    if (bpp != 2 && bpp != 4)
        return;
    present_pixels((const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset()),
                   FB_W, FB_H, pitch, FB_W * bpp, guest_format(bpp, bpp == 2 && s_fb_x555));
}

static void present_guest_locked(uint32_t fb_va, uint32_t pitch)
{
#if defined(XBOXRECOMP_LSFG)
    /* Headless frame generation: the pacer takes the frame, nobody shows it. */
    if (!s_fg_window && fb_va && fg_feeding()) {
        uint32_t bpp = pitch / FB_W;
        if (bpp == 2 || bpp == 4)
            framegen_pacer_submit_format(
                (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset()), FB_W, FB_H,
                bpp == 4 ? FRAMEGEN_FMT_BGRX8888
                         : s_fb_x555 ? FRAMEGEN_FMT_X1R5G5B5 : FRAMEGEN_FMT_R5G6B5,
                pitch, now_ns());
        return;
    }
#endif
    /* Dekker-style handshake with the close path: each side stores its own
     * flag and then loads the other's, all seq_cst, so either the window sees
     * this present in flight or this present sees the window closing. */
    if (!__atomic_load_n(&s_running, __ATOMIC_SEQ_CST) || !fb_va || !pitch)
        return;
    __atomic_add_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&s_running, __ATOMIC_SEQ_CST)) {
#if defined(XBOXRECOMP_LSFG)
        uint32_t bpp = pitch / FB_W;
        if (fg_feeding() && (bpp == 2 || bpp == 4)) {
            const uint8_t *src = (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset());
            (void)aspect_classify(src, FB_W, FB_H, pitch,
                                  guest_format(bpp, bpp == 2 && s_fb_x555));
            /* The flip time is now: the read-back is synchronous under
             * frame generation (framegen_policy.h). */
            framegen_pacer_submit_format(
                (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset()), FB_W, FB_H,
                bpp == 4 ? FRAMEGEN_FMT_BGRX8888
                         : s_fb_x555 ? FRAMEGEN_FMT_X1R5G5B5 : FRAMEGEN_FMT_R5G6B5,
                pitch, now_ns());
        } else
#endif
        present_copy(fb_va, pitch);
    }
    __atomic_sub_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
}

/* Called by the pushbuffer executor when the title flips. */
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    uint32_t bpp = pitch / FB_W;

    /* App in the background: the flip waits here, and the title's other
     * threads stop behind it (they wait for the GPU). Before the handshake
     * below, so a window that closes meanwhile is not kept waiting. */
    recomp_lifecycle_hold();
    pthread_mutex_lock(&s_present_mu);
    /* While the video overlay is up the screen is the frame with the
     * overlay on it: the overlay takes the frame (headless too, for the
     * dumps). */
    if (!fb_va || (bpp != 2 && bpp != 4)
            || !pvideo_frame((const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset()),
                             FB_W, FB_H, pitch, guest_format(bpp, bpp == 2 && s_fb_x555)))
        present_guest_locked(fb_va, pitch);
    pthread_mutex_unlock(&s_present_mu);
}

static uint32_t host_sdl_format(uint32_t format)
{
    return format == XBOX_FB_HOST_BGRX8888 ? SDL_PIXELFORMAT_XRGB8888   /* 0xXXRRGGBB words */
         : format == XBOX_FB_HOST_RGBX8888 ? SDL_PIXELFORMAT_XBGR8888   /* bytes R, G, B, X */
         : 0;
}

static int present_host_locked(const void *px, uint32_t w, uint32_t h,
                               uint32_t pitch, uint32_t format)
{
    uint32_t sdl_format = host_sdl_format(format);
    int taken = 0;

    if (!__atomic_load_n(&s_running, __ATOMIC_SEQ_CST) || !px || !w || !h
            || w > 8192 || h > 8192 || pitch < w * 4u || !sdl_format)
        return 0;
    __atomic_add_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&s_running, __ATOMIC_SEQ_CST)) {
        /* The copy's cost on the executor thread, logged every 600 host
         * frames (only this thread presents). */
        static uint32_t frames;
        static uint64_t ns;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
#if defined(XBOXRECOMP_LSFG)
        if (fg_feeding()) {
            (void)aspect_classify((const uint8_t *)px, w, h, pitch, sdl_format);
            /* At its own size: the pacer reopens the generator when it
             * changes. */
            framegen_pacer_submit_format((const uint8_t *)px, w, h,
                                         format == XBOX_FB_HOST_BGRX8888 ? FRAMEGEN_FMT_BGRX8888
                                                                         : FRAMEGEN_FMT_RGBX8888,
                                         pitch, (uint64_t)t0.tv_sec * 1000000000ull
                                                + (uint64_t)t0.tv_nsec);
        } else
#endif
        present_pixels((const uint8_t *)px, w, h, pitch, w * 4u, sdl_format);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        ns += (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull
            + (uint64_t)t1.tv_nsec - (uint64_t)t0.tv_nsec;
        if (++frames == 600) {
            fprintf(stderr, "  [FBWIN] host frames %ux%u: copy %.3f ms/frame\n", w, h,
                    (double)ns / 1e6 / frames);
            frames = 0;
            ns = 0;
        }
        taken = 1;
    }
    __atomic_sub_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
    return taken;
}

/* A GPU back end's own frame (FLIP_STALL with Nv2aPresentSurface.host_px):
 * copied into a slot like a guest frame, at its own size. Returns 1 when
 * the window took it, 0 when there is no window (headless, closing) or the
 * frame is not one it can show. While the video overlay is up
 * the frame goes under it (pvideo_frame). */
int xbox_FramebufferWindowPresentHost(const void *px, uint32_t w, uint32_t h,
                                      uint32_t pitch, uint32_t format)
{
    int taken;

    recomp_lifecycle_hold();
    pthread_mutex_lock(&s_present_mu);
    if (px && w && h && w <= 8192 && h <= 8192 && pitch >= w * 4u && host_sdl_format(format)
            && pvideo_frame((const uint8_t *)px, w, h, pitch, host_sdl_format(format)))
        taken = __atomic_load_n(&s_running, __ATOMIC_SEQ_CST);
    else
        taken = present_host_locked(px, w, h, pitch, format);
    pthread_mutex_unlock(&s_present_mu);
    return taken;
}

/* ── For the overlay scan-out (pvideo_posix.h) ────────────────────────── */

void fbwin_present_lock(void)   { pthread_mutex_lock(&s_present_mu); }
void fbwin_present_unlock(void) { pthread_mutex_unlock(&s_present_mu); }

/* One pixel as 0xRRGGBB (defined with the screenshots below). */
static uint32_t slot_rgb(const uint8_t *p, uint32_t format);

void fbwin_to_xrgb(const uint8_t *src, uint32_t w, uint32_t h, uint32_t pitch,
                   uint32_t format, uint32_t *dst)
{
    uint32_t bypp = SDL_BYTESPERPIXEL(format), x, y;

    for (y = 0; y < PVIDEO_H; y++) {
        const uint8_t *row = src + (size_t)(y * h / PVIDEO_H) * pitch;
        uint32_t *out = dst + (size_t)y * PVIDEO_W;
        if (w == PVIDEO_W && bypp == 4 && format == SDL_PIXELFORMAT_XRGB8888) {
            for (x = 0; x < PVIDEO_W; x++)
                out[x] = 0xFF000000u | ((const uint32_t *)row)[x];
            continue;
        }
        for (x = 0; x < PVIDEO_W; x++)
            out[x] = 0xFF000000u | slot_rgb(row + (size_t)(x * w / PVIDEO_W) * bypp, format);
    }
}

/* The newest frame shown or about to be: the frame generation FIFO's front,
 * else the newest published slot, else (headless, or before the first flip)
 * the display surface in guest memory. */
int fbwin_last_frame(uint32_t *dst)
{
    int got = 0;

#if defined(XBOXRECOMP_LSFG)
    if (fg_feeding() && s_fg_window) {
        pthread_mutex_lock(&s_fg_lock);
        if (s_fg_front.px && s_fg_front.w && s_fg_front.h) {
            fbwin_to_xrgb(s_fg_front.px, s_fg_front.w, s_fg_front.h, s_fg_front.w * 4u,
                          SDL_PIXELFORMAT_XRGB8888, dst);
            got = 1;
        }
        pthread_mutex_unlock(&s_fg_lock);
        if (got)
            return 1;
    }
#endif
    /* The back slot is only written under s_present_mu, which the caller
     * holds; the main loop swaps ready and front under s_lock and only
     * reads them. */
    pthread_mutex_lock(&s_lock);
    {
        struct fb_slot *a = &s_slot[s_ready], *b = &s_slot[s_front];
        struct fb_slot *n = a->seq >= b->seq ? a : b;
        if (n->seq && n->px && n->w && n->h) {
            fbwin_to_xrgb(n->px, n->w, n->h, n->pitch, n->format, dst);
            got = 1;
        }
    }
    pthread_mutex_unlock(&s_lock);
    if (!got && s_fb_va && s_fb_pitch) {
        uint32_t bpp = s_fb_pitch / FB_W;
        if (bpp == 2 || bpp == 4) {
            fbwin_to_xrgb((const uint8_t *)((uintptr_t)s_fb_va + xbox_GetMemoryOffset()),
                          FB_W, FB_H, s_fb_pitch, guest_format(bpp, bpp == 2 && s_fb_x555), dst);
            got = 1;
        }
    }
    return got;
}

int fbwin_publish_overlay(const uint32_t *px)
{
    int taken;

    s_overlay_publishing = 1;
    taken = present_host_locked(px, PVIDEO_W, PVIDEO_H, PVIDEO_W * 4u, XBOX_FB_HOST_BGRX8888);
    s_overlay_publishing = 0;
    return taken;
}

/* Title bar: "<XBE title> | FPS: n | draws: n", as the GDI window shows it.
 * Written by the loader and the flip, read once a second by the main
 * thread; a torn read shows one stale value for a second. */
static char s_title[128] = "Xbox Recomp";

/* src/kernel/prep_progress.h: "preparing data <n> MB" while the title fills
 * its utility drive (Z:), else 0 and an empty string (WP7). */
int recomp_prep_status(char *buf, size_t n);

/* The status the title bar and the overlay show: the XBE title, then the
 * preparation line while there is one. */
static const char *window_status(char *buf, size_t n)
{
    char prep[64];

    if (!recomp_prep_status(prep, sizeof prep))
        return s_title;
    snprintf(buf, n, "%s - %s", s_title, prep);
    return buf;
}

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    char buf[sizeof s_title];
    size_t o = 0;
    int i;

    /* UTF-16 (the certificate's title) to UTF-8 for SDL. BMP only: a lone
     * surrogate becomes '?'. */
    for (i = 0; i < max_chars && name[i]; i++) {
        uint32_t c = name[i];
        if (c >= 0xD800 && c <= 0xDFFF)
            c = '?';
        if (c < 0x80) {
            if (o + 1 >= sizeof buf) break;
            buf[o++] = (char)c;
        } else if (c < 0x800) {
            if (o + 2 >= sizeof buf) break;
            buf[o++] = (char)(0xC0 | (c >> 6));
            buf[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (o + 3 >= sizeof buf) break;
            buf[o++] = (char)(0xE0 | (c >> 12));
            buf[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            buf[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    if (o) {
        buf[o] = 0;
        memcpy(s_title, buf, o + 1);
    }
}

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    __atomic_add_fetch(&s_flips, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&s_frame_draws, draws, __ATOMIC_RELAXED);
}

/* The title bar's rate, kept for other readers (the Android companion
 * panel): frames per second x 100, set once a second by the main loop. */
static uint32_t s_fps_centi;

void xbox_FramebufferWindowGetStats(uint32_t *fps_centi, uint32_t *draws)
{
    if (fps_centi)
        *fps_centi = __atomic_load_n(&s_fps_centi, __ATOMIC_RELAXED);
    if (draws)
        *draws = __atomic_load_n(&s_frame_draws, __ATOMIC_RELAXED);
}

/* One pixel of a slot as 0xRRGGBB. */
static uint32_t slot_rgb(const uint8_t *p, uint32_t format)
{
    uint32_t v;

    switch (format) {
    case SDL_PIXELFORMAT_XRGB8888:      /* bytes B, G, R, X */
        return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
    case SDL_PIXELFORMAT_XBGR8888:      /* bytes R, G, B, X */
        return (uint32_t)p[2] | (uint32_t)p[1] << 8 | (uint32_t)p[0] << 16;
    case SDL_PIXELFORMAT_RGB555:
        v = (uint32_t)p[0] | (uint32_t)p[1] << 8;
        return ((v >> 10 & 0x1F) * 255u / 31u) << 16 | ((v >> 5 & 0x1F) * 255u / 31u) << 8
             | (v & 0x1F) * 255u / 31u;
    default:                            /* RGB565 */
        v = (uint32_t)p[0] | (uint32_t)p[1] << 8;
        return ((v >> 11 & 0x1F) * 255u / 31u) << 16 | ((v >> 5 & 0x3F) * 255u / 63u) << 8
             | (v & 0x1F) * 255u / 31u;
    }
}

/* w x h pixels of a slot as a 24-bit BMP, shaped as the window shows them
 * (aspect: its class): a WIDE frame resampled linearly along x to 16:9
 * (1440x1080 -> 1920x1080), a ZOOM frame cropped to its rows first.
 * -1: the file could not be written. */
static int write_bmp(const char *path, const uint8_t *px, uint32_t sw, uint32_t sh,
                     uint32_t pitch, uint32_t format, uint64_t aspect)
{
    uint32_t bypp = SDL_BYTESPERPIXEL(format), w, h, row, img, total, x, y, y0 = 0;
    uint8_t hdr[54], *line;
    FILE *f;
    int cls = aspect_cls(aspect);

    aspect_shot_size(cls, sw, sh, aspect_rows(aspect), &w, &h);
    if (cls == ASPECT_ZOOM && h != sh)
        y0 = aspect_y0(aspect) + h <= sh ? aspect_y0(aspect) : sh - h;
    row = (w * 3u + 3u) & ~3u;
    img = row * h;
    total = 54u + img;

    line = (uint8_t *)calloc(1, row);
    f = line ? fopen(path, "wb") : NULL;
    if (!f) {
        free(line);
        return -1;
    }
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof hdr, f);
    for (y = 0; y < h; y++) {
        const uint8_t *src = px + (size_t)(y0 + h - 1 - y) * pitch;
        for (x = 0; x < w; x++) {
            uint32_t v;
            if (w == sw)
                v = slot_rgb(src + (size_t)x * bypp, format);
            else {
                /* Pixel centres: source x = (x + 0.5) * sw / w - 0.5, in
                 * 1/256ths, between two neighbours. */
                int64_t fx = ((int64_t)(2 * x + 1) * sw * 256) / (2 * w) - 128;
                uint32_t x0, x1, f, a, b, c;
                if (fx < 0)
                    fx = 0;
                x0 = (uint32_t)(fx >> 8);
                f = (uint32_t)(fx & 255);
                if (x0 >= sw - 1) {
                    x0 = sw - 1;
                    f = 0;
                }
                x1 = f ? x0 + 1 : x0;
                a = slot_rgb(src + (size_t)x0 * bypp, format);
                b = slot_rgb(src + (size_t)x1 * bypp, format);
                v = 0;
                for (c = 0; c < 24; c += 8)
                    v |= ((((a >> c) & 255) * (256 - f) + ((b >> c) & 255) * f + 128) >> 8) << c;
            }
            line[x * 3 + 0] = (uint8_t)v;
            line[x * 3 + 1] = (uint8_t)(v >> 8);
            line[x * 3 + 2] = (uint8_t)(v >> 16);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    if (fclose(f) != 0)
        return -1;
    fprintf(stderr, "  [FBWIN] wrote %s (%ux%u)\n", path, w, h);
    return 0;
}

/* The last published frame as a 24-bit BMP at its own height (480 for a
 * guest frame, the back end's height for a host frame) and its shown shape
 * (write_bmp: 16:9 frames are widened), from any thread. The
 * ready slot is only swapped under s_lock and nobody writes it, so it is
 * copied under the lock and written out after. -1: no frame yet, or the
 * file could not be written. */
int xbox_FramebufferDumpBmp(const char *path)
{
    uint32_t w = 0, h = 0, pitch = 0, format = 0;
    uint64_t aspect = 0;
    uint8_t *px = NULL;
    int rc;

#if defined(XBOXRECOMP_LSFG)
    /* Frame generation: the frame on screen is the FIFO's last shown one. */
    if (fg_feeding() && s_fg_window) {
        pthread_mutex_lock(&s_fg_lock);
        if (s_fg_front.px && s_fg_front.w) {
            w = s_fg_front.w;
            h = s_fg_front.h;
            pitch = w * 4u;
            format = SDL_PIXELFORMAT_XRGB8888;
            aspect = s_fg_front.aspect;
            if ((px = (uint8_t *)malloc((size_t)pitch * h)))
                memcpy(px, s_fg_front.px, (size_t)pitch * h);
        }
        pthread_mutex_unlock(&s_fg_lock);
        if (px) {
            rc = write_bmp(path, px, w, h, pitch, format, aspect);
            free(px);
            return rc;
        }
    }
#endif
    pthread_mutex_lock(&s_lock);
    if (s_ready_seq && s_slot[s_ready].px) {
        w = s_slot[s_ready].w;
        h = s_slot[s_ready].h;
        pitch = s_slot[s_ready].pitch;
        format = s_slot[s_ready].format;
        aspect = s_slot[s_ready].aspect;
        if ((px = (uint8_t *)malloc((size_t)pitch * h)))
            memcpy(px, s_slot[s_ready].px, (size_t)pitch * h);
    }
    pthread_mutex_unlock(&s_lock);
    if (!px)
        return -1;
    rc = write_bmp(path, px, w, h, pitch, format, aspect);
    free(px);
    return rc;
}

int fbwin_write_xrgb_bmp(const char *path, const uint32_t *px)
{
    return write_bmp(path, (const uint8_t *)px, PVIDEO_W, PVIDEO_H, PVIDEO_W * 4u,
                     SDL_PIXELFORMAT_XRGB8888, aspect_pack(ASPECT_NORMAL, 0, 0));
}

#if defined(XBOXRECOMP_LSFG)
#define FG_NONE_YET (!fg_any)
#else
#define FG_NONE_YET 1
#endif

/* ── The guest thread ─────────────────────────────────────────────────── */

static int (*s_guest_main)(void);
static volatile int s_guest_rc;
static Uint32 s_guest_done_event;

static void *guest_thread(void *arg)
{
    SDL_Event ev;

    (void)arg;
    s_guest_rc = s_guest_main();
    memset(&ev, 0, sizeof ev);
    ev.type = s_guest_done_event;
    SDL_PushEvent(&ev);
    return NULL;
}

static int start_guest(void)
{
    pthread_attr_t attr;
    pthread_t th;
    struct rlimit rl;
    size_t stack = 64u << 20;
    int rc;

    /* At least the main thread's stack (8 MB by default), and generously
     * more: lifted code recurses on the native stack. */
    if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY
            && rl.rlim_cur > stack)
        stack = (size_t)rl.rlim_cur;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack);
#if defined(__APPLE__)
    /* Same scheduling class as the thread it replaces, so moving the guest
     * off the main thread does not move it to the efficiency cores. */
    {
        qos_class_t qos;
        int rel;
        if (pthread_get_qos_class_np(pthread_self(), &qos, &rel) == 0
                && qos != QOS_CLASS_UNSPECIFIED)
            pthread_attr_set_qos_class_np(&attr, qos, rel);
    }
#endif
    rc = pthread_create(&th, &attr, guest_thread, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0)
        return 0;
    pthread_detach(th);
    return 1;
}

/* ── The window, on the process main thread ───────────────────────────── */

/* SDL's background / foreground events, on the thread that sends them (the
 * Android UI thread), before the main loop sees them. */
static int SDLCALL lifecycle_watch(void *user, SDL_Event *ev)
{
    (void)user;
    if (ev->type == SDL_APP_WILLENTERBACKGROUND)
        recomp_lifecycle_pause();
    else if (ev->type == SDL_APP_DIDENTERFOREGROUND)
        recomp_lifecycle_resume();
    return 1;
}

static int env_on(const char *name)
{
    const char *e = getenv(name);
    return e && *e && *e != '0';
}

/* ── Picture shaders (XBA-203, present_shaders.h) ─────────────────────────
 * The console window's presets (none, sharp, scanlines, crt; lcd is a handheld's grid and draws none here), from
 * RECOMP_XBOX_SHADER, else RECOMP_SHADER, with the same strength / curvature keys. The pass draws the shown frame
 * (a flip, a GPU back end's frame or a frame generation entry) into the letterboxed rectangle at the output
 * resolution, inside SDL's renderer: its GL context (the opengles2 or opengl renderer; GLSL ES 1.00 / 1.20), its
 * texture (SDL_GL_BindTexture), every cached state put back; the perf overlay is drawn over it by SDL. Any other
 * renderer, a texture SDL pads, or a shader that does not build: the plain copy, logged once. Main thread only,
 * except s_xshader_active (read by xbox_FramebufferWindowShaderActive). */
static HostShader s_xshader = { HOST_SHADER_NONE, -1, 0, 0 };
static PresentShaderProg s_xsprog[HOST_SHADER_COUNT];
static int s_xshader_glsl = -1;          /* PRESENT_GLSL_ES100 / _120, -1: no GL renderer */
static int s_xshader_off;                /* the pass cannot run in this renderer (logged) */
static volatile int s_xshader_active;
static SDL_RendererInfo s_xren_info;
static pthread_mutex_t s_capture_mu = PTHREAD_MUTEX_INITIALIZER;
static char s_capture[1024];             /* xbox_FramebufferWindowCapture, under s_capture_mu */

int xbox_FramebufferWindowShaderActive(void)
{
    return __atomic_load_n(&s_xshader_active, __ATOMIC_ACQUIRE);
}

/* A live change (the in-game menu, any thread): the main loop takes it at its next turn (xshader_live_take) and
 * draws the shown frame again, so it is seen at once, also while the game is held. */
static pthread_mutex_t s_xlive_mu = PTHREAD_MUTEX_INITIALIZER;
static int s_xlive_pending;              /* atomic */
static HostShader s_xlive;               /* under s_xlive_mu */

void xbox_FramebufferWindowSetShader(int preset, int strength, int curvature, int ghosting)
{
    pthread_mutex_lock(&s_xlive_mu);
    s_xlive.preset = preset;
    s_xlive.strength = strength;
    s_xlive.curvature = curvature;
    s_xlive.ghosting = ghosting;
    pthread_mutex_unlock(&s_xlive_mu);
    __atomic_store_n(&s_xlive_pending, 1, __ATOMIC_RELEASE);
}

void xbox_FramebufferWindowCapture(const char *path)
{
    pthread_mutex_lock(&s_capture_mu);
    snprintf(s_capture, sizeof s_capture, "%s", path ? path : "");
    pthread_mutex_unlock(&s_capture_mu);
}

/* Before the renderer: the preset from the environment; on macOS (Metal by default) a shader asks SDL for its
 * OpenGL renderer, unless SDL_RENDER_DRIVER says otherwise. */
static void xshader_env(void)
{
    HostShader s;

    present_shader_env("RECOMP_XBOX_SHADER", HOST_SHADER_NONE, &s);
    if (s.preset == HOST_SHADER_LCD) {
        fprintf(stderr, "  [FBWIN] picture shader lcd is a handheld's pixel grid; an Xbox frame is drawn "
                        "without one\n");
        s.preset = HOST_SHADER_NONE;
    }
    s_xshader = present_shader_clamp(&s);
    if (s_xshader.preset == HOST_SHADER_NONE)
        return;
    fprintf(stderr, "  [FBWIN] picture shader %s requested (strength %d, curvature %d)\n",
            present_shader_name(s_xshader.preset), present_shader_strength(&s_xshader), s_xshader.curvature);
#if defined(__APPLE__)
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
#endif
}

/* After the renderer: the dialect its GL context takes, or none. */
static void xshader_renderer(SDL_Renderer *ren)
{
    s_xshader_glsl = -1;
    if (SDL_GetRendererInfo(ren, &s_xren_info) != 0)
        memset(&s_xren_info, 0, sizeof s_xren_info);
    if (s_xren_info.name && !strcmp(s_xren_info.name, "opengles2"))
        s_xshader_glsl = PRESENT_GLSL_ES100;
    else if (s_xren_info.name && !strcmp(s_xren_info.name, "opengl"))
        s_xshader_glsl = PRESENT_GLSL_120;
    if (s_xshader.preset != HOST_SHADER_NONE && s_xshader_glsl < 0) {
        fprintf(stderr, "  [FBWIN] picture shader %s needs SDL's OpenGL renderer (this one is %s); drawing "
                        "without one\n", present_shader_name(s_xshader.preset),
                s_xren_info.name ? s_xren_info.name : "?");
        s_xshader_off = 1;
    }
    __atomic_store_n(&s_xshader_active, s_xshader.preset != HOST_SHADER_NONE && !s_xshader_off
                                        ? s_xshader.preset : HOST_SHADER_NONE, __ATOMIC_RELEASE);
}

/* Takes a live change (xbox_FramebufferWindowSetShader): 1 when one was taken (the frame is to be drawn again). A
 * preset whose shader failed to build stays off; any other is tried again. */
static int xshader_live_take(void)
{
    HostShader s;

    if (!__atomic_load_n(&s_xlive_pending, __ATOMIC_ACQUIRE))
        return 0;
    pthread_mutex_lock(&s_xlive_mu);
    __atomic_store_n(&s_xlive_pending, 0, __ATOMIC_RELAXED);
    s = s_xlive;
    pthread_mutex_unlock(&s_xlive_mu);
    if (s.preset == HOST_SHADER_LCD)
        s.preset = HOST_SHADER_NONE;   /* a handheld's pixel grid: an Xbox frame is drawn without one */
    s_xshader = present_shader_clamp(&s);
    s_xshader_off = s_xshader.preset != HOST_SHADER_NONE && s_xshader_glsl < 0;
    fprintf(stderr, "  [FBWIN] picture shader changed live: %s%s\n", present_shader_name(s_xshader.preset),
            s_xshader_off ? " (needs SDL's OpenGL renderer: drawing without one)" : "");
    __atomic_store_n(&s_xshader_active, s_xshader.preset != HOST_SHADER_NONE && !s_xshader_off
                                        ? s_xshader.preset : HOST_SHADER_NONE, __ATOMIC_RELEASE);
    return 1;
}

/* The preset to draw with, or none (unset, no GL renderer, or its shader failed). */
static int xshader_preset(void)
{
    int p = s_xshader.preset;

    return p > HOST_SHADER_NONE && p < HOST_SHADER_COUNT && !s_xshader_off && !s_xsprog[p].failed
           ? p : HOST_SHADER_NONE;
}

static void xshader_fail(const char *why)
{
    fprintf(stderr, "  [FBWIN] picture shader %s: %s; drawing without one\n",
            present_shader_name(s_xshader.preset), why);
    s_xshader_off = 1;
    __atomic_store_n(&s_xshader_active, HOST_SHADER_NONE, __ATOMIC_RELEASE);
}

/* The format SDL keeps a texture of `format` in (SDL_CreateTexture's choice: the format itself when the renderer
 * has it, else its first one with the same alpha). SDL's opengles2 renderer stores ARGB8888 / RGB888 as RGBA bytes
 * and swaps B and R in its shaders: so must ours. */
static int xshader_swap(uint32_t format)
{
    uint32_t native = 0;

    if (s_xshader_glsl != PRESENT_GLSL_ES100)
        return 0;
    for (Uint32 i = 0; i < s_xren_info.num_texture_formats; i++) {
        if (s_xren_info.texture_formats[i] == format)
            native = format;
    }
    for (Uint32 i = 0; !native && i < s_xren_info.num_texture_formats; i++) {
        uint32_t f = s_xren_info.texture_formats[i];
        if (!SDL_ISPIXELFORMAT_FOURCC(f) && SDL_ISPIXELFORMAT_ALPHA(f) == SDL_ISPIXELFORMAT_ALPHA(format))
            native = f;
    }
    return native == SDL_PIXELFORMAT_ARGB8888 || native == SDL_PIXELFORMAT_RGB888;
}

/* Draws tex (tw x th, `format`, aspect_pack class) through the preset into the rectangle rw x rh centred in the
 * ow x oh drawable, after the clear SDL has queued. Returns 0 when the caller is to copy it as usual. */
static int xshader_draw(SDL_Renderer *ren, SDL_Texture *tex, uint32_t tw, uint32_t th, uint32_t format,
                        uint64_t aspect, int ow, int oh, uint32_t rw, uint32_t rh)
{
    int p = xshader_preset();
    float sx = 0, sy = 0;
    uint32_t y0 = 0, rows = th;
    int cells;
    unsigned err;

    if (p == HOST_SHADER_NONE || !tex || !tw || !th || !rw || !rh)
        return 0;
    if (aspect_cls(aspect) == ASPECT_ZOOM && aspect_y0(aspect) + aspect_rows(aspect) <= th && aspect_rows(aspect)) {
        y0 = aspect_y0(aspect);
        rows = aspect_rows(aspect);
    }
    SDL_RenderFlush(ren);   /* the clear first; SDL's context is current from here */
    if (SDL_GL_BindTexture(tex, &sx, &sy) != 0) {
        xshader_fail("SDL cannot bind its texture");
        return 0;
    }
    if (sx != 1.0f || sy != 1.0f) {
        SDL_GL_UnbindTexture(tex);
        xshader_fail("SDL pads the texture (no NPOT textures)");
        return 0;
    }
    if (!present_shader_build(&s_xsprog[p], (PresentGlsl)s_xshader_glsl, p, "[FBWIN]")) {
        SDL_GL_UnbindTexture(tex);
        __atomic_store_n(&s_xshader_active, HOST_SHADER_NONE, __ATOMIC_RELEASE);
        return 0;
    }
    cells = present_shader_xbox_cells((int)th);
    {
        PresentShaderParams pp = {
            .src_w = (float)tw, .src_h = (float)th,
            /* The viewport as if the whole texture filled it: a ZOOM crop shows rows of it over rh. */
            .out_w = (float)rw, .out_h = (float)rh * (float)th / (float)rows,
            .cells_w = (float)tw * (float)cells / (float)th, .cells_h = (float)cells,
            .rect = { 0.0f, (float)y0 / (float)th, 1.0f, (float)rows / (float)th },
            .flip = 1, .swap = xshader_swap(format),
            .strength = present_shader_strength(&s_xshader), .curvature = s_xshader.curvature,
        };
        err = present_shader_draw_legacy(&s_xsprog[p], &pp, (ow - (int)rw) / 2, (oh - (int)rh) / 2,
                                         (int)rw, (int)rh);
    }
    SDL_GL_UnbindTexture(tex);
    if (err) {
        static int logged;
        if (!logged) {
            logged = 1;
            fprintf(stderr, "  [FBWIN] picture shader %s: GL error 0x%04x at its draw\n", present_shader_name(p),
                    err);
        }
    }
    return 1;
}

/* xbox_FramebufferWindowCapture: the drawable as drawn, before the present. */
static void capture_drawable(SDL_Renderer *ren)
{
    char path[sizeof s_capture];
    int ow = 0, oh = 0;
    unsigned char *rgb;
    FILE *f;

    pthread_mutex_lock(&s_capture_mu);
    memcpy(path, s_capture, sizeof path);
    s_capture[0] = 0;
    pthread_mutex_unlock(&s_capture_mu);
    if (!path[0])
        return;
    if (s_xshader_glsl < 0 || SDL_GetRendererOutputSize(ren, &ow, &oh) != 0 || ow <= 0 || oh <= 0) {
        fprintf(stderr, "  [FBWIN] capture: needs SDL's OpenGL renderer\n");
        return;
    }
    SDL_RenderFlush(ren);
    rgb = malloc((size_t)ow * oh * 3);
    if (!rgb || !present_shader_read_rgb(ow, oh, rgb)) {
        free(rgb);
        return;
    }
    {
        /* Written aside, then renamed: a reader never sees half a file. */
        char tmp[sizeof path + 8];
        snprintf(tmp, sizeof tmp, "%s.part", path);
        f = fopen(tmp, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", ow, oh);
            fwrite(rgb, 3, (size_t)ow * oh, f);
            fclose(f);
            if (rename(tmp, path) != 0)
                remove(tmp);
        } else
            fprintf(stderr, "  [FBWIN] capture: cannot write %s\n", path);
    }
    free(rgb);
}

int xbox_HostWindowMain(int (*guest_main)(void))
{
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex = NULL;
    uint32_t tex_w = 0, tex_h = 0, tex_format = 0;
    int tex_linear = 0;
    uint64_t tex_aspect = 0;            /* the shown frame's class (aspect_pack) */
    int log_cls = ASPECT_NORMAL;        /* the renderer's logical size's class */
    uint64_t shown_seq = 0, t_title = 0, flips_title = 0, t_overlay = 0;
    const char *e;
    int vsync, scale, win_w, quit = 0, guest_done = 0, dirty = 1, reupload = 0;
    int stay_after_guest = 0;   /* Android: the window outlives the guest */
    uint64_t back_at = 0;
#if defined(XBOXRECOMP_LSFG)
    uint64_t fg_next_due = 0, t_fg_log = 0;
    int fg_any = 0, fg_off_seen = 0;
#endif

    /* Ctrl-C and SIGTERM keep their default meaning; SDL would turn them
     * into an SDL_QUIT that a headless run never reads. Set before the
     * headless return: audio and input start SDL subsystems there too. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    /* The video overlay's scan-out, windowed or not (vblank hook, dumps). */
    pvideo_install();
#if defined(XBOXRECOMP_HAVE_GPU)
    /* RECOMP_GPU=1 (the default on Android): the GL back end draws instead
     * of the CPU rasteriser.
     * Before the guest starts, windowed or not; its context lives on the
     * executor thread, frames still reach this window through guest
     * memory (src/gpu/nv2a_gl.c). */
    nv2a_gl_install_from_env();
#endif
    if (!env_on("RECOMP_FB_WINDOW") || env_on("RECOMP_HEADLESS")) {
#if defined(XBOXRECOMP_LSFG)
        fg_start(0);
#endif
        return guest_main();
    }

    e = getenv("RECOMP_PRESENT_VSYNC");
    vsync = !e || env_on("RECOMP_PRESENT_VSYNC");
    e = getenv("RECOMP_WINDOW_SCALE");
    scale = e ? atoi(e) : 2;
    if (scale < 1 || scale > 8)
        scale = 2;
    /* RECOMP_ASPECT=16:9: a 16:9 window (854 x 480 per scale step), so the
     * wide frames fill it; the 4:3 ones get side bars. */
    win_w = (int)(xbox_AspectSetting() == ASPECT_SETTING_16_9 ? 854u : FB_W) * scale;

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    xshader_env();
#ifdef __ANDROID__
    /* Back is trapped. In the app ShellActivity takes it first (it opens the
     * in-game menu, docs/android/app-shell.md « In-game menu »), so the
     * Back-twice path below only runs where nothing above SDL takes it. */
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
#endif
    if (SDL_Init(FBWIN_SDL_SUBSYSTEMS) != 0) {
        fprintf(stderr, "  [FBWIN] SDL video unavailable (%s); running headless\n",
                SDL_GetError());
        return guest_main();
    }
    /* Honor the same fullscreen environment setting as the Windows presenter. */
    Uint32 window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (env_on("RECOMP_FULLSCREEN"))
        window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    win = SDL_CreateWindow(s_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           win_w, (int)FB_H * scale,
                           window_flags);
    ren = win ? SDL_CreateRenderer(win, -1,
                                   vsync ? SDL_RENDERER_PRESENTVSYNC : 0)
              : NULL;
    if (!ren) {
        fprintf(stderr, "  [FBWIN] SDL window/renderer failed (%s); running headless\n",
                SDL_GetError());
        if (win)
            SDL_DestroyWindow(win);
        SDL_QuitSubSystem(FBWIN_SDL_SUBSYSTEMS);
        return guest_main();
    }
    SDL_RenderSetLogicalSize(ren, (int)FB_W, (int)FB_H);
    {
        SDL_RendererInfo ri;
        if (SDL_GetRendererInfo(ren, &ri) == 0)
            fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u, SDL %s renderer,"
                    " vsync %s); guest runs on a pthread\n",
                    FB_W, FB_H, ri.name, vsync ? "on" : "off");
    }
    xshader_renderer(ren);
#if defined(XBOXRECOMP_HAVE_GPU)
    {
        /* The drawable's real size (HiDPI pixels), for the GL back end's
         * RECOMP_GPU_SCALE=auto; read on the executor thread after the
         * guest starts. Headless runs never set it: scale 1, no host
         * frames. */
        int ow = 0, oh = 0;
        if (SDL_GetRendererOutputSize(ren, &ow, &oh) == 0 && ow > 0 && oh > 0)
            nv2a_gl_set_display_size((uint32_t)ow, (uint32_t)oh);
    }
#endif

    SDL_AddEventWatch(lifecycle_watch, NULL);

    s_guest_done_event = SDL_RegisterEvents(2);
    if (s_guest_done_event == (Uint32)-1) {
        fprintf(stderr, "  [FBWIN] SDL_RegisterEvents failed; running headless\n");
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_QuitSubSystem(FBWIN_SDL_SUBSYSTEMS);
        return guest_main();
    }
    s_frame_event = s_guest_done_event + 1;
#ifdef __ANDROID__
    stay_after_guest = 1;
#endif
    s_guest_main = guest_main;
    /* This thread pumps SDL events from here on, which also updates the
     * game controllers; the input layer then only reads them. */
    xbox_InputSetMainPump(1);
    xbox_InputSdlInit();
    display_sync_start(report_display_hz(win), vsync);
#if defined(XBOXRECOMP_LSFG)
    fg_report_display_rate(win);
    fg_start(1);
#endif
    s_running = 1;
    if (!start_guest()) {
        fprintf(stderr, "  [FBWIN] cannot start the guest thread; running headless\n");
        s_running = 0;
#if defined(XBOXRECOMP_LSFG)
        if (s_fg_on) {
            framegen_pacer_stop();
            s_fg_on = s_fg_window = 0;
        }
#endif
        xbox_InputSetMainPump(0);
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_QuitSubSystem(FBWIN_SDL_SUBSYSTEMS);
        return guest_main();
    }

    while (!quit && !guest_done) {
        SDL_Event ev;
        uint64_t seq, now, frame_pub = 0;
        const uint8_t *px = NULL;
        uint32_t fw = 0, fh = 0, pitch = 0, format = 0;
        uint64_t aspect = 0;
        int timeout = shown_seq ? 100 : 16;

#if defined(XBOXRECOMP_LSFG)
        if (s_fg_on && __atomic_load_n(&s_fg_stop_req, __ATOMIC_SEQ_CST)) {
            /* A present that passed fg_feeding before the request may still
             * be submitting: let it finish first. */
            while (__atomic_load_n(&s_in_present, __ATOMIC_SEQ_CST))
                sched_yield();
            fg_log_stats();
            framegen_pacer_stop();
            fg_fifo_clear();
            s_fg_on = s_fg_window = 0;
            __atomic_store_n(&s_fg_wait, 0, __ATOMIC_RELEASE);
            perf_overlay_set_framegen(2, 0, 0, 0);     /* "LSFG OFF" from now on */
            fg_any = 0;
            fg_next_due = 0;
            dirty = 1;
        }
        if (fg_any && !shown_seq)
            timeout = 100;
        if (fg_next_due && s_fg_on) {
            /* Sleep until the next frame generation entry is due (in whole
             * ms, rounded down; fg_take_due counts the last ms as due). */
            uint64_t t = now_ns();
            uint64_t ms = fg_next_due > t ? (fg_next_due - t) / 1000000ull : 0;
            if (ms < (uint64_t)timeout)
                timeout = (int)ms;
        }
#endif
        /* Before the first flip, poll at the display rate: the window then
         * reads the display surface itself. After, the flip's event wakes
         * the loop. */
        if (SDL_WaitEventTimeout(&ev, timeout)) {
            do {
                /* Keys for the pad stand-in, and the window events that
                 * release them; it only reads the event. */
                xbox_InputSdlEvent(&ev);
                if (ev.type == SDL_QUIT)
                    quit = 1;
                else if (ev.type == s_guest_done_event) {
                    if (stay_after_guest) {
                        fprintf(stderr, "  [FBWIN] guest returned (%d); the window stays\n",
                                s_guest_rc);
#ifdef __ANDROID__
                        char done_msg[96];
                        SDL_snprintf(done_msg, sizeof done_msg, TEXT_GUEST_DONE, s_guest_rc);
                        SDL_AndroidShowToast(done_msg, 1, -1, 0, 0);
#endif
                    } else
                        guest_done = 1;
                }
                else if (ev.type == s_frame_event)
                    __atomic_store_n(&s_wake_pending, 0, __ATOMIC_RELEASE);
                else if (ev.type == SDL_WINDOWEVENT
                         && (ev.window.event == SDL_WINDOWEVENT_EXPOSED
                             || ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED))
                    dirty = 1;
                else if (ev.type == SDL_RENDER_TARGETS_RESET)
                    dirty = 1;
                else if (ev.type == SDL_RENDER_DEVICE_RESET) {
                    /* GL context lost (some Android devices, on resume): the
                     * texture is gone; upload the frame shown again. */
                    if (tex)
                        SDL_DestroyTexture(tex);
                    tex = NULL;
                    perf_overlay_reset();
                    for (int i = 0; i < HOST_SHADER_COUNT; i++)
                        present_shader_release(&s_xsprog[i], 1);   /* went with the context */
                    reupload = 1;
                    dirty = 1;
                } else if (ev.type == SDL_KEYDOWN && !ev.key.repeat
                           && ev.key.keysym.scancode == SDL_SCANCODE_AC_BACK) {
                    uint64_t t = now_ms();
                    if (back_at && t - back_at <= BACK_QUIT_MS) {
                        fprintf(stderr, "  [FBWIN] Back pressed twice, quitting\n");
                        quit = 1;
                    } else {
                        back_at = t;
                        fprintf(stderr, "  [FBWIN] QUIT_PROMPT shown\n");
#ifdef __ANDROID__
                        SDL_AndroidShowToast(TEXT_QUIT_PROMPT, 0, -1, 0, 0);
#endif
                    }
                }
            } while (SDL_PollEvent(&ev));
        }

#if defined(XBOXRECOMP_LSFG)
        if (s_fg_on) {
            if (framegen_pacer_active()) {
                if (fg_take_due(now_ns(), &fg_next_due)) {
                    px = s_fg_front.px;
                    fw = s_fg_front.w;
                    fh = s_fg_front.h;
                    pitch = fw * 4u;
                    format = SDL_PIXELFORMAT_XRGB8888;   /* bytes B, G, R, X */
                    aspect = s_fg_front.aspect;
                    fg_any = 1;
                } else if (reupload && fg_any) {
                    px = s_fg_front.px;                  /* only this thread swaps it */
                    fw = s_fg_front.w;
                    fh = s_fg_front.h;
                    pitch = fw * 4u;
                    format = SDL_PIXELFORMAT_XRGB8888;
                    aspect = s_fg_front.aspect;
                }
            } else if (!fg_off_seen) {
                /* Fallen back: the flips fill the slots again from the
                 * next one. */
                fg_off_seen = 1;
                fg_next_due = 0;
                fg_fifo_clear();
                fprintf(stderr, "  [FBWIN] frame generation off (%s); normal present\n",
                        framegen_pacer_off_reason());
#ifdef __ANDROID__
                {
                    char msg[224];
                    SDL_snprintf(msg, sizeof msg, "Frame generation off: %s",
                                 framegen_pacer_off_reason());
                    SDL_AndroidShowToast(msg, 1, -1, 0, 0);
                }
#endif
            }
        }
#endif
        pthread_mutex_lock(&s_lock);
        seq = s_ready_seq;
        if (seq != shown_seq) {
            int t = s_front;
            s_front = s_ready;
            s_ready = t;
        }
        pthread_mutex_unlock(&s_lock);

        if (px) {
            /* A frame generation entry, set above. */
        } else if (seq != shown_seq || (reupload && shown_seq)) {
            if (seq != shown_seq)
                frame_pub = s_slot[s_front].pub_ns;
            shown_seq = seq;
            px = s_slot[s_front].px;
            fw = s_slot[s_front].w;
            fh = s_slot[s_front].h;
            pitch = s_slot[s_front].pitch;
            format = s_slot[s_front].format;
            aspect = s_slot[s_front].aspect;
        } else if (!shown_seq && FG_NONE_YET && s_fb_va && s_fb_pitch) {
            /* No flip yet: the display surface, as the GDI window reads it. */
            uint32_t bpp = s_fb_pitch / FB_W;
            fw = FB_W;
            fh = FB_H;
            format = guest_format(bpp, bpp == 2 && s_fb_x555);
            pitch = s_fb_pitch;
            if (bpp == 2 || bpp == 4)
                px = (const uint8_t *)((uintptr_t)s_fb_va + xbox_GetMemoryOffset());
        }
        if (px && fw && fh) {
            if (!tex || fw != tex_w || fh != tex_h || format != tex_format) {
                if (tex)
                    SDL_DestroyTexture(tex);
                tex = SDL_CreateTexture(ren, format, SDL_TEXTUREACCESS_STREAMING,
                                        (int)fw, (int)fh);
                if (tex && (fw != tex_w || fh != tex_h))
                    fprintf(stderr, "  [FBWIN] texture %ux%u (%s)\n", fw, fh,
                            SDL_GetPixelFormatName(format));
                tex_w = tex ? fw : 0;
                tex_h = tex ? fh : 0;
                tex_format = format;
                tex_linear = -1;            /* chosen below */
            }
            if (tex)
                SDL_UpdateTexture(tex, NULL, px, (int)pitch);
            tex_aspect = aspect;
            dirty = 1;
            reupload = 0;
        }
        if (xshader_live_take()) {
            dirty = 1;
            tex_linear = -1;   /* sharp bilinear samples linearly: the filter is chosen again */
        }
        if (aspect_cls(tex_aspect) != log_cls
                && (aspect_cls(tex_aspect) == ASPECT_NORMAL || log_cls == ASPECT_NORMAL)) {
            /* 4:3 <-> 16:9: the renderer letterboxes into the other shape
             * (the perf overlay follows the logical height). */
            if (aspect_cls(tex_aspect) == ASPECT_NORMAL)
                SDL_RenderSetLogicalSize(ren, (int)FB_W, (int)FB_H);
            else
                SDL_RenderSetLogicalSize(ren, 1280, 720);
            dirty = 1;
        }
        log_cls = aspect_cls(tex_aspect);
        if (dirty && tex) {
            /* A 4:3 frame at the guest's 640x480 keeps the window's nearest
             * filter; any other (a GPU back end's internal resolution, a
             * frame widened to 16:9 or a ZOOM crop) is filtered linearly
             * unless what is shown of it maps 1:1 onto the letterboxed
             * rectangle of the current logical aspect it is shown in. */
            int linear = 0, cls = aspect_cls(tex_aspect);
            if (tex_w != FB_W || tex_h != FB_H || cls != ASPECT_NORMAL) {
                int ow = 0, oh = 0;
                uint32_t rw, rh, lw = cls == ASPECT_NORMAL ? FB_W : 16u;
                uint32_t lh = cls == ASPECT_NORMAL ? FB_H : 9u;
                uint32_t sh = cls == ASPECT_ZOOM ? aspect_rows(tex_aspect) : tex_h;
                SDL_GetRendererOutputSize(ren, &ow, &oh);
                if ((uint64_t)ow * lh <= (uint64_t)oh * lw) {
                    rw = (uint32_t)ow;
                    rh = (uint32_t)((uint64_t)ow * lh / lw);
                } else {
                    rh = (uint32_t)oh;
                    rw = (uint32_t)((uint64_t)oh * lw / lh);
                }
                linear = tex_w != rw || sh != rh;
            }
            /* Sharp bilinear blends texels itself: it samples linearly whatever the scale. */
            if (xshader_preset() == HOST_SHADER_SHARP)
                linear = 1;
            if (linear != tex_linear) {
                SDL_SetTextureScaleMode(tex, linear ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
                tex_linear = linear;
            }
        }
        if (dirty) {
            int shaded = 0;
            dirty = 0;
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            if (tex && xshader_preset() != HOST_SHADER_NONE) {
                /* The letterboxed rectangle SDL's logical size gives: 640x480, or 16:9. */
                int ow = 0, oh = 0, cls = aspect_cls(tex_aspect);
                uint32_t rw, rh, lw = cls == ASPECT_NORMAL ? FB_W : 16u, lh = cls == ASPECT_NORMAL ? FB_H : 9u;
                SDL_GetRendererOutputSize(ren, &ow, &oh);
                if ((uint64_t)ow * lh <= (uint64_t)oh * lw) {
                    rw = (uint32_t)ow;
                    rh = (uint32_t)((uint64_t)ow * lh / lw);
                } else {
                    rh = (uint32_t)oh;
                    rw = (uint32_t)((uint64_t)oh * lw / lh);
                }
                shaded = xshader_draw(ren, tex, tex_w, tex_h, tex_format, tex_aspect, ow, oh, rw, rh);
            }
            if (shaded) {
                /* Drawn through the picture shader. */
            } else if (tex && aspect_cls(tex_aspect) == ASPECT_ZOOM
                    && aspect_y0(tex_aspect) + aspect_rows(tex_aspect) <= tex_h) {
                SDL_Rect src = { 0, (int)aspect_y0(tex_aspect), (int)tex_w,
                                 (int)aspect_rows(tex_aspect) };
                SDL_RenderCopy(ren, tex, &src, NULL);
            } else if (tex)
                SDL_RenderCopy(ren, tex, NULL, NULL);
            perf_overlay_draw(ren);
            if (tex)
                capture_drawable(ren);   /* a frame, not the clear of a window event before the first */
            {
                uint64_t t_call = now_ns();
                SDL_RenderPresent(ren);
                display_sync_present(frame_pub, t_call, now_ns());
                frame_pub = 0;
            }
            /* XBA-29: how even the presents are (scenario 3: sd < 2 ms). */
            {
                static FrameTimes ft;
                uint32_t n;
                double mean, sd, worst;
                uint64_t t = now_ns();
                frame_times_add(&ft, t);
                if (frame_times_take(&ft, t, 10000000000ull, &n, &mean, &sd, &worst))
                    fprintf(stderr, "  [FBWIN] frame times over 10 s: %u presents, mean %.2f ms "
                                    "(%.1f fps), sd %.2f ms, worst %.1f ms\n",
                            n, mean, mean > 0 ? 1000.0 / mean : 0.0, sd, worst);
            }
        }

        now = now_ms();
#if defined(XBOXRECOMP_LSFG)
        if (s_fg_on) {
            uint32_t real, shown, gen;
            framegen_pacer_stats(&real, &shown, &gen);
            perf_overlay_set_framegen(__atomic_load_n(&s_fg_wait, __ATOMIC_ACQUIRE) ? 3
                                      : framegen_pacer_active() ? 1 : 2, real, shown, gen);
            if (now - t_fg_log >= 5000) {
                if (t_fg_log && !fg_off_seen)
                    fg_log_stats();
                t_fg_log = now;
            }
        }
#endif
        if (perf_overlay_enabled() && now - t_overlay >= 500) {
            char st[192];
            const char *status = window_status(st, sizeof st);
            if (perf_overlay_tick(now, __atomic_load_n(&s_flips, __ATOMIC_RELAXED),
                                  __atomic_load_n(&s_frame_draws, __ATOMIC_RELAXED),
                                  strcmp(status, "Xbox Recomp") ? status : NULL))
                dirty = 1;
            t_overlay = now;
        }
        if (now - t_title >= 1000) {
            uint64_t f = __atomic_load_n(&s_flips, __ATOMIC_RELAXED);
            char tb[320], st[192];
            snprintf(tb, sizeof tb, "%s | FPS: %.1f | draws: %u", window_status(st, sizeof st),
                     t_title ? (double)(f - flips_title) * 1000.0 / (double)(now - t_title)
                             : 0.0,
                     (unsigned)__atomic_load_n(&s_frame_draws, __ATOMIC_RELAXED));
            SDL_SetWindowTitle(win, tb);
            __atomic_store_n(&s_fps_centi, t_title ? (uint32_t)((f - flips_title) * 100000u
                                                                / (now - t_title)) : 0u,
                             __ATOMIC_RELAXED);
            t_title = now;
            flips_title = f;
        }
    }

    /* No present may be mid-publish (it pushes an SDL event) once SDL goes.
     * A flip held by the lifecycle is let go first: it then sees s_running
     * clear and returns. */
    SDL_DelEventWatch(lifecycle_watch, NULL);
#ifdef __ANDROID__
    display_vsync_android_stop();
#endif
    __atomic_store_n(&s_running, 0, __ATOMIC_SEQ_CST);
    recomp_lifecycle_resume_for(RECOMP_HOLD_ALL);   /* background and menu */
    while (__atomic_load_n(&s_in_present, __ATOMIC_SEQ_CST))
        sched_yield();
#if defined(XBOXRECOMP_LSFG)
    /* The sink wakes this loop through SDL: the pacer goes before SDL. */
    if (s_fg_on) {
        fg_log_stats();
        framegen_pacer_stop();
        s_fg_on = 0;
    }
#endif
    fprintf(stderr, "  [FBWIN] %s\n", quit ? "window closed" : "guest returned");
    fflush(stdout);
    fflush(stderr);

    /* Window and renderer go first, on this thread as AppKit wants. SDL_Quit
     * is only safe once nothing else uses SDL: the USB and guest threads read
     * the game controllers, so with one open SDL stays up and exit()
     * reclaims it. The main-pump flag stays set, so those threads do not
     * start calling SDL_GameControllerUpdate while SDL goes down. */
    if (tex)
        SDL_DestroyTexture(tex);
    perf_overlay_reset();
    for (int i = 0; i < HOST_SHADER_COUNT; i++)
        present_shader_release(&s_xsprog[i], 1);   /* go with the renderer's context */
    __atomic_store_n(&s_xshader_active, HOST_SHADER_NONE, __ATOMIC_RELEASE);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    if (!xbox_InputAnyOpen())
        SDL_Quit();
    else
        SDL_QuitSubSystem(FBWIN_SDL_SUBSYSTEMS);
    if (quit)
        recomp_quit(0);     /* exit(0); on Android flush, drain, _exit(0) */
    return s_guest_rc;
}

#endif /* !_WIN32 */
