/* The VI's picture path (n64_display; XBA-172): what a television showed, not a raw crop of the framebuffer.
 *
 * Our own code, from the n64brew wiki's VI pages and the VI's documented behaviour (as ares (ISC) and angrylion's
 * renderer describe it); nothing copied. The registers (n64_io.c keeps them): CTRL (type, gamma dither, gamma, divot,
 * serrate, AA mode, dither filter), ORIGIN, WIDTH (the line stride in pixels), H_VIDEO / V_VIDEO (the active area, in
 * VI pixels and half-lines), X_SCALE / Y_SCALE (2.10 step per output pixel / line, and a 2.10 start offset in the
 * upper half).
 *
 * The output is a fixed "television" canvas: 640 VI pixels from H = 108 (NTSC) / 128 (PAL) and 240 / 288 lines (480
 * / 576 interlaced) from half-line 34 / 44, which holds the active area of every libultra and libdragon mode. The
 * active area is placed on it where H_VIDEO / V_VIDEO put it and the rest is black, so a narrow or short picture keeps
 * its aspect and position (the runtime shows the canvas at 4:3). When X_SCALE is 0.5 or less (a framebuffer 320 or
 * fewer pixels wide) the canvas is sampled at every other VI pixel: 320 wide, the framebuffer's own pixels when the
 * scale is exactly 0.5, so a 320 x 240 game comes out 320 x 240.
 *
 * Per output pixel, as the VI does:
 *   - fetch: 16-bit RGBA 5551 (each channel 5 -> 8 bits by bit replication) with its coverage (3 bits: the pixel's
 *     low bit and the two hidden RDRAM bits the RDP keeps, n64_rdp_hidden_bits; without them a set low bit is full
 *     coverage, a clear one none), or 32-bit RGBA 8888 (coverage in the alpha's top 3 bits).
 *   - AA modes 0 / 1 (CTRL bits 8-9): the anti-aliasing filter on partially covered pixels: the "background" colour
 *     is estimated from the fully covered neighbours (left, right and the four diagonals) by the penultimate maximum
 *     and minimum of each channel, and the pixel moves towards it by (7 - coverage) / 8. Then the divot filter (CTRL
 *     bit 4): a pixel among partially covered horizontal neighbours takes the median of the three per channel.
 *   - dither filter (CTRL bit 16, 16-bit modes, fully covered pixels): the 5-bit dither is undone by the 3 x 3
 *     neighbours within one step (each pulls the 8-bit value by one).
 *   - scaler: AA modes 0..2 interpolate bilinearly between framebuffer pixels (5-bit fractions of the 2.10 positions,
 *     the next line too), mode 3 replicates (nearest).
 *   - gamma (CTRL bit 3): the VI's square-root curve on 8 bits + 6 bits of dither when gamma dither (bit 2) is on;
 *     gamma dither alone adds one bit of noise. The noise is a hash of the position and the frame, so a picture is
 *     a function of the machine's state (the recompiler's lockstep compares pictures).
 *   - interlace (CTRL bit 6, serrate): weave. The canvas keeps both fields; each call draws the field the VI scans
 *     now into its rows (even or odd). Which one is the lower field comes from the framebuffer position of the first
 *     line (ORIGIN / WIDTH and the Y offset: the field 0.5 line or one line further down is the odd one), else the
 *     VI's field bit. A game that renders 480 lines and shows them as two fields gets its picture back whole, no
 *     combing and no bobbing; one that shows the same 240 lines in both fields gets them line-doubled.
 * Horizontal guard band: the first 8 and last 7 VI pixels of the active area are black (not when that edge is clamped
 * to the scan-out), so a standard 640-pixel mode shows VI pixels 8..632 (canvas columns 4..316 at 320 wide).
 * Upscaled (XBA-185): with the Vulkan RDP drawing at 2x / 4x / 8x, n64_display returns its VI's scanout instead
 * (paraLLEl-RDP's VI: the same canvas, 640 x 240 / 288 VI pixels times the scale, interlace by upscaled bob rather
 * than weave) when the buffer is large enough for it.
 * Not modelled: the VI's fetch bugs, the AA filter's exact neighbour set (an approximation),
 * per-line register changes within a field (the picture uses the registers at the call). */
