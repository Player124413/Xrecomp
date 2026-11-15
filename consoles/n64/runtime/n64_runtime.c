/* n64_runtime.c -- see n64_runtime.h (XBA-146, XBA-174).
 *
 * The PS1 runtime's shape (consoles/ps1/runtime/ps1_runtime.c): one thread does everything, frame by frame: pump the
 * window's events (quit, background, pads), read the pads, run one N64 frame (to the next VI interrupt), queue its
 * audio, present the VI's picture (made, converted and uploaded only when it changed: n64_display_changed; the same
 * texture drawn again otherwise, RECOMP_N64_PRESENT_ALWAYS=1 makes one every VI), then sleep to the next frame's deadline. The frame's length comes from the CPU
 * cycles the core ran (its VI follows the game's timing: NTSC 60 Hz, PAL 50 Hz, one field per interrupt when
 * interlaced), so pacing is the N64's own VI rate; absolute deadlines (no drift), nudged by the audio queue once the
 * AI makes sound, a stall of more than three frames resets the clock. While the app is paused (background, the
 * in-game menu: recomp_lifecycle) no frame runs, the saves are written and the window keeps pumping.
 *
 * Picture: n64_display gives the VI's visible area at framebuffer resolution (320 x 240, 640 x 480, 512 x 288 PAL...)
 * as XRGB; it is shown at 4:3 whatever its size (host_window_set_aspect), as a television did, less the VI's black
 * guard band (8 VI pixels each side, RECOMP_N64_CROP_EDGES), which a television hid in its overscan. With the Vulkan
 * RDP upscaling (RECOMP_N64_UPSCALE / RECOMP_UPSCALE, XBA-185) the picture is its scanout, 640 VI pixels x the scale
 * wide, in buffers allocated for that size.
 *
 * Save states and fast-forward (XBA-183, recomp_states.h), the PS1 runtime's way: the run attaches itself while it
 * plays; the in-game menu and the player's hotkeys (XBA-190, recomp_hotkeys.h: by default Back + right / left
 * shoulder quick save / load, Back + right trigger held fast-forwards, F1-F4 / Shift+F1-F4 / Tab on a desktop) save
 * and load between frames under s_mu; fast-forward runs `speed` frames per shown frame, their audio averaged down. A state never writes
 * the save files: after a load the cartridge and paks the game sees are the state's, written once the game writes
 * them.
 *
 * Rewind (XBA-194, recomp_rewind.h), the same way too, but off unless RECOMP_REWIND=1 (a capture is a 4 / 8 MB state,
 * and the Vulkan RDP drains its work for each): a capture every RECOMP_REWIND_FRAMES frames (4 by default), one step
 * back per shown frame while the hotkey is held (Back + left trigger, R), silent.
 *
 * Cheats (XBA-197, recomp_cheats.h): RECOMP_CHEATS_FILE's enabled GameShark codes run after every N64 frame under
 * s_mu through n64_peek32 / n64_poke32 (a write that changes code a pack runs from RDRAM moves that page's write
 * counter: the interpreter takes over that function); the file is read again when the app changes it. */
#define _GNU_SOURCE /* pread, nanosleep */
#include "n64_runtime.h"

#include "host_audio.h"
#include "console_pad.h"
#include "host_pad.h"
#include "host_window.h"
#include "n64_core.h"
#include "n64_pack.h"
#include "host_perf_hint.h"
#include "n64_rdp.h"
#include "recomp_cheats.h"
#include "recomp_exit_hook.h"
#include "recomp_hotkeys.h"
#include "recomp_lifecycle.h"
#include "recomp_rewind.h"
#include "recomp_states.h"

#include <SDL.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#ifdef __linux__
#include <sched.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RT_CPU_HZ 93750000ull
#define SAVE_DEBOUNCE_NS 1000000000ull            /* a second after the last write to a save */
#define SAVE_LATEST_NS 5000000000ull              /* and at most five after the first */
#define AUDIO_TARGET_FRAMES (N64_AUDIO_RATE / 20) /* 50 ms queued */
#define FPS_LOG_NS 10000000000ull                 /* a frame-rate line every 10 s (the device check) */
#define STUCK_NS 5000000000ull                    /* the guest stuck detector's limit (n64_runtime_stuck_check) */
#define STICK_RANGE 80
#define N_SAVES 9 /* n64_save_kind: NONE .. PAK3 */

const char *n64_runtime_version(void)
{
    return "n64_runtime " N64_RUNTIME_VERSION;
}

/* ---- Pure helpers --------------------------------------------------------------------------------------------- */

/* The player's mapping (console_pad.h, RECOMP_N64_PAD_MAP), loaded when a game starts; the defaults until then. */
static ConsolePadMap s_pad_map;

uint16_t n64_runtime_buttons_of(uint32_t t)
{
    static const uint16_t bits[N64_T_COUNT] = {
        [N64_T_A] = N64_BTN_A, [N64_T_B] = N64_BTN_B, [N64_T_Z] = N64_BTN_Z, [N64_T_L] = N64_BTN_L,
        [N64_T_R] = N64_BTN_R, [N64_T_START] = N64_BTN_START, [N64_T_UP] = N64_BTN_DUP, [N64_T_DOWN] = N64_BTN_DDOWN,
        [N64_T_LEFT] = N64_BTN_DLEFT, [N64_T_RIGHT] = N64_BTN_DRIGHT, [N64_T_C_UP] = N64_BTN_CUP,
        [N64_T_C_DOWN] = N64_BTN_CDOWN, [N64_T_C_LEFT] = N64_BTN_CLEFT, [N64_T_C_RIGHT] = N64_BTN_CRIGHT,
    };
    uint16_t k = 0;
    for (int i = 0; i < N64_T_COUNT; i++)
        if (t & (1u << i))
            k |= bits[i];
    /* Opposite directions at once (two pads, a worn stick): a real controller cannot, some games misbehave. */
    const uint16_t pairs[4][2] = {{N64_BTN_DUP, N64_BTN_DDOWN}, {N64_BTN_DLEFT, N64_BTN_DRIGHT},
                                  {N64_BTN_CUP, N64_BTN_CDOWN}, {N64_BTN_CLEFT, N64_BTN_CRIGHT}};
    for (int i = 0; i < 4; i++)
        if ((k & pairs[i][0]) && (k & pairs[i][1]))
            k &= (uint16_t)~(pairs[i][0] | pairs[i][1]);
    return k;
}

uint16_t n64_runtime_pad_buttons(uint32_t b, int rx, int ry, int lt, int rt)
{
    ConsolePadMap m;
    ConsolePadOut o;
    HostPadState st = {.connected = 1, .buttons = b};
    st.axis[HOST_PAD_AXIS_RX] = (int16_t)rx;
    st.axis[HOST_PAD_AXIS_RY] = (int16_t)ry;
    st.axis[HOST_PAD_AXIS_LT] = (int16_t)lt;
    st.axis[HOST_PAD_AXIS_RT] = (int16_t)rt;
    console_pad_defaults(&m, CONSOLE_PAD_N64);
    console_pad_apply(&m, &st, &o);
    return n64_runtime_buttons_of(o.buttons);
}

void n64_runtime_stick(int lx, int ly, int range, int dead, int8_t *x, int8_t *y)
{
    if (range <= 0)
        range = STICK_RANGE;
    if (range > 127)
        range = 127;
    if (dead < 0)
        dead = 32767 * 8 / 100;
    if (dead > 30000)
        dead = 30000;
    double fx = lx, fy = -(double)ly; /* the N64's +y is up */
    double r = sqrt(fx * fx + fy * fy);
    if (r <= dead) {
        *x = *y = 0;
        return;
    }
    double t = (r - dead) / (32767.0 - dead); /* 0..1 past the dead zone */
    if (t > 1.0)
        t = 1.0;
    /* The host stick's round gate to the N64's octagon: the diagonals go 20 % further (0.707 x 1.2 = 0.85 a side). */
    double s2 = 2.0 * fabs(fx) * fabs(fy) / (r * r); /* sin 2θ: 0 on an axis, 1 on a diagonal */
    double mag = t * range * (1.0 + 0.2 * s2);
    double vx = fx / r * mag, vy = fy / r * mag;
    if (vx > range) vx = range;
    if (vx < -range) vx = -range;
    if (vy > range) vy = range;
    if (vy < -range) vy = -range;
    *x = (int8_t)lround(vx);
    *y = (int8_t)lround(vy);
}

void n64_runtime_xrgb_to_rgba(const uint32_t *src, uint8_t *dst, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t c = src[i];
        dst[4 * i + 0] = (uint8_t)(c >> 16);
        dst[4 * i + 1] = (uint8_t)(c >> 8);
        dst[4 * i + 2] = (uint8_t)c;
        dst[4 * i + 3] = 255;
    }
}

static const char *save_name(int kind)
{
    switch (kind) {
    case N64_SAVE_EEPROM4K:
    case N64_SAVE_EEPROM16K: return "game.eep";
    case N64_SAVE_SRAM: return "game.sra";
    case N64_SAVE_FLASH: return "game.fla";
    case N64_SAVE_PAK0: return "pak1.mpk";
    case N64_SAVE_PAK1: return "pak2.mpk";
    case N64_SAVE_PAK2: return "pak3.mpk";
    case N64_SAVE_PAK3: return "pak4.mpk";
    default: return NULL;
    }
}

int n64_runtime_save_path(const char *save_dir, const char *key, int kind, char *out, size_t out_n)
{
    const char *name = save_name(kind);
    if (!save_dir || !*save_dir || !key || !*key || !out || !name)
        return -1;
    int n = snprintf(out, out_n, "%s/n64-%s/%s", save_dir, key, name);
    return n < 0 || (size_t)n >= out_n ? -1 : 0;
}

static char code_char(uint8_t c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? (char)c : '_';
}

int n64_runtime_fallback_key(const uint8_t *rom, size_t n, char *out, size_t out_n)
{
    uint8_t h[0x40];
    if (!rom || n < sizeof h || !out || out_n < 16)
        return -1;
    memcpy(h, rom, sizeof h);
    if (n64_rom_normalize(h, sizeof h) != 0)
        return -1;
    snprintf(out, out_n, "%c%c%c%c-%02X%02X%02X%02X", code_char(h[0x3B]), code_char(h[0x3C]), code_char(h[0x3D]),
             code_char(h[0x3E]), h[0x10], h[0x11], h[0x12], h[0x13]);
    return 0;
}

uint64_t n64_runtime_frame_ns(uint64_t cycles)
{
    uint64_t ns = cycles * 1000000000ull / RT_CPU_HZ;
    const uint64_t lo = 1000000000ull / 75, hi = 1000000000ull / 45;
    return ns < lo ? lo : ns > hi ? hi : ns;
}

int n64_runtime_pak_parse(const char *v)
{
    if (!v)
        return N64_RT_PAK_DEFAULT;
    if (!strcasecmp(v, "controller") || !strcasecmp(v, "pak") || !strcasecmp(v, "mempak") ||
        !strcasecmp(v, "controller_pak"))
        return N64_RT_PAK_CONTROLLER;
    if (!strcasecmp(v, "rumble") || !strcasecmp(v, "rumble_pak"))
        return N64_RT_PAK_RUMBLE;
    if (!strcasecmp(v, "none") || !strcasecmp(v, "off"))
        return N64_RT_PAK_NONE;
    return N64_RT_PAK_DEFAULT;
}

