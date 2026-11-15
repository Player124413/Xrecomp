/* N64 machine (XBA-169 / 171 / 172): creation, reset, the event scheduler (VI lines, COUNT = COMPARE, PI / SI / AI
 * DMA completions) around the CPU loop, the RSP's time slices, and the debug accessors. */
#include "n64_internal.h"
#include "n64_rdp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* While the RSP runs, the CPU runs in slices this long (ticks) and the RSP catches up after each. */
#define RSP_SLICE 600

/* ---- Deferred DP interrupt (n64_core.h n64_set_rdp_defer) --------------------------------------------------------
 * The Vulkan RDP (n64_rdp_vk.cpp) queues a SYNC_FULL's work to the GPU and asks rcp_defer_dp whether the DP interrupt
 * may come later. Yes: EV_DP is scheduled at the deadline and n64_rdp_dp_complete waits for the GPU there, writes the
 * pixels' rows to the write tracking, clears the busy bits and raises the interrupt. Ocarina of Time PAL (one picture
 * per 3 VIs): the graphics task starts right after a buffer swap, reaches SYNC_FULL at 70 % of the same VI and swaps
 * 2.3 VIs later; at 4x the emulation thread used to wait ~17 ms for the GPU inside that one VI frame.
 * Policy (emulated facts only: the same moments on every host):
 *   - a picture qualifies when it had exactly one SYNC_FULL, lasted 2 VIs or more, its DP interrupt came before the
 *     next VI_ORIGIN change and the SYNC_FULL 2 VI interrupts or more before it: the game did not swap at the first
 *     VI after the RDP was done, so the DP interrupt may come just after that VI (a game that swaps as soon as it can
 *     never qualifies: its SYNC_FULL is 1 VI interrupt before its swap);
 *   - a SYNC_FULL is deferred when the last DP_STREAK pictures qualified, it is the first one since the last
 *     VI_ORIGIN change, DP_WARM_VIS VIs went by since reset / state load, and no backoff runs. Deadline: the next VI
 *     interrupt plus offset_pm / 1000 of a VI (the remaining wait lands after the host's frame pacing, at the start of
 *     a light frame). OoT: at most +0.36 VI against 2.3 VIs of slack;
 *   - while deferring, a picture longer than the longest of the qualifying streak before stops it for DP_BACKOFF_VIS
 *     VIs (the deferral would cost the game a VI);
 *   - a second SYNC_FULL before the deadline: the back end completes the first one at once, the second is not
 *     deferred.
 * A save state completes a pending one first (n64_dp_complete_now: states never hold one, their layout is unchanged);
 * reset and state loads drop it (the back end synchronises). */
#define DP_WARM_VIS 60
#define DP_STREAK 3
#define DP_BACKOFF_VIS 500
#define DP_OFFSET_DEFAULT 62 /* per mille of a VI: 1/16 */

static void dp_policy_restart(n64_machine *m)
{
    int mode = m->dpp.mode;
    uint32_t off = m->dpp.offset_pm;
    memset(&m->dpp, 0, sizeof m->dpp);
    m->dpp.mode = mode;
    m->dpp.offset_pm = off;
}

int n64_rdp_defer_parse(const char *s, int *offset_pm)
{
    *offset_pm = DP_OFFSET_DEFAULT;
    if (!s || !*s || !strcmp(s, "auto") || !strcmp(s, "on") || !strcmp(s, "1"))
        return N64_DP_DEFER_AUTO; /* "1" reads as on, not 1 % */
    char *end;
    double v = strtod(s, &end);
    if (end == s || *end || v != v)
        return N64_DP_DEFER_AUTO;
    if (v <= 0)
        return N64_DP_DEFER_OFF;
    if (v > 100)
        v = 100;
    *offset_pm = (int)(v * 10 + 0.5);
    if (*offset_pm < 1)
        *offset_pm = 1;
    return N64_DP_DEFER_AUTO;
}

void n64_set_rdp_defer(n64_machine *m, int mode, int offset_pm)
{
    m->dpp.mode = mode == N64_DP_DEFER_AUTO ? N64_DP_DEFER_AUTO : N64_DP_DEFER_OFF;
    m->dpp.offset_pm = offset_pm <= 0 ? DP_OFFSET_DEFAULT : offset_pm > 1000 ? 1000u : (uint32_t)offset_pm;
}

