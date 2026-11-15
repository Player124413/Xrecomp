/* N64 core unit tests (XBA-169, XBA-172): ROM byte orders and header, the HLE PIF boot state, joybus (controllers,
 * controller pak with its CRC, rumble pak, EEPROM), SRAM and FlashRAM through the PI, PI DMA, the AI (queue,
 * interrupt, 48 kHz output), the VI picture and save states (XBA-183). Register sequences and data are made here; no
 * ROM files. */
#include "n64_internal.h"
#include "n64_rdp.h"
#include "n64_recomp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;

#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        checks++;                                                                                                     \
        if (!(cond)) {                                                                                                \
            fails++;                                                                                                  \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                               \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
        }                                                                                                             \
    } while (0)
#define CHECK_EQ(got, want, what)                                                                                     \
    CHECK((uint64_t)(got) == (uint64_t)(want), "%s = 0x%llX, want 0x%llX", what, (unsigned long long)(got),          \
          (unsigned long long)(want))

#define ROM_SIZE 0x10000

static void make_rom(uint8_t *rom)
{
    memset(rom, 0, ROM_SIZE);
    static const uint8_t hdr[] = {0x80, 0x37, 0x12, 0x40, 0x00, 0x00, 0x00, 0x0F, 0x80, 0x00, 0x04, 0x00,
                                  0x00, 0x00, 0x14, 0x4C, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};
    memcpy(rom, hdr, sizeof hdr);
    memcpy(rom + 0x20, "XBA TEST ROM        ", 20);
    memcpy(rom + 0x3B, "NXTP", 4); /* region 'P': PAL */
    rom[0x3F] = 2;
    memcpy(rom + 0x40 + 0x300, " Libdragon IPL3 ", 16);
    for (int i = 0; i < 16; i++)
        rom[0x2000 + i] = (uint8_t)(0xA0 + i);
}

static n64_machine *mk(const n64_options *o)
{
    static uint8_t rom[ROM_SIZE];
    make_rom(rom);
    char err[128];
    n64_machine *m = n64_create(rom, ROM_SIZE, o, err, sizeof err);
    if (!m) {
        printf("FAIL: n64_create: %s\n", err);
        exit(1);
    }
    m->cpu.cop0[C0_STATUS] = 0x34000000u; /* interrupts off: these tests drive the bus directly */
    n64_cpu_status_changed(m);
    n64_poke32(m, 0x80000800u, 0x08000200u); /* j 0x80000800 */
    n64_poke32(m, 0x80000804u, 0);
    m->cpu.pc = 0xFFFFFFFF80000800ull;
    m->cpu.npc = m->cpu.pc + 4;
    return m;
}

static void w32(n64_machine *m, uint32_t pa, uint32_t v) { n64_bus_write32(m, pa, v, 0xFFFFFFFFu); }
static uint32_t r32(n64_machine *m, uint32_t pa) { return n64_bus_read32(m, pa); }

/* ---- ROM ------------------------------------------------------------------------------------------------------- */

static void test_rom(void)
{
    static uint8_t z[ROM_SIZE], v[ROM_SIZE], n[ROM_SIZE];
    make_rom(z);
    for (int i = 0; i < ROM_SIZE; i += 2) {
        v[i] = z[i + 1];
        v[i + 1] = z[i];
    }
    for (int i = 0; i < ROM_SIZE; i += 4) {
        n[i] = z[i + 3];
        n[i + 1] = z[i + 2];
        n[i + 2] = z[i + 1];
        n[i + 3] = z[i];
    }
    CHECK_EQ(n64_rom_detect(z, ROM_SIZE), N64_ROM_Z64, "z64 detected");
    CHECK_EQ(n64_rom_detect(v, ROM_SIZE), N64_ROM_V64, "v64 detected");
    CHECK_EQ(n64_rom_detect(n, ROM_SIZE), N64_ROM_N64, "n64 detected");
    CHECK(n64_rom_normalize(v, ROM_SIZE) == 0 && memcmp(v, z, ROM_SIZE) == 0, "v64 normalised");
    CHECK(n64_rom_normalize(n, ROM_SIZE) == 0 && memcmp(n, z, ROM_SIZE) == 0, "n64 normalised");
    uint8_t junk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK_EQ(n64_rom_detect(junk, 8), N64_ROM_UNKNOWN, "junk not a ROM");

    n64_rom_info info;
    CHECK(n64_rom_parse(z, ROM_SIZE, &info) == 0, "parse");
    CHECK(strcmp(info.title, "XBA TEST ROM") == 0, "title \"%s\"", info.title);
    CHECK(strcmp(info.game_code, "NXTP") == 0, "game code %s", info.game_code);
    CHECK_EQ(info.crc1, 0x12345678, "crc1");
    CHECK_EQ(info.crc2, 0x9ABCDEF0, "crc2");
    CHECK_EQ(info.boot_address, 0x80000400, "boot address");
    CHECK_EQ(info.region, 'P', "region");
    CHECK_EQ(info.pal, 1, "PAL");
    CHECK_EQ(info.version, 2, "version");
    CHECK_EQ(info.cic, N64_CIC_LIBDRAGON, "libdragon IPL3 recognised");
    CHECK_EQ(info.save_hint, N64_SAVE_NONE, "no save hint");
    z[0x3C] = 'E';
    z[0x3D] = 'D';
    z[0x3F] = 0x51; /* FlashRAM + RTC */
    n64_rom_parse(z, ROM_SIZE, &info);
    CHECK_EQ(info.save_hint, N64_SAVE_FLASH, "ED64 save byte: FlashRAM");
    CHECK_EQ(info.save_hint_rtc, 1, "ED64 save byte: RTC");
    CHECK_EQ(n64_crc32((const uint8_t *)"123456789", 9), 0xCBF43926u, "CRC-32 check value");

    /* A machine from a v64 file: the ROM is normalised, the order reported. */
    make_rom(z);
    for (int i = 0; i < ROM_SIZE; i += 2) {
        v[i] = z[i + 1];
        v[i + 1] = z[i];
    }
    char err[64];
    n64_machine *m = n64_create(v, ROM_SIZE, NULL, err, sizeof err);
    CHECK(m != NULL, "create from v64");
    if (m) {
        CHECK_EQ(n64_info(m)->order, N64_ROM_V64, "order reported");
        CHECK_EQ(n64_peek32(m, 0xB0002000u), 0xA0A1A2A3u, "ROM readable big-endian");
        n64_destroy(m);
    }
    CHECK(n64_create(junk, 8, NULL, err, sizeof err) == NULL, "tiny file refused");
}

/* ---- Boot ------------------------------------------------------------------------------------------------------ */

static void test_boot(void)
{
    n64_machine *m = mk(NULL);
    n64_reset(m);
    CHECK_EQ(n64_pc(m), 0xFFFFFFFFA4000040ull, "boot PC in DMEM");
    CHECK_EQ(n64_peek32(m, 0xA4000000u), 0x80371240u, "header copied to DMEM");
    CHECK_EQ(n64_peek32(m, 0xA4000340u), 0x204C6962u, "IPL3 copied to DMEM");
    CHECK_EQ(n64_reg(m, 20), 0, "s4: PAL from the region");
    CHECK_EQ(n64_reg(m, 22), 0x3F, "s6: the 6102 seed");
    CHECK_EQ(n64_reg(m, 29), 0xFFFFFFFFA4001FF0ull, "sp");
    CHECK(r32(m, 0x0470000C) != 0, "RI_SELECT reports the RDRAM configured");
    CHECK_EQ(n64_peek32(m, 0x80000318u), N64_RDRAM_SIZE, "osMemSize");
    CHECK_EQ(m->pi_dom[0][0], 0x40, "PI domain 1 latency from the header");
    CHECK_EQ(m->pi_dom[0][1], 0x12, "PI domain 1 pulse width");
    CHECK_EQ(m->pi_dom[0][2], 0x07, "PI domain 1 page size");
    CHECK_EQ(m->pi_dom[0][3], 0x03, "PI domain 1 release");
    CHECK_EQ(r32(m, 0x04300004), 0x02020102u, "MI_VERSION");
    /* IPL2's first words left in IMEM (n64_pif.c; the CIC-6105 IPL3 decrypts its RSP code with them), PAL variant. */
    CHECK_EQ(n64_peek32(m, 0xA4001000u), 0x3C0DBFC0u, "IMEM 0x000: IPL2");
    CHECK_EQ(n64_peek32(m, 0xA4001004u), 0xBDA807FCu, "IMEM 0x004: IPL2 (PAL)");
    CHECK_EQ(n64_peek32(m, 0xA400101Cu), 0x3C0BB000u, "IMEM 0x01C: IPL2");
    CHECK_EQ(m->boot_x105, N64_X105_OFF, "no 6105 boot handshake for another CIC");
    for (int r = 1; r < 32; r++)
        CHECK_EQ(n64_rsp_get_gpr(m->rsp, r), 0, "RSP register after the boot (not a 6105)");
    n64_destroy(m);
    n64_options o = {.force_pal = 0};
    m = mk(&o);
    CHECK_EQ(n64_reg(m, 20), 1, "s4: NTSC forced");
    CHECK_EQ(n64_peek32(m, 0xA4001004u), 0x8DA807FCu, "IMEM 0x004: IPL2 (NTSC)");
    n64_destroy(m);
}

/* The CIC-6105 IPL3's RSP semaphore wait (n64_pif.c): a program shaped like the IPL3's (take SP_SEMAPHORE, spin on it
 * with $5 counting down, give up when $5 goes negative), started the way IPL3 starts it. With the handshake armed the
 * RSP is held while the CPU keeps the semaphore, so it does not give up however long the CPU takes, and once the
 * program has ended $5 is the console's value; without it, the same program gives up. */
