/* n64_rdp_select.c -- the RDP of n64_devices.h / n64_rdp.h as a choice of back ends (XBA-173, n64_rdp_backend.h): the
 * software RDP (n64_rdp.c, its functions renamed n64_rdp_sw_*) or the Vulkan one (n64_rdp_vk.cpp, paraLLEl-RDP). The
 * back end is chosen at n64_rdp_create; every other call goes through its table. A back end that stops being able to
 * draw (ops->failed: the Vulkan RDP with a shader its driver cannot compile) is replaced by the software RDP once
 * the RDP is idle, after n64_rdp_run or n64_rdp_flush. The internal resolution (XBA-185, n64_rdp.h) is resolved here
 * ($RECOMP_N64_UPSCALE, $RECOMP_UPSCALE) and asked of the back end; only the Vulkan one honours it. */
#include "n64_rdp.h"
#include "n64_rdp_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The software RDP, as n64_rdp_sw_names.h renames it. */
typedef struct n64_rdp_sw n64_rdp_sw;
n64_rdp_sw *n64_rdp_sw_create(n64_rcp_bus bus);
void n64_rdp_sw_destroy(n64_rdp_sw *d);
void n64_rdp_sw_reset(n64_rdp_sw *d);
void n64_rdp_sw_attach_dmem(n64_rdp_sw *d, const uint8_t *dmem);
uint32_t n64_rdp_sw_read_reg(n64_rdp_sw *d, uint32_t reg);
void n64_rdp_sw_write_reg(n64_rdp_sw *d, uint32_t reg, uint32_t v);
void n64_rdp_sw_run(n64_rdp_sw *d);
int n64_rdp_sw_set_threads(n64_rdp_sw *d, int n);
void n64_rdp_sw_flush(n64_rdp_sw *d);
size_t n64_rdp_sw_state_extra_size(n64_rdp_sw *d);
void n64_rdp_sw_state_save(n64_rdp_sw *d, n64_rdp_common_state *c, uint8_t *extra);
int n64_rdp_sw_state_load(n64_rdp_sw *d, const n64_rdp_common_state *c, const uint8_t *extra, size_t n);
void n64_rdp_sw_get_stats(n64_rdp_sw *d, n64_rdp_stats *out);

static void *sw_create(n64_rcp_bus bus) { return n64_rdp_sw_create(bus); }
static void sw_destroy(void *d) { n64_rdp_sw_destroy(d); }
static void sw_reset(void *d) { n64_rdp_sw_reset(d); }
static void sw_attach_dmem(void *d, const uint8_t *dmem) { n64_rdp_sw_attach_dmem(d, dmem); }
static uint32_t sw_read_reg(void *d, uint32_t reg) { return n64_rdp_sw_read_reg(d, reg); }
static void sw_write_reg(void *d, uint32_t reg, uint32_t v) { n64_rdp_sw_write_reg(d, reg, v); }
static void sw_run(void *d) { n64_rdp_sw_run(d); }
static int sw_set_threads(void *d, int n) { return n64_rdp_sw_set_threads(d, n); }
static void sw_flush(void *d) { n64_rdp_sw_flush(d); }
static size_t sw_state_extra_size(void *d) { return n64_rdp_sw_state_extra_size(d); }
static void sw_state_save(void *d, n64_rdp_common_state *c, uint8_t *extra) { n64_rdp_sw_state_save(d, c, extra); }
static int sw_state_load(void *d, const n64_rdp_common_state *c, const uint8_t *extra, size_t n)
{
    return n64_rdp_sw_state_load(d, c, extra, n);
}
static void sw_stats(void *d, n64_rdp_stats *out) { n64_rdp_sw_get_stats(d, out); }

const n64_rdp_ops n64_rdp_sw_ops = { "software", sw_create, sw_destroy, sw_reset, sw_attach_dmem,
                                     sw_read_reg, sw_write_reg, sw_run, sw_set_threads, sw_flush, NULL,
                                     NULL, NULL, NULL, sw_state_extra_size, sw_state_save, sw_state_load,
                                     sw_stats, NULL, NULL, NULL, NULL, NULL };

