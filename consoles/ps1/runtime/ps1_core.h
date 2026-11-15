/* PlayStation core (P-4 XBA-162 interpreter, P-5 XBA-163 BIOS, P-6 XBA-164 GTE, P-9 XBA-167 pads / memory cards).
 *
 * An R3000A interpreter (COP0, branch and load delay slots, exceptions) with the GTE (COP2), the PS1 memory map,
 * the interrupt controller, the DMA controller, the three root counters, SIO0 (pads and memory cards) and the
 * scanline / VBlank scheduler. The other chips (GPU, CD-ROM, SPU, MDEC) are reached only through ps1_devices.h. All
 * the state lives in one ps1_machine: no globals, several machines may run side by side (one per thread).
 *
 * BIOS: an image (LLE), OpenBIOS (MIT) built from source by tools/consoles/build_ps1_bios.sh, never a Sony BIOS dump
 * in this repository; or none: the HLE BIOS (ps1_hle.c, XBA-163), the kernel and the A / B / C function tables in C.
 *
 * Boot, LLE: from the reset vector through the BIOS. With a PS-X EXE the BIOS runs its init stage and, where it would
 * enter its shell (0x80030000), the EXE is loaded instead (fast boot). With a disc and no EXE the BIOS boots the disc
 * itself (SYSTEM.CNF) through the CD-ROM controller. HLE: kernel RAM is set up and the EXE (or the disc's SYSTEM.CNF
 * boot file, read from ISO 9660) starts at once. */
#ifndef PS1_CORE_H
#define PS1_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps1_machine ps1_machine;

#define PS1_BIOS_SIZE 0x80000u     /* 512 KB */
#define PS1_RAM_SIZE 0x200000u     /* 2 MB */
#define PS1_CARD_SIZE 0x20000u     /* 128 KB per memory card */
#define PS1_AUDIO_RATE 44100

/* Digital pad buttons, 1 = pressed (the wire protocol is active low; the core inverts). Bit order of the pad's two
 * button bytes. */
enum {
    PS1_PAD_SELECT = 1 << 0,
    PS1_PAD_L3 = 1 << 1,
    PS1_PAD_R3 = 1 << 2,
    PS1_PAD_START = 1 << 3,
    PS1_PAD_UP = 1 << 4,
    PS1_PAD_RIGHT = 1 << 5,
    PS1_PAD_DOWN = 1 << 6,
    PS1_PAD_LEFT = 1 << 7,
    PS1_PAD_L2 = 1 << 8,
    PS1_PAD_R2 = 1 << 9,
    PS1_PAD_L1 = 1 << 10,
    PS1_PAD_R1 = 1 << 11,
    PS1_PAD_TRIANGLE = 1 << 12,
    PS1_PAD_CIRCLE = 1 << 13,
    PS1_PAD_CROSS = 1 << 14,
    PS1_PAD_SQUARE = 1 << 15,
};

/* What is plugged into a controller port. Both DualShock kinds are the same pad (SCPH-1200), plugged in with its
 * ANALOG LED on or off: the game may switch it (config mode, command 0x44) and set up its rumble motors (0x4D), and
 * the player may press its ANALOG button (ps1_pad_analog_button) unless the game locked it. In digital mode it
 * answers like the digital pad (id 0x5A41, L3 / R3 not reported); in analog mode id 0x5A73, buttons, then right X / Y
 * and left X / Y. */
typedef enum ps1_pad_kind {
    PS1_PAD_NONE = 0,      /* empty port */
    PS1_PAD_DIGITAL = 1,   /* SCPH-1080, id 0x5A41, no config mode */
    PS1_PAD_ANALOG = 2,    /* DualShock, plugged in in analog mode */
    PS1_PAD_DUALSHOCK = 3, /* DualShock, plugged in in digital mode */
} ps1_pad_kind;

/* Creates a machine and starts it at the BIOS reset vector.
 *   bios / bios_size: a 512 KB BIOS image (copied); another size: error. NULL: the HLE BIOS (no image).
 *   disc_path: a disc image for the CD-ROM controller (ps1_disc_open: .cue, .bin, .iso, .chd, "fd:N"), or NULL.
 *   exe / exe_size: a PS-X EXE to fast-boot (copied), or NULL: then the BIOS boots the disc (or its shell).
 * Returns NULL and a message in err (may be NULL) on failure. Pad 1 is a digital pad, pad 2 empty; both card slots
 * hold a freshly formatted card. */
