/* VI and AI (N-4, XBA-172).
 *
 * VI: the registers, a line counter timed from H_SYNC (VI clocks per line) and V_SYNC (half-lines per frame), the
 * VI interrupt when the counter reaches V_INTR, the field bit for interlaced modes. The picture (n64_display, the
 * scaler, the filters, gamma, interlace) is n64_vi.c.
 *
 * AI: the double-buffered DMA from RDRAM (two buffers queued, the AI interrupt when one starts playing), the DAC rate
 * (VI clock / (DACRATE + 1)), 16-bit big-endian stereo samples, resampled to 48 kHz by a band-limited windowed-sinc
 * interpolator (below) into a ring that n64_audio_pull empties. */
#include "n64_internal.h"
#include "n64_rdp.h"

#include <math.h>
#include <string.h>

#define AI_PI 3.14159265358979323846

static uint32_t vi_clock(const n64_machine *m)
{
    return m->pal ? 49656530u : 48681812u;
}

/* ---- VI -------------------------------------------------------------------------------------------------------- */

static void vi_timing(n64_machine *m)
{
    uint32_t hsync = m->vi[7] & 0xFFF, vsync = m->vi[6] & 0x3FF;
    if (hsync == 0)
        hsync = m->pal ? 3177 : 3093;
    if (vsync < 16)
        vsync = m->pal ? 625 : 525;
    m->vi_line_ticks = (uint64_t)(hsync + 1) * N64_TICK_HZ / vi_clock(m);
    m->vi_lines = (vsync + 1) / 2;
    if (m->vi_line >= m->vi_lines)
        m->vi_line = 0;
}

void n64_vi_reset(n64_machine *m)
{
    memset(m->vi, 0, sizeof m->vi);
    m->vi_line = 0;
    m->vi_field = 0;
    vi_timing(m);
    n64_schedule(m, EV_VI, m->now + m->vi_line_ticks);
}

uint32_t n64_vi_read(n64_machine *m, uint32_t reg)
{
    if (reg == 4)
        return ((m->vi_line << 1) | m->vi_field) & 0x3FF;
    return m->vi[reg];
}

void n64_vi_write(n64_machine *m, uint32_t reg, uint32_t v)
{
    switch (reg) {
    case 0: m->vi[0] = v & 0x1FBFF; break;
    case 1:
        if ((v & 0x00FFFFFF) != m->vi[1]) {
            m->vi_origin_changes++;
            n64_dp_origin(m);
            m->vi[1] = v & 0x00FFFFFF;
            n64_vi_origin_changed(m);
        }
        m->vi[1] = v & 0x00FFFFFF;
        break;
    case 2: m->vi[2] = v & 0xFFF; break;
    case 3: m->vi[3] = v & 0x3FF; break;
    case 4: n64_mi_clear(m, N64_MI_VI); break;
    case 6: m->vi[6] = v & 0x3FF; vi_timing(m); break;
    case 7: m->vi[7] = v & 0x1FFFFF; vi_timing(m); break;
    default: m->vi[reg] = v; break;
    }
}

void n64_vi_event(n64_machine *m)
{
    uint32_t intr_line = (m->vi[3] & 0x3FF) >> 1;
    m->vi_line++;
    if (m->vi_line >= m->vi_lines) {
        m->vi_line = 0;
        if (m->vi[0] & 0x40)
            m->vi_field ^= 1;
        else
            m->vi_field = 0;
        m->frames++;
        if (intr_line >= m->vi_lines)
            m->frame_done = true; /* V_INTR out of range: a field is still a frame */
    }
    if (m->vi_line == intr_line) {
        n64_dp_vi(m, m->ev[EV_VI]);
        n64_mi_raise(m, N64_MI_VI);
        m->frame_done = true;
    }
    n64_schedule(m, EV_VI, m->ev[EV_VI] + m->vi_line_ticks);
    n64_rdp_run(m->rdp);
}

/* ---- AI -------------------------------------------------------------------------------------------------------- */

