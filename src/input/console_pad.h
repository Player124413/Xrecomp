/* console_pad -- the player's mapping from a host pad (host_pad.h) to a console's
 * own pad: the GBA's ten keys, a PS1 DualShock, an N64 controller (XBA-158 /
 * XBA-168 / XBA-174). Part of the host layer (src/platform/HOST_LAYER.md):
 * pure logic, libc only, no SDL; host_pad.c adds console_pad_read on top.
 *
 * A console has targets (its buttons, in the order below: bit n of
 * ConsolePadOut.buttons is target n) and up to two analog sticks. Each target
 * takes a set of host sources (a button, a trigger past trigger_threshold, a
 * stick direction past stick_threshold); each stick takes a host stick (left,
 * right) or none. The defaults are the fixed mappings the runtimes had before
 * this layer, so a game with no mapping plays as it did.
 *
 * Text form (RECOMP_GBA_PAD_MAP / RECOMP_PS1_PAD_MAP / RECOMP_N64_PAD_MAP in
 * recomp.cfg, or the per-game value the Android app sets in the environment):
 * entries separated by commas, semicolons or spaces, each
 *   <target>=<source>[+<source>...] | <target>=none
 *   left_stick= / right_stick= (PS1), stick= (N64): left | right | none
 *   range=<1..127>, deadzone=<0..90> (N64: the stick's travel and dead zone in
 *   percent; absent: the runtime's RECOMP_N64_STICK_RANGE / RECOMP_N64_DEADZONE)
 * listing only what differs from the default. Sources are the names
 * src/input/input_map.c and the app's PadMapping use (south = the bottom face
 * button, SDL's A): south east west north left_shoulder right_shoulder
 * left_trigger right_trigger left_stick_click right_stick_click back start
 * dpad_up dpad_down dpad_left dpad_right left_stick_up ... right_stick_right.
 * Example: "a=east,b=south,l=left_trigger". */
#ifndef XBR_CONSOLE_PAD_H
#define XBR_CONSOLE_PAD_H

#include <stdint.h>

#include "host_pad.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { CONSOLE_PAD_NONE, CONSOLE_PAD_GBA, CONSOLE_PAD_PS1, CONSOLE_PAD_N64, CONSOLE_PAD_CONSOLES };

/* Host sources, bit n of a source set. */
enum {
    CPS_SOUTH, CPS_EAST, CPS_WEST, CPS_NORTH,
    CPS_LEFT_SHOULDER, CPS_RIGHT_SHOULDER, CPS_LEFT_TRIGGER, CPS_RIGHT_TRIGGER,
    CPS_LEFT_STICK_CLICK, CPS_RIGHT_STICK_CLICK, CPS_BACK, CPS_START,
    CPS_DPAD_UP, CPS_DPAD_DOWN, CPS_DPAD_LEFT, CPS_DPAD_RIGHT,
    CPS_LSTICK_UP, CPS_LSTICK_DOWN, CPS_LSTICK_LEFT, CPS_LSTICK_RIGHT,
    CPS_RSTICK_UP, CPS_RSTICK_DOWN, CPS_RSTICK_LEFT, CPS_RSTICK_RIGHT,
    CPS_SOURCES
};
#define CPS_BIT(s) (1u << (s))

/* Targets per console (the app's ConsolePadMap.java keeps the same order). */
enum {
    GBA_T_A, GBA_T_B, GBA_T_L, GBA_T_R, GBA_T_START, GBA_T_SELECT,
    GBA_T_UP, GBA_T_DOWN, GBA_T_LEFT, GBA_T_RIGHT, GBA_T_COUNT
};
enum {
    PS1_T_CROSS, PS1_T_CIRCLE, PS1_T_SQUARE, PS1_T_TRIANGLE, PS1_T_L1, PS1_T_R1, PS1_T_L2, PS1_T_R2,
    PS1_T_L3, PS1_T_R3, PS1_T_START, PS1_T_SELECT, PS1_T_UP, PS1_T_DOWN, PS1_T_LEFT, PS1_T_RIGHT, PS1_T_COUNT
};
enum {
    N64_T_A, N64_T_B, N64_T_Z, N64_T_L, N64_T_R, N64_T_START, N64_T_UP, N64_T_DOWN, N64_T_LEFT, N64_T_RIGHT,
    N64_T_C_UP, N64_T_C_DOWN, N64_T_C_LEFT, N64_T_C_RIGHT, N64_T_COUNT
};

#define CONSOLE_PAD_MAX_TARGETS 16
#define CONSOLE_PAD_MAX_STICKS 2