int n64_runtime_frameskip_parse(const char *v)
{
    if (v && v[0] >= '0' && v[0] <= '9' && !v[1])
        return v[0] - '0';
    return N64_RT_FRAMESKIP_AUTO;
}

static const char *exc_name(uint32_t code)
{
    switch (code) {
    case 1: return "TLB modified";
    case 2: return "TLB miss (load / fetch)";
    case 3: return "TLB miss (store)";
    case 4: return "address error (load / fetch)";
    case 5: return "address error (store)";
    case 6: return "bus error (fetch)";
    case 7: return "bus error (data)";
    case 10: return "reserved instruction";
    case 11: return "coprocessor unusable";
    default: return "exception";
    }
}

/* Diagnostics of the frame loop, logged once (the Android shell sends stdout to logcat): the first guest fault
 * exception of the session, and the « guest stuck » detector (n64_runtime_stuck_check). No effect on the game. */
static void guest_health(n64_machine *m, n64_runtime_stuck *st, uint64_t *fault_logged, uint64_t frame_ns)
{
    const n64_guest_fault *f = n64_guest_fault_info(m);
    if (f->count && !*fault_logged) {
        *fault_logged = 1;
        printf("n64: guest fault at frame %llu: %s (Cause %08x), EPC %08llx, BadVAddr %08llx, Status %08x\n",
               (unsigned long long)f->frame, exc_name(f->code), f->cause, (unsigned long long)(f->epc & 0xFFFFFFFFu),
               (unsigned long long)(f->badvaddr & 0xFFFFFFFFu), f->status);
        fflush(stdout);
    }
    n64_activity a;
    n64_guest_activity(m, &a);
    uint64_t limit = frame_ns ? STUCK_NS / frame_ns : 300;
    int why = n64_runtime_stuck_check(st, a.vi_origin_changes, a.si_dmas + a.pi_dmas + a.ai_buffers, f->count, limit);
    if (why == N64_RT_STUCK_NONE)
        return;
    struct n64_rdp *rdp = n64_get_rdp(m);
    printf("n64: guest stuck at frame %llu (%s for %.0f s): pc %08llx Status %08llx Cause %08llx EPC %08llx "
           "BadVAddr %08llx, %llu fault exceptions, DPC start %06x end %06x current %06x status %04x\n",
           (unsigned long long)n64_frames(m),
           why == N64_RT_STUCK_IDLE ? "no picture, DMA or audio" : "no new picture since a fault exception",
           (double)STUCK_NS / 1e9, (unsigned long long)(n64_pc(m) & 0xFFFFFFFFu),
           (unsigned long long)(n64_cop0(m, 12) & 0xFFFFFFFFu), (unsigned long long)(n64_cop0(m, 13) & 0xFFFFFFFFu),
           (unsigned long long)(n64_cop0(m, 14) & 0xFFFFFFFFu), (unsigned long long)(n64_cop0(m, 8) & 0xFFFFFFFFu),
           (unsigned long long)f->count, rdp ? n64_rdp_read_reg(rdp, 0) : 0, rdp ? n64_rdp_read_reg(rdp, 1) : 0,
           rdp ? n64_rdp_read_reg(rdp, 2) : 0, rdp ? n64_rdp_read_reg(rdp, 3) : 0);
    fflush(stdout);
}

void n64_runtime_stuck_init(n64_runtime_stuck *s)
{
    memset(s, 0, sizeof *s);
    s->fault_age = UINT64_MAX;
}

int n64_runtime_stuck_check(n64_runtime_stuck *s, uint64_t vi_origin_changes, uint64_t other_activity,
                            uint64_t faults, uint64_t limit)
{
    int vi_moved = vi_origin_changes != s->vi, other_moved = other_activity != s->other;
    if (faults != s->faults)
        s->fault_age = 0;
    else if (s->fault_age < UINT64_MAX)
        s->fault_age++;
    if (vi_moved) {
        s->vi_still = 0;
        /* A crash screen is often one last picture drawn after the fault: a fault shortly before still counts. */
        s->fault_since_vi = limit && s->fault_age < limit;
        s->reported = N64_RT_STUCK_NONE; /* a new picture: whatever was reported is over */
    } else {
        s->vi_still++;
        if (s->fault_age == 0)
            s->fault_since_vi = 1;
    }
    if (vi_moved || other_moved) {
        s->all_still = 0;
        if (s->reported == N64_RT_STUCK_IDLE)
            s->reported = N64_RT_STUCK_NONE;
    } else {
        s->all_still++;
    }
    s->vi = vi_origin_changes;
    s->other = other_activity;
    s->faults = faults;
    if (!limit)
        return N64_RT_STUCK_NONE;
    if (s->all_still >= limit && s->reported == N64_RT_STUCK_NONE)
        return s->reported = N64_RT_STUCK_IDLE;
    if (s->fault_since_vi && s->vi_still >= limit && s->reported != N64_RT_STUCK_FAULT &&
        s->all_still < limit)
        return s->reported = N64_RT_STUCK_FAULT;
    return N64_RT_STUCK_NONE;
}

int n64_runtime_frameskip(int mode, int64_t behind_ns, uint64_t frame_ns, int audio_low, int *run)
{
    int skip;
    if (mode == 0)
        skip = 0;
    else if (mode > 0)
        skip = *run < mode;
    else
        skip = *run < 3 && (behind_ns > (int64_t)(frame_ns / 2) || audio_low);
    *run = skip ? *run + 1 : 0;
    return skip;
}

int n64_runtime_frameskip_picture(int mode, int64_t behind_ns, uint64_t frame_ns, int audio_low, int *run)
{
    if (mode >= 0)
        return n64_runtime_frameskip(mode, behind_ns, frame_ns, audio_low, run);
    int skip = *run < 1 && (behind_ns > (int64_t)(frame_ns / 2) || audio_low);
    *run = skip ? *run + 1 : 0;
    return skip;
}

int n64_runtime_perf_format(const n64_runtime_perf *p, char *out, size_t n)
{
    double fps = p->seconds > 0 ? (double)p->frames / p->seconds : 0.0;
    char pack[160], core[64];
    if (p->pack)
        snprintf(pack, sizeof pack, "pack %.0f %% native, %.1f k interp instr/frame, %llu rehashes, %llu slow lookups",
                 p->native_pct, p->interp_instr / 1e3, (unsigned long long)p->rehashes,
                 (unsigned long long)p->slow_lookups);
    else
        snprintf(pack, sizeof pack, "no pack");
    if (p->cpu < 0)
        snprintf(core, sizeof core, "core ?");
    else if (p->cpu_khz <= 0)
        snprintf(core, sizeof core, "core %d", p->cpu);
    else
        snprintf(core, sizeof core, "core %d @ %.2f GHz", p->cpu, (double)p->cpu_khz / 1e6);
    char threads[32] = "";
    if (p->rdp && !strcmp(p->rdp, "software"))
        snprintf(threads, sizeof threads, ", %d thread%s", p->rdp_threads, p->rdp_threads == 1 ? "" : "s");
    return snprintf(out, n,
                    "speed %.0f %% (%.1f of %.1f VI/s); cpu %.1f rsp %.1f rdp %.1f (queue %.1f, sync %.1f, scanout "
                    "%.1f) sched %.1f vi %.1f "
                    "present %.1f audio %.1f idle %.1f other %.1f ms/frame; worst %.0f ms, %llu > %.1f ms, %llu > "
                    "%.1f ms of %llu frames; rsp %.2f M instr, rdp %.0f cmds, %.1f full syncs /frame; %s; rdp %s%s, "
                    "upscale %dx, rewind %s; %s",
                    p->vi_hz > 0 ? 100.0 * fps / p->vi_hz : 0.0, fps, p->vi_hz, p->cpu_ms, p->rsp_ms, p->rdp_ms,
                    p->rdp_queue_ms, p->rdp_sync_ms, p->rdp_scanout_ms, p->sched_ms, p->vi_ms, p->present_ms, p->audio_ms, p->idle_ms, p->other_ms,
                    p->worst_ms, (unsigned long long)p->over_1, p->over_ms, (unsigned long long)p->over_2,
                    2 * p->over_ms, (unsigned long long)p->host_frames, p->rsp_instr / 1e6, p->rdp_cmds,
                    p->rdp_syncs, pack, p->rdp ? p->rdp : "none", threads, p->upscale > 0 ? p->upscale : 1,
                    p->rewind ? "on" : "off", core);
}

int n64_runtime_perf_detail_format(const n64_runtime_perf *p, char *out, size_t n)
{
    return snprintf(out, n,
                    "%llu VIs, %llu pictures (%.1f/s), %llu unchanged, %llu skipped; %llu VI_ORIGIN changes, %llu "
                    "scanouts (%llu early), %llu full rings; DP %llu deferred (%llu early, %llu backoffs), %llu at "
                    "once, %llu waited %.1f ms (%.2f ms each); %llu stalled compiles (%.0f ms); worst %.0f ms: core "
                    "%.1f vi %.1f present %.1f audio %.1f other %.1f (rdp queue %.1f, sync %.1f, deadline %.1f, "
                    "scanout %.1f)",
                    (unsigned long long)p->frames, (unsigned long long)p->pictures,
                    p->seconds > 0 ? (double)p->pictures / p->seconds : 0.0, (unsigned long long)p->unchanged,
                    (unsigned long long)p->skipped, (unsigned long long)p->vi_origin_changes,
                    (unsigned long long)p->scanouts, (unsigned long long)p->scanouts_early,
                    (unsigned long long)p->queue_waits, (unsigned long long)p->dp_deferred,
                    (unsigned long long)p->dp_early, (unsigned long long)p->dp_backoffs,
                    (unsigned long long)p->dp_now, (unsigned long long)p->dp_waits, p->dp_wait_ms,
                    p->dp_deferred ? p->dp_wait_ms / (double)p->dp_deferred : 0.0,
                    (unsigned long long)p->stalled_compiles, p->stalled_compile_ms, p->worst_ms, p->worst_core_ms,
                    p->worst_vi_ms, p->worst_present_ms, p->worst_audio_ms, p->worst_other_ms, p->worst_queue_ms,
                    p->worst_sync_ms, p->worst_deadline_ms, p->worst_scanout_ms);
}

/* ---- Files ---------------------------------------------------------------------------------------------------- */

static int spec_fd(const char *spec, int *own)
{
    if (strncmp(spec, "fd:", 3) == 0) {
        char *end;
        long v = strtol(spec + 3, &end, 10);
        *own = 0;
        return end == spec + 3 || *end || v < 0 ? -1 : (int)v;
    }
    *own = 1;
    return open(spec, O_RDONLY | O_CLOEXEC);
}

/* A whole file, or "fd:N" read from offset 0 (the descriptor stays open and its offset is not used). NULL when it
 * cannot be read, is empty or is larger than max. */
