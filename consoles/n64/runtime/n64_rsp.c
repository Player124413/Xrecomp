/* n64_rsp.c -- the Reality Signal Processor, low level (N-5, XBA-173): the scalar unit and the vector unit (VU)
 * interpreted instruction by instruction, so every microcode (audio and graphics, libultra's and libdragon's) runs as
 * on the hardware. Implements the RSP part of n64_devices.h. Our own code, written from the n64brew wiki's RSP pages
 * and the behaviour of ares (ISC) and cen64 (BSD-3) as documented by their authors; nothing copied.
 *
 * Scalar unit: a 32-bit MIPS subset (no multiply / divide, no exceptions: ADD / ADDI / SUB never trap, unknown opcodes
 * are no-ops), 12-bit PC in IMEM, branch delay slots (pc / npc pair, so a branch in a delay slot behaves as on a MIPS
 * pipeline), loads and stores anywhere in DMEM (unaligned accesses allowed, addresses wrap at 4 KB), BREAK = halt +
 * broke (+ the SP interrupt when SP_STATUS.intr_break is set). COP0: 0..7 the SP registers, 8..15 the DPC registers
 * of the attached RDP (reads 0 without one).
 *
 * Vector unit: 32 registers of 8 x 16-bit lanes (element 0 = the most significant halfword, bytes 0-1 in the
 * big-endian byte view the loads and stores use), a 48-bit accumulator per lane kept as bits 47..16 (int32) and
 * 15..0 (uint16), so every lane operation is 32-bit,
 * VCO (carry / not-equal), VCC (compare / clip), VCE, and the divide unit (DIVIN / DIVOUT / DIVDP) with the
 * reciprocal and inverse square root ROMs computed at create time from their defining formulas. Computational ops are
 * branchless 8-lane loops over 16 / 32-bit values the compiler vectorises; on arm64 the hot ones (multiply-accumulate
 * family, add / sub, compares, clips, merge, logic, divide moves), the element selectors and the aligned quad / double
 * loads and stores are NEON intrinsics instead (vu_op_neon), bit-identical to the loops.
 *
 * Timing: one RSP cycle per instruction (no dual issue, no pipeline stalls); SP DMA completes instantly (DMA_BUSY and
 * DMA_FULL always read 0). */
#include "n64_devices.h"

#include <stdlib.h>
#include <stddef.h>
#include <string.h>

/* arm64: the vector unit's hot ops and the quad loads / stores as NEON intrinsics (XBA-173), the portable C
 * everywhere else and for the remaining ops. -DN64_RSP_NO_NEON forces the portable code (to compare). */
#if defined(__aarch64__) && defined(__ARM_NEON) && !defined(N64_RSP_NO_NEON)
#define RSP_NEON 1
#include <arm_neon.h>
#else
#define RSP_NEON 0
#endif

#if defined(__GNUC__) || defined(__clang__)
#define RSP_NOINLINE __attribute__((noinline))
static inline unsigned clz32(uint32_t x) /* x != 0 */
{
    return (unsigned)__builtin_clz(x);
}
#else
#define RSP_NOINLINE
static inline unsigned clz32(uint32_t x)
{
    unsigned n = 0;
    while (!(x & 0x80000000u)) {
        x <<= 1;
        n++;
    }
    return n;
}
#endif

/* SP_STATUS read bits. */
#define ST_HALT 0x0001u
#define ST_BROKE 0x0002u
#define ST_DMA_BUSY 0x0004u
#define ST_DMA_FULL 0x0008u
#define ST_IO_FULL 0x0010u
#define ST_SSTEP 0x0020u
#define ST_INTR_BREAK 0x0040u
#define ST_SIG0 0x0080u

struct n64_rsp {
    uint8_t mem[0x2000]; /* DMEM 0x000..0xFFF, IMEM 0x1000..0x1FFF, big-endian */
    int16_t vr[32][8];   /* lane i = element i */
    int32_t acch[8];     /* accumulator bits 47..16 (signed) */
    uint16_t accl[8];    /* accumulator bits 15..0 */
    uint16_t vcol[8], vcoh[8], vccl[8], vcch[8], vce[8]; /* one flag per lane, 0 or 1 */
    int16_t divin, divout;
    int divdp;
    uint32_t r[32];
    uint32_t pc, npc; /* IMEM offsets (word aligned, 12 bits) */
    uint32_t status;
    uint32_t mem_addr, dram_addr, dma_len;
    uint32_t semaphore;
    n64_rcp_bus bus;
    n64_rdp *dp;
    uint16_t rcp_rom[512];
    uint16_t rsq_rom[512];
};

/* ---- ROMs --------------------------------------------------------------------------------------------------------- */

/* Reciprocal ROM: entry i is 1 / (1 + i / 512) as 1.16 fixed point with the leading one dropped, rounded:
 * ((2^34 / (512 + i)) + 1) >> 8, minus 2^16 (entry 0, exactly 1.0, saturates to 0xFFFF). Inverse square root ROM:
 * entries 0..255 hold 2 / sqrt(1 + i / 256) and 256..511 hold sqrt(2 / (1 + (i - 256) / 256)), both 1.16 with the
 * leading one dropped: the largest b with a * b^2 < 2^44 (a = 256 + i, or 512 + 2 (i - 256)), halved. Both checked
 * entry by entry against the ares oracle (oracle group 92). */
static void build_roms(n64_rsp *s)
{
    for (uint32_t i = 0; i < 512; i++) {
        uint64_t v = ((((uint64_t)1 << 34) / (512 + i)) + 1) >> 8;
        s->rcp_rom[i] = v > 0x1FFFF ? 0xFFFF : (uint16_t)(v - 0x10000);
    }
    for (uint32_t i = 0; i < 512; i++) {
        uint64_t a = i < 256 ? 256 + i : 512 + ((i - 256) << 1);
        /* b in [2^17, 2^18): the largest b with a * b^2 < 2^44 (then a * (b + 1)^2 >= 2^44). */
        uint64_t lo = (uint64_t)1 << 17, hi = (uint64_t)1 << 18;
        while (lo + 1 < hi) {
            uint64_t mid = (lo + hi) >> 1;
            if (a * mid * mid < ((uint64_t)1 << 44))
                lo = mid;
            else
                hi = mid;
        }
        uint64_t b = lo;
        s->rsq_rom[i] = (uint16_t)((b >> 1) & 0xFFFF);
    }
}

/* ---- Helpers ------------------------------------------------------------------------------------------------------ */

