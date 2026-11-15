/* console_pad (src/input/console_pad.h): see CMakeLists.txt. */
#include "console_pad.h"

#include <stdio.h>
#include <string.h>

static int g_fail, g_checks;

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        g_checks++;                                                                                                    \
        if (!(c)) {                                                                                                    \
            g_fail++;                                                                                                  \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c);                                      \
        }                                                                                                              \
    } while (0)

#define T(n) (1u << (n))

static HostPadState pad(uint32_t buttons)
{
    HostPadState st;
    memset(&st, 0, sizeof st);
    st.connected = 1;
    st.buttons = buttons;
    return st;
}

static uint32_t press(const ConsolePadMap *m, const HostPadState *st)
{
    ConsolePadOut o;
    console_pad_apply(m, st, &o);
    return o.buttons;
}

static void test_names(void)
{
    CHECK(console_pad_console("gba") == CONSOLE_PAD_GBA);
    CHECK(console_pad_console("ps1") == CONSOLE_PAD_PS1);
    CHECK(console_pad_console("n64") == CONSOLE_PAD_N64);
    CHECK(console_pad_console("xbox") == CONSOLE_PAD_NONE);
    CHECK(console_pad_console(NULL) == CONSOLE_PAD_NONE);
    CHECK(strcmp(console_pad_env(CONSOLE_PAD_GBA), "RECOMP_GBA_PAD_MAP") == 0);
    CHECK(strcmp(console_pad_env(CONSOLE_PAD_PS1), "RECOMP_PS1_PAD_MAP") == 0);
    CHECK(strcmp(console_pad_env(CONSOLE_PAD_N64), "RECOMP_N64_PAD_MAP") == 0);
    CHECK(console_pad_env(0) == NULL);
    CHECK(strcmp(console_pad_target_name(CONSOLE_PAD_PS1, PS1_T_TRIANGLE), "triangle") == 0);
    CHECK(strcmp(console_pad_target_name(CONSOLE_PAD_N64, N64_T_C_LEFT), "c_left") == 0);
    CHECK(console_pad_target_name(CONSOLE_PAD_GBA, GBA_T_COUNT) == NULL);
    CHECK(strcmp(console_pad_source_name(CPS_RSTICK_RIGHT), "right_stick_right") == 0);
    CHECK(console_pad_source_name(CPS_SOURCES) == NULL);
    ConsolePadMap m;
    CHECK(console_pad_defaults(&m, 0) == -1);
    CHECK(console_pad_defaults(&m, 99) == -1);
}

static void test_sources(void)
{
    HostPadState st = pad(HOST_PAD_A | HOST_PAD_BACK | HOST_PAD_DPAD_RIGHT | HOST_PAD_RSTICK);
    uint32_t s = console_pad_sources(&st, 10000, 16000);
    CHECK(s == (CPS_BIT(CPS_SOUTH) | CPS_BIT(CPS_BACK) | CPS_BIT(CPS_DPAD_RIGHT) | CPS_BIT(CPS_RIGHT_STICK_CLICK)));
    st = pad(0);
    st.axis[HOST_PAD_AXIS_LT] = 16000; /* at the threshold: not past it */
    st.axis[HOST_PAD_AXIS_RT] = 16001;
    st.axis[HOST_PAD_AXIS_LX] = -10001;
    st.axis[HOST_PAD_AXIS_LY] = 10000;
    st.axis[HOST_PAD_AXIS_RX] = 32767;
    st.axis[HOST_PAD_AXIS_RY] = -32768;
    s = console_pad_sources(&st, 10000, 16000);
    CHECK(s == (CPS_BIT(CPS_RIGHT_TRIGGER) | CPS_BIT(CPS_LSTICK_LEFT) | CPS_BIT(CPS_RSTICK_RIGHT)
                | CPS_BIT(CPS_RSTICK_UP)));
    CHECK(console_pad_sources(NULL, 0, 0) == 0);
}

