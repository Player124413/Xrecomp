# n64_tools: static recompilation of N64 games

`n64_tools` turns an N64 ROM into the C sources of a title pack (Plane N-1 XBA-169, N-2 XBA-170, N-3 XBA-171). It is
pure Python (it runs in the Android app's embedded CPython for Prepare) and needs the ROM alone: no symbol file, no
ELF, no per-title configuration. Unlike N64Recomp, which lifts from a decompilation's ELF, it finds the code itself
and lets the runtime record what it missed (the discovery loop). The contract with the runtime is
[`consoles/n64/runtime/n64_recomp.h`](../runtime/n64_recomp.h) (pack table, discovered file) and
[`n64_recomp_rt.h`](../runtime/n64_recomp_rt.h) (the lifted code's ABI).

```sh
PYTHONPATH=consoles/n64/tools python3 -m n64_tools.recomp game.z64 out/src [--discovered out/discovered.txt]
tools/consoles/n64_recomp_build.py game.z64 build/rc/game --loop 4 --n64-run build/n64/n64_runtime/n64_run --frames 1800
build/n64/n64_runtime/n64_run game.z64 --pack build/rc/game/libtitle.so --lockstep --stats --frames 600
PYTHONPATH=consoles/n64/tools python3 -m n64_tools.coverage hello.z64 hello.elf   # homebrew only: measures discovery
```

| Module | What |
|---|---|
| `rom.py` | byte orders (z64 / v64 / n64), header, CIC from the IPL3 CRC (as `n64_rom.c`), the boot's segments |
| `isa.py` | our MIPS III / VR4300 decoder (COP1 included), classes and register use for discovery and lifting |
| `discover.py` | function discovery over images (static segments, recorded RDRAM code) |
| `dma.py` | code segments copied by PI DMA with constant arguments |
| `lift.py` | one C function per discovered function (`n64_recomp_rt.h` macros) |
| `recomp.py` | the CLI: `n64_rc_AAAAAAAA[_HHHHHHHH].c` (stable files, below) + `n64_rc_table.c` |
| `coverage.py` | test tool: the share of an ELF's function symbols the discovery found (never an input) |

## Finding the code the game runs

**Boot.** Two boots exist. A libultra game's IPL3 copies the first megabyte after the header area (ROM
0x1000..0x101000) to the header's boot address (minus 0x100000 for a 6103 CIC, 0x200000 for a 6106) and jumps there.
libdragon's open IPL3 finds the program's ELF stored in the ROM (at a 256-byte boundary after IPL3, uncompressed with
`N64_ROM_ELFCOMPRESS=0`), copies its PT_LOAD segments and jumps to the ELF entry; `rom.py` reads the same ELF and
program headers, never section headers or symbols (libdragon strips them from the ROM's copy anyway). The boot PC
goes into the table: lifted code runs, and missed code is recorded, only once the CPU got there, so IPL3 (Nintendo's,
in the user's ROM, or libdragon's stage 2 at the top of RDRAM) is never lifted.

**PI DMA.** libultra copies more segments with `osPiStartDma` (through the PI manager thread), games with wrappers
(`dma_read(dst, romStart, romEnd)`), libdragon with `dma_read(dst, pi, len)`; the arguments are usually link-time
constants. Without symbols the DMA routine is unknown, so `dma.py` looks at every call site: constant arguments
(a0..a3, and sp+0x10..0x1C stored right before the call) that read as an RDRAM destination, a ROM source (offset or
cartridge address) and a length or source end make a candidate segment, kept when it decodes as code. A wrong
candidate costs compile time only (an entry runs only when its bytes are in memory); segments copied with computed
arguments (segment tables, compressed code) are found by the discovery loop.

**Functions.** Per image, `discover.py`:

1. seeds: the boot PC, the exception vectors 0x80000000 / 0x80000080 / 0x80000100 / 0x80000180 when an image
   holds them (libdragon loads its vectors there; libultra copies `__osExceptionPreamble` there at run time, which
   the discovery loop records), the discovered file's addresses;
2. a recursive walk: JAL / BAL targets, tail-call J targets (a J to the code right after its own slot and padding
   is a tail call too), LUI + ADDIU / ORI / DADDIU addresses that look like function starts, jump tables (LW + JR
   through a LUI-based table bound by SLTIU; tables read from data images too), code pointers in data (aligned words
   pointing at a stack frame setup or right after a JR ra / ERET / far J and its padding);
3. the extent of a function: a linear walk that ends after the slot of an unconditional transfer once no branch
   goes further, at the next start, at a reserved encoding, or after a call that does not return (call, slot,
   padding to an aligned address, or a stack frame setup right after the slot);
4. a linear sweep: compiler output is contiguous, so the gaps left inside the text the walk reached are swept (each
   run of valid code after padding is a function, explored in turn); after the last function reached, and over a gap
   of more than 64 KB between two functions reached, the sweep goes on only while candidates look like functions
   (plausible start, valid instructions, a return), so rodata and data are not taken for code. A function (but a
   seed) whose body holds an instruction compilers do not emit (LL / SC, immediate traps, COP0 other than the moves,
   TLB operations and ERET, a write to r0) is data: neither lifted nor explored. Ocarina of Time (PAL 1.1) shows why:
   its boot JALs into the `code` overlay's run-time addresses, which hold audio data in the boot megabyte; before
   2026-11-15 that pulled 24,400 junk functions (223,148 instructions, a 5-minute lift) into the pack, each one a
   content check at run time; now 146 functions, 6,092 instructions, a second.
   In recorded code (no sweep), a recorded address inside the function an earlier one starts (a return point, a
   branch target: the runtime records where the interpreter had to start) is not a start of its own: the function's
   entry switch resumes there, rather than pieces each left through the dispatcher.
   A branch to itself over a NOP (an idle loop) gets `RC_IDLE`: the turns its event check would let run go by at
   once, as the interpreter's blocks fast-forward them. Code that follows a function jumping through a table we could not read (a JR through a
   computed register), right after one of its internal branches, continues that function (its switch cases);
5. bodies longer than 1000 instructions are cut (compile time grows faster than linearly), and TLB-mapped ones at
   every 4 KB page (one translation per entry).

Measured on our libdragon homebrew against the ELFs' function symbols (statically, before any run; the loop adds the
code that runs but is in no image, e.g. none for these ROMs):

