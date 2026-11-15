/* gba_runtime.c -- see gba_runtime.h (XBA-146, XBA-157).
 *
 * One thread does everything, frame by frame: pump the window's events (quit, background, pads), read the pads,
 * run one GBA frame, queue its audio, present it, then sleep to the next frame's deadline. Pacing is a clock at the
 * GBA's own 59.7275 Hz with absolute deadlines (no drift), nudged by the audio queue once the core produces sound
 * (so the audio device's clock wins in the long run and the queue neither starves nor grows); vsync is on, so a swap
 * may wait for the display too, and a frame late by more than three periods resets the clock instead of running
 * fast to catch up. While the app is paused (background, the in-game menu: recomp_lifecycle) no frame runs, the save
 * is written and the window keeps pumping.
 *
 * Save states and fast-forward (XBA-182, recomp_states.h): the run attaches itself while it plays, so the in-game
 * menu and the hotkeys (XBA-190, recomp_hotkeys.h: pad chords, desktop keys F1-F4 / Shift+F1-F4 / Tab by default) save
 * and load between frames under s_mu. Fast-forward runs `speed` GBA frames per shown frame with their audio averaged down (or, at
 * "max", as many as fit in a display frame, silent).
 *
 * Rewind (XBA-194, recomp_rewind.h): a capture (the state and the last frame's picture) every RECOMP_REWIND_FRAMES
 * frames (2 by default) into a ring of RECOMP_REWIND_MB; while the rewind hotkey is held (Select + LT, R) each shown
 * frame loads the newest capture and drops it, silent; letting go plays on from there.
 *
 * Cheats (XBA-197, gba_cheats.h): RECOMP_CHEATS_FILE's enabled codes run after every GBA frame under s_mu; the file is
 * read again when the app changes it (checked every 30 shown frames). */
#define _GNU_SOURCE /* pread, nanosleep */
#include "gba_runtime.h"
#include "gba_cheats.h"

#include "gba_core.h"
#include "gba_pack.h"
#include "host_audio.h"
#include "console_pad.h"
#include "host_pad.h"
#include "host_perf_hint.h"
#include "host_window.h"
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

#define GBA_ROM_MAX (32u * 1024 * 1024)
#define BIOS_SIZE 16384
#define SAVE_DEBOUNCE_NS 1000000000ull
#define AUDIO_TARGET_FRAMES (GBA_AUDIO_RATE / 20) /* 50 ms queued */

const char *gba_runtime_version(void)
{
    return "gba_runtime " GBA_RUNTIME_VERSION;
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

static char code_char(uint8_t c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? (char)c : '_';
}

int gba_runtime_key(const uint8_t *rom, size_t size, char *out, size_t out_n)
{
    char code[5] = "____";
    if (!out || out_n < 24)
        return -1;
    if (rom && size >= 0xC0 && rom[0xB2] == 0x96) {
        uint8_t sum = 0;
        for (int i = 0xA0; i <= 0xBC; i++)
            sum = (uint8_t)(sum - rom[i]);
        if ((uint8_t)(sum - 0x19) == rom[0xBD])
            for (int i = 0; i < 4; i++)
                code[i] = code_char(rom[0xAC + i]);
    }
    snprintf(out, out_n, "%s-%08X", code, (unsigned)crc32_of(rom ? rom : (const uint8_t *)"", rom ? size : 0));
    return 0;
}

/* The player's mapping (console_pad.h, RECOMP_GBA_PAD_MAP), loaded when a game starts; the defaults until then. */
static ConsolePadMap s_pad_map;

uint16_t gba_runtime_keys_of(uint32_t t)
{
    static const uint16_t keys[GBA_T_COUNT] = {
        [GBA_T_A] = GBA_KEY_A, [GBA_T_B] = GBA_KEY_B, [GBA_T_L] = GBA_KEY_L, [GBA_T_R] = GBA_KEY_R,
        [GBA_T_START] = GBA_KEY_START, [GBA_T_SELECT] = GBA_KEY_SELECT, [GBA_T_UP] = GBA_KEY_UP,
        [GBA_T_DOWN] = GBA_KEY_DOWN, [GBA_T_LEFT] = GBA_KEY_LEFT, [GBA_T_RIGHT] = GBA_KEY_RIGHT,
    };
    uint16_t k = 0;
    for (int i = 0; i < GBA_T_COUNT; i++)
        if (t & (1u << i))
            k |= keys[i];
    /* Opposite directions at once (a worn stick, two pads): the GBA's own pad cannot, some games misbehave. */
    if ((k & (GBA_KEY_UP | GBA_KEY_DOWN)) == (GBA_KEY_UP | GBA_KEY_DOWN))
        k &= (uint16_t)~(GBA_KEY_UP | GBA_KEY_DOWN);
    if ((k & (GBA_KEY_LEFT | GBA_KEY_RIGHT)) == (GBA_KEY_LEFT | GBA_KEY_RIGHT))
        k &= (uint16_t)~(GBA_KEY_LEFT | GBA_KEY_RIGHT);
    return k;
}

uint16_t gba_runtime_pad_keys(uint32_t b, int lx, int ly, int lt, int rt)
{
    ConsolePadMap m;
    ConsolePadOut o;
    HostPadState st = {.connected = 1, .buttons = b};
    st.axis[HOST_PAD_AXIS_LX] = (int16_t)lx;
    st.axis[HOST_PAD_AXIS_LY] = (int16_t)ly;
    st.axis[HOST_PAD_AXIS_LT] = (int16_t)lt;
    st.axis[HOST_PAD_AXIS_RT] = (int16_t)rt;
    console_pad_defaults(&m, CONSOLE_PAD_GBA);
    console_pad_apply(&m, &st, &o);
    return gba_runtime_keys_of(o.buttons);
}

void gba_runtime_rgb555_to_rgba(const uint16_t *src, uint8_t *dst, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned c = src[i], r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        dst[4 * i + 0] = (uint8_t)(r << 3 | r >> 2);
        dst[4 * i + 1] = (uint8_t)(g << 3 | g >> 2);
        dst[4 * i + 2] = (uint8_t)(b << 3 | b >> 2);
        dst[4 * i + 3] = 255;
    }
}