struct n64_rdp {
    const n64_rdp_ops *ops;
    void *impl;
    n64_rcp_bus bus;     /* for a replacement back end */
    const uint8_t *dmem;
    int threads;         /* the last n64_rdp_set_threads, 0: never called */
    n64_rdp_stats past;  /* the counters of the back ends replaced (n64_rdp_get_stats) */
    int timing;          /* n64_rdp_time_runs */
    uint64_t run_ns;
    /* n64_rdp_set_defer_software: the software RDP's DP interrupt goes through sel_raise, which asks bus.defer_dp and
     * holds it (dp_held) until n64_rdp_dp_complete. */
    int defer_sw;
    int dp_held;
    uint64_t dp_completed; /* by this layer (back ends without dp_complete) */
};

static int s_defer_sw = -1; /* n64_rdp_set_defer_software; -1: $RECOMP_N64_RDP_DEFER_SOFTWARE */

void n64_rdp_set_defer_software(int on) { s_defer_sw = on != 0; }

static void sel_raise(void *ctx, int irq)
{
    n64_rdp *d = ctx;
    if (irq == N64_MI_DP && d->bus.defer_dp) {
        if (d->dp_held) { /* a second SYNC_FULL before the deadline: the first one completes now */
            d->dp_held = 0;
            d->dp_completed++;
            if (d->bus.raise)
                d->bus.raise(d->bus.ctx, irq);
        }
        if (d->bus.defer_dp(d->bus.ctx)) {
            d->dp_held = 1;
            return;
        }
    }
    if (d->bus.raise)
        d->bus.raise(d->bus.ctx, irq);
}

static void sel_clear(void *ctx, int irq)
{
    n64_rdp *d = ctx;
    if (d->bus.clear)
        d->bus.clear(d->bus.ctx, irq);
}

static void sel_wrote(void *ctx, uint32_t addr, uint32_t len)
{
    n64_rdp *d = ctx;
    if (d->bus.wrote)
        d->bus.wrote(d->bus.ctx, addr, len);
}

/* The bus a software RDP gets: the core's, or with n64_rdp_set_defer_software this layer's in front of it. */
static n64_rcp_bus sw_bus(n64_rdp *d)
{
    n64_rcp_bus b = d->bus;
    if (d->defer_sw && d->bus.defer_dp) {
        b.raise = sel_raise;
        b.clear = sel_clear;
        b.wrote = d->bus.wrote ? sel_wrote : NULL;
        b.ctx = d;
    }
    b.defer_dp = NULL;
    return b;
}

static uint64_t run_clock_ns(void)
{
    struct timespec ts;
#ifdef _WIN32
    timespec_get(&ts, TIME_UTC);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void n64_rdp_time_runs(n64_rdp *d, int on)
{
    if (d)
        d->timing = on != 0;
}

uint64_t n64_rdp_run_ns(const n64_rdp *d) { return d ? d->run_ns : 0; }

static void backend_stats(const n64_rdp *d, n64_rdp_stats *out)
{
    memset(out, 0, sizeof *out);
    if (d->ops->stats)
        d->ops->stats(d->impl, out);
}

void n64_rdp_get_stats(n64_rdp *d, n64_rdp_stats *out)
{
    if (!d) {
        memset(out, 0, sizeof *out);
        return;
    }
    backend_stats(d, out);
    out->commands += d->past.commands;
    out->full_syncs += d->past.full_syncs;
    out->waits += d->past.waits;
    out->wait_ns += d->past.wait_ns;
    out->queue_waits += d->past.queue_waits;
    out->queue_ns += d->past.queue_ns;
    out->scanouts += d->past.scanouts;
    out->scanout_ns += d->past.scanout_ns;
    out->stalled_compiles += d->past.stalled_compiles;
    out->stalled_compile_ns += d->past.stalled_compile_ns;
    out->dp_completed += d->past.dp_completed + d->dp_completed;
    out->dp_waits += d->past.dp_waits;
    out->dp_wait_ns += d->past.dp_wait_ns;
    out->prefetches += d->past.prefetches;
    out->prefetch_hits += d->past.prefetch_hits;
}

#ifndef N64_RDP_DEFAULT_BACKEND
#if defined(__ANDROID__) && defined(N64_RDP_HAVE_VULKAN)
#define N64_RDP_DEFAULT_BACKEND N64_RDP_VULKAN
#else
#define N64_RDP_DEFAULT_BACKEND N64_RDP_SOFTWARE
#endif
#endif

static n64_rdp_backend s_backend = N64_RDP_AUTO;

void n64_rdp_set_backend(n64_rdp_backend b) { s_backend = b; }

n64_rdp_backend n64_rdp_backend_parse(const char *s)
{
    if (s && (!strcmp(s, "vulkan") || !strcmp(s, "vk") || !strcmp(s, "gpu")))
        return N64_RDP_VULKAN;
    if (s && (!strcmp(s, "software") || !strcmp(s, "sw") || !strcmp(s, "cpu")))
        return N64_RDP_SOFTWARE;
    return N64_RDP_AUTO;
}

const char *n64_rdp_backend_name(const n64_rdp *d) { return d ? d->ops->name : "none"; }

/* ---- Internal resolution (XBA-185) ---------------------------------------------------------------------------------- */
static int s_upscale; /* n64_rdp_set_upscale; 0: the environment */

int n64_rdp_upscale_parse(const char *s)
{
    if (!s || !*s)
        return 0;
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end || v < 1)
        return 0;
    return v >= 8 ? 8 : v >= 4 ? 4 : v >= 2 ? 2 : 1;
}

