/* Internal state of the PS1 core (XBA-162 / 163 / 164 / 167). Not a public API: ps1_core.h is. The unit tests include
 * it to set up CPU and GTE states directly. */
#ifndef PS1_INTERNAL_H
#define PS1_INTERNAL_H

#include "ps1_core.h"
#include "ps1_devices.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PS1_SCRATCH_SIZE 0x400u

/* RAM write tracking for the static recompiler (ps1_recomp.c): one counter per 1 KB page of the 2 MB, bumped by
 * every write into it (CPU stores, lifted stores, DMA, EXE load), so lifted code is re-checked against its hash only
 * when a page it covers changed. */
#define PS1_RAM_PAGE_SHIFT 10
#define PS1_RAM_PAGES (PS1_RAM_SIZE >> PS1_RAM_PAGE_SHIFT)
#define PS1_RAM_TOUCH(m, phys) ((m)->ram_gen[((phys) & (PS1_RAM_SIZE - 1)) >> PS1_RAM_PAGE_SHIFT]++)

/* COP0 registers. */
enum {
    C0_BPC = 3,
    C0_BDA = 5,
    C0_JUMPDEST = 6,
    C0_DCIC = 7,
    C0_BADVADDR = 8,
    C0_BDAM = 9,
    C0_BPCM = 11,
    C0_SR = 12,
    C0_CAUSE = 13,
    C0_EPC = 14,
    C0_PRID = 15,
};

/* Exception codes (CAUSE bits 2-6). */
enum {
    EXC_INT = 0,
    EXC_ADEL = 4,  /* address error, load or fetch */
    EXC_ADES = 5,  /* address error, store */
    EXC_IBE = 6,   /* bus error, fetch */
    EXC_DBE = 7,   /* bus error, data */
    EXC_SYS = 8,
    EXC_BP = 9,
    EXC_RI = 10,   /* reserved instruction */
    EXC_CPU = 11,  /* coprocessor unusable */
    EXC_OV = 12,   /* arithmetic overflow */
};

#define SR_IEC 0x1u
#define SR_KUC 0x2u
#define SR_ISC 0x10000u
#define SR_BEV 0x400000u
#define SR_CU0 0x10000000u
#define SR_CU2 0x40000000u

/* ---- GTE (ps1_gte.c) ---- */
typedef struct ps1_gte {
    /* Data registers 0-31 as the hardware holds them (sign / zero extension applied on MFC2). */
    int16_t v[3][3];        /* VXY0..VZ2 (0-5) */
    uint8_t rgbc[4];        /* 6 */
    uint16_t otz;           /* 7 */
    int16_t ir[4];          /* 8-11: IR0..IR3 */
    int16_t sxy[3][2];      /* 12-14: SXY0..SXY2 (SXYP is 15) */
    uint16_t sz[4];         /* 16-19: SZ0..SZ3 */
    uint8_t rgb_fifo[3][4]; /* 20-22: RGB0..RGB2 */
    uint32_t res1;          /* 23 */
    int32_t mac[4];         /* 24-27: MAC0..MAC3 */
    uint16_t irgb;          /* 28 (write), ORGB 29 is computed */
    int32_t lzcs;           /* 30 */
    int32_t lzcr;           /* 31 */
    /* Control registers 32-63. */
    int16_t rt[3][3];       /* rotation */
    int32_t tr[3];          /* translation */
    int16_t llm[3][3];      /* light */
    int32_t bk[3];          /* background colour */
    int16_t lcm[3][3];      /* light colour */
    int32_t fc[3];          /* far colour */
    int32_t ofx, ofy;
    uint16_t h;
    int16_t dqa;
    int32_t dqb;
    int16_t zsf3, zsf4;
    uint32_t flag;
} ps1_gte;

void ps1_gte_reset(ps1_gte *g);
uint32_t ps1_gte_read_data(ps1_gte *g, int r);
void ps1_gte_write_data(ps1_gte *g, int r, uint32_t v);
uint32_t ps1_gte_read_ctrl(ps1_gte *g, int r);
void ps1_gte_write_ctrl(ps1_gte *g, int r, uint32_t v);
/* Runs a GTE command (the low 25 bits of a COP2 imm25 instruction). Returns its cycle cost. */
int ps1_gte_command(ps1_gte *g, uint32_t cmd);

