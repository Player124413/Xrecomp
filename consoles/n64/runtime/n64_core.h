/* Nintendo 64 core (N-1 XBA-169 ROM / boot, N-3 XBA-171 interpreter, N-4 XBA-172 PI / SI / VI / AI / saves).
 *
 * A VR4300 interpreter (MIPS III, 64-bit: every integer instruction, COP0 with the TLB and every exception, COP1 with
 * the FCSR rounding modes, flags, enables and the unimplemented-operation cases) on the N64 memory map: RDRAM (8 MB,
 * big-endian bytes), the RCP's MI, PI (cartridge DMA), SI + PIF (HLE boot, joybus: controllers, controller / rumble
 * paks, EEPROM), VI (scanline timing, interrupt, framebuffer output) and AI (DMA, 48 kHz output), and the saves (SRAM,
 * FlashRAM, EEPROM, controller paks). The RSP and the RDP are reached only through n64_devices.h: target n64_core links
 * n64_rsp and n64_rdp, each the real module or its stub (n64_rsp_stub.c / n64_rdp_stub.c) when the source is absent.
 * All the state lives in one n64_machine: no globals, several machines may run side by side (one per thread).
 *
 * Boot: the PIF is high-level emulated (there is no PIF ROM dump anywhere): the CIC is recognised from the IPL3 in
 * the ROM (CRC32 of 0x40..0xFFF: 6101, 6102 / 7101, 6103 / 7103, 6105 / 7105, 6106 / 7106, 7102; libdragon's open
 * IPL3 by its banner; anything else boots as a 6102), the PIF's work is done (header and IPL3 copied to DMEM, PI
 * domain 1 timing from the header, the registers IPL3 expects: seed, TV type, reset type, stack), and the CPU starts
 * at 0xA4000040 in IPL3, which runs as on the hardware (the RDRAM is reported configured, so IPL3 takes its warm-boot
 * path). The IPL3 of a commercial ROM is part of the user's own ROM at run time and is never stored. */
#ifndef N64_CORE_H
#define N64_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct n64_machine n64_machine;

#define N64_RDRAM_SIZE 0x800000u   /* 8 MB (expansion pak) */
#define N64_ROM_MAX 0x4000000u     /* 64 MB */
#define N64_AUDIO_RATE 48000
#define N64_SCREEN_MAX_W 640
#define N64_SCREEN_MAX_H 576

/* ---- ROM (n64_rom.c) ------------------------------------------------------------------------------------------- */
/* Byte orders of a ROM file, from the first word: z64 is the console's order (80 37 12 40), v64 swaps each 16-bit
 * half (37 80 40 12), n64 each 32-bit word (40 12 37 80). */
typedef enum n64_rom_order { N64_ROM_UNKNOWN = 0, N64_ROM_Z64, N64_ROM_V64, N64_ROM_N64 } n64_rom_order;

typedef enum n64_cic {
    N64_CIC_UNKNOWN = 0, /* boots as a 6102 */
    N64_CIC_6101,
    N64_CIC_6102, /* NTSC 6102, PAL 7101 */
    N64_CIC_6103, /* 7103 */
    N64_CIC_6105, /* 7105 */
    N64_CIC_6106, /* 7106 */
    N64_CIC_7102,
    N64_CIC_LIBDRAGON, /* libdragon's open IPL3 (6102 compatible) */
} n64_cic;

/* Save kinds (n64_save_*). The cartridge's own save is one of EEPROM 4K / 16K, SRAM, FlashRAM (or none); the
 * controller paks are per port. */
typedef enum n64_save_kind {
    N64_SAVE_NONE = 0,
    N64_SAVE_EEPROM4K, /* 512 B */
    N64_SAVE_EEPROM16K, /* 2 KB */
    N64_SAVE_SRAM,     /* 32 KB */
    N64_SAVE_FLASH,    /* 128 KB */
    N64_SAVE_PAK0,     /* 32 KB controller pak on port 0 (then 1, 2, 3) */
    N64_SAVE_PAK1,
    N64_SAVE_PAK2,
    N64_SAVE_PAK3,
} n64_save_kind;

