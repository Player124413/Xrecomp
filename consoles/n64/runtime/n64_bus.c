/* The N64 physical memory map (N-4, XBA-172): RDRAM and its registers, RSP memory and registers, the RDP command
 * registers, MI, PI (cartridge DMA both ways), RI, SI (PIF DMA), the cartridge domains (ROM, SRAM / FlashRAM), the
 * PIF RAM and the IS-Viewer debug buffer. VI and AI are in n64_io.c, the PIF / joybus / saves in n64_pif.c.
 *
 * Simplifications (documented): DMAs copy their data at once and only their completion (busy flag, interrupt) is
 * timed; PI timing comes from the domain's LAT / PWD / PGS / RLS registers, without the PI's short-transfer and
 * misalignment quirks; the RDRAM and RI registers are plain storage whose reset values tell IPL3 the memory is
 * already configured (it then takes its warm-boot path). */
#include "n64_internal.h"

#include <stdlib.h>
#include <string.h>

#define MI_VERSION 0x02020102u

void n64_schedule(n64_machine *m, int ev, uint64_t at)
{
    m->ev[ev] = at;
    if (at < m->stop_at)
        m->stop_at = at;
}

void n64_mi_raise(n64_machine *m, int irq)
{
    m->mi_intr |= 1u << irq;
    n64_cpu_update_irq(m);
}

void n64_mi_clear(n64_machine *m, int irq)
{
    m->mi_intr &= ~(1u << irq);
    n64_cpu_update_irq(m);
}

uint32_t n64_bus_cost(uint32_t pa)
{
    if (pa < 0x04000000u)
        return 8;
    if (pa < 0x04040000u)
        return 12; /* RSP DMEM / IMEM */
    if (pa < 0x05000000u)
        return 40; /* RCP registers: about 20 cycles */
    if (pa < 0x1FC00000u)
        return 120; /* cartridge bus through the PI */
    return 600;    /* PIF RAM through the SI */
}

/* ---- MI -------------------------------------------------------------------------------------------------------- */

static uint32_t mi_read(n64_machine *m, uint32_t reg)
{
    switch (reg) {
    case 0: return m->mi_mode;
    case 1: return MI_VERSION;
    case 2: return m->mi_intr;
    case 3: return m->mi_mask;
    default: return 0;
    }
}

static void mi_write(n64_machine *m, uint32_t reg, uint32_t v)
{
    if (reg == 0) {
        uint32_t mode = (m->mi_mode & ~0x7Fu) | (v & 0x7F);
        if (v & 0x80) mode &= ~0x80u;
        if (v & 0x100) mode |= 0x80u;
        if (v & 0x200) mode &= ~0x100u;
        if (v & 0x400) mode |= 0x100u;
        if (v & 0x1000) mode &= ~0x200u;
        if (v & 0x2000) mode |= 0x200u;
        m->mi_mode = mode;
        if (v & 0x800)
            n64_mi_clear(m, N64_MI_DP);
    } else if (reg == 3) {
        for (int i = 0; i < 6; i++) {
            if (v & (1u << (2 * i)))
                m->mi_mask &= ~(1u << i);
            if (v & (2u << (2 * i)))
                m->mi_mask |= 1u << i;
        }
        n64_cpu_update_irq(m);
    }
}

/* ---- Cartridge bus --------------------------------------------------------------------------------------------- */

static uint32_t open_bus(uint32_t pa)
{
    return ((pa & 0xFFFF) << 16) | ((pa + 2) & 0xFFFF);
}

static uint32_t rom_read32(n64_machine *m, uint32_t pa)
{
    uint32_t off = pa - 0x10000000u;
    if (off + 4 <= m->rom_size)
        return n64_be32(m->rom + off);
    if (m->isv && pa >= 0x13FF0000u && pa < 0x14000000u)
        return n64_be32(m->isv + (pa & 0xFFFC));
    return open_bus(pa);
}