static void test_x105_boot(void)
{
    static const uint32_t prog[] = {
        0x3C050001u, /* 000 lui   $5, 0x0001      (a short count: 65536 iterations) */
        0x40083800u, /* 004 mfc0  $8, SP_SEMAPHORE  (takes it) */
        0x04A00005u, /* 008 bltz  $5, 0x020 */
        0x40033800u, /* 00C mfc0  $3, SP_SEMAPHORE */
        0x1460FFFDu, /* 010 bne   $3, $0, 0x008 */
        0x20A5FFFFu, /* 014 addi  $5, $5, -1 */
        0x34061234u, /* 018 ori   $6, $0, 0x1234  (released) */
        0x0000000Du, /* 01C break */
        0x3406DEADu, /* 020 ori   $6, $0, 0xDEAD  (gave up) */
        0x0000000Du, /* 024 break */
    };
    for (int armed = 1; armed >= 0; armed--) {
        n64_machine *m = mk(NULL);
        for (unsigned i = 0; i < sizeof prog / sizeof prog[0]; i++)
            n64_poke32(m, 0xA4001000u + 4 * i, prog[i]);
        m->boot_x105 = armed ? N64_X105_ARMED : N64_X105_OFF; /* what n64_pif_boot sets for a 6105 cartridge */
        w32(m, 0x04080000, 0);       /* SP_PC */
        w32(m, 0x04040010, 0x1 | 0x4); /* SP_STATUS: clear halt and broke */
        n64_run_cycles(m, 3000000);  /* far longer than 65536 iterations */
        if (armed) {
            CHECK(!n64_rsp_halted(m->rsp), "6105 boot: the RSP waits for the semaphore");
            CHECK(n64_rsp_get_gpr(m->rsp, 5) > 0x8000u, "6105 boot: the RSP is held, not spinning ($5 = 0x%X)",
                  n64_rsp_get_gpr(m->rsp, 5));
        } else {
            CHECK(n64_rsp_halted(m->rsp), "no handshake: the RSP gives up");
            CHECK_EQ(n64_rsp_get_gpr(m->rsp, 6), 0xDEAD, "no handshake: the give-up path");
        }
        w32(m, 0x0404001C, 0); /* the CPU releases SP_SEMAPHORE */
        n64_run_cycles(m, 100000);
        CHECK(n64_rsp_halted(m->rsp), "the program ended");
        if (armed) {
            CHECK_EQ(n64_rsp_get_gpr(m->rsp, 6), 0x1234, "6105 boot: the released path");
            CHECK_EQ(n64_rsp_get_gpr(m->rsp, 5), 0x4E000, "6105 boot: $5 the console leaves");
            CHECK_EQ(m->boot_x105, N64_X105_OFF, "6105 boot: the handshake is over");
            /* Over: a later wait on the semaphore runs as it is (no hold). */
            n64_rsp_set_gpr(m->rsp, 5, 7);
            CHECK_EQ(n64_rsp_get_gpr(m->rsp, 5), 7, "RSP register setter");
        }
        n64_destroy(m);
    }
}

/* Guest health counters (n64_guest_activity, n64_guest_fault_info): what the runtime's stuck detector reads. */
static void test_guest_health(void)
{
    n64_machine *m = mk(NULL);
    n64_activity a0, a1;
    n64_guest_activity(m, &a0);
    w32(m, 0x04400004, 0x100000); /* VI_ORIGIN, new value */
    w32(m, 0x04400004, 0x100000); /* the same again: not a change */
    w32(m, 0x04500000, 0x2000);   /* AI_DRAM_ADDR */
    w32(m, 0x04500004, 0x100);    /* AI_LEN: a buffer */
    n64_guest_activity(m, &a1);
    CHECK_EQ(a1.vi_origin_changes - a0.vi_origin_changes, 1, "VI origin changes");
    CHECK_EQ(a1.ai_buffers - a0.ai_buffers, 1, "audio buffers");
    CHECK_EQ(n64_guest_fault_info(m)->count, 0, "no fault yet");
    n64_poke32(m, 0x80000800u, 0x8C080001u); /* lw t0, 1(zero): address error */
    n64_run_cycles(m, 100);
    const n64_guest_fault *f = n64_guest_fault_info(m);
    CHECK(f->count >= 1, "a fault recorded");
    CHECK_EQ(f->code, 4, "AdEL");
    CHECK_EQ(f->epc, 0xFFFFFFFF80000800ull, "its EPC");
    CHECK_EQ(f->badvaddr, 1, "its BadVAddr");
    n64_destroy(m);
}

/* ---- Joybus ---------------------------------------------------------------------------------------------------- */

/* Sends a PIF RAM command block through the SI and reads the answer back. */
static void pif_run(n64_machine *m, uint8_t block[64])
{
    memcpy(m->rdram + 0x1000, block, 64);
    w32(m, 0x04800000, 0x1000);
    w32(m, 0x04800010, 0x1FC007C0); /* PIF_AD_WR64B */
    n64_run_cycles(m, 5000);
    CHECK(r32(m, 0x04300008) & (1u << N64_MI_SI), "SI interrupt after the write");
    w32(m, 0x04800018, 0); /* clear it */
    w32(m, 0x04800000, 0x2000);
    w32(m, 0x04800004, 0x1FC007C0); /* PIF_AD_RD64B */
    n64_run_cycles(m, 5000);
    memcpy(block, m->rdram + 0x2000, 64);
}