static uint8_t *read_all(const char *spec, size_t max, size_t *size)
{
    int own, fd = spec_fd(spec, &own);
    *size = 0;
    if (fd < 0)
        return NULL;
    size_t cap = 1 << 16, n = 0;
    uint8_t *buf = malloc(cap);
    while (buf) {
        if (n == cap) {
            if (cap >= max + 1)
                break;
            uint8_t *nb = realloc(buf, cap * 2);
            if (!nb) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = nb;
            cap *= 2;
        }
        ssize_t r = pread(fd, buf + n, cap - n, (off_t)n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0) {
            free(buf);
            buf = NULL;
        }
        if (r <= 0)
            break;
        n += (size_t)r;
    }
    if (own)
        close(fd);
    if (buf && (n == 0 || n > max)) {
        free(buf);
        buf = NULL;
    }
    *size = n;
    return buf;
}

static void mkdirs(const char *path)
{
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    mkdir(tmp, 0755);
}

/* Writes path through path.tmp + rename, so a kill mid-write leaves the old save. */
static int write_file(const char *path, const uint8_t *data, size_t n)
{
    char tmp[1300], dir[1200];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        mkdirs(dir);
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return -1;
    int ok = fwrite(data, 1, n, f) == n;
    ok &= fflush(f) == 0;
    ok &= fsync(fileno(f)) == 0;
    ok &= fclose(f) == 0;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int write_ppm(const char *path, const uint32_t *px, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint8_t rgb[3] = {(uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i]};
        fwrite(rgb, 1, 3, f);
    }
    return fclose(f);
}

/* ---- Saves ---------------------------------------------------------------------------------------------------- */

/* The run's machine and its saves, shared with the exit hook (the in-game menu's Quit ends the process from the UI
 * thread) and the lifecycle listener: s_mu is held while a frame runs and while a save is written. */
static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;

/* The upscaled picture presented last straight from the Vulkan RDP's buffer (n64_display_rgba,
 * host_window_present_rgba_shared): host_window_screenshot_bmp copies it on demand (any thread) instead of the window
 * copying every picture. Under s_mu (the RDP's buffers change only while it is held). */
static struct {
    const uint8_t *px;
    int w, h, pitch;
} s_shared;

static int shared_last(void *user, uint8_t **px, int *w, int *h)
{
    (void)user;
    int rc = -1;
    pthread_mutex_lock(&s_mu);
    if (s_shared.px && s_shared.w > 0 && s_shared.h > 0 &&
        (*px = malloc((size_t)s_shared.w * (size_t)s_shared.h * 4)) != NULL) {
        for (int y = 0; y < s_shared.h; y++)
            memcpy(*px + (size_t)y * (size_t)s_shared.w * 4, s_shared.px + (size_t)y * (size_t)s_shared.pitch,
                   (size_t)s_shared.w * 4);
        *w = s_shared.w;
        *h = s_shared.h;
        rc = 0;
    }
    pthread_mutex_unlock(&s_mu);
    return rc;
}
static n64_machine *s_m;
static char s_save_dir[1024], s_key[64];
static uint8_t *s_saved[N_SAVES]; /* each image as last read or written (or as the run started), by kind */
static char s_discovered[1200];   /* n64_runtime_opts.discovered_path of the run */

static void save_path(int kind, char *out, size_t n)
{
    if (n64_runtime_save_path(s_save_dir, s_key, kind, out, n) != 0)
        out[0] = 0;
}

/* Writes the cartridge's save (once its kind is known) and each controller pak that differs from what is on disk
 * (or was in the machine at start). Called with s_mu held. */
static void saves_flush_locked(const char *why)
{
    if (!s_m || !s_save_dir[0])
        return;
    int kinds[5] = {n64_save_kind_detected(s_m), N64_SAVE_PAK0, N64_SAVE_PAK1, N64_SAVE_PAK2, N64_SAVE_PAK3};
    for (int i = 0; i < 5; i++) {
        int kind = kinds[i];
        size_t n = 0;
        const uint8_t *d = kind != N64_SAVE_NONE ? n64_save_data(s_m, (n64_save_kind)kind, &n) : NULL;
        if (!d || !n)
            continue;
        if (s_saved[kind] && memcmp(d, s_saved[kind], n) == 0)
            continue;
        char path[1200];
        save_path(kind, path, sizeof path);
        if (!path[0])
            continue;
        if (write_file(path, d, n) != 0) {
            fprintf(stderr, "n64: cannot write the save %s: %s\n", path, strerror(errno));
            continue;
        }
        if (!s_saved[kind])
            s_saved[kind] = malloc(n);
        if (s_saved[kind])
            memcpy(s_saved[kind], d, n);
        printf("n64: save written (%zu bytes, %s) to %s\n", n, why, path);
    }
    fflush(stdout);
}

static void saves_flush(const char *why)
{
    pthread_mutex_lock(&s_mu);
    saves_flush_locked(why);
    pthread_mutex_unlock(&s_mu);
}

/* The discovery loop's record to disk (XBA-174): at exit, when the app pauses (a session usually ends killed in the
 * background, after the pause) and every few minutes of play. Appends only what the file lacks. */
#define MISSED_EVERY_NS (180ull * 1000000000ull)
static void missed_flush(void)
{
    pthread_mutex_lock(&s_mu);
    if (s_m && s_discovered[0])
        n64_pack_save_missed(s_m, s_discovered);
    pthread_mutex_unlock(&s_mu);
}

/* missed_flush every MISSED_EVERY_NS of the run loop's time (the emulation thread, between frames). */
static void missed_flush_every(uint64_t now)
{
    static uint64_t last;
    if (!last)
        last = now;
    if (now - last >= MISSED_EVERY_NS) {
        last = now;
        missed_flush();
    }
}

static void on_exit_hook(const char *why)
{
    (void)why;
    saves_flush("exit");
    n64_rdp_save_caches(); /* the Vulkan RDP's pipeline cache, when new pipelines were compiled */
    missed_flush(); /* the in-game menu's Quit ends the process here */
}

/* The app paused (background, the in-game menu): the saves go to disk at once, from the thread that paused it (on
 * Android SDL parks the thread that pumps events in the background, and the process can be killed there). Registered
 * once; no run, no-op. */
static void on_lifecycle(int paused, void *user)
{
    (void)user;
    if (paused) {
        saves_flush("pause");
        n64_rdp_save_caches(); /* the Vulkan RDP's pipeline cache (n64_vk_cache.h): sessions end killed after a pause */
        missed_flush();
    }
}

static void snapshot(n64_machine *m, int kind)
{
    size_t n = 0;
    const uint8_t *d = n64_save_data(m, (n64_save_kind)kind, &n);
    free(s_saved[kind]);
    s_saved[kind] = NULL;
    if (d && n && (s_saved[kind] = malloc(n)))
        memcpy(s_saved[kind], d, n);
    n64_save_take_dirty(m, (n64_save_kind)kind);
}

/* Loads the cartridge save found on disk (game.eep / game.sra / game.fla: the one the header's save type names
 * first, else the first there) and the controller paks. An .eep's size picks 4K (512 B) or 16K (2 KB) unless the
 * kind is known already; then a 2 KB file for a 4K game (some emulators always write 2 KB) gives its first 512 bytes,
 * a 512-byte one for a 16K game is padded with 0xFF. Called with s_mu held. */
static void saves_load(n64_machine *m)
{
    char path[1200];
    int known = n64_save_kind_detected(m);
    const int order[3] = {N64_SAVE_EEPROM4K, N64_SAVE_SRAM, N64_SAVE_FLASH};
    int loaded = 0;
    for (int pass = 0; pass < 2 && !loaded && s_save_dir[0]; pass++) {
        for (int i = 0; i < 3 && !loaded; i++) {
            int kind = order[i];
            int is_eep = kind == N64_SAVE_EEPROM4K;
            int matches = known == kind || (is_eep && known == N64_SAVE_EEPROM16K);
            if (pass == 0 ? !matches : known != N64_SAVE_NONE)
                continue; /* first the header's kind; the others only while it is unknown */
            save_path(kind, path, sizeof path);
            size_t n = 0;
            uint8_t *d = path[0] ? read_all(path, 128 * 1024, &n) : NULL;
            if (!d)
                continue;
            int want = kind;
            if (is_eep)
                want = known == N64_SAVE_EEPROM16K || (known == N64_SAVE_NONE && n == 2048) ? N64_SAVE_EEPROM16K
                                                                                              : N64_SAVE_EEPROM4K;
            size_t need = n64_save_size((n64_save_kind)want);
            uint8_t *img = malloc(need);
            if (img) {
                memset(img, 0xFF, need);
                memcpy(img, d, n < need ? n : need);
            }
            if (img && (n == need || is_eep) && n64_save_load(m, (n64_save_kind)want, img, need) == 0) {
                printf("n64: save loaded (%zu bytes, %s) from %s\n", need,
                       want == N64_SAVE_EEPROM4K ? "EEPROM 4K" : want == N64_SAVE_EEPROM16K ? "EEPROM 16K"
                       : want == N64_SAVE_SRAM ? "SRAM" : "FlashRAM", path);
                loaded = 1;
            } else {
                fprintf(stderr, "n64: %s is not a %s image (%zu bytes), ignored (kept until the game saves)\n", path,
                        is_eep ? "512 B / 2 KB EEPROM" : kind == N64_SAVE_SRAM ? "32 KB SRAM" : "128 KB FlashRAM", n);
            }
            free(img);
            free(d);
        }
    }
    if (!loaded && s_save_dir[0])
        printf("n64: no cartridge save yet (%s/n64-%s/)\n", s_save_dir, s_key);
    for (int p = 0; p < 4; p++) {
        int kind = N64_SAVE_PAK0 + p;
        save_path(kind, path, sizeof path);
        size_t n = 0;
        uint8_t *d = path[0] ? read_all(path, 64 * 1024, &n) : NULL;
        if (d && n == n64_save_size((n64_save_kind)kind) && n64_save_load(m, (n64_save_kind)kind, d, n) == 0)
            printf("n64: controller pak %d loaded from %s\n", p + 1, path);
        else if (d)
            fprintf(stderr, "n64: %s is not a 32 KB controller pak image (%zu bytes), ignored\n", path, n);
        free(d);
    }
    /* What is on disk now (or the fresh images): written once they change. */
    for (int kind = N64_SAVE_EEPROM4K; kind < N_SAVES; kind++)
        snapshot(m, kind);
}

static void saves_clear(void)
{
    for (int k = 0; k < N_SAVES; k++) {
        free(s_saved[k]);
        s_saved[k] = NULL;
    }
}

/* ---- Save states (XBA-183) ------------------------------------------------------------------------------------ */

static char s_state_dir[1100]; /* <save dir>/n64-<KEY> */
static int s_state_loaded;     /* a state was loaded: the loop resets its clock (under s_mu) */
static int s_state_crop;       /* the run crops the VI's guard band: the thumbnail too */

/* recomp_states_save's callback, any thread: the state and a thumbnail of the picture the VI shows (at 1x), taken
 * under s_mu (between frames), then written to <dir>/state<slot>.st. */
