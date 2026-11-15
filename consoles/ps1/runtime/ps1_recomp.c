/* ps1_recomp.c -- see ps1_recomp.h and ps1_recomp_rt.h (XBA-160 / XBA-161 / XBA-162): attaching a pack's table, the
 * run loop that dispatches to lifted code, the content check of RAM code, the record of code the interpreter had to
 * run (discovery loop), the core functions lifted code calls, and the state comparison of lockstep runs.
 *
 * Dispatch: entries are functions (address, length, hash). A page index (1 KB virtual pages -> entries crossing them)
 * finds the entries that hold a PC; a direct-mapped cache remembers the answer per PC. An entry runs only while its
 * code is in RAM: its "check sum" is the sum of the write counters of the RAM pages it covers; when that sum moved
 * since the entry was last hashed, the words are hashed again and compared with the table's hash. Overlays sharing an
 * address are separate entries: the one whose hash matches wins. The loop keeps the interpreter's order of checks
 * (scheduler, IRQ, BIOS hooks) between every two steps, lifted or interpreted, so both run cycle for cycle alike. */
#define _POSIX_C_SOURCE 200809L
#include "ps1_internal.h"
#include "ps1_recomp_rt.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ps1_rc_head is the start of ps1_machine. */
#define HEAD_AT(f) _Static_assert(offsetof(ps1_machine, f) == offsetof(ps1_rc_head, f), "ps1_rc_head." #f)
HEAD_AT(r);
HEAD_AT(pc);
HEAD_AT(next_pc);
HEAD_AT(hi);
HEAD_AT(lo);
HEAD_AT(cur_pc);
HEAD_AT(ld_reg);
HEAD_AT(ld_val);
HEAD_AT(pend_reg);
HEAD_AT(pend_val);
HEAD_AT(in_delay);
HEAD_AT(next_delay);
HEAD_AT(branch_taken);
HEAD_AT(next_taken);
HEAD_AT(irq_pending);
HEAD_AT(hook_at);
HEAD_AT(cop0);
HEAD_AT(cycles);
HEAD_AT(next_event);
HEAD_AT(ram);
_Static_assert(offsetof(ps1_machine, rc_scratch) == offsetof(ps1_rc_head, scratch), "ps1_rc_head.scratch");
HEAD_AT(ram_gen);
HEAD_AT(rc_native);
HEAD_AT(rc_exit);
HEAD_AT(rc_hint);
_Static_assert(PS1_RAM_PAGE_SHIFT == 10, "lifted stores bump ram_gen[offset >> 10]");

#define CACHE_BITS 14
#define CACHE_SIZE (1u << CACHE_BITS)
#define MISSED_MAX 4096u   /* distinct records at most */
#define KEY_NONE 0xFFFFFFFFu
#define LOW_RAM 0x10000u   /* the BIOS's kernel area: always interpreted, never recorded */

enum { V_UNKNOWN = 0, V_OK = 1, V_BAD = 2 };
enum { E_NONE = -1, E_STALE = -2 };

typedef struct rc_page_ref {
    uint32_t page; /* virtual address >> 10 */
    uint32_t e;
} rc_page_ref;

typedef struct rc_line {
    uint32_t key;  /* pc, KEY_NONE: empty */
    int32_t e;     /* entry index, E_NONE: nothing covers pc, E_STALE: candidates whose code is not in RAM */
    uint32_t p0, p1, sum; /* E_STALE: the RAM pages of the candidates and their counter sum then */
} rc_line;

typedef struct rc_missed {
    uint32_t addr;
    uint32_t n;      /* words */
    uint64_t hash;
    uint32_t *words;
    uint32_t next1;  /* 1 + the next record of the same address, 0: none */
    bool owner;      /* words is this record's copy (else a view into an earlier record's) */
} rc_missed;