#include "n64_internal.h"
#include "n64_rdp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CANVAS_W 640
#define LINE_MAX 4096 /* framebuffer pixels decoded per line at most */
#define FLINES 4      /* filtered framebuffer lines kept */

typedef struct vi_line {
    int32_t y;        /* framebuffer line, or INT32_MIN: empty */
    int n;            /* pixels */
    uint8_t *rgb;     /* 3 bytes per pixel (+ 1 spare pixel for the interpolation) */
} vi_line;

struct n64_vi_state {
    uint32_t canvas[CANVAS_W * N64_SCREEN_MAX_H];
    int cw, ch;          /* canvas size of the last picture */
    int interlaced;
    int64_t last_pos;    /* the last field's first-line position (2.10 framebuffer lines), INT64_MIN: none */
    uint8_t gamma[1 << 14]; /* 8.6 bits in -> 8 bits out */
    /* Decoding: raw lines (r, g, b, coverage per pixel) for the filters, and filtered lines. */
    int32_t raw_y[3];
    int raw_n;
    uint8_t raw[3][(LINE_MAX + 4) * 4];
    vi_line fl[FLINES];
    uint8_t flbuf[FLINES][(LINE_MAX + 4) * 3];
    int fl_next;
    int32_t xpos[CANVAS_W];
};

static struct n64_vi_state *vi_state(n64_machine *m)
{
    if (!m->vis) {
        struct n64_vi_state *v = calloc(1, sizeof *v);
        if (!v)
            return NULL;
        for (int i = 0; i < (1 << 14); i++) {
            int g = (int)lrint(sqrt((double)i / 16383.0) * 255.0);
            v->gamma[i] = (uint8_t)(g > 255 ? 255 : g);
        }
        v->last_pos = INT64_MIN;
        m->vis = v;
    }
    return m->vis;
}

void n64_vi_free(n64_machine *m)
{
    free(m->vis);
    m->vis = NULL;
}

/* The fetch context of one picture. */
typedef struct vi_ctx {
    const uint8_t *ram;
    uint32_t ram_size;
    const uint8_t *hidden;
    uint32_t origin, stride; /* bytes */
    int bpp32;
    int npix;                /* pixels decoded per line */
    int aa, divot, dfilter;
} vi_ctx;

static inline uint8_t x5(uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); }

/* Raw line y (clamped to >= 0): r, g, b, coverage per pixel. */
static const uint8_t *raw_line(struct n64_vi_state *v, const vi_ctx *c, int32_t y)
{
    if (y < 0)
        y = 0;
    for (int i = 0; i < 3; i++)
        if (v->raw_y[i] == y)
            return v->raw[i];
    int slot = v->raw_n;
    v->raw_n = (v->raw_n + 1) % 3;
    v->raw_y[slot] = y;
    uint8_t *o = v->raw[slot];
    uint32_t base = c->origin + (uint32_t)y * c->stride;
    int n = c->npix;
    if (c->bpp32) {
        for (int x = 0; x < n; x++, o += 4) {
            uint32_t a = base + (uint32_t)x * 4;
            if (a + 4 > c->ram_size) {
                o[0] = o[1] = o[2] = o[3] = 0;
                continue;
            }
            const uint8_t *p = c->ram + a;
            o[0] = p[0];
            o[1] = p[1];
            o[2] = p[2];
            o[3] = p[3] >> 5;
        }
    } else {
        for (int x = 0; x < n; x++, o += 4) {
            uint32_t a = base + (uint32_t)x * 2;
            if (a + 2 > c->ram_size) {
                o[0] = o[1] = o[2] = o[3] = 0;
                continue;
            }
            uint32_t p = n64_be16(c->ram + a);
            o[0] = x5((p >> 11) & 31);
            o[1] = x5((p >> 6) & 31);
            o[2] = x5((p >> 1) & 31);
            if (c->hidden)
                o[3] = (uint8_t)((p & 1) << 2 | (c->hidden[a >> 1] & 3));
            else
                o[3] = (p & 1) ? 7 : 0;
        }
    }
    /* One spare pixel (the last repeated) so the interpolation can read x + 1. */
    memcpy(v->raw[slot] + (size_t)n * 4, v->raw[slot] + (size_t)(n ? n - 1 : 0) * 4, 4);
    return v->raw[slot];
}

