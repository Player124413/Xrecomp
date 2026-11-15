/* n64_recomp.c -- see n64_recomp.h and n64_recomp_rt.h (XBA-169 / XBA-170 / XBA-171): attaching a pack's table, the
 * run loop that dispatches to lifted code, the content check of RDRAM code (direct-mapped and TLB-mapped), the record
 * of code the interpreter had to run (discovery loop), and the state comparison of lockstep runs.
 *
 * Dispatch: entries are functions (virtual address, length, hash). A page index (4 KB virtual pages -> entries
 * crossing them) finds the entries that hold a PC; a direct-mapped cache remembers the answer per PC. An entry runs
 * only while its code is in memory: its "check sum" is the sum of the write counters of the RDRAM pages it covers
 * (physical: for a TLB-mapped entry, the page its virtual page maps to now); when that sum moved since the entry was
 * last hashed, or the entry now maps elsewhere, the words are hashed again and compared with the table's hash. The
 * loop keeps the interpreter's order of checks (scheduler, interrupt) between every two steps, lifted or interpreted,
 * so both run tick for tick alike. Code no entry covers runs as the interpreter's predecoded blocks (XBA-210), each
 * stopping in front of the next entry's start whose code is there (an entry whose code is not there, data the static
 * pass took for code or another overlay, cuts nothing), which hold those checks themselves; in a KSEG0 / KSEG1 page no
 * entry's code is in, as chained blocks (n64_cpu_run_region) without a lookup per block. */
#define _POSIX_C_SOURCE 200809L
#include "n64_internal.h"
#include "n64_recomp_rt.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* n64_rc_head is the start of n64_machine, n64_rc_cpu the start of n64_cpu. */
#define HEAD_AT(f) _Static_assert(offsetof(n64_machine, f) == offsetof(n64_rc_head, f), "n64_rc_head." #f)
#define CPU_AT(f)                                                                                                    \
    _Static_assert(offsetof(n64_machine, cpu) + offsetof(n64_cpu, f) == offsetof(n64_rc_head, cpu.f),                \
                   "n64_rc_cpu." #f)
HEAD_AT(now);
HEAD_AT(stop_at);
HEAD_AT(instructions);
HEAD_AT(rdram);
HEAD_AT(rdram_gen);
HEAD_AT(rdram_size);
HEAD_AT(mi_mode);
HEAD_AT(rc_lo);
HEAD_AT(rc_len);
HEAD_AT(rc_hit);
HEAD_AT(rc_exit);
HEAD_AT(rc_hint);
HEAD_AT(rc_native);
HEAD_AT(cpu);
CPU_AT(gpr);
CPU_AT(hi);
CPU_AT(lo);
CPU_AT(pc);
CPU_AT(npc);
CPU_AT(cur);
CPU_AT(ds);
CPU_AT(next_ds);
CPU_AT(llbit);
CPU_AT(irq);
CPU_AT(ksu);
CPU_AT(addr64);
CPU_AT(ops64);
CPU_AT(cop0);
CPU_AT(fpr);
CPU_AT(fcsr);
_Static_assert(N64_PAGE_SHIFT == 12, "lifted stores bump rdram_gen[offset >> 12]");
_Static_assert(N64_RC_CPI_CACHED == COST_CACHED && N64_RC_CPI_UNCACHED == COST_UNCACHED_RAM, "fetch costs");
_Static_assert(N64_RC_COST_MULT == COST_MULT && N64_RC_COST_DMULT == COST_DMULT && N64_RC_COST_DIV == COST_DIV &&
                   N64_RC_COST_DDIV == COST_DDIV,
               "multiply / divide costs");

#define CACHE_BITS 14
#define CACHE_SIZE (1u << CACHE_BITS)
#define MISSED_MAX 65536u            /* records at most */
#define MISSED_BYTES (64u << 20)      /* bytes of recorded code at most */
#define ADDR_SLOTS (2 * MISSED_MAX)   /* the address index (a power of two) */
#define VIEW_WORDS 16u                /* words of a view written to the discovered file */
#define KEY_NONE 0xFFFFFFFFu
#define PAGE 4096u

enum { V_UNKNOWN = 0, V_OK = 1, V_BAD = 2 };
enum { E_NONE = -1, E_STALE = -2 };

typedef struct rc_page_ref {
    uint32_t page; /* virtual address >> 12 */
    uint32_t e;
} rc_page_ref;

typedef struct rc_line {
    uint32_t key;  /* pc (32-bit virtual), KEY_NONE: empty */
    int32_t e;     /* entry index, E_NONE: nothing covers pc, E_STALE: candidates whose code is not in memory */
    uint32_t tg;   /* mapped pc: the TLB generation the translation was made under (and ERL in bit 31) */
    uint32_t pbase; /* the physical address the entry's start maps to (E_STALE: unused) */
    uint32_t p0, p1, sum; /* E_STALE: the RDRAM pages of the candidates and their counter sum then */
    uint32_t run;  /* E_NONE / E_STALE: the instructions from pc that no other entry covers (to the next entry's start
                    * or the page's end): the interpreter's block may run them (n64_recomp_run) */
    uint32_t rec_n;   /* E_NONE / E_STALE: 1 + the record count when recording pc last changed nothing, 0: unknown */
    uint32_t rec_sum; /* and the counter sum of the RDRAM pages its window covers then (rec_window) */
} rc_line;

typedef struct rc_missed {
    uint32_t addr;
    uint32_t n;      /* words */
    uint64_t hash;
    uint32_t *words; /* big-endian values */
    uint32_t next1;  /* 1 + the next record of the same address, 0: none */
    bool owner;      /* words is this record's copy (else a view into an earlier record's, or a session record's) */
} rc_missed;

/* An address's records (rc_set.slots, open addressing). */
typedef struct rc_addr {
    uint32_t addr;
    uint32_t head1;  /* 1 + its first record, 0: none (yet) */
    uint32_t tail1;  /* 1 + its last record */
    uint8_t var;     /* contents recorded there from memory (views not counted) */
    uint8_t used;
} rc_addr;

/* Records with an index by address: the session's (what the interpreter had to run), and the discovered file's. */
typedef struct rc_set {
    rc_missed *r;
    uint32_t n, cap;
    rc_addr *slots;
    uint32_t mask;   /* slots - 1 */
    uint32_t naddr;  /* addresses with a record */
    size_t bytes;    /* words owned, in bytes */
} rc_set;

/* Per 4 KB page of KSEG0 / KSEG1 RDRAM (n64_blk_region): the entries that touch it and whether one of them is valid. */
typedef struct rc_page {
    uint32_t r0, nref;    /* its refs (s->refs[r0 .. r0 + nref)) */
    uint32_t p0, p1, sum; /* the RDRAM pages those entries' words are in, and their counter sum when last checked */
    uint8_t known;        /* free[] holds for that sum */
} rc_page;

struct n64_rc_state {
    const n64_recomp_table *table;
    uint32_t n;
    uint32_t *vsum;             /* per entry: page counter sum when last hashed */
    uint32_t *vpa;              /* per entry: the physical address it was hashed at */
    uint8_t *vstate;            /* V_* */
    uint8_t *mapped;            /* per entry: 1 for a TLB-mapped address */
    rc_page_ref *refs;
    uint32_t nrefs;
    bool booted;                /* the CPU reached the table's boot_pc (IPL3 is done) */
    uint64_t interp_pc;         /* lifted code handed this instruction to the interpreter */
    uint32_t interp_left;       /* and this many in sequence from there (rc_hint: the rest of a lifted block) */
    uint64_t expect;            /* the instruction after the last interpreted RDRAM step */
    n64_recomp_stats stats;
    rc_set rec;                 /* the missed record */
    rc_set file;                /* the records the discovered file holds (n64_recomp_write_missed), read once */
    char *file_path;
    uint32_t file_upto;         /* session records [0, file_upto) were compared with the file (and written) */
    size_t file_size;           /* its size after this machine's last write */
    rc_page *pages;             /* 2 * npages: KSEG0 then KSEG1 */
    uint8_t *pfree;             /* n64_blk_region.free */
    n64_blk_region region;
    rc_line cache[CACHE_SIZE];
};

uint64_t n64_recomp_hash(const uint32_t *words, size_t n)
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
        h ^= n64_be32(ram + off + i);
        h *= 0x100000001B3ull;
    }
    return h;
}

