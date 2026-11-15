/* console_pad.c -- see console_pad.h (XBA-158 / XBA-168 / XBA-174). */
#include "console_pad.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_source_names[CPS_SOURCES] = {
    "south", "east", "west", "north",
    "left_shoulder", "right_shoulder", "left_trigger", "right_trigger",
    "left_stick_click", "right_stick_click", "back", "start",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
    "left_stick_up", "left_stick_down", "left_stick_left", "left_stick_right",
    "right_stick_up", "right_stick_down", "right_stick_left", "right_stick_right",
};

static const char *const k_gba_targets[GBA_T_COUNT] = {
    "a", "b", "l", "r", "start", "select", "dpad_up", "dpad_down", "dpad_left", "dpad_right",
};
static const char *const k_ps1_targets[PS1_T_COUNT] = {
    "cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2",
    "l3", "r3", "start", "select", "dpad_up", "dpad_down", "dpad_left", "dpad_right",
};
static const char *const k_n64_targets[N64_T_COUNT] = {
    "a", "b", "z", "l", "r", "start", "dpad_up", "dpad_down", "dpad_left", "dpad_right",
    "c_up", "c_down", "c_left", "c_right",
};
static const char *const k_stick_values[3] = { "left", "right", "none" };
static const char *const k_ps1_sticks[2] = { "left_stick", "right_stick" };
static const char *const k_n64_sticks[1] = { "stick" };

#define B(s) CPS_BIT(CPS_##s)
#define DPAD_AND_LSTICK(dir, stick) (B(DPAD_##dir) | B(LSTICK_##stick))

int console_pad_console(const char *name)
{
    if (!name)
        return CONSOLE_PAD_NONE;
    for (int c = 1; c < CONSOLE_PAD_CONSOLES; c++)
        if (strcmp(name, console_pad_console_name(c)) == 0)
            return c;
    return CONSOLE_PAD_NONE;
}

const char *console_pad_console_name(int console)
{
    switch (console) {
    case CONSOLE_PAD_GBA: return "gba";
    case CONSOLE_PAD_PS1: return "ps1";
    case CONSOLE_PAD_N64: return "n64";
    default: return NULL;
    }
}

const char *console_pad_env(int console)
{
    switch (console) {
    case CONSOLE_PAD_GBA: return "RECOMP_GBA_PAD_MAP";
    case CONSOLE_PAD_PS1: return "RECOMP_PS1_PAD_MAP";
    case CONSOLE_PAD_N64: return "RECOMP_N64_PAD_MAP";
    default: return NULL;
    }
}

static const char *const *targets_of(int console, int *n)
{
    switch (console) {
    case CONSOLE_PAD_GBA: *n = GBA_T_COUNT; return k_gba_targets;
    case CONSOLE_PAD_PS1: *n = PS1_T_COUNT; return k_ps1_targets;
    case CONSOLE_PAD_N64: *n = N64_T_COUNT; return k_n64_targets;
    default: *n = 0; return NULL;
    }
}

static const char *const *sticks_of(int console, int *n)
{
    switch (console) {
    case CONSOLE_PAD_PS1: *n = 2; return k_ps1_sticks;
    case CONSOLE_PAD_N64: *n = 1; return k_n64_sticks;
    default: *n = 0; return NULL;
    }
}

const char *console_pad_target_name(int console, int target)
{
    int n;
    const char *const *t = targets_of(console, &n);
    return t && target >= 0 && target < n ? t[target] : NULL;
}

const char *console_pad_source_name(int source)
{
    return source >= 0 && source < CPS_SOURCES ? k_source_names[source] : NULL;
}