int n64_rdp_upscale_env(void)
{
    int f = n64_rdp_upscale_parse(getenv("RECOMP_N64_UPSCALE"));
    if (!f)
        f = n64_rdp_upscale_parse(getenv("RECOMP_UPSCALE"));
    return f ? f : 1;
}

void n64_rdp_set_upscale(int factor)
{
    s_upscale = factor <= 0 ? 0 : factor >= 8 ? 8 : factor >= 4 ? 4 : factor >= 2 ? 2 : 1;
}

int n64_rdp_upscale_wanted(void) { return s_upscale ? s_upscale : n64_rdp_upscale_env(); }

int n64_rdp_upscale(const n64_rdp *d) { return d && d->ops->upscale ? d->ops->upscale(d->impl) : 1; }

int n64_rdp_scanout_scale(const n64_rdp *d)
{
    return d && d->ops->scanout_scale && d->ops->scanout ? d->ops->scanout_scale(d->impl) : 0;
}

int n64_rdp_scanout(n64_rdp *d, const uint32_t *vi, uint32_t field, uint32_t *xrgb, int max_w, int max_h, int *w,
                    int *h)
{
    if (!n64_rdp_scanout_scale(d))
        return -1;
    return d->ops->scanout(d->impl, vi, field, xrgb, max_w, max_h, w, h);
}

/* The software RDP draws at 1x: said once when more was asked (also when the Vulkan RDP could not start or failed). */
static void upscale_needs_vulkan(void)
{
    int f = n64_rdp_upscale_wanted();
    if (f > 1)
        fprintf(stderr, "n64: upscale %dx needs the Vulkan RDP, the software RDP draws at 1x\n", f);
}

n64_rdp *n64_rdp_create(n64_rcp_bus bus)
{
    n64_rdp *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->bus = bus;
    if (s_defer_sw < 0) {
        const char *e = getenv("RECOMP_N64_RDP_DEFER_SOFTWARE");
        d->defer_sw = e && atoi(e) != 0;
    } else {
        d->defer_sw = s_defer_sw;
    }
    n64_rdp_backend b = s_backend != N64_RDP_AUTO ? s_backend : n64_rdp_backend_parse(getenv("RECOMP_N64_RDP"));
    const int asked = b == N64_RDP_VULKAN;
    (void)asked; /* only reported in builds without the Vulkan RDP */
    if (b == N64_RDP_AUTO)
        b = N64_RDP_DEFAULT_BACKEND;
    if (b == N64_RDP_VULKAN) {
#ifdef N64_RDP_HAVE_VULKAN
        d->ops = &n64_rdp_vk_ops;
        d->impl = d->ops->create(bus);
        if (!d->impl)
            fprintf(stderr, "n64: the Vulkan RDP cannot run here (no device, or its shaders fail), "
                            "software RDP instead\n");
#else
        if (asked)
            fprintf(stderr, "n64: this build has no Vulkan RDP (XBR_N64_RDP_VULKAN), software RDP instead\n");
#endif
    }
    if (!d->impl) {
        d->ops = &n64_rdp_sw_ops;
        d->impl = d->ops->create(sw_bus(d));
        if (d->impl)
            upscale_needs_vulkan();
    }
    if (!d->impl) {
        free(d);
        return NULL;
    }
    return d;
}

void n64_rdp_destroy(n64_rdp *d)
{
    if (!d)
        return;
    d->ops->destroy(d->impl);
    free(d);
}

