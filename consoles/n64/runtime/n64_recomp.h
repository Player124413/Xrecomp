/* n64_recomp.h -- static recompilation of Nintendo 64 games (Plane N-1 XBA-169, N-2 XBA-170, N-3 XBA-171).
 *
 * An N64 title pack (libtitle.so, pack.json with "console": "n64") holds the game's VR4300 code lifted to C by
 * consoles/n64/tools (n64_tools) and compiled for the host. Universal: no per-title configuration, no symbol or ELF
 * file (the ROM alone). The runtime attaches the pack's table to the machine: when the CPU reaches code the table
 * covers, the recompiled function runs natively; anything else (code the static pass did not find, code loaded or
 * generated at run time and not lifted yet, IPL3) runs in the interpreter, which records the code it had to run,
 * bytes included, so the next Prepare lifts it (the discovery loop). RSP, RDP, VI, AI, PI, SI and the PIF are the
 * same code either way.
 *
 * N64 code runs from RDRAM: IPL3 copies the boot segment (or libdragon's ELF) there, libultra / libdragon copy more
 * code by PI DMA, games load overlays over each other, and some map code through the TLB at virtual addresses. An
 * address alone does not say which code is there, so every table entry names its code by virtual address AND
 * content: a range of words and their hash. The core keeps a write counter per 4 KB page of RDRAM (CPU stores,
 * lifted stores, PI DMA, SI DMA and SP (RSP) DMA bump it); an entry is re-hashed only when a page it covers was
 * written since it was last checked, and runs only while its hash matches. Several entries may share an address
 * (overlays): the one whose hash matches the memory runs, never stale code.
 *
 * Addresses: an entry's address is the 32-bit virtual address its code runs at (sign-extended to 64 bits by the
 * CPU): KSEG0 / KSEG1 (direct-mapped RDRAM), or a TLB-mapped address (KUSEG, KSSEG, KSEG3). A mapped entry lies
 * within one 4 KB virtual page (n64_tools cuts functions at page boundaries there), so one translation covers all
 * of it: the dispatcher translates the PC with the current TLB, ASID and mode, checks the bytes at the physical
 * address it maps to, and caches the answer until the TLB changes (TLBWI / TLBWR / EntryHi) or the page is written.
 * Lifted code runs only in kernel mode (the N64 runs everything in kernel mode) and never touches the TLB or COP0
 * itself, so the mapping cannot change under a running function.
 *
 * Contract (frozen between n64_tools, Prepare, the loader and the core; change together and bump N64_RECOMP_ABI):
 * - `python3 -m n64_tools.recomp ROM OUTDIR [--discovered FILE]` writes C sources into OUTDIR (nothing else needed to
 *   build): compile each of them with -I consoles/n64/runtime into one shared library. ROM in any byte order.
 * - That library exports `n64_recomp_title_table` (pack.json "entry"), returning the table below.
 * - The library is linked against the core's symbols (n64_recomp_rt.h; the shell / n64_run provide them at load).
 * - The discovered file (--discovered; the runtime writes it, n64_recomp_write_missed) is text, one record per line,
 *   `#` comments:  `ADDR HASH HEXBYTES`  -- ADDR the 32-bit virtual address (hex) where the interpreter had to start,
 *   HASH n64_recomp_hash of the words recorded (16 hex digits), HEXBYTES the code bytes from ADDR as they were in
 *   memory then (big-endian, the console's order; up to N64_RECOMP_SNAPSHOT bytes, never past the end of a mapped
 *   page; a record that lies inside an earlier one of the file with the same bytes holds only its first words). A
 *   line with ADDR alone names code of the ROM's static images. n64_tools lifts from the recorded bytes, so
 *   code that is in no static image (overlays, decompressed code) gets lifted. An address recorded with
 *   N64_RECOMP_VARIANTS different contents is code generated at run time: it is recorded no more and not lifted.
 *   Records of an address that agree over their common length are the same code: recorded once (n64_tools merges
 *   them too). */
#ifndef N64_RECOMP_H
#define N64_RECOMP_H

#include <stddef.h>
#include <stdint.h>

#include "n64_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define N64_RECOMP_ABI 1
#define N64_RECOMP_SNAPSHOT 2048u /* bytes of code recorded per missed address at most */
#define N64_RECOMP_VARIANTS 8     /* contents recorded per address at most: more is code generated at run time */

/* Entered with the CPU at an instruction of the entry's range (not a delay slot, kernel mode); returns when control
 * leaves the recompiled code, with the CPU state set to where execution continues. See n64_recomp_rt.h. */