static void flush_cache(struct n64_rc_state *s)
{
    for (uint32_t i = 0; i < CACHE_SIZE; i++)
        s->cache[i].key = KEY_NONE;
    s->interp_pc = 1; /* never a PC */
    s->expect = 1;
    if (s->pages) {
        uint32_t np = 2 * s->region.npages;
        for (uint32_t i = 0; i < np; i++) {
            s->pages[i].known = 0;
            s->pfree[i] = s->pages[i].nref == 0;
        }
    }
}

static void drop_table(struct n64_rc_state *s)
{
    free(s->vsum);
    free(s->vpa);
    free(s->vstate);
    free(s->mapped);
    free(s->refs);
    free(s->pages);
    free(s->pfree);
    s->vsum = s->vpa = NULL;
    s->vstate = s->mapped = NULL;
    s->refs = NULL;
    s->pages = NULL;
    s->pfree = NULL;
    s->region.free = NULL;
    s->region.npages = 0;
    s->nrefs = s->n = 0;
    s->table = NULL;
}

static bool set_init(rc_set *t, uint32_t cap, uint32_t slots)
{
    memset(t, 0, sizeof *t);
    t->r = calloc(cap ? cap : 1, sizeof *t->r);
    t->slots = calloc(slots, sizeof *t->slots);
    t->cap = cap;
    t->mask = slots - 1;
    if (t->r && t->slots)
        return true;
    free(t->r);
    free(t->slots);
    memset(t, 0, sizeof *t);
    return false;
}

static void set_free(rc_set *t)
{
    for (uint32_t i = 0; i < t->n; i++)
        if (t->r[i].owner)
            free(t->r[i].words);
    free(t->r);
    free(t->slots);
    memset(t, 0, sizeof *t);
}

/* The slot of addr (made when missing), NULL when the index is full. */
static rc_addr *set_addr(rc_set *t, uint32_t addr)
{
    uint32_t i = (addr * 2654435761u >> 8) & t->mask;
    for (uint32_t probes = 0; probes <= t->mask; probes++) {
        rc_addr *a = &t->slots[i];
        if (!a->used) {
            a->used = 1;
            a->addr = addr;
            return a;
        }
        if (a->addr == addr)
            return a;
        i = (i + 1) & t->mask;
    }
    return NULL;
}

/* Appends a record of a's address (the caller checked the room). */
static void set_add(rc_set *t, rc_addr *a, uint32_t n, uint64_t hash, uint32_t *words, bool owner)
{
    t->r[t->n] = (rc_missed){a->addr, n, hash, words, 0, owner};
    if (a->tail1)
        t->r[a->tail1 - 1].next1 = t->n + 1;
    else
        t->naddr++, a->head1 = t->n + 1;
    a->tail1 = t->n + 1;
    t->n++;
    if (owner)
        t->bytes += (size_t)n * 4;
}