/* ---- Timers (root counters) ---- */
typedef struct ps1_timer {
    uint16_t counter;
    uint16_t mode;
    uint16_t target;
    uint32_t frac;      /* sub-tick accumulator (sysclk/8: CPU cycles; dot clock: CPU cycles x 11) */
    bool irq_done;      /* one-shot mode: the IRQ already fired */
    bool gate_wait;     /* sync mode 3: waiting for the first blank */
} ps1_timer;

/* ---- DMA ---- */
typedef struct ps1_dma_chan {
    uint32_t madr, bcr, chcr;
} ps1_dma_chan;

/* ---- SIO0: pads and memory cards ---- */
typedef struct ps1_pad {
    ps1_pad_kind kind;
    uint16_t buttons;   /* 1 = pressed */
    uint8_t lx, ly, rx, ry;
    /* DualShock (ps1_sio.c). */
    bool analog;        /* reports id 0x73 (LED on) */
    bool config;        /* in config mode (id 0xF3), entered with 0x43 */
    bool locked;        /* the game locked the mode (0x44 key 03h): the ANALOG button does nothing */
    uint8_t rumble_map[6]; /* 0x4D: which motor each poll byte drives (00h small, 01h large, FFh none) */
    uint8_t motor_small, motor_large;
} ps1_pad;

typedef struct ps1_card {
    uint8_t data[PS1_CARD_SIZE];
    bool inserted;
    bool dirty;
    uint8_t flag;       /* FLAG byte: bit 3 = directory not read since insertion */
} ps1_card;

/* CPU cycles the device holds /ACK low (JOY_STAT bit 7) from the IRQ: psx-spx « Controllers and Memory Cards »
 * gives about 2 us (68 cycles at 33.8688 MHz); a little more so a poll right after the IRQ still sees it. */
#define PS1_SIO_ACK_PULSE 100

typedef struct ps1_sio {
    uint16_t mode, ctrl, baud;
    uint16_t stat_irq;  /* JOY_STAT bit 9 */
    uint8_t rx;         /* the received byte */
    bool rx_full;
    bool ack;           /* /ACK seen for the last byte: JOY_STAT bit 7 while cycles < done_at (the pulse) */
    bool busy;          /* transfer in flight until done_at */
    bool ack_pending;   /* the device's /ACK comes at done_at */
    uint64_t done_at;
    uint8_t tx;
    /* Device state machine. */
    int device;         /* 0 none selected yet, 1 pad, 2 memory card, -1 ignore until deselect */
    int step;           /* byte index within the command */
    uint8_t cmd;        /* memory card command: 'R', 'W', 'S' */
    uint16_t sector;
    uint8_t chk;
    uint8_t buf[128];
    uint8_t next_reply; /* the device's reply to the byte in flight */
    bool next_ack;
    uint8_t wr_status;  /* memory card write: end status (47h good, 4Eh bad checksum) */
    uint8_t pad_cmd;    /* pad command byte (42h read, 43h..4Dh config) */
    uint8_t pad_arg;    /* its first parameter byte (the mode of 44h, the index of 46h / 47h / 4Ch) */
    bool pad_long;      /* the command answers 6 data bytes (analog mode or config mode), else 2 */
    bool pad_poll;      /* it answers the pad's state (42h, 43h outside config mode) */
} ps1_sio;

/* A DualShock plugged in (another kind than before on the port, ps1_set_pad): mode from the kind, unlocked, no rumble
 * mapping (ps1_sio.c). */
void ps1_pad_plug(ps1_pad *p, ps1_pad_kind kind);