ps1_machine *ps1_create(const uint8_t *bios, size_t bios_size, const char *disc_path, const uint8_t *exe,
                        size_t exe_size, char *err, size_t errlen);
void ps1_destroy(ps1_machine *m);

/* Controller port 0 or 1: kind, buttons held (PS1_PAD_*, 1 = pressed) and, for the analog pad, the sticks
 * (0..255, 128 = centre). Called every frame; another kind than the port's is a new pad plugged in (a DualShock's
 * mode, lock and rumble set-up start over), the same kind keeps them. */
void ps1_set_pad(ps1_machine *m, int port, ps1_pad_kind kind, uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx,
                 uint8_t ry);

/* A DualShock on port 0 or 1 (XBA-167): ps1_pad_analog 1 when it reports analog (id 0x73), 0 digital, -1 no DualShock
 * there. ps1_pad_analog_button presses its ANALOG button: the mode flips unless the game locked it (0x44); returns
 * the mode then (-1 no DualShock). */
int ps1_pad_analog(ps1_machine *m, int port);
int ps1_pad_analog_button(ps1_machine *m, int port);

/* The DualShock's rumble motors on port 0 or 1 as the game drives them (the poll's bytes it mapped with 0x4D): small
 * (right, on / off: 0 or 255) and large (left, 0..255). Both 0 without a DualShock or a mapping. */
void ps1_pad_rumble(ps1_machine *m, int port, uint8_t *small, uint8_t *large);

/* Runs until the start of the next VBlank (the VBlank interrupt is raised as it returns). */
void ps1_run_frame(ps1_machine *m);

/* Runs at least `cycles` CPU cycles (tests). */
void ps1_run_cycles(ps1_machine *m, uint64_t cycles);

/* The displayed picture (forwards to the GPU: ps1_gpu_display): 0x00RRGGBB, at most max_w x max_h. Returns 0 and the
 * size, or -1 when the display is off. */
int ps1_display(ps1_machine *m, uint32_t *xrgb, int max_w, int max_h, int *w, int *h);

/* Audio (forwards to the SPU): stereo int16, interleaved L/R, PS1_AUDIO_RATE, produced while ps1_run_frame runs.
 * Copies at most frames sample frames and returns how many. Same thread as ps1_run_frame. */
size_t ps1_audio_pull(ps1_machine *m, int16_t *stereo, size_t frames);

/* Memory cards, slot 0 or 1 (like gba_save): ps1_card_data the 128 KB image (write it to storage as is);
 * ps1_card_take_dirty 1 when the game wrote the card since the last call (clears the flag); ps1_card_load puts a stored
 * image back (size must be PS1_CARD_SIZE; returns 0 or -1); ps1_card_insert(m, slot, 0) pulls a card out, 1 puts it
 * back. */
const uint8_t *ps1_card_data(ps1_machine *m, int slot, size_t *size);
int ps1_card_take_dirty(ps1_machine *m, int slot);
int ps1_card_load(ps1_machine *m, int slot, const uint8_t *data, size_t size);
void ps1_card_insert(ps1_machine *m, int slot, int inserted);

/* Debug / test accessors: memory without side effects (RAM, scratchpad, BIOS; other addresses read 0), CPU registers
 * (r0..r31), PC (the next instruction), HI / LO, COP0 register n. */
uint32_t ps1_peek32(ps1_machine *m, uint32_t addr);
uint16_t ps1_peek16(ps1_machine *m, uint32_t addr);
uint8_t ps1_peek8(ps1_machine *m, uint32_t addr);
void ps1_poke32(ps1_machine *m, uint32_t addr, uint32_t v); /* RAM / scratchpad only */
uint32_t ps1_reg(ps1_machine *m, int r);
uint32_t ps1_pc(ps1_machine *m);
uint32_t ps1_cop0(ps1_machine *m, int r);
void ps1_irq_regs(ps1_machine *m, uint32_t *i_stat, uint32_t *i_mask); /* the interrupt controller, no side effect */
uint64_t ps1_cycles(ps1_machine *m);
uint64_t ps1_frames(ps1_machine *m);
uint8_t *ps1_ram(ps1_machine *m); /* the 2 MB of main RAM, writable */

