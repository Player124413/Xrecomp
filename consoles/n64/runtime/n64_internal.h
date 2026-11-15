/* Internal state of the N64 core (XBA-169 / 171 / 172). Not a public API: n64_core.h is. The unit tests include it to
 * set up CPU states directly. */
#ifndef N64_INTERNAL_H
#define N64_INTERNAL_H

#include "n64_core.h"
#include "n64_devices.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define N64_LIKELY(x) __builtin_expect(!!(x), 1)
#define N64_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define N64_LIKELY(x) (x)
#define N64_UNLIKELY(x) (x)
#endif
/* The interpreter's decode and execute, inlined into its run loops (XBA-191): a call per instruction, with the
 * callee-saved registers of a big switch, was a fifth of the interpreter's cost. */
#if defined(__GNUC__) || defined(__clang__)
#define N64_HOT_INLINE inline __attribute__((always_inline))
#else
#define N64_HOT_INLINE inline
#endif

/* Time: the core counts ticks of half a CPU cycle (187.5 MHz), so the 1.5-cycle average instruction is a whole
 * number: COUNT advances every 4 ticks, the RCP (62.5 MHz) every 3. */
#define N64_TICKS_PER_CYCLE 2
#define N64_TICK_HZ (N64_CPU_HZ * 2ull)

/* ---- COP0 ------------------------------------------------------------------------------------------------------ */
enum {
    C0_INDEX = 0,
    C0_RANDOM = 1,
    C0_ENTRYLO0 = 2,
    C0_ENTRYLO1 = 3,
    C0_CONTEXT = 4,
    C0_PAGEMASK = 5,
    C0_WIRED = 6,
    C0_BADVADDR = 8,
    C0_COUNT = 9,
    C0_ENTRYHI = 10,
    C0_COMPARE = 11,
    C0_STATUS = 12,
    C0_CAUSE = 13,
    C0_EPC = 14,
    C0_PRID = 15,
    C0_CONFIG = 16,
    C0_LLADDR = 17,
    C0_WATCHLO = 18,
    C0_WATCHHI = 19,
    C0_XCONTEXT = 20,
    C0_PARITY = 26,
    C0_CACHEERR = 27,
    C0_TAGLO = 28,
    C0_TAGHI = 29,
    C0_ERROREPC = 30,
};

/* Exception codes (CAUSE bits 2-6). */
enum {
    EXC_INT = 0,
    EXC_MOD = 1,
    EXC_TLBL = 2,
    EXC_TLBS = 3,
    EXC_ADEL = 4,
    EXC_ADES = 5,
    EXC_IBE = 6,
    EXC_DBE = 7,
    EXC_SYS = 8,
    EXC_BP = 9,
    EXC_RI = 10,
    EXC_CPU = 11,
    EXC_OV = 12,
    EXC_TR = 13,
    EXC_FPE = 15,
    EXC_WATCH = 23,
};

#define SR_IE 0x00000001u
#define SR_EXL 0x00000002u
#define SR_ERL 0x00000004u
#define SR_KSU 0x00000018u
#define SR_UX 0x00000020u
#define SR_SX 0x00000040u
#define SR_KX 0x00000080u
#define SR_IM 0x0000FF00u
#define SR_BEV 0x00400000u
#define SR_FR 0x04000000u
#define SR_RE 0x02000000u
#define SR_CU0 0x10000000u
#define SR_CU1 0x20000000u
#define SR_CU2 0x40000000u

#define CAUSE_IP2 0x400u  /* the RCP (MI) */
#define CAUSE_IP7 0x8000u /* COUNT = COMPARE */
#define CAUSE_BD 0x80000000u

/* FCSR. */
#define FCSR_RM 0x3u
#define FCSR_FLAGS_SHIFT 2
#define FCSR_ENABLES_SHIFT 7
#define FCSR_CAUSE_SHIFT 12
#define FCSR_C 0x00800000u
#define FCSR_FS 0x01000000u
enum { FPE_I = 1, FPE_U = 2, FPE_O = 4, FPE_Z = 8, FPE_V = 16, FPE_E = 32 };

