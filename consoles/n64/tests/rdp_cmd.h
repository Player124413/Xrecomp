/* rdp_cmd.h -- RDP command encoders and a triangle setup for the RDP tests and benchmark (XBA-173). Our own code,
 * written from the n64brew wiki command layouts; the triangle setup computes the edge and attribute coefficients the
 * way libdragon's rdpq documentation describes them (XH / XM at the scan line of YH, XL at YM, DaDe along the major
 * edge). A test owns an 8 MB RDRAM, writes a command list at CMD_BASE, points DPC_START / DPC_END at it and runs. */
#ifndef RDP_CMD_H
#define RDP_CMD_H

#include "n64_devices.h"
#include "n64_rdp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE (8u << 20)
#define CMD_BASE 0x700000u
#define FB_ADDR 0x100000u
#define ZB_ADDR 0x200000u
#define TEX_ADDR 0x300000u

typedef uint64_t u64;

/* Command list being built. */
typedef struct {
    u64 w[1 << 15];
    int n;
} rdp_list;

static inline void L(rdp_list *l, u64 w) { l->w[l->n++] = w; }

/* ---- Encoders (coordinates in 10.2 where the hardware takes 10.2: pass pixels * 4) ------------------------------- */
#define OP(id) ((u64)(id) << 56)
enum { SZ4 = 0, SZ8 = 1, SZ16 = 2, SZ32 = 3 };
enum { F_RGBA = 0, F_YUV = 1, F_CI = 2, F_IA = 3, F_I = 4 };

static inline u64 c_color_image(int fmt, int size, int width, uint32_t addr)
{
    return OP(0x3F) | (u64)fmt << 53 | (u64)size << 51 | (u64)(width - 1) << 32 | addr;
}
static inline u64 c_z_image(uint32_t addr) { return OP(0x3E) | addr; }
static inline u64 c_tex_image(int fmt, int size, int width, uint32_t addr)
{
    return OP(0x3D) | (u64)fmt << 53 | (u64)size << 51 | (u64)(width - 1) << 32 | addr;
}
static inline u64 c_scissor(int xh, int yh, int xl, int yl) /* 10.2 */
{
    return OP(0x2D) | (u64)xh << 44 | (u64)yh << 32 | (u64)xl << 12 | (u64)yl;
}
static inline u64 c_scissor_field(int xh, int yh, int xl, int yl, int odd)
{
    return c_scissor(xh, yh, xl, yl) | 1ull << 25 | (u64)odd << 24;
}
static inline u64 c_fill_color(uint32_t c) { return OP(0x37) | c; }
static inline u64 c_fog(uint32_t c) { return OP(0x38) | c; }
static inline u64 c_blend(uint32_t c) { return OP(0x39) | c; }
static inline u64 c_prim(int min_level, int lod_frac, uint32_t c)
{
    return OP(0x3A) | (u64)min_level << 40 | (u64)lod_frac << 32 | c;
}
static inline u64 c_env(uint32_t c) { return OP(0x3B) | c; }
static inline u64 c_prim_depth(int z, int dz) { return OP(0x2E) | (u64)(z & 0xFFFF) << 16 | (u64)(dz & 0xFFFF); }
static inline u64 c_fill_rect(int xh, int yh, int xl, int yl) /* 10.2 */
{
    return OP(0x36) | (u64)xl << 44 | (u64)yl << 32 | (u64)xh << 12 | (u64)yh;
}
static inline void c_texrect(rdp_list *l, int flip, int tile, int xh, int yh, int xl, int yl, int s, int t, int dsdx,
                             int dtdy)
{
    L(l, OP(flip ? 0x25 : 0x24) | (u64)xl << 44 | (u64)yl << 32 | (u64)tile << 24 | (u64)xh << 12 | (u64)yh);
    L(l, (u64)(uint16_t)s << 48 | (u64)(uint16_t)t << 32 | (u64)(uint16_t)dsdx << 16 | (uint16_t)dtdy);
}
static inline u64 c_set_tile(int fmt, int size, int line, int tmem, int tile, int pal, int ct, int mt, int mask_t,
                             int shift_t, int cs, int ms, int mask_s, int shift_s)
{
    return OP(0x35) | (u64)fmt << 53 | (u64)size << 51 | (u64)line << 41 | (u64)tmem << 32 | (u64)tile << 24 |
           (u64)pal << 20 | (u64)ct << 19 | (u64)mt << 18 | (u64)mask_t << 14 | (u64)shift_t << 10 | (u64)cs << 9 |
           (u64)ms << 8 | (u64)mask_s << 4 | (u64)shift_s;
}
static inline u64 c_tile_op(int id, int tile, int sl, int tl, int sh, int th)
{
    return OP(id) | (u64)sl << 44 | (u64)tl << 32 | (u64)tile << 24 | (u64)sh << 12 | (u64)th;
}
#define c_tile_size(t, sl, tl, sh, th) c_tile_op(0x32, t, sl, tl, sh, th)
#define c_load_tile(t, sl, tl, sh, th) c_tile_op(0x34, t, sl, tl, sh, th)
#define c_load_tlut(t, sl, tl, sh, th) c_tile_op(0x30, t, sl, tl, sh, th)
#define c_load_block(t, sl, tl, sh, dxt) c_tile_op(0x33, t, sl, tl, sh, dxt)
static inline u64 c_convert(int k0, int k1, int k2, int k3, int k4, int k5)
{
    return OP(0x2C) | (u64)(k0 & 0x1FF) << 45 | (u64)(k1 & 0x1FF) << 36 | (u64)(k2 & 0x1FF) << 27 |
           (u64)(k3 & 0x1FF) << 18 | (u64)(k4 & 0x1FF) << 9 | (u64)(k5 & 0x1FF);
}
static inline u64 c_key_r(int width, int center, int scale) { return OP(0x2B) | (u64)width << 16 | (u64)center << 8 | (u64)scale; }
static inline u64 c_key_gb(int wg, int wb, int cg, int sg, int cb, int sb)
{
    return OP(0x2A) | (u64)wg << 44 | (u64)wb << 32 | (u64)cg << 24 | (u64)sg << 16 | (u64)cb << 8 | (u64)sb;
}
#define c_sync_full() OP(0x29)
#define c_sync_pipe() OP(0x27)
#define c_sync_load() OP(0x26)
#define c_sync_tile() OP(0x28)

