/* Placeholder RDP (n64_devices.h), built when n64_rdp.c (N-6) is absent: the DPC registers and a command walker that
 * draws nothing. Commands from CURRENT to END (RDRAM or, with XBUS, DMEM) are skipped by their length; SYNC_FULL raises
 * the DP interrupt, so code that waits for the RDP to finish carries on. */
#include "n64_rdp.h"
#include "n64_rdp_backend.h"

#include <stdlib.h>
#include <string.h>

enum {
    DP_XBUS = 1u << 0,
    DP_FREEZE = 1u << 1,
    DP_FLUSH = 1u << 2,
    DP_CBUF_READY = 1u << 7,
};

struct n64_rdp {
    n64_rcp_bus bus;
    const uint8_t *dmem;
    uint32_t start, end, current, status;
    uint64_t commands, full_syncs; /* n64_rdp_get_stats */
};

n64_rdp *n64_rdp_create(n64_rcp_bus bus)
{
    n64_rdp *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->bus = bus;
    n64_rdp_reset(d);
    return d;
}

void n64_rdp_destroy(n64_rdp *d) { free(d); }

void n64_rdp_reset(n64_rdp *d)
{
    d->start = d->end = d->current = 0;
    d->status = DP_CBUF_READY;
}

void n64_rdp_attach_dmem(n64_rdp *d, const uint8_t *dmem) { d->dmem = dmem; }

/* Save states (XBA-183): the registers only. */
size_t n64_rdp_state_extra_size(n64_rdp *d)
{
    (void)d;
    return 0;
}

void n64_rdp_state_save_sw(n64_rdp *d, n64_rdp_common_state *c, uint8_t *extra)
{
    (void)extra;
    memset(c, 0, sizeof *c);
    c->start = d->start;
    c->end = d->end;
    c->current = d->current;
    c->status = d->status;
}

int n64_rdp_state_load_sw(n64_rdp *d, const n64_rdp_common_state *c, const uint8_t *extra, size_t n)
{
    if (extra && n != 0)
        return -1;
    d->start = c->start;
    d->end = c->end;
    d->current = c->current;
    d->status = c->status;
    return 0;
}

uint32_t n64_rdp_read_reg(n64_rdp *d, uint32_t reg)
{
    switch (reg) {
    case 0: return d->start;
    case 1: return d->end;
    case 2: return d->current;
    case 3: return d->status;
    default: return 0;
    }
}

void n64_rdp_write_reg(n64_rdp *d, uint32_t reg, uint32_t v)
{
    switch (reg) {
    case 0:
        d->start = d->current = v & 0xFFFFF8;
        break;
    case 1:
        d->end = v & 0xFFFFF8;
        break;
    case 3:
        if (v & 0x01) d->status &= ~DP_XBUS;
        if (v & 0x02) d->status |= DP_XBUS;
        if (v & 0x04) d->status &= ~DP_FREEZE;
        if (v & 0x08) d->status |= DP_FREEZE;
        if (v & 0x10) d->status &= ~DP_FLUSH;
        if (v & 0x20) d->status |= DP_FLUSH;
        break;
    default:
        break;
    }
}

/* Length in 64-bit words of each command. */
static uint32_t cmd_words(uint32_t op)
{
    switch (op) {
    case 0x08: return 4;
    case 0x09: return 6;
    case 0x0A: return 12;
    case 0x0B: return 14;
    case 0x0C: return 12;
    case 0x0D: return 14;
    case 0x0E: return 20;
    case 0x0F: return 22;
    case 0x24: case 0x25: return 2;
    default: return 1;
    }
}

void n64_rdp_run(n64_rdp *d)
{
    if (d->status & DP_FREEZE)
        return;
    while (d->current < d->end) {
        uint8_t op;
        if (d->status & DP_XBUS)
            op = d->dmem ? d->dmem[d->current & 0xFF8] & 0x3F : 0;
        else
            op = d->current < d->bus.rdram_size ? d->bus.rdram[d->current] & 0x3F : 0;
        uint32_t n = cmd_words(op) * 8;
        if (d->current + n > d->end)
            break; /* the rest of this command is not there yet */
        d->current += n;
        d->commands++;
        if (op == 0x29) {
            d->full_syncs++;
            d->bus.raise(d->bus.ctx, N64_MI_DP);
        }
    }
}

/* n64_rdp.h: no threads here. */
void n64_rdp_set_thread_hook(n64_rdp_thread_hook fn, void *user)
{
    (void)fn;
    (void)user;
}

void n64_rdp_get_thread_hook(n64_rdp_thread_hook *fn, void **user)
{
    *fn = NULL;
    *user = NULL;
}

/* n64_rdp.h: nothing is batched here. */
int n64_rdp_set_threads(n64_rdp *d, int n)
{
    (void)d;
    (void)n;
    return 1;
}

void n64_rdp_flush(n64_rdp *d) { (void)d; }
void n64_rdp_get_stats(n64_rdp *d, n64_rdp_stats *out)
{
    memset(out, 0, sizeof *out);
    out->commands = d->commands;
    out->full_syncs = d->full_syncs;
}
const uint8_t *n64_rdp_hidden_bits(n64_rdp *d)
{
    (void)d;
    return NULL;
}