int gba_runtime_save_path(const char *save_dir, const char *key, char *out, size_t out_n)
{
    if (!save_dir || !*save_dir || !key || !*key || !out)
        return -1;
    int n = snprintf(out, out_n, "%s/gba-%s/game.sav", save_dir, key);
    return n < 0 || (size_t)n >= out_n ? -1 : 0;
}

/* ---- Files ---------------------------------------------------------------------------------------------------- */

/* A whole file, or "fd:N" read from offset 0 (the descriptor stays open and its offset is not used). */
static uint8_t *read_all(const char *spec, size_t max, size_t *size)
{
    int fd, own = 1;
    if (strncmp(spec, "fd:", 3) == 0) {
        char *end;
        long v = strtol(spec + 3, &end, 10);
        if (end == spec + 3 || *end || v < 0)
            return NULL;
        fd = (int)v;
        own = 0;
    } else {
        fd = open(spec, O_RDONLY | O_CLOEXEC);
    }
    if (fd < 0)
        return NULL;
    size_t cap = 1 << 20, n = 0;
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

/* Writes path through path.tmp + rename, so a kill mid-write leaves the old save. */
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

static int write_ppm(const char *path, const uint16_t *fb)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fprintf(f, "P6\n%d %d\n255\n", GBA_SCREEN_W, GBA_SCREEN_H);
    uint8_t px[4];
    for (int i = 0; i < GBA_SCREEN_W * GBA_SCREEN_H; i++) {
        gba_runtime_rgb555_to_rgba(&fb[i], px, 1);
        fwrite(px, 1, 3, f);
    }
    return fclose(f);
}

/* ---- Saves ---------------------------------------------------------------------------------------------------- */

/* The run's machine and its save, shared with the exit hook (the in-game menu's Quit ends the process from the UI
 * thread): s_mu is held while a frame runs and while the save is written. */
static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static gba_machine *s_m;
static char s_save_path[1100];
static uint8_t *s_saved;   /* the image as last read or written */
static size_t s_saved_n;
static char s_discovered[1200];   /* gba_runtime_opts.discovered_path of the run */

/* Writes the save when it differs from what is on disk (or was there at start). Called with s_mu held. */
static void save_flush_locked(const char *why)
{
    size_t n = 0;
    const uint8_t *d;
    if (!s_m || !s_save_path[0] || gba_save_type(s_m) == GBA_SAVE_NONE || !(d = gba_save_data(s_m, &n)) || !n)
        return;
    if (s_saved && n == s_saved_n && memcmp(d, s_saved, n) == 0)
        return;
    if (write_file(s_save_path, d, n) != 0) {
        fprintf(stderr, "gba: cannot write the save %s: %s\n", s_save_path, strerror(errno));
        return;
    }
    uint8_t *copy = realloc(s_saved, n);
    if (copy) {
        memcpy(copy, d, n);
        s_saved = copy;
        s_saved_n = n;
    }
    printf("gba: save written (%zu bytes, %s) to %s\n", n, why, s_save_path);
    fflush(stdout);
}