/* ---- The machine ---- */
struct ps1_machine {
    /* CPU (hot fields first). */
    uint32_t r[32];
    uint32_t pc;            /* the instruction about to run */
    uint32_t next_pc;       /* the one after it (a branch target once a branch ran) */
    uint32_t hi, lo;
    uint32_t cur_pc;        /* the running instruction */
    uint32_t ld_reg, ld_val;     /* load issued by the running instruction (lands after the next one) */
    uint32_t pend_reg, pend_val; /* load issued by the previous instruction (lands after the running one) */
    bool in_delay;          /* the running instruction is in a branch delay slot */
    bool next_delay;        /* the next instruction will be */
    bool branch_taken;      /* the branch before the running instruction was taken (CAUSE.BT) */
    bool next_taken;
    bool irq_pending;       /* SR.IEc && (SR.IM & CAUSE.IP) */
    uint32_t hook_at;      /* a jump went to a hooked address (BIOS tables 0xA0 / 0xB0, shell 0x80030000) */
    uint32_t cop0[32];
    uint64_t cycles;
    uint64_t next_event;
    /* Static recompilation (ps1_recomp.h): the fields above and these five are ps1_rc_head (ps1_recomp_rt.h), the
     * lifted code's view of the machine; ps1_recomp.c checks every offset. */
    uint8_t *ram;           /* PS1_RAM_SIZE */
    uint8_t *rc_scratch;    /* = scratch */
    uint32_t *ram_gen;      /* PS1_RAM_PAGES write counters: bumped by every store / DMA into the page */
    uint64_t rc_native;     /* instructions run by lifted code (packs built with -DPS1_RC_COUNT) */
    uint8_t rc_exit;        /* PS1_RC_EXIT_*: why lifted code returned */
    uint8_t rc_hint;        /* with PS1_RC_EXIT_INTERP: how many instructions the interpreter runs (rt.h) */
    struct ps1_rc_state *rc; /* attached table, dispatch cache, missed record (NULL: none) */

    ps1_gte gte;

    uint8_t *bios;          /* PS1_BIOS_SIZE */
    uint8_t scratch[PS1_SCRATCH_SIZE];

    /* Memory / cache control, accepted and read back. */
    uint32_t mem_ctrl[9];
    uint32_t ram_size;
    uint32_t cache_ctrl;
    uint8_t post;

    /* Interrupt controller. */
    uint32_t i_stat, i_mask;

    /* DMA. */
    ps1_dma_chan dma[7];
    uint64_t dma_done_at[7]; /* a block / linked-list transfer in progress completes then (UINT64_MAX: none) */
    uint32_t dpcr, dicr;

    /* Timers. */
    ps1_timer tmr[3];
    uint64_t tmr_time;      /* cycle the timers have been advanced to */

    /* Video timing / scheduler. */
    ps1_gpu_timing timing;
    int line;               /* current scanline */
    uint64_t line_at;       /* cycle the current line started */
    uint64_t hblank_at;     /* cycle its hblank starts */
    uint64_t next_line_at;
    uint32_t line_rem;      /* remainder of video cycles x 7 / 11 */
    bool in_hblank, in_vblank;
    bool frame_done;
    uint64_t frames;
    uint64_t dev_time;      /* cycle the devices have been ticked to */

    /* SIO0. */
    ps1_sio sio;
    ps1_pad pads[2];
    ps1_card cards[2];
    uint16_t sio1_ctrl, sio1_mode, sio1_baud;

    /* Devices (ps1_devices.h). */
    ps1_gpu *gpu;
    ps1_cdrom *cdrom;
    ps1_spu *spu;
    ps1_mdec *mdec;
    ps1_disc *disc;

    /* Boot. */
    uint8_t *exe;
    size_t exe_size;
    bool exe_started;
    bool skip_shell;   /* ps1_set_fast_boot: the BIOS shell (its logo) returns at once, the BIOS boots the disc */
    bool shell_skipped;

    void (*tty)(void *ctx, char c);
    void *tty_ctx;

    /* The HLE BIOS (ps1_hle.c, XBA-163): NULL when a BIOS image runs (LLE). */
    struct ps1_hle *hle;

    /* Host side only, after everything a save state holds (ps1_state.c saves the machine up to PS1_MACHINE_SAVED). */
    struct ps1_blocks *blocks; /* the interpreter's predecoded blocks (ps1_cpu.c, XBA-204), made on first use */
    bool interp_ref;           /* the reference interpreter loop (ps1_cpu_set_reference) */
};
#define PS1_MACHINE_SAVED offsetof(ps1_machine, blocks)