/* n64_io.c, at each VI interrupt (t: its time). */
void n64_dp_vi(n64_machine *m, uint64_t t)
{
    m->dpp.warm++;
    m->dpp.last_vi = t ? t : 1;
    m->dpp.vis++;
    m->dpp.vis_sync++;
}

/* n64_io.c, at each VI_ORIGIN write with a new value: the picture that ends is judged. */
void n64_dp_origin(n64_machine *m)
{
    struct { uint32_t vis, syncs, vis_sync; bool raised; } pic = { m->dpp.vis, m->dpp.syncs, m->dpp.vis_sync,
                                                                   m->dpp.raised };
    const bool q = pic.syncs == 1 && pic.vis >= 2 && pic.raised && pic.vis_sync >= 2;
    if (m->dpp.deferred && m->dpp.base && pic.vis > m->dpp.base) {
        m->dpp.backoff_until = m->dpp.warm + DP_BACKOFF_VIS;
        m->dpp.streak = 0;
        m->dpp.base = 0;
        m->perf.dp_backoffs++;
    } else if (q) {
        m->dpp.streak++;
        if (!m->dpp.deferred && pic.vis > m->dpp.base)
            m->dpp.base = pic.vis;
    } else {
        m->dpp.streak = 0;
        m->dpp.base = 0;
    }
    m->dpp.vis = 0;
    m->dpp.syncs = 0;
    m->dpp.raised = false;
    m->dpp.deferred = false;
}

/* When the DP interrupt of the SYNC_FULL being handled now should come (ticks), 0: now. */
static uint64_t dp_deadline(n64_machine *m)
{
    const uint64_t period = m->vi_line_ticks * (uint64_t)m->vi_lines;
    if (m->dpp.mode != N64_DP_DEFER_AUTO || m->dpp.warm < DP_WARM_VIS || m->dpp.warm < m->dpp.backoff_until ||
        m->dpp.syncs != 1 || m->dpp.streak < DP_STREAK || !m->dpp.last_vi || !period)
        return 0;
    uint64_t next = m->dpp.last_vi + period;
    while (next <= m->now)
        next += period;
    return next + period * m->dpp.offset_pm / 1000;
}

static int rcp_defer_dp(void *ctx)
{
    n64_machine *m = ctx;
    m->ev[EV_DP] = EV_NEVER; /* one at a time: the back end completed any earlier one before asking */
    m->dpp.syncs++;
    m->dpp.vis_sync = 0;
    uint64_t at = dp_deadline(m);
    if (!at) {
        m->perf.dp_now++;
        return 0;
    }
    m->dpp.deferred = true;
    m->perf.dp_deferred++;
    n64_schedule(m, EV_DP, at);
    return 1;
}

/* EV_DP: the deadline. The back end's wait for the GPU counts as the RDP's time in a timed frame. */
static uint64_t perf_ns(void);
static void dp_fire(n64_machine *m)
{
    m->ev[EV_DP] = EV_NEVER;
    if (N64_UNLIKELY(m->perf_timing)) {
        uint64_t t0 = perf_ns();
        n64_rdp_dp_complete(m->rdp);
        m->perf.rdp_ns += perf_ns() - t0;
    } else {
        n64_rdp_dp_complete(m->rdp);
    }
}

void n64_dp_complete_now(n64_machine *m)
{
    if (m->ev[EV_DP] == EV_NEVER)
        return;
    m->ev[EV_DP] = EV_NEVER;
    m->perf.dp_early++;
    n64_rdp_dp_complete(m->rdp);
}

void n64_dp_state_loaded(n64_machine *m)
{
    dp_policy_restart(m);
}

static void rcp_raise(void *ctx, int irq)
{
    n64_machine *m = ctx;
    if (irq == N64_MI_DP)
        m->dpp.raised = true;
    n64_mi_raise(m, irq);
}
static void rcp_clear(void *ctx, int irq) { n64_mi_clear(ctx, irq); }
static void rcp_wrote(void *ctx, uint32_t addr, uint32_t len)
{
    n64_machine *m = ctx;
    n64_rdram_wrote(m, addr, len);
    if (!len || addr >= m->rdram_size)
        return;
    uint32_t last = addr + len - 1;
    if (last >= m->rdram_size)
        last = m->rdram_size - 1;
    for (uint32_t p = addr >> N64_PAGE_SHIFT; p <= last >> N64_PAGE_SHIFT; p++)
        m->rcp_gen[p]++; /* the same pages n64_rdram_wrote bumped: n64_display_changed tells them apart */
    if (addr < m->disp.hi && last >= m->disp.lo)
        m->disp_rcp_hits++;
}