enum { CONSOLE_STICK_LEFT, CONSOLE_STICK_RIGHT, CONSOLE_STICK_NONE };

typedef struct ConsolePadMap {
    int console;                               /* CONSOLE_PAD_* */
    int n_targets, n_sticks;
    uint32_t source[CONSOLE_PAD_MAX_TARGETS];  /* CPS_BIT set per target */
    int stick[CONSOLE_PAD_MAX_STICKS];         /* CONSOLE_STICK_* */
    int stick_threshold;                       /* a stick direction as a button: past this (0..32767) */
    int trigger_threshold;                     /* a trigger as a button: past this (0..32767) */
    int range;                                 /* N64 stick travel, -1 = the runtime's */
    int deadzone;                              /* N64 dead zone in percent, -1 = the runtime's */
} ConsolePadMap;

typedef struct ConsolePadOut {
    uint32_t buttons;                          /* bit n = target n */
    int16_t stick_x[CONSOLE_PAD_MAX_STICKS];   /* host units, -32768..32767, +y down (SDL) */
    int16_t stick_y[CONSOLE_PAD_MAX_STICKS];
    uint32_t host_buttons;                     /* the physical pad's raw HOST_PAD_* (chords the runtime reads) */
    uint32_t overlay_buttons;                  /* the targets the touch overlay presses (console_pad_merge) */
} ConsolePadOut;

/* "gba", "ps1", "n64" -> CONSOLE_PAD_*, else CONSOLE_PAD_NONE. */
int console_pad_console(const char *name);
const char *console_pad_console_name(int console);
/* The environment variable a console's mapping is read from (RECOMP_GBA_PAD_MAP ...), or NULL. */
const char *console_pad_env(int console);

/* The console's default mapping into *m. Returns 0, -1 for an unknown console (*m zeroed). */
int console_pad_defaults(ConsolePadMap *m, int console);

/* Applies text on top of *m (normally its defaults). Returns the number of entries rejected (unknown target or
 * source, bad value), or -1 without a map. NULL or "" changes nothing. */
int console_pad_parse(ConsolePadMap *m, const char *text);

/* What differs from the console's defaults, in the text form ("" for the default mapping). Whole entries only. */
const char *console_pad_format(const ConsolePadMap *m, char *buf, int size);

/* Defaults, then text (may be NULL); logs one line "[INPUT] <console> pad map: <text>" (or "(default)", with the
 * entries rejected). Returns the number rejected, -1 for an unknown console. */
int console_pad_load(ConsolePadMap *m, int console, const char *text);

const char *console_pad_target_name(int console, int target);
const char *console_pad_source_name(int source);

/* The targets any of `sources` (CPS_BIT set) drives in *m: what a runtime hides from the game while a hotkey chord of
 * those sources is held (XBA-182: Back + right trigger, hold to fast-forward). */
uint32_t console_pad_targets_using(const ConsolePadMap *m, uint32_t sources);

/* A held hotkey chord's buttons (`swallow`: the physical pad's sources, recomp_hotkeys_pad_update) taken out of a
 * merged *out: only what the physical pad presses is hidden; the touch overlay's targets (out->overlay_buttons) still
 * reach the game, so a chord on the pad never masks an on-screen button (XBA-212). */
void console_pad_swallow(const ConsolePadMap *m, ConsolePadOut *out, uint32_t swallow);

/* The host sources a pad state presses (buttons, triggers and stick directions past the thresholds). */
uint32_t console_pad_sources(const HostPadState *st, int stick_threshold, int trigger_threshold);

/* One pad through a mapping: *out gets the targets pressed, the sticks and the raw buttons. */
void console_pad_apply(const ConsolePadMap *m, const HostPadState *st, ConsolePadOut *out);

/* A physical pad (may be NULL: none) through the player's mapping, merged with the touch overlay (may be NULL)
 * through the console's default mapping: the on-screen buttons are the console's own and never remapped. Buttons
 * or'ed, each stick from whichever is pushed further. Returns 1 when either is there, 0 otherwise (*out zeroed). */
int console_pad_merge(const ConsolePadMap *m, const HostPadState *pad, const HostPadState *overlay,
                      ConsolePadOut *out);

/* Port `port` of host_pad (with the overlay on port 0) through *m: console_pad_merge over host_pad_read and
 * host_pad_overlay_get. In host_pad.c (xbr_host). */
int console_pad_read(const ConsolePadMap *m, int port, ConsolePadOut *out);

#ifdef __cplusplus
}
#endif

#endif /* XBR_CONSOLE_PAD_H */