typedef struct n64_tlb_entry {
    uint64_t hi;      /* EntryHi as written: R (63:62), VPN2 (39:13), ASID (7:0) */
    uint32_t mask;    /* PageMask (24:13) */
    uint32_t lo[2];   /* EntryLo0 / 1: PFN (25:6), C (5:3), D (2), V (1), G (0) */
    /* Derived on write: the bits a virtual address is compared on, and the global flag (G of both halves). */
    uint64_t vpn_mask;
    uint64_t vpn;
    bool global;
} n64_tlb_entry;

/* The first members, through fcsr, are lifted code's view of the CPU (n64_rc_cpu in n64_recomp_rt.h; n64_recomp.c
 * checks every offset): keep them first and in this order. */
typedef struct n64_cpu {
    uint64_t gpr[32];
    uint64_t hi, lo;
    uint64_t pc;      /* the instruction to execute next */
    uint64_t npc;     /* the one after (a branch target once a branch ran) */
    uint64_t cur;     /* the instruction being executed (EPC of its exceptions) */
    bool ds;          /* the instruction at pc is in a delay slot */
    bool next_ds;
    bool llbit;
    bool irq;         /* an interrupt is to be taken before the next instruction */
    /* Derived from STATUS (n64_cpu_status_changed). */
    int ksu;          /* 0 kernel, 1 supervisor, 2 user (EXL / ERL force kernel) */
    bool addr64;      /* 64-bit addressing in the current mode (KX / SX / UX) */
    bool ops64;       /* 64-bit operations allowed (kernel, or SX / UX) */
    uint64_t cop0[32];
    uint64_t fpr[32];
    uint32_t fcsr;
    /* ---- end of n64_rc_cpu ---- */
    uint32_t cop2_latch_lo;
    uint64_t cop0_latch;  /* last value written to a COP0 register (unused registers read it) */
    n64_tlb_entry tlb[32];
    uint32_t tlb_gen;     /* bumped by TLBWI / TLBWR and EntryHi writes: cached translations of code are stale */
    uint64_t cop2_latch;
    /* COUNT runs at half the CPU clock: COUNT = count_base + (now - count_tick) / 4. */
    uint32_t count_base;
    uint64_t count_tick;
    /* RANDOM (n64_cpu.c random_reg): 31 at instruction random_base, the count of the instruction two after the last
     * WIRED write; before that instruction it still runs on the previous WIRED (wired_prev, random_base_prev). */
    uint64_t random_base;
    uint64_t random_base_prev;
    uint32_t wired_prev;
    bool re;              /* derived from STATUS: user mode with RE, reverse-endian accesses (n64_cpu.c) */
} n64_cpu;

/* ---- Events ---------------------------------------------------------------------------------------------------- */
enum { EV_VI, EV_COMPARE, EV_PI, EV_SI, EV_AI, EV_RSP, EV_COUNT };
#define EV_NEVER UINT64_MAX

/* ---- Joybus / saves -------------------------------------------------------------------------------------------- */
typedef struct n64_pad {
    bool connected;
    uint16_t buttons;
    int8_t x, y;
    n64_accessory acc;
    bool rumble;
    bool rumble_probe; /* 0x80 written to 0x8000: the rumble pak answers 0x80 */
} n64_pad;

#define N64_PAK_SIZE 0x8000u
#define N64_EEPROM_MAX 2048u
#define N64_SRAM_SIZE 0x8000u
#define N64_FLASH_SIZE 0x20000u

typedef enum { FLASH_IDLE = 0, FLASH_READ, FLASH_STATUS, FLASH_ERASE, FLASH_WRITE } n64_flash_mode;

