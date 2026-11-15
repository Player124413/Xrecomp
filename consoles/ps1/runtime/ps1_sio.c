/* SIO0: controllers and memory cards (P-9, XBA-167).
 *
 * Registers at 0x1F801040: JOY_DATA (+0), JOY_STAT (+4), JOY_MODE (+8), JOY_CTRL (+A), JOY_BAUD (+E). A byte written
 * to JOY_DATA is exchanged with the selected device (JOY_CTRL bit 1 = /JOYn asserted, bit 13 = port 2) after
 * 8 x JOY_BAUD cycles; the device's reply lands in the RX buffer and, when it pulls /ACK (more bytes expected) and
 * JOY_CTRL bit 12 is set, IRQ 7 is raised (JOY_STAT bit 9, acknowledged by JOY_CTRL bit 4).
 *
 * Devices: the digital pad (0x01 0x42: id 0x41 0x5A, two button bytes, active low), the DualShock and the 128 KB
 * memory card (0x81, then 'R' read / 'W' write / 'S' id, the standard frame protocol: 1024 frames of 128 bytes,
 * checksum = XOR of the address bytes and data).
 *
 * The DualShock (psx-spx « Controllers - Configuration Commands »): in digital mode it answers 0x42 like the digital
 * pad (L3 / R3 not reported), in analog mode id 0x73 and six bytes (buttons, right X / Y, left X / Y). 0x43 (a read
 * too) with 01h as its first parameter enters config mode: id 0xF3, every command 9 bytes, 0x43 00h leaves it.
 * Config commands: 0x44 mode (00h digital, 01h analog) + key (03h locks the ANALOG button, 02h unlocks), 0x45 status
 * (01h DualShock, 02h, LED, 02h, 01h, 00h), 0x46 / 0x47 / 0x4C constant tables by index, 0x4D the rumble mapping (the
 * old one is answered; per poll byte 00h = small motor, 01h = large, FFh = none); other 0x40..0x4F answer zeros. A
 * 0x42 poll's parameter bytes then drive the motors: small on when bit 0 is set, large 00h..FFh. */
#include "ps1_internal.h"

#include <string.h>

/* CPU cycles from the end of a byte to the device's /ACK pulse (the IRQ): drivers clear the IRQ, then wait for RX
 * ready and only then for the IRQ. */
#define ACK_DELAY 100

/* JOY_STAT bit 7 is the /ACK input level, not a latch: the device holds /ACK low for a short pulse only (psx-spx
 * « Controllers and Memory Cards »: about 2 us, some 68 CPU cycles), then releases it while the IRQ (bit 9) stays
 * until JOY_CTRL bit 4. Code that waits for /ACK high again with interrupts off (Duke Nukem - Time to Kill's libcard
 * hook in the card IRQ handler) hangs on a latch. PS1_SIO_ACK_PULSE (ps1_internal.h) CPU cycles from the IRQ. */

void ps1_card_format(uint8_t *d)
{
    memset(d, 0, PS1_CARD_SIZE);
    d[0] = 'M';
    d[1] = 'C';
    d[0x7F] = 'M' ^ 'C';
    for (int f = 1; f < 16; f++) { /* directory: 15 free blocks */
        uint8_t *p = d + f * 128;
        p[0] = 0xA0;
        p[8] = p[9] = 0xFF;
        p[0x7F] = 0xA0;
    }
    for (int f = 16; f < 36; f++) { /* broken sector list: none */
        uint8_t *p = d + f * 128;
        memset(p, 0xFF, 4);
        p[8] = p[9] = 0xFF;
        p[0x7F] = 0;
    }
    memcpy(d + 63 * 128, d, 128); /* write test frame */
}

void ps1_sio_reset(ps1_machine *m)
{
    ps1_sio *s = &m->sio;
    memset(s, 0, sizeof *s);
    s->baud = 0x88;
}

void ps1_pad_plug(ps1_pad *p, ps1_pad_kind kind)
{
    p->kind = kind;
    p->analog = kind == PS1_PAD_ANALOG;
    p->config = false;
    p->locked = false;
    memset(p->rumble_map, 0xFF, sizeof p->rumble_map);
    p->motor_small = p->motor_large = 0;
}