void n64_rdp_reset(n64_rdp *d)
{
    d->dp_held = 0; /* the core dropped its deadline (machine reset) */
    d->ops->reset(d->impl);
}
void n64_rdp_attach_dmem(n64_rdp *d, const uint8_t *dmem)
{
    d->dmem = dmem;
    d->ops->attach_dmem(d->impl, dmem);
}
uint32_t n64_rdp_read_reg(n64_rdp *d, uint32_t reg) { return d->ops->read_reg(d->impl, reg); }
void n64_rdp_write_reg(n64_rdp *d, uint32_t reg, uint32_t v) { d->ops->write_reg(d->impl, reg, v); }
int n64_rdp_set_threads(n64_rdp *d, int n)
{
    d->threads = n;
    return d->ops->set_threads(d->impl, n);
}

/* The software RDP in place of a failed back end, with the DPC registers the game sees (the RDP is idle: CURRENT is
 * END) and its status bits. The RDP state the old back end held (modes, tiles, TMEM) is lost: games set it again in
 * their next display list. */
static void replace_failed(n64_rdp *d)
{
    const uint32_t end = d->ops->read_reg(d->impl, 1);
    const uint32_t status = d->ops->read_reg(d->impl, 3);
    if (d->ops->read_reg(d->impl, 2) != end)
        return; /* commands still to run: at a later flush */
    void *sw = n64_rdp_sw_ops.create(sw_bus(d)); /* a DP interrupt the failed one deferred: n64_rdp_dp_complete */
    if (!sw) {
        fprintf(stderr, "n64: the software RDP cannot start, the %s RDP stays\n", d->ops->name);
        return;
    }
    fprintf(stderr, "n64: the %s RDP failed, software RDP from now on\n", d->ops->name);
    if (n64_rdp_upscale(d) > 1)
        upscale_needs_vulkan();
    n64_rdp_get_stats(d, &d->past);
    d->dp_completed = 0; /* in past now */
    if (d->ops->dp_complete)
        d->dp_held = 1; /* a DP interrupt it deferred, if any, comes at the core's deadline (n64_rdp_dp_complete) */
    d->ops->destroy(d->impl);
    d->ops = &n64_rdp_sw_ops;
    d->impl = sw;
    if (d->dmem)
        d->ops->attach_dmem(d->impl, d->dmem);
    if (d->threads > 0)
        d->ops->set_threads(d->impl, d->threads);
    d->ops->write_reg(d->impl, 3, (status & 1 ? 0x002 : 0) | (status & 2 ? 0x008 : 0) | (status & 4 ? 0x020 : 0));
    d->ops->write_reg(d->impl, 0, end); /* START then END: CURRENT = END, nothing to run */
    d->ops->write_reg(d->impl, 1, end);
}

/* After the commands ran, the RDP is idle (CURRENT is END): the moment to replace a failed back end. */
void n64_rdp_run(n64_rdp *d)
{
    uint64_t t0 = d->timing ? run_clock_ns() : 0;
    d->ops->run(d->impl);
    if (d->ops->failed && d->ops->failed(d->impl))
        replace_failed(d);
    if (t0)
        d->run_ns += run_clock_ns() - t0;
}

int n64_rdp_unreported(n64_rdp *d, uint32_t lo, uint32_t hi)
{
    if (!d)
        return 0;
    if (d->ops->unreported)
        return d->ops->unreported(d->impl, lo, hi);
    n64_rdp_flush(d); /* the software RDP: its pixels land and are reported at a commit (already done at frame end) */
    return 0;
}

void n64_rdp_dp_complete(n64_rdp *d)
{
    if (!d)
        return;
    if (d->ops->dp_complete) {
        d->ops->dp_complete(d->impl);
        if (d->ops->failed && d->ops->failed(d->impl))
            replace_failed(d);
        return;
    }
    if (!d->dp_held) /* held here, or maybe deferred by a back end replaced since (replace_failed) */
        return;
    d->dp_held = 0;
    d->dp_completed++;
    if (d->bus.raise)
        d->bus.raise(d->bus.ctx, N64_MI_DP);
}

void n64_rdp_scanout_prefetch(n64_rdp *d, const uint32_t *vi, uint32_t field, uint32_t lo, uint32_t hi)
{
    if (d && d->ops->scanout_prefetch && n64_rdp_scanout_scale(d))
        d->ops->scanout_prefetch(d->impl, vi, field, lo, hi);
}

