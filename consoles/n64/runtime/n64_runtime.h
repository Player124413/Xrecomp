/* n64_runtime -- the Nintendo 64 runtime (XBA-146, XBA-174): the machine of n64_core.h played on the host layer
 * (xbr_host: host_window, host_pad, host_audio, recomp_lifecycle). The app shell's loader calls n64_runtime_start;
 * the desktop's n64_play calls n64_runtime_run. consoles/README.md « Playing an N64 game », docs/android/app-shell.md
 * « Nintendo 64 games », Plane « Spec · Other consoles ». */
#ifndef XBR_N64_RUNTIME_H
#define XBR_N64_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define N64_RUNTIME_VERSION "0.1.0"

/* "n64_runtime <version>". */
const char *n64_runtime_version(void);

/* The accessory in each controller's slot (n64_runtime_opts.pak). */
enum { N64_RT_PAK_DEFAULT = -1, N64_RT_PAK_NONE = 0, N64_RT_PAK_CONTROLLER = 1, N64_RT_PAK_RUMBLE = 2 };

/* What n64_runtime_run plays and how. Zeroed = defaults (n64_runtime_opts_init). */
typedef struct n64_runtime_opts {
    /* The cartridge: a ROM file in any byte order (.z64, .v64, .n64: n64_rom_detect), or "fd:N" holding one (a
     * descriptor the app shell opened from a document; read from offset 0, left open). No BIOS: the PIF is HLE. */
    const char *rom;
    /* The game's key (NSME-635A2BFF, recomp_pack_rom_identify); NULL or "": n64_runtime_fallback_key. */
    const char *title_key;
    /* Saves: <save_dir>/n64-<KEY>/game.eep (EEPROM 4K: 512 B, 16K: 2 KB), game.sra (SRAM, 32 KB), game.fla
     * (FlashRAM, 128 KB) and pak1.mpk .. pak4.mpk (controller paks, 32 KB): raw images in the cartridge's byte order,
     * the files other emulators (mupen64plus, ares) read and write. Read before the first frame (a cartridge save
     * also fixes the save type); written a second after the game's last write (at most five seconds after its
     * first), when the app pauses and at exit; only an image that changed is written. NULL: no saves. */
    const char *save_dir;
    /* Stop after this many frames (VI interrupts; 0: until the player quits). */
    uint64_t frames;
    /* 1: no window (frames still run, pads and audio still open); also when host_window_headless(). */
    int headless;
    /* 1: no pacing, as fast as the host runs (tests). */
    int unpaced;
    /* Write the last displayed picture (its own size) as a binary PPM here at exit. */
    const char *screenshot;
    /* Capture the window's drawable (the real present path, host_window_capture) at this frame, to capture_path. */
    uint64_t capture_frame;
    const char *capture_path;
    /* Buttons held on controller 1 for the whole run, on top of the pads (N64_BTN_*, 1 = pressed; tests). */
    uint16_t buttons;
    /* Window scaling: -1 = $RECOMP_N64_SCALE, else $RECOMP_SCALE ("aspect", "integer" or "stretch", default aspect;
     * host_window_env_scale); else HOST_SCALE_*. Filter: -1 = $RECOMP_N64_FILTER, else $RECOMP_FILTER ("linear" or
     * "nearest", default linear: the VI's 256..640-wide pictures are stretched to 4:3); else HOST_FILTER_*. */
    int scale, filter;
    /* The VI's horizontal guard band (the black first 8 and last 7 VI pixels of a line, n64_vi.c): 1 = not shown, 8 VI
     * pixels cut from each side of the picture (4 columns of a 320-wide one) and the rest shown at 4:3 (2.5 %
     * wider), as a television hid it in its overscan; 0 = the whole VI picture, thin black strips left and right.
     * -1 = $RECOMP_N64_CROP_EDGES ("0" shows the edges; default 1). Only the window: --screenshot stays the whole
     * picture. */
    int crop_edges;
    /* The accessory in the controllers: N64_RT_PAK_DEFAULT = $RECOMP_N64_PAK ("controller", "rumble" or "none";
     * default controller: the controller pak, saved as pakN.mpk); a rumble pak drives the host pad's rumble. */
    int pak;
    /* 1: every host pad drives controller 1 (a handheld's own controls and an external pad alike); 2..4: host pad N
     * -> controller N. -1 = $RECOMP_N64_PLAYERS (default 1). */
    int players;
    /* RDP scanline-band threads (n64_rdp_set_threads): -1 = $RECOMP_N64_RDP_THREADS, else 2 on Android and the core's
     * default elsewhere ($XBR_N64_RDP_THREADS, else 1); 1..N64_RDP_MAX_THREADS. */
    int rdp_threads;
    /* TV type: -1 from the ROM's region (default), 0 NTSC (60 Hz), 1 PAL (50 Hz). */
    int force_pal;
    /* 1: copy the game's IS-Viewer debug text (libdragon debugf, osSyncPrintf) to stdout, "n64: tty: ..." lines; -1 =
     * $RECOMP_N64_TTY (default off). */
    int tty;
    /* An N64 title pack's library (XBA-174, n64_pack.h): taken when its pack.json (beside it) says it is this game's
     * (title key, the ROM's big-endian SHA-256), this shell's and this host's and its table loads, else the game runs
     * interpreted (a log line says why). NULL or "": the interpreter alone. */
    const char *pack_lib;
    /* At exit (and from the in-game menu's Quit), the code the interpreter had to run while a pack was attached
     * (n64_recomp_write_missed: address, hash, bytes) is appended to this file without repeats, which the next
     * Prepare hands to the lifter (the discovery loop). NULL: not written. */
    const char *discovered_path;
    /* Frame skipping when the host cannot keep up (the software RDP on a phone): N64_RT_FRAMESKIP_ENV (-2) =
     * $RECOMP_N64_FRAMESKIP ("auto", default; "0" never; "N" shows one frame in N + 1), N64_RT_FRAMESKIP_AUTO (-1),
     * 0, or N. A skipped frame still runs whole (CPU, RSP, RDP drawing into RDRAM: the game may read it back) and
     * makes its audio; only the VI picture (n64_display) and the present are skipped. */
    int frameskip;
} n64_runtime_opts;