typedef struct n64_rom_info {
    n64_rom_order order;    /* of the file as given */
    uint32_t pi_config;     /* header word 0 (PI domain 1 timing; 0x80371240) */
    uint32_t clock_rate;
    uint32_t boot_address;  /* where a classic IPL3 copies the first MB */
    uint32_t release;       /* libultra version word */
    uint32_t crc1, crc2;    /* header checksums (not checked) */
    char title[21];         /* trailing spaces removed */
    char game_code[5];      /* e.g. "NSME": media, 2-letter id, region */
    uint8_t version;
    char region;            /* game_code[3] */
    int pal;                /* 1 for a PAL region code (D F I P S U X Y ...), 0 NTSC / MPAL */
    uint32_t ipl3_crc;      /* CRC32 of bytes 0x40..0xFFF */
    n64_cic cic;
    n64_save_kind save_hint; /* from the ED64 / libdragon header byte ("ED" at 0x3C, type in 0x3F >> 4), else NONE */
    int save_hint_rtc;      /* the same byte asks for the joybus RTC */
} n64_rom_info;

/* Detects the byte order of a file (at least 4 bytes). */
n64_rom_order n64_rom_detect(const uint8_t *data, size_t size);
/* Converts a ROM to the console's big-endian order in place (size a multiple of 4 for n64, 2 for v64). Returns 0 or
 * -1 for an unknown order. */
int n64_rom_normalize(uint8_t *data, size_t size);
/* Reads the header of a big-endian ROM (at least 0x1000 bytes). Returns 0 or -1. */
int n64_rom_parse(const uint8_t *be, size_t size, n64_rom_info *info);
const char *n64_cic_name(n64_cic cic);
/* CRC-32 (IEEE, as zlib), for the IPL3 and title keys. */
uint32_t n64_crc32(const uint8_t *p, size_t n);

/* ---- Machine ----------------------------------------------------------------------------------------------------- */
typedef struct n64_options {
    int force_pal;            /* -1 from the ROM's region (default), 0 NTSC, 1 PAL */
    n64_save_kind save_kind;  /* NONE: detect (header hint, else at the first access); or force one */
    uint32_t rdram_size;      /* 0: 8 MB; or 4 MB (no expansion pak) */
} n64_options;

/* Creates a machine from a ROM in any byte order (copied and normalised; at most N64_ROM_MAX bytes) and starts it in
 * IPL3 through the HLE PIF. opts may be NULL (defaults). Returns NULL and a message in err (may be NULL) on failure.
 * Controller 1 is plugged in (with a controller pak), 2-4 are not. */
n64_machine *n64_create(const uint8_t *rom, size_t size, const n64_options *opts, char *err, size_t errlen);
void n64_destroy(n64_machine *m);
/* Back to the state n64_create left (saves, paks and controllers kept). */
void n64_reset(n64_machine *m);
const n64_rom_info *n64_info(n64_machine *m);

/* Controller buttons (1 = pressed), the bit layout of the joybus reply's first two bytes. */
enum {
    N64_BTN_CRIGHT = 1 << 0,
    N64_BTN_CLEFT = 1 << 1,
    N64_BTN_CDOWN = 1 << 2,
    N64_BTN_CUP = 1 << 3,
    N64_BTN_R = 1 << 4,
    N64_BTN_L = 1 << 5,
    N64_BTN_DRIGHT = 1 << 8,
    N64_BTN_DLEFT = 1 << 9,
    N64_BTN_DDOWN = 1 << 10,
    N64_BTN_DUP = 1 << 11,
    N64_BTN_START = 1 << 12,
    N64_BTN_Z = 1 << 13,
    N64_BTN_B = 1 << 14,
    N64_BTN_A = 1 << 15,
};