int console_pad_defaults(ConsolePadMap *m, int console)
{
    if (!m)
        return -1;
    memset(m, 0, sizeof *m);
    m->range = m->deadzone = -1;
    for (int i = 0; i < CONSOLE_PAD_MAX_STICKS; i++)
        m->stick[i] = CONSOLE_STICK_NONE;
    switch (console) {
    case CONSOLE_PAD_GBA:
        /* gba_runtime.c before XBA-158: Y is a second A and X a second B under the thumb, a trigger is its shoulder,
         * the left stick is the D-pad (a third of its travel). */
        m->n_targets = GBA_T_COUNT;
        m->source[GBA_T_A] = B(SOUTH) | B(NORTH);
        m->source[GBA_T_B] = B(EAST) | B(WEST);
        m->source[GBA_T_L] = B(LEFT_SHOULDER) | B(LEFT_TRIGGER);
        m->source[GBA_T_R] = B(RIGHT_SHOULDER) | B(RIGHT_TRIGGER);
        m->source[GBA_T_START] = B(START);
        m->source[GBA_T_SELECT] = B(BACK);
        m->source[GBA_T_UP] = DPAD_AND_LSTICK(UP, UP);
        m->source[GBA_T_DOWN] = DPAD_AND_LSTICK(DOWN, DOWN);
        m->source[GBA_T_LEFT] = DPAD_AND_LSTICK(LEFT, LEFT);
        m->source[GBA_T_RIGHT] = DPAD_AND_LSTICK(RIGHT, RIGHT);
        m->stick_threshold = 32768 / 3;
        m->trigger_threshold = 16384;
        break;
    case CONSOLE_PAD_PS1:
        /* ps1_runtime.c before XBA-168: by position, the triggers are L2 / R2, the sticks are the DualShock's (and in
         * digital mode the runtime reads the left one as the D-pad too). */
        m->n_targets = PS1_T_COUNT;
        m->n_sticks = 2;
        m->source[PS1_T_CROSS] = B(SOUTH);
        m->source[PS1_T_CIRCLE] = B(EAST);
        m->source[PS1_T_SQUARE] = B(WEST);
        m->source[PS1_T_TRIANGLE] = B(NORTH);
        m->source[PS1_T_L1] = B(LEFT_SHOULDER);
        m->source[PS1_T_R1] = B(RIGHT_SHOULDER);
        m->source[PS1_T_L2] = B(LEFT_TRIGGER);
        m->source[PS1_T_R2] = B(RIGHT_TRIGGER);
        m->source[PS1_T_L3] = B(LEFT_STICK_CLICK);
        m->source[PS1_T_R3] = B(RIGHT_STICK_CLICK);
        m->source[PS1_T_START] = B(START);
        m->source[PS1_T_SELECT] = B(BACK);
        m->source[PS1_T_UP] = B(DPAD_UP);
        m->source[PS1_T_DOWN] = B(DPAD_DOWN);
        m->source[PS1_T_LEFT] = B(DPAD_LEFT);
        m->source[PS1_T_RIGHT] = B(DPAD_RIGHT);
        m->stick[0] = CONSOLE_STICK_LEFT;
        m->stick[1] = CONSOLE_STICK_RIGHT;
        m->stick_threshold = 32768 / 3;
        m->trigger_threshold = 16384;
        break;
    case CONSOLE_PAD_N64:
        /* n64_runtime.c before XBA-174: X is a second B, Y a C-up, either trigger is Z, the right stick is the four C
         * buttons (half its travel), the left stick is the analog stick. */
        m->n_targets = N64_T_COUNT;
        m->n_sticks = 1;
        m->source[N64_T_A] = B(SOUTH);
        m->source[N64_T_B] = B(EAST) | B(WEST);
        m->source[N64_T_Z] = B(LEFT_TRIGGER) | B(RIGHT_TRIGGER);
        m->source[N64_T_L] = B(LEFT_SHOULDER);
        m->source[N64_T_R] = B(RIGHT_SHOULDER);
        m->source[N64_T_START] = B(START);
        m->source[N64_T_UP] = B(DPAD_UP);
        m->source[N64_T_DOWN] = B(DPAD_DOWN);
        m->source[N64_T_LEFT] = B(DPAD_LEFT);
        m->source[N64_T_RIGHT] = B(DPAD_RIGHT);
        m->source[N64_T_C_UP] = B(NORTH) | B(RSTICK_UP);
        m->source[N64_T_C_DOWN] = B(RSTICK_DOWN);
        m->source[N64_T_C_LEFT] = B(RSTICK_LEFT);
        m->source[N64_T_C_RIGHT] = B(RSTICK_RIGHT);
        m->stick[0] = CONSOLE_STICK_LEFT;
        m->stick_threshold = 16384;
        m->trigger_threshold = 8192;
        break;
    default:
        m->range = m->deadzone = 0;
        return -1;
    }
    m->console = console;
    return 0;
}

