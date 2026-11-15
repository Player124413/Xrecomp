# Other consoles

XRecomp is adding **Game Boy Advance**, **PlayStation** and **Nintendo 64**, statically recompiled the same way as
Xbox discs. This is early work: the layout, build options and test corpus are in place, the runtimes are empty, and no
game for these consoles runs yet. Goal, order, rules (legal, Linux arm64 first), layout, pipeline and milestones are in the Plane spec:
`plane page show -p XBA "Spec · Other consoles"` (epics C0, G, P, N).

| Directory | Console | CPU |
|---|---|---|
| `gba/` | Game Boy Advance | ARM7TDMI (ARM + Thumb) |
| `ps1/` | PlayStation | MIPS R3000A |
| `n64/` | Nintendo 64 | MIPS R4300i (big-endian, 64-bit) |

Each one has:

- `runtime/`: C, CMake target `<name>_runtime`. It links only the host layer `xbr_host` (`src/host`,
  `src/platform/HOST_LAYER.md`) and never the Xbox libraries. GBA, PS1 and N64 have their cores and play games on
  the host layer, see below.
- `tools/`: the Python pipeline (parse → disasm → function discovery → lift → C), package `<name>_tools`. Its
  tests live in the package and run with the tools suite: `python3 -m pytest tools/ consoles/`.
- `tests/`: a self-contained CMake test project. `tools/run_c_tests.sh` runs it as `console_<name>` (exit 77 =
  skipped), and the root build's ctest runs it when the console is ON.

**Title packs (XBA-147).** `pack.json` has an optional `console` field (`xbox` when absent, so every Xbox pack made
before keeps loading, still format 1) and, for `gba` / `ps1` / `n64`, a `title_key` in place of `title_id`. The key is
read from the game's content (`recomp_pack_rom_identify` in `src/platform/recomp_pack.c`, `Console.identify` in the
app): GBA game code + CRC32 of the ROM (`AXVE-1A2B3C4D`), PS1 serial from `SYSTEM.CNF` (`SLUS-00594`, or
`PSX-<CRC32 of the volume descriptor>` without a serial-shaped boot file), N64 game code + header CRC1
(`NSME-635A2BFF`, any byte order). Such a pack lives in `packs/<console>-<key>/`. The app shell's loader
(`android/app/src/main/cpp/pack_loader.c`, `xba_pack_load_rom`) checks it and calls `<console>_runtime_start`
(libmain.so links the three runtimes, which all play games: PS1 XBA-168 and N64 XBA-174 below); the GBA's plays the game, and
needs no pack (`PACK_NONE`, XBA-157); a GBA pack that Prepare built (`python3 -m tools.prepare game.gba`, XBA-158:
docs/android/prepare.md « GBA packs ») makes it run natively, checked and loaded by `runtime/gba_pack.c`
(`PACK_LOADED` / `PACK_STALE`, the game always starts), and the addresses the interpreter had to run are written at
exit for the next Prepare (`gba_runtime_opts.discovered_path`). Import filter (`Console.java`):
`.gba`; `.bin`, `.cue`, `.chd`; `.z64`, `.n64`, `.v64`; and `.iso`, which both an Xbox and a PS1 disc use, so its
content decides (Xbox volume magic `MICROSOFT*XBOX*MEDIA` first, then an ISO 9660 volume with system ID
`PLAYSTATION`). CHD is accepted by name but not read yet (no title key).

**Cores and clocks on Android.** The app shell puts the runtime on the fast cores before it starts
(`recomp_host_cpus_init`, `RECOMP_BIG_CORES`), and every runtime thread inherits them. The threads a frame waits on
(the N64 software RDP's scanline bands, the PS1 GPU's shadow-pass workers) call a thread hook as they start and end
(`n64_rdp_set_thread_hook`, `ps1_gpu_set_thread_hook`; the devices stay free of host code): the runtime's hook,
`recomp_host_worker_hook` (`src/platform/host_perf_hint.h`), has `host_cpus` note them, so after the app comes back
from the background (Home, a notification), when the 5.15 kernel of the AYN Thor has left them on the little cores,
`recomp_lifecycle_resume` puts them back (XBA-50). The frame loop of each runtime (GBA, PS1, N64) is an ADPF
performance hint session (`APerformanceHint`, Android 13+, looked up in libandroid.so at run time; a no-op before and
off Android) together with those workers: its target is the frame period, and each frame reports the time it spent
working, not the sleep to the next deadline, so the governor raises the clocks only as far as the frames need. The
frame loop and the workers also run at a display thread's priority (nice -8). The log says
`perf hint: ADPF session open for N thread(s), target 16.68 ms per frame`. paraLLEl-RDP's own threads (the Vulkan
RDP's command ring worker, fence executor and pipeline compile worker) call the same hook (`RDP::set_thread_hook`, our
patch in `third_party/parallel-rdp`, given the hook of `n64_rdp_set_thread_hook` when the Vulkan RDP is created), so
they are in the session, at nice -8 and put back on the fast cores after a resume like the software RDP's bands.

CMake options `XBR_CONSOLE_GBA`, `XBR_CONSOLE_PS1` and `XBR_CONSOLE_N64` are ON by default on Linux and macOS and OFF
on Android and Windows. A console is turned on for Android once it is ready: the GBA (XBA-157), the PS1 (XBA-168) and
the N64 (XBA-174) are.

**GBA core (XBA-152, XBA-153).** Interpreter first (decision of 2026-11-14, XBA-142); the static lifter (below) is an
acceleration layer. `consoles/gba/runtime/gba_core.h`: an ARM7TDMI interpreter (ARM + Thumb), the memory map, LCD
timing, timers, DMA, interrupts, keypad, and either a BIOS image (LLE: the open-source Cult-of-GBA BIOS built by
`tools/consoles/build_gba_bios.sh`) or a small built-in BIOS of our own (IRQ vector and IntrWait in ARM code, the other
SWIs in C). Direct boot by default. The PPU (`gba_ppu.h`) is a pure line renderer the core calls at the end of each
visible line. The APU (`gba_audio.c`, XBA-155: PSG squares / wave / noise, Direct Sound FIFOs fed by DMA1/2 on
timer overflows, SOUNDBIAS) mixes at 131 kHz and hands out 48 kHz stereo through `gba_audio_pull`. Saves
(`gba_save.c`, XBA-156): the kind comes from the ROM's library ID string (SRAM 32 KB, flash 64 / 128 KB, EEPROM
512 B / 8 KB; none: SRAM from the first write at 0x0E000000), read and written back through `gba_save_data` /
`gba_save_load` / `gba_save_take_dirty`. CMake target `gba_core` is the machine alone (C11 + libm, no SDL),
`gba_runtime` adds the host layer; `gba_run ROM [--result] [--bios PATH] [--ppm OUT] [--wav OUT] [--save PATH]` runs
a ROM headless. Tests: `consoles/gba/tests/CMakeLists.txt` (unit tests, our homebrew HLE and LLE, jsmolka/gba-tests
CPU / memory / BIOS / save suites fetched by `tools/consoles/fetch_gba_tests.sh`).

**Compatibility: the mGBA test suite (XBA-201).** `tools/consoles/build_gba_suite.sh` builds
[mgba-emu/suite](https://github.com/mgba-emu/suite) (MIT, published as source only) at a pinned commit in the devkitARM
image; the ROM is never committed. `consoles/gba/tests/mgba_suite.sh` runs each of its groups from a fresh boot under
`gba_run --pad-script` (press / wait / until-log / hash commands walk its menus) and reads the suite's own
`END: passed/total` line from mGBA's debug-print port (`gba_run --debug-log`, `gba_set_debug_log`); the video group
compares each scene's frame with its reference frame. ctest `gba_mgba_<group>` (built-in BIOS), `gba_mgba_lle_<group>`
(Cult-of-GBA) and `gba_rc_mgba` (recompiled, in lockstep) fail when a count drops below the one reached (the
thresholds in the CMakeLists: raise them with each fix). Counts after XBA-205 (HLE / LLE): memory 1552 / 1488 of 1552,
I/O read 130 of 130, timing 2020 / 1900 of 2020, timer count-up 936 / 847 of 936, timer IRQ 90 of 90, shifter, carry,
DMA, SIO register R/W and multiply long all, BIOS math 615 / 603 of 615, SIO timing 4 of 4, misc 6 of 12, video 6 of
7. XBA-205 brought the cartridge prefetch buffer (`gba_recomp_rt.h` `rc_pf_*`, shared with lifted code), the BIOS's
SWI dispatcher and routine cycle counts in HLE, the immediate DMA's start delay, a DMA's wait on the prefetch
buffer's halfword in flight, the long multiplies' carry flag (bit 31 of the last Booth addend, `rc_mull_carry`) and
timed normal-mode serial transfers, the BIOS's IntrWait loop call for call (BL to its check: the wake's cycles, which
set the prescaler phase of the timer count-up tests) and CpuSet / CpuFastSet refusing a source in the BIOS area and
keeping the save chip's unaligned address on LDM / STM. What is left: the misc edge cases (an H-blank DMA seen by the
CPU's open bus, H-blank flag flips a cycle or two off), video 6 of 7, and through the Cult-of-GBA BIOS (LLE) what its
own code times or checks differently.