/* Combiner inputs. */
enum { CC_COMBINED = 0, CC_TEX0 = 1, CC_TEX1 = 2, CC_PRIM = 3, CC_SHADE = 4, CC_ENV = 5, CC_ONE = 6, CC_NOISE = 7,
       CC_KEYCENTER = 6, CC_K4 = 7, CC_KEYSCALE = 6, CC_COMBINED_A = 7, CC_TEX0_A = 8, CC_TEX1_A = 9, CC_PRIM_A = 10,
       CC_SHADE_A = 11, CC_ENV_A = 12, CC_LODFRAC = 13, CC_PRIMLODFRAC = 14, CC_K5 = 15, CC_ZERO_MUL = 31,
       CC_ZERO_A = 15, CC_ZERO_ADD = 7 };
enum { AC_COMBINED = 0, AC_TEX0 = 1, AC_TEX1 = 2, AC_PRIM = 3, AC_SHADE = 4, AC_ENV = 5, AC_ONE = 6, AC_ZERO = 7,
       AC_LODFRAC = 0, AC_PRIMLODFRAC = 6 };
/* (a - b) * c + d for RGB and alpha, cycle 0 then cycle 1. */
static inline u64 c_combine(int a0, int b0, int c0, int d0, int aa0, int ab0, int ac0, int ad0, int a1, int b1, int c1,
                            int d1, int aa1, int ab1, int ac1, int ad1)
{
    return OP(0x3C) | (u64)a0 << 52 | (u64)c0 << 47 | (u64)aa0 << 44 | (u64)ac0 << 41 | (u64)a1 << 37 | (u64)c1 << 32 |
           (u64)b0 << 28 | (u64)b1 << 24 | (u64)aa1 << 21 | (u64)ac1 << 18 | (u64)d0 << 15 | (u64)ab0 << 12 |
           (u64)ad0 << 9 | (u64)d1 << 6 | (u64)ab1 << 3 | (u64)ad1;
}
/* The same equation in both cycles (1-cycle mode reads cycle 1). */
static inline u64 c_combine1(int a, int b, int c, int d, int aa, int ab, int ac, int ad)
{
    return c_combine(a, b, c, d, aa, ab, ac, ad, a, b, c, d, aa, ab, ac, ad);
}
/* Output = x (RGB) and y (alpha) via the add input. */
#define CC_PASS(x, y) c_combine1(CC_ZERO_A, CC_ZERO_A, CC_ZERO_MUL, x, AC_ZERO, AC_ZERO, AC_ZERO, y)