static inline int median3(int a, int b, int c)
{
    int lo = a < b ? a : b, hi = a < b ? b : a;
    return c < lo ? lo : c > hi ? hi : c;
}

/* Penultimate maximum and minimum of n values (n >= 1). */
static inline void penult(const int *val, int n, int *pmax, int *pmin)
{
    if (n == 1) {
        *pmax = *pmin = val[0];
        return;
    }
    int max1 = -1, max2 = -1, min1 = 256, min2 = 256;
    for (int i = 0; i < n; i++) {
        int x = val[i];
        if (x > max1) {
            max2 = max1;
            max1 = x;
        } else if (x > max2) {
            max2 = x;
        }
        if (x < min1) {
            min2 = min1;
            min1 = x;
        } else if (x < min2) {
            min2 = x;
        }
    }
    *pmax = max2;
    *pmin = min2;
}

/* Filtered line y: AA (+ divot) on partial pixels, the dither filter on full ones, r g b. */
static const uint8_t *filtered_line(struct n64_vi_state *v, const vi_ctx *c, int32_t y)
{
    if (y < 0)
        y = 0;
    for (int i = 0; i < FLINES; i++)
        if (v->fl[i].y == y)
            return v->fl[i].rgb;
    vi_line *L = &v->fl[v->fl_next];
    v->fl_next = (v->fl_next + 1) % FLINES;
    L->y = y;
    L->n = c->npix;
    uint8_t *o = L->rgb;
    int n = c->npix;
    const uint8_t *cur = raw_line(v, c, y);
    if (!c->aa && !c->dfilter) {
        for (int x = 0; x <= n; x++) {
            o[3 * x] = cur[4 * x];
            o[3 * x + 1] = cur[4 * x + 1];
            o[3 * x + 2] = cur[4 * x + 2];
        }
        return o;
    }
    const uint8_t *up = raw_line(v, c, y - 1);
    const uint8_t *dn = raw_line(v, c, y + 1);
    cur = raw_line(v, c, y); /* still cached: three slots */
    for (int x = 0; x < n; x++) {
        const uint8_t *p = cur + 4 * x;
        int cvg = p[3];
        int xl = x > 0 ? x - 1 : 0, xr = x + 1 < n ? x + 1 : n - 1;
        if (cvg == 7 || !c->aa) {
            if (c->dfilter && !c->bpp32 && cvg == 7) {
                /* Undo the 5-bit dither: each 3 x 3 neighbour one 5-bit step above / below pulls by one. */
                const uint8_t *nb[8] = {up + 4 * xl, up + 4 * x, up + 4 * xr, cur + 4 * xl,
                                        cur + 4 * xr, dn + 4 * xl, dn + 4 * x, dn + 4 * xr};
                for (int ch = 0; ch < 3; ch++) {
                    int c5 = p[ch] >> 3, acc = 0;
                    for (int k = 0; k < 8; k++) {
                        int d = (nb[k][ch] >> 3) - c5;
                        acc += d == 1 ? 1 : d == -1 ? -1 : 0;
                    }
                    int val = p[ch] + acc;
                    o[3 * x + ch] = (uint8_t)(val < 0 ? 0 : val > 255 ? 255 : val);
                }
            } else {
                o[3 * x] = p[0];
                o[3 * x + 1] = p[1];
                o[3 * x + 2] = p[2];
            }
            continue;
        }
        /* AA: background from the fully covered neighbours. */
        const uint8_t *nb[6] = {up + 4 * xl, up + 4 * xr, cur + 4 * xl, cur + 4 * xr, dn + 4 * xl, dn + 4 * xr};
        int coeff = 7 - cvg;
        for (int ch = 0; ch < 3; ch++) {
            int val[7], k = 0;
            val[k++] = p[ch];
            for (int j = 0; j < 6; j++)
                if (nb[j][3] == 7)
                    val[k++] = nb[j][ch];
            int pmax, pmin;
            penult(val, k, &pmax, &pmin);
            int col = pmax + pmin - 2 * p[ch];
            int r = p[ch] + ((col * coeff + 4) >> 3);
            o[3 * x + ch] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
        }
    }
    if (c->aa && c->divot && n >= 3) {
        /* Divot: the median of three where coverage is partial, on the AA output (a copy of the line first). */
        uint8_t row[(LINE_MAX + 4) * 3];
        memcpy(row, o, (size_t)n * 3);
        for (int x = 1; x + 1 < n; x++) {
            if ((cur[4 * (x - 1) + 3] & cur[4 * x + 3] & cur[4 * (x + 1) + 3]) == 7)
                continue;
            for (int ch = 0; ch < 3; ch++)
                o[3 * x + ch] = (uint8_t)median3(row[3 * (x - 1) + ch], row[3 * x + ch], row[3 * (x + 1) + ch]);
        }
    }
    memcpy(o + (size_t)n * 3, o + (size_t)(n ? n - 1 : 0) * 3, 3);
    return o;
}

