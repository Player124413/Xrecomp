/* The DualShock's configuration commands on SIO0 (XBA-167, ps1_sio.c): scripted exchanges through the JOY_* registers,
 * each byte's reply and /ACK checked against psx-spx « Controllers - Configuration Commands »: polls in digital and
 * analog mode, 0x43 enter / exit config, 0x44 mode + lock, 0x45 status, the 0x46 / 0x47 / 0x4C tables, 0x4D rumble
 * mapping and the motors a poll then drives, the ANALOG button, and the digital pad that has none of it. The "BIOS"
 * is a branch to itself (no BIOS image needed). Our own code. */
#include "ps1_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        if (!(cond)) {                                                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                               \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
            failures++;                                                                                               \
        }                                                                                                             \
    } while (0)

static uint8_t *g_bios;

static ps1_machine *machine(void)
{
    char err[128];
    ps1_machine *m = ps1_create(g_bios, PS1_BIOS_SIZE, NULL, NULL, 0, err, sizeof err);
    if (!m) {
        printf("FAIL: ps1_create: %s\n", err);
        exit(1);
    }
    return m;
}

/* One byte through JOY_DATA; *ack: the device pulled /ACK (the IRQ, JOY_STAT bit 9). */
static uint8_t xfer(ps1_machine *m, uint8_t tx, int *ack)
{
    ps1_io_write(m, 0x1F801040u, tx, 1);
    ps1_run_cycles(m, 2000);
    *ack = (ps1_io_read(m, 0x1F801044u, 4) & 0x200) != 0;
    ps1_io_write(m, 0x1F80104Au, ps1_io_read(m, 0x1F80104Au, 2) | 0x10, 2);
    return (uint8_t)ps1_io_read(m, 0x1F801040u, 1);
}

/* A whole command to port 0: n bytes sent, the replies into rx, /ACK on every byte but the last (which ends it: the
 * pad stops acknowledging). Returns how many bytes the pad acknowledged + 1 (the transfer's length as the pad sees
 * it). */
static int command(ps1_machine *m, const uint8_t *tx, int n, uint8_t *rx)
{
    int acked = 0, ack = 0;
    ps1_io_write(m, 0x1F80104Au, 0x1003, 2); /* TX enable, /JOY1, ACK IRQ */
    for (int i = 0; i < n; i++) {
        rx[i] = xfer(m, tx[i], &ack);
        if (!ack)
            break;
        acked++;
    }
    ps1_io_write(m, 0x1F80104Au, 0, 2); /* /JOY1 high: deselect */
    return acked + 1;
}

/* Sends the command and checks every reply byte against want (n bytes, the length the pad must answer). */
static void expect(ps1_machine *m, const char *what, const uint8_t *tx, const uint8_t *want, int n)
{
    uint8_t buf[9];
    uint8_t rx[9];
    memset(rx, 0xEE, sizeof rx);
    memset(buf, 0, sizeof buf);
    memcpy(buf, tx, (size_t)n);
    int len = command(m, buf, 9, rx);
    CHECK(len == n, "%s: %d bytes, want %d", what, len, n);
    for (int i = 1; i < n && i < len; i++)
        CHECK(rx[i] == want[i], "%s: byte %d %02X, want %02X", what, i, rx[i], want[i]);
}

#define EXPECT(m, what, txs, wants)                                                                                    \
    do {                                                                                                              \
        static const uint8_t t_[] = txs, w_[] = wants;                                                                \
        _Static_assert(sizeof t_ == sizeof w_, "tx and reply lengths");                                               \
        expect(m, what, t_, w_, (int)sizeof w_);                                                                      \
    } while (0)
#define B(...) { __VA_ARGS__ }