static inline int16_t sclamp(int32_t v)
{
    return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

/* acc[15:0], or 0 / 0xFFFF when the accumulator does not fit in 32 signed bits (VMADL, VMADN). */
static inline int16_t clamp_lo(int32_t hm, uint16_t lo)
{
    return hm < -32768 ? 0 : hm > 32767 ? -1 : (int16_t)lo;
}

/* Unsigned result of VMULU / VMACU: 0 when the accumulator is negative, 0xFFFF when acc[47:16] exceeds 0x7FFF,
 * else acc[31:16]. */
static inline int16_t clamp_u(int32_t hm)
{
    return hm < 0 ? 0 : hm > 32767 ? -1 : (int16_t)hm;
}

static inline uint8_t vgetb(const int16_t *v, unsigned b)
{
    uint16_t x = (uint16_t)v[(b >> 1) & 7];
    return (uint8_t)(b & 1 ? x : x >> 8);
}

static inline void vsetb(int16_t *v, unsigned b, uint8_t x)
{
    uint16_t w = (uint16_t)v[(b >> 1) & 7];
    w = b & 1 ? (uint16_t)((w & 0xFF00) | x) : (uint16_t)((w & 0x00FF) | x << 8);
    v[(b >> 1) & 7] = (int16_t)w;
}

#define DM(a) (s->mem[(a) & 0xFFF])


/* ---- Lifecycle ---------------------------------------------------------------------------------------------------- */

n64_rsp *n64_rsp_create(n64_rcp_bus bus)
{
    n64_rsp *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->bus = bus;
    build_roms(s);
    n64_rsp_reset(s);
    return s;
}

void n64_rsp_destroy(n64_rsp *s)
{
    free(s);
}

/* Registers and state back to power-on; DMEM / IMEM keep their contents (the boot code may already be there). */
void n64_rsp_reset(n64_rsp *s)
{
    memset(s->vr, 0, sizeof s->vr);
    memset(s->acch, 0, sizeof s->acch);
    memset(s->accl, 0, sizeof s->accl);
    memset(s->vcol, 0, sizeof s->vcol);
    memset(s->vcoh, 0, sizeof s->vcoh);
    memset(s->vccl, 0, sizeof s->vccl);
    memset(s->vcch, 0, sizeof s->vcch);
    memset(s->vce, 0, sizeof s->vce);
    s->divin = s->divout = 0;
    s->divdp = 0;
    memset(s->r, 0, sizeof s->r);
    s->pc = 0;
    s->npc = 4;
    s->status = ST_HALT;
    s->mem_addr = s->dram_addr = 0;
    s->dma_len = 0;
    s->semaphore = 0;
}

/* Save states (XBA-183): everything up to the host side (the bus, the RDP; the ROMs after them are constants). */
#define RSP_STATE_SIZE offsetof(n64_rsp, bus)

size_t n64_rsp_state_size(n64_rsp *s)
{
    (void)s;
    return RSP_STATE_SIZE;
}

void n64_rsp_state_save(n64_rsp *s, uint8_t *out) { memcpy(out, s, RSP_STATE_SIZE); }

int n64_rsp_state_load(n64_rsp *s, const uint8_t *in, size_t n)
{
    if (n != RSP_STATE_SIZE)
        return -1;
    memcpy(s, in, RSP_STATE_SIZE);
    return 0;
}

void n64_rsp_attach_rdp(n64_rsp *s, n64_rdp *dp)
{
    s->dp = dp;
    if (dp)
        n64_rdp_attach_dmem(dp, s->mem);
}

uint8_t *n64_rsp_mem(n64_rsp *s)
{
    return s->mem;
}

int n64_rsp_halted(n64_rsp *s)
{
    return (s->status & ST_HALT) != 0;
}

uint32_t n64_rsp_get_gpr(n64_rsp *s, int r)
{
    return (r > 0 && r < 32) ? s->r[r] : 0;
}

void n64_rsp_set_gpr(n64_rsp *s, int r, uint32_t v)
{
    if (r > 0 && r < 32)
        s->r[r] = v;
}

int n64_rsp_semaphore_held(n64_rsp *s)
{
    return s->semaphore != 0;
}

/* ---- SP registers ------------------------------------------------------------------------------------------------- */

static void raise_sp(n64_rsp *s)
{
    if (s->bus.raise)
        s->bus.raise(s->bus.ctx, N64_MI_SP);
}

static void clear_sp(n64_rsp *s)
{
    if (s->bus.clear)
        s->bus.clear(s->bus.ctx, N64_MI_SP);
}

/* SP DMA: (count + 1) rows of (length | 7) + 1 bytes, RDRAM advancing by skip between rows, SP memory wrapping
 * inside its 4 KB bank. Completes at once; the address registers end just past the last byte moved (no skip after
 * the last row), the length register reads length 0xFF8, count 0, skip (8-aligned) unchanged. */
static void sp_dma(n64_rsp *s, int to_rdram, uint32_t lenreg)
{
    uint32_t len = (lenreg & 0xFF8) + 8;
    uint32_t count = ((lenreg >> 12) & 0xFF) + 1;
    uint32_t skip = (lenreg >> 20) & 0xFF8;
    uint32_t bank = s->mem_addr & 0x1000, off = s->mem_addr & 0xFF8;
    uint32_t da = s->dram_addr & 0xFFFFF8;
    uint8_t *rd = s->bus.rdram;
    uint32_t size = s->bus.rdram_size;
    for (uint32_t row = 0; row < count; row++) {
        /* In runs that stay inside the SP bank, inside the 24-bit RDRAM address space and on one side of RDRAM's
         * end: the bytes of the 8-byte steps the hardware takes, one memcpy and one write notice per run (XBA-191). */
        for (uint32_t b = 0; b < len;) {
            uint32_t n = len - b;
            if (n > 0x1000 - off)
                n = 0x1000 - off;
            if (n > 0x1000000 - da)
                n = 0x1000000 - da;
            int in = rd && da + 8 <= size;
            if (in && n > size - da)
                n = size - da;
            uint8_t *sp = s->mem + (bank | off);
            if (to_rdram) {
                if (in) {
                    memcpy(rd + da, sp, n);
                    if (s->bus.wrote)
                        s->bus.wrote(s->bus.ctx, da, n);
                }
            } else {
                if (in)
                    memcpy(sp, rd + da, n);
                else
                    memset(sp, 0, n);
            }
            b += n;
            off = (off + n) & 0xFF8;
            da = (da + n) & 0xFFFFF8;
        }
        if (row + 1 < count)
            da = (da + skip) & 0xFFFFF8;
    }
    s->mem_addr = bank | off;
    s->dram_addr = da;
    s->dma_len = 0xFF8 | (lenreg & 0xFF800000u);
}

/* SP_STATUS write: clear / set pairs (both bits set = no change), clear broke, MI SP interrupt clear / raise. */
static void status_write(n64_rsp *s, uint32_t v)
{
    uint32_t st = s->status;
#define SP_PAIR(clr, set, bit)                                                                                         \
    do {                                                                                                               \
        if ((v & (clr)) && !(v & (set)))                                                                               \
            st &= ~(uint32_t)(bit);                                                                                    \
        else if ((v & (set)) && !(v & (clr)))                                                                          \
            st |= (bit);                                                                                               \
    } while (0)
    SP_PAIR(1u << 0, 1u << 1, ST_HALT);
    if (v & (1u << 2))
        st &= ~ST_BROKE;
    SP_PAIR(1u << 5, 1u << 6, ST_SSTEP);
    SP_PAIR(1u << 7, 1u << 8, ST_INTR_BREAK);
    for (unsigned k = 0; k < 8; k++)
        SP_PAIR(1u << (9 + 2 * k), 1u << (10 + 2 * k), ST_SIG0 << k);
#undef SP_PAIR
    s->status = st;
    if ((v & (1u << 3)) && !(v & (1u << 4)))
        clear_sp(s);
    else if ((v & (1u << 4)) && !(v & (1u << 3)))
        raise_sp(s);
}

uint32_t n64_rsp_read_reg(n64_rsp *s, uint32_t reg)
{
    switch (reg) {
    case 0: return s->mem_addr;
    case 1: return s->dram_addr;
    case 2:
    case 3: return s->dma_len;
    case 4: return s->status;
    case 5: /* DMA_FULL */
    case 6: return 0; /* DMA_BUSY */
    case 7: {
        uint32_t v = s->semaphore;
        s->semaphore = 1;
        return v;
    }
    case 8: return s->pc & 0xFFC;
    default: return 0;
    }
}

void n64_rsp_write_reg(n64_rsp *s, uint32_t reg, uint32_t v)
{
    switch (reg) {
    case 0: s->mem_addr = v & 0x1FF8; break;
    case 1: s->dram_addr = v & 0xFFFFF8; break;
    case 2: sp_dma(s, 0, v); break;
    case 3: sp_dma(s, 1, v); break;
    case 4: status_write(s, v); break;
    case 7: s->semaphore = 0; break;
    case 8:
        s->pc = v & 0xFFC;
        s->npc = (s->pc + 4) & 0xFFC;
        break;
    default: break;
    }
}

static uint32_t cop0_read(n64_rsp *s, unsigned r)
{
    if (r < 8)
        return n64_rsp_read_reg(s, r);
    return s->dp ? n64_rdp_read_reg(s->dp, r - 8) : 0;
}

static void cop0_write(n64_rsp *s, unsigned r, uint32_t v)
{
    if (r < 8) {
        n64_rsp_write_reg(s, r, v);
        return;
    }
    if (!s->dp)
        return;
    n64_rdp_write_reg(s->dp, r - 8, v);
    if (r == 9) /* DPC_END: the RDP works in parallel with the RSP, let it catch up now (XBUS lists live in DMEM) */
        n64_rdp_run(s->dp);
}

/* ---- Vector unit: computational ops ------------------------------------------------------------------------------- */

/* VRCP / VRSQ (and their L forms) on a 16 or 32-bit input: the result's low half goes to the lane, the high half to
 * DIVOUT. Negative inputs use the absolute value (one's complement below -32768) and complement the result. */
static void vu_divide(n64_rsp *s, int rsq, int32_t input, int16_t *out_lane)
{
    int32_t mask = input >> 31;
    int32_t data = input ^ mask;
    int32_t result;
    if (input > -32768)
        data -= mask;
    if (data == 0) {
        result = 0x7FFFFFFF;
    } else if (input == -32768) {
        result = (int32_t)0xFFFF0000u;
    } else {
        unsigned shift = clz32((uint32_t)data);
        uint32_t m9 = (uint32_t)(((uint64_t)(uint32_t)data << shift) & 0x7FC00000u) >> 22;
        uint32_t base;
        if (rsq)
            base = s->rsq_rom[((shift & 1) ? 0 : 256) | (m9 >> 1)];
        else
            base = s->rcp_rom[m9];
        uint32_t r = (0x10000u | base) << 14;
        r = rsq ? r >> ((31 - shift) >> 1) : r >> (31 - shift);
        result = (int32_t)(r ^ (uint32_t)mask);
    }
    s->divdp = 0;
    s->divout = (int16_t)(result >> 16);
    *out_lane = (int16_t)result;
}

/* acc += addend, the addend given as its high part h (bits 47..16, signed) and low part l (bits 15..0). */
#define ACC_ADD(i, h, l)                                                                                               \
    do {                                                                                                               \
        uint32_t lo_ = (uint32_t)accl[i] + (uint32_t)(l);                                                              \
        acch[i] = (int32_t)((uint32_t)acch[i] + (uint32_t)(h) + (lo_ >> 16));                                          \
        accl[i] = (uint16_t)lo_;                                                                                       \
    } while (0)

#if RSP_NEON
/* ---- Vector unit on NEON (arm64) ---------------------------------------------------------------------------------- */

/* The same results as the portable loops of vu_op, bit for bit (test_rsp.c's ares-checked groups run on both), as
 * explicit intrinsics so they do not depend on what the compiler manages to vectorise: one 8 x 16-bit register per
 * operand, the accumulator's bits 47..16 as two 4 x 32-bit halves. Flags are 0 / 1 per lane in memory, masks here. */

/* Byte shuffles for the 16 element selectors (0, 1: the vector; 2, 3: one element of each pair; 4..7: of each half;
 * 8..15: one element everywhere), element j being bytes 2j, 2j + 1: one table lookup for any selector, no branch. */
#define SB(x) 2 * (x), 2 * (x) + 1
#define SB4(a, b, c, d) SB(a), SB(b), SB(c), SB(d)
static const uint8_t sel_bytes[16][16] = {
    {SB4(0, 1, 2, 3), SB4(4, 5, 6, 7)}, {SB4(0, 1, 2, 3), SB4(4, 5, 6, 7)}, {SB4(0, 0, 2, 2), SB4(4, 4, 6, 6)},
    {SB4(1, 1, 3, 3), SB4(5, 5, 7, 7)}, {SB4(0, 0, 0, 0), SB4(4, 4, 4, 4)}, {SB4(1, 1, 1, 1), SB4(5, 5, 5, 5)},
    {SB4(2, 2, 2, 2), SB4(6, 6, 6, 6)}, {SB4(3, 3, 3, 3), SB4(7, 7, 7, 7)}, {SB4(0, 0, 0, 0), SB4(0, 0, 0, 0)},
    {SB4(1, 1, 1, 1), SB4(1, 1, 1, 1)}, {SB4(2, 2, 2, 2), SB4(2, 2, 2, 2)}, {SB4(3, 3, 3, 3), SB4(3, 3, 3, 3)},
    {SB4(4, 4, 4, 4), SB4(4, 4, 4, 4)}, {SB4(5, 5, 5, 5), SB4(5, 5, 5, 5)}, {SB4(6, 6, 6, 6), SB4(6, 6, 6, 6)},
    {SB4(7, 7, 7, 7), SB4(7, 7, 7, 7)},
};
#undef SB4
#undef SB

static inline int16x8_t nv_sel(const int16_t *tv, unsigned e)
{
    return vreinterpretq_s16_u8(vqtbl1q_u8(vreinterpretq_u8_s16(vld1q_s16(tv)), vld1q_u8(sel_bytes[e])));
}

static inline uint16x8_t nv_mask(const uint16_t *f) /* 0 / 1 flags -> 0 / all-ones */
{
    return vreinterpretq_u16_s16(vnegq_s16(vreinterpretq_s16_u16(vld1q_u16(f))));
}

static inline void nv_flag(uint16_t *f, uint16x8_t m) /* mask -> 0 / 1 flags */
{
    vst1q_u16(f, vshrq_n_u16(m, 15));
}

static inline int16x8_t nv_sclamp(int32x4_t h0, int32x4_t h1)
{
    return vcombine_s16(vqmovn_s32(h0), vqmovn_s32(h1));
}

/* clamp_u: 0 below 0, 0xFFFF above 0x7FFF. */
static inline int16x8_t nv_clamp_u(int32x4_t h0, int32x4_t h1)
{
    int16x8_t x = vreinterpretq_s16_u16(vcombine_u16(vqmovun_s32(h0), vqmovun_s32(h1)));
    return vorrq_s16(x, vshrq_n_s16(x, 15));
}

/* clamp_lo: acc[15:0], 0 when acc[47:16] < -32768, 0xFFFF when it is > 32767. */
static inline int16x8_t nv_clamp_lo(int32x4_t h0, int32x4_t h1, uint16x8_t lo)
{
    uint16x8_t lt = vuzp1q_u16(vreinterpretq_u16_u32(vcltq_s32(h0, vdupq_n_s32(-32768))),
                               vreinterpretq_u16_u32(vcltq_s32(h1, vdupq_n_s32(-32768))));
    uint16x8_t gt = vuzp1q_u16(vreinterpretq_u16_u32(vcgtq_s32(h0, vdupq_n_s32(32767))),
                               vreinterpretq_u16_u32(vcgtq_s32(h1, vdupq_n_s32(32767))));
    return vreinterpretq_s16_u16(vbicq_u16(vorrq_u16(lo, gt), lt));
}

/* sa * ub (signed x unsigned 16-bit) as 32-bit products, lanes 0..3 and 4..7. */
static inline int32x4_t nv_mulsu_lo(int16x8_t sa, int16x8_t ub)
{
    return vmulq_s32(vmovl_s16(vget_low_s16(sa)),
                     vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(vreinterpretq_u16_s16(ub)))));
}