static inline uint32_t noise(uint32_t x, uint32_t y, uint32_t f)
{
    uint32_t h = x * 0x9E3779B1u ^ y * 0x85EBCA77u ^ f * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}

static void vi_read_range(const n64_machine *m, uint32_t *lo, uint32_t *hi);
static void vi_lines_range(const n64_machine *m, uint32_t *lo, uint32_t *hi, int strict);
static uint64_t non_rcp_gen(const n64_machine *m, uint32_t lo, uint32_t hi);

/* VI_ORIGIN changed (n64_io.c): the Vulkan RDP may start this picture's upscaled scanout now, while nothing is queued
 * in front of it on the GPU (n64_rdp.h n64_rdp_scanout_prefetch); by the end of the VI frame the game has usually
 * queued its next picture, which a scanout would wait for. Only for a runtime that shows the upscaled scanout (the last
 * n64_display used it), progressive modes, and with the deferred DP interrupt on (RECOMP_N64_RDP_DEFER=0 turns both
 * off; RECOMP_N64_SCANOUT_EARLY=0 this one alone, to compare). Host-side only. */
void n64_vi_origin_changed(n64_machine *m)
{
    static int early = -1;
    if (early < 0) {
        const char *e = getenv("RECOMP_N64_SCANOUT_EARLY");
        early = !(e && *e && atoi(e) == 0);
    }
    m->pf.on = false;
    if (!early || m->dpp.mode != N64_DP_DEFER_AUTO || !m->disp.valid || m->disp.sscale <= 0 || (m->vi[0] & 0x40))
        return;
    uint32_t lo, hi;
    vi_read_range(m, &lo, &hi);
    if (hi <= lo)
        return;
    m->pf.on = true;
    m->pf.lo = lo;
    m->pf.hi = hi;
    m->pf.gen = non_rcp_gen(m, lo, hi);
    /* The RDP's rows since: tested against the lines shown only (the line under the last one, which the VI blends in
     * with some weight, is often the next buffer's first, drawn from the start of the next picture: the scanout then
     * blends in what was there at the VI_ORIGIN change). */
    uint32_t slo, shi;
    vi_lines_range(m, &slo, &shi, 1);
    n64_rdp_scanout_prefetch(m->rdp, m->vi, m->vi_field, slo, shi);
}