struct ps1_rc_state {
    const ps1_recomp_table *table;
    uint32_t n;
    uint32_t *pfirst, *plast;   /* per entry: RAM pages (physical, in the 2 MB) */
    uint32_t *vsum;             /* per entry: page counter sum when last hashed */
    uint8_t *vstate;            /* V_* */
    rc_page_ref *refs;
    uint32_t nrefs;
    bool booted;                /* the CPU reached the table's boot_pc (the BIOS is done) */
    uint32_t interp_pc;         /* lifted code handed this instruction to the interpreter */
    uint32_t interp_left;       /* and this many in sequence from there (rc_hint: the rest of a lifted block) */
    uint32_t expect;            /* the instruction after the last interpreted RAM step */
    ps1_recomp_stats stats;
    rc_missed *missed;
    uint32_t nmissed;
    uint64_t *missed_set;       /* (addr, hash) keys, open addressing, 2 * MISSED_MAX slots, 0 = empty */
    uint32_t *var_addr;         /* address -> contents recorded there (PS1_RECOMP_VARIANTS at most), same layout */
    uint8_t *var_cnt;
    uint32_t *var_head1;        /* and 1 + its first record (0: none yet), its last */
    uint32_t *var_tail1;
    rc_line cache[CACHE_SIZE];
};

uint64_t ps1_recomp_hash(const uint32_t *words, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++) {
        h ^= words[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

static uint64_t hash_ram(const uint8_t *ram, uint32_t off, uint32_t len)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t w;
        memcpy(&w, ram + off + i, 4);
        h ^= w;
        h *= 0x100000001B3ull;
    }
    return h;
}

static void flush_cache(struct ps1_rc_state *s)
{
    for (uint32_t i = 0; i < CACHE_SIZE; i++)
        s->cache[i].key = KEY_NONE;
    s->interp_pc = KEY_NONE;
    s->expect = KEY_NONE;
}

/* Save states (XBA-182): whether the program booted (lifted code may run), -1 without recompiled code. */
int ps1_recomp_booted(ps1_machine *m)
{
    return m->rc && m->rc->table ? m->rc->booted : -1;
}

/* A state was loaded: every RAM page counts as written (ps1_state.c bumps them), the dispatch cache and the
 * hand-offs in flight are forgotten, the boot flag is the state's (-1: unknown, then the EXE having started). */
void ps1_recomp_state_loaded(ps1_machine *m, int booted)
{
    struct ps1_rc_state *s = m->rc;
    if (!s)
        return;
    flush_cache(s);
    s->interp_left = 0;
    for (uint32_t i = 0; i < s->n; i++)
        s->vstate[i] = V_UNKNOWN;
    s->booted = booted >= 0 ? booted != 0 : m->exe_started;
}

static void drop_table(struct ps1_rc_state *s)
{
    free(s->pfirst);
    free(s->plast);
    free(s->vsum);
    free(s->vstate);
    free(s->refs);
    s->pfirst = s->plast = s->vsum = NULL;
    s->vstate = NULL;
    s->refs = NULL;
    s->nrefs = s->n = 0;
    s->table = NULL;
}

static struct ps1_rc_state *state(ps1_machine *m)
{
    if (!m->rc) {
        struct ps1_rc_state *s = calloc(1, sizeof *s);
        if (!s)
            return NULL;
        s->missed = calloc(MISSED_MAX, sizeof *s->missed);
        s->missed_set = calloc(2 * MISSED_MAX, sizeof *s->missed_set);
        s->var_addr = calloc(2 * MISSED_MAX, sizeof *s->var_addr);
        s->var_cnt = calloc(2 * MISSED_MAX, 1);
        s->var_head1 = calloc(2 * MISSED_MAX, sizeof *s->var_head1);
        s->var_tail1 = calloc(2 * MISSED_MAX, sizeof *s->var_tail1);
        if (!s->missed || !s->missed_set || !s->var_addr || !s->var_cnt || !s->var_head1 || !s->var_tail1) {
            free(s->var_head1);
            free(s->var_tail1);
            free(s->missed);
            free(s->missed_set);
            free(s->var_addr);
            free(s->var_cnt);
            free(s);
            return NULL;
        }
        flush_cache(s);
        m->rc = s;
    }
    return m->rc;
}

static int cmp_ref(const void *a, const void *b)
{
    const rc_page_ref *x = a, *y = b;
    if (x->page != y->page)
        return x->page < y->page ? -1 : 1;
    return x->e < y->e ? -1 : x->e > y->e;
}

static bool entry_ok(const ps1_recomp_entry *e)
{
    uint32_t seg = e->addr >> 29, p = e->addr & 0x1FFFFFFFu;
    return e->fn && e->len && !(e->addr & 3) && !(e->len & 3) && (seg == 0 || seg == 4 || seg == 5) &&
           p < 0x00800000u && (p & 0x1FFFFFu) + e->len <= PS1_RAM_SIZE;
}

