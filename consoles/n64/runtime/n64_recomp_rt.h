/* n64_recomp_rt.h -- the ABI between lifted N64 code (consoles/n64/tools n64_tools.recomp) and the core (XBA-170 /
 * XBA-171). Part of N64_RECOMP_ABI (n64_recomp.h): change this layout, these macros' meaning or the symbols below and
 * bump N64_RECOMP_ABI.
 *
 * Lifted code sees the machine through n64_rc_head, the first members of n64_machine (n64_recomp.c checks every
 * offset with static asserts): time, the scheduler's stop, the instruction count, RDRAM and its page write counters,
 * the MI repeat mode, the CPU registers, COP0, the FPRs and FCSR. Everything else goes through functions of the core,
 * which the host executable (n64_run, the Android shell's libmain.so) exports for the pack to bind to: n64_rc_read,
 * n64_rc_write, n64_rc_read_partial, n64_rc_write_masked, n64_rc_cop1, n64_rc_ctc1 (n64_cpu.c).
 *
 * Lifted code is tick-exact with the interpreter (n64_cpu.c): every instruction costs the same fetch ticks (by the
 * function's segment: cached KSEG0 / TLB-mapped, uncached KSEG1), the same multiply / divide / uncached-access /
 * bus ticks, at the same point, and is counted (COP0 Random derives from the count). The function is cut into blocks
 * (n64_tools lift.py « blocks and event checks »): at a block's start one check sees whether `now < stop_at` (and no
 * interrupt pending) would fail in front of any of its instructions, as the run loop checks before each; if so it
 * leaves (due now) or lets the interpreter run the rest of the block (rc_hint), which stops exactly where it would
 * have. After a call into the core (an access off cached RDRAM, COP1 arithmetic: it may charge ticks, raise an
 * interrupt, start a DMA, move the stop) it reloads the clock, the stop and the interrupt line and checks the rest of
 * the block again. So COUNT = COMPARE interrupts, VI / PI / SI / AI / SP / DP interrupts land in front of the same
 * instruction as in the interpreter, and a recompiled run is the interpreter's run, frame for frame.
 *
 * What lifted code leaves to the interpreter (it returns with rc_exit = N64_RC_EXIT_INTERP and the state in front of
 * that instruction, nothing of it done; the interpreter runs it, raising whatever it raises with the exact EPC, BD
 * and BadVAddr): every COP0 instruction but CACHE (MTC0 / MFC0 / DMxC0, TLBR / TLBWI / TLBWR / TLBP, ERET), SYSCALL,
 * BREAK, reserved encodings, COP2 / COP3, LL / LLD / SC / SCD, a branch in a delay slot, and any instruction whose
 * exception condition holds (integer overflow, a trap taken, an access that would raise an address error or a TLB
 * exception, COP1 unusable). COP1 arithmetic and CTC1 call n64_fpu.c (n64_rc_cop1 / n64_rc_ctc1: identical results,
 * rounding modes, flags, causes, FR views), which raises its exceptions itself (FPE, unimplemented operation): the
 * lifted code then returns with the exception's state as the core set it. CACHE and SYNC do nothing (no cache model:
 * the interpreter's semantics). Calls, returns, indirect jumps and jumps out of the function return to the
 * dispatcher (a bounded C stack); it picks the next function. A load or store at an address the lifter could not
 * tell (a pointer) that turns out to be off cached RDRAM (uncached loads, IO, TLB-mapped, a fault, a store into the
 * code running) is handed to the interpreter with the rest of its block (RC_HAND, rc_hint).
 *
 * Entry: the dispatcher calls a table function with pc at any instruction of its range, never in a delay slot, only
 * in kernel mode, with now < stop_at and no interrupt pending, rc_lo / rc_len the physical range of the entry's code,
 * rc_hit and rc_hint 0. The function resumes at that instruction when it starts a block; inside a block it returns at
 * once with N64_RC_EXIT_INTERP and rc_hint the instructions to the block's end (the interpreter runs them, as long as
 * execution stays in sequence); if pc is not one of its own it returns at once with N64_RC_EXIT_NOT_COVERED. */
#ifndef N64_RECOMP_RT_H
#define N64_RECOMP_RT_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "n64_recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The first members of n64_cpu (n64_internal.h). */
typedef struct n64_rc_cpu {
    uint64_t gpr[32];
    uint64_t hi, lo;
    uint64_t pc;      /* the next instruction (sign-extended) */
    uint64_t npc;     /* the one after it (a branch target when pc is a delay slot) */
    uint64_t cur;     /* interpreter scratch */
    bool ds;          /* interpreter scratch */
    bool next_ds;     /* pc is a branch delay slot */
    bool llbit;
    bool irq;         /* an interrupt is to be taken before pc */
    int ksu;
    bool addr64, ops64;
    uint64_t cop0[32];
    uint64_t fpr[32];
    uint32_t fcsr;
} n64_rc_cpu;