static void seterr(char *err, size_t n, const char *msg)
{
    if (err && n)
        snprintf(err, n, "%s", msg);
}

static void machine_reset(n64_machine *m)
{
    m->now = 0;
    m->stop_at = 0;
    for (int i = 0; i < EV_COUNT; i++)
        m->ev[i] = EV_NEVER; /* a deferred DP interrupt too: the RDP's reset below drops its GPU wait */
    dp_policy_restart(m);
    m->instructions = 0;
    m->frames = 0;
    m->frame_done = false;
    memset(m->rdram, 0, m->rdram_size);
    m->mi_mode = 0;
    m->mi_intr = 0;
    m->mi_mask = 0;
    memset(m->ri, 0, sizeof m->ri);
    memset(m->rdram_regs, 0, sizeof m->rdram_regs);
    m->rdram_regs[0] = 0xB4190010u; /* device type */
    m->pi_dram = m->pi_cart = m->pi_rd_len = m->pi_wr_len = m->pi_status = 0;
    memset(m->pi_dom, 0, sizeof m->pi_dom);
    m->si_dram = m->si_stat = 0;
    m->si_read_pending = false;
    m->pif_rom_lock = 0;
    m->flash_mode = FLASH_IDLE;
    m->flash_status = 0;
    n64_rsp_reset(m->rsp);
    n64_rdp_reset(m->rdp);
    m->rsp_tick = 0;
    n64_cpu_reset(m);
    n64_vi_reset(m);
    n64_ai_reset(m);
    n64_pif_boot(m);
    n64_cpu_set_count(m, 0);
    for (uint32_t p = 0; p < (m->rdram_size >> N64_PAGE_SHIFT); p++)
        m->rdram_gen[p]++; /* RDRAM was cleared and rewritten */
    m->rc_lo = m->rc_len = 0;
    m->rc_hit = 0;
    if (m->rc)
        n64_recomp_reset(m);
}

n64_machine *n64_create(const uint8_t *rom, size_t size, const n64_options *opts, char *err, size_t errlen)
{
    if (!rom || size < 0x1000) {
        seterr(err, errlen, "ROM too small (at least 4 KB)");
        return NULL;
    }
    if (size > N64_ROM_MAX) {
        seterr(err, errlen, "ROM too large (at most 64 MB)");
        return NULL;
    }
    n64_machine *m = calloc(1, sizeof *m);
    if (!m) {
        seterr(err, errlen, "out of memory");
        return NULL;
    }
    if (opts)
        m->opts = *opts;
    else
        m->opts.force_pal = -1;
    uint32_t rsize = (uint32_t)((size + 3) & ~(size_t)3);
    m->rom = calloc(1, rsize);
    m->rdram_size = m->opts.rdram_size == 0x400000u ? 0x400000u : N64_RDRAM_SIZE;
    m->rdram = calloc(1, m->rdram_size);
    m->rdram_gen = calloc(m->rdram_size >> N64_PAGE_SHIFT, sizeof *m->rdram_gen);
    m->rcp_gen = calloc(m->rdram_size >> N64_PAGE_SHIFT, sizeof *m->rcp_gen);
    if (!m->rom || !m->rdram || !m->rdram_gen || !m->rcp_gen) {
        seterr(err, errlen, "out of memory");
        n64_destroy(m);
        return NULL;
    }
    memcpy(m->rom, rom, size);
    m->rom_size = rsize;
    n64_rom_order order = n64_rom_detect(m->rom, m->rom_size);
    if (n64_rom_normalize(m->rom, m->rom_size) != 0 || n64_rom_parse(m->rom, m->rom_size, &m->info) != 0) {
        seterr(err, errlen, "not an N64 ROM (unknown byte order)");
        n64_destroy(m);
        return NULL;
    }
    m->info.order = order;
    m->pal = m->opts.force_pal >= 0 ? m->opts.force_pal != 0 : m->info.pal != 0;

    {
        int off = 0;
        int mode = n64_rdp_defer_parse(getenv("RECOMP_N64_RDP_DEFER"), &off);
        n64_set_rdp_defer(m, mode, off);
    }
    n64_rcp_bus bus = {m->rdram, m->rdram_size, rcp_raise, rcp_clear, m, rcp_wrote, rcp_defer_dp};
    m->rsp = n64_rsp_create(bus);
    m->rdp = n64_rdp_create(bus);
    if (!m->rsp || !m->rdp) {
        seterr(err, errlen, "cannot create the RSP / RDP");
        n64_destroy(m);
        return NULL;
    }
    n64_rsp_attach_rdp(m->rsp, m->rdp);
    m->spmem = n64_rsp_mem(m->rsp);
    n64_rdp_attach_dmem(m->rdp, m->spmem);

    if (m->opts.save_kind != N64_SAVE_NONE && m->opts.save_kind < N64_SAVE_PAK0)
        n64_save_set_kind(m, m->opts.save_kind);
    else if (m->info.save_hint != N64_SAVE_NONE)
        n64_save_set_kind(m, m->info.save_hint);
    m->save_fixed = m->save_kind != N64_SAVE_NONE;
    for (int p = 0; p < 4; p++)
        n64_pak_format(m->pak[p]);
    m->pad[0].connected = true;
    m->pad[0].acc = N64_ACC_CONTROLLER_PAK;
    machine_reset(m);
    return m;
}