static int state_save_op(int slot, void *user)
{
    (void)user;
    char path[1200];
    if (recomp_states_slot_path(s_state_dir, slot, path, sizeof path) != 0)
        return RECOMP_STATE_ESLOT;
    static uint32_t xrgb[N64_SCREEN_MAX_W * N64_SCREEN_MAX_H];
    static uint8_t rgba[N64_SCREEN_MAX_W * N64_SCREEN_MAX_H * 4];
    static uint16_t thumb[RECOMP_STATE_THUMB_W * RECOMP_STATE_THUMB_H];
    recomp_state_meta meta;
    memset(&meta, 0, sizeof meta);
    snprintf(meta.console, sizeof meta.console, "n64");
    snprintf(meta.key, sizeof meta.key, "%.47s", s_key);
    int w = 0, h = 0;
    pthread_mutex_lock(&s_mu);
    if (!s_m) {
        pthread_mutex_unlock(&s_mu);
        return RECOMP_STATE_ENOGAME;
    }
    size_t cap = n64_state_size(s_m), n = 0;
    uint8_t *blob = malloc(cap);
    if (blob)
        n = n64_state_save(s_m, blob, cap);
    meta.frames = n64_frames(s_m);
    if (n64_display(s_m, xrgb, N64_SCREEN_MAX_W, N64_SCREEN_MAX_H, &w, &h) != 0 || w <= 0 || h <= 0) {
        w = 320;
        h = 240;
        memset(xrgb, 0, sizeof(uint32_t) * 320 * 240);
    }
    pthread_mutex_unlock(&s_mu);
    n64_runtime_xrgb_to_rgba(xrgb, rgba, (size_t)w * (size_t)h);
    int cc = s_state_crop ? n64_runtime_crop_columns(w) : 0;
    recomp_states_thumb(rgba + 4 * cc, w - 2 * cc, h, w * 4, 4, 3, thumb, &meta.thumb_w, &meta.thumb_h);
    int rc = n ? recomp_states_write(path, &meta, thumb, blob, n) : RECOMP_STATE_EIO;
    free(blob);
    printf("n64: state slot %d %s (%zu bytes, frame %llu) %s\n", slot, rc == 0 ? "saved" : "not saved", n,
           (unsigned long long)meta.frames, rc == 0 ? path : recomp_states_error(rc));
    fflush(stdout);
    return rc;
}

/* A state was loaded into m (a slot, a rewind step), s_mu held: the audio it had queued was heard already, and its
 * saves are what the game sees now: the files change only once the game writes them. */
static void state_loaded_locked(n64_machine *m)
{
    int16_t sink[1024 * 2];
    while (n64_audio_pull(m, sink, 1024) > 0)
        ;
    for (int kind = N64_SAVE_EEPROM4K; kind < N_SAVES; kind++) {
        size_t sn = 0;
        const uint8_t *cur = n64_save_data(m, (n64_save_kind)kind, &sn);
        if (cur && sn && (s_saved[kind] || (s_saved[kind] = malloc(sn))))
            memcpy(s_saved[kind], cur, sn);
    }
}

/* Rewind (XBA-194): a capture is the state alone (the picture is in RDRAM, n64_display). Under s_mu. */
static size_t rewind_size(void *user)
{
    return n64_state_size((n64_machine *)user); /* only when a capture is due: it draws the RDP's work out */
}

static size_t rewind_save(uint8_t *out, size_t cap, void *user)
{
    return n64_state_save((n64_machine *)user, out, cap);
}

static int rewind_load(const uint8_t *in, size_t n, void *user)
{
    n64_machine *m = user;
    if (n64_state_load(m, in, n) != N64_STATE_OK)
        return -1;
    state_loaded_locked(m);
    return 0;
}

static int state_load_op(int slot, void *user)
{
    (void)user;
    char path[1200];
    if (recomp_states_slot_path(s_state_dir, slot, path, sizeof path) != 0)
        return RECOMP_STATE_ESLOT;
    recomp_state_meta meta;
    uint8_t *blob = NULL;
    size_t n = 0;
    int rc = recomp_states_read(path, &meta, NULL, &blob, &n);
    if (rc == RECOMP_STATE_OK && strcmp(meta.console, "n64") != 0)
        rc = RECOMP_STATE_EGAME;
    if (rc == RECOMP_STATE_OK) {
        pthread_mutex_lock(&s_mu);
        if (!s_m) {
            rc = RECOMP_STATE_ENOGAME;
        } else {
            rc = n64_state_load(s_m, blob, n); /* N64_STATE_* share RECOMP_STATE_*'s values */
            if (rc == N64_STATE_OK) {
                state_loaded_locked(s_m);
                s_state_loaded = 1;
            }
        }
        pthread_mutex_unlock(&s_mu);
    }
    free(blob);
    if (rc == 0)
        printf("n64: state slot %d loaded (%s)\n", slot, path);
    else
        printf("n64: state slot %d not loaded: %s\n", slot, recomp_states_error(rc));
    fflush(stdout);
    return rc;
}

/* ---- Cheats (XBA-197): the bus, RDRAM words through the core's side-effect-free accessors --------------------- */

static uint32_t cheat_peek32(void *m, uint32_t addr)
{
    return n64_peek32(m, addr);
}

static void cheat_poke32(void *m, uint32_t addr, uint32_t v)
{
    n64_poke32(m, addr, v);
}

/* ---- The run -------------------------------------------------------------------------------------------------- */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- The performance line (n64_runtime_perf) ------------------------------------------------------------------ */
/* The runtime's stage times, totals since the run started (ns), and the host frames. */
typedef struct rt_perf_acc {
    uint64_t core, vi, present, audio, idle;
    uint64_t host_frames, over_1, over_2;
    uint64_t pictures, unchanged, skipped; /* n64_runtime_perf's */
} rt_perf_acc;

/* The worst host frame's breakdown (ns): its length without the sleep, its stages, the RDP's waits in it. */
typedef struct rt_worst {
    uint64_t busy, core, vi, present, audio, queue, wait, scanout, deadline;
} rt_worst;

/* Everything a line is the difference of: taken at the start, at each line and after a pause. */
typedef struct rt_perf_mark {
    uint64_t t;
    rt_perf_acc acc;
    n64_perf core;
    n64_recomp_stats rc;
    uint64_t instructions;
    uint64_t vi_origin_changes;
} rt_perf_mark;

static n64_runtime_perf s_perf_last;

void n64_runtime_last_perf(n64_runtime_perf *out)
{
    *out = s_perf_last;
}

/* Under s_mu (a save state may be taken meanwhile). */
static void perf_mark(rt_perf_mark *k, n64_machine *m, const rt_perf_acc *acc, uint64_t t)
{
    k->t = t;
    k->acc = *acc;
    n64_perf_get(m, &k->core);
    n64_recomp_get_stats(m, &k->rc);
    k->instructions = n64_instructions(m);
    n64_activity a;
    n64_guest_activity(m, &a);
    k->vi_origin_changes = a.vi_origin_changes;
}

/* The host core this thread runs on and its clock, where the system says (Linux, Android). */
static void perf_cpu(int *cpu, long *khz)
{
    *cpu = -1;
    *khz = 0;
#ifdef __linux__
    *cpu = sched_getcpu();
    if (*cpu >= 0) {
        char path[96];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", *cpu);
        FILE *f = fopen(path, "r");
        if (f) {
            if (fscanf(f, "%ld", khz) != 1)
                *khz = 0;
            fclose(f);
        }
    }
#endif
}

static double ms_per(uint64_t ns, uint64_t frames)
{
    return frames ? (double)ns / 1e6 / (double)frames : 0.0;
}

/* The line's figures from mark a to mark b (wall time `wall`). */
static void perf_fill(n64_runtime_perf *p, const rt_perf_mark *a, const rt_perf_mark *b, uint64_t wall,
                      const rt_worst *worst, uint64_t frame_ns, int pack, struct n64_rdp *rdp, int rdp_threads,
                      int rewind)
{
    memset(p, 0, sizeof *p);
    const uint64_t f = b->core.frames - a->core.frames;
    p->seconds = (double)wall / 1e9;
    p->frames = f;
    p->host_frames = b->acc.host_frames - a->acc.host_frames;
    p->vi_hz = frame_ns ? 1e9 / (double)frame_ns : 0.0;
    /* n64_run_frame as a whole (every frame), split as the timed frames split it. */
    const double core = ms_per(b->acc.core - a->acc.core, f);
    const double tf = (double)(b->core.frame_ns - a->core.frame_ns);
    if (tf > 0) {
        p->cpu_ms = core * (double)(b->core.cpu_ns - a->core.cpu_ns) / tf;
        p->rsp_ms = core * (double)(b->core.rsp_ns - a->core.rsp_ns) / tf;
        p->rdp_ms = core * (double)(b->core.rdp_ns - a->core.rdp_ns) / tf;
    }
    p->sched_ms = core - p->cpu_ms - p->rsp_ms - p->rdp_ms;
    if (p->sched_ms < 0)
        p->sched_ms = 0;
    p->rdp_wait_ms = ms_per(b->core.rdp_wait_ns - a->core.rdp_wait_ns, f);
    p->rdp_queue_ms = ms_per(b->core.rdp_queue_ns - a->core.rdp_queue_ns, f);
    p->rdp_scanout_ms = ms_per(b->core.rdp_scanout_ns - a->core.rdp_scanout_ns, f);
    p->rdp_sync_ms = p->rdp_wait_ms > p->rdp_scanout_ms ? p->rdp_wait_ms - p->rdp_scanout_ms : 0.0;
    p->vi_ms = ms_per(b->acc.vi - a->acc.vi, f);
    p->present_ms = ms_per(b->acc.present - a->acc.present, f);
    p->audio_ms = ms_per(b->acc.audio - a->acc.audio, f);
    p->idle_ms = ms_per(b->acc.idle - a->acc.idle, f);
    p->other_ms = ms_per(wall, f) - core - p->vi_ms - p->present_ms - p->audio_ms - p->idle_ms;
    if (p->other_ms < 0)
        p->other_ms = 0;
    p->worst_ms = (double)worst->busy / 1e6;
    p->worst_core_ms = (double)worst->core / 1e6;
    p->worst_vi_ms = (double)worst->vi / 1e6;
    p->worst_present_ms = (double)worst->present / 1e6;
    p->worst_audio_ms = (double)worst->audio / 1e6;
    {
        uint64_t parts = worst->core + worst->vi + worst->present + worst->audio;
        p->worst_other_ms = worst->busy > parts ? (double)(worst->busy - parts) / 1e6 : 0.0;
    }
    p->worst_queue_ms = (double)worst->queue / 1e6;
    p->worst_scanout_ms = (double)worst->scanout / 1e6;
    p->worst_sync_ms = worst->wait > worst->scanout ? (double)(worst->wait - worst->scanout) / 1e6 : 0.0;
    p->worst_deadline_ms = (double)worst->deadline / 1e6;
    p->pictures = b->acc.pictures - a->acc.pictures;
    p->unchanged = b->acc.unchanged - a->acc.unchanged;
    p->skipped = b->acc.skipped - a->acc.skipped;
    p->vi_origin_changes = b->vi_origin_changes - a->vi_origin_changes;
    p->scanouts = b->core.rdp_scanouts - a->core.rdp_scanouts;
    p->scanouts_early = b->core.rdp_prefetch_hits - a->core.rdp_prefetch_hits;
    p->dp_deferred = b->core.dp_deferred - a->core.dp_deferred;
    p->dp_now = b->core.dp_now - a->core.dp_now;
    p->dp_early = b->core.dp_early - a->core.dp_early;
    p->dp_backoffs = b->core.dp_backoffs - a->core.dp_backoffs;
    p->dp_waits = b->core.rdp_dp_waits - a->core.rdp_dp_waits;
    p->dp_wait_ms = (double)(b->core.rdp_dp_wait_ns - a->core.rdp_dp_wait_ns) / 1e6;
    p->queue_waits = b->core.rdp_queue_waits - a->core.rdp_queue_waits;
    p->stalled_compiles = b->core.rdp_stalled_compiles - a->core.rdp_stalled_compiles;
    p->stalled_compile_ms = (double)(b->core.rdp_stalled_compile_ns - a->core.rdp_stalled_compile_ns) / 1e6;
    p->over_ms = (double)frame_ns / 1e6;
    p->over_1 = b->acc.over_1 - a->acc.over_1;
    p->over_2 = b->acc.over_2 - a->acc.over_2;
    const double fd = f ? (double)f : 1.0;
    p->rsp_instr = (double)(b->core.rsp_instructions - a->core.rsp_instructions) / fd;
    p->rdp_cmds = (double)(b->core.rdp_commands - a->core.rdp_commands) / fd;
    p->rdp_syncs = (double)(b->core.rdp_full_syncs - a->core.rdp_full_syncs) / fd;
    p->pack = pack;
    if (pack) {
        /* Lifted code counts its instructions in n64_instructions too: what the interpreter did not run is native
         * (the IPL3 before the boot PC aside). */
        const uint64_t all = b->instructions - a->instructions, interp = b->rc.interp_ram - a->rc.interp_ram;
        p->native_pct = all ? 100.0 * (double)(all > interp ? all - interp : 0) / (double)all : 0.0;
        p->interp_instr = (double)interp / fd;
        p->rehashes = b->rc.rehashes - a->rc.rehashes;
        p->slow_lookups = b->rc.slow_lookups - a->rc.slow_lookups;
    }
    p->rdp = n64_rdp_backend_name(rdp);
    p->rdp_threads = rdp_threads;
    p->upscale = rdp ? n64_rdp_upscale(rdp) : 1;
    p->rewind = rewind;
    perf_cpu(&p->cpu, &p->cpu_khz);
}