/* The resampler: a windowed-sinc interpolator (AI_TAPS taps, Kaiser window, beta 8) evaluated at the output
 * sample's position between the DAC's samples, in AI_PHASES polyphase rows (linear interpolation between two rows).
 * Its cutoff is 0.95 of the lower Nyquist frequency (the DAC's, or 24 kHz when the DAC runs faster than 48 kHz), so
 * the DAC's images above it are gone (sample and hold left them all in). Each row sums to 1. Rebuilt when the DAC
 * rate changes; the history keeps going through a change (the samples are just spaced differently from then on), so
 * a rate change or a gap between buffers (the DAC holds its last sample, as the hardware does) makes no click. The
 * cost: a delay of AI_TAPS / 2 DAC samples (0.5 ms at 32 kHz) and 64 multiply-adds per output frame. */
static double bessel_i0(double x)
{
    double sum = 1, term = 1;
    for (int k = 1; k < 40; k++) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
        if (term < sum * 1e-12)
            break;
    }
    return sum;
}

static void ai_build_kernel(n64_machine *m, uint32_t dac_hz)
{
    const double beta = 8.0, half = AI_TAPS / 2;
    double fc = dac_hz > N64_AUDIO_RATE ? (double)N64_AUDIO_RATE / dac_hz : 1.0; /* of the DAC's Nyquist */
    fc *= 0.95;
    double i0b = bessel_i0(beta);
    for (int ph = 0; ph <= AI_PHASES; ph++) {
        double frac = (double)ph / AI_PHASES, sum = 0;
        float *row = m->ai_kernel + ph * AI_TAPS;
        for (int k = 0; k < AI_TAPS; k++) {
            /* Tap k weighs history sample (base - AI_TAPS / 2 + 1 + k) for a position base + frac. */
            double t = (double)(k - (AI_TAPS / 2 - 1)) - frac;
            double x = t * fc;
            double sinc = x == 0 ? 1.0 : sin(AI_PI * x) / (AI_PI * x);
            double r = t / half;
            double win = r * r < 1 ? bessel_i0(beta * sqrt(1 - r * r)) / i0b : 0;
            row[k] = (float)(fc * sinc * win);
            sum += row[k];
        }
        for (int k = 0; k < AI_TAPS; k++)
            row[k] = (float)(row[k] / sum);
    }
    m->ai_kernel_hz = dac_hz;
}

void n64_ai_reset(n64_machine *m)
{
    m->ai_dram = m->ai_len = m->ai_control = m->ai_dacrate = m->ai_bitrate = 0;
    m->ai_count = 0;
    m->ai_pos = 0;
    m->ai_tick = m->now;
    m->ai_src_phase = 0;
    m->ai_out_phase = 0;
    m->ai_cur[0] = m->ai_cur[1] = 0;
    memset(m->ai_hist, 0, sizeof m->ai_hist);
    m->ai_nsrc = 0;
    m->ai_kernel_hz = 0;
    m->audio_rd = m->audio_wr = 0;
    m->ev[EV_AI] = EV_NEVER;
}

static double src_period(const n64_machine *m)
{
    /* ticks per sample frame: the DAC rate, 48 kHz before the game sets one */
    if (m->ai_dacrate == 0)
        return (double)N64_TICK_HZ / N64_AUDIO_RATE;
    return (double)N64_TICK_HZ * (double)(m->ai_dacrate + 1) / (double)vi_clock(m);
}

/* EV_AI at the DAC's pull that finds the playing buffer used up (its next pull is sp - phase away). */
static void ai_schedule_end(n64_machine *m, uint64_t at)
{
    double sp = src_period(m);
    uint32_t left = m->ai_pos < m->ai_fifo[0].len ? (m->ai_fifo[0].len - m->ai_pos) / 4 : 0;
    double dt = sp - m->ai_src_phase + (double)left * sp;
    n64_schedule(m, EV_AI, at + (uint64_t)(dt > 0 ? dt : 0) + 1);
}