/* ---- Audio ----------------------------------------------------------------------------------------------------- */
#define N64_AUDIO_RING 16384u /* sample frames, power of two */
#define AI_TAPS 32               /* the resampler's taps (n64_io.c) */
#define AI_PHASES 256            /* its polyphase rows */
#define AI_HIST 64               /* history ring, a power of two >= AI_TAPS */

/* RDRAM write tracking (static recompilation, n64_recomp.h): one counter per 4 KB page, bumped by every write into
 * RDRAM (CPU stores, lifted stores, PI / SI / SP DMA; not the RDP's rendering). */
#define N64_PAGE_SHIFT 12

struct n64_machine {
    /* ---- n64_rc_head: lifted code's view of the machine (n64_recomp_rt.h; n64_recomp.c checks every offset). Keep
     * these first and in this order. ---- */
    uint64_t now;                /* ticks */
    uint64_t stop_at;            /* the CPU loop runs while now < stop_at */
    uint64_t instructions;
    uint8_t *rdram;
    uint32_t *rdram_gen;         /* write counter per 4 KB page of RDRAM */
    uint32_t rdram_size;
    uint32_t mi_mode;
    uint32_t rc_lo, rc_len;      /* the physical code range of the lifted function running (rc_len 0: none) */
    uint8_t rc_hit;              /* a write landed in that range: the lifted function stops after the instruction */
    uint8_t rc_exit;             /* N64_RC_EXIT_*: why the lifted function returned */
    uint8_t rc_hint;             /* with N64_RC_EXIT_INTERP: how many instructions the interpreter runs */
    uint64_t rc_native;          /* instructions lifted code ran (packs built with -DN64_RC_COUNT) */
    n64_cpu cpu;
    /* ---- end of n64_rc_head ---- */
    struct n64_rc_state *rc;     /* n64_recomp.c: the attached table and the record of missed code (NULL: none) */
    uint64_t ev[EV_COUNT];
    uint64_t frames;
    bool frame_done;

    uint8_t *rom;
    uint32_t rom_size;
    n64_rom_info info;
    n64_options opts;
    bool pal;

    n64_rsp *rsp;
    n64_rdp *rdp;
    uint8_t *spmem;              /* n64_rsp_mem: DMEM + IMEM */
    uint8_t boot_x105;           /* N64_X105_*: the CIC-6105 IPL3's RSP semaphore wait (n64_pif.c) */
    n64_guest_fault fault;       /* the first guest fault exception and how many (n64_guest_fault_info) */
    uint64_t vi_origin_changes;  /* n64_guest_activity: VI_ORIGIN writes with a new value, */
    uint64_t si_dmas, pi_dmas, ai_buffers; /* SI / PI DMAs, audio buffers queued */
    uint64_t rsp_tick;           /* the RSP has run up to here */