static int display_picture(n64_machine *m, uint32_t *xrgb, int max_w, int max_h, int *w, int *h)
{
    uint32_t ctrl = m->vi[0], type = ctrl & 3;
    uint32_t origin = m->vi[1] & 0xFFFFFF, width = m->vi[2] & 0xFFF;
    int hs = (int)((m->vi[9] >> 16) & 0x3FF), he = (int)(m->vi[9] & 0x3FF);
    int vs = (int)((m->vi[10] >> 16) & 0x3FF), ve = (int)(m->vi[10] & 0x3FF);
    uint32_t xs = m->vi[12] & 0xFFF, ys = m->vi[13] & 0xFFF;
    uint32_t xoff = (m->vi[12] >> 16) & 0xFFF, yoff = (m->vi[13] >> 16) & 0xFFF;
    int interlaced = (ctrl & 0x40) != 0;
    int field_h = m->pal ? 288 : 240;
    *w = 320;
    *h = field_h;
    /* The RDRAM read below in RDRAM: the software RDP commits what its renderer threads drew (n64_rdp_unreported
     * flushes it); the Vulkan RDP waits for the GPU only when rows it drew there are not in RDRAM yet (a deferred
     * SYNC_FULL drawing the next buffer is not waited for here). */
    uint32_t rlo, rhi;
    vi_read_range(m, &rlo, &rhi);
    if (interlaced || rhi <= rlo || n64_rdp_unreported(m->rdp, rlo, rhi))
        n64_rdp_flush(m->rdp);
    if (m->pf.on) { /* the early scanout: not when the CPU wrote where it reads since (n64_vi_origin_changed) */
        m->pf.on = false;
        if (rlo != m->pf.lo || rhi != m->pf.hi || non_rcp_gen(m, rlo, rhi) != m->pf.gen)
            n64_rdp_scanout_forget(m->rdp);
    }
    /* Internal resolution (XBA-185, n64_rdp.h): the Vulkan RDP's own VI scans out its upscaled picture when the
     * caller's buffer holds it (640 VI pixels wide times the scale); else, or when it shows nothing, the picture
     * below, from RDRAM at 1x. */
    int sscale = n64_rdp_scanout_scale(m->rdp);
    if (sscale > 0 && max_w >= CANVAS_W * sscale &&
        n64_rdp_scanout(m->rdp, m->vi, m->vi_field, xrgb, max_w, max_h, w, h) == 0)
        return 0;
    if (type < 2 || width == 0 || he <= hs || ve <= vs || xs == 0 || ys == 0)
        return -1;
    struct n64_vi_state *v = vi_state(m);
    if (!v)
        return -1;

    int k = xs <= 0x200 ? 2 : 1; /* VI pixels per output pixel */
    int cw = CANVAS_W / k, ch = interlaced ? 2 * field_h : field_h;
    int href = m->pal ? 128 : 108, vref = m->pal ? 44 : 34;
    uint32_t bpp = type == 3 ? 4 : 2;

    /* Interlace: which rows this field goes to. */
    int parity = 0;
    if (interlaced) {
        if (!v->interlaced || v->cw != cw || v->ch != ch) {
            memset(v->canvas, 0, sizeof v->canvas);
            v->last_pos = INT64_MIN;
        }
        uint32_t stride = width * bpp;
        int64_t pos = (int64_t)origin * 1024 / stride + yoff;
        parity = (int)m->vi_field;
        if (v->last_pos != INT64_MIN) {
            /* The field half a line (or a line) below the last one is the odd one, the one above the even one. */
            int64_t d = pos - v->last_pos;
            if (d > 0 && d < (int64_t)ys)
                parity = 1;
            else if (d < 0 && d > -(int64_t)ys)
                parity = 0;
        }
        v->last_pos = pos;
    }
    v->interlaced = interlaced;
    v->cw = cw;
    v->ch = ch;

    vi_ctx c;
    c.ram = m->rdram;
    c.ram_size = m->rdram_size;
    c.hidden = n64_rdp_hidden_bits(m->rdp);
    c.origin = origin;
    c.stride = width * bpp;
    c.bpp32 = bpp == 4;
    uint32_t aamode = (ctrl >> 8) & 3;
    c.aa = aamode <= 1;
    c.divot = (ctrl & 0x10) != 0;
    c.dfilter = (ctrl & 0x10000) != 0 && bpp == 2;
    int interp = aamode != 3;
    int gamma = (ctrl & 0x8) != 0, gdither = (ctrl & 0x4) != 0;

    /* Columns of the canvas inside the active area, and their framebuffer positions. */
    int a0 = (hs - href + k - 1) / k, a1 = (he - href + k - 1) / k;
    if (hs < href)
        a0 = 0;
    if (a0 < 0)
        a0 = 0;
    if (a1 > cw)
        a1 = cw;
    int maxx = 0, anyxf = 0;
    for (int col = a0; col < a1; col++) {
        int32_t p = (int32_t)xoff + (href + col * k - hs) * (int32_t)xs;
        if (p < 0)
            p = 0;
        v->xpos[col] = p;
        anyxf |= interp && ((p >> 5) & 31);
        if ((p >> 10) + 1 > maxx)
            maxx = (p >> 10) + 1;
    }
    /* The VI blanks a guard band of 8 VI pixels at the start and 7 at the end of the active area, unless that edge
     * is clamped to the scan-out (H_START before the canvas / H_END past it), as paraLLEl-RDP's VI and angrylion
     * do. Only the visible range shrinks: the sampling positions (and the pixels the filters decode) are those of
     * the whole active area. */
    int h0 = hs < href ? hs : hs + 8, h1 = he > href + CANVAS_W ? he : he - 7;
    int c0 = h0 <= href ? 0 : (h0 - href + k - 1) / k, c1 = h1 <= href ? 0 : (h1 - href + k - 1) / k;
    if (c0 < a0)
        c0 = a0;
    if (c1 > a1)
        c1 = a1;
    c.npix = maxx < (int)width ? maxx : (int)width;
    if (c.npix > LINE_MAX)
        c.npix = LINE_MAX;
    if (c.npix < 1)
        c.npix = 1;
    for (int i = 0; i < 3; i++)
        v->raw_y[i] = INT32_MIN;
    for (int i = 0; i < FLINES; i++) {
        v->fl[i].y = INT32_MIN;
        v->fl[i].rgb = v->flbuf[i];
    }

    int lines = (ve - vs) >> 1;
    int row0 = (vs - vref) >> 1; /* the field row of active line 0 (may be negative: clipped) */
    uint32_t frame = (uint32_t)m->frames;
    for (int fr = 0; fr < field_h; fr++) {
        int row = interlaced ? 2 * fr + parity : fr;
        uint32_t *out = v->canvas + (size_t)row * (size_t)cw;
        int j = fr - row0;
        if (j < 0 || j >= lines || c1 <= c0) {
            memset(out, 0, (size_t)cw * sizeof *out);
            continue;
        }
        if (c0 > 0)
            memset(out, 0, (size_t)c0 * sizeof *out);
        if (c1 < cw)
            memset(out + c1, 0, (size_t)(cw - c1) * sizeof *out);
        uint32_t yp = yoff + (uint32_t)j * ys;
        int32_t ly = (int32_t)(yp >> 10);
        int yf = interp ? (int)((yp >> 5) & 31) : 0;
        const uint8_t *a = filtered_line(v, &c, ly);
        const uint8_t *b = yf ? filtered_line(v, &c, ly + 1) : a;
        if (!yf && !anyxf && !gamma && !gdither) { /* framebuffer pixels as they are (320 x 240 at 0.5, 1.0) */
            for (int col = c0; col < c1; col++) {
                int x = v->xpos[col] >> 10;
                const uint8_t *pa = a + 3 * (x < c.npix ? x : c.npix - 1);
                out[col] = (uint32_t)pa[0] << 16 | (uint32_t)pa[1] << 8 | (uint32_t)pa[2];
            }
            continue;
        }
        for (int col = c0; col < c1; col++) {
            int32_t p = v->xpos[col];
            int x = p >> 10;
            if (x >= c.npix)
                x = c.npix - 1;
            int xf = interp ? (p >> 5) & 31 : 0;
            const uint8_t *pa = a + 3 * x, *pb = b + 3 * x;
            int rgb[3];
            for (int chn = 0; chn < 3; chn++) {
                int t = pa[chn] + (((pa[chn + 3] - pa[chn]) * xf + 16) >> 5);
                if (yf) {
                    int u = pb[chn] + (((pb[chn + 3] - pb[chn]) * xf + 16) >> 5);
                    t += ((u - t) * yf + 16) >> 5;
                }
                rgb[chn] = t;
            }
            if (gamma || gdither) {
                uint32_t nz = gdither ? noise((uint32_t)col, (uint32_t)row, frame) : 0;
                for (int chn = 0; chn < 3; chn++) {
                    if (gamma)
                        rgb[chn] = v->gamma[(rgb[chn] << 6) | ((nz >> (6 * chn)) & 63)];
                    else if (rgb[chn] < 255)
                        rgb[chn] += (int)((nz >> chn) & 1);
                }
            }
            out[col] = (uint32_t)rgb[0] << 16 | (uint32_t)rgb[1] << 8 | (uint32_t)rgb[2];
        }
    }

    int ow = cw < max_w ? cw : max_w, oh = ch < max_h ? ch : max_h;
    if (ow <= 0 || oh <= 0)
        return -1;
    for (int y = 0; y < oh; y++)
        memcpy(xrgb + (size_t)y * (size_t)ow, v->canvas + (size_t)y * (size_t)cw, (size_t)ow * sizeof *xrgb);
    *w = ow;
    *h = oh;
    return 0;
}

