/* VR4300 interpreter (N-3, XBA-171): MIPS III integer unit, COP0 (TLB, exceptions, COUNT / COMPARE), the
 * address spaces (32-bit and 64-bit kernel / supervisor / user), and the dispatch to COP1 (n64_fpu.c).
 *
 * Semantics follow the VR4300 user's manual; where it is silent, the behaviour the n64-systemtest suite pins (as ares
 * and N64Recomp implement it: 32-bit results sign-extended, SRA / SRAV shifting the whole 64-bit register, LWL / LWR
 * sign-extending the merged word, DIV / DDIV by zero giving LO = +1 / -1 and HI = the dividend). The caches keep their
 * tags only (cache_op): every access sees memory. Timing: a fixed cost per instruction by fetch region (1.5 cycles
 * from cached RDRAM, more uncached, RSP memory, ROM) plus a cost per uncached data access and the multiply / divide
 * latencies. step() is the reference interpreter; n64_cpu_run runs RDRAM code as predecoded blocks (« Predecoded
 * blocks » below, XBA-210), instruction for instruction the same machine. */
#include "n64_internal.h"

#include <stdlib.h>
#include <string.h>

#define RS(i) (((i) >> 21) & 31)
#define RT(i) (((i) >> 16) & 31)
#define RD(i) (((i) >> 11) & 31)
#define SA(i) (((i) >> 6) & 31)
#define IMM(i) ((int64_t)(int16_t)(i))

static inline uint64_t sx32(uint64_t v) { return (uint64_t)(int64_t)(int32_t)(uint32_t)v; }

/* Tick costs: COST_* in n64_internal.h (lifted code charges the same). */

enum { ACC_LOAD = 0, ACC_STORE = 1, ACC_FETCH = 2 };

/* ---- COP0 helpers ---------------------------------------------------------------------------------------------- */

void n64_cpu_status_changed(n64_machine *m)
{
    n64_cpu *c = &m->cpu;
    uint32_t sr = (uint32_t)c->cop0[C0_STATUS];
    if (sr & (SR_EXL | SR_ERL))
        c->ksu = 0;
    else
        c->ksu = (int)((sr >> 3) & 3);
    if (c->ksu == 3)
        c->ksu = 2; /* undefined: treated as user */
    switch (c->ksu) {
    case 0:
        c->addr64 = (sr & SR_KX) != 0;
        c->ops64 = true;
        break;
    case 1:
        c->addr64 = (sr & SR_SX) != 0;
        c->ops64 = c->addr64;
        break;
    default:
        c->addr64 = (sr & SR_UX) != 0;
        c->ops64 = c->addr64;
        break;
    }
    /* Status.RE (reverse endian) in user mode: the aligned accesses reach their address XOR 8 - size (bytes 7,
     * halfwords 6, words and instructions 4), the partial ones (LWL ... SDR) theirs XOR 7; BadVAddr keeps the
     * address as given (n64-systemtest "RE user mode ..."). */
    c->re = c->ksu == 2 && (sr & SR_RE);
    n64_cpu_update_irq(m);
}

void n64_cpu_update_irq(n64_machine *m)
{
    n64_cpu *c = &m->cpu;
    uint32_t cause = (uint32_t)c->cop0[C0_CAUSE];
    if (m->mi_intr & m->mi_mask)
        cause |= CAUSE_IP2;
    else
        cause &= ~CAUSE_IP2;
    c->cop0[C0_CAUSE] = cause;
    uint32_t sr = (uint32_t)c->cop0[C0_STATUS];
    c->irq = (sr & SR_IE) && !(sr & (SR_EXL | SR_ERL)) && (sr & cause & 0xFF00u);
}

uint32_t n64_cpu_count(n64_machine *m)
{
    return m->cpu.count_base + (uint32_t)((m->now - m->cpu.count_tick) >> 2);
}

void n64_cpu_schedule_compare(n64_machine *m)
{
    n64_cpu *c = &m->cpu;
    uint64_t elapsed = (m->now - c->count_tick) >> 2;
    uint32_t now_count = c->count_base + (uint32_t)elapsed;
    uint32_t delta = (uint32_t)c->cop0[C0_COMPARE] - now_count;
    uint64_t d = delta ? delta : 0x100000000ull;
    n64_schedule(m, EV_COMPARE, c->count_tick + ((elapsed + d) << 2));
}

void n64_cpu_set_count(n64_machine *m, uint32_t v)
{
    m->cpu.count_base = v;
    m->cpu.count_tick = m->now;
    n64_cpu_schedule_compare(m);
}

/* RANDOM counts down from 31 by one per instruction and wraps to 31 after reaching WIRED (6-bit counter: with
 * WIRED > 31 it runs through 0 and 63 down to WIRED). Derived from the instruction count rather than kept per
 * instruction. A WIRED write sets it to 31 two instructions later; an MFC0 right after the MTC0 still sees the old
 * sequence (n64-systemtest "Random (decrement)", timing group, and "Random (read early)", cop0hazard group). */
static uint32_t random_reg(n64_machine *m)
{
    const n64_cpu *c = &m->cpu;
    uint32_t wired = (uint32_t)c->cop0[C0_WIRED] & 63;
    uint64_t base = c->random_base;
    if (m->instructions < base) {
        wired = c->wired_prev;
        base = c->random_base_prev;
    }
    uint32_t span = wired <= 31 ? 32 - wired : 32 + 64 - wired;
    uint32_t i = (uint32_t)((m->instructions - base) % span);
    return i <= 31 ? 31 - i : 63 - (i - 32);
}

void n64_cpu_reset(n64_machine *m)
{
    n64_cpu *c = &m->cpu;
    memset(c, 0, sizeof *c);
    c->cop0[C0_STATUS] = 0x34000000u; /* CU1 CU0 FR, as the PIF leaves it */
    c->cop0[C0_CONFIG] = 0x7006E463u;
    c->cop0[C0_PRID] = 0x00000B22u;
    c->cop0[C0_RANDOM] = 31;
    c->cop0[C0_EPC] = ~0ull;
    c->cop0[C0_ERROREPC] = ~0ull;
    c->cop0[C0_CONTEXT] = 0;
    c->cop0[C0_BADVADDR] = ~0ull;
    for (int i = 0; i < 32; i++) {
        c->tlb[i].vpn_mask = ~0ull;
        c->tlb[i].vpn = 1; /* matches nothing */
    }
    c->pc = 0xFFFFFFFFBFC00000ull;
    c->npc = c->pc + 4;
    c->count_tick = m->now;
    n64_cpu_status_changed(m);
}

/* ---- Exceptions ------------------------------------------------------------------------------------------------ */

static void exception_vec(n64_machine *m, int code, int ce, uint32_t offset)
{
    n64_cpu *c = &m->cpu;
    uint32_t sr = (uint32_t)c->cop0[C0_STATUS];
    uint32_t cause = (uint32_t)c->cop0[C0_CAUSE];
    if (!(sr & SR_EXL)) {
        if (c->ds) {
            c->cop0[C0_EPC] = c->cur - 4;
            cause |= CAUSE_BD;
        } else {
            c->cop0[C0_EPC] = c->cur;
            cause &= ~CAUSE_BD;
        }
    } else {
        offset = 0x180; /* refills while EXL go to the general vector */
    }
    cause = (cause & ~0x3000007Cu) | ((uint32_t)code << 2) | ((uint32_t)ce << 28);
    c->cop0[C0_CAUSE] = cause;
    c->cop0[C0_STATUS] = sr | SR_EXL;
    uint64_t base = (sr & SR_BEV) ? 0xFFFFFFFFBFC00200ull : 0xFFFFFFFF80000000ull;
    c->pc = base + offset;
    if (N64_UNLIKELY((code >= EXC_MOD && code <= EXC_DBE) || code == EXC_RI || (code == EXC_CPU && ce != 1))) {
        /* A fault a healthy game does not take (n64_guest_fault_info): recorded for the runtime's log. */
        if (m->fault.count++ == 0) {
            m->fault.code = (uint32_t)code;
            m->fault.cause = cause;
            m->fault.status = sr | SR_EXL;
            m->fault.pc = c->pc;
            m->fault.epc = c->cop0[C0_EPC];
            m->fault.badvaddr = c->cop0[C0_BADVADDR];
            m->fault.frame = m->frames;
        }
    }
    c->npc = c->pc + 4;
    c->next_ds = false;
    n64_cpu_status_changed(m);
}

void n64_cpu_exception(n64_machine *m, int code, int ce)
{
    exception_vec(m, code, ce, 0x180);
}

int n64_cpu_next_cop(n64_machine *m)
{
    const n64_cpu *c = &m->cpu;
    if (c->ds)
        return 0;
    uint64_t pc = c->cur + 4;
    uint64_t off = (pc - 0xFFFFFFFF80000000ull) & ~0x20000000ull; /* KSEG0 or KSEG1 */
    if (pc < 0xFFFFFFFF80000000ull || pc >= 0xFFFFFFFFC0000000ull || off >= (uint64_t)m->rdram_size)
        return 0;
    uint32_t op = n64_be32(m->rdram + off) >> 26;
    return op == 0x11 ? 1 : op == 0x12 ? 2 : 0;
}

/* BadVAddr, Context / XContext BadVPN2 and EntryHi VPN2 / R: set by TLB and address error exceptions alike. */
static void bad_vaddr(n64_cpu *c, uint64_t va)
{
    c->cop0[C0_BADVADDR] = va;
    c->cop0[C0_CONTEXT] = (c->cop0[C0_CONTEXT] & ~0x7FFFFFull) | (((va >> 13) & 0x7FFFF) << 4);
    c->cop0[C0_XCONTEXT] = (c->cop0[C0_XCONTEXT] & ~0x1FFFFFFFFull) | (((va >> 62) & 3) << 31) |
                           (((va >> 13) & 0x7FFFFFFull) << 4);
    c->cop0[C0_ENTRYHI] = (va & 0xC00000FFFFFFE000ull) | (c->cop0[C0_ENTRYHI] & 0xFF);
}

static void address_error(n64_machine *m, uint64_t va, int acc)
{
    bad_vaddr(&m->cpu, va);
    n64_cpu_exception(m, acc == ACC_STORE ? EXC_ADES : EXC_ADEL, 0);
}

static void tlb_exception(n64_machine *m, uint64_t va, int acc, int code, bool refill)
{
    n64_cpu *c = &m->cpu;
    bad_vaddr(c, va);
    if (code < 0)
        code = acc == ACC_STORE ? EXC_TLBS : EXC_TLBL;
    exception_vec(m, code, 0, refill ? (c->addr64 ? 0x080 : 0x000) : 0x180);
}

/* ---- TLB ------------------------------------------------------------------------------------------------------- */

static void tlb_derive(n64_tlb_entry *e)
{
    e->vpn_mask = 0xC00000FFFFFFE000ull & ~(uint64_t)e->mask;
    e->vpn = e->hi & e->vpn_mask;
    e->global = (e->lo[0] & e->lo[1] & 1) != 0;
}

/* Looks va up: 1 and *pa, 0 no match (refill), -1 invalid, -2 modified (store to a clean page). */
static int tlb_lookup(n64_cpu *c, uint64_t va, int acc, uint32_t *pa)
{
    uint8_t asid = (uint8_t)c->cop0[C0_ENTRYHI];
    for (int i = 0; i < 32; i++) {
        const n64_tlb_entry *e = &c->tlb[i];
        if ((va & e->vpn_mask) != e->vpn)
            continue;
        if (!e->global && (uint8_t)e->hi != asid)
            continue;
        uint32_t sel = (e->mask + 0x2000u) >> 1; /* the bit picking the odd page */
        uint32_t lo = e->lo[(va & sel) ? 1 : 0];
        if (!(lo & 2))
            return -1;
        if (acc == ACC_STORE && !(lo & 4))
            return -2;
        *pa = (uint32_t)((((uint64_t)(lo >> 6) & 0xFFFFF) << 12) + (va & (sel - 1)));
        return 1;
    }
    return 0;
}

/* Full translation; raises the exception and returns false on failure. */
static bool vtop_slow(n64_machine *m, uint64_t va, int acc, uint32_t *pa, bool raise)
{
    n64_cpu *c = &m->cpu;
    int mode = c->ksu;
    bool erl = (c->cop0[C0_STATUS] & SR_ERL) != 0;
    bool mapped = false;
    bool sext = (uint64_t)(int64_t)(int32_t)va == va;
    if (!c->addr64 && !sext)
        goto adel; /* 32-bit mode: the address must be a sign-extended 32-bit value */
    if (sext) {
        uint32_t a = (uint32_t)va;
        if (a < 0x80000000u) {
            if (erl) { /* ERL: kuseg is unmapped and uncached */
                *pa = a;
                return true;
            }
            mapped = true;
        } else if (mode == 2) {
            goto adel;
        } else if (a < 0xA0000000u) {
            if (mode != 0)
                goto adel;
            *pa = a - 0x80000000u;
            return true;
        } else if (a < 0xC0000000u) {
            if (mode != 0)
                goto adel;
            *pa = a - 0xA0000000u;
            return true;
        } else if (a < 0xE0000000u) {
            mapped = true; /* ksseg / csseg: supervisor or kernel */
        } else {
            if (mode != 0)
                goto adel;
            mapped = true;
        }
    } else {
        unsigned region = (unsigned)(va >> 62);
        switch (region) {
        case 0: /* xkuseg */
            if (va >= (1ull << 40))
                goto adel;
            if (erl && va < 0x80000000ull) {
                *pa = (uint32_t)va;
                return true;
            }
            mapped = true;
            break;
        case 1: /* xksseg */
            if (mode == 2 || (va & 0x3FFFFFFFFFFFFFFFull) >= (1ull << 40))
                goto adel;
            mapped = true;
            break;
        case 2: /* xkphys */
            if (mode != 0 || (va & 0x07FFFFFF00000000ull))
                goto adel;
            *pa = (uint32_t)va;
            return true;
        default: /* xkseg (ckseg* are the sign-extended 32-bit forms above) */
            if (mode != 0 || (va & 0x3FFFFFFFFFFFFFFFull) >= (1ull << 40) - 0x80000000ull)
                goto adel;
            mapped = true;
            break;
        }
    }
    if (mapped) {
        int r = tlb_lookup(c, va, acc, pa);
        if (r == 1)
            return true;
        if (raise) {
            if (r == 0)
                tlb_exception(m, va, acc, -1, true);
            else if (r == -1)
                tlb_exception(m, va, acc, -1, false);
            else
                tlb_exception(m, va, acc, EXC_MOD, false);
        }
        return false;
    }
adel:
    if (raise)
        address_error(m, va, acc);
    return false;
}

/* raise: false for lifted code's accesses (n64_rc_*), which must leave everything untouched when the access faults:
 * the interpreter then runs the instruction again and raises. */
static inline bool vtop(n64_machine *m, uint64_t va, int acc, uint32_t *pa, bool raise)
{
    if (N64_LIKELY(va - 0xFFFFFFFF80000000ull < 0x40000000ull) && m->cpu.ksu == 0) {
        *pa = (uint32_t)va & 0x1FFFFFFFu;
        return true;
    }
    return vtop_slow(m, va, acc, pa, raise);
}

int n64_cpu_probe(n64_machine *m, uint64_t va, uint32_t *pa)
{
    if (va - 0xFFFFFFFF80000000ull < 0x40000000ull) {
        *pa = (uint32_t)va & 0x1FFFFFFFu;
        return 0;
    }
    return vtop_slow(m, va, ACC_LOAD, pa, false) ? 0 : -1;
}

/* ---- Memory access --------------------------------------------------------------------------------------------- */

static inline uint64_t be64(const uint8_t *p) { return (uint64_t)n64_be32(p) << 32 | n64_be32(p + 4); }
static inline void wbe64(uint8_t *p, uint64_t v)
{
    n64_wbe32(p, (uint32_t)(v >> 32));
    n64_wbe32(p + 4, (uint32_t)v);
}