/* A buffer starts playing (AI_LEN with the AI idle, or the next queued one): the AI interrupt. */
static void ai_start(n64_machine *m, uint64_t at)
{
    m->ai_pos = 0;
    ai_schedule_end(m, at);
    n64_mi_raise(m, N64_MI_AI);
}

/* The DAC takes its next sample: the playing buffer's next one (the next buffer once one is used up: the samples run
 * on without a gap or a skipped sample), or the one it holds when none is queued. */
static void ai_src_push(n64_machine *m, uint64_t at)
{
    if (m->ai_count > 0 && m->ai_pos + 4 > m->ai_fifo[0].len) {
        m->ai_fifo[0] = m->ai_fifo[1];
        m->ai_count--;
        m->ev[EV_AI] = EV_NEVER;
        if (m->ai_count > 0)
            ai_start(m, at);
    }
    if (m->ai_count > 0 && m->ai_pos + 4 <= m->ai_fifo[0].len) {
        uint32_t a = (m->ai_fifo[0].addr + m->ai_pos) & 0x00FFFFFCu;
        if (a + 4 <= m->rdram_size) {
            m->ai_cur[0] = (int16_t)n64_be16(m->rdram + a);
            m->ai_cur[1] = (int16_t)n64_be16(m->rdram + a + 2);
        }
        m->ai_pos += 4;
    }
    uint32_t i = m->ai_nsrc & (AI_HIST - 1);
    m->ai_hist[0][i] = m->ai_cur[0];
    m->ai_hist[1][i] = m->ai_cur[1];
    m->ai_nsrc++;
}

/* An output sample at frac (0..1) of the way from the newest DAC sample to the next, AI_TAPS / 2 samples late. */
static void push_out(n64_machine *m, double frac)
{
    if (frac < 0)
        frac = 0;
    double fp = frac * AI_PHASES;
    int ph = (int)fp;
    if (ph >= AI_PHASES)
        ph = AI_PHASES - 1;
    float w = (float)(fp - ph);
    const float *k0 = m->ai_kernel + ph * AI_TAPS, *k1 = k0 + AI_TAPS;
    /* The taps first (independent: they vectorise), then the two sums in tap order, the history ring walked as its
     * two runs instead of masking every index: the same operations in the same order as one loop (XBA-191). */
    float coef[AI_TAPS];
    for (int k = 0; k < AI_TAPS; k++)
        coef[k] = k0[k] + (k1[k] - k0[k]) * w;
    uint32_t base = (m->ai_nsrc - AI_TAPS) & (AI_HIST - 1); /* the oldest sample the taps read */
    int run = AI_HIST - (int)base < AI_TAPS ? AI_HIST - (int)base : AI_TAPS;
    const float *h0 = m->ai_hist[0], *h1 = m->ai_hist[1];
    float acc[2] = {0, 0};
    for (int k = 0; k < run; k++) {
        acc[0] += coef[k] * h0[base + (uint32_t)k];
        acc[1] += coef[k] * h1[base + (uint32_t)k];
    }
    for (int k = run; k < AI_TAPS; k++) {
        acc[0] += coef[k] * h0[k - run];
        acc[1] += coef[k] * h1[k - run];
    }
    if (m->audio_wr - m->audio_rd >= N64_AUDIO_RING)
        m->audio_rd++; /* full: drop the oldest */
    uint32_t o = m->audio_wr & (N64_AUDIO_RING - 1);
    for (int c = 0; c < 2; c++) {
        float v = acc[c] + (acc[c] >= 0 ? 0.5f : -0.5f);
        m->audio[2 * o + c] = (int16_t)(v > 32767.f ? 32767 : v < -32768.f ? -32768 : (int)v);
    }
    m->audio_wr++;
}

