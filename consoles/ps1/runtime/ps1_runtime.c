/* ps1_runtime.c -- see ps1_runtime.h (XBA-146, XBA-168).
 *
 * The GBA runtime's shape (consoles/gba/runtime/gba_runtime.c): one thread does everything, frame by frame: pump the
 * window's events (quit, background, pads), read the pads, run one PS1 frame (to the next VBlank), queue its audio,
 * present the GPU's display, then sleep to the next frame's deadline. The frame's length comes from the CPU cycles
 * the core ran (its scanline scheduler follows the GPU's video mode: NTSC or PAL, interlaced or not), so pacing is the
 * PS1's own field rate; absolute deadlines (no drift), nudged by the audio queue once the SPU makes sound, a stall of
 * more than three frames resets the clock. While the app is paused (background, the in-game menu: recomp_lifecycle) no
 * frame runs, the memory cards are written and the window keeps pumping.
 *
 * Save states and fast-forward (XBA-182, recomp_states.h), the GBA runtime's way: the run attaches itself while it
 * plays; the in-game menu and the hotkeys (XBA-190, recomp_hotkeys.h; desktop F1-F4 / Shift+F1-F4 / Tab by default) save and load
 * between frames under s_mu; fast-forward runs `speed` frames per shown frame, their audio averaged down.
 * Rewind (XBA-194, recomp_rewind.h), the same way too: a capture every RECOMP_REWIND_FRAMES frames (3 by default),
 * one step back per shown frame while the hotkey is held (Select + L2, R), silent.
 * Cheats (XBA-197, recomp_cheats.h): RECOMP_CHEATS_FILE's enabled codes run after every PS1 frame under s_mu through
 * ps1_peek32 / ps1_poke32 (a write that changes code a pack runs from RAM moves that page's write counter: the
 * interpreter takes over that function); the file is read again when the app changes it (every 30 shown frames).
 *
 * Picture: ps1_display gives the display area (256 / 320 / 368 / 512 / 640 x 240 / 480, 15- or 24-bit) as XRGB; it is
 * shown at 4:3 whatever its width (host_window_set_aspect), as a television did. At an internal resolution above 1x
 * (XBA-184, RECOMP_UPSCALE) the GPU draws a shadow VRAM N times the size too and the picture is N times wider and
 * higher (up to 2560 x 2048 at 4x); the frame buffers are sized for it. */
#define _GNU_SOURCE /* pread, nanosleep, readlink */
#include "ps1_runtime.h"

#include "host_audio.h"
#include "console_pad.h"
#include "host_pad.h"
#include "host_perf_hint.h"
#include "host_window.h"
#include "ps1_core.h"
#include "ps1_devices.h"
#include "ps1_pack.h"
#include "recomp_cheats.h"
#include "recomp_exit_hook.h"
#include "recomp_hotkeys.h"
#include "recomp_lifecycle.h"
#include "recomp_rewind.h"
#include "recomp_states.h"

#include <SDL.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PS1_EXE_MAX (PS1_RAM_SIZE + 0x800u) /* header + at most the whole RAM */
#define SAVE_DEBOUNCE_NS 1000000000ull      /* a second after the last write to a card */
#define SAVE_LATEST_NS 5000000000ull        /* and at most five after the first */
#define AUDIO_TARGET_FRAMES (PS1_AUDIO_RATE / 20) /* 50 ms queued */
#define DISPLAY_MAX_W 640
#define DISPLAY_MAX_H 512

const char *ps1_runtime_version(void)
{
    return "ps1_runtime " PS1_RUNTIME_VERSION;
}

/* ---- Pure helpers --------------------------------------------------------------------------------------------- */

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    uint32_t crc = 0xffffffffu;
    while (n--)
        crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

/* The player's mapping (console_pad.h, RECOMP_PS1_PAD_MAP), loaded when a game starts; the defaults until then. */
static ConsolePadMap s_pad_map;

uint16_t ps1_runtime_pad_of(const ConsolePadOut *o, int stick_threshold, int analog)
{
    static const uint16_t bits[PS1_T_COUNT] = {
        [PS1_T_CROSS] = PS1_PAD_CROSS, [PS1_T_CIRCLE] = PS1_PAD_CIRCLE, [PS1_T_SQUARE] = PS1_PAD_SQUARE,
        [PS1_T_TRIANGLE] = PS1_PAD_TRIANGLE, [PS1_T_L1] = PS1_PAD_L1, [PS1_T_R1] = PS1_PAD_R1,
        [PS1_T_L2] = PS1_PAD_L2, [PS1_T_R2] = PS1_PAD_R2, [PS1_T_L3] = PS1_PAD_L3, [PS1_T_R3] = PS1_PAD_R3,
        [PS1_T_START] = PS1_PAD_START, [PS1_T_SELECT] = PS1_PAD_SELECT, [PS1_T_UP] = PS1_PAD_UP,
        [PS1_T_DOWN] = PS1_PAD_DOWN, [PS1_T_LEFT] = PS1_PAD_LEFT, [PS1_T_RIGHT] = PS1_PAD_RIGHT,
    };
    uint16_t k = 0;
    for (int i = 0; i < PS1_T_COUNT; i++)
        if (o->buttons & (1u << i))
            k |= bits[i];
    if (!analog) {   /* digital: the DualShock's left stick (as mapped) is a D-pad too */
        int lx = o->stick_x[0], ly = o->stick_y[0];
        if (ly < -stick_threshold) k |= PS1_PAD_UP;
        if (ly > stick_threshold) k |= PS1_PAD_DOWN;
        if (lx < -stick_threshold) k |= PS1_PAD_LEFT;
        if (lx > stick_threshold) k |= PS1_PAD_RIGHT;
    }
    /* Opposite directions at once (a worn stick, two pads): a real D-pad cannot, some games misbehave. */
    if ((k & (PS1_PAD_UP | PS1_PAD_DOWN)) == (PS1_PAD_UP | PS1_PAD_DOWN))
        k &= (uint16_t)~(PS1_PAD_UP | PS1_PAD_DOWN);
    if ((k & (PS1_PAD_LEFT | PS1_PAD_RIGHT)) == (PS1_PAD_LEFT | PS1_PAD_RIGHT))
        k &= (uint16_t)~(PS1_PAD_LEFT | PS1_PAD_RIGHT);
    return k;
}

uint16_t ps1_runtime_pad_buttons(uint32_t b, int lx, int ly, int lt, int rt, int analog)
{
    ConsolePadMap m;
    ConsolePadOut o;
    HostPadState st = {.connected = 1, .buttons = b};
    st.axis[HOST_PAD_AXIS_LX] = (int16_t)lx;
    st.axis[HOST_PAD_AXIS_LY] = (int16_t)ly;
    st.axis[HOST_PAD_AXIS_LT] = (int16_t)lt;
    st.axis[HOST_PAD_AXIS_RT] = (int16_t)rt;
    console_pad_defaults(&m, CONSOLE_PAD_PS1);
    console_pad_apply(&m, &st, &o);
    return ps1_runtime_pad_of(&o, m.stick_threshold, analog);
}

uint8_t ps1_runtime_axis(int v)
{
    if (v < -32768)
        v = -32768;
    if (v > 32767)
        v = 32767;
    return (uint8_t)((v + 32768) >> 8);
}