static void test_joybus(void)
{
    n64_machine *m = mk(NULL);
    uint8_t b[64];

    /* Info and buttons on port 0; port 1 empty. */
    n64_set_controller(m, 0, 1, N64_BTN_A | N64_BTN_START | N64_BTN_CLEFT, 42, -17);
    memset(b, 0, 64);
    uint8_t q[] = {0x01, 0x03, 0x00, 0xFF, 0xFF, 0xFF, 0x01, 0x03, 0x00, 0xFF, 0xFF, 0xFF, 0xFE};
    memcpy(b, q, sizeof q);
    b[63] = 0x01;
    pif_run(m, b);
    CHECK(b[3] == 0x05 && b[4] == 0x00 && b[5] == 0x01, "port 0 info: controller, pak in (%02X %02X %02X)", b[3], b[4],
          b[5]);
    CHECK(b[7] & 0x80, "port 1 empty: no response flag");
    memset(b, 0, 64);
    uint8_t q2[] = {0x01, 0x04, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE};
    memcpy(b, q2, sizeof q2);
    b[63] = 0x01;
    pif_run(m, b);
    CHECK(b[3] == 0x90 && b[4] == 0x02, "buttons A START C-left (%02X %02X)", b[3], b[4]);
    CHECK(b[5] == 42 && (int8_t)b[6] == -17, "stick");
    CHECK_EQ(b[63] & 1, 0, "command bit cleared");

    /* Controller pak: write 32 bytes at 0x0100, read them back, both CRCs. */
    uint8_t data[32];
    for (int i = 0; i < 32; i++)
        data[i] = (uint8_t)(i * 7 + 1);
    memset(b, 0, 64);
    b[0] = 35;
    b[1] = 1;
    b[2] = 0x03;
    b[3] = 0x01;
    b[4] = 0x00;
    memcpy(b + 5, data, 32);
    b[38] = 0xFE;
    b[63] = 1;
    pif_run(m, b);
    uint8_t wcrc = b[37];
    memset(b, 0, 64);
    b[0] = 3;
    b[1] = 33;
    b[2] = 0x02;
    b[3] = 0x01;
    b[4] = 0x00;
    b[38] = 0xFE;
    b[63] = 1;
    pif_run(m, b);
    CHECK(memcmp(b + 5, data, 32) == 0, "pak read back");
    CHECK_EQ(b[37], wcrc, "read CRC = write CRC");
    size_t sz;
    const uint8_t *pak = n64_save_data(m, N64_SAVE_PAK0, &sz);
    CHECK(pak && sz == N64_PAK_SIZE && memcmp(pak + 0x100, data, 32) == 0, "pak image");
    CHECK_EQ(n64_save_take_dirty(m, N64_SAVE_PAK0), 1, "pak dirty");
    CHECK_EQ(n64_save_take_dirty(m, N64_SAVE_PAK0), 0, "pak dirty cleared");
    /* The pak data CRC (CRC-8, polynomial 0x85, over the 32 bytes and a zero byte): 0x0A for 32 x 0xFF. */
    memset(b, 0, 64);
    b[0] = 35;
    b[1] = 1;
    b[2] = 0x03;
    b[3] = 0x02;
    b[4] = 0x00;
    memset(b + 5, 0xFF, 32);
    b[38] = 0xFE;
    b[63] = 1;
    pif_run(m, b);
    CHECK_EQ(b[37], 0x0A, "CRC of 32 x 0xFF");

    /* Rumble pak: probe at 0x8000, motor at 0xC000. */
    n64_set_accessory(m, 0, N64_ACC_RUMBLE_PAK);
    memset(b, 0, 64);
    b[0] = 35;
    b[1] = 1;
    b[2] = 0x03;
    b[3] = 0x80;
    b[4] = 0x00;
    memset(b + 5, 0x80, 32);
    b[38] = 0xFE;
    b[63] = 1;
    pif_run(m, b);
    memset(b, 0, 64);
    b[0] = 3;
    b[1] = 33;
    b[2] = 0x02;
    b[3] = 0x80;
    b[4] = 0x00;
    b[38] = 0xFE;
    b[63] = 1;
    pif_run(m, b);
    CHECK(b[5] == 0x80 && b[36] == 0x80, "rumble pak answers 0x80 once probed");
    memset(b, 0, 64);
    b[0] = 35;
    b[1] = 1;
    b[2] = 0x03;
    b[3] = 0xC0;
    b[4] = 0x00;
    memset(b + 5, 0x01, 32);
    b[38] = 0xFE;
    b[63] = 1;
    pif_run(m, b);
    CHECK_EQ(n64_rumble(m, 0), 1, "motor on");

    /* EEPROM on channel 4: info, write block 3, read it back; detected as 4K. */
    memset(b, 0, 64);
    uint8_t e1[] = {0, 0, 0, 0, 0x01, 0x03, 0x00, 0xFF, 0xFF, 0xFF, 0xFE};
    memcpy(b, e1, sizeof e1);
    b[63] = 1;
    pif_run(m, b);
    CHECK(b[7] == 0x00 && b[8] == 0xC0, "EEPROM info, kind not fixed: a 16K part (%02X %02X)", b[7], b[8]);
    memset(b, 0, 64);
    uint8_t e2[] = {0, 0, 0, 0, 0x0A, 0x01, 0x05, 0x03, 1, 2, 3, 4, 5, 6, 7, 8, 0xFF, 0xFE};
    memcpy(b, e2, sizeof e2);
    b[63] = 1;
    pif_run(m, b);
    memset(b, 0, 64);
    uint8_t e3[] = {0, 0, 0, 0, 0x02, 0x08, 0x04, 0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0xFE};
    memcpy(b, e3, sizeof e3);
    b[63] = 1;
    pif_run(m, b);
    CHECK(b[8] == 1 && b[15] == 8, "EEPROM block read back");
    CHECK_EQ(n64_save_kind_detected(m), N64_SAVE_EEPROM4K, "EEPROM detected at its first command");
    const uint8_t *ee = n64_save_data(m, N64_SAVE_EEPROM4K, &sz);
    CHECK(ee && sz == 512 && ee[24] == 1, "EEPROM image");
    CHECK_EQ(n64_save_take_dirty(m, N64_SAVE_EEPROM4K), 1, "EEPROM dirty");
    /* Still answered as 16K (a 16K game's later blocks pass libultra's range check); block 70 makes it 16K, the
     * 4K image's bytes kept. */
    memset(b, 0, 64);
    uint8_t e4[] = {0, 0, 0, 0, 0x0A, 0x01, 0x05, 70, 9, 9, 9, 9, 9, 9, 9, 9, 0xFF, 0xFE};
    memcpy(b, e4, sizeof e4);
    b[63] = 1;
    pif_run(m, b);
    CHECK_EQ(n64_save_kind_detected(m), N64_SAVE_EEPROM16K, "EEPROM 16K once block 70 is written");
    ee = n64_save_data(m, N64_SAVE_EEPROM16K, &sz);
    CHECK(ee && sz == 2048 && ee[24] == 1 && ee[560] == 9, "EEPROM 16K image");

    /* RTC on channel 4: status, the time block, a time set by the game read back (the clock fixed for the test:
     * 2001-02-03 04:05:06, a Saturday). */
    m->rtc_fixed = 981173106;
    memset(b, 0, 64);
    uint8_t r1[] = {0, 0, 0, 0, 0x01, 0x03, 0x06, 0xFF, 0xFF, 0xFF, 0xFE};
    memcpy(b, r1, sizeof r1);
    b[63] = 1;
    pif_run(m, b);
    CHECK(!(b[5] & 0x80) && b[7] == 0x00 && b[8] == 0x10, "RTC status (%02X %02X)", b[7], b[8]);
    memset(b, 0, 64);
    uint8_t r2[] = {0, 0, 0, 0, 0x02, 0x09, 0x07, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFE};
    memcpy(b, r2, sizeof r2);
    b[63] = 1;
    pif_run(m, b);
    CHECK(b[8] == 0x06 && b[9] == 0x05 && b[10] == 0x84 && b[11] == 0x03 && b[12] == 0x06 && b[13] == 0x02 &&
              b[14] == 0x01 && b[15] == 0x01,
          "RTC time %02X:%02X:%02X %02X/%02X/%02X wd %02X c %02X", b[10], b[9], b[8], b[11], b[13], b[14], b[12],
          b[15]);
    memset(b, 0, 64);
    uint8_t r3[] = {0, 0, 0, 0, 0x0A, 0x01, 0x08, 0x02, 0x30, 0x59, 0x80 | 0x23, 0x31, 0x00, 0x12, 0x99, 0x00, 0xFE};
    memcpy(b, r3, sizeof r3); /* 1999-12-31 23:59:30 */
    b[63] = 1;
    pif_run(m, b);
    m->rtc_fixed += 45; /* 45 s later: 2000-01-01 00:00:15 */
    memset(b, 0, 64);
    memcpy(b, r2, sizeof r2);
    b[63] = 1;
    pif_run(m, b);
    CHECK(b[8] == 0x15 && b[9] == 0x00 && (b[10] & 0x3F) == 0x00 && b[11] == 0x01 && b[13] == 0x01 && b[14] == 0x00 &&
              b[15] == 0x01,
          "RTC set by the game, 45 s on: %02X:%02X:%02X %02X/%02X/%02X c %02X", b[10], b[9], b[8], b[11], b[13], b[14],
          b[15]);
    n64_destroy(m);

    /* A 4K EEPROM fixed by the options answers as 4K. */
    n64_options o4 = {.force_pal = -1, .save_kind = N64_SAVE_EEPROM4K};
    m = mk(&o4);
    memset(b, 0, 64);
    memcpy(b, e1, sizeof e1);
    b[63] = 1;
    pif_run(m, b);
    CHECK(b[7] == 0x00 && b[8] == 0x80, "fixed EEPROM 4K info (%02X %02X)", b[7], b[8]);

    /* A fresh controller pak is formatted: the ID block and its copies with a valid checksum, the index table's free
     * entries and checksum, an empty note table. */
    const uint8_t *pk = n64_save_data(m, N64_SAVE_PAK0, &sz);
    CHECK(pk && sz == 32768, "pak image");
    if (pk) {
        static const int at[4] = {0x20, 0x60, 0x80, 0xC0};
        for (int i = 0; i < 4; i++) {
            uint32_t sum = 0;
            for (int k = 0; k < 28; k += 2)
                sum += (uint32_t)(pk[at[i] + k] << 8 | pk[at[i] + k + 1]);
            uint32_t got = (uint32_t)(pk[at[i] + 28] << 8 | pk[at[i] + 29]);
            uint32_t inv = (uint32_t)(pk[at[i] + 30] << 8 | pk[at[i] + 31]);
            CHECK(got == (sum & 0xFFFF) && inv == ((0xFFF2u - got) & 0xFFFF), "pak ID block %d checksum", i);
        }
        CHECK(pk[0x20 + 28] == 0x66 && pk[0x20 + 29] == 0x25 && pk[0x20 + 30] == 0x99 && pk[0x20 + 31] == 0xCD,
              "pak ID checksum bytes as a fresh .mpk");
        uint32_t c = 0;
        for (int e = 5; e < 128; e++)
            c += pk[0x100 + 2 * e + 1];
        CHECK(pk[0x101] == (c & 0xFF) && pk[0x101] == 0x71 && pk[0x10B] == 0x03 && pk[0x1FF] == 0x03 &&
                  !memcmp(pk + 0x100, pk + 0x200, 256),
              "pak index table and its copy");
        int empty = 1;
        for (int i = 0x300; i < 0x500; i++)
            empty &= pk[i] == 0;
        CHECK(empty, "pak note table empty");
        CHECK_EQ(n64_save_take_dirty(m, N64_SAVE_PAK0), 0, "a fresh pak is not dirty");
    }
    n64_destroy(m);
}

/* ---- PI, SRAM, FlashRAM ---------------------------------------------------------------------------------------- */

static void pi_dma(n64_machine *m, uint32_t dram, uint32_t cart, uint32_t len, bool to_rdram)
{
    w32(m, 0x04600000, dram);
    w32(m, 0x04600004, cart);
    w32(m, to_rdram ? 0x0460000C : 0x04600008, len - 1);
    CHECK(r32(m, 0x04600010) & 1, "PI busy during the DMA");
    n64_run_cycles(m, 200000);
    CHECK(!(r32(m, 0x04600010) & 1), "PI idle after the DMA");
    CHECK(r32(m, 0x04300008) & (1u << N64_MI_PI), "PI interrupt");
    w32(m, 0x04600010, 2); /* clear */
}