/* The first members of n64_machine (n64_internal.h). */
typedef struct n64_rc_head {
    uint64_t now;          /* ticks (half CPU cycles) */
    uint64_t stop_at;      /* the CPU runs while now < stop_at */
    uint64_t instructions; /* executed so far */
    uint8_t *rdram;        /* big-endian bytes */
    uint32_t *rdram_gen;   /* write counter per 4 KB page: lifted stores bump it like the core's */
    uint32_t rdram_size;
    uint32_t mi_mode;      /* bit 7: MI repeat mode (the next RDRAM store is repeated: through the core) */
    uint32_t rc_lo, rc_len; /* the running function's physical code range */
    uint8_t rc_hit;        /* the core wrote into that range: stop after the instruction */
    uint8_t rc_exit;       /* N64_RC_EXIT_*: why lifted code returned */
    uint8_t rc_hint;       /* with N64_RC_EXIT_INTERP: how many instructions the interpreter runs (the rest of a block) */
    uint64_t rc_native;    /* instructions run natively (counted when the pack is built with -DN64_RC_COUNT) */
    n64_rc_cpu cpu;
} n64_rc_head;

enum {
    N64_RC_EXIT_NORMAL = 0,      /* the state says where execution continues */
    N64_RC_EXIT_INTERP = 1,      /* the interpreter runs the instruction at pc, then dispatch goes on */
    N64_RC_EXIT_NOT_COVERED = 2, /* this function does not hold pc: nothing ran */
};

/* Ticks, as n64_cpu.c charges them (n64_recomp.c checks they agree with COST_* of n64_internal.h). */
#define N64_RC_CPI_CACHED 3     /* instruction fetch from KSEG0 or a TLB-mapped page */
#define N64_RC_CPI_UNCACHED 20  /* instruction fetch from KSEG1 */
#define N64_RC_COST_MULT 8
#define N64_RC_COST_DMULT 14
#define N64_RC_COST_DIV 70
#define N64_RC_COST_DDIV 134

/* The core's side (n64_cpu.c). The access functions return 0, or 1 with nothing done when the access would raise
 * (the lifted code then hands the instruction to the interpreter). They do the interpreter's translation, costs, bus
 * side effects and RDRAM write tracking. */
int n64_rc_read(n64_machine *m, uint64_t va, int size, uint64_t *out);            /* LB .. LD, LWC1, LDC1 */
int n64_rc_write(n64_machine *m, uint64_t va, int size, uint64_t v);              /* SB .. SD, SWC1, SDC1 */
int n64_rc_read_partial(n64_machine *m, uint64_t va, int size, uint64_t *out);    /* the word / doubleword of LWL .. LDR */
int n64_rc_write_masked(n64_machine *m, uint64_t va, uint64_t data, uint64_t mask, int size); /* SWL .. SDR */
/* COP1 arithmetic / conversions / compares (ins: the instruction word) and CTC1 (v: rt's value) for the instruction
 * at pc (ds: in a delay slot). Return 1 when they raised an exception (the core set pc / COP0: lifted code returns). */
int n64_rc_cop1(n64_machine *m, uint32_t ins, uint64_t pc, int ds);
int n64_rc_ctc1(n64_machine *m, uint32_t v, uint64_t pc, int ds);
/* CACHE (op: its rt field, va: the address): the caches' tags (n64_cpu.c cache_op); 1 with nothing done when it would
 * raise (a hit operation's TLB miss). Weak, a compatible addition (XBA-202): a pack also runs on a core without it,
 * where CACHE does nothing, as lifted code did before. */
int n64_rc_cache(n64_machine *m, int op, uint64_t va) __attribute__((weak));

/* ---- the shape of a lifted function (generated by n64_tools.recomp) ----
 *
 *   RC_FN f(n64_machine *m_) {
 *       RC_ENTER(N64_RC_CPI_CACHED);
 *       uint64_t r4 = h->cpu.gpr[4], ...;             (the registers the function uses)
 *       switch ((uint32_t)h->cpu.pc) { case 0x80001000u: goto L_80001000; ...        (the blocks' starts)
 *       default: { static const uint8_t mid_[] = {...}; RC_MID_TABLE(0x80001000u, mid_); RC_NOT_COVERED(); } }
 *   L_80001000: RC_SEG(0x80001000u, ticks, n);                                         (the block's event check)
 *       RC_I(); ... one instruction ...  RC_I(); ... the next ...
 *       ...
 *       RC_EXIT(end);
 *       RC_EPILOGUE_BEGIN();
 *       h->cpu.gpr[4] = r4; ...                       (the registers it writes)
 *       RC_EPILOGUE_END();
 *   }
 *
 * Guest registers live in locals, and so do the clock (`now`), the scheduler's stop (`stop`: 0 when the interrupt
 * line is up or the core wrote into this code) and the instruction count (`ic`), synced around every call into the
 * core. */
#define RC_SX(A) ((uint64_t)(int64_t)(int32_t)(uint32_t)(A))