static void save_flush(const char *why)
{
    pthread_mutex_lock(&s_mu);
    save_flush_locked(why);
    pthread_mutex_unlock(&s_mu);
}

static void on_exit_hook(const char *why)
{
    (void)why;
    save_flush("exit");
    /* The in-game menu's Quit ends the process here: the discovery loop's addresses too. */
    pthread_mutex_lock(&s_mu);
    if (s_m && s_discovered[0])
        gba_pack_save_missed(s_m, s_discovered);
    pthread_mutex_unlock(&s_mu);
}

/* The app paused (background, the in-game menu): the save goes to disk at once, from the thread that paused it. On
 * Android SDL parks the thread that pumps events while the app is in the background, so the run loop may not get to
 * it, and the process can be killed there. Registered once (the lifecycle keeps its listeners); no run, no-op. */
static void on_lifecycle(int paused, void *user)
{
    (void)user;
    if (paused)
        save_flush("pause");
}

static void save_load(gba_machine *m)
{
    size_t n = 0;
    uint8_t *d = s_save_path[0] ? read_all(s_save_path, 1u << 20, &n) : NULL;
    if (d) {
        if (gba_save_load(m, d, n) == 0)
            printf("gba: save loaded (%zu bytes) from %s\n", n, s_save_path);
        else
            fprintf(stderr, "gba: the save %s (%zu bytes) does not fit this game's save memory, not loaded\n",
                    s_save_path, n);
        free(d);
    } else if (s_save_path[0]) {
        printf("gba: no save yet (%s)\n", s_save_path);
    }
    /* What is on disk now (or the blank memory): written again only once it changes. */
    const uint8_t *cur = gba_save_data(m, &n);
    free(s_saved);
    s_saved = NULL;
    s_saved_n = 0;
    if (cur && n && (s_saved = malloc(n))) {
        memcpy(s_saved, cur, n);
        s_saved_n = n;
    }
}

/* ---- Save states (XBA-182) ------------------------------------------------------------------------------------ */

static char s_state_dir[1100];                  /* <save dir>/gba-<KEY> */
static char s_key[64];
static uint16_t s_fb[GBA_SCREEN_W * GBA_SCREEN_H]; /* the last frame (written under s_mu) */
static int s_state_loaded;                      /* a state was loaded: the loop resets its clock (under s_mu) */

/* recomp_states_save's callback, any thread: the state and a thumbnail of the last frame, taken under s_mu (between
 * frames), then written to <dir>/state<slot>.st. */
static int state_save_op(int slot, void *user)
{
    (void)user;
    char path[1200];
    if (recomp_states_slot_path(s_state_dir, slot, path, sizeof path) != 0)
        return RECOMP_STATE_ESLOT;
    static uint8_t rgba[GBA_SCREEN_W * GBA_SCREEN_H * 4];
    static uint16_t thumb[RECOMP_STATE_THUMB_W * RECOMP_STATE_THUMB_H];
    recomp_state_meta meta;
    memset(&meta, 0, sizeof meta);
    snprintf(meta.console, sizeof meta.console, "gba");
    snprintf(meta.key, sizeof meta.key, "%.47s", s_key);
    pthread_mutex_lock(&s_mu);
    if (!s_m) {
        pthread_mutex_unlock(&s_mu);
        return RECOMP_STATE_ENOGAME;
    }
    size_t cap = gba_state_size(s_m), n = 0;
    uint8_t *blob = malloc(cap);
    if (blob)
        n = gba_state_save(s_m, blob, cap);
    meta.frames = gba_frames(s_m);
    gba_runtime_rgb555_to_rgba(s_fb, rgba, GBA_SCREEN_W * GBA_SCREEN_H);
    pthread_mutex_unlock(&s_mu);
    recomp_states_thumb(rgba, GBA_SCREEN_W, GBA_SCREEN_H, GBA_SCREEN_W * 4, 3, 2, thumb, &meta.thumb_w, &meta.thumb_h);
    int rc = n ? recomp_states_write(path, &meta, thumb, blob, n) : RECOMP_STATE_EIO;
    free(blob);
    printf("gba: state slot %d %s (%zu bytes, frame %llu) %s\n", slot, rc == 0 ? "saved" : "not saved", n,
           (unsigned long long)meta.frames, rc == 0 ? path : recomp_states_error(rc));
    fflush(stdout);
    return rc;
}