void n64_destroy(n64_machine *m)
{
    if (!m)
        return;
    if (m->rsp)
        n64_rsp_destroy(m->rsp);
    if (m->rdp)
        n64_rdp_destroy(m->rdp);
    n64_recomp_free(m);
    n64_cpu_free(m);
    free(m->rom);
    free(m->rdram);
    free(m->rdram_gen);
    free(m->rcp_gen);
    free(m->flash);
    free(m->isv);
    n64_vi_free(m);
    free(m);
}

void n64_reset(n64_machine *m)
{
    machine_reset(m);
}

const n64_rom_info *n64_info(n64_machine *m)
{
    return &m->info;
}

/* ---- Scheduler ------------------------------------------------------------------------------------------------- */

static void dispatch(n64_machine *m)
{
    for (;;) {
        int e = -1;
        uint64_t t = m->now;
        for (int i = 0; i < EV_COUNT; i++)
            if (m->ev[i] <= t) {
                t = m->ev[i];
                e = i;
            }
        if (e < 0)
            return;
        switch (e) {
        case EV_VI:
            n64_vi_event(m);
            break;
        case EV_COMPARE:
            m->cpu.cop0[C0_CAUSE] |= CAUSE_IP7;
            n64_cpu_update_irq(m);
            m->ev[EV_COMPARE] = EV_NEVER;
            n64_cpu_schedule_compare(m);
            if (m->ev[EV_COMPARE] <= m->now) /* COMPARE = COUNT right now: next wrap */
                m->ev[EV_COMPARE] = m->now + (0x100000000ull << 2);
            break;
        case EV_PI:
            m->ev[EV_PI] = EV_NEVER;
            m->pi_status &= ~1u;
            n64_mi_raise(m, N64_MI_PI);
            break;
        case EV_SI:
            m->ev[EV_SI] = EV_NEVER;
            m->si_stat &= ~1u;
            n64_mi_raise(m, N64_MI_SI);
            break;
        case EV_AI:
            n64_ai_event(m);
            break;
        case EV_DP:
            dp_fire(m);
            break;
        default:
            m->ev[e] = EV_NEVER;
            break;
        }
    }
}

/* ---- Host performance counters (n64_perf_enable): no effect on the machine ----------------------------------- */