/* Through the window: with frame generation (XBA-186) it draws the generated and real frames due meanwhile. */
static void sleep_until(uint64_t t)
{
    host_window_sleep_until(t);
}

static int env_choice(const char *name, const char *one, int def)
{
    const char *v = getenv(name);
    if (!v || !*v)
        return def;
    return strcasecmp(v, one) == 0;
}

static int env_int(const char *name, int def)
{
    const char *v = getenv(name);
    return v && *v ? atoi(v) : def;
}

#ifndef __ANDROID__
/* Desktop keys: arrows = stick, X = A, C = B, Z = Z, Enter = Start, Q / E = L / R, I / K / J / L = C up / down /
 * left / right, T / G / F / H = D-pad up / down / left / right. */
static uint16_t keyboard_buttons(int *kx, int *ky)
{
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    uint16_t k = 0;
    *kx = *ky = 0;
    if (!ks)
        return 0;
    if (ks[SDL_SCANCODE_X]) k |= N64_BTN_A;
    if (ks[SDL_SCANCODE_C]) k |= N64_BTN_B;
    if (ks[SDL_SCANCODE_Z]) k |= N64_BTN_Z;
    if (ks[SDL_SCANCODE_RETURN]) k |= N64_BTN_START;
    if (ks[SDL_SCANCODE_Q]) k |= N64_BTN_L;
    if (ks[SDL_SCANCODE_E]) k |= N64_BTN_R;
    if (ks[SDL_SCANCODE_I]) k |= N64_BTN_CUP;
    if (ks[SDL_SCANCODE_K]) k |= N64_BTN_CDOWN;
    if (ks[SDL_SCANCODE_J]) k |= N64_BTN_CLEFT;
    if (ks[SDL_SCANCODE_L]) k |= N64_BTN_CRIGHT;
    if (ks[SDL_SCANCODE_T]) k |= N64_BTN_DUP;
    if (ks[SDL_SCANCODE_G]) k |= N64_BTN_DDOWN;
    if (ks[SDL_SCANCODE_F]) k |= N64_BTN_DLEFT;
    if (ks[SDL_SCANCODE_H]) k |= N64_BTN_DRIGHT;
    if (ks[SDL_SCANCODE_LEFT]) *kx -= 32767;
    if (ks[SDL_SCANCODE_RIGHT]) *kx += 32767;
    if (ks[SDL_SCANCODE_UP]) *ky -= 32767;
    if (ks[SDL_SCANCODE_DOWN]) *ky += 32767;
    return k;
}
#endif

typedef struct pad_in {
    int any; /* a host pad drives this controller */
    uint16_t buttons;
    int8_t x, y;
    uint32_t hk_held, hk_pressed; /* the hotkeys a pad holds / pressed this frame (recomp_hotkeys.h, XBA-190) */
} pad_in;

static recomp_hotkeys_state s_hk_pad[HOST_PAD_PORTS]; /* recomp_hotkeys_pad_update's state per host pad */

/* The host pads for N64 controller `port`, through the player's mapping (the on-screen pad joins host pad 1,
 * console_pad_read; the mapping's range / deadzone, when set, over the runtime's): with one player every pad drives
 * controller 1 (buttons merged, the most deflected stick wins); with more, host pad N drives controller N. */
static void pads_read(int port, int players, int range, int dead, pad_in *out)
{
    memset(out, 0, sizeof *out);
    if (!s_pad_map.console)
        console_pad_defaults(&s_pad_map, CONSOLE_PAD_N64);
    if (s_pad_map.range > 0)
        range = s_pad_map.range;
    if (s_pad_map.deadzone >= 0)
        dead = 32767 * s_pad_map.deadzone / 100;
    for (int p = 0; p < HOST_PAD_PORTS; p++) {
        if (players >= 2 ? p != port : port != 0)
            continue;
        ConsolePadOut o;
        if (!console_pad_read(&s_pad_map, p, &o))
            continue;
        out->any = 1;
        HostPadState st;
        uint32_t src = host_pad_read(p, &st) ? console_pad_sources(&st, s_pad_map.stick_threshold, RECOMP_HK_TRIGGER) : 0;
        uint32_t pressed = 0, swallow = 0;
        out->hk_held |= recomp_hotkeys_pad_update(recomp_hotkeys_get(), &s_hk_pad[p], src, &pressed, &swallow);
        out->hk_pressed |= pressed;
        console_pad_swallow(&s_pad_map, &o, swallow); /* a held chord's buttons: not the on-screen pad's */
        out->buttons |= n64_runtime_buttons_of(o.buttons);
        int8_t x, y;
        n64_runtime_stick(o.stick_x[0], o.stick_y[0], range, dead, &x, &y);
        if (abs(x) + abs(y) > abs(out->x) + abs(out->y)) {
            out->x = x;
            out->y = y;
        }
    }
}

/* The rumble pak's motor to the host pads (host_pad_rumble, both motors): with one player every pad (they all drive
 * controller 1), with more host pad N for controller N. Refreshed every frame while on (so a lost update cannot leave
 * a pad buzzing for long), stopped once when off. */
static void rumble_drive(n64_machine *m, int players, int sent[4])
{
    for (int port = 0; port < (players >= 2 ? players : 1); port++) {
        int on = n64_rumble(m, port);
        if (!on && !sent[port])
            continue;
        for (int p = 0; p < HOST_PAD_PORTS; p++)
            if (players >= 2 ? p == port : 1)
                host_pad_rumble(p, on ? 0xC000 : 0, on ? 0x8000 : 0, on ? 100 : 0);
        sent[port] = on;
    }
}

static void on_tty(void *ctx, char c)
{
    static char line[256];
    static size_t n;
    (void)ctx;
    if (c != '\n' && n < sizeof line - 1) {
        line[n++] = c;
        return;
    }
    line[n] = 0;
    printf("n64: tty: %s\n", line);
    fflush(stdout);
    n = 0;
}