/* ---- The change test (n64_core.h n64_display_changed) ------------------------------------------------------------- */
/* The RDRAM bytes the VI reads for the registers as they are, [*lo, *hi) (empty when it shows nothing): the
 * framebuffer lines its visible rows sample, one line more for the vertical interpolation, one more each side for the
 * AA / dither filters' neighbours (n64_display above), and one more line for paraLLEl-RDP's own VI (whose filters
 * are not ours). Lines are whole (WIDTH pixels). */
static void vi_read_range(const n64_machine *m, uint32_t *lo, uint32_t *hi) { vi_lines_range(m, lo, hi, 0); }

/* strict: the framebuffer lines the visible rows start on only, without the neighbours (the early scanout's test for
 * rows the RDP drew since: a game whose buffers follow each other draws the next one right after the last line). */
static void vi_lines_range(const n64_machine *m, uint32_t *lo, uint32_t *hi, int strict)
{
    uint32_t ctrl = m->vi[0], type = ctrl & 3;
    uint32_t origin = m->vi[1] & 0xFFFFFF, width = m->vi[2] & 0xFFF;
    int vs = (int)((m->vi[10] >> 16) & 0x3FF), ve = (int)(m->vi[10] & 0x3FF);
    uint32_t ys = m->vi[13] & 0xFFF, yoff = (m->vi[13] >> 16) & 0xFFF;
    *lo = *hi = 0;
    if (type < 2 || width == 0 || ve <= vs || ys == 0)
        return;
    int field_h = m->pal ? 288 : 240, vref = m->pal ? 44 : 34;
    int lines = (ve - vs) >> 1, row0 = (vs - vref) >> 1;
    int j0 = row0 < 0 ? -row0 : 0, j1 = field_h - row0 < lines ? field_h - row0 : lines;
    if (j1 <= j0)
        return;
    uint64_t stride = (uint64_t)width * (type == 3 ? 4 : 2);
    int64_t y0 = (int64_t)((yoff + (uint64_t)j0 * ys) >> 10) - (strict ? 0 : 1);
    int64_t y1 = (int64_t)((yoff + (uint64_t)(j1 - 1) * ys) >> 10) + (strict ? 0 : 3); /* last line read +
                                                                            interpolation, filter, paraLLEl-RDP's VI */
    if (y0 < 0)
        y0 = 0;
    uint64_t a = origin + (uint64_t)y0 * stride, b = origin + (uint64_t)(y1 + 1) * stride;
    if (a >= m->rdram_size)
        return;
    if (b > m->rdram_size)
        b = m->rdram_size;
    *lo = (uint32_t)a;
    *hi = (uint32_t)b;
}

