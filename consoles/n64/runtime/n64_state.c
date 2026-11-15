/* n64_state.c -- save states of the N64 machine (n64_core.h « Save states », XBA-183).
 *
 * The blob: a header (magic "XRSTATE", console "n64", N64_STATE_VERSION, the ROM's hash, RDRAM's size, the section
 * count), then sections (id, size, bytes) in order, each checked against this build before anything changes, so a
 * state from another build (another layout) or another RDRAM size is refused whole. Host byte order and sizes, like
 * the GBA's and the PS1's (gba_state.c, ps1_state.c): a state belongs to the build family that wrote it.
 *
 * Not state: the ROM (identified by its hash), the machine's options, the RDRAM page write counters of the recompiled
 * code (every page is bumped on load, so each lifted function is checked against RDRAM again before it runs), the
 * attached table's dispatch cache, the VI's picture path (n64_vi.c: canvas and line buffers, rebuilt from the next
 * picture), host pointers (devices, buffers, the tty sink), the RTC's test clock. A deferred DP interrupt (EV_DP,
 * n64_core.c) is completed before a save, so a state never holds one. */
#include "n64_internal.h"
#include "n64_rdp.h"

#include <stdlib.h>
#include <string.h>

#define STATE_MAGIC "XRSTATE\0"
#define HEADER_BYTES 40u
#define PAGE 4096u
#define ISV_SIZE 0x10000u

enum { SEC_MACHINE = 1, SEC_RDRAM, SEC_RSP, SEC_RDP, SEC_FLASH, SEC_ISV, SEC_RC, SEC_COUNT = SEC_RC };

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}
static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get64(const uint8_t *p)
{
    return get32(p) | (uint64_t)get32(p + 4) << 32;
}

/* The ROM (big-endian, as the machine holds it), 8 bytes at a time: computed once per machine (up to 64 MB). */
static uint64_t rom_hash(n64_machine *m)
{
    if (!m->rom_hashed) {
        uint64_t h = 0xCBF29CE484222325ull ^ m->rom_size;
        for (uint32_t i = 0; i + 8 <= m->rom_size; i += 8) {
            uint64_t w;
            memcpy(&w, m->rom + i, 8);
            h = (h ^ w) * 0x100000001B3ull;
            h ^= h >> 29;
        }
        m->rom_hash = h;
        m->rom_hashed = true;
    }
    return m->rom_hash;
}

static int page_zero(const uint8_t *p)
{
    static const uint8_t zero[PAGE];
    return memcmp(p, zero, PAGE) == 0;
}

/* RDRAM: a bitmap of the pages stored (bit set: not all zero), then those pages. */
static size_t rdram_bytes(n64_machine *m)
{
    uint32_t pages = m->rdram_size / PAGE;
    size_t n = (pages + 7) / 8;
    for (uint32_t i = 0; i < pages; i++)
        if (!page_zero(m->rdram + (size_t)i * PAGE))
            n += PAGE;
    return n;
}

static void rdram_save(n64_machine *m, uint8_t *out)
{
    uint32_t pages = m->rdram_size / PAGE;
    uint8_t *bits = out, *p = out + (pages + 7) / 8;
    memset(bits, 0, (pages + 7) / 8);
    for (uint32_t i = 0; i < pages; i++) {
        const uint8_t *src = m->rdram + (size_t)i * PAGE;
        if (page_zero(src))
            continue;
        bits[i >> 3] |= (uint8_t)(1u << (i & 7));
        memcpy(p, src, PAGE);
        p += PAGE;
    }
}

/* The bitmap's page count matches the section's size. */
static int rdram_check(n64_machine *m, const uint8_t *in, size_t n)
{
    uint32_t pages = m->rdram_size / PAGE;
    size_t head = (pages + 7) / 8, stored = 0;
    if (n < head)
        return -1;
    for (uint32_t i = 0; i < pages; i++)
        stored += (in[i >> 3] >> (i & 7)) & 1;
    return n == head + stored * PAGE ? 0 : -1;
}

static void rdram_load(n64_machine *m, const uint8_t *in)
{
    uint32_t pages = m->rdram_size / PAGE;
    const uint8_t *p = in + (pages + 7) / 8;
    for (uint32_t i = 0; i < pages; i++) {
        uint8_t *dst = m->rdram + (size_t)i * PAGE;
        if ((in[i >> 3] >> (i & 7)) & 1) {
            memcpy(dst, p, PAGE);
            p += PAGE;
        } else {
            memset(dst, 0, PAGE);
        }
    }
}