/* Test hook (XBA-204): on, the machine runs the reference interpreter loop (one full step per instruction) instead of
 * the register-cached one; ps1_run --lockstep-ref runs both and compares them. A host setting, not in save states. */
void ps1_cpu_set_reference(ps1_machine *m, int on);

/* Console text (BIOS putchar / puts, A(3Ch) A(3Eh) B(3Dh) B(3Fh)): each character goes to fn. NULL: dropped. */
void ps1_set_tty(ps1_machine *m, void (*fn)(void *ctx, char c), void *ctx);

/* Fast boot of a disc (XBA-168), set before the first frame: the BIOS's shell (OpenBIOS's logo animation, about five
 * seconds) returns at once instead of running, and the BIOS boots the disc as it would after it (SYSTEM.CNF's TCB /
 * EVENT / STACK, the boot EXE read through the CD-ROM). No effect with an EXE to fast-boot. */
void ps1_set_fast_boot(ps1_machine *m, int on);

/* 1 once the fast boot has put the EXE in place (or immediately when there is none). */
int ps1_exe_started(ps1_machine *m);

/* 1 when the machine runs the HLE BIOS (ps1_create without an image), 0 with a BIOS image. */
int ps1_bios_hle(ps1_machine *m);

/* Multi-disc games (XBA-166). ps1_set_lid opens (1) or closes (0) the CD drive's lid: opening stops the drive and
 * tells the game (INT5), as the console's lid switch. ps1_change_disc puts another disc image in the drive (a path as
 * ps1_create takes; NULL or "": none), closing the previous one: with the lid open only the disc changes (close it
 * afterwards), with the lid closed it is a whole swap at once (open, change, close). Returns 0, or -1 with a message
 * when the image cannot be opened (the drive keeps its disc). */
void ps1_set_lid(ps1_machine *m, int open);
int ps1_change_disc(ps1_machine *m, const char *disc_path, char *err, size_t errlen);

/* Save states (XBA-182): the whole machine as a versioned blob: a header (magic, console, PS1_STATE_VERSION, hashes of
 * the game (the EXE given, else the disc in the drive: its size and volume descriptor) and of the BIOS (0: HLE)), then
 * sections: the CPU / COP0 / GTE / scratchpad / interrupt controller / DMA / timers / video scheduler / SIO with the
 * pads and both memory cards (the machine), main RAM, the GPU (VRAM and state), the CD-ROM controller (its
 * registers, commands in flight, the drive's position and sector buffers, XA / CD-DA), the SPU (state and RAM), the
 * MDEC, the HLE BIOS, the recompiled code's boot flag. Take and load them between frames. A state is the machine
 * only: it loads the same whether the program runs interpreted or with a title pack attached (lifted code is
 * re-checked against RAM after a load). The disc in the drive stays the one there (a state saved with another disc
 * of the game in the drive is refused: change the disc first).
 * ps1_state_size: the bytes ps1_state_save needs now. ps1_state_save: returns the size written, 0 when cap is too
 * small or no state can be taken (PS1_STATE_EBUSY from ps1_state_check_save: an HLE BIOS call into guest code in
 * progress, never the case between frames). ps1_state_check / ps1_state_load: PS1_STATE_OK, or why the state is
 * refused (the machine is left as it was). Loading leaves the cards' dirty flags clear: a card the state brought back
 * reaches storage only once the game writes it again. */
#define PS1_STATE_VERSION 1
enum {
    PS1_STATE_OK = 0,
    PS1_STATE_EGAME = -3,
    PS1_STATE_EVERSION = -4,
    PS1_STATE_EBUSY = -6,
    PS1_STATE_ECORRUPT = -7,
};
size_t ps1_state_size(ps1_machine *m);
size_t ps1_state_save(ps1_machine *m, uint8_t *out, size_t cap);
int ps1_state_check(ps1_machine *m, const uint8_t *data, size_t size);
int ps1_state_load(ps1_machine *m, const uint8_t *data, size_t size);

/* The devices, for tests and the runtime (NULL when absent). */
struct ps1_gpu *ps1_get_gpu(ps1_machine *m);

#ifdef __cplusplus
}
#endif

#endif