static void test_pi_saves(void)
{
    n64_machine *m = mk(NULL);
    /* ROM -> RDRAM, registers advanced. */
    pi_dma(m, 0x3000, 0x10002000, 16, true);
    CHECK_EQ(n64_peek32(m, 0x80003000u), 0xA0A1A2A3u, "ROM DMA data");
    CHECK_EQ(r32(m, 0x04600000), 0x3010, "PI_DRAM_ADDR advanced");
    CHECK_EQ(r32(m, 0x04600004), 0x10002010, "PI_CART_ADDR advanced");

    /* SRAM, detected at the first access: RDRAM -> SRAM -> RDRAM. */
    CHECK_EQ(n64_save_kind_detected(m), N64_SAVE_NONE, "save kind unknown at first");
    for (int i = 0; i < 256; i++)
        m->rdram[0x4000 + i] = (uint8_t)(255 - i);
    pi_dma(m, 0x4000, 0x08000100, 256, false);
    CHECK_EQ(n64_save_kind_detected(m), N64_SAVE_SRAM, "SRAM detected");
    memset(m->rdram + 0x5000, 0, 256);
    pi_dma(m, 0x5000, 0x08000100, 256, true);
    CHECK(memcmp(m->rdram + 0x5000, m->rdram + 0x4000, 256) == 0, "SRAM read back");
    size_t sz;
    const uint8_t *sram = n64_save_data(m, N64_SAVE_SRAM, &sz);
    CHECK(sram && sz == N64_SRAM_SIZE && sram[0x100] == 255, "SRAM image");
    CHECK_EQ(n64_save_take_dirty(m, N64_SAVE_SRAM), 1, "SRAM dirty");
    CHECK(n64_save_data(m, N64_SAVE_FLASH, &sz) == NULL, "no flash image");
    n64_destroy(m);

    /* FlashRAM, detected from its command register; ID, erase, program, read. */
    m = mk(NULL);
    w32(m, 0x08010000, 0xE1000000); /* status mode */
    n64_run_cycles(m, 1000);
    CHECK_EQ(n64_save_kind_detected(m), N64_SAVE_FLASH, "FlashRAM detected");
    pi_dma(m, 0x3000, 0x08000000, 8, true);
    CHECK_EQ(n64_peek32(m, 0x80003000u), 0x11118001u, "flash ID high");
    CHECK_EQ(n64_peek32(m, 0x80003004u), 0x00C2001Eu, "flash ID low (MX29L1100)");
    for (int i = 0; i < 128; i++)
        m->rdram[0x4000 + i] = (uint8_t)i;
    w32(m, 0x08010000, 0xB4000000); /* write buffer */
    n64_run_cycles(m, 1000);
    pi_dma(m, 0x4000, 0x08000000, 128, false);
    w32(m, 0x08010000, 0xA5000000 | 130); /* page 130 */
    n64_run_cycles(m, 1000);
    w32(m, 0x08010000, 0xD2000000); /* execute */
    n64_run_cycles(m, 1000);
    w32(m, 0x08010000, 0xF0000000); /* read array */
    n64_run_cycles(m, 1000);
    memset(m->rdram + 0x5000, 0, 128);
    pi_dma(m, 0x5000, 0x08000000 + 130 * 128 / 2, 128, true); /* 16-bit bus: half the offset */
    CHECK(memcmp(m->rdram + 0x5000, m->rdram + 0x4000, 128) == 0, "flash page read back");
    const uint8_t *fl = n64_save_data(m, N64_SAVE_FLASH, &sz);
    CHECK(fl && sz == N64_FLASH_SIZE && fl[130 * 128 + 5] == 5 && fl[0] == 0xFF, "flash image");
    w32(m, 0x08010000, 0x4B000000 | 128); /* erase the sector of page 128 */
    n64_run_cycles(m, 1000);
    w32(m, 0x08010000, 0x78000000);
    n64_run_cycles(m, 1000);
    w32(m, 0x08010000, 0xD2000000);
    n64_run_cycles(m, 1000);
    CHECK_EQ(fl[130 * 128 + 5], 0xFF, "sector erased");

    /* Save load: the right size only, and the kind must agree. */
    static uint8_t img[N64_FLASH_SIZE];
    memset(img, 0x5A, sizeof img);
    CHECK(n64_save_load(m, N64_SAVE_FLASH, img, sizeof img) == 0, "flash load");
    CHECK_EQ(n64_save_data(m, N64_SAVE_FLASH, &sz)[77], 0x5A, "flash loaded");
    CHECK(n64_save_load(m, N64_SAVE_SRAM, img, N64_SRAM_SIZE) != 0, "a different kind is refused");
    CHECK(n64_save_load(m, N64_SAVE_FLASH, img, 100) != 0, "a wrong size is refused");
    n64_destroy(m);

    /* The header hint decides before any access. */
    n64_options o = {.force_pal = -1, .save_kind = N64_SAVE_EEPROM16K};
    m = mk(&o);
    CHECK_EQ(n64_save_kind_detected(m), N64_SAVE_EEPROM16K, "forced save kind");
    n64_destroy(m);
}

/* ---- AI -------------------------------------------------------------------------------------------------------- */

static void test_ai(void)
{
    n64_machine *m = mk(NULL);
    int16_t buf[4096 * 2];
    n64_audio_pull(m, buf, 4096); /* drop what boot made */
    uint32_t vi_clock = 48681812u;
    for (uint32_t i = 0; i < 0x1000; i += 4) {
        n64_wbe16(m->rdram + 0x10000 + i, 0x1000);
        n64_wbe16(m->rdram + 0x10000 + i + 2, (uint16_t)-0x1000);
    }
    m->pal = false;
    w32(m, 0x04500010, vi_clock / 32000 - 1); /* DACRATE: 32 kHz */
    w32(m, 0x04500014, 15);
    w32(m, 0x04500008, 1); /* DMA on */
    w32(m, 0x04500000, 0x10000);
    w32(m, 0x04500004, 0x1000); /* 1024 frames: 32 ms */
    CHECK(r32(m, 0x04300008) & (1u << N64_MI_AI), "AI interrupt when the buffer starts");
    CHECK(r32(m, 0x0450000C) & 0x40000000u, "AI busy");
    CHECK(!(r32(m, 0x0450000C) & 0x80000000u), "AI not full with one buffer");
    w32(m, 0x0450000C, 0); /* clear the interrupt */
    w32(m, 0x04500004, 0x1000); /* queue a second */
    CHECK(r32(m, 0x0450000C) & 0x80000000u, "AI full with two buffers");
    CHECK(!(r32(m, 0x04300008) & (1u << N64_MI_AI)), "no interrupt for a queued buffer");
    CHECK_EQ(n64_audio_source_rate(m), vi_clock / (vi_clock / 32000), "source rate");
    n64_run_cycles(m, N64_CPU_HZ / 50); /* 20 ms */
    size_t n = n64_audio_pull(m, buf, 4096);
    CHECK(n >= 950 && n <= 970, "20 ms of output at 48 kHz: %zu frames", n);
    CHECK(buf[2 * 100] == 0x1000 && buf[2 * 100 + 1] == -0x1000, "samples (%d %d)", buf[200], buf[201]);
    n64_run_cycles(m, N64_CPU_HZ / 50); /* 40 ms: the first buffer ended, the second started */
    CHECK(r32(m, 0x04300008) & (1u << N64_MI_AI), "AI interrupt when the second buffer starts");
    CHECK(!(r32(m, 0x0450000C) & 0x80000000u), "not full any more");
    uint32_t left = r32(m, 0x04500004);
    CHECK(left > 0 && left < 0x1000, "AI_LEN counts down (%u)", left);
    n64_run_cycles(m, N64_CPU_HZ / 20);
    CHECK(!(r32(m, 0x0450000C) & 0x40000000u), "idle after both");
    n = n64_audio_pull(m, buf, 4096);
    CHECK(n > 3000, "output keeps flowing while idle (%zu)", n);
    n64_destroy(m);
}

/* A game's audio loop: buffers of a sine (continuous phase across buffers) queued whenever the AI has room, at the
 * DAC rate dac_hz (the exact rate the divider gives is used for the phase). Output collected at 48 kHz. */
typedef struct tone {
    double phase, freq, amp;
    uint32_t next_buf;
} tone;

static size_t play_tone(n64_machine *m, tone *t, uint32_t dacrate, double ms, int16_t *out, size_t max)
{
    const double vi_clock = 48681812.0;
    double fs = vi_clock / (dacrate + 1);
    w32(m, 0x04500010, dacrate);
    w32(m, 0x04500008, 1);
    size_t n = 0;
    int steps = (int)(ms * 4);
    for (int s = 0; s < steps; s++) {
        while (!(r32(m, 0x0450000C) & 0x80000000u)) { /* room for a buffer: 512 frames */
            uint32_t addr = 0x200000 + (t->next_buf++ & 3) * 0x1000;
            for (int i = 0; i < 512; i++) {
                int16_t v = (int16_t)lrint(t->amp * sin(t->phase));
                t->phase += 2 * 3.14159265358979323846 * t->freq / fs;
                n64_wbe16(m->rdram + addr + 4 * i, (uint16_t)v);
                n64_wbe16(m->rdram + addr + 4 * i + 2, (uint16_t)v);
            }
            w32(m, 0x04500000, addr);
            w32(m, 0x04500004, 512 * 4);
            w32(m, 0x0450000C, 0);
        }
        n64_run_cycles(m, N64_CPU_HZ / 4000); /* 0.25 ms */
        int16_t tmp[2 * 64];
        size_t got;
        while ((got = n64_audio_pull(m, tmp, 64)) > 0)
            for (size_t i = 0; i < got && n < max; i++)
                out[n++] = tmp[2 * i];
    }
    return n;
}

/* Signal-to-(noise + distortion + images) of a stretch of 48 kHz output against a sine at freq: least-squares fit of
 * a sine, a cosine and DC, then the residual's power, in dB. */
static double sine_snr(const int16_t *x, size_t n, double freq)
{
    double w = 2 * 3.14159265358979323846 * freq / 48000.0;
    double A[3][4] = {{0}};
    for (size_t i = 0; i < n; i++) {
        double f[3] = {sin(w * (double)i), cos(w * (double)i), 1.0};
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++)
                A[r][c] += f[r] * f[c];
            A[r][3] += f[r] * x[i];
        }
    }
    for (int col = 0; col < 3; col++) /* Gauss-Jordan on the 3 x 3 normal equations */
        for (int r = 0; r < 3; r++) {
            if (r == col)
                continue;
            double q = A[r][col] / A[col][col];
            for (int c = 0; c < 4; c++)
                A[r][c] -= q * A[col][c];
        }
    double a = A[0][3] / A[0][0], b = A[1][3] / A[1][1], dc = A[2][3] / A[2][2];
    double sig = 0, err = 0;
    for (size_t i = 0; i < n; i++) {
        double fit = a * sin(w * (double)i) + b * cos(w * (double)i);
        double e = x[i] - dc - fit;
        sig += fit * fit;
        err += e * e;
    }
    return 10 * log10(sig / (err > 1e-9 ? err : 1e-9));
}

static int max_step(const int16_t *x, size_t n)
{
    int mx = 0;
    for (size_t i = 1; i < n; i++) {
        int d = abs(x[i] - x[i - 1]);
        if (d > mx)
            mx = d;
    }
    return mx;
}