static void isv_write(n64_machine *m, uint32_t pa, uint32_t v)
{
    if (!m->isv) {
        m->isv = calloc(1, 0x10000);
        if (!m->isv)
            return;
    }
    uint32_t off = pa & 0xFFFC;
    n64_wbe32(m->isv + off, v);
    if (off == 0x14 && v) { /* length of the text at 0x20 */
        uint32_t n = v < 0x10000 - 0x20 ? v : 0x10000 - 0x20;
        if (m->tty)
            for (uint32_t i = 0; i < n; i++)
                m->tty(m->tty_ctx, (char)m->isv[0x20 + i]);
        n64_wbe32(m->isv + 0x14, 0);
    }
}

/* Bytes of the cartridge bus (PI DMA source). */
static void cart_read(n64_machine *m, uint32_t cart, uint8_t *dst, uint32_t len)
{
    if (cart >= 0x08000000u && cart < 0x10000000u) {
        n64_cart2_dma_read(m, cart, dst, len);
        return;
    }
    for (uint32_t i = 0; i < len; i++) {
        uint32_t a = cart + i;
        uint32_t off = a - 0x10000000u;
        if (a >= 0x10000000u && off < m->rom_size)
            dst[i] = m->rom[off];
        else if (a >= 0x10000000u && a < 0x1FC00000u)
            dst[i] = (uint8_t)(open_bus(a & ~3u) >> (8 * (3 - (a & 3))));
        else
            dst[i] = 0;
    }
}

/* ---- PI -------------------------------------------------------------------------------------------------------- */

static uint64_t pi_dma_ticks(n64_machine *m, uint32_t cart, uint32_t len)
{
    int dom = (cart >= 0x08000000u && cart < 0x10000000u) || (cart >= 0x05000000u && cart < 0x06000000u) ? 1 : 0;
    uint32_t lat = m->pi_dom[dom][0] & 0xFF, pwd = m->pi_dom[dom][1] & 0xFF;
    uint32_t pgs = m->pi_dom[dom][2] & 0xF, rls = m->pi_dom[dom][3] & 3;
    uint64_t page = 1ull << (pgs + 2);
    uint64_t pages = (len + page - 1) / page;
    uint64_t rcp = pages * (lat + 1) + (uint64_t)((len + 1) / 2) * (pwd + 1 + rls + 1);
    if (rcp < 16)
        rcp = 16;
    return rcp * 3; /* RCP cycles -> ticks */
}

/* Cartridge -> RDRAM the way the PI does it (n64brew « PI DMA », checked by n64-systemtest): blocks of up to 128 bytes
 * through its buffer, each ending at the RDRAM address's 2 KB page and shortened by the address's misalignment to 8
 * bytes, halfwords read from the cartridge, an odd remainder rounded up after the first block; the first block loses
 * as many bytes at its end as the address is misaligned (one more byte when it is one short of a full 128 - misalign
 * block, but not of one the page's end cut), and the RDRAM
 * address is rounded up to 8 after each block. When the first address is in the last 8 bytes of its page, the next
 * block (at the next page's start, aligned) is still shortened by that misalignment, and the rounding of the address
 * after it skips as many bytes (n64-systemtest "DMA CART -> RDRAM (misaligned, end of page)"; "cross page", a first
 * block cut further from the page's end, shows no such loss). */