static bool dualshock(const ps1_pad *p)
{
    return p->kind == PS1_PAD_ANALOG || p->kind == PS1_PAD_DUALSHOCK;
}

int ps1_pad_analog(ps1_machine *m, int port)
{
    if (port < 0 || port > 1 || !dualshock(&m->pads[port]))
        return -1;
    return m->pads[port].analog;
}

int ps1_pad_analog_button(ps1_machine *m, int port)
{
    if (port < 0 || port > 1 || !dualshock(&m->pads[port]))
        return -1;
    ps1_pad *p = &m->pads[port];
    if (!p->locked)
        p->analog = !p->analog;
    return p->analog;
}

void ps1_pad_rumble(ps1_machine *m, int port, uint8_t *small, uint8_t *large)
{
    const ps1_pad *p = port >= 0 && port <= 1 ? &m->pads[port] : NULL;
    bool on = p && dualshock(p);
    if (small)
        *small = on ? p->motor_small : 0;
    if (large)
        *large = on ? p->motor_large : 0;
}

/* The pad's reply to byte tx at step (1: the command, 2: 5Ah, 3.. the parameters), and its last step. */
static uint8_t pad_exchange(ps1_sio *s, ps1_pad *p, int step, uint8_t tx, int *last)
{
    bool ds = dualshock(p);
    if (step == 1) {
        bool ok = tx == 0x42 || (ds && (tx == 0x43 || (p->config && (tx & 0xF0) == 0x40)));
        if (!ok)
            return *last = 0, 0xFF;
        s->pad_cmd = tx;
        s->pad_arg = 0;
        s->pad_long = p->config || p->analog;
        s->pad_poll = tx == 0x42 || !p->config;
        *last = s->pad_long ? 8 : 4;
        return p->config ? 0xF3 : p->analog ? 0x73 : 0x41;
    }
    *last = s->pad_long ? 8 : 4;
    if (step == 2)
        return 0x5A;
    int i = step - 3; /* parameter / data byte 0..5 */
    if (i == 0)
        s->pad_arg = tx;
    uint8_t cmd = s->pad_cmd;
    if (s->pad_poll) {
        /* a poll: buttons (active low), then the sticks */
        uint16_t b = (uint16_t)~p->buttons;
        if (!p->analog)
            b |= PS1_PAD_L3 | PS1_PAD_R3;
        if (cmd == 0x43 && i == 0 && tx == 0x01)
            p->config = true; /* from the next command on */
        if (cmd == 0x42 && ds && i < 6) {
            if (p->rumble_map[i] == 0x00)
                p->motor_small = tx & 1 ? 255 : 0;
            else if (p->rumble_map[i] == 0x01)
                p->motor_large = tx;
        }
        bool sticks = p->analog;
        switch (i) {
        case 0: return (uint8_t)b;
        case 1: return (uint8_t)(b >> 8);
        case 2: return sticks ? p->rx : 0x80;
        case 3: return sticks ? p->ry : 0x80;
        case 4: return sticks ? p->lx : 0x80;
        default: return sticks ? p->ly : 0x80;
        }
    }
    /* config mode */
    switch (cmd) {
    case 0x43:
        if (i == 0 && tx == 0x00)
            p->config = false;
        return 0x00;
    case 0x44:
        if (i == 0 && tx <= 1)
            p->analog = tx == 1;
        else if (i == 1 && (tx == 0x02 || tx == 0x03))
            p->locked = tx == 0x03;
        return 0x00;
    case 0x45: {
        const uint8_t r[6] = { 0x01, 0x02, (uint8_t)p->analog, 0x02, 0x01, 0x00 };
        return r[i];
    }
    case 0x46: {
        static const uint8_t r[2][6] = { { 0x00, 0x00, 0x01, 0x02, 0x00, 0x0A }, { 0x00, 0x00, 0x01, 0x01, 0x01, 0x14 } };
        return s->pad_arg <= 1 ? r[s->pad_arg][i] : 0x00;
    }
    case 0x47: {
        static const uint8_t r[6] = { 0x00, 0x00, 0x02, 0x00, 0x01, 0x00 };
        return s->pad_arg == 0 ? r[i] : 0x00;
    }
    case 0x4C:
        return i == 3 && s->pad_arg <= 1 ? (s->pad_arg ? 0x07 : 0x04) : 0x00;
    case 0x4D: {
        uint8_t old = p->rumble_map[i];
        p->rumble_map[i] = tx;
        if (i == 5)
            p->motor_small = p->motor_large = 0;
        return old;
    }
    default:
        return 0x00;
    }
}