/* Other modes. */
#define OM_CYCLE(c) ((u64)(c) << 52)
#define OM_1CYC OM_CYCLE(0)
#define OM_2CYC OM_CYCLE(1)
#define OM_COPY OM_CYCLE(2)
#define OM_FILL OM_CYCLE(3)
#define OM_PERSP (1ull << 51)
#define OM_DETAIL (1ull << 50)
#define OM_SHARPEN (1ull << 49)
#define OM_LOD (1ull << 48)
#define OM_TLUT (1ull << 47)
#define OM_TLUT_IA (1ull << 46)
#define OM_BILERP (1ull << 45)
#define OM_MID (1ull << 44)
#define OM_TF0_RGB (1ull << 43) /* bi_lerp_0: filter, not convert */
#define OM_TF1_RGB (1ull << 42)
#define OM_CONVERT_ONE (1ull << 41)
#define OM_KEY (1ull << 40)
#define OM_RGB_DITHER(d) ((u64)(d) << 38)
#define OM_ALPHA_DITHER(d) ((u64)(d) << 36)
#define OM_BLEND0(p, a, m, b) ((u64)(p) << 30 | (u64)(a) << 26 | (u64)(m) << 22 | (u64)(b) << 18)
#define OM_BLEND1(p, a, m, b) ((u64)(p) << 28 | (u64)(a) << 24 | (u64)(m) << 20 | (u64)(b) << 16)
#define OM_FORCE_BLEND (1ull << 14)
#define OM_ALPHA_CVG_SEL (1ull << 13)
#define OM_CVG_X_ALPHA (1ull << 12)
#define OM_ZMODE(m) ((u64)(m) << 10)
#define OM_CVG_DEST(d) ((u64)(d) << 8)
#define OM_COLOR_ON_CVG (1ull << 7)
#define OM_IMAGE_READ (1ull << 6)
#define OM_Z_UPDATE (1ull << 5)
#define OM_Z_COMPARE (1ull << 4)
#define OM_AA (1ull << 3)
#define OM_Z_PRIM (1ull << 2)
#define OM_DITHER_ALPHA (1ull << 1)
#define OM_ALPHA_COMPARE 1ull
/* Standard: 1-cycle, filters on, no dither. */
#define OM_STD (OM_1CYC | OM_TF0_RGB | OM_TF1_RGB | OM_RGB_DITHER(3) | OM_ALPHA_DITHER(3))
static inline u64 c_modes(u64 m) { return OP(0x2F) | m; }

/* ---- Triangle setup ---------------------------------------------------------------------------------------------- */
typedef struct {
    float x, y, r, g, b, a, s, t, w, z; /* s, t in texels; w in 0..1 (perspective), z in 0..0x7FFF */
} vtx;

/* Vertex constructors: position only, + colour, + texture (s, t, w), + depth. */
static inline vtx V(float x, float y)
{
    vtx v;
    memset(&v, 0, sizeof v);
    v.x = x;
    v.y = y;
    return v;
}
static inline vtx VC(float x, float y, float r, float g, float b, float a)
{
    vtx v = V(x, y);
    v.r = r;
    v.g = g;
    v.b = b;
    v.a = a;
    return v;
}
static inline vtx VT(float x, float y, float s, float t, float w)
{
    vtx v = V(x, y);
    v.s = s;
    v.t = t;
    v.w = w;
    return v;
}
static inline vtx VZ(float x, float y, float z)
{
    vtx v = V(x, y);
    v.z = z;
    return v;
}

static inline int32_t fx16(double v)
{
    double f = floor(v * 65536.0 + 0.5);
    if (f > 2147483647.0)
        f = 2147483647.0;
    if (f < -2147483648.0)
        f = -2147483648.0;
    return (int32_t)(int64_t)f;
}

static inline u64 pack4(int32_t a, int32_t b, int32_t c, int32_t d, int hi)
{
    uint32_t sh = hi ? 16 : 0;
    return (u64)(((uint32_t)a >> sh) & 0xFFFF) << 48 | (u64)(((uint32_t)b >> sh) & 0xFFFF) << 32 |
           (u64)(((uint32_t)c >> sh) & 0xFFFF) << 16 | (u64)(((uint32_t)d >> sh) & 0xFFFF);
}