static uint64_t perf_ns(void)
{
#ifdef _WIN32
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void n64_perf_enable(n64_machine *m, int every)
{
    m->perf_every = every > 0 ? (uint32_t)every : 0;
    m->perf_count = 0;
}

void n64_perf_get(n64_machine *m, n64_perf *out)
{
    *out = m->perf;
    n64_rdp_stats r;
    n64_rdp_get_stats(m->rdp, &r);
    out->rdp_commands = r.commands;
    out->rdp_full_syncs = r.full_syncs;
    out->rdp_waits = r.waits;
    out->rdp_wait_ns = r.wait_ns;
    out->rdp_queue_waits = r.queue_waits;
    out->rdp_queue_ns = r.queue_ns;
    out->rdp_scanouts = r.scanouts;
    out->rdp_scanout_ns = r.scanout_ns;
    out->rdp_stalled_compiles = r.stalled_compiles;
    out->rdp_stalled_compile_ns = r.stalled_compile_ns;
    out->rdp_dp_waits = r.dp_waits;
    out->rdp_dp_wait_ns = r.dp_wait_ns;
    out->rdp_prefetches = r.prefetches;
    out->rdp_prefetch_hits = r.prefetch_hits;
}

/* One slice timed stage by stage (one frame in perf_every): the same work as below. The RDP runs the CPU's and the
 * RSP's DPC_END writes start inside n64_cpu_run / n64_rsp_run (n64_rdp_time_runs, on for the timed frame) are taken
 * out of their stage and counted as the RDP's. */
static void slice_timed(n64_machine *m, bool rsp_on)
{
    uint64_t r0 = n64_rdp_run_ns(m->rdp);
    uint64_t t0 = perf_ns();
    if (m->now < m->stop_at)
        n64_cpu_run(m);
    uint64_t t1 = perf_ns();
    uint64_t r1 = n64_rdp_run_ns(m->rdp);
    m->perf.cpu_ns += (t1 - t0) - (r1 - r0);
    if (rsp_on) {
        uint64_t d = (m->now - m->rsp_tick) / 3;
        while (d > 0 && !n64_rsp_halted(m->rsp)) {
            uint32_t n = d > 1000000 ? 1000000 : (uint32_t)d;
            m->perf.rsp_instructions += n64_rsp_run(m->rsp, n);
            d -= n;
        }
        m->rsp_tick = m->now;
        uint64_t t2 = perf_ns();
        uint64_t r2 = n64_rdp_run_ns(m->rdp);
        n64_rdp_run(m->rdp);
        uint64_t r3 = n64_rdp_run_ns(m->rdp);
        m->perf.rsp_ns += (t2 - t1) - (r2 - r1);
        m->perf.rdp_ns += r3 - r0;
    } else {
        m->rsp_tick = m->now;
        m->perf.rdp_ns += r1 - r0;
    }
}

static void slice(n64_machine *m, uint64_t end)
{
    uint64_t stop = end;
    for (int i = 0; i < EV_COUNT; i++)
        if (m->ev[i] < stop)
            stop = m->ev[i];
    bool rsp_on = !n64_rsp_halted(m->rsp) && !(m->boot_x105 && n64_pif_x105_hold(m));
    if (rsp_on && stop > m->now + RSP_SLICE)
        stop = m->now + RSP_SLICE;
    m->stop_at = stop;
    if (N64_UNLIKELY(m->perf_timing)) {
        slice_timed(m, rsp_on);
    } else {
        if (m->now < stop)
            n64_cpu_run(m);
        if (rsp_on) {
            uint64_t d = (m->now - m->rsp_tick) / 3;
            while (d > 0 && !n64_rsp_halted(m->rsp)) {
                uint32_t n = d > 1000000 ? 1000000 : (uint32_t)d;
                m->perf.rsp_instructions += n64_rsp_run(m->rsp, n);
                d -= n;
            }
            m->rsp_tick = m->now;
            n64_rdp_run(m->rdp);
        } else {
            m->rsp_tick = m->now; /* halted, or started during this slice: it runs from now */
        }
    }
    if (N64_UNLIKELY(m->boot_x105))
        n64_pif_x105_step(m);
    dispatch(m);
}

void n64_run_frame(n64_machine *m)
{
    m->perf.frames++;
    uint64_t t0 = 0;
    if (m->perf_every && ++m->perf_count >= m->perf_every) {
        m->perf_count = 0;
        m->perf_timing = true;
        n64_rdp_time_runs(m->rdp, 1);
        t0 = perf_ns();
    }
    m->frame_done = false;
    uint64_t cap = m->now + m->vi_line_ticks * (uint64_t)(m->vi_lines + 2) * 2;
    while (!m->frame_done && m->now < cap)
        slice(m, cap);
    n64_rdp_frame_end(m->rdp); /* the software RDP's pixels in RDRAM at an emulated moment, shown or not */
    n64_ai_advance(m);
    if (N64_UNLIKELY(m->perf_timing)) {
        m->perf_timing = false;
        n64_rdp_time_runs(m->rdp, 0);
        m->perf.timed++;
        m->perf.frame_ns += perf_ns() - t0;
    }
}

void n64_run_cycles(n64_machine *m, uint64_t cycles)
{
    uint64_t end = m->now + cycles * N64_TICKS_PER_CYCLE;
    while (m->now < end)
        slice(m, end);
    n64_ai_advance(m);
}

/* ---- Accessors ------------------------------------------------------------------------------------------------- */

static const uint8_t *peek_ptr(n64_machine *m, uint32_t addr, uint32_t n)
{
    uint32_t pa;
    if (addr >= 0x80000000u && addr < 0xC0000000u)
        pa = addr & 0x1FFFFFFFu;
    else if (addr < 0x20000000u)
        pa = addr;
    else
        return NULL;
    if (pa + n <= m->rdram_size)
        return m->rdram + pa;
    if (pa >= 0x04000000u && pa < 0x04040000u)
        return m->spmem + (pa & 0x1FFF);
    if (pa >= 0x10000000u && pa - 0x10000000u + n <= m->rom_size)
        return m->rom + (pa - 0x10000000u);
    if (pa >= 0x1FC007C0u && pa + n <= 0x1FC00800u)
        return m->pif_ram + (pa - 0x1FC007C0u);
    return NULL;
}

uint32_t n64_peek32(n64_machine *m, uint32_t addr)
{
    const uint8_t *p = peek_ptr(m, addr & ~3u, 4);
    return p ? n64_be32(p) : 0;
}

uint16_t n64_peek16(n64_machine *m, uint32_t addr)
{
    const uint8_t *p = peek_ptr(m, addr & ~1u, 2);
    return p ? n64_be16(p) : 0;
}

uint8_t n64_peek8(n64_machine *m, uint32_t addr)
{
    const uint8_t *p = peek_ptr(m, addr, 1);
    return p ? *p : 0;
}

void n64_poke32(n64_machine *m, uint32_t addr, uint32_t v)
{
    uint8_t *p = (uint8_t *)peek_ptr(m, addr & ~3u, 4);
    if (p && (p < m->rom || p >= m->rom + m->rom_size) && (p < m->pif_ram || p >= m->pif_ram + 64)) {
        n64_wbe32(p, v);
        if (p >= m->rdram && p < m->rdram + m->rdram_size)
            n64_rdram_wrote(m, (uint32_t)(p - m->rdram), 4);
    }
}

uint64_t n64_reg(n64_machine *m, int r) { return r >= 0 && r < 32 ? m->cpu.gpr[r] : 0; }
uint64_t n64_pc(n64_machine *m) { return m->cpu.pc; }
uint64_t n64_cop0(n64_machine *m, int r)
{
    if (r == C0_COUNT)
        return n64_cpu_count(m);
    return r >= 0 && r < 32 ? m->cpu.cop0[r] : 0;
}
uint64_t n64_fpr(n64_machine *m, int r) { return r >= 0 && r < 32 ? m->cpu.fpr[r] : 0; }
uint32_t n64_fcsr(n64_machine *m) { return m->cpu.fcsr; }
uint64_t n64_cycles(n64_machine *m) { return m->now / N64_TICKS_PER_CYCLE; }
uint64_t n64_frames(n64_machine *m) { return m->frames; }
uint8_t *n64_rdram(n64_machine *m)
{
    for (uint32_t p = 0; p < (m->rdram_size >> N64_PAGE_SHIFT); p++)
        m->rdram_gen[p]++; /* the caller may write code there: predecoded blocks and lifted code check it again */
    return m->rdram;
}
uint32_t n64_rdram_size(n64_machine *m) { return m->rdram_size; }
uint64_t n64_instructions(n64_machine *m) { return m->instructions; }

const n64_guest_fault *n64_guest_fault_info(n64_machine *m) { return &m->fault; }

void n64_guest_activity(n64_machine *m, n64_activity *out)
{
    out->vi_origin_changes = m->vi_origin_changes;
    out->si_dmas = m->si_dmas;
    out->pi_dmas = m->pi_dmas;
    out->ai_buffers = m->ai_buffers;
}

void n64_set_tty(n64_machine *m, void (*fn)(void *ctx, char c), void *ctx)
{
    m->tty = fn;
    m->tty_ctx = ctx;
}

struct n64_rsp *n64_get_rsp(n64_machine *m) { return m->rsp; }
struct n64_rdp *n64_get_rdp(n64_machine *m) { return m->rdp; }