void n64_runtime_opts_init(n64_runtime_opts *o);

/* How the run shows its picture: o->scale / o->filter when set (>= 0), else $RECOMP_N64_SCALE / _FILTER, else the
 * shared $RECOMP_SCALE / $RECOMP_FILTER (host_window_env_scale / _filter), else aspect and linear. o may be NULL. */
void n64_runtime_display(const n64_runtime_opts *o, int *scale, int *filter);

/* Whether the window hides the VI's guard band: o->crop_edges when set (>= 0), else 0 when $RECOMP_N64_CROP_EDGES
 * starts with '0', else 1. o may be NULL. */
int n64_runtime_crop_edges(const n64_runtime_opts *o);

/* The columns cut from each side of a w-wide VI picture when the guard band is hidden: 8 VI pixels of the 640-pixel
 * canvas (4 of 320, 8 of 640), 0 for a picture too narrow to crop. */
int n64_runtime_crop_columns(int w);

/* Plays o->rom until the player quits (window closed, SDL_QUIT) or o->frames ran. Returns 0, or -1 when the ROM cannot
 * be read or the machine cannot start (why on stderr). Logs one "n64: ..." line per step on stdout. */
int n64_runtime_run(const n64_runtime_opts *o);

/* Where n64_runtime_start keeps saves (the app shell's <internal files>/save); NULL: none. Copied. */
void n64_runtime_set_save_dir(const char *dir);

/* Where n64_runtime_start writes the discovered code: <dir>/n64-<KEY>/discovered.txt (the app shell's
 * <internal files>/work: the Prepare's work folder of the game, which it reads them from); NULL: not written. */
void n64_runtime_set_work_dir(const char *dir);

/* The app shell's entry point (XBA-147, android/app/src/main/cpp/pack_loader.c): rom is the cartridge (a path or
 * "fd:N"), pack_lib a title pack's library (NULL or "" without a pack: the interpreter alone; a pack that is not this
 * game's: logged, interpreted), title_key the game's key. Plays it with n64_runtime_run (saves in
 * n64_runtime_set_save_dir's dir, discovered code in n64_runtime_set_work_dir's; $RECOMP_N64_FRAMES bounds the run)
 * and returns its result once the player quit. */
int n64_runtime_start(const char *rom, const char *pack_lib, const char *title_key);