void n64_runtime_opts_init(n64_runtime_opts *o)
{
    memset(o, 0, sizeof *o);
    o->scale = -1;
    o->filter = -1;
    o->crop_edges = -1;
    o->pak = N64_RT_PAK_DEFAULT;
    o->players = -1;
    o->rdp_threads = -1;
    o->force_pal = -1;
    o->tty = -1;
    o->frameskip = N64_RT_FRAMESKIP_ENV;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static const char *pak_name(int pak)
{
    return pak == N64_RT_PAK_RUMBLE ? "rumble pak" : pak == N64_RT_PAK_NONE ? "no pak" : "controller pak";
}

static const char *save_kind_name(int kind)
{
    switch (kind) {
    case N64_SAVE_EEPROM4K: return "EEPROM 4K";
    case N64_SAVE_EEPROM16K: return "EEPROM 16K";
    case N64_SAVE_SRAM: return "SRAM";
    case N64_SAVE_FLASH: return "FlashRAM";
    default: return "none yet";
    }
}

void n64_runtime_display(const n64_runtime_opts *o, int *scale, int *filter)
{
    /* RECOMP_N64_SCALE / _FILTER, else the shared RECOMP_SCALE / RECOMP_FILTER (XBA-181); smooth by default. */
    int s = o && o->scale >= 0 ? o->scale : host_window_env_scale("RECOMP_N64_SCALE", HOST_SCALE_ASPECT);
    int f = o && o->filter >= 0 ? o->filter : host_window_env_filter("RECOMP_N64_FILTER", HOST_FILTER_LINEAR);
    if (scale)
        *scale = s;
    if (filter)
        *filter = f;
}

int n64_runtime_crop_edges(const n64_runtime_opts *o)
{
    if (o && o->crop_edges >= 0)
        return o->crop_edges != 0;
    const char *v = getenv("RECOMP_N64_CROP_EDGES");
    return !(v && v[0] == '0');
}

int n64_runtime_crop_columns(int w)
{
    int c = w * 8 / 640; /* the canvas is 640 VI pixels wide, whatever its width in output pixels */
    return w > 4 * c ? c : 0;
}

int n64_runtime_run(const n64_runtime_opts *o)
{
    if (!o || !o->rom || !*o->rom) {
        fprintf(stderr, "n64: no ROM\n");
        return -1;
    }
    size_t rom_n = 0;
    uint8_t *rom = read_all(o->rom, N64_ROM_MAX, &rom_n);
    if (!rom) {
        fprintf(stderr, "n64: cannot read the ROM %s (missing, empty, over 64 MB or no permission)\n", o->rom);
        return -1;
    }
    char key[64];
    if (o->title_key && *o->title_key)
        snprintf(key, sizeof key, "%s", o->title_key);
    else if (n64_runtime_fallback_key(rom, rom_n, key, sizeof key) != 0)
        snprintf(key, sizeof key, "N64-UNKNOWN");

    n64_options mo = {.force_pal = o->force_pal >= 0 ? o->force_pal != 0 : -1, .save_kind = N64_SAVE_NONE};
    /* A pack's source identity (n64_pack.h): the SHA-256 of the ROM in big-endian order, computed only when there is
     * a pack to check (the ROM is in memory already). */
    char src_sha[65] = "";
    double sha_ms = 0;
    if (o->pack_lib && *o->pack_lib) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        n64_pack_rom_sha256(rom, rom_n, src_sha);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        sha_ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    }
    char err[256] = "";
    /* The RDP's band threads, as they start: their fast cores put back after a resume, the performance hint. */
    n64_rdp_set_thread_hook(recomp_host_worker_hook, NULL);
    n64_machine *m = n64_create(rom, rom_n, &mo, err, sizeof err);
    free(rom);
    if (!m) {
        fprintf(stderr, "n64: the machine cannot start: %s\n", err[0] ? err : "?");
        return -1;
    }
    const n64_rom_info *info = n64_info(m);
    const char *order = info->order == N64_ROM_V64 ? "v64" : info->order == N64_ROM_N64 ? "n64" : "z64";
    int pal = o->force_pal >= 0 ? o->force_pal != 0 : info->pal;
    printf("n64: %s, ROM %s (%s, \"%s\", %s, %zu KB), key %s, CIC %s, %s\n", n64_runtime_version(), base_name(o->rom),
           order, info->title, info->game_code, rom_n / 1024, key, n64_cic_name(info->cic),
           pal ? "PAL (50 Hz)" : "NTSC (60 Hz)");
    if (o->pack_lib && *o->pack_lib)
        printf("n64: ROM identity %.16s... (%.0f ms)\n", src_sha, sha_ms);
    n64_pack pack;
    n64_pack_load(m, o->pack_lib, key, src_sha, &pack); /* the interpreter whatever happens (logged) */

    /* The RDP's scanline-band threads: 2 by default on Android (the phone's big cores), the core's elsewhere. */
    int threads = o->rdp_threads > 0 ? o->rdp_threads : env_int("RECOMP_N64_RDP_THREADS", 0);
#ifdef __ANDROID__
    if (threads <= 0 && !getenv("XBR_N64_RDP_THREADS"))
        threads = 2;
#endif
    struct n64_rdp *rdp = n64_get_rdp(m);
    int rdp_threads = 1;
    if (rdp) /* without a choice, the count n64_rdp_create took from $XBR_N64_RDP_THREADS (or 1) */
        rdp_threads = n64_rdp_set_threads(rdp, threads > 0 ? threads : env_int("XBR_N64_RDP_THREADS", 1));

    int pak = o->pak != N64_RT_PAK_DEFAULT ? o->pak : n64_runtime_pak_parse(getenv("RECOMP_N64_PAK"));
    if (pak == N64_RT_PAK_DEFAULT)
        pak = N64_RT_PAK_CONTROLLER;
    int players = o->players >= 1 ? o->players : env_int("RECOMP_N64_PLAYERS", 1);
    players = players < 1 ? 1 : players > 4 ? 4 : players;
    int range = env_int("RECOMP_N64_STICK_RANGE", STICK_RANGE);
    int dead_pct = env_int("RECOMP_N64_DEADZONE", 8);
    int dead = dead_pct >= 0 && dead_pct <= 90 ? 32767 * dead_pct / 100 : -1;
    n64_accessory acc = pak == N64_RT_PAK_RUMBLE ? N64_ACC_RUMBLE_PAK
                        : pak == N64_RT_PAK_NONE ? N64_ACC_NONE : N64_ACC_CONTROLLER_PAK;
    for (int port = 0; port < 4; port++) {
        n64_set_accessory(m, port, acc);
        n64_set_controller(m, port, port < players, 0, 0, 0);
    }
    int fskip = o->frameskip != N64_RT_FRAMESKIP_ENV ? o->frameskip
                                                      : n64_runtime_frameskip_parse(getenv("RECOMP_N64_FRAMESKIP"));
    if (fskip > N64_RT_FRAMESKIP_MAX)
        fskip = N64_RT_FRAMESKIP_MAX;
    if (fskip < N64_RT_FRAMESKIP_AUTO)
        fskip = N64_RT_FRAMESKIP_AUTO;
    /* A picture is made, converted and uploaded only when it changed (n64_display_changed); RECOMP_N64_PRESENT_ALWAYS=1:
     * on every VI, as before. */
    int present_always = env_choice("RECOMP_N64_PRESENT_ALWAYS", "1", 0);
    if (fskip == N64_RT_FRAMESKIP_AUTO)
        printf("n64: frame skip auto (when late or the audio runs low, at most %s in a row)\n",
               present_always ? "3" : "1 new picture");
    else
        printf("n64: frame skip %d\n", fskip);
    printf("n64: present %s\n", present_always ? "every VI (RECOMP_N64_PRESENT_ALWAYS=1)"
                                                : "new pictures only (unchanged VIs show the last one again)");
    int tty = o->tty >= 0 ? o->tty : env_choice("RECOMP_N64_TTY", "1", 0);
    if (tty)
        n64_set_tty(m, on_tty, NULL);

    pthread_mutex_lock(&s_mu);
    s_m = m;
    snprintf(s_save_dir, sizeof s_save_dir, "%s", o->save_dir ? o->save_dir : "");
    snprintf(s_key, sizeof s_key, "%s", key);
    saves_load(m);
    snprintf(s_discovered, sizeof s_discovered, "%s", o->discovered_path ? o->discovered_path : "");
    pthread_mutex_unlock(&s_mu);
    recomp_exit_hook_set(on_exit_hook);
    static int listening;
    if (!listening && recomp_lifecycle_listen(on_lifecycle, NULL) == 0)
        listening = 1;
    /* Save states: <save dir>/n64-<KEY>/state<N>.st, beside the saves (none without a save dir). */
    if (!o->save_dir || !*o->save_dir ||
        snprintf(s_state_dir, sizeof s_state_dir, "%s/n64-%s", o->save_dir, key) >= (int)sizeof s_state_dir)
        s_state_dir[0] = 0;
    s_state_crop = n64_runtime_crop_edges(o);
    recomp_state_ops sops = {"n64", state_save_op, state_load_op, NULL};
    recomp_states_attach(&sops, s_state_dir);
    printf("n64: save states %s%s, fast-forward x%d%s\n", s_state_dir[0] ? "in " : "off (no save dir)", s_state_dir,
           recomp_ff_speed(), recomp_ff_speed() ? "" : " (max)");
    memset(s_hk_pad, 0, sizeof s_hk_pad);
    recomp_hotkeys_get(); /* RECOMP_HOTKEYS / _KEYS, logged once (XBA-190) */
    recomp_rewinder rw;
    recomp_rewinder_init(&rw, "n64", NULL); /* RECOMP_REWIND (off by default), _MB, _FRAMES (XBA-194) */
    recomp_cheats cheats;
    recomp_cheats_init(&cheats, RECOMP_CHEAT_N64);
    recomp_cheats_load_env(&cheats, "n64"); /* RECOMP_CHEATS_FILE (XBA-197) */
    recomp_cheat_bus cheat_bus = {m, NULL, NULL, cheat_peek32, cheat_poke32, 1, NULL, 0};
    unsigned cheat_polls = 0;
    int resume = recomp_states_resume_enabled() && s_state_dir[0];
    if (resume) {
        int r = recomp_states_resume();
        printf("n64: resume from the automatic slot: %s\n", r == RECOMP_STATE_EEMPTY ? "nothing saved yet"
                                                                                     : recomp_states_error(r));
    }

    int window = !o->headless && !host_window_headless();
    if (SDL_WasInit(SDL_INIT_EVENTS) == 0)
        SDL_InitSubSystem(SDL_INIT_EVENTS);
    int scale, filter;
    n64_runtime_display(o, &scale, &filter);
    host_window_set_scaling(scale, filter);
    host_window_shader_env("RECOMP_N64_SHADER"); /* XBA-195: picture shader */
    host_window_set_aspect(4, 3);
    int crop = n64_runtime_crop_edges(o);
    if (window) {
        char title[300];
        snprintf(title, sizeof title, "XRecomp N64 - %s", info->title[0] ? info->title : base_name(o->rom));
        if (!host_window_open(title, 320 * 3, 240 * 3, host_window_env_vsync())) {
            fprintf(stderr, "n64: no window, running headless\n");
            window = 0;
        }
    }
    if (!window)
        host_pad_init();
    console_pad_load(&s_pad_map, CONSOLE_PAD_N64, getenv(console_pad_env(CONSOLE_PAD_N64)));
    int audio = host_audio_open(N64_AUDIO_RATE, 2);
    printf("n64: window %s (4:3, %s, %s%s), audio %s, pads %u, %s%s, RDP %s%s, threads %d\n", window ? "on" : "off",
           scale == HOST_SCALE_INTEGER ? "integer" : scale == HOST_SCALE_STRETCH ? "stretch" : "aspect",
           filter == HOST_FILTER_NEAREST ? "nearest" : "linear", crop ? ", edges cropped" : "",
           audio ? "on" : "off", host_pad_mask(), pak_name(pak), players >= 2 ? ", several players" : "",
           n64_rdp_backend_name(rdp), rdp && n64_rdp_upscale(rdp) > 1 ? " upscaled" : "", rdp_threads);
    fflush(stdout);

    static uint32_t xrgb_1x[N64_SCREEN_MAX_W * N64_SCREEN_MAX_H];
    static uint8_t rgba_1x[N64_SCREEN_MAX_W * N64_SCREEN_MAX_H * 4];
    /* The Vulkan RDP's upscaled scanout (XBA-185, n64_rdp_scanout_scale): pictures that many times larger. */
    uint32_t *xrgb = xrgb_1x, *xrgb_up = NULL;
    uint8_t *rgba = rgba_1x, *rgba_up = NULL;
    int max_w = N64_SCREEN_MAX_W, max_h = N64_SCREEN_MAX_H;
    int sscale = rdp ? n64_rdp_scanout_scale(rdp) : 0;
    if (sscale > 1) {
        size_t n = (size_t)N64_SCREEN_MAX_W * (size_t)sscale * (size_t)N64_SCREEN_MAX_H * (size_t)sscale;
        xrgb_up = malloc(n * sizeof *xrgb_up);
        rgba_up = malloc(n * 4);
        if (xrgb_up && rgba_up) {
            xrgb = xrgb_up;
            rgba = rgba_up;
            max_w *= sscale;
            max_h *= sscale;
        } else {
            fprintf(stderr, "n64: no memory for the %dx picture, shown at 1x\n", sscale);
        }
    }
    /* The upscaled picture handed to the window straight from the Vulkan RDP's scanout buffer (n64_display_rgba): one
     * copy (the texture upload) instead of RGBA -> XRGB -> RGBA and the window's copy for screenshots.
     * RECOMP_N64_PRESENT_DIRECT=0: through the XRGB buffer as before. */
    const int rgba_direct = window && xrgb_up && xrgb == xrgb_up && !env_choice("RECOMP_N64_PRESENT_DIRECT", "0", 0);
    static int16_t pcm[N64_AUDIO_RATE / 10 * 2];
    int dw = 320, dh = 240, last_w = 0, last_h = 0, display_on = -1, last_kind = N64_SAVE_NONE;
    uint64_t frames = 0, next = now_ns(), t0 = next, late_resets = 0, dirty_at = 0, dirty_first = 0;
    uint64_t fps_t = t0, fps_frames = 0, last_frame_ns = 0;
    int was_paused = 0, heard = 0, rc = 0;
    int rumble_sent[4] = {0, 0, 0, 0};
    int rumble_on = host_pad_rumble_env(); /* RECOMP_RUMBLE=0 turns the Rumble Pak off (XBA-181) */
    int64_t behind = 0; /* how late this frame starts (pacing below) */
    int skip_run = 0;
    uint64_t skipped = 0, skipped_log = 0;
    n64_runtime_stuck stuck;
    n64_runtime_stuck_init(&stuck);
    uint64_t fault_logged = 0;
    int was_ff = 0;
    recomp_hotkeys_state hk_keys = {0};
    recomp_ff_carry carry = {{0, 0}, 0};
    /* The performance line (n64_runtime_perf): the core times one frame in 8 stage by stage. */
    rt_perf_acc pacc;
    memset(&pacc, 0, sizeof pacc);
    rt_perf_mark pm_run, pm_line;
    n64_perf_enable(m, 8);
    pthread_mutex_lock(&s_mu);
    perf_mark(&pm_run, m, &pacc, t0);
    pthread_mutex_unlock(&s_mu);
    pm_line = pm_run;
    uint64_t p_iter = 0, p_iter_idle = 0, p_paused_at = 0, p_frame_ns = 16683333;
    rt_worst p_worst, p_worst_run; /* the worst host frame of the line, of the run */
    memset(&p_worst, 0, sizeof p_worst);
    memset(&p_worst_run, 0, sizeof p_worst_run);
    rt_perf_acc p_iter_acc = pacc; /* the stages and the RDP's counters at the host frame's start */
    n64_rdp_stats p_iter_rdp;
    memset(&p_iter_rdp, 0, sizeof p_iter_rdp);
    recomp_perf_hint_begin(16683333); /* the frame period; each frame's own below */

    for (;;) {
        int running = 1;
        if (window) {
            running = host_window_pump();
        } else {
            SDL_Event ev;
            host_pad_update();
            running = SDL_PeepEvents(&ev, 1, SDL_GETEVENT, SDL_QUIT, SDL_QUIT) <= 0; /* SIGINT, a test */
            SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
        }
        if (!running) {
            printf("n64: quit asked after %llu frames\n", (unsigned long long)frames);
            if (resume && frames > 0) /* the player closed the window: the next start resumes here */
                printf("n64: automatic slot %s\n", recomp_states_error(recomp_states_save(RECOMP_STATE_AUTO)));
            break;
        }
        missed_flush_every(now_ns());
        if (recomp_lifecycle_paused()) {
            if (!was_paused) {
                printf("n64: paused at frame %llu\n", (unsigned long long)frames);
                fflush(stdout);
                saves_flush("pause");
                missed_flush();
                rumble_sent[0] = rumble_sent[1] = rumble_sent[2] = rumble_sent[3] = 1; /* stopped below on resume */
                for (int p = 0; p < HOST_PAD_PORTS; p++)
                    host_pad_rumble(p, 0, 0, 0);
                dirty_at = dirty_first = 0;
                was_paused = 1;
                p_paused_at = now_ns();
            }
            SDL_Delay(16);
            continue;
        }
        if (was_paused) {
            printf("n64: resumed\n");
            fflush(stdout);
            was_paused = 0;
            next = now_ns(); /* the time away is not caught up */
            fps_t = next;
            fps_frames = 0;
            pm_run.t += next - p_paused_at; /* the time away is not the run's */
            pthread_mutex_lock(&s_mu);
            perf_mark(&pm_line, m, &pacc, next);
            pthread_mutex_unlock(&s_mu);
            memset(&p_worst, 0, sizeof p_worst);
            p_iter = 0;
        }
        { /* the host frame that ended: its length without the sleep, and for the worst one its own breakdown */
            uint64_t it = now_ns();
            n64_rdp_stats rs;
            pthread_mutex_lock(&s_mu);
            n64_rdp_get_stats(rdp, &rs);
            pthread_mutex_unlock(&s_mu);
            if (p_iter) {
                uint64_t busy = it - p_iter - p_iter_idle;
                pacc.host_frames++;
                pacc.over_1 += busy > p_frame_ns;
                pacc.over_2 += busy > 2 * p_frame_ns;
                if (busy > p_worst.busy || busy > p_worst_run.busy) {
                    rt_worst w = {busy, pacc.core - p_iter_acc.core, pacc.vi - p_iter_acc.vi,
                                  pacc.present - p_iter_acc.present, pacc.audio - p_iter_acc.audio,
                                  rs.queue_ns - p_iter_rdp.queue_ns, rs.wait_ns - p_iter_rdp.wait_ns,
                                  rs.scanout_ns - p_iter_rdp.scanout_ns, rs.dp_wait_ns - p_iter_rdp.dp_wait_ns};
                    if (busy > p_worst.busy)
                        p_worst = w;
                    if (busy > p_worst_run.busy)
                        p_worst_run = w;
                }
            }
            p_iter = it;
            p_iter_idle = 0;
            p_iter_acc = pacc;
            p_iter_rdp = rs;
        }
        uint64_t work_t0 = now_ns(); /* the frame's work, for the performance hint (the sleep after it is not) */

        pad_in pads[4];
        uint32_t hk_held = 0, hk_pressed = 0;
        for (int port = 0; port < players; port++) {
            pads_read(port, players, range, dead, &pads[port]);
            hk_held |= pads[port].hk_held;
            hk_pressed |= pads[port].hk_pressed;
        }
        pads[0].buttons |= o->buttons;
#ifndef __ANDROID__ /* a device's own controls may also come as keys (arrows for its D-pad): not the stick there */
        if (window) {
            int kx, ky;
            pads[0].buttons |= keyboard_buttons(&kx, &ky);
            if (kx || ky)
                n64_runtime_stick(kx, ky, range, 0, &pads[0].x, &pads[0].y);
        }
#endif
        if (window) { /* the desktop keys (RECOMP_HOTKEYS_KEYS: Tab, F1-F4, Shift+F1-F4 by default) */
            int nks = 0;
            uint32_t key_pressed = 0;
            const Uint8 *ks = SDL_GetKeyboardState(&nks);
            hk_held |= recomp_hotkeys_keys_update(recomp_hotkeys_get(), &hk_keys, ks, nks, &key_pressed);
            hk_pressed |= key_pressed;
        }
        recomp_hotkeys_run(hk_pressed);
        recomp_ff_hold(!!(hk_held & RECOMP_HK_BIT(RECOMP_HK_FF_HOLD)));
        int rewinding = recomp_rewinder_begin(&rw, !!(hk_held & RECOMP_HK_BIT(RECOMP_HK_REWIND)));
        int ff = recomp_ff_active(), speed = recomp_ff_speed();
        if (ff != was_ff) {
            printf("n64: fast-forward %s at frame %llu\n", ff ? (speed ? "on" : "on (max)") : "off",
                   (unsigned long long)frames);
            fflush(stdout);
            carry = (recomp_ff_carry){{0, 0}, 0};
            was_ff = ff;
            next = now_ns();
            behind = 0;
        }
        /* Fast-forward: `speed` N64 frames per shown frame (max: as many as fit in one display frame), no frame skip
         * (only the last frame's picture is made anyway). */
        int steps = rewinding ? 0 : ff ? (speed ? speed : 64) : 1, dirty = 0, skip = 0, on = display_on > 0;
        int fresh = 1; /* a new picture made this host frame (0: the VI's picture had not changed, nothing made) */
        const uint8_t *prgba = NULL; /* the picture straight from the Vulkan RDP's scanout (n64_display_rgba) */
        int ppitch = 0;
        uint64_t frame_ns = 16683333, burst_end = now_ns() + 15000000;
        if (rewinding) { /* rewind held: one capture back per shown frame, no N64 frame runs, nothing heard */
            pthread_mutex_lock(&s_mu);
            if (recomp_rewinder_step(&rw, rewind_load, m) == 0)
                recomp_states_note_load(); /* a fresh frame generation history */
            uint64_t tv = now_ns();
            on = n64_display(m, xrgb, max_w, max_h, &dw, &dh) == 0;
            pacc.vi += now_ns() - tv;
            pacc.pictures++;
            pthread_mutex_unlock(&s_mu);
        }
        if (++cheat_polls % 30 == 0)
            recomp_cheats_poll(&cheats); /* the app changed the list: read it again */
        for (int step = 0; step < steps; step++) {
            pthread_mutex_lock(&s_mu);
            for (int port = 0; port < players; port++)
                n64_set_controller(m, port, 1, pads[port].buttons, pads[port].x, pads[port].y);
            uint64_t c0 = n64_cycles(m), tc = now_ns();
            n64_run_frame(m);
            pacc.core += now_ns() - tc;
            frame_ns = n64_runtime_frame_ns(n64_cycles(m) - c0);
            p_frame_ns = frame_ns;
            if (cheats.n)
                recomp_cheats_apply(&cheats, &cheat_bus);
            recomp_rewinder_frame(&rw, rewind_size, rewind_save, m);
            if (s_state_loaded) {
                s_state_loaded = 0;
                next = now_ns(); /* a loaded state: the clock starts again from it */
                behind = 0;
            }
            guest_health(m, &stuck, &fault_logged, frame_ns);
            int kind = n64_save_kind_detected(m);
            int cart_dirty = kind != N64_SAVE_NONE && n64_save_take_dirty(m, (n64_save_kind)kind);
            if (kind != last_kind && kind != N64_SAVE_NONE && !cart_dirty && !s_saved[kind])
                snapshot(m, kind); /* detected at its first access: written once the game changes it */
            dirty |= cart_dirty;
            for (int p = 0; p < 4; p++)
                dirty |= n64_save_take_dirty(m, (n64_save_kind)(N64_SAVE_PAK0 + p));
            int last = step == steps - 1 || !ff || (o->frames && frames + 1 >= o->frames) ||
                       (!speed && now_ns() >= burst_end);
            if (last) {
                /* Frame skip: the picture of a frame the host is late for is not made nor shown (the frame itself
                 * ran). */
                /* An unchanged picture is neither made nor skipped: frame skip decides on new pictures only (and
                 * a picture skipped is still new at the next VI). */
                int audio_low = audio && heard && !o->unpaced && host_audio_queued() < AUDIO_TARGET_FRAMES / 4;
                int forced = (o->frames && frames + 1 >= o->frames) ||
                             (o->capture_path && o->capture_frame == frames + 1);
                fresh = present_always || forced || n64_display_changed(m);
                int fmode = o->unpaced && fskip < 0 ? 0 : fskip;
                int64_t fbehind = o->unpaced ? 0 : behind;
                skip = ff || !fresh ? 0
                       : present_always ? n64_runtime_frameskip(fmode, fbehind, frame_ns, audio_low, &skip_run)
                                        : n64_runtime_frameskip_picture(fmode, fbehind, frame_ns, audio_low, &skip_run);
                if (forced)
                    skip = 0; /* the last frame of a bounded run is the screenshot; a capture is a capture */
                uint64_t tv = now_ns();
                if (skip || !fresh) {
                    on = display_on > 0;
                } else {
                    /* Upscaled: the scanout's own RGBA, no conversion nor copy (the last frame of a bounded run
                     * keeps the XRGB picture: --screenshot writes it). */
                    int last_frame = o->frames && frames + 1 >= o->frames;
                    int r = rgba_direct && !last_frame ? n64_display_rgba(m, &prgba, &dw, &dh, &ppitch) : -2;
                    if (r != 0) { /* -1: the upscaled scanout shows nothing, the 1x picture (without a second try) */
                        prgba = NULL;
                        on = n64_display(m, xrgb, r == -1 ? N64_SCREEN_MAX_W : max_w,
                                         r == -1 ? N64_SCREEN_MAX_H : max_h, &dw, &dh) == 0;
                    } else {
                        on = 1;
                    }
                }
                pacc.vi += now_ns() - tv;
                skipped += (uint64_t)skip;
                pacc.skipped += (uint64_t)skip;
                pacc.pictures += (uint64_t)(fresh && !skip);
                pacc.unchanged += (uint64_t)!fresh;
            }
            if (rumble_on)
                rumble_drive(m, players, rumble_sent); /* RECOMP_RUMBLE=0: never */
            pthread_mutex_unlock(&s_mu);
            frames++;
            fps_frames++;
            if (kind != last_kind) {
                printf("n64: cartridge save type %s at frame %llu\n", save_kind_name(kind), (unsigned long long)frames);
                last_kind = kind;
            }
            if (frames == 2 ||
                (frames > 2 && (frame_ns > last_frame_ns + 500000 || last_frame_ns > frame_ns + 500000))) {
                printf("n64: VI rate %.2f Hz at frame %llu\n", 1e9 / (double)frame_ns, (unsigned long long)frames);
                last_frame_ns = frame_ns;
            }
            /* Sound: everything the frame made (about 800 sample frames at 60 Hz); averaged down while
             * fast-forwarding. */
            size_t got;
            uint64_t ta = now_ns();
            while ((got = n64_audio_pull(m, pcm, sizeof pcm / sizeof pcm[0] / 2)) > 0) {
                heard = 1;
                if (ff)
                    got = speed ? recomp_ff_audio(pcm, got, speed, &carry) : 0;
                if (audio && got)
                    host_audio_queue(pcm, (uint32_t)got);
            }
            pacc.audio += now_ns() - ta;
            if (last)
                break;
        }

        if (skip || !fresh) {
            /* keep the last picture and its size */
        } else if (prgba && on && dw > 0 && dh > 0) {
            if (dw != last_w || dh != last_h) {
                printf("n64: display %d x %d at frame %llu\n", dw, dh, (unsigned long long)frames);
                last_w = dw;
                last_h = dh;
            }
        } else if (!on || dw <= 0 || dh <= 0 || dw > max_w || dh > max_h) {
            prgba = NULL;
            dw = last_w ? last_w : 320;
            dh = last_h ? last_h : 240;
            memset(xrgb, 0, (size_t)dw * (size_t)dh * sizeof xrgb[0]);
        } else if (dw != last_w || dh != last_h) {
            printf("n64: display %d x %d at frame %llu\n", dw, dh, (unsigned long long)frames);
            last_w = dw;
            last_h = dh;
        }
        if (on != display_on) {
            if (display_on >= 0 || !on)
                printf("n64: display %s at frame %llu\n", on ? "on" : "off", (unsigned long long)frames);
            display_on = on;
        }

        uint64_t t = now_ns();
        if (dirty) {
            dirty_at = t;
            if (!dirty_first)
                dirty_first = t;
        }
        if (dirty_at && (t - dirty_at >= SAVE_DEBOUNCE_NS || t - dirty_first >= SAVE_LATEST_NS)) {
            saves_flush("written by the game");
            dirty_at = dirty_first = 0;
        }
        if (t - fps_t >= FPS_LOG_NS) {
            printf("n64: %.1f fps over the last %.0f s (VI %.1f Hz), %llu frames skipped, %llu late resets so far, "
                   "pc %08llx\n",
                   (double)fps_frames * 1e9 / (double)(t - fps_t), (double)(t - fps_t) / 1e9, 1e9 / (double)frame_ns,
                   (unsigned long long)(skipped - skipped_log), (unsigned long long)late_resets,
                   (unsigned long long)(n64_pc(m) & 0xFFFFFFFFu));
            skipped_log = skipped;
            rt_perf_mark pm_now;
            n64_runtime_perf pf;
            char line[1024];
            pthread_mutex_lock(&s_mu);
            perf_mark(&pm_now, m, &pacc, t);
            pthread_mutex_unlock(&s_mu);
            perf_fill(&pf, &pm_line, &pm_now, t - pm_line.t, &p_worst, frame_ns, pack.table != NULL, rdp,
                      rdp_threads, rw.ring != NULL);
            n64_runtime_perf_format(&pf, line, sizeof line);
            printf("n64: perf %s\n", line);
            n64_runtime_perf_detail_format(&pf, line, sizeof line);
            printf("n64: perf detail %s\n", line);
            pm_line = pm_now;
            memset(&p_worst, 0, sizeof p_worst);
            fflush(stdout);
            fps_t = t;
            fps_frames = 0;
        }

        if (window && !skip && !fresh) {
            /* The same picture: the window's texture drawn again (frame generation: nothing, it paces itself). */
            uint64_t tp = now_ns();
            host_window_present_again();
            pacc.present += now_ns() - tp;
        } else if (window && !skip) {
            uint64_t tp = now_ns();
            if (o->capture_path && o->capture_frame == frames)
                host_window_capture(o->capture_path);
            /* The VI's guard band (RECOMP_N64_CROP_EDGES): the columns either side are left out of the texture and
             * the rest still fills the 4:3 shape. */
            int cc = crop ? n64_runtime_crop_columns(dw) : 0;
            if (prgba) { /* one copy, GPU buffer -> texture (the shader shows it opaque: the VI's alpha is ignored) */
                pthread_mutex_lock(&s_mu);
                s_shared.px = prgba + 4 * cc;
                s_shared.w = dw - 2 * cc;
                s_shared.h = dh;
                s_shared.pitch = ppitch;
                pthread_mutex_unlock(&s_mu);
                host_window_present_rgba_shared(prgba + 4 * cc, dw - 2 * cc, dh, ppitch, shared_last, NULL);
            } else {
                n64_runtime_xrgb_to_rgba(xrgb, rgba, (size_t)dw * (size_t)dh);
                host_window_present_rgba(rgba + 4 * cc, dw - 2 * cc, dh, dw * 4);
            }
            pacc.present += now_ns() - tp;
        }
        if (o->frames && frames >= o->frames)
            break;
        recomp_perf_hint_frame((int64_t)(now_ns() - work_t0), (int64_t)frame_ns);

        if (!o->unpaced && !(ff && !speed)) {
            next += frame_ns;
            /* Audio-driven: a queue above its target slows the clock a little, below it speeds it up (±2%). */
            if (audio && heard) {
                uint32_t q = host_audio_queued();
                if (q > AUDIO_TARGET_FRAMES * 2)
                    next += frame_ns / 50;
                else if (q < AUDIO_TARGET_FRAMES / 2)
                    next -= frame_ns / 50;
            }
            t = now_ns();
            behind = (int64_t)(t - next);
            if (t > next + 3 * frame_ns) {
                next = t; /* a stall (a slow host, the debugger): no burst to catch up */
                late_resets++;
            } else if (next > t) {
                sleep_until(next);
                uint64_t slept = now_ns() - t;
                pacc.idle += slept;
                p_iter_idle += slept;
            }
        }
    }

    recomp_perf_hint_end();
    if (rgba_direct) { /* the RDP's buffers go with the machine: the window keeps its own copy of the last picture */
        host_window_keep_shared();
        pthread_mutex_lock(&s_mu);
        s_shared.px = NULL;
        pthread_mutex_unlock(&s_mu);
    }
    uint64_t t_end = now_ns();
    double secs = (double)(t_end - t0) / 1e9;
    printf("n64: %llu frames in %.1f s (%.2f fps), %llu skipped, %llu late resets\n", (unsigned long long)frames, secs,
           secs > 0 ? (double)frames / secs : 0.0, (unsigned long long)skipped, (unsigned long long)late_resets);
    {
        rt_perf_mark pm_end;
        char line[1024];
        if (was_paused)
            pm_run.t += t_end - p_paused_at;
        pthread_mutex_lock(&s_mu);
        perf_mark(&pm_end, m, &pacc, t_end);
        pthread_mutex_unlock(&s_mu);
        perf_fill(&s_perf_last, &pm_run, &pm_end, t_end - pm_run.t, &p_worst_run, p_frame_ns,
                  pack.table != NULL, rdp, rdp_threads, rw.ring != NULL);
        n64_runtime_perf_format(&s_perf_last, line, sizeof line);
        printf("n64: perf over the run: %s\n", line);
        n64_runtime_perf_detail_format(&s_perf_last, line, sizeof line);
        printf("n64: perf detail over the run: %s\n", line);
    }
    saves_flush("exit");
    if (o->screenshot) {
        if (write_ppm(o->screenshot, xrgb, dw, dh) == 0)
            printf("n64: last frame (%d x %d) written to %s\n", dw, dh, o->screenshot);
        else
            rc = -1;
    }
    free(xrgb_up);
    free(rgba_up);
    fflush(stdout);
    if (rumble_sent[0] || rumble_sent[1] || rumble_sent[2] || rumble_sent[3])
        for (int p = 0; p < HOST_PAD_PORTS; p++)
            host_pad_rumble(p, 0, 0, 0);
    recomp_exit_hook_set(NULL);
    recomp_states_detach(); /* before s_mu: a save / load in progress finishes first */
    recomp_ff_hold(0);
    recomp_rewinder_free(&rw); /* logs the capture cost */
    recomp_cheats_free(&cheats);
    if (audio)
        host_audio_close();
    if (window)
        host_window_close();
    host_window_set_aspect(0, 0);
    pthread_mutex_lock(&s_mu);
    s_m = NULL;
    s_save_dir[0] = 0;
    saves_clear();
    if (s_discovered[0])
        n64_pack_save_missed(m, s_discovered);
    s_discovered[0] = 0;
    pthread_mutex_unlock(&s_mu);
    n64_destroy(m);
    n64_pack_close(&pack);
    return rc;
}