| ROM | function symbols | found (a function starts there) | covered (inside lifted code) | functions lifted |
|---|---|---|---|---|
| hello | 261 | 260 (99.62 %) | 100 % | 739 |
| cpu | 261 | 260 (99.62 %) | 100 % | 753 |
| timer_irq | 272 | 271 (99.63 %) | 100 % | 756 |
| gfx | 351 | 349 (99.43 %) | 100 % | 1203 |
| rsp | 270 | 269 (99.63 %) | 100 % | 764 |

The symbols missed (`__ascii_wctomb`, `__ulp`) sit right after a function that ends without a return the walk
sees; they are inside a lifted entry, and lifted functions resume at any instruction, so they run natively anyway.
More functions are lifted than there are symbols: the jump-table cases of a few large functions (`_vfprintf_r`,
`mips_disasm`) whose table base comes from the stack are separate entries, which costs a dispatch at those cases.

**TLB-mapped code.** Entries are keyed by virtual address and content. Code at a mapped address (KUSEG, KSSEG,
KSEG3: some games map overlays at 0x7F000000 and the like) cannot be found statically (the mappings are set up at run
time), so the discovery loop records it with the bytes read through the translation, never past the end of a 4 KB
page, and the tools cut mapped functions at page boundaries. The runtime translates the PC with the current TLB,
ASID and mode, checks the bytes at the physical page it maps to, and caches the answer until a TLBWI / TLBWR /
EntryHi write or a write to the page; lifted code never touches the TLB itself.

**RSP microcode is data.** It is DMA'd to IMEM and run by the RSP (LLE, `n64_rsp.c`); it is in no executable image
of the CPU and the runtime records only code the CPU ran, so it is never lifted.

**libultra signatures.** Not used: recognising library functions would only name them (the lifter needs no names),
and we keep no Nintendo code or byte patterns derived from it.

## Lifting

`lift.py` follows the interpreter (`n64_cpu.c`, `n64_fpu.c`) instruction for instruction; the ABI and its rules are
in `n64_recomp_rt.h`, the design notes at the end of `lift.py`. The shape is the GBA / PS1 recompilers' (designed for
compile time on the phone):

- **Blocks.** A function is cut into blocks: its start, jump targets, the fall-through and return point after a
  transfer, the instruction after one always handed to the interpreter, then every 12 instructions. Only block starts
  are resume points (a `case` of the entry switch); entered inside a block (an interrupt's ERET returns anywhere),
  the function lets the interpreter run to the block's end (`rc_hint`, from a small table).
- **One event check per block.** The interpreter checks the scheduler's stop and the interrupt line before every
  instruction; lifted code checks once at the block's start whether any of its instructions would be stopped (static
  ticks: fetch, multiply / divide latencies); if so it leaves (due now) or the interpreter runs the block, stopping
  exactly where it would have. After a call into the core the rest of the block is checked again.
- **Memory.** Cached RDRAM inline (KSEG0; stores through KSEG1 too: the interpreter charges nothing more for an
  uncached store, and libdragon draws into uncached framebuffers). An access at a known constant address off cached
  RDRAM (a LUI-built base, also a 64 KB base plus an ANDI-masked index: the drivers' RCP registers, uncached RDRAM)
  goes through the core and stays native. An access through a pointer that turns out to be off cached RDRAM is handed
  to the interpreter with the rest of its block (one exit, no call into the core: small code).
- 64-bit GPRs in C locals (only those the function uses, written back only when it writes them), 32-bit results
  sign-extended, delay slots inline, branch-likely skipping its slot, COP1 moves inline and arithmetic through
  `n64_fpu.c` (identical results, flags, causes, exceptions), COP0 / exceptions / LL-SC / a branch in a delay slot
  handed to the interpreter with the state in front of the instruction (exact EPC / BD / BadVAddr), calls and
  indirect jumps through the dispatcher (bounded C stack).
- **Stable files.** Functions in address order, a file cut after a function whose identity (start, hash) hashes to
  0 mod 8 once it holds 1000 instructions (at 4000 at most); files are named after their first function. A
  discovery round changes only the files that gained functions, and the object cache (`n64_recomp_build.py`, Prepare)
  recompiles only those. Flags: `-O1 -fno-tree-slp-vectorize` (tools/prepare/gba.py `lifted_cflags`).

`--resume-all` makes every instruction a block of its own (debugging).

## Limits

- The RDP's rendering into RDRAM is write-tracked per command list (XBA-174: `n64_rdp.c` reports the rows of its
  colour and Z images it drew through the bus's `wrote` callback, once the pixels are written), so code drawn over by
  the RDP is checked again like code overwritten by DMA. Texture loads only read RDRAM.
- Lifted code runs in kernel mode only (every N64 game runs in kernel mode); user / supervisor code is interpreted.
- Compile time: about 0.8-0.9 s of compiler per 1000 instructions at -O1 on a desktop x86 core with gcc (the
  per-instruction design of the first version cost 1.6-2.8 s): homebrew ~28 k instructions in ~4 s with 6 jobs,
  n64-systemtest 407 k instructions in ~77 s with 6 jobs (1.1 s / 1k on the loaded host).