typedef enum n64_accessory {
    N64_ACC_NONE = 0,
    N64_ACC_CONTROLLER_PAK, /* 32 KB memory pak (N64_SAVE_PAKn) */
    N64_ACC_RUMBLE_PAK,
} n64_accessory;

/* Port 0..3: plugged in or not, buttons (N64_BTN_*) and the stick (-128..127, about +-85 at the gate; up is +y). */
void n64_set_controller(n64_machine *m, int port, int connected, uint16_t buttons, int8_t x, int8_t y);
void n64_set_accessory(n64_machine *m, int port, n64_accessory acc);
/* 1 while the game runs the rumble pak's motor on that port. */
int n64_rumble(n64_machine *m, int port);

/* Test hook (XBA-210): on, the machine runs the interpreter's reference loop (one full step per instruction) instead
 * of its predecoded blocks; n64_run --lockstep-ref runs both and compares them. A host setting, not in save states. */
void n64_set_interp_reference(n64_machine *m, int on);

/* Runs until the next VI interrupt (one field: 1/60 s NTSC, 1/50 s PAL; a field's length when the game masks it). */
void n64_run_frame(n64_machine *m);
/* Runs at least `cycles` CPU cycles (93.75 MHz; tests). */
void n64_run_cycles(n64_machine *m, uint64_t cycles);

/* The picture the VI shows: 0x00RRGGBB, the visible area (H/V_VIDEO, X/Y_SCALE) at framebuffer resolution, at most
 * max_w x max_h. Returns 0 and the size, or -1 when the VI is blank (w / h then a default size). With the Vulkan RDP
 * upscaling (n64_rdp.h n64_rdp_scanout_scale > 0) and a buffer that holds it, its VI's picture at the upscaled size. */
int n64_display(n64_machine *m, uint32_t *xrgb, int max_w, int max_h, int *w, int *h);
/* n64_display's upscaled picture (the Vulkan RDP's scanout, n64_rdp.h n64_rdp_scanout_scale) without its copies:
 * *rgba points at the RDP's own RGBA8 bytes (r, g, b, a; show it opaque), *w x *h, rows *pitch bytes apart, valid
 * until the second picture after it (n64_rdp_scanout_rgba). 0; -2 when there is no upscaled scanout now (none, or
 * interlaced): n64_display then; -1 when the upscaled scanout shows nothing: n64_display with a 1x buffer then (as
 * n64_display would fall back to). Counts as n64_display for n64_display_changed when it returns 0. */
int n64_display_rgba(n64_machine *m, const uint8_t **rgba, int *w, int *h, int *pitch);
/* 0 when n64_display would now give the same picture as at its last call, 1 when it may differ (or there was none).
 * Conservative: 1 unless all of these are as they were then: the VI registers that shape the picture (V_CURRENT
 * aside), the back end and its scanout scale (the caller's buffer the same as then); the field order is not tracked, so an
 * interlaced mode is always 1; the RDRAM the VI reads (its lines and the filters' neighbours) was not written: by
 * the CPU, lifted code or a PI / SI DMA (per 4 KB page: n64_core's write counters, which a state load, a reset and
 * n64_rdram move too), nor by the RDP or SP DMA (their exact bus.wrote ranges), and no row the RDP has drawn but
 * not yet reported (n64_rdp.h n64_rdp_unreported; the software RDP is flushed) overlaps it. With an upscaled scanout
 * a non-RCP write keeps it 1 until the VI has shown the RDP's next SYNC_FULL (paraLLEl-RDP takes such writes into its
 * upscaled copy at its next pass). The gamma dither noise of the 1x VI (a function of the frame) is not a change.
 * Host-only (no effect on the machine). The runtime makes, converts and uploads a picture only when this says so. */