/* ---- Pure helpers (tested in consoles/n64/tests) ---- */

/* The N64 buttons (N64_BTN_*, 1 = pressed) for an SDL game controller's state (host_pad.h: HOST_PAD_* buttons, right
 * stick -32768..32767 with +y down, triggers 0..32767). Xbox layout: A -> A, B and X -> B, Y -> C-up, LB -> L,
 * RB -> R, LT or RT past a quarter -> Z, Start -> Start, D-pad -> D-pad, the right stick past half its travel ->
 * the C buttons (up / down / left / right). Opposite directions at once cancel. */
uint16_t n64_runtime_pad_buttons(uint32_t buttons, int rx, int ry, int lt, int rt);
/* The N64 buttons for console_pad targets (bit N64_T_* of console_pad.h), opposite directions cancelled. The game
 * reads its pads through the player's mapping (RECOMP_N64_PAD_MAP, console_pad.h); n64_runtime_pad_buttons is that
 * mapping's defaults, the layout above. */
uint16_t n64_runtime_buttons_of(uint32_t targets);

/* An SDL left stick (-32768..32767 each, +y down) as the N64 stick (+y up), range = the value at full travel on an
 * axis (the real stick's gate is about +-80; 0: 80), dead = the radial dead zone in SDL units (-1: 8 % of the travel).
 * The diagonals reach about 0.85 x range on each axis, as the real stick's octagonal gate does. */
void n64_runtime_stick(int lx, int ly, int range, int dead, int8_t *x, int8_t *y);

/* 0x00RRGGBB pixels (n64_display) to RGBA8 bytes (alpha 255), n pixels. */
void n64_runtime_xrgb_to_rgba(const uint32_t *src, uint8_t *dst, size_t n);

/* A save file of a game: "<save_dir>/n64-<key>/<name>" where name is game.eep (both EEPROM kinds), game.sra,
 * game.fla, or pak1.mpk .. pak4.mpk for N64_SAVE_PAK0..3 (n64_core.h n64_save_kind). Returns 0, or -1 when it does not
 * fit, an argument is missing or the kind is N64_SAVE_NONE. */
int n64_runtime_save_path(const char *save_dir, const char *key, int kind, char *out, size_t out_n);

/* A key for a ROM recomp_pack_rom_identify could not name (the same formula: game code + header CRC1,
 * "NSME-635A2BFF"; any byte order). Returns 0, or -1 when the ROM is shorter than its header or out is too small
 * (16 bytes is enough). */
int n64_runtime_fallback_key(const uint8_t *rom, size_t n, char *out, size_t out_n);

/* The length of a frame of the core in ns, from the CPU cycles it ran (93.75 MHz), held between 1/75 and 1/45 s (a
 * first frame from reset, a mode change). NTSC about 16.7 ms (60 Hz), PAL 20 ms (50 Hz). */
uint64_t n64_runtime_frame_ns(uint64_t cycles);

/* The accessory a value names ("controller" / "pak" / "mempak", "rumble", "none"), or N64_RT_PAK_DEFAULT when it is
 * none of them (NULL too). */
int n64_runtime_pak_parse(const char *v);

enum { N64_RT_FRAMESKIP_ENV = -2, N64_RT_FRAMESKIP_AUTO = -1, N64_RT_FRAMESKIP_MAX = 9 };
/* "auto" (or NULL / "") -> N64_RT_FRAMESKIP_AUTO, "0".."9" -> that, anything else -> N64_RT_FRAMESKIP_AUTO. */
int n64_runtime_frameskip_parse(const char *v);
/* Whether to skip the next frame's picture. mode: N64_RT_FRAMESKIP_AUTO or 0..N64_RT_FRAMESKIP_MAX; behind_ns: how
 * late the frame starts against its deadline (0 or less: on time); audio_low: the host's audio queue is below a
 * quarter of its target (it is about to run dry); *run: frames skipped in a row (updated). Auto skips when the frame
 * is more than half a frame late or the audio is about to run dry, at most 3 in a row (15 fps shown at worst at
 * 60 Hz); a fixed N skips N, shows one. */