int ps1_attach_recomp(ps1_machine *m, const ps1_recomp_table *t)
{
    if (t && (t->abi != PS1_RECOMP_ABI || (t->count && !t->entries)))
        return -1;
    if (!t && !m->rc)
        return 0;
    struct ps1_rc_state *s = state(m);
    if (!s)
        return -1;
    drop_table(s);
    flush_cache(s);
    if (!t)
        return 0; /* the record of missed code stays; ps1_cpu_run keeps this loop (it is the interpreter's then) */
    uint32_t n = t->count, nrefs = 0;
    for (uint32_t i = 0; i < n; i++)
        if (entry_ok(&t->entries[i]))
            nrefs += ((t->entries[i].addr + t->entries[i].len - 1) >> 10) - (t->entries[i].addr >> 10) + 1;
    s->pfirst = malloc((n ? n : 1) * sizeof *s->pfirst);
    s->plast = malloc((n ? n : 1) * sizeof *s->plast);
    s->vsum = calloc(n ? n : 1, sizeof *s->vsum);
    s->vstate = calloc(n ? n : 1, 1);
    s->refs = malloc((nrefs ? nrefs : 1) * sizeof *s->refs);
    if (!s->pfirst || !s->plast || !s->vsum || !s->vstate || !s->refs) {
        drop_table(s);
        return -1;
    }
    uint32_t k = 0;
    /* Debugging: PS1_RC_ENTRIES=LO-HI attaches only the entries LO..HI (indices), to bisect a divergence. */
    uint32_t only_lo = 0, only_hi = 0xFFFFFFFFu;
    const char *only = getenv("PS1_RC_ENTRIES");
    if (only && *only) {
        char *end = NULL;
        only_lo = (uint32_t)strtoul(only, &end, 0);
        only_hi = (end && *end == '-') ? (uint32_t)strtoul(end + 1, NULL, 0) : only_lo;
    }
    for (uint32_t i = 0; i < n; i++) {
        const ps1_recomp_entry *e = &t->entries[i];
        if (!entry_ok(e) || i < only_lo || i > only_hi) {
            s->vstate[i] = V_BAD;
            s->pfirst[i] = s->plast[i] = 0;
            continue;
        }
        uint32_t off = e->addr & 0x1FFFFFu;
        s->pfirst[i] = off >> PS1_RAM_PAGE_SHIFT;
        s->plast[i] = (off + e->len - 1) >> PS1_RAM_PAGE_SHIFT;
        for (uint32_t pg = e->addr >> 10; pg <= (e->addr + e->len - 1) >> 10; pg++)
            s->refs[k++] = (rc_page_ref){pg, i};
    }
    qsort(s->refs, k, sizeof *s->refs, cmp_ref);
    s->nrefs = k;
    s->n = n;
    s->table = t;
    return 0;
}

void ps1_recomp_free(ps1_machine *m)
{
    struct ps1_rc_state *s = m->rc;
    if (!s)
        return;
    drop_table(s);
    for (uint32_t i = 0; i < s->nmissed; i++)
        if (s->missed[i].owner)
            free(s->missed[i].words);
    free(s->missed);
    free(s->missed_set);
    free(s->var_addr);
    free(s->var_cnt);
    free(s->var_head1);
    free(s->var_tail1);
    free(s);
    m->rc = NULL;
}

/* ---- the content check ---- */

static inline uint32_t page_sum(const ps1_machine *m, uint32_t p0, uint32_t p1)
{
    uint32_t sum = 0;
    for (uint32_t p = p0; p <= p1; p++)
        sum += m->ram_gen[p];
    return sum;
}

/* 1 when entry i's code is in RAM now. */
static inline bool entry_valid(ps1_machine *m, struct ps1_rc_state *s, uint32_t i)
{
    uint32_t sum = page_sum(m, s->pfirst[i], s->plast[i]);
    if (s->vstate[i] != V_UNKNOWN && s->vsum[i] == sum)
        return s->vstate[i] == V_OK;
    const ps1_recomp_entry *e = &s->table->entries[i];
    bool ok = hash_ram(m->ram, e->addr & 0x1FFFFFu, e->len) == e->hash;
    if (s->vstate[i] != V_UNKNOWN) {
        s->stats.rehashes++;
        if (!ok && s->vstate[i] == V_OK)
            s->stats.stale++;
    }
    s->vstate[i] = ok ? V_OK : V_BAD;
    s->vsum[i] = sum;
    return ok;
}