int n64_display_changed(n64_machine *m);
/* The framebuffer the VI scans (ORIGIN, WIDTH and the pixel type of its registers), raw: 0x00RRGGBB from its top-left
 * pixel, WIDTH (at most max_w) wide and max_h lines (fewer at the end of RDRAM), no VI filter or scaling (XBA-202:
 * test ROMs whose references are framebuffer dumps). Returns 0 and the size, or -1 when the VI is blank. */
int n64_framebuffer(n64_machine *m, uint32_t *xrgb, int max_w, int max_h, int *w, int *h);

/* Audio: stereo int16, interleaved L/R, N64_AUDIO_RATE, made while n64_run_frame runs (silence while the AI is
 * idle). Copies at most frames sample frames and returns how many. Same thread as n64_run_frame. */
size_t n64_audio_pull(n64_machine *m, int16_t *stereo, size_t frames);
/* The AI's current DAC rate in Hz (0 before the game sets it). */
uint32_t n64_audio_source_rate(n64_machine *m);

/* Saves (like gba_save / ps1_card): the cartridge's kind once known (N64_SAVE_NONE until detected); n64_save_data the
 * image of that kind (NULL if not there; write it to storage as is); n64_save_take_dirty 1 when the game wrote it
 * since the last call (clears the flag); n64_save_load puts a stored image back (exact size; a cartridge kind also
 * fixes the cartridge's save kind). Returns 0 or -1. */
n64_save_kind n64_save_kind_detected(n64_machine *m);
size_t n64_save_size(n64_save_kind kind);
const uint8_t *n64_save_data(n64_machine *m, n64_save_kind kind, size_t *size);
int n64_save_take_dirty(n64_machine *m, n64_save_kind kind);
int n64_save_load(n64_machine *m, n64_save_kind kind, const uint8_t *data, size_t size);

/* Save states (XBA-183): the whole machine as a versioned blob: a header (magic, console "n64", N64_STATE_VERSION, the
 * ROM's hash, RDRAM's size), then sections each checked against this build: the machine (the VR4300 with its GPRs,
 * HI / LO, PC and delay slot state, COP0 with COUNT / COMPARE and the TLB, COP1 with FCSR and the FR mode; time and
 * the event scheduler; MI, VI, AI with its DMA queue and resampler, PI, SI / PIF RAM with the controllers and paks, RI;
 * EEPROM, SRAM and controller pak contents, the RTC), RDRAM (4 or 8 MB, all-zero 4 KB pages left out), the RSP
 * (n64_devices.h: IMEM / DMEM, scalar and vector units), the RDP (its DPC registers and the back end's drawing state;
 * drawn out first), FlashRAM, the IS-Viewer buffer, the recompiled code's boot flag. Take and load them between
 * frames. A state is the machine only, so it loads the same interpreted or recompiled (lifted code is checked against
 * RDRAM again after a load: every page counts as written). A state never writes the save files: the cartridge and
 * paks in the machine become the state's, the runtime writes them once the game does (n64_save_take_dirty is clear
 * after a load).
 * n64_state_size: the bytes n64_state_save needs now (RDRAM's zero pages depend on its contents: call it right
 * before). n64_state_save: the size written, 0 when cap is too small. n64_state_check / n64_state_load:
 * N64_STATE_OK, or why the state is refused (another ROM, another version / build or RDRAM size, not a state); the
 * machine is left as it was. The values match recomp_states.h's RECOMP_STATE_*. */
#define N64_STATE_VERSION 1
enum {
    N64_STATE_OK = 0,
    N64_STATE_EGAME = -3,
    N64_STATE_EVERSION = -4,
    N64_STATE_ECORRUPT = -7,
};
size_t n64_state_size(n64_machine *m);
size_t n64_state_save(n64_machine *m, uint8_t *out, size_t cap);
int n64_state_check(n64_machine *m, const uint8_t *data, size_t size);
int n64_state_load(n64_machine *m, const uint8_t *data, size_t size);