static void pi_dma_to_rdram(n64_machine *m, uint32_t *dram_p, uint32_t *cart_p, int32_t len)
{
    uint8_t buf[128];
    uint32_t dram = *dram_p, cart = *cart_p;
    bool first = true;
    int32_t carry = 0; /* the first block's misalignment, when the page's end cut that block */
    while (len > 0) {
        int32_t misalign = (int32_t)(dram & 7);
        int32_t block = 128 - (carry ? carry : misalign);
        int32_t to_page = 0x800 - (int32_t)(dram & 0x7FF);
        carry = 0;
        if (block > to_page) {
            block = to_page;
            if (first && to_page < 8)
                carry = misalign;
        }
        int32_t cur = len < block ? len : block;
        len -= cur;
        if (len & 1)
            len++;
        int32_t rom_len = (cur + 1) & ~1;
        cart_read(m, cart, buf, (uint32_t)rom_len);
        cart += (uint32_t)rom_len;
        if (first) {
            if (cur == block - 1 && block == 128 - misalign) /* not when the page's end cut it (libdragon's
                                                              * test_dma_read_misalign: 31 bytes before a page end) */
                cur++;
            cur -= misalign;
            if (cur < 0)
                cur = 0;
        }
        n64_rdram_wrote(m, dram, (uint32_t)cur);
        for (int32_t i = 0; i < cur; i++, dram++)
            if (dram < m->rdram_size)
                m->rdram[dram] = buf[i];
        dram = (dram + 7) & ~7u;
        first = false;
    }
    *dram_p = dram & 0x00FFFFFFu;
    *cart_p = cart;
}

static void pi_dma(n64_machine *m, bool to_rdram, uint32_t len_reg)
{
    uint32_t len = (len_reg & 0x00FFFFFFu) + 1;
    uint32_t dram = m->pi_dram & 0x00FFFFFEu, cart = m->pi_cart & 0xFFFFFFFEu;
    uint32_t start_cart = cart;
    if (to_rdram) {
        pi_dma_to_rdram(m, &dram, &cart, (int32_t)len);
    } else {
        if (cart >= 0x08000000u && cart < 0x10000000u && dram < m->rdram_size) {
            uint32_t n = len;
            if (dram + n > m->rdram_size)
                n = m->rdram_size - dram;
            n64_cart2_dma_write(m, cart, m->rdram + dram, n);
        }
        dram = (dram + len + 7) & ~7u & 0x00FFFFFFu;
        cart = (cart + len + 1) & ~1u;
    }
    m->pi_dram = dram;
    m->pi_cart = cart;
    m->pi_status |= 1u;
    n64_schedule(m, EV_PI, m->now + pi_dma_ticks(m, start_cart, len));
    m->pi_dmas++;
}

/* CPU accesses to the cartridge bus (PI I/O). A write is held by the PI: the next read anywhere on the bus returns it
 * (once), and it fades after a short while; while it is held, PI_STATUS shows I/O busy and further writes are
 * dropped (n64-systemtest; A Bug's Life relies on it). The write still reaches the save chip in domain 2. */
#define PI_LATCH_TICKS 400

static bool pi_latch_held(n64_machine *m)
{
    return m->pi_latch_valid && m->now < m->pi_latch_until;
}

static uint32_t cart_io_read(n64_machine *m, uint32_t pa)
{
    if (pi_latch_held(m)) {
        m->pi_latch_valid = false;
        return m->pi_latch;
    }
    m->pi_latch_valid = false;
    if (pa < 0x08000000u)
        return 0; /* 64DD (absent) */
    if (pa < 0x10000000u)
        return n64_cart2_read32(m, pa);
    return rom_read32(m, pa);
}

static void cart_io_write(n64_machine *m, uint32_t pa, uint32_t v)
{
    if (pa >= 0x13FF0000u && pa < 0x14000000u) { /* IS-Viewer: RAM on development carts */
        isv_write(m, pa, v);
        return;
    }
    if (pi_latch_held(m))
        return;
    m->pi_latch = v;
    m->pi_latch_valid = true;
    m->pi_latch_until = m->now + PI_LATCH_TICKS;
    if (pa >= 0x08000000u && pa < 0x10000000u)
        n64_cart2_write32(m, pa, v);
}

static uint32_t pi_read(n64_machine *m, uint32_t reg)
{
    switch (reg) {
    case 0: return m->pi_dram;
    case 1: return m->pi_cart;
    case 2: case 3: return 0x7F;
    case 4: return (m->pi_status & 1u) | (pi_latch_held(m) ? 2u : 0u) | ((m->mi_intr >> N64_MI_PI) & 1u) << 3;
    default:
        if (reg >= 5 && reg <= 12)
            return m->pi_dom[(reg - 5) / 4][(reg - 5) % 4];
        return 0;
    }
}