#define RC_ENTER(CPI)                                                                                                \
    n64_rc_head *const h = (n64_rc_head *)(void *)m_;                                                                \
    enum { RC_CPI = (CPI) };                                                                                         \
    uint64_t now = h->now, stop = h->stop_at, ic = h->instructions;                                                  \
    uint8_t *const ram = h->rdram;                                                                                   \
    uint32_t *const gen = h->rdram_gen;                                                                              \
    const uint64_t rsz = h->rdram_size;                                                                              \
    const uint32_t plo = h->rc_lo, plen = h->rc_len;                                                                 \
    const uint64_t sr = h->cpu.cop0[12]; /* Status: constant while lifted code runs */                               \
    uint32_t mrep = h->mi_mode & 0x80u;                                                                              \
    uint64_t hi = h->cpu.hi, lo = h->cpu.lo, dz = 0;                                                                 \
    uint64_t epc = 0, enp = 0, jt = 0;                                                                               \
    int tk = 0;                                                                                                      \
    unsigned ehint = 0;                                                                                              \
    bool edl = false;                                                                                                \
    (void)ram; (void)gen; (void)rsz; (void)plo; (void)plen; (void)sr; (void)mrep; (void)dz

#define RC_M ((n64_machine *)(void *)h)

#ifdef N64_RC_COUNT
#define RC_COUNT() (h->rc_native++)
#define RC_COUNT_N(N) (h->rc_native += (N))
#define RC_UNCOUNT() (h->rc_native--)
#else
#define RC_COUNT() ((void)0)
#define RC_COUNT_N(N) ((void)(N))
#define RC_UNCOUNT() ((void)0)
#endif

/* Continue at the static address A (32-bit), or at the register value V (64-bit). */
#define RC_EXIT(A)                                                                                                   \
    do {                                                                                                             \
        epc = RC_SX(A);                                                                                              \
        goto x_plain;                                                                                                \
    } while (0)
#define RC_EXIT64(V)                                                                                                 \
    do {                                                                                                             \
        epc = (V);                                                                                                   \
        goto x_plain;                                                                                                \
    } while (0)

/* Start of an instruction: its fetch and its count (the event checks are per segment: RC_SEG). */
#define RC_I()                                                                                                       \
    do {                                                                                                             \
        now += RC_CPI;                                                                                               \
        ic++;                                                                                                        \
        RC_COUNT();                                                                                                  \
    } while (0)

/* The event check of a segment starting at A (n64_tools lift.py « blocks and event checks »): TICKS the static ticks
 * at most before its last instruction, N the instructions from A to the end of its block. Due now (or an interrupt
 * pending: stop is 0 then): leave for the dispatcher, the run loop's own check in front of A; due inside the segment:
 * the interpreter runs from A (N instructions: rc_hint), so it stops exactly where it would have. */
#define RC_SEG(A, TICKS, N)                                                                                          \
    do {                                                                                                             \
        if (__builtin_expect(now + (TICKS) >= stop, 0)) {                                                            \
            epc = RC_SX(A);                                                                                          \
            ehint = (N);                                                                                             \
            goto x_seg;                                                                                              \
        }                                                                                                            \
    } while (0)

/* An idle loop (a branch to itself over a NOP, n64_tools lift.py idle_loop), just taken: nothing but the clock and the
 * count move while it spins, so the turns that would all pass its segment check (RC_SEG(A, TICKS, 2) in front of it,
 * two instructions a turn) run at once; the next check then leaves where the spinning would have (the interpreter's
 * blocks fast-forward the same loop, n64_cpu.c). */
#define RC_IDLE(TICKS)                                                                                               \
    do {                                                                                                             \
        if (now + (TICKS) < stop) {                                                                                  \
            uint64_t turns_ = (stop - 1 - (TICKS) - now) / (2 * RC_CPI);                                             \
            now += turns_ * 2 * RC_CPI;                                                                              \
            ic += turns_ * 2;                                                                                        \
            RC_COUNT_N(turns_ * 2);                                                                                  \
        }                                                                                                            \
    } while (0)

/* Entered at pc, not a resume point (an interrupt returned inside a block): TAB[(pc - LO) / 4] is the number of
 * instructions to the end of pc's block (0: not one of this function's): the interpreter runs them, nothing done. */
#define RC_MID_TABLE(LO, TAB)                                                                                        \
    do {                                                                                                             \
        uint32_t o__ = (uint32_t)h->cpu.pc - (LO), i__ = o__ >> 2;                                                   \
        if (!(o__ & 3u) && i__ < sizeof(TAB) && (TAB)[i__]) {                                                        \
            h->rc_hint = (TAB)[i__];                                                                                 \
            h->rc_exit = N64_RC_EXIT_INTERP;                                                                         \
            return;                                                                                                  \
        }                                                                                                            \
    } while (0)

/* The interpreter runs the instruction at A and the N - 1 after it, the rest of its block (rc_hint): an access the
 * compact forms (RC_*_RAM) do not do, nothing of it done. */