/* Whether the words agree with a record of a's address over their common length (the same code, recorded from
 * further or less far). */
static bool set_agrees(const rc_set *t, const rc_addr *a, const uint32_t *words, uint32_t n)
{
    for (uint32_t i = a->head1; i; i = t->r[i - 1].next1) {
        const rc_missed *r = &t->r[i - 1];
        uint32_t k = r->n < n ? r->n : n;
        if (!memcmp(r->words, words, (size_t)k * 4))
            return true;
    }
    return false;
}

static struct n64_rc_state *state(n64_machine *m)
{
    if (!m->rc) {
        struct n64_rc_state *s = calloc(1, sizeof *s);
        if (!s)
            return NULL;
        if (!set_init(&s->rec, MISSED_MAX, ADDR_SLOTS)) {
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

static inline bool is_direct(uint32_t va) { return va - 0x80000000u < 0x40000000u; } /* KSEG0 / KSEG1 */

static bool entry_ok(const n64_machine *m, const n64_recomp_entry *e)
{
    if (!e->fn || !e->len || (e->addr & 3) || (e->len & 3) || e->len > 0x100000u)
        return false;
    if ((uint64_t)e->addr + e->len > 0x100000000ull)
        return false;
    if (is_direct(e->addr)) {
        uint32_t pa = e->addr & 0x1FFFFFFFu;
        return pa < m->rdram_size && pa + e->len <= m->rdram_size && is_direct(e->addr + e->len - 1);
    }
    return (e->addr & ~(PAGE - 1)) == ((e->addr + e->len - 1) & ~(PAGE - 1)); /* mapped: within one page */
}

bool n64_recomp_active(const n64_machine *m) { return m->rc && m->rc->table; }

int n64_attach_recomp(n64_machine *m, const n64_recomp_table *t)
{
    if (t && (t->abi != N64_RECOMP_ABI || (t->count && !t->entries)))
        return -1;
    if (!t && !m->rc)
        return 0;
    struct n64_rc_state *s = state(m);
    if (!s)
        return -1;
    drop_table(s);
    flush_cache(s);
    if (!t)
        return 0; /* the record of missed code stays */
    uint32_t n = t->count, nrefs = 0;
    for (uint32_t i = 0; i < n; i++)
        if (entry_ok(m, &t->entries[i]))
            nrefs += ((t->entries[i].addr + t->entries[i].len - 1) >> 12) - (t->entries[i].addr >> 12) + 1;
    s->vsum = calloc(n ? n : 1, sizeof *s->vsum);
    s->vpa = calloc(n ? n : 1, sizeof *s->vpa);
    s->vstate = calloc(n ? n : 1, 1);
    s->mapped = calloc(n ? n : 1, 1);
    s->refs = malloc((nrefs ? nrefs : 1) * sizeof *s->refs);
    if (!s->vsum || !s->vpa || !s->vstate || !s->mapped || !s->refs) {
        drop_table(s);
        return -1;
    }
    /* Debugging: N64_RC_ENTRIES=LO-HI attaches only the entries LO..HI (indices), to bisect a divergence. */
    uint32_t only_lo = 0, only_hi = 0xFFFFFFFFu;
    const char *only = getenv("N64_RC_ENTRIES");
    if (only && *only) {
        char *end = NULL;
        only_lo = (uint32_t)strtoul(only, &end, 0);
        only_hi = (end && *end == '-') ? (uint32_t)strtoul(end + 1, NULL, 0) : only_lo;
    }
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        const n64_recomp_entry *e = &t->entries[i];
        if (!entry_ok(m, e) || i < only_lo || i > only_hi) {
            s->vstate[i] = V_BAD;
            continue;
        }
        s->mapped[i] = !is_direct(e->addr);
        for (uint32_t pg = e->addr >> 12; pg <= (e->addr + e->len - 1) >> 12; pg++)
            s->refs[k++] = (rc_page_ref){pg, i};
    }
    qsort(s->refs, k, sizeof *s->refs, cmp_ref);
    s->nrefs = k;
    s->n = n;
    s->table = t;
    s->booted = false;
    /* The KSEG0 / KSEG1 pages and the entries that touch each (refs are sorted by page). */
    uint32_t np = m->rdram_size >> 12;
    s->pages = calloc(2 * (size_t)np + 1, sizeof *s->pages);
    s->pfree = calloc(2 * (size_t)np + 1, 1);
    if (!s->pages || !s->pfree) {
        drop_table(s);
        return -1;
    }
    for (uint32_t j = 0; j < k; j++) {
        uint32_t va = s->refs[j].page << 12;
        if (!is_direct(va) || ((va & 0x1FFFFFFFu) >> 12) >= np)
            continue;
        rc_page *g = &s->pages[((va - 0x80000000u) >> 29) * np + ((va & 0x1FFFFFFFu) >> 12)];
        if (!g->nref)
            g->r0 = j;
        g->nref++;
    }
    s->region.npages = np;
    s->region.free = s->pfree;
    flush_cache(s);
    return 0;
}

void n64_recomp_reset(n64_machine *m)
{
    struct n64_rc_state *s = m->rc;
    if (!s)
        return;
    flush_cache(s);
    s->booted = false;
    for (uint32_t i = 0; i < s->n; i++)
        if (s->vstate[i] == V_OK)
            s->vstate[i] = V_UNKNOWN;
}

int n64_recomp_booted(n64_machine *m)
{
    return m->rc && m->rc->table ? (int)m->rc->booted : -1;
}

void n64_recomp_state_loaded(n64_machine *m, int booted)
{
    m->rc_lo = m->rc_len = 0;
    m->rc_hit = 0;
    struct n64_rc_state *s = m->rc;
    if (!s)
        return;
    flush_cache(s);
    /* A state from an interpreter-only run does not say: IPL3 takes a fraction of a second, a state is saved later. */
    s->booted = booted >= 0 ? booted != 0 : m->frames >= 30;
    for (uint32_t i = 0; i < s->n; i++)
        if (s->vstate[i] == V_OK)
            s->vstate[i] = V_UNKNOWN;
}

void n64_recomp_free(n64_machine *m)
{
    struct n64_rc_state *s = m->rc;
    if (!s)
        return;
    drop_table(s);
    set_free(&s->file); /* views into the session's words: freed first */
    set_free(&s->rec);
    free(s->file_path);
    free(s);
    m->rc = NULL;
}

/* ---- translation and the content check ---- */

/* The translation key of a mapped address: the TLB generation and ERL (which unmaps KUSEG). */
static inline uint32_t tlb_key(const n64_machine *m)
{
    return (m->cpu.tlb_gen & 0x7FFFFFFFu) | ((m->cpu.cop0[C0_STATUS] & SR_ERL) ? 0x80000000u : 0);
}

/* The physical RDRAM address of virtual va (kernel mode), or false (unmapped, not RDRAM). */
static inline bool translate(n64_machine *m, uint32_t va, uint32_t *pa)
{
    if (is_direct(va)) {
        *pa = va & 0x1FFFFFFFu;
    } else if (n64_cpu_probe(m, (uint64_t)(int64_t)(int32_t)va, pa) != 0) {
        return false;
    }
    return *pa < m->rdram_size;
}

static inline uint32_t page_sum(const n64_machine *m, uint32_t p0, uint32_t p1)
{
    uint32_t sum = 0;
    for (uint32_t p = p0; p <= p1; p++)
        sum += m->rdram_gen[p];
    return sum;
}

/* 1 when entry i's code is in RDRAM at pa (the physical address its start maps to now). */
static inline bool entry_valid(n64_machine *m, struct n64_rc_state *s, uint32_t i, uint32_t pa)
{
    const n64_recomp_entry *e = &s->table->entries[i];
    if (pa + e->len > m->rdram_size)
        return false;
    uint32_t sum = page_sum(m, pa >> 12, (pa + e->len - 1) >> 12);
    if (s->vstate[i] != V_UNKNOWN && s->vsum[i] == sum && s->vpa[i] == pa)
        return s->vstate[i] == V_OK;
    bool ok = hash_ram(m->rdram, pa, e->len) == e->hash;
    if (s->vstate[i] != V_UNKNOWN) {
        s->stats.rehashes++;
        if (!ok && s->vstate[i] == V_OK)
            s->stats.stale++;
    }
    s->vstate[i] = ok ? V_OK : V_BAD;
    s->vsum[i] = sum;
    s->vpa[i] = pa;
    return ok;
}

static inline rc_line *line_of(struct n64_rc_state *s, uint32_t pc)
{
    return &s->cache[((pc >> 2) ^ (pc >> (CACHE_BITS + 2))) & (CACHE_SIZE - 1)];
}

/* The entry to run at pc (index; *pbase the physical address of its start), or -1 and *run: rc_line.run. pc_pa: pc's
 * physical address. */
static int32_t lookup(n64_machine *m, struct n64_rc_state *s, uint32_t pc, uint32_t pc_pa, uint32_t *pbase,
                      uint32_t *run)
{
    rc_line *ln = line_of(s, pc);
    bool direct = is_direct(pc);
    uint32_t tg = direct ? 0 : tlb_key(m);
    if (ln->key == pc && ln->tg == tg) {
        if (ln->e >= 0) {
            if (entry_valid(m, s, (uint32_t)ln->e, ln->pbase)) {
                *pbase = ln->pbase;
                return ln->e;
            }
        } else if (ln->e == E_NONE || page_sum(m, ln->p0, ln->p1) == ln->sum) {
            *run = ln->run;
            return -1;
        }
    }
    s->stats.slow_lookups++;
    uint32_t page = pc >> 12, lo = 0, hi = s->nrefs;
    while (lo < hi) { /* first ref with page >= pc's */
        uint32_t mid = lo + (hi - lo) / 2;
        if (s->refs[mid].page < page)
            lo = mid + 1;
        else
            hi = mid;
    }
    int32_t found = -1;
    bool any = false;
    uint32_t p0 = 0xFFFFFFFFu, p1 = 0, fbase = 0;
    for (uint32_t k = lo; k < s->nrefs && s->refs[k].page == page; k++) {
        uint32_t i = s->refs[k].e;
        const n64_recomp_entry *e = &s->table->entries[i];
        if (pc - e->addr >= e->len)
            continue;
        uint32_t base = pc_pa - (pc - e->addr); /* direct: contiguous; mapped: the same page */
        if (base + e->len > m->rdram_size || base > pc_pa)
            continue;
        any = true;
        if ((base >> 12) < p0)
            p0 = base >> 12;
        if (((base + e->len - 1) >> 12) > p1)
            p1 = (base + e->len - 1) >> 12;
        if (entry_valid(m, s, i, base)) {
            found = (int32_t)i;
            fbase = base;
            break;
        }
    }
    ln->key = pc;
    ln->tg = tg;
    ln->rec_n = 0;
    if (found >= 0) {
        ln->e = found;
        ln->pbase = fbase;
        *pbase = fbase;
        return found;
    }
    /* Up to the first entry that starts after pc in its page and whose code is there (entries from earlier pages start
     * before pc: they hold pc, and are the stale candidates above, or end before it), else the page's end. An entry
     * whose code is not there (data taken for code by the static pass, another overlay) does not cut the run: like a
     * stale candidate its pages join the line's check, so the run is worked out again once they are written. */
    uint32_t end = PAGE - (pc & (PAGE - 1));
    for (uint32_t k = lo; k < s->nrefs && s->refs[k].page == page; k++) {
        uint32_t i = s->refs[k].e;
        const n64_recomp_entry *e = &s->table->entries[i];
        if (e->addr <= pc || e->addr - pc >= end)
            continue;
        uint32_t base = pc_pa + (e->addr - pc);
        if (base + e->len > m->rdram_size)
            continue; /* cannot be there at this mapping */
        if (entry_valid(m, s, i, base)) {
            end = e->addr - pc;
            continue;
        }
        any = true;
        if ((base >> 12) < p0)
            p0 = base >> 12;
        if (((base + e->len - 1) >> 12) > p1)
            p1 = (base + e->len - 1) >> 12;
    }
    if (!any) {
        ln->e = E_NONE;
    } else {
        ln->e = E_STALE;
        ln->p0 = p0;
        ln->p1 = p1;
        ln->sum = page_sum(m, p0, p1);
    }
    ln->run = end / 4;
    *run = ln->run;
    return found;
}

/* Whether no entry's code is in the KSEG0 / KSEG1 page of va (direct) now: s->pfree, worked out again when a page
 * the entries touching it cover was written. */
static bool page_free(n64_machine *m, struct n64_rc_state *s, uint32_t va)
{
    uint32_t np = s->region.npages, pg = (va & 0x1FFFFFFFu) >> 12;
    if (pg >= np)
        return false;
    uint32_t idx = ((va - 0x80000000u) >> 29) * np + pg;
    rc_page *g = &s->pages[idx];
    if (!g->nref)
        return true;
    if (g->known && page_sum(m, g->p0, g->p1) == g->sum)
        return s->pfree[idx] != 0;
    bool free_ = true;
    uint32_t p0 = 0xFFFFFFFFu, p1 = 0;
    for (uint32_t k = g->r0; k < g->r0 + g->nref; k++) {
        uint32_t i = s->refs[k].e;
        const n64_recomp_entry *e = &s->table->entries[i];
        uint32_t base = e->addr & 0x1FFFFFFFu;
        if (base + e->len > m->rdram_size)
            continue;
        if ((base >> 12) < p0)
            p0 = base >> 12;
        if (((base + e->len - 1) >> 12) > p1)
            p1 = (base + e->len - 1) >> 12;
        if (entry_valid(m, s, i, base)) {
            free_ = false;
            break; /* covered while that entry is there: its pages are in [p0, p1] */
        }
    }
    if (p0 > p1)
        p0 = p1 = pg; /* no entry fits: nothing ever runs here */
    g->p0 = p0;
    g->p1 = p1;
    g->sum = page_sum(m, p0, p1);
    g->known = 1;
    s->pfree[idx] = free_;
    return free_;
}

/* ---- the missed record ---- */

/* The slot of addr, NULL when it has none. */
static const rc_addr *set_find(const rc_set *t, uint32_t addr)
{
    if (!t->slots)
        return NULL;
    uint32_t i = (addr * 2654435761u >> 8) & t->mask;
    for (uint32_t probes = 0; probes <= t->mask; probes++) {
        const rc_addr *a = &t->slots[i];
        if (!a->used)
            return NULL;
        if (a->addr == addr)
            return a;
        i = (i + 1) & t->mask;
    }
    return NULL;
}

/* pc: virtual, pa: where it is in RDRAM now. The window ends at the end of RDRAM, and of the page when mapped. A
 * record is new only when no record of pc agrees with memory over their common length: the same code reached again
 * (as a view into another record, or recorded from further) is recorded once, and only contents that differ count
 * as variants of the address (N64_RECOMP_VARIANTS of them: code generated at run time); views are never counted. */
static void record_missed(n64_machine *m, struct n64_rc_state *s, uint32_t pc, uint32_t pa)
{
    rc_set *t = &s->rec;
    if (t->n >= t->cap || t->bytes >= MISSED_BYTES)
        return;
    uint32_t n = N64_RECOMP_SNAPSHOT / 4;
    if (pa + n * 4 > m->rdram_size)
        n = (m->rdram_size - pa) / 4;
    if (!is_direct(pc) && (pc & (PAGE - 1)) + n * 4 > PAGE)
        n = (PAGE - (pc & (PAGE - 1))) / 4;
    if (!n)
        return;
    rc_addr *a = set_addr(t, pc);
    if (!a || a->var >= N64_RECOMP_VARIANTS)
        return; /* code rewritten over and over at this address: left to the interpreter */
    uint32_t buf[N64_RECOMP_SNAPSHOT / 4];
    for (uint32_t j = 0; j < n; j++)
        buf[j] = n64_be32(m->rdram + pa + 4 * j);
    if (set_agrees(t, a, buf, n))
        return;
    /* Inside a recent record whose bytes are still there: a view into its copy. */
    for (uint32_t k = t->n, seen = 0; k > 0 && seen < 32; k--, seen++) {
        const rc_missed *r = &t->r[k - 1];
        if (r->owner && pc - r->addr < r->n * 4u && (pc - r->addr) / 4 + 8 <= r->n) {
            uint32_t w = (pc - r->addr) / 4, len = r->n - w;
            if (len <= n && !memcmp(r->words + w, buf, (size_t)len * 4)) {
                set_add(t, a, len, n64_recomp_hash(r->words + w, len), r->words + w, false);
                return;
            }
        }
    }
    uint32_t *w = malloc((size_t)n * 4);
    if (!w)
        return;
    memcpy(w, buf, (size_t)n * 4);
    set_add(t, a, n, n64_recomp_hash(buf, n), w, true);
    a->var++;
}

/* The counter sum of the RDRAM pages record_missed reads at pa (its window and the words it compares: at most
 * N64_RECOMP_SNAPSHOT bytes from pa). */
static inline uint32_t rec_window(const n64_machine *m, uint32_t pa)
{
    uint32_t end = pa + N64_RECOMP_SNAPSHOT;
    if (end > m->rdram_size)
        end = m->rdram_size;
    return page_sum(m, pa >> 12, (end - 1) >> 12);
}

/* record_missed at a miss, skipped when it is sure to change nothing: it changed nothing the last time at this pc (the
 * line), and neither the records (their count) nor the bytes it reads (the window's page counters) moved since. Its
 * outcome depends on nothing else, so the record is the same as with a call at every miss. */
static inline void record_miss(n64_machine *m, struct n64_rc_state *s, uint32_t pc, uint32_t pa)
{
    rc_line *ln = line_of(s, pc); /* lookup left pc's line there */
    uint32_t sum = rec_window(m, pa), before = s->rec.n;
    if (ln->key == pc && ln->rec_n == before + 1 && ln->rec_sum == sum)
        return;
    record_missed(m, s, pc, pa);
    if (ln->key == pc && s->rec.n == before) {
        ln->rec_n = before + 1;
        ln->rec_sum = sum;
    }
}

size_t n64_recomp_missed(n64_machine *m, uint32_t *out, size_t max)
{
    struct n64_rc_state *s = m->rc;
    if (!s)
        return 0;
    size_t n = 0;
    for (uint32_t i = 0; i < s->rec.n && n < max; i++) {
        const rc_addr *a = set_find(&s->rec, s->rec.r[i].addr);
        if (a && a->head1 == i + 1)
            out[n++] = s->rec.r[i].addr;
    }
    return s->rec.naddr;
}

static int hexval(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* The discovered file's records into s->file (read once per path; again when its size is not what this machine left:
 * someone else wrote it). Returns false when out of memory. */
static bool load_file(struct n64_rc_state *s, const char *path)
{
    set_free(&s->file);
    free(s->file_path);
    s->file_path = strdup(path);
    s->file_upto = 0;
    s->file_size = 0;
    if (!s->file_path)
        return false;
    char *text = NULL;
    size_t len = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        if (fseek(f, 0, SEEK_END) == 0) {
            long sz = ftell(f);
            if (sz > 0 && fseek(f, 0, SEEK_SET) == 0 && (text = malloc((size_t)sz + 1)))
                len = fread(text, 1, (size_t)sz, f);
        }
        fclose(f);
    }
    uint32_t lines = 0;
    for (size_t i = 0; i < len; i++)
        lines += text[i] == '\n';
    uint32_t cap = lines + 1 + MISSED_MAX, slots = 1;
    while (slots < 2 * cap)
        slots <<= 1;
    if (!set_init(&s->file, cap, slots)) {
        free(text);
        return false;
    }
    s->file_size = len;
    for (size_t i = 0; i < len;) {
        size_t e = i;
        while (e < len && text[e] != '\n')
            e++;
        /* ADDR HASH HEXBYTES */
        uint32_t addr = 0, digits = 0;
        size_t p = i;
        while (p < e && hexval(text[p]) >= 0 && digits < 8)
            addr = addr << 4 | (uint32_t)hexval(text[p++]), digits++;
        if (digits && p < e && text[p] == ' ' && text[i] != '#') {
            uint64_t hash = 0;
            p++;
            while (p < e && hexval(text[p]) >= 0)
                hash = hash << 4 | (uint64_t)hexval(text[p++]);
            size_t q = p < e && text[p] == ' ' ? p + 1 : e, d = q;
            while (d < e && hexval(text[d]) >= 0)
                d++;
            uint32_t n = (uint32_t)((d - q) / 8);
            if (n > N64_RECOMP_SNAPSHOT / 4)
                n = N64_RECOMP_SNAPSHOT / 4;
            uint32_t buf[N64_RECOMP_SNAPSHOT / 4];
            for (uint32_t k = 0; k < n; k++) {
                uint32_t w = 0;
                for (int c = 0; c < 8; c++)
                    w = w << 4 | (uint32_t)hexval(text[q + 8 * k + (size_t)c]);
                buf[k] = w;
            }
            rc_addr *a = n && s->file.n < s->file.cap ? set_addr(&s->file, addr) : NULL;
            uint32_t *words = a && !set_agrees(&s->file, a, buf, n) ? malloc((size_t)n * 4) : NULL;
            if (words) {
                memcpy(words, buf, (size_t)n * 4);
                set_add(&s->file, a, n, hash, words, true);
            }
        }
        i = e + 1;
    }
    free(text);
    return true;
}

int n64_recomp_write_missed(n64_machine *m, const char *path)
{
    struct n64_rc_state *s = m->rc;
    if (!s || !s->rec.n)
        return 0;
    struct stat st;
    size_t now = stat(path, &st) == 0 ? (size_t)st.st_size : 0;
    if (!s->file_path || strcmp(s->file_path, path) != 0 || now != s->file_size) {
        if (!load_file(s, path))
            return -1;
    }
    FILE *f = fopen(path, "a");
    if (!f)
        return -1;
    int written = 0;
    for (uint32_t i = s->file_upto; i < s->rec.n; i++) {
        const rc_missed *r = &s->rec.r[i];
        rc_addr *a = s->file.n < s->file.cap ? set_addr(&s->file, r->addr) : NULL;
        if (!a)
            break;
        /* A view: its first words only, the record it lies in (written before it) holds the rest. */
        uint32_t n = r->owner || r->n < VIEW_WORDS ? r->n : VIEW_WORDS;
        if (set_agrees(&s->file, a, r->words, n))
            continue; /* the file has this code (another session, a save before this one) */
        uint64_t hash = n == r->n ? r->hash : n64_recomp_hash(r->words, n);
        fprintf(f, "%08X %016llX ", r->addr, (unsigned long long)hash);
        for (uint32_t k = 0; k < n; k++)
            fprintf(f, "%08X", r->words[k]);
        fputc('\n', f);
        set_add(&s->file, a, n, hash, r->words, false);
        written++;
    }
    s->file_upto = s->rec.n;
    long end = ftell(f);
    if (fclose(f) != 0)
        return -1;
    s->file_size = end > 0 ? (size_t)end : 0;
    return written;
}

void n64_recomp_get_stats(n64_machine *m, n64_recomp_stats *out)
{
    memset(out, 0, sizeof *out);
    if (m->rc)
        *out = m->rc->stats;
    out->native = m->rc_native;
}

/* ---- the run loop ---- */

void n64_recomp_run(n64_machine *m)
{
    struct n64_rc_state *s = m->rc;
    n64_cpu *c = &m->cpu;
    const n64_recomp_table *t = s->table;
    while (m->now < m->stop_at) {
        if (c->irq) {
            s->stats.interrupts++;
            n64_cpu_take_interrupt(m);
            continue;
        }
        uint64_t pc = c->pc;
        uint32_t va = (uint32_t)pc, pa;
        if (N64_UNLIKELY(!s->booted)) {
            if (pc != (uint64_t)(int64_t)(int32_t)t->boot_pc) {
                n64_cpu_run_until(m, (uint64_t)(int64_t)(int32_t)t->boot_pc);
                continue;
            }
            s->booted = true;
        }
        if ((pc & 3) || pc != (uint64_t)(int64_t)(int32_t)va || c->ksu != 0 || !translate(m, va, &pa)) {
            n64_cpu_step(m); /* not RDRAM code in kernel mode: IPL3 in DMEM, the ROM, user mode, a fault to come */
            continue;
        }
        if (pc == s->interp_pc || c->next_ds) {
            /* the instruction lifted code left to the interpreter (after the loop's checks), or a delay slot; then
             * the rest of its block while execution stays in sequence (inside a block nothing is a resume point) */
            bool handed = pc == s->interp_pc;
            s->interp_pc = 1;
            s->expect = 1;
            if (handed) { /* the handed run, in the interpreter's inlined loop while it stays in sequence */
                bool chained;
                uint32_t n = n64_cpu_step_handed(m, &s->interp_left, &chained);
                s->stats.fallbacks += n;
                s->stats.interp_ram += n;
                if (chained)
                    s->interp_pc = c->pc;
                continue;
            }
            s->stats.interp_ram++;
            n64_cpu_step(m);
            s->expect = c->cur + 4; /* the branch taken: its target is recorded; not taken: execution goes on */
            continue;
        }
        uint32_t base = 0, run = 1;
        int32_t e = lookup(m, s, va, pa, &base, &run);
        if (e >= 0) {
            const n64_recomp_entry *en = &t->entries[e];
            m->rc_lo = base;
            m->rc_len = en->len;
            m->rc_hit = 0;
            m->rc_exit = N64_RC_EXIT_NORMAL;
            m->rc_hint = 0;
            en->fn(m);
            m->rc_len = 0;
            c->gpr[0] = 0;
            uint8_t why = m->rc_exit;
            m->rc_exit = N64_RC_EXIT_NORMAL;
            if (why != N64_RC_EXIT_NOT_COVERED) {
                s->stats.calls++;
                if (why == N64_RC_EXIT_INTERP) {
                    s->interp_pc = c->pc;
                    s->interp_left = m->rc_hint ? m->rc_hint : 1;
                }
                s->expect = 1;
                continue;
            }
        } else {
            bool missed = pc != s->expect;
            if (missed)
                record_miss(m, s, va, pa);
            if (N64_LIKELY(!m->interp_ref && !(m->mi_mode & 0x80) && c->npc == pc + 4) && s->interp_pc == 1 &&
                is_direct(va) && page_free(m, s, va)) {
                /* No entry's code in this page: the interpreter's chained blocks as n64_cpu_run runs them, without a
                 * lookup per block, until execution leaves the pages that hold no entry's code or reaches a branch
                 * target not recorded yet (then back here: it is recorded, lifted code entered where it is valid). */
                uint64_t ran = m->instructions;
                int r = n64_cpu_run_region(m, &s->region, missed);
                s->stats.blocks++;
                if (r == 1 && m->now < m->stop_at && !c->irq) {
                    n64_cpu_step(m);
                } else if (r == 2) {
                    while (m->now < m->stop_at && !c->irq)
                        n64_cpu_step(m);
                }
                s->stats.interp_ram += m->instructions - ran;
                /* Where the run left the pages or met a branch target not recorded: the next pc is where code was
                 * entered (recorded when nothing covers it). Stopped at stop_at, an interrupt or a store: the flow
                 * goes on there (not an entry point). After a step: as after any step. */
                s->expect = r == 3 ? 1 : r == 1 ? c->cur + 4 : c->pc;
                continue;
            }
            if (N64_LIKELY(!m->interp_ref && !(m->mi_mode & 0x80) && c->npc == pc + 4)) {
                /* Code no entry covers: a predecoded block of the interpreter's (XBA-210) to the next entry's start at
                 * most, so lifted code is entered where step() would let it. Inside the block execution stays in
                 * sequence: no instruction of it would be recorded, and none but the first can be a resume point. */
                if (s->interp_pc - pc - 4 < 4ull * run - 4) /* a hand-off still pending ahead (an interrupt came) */
                    run = (uint32_t)((s->interp_pc - pc) / 4);
                uint64_t ran = m->instructions;
                bool step = n64_cpu_run_block(m, run);
                s->stats.blocks++;
                if (step && m->now < m->stop_at && !c->irq)
                    n64_cpu_step(m);
                s->stats.interp_ram += m->instructions - ran;
                s->expect = c->cur + 4; /* as after the last step: after a branch's delay slot, its target when taken */
                continue;
            }
        }
        s->stats.interp_ram++;
        n64_cpu_step(m);
        s->expect = pc + 4;
    }
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

int n64_cpu_compare(n64_machine *a, n64_machine *b, char *why, size_t why_n)
{
    char name[32];
    const n64_cpu *x = &a->cpu, *y = &b->cpu;
    CMP_FIELD("instructions", a->instructions, b->instructions);
    CMP_FIELD("now", a->now, b->now);
    CMP_FIELD("stop_at", a->stop_at, b->stop_at);
    for (int i = 0; i < 32; i++) {
        snprintf(name, sizeof name, "r%d", i);
        CMP_FIELD(name, x->gpr[i], y->gpr[i]);
    }
    CMP_FIELD("pc", x->pc, y->pc);
    CMP_FIELD("npc", x->npc, y->npc);
    CMP_FIELD("cur", x->cur, y->cur);
    CMP_FIELD("ds", x->ds, y->ds);
    CMP_FIELD("delay slot", x->next_ds, y->next_ds);
    CMP_FIELD("irq", x->irq, y->irq);
    CMP_FIELD("hi", x->hi, y->hi);
    CMP_FIELD("lo", x->lo, y->lo);
    CMP_FIELD("llbit", x->llbit, y->llbit);
    for (int i = 0; i < 32; i++) {
        snprintf(name, sizeof name, "cop0 %d", i);
        CMP_FIELD(name, x->cop0[i], y->cop0[i]);
    }
    for (int i = 0; i < 32; i++) {
        snprintf(name, sizeof name, "f%d", i);
        CMP_FIELD(name, x->fpr[i], y->fpr[i]);
    }
    CMP_FIELD("fcsr", x->fcsr, y->fcsr);
    return 0;
}

int n64_recomp_compare(n64_machine *a, n64_machine *b, char *why, size_t why_n)
{
    char name[32];
    const n64_cpu *x = &a->cpu, *y = &b->cpu;
    CMP_FIELD("instructions", a->instructions, b->instructions);
    CMP_FIELD("now", a->now, b->now);
    for (int i = 0; i < 32; i++) {
        snprintf(name, sizeof name, "r%d", i);
        CMP_FIELD(name, x->gpr[i], y->gpr[i]);
    }
    CMP_FIELD("pc", x->pc, y->pc);
    CMP_FIELD("npc", x->npc, y->npc);
    CMP_FIELD("delay slot", x->next_ds, y->next_ds);
    CMP_FIELD("hi", x->hi, y->hi);
    CMP_FIELD("lo", x->lo, y->lo);
    CMP_FIELD("llbit", x->llbit, y->llbit);
    for (int i = 0; i < 32; i++) {
        if (i == C0_COUNT || i == C0_RANDOM)
            continue;
        snprintf(name, sizeof name, "cop0 %d", i);
        CMP_FIELD(name, x->cop0[i], y->cop0[i]);
    }
    CMP_FIELD("COUNT", n64_cpu_count(a), n64_cpu_count(b));
    for (int i = 0; i < 32; i++) {
        snprintf(name, sizeof name, "tlb %d", i);
        CMP_FIELD(name, x->tlb[i].hi, y->tlb[i].hi);
        CMP_FIELD(name, x->tlb[i].lo[0] ^ ((uint64_t)x->tlb[i].lo[1] << 32),
                  y->tlb[i].lo[0] ^ ((uint64_t)y->tlb[i].lo[1] << 32));
        CMP_FIELD(name, x->tlb[i].mask, y->tlb[i].mask);
    }
    for (int i = 0; i < 32; i++) {
        snprintf(name, sizeof name, "f%d", i);
        CMP_FIELD(name, x->fpr[i], y->fpr[i]);
    }
    CMP_FIELD("fcsr", x->fcsr, y->fcsr);
    for (int i = 0; i < EV_COUNT; i++) {
        snprintf(name, sizeof name, "event %d", i);
        CMP_FIELD(name, a->ev[i], b->ev[i]);
    }
    CMP_FIELD("frames", a->frames, b->frames);
    CMP_FIELD("MI_INTR", a->mi_intr, b->mi_intr);
    CMP_FIELD("MI_MASK", a->mi_mask, b->mi_mask);
    CMP_FIELD("MI_MODE", a->mi_mode, b->mi_mode);
    CMP_FIELD("rdram size", a->rdram_size, b->rdram_size);
    if (cmp_mem("rdram", a->rdram, b->rdram, a->rdram_size, why, why_n) ||
        cmp_mem("rsp mem", a->spmem, b->spmem, 0x2000, why, why_n))
        return 1;
    return 0;
}