/* Appends a triangle command (shade / tex / z as asked); returns its word count. */
static int c_triangle(rdp_list *l, int shade, int tex, int zb, int tile, int level, const vtx *pa, const vtx *pb,
                      const vtx *pc)
{
    const vtx *v[3] = { pa, pb, pc };
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2 - i; j++)
            if (v[j]->y > v[j + 1]->y) {
                const vtx *t = v[j];
                v[j] = v[j + 1];
                v[j + 1] = t;
            }
    const vtx *v1 = v[0], *v2 = v[1], *v3 = v[2];
    double x1 = v1->x, y1 = v1->y, x2 = v2->x, y2 = v2->y, x3 = v3->x, y3 = v3->y;
    int yh = (int)floor(y1 * 4), ym = (int)floor(y2 * 4), yl = (int)floor(y3 * 4);
    double ish = y3 != y1 ? (x3 - x1) / (y3 - y1) : 0, ism = y2 != y1 ? (x2 - x1) / (y2 - y1) : 0;
    double isl = y3 != y2 ? (x3 - x2) / (y3 - y2) : 0;
    double nz = (x2 - x1) * (y3 - y1) - (y2 - y1) * (x3 - x1);
    int lft = nz > 0;
    double fy = floor(yh / 4.0) - y1;
    double xh = x1 + fy * ish, xm = x1 + fy * ism, xl = x2 + (ym / 4.0 - y2) * isl;
    int id = 0x08 | shade << 2 | tex << 1 | zb;
    int n0 = l->n;
    L(l, OP(id) | (u64)lft << 55 | (u64)level << 51 | (u64)tile << 48 | (u64)(yl & 0x3FFF) << 32 |
             (u64)(ym & 0x3FFF) << 16 | (u64)(yh & 0x3FFF));
    L(l, (u64)(uint32_t)fx16(xl) << 32 | (uint32_t)fx16(isl));
    L(l, (u64)(uint32_t)fx16(xh) << 32 | (uint32_t)fx16(ish));
    L(l, (u64)(uint32_t)fx16(xm) << 32 | (uint32_t)fx16(ism));
    double attr[3][8];
    for (int i = 0; i < 3; i++) {
        const vtx *p = v[i];
        double w = p->w > 0 ? p->w : 1.0;
        attr[i][0] = p->r;
        attr[i][1] = p->g;
        attr[i][2] = p->b;
        attr[i][3] = p->a;
        attr[i][4] = p->s * 32.0 * (tex && p->w > 0 ? w : 1.0);
        attr[i][5] = p->t * 32.0 * (tex && p->w > 0 ? w : 1.0);
        attr[i][6] = w * 32767.0;
        attr[i][7] = p->z;
    }
    int32_t base[8], dx[8], de[8], dy[8];
    for (int k = 0; k < 8; k++) {
        double a1 = attr[0][k], a2 = attr[1][k], a3 = attr[2][k];
        double ddx = nz != 0 ? ((a2 - a1) * (y3 - y1) - (a3 - a1) * (y2 - y1)) / nz : 0;
        double ddy = nz != 0 ? ((a3 - a1) * (x2 - x1) - (a2 - a1) * (x3 - x1)) / nz : 0;
        double dde = ddy + ddx * ish;
        base[k] = fx16(a1 + fy * dde);
        dx[k] = fx16(ddx);
        de[k] = fx16(dde);
        dy[k] = fx16(ddy);
    }
    if (shade) {
        L(l, pack4(base[0], base[1], base[2], base[3], 1));
        L(l, pack4(dx[0], dx[1], dx[2], dx[3], 1));
        L(l, pack4(base[0], base[1], base[2], base[3], 0));
        L(l, pack4(dx[0], dx[1], dx[2], dx[3], 0));
        L(l, pack4(de[0], de[1], de[2], de[3], 1));
        L(l, pack4(dy[0], dy[1], dy[2], dy[3], 1));
        L(l, pack4(de[0], de[1], de[2], de[3], 0));
        L(l, pack4(dy[0], dy[1], dy[2], dy[3], 0));
    }
    if (tex) {
        L(l, pack4(base[4], base[5], base[6], 0, 1));
        L(l, pack4(dx[4], dx[5], dx[6], 0, 1));
        L(l, pack4(base[4], base[5], base[6], 0, 0));
        L(l, pack4(dx[4], dx[5], dx[6], 0, 0));
        L(l, pack4(de[4], de[5], de[6], 0, 1));
        L(l, pack4(dy[4], dy[5], dy[6], 0, 1));
        L(l, pack4(de[4], de[5], de[6], 0, 0));
        L(l, pack4(dy[4], dy[5], dy[6], 0, 0));
    }
    if (zb) {
        L(l, (u64)(uint32_t)base[7] << 32 | (uint32_t)dx[7]);
        L(l, (u64)(uint32_t)de[7] << 32 | (uint32_t)dy[7]);
    }
    return l->n - n0;
}