static int sep(int c)
{
    return c == ',' || c == ';' || isspace(c);
}

static int source_of(const char *name, size_t n)
{
    for (int s = 0; s < CPS_SOURCES; s++)
        if (strlen(k_source_names[s]) == n && strncmp(k_source_names[s], name, n) == 0)
            return s;
    return -1;
}

static int index_of(const char *const *names, int n, const char *key)
{
    for (int i = 0; i < n; i++)
        if (strcmp(names[i], key) == 0)
            return i;
    return -1;
}

static int parse_int(const char *v, int lo, int hi, int *out)
{
    char *end;
    long x;
    if (!*v)
        return 0;
    x = strtol(v, &end, 10);
    if (*end || x < lo || x > hi)
        return 0;
    *out = (int)x;
    return 1;
}

/* One "key=value" (lowercase); 1 when taken. */
static int entry(ConsolePadMap *m, const char *key, const char *value)
{
    int nt, ns, i;
    const char *const *targets = targets_of(m->console, &nt);
    const char *const *sticks = sticks_of(m->console, &ns);

    if (sticks && (i = index_of(sticks, ns, key)) >= 0) {
        int v = index_of(k_stick_values, 3, value);
        if (v < 0)
            return 0;
        m->stick[i] = v;
        return 1;
    }
    if (m->console == CONSOLE_PAD_N64 && strcmp(key, "range") == 0)
        return parse_int(value, 1, 127, &m->range);
    if (m->console == CONSOLE_PAD_N64 && strcmp(key, "deadzone") == 0)
        return parse_int(value, 0, 90, &m->deadzone);
    if (!targets || (i = index_of(targets, nt, key)) < 0)
        return 0;
    if (strcmp(value, "none") == 0) {
        m->source[i] = 0;
        return 1;
    }
    uint32_t set = 0;
    const char *p = value;
    while (*p) {
        const char *plus = strchr(p, '+');
        size_t n = plus ? (size_t)(plus - p) : strlen(p);
        int s = source_of(p, n);
        if (s < 0)
            return 0;
        set |= CPS_BIT(s);
        if (!plus)
            break;
        p = plus + 1;
        if (!*p)
            return 0;
    }
    if (!set)
        return 0;
    m->source[i] = set;
    return 1;
}

int console_pad_parse(ConsolePadMap *m, const char *text)
{
    int rejected = 0;
    if (!m)
        return -1;
    if (!text)
        return 0;
    while (*text) {
        char e[128], *eq;
        size_t n = 0;
        while (*text && sep((unsigned char)*text))
            text++;
        if (!*text)
            break;
        while (text[n] && !sep((unsigned char)text[n]))
            n++;
        if (n >= sizeof e) {
            rejected++;
        } else {
            for (size_t i = 0; i < n; i++)
                e[i] = (char)tolower((unsigned char)text[i]);
            e[n] = '\0';
            eq = strchr(e, '=');
            if (!eq) {
                rejected++;
            } else {
                *eq = '\0';
                if (!entry(m, e, eq + 1))
                    rejected++;
            }
        }
        text += n;
    }
    return rejected;
}

/* Appends "name=value" with a comma; 0 when it does not fit (buf keeps whole entries). */
static int put(char *buf, int size, int *used, const char *name, const char *value)
{
    int n = snprintf(buf + *used, (size_t)(size - *used), "%s%s=%s", *used ? "," : "", name, value);
    if (n < 0 || n >= size - *used) {
        buf[*used] = '\0';
        return 0;
    }
    *used += n;
    return 1;
}

