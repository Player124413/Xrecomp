/* n64_devices.h -- the Nintendo 64 Reality Co-Processor parts the core drives (Plane epic N, XBA-144).
 *
 * The core (n64_core.h: VR4300 CPU + COP0 / TLB + COP1, RDRAM, MI, PI, SI / PIF, VI, AI, saves) talks to the RSP and
 * the RDP only through this header, so each can be written and tested on its own:
 *   RSP  n64_rsp.c  scalar unit + vector unit (VU) interpreter, IMEM / DMEM, SP DMA, SP status and semaphore (LLE:
 *                   every microcode, audio and graphics, runs as on the hardware: universal, no per-game HLE)
 *   RDP  n64_rdp.c  command list processing (from RDRAM or from DMEM over XBUS), software rasterizer into RDRAM
 * Decision (C0-4 / N-5): the RDP is our own software renderer first (ideas from ares (ISC) and cen64 (BSD-3)); a GPU
 * path (GLES HLE renderer, or Vulkan for RT64) comes later behind the same command interface.
 *
 * Memory: RDRAM is one byte array in the console's big-endian byte order (byte i of the array is the byte at physical
 * address i); 32-bit accesses go through n64_be32 / n64_wbe32 below. Default size 8 MB (expansion pak on).
 * Time: the core calls n64_rsp_run with RSP cycles (62.5 MHz, 2/3 of the CPU's 93.75 MHz) while the RSP is not halted;
 * the RDP runs its pending commands when the core calls n64_rdp_run (after DPC_END writes and periodically). Interrupts
 * go through the MI sink. Frozen between the core, RSP and RDP agents: change together. */
#ifndef N64_DEVICES_H
#define N64_DEVICES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define N64_CPU_HZ 93750000u
#define N64_RCP_HZ 62500000u

static inline uint32_t n64_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static inline void n64_wbe32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static inline uint16_t n64_be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline void n64_wbe16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/* MI_INTR bits. */
enum { N64_MI_SP = 0, N64_MI_SI = 1, N64_MI_AI = 2, N64_MI_VI = 3, N64_MI_PI = 4, N64_MI_DP = 5 };

typedef struct n64_rcp_bus {
    uint8_t *rdram;        /* big-endian bytes */
    uint32_t rdram_size;   /* 4 or 8 MB */
    void (*raise)(void *ctx, int mi_irq); /* sets an MI_INTR bit */
    void (*clear)(void *ctx, int mi_irq); /* clears it (SP status writes clear the SP interrupt) */
    void *ctx;
    /* Optional (NULL: none): a device wrote RDRAM [addr, addr + len) (SP DMA to RDRAM). The core tracks writes into
     * RDRAM for its static recompilation (n64_recomp.h: code is checked by content). */
    void (*wrote)(void *ctx, uint32_t addr, uint32_t len);
    /* Optional (NULL: never): an RDP that can finish a SYNC_FULL later (the Vulkan RDP: the GPU still drawing) asks
     * at each SYNC_FULL. 0: complete it now (draw out, DP interrupt) as always; 1: the core calls
     * n64_rdp_dp_complete (n64_rdp.h) at the deadline it picked, and until then the RDP keeps its busy bits and does
     * not raise the interrupt. The RDP completes a pending one itself before asking again (n64_core.c « Deferred DP
     * interrupt »). */
    int (*defer_dp)(void *ctx);
} n64_rcp_bus;

typedef struct n64_rsp n64_rsp;
typedef struct n64_rdp n64_rdp;