**Playing a GBA game (XBA-157).** `gba_runtime_run` (`gba_runtime.h`) plays a ROM on the host layer: `host_window`
(RGB555 → RGBA texture, letterboxed, aspect, integer or stretch scaling, nearest by default or linear: the display
options shared by every console, [environment-variables.md](../docs/runtime/environment-variables.md#consoles-gba-ps1-n64)), every `host_pad` controller in
the Xbox layout (A → A, B → B, Y → A, X → B, shoulders and triggers → L / R, Start, Back → Select, D-pad and left
stick → D-pad), `host_audio` fed from `gba_audio_pull`, pacing at 59.73 Hz with absolute deadlines nudged by the
audio queue, saves in `<save dir>/gba-<KEY>/game.sav` (read at start, written a second after the game's last write,
at pause and at exit), pause and resume with `recomp_lifecycle`. The ROM is a path or `fd:N`. The Android app calls
it through `gba_runtime_start` with no pack (the GBA is ON for Android: [app-shell.md](../docs/android/app-shell.md)
« Game Boy Advance games »). On a desktop:

```sh
cmake -S consoles/gba/tests -B build/gba && cmake --build build/gba     # or the root build (XBR_CONSOLE_GBA ON)
build/gba/gba_runtime/gba_play game.gba          # keyboard: arrows, X = A, Z = B, A = L, S = R, Enter, Backspace
build/gba/gba_runtime/gba_play game.gba --scale integer --save-dir /tmp/saves
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy build/gba/gba_runtime/gba_play game.gba --frames 600 --screenshot last.ppm
xvfb-run -a build/gba/gba_runtime/gba_play gfx.gba --frames 240 --capture 200 present.ppm   # the real present path
```

**GBA static recompilation (XBA-150, XBA-151, XBA-152).** A GBA game's ARM / Thumb code is lifted to C, compiled for
the host into a title pack (`libtitle.so`), and runs natively on the same core: graphics, sound, timers, DMA, IRQs
and saves are the interpreter's code either way. Universal, no per-title configuration; whatever the static pass
misses runs in the interpreter, which records it for the next build (the discovery loop). Contract, frozen between
`gba_tools`, Prepare, the loader and the core (change together and bump `GBA_RECOMP_ABI`):

- `python3 -m gba_tools.recomp ROM OUTDIR [--discovered FILE]` (`consoles/gba/tools`, pure Python; Capstone only
  for disassembly comments) writes `OUTDIR/gba_rc_*.c` + `gba_rc_table.c`, nothing else; each compiles with
  `-I consoles/gba/runtime` alone. `--discovered`: one hex address per line, bit 0 = Thumb, `#` comments.
- The library exports `gba_recomp_title_table` (`gba_recomp.h`): sorted entries (address, Thumb flag, function),
  the ROM's SHA-256. The lifted code's ABI is `gba_recomp_rt.h`: the first members of `gba_machine` (`gba_rc_head`:
  registers, cycle clock, next event, wait-state tables, IRQ line, EWRAM / IWRAM pointers; static asserts in
  `gba_internal.h`) and the bus functions `gba_read8/16/32`, `gba_write8/16/32`, which the loading executable exports
  (`gba_core_exported()`: gba_play, gba_run; `--whole-archive gba_core` in the app's libmain.so).
- `gba_attach_recomp` / `gba_recomp_missed` (`gba_recomp.c`): with a table attached the CPU loop calls lifted code
  whenever the PC is in the cartridge and an entry covers it, the interpreter otherwise; the cartridge addresses
  where the interpreter had to start are the missed set. Loading a pack: `gba_pack.h` (pack.json checks, dlopen).

Lifted code is cycle-exact with the interpreter (same costs, same order, it stops before the same instruction when a
scheduler event or an IRQ is due: one test per block segment, and the interpreter runs the rest of a block an event
falls in), so a recompiled run is the interpreter's run, frame for frame.
`gba_run ROM --pack LIB --lockstep` checks it: an interpreter-only twin runs alongside, the whole state compared
after every frame; `GBA_RC_VERIFY=1` replays every lifted call in the interpreter (debugging). On a desktop:

```sh
tools/consoles/gba_recomp_build.py game.gba build/rc/game --loop 4 --gba-run build/gba/gba_runtime/gba_run --frames 1800
build/gba/gba_runtime/gba_play game.gba --pack build/rc/game/libtitle.so --missed build/rc/game/discovered.txt
build/gba/gba_runtime/gba_run game.gba --pack build/rc/game/libtitle.so --lockstep --stats --frames 600
```

Tests (`consoles/gba/tests`): `gba_rc_*` lift every homebrew and jsmolka ROM, run the discovery loop and play them in
lockstep with the interpreter (HLE and LLE BIOS); `gba_rc_fuzz` does the same with random ARM / Thumb programs under
timer IRQs (`recomp_fuzz.py`); pytest `consoles/gba/tools/gba_tools/test_recomp.py` (discovery on hand-encoded code,
the CLI, Prepare end to end). Design notes: [gba/tools/README.md](gba/tools/README.md).

The ROM tests skip (exit 77) when their ROMs are not built, which is the case in the linux CI job (no Docker inside
it, so no corpus). The homebrew corpus runs in the **homebrew job** instead (XBA-180), after the corpus build, through
`tools/consoles/run_gba_packs.sh` in the linux job's deps image: ctest `gba_hb_*` and `gba_rc_hb_*`, then, with the
inputs from `tools/consoles/gba_test_inputs.sh` (XBA-193: the Cult-of-GBA BIOS built from source and jsmolka/gba-tests,
both MIT, never committed or uploaded, cached in the host's Docker as the data image `xba-gba-inputs:<hash of the
pins>`), `gba_hb_lle_*`, `gba_jsmolka_*` and their `gba_rc_*`, and since XBA-201 the mGBA suite's `gba_mgba*` /
`gba_rc_mgba` (the image holds its ROM too; 59 tests, about 100 s; one that skips fails the job), then `tools/consoles/gba_corpus_packs.py`, the user's chain on each program:
`python3 -m tools.prepare <prog>.gba` (a desktop pack), `gba_run --pack libtitle.so --lockstep --result` until the
verdict, and a screenshot 30 frames later (`--ppm`, kept as PNG in the run's artefact `gba-corpus-screens`). A program
fails on a failing result block, a lockstep difference, or a screenshot whose sha256 is not the one in
`consoles/gba/tests/corpus_screens.sha256`. The GitHub mirror runs the same (`.github/workflows/linux.yml`,
gba-corpus). Locally, and to record new screenshots after an intended change (check the PNGs first):

```sh
tools/consoles/build_homebrew.sh gba build/homebrew && tools/consoles/gba_test_inputs.sh build/gba-inputs
tools/consoles/run_gba_packs.sh build/homebrew build/gba-packs build/gba-inputs
tools/consoles/gba_corpus_packs.py build/homebrew/gba build/gba-packs --gba-run build/gba/gba_runtime/gba_run --update
```

Saves default to `$XDG_DATA_HOME/xrecomp/save` (else `~/.local/share/xrecomp/save`); `--bios PATH` or
`RECOMP_GBA_BIOS` uses a BIOS image of your own; every option is listed at the top of `runtime/tools/gba_play.c`. The
homebrew `pad.gba` (`consoles/gba/tests/homebrew/pad`) shows the keys held and keeps a counter in SRAM: the controls
and saves check on a real host.

**PS1 core (P-4 XBA-162, P-5 XBA-163, P-6 XBA-164, pads / cards of P-9 XBA-167).** Interpreter first, the GBA's
shape. `consoles/ps1/runtime/ps1_core.h`: an R3000A interpreter (`ps1_cpu.c`: every MIPS I instruction, branch and
load delay slots with the cancellation rules, LWL / LWR forwarding, all exceptions, COP0 with SR.IsC cache isolation,
RFE, interrupt entry; a fixed cost per instruction by fetch region, no cache model), the GTE (`ps1_gte.c`, every command,
44-bit MAC checks, FLAG, the UNR divide, register quirks), the memory map with its mirrors (`ps1_bus.c`), the interrupt
controller, DMA (7 channels, DPCR / DICR, sync modes 0 / 1 / 2, OTC; bursts stall the CPU, block and linked-list
transfers complete after their length), the three root counters with every clock source and sync mode, and the
scanline / VBlank scheduler driven by `ps1_gpu_get_timing` (`ps1_io.c`), SIO0 with the digital pad, the DualShock
(digital or analog mode; config mode: 0x43 enter / exit, 0x44 mode + lock, 0x45 status, 0x46 / 0x47 / 0x4C tables,
0x4D rumble mapping, whose motors `ps1_pad_rumble` reports; `ps1_pad_analog_button`) and two 128 KB memory cards (`ps1_sio.c`; `ps1_card_data` / `ps1_card_take_dirty` / `ps1_card_load` like
the GBA saves; JOY_STAT bit 7 is the /ACK level, a pulse of `PS1_SIO_ACK_PULSE` = 100 cycles from the IRQ as on
hardware, not a latch: Duke Nukem - Time to Kill's card handler waits for it to drop with interrupts off; bit 9, the
IRQ, stays until JOY_CTRL bit 4). The other chips are reached through `ps1_devices.h` only: target `ps1_core` links `ps1_gpu`, `ps1_cd`,
`ps1_spu` and `ps1_mdec`, each the real module or its placeholder `ps1_<dev>_stub.c` when the source is absent.

BIOS: **OpenBIOS** (MIT, PCSX-Redux's nugget tree), built from source by `tools/consoles/build_ps1_bios.sh` at the
commit the oracle pins, byte-identical to the oracle's (sha256 `68942dc2…`); never a Sony BIOS. Boot is LLE from the
reset vector; with a PS-X EXE the BIOS runs its init and, where it would enter its shell (0x80030000), the EXE is loaded
(header PC / GP / SP+FP / text / BSS: fast boot); with a disc and no EXE the BIOS boots `SYSTEM.CNF` itself. Or no
image at all: the **HLE BIOS** (P-5 HLE, `runtime/ps1_hle.c`; [ps1/HLE_BIOS.md](ps1/HLE_BIOS.md): how it works and the
190 implemented calls, each with a check). `ps1_create(NULL, ...)`, `ps1_run --bios hle`, `RECOMP_PS1_BIOS=hle`, and
the default of `ps1_run` / `ps1_play` / the app when no image is found. Kernel RAM laid out as the retail BIOS /
OpenBIOS lay it out (vectors, table of tables, A0 / B0 / C0 tables, TCBs, EvCBs), each table entry a trap stub the
interpreter hands to C (packs reach it the same way: kernel RAM is never lifted), the exception handler and its
SysEnqIntRP chains in MIPS like OpenBIOS's (syscalls, root counter / IRQ events, custom exit), events, threads, pads,
`cdrom:` straight from ISO 9660 and `bu00:` / `bu10:` on the card images; the EXE (or the disc's `SYSTEM.CNF` boot
file) starts at once. A call it does not implement returns 0 and is reported once with its number.

`ps1_run [--bios PATH|hle] [--exe X.exe | --disc X.cue] [--frames N] [--result] [--ppm OUT] [--vram OUT] [--wav OUT]
[--tty] [--until-tty TEXT] [--card FILE]` runs a program headless (`--result`: our homebrew's result block at 0x801F0000;
`--vram`: the whole 1024x512 VRAM as a PPM;
`--tty`: the console text, BIOS putchar / puts; `--card`: memory card 1 kept in a 128 KB .mcr file, loaded when
there, written back at exit when the game wrote it). Tests (`consoles/ps1/tests`): `ps1_cpu` and `ps1_gte` unit
tests, our homebrew (`ps1_hb_*`, the same results as PCSX-Redux, checksums included; `mcard` saves through the BIOS
memory card functions: `ps1_mcard` runs it on a blank card, again on the card it saved (the save found and read
back, the card unchanged) and on an unformatted one (formatted by the BIOS first), and checks the card image:
checksums, directory entry, data), JaCzekanski/ps1-tests (MIT, release fetched by
`tools/consoles/fetch_ps1_tests.sh`: cpu/cop, cpu/code-in-io, gte/test-all 1150 / 1150, dma/otc-test, dma/dpcr, timers;
the GPU, DMA and input programs of « PS1 compatibility » below),
and the same under the HLE BIOS (`ps1_hle` unit tests, one per implemented call; `ps1_hle_*`: the homebrew with the
same result block as under OpenBIOS, its discs booted by the HLE BIOS, mcard, ps1-tests, packs in lockstep).
On a desktop:

```sh
tools/consoles/build_ps1_bios.sh && tools/consoles/build_homebrew.sh ps1 && tools/consoles/fetch_ps1_tests.sh
cmake -S consoles/ps1/tests -B build/ps1 -DPS1_CORE_ONLY=ON && cmake --build build/ps1 && (cd build/ps1 && ctest)
build/ps1/ps1_runtime/ps1_run --exe build/homebrew/ps1/gfx.exe --result --ppm gfx.ppm
```

The linux CI job has neither the BIOS, the corpus nor ps1-tests, so those tests skip (77) there. The **homebrew job**
runs them (XBA-200): `tools/consoles/ps1_test_inputs.sh` keeps OpenBIOS and the ps1-tests release in the host's
Docker as the data image `xba-ps1-inputs:<hash of the pins>` (about a second cached; cold, the BIOS build pulls
PCSX-Redux's build image unless `xba-ps1-oracle` is there, and removes it again), then `tools/consoles/run_ps1_tests.sh`
builds `consoles/ps1/tests` in the linux job's deps image and runs its whole ctest (177 tests: `ps1_hb_*`, `ps1_rc_*`,
`ps1_jcz_*` with the GPU VRAM dumps, `ps1_hle_*`, states, rewind, cheats, `ps1_lockref_*` and the unit tests; about
80 s). One that skips fails the job; `PS1_CI_EXCLUDE` (a ctest `-E` regex) can leave slow tests to local runs. The job
also runs on a PR that touches `consoles/ps1/`; GitHub's `ps1-corpus` job does the same. The same locally:
`tools/consoles/build_homebrew.sh ps1 build/homebrew && tools/consoles/ps1_test_inputs.sh && tools/consoles/run_ps1_tests.sh`.

**PS1 static recompilation (P-2 XBA-160, P-3 XBA-161, P-4 XBA-162).** The GBA's model on RAM-loaded code: a PS1
program's MIPS code is lifted to C, compiled for the host into a title pack (`libtitle.so`), and runs natively on the
same core; the BIOS, the devices, DMA, timers and interrupts are the interpreter's code either way. Universal, no
per-title configuration; what the static pass misses runs in the interpreter, which records it, bytes included, for
the next build (the discovery loop). Contract (`ps1_recomp.h`, frozen between `ps1_tools`, Prepare, the loader and
the core; change together and bump `PS1_RECOMP_ABI`):

- `python3 -m ps1_tools.recomp INPUT OUTDIR [--discovered FILE]` (`consoles/ps1/tools`, pure Python, our own MIPS
  decoder) writes `OUTDIR/ps1_rc_NNN.c` + `ps1_rc_table.c`; each compiles with `-I consoles/ps1/runtime` alone. INPUT
  is a disc image (`.cue` / `.bin` / `.iso`: the EXE `SYSTEM.CNF` boots, read from ISO 9660) or a PS-X EXE.
- The library exports `ps1_recomp_title_table`: entries (address, length, **content hash**, function), the EXE's
  SHA-256 and entry point. PS1 code runs from RAM (the EXE, then overlays read over each other), so an entry runs only
  while RAM holds its words: the core keeps a write counter per 1 KB RAM page (CPU and lifted stores, DMA, the EXE
  load), re-hashes an entry only when a page it covers changed, and picks, among entries sharing an address
  (overlays), the one that matches. The lifted code's ABI is `ps1_recomp_rt.h`: the first members of `ps1_machine`
  (`ps1_rc_head`: registers and delay-slot state, COP0, clock and next event, RAM / scratchpad / page counters;
  static asserts in `ps1_recomp.c`) and the core functions `ps1_rc_lw` / `_read` / `_write` / `_swlr` / `_gte_cmd` /
  `_gte_read` / `_gte_write`, which the loading executable exports (`ps1_core_exported()`: ps1_run).
- `ps1_attach_recomp` / `ps1_recomp_missed` / `ps1_recomp_write_missed` (`ps1_recomp.c`, called from the
  `ps1_cpu.c` run loop): main RAM above the BIOS's 64 KB, once the CPU reached the EXE's entry, runs lifted code
  when an entry matches, the interpreter otherwise; the discovered file gets `ADDR HASH HEXBYTES` per miss (a 2 KB
  window of the code as it was), so overlays are lifted on the next round from their own bytes. An address seen
  with 8 contents is code generated at run time: left to the interpreter.

Lifted code is cycle-exact with the interpreter (same fetch and load costs at the same points, the scheduler test
before every instruction, the IRQ line after IO accesses; load delay slots committed early only when the next
instruction cannot tell, else left in flight to the interpreter; branch delay slots inline; exceptions, COP0 and a
branch in a delay slot handed to the interpreter; calls, returns and indirect jumps through the dispatcher, so the C
stack stays bounded). `ps1_run INPUT --pack LIB --lockstep` checks it: an interpreter-only twin alongside, the whole
state (CPU, cycles, scheduler, IRQ / DMA / timer registers, GTE, RAM, scratchpad, VRAM) compared after every frame
(`--lockstep-cycles N`: every N cycles). On a desktop:

```sh
tools/consoles/ps1_recomp_build.py game.cue build/rc/game --loop 4 --ps1-run build/ps1/ps1_runtime/ps1_run \
    --frames 3000 --run-args "--bios build/ps1-bios/openbios.bin"
build/ps1/ps1_runtime/ps1_run game.cue --pack build/rc/game/libtitle.so --lockstep --stats --frames 600
```

Tests (`consoles/ps1/tests`, ctest `ps1_rc_*`): every homebrew (the overlay one from its EXE and from its disc:
both overlays are recorded in round 1, lifted in round 2, and B replacing A at 0x80100000 never runs A's native
code) and ps1-tests (cpu/cop, cpu/code-in-io, gte/test-all, dma/otc-test, dma/dpcr, timers) through the discovery
loop then in lockstep; `ps1_rc_fuzz`: random MIPS code under a timer IRQ every 37-2000 cycles; pytest
`consoles/ps1/tools/ps1_tools/test_recomp.py`. All converge in 2-3 rounds at 99.1-99.9 % native instructions, except
gte/test-all (81.3 %: it writes each GTE command into a stub before running it). Lifted C is cut into blocks with one
scheduler check per segment, RAM accesses inline and anything else handed to the interpreter (XBA-161): about 0.6 s
of NDK clang (aarch64, -O1) per 1000 instructions on the synthetic game-sized program of `ps1/tests/bench` (was 3.5-4
s with a label and a check per instruction; docs/android/prepare.md « PS1 packs »); function bodies are cut at 1000
instructions because compile time grows faster than linearly with them. Design notes:
[ps1/tools/README.md](ps1/tools/README.md).

**PS1 GPU (P-7, XBA-165).** `consoles/ps1/runtime/ps1_gpu.c`, the GPU half of `ps1_devices.h` (CMake target
`ps1_gpu`, plain C11, no host dependency): GP0 / GP1, GPUSTAT / GPUREAD, CPU / VRAM / DMA transfers and a software
rasterizer with the hardware's rules (top-left fill, 1023 x 511 culling, quads as (v0 v1 v2) + (v1 v2 v3), 4 / 8 / 15
bpp textures with CLUT, page and window, the four semi-transparency modes, dithering, mask bit), span loops specialised
per primitive kind; `ps1_gpu_display` crops the display ranges and outputs 15- or 24-bit as XRGB. Commands run when
their last word arrives, so the GPU is never busy. Tests: `consoles/ps1/tests/test_gpu.c` (`PS1_CORE_ONLY=ON` builds
them without SDL2); `ps1_gpu_bench` is the micro benchmark. The gfx.exe homebrew frame matches PCSX-Redux pixel for
pixel.

**PS1 internal resolution (XBA-184).** `ps1_gpu_set_upscale(gpu, 2 | 4)` (`RECOMP_UPSCALE`, `RECOMP_PS1_UPSCALE`,
`ps1_play --upscale`; Android: Settings › Graphics › Internal resolution, a game's own in its Options), Beetle PSX's
software way: the native 1024 x 512 VRAM stays the truth (CPU / DMA reads, GPUREAD, texture and CLUT fetches,
VRAM-to-VRAM copies see exactly what they see at 1x), and a shadow VRAM of 1024N x 512N is drawn as well:
polygons and rectangles rasterized again with their vertices scaled by N (same rules and span loops, drawing area
scaled, textures sampled from the native VRAM; no PGXP, vertices stay on the native grid), lines N times finer and N
pixels thick, fills and CPU writes as N x N blocks, VRAM copies copying the shadow's blocks (an off-screen picture
copied to the display keeps its detail), the mask bit per copy. `ps1_gpu_display` reads the shadow: the picture is
N times wider and higher (up to 2560 x 2048 at 4x, which `host_window` uploads as it is; 24-bit MDEC pictures are
native pixels repeated); given a buffer too small for it (the save state thumbnail at 4x), the native picture. At 1x
there is no shadow and none of this runs: the output is byte-identical to before (the golden hashes did not move).
The shadow is derived, never saved: a state is the native GPU only (its size unchanged, so 0.3.0 states load), and
loading one rebuilds the shadow from the VRAM as N x N blocks, the detail coming back with the next frame drawn.
Cost: the shadow pass is N^2 times the native one, so a primitive covering enough shadow pixels has its rows shared
(interleaved) between the emulation thread and up to three workers (`ps1_gpu_set_threads`, `RECOMP_PS1_GPU_THREADS`;
by default 3 from 8 cores); each pixel is computed as by one thread, so the picture does not depend on the count.
`ps1_gpu_bench [seconds] [factor] [workers]`, x86-64 (Ryzen AI Max+ 395 at 3 GHz), the busy 2000-primitive frame
(about 0.5 Mpixels native, the PS1's fill-rate ceiling): 1x 3.8 ms; 2x 17.8 ms alone, 12.6 ms with 3 workers; 4x
54 ms alone, 22 ms with 3 workers; a big textured triangle's shadow pass scales 3.3 times with 3 workers. Real games
draw a fraction of that frame. Tests (`test_gpu.c`): the native VRAM and GPUREAD at 2x / 4x equal 1x's, golden hashes
of the shadow (gfx.exe replica, busy scene), flat rectangles / fills / sprites (flipped too) / straight lines exactly
the native pixels as blocks, the display, the state round trip, the same shadow with 0 or 3 workers, a 4x fuzz;
`test_main.c`: gfx.exe through the runtime at 1x / 2x / 4x (screenshot sizes and golden CRCs).

**PS1 compatibility: the whole ps1-tests release (XBA-200).** Every program of JaCzekanski/ps1-tests build-158 run
headless under OpenBIOS and the HLE BIOS, checked against what the release ships: the hardware VRAM dumps of the GPU
programs (`ps1_run --vram`, compared at 5 bits per channel by `consoles/ps1/tests/compare_vram.py`), the hardware
console logs (psx.log) of the others. Fixed on the way: polygon colour / UV interpolation at the hardware's precision
(gradients in 1/4096 truncated, stepped from the leftmost vertex plus a half: gpu/triangle, uv-interpolation), line
stepping (32.32, steps rounded away from zero) and dithering of flat lines (gpu/lines), the CLUT cache (kept until the
CLUT position or a larger depth needs a reload, or GP0(01h); gpu/clut-cache), x-flipped rectangles starting at u0 + 1
(gpu/texture-flip), a DMA channel started again while its last transfer's completion is pending (PSn00bSDK's
LoadImage; gpu/texture-overflow), MDEC blocks ending at their 64th coefficient, the decoder's rounding (mdec/4bit
exact; 8bit and frame within one unit) and its raw 8x8 block order on a PIO read, DMA1 with BA = 0 moving the blocks
the MDEC has ready (mdec/4bit and 8bit's release binaries), the HLE printf's `%2-d`. Now
checked by ctest (both BIOSes, skipped without the fetched release): gpu/clipping, clut-cache, lines, quad,
rectangles, texture-flip, texture-overflow, transparency, triangle, uv-interpolation, vram-to-vram-overlap against
their dumps (`ps1_jcz_gpu_*`, `ps1_hle_jcz_gpu_*`), mdec/frame-15bit-dma and frame-15bit within one 5-bit unit
and frame-24bit-dma within 4 per byte (`ps1_jcz_mdec_*`, `compare_vram.py --tolerance 1` / `--bytes 4`; the CPU build waits for output after its very last word, as the
hardware would, so its last column is never uploaded and is left out), gpu/gp0-e1, mask-bit, version-detect,
dma/chain-looping, input/pad and mdec/4bit by their text, on top of the earlier cpu / gte / dma / timers / spu /
cdrom ones. Not matched, by design or
out of reach: the timing measurements (cpu/access-time: no memory wait states; gpu/bandwidth, benchmark's frame
time, dma/chopping and chain-looping's tick counts: the GPU and DMA take no time), cpu/io-access-bitwidth's
odd-width IO writes, the MDEC's last unit of rounding and its FIFO stepping (mdec/step-by-step-log, movie),
timer-dump (needs a modified console); spu/toolbox, ram-sandbox and cdrom/terminal are interactive (spu/stereo
checked by hand: each voice in the right channel).

**PS1 disc, CD-ROM, SPU, MDEC (P-1 XBA-159, P-8 XBA-166, P-9 XBA-167).** The other devices of `ps1_devices.h`, plain
C11 without host dependency (CMake targets `ps1_cd`, `ps1_spu`, `ps1_mdec`). `ps1_disc.c` reads raw sectors and
tracks only (no ISO 9660): `.cue` (several `.bin` files, INDEX 00 / 01, PREGAP, relative paths, names matched ignoring
case), a single `.bin`, `.iso` (2048-byte sectors in a synthesized Mode 2 Form 1 sector), `.chd` through the vendored
libchdr (`third_party/libchdr`, master 47d7941 after v0.3.0, with the crafted-CHD bounds fixes; option `XBR_PS1_CHD`,
ON by default), and `fd:N` for single-file images. `ps1_identify.c` (target `ps1_identify`) makes this reader
`recomp_pack_rom_identify`'s disc hook (`recomp_pack_set_disc_hook`), so a `.chd` gets its title key (SYSTEM.CNF's
serial) where both are linked: libmain, the launcher's `libxba_import.so` (`Ps1Native`, the library scan) and
`ps1_play`; `recomp_pack.c` stays libc-only and leaves a `.chd` unidentified without the hook.
`ps1_cdrom.c` is the controller of psx-spx: banked registers, parameter / response / data FIFOs, INT1..INT5 queued
until acknowledged, first / second response times from psx-spx's measurements and the ps1-tests cdrom/timing hardware
log (seeks: a settling time plus distance, fitted to it), every command up to ReadTOC and GetQ (Test 20h answers vC2),
ReadN / ReadS with 800h / 924h sectors and DMA3, XA-ADPCM (filter, 37800 / 18900 Hz, psx-spx zigzag interpolation to
44.1 kHz) and CD-DA with the volume matrix into a 44.1 kHz stream (at 2x when Setmode says so: double pitch), report
mode with peak levels, sub-channel Q for GetlocP and the reports (pregaps: index 00 of the next track; the lead-out
AAh), the lid (`ps1_cdrom_set_lid`: INT5 01h 08h, stat bit 4 latched until a Getstat; `ps1_cdrom_set_disc` swaps
discs, XBA-166). GetID answers a data
disc as licensed (`SCEx` from the licence text at LBA 4, `SCEA` without one, as a modchip would), so a BIOS boots our
homebrew. `ps1_spu.c`: 24 ADPCM voices with Gaussian interpolation, exact ADSR and sweep envelopes, noise, pitch
modulation, ENDX, reverb (psx-spx formula with its FIR, the hardware's IIR form with the vIIR = -8000h quirk), capture
buffers, the SPU IRQ, FIFO and DMA4 transfers, CD input; KON / KOFF latched to the next sample, a voice starts four
samples after its key on, and the main volume scales the whole mix (voices, CD, reverb). `ps1_mdec.c`: RLE, IDCT with the scale table, 4 / 8 / 15 / 24-bit output, DMA0 / DMA1. Tests:
`consoles/ps1/tests/test_{disc,cdrom,spu,mdec}.c` on images and register streams they generate (a compressed CHD
from `chdman` too when it is installed); JaCzekanski's cdrom/getloc (passes), cdrom/timing (within the hardware log's
ranges) and cdrom/disc-swap on a 74:10 sparse synthetic disc (`run_jcz_cd.sh`); `ps1_hb_chd_overlay_*` boots CHDs
written by `ps1_tools.chd` through libchdr. The limits are listed at the top of each source (the CD-ROM's: its
sector buffers, the head after a Pause, audio sub-channels other than Q).

**Playing a PS1 game (XBA-168).** `ps1_runtime_run` (`consoles/ps1/runtime/ps1_runtime.h`) plays a disc or an EXE
on the host layer, the GBA's way: a disc (`.cue` with its tracks, `.bin`, `.iso`, `.chd`, or `fd:N` for a
single-file image) boots through OpenBIOS from `SYSTEM.CNF`, an EXE (recognised by its `PS-X EXE` magic) is
fast-booted. A disc boots fast too (`ps1_set_fast_boot`, on by default, `RECOMP_PS1_FASTBOOT=0` / `--no-fast-boot`
turn it off): OpenBIOS's shell (its own logo animation, about five seconds; there is no Sony logo) returns at once and
the BIOS boots the disc itself, SYSTEM.CNF's settings included, so nothing is skipped but the animation: our homebrew
discs reach their result in 85-145 frames instead of 390-420. A `.m3u` playlist (multi-disc games, one image per line,
`ps1_runtime_m3u`) plays its first disc; `ps1_runtime_change_disc(path)` (any thread: the app's in-game menu « Change
disc », F6 on the desktop for the playlist's next disc, `ps1_play --swap FRAME:DISC`) opens the lid, and a second later
closes it on the new disc (XBA-166). The picture is the GPU's display area (256 / 320 / 368 / 512 / 640 x 240 / 480, 15- or 24-bit) shown at
4:3 whatever its width (`host_window_set_aspect`), letterboxed, linear by default (`RECOMP_PS1_FILTER=nearest`,
`RECOMP_PS1_SCALE=integer`: whole multiples of the height; or the shared `RECOMP_FILTER` / `RECOMP_SCALE`). Pads, Xbox layout: A → Cross, B → Circle, X → Square,
Y → Triangle, LB / RB → L1 / R1, LT / RT → L2 / R2, Start, Back → Select, stick clicks → L3 / R3, D-pad; port 1 is a
DualShock plugged in in digital mode (the left stick drives the D-pad too) or, with `RECOMP_PS1_PAD=analog`, in
analog mode (both sticks); the game may switch it itself (config mode) and lock it, Back + L3 + R3 while playing is its
ANALOG button, and its rumble motors drive the host pad's (`host_pad_rumble`); every host pad drives port 1, or with `RECOMP_PS1_PLAYERS=2` host pads 1 and
2 drive ports 1 and 2. Sound: `ps1_audio_pull` (44.1 kHz) into `host_audio`, which SDL converts to the device's rate.
Pacing: the frame's length is the CPU cycles the core ran to the next VBlank (NTSC 59.94 / 59.29, PAL 50 Hz, as the
GPU's mode says), absolute deadlines nudged ±2 % by the audio queue. Memory cards: `<save dir>/ps1-<KEY>/card1.mcr`
and `card2.mcr`, 128 KB raw images as other emulators write them (.mcr / .mcd), read before the first frame, written
a second after the game's last write to the card (five at most after its first), at pause and at exit, only when
changed (the `ps1_runtime` test saves with the mcard homebrew: written by the debounce, then reloaded and found by
the game). BIOS: `--bios`, `RECOMP_PS1_BIOS`, the app's asset `ps1-bios/openbios.bin`, else `build/ps1-bios/openbios.bin`
from the working directory or an ancestor of the program. A title pack (`--pack LIB`, XBA-168: `python3 -m
tools.prepare game.cue` builds one, [prepare.md « PS1 packs »](../docs/android/prepare.md#ps1-packs-xba-168)) runs the
lifted code when its `pack.json` and table say it is this disc's (the SHA-256 of its boot EXE) and the game plays
interpreted otherwise; `--missed FILE` keeps what it did not cover for the next Prepare (`consoles/ps1/runtime/ps1_pack.h`). The Android app calls it through `ps1_runtime_start` ([app-shell.md](../docs/android/app-shell.md)
« PlayStation games »); the APK carries OpenBIOS (the `ps1-bios` stage of `tools/docker/android.Dockerfile`, sha256
checked; `LICENSES/MIT-OpenBIOS.txt`). On a desktop:

```sh
cmake -S consoles/ps1/tests -B build/ps1 && cmake --build build/ps1          # or the root build (XBR_CONSOLE_PS1 ON)
build/ps1/ps1_runtime/ps1_play game.cue      # keyboard: arrows, X = Cross, C = Circle, Z = Square, S = Triangle,
                                             # Q / E = L1 / R1, 1 / 3 = L2 / R2, Enter = Start, Backspace = Select
build/ps1/ps1_runtime/ps1_play build/homebrew/ps1/gfx.exe --scale integer --save-dir /tmp/saves
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy build/ps1/ps1_runtime/ps1_play gfx.cue --frames 600 --screenshot last.ppm
xvfb-run -a build/ps1/ps1_runtime/ps1_play gfx.exe --frames 240 --capture 200 present.ppm   # the real present path
```

The title key comes from the disc as the app reads it (`recomp_pack_rom_identify`: `SLUS-00594`, or `PSX-<CRC32>`);
an EXE gets `EXE-<CRC32>`. Every option is listed at the top of `runtime/tools/ps1_play.c`. Test: `ps1_runtime`
(`consoles/ps1/tests/test_main.c`: the helpers and `.m3u` parsing, then gfx.exe fast-booted, through `fd:N`, a memory
card read back and left alone, gfx.cue booted by the BIOS to its 320 x 240 picture, the errors, a save written and
reloaded, a playlist's disc swap, fast boot on and off); `ps1_hb_fastboot_*` boot every homebrew disc fast.
Diagnostic in the log (stdout, logcat on Android; no effect on the game): `ps1: the game has not left pc 0x… for 5 s
with interrupts off (I_STAT …, I_MASK …)` once when, for 300 frame ends in a row, SR.IEc is 0 and the PC stays in one
64-byte window (a hang in a device the loop polls: no IRQ handler can get it out), and `ps1: running again` when it
leaves (`ps1_runtime.c`, `stall_check`).

**N64 core (N-1 XBA-169, N-3 XBA-171, N-4 XBA-172).** Interpreter first, the PS1's shape. `consoles/n64/runtime/n64_core.h`:
a VR4300 interpreter (`n64_cpu.c`: every MIPS III integer instruction, 64-bit ops, LL / SC / LLD / SCD, LWL..SDR,
branch-likely, traps; 32- and 64-bit kernel / supervisor / user address spaces; COP0 with COUNT / COMPARE, every
exception and its EPC / BD / BadVAddr / Context / XContext / EntryHi, the 32-entry TLB with page masks and ASIDs,
refill / XTLB refill / invalid / modified; a fixed cost per instruction by fetch region, 1.5 cycles from cached RDRAM;
no cache model: CACHE does nothing), COP1 (`n64_fpu.c`, on the host FPU: rounding modes, FCSR flags / causes /
enables, the unimplemented operation for denormals, quiet NaNs and out-of-range conversions, FS flushing, the FR = 0 /
1 register views and the half-mode operand rules), the bus (`n64_bus.c`: RDRAM 8 MB big-endian, MI, PI DMA with the
PI's block / misalignment rules and its write latch, RI / RDRAM registers reporting configured memory, SI, the RCP bus
rules for 8 / 16 / 64-bit accesses, MI repeat mode, the IS-Viewer debug buffer), VI and AI (`n64_io.c`: line timing
from H_SYNC / V_SYNC, the interrupt at V_INTR; double-buffered AI DMA at the DAC rate, the DAC's pulls switching
buffers without a gap, resampled to 48 kHz stereo by a 32-tap Kaiser-windowed sinc: SNR above 80 dB for tones up to
14 kHz from a 32 kHz DAC, no click at buffer boundaries, gaps or DAC rate changes), the VI's picture path
(`n64_vi.c`, below), and the PIF and saves (`n64_pif.c`). The RSP and the RDP are reached only through
`n64_devices.h`: target `n64_core` links `n64_rsp` and `n64_rdp`, each the real module or its stub
(`n64_rsp_stub.c`: SP registers and DMA, halts at once; `n64_rdp_stub.c`: walks the commands, SYNC_FULL interrupt).

Boot: the PIF is HLE, never a PIF ROM: the CIC is recognised from the ROM's IPL3 (CRC32 tables for 6101, 6102 / 7101,
6103, 6105, 6106, 7102; libdragon's open IPL3 by its banner; else a 6102), the header and IPL3 go to DMEM, PI domain 1
timing comes from the header, s3..s7 / sp / ra are what IPL3 expects (seed, TV type from the region, cold reset), the
RDRAM is reported configured (RI_SELECT, osMemSize at 0x80000318) so IPL3 takes its warm path, and the CPU starts at
0xA4000040 in the ROM's own IPL3. IMEM holds IPL2's first eight words, as the PIF leaves them (the 6105 / 7105 IPL3
decrypts its RSP program with them). That program spins on SP_SEMAPHORE while the CPU computes the boot checksum,
counting down in RSP $5, and games check what it leaves (Ocarina of Time's rspboot: $4, $6, $11 and $5 within 0.2 % of
the console's count; when the check fails, the game corrupts RDRAM at random and crashes). Our cost model cannot meet
that count, so for this IPL3 only the RSP is not run while it waits for the semaphore, and $5 gets the console's value
once its program ends (`n64_pif.c` header: sources and values). Joybus: controllers (info, buttons + stick), the controller pak (32 KB, data CRC;
a fresh one is formatted like a new .mpk: ID block and copies, index table, empty notes) and the rumble pak, EEPROM
4K / 16K, the RTC (status, control and time blocks on the host's local clock plus the offset the game sets). The CIC
challenge (PIF command bit 1, used by 6105 games) is acknowledged without a response: nothing of ours needs it. Saves: SRAM 32 KB, FlashRAM 128 KB (MX29L1100 ID, erase / program / read
through the PI), EEPROM, the four paks (`n64_save_data` / `n64_save_take_dirty` / `n64_save_load` per kind). The
cartridge's kind is never a per-game table: forced by `n64_options`, else the ED64 / libdragon header byte (`ED` at
0x3C, type in 0x3F), else the first access (a write to the flash command register makes it FlashRAM, any other
domain 2 access SRAM, an EEPROM command EEPROM, 16K once a block above 63 is used). libultra checks EEPROM block
numbers against the type the status command reports, so unless the kind is fixed the status answers like a 16K part
(4K games accept it) and the image stays 512 B until a block above 63 is used. ROMs: z64 / v64 / n64 byte orders
normalised, up to 64 MB, header parsed (`n64_rom_parse`).

`n64_run ROM [--frames N] [--result] [--ppm OUT] [--wav OUT] [--tty] [--pad MASK] [--pal|--ntsc] [--save PATH] [--hash]
[--info] [--regs]` runs a ROM headless (`--result`: our homebrew's block at physical 0x00300000; `--tty`: the IS-Viewer
text). Tests (`consoles/n64/tests`): `n64_cpu` and `n64_io` unit tests, our homebrew (`n64_hb_*`: hello, cpu,
timer_irq; rsp with `n64_rsp.c`, gfx with both modules; the same results as ares), and lemmy-64/n64-systemtest (MIT,
built from source by `tools/consoles/build_n64_systemtest.sh`; `n64_systemtest` passes when every failing test is a
known gap of `systemtest_known.txt`: the caches, reverse-endian user mode, one PI end-of-page quirk). The caches
are left out on purpose: those tests check stale lines (a cached read not seeing an uncached write until an
invalidate, write-back timing), which needs a tag lookup on every cached access in the interpreter and the same in
lifted code (the lockstep must match), for a behaviour games only ever avoid; reverse-endian user mode is used by no
game (only by Unix-style kernels). About 180 frames/s
(190 MIPS) on the x86 dev host for cpu.z64 and gfx.z64. On a desktop:

```sh
tools/consoles/build_homebrew.sh n64 && tools/consoles/build_n64_systemtest.sh
cmake -S consoles/n64/tests -B build/n64 -DN64_CORE_ONLY=ON && cmake --build build/n64 && (cd build/n64 && ctest)
build/n64/n64_runtime/n64_run build/homebrew/n64/gfx.z64 --result --ppm gfx.ppm
build/n64/n64_runtime/n64_run build/n64-systemtest/n64-systemtest.z64 --tty --frames 900
```

The linux CI job has no corpus, so its N64 ROM tests skip (77). The **homebrew job** runs them after the corpus build
(`tools/consoles/run_n64_homebrew.sh`, the linux job's deps image, about 100 s cached; GitHub's `n64-corpus` the
same): ctest `n64_runtime`, `n64_pack`, `n64_hb_*`, `n64_rc_hb_*`, `n64_(rc_)state_*`, `n64_rewind_hb_*` and
`n64_cheats_hb_*` (29 tests; one that skips fails the job). The job also runs on a PR that touches `consoles/n64/`.
Locally: `tools/consoles/build_homebrew.sh n64 build/homebrew && tools/consoles/run_n64_homebrew.sh build/homebrew`.

**The VI's picture (`n64_vi.c`, `n64_display`).** What a television showed, not a crop of the framebuffer: a fixed
canvas of 640 VI pixels from H = 108 (NTSC) / 128 (PAL) and 240 / 288 lines from half-line 34 / 44 (480 / 576
interlaced), the active area placed where H / V_VIDEO put it and black around it (a narrow or short picture keeps
its aspect and place); 320 wide when X_SCALE is 0.5 or less (a 320 x 240 game comes out at its own pixels). The
VI's horizontal guard band is modelled (as paraLLEl-RDP's VI and angrylion do): the first 8 and last 7 VI pixels of
the active area are black unless that edge is clamped to the scan-out, so a standard mode shows canvas columns
4..316 of 320 (OoT draws its fades one column short; a console never shows that column). Per
pixel: X / Y_SCALE steps and their start offsets, bilinear resampling (AA modes 0..2) or replication (mode 3), the AA
filter on partially covered pixels (coverage from the RDP's hidden RDRAM bits, `n64_rdp_hidden_bits`; without them
the pixel's low bit), the divot filter, the dither filter (16-bit), gamma and gamma dither. Interlaced modes are
woven: each field goes to its rows of the canvas (the lower field by the framebuffer position of its first line), so
a 480-line game is shown whole, without combing or bobbing. About 0.4 ms per 320 x 240 picture on the x86 dev host
(1.2 ms with AA on every pixel, the worst case).

**Playing an N64 game (XBA-174).** `n64_runtime_run` (`consoles/n64/runtime/n64_runtime.h`) plays a cartridge on the
host layer, the PS1's way: a ROM file in any byte order (`.z64`, `.v64`, `.n64`, or `fd:N`), no BIOS (the PIF is HLE),
a title pack optional (`--pack LIB`, XBA-174: `python3 -m tools.prepare game.z64` builds one, [prepare.md « N64
packs »](../docs/android/prepare.md#n64-packs-xba-174); it runs the lifted code when its `pack.json` and table say it is
this cartridge's, the SHA-256 of the ROM in big-endian order, and the game plays interpreted otherwise; `--missed FILE`
keeps what it did not cover for the next Prepare, `consoles/n64/runtime/n64_pack.h`). The picture is the VI's
canvas (320 or 640 x 240, 480 interlaced, PAL 288 / 576 lines; `n64_vi.c` above) shown at 4:3 whatever its size,
letterboxed, less the VI's black guard band (8 VI pixels cut from each side, 4 columns of 320, the rest still filling
4:3 as a television's overscan hid them; `RECOMP_N64_CROP_EDGES=0` or `n64_play --edges` shows them), linear by default (`RECOMP_N64_FILTER=nearest`, `RECOMP_N64_SCALE=integer`; or the shared `RECOMP_FILTER` /
`RECOMP_SCALE`). Controller, Xbox layout: left
stick → stick (radial dead zone 8 %, `RECOMP_N64_DEADZONE` in percent; ±80 on an axis at full travel,
`RECOMP_N64_STICK_RANGE`; the diagonals go to about ±68, the real stick's octagonal gate), A → A, B and X → B, right
stick → C buttons (past half its travel), Y → C-up, LB → L, RB → R, LT or RT → Z, Start, D-pad; keyboard on a desktop
(arrows = stick, X = A, C = B, Z = Z, Enter = Start, Q / E = L / R, I / K / J / L = C buttons, T / G / F / H = D-pad).
Every host pad drives controller 1, or with `RECOMP_N64_PLAYERS=2..4` host pad N drives controller N. The accessory
(`RECOMP_N64_PAK`): a controller pak by default, `rumble` a rumble pak whose motor drives the host pad's rumble, `none`.
Sound: `n64_audio_pull` (48 kHz) into `host_audio`. Pacing: the frame's length is the CPU cycles the core ran to the
next VI interrupt (NTSC about 60 Hz, PAL 50 Hz, one field per interrupt when interlaced), absolute deadlines nudged ±2 %
by the audio queue; a line every 10 s gives the frame rate reached (`n64: N fps over the last 10 s`, with the frames
skipped), followed by a performance line (`n64_runtime_perf`; also once at exit over the whole run, `n64: perf over
the run:`), for device reports:

```
n64: perf speed 25 % (15.0 of 60.0 VI/s); cpu 7.9 rsp 4.1 rdp 5.2 (queue 1.0, sync 2.5, scanout 0.5) sched 0.3 vi 1.8
present 0.9 audio 0.1 idle 0.0 other 0.2 ms/frame; worst 41 ms, 212 > 16.7 ms, 30 > 33.4 ms of 150 frames; rsp 1.20 M
instr, rdp 3400 cmds, 2.0 full syncs /frame; pack 83 % native, 3.1 k interp instr/frame, 12 rehashes, 40 slow lookups;
rdp vulkan, upscale 2x, rewind off; core 6 @ 1.90 GHz
n64: perf detail 500 VIs, 167 pictures (16.7/s), 333 unchanged, 0 skipped; 167 VI_ORIGIN changes, 167 scanouts (160
early), 40 full rings; DP 160 deferred (0 early, 0 backoffs), 7 at once, 12 waited 3.5 ms (0.02 ms each); 0 stalled
compiles (0 ms); worst 40 ms: core 30.1 vi 7.2 present 1.8 audio 0.1 other 0.8 (rdp queue 9.0, sync 8.0, deadline
0.2, scanout 3.0)
```

Speed is N64 frames per second against the VI's rate. Times are ms per N64 frame: `n64_run_frame` split into the CPU
(interpreter or pack), the RSP, the RDP and `sched` (the scheduler, VI / AI / PI / SI events) as the core's timed
frames split it (`n64_perf`: one frame in 8 is timed stage by stage, two clock reads per CPU slice; the counts run on
every frame). `rdp` is every `n64_rdp_run`, also those the RSP's DPC_END writes start inside its time slice (which `rsp`
then leaves out, so `rsp` is the RSP's own work): the software RDP draws there, the Vulkan RDP queues its commands to
paraLLEl-RDP, blocks while paraLLEl-RDP's command ring is full (`queue`) and waits at SYNC_FULL. In brackets the RDP's
waits: `queue` that back-pressure (inside `rdp`), `sync` its flushes (the threaded software RDP drawing its batch, the
Vulkan RDP waiting for the GPU and its write-back: at SYNC_FULL inside `rdp`, the VI's inside `vi`), `scanout` the
upscaled scanout's GPU wait and read-back (inside `vi`); then `n64_display`, the RGBA conversion and present, the audio pull and queue, the sleep to the next deadline
(`idle`, frame generation's draws during it included), and `other` what is left (pads, cheats, rewind captures, save
files). The host frames (one shown or skipped picture) are measured without their sleep: the worst one and how many took
longer than one and two VI frames. With a pack: the share of the CPU's instructions lifted code ran, the RDRAM
instructions the interpreter ran, re-hashes (a page holding lifted code written) and dispatch cache misses. Then the RDP
back end (and the software RDP's threads), the internal resolution, rewind, and the host core the loop is on with its
`scaling_cur_freq` (Linux / Android; `core ?` elsewhere). The counters are host-only: not in save states, no effect on
the run (`n64_run --perf N` prints the core's part, run hashes unchanged). The detail line that follows: the VIs (N64
frames) against the pictures made and shown (`n64_display`), the VIs whose picture had not changed and the new
pictures frame skip dropped; the game's buffer swaps (VI_ORIGIN changes), the Vulkan RDP's upscaled scanouts (of
which started early, at the VI_ORIGIN change) and the times its command ring was full; the deferred DP interrupts
(below: SYNC_FULLs deferred, completed early for a save state, backoffs, completed at once; the deadlines where the
GPU was still drawing, the wait at the deadlines and its mean per deferred one); paraLLEl-RDP's "Stalled compile"
pipelines (5 ms or more) and their time; then the worst host frame's own breakdown: `n64_run_frame`, `n64_display`,
present, audio, the rest, and the RDP's waits inside that frame (`deadline`: at a deferred DP interrupt's deadline,
inside `core`).

Deferred DP interrupt (Vulkan RDP; `n64_core.c` « Deferred DP interrupt », `n64_core.h` `n64_set_rdp_defer`). At a
SYNC_FULL the Vulkan RDP hands the work to the GPU and asks the core (`n64_devices.h` `bus.defer_dp`) whether the DP
interrupt may come later; paraLLEl-RDP's command ring holds 1 MB of commands (our patch; 16 KB before, so the
emulation thread used to wait for the GPU while a display list was queued: `queue`). The core answers from emulated
facts only, so the interrupt's moment is the same on every host: when the last 3 pictures each lasted 2 VIs or more,
had exactly one SYNC_FULL, at least 2 VI interrupts before the next VI_ORIGIN change, and their DP interrupt came
before it (Ocarina of Time PAL: SYNC_FULL at 0.7 VI, the swap 2.3 VIs later), a second after reset or a state load,
the interrupt comes at the next VI interrupt plus 1/16 VI: the GPU's work is waited for there (`deadline`), after the
host's frame pacing, at the start of a light frame, instead of inside the frame that queued it. Otherwise (a picture
every VI, several SYNC_FULLs per picture, a game that swaps at the first VI after the RDP is done) at once as before.
A picture that comes later than before while deferring stops it for 500 VIs (`backoffs`). Until the deadline the RDP
keeps its busy bits; `n64_rdp_flush` (a picture to show, a save state) waits for the GPU but leaves the interrupt to
its deadline; a save state completes a pending one first (states never hold one, their format is unchanged); reset
and state loads drop it; a second SYNC_FULL completes the first. With it, the upscaled scanout of a new picture starts
at the VI_ORIGIN change (`n64_rdp_scanout_prefetch`, while nothing is queued in front of it on the GPU) and
`n64_display` takes it unless the registers changed or something was drawn or written where the VI reads (`scanouts
(N early)`). `RECOMP_N64_RDP_DEFER=0` turns both off (`auto` default; a number: the deadline's offset after the VI
interrupt in percent of a VI), `RECOMP_N64_SCANOUT_EARLY=0` the early scanout alone. The software RDP never defers
(`n64_rdp_set_defer_software` / `RECOMP_N64_RDP_DEFER_SOFTWARE=1` route it through the policy, for tests).

Upscaled picture path: the frame loop hands the window the Vulkan RDP's scanout where the GPU copied it
(`n64_display_rgba` / `n64_rdp_scanout_rgba`: RGBA8 in one of two kept, mapped `VIScanoutBuffer`s, the crop as an
offset, the row pitch as it is): one copy (the texture upload) instead of the read-back's copy, RGBA -> XRGB -> RGBA
and the window's copy for screenshots (`host_window_present_rgba_shared`: `host_window_screenshot_bmp` asks the
runtime for the last picture on demand). The window's shaders draw it opaque (the VI's alpha byte is ignored). The
last frame of a bounded run (`--screenshot`), save-state thumbnails (1x) and the 1x / software pictures keep the XRGB
path. `RECOMP_N64_PRESENT_DIRECT=0` goes through the XRGB buffer as before.

Pictures: the frame loop makes, converts and
uploads a picture only for a VI whose picture changed since the last one shown (`n64_display_changed`: the VI
registers, the RDRAM it reads written by the CPU, a DMA or the RDP, rows the Vulkan RDP has queued there, a state load;
interlaced modes always): a game that draws one picture per 2 or 3 VIs (Ocarina of Time PAL) scans out, converts and
uploads once per picture, and the other VIs draw the window's texture again (`host_window_present_again`; nothing
with frame generation, which paces itself). `RECOMP_N64_PRESENT_ALWAYS=1` makes a picture on every VI as before.
Frame skip (`RECOMP_N64_FRAMESKIP`, `n64_play --frameskip`) decides on new pictures only (an unchanged VI is neither
shown nor skipped): `auto` (default) skips a new picture when its frame starts more than half a frame late or while
the audio queue is about to run dry, never two in a row (a skipped picture is still new at the next VI and shown
then); `N` shows one new picture in N + 1; `0` never. With `RECOMP_N64_PRESENT_ALWAYS=1` it decides on every VI,
`auto` at most 3 in a row. A skipped frame still runs whole (CPU, RSP, the RDP drawing into RDRAM, which games
read back) and makes its audio: only `n64_display` and the present are skipped (the software RDP commits its pixels
at the end of every emulated frame, `n64_rdp_frame_end`, shown or not). The software RDP's renderer threads
(`n64_rdp_set_threads`): `RECOMP_N64_RDP_THREADS`, else 2 on Android, else the core's (`XBR_N64_RDP_THREADS`, else
1); they are put back on the fast cores after a resume and share the frame loop's performance hint session (« Cores
and clocks on Android » above). Saves: `<save dir>/n64-<KEY>/game.eep` (EEPROM 4K 512 B / 16K 2 KB), `game.sra` (SRAM
32 KB), `game.fla` (FlashRAM 128 KB), `pak1.mpk`..`pak4.mpk` (controller paks, 32 KB): raw images, the files
mupen64plus and ares use (rename to copy one in). A cartridge save on disk is read before the first frame and also
fixes the save type (an `.eep`'s size picks 4K or 16K unless the header says; a 2 KB `.eep` for a 4K game gives its
first 512 bytes); written a second after the game's last write (five at most after its first), at pause and at exit,
only when changed. `RECOMP_N64_TTY=1` prints the IS-Viewer text. The Android app calls it through `n64_runtime_start`
([app-shell.md](../docs/android/app-shell.md) « Nintendo 64 games »). On a desktop:

```sh
cmake -S consoles/n64/tests -B build/n64 && cmake --build build/n64          # or the root build (XBR_CONSOLE_N64 ON)
build/n64/n64_runtime/n64_play game.z64
build/n64/n64_runtime/n64_play build/homebrew/n64/gfx.z64 --scale integer --pak rumble --rdp-threads 2
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy build/n64/n64_runtime/n64_play gfx.z64 --frames 300 --screenshot last.ppm
xvfb-run -a build/n64/n64_runtime/n64_play gfx.z64 --frames 200 --capture 180 present.ppm   # the real present path
```

The title key comes from the ROM as the app reads it (`recomp_pack_rom_identify`: `NSME-635A2BFF`; the same formula
when it cannot, `n64_runtime_fallback_key`). Every option is listed at the top of `runtime/tools/n64_play.c`. Test:
`n64_runtime` (`consoles/n64/tests/test_main.c`: the helpers, then gfx.z64's picture, through `fd:N` with two RDP
threads and a rumble pak, byte-swapped as `.v64`, an EEPROM and a controller pak read back and left alone, the app
shell's entry, the errors). Diagnostics in the log (stdout, logcat on Android; no effect on the game): the first
guest fault exception of a session (`n64: guest fault at frame N: TLB miss (store) (Cause …), EPC …, BadVAddr …`:
TLB, address and bus errors, reserved instruction, coprocessor unusable other than COP1), and `n64: guest stuck at
frame N (…)` with pc, Status, Cause, EPC, BadVAddr and the DPC registers, once per episode, when for 5 s nothing moves
(no new VI origin, SI / PI DMA or audio buffer: an exception storm, a deadlock) or the picture stays after a fault
(a game's own crash screen); the 10 s frame-rate line also gives the pc. gfx.z64 runs at about 60 fps paced on the x86 dev host with one RDP thread.

**N64 RSP (N-5, XBA-173).** `consoles/n64/runtime/n64_rsp.c`, the RSP half of `n64_devices.h` (CMake target
`n64_rsp`, plain C11, no host dependency): LLE, so every microcode runs as on the hardware (no per-game HLE). Scalar
unit (MIPS subset, delay slots, wrapping DMEM accesses, BREAK / intr-break), COP0 = SP registers (instant DMA with
length / count / skip, status set / clear pairs, semaphore) and the DPC registers of the attached RDP (an END write
runs the RDP at once, so XBUS lists in DMEM are consumed before the microcode reuses them), vector unit (every load /
store with its wraparound rules, the 16 element selectors, every computational op, the reciprocal / inverse square
root ROMs generated from their formulas). Accumulator as bits 47..16 (int32) + 15..0 (uint16): 8-lane 32-bit loops
the compiler vectorises, identical results everywhere. One cycle per instruction. Tests
(`consoles/n64/tests`): `test_rsp.c` (hand-checked vectors, and 93 differential groups of random microcode over every
vector funct, load, store, DMA, status and scalar op whose checksums must equal those the ares oracle computed from the
homebrew ROM `homebrew/rsp`), `test_rsp_rdp.c` (microcode driving the real RDP over XBUS and from RDRAM);
`n64_rsp_bench` (by hand; `-n COUNT` runs a fixed instruction count and prints a DMEM hash): ~139 M RSP
instructions/s on a vector-dense loop on the x86 dev host (was ~97, then ~115 before the predecoded table).

*Predecoded table.* `n64_rsp_run` runs each IMEM word from a predecoded entry (one per word, 16 bytes: the handler
kind, register indices or the vector registers' offsets, the immediate already extended, the branch target and link,
the element selector). An entry keeps the word it was decoded from and the loop compares it with the word it fetches,
decoding again when they differ: no IMEM writer (CPU stores, SP DMA, the HLE boot, save state loads) has to
invalidate anything, and a stale entry cannot run. The table is host only, outside the save state. Vector
computational ops are 128 functions (64 opcodes x selector 0 / 1 or a shuffle), each the NEON or portable body inlined
at a constant opcode, so no opcode switch is left at run time; the aligned LDV / SDV / LQV / SQV run inline in the
loop. Inlining the vector bodies into the loop itself was measured slower (register pressure on both paths). Same
results, cycles (one per instruction) and stop conditions as before: the 93 ares groups, the RSP + RDP tests and
the bench's DMEM hash are identical on x86, arm64 NEON and arm64 portable (qemu-user), as are the run hashes of
every homebrew ROM, both n64-systemtest ROMs, libdragon's test ROM and rsp_inval_instrs (900 frames); `test_rsp.c`
rewrites IMEM after it ran (CPU writes, the microcode's own DMA, a state load, a selector change at one slot).
Callgrind on `n64_rsp_bench -n`: arm64 NEON 68.4 -> 48.8 instructions per RSP instruction, arm64 portable 92.8 ->
75.4, x86-64 123.5 -> 93.5.

*NEON (arm64, XBA-173).* On arm64 the hot vector ops are NEON intrinsics (`vu_op_neon`: the multiply-accumulate family
VMULF..VMADH, VADD / VSUB / VADDC / VSUBC, VLT / VEQ / VNE / VGE, VCL / VCH, VMRG, the logic ops, the divide moves),
every element selector is one table lookup (`tbl`), and the aligned LQV / SQV / LDV / SDV are a load + `rev16`; the
remaining ops run the portable loops (`-DN64_RSP_NO_NEON` forces them everywhere). Portable gains for every host:
vector computational ops dispatched before the scalar decode, selectors 2..7 without the table gather, fixed-count
lane loads / stores. Bit-identical: the 93 ares groups and the RSP + RDP tests pass on x86 and on arm64 (qemu-user),
and a random-microcode differential (vector ops, loads, stores, flags) hashes the same on x86, arm64 NEON and arm64
portable. Measured with callgrind (aarch64, gcc 14 -O3, under qemu-user in the `xba-arm64-deps` image) on
`n64_rsp_bench -n`: 105.7 -> 68.4 arm64 instructions per RSP instruction (-35 %; 92.8 with the portable gains alone);
x86-64 (SSE2): 153.5 -> 136.5.

**N64 RDP: software and Vulkan (N-5, XBA-173).** `n64_rdp` (the RDP half of `n64_devices.h` + `n64_rdp.h`) is
`n64_rdp_select.c`, which picks a back end at `n64_rdp_create` (`n64_rdp_backend.h`). The software RDP is `n64_rdp.c`
(target `n64_rdp_sw`, its public names renamed `n64_rdp_sw_*` by the forced `n64_rdp_sw_names.h`). The Vulkan RDP is
`n64_rdp_vk.cpp` (target `n64_rdp_vk`), the same DPC front end over paraLLEl-RDP (Themaister, MIT, vendored in
[third_party/parallel-rdp](../third_party/parallel-rdp/README.md)): an LLE RDP on Vulkan compute, conformance-tested
bit for bit against angrylion.
- **Building it.** CMake option `XBR_N64_RDP_VULKAN`: ON for Android, OFF elsewhere (`-DXBR_N64_RDP_VULKAN=ON`; it
  needs C++17). Vulkan is dlopened by a private volk (symbols renamed `prdp_*`), so a device without Vulkan still runs.
- **Choosing it.** `n64_rdp_set_backend`, else `RECOMP_N64_RDP` = `vulkan` | `software` | `auto` (the default). `auto`
  means Vulkan on Android when a device with Vulkan 1.1 and 8/16-bit storage is there, software elsewhere. Anything
  that stops the Vulkan RDP from starting falls back to software (logged). The start line names the back end
  (`n64: ... RDP vulkan, threads 1`), and the Vulkan one also logs `n64: Vulkan RDP on <GPU> (paraLLEl-RDP)`.
- **Software RDP threads.** The emulation thread only queues the commands; `n64_rdp_set_threads` renderers on their
  own threads (1 by default, 2 on Android) draw them as they come, each on its rows ((y / 4) % n), so drawing overlaps
  the CPU and RSP work of the same display list and the picture does not depend on the count. Deterministic by
  construction: TMEM loads are done by the front end from RDRAM when the command is dispatched (the TMEM bytes travel
  in the queue); the renderers draw into a mirror of RDRAM and of the hidden bits (the 1 KB blocks a primitive may
  touch copied in by the front end the first time an epoch needs them); the bytes they wrote reach RDRAM only at
  commits, fixed emulated moments: SYNC_FULL (before the DP interrupt), a texture load from pixels drawn since the last
  commit, `n64_rdp_flush` (the VI's `n64_display`, the end of each emulated frame, save states, reset), a full table
  of image layouts. Two images with different row layouts over the same bytes get a barrier (the renderers finish
  what came before). So what the CPU and the RSP read never depends on host timing or thread count; a game sees the
  pixels at SYNC_FULL rather than as each command runs (libultra and libdragon wait for it anyway). Costs: 8 MB of
  mirror plus 4 MB of hidden bits for 8 MB of RDRAM, a dirty bit per byte per renderer (1 MB each). Without threads
  (Windows, or when none can start) the emulation thread runs the renderers at the commits. Tests: every
  `n64_run --run-hash --hash` of the homebrew, n64-systemtest, libdragon's test ROM and rsp_inval_instrs identical
  to the synchronous RDP's for 1, 2 and 4 threads; `n64_rdp_stress` (`test_rdp.c` with renderers stalling at random,
  `N64_RDP_STRESS`, checking after every commit that no renderer wrote outside the tracked bytes); clean under
  ThreadSanitizer. `n64_rdp_bench`'s streamed case (the busy scene fed in 64-word pieces under 15 ms of emulation
  work, x86-64 Ryzen AI Max+ 395): 27.0 ms per frame -> 15.5 ms on 1 thread, 22.1 -> 15.6 on 2; the busy scene
  drawn at once costs about 3 % more on 1 thread (mirror copies, dirty bits), the same on 2 and 4.
- **Coherence.** paraLLEl-RDP gets the core's RDRAM without imported host memory (its "incoherent" path). It uploads
  the pages a render pass reads and writes back, byte-masked, the bytes the GPU wrote, swapping each word between the
  N64 byte order and the GPU's host words (our patch). On SYNC_FULL the CPU waits for the GPU and the write-back,
  then the DP interrupt is raised. `n64_rdp_flush` (called before the VI reads the frame buffer) waits for commands
  queued since then. The colour / Z rows drawn are reported through `bus.wrote` after the wait, as `n64_rdp.c` does.
  `n64_rdp_set_threads` is a no-op there (1).
- **Shaders the driver cannot compile.** At start, `n64_rdp_vk.cpp` draws a few commands into a corner of RDRAM
  (saved and restored): fills of the colour and Z images and a textured rectangle with a Z test. Every pipeline must
  compile, and the pixels must come out right. If not, it tries again without subgroup operations (when the default
  shaders used them), then with paraLLEl-RDP's ubershader. The ubershader is skipped on Qualcomm's proprietary
  driver (Adreno 740 on the AYN Thor): it passes the check there but Ocarina of Time comes out black, without a
  Vulkan error; `RECOMP_N64_RDP_UBERSHADER=1` tries it anyway. When all three fail, the software RDP takes over. The
  start line names what was kept and the driver (`n64: Vulkan RDP on <GPU> (paraLLEl-RDP, default shaders; driver
  <driverName> <driverInfo>, system)`). Our Granite patch
  never compiles a failed pipeline twice, and logs the program and its spec constants once
  ([third_party/parallel-rdp](../third_party/parallel-rdp/README.md)). If a pipeline still fails during the game
  (another frame buffer format, for example), the failure is logged and `n64_rdp_select.c` switches to the software
  RDP when the RDP is idle. The RDP state is lost then, and so is the frame being drawn. `RECOMP_N64_RDP_CHECK=0`
  skips the check. `PARALLEL_RDP_TEST_FAIL_PROGRAM=depth_blend` (or `rasterizer`, `tile_binning_combined`, ...)
  fakes a failing driver, to test the fallbacks on lavapipe.
- **Internal resolution (XBA-185).** `RECOMP_N64_UPSCALE` (else the shared `RECOMP_UPSCALE`) = `1` | `2` | `4` |
  `8`, default 1, read at `n64_rdp_create` (`n64_rdp_set_upscale` overrides). The Vulkan RDP then draws each frame
  buffer pass twice: at 1x into RDRAM as before, so whatever the game reads back is unchanged, and at 2x / 4x / 8x
  into GPU copies of RDRAM (paraLLEl-RDP's upscaling, only for 16 / 32-bit frame buffers at least 256 pixels wide).
  Its own VI scans the upscaled one out (`n64_rdp_scanout`) and `n64_display` returns that picture, read back to the
  CPU: 640 VI pixels x the scale wide, 240 / 288 lines (480 / 576 interlaced, upscaled bob instead of our VI's weave)
  x the scale high, 1280 x 480 at 2x. The runtime shows it at 4:3 and crops the guard band as at 1x
  (`RECOMP_N64_CROP_EDGES`); `--screenshot` and `n64_run --ppm` write it at that size. Costs and limits:
  - the upscaled RDRAM (8 MB x the factor squared) is bound as one storage buffer, so it must fit the device's
    `maxStorageBufferRange` (128 MB on lavapipe and many mobile drivers: 4x at most), and the buffers and the VI's
    images must fit in half the largest device-local heap. The factor is lowered with a log line otherwise
    (`upscale 8x needs a 512 MB storage buffer, ... allows 128 MB`, `upscale lowered from 8x to 4x`);
  - the scanout is halved (filtered, super-sampled) until it is at most 2560 pixels wide, so 8x is read back at 4x;
    `RECOMP_N64_UPSCALE_DOWNSCALE=0..3` sets the halvings;
  - texture rectangles are drawn at 1x by default (2D and HUD art made for 1:1 texels shows seams otherwise):
    `RECOMP_N64_UPSCALE_NATIVE_TEXRECT=0` upscales them; `RECOMP_N64_UPSCALE_NATIVE_LOD=1` computes texture LOD as at
    1x; `RECOMP_N64_UPSCALE_SSREADBACK=1` makes the RDRAM copy the upscaled picture averaged down (not bit-exact);
  - the start-up check runs upscaled: its frame buffer is 256 pixels wide, so the upscaled passes run, and its VI
    scanout must come out at the expected size and colour. A shader try that fails upscaled is made again at 1x
    (`default shaders fail upscaled (2x) on <GPU>, trying 1x`). The start line ends `upscale 2x, scanout 2x`;
  - the software RDP always draws at 1x: `n64: upscale 2x needs the Vulkan RDP, the software RDP draws at 1x`, also
    when the Vulkan RDP cannot start (Qualcomm's own driver on the Thor) or fails during the game.
- **Custom Vulkan driver (Android).** The player can supply a driver, Mesa Turnip for the Adreno, which compiles
  paraLLEl-RDP's shaders where Qualcomm's driver does not. `n64_vk_driver.c` reads the selection the app put in the
  environment (`RECOMP_VK_DRIVER_DIR`, `RECOMP_VK_DRIVER_LIB`, `RECOMP_VK_DRIVER_ID`, `RECOMP_VK_HOOK_DIR`;
  `RECOMP_VK_DRIVER=system` forces the system driver: [environment variables](../docs/runtime/environment-variables.md#custom-vulkan-driver-n64-rdp)),
  dlopens `libxba_vkdriver.so` (libadrenotools, [third_party/adrenotools](../third_party/adrenotools/README.md)) and
  hands its `vkGetInstanceProcAddr` to Granite's `init_loader`. The same shader tries run on that driver. If it cannot
  be opened, makes no device, or fails every try, the system's libvulkan is loaded again and the tries run there:
  custom driver, then system driver, then the software RDP, each step logged. When adrenotools could not load the
  custom library it silently loads Qualcomm's driver; the `driverID` tells, and the log says `custom driver <id> was
  not loaded`. A driver that crashes the process instead: `vk_drivers/trying` holds its id while it is opened and
  checked, and the next start skips it. The start line ends `custom <id>` or `system`. Off Android the open always
  fails (no `libxba_vkdriver.so`), the same path as a driver that cannot be opened. In the app the player picks
  the driver in Settings › Graphics › Vulkan driver (downloaded from GitHub releases or imported as a zip; none
  ships with the app): [app-shell.md « Vulkan drivers »](../docs/android/app-shell.md#vulkan-drivers).
- **Pipeline cache.** With `RECOMP_VK_CACHE_DIR` (the app: `<cache>/vk_cache/<driver id or system>/`), the device's
  VkPipelineCache starts from `pipelines-<pipelineCacheUUID>.bin` there before the check (`pipeline cache loaded (N
  KB)` / `no pipeline cache yet` / `pipeline cache ... ignored (truncated)`), and is written back, to a temporary
  file then renamed, when it grew: after the check, every 10 s (a thread of its own), when the app pauses (sessions
  on Android end killed after a pause) and at destroy (`pipeline cache saved (N KB, <why>, T ms)`). Mesa's own disk
  cache goes to `<dir>/mesa` (`MESA_SHADER_CACHE_DIR`, unless set). Without it every start compiled the same
  pipelines again: ~1.35 s of a 4x start of Ocarina of Time on the Thor's Turnip. `n64_vk_cache.h`;
  [environment variables](../docs/runtime/environment-variables.md#custom-vulkan-driver-n64-rdp).

Tests (`consoles/n64/tests`, built with `-DXBR_N64_RDP_VULKAN=ON`; without a GPU, Mesa's lavapipe with
`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json`):
- `n64_rdp_vulkan` runs `test_rdp.c` on the Vulkan RDP with its own golden hashes (busy scene, gfx frame, mode
  matrix). Seven feature cases are reported, not failed (`vk_known` in `test_rdp.c`), where the two RDPs differ on
  purpose: fill rectangles against the scissor edge, flipped and TLUT / 8-bit copy mode, a 4-bit texture-image load
  (crashes a real RDP; paraLLEl-RDP drops it), the YUV convert, the non-forced blender on fully covered pixels. Six of
  the seven are places where paraLLEl-RDP is the angrylion-accurate one. The seventh is the chroma key, which
  paraLLEl-RDP does not implement and `n64_rdp.c` approximates.
- `n64_rdp_compare` (`n64_rdp_test --compare`) draws the same lists on both and reports the differences per 16-bit
  unit. The gfx.z64 frame differs in 322 of 76,800 pixels, all on the triangle's edges: the software RDP draws
  edge pixels angrylion leaves out. The busy scene differs in 25 % of its pixels, 72 % of those off by one step of a
  5-bit channel (the software RDP's interpolation and filter rounding). The 64 random mode-matrix lists (every mode,
  many invalid on purpose) differ in 0.1 to 11 % of their colour units (one is identical).
- `n64_rdp_vulkan_bad_driver`: `n64_rdp_vulkan` with a custom driver that cannot be opened; the log says why and the
  system driver passes the same tests. `n64_vk_driver` (every build, no Vulkan): the `RECOMP_VK_*` variables, the
  trial marker and `last_start`.
- `n64_hb_gfx` passes with `RECOMP_N64_RDP=vulkan` too.
- Upscaling (XBA-185): `n64_rdp` / `n64_rdp_vulkan` test the option parsing (`RECOMP_N64_UPSCALE` over
  `RECOMP_UPSCALE`) and a 2x scanout of a 320 x 240 frame buffer (1280 x 480, the colours in place; the software RDP
  refuses it). `n64_hb_gfx_upscale` (`run_upscale.sh`) runs gfx.z64 at 1x and 2x: the 2x picture is 1280 x 480 and
  matches the 1x one sampled at its pixel centres (`upscale_compare.py`).
- lavapipe's speed means nothing for a phone.

**N64 static recompilation (N-1 XBA-169, N-2 XBA-170, N-3 XBA-171).** The PS1's model on the VR4300: an N64 game's
code is lifted to C, compiled for the host into a title pack (`libtitle.so`), and runs natively on the same core; IPL3,
the RSP, the RDP, VI / AI / PI / SI, the PIF and interrupts are the interpreter's code either way. Universal: the ROM
alone, no symbol or ELF file (N64Recomp needs a decompilation's ELF), no per-title configuration; what the static
pass misses runs in the interpreter, which records it, bytes included, for the next build (the discovery loop).
Contract (`n64_recomp.h`, frozen between `n64_tools`, Prepare, the loader and the core; change together and bump
`N64_RECOMP_ABI`):

- `python3 -m n64_tools.recomp ROM OUTDIR [--discovered FILE]` (`consoles/n64/tools`, pure Python, our own MIPS III
  decoder) writes `OUTDIR/n64_rc_NNN.c` + `n64_rc_table.c`; each compiles with `-I consoles/n64/runtime` alone. The
  code comes from the boot (the classic boot megabyte at the header's address, or libdragon's ELF in the ROM read
  from its program headers as its IPL3 does), PI DMA copies with constant arguments, and the recorded code; functions
  from a recursive walk (calls, tail calls, jump tables, code pointers, the exception vectors) and a linear sweep of
  the text. Design notes and measurements: [n64/tools/README.md](n64/tools/README.md).
- The library exports `n64_recomp_title_table`: entries (32-bit virtual address, length, **content hash**, function),
  the ROM's SHA-256 and boot PC. N64 code runs from RDRAM (IPL3's copy, PI DMA, overlays, TLB-mapped segments), so an
  entry runs only while memory holds its words: the core keeps a write counter per 4 KB RDRAM page (CPU and lifted
  stores, PI / SI / SP DMA), re-hashes an entry only when a page it covers changed, and picks, among entries sharing
  an address, the one that matches. TLB-mapped entries stay within one 4 KB page: the PC is translated with the
  current TLB / ASID / mode, the bytes are checked at the physical page, the answer is cached until a TLB write or an
  EntryHi write. The lifted code's ABI is `n64_recomp_rt.h`: the first members of `n64_machine` (`n64_rc_head`: time,
  stop, instruction count, RDRAM and its page counters, MI mode, then `n64_rc_cpu`: GPRs, HI / LO, PC / next PC /
  delay slot, COP0, FPRs, FCSR; static asserts in `n64_recomp.c`) and the core functions `n64_rc_read` / `_write` /
  `_read_partial` / `_write_masked` / `_cop1` / `_ctc1`, which the loading executable exports
  (`n64_core_exported()`: n64_run; the app's libmain.so must link `n64_core` whole-archive the same way).
- `n64_attach_recomp` / `n64_recomp_missed` / `n64_recomp_write_missed` (`n64_recomp.c`, called from `n64_cpu_run`):
  RDRAM code in kernel mode, once the CPU reached the boot PC, runs lifted code when an entry matches, the interpreter
  otherwise; the discovered file gets `ADDR HASH HEXBYTES` per miss (a 2 KB window, never past a mapped page). An
  address seen with 8 contents is code generated at run time: left to the interpreter; records of an address that
  agree over their common length are one code (recorded once, merged by the lifter). An entry whose code is not in
  memory does not cut the interpreter's blocks, and pages no entry's code is in run as the interpreter's chained
  blocks (`n64_cpu_run_region`), back to the dispatcher only when execution leaves them, at a branch target not
  recorded yet, or at an event (2026-11-15: the pack's dispatching costs about what the interpreter does). The runtime saves the
  record at pause, every three minutes and at exit.

Lifted code is tick-exact with the interpreter, in the GBA / PS1 recompilers' shape (compile time on the phone):
functions cut into blocks (entry, jump targets, return points, every 12 instructions), only block starts resumable
(inside one, the interpreter runs to its end: `rc_hint`), one scheduler / interrupt check per block (the interpreter
finishes the block when an event lands inside), checked again after every call into the core; the same fetch cost by
segment, the same multiply / divide / FPU / uncached / bus costs at the same points, the instruction count (COP0
Random derives from it), so COUNT = COMPARE and RCP interrupts land in front of the same instruction; cached RDRAM
accesses (and KSEG1 stores) inline, known constant addresses off it (the drivers' RCP registers) through the core,
pointer accesses off it handed to the interpreter with the rest of their block; only the guest registers a function
uses in locals; files cut by a hash of function identity (stable names: a discovery round recompiles about one file
per new function); delay slots inline, branch-likely skipping its slot; COP0 (but CACHE), SYSCALL / BREAK, LL / SC, a branch in a delay slot and any instruction whose exception
condition holds (overflow, trap, faulting access, COP1 unusable) handed to the interpreter with the state in front of
it (exact EPC / BD / BadVAddr); COP1 arithmetic and CTC1 through `n64_fpu.c`, which raises FPE / unimplemented
itself; calls and indirect jumps through the dispatcher (bounded C stack). `n64_run ROM --pack LIB --lockstep` checks
it: an interpreter-only twin alongside, the whole state (GPRs, FPRs, COP0 with COUNT, the TLB, time, instruction
count, scheduler, MI, RDRAM, RSP memory) and the picture compared after every frame. On a desktop:

```sh
tools/consoles/n64_recomp_build.py game.z64 build/rc/game --loop 4 --n64-run build/n64/n64_runtime/n64_run --frames 1800
build/n64/n64_runtime/n64_run game.z64 --pack build/rc/game/libtitle.so --lockstep --stats --frames 600
build/n64/n64_runtime/n64_run game.z64 --pack build/rc/game/libtitle.so --missed build/rc/game/discovered.txt
```

Tests (`consoles/n64/tests`, ctest `n64_rc_*`): every homebrew through the discovery loop then in lockstep, with
the discovery measured against its ELF (`recomp_rom.sh`: at least 99 % of the function symbols found, never an
input); n64-systemtest under `--pack` (the interpreter's pass / known-gap set) and in lockstep
(`recomp_systemtest.sh`); `n64_rc_fuzz`: random MIPS III code (64-bit ops, branch-likely, unaligned accesses, COP1
in both FR modes, overflows / traps / FPE exceptions) under a COUNT = COMPARE interrupt every 40-600 COUNT ticks
(`recomp_fuzz.py`); pytest `consoles/n64/tools/n64_tools/test_recomp.py`. Measured on the x86 dev host (loaded,
12 cores): the homebrew converge in 1-2 rounds, 98.4-99.5 % native instructions once IPL3 is past; n64-systemtest
in 3 rounds at 97.2 % native (rounds 2 and 3 recompiled 5 and 2 of its 237 files), its 3721 tests giving the
interpreter's result. Recompiled vs interpreted: 1075 vs 146 frames/s on n64-systemtest (CPU-bound), 3700-4600 vs
180 on timer_irq / gfx (their idle loop after the verdict), 1190-1215 vs 190 on rsp (RSP-bound); the interpreter
alone is within noise of what it was before the page write tracking. Lifted C costs 0.8-0.9 s of compiler per 1000
instructions with gcc -O1 -fno-tree-slp-vectorize on that host (homebrew: 28-38 k instructions in ~4-5 s with 6 jobs;
n64-systemtest: 407 k instructions in ~77 s with 6 jobs, 1.1 s / 1k).

**GBA / PS1 interpreter speed (XBA-191).** Profiled with callgrind (instruction counts, portable enough to steer arm64
work; qemu timings mean nothing) on the homebrew corpus, the jsmolka / JaCzekanski suites and the synthetic game-sized
programs of `gba/tests/bench` and `ps1/tests/bench` (`build_rom.sh` / `build_exe.sh`, CPU-bound), each run with
`--run-hash` so a change that alters a picture, a sample or a cycle shows. What changed, all byte-identical (run
hashes, lockstep, save-state round trips, a differential fuzz of the PPU against its previous version):
- Both interpreters' step functions, and on the GBA both decoders and the data-processing class, are inlined into
  the run loop (a call per instruction and its callee-saved registers were a fifth of the PS1 interpreter's cost);
  the opcode fetch from the regions code runs from (GBA ROM / IWRAM / BIOS with the `fetch16` / `fetch32` cost tables
  lifted code already uses; PS1 main RAM) is inline, the rest goes through the bus as before.
- PS1 byte / halfword loads and every store to main RAM skip the bus call (same decode, page write counter bumped).
- With a pack, code no pack holds (the BIOS and its interrupt handlers, GBA RAM code) runs in the inlined
  interpreter loop (`gba_cpu_step_outside_rom`, `ps1_cpu_step_unliftable`) instead of a call per instruction from
  the recompiler's loop; on the PS1 bench that code is still most of the frame.
- GBA PPU: layers composed back to front a layer at a time instead of a front-to-back search per pixel; 4bpp text rows
  mirrored once, empty rows skipped; unscaled bitmap rows walked directly. PS1 SPU: no interpolation for an off,
  silent voice.

x86-64 Ryzen AI Max+ 395, gcc -O3, before -> after, instructions per frame: GBA bench 20.3 M -> 13.5 M, gfx.gba 7.7 M
-> 4.9 M, jsmolka arm 5.7 M -> 2.6 M, bench with its pack 9.6 M -> 6.3 M; PS1 bench 45.4 M -> 36.5 M, gfx.exe 53.5 M
-> 42.6 M, bench with its pack 33.9 M -> 26.9 M. Wall clock per 600 frames: GBA 1.3-1.6x faster, PS1 1.06-1.16x
(that wide core hides most of the PS1 interpreter's removed loads and stores; the Cortex-A715 / A710 cores of the
Thor should see more of it). LTO of the cores gains 0-4 % there and is not enabled; the Android build already
compiles them -O3 -mtune=cortex-a715. What is left on the PS1 is the interpreter itself (~80 instructions per guest
instruction: the per-step delay-slot and load-delay bookkeeping through memory), which a register-cached loop or a
predecoded block cache would address.

**PS1 interpreter: predecoded blocks (XBA-204).** `ps1_cpu_run` (and the recompiler's loop for code no pack holds)
now runs predecoded blocks (`ps1_cpu.c` « Predecoded blocks »): straight runs of main RAM or BIOS code decoded once
into ops (operands extracted, branch targets computed), executed by a computed-goto loop with the CPU state in
locals. A load whose next instruction cannot tell lands at once (put back in flight with the register's old value if
that instruction falls back), so the per-instruction delay-slot bookkeeping is gone; a block ends after a branch and
its delay slot, a GTE command, a load the next instruction would see in flight, an instruction the ops don't hold, the
1 KB page end. A block is entered only where the run loop's per-instruction checks are known to pass (no interrupt
pending, the hook address not inside, the clock below the next event: a per-op bound of fetch and load cycles; near
the event only the ops sure to start before it run), so interrupts and events land at the same instruction as
before. Ops that would raise an exception fall back to `step()` from the state in front of them; IO and BIOS-ROM data
accesses go through the bus with the clock where `step()` has it, and stop the block. Blocks are checked against the
RAM page write counters (`ram_gen`, the packs' mechanism) and their words compared again when a page moved; a store
into a running block's own code stops it. Between the run loop's stops every field of the machine is what the old
loop leaves (the interpreter scratch fields too), so save states and frames are byte-identical. `step()`'s own loop
stays as the reference: `ps1_cpu_set_reference`, `ps1_run --lockstep-ref` (twin compared after every frame, with
`--lockstep-cycles N` the CPU every N cycles and the whole machine every 1024 stops), `recomp_fuzz.py --ref`, the
`ps1_lockref_*` ctests. Checked: the whole PS1 suite (x86-64 and aarch64 under qemu), 548 lockstep runs (every
homebrew and every ps1-tests program, OpenBIOS and HLE, per frame and every 1 / 7 / 61 / 997 cycles, the overlay
disc), 450 random-program seeds under timer IRQs, the run hashes below unchanged.

x86-64 Ryzen AI Max+ 395, gcc -O3, before -> after: host instructions per frame (callgrind, 120 frames) bench 33.0 M
-> 17.7 M, gfx.exe 39.0 M -> 19.3 M, cpu.exe 43.5 M -> 20.4 M, gfx.exe HLE 45.0 M -> 21.7 M, bench with its pack
23.3 M -> 15.6 M, gfx.exe with its pack 4.68 M -> 4.41 M. Wall clock per 600 frames (best of 3): hello 2120 -> 1030 ms,
gfx 2099 -> 987, gfx HLE 2206 -> 1005, cpu 2156 -> 988, ps1-tests gpu/benchmark 2792 -> 1265, gpu/triangle 2296 ->
1268, spu/stereo 1974 -> 1164, bench 2428 -> 1466, bench with its pack 1614 -> 1202, gfx with its pack 505 -> 510
(1.7-2.2x interpreted, 1.34x for the bench pack's BIOS share). What remains on the interpreted bench is block entry
and exit (that code branches every few instructions) and the SPU.

**GBA interpreter: predecoded blocks (XBA-208).** Same approach for the ARM7TDMI (`gba_cpu.c` « predecoded blocks »):
`gba_cpu_run` and the recompiler's loop for code no pack holds (the BIOS, routines copied to IWRAM, EWRAM code) run
straight runs of cartridge, IWRAM, EWRAM or BIOS code decoded once into ARM and Thumb ops (operands and condition
extracted, PC-relative values and branch targets computed; LDM / STM / PUSH / POP, halfword transfers and MRS
included, the rare rest are generic ops through the old decoders), executed by computed-goto loops with the flags in
locals; a run goes on across taken branches in the same state and region. Each instruction costs what `step()`
charges, in the same order: the opcode fetch (through the cartridge's prefetch buffer when it is on), wait states,
the non-sequential fetch after data accesses or internal cycles, refills, the IRQ line's 4-cycle delay and the DMA
start delay as the scheduler has them. EWRAM / IWRAM accesses skip the bus call while no DMA is due; the others go
through the bus with r15, the open-bus / BIOS latches and the clock where `step()` has them. An IO register write, a
DMA, a wait-state change or a write to code bumps `blk_attn` and stops the run after that instruction, so IRQs,
halts and events land at the same instruction as before. RAM blocks are invalidated per 256-byte page through a code
map (a bit per halfword some block was decoded from, checked by every bus write and cheat poke): routines copied to
IWRAM and self-modifying code decode again; save state loads, the BIOS's RAM resets and cartridge patches flush
everything. `step()`'s loop stays as the reference: `gba_cpu_set_reference`, `gba_run --lockstep-ref` (twin compared
after every frame on its whole save state, `gba_state_compare`; with `--lockstep-cycles N` both run in slices of N
cycles, `gba_run_cycles`, the CPU compared at every stop and the whole state every 1024), `recomp_fuzz.py --ref`
(`--iwram`: the random program copied to IWRAM before every pass with an immediate patched), the `gba_lockref_*`
ctests. Checked: the whole GBA suite with every input (x86-64, and aarch64 under qemu with the same run hashes), the
mGBA suite counts unchanged group by group (HLE and LLE), 335 lockstep runs (every homebrew HLE / LLE / BIOS boot,
jsmolka's 8 ROMs, the bench, every mGBA suite group; per frame and every 1 / 7 / 61 / 997 cycles), 702 random-program
seeds (cartridge and IWRAM, built-in and Cult-of-GBA BIOS, per frame and every 1 / 7 / 13 / 997 cycles), the corpus
screenshots, the run hashes below unchanged.

x86-64 Ryzen AI Max+ 395, gcc -O3, before -> after: host instructions per frame (callgrind, 120 frames) bench 14.28 M
-> 9.92 M, gfx.gba 4.87 M -> 3.66 M, cpu.gba 2.48 M -> 1.69 M, iwram_copy 2.47 M -> 1.68 M, jsmolka arm 2.56 M ->
1.98 M, thumb 2.55 M -> 1.98 M. CPU time per 3000 frames (best of 5, interpreter only): bench 3.29 -> 2.61 s (1.26x),
jsmolka arm 0.53 -> 0.36 s (1.47x), thumb 0.53 -> 0.37 s (1.43x), gfx 0.75 -> 0.51 s (1.47x), cpu 0.58 -> 0.31 s
(1.87x); mGBA suite groups 1.2-1.6x (memory 77 -> 56 ms, timing 102 -> 75, timers 98 -> 63, dma 118 -> 85). The
bench's cartridge code is bound by the prefetch buffer's model (every opcode fetch goes through it); the blocks
(host memory: up to ~18 MB for a game-sized code footprint, committed as used) are not in save states.

**N64 core speed (XBA-191).** Same method: callgrind on the homebrew corpus, n64-systemtest and their packs
(`n64_run --run-hash --hash`: picture, audio, cycles and RDRAM must not move), `n64_rsp_bench` / `n64_rdp_bench` for
the RCP. The homebrew sit in libdragon's idle loops after their checks, so they measure the interpreter (about 1.04 M
guest instructions per frame) and the per-frame device work. What changed, all byte-identical (run hashes on x86-64
and aarch64, lockstep, save-state and rewind round trips, `--stats`):
- The interpreter's decode / execute and its load / store paths are always_inline in its loops (`N64_HOT_INLINE`):
  ~85 -> ~61 host instructions per guest instruction.
- Packs: the dispatcher's interpreter runs (before the boot PC, and the instructions lifted code hands back while
  they follow in sequence) go through that inlined loop instead of a call per instruction.
- AI resampler: the 32 taps computed in their own loop, the history ring read as two runs (same sums, same order).
- SP DMA: one memcpy and one RDRAM write notice per contiguous run instead of per 8 bytes.

x86-64 Ryzen AI Max+ 395, gcc -O3, before -> after, host instructions per frame: homebrew (hello / cpu / timer_irq /
gfx) 88 M -> 62 M, rsp.z64 80 M -> 57 M, n64-systemtest 99 M -> 73 M, gfx.z64 with its pack 4.0 M -> 3.6 M,
n64-systemtest with its pack 24.7 M -> 21.3 M. Wall clock per 600 frames: interpreter 3.8 s -> 3.5 s (1.08-1.09x),
packs within noise to 1.03x: this core hides most of what was removed (the interpreter's remaining cost is a chain
of stores and loads of pc / npc / now through memory per instruction); the Thor's cores should see more. The RSP
(`n64_rsp.c`, low level: there is no HLE) and the software RDP are unchanged and are what a real game spends its
frame on: on that host `n64_rsp_bench` runs the vector-heavy loop at ~1.8x the RSP's clock with the portable vector
code (the NEON path runs on arm64), and the busy 320x240 RDP scene at ~85 frames/s on one thread (spans and the
bilinear sampler are 80 % of it). VI scanout, the scheduler and the bus are under 1 % of any profile here.

**N64 interpreter: predecoded blocks (XBA-210).** The PS1's model (XBA-204) on the VR4300: `n64_cpu_run` runs RDRAM
code (through KSEG0, KSEG1 or a TLB-mapped 32-bit segment) as blocks decoded once into directly threaded ops
(`n64_cpu.c` « Predecoded blocks »): operands, immediates, branch targets and link values computed, 64-bit registers
in place, delay slots resolved at decode (a likely branch not taken skips its slot with neither fetch cost nor count,
as `step()` does). A block ends after a branch and its delay slot, before an instruction the ops don't hold (TLB
instructions, ERET, LL / SC, traps, SYSCALL / BREAK, COP2 / COP3), at the 4 KB page end or at 64 ops. It is entered
only where `step()`'s loop would run its instructions back to back (kernel mode, no delay slot, no interrupt pending,
MI repeat mode off): per-op tick bounds (fetch by region, MULT / DIV latencies, the uncached data cost, the FPU's
latency) let a block near `stop_at` run only the ops sure to start before it, so VI lines, COUNT = COMPARE, DMA ends
and RSP slices land at the same instruction. Exceptions are found before an op changes anything and `step()` runs that
instruction from the state in front of it (EPC / BD / BadVAddr / the TLB refill vector are its own); COP1 goes
through `n64_fpu_cop1`, MFC0 / MTC0 / CACHE through `cop0_read` / `cop0_write` / `cache_op`, with the clock and the
instruction count `step()` would have (COUNT, RANDOM); a block stops after MTC0, and after a bus access that raised
the interrupt line, moved `stop_at`, switched MI repeat mode on or wrote into its page. Blocks are checked against the
RDRAM page counters (CPU stores, PI / SI / SP DMA, the RSP: the packs' mechanism), mapped blocks also against the TLB
generation (TLB writes, EntryHi / ASID) and Status.ERL; a store into the running block's words stops it; the CACHE
instruction only keeps tags, so it never changes what a block runs. Blocks link to the blocks that ran after them; a
branch to itself over a NOP (an idle loop) fast-forwards to `stop_at`, counting every op that would have started.
Between the run loop's stops every field of the machine is `step()`'s, so run hashes, save states (same format: the
block cache is a host-only tail of `n64_machine`) and packs are unchanged. `step()`'s loop stays as the oracle:
`n64_set_interp_reference`, `n64_run --lockstep-ref` (the twin compared after every frame, its whole save state byte
for byte; with `--lockstep-cycles N`, `n64_cpu_compare` every N cycles and the whole machine every 1024 stops),
`recomp_fuzz.py --ref` (random MIPS III code under fast timer interrupts that also calls subroutines through KSEG1 and
through a TLB page whose two ASIDs map different code, flips its own instructions, swaps code under itself by PI DMA and
toggles Status.IE), the `n64_lockref_*` ctests. Checked: the whole N64 suite (x86-64, and aarch64 under qemu), 153
lockstep runs (every homebrew, both n64-systemtest ROMs, libdragon's test ROM, rsp_inval_instrs: 900 frames, and every
1 / 7 / 61 / 997 cycles from four points of each run) and 500 random-program seeds (every frame, and every 1 / 7 / 61
cycles), no difference; the open test ROMs' pass counts unchanged (3716 / 3726 / 102).

x86-64 Ryzen AI Max+ 395, gcc -O3, before -> blocks without the idle fast-forward -> final: host instructions per frame
(callgrind, 120 frames, no audio pulled) hello 61.2 M -> 25.1 M -> 0.93 M, timer_irq 63.8 M -> 23.9 M -> 12.0 M, gfx
62.7 M -> 24.7 M -> 7.1 M, rsp 57.2 M -> 26.7 M -> 26.7 M, n64-systemtest 73.7 M -> 31.5 M -> 31.5 M, libdragon's test
ROM 57.1 M -> 28.4 M -> 28.4 M (about 1.04 M guest instructions per frame: ~59 host instructions per guest instruction
before, ~24-34 with the blocks, devices included). Wall clock per 600 frames with `--run-hash` (best of 3, on a loaded
host): hello 3.42 -> 1.50 -> 0.51 s, timer_irq 3.76 -> 1.58 -> 0.57, gfx 3.44 -> 1.47 -> 0.61, rsp 2.94 -> 1.17 -> 0.88,
n64-systemtest 4.87 -> 2.40 -> 2.03, libdragon's test ROM 4.54 -> 3.58 -> 3.24 (2.0-2.5x from the blocks where the code
is interpreter-bound, 1.3-1.4x on the test ROM, which spends much of its time in device accesses and short blocks;
the homebrew sit in idle loops, which now cost almost nothing). Packs were unchanged then (the dispatcher's interpreter
runs used `step()`): see the next paragraph.

**N64 packs: the interpreter's blocks for code no entry covers.** Under a pack, RDRAM code no entry matches (code the
last Prepare did not see: overlays relocated to other heap addresses, other contents at an entry's address) used to
run through `step()` one instruction at a time with a dispatch lookup each, 2-3x slower than without a pack since
XBA-210. `n64_recomp_run` now records the miss as before, then runs one predecoded block from that PC
(`n64_cpu_run_block`, XBA-210's runner without its chaining) and comes back to the dispatcher. The block stops in front
of the next entry's start in that page (cached in the dispatch line with the miss), and of a hand-off still pending,
so lifted code is entered at the same instructions as before (the same call counts); a branch that is its last
instruction keeps its delay slot, which `step()` also ran without a lookup. Inside a block execution stays in
sequence, so the discovery record is the same (`--missed` files byte-identical); after it the dispatcher's
« next expected PC » is the one the last step would have left. Recording a miss (a hash of its 2 KB window) is skipped
when it is sure to change nothing: the last call at that PC changed nothing, and neither the record count nor the
window's RDRAM page counters moved since (kept in the dispatch line); before, every taken branch into such code paid
it, an idle loop there at every turn. The handed run (`interp_pc`), delay slots and the
reference loop (`--lockstep-ref`) keep `step()`. Both paths are tick-exact: run hashes, lockstep, save states
unchanged; `--stats` adds the block count (and the slow lookups drop). Tests: `recomp_fuzz.py` now also runs every
seed in lockstep with part of its table (`N64_RC_ENTRIES` 0-0 and 1-1000000: blocks between lifted functions under the
fast timer interrupt). Measured (x86-64 dev host, loaded, n64-systemtest 1200 frames under its pack, before -> after,
same run hash, `--missed` identical; blocks alone, then with the record skip): whole table (97.2 % native) 3.17 ->
3.16 -> 3.15 s; `N64_RC_ENTRIES=0-6000` (8 % of the instructions interpreted, 5519 records) 5.89 -> 5.36 -> 3.55 s;
`N64_RC_ENTRIES=0-2500` (94 % interpreted, the case of a game far from its last Prepare) 15.67 -> 5.14 -> 3.58 s (no
pack: 3.17 s).

**Frame generation (XBA-186).** In a build with `XBOXRECOMP_LSFG` (Android `-PxbaLsfg=1`), under `RECOMP_LSFG=1`, the
host window feeds the LSFG pacer for every console: only distinct pictures reach the generator (a 30 fps game
presents each one twice at 60 Hz), the generated frames take the repeats' slots, and the runtimes' frame sleep goes
through `host_window_sleep_until`, which draws the frames due meanwhile. A game already at 60 distinct pictures is shown
as it comes; fast-forward bypasses it, a state load and a pause start a fresh history; pictures wider than 1280 are
generated at a reduced size. Latency: half the game's frame period plus the generation cost (16.7 ms + c at 30 fps).
Details: [Frame generation › Console window](../docs/technical/framegen.md#console-window).

**Save states and fast-forward (XBA-182, XBA-183).** GBA, PS1 and N64 (the Xbox has none: its state is the whole
guest memory and GPU). Three layers:

- The cores: `gba_state_save` / `gba_state_load` (`gba_core.h`, `gba_state.c`) and `ps1_state_save` / `ps1_state_load`
  (`ps1_core.h`, `ps1_state.c`) put the whole machine in a blob: a header (magic `XRSTATE`, the console, its format
  version `GBA_STATE_VERSION` / `PS1_STATE_VERSION`, an FNV-1a 64 hash of the game: the ROM; the EXE given, else the
  disc in the drive (its size and volume descriptor); and of the BIOS: 0 for the built-in / HLE one) and sections
  (id, size, bytes). GBA: the `gba_machine` whole (CPU and banks, cycle clock and scheduler, IO, timers, DMA, PPU
  registers, VRAM / OAM / palette, EWRAM / IWRAM, the APU and its FIFOs, the save chip and its contents). PS1: the
  machine (CPU, COP0, delay slots, GTE, scratchpad, interrupt controller, DMA, timers, the video scheduler, SIO with
  the pads and both memory cards), main RAM, the GPU (VRAM and its state), the CD-ROM controller (registers, FIFOs,
  commands and responses in flight, the drive's position, sector buffers, XA / CD-DA), the SPU (state and its 512 KB),
  the MDEC (its output not read yet), the HLE BIOS's state, the recompiled code's boot flag. Each device has its
  `ps1_<dev>_state_size / _save / _load` (`ps1_devices.h`; the placeholders keep nothing). A state from another game,
  another BIOS, another format version or another build (a section whose size is not this build's) is refused and
  the machine is left as it was. Host pointers are never in a state, and neither is anything a pack adds: a state is
  the machine only, so it loads the same whether the game runs interpreted or recompiled, either way round. On load
  the GBA's lifted-code hand-offs in flight are reset (its dispatch is a function of the ROM, which never changes);
  the PS1 bumps every RAM page's write counter and flushes its dispatch cache, so each lifted function is hashed
  against RAM again before it runs. Take and load states between frames. Loading clears the save chip's / cards'
  dirty flags: what the state brought back reaches `game.sav` / `card1.mcr` only once the game writes its save again.
  N64 (XBA-183): `n64_state_save` / `n64_state_load` (`n64_core.h`, `n64_state.c`), `N64_STATE_VERSION`, the ROM's
  hash and RDRAM's size (4 or 8 MB) in the header. Sections: the `n64_machine` (the VR4300's GPRs, HI / LO, PC and
  delay slot state, LL bit, COP0 with COUNT / COMPARE and the 32 TLB entries, COP1's registers, FCSR and the FR mode;
  the tick clock and the event scheduler; MI, VI, AI with its two-buffer DMA queue and the resampler's phase and
  history, PI with its latch, SI and PIF RAM, the controllers with their paks, RI; EEPROM, SRAM and the four
  controller paks, the RTC and the FlashRAM's command state), RDRAM (all-zero 4 KB pages left out: 1.5 MB for our
  homebrew, up to 8 MB for a game), the RSP (`n64_rsp_state_*`, `n64_devices.h`: IMEM / DMEM, the scalar registers,
  the vector unit's registers, accumulator, flags and divider, PC, the SP registers and semaphore: the RSP is LLE, so
  there is no HLE state), the RDP (`n64_rdp_state_*`: the DPC registers and the command being collected, then the back
  end's own: the software RDP's modes, combiner, colours, tiles, TMEM, noise seed and hidden RDRAM bits, drawn out
  first whatever its thread count; the Vulkan RDP is synchronised first, so RDRAM holds every pixel, and keeps the
  last of each state command it was given, replayed into paraLLEl-RDP on load, TMEM not kept: games load textures
  again before they use them), FlashRAM's 128 KB when the cartridge has it, the IS-Viewer buffer, the recompiled
  code's boot flag. A state of one RDP back end loads on the other with its DPC registers only (the drawing state
  starts as after a reset: the game sets it again in its next display list). The upscaled copy of RDRAM (XBA-185) is
  derived and never saved: paraLLEl-RDP takes the 1x pixels where RDRAM changed, so what the game draws again is
  upscaled again, a picture it never redraws shows at 1x until then. On load every RDRAM page's write counter is
  bumped and the dispatch cache flushed, as on the PS1; the VI's picture path (`n64_vi.c`) starts afresh (an
  interlaced game's first field after a load is shown alone). The save files are never written by a state: the
  runtime writes the cartridge and paks once the game does.
- The shared interface, `src/platform/recomp_states.h` (in `xbr_host_core`): a runtime attaches itself while it
  plays (console, state folder, a save and a load callback that run under its frame lock), the in-game menu and the
  desktop keys call `recomp_states_save / _load / _info (slot)`. Slots: `<save dir>/<console>-<KEY>/state1.st` ..
  `state4.st`, beside the saves: a header (magic `XRSLOT`, version, console, key, the time saved, the frames run), a
  thumbnail of the last frame (RGB565, at most 160 x 120, at the console's display aspect), then the core's blob;
  written through `.tmp` + rename. About 0.6 MB for the GBA, 4.1 MB for the PS1, 1 to 10 MB for the N64. No save
  dir, no states.
- The automatic slot, `auto.st` (slot `RECOMP_STATE_AUTO` = 0), for « resume where I left off »: with
  `RECOMP_STATE_RESUME=1` the runtime loads it before its first frame (`recomp_states_resume`; an empty slot starts
  the game as usual, a refused one too) and saves it when the player closes the window. On Android the app sets the
  variable and the in-game menu saves the slot when the player leaves (Quit, Back to launcher, the background).
- Quick save / load: on a pad, **Back + right shoulder** saves and **Back + left shoulder** loads the quick slot
  (the last numbered slot saved or loaded this run; at the start, the most recently saved one, else slot 1). Back and
  the shoulders then reach the game as nothing. `recomp_states_last_event` tells the UI what the last quick save /
  load / resume / quick slot change did (the Android menu shows « Saved to slot N », « Quick slot: N »).
- Fast-forward (`recomp_ff_*`): a toggle (the menu's « Fast-forward », or a hotkey) and a hold (Tab on a desktop; on a
  pad, Back + right trigger held, whose two controls then reach the game as nothing); the speed is `RECOMP_FF_SPEED` (2..16
  times the console's rate, default 4) or `max` (as many frames as fit in one display frame, silent). The runtime
  runs `speed` frames per shown frame and averages their audio down by the same factor (higher pitched, never a gap:
  the audio queue keeps its length, so pacing stays audio-driven).

- Hotkeys (XBA-190, `src/platform/recomp_hotkeys.h`, in `xbr_host_core`): the chords above are the defaults of a
  map the player can change. Actions: `ff_hold`, `ff_toggle`, `qsave`, `qload`, `slot_next` / `slot_prev` (the quick
  slot + / - 1, around), `menu` (the Android shell's, `MenuTriggers`), `save1..4` / `load1..4`; each takes a pad chord
  (`RECOMP_HOTKEYS`: a modifier then a button, a button alone, or none) and a desktop key (`RECOMP_HOTKEYS_KEYS`:
  `[ctrl+][alt+][shift+]key`), parsed once in `recomp_hotkeys_get`
  ([environment-variables.md](../docs/runtime/environment-variables.md)). Each frame a runtime hands every physical
  pad's sources (`console_pad_sources`, triggers past half) to `recomp_hotkeys_pad_update` (what is held, what went
  down, the sources to hide from the game) and the SDL keyboard to `recomp_hotkeys_keys_update`, runs what went down
  with `recomp_hotkeys_run` and holds fast-forward while `ff_hold` is held. A two-button chord fires once when both
  are down (either order); a button-alone chord waits while a two-button chord holds its button; the menu chord is
  never fired or swallowed by a runtime (the Android shell opens the menu and the game keeps the buttons). The
  Android app edits them per console and per game ([app-shell.md « Hotkeys »](../docs/android/app-shell.md#hotkeys)).
  No screenshot hotkey: the menu's Screenshot is the shell's (the desktop players have `--screenshot`).

Desktop: `gba_play` / `ps1_play` / `n64_play` by default F1-F4 save slots 1-4, Shift+F1-F4 load them, Tab held
fast-forwards (`RECOMP_HOTKEYS_KEYS` remaps them), the pad chords as above (`RECOMP_HOTKEYS`). Headless, `gba_run` / `ps1_run` / `n64_run --state-save N:FILE` writes a state after frame N,
`--state-load FILE` starts from one (exit 5 when the core refuses it), `--run-hash` prints one hash of every frame's
picture and audio after the save point. Tests: `tests/recomp_states` (slot files, refusals, thumbnails, the attach /
save / load path, the automatic and quick slots, the chords, the events, fast-forward speed and audio; the hotkeys:
both text forms and their round trip, remapped pad chords and keys frame by frame, what they run); the round trip
on every homebrew (`gba_state_hb_*`, `ps1_state_hb_*`, `ps1_state_hle_hb_*`: save after 120 / 150 frames, hash the
next 180 / 150, then the same from the state in a new machine; a run from power-on must differ;
`consoles/{gba,ps1}/tests/state_*.sh`), the same with each program's pack (`gba_rc_state_hb_*`, `ps1_rc_state_hb_*`:
the state of a recompiled run loads in the interpreter and the other way round, same hash, and a recompiled run from a
state stays in lockstep), a disc read in progress (`ps1_state_hb_disc_overlay`), refusals (`gba_state_wrong_game`,
`ps1_state_wrong_game`, `ps1_state_wrong_bios`); the runtimes' tests save and load through `recomp_states.h` while the
game runs on its own thread (`gba_runtime`, `ps1_runtime`). N64: `n64_io` (a machine with something in every part
saved, loaded into a fresh one and compared, the same run after; refusals: version, magic, truncated, another ROM, a 4
MB machine), `n64_state_hb_*` (every homebrew ROM, `consoles/n64/tests/state_rom.sh`; also loaded with 2 and 4 RDP
threads), `n64_rc_state_hb_*` (with each ROM's pack), `n64_state_vulkan_gfx` (the Vulkan RDP's round trip, states
crossing back ends, upscaled 2x: the same machine after the load), `n64_state_wrong_game`, and `n64_runtime` (slots,
the quick slot and fast-forward while gfx.z64 runs, the automatic slot resumed, another game's refused). The Android
in-game menu: [app-shell.md « In-game menu »](../docs/android/app-shell.md#in-game-menu).

`n64_runtime.c` attaches as `"n64"` with `<save dir>/n64-<KEY>`, runs its frames through the same fast-forward loop
(no frame skip while fast-forwarding), reads the hotkeys from every host pad, and
the in-game menu shows the rows for an N64 game (`Console.hasSaveStates`).

**Rewind (XBA-194).** Hold the rewind hotkey (pad: **Back + left trigger**; desktop: **R**, not Backspace, the GBA's
Select; `rewind` in `RECOMP_HOTKEYS` / `_KEYS`) and the game goes back in time, one capture per shown frame, silent;
let go and it plays on from there. `src/platform/recomp_rewind.h` (in `xbr_host_core`):

- The ring: the runtime captures its machine (the save-state blob above; the GBA adds the frame's picture, which its
  state does not hold; the PS1 and N64 pictures come back from VRAM / RDRAM) every `RECOMP_REWIND_FRAMES` frames.
  The newest capture is held whole; each older one is the XOR with the capture after it, coded as runs of zero bytes
  skipped and literal bytes kept (no dependency, a few dozen lines). Stepping back decodes one delta onto the newest,
  so a step costs the same at any depth, and the oldest capture goes for free when the memory cap
  (`RECOMP_REWIND_MB`, default 64; it counts the deltas, the newest capture and the coding buffer) is reached: these
  backward deltas need no keyframe. Captures of different sizes (the N64 leaves zero RDRAM pages out) code the same
  way.
- The runtimes (`recomp_rewinder`): a capture after a console frame when one is due, at most one per shown frame (so
  fast-forward never captures more than once per display frame); while the hotkey is held, no console frame runs,
  the newest capture is loaded (the same post-load path as a slot: queued audio dropped, the save chip / cards / paks
  the state brought back reach storage only once the game writes them) and dropped unless it is the last; the frame
  generation history starts again on each step (`recomp_states_note_load`). The cost is logged at the end of a run
  (`<console>: rewind: N captures, x ms each ...`).
- Defaults: GBA on, a capture every 2 frames; PS1 on, every 3; N64 **off** (every 4 when `RECOMP_REWIND=1`). Capture
  cost measured on our homebrew (x86-64 host under load, `--rewind`): GBA 0.06 ms (639 KB raw, about 5 KB a capture
  held); PS1 0.5 ms (4 MB raw, about 40 KB a capture: 64 MB keep minutes of a static scene, tens of seconds of a busy
  one); N64 1.4 ms on the homebrew, 2.0 ms average with a full 8 MB RDRAM and up to 10 ms peaks, before any Vulkan RDP
  drain (each capture synchronises the GPU): off by default, a game can turn it on (its settings). The N64 software
  RDP's hidden-bit packing in `n64_rdp_state_save_sw` went from 6 ms to under 1 ms for this (the format is unchanged).

Headless: `gba_run` / `ps1_run` / `n64_run --rewind C:R [--rewind-mb M]` (`recomp_rewind_check`): `--frames` frames
with pseudo-random pads, a capture every C frames through the runtimes' `recomp_rewinder`, R steps back; the picture
must be the one recorded after that capture's frame and every frame after it (picture and audio) the first run's;
prints `REWIND OK` with the capture cost (exit 6 on a difference). Tests: `tests/recomp_rewind` (the codec with sizes
that change and corrupt deltas, the ring's round trips, the memory cap and the oldest dropped, the settings, the
capture schedule under fast-forward, the steps, the check on a fake machine, a load that is not exact caught),
`gba_rewind_hb_*`, `gba_rewind_cap` (a 2 MB cap), `ps1_rewind_hb_*` (with a disc read, `overlay`), `ps1_rewind_cap`,
`n64_rewind_hb_*`; the hotkey's default and text form in `tests/recomp_states`.

**Cheats (XBA-197).** GameShark, Action Replay, CodeBreaker and raw codes for the three consoles, from a per-game
list the player edits in the app (in-game menu « Cheats », Game settings › Cheats: [app-shell.md
« Cheats »](../docs/android/app-shell.md#cheats)). `src/platform/recomp_cheats.h` (in `xbr_host_core`, libc only):

- The list is a text file, the app's `files/settings/game/<KEY>.cheats` beside the game's settings (its own file:
  codes are many lines, and « Reset to Settings » leaves them), one section per cheat: `[name]`, `enabled=1|0`, an
  optional `format=`, the code's lines. The runtime reads `RECOMP_CHEATS_FILE` at start and again when the file
  changes (checked every 30 shown frames), so a toggle in the menu takes effect at once; the desktop players take
  `--cheats FILE`.
- Each cheat compiles once into a flat op list (writes with repeat / steps, adds, OR / AND, conditional jumps, pointer
  writes, copies, GBA cartridge patches); one code (even of several lines) is one op, so « the next N codes » is the
  next N ops, and Action Replay v3 if / else / endif blocks become jumps. The runtime runs the enabled lists after
  every console frame under its lock. Writes go through the core's side-effect-free accessors (`gba_cheat_poke`: RAM
  directly, I/O registers through their write handlers; `ps1_poke32` / `n64_poke32` with read-modify-write for 8 /
  16-bit codes) and **only when the value differs**: a constant write costs a read per frame, and a code page is not
  invalidated for nothing.
- Recompiled code: packs read RAM through the bus, so RAM codes need nothing. A write that changes code a PS1 / N64
  pack runs from RAM bumps that page's write counter (`PS1_RAM_TOUCH`, `n64_rdram_wrote`): the pack's content check
  drops the lifted function and the interpreter runs the patched code (the existing self-modifying-code path). GBA
  ROM patch codes (GS `6`, AR v3 `00000000 18..1E`) replace cartridge words (`gba_rom_patch16`; the ROM's hash for
  save states is taken first, so states stay the game's); lifted code is the ROM's instructions compiled to C (and
  may fold its literal pool), so while any patch is in place the runtime detaches the pack's table and the
  interpreter runs the whole cartridge, and attaches it again once none is (`gba_cheats.h`, logged). Patches are put
  back when the cheat goes off and at exit.
- Save states and rewind keep working: cheats are not part of a state; after a load they apply again from the next
  frame.

| Console | Formats (`format=`) | Code types |
|---|---|---|
| GBA | `raw` | `AAAAAAAA:VV` / `:VVVV` / `:VVVVVVVV` (or a space): constant write, size from the value's digits |
| GBA | `gsa` (encrypted), `gsa-raw` | GameShark / Action Replay v1-v2: `0` / `1` / `2` 8 / 16 / 32-bit writes, `3000` group write, `3010`-`3040` add / subtract, `6` ROM patch, `8x1` / `8x2` button writes (once, when turned on), `D` 16-bit if (==, !=, <=, >=) for the next code, `E0` for the next zz codes |
| GBA | `arv3` (encrypted), `arv3-raw` | Action Replay v3 / GameShark v3: `00` / `02` / `04` writes and fills, `40` / `42` / `44` pointer writes, `80` / `82` / `84` adds, `C6` / `C7` I/O writes, every conditional (==, !=, signed / unsigned <, >, AND, never) for 1 code, 2 codes, a block with else / endif, or the rest; `00000000` specials: end, button writes (once), ROM patches 1-4, slides |
| GBA | `cb` | CodeBreaker, unencrypted: `2` OR, `3` / `8` writes, `4` slide, `5` super code, `6` AND, `7` / `A` / `B` / `C` / `F` conditionals, `D` keys held / not held, `E` add |
| PS1 | `gs` | GameShark: `30` / `80` writes, `10` / `11` / `20` / `21` increment / decrement, `D0`-`D3` (16-bit ==, !=, <, >) and `E0`-`E3` (8-bit) for the next code, `50` serial repeater, `C0` master activator, `C1` delay (YYYY / 3 frames), `C2` copy |
| N64 | `gs` | GameShark: `80` / `81` (and `A0` / `A1`) writes, `88` / `89` (the GameShark button: once, when turned on), `D0`-`D3` (8 / 16-bit ==, !=), `50` serial repeater, `F0` / `F1` boot-time writes (once, after the first frame) |

`auto` (the default): `raw` when a line has `:`; on the GBA `cb` for `AAAAAAAA VVVV` lines, else the reading
(encrypted or decrypted GameShark v1-v2 or Action Replay v3) whose lines decode to the most plausible codes (real RAM
/ I/O addresses, ID codes); `gs` on the PS1 and N64. The GBA encryption is the devices' TEA variant (32 rounds, delta
0x9E3779B9) with their default seeds (GS v1-v2 `09F4FBBD 9681884A 352027E9 F3DEE5A7`, AR v3 `7AA9648F 7FAE6994
C0EFAAD5 42712C57`, both derived from the devices' DEADFACE tables with parameter 0).

Not supported (the cheat is not applied, the log says why): GBA `DEADFACE` reseeding, encrypted CodeBreaker codes
(`9xxxxxxx` first line: use the decrypted ones), AR v3 slowdown; PS1 joker codes `D4`-`D6`; N64 `DE` (entry point,
which only matters to a real cartridge's boot). Ignored with a note: master / hook / ID codes (GBA `F`, `C4`, CB
`0` / `1`, `xxxxxxxx 001DC0DE`), N64 enabler codes (`EE`, `FF`, `CC`). F0 / F1 apply once after the first frame (the
game's boot code has run by then; a real GameShark writes them before).

Headless: `gba_run` / `ps1_run` / `n64_run --cheats FILE [--peek ADDR]...` (with `--lockstep`, on both machines).
Tests: `tests/recomp_cheats` (every code type on a fake memory, the encryption's round trips and fixed vectors, the
format detection, blocks, once-codes, cartridge patches in and back, writes only on a change, the file and its
reload), `gba_cheats_hb_hello` (raw, encrypted GS v1-v2 and AR v3, CodeBreaker, a cartridge patch, a cheat off, on
our `hello`; `consoles/gba/tests/cheats/`), `gba_rc_cheats_hb_hello` (RAM codes on the pack in lockstep) and
`gba_rc_cheats_hb_hello_rompatch` (the pack set aside), `ps1_cheats_hb_hello`, `ps1_hle_rc_cheats_hb_hello`,
`n64_cheats_hb_hello`; the app's `CheatsTest` and `GameMenuTest.cheatsPage`. No commercial cheat database is in the
repository: the codes are the player's.

**Legal**: no ROM, disc image, BIOS or game data in this repository, its CI or its artifacts. Tests use homebrew we
build ourselves (devkitARM / libtonc, PSn00bSDK, libdragon) and dumps Alex made from his own media, kept outside every
repository.