static void deselect(ps1_sio *s)
{
    s->device = 0;
    s->step = 0;
}

/* The reply to byte tx from the selected device, and whether it acknowledges (expects more). */
static uint8_t exchange(ps1_machine *m, uint8_t tx, bool *ack)
{
    ps1_sio *s = &m->sio;
    int port = (s->ctrl >> 13) & 1;
    *ack = false;
    if (!(s->ctrl & 2))
        return 0xFF;
    if (s->device == 0) {
        s->step = 0;
        if (tx == 0x01 && m->pads[port].kind != PS1_PAD_NONE)
            s->device = 1;
        else if (tx == 0x81 && m->cards[port].inserted)
            s->device = 2;
        else {
            s->device = -1;
            return 0xFF;
        }
        s->step = 1;
        *ack = true;
        return 0xFF;
    }
    if (s->device < 0)
        return 0xFF;

    int step = s->step++;
    if (s->device == 1) { /* pad */
        int last = 0;
        uint8_t r = step <= 8 ? pad_exchange(s, &m->pads[port], step, tx, &last) : 0xFF;
        if (step > last) {
            s->device = -1;
            return 0xFF;
        }
        *ack = step < last;
        return r;
    }

    /* Memory card. */
    ps1_card *c = &m->cards[port];
    *ack = true;
    if (step == 1) {
        if (tx != 'R' && tx != 'W' && tx != 'S') {
            s->device = -1;
            *ack = false;
            return 0xFF;
        }
        s->cmd = tx;
        return c->flag;
    }
    if (step == 2)
        return 0x5A;
    if (step == 3)
        return 0x5D;
    if (s->cmd == 'S') {
        static const uint8_t id[] = {0x5C, 0x5D, 0x04, 0x00, 0x00, 0x80};
        if (step - 4 >= (int)sizeof id) {
            s->device = -1;
            *ack = false;
            return 0xFF;
        }
        *ack = step - 4 < (int)sizeof id - 1;
        return id[step - 4];
    }
    if (step == 4) {
        s->sector = (uint16_t)(tx << 8);
        s->chk = tx;
        return 0x00;
    }
    if (step == 5) {
        s->sector |= tx;
        s->chk ^= tx;
        return (uint8_t)(s->sector >> 8);
    }
    if (s->cmd == 'R') {
        bool bad = s->sector > 0x3FF;
        switch (step) {
        case 6: return 0x5C;
        case 7: return 0x5D;
        case 8:
            if (bad) {
                s->device = -1;
                *ack = false;
                return 0xFF;
            }
            return (uint8_t)(s->sector >> 8);
        case 9: return (uint8_t)s->sector;
        default:
            if (step < 10 + 128) {
                uint8_t v = c->data[s->sector * 128u + (uint32_t)(step - 10)];
                s->chk ^= v;
                return v;
            }
            if (step == 138)
                return s->chk;
            *ack = false;
            s->device = -1;
            return 0x47;
        }
    }
    /* 'W' */
    if (step < 6 + 128) {
        s->buf[step - 6] = tx;
        s->chk ^= tx;
        return step == 6 ? (uint8_t)s->sector : s->buf[step - 7];
    }
    switch (step) {
    case 134: /* checksum in */
        s->wr_status = tx == s->chk ? 0x47 : 0x4E;
        return s->buf[127];
    case 135: return 0x5C;
    case 136: return 0x5D;
    default: {
        uint8_t st = s->wr_status;
        if (s->sector > 0x3FF)
            st = 0xFF;
        else if (st == 0x47) {
            memcpy(c->data + s->sector * 128u, s->buf, 128);
            c->dirty = true;
            c->flag &= (uint8_t)~0x08;
        }
        *ack = false;
        s->device = -1;
        return st;
    }
    }
}