#define RC_HAND(A, N)                                                                                                \
    do {                                                                                                             \
        epc = RC_SX(A);                                                                                              \
        ehint = (N);                                                                                                 \
        goto x_hand;                                                                                                 \
    } while (0)

/* The interpreter runs the instruction at A instead (its fetch and count are undone: the interpreter does them). */
#define RC_INTERP(A)                                                                                                 \
    do {                                                                                                             \
        epc = RC_SX(A);                                                                                              \
        goto x_interp;                                                                                               \
    } while (0)
#define RC_INTERP_SLOT(A, NPC)                                                                                       \
    do {                                                                                                             \
        epc = RC_SX(A);                                                                                              \
        enp = (NPC);                                                                                                 \
        goto x_interp_slot;                                                                                          \
    } while (0)
/* The core took an exception during the instruction (n64_rc_cop1 / n64_rc_ctc1): it set pc / npc / COP0. */
#define RC_RAISED()                                                                                                  \
    do {                                                                                                             \
        now = h->now; /* with what the core charged */                                                               \
        goto x_raised;                                                                                               \
    } while (0)

#define RC_NOT_COVERED()                                                                                             \
    do {                                                                                                             \
        h->rc_exit = N64_RC_EXIT_NOT_COVERED;                                                                        \
        return;                                                                                                      \
    } while (0)

/* Calls into the core: the clock and count go out; the clock, the stop (0 when an interrupt is pending or the core
 * wrote into this function's code) and the MI repeat mode come back. */
#define RC_SYNC_OUT() (h->now = now, h->instructions = ic)
#define RC_SYNC_IN()                                                                                                 \
    (now = h->now, stop = (h->cpu.irq || h->rc_hit) ? 0 : h->stop_at, mrep = h->mi_mode & 0x80u)