/* ---- The app shell's entry ------------------------------------------------------------------------------------ */

static char s_start_save_dir[1024];
static char s_start_work_dir[1024];

void n64_runtime_set_work_dir(const char *dir)
{
    snprintf(s_start_work_dir, sizeof s_start_work_dir, "%s", dir ? dir : "");
}

void n64_runtime_set_save_dir(const char *dir)
{
    snprintf(s_start_save_dir, sizeof s_start_save_dir, "%s", dir ? dir : "");
}

int n64_runtime_start(const char *rom, const char *pack_lib, const char *title_key)
{
    n64_runtime_opts o;
    n64_runtime_opts_init(&o);
    o.rom = rom;
    o.title_key = title_key;
    o.save_dir = s_start_save_dir[0] ? s_start_save_dir : NULL;
    o.pack_lib = pack_lib;
    char discovered[1200];
    if (s_start_work_dir[0] && title_key && *title_key) {
        snprintf(discovered, sizeof discovered, "%s/n64-%s/discovered.txt", s_start_work_dir, title_key);
        o.discovered_path = discovered;
    }
    const char *frames = getenv("RECOMP_N64_FRAMES"); /* a bounded run (checks) */
    if (frames && *frames)
        o.frames = strtoull(frames, NULL, 10);
    printf("n64 runtime ready (%s; title %s, pack %s, game %s)\n", n64_runtime_version(), title_key ? title_key : "?",
           pack_lib && *pack_lib ? pack_lib : "none (interpreter)", rom ? rom : "?");
    fflush(stdout);
    return n64_runtime_run(&o);
}