int n64_runtime_frameskip(int mode, int64_t behind_ns, uint64_t frame_ns, int audio_low, int *run);
/* The same over new pictures only (the frame loop asks it only for a VI whose picture changed, n64_display_changed;
 * an unchanged VI is neither shown nor skipped and leaves *run alone): auto skips at most one new picture in a row
 * (a skipped picture is still new at the next VI, so it is shown one VI late); a fixed N as above.
 * RECOMP_N64_PRESENT_ALWAYS=1 brings back n64_runtime_frameskip on every VI. */
int n64_runtime_frameskip_picture(int mode, int64_t behind_ns, uint64_t frame_ns, int audio_low, int *run);

/* The performance line (n64: perf ...), logged every 10 s after the frame-rate line and once at exit over the whole
 * run (pauses left out): which configuration ran and where the host's time went. Times are ms per N64 frame: the
 * core's n64_run_frame split into cpu / rsp / rdp / sched (the scheduler and the VI / AI / PI / SI events) by the
 * core's timed frames (n64_perf, one in 8), the RDP's flush waits (inside rdp, or inside vi when the VI asks for
 * them), then the runtime's own stages (idle: the sleep, the frame-generation draws during it included); other is
 * what is left of the wall time (pads, cheats, rewind captures, save files). Host frames are the loop's iterations
 * (one shown or skipped picture; several N64 frames each while fast-forwarding), their length without the sleep. */
typedef struct n64_runtime_perf {
    double seconds;           /* wall time covered */
    uint64_t frames;          /* N64 frames run */
    uint64_t host_frames;     /* host frames */
    double vi_hz;             /* the game's VI rate (the full speed) */
    double cpu_ms, rsp_ms, rdp_ms, sched_ms, rdp_wait_ms;
    /* The RDP's waits split (ms per N64 frame): queue = room in the Vulkan RDP's full command ring (inside rdp), sync
     * = the flushes (SYNC_FULL inside rdp, the VI's inside vi; rdp_wait_ms less the scanout), scanout = the
     * upscaled scanout's GPU wait and read-back (inside vi). */
    double rdp_queue_ms, rdp_sync_ms, rdp_scanout_ms;
    double vi_ms;             /* n64_display (the VI's picture, the upscaled scanout) */
    double present_ms;        /* RGBA conversion and host_window_present_rgba */
    double audio_ms;          /* n64_audio_pull and host_audio_queue */
    double idle_ms;           /* sleeping to the next frame's deadline */
    double other_ms;
    double worst_ms;          /* the longest host frame */
    double over_ms;           /* one VI frame (16.7 ms NTSC, 20 ms PAL) */
    uint64_t over_1, over_2;  /* host frames longer than one VI frame, than two */
    double rsp_instr, rdp_cmds, rdp_syncs; /* per N64 frame: RSP instructions, RDP commands, SYNC_FULL */
    int pack;                 /* a title pack is attached */
    double native_pct;        /* with a pack: the share of the CPU's instructions lifted code ran */
    double interp_instr;      /* with a pack: RDRAM instructions the interpreter ran, per N64 frame */
    uint64_t rehashes, slow_lookups; /* with a pack: entries re-hashed (a page they cover written), cache misses */
    const char *rdp;          /* the RDP back end ("software", "vulkan") */
    int rdp_threads;          /* the software RDP's scanline-band threads */
    int upscale;              /* the RDP's internal resolution factor */
    int rewind;               /* rewind captures on */
    int cpu;                  /* the host core the loop ran on at the end (-1: unknown) */
    long cpu_khz;             /* that core's scaling_cur_freq (0: unreadable) */
    /* The detail line (n64_runtime_perf_detail_format). Pictures: n64_display calls (a new picture made and shown);
     * unchanged: VIs whose picture had not changed since the last one shown (nothing made nor uploaded, the window's
     * texture shown again); skipped: new pictures frame skip dropped. */
    uint64_t pictures, unchanged, skipped;
    uint64_t vi_origin_changes;   /* the game's buffer swaps (n64_guest_activity) */
    uint64_t scanouts;            /* the Vulkan RDP's upscaled scanouts */
    uint64_t scanouts_early;      /* of those, started at the VI_ORIGIN change (n64_rdp_scanout_prefetch) */
    uint64_t queue_waits;         /* times the emulation thread found the Vulkan RDP's command ring full */
    uint64_t stalled_compiles;    /* "Stalled compile" pipelines (5 ms or more) */
    double stalled_compile_ms;
    /* The deferred DP interrupt (n64_core.h n64_set_rdp_defer): SYNC_FULLs deferred, completed at once, completed
     * before the deadline (save states), backoffs; deadlines where the GPU was still drawing, the wait there (ms). */
    uint64_t dp_deferred, dp_now, dp_early, dp_backoffs, dp_waits;
    double dp_wait_ms;
    /* The worst host frame's own breakdown (ms): n64_run_frame (core), n64_display (vi), present, audio, the rest;
     * and the RDP's waits inside it (queue and sync inside core or vi, scanout inside vi). */
    double worst_core_ms, worst_vi_ms, worst_present_ms, worst_audio_ms, worst_other_ms;
    double worst_queue_ms, worst_sync_ms, worst_scanout_ms, worst_deadline_ms;
} n64_runtime_perf;
/* The line's text after "n64: perf ": "speed 25 % (15.0 of 60.0 VI/s); cpu 7.9 rsp 4.1 rdp 5.2 (queue 1.0, sync 2.5,
 * scanout 0.5) sched 0.3
 * vi 1.8 present 0.9 audio 0.1 idle 0.0 other 0.2 ms/frame; worst 41 ms, 212 > 16.7 ms, 30 > 33.4 ms of 150 frames;
 * rsp 1.20 M instr, rdp 3400 cmds, 2.0 full syncs /frame; pack 83 % native, 3.1 k interp instr/frame, 12 rehashes,
 * 40 slow lookups; rdp vulkan, upscale 2x, rewind off; core 6 @ 1.90 GHz" ("no pack", "rdp software, 2 threads",
 * "core ?" / "core 6" when unknown). Returns what snprintf returns. */