const char *console_pad_format(const ConsolePadMap *m, char *buf, int size)
{
    ConsolePadMap d;
    int used = 0, nt, ns;
    const char *const *targets, *const *sticks;
    if (!buf || size <= 0)
        return buf;
    buf[0] = '\0';
    if (!m || console_pad_defaults(&d, m->console) != 0)
        return buf;
    targets = targets_of(m->console, &nt);
    sticks = sticks_of(m->console, &ns);
    for (int t = 0; t < nt; t++) {
        char v[512];
        size_t vn = 0;
        if (m->source[t] == d.source[t])
            continue;
        v[0] = '\0';
        if (!m->source[t])
            snprintf(v, sizeof v, "none");
        for (int s = 0; s < CPS_SOURCES; s++)
            if (m->source[t] & CPS_BIT(s))
                vn += (size_t)snprintf(v + vn, sizeof v - vn, "%s%s", vn ? "+" : "", k_source_names[s]);
        if (!put(buf, size, &used, targets[t], v))
            return buf;
    }
    for (int i = 0; i < ns; i++) {
        if (m->stick[i] == d.stick[i] || m->stick[i] < 0 || m->stick[i] > CONSOLE_STICK_NONE)
            continue;
        if (!put(buf, size, &used, sticks[i], k_stick_values[m->stick[i]]))
            return buf;
    }
    if (m->console == CONSOLE_PAD_N64) {
        char v[16];
        if (m->range != d.range && m->range > 0) {
            snprintf(v, sizeof v, "%d", m->range);
            if (!put(buf, size, &used, "range", v))
                return buf;
        }
        if (m->deadzone != d.deadzone && m->deadzone >= 0) {
            snprintf(v, sizeof v, "%d", m->deadzone);
            put(buf, size, &used, "deadzone", v);
        }
    }
    return buf;
}

int console_pad_load(ConsolePadMap *m, int console, const char *text)
{
    char buf[1024];
    int rejected;
    if (console_pad_defaults(m, console) != 0)
        return -1;
    rejected = console_pad_parse(m, text);
    console_pad_format(m, buf, sizeof buf);
    fprintf(stderr, "  [INPUT] %s pad map: %s", console_pad_console_name(console), buf[0] ? buf : "(default)");
    if (rejected > 0)
        fprintf(stderr, " (%d entr%s of \"%s\" not understood, skipped)", rejected, rejected == 1 ? "y" : "ies",
                text);
    fprintf(stderr, "\n");
    return rejected;
}

uint32_t console_pad_targets_using(const ConsolePadMap *m, uint32_t sources)
{
    uint32_t t = 0;
    for (int i = 0; m && i < m->n_targets && i < CONSOLE_PAD_MAX_TARGETS; i++)
        if (m->source[i] & sources)
            t |= 1u << i;
    return t;
}