static void test_dualshock(void)
{
    ps1_machine *m = machine();
    /* CROSS + UP + L3 held, left stick right, right stick up */
    uint16_t held = PS1_PAD_CROSS | PS1_PAD_UP | PS1_PAD_L3;
    ps1_set_pad(m, 0, PS1_PAD_DUALSHOCK, held, 0xFF, 0x80, 0x80, 0x00);
    CHECK(ps1_pad_analog(m, 0) == 0, "plugged in digital");

    /* Digital mode: the digital pad's answer, L3 not reported (0xEF 0xBF as SCPH-1080). */
    EXPECT(m, "poll (digital)", B(0x01, 0x42, 0x00, 0x00, 0x00), B(0xFF, 0x41, 0x5A, 0xEF, 0xBF));
    /* 0x43 in digital mode is a poll too; 01h enters config mode. */
    EXPECT(m, "enter config", B(0x01, 0x43, 0x00, 0x01, 0x00), B(0xFF, 0x41, 0x5A, 0xEF, 0xBF));
    /* Config mode: id F3, 9 bytes for every command. */
    EXPECT(m, "status (digital)", B(0x01, 0x45, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
           B(0xFF, 0xF3, 0x5A, 0x01, 0x02, 0x00, 0x02, 0x01, 0x00));
    EXPECT(m, "table 46h index 0", B(0x01, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
           B(0xFF, 0xF3, 0x5A, 0x00, 0x00, 0x01, 0x02, 0x00, 0x0A));
    EXPECT(m, "table 46h index 1", B(0x01, 0x46, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00),
           B(0xFF, 0xF3, 0x5A, 0x00, 0x00, 0x01, 0x01, 0x01, 0x14));
    EXPECT(m, "table 47h", B(0x01, 0x47, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
           B(0xFF, 0xF3, 0x5A, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00));
    EXPECT(m, "table 4Ch index 0", B(0x01, 0x4C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
           B(0xFF, 0xF3, 0x5A, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00));
    EXPECT(m, "table 4Ch index 1", B(0x01, 0x4C, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00),
           B(0xFF, 0xF3, 0x5A, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00));
    /* 0x44: analog on, locked. */
    EXPECT(m, "set analog + lock", B(0x01, 0x44, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00),
           B(0xFF, 0xF3, 0x5A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00));
    EXPECT(m, "status (analog)", B(0x01, 0x45, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
           B(0xFF, 0xF3, 0x5A, 0x01, 0x02, 0x01, 0x02, 0x01, 0x00));
    /* 0x4D: poll byte 0 drives the small motor, byte 1 the large one; the old mapping (none) is answered. */
    EXPECT(m, "rumble mapping", B(0x01, 0x4D, 0x00, 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF),
           B(0xFF, 0xF3, 0x5A, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF));
    EXPECT(m, "rumble mapping again", B(0x01, 0x4D, 0x00, 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF),
           B(0xFF, 0xF3, 0x5A, 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF));
    /* 0x42 in config mode: id F3, the pad's state (analog now). */
    EXPECT(m, "poll (config)", B(0x01, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
           B(0xFF, 0xF3, 0x5A, 0xED, 0xBF, 0x80, 0x00, 0xFF, 0x80));
    EXPECT(m, "unknown 40h", B(0x01, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
           B(0xFF, 0xF3, 0x5A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00));
    EXPECT(m, "exit config", B(0x01, 0x43, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
           B(0xFF, 0xF3, 0x5A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00));
    CHECK(ps1_pad_analog(m, 0) == 1, "analog after 44h");

    /* Analog mode: id 73, buttons with L3 (0xED), right X / Y, left X / Y. The poll's bytes drive the motors. */
    uint8_t small = 9, large = 9;
    ps1_pad_rumble(m, 0, &small, &large);
    CHECK(small == 0 && large == 0, "motors off after mapping: %u %u", small, large);
    EXPECT(m, "poll (analog) + rumble", B(0x01, 0x42, 0x00, 0x01, 0xC0, 0x00, 0x00, 0x00, 0x00),
           B(0xFF, 0x73, 0x5A, 0xED, 0xBF, 0x80, 0x00, 0xFF, 0x80));
    ps1_pad_rumble(m, 0, &small, &large);
    CHECK(small == 255 && large == 0xC0, "motors %u %u", small, large);
    EXPECT(m, "poll, motors off", B(0x01, 0x42, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00),
           B(0xFF, 0x73, 0x5A, 0xED, 0xBF, 0x80, 0x00, 0xFF, 0x80));
    ps1_pad_rumble(m, 0, &small, &large);
    CHECK(small == 0 && large == 0, "small motor: bit 0 only (%u %u)", small, large);

    /* Locked: the ANALOG button does nothing; the same kind every frame keeps the mode. */
    CHECK(ps1_pad_analog_button(m, 0) == 1, "locked analog");
    ps1_set_pad(m, 0, PS1_PAD_DUALSHOCK, 0, 0x80, 0x80, 0x80, 0x80);
    CHECK(ps1_pad_analog(m, 0) == 1, "kept by ps1_set_pad");

    /* Unlock (key 02h) and back to digital through config mode; then the button works. */
    EXPECT(m, "enter config (analog)", B(0x01, 0x43, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00),
           B(0xFF, 0x73, 0x5A, 0xFF, 0xFF, 0x80, 0x80, 0x80, 0x80));
    EXPECT(m, "set digital + unlock", B(0x01, 0x44, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00),
           B(0xFF, 0xF3, 0x5A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00));
    EXPECT(m, "exit config (digital)", B(0x01, 0x43, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
           B(0xFF, 0xF3, 0x5A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00));
    EXPECT(m, "poll (digital again)", B(0x01, 0x42, 0x00, 0x00, 0x00), B(0xFF, 0x41, 0x5A, 0xFF, 0xFF));
    CHECK(ps1_pad_analog_button(m, 0) == 1, "ANALOG button: analog");
    EXPECT(m, "poll after the button", B(0x01, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
           B(0xFF, 0x73, 0x5A, 0xFF, 0xFF, 0x80, 0x80, 0x80, 0x80));

    /* Config commands outside config mode are not answered. */
    EXPECT(m, "45h outside config", B(0x01, 0x45), B(0xFF, 0xFF));

    /* Another kind is a new pad: analog from the kind, mapping gone. */
    ps1_set_pad(m, 0, PS1_PAD_ANALOG, 0, 0x80, 0x80, 0x80, 0x80);
    CHECK(ps1_pad_analog(m, 0) == 1, "plugged in analog");
    ps1_set_pad(m, 0, PS1_PAD_DUALSHOCK, 0, 0x80, 0x80, 0x80, 0x80);
    CHECK(ps1_pad_analog(m, 0) == 0, "plugged in digital again");
    EXPECT(m, "enter config (new pad)", B(0x01, 0x43, 0x00, 0x01, 0x00), B(0xFF, 0x41, 0x5A, 0xFF, 0xFF));
    EXPECT(m, "mapping reset", B(0x01, 0x4D, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF),
           B(0xFF, 0xF3, 0x5A, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF));
    ps1_destroy(m);
}

static void test_digital_pad(void)
{
    ps1_machine *m = machine();
    ps1_set_pad(m, 0, PS1_PAD_DIGITAL, PS1_PAD_START, 0x80, 0x80, 0x80, 0x80);
    EXPECT(m, "SCPH-1080 poll", B(0x01, 0x42, 0x00, 0x00, 0x00), B(0xFF, 0x41, 0x5A, 0xF7, 0xFF));
    /* No config mode: 0x43 is not answered, and the pad stays digital. */
    EXPECT(m, "SCPH-1080 43h", B(0x01, 0x43), B(0xFF, 0xFF));
    CHECK(ps1_pad_analog(m, 0) == -1 && ps1_pad_analog_button(m, 0) == -1, "no DualShock");
    uint8_t small = 1, large = 1;
    ps1_pad_rumble(m, 0, &small, &large);
    CHECK(small == 0 && large == 0, "no motors");
    ps1_destroy(m);
}

/* JOY_STAT bit 7 is the /ACK level: a pulse of PS1_SIO_ACK_PULSE cycles from the IRQ, not a latch (code that waits for
 * it to drop with interrupts off, like Duke Nukem - Time to Kill's libcard, hangs on a latch); bit 9 stays until
 * JOY_CTRL bit 4. One byte sent, then cycles in small steps until IRQ 7 arrives. */
static void ack_pulse(ps1_machine *m, const char *what, uint8_t tx)
{
    m->i_stat &= ~(1u << PS1_IRQ_SIO0);
    ps1_io_write(m, 0x1F801040u, tx, 1);
    int n = 0;
    while (!(m->i_stat & (1u << PS1_IRQ_SIO0)) && n++ < 1000)
        ps1_run_cycles(m, 4);
    CHECK(m->i_stat & (1u << PS1_IRQ_SIO0), "%s: no IRQ 7", what);
    uint32_t st = ps1_io_read(m, 0x1F801044u, 4);
    CHECK(st & 0x80, "%s: JOY_STAT %03X at the IRQ, want bit 7 (/ACK low)", what, st);
    CHECK(st & 0x200, "%s: JOY_STAT %03X at the IRQ, want bit 9", what, st);
    ps1_run_cycles(m, PS1_SIO_ACK_PULSE + 8);
    st = ps1_io_read(m, 0x1F801044u, 4);
    CHECK(!(st & 0x80), "%s: JOY_STAT %03X after the pulse, want bit 7 clear", what, st);
    CHECK(st & 0x200, "%s: JOY_STAT %03X after the pulse, want bit 9 until acknowledged", what, st);
    ps1_io_write(m, 0x1F80104Au, ps1_io_read(m, 0x1F80104Au, 2) | 0x10, 2);
    st = ps1_io_read(m, 0x1F801044u, 4);
    CHECK(!(st & 0x200), "%s: JOY_STAT %03X after JOY_CTRL bit 4, want bit 9 clear", what, st);
    (void)ps1_io_read(m, 0x1F801040u, 1);
    m->i_stat &= ~(1u << PS1_IRQ_SIO0);
}

static void test_ack_pulse(void)
{
    ps1_machine *m = machine();
    ps1_set_pad(m, 0, PS1_PAD_DIGITAL, 0, 0x80, 0x80, 0x80, 0x80);
    ps1_io_write(m, 0x1F80104Au, 0x1003, 2); /* TX enable, /JOY1, ACK IRQ */
    ack_pulse(m, "pad 01h", 0x01);
    ack_pulse(m, "pad 42h", 0x42);
    ps1_io_write(m, 0x1F80104Au, 0, 2);
    ps1_io_write(m, 0x1F80104Au, 0x1003, 2); /* the memory card in slot 1 */
    ack_pulse(m, "card 81h", 0x81);
    ack_pulse(m, "card 'R'", 'R');
    ps1_io_write(m, 0x1F80104Au, 0, 2);
    ps1_destroy(m);
}

int main(void)
{
    g_bios = calloc(1, PS1_BIOS_SIZE);
    if (!g_bios)
        return 1;
    g_bios[0] = 0xFF; /* reset vector: b . ; nop */
    g_bios[1] = 0xFF;
    g_bios[2] = 0x00;
    g_bios[3] = 0x10;
    test_dualshock();
    test_digital_pad();
    test_ack_pulse();
    free(g_bios);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ps1 pad: ok\n");
    return 0;
}