void ps1_runtime_xrgb_to_rgba(const uint32_t *src, uint8_t *dst, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t c = src[i];
        dst[4 * i + 0] = (uint8_t)(c >> 16);
        dst[4 * i + 1] = (uint8_t)(c >> 8);
        dst[4 * i + 2] = (uint8_t)c;
        dst[4 * i + 3] = 255;
    }
}

int ps1_runtime_card_path(const char *save_dir, const char *key, int slot, char *out, size_t out_n)
{
    if (!save_dir || !*save_dir || !key || !*key || !out || slot < 0 || slot > 1)
        return -1;
    int n = snprintf(out, out_n, "%s/ps1-%s/card%d.mcr", save_dir, key, slot + 1);
    return n < 0 || (size_t)n >= out_n ? -1 : 0;
}

int ps1_runtime_is_exe(const uint8_t *head, size_t n)
{
    return head && n >= 8 && memcmp(head, "PS-X EXE", 8) == 0;
}

int ps1_runtime_fallback_key(const uint8_t *exe, size_t exe_n, const char *disc, char *out, size_t out_n)
{
    if (!out || out_n < 24)
        return -1;
    if (exe && exe_n) {
        snprintf(out, out_n, "EXE-%08X", (unsigned)crc32_of(exe, exe_n));
        return 0;
    }
    snprintf(out, out_n, "PSX-UNKNOWN");
    char err[128];
    ps1_disc *d = disc && *disc ? ps1_disc_open(disc, err, sizeof err) : NULL;
    if (d) {
        uint8_t sec[2352];
        if (ps1_disc_read_sector(d, 16, sec) == 0) {
            /* Mode 1: user data at 16; Mode 2 Form 1: after the 8-byte subheader, at 24. */
            const uint8_t *user = sec[15] == 1 ? sec + 16 : sec + 24;
            snprintf(out, out_n, "PSX-%08X", (unsigned)crc32_of(user, 2048));
        }
        ps1_disc_close(d);
    }
    return 0;
}