static inline rc_line *line_of(struct ps1_rc_state *s, uint32_t pc)
{
    return &s->cache[((pc >> 2) ^ (pc >> (CACHE_BITS + 2))) & (CACHE_SIZE - 1)];
}

/* The entry to run at pc (index), or -1. */
static int32_t lookup(ps1_machine *m, struct ps1_rc_state *s, uint32_t pc)
{
    rc_line *ln = line_of(s, pc);
    if (ln->key == pc) {
        if (ln->e >= 0) {
            if (entry_valid(m, s, (uint32_t)ln->e))
                return ln->e;
        } else if (ln->e == E_NONE || page_sum(m, ln->p0, ln->p1) == ln->sum) {
            return -1;
        }
    }
    s->stats.slow_lookups++;
    uint32_t page = pc >> 10, lo = 0, hi = s->nrefs;
    while (lo < hi) { /* first ref with page >= pc's */
        uint32_t mid = lo + (hi - lo) / 2;
        if (s->refs[mid].page < page)
            lo = mid + 1;
        else
            hi = mid;
    }
    int32_t found = -1;
    bool any = false;
    uint32_t p0 = 0xFFFFFFFFu, p1 = 0;
    for (uint32_t k = lo; k < s->nrefs && s->refs[k].page == page; k++) {
        uint32_t i = s->refs[k].e;
        const ps1_recomp_entry *e = &s->table->entries[i];
        if (pc - e->addr >= e->len)
            continue;
        any = true;
        if (s->pfirst[i] < p0)
            p0 = s->pfirst[i];
        if (s->plast[i] > p1)
            p1 = s->plast[i];
        if (entry_valid(m, s, i)) {
            found = (int32_t)i;
            break;
        }
    }
    ln->key = pc;
    if (found >= 0) {
        ln->e = found;
    } else if (!any) {
        ln->e = E_NONE;
    } else {
        ln->e = E_STALE;
        ln->p0 = p0;
        ln->p1 = p1;
        ln->sum = page_sum(m, p0, p1);
    }
    return found;
}

/* ---- the missed record ---- */

static uint64_t missed_key(uint32_t addr, uint64_t hash)
{
    uint64_t k = (hash ^ ((uint64_t)addr * 0x9E3779B97F4A7C15ull)) | 1u; /* never 0 */
    return k;
}

/* Inserts (addr, hash) into the set; false when it was there. */
static bool missed_insert(struct ps1_rc_state *s, uint64_t key)
{
    uint32_t i = (uint32_t)(key * 0x9E3779B97F4A7C15ull >> 40) & (2 * MISSED_MAX - 1);
    while (s->missed_set[i]) {
        if (s->missed_set[i] == key)
            return false;
        i = (i + 1) & (2 * MISSED_MAX - 1);
    }
    s->missed_set[i] = key;
    return true;
}

/* The slot of an address in the variant map (UINT32_MAX when the map is full). */
static uint32_t variants(struct ps1_rc_state *s, uint32_t addr)
{
    uint32_t i = (addr * 2654435761u >> 8) & (2 * MISSED_MAX - 1);
    for (uint32_t probes = 0; probes < 2 * MISSED_MAX; probes++) {
        if ((s->var_cnt[i] == 0 && s->var_head1[i] == 0) || s->var_addr[i] == addr) {
            s->var_addr[i] = addr;
            return i;
        }
        i = (i + 1) & (2 * MISSED_MAX - 1);
    }
    return UINT32_MAX;
}

static void add_record(struct ps1_rc_state *s, uint32_t slot, rc_missed r)
{
    s->missed[s->nmissed] = r;
    if (s->var_tail1[slot])
        s->missed[s->var_tail1[slot] - 1].next1 = s->nmissed + 1;
    else
        s->var_head1[slot] = s->nmissed + 1;
    s->var_tail1[slot] = ++s->nmissed;
}

/* A record is new only when no record of pc agrees with RAM over their common length: the same code reached again
 * (as a view into another record, of another length) is recorded once, and only contents that differ count as
 * variants (PS1_RECOMP_VARIANTS: code generated at run time); views are never counted. */