/* ps1_cpu.c */
void ps1_cpu_reset(ps1_machine *m);
void ps1_cpu_run(ps1_machine *m);   /* runs instructions until cycles >= next_event */
void ps1_cpu_update_irq(ps1_machine *m);
void ps1_cpu_free(ps1_machine *m);          /* the predecoded blocks (ps1_destroy) */
void ps1_cpu_flush_blocks(ps1_machine *m);  /* RAM written behind the core's back (ps1_ram): decode again */
void ps1_cpu_exception(ps1_machine *m, uint32_t code, uint32_t badvaddr_or_cop);

/* ps1_bus.c: memory accesses. They return 0 or a pending exception code (EXC_*), the CPU raises it. */
int ps1_read32(ps1_machine *m, uint32_t addr, uint32_t *v);
int ps1_read16(ps1_machine *m, uint32_t addr, uint32_t *v);
int ps1_read8(ps1_machine *m, uint32_t addr, uint32_t *v);
int ps1_write32(ps1_machine *m, uint32_t addr, uint32_t v);
int ps1_write16(ps1_machine *m, uint32_t addr, uint32_t v);
int ps1_write8(ps1_machine *m, uint32_t addr, uint32_t v);

/* ps1_io.c: IO registers (phys = physical address in 0x1F801000..0x1F802FFF), width 1, 2 or 4. */
uint32_t ps1_io_read(ps1_machine *m, uint32_t phys, int width);
void ps1_io_write(ps1_machine *m, uint32_t phys, uint32_t v, int width);
void ps1_irq_raise(ps1_machine *m, int irq);
void ps1_sched_init(ps1_machine *m);
void ps1_sched_events(ps1_machine *m);  /* handles every event due at m->cycles, recomputes next_event */
void ps1_sched_recompute(ps1_machine *m);
void ps1_timers_sync(ps1_machine *m);
void ps1_devices_sync(ps1_machine *m);

/* ps1_sio.c */
void ps1_sio_reset(ps1_machine *m);
uint32_t ps1_sio_read(ps1_machine *m, uint32_t off, int width);
void ps1_sio_write(ps1_machine *m, uint32_t off, uint32_t v, int width);
void ps1_sio_event(ps1_machine *m);      /* the transfer in flight finished */
void ps1_card_format(uint8_t *data);

/* ps1_core.c: the shell entry hook (fast boot). */
void ps1_boot_exe(ps1_machine *m);

/* ps1_bus.c: the exception a load (store = 0) or store (store = 1) at addr would raise, 0 if none (no side effect). */
int ps1_bus_fault(ps1_machine *m, uint32_t addr, int store);

/* ps1_cpu.c: one interpreter step (the instruction at pc; no IRQ / hook check), the interpreter-only run loop. */
void ps1_cpu_step(ps1_machine *m);
/* Code a title pack can hold (ps1_recomp.c): main RAM above the BIOS's kernel area (the first 64 KB), from KUSEG, KSEG0
 * or KSEG1, word aligned. */
#define PS1_RC_LOW_RAM 0x10000u
static inline bool ps1_rc_liftable_pc(uint32_t pc)
{
    uint32_t seg = pc >> 29, p = pc & 0x1FFFFFFFu;
    return !(pc & 3) && (seg == 0 || seg == 4 || seg == 5) && p < 0x00800000u && (p & 0x1FFFFFu) >= PS1_RC_LOW_RAM;
}
/* The recompiler's loop for code no pack can hold (ps1_recomp_run): steps while the next instruction is not liftable,
 * no interrupt is pending, no hook is due and the time slice is not up, as that loop would call ps1_cpu_step for each
 * (the BIOS, its exception handler), without a call per instruction. */
void ps1_cpu_step_unliftable(ps1_machine *m);
void ps1_cpu_hook_check(ps1_machine *m);   /* the run loop's BIOS / fast-boot hook test before an instruction */
void ps1_cpu_take_interrupt(ps1_machine *m);

/* ps1_recomp.c */
void ps1_recomp_run(ps1_machine *m);        /* the run loop with a table attached (ps1_cpu_run calls it) */
void ps1_recomp_free(ps1_machine *m);
/* Save states (XBA-182, ps1_state.c): the boot flag of the attached code (-1: none), and the reset after a load. */
int ps1_recomp_booted(ps1_machine *m);
void ps1_recomp_state_loaded(ps1_machine *m, int booted);

#endif