uint32_t console_pad_sources(const HostPadState *st, int stick_threshold, int trigger_threshold)
{
    static const struct { uint32_t host; int source; } k_buttons[] = {
        {HOST_PAD_A, CPS_SOUTH}, {HOST_PAD_B, CPS_EAST}, {HOST_PAD_X, CPS_WEST}, {HOST_PAD_Y, CPS_NORTH},
        {HOST_PAD_LSHOULDER, CPS_LEFT_SHOULDER}, {HOST_PAD_RSHOULDER, CPS_RIGHT_SHOULDER},
        {HOST_PAD_LSTICK, CPS_LEFT_STICK_CLICK}, {HOST_PAD_RSTICK, CPS_RIGHT_STICK_CLICK},
        {HOST_PAD_BACK, CPS_BACK}, {HOST_PAD_START, CPS_START},
        {HOST_PAD_DPAD_UP, CPS_DPAD_UP}, {HOST_PAD_DPAD_DOWN, CPS_DPAD_DOWN},
        {HOST_PAD_DPAD_LEFT, CPS_DPAD_LEFT}, {HOST_PAD_DPAD_RIGHT, CPS_DPAD_RIGHT},
    };
    uint32_t s = 0;
    const int16_t *a;
    if (!st)
        return 0;
    for (size_t i = 0; i < sizeof k_buttons / sizeof k_buttons[0]; i++)
        if (st->buttons & k_buttons[i].host)
            s |= CPS_BIT(k_buttons[i].source);
    a = st->axis;
    if (a[HOST_PAD_AXIS_LT] > trigger_threshold) s |= B(LEFT_TRIGGER);
    if (a[HOST_PAD_AXIS_RT] > trigger_threshold) s |= B(RIGHT_TRIGGER);
    if (a[HOST_PAD_AXIS_LY] < -stick_threshold) s |= B(LSTICK_UP);
    if (a[HOST_PAD_AXIS_LY] > stick_threshold) s |= B(LSTICK_DOWN);
    if (a[HOST_PAD_AXIS_LX] < -stick_threshold) s |= B(LSTICK_LEFT);
    if (a[HOST_PAD_AXIS_LX] > stick_threshold) s |= B(LSTICK_RIGHT);
    if (a[HOST_PAD_AXIS_RY] < -stick_threshold) s |= B(RSTICK_UP);
    if (a[HOST_PAD_AXIS_RY] > stick_threshold) s |= B(RSTICK_DOWN);
    if (a[HOST_PAD_AXIS_RX] < -stick_threshold) s |= B(RSTICK_LEFT);
    if (a[HOST_PAD_AXIS_RX] > stick_threshold) s |= B(RSTICK_RIGHT);
    return s;
}

void console_pad_apply(const ConsolePadMap *m, const HostPadState *st, ConsolePadOut *out)
{
    uint32_t src;
    if (!out)
        return;
    memset(out, 0, sizeof *out);
    if (!m || !st)
        return;
    src = console_pad_sources(st, m->stick_threshold, m->trigger_threshold);
    for (int t = 0; t < m->n_targets && t < CONSOLE_PAD_MAX_TARGETS; t++)
        if (src & m->source[t])
            out->buttons |= 1u << t;
    for (int i = 0; i < m->n_sticks && i < CONSOLE_PAD_MAX_STICKS; i++) {
        if (m->stick[i] == CONSOLE_STICK_LEFT) {
            out->stick_x[i] = st->axis[HOST_PAD_AXIS_LX];
            out->stick_y[i] = st->axis[HOST_PAD_AXIS_LY];
        } else if (m->stick[i] == CONSOLE_STICK_RIGHT) {
            out->stick_x[i] = st->axis[HOST_PAD_AXIS_RX];
            out->stick_y[i] = st->axis[HOST_PAD_AXIS_RY];
        }
    }
    out->host_buttons = st->buttons;
}

void console_pad_swallow(const ConsolePadMap *m, ConsolePadOut *out, uint32_t swallow)
{
    if (!m || !out || !swallow)
        return;
    out->buttons = (out->buttons & ~console_pad_targets_using(m, swallow)) | out->overlay_buttons;
}

static int deflection(int x, int y)
{
    return abs(x) + abs(y);
}

int console_pad_merge(const ConsolePadMap *m, const HostPadState *pad, const HostPadState *overlay,
                      ConsolePadOut *out)
{
    ConsolePadOut o;
    ConsolePadMap d;
    if (!out)
        return 0;
    memset(out, 0, sizeof *out);
    if (!m || (!pad && !overlay))
        return 0;
    if (pad)
        console_pad_apply(m, pad, out);
    if (overlay && console_pad_defaults(&d, m->console) == 0) {
        console_pad_apply(&d, overlay, &o);
        out->buttons |= o.buttons;
        out->overlay_buttons = o.buttons;
        for (int i = 0; i < CONSOLE_PAD_MAX_STICKS; i++)
            if (deflection(o.stick_x[i], o.stick_y[i]) > deflection(out->stick_x[i], out->stick_y[i])) {
                out->stick_x[i] = o.stick_x[i];
                out->stick_y[i] = o.stick_y[i];
            }
    }
    return 1;
}