    /* MI (mi_mode is in the head above) */
    uint32_t mi_intr, mi_mask;
    /* RI and the RDRAM registers (stubs IPL3 is happy with) */
    uint32_t ri[8];
    uint32_t rdram_regs[16];
    /* PI */
    uint32_t pi_dram, pi_cart, pi_rd_len, pi_wr_len, pi_status;
    uint32_t pi_dom[2][4];       /* LAT, PWD, PGS, RLS per domain */
    uint32_t pi_latch;           /* a CPU write to the cartridge bus, held by the PI */
    bool pi_latch_valid;
    uint64_t pi_latch_until;
    /* SI / PIF */
    uint32_t si_dram, si_status;
    bool si_read_pending;        /* the DMA in flight is PIF -> RDRAM */
    uint8_t pif_ram[64];
    uint8_t pif_rom_lock;
    n64_pad pad[4];
    uint8_t pak[4][N64_PAK_SIZE];
    bool pak_dirty[4];
    /* VI */
    uint32_t vi[16];
    uint32_t vi_line;            /* the half-line counter's line (0 .. lines per field - 1) */
    uint32_t vi_field;
    uint64_t vi_line_ticks;      /* length of a line */
    uint32_t vi_lines;           /* lines per field */
    struct n64_vi_state *vis;    /* n64_vi.c: the picture path's canvas and buffers (allocated at the first picture) */
    /* AI */
    uint32_t ai_dram, ai_len, ai_control, ai_dacrate, ai_bitrate;
    struct { uint32_t addr, len; } ai_fifo[2];
    int ai_count;                /* buffers queued (0..2); ai_fifo[0] plays */
    uint32_t ai_pos;             /* bytes of ai_fifo[0] played */
    uint64_t ai_tick;            /* audio made up to here */
    double ai_src_phase;         /* source samples (fraction) since the current one started */
    double ai_out_phase;         /* output samples (fraction) owed */
    int16_t ai_cur[2];           /* the sample the DAC holds */
    float ai_hist[2][AI_HIST];   /* the DAC's samples, L / R (the resampler's history, ai_nsrc of them so far) */
    uint32_t ai_nsrc;
    uint32_t ai_kernel_hz;       /* the DAC rate ai_kernel was built for (0: none) */
    float ai_kernel[(AI_PHASES + 1) * AI_TAPS]; /* windowed-sinc rows, one per output phase */
    int16_t audio[N64_AUDIO_RING * 2];
    uint32_t audio_rd, audio_wr; /* sample frames, free-running */
    /* Cartridge save */
    n64_save_kind save_kind;     /* NONE until known */
    bool save_fixed;             /* the kind came from n64_options or the ROM header (not detected) */
    /* Joybus RTC */
    uint8_t rtc_control[2];      /* block 0 */
    int64_t rtc_offset;          /* seconds added to the host clock (the game set the time) */
    int64_t rtc_fixed;           /* nonzero: the host clock reads this (tests) */
    uint8_t eeprom[N64_EEPROM_MAX];
    uint8_t sram[N64_SRAM_SIZE];
    uint8_t *flash;              /* N64_FLASH_SIZE */
    bool save_dirty;
    n64_flash_mode flash_mode;
    uint32_t flash_status;
    uint32_t flash_offset;       /* erase / write target */
    uint8_t flash_buf[128];
    /* IS-Viewer */
    uint8_t *isv;                /* 64 KB buffer at 0x13FF0000 (allocated at the first write) */
    void (*tty)(void *ctx, char c);
    void *tty_ctx;
    /* Save states (n64_state.c): the ROM's hash, computed at the first save or load (host side, not state). */
    uint64_t rom_hash;
    bool rom_hashed;
};

/* Instruction / access costs in ticks (n64_cpu.c; lifted code charges the same: n64_recomp_rt.h). */
enum {
    COST_CACHED = 3,        /* 1.5 cycles: an instruction fetched from cached memory */
    COST_UNCACHED_RAM = 20, /* uncached RDRAM fetch */
    COST_RSPMEM = 12,       /* IPL3 runs from DMEM */
    COST_ROM = 60,
    COST_DATA_UNCACHED = 8, /* uncached RDRAM data access */
    COST_MULT = 8,          /* extra over the base for MULT / MULTU (5 cycles) */
    COST_DMULT = 14,
    COST_DIV = 70,
    COST_DDIV = 134,
    COST_INTERRUPT = 4,     /* taking an interrupt */
};

/* A write landed in RDRAM [pa, pa + len): bumps the page counters (and flags the running lifted function's range). */
static inline void n64_rdram_wrote(n64_machine *m, uint32_t pa, uint32_t len)
{
    if (N64_UNLIKELY(!len || pa >= m->rdram_size))
        return;
    uint32_t last = pa + len - 1;
    if (last >= m->rdram_size)
        last = m->rdram_size - 1;
    for (uint32_t p = pa >> N64_PAGE_SHIFT; p <= last >> N64_PAGE_SHIFT; p++)
        m->rdram_gen[p]++;
    if (N64_UNLIKELY(m->rc_len) && pa < m->rc_lo + m->rc_len && last >= m->rc_lo)
        m->rc_hit = 1;
}