/* ---- Running ----------------------------------------------------------------------------------------------------- */
typedef struct {
    uint8_t *ram;
    n64_rdp *dp;
    int irqs;
} rdp_rig;

static void rig_raise(void *ctx, int irq)
{
    if (irq == N64_MI_DP)
        ((rdp_rig *)ctx)->irqs++;
}
static void rig_clear(void *ctx, int irq)
{
    (void)ctx;
    (void)irq;
}

static inline void rig_open(rdp_rig *g)
{
    g->ram = calloc(RAM_SIZE, 1);
    g->irqs = 0;
    g->dp = n64_rdp_create((n64_rcp_bus){ g->ram, RAM_SIZE, rig_raise, rig_clear, g, NULL, NULL });
}
static inline void rig_close(rdp_rig *g)
{
    n64_rdp_destroy(g->dp);
    free(g->ram);
}

/* Writes the list at CMD_BASE and runs it (START, END, run). */
static inline void rig_run(rdp_rig *g, const rdp_list *l)
{
    for (int i = 0; i < l->n; i++) {
        n64_wbe32(g->ram + CMD_BASE + i * 8, (uint32_t)(l->w[i] >> 32));
        n64_wbe32(g->ram + CMD_BASE + i * 8 + 4, (uint32_t)l->w[i]);
    }
    n64_rdp_write_reg(g->dp, 0, CMD_BASE);
    n64_rdp_write_reg(g->dp, 1, CMD_BASE + (uint32_t)l->n * 8);
    n64_rdp_run(g->dp);
    n64_rdp_flush(g->dp); /* lists without SYNC_FULL: drawn before the test reads RDRAM (the Vulkan RDP's GPU) */
}