static void pi_write(n64_machine *m, uint32_t reg, uint32_t v)
{
    switch (reg) {
    case 0: m->pi_dram = v & 0x00FFFFFEu; break;
    case 1: m->pi_cart = v & 0xFFFFFFFEu; break;
    case 2: if (!(m->pi_status & 1)) pi_dma(m, false, v); break;
    case 3: if (!(m->pi_status & 1)) pi_dma(m, true, v); break;
    case 4:
        if (v & 1) { /* reset: abort the DMA */
            m->pi_status = 0;
            m->ev[EV_PI] = EV_NEVER;
        }
        if (v & 2)
            n64_mi_clear(m, N64_MI_PI);
        break;
    default:
        if (reg >= 5 && reg <= 12) {
            static const uint32_t mask[4] = {0xFF, 0xFF, 0xF, 0x3};
            m->pi_dom[(reg - 5) / 4][(reg - 5) % 4] = v & mask[(reg - 5) % 4];
        }
        break;
    }
}

/* ---- SI -------------------------------------------------------------------------------------------------------- */

#define SI_DMA_TICKS (4096 * N64_TICKS_PER_CYCLE)

static uint32_t si_read(n64_machine *m, uint32_t reg)
{
    switch (reg) {
    case 0: return m->si_dram;
    case 6: return (m->si_status & 3u) | ((m->mi_intr >> N64_MI_SI) & 1u) << 12;
    default: return 0;
    }
}

static void si_write(n64_machine *m, uint32_t reg, uint32_t v)
{
    switch (reg) {
    case 0:
        m->si_dram = v & 0x00FFFFFCu;
        break;
    case 1: /* PIF_AD_RD64B: PIF RAM -> RDRAM */
        m->si_dmas++;
        if (m->si_dram + 64 <= m->rdram_size) {
            memcpy(m->rdram + m->si_dram, m->pif_ram, 64);
            n64_rdram_wrote(m, m->si_dram, 64);
        }
        m->si_status |= 1u;
        n64_schedule(m, EV_SI, m->now + SI_DMA_TICKS);
        break;
    case 4: /* PIF_AD_WR64B: RDRAM -> PIF RAM, then the PIF runs the commands */
        m->si_dmas++;
        if (m->si_dram + 64 <= m->rdram_size)
            for (uint32_t i = 0; i < 64; i += 4)
                n64_pif_write_ram(m, i, n64_be32(m->rdram + m->si_dram + i));
        n64_pif_process(m);
        m->si_status |= 1u;
        n64_schedule(m, EV_SI, m->now + SI_DMA_TICKS);
        break;
    case 6:
        n64_mi_clear(m, N64_MI_SI);
        break;
    default:
        break;
    }
}

/* ---- Dispatch -------------------------------------------------------------------------------------------------- */

uint32_t n64_bus_read32(n64_machine *m, uint32_t pa)
{
    if (pa < 0x03F00000u)
        return pa + 4 <= m->rdram_size ? n64_be32(m->rdram + pa) : 0;
    if (pa < 0x04000000u)
        return m->rdram_regs[(pa >> 2) & 15];
    if (pa < 0x04040000u)
        return n64_be32(m->spmem + (pa & 0x1FFC));
    if (pa < 0x04100000u) {
        if (pa < 0x04080000u)
            return n64_rsp_read_reg(m->rsp, (pa >> 2) & 7);
        return n64_rsp_read_reg(m->rsp, 8 + ((pa >> 2) & 1));
    }
    if (pa < 0x04200000u)
        return n64_rdp_read_reg(m->rdp, (pa >> 2) & 7);
    if (pa < 0x04300000u)
        return 0; /* DP span registers */
    if (pa < 0x04400000u)
        return mi_read(m, (pa >> 2) & 3);
    if (pa < 0x04500000u)
        return n64_vi_read(m, (pa >> 2) & 15);
    if (pa < 0x04600000u)
        return n64_ai_read(m, (pa >> 2) & 7);
    if (pa < 0x04700000u)
        return pi_read(m, (pa >> 2) & 15);
    if (pa < 0x04800000u)
        return m->ri[(pa >> 2) & 7];
    if (pa < 0x04900000u)
        return si_read(m, (pa >> 2) & 7);
    if (pa < 0x05000000u)
        return 0; /* unmapped */
    if (pa < 0x1FC00000u)
        return cart_io_read(m, pa);
    if (pa < 0x1FC007C0u)
        return 0; /* PIF ROM: HLE, no image */
    if (pa < 0x1FC00800u)
        return n64_be32(m->pif_ram + (pa & 0x3C));
    return 0;
}