static void record_missed(ps1_machine *m, struct ps1_rc_state *s, uint32_t pc)
{
    uint32_t off = pc & 0x1FFFFFu;
    if (s->nmissed >= MISSED_MAX)
        return;
    uint32_t slot = variants(s, pc);
    if (slot == UINT32_MAX || s->var_cnt[slot] >= PS1_RECOMP_VARIANTS)
        return; /* code rewritten over and over at this address: left to the interpreter */
    uint32_t n = PS1_RECOMP_SNAPSHOT / 4;
    if (off + n * 4 > PS1_RAM_SIZE)
        n = (PS1_RAM_SIZE - off) / 4;
    for (uint32_t i = s->var_head1[slot]; i; i = s->missed[i - 1].next1) {
        const rc_missed *r = &s->missed[i - 1];
        if (!memcmp(m->ram + off, r->words, (size_t)(r->n < n ? r->n : n) * 4))
            return;
    }
    /* Inside a recent record whose bytes are still there: a view into its copy. */
    for (uint32_t k = s->nmissed, seen = 0; k > 0 && seen < 32; k--, seen++) {
        const rc_missed *r = &s->missed[k - 1];
        if (r->owner && pc - r->addr < r->n * 4u && (pc - r->addr) / 4 + 8 <= r->n) {
            uint32_t w = (pc - r->addr) / 4;
            if (!memcmp(m->ram + off, r->words + w, (size_t)(r->n - w) * 4)) {
                uint64_t hash = ps1_recomp_hash(r->words + w, r->n - w);
                if (missed_insert(s, missed_key(pc, hash)))
                    add_record(s, slot, (rc_missed){pc, r->n - w, hash, r->words + w, 0, false});
                return;
            }
        }
    }
    uint64_t hash = hash_ram(m->ram, off, n * 4);
    if (!missed_insert(s, missed_key(pc, hash)))
        return;
    uint32_t *w = malloc(n * 4);
    if (!w)
        return;
    memcpy(w, m->ram + off, n * 4);
    add_record(s, slot, (rc_missed){pc, n, hash, w, 0, true});
    s->var_cnt[slot]++;
}

size_t ps1_recomp_missed(ps1_machine *m, uint32_t *out, size_t max)
{
    struct ps1_rc_state *s = m->rc;
    if (!s)
        return 0;
    size_t n = 0;
    for (uint32_t i = 0; i < s->nmissed; i++) {
        bool dup = false;
        for (uint32_t j = 0; j < i && !dup; j++)
            dup = s->missed[j].addr == s->missed[i].addr;
        if (dup)
            continue;
        if (n < max)
            out[n] = s->missed[i].addr;
        n++;
    }
    return n;
}

int ps1_recomp_write_missed(ps1_machine *m, const char *path)
{
    struct ps1_rc_state *s = m->rc;
    if (!s || !s->nmissed)
        return 0;
    /* (addr, hash) pairs the file holds already */
    uint64_t *have = NULL;
    size_t nhave = 0, cap = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        char head[64];
        int c;
        for (;;) {
            size_t len = 0;
            while ((c = fgetc(f)) != EOF && c != '\n') {
                if (len < sizeof head - 1)
                    head[len++] = (char)c;
            }
            head[len] = 0;
            unsigned long a = 0;
            unsigned long long hsh = 0;
            if (head[0] != '#' && sscanf(head, "%lx %llx", &a, &hsh) == 2) {
                if (nhave == cap) {
                    cap = cap ? cap * 2 : 256;
                    uint64_t *nh = realloc(have, cap * sizeof *have);
                    if (!nh)
                        break;
                    have = nh;
                }
                have[nhave++] = missed_key((uint32_t)a, (uint64_t)hsh);
            }
            if (c == EOF)
                break;
        }
        fclose(f);
    }
    f = fopen(path, "a");
    if (!f) {
        free(have);
        return -1;
    }
    int written = 0;
    for (uint32_t i = 0; i < s->nmissed; i++) {
        const rc_missed *r = &s->missed[i];
        uint64_t key = missed_key(r->addr, r->hash);
        bool dup = false;
        for (size_t j = 0; j < nhave && !dup; j++)
            dup = have[j] == key;
        if (dup)
            continue;
        fprintf(f, "%08X %016llX ", r->addr, (unsigned long long)r->hash);
        for (uint32_t k = 0; k < r->n; k++) {
            uint32_t w = r->words[k];
            fprintf(f, "%02X%02X%02X%02X", w & 0xFF, (w >> 8) & 0xFF, (w >> 16) & 0xFF, w >> 24);
        }
        fputc('\n', f);
        written++;
    }
    free(have);
    return fclose(f) == 0 ? written : -1;
}