/* The writes into the pages of [lo, hi) that did not come through bus.wrote (CPU, lifted code, PI / SI DMA...). */
static uint64_t non_rcp_gen(const n64_machine *m, uint32_t lo, uint32_t hi)
{
    uint64_t sum = 0;
    if (hi <= lo)
        return 0;
    for (uint32_t p = lo >> N64_PAGE_SHIFT; p <= (hi - 1) >> N64_PAGE_SHIFT; p++)
        sum += (uint32_t)(m->rdram_gen[p] - m->rcp_gen[p]);
    return sum;
}

static uint64_t rdp_full_syncs(n64_machine *m)
{
    n64_rdp_stats st;
    n64_rdp_get_stats(m->rdp, &st);
    return st.full_syncs;
}

int n64_display_changed(n64_machine *m)
{
    if (!m->disp.valid || (m->vi[0] & 0x40)) /* none yet; interlaced: the field order is n64_display's state */
        return 1;
    for (int r = 0; r < 14; r++)
        if (r != 4 && m->vi[r] != m->disp.vi[r]) /* V_CURRENT (4) is the beam, not the picture */
            return 1;
    if (n64_rdp_backend_name(m->rdp) != m->disp.rdp || n64_rdp_scanout_scale(m->rdp) != m->disp.sscale)
        return 1;
    uint32_t lo = m->disp.lo, hi = m->disp.hi; /* the same registers: the same range */
    if (hi > lo) {
        if (n64_rdp_unreported(m->rdp, lo, hi)) /* drawn there, not in RDRAM yet (the software RDP: flushed) */
            return 1;
        if (m->disp_rcp_hits != m->disp.hits || non_rcp_gen(m, lo, hi) != m->disp.gen)
            return 1;
    }
    if (m->disp.cpu_pending && rdp_full_syncs(m) != m->disp.syncs)
        return 1;
    return 0;
}