/* Debug / test accessors: memory without side effects (virtual KSEG0 / KSEG1 or physical addresses below
 * 0x20000000: RDRAM, RSP DMEM / IMEM, ROM, PIF RAM; others read 0), CPU registers, PC, HI / LO, COP0 / COP1. */
uint32_t n64_peek32(n64_machine *m, uint32_t addr);
uint16_t n64_peek16(n64_machine *m, uint32_t addr);
uint8_t n64_peek8(n64_machine *m, uint32_t addr);
void n64_poke32(n64_machine *m, uint32_t addr, uint32_t v); /* RDRAM / DMEM / IMEM only */
uint64_t n64_reg(n64_machine *m, int r);
uint64_t n64_pc(n64_machine *m);
uint64_t n64_cop0(n64_machine *m, int r);
uint64_t n64_fpr(n64_machine *m, int r); /* the 64-bit register file as the FR=1 view sees it */
uint32_t n64_fcsr(n64_machine *m);
uint64_t n64_cycles(n64_machine *m);
uint64_t n64_frames(n64_machine *m);
uint8_t *n64_rdram(n64_machine *m); /* RDRAM, big-endian bytes, writable (code written there is seen) */
uint32_t n64_rdram_size(n64_machine *m);

/* Debug text: what the game writes to the IS-Viewer 64 buffer (0x13FF0020, length at 0x13FF0014: libdragon's
 * debug_init_isviewer, libultra's osSyncPrintf on development carts). NULL: dropped. */
void n64_set_tty(n64_machine *m, void (*fn)(void *ctx, char c), void *ctx);

/* The RCP devices, for tests and the runtime. */
struct n64_rsp *n64_get_rsp(n64_machine *m);
struct n64_rdp *n64_get_rdp(n64_machine *m);

/* Statistics: instructions executed (n64_run_frame / n64_run_cycles). */
uint64_t n64_instructions(n64_machine *m);

/* Host performance counters (the runtime's « n64: perf » line): where the host's time goes, totals since n64_create.
 * Host-only: not in save states, no effect on the machine (runs hash the same with them on or off). The stage
 * timers cost two clock reads per CPU slice (about 5000 a frame while the RSP runs), so they time one frame in
 * `every` (n64_perf_enable; 0, the default: off). The counts run on every frame. */
typedef struct n64_perf {
    uint64_t frames;           /* n64_run_frame calls */
    uint64_t timed;            /* of those, the frames whose stages were timed */
    uint64_t cpu_ns;           /* in the timed frames: the CPU (interpreter or pack: n64_cpu_run), its RDP runs out */
    uint64_t rsp_ns;           /* the RSP (n64_rsp_run), without the RDP runs its DPC_END writes start */
    uint64_t rdp_ns;           /* the RDP: every n64_rdp_run (after an RSP slice, and those the RSP's or the CPU's
                                * DPC_END writes start); the software RDP draws there, the Vulkan RDP queues (and
                                * blocks on a full command ring, rdp_queue_ns) and waits at SYNC_FULL */
    uint64_t frame_ns;         /* the whole n64_run_frame (the scheduler, VI / AI / PI / SI events included) */
    uint64_t rsp_instructions; /* every frame: RSP instructions run */
    uint64_t rdp_commands;     /* RDP commands run (n64_rdp_get_stats) */
    uint64_t rdp_full_syncs;   /* of those, SYNC_FULL */
    /* RDP flushes that drew or waited (the threaded software RDP's batches, the Vulkan RDP's waits for the GPU) and
     * their time, wherever they happen: in n64_rdp_run, and those the VI asks for (n64_display, save states). */
    uint64_t rdp_waits;
    uint64_t rdp_wait_ns;
    /* The Vulkan RDP (n64_rdp.h n64_rdp_stats, every frame): waits for room in its full command ring (inside
     * rdp_ns, not in rdp_wait_ns), its upscaled scanouts (their wait is part of rdp_wait_ns), stalled pipeline
     * compiles. */
    uint64_t rdp_queue_waits, rdp_queue_ns;
    uint64_t rdp_scanouts, rdp_scanout_ns;
    uint64_t rdp_stalled_compiles, rdp_stalled_compile_ns;
    /* The deferred DP interrupt (n64_set_rdp_defer): SYNC_FULLs whose DP interrupt the policy put off to a deadline,
     * those completed at once (the policy said now), those completed before their deadline (a save state), pictures
     * that came later while deferring (the policy then stops for a while); the back end's waits for the GPU at the
     * deadlines and their time (n64_rdp_stats dp_*, inside rdp_ns). */
    uint64_t dp_deferred, dp_now, dp_early, dp_backoffs;
    uint64_t rdp_dp_waits, rdp_dp_wait_ns;
    uint64_t rdp_prefetches, rdp_prefetch_hits; /* early scanouts started, taken (n64_rdp.h n64_rdp_scanout_prefetch) */
} n64_perf;
void n64_perf_enable(n64_machine *m, int every);