typedef void (*n64_recomp_fn)(n64_machine *m);

typedef struct n64_recomp_entry {
    uint32_t addr;  /* virtual address of the first instruction (4-aligned; low 32 bits of the sign-extended PC) */
    uint32_t len;   /* bytes of code from addr (a multiple of 4; within one 4 KB page for a TLB-mapped address) */
    uint64_t hash;  /* n64_recomp_hash of those words: the function runs only while memory holds them */
    n64_recomp_fn fn;
} n64_recomp_entry;

typedef struct n64_recomp_table {
    uint32_t abi;               /* N64_RECOMP_ABI */
    uint32_t count;             /* entries, sorted by addr (several may share an address: overlays) */
    const n64_recomp_entry *entries;
    const char *source_sha256;  /* the ROM the table was built from (big-endian order), lower-case hex */
    uint32_t boot_pc;           /* the game's entry point (IPL3 jumps there): lifted code runs, and missed code is
                                 * recorded, only once the CPU got there (before, it is IPL3) */
} n64_recomp_table;

/* Exported by an N64 pack library. */
typedef const n64_recomp_table *(*n64_recomp_title_table_fn)(void);
#define N64_RECOMP_ENTRY_SYMBOL "n64_recomp_title_table"

/* The content hash of n 32-bit words (their big-endian values): FNV-1a 64 over the words (h ^= w; h *=
 * 0x100000001B3, from 0xCBF29CE484222325). n64_tools computes the same. */
uint64_t n64_recomp_hash(const uint32_t *words, size_t n);

/* Attaches a table (NULL detaches: pure interpreter). Returns 0, or -1 when its ABI differs. */
int n64_attach_recomp(n64_machine *m, const n64_recomp_table *t);

/* Discovery: the distinct addresses where the interpreter had to start running RDRAM code (after the boot PC was
 * reached, in kernel mode; a function's start, a branch taken or jump into code no entry covers, not the code
 * that follows in sequence) that no attached entry matched. Copies at most max into out, returns the total count.
 * The record holds 65536 records and 64 MB of code at most. */
size_t n64_recomp_missed(n64_machine *m, uint32_t *out, size_t max);

/* Appends the missed records (the format above) to path, skipping records that agree with one the file already
 * holds (read once, then kept: it can be called often, at every pause, and appends only what is new). Returns the
 * records written, or -1 on an I/O error. */
int n64_recomp_write_missed(n64_machine *m, const char *path);

/* ---- Helpers for hosts and tests (not part of the pack ABI) ---- */

typedef struct n64_recomp_stats {
    uint64_t native;       /* instructions lifted code ran (only counted by packs built with -DN64_RC_COUNT) */
    uint64_t interp_ram;   /* RDRAM instructions the interpreter ran while a table was attached (after boot) */
    uint64_t fallbacks;    /* of those, instructions lifted code handed to the interpreter (COP0, exceptions, ...) */
    uint64_t calls;        /* calls into lifted code */
    uint64_t slow_lookups; /* dispatch cache misses */
    uint64_t rehashes;     /* entries re-hashed because a page they cover was written */
    uint64_t stale;        /* of those, entries whose code was no longer there (overlay swapped, code patched) */
    uint64_t interrupts;   /* interrupts taken */
    uint64_t blocks;       /* runs of predecoded blocks the dispatcher handed to the interpreter for code no entry
                            * covers (one block, or chained blocks in a page no entry's code is in; of interp_ram) */
} n64_recomp_stats;
void n64_recomp_get_stats(n64_machine *m, n64_recomp_stats *out);

/* Differential testing: 0 when two machines are in the same state (GPRs, HI / LO, PC / next PC / delay slot, LL bit,
 * COP0 (COUNT included) and the TLB, FPRs and FCSR, time, instruction count, scheduler, MI, RDRAM, RSP memory);
 * else 1 and the first difference in why. */
int n64_recomp_compare(n64_machine *a, n64_machine *b, char *why, size_t why_n);
/* The CPU part only (cheap: n64_run --lockstep-ref --lockstep-cycles compares it at every stop): GPRs, HI / LO, PC /
 * next PC / the delay-slot flags and the instruction being run (cur / ds), LL bit, COP0, FPRs, FCSR, time, instruction
 * count, stop_at. */
int n64_cpu_compare(n64_machine *a, n64_machine *b, char *why, size_t why_n);

#ifdef __cplusplus
}
#endif

#endif