static inline int32x4_t nv_mulsu_hi(int16x8_t sa, int16x8_t ub)
{
    return vmulq_s32(vmovl_high_s16(sa), vreinterpretq_s32_u32(vmovl_high_u16(vreinterpretq_u16_s16(ub))));
}

/* acc += addend, given as its low 16 bits l16 and its bits 47..16 (h0, h1): ACC_ADD on 8 lanes. */
#define NV_ACC_ADD(l16, h0, h1)                                                                                        \
    do {                                                                                                               \
        uint32x4_t s0_ = vaddl_u16(vget_low_u16(accl), vget_low_u16(l16));                                             \
        uint32x4_t s1_ = vaddl_high_u16(accl, l16);                                                                    \
        acch0 = vaddq_s32(acch0, vaddq_s32(vreinterpretq_s32_u32(vshrq_n_u32(s0_, 16)), (h0)));                        \
        acch1 = vaddq_s32(acch1, vaddq_s32(vreinterpretq_s32_u32(vshrq_n_u32(s1_, 16)), (h1)));                        \
        accl = vaddq_u16(accl, l16);                                                                                   \
    } while (0)

#define NV_ACC_LOAD()                                                                                                  \
    do {                                                                                                               \
        acch0 = vld1q_s32(s->acch);                                                                                    \
        acch1 = vld1q_s32(s->acch + 4);                                                                                \
        accl = vld1q_u16(s->accl);                                                                                     \
    } while (0)