/* A state was loaded into m (a slot, a rewind step), s_mu held: the audio it had queued was heard already, and its
 * save memory is what the game sees now: the file on disk changes only once the game writes. */
static void state_loaded_locked(gba_machine *m)
{
    int16_t sink[1024 * 2];
    while (gba_audio_pull(m, sink, 1024) > 0)
        ;
    size_t sn = 0;
    const uint8_t *cur = gba_save_data(m, &sn);
    uint8_t *copy = cur && sn ? realloc(s_saved, sn) : NULL;
    if (copy) {
        memcpy(copy, cur, sn);
        s_saved = copy;
        s_saved_n = sn;
    }
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
    if (rc == RECOMP_STATE_OK && strcmp(meta.console, "gba") != 0)
        rc = RECOMP_STATE_EGAME;
    if (rc == RECOMP_STATE_OK) {
        pthread_mutex_lock(&s_mu);
        if (!s_m) {
            rc = RECOMP_STATE_ENOGAME;
        } else {
            rc = gba_state_load(s_m, blob, n); /* GBA_STATE_* share RECOMP_STATE_*'s values */
            if (rc == GBA_STATE_OK) {
                state_loaded_locked(s_m);
                s_state_loaded = 1;
            }
        }
        pthread_mutex_unlock(&s_mu);
    }
    free(blob);
    if (rc == 0)
        printf("gba: state slot %d loaded (%s)\n", slot, path);
    else
        printf("gba: state slot %d not loaded: %s\n", slot, recomp_states_error(rc));
    fflush(stdout);
    return rc;
}

/* ---- Rewind (XBA-194) ----------------------------------------------------------------------------------------- */

/* A capture: the core's state, then the picture of the frame it ends (the state does not hold it), so a step back
 * shows that frame at once. Under s_mu, on the frame thread. */
static size_t rewind_size(void *user)
{
    return gba_state_size((gba_machine *)user) + sizeof s_fb;
}

static size_t rewind_save(uint8_t *out, size_t cap, void *user)
{
    gba_machine *m = user;
    if (cap <= sizeof s_fb)
        return 0;
    size_t n = gba_state_save(m, out, cap - sizeof s_fb);
    if (!n)
        return 0;
    memcpy(out + n, s_fb, sizeof s_fb);
    return n + sizeof s_fb;
}

static int rewind_load(const uint8_t *in, size_t n, void *user)
{
    gba_machine *m = user;
    if (n <= sizeof s_fb || gba_state_load(m, in, n - sizeof s_fb) != GBA_STATE_OK)
        return -1;
    memcpy(s_fb, in + n - sizeof s_fb, sizeof s_fb);
    state_loaded_locked(m);
    return 0;
}

/* ---- The run -------------------------------------------------------------------------------------------------- */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Relative sleeps to an absolute deadline (macOS has no clock_nanosleep); the deadline itself never drifts. */
/* Through the window: with frame generation (XBA-186) it draws the generated and real frames due meanwhile. */
static void sleep_until(uint64_t t)
{
    host_window_sleep_until(t);
}

/* Desktop keys: arrows, X / Z = A / B, A / S = L / R, Enter = Start, Backspace or right Shift = Select. */
static uint16_t keyboard_keys(void)
{
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    uint16_t k = 0;
    if (!ks)
        return 0;
    if (ks[SDL_SCANCODE_X]) k |= GBA_KEY_A;
    if (ks[SDL_SCANCODE_Z]) k |= GBA_KEY_B;
    if (ks[SDL_SCANCODE_A]) k |= GBA_KEY_L;
    if (ks[SDL_SCANCODE_S]) k |= GBA_KEY_R;
    if (ks[SDL_SCANCODE_RETURN]) k |= GBA_KEY_START;
    if (ks[SDL_SCANCODE_BACKSPACE] || ks[SDL_SCANCODE_RSHIFT]) k |= GBA_KEY_SELECT;
    if (ks[SDL_SCANCODE_UP]) k |= GBA_KEY_UP;
    if (ks[SDL_SCANCODE_DOWN]) k |= GBA_KEY_DOWN;
    if (ks[SDL_SCANCODE_LEFT]) k |= GBA_KEY_LEFT;
    if (ks[SDL_SCANCODE_RIGHT]) k |= GBA_KEY_RIGHT;
    return k;
}