int n64_rdp_scanout_rgba(n64_rdp *d, const uint32_t *vi, uint32_t field, const uint8_t **rgba, int *w, int *h,
                         int *pitch)
{
    if (!n64_rdp_scanout_scale(d) || !d->ops->scanout_rgba)
        return -1;
    return d->ops->scanout_rgba(d->impl, vi, field, rgba, w, h, pitch);
}

void n64_rdp_scanout_forget(n64_rdp *d)
{
    if (d && d->ops->scanout_forget)
        d->ops->scanout_forget(d->impl);
}

void n64_rdp_flush(n64_rdp *d)
{
    d->ops->flush(d->impl);
    if (d->ops->failed && d->ops->failed(d->impl))
        replace_failed(d);
}

void n64_rdp_save_caches(void)
{
#ifdef N64_RDP_HAVE_VULKAN
    n64_rdp_vk_save_caches("pause");
#endif
}

void n64_rdp_frame_end(n64_rdp *d)
{
    if (d->ops == &n64_rdp_sw_ops)
        n64_rdp_flush(d);
}

/* The 16-bit pixels' hidden coverage bits (n64_rdp.h, for the VI's anti-aliasing filter; NULL: none, the VI then uses
 * the pixel's low bit). The software RDP keeps them in a side array; the Vulkan RDP keeps them on the GPU: NULL. */
const uint8_t *n64_rdp_sw_hidden_bits(n64_rdp_sw *d);
const uint8_t *n64_rdp_hidden_bits(n64_rdp *d)
{
    return d->ops == &n64_rdp_sw_ops ? n64_rdp_sw_hidden_bits(d->impl) : NULL;
}

/* ---- Save states (XBA-183) ------------------------------------------------------------------------------------------ */
/* The bytes: a small header (magic, the back end's name, the common part's and the extra part's sizes), the common
 * part (n64_rdp_common_state), the back end's own. A state of the other back end loads its common part only (the
 * player switched renderers between saving and loading: the game sets its drawing state again in its next display
 * list, as after replace_failed). */
#define RDP_STATE_MAGIC 0x52445053u /* "RDPS" */
#define RDP_STATE_HEAD 24u

static size_t extra_size(n64_rdp *d) { return d->ops->state_extra_size ? d->ops->state_extra_size(d->impl) : 0; }

size_t n64_rdp_state_size(n64_rdp *d)
{
    return RDP_STATE_HEAD + sizeof(n64_rdp_common_state) + extra_size(d);
}

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t get32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

void n64_rdp_state_save(n64_rdp *d, uint8_t *out)
{
    size_t xn = extra_size(d);
    memset(out, 0, RDP_STATE_HEAD);
    put32(out, RDP_STATE_MAGIC);
    snprintf((char *)out + 4, 12, "%s", d->ops->name);
    put32(out + 16, (uint32_t)sizeof(n64_rdp_common_state));
    put32(out + 20, (uint32_t)xn);
    n64_rdp_common_state c;
    memset(&c, 0, sizeof c);
    if (d->ops->state_save)
        d->ops->state_save(d->impl, &c, out + RDP_STATE_HEAD + sizeof c);
    memcpy(out + RDP_STATE_HEAD, &c, sizeof c);
}

int n64_rdp_state_load(n64_rdp *d, const uint8_t *in, size_t n)
{
    if (n < RDP_STATE_HEAD + sizeof(n64_rdp_common_state) || get32(in) != RDP_STATE_MAGIC ||
        get32(in + 16) != sizeof(n64_rdp_common_state) || !d->ops->state_load)
        return -1;
    size_t xn = get32(in + 20);
    if (xn != n - RDP_STATE_HEAD - sizeof(n64_rdp_common_state))
        return -1;
    char name[12];
    memcpy(name, in + 4, sizeof name);
    name[sizeof name - 1] = 0;
    const int same = strcmp(name, d->ops->name) == 0;
    if (same && xn != extra_size(d))
        return -1;
    n64_rdp_common_state c;
    memcpy(&c, in + RDP_STATE_HEAD, sizeof c);
    if (!same)
        fprintf(stderr, "n64: state saved with the %s RDP, loaded on the %s RDP: its drawing state starts afresh\n",
                name, d->ops->name);
    int rc = d->ops->state_load(d->impl, &c, same ? in + RDP_STATE_HEAD + sizeof c : NULL, xn);
    if (rc == 0)
        d->dp_held = 0; /* the machine's own events are the state's now (none pending: n64_state_save) */
    return rc;
}
