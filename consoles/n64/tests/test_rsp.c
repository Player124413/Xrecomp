/* N64 RSP tests (N-5, XBA-173), against n64_rsp.c through n64_devices.h only:
 *   1. hand-checked vectors (worked out from the n64brew RSP pages): scalar unit (delay slots, links, wrapping
 *      unaligned DMEM accesses, BREAK), SP_STATUS pairs and the MI SP interrupt, single-step, the semaphore, SP DMA
 *      (rows, skip, rounding, bank wraparound, register readback, RDRAM bounds, DMA from the RSP), the DPC
 *      registers through COP0, and the vector unit: multiplies and accumulator clamps, adds and flags, selects and
 *      clips, the divide unit (ROM values), element selectors, COP2 moves, every vector load / store with its
 *      element and wraparound rules;
 *   2. the differential groups of homebrew/common/rsp_oracle.h (random inputs over every vector funct, load, store,
 *      DMA and scalar op), whose checksums must equal those recorded from the ares oracle (rsp_oracle_expected.h).
 * Microcode is written with the encoders of homebrew/common/rsp_asm.h. No RDP library: a fake one below records the
 * DPC accesses (the RSP must work without a real RDP). `n64_rsp_test --print` prints our checksums. */
#include "n64_devices.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "homebrew/common/rsp_asm.h"
#include "homebrew/common/rsp_oracle.h"
#include "rsp_oracle_expected.h"

static int failures;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

/* ---- A fake RDP: the DPC registers as plain storage, run calls counted ---- */
struct n64_rdp {
    uint32_t regs[8];
    int writes, runs;
    const uint8_t *dmem;
};
static struct n64_rdp fake_rdp;
n64_rdp *n64_rdp_create(n64_rcp_bus bus)
{
    (void)bus;
    return &fake_rdp;
}
void n64_rdp_destroy(n64_rdp *d) { (void)d; }
void n64_rdp_reset(n64_rdp *d) { memset(d->regs, 0, sizeof d->regs); }
void n64_rdp_attach_dmem(n64_rdp *d, const uint8_t *dmem) { d->dmem = dmem; }
uint32_t n64_rdp_read_reg(n64_rdp *d, uint32_t reg) { return d->regs[reg & 7]; }
void n64_rdp_write_reg(n64_rdp *d, uint32_t reg, uint32_t v)
{
    d->regs[reg & 7] = v;
    d->writes++;
}
void n64_rdp_run(n64_rdp *d) { d->runs++; }

/* ---- Harness ---- */
#define RDRAM_SIZE (4u << 20)
static uint8_t *rdram;
static int irq_line, irq_raises, irq_clears;
static void bus_raise(void *ctx, int irq)
{
    (void)ctx;
    if (irq == N64_MI_SP) {
        irq_line = 1;
        irq_raises++;
    }
}
static void bus_clear(void *ctx, int irq)
{
    (void)ctx;
    if (irq == N64_MI_SP) {
        irq_line = 0;
        irq_clears++;
    }
}

static n64_rsp *S;
static uint8_t *M; /* DMEM then IMEM */

/* The RDRAM write notices (bus.wrote): every byte noticed, and how many notices. */
static uint8_t *wrote_map;
static int wrote_calls;
static void bus_wrote(void *ctx, uint32_t addr, uint32_t len)
{
    (void)ctx;
    wrote_calls++;
    for (uint32_t i = 0; i < len; i++)
        if (addr + i < RDRAM_SIZE)
            wrote_map[addr + i] = 1;
}

static void new_rsp(void)
{
    n64_rcp_bus bus = {rdram, RDRAM_SIZE, bus_raise, bus_clear, NULL, wrote_map ? bus_wrote : NULL, NULL};
    if (S)
        n64_rsp_destroy(S);
    S = n64_rsp_create(bus);
    M = n64_rsp_mem(S);
    memset(M, 0, 0x2000);
    irq_line = irq_raises = irq_clears = 0;
}

static void load(const uint32_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        n64_wbe32(M + 0x1000 + 4 * i, p[i]);
}
#define LOAD(...)                                                                                                      \
    do {                                                                                                               \
        const uint32_t prog_[] = {__VA_ARGS__};                                                                        \
        load(prog_, sizeof prog_ / sizeof prog_[0]);                                                                   \
    } while (0)

/* Starts at PC 0 and runs to the halt; returns the cycles used. */
static uint32_t go(void)
{
    n64_rsp_write_reg(S, 8, 0);
    n64_rsp_write_reg(S, 4, 0x5);
    uint32_t n = n64_rsp_run(S, 1000000);
    CHECK(n64_rsp_halted(S), "program did not halt");
    return n;
}