/* Every connected pad drives the one GBA (a handheld's own controls and an external pad both work), through the
 * player's mapping; the on-screen pad joins pad 1 (console_pad_read). The player's hotkeys (XBA-190,
 * recomp_hotkeys.h) are read on each physical pad: *hk_held gets the actions whose chord a pad holds, *hk_pressed
 * those whose chord went down this frame; a held chord's buttons reach the game as nothing. */
static recomp_hotkeys_state s_hk_pad[HOST_PAD_PORTS];
static uint16_t pads_keys(uint32_t *hk_held, uint32_t *hk_pressed)
{
    uint16_t k = 0;
    const recomp_hotkeys *hk = recomp_hotkeys_get();
    if (!s_pad_map.console)
        console_pad_defaults(&s_pad_map, CONSOLE_PAD_GBA);
    *hk_held = *hk_pressed = 0;
    for (int p = 0; p < HOST_PAD_PORTS; p++) {
        ConsolePadOut o;
        if (console_pad_read(&s_pad_map, p, &o)) {
            HostPadState st;
            uint32_t src = host_pad_read(p, &st) ? console_pad_sources(&st, s_pad_map.stick_threshold, RECOMP_HK_TRIGGER)
                                                 : 0;
            uint32_t pressed = 0, swallow = 0;
            *hk_held |= recomp_hotkeys_pad_update(hk, &s_hk_pad[p], src, &pressed, &swallow);
            *hk_pressed |= pressed;
            console_pad_swallow(&s_pad_map, &o, swallow); /* a held chord's buttons: not the on-screen pad's */
            k |= gba_runtime_keys_of(o.buttons);
        }
    }
    return k;
}