/* After a picture: what it was made from, for n64_display_changed. */
static void display_snapshot(n64_machine *m)
{
    uint32_t lo, hi;
    vi_read_range(m, &lo, &hi);
    const int same_range = m->disp.valid && lo == m->disp.lo && hi == m->disp.hi;
    const uint64_t gen = non_rcp_gen(m, lo, hi);
    const int sscale = n64_rdp_scanout_scale(m->rdp);
    if (sscale > 0) {
        uint64_t syncs = rdp_full_syncs(m);
        if (m->disp.cpu_pending && syncs != m->disp.syncs)
            m->disp.cpu_pending = false; /* this picture was scanned out after the RDP's next pass */
        /* (A write before a buffer swap, the range then another, is not caught: the CPU drawing into a buffer the
         * RDP does not draw, rare in an upscaled game.) */
        if (same_range && gen != m->disp.gen) {
            m->disp.cpu_pending = true; /* the upscaled copy may not have it yet: again after the next SYNC_FULL */
            m->disp.syncs = syncs;
        }
    } else {
        m->disp.cpu_pending = false;
    }
    memcpy(m->disp.vi, m->vi, sizeof m->disp.vi);
    m->disp.lo = lo;
    m->disp.hi = hi;
    m->disp.gen = gen;
    m->disp.hits = m->disp_rcp_hits;
    m->disp.rdp = n64_rdp_backend_name(m->rdp);
    m->disp.sscale = sscale;
    m->disp.valid = true;
}

int n64_display(n64_machine *m, uint32_t *xrgb, int max_w, int max_h, int *w, int *h)
{
    int r = display_picture(m, xrgb, max_w, max_h, w, h);
    display_snapshot(m);
    return r;
}

int n64_display_rgba(n64_machine *m, const uint8_t **rgba, int *w, int *h, int *pitch)
{
    if (n64_rdp_scanout_scale(m->rdp) <= 0 || (m->vi[0] & 0x40))
        return -2;
    /* As display_picture: the RDP's rows where the VI reads in RDRAM first; the early scanout unless the CPU wrote
     * there since. */
    uint32_t rlo, rhi;
    vi_read_range(m, &rlo, &rhi);
    if (rhi <= rlo || n64_rdp_unreported(m->rdp, rlo, rhi))
        n64_rdp_flush(m->rdp);
    if (m->pf.on) {
        m->pf.on = false;
        if (rlo != m->pf.lo || rhi != m->pf.hi || non_rcp_gen(m, rlo, rhi) != m->pf.gen)
            n64_rdp_scanout_forget(m->rdp);
    }
    if (n64_rdp_scanout_rgba(m->rdp, m->vi, m->vi_field, rgba, w, h, pitch) != 0)
        return -1; /* the 1x picture then (n64_core.h) */
    display_snapshot(m);
    return 0;
}

int n64_framebuffer(n64_machine *m, uint32_t *xrgb, int max_w, int max_h, int *w, int *h)
{
    uint32_t type = m->vi[0] & 3, origin = m->vi[1] & 0xFFFFFF, width = m->vi[2] & 0xFFF;
    *w = *h = 0;
    if (type < 2 || width == 0)
        return -1;
    n64_rdp_flush(m->rdp);
    uint32_t bpp = type == 3 ? 4 : 2;
    int ow = (int)width < max_w ? (int)width : max_w;
    int oh = 0;
    for (int y = 0; y < max_h; y++) {
        uint32_t line = origin + (uint32_t)y * width * bpp;
        if ((uint64_t)line + (uint64_t)ow * bpp > m->rdram_size)
            break;
        const uint8_t *p = m->rdram + line;
        uint32_t *out = xrgb + (size_t)y * (size_t)ow;
        for (int x = 0; x < ow; x++) {
            if (bpp == 4) {
                out[x] = (uint32_t)p[4 * x] << 16 | (uint32_t)p[4 * x + 1] << 8 | p[4 * x + 2];
            } else {
                uint32_t v = n64_be16(p + 2 * x);
                uint32_t r = (v >> 11) & 31, g = (v >> 6) & 31, b = (v >> 1) & 31;
                out[x] = (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
            }
        }
        oh = y + 1;
    }
    *w = ow;
    *h = oh;
    return oh > 0 ? 0 : -1;
}