/* The defaults are the runtimes' fixed mappings from before XBA-158 (gba_runtime_pad_keys and friends). */
static void test_gba_defaults(void)
{
    ConsolePadMap m;
    HostPadState st;
    CHECK(console_pad_defaults(&m, CONSOLE_PAD_GBA) == 0);
    CHECK(m.n_targets == GBA_T_COUNT && m.n_sticks == 0);
    st = pad(HOST_PAD_A);
    CHECK(press(&m, &st) == T(GBA_T_A));
    st = pad(HOST_PAD_Y);
    CHECK(press(&m, &st) == T(GBA_T_A));
    st = pad(HOST_PAD_B | HOST_PAD_X);
    CHECK(press(&m, &st) == T(GBA_T_B));
    st = pad(HOST_PAD_LSHOULDER | HOST_PAD_START | HOST_PAD_BACK);
    CHECK(press(&m, &st) == (T(GBA_T_L) | T(GBA_T_START) | T(GBA_T_SELECT)));
    st = pad(0);
    st.axis[HOST_PAD_AXIS_RT] = 20000;
    st.axis[HOST_PAD_AXIS_LT] = 16384; /* half: not past it */
    CHECK(press(&m, &st) == T(GBA_T_R));
    st = pad(HOST_PAD_DPAD_UP);
    st.axis[HOST_PAD_AXIS_LX] = 11000; /* past a third */
    CHECK(press(&m, &st) == (T(GBA_T_UP) | T(GBA_T_RIGHT)));
    st = pad(0);
    st.axis[HOST_PAD_AXIS_LY] = -10922; /* a third exactly: no */
    CHECK(press(&m, &st) == 0);
    st = pad(HOST_PAD_LSTICK | HOST_PAD_RSTICK | HOST_PAD_GUIDE);
    CHECK(press(&m, &st) == 0);
    st = pad(0);
    st.axis[HOST_PAD_AXIS_RX] = 32767; /* the right stick does nothing */
    CHECK(press(&m, &st) == 0);
}

static void test_ps1_defaults(void)
{
    ConsolePadMap m;
    HostPadState st;
    ConsolePadOut o;
    CHECK(console_pad_defaults(&m, CONSOLE_PAD_PS1) == 0);
    CHECK(m.n_targets == PS1_T_COUNT && m.n_sticks == 2);
    st = pad(HOST_PAD_A | HOST_PAD_B | HOST_PAD_X | HOST_PAD_Y);
    CHECK(press(&m, &st) == (T(PS1_T_CROSS) | T(PS1_T_CIRCLE) | T(PS1_T_SQUARE) | T(PS1_T_TRIANGLE)));
    st = pad(HOST_PAD_LSHOULDER | HOST_PAD_RSHOULDER | HOST_PAD_LSTICK | HOST_PAD_RSTICK);
    CHECK(press(&m, &st) == (T(PS1_T_L1) | T(PS1_T_R1) | T(PS1_T_L3) | T(PS1_T_R3)));
    st = pad(HOST_PAD_START | HOST_PAD_BACK | HOST_PAD_DPAD_DOWN);
    st.axis[HOST_PAD_AXIS_LT] = 30000;
    st.axis[HOST_PAD_AXIS_RT] = 30000;
    CHECK(press(&m, &st) == (T(PS1_T_START) | T(PS1_T_SELECT) | T(PS1_T_DOWN) | T(PS1_T_L2) | T(PS1_T_R2)));
    /* The left stick is the DualShock's stick, not a D-pad (the runtime adds that in digital mode). */
    st = pad(HOST_PAD_BACK | HOST_PAD_LSTICK);
    st.axis[HOST_PAD_AXIS_LX] = -32768;
    st.axis[HOST_PAD_AXIS_LY] = 1234;
    st.axis[HOST_PAD_AXIS_RX] = 500;
    st.axis[HOST_PAD_AXIS_RY] = 32767;
    console_pad_apply(&m, &st, &o);
    CHECK(o.buttons == (T(PS1_T_SELECT) | T(PS1_T_L3)));
    CHECK(o.stick_x[0] == -32768 && o.stick_y[0] == 1234);
    CHECK(o.stick_x[1] == 500 && o.stick_y[1] == 32767);
    CHECK(o.host_buttons == (HOST_PAD_BACK | HOST_PAD_LSTICK));
}

static void test_n64_defaults(void)
{
    ConsolePadMap m;
    HostPadState st;
    ConsolePadOut o;
    CHECK(console_pad_defaults(&m, CONSOLE_PAD_N64) == 0);
    CHECK(m.n_targets == N64_T_COUNT && m.n_sticks == 1 && m.range == -1 && m.deadzone == -1);
    st = pad(HOST_PAD_A);
    CHECK(press(&m, &st) == T(N64_T_A));
    st = pad(HOST_PAD_X);
    CHECK(press(&m, &st) == T(N64_T_B));
    st = pad(HOST_PAD_Y);
    CHECK(press(&m, &st) == T(N64_T_C_UP));
    st = pad(HOST_PAD_START | HOST_PAD_BACK); /* Back: nothing on an N64 */
    CHECK(press(&m, &st) == T(N64_T_START));
    st = pad(0);
    st.axis[HOST_PAD_AXIS_LT] = 8193;
    CHECK(press(&m, &st) == T(N64_T_Z));
    st.axis[HOST_PAD_AXIS_LT] = 8192;
    CHECK(press(&m, &st) == 0);
    st = pad(0);
    st.axis[HOST_PAD_AXIS_RY] = 30000;
    st.axis[HOST_PAD_AXIS_RX] = -30000;
    CHECK(press(&m, &st) == (T(N64_T_C_DOWN) | T(N64_T_C_LEFT)));
    st.axis[HOST_PAD_AXIS_RY] = 16384; /* half: not past it */
    st.axis[HOST_PAD_AXIS_RX] = 0;
    CHECK(press(&m, &st) == 0);
    st = pad(0);
    st.axis[HOST_PAD_AXIS_LX] = 20000;
    st.axis[HOST_PAD_AXIS_LY] = -20000; /* the left stick: analog only, never a button */
    console_pad_apply(&m, &st, &o);
    CHECK(o.buttons == 0 && o.stick_x[0] == 20000 && o.stick_y[0] == -20000);
}