/* The hot computational ops; returns 0 for the others, which the portable code runs. */
static inline int vu_op_neon(n64_rsp *s, uint32_t op, unsigned e, unsigned vt, unsigned vs, unsigned vd)
{
    int16x8_t a = vld1q_s16(s->vr[vs]), t = nv_sel(s->vr[vt], e), r;
    int32x4_t acch0, acch1;
    uint16x8_t accl, z = vdupq_n_u16(0);
    switch (op & 63) {
    case 0x00: /* VMULF */
    case 0x01: /* VMULU */
    {
        int32x4_t p0 = vmull_s16(vget_low_s16(a), vget_low_s16(t)), p1 = vmull_high_s16(a, t);
        acch0 = vrshrq_n_s32(p0, 15); /* (p + 0x4000) >> 15 */
        acch1 = vrshrq_n_s32(p1, 15);
        accl = vaddq_u16(vshlq_n_u16(vreinterpretq_u16_s16(vmulq_s16(a, t)), 1), vdupq_n_u16(0x8000));
        r = (op & 1) ? nv_clamp_u(acch0, acch1) : vqrdmulhq_s16(a, t); /* sat((2ab + 0x8000) >> 16) = sclamp(hm) */
        break;
    }
    case 0x04: /* VMUDL */
    {
        uint16x8_t ua = vreinterpretq_u16_s16(a), ut = vreinterpretq_u16_s16(t);
        uint32x4_t p0 = vmull_u16(vget_low_u16(ua), vget_low_u16(ut)), p1 = vmull_high_u16(ua, ut);
        accl = vuzp2q_u16(vreinterpretq_u16_u32(p0), vreinterpretq_u16_u32(p1)); /* the high halves */
        acch0 = acch1 = vdupq_n_s32(0);
        r = vreinterpretq_s16_u16(accl);
        break;
    }
    case 0x05: /* VMUDM: a * ub */
    case 0x06: /* VMUDN: ua * b */
    {
        int16x8_t sx = (op & 1) ? a : t, ux = (op & 1) ? t : a;
        int32x4_t p0 = nv_mulsu_lo(sx, ux), p1 = nv_mulsu_hi(sx, ux);
        acch0 = vshrq_n_s32(p0, 16);
        acch1 = vshrq_n_s32(p1, 16);
        accl = vreinterpretq_u16_s16(vmulq_s16(a, t));
        r = (op & 1) ? vuzp2q_s16(vreinterpretq_s16_s32(p0), vreinterpretq_s16_s32(p1)) : vreinterpretq_s16_u16(accl);
        break;
    }
    case 0x07: /* VMUDH */
        acch0 = vmull_s16(vget_low_s16(a), vget_low_s16(t));
        acch1 = vmull_high_s16(a, t);
        accl = z;
        r = nv_sclamp(acch0, acch1);
        break;
    case 0x08: /* VMACF */
    case 0x09: /* VMACU */
    {
        int32x4_t p0 = vmull_s16(vget_low_s16(a), vget_low_s16(t)), p1 = vmull_high_s16(a, t);
        uint16x8_t l = vshlq_n_u16(vreinterpretq_u16_s16(vmulq_s16(a, t)), 1);
        NV_ACC_LOAD();
        NV_ACC_ADD(l, vshrq_n_s32(p0, 15), vshrq_n_s32(p1, 15));
        r = (op & 1) ? nv_clamp_u(acch0, acch1) : nv_sclamp(acch0, acch1);
        break;
    }
    case 0x0C: /* VMADL */
    {
        uint16x8_t ua = vreinterpretq_u16_s16(a), ut = vreinterpretq_u16_s16(t);
        uint32x4_t p0 = vmull_u16(vget_low_u16(ua), vget_low_u16(ut)), p1 = vmull_high_u16(ua, ut);
        uint16x8_t l = vuzp2q_u16(vreinterpretq_u16_u32(p0), vreinterpretq_u16_u32(p1));
        NV_ACC_LOAD();
        NV_ACC_ADD(l, vdupq_n_s32(0), vdupq_n_s32(0));
        r = nv_clamp_lo(acch0, acch1, accl);
        break;
    }
    case 0x0D: /* VMADM: a * ub */
    case 0x0E: /* VMADN: ua * b */
    {
        int16x8_t sx = (op & 1) ? a : t, ux = (op & 1) ? t : a;
        int32x4_t p0 = nv_mulsu_lo(sx, ux), p1 = nv_mulsu_hi(sx, ux);
        uint16x8_t l = vreinterpretq_u16_s16(vmulq_s16(a, t));
        NV_ACC_LOAD();
        NV_ACC_ADD(l, vshrq_n_s32(p0, 16), vshrq_n_s32(p1, 16));
        r = (op & 1) ? nv_sclamp(acch0, acch1) : nv_clamp_lo(acch0, acch1, accl);
        break;
    }
    case 0x0F: /* VMADH: ACCL unchanged */
        acch0 = vaddq_s32(vld1q_s32(s->acch), vmull_s16(vget_low_s16(a), vget_low_s16(t)));
        acch1 = vaddq_s32(vld1q_s32(s->acch + 4), vmull_high_s16(a, t));
        vst1q_s32(s->acch, acch0);
        vst1q_s32(s->acch + 4, acch1);
        vst1q_s16(s->vr[vd], nv_sclamp(acch0, acch1));
        return 1;
    case 0x10: /* VADD */
    case 0x11: /* VSUB */
    {
        int16x8_t c = vreinterpretq_s16_u16(vld1q_u16(s->vcol));
        int32x4_t v0, v1;
        if (op & 1) {
            v0 = vsubw_s16(vsubl_s16(vget_low_s16(a), vget_low_s16(t)), vget_low_s16(c));
            v1 = vsubw_high_s16(vsubl_high_s16(a, t), c);
        } else {
            v0 = vaddw_s16(vaddl_s16(vget_low_s16(a), vget_low_s16(t)), vget_low_s16(c));
            v1 = vaddw_high_s16(vaddl_high_s16(a, t), c);
        }
        vst1q_u16(s->accl, vuzp1q_u16(vreinterpretq_u16_s32(v0), vreinterpretq_u16_s32(v1)));
        vst1q_u16(s->vcol, z);
        vst1q_u16(s->vcoh, z);
        vst1q_s16(s->vr[vd], nv_sclamp(v0, v1));
        return 1;
    }
    case 0x14: /* VADDC */
    {
        uint16x8_t ua = vreinterpretq_u16_s16(a), sum = vaddq_u16(ua, vreinterpretq_u16_s16(t));
        vst1q_u16(s->accl, sum);
        nv_flag(s->vcol, vcltq_u16(sum, ua));
        vst1q_u16(s->vcoh, z);
        vst1q_s16(s->vr[vd], vreinterpretq_s16_u16(sum));
        return 1;
    }
    case 0x15: /* VSUBC */
    {
        uint16x8_t ua = vreinterpretq_u16_s16(a), ut = vreinterpretq_u16_s16(t), d = vsubq_u16(ua, ut);
        vst1q_u16(s->accl, d);
        nv_flag(s->vcol, vcltq_u16(ua, ut));
        nv_flag(s->vcoh, vmvnq_u16(vceqq_u16(ua, ut)));
        vst1q_s16(s->vr[vd], vreinterpretq_s16_u16(d));
        return 1;
    }
    case 0x20: /* VLT */
    case 0x21: /* VEQ */
    case 0x22: /* VNE */
    case 0x23: /* VGE */
    {
        uint16x8_t eq = vceqq_s16(a, t), coh = nv_mask(s->vcoh), col = nv_mask(s->vcol), c;
        switch (op & 3) {
        case 0: c = vorrq_u16(vcltq_s16(a, t), vandq_u16(eq, vandq_u16(coh, col))); break;
        case 1: c = vbicq_u16(eq, coh); break;
        case 2: c = vorrq_u16(vmvnq_u16(eq), coh); break;
        default: c = vorrq_u16(vcgtq_s16(a, t), vbicq_u16(eq, vandq_u16(coh, col))); break;
        }
        r = vbslq_s16(c, a, t);
        vst1q_u16(s->accl, vreinterpretq_u16_s16(r));
        nv_flag(s->vccl, c);
        vst1q_u16(s->vcch, z);
        vst1q_u16(s->vcol, z);
        vst1q_u16(s->vcoh, z);
        vst1q_s16(s->vr[vd], r);
        return 1;
    }
    case 0x24: /* VCL */
    {
        uint16x8_t ua = vreinterpretq_u16_s16(a), ut = vreinterpretq_u16_s16(t), sum = vaddq_u16(ua, ut);
        uint16x8_t zero = vceqzq_u16(sum), nocy = vcgeq_u16(sum, ua);
        uint16x8_t carry = nv_mask(s->vcol), ne = nv_mask(s->vcoh), ce = nv_mask(s->vce);
        uint16x8_t lo_new = vbslq_u16(ce, vorrq_u16(zero, nocy), vandq_u16(zero, nocy));
        uint16x8_t hi_new = vcgeq_u16(ua, ut);
        uint16x8_t ccl = vbslq_u16(vbicq_u16(carry, ne), lo_new, nv_mask(s->vccl));
        uint16x8_t cch = vbslq_u16(vorrq_u16(carry, ne), nv_mask(s->vcch), hi_new);
        uint16x8_t sel = vbslq_u16(carry, ccl, cch);
        uint16x8_t other = vbslq_u16(carry, vreinterpretq_u16_s16(vnegq_s16(t)), ut);
        uint16x8_t lo = vbslq_u16(sel, other, ua);
        nv_flag(s->vccl, ccl);
        nv_flag(s->vcch, cch);
        vst1q_u16(s->accl, lo);
        vst1q_u16(s->vcol, z);
        vst1q_u16(s->vcoh, z);
        vst1q_u16(s->vce, z);
        vst1q_s16(s->vr[vd], vreinterpretq_s16_u16(lo));
        return 1;
    }
    case 0x25: /* VCH: a + b (signs differ) or a - b (same signs) never overflows 16 bits */
    {
        uint16x8_t dm = vreinterpretq_u16_s16(vshrq_n_s16(veorq_s16(a, t), 15));
        uint16x8_t tnm = vreinterpretq_u16_s16(vshrq_n_s16(t, 15));
        int16x8_t v = vbslq_s16(dm, vaddq_s16(a, t), vsubq_s16(a, t));
        uint16x8_t lem = vclezq_s16(v), gem = vcgezq_s16(v);
        int16x8_t lo = vbslq_s16(vbslq_u16(dm, lem, gem), vbslq_s16(dm, vnegq_s16(t), t), a);
        uint16x8_t ne = vbicq_u16(vmvnq_u16(vceqzq_s16(v)), vceqq_s16(a, vmvnq_s16(t)));
        nv_flag(s->vccl, vbslq_u16(dm, lem, tnm));
        nv_flag(s->vcch, vbslq_u16(dm, tnm, gem));
        nv_flag(s->vcol, dm);
        nv_flag(s->vcoh, ne);
        nv_flag(s->vce, vandq_u16(dm, vceqq_s16(v, vdupq_n_s16(-1))));
        vst1q_u16(s->accl, vreinterpretq_u16_s16(lo));
        vst1q_s16(s->vr[vd], lo);
        return 1;
    }
    case 0x27: /* VMRG */
        r = vbslq_s16(nv_mask(s->vccl), a, t);
        vst1q_u16(s->vcol, z);
        vst1q_u16(s->vcoh, z);
        goto logic;
    case 0x28: r = vandq_s16(a, t); goto logic;            /* VAND */
    case 0x29: r = vmvnq_s16(vandq_s16(a, t)); goto logic; /* VNAND */
    case 0x2A: r = vorrq_s16(a, t); goto logic;            /* VOR */
    case 0x2B: r = vmvnq_s16(vorrq_s16(a, t)); goto logic; /* VNOR */
    case 0x2C: r = veorq_s16(a, t); goto logic;            /* VXOR */
    case 0x2D: r = vmvnq_s16(veorq_s16(a, t)); goto logic; /* VNXOR */
    logic:
        vst1q_u16(s->accl, vreinterpretq_u16_s16(r));
        vst1q_s16(s->vr[vd], r);
        return 1;
    case 0x30: /* VRCP */
    case 0x31: /* VRCPL */
    case 0x34: /* VRSQ */
    case 0x35: /* VRSQL */
    {
        const int16_t *tv = s->vr[vt];
        int32_t input = (op & 1) && s->divdp ? (int32_t)((uint32_t)(uint16_t)s->divin << 16 | (uint16_t)tv[e & 7])
                                             : (int32_t)tv[e & 7];
        vst1q_u16(s->accl, vreinterpretq_u16_s16(t));
        vu_divide(s, (op & 4) != 0, input, &s->vr[vd][vs & 7]);
        return 1;
    }
    case 0x32: /* VRCPH */
    case 0x36: /* VRSQH */
        vst1q_u16(s->accl, vreinterpretq_u16_s16(t));
        s->divdp = 1;
        s->divin = s->vr[vt][e & 7];
        s->vr[vd][vs & 7] = s->divout;
        return 1;
    case 0x37: /* VNOP */
    case 0x3F: /* VNULL */
        return 1;
    default:
        return 0;
    }
    vst1q_s32(s->acch, acch0);
    vst1q_s32(s->acch + 4, acch1);
    vst1q_u16(s->accl, accl);
    vst1q_s16(s->vr[vd], r);
    return 1;
}
#endif /* RSP_NEON */