void ps1_recomp_get_stats(ps1_machine *m, ps1_recomp_stats *out)
{
    memset(out, 0, sizeof *out);
    if (m->rc)
        *out = m->rc->stats;
    out->native = m->rc_native;
}

/* ---- the run loop ---- */

/* Main RAM above the kernel area, through KUSEG / KSEG0 / KSEG1: where lifted code may run. */
static inline bool liftable_pc(uint32_t pc)
{
    return ps1_rc_liftable_pc(pc);
}
_Static_assert(LOW_RAM == PS1_RC_LOW_RAM, "ps1_recomp LOW_RAM");

/* Does instruction ins read register r as an operand, or load into it (other than LWL / LWR, which merge with the
 * load in flight)? Then the load in flight to r must stay in flight across it. */
static bool conflicts(uint32_t ins, uint32_t r)
{
    uint32_t op = ins >> 26, rs = (ins >> 21) & 31, rt = (ins >> 16) & 31;
    switch (op) {
    case 0x00:
        switch (ins & 63) {
        case 0x00: case 0x02: case 0x03: return rt == r;                  /* shifts by immediate */
        case 0x08: case 0x09: case 0x11: case 0x13: return rs == r;       /* JR JALR MTHI MTLO */
        case 0x0C: case 0x0D: case 0x10: case 0x12: return false;         /* SYSCALL BREAK MFHI MFLO */
        default: return rs == r || rt == r;
        }
    case 0x01: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E:
        return rs == r;
    case 0x04: case 0x05: return rs == r || rt == r;
    case 0x02: case 0x03: case 0x0F: return false;
    case 0x10: case 0x12: { /* COP0 / COP2 transfers */
        if (ins & (1u << 25))
            return false;
        uint32_t sub = (ins >> 21) & 31;
        if (sub == 0x04 || sub == 0x06)
            return rt == r; /* MTC / CTC read rt */
        if (sub == 0x00 || sub == 0x02)
            return rt == r; /* MFC / CFC: a delayed load into rt */
        return false;
    }
    case 0x20: case 0x21: case 0x23: case 0x24: case 0x25: return rs == r || rt == r; /* loads into rt */
    case 0x22: case 0x26: return rs == r;  /* LWL / LWR: forwarding */
    case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2E: return rs == r || rt == r;
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x38: case 0x39: case 0x3A: case 0x3B: return rs == r;
    default: return true; /* reserved: let the interpreter decide */
    }
}

void ps1_recomp_run(ps1_machine *m)
{
    struct ps1_rc_state *s = m->rc;
    while (m->cycles < m->next_event) {
        if (m->irq_pending) {
            s->stats.interrupts++;
            ps1_cpu_take_interrupt(m);
            continue;
        }
        ps1_cpu_hook_check(m);
        uint32_t pc = m->pc;
        if (!s->table || !liftable_pc(pc) || (!s->booted && !(s->booted = pc == s->table->boot_pc))) {
            if (s->table && !liftable_pc(pc))
                ps1_cpu_step_unliftable(m); /* the BIOS, its handlers: as long as that lasts */
            else
                ps1_cpu_step(m);
            continue;
        }
        if (pc == s->interp_pc || m->next_delay || (m->cop0[C0_SR] & (SR_KUC | SR_ISC))) {
            /* the instruction lifted code left to the interpreter (after the loop's checks), a delay slot, user mode
             * or an isolated cache. Then the rest of its block, while execution stays in sequence: inside a block
             * nothing is a resume point. */
            bool handed = pc == s->interp_pc;
            if (handed)
                s->stats.fallbacks++;
            s->interp_pc = KEY_NONE;
            s->stats.interp_ram++;
            ps1_cpu_step(m);
            s->expect = KEY_NONE;
            if (handed && s->interp_left > 1 && m->pc == pc + 4) {
                s->interp_pc = pc + 4;
                s->interp_left--;
            }
            continue;
        }
        if (m->ld_reg) {
            /* A load in flight: commit it now unless the next instruction can tell (ps1_recomp_rt.h). */
            uint32_t ins;
            memcpy(&ins, m->ram + (pc & 0x1FFFFCu), 4);
            if (conflicts(ins, m->ld_reg)) {
                s->stats.interp_ram++;
                ps1_cpu_step(m);
                s->expect = KEY_NONE;
                continue;
            }
            m->r[m->ld_reg] = m->ld_val;
            m->ld_reg = 0;
        }
        int32_t e = lookup(m, s, pc);
        if (e >= 0) {
            m->rc_exit = PS1_RC_EXIT_NORMAL;
            m->rc_hint = 0;
            s->table->entries[e].fn(m);
            m->r[0] = 0;
            uint8_t why = m->rc_exit;
            m->rc_exit = PS1_RC_EXIT_NORMAL;
            if (why != PS1_RC_EXIT_NOT_COVERED) {
                s->stats.calls++;
                if (why == PS1_RC_EXIT_INTERP) {
                    s->interp_pc = m->pc;
                    s->interp_left = m->rc_hint ? m->rc_hint : 1;
                }
                s->expect = KEY_NONE;
                continue;
            }
        } else if (pc != s->expect) {
            record_missed(m, s, pc);
        }
        s->stats.interp_ram++;
        ps1_cpu_step(m);
        s->expect = pc + 4;
    }
}

