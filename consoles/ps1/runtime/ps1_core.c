/* PS1 machine: creation, boot (LLE through the BIOS, fast boot of a PS-X EXE), frame loop, public accessors
 * (XBA-162 / XBA-163 / XBA-167). See ps1_core.h. */
#include "ps1_hle.h"
#include "ps1_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_err(char *err, size_t len, const char *msg)
{
    if (err && len)
        snprintf(err, len, "%s", msg);
}

static void sink_raise(void *ctx, int irq)
{
    ps1_irq_raise((ps1_machine *)ctx, irq);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Checks a PS-X EXE header: magic, text inside main RAM and inside the file. */
static int exe_valid(const uint8_t *exe, size_t size)
{
    if (size < 0x800 || memcmp(exe, "PS-X EXE", 8) != 0)
        return 0;
    uint32_t t_addr = le32(exe + 0x18) & 0x1FFFFFFFu, t_size = le32(exe + 0x1C);
    if (t_addr >= PS1_RAM_SIZE * 4 || t_size > PS1_RAM_SIZE)
        return 0;
    return 1;
}

/* The BIOS is about to enter its shell (0x80030000): the EXE takes over, as the shell would have run it. */
void ps1_boot_exe(ps1_machine *m)
{
    const uint8_t *e = m->exe;
    uint32_t pc0 = le32(e + 0x10), gp0 = le32(e + 0x14);
    uint32_t t_addr = le32(e + 0x18), t_size = le32(e + 0x1C);
    uint32_t b_addr = le32(e + 0x28), b_size = le32(e + 0x2C);
    uint32_t s_addr = le32(e + 0x30), s_size = le32(e + 0x34);
    size_t avail = m->exe_size - 0x800;
    if (t_size > avail)
        t_size = (uint32_t)avail;
    for (uint32_t i = 0; i < t_size; i++)
        m->ram[(t_addr + i) & (PS1_RAM_SIZE - 1)] = e[0x800 + i];
    for (uint32_t i = 0; i < b_size && i < PS1_RAM_SIZE; i++)
        m->ram[(b_addr + i) & (PS1_RAM_SIZE - 1)] = 0;
    for (uint32_t i = 0; i < PS1_RAM_PAGES; i++)
        m->ram_gen[i]++; /* new code and data everywhere, as far as lifted code is concerned */
    m->r[28] = gp0;
    if (s_addr) {
        m->r[29] = s_addr + s_size;
        m->r[30] = s_addr + s_size;
    }
    m->r[4] = 0; /* argc */
    m->r[5] = 0; /* argv */
    m->pc = pc0;
    m->next_pc = pc0 + 4;
    m->next_delay = false;
    m->exe_started = true;
}

ps1_machine *ps1_create(const uint8_t *bios, size_t bios_size, const char *disc_path, const uint8_t *exe,
                        size_t exe_size, char *err, size_t errlen)
{
    if (bios && bios_size != PS1_BIOS_SIZE) {
        set_err(err, errlen, "the BIOS image must be 512 KB");
        return NULL;
    }
    if (exe && !exe_valid(exe, exe_size)) {
        set_err(err, errlen, "not a PS-X EXE");
        return NULL;
    }
    ps1_machine *m = calloc(1, sizeof *m);
    if (!m) {
        set_err(err, errlen, "out of memory");
        return NULL;
    }
    m->ram = calloc(1, PS1_RAM_SIZE);
    m->bios = malloc(PS1_BIOS_SIZE);
    m->ram_gen = calloc(PS1_RAM_PAGES, sizeof *m->ram_gen);
    m->rc_scratch = m->scratch;
    if (exe) {
        m->exe = malloc(exe_size);
        m->exe_size = exe_size;
    }
    if (!m->ram || !m->ram_gen || !m->bios || (exe && !m->exe) || (!bios && ps1_hle_create(m) != 0)) {
        set_err(err, errlen, "out of memory");
        ps1_destroy(m);
        return NULL;
    }
    if (bios)
        memcpy(m->bios, bios, PS1_BIOS_SIZE);
    else
        memset(m->bios, 0, PS1_BIOS_SIZE); /* HLE: no ROM code; the BIOS region reads zeros */
    if (exe)
        memcpy(m->exe, exe, exe_size);
    m->exe_started = exe == NULL;

    ps1_irq_sink sink = {sink_raise, m};
    if (disc_path) {
        char derr[256] = "";
        m->disc = ps1_disc_open(disc_path, derr, sizeof derr);
        if (!m->disc) {
            set_err(err, errlen, derr[0] ? derr : "cannot open the disc image");
            ps1_destroy(m);
            return NULL;
        }
    }
    m->gpu = ps1_gpu_create(sink);
    m->spu = ps1_spu_create(sink);
    m->cdrom = ps1_cdrom_create(sink, m->disc);
    m->mdec = ps1_mdec_create();
    if (!m->gpu || !m->spu || !m->cdrom || !m->mdec) {
        set_err(err, errlen, "cannot create the devices");
        ps1_destroy(m);
        return NULL;
    }

    ps1_cpu_reset(m);
    ps1_gte_reset(&m->gte);
    ps1_sio_reset(m);
    m->ram_size = 0x00000B88u;
    m->dpcr = 0x07654321u;
    for (int i = 0; i < 3; i++)
        m->tmr[i].mode = 0x400;
    ps1_pad_plug(&m->pads[0], PS1_PAD_DIGITAL);
    ps1_pad_plug(&m->pads[1], PS1_PAD_NONE);
    for (int i = 0; i < 2; i++) {
        m->pads[i].lx = m->pads[i].ly = m->pads[i].rx = m->pads[i].ry = 0x80;
        ps1_card_format(m->cards[i].data);
        m->cards[i].inserted = true;
        m->cards[i].flag = 0x08;
    }
    ps1_sched_init(m);
    if (m->hle)
        ps1_hle_boot(m); /* kernel RAM set up, the EXE (or the disc's boot file) started at once */
    return m;
}

void ps1_destroy(ps1_machine *m)
{
    if (!m)
        return;
    if (m->gpu)
        ps1_gpu_destroy(m->gpu);
    if (m->cdrom)
        ps1_cdrom_destroy(m->cdrom);
    if (m->spu)
        ps1_spu_destroy(m->spu);
    if (m->mdec)
        ps1_mdec_destroy(m->mdec);
    if (m->disc)
        ps1_disc_close(m->disc);
    ps1_recomp_free(m);
    ps1_cpu_free(m);
    ps1_hle_destroy(m);
    free(m->ram);
    free(m->ram_gen);
    free(m->bios);
    free(m->exe);
    free(m);
}

void ps1_set_pad(ps1_machine *m, int port, ps1_pad_kind kind, uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx,
                 uint8_t ry)
{
    if (port < 0 || port > 1)
        return;
    ps1_pad *p = &m->pads[port];
    if (p->kind != kind)
        ps1_pad_plug(p, kind);
    p->buttons = buttons;
    p->lx = lx;
    p->ly = ly;
    p->rx = rx;
    p->ry = ry;
}

void ps1_run_frame(ps1_machine *m)
{
    m->frame_done = false;
    while (!m->frame_done) {
        ps1_cpu_run(m);
        ps1_sched_events(m);
    }
}

void ps1_run_cycles(ps1_machine *m, uint64_t cycles)
{
    uint64_t end = m->cycles + cycles;
    while (m->cycles < end) {
        uint64_t ne = m->next_event;
        if (ne > end)
            m->next_event = end;
        ps1_cpu_run(m);
        if (m->cycles >= ne)
            ps1_sched_events(m);
        else
            ps1_sched_recompute(m);
    }
}

int ps1_display(ps1_machine *m, uint32_t *xrgb, int max_w, int max_h, int *w, int *h)
{
    return ps1_gpu_display(m->gpu, xrgb, max_w, max_h, w, h);
}

size_t ps1_audio_pull(ps1_machine *m, int16_t *stereo, size_t frames)
{
    return ps1_spu_audio_pull(m->spu, stereo, frames);
}

const uint8_t *ps1_card_data(ps1_machine *m, int slot, size_t *size)
{
    if (slot < 0 || slot > 1)
        return NULL;
    if (size)
        *size = PS1_CARD_SIZE;
    return m->cards[slot].data;
}

int ps1_card_take_dirty(ps1_machine *m, int slot)
{
    if (slot < 0 || slot > 1)
        return 0;
    int d = m->cards[slot].dirty;
    m->cards[slot].dirty = false;
    return d;
}

int ps1_card_load(ps1_machine *m, int slot, const uint8_t *data, size_t size)
{
    if (slot < 0 || slot > 1 || !data || size != PS1_CARD_SIZE)
        return -1;
    memcpy(m->cards[slot].data, data, PS1_CARD_SIZE);
    m->cards[slot].dirty = false;
    m->cards[slot].flag = 0x08;
    return 0;
}

void ps1_card_insert(ps1_machine *m, int slot, int inserted)
{
    if (slot < 0 || slot > 1)
        return;
    if (inserted && !m->cards[slot].inserted)
        m->cards[slot].flag = 0x08;
    m->cards[slot].inserted = inserted != 0;
}

uint32_t ps1_reg(ps1_machine *m, int r) { return (r >= 0 && r < 32) ? m->r[r] : 0; }
uint32_t ps1_pc(ps1_machine *m) { return m->pc; }
uint32_t ps1_cop0(ps1_machine *m, int r) { return (r >= 0 && r < 32) ? m->cop0[r] : 0; }
uint64_t ps1_cycles(ps1_machine *m) { return m->cycles; }
void ps1_irq_regs(ps1_machine *m, uint32_t *i_stat, uint32_t *i_mask)
{
    *i_stat = m->i_stat;
    *i_mask = m->i_mask;
}
uint64_t ps1_frames(ps1_machine *m) { return m->frames; }
uint8_t *ps1_ram(ps1_machine *m)
{
    ps1_cpu_flush_blocks(m); /* the caller may write code there */
    return m->ram;
}
int ps1_exe_started(ps1_machine *m) { return m->exe_started; }
int ps1_bios_hle(ps1_machine *m) { return m->hle != NULL; }
struct ps1_gpu *ps1_get_gpu(ps1_machine *m) { return m->gpu; }

void ps1_set_tty(ps1_machine *m, void (*fn)(void *ctx, char c), void *ctx)
{
    m->tty = fn;
    m->tty_ctx = ctx;
}

void ps1_set_fast_boot(ps1_machine *m, int on) { m->skip_shell = on != 0; }

/* ---- Disc swap (XBA-166) ---- */

void ps1_set_lid(ps1_machine *m, int open)
{
    ps1_devices_sync(m);
    ps1_cdrom_set_lid(m->cdrom, open);
}

int ps1_change_disc(ps1_machine *m, const char *disc_path, char *err, size_t errlen)
{
    ps1_disc *d = NULL;
    if (disc_path && *disc_path) {
        char derr[256] = "";
        d = ps1_disc_open(disc_path, derr, sizeof derr);
        if (!d) {
            set_err(err, errlen, derr[0] ? derr : "cannot open the disc image");
            return -1;
        }
    }
    ps1_devices_sync(m);
    ps1_cdrom_set_disc(m->cdrom, d);
    if (m->disc)
        ps1_disc_close(m->disc);
    m->disc = d;
    return 0;
}