static void test_ai_resampler(void)
{
    static int16_t out[48000];
    const uint32_t vi_clock = 48681812u;
    /* A sweep of tones from a 32 kHz DAC: sample and hold left the DAC's images (32 kHz - f, folded at 24 kHz) at a
     * few dB below the tone near the top; the windowed sinc keeps them more than 70 dB down through 12 kHz (60 at
     * 14 kHz). */
    static const double freqs[] = {100, 440, 1000, 3000, 6000, 9000, 12000, 14000};
    for (size_t k = 0; k < sizeof freqs / sizeof freqs[0]; k++) {
        n64_options ntsc = {.force_pal = 0};
        n64_machine *m = mk(&ntsc);
        tone t = {0, freqs[k], 12000, 0};
        size_t n = play_tone(m, &t, vi_clock / 32000 - 1, 120, out, 48000);
        double snr = n > 4000 ? sine_snr(out + 1000, n - 2000, freqs[k]) : 0;
        double want = freqs[k] <= 12000 ? 70 : 60;
        printf("  32 kHz DAC, %5.0f Hz: SNR %.1f dB\n", freqs[k], snr);
        CHECK(n > 5000 && snr > want, "32 kHz DAC, %.0f Hz: SNR %.1f dB (want > %.0f), %zu samples", freqs[k], snr,
              want, n);
        n64_destroy(m);
    }
    /* 22.05 kHz and 44.1 kHz DACs. */
    static const uint32_t rates[] = {22050, 44100};
    for (int r = 0; r < 2; r++) {
        n64_options ntsc = {.force_pal = 0};
        n64_machine *m = mk(&ntsc);
        tone t = {0, 5000, 12000, 0};
        size_t n = play_tone(m, &t, vi_clock / rates[r] - 1, 100, out, 48000);
        double snr = sine_snr(out + 1000, n - 2000, 5000);
        CHECK(snr > 70, "%u Hz DAC, 5 kHz: SNR %.1f dB", rates[r], snr);
        n64_destroy(m);
    }
    /* Buffer boundaries and a DAC rate change mid-stream (32 kHz -> 22.05 kHz, the sine's phase continuous): no
     * click: no step between two output samples beyond what a 440 Hz sine of that amplitude makes (2 pi f A / 48000:
     * about 690), with margin. Then a gap (no buffer queued: the DAC holds its last sample) and a restart. */
    {
        n64_options ntsc = {.force_pal = 0};
        n64_machine *m = mk(&ntsc);
        int16_t buf[4096 * 2];
        n64_run_cycles(m, N64_CPU_HZ / 100);
        n64_audio_pull(m, buf, 4096);
        tone t = {0, 440, 12000, 0};
        size_t n = play_tone(m, &t, vi_clock / 32000 - 1, 50, out, 48000);
        size_t n2 = play_tone(m, &t, vi_clock / 22050 - 1, 150, out + n, 48000 - n);
        int step = max_step(out + 200, n + n2 - 200);
        CHECK(step < 900, "rate change and buffer boundaries: largest step %d", step);
        /* The two buffers queued before the change play at the new rate (as on the hardware); then 440 Hz again. */
        double snr = sine_snr(out + n + n2 - 2880, 2880, 440);
        CHECK(snr > 70, "after the rate change: SNR %.1f dB", snr);
        n64_destroy(m);
    }
}

/* ---- VI -------------------------------------------------------------------------------------------------------- */

static uint32_t rgb_of(int r, int g, int b) { return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b; }
static int near(uint32_t a, uint32_t b, int tol)
{
    for (int s = 0; s < 24; s += 8) {
        int d = (int)((a >> s) & 255) - (int)((b >> s) & 255);
        if (d < -tol || d > tol)
            return 0;
    }
    return 1;
}
static uint16_t c16(int r5, int g5, int b5, int a) { return (uint16_t)(r5 << 11 | g5 << 6 | b5 << 1 | a); }

/* A VI mode: ctrl, origin, width, H / V_VIDEO, X / Y_SCALE (offset << 16 | scale). */
static void vi_mode(n64_machine *m, uint32_t ctrl, uint32_t origin, uint32_t width, uint32_t hv, uint32_t vv,
                    uint32_t xs, uint32_t ys)
{
    w32(m, 0x04400004, origin);
    w32(m, 0x04400008, width);
    w32(m, 0x04400024, hv);
    w32(m, 0x04400028, vv);
    w32(m, 0x04400030, xs);
    w32(m, 0x04400034, ys);
    w32(m, 0x04400000, ctrl);
}