/* ---- the core functions lifted code calls (ps1_recomp_rt.h) ---- */

int ps1_rc_lw(ps1_machine *m, uint32_t addr, uint32_t *v)
{
    int e = ps1_bus_fault(m, addr, 0);
    if (e)
        return e;
    uint32_t p = addr & 0x1FFFFFFFu;
    m->cycles += (p & 0xFFF80000u) == 0x1FC00000u ? 12 : 2; /* ps1_cpu.c load32: LOAD_BIOS / LOAD_IO */
    return ps1_read32(m, addr, v);
}

int ps1_rc_read(ps1_machine *m, uint32_t addr, int width, uint32_t *v)
{
    int e = ps1_bus_fault(m, addr, 0);
    if (e)
        return e;
    return width == 1 ? ps1_read8(m, addr, v) : ps1_read16(m, addr, v);
}

int ps1_rc_write(ps1_machine *m, uint32_t addr, int width, uint32_t v)
{
    int e = ps1_bus_fault(m, addr, 1);
    if (e)
        return e;
    return width == 1 ? ps1_write8(m, addr, v) : width == 2 ? ps1_write16(m, addr, v) : ps1_write32(m, addr, v);
}

/* SWL / SWR as ps1_cpu.c runs them: the aligned word is read with load32's cost, merged, written back. */
int ps1_rc_swlr(ps1_machine *m, uint32_t addr, uint32_t rt, int right)
{
    uint32_t a = addr & ~3u, sh = addr & 3, w = 0, r;
    if (m->cop0[C0_SR] & SR_ISC)
        return 0;
    uint32_t seg = a >> 29, p = (seg == 7) ? a : (a & 0x1FFFFFFFu);
    if (p < 0x00800000u && seg != 1 && seg != 2 && seg != 3) {
        memcpy(&w, m->ram + (p & 0x1FFFFCu), 4);
        if (ps1_bus_fault(m, a, 1))
            return ps1_bus_fault(m, a, 1); /* KSEG2 mirror quirk: readable, not writable */
        m->cycles += 3;
    } else if ((p & 0xFFFFFC00u) == 0x1F800000u) {
        memcpy(&w, m->scratch + (p & 0x3FCu), 4);
        if (ps1_bus_fault(m, a, 1))
            return ps1_bus_fault(m, a, 1);
    } else {
        int e = ps1_bus_fault(m, a, 0);
        if (!e)
            e = ps1_bus_fault(m, a, 1);
        if (e)
            return e;
        m->cycles += (p & 0xFFF80000u) == 0x1FC00000u ? 12 : 2;
        ps1_read32(m, a, &w);
    }
    if (!right)
        r = sh == 3 ? rt : (w & (0xFFFFFF00u << (sh * 8))) | (rt >> (24 - sh * 8));
    else
        r = sh == 0 ? rt : (w & (0x00FFFFFFu >> (24 - sh * 8))) | (rt << (sh * 8));
    return ps1_write32(m, a, r);
}

int ps1_rc_gte_cmd(ps1_machine *m, uint32_t cmd)
{
    return ps1_gte_command(&m->gte, cmd & 0x1FFFFFFu);
}