/* Kept out of the run loop: the scalar path stays small and the vector loops get registers of their own. */
static RSP_NOINLINE void vu_op(n64_rsp *s, uint32_t op)
{
    unsigned e = (op >> 21) & 15, vt = (op >> 16) & 31, vs = (op >> 11) & 31, vd = (op >> 6) & 31;
#if RSP_NEON
    if (vu_op_neon(s, op, e, vt, vs, vd))
        return;
#endif
    const int16_t *tv = s->vr[vt];
    int16_t a[8], t[8], r[8];
    int32_t *acch = s->acch;
    uint16_t *accl = s->accl;
    uint16_t *vcol = s->vcol, *vcoh = s->vcoh, *vccl = s->vccl, *vcch = s->vcch, *vce = s->vce;
    memcpy(a, s->vr[vs], sizeof a);
    if (e < 2) {
        memcpy(t, tv, sizeof t);
    } else if (e >= 8) {
        int16_t x = tv[e & 7];
        for (int i = 0; i < 8; i++)
            t[i] = x;
    } else if (e >= 4) { /* 4..7: element e - 4 of each half */
        int16_t x0 = tv[e - 4], x1 = tv[e];
        for (int i = 0; i < 4; i++) {
            t[i] = x0;
            t[i + 4] = x1;
        }
    } else { /* 2, 3: element e - 2 of each pair */
        for (int i = 0; i < 8; i += 2)
            t[i] = t[i + 1] = tv[i + (e & 1)];
    }

    switch (op & 63) {
    case 0x00: /* VMULF: acc = a * b * 2 + 0x8000 */
        for (int i = 0; i < 8; i++) {
            int32_t p = (int32_t)a[i] * t[i];
            int32_t hm = (p + 0x4000) >> 15;
            acch[i] = hm;
            accl[i] = (uint16_t)(((uint32_t)p << 1) + 0x8000u);
            r[i] = sclamp(hm);
        }
        break;
    case 0x01: /* VMULU */
        for (int i = 0; i < 8; i++) {
            int32_t p = (int32_t)a[i] * t[i];
            int32_t hm = (p + 0x4000) >> 15;
            acch[i] = hm;
            accl[i] = (uint16_t)(((uint32_t)p << 1) + 0x8000u);
            r[i] = clamp_u(hm);
        }
        break;
    case 0x02: /* VRNDP: acc += b (or b << 16 when vs is odd) when acc >= 0 */
    case 0x0A: /* VRNDN: the same when acc < 0 */
    {
        int neg = (op & 63) == 0x0A, sh = vs & 1;
        for (int i = 0; i < 8; i++) {
            int c = neg ? acch[i] < 0 : acch[i] >= 0;
            int32_t h = sh ? t[i] : (t[i] >> 16);
            uint32_t l = sh ? 0 : (uint16_t)t[i];
            ACC_ADD(i, c ? h : 0, c ? l : 0);
            r[i] = sclamp(acch[i]);
        }
        break;
    }
    case 0x03: /* VMULQ: acc = (a * b, + 31 when negative) << 16 */
        for (int i = 0; i < 8; i++) {
            int32_t p = (int32_t)a[i] * t[i];
            p += (p >> 31) & 31;
            acch[i] = p;
            accl[i] = 0;
            r[i] = (int16_t)(sclamp(p >> 1) & ~15);
        }
        break;
    case 0x04: /* VMUDL: acc = (ua * ub) >> 16 */
        for (int i = 0; i < 8; i++) {
            uint16_t p = (uint16_t)(((uint32_t)(uint16_t)a[i] * (uint16_t)t[i]) >> 16);
            acch[i] = 0;
            accl[i] = p;
            r[i] = (int16_t)p;
        }
        break;
    case 0x05: /* VMUDM: acc = a * ub */
        for (int i = 0; i < 8; i++) {
            int32_t p = (int32_t)a[i] * (int32_t)(uint16_t)t[i];
            acch[i] = p >> 16;
            accl[i] = (uint16_t)p;
            r[i] = (int16_t)(p >> 16);
        }
        break;
    case 0x06: /* VMUDN: acc = ua * b */
        for (int i = 0; i < 8; i++) {
            int32_t p = (int32_t)(uint16_t)a[i] * (int32_t)t[i];
            acch[i] = p >> 16;
            accl[i] = (uint16_t)p;
            r[i] = (int16_t)p;
        }
        break;
    case 0x07: /* VMUDH: acc = (a * b) << 16 */
        for (int i = 0; i < 8; i++) {
            int32_t p = (int32_t)a[i] * t[i];
            acch[i] = p;
            accl[i] = 0;
            r[i] = sclamp(p);
        }
        break;
    case 0x08: /* VMACF: acc += a * b * 2 */
        for (int i = 0; i < 8; i++) {
            int32_t p = (int32_t)a[i] * t[i];
            ACC_ADD(i, p >> 15, ((uint32_t)p << 1) & 0xFFFF);
            r[i] = sclamp(acch[i]);
        }
        break;
    case 0x09: /* VMACU */
        for (int i = 0; i < 8; i++) {
            int32_t p = (int32_t)a[i] * t[i];
            ACC_ADD(i, p >> 15, ((uint32_t)p << 1) & 0xFFFF);
            r[i] = clamp_u(acch[i]);
        }
        break;
    case 0x0B: /* VMACQ: rounds acc[47:16] towards bit 5 (MPEG), result (acc[47:17] clamped) & ~15 */
        for (int i = 0; i < 8; i++) {
            int32_t p = acch[i];
            int32_t adj = ((p >> 31) & 32) | (-(int32_t)(p >= 32) & -32);
            p += adj & -(int32_t)((p & 32) == 0);
            acch[i] = p;
            r[i] = (int16_t)(sclamp(p >> 1) & ~15);
        }
        break;
    case 0x0C: /* VMADL: acc += (ua * ub) >> 16 */
        for (int i = 0; i < 8; i++) {
            uint32_t p = ((uint32_t)(uint16_t)a[i] * (uint16_t)t[i]) >> 16;
            ACC_ADD(i, 0, p);
            r[i] = clamp_lo(acch[i], accl[i]);
        }
        break;
    case 0x0D: /* VMADM: acc += a * ub */
        for (int i = 0; i < 8; i++) {
            int32_t p = (int32_t)a[i] * (int32_t)(uint16_t)t[i];
            ACC_ADD(i, p >> 16, (uint32_t)p & 0xFFFF);
            r[i] = sclamp(acch[i]);
        }
        break;
    case 0x0E: /* VMADN: acc += ua * b */
        for (int i = 0; i < 8; i++) {
            int32_t p = (int32_t)(uint16_t)a[i] * (int32_t)t[i];
            ACC_ADD(i, p >> 16, (uint32_t)p & 0xFFFF);
            r[i] = clamp_lo(acch[i], accl[i]);
        }
        break;
    case 0x0F: /* VMADH: acc += (a * b) << 16 */
        for (int i = 0; i < 8; i++) {
            int32_t p = (int32_t)a[i] * t[i];
            acch[i] = (int32_t)((uint32_t)acch[i] + (uint32_t)p);
            r[i] = sclamp(acch[i]);
        }
        break;
    case 0x10: /* VADD: a + b + carry, clamped; ACCL unclamped; VCO cleared */
        for (int i = 0; i < 8; i++) {
            int32_t v = (int32_t)a[i] + t[i] + vcol[i];
            accl[i] = (uint16_t)v;
            r[i] = sclamp(v);
            vcol[i] = vcoh[i] = 0;
        }
        break;
    case 0x11: /* VSUB: a - b - carry */
        for (int i = 0; i < 8; i++) {
            int32_t v = (int32_t)a[i] - t[i] - vcol[i];
            accl[i] = (uint16_t)v;
            r[i] = sclamp(v);
            vcol[i] = vcoh[i] = 0;
        }
        break;
    case 0x13: /* VABS: b with the sign of a (0 when a is 0); -(-32768) = 0x7FFF, ACCL 0x8000 */
        for (int i = 0; i < 8; i++) {
            int16_t neg = (int16_t)-(uint16_t)t[i];
            int16_t lo = a[i] < 0 ? neg : a[i] == 0 ? 0 : t[i];
            accl[i] = (uint16_t)lo;
            r[i] = (a[i] < 0 && t[i] == -32768) ? 32767 : lo;
        }
        break;
    case 0x14: /* VADDC: unsigned a + b, carry into VCO low */
        for (int i = 0; i < 8; i++) {
            uint32_t v = (uint32_t)(uint16_t)a[i] + (uint16_t)t[i];
            accl[i] = (uint16_t)v;
            r[i] = (int16_t)(uint16_t)v;
            vcol[i] = (uint16_t)(v >> 16);
            vcoh[i] = 0;
        }
        break;
    case 0x15: /* VSUBC: unsigned a - b, borrow into VCO low, not-equal into VCO high */
        for (int i = 0; i < 8; i++) {
            int32_t v = (int32_t)(uint16_t)a[i] - (int32_t)(uint16_t)t[i];
            accl[i] = (uint16_t)v;
            r[i] = (int16_t)(uint16_t)v;
            vcol[i] = v < 0;
            vcoh[i] = v != 0;
        }
        break;
    case 0x1D: /* VSAR: reads a slice of the accumulator, leaves it unchanged */
        if (e == 8)
            for (int i = 0; i < 8; i++)
                r[i] = (int16_t)(acch[i] >> 16);
        else if (e == 9)
            for (int i = 0; i < 8; i++)
                r[i] = (int16_t)acch[i];
        else if (e == 10)
            memcpy(r, accl, sizeof r);
        else
            memset(r, 0, sizeof r);
        break;
    case 0x20: /* VLT */
        for (int i = 0; i < 8; i++) {
            int c = (a[i] < t[i]) | ((a[i] == t[i]) & vcoh[i] & vcol[i]);
            r[i] = c ? a[i] : t[i];
            accl[i] = (uint16_t)r[i];
            vccl[i] = (uint16_t)c;
            vcch[i] = vcol[i] = vcoh[i] = 0;
        }
        break;
    case 0x21: /* VEQ */
        for (int i = 0; i < 8; i++) {
            int c = (a[i] == t[i]) & (vcoh[i] ^ 1);
            r[i] = c ? a[i] : t[i];
            accl[i] = (uint16_t)r[i];
            vccl[i] = (uint16_t)c;
            vcch[i] = vcol[i] = vcoh[i] = 0;
        }
        break;
    case 0x22: /* VNE */
        for (int i = 0; i < 8; i++) {
            int c = (a[i] != t[i]) | vcoh[i];
            r[i] = c ? a[i] : t[i];
            accl[i] = (uint16_t)r[i];
            vccl[i] = (uint16_t)c;
            vcch[i] = vcol[i] = vcoh[i] = 0;
        }
        break;
    case 0x23: /* VGE */
        for (int i = 0; i < 8; i++) {
            int c = (a[i] > t[i]) | ((a[i] == t[i]) & ((vcoh[i] & vcol[i]) ^ 1));
            r[i] = c ? a[i] : t[i];
            accl[i] = (uint16_t)r[i];
            vccl[i] = (uint16_t)c;
            vcch[i] = vcol[i] = vcoh[i] = 0;
        }
        break;
    case 0x24: /* VCL: the low half of a double-precision clip, after VCH */
        for (int i = 0; i < 8; i++) {
            int32_t us = (uint16_t)a[i], ut = (uint16_t)t[i], sum = us + ut;
            int32_t zero = (sum & 0xFFFF) == 0, nocy = sum <= 0xFFFF;
            int32_t carry = vcol[i], ne = vcoh[i], ce = vce[i];
            int32_t lo_new = ce ? (zero | nocy) : (zero & nocy);
            int32_t hi_new = us >= ut;
            int32_t ccl = (carry & (ne ^ 1)) ? lo_new : vccl[i];
            int32_t cch = ((carry | ne) ^ 1) ? hi_new : vcch[i];
            int32_t sel = carry ? ccl : cch;
            int32_t other = carry ? -ut : ut;
            int32_t lo = sel ? other : us;
            vccl[i] = (uint16_t)ccl;
            vcch[i] = (uint16_t)cch;
            accl[i] = (uint16_t)lo;
            r[i] = (int16_t)lo;
            vcol[i] = vcoh[i] = vce[i] = 0;
        }
        break;
    case 0x25: /* VCH: clip test, high half / single precision */
        for (int i = 0; i < 8; i++) {
            int32_t sv = a[i], tt = t[i];
            int32_t dm = (sv ^ tt) >> 31; /* -1: signs differ (compare against -b) */
            int32_t other = (tt ^ dm) - dm; /* -b or b */
            int32_t v = sv + ((tt ^ ~dm) + 1 + dm); /* a + b or a - b */
            int32_t lem = -(int32_t)(v <= 0), gem = -(int32_t)(v >= 0), tnm = tt >> 31;
            int32_t take = (lem & dm) | (gem & ~dm);
            int32_t lo = (other & take) | (sv & ~take);
            int32_t ne = (v != 0) & ((sv & 0xFFFF) != (~tt & 0xFFFF));
            vccl[i] = (uint16_t)(((lem & dm) | (tnm & ~dm)) & 1);
            vcch[i] = (uint16_t)(((tnm & dm) | (gem & ~dm)) & 1);
            vcol[i] = (uint16_t)(dm & 1);
            vcoh[i] = (uint16_t)ne;
            vce[i] = (uint16_t)(dm & (v == -1) & 1);
            accl[i] = (uint16_t)lo;
            r[i] = (int16_t)lo;
        }
        break;
    case 0x26: /* VCR: one's complement clip */
        for (int i = 0; i < 8; i++) {
            int32_t sv = a[i], tt = t[i];
            int32_t dm = (sv ^ tt) >> 31, tnm = tt >> 31;
            int32_t v = sv + (tt ^ ~dm); /* a + b (signs differ) or a - b - 1 */
            int32_t take = (-(int32_t)(v + 1 <= 0) & dm) | (-(int32_t)(v + 1 >= 0) & ~dm);
            int32_t other = tt ^ dm; /* ~b or b */
            int32_t lo = (other & take) | (sv & ~take);
            vccl[i] = (uint16_t)(((take & dm) | (tnm & ~dm)) & 1);
            vcch[i] = (uint16_t)(((tnm & dm) | (take & ~dm)) & 1);
            accl[i] = (uint16_t)lo;
            r[i] = (int16_t)lo;
            vcol[i] = vcoh[i] = vce[i] = 0;
        }
        break;
    case 0x27: /* VMRG: VCC low picks a */
        for (int i = 0; i < 8; i++) {
            r[i] = vccl[i] ? a[i] : t[i];
            accl[i] = (uint16_t)r[i];
            vcol[i] = vcoh[i] = 0;
        }
        break;
    case 0x28: /* VAND */
        for (int i = 0; i < 8; i++)
            accl[i] = (uint16_t)(r[i] = (int16_t)(a[i] & t[i]));
        break;
    case 0x29: /* VNAND */
        for (int i = 0; i < 8; i++)
            accl[i] = (uint16_t)(r[i] = (int16_t)~(a[i] & t[i]));
        break;
    case 0x2A: /* VOR */
        for (int i = 0; i < 8; i++)
            accl[i] = (uint16_t)(r[i] = (int16_t)(a[i] | t[i]));
        break;
    case 0x2B: /* VNOR */
        for (int i = 0; i < 8; i++)
            accl[i] = (uint16_t)(r[i] = (int16_t)~(a[i] | t[i]));
        break;
    case 0x2C: /* VXOR */
        for (int i = 0; i < 8; i++)
            accl[i] = (uint16_t)(r[i] = (int16_t)(a[i] ^ t[i]));
        break;
    case 0x2D: /* VNXOR */
        for (int i = 0; i < 8; i++)
            accl[i] = (uint16_t)(r[i] = (int16_t)~(a[i] ^ t[i]));
        break;
    case 0x30: /* VRCP */
    case 0x31: /* VRCPL */
    case 0x34: /* VRSQ */
    case 0x35: /* VRSQL */
    {
        int low = op & 1, sq = (op & 4) != 0;
        int32_t input = low && s->divdp ? (int32_t)((uint32_t)(uint16_t)s->divin << 16 | (uint16_t)tv[e & 7])
                                         : (int32_t)tv[e & 7];
        memcpy(accl, t, sizeof t);
        vu_divide(s, sq, input, &s->vr[vd][vs & 7]);
        return;
    }
    case 0x32: /* VRCPH */
    case 0x36: /* VRSQH */
        memcpy(accl, t, sizeof t);
        s->divdp = 1;
        s->divin = tv[e & 7];
        s->vr[vd][vs & 7] = s->divout;
        return;
    case 0x33: /* VMOV: vd[de] = (b with the selector)[de] */
        memcpy(accl, t, sizeof t);
        s->vr[vd][vs & 7] = t[vs & 7];
        return;
    case 0x37: /* VNOP */
    case 0x3F: /* VNULL */
        return;
    default: /* reserved opcodes: the accumulator takes a + b, the result is 0 */
        for (int i = 0; i < 8; i++) {
            accl[i] = (uint16_t)(a[i] + t[i]);
            r[i] = 0;
        }
        break;
    }
    memcpy(s->vr[vd], r, sizeof r);
}