uint64_t ps1_runtime_frame_ns(uint64_t cycles)
{
    uint64_t ns = cycles * 1000000000ull / PS1_CPU_HZ;
    const uint64_t lo = 1000000000ull / 75, hi = 1000000000ull / 45;
    return ns < lo ? lo : ns > hi ? hi : ns;
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

/* A whole file, or "fd:N" read from offset 0 (the descriptor stays open and its offset is not used). */
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

/* The first bytes of a file or "fd:N" (from offset 0). Returns how many were read, or -1. */
static ssize_t read_head(const char *spec, uint8_t *out, size_t n)
{
    int own, fd = spec_fd(spec, &own);
    if (fd < 0)
        return -1;
    ssize_t r;
    do
        r = pread(fd, out, n, 0);
    while (r < 0 && errno == EINTR);
    if (own)
        close(fd);
    return r;
}

static void mkdirs(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    mkdir(tmp, 0755);
}

/* Writes path through path.tmp + rename, so a kill mid-write leaves the old card. */
static int write_file(const char *path, const uint8_t *data, size_t n)
{
    char tmp[1100], dir[1024];
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

/* ---- BIOS ----------------------------------------------------------------------------------------------------- */

static char s_start_bios[1024];

static uint8_t *bios_try(const char *path, size_t *n)
{
    uint8_t *b = path && *path ? read_all(path, PS1_BIOS_SIZE, n) : NULL;
    if (b && *n != PS1_BIOS_SIZE) {
        free(b);
        b = NULL;
    }
    return b;
}

/* The BIOS image (ps1_runtime_opts.bios); where it came from in `from`. NULL: the HLE BIOS ("hle" asked for, or no
 * image found: from says which), or *bad when the path the caller gave is not a 512 KB image. */
static uint8_t *bios_load(const char *explicit_path, char *from, size_t from_n, int *bad)
{
    size_t n = 0;
    uint8_t *b;
    const char *env = getenv("RECOMP_PS1_BIOS");
    const char *first[] = {explicit_path, env && *env ? env : NULL, s_start_bios[0] ? s_start_bios : NULL};
    *bad = 0;
    for (int i = 0; i < 3; i++) {
        if (!first[i] || !*first[i])
            continue;
        if (!strcmp(first[i], "hle")) {
            snprintf(from, from_n, "HLE (asked for)");
            return NULL;
        }
        if ((b = bios_try(first[i], &n))) {
            snprintf(from, from_n, "%s", first[i]);
            return b;
        }
        fprintf(stderr, "ps1: BIOS %s is not a 512 KB image\n", first[i]);
        if (i == 0) {
            *bad = 1; /* asked for that one */
            return NULL;
        }
    }
    /* The app's asset (SDL reads the APK's assets for a relative path on Android; on a desktop, relative to the
     * working directory). */
    SDL_RWops *rw = SDL_RWFromFile("ps1-bios/openbios.bin", "rb");
    if (rw) {
        Sint64 size = SDL_RWsize(rw);
        b = size == PS1_BIOS_SIZE ? malloc(PS1_BIOS_SIZE) : NULL;
        if (b && SDL_RWread(rw, b, 1, PS1_BIOS_SIZE) != PS1_BIOS_SIZE) {
            free(b);
            b = NULL;
        }
        SDL_RWclose(rw);
        if (b) {
            snprintf(from, from_n, "asset ps1-bios/openbios.bin");
            return b;
        }
    }
    /* A desktop build tree: build/ps1-bios/openbios.bin (tools/consoles/build_ps1_bios.sh) in the working directory,
     * or beside an ancestor of the executable (build/ps1/ps1_runtime/ps1_play -> build/ps1-bios). */
    char path[1200], dir[1024];
    if ((b = bios_try("build/ps1-bios/openbios.bin", &n))) {
        snprintf(from, from_n, "build/ps1-bios/openbios.bin");
        return b;
    }
    ssize_t len = readlink("/proc/self/exe", dir, sizeof dir - 1);
    if (len > 0) {
        dir[len] = 0;
        char *slash;
        while ((slash = strrchr(dir, '/')) && slash != dir) {
            *slash = 0;
            const char *rel[] = {"ps1-bios/openbios.bin", "build/ps1-bios/openbios.bin"};
            for (int i = 0; i < 2; i++) {
                snprintf(path, sizeof path, "%s/%s", dir, rel[i]);
                if ((b = bios_try(path, &n))) {
                    snprintf(from, from_n, "%s", path);
                    return b;
                }
            }
        }
    }
    snprintf(from, from_n, "HLE (no image found)");
    return NULL;
}

void ps1_runtime_set_bios(const char *path)
{
    snprintf(s_start_bios, sizeof s_start_bios, "%s", path ? path : "");
}

/* ---- Memory cards --------------------------------------------------------------------------------------------- */

/* The run's machine and its cards, shared with the exit hook (the in-game menu's Quit ends the process from the UI
 * thread) and the lifecycle listener: s_mu is held while a frame runs and while a card is written. */
static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static ps1_machine *s_m;
static char s_card_path[2][1100];
static uint8_t s_saved[2][PS1_CARD_SIZE]; /* each card as last read or written */
static char s_discovered[1200];           /* ps1_runtime_opts.discovered_path of the run */

/* Writes each card that differs from what is on disk (or was there at start). Called with s_mu held. */
static void cards_flush_locked(const char *why)
{
    for (int slot = 0; slot < 2; slot++) {
        size_t n = 0;
        const uint8_t *d;
        if (!s_m || !s_card_path[slot][0] || !(d = ps1_card_data(s_m, slot, &n)) || n != PS1_CARD_SIZE)
            continue;
        if (memcmp(d, s_saved[slot], n) == 0)
            continue;
        if (write_file(s_card_path[slot], d, n) != 0) {
            fprintf(stderr, "ps1: cannot write the memory card %s: %s\n", s_card_path[slot], strerror(errno));
            continue;
        }
        memcpy(s_saved[slot], d, n);
        printf("ps1: memory card %d written (%s) to %s\n", slot + 1, why, s_card_path[slot]);
    }
    fflush(stdout);
}

static void cards_flush(const char *why)
{
    pthread_mutex_lock(&s_mu);
    cards_flush_locked(why);
    pthread_mutex_unlock(&s_mu);
}

static void on_exit_hook(const char *why)
{
    (void)why;
    cards_flush("exit");
    /* The in-game menu's Quit ends the process here: the discovery loop's code too (XBA-168). */
    pthread_mutex_lock(&s_mu);
    if (s_m && s_discovered[0])
        ps1_pack_save_missed(s_m, s_discovered);
    pthread_mutex_unlock(&s_mu);
}

/* The app paused (background, the in-game menu): the cards go to disk at once, from the thread that paused it (on
 * Android SDL parks the thread that pumps events in the background, and the process can be killed there). Registered
 * once; no run, no-op. */
static void on_lifecycle(int paused, void *user)
{
    (void)user;
    if (paused)
        cards_flush("pause");
}

static void cards_load(ps1_machine *m)
{
    for (int slot = 0; slot < 2; slot++) {
        size_t n = 0;
        uint8_t *d = s_card_path[slot][0] ? read_all(s_card_path[slot], PS1_CARD_SIZE, &n) : NULL;
        if (d && n == PS1_CARD_SIZE && ps1_card_load(m, slot, d, n) == 0)
            printf("ps1: memory card %d loaded from %s\n", slot + 1, s_card_path[slot]);
        else if (d || (s_card_path[slot][0] && access(s_card_path[slot], F_OK) == 0))
            fprintf(stderr, "ps1: %s is not a 128 KB memory card image, a blank card is in slot %d (the file is "
                            "kept until the game writes the card)\n", s_card_path[slot], slot + 1);
        else if (s_card_path[slot][0] && slot == 0)
            printf("ps1: no memory card yet (%s), a blank one is in slot 1\n", s_card_path[slot]);
        free(d);
        const uint8_t *cur = ps1_card_data(m, slot, &n);
        if (cur && n == PS1_CARD_SIZE)
            memcpy(s_saved[slot], cur, n); /* what is on disk now (or the blank card): written once it changes */
        ps1_card_take_dirty(m, slot);
    }
}

/* ---- Save states (XBA-182) ------------------------------------------------------------------------------------ */

static char s_state_dir[1100];   /* <save dir>/ps1-<KEY> */
static char s_key[64];
static int s_state_loaded;       /* a state was loaded: the loop resets its clock (under s_mu) */

/* recomp_states_save's callback, any thread: the state and a thumbnail of the picture the GPU shows, taken under s_mu
 * (between frames), then written to <dir>/state<slot>.st. */
static int state_save_op(int slot, void *user)
{
    (void)user;
    char path[1200];
    if (recomp_states_slot_path(s_state_dir, slot, path, sizeof path) != 0)
        return RECOMP_STATE_ESLOT;
    static uint32_t xrgb[DISPLAY_MAX_W * DISPLAY_MAX_H];
    static uint8_t rgba[DISPLAY_MAX_W * DISPLAY_MAX_H * 4];
    static uint16_t thumb[RECOMP_STATE_THUMB_W * RECOMP_STATE_THUMB_H];
    recomp_state_meta meta;
    memset(&meta, 0, sizeof meta);
    snprintf(meta.console, sizeof meta.console, "ps1");
    snprintf(meta.key, sizeof meta.key, "%.47s", s_key);
    int w = 0, h = 0;
    pthread_mutex_lock(&s_mu);
    if (!s_m) {
        pthread_mutex_unlock(&s_mu);
        return RECOMP_STATE_ENOGAME;
    }
    size_t cap = ps1_state_size(s_m), n = 0;
    uint8_t *blob = malloc(cap);
    if (blob)
        n = ps1_state_save(s_m, blob, cap);
    meta.frames = ps1_frames(s_m);
    if (ps1_display(s_m, xrgb, DISPLAY_MAX_W, DISPLAY_MAX_H, &w, &h) != 0 || w <= 0 || h <= 0) {
        w = 320;
        h = 240;
        memset(xrgb, 0, sizeof(uint32_t) * 320 * 240);
    }
    pthread_mutex_unlock(&s_mu);
    ps1_runtime_xrgb_to_rgba(xrgb, rgba, (size_t)w * (size_t)h);
    recomp_states_thumb(rgba, w, h, w * 4, 4, 3, thumb, &meta.thumb_w, &meta.thumb_h);
    int rc = n ? recomp_states_write(path, &meta, thumb, blob, n) : RECOMP_STATE_EIO;
    free(blob);
    printf("ps1: state slot %d %s (%zu bytes, frame %llu) %s\n", slot, rc == 0 ? "saved" : "not saved", n,
           (unsigned long long)meta.frames, rc == 0 ? path : recomp_states_error(rc));
    fflush(stdout);
    return rc;
}

/* A state was loaded into m (a slot, a rewind step), s_mu held: the audio it had queued was heard already, and its
 * cards are what the game sees now: the files change only once the game writes them. */
static void state_loaded_locked(ps1_machine *m)
{
    int16_t sink[1024 * 2];
    while (ps1_audio_pull(m, sink, 1024) > 0)
        ;
    for (int c = 0; c < 2; c++) {
        size_t cn = 0;
        const uint8_t *cur = ps1_card_data(m, c, &cn);
        if (cur && cn == PS1_CARD_SIZE)
            memcpy(s_saved[c], cur, cn);
    }
}

/* Rewind (XBA-194): a capture is the state alone (the picture comes from VRAM, ps1_display). Under s_mu. */
static size_t rewind_size(void *user)
{
    return ps1_state_size((ps1_machine *)user);
}

static size_t rewind_save(uint8_t *out, size_t cap, void *user)
{
    return ps1_state_save((ps1_machine *)user, out, cap);
}

static int rewind_load(const uint8_t *in, size_t n, void *user)
{
    ps1_machine *m = user;
    if (ps1_state_load(m, in, n) != PS1_STATE_OK)
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
    if (rc == RECOMP_STATE_OK && strcmp(meta.console, "ps1") != 0)
        rc = RECOMP_STATE_EGAME;
    if (rc == RECOMP_STATE_OK) {
        pthread_mutex_lock(&s_mu);
        if (!s_m) {
            rc = RECOMP_STATE_ENOGAME;
        } else {
            rc = ps1_state_load(s_m, blob, n); /* PS1_STATE_* share RECOMP_STATE_*'s values */
            if (rc == PS1_STATE_OK) {
                state_loaded_locked(s_m);
                s_state_loaded = 1;
            }
        }
        pthread_mutex_unlock(&s_mu);
    }
    free(blob);
    if (rc == 0)
        printf("ps1: state slot %d loaded (%s)\n", slot, path);
    else
        printf("ps1: state slot %d not loaded: %s\n", slot, recomp_states_error(rc));
    fflush(stdout);
    return rc;
}

/* ---- Multi-disc games (XBA-166) ------------------------------------------------------------------------------- */

#define SWAP_LID_FRAMES 60 /* the lid stays open about a second: games poll Getstat, they see it */

static const char *base_name(const char *p);

int ps1_runtime_m3u(const char *path, char out[][PS1_RUNTIME_PATH_MAX], int max)
{
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f)
        return 0;
    char dir[PS1_RUNTIME_PATH_MAX] = "";
    const char *slash = strrchr(path, '/');
    if (slash)
        snprintf(dir, sizeof dir, "%.*s", (int)(slash - path + 1), path);
    char line[PS1_RUNTIME_PATH_MAX];
    int n = 0, first = 1;
    while (n < max && fgets(line, sizeof line, f)) {
        char *s = line;
        if (first && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
            s += 3;
        first = 0;
        size_t len = strlen(s);
        while (len && (s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == ' ' || s[len - 1] == '\t'))
            s[--len] = 0;
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s || *s == '#')
            continue;
        int w = s[0] == '/' ? snprintf(out[n], PS1_RUNTIME_PATH_MAX, "%s", s)
                            : snprintf(out[n], PS1_RUNTIME_PATH_MAX, "%s%s", dir, s);
        if (w > 0 && w < PS1_RUNTIME_PATH_MAX)
            n++;
    }
    fclose(f);
    return n;
}

static int is_m3u(const char *p)
{
    size_t n = p ? strlen(p) : 0;
    return n > 4 && strcasecmp(p + n - 4, ".m3u") == 0;
}

/* A swap asked from another thread (s_mu): the run's loop takes it between frames. */
static char s_swap_path[PS1_RUNTIME_PATH_MAX];
static int s_swap_asked;

int ps1_runtime_change_disc(const char *path)
{
    if (!path || !*path)
        return -1;
    pthread_mutex_lock(&s_mu);
    int running = s_m != NULL;
    if (running) {
        snprintf(s_swap_path, sizeof s_swap_path, "%s", path);
        s_swap_asked = 1;
    }
    pthread_mutex_unlock(&s_mu);
    return running ? 0 : -1;
}

/* The swap in progress: the lid is open for `frames` more frames, then `path` goes in. */
typedef struct disc_swap {
    char path[PS1_RUNTIME_PATH_MAX];
    int frames; /* 0: none in progress */
} disc_swap;

/* Called with s_mu held, between frames: starts an asked swap (the image is checked first: one that does not open
 * leaves the disc and the lid alone), finishes one whose lid time is over. */
static void disc_swap_step(ps1_machine *m, disc_swap *sw, uint64_t frame)
{
    if (s_swap_asked && !sw->frames) {
        s_swap_asked = 0;
        char err[256] = "";
        ps1_disc *probe = ps1_disc_open(s_swap_path, err, sizeof err);
        if (!probe) {
            fprintf(stderr, "ps1: cannot change the disc to %s: %s\n", s_swap_path, err);
            return;
        }
        ps1_disc_close(probe);
        snprintf(sw->path, sizeof sw->path, "%s", s_swap_path);
        sw->frames = SWAP_LID_FRAMES;
        ps1_set_lid(m, 1);
        printf("ps1: lid opened at frame %llu (changing the disc to %s)\n", (unsigned long long)frame,
               base_name(sw->path));
        fflush(stdout);
        return;
    }
    if (sw->frames && --sw->frames == 0) {
        char err[256] = "";
        if (ps1_change_disc(m, sw->path, err, sizeof err) != 0)
            fprintf(stderr, "ps1: the disc %s cannot be read now (%s): the previous one stays\n", sw->path, err);
        ps1_set_lid(m, 0);
        printf("ps1: lid closed at frame %llu, disc %s in\n", (unsigned long long)frame, base_name(sw->path));
        fflush(stdout);
    }
}

/* ---- Cheats (XBA-197): the bus, RAM words through the core's side-effect-free accessors ----------------------- */

static uint32_t cheat_peek32(void *m, uint32_t addr)
{
    return ps1_peek32(m, addr);
}

static void cheat_poke32(void *m, uint32_t addr, uint32_t v)
{
    ps1_poke32(m, addr, v);
}

/* ---- Freeze diagnostic: a game stuck in a loop with interrupts off --------------------------------------------- */

/* Nothing but that loop can get it out (no IRQ handler runs), so it is a hang in the core (a device that never gives
 * the loop what it waits for), not a slow scene: said once in the log, with the interrupt controller's state, and
 * again when the game leaves. Checked at each frame's end; host-side only (nothing in the machine changes). */
#define STALL_FRAMES 300 /* 5 s at 60 Hz */

typedef struct stall_watch {
    uint32_t window; /* the 64-byte code window the PC stays in */
    int frames;      /* consecutive frame ends in it with SR.IEc = 0 (0: not watching) */
    int told;
} stall_watch;

static void stall_check(ps1_machine *m, stall_watch *w)
{
    uint32_t pc = ps1_pc(m), off = !(ps1_cop0(m, 12) & 1); /* SR bit 0: IEc */
    if (off && w->frames && (pc & ~63u) == w->window) {
        if (w->frames < STALL_FRAMES && ++w->frames == STALL_FRAMES) {
            uint32_t st, mask;
            ps1_irq_regs(m, &st, &mask);
            printf("ps1: the game has not left pc 0x%08x for 5 s with interrupts off (I_STAT %x, I_MASK %x)\n", pc,
                   (unsigned)st, (unsigned)mask);
            fflush(stdout);
            w->told = 1;
        }
        return;
    }
    if (w->told) {
        printf("ps1: running again\n");
        fflush(stdout);
        w->told = 0;
    }
    w->window = pc & ~63u;
    w->frames = off ? 1 : 0;
}

/* ---- The run -------------------------------------------------------------------------------------------------- */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
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

/* Desktop keys: arrows, X = Cross, C = Circle, Z = Square, S = Triangle, Q / E = L1 / R1, 1 / 3 = L2 / R2,
 * Enter = Start, Backspace or right Shift = Select. */
static uint16_t keyboard_buttons(void)
{
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    uint16_t k = 0;
    if (!ks)
        return 0;
    if (ks[SDL_SCANCODE_X]) k |= PS1_PAD_CROSS;
    if (ks[SDL_SCANCODE_C]) k |= PS1_PAD_CIRCLE;
    if (ks[SDL_SCANCODE_Z]) k |= PS1_PAD_SQUARE;
    if (ks[SDL_SCANCODE_S]) k |= PS1_PAD_TRIANGLE;
    if (ks[SDL_SCANCODE_Q]) k |= PS1_PAD_L1;
    if (ks[SDL_SCANCODE_E]) k |= PS1_PAD_R1;
    if (ks[SDL_SCANCODE_1]) k |= PS1_PAD_L2;
    if (ks[SDL_SCANCODE_3]) k |= PS1_PAD_R2;
    if (ks[SDL_SCANCODE_RETURN]) k |= PS1_PAD_START;
    if (ks[SDL_SCANCODE_BACKSPACE] || ks[SDL_SCANCODE_RSHIFT]) k |= PS1_PAD_SELECT;
    if (ks[SDL_SCANCODE_UP]) k |= PS1_PAD_UP;
    if (ks[SDL_SCANCODE_DOWN]) k |= PS1_PAD_DOWN;
    if (ks[SDL_SCANCODE_LEFT]) k |= PS1_PAD_LEFT;
    if (ks[SDL_SCANCODE_RIGHT]) k |= PS1_PAD_RIGHT;
    return k;
}

typedef struct pad_in {
    uint16_t buttons;
    uint8_t lx, ly, rx, ry;
    int toggle; /* Back + L3 + R3 held */
    uint32_t hk_held, hk_pressed; /* the hotkeys a pad holds / pressed this frame (recomp_hotkeys.h, XBA-190) */
} pad_in;

static int deflection(uint8_t v)
{
    return v > 0x80 ? v - 0x80 : 0x80 - v;
}

/* The host pads for PS1 port `port`, through the player's mapping (the on-screen pad joins host pad 1,
 * console_pad_read): with one player every pad drives port 0 (buttons merged, the most deflected stick wins); with
 * two, host pad N drives port N. */
static recomp_hotkeys_state s_hk_pad[HOST_PAD_PORTS]; /* recomp_hotkeys_pad_update's state per host pad */
static void pads_read(int port, int players, int analog, pad_in *out)
{
    memset(out, 0, sizeof *out);
    out->lx = out->ly = out->rx = out->ry = 0x80;
    if (!s_pad_map.console)
        console_pad_defaults(&s_pad_map, CONSOLE_PAD_PS1);
    for (int p = 0; p < HOST_PAD_PORTS; p++) {
        if (players >= 2 ? p != port : port != 0)
            continue;
        ConsolePadOut o;
        if (!console_pad_read(&s_pad_map, p, &o))
            continue;
        HostPadState st;
        uint32_t src = host_pad_read(p, &st) ? console_pad_sources(&st, s_pad_map.stick_threshold, RECOMP_HK_TRIGGER) : 0;
        uint32_t pressed = 0, swallow = 0;
        out->hk_held |= recomp_hotkeys_pad_update(recomp_hotkeys_get(), &s_hk_pad[p], src, &pressed, &swallow);
        out->hk_pressed |= pressed;
        console_pad_swallow(&s_pad_map, &o, swallow); /* a held chord's buttons: not the on-screen pad's */
        const uint32_t chord = HOST_PAD_BACK | HOST_PAD_LSTICK | HOST_PAD_RSTICK;
        if ((o.host_buttons & chord) == chord)
            out->toggle = 1;
        out->buttons |= ps1_runtime_pad_of(&o, s_pad_map.stick_threshold, analog);
        uint8_t lx = ps1_runtime_axis(o.stick_x[0]), ly = ps1_runtime_axis(o.stick_y[0]);
        uint8_t rx = ps1_runtime_axis(o.stick_x[1]), ry = ps1_runtime_axis(o.stick_y[1]);
        if (deflection(lx) + deflection(ly) > deflection(out->lx) + deflection(out->ly)) {
            out->lx = lx;
            out->ly = ly;
        }
        if (deflection(rx) + deflection(ry) > deflection(out->rx) + deflection(out->ry)) {
            out->rx = rx;
            out->ry = ry;
        }
    }
}

void ps1_runtime_opts_init(ps1_runtime_opts *o)
{
    memset(o, 0, sizeof *o);
    o->scale = -1;
    o->filter = -1;
    o->analog = -1;
    o->players = -1;
    o->fast_boot = -1;
    o->upscale = -1;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* The DualShock motors to the host pads (host_pad_rumble: low frequency = the large motor, high = the small one): with
 * one player every pad (they all drive port 1), with two host pad N for port N. Refreshed every frame while on (so
 * a lost update cannot leave a pad buzzing for long), stopped once when off. */
static void rumble_drive(int players, uint8_t motors[2][2], uint8_t sent[2][2])
{
    for (int port = 0; port < (players >= 2 ? 2 : 1); port++) {
        uint8_t small = motors[port][0], large = motors[port][1];
        int on = small || large;
        if (!on && !sent[port][0] && !sent[port][1])
            continue;
        for (int p = 0; p < HOST_PAD_PORTS; p++)
            if (players >= 2 ? p == port : 1)
                host_pad_rumble(p, (uint16_t)(large * 257u), (uint16_t)(small * 257u), on ? 100 : 0);
        sent[port][0] = small;
        sent[port][1] = large;
    }
}

static const char *pad_name(int analog)
{
    return analog ? "analog (DualShock)" : "digital";
}

void ps1_runtime_display(const ps1_runtime_opts *o, int *scale, int *filter)
{
    /* RECOMP_PS1_SCALE / _FILTER, else the shared RECOMP_SCALE / RECOMP_FILTER (XBA-181); smooth by default. */
    int s = o && o->scale >= 0 ? o->scale : host_window_env_scale("RECOMP_PS1_SCALE", HOST_SCALE_ASPECT);
    int f = o && o->filter >= 0 ? o->filter : host_window_env_filter("RECOMP_PS1_FILTER", HOST_FILTER_LINEAR);
    if (scale)
        *scale = s;
    if (filter)
        *filter = f;
}

static int upscale_parse(const char *v)
{
    if (!v || *v < '1' || *v > '9')
        return 0;
    int n = atoi(v);
    return n >= 1 && n <= 8 ? n : 0;
}

int ps1_runtime_upscale(const ps1_runtime_opts *o)
{
    int n = o && o->upscale >= 1 ? o->upscale : 0;
    if (!n)
        n = upscale_parse(getenv("RECOMP_PS1_UPSCALE"));
    if (!n)
        n = upscale_parse(getenv("RECOMP_UPSCALE"));
    return n >= 4 ? 4 : n >= 2 ? 2 : 1;
}

int ps1_runtime_run(const ps1_runtime_opts *o)
{
    if (!o || !o->game || !*o->game) {
        fprintf(stderr, "ps1: no game\n");
        return -1;
    }
    /* A .m3u playlist (XBA-166): its first disc plays, the others are a disc swap away. */
    static char discs[PS1_RUNTIME_DISCS_MAX][PS1_RUNTIME_PATH_MAX];
    int ndiscs = 0, disc_at = 0;
    const char *game = o->game;
    if (is_m3u(game)) {
        ndiscs = ps1_runtime_m3u(game, discs, PS1_RUNTIME_DISCS_MAX);
        if (!ndiscs) {
            fprintf(stderr, "ps1: the playlist %s names no disc (or cannot be read)\n", game);
            return -1;
        }
        printf("ps1: playlist %s: %d disc%s, disc 1 is %s\n", base_name(game), ndiscs, ndiscs > 1 ? "s" : "",
               discs[0]);
        game = discs[0];
    }
    /* An EXE (fast boot), or a disc the BIOS boots. */
    uint8_t head[8] = {0};
    ssize_t hn = read_head(game, head, sizeof head);
    if (hn <= 0) {
        fprintf(stderr, "ps1: cannot read the game %s (missing, empty or no permission)\n", game);
        return -1;
    }
    uint8_t *exe = NULL;
    size_t exe_n = 0;
    const char *disc = NULL, *exe_path = NULL;
    if (ps1_runtime_is_exe(head, (size_t)hn)) {
        exe_path = game;
    } else {
        disc = game;
        exe_path = o->exe && *o->exe ? o->exe : NULL;
    }
    if (exe_path && !(exe = read_all(exe_path, PS1_EXE_MAX, &exe_n))) {
        fprintf(stderr, "ps1: cannot read the EXE %s (missing, empty or over 2 MB)\n", exe_path);
        return -1;
    }
    char bios_from[1200] = "";
    int bios_bad = 0;
    uint8_t *bios = bios_load(o->bios, bios_from, sizeof bios_from, &bios_bad);
    if (!bios && bios_bad) {
        fprintf(stderr, "ps1: the BIOS %s cannot be used (a 512 KB image, or \"hle\")\n", o->bios);
        free(exe);
        return -1;
    }
    char key[64];
    if (o->title_key && *o->title_key)
        snprintf(key, sizeof key, "%s", o->title_key);
    else
        ps1_runtime_fallback_key(exe_path == game ? exe : NULL, exe_path == game ? exe_n : 0, disc, key,
                                 sizeof key);
    /* A pack's source identity (ps1_pack.h): the SHA-256 of the boot EXE, the file itself for an EXE game. */
    char src_sha[65] = "", src_why[300] = "";
    if (o->pack_lib && *o->pack_lib) {
        if (exe_path == game) {
            uint8_t sum[32];
            ps1_pack_sha256(exe, exe_n, sum);
            ps1_pack_hex(sum, src_sha);
        } else if (ps1_pack_disc_boot_sha256(disc, src_sha, NULL, 0, src_why, sizeof src_why) != 0) {
            fprintf(stderr, "ps1: %s\n", src_why);
        }
    }
    char err[256] = "";
    /* The GPU's shadow-pass workers, as they start: their fast cores put back after a resume, the performance hint. */
    ps1_gpu_set_thread_hook(recomp_host_worker_hook, NULL);
    ps1_machine *m = ps1_create(bios, bios ? PS1_BIOS_SIZE : 0, disc, exe, exe_n, err, sizeof err);
    free(bios);
    free(exe);
    if (!m) {
        fprintf(stderr, "ps1: the machine cannot start: %s\n", err[0] ? err : "?");
        return -1;
    }
    int fast_boot = o->fast_boot >= 0 ? o->fast_boot != 0 : !env_choice("RECOMP_PS1_FASTBOOT", "0", 0);
    ps1_set_fast_boot(m, fast_boot);
    /* Internal resolution (XBA-184): the frame buffers hold the biggest picture at that factor. */
    int up_asked = ps1_runtime_upscale(o), up = 1, gpu_threads = 0;
    if (ps1_get_gpu(m) && up_asked > 1) {
        const char *th = getenv("RECOMP_PS1_GPU_THREADS"); /* the shadow pass's workers (tuning; default by cores) */
        up = ps1_gpu_set_upscale(ps1_get_gpu(m), up_asked);
        gpu_threads = ps1_gpu_set_threads(ps1_get_gpu(m), th && *th >= '0' && *th <= '9' ? atoi(th) : -1);
    }
    int max_w = DISPLAY_MAX_W * up, max_h = DISPLAY_MAX_H * up;
    uint32_t *xrgb = malloc((size_t)max_w * (size_t)max_h * sizeof *xrgb);
    uint8_t *rgba = malloc((size_t)max_w * (size_t)max_h * 4);
    if (!xrgb || !rgba) {
        fprintf(stderr, "ps1: no memory for the picture (%d x %d)\n", max_w, max_h);
        free(xrgb);
        free(rgba);
        ps1_destroy(m);
        return -1;
    }
    printf("ps1: %s, game %s (%s), key %s, BIOS %s\n", ps1_runtime_version(), base_name(game),
           exe_path == game ? "EXE, fast boot"
           : exe_path       ? "disc + EXE fast boot"
           : fast_boot      ? "disc, BIOS boot without its shell"
                            : "disc, BIOS boot",
           key, bios_from);
    if (up_asked > 1)
        printf("ps1: internal resolution %dx%s, %d GPU worker thread%s\n", up,
               up == up_asked ? "" : " (no memory for more)", gpu_threads, gpu_threads == 1 ? "" : "s");
    ps1_pack pack;
    ps1_pack_load(m, o->pack_lib, key, src_sha, &pack); /* the interpreter whatever happens (logged) */

    int analog = o->analog >= 0 ? o->analog != 0 : env_choice("RECOMP_PS1_PAD", "analog", 0);
    int players = o->players >= 1 ? o->players : 0;
    if (!players) {
        const char *v = getenv("RECOMP_PS1_PLAYERS");
        players = v && atoi(v) >= 2 ? 2 : 1;
    }
    players = players >= 2 ? 2 : 1;

    pthread_mutex_lock(&s_mu);
    s_m = m;
    for (int slot = 0; slot < 2; slot++)
        if (ps1_runtime_card_path(o->save_dir, key, slot, s_card_path[slot], sizeof s_card_path[slot]) != 0)
            s_card_path[slot][0] = 0;
    cards_load(m);
    snprintf(s_discovered, sizeof s_discovered, "%s", o->discovered_path ? o->discovered_path : "");
    pthread_mutex_unlock(&s_mu);
    recomp_exit_hook_set(on_exit_hook);
    static int listening;
    if (!listening && recomp_lifecycle_listen(on_lifecycle, NULL) == 0)
        listening = 1;
    /* Save states: <save dir>/ps1-<KEY>/state<N>.st, beside the memory cards (none without a save dir). */
    snprintf(s_key, sizeof s_key, "%s", key);
    if (!o->save_dir || !*o->save_dir ||
        snprintf(s_state_dir, sizeof s_state_dir, "%s/ps1-%s", o->save_dir, key) >= (int)sizeof s_state_dir)
        s_state_dir[0] = 0;
    recomp_state_ops ops = {"ps1", state_save_op, state_load_op, NULL};
    recomp_states_attach(&ops, s_state_dir);
    printf("ps1: save states %s%s, fast-forward x%d%s\n", s_state_dir[0] ? "in " : "off (no save dir)", s_state_dir,
           recomp_ff_speed(), recomp_ff_speed() ? "" : " (max)");
    memset(s_hk_pad, 0, sizeof s_hk_pad);
    recomp_hotkeys_get(); /* RECOMP_HOTKEYS / _KEYS, logged once (XBA-190) */
    recomp_rewinder rw;
    recomp_rewinder_init(&rw, "ps1", NULL); /* RECOMP_REWIND, _MB, _FRAMES (XBA-194) */
    recomp_cheats cheats;
    recomp_cheats_init(&cheats, RECOMP_CHEAT_PS1);
    recomp_cheats_load_env(&cheats, "ps1"); /* RECOMP_CHEATS_FILE (XBA-197) */
    recomp_cheat_bus cheat_bus = {m, NULL, NULL, cheat_peek32, cheat_poke32, 0, NULL, 0};
    unsigned cheat_polls = 0;
    int resume = recomp_states_resume_enabled() && s_state_dir[0];
    if (resume) {
        int r = recomp_states_resume();
        printf("ps1: resume from the automatic slot: %s\n", r == RECOMP_STATE_EEMPTY ? "nothing saved yet"
                                                                                     : recomp_states_error(r));
    }

    int window = !o->headless && !host_window_headless();
    if (SDL_WasInit(SDL_INIT_EVENTS) == 0)
        SDL_InitSubSystem(SDL_INIT_EVENTS);
    int scale, filter;
    ps1_runtime_display(o, &scale, &filter);
    host_window_set_scaling(scale, filter);
    host_window_shader_env("RECOMP_PS1_SHADER"); /* XBA-195: picture shader */
    host_window_set_aspect(4, 3);
    if (window) {
        char title[300];
        snprintf(title, sizeof title, "XRecomp PS1 - %s", base_name(game));
        if (!host_window_open(title, 320 * 3, 240 * 3, host_window_env_vsync())) {
            fprintf(stderr, "ps1: no window, running headless\n");
            window = 0;
        }
    }
    if (!window)
        host_pad_init();
    console_pad_load(&s_pad_map, CONSOLE_PAD_PS1, getenv(console_pad_env(CONSOLE_PAD_PS1)));
    int audio = host_audio_open(PS1_AUDIO_RATE, 2);
    printf("ps1: window %s (4:3, %s, %s), audio %s, pads %u, port 1 %s%s\n", window ? "on" : "off",
           scale == HOST_SCALE_INTEGER ? "integer" : scale == HOST_SCALE_STRETCH ? "stretch" : "aspect",
           filter == HOST_FILTER_NEAREST ? "nearest" : "linear",
           audio ? "on" : "off", host_pad_mask(), pad_name(analog), players >= 2 ? ", two players" : "");
    fflush(stdout);

    static int16_t pcm[PS1_AUDIO_RATE / 10 * 2];
    int dw = 320, dh = 240, last_w = 0, last_h = 0, last_pal = -1, last_int = -1, display_on = -1;
    uint64_t frames = 0, next = now_ns(), t0 = next, late_resets = 0, dirty_at = 0, dirty_first = 0;
    int was_paused = 0, heard = 0, rc = 0, toggle_held = 0, f6_held = 0, was_ff = 0;
    recomp_hotkeys_state hk_keys = {0};
    recomp_ff_carry carry = {{0, 0}, 0};
    disc_swap swap = {"", 0};
    stall_watch stall = {0, 0, 0};
    ps1_gpu *gpu = ps1_get_gpu(m);
    /* DualShocks (XBA-167): plugged in in the mode chosen (RECOMP_PS1_PAD); the game may switch them (config mode)
     * and the toggle chord is their ANALOG button. The left stick is a D-pad while a port reports digital. */
    ps1_pad_kind kind = analog ? PS1_PAD_ANALOG : PS1_PAD_DUALSHOCK;
    int analog2 = analog;
    uint8_t rumble[2][2] = {{0, 0}, {0, 0}};   /* per port: small, large, as last sent to the host pads */
    int rumble_on = host_pad_rumble_env();     /* RECOMP_RUMBLE=0 turns the DualShock motors off (XBA-181) */
    recomp_perf_hint_begin(16683333);          /* the frame period; each frame's own below */

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
            printf("ps1: quit asked after %llu frames\n", (unsigned long long)frames);
            if (resume && frames > 0) /* the player closed the window: the next start resumes here */
                printf("ps1: automatic slot %s\n", recomp_states_error(recomp_states_save(RECOMP_STATE_AUTO)));
            break;
        }
        if (recomp_lifecycle_paused()) {
            if (!was_paused) {
                printf("ps1: paused at frame %llu\n", (unsigned long long)frames);
                fflush(stdout);
                cards_flush("pause");
                dirty_at = dirty_first = 0;
                was_paused = 1;
            }
            SDL_Delay(16);
            continue;
        }
        if (was_paused) {
            printf("ps1: resumed\n");
            fflush(stdout);
            was_paused = 0;
            next = now_ns(); /* the time away is not caught up */
        }
        uint64_t work_t0 = now_ns(); /* the frame's work, for the performance hint (the sleep after it is not) */

        pad_in p1, p2;
        pads_read(0, players, analog, &p1);
        pads_read(1, players, analog2, &p2);
        if (p1.toggle && !toggle_held) {
            pthread_mutex_lock(&s_mu);
            int was = analog;
            analog = ps1_pad_analog_button(m, 0) == 1;
            pthread_mutex_unlock(&s_mu);
            if (analog == was)
                printf("ps1: port 1 stays %s (locked by the game)\n", pad_name(analog));
            else
                printf("ps1: port 1 is now %s\n", pad_name(analog));
            fflush(stdout);
        }
        toggle_held = p1.toggle;
        if (p1.toggle)
            p1.buttons &= (uint16_t)~(PS1_PAD_SELECT | PS1_PAD_L3 | PS1_PAD_R3);
        p1.buttons |= o->buttons;
        if (window)
            p1.buttons |= keyboard_buttons();
        if (o->swap_frame && frames + 1 == o->swap_frame && o->swap_disc)
            ps1_runtime_change_disc(o->swap_disc);
        if (window && ndiscs > 1) { /* F6: the playlist's next disc */
            const Uint8 *ks = SDL_GetKeyboardState(NULL);
            int f6 = ks && ks[SDL_SCANCODE_F6];
            if (f6 && !f6_held) {
                disc_at = (disc_at + 1) % ndiscs;
                ps1_runtime_change_disc(discs[disc_at]);
            }
            f6_held = f6;
        }
        uint32_t hk_held = p1.hk_held | p2.hk_held, hk_pressed = p1.hk_pressed | p2.hk_pressed;
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
            printf("ps1: fast-forward %s at frame %llu\n", ff ? (speed ? "on" : "on (max)") : "off",
                   (unsigned long long)frames);
            fflush(stdout);
            carry = (recomp_ff_carry){{0, 0}, 0};
            was_ff = ff;
            next = now_ns();
        }
        /* Fast-forward: `speed` PS1 frames per shown frame (max: as many as fit in one display frame). */
        int steps = rewinding ? 0 : ff ? (speed ? speed : 64) : 1, dirty = 0, on = 0;
        uint64_t frame_ns = 16683333, burst_end = now_ns() + 15000000;
        if (rewinding) { /* rewind held: one capture back per shown frame, no PS1 frame runs, nothing heard */
            pthread_mutex_lock(&s_mu);
            if (recomp_rewinder_step(&rw, rewind_load, m) == 0)
                recomp_states_note_load(); /* a fresh frame generation history */
            on = ps1_display(m, xrgb, max_w, max_h, &dw, &dh) == 0;
            pthread_mutex_unlock(&s_mu);
        }
        if (++cheat_polls % 30 == 0)
            recomp_cheats_poll(&cheats); /* the app changed the list: read it again */
        for (int step = 0; step < steps; step++) {
            pthread_mutex_lock(&s_mu);
            disc_swap_step(m, &swap, frames + 1); /* the frame about to run */
            ps1_set_pad(m, 0, kind, p1.buttons, p1.lx, p1.ly, p1.rx, p1.ry);
            if (players >= 2)
                ps1_set_pad(m, 1, kind, p2.buttons, p2.lx, p2.ly, p2.rx, p2.ry);
            uint64_t c0 = ps1_cycles(m);
            ps1_run_frame(m);
            frame_ns = ps1_runtime_frame_ns(ps1_cycles(m) - c0);
            stall_check(m, &stall);
            if (cheats.n)
                recomp_cheats_apply(&cheats, &cheat_bus);
            recomp_rewinder_frame(&rw, rewind_size, rewind_save, m);
            if (s_state_loaded) {
                s_state_loaded = 0;
                next = now_ns(); /* a loaded state: the clock starts again from it */
            }
            int was_analog = analog;
            analog = ps1_pad_analog(m, 0) == 1;
            analog2 = players >= 2 ? ps1_pad_analog(m, 1) == 1 : analog;
            uint8_t motors[2][2];
            for (int port = 0; port < 2; port++)
                ps1_pad_rumble(m, port, &motors[port][0], &motors[port][1]);
            dirty |= ps1_card_take_dirty(m, 0) | ps1_card_take_dirty(m, 1);
            int last = step == steps - 1 || !ff || (o->frames && frames + 1 >= o->frames) ||
                       (!speed && now_ns() >= burst_end);
            if (last) /* the picture of the frame shown only */
                on = ps1_display(m, xrgb, max_w, max_h, &dw, &dh) == 0;
            pthread_mutex_unlock(&s_mu);
            frames++;
            if (analog != was_analog) {
                printf("ps1: the game set port 1 %s at frame %llu\n", pad_name(analog), (unsigned long long)frames);
                fflush(stdout);
            }
            if (rumble_on)
                rumble_drive(players, motors, rumble); /* RECOMP_RUMBLE=0: never */

            /* Sound: everything the frame made (about 735 sample frames); averaged down while fast-forwarding. */
            size_t got;
            while ((got = ps1_audio_pull(m, pcm, sizeof pcm / sizeof pcm[0] / 2)) > 0) {
                heard = 1;
                if (ff)
                    got = speed ? recomp_ff_audio(pcm, got, speed, &carry) : 0;
                if (audio && got)
                    host_audio_queue(pcm, (uint32_t)got);
            }
            if (last)
                break;
        }

        if (gpu) {
            ps1_gpu_timing tm;
            ps1_gpu_get_timing(gpu, &tm);
            if (tm.pal != last_pal || tm.interlaced != last_int) {
                printf("ps1: video %s%s at frame %llu\n", tm.pal ? "PAL (50 Hz)" : "NTSC (59.94 Hz)",
                       tm.interlaced ? ", interlaced" : "", (unsigned long long)frames);
                last_pal = tm.pal;
                last_int = tm.interlaced;
            }
        }
        if (!on || dw <= 0 || dh <= 0 || dw > max_w || dh > max_h) {
            dw = last_w ? last_w : 320;
            dh = last_h ? last_h : 240;
            memset(xrgb, 0, (size_t)dw * (size_t)dh * sizeof xrgb[0]);
        } else if (dw != last_w || dh != last_h) {
            printf("ps1: display %d x %d at frame %llu\n", dw, dh, (unsigned long long)frames);
            last_w = dw;
            last_h = dh;
        }
        if (on != display_on) {
            if (display_on >= 0 || !on)
                printf("ps1: display %s at frame %llu\n", on ? "on" : "off", (unsigned long long)frames);
            display_on = on;
        }

        uint64_t t = now_ns();
        if (dirty) {
            dirty_at = t;
            if (!dirty_first)
                dirty_first = t;
        }
        if (dirty_at && (t - dirty_at >= SAVE_DEBOUNCE_NS || t - dirty_first >= SAVE_LATEST_NS)) {
            cards_flush("written by the game");
            dirty_at = dirty_first = 0;
        }

        if (window) {
            if (o->capture_path && o->capture_frame == frames)
                host_window_capture(o->capture_path);
            ps1_runtime_xrgb_to_rgba(xrgb, rgba, (size_t)dw * (size_t)dh);
            host_window_present_rgba(rgba, dw, dh, dw * 4);
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
            if (t > next + 3 * frame_ns) {
                next = t; /* a stall (a slow host, the debugger): no burst to catch up */
                late_resets++;
            } else if (next > t) {
                sleep_until(next);
            }
        }
    }

    recomp_perf_hint_end();
    double secs = (double)(now_ns() - t0) / 1e9;
    printf("ps1: %llu frames in %.1f s (%.2f fps), %llu late resets\n", (unsigned long long)frames, secs,
           secs > 0 ? (double)frames / secs : 0.0, (unsigned long long)late_resets);
    cards_flush("exit");
    if (o->screenshot) {
        if (write_ppm(o->screenshot, xrgb, dw, dh) == 0)
            printf("ps1: last frame (%d x %d) written to %s\n", dw, dh, o->screenshot);
        else
            rc = -1;
    }
    fflush(stdout);
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
    s_swap_asked = 0;
    s_card_path[0][0] = s_card_path[1][0] = 0;
    if (s_discovered[0])
        ps1_pack_save_missed(m, s_discovered);
    s_discovered[0] = 0;
    pthread_mutex_unlock(&s_mu);
    ps1_destroy(m);
    ps1_pack_close(&pack);
    free(xrgb);
    free(rgba);
    return rc;
}