static void test_vi(void)
{
    n64_options ntsc = {.force_pal = 0};
    n64_machine *m = mk(&ntsc);
    static uint32_t px[640 * 576];
    int w, h;
    CHECK(n64_display(m, px, 640, 576, &w, &h) != 0, "blank before the VI is set up");
    const uint8_t *rgba = NULL;
    int pitch = 0;
    CHECK(n64_display_rgba(m, &rgba, &w, &h, &pitch) == -2, "no upscaled scanout: n64_display instead");

    /* NTSC 320 x 240, 16-bit, AA off (replicate): the canvas is 320 x 240, the active area (half-lines 37..511) starts
     * on row 1, the framebuffer's own pixels. The VI's guard band blanks VI pixels 108..115 and 741..747: canvas
     * columns 0..3 and 317..319. */
    const uint32_t fb = 0x100000;
    vi_mode(m, 0x3302, fb, 320, 0x006C02EC, 0x002501FF, 0x200, 0x400);
    n64_wbe16(m->rdram + fb + 2 * 4, c16(31, 0, 0, 1));   /* red at (4, 0) */
    n64_wbe16(m->rdram + fb + 2 * 325, c16(0, 31, 0, 1)); /* green at (5, 1) */
    n64_wbe16(m->rdram + fb + 2 * 3, c16(31, 31, 31, 1)); /* white in the guard band */
    n64_wbe16(m->rdram + fb + 2 * 317, c16(31, 31, 31, 1));
    n64_wbe16(m->rdram + fb + 2 * 316, c16(0, 0, 31, 1)); /* blue: the last visible column */
    CHECK(n64_display(m, px, 640, 576, &w, &h) == 0, "16-bit picture");
    CHECK(w == 320 && h == 240, "NTSC canvas %dx%d", w, h);
    CHECK_EQ(px[0], 0, "row 0: border");
    CHECK_EQ(px[320 * 1 + 4], 0xFF0000, "red pixel");
    CHECK_EQ(px[320 * 2 + 5], 0x00FF00, "green pixel");
    CHECK_EQ(px[320 * 1 + 3], 0, "guard band: column 3 blank");
    CHECK_EQ(px[320 * 1 + 316], 0x0000FF, "column 316 visible");
    CHECK_EQ(px[320 * 1 + 317], 0, "guard band: column 317 blank");
    CHECK_EQ(px[320 * 239], 0, "last row: border (237 active lines)");
    w32(m, 0x04400000, 0x3303); /* 32-bit */
    n64_wbe32(m->rdram + fb + 4 * 4, 0x123456FF);
    n64_display(m, px, 640, 576, &w, &h);
    CHECK_EQ(px[320 + 4], 0x123456, "32-bit pixel");
    CHECK(n64_display(m, px, 100, 50, &w, &h) == 0 && w == 100 && h == 50, "clipped to the buffer");

    /* A narrow active area (H 188..668: 480 VI pixels) keeps its place: 40 output pixels of border either side, plus
     * the guard band (VI 196..660 visible: columns 44..276). */
    memset(m->rdram + fb, 0xFF, 320 * 240 * 2);
    vi_mode(m, 0x3302, fb, 240, 0x00BC029C, 0x002501FF, 0x200, 0x400);
    n64_display(m, px, 640, 576, &w, &h);
    CHECK(w == 320 && px[320 * 10 + 43] == 0 && px[320 * 10 + 44] == 0xFFFFFF && px[320 * 10 + 276] == 0xFFFFFF &&
              px[320 * 10 + 277] == 0,
          "borders around a narrow picture (%06X %06X %06X %06X)", px[320 * 10 + 43], px[320 * 10 + 44],
          px[320 * 10 + 276], px[320 * 10 + 277]);

    /* No guard band on an edge clamped to the scan-out: H 100..760 starts before the canvas (VI 108) and ends past it
     * (748), so every canvas column is visible. */
    vi_mode(m, 0x3302, fb, 330, 0x006402F8, 0x002501FF, 0x200, 0x400);
    n64_display(m, px, 640, 576, &w, &h);
    CHECK(w == 320 && px[320 * 10 + 0] == 0xFFFFFF && px[320 * 10 + 319] == 0xFFFFFF,
          "clamped edges: no guard band (%06X %06X)", px[320 * 10 + 0], px[320 * 10 + 319]);
    /* Only the left edge clamped (H 100..740): the right guard band stays (VI 733: columns 0..312). */
    vi_mode(m, 0x3302, fb, 320, 0x006402E4, 0x002501FF, 0x200, 0x400);
    n64_display(m, px, 640, 576, &w, &h);
    CHECK(px[320 * 10 + 0] == 0xFFFFFF && px[320 * 10 + 312] == 0xFFFFFF && px[320 * 10 + 313] == 0,
          "left edge clamped, right guard band (%06X %06X %06X)", px[320 * 10 + 0], px[320 * 10 + 312],
          px[320 * 10 + 313]);

    /* 640 wide (X_SCALE 1.0): a 640-pixel canvas. A 256-wide framebuffer stretched to 640 VI pixels (X_SCALE 0.4):
     * 320 wide, interpolated between framebuffer pixels (resample, AA mode 2). */
    vi_mode(m, 0x3203, fb, 640, 0x006C02EC, 0x002501FF, 0x400, 0x400);
    n64_display(m, px, 640, 576, &w, &h);
    CHECK(w == 640 && h == 240, "640 canvas %dx%d", w, h);
    for (int x = 0; x < 256; x++)
        for (int y = 0; y < 4; y++)
            n64_wbe16(m->rdram + fb + 2 * (y * 256 + x), c16(x & 1 ? 31 : 0, 0, 0, 1));
    vi_mode(m, 0x3202, fb, 256, 0x006C02EC, 0x002501FF, 0x19A, 0x400);
    n64_display(m, px, 640, 576, &w, &h);
    int mid = 0;
    for (int x = 0; x < 320; x++) {
        int r = (int)(px[320 + x] >> 16);
        mid += r > 40 && r < 215;
    }
    CHECK(w == 320 && mid > 100, "stretched 256 -> 320: interpolated (%d mid values)", mid);
    vi_mode(m, 0x3302, fb, 256, 0x006C02EC, 0x002501FF, 0x19A, 0x400);
    n64_display(m, px, 640, 576, &w, &h);
    mid = 0;
    for (int x = 0; x < 320; x++) {
        int r = (int)(px[320 + x] >> 16);
        mid += r > 40 && r < 215;
    }
    CHECK(mid == 0, "replicate mode: no interpolation (%d)", mid);

    /* Gamma: the square-root curve (0x40 -> 0x80); with gamma dither the low bits vary, within two steps. */
    for (int i = 0; i < 640 * 4; i++)
        n64_wbe32(m->rdram + fb + 4 * i, 0x404040FF);
    vi_mode(m, 0x3303 | 0x8, fb, 640, 0x006C02EC, 0x002501FF, 0x400, 0x400);
    n64_display(m, px, 640, 576, &w, &h);
    CHECK(near(px[640 + 16], rgb_of(0x80, 0x80, 0x80), 1), "gamma 0x40 -> %06X", px[640 + 16]);
    for (int i = 0; i < 640 * 4; i++)
        n64_wbe32(m->rdram + fb + 4 * i, 0x101010FF); /* 0x10 -> 0x40, and up to 0x42 with the dither */
    vi_mode(m, 0x3303 | 0xC, fb, 640, 0x006C02EC, 0x002501FF, 0x400, 0x400);
    n64_display(m, px, 640, 576, &w, &h);
    int differ = 0;
    for (int x = 8; x < 72; x++) { /* columns 0..7: the guard band at 640 wide */
        CHECK(near(px[640 + x], rgb_of(0x41, 0x41, 0x41), 2), "gamma dither %06X", px[640 + x]);
        differ += px[640 + x] != px[640 + 8];
    }
    CHECK(differ > 0, "gamma dither varies");

    /* Dither filter (16-bit, full coverage): alternating 5-bit 15 / 16 comes back near 8-bit 127. */
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 320; x++) {
            int v = ((x + y) & 1) ? 16 : 15;
            n64_wbe16(m->rdram + fb + 2 * (y * 320 + x), c16(v, v, v, 1));
        }
    uint8_t *hid = (uint8_t *)n64_rdp_hidden_bits(m->rdp);
    if (hid)
        memset(hid + (fb >> 1), 3, 320 * 8);
    vi_mode(m, 0x3302 | 0x10000, fb, 320, 0x006C02EC, 0x002501FF, 0x200, 0x400);
    n64_display(m, px, 640, 576, &w, &h);
    CHECK(near(px[320 * 3 + 10], rgb_of(127, 127, 127), 5) && near(px[320 * 3 + 11], rgb_of(127, 127, 127), 5),
          "dither filter: %06X %06X", px[320 * 3 + 10], px[320 * 3 + 11]);

    /* AA (mode 0) with the RDP's coverage: a partially covered white pixel between full black and full white moves
     * towards the background; without AA (mode 2) it stays white. */
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 320; x++)
            n64_wbe16(m->rdram + fb + 2 * (y * 320 + x), x < 100 ? c16(0, 0, 0, 1) : c16(31, 31, 31, 1));
    if (hid) {
        memset(hid + (fb >> 1), 3, 320 * 8);
        n64_wbe16(m->rdram + fb + 2 * (4 * 320 + 100), c16(31, 31, 31, 0)); /* coverage 3 of 7 */
        hid[(fb + 2 * (4 * 320 + 100)) >> 1] = 3;
        vi_mode(m, 0x3002, fb, 320, 0x006C02EC, 0x002501FF, 0x200, 0x400);
        n64_display(m, px, 640, 576, &w, &h);
        uint32_t aa = px[320 * 5 + 100];
        CHECK(aa != 0xFFFFFF && (aa & 255) > 40 && (aa & 255) < 230, "AA edge pixel %06X", aa);
        CHECK_EQ(px[320 * 5 + 150], 0xFFFFFF, "full coverage untouched");
        vi_mode(m, 0x3202, fb, 320, 0x006C02EC, 0x002501FF, 0x200, 0x400);
        n64_display(m, px, 640, 576, &w, &h);
        CHECK_EQ(px[320 * 5 + 100], 0xFFFFFF, "no AA in resample mode");
    }
    n64_destroy(m);

    /* Interlaced 640 x 480 (libultra's F1 modes: WIDTH 1280, the second field one line further): weave gives the
     * framebuffer's 480 lines back in order, whichever field comes first. */
    m = mk(&ntsc);
    for (int y = 0; y < 480; y++)
        for (int x = 0; x < 640; x++)
            n64_wbe16(m->rdram + fb + 2 * (y * 640 + x), c16(y & 31, (y >> 5) & 31, x & 1, 1));
    for (int order = 0; order < 2; order++) {
        for (int f = 0; f < 2; f++) {
            int field = f ^ order;
            vi_mode(m, 0x3342, fb + (uint32_t)field * 1280, 640 * 2, 0x006C02EC, 0x002501FF, 0x400, 0x400);
            CHECK(n64_display(m, px, 640, 576, &w, &h) == 0, "interlaced field");
        }
        CHECK(w == 640 && h == 480, "interlaced canvas %dx%d", w, h);
        int bad = 0;
        for (int y = 0; y < 474; y++) {
            uint32_t p = px[(size_t)(y + 2) * 640 + 10]; /* active line 0 on field row 1: canvas rows 2, 3 */
            int r = (int)(p >> 16), g = (int)((p >> 8) & 255);
            if (r != ((y & 31) << 3 | (y & 31) >> 2) || g != (((y >> 5) & 31) << 3 | ((y >> 5) & 31) >> 2))
                bad++;
        }
        CHECK(bad == 0, "weave order %d: %d lines out of place", order, bad);
    }
    n64_destroy(m);

    /* PAL: 288 lines, the PAL active area (half-lines 95..569) starts on row 25. */
    n64_options pal = {.force_pal = 1};
    m = mk(&pal);
    for (int i = 0; i < 320 * 240; i++)
        n64_wbe16(m->rdram + fb + 2 * i, c16(0, 0, 31, 1));
    vi_mode(m, 0x3302, fb, 320, 0x00800300, 0x005F0239, 0x200, 0x400);
    n64_display(m, px, 640, 576, &w, &h);
    CHECK(w == 320 && h == 288, "PAL canvas %dx%d", w, h);
    CHECK(px[320 * 24 + 5] == 0 && px[320 * 25 + 5] == 0x0000FF && px[320 * 261 + 5] == 0x0000FF &&
              px[320 * 262 + 5] == 0,
          "PAL active rows 25..261");
    n64_destroy(m);

    m = mk(&ntsc);
    /* Timing: V_CURRENT counts lines; the VI interrupt at V_INTR ends n64_run_frame; 60 fields a second (NTSC). */
    w32(m, 0x0440000C, 0x200); /* V_INTR: line 256 */
    n64_run_frame(m);
    CHECK_EQ(r32(m, 0x04400010) >> 1, 256, "frame ends at V_INTR");
    CHECK(r32(m, 0x04300008) & (1u << N64_MI_VI), "VI interrupt");
    uint64_t c0 = n64_cycles(m);
    for (int i = 0; i < 60; i++)
        n64_run_frame(m);
    double secs = (double)(n64_cycles(m) - c0) / N64_CPU_HZ;
    CHECK(secs > 0.99 && secs < 1.01, "60 NTSC fields in %.4f s", secs);
    n64_destroy(m);

    n64_options o = {.force_pal = 1};
    m = mk(&o);
    c0 = n64_cycles(m);
    for (int i = 0; i < 50; i++)
        n64_run_frame(m);
    secs = (double)(n64_cycles(m) - c0) / N64_CPU_HZ;
    CHECK(secs > 0.99 && secs < 1.01, "50 PAL fields in %.4f s", secs);
    n64_destroy(m);
}

/* ---- The VI's change test (n64_display_changed) ------------------------------------------------------------------ */

/* RDP commands into RDRAM at 0x700000 and run through the DPC registers (the software RDP): a fill of rows y0..y1 of
 * a 320-pixel 16-bit colour image at addr, then SYNC_FULL. */
static void rdp_fill(n64_machine *m, uint32_t addr, int y0, int y1)
{
    const uint64_t op[] = {
        (uint64_t)0x3F << 56 | (uint64_t)2 << 51 | (uint64_t)(320 - 1) << 32 | addr, /* colour image RGBA16 */
        (uint64_t)0x2D << 56 | (uint64_t)(320 << 2) << 12 | (uint64_t)(240 << 2),    /* scissor 0,0 .. 320,240 */
        (uint64_t)0x2F << 56 | (uint64_t)3 << 52,                                    /* fill mode */
        (uint64_t)0x37 << 56 | 0xF801F801u,                                          /* red */
        (uint64_t)0x36 << 56 | (uint64_t)(319 << 2) << 44 | (uint64_t)(y1 << 2) << 32 | (uint64_t)(y0 << 2),
        (uint64_t)0x29 << 56,                                                        /* SYNC_FULL */
    };
    const uint32_t base = 0x700000;
    for (unsigned i = 0; i < sizeof op / sizeof op[0]; i++) {
        n64_wbe32(m->rdram + base + 8 * i, (uint32_t)(op[i] >> 32));
        n64_wbe32(m->rdram + base + 8 * i + 4, (uint32_t)op[i]);
    }
    w32(m, 0x04100000, base);
    w32(m, 0x04100004, base + 8 * (uint32_t)(sizeof op / sizeof op[0]));
}