static inline bool kseg1(uint64_t va) { return va - 0xFFFFFFFFA0000000ull < 0x20000000ull; }

/* Reads size (1, 2, 4, 8) bytes at va (aligned). False when an exception was raised. */
static N64_HOT_INLINE bool rd_x(n64_machine *m, uint64_t va, int size, uint64_t *out, bool raise)
{
    if (N64_UNLIKELY(va & (uint64_t)(size - 1))) {
        if (raise)
            address_error(m, va, ACC_LOAD);
        return false;
    }
    uint32_t pa;
    if (!vtop(m, va, ACC_LOAD, &pa, raise))
        return false;
    if (N64_UNLIKELY(m->cpu.re))
        pa ^= (uint32_t)(8 - size) & 7;
    if (N64_LIKELY(pa < m->rdram_size)) {
        const uint8_t *p = m->rdram + pa;
        switch (size) {
        case 1: *out = p[0]; break;
        case 2: *out = n64_be16(p); break;
        case 4: *out = n64_be32(p); break;
        default: *out = be64(p); break;
        }
        if (kseg1(va))
            m->now += COST_DATA_UNCACHED;
        return true;
    }
    m->now += n64_bus_cost(pa);
    if (size == 8) {
        *out = n64_bus_read64(m, pa);
        return true;
    }
    if (size < 4 && pa >= 0x05000000u && pa < 0x1FC00000u) {
        /* The cartridge bus answers 8 / 16-bit reads from the halfword at the next word boundary when the address is
         * in a word's second half (n64-systemtest: "every other 16-bit word can not be reached"). */
        if (pa & 2)
            pa = (pa & ~3u) + 4 + (pa & 1);
    }
    uint32_t w = n64_bus_read32(m, pa & ~3u);
    switch (size) {
    case 1: *out = (w >> (8 * (3 - (pa & 3)))) & 0xFF; break;
    case 2: *out = (w >> (8 * (2 - (pa & 2)))) & 0xFFFF; break;
    default: *out = w; break;
    }
    return true;
}

static N64_HOT_INLINE bool rd(n64_machine *m, uint64_t va, int size, uint64_t *out)
{
    return rd_x(m, va, size, out, true);
}

/* MI repeat mode (MI_MODE bit 7, set with the length): the next RDRAM write repeats the bus word (32 bits, 64 for SD) over the
 * length (less the start's misalignment to 8), wrapping in the 2 KB page, then the mode ends. */
static void mi_repeat_write(n64_machine *m, uint32_t pa, int size, uint64_t v)
{
    uint32_t sh = size == 1 ? 8 * (3 - (pa & 3)) : size == 2 ? 8 * (2 - (pa & 2)) : 0;
    uint32_t w = size == 8 ? (uint32_t)(v >> 32) : (uint32_t)v << sh;
    uint32_t len = (m->mi_mode & 0x7F) + 1;
    uint32_t n = len > (pa & 7) ? len - (pa & 7) : 0;
    for (uint32_t k = 0; k < n; k++) { /* SD drives all 64 bits of the bus */
        uint32_t a = (pa & ~0x7FFu) | ((pa + k) & 0x7FF);
        if (a < m->rdram_size)
            m->rdram[a] = size == 8 ? (uint8_t)(v >> (8 * (7 - (a & 7)))) : (uint8_t)(w >> (8 * (3 - (a & 3))));
    }
    n64_rdram_wrote(m, pa & ~0x7FFu, 0x800);
    m->mi_mode &= ~0x80u;
}

static N64_HOT_INLINE bool wr_x(n64_machine *m, uint64_t va, int size, uint64_t v, bool raise)
{
    if (N64_UNLIKELY(va & (uint64_t)(size - 1))) {
        if (raise)
            address_error(m, va, ACC_STORE);
        return false;
    }
    uint32_t pa;
    if (!vtop(m, va, ACC_STORE, &pa, raise))
        return false;
    if (N64_UNLIKELY(m->cpu.re))
        pa ^= (uint32_t)(8 - size) & 7;
    if (N64_LIKELY(pa < m->rdram_size)) {
        if (N64_UNLIKELY(m->mi_mode & 0x80)) {
            mi_repeat_write(m, pa, size, v);
            return true;
        }
        uint8_t *p = m->rdram + pa;
        switch (size) {
        case 1: p[0] = (uint8_t)v; break;
        case 2: n64_wbe16(p, (uint16_t)v); break;
        case 4: n64_wbe32(p, (uint32_t)v); break;
        default: wbe64(p, v); break;
        }
        n64_rdram_wrote_aligned(m, pa, (uint32_t)size);
        return true;
    }
    /* Outside RDRAM the bus is 32 bits wide: SB / SH write the whole word with the register shifted into their byte
     * lanes (SB at offset 1 of 0x12345678 writes 0x56780000), SD only its upper word. */
    m->now += n64_bus_cost(pa);
    uint32_t sh = size == 1 ? 8 * (3 - (pa & 3)) : size == 2 ? 8 * (2 - (pa & 2)) : 0;
    if (size == 8)
        n64_bus_write32(m, pa, (uint32_t)(v >> 32), 0xFFFFFFFFu);
    else
        n64_bus_write32(m, pa & ~3u, (uint32_t)v << sh, 0xFFFFFFFFu);
    return true;
}

static N64_HOT_INLINE bool wr(n64_machine *m, uint64_t va, int size, uint64_t v) { return wr_x(m, va, size, v, true); }

/* The aligned word / doubleword a partial load (LWL / LWR / LDL / LDR) reads, translated from the unaligned address. */
static inline bool rd_partial_x(n64_machine *m, uint64_t va, int size, uint64_t *out, bool raise)
{
    uint32_t pa;
    if (!vtop(m, va, ACC_LOAD, &pa, raise))
        return false;
    pa &= ~(uint32_t)(size - 1);
    if (pa < m->rdram_size) {
        *out = size == 8 ? be64(m->rdram + pa) : n64_be32(m->rdram + pa);
        return true;
    }
    m->now += n64_bus_cost(pa);
    *out = size == 8 ? n64_bus_read64(m, pa) : n64_bus_read32(m, pa);
    return true;
}

static bool rd_partial(n64_machine *m, uint64_t va, int size, uint64_t *out)
{
    return rd_partial_x(m, va, size, out, true);
}

/* A partial store (SWL / SWR / SDL / SDR): translated as a store, then only the bytes in mask are written (data and
 * mask left-aligned in 64 bits; size 4 uses the upper word). */
static inline bool store_masked_x(n64_machine *m, uint64_t va, uint64_t data, uint64_t mask, int size, bool raise)
{
    uint32_t pa;
    if (!vtop(m, va, ACC_STORE, &pa, raise)) /* the unaligned address: BadVAddr reports it */
        return false;
    pa &= ~(uint32_t)(size - 1);
    if (pa < m->rdram_size) {
        uint8_t *p = m->rdram + pa;
        for (int i = 0; i < size; i++) {
            unsigned sh = 56 - 8 * (unsigned)i;
            if ((mask >> sh) & 0xFF)
                p[i] = (uint8_t)(data >> sh);
        }
        n64_rdram_wrote_aligned(m, pa, (uint32_t)size);
        return true;
    }
    m->now += n64_bus_cost(pa);
    n64_bus_write32(m, pa, (uint32_t)(data >> 32), (uint32_t)(mask >> 32));
    if (size == 8)
        n64_bus_write32(m, pa + 4, (uint32_t)data, (uint32_t)mask);
    return true;
}

static void store_masked(n64_machine *m, uint64_t va, uint64_t data, uint64_t mask, int size)
{
    store_masked_x(m, va, data, mask, size, true);
}

/* Instruction fetch, slow path (anything but cached RDRAM). */
static bool fetch_slow(n64_machine *m, uint64_t pc, uint32_t *ins)
{
    if (pc & 3) {
        address_error(m, pc, ACC_FETCH);
        return false;
    }
    uint32_t pa;
    if (!vtop_slow(m, pc, ACC_FETCH, &pa, true))
        return false;
    if (m->cpu.re)
        pa ^= 4;
    if (pa < m->rdram_size) {
        *ins = n64_be32(m->rdram + pa);
        m->now += kseg1(pc) ? COST_UNCACHED_RAM : COST_CACHED;
        return true;
    }
    if (pa >= 0x04000000u && pa < 0x04040000u) {
        *ins = n64_be32(m->spmem + (pa & 0x1FFC));
        m->now += COST_RSPMEM;
        return true;
    }
    *ins = n64_bus_read32(m, pa);
    m->now += pa >= 0x10000000u ? COST_ROM : COST_UNCACHED_RAM;
    return true;
}

/* CACHE (op: the rt field, cache in bits 1:0, operation in 4:2). Only the tags are modelled (n64_cpu itag / dtag):
 * every access still reads and writes memory, so the operations on data (fills, write backs) leave memory as it is,
 * while index / hit invalidates, store / load tag, fill and create dirty exclusive keep the tags the way the hardware
 * does (n64-systemtest "CACHE icache index invalidate ...", "CACHE dcache create dirty exclusive ..."). The hit
 * operations translate the address and can raise TLB exceptions; the index ones use its low bits only. False when
 * the translation raised (raise: false for lifted code, n64_rc_cache: then nothing is done). */
static bool cache_op(n64_machine *m, int op, uint64_t va, bool raise)
{
    n64_cpu *c = &m->cpu;
    bool dcache = (op & 3) == 1;
    if ((op & 3) > 1)
        return true; /* no secondary cache */
    uint32_t *tag = dcache ? &c->dtag[(va >> 4) & 511] : &c->itag[(va >> 5) & 511];
    uint32_t valid = dcache ? 3u << 6 : 2u << 6; /* D: valid dirty (PState 3), I: valid (2) */
    int kind = op >> 2;
    uint32_t pa = 0;
    if (kind >= 3) { /* create dirty exclusive, the hit operations, fill */
        if (!vtop(m, va, ACC_LOAD, &pa, raise))
            return false;
    }
    uint32_t pfn = (pa >> 12) << 8;
    bool hit = (*tag & 0xC0u) && (*tag & 0x0FFFFF00u) == pfn;
    switch (kind) {
    case 0: /* index invalidate (I) / index write back invalidate (D) */
        *tag &= ~0xC0u;
        break;
    case 1: /* index load tag */
        c->cop0[C0_TAGLO] = *tag;
        c->cop0[C0_TAGHI] = 0;
        break;
    case 2: /* index store tag */
        *tag = (uint32_t)c->cop0[C0_TAGLO] & 0x0FFFFFC0u;
        break;
    case 3: /* create dirty exclusive (D) */
        if (dcache)
            *tag = pfn | valid;
        break;
    case 4: /* hit invalidate */
        if (hit)
            *tag &= ~0xC0u;
        break;
    case 5: /* hit write back invalidate (D) / fill (I) */
        if (dcache) {
            if (hit)
                *tag &= ~0xC0u;
        } else {
            *tag = pfn | valid;
        }
        break;
    default: /* hit write back: memory is always current */
        break;
    }
    return true;
}

int n64_rc_cache(n64_machine *m, int op, uint64_t va)
{
    return cache_op(m, op, va, false) ? 0 : 1;
}

/* ---- Arithmetic helpers ---------------------------------------------------------------------------------------- */