/* ---- memory ---- */
static inline uint64_t rc_be1(const uint8_t *p) { return p[0]; }
static inline uint64_t rc_be2(const uint8_t *p) { return (uint64_t)((uint32_t)p[0] << 8 | p[1]); }
static inline uint64_t rc_be4(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}
static inline uint64_t rc_be8(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return __builtin_bswap64(v);
}
static inline void rc_wbe1(uint8_t *p, uint64_t v) { p[0] = (uint8_t)v; }
static inline void rc_wbe2(uint8_t *p, uint64_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static inline void rc_wbe4(uint8_t *p, uint64_t v)
{
    uint32_t w = __builtin_bswap32((uint32_t)v);
    memcpy(p, &w, 4);
}
static inline void rc_wbe8(uint8_t *p, uint64_t v)
{
    uint64_t w = __builtin_bswap64(v);
    memcpy(p, &w, 8);
}

#define RC_KSEG0 0xFFFFFFFF80000000ull

/* ---- memory (n64_tools lift.py « memory accesses ») ----
 * Cached RDRAM (KSEG0, aligned, MI repeat mode off, not into this function's own code) inline with static ticks;
 * anything else through the core (n64_rc_read / write / ...: translation, costs, devices, write tracking), then CONT,
 * the lifter's re-check of the rest of the segment with the clock and stop the core left (an RC_SEG for the next
 * instruction: the core may have charged bus ticks, raised an interrupt (stop 0) or written into this code). FAULT
 * runs when the access would raise: nothing done, the interpreter raises it. */
#define RC_S8(v) ((uint64_t)(int64_t)(int8_t)(v))
#define RC_U8(v) ((uint64_t)(uint8_t)(v))
#define RC_S16(v) ((uint64_t)(int64_t)(int16_t)(v))
#define RC_U16(v) ((uint64_t)(uint16_t)(v))
#define RC_S32(v) ((uint64_t)(int64_t)(int32_t)(v))
#define RC_U32(v) ((uint64_t)(uint32_t)(v))
#define RC_U64(v) ((uint64_t)(v))
/* The RDRAM offset a store reaches through KSEG0 or KSEG1 (the interpreter charges nothing more for an uncached store,
 * unlike an uncached load: libdragon draws into uncached framebuffers), or a value >= rsz for any other address. */
#define RC_STORE_OFF(A)                                                                                              \
    ((((A) - RC_KSEG0) & ~0x20000000ull) < rsz ? (((A) - RC_KSEG0) & 0x1FFFFFFFull) : ~0ull)
/* Does a store of SZ bytes at RDRAM offset O touch this function's own code? */
#define RC_OWN(O, SZ) ((uint32_t)((O) - plo + (SZ) - 1) < plen + (SZ) - 1)

/* Aligned loads (LB LBU LH LHU LW LWU LD, the COP1 loads): SZ the size, OUT the destination, CONV the extension. */
#define RC_LOAD(OUT, A, SZ, CONV, FAULT, CONT)                                                                       \
    do {                                                                                                             \
        uint64_t qa_ = (A), qo_ = qa_ - RC_KSEG0;                                                                    \
        if (__builtin_expect(qo_ < rsz && !(qa_ & (SZ - 1)), 1)) {                                                   \
            (OUT) = CONV(rc_be##SZ(ram + qo_));                                                                      \
        } else {                                                                                                     \
            uint64_t qv_ = 0;                                                                                        \
            RC_SYNC_OUT();                                                                                           \
            int qe_ = n64_rc_read(RC_M, qa_, SZ, &qv_);                                                              \
            RC_SYNC_IN();                                                                                            \
            if (qe_) {                                                                                               \
                FAULT;                                                                                               \
            }                                                                                                        \
            (OUT) = CONV(qv_);                                                                                       \
            CONT;                                                                                                    \
        }                                                                                                            \
    } while (0)

/* Aligned stores (SB SH SW SD, the COP1 stores). */
#define RC_STORE(A, SZ, V, FAULT, CONT)                                                                              \
    do {                                                                                                             \
        uint64_t qa_ = (A), qo_ = RC_STORE_OFF(qa_);                                                                 \
        if (__builtin_expect(qo_ < rsz && !(qa_ & (SZ - 1)) && !mrep && !RC_OWN((uint32_t)qo_, SZ), 1)) {            \
            rc_wbe##SZ(ram + qo_, (V));                                                                              \
            gen[qo_ >> 12]++;                                                                                        \
        } else {                                                                                                     \
            RC_SYNC_OUT();                                                                                           \
            int qe_ = n64_rc_write(RC_M, qa_, SZ, (V));                                                              \
            RC_SYNC_IN();                                                                                            \
            if (qe_) {                                                                                               \
                FAULT;                                                                                               \
            }                                                                                                        \
            CONT;                                                                                                    \
        }                                                                                                            \
    } while (0)

/* The aligned word (SZ 4) / doubleword (8) an LWL / LWR / LDL / LDR at A reads. */
#define RC_LOADP(OUT, A, SZ, FAULT, CONT)                                                                            \
    do {                                                                                                             \
        uint64_t qa_ = (A), qo_ = (qa_ & ~(uint64_t)(SZ - 1)) - RC_KSEG0;                                            \
        if (__builtin_expect(qo_ < rsz, 1)) {                                                                        \
            (OUT) = rc_be##SZ(ram + qo_);                                                                            \
        } else {                                                                                                     \
            uint64_t qv_ = 0;                                                                                        \
            RC_SYNC_OUT();                                                                                           \
            int qe_ = n64_rc_read_partial(RC_M, qa_, SZ, &qv_);                                                      \
            RC_SYNC_IN();                                                                                            \
            if (qe_) {                                                                                               \
                FAULT;                                                                                               \
            }                                                                                                        \
            (OUT) = qv_;                                                                                             \
            CONT;                                                                                                    \
        }                                                                                                            \
    } while (0)

/* SWL / SWR / SDL / SDR: the bytes of MASK (left-aligned in 64 bits with DATA; SZ 4 uses the upper word). */
#define RC_STOREP(A, DATA, MASK, SZ, FAULT, CONT)                                                                    \
    do {                                                                                                             \
        uint64_t qa_ = (A), qo_ = RC_STORE_OFF(qa_ & ~(uint64_t)(SZ - 1)), qd_ = (DATA), qk_ = (MASK);               \
        if (__builtin_expect(qo_ < rsz && !RC_OWN((uint32_t)qo_, SZ), 1)) {                                          \
            for (int qi_ = 0; qi_ < (SZ); qi_++) {                                                                   \
                unsigned qs_ = 56 - 8 * (unsigned)qi_;                                                               \
                if ((qk_ >> qs_) & 0xFF)                                                                             \
                    ram[qo_ + (uint64_t)qi_] = (uint8_t)(qd_ >> qs_);                                                \
            }                                                                                                        \
            gen[qo_ >> 12]++;                                                                                        \
        } else {                                                                                                     \
            RC_SYNC_OUT();                                                                                           \
            int qe_ = n64_rc_write_masked(RC_M, qa_, qd_, qk_, SZ);                                                  \
            RC_SYNC_IN();                                                                                            \
            if (qe_) {                                                                                               \
                FAULT;                                                                                               \
            }                                                                                                        \
            CONT;                                                                                                    \
        }                                                                                                            \
    } while (0)

/* The compact forms for an access whose address the lifter does not know: cached RDRAM inline as above, anything else
 * (uncached, IO, TLB-mapped, a fault, MI repeat mode, a store into this function's own code) runs HAND, which gives
 * the instruction to the interpreter with the rest of its block (no call into the core: small code). */
#define RC_LOAD_RAM(OUT, A, SZ, CONV, HAND)                                                                          \
    do {                                                                                                             \
        uint64_t qa_ = (A), qo_ = qa_ - RC_KSEG0;                                                                    \
        if (__builtin_expect(qo_ < rsz && !(qa_ & (SZ - 1)), 1)) {                                                   \
            (OUT) = CONV(rc_be##SZ(ram + qo_));                                                                      \
        } else {                                                                                                     \
            HAND;                                                                                                    \
        }                                                                                                            \
    } while (0)
#define RC_STORE_RAM(A, SZ, V, HAND)                                                                                 \
    do {                                                                                                             \
        uint64_t qa_ = (A), qo_ = RC_STORE_OFF(qa_);                                                                 \
        if (__builtin_expect(qo_ < rsz && !(qa_ & (SZ - 1)) && !mrep && !RC_OWN((uint32_t)qo_, SZ), 1)) {            \
            rc_wbe##SZ(ram + qo_, (V));                                                                              \
            gen[qo_ >> 12]++;                                                                                        \
        } else {                                                                                                     \
            HAND;                                                                                                    \
        }                                                                                                            \
    } while (0)
#define RC_LOADP_RAM(OUT, A, SZ, HAND)                                                                               \
    do {                                                                                                             \
        uint64_t qo_ = ((A) & ~(uint64_t)(SZ - 1)) - RC_KSEG0;                                                       \
        if (__builtin_expect(qo_ < rsz, 1)) {                                                                        \
            (OUT) = rc_be##SZ(ram + qo_);                                                                            \
        } else {                                                                                                     \
            HAND;                                                                                                    \
        }                                                                                                            \
    } while (0)
#define RC_STOREP_RAM(A, DATA, MASK, SZ, HAND)                                                                       \
    do {                                                                                                             \
        uint64_t qo_ = RC_STORE_OFF((A) & ~(uint64_t)(SZ - 1)), qd_ = (DATA), qk_ = (MASK);                          \
        if (__builtin_expect(qo_ < rsz && !RC_OWN((uint32_t)qo_, SZ), 1)) {                                          \
            for (int qi_ = 0; qi_ < (SZ); qi_++) {                                                                   \
                unsigned qs_ = 56 - 8 * (unsigned)qi_;                                                               \
                if ((qk_ >> qs_) & 0xFF)                                                                             \
                    ram[qo_ + (uint64_t)qi_] = (uint8_t)(qd_ >> qs_);                                                \
            }                                                                                                        \
            gen[qo_ >> 12]++;                                                                                        \
        } else {                                                                                                     \
            HAND;                                                                                                    \
        }                                                                                                            \
    } while (0)

/* The register values the partial loads leave (CUR the register before, A the address, W the aligned word /
 * doubleword), and the data / mask of the partial stores: n64_cpu.c's arithmetic. */
static inline uint64_t rc_lwl(uint64_t cur, uint64_t w, uint64_t a)
{
    unsigned sh = 8 * (unsigned)(a & 3);
    uint32_t mask = 0xFFFFFFFFu << sh;
    uint32_t r = ((uint32_t)cur & ~mask) | ((uint32_t)w << sh);
    return RC_S32(r);
}
static inline uint64_t rc_lwr(uint64_t cur, uint64_t w, uint64_t a)
{
    unsigned sh = 8 * (unsigned)((a ^ 3) & 3);
    uint32_t mask = 0xFFFFFFFFu >> sh;
    uint32_t r = ((uint32_t)cur & ~mask) | ((uint32_t)w >> sh);
    return (a & 3) == 3 ? RC_S32(r) : (cur & 0xFFFFFFFF00000000ull) | r;
}
static inline uint64_t rc_ldl(uint64_t cur, uint64_t v, uint64_t a)
{
    unsigned sh = 8 * (unsigned)(a & 7);
    uint64_t mask = ~0ull << sh;
    return (cur & ~mask) | (v << sh);
}
static inline uint64_t rc_ldr(uint64_t cur, uint64_t v, uint64_t a)
{
    unsigned sh = 8 * (unsigned)((a ^ 7) & 7);
    uint64_t mask = ~0ull >> sh;
    return (cur & ~mask) | (v >> sh);
}
#define RC_SWL_D(r, a) ((uint64_t)((uint32_t)(r) >> (8 * ((a) & 3))) << 32)
#define RC_SWL_M(a) ((uint64_t)(0xFFFFFFFFu >> (8 * ((a) & 3))) << 32)
#define RC_SWR_D(r, a) ((uint64_t)((uint32_t)(r) << (8 * (((a) ^ 3) & 3))) << 32)
#define RC_SWR_M(a) ((uint64_t)(0xFFFFFFFFu << (8 * (((a) ^ 3) & 3))) << 32)
#define RC_SDL_D(r, a) ((uint64_t)(r) >> (8 * ((a) & 7)))
#define RC_SDL_M(a) (~0ull >> (8 * ((a) & 7)))
#define RC_SDR_D(r, a) ((uint64_t)(r) << (8 * (((a) ^ 7) & 7)))
#define RC_SDR_M(a) (~0ull << (8 * (((a) ^ 7) & 7)))

/* ---- integer unit helpers (n64_cpu.c's results) ---- */
static inline void rc_mult(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
    int64_t p = (int64_t)(int32_t)a * (int32_t)b;
    *lo = RC_S32((uint64_t)p);
    *hi = RC_S32((uint64_t)p >> 32);
}
static inline void rc_multu(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
    uint64_t p = (uint64_t)(uint32_t)a * (uint32_t)b;
    *lo = RC_S32(p);
    *hi = RC_S32(p >> 32);
}
static inline void rc_div(uint64_t x, uint64_t y, uint64_t *hi, uint64_t *lo)
{
    int32_t a = (int32_t)x, b = (int32_t)y;
    if (b == 0) {
        *lo = a < 0 ? 1 : ~0ull;
        *hi = RC_S32((uint32_t)a);
    } else if (a == INT32_MIN && b == -1) {
        *lo = RC_S32(0x80000000u);
        *hi = 0;
    } else {
        *lo = RC_S32((uint32_t)(a / b));
        *hi = RC_S32((uint32_t)(a % b));
    }
}
static inline void rc_divu(uint64_t x, uint64_t y, uint64_t *hi, uint64_t *lo)
{
    uint32_t a = (uint32_t)x, b = (uint32_t)y;
    if (b == 0) {
        *lo = ~0ull;
        *hi = RC_S32(a);
    } else {
        *lo = RC_S32(a / b);
        *hi = RC_S32(a % b);
    }
}
static inline void rc_mulu64(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
#ifdef __SIZEOF_INT128__
    unsigned __int128 p = (unsigned __int128)a * b;
    *hi = (uint64_t)(p >> 64);
    *lo = (uint64_t)p;
#else
    uint64_t al = (uint32_t)a, ah = a >> 32, bl = (uint32_t)b, bh = b >> 32;
    uint64_t ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    uint64_t mid = (ll >> 32) + (uint32_t)lh + (uint32_t)hl;
    *lo = (mid << 32) | (uint32_t)ll;
    *hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
#endif
}
static inline void rc_dmult(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
    rc_mulu64(a, b, hi, lo);
    if ((int64_t)a < 0)
        *hi -= b;
    if ((int64_t)b < 0)
        *hi -= a;
}
static inline void rc_ddiv(uint64_t x, uint64_t y, uint64_t *hi, uint64_t *lo)
{
    int64_t a = (int64_t)x, b = (int64_t)y;
    if (b == 0) {
        *lo = a < 0 ? 1 : ~0ull;
        *hi = (uint64_t)a;
    } else if (a == INT64_MIN && b == -1) {
        *lo = (uint64_t)a;
        *hi = 0;
    } else {
        *lo = (uint64_t)(a / b);
        *hi = (uint64_t)(a % b);
    }
}
static inline void rc_ddivu(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
    if (b == 0) {
        *lo = ~0ull;
        *hi = a;
    } else {
        *lo = a / b;
        *hi = a % b;
    }
}
/* ADD / ADDI / SUB (32-bit) and DADD / DADDI / DSUB: FAULT on overflow, else D = the result. */
#define RC_ADD32(D, A, B, FAULT)                                                                                     \
    do {                                                                                                             \
        int32_t qr_;                                                                                                  \
        if (__builtin_add_overflow((int32_t)(A), (int32_t)(B), &qr_)) {                                               \
            FAULT;                                                                                                   \
        }                                                                                                            \
        D = (uint64_t)(int64_t)qr_;                                                                                   \
    } while (0)
#define RC_SUB32(D, A, B, FAULT)                                                                                     \
    do {                                                                                                             \
        int32_t qr_;                                                                                                  \
        if (__builtin_sub_overflow((int32_t)(A), (int32_t)(B), &qr_)) {                                               \
            FAULT;                                                                                                   \
        }                                                                                                            \
        D = (uint64_t)(int64_t)qr_;                                                                                   \
    } while (0)
#define RC_ADD64(D, A, B, FAULT)                                                                                     \
    do {                                                                                                             \
        int64_t qr_;                                                                                                  \
        if (__builtin_add_overflow((int64_t)(A), (int64_t)(B), &qr_)) {                                               \
            FAULT;                                                                                                   \
        }                                                                                                            \
        D = (uint64_t)qr_;                                                                                            \
    } while (0)
#define RC_SUB64(D, A, B, FAULT)                                                                                     \
    do {                                                                                                             \
        int64_t qr_;                                                                                                  \
        if (__builtin_sub_overflow((int64_t)(A), (int64_t)(B), &qr_)) {                                               \
            FAULT;                                                                                                   \
        }                                                                                                            \
        D = (uint64_t)qr_;                                                                                            \
    } while (0)

/* ---- COP1 ---- */
#define RC_CU1() ((sr & 0x20000000u) != 0)
#define RC_FR() ((sr & 0x04000000u) != 0)
#define RC_FCSR_C 0x00800000u
/* The moves' register views (n64_fpu.c n64_fpu_read_s / write_s / read_d / write_d). */
static inline uint32_t rc_fpu_read_s(const n64_rc_head *h, uint64_t sr, int r)
{
    if (sr & 0x04000000u)
        return (uint32_t)h->cpu.fpr[r];
    return (r & 1) ? (uint32_t)(h->cpu.fpr[r & ~1] >> 32) : (uint32_t)h->cpu.fpr[r];
}
static inline void rc_fpu_write_s(n64_rc_head *h, uint64_t sr, int r, uint32_t v)
{
    if (sr & 0x04000000u)
        h->cpu.fpr[r] = (h->cpu.fpr[r] & 0xFFFFFFFF00000000ull) | v;
    else if (r & 1)
        h->cpu.fpr[r & ~1] = (h->cpu.fpr[r & ~1] & 0xFFFFFFFFull) | (uint64_t)v << 32;
    else
        h->cpu.fpr[r] = (h->cpu.fpr[r] & 0xFFFFFFFF00000000ull) | v;
}
static inline uint64_t rc_fpu_read_d(const n64_rc_head *h, uint64_t sr, int r)
{
    return h->cpu.fpr[(sr & 0x04000000u) ? r : (r & ~1)];
}
static inline void rc_fpu_write_d(n64_rc_head *h, uint64_t sr, int r, uint64_t v)
{
    h->cpu.fpr[(sr & 0x04000000u) ? r : (r & ~1)] = v;
}
/* CFC1: FCSR (31), the implementation register (0), else 0; sign-extended. */
static inline uint64_t rc_cfc1(const n64_rc_head *h, int r)
{
    uint32_t v = r == 31 ? h->cpu.fcsr : r == 0 ? 0x00000A00u : 0;
    return RC_S32(v);
}
/* CACHE OP at A (n64_rc_cache). */
#define RC_CACHE(OP, A, FAULT)                                                                                       \
    do {                                                                                                             \
        if (n64_rc_cache && n64_rc_cache(RC_M, (OP), (A))) {                                                         \
            FAULT;                                                                                                   \
        }                                                                                                            \
    } while (0)
/* COP1 operations through n64_fpu.c (PC the instruction, DS 1 in a delay slot); they charge their latency (DIV,
 * SQRT, MUL) to the clock. */
#define RC_COP1(INS, PC, DS, CONT)                                                                                   \
    do {                                                                                                             \
        RC_SYNC_OUT();                                                                                               \
        if (n64_rc_cop1(RC_M, (INS), RC_SX(PC), (DS)))                                                               \
            RC_RAISED();                                                                                             \
        now = h->now;                                                                                                \
        CONT;                                                                                                        \
    } while (0)
#define RC_CTC1(V, PC, DS, CONT)                                                                                     \
    do {                                                                                                             \
        RC_SYNC_OUT();                                                                                               \
        if (n64_rc_ctc1(RC_M, (uint32_t)(V), RC_SX(PC), (DS)))                                                       \
            RC_RAISED();                                                                                             \
        CONT;                                                                                                        \
    } while (0)

/* The common exits, then (generated) the stores of the registers the function writes, then RC_EPILOGUE_END. */
#define RC_EPILOGUE_BEGIN()                                                                                          \
    x_seg: __attribute__((unused));                                                                                  \
    if (now < stop) {                                                                                                \
        h->rc_exit = N64_RC_EXIT_INTERP;                                                                             \
        h->rc_hint = (uint8_t)ehint;                                                                                 \
    }                                                                                                                \
    goto x_plain;                                                                                                    \
    x_hand: __attribute__((unused));                                                                                 \
    h->rc_hint = (uint8_t)ehint;                                                                                     \
    goto x_interp;                                                                                                   \
    x_interp_slot: __attribute__((unused));                                                                          \
    now -= RC_CPI;                                                                                                   \
    ic--;                                                                                                            \
    RC_UNCOUNT();                                                                                                    \
    h->rc_exit = N64_RC_EXIT_INTERP;                                                                                 \
    edl = true;                                                                                                      \
    goto x_exit;                                                                                                     \
    x_interp: __attribute__((unused));                                                                               \
    now -= RC_CPI;                                                                                                   \
    ic--;                                                                                                            \
    RC_UNCOUNT();                                                                                                    \
    h->rc_exit = N64_RC_EXIT_INTERP;                                                                                 \
    x_plain: __attribute__((unused));                                                                                \
    enp = epc + 4u;                                                                                                  \
    edl = false;                                                                                                     \
    x_exit: __attribute__((unused));                                                                                 \
    h->cpu.pc = epc;                                                                                                 \
    h->cpu.npc = enp;                                                                                                \
    h->cpu.next_ds = edl;                                                                                            \
    x_raised: __attribute__((unused))
#define RC_EPILOGUE_END()                                                                                            \
    h->cpu.hi = hi;                                                                                                  \
    h->cpu.lo = lo;                                                                                                  \
    h->now = now;                                                                                                    \
    h->instructions = ic;                                                                                            \
    (void)tk;                                                                                                        \
    (void)jt;                                                                                                        \
    (void)ehint

/* Marks the functions of a pack (hidden: only the table function is exported). */
#define RC_FN __attribute__((visibility("hidden"))) void
#define RC_EXPORT __attribute__((visibility("default")))

#ifdef __cplusplus
}
#endif

#endif