void n64_ai_advance(n64_machine *m)
{
    if (m->now <= m->ai_tick)
        return;
    double t = (double)(m->now - m->ai_tick);
    m->ai_tick = m->now;
    const double out_period = (double)N64_TICK_HZ / N64_AUDIO_RATE;
    /* The DAC's sample period: the rate the game set, kept while idle (the DAC holds its last sample at that rate,
     * so the filter's history stays evenly spaced); 48 kHz before any. */
    double sp = src_period(m);
    uint32_t hz = m->ai_dacrate > 0 ? n64_audio_source_rate(m) : N64_AUDIO_RATE;
    if (hz != m->ai_kernel_hz)
        ai_build_kernel(m, hz);
    while (t > 0) {
        double to_out = out_period - m->ai_out_phase;
        double to_src = sp - m->ai_src_phase;
        double step = to_out < to_src ? to_out : to_src;
        if (step > t)
            step = t;
        if (step < 0)
            step = 0;
        t -= step;
        m->ai_out_phase += step;
        m->ai_src_phase += step;
        if (m->ai_src_phase >= sp) {
            m->ai_src_phase -= sp;
            if (m->ai_src_phase >= sp) /* the rate went up a lot: start over on this sample */
                m->ai_src_phase = 0;
            ai_src_push(m, m->now - (uint64_t)t);
        }
        if (m->ai_out_phase >= out_period) {
            m->ai_out_phase -= out_period;
            push_out(m, m->ai_src_phase / sp);
        }
    }
}

void n64_ai_event(n64_machine *m)
{
    /* The DAC's pulls switch buffers (ai_src_push); this event only makes sure the CPU sees it on time. */
    m->ev[EV_AI] = EV_NEVER;
    n64_ai_advance(m);
    if (m->ai_count > 0 && m->ev[EV_AI] == EV_NEVER)
        ai_schedule_end(m, m->now); /* rounding: the pull is a tick or two away */
}

uint32_t n64_ai_read(n64_machine *m, uint32_t reg)
{
    n64_ai_advance(m);
    switch (reg) {
    case 1: /* remaining bytes of the buffer playing */
        if (m->ai_count == 0)
            return 0;
        return (m->ai_fifo[0].len - (m->ai_pos < m->ai_fifo[0].len ? m->ai_pos : m->ai_fifo[0].len)) & 0x3FFF8u;
    case 3: {
        uint32_t s = 0x00100000u | 0x01000000u; /* as the hardware reports */
        if (m->ai_count >= 2)
            s |= 0x80000001u;
        if (m->ai_count > 0)
            s |= 0x40000000u;
        if (m->ai_control & 1)
            s |= 0x02000000u;
        return s;
    }
    default:
        return m->ai_len;
    }
}

void n64_ai_write(n64_machine *m, uint32_t reg, uint32_t v)
{
    n64_ai_advance(m);
    switch (reg) {
    case 0: m->ai_dram = v & 0x00FFFFF8u; break;
    case 1:
        m->ai_len = v & 0x3FFF8u;
        if (m->ai_len)
            m->ai_buffers++;
        if (m->ai_len && m->ai_count < 2) {
            m->ai_fifo[m->ai_count].addr = m->ai_dram;
            m->ai_fifo[m->ai_count].len = m->ai_len;
            m->ai_count++;
            if (m->ai_count == 1)
                ai_start(m, m->now);
        }
        break;
    case 2: m->ai_control = v & 1; break;
    case 3: n64_mi_clear(m, N64_MI_AI); break;
    case 4: m->ai_dacrate = v & 0x3FFF; break;
    case 5: m->ai_bitrate = v & 0xF; break;
    default: break;
    }
}

size_t n64_audio_pull(n64_machine *m, int16_t *stereo, size_t frames)
{
    size_t n = 0;
    while (n < frames && m->audio_rd != m->audio_wr) {
        uint32_t i = m->audio_rd & (N64_AUDIO_RING - 1);
        stereo[2 * n] = m->audio[2 * i];
        stereo[2 * n + 1] = m->audio[2 * i + 1];
        m->audio_rd++;
        n++;
    }
    return n;
}

uint32_t n64_audio_source_rate(n64_machine *m)
{
    return m->ai_dacrate ? vi_clock(m) / (m->ai_dacrate + 1) : 0;
}