static void test_parse_format(void)
{
    ConsolePadMap m;
    char buf[512];

    console_pad_defaults(&m, CONSOLE_PAD_GBA);
    CHECK(strcmp(console_pad_format(&m, buf, sizeof buf), "") == 0);
    CHECK(console_pad_parse(&m, NULL) == 0);
    CHECK(console_pad_parse(&m, "") == 0);
    CHECK(console_pad_parse(&m, "a=east, B=South;l=left_trigger  select=none") == 0);
    CHECK(m.source[GBA_T_A] == CPS_BIT(CPS_EAST));
    CHECK(m.source[GBA_T_B] == CPS_BIT(CPS_SOUTH));
    CHECK(m.source[GBA_T_L] == CPS_BIT(CPS_LEFT_TRIGGER));
    CHECK(m.source[GBA_T_SELECT] == 0);
    CHECK(strcmp(console_pad_format(&m, buf, sizeof buf), "a=east,b=south,l=left_trigger,select=none") == 0);
    HostPadState st = pad(HOST_PAD_A | HOST_PAD_BACK);
    CHECK(press(&m, &st) == T(GBA_T_B)); /* remapped: south is B now, Select is gone */

    /* Several sources, and the default's own set written back. */
    console_pad_defaults(&m, CONSOLE_PAD_GBA);
    CHECK(console_pad_parse(&m, "start=start+north,a=south") == 0);
    CHECK(m.source[GBA_T_START] == (CPS_BIT(CPS_START) | CPS_BIT(CPS_NORTH)));
    CHECK(strcmp(console_pad_format(&m, buf, sizeof buf), "a=south,start=north+start") == 0);
    CHECK(console_pad_parse(&m, "a=south+north") == 0); /* back to the default: not written */
    CHECK(strcmp(console_pad_format(&m, buf, sizeof buf), "start=north+start") == 0);

    /* Rejects: unknown target, source, value, no '=', a PS1 target on a GBA, a dangling '+'. Good entries stay. */
    console_pad_defaults(&m, CONSOLE_PAD_GBA);
    CHECK(console_pad_parse(&m, "x=south,a=nope,cross=south,a,b=east+,r=north,stick=left,range=80") == 7);
    CHECK(m.source[GBA_T_R] == CPS_BIT(CPS_NORTH));
    CHECK(strcmp(console_pad_format(&m, buf, sizeof buf), "r=north") == 0);

    /* PS1 sticks. */
    console_pad_defaults(&m, CONSOLE_PAD_PS1);
    CHECK(console_pad_parse(&m, "left_stick=right,right_stick=none,cross=east,circle=south") == 0);
    CHECK(m.stick[0] == CONSOLE_STICK_RIGHT && m.stick[1] == CONSOLE_STICK_NONE);
    CHECK(strcmp(console_pad_format(&m, buf, sizeof buf), "cross=east,circle=south,left_stick=right,right_stick=none")
          == 0);
    st = pad(0);
    st.axis[HOST_PAD_AXIS_LX] = 111;
    st.axis[HOST_PAD_AXIS_RX] = 222;
    st.axis[HOST_PAD_AXIS_RY] = -333;
    ConsolePadOut o;
    console_pad_apply(&m, &st, &o);
    CHECK(o.stick_x[0] == 222 && o.stick_y[0] == -333 && o.stick_x[1] == 0 && o.stick_y[1] == 0);
    CHECK(console_pad_parse(&m, "left_stick=up,stick=left") == 2);

    /* N64 stick, range and dead zone. */
    console_pad_defaults(&m, CONSOLE_PAD_N64);
    CHECK(console_pad_parse(&m, "stick=right,range=96,deadzone=12,z=left_shoulder,l=left_trigger") == 0);
    CHECK(m.stick[0] == CONSOLE_STICK_RIGHT && m.range == 96 && m.deadzone == 12);
    CHECK(strcmp(console_pad_format(&m, buf, sizeof buf), "z=left_shoulder,l=left_trigger,stick=right,range=96,deadzone=12")
          == 0);
    CHECK(console_pad_parse(&m, "range=0,range=128,deadzone=91,deadzone=x,range=") == 5);
    CHECK(m.range == 96 && m.deadzone == 12);

    /* Round trip: the format parses back to the same map. */
    ConsolePadMap back;
    console_pad_defaults(&back, CONSOLE_PAD_N64);
    CHECK(console_pad_parse(&back, buf) == 0);
    CHECK(memcmp(&back, &m, sizeof m) == 0);

    /* A small buffer keeps whole entries. */
    char small[20];
    console_pad_format(&m, small, sizeof small);
    CHECK(strcmp(small, "z=left_shoulder") == 0);

    /* load: defaults first, then the text. */
    CHECK(console_pad_load(&m, CONSOLE_PAD_N64, "a=east,bogus") == 1);
    CHECK(m.source[N64_T_A] == CPS_BIT(CPS_EAST) && m.stick[0] == CONSOLE_STICK_LEFT && m.range == -1);
    CHECK(console_pad_load(&m, CONSOLE_PAD_GBA, NULL) == 0);
    CHECK(console_pad_load(&m, 0, "a=b") == -1);
}