/* 1 until the picture was made, then 0 until something it is made from changes: the VI's registers, a CPU store or
 * the RDP's rows inside the RDRAM it reads (not the next buffer, even on a shared 4 KB page), a state load; always 1
 * interlaced; the gamma dither's noise is not a change. */
static void test_display_changed(void)
{
    n64_options ntsc = {.force_pal = 0};
    n64_machine *m = mk(&ntsc);
    static uint32_t px[640 * 576];
    int w, h;
    const uint32_t fb = 0x100000, fb2 = fb + 320 * 240 * 2; /* fb2 starts inside fb's last 4 KB page */
    vi_mode(m, 0x3302, fb, 320, 0x006C02EC, 0x002501FF, 0x200, 0x400);
    CHECK(n64_display_changed(m) == 1, "no picture yet");
    n64_display(m, px, 640, 576, &w, &h);
    CHECK(n64_display_changed(m) == 0, "the same picture");
    n64_run_frame(m);
    n64_run_frame(m);
    CHECK(n64_display_changed(m) == 0, "frames run, nothing written");

    w32(m, fb + 100 * 640 + 8, 0x12345678); /* a CPU store into the framebuffer */
    CHECK(n64_display_changed(m) == 1, "CPU store into the framebuffer");
    CHECK(n64_display_changed(m) == 1, "still changed until shown");
    n64_display(m, px, 640, 576, &w, &h);
    CHECK(n64_display_changed(m) == 0, "shown");
    w32(m, 0x300000, 1);
    CHECK(n64_display_changed(m) == 0, "a store elsewhere");

    rdp_fill(m, fb2, 0, 9);
    CHECK(n64_display_changed(m) == 0, "the RDP drawing the next buffer (same page as the last line)");
    CHECK(m->rdram[fb2] != 0, "the RDP drew there");
    rdp_fill(m, fb, 10, 19);
    CHECK(n64_display_changed(m) == 1, "the RDP drawing the framebuffer");
    n64_display(m, px, 640, 576, &w, &h);
    CHECK_EQ(px[320 * 12 + 100], 0xFF0000, "its rows shown");
    CHECK(n64_display_changed(m) == 0, "shown");

    w32(m, 0x04400030, 0x201); /* X_SCALE */
    CHECK(n64_display_changed(m) == 1, "a VI register");
    n64_display(m, px, 640, 576, &w, &h);
    CHECK(n64_display_changed(m) == 0, "shown");
    w32(m, 0x04400004, fb2); /* the buffer swap */
    CHECK(n64_display_changed(m) == 1, "VI_ORIGIN");
    n64_display(m, px, 640, 576, &w, &h);

    w32(m, 0x04400000, 0x3306); /* gamma dither: noise that follows the frame count */
    n64_display(m, px, 640, 576, &w, &h);
    n64_run_frame(m);
    CHECK(n64_display_changed(m) == 0, "gamma dither alone is no change");

    static uint8_t st[N64_RDRAM_SIZE + (1u << 20)];
    size_t n = n64_state_save(m, st, sizeof st);
    CHECK(n > 0, "state saved");
    CHECK(n64_display_changed(m) == 0, "a state save changes nothing");
    CHECK(n64_state_load(m, st, n) == N64_STATE_OK, "state loaded");
    CHECK(n64_display_changed(m) == 1, "a state load");
    n64_display(m, px, 640, 576, &w, &h);

    vi_mode(m, 0x3342, fb, 640, 0x006C02EC, 0x002501FF, 0x400, 0x400); /* interlaced */
    for (int i = 0; i < 3; i++) {
        n64_run_frame(m);
        CHECK(n64_display_changed(m) == 1, "interlaced: every VI (%d)", i);
        n64_display(m, px, 640, 576, &w, &h);
    }
    n64_destroy(m);
}

/* ---- Deferred DP interrupt (n64_core.c) -------------------------------------------------------------------------- */
/* The core's policy with a back end that defers (the software RDP behind n64_rdp_set_defer_software: drawn at once,
 * only its DP interrupt held until n64_rdp_dp_complete). A "game" swaps VI_ORIGIN right after a VI interrupt, sends
 * `syncs` SYNC_FULLs at `at` VIs into the picture and shows a picture every `vis` VIs. */
static void dp_sync_full(n64_machine *m)
{
    const uint32_t base = 0x700000;
    n64_wbe32(m->rdram + base, 0x29u << 24);
    n64_wbe32(m->rdram + base + 4, 0);
    w32(m, 0x04100000, base);
    w32(m, 0x04100004, base + 8);
}

static int dp_irq(n64_machine *m) { return (int)(m->mi_intr >> N64_MI_DP & 1); }

typedef struct {
    uint64_t deadline; /* EV_DP right after the SYNC_FULL (0: completed at once) */
    uint64_t vi1;      /* the first VI interrupt after it */
    int irq_now;       /* the DP interrupt right after the SYNC_FULL */
    int irq_end;       /* ... at the end of the picture */
} dp_pic;

static dp_pic dp_picture(n64_machine *m, int vis, double at, int syncs, int k)
{
    dp_pic r = {0, 0, 0, 0};
    const uint64_t period = m->vi_line_ticks * (uint64_t)m->vi_lines;
    w32(m, 0x04400004, k & 1 ? 0x200000 : 0x100000);
    n64_run_cycles(m, (uint64_t)(at * (double)period) / N64_TICKS_PER_CYCLE);
    uint64_t f0 = n64_frames(m);
    for (int i = 0; i < syncs; i++) {
        n64_mi_clear(m, N64_MI_DP);
        dp_sync_full(m);
    }
    r.deadline = m->ev[EV_DP] == EV_NEVER ? 0 : m->ev[EV_DP];
    r.irq_now = dp_irq(m);
    int left = vis - (int)at;
    for (int i = 0; i < left; i++) {
        n64_run_frame(m);
        if (i == 0)
            r.vi1 = m->dpp.last_vi;
    }
    (void)f0;
    r.irq_end = dp_irq(m);
    return r;
}

static void test_dp_defer(void)
{
    int off = 0;
    CHECK(n64_rdp_defer_parse(NULL, &off) == N64_DP_DEFER_AUTO && off == 62, "default: auto, 1/16 VI");
    CHECK(n64_rdp_defer_parse("auto", &off) == N64_DP_DEFER_AUTO && off == 62, "auto");
    CHECK(n64_rdp_defer_parse("0", &off) == N64_DP_DEFER_OFF, "0: off");
    CHECK(n64_rdp_defer_parse("25", &off) == N64_DP_DEFER_AUTO && off == 250, "25 %% of a VI");
    CHECK(n64_rdp_defer_parse("1", &off) == N64_DP_DEFER_AUTO && off == 62, "1: on");
    CHECK(n64_rdp_defer_parse("500", &off) == N64_DP_DEFER_AUTO && off == 1000, "at most a VI");

    n64_rdp_set_defer_software(1);
    n64_machine *m = mk(NULL); /* PAL */
    n64_set_rdp_defer(m, N64_DP_DEFER_AUTO, 0);
    const uint64_t period = m->vi_line_ticks * (uint64_t)m->vi_lines;
    for (int i = 0; i < 70; i++) /* the first second: never */
        n64_run_frame(m);

    /* A picture every VI: never deferred. */
    int deferred = 0;
    for (int k = 0; k < 10; k++) {
        dp_pic p = dp_picture(m, 1, 0.3, 1, k);
        deferred += p.deadline != 0;
        CHECK(p.irq_now, "1 VI: the DP interrupt at once (%d)", k);
    }
    CHECK(deferred == 0, "a picture every VI: %d deferred", deferred);

    /* Every 3 VIs, SYNC_FULL at 0.7 VI (Ocarina of Time): deferred from the 4th picture, to the next VI + 1/16. */
    deferred = 0;
    for (int k = 0; k < 10; k++) {
        dp_pic p = dp_picture(m, 3, 0.7, 1, k);
        if (k < 3) {
            CHECK(!p.deadline && p.irq_now, "3 VIs, picture %d: at once (3 qualifying pictures first)", k);
        } else {
            CHECK(p.deadline && !p.irq_now, "3 VIs, picture %d: deferred", k);
            CHECK_EQ(p.deadline, p.vi1 + period * 62 / 1000, "deadline: the next VI + 1/16");
            CHECK(p.irq_end, "3 VIs, picture %d: the DP interrupt by the end", k);
        }
        deferred += p.deadline != 0;
    }
    CHECK(deferred == 7, "3 VIs: %d deferred", deferred);
    n64_perf pf;
    n64_perf_get(m, &pf);
    CHECK(pf.dp_backoffs == 0, "no backoff (%llu)", (unsigned long long)pf.dp_backoffs);

    /* Every 2 VIs, SYNC_FULL early in the first one: deferred too (the swap is a whole VI after the deadline). */
    deferred = 0;
    for (int k = 0; k < 8; k++) {
        dp_pic p = dp_picture(m, 2, 0.3, 1, k);
        deferred += p.deadline != 0;
        if (p.deadline)
            CHECK(p.irq_end, "2 VIs, picture %d: the DP interrupt by the end", k);
    }
    CHECK(deferred >= 4, "2 VIs, SYNC_FULL at 0.3: %d deferred", deferred);

    /* Every 2 VIs, SYNC_FULL in the second VI (the game swaps at the first VI after it): never, once the first such
     * picture (deferred on the strength of the ones before) is judged. */
    deferred = 0;
    for (int k = 0; k < 8; k++)
        deferred += dp_picture(m, 2, 1.5, 1, k).deadline != 0 && k > 0;
    CHECK(deferred == 0, "2 VIs, SYNC_FULL at 1.5: %d deferred", deferred);

    /* Two SYNC_FULLs per picture: never. */
    deferred = 0;
    for (int k = 0; k < 8; k++)
        deferred += dp_picture(m, 3, 0.7, 2, k).deadline != 0;
    CHECK(deferred == 0, "2 SYNC_FULLs a picture: %d deferred", deferred);

    /* Deferring again, then a picture that comes a VI later: backoff, nothing deferred for 500 VIs. */
    for (int k = 0; k < 5; k++)
        dp_picture(m, 3, 0.7, 1, k);
    dp_pic p = dp_picture(m, 4, 0.7, 1, 1);
    CHECK(p.deadline != 0, "deferred before the late picture");
    dp_picture(m, 3, 0.7, 1, 0); /* its VI_ORIGIN change judges the 4-VI picture */
    n64_perf_get(m, &pf);
    CHECK(pf.dp_backoffs == 1, "a later picture while deferring: backoff (%llu)", (unsigned long long)pf.dp_backoffs);
    deferred = 0;
    for (int k = 0; k < 20; k++)
        deferred += dp_picture(m, 3, 0.7, 1, k).deadline != 0;
    CHECK(deferred == 0, "during the backoff: %d deferred", deferred);
    for (int i = 0; i < 500; i++)
        n64_run_frame(m);

    /* A save state completes a pending one first: it never holds one. */
    for (int k = 0; k < 4; k++)
        dp_picture(m, 3, 0.7, 1, k);
    w32(m, 0x04400004, 0x100000);
    n64_run_cycles(m, (uint64_t)(0.7 * (double)period) / N64_TICKS_PER_CYCLE);
    n64_mi_clear(m, N64_MI_DP);
    dp_sync_full(m);
    CHECK(m->ev[EV_DP] != EV_NEVER && !dp_irq(m), "pending before the save");
    static uint8_t st[N64_RDRAM_SIZE + (1u << 20)];
    size_t n = n64_state_save(m, st, sizeof st);
    CHECK(n > 0, "state saved");
    CHECK(m->ev[EV_DP] == EV_NEVER && dp_irq(m), "the save completed it");
    n64_perf_get(m, &pf);
    CHECK(pf.dp_early == 1, "dp_early %llu", (unsigned long long)pf.dp_early);
    CHECK(n64_state_load(m, st, n) == N64_STATE_OK, "state loaded");
    CHECK(m->ev[EV_DP] == EV_NEVER, "the state holds none");
    CHECK(m->dpp.warm == 0 && m->dpp.streak == 0, "the policy starts again after a load");

    /* Reset drops a pending one; off: never. */
    for (int i = 0; i < 70; i++)
        n64_run_frame(m);
    for (int k = 0; k < 4; k++)
        dp_picture(m, 3, 0.7, 1, k);
    w32(m, 0x04400004, 0x100000);
    n64_run_cycles(m, (uint64_t)(0.7 * (double)period) / N64_TICKS_PER_CYCLE);
    n64_mi_clear(m, N64_MI_DP);
    dp_sync_full(m);
    CHECK(m->ev[EV_DP] != EV_NEVER, "pending before the reset");
    n64_reset(m);
    CHECK(m->ev[EV_DP] == EV_NEVER && !dp_irq(m), "reset dropped it");
    n64_destroy(m);

    m = mk(NULL);
    n64_set_rdp_defer(m, N64_DP_DEFER_OFF, 0);
    for (int i = 0; i < 70; i++)
        n64_run_frame(m);
    deferred = 0;
    for (int k = 0; k < 10; k++)
        deferred += dp_picture(m, 3, 0.7, 1, k).deadline != 0;
    CHECK(deferred == 0, "off: %d deferred", deferred);
    n64_destroy(m);
    n64_rdp_set_defer_software(0);
}