int n64_runtime_perf_format(const n64_runtime_perf *p, char *out, size_t n);
/* The detail line's text after "n64: perf detail ", logged right after the perf line: "500 VIs, 167 pictures (16.7/s),
 * 333 unchanged, 0 skipped; 167 VI_ORIGIN changes, 167 scanouts (160 early), 40 full rings; DP 160 deferred (0
 * early, 0 backoffs), 7 at once, 12 waited 3.5 ms (0.02 ms each); 0 stalled compiles (0 ms); worst 40 ms: core 30.1
 * vi 7.2 present 1.8 audio 0.1 other 0.8 (rdp queue 9.0, sync 8.0, deadline 0.2, scanout 3.0)". */
int n64_runtime_perf_detail_format(const n64_runtime_perf *p, char *out, size_t n);
/* The whole-run figures of the last n64_runtime_run (zeroed before the first; tests). */
void n64_runtime_last_perf(n64_runtime_perf *out);

/* The « guest stuck » detector of the frame loop (diagnostics only: it logs, it changes nothing). Fed once per frame
 * with the guest's progress counters (n64_guest_activity) and its fault count (n64_guest_fault_info). Returns
 * N64_RT_STUCK_NONE, or once per episode the reason the guest looks dead: no VI origin change, SI / PI DMA or audio
 * buffer for `limit` frames (an exception storm, a deadlock), or no VI origin change for `limit` frames after a fault
 * exception (a game's own crash screen holds the picture, often one drawn after the fault, while its other threads
 * still poll the controller and play sound). An episode ends when the counters move again (any of them for the first, the VI origin for the second). */
enum { N64_RT_STUCK_NONE = 0, N64_RT_STUCK_IDLE, N64_RT_STUCK_FAULT };
typedef struct n64_runtime_stuck {
    uint64_t vi, other, faults; /* the counters seen last */
    uint64_t vi_still;          /* frames without a VI origin change */
    uint64_t all_still;         /* frames without any change */
    uint64_t fault_age;         /* frames since the fault count last moved (UINT64_MAX: never) */
    int fault_since_vi;         /* a fault exception since (or shortly before) the last VI origin change */
    int reported;               /* N64_RT_STUCK_* reported for this episode */
} n64_runtime_stuck;
void n64_runtime_stuck_init(n64_runtime_stuck *s);
int n64_runtime_stuck_check(n64_runtime_stuck *s, uint64_t vi_origin_changes, uint64_t other_activity,
                            uint64_t faults, uint64_t limit);

#ifdef __cplusplus
}
#endif

#endif /* XBR_N64_RUNTIME_H */