uint32_t ps1_rc_gte_read(ps1_machine *m, int r)
{
    return r < 32 ? ps1_gte_read_data(&m->gte, r) : ps1_gte_read_ctrl(&m->gte, r - 32);
}

void ps1_rc_gte_write(ps1_machine *m, int r, uint32_t v)
{
    if (r < 32)
        ps1_gte_write_data(&m->gte, r, v);
    else
        ps1_gte_write_ctrl(&m->gte, r - 32, v);
}

/* ---- differential testing ---- */

#define CMP_FIELD(name, x, y)                                                                                        \
    do {                                                                                                             \
        if ((x) != (y)) {                                                                                            \
            snprintf(why, why_n, "%s: 0x%llX vs 0x%llX", name, (unsigned long long)(x), (unsigned long long)(y));    \
            return 1;                                                                                                \
        }                                                                                                            \
    } while (0)

static int cmp_mem(const char *name, const uint8_t *x, const uint8_t *y, size_t n, char *why, size_t why_n)
{
    if (!memcmp(x, y, n))
        return 0;
    size_t i = 0;
    while (x[i] == y[i])
        i++;
    snprintf(why, why_n, "%s[0x%zX]: 0x%02X vs 0x%02X", name, i, x[i], y[i]);
    return 1;
}

static uint32_t reg_eff(const ps1_machine *m, int i)
{
    return (m->ld_reg && (int)m->ld_reg == i) ? m->ld_val : m->r[i];
}

int ps1_recomp_compare(ps1_machine *a, ps1_machine *b, char *why, size_t why_n)
{
    char name[32];
    for (int i = 0; i < 32; i++) {
        snprintf(name, sizeof name, "r%d", i);
        CMP_FIELD(name, reg_eff(a, i), reg_eff(b, i));
    }
    CMP_FIELD("pc", a->pc, b->pc);
    CMP_FIELD("next_pc", a->next_pc, b->next_pc);
    CMP_FIELD("delay slot", a->next_delay, b->next_delay);
    CMP_FIELD("hi", a->hi, b->hi);
    CMP_FIELD("lo", a->lo, b->lo);
    CMP_FIELD("SR", a->cop0[C0_SR], b->cop0[C0_SR]);
    CMP_FIELD("CAUSE", a->cop0[C0_CAUSE], b->cop0[C0_CAUSE]);
    CMP_FIELD("EPC", a->cop0[C0_EPC], b->cop0[C0_EPC]);
    CMP_FIELD("BadVaddr", a->cop0[C0_BADVADDR], b->cop0[C0_BADVADDR]);
    CMP_FIELD("cycles", a->cycles, b->cycles);
    CMP_FIELD("next_event", a->next_event, b->next_event);
    CMP_FIELD("line", a->line, b->line);
    CMP_FIELD("frames", a->frames, b->frames);
    CMP_FIELD("I_STAT", a->i_stat, b->i_stat);
    CMP_FIELD("I_MASK", a->i_mask, b->i_mask);
    CMP_FIELD("DICR", a->dicr, b->dicr);
    for (int i = 0; i < 3; i++) {
        CMP_FIELD("timer counter", a->tmr[i].counter, b->tmr[i].counter);
        CMP_FIELD("timer mode", a->tmr[i].mode, b->tmr[i].mode);
    }
    for (int i = 0; i < 64; i++) {
        snprintf(name, sizeof name, "gte reg %d", i);
        CMP_FIELD(name, i < 32 ? ps1_gte_read_data(&a->gte, i) : ps1_gte_read_ctrl(&a->gte, i - 32),
                  i < 32 ? ps1_gte_read_data(&b->gte, i) : ps1_gte_read_ctrl(&b->gte, i - 32));
    }
    if (cmp_mem("ram", a->ram, b->ram, PS1_RAM_SIZE, why, why_n) ||
        cmp_mem("scratch", a->scratch, b->scratch, PS1_SCRATCH_SIZE, why, why_n))
        return 1;
    const uint16_t *va = a->gpu ? ps1_gpu_vram(a->gpu) : NULL, *vb = b->gpu ? ps1_gpu_vram(b->gpu) : NULL;
    if (va && vb && cmp_mem("vram", (const uint8_t *)va, (const uint8_t *)vb, 1024 * 512 * 2, why, why_n))
        return 1;
    return 0;
}