/* ---- Vector unit: loads and stores (LWC2 / SWC2) ------------------------------------------------------------------ */

/* A register as its 16 big-endian bytes (byte 0 = high byte of element 0) and back: the byte-granular loads and
 * stores work on this view. */
static inline void vr_unpack(const int16_t *v, uint8_t *b)
{
    for (int i = 0; i < 8; i++) {
        b[2 * i] = (uint8_t)((uint16_t)v[i] >> 8);
        b[2 * i + 1] = (uint8_t)v[i];
    }
}

static inline void vr_pack(const uint8_t *b, int16_t *v)
{
    for (int i = 0; i < 8; i++)
        v[i] = (int16_t)(uint16_t)(b[2 * i] << 8 | b[2 * i + 1]);
}

/* Four or eight lanes from / to big-endian halfwords (fixed counts, so the portable loops vectorise too). */
static inline void ld_be16x8(int16_t *v, const uint8_t *p)
{
#if RSP_NEON
    vst1q_s16(v, vreinterpretq_s16_u8(vrev16q_u8(vld1q_u8(p))));
#else
    for (int k = 0; k < 8; k++)
        v[k] = (int16_t)n64_be16(p + 2 * k);
#endif
}

static inline void ld_be16x4(int16_t *v, const uint8_t *p)
{
#if RSP_NEON
    vst1_s16(v, vreinterpret_s16_u8(vrev16_u8(vld1_u8(p))));
#else
    for (int k = 0; k < 4; k++)
        v[k] = (int16_t)n64_be16(p + 2 * k);
#endif
}

static inline void st_be16x8(uint8_t *p, const int16_t *v)
{
#if RSP_NEON
    vst1q_u8(p, vrev16q_u8(vreinterpretq_u8_s16(vld1q_s16(v))));
#else
    for (int k = 0; k < 8; k++)
        n64_wbe16(p + 2 * k, (uint16_t)v[k]);
#endif
}

static inline void st_be16x4(uint8_t *p, const int16_t *v)
{
#if RSP_NEON
    vst1_u8(p, vrev16_u8(vreinterpret_u8_s16(vld1_s16(v))));
#else
    for (int k = 0; k < 4; k++)
        n64_wbe16(p + 2 * k, (uint16_t)v[k]);
#endif
}

/* n bytes of DMEM from addr (wrapping at 4 KB) into dst. */
static inline void dm_read(const n64_rsp *s, uint32_t addr, uint8_t *dst, unsigned n)
{
    addr &= 0xFFF;
    if (addr + n <= 0x1000) {
        memcpy(dst, s->mem + addr, n);
        return;
    }
    for (unsigned i = 0; i < n; i++)
        dst[i] = s->mem[(addr + i) & 0xFFF];
}

static inline void dm_write(n64_rsp *s, uint32_t addr, const uint8_t *src, unsigned n)
{
    addr &= 0xFFF;
    if (addr + n <= 0x1000) {
        memcpy(s->mem + addr, src, n);
        return;
    }
    for (unsigned i = 0; i < n; i++)
        s->mem[(addr + i) & 0xFFF] = src[i];
}

/* n bytes of DMEM from addr into register bytes e.. (e + n <= 16); whole halfwords when aligned in the register. */
static inline void ld_bytes(n64_rsp *s, int16_t *v, unsigned e, uint32_t addr, unsigned n)
{
    addr &= 0xFFF;
    if (!((e | n) & 1) && addr + n <= 0x1000) {
        const uint8_t *p = s->mem + addr;
        if (n == 16) { /* LQV / LRV of a whole aligned register: e = 0 */
            ld_be16x8(v, p);
            return;
        }
        if (n == 8) { /* LDV */
            ld_be16x4(v + e / 2, p);
            return;
        }
        for (unsigned k = 0; k < n / 2; k++)
            v[e / 2 + k] = (int16_t)n64_be16(p + 2 * k);
        return;
    }
    for (unsigned k = 0; k < n; k++)
        vsetb(v, e + k, s->mem[(addr + k) & 0xFFF]);
}