static uint32_t r32(uint32_t a) { return n64_be32(M + a); }
static uint16_t r16(uint32_t a) { return n64_be16(M + a); }
static void w32(uint32_t a, uint32_t v) { n64_wbe32(M + a, v); }
static void w16(uint32_t a, uint16_t v) { n64_wbe16(M + a, v); }
static void wvec(uint32_t a, const uint16_t v[8])
{
    for (int i = 0; i < 8; i++)
        w16(a + 2 * i, v[i]);
}
static int vec_eq(uint32_t a, const uint16_t v[8])
{
    for (int i = 0; i < 8; i++)
        if (r16(a + 2 * i) != v[i])
            return 0;
    return 1;
}
static void dump(const char *what, uint32_t a)
{
    printf("  %s @%03X:", what, a);
    for (int i = 0; i < 8; i++)
        printf(" %04X", r16(a + 2 * i));
    printf("\n");
}
#define CHECKV(a, ...)                                                                                                 \
    do {                                                                                                               \
        const uint16_t want_[8] = {__VA_ARGS__};                                                                       \
        if (!vec_eq(a, want_)) {                                                                                       \
            CHECK(0, "vector at %03X", (unsigned)(a));                                                                 \
            dump("got ", a);                                                                                           \
            printf("  want    :");                                                                                     \
            for (int i_ = 0; i_ < 8; i_++)                                                                             \
                printf(" %04X", want_[i_]);                                                                            \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

/* Vector op harness: v1 = A (0x000), v2 = B (0x010), VCO / VCC / VCE from 0x040 / 0x044 / 0x048, then `pre` (up to
 * 4 instructions, e.g. setting the accumulator), the op as v3 = op(v1, v2[e]), and out: v3 at 0x100, ACCH / ACCM /
 * ACCL at 0x110 / 0x120 / 0x130, VCO / VCC / VCE words at 0x140 / 0x144 / 0x148. */
static void vrun(const uint16_t a[8], const uint16_t b[8], uint32_t vco, uint32_t vcc, uint32_t vce,
                 const uint32_t *pre, int npre, uint32_t op)
{
    uint32_t p[48];
    int n = 0;
    p[n++] = R_LQV(1, 0, 0, 0);
    p[n++] = R_LQV(2, 0, 1, 0);
    for (int i = 0; i < npre; i++)
        p[n++] = pre[i];
    p[n++] = R_LW(5, 0x40, 0);
    p[n++] = R_CTC2(5, 0);
    p[n++] = R_LW(5, 0x44, 0);
    p[n++] = R_CTC2(5, 1);
    p[n++] = R_LW(5, 0x48, 0);
    p[n++] = R_CTC2(5, 2);
    p[n++] = op;
    p[n++] = R_SQV(3, 0, 16, 0);
    p[n++] = R_VOP(RV_VSAR, 4, 0, 0, 8);
    p[n++] = R_SQV(4, 0, 17, 0);
    p[n++] = R_VOP(RV_VSAR, 4, 0, 0, 9);
    p[n++] = R_SQV(4, 0, 18, 0);
    p[n++] = R_VOP(RV_VSAR, 4, 0, 0, 10);
    p[n++] = R_SQV(4, 0, 19, 0);
    p[n++] = R_CFC2(5, 0);
    p[n++] = R_SW(5, 0x140, 0);
    p[n++] = R_CFC2(5, 1);
    p[n++] = R_SW(5, 0x144, 0);
    p[n++] = R_CFC2(5, 2);
    p[n++] = R_SW(5, 0x148, 0);
    p[n++] = R_BREAK;
    load(p, (size_t)n);
    wvec(0x000, a);
    wvec(0x010, b);
    w32(0x40, vco);
    w32(0x44, vcc);
    w32(0x48, vce);
    go();
}
#define VEC(...) ((const uint16_t[8]){__VA_ARGS__})

/* ---- 1a. Scalar unit ---- */
static void test_scalar(void)
{
    new_rsp();
    /* Delay slots: the instruction after a taken branch runs; the link is the address after the delay slot. */
    LOAD(R_ORI(1, 0, 5),             /* 0x00 */
         R_BEQ(1, 1, 2),             /* 0x04 -> 0x10 */
         R_ADDIU(2, 0, 7),           /* 0x08 delay slot */
         R_ADDIU(2, 0, 99),          /* 0x0C skipped */
         R_JAL(0x20),                /* 0x10 */
         R_ADDIU(3, 0, 1),           /* 0x14 delay slot */
         R_BREAK,                    /* 0x18 */
         R_NOP,                      /* 0x1C */
         R_SW(31, 0x10, 0),          /* 0x20: r31 = 0x18 */
         R_SW(2, 0x14, 0),
         R_SW(3, 0x18, 0),
         R_ADDIU(4, 0, -1),
         R_BGEZAL(4, 5),             /* 0x30: not taken, links anyway: r31 = 0x38 */
         R_NOP,
         R_SW(31, 0x1C, 0),
         R_JR(31),                   /* 0x3C -> 0x38 ... */
         R_ADDIU(31, 0, 0x18),       /* delay slot changes r31 after JR read it */
         );
    /* The JR at 0x3C jumps to 0x38 (the SW again, then the JR to 0x18 = BREAK). */
    uint32_t cycles = go();
    CHECK(r32(0x10) == 0x18, "JAL link %08X", r32(0x10));
    CHECK(r32(0x14) == 7, "delay slot / skip %u", r32(0x14));
    CHECK(r32(0x18) == 1, "JAL delay slot %u", r32(0x18));
    CHECK(r32(0x1C) == 0x18, "BGEZAL link then rewritten %08X", r32(0x1C));
    CHECK(cycles == 18, "cycles %u", cycles);
    CHECK(n64_rsp_read_reg(S, 8) == 0x1C, "PC after BREAK %03X", n64_rsp_read_reg(S, 8));
    CHECK(n64_rsp_read_reg(S, 4) & 0x2, "broke set");

    /* Unaligned and wrapping loads / stores; ADD never traps; r0 stays 0. */
    new_rsp();
    M[0xFFF] = 0x81;
    M[0x000] = 0x23;
    M[0x001] = 0x45;
    M[0x002] = 0x67;
    LOAD(R_LH(1, 0xFFF, 0), R_LHU(2, 0xFFF, 0), R_LW(3, 0xFFF, 0), R_LB(4, 0xFFF, 0), R_LBU(5, -1, 0),
         R_LUI(6, 0x7FFF), R_ORI(6, 6, 0xFFFF), R_ADD(7, 6, 6), R_ADDIU(0, 0, 5), R_LUI(8, 0xA1B2),
         R_ORI(8, 8, 0xC3D4), R_SW(8, 0xFFE, 0), R_SW(1, 0x100, 0), R_SW(2, 0x104, 0), R_SW(3, 0x108, 0),
         R_SW(4, 0x10C, 0), R_SW(5, 0x110, 0), R_SW(7, 0x114, 0), R_SW(0, 0x118, 0), R_BREAK);
    go();
    CHECK(r32(0x100) == 0xFFFF8123u, "LH wrap %08X", r32(0x100));
    CHECK(r32(0x104) == 0x8123u, "LHU wrap %08X", r32(0x104));
    CHECK(r32(0x108) == 0x81234567u, "LW wrap %08X", r32(0x108));
    CHECK(r32(0x10C) == 0xFFFFFF81u && r32(0x110) == 0x81, "LB / LBU %08X %08X", r32(0x10C), r32(0x110));
    CHECK(r32(0x114) == 0xFFFFFFFEu, "ADD wraps %08X", r32(0x114));
    CHECK(r32(0x118) == 0, "r0 %08X", r32(0x118));
    CHECK(M[0xFFE] == 0xA1 && M[0xFFF] == 0xB2 && M[0x000] == 0xC3 && M[0x001] == 0xD4, "SW wrap %02X%02X%02X%02X",
          M[0xFFE], M[0xFFF], M[0], M[1]);

    /* Branch in a delay slot (MIPS pipeline order): B1 at 0, B2 in its slot; B1's target runs one instruction,
     * then B2's target. */
    new_rsp();
    LOAD(R_BEQ(0, 0, 3),    /* 0x00 -> 0x10 */
         R_BEQ(0, 0, 5),    /* 0x04 (delay slot) -> 0x1C */
         R_ADDIU(1, 1, 1),  /* 0x08 */
         R_ADDIU(1, 1, 2),  /* 0x0C */
         R_ADDIU(1, 1, 4),  /* 0x10: runs once (B1's target) */
         R_ADDIU(1, 1, 8),  /* 0x14 */
         R_ADDIU(1, 1, 16), /* 0x18 */
         R_SW(1, 0, 0),     /* 0x1C: B2's target */
         R_BREAK);
    go();
    CHECK(r32(0) == 4, "branch in delay slot %u", r32(0));

    /* The run budget: an endless loop uses exactly the cycles it is given. */
    new_rsp();
    LOAD(R_J(0), R_NOP);
    n64_rsp_write_reg(S, 8, 0);
    n64_rsp_write_reg(S, 4, 0x1);
    CHECK(n64_rsp_run(S, 1000) == 1000 && !n64_rsp_halted(S), "budget");
    n64_rsp_write_reg(S, 4, 0x2); /* set halt from the CPU */
    CHECK(n64_rsp_run(S, 1000) == 0, "halted runs nothing");
    CHECK(!(n64_rsp_read_reg(S, 4) & 2), "a CPU halt is not a break");
}

/* ---- 1b. SP_STATUS, interrupts, single-step, semaphore ---- */
static void test_status(void)
{
    new_rsp();
    CHECK(n64_rsp_read_reg(S, 4) == 0x1, "reset status %08X", n64_rsp_read_reg(S, 4));
    n64_rsp_write_reg(S, 4, 1u << 4);
    CHECK(irq_line && irq_raises == 1, "set intr");
    n64_rsp_write_reg(S, 4, 3u << 3);
    CHECK(irq_line && irq_clears == 0, "set + clear intr: no change");
    n64_rsp_write_reg(S, 4, 1u << 3);
    CHECK(!irq_line && irq_clears == 1, "clear intr");
    n64_rsp_write_reg(S, 4, 1u << 10 | 1u << 24 | 1u << 8 | 1u << 6);
    CHECK(n64_rsp_read_reg(S, 4) == (0x1u | 0x20 | 0x40 | 0x80 | 0x4000), "signals 0 / 7, intr-break, sstep %08X",
          n64_rsp_read_reg(S, 4));
    n64_rsp_write_reg(S, 4, 3u << 9 | 1u << 23 | 1u << 5 | 1u << 7);
    CHECK(n64_rsp_read_reg(S, 4) == (0x1u | 0x80), "pair both set keeps sig0, clears the rest %08X",
          n64_rsp_read_reg(S, 4));
    n64_rsp_write_reg(S, 4, 1u << 9);

    /* BREAK with intr-break raises the SP interrupt; broke stays until cleared. */
    LOAD(R_ORI(1, 0, 1u << 12), R_MTC0(1, 4), R_BREAK); /* the RSP sets signal 1 itself */
    n64_rsp_write_reg(S, 4, 1u << 8);
    go();
    CHECK(irq_line && irq_raises == 2, "BREAK interrupt");
    CHECK((n64_rsp_read_reg(S, 4) & 0x143) == 0x143, "halt + broke + intr-break + sig1 %08X", n64_rsp_read_reg(S, 4));
    n64_rsp_write_reg(S, 4, 1u << 2 | 1u << 3 | 1u << 7);
    CHECK(!(n64_rsp_read_reg(S, 4) & 0x42) && !irq_line, "broke / intr cleared %08X", n64_rsp_read_reg(S, 4));
    LOAD(R_BREAK);
    go();
    CHECK(irq_raises == 2, "no interrupt without intr-break");
    /* The RSP raising the interrupt through SP_STATUS (MTC0) and halting itself. */
    LOAD(R_ORI(1, 0, 1u << 4 | 1u << 1), R_MTC0(1, 4), R_ADDIU(2, 0, 1), R_SW(2, 0, 0), R_BREAK);
    w32(0, 0);
    go();
    CHECK(irq_line && irq_raises == 3, "MTC0 SP_STATUS set intr");
    CHECK(r32(0) == 0 && n64_rsp_read_reg(S, 8) == 0x8, "MTC0 set halt stops at once (PC %03X)",
          n64_rsp_read_reg(S, 8));

    /* Single-step: one instruction per halt clear. */
    LOAD(R_ORI(1, 0, 0), R_ADDIU(1, 1, 1), R_ADDIU(1, 1, 1), R_SW(1, 0x20, 0), R_BREAK);
    n64_rsp_write_reg(S, 8, 0);
    n64_rsp_write_reg(S, 4, 1u << 6);
    n64_rsp_write_reg(S, 4, 0x1);
    CHECK(n64_rsp_run(S, 100) == 1 && n64_rsp_halted(S) && n64_rsp_read_reg(S, 8) == 4, "single step");
    n64_rsp_write_reg(S, 4, 0x1);
    CHECK(n64_rsp_run(S, 100) == 1 && n64_rsp_read_reg(S, 8) == 8, "single step again");
    n64_rsp_write_reg(S, 4, 0x1 | 1u << 5);
    n64_rsp_run(S, 100);
    CHECK(r32(0x20) == 2, "resumed after single step %u", r32(0x20));

    /* Semaphore: a read returns it and sets it; any write clears it. MFC0 from the RSP does the same. */
    CHECK(n64_rsp_read_reg(S, 7) == 0 && n64_rsp_read_reg(S, 7) == 1, "semaphore read-set");
    n64_rsp_write_reg(S, 7, 0xFFFFFFFFu);
    LOAD(R_MFC0(1, 7), R_MFC0(2, 7), R_MFC0(3, 5), R_MFC0(4, 6), R_SW(1, 0, 0), R_SW(2, 4, 0), R_SW(3, 8, 0),
         R_SW(4, 12, 0), R_MTC0(0, 7), R_BREAK);
    go();
    CHECK(r32(0) == 0 && r32(4) == 1 && r32(8) == 0 && r32(12) == 0, "MFC0 semaphore / DMA_FULL / DMA_BUSY");
    CHECK(n64_rsp_read_reg(S, 7) == 0, "MTC0 semaphore clears");
}

/* ---- 1c. SP DMA ---- */
static void test_dma(void)
{
    new_rsp();
    for (uint32_t i = 0; i < 0x4000; i++)
        rdram[0x1000 + i] = (uint8_t)(i * 7 + 1);
    /* Two rows of 16 bytes, skip 8: RDRAM 0x1000..0x100F and 0x1018..0x1027 -> DMEM 0x000..0x01F. */
    n64_rsp_write_reg(S, 0, 0x000);
    n64_rsp_write_reg(S, 1, 0x1000);
    n64_rsp_write_reg(S, 2, 0x00F | 1u << 12 | 0xFu << 20); /* skip 15 -> 8 */
    CHECK(M[0x00] == rdram[0x1000] && M[0x0F] == rdram[0x100F] && M[0x10] == rdram[0x1018] &&
              M[0x1F] == rdram[0x1027] && M[0x20] == 0,
          "RD rows / skip");
    CHECK(n64_rsp_read_reg(S, 0) == 0x020 && n64_rsp_read_reg(S, 1) == 0x1028, "addresses after (no skip after the last row) %08X %08X",
          n64_rsp_read_reg(S, 0), n64_rsp_read_reg(S, 1));
    CHECK(n64_rsp_read_reg(S, 2) == 0x00800FF8u && n64_rsp_read_reg(S, 3) == 0x00800FF8u, "length readback %08X",
          n64_rsp_read_reg(S, 2));
    CHECK(!(n64_rsp_read_reg(S, 4) & 0xC) && !n64_rsp_read_reg(S, 5) && !n64_rsp_read_reg(S, 6), "DMA idle");

    /* Unaligned addresses are rounded down, a length of 0 moves 8 bytes, length 9 moves 16. */
    memset(rdram + 0x2000, 0, 64);
    for (int i = 0; i < 32; i++)
        M[0x100 + i] = (uint8_t)(0xA0 + i);
    n64_rsp_write_reg(S, 0, 0x103);
    n64_rsp_write_reg(S, 1, 0x2005);
    n64_rsp_write_reg(S, 3, 0);
    CHECK(rdram[0x2000] == 0xA0 && rdram[0x2007] == 0xA7 && rdram[0x2008] == 0, "WR length 0 = 8 bytes");
    CHECK(n64_rsp_read_reg(S, 0) == 0x108 && n64_rsp_read_reg(S, 1) == 0x2008, "WR addresses");
    n64_rsp_write_reg(S, 0, 0x100);
    n64_rsp_write_reg(S, 1, 0x2010);
    n64_rsp_write_reg(S, 3, 9);
    CHECK(rdram[0x201F] == 0xAF && rdram[0x2020] == 0, "WR length 9 = 16 bytes");

    /* SP memory wraps inside its bank: DMEM end -> DMEM start, IMEM end -> IMEM start. */
    n64_rsp_write_reg(S, 0, 0xFF8);
    n64_rsp_write_reg(S, 1, 0x1000);
    n64_rsp_write_reg(S, 2, 0x00F);
    CHECK(M[0xFF8] == rdram[0x1000] && M[0x000] == rdram[0x1008] && M[0x007] == rdram[0x100F] && M[0x1000] == 0,
          "DMEM wrap");
    CHECK(n64_rsp_read_reg(S, 0) == 0x008, "DMEM wrap address %03X", n64_rsp_read_reg(S, 0));
    n64_rsp_write_reg(S, 0, 0x1FF8);
    n64_rsp_write_reg(S, 1, 0x1000);
    n64_rsp_write_reg(S, 2, 0x00F);
    CHECK(M[0x1FF8] == rdram[0x1000] && M[0x1000] == rdram[0x1008] && n64_rsp_read_reg(S, 0) == 0x1008, "IMEM wrap");

    /* Past the end of RDRAM: reads give 0, writes are dropped; the DRAM address wraps at 16 MB. */
    memset(M, 0x55, 16);
    n64_rsp_write_reg(S, 0, 0);
    n64_rsp_write_reg(S, 1, RDRAM_SIZE - 8);
    n64_rsp_write_reg(S, 2, 0x00F);
    CHECK(M[0] == rdram[RDRAM_SIZE - 8] && M[8] == 0 && M[15] == 0, "RD past the end");
    n64_rsp_write_reg(S, 1, 0xFFFFF8);
    n64_rsp_write_reg(S, 3, 0x00F);
    CHECK(n64_rsp_read_reg(S, 1) == 0x000008, "DRAM address wraps %06X", n64_rsp_read_reg(S, 1));

    /* DMA from the RSP (COP0 0..3), waiting on DMA_BUSY as microcode does. */
    new_rsp();
    for (int i = 0; i < 64; i++)
        rdram[0x3000 + i] = (uint8_t)(0x40 + i);
    LOAD(R_ORI(1, 0, 0x200), R_MTC0(1, 0), R_ORI(2, 0, 0x3000), R_MTC0(2, 1), R_ORI(3, 0, 0x3F), R_MTC0(3, 2),
         R_MFC0(4, 6), R_BNE(4, 0, -2), R_NOP, R_LW(5, 0x200, 0), R_LW(6, 0x23C, 0), R_ORI(1, 0, 0x200),
         R_MTC0(1, 0), R_ORI(2, 0, 0x3100), R_MTC0(2, 1), R_MTC0(3, 3), R_MFC0(7, 0), R_MFC0(8, 1), R_MFC0(9, 2),
         R_SW(5, 0x300, 0), R_SW(6, 0x304, 0), R_SW(7, 0x308, 0), R_SW(8, 0x30C, 0), R_SW(9, 0x310, 0), R_BREAK);
    go();
    CHECK(r32(0x300) == 0x40414243u && r32(0x304) == 0x7C7D7E7Fu, "RSP DMA in %08X %08X", r32(0x300), r32(0x304));
    CHECK(memcmp(rdram + 0x3100, rdram + 0x3000, 64) == 0, "RSP DMA out");
    CHECK(r32(0x308) == 0x240 && r32(0x30C) == 0x3140 && r32(0x310) == 0xFF8, "RSP DMA readback %X %X %X",
          r32(0x308), r32(0x30C), r32(0x310));

    /* DMEM -> RDRAM notices (XBA-191: a run per row, split where the bank or RDRAM ends): exactly the bytes written,
     * nothing past RDRAM's end, and far fewer notices than 8-byte steps. */
    wrote_map = calloc(RDRAM_SIZE, 1);
    new_rsp();
    for (int i = 0; i < 0x1000; i++)
        M[i] = (uint8_t)(i * 13 + 5);
    memset(rdram + 0x5000, 0, 0x400);
    wrote_calls = 0;
    n64_rsp_write_reg(S, 0, 0xF80);                         /* 3 rows of 256 bytes, skip 64: the first wraps the bank */
    n64_rsp_write_reg(S, 1, 0x5000);
    n64_rsp_write_reg(S, 3, 0x0FF | 2u << 12 | 0x40u << 20);
    int ok = 1;
    for (uint32_t a = 0x4F00; a < 0x5500 && ok; a++) {
        uint32_t k = a - 0x5000, row = k / 320, col = k % 320;
        int in = a >= 0x5000 && row < 3 && col < 256;
        uint8_t want = in ? M[(0xF80 + row * 256 + col) & 0xFFF] : 0;
        ok = wrote_map[a] == in && (!in || rdram[a] == want);
    }
    CHECK(ok, "WR rows: written bytes and notices");
    CHECK(wrote_calls == 4, "WR notices: one per run (%d)", wrote_calls);
    CHECK(n64_rsp_read_reg(S, 0) == 0x280 && n64_rsp_read_reg(S, 1) == 0x5000 + 2 * 320 + 256, "WR rows addresses");
    memset(wrote_map, 0, RDRAM_SIZE);
    n64_rsp_write_reg(S, 0, 0);
    n64_rsp_write_reg(S, 1, RDRAM_SIZE - 16);
    n64_rsp_write_reg(S, 3, 0x03F);
    CHECK(wrote_map[RDRAM_SIZE - 16] && wrote_map[RDRAM_SIZE - 1] && rdram[RDRAM_SIZE - 1] == M[15],
          "WR up to RDRAM's end noticed");
    CHECK(n64_rsp_read_reg(S, 1) == RDRAM_SIZE + 48, "WR past the end: address %06X", n64_rsp_read_reg(S, 1));
    free(wrote_map);
    wrote_map = NULL;
    new_rsp();
}

/* ---- 1d. DPC registers through COP0 8..15 ---- */
static void test_dpc(void)
{
    new_rsp();
    LOAD(R_ORI(1, 0, 0x1234), R_MTC0(1, 8), R_MFC0(2, 11), R_SW(2, 0, 0), R_BREAK);
    go();
    CHECK(r32(0) == 0, "no RDP: DPC reads 0 (%08X)", r32(0));
    memset(&fake_rdp, 0, sizeof fake_rdp);
    fake_rdp.regs[3] = 0x81;
    n64_rsp_attach_rdp(S, &fake_rdp);
    CHECK(fake_rdp.dmem == M, "attach gives the RDP the DMEM (XBUS)");
    LOAD(R_ORI(1, 0, 0x1000), R_MTC0(1, 8), R_ORI(1, 0, 0x1100), R_MTC0(1, 9), R_MFC0(2, 11), R_MFC0(3, 8),
         R_SW(2, 0, 0), R_SW(3, 4, 0), R_BREAK);
    go();
    CHECK(fake_rdp.regs[0] == 0x1000 && fake_rdp.regs[1] == 0x1100 && fake_rdp.writes == 2, "DPC START / END");
    CHECK(fake_rdp.runs == 1, "END write runs the RDP (%d)", fake_rdp.runs);
    CHECK(r32(0) == 0x81 && r32(4) == 0x1000, "DPC reads %08X %08X", r32(0), r32(4));
}

/* ---- 1e. Vector unit: computational ops ---- */
static void test_vu_ops(void)
{
    static const uint16_t A[8] = {0x4000, 0x8000, 0x8000, 0x7FFF, 0xFFFF, 0x0001, 0x1234, 0xC000};
    static const uint16_t B[8] = {0x4000, 0x8000, 0x7FFF, 0x7FFF, 0xFFFF, 0x0001, 0x0002, 0x4000};
    new_rsp();

    /* VMULF: (a * b * 2 + 0x8000), result = acc[47:16] clamped. */
    vrun(A, B, 0, 0, 0, NULL, 0, R_VOP(RV_VMULF, 3, 1, 2, 0));
    CHECKV(0x100, 0x2000, 0x7FFF, 0x8001, 0x7FFE, 0x0000, 0x0000, 0x0000, 0xE000);
    CHECKV(0x110, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF);
    CHECKV(0x120, 0x2000, 0x8000, 0x8001, 0x7FFE, 0x0000, 0x0000, 0x0000, 0xE000);
    CHECKV(0x130, 0x8000, 0x8000, 0x8000, 0x8002, 0x8002, 0x8002, 0xC8D0, 0x8000);
    /* VMULU: negative -> 0, above 0x7FFF -> 0xFFFF. */
    vrun(A, B, 0, 0, 0, NULL, 0, R_VOP(RV_VMULU, 3, 1, 2, 0));
    CHECKV(0x100, 0x2000, 0xFFFF, 0x0000, 0x7FFE, 0x0000, 0x0000, 0x0000, 0x0000);

    /* VMUDH then VMADH twice: 0x3FFF0001 << 16 accumulates to 0x7FFE0002 << 16 (clamped 0x7FFF), then wraps past
     * 48 bits to a negative value (clamped 0x8000). */
    {
        const uint32_t pre[] = {R_VOP(RV_VMUDH, 3, 1, 1, 0), R_VOP(RV_VMADH, 3, 1, 1, 0)};
        vrun(VEC(0x7FFF, 0x7FFF, 0, 0, 0, 0, 0, 0), VEC(0), 0, 0, 0, pre, 1, R_VOP(RV_VMADH, 3, 1, 1, 0));
        CHECK(r16(0x100) == 0x7FFF && r16(0x110) == 0x7FFE && r16(0x120) == 0x0002 && r16(0x130) == 0,
              "VMADH acc %04X %04X %04X -> %04X", r16(0x110), r16(0x120), r16(0x130), r16(0x100));
        vrun(VEC(0x7FFF, 0x7FFF, 0, 0, 0, 0, 0, 0), VEC(0), 0, 0, 0, pre, 2, R_VOP(RV_VMADH, 3, 1, 1, 0));
        CHECK(r16(0x100) == 0x8000 && r16(0x110) == 0xBFFD && r16(0x120) == 0x0003, "VMADH 48-bit wrap %04X %04X",
              r16(0x110), r16(0x100));
    }
    /* VMUDL / VMUDM / VMUDN: unsigned x unsigned >> 16, signed x unsigned, unsigned x signed. */
    vrun(VEC(0xFFFF, 0x8000, 0x0002), VEC(0xFFFF, 0x8000, 0x8000), 0, 0, 0, NULL, 0, R_VOP(RV_VMUDL, 3, 1, 2, 0));
    CHECKV(0x100, 0xFFFE, 0x4000, 0x0001, 0, 0, 0, 0, 0);
    CHECKV(0x120, 0, 0, 0, 0, 0, 0, 0, 0);
    vrun(VEC(0xFFFF, 0x8000, 0x0002), VEC(0xFFFF, 0x8000, 0x8000), 0, 0, 0, NULL, 0, R_VOP(RV_VMUDM, 3, 1, 2, 0));
    CHECKV(0x100, 0xFFFF, 0xC000, 0x0001, 0, 0, 0, 0, 0);
    CHECKV(0x110, 0xFFFF, 0xFFFF, 0x0000, 0, 0, 0, 0, 0);
    CHECKV(0x130, 0x0001, 0x0000, 0x0000, 0, 0, 0, 0, 0);
    vrun(VEC(0xFFFF, 0x8000, 0x0002), VEC(0xFFFF, 0x8000, 0x8000), 0, 0, 0, NULL, 0, R_VOP(RV_VMUDN, 3, 1, 2, 0));
    CHECKV(0x100, 0x0001, 0x0000, 0x0000, 0, 0, 0, 0, 0);
    CHECKV(0x120, 0xFFFF, 0xC000, 0xFFFF, 0, 0, 0, 0, 0);
    /* VMADN / VMADL: acc[15:0], or 0 / 0xFFFF when the accumulator leaves the signed 32-bit range. */
    {
        const uint32_t pre[] = {R_VOP(RV_VMUDH, 3, 1, 2, 0)};
        vrun(VEC(0x7FFF, 0x8000, 0x0001, 0xFFFF), VEC(0x7FFF, 0x7FFF, 0x0001, 0x0001), 0, 0, 0, pre, 1,
             R_VOP(RV_VMADN, 3, 0, 0, 0));
        CHECKV(0x100, 0xFFFF, 0x0000, 0x0000, 0x0000, 0, 0, 0, 0);
        vrun(VEC(0x7FFF, 0x8000, 0x0001, 0xFFFF), VEC(0x7FFF, 0x7FFF, 0x0001, 0x0001), 0, 0, 0, pre, 1,
             R_VOP(RV_VMADL, 3, 1, 2, 0));
        /* lane 2: acc 0x1_0000 + (1 * 1 >> 16 = 0) -> fits: 0x0000; lane 3: -0x1_0000 + (0xFFFF * 1 >> 16 = 0). */
        CHECKV(0x100, 0xFFFF, 0x0000, 0x0000, 0x0000, 0, 0, 0, 0);
        CHECKV(0x120, 0x0001, 0x8000, 0x0001, 0xFFFF, 0, 0, 0, 0);
    }
    /* VMACF accumulates a * b * 2 (no rounding). */
    {
        const uint32_t pre[] = {R_VOP(RV_VMULF, 3, 1, 2, 0)};
        vrun(VEC(0x4000, 0x0001), VEC(0x4000, 0x0001), 0, 0, 0, pre, 1, R_VOP(RV_VMACF, 3, 1, 2, 0));
        CHECKV(0x100, 0x4000, 0x0000, 0, 0, 0, 0, 0, 0);
        CHECKV(0x130, 0x8000, 0x8004, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000);
    }

    /* VADD: a + b + carry, clamped; ACCL takes the unclamped sum; VCO cleared. */
    vrun(VEC(1, 0x7FFF, 0x8000, 0xFFFF, 5, 0x8000, 0x7FFF, 0), VEC(1, 1, 0xFFFF, 0xFFFF, 0xFFFB, 0x8000, 0x7FFF, 0),
         0x00FF, 0, 0, NULL, 0, R_VOP(RV_VADD, 3, 1, 2, 0));
    CHECKV(0x100, 3, 0x7FFF, 0x8000, 0xFFFF, 1, 0x8000, 0x7FFF, 1);
    CHECKV(0x130, 3, 0x8001, 0x8000, 0xFFFF, 1, 0x0001, 0xFFFF, 1);
    CHECK(r32(0x140) == 0, "VADD clears VCO");
    /* VSUB: a - b - carry. */
    vrun(VEC(0x8000, 0x7FFF, 0, 5), VEC(1, 0xFFFF, 0, 3), 0x0005, 0, 0, NULL, 0, R_VOP(RV_VSUB, 3, 1, 2, 0));
    CHECKV(0x100, 0x8000, 0x7FFF, 0xFFFF, 2, 0, 0, 0, 0);
    CHECKV(0x130, 0x7FFE, 0x8000, 0xFFFF, 2, 0, 0, 0, 0); /* lane 0: -32768 - 1 - carry */
    /* VABS: sign of a applied to b; -(-32768) gives 0x7FFF with ACCL 0x8000. */
    vrun(VEC(0xFFFF, 0, 5, 0xFFFD, 0xFFFF), VEC(0x8000, 7, 7, 0xFFFC, 9), 0, 0, 0, NULL, 0,
         R_VOP(RV_VABS, 3, 1, 2, 0));
    CHECKV(0x100, 0x7FFF, 0, 7, 4, 0xFFF7, 0, 0, 0);
    CHECKV(0x130, 0x8000, 0, 7, 4, 0xFFF7, 0, 0, 0);
    /* VADDC / VSUBC: unsigned, carry / borrow in VCO low, not-equal (VSUBC) in VCO high. */
    vrun(VEC(0xFFFF, 0x8000, 0, 0, 0, 0, 0, 0xFFFF), VEC(1, 0x7FFF, 0, 0, 0, 0, 0, 0xFFFF), 0xFFFF, 0, 0, NULL, 0,
         R_VOP(RV_VADDC, 3, 1, 2, 0));
    CHECKV(0x100, 0, 0xFFFF, 0, 0, 0, 0, 0, 0xFFFE);
    CHECK(r32(0x140) == 0x0081, "VADDC VCO %08X", r32(0x140));
    vrun(VEC(0, 5, 7), VEC(1, 5, 3), 0, 0, 0, NULL, 0, R_VOP(RV_VSUBC, 3, 1, 2, 0));
    CHECKV(0x100, 0xFFFF, 0, 4, 0, 0, 0, 0, 0);
    CHECK(r32(0x140) == 0x0501, "VSUBC VCO %08X", r32(0x140));

    /* Selects: VCC low = condition, result = a when set else b; VCO cleared, VCC high cleared. Lane 3 has
     * VCO carry + not-equal set: VLT takes equal as less, VGE takes it as not greater-or-equal. */
    {
        const uint16_t a[8] = {1, 2, 3, 3, 0x8000, 4, 4, 4}, b[8] = {2, 2, 2, 3, 0x7FFF, 4, 4, 4};
        vrun(a, b, 0x6868, 0xFF00, 0, NULL, 0, R_VOP(RV_VLT, 3, 1, 2, 0));
        CHECKV(0x100, 1, 2, 2, 3, 0x8000, 4, 4, 4);
        CHECK(r32(0x144) == 0x0079 && r32(0x140) == 0, "VLT VCC %04X VCO %04X", r32(0x144), r32(0x140));
        vrun(a, b, 0x6868, 0, 0, NULL, 0, R_VOP(RV_VEQ, 3, 1, 2, 0));
        CHECK(r32(0x144) == 0x0082, "VEQ VCC %04X", r32(0x144));
        vrun(a, b, 0x6868, 0, 0, NULL, 0, R_VOP(RV_VNE, 3, 1, 2, 0));
        CHECK(r32(0x144) == 0x007D, "VNE VCC %04X", r32(0x144));
        vrun(a, b, 0x6868, 0, 0, NULL, 0, R_VOP(RV_VGE, 3, 1, 2, 0));
        CHECKV(0x100, 2, 2, 3, 3, 0x7FFF, 4, 4, 4);
        CHECK(r32(0x144) == 0x0086, "VGE VCC %04X", r32(0x144));
        /* VMRG: VCC low picks a. */
        vrun(a, b, 0xFFFF, 0x00AA, 0, NULL, 0, R_VOP(RV_VMRG, 3, 1, 2, 0));
        CHECKV(0x100, 2, 2, 2, 3, 0x7FFF, 4, 4, 4);
        CHECK(r32(0x140) == 0 && r32(0x144) == 0x00AA, "VMRG flags %04X %04X", r32(0x140), r32(0x144));
    }
    /* VCH: same signs (5, 3): result b, VCC high (a - b >= 0), not-equal; different signs (-5, 3): -b, VCC low,
     * VCO carry, not-equal; (-4, 3): sum -1 -> VCE, and a == ~b -> not-equal clear; (1, 3): a, not-equal;
     * (0, 0): b, VCC high. CFC2 sign-extends VCC. */
    vrun(VEC(5, 0xFFFB, 0xFFFC, 1), VEC(3, 3, 3, 3), 0, 0, 0, NULL, 0, R_VOP(RV_VCH, 3, 1, 2, 0));
    CHECKV(0x100, 3, 0xFFFD, 0xFFFD, 1, 0, 0, 0, 0);
    CHECK(r32(0x140) == 0x0B06 && r32(0x144) == 0xFFFFF106u && r32(0x148) == 0x04, "VCH VCO %04X VCC %04X VCE %02X",
          r32(0x140), r32(0x144), r32(0x148));
    /* VCL after VCH (the double-precision clip pair): lanes with VCO carry clear compare unsigned a >= b into VCC
     * high; with carry set and not-equal clear, VCC low from the sum (VCE picks the rule). */
    {
        vrun(VEC(5, 0xFFFB, 0xFFFC, 1), VEC(3, 3, 3, 3), 0, 0, 0, NULL, 0, R_VOP(RV_VCL, 3, 1, 2, 0));
        /* Flags as loaded (0): every lane takes the "carry clear, not-equal clear" path: VCC high = a >= b. */
        CHECKV(0x100, 3, 3, 3, 1, 0, 0, 0, 0);
        CHECK(r32(0x144) == 0xFFFFF700u && r32(0x140) == 0 && r32(0x148) == 0, "VCL VCC %04X", r32(0x144));
        /* carry set, not-equal clear, VCE clear: VCC low = (sum == 0 && no carry): 0 + 0 -> -b = 0. */
        vrun(VEC(0, 0xFFFF, 0x8000), VEC(0, 1, 0x8000), 0x00FF, 0, 0, NULL, 0, R_VOP(RV_VCL, 3, 1, 2, 0));
        CHECKV(0x100, 0, 0xFFFF, 0x8000, 0, 0, 0, 0, 0);
        CHECK(r32(0x144) == 0x00F9, "VCL low %04X", r32(0x144));
        /* the same with VCE set: VCC low = (sum == 0 || no carry): every lane, so -b everywhere. */
        vrun(VEC(0, 0xFFFF, 0x8000, 2), VEC(0, 1, 0x8000, 1), 0x00FF, 0, 0xFF, NULL, 0, R_VOP(RV_VCL, 3, 1, 2, 0));
        CHECKV(0x100, 0, 0xFFFF, 0x8000, 0xFFFF, 0, 0, 0, 0);
        CHECK(r32(0x144) == 0x00FF, "VCL low VCE %04X", r32(0x144));
    }
    /* VCR: one's complement clip. (5, 3) -> 3, VCC high; (-5, 3) -> ~3, VCC low; (2, -3): a + b + 1 = 0 <= 0 -> ~b. */
    vrun(VEC(5, 0xFFFB, 2, 1), VEC(3, 3, 0xFFFD, 3), 0xFFFF, 0, 0xFF, NULL, 0, R_VOP(RV_VCR, 3, 1, 2, 0));
    CHECKV(0x100, 3, 0xFFFC, 0x0002, 1, 0, 0, 0, 0);
    CHECK(r32(0x144) == 0xFFFFF506u && r32(0x140) == 0 && r32(0x148) == 0, "VCR VCC %04X", r32(0x144));

    /* Logic ops. */
    {
        static const uint16_t a[8] = {0xF0F0, 0x1234, 0, 0xFFFF}, b[8] = {0xFF00, 0x00FF, 0, 0x0F0F};
        vrun(a, b, 0, 0, 0, NULL, 0, R_VOP(RV_VAND, 3, 1, 2, 0));
        CHECKV(0x100, 0xF000, 0x0034, 0, 0x0F0F, 0, 0, 0, 0);
        vrun(a, b, 0, 0, 0, NULL, 0, R_VOP(RV_VNAND, 3, 1, 2, 0));
        CHECKV(0x100, 0x0FFF, 0xFFCB, 0xFFFF, 0xF0F0, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF);
        vrun(a, b, 0, 0, 0, NULL, 0, R_VOP(RV_VOR, 3, 1, 2, 0));
        CHECKV(0x100, 0xFFF0, 0x12FF, 0, 0xFFFF, 0, 0, 0, 0);
        vrun(a, b, 0, 0, 0, NULL, 0, R_VOP(RV_VNOR, 3, 1, 2, 0));
        CHECKV(0x100, 0x000F, 0xED00, 0xFFFF, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF);
        vrun(a, b, 0, 0, 0, NULL, 0, R_VOP(RV_VXOR, 3, 1, 2, 0));
        CHECKV(0x100, 0x0FF0, 0x12CB, 0, 0xF0F0, 0, 0, 0, 0);
        vrun(a, b, 0, 0, 0, NULL, 0, R_VOP(RV_VNXOR, 3, 1, 2, 0));
        CHECKV(0x100, 0xF00F, 0xED34, 0xFFFF, 0x0F0F, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF);
        CHECKV(0x130, 0xF00F, 0xED34, 0xFFFF, 0x0F0F, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF);
    }

    /* Element selectors: VOR with v0 = 0 gives the selected b. */
    {
        static const uint8_t want[16][8] = {
            {0, 1, 2, 3, 4, 5, 6, 7}, {0, 1, 2, 3, 4, 5, 6, 7}, {0, 0, 2, 2, 4, 4, 6, 6}, {1, 1, 3, 3, 5, 5, 7, 7},
            {0, 0, 0, 0, 4, 4, 4, 4}, {1, 1, 1, 1, 5, 5, 5, 5}, {2, 2, 2, 2, 6, 6, 6, 6}, {3, 3, 3, 3, 7, 7, 7, 7},
            {0, 0, 0, 0, 0, 0, 0, 0}, {1, 1, 1, 1, 1, 1, 1, 1}, {2, 2, 2, 2, 2, 2, 2, 2}, {3, 3, 3, 3, 3, 3, 3, 3},
            {4, 4, 4, 4, 4, 4, 4, 4}, {5, 5, 5, 5, 5, 5, 5, 5}, {6, 6, 6, 6, 6, 6, 6, 6}, {7, 7, 7, 7, 7, 7, 7, 7},
        };
        for (unsigned e = 0; e < 16; e++) {
            vrun(VEC(0), VEC(0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17), 0, 0, 0, NULL, 0,
                 R_VOP(RV_VOR, 3, 1, 2, e));
            for (int i = 0; i < 8; i++)
                CHECK(r16(0x100 + 2 * i) == 0x10 + want[e][i], "element %u lane %d: %04X", e, i, r16(0x100 + 2 * i));
        }
    }

    /* VSAR reads without writing the accumulator; other selectors read 0. */
    {
        const uint32_t pre[] = {R_VOP(RV_VMULF, 6, 1, 2, 0), R_VOP(RV_VSAR, 7, 0, 0, 9)};
        vrun(A, B, 0, 0, 0, pre, 2, R_VOP(RV_VSAR, 3, 0, 0, 3));
        CHECKV(0x100, 0, 0, 0, 0, 0, 0, 0, 0);
        CHECKV(0x120, 0x2000, 0x8000, 0x8001, 0x7FFE, 0x0000, 0x0000, 0x0000, 0xE000);
    }
    /* Reserved opcodes: result 0, ACCL = a + b. VNOP changes nothing. */
    vrun(VEC(1, 0xFFFF), VEC(2, 2), 0, 0, 0, NULL, 0, R_VOP(0x19, 3, 1, 2, 0));
    CHECKV(0x100, 0, 0, 0, 0, 0, 0, 0, 0);
    CHECK(r16(0x130) == 3 && r16(0x132) == 1, "reserved ACCL %04X %04X", r16(0x130), r16(0x132));
}

/* ---- 1f. Divide unit, VMOV ---- */
static void test_vu_divide(void)
{
    new_rsp();
    /* v1 = inputs; v3[i] = VRCP / VRSQ of v1[i]; then VRCPH v4[0] reads DIVOUT. */
    struct {
        uint32_t fn;
        uint16_t in, lane, divout;
    } cases[] = {
        {RV_VRCP, 1, 0xC000, 0x7FFF},      /* 1 / 1 = 0x7FFFC000 (ROM entry 0 = 0xFFFF) */
        {RV_VRCP, 0, 0xFFFF, 0x7FFF},      /* 1 / 0 = 0x7FFFFFFF */
        {RV_VRCP, 0x8000, 0x0000, 0xFFFF}, /* -32768 -> 0xFFFF0000 */
        {RV_VRCP, 2, 0xE000, 0x3FFF},      /* 0x3FFFE000 */
        {RV_VRCP, 3, 0xA000, 0x2AAA},      /* ROM[256] = 0x5555 -> 0x2AAAA000 */
        {RV_VRCP, 0xFFFF, 0x3FFF, 0x8000}, /* -1: one's complement of 1 / 1 */
        {RV_VRCP, 0xFFFD, 0x5FFF, 0xD555}, /* -3: ~0x2AAAA000 */
        {RV_VRSQ, 1, 0xC000, 0x7FFF},      /* 1 / sqrt(1) */
        {RV_VRSQ, 4, 0xE000, 0x3FFF},      /* 1 / sqrt(4) = 0x3FFFE000 */
        {RV_VRSQ, 2, 0x4000, 0x5A82},      /* 1 / sqrt(2): ROM[256] = 0x6A09 -> 0x5A824000 */
        {RV_VRSQ, 0, 0xFFFF, 0x7FFF},
        {RV_VRSQ, 0x8000, 0x0000, 0xFFFF},
    };
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        LOAD(R_LQV(1, 0, 0, 0), R_LQV(3, 0, 1, 0), R_VOP(cases[k].fn, 3, 5, 1, 8 + 2), R_VOP(RV_VRCPH, 4, 0, 0, 8),
             R_SQV(3, 0, 16, 0), R_SQV(4, 0, 17, 0), R_BREAK);
        wvec(0x00, VEC(0, 0, cases[k].in, 0, 0, 0, 0, 0));
        wvec(0x10, VEC(0x1111, 0x1111, 0x1111, 0x1111, 0x1111, 0x1111, 0x1111, 0x1111));
        go();
        CHECK(r16(0x10A) == cases[k].lane && r16(0x110) == cases[k].divout,
              "%s(%04X): lane %04X divout %04X (want %04X %04X)", cases[k].fn == RV_VRCP ? "VRCP" : "VRSQ",
              cases[k].in, r16(0x10A), r16(0x110), cases[k].lane, cases[k].divout);
        CHECK(r16(0x100) == 0x1111 && r16(0x108) == 0x1111, "only lane de written");
    }
    /* Double precision: VRCPH sets DIVIN (high half) and returns the previous DIVOUT; VRCPL divides
     * (DIVIN << 16 | low): 0x00010000 -> 0x00007FFF (1 / 65536 = 2^15). Then VRCPL again without VRCPH is 16-bit. */
    LOAD(R_LQV(1, 0, 0, 0), R_VOP(RV_VXOR, 3, 3, 3, 0), R_VOP(RV_VRCP, 3, 0, 1, 8 + 7), R_VOP(RV_VRCPH, 3, 1, 1, 8 + 1),
         R_VOP(RV_VRCPL, 3, 2, 1, 8 + 0), R_VOP(RV_VRCPH, 3, 3, 1, 8 + 0), R_VOP(RV_VRCPL, 3, 4, 1, 8 + 1),
         R_VOP(RV_VRCPH, 3, 5, 0, 8), R_SQV(3, 0, 16, 0), R_BREAK);
    wvec(0x00, VEC(0x0000, 0x0001, 0, 0, 0, 0, 0, 2));
    go();
    /* lane0 = rcp(2) low = 0xE000; lane1 = DIVOUT of it = 0x3FFF; lane2 = rcp(0x00010000) low = 0x7FFF (result
     * 0x00007FFF); lane3 = its DIVOUT 0; lane4 = 16-bit rcp(1) = 0xC000 (DIVDP was cleared); lane5 = 0x7FFF. */
    CHECKV(0x100, 0xE000, 0x3FFF, 0x7FFF, 0x0000, 0xC000, 0x7FFF, 0, 0);

    /* VMOV: vd[de] = (b with the selector)[de]; ACCL = the selected b. */
    LOAD(R_LQV(1, 0, 0, 0), R_LQV(3, 0, 1, 0), R_VOP(RV_VMOV, 3, 6, 1, 8 + 2), R_VOP(RV_VMOV, 3, 1, 1, 0),
         R_SQV(3, 0, 16, 0), R_VOP(RV_VSAR, 4, 0, 0, 10), R_SQV(4, 0, 17, 0), R_BREAK);
    wvec(0x00, VEC(10, 11, 12, 13, 14, 15, 16, 17));
    wvec(0x10, VEC(0, 0, 0, 0, 0, 0, 0, 0));
    go();
    CHECKV(0x100, 0, 11, 0, 0, 0, 0, 12, 0);
    CHECKV(0x110, 10, 11, 12, 13, 14, 15, 16, 17);
}

/* ---- 1g. COP2 moves ---- */
static void test_cop2_moves(void)
{
    new_rsp();
    LOAD(R_LQV(1, 0, 0, 0), R_MFC2(2, 1, 0), R_MFC2(3, 1, 1), R_MFC2(4, 1, 15), R_ORI(5, 0, 0xBEEF),
         R_MTC2(5, 1, 15), R_MTC2(5, 1, 7), R_SQV(1, 0, 16, 0), R_LUI(6, 0x1234), R_ORI(6, 6, 0x8001), R_CTC2(6, 0),
         R_CFC2(7, 0), R_CTC2(6, 1), R_CFC2(8, 1), R_CTC2(6, 2), R_CFC2(9, 2), R_ORI(6, 0, 0x7F80), R_CTC2(6, 0),
         R_CFC2(10, 0), R_SW(2, 0x200, 0), R_SW(3, 0x204, 0), R_SW(4, 0x208, 0), R_SW(7, 0x20C, 0),
         R_SW(8, 0x210, 0), R_SW(9, 0x214, 0), R_SW(10, 0x218, 0), R_BREAK);
    wvec(0x00, VEC(0x8123, 0x4567, 0x89AB, 0xCDEF, 0x0011, 0x2233, 0x4455, 0x66F7));
    go();
    CHECK(r32(0x200) == 0xFFFF8123u, "MFC2 e0 %08X", r32(0x200));
    CHECK(r32(0x204) == 0x00002345u, "MFC2 e1 (odd byte) %08X", r32(0x204));
    CHECK(r32(0x208) == 0xFFFFF781u, "MFC2 e15 wraps to byte 0 %08X", r32(0x208));
    /* MTC2 e15 writes byte 15 only; e7 writes bytes 7 and 8 (across two elements). */
    CHECKV(0x100, 0x8123, 0x4567, 0x89AB, 0xCDBE, 0xEF11, 0x2233, 0x4455, 0x66BE);
    CHECK(r32(0x20C) == 0xFFFF8001u && r32(0x210) == 0xFFFF8001u, "CFC2 VCO / VCC sign-extended %08X", r32(0x20C));
    CHECK(r32(0x214) == 0x01, "CFC2 VCE 8 bits %08X", r32(0x214));
    CHECK(r32(0x218) == 0x7F80, "CFC2 VCO positive %08X", r32(0x218));
}

/* ---- 1h. Vector loads / stores ---- */
static void fill_pattern(void)
{
    for (int i = 0; i < 0x1000; i++)
        M[i] = (uint8_t)i;
}

/* Runs: v1 <- 0x800 (preset 0xEEEE lanes), then `op`, then v1 -> 0x900. Returns lane i. */
static void ls_run(uint32_t op, uint32_t base)
{
    LOAD(R_ORI(2, 0, 0x800), R_LQV(1, 0, 0, 2), R_ORI(1, 0, base), op, R_ORI(3, 0, 0x900), R_SQV(1, 0, 0, 3),
         R_BREAK);
    fill_pattern();
    for (int i = 0; i < 8; i++)
        w16(0x800 + 2 * i, 0xEEEE);
    go();
}

static void test_loads_stores(void)
{
    new_rsp();
    ls_run(R_LWC2(RL_BV, 1, 3, 2, 1), 0x10); /* LBV byte 3 <- [0x12] */
    CHECKV(0x900, 0xEEEE, 0xEE12, 0xEEEE, 0xEEEE, 0xEEEE, 0xEEEE, 0xEEEE, 0xEEEE);
    ls_run(R_LWC2(RL_SV, 1, 15, 1, 1), 0x20); /* LSV at byte 15: only one byte fits */
    CHECKV(0x900, 0xEEEE, 0xEEEE, 0xEEEE, 0xEEEE, 0xEEEE, 0xEEEE, 0xEEEE, 0xEE22);
    ls_run(R_LWC2(RL_DV, 1, 12, 0, 1), 0xFFC); /* LDV byte 12: 4 bytes, from 0xFFC (wraps at 4 KB) */
    CHECKV(0x900, 0xEEEE, 0xEEEE, 0xEEEE, 0xEEEE, 0xEEEE, 0xEEEE, 0xFCFD, 0xFEFF);
    ls_run(R_LWC2(RL_LV, 1, 2, 0, 1), 0xFFE); /* LLV byte 2 from 0xFFE: FE FF 00 01 */
    CHECKV(0x900, 0xEEEE, 0xFEFF, 0x0001, 0xEEEE, 0xEEEE, 0xEEEE, 0xEEEE, 0xEEEE);
    ls_run(R_LQV(1, 0, 0, 1), 0x108); /* LQV unaligned: bytes 0..7 up to the 16-byte boundary */
    CHECKV(0x900, 0x0809, 0x0A0B, 0x0C0D, 0x0E0F, 0xEEEE, 0xEEEE, 0xEEEE, 0xEEEE);
    ls_run(R_LQV(1, 4, 0, 1), 0x10C); /* LQV e4 at 0x10C: bytes 4..7 */
    CHECKV(0x900, 0xEEEE, 0xEEEE, 0x0C0D, 0x0E0F, 0xEEEE, 0xEEEE, 0xEEEE, 0xEEEE);
    ls_run(R_LWC2(RL_RV, 1, 0, 1, 1), 0x8); /* LRV at 0x18: bytes 8..15 <- 0x10..0x17 */
    CHECKV(0x900, 0xEEEE, 0xEEEE, 0xEEEE, 0xEEEE, 0x1011, 0x1213, 0x1415, 0x1617);
    ls_run(R_LWC2(RL_RV, 1, 0, 0, 1), 0x20); /* LRV aligned: nothing */
    CHECKV(0x900, 0xEEEE, 0xEEEE, 0xEEEE, 0xEEEE, 0xEEEE, 0xEEEE, 0xEEEE, 0xEEEE);
    ls_run(R_LWC2(RL_PV, 1, 0, 0, 1), 0x31); /* LPV: bytes [0x31..0x38] << 8 */
    CHECKV(0x900, 0x3100, 0x3200, 0x3300, 0x3400, 0x3500, 0x3600, 0x3700, 0x3800);
    ls_run(R_LWC2(RL_UV, 1, 0, 0, 1), 0x3C); /* LUV: << 7, the 16-byte window wraps: 3C..3F 30.. no: 3C..43 */
    CHECKV(0x900, 0x1E00, 0x1E80, 0x1F00, 0x1F80, 0x2000, 0x2080, 0x2100, 0x2180);
    ls_run(R_LWC2(RL_PV, 1, 1, 0, 1), 0x40); /* LPV e1: index -1 -> lane i <- [0x40 + ((i - 1) & 15)] */
    CHECKV(0x900, 0x4F00, 0x4000, 0x4100, 0x4200, 0x4300, 0x4400, 0x4500, 0x4600);
    ls_run(R_LWC2(RL_HV, 1, 0, 0, 1), 0x50); /* LHV: every other byte << 7 */
    CHECKV(0x900, 0x2800, 0x2900, 0x2A00, 0x2B00, 0x2C00, 0x2D00, 0x2E00, 0x2F00);
    ls_run(R_LWC2(RL_FV, 1, 0, 0, 1), 0x60); /* LFV e0: elements 0..3 <- [0x60, 0x64, 0x68, 0x6C] << 7 */
    CHECKV(0x900, 0x3000, 0x3200, 0x3400, 0x3600, 0xEEEE, 0xEEEE, 0xEEEE, 0xEEEE);
    /* LFV e8: the window index starts at -8, so elements 4..7 <- [0x60, 0x64, 0x68, 0x6C] << 7 */
    ls_run(R_LWC2(RL_FV, 1, 8, 0, 1), 0x60);
    CHECKV(0x900, 0xEEEE, 0xEEEE, 0xEEEE, 0xEEEE, 0x3000, 0x3200, 0x3400, 0x3600);

    /* Stores, into a pattern-filled DMEM. v1 = 0x0102 .. 0x0F10. */
    {
        static const uint16_t v[8] = {0x0102, 0x0304, 0x0506, 0x0708, 0x090A, 0x0B0C, 0x0D0E, 0x8F10};
#define ST(op, base)                                                                                                   \
    do {                                                                                                               \
        LOAD(R_ORI(2, 0, 0x800), R_LQV(1, 0, 0, 2), R_ORI(1, 0, base), op, R_BREAK);                                   \
        fill_pattern();                                                                                                \
        wvec(0x800, v);                                                                                                \
        go();                                                                                                          \
    } while (0)
        ST(R_SWC2(RL_BV, 1, 5, 0, 1), 0x100);
        CHECK(M[0x100] == 0x06 && M[0x101] == 0x01, "SBV");
        ST(R_SWC2(RL_DV, 1, 12, 0, 1), 0xFFC); /* SDV e12: bytes 12..15 then 0..3 (register wraps), DMEM wraps */
        CHECK(M[0xFFC] == 0x0D && M[0xFFF] == 0x10 && M[0x000] == 0x01 && M[0x003] == 0x04 && M[0x004] == 0x04,
              "SDV wraps %02X %02X %02X %02X", M[0xFFC], M[0xFFF], M[0], M[3]);
        ST(R_SQV(1, 0, 0, 1), 0x10A); /* SQV unaligned: up to the boundary */
        CHECK(M[0x10A] == 0x01 && M[0x10F] == 0x06 && M[0x110] == 0x10, "SQV unaligned");
        ST(R_SQV(1, 3, 0, 1), 0x120); /* SQV e3: register bytes 3..15, 0..2 */
        CHECK(M[0x120] == 0x04 && M[0x12C] == 0x10 && M[0x12D] == 0x01 && M[0x12F] == 0x03, "SQV e3");
        ST(R_SWC2(RL_RV, 1, 0, 0, 1), 0x13A); /* SRV at 0x13A: bytes 6..15 -> 0x130..0x139 */
        CHECK(M[0x130] == 0x07 && M[0x139] == 0x10 && M[0x13A] == 0x3A && M[0x12F] == 0x2F, "SRV");
        ST(R_SWC2(RL_PV, 1, 0, 0, 1), 0x140); /* SPV: high bytes */
        CHECK(M[0x140] == 0x01 && M[0x147] == 0x8F && M[0x148] == 0x48, "SPV");
        ST(R_SWC2(RL_UV, 1, 0, 0, 1), 0x150); /* SUV: lane >> 7 */
        CHECK(M[0x150] == 0x02 && M[0x151] == 0x06 && M[0x157] == 0x1E, "SUV %02X %02X %02X", M[0x150], M[0x151],
              M[0x157]);
        ST(R_SWC2(RL_PV, 1, 8, 0, 1), 0x160); /* SPV e8: lanes 0..7 as >> 7 (the byte index has bit 3 set) */
        CHECK(M[0x160] == 0x02 && M[0x167] == 0x1E, "SPV e8 %02X", M[0x160]);
        ST(R_SWC2(RL_HV, 1, 0, 0, 1), 0x170); /* SHV: lane >> 7 to every other byte */
        CHECK(M[0x170] == 0x02 && M[0x171] == 0x71 && M[0x172] == 0x06 && M[0x17E] == 0x1E, "SHV");
        ST(R_SWC2(RL_FV, 1, 0, 0, 1), 0x180); /* SFV e0: lanes 0..3 >> 7 at +0, +4, +8, +12 */
        CHECK(M[0x180] == 0x02 && M[0x184] == 0x06 && M[0x188] == 0x0A && M[0x18C] == 0x0E && M[0x181] == 0x81,
              "SFV");
        ST(R_SWC2(RL_FV, 1, 2, 0, 1), 0x190); /* SFV e2: zeros */
        CHECK(M[0x190] == 0 && M[0x194] == 0 && M[0x191] == 0x91, "SFV zero");
        ST(R_SWC2(RL_WV, 1, 1, 0, 1), 0x1A0); /* SWV e1: bytes 1..15, 0 */
        CHECK(M[0x1A0] == 0x02 && M[0x1AE] == 0x10 && M[0x1AF] == 0x01, "SWV");
#undef ST
    }
    /* LTV / STV: e0 loads element i of v(8 + i) from halfword i; STV writes it back (a transpose round trip). */
    LOAD(R_ORI(1, 0, 0x200), R_LWC2(RL_TV, 8, 0, 0, 1), R_ORI(2, 0, 0x300), R_SWC2(RL_TV, 8, 0, 0, 2),
         R_SWC2(RL_TV, 8, 2, 1, 2), R_SQV(9, 0, 0, 0), R_BREAK);
    fill_pattern();
    go();
    for (int i = 0; i < 16; i++)
        CHECK(M[0x300 + i] == (uint8_t)(i), "STV round trip byte %d: %02X", i, M[0x300 + i]);
    CHECK(r16(0x002) == 0x0203, "LTV v9[1] %04X", r16(0x002));
    /* STV e2 at 0x310: register v(8 + k) element (k + 1) & 7 to halfword k, starting at byte -2: the first
     * halfword written is v8[1] at 0x31E. v8 holds only element 0 (0x0001)... v9[2] = 0 etc. */
    CHECK(M[0x31E] == 0 && M[0x31F] == 0, "STV e2");
}

/* ---- 2. Differential groups ---- */
static void io_wmem(void *c, uint32_t a, uint32_t w) { (void)c; n64_wbe32(M + (a & 0x1FFC), w); }
static uint32_t io_rmem(void *c, uint32_t a) { (void)c; return n64_be32(M + (a & 0x1FFC)); }
static void io_wreg(void *c, uint32_t r, uint32_t v) { (void)c; n64_rsp_write_reg(S, r, v); }
static uint32_t io_rreg(void *c, uint32_t r) { (void)c; return n64_rsp_read_reg(S, r); }
#define ORACLE_DRAM 0x00200000u
static void io_wdram(void *c, uint32_t o, uint32_t w) { (void)c; n64_wbe32(rdram + ORACLE_DRAM + o, w); }
static uint32_t io_rdram(void *c, uint32_t o) { (void)c; return n64_be32(rdram + ORACLE_DRAM + o); }
static void io_run(void *c)
{
    (void)c;
    n64_rsp_write_reg(S, 8, 0);
    n64_rsp_write_reg(S, 4, 0x5);
    n64_rsp_run(S, 1000000);
}

static void test_oracle(int print)
{
    static const char *names[] = {"VMULF", "VMULU", "VRNDP", "VMULQ", "VMUDL", "VMUDM", "VMUDN", "VMUDH",
                                  "VMACF", "VMACU", "VRNDN", "VMACQ", "VMADL", "VMADM", "VMADN", "VMADH",
                                  "VADD", "VSUB", "V12", "VABS", "VADDC", "VSUBC", "V16", "V17",
                                  "V18", "V19", "V1A", "V1B", "V1C", "VSAR", "V1E", "V1F",
                                  "VLT", "VEQ", "VNE", "VGE", "VCL", "VCH", "VCR", "VMRG",
                                  "VAND", "VNAND", "VOR", "VNOR", "VXOR", "VNXOR", "V2E", "V2F",
                                  "VRCP", "VRCPL", "VRCPH", "VMOV", "VRSQ", "VRSQL", "VRSQH", "VNOP",
                                  "V38", "V39", "V3A", "V3B", "V3C", "V3D", "V3E", "VNULL",
                                  "divide", "LBV", "LSV", "LLV", "LDV", "LQV", "LRV", "LPV",
                                  "LUV", "LHV", "LFV", "LWV", "LTV", "SBV", "SSV", "SLV",
                                  "SDV", "SQV", "SRV", "SPV", "SUV", "SHV", "SFV", "SWV",
                                  "STV", "DMA", "status", "scalar", "ROMs"};
    new_rsp();
    rspo_io io = {NULL, io_wmem, io_rmem, io_wreg, io_rreg, io_wdram, io_rdram, io_run, ORACLE_DRAM};
    int bad = 0;
    for (unsigned g = 0; g < RSPO_GROUPS; g++) {
        uint32_t h = rspo_group(&io, g);
        if (print)
            printf("    0x%08Xu, /* %2u %s */\n", h, g, names[g]);
        if (h != rspo_expected[g]) {
            printf("FAIL oracle group %u (%s): %08X, ares %08X\n", g, names[g], h, rspo_expected[g]);
            bad++;
        }
    }
    failures += bad;
}

/* IMEM rewritten after its code ran (n64_rsp.c keeps a predecoded entry per IMEM word): by the CPU between runs, by
 * an SP DMA the microcode starts itself and then jumps into, by a save state load, and by a vector op whose selector
 * class changes (whole vector vs a shuffle) at the same slot. The new words must run, never the old decode. */
static void test_imem_rewrite(void)
{
    new_rsp();
    LOAD(R_ORI(1, 0, 0x11), R_LQV(1, 0, 0, 0), R_BREAK);
    go();
    CHECK(n64_rsp_get_gpr(S, 1) == 0x11, "first program");
    LOAD(R_ORI(1, 0, 0x22), R_ORI(2, 0, 0x33), R_BREAK); /* the CPU's writes (n64_rsp_mem) */
    go();
    CHECK(n64_rsp_get_gpr(S, 1) == 0x22 && n64_rsp_get_gpr(S, 2) == 0x33, "rewritten by the CPU: %X %X",
          n64_rsp_get_gpr(S, 1), n64_rsp_get_gpr(S, 2));

    /* IMEM 0x100 runs once, then the microcode DMAs other code over it from RDRAM and jumps there. */
    new_rsp();
    n64_wbe32(M + 0x1100, R_ORI(3, 0, 0x44));
    n64_wbe32(M + 0x1104, R_BREAK);
    n64_rsp_write_reg(S, 8, 0x100);
    n64_rsp_write_reg(S, 4, 0x5);
    n64_rsp_run(S, 100);
    CHECK(n64_rsp_halted(S) && n64_rsp_get_gpr(S, 3) == 0x44, "IMEM 0x100 before the DMA");
    n64_wbe32(rdram + 0x3000, R_ORI(3, 0, 0x55));
    n64_wbe32(rdram + 0x3004, R_BREAK);
    LOAD(R_ORI(1, 0, 0x1100), R_MTC0(1, 0), R_ORI(1, 0, 0x3000), R_MTC0(1, 1), R_MTC0(0, 2), /* 8 bytes */
         R_J(0x100), R_NOP);
    go();
    CHECK(n64_rsp_get_gpr(S, 3) == 0x55, "rewritten by the RSP's own DMA: %X", n64_rsp_get_gpr(S, 3));

    /* A save state's IMEM replaces what ran since it was taken. */
    size_t sz = n64_rsp_state_size(S);
    uint8_t *st = malloc(sz);
    CHECK(st != NULL, "state buffer");
    if (st) {
        LOAD(R_ORI(4, 0, 0x66), R_BREAK);
        n64_rsp_state_save(S, st);
        LOAD(R_ORI(4, 0, 0x77), R_BREAK);
        go();
        CHECK(n64_rsp_get_gpr(S, 4) == 0x77, "before the load");
        CHECK(n64_rsp_state_load(S, st, sz) == 0, "state load");
        go();
        CHECK(n64_rsp_get_gpr(S, 4) == 0x66, "IMEM from the state: %X", n64_rsp_get_gpr(S, 4));
        free(st);
    }

    /* The same slot as VADD with selector 0 (the whole vector) then 8 (element 0 everywhere). */
    new_rsp();
    static const uint16_t v1[8] = {1, 2, 3, 4, 5, 6, 7, 8}, v2[8] = {10, 20, 30, 40, 50, 60, 70, 80};
    static const uint16_t whole[8] = {11, 22, 33, 44, 55, 66, 77, 88}, elem0[8] = {11, 12, 13, 14, 15, 16, 17, 18};
    wvec(0x000, v1);
    wvec(0x010, v2);
    LOAD(R_LQV(1, 0, 0, 0), R_LQV(2, 0, 1, 0), R_VOP(RV_VADD, 3, 1, 2, 0), R_SQV(3, 0, 2, 0), R_BREAK);
    go();
    CHECK(vec_eq(0x020, whole), "VADD whole vector");
    n64_wbe32(M + 0x1008, R_VOP(RV_VADD, 3, 1, 2, 8));
    go();
    CHECK(vec_eq(0x020, elem0), "VADD element 0 after the rewrite");
}

int main(int argc, char **argv)
{
    int print = argc > 1 && strcmp(argv[1], "--print") == 0;
    rdram = calloc(1, RDRAM_SIZE);
    if (!rdram)
        return 1;
    if (!print) {
        test_scalar();
        test_status();
        test_dma();
        test_dpc();
        test_vu_ops();
        test_vu_divide();
        test_cop2_moves();
        test_loads_stores();
        test_imem_rewrite();
    }
    test_oracle(print);
    n64_rsp_destroy(S);
    free(rdram);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("n64 rsp: all tests passed\n");
    return 0;
}