/* The same for an aligned CPU store (1..8 bytes inside RDRAM: one page). */
static inline void n64_rdram_wrote_aligned(n64_machine *m, uint32_t pa, uint32_t size)
{
    m->rdram_gen[pa >> N64_PAGE_SHIFT]++;
    if (N64_UNLIKELY(m->rc_len) && pa + size > m->rc_lo && pa < m->rc_lo + m->rc_len)
        m->rc_hit = 1;
}

/* ---- CPU (n64_cpu.c) ------------------------------------------------------------------------------------------- */
void n64_cpu_reset(n64_machine *m);
/* Runs instructions while now < stop_at. */
void n64_cpu_run(n64_machine *m);
/* STATUS or CAUSE changed: recompute the mode and the interrupt line. */
void n64_cpu_status_changed(n64_machine *m);
void n64_cpu_update_irq(n64_machine *m);
uint32_t n64_cpu_count(n64_machine *m);
void n64_cpu_set_count(n64_machine *m, uint32_t v);
void n64_cpu_schedule_compare(n64_machine *m);
/* Raises an exception (EPC from the instruction being run). */
void n64_cpu_exception(n64_machine *m, int code, int ce);
/* The coprocessor number (1, 2) of the instruction after the current one when it is a COP1 / COP2 instruction in
 * cached / uncached RDRAM, else 0: the Cause CE of an FPE raised by CTC1 (n64_fpu_write_fcsr). */
int n64_cpu_next_cop(n64_machine *m);
/* Address translation without side effects (debug): 0 and the physical address, or -1. */
int n64_cpu_probe(n64_machine *m, uint64_t va, uint32_t *pa);
/* One instruction at pc (the interpreter's loop body without its checks), and taking the pending interrupt: the
 * static recompilation's dispatcher (n64_recomp.c) runs the interpreter through these. */
void n64_cpu_step(n64_machine *m);
void n64_cpu_take_interrupt(n64_machine *m);
/* n64_cpu_step in the interpreter's inlined loop until pc == stop_pc, an interrupt is pending or now reaches stop_at
 * (the dispatcher's steps before the boot PC: IPL3, XBA-191). */
void n64_cpu_run_until(n64_machine *m, uint64_t stop_pc);
/* The instructions lifted code handed to the interpreter (n64_recomp.c): steps the one at pc; while *left > 1 and the
 * next pc follows in sequence, decrements *left and sets *chained, and goes on in the inlined loop as long as the
 * dispatcher would hand that next instruction straight back (time left, no interrupt pending, kernel mode, a KSEG0 /
 * KSEG1 RDRAM address); otherwise *chained is false. Returns the number of instructions run (XBA-191). */
uint32_t n64_cpu_step_handed(n64_machine *m, uint32_t *left, bool *chained);

/* ---- Static recompilation (n64_recomp.c) ------------------------------------------------------------------------ */
/* n64_cpu_run's loop while a table is attached (m->rc). */
void n64_recomp_run(n64_machine *m);
bool n64_recomp_active(const n64_machine *m);
void n64_recomp_free(n64_machine *m);
/* RDRAM was reset (memset): every cached content check is stale. */
void n64_recomp_reset(n64_machine *m);
/* Save states (n64_state.c): whether IPL3 is done as far as the attached table knows (-1: no table), and a state was
 * loaded (RDRAM pages bumped by the caller): the dispatch cache and hand-offs are forgotten, the boot flag is the
 * state's (-1: unknown, then from the frames the state had run). */
int n64_recomp_booted(n64_machine *m);
void n64_recomp_state_loaded(n64_machine *m, int booted);