void ps1_sio_event(ps1_machine *m)
{
    ps1_sio *s = &m->sio;
    if (s->busy) { /* the byte is in: RX ready; the device pulls /ACK a little later */
        s->busy = false;
        s->rx = s->next_reply;
        s->rx_full = true;
        if (s->next_ack) {
            s->ack_pending = true;
            s->done_at = m->cycles + ACK_DELAY;
        }
        return;
    }
    s->ack_pending = false;
    s->ack = true;
    s->done_at = m->cycles + PS1_SIO_ACK_PULSE; /* /ACK low until then (not scheduled: neither busy nor pending) */
    if (s->ctrl & 0x1000) {
        s->stat_irq = 1;
        ps1_irq_raise(m, PS1_IRQ_SIO0);
    }
}

static uint32_t stat(ps1_machine *m)
{
    ps1_sio *s = &m->sio;
    uint32_t v = 0x1; /* TX ready to start */
    if (s->rx_full)
        v |= 0x2;
    if (!s->busy)
        v |= 0x4;
    if (s->ack && m->cycles < s->done_at)
        v |= 0x80;
    if (s->stat_irq)
        v |= 0x200;
    return v;
}

uint32_t ps1_sio_read(ps1_machine *m, uint32_t off, int width)
{
    ps1_sio *s = &m->sio;
    uint32_t v;
    while ((s->busy || s->ack_pending) && m->cycles >= s->done_at)
        ps1_sio_event(m);
    switch (off & ~1u) {
    case 0x0: case 0x2:
        v = s->rx_full ? s->rx : 0xFF;
        s->rx_full = false;
        if (width >= 2)
            v |= 0xFFFFFF00u; /* the empty FIFO's other bytes */
        break;
    case 0x4: v = stat(m); break;
    case 0x6: v = stat(m) >> 16; break;
    case 0x8: v = s->mode | (uint32_t)s->ctrl << 16; break;
    case 0xA: v = s->ctrl; break;
    case 0xC: v = (uint32_t)s->baud << 16; break;
    case 0xE: v = s->baud; break;
    default: v = 0; break;
    }
    if (width == 1)
        v &= 0xFF;
    else if (width == 2)
        v &= 0xFFFF;
    return v;
}

void ps1_sio_write(ps1_machine *m, uint32_t off, uint32_t v, int width)
{
    ps1_sio *s = &m->sio;
    switch (off) {
    case 0x0:
        s->tx = (uint8_t)v;
        s->busy = true;
        s->ack = false;
        s->ack_pending = false;
        /* The device sees the byte now (with the current /JOYn and port select); its reply lands at the end. */
        s->next_reply = exchange(m, s->tx, &s->next_ack);
        {
            uint32_t t = (uint32_t)(s->baud ? s->baud : 0x88) * 8u;
            s->done_at = m->cycles + (t < 64 ? 64 : t);
        }
        ps1_sched_recompute(m);
        break;
    case 0x8:
        s->mode = (uint16_t)v;
        if (width != 4)
            break;
        v >>= 16; /* a 32-bit store also writes JOY_CTRL */
        /* fall through */
    case 0xA: {
        uint16_t old = s->ctrl;
        if (v & 0x40) { /* reset */
            uint16_t baud = s->baud;
            ps1_sio_reset(m);
            s->baud = baud;
            return;
        }
        if (v & 0x10) /* acknowledge */
            s->stat_irq = 0;
        s->ctrl = (uint16_t)(v & ~0x50u);
        if (!(s->ctrl & 2) || ((old ^ s->ctrl) & 0x2000))
            deselect(s);
        break;
    }
    case 0xE:
        s->baud = (uint16_t)v;
        break;
    default:
        break;
    }
}