void n64_perf_get(n64_machine *m, n64_perf *out);

/* Deferred DP interrupt (n64_core.c « Deferred DP interrupt »): an RDP back end that can let the GPU finish a SYNC_FULL
 * later (the Vulkan RDP: n64_devices.h bus.defer_dp) asks the core at each SYNC_FULL; the core answers from emulated
 * facts only. When the game shows a picture at most every second VI, each picture had exactly one SYNC_FULL, at least
 * two VI interrupts before the next VI_ORIGIN change (the game did not swap at the first VI after the RDP was done),
 * and that held for the last 3 pictures, a second after reset or a state load, the DP interrupt comes at the next VI
 * interrupt plus offset_pm / 1000 of a VI (default 62: 1/16), and the GPU's work is waited for there, after the host's
 * frame pacing, instead of inside the frame that queued it. Otherwise (a picture every VI, several SYNC_FULLs per
 * picture, a game that swaps as soon as the RDP is done), at once as before. A picture that comes later than before
 * while deferring stops it for 500 VIs. mode: N64_DP_DEFER_OFF, or N64_DP_DEFER_AUTO with offset_pm 1..1000 (0: the
 * default). n64_create takes $RECOMP_N64_RDP_DEFER: "0" (off), "auto" (default), or a percentage of a VI for the
 * offset (1..100). No effect with the software RDP. */
enum { N64_DP_DEFER_OFF = 0, N64_DP_DEFER_AUTO = 1 };
void n64_set_rdp_defer(n64_machine *m, int mode, int offset_pm);
/* $RECOMP_N64_RDP_DEFER as above: the mode, and the offset (per mille) through *offset_pm. NULL / "" / "auto": auto. */
int n64_rdp_defer_parse(const char *s, int *offset_pm);

/* Guest health, for the runtime's diagnostics (no effect on the machine).
 * n64_guest_fault_info: the first exception of a kind a healthy game does not take (TLB refill / invalid / modified,
 * address error, bus error, reserved instruction, coprocessor unusable other than COP1), with the registers at that
 * moment, and how many such exceptions there have been since n64_create (count 0: none).
 * n64_guest_activity: counters of the guest's outside progress since n64_create (VI_ORIGIN writes with a new value,
 * SI and PI DMAs, audio buffers queued); a running game moves them every frame or so. */
typedef struct n64_guest_fault {
    uint64_t count;
    uint32_t code, cause, status; /* ExcCode, Cause and Status as the exception left them */
    uint64_t pc, epc, badvaddr;   /* pc: the vector; epc: the faulting instruction */
    uint64_t frame;               /* n64_frames at that moment */
} n64_guest_fault;
typedef struct n64_activity {
    uint64_t vi_origin_changes, si_dmas, pi_dmas, ai_buffers;
} n64_activity;
const n64_guest_fault *n64_guest_fault_info(n64_machine *m);
void n64_guest_activity(n64_machine *m, n64_activity *out);

#ifdef __cplusplus
}
#endif

#endif