/* Section sizes as this build writes them now (RDRAM, the RDP, FlashRAM and the IS-Viewer vary). */
static size_t sec_size(n64_machine *m, int id)
{
    switch (id) {
    case SEC_MACHINE: return N64_MACHINE_STATE_BYTES;
    case SEC_RDRAM: return rdram_bytes(m);
    case SEC_RSP: return n64_rsp_state_size(m->rsp);
    case SEC_RDP: return n64_rdp_state_size(m->rdp);
    case SEC_FLASH: return m->flash ? N64_FLASH_SIZE : 0;
    case SEC_ISV: return m->isv ? ISV_SIZE : 0;
    case SEC_RC: return 4;
    default: return 0;
    }
}

size_t n64_state_size(n64_machine *m)
{
    if (!m)
        return 0;
    n64_rdp_flush(m->rdp); /* everything drawn is in RDRAM (its zero pages are counted) */
    size_t n = HEADER_BYTES;
    for (int id = 1; id <= SEC_COUNT; id++)
        n += 8 + sec_size(m, id);
    return n;
}

static void save_machine(n64_machine *m, uint8_t *out)
{
    memcpy(out, m, N64_MACHINE_STATE_BYTES); /* the host-only tail (blocks, interp_ref) left out */
    /* Host pointers and host-side fields: not the machine's (the blob may not be aligned: by offset). */
#define CLEAR(f) do { size_t n_ = sizeof m->f; memset(out + offsetof(n64_machine, f), 0, n_); } while (0)
    CLEAR(rdram);
    CLEAR(rdram_gen);
    CLEAR(rc);
    CLEAR(rom);
    CLEAR(rsp);
    CLEAR(rdp);
    CLEAR(spmem);
    CLEAR(vis);
    CLEAR(flash);
    CLEAR(isv);
    CLEAR(tty);
    CLEAR(tty_ctx);
    CLEAR(rom_hash);
    CLEAR(rom_hashed);
#undef CLEAR
}

size_t n64_state_save(n64_machine *m, uint8_t *out, size_t cap)
{
    if (!m || !out)
        return 0;
    n64_dp_complete_now(m); /* a deferred DP interrupt comes now: a state never holds one (n64_core.c) */
    n64_rdp_flush(m->rdp);  /* everything drawn is in RDRAM */
    size_t need = n64_state_size(m);
    if (cap < need)
        return 0;
    memset(out, 0, HEADER_BYTES);
    memcpy(out, STATE_MAGIC, 8);
    memcpy(out + 8, "n64", 4);
    put32(out + 12, N64_STATE_VERSION);
    put64(out + 16, rom_hash(m));
    put32(out + 24, m->rdram_size);
    put32(out + 28, SEC_COUNT);
    uint8_t *p = out + HEADER_BYTES;
    for (int id = 1; id <= SEC_COUNT; id++) {
        size_t n = sec_size(m, id);
        put32(p, (uint32_t)id);
        put32(p + 4, (uint32_t)n);
        p += 8;
        switch (id) {
        case SEC_MACHINE: save_machine(m, p); break;
        case SEC_RDRAM: rdram_save(m, p); break;
        case SEC_RSP: n64_rsp_state_save(m->rsp, p); break;
        case SEC_RDP: n64_rdp_state_save(m->rdp, p); break;
        case SEC_FLASH: if (n) memcpy(p, m->flash, n); break;
        case SEC_ISV: if (n) memcpy(p, m->isv, n); break;
        case SEC_RC: put32(p, (uint32_t)n64_recomp_booted(m)); break;
        }
        p += n;
    }
    return need;
}

/* Walks the sections: each id once, in order, sizes this build's (the RDP's checked by its own load). Fills at[] with
 * each section's bytes. */
static int parse(n64_machine *m, const uint8_t *data, size_t size, const uint8_t *at[], size_t len[])
{
    if (!m || !data || size < HEADER_BYTES || memcmp(data, STATE_MAGIC, 8) != 0 || memcmp(data + 8, "n64", 4) != 0)
        return N64_STATE_ECORRUPT;
    if (get32(data + 12) != N64_STATE_VERSION)
        return N64_STATE_EVERSION;
    if (get64(data + 16) != rom_hash(m))
        return N64_STATE_EGAME;
    if (get32(data + 24) != m->rdram_size || get32(data + 28) != SEC_COUNT)
        return N64_STATE_EVERSION;
    size_t off = HEADER_BYTES;
    for (int id = 1; id <= SEC_COUNT; id++) {
        if (size - off < 8)
            return N64_STATE_ECORRUPT;
        uint32_t sid = get32(data + off), n = get32(data + off + 4);
        off += 8;
        if (sid != (uint32_t)id || n > size - off)
            return N64_STATE_ECORRUPT;
        int ok;
        switch (id) {
        case SEC_MACHINE: ok = n == N64_MACHINE_STATE_BYTES; break;
        case SEC_RDRAM: ok = rdram_check(m, data + off, n) == 0; break;
        case SEC_RSP: ok = n == n64_rsp_state_size(m->rsp); break;
        case SEC_RDP: ok = 1; break; /* its own load checks it, first */
        case SEC_FLASH: ok = n == 0 || n == N64_FLASH_SIZE; break;
        case SEC_ISV: ok = n == 0 || n == ISV_SIZE; break;
        default: ok = n == 4; break;
        }
        if (!ok)
            return id == SEC_RDRAM ? N64_STATE_ECORRUPT : N64_STATE_EVERSION;
        at[id] = data + off;
        len[id] = n;
        off += n;
    }
    return N64_STATE_OK;
}