/* Register bytes e.. (wrapping at 16) to n bytes of DMEM from addr. */
static inline void st_bytes(n64_rsp *s, const int16_t *v, unsigned e, uint32_t addr, unsigned n)
{
    addr &= 0xFFF;
    if (!((e | n) & 1) && addr + n <= 0x1000) {
        uint8_t *p = s->mem + addr;
        if (n == 16 && e == 0) { /* SQV of an aligned register */
            st_be16x8(p, v);
            return;
        }
        if (n == 8 && e <= 8) { /* SDV */
            st_be16x4(p, v + e / 2);
            return;
        }
        for (unsigned k = 0; k < n / 2; k++)
            n64_wbe16(p + 2 * k, (uint16_t)v[(e / 2 + k) & 7]);
        return;
    }
    for (unsigned k = 0; k < n; k++)
        s->mem[(addr + k) & 0xFFF] = vgetb(v, (e + k) & 15);
}

static void vu_load(n64_rsp *s, uint32_t op)
{
    unsigned vt = (op >> 16) & 31, f = (op >> 11) & 31, e = (op >> 7) & 15;
    int32_t off = (int32_t)(op << 25) >> 25;
    uint32_t base = s->r[(op >> 21) & 31];
    int16_t *v = s->vr[vt];
    uint8_t vb[16];
    uint32_t addr;
    switch (f) {
    case 0x00: /* LBV */
    case 0x01: /* LSV */
    case 0x02: /* LLV */
    case 0x03: /* LDV: 1 << f bytes into bytes e.., none past byte 15 */
    {
        unsigned n = 1u << f;
        addr = base + (uint32_t)(off * (int32_t)n);
        ld_bytes(s, v, e, addr, e + n > 16 ? 16 - e : n);
        break;
    }
    case 0x04: /* LQV: bytes e.. up to the 16-byte boundary of the address */
    {
        addr = base + (uint32_t)(off * 16);
        unsigned n = 16 - (addr & 15);
        ld_bytes(s, v, e, addr, n > 16 - e ? 16 - e : n);
        break;
    }
    case 0x05: /* LRV: the bytes before the address in its 16-byte block, into the end of the register */
    {
        addr = base + (uint32_t)(off * 16);
        unsigned start = 16 - ((addr & 15) - e); /* (addr & 15) < e: start > 16, nothing loaded */
        if (start < 16)
            ld_bytes(s, v, start, addr & ~15u, 16 - start);
        break;
    }
    case 0x06: /* LPV: 8 bytes << 8 into the lanes, from a 16-byte window */
    case 0x07: /* LUV: << 7 */
    {
        addr = base + (uint32_t)(off * 8);
        unsigned idx = (addr & 7) - e;
        unsigned sh = f == 6 ? 8 : 7;
        uint8_t w[16];
        dm_read(s, addr & ~7u, w, 16);
        for (unsigned i = 0; i < 8; i++)
            v[i] = (int16_t)(uint16_t)(w[(idx + i) & 15] << sh);
        break;
    }
    case 0x08: /* LHV: every other byte << 7 */
    {
        addr = base + (uint32_t)(off * 16);
        unsigned idx = (addr & 7) - e;
        uint8_t w[16];
        dm_read(s, addr & ~7u, w, 16);
        for (unsigned i = 0; i < 8; i++)
            v[i] = (int16_t)(uint16_t)(w[(idx + i * 2) & 15] << 7);
        break;
    }
    case 0x09: /* LFV: every fourth byte << 7 into four lanes (bytes e .. e + 7 of the register) */
    {
        addr = base + (uint32_t)(off * 16);
        unsigned idx = (addr & 7) - e;
        uint8_t w[16], tb[16];
        int16_t tmp[8];
        dm_read(s, addr & ~7u, w, 16);
        for (unsigned i = 0; i < 4; i++) {
            tmp[i] = (int16_t)(uint16_t)(w[(idx + i * 4) & 15] << 7);
            tmp[i + 4] = (int16_t)(uint16_t)(w[(idx + i * 4 + 8) & 15] << 7);
        }
        vr_unpack(tmp, tb);
        vr_unpack(v, vb);
        unsigned end = e + 8 > 16 ? 16 : e + 8;
        for (unsigned b = e; b < end; b++)
            vb[b] = tb[b];
        vr_pack(vb, v);
        break;
    }
    case 0x0B: /* LTV: element i of register (vt & ~7) + ((e / 2 + i) & 7) from halfword i of the 16-byte block */
    {
        addr = base + (uint32_t)(off * 16);
        uint32_t begin = addr & ~7u;
        unsigned pos = (e + (addr & 8)) & 15, vbase = vt & ~7u, voff = e >> 1;
        uint8_t w[16];
        dm_read(s, begin, w, 16);
        for (unsigned i = 0; i < 8; i++) {
            int16_t *dst = s->vr[vbase + voff];
            dst[i] = (int16_t)(uint16_t)(w[pos] << 8 | w[(pos + 1) & 15]);
            pos = (pos + 2) & 15;
            voff = (voff + 1) & 7;
        }
        break;
    }
    default: /* LWV and the reserved ones do nothing */
        break;
    }
}

static void vu_store(n64_rsp *s, uint32_t op)
{
    unsigned vt = (op >> 16) & 31, f = (op >> 11) & 31, e = (op >> 7) & 15;
    int32_t off = (int32_t)(op << 25) >> 25;
    uint32_t base = s->r[(op >> 21) & 31];
    const int16_t *v = s->vr[vt];
    uint8_t w[16];
    uint32_t addr;
    switch (f) {
    case 0x00: /* SBV */
    case 0x01: /* SSV */
    case 0x02: /* SLV */
    case 0x03: /* SDV: 1 << f bytes from bytes e.. (wrapping in the register) */
    {
        unsigned n = 1u << f;
        addr = base + (uint32_t)(off * (int32_t)n);
        st_bytes(s, v, e, addr, n);
        break;
    }
    case 0x04: /* SQV: bytes e.. (wrapping) up to the 16-byte boundary of the address */
        addr = base + (uint32_t)(off * 16);
        st_bytes(s, v, e, addr, 16 - (addr & 15));
        break;
    case 0x05: /* SRV: the bytes before the address in its 16-byte block */
    {
        addr = base + (uint32_t)(off * 16);
        unsigned n = addr & 15;
        st_bytes(s, v, (e + 16 - n) & 15, addr & ~15u, n);
        break;
    }
    case 0x06: /* SPV: high bytes of the lanes (byte index < 8), else lane >> 7 */
    case 0x07: /* SUV: the reverse */
    {
        addr = base + (uint32_t)(off * 8);
        for (unsigned i = 0; i < 8; i++) {
            unsigned b = e + i;
            int packed = ((b & 15) < 8) == (f == 6);
            w[i] = packed ? (uint8_t)((uint16_t)v[b & 7] >> 8) : (uint8_t)((uint16_t)v[b & 7] >> 7);
        }
        dm_write(s, addr, w, 8);
        break;
    }
    case 0x08: /* SHV: lane bits 14..7 to every other byte of a 16-byte window */
    {
        addr = base + (uint32_t)(off * 16);
        unsigned idx = addr & 7;
        addr &= ~7u;
        for (unsigned i = 0; i < 8; i++) {
            unsigned b = e + i * 2;
            DM(addr + ((idx + i * 2) & 15)) = (uint8_t)(vgetb(v, b & 15) << 1 | vgetb(v, (b + 1) & 15) >> 7);
        }
        break;
    }
    case 0x09: /* SFV: four lanes >> 7 to every fourth byte; only some elements select lanes, the others store 0 */
    {
        static const int8_t lanes[16][4] = {
            {0, 1, 2, 3}, {6, 7, 4, 5}, {-1}, {-1}, {1, 2, 3, 0}, {7, 4, 5, 6}, {-1}, {-1},
            {4, 5, 6, 7}, {-1}, {-1}, {3, 0, 1, 2}, {5, 6, 7, 4}, {-1}, {-1}, {0, 1, 2, 3},
        };
        addr = base + (uint32_t)(off * 16);
        unsigned bs = addr & 7;
        addr &= ~7u;
        for (unsigned i = 0; i < 4; i++) {
            int l = lanes[e][0] < 0 ? -1 : lanes[e][i];
            DM(addr + ((bs + i * 4) & 15)) = l < 0 ? 0 : (uint8_t)((uint16_t)v[l] >> 7);
        }
        break;
    }
    case 0x0A: /* SWV: bytes e.. (wrapping) to the 16-byte window, starting at the address' offset in it */
    {
        addr = base + (uint32_t)(off * 16);
        unsigned bs = addr & 7;
        addr &= ~7u;
        for (unsigned i = 0; i < 16; i++)
            DM(addr + ((bs + i) & 15)) = vgetb(v, (e + i) & 15);
        break;
    }
    case 0x0B: /* STV: element (e / 2 + k) & 7 of register (vt & ~7) + k to halfword k (a transpose) */
    {
        addr = base + (uint32_t)(off * 16);
        unsigned start = vt & ~7u, el = 16 - (e & ~1u), bs = (addr & 7) - (e & ~1u);
        addr &= ~7u;
        for (unsigned k = start; k < start + 8; k++) {
            DM(addr + (bs++ & 15)) = vgetb(s->vr[k], el++ & 15);
            DM(addr + (bs++ & 15)) = vgetb(s->vr[k], el++ & 15);
        }
        break;
    }
    default:
        break;
    }
}

/* ---- COP2 moves --------------------------------------------------------------------------------------------------- */

