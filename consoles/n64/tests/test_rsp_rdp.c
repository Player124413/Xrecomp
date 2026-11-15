/* N64 RSP + RDP together (N-5, XBA-173): RSP microcode drives the real RDP (n64_rdp.c) through COP0 8..15, the way
 * graphics microcode does. The CPU leaves an RDP command list in RDRAM; the microcode DMAs it into DMEM, switches the
 * RDP to XBUS (DPC_STATUS set xbus), points DPC_START / DPC_END at it, polls DPC_CURRENT until the RDP consumed it,
 * then breaks. Checked: the framebuffer the fill rectangle drew, the DP interrupt of SYNC_FULL, the DPC registers as
 * the RSP read them back, and the same list from RDRAM (xbus cleared). */
#include "n64_devices.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "homebrew/common/rsp_asm.h"
#include "rdp_cmd.h"

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

static int irq_dp, irq_sp;
static void raise_irq(void *ctx, int irq)
{
    (void)ctx;
    irq_dp += irq == N64_MI_DP;
    irq_sp += irq == N64_MI_SP;
}
static void clear_irq(void *ctx, int irq) { (void)ctx; (void)irq; }

static rdp_list LS;

/* Pixels of the 32 x 16 RGBA16 framebuffer equal to c. */
static int count16(const uint8_t *ram, uint16_t c)
{
    int n = 0;
    for (uint32_t i = 0; i < 32 * 16; i++)
        n += n64_be16(ram + FB_ADDR + 2 * i) == c;
    return n;
}

int main(void)
{
    uint8_t *ram = calloc(1, RAM_SIZE);
    if (!ram)
        return 1;
    n64_rcp_bus bus = {ram, RAM_SIZE, raise_irq, clear_irq, NULL, NULL, NULL};
    n64_rsp *s = n64_rsp_create(bus);
    n64_rdp *d = n64_rdp_create(bus);
    n64_rsp_attach_rdp(s, d);
    uint8_t *m = n64_rsp_mem(s);

    /* The list: 16-bit 32 x 16 frame, fill mode, fill rectangle (4,2)-(11,5) inclusive = 8 x 4 pixels, SYNC_FULL. */
    LS.n = 0;
    L(&LS, c_color_image(F_RGBA, SZ16, 32, FB_ADDR));
    L(&LS, c_scissor(0, 0, 32 * 4, 16 * 4));
    L(&LS, c_modes(OM_FILL));
    L(&LS, c_fill_color(0xF801F801u));
    L(&LS, c_fill_rect(4 * 4, 2 * 4, 11 * 4, 5 * 4));
    L(&LS, c_sync_full());
    for (int i = 0; i < LS.n; i++) {
        n64_wbe32(ram + CMD_BASE + 8 * i, (uint32_t)(LS.w[i] >> 32));
        n64_wbe32(ram + CMD_BASE + 8 * i + 4, (uint32_t)LS.w[i]);
    }
    uint32_t len = 8u * (uint32_t)LS.n;

    /* Microcode: DMA RDRAM CMD_BASE -> DMEM 0x800 (r1 = DRAM address, r2 = length - 1 from DMEM 0/4), XBUS on, START,
     * END, poll CURRENT == END, store the DPC registers it sees at 0x100.., BREAK. */
    const uint32_t prog[] = {
        R_LW(1, 0, 0), R_LW(2, 4, 0), R_ORI(3, 0, 0x800),
        R_MTC0(3, 0), R_MTC0(1, 1), R_MTC0(2, 2),         /* SP DMA in */
        R_MFC0(4, 6), R_BNE(4, 0, -2), R_NOP,             /* wait DMA_BUSY */
        R_ORI(5, 0, 0x2), R_MTC0(5, 11),                  /* DPC_STATUS: set xbus */
        R_MTC0(3, 8),                                     /* DPC_START = 0x800 */
        R_ADDIU(6, 3, (int)len), R_MTC0(6, 9),            /* DPC_END = 0x800 + len */
        R_MFC0(7, 10), R_BNE(7, 6, -2), R_NOP,            /* wait DPC_CURRENT == END */
        R_MFC0(8, 11), R_SW(7, 0x100, 0), R_SW(8, 0x104, 0),
        R_ORI(5, 0, 0x1), R_MTC0(5, 11),                  /* DPC_STATUS: clear xbus */
        R_BREAK,
    };
    for (unsigned i = 0; i < sizeof prog / sizeof prog[0]; i++)
        n64_wbe32(m + 0x1000 + 4 * i, prog[i]);
    n64_wbe32(m + 0, CMD_BASE);
    n64_wbe32(m + 4, len - 1);
    n64_rsp_write_reg(s, 8, 0);
    n64_rsp_write_reg(s, 4, 0x5);
    uint32_t used = n64_rsp_run(s, 100000);
    CHECK(n64_rsp_halted(s) && used < 1000, "microcode halted (%u cycles)", used);
    n64_rdp_flush(d);
    CHECK(count16(ram, 0xF801) == 32, "fill rectangle from DMEM over XBUS: %d pixels", count16(ram, 0xF801));
    CHECK(n64_be16(ram + FB_ADDR + 2 * (2 * 32 + 4)) == 0xF801 && n64_be16(ram + FB_ADDR + 2 * (2 * 32 + 3)) == 0,
          "rectangle edges");
    CHECK(irq_dp == 1 && irq_sp == 0, "SYNC_FULL raised the DP interrupt (%d, sp %d)", irq_dp, irq_sp);
    CHECK(n64_be32(m + 0x100) == 0x800 + len, "DPC_CURRENT as the RSP saw it %08X", n64_be32(m + 0x100));
    CHECK(n64_be32(m + 0x104) & 0x1, "DPC_STATUS xbus as the RSP saw it %08X", n64_be32(m + 0x104));
    CHECK(!(n64_rdp_read_reg(d, 3) & 0x1), "xbus cleared by the microcode");

    /* The same list straight from RDRAM, started by the RSP without XBUS, after clearing the frame. */
    memset(ram + FB_ADDR, 0, 32 * 16 * 2);
    const uint32_t prog2[] = {
        R_LW(1, 0, 0), R_MTC0(1, 8), R_ADDIU(6, 1, (int)len), R_MTC0(6, 9),
        R_MFC0(7, 10), R_BNE(7, 6, -2), R_NOP, R_BREAK,
    };
    for (unsigned i = 0; i < sizeof prog2 / sizeof prog2[0]; i++)
        n64_wbe32(m + 0x1000 + 4 * i, prog2[i]);
    n64_rsp_write_reg(s, 8, 0);
    n64_rsp_write_reg(s, 4, 0x5);
    n64_rsp_run(s, 100000);
    n64_rdp_flush(d);
    CHECK(n64_rsp_halted(s), "second microcode halted");
    CHECK(count16(ram, 0xF801) == 32, "fill rectangle from RDRAM: %d pixels", count16(ram, 0xF801));
    CHECK(irq_dp == 2, "second DP interrupt (%d)", irq_dp);

    n64_rdp_destroy(d);
    n64_rsp_destroy(s);
    free(ram);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("n64 rsp + rdp: all tests passed\n");
    return 0;
}