/* ---- The app shell's entry ------------------------------------------------------------------------------------ */

static char s_start_save_dir[1024];
static char s_start_work_dir[1024];

void ps1_runtime_set_work_dir(const char *dir)
{
    snprintf(s_start_work_dir, sizeof s_start_work_dir, "%s", dir ? dir : "");
}

void ps1_runtime_set_save_dir(const char *dir)
{
    snprintf(s_start_save_dir, sizeof s_start_save_dir, "%s", dir ? dir : "");
}

int ps1_runtime_start(const char *rom, const char *pack_lib, const char *title_key)
{
    ps1_runtime_opts o;
    ps1_runtime_opts_init(&o);
    o.game = rom;
    o.title_key = title_key;
    o.save_dir = s_start_save_dir[0] ? s_start_save_dir : NULL;
    o.pack_lib = pack_lib;
    char discovered[1200];
    if (s_start_work_dir[0] && title_key && *title_key) {
        snprintf(discovered, sizeof discovered, "%s/ps1-%s/discovered.txt", s_start_work_dir, title_key);
        o.discovered_path = discovered;
    }
    const char *frames = getenv("RECOMP_PS1_FRAMES"); /* a bounded run (checks) */
    if (frames && *frames)
        o.frames = strtoull(frames, NULL, 10);
    printf("ps1 runtime ready (%s; title %s, pack %s, game %s)\n", ps1_runtime_version(), title_key ? title_key : "?",
           pack_lib && *pack_lib ? pack_lib : "none (interpreter)", rom ? rom : "?");
    fflush(stdout);
    return ps1_runtime_run(&o);
}