static uint32_t flags_get(const uint16_t *lo, const uint16_t *hi)
{
    uint32_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (uint32_t)(lo[i] & 1) << i | (hi ? (uint32_t)(hi[i] & 1) << (8 + i) : 0);
    return v;
}

static void flags_set(uint16_t *lo, uint16_t *hi, uint32_t v)
{
    for (int i = 0; i < 8; i++) {
        lo[i] = (uint16_t)(v >> i & 1);
        if (hi)
            hi[i] = (uint16_t)(v >> (8 + i) & 1);
    }
}

static void cop2_move(n64_rsp *s, uint32_t op)
{
    unsigned rt = (op >> 16) & 31, rd = (op >> 11) & 31, e = (op >> 7) & 15;
    switch ((op >> 21) & 31) {
    case 0: /* MFC2: bytes e and e + 1 (wrapping) of vs, sign-extended */
        s->r[rt] = (uint32_t)(int32_t)(int16_t)(uint16_t)(vgetb(s->vr[rd], e) << 8 | vgetb(s->vr[rd], (e + 1) & 15));
        break;
    case 2: /* CFC2 */
        switch (rd & 3) {
        case 0: s->r[rt] = (uint32_t)(int32_t)(int16_t)flags_get(s->vcol, s->vcoh); break;
        case 1: s->r[rt] = (uint32_t)(int32_t)(int16_t)flags_get(s->vccl, s->vcch); break;
        default: s->r[rt] = flags_get(s->vce, NULL); break;
        }
        break;
    case 4: /* MTC2: byte e and (unless e is 15) byte e + 1 */
        vsetb(s->vr[rd], e, (uint8_t)(s->r[rt] >> 8));
        if (e != 15)
            vsetb(s->vr[rd], e + 1, (uint8_t)s->r[rt]);
        break;
    case 6: /* CTC2 */
        switch (rd & 3) {
        case 0: flags_set(s->vcol, s->vcoh, s->r[rt]); break;
        case 1: flags_set(s->vccl, s->vcch, s->r[rt]); break;
        default: flags_set(s->vce, NULL, s->r[rt]); break;
        }
        break;
    default:
        break;
    }
}

/* ---- Scalar unit -------------------------------------------------------------------------------------------------- */

static inline uint32_t ld32(const n64_rsp *s, uint32_t a)
{
    a &= 0xFFF;
    if (a <= 0xFFC)
        return n64_be32(s->mem + a);
    return (uint32_t)DM(a) << 24 | (uint32_t)DM(a + 1) << 16 | (uint32_t)DM(a + 2) << 8 | DM(a + 3);
}

static inline uint16_t ld16(const n64_rsp *s, uint32_t a)
{
    return (uint16_t)(DM(a) << 8 | DM(a + 1));
}

static inline void st32(n64_rsp *s, uint32_t a, uint32_t v)
{
    a &= 0xFFF;
    if (a <= 0xFFC) {
        n64_wbe32(s->mem + a, v);
        return;
    }
    DM(a) = (uint8_t)(v >> 24);
    DM(a + 1) = (uint8_t)(v >> 16);
    DM(a + 2) = (uint8_t)(v >> 8);
    DM(a + 3) = (uint8_t)v;
}

uint32_t n64_rsp_run(n64_rsp *s, uint32_t cycles)
{
    if (s->status & ST_HALT)
        return 0;
    /* pc / npc live in locals: nothing the loop calls reads them (COP0 cannot reach SP_PC). The loop stops when an
     * instruction may have halted the RSP (BREAK, an SP_STATUS write) or single-stepping is on. */
    uint32_t n = 0, pc = s->pc, npc = s->npc;
    uint32_t *r = s->r;
    const uint8_t *imem = s->mem + 0x1000;
    int sstep = (s->status & ST_SSTEP) != 0;
    while (n < cycles) {
        uint32_t ia = pc;
        uint32_t op = n64_be32(imem + ia);
        if ((op >> 25) == 0x25) { /* COP2 computational, most of a microcode: before the scalar decode below */
            pc = npc;
            npc = (npc + 4) & 0xFFC;
            n++;
            vu_op(s, op);
            if (sstep) {
                if (s->status & ST_SSTEP)
                    s->status |= ST_HALT;
                if (s->status & ST_HALT)
                    break;
            }
            continue;
        }
        int stop = 0;
        uint32_t rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31;
        uint32_t imm = (uint32_t)(int32_t)(int16_t)op;
        uint32_t link = (ia + 8) & 0xFFC;
        uint32_t btarget = (ia + 4 + (imm << 2)) & 0xFFC;
        pc = npc;
        npc = (npc + 4) & 0xFFC;
        n++;
        switch (op >> 26) {
        case 0x00: /* SPECIAL */
            switch (op & 63) {
            case 0x00: r[rd] = r[rt] << ((op >> 6) & 31); break;
            case 0x02: r[rd] = r[rt] >> ((op >> 6) & 31); break;
            case 0x03: r[rd] = (uint32_t)((int32_t)r[rt] >> ((op >> 6) & 31)); break;
            case 0x04: r[rd] = r[rt] << (r[rs] & 31); break;
            case 0x06: r[rd] = r[rt] >> (r[rs] & 31); break;
            case 0x07: r[rd] = (uint32_t)((int32_t)r[rt] >> (r[rs] & 31)); break;
            case 0x08: npc = r[rs] & 0xFFC; break; /* JR */
            case 0x09: /* JALR */
            {
                uint32_t tgt = r[rs] & 0xFFC;
                r[rd] = link;
                npc = tgt;
                break;
            }
            case 0x0D: /* BREAK */
                s->status |= ST_HALT | ST_BROKE;
                if (s->status & ST_INTR_BREAK)
                    raise_sp(s);
                stop = 1;
                break;
            case 0x20:
            case 0x21: r[rd] = r[rs] + r[rt]; break;
            case 0x22:
            case 0x23: r[rd] = r[rs] - r[rt]; break;
            case 0x24: r[rd] = r[rs] & r[rt]; break;
            case 0x25: r[rd] = r[rs] | r[rt]; break;
            case 0x26: r[rd] = r[rs] ^ r[rt]; break;
            case 0x27: r[rd] = ~(r[rs] | r[rt]); break;
            case 0x2A: r[rd] = (int32_t)r[rs] < (int32_t)r[rt]; break;
            case 0x2B: r[rd] = r[rs] < r[rt]; break;
            /* Every other SPECIAL minor opcode behaves as SRLV rd, rs, rs (rt ignored), not a no-op
             * (sauraen/rsp_inval_instrs, measured on hardware). */
            default: r[rd] = r[rs] >> (r[rs] & 31); break;
            }
            break;
        case 0x01: /* REGIMM */
        {
            if (rt & 0x0E)
                break;
            int taken = (rt & 1) ? (int32_t)r[rs] >= 0 : (int32_t)r[rs] < 0;
            if (rt & 0x10)
                r[31] = link;
            if (taken)
                npc = btarget;
            break;
        }
        case 0x02: npc = (op << 2) & 0xFFC; break; /* J */
        case 0x03: /* JAL */
            r[31] = link;
            npc = (op << 2) & 0xFFC;
            break;
        case 0x04: if (r[rs] == r[rt]) npc = btarget; break;
        case 0x05: if (r[rs] != r[rt]) npc = btarget; break;
        case 0x06: if ((int32_t)r[rs] <= 0) npc = btarget; break;
        case 0x07: if ((int32_t)r[rs] > 0) npc = btarget; break;
        case 0x08:
        case 0x09: r[rt] = r[rs] + imm; break;
        case 0x0A: r[rt] = (int32_t)r[rs] < (int32_t)imm; break;
        case 0x0B: r[rt] = r[rs] < imm; break;
        case 0x0C: r[rt] = r[rs] & (imm & 0xFFFF); break;
        case 0x0D: r[rt] = r[rs] | (imm & 0xFFFF); break;
        case 0x0E: r[rt] = r[rs] ^ (imm & 0xFFFF); break;
        case 0x0F: r[rt] = imm << 16; break;
        case 0x10: /* COP0 */
            if (rs == 0)
                r[rt] = cop0_read(s, rd & 15);
            else if (rs == 4) {
                cop0_write(s, rd & 15, r[rt]);
                stop = 1; /* SP_STATUS may have halted the RSP or turned single-step on */
            }
            break;
        case 0x12: cop2_move(s, op); break; /* COP2 moves (the computational ops are above) */
        case 0x20: r[rt] = (uint32_t)(int32_t)(int8_t)DM(r[rs] + imm); break;             /* LB */
        case 0x21: r[rt] = (uint32_t)(int32_t)(int16_t)ld16(s, r[rs] + imm); break;       /* LH */
        case 0x23:                                                                         /* LW */
        case 0x27: r[rt] = ld32(s, r[rs] + imm); break;                                     /* LWU */
        case 0x24: r[rt] = DM(r[rs] + imm); break;                                          /* LBU */
        case 0x25: r[rt] = ld16(s, r[rs] + imm); break;                                     /* LHU */
        case 0x28: DM(r[rs] + imm) = (uint8_t)r[rt]; break;                                 /* SB */
        case 0x29:                                                                          /* SH */
            DM(r[rs] + imm) = (uint8_t)(r[rt] >> 8);
            DM(r[rs] + imm + 1) = (uint8_t)r[rt];
            break;
        case 0x2B: st32(s, r[rs] + imm, r[rt]); break; /* SW */
        case 0x32: vu_load(s, op); break;              /* LWC2 */
        case 0x3A: vu_store(s, op); break;             /* SWC2 */
        default: break;
        }
        r[0] = 0;
        if (stop | sstep) {
            if (s->status & ST_SSTEP)
                s->status |= ST_HALT;
            if (s->status & ST_HALT)
                break;
        }
    }
    s->pc = pc;
    s->npc = npc;
    return n;
}