int n64_state_check(n64_machine *m, const uint8_t *data, size_t size)
{
    const uint8_t *at[SEC_COUNT + 1];
    size_t len[SEC_COUNT + 1];
    return parse(m, data, size, at, len);
}

int n64_state_load(n64_machine *m, const uint8_t *data, size_t size)
{
    const uint8_t *at[SEC_COUNT + 1];
    size_t len[SEC_COUNT + 1];
    int rc = parse(m, data, size, at, len);
    if (rc != N64_STATE_OK)
        return rc;
    /* What may still fail first: the buffers the state needs, then the RDP (the only section its own load checks). */
    uint8_t *flash = m->flash, *isv = m->isv;
    if (len[SEC_FLASH] && !flash && !(flash = malloc(N64_FLASH_SIZE)))
        return N64_STATE_ECORRUPT;
    if (len[SEC_ISV] && !isv && !(isv = calloc(1, ISV_SIZE))) {
        if (flash != m->flash)
            free(flash);
        return N64_STATE_ECORRUPT;
    }
    if (n64_rdp_state_load(m->rdp, at[SEC_RDP], len[SEC_RDP]) != 0) {
        if (flash != m->flash)
            free(flash);
        if (isv != m->isv)
            free(isv);
        return N64_STATE_EVERSION;
    }
    n64_rsp_state_load(m->rsp, at[SEC_RSP], len[SEC_RSP]); /* its size was checked */

    /* The machine, keeping this one's host side. */
    uint8_t *rdram = m->rdram, *rom = m->rom, *spmem = m->spmem;
    uint32_t *rdram_gen = m->rdram_gen;
    uint32_t rdram_size = m->rdram_size, rom_size = m->rom_size;
    struct n64_rc_state *rcs = m->rc;
    n64_rom_info info = m->info;
    n64_options opts = m->opts;
    n64_rsp *rsp = m->rsp;
    n64_rdp *rdp = m->rdp;
    struct n64_vi_state *vis = m->vis;
    void (*tty)(void *, char) = m->tty;
    void *tty_ctx = m->tty_ctx;
    int64_t rtc_fixed = m->rtc_fixed;
    uint64_t hash = m->rom_hash;
    bool hashed = m->rom_hashed;
    memcpy(m, at[SEC_MACHINE], N64_MACHINE_STATE_BYTES); /* the host-only tail stays */
    m->rdram = rdram;
    m->rdram_gen = rdram_gen;
    m->rdram_size = rdram_size;
    m->rc = rcs;
    m->rom = rom;
    m->rom_size = rom_size;
    m->info = info;
    m->opts = opts;
    m->rsp = rsp;
    m->rdp = rdp;
    m->spmem = spmem;
    m->vis = vis;
    m->tty = tty;
    m->tty_ctx = tty_ctx;
    m->rtc_fixed = rtc_fixed;
    m->rom_hash = hash;
    m->rom_hashed = hashed;
    m->save_dirty = false;
    for (int p = 0; p < 4; p++)
        m->pak_dirty[p] = false;

    m->flash = flash;
    if (len[SEC_FLASH])
        memcpy(m->flash, at[SEC_FLASH], N64_FLASH_SIZE);
    else if (m->flash)
        memset(m->flash, 0xFF, N64_FLASH_SIZE); /* erased, as before the game first used it */
    if (len[SEC_ISV]) {
        m->isv = isv;
        memcpy(m->isv, at[SEC_ISV], ISV_SIZE);
    } else {
        free(isv);
        m->isv = NULL;
    }
    rdram_load(m, at[SEC_RDRAM]);
    for (uint32_t p = 0; p < (m->rdram_size >> N64_PAGE_SHIFT); p++)
        m->rdram_gen[p]++; /* new contents everywhere, as far as lifted code is concerned */
    n64_vi_free(m); /* the picture path starts afresh at the next picture */
    n64_dp_state_loaded(m); /* the deferred DP interrupt's policy too (the RDP's load dropped any pending one) */
    n64_recomp_state_loaded(m, (int32_t)get32(at[SEC_RC]));
    return N64_STATE_OK;
}