/* ---- COP1 (n64_fpu.c) ------------------------------------------------------------------------------------------ */
/* Executes a COP1 instruction (opcode 0x11), CU1 already checked. */
void n64_fpu_cop1(n64_machine *m, uint32_t ins);
/* CTC1 to FCSR (may raise the FPE exception). */
void n64_fpu_write_fcsr(n64_machine *m, uint32_t v);
uint32_t n64_fpu_read_s(n64_cpu *c, int r);
void n64_fpu_write_s(n64_cpu *c, int r, uint32_t v);
uint64_t n64_fpu_read_d(n64_cpu *c, int r);
void n64_fpu_write_d(n64_cpu *c, int r, uint64_t v);

/* ---- Bus (n64_bus.c) ------------------------------------------------------------------------------------------- */
/* Physical accesses (addresses below 0x20000000; RDRAM handled inline by the CPU for speed). Return the extra ticks
 * the access costs through *cost when non-NULL. */
uint32_t n64_bus_read32(n64_machine *m, uint32_t pa);
void n64_bus_write32(n64_machine *m, uint32_t pa, uint32_t v, uint32_t mask);
uint64_t n64_bus_read64(n64_machine *m, uint32_t pa);
void n64_bus_write64(n64_machine *m, uint32_t pa, uint64_t v);
uint32_t n64_bus_cost(uint32_t pa); /* extra ticks of an uncached access there */
void n64_mi_raise(n64_machine *m, int irq);
void n64_mi_clear(n64_machine *m, int irq);
void n64_schedule(n64_machine *m, int ev, uint64_t at);

/* ---- Video / audio (n64_io.c) ---------------------------------------------------------------------------------- */
void n64_vi_reset(n64_machine *m);
uint32_t n64_vi_read(n64_machine *m, uint32_t reg);
void n64_vi_write(n64_machine *m, uint32_t reg, uint32_t v);
void n64_vi_event(n64_machine *m);
void n64_vi_free(n64_machine *m); /* n64_vi.c */
void n64_ai_reset(n64_machine *m);
uint32_t n64_ai_read(n64_machine *m, uint32_t reg);
void n64_ai_write(n64_machine *m, uint32_t reg, uint32_t v);
void n64_ai_advance(n64_machine *m); /* make the audio up to now */
void n64_ai_event(n64_machine *m);

/* ---- PIF / joybus / saves (n64_pif.c) -------------------------------------------------------------------------- */
void n64_pif_boot(n64_machine *m);
/* The CIC-6105 IPL3's RSP part (n64_pif.c header): n64_pif_x105_hold says whether the RSP is held this slice (it waits
 * on the semaphore IPL3 holds), n64_pif_x105_step follows the RSP after each slice, n64_pif_x105_release sees the CPU
 * write SP_SEMAPHORE. */
enum { N64_X105_OFF = 0, N64_X105_ARMED, N64_X105_RUNNING, N64_X105_RELEASED };
bool n64_pif_x105_hold(n64_machine *m);
void n64_pif_x105_step(n64_machine *m);
void n64_pif_x105_release(n64_machine *m);
void n64_pif_process(n64_machine *m); /* runs the joybus commands in PIF RAM */
void n64_pif_write_ram(n64_machine *m, uint32_t off, uint32_t v); /* CPU or SI write of a word */
uint32_t n64_cart2_read32(n64_machine *m, uint32_t pa);
void n64_cart2_write32(n64_machine *m, uint32_t pa, uint32_t v);
void n64_cart2_dma_read(n64_machine *m, uint32_t cart, uint8_t *dst, uint32_t len);  /* cart -> RDRAM */
void n64_cart2_dma_write(n64_machine *m, uint32_t cart, const uint8_t *src, uint32_t len); /* RDRAM -> cart */
void n64_save_set_kind(n64_machine *m, n64_save_kind kind);
void n64_pak_format(uint8_t *pak); /* a freshly formatted 32 KB controller pak */

#endif