/* ---- RSP ------------------------------------------------------------------------------------------------------------ */
n64_rsp *n64_rsp_create(n64_rcp_bus bus);
void n64_rsp_destroy(n64_rsp *s);
void n64_rsp_reset(n64_rsp *s); /* halted, PC 0 */
void n64_rsp_attach_rdp(n64_rsp *s, n64_rdp *dp); /* RSP COP0 registers 8..15 are the DPC registers */
/* DMEM (0x000..0xFFF) and IMEM (0x1000..0x1FFF) as 8 KB of big-endian bytes: the CPU reaches them at 0x04000000. */
uint8_t *n64_rsp_mem(n64_rsp *s);
/* SP registers: reg = (address - 0x04040000) / 4 for 0 .. 7 (MEM_ADDR, DRAM_ADDR, RD_LEN, WR_LEN, STATUS, DMA_FULL,
 * DMA_BUSY, SEMAPHORE); reg 8 = SP_PC (0x04080000), reg 9 = SP_IBIST (0x04080004). Reads of SEMAPHORE set it. */
uint32_t n64_rsp_read_reg(n64_rsp *s, uint32_t reg);
void n64_rsp_write_reg(n64_rsp *s, uint32_t reg, uint32_t v);
int n64_rsp_halted(n64_rsp *s);
/* Scalar register r (1..31; reads 0 and writes are ignored for anything else). The setter is for the HLE boot (the
 * register a CIC-6105 IPL3 leaves, n64_pif.c) and tests. */
uint32_t n64_rsp_get_gpr(n64_rsp *s, int r);
void n64_rsp_set_gpr(n64_rsp *s, int r, uint32_t v);
/* 1 while SP_SEMAPHORE is set, without the side effect of a read (which sets it). */
int n64_rsp_semaphore_held(n64_rsp *s);
/* Runs up to cycles RSP cycles (stops early on BREAK / halt). Returns the cycles used. */
uint32_t n64_rsp_run(n64_rsp *s, uint32_t cycles);

/* ---- Save states (XBA-183) ------------------------------------------------------------------------------------------ */
/* Each device's state as opaque bytes of this build (n64_core.h « Save states » puts them together). The RSP: IMEM /
 * DMEM, the scalar and vector registers, the accumulator and flags, the divider, PC, the SP registers and semaphore
 * (the placeholder: what it has). The RDP: the DPC registers and the command being collected, then the back end's own
 * state (the software RDP: modes, combiner, tiles, TMEM, the hidden RDRAM bits; the Vulkan RDP: nothing, it is drawn
 * out and synchronised first); a state from the other back end loads its registers only. _size first, then _save
 * into that many bytes (the RDP flushes what is batched first: take states between frames). _load returns 0, or -1
 * when the bytes are not this build's (wrong size or kind), the device then left as it was. */
size_t n64_rsp_state_size(n64_rsp *s);
void n64_rsp_state_save(n64_rsp *s, uint8_t *out);
int n64_rsp_state_load(n64_rsp *s, const uint8_t *in, size_t n);
size_t n64_rdp_state_size(n64_rdp *d);
void n64_rdp_state_save(n64_rdp *d, uint8_t *out);
int n64_rdp_state_load(n64_rdp *d, const uint8_t *in, size_t n);

/* ---- RDP ------------------------------------------------------------------------------------------------------------ */
n64_rdp *n64_rdp_create(n64_rcp_bus bus);
void n64_rdp_destroy(n64_rdp *d);
void n64_rdp_reset(n64_rdp *d);
/* XBUS: command lists read from the RSP's DMEM when DPC_STATUS.xbus_dmem_dma is set. */
void n64_rdp_attach_dmem(n64_rdp *d, const uint8_t *dmem /* 4 KB, big-endian */);
/* DPC registers: reg = (address - 0x04100000) / 4 for 0 .. 7 (START, END, CURRENT, STATUS, CLOCK, BUFBUSY, PIPEBUSY,
 * TMEM). A write to END queues the commands from CURRENT to END. */
uint32_t n64_rdp_read_reg(n64_rdp *d, uint32_t reg);
void n64_rdp_write_reg(n64_rdp *d, uint32_t reg, uint32_t v);
/* Executes the queued commands (rendering into RDRAM); a SYNC_FULL raises the DP interrupt. */
void n64_rdp_run(n64_rdp *d);

#ifdef __cplusplus
}
#endif

#endif
