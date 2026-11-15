/* VR4300 interpreter (N-3, XBA-171): MIPS III integer unit, COP0 (TLB, exceptions, COUNT / COMPARE), the
 * address spaces (32-bit and 64-bit kernel / supervisor / user), and the dispatch to COP1 (n64_fpu.c).
 *
 * Semantics follow the VR4300 user's manual; where it is silent, the behaviour the n64-systemtest suite pins (as ares
 * and N64Recomp implement it: 32-bit results sign-extended, SRA / SRAV shifting the whole 64-bit register, LWL / LWR
 * sign-extending the merged word, DIV / DDIV by zero giving LO = +1 / -1 and HI = the dividend). No cache model: the
 * CACHE instruction is accepted and does nothing, every access sees memory. Timing: a fixed cost per instruction by
 * fetch region (1.5 cycles from cached RDRAM, more uncached, RSP memory, ROM) plus a cost per uncached data access
 * and the multiply / divide latencies. */
#include "n64_internal.h"

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
 * operations translate the address and can raise TLB exceptions; the index ones use its low bits only. */
static void cache_op(n64_machine *m, int op, uint64_t va)
{
    n64_cpu *c = &m->cpu;
    bool dcache = (op & 3) == 1;
    if ((op & 3) > 1)
        return; /* no secondary cache */
    uint32_t *tag = dcache ? &c->dtag[(va >> 4) & 511] : &c->itag[(va >> 5) & 511];
    uint32_t valid = dcache ? 3u << 6 : 2u << 6; /* D: valid dirty (PState 3), I: valid (2) */
    int kind = op >> 2;
    uint32_t pa = 0;
    if (kind >= 3) { /* create dirty exclusive, the hit operations, fill */
        if (!vtop(m, va, ACC_LOAD, &pa, true))
            return;
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
        cache_op(m, rt, g[rs] + (uint64_t)imm);
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

void n64_cpu_run(n64_machine *m)
{
    if (N64_UNLIKELY(m->rc != NULL) && n64_recomp_active(m)) {
        n64_recomp_run(m); /* the same loop, with lifted code where a table covers the PC */
        return;
    }
    while (m->now < m->stop_at) {
        if (N64_UNLIKELY(m->cpu.irq)) {
            take_interrupt(m);
            continue;
        }
        step(m);
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