static void mulu64(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
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

/* ---- COP0 instructions ----------------------------------------------------------------------------------------- */

static uint64_t cop0_read(n64_machine *m, int r)
{
    n64_cpu *c = &m->cpu;
    switch (r) {
    case C0_RANDOM:
        return random_reg(m);
    case C0_COUNT:
        return n64_cpu_count(m);
    case 7: case 21: case 22: case 23: case 24: case 25: case 31:
        return c->cop0_latch; /* unused registers read the last value written to any COP0 register */
    default:
        return c->cop0[r];
    }
}

static void cop0_write(n64_machine *m, int r, uint64_t v)
{
    n64_cpu *c = &m->cpu;
    c->cop0_latch = v;
    switch (r) {
    case C0_INDEX:
        c->cop0[r] = v & 0x8000003Fu;
        break;
    case C0_RANDOM:
    case C0_PRID:
    case C0_BADVADDR:
    case C0_CACHEERR:
        break;
    case C0_ENTRYLO0:
    case C0_ENTRYLO1:
        c->cop0[r] = v & 0x3FFFFFFFu;
        break;
    case C0_CONTEXT:
        c->cop0[r] = (v & ~0x7FFFFFull) | (c->cop0[r] & 0x7FFFF0ull);
        break;
    case C0_PAGEMASK:
        c->cop0[r] = v & 0x01FFE000u;
        break;
    case C0_WIRED:
        if (m->instructions >= c->random_base) { /* else the previous write has not taken effect yet */
            c->wired_prev = (uint32_t)c->cop0[r] & 63;
            c->random_base_prev = c->random_base;
        }
        c->cop0[r] = v & 0x3F;
        c->random_base = m->instructions + 2;
        break;
    case C0_COUNT:
        n64_cpu_set_count(m, (uint32_t)v);
        break;
    case C0_ENTRYHI:
        c->cop0[r] = v & 0xC00000FFFFFFE0FFull;
        c->tlb_gen++; /* the ASID may have changed */
        break;
    case C0_COMPARE:
        c->cop0[r] = (uint32_t)v;
        c->cop0[C0_CAUSE] &= ~(uint64_t)CAUSE_IP7;
        n64_cpu_schedule_compare(m);
        n64_cpu_update_irq(m);
        break;
    case C0_STATUS:
        c->cop0[r] = (uint32_t)v & 0xFFF7FFFFu;
        n64_cpu_status_changed(m);
        break;
    case C0_CAUSE:
        c->cop0[r] = (c->cop0[r] & ~0x300ull) | (v & 0x300);
        n64_cpu_update_irq(m);
        break;
    case C0_CONFIG:
        c->cop0[r] = (c->cop0[r] & ~0x0F00800Full) | (v & 0x0F00800Fu);
        break;
    case C0_LLADDR:
        c->cop0[r] = (uint32_t)v;
        break;
    case C0_WATCHLO:
        c->cop0[r] = (uint32_t)v & 0xFFFFFFFBu;
        break;
    case C0_WATCHHI:
        c->cop0[r] = v & 0xF;
        break;
    case C0_XCONTEXT:
        c->cop0[r] = (v & ~0x1FFFFFFFFull) | (c->cop0[r] & 0x1FFFFFFFFull);
        break;
    case C0_PARITY:
        c->cop0[r] = v & 0xFF;
        break;
    case C0_TAGLO:
        c->cop0[r] = (uint32_t)v & 0x0FFFFFC0u;
        break;
    case C0_TAGHI:
        c->cop0[r] = 0;
        break;
    default:
        c->cop0[r] = v;
        break;
    }
}

static void tlb_write(n64_machine *m, int i)
{
    n64_cpu *c = &m->cpu;
    n64_tlb_entry *e = &c->tlb[i & 31];
    /* PageMask: the upper bit of each pair counts for both. */
    uint32_t pm = (uint32_t)c->cop0[C0_PAGEMASK] & 0x01554000u;
    e->mask = pm | (pm >> 1);
    e->hi = c->cop0[C0_ENTRYHI] & ~(uint64_t)e->mask & 0xC00000FFFFFFE0FFull;
    e->lo[0] = (uint32_t)c->cop0[C0_ENTRYLO0];
    e->lo[1] = (uint32_t)c->cop0[C0_ENTRYLO1];
    tlb_derive(e); /* global: G of both halves */
    e->lo[0] &= 0x03FFFFFEu; /* the entry keeps PFN, C, D, V; G apart */
    e->lo[1] &= 0x03FFFFFEu;
    c->tlb_gen++;
}

static void cop0_op(n64_machine *m, uint32_t ins)
{
    n64_cpu *c = &m->cpu;
    int rt = RT(ins), rd = RD(ins);
    switch (RS(ins)) {
    case 0x00: /* MFC0 */
        if (rt)
            c->gpr[rt] = sx32(cop0_read(m, rd));
        return;
    case 0x01: /* DMFC0 */
        if (!c->ops64)
            break;
        if (rt)
            c->gpr[rt] = cop0_read(m, rd);
        return;
    case 0x04: /* MTC0 */
        cop0_write(m, rd, sx32(c->gpr[rt]));
        return;
    case 0x05: /* DMTC0 */
        if (!c->ops64)
            break;
        cop0_write(m, rd, c->gpr[rt]);
        return;
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        switch (ins & 0x3F) {
        case 0x01: { /* TLBR */
            const n64_tlb_entry *e = &c->tlb[c->cop0[C0_INDEX] & 31];
            c->cop0[C0_PAGEMASK] = e->mask;
            c->cop0[C0_ENTRYHI] = e->hi & ~(uint64_t)e->mask & 0xC00000FFFFFFE0FFull;
            c->cop0[C0_ENTRYLO0] = e->lo[0] | (e->global ? 1u : 0u);
            c->cop0[C0_ENTRYLO1] = e->lo[1] | (e->global ? 1u : 0u);
            return;
        }
        case 0x02: /* TLBWI */
            tlb_write(m, (int)(c->cop0[C0_INDEX] & 31));
            return;
        case 0x06: /* TLBWR */
            tlb_write(m, (int)random_reg(m));
            return;
        case 0x08: { /* TLBP */
            uint64_t hi = c->cop0[C0_ENTRYHI];
            c->cop0[C0_INDEX] = 0x80000000u;
            for (int i = 0; i < 32; i++) {
                const n64_tlb_entry *e = &c->tlb[i];
                if ((hi & e->vpn_mask) == e->vpn && (e->global || (uint8_t)e->hi == (uint8_t)hi)) {
                    c->cop0[C0_INDEX] = (uint64_t)i;
                    break;
                }
            }
            return;
        }
        case 0x18: { /* ERET */
            uint32_t sr = (uint32_t)c->cop0[C0_STATUS];
            if (sr & SR_ERL) {
                c->pc = c->cop0[C0_ERROREPC];
                sr &= ~SR_ERL;
            } else {
                c->pc = c->cop0[C0_EPC];
                sr &= ~SR_EXL;
            }
            c->cop0[C0_STATUS] = sr;
            c->npc = c->pc + 4;
            c->next_ds = false;
            c->llbit = false;
            n64_cpu_status_changed(m);
            return;
        }
        default: /* undefined CO functions do nothing on the VR4300 (emulator probes rely on it) */
            return;
        }
    default:
        break;
    }
    n64_cpu_exception(m, EXC_RI, 0);
}

/* ---- The interpreter ------------------------------------------------------------------------------------------- */

#define RI() do { n64_cpu_exception(m, EXC_RI, 0); return; } while (0)
#define NEED64() do { if (N64_UNLIKELY(!c->ops64)) RI(); } while (0)
#define SET(r, v) do { if (r) c->gpr[r] = (v); } while (0)

/* A branch adds its offset to the PC of the instruction after it: cur + 4, but in the delay slot of a jump or another
 * branch, that one's target (n64-systemtest "BEQ: Within delay slot of J", cycle group). */
static inline void branch(n64_cpu *c, bool taken, int64_t off)
{
    c->next_ds = true;
    if (taken)
        c->npc = c->pc + (uint64_t)(off << 2);
}

static inline void branch_likely(n64_cpu *c, bool taken, int64_t off)
{
    if (taken) {
        c->next_ds = true;
        c->npc = c->pc + (uint64_t)(off << 2);
    } else { /* the delay slot is nullified */
        c->pc = c->npc;
        c->npc += 4;
    }
}

static inline bool cop1_usable(n64_machine *m)
{
    if (N64_UNLIKELY(!(m->cpu.cop0[C0_STATUS] & SR_CU1))) {
        n64_cpu_exception(m, EXC_CPU, 1);
        return false;
    }
    return true;
}

static N64_HOT_INLINE void exec_special(n64_machine *m, uint32_t ins)
{
    n64_cpu *c = &m->cpu;
    uint64_t *g = c->gpr;
    int rs = RS(ins), rt = RT(ins), rd = RD(ins), sa = SA(ins);
    switch (ins & 0x3F) {
    case 0x00: SET(rd, sx32((uint32_t)g[rt] << sa)); break;                          /* SLL */
    case 0x02: SET(rd, sx32((uint32_t)g[rt] >> sa)); break;                          /* SRL */
    case 0x03: SET(rd, sx32((uint64_t)((int64_t)g[rt] >> sa))); break;               /* SRA */
    case 0x04: SET(rd, sx32((uint32_t)g[rt] << (g[rs] & 31))); break;                /* SLLV */
    case 0x06: SET(rd, sx32((uint32_t)g[rt] >> (g[rs] & 31))); break;                /* SRLV */
    case 0x07: SET(rd, sx32((uint64_t)((int64_t)g[rt] >> (g[rs] & 31)))); break;     /* SRAV */
    case 0x08: /* JR */
        c->next_ds = true;
        c->npc = g[rs];
        break;
    case 0x09: { /* JALR */
        uint64_t t = g[rs];
        SET(rd, c->pc + 4);
        c->next_ds = true;
        c->npc = t;
        break;
    }
    case 0x0C: n64_cpu_exception(m, EXC_SYS, 0); break;
    case 0x0D: n64_cpu_exception(m, EXC_BP, 0); break;
    case 0x0F: break; /* SYNC */
    case 0x10: SET(rd, c->hi); break;
    case 0x11: c->hi = g[rs]; break;
    case 0x12: SET(rd, c->lo); break;
    case 0x13: c->lo = g[rs]; break;
    case 0x14: NEED64(); SET(rd, g[rt] << (g[rs] & 63)); break;                     /* DSLLV */
    case 0x16: NEED64(); SET(rd, g[rt] >> (g[rs] & 63)); break;                     /* DSRLV */
    case 0x17: NEED64(); SET(rd, (uint64_t)((int64_t)g[rt] >> (g[rs] & 63))); break; /* DSRAV */
    case 0x18: { /* MULT */
        int64_t p = (int64_t)(int32_t)g[rs] * (int32_t)g[rt];
        c->lo = sx32((uint64_t)p);
        c->hi = sx32((uint64_t)p >> 32);
        m->now += COST_MULT;
        break;
    }
    case 0x19: { /* MULTU */
        uint64_t p = (uint64_t)(uint32_t)g[rs] * (uint32_t)g[rt];
        c->lo = sx32(p);
        c->hi = sx32(p >> 32);
        m->now += COST_MULT;
        break;
    }
    case 0x1A: { /* DIV */
        int32_t a = (int32_t)g[rs], b = (int32_t)g[rt];
        if (b == 0) {
            c->lo = a < 0 ? 1 : ~0ull;
            c->hi = sx32((uint32_t)a);
        } else if (a == INT32_MIN && b == -1) {
            c->lo = sx32(0x80000000u);
            c->hi = 0;
        } else {
            c->lo = sx32((uint32_t)(a / b));
            c->hi = sx32((uint32_t)(a % b));
        }
        m->now += COST_DIV;
        break;
    }
    case 0x1B: { /* DIVU */
        uint32_t a = (uint32_t)g[rs], b = (uint32_t)g[rt];
        if (b == 0) {
            c->lo = ~0ull;
            c->hi = sx32(a);
        } else {
            c->lo = sx32(a / b);
            c->hi = sx32(a % b);
        }
        m->now += COST_DIV;
        break;
    }
    case 0x1C: { /* DMULT */
        NEED64();
        uint64_t a = g[rs], b = g[rt], hi, lo;
        mulu64(a, b, &hi, &lo);
        if ((int64_t)a < 0)
            hi -= b;
        if ((int64_t)b < 0)
            hi -= a;
        c->hi = hi;
        c->lo = lo;
        m->now += COST_DMULT;
        break;
    }
    case 0x1D: /* DMULTU */
        NEED64();
        mulu64(g[rs], g[rt], &c->hi, &c->lo);
        m->now += COST_DMULT;
        break;
    case 0x1E: { /* DDIV */
        NEED64();
        int64_t a = (int64_t)g[rs], b = (int64_t)g[rt];
        if (b == 0) {
            c->lo = a < 0 ? 1 : ~0ull;
            c->hi = (uint64_t)a;
        } else if (a == INT64_MIN && b == -1) {
            c->lo = (uint64_t)a;
            c->hi = 0;
        } else {
            c->lo = (uint64_t)(a / b);
            c->hi = (uint64_t)(a % b);
        }
        m->now += COST_DDIV;
        break;
    }
    case 0x1F: { /* DDIVU */
        NEED64();
        uint64_t a = g[rs], b = g[rt];
        if (b == 0) {
            c->lo = ~0ull;
            c->hi = a;
        } else {
            c->lo = a / b;
            c->hi = a % b;
        }
        m->now += COST_DDIV;
        break;
    }
    case 0x20: { /* ADD */
        int32_t a = (int32_t)g[rs], b = (int32_t)g[rt], r;
        if (__builtin_add_overflow(a, b, &r)) {
            n64_cpu_exception(m, EXC_OV, 0);
            return;
        }
        SET(rd, (uint64_t)(int64_t)r);
        break;
    }
    case 0x21: SET(rd, sx32((uint32_t)g[rs] + (uint32_t)g[rt])); break; /* ADDU */
    case 0x22: { /* SUB */
        int32_t a = (int32_t)g[rs], b = (int32_t)g[rt], r;
        if (__builtin_sub_overflow(a, b, &r)) {
            n64_cpu_exception(m, EXC_OV, 0);
            return;
        }
        SET(rd, (uint64_t)(int64_t)r);
        break;
    }
    case 0x23: SET(rd, sx32((uint32_t)g[rs] - (uint32_t)g[rt])); break; /* SUBU */
    case 0x24: SET(rd, g[rs] & g[rt]); break;
    case 0x25: SET(rd, g[rs] | g[rt]); break;
    case 0x26: SET(rd, g[rs] ^ g[rt]); break;
    case 0x27: SET(rd, ~(g[rs] | g[rt])); break;
    case 0x2A: SET(rd, (int64_t)g[rs] < (int64_t)g[rt]); break;
    case 0x2B: SET(rd, g[rs] < g[rt]); break;
    case 0x2C: { /* DADD */
        NEED64();
        int64_t r;
        if (__builtin_add_overflow((int64_t)g[rs], (int64_t)g[rt], &r)) {
            n64_cpu_exception(m, EXC_OV, 0);
            return;
        }
        SET(rd, (uint64_t)r);
        break;
    }
    case 0x2D: NEED64(); SET(rd, g[rs] + g[rt]); break;
    case 0x2E: { /* DSUB */
        NEED64();
        int64_t r;
        if (__builtin_sub_overflow((int64_t)g[rs], (int64_t)g[rt], &r)) {
            n64_cpu_exception(m, EXC_OV, 0);
            return;
        }
        SET(rd, (uint64_t)r);
        break;
    }
    case 0x2F: NEED64(); SET(rd, g[rs] - g[rt]); break;
    case 0x30: if ((int64_t)g[rs] >= (int64_t)g[rt]) n64_cpu_exception(m, EXC_TR, 0); break; /* TGE */
    case 0x31: if (g[rs] >= g[rt]) n64_cpu_exception(m, EXC_TR, 0); break;                   /* TGEU */
    case 0x32: if ((int64_t)g[rs] < (int64_t)g[rt]) n64_cpu_exception(m, EXC_TR, 0); break;  /* TLT */
    case 0x33: if (g[rs] < g[rt]) n64_cpu_exception(m, EXC_TR, 0); break;                    /* TLTU */
    case 0x34: if (g[rs] == g[rt]) n64_cpu_exception(m, EXC_TR, 0); break;                   /* TEQ */
    case 0x36: if (g[rs] != g[rt]) n64_cpu_exception(m, EXC_TR, 0); break;                   /* TNE */
    case 0x38: NEED64(); SET(rd, g[rt] << sa); break;                                        /* DSLL */
    case 0x3A: NEED64(); SET(rd, g[rt] >> sa); break;                                        /* DSRL */
    case 0x3B: NEED64(); SET(rd, (uint64_t)((int64_t)g[rt] >> sa)); break;                   /* DSRA */
    case 0x3C: NEED64(); SET(rd, g[rt] << (sa + 32)); break;                                 /* DSLL32 */
    case 0x3E: NEED64(); SET(rd, g[rt] >> (sa + 32)); break;                                 /* DSRL32 */
    case 0x3F: NEED64(); SET(rd, (uint64_t)((int64_t)g[rt] >> (sa + 32))); break;            /* DSRA32 */
    default: RI();
    }
}

static N64_HOT_INLINE void exec_regimm(n64_machine *m, uint32_t ins)
{
    n64_cpu *c = &m->cpu;
    int64_t s = (int64_t)c->gpr[RS(ins)];
    int64_t imm = IMM(ins);
    switch (RT(ins)) {
    case 0x00: branch(c, s < 0, imm); break;
    case 0x01: branch(c, s >= 0, imm); break;
    case 0x02: branch_likely(c, s < 0, imm); break;
    case 0x03: branch_likely(c, s >= 0, imm); break;
    case 0x08: if (s >= imm) n64_cpu_exception(m, EXC_TR, 0); break;
    case 0x09: if ((uint64_t)s >= (uint64_t)imm) n64_cpu_exception(m, EXC_TR, 0); break;
    case 0x0A: if (s < imm) n64_cpu_exception(m, EXC_TR, 0); break;
    case 0x0B: if ((uint64_t)s < (uint64_t)imm) n64_cpu_exception(m, EXC_TR, 0); break;
    case 0x0C: if (s == imm) n64_cpu_exception(m, EXC_TR, 0); break;
    case 0x0E: if (s != imm) n64_cpu_exception(m, EXC_TR, 0); break;
    case 0x10: c->gpr[31] = c->pc + 4; branch(c, s < 0, imm); break;
    case 0x11: c->gpr[31] = c->pc + 4; branch(c, s >= 0, imm); break;
    case 0x12: c->gpr[31] = c->pc + 4; branch_likely(c, s < 0, imm); break;
    case 0x13: c->gpr[31] = c->pc + 4; branch_likely(c, s >= 0, imm); break;
    default: RI();
    }
}

static N64_HOT_INLINE void exec(n64_machine *m, uint32_t ins)
{
    n64_cpu *c = &m->cpu;
    uint64_t *g = c->gpr;
    int rs = RS(ins), rt = RT(ins);
    int64_t imm = IMM(ins);
    uint64_t v;
    switch (ins >> 26) {
    case 0x00: exec_special(m, ins); break;
    case 0x01: exec_regimm(m, ins); break;
    case 0x02: /* J */
        c->next_ds = true;
        c->npc = ((c->cur + 4) & ~0x0FFFFFFFull) | ((uint64_t)(ins & 0x03FFFFFF) << 2);
        break;
    case 0x03: /* JAL */
        g[31] = c->pc + 4; /* the address after the next one to run: in a delay slot, the first jump's target + 4 */
        c->next_ds = true;
        c->npc = ((c->cur + 4) & ~0x0FFFFFFFull) | ((uint64_t)(ins & 0x03FFFFFF) << 2);
        break;
    case 0x04: branch(c, g[rs] == g[rt], imm); break;
    case 0x05: branch(c, g[rs] != g[rt], imm); break;
    case 0x06: branch(c, (int64_t)g[rs] <= 0, imm); break;
    case 0x07: branch(c, (int64_t)g[rs] > 0, imm); break;
    case 0x08: { /* ADDI */
        int32_t r;
        if (__builtin_add_overflow((int32_t)g[rs], (int32_t)imm, &r)) {
            n64_cpu_exception(m, EXC_OV, 0);
            return;
        }
        SET(rt, (uint64_t)(int64_t)r);
        break;
    }
    case 0x09: SET(rt, sx32((uint32_t)g[rs] + (uint32_t)imm)); break;  /* ADDIU */
    case 0x0A: SET(rt, (int64_t)g[rs] < imm); break;                     /* SLTI */
    case 0x0B: SET(rt, g[rs] < (uint64_t)imm); break;                    /* SLTIU */
    case 0x0C: SET(rt, g[rs] & (ins & 0xFFFF)); break;
    case 0x0D: SET(rt, g[rs] | (ins & 0xFFFF)); break;
    case 0x0E: SET(rt, g[rs] ^ (ins & 0xFFFF)); break;
    case 0x0F: SET(rt, (uint64_t)(imm << 16)); break;                    /* LUI */
    case 0x10: /* COP0 */
        if (c->ksu != 0 && !(c->cop0[C0_STATUS] & SR_CU0)) {
            n64_cpu_exception(m, EXC_CPU, 0);
            return;
        }
        cop0_op(m, ins);
        break;
    case 0x11: /* COP1 */
        if (cop1_usable(m))
            n64_fpu_cop1(m, ins);
        break;
    case 0x12: /* COP2: no coprocessor 2, but its moves go through a latch */
        if (!(c->cop0[C0_STATUS] & SR_CU2)) {
            n64_cpu_exception(m, EXC_CPU, 2);
            return;
        }
        switch (rs) {
        case 0x00: SET(rt, sx32(c->cop2_latch)); break;
        case 0x01: SET(rt, c->cop2_latch); break;
        case 0x02: SET(rt, sx32(c->cop2_latch)); break;
        case 0x04: case 0x05: case 0x06: c->cop2_latch = g[rt]; break;
        default: n64_cpu_exception(m, EXC_RI, 2); return;
        }
        break;
    case 0x13: /* COP3 */
        RI();
    case 0x14: branch_likely(c, g[rs] == g[rt], imm); break;
    case 0x15: branch_likely(c, g[rs] != g[rt], imm); break;
    case 0x16: branch_likely(c, (int64_t)g[rs] <= 0, imm); break;
    case 0x17: branch_likely(c, (int64_t)g[rs] > 0, imm); break;
    case 0x18: { /* DADDI */
        NEED64();
        int64_t r;
        if (__builtin_add_overflow((int64_t)g[rs], imm, &r)) {
            n64_cpu_exception(m, EXC_OV, 0);
            return;
        }
        SET(rt, (uint64_t)r);
        break;
    }
    case 0x19: NEED64(); SET(rt, g[rs] + (uint64_t)imm); break; /* DADDIU */
    case 0x1A: case 0x1B: { /* LDL LDR */
        NEED64();
        uint64_t a = g[rs] + (uint64_t)imm;
        if (N64_UNLIKELY(c->re))
            a ^= 7;
        if (!rd_partial(m, a, 8, &v))
            return;
        uint64_t r = g[rt];
        if ((ins >> 26) == 0x1A) {
            unsigned sh = 8 * (unsigned)(a & 7);
            uint64_t mask = ~0ull << sh;
            r = (r & ~mask) | (v << sh);
        } else {
            unsigned sh = 8 * (unsigned)((a ^ 7) & 7);
            uint64_t mask = ~0ull >> sh;
            r = (r & ~mask) | (v >> sh);
        }
        SET(rt, r);
        break;
    }
    case 0x1F: /* reserved */
        RI();
    case 0x20: if (rd(m, g[rs] + (uint64_t)imm, 1, &v)) SET(rt, (uint64_t)(int64_t)(int8_t)v); break;   /* LB */
    case 0x21: if (rd(m, g[rs] + (uint64_t)imm, 2, &v)) SET(rt, (uint64_t)(int64_t)(int16_t)v); break;  /* LH */
    case 0x22: case 0x26: { /* LWL LWR */
        uint64_t a = g[rs] + (uint64_t)imm;
        if (N64_UNLIKELY(c->re))
            a ^= 7;
        if (!rd_partial(m, a, 4, &v))
            return;
        uint32_t r = (uint32_t)g[rt], w = (uint32_t)v;
        if ((ins >> 26) == 0x22) { /* LWL always loads bit 31: sign-extended */
            unsigned sh = 8 * (unsigned)(a & 3);
            uint32_t mask = 0xFFFFFFFFu << sh;
            r = (r & ~mask) | (w << sh);
            SET(rt, sx32(r));
        } else { /* LWR sign-extends only when it loads the whole word, else the upper half stays */
            unsigned sh = 8 * (unsigned)((a ^ 3) & 3);
            uint32_t mask = 0xFFFFFFFFu >> sh;
            r = (r & ~mask) | (w >> sh);
            SET(rt, (a & 3) == 3 ? sx32(r) : (g[rt] & 0xFFFFFFFF00000000ull) | r);
        }
        break;
    }
    case 0x23: if (rd(m, g[rs] + (uint64_t)imm, 4, &v)) SET(rt, sx32(v)); break;                  /* LW */
    case 0x24: if (rd(m, g[rs] + (uint64_t)imm, 1, &v)) SET(rt, v); break;                        /* LBU */
    case 0x25: if (rd(m, g[rs] + (uint64_t)imm, 2, &v)) SET(rt, v); break;                        /* LHU */
    case 0x27: NEED64(); if (rd(m, g[rs] + (uint64_t)imm, 4, &v)) SET(rt, v); break;              /* LWU */
    case 0x28: wr(m, g[rs] + (uint64_t)imm, 1, g[rt]); break;
    case 0x29: wr(m, g[rs] + (uint64_t)imm, 2, g[rt]); break;
    case 0x2A: case 0x2E: { /* SWL SWR: the bytes of the aligned word they cover */
        uint64_t a = g[rs] + (uint64_t)imm;
        if (N64_UNLIKELY(c->re))
            a ^= 7;
        uint32_t r = (uint32_t)g[rt], mask, data;
        if ((ins >> 26) == 0x2A) {
            unsigned sh = 8 * (unsigned)(a & 3);
            mask = 0xFFFFFFFFu >> sh;
            data = r >> sh;
        } else {
            unsigned sh = 8 * (unsigned)((a ^ 3) & 3);
            mask = 0xFFFFFFFFu << sh;
            data = r << sh;
        }
        store_masked(m, a, (uint64_t)data << 32, (uint64_t)mask << 32, 4);
        break;
    }
    case 0x2B: wr(m, g[rs] + (uint64_t)imm, 4, g[rt]); break;
    case 0x2C: case 0x2D: { /* SDL SDR */
        NEED64();
        uint64_t a = g[rs] + (uint64_t)imm;
        if (N64_UNLIKELY(c->re))
            a ^= 7;
        uint64_t r = g[rt], mask, data;
        if ((ins >> 26) == 0x2C) {
            unsigned sh = 8 * (unsigned)(a & 7);
            mask = ~0ull >> sh;
            data = r >> sh;
        } else {
            unsigned sh = 8 * (unsigned)((a ^ 7) & 7);
            mask = ~0ull << sh;
            data = r << sh;
        }
        store_masked(m, a, data, mask, 8);
        break;
    }
    case 0x2F: /* CACHE */
        if (c->ksu != 0 && !(c->cop0[C0_STATUS] & SR_CU0)) {
            n64_cpu_exception(m, EXC_CPU, 0);
            return;
        }
        cache_op(m, rt, g[rs] + (uint64_t)imm, true);
        break;
    case 0x30: { /* LL */
        uint64_t a = g[rs] + (uint64_t)imm;
        uint32_t pa = 0;
        if (!rd(m, a, 4, &v))
            return;
        vtop_slow(m, a, ACC_LOAD, &pa, false);
        SET(rt, sx32(v));
        c->llbit = true;
        c->cop0[C0_LLADDR] = pa >> 4;
        break;
    }
    case 0x31: /* LWC1 */
        if (cop1_usable(m) && rd(m, g[rs] + (uint64_t)imm, 4, &v))
            n64_fpu_write_s(c, rt, (uint32_t)v);
        break;
    case 0x34: { /* LLD */
        NEED64();
        uint64_t a = g[rs] + (uint64_t)imm;
        uint32_t pa = 0;
        if (!rd(m, a, 8, &v))
            return;
        vtop_slow(m, a, ACC_LOAD, &pa, false);
        SET(rt, v);
        c->llbit = true;
        c->cop0[C0_LLADDR] = pa >> 4;
        break;
    }
    case 0x35: /* LDC1 */
        if (cop1_usable(m) && rd(m, g[rs] + (uint64_t)imm, 8, &v))
            n64_fpu_write_d(c, rt, v);
        break;
    case 0x37: NEED64(); if (rd(m, g[rs] + (uint64_t)imm, 8, &v)) SET(rt, v); break; /* LD */
    case 0x38: /* SC */
        if (c->llbit) {
            if (wr(m, g[rs] + (uint64_t)imm, 4, g[rt]))
                SET(rt, 1);
        } else {
            uint64_t a = g[rs] + (uint64_t)imm;
            if (a & 3) {
                address_error(m, a, ACC_STORE);
                return;
            }
            SET(rt, 0);
        }
        break;
    case 0x39: /* SWC1 */
        if (cop1_usable(m))
            wr(m, g[rs] + (uint64_t)imm, 4, n64_fpu_read_s(c, rt));
        break;
    case 0x3C: /* SCD */
        NEED64();
        if (c->llbit) {
            if (wr(m, g[rs] + (uint64_t)imm, 8, g[rt]))
                SET(rt, 1);
        } else {
            uint64_t a = g[rs] + (uint64_t)imm;
            if (a & 7) {
                address_error(m, a, ACC_STORE);
                return;
            }
            SET(rt, 0);
        }
        break;
    case 0x3D: /* SDC1 */
        if (cop1_usable(m))
            wr(m, g[rs] + (uint64_t)imm, 8, n64_fpu_read_d(c, rt));
        break;
    case 0x3F: NEED64(); wr(m, g[rs] + (uint64_t)imm, 8, g[rt]); break; /* SD */
    case 0x32: case 0x36: case 0x3A: case 0x3E: /* LWC2 LDC2 SWC2 SDC2 */
        if (!(c->cop0[C0_STATUS] & SR_CU2)) {
            n64_cpu_exception(m, EXC_CPU, 2);
            return;
        }
        /* With CU2 set they raise nothing and go through the COP2 latch like the moves (n64-systemtest
         * "LWC2/LDC2/SWC2/SDC2", a poorly understood quirk): both loads fill the latch with the aligned doubleword,
         * SDC2 stores the latch, SWC2 its low word. */
        {
            uint64_t a = g[rs] + (uint64_t)imm;
            if ((ins >> 26) == 0x32 || (ins >> 26) == 0x36) {
                if (rd(m, a & ~(uint64_t)7, 8, &v))
                    c->cop2_latch = v;
            } else if ((ins >> 26) == 0x3E) {
                wr(m, a & ~(uint64_t)7, 8, c->cop2_latch);
            } else {
                wr(m, a & ~(uint64_t)3, 4, (uint32_t)c->cop2_latch);
            }
        }
        break;
    default: /* 0x1C-0x1E, 0x33, 0x3B: reserved (COP3 loads / stores) */
        RI();
    }
}

_Static_assert(COST_INTERRUPT == 2 * N64_TICKS_PER_CYCLE, "an interrupt costs two cycles");

static inline void take_interrupt(n64_machine *m)
{
    n64_cpu *c = &m->cpu;
    c->cur = c->pc;
    c->ds = c->next_ds;
    exception_vec(m, EXC_INT, 0, 0x180);
    m->now += COST_INTERRUPT;
}

static N64_HOT_INLINE void step(n64_machine *m)
{
    n64_cpu *c = &m->cpu;
    uint64_t pc = c->pc;
    c->cur = pc;
    c->ds = c->next_ds;
    c->next_ds = false;
    uint32_t ins;
    uint64_t off = pc - 0xFFFFFFFF80000000ull;
    if (N64_LIKELY(off < (uint64_t)m->rdram_size && c->ksu == 0 && !(pc & 3))) {
        ins = n64_be32(m->rdram + off);
        m->now += COST_CACHED;
    } else if (!fetch_slow(m, pc, &ins)) {
        m->now += COST_CACHED;
        return;
    }
    c->pc = c->npc;
    c->npc += 4;
    exec(m, ins);
    c->gpr[0] = 0;
    m->instructions++;
}

void n64_cpu_step(n64_machine *m) { step(m); }

void n64_cpu_run_until(n64_machine *m, uint64_t stop_pc)
{
    while (m->now < m->stop_at && !m->cpu.irq && m->cpu.pc != stop_pc)
        step(m);
}

uint32_t n64_cpu_step_handed(n64_machine *m, uint32_t *left, bool *chained)
{
    n64_cpu *c = &m->cpu;
    uint32_t n = 0;
    for (;;) {
        uint64_t pc = c->pc;
        step(m);
        n++;
        if (*left <= 1 || c->pc != pc + 4) {
            *chained = false;
            return n;
        }
        (*left)--;
        *chained = true;
        /* The dispatcher's checks before it hands the next instruction over (n64_recomp_run), the simple case. */
        uint64_t nx = c->pc;
        uint32_t va = (uint32_t)nx;
        if (m->now >= m->stop_at || c->irq || c->ksu != 0 || nx != (uint64_t)(int64_t)(int32_t)va ||
            va - 0x80000000u >= 0x40000000u || (va & 0x1FFFFFFFu) >= m->rdram_size)
            return n;
    }
}
void n64_cpu_take_interrupt(n64_machine *m) { take_interrupt(m); }

/* ---- Predecoded blocks (XBA-210) ------------------------------------------------------------------------------------
 * The model is the PS1 interpreter's (ps1_cpu.c, XBA-204). A block is a straight run of code in RDRAM, reached through
 * KSEG0 (fetch cost COST_CACHED), KSEG1 (COST_UNCACHED_RAM) or a TLB-mapped 32-bit segment (COST_CACHED, as step()
 * charges a mapped fetch), decoded once into ops: operands extracted, immediates sign- or zero-extended, branch and
 * jump targets and link values computed, each op holding its handler's address (direct threading) and the block
 * ending with a K_END op, so an op is a few host instructions and a jump. A block never crosses a 4 KB page (one
 * rdram_gen counter covers it, and a mapped page translates to contiguous RDRAM). It ends after a branch or jump and its
 * delay slot (the delay slot is the block's last op: delay slots are resolved at decode, a likely branch that is not
 * taken skips it, with neither its fetch cost nor its instruction count, as step() does), before an instruction the
 * ops don't hold (TLB instructions, ERET, LL / SC, traps, SYSCALL / BREAK, COP2 / COP3, a trapping or loading
 * instruction whose destination is r0, a branch whose delay slot would be one of those or a branch), at the page's end
 * or at BLK_MAX ops. The RSP's memories and the ROM are not block code (IPL3 only): step() runs them.
 *
 * A block is entered only where step() would run its instructions back to back with every check of the run loop
 * passing: not in a delay slot, kernel mode (so 64-bit operations are allowed and COP0 is usable), MI repeat mode off,
 * no interrupt pending, now below stop_at. The clock: every op has a bound of the ticks it takes (its fetch plus its
 * fixed extras: COST_MULT / COST_DMULT / COST_DIV / COST_DDIV, COST_DATA_UNCACHED for a load, the FPU's latency of a
 * COP1 instruction), cum[k] bounds ops 0..k-1; a block that may meet stop_at runs only the ops sure to start before
 * it (from a copy of them ending with a K_CUT op), and the run loop then steps to the event, so events (VI lines,
 * COUNT = COMPARE, PI / SI / AI DMA ends, RSP slices) and interrupts land at the instruction they do with step().
 * Fetch costs are added for the whole block at entry and taken back for the ops that did not run. A block made of a
 * branch to itself over a NOP (an idle loop) fast-forwards to stop_at: every op that would start before it is counted
 * with its fetch cost, nothing else can change meanwhile.
 *
 * Exceptions are exact: an op that would raise one (ADD / SUB / ADDI / DADD / DSUB / DADDI overflow, a misaligned
 * address, a TLB miss / invalid / modified page or an address error on a data access or a CACHE hit operation, COP1
 * unusable) is detected before it changes anything; the state is rebuilt as step() holds it in front of that
 * instruction (pc, npc and the delay-slot flag of a delay slot, the clock, the instruction count) and step() runs it,
 * so EPC / BD / BadVAddr / Context and the TLB refill vector are step()'s. COP1 arithmetic, moves and CTC1 run through
 * n64_fpu_cop1 with the clock, cur and ds set as step() has them, and an FPE / unimplemented exception it takes ends
 * the run there. MFC0 / DMFC0 read COUNT and RANDOM from the clock and the instruction count step() would have; MTC0 /
 * DMTC0 go through cop0_write the same way and the block stops after them (the interrupt line, stop_at, the mode, the
 * TLB generation may move). Data accesses to RDRAM through KSEG0 / KSEG1 are inline; anything else goes through the
 * interpreter's own access paths (rd_x / wr_x / rd_partial_x / store_masked_x, non-raising) with the clock where step()
 * has it: mapped RDRAM goes on; after a bus access (devices, cartridge, PIF) the block stops if the access raised the
 * interrupt line, moved stop_at, switched MI repeat mode on, wrote into the block's page (a DMA) or took long enough
 * that its remaining ops might no longer start before stop_at, and goes on otherwise.
 *
 * Invalidation: every write into RDRAM bumps its page counter (CPU stores, PI / SI / SP DMA, the RSP, n64_poke32,
 * n64_rdram, cheats, save state loads, reset: the packs' mechanism, n64_rdram_wrote); a block whose counter moved has
 * its instruction words compared again and is decoded again if they differ. A store into the running block's own
 * words stops it after the store. A mapped block also keeps the TLB generation (tlb_gen: TLBWI / TLBWR / EntryHi,
 * so the ASID, bump it) and Status.ERL it was translated under; when either moved its start is translated again and
 * must land on the same RDRAM. The CACHE instruction only keeps tags (no data or instructions are cached, cache_op),
 * so it never changes what a block executes. Blocks are found through a direct-mapped table by address, and through
 * each block's links to the blocks that ran after it (fall-through and taken), which skip the table but not the
 * checks above.
 *
 * Between the run loop's stops every field of the machine is what step()'s loop leaves (cur and ds too), so frames,
 * save states and lockstep comparisons are byte-identical; n64_set_interp_reference keeps step()'s loop as the
 * oracle (n64_run --lockstep-ref). The block cache is host state (the tail of n64_machine), not in save states. */
enum {
    K_NOP,
    /* d = s op t */
    K_ADDU, K_SUBU, K_AND, K_OR, K_XOR, K_NOR, K_SLT, K_SLTU, K_DADDU, K_DSUBU,
    K_SLLV, K_SRLV, K_SRAV, K_DSLLV, K_DSRLV, K_DSRAV,
    K_ADD, K_SUB, K_DADD, K_DSUB,
    /* d = t op imm (shift amount; DSLL32 & co. as imm + 32) */
    K_SLL, K_SRL, K_SRA, K_DSLL, K_DSRL, K_DSRA,
    /* d = s op imm (sign- or zero-extended at decode), K_LI: d = imm */
    K_ADDIU, K_DADDIU, K_SLTI, K_SLTIU, K_ANDI, K_ORI, K_XORI, K_LI, K_ADDI, K_DADDI,
    /* HI / LO */
    K_MFHI, K_MFLO, K_MTHI, K_MTLO, K_MULT, K_MULTU, K_DIV, K_DIVU, K_DMULT, K_DMULTU, K_DDIV, K_DDIVU,
    /* loads into t, stores of t: address s + imm */
    K_LB, K_LBU, K_LH, K_LHU, K_LW, K_LWU, K_LD, K_LWL, K_LWR, K_LDL, K_LDR,
    K_SB, K_SH, K_SW, K_SD, K_SWL, K_SWR, K_SDL, K_SDR,
    K_LWC1, K_LDC1, K_SWC1, K_SDC1,
    /* any other COP1 instruction but BC1x (imm: the instruction), through n64_fpu_cop1 */
    K_COP1,
    /* COP0: MFC0 / DMFC0 (t = COP0 register d; likely: 64-bit), MTC0 / DMTC0 (through cop0_write, the block stops
     * after it), CACHE (t: the operation, address s + imm; cache_op) */
    K_MFC0, K_MTC0, K_CACHE,
    /* branches (imm: the target; d: 31 for the linking ones; likely: the delay slot is skipped when not taken) and
     * jumps (J: imm the target, JR: s; d the link register or 0): the block's last op but one */
    K_BEQ, K_BNE, K_BLEZ, K_BGTZ, K_BLTZ, K_BGEZ, K_BC1F, K_BC1T, K_J, K_JR,
    K_UNSUPPORTED,
    K_END, /* after the block's last op */
    K_CUT, /* after the last op of a run cut short in front of stop_at */
};
#define K_IS_BRANCH(k) ((k) >= K_BEQ && (k) <= K_JR)

/* An op: its handler in run_blocks (direct threading), the immediate (sign-extended to 64 bits where it is used: block
 * code lives in the 32-bit segments, so targets and link values fit), the registers. */
typedef struct bop {
    const void *h;
    int32_t imm;
    uint8_t s, t, d;
    uint8_t likely;
} bop;
#define IMM64(o) ((uint64_t)(int64_t)(o)->imm)

#define BLK_MAX 64
#define BLK_TAB 65536u
#define BLK_ARENA (8u << 20)
#define BLK_NO_PC 1u /* never a PC (odd): exception_vec overwrites it */

typedef struct blk {
    uint64_t vpc;      /* its first instruction */
    uint64_t fall;     /* the address after its last op (the link value of its branch) */
    uint32_t pa;       /* the RDRAM address of its first instruction */
    uint32_t page;     /* its rdram_gen page */
    uint32_t gen;      /* that page's counter when the words were last checked */
    uint32_t tlb_gen;  /* mapped: the TLB generation it was translated under */
    uint32_t len4;     /* 4 x n */
    uint32_t cmp4;     /* the bytes of words (every instruction decode looked at, the first at least) */
    uint32_t maxcost;  /* bound of the ticks its ops take before the last one starts (cum[n - 1]) */
    uint32_t fcsum;    /* fetch ticks of all of it */
    uint16_t n;        /* ops (0: none, step() runs the instruction) */
    uint8_t fc;        /* fetch ticks per instruction */
    uint8_t branch;    /* it ends with a branch or jump and its delay slot */
    uint8_t mapped;    /* translated through the TLB (or kuseg under ERL) */
    uint8_t erl;       /* mapped: Status.ERL it was translated under */
    uint8_t idle;      /* a branch to itself with a NOP in its delay slot (nothing changes while it spins) */
    uint8_t nolink;    /* links may not lead here: mapped (its translation is checked through the table), or replaced
                        * in the table (decoded again or evicted: no longer revalidated, its counter could wrap) */
    uint8_t rec;       /* the dispatcher recorded missed code here (n64_cpu_run_region): taken branches to it go on */
    uint64_t lastpc;   /* its last op's address */
    /* The blocks run after it last (fall-through / not taken, and taken or jumped to): a link skips the table, the
     * block's own checks still run. A link is only written when no flush happened since both blocks were found, so it
     * never points into a reused arena. */
    struct blk *succ[2];
    const uint32_t *words; /* the instruction words decode looked at (RDRAM's bytes) */
    const uint32_t *cum;   /* cum[k]: bound of the ticks ops 0..k-1 take */
    bop ops[];         /* n ops, then a K_END */
} blk;

struct n64_blocks {
    blk *tab[BLK_TAB]; /* direct mapped by vpc */
    uint8_t *arena;
    size_t used;
    uint32_t epoch;    /* bumped by every flush */
};

void n64_cpu_free(n64_machine *m)
{
    if (m->blocks) {
        free(m->blocks->arena);
        free(m->blocks);
        m->blocks = NULL;
    }
}

void n64_set_interp_reference(n64_machine *m, int on) { m->interp_ref = on != 0; }

static void blocks_flush(struct n64_blocks *bc)
{
    memset(bc->tab, 0, sizeof bc->tab);
    bc->used = 0;
    bc->epoch++;
}

static inline uint32_t blk_index(uint64_t pc) { return (((uint32_t)pc >> 2) ^ ((uint32_t)pc >> 29)) & (BLK_TAB - 1); }

/* The latency n64_fpu_cop1 charges for a COP1 instruction (op_s / op_d: ADD / SUB, MUL, DIV, SQRT). */
static uint32_t fpu_extra(uint32_t ins)
{
    uint32_t fmt = (ins >> 21) & 31, fn = ins & 0x3F;
    if (fmt == 0x10)
        return fn <= 1 ? 4 : fn == 2 ? 6 : fn == 3 || fn == 4 ? 56 : 0;
    if (fmt == 0x11)
        return fn <= 1 ? 4 : fn == 2 ? 12 : fn == 3 || fn == 4 ? 114 : 0;
    return 0;
}

/* Decodes one instruction at ipc into o. Returns its kind (K_UNSUPPORTED: step() runs it) and the bound of the ticks
 * it adds beyond its fetch in *extra. */
static uint32_t blk_decode(uint32_t ins, uint64_t ipc, bop *o, uint32_t *extra)
{
    uint32_t rs = RS(ins), rt = RT(ins), rd = RD(ins), sa = SA(ins);
    uint64_t simm = (uint64_t)IMM(ins), zimm = ins & 0xFFFFu;
    uint32_t k = K_UNSUPPORTED;
    uint64_t imm64 = 0;
    memset(o, 0, sizeof *o);
    *extra = 0;
    o->s = (uint8_t)rs;
    o->t = (uint8_t)rt;
    switch (ins >> 26) {
    case 0x00: { /* SPECIAL */
        static const uint8_t rr[64] = {
            [0x04] = K_SLLV, [0x06] = K_SRLV, [0x07] = K_SRAV, [0x14] = K_DSLLV, [0x16] = K_DSRLV, [0x17] = K_DSRAV,
            [0x21] = K_ADDU, [0x23] = K_SUBU, [0x24] = K_AND, [0x25] = K_OR, [0x26] = K_XOR, [0x27] = K_NOR,
            [0x2A] = K_SLT, [0x2B] = K_SLTU, [0x2D] = K_DADDU, [0x2F] = K_DSUBU,
            [0x00] = K_SLL, [0x02] = K_SRL, [0x03] = K_SRA, [0x38] = K_DSLL, [0x3A] = K_DSRL, [0x3B] = K_DSRA,
            [0x3C] = K_DSLL, [0x3E] = K_DSRL, [0x3F] = K_DSRA,
        };
        uint32_t fn = ins & 0x3F;
        o->d = (uint8_t)rd;
        if (rr[fn] || fn == 0x00) {
            k = rr[fn];
            imm64 = fn >= 0x3C ? sa + 32 : sa;
            if (rd == 0)
                k = K_NOP;
            break;
        }
        switch (fn) {
        case 0x08: k = K_JR; o->d = 0; break;
        case 0x09: k = K_JR; break; /* JALR: d the link register (0: none) */
        case 0x0F: k = K_NOP; break; /* SYNC */
        case 0x10: k = rd ? K_MFHI : K_NOP; break;
        case 0x12: k = rd ? K_MFLO : K_NOP; break;
        case 0x11: k = K_MTHI; break;
        case 0x13: k = K_MTLO; break;
        case 0x18: k = K_MULT; *extra = COST_MULT; break;
        case 0x19: k = K_MULTU; *extra = COST_MULT; break;
        case 0x1A: k = K_DIV; *extra = COST_DIV; break;
        case 0x1B: k = K_DIVU; *extra = COST_DIV; break;
        case 0x1C: k = K_DMULT; *extra = COST_DMULT; break;
        case 0x1D: k = K_DMULTU; *extra = COST_DMULT; break;
        case 0x1E: k = K_DDIV; *extra = COST_DDIV; break;
        case 0x1F: k = K_DDIVU; *extra = COST_DDIV; break;
        case 0x20: k = rd ? K_ADD : K_UNSUPPORTED; break;
        case 0x22: k = rd ? K_SUB : K_UNSUPPORTED; break;
        case 0x2C: k = rd ? K_DADD : K_UNSUPPORTED; break;
        case 0x2E: k = rd ? K_DSUB : K_UNSUPPORTED; break;
        default: break; /* traps, SYSCALL, BREAK, reserved */
        }
        break;
    }
    case 0x01: /* REGIMM */
        switch (rt) {
        case 0x00: case 0x02: case 0x10: case 0x12: k = K_BLTZ; break;
        case 0x01: case 0x03: case 0x11: case 0x13: k = K_BGEZ; break;
        default: break; /* traps */
        }
        o->likely = (rt & 2) != 0;
        o->d = (rt & 0x10) ? 31 : 0;
        imm64 = ipc + 4 + (simm << 2);
        break;
    case 0x02: case 0x03: /* J JAL */
        k = K_J;
        o->d = (ins >> 26) == 0x03 ? 31 : 0;
        imm64 = ((ipc + 4) & ~0x0FFFFFFFull) | ((uint64_t)(ins & 0x03FFFFFF) << 2);
        break;
    case 0x04: case 0x14: k = K_BEQ; goto branch;
    case 0x05: case 0x15: k = K_BNE; goto branch;
    case 0x06: case 0x16: k = K_BLEZ; goto branch;
    case 0x07: case 0x17: k = K_BGTZ;
    branch:
        o->likely = (ins >> 26) >= 0x14;
        imm64 = ipc + 4 + (simm << 2);
        break;
    case 0x08: k = rt ? K_ADDI : K_UNSUPPORTED; imm64 = simm; break;
    case 0x09: k = K_ADDIU; imm64 = simm; break;
    case 0x0A: k = K_SLTI; imm64 = simm; break;
    case 0x0B: k = K_SLTIU; imm64 = simm; break;
    case 0x0C: k = K_ANDI; imm64 = zimm; break;
    case 0x0D: k = K_ORI; imm64 = zimm; break;
    case 0x0E: k = K_XORI; imm64 = zimm; break;
    case 0x0F: k = K_LI; imm64 = simm << 16; break;
    case 0x18: k = rt ? K_DADDI : K_UNSUPPORTED; imm64 = simm; break;
    case 0x19: k = K_DADDIU; imm64 = simm; break;
    case 0x11: /* COP1 */
        if (rs == 0x08) {
            k = (rt & 1) ? K_BC1T : K_BC1F;
            o->likely = (rt & 2) != 0;
            imm64 = ipc + 4 + (simm << 2);
        } else {
            k = K_COP1;
            imm64 = ins;
            *extra = fpu_extra(ins);
        }
        break;
    case 0x10: /* COP0: the moves (kernel mode: usable, 64-bit allowed) */
        o->d = (uint8_t)rd;
        o->likely = (rs & 1) != 0;
        if (rs == 0x00 || rs == 0x01)
            k = rt ? K_MFC0 : K_NOP;
        else if (rs == 0x04 || rs == 0x05)
            k = K_MTC0;
        break;
    case 0x2F: k = K_CACHE; break;
    case 0x1A: k = K_LDL; break;
    case 0x1B: k = K_LDR; break;
    case 0x20: k = K_LB; *extra = COST_DATA_UNCACHED; break;
    case 0x21: k = K_LH; *extra = COST_DATA_UNCACHED; break;
    case 0x22: k = K_LWL; break;
    case 0x23: k = K_LW; *extra = COST_DATA_UNCACHED; break;
    case 0x24: k = K_LBU; *extra = COST_DATA_UNCACHED; break;
    case 0x25: k = K_LHU; *extra = COST_DATA_UNCACHED; break;
    case 0x26: k = K_LWR; break;
    case 0x27: k = K_LWU; *extra = COST_DATA_UNCACHED; break;
    case 0x37: k = K_LD; *extra = COST_DATA_UNCACHED; break;
    case 0x28: k = K_SB; break;
    case 0x29: k = K_SH; break;
    case 0x2A: k = K_SWL; break;
    case 0x2B: k = K_SW; break;
    case 0x2C: k = K_SDL; break;
    case 0x2D: k = K_SDR; break;
    case 0x2E: k = K_SWR; break;
    case 0x3F: k = K_SD; break;
    case 0x31: k = K_LWC1; *extra = COST_DATA_UNCACHED; break;
    case 0x35: k = K_LDC1; *extra = COST_DATA_UNCACHED; break;
    case 0x39: k = K_SWC1; break;
    case 0x3D: k = K_SDC1; break;
    default: break; /* COP2, COP3, LL / SC / LLD / SCD, reserved */
    }
    if ((k >= K_LB && k <= K_LDR) || k == K_LWC1 || k == K_LDC1 || (k >= K_SB && k <= K_SDC1) || k == K_CACHE)
        imm64 = simm;
    if (imm64 != (uint64_t)(int64_t)(int32_t)imm64)
        k = K_UNSUPPORTED; /* a target out of the 32-bit segments (step() faults on it) */
    o->imm = (int32_t)imm64;
    if (k >= K_LB && k <= K_LDR && rt == 0)
        k = K_UNSUPPORTED; /* a load into r0: rare, step() */
    if (k >= K_ADDIU && k <= K_LI && rt == 0)
        k = K_NOP;
    return k;
}

static __attribute__((noinline)) blk *blk_build(n64_machine *m, struct n64_blocks *bc, const void *const *h,
                                                uint64_t pc, uint32_t pa, uint32_t fc, bool mapped, uint32_t maxops)
{
    uint32_t limit = (0x1000u - (pa & 0xFFFu)) / 4;
    if (limit > BLK_MAX)
        limit = BLK_MAX;
    size_t need = sizeof(blk) + (limit + 1) * sizeof(bop) + 2 * limit * 4 + 32;
    if (bc->used + need > BLK_ARENA)
        return NULL;
    blk *b = (blk *)(void *)(bc->arena + bc->used);
    const uint8_t *src = m->rdram + pa;
    uint32_t n = 0, branch = 0, seen = 1, ex[BLK_MAX], kind[BLK_MAX];
    for (uint32_t i = 0; i < limit && i < maxops; i++) {
        uint64_t ipc = pc + 4 * i;
        uint32_t k = blk_decode(n64_be32(src + 4 * i), ipc, &b->ops[i], &ex[i]);
        kind[i] = k;
        seen = i + 1;
        if (k == K_UNSUPPORTED)
            break;
        if (K_IS_BRANCH(k)) {
            /* With its delay slot in the block: an op, not a branch, in the page. */
            if (i + 1 >= limit)
                break;
            seen = i + 2;
            uint32_t dk = blk_decode(n64_be32(src + 4 * (i + 1)), ipc + 4, &b->ops[i + 1], &ex[i + 1]);
            if (dk == K_UNSUPPORTED || K_IS_BRANCH(dk))
                break;
            kind[i + 1] = dk;
            n = i + 2;
            branch = 1;
            break;
        }
        n = i + 1;
    }
    for (uint32_t i = 0; i < n; i++)
        b->ops[i].h = h[kind[i]];
    memset(&b->ops[n], 0, sizeof(bop));
    b->ops[n].h = h[K_END];
    uint32_t *words = (uint32_t *)(void *)&b->ops[n + 1];
    uint32_t *cum = words + seen;
    memcpy(words, src, (size_t)seen * 4);
    for (uint32_t i = 0, t = 0; i < n; i++) {
        cum[i] = t;
        t += fc + ex[i];
    }
    b->vpc = pc;
    b->fall = pc + 4 * n;
    b->pa = pa;
    b->page = pa >> N64_PAGE_SHIFT;
    b->gen = m->rdram_gen[b->page];
    b->tlb_gen = m->cpu.tlb_gen;
    b->len4 = 4 * n;
    b->cmp4 = 4 * seen;
    b->maxcost = n ? cum[n - 1] : 0;
    b->fcsum = fc * n;
    b->n = (uint16_t)n;
    b->fc = (uint8_t)fc;
    b->branch = (uint8_t)branch;
    b->mapped = mapped;
    b->nolink = mapped;
    b->erl = (m->cpu.cop0[C0_STATUS] & SR_ERL) != 0;
    b->lastpc = pc + 4 * (n ? n - 1 : 0);
    b->succ[0] = b->succ[1] = NULL;
    b->rec = 0;
    b->idle = n == 2 && branch && kind[0] != K_JR && kind[0] != K_BC1F && kind[0] != K_BC1T && !b->ops[0].d &&
              IMM64(&b->ops[0]) == pc && kind[1] == K_NOP;
#ifdef N64_BLK_NO_IDLE /* measurements of the blocks alone */
    b->idle = 0;
#endif
    b->words = words;
    b->cum = cum;
    bc->used += (sizeof(blk) + (size_t)(n + 1) * sizeof(bop) + (size_t)(seen + n) * 4 + 7) & ~(size_t)7;
    blk **slot = &bc->tab[blk_index(pc)];
    if (*slot)
        (*slot)->nolink = 1; /* replaced: links to it now go through the table */
    *slot = b;
    return b;
}

/* Whether b holds at most maxops ops, not counting the delay slot of a branch that is op maxops - 1 (BLK_MAX: any). */
static inline bool blk_fits(const blk *b, uint32_t maxops)
{
    return maxops >= BLK_MAX || b->n <= maxops || (b->branch && b->n == maxops + 1u);
}

/* The block at pc (kernel mode), decoded when missing, checked against its page counter and its translation. NULL or
 * n = 0: none (step() runs the instruction). h: run_blocks' handlers. maxops: blk_fits (a block that does not is
 * decoded again, shorter). */
static __attribute__((noinline)) blk *blk_get_slow(n64_machine *m, uint64_t pc, const void *const *h, uint32_t maxops)
{
    struct n64_blocks *bc = m->blocks;
    if (!bc) {
        bc = m->blocks = calloc(1, sizeof *bc);
        if (bc && !(bc->arena = malloc(BLK_ARENA))) {
            free(bc);
            bc = m->blocks = NULL;
        }
        if (!bc)
            return NULL;
    }
    if ((pc & 3) || pc != (uint64_t)(int64_t)(int32_t)pc)
        return NULL;
    uint32_t a = (uint32_t)pc, pa, fc = COST_CACHED;
    bool mapped = false;
    if (a - 0x80000000u < 0x20000000u) {
        pa = a - 0x80000000u;
    } else if (a - 0xA0000000u < 0x20000000u) {
        pa = a - 0xA0000000u;
        fc = COST_UNCACHED_RAM;
    } else {
        if (!vtop_slow(m, pc, ACC_FETCH, &pa, false))
            return NULL; /* step() raises the TLB / address exception */
        mapped = true;
    }
    if (pa >= m->rdram_size)
        return NULL;
    blk *b = bc->tab[blk_index(pc)];
    if (b && b->vpc == pc && b->pa == pa && b->fc == fc && blk_fits(b, maxops) &&
        !memcmp(m->rdram + pa, b->words, b->cmp4)) {
        b->gen = m->rdram_gen[b->page]; /* the page was written, not this code; or the TLB, not this mapping */
        b->tlb_gen = m->cpu.tlb_gen;
        b->erl = (m->cpu.cop0[C0_STATUS] & SR_ERL) != 0;
        return b;
    }
    b = blk_build(m, bc, h, pc, pa, fc, mapped, maxops);
    if (!b) {
        blocks_flush(bc);
        b = blk_build(m, bc, h, pc, pa, fc, mapped, maxops);
    }
    return b;
}

static N64_HOT_INLINE blk *blk_get(n64_machine *m, uint64_t pc, const void *const *h, uint32_t maxops)
{
    struct n64_blocks *bc = m->blocks;
    if (N64_LIKELY(bc != NULL)) {
        blk *b = bc->tab[blk_index(pc)];
        if (N64_LIKELY(b && b->vpc == pc && m->rdram_gen[b->page] == b->gen &&
                       (!b->mapped || (b->tlb_gen == m->cpu.tlb_gen &&
                                       b->erl == ((m->cpu.cop0[C0_STATUS] & SR_ERL) != 0))) &&
                       blk_fits(b, maxops)))
            return b;
    }
    return blk_get_slow(m, pc, h, maxops);
}

/* A block's data access outside the inline KSEG0 / KSEG1 RDRAM paths (mapped addresses, the bus): the interpreter's
 * own path, with the clock where step() has it (now). Return -1 with nothing done when the access would raise an
 * exception (step() then runs the instruction), 0 for RDRAM (*pa: where), 1 for the bus (the block stops after it). */
static __attribute__((noinline)) int blk_load(n64_machine *m, uint64_t va, int size, bool partial, uint64_t *out,
                                              uint64_t now)
{
    uint32_t pa;
    if (!vtop(m, va, ACC_LOAD, &pa, false))
        return -1;
    m->now = now;
    if (partial)
        rd_partial_x(m, va, size, out, false);
    else
        rd_x(m, va, size, out, false);
    return (pa & ~(uint32_t)(size - 1)) >= m->rdram_size;
}

static __attribute__((noinline)) int blk_store(n64_machine *m, uint64_t va, int size, uint64_t v, uint64_t mask,
                                               bool partial, uint64_t now, uint32_t *pa_out)
{
    uint32_t pa;
    if (!vtop(m, va, ACC_STORE, &pa, false))
        return -1;
    m->now = now;
    if (partial)
        store_masked_x(m, va, v, mask, size, false);
    else
        wr_x(m, va, size, v, false);
    pa &= ~(uint32_t)(size - 1);
    *pa_out = pa;
    return pa >= m->rdram_size;
}

/* COP1 through n64_fpu_cop1 with cur / ds / the clock as step() has them. True when it took an exception (pc, npc and
 * COP0 then hold the exception's entry). */
static __attribute__((noinline)) bool blk_cop1(n64_machine *m, uint32_t ins, uint64_t pc, bool ds, uint64_t now,
                                               uint64_t ran)
{
    n64_cpu *c = &m->cpu;
    m->now = now;
    m->instructions = ran;
    c->cur = pc;
    c->ds = ds;
    c->pc = BLK_NO_PC;
    n64_fpu_cop1(m, ins);
    c->gpr[0] = 0;
    return c->pc != BLK_NO_PC;
}

enum { BLK_STOPPED, BLK_INTERPRET, BLK_SLOW, BLK_NEAR, BLK_LEAVE };

/* After a bus access by op i of b, the clock at now: whether the block must stop after that op (SLOW_DONE). */
static inline bool bus_moved(n64_machine *m, const blk *b, uint32_t i, uint64_t now, uint64_t stop)
{
    return m->cpu.irq || m->stop_at != stop || (m->mi_mode & 0x80) || m->rdram_gen[b->page] != b->gen ||
           (i + 1 < b->n && now + (b->maxcost - b->cum[i + 1]) >= stop);
}

static inline uint64_t be_load(const uint8_t *p, int size)
{
    switch (size) {
    case 1: return p[0];
    case 2: return n64_be16(p);
    case 4: return n64_be32(p);
    default: return be64(p);
    }
}

static inline void be_store(uint8_t *p, int size, uint64_t v)
{
    switch (size) {
    case 1: p[0] = (uint8_t)v; break;
    case 2: n64_wbe16(p, (uint16_t)v); break;
    case 4: n64_wbe32(p, (uint32_t)v); break;
    default: wbe64(p, v); break;
    }
}

/* Runs blocks one after the other from pc (kernel mode, not in a delay slot, MI repeat mode off, no interrupt pending,
 * now < stop_at). Returns BLK_STOPPED where the run loop must check again (every field of the machine as step()'s loop
 * leaves it), BLK_INTERPRET when the instruction at pc must be stepped (no block there), BLK_SLOW when step() must run
 * it (an op that falls back: the machine holds the state in front of it), BLK_NEAR when stop_at is close: the run loop
 * steps to it. one (n64_cpu_run_block): only the block at pc, of at most maxops ops (blk_fits), and back to the caller
 * after it (an idle loop still fast-forwards). */
static __attribute__((noinline)) int run_blocks(n64_machine *m, bool one, uint32_t maxops, const n64_blk_region *rg,
                                                bool mark)
{
    n64_cpu *const c = &m->cpu;
    uint64_t *const r = c->gpr;
    uint8_t *const rdram = m->rdram;
    const uint64_t rdsize = m->rdram_size;
    const uint32_t sr = (uint32_t)c->cop0[C0_STATUS];
    uint64_t pc = c->pc, npc, now = m->now, stop = m->stop_at, ran = m->instructions;
    const blk *b;
    const bop *o, *ob;
    uint64_t btgt, va, off, v;
    uint32_t pa;
    int brk, ret, nds = 0;
    bop part[BLK_MAX + 1]; /* a block's first ops and a K_END: a run cut short in front of stop_at */
    static const void *const op_label[K_CUT + 1] = {
        [K_NOP] = &&l_nop, [K_ADDU] = &&l_addu, [K_SUBU] = &&l_subu, [K_AND] = &&l_and, [K_OR] = &&l_or,
        [K_XOR] = &&l_xor, [K_NOR] = &&l_nor, [K_SLT] = &&l_slt, [K_SLTU] = &&l_sltu, [K_DADDU] = &&l_daddu,
        [K_DSUBU] = &&l_dsubu, [K_SLLV] = &&l_sllv, [K_SRLV] = &&l_srlv, [K_SRAV] = &&l_srav, [K_DSLLV] = &&l_dsllv,
        [K_DSRLV] = &&l_dsrlv, [K_DSRAV] = &&l_dsrav, [K_ADD] = &&l_add, [K_SUB] = &&l_sub, [K_DADD] = &&l_dadd,
        [K_DSUB] = &&l_dsub, [K_SLL] = &&l_sll, [K_SRL] = &&l_srl, [K_SRA] = &&l_sra, [K_DSLL] = &&l_dsll,
        [K_DSRL] = &&l_dsrl, [K_DSRA] = &&l_dsra, [K_ADDIU] = &&l_addiu, [K_DADDIU] = &&l_daddiu,
        [K_SLTI] = &&l_slti, [K_SLTIU] = &&l_sltiu, [K_ANDI] = &&l_andi, [K_ORI] = &&l_ori, [K_XORI] = &&l_xori,
        [K_LI] = &&l_li, [K_ADDI] = &&l_addi, [K_DADDI] = &&l_daddi, [K_MFHI] = &&l_mfhi, [K_MFLO] = &&l_mflo,
        [K_MTHI] = &&l_mthi, [K_MTLO] = &&l_mtlo, [K_MULT] = &&l_mult, [K_MULTU] = &&l_multu, [K_DIV] = &&l_div,
        [K_DIVU] = &&l_divu, [K_DMULT] = &&l_dmult, [K_DMULTU] = &&l_dmultu, [K_DDIV] = &&l_ddiv,
        [K_DDIVU] = &&l_ddivu, [K_LB] = &&l_lb, [K_LBU] = &&l_lbu, [K_LH] = &&l_lh, [K_LHU] = &&l_lhu,
        [K_LW] = &&l_lw, [K_LWU] = &&l_lwu, [K_LD] = &&l_ld, [K_LWL] = &&l_lwl, [K_LWR] = &&l_lwr,
        [K_LDL] = &&l_ldl, [K_LDR] = &&l_ldr, [K_SB] = &&l_sb, [K_SH] = &&l_sh, [K_SW] = &&l_sw, [K_SD] = &&l_sd,
        [K_SWL] = &&l_swl, [K_SWR] = &&l_swr, [K_SDL] = &&l_sdl, [K_SDR] = &&l_sdr, [K_LWC1] = &&l_lwc1,
        [K_LDC1] = &&l_ldc1, [K_SWC1] = &&l_swc1, [K_SDC1] = &&l_sdc1, [K_COP1] = &&l_cop1, [K_MFC0] = &&l_mfc0,
        [K_MTC0] = &&l_mtc0, [K_CACHE] = &&l_cache, [K_BEQ] = &&l_beq,
        [K_BNE] = &&l_bne, [K_BLEZ] = &&l_blez, [K_BGTZ] = &&l_bgtz, [K_BLTZ] = &&l_bltz, [K_BGEZ] = &&l_bgez,
        [K_BC1F] = &&l_bc1f, [K_BC1T] = &&l_bc1t, [K_J] = &&l_j, [K_JR] = &&l_jr, [K_END] = &&l_end,
        [K_CUT] = &&l_cut,
    };

/* The next op (the K_END after the last one that runs ends the block). */
#define NEXT() goto *(++o)->h
/* After an op that may stop the block (a bus access, a store into its own words). */
#define NEXT_MEM()                                                                                                     \
    do {                                                                                                               \
        if (N64_UNLIKELY(brk))                                                                                         \
            goto stop_after;                                                                                           \
        NEXT();                                                                                                        \
    } while (0)
#define IDX() ((uint32_t)(o - ob))
/* The fetch ticks of the ops after o, added at entry: the clock step() has during o is now - REST(). */
#define REST() ((uint64_t)b->fc * (b->n - IDX() - 1u))
#define IN_DS() (b->branch && IDX() == b->n - 1u)
/* After a slow access: -1 falls back. 1, the bus: the block stops after this op if the access raised the interrupt
 * line, moved stop_at (an event, a DMA's end, the RSP started), switched MI repeat mode on, wrote into the block's page
 * (a DMA) or took so long (its bus cost) that the ops left may no longer all start before stop_at; otherwise nothing
 * the block relies on moved and it goes on. */
#define SLOW_DONE(res)                                                                                                 \
    do {                                                                                                               \
        int res_ = (res);                                                                                              \
        if (res_ < 0)                                                                                                  \
            goto bail;                                                                                                 \
        now = m->now + REST();                                                                                         \
        if (res_ > 0 && N64_UNLIKELY(bus_moved(m, b, IDX(), now - REST(), stop))) {                                    \
            stop = m->stop_at;                                                                                         \
            brk = 1;                                                                                                   \
        }                                                                                                              \
    } while (0)
/* An aligned load of size bytes at s + imm into v (rd_x). */
#define BLOAD(size)                                                                                                    \
    do {                                                                                                               \
        va = r[o->s] + IMM64(o);                                                                                       \
        if (N64_UNLIKELY(va & (uint64_t)((size) - 1)))                                                                 \
            goto bail;                                                                                                 \
        off = va - 0xFFFFFFFF80000000ull;                                                                              \
        if (N64_LIKELY(off < rdsize)) {                                                                                \
            v = be_load(rdram + off, size);                                                                            \
        } else if (off - 0x20000000u < rdsize) {                                                                       \
            v = be_load(rdram + (off - 0x20000000u), size);                                                            \
            now += COST_DATA_UNCACHED;                                                                                 \
        } else {                                                                                                       \
            SLOW_DONE(blk_load(m, va, size, false, &v, now - REST()));                                                 \
        }                                                                                                              \
    } while (0)
/* The word / doubleword a partial load reads (rd_partial_x: the aligned one, no uncached cost). */
#define BLOAD_PARTIAL(size)                                                                                            \
    do {                                                                                                               \
        va = r[o->s] + IMM64(o);                                                                                       \
        off = va - 0xFFFFFFFF80000000ull;                                                                              \
        if (N64_LIKELY(off < 0x40000000u) && (pa = ((uint32_t)off & 0x1FFFFFFFu & ~(uint32_t)((size) - 1))) < rdsize) \
            v = be_load(rdram + pa, size);                                                                             \
        else                                                                                                           \
            SLOW_DONE(blk_load(m, va, size, true, &v, now - REST()));                                                  \
    } while (0)
/* A store into the running block's words stops it after this op (the next lookup sees the new words). */
#define WROTE_CHECK(pa_, size)                                                                                         \
    do {                                                                                                               \
        if (N64_UNLIKELY((pa_) + (size) - 1 - b->pa < b->len4 + (size) - 1))                                           \
            brk = 1;                                                                                                   \
    } while (0)
/* RDRAM at pa was written inline: its page counter (n64_rdram_wrote_aligned without the lifted range: no lifted
 * function runs while the interpreter does, rc_len is 0), the check above. */
#define WROTE(pa_, size)                                                                                               \
    do {                                                                                                               \
        m->rdram_gen[(pa_) >> N64_PAGE_SHIFT]++;                                                                       \
        WROTE_CHECK(pa_, size);                                                                                        \
    } while (0)
/* An aligned store of size bytes of val at s + imm (wr_x: no uncached cost). */
#define BSTORE(size, val)                                                                                              \
    do {                                                                                                               \
        va = r[o->s] + IMM64(o);                                                                                       \
        v = (val);                                                                                                     \
        if (N64_UNLIKELY(va & (uint64_t)((size) - 1)))                                                                 \
            goto bail;                                                                                                 \
        off = va - 0xFFFFFFFF80000000ull;                                                                              \
        if (N64_LIKELY(off < 0x40000000u) && (pa = (uint32_t)off & 0x1FFFFFFFu) < rdsize) {                            \
            be_store(rdram + pa, size, v);                                                                             \
            WROTE(pa, size);                                                                                           \
        } else {                                                                                                       \
            int res2_ = blk_store(m, va, size, v, 0, false, now - REST(), &pa);                                        \
            SLOW_DONE(res2_);                                                                                          \
            if (res2_ == 0)                                                                                            \
                WROTE_CHECK(pa, size);                                                                                 \
        }                                                                                                              \
    } while (0)
/* A partial store (store_masked_x): data and mask left-aligned in 64 bits, size 4 uses the upper word. */
#define BSTORE_MASKED(size, data, mask)                                                                                \
    do {                                                                                                               \
        off = va - 0xFFFFFFFF80000000ull;                                                                              \
        if (N64_LIKELY(off < 0x40000000u) && (pa = ((uint32_t)off & 0x1FFFFFFFu & ~(uint32_t)((size) - 1))) < rdsize) {\
            uint8_t *q_ = rdram + pa;                                                                                  \
            for (int i_ = 0; i_ < (size); i_++) {                                                                      \
                unsigned sh_ = 56 - 8 * (unsigned)i_;                                                                  \
                if (((mask) >> sh_) & 0xFF)                                                                            \
                    q_[i_] = (uint8_t)((data) >> sh_);                                                                 \
            }                                                                                                          \
            WROTE(pa, size);                                                                                           \
        } else {                                                                                                       \
            int res2_ = blk_store(m, va, size, data, mask, true, now - REST(), &pa);                                   \
            SLOW_DONE(res2_);                                                                                          \
            if (res2_ == 0)                                                                                            \
                WROTE_CHECK(pa, size);                                                                                 \
        }                                                                                                              \
    } while (0)
#define BRANCH(cond)                                                                                                   \
    do {                                                                                                               \
        if (cond)                                                                                                      \
            btgt = IMM64(o);                                                                                           \
        else if (o->likely)                                                                                            \
            goto likely_skip;                                                                                          \
    } while (0)

    b = blk_get(m, pc, op_label, maxops);
    if (N64_UNLIKELY(mark) && b)
        ((blk *)b)->rec = 1;
have_block:
    if (!b || !b->n) {
        ret = BLK_INTERPRET;
        goto out;
    }
again:
    ob = b->ops;
    btgt = b->fall;
    brk = 0;
    if (N64_UNLIKELY(now + b->maxcost >= stop)) {
        /* Only the ops sure to start before stop_at (the loop checked the first: now < stop_at), from a copy ending
         * with a K_END. */
        uint32_t k = 1;
        while (k < b->n && now + b->cum[k] < stop)
            k++;
        memcpy(part, b->ops, k * sizeof(bop));
        memset(&part[k], 0, sizeof(bop));
        part[k].h = &&l_cut;
        ob = part;
    }
    now += b->fcsum;
    o = ob;
    goto *o->h;

l_nop: NEXT();
l_addu: r[o->d] = sx32((uint32_t)r[o->s] + (uint32_t)r[o->t]); NEXT();
l_subu: r[o->d] = sx32((uint32_t)r[o->s] - (uint32_t)r[o->t]); NEXT();
l_and: r[o->d] = r[o->s] & r[o->t]; NEXT();
l_or: r[o->d] = r[o->s] | r[o->t]; NEXT();
l_xor: r[o->d] = r[o->s] ^ r[o->t]; NEXT();
l_nor: r[o->d] = ~(r[o->s] | r[o->t]); NEXT();
l_slt: r[o->d] = (int64_t)r[o->s] < (int64_t)r[o->t]; NEXT();
l_sltu: r[o->d] = r[o->s] < r[o->t]; NEXT();
l_daddu: r[o->d] = r[o->s] + r[o->t]; NEXT();
l_dsubu: r[o->d] = r[o->s] - r[o->t]; NEXT();
l_sllv: r[o->d] = sx32((uint32_t)r[o->t] << (r[o->s] & 31)); NEXT();
l_srlv: r[o->d] = sx32((uint32_t)r[o->t] >> (r[o->s] & 31)); NEXT();
l_srav: r[o->d] = sx32((uint64_t)((int64_t)r[o->t] >> (r[o->s] & 31))); NEXT();
l_dsllv: r[o->d] = r[o->t] << (r[o->s] & 63); NEXT();
l_dsrlv: r[o->d] = r[o->t] >> (r[o->s] & 63); NEXT();
l_dsrav: r[o->d] = (uint64_t)((int64_t)r[o->t] >> (r[o->s] & 63)); NEXT();
l_add: {
    int32_t x;
    if (__builtin_add_overflow((int32_t)r[o->s], (int32_t)r[o->t], &x))
        goto bail;
    r[o->d] = (uint64_t)(int64_t)x;
    NEXT();
}
l_sub: {
    int32_t x;
    if (__builtin_sub_overflow((int32_t)r[o->s], (int32_t)r[o->t], &x))
        goto bail;
    r[o->d] = (uint64_t)(int64_t)x;
    NEXT();
}
l_dadd: {
    int64_t x;
    if (__builtin_add_overflow((int64_t)r[o->s], (int64_t)r[o->t], &x))
        goto bail;
    r[o->d] = (uint64_t)x;
    NEXT();
}
l_dsub: {
    int64_t x;
    if (__builtin_sub_overflow((int64_t)r[o->s], (int64_t)r[o->t], &x))
        goto bail;
    r[o->d] = (uint64_t)x;
    NEXT();
}
l_sll: r[o->d] = sx32((uint32_t)r[o->t] << IMM64(o)); NEXT();
l_srl: r[o->d] = sx32((uint32_t)r[o->t] >> IMM64(o)); NEXT();
l_sra: r[o->d] = sx32((uint64_t)((int64_t)r[o->t] >> IMM64(o))); NEXT();
l_dsll: r[o->d] = r[o->t] << IMM64(o); NEXT();
l_dsrl: r[o->d] = r[o->t] >> IMM64(o); NEXT();
l_dsra: r[o->d] = (uint64_t)((int64_t)r[o->t] >> IMM64(o)); NEXT();
l_addiu: r[o->t] = sx32((uint32_t)r[o->s] + (uint32_t)IMM64(o)); NEXT();
l_daddiu: r[o->t] = r[o->s] + IMM64(o); NEXT();
l_slti: r[o->t] = (int64_t)r[o->s] < (int64_t)IMM64(o); NEXT();
l_sltiu: r[o->t] = r[o->s] < IMM64(o); NEXT();
l_andi: r[o->t] = r[o->s] & IMM64(o); NEXT();
l_ori: r[o->t] = r[o->s] | IMM64(o); NEXT();
l_xori: r[o->t] = r[o->s] ^ IMM64(o); NEXT();
l_li: r[o->t] = IMM64(o); NEXT();
l_addi: {
    int32_t x;
    if (__builtin_add_overflow((int32_t)r[o->s], (int32_t)IMM64(o), &x))
        goto bail;
    r[o->t] = (uint64_t)(int64_t)x;
    NEXT();
}
l_daddi: {
    int64_t x;
    if (__builtin_add_overflow((int64_t)r[o->s], (int64_t)IMM64(o), &x))
        goto bail;
    r[o->t] = (uint64_t)x;
    NEXT();
}
l_mfhi: r[o->d] = c->hi; NEXT();
l_mflo: r[o->d] = c->lo; NEXT();
l_mthi: c->hi = r[o->s]; NEXT();
l_mtlo: c->lo = r[o->s]; NEXT();
l_mult: {
    int64_t p = (int64_t)(int32_t)r[o->s] * (int32_t)r[o->t];
    c->lo = sx32((uint64_t)p);
    c->hi = sx32((uint64_t)p >> 32);
    now += COST_MULT;
    NEXT();
}
l_multu: {
    uint64_t p = (uint64_t)(uint32_t)r[o->s] * (uint32_t)r[o->t];
    c->lo = sx32(p);
    c->hi = sx32(p >> 32);
    now += COST_MULT;
    NEXT();
}
l_div: {
    int32_t a = (int32_t)r[o->s], d = (int32_t)r[o->t];
    if (d == 0) {
        c->lo = a < 0 ? 1 : ~0ull;
        c->hi = sx32((uint32_t)a);
    } else if (a == INT32_MIN && d == -1) {
        c->lo = sx32(0x80000000u);
        c->hi = 0;
    } else {
        c->lo = sx32((uint32_t)(a / d));
        c->hi = sx32((uint32_t)(a % d));
    }
    now += COST_DIV;
    NEXT();
}
l_divu: {
    uint32_t a = (uint32_t)r[o->s], d = (uint32_t)r[o->t];
    if (d == 0) {
        c->lo = ~0ull;
        c->hi = sx32(a);
    } else {
        c->lo = sx32(a / d);
        c->hi = sx32(a % d);
    }
    now += COST_DIV;
    NEXT();
}
l_dmult: {
    uint64_t a = r[o->s], d = r[o->t], hi, lo;
    mulu64(a, d, &hi, &lo);
    if ((int64_t)a < 0)
        hi -= d;
    if ((int64_t)d < 0)
        hi -= a;
    c->hi = hi;
    c->lo = lo;
    now += COST_DMULT;
    NEXT();
}
l_dmultu:
    mulu64(r[o->s], r[o->t], &c->hi, &c->lo);
    now += COST_DMULT;
    NEXT();
l_ddiv: {
    int64_t a = (int64_t)r[o->s], d = (int64_t)r[o->t];
    if (d == 0) {
        c->lo = a < 0 ? 1 : ~0ull;
        c->hi = (uint64_t)a;
    } else if (a == INT64_MIN && d == -1) {
        c->lo = (uint64_t)a;
        c->hi = 0;
    } else {
        c->lo = (uint64_t)(a / d);
        c->hi = (uint64_t)(a % d);
    }
    now += COST_DDIV;
    NEXT();
}
l_ddivu: {
    uint64_t a = r[o->s], d = r[o->t];
    if (d == 0) {
        c->lo = ~0ull;
        c->hi = a;
    } else {
        c->lo = a / d;
        c->hi = a % d;
    }
    now += COST_DDIV;
    NEXT();
}
l_lb: BLOAD(1); r[o->t] = (uint64_t)(int64_t)(int8_t)v; NEXT_MEM();
l_lbu: BLOAD(1); r[o->t] = v; NEXT_MEM();
l_lh: BLOAD(2); r[o->t] = (uint64_t)(int64_t)(int16_t)v; NEXT_MEM();
l_lhu: BLOAD(2); r[o->t] = v; NEXT_MEM();
l_lw: BLOAD(4); r[o->t] = sx32(v); NEXT_MEM();
l_lwu: BLOAD(4); r[o->t] = v; NEXT_MEM();
l_ld: BLOAD(8); r[o->t] = v; NEXT_MEM();
l_lwl: {
    BLOAD_PARTIAL(4);
    unsigned sh = 8 * (unsigned)(va & 3);
    uint32_t mask = 0xFFFFFFFFu << sh;
    r[o->t] = sx32(((uint32_t)r[o->t] & ~mask) | ((uint32_t)v << sh)); /* LWL always loads bit 31 */
    NEXT_MEM();
}
l_lwr: {
    BLOAD_PARTIAL(4);
    unsigned sh = 8 * (unsigned)((va ^ 3) & 3);
    uint32_t mask = 0xFFFFFFFFu >> sh;
    uint32_t x = ((uint32_t)r[o->t] & ~mask) | ((uint32_t)v >> sh);
    r[o->t] = (va & 3) == 3 ? sx32(x) : (r[o->t] & 0xFFFFFFFF00000000ull) | x;
    NEXT_MEM();
}
l_ldl: {
    BLOAD_PARTIAL(8);
    unsigned sh = 8 * (unsigned)(va & 7);
    uint64_t mask = ~0ull << sh;
    r[o->t] = (r[o->t] & ~mask) | (v << sh);
    NEXT_MEM();
}
l_ldr: {
    BLOAD_PARTIAL(8);
    unsigned sh = 8 * (unsigned)((va ^ 7) & 7);
    uint64_t mask = ~0ull >> sh;
    r[o->t] = (r[o->t] & ~mask) | (v >> sh);
    NEXT_MEM();
}
l_sb: BSTORE(1, r[o->t]); NEXT_MEM();
l_sh: BSTORE(2, r[o->t]); NEXT_MEM();
l_sw: BSTORE(4, r[o->t]); NEXT_MEM();
l_sd: BSTORE(8, r[o->t]); NEXT_MEM();
l_swl: {
    va = r[o->s] + IMM64(o);
    unsigned sh = 8 * (unsigned)(va & 3);
    uint64_t mask = (uint64_t)(0xFFFFFFFFu >> sh) << 32, data = (uint64_t)((uint32_t)r[o->t] >> sh) << 32;
    BSTORE_MASKED(4, data, mask);
    NEXT_MEM();
}
l_swr: {
    va = r[o->s] + IMM64(o);
    unsigned sh = 8 * (unsigned)((va ^ 3) & 3);
    uint64_t mask = (uint64_t)(0xFFFFFFFFu << sh) << 32, data = (uint64_t)((uint32_t)r[o->t] << sh) << 32;
    BSTORE_MASKED(4, data, mask);
    NEXT_MEM();
}
l_sdl: {
    va = r[o->s] + IMM64(o);
    unsigned sh = 8 * (unsigned)(va & 7);
    uint64_t mask = ~0ull >> sh, data = r[o->t] >> sh;
    BSTORE_MASKED(8, data, mask);
    NEXT_MEM();
}
l_sdr: {
    va = r[o->s] + IMM64(o);
    unsigned sh = 8 * (unsigned)((va ^ 7) & 7);
    uint64_t mask = ~0ull << sh, data = r[o->t] << sh;
    BSTORE_MASKED(8, data, mask);
    NEXT_MEM();
}
l_lwc1:
    if (!(sr & SR_CU1))
        goto bail;
    BLOAD(4);
    n64_fpu_write_s(c, o->t, (uint32_t)v);
    NEXT_MEM();
l_ldc1:
    if (!(sr & SR_CU1))
        goto bail;
    BLOAD(8);
    n64_fpu_write_d(c, o->t, v);
    NEXT_MEM();
l_swc1:
    if (!(sr & SR_CU1))
        goto bail;
    BSTORE(4, n64_fpu_read_s(c, o->t));
    NEXT_MEM();
l_sdc1:
    if (!(sr & SR_CU1))
        goto bail;
    BSTORE(8, n64_fpu_read_d(c, o->t));
    NEXT_MEM();
l_cop1:
    if (!(sr & SR_CU1))
        goto bail;
    if (blk_cop1(m, (uint32_t)IMM64(o), b->vpc + 4 * IDX(), IN_DS(), now - REST(), ran + IDX())) {
        ran += IDX() + 1; /* an exception: the machine is where step() leaves it */
        m->instructions = ran;
        return BLK_STOPPED;
    }
    now = m->now + REST();
    NEXT();
l_mfc0:
    /* cop0_read: COUNT from the clock, RANDOM from the instruction count, as step() has them */
    m->now = now - REST();
    m->instructions = ran + IDX();
    v = cop0_read(m, o->d);
    r[o->t] = o->likely ? v : sx32(v);
    NEXT();
l_mtc0:
    /* May move the interrupt line, stop_at (COMPARE), the mode (STATUS), the TLB generation (EntryHi): the block
     * stops after it. */
    m->now = now - REST();
    m->instructions = ran + IDX();
    cop0_write(m, o->d, o->likely ? r[o->t] : sx32(r[o->t]));
    now = m->now + REST();
    stop = m->stop_at;
    brk = 1;
    NEXT_MEM();
l_cache:
    if (!cache_op(m, o->t, r[o->s] + IMM64(o), false))
        goto bail; /* a TLB exception: step() raises it */
    NEXT();
l_beq: BRANCH(r[o->s] == r[o->t]); NEXT();
l_bne: BRANCH(r[o->s] != r[o->t]); NEXT();
l_blez: BRANCH((int64_t)r[o->s] <= 0); NEXT();
l_bgtz: BRANCH((int64_t)r[o->s] > 0); NEXT();
l_bltz: {
    bool t = (int64_t)r[o->s] < 0;
    if (o->d)
        r[31] = b->fall; /* BLTZAL & co. link whether taken or not */
    BRANCH(t);
    NEXT();
}
l_bgez: {
    bool t = (int64_t)r[o->s] >= 0;
    if (o->d)
        r[31] = b->fall;
    BRANCH(t);
    NEXT();
}
l_bc1f:
    if (!(sr & SR_CU1))
        goto bail;
    BRANCH(!(c->fcsr & FCSR_C));
    NEXT();
l_bc1t:
    if (!(sr & SR_CU1))
        goto bail;
    BRANCH(c->fcsr & FCSR_C);
    NEXT();
l_j:
    btgt = IMM64(o);
    if (o->d)
        r[o->d] = b->fall;
    NEXT();
l_jr:
    btgt = r[o->s];
    if (o->d)
        r[o->d] = b->fall;
    NEXT();


l_end:
    /* The whole block ran: on to its branch's target or its fall-through. */
    ran += b->n;
    pc = btgt;
    c->cur = b->lastpc;
    c->ds = b->branch;
    if (N64_UNLIKELY(now >= stop)) {
        ret = BLK_STOPPED;
        goto out;
    }
    if (pc == b->vpc) {
        if (N64_UNLIKELY(b->idle)) {
            /* A branch to itself over a NOP: nothing but the clock and the count move until stop_at. Every op that
             * starts before it runs (fc ticks each), as step() would run them. */
            uint64_t nops = (stop - now + b->fc - 1) / b->fc;
            now += nops * b->fc;
            ran += nops;
            if (nops & 1) { /* ends after the branch, its delay slot next */
                c->cur = b->vpc;
                c->ds = false;
                pc = b->vpc + 4;
                npc = b->vpc;
                nds = 1;
                ret = BLK_STOPPED;
                goto out_npc;
            }
            ret = BLK_STOPPED;
            goto out;
        }
        if (N64_UNLIKELY(one)) {
            ret = BLK_STOPPED;
            goto out;
        }
        goto again; /* a loop on itself: its words and translation unchanged (no store into them, no TLB write) */
    }
    if (N64_UNLIKELY(one)) {
        ret = BLK_STOPPED;
        goto out;
    }
    goto link;
l_cut:
    /* A run cut short in front of stop_at: the state after op IDX() - 1. */
    goto cut;
stop_after:
    /* The op at o may have changed what the run loop checks (the bus) or written the block's own words. */
    ++o;
    if (IDX() == b->n) {
        ran += b->n;
        c->cur = b->vpc + 4 * (b->n - 1u);
        c->ds = b->branch;
        pc = btgt;
        ret = BLK_STOPPED;
        goto out;
    }
cut: {
    uint32_t k = IDX();
    now -= (uint64_t)b->fc * (b->n - k);
    ran += k;
    c->cur = b->vpc + 4 * (k - 1);
    c->ds = false;
    pc = b->vpc + 4 * k;
    if (b->branch && k == b->n - 1u) { /* stopped between the branch and its delay slot */
        npc = btgt;
        nds = 1;
        ret = BLK_STOPPED;
        goto out_npc;
    }
    ret = !brk && now < stop ? BLK_NEAR : BLK_STOPPED;
    goto out;
}

likely_skip:
    /* The likely branch at o not taken: its delay slot (the block's last op) is nullified, neither fetched nor
     * counted; step() leaves pc after it. */
    now -= b->fc;
    ran += b->n - 1;
    c->cur = b->vpc + 4 * IDX();
    c->ds = false;
    pc = b->fall;
    if (now >= stop || N64_UNLIKELY(one)) {
        ret = BLK_STOPPED;
        goto out;
    }
    if (pc == b->vpc)
        goto again; /* a loop on itself: its words and translation unchanged (no store into them, no TLB write) */
link: {
    /* The next block through b's link when it still holds, else the table (and the link remembers it). */
    unsigned w = pc != b->fall;
    if (N64_UNLIKELY(rg != NULL)) {
        /* n64_cpu_run_region: only in the pages it marks (KSEG0 / KSEG1 RDRAM) */
        uint64_t off = pc - 0xFFFFFFFF80000000ull;
        uint32_t pg = (uint32_t)(off & 0x1FFFFFFFu) >> 12;
        if (off >= 0x40000000u || pg >= rg->npages || !rg->free[(uint32_t)(off >> 29) * rg->npages + pg]) {
            ret = w ? BLK_LEAVE : BLK_STOPPED; /* a jump out, or the code going on into the next page */
            goto out;
        }
    }
    blk *nb = b->succ[w];
    if (N64_LIKELY(nb && nb->vpc == pc && m->rdram_gen[nb->page] == nb->gen && !nb->nolink)) {
        if (N64_UNLIKELY(rg != NULL) && w && !nb->rec) {
            ret = BLK_LEAVE; /* a branch target the dispatcher has not recorded yet */
            goto out;
        }
        b = nb;
        goto have_block;
    }
    uint32_t ep = m->blocks->epoch;
    nb = blk_get(m, pc, op_label, BLK_MAX);
    if (nb && m->blocks->epoch == ep) /* no flush meanwhile: b is still where it was */
        ((blk *)b)->succ[w] = nb;
    if (N64_UNLIKELY(rg != NULL) && w && nb && !nb->rec) {
        ret = BLK_LEAVE;
        goto out;
    }
    b = nb;
    goto have_block;
}

bail: {
    /* The op at o falls back to step(), from the state in front of it. */
    uint32_t k = IDX();
    now -= (uint64_t)b->fc * (b->n - k);
    ran += k;
    if (k > 0) {
        c->cur = b->vpc + 4 * (k - 1);
        c->ds = false;
    }
    pc = b->vpc + 4 * k;
    ret = BLK_SLOW;
    if (b->branch && k == b->n - 1u) {
        npc = btgt;
        nds = 1;
        goto out_npc;
    }
}
out:
    npc = pc + 4;
    nds = 0;
out_npc:
    c->pc = pc;
    c->npc = npc;
    c->next_ds = nds != 0;
    m->now = now;
    m->instructions = ran;
    return ret;
#undef NEXT
#undef NEXT_MEM
#undef IDX
#undef REST
#undef IN_DS
#undef SLOW_DONE
#undef BLOAD
#undef BLOAD_PARTIAL
#undef WROTE
#undef WROTE_CHECK
#undef BSTORE
#undef BSTORE_MASKED
#undef BRANCH
}

bool n64_cpu_run_block(n64_machine *m, uint32_t maxops)
{
    int r = run_blocks(m, true, maxops ? maxops : 1u, NULL, false);
    return r == BLK_INTERPRET || r == BLK_SLOW;
}

int n64_cpu_run_region(n64_machine *m, const n64_blk_region *rg, bool mark)
{
    int r = run_blocks(m, false, BLK_MAX, rg, mark);
    return r == BLK_INTERPRET || r == BLK_SLOW ? 1 : r == BLK_NEAR ? 2 : r == BLK_LEAVE ? 3 : 0;
}

void n64_cpu_run(n64_machine *m)
{
    if (N64_UNLIKELY(m->rc != NULL) && n64_recomp_active(m)) {
        n64_recomp_run(m); /* the same loop, with lifted code where a table covers the PC */
        return;
    }
    n64_cpu *c = &m->cpu;
    if (N64_UNLIKELY(m->interp_ref)) { /* the reference: step()'s own loop */
        while (m->now < m->stop_at) {
            if (N64_UNLIKELY(c->irq)) {
                take_interrupt(m);
                continue;
            }
            step(m);
        }
        return;
    }
    while (m->now < m->stop_at) {
        if (N64_UNLIKELY(c->irq)) {
            take_interrupt(m);
            continue;
        }
        if (N64_LIKELY(!c->next_ds && c->ksu == 0 && !(m->mi_mode & 0x80) && c->npc == c->pc + 4)) {
            int r = run_blocks(m, false, BLK_MAX, NULL, false);
            if (r == BLK_STOPPED)
                continue;
            if (r == BLK_NEAR) { /* close to stop_at: step()'s loop to it */
                while (m->now < m->stop_at && !c->irq)
                    step(m);
                continue;
            }
        }
        step(m); /* the loop's checks hold here (BLK_INTERPRET / BLK_SLOW: in front of an instruction that may run) */
    }
}

/* ---- The core functions lifted code calls (n64_recomp_rt.h) -------------------------------------------------------
 * Each does what the interpreter does for the access (translation, costs, bus side effects, RDRAM write tracking) and
 * returns 0, or 1 with nothing done when the access would raise an exception: the lifted code then hands the
 * instruction to the interpreter, which raises it with the exact EPC / BD / BadVAddr. */

int n64_rc_read(n64_machine *m, uint64_t va, int size, uint64_t *out)
{
    return rd_x(m, va, size, out, false) ? 0 : 1;
}

int n64_rc_write(n64_machine *m, uint64_t va, int size, uint64_t v)
{
    return wr_x(m, va, size, v, false) ? 0 : 1;
}

int n64_rc_read_partial(n64_machine *m, uint64_t va, int size, uint64_t *out)
{
    return rd_partial_x(m, va, size, out, false) ? 0 : 1;
}

int n64_rc_write_masked(n64_machine *m, uint64_t va, uint64_t data, uint64_t mask, int size)
{
    return store_masked_x(m, va, data, mask, size, false) ? 0 : 1;
}

/* COP1 operations (anything but the moves, branches, loads and stores, which lifted code runs inline) and CTC1 run in
 * n64_fpu.c, which raises its exceptions itself: the instruction is at pc (ds: in a branch delay slot). Returns 1 when
 * an exception was taken (pc / npc / COP0 then hold the exception's entry; nothing more of the instruction runs). */
#define RC_NO_PC 1u /* never a PC (odd): exception_vec overwrites it */
int n64_rc_cop1(n64_machine *m, uint32_t ins, uint64_t pc, int ds)
{
    n64_cpu *c = &m->cpu;
    c->cur = pc;
    c->ds = ds != 0;
    c->pc = RC_NO_PC;
    n64_fpu_cop1(m, ins);
    c->gpr[0] = 0;
    return c->pc != RC_NO_PC;
}

int n64_rc_ctc1(n64_machine *m, uint32_t v, uint64_t pc, int ds)
{
    n64_cpu *c = &m->cpu;
    c->cur = pc;
    c->ds = ds != 0;
    c->pc = RC_NO_PC;
    n64_fpu_write_fcsr(m, v);
    return c->pc != RC_NO_PC;
}