/* ---- Save states (XBA-183) -------------------------------------------------------------------------------------- */

static void test_states(void)
{
    n64_machine *m = mk(NULL);
    /* A machine with something everywhere: CPU and FPU registers, a TLB entry, RDRAM, the RSP, saves and paks, VI and
     * AI registers, a FlashRAM cartridge, an IS-Viewer buffer. */
    for (int r = 1; r < 32; r++)
        m->cpu.gpr[r] = 0x0123456789ABCDEFull * (uint64_t)r;
    m->cpu.hi = 0x1111;
    m->cpu.lo = 0x2222;
    for (int r = 0; r < 32; r++)
        m->cpu.fpr[r] = 0x4000000000000000ull + (uint64_t)r;
    m->cpu.fcsr = 0x01000003u;
    m->cpu.tlb[3].hi = 0x80000000ull;
    m->cpu.tlb[3].lo[0] = 0x1234;
    for (uint32_t i = 0; i < 0x1000; i += 4)
        n64_poke32(m, 0x80100000u + i, 0xC0DE0000u + i);
    n64_poke32(m, 0x80700000u, 0xFEEDFACEu);  /* the expansion pak's half */
    n64_poke32(m, 0xA4000100u, 0x55AA55AAu);  /* DMEM */
    n64_rsp_set_gpr(m->rsp, 7, 0xBEEF);
    for (int i = 0; i < 64; i++) {
        m->eeprom[i] = (uint8_t)(i * 3);
        m->sram[i] = (uint8_t)(i * 5);
        m->pak[1][i] = (uint8_t)(i * 7);
    }
    n64_save_set_kind(m, N64_SAVE_FLASH);
    m->flash[100] = 0x42;
    w32(m, 0x13FF0020, 0x41424344); /* IS-Viewer buffer */
    w32(m, 0x0440000C, 0x200);      /* V_INTR */
    w32(m, 0x04500014, 15);         /* AI_BITRATE */
    for (int i = 0; i < 3; i++)
        n64_run_frame(m);
    n64_save_take_dirty(m, N64_SAVE_FLASH);

    size_t cap = n64_state_size(m);
    uint8_t *st = malloc(cap + 16);
    size_t n = n64_state_save(m, st, cap);
    CHECK(n == cap && n > 0, "state size %zu, saved %zu", cap, n);
    CHECK(n64_state_save(m, st, cap - 1) == 0, "a too small buffer is refused");
    n = n64_state_save(m, st, cap);
    CHECK(n < 2u * 1024 * 1024, "zero pages left out: %zu bytes", n);
    CHECK(n64_state_check(m, st, n) == N64_STATE_OK, "own state checks");

    n64_machine *b = mk(NULL);
    CHECK(n64_state_load(b, st, n) == N64_STATE_OK, "loads into a fresh machine");
    char why[160] = "";
    CHECK(n64_recomp_compare(m, b, why, sizeof why) == 0, "same machine after the load: %s", why);
    CHECK_EQ(n64_peek32(b, 0x80700000u), 0xFEEDFACEu, "RDRAM above 4 MB");
    CHECK_EQ(n64_rsp_get_gpr(b->rsp, 7), 0xBEEF, "RSP register");
    CHECK(!memcmp(m->eeprom, b->eeprom, sizeof m->eeprom) && !memcmp(m->sram, b->sram, sizeof m->sram) &&
              !memcmp(m->pak, b->pak, sizeof m->pak), "EEPROM, SRAM and paks");
    CHECK(b->flash && b->flash[100] == 0x42 && b->save_kind == N64_SAVE_FLASH, "FlashRAM");
    CHECK(b->isv && n64_be32(b->isv + 0x20) == 0x41424344u, "IS-Viewer buffer");
    CHECK(!memcmp(m->vi, b->vi, sizeof m->vi) && m->ai_bitrate == b->ai_bitrate, "VI / AI registers");
    CHECK(b->rom != m->rom && b->rsp != m->rsp && b->rsp && b->rdp && b->spmem == n64_rsp_mem(b->rsp), "host side kept");
    int dirty = 0;
    for (int k = N64_SAVE_EEPROM4K; k <= N64_SAVE_PAK3; k++)
        dirty |= n64_save_take_dirty(b, (n64_save_kind)k);
    CHECK(!dirty, "a load leaves the saves clean (the files change once the game writes)");
    for (int i = 0; i < 3; i++) {
        n64_run_frame(m);
        n64_run_frame(b);
    }
    CHECK(n64_recomp_compare(m, b, why, sizeof why) == 0, "same run after the load: %s", why);

    /* Refusals leave the machine as it was. */
    n64_machine *c = mk(NULL);
    uint32_t before = n64_peek32(c, 0x80100000u);
    uint8_t *bad = malloc(n);
    memcpy(bad, st, n);
    bad[12] ^= 0xFF; /* the format version */
    CHECK(n64_state_load(c, bad, n) == N64_STATE_EVERSION, "another version refused");
    memcpy(bad, st, n);
    bad[0] = 'Y';
    CHECK(n64_state_load(c, bad, n) == N64_STATE_ECORRUPT, "not a state");
    CHECK(n64_state_load(c, st, n - 100) == N64_STATE_ECORRUPT, "truncated");
    CHECK(n64_state_load(c, st, 20) == N64_STATE_ECORRUPT, "header only");
    CHECK_EQ(n64_peek32(c, 0x80100000u), before, "refused states change nothing");
    n64_destroy(c);
    n64_options small = {.force_pal = -1, .rdram_size = 0x400000u};
    c = mk(&small);
    CHECK(n64_state_load(c, st, n) == N64_STATE_EVERSION, "a 4 MB machine refuses an 8 MB state");
    n64_destroy(c);
    static uint8_t rom2[ROM_SIZE];
    make_rom(rom2);
    rom2[0x2000] ^= 1;
    char err[128];
    c = n64_create(rom2, ROM_SIZE, NULL, err, sizeof err);
    CHECK(c && n64_state_load(c, st, n) == N64_STATE_EGAME, "another ROM refused");
    n64_destroy(c);
    free(bad);
    free(st);
    n64_destroy(b);
    n64_destroy(m);
}

int main(void)
{
    test_rom();
    test_boot();
    test_x105_boot();
    test_guest_health();
    test_joybus();
    test_pi_saves();
    test_ai();
    test_ai_resampler();
    test_vi();
    test_display_changed();
    test_dp_defer();
    test_states();
    printf("n64_io: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