void gba_runtime_opts_init(gba_runtime_opts *o)
{
    memset(o, 0, sizeof *o);
    o->scale = -1;
    o->filter = -1;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

void gba_runtime_display(const gba_runtime_opts *o, int *scale, int *filter)
{
    /* RECOMP_GBA_SCALE / _FILTER, else the shared RECOMP_SCALE / RECOMP_FILTER (XBA-181); sharp pixels by default. */
    int s = o && o->scale >= 0 ? o->scale : host_window_env_scale("RECOMP_GBA_SCALE", HOST_SCALE_ASPECT);
    int f = o && o->filter >= 0 ? o->filter : host_window_env_filter("RECOMP_GBA_FILTER", HOST_FILTER_NEAREST);
    if (scale)
        *scale = s;
    if (filter)
        *filter = f;
}

int gba_runtime_run(const gba_runtime_opts *o)
{
    size_t rom_n = 0, bios_n = 0;
    uint8_t *rom = o && o->rom ? read_all(o->rom, GBA_ROM_MAX, &rom_n) : NULL;
    uint8_t *bios = NULL;
    char key[32];

    if (!rom) {
        fprintf(stderr, "gba: cannot read the ROM %s (missing, empty or over 32 MB)\n", o && o->rom ? o->rom : "(none)");
        return -1;
    }
    if (o->title_key && *o->title_key)
        snprintf(key, sizeof key, "%s", o->title_key);
    else
        gba_runtime_key(rom, rom_n, key, sizeof key);
    const char *bios_path = o->bios ? o->bios : getenv("RECOMP_GBA_BIOS");
    if (bios_path && *bios_path) {
        bios = read_all(bios_path, BIOS_SIZE, &bios_n);
        if (!bios || bios_n != BIOS_SIZE) {
            fprintf(stderr, "gba: BIOS %s is not a 16 KB image, using the built-in BIOS\n", bios_path);
            free(bios);
            bios = NULL;
            bios_n = 0;
        }
    }
    char rom_sha[65] = "";
    if (o->pack_lib && *o->pack_lib) {
        uint8_t sum[32];
        gba_pack_sha256(rom, rom_n, sum);
        gba_pack_hex(sum, rom_sha);
    }
    gba_machine *m = gba_create(rom, rom_n, bios, bios_n);
    free(bios);
    free(rom);
    if (!m) {
        fprintf(stderr, "gba: the machine cannot start (ROM of %zu bytes)\n", rom_n);
        return -1;
    }
    printf("gba: %s, game %s (%zu KB), key %s, BIOS %s\n", gba_runtime_version(), base_name(o->rom), rom_n / 1024,
           key, bios_n ? bios_path : "built-in");
    gba_pack pack;
    gba_pack_load(m, o->pack_lib, key, rom_sha, &pack);   /* the interpreter whatever happens (logged) */

    pthread_mutex_lock(&s_mu);
    s_m = m;
    if (gba_runtime_save_path(o->save_dir, key, s_save_path, sizeof s_save_path) != 0)
        s_save_path[0] = 0;
    save_load(m);
    snprintf(s_discovered, sizeof s_discovered, "%s", o->discovered_path ? o->discovered_path : "");
    pthread_mutex_unlock(&s_mu);
    recomp_exit_hook_set(on_exit_hook);
    static int listening;
    if (!listening && recomp_lifecycle_listen(on_lifecycle, NULL) == 0)
        listening = 1;
    /* Save states: <save dir>/gba-<KEY>/state<N>.st, beside the save (none without a save dir). */
    snprintf(s_key, sizeof s_key, "%s", key);
    if (!o->save_dir || !*o->save_dir ||
        snprintf(s_state_dir, sizeof s_state_dir, "%s/gba-%s", o->save_dir, key) >= (int)sizeof s_state_dir)
        s_state_dir[0] = 0;
    recomp_state_ops ops = {"gba", state_save_op, state_load_op, NULL};
    recomp_states_attach(&ops, s_state_dir);
    printf("gba: save states %s%s, fast-forward x%d%s\n", s_state_dir[0] ? "in " : "off (no save dir)", s_state_dir,
           recomp_ff_speed(), recomp_ff_speed() ? "" : " (max)");
    memset(s_hk_pad, 0, sizeof s_hk_pad);
    recomp_hotkeys_get(); /* RECOMP_HOTKEYS / _KEYS, logged once (XBA-190) */
    recomp_rewinder rw;
    recomp_rewinder_init(&rw, "gba", NULL); /* RECOMP_REWIND, _MB, _FRAMES (XBA-194) */
    gba_cheats cheats;
    gba_cheats_open(&cheats, m, pack.table, NULL); /* RECOMP_CHEATS_FILE (XBA-197) */
    int resume = recomp_states_resume_enabled() && s_state_dir[0];
    if (resume) {
        int r = recomp_states_resume();
        printf("gba: resume from the automatic slot: %s\n", r == RECOMP_STATE_EEMPTY ? "nothing saved yet"
                                                                                     : recomp_states_error(r));
    }

    /* The window, or none (headless checks: SDL dummy drivers). */
    int window = !o->headless && !host_window_headless();
    if (SDL_WasInit(SDL_INIT_EVENTS) == 0)
        SDL_InitSubSystem(SDL_INIT_EVENTS);
    int scale, filter;
    gba_runtime_display(o, &scale, &filter);
    host_window_set_scaling(scale, filter);
    host_window_shader_env("RECOMP_GBA_SHADER"); /* XBA-195: picture shader */
    if (window) {
        char title[300];
        snprintf(title, sizeof title, "XRecomp GBA - %s", base_name(o->rom));
        if (!host_window_open(title, GBA_SCREEN_W * 4, GBA_SCREEN_H * 4, host_window_env_vsync())) {
            fprintf(stderr, "gba: no window, running headless\n");
            window = 0;
        }
    }
    if (!window)
        host_pad_init();
    console_pad_load(&s_pad_map, CONSOLE_PAD_GBA, getenv(console_pad_env(CONSOLE_PAD_GBA)));
    int audio = host_audio_open(GBA_AUDIO_RATE, 2);
    printf("gba: window %s (%s, %s), audio %s, pads %u\n", window ? "on" : "off",
           scale == HOST_SCALE_INTEGER ? "integer" : scale == HOST_SCALE_STRETCH ? "stretch" : "aspect",
           filter == HOST_FILTER_NEAREST ? "nearest" : "linear",
           audio ? "on" : "off", host_pad_mask());
    fflush(stdout);

    uint16_t *fb = s_fb;
    static uint8_t rgba[GBA_SCREEN_W * GBA_SCREEN_H * 4];
    static int16_t pcm[GBA_AUDIO_RATE / 10 * 2];
    uint64_t frames = 0, next = now_ns(), dirty_at = 0, t0 = next, late_resets = 0;
    int was_paused = 0, heard = 0, rc = 0, was_ff = 0;
    recomp_hotkeys_state hk_keys = {0};
    recomp_ff_carry carry = {{0, 0}, 0};
    recomp_perf_hint_begin(GBA_FRAME_NS); /* the frame loop, one session (no worker threads) */

    for (;;) {
        int running = 1;
        if (window) {
            running = host_window_pump();
        } else {
            SDL_Event ev;
            host_pad_update();
            running = SDL_PeepEvents(&ev, 1, SDL_GETEVENT, SDL_QUIT, SDL_QUIT) <= 0;   /* SIGINT, a test */
            SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
        }
        if (!running) {
            printf("gba: quit asked after %llu frames\n", (unsigned long long)frames);
            if (resume && frames > 0) /* the player closed the window: the next start resumes here */
                printf("gba: automatic slot %s\n", recomp_states_error(recomp_states_save(RECOMP_STATE_AUTO)));
            break;
        }
        if (recomp_lifecycle_paused()) {
            if (!was_paused) {
                printf("gba: paused at frame %llu\n", (unsigned long long)frames);
                fflush(stdout);
                save_flush("pause");
                dirty_at = 0;
                was_paused = 1;
            }
            SDL_Delay(16);
            continue;
        }
        if (was_paused) {
            printf("gba: resumed\n");
            fflush(stdout);
            was_paused = 0;
            next = now_ns();   /* the time away is not caught up */
        }
        uint64_t work_t0 = now_ns(); /* the frame's work, for the performance hint (the sleep after it is not) */

        uint32_t hk_held = 0, hk_pressed = 0, key_pressed = 0;
        uint16_t keys = (uint16_t)(o->keys | pads_keys(&hk_held, &hk_pressed) | (window ? keyboard_keys() : 0));
        if (window) { /* the desktop keys (RECOMP_HOTKEYS_KEYS: Tab, F1-F4, Shift+F1-F4 by default) */
            int nks = 0;
            const Uint8 *ks = SDL_GetKeyboardState(&nks);
            hk_held |= recomp_hotkeys_keys_update(recomp_hotkeys_get(), &hk_keys, ks, nks, &key_pressed);
        }
        recomp_hotkeys_run(hk_pressed | key_pressed);
        recomp_ff_hold(!!(hk_held & RECOMP_HK_BIT(RECOMP_HK_FF_HOLD)));
        /* Rewind held: one capture back per shown frame, no GBA frame runs, nothing heard (XBA-194). */
        int rewinding = recomp_rewinder_begin(&rw, !!(hk_held & RECOMP_HK_BIT(RECOMP_HK_REWIND)));
        if (rewinding) {
            pthread_mutex_lock(&s_mu);
            if (recomp_rewinder_step(&rw, rewind_load, m) == 0)
                recomp_states_note_load(); /* a fresh frame generation history */
            pthread_mutex_unlock(&s_mu);
        }
        int ff = recomp_ff_active(), speed = recomp_ff_speed();
        if (ff != was_ff) {
            printf("gba: fast-forward %s at frame %llu\n", ff ? (speed ? "on" : "on (max)") : "off",
                   (unsigned long long)frames);
            fflush(stdout);
            carry = (recomp_ff_carry){{0, 0}, 0};
            was_ff = ff;
            next = now_ns();
        }
        /* Fast-forward: `speed` GBA frames per shown frame (max: as many as fit in one display frame). */
        int steps = rewinding ? 0 : ff ? (speed ? speed : 64) : 1, dirty = 0;
        uint64_t burst_end = now_ns() + GBA_FRAME_NS * 9 / 10;
        gba_cheats_poll(&cheats); /* the app changed the list: read it again */
        for (int step = 0; step < steps; step++) {
            pthread_mutex_lock(&s_mu);
            gba_set_keys(m, keys);
            gba_run_frame(m, fb);
            gba_cheats_frame(&cheats, keys);
            recomp_rewinder_frame(&rw, rewind_size, rewind_save, m);
            dirty |= gba_save_take_dirty(m);
            if (s_state_loaded) {
                s_state_loaded = 0;
                next = now_ns(); /* a loaded state: the clock starts again from it */
            }
            pthread_mutex_unlock(&s_mu);
            frames++;

            /* Sound: everything the frame made (about 803 sample frames); averaged down while fast-forwarding. */
            size_t got;
            while ((got = gba_audio_pull(m, pcm, sizeof pcm / sizeof pcm[0] / 2)) > 0) {
                heard = 1;
                if (ff)
                    got = speed ? recomp_ff_audio(pcm, got, speed, &carry) : 0;
                if (audio && got)
                    host_audio_queue(pcm, (uint32_t)got);
            }
            if (o->frames && frames >= o->frames)
                break;
            if (ff && !speed && now_ns() >= burst_end)
                break;
        }

        uint64_t t = now_ns();
        if (dirty && !dirty_at)
            dirty_at = t;
        if (dirty_at && t - dirty_at >= SAVE_DEBOUNCE_NS) {
            save_flush("written by the game");
            dirty_at = 0;
        }

        if (window) {
            if (o->capture_path && o->capture_frame == frames)
                host_window_capture(o->capture_path);
            gba_runtime_rgb555_to_rgba(fb, rgba, GBA_SCREEN_W * GBA_SCREEN_H);
            host_window_present_rgba(rgba, GBA_SCREEN_W, GBA_SCREEN_H, GBA_SCREEN_W * 4);
        }
        if (o->frames && frames >= o->frames)
            break;
        recomp_perf_hint_frame((int64_t)(now_ns() - work_t0), (int64_t)GBA_FRAME_NS);

        if (!o->unpaced && !(ff && !speed)) {
            next += GBA_FRAME_NS;
            /* Audio-driven: a queue above its target slows the clock a little, below it speeds it up (±2%). */
            if (audio && heard) {
                uint32_t q = host_audio_queued();
                if (q > AUDIO_TARGET_FRAMES * 2)
                    next += GBA_FRAME_NS / 50;
                else if (q < AUDIO_TARGET_FRAMES / 2)
                    next -= GBA_FRAME_NS / 50;
            }
            t = now_ns();
            if (t > next + 3 * GBA_FRAME_NS) {
                next = t;   /* a stall (a slow host, the debugger): no burst to catch up */
                late_resets++;
            } else if (next > t) {
                sleep_until(next);
            }
        }
    }

    recomp_perf_hint_end();
    double secs = (double)(now_ns() - t0) / 1e9;
    printf("gba: %llu frames in %.1f s (%.2f fps), %llu late resets\n", (unsigned long long)frames, secs,
           secs > 0 ? (double)frames / secs : 0.0, (unsigned long long)late_resets);
    save_flush("exit");
    if (o->screenshot) {
        if (write_ppm(o->screenshot, fb) == 0)
            printf("gba: last frame written to %s\n", o->screenshot);
        else
            rc = -1;
    }
    fflush(stdout);
    recomp_exit_hook_set(NULL);
    recomp_states_detach(); /* before s_mu: a save / load in progress finishes first */
    recomp_ff_hold(0);
    recomp_rewinder_free(&rw); /* logs the capture cost */
    gba_cheats_close(&cheats); /* the cartridge as it was */
    if (audio)
        host_audio_close();
    if (window)
        host_window_close();
    pthread_mutex_lock(&s_mu);
    s_m = NULL;
    s_save_path[0] = 0;
    free(s_saved);
    s_saved = NULL;
    s_saved_n = 0;
    pthread_mutex_unlock(&s_mu);
    if (s_discovered[0])
        gba_pack_save_missed(m, s_discovered);
    s_discovered[0] = 0;
    gba_destroy(m);
    gba_pack_close(&pack);
    return rc;
}

/* ---- The app shell's entry ------------------------------------------------------------------------------------ */

static char s_start_save_dir[1024];
static char s_start_work_dir[1024];

void gba_runtime_set_work_dir(const char *dir)
{
    snprintf(s_start_work_dir, sizeof s_start_work_dir, "%s", dir ? dir : "");
}

void gba_runtime_set_save_dir(const char *dir)
{
    snprintf(s_start_save_dir, sizeof s_start_save_dir, "%s", dir ? dir : "");
}

int gba_runtime_start(const char *rom, const char *pack_lib, const char *title_key)
{
    gba_runtime_opts o;
    gba_runtime_opts_init(&o);
    o.rom = rom;
    o.title_key = title_key;
    o.save_dir = s_start_save_dir[0] ? s_start_save_dir : NULL;
    o.pack_lib = pack_lib;
    char discovered[1200];
    if (s_start_work_dir[0] && title_key && *title_key) {
        snprintf(discovered, sizeof discovered, "%s/gba-%s/discovered.txt", s_start_work_dir, title_key);
        o.discovered_path = discovered;
    }
    const char *frames = getenv("RECOMP_GBA_FRAMES");   /* a bounded run (checks) */
    if (frames && *frames)
        o.frames = strtoull(frames, NULL, 10);
    printf("gba runtime ready (%s; title %s, pack %s, game %s)\n", gba_runtime_version(), title_key ? title_key : "?",
           pack_lib && *pack_lib ? pack_lib : "none (interpreter)", rom ? rom : "?");
    fflush(stdout);
    return gba_runtime_run(&o);
}