static void test_merge(void)
{
    ConsolePadMap m;
    ConsolePadOut o;
    HostPadState p = pad(HOST_PAD_A), ov = pad(HOST_PAD_B);

    console_pad_defaults(&m, CONSOLE_PAD_PS1);
    console_pad_parse(&m, "cross=east,circle=south");
    CHECK(console_pad_merge(&m, NULL, NULL, &o) == 0 && o.buttons == 0);
    /* The pad goes through the player's mapping, the overlay through the defaults (its Circle stays Circle). */
    CHECK(console_pad_merge(&m, &p, NULL, &o) == 1 && o.buttons == T(PS1_T_CIRCLE));
    CHECK(console_pad_merge(&m, NULL, &ov, &o) == 1 && o.buttons == T(PS1_T_CIRCLE));
    ov = pad(HOST_PAD_A);
    CHECK(console_pad_merge(&m, &p, &ov, &o) == 1 && o.buttons == (T(PS1_T_CIRCLE) | T(PS1_T_CROSS)));
    /* Sticks: whichever is pushed further. */
    p.axis[HOST_PAD_AXIS_LX] = 1000;
    ov.axis[HOST_PAD_AXIS_LX] = -20000;
    ov.axis[HOST_PAD_AXIS_RY] = 50;
    p.axis[HOST_PAD_AXIS_RY] = 3000;
    console_pad_merge(&m, &p, &ov, &o);
    CHECK(o.stick_x[0] == -20000 && o.stick_x[1] == 0 && o.stick_y[1] == 3000);
    CHECK(o.host_buttons == HOST_PAD_A); /* the physical pad's raw buttons only */
    /* A remapped stick on the pad does not move the overlay's. */
    console_pad_parse(&m, "left_stick=none");
    p = pad(0);
    p.axis[HOST_PAD_AXIS_LX] = 30000;
    ov = pad(0);
    ov.axis[HOST_PAD_AXIS_LX] = 5000;
    console_pad_merge(&m, &p, &ov, &o);
    CHECK(o.stick_x[0] == 5000);
}

/* XBA-212: a hotkey chord held on the physical pad hides its buttons from the game, never the on-screen pad's. */
static void test_swallow(void)
{
    ConsolePadMap m;
    ConsolePadOut o;
    console_pad_defaults(&m, CONSOLE_PAD_PS1);
    /* The pad holds Back + R1 (a chord), the overlay holds R1 (white) and Cross. */
    HostPadState p = pad(HOST_PAD_BACK), ov = pad(HOST_PAD_A);
    p.buttons |= HOST_PAD_RSHOULDER;
    ov.buttons |= HOST_PAD_RSHOULDER;
    console_pad_merge(&m, &p, &ov, &o);
    CHECK(o.overlay_buttons == (T(PS1_T_CROSS) | T(PS1_T_R1)));
    uint32_t chord = CPS_BIT(CPS_BACK) | CPS_BIT(CPS_RIGHT_SHOULDER);
    console_pad_swallow(&m, &o, chord);
    CHECK(o.buttons == (T(PS1_T_CROSS) | T(PS1_T_R1))); /* the overlay's R1 and Cross stay, the pad's Select goes */
    /* Without the overlay, the chord's buttons are hidden. */
    console_pad_merge(&m, &p, NULL, &o);
    CHECK(o.overlay_buttons == 0);
    console_pad_swallow(&m, &o, chord);
    CHECK(o.buttons == 0);
    /* Nothing swallowed: nothing changes. */
    console_pad_merge(&m, &p, &ov, &o);
    uint32_t before = o.buttons;
    console_pad_swallow(&m, &o, 0);
    CHECK(o.buttons == before);
}

int main(void)
{
    test_names();
    test_sources();
    test_gba_defaults();
    test_ps1_defaults();
    test_n64_defaults();
    test_parse_format();
    test_merge();
    test_swallow();
    printf("console_pad: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