void n64_bus_write32(n64_machine *m, uint32_t pa, uint32_t v, uint32_t mask)
{
    if (pa < 0x03F00000u) {
        if (pa + 4 <= m->rdram_size) {
            uint8_t *p = m->rdram + pa;
            n64_wbe32(p, (n64_be32(p) & ~mask) | (v & mask));
            n64_rdram_wrote(m, pa, 4);
        }
        return;
    }
    if (pa < 0x04000000u) {
        m->rdram_regs[(pa >> 2) & 15] = v;
        return;
    }
    if (pa < 0x04040000u) {
        uint8_t *p = m->spmem + (pa & 0x1FFC);
        n64_wbe32(p, (n64_be32(p) & ~mask) | (v & mask));
        return;
    }
    if (pa < 0x04100000u) {
        if (pa < 0x04080000u) {
            n64_rsp_write_reg(m->rsp, (pa >> 2) & 7, v);
            if (((pa >> 2) & 7) == 7)
                n64_pif_x105_release(m);
        }
        else
            n64_rsp_write_reg(m->rsp, 8 + ((pa >> 2) & 1), v);
        m->stop_at = m->now; /* the RSP may have been started: let the scheduler see it */
        return;
    }
    if (pa < 0x04200000u) {
        n64_rdp_write_reg(m->rdp, (pa >> 2) & 7, v);
        n64_rdp_run(m->rdp);
        return;
    }
    if (pa < 0x04300000u)
        return;
    if (pa < 0x04400000u) {
        mi_write(m, (pa >> 2) & 3, v);
        return;
    }
    if (pa < 0x04500000u) {
        n64_vi_write(m, (pa >> 2) & 15, v);
        return;
    }
    if (pa < 0x04600000u) {
        n64_ai_write(m, (pa >> 2) & 7, v);
        return;
    }
    if (pa < 0x04700000u) {
        pi_write(m, (pa >> 2) & 15, v);
        return;
    }
    if (pa < 0x04800000u) {
        m->ri[(pa >> 2) & 7] = v;
        return;
    }
    if (pa < 0x04900000u) {
        si_write(m, (pa >> 2) & 7, v);
        return;
    }
    if (pa < 0x05000000u)
        return;
    if (pa < 0x1FC00000u) {
        cart_io_write(m, pa, v);
        return;
    }
    if (pa >= 0x1FC007C0u && pa < 0x1FC00800u) {
        uint32_t off = pa & 0x3C;
        uint32_t old = n64_be32(m->pif_ram + off);
        n64_pif_write_ram(m, off, (old & ~mask) | (v & mask));
        if (off == 0x3C)
            n64_pif_process(m);
    }
}

uint64_t n64_bus_read64(n64_machine *m, uint32_t pa)
{
    return (uint64_t)n64_bus_read32(m, pa) << 32 | n64_bus_read32(m, pa + 4);
}

void n64_bus_write64(n64_machine *m, uint32_t pa, uint64_t v)
{
    n64_bus_write32(m, pa, (uint32_t)(v >> 32), 0xFFFFFFFFu);
    n64_bus_write32(m, pa + 4, (uint32_t)v, 0xFFFFFFFFu);
}