static inline uint16_t fb16(const rdp_rig *g, int width, int x, int y) { return n64_be16(g->ram + FB_ADDR + (y * width + x) * 2); }
static inline uint32_t fb32(const rdp_rig *g, int width, int x, int y) { return n64_be32(g->ram + FB_ADDR + (y * width + x) * 4); }
static inline uint16_t rgba16(int r, int g, int b, int a) { return (uint16_t)((r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | (a ? 1 : 0)); }

static inline uint32_t fnv1a(const uint8_t *p, size_t n)
{
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x01000193u;
    return h;
}

/* A busy 320x240 RGBA16 scene: z-buffered textured and shaded triangles of mixed sizes, rectangles in every mode,
 * blended sprites, fog. Deterministic from seed. Draws into FB_ADDR with Z at ZB_ADDR, texture at TEX_ADDR. */
static uint32_t scene_rng(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

static void scene_textures(uint8_t *ram)
{
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            int c = ((x >> 2) ^ (y >> 2)) & 1;
            n64_wbe16(ram + TEX_ADDR + (y * 32 + x) * 2, c ? rgba16(255, 240, 200, 1) : rgba16(40, 90, 200, 1));
        }
    for (int i = 0; i < 64 * 64; i++) /* I8 64x64 */
        ram[TEX_ADDR + 0x1000 + i] = (uint8_t)((i * 7) ^ (i >> 6) * 13);
}

static void scene_busy(rdp_list *l, int nprims, uint32_t seed)
{
    uint32_t s = seed;
    L(l, c_color_image(F_RGBA, SZ16, 320, ZB_ADDR));
    L(l, c_scissor(0, 0, 320 * 4, 240 * 4));
    L(l, c_modes(OM_FILL));
    L(l, c_fill_color(0xFFFCFFFC));
    L(l, c_fill_rect(0, 0, 319 * 4, 239 * 4));
    L(l, c_sync_pipe());
    L(l, c_color_image(F_RGBA, SZ16, 320, FB_ADDR));
    L(l, c_z_image(ZB_ADDR));
    L(l, c_fill_color(0x00010001));
    L(l, c_fill_rect(0, 0, 319 * 4, 239 * 4));
    /* texture: 32x32 RGBA16 in tile 0 (load via tile 7) */
    L(l, c_tex_image(F_RGBA, SZ16, 32, TEX_ADDR));
    L(l, c_set_tile(F_RGBA, SZ16, 8, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0));
    L(l, c_sync_load());
    L(l, c_load_tile(7, 0, 0, 31 * 4, 31 * 4));
    L(l, c_set_tile(F_RGBA, SZ16, 8, 0, 0, 0, 0, 0, 5, 0, 0, 0, 5, 0));
    L(l, c_tile_size(0, 0, 0, 31 * 4, 31 * 4));
    L(l, c_fog(0x8090A0FF));
    for (int i = 0; i < nprims; i++) {
        int kind = (int)(scene_rng(&s) % 8);
        float x = (float)(scene_rng(&s) % 300), y = (float)(scene_rng(&s) % 220);
        float sz = 4.0f + (float)(scene_rng(&s) % 40);
        float z = (float)(scene_rng(&s) % 0x7000);
        vtx a = { x, y, 255, 40, 40, 255, 0, 0, 1, z };
        vtx b = { x + sz, y + sz * 0.3f, 40, 255, 40, 160, 31, 0, 1, z + 300 };
        vtx c = { x + sz * 0.2f, y + sz, 40, 40, 255, 255, 0, 31, 1, z + 600 };
        switch (kind) {
        case 0: case 1: case 2: /* textured shaded z, bilinear */
            L(l, c_modes(OM_1CYC | OM_TF0_RGB | OM_TF1_RGB | OM_RGB_DITHER(0) | OM_ALPHA_DITHER(3) | OM_BILERP |
                         OM_Z_COMPARE | OM_Z_UPDATE));
            L(l, c_combine1(CC_TEX0, CC_ZERO_A, CC_SHADE, CC_ZERO_ADD, AC_ZERO, AC_ZERO, AC_ZERO, AC_SHADE));
            c_triangle(l, 1, 1, 1, 0, 0, &a, &b, &c);
            break;
        case 3: /* shaded z */
            L(l, c_modes(OM_STD | OM_Z_COMPARE | OM_Z_UPDATE));
            L(l, CC_PASS(CC_SHADE, AC_SHADE));
            c_triangle(l, 1, 0, 1, 0, 0, &a, &b, &c);
            break;
        case 4: /* 2-cycle fog + alpha blend */
            L(l, c_modes(OM_2CYC | OM_TF0_RGB | OM_TF1_RGB | OM_RGB_DITHER(1) | OM_ALPHA_DITHER(3) | OM_IMAGE_READ |
                         OM_FORCE_BLEND | OM_Z_COMPARE | OM_BLEND0(0, 2, 3, 0) | OM_BLEND1(0, 0, 1, 0)));
            L(l, c_combine(CC_TEX0, CC_ZERO_A, CC_SHADE, CC_ZERO_ADD, AC_ZERO, AC_ZERO, AC_ZERO, AC_SHADE, CC_ZERO_A,
                           CC_ZERO_A, CC_ZERO_MUL, CC_COMBINED, AC_ZERO, AC_ZERO, AC_ZERO, AC_COMBINED));
            c_triangle(l, 1, 1, 1, 0, 0, &a, &b, &c);
            break;
        case 5: /* fill rect */
            L(l, c_modes(OM_FILL));
            L(l, c_fill_color(scene_rng(&s) | 0x00010001));
            L(l, c_fill_rect((int)x * 4, (int)y * 4, (int)(x + sz) * 4, (int)(y + sz / 2) * 4));
            break;
        case 6: /* copy blit */
            L(l, c_modes(OM_COPY | OM_ALPHA_COMPARE));
            c_texrect(l, 0, 0, (int)x * 4, (int)y * 4, (int)(x + 31) * 4, (int)(y + 15) * 4, 0, 0, 4 << 10, 1 << 10);
            break;
        default: /* scaled textured rect, prim-tinted, alpha blended */
            L(l, c_modes(OM_STD | OM_BILERP | OM_IMAGE_READ | OM_FORCE_BLEND | OM_BLEND0(0, 0, 1, 0) |
                         OM_BLEND1(0, 0, 1, 0)));
            L(l, c_prim(0, 0, 0xFFC08080));
            L(l, c_combine1(CC_TEX0, CC_ZERO_A, CC_PRIM, CC_ZERO_ADD, AC_ZERO, AC_ZERO, AC_ZERO, AC_PRIM));
            c_texrect(l, 0, 0, (int)x * 4, (int)y * 4, (int)(x + sz) * 4, (int)(y + sz) * 4, 0, 0, 1 << 9, 1 << 9);
            break;
        }
    }
    L(l, c_sync_full());
}

#endif
