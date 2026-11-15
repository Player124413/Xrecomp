# Environment Variables

> Every `RECOMP_*` switch the runtime and tools read, what it does, and what it defaults to.
>
> **Read this when:** a title needs a bring-up switch, a log is asking you to set one, or you want to know what a variable you found in a script does.
>
> Part of the [Documentation Index](../INDEX.md)

Debug switches are **off by default**; they only print or dump and never change what the title does. A console behaviour the runtime now models by default keeps an **A/B switch** (usually `=0`) that restores the behaviour before, for one release; its row names the ticket that changed the default. No switch belongs to one title: what a title needs comes from its profile layers ([Forced by a title](#forced-by-a-title), [profiles.md](../technical/profiles.md)), so one title's fix never moves another's output.

How to read the tables:

- **Default** is what happens with the variable unset. "off" means the variable has to be present (or non-zero, where the entry says so). "on" means it is active until you set `0`.
- **Where** is the source file that reads it, so the code is one search away.
- Values are text. A flag is "set" when the variable exists, unless the entry says `1` or non-zero is required.

On Android these come from `recomp.cfg` in the app's external files dir. The launcher's Settings screen writes a few of
them there for the player (`RECOMP_LANGUAGE`, `RECOMP_WIDESCREEN`, `RECOMP_480P`, `RECOMP_ASPECT`, `RECOMP_GPU`,
`RECOMP_GPU_SCALE`, `RECOMP_GPU_FPS_CAP`, `RECOMP_PRESENT_VSYNC`, `RECOMP_TEX_BILINEAR`, `RECOMP_TEX_ANISO`,
`RECOMP_APU_HRTF`, `RECOMP_AUDIO_BUF_SAMPLES`, `RECOMP_RASTER_THREADS`, `RECOMP_BIG_CORES`, `RECOMP_PAD_MAP`,
`RECOMP_PAD_PORTS`, `RECOMP_LSFG*`, `RECOMP_SHELL_SELFTEST`; it sets `RECOMP_GAME_DIR`, `RECOMP_PAD_BUILTIN` and,
for a library check, `RECOMP_STATUS` itself, and reads `RECOMP_DISC_IMAGE` and `RECOMP_COMPANION_FOCUS`) and keeps every
other line as written: see [Settings](../android/app-shell.md#settings).

### XDK defaults

The switches every XDK title needs because the console works that way are set by the title project itself
(`recomp_xdk_defaults()`, `src/kernel/recomp_xdk_defaults.c`, called by `templates/new-game` main right after the profile
layers' `xba_title_configure`), unless the environment, `recomp.cfg`, a forced option or a profile layer already set them:
`RECOMP_DISPATCH_GATE=1`, `RECOMP_VBLANK=paced`, `RECOMP_USB=1`, `RECOMP_KEYBOARD=1`, `RECOMP_AC97_READY=1`
(`RECOMP_PB_EXEC=1` already was a default of the title project and of the Android shell). The XDK layers
(`profiles/xdk/*`) that used to set them are gone: every title, whatever its library table says, boots with picture,
sound and pad (XBA-106). When it sets any, one line:
`[XDK] defaults: RECOMP_DISPATCH_GATE=1 ... (no profile layer set them; RECOMP_XDK_DEFAULTS=0 turns them off)`.

| Variable | Default | What it does | Where |
|---|---|---|---|
| `RECOMP_XDK_DEFAULTS=0` | on | Sets none of the XDK defaults (A/B). Each switch can also be turned off on its own with `=0`. | `recomp_xdk_defaults.c` |

### D3D service

D3D's GPU semaphore and software methods are answered by the D3D service (`src/kernel/recomp_d3d_service.c`, the
former profile layer `profiles/xdk/d3d8/d3d8.c`), which `templates/new-game` main configures right after
`recomp_xdk_defaults()` and starts right before the guest's entry point, for every title: D3D's device is found at
run time (`[D3D] device found at ...`); a title without D3D finds none and the service stays idle. It also makes the
DMA walk the default walk (`RECOMP_PB_WALK`, below). [profiles.md](../technical/profiles.md#the-d3d-service).

| Variable | Default | What it does | Where |
|---|---|---|---|
| `RECOMP_D3D_SERVICE=0` | on | Leaves the service out: no software methods answered, no semaphore watch, no DMA-walk default (A/B; D3D titles freeze without it). | `recomp_d3d_service.c` |
| `RECOMP_D3D_DEVICE_PTR=0x...`, `RECOMP_D3D_DEVICE=0x...` | found at run time | The device pointer slot (D3D's `g_pDevice`) or the device itself, instead of the search (A/B; they were a title layer's `XBA_D3D_DEVICE_PTR` / `XBA_D3D_DEVICE` params). Either one alone is enough: the device is not searched for, the semaphore watch reads the given device (else what the given slot holds), every software method is answered as for a found device unless `RECOMP_D3D_SW_METHODS` says otherwise, and RunPushBuffer's methods write without the run-time guards. Said at start-up: `[D3D] device given (...): not searched for; RunPushBuffer methods write unchecked`. Before WP-C a given address left RunPushBuffer's methods off unless `RECOMP_D3D_SW_METHODS` turned them on, and Fable stopped flipping after its load (`not serviced (type:count) D:4620`). | `recomp_d3d_service.c` |
| `RECOMP_D3D_SW_METHODS=0x...` | every one (device found or given) | The software methods answered, bit n = method n (`0x7000`: RunPushBuffer, 0xC-0xE); 5 (the fence event), 6-7 and 8-9 are on whatever it says. A value that leaves a RunPushBuffer method out is said at start-up (`[D3D] RECOMP_D3D_SW_METHODS=... leaves RunPushBuffer methods 0x... unserviced`): a title that defers RunPushBuffer then stops, and the methods are counted under `not serviced` in the `[GPU] software methods:` line. | `recomp_d3d_service.c` |
| `RECOMP_D3D_CALLBACKS=0` | on | Leaves D3D's InsertCallback software methods (6, 7) unserviced instead of calling the title's routine at DPC level (XBA-94). | `recomp_d3d_service.c` |
| `RECOMP_D3D_FENCE=mirror` | off | The A/B fallback Burnout 3 was brought up with: no software methods, no semaphore watch, the segment scan, D3D's fence mirrored as "GPU always caught up". Needs `RECOMP_D3D_DEVICE_PTR`. Its old name `XBA_D3D_FENCE` is still read. | `recomp_d3d_service.c` |

### Forced by a title

A title profile (`profiles/titles/<title id>/title.json`, or any profile layer: [profiles.md](../technical/profiles.md)) can force options that the title cannot run right without, over
everything else (the shell environment, `recomp.cfg`, the launcher's Settings):

```json
"env_force": {"RECOMP_GPU": "0"}
```

No shipped profile forces anything since WP4 (Burnout 3's `RECOMP_LSFG=0` went: frame generation now waits for the
run-time lock verdict and stays off by itself for a title locked to 60, [Frame Generation](../technical/framegen.md#titles-locked-to-the-flip-rate)).

- **Allowed**: names `RECOMP_[A-Z0-9_]+` (63 characters at most), values strings of printable ASCII (255 at most; a
  JSON number is taken as its text), 16 entries at most. `tools.prepare` refuses anything else (`BAD_PROFILE` on
  Android, an error on the desktop); the runtime checks again and ignores a refused entry with a `[TITLE]` line on
  stderr. The value is set as is: for a switch read by presence, `"0"` still turns it on.
- **Why not `setenv` in the profile**: `xba_title_configure` runs when the guest thread starts, after the window
  and the frame generation pacer have read their options.
- **Android**: `tools/prepare` copies it into `pack.json` ([prepare.md](../android/prepare.md#packjson));
  `pack_loader.c` sets it after `recomp.cfg` and before the dlopen (`recomp_pack_apply_env_force`,
  `recomp_cfg_force`). The launcher's frame generation row says « Frame generation unavailable for this title » when
  the selected game's pack forces `RECOMP_LSFG` off.
- **Desktop**: `tools.prepare` writes it to the project's `src/title/env_force.h`; `main()` (templates/new-game)
  sets it first thing.
- Log, one line per option: `[TITLE] 4541005B forces RECOMP_GPU=0 (title profile)`.

## Bring-up and diagnostics

Start here when a title does not run. See [Step 6: Iterative Debugging](../pipeline/06-debugging.md) for the workflow these belong to.

| Variable | Default | What it does | Where |
|---|---|---|---|
| `RECOMP_WATCHDOG_SECS=N` | off | Dumps registers, PUT/GET and the guest stack when the title stops making progress for N seconds. | `xbox_memory_layout.c` |
| `RECOMP_WATCH=<va>` | off | Names the guest code that writes a value at an address (a callback slot, a "ready" flag). Wrap the address in `[` `]` to watch the word it points to. | `xbox_memory_layout.c` |
| `RECOMP_WATCH_LEN` | off | Length in bytes of the watched range. | `xbox_memory_layout.c` |
| `RECOMP_WAIT_LOG=1` | off | One `[WAIT]` line for each single-object wait that timed out or lasted 500 ms or more. | `kernel_sync.c` |
| `RECOMP_WAIT_REPORT=0` | on, top 4 | Turns off the per-site wait report: every periodic `[KERNEL] summary` (2 s) is followed by up to N lines `[KERNEL] waits: <object> set xS; from <return address> woke xW of T, blocked M ms` for the wait sites that waited since the last summary, ranked by blocked time. `set` counts KeSetEvent, KePulseEvent, NtSetEvent, NtPulseEvent and device-model sets on the object (absent when nothing set it), `woke` the waits that returned satisfied, `of` all waits from that call site, `blocked` the host time inside them (zero-timeout polls are not timed); totals since start; multi-object waits are filed under their first object and marked `(any of N)` or `(all of N)`. Wakes far below sets on an event two threads wait on means lost wake-ups. `=N` lists the top N (1-16). Fixed tables (256 sites, 128 set objects), lock-free; a full table says how many it dropped. | `kernel_wait_stats.c` |
| `RECOMP_SMC=0` | on (POSIX) | Code written at run time (XBA-118): the lifted code pages are write-protected right before the guest's entry point, and the first write into each is recorded (`[SMC] code written at run time ...`, a `{"t":"smc","kind":"code_write"}` discovery entry), the page opened and the write retried: nothing is paid after the first write per page. `0` protects nothing (A/B). Calls into the heap, stack or data are named either way (`exec_outside_image`). Nothing is protected on Windows. | `recomp_smc.c`, `xbox_memory_layout.c` |
| `RECOMP_SMC_WX=1` | off | With `RECOMP_SMC`: also protect the sections marked writable as well as executable, left writable by default since writes there may be the title's data. | `recomp_smc.c` |
| `RECOMP_WATCH_DEPTH` | 14 | Frames printed per watch hit; identical consecutive return addresses are folded. | `xbox_memory_layout.c` |
| `RECOMP_WATCH_RAW` | off | Also prints the raw stack frame, to see which object the writer was working on. | `xbox_memory_layout.c` |
| `RECOMP_KERNEL_WATCH=<va>` | off | Reports a guest address whenever a kernel bridge call changes it. | `kernel_bridge.c` |
| `RECOMP_KERNEL_WATCH_ALL` | off | With the above: prints every sample, so a value that was already wrong at the first call shows. | `kernel_bridge.c` |
| `RECOMP_KERNEL_LOG_BUDGET=N` | 200 | Number of kernel-call log lines kept before the log goes quiet. | `kernel_bridge.c` |
| `RECOMP_IRQL_TRACE` | off | Prints the first IRQL transitions; a lower from a level the raise did not set is a calling-convention bug. | `kernel_hal.c` |
| `RECOMP_STATUS=<file>` | off | A headless run's status, one JSON line per period: flips, how often the picture changed, whether it is lit, non-silent audio, pad reads, guest threads, discovery entries. What the compat runner reads (`src/kernel/recomp_status.h` has the format). `RECOMP_STATUS_MS` sets the period (default 1000, at least 50). | `src/kernel/recomp_status.c` |
| `RECOMP_DISCOVERY=<file>` | the prepare's work folder (desktop: set by `tools.prepare`; Android: `<files>/work/<TITLEID>/`) | Where a run records what the static analysis missed: unknown call targets, thread starts, untranslated instructions reached. `0` turns it off. [discovery.md](../technical/discovery.md) | `src/kernel/recomp_discovery.c` |
| `RECOMP_STATIC_DISCOVERY=0\|1\|2` | 2 | Read by the Prepare (`tools.disasm`), not by the runtime: how hard the first analysis looks for code only a run would reveal (XBA-137). `2`: all passes, including code in gaps nothing references and the function pointers of packed (unaligned) tables (XBA-140); `1`: strong evidence only (callback arguments of kernel calls, constructor and RTTI vtables, exception handlers, noreturn kernel calls, switch tables outside code); `0`: as before XBA-137 (A/B; also what `tools/compat/test_e2e.py`'s self-healing test sets to keep its threads hidden). A change analyses again. [discovery.md](../technical/discovery.md#static-discovery-at-the-first-prepare-xba-137) | `tools/disasm/discovery.py` |
| `RECOMP_STATIC_DISCOVERY_CAP=<pct>` | no cap | With the above: static discovery adds at most this many percent more functions than the passes before it found (each costs Prepare time on a phone); the summary says `capped`. `tools.disasm --discovery-cap` overrides it. | `tools/disasm/discovery.py` |
| `RECOMP_STATIC_DISCOVERY_STRICT=1` | off | With the above (level 1 or 2): also tighten the older passes of the analysis: a start right after a ret must be a compiled body, a data word must not name the middle of a path nor random bytes after padding, a data section's switch table holds no function pointers. It keeps decoys out (the decoy tests run with it), but drops 6-14 % of the functions the older passes found on synthetic titles, and a real title may need one of them: off until the owner has compared the function counts on the reference titles (wave 5). By default static discovery only adds. A change analyses again. | `tools/disasm/discovery.py` |
| `RECOMP_UNIMPL_TRAP=1` | off | Stops at the first untranslated instruction that is actually reached, instead of logging it. | `templates/new-game/src/recomp_manual.c` |
| `RECOMP_MISDECODE_TRAP=0` | on (stops) | String port I/O (`ins*`, `outs*`) in lifted code is taken for data decoded as code: reached, it prints one `[MISDECODE]` line, writes an `unimpl` discovery entry and stops the title (KeBugCheckEx 0x1E). `0` reports each site once and carries on with the instruction a no-op, as before XBA-139. The lift report counts the sites (`lift health: ... misdecoded`). | `src/kernel/kernel_hal.c` |
| `RECOMP_STUB_LOG=1` | off | Prints each unresolved stub the first time it runs, with return address and `esp`. Read by the generated stubs file. | `tools/recomp/translator.py` |
| `RECOMP_TRACE_BUDGET=N` | 400000 | Lines of the call trace kept; a larger file is better than a hole. | `recomp_trace.c` |
| `RECOMP_TRACE_PROFILE=N` | 20000000 | Prints the call profile every N calls (and at exit). | `recomp_trace.c` |
| `RECOMP_TRACE_ARGS=N` | off | Adds N stack arguments to each trace line. | `recomp_trace.c` |
| `RECOMP_TRACE_DEREF` | off | Follows pointer arguments one level in the trace. | `recomp_trace.c` |
| `RECOMP_CS_TRACE_CRT[=all]` | off | Traces critical-section activity (CRT sections only, or all). | `kernel_rtl.c` |
| `RECOMP_CS_WATCH=<va>` | off | Traces one critical section by address. | `kernel_rtl.c` |
| `RECOMP_POKE=va[:val][,...]` | off | Holds guest globals at a value. A bring-up probe, not a fix. | `xbox_memory_layout.c` |
| `RECOMP_PEEK=<va>[,...]` | off | Prints guest words at each report, both as a number and as a pointer. | `nv2a_pb_exec.c` |
| `RECOMP_PEEK_CHAIN` | off | Follows a pointer chain level by level and says where a null breaks it. | `nv2a_pb_exec.c` |
| `RECOMP_TRAP_NULL` | off | Protects the null page so a null dereference faults. | `xbox_memory_layout.c` |
| `RECOMP_FORCE_RETURN` | off | Turns on the early return of functions generated with `--force-return`; without it they behave normally. | `xbox_memory_layout.c` |
| `RECOMP_MISSING_FILES` | list | `0` turns the missing-file report off, `all` lists every miss, otherwise the default report. | `kernel_missing.c` |
| `RECOMP_MISSING_FILES_LIST=<path>` | off | Writes the missing-file list to that path on exit. | `kernel_missing.c` |
| `RECOMP_CMDLINE=<text>` | off | Command line handed to the title, as a launcher would (`+map intro`). | `kernel_bridge.c` |
| `RECOMP_IMAGE=<path>`, `RECOMP_IMAGE_MAP=<file>` | set by the launcher | A disc with several XBEs ([multi-xbe.md](../technical/multi-xbe.md)): `RECOMP_IMAGE` names the XBE this executable is (it becomes `XeImageFileName`), `RECOMP_IMAGE_MAP` the table from an XBE path to its executable or pack, read when the title launches another XBE. Set by the Android pack loader and the desktop project, not by hand. | `kernel_reboot.c`, `pack_loader.c`, `templates/new-game/src/main.c` |
| `RECOMP_SHELL_SELFTEST=1` | off | Android app shell: after `SDL_Init`, runs the self-test in the app process (memory layout on the synthetic XBE asset, the 4 GB window, mirrors, trapped MMIO of each decoder class, execute protections, the save dir), one `selftest NAME: PASS` / `FAIL (reason)` line each and `selftest: N passed, M failed`, then starts as usual. Set it in `recomp.cfg`; `0` is off (docs/android/app-shell.md). | `shell_main.c`, `recomp_selftest.c` |
| `RECOMP_COMPANION_FOCUS=forward` | `none` | Android app, dual screen (AYN Thor): the second-screen panel's window is focusable and forwards the key and motion events it gets to the game, which ignores losing the focus to it (Plan B). `none`: the panel is never focusable (Plan A). Read from `recomp.cfg` by the Java side at each start of the panel (docs/android/app-shell.md « Dual screen »). | `DualScreen.java`, `CompanionScreen.java` |
| `RECOMP_SELFTEST_SKIP=<names>` | off | Self-test checks to skip, separated by commas or spaces (`mmio_simd`); each prints `SKIP`. The arm64 CI job skips `mmio_simd` under qemu-user. | `recomp_selftest.c` |
| `RECOMP_SHELL_CRASH_TEST=1` | off | Android app shell: a deliberate null write at start-up, to check on the device that a crash still reaches debuggerd (a tombstone naming `shell_main`) with the log line above it in logcat (XBA-52). Set it in `recomp.cfg`; never leave it there. | `shell_main.c` |

## Memory and kernel

Switches that change how the kernel layer behaves, one table per area. Many are now on by default (the console's behaviour) with an `=0` A/B switch; the rest fix a class of titles and stay off. See [Xbox Kernel Replacement](../technical/kernel-replacement.md) and [Memory Layout Reproduction](../technical/memory-layout.md).

### Console settings

What the title reads as the console's own settings (the dashboard's choices, the region, the keys). Player settings on Android: the launcher writes them.

| Variable | Default | What it does | Where |
|---|---|---|---|
| `RECOMP_LANGUAGE=<code>` | English | The console language the title reads (`XC_LANGUAGE` and the `XC_MAX_OS` block): `en`, `ja`, `de`, `fr`, `es`, `it`, `ko`, `zh`, `pt` (a region suffix such as `fr-CA` is ignored) or the Xbox value `1`..`9`. The title uses it when its disc has that language. A player setting, not a fix: on Android the launcher sets it (Settings › Game › Language, per-title choice in the in-game menu). | `kernel_xbox.c` |
| `RECOMP_PKCS1_VERIFY=strict` | accept | `XcVerifyPKCS1Signature` answers what the RSA check finds instead of TRUE. The check runs either way (PKCS#1 v1.5, SHA-1, BSAFE public key) and a failure is logged (`[CRYPTO] XcVerifyPKCS1Signature: signature does not verify`, first 8). Accepting is the default because what titles verify is signed with Microsoft's key, which the runtime does not hold (`XePublicKeyData` is zeros): XAPI's DVD security-sector check (KOTOR's certificate asks for it) and Xbox Live. `strict` is for a title that signs and verifies with a key of its own. Saves and content are HMAC, not RSA (XBA-116). | `kernel_crypto.c` |
| `RECOMP_GAME_REGION=na\|jp\|eu` | the title's | The console's region (XBA-121). Unset: the first the XBE certificate allows (+0xA0) of North America, Japan, rest of world; North America when it names none (the fixed answer before). The video standard follows: NTSC-M 60 Hz, NTSC-J 60 Hz, PAL-I 50 Hz. Read by `XC_FACTORY_GAME_REGION`, `XC_FACTORY_AV_REGION`, `XC_DVD_REGION` (1, or 2 outside North America), `XC_VIDEO` and the AV capabilities word (AV query 6). `na` restores the old answer for a PAL disc; `eu` shows a multi-region disc as a PAL console does. Logged once at load: `Certificate game regions ...`. | `kernel_xbox.c` |
| `RECOMP_PAL60=0` | on | On a PAL console, the dashboard's « PAL 60 » choice is on: `XC_VIDEO` has its 60Hz bit, so a title that supports PAL 60 sets a 60 Hz mode, as every title ran before XBA-121 reported a PAL console (a PAL Burnout 3 runs at 60 fps). A title without PAL 60 support still sets a PAL-I mode and the vblank follows it (50 Hz). `0`: the console's own default, PAL 50 Hz until the title sets a mode. No effect on NTSC consoles (60 Hz already). | `kernel_xbox.c` |
| `RECOMP_WIDESCREEN=1` | off | The dashboard's widescreen choice: `XC_VIDEO` and the AV capabilities word get the widescreen bit, so a title that supports it draws anamorphic 16:9 frames. Showing them wide is `RECOMP_ASPECT` (16:9). A player setting: Settings › Graphics › Widescreen on Android. | `kernel_xbox.c`, `kernel_hal.c` |
| `RECOMP_480P=1` | off | The dashboard's 480p choice (the AV pack is always the HDTV pack, which carries 480i without it): `XC_VIDEO` and the AV capabilities word get the 480p bit, so a title that offers progressive output picks it. A player setting: Settings › Graphics › 480p on Android. | `kernel_xbox.c`, `kernel_hal.c` |

### Files, discs and the XBE

| Variable | Default | What it does | Where |
|---|---|---|---|
| `RECOMP_SAVE_DIR=<dir>` | off | Root for the title's `UDATA`, `TDATA`, `TitleCache` and utility drive (`Cache/<title id>`, Z:) folders; created if missing. Absolute. Saves left in the game folder are then not used. | `kernel_path.c` |
| `RECOMP_GAME_DIR=<dir>` | `<external files dir>/game` | Android app shell: where the title's data (`default.xbe` and the disc's files) is read from. The launcher sets it from the folder the player chose (XBA-54; it then wins over `recomp.cfg`); otherwise set it in `recomp.cfg`. | `recomp_storage.c` |
| `RECOMP_DISC_IMAGE=0` | on | A disc image as the game directory (`RECOMP_GAME_DIR` or the desktop's `build/game` naming an `.iso`/`.cso`/`.zso`/`.cci` file, a split part, or `fd:N[,M]`) is served in place as D:\ (XBA-126, [disc-images.md](../technical/disc-images.md)); `0` refuses images (the game dir must then be an extracted folder). The Android launcher reads it from `recomp.cfg` too: `0` makes it extract a picked image as before instead of reading it in place. | `kernel_disc.c`, `DiscImages.java` |
| `RECOMP_ASYNC_IO` | off | Reports a read on an asynchronous handle as still in flight. The data is read synchronously either way. | `kernel_bridge.c` |
| `RECOMP_INLINE_APC=1` | off (deferred) | Runs a file request's completion APC (ReadFileEx, WriteFileEx) inline inside NtReadFile/NtWriteFile, the default before XBA-102, instead of at the issuing thread's next alertable user-mode wait (SleepEx, the Nt/Ke waits), which then returns STATUS_USER_APC as on the console. The status block and the event are set at the request either way. A/B switch kept for one release; `xbox_SetDeferredIoApc` wins over it. | `kernel_bridge.c` |
| `RECOMP_XBE_SECTIONS=0` | on | `XeLoadSection` / `XeUnloadSection` only move the reference count from what the file says, as before XBA-124. On by default: Preload sections start at 1 (with their shared-page counters), a writable section or one whose addresses another section held is copied again from the image on 0 → 1, and of two overlapping sections the Preload one is in memory at boot ([xbe.md](../formats/xbe.md#sections-loaded-on-demand-xba-124)). | `xbox_xbe_sections.c` |

### Memory

| Variable | Default | What it does | Where |
|---|---|---|---|
| `RECOMP_HEAP_RECLAIM` | off | Gives freed guest heap back: freed heap blocks are split instead of handed over whole, and `NtFreeVirtualMemory` returns heap memory. Contiguous memory is `RECOMP_CONTIG_RECLAIM`. | `xbox_memory_layout.c`, `kernel_bridge.c` |
| `RECOMP_CONTIG_RECLAIM=0` | on | `MmFreeContiguousMemory` gives the block back to the contiguous window, so the next request that fits gets its pages (and possibly its address). `0` keeps every block for good, the behaviour before (when only `RECOMP_HEAP_RECLAIM` freed them): addresses are never seen twice, but a title that frees and reloads textures per scene runs the 64 MB window out (Fable: `[CONTIG] arena exhausted`). The `[CONTIG]` lines count `released=` blocks and `free calls=`. Read once. | `xbox_memory_layout.c` |
| `RECOMP_EXT_VMA` | off | Extended virtual-memory areas above the mirrors. | `guest_vmem.c`, `xbox_memory_layout.c` |
| `RECOMP_GUEST_WINDOW=0/1` | on for every 64-bit POSIX host (Android, Linux, macOS); 32-bit hosts cannot hold it | POSIX: hold the whole 4 GB guest window from the start and carve every view out of it, so the contiguous window, the apertures and the tiled alias never land on a taken host range. `0` is the old desktop layout (base + mirrors only), which loses the contiguous window once a map passes 64 MB (XBA-103, staged in `tests/memory_layout_posix`). Implied by `RECOMP_EXT_VMA`. | `xbox_memory_layout.c` |
| `RECOMP_LAZY_RESERVE=0` | on (POSIX) | XBA-103. A pure `MEM_RESERVE` the heap cannot back is granted whole as a lazy reservation, carved top-down from guest 0x7FFE0000 inside the held window: zero-filled read-write pages (`MAP_NORESERVE`) the host backs only once touched, replacing the RAM mirrors there. No title needs `xbox_SetMapSize` for its large reserves any more (Fable 608 MB, Half-Life 2 128 + 200 MB). Release and decommit give the pages back; the address is not reused. `MmQueryStatistics` still reports 64 MB. `0` clamps such a reserve to what the heap can back, as before. Off under `RECOMP_EXT_VMA`; Windows keeps the old path. | `xbox_memory_layout.c`, `kernel_bridge.c` |
| `RECOMP_CONTIG_AVOID_IMAGE=1` | off | Keeps contiguous allocations out of the image range. | `xbox_memory_layout.c` |
| `RECOMP_WIN_PLACEHOLDER` | off | Reserves the guest window with placeholders on Windows hosts where fixed bases are taken. | `xbox_memory_layout.c` |
| `RECOMP_PUT_BARRIER=1` | off | Linux/Android: `membarrier` at each new DMA_PUT, so the executor never reads a pushbuffer word the title stored before PUT but another core has not seen yet (arm64). One IPI per kick. | `xbox_memory_layout.c` |

### Threads, synchronisation and APCs

| Variable | Default | What it does | Where |
|---|---|---|---|
| `RECOMP_THREAD_STACK_MB=N` | 8 | POSIX: floor, in MB (1 to 1024), for the host stack of every thread the Win32 shim creates for a routine (`CreateThread`, so guest threads from `PsCreateSystemThreadEx`, plus timer and DPC callbacks). Lifted code recurses on the native stack; without a floor bionic gives about 1 MB and an Xbox thread's 64 KB would be the whole stack. A larger request is kept. Logged once as `[win32_compat] thread stack floor`. The guest main thread has its own 64 MB stack (`fb_present_sdl.c`). The shim's threads are detached, so a thread that ends gives its stack back at once; its handle keeps the exit code and the signalled state (XBA-56). | `win32_compat.c` |
| `RECOMP_BIG_CORES=0/1` | on for Android, off elsewhere | big.LITTLE hosts: run the runtime on the cores whose cpufreq maximum is above the slowest cluster's (set once at start by `recomp_host_cpus_init`; every thread inherits it). The runtime's threads, the consoles' RDP / GPU workers included, are put back on them after a resume. | `host_cpus.c`, `host_perf_hint.c` |
| `RECOMP_GUEST_CPUS` | `one` on arm64 Linux and Android, `all` elsewhere | `all`, `one`, or a core number: pin the title's threads to one core. On arm64 this gives the title's threads x86's store order among themselves (XBA-81). | `kernel_thread.c` |
| `RECOMP_GUEST_LOCK=1` | off | Only one guest thread runs guest code at a time. | `kernel_guest_cpu.c` |
| `RECOMP_GUEST_QUANTUM=ms` | 4 | With the lock: milliseconds before the lock is handed over (0 = at every call). | `kernel_guest_cpu.c` |
| `RECOMP_GUEST_CPU_TRACE[=ms]` | off | With the lock: periodic trace of who holds it. | `kernel_guest_cpu.c` |
| `RECOMP_TIB_PER_THREAD=0` | on | An image without a TLS directory gets no TIB per thread, as before XBA-113 (A/B): every thread shares the main TIB (SEH head, IRQL byte, `fs:[0x28]` KTHREAD) and the timer thread, which needs a TIB of its own, runs no timer DPCs. An image with TLS always gets one. | `xbox_memory_layout.c` |
| `RECOMP_THREAD_STACKS=N` | unset (no cap) | Optional cap on worker thread stacks alive at once, for A/B (`8` was the old fixed pool). Each stack is 512 KB of guest heap; without a cap the pool grows until the heap is exhausted (about 100 stacks in a 64 MB window if the title took nothing). Past the cap or the heap, `PsCreateSystemThread(Ex)` fails with `STATUS_INSUFFICIENT_RESOURCES` (it used to run the worker inline, which deadlocked a worker that waits for requests). The high-water mark is logged at 8 and at each doubling (`[MEM] guest thread stacks: high-water mark`); `xbox_ThreadStacksHighWater()` reads it (XBA-104). | `xbox_memory_layout.c` |
| `RECOMP_IRP_DISPATCH=0` | on | IRPs in guest memory (XBA-138): IoAllocateIrp / IoInitializeIrp lay out an initialised IRP, IoBuildAsynchronousFsdRequest builds one for a device, IofCallDriver dispatches to the title's own driver (`DriverObject->MajorFunction[major]`) or completes the IRP with STATUS_INVALID_DEVICE_REQUEST, IofCompleteRequest runs the completion routines, writes UserIosb and sets UserEvent. `0`: the stubs before (IofCallDriver answers success without dispatching, IofCompleteRequest does nothing, IoBuildAsynchronousFsdRequest answers NULL, IoInitializeIrp only zeroes). The stack-location layout is partly from memory: `kernel_bridge.c`. | `kernel_bridge.c` |
| `RECOMP_THREAD_NOTIFY=0` | on | Routines a title registers with PsSetCreateThreadNotifyRoutine are kept but never called. With them on (XBA-114), each runs with Create TRUE on the creating thread before a PsCreateSystemThreadEx worker starts, and with FALSE on the worker as it ends, as on the console. A/B switch for one release. | `kernel_bridge.c` |
| `RECOMP_WAIT_SPIN=0` | on | Turns off the wait-spin detector: a thread that waits on one object 2,000 times within a second is reported (object, header, status, caller, frames; `wait_spin` in the discovery file), and an event header whose waits fail at once is promoted to a guest-built event, as `RECOMP_TITLE_KEVENTS` does for every object, once something has tried to set it (KeSetEvent, software method 5); `=promote` promotes without that evidence (XBA-93). | `kernel_bridge.c` |
| `RECOMP_TITLE_KEVENTS=0` | on | Turns off host events for events a title builds inline instead of through `KeInitializeEvent` (XAPI, XPP and D3D build theirs that way): a wait on one then fails at once instead of blocking until it is set, as before K-5. Even then, the XDK D3D layer adopts D3D's fence event alone when it finds the device at run time under the DMA walk (`xbox_KeAdoptGuestEvent`), and the wait-spin detector promotes a spinning one. On by default since KOTOR's synchronous USB request returned early on a failed wait and its stack URB was later completed into garbage. | `kernel_bridge.c` |
| `RECOMP_CS_MODE=single\|guest` | per-section | `single`: backs every critical section with one host lock. `guest`: runs critical sections on the guest's own structure (state at +0x10), falling back to the shadow for ones that don't read as critical sections. | `kernel_rtl.c` |
| `RECOMP_QUEUE_LIMIT=0` | on | No concurrency limit on KQUEUEs and I/O completion objects, as before XBA-133: every remover gets an entry. With the limit, a thread that took an entry counts as running (CurrentCount) until its next kernel call, and a remover waits while CurrentCount has reached MaximumCount (1 by default, the console's processor count). A/B switch for one release. | `kernel_ksync.c` |
| `RECOMP_APC_WAKE=0` | on | A user APC queued for another thread (KeInsertQueueApc, NtQueueApcThread) no longer ends the alertable user-mode wait that thread is blocked in; it runs at the thread's next alertable wait, as before XBA-133. KeAlertThread still sets the thread's Alerted flag. A/B switch for one release. | `kernel_bridge.c` |
| `RECOMP_WORKERS` | off | Runs requests of the title's worker threads inline. A bisecting tool for concurrency bugs. | `kernel_bridge.c` |

### Exceptions and stack switching

| Variable | Default | What it does | Where |
|---|---|---|---|
| `RECOMP_SEH_DISPATCH=0` | on | Guest structured exception handling (XBA-112): `RtlRaiseException`, `ExRaiseException` and `ExRaiseStatus` walk the chain at `fs:[0]` and call the lifted handlers, `RtlUnwind` unwinds, and a handler's resume longjmps to the frame's anchor; a lifted divide error with a handler chain is raised as `STATUS_INTEGER_DIVIDE_BY_ZERO` / `_OVERFLOW`, and a native guest `longjmp` unwinds the exception frames it leaves and the try levels of the `setjmp` caller's own frame (XBA-132). `0`: a raise is logged and returns, a divide error stops with its report, a `longjmp` leaves `fs:[0]` alone (the behaviour before), and `RECOMP_SEH_FAULTS` is off too. Read once. [seh-handling.md](../technical/seh-handling.md) | `xbox_seh.c`, `kernel_hal.c`, `xbox_memory_layout.c` |
| `RECOMP_SEH_FAULTS=0` | on (Linux x86-64, Linux/Android arm64; not macOS yet) | Host faults on guest addresses (XBA-132): a load or store that faults on a guest address that is not trapped MMIO, on a thread with a guest handler chain, is raised as `STATUS_ACCESS_VIOLATION` from a trampoline on the thread's own stack; a handler resumes at its `__except`, or continues and the access runs again with the faulting context restored; nothing handled it, the fault goes down the signal chain with its original context (ART, debuggerd, the default action). `0`: every such fault goes down the chain at once (the behaviour before). Read once. [seh-handling.md](../technical/seh-handling.md#host-faults-on-guest-addresses-xba-132) Only from lifted code: a fault inside one of the runtime's own bridges (a kernel call) is never raised to the guest. | `xbox_seh_fault.c`, `xbox_seh.c` |
| `RECOMP_STACK_SWITCH=0` | on (POSIX arm64 / x86-64) | A function that loads esp from memory and returns on the other stack (a fiber or coroutine switch) runs the other side on a host stack of its own (XBA-101). `0`: its ret is a plain C return, the behaviour before (the other side never runs). Read once. [stack-switching.md](../technical/stack-switching.md) | `recomp_stack_switch.c` |
| `RECOMP_STACK_SWITCH_STACK_MB=N` | 8 | XBA-101. Host stack of each guest stack the switch runs, 1 to 256 MB, reserved and committed as used. | `recomp_stack_switch.c` |
| `RECOMP_STACK_SWITCH_MAX=N` | 1024 | XBA-101. Contexts (guest stacks) per thread before the one parked longest is dropped, 4 to 65536. | `recomp_stack_switch.c` |
| `RECOMP_STACK_SWITCH_WHOLE=0` | 1 | A/B (XBA-101): contexts parked at the same esp and return address (copy-stack fibers) are told apart by their first 64 bytes of guest stack only, not the whole of it. | `recomp_stack_switch.c` |

### Interrupts and devices

The one-CPU gate and the device models' readiness.

| Variable | Default | What it does | Where |
|---|---|---|---|
| `RECOMP_DISPATCH_GATE=1` | on in a title project (XDK default) | The one-CPU gate: guest code runs only at safe points while interrupts are delivered. Off in the runtime library itself (tests); a title project sets it ([XDK defaults](#xdk-defaults)); `0` turns it off. | `kernel_hal.c` |
| `RECOMP_IRQ_SAFE_POINTS` | on with the gate | `0` disables safe-point delivery under the gate. | `kernel_hal.c` |
| `RECOMP_IRQ_SAFE_FORCE` | off | Forces delivery at a safe point. | `kernel_hal.c` |
| `RECOMP_IRQ_SAFE_MS=N` | 250 | Milliseconds the gate waits for a safe point. | `kernel_hal.c` |
| `RECOMP_DPC_ON_RAISE` | on with the gate | `0` stops DPCs running when IRQL is raised. | `kernel_hal.c` |
| `RECOMP_CLI_GATE` | on | A guest `cli` below DISPATCH_LEVEL masks device interrupts until `sti` (or a `popfd` that sets IF) as a raise does: counted for the device models' hold-off, and under `RECOMP_DISPATCH_GATE` the one-CPU gate is held, so an interrupt is posted and runs at the `sti`. The guest-visible IRQL does not change. `0`: `cli`/`sti` only change EFLAGS.IF, as when they were no-ops. | `kernel_hal.c` |
| `RECOMP_CPUID_NO_SIMD` | off | Hides SIMD from `cpuid`. | `kernel_hal.c` |
| `RECOMP_AC97_READY=1` | on in a title project (XDK default) | Reports the AC'97 codec ready and routes the APU aperture to the emulated APU. `0` (or empty) is off. | `xbox_memory_layout.c`, `templates/new-game/src/main.c` |
| `RECOMP_AC97_ALIAS=0` | on (POSIX) | With `RECOMP_AC97_READY`: arm the AC'97 bus-master write trap with the old `mprotect` window instead of a read-only shared page plus a writable view. The window lets a second thread's store keep the RR bit, so a channel reset can spin (Fable during movies); for A/B only. | `xbox_memory_layout.c` |
| `RECOMP_DSP_ACK=va[,...]` | off | Guest addresses to acknowledge as the DSP's doorbell. | `xbox_memory_layout.c` |

## Graphics: pushbuffer executor

The executor is itself opt-in: set `RECOMP_PB_EXEC=1` to execute the pushbuffer. See [Pushbuffer Executor](../technical/pushbuffer-executor.md).

| Variable | Default | What it does | Where |
|---|---|---|---|
| `RECOMP_PB_EXEC` | off (Android: on) | Executes the title's pushbuffer. On Android the app shell sets it unless `recomp.cfg` says `RECOMP_PB_EXEC=0` (docs/android/app-shell.md). | `nv2a_pb_scan.c`, `xbox_memory_layout.c` |
| `RECOMP_PB_SCAN` | off | Read-only survey of the pushbuffer; ranks what is not implemented. | `nv2a_pb_scan.c`, `xbox_memory_layout.c` |
| `RECOMP_PB_WALK=dma`, `=scan` | `dma` in a title project (the D3D service sets it unless something already did); the segment scan in a project that never configures the service | `dma`: walks the pushbuffer as the DMA engine does (JUMPs, ring wraps, its own GET published before every command), which keeps D3D8LTCG's fence wait on DMA_GET and the 0x0310 latch honest and delivers D3D's software-method interrupt. `scan`: the segment scan (A/B; KOTOR ran 5 of 5 runs either way, K-7). It was the `xdk/d3d8ltcg` layer's `XBA_D3D_DMA_WALK`. | `nv2a_pb_scan.c`, `recomp_d3d_service.c` |
| `RECOMP_NV2A_TRACE` | off | Shows whether `DMA_PUT` moves at all. | `xbox_memory_layout.c` |
| `RECOMP_POLL_WAIT=0` | on | The lifter marks loops that only poll one memory word (`test dword [mem], imm ; jne/je` to itself) with `RECOMP_POLL_WAIT`. For D3D KickOff's wait on the PFB flush bit (0x100410 bit 16) the thread then blocks until the ack thread clears the bit (`xbox_Nv2aKickWait`) instead of spinning a host core; other words still spin. `0` keeps the spin for the flush bit too. Takes effect on titles prepared with a toolkit that has the lifter change. | `xbox_memory_layout.c`, `tools/recomp/translator.py` |
| `RECOMP_PB_REPORT_MS=N` | 10000 | Milliseconds between `[GPU]` reports (minimum 100). | `xbox_memory_layout.c` |
| `RECOMP_PB_EXEC_VERBOSE` | off | Per-batch trace. | `nv2a_pb_exec.c` |
| `RECOMP_PB_UNHANDLED_ALL` | top 10 | Lists every method the executor does not model, not only the top ten. | `nv2a_pb_exec.c` |
| `RECOMP_VBLANK[=paced]` | `paced` in a title project (XDK default); no vblanks in the bare runtime | Raises vblanks: a ~50 Hz tick, or `paced` for the display's schedule: 60 Hz, or 50 Hz once the title sets a PAL-I mode (before any mode, 50 Hz on a PAL console with `RECOMP_PAL60=0`; XBA-121). `0` or empty is off. | `kernel_bridge.c`, `kernel_hal.c` |
| `RECOMP_FLIP_PACING[=edge]` | off | Holds each flip to the vblank count. | `nv2a_pb_exec.c` |
| `RECOMP_FFP_INLINE=<0\|1\|2>` | 1 | Inline (Begin/End) fixed-function batches: `1` transformed by the composite matrix, `0` the old screen-space guess, `2` transformed only when the guess would skip them. A/B switch for one release. | `nv2a_pb_exec.c` |
| `RECOMP_FFP_FLIP_NORMALS` | off | Lights the side of a fixed-function mesh that faces the viewer. Not hardware behaviour. | `nv2a_pb_exec.c` |
| `RECOMP_CLIP_NEAR_Z` | on | `0` turns off the near-plane clip (window z below `SET_CLIP_MIN`). | `nv2a_pb_exec.c` |
| `RECOMP_SWIZZLED_RT` | on | `0` stops rendering into swizzled render targets. | `nv2a_pb_exec.c` |
| `RECOMP_NO_COMBINERS` | off | Skips the register combiners. | `nv2a_pb_exec.c` |
| `RECOMP_NO_VSH` | off | Skips vertex programs. | `nv2a_pb_exec.c` |
| `RECOMP_RASTER_THREADS=N` | auto | Threads drawing (and shading vertex programs) for the software rasteriser, the executor thread included, on Windows and POSIX. Auto: online cores − 4 on desktop; on Android the cores the runtime may run on (the fast cluster under `RECOMP_BIG_CORES`) − 1. | `nv2a_pb_exec.c` |
| `RECOMP_ACK_IDLE_US=N` | 250 | POSIX: with nothing submitted, the NV2A ack thread sleeps in 50 us steps for up to N us (watching PUT and the busy bits) instead of yielding in a loop. While the title kicks through `xbox_Nv2aKickWait` (D3D's KickOff wait, through the lifter's `RECOMP_POLL_WAIT` mark), it instead sleeps until the next kick, for at most N us, and yields only briefly for the DMA_PUT after an ack that released the title. 0 restores the bare yield. | `xbox_memory_layout.c` |
| `RECOMP_RASTER_SPIN_US=N` | 20 | POSIX: how long an idle rasteriser worker watches for the next job before it parks (`0`: park at once). The executor hands batches over back to back, and a wake costs tens of microseconds. | `nv2a_pb_exec.c` |
| `RECOMP_VSH_POOL=0` | on | Shades every vertex-program batch on the executor thread instead of splitting batches of 192+ vertices across the rasteriser's worker threads (same output either way). | `nv2a_pb_exec.c` |
| `RECOMP_FFP_BATCH=0` | on | Lights and transforms fixed-function vertices one by one, re-reading the light registers and attribute layout for each vertex, instead of resolving them once per batch and transforming each unique index once (same output either way; A/B switch). | `nv2a_pb_exec.c` |
| `RECOMP_FFP_POOL=1` | off | Splits fixed-function batches of 192+ unique vertices across the rasteriser's worker threads (same output). Off by default: slower in KOTOR on a busy host. | `nv2a_pb_exec.c` |
| `RECOMP_TEX_BILINEAR=1` | off | CPU rasteriser blends the four nearest texels instead of taking the nearest. | `nv2a_pb_exec.c` |
| `RECOMP_TEX_ANISO=8\|16` | off | Anisotropic filtering on the CPU rasteriser; implies `RECOMP_TEX_BILINEAR`. Slower. | `nv2a_pb_exec.c` |
| `RECOMP_GPU=1`, `=0` | on on Android, off elsewhere | XBA-119 (default on Android). `1`: draw with the GL back end (desktop GL 3.3/4.1 core, GLES 3.0 on Android) instead of the CPU rasteriser; vertex programs run as GLSL vertex shaders (see `RECOMP_GPU_VSH`). `0`: the CPU rasteriser (the A/B switch on Android, launcher › Renderer › Software). Unset: the platform's default (on Android the runtime then sets `RECOMP_GPU=1` itself, so later readers see the choice). Falls back to the CPU rasteriser if no GL context can be made (logged `[GPUGL] no GL context: falling back to the CPU rasteriser`). See [GPU Back End](../technical/gpu-backend.md). | `src/gpu/nv2a_gl.c` |
| `RECOMP_OVERLAY=1` | off | The SDL presenter draws its small debug overlay (FPS, frame time, draws, memory, CPU, battery, temperature, the title's status line) in the top left corner of the game. Off by default everywhere, Android included; the in-game menu's Settings page has its own performance display for normal use. | `src/video/perf_overlay.c` |
| `RECOMP_PREP_INDICATOR=0` | on | Turns off the "preparing data" indicator: while a title writes a burst of at least 8 MB to its utility drive (Z:), as Fable does unpacking its data on the first run, the window title and the overlay's status line say `preparing data <n> MB`, and the log has `[PREP]` lines (start, every 64 MB, the total and duration once 5 s pass without a write). No total or percentage: the format is the title's own (WP7). | `src/kernel/prep_progress.c` |
| `RECOMP_FRAME_LOCK=0` | on | Under frame generation, no lock check: frame generation and its 30 fps cap from the first frame, never stopped. Otherwise (WP4) under `RECOMP_PACING=auto` frame generation waits for the pacer's lock verdict (25 s after the first flip for a title that holds its rate, 50 s for one judged free) without lowering the cap, and stays off for a title locked to an ask above its cap; under `RECOMP_PACING=off` the cap is lowered at once and a title that asks for every vblank and waits at the 30 fps cap on nearly every frame turns it off (XBA-96). [framegen.md](../technical/framegen.md#titles-locked-to-the-flip-rate). | `nv2a_pb_exec.c`, `frame_lock.h` |
| `RECOMP_GPU_FPS_CAP=N` | unset: `RECOMP_PACING=auto` picks the rate; under `RECOMP_PACING=off` 60 on macOS and Android, off elsewhere | Set (the launcher's Frame cap 30 / 60 / Off): turns `RECOMP_PACING=auto` off. With a back end drawing (`RECOMP_GPU=1`), at most N flips per second of host time: after `FLIP_STALL` the executor sleeps until the frame's slot before it walks on. `0` turns it off. Independent of `RECOMP_FLIP_PACING`. The `[GPU] frame cap` report line gives waits, time slept and how late the sleeps woke. | `nv2a_pb_exec.c` |
| `RECOMP_PACING=auto`, `=ask`, `=off` | auto (WP4; `off` or `0`: the fixed cap, the A/B switch) | XBA-107 / XBA-135. `auto`: the cap's rate is chosen at every flip, on either renderer: the title's ask (vblank rate / the presentation interval its swaps carry; IMMEDIATE: the display's rate), then the highest even rate (vblank rate / k, at least 15 fps) the measured frame cost sustains, with hysteresis. A title judged locked to its ask (fixed interval, held it over a 5 s window from 20 s after its first flip; free only after six windows in a row that do not hold) is never divided: logic that advances per flip would run in slow motion. Vblanks keep the console's rate. `ask`: the ask only. Off when `RECOMP_GPU_FPS_CAP` is set (the launcher's Frame cap): that cap wins. Frame generation waits for its verdict (`RECOMP_FRAME_LOCK`). [GPU Back End](../technical/gpu-backend.md#pacing-from-the-title-recomp_pacingauto-xba-107-xba-135). | `nv2a_pb_exec.c`, `frame_pacing.h` |
| `RECOMP_PACING_LOCK=1`, `=0` | judged | Under `RECOMP_PACING=auto`: `1` treats the title as locked to its ask (never divided), `0` as free (divided from the first flip). A/B switch for the verdict (XBA-107). | `frame_pacing.h` |
| `RECOMP_PACING_ALIGN=0`, `=0.N` | on under `RECOMP_PACING=auto`, phase 0.5 | XBA-136. Under `RECOMP_PACING=auto` / `=ask`, the paced flips are kept in phase with the display's refresh: each slot moves (at most a 32nd of a refresh per frame, at once where the schedule restarts) so the frame's present call falls half a refresh from the display's latch edge (Android 13+: the Choreographer frame deadline; Android 11-12: the vsync tick; desktop: the return of a vsynced present that blocked). `0`: off (the absolute schedule as XBA-107 left it). A fraction (`0.3`) sets the present's phase in a refresh. No effect without a display clock (headless, `RECOMP_PRESENT_VSYNC=0` on the desktop, a present that never blocks, a variable-refresh display) or when the frame period is not a whole number of refreshes (60 on 90 Hz). [GPU Back End](../technical/gpu-backend.md#flips-in-phase-with-the-display-xba-136). | `frame_pacing.h`, `display_clock.h`, `display_vsync_android.c`, `fb_present_sdl.c` |
| `RECOMP_DISPLAY_HZ=N` | the SDL window's display | The display's refresh rate for `RECOMP_PACING=auto` (bounds a title that presents IMMEDIATE); the SDL presenter sets it before the guest starts unless it is already set; 60 when unknown (headless). | `fb_present_sdl.c`, `nv2a_pb_exec.c` |
| `RECOMP_GPU_PRIORITY=0` | high where offered | GL back end on EGL: do not ask for a high-priority context (`EGL_IMG_context_priority`). | `src/gpu/nv2a_gl_context.c` |
| `RECOMP_GPU_FAKE_RESET=<frame>[,oom]` | off | Test aid: the GL back end acts as if its context were lost (or `GL_OUT_OF_MEMORY` were raised) at that frame, then re-creates its objects. | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_CULL=0` | on | GL back end: draw both faces (no back-face culling). | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_ZPASS=sync`, `=always`, `=lazy` | deferred | GPU back end: visibility reports (`GET_REPORT`) are written when the GPU's count is in, without waiting (when the walker idles, at the flip and after the present); a report still pending at the head after 300 us of idle calls gets one flush, after 2 ms a wait; past 512 pending the oldest are waited for. `sync` waits at every report; `always` runs no queries and reports 1; `lazy` never flushes or waits from the idle hook and answers 1 (visible) where it would wait. | `nv2a_pb_exec.c` |
| `RECOMP_GPU_FFP=1` | off | GL back end: transform and light fixed-function batches in a vertex shader generated from the fixed-function state (cached guest vertex buffers like vertex programs) instead of on the CPU with a per-batch upload (XBA-134). Off until measured on the device. | `src/kernel/nv2a_pb_exec.c`, `src/gpu/nv2a_gl_vsh.c` |
| `RECOMP_GPU_VSH=0` | on | GL back end: run vertex programs on the CPU (interpreter, near-plane clipping, triangle expansion) instead of as GLSL vertex shaders; for comparing the two paths. | `src/kernel/nv2a_pb_exec.c` |
| `RECOMP_GPU_VBO=0` | on | GL back end: draw vertex-program batches from cached GL buffers holding the guest's vertex bytes in their own formats (validated by a hash once a frame) instead of attributes gathered on the CPU; `0` gathers every batch (the old path, for comparison). See `docs/technical/gpu-backend.md`. | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_MIPS=0` | on | GL back end: upload and sample level 0 of every texture only, as before XBA-120 (and as the CPU rasteriser does), instead of the whole mip chain with the title's mip filter, LOD clamps and bias. | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_S3TC=0` | on where supported | GL back end: decode DXT textures on the CPU instead of uploading them compressed. | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_TEX_RECHECK=frame`, `=switch` | per frame, on texture switch for volatile textures, never over GPU targets | GL back end: `frame` hashes a cached texture's guest bytes only once per frame; `switch` restores the old policy (every frame and every texture switch, targets included). | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_RT_DIRECT=0` | on | GL back end: never sample a render target's GL texture directly; read the target back into guest memory and upload it as a texture instead (the pre-7d path). | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_RT_RELAYOUT_READBACK=1` | off | GL back end: read a render target back when the title switches it between swizzled and linear or between sizes (it is kept on the GPU either way). | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_PRESENT=sync\|host` | async, through guest memory (host present on by itself at `RECOMP_GPU_SCALE` above 1) | `sync`: read the presented surface back at its own flip instead of one frame later through a pixel pack buffer. Frame dumps always read synchronously. `host`: SDL window only: hand the presented target to the window at its real GL size (one frame late, through a pack buffer of its own) instead of converting it into guest memory; the guest bytes are then refreshed only when something needs them (or with `RECOMP_GPU_READBACK=present`). Headless runs and frame dumps keep the guest-memory path. See [GPU Back End](../technical/gpu-backend.md#present). | `src/gpu/nv2a_gl.c`, `src/gpu/nv2a_gl.c`, `src/video/fb_present_sdl.c` |
| `RECOMP_GPU_CPU_WRITES=0` | on | GL back end: do not look for guest CPU writes into render targets. By default a target's guest bytes are compared with a copy taken when guest memory and the target last agreed (seed, read-back) when it is first bound or sampled in a frame, before every read-back and at every present, and the pixels the CPU changed (a movie decoded into the back buffer, a CPU-drawn loading bar) are merged into the GL texture. Off: such writes are not seen, and the target's next read-back overwrites them. | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_READBACK=all\|present` | the presented surface only, on demand under host present | `all`: at every flip also read back every other render target drawn since the last one (for a title whose CPU reads its render targets). `present`: still copy the presented surface into guest memory at every flip, as without host present (for a title whose CPU reads its front buffer). | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_SCALE=N`, `=auto` | 1 | GL back end: internal resolution, a multiple of the 640x480 frame buffer (2, 3, or a decimal 1-4). `auto`: the window's drawable height / 480 (1080 -> 2.25), at most 3 and within `GL_MAX_TEXTURE_SIZE`; 1 headless. Logged as `[GPUGL] internal scale <n>`. Above 1 the window gets host frames (`RECOMP_GPU_PRESENT=host`). Launcher: Settings › Graphics › Internal resolution. See [GPU Back End](../technical/gpu-backend.md#internal-resolution). | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_SCALE_RESOLVE=0` | on | With `RECOMP_GPU_SCALE` above 1: a native target sampling a scaled one reads the scaled texture instead of its native-size resolve (comparison aid). | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_SCALE_SHADOW=1` | off | With `RECOMP_GPU_SCALE` above 1: depth-only targets (shadow maps) scale too. | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_SCALE_TARGETS=WxH[,WxH]` | none | With `RECOMP_GPU_SCALE` above 1: colour targets of these real sizes scale as well as the 640x480 frame buffer. | `src/gpu/nv2a_gl_scale.c` |
| `RECOMP_GPU_SCALE_DUMP=1` | off | At every dumping flip (`RECOMP_FB_DUMP`), also write the presented target at its real (scaled) size as `<prefix>S<n>.bmp`. | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_SCALE_STICKY=0` | on | With `RECOMP_GPU_SCALE` above 1: a surface the policy would leave native keeps the scale of the GL target already held at its address (same pitch, linear, valid colour), so a scaled target re-bound under another size is not dropped to native; `0`: the policy alone (comparison aid). | `src/gpu/nv2a_gl.c`, `nv2a_gl_scale.c` |
| `RECOMP_GPU_SHADER_CACHE=0` | on where the driver offers a binary format | GL back end: do not load or save linked programs in `<save dir>/shadercache/<title id>/` (`glProgramBinary`); every program is compiled at its first use. | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_INVALIDATE=0` | on where supported (GLES 3.0, GL 4.3) | GL back end: do not call `glInvalidateSubFramebuffer` before full clears, nor `glInvalidateFramebuffer` on the scratch depth of a target drawn without a zeta surface when it is left. | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_SIGDUMP=<file>` | off | GL back end, diagnosis: at the first flip where `<file>` exists, print that frame's batch signature table (`[GPUSIG] <sig> n <batches> tris <triangles> : <description>`, one per signature) and its target runs (`[GPUSEQ]`), then delete the file. See [GPU Back End](../technical/gpu-backend.md#batch-signatures). | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_SIGDUMP_KEEP=1` | off | With `RECOMP_GPU_SIGDUMP`: do not delete the file; print the table at every flip for as long as it exists (a window of frames, for `python3 -m tools.frame_outliers sigs <log>`). | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_BATCHLOG=<file>` | off | GL back end, diagnosis: write one binary record per batch to `<file>` (132 bytes: frame, batch number, state signature with and without the target address, colour and depth target, vertex buffer, counts, the four stage textures with format and size, hashes of the vertex program, indices, vertices and constants; layout in `src/gpu/nv2a_gl.c`, reader `tools/frame_outliers/records.py`). About 50 KB a frame for 400 batches: gate it with `RECOMP_GPU_BATCHLOG_FLIPS`. Analyse with `python3 -m tools.frame_outliers batches <file>`. See [GPU Back End](../technical/gpu-backend.md#checking-for-one-frame-glitches). | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_BATCHLOG_FLIPS=<flag file>` | every frame | With `RECOMP_GPU_BATCHLOG`: record only the frames that start while `<flag file>` exists (`1`: every frame), like `RECOMP_FB_DUMP_FLIPS`. | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_SIGSKIP=<file>` | off | GL back end, diagnosis: do not draw batches whose signature (hex, as `RECOMP_GPU_SIGDUMP` prints it) is listed in `<file>`; re-read every 16 flips, so it can be edited while the game runs. | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_VERBOSE=1` | off | GL back end: log render targets as they are created and a statistics line every 60 frames (default every 600); on EGL, the EGL version, vendor and extension list once. | `src/gpu/nv2a_gl.c`, `nv2a_gl_context.c` |
| `RECOMP_STRIP_ALT_WINDING` | off | Odd triangles of a strip keep the strip's winding, as on hardware. | `nv2a_pb_exec.c` |
| `RECOMP_PB_CONST_FAST=0` | on | Sends every `SET_TRANSFORM_CONSTANT` word through the general dispatcher. | `nv2a_pb_exec.c` |
| `RECOMP_RASTER_TEST` | off | Bring-up aid: draws a magenta triangle after every clear, to prove the rasteriser and presenter work. | `nv2a_pb_exec.c` |
| `RECOMP_FFP_TRACE` | off | Matrix and first vertices of the first fixed-function batches. | `nv2a_pb_exec.c` |
| `RECOMP_VSH_TRACE=N` | 0 | Prints N vertex-program batches. | `nv2a_pb_exec.c` |
| `RECOMP_VSH_DUMP` | off | Vertex-program constant traffic. | `nv2a_vsh_interp.c` |
| `RECOMP_VSH_DECODED=0`, `RECOMP_VSH_CHECK=1` | decoded | The vertex-program interpreter runs programs decoded once into an instruction list; `RECOMP_VSH_DECODED=0` decodes the fields at every vertex (the old path), `RECOMP_VSH_CHECK=1` runs both and compares them. `RECOMP_VSH_DUMP` forces the field path. Read once. | `nv2a_vsh_interp.c` |
| `RECOMP_FRAME_TRACE` | off | Traces one frame in detail. | `nv2a_pb_exec.c` |
| `RECOMP_FRAME_TRACE_METHODS` | off | With the above: every method of the traced frame, less the bulk uploads. | `nv2a_pb_exec.c` |
| `RECOMP_TEX_STATE` | off | Texture state per report. | `nv2a_pb_exec.c` |
| `RECOMP_TEX_DUMP=<prefix>` | off | Writes each distinct texture as a BMP, first use only. | `nv2a_pb_exec.c` |
| `RECOMP_TEX_DUMP_EVERY=N` | 0 | Dumps every Nth texture bind instead. | `nv2a_pb_exec.c` |
| `RECOMP_FB_DUMP=<prefix>` | off | Writes a BMP per report: the frame the next flip presents (complete), or, for a title that has not flipped yet or stopped flipping, the draw surface as it is; before the first flip also the first frames that draw geometry. On POSIX hosts, while the video overlay is up, the overlay's picture over the last frame instead (what the screen shows). | `nv2a_pb_exec.c`, `fb_present.c`, `pvideo_posix.c` |
| `RECOMP_FB_DUMP_FLIPS` | off | `1` dumps every flip; any other value names a file whose existence turns dumping on. | `nv2a_pb_exec.c` |
| `RECOMP_FIND_NAN` | off | Scans for NaN matrices at every report. | `nv2a_pb_exec.c` |
| `RECOMP_FIND_QUAD` | off | Scans once for quad vertices. | `nv2a_pb_exec.c` |

## Video output

| Variable | Default | What it does | Where |
|---|---|---|---|
| `RECOMP_FB_WINDOW` | off (Android: on) | Opens a window showing the frame buffer. On Android the app shell sets it unless `recomp.cfg` says `RECOMP_FB_WINDOW=0`; `RECOMP_HEADLESS=1` keeps it off everywhere. | `fb_present.c`, `fb_present_sdl.c` |
| `RECOMP_FB_VA=<va>` | follows the draw surface | Pins the window to one guest address. | `fb_present.c` |
| `RECOMP_FB_WINDOW_DUMP_EVERY=N` | 0 | With `RECOMP_FB_DUMP`: dumps what the window shows every N frames. | `fb_present.c` |
| `RECOMP_WINDOW_SCALE=1..8` | 2 | Client-area scale. | `fb_present.c`, `fb_present_sdl.c` |
| `RECOMP_FULLSCREEN` | off | Starts fullscreen (Win32 and SDL presenters). | `fb_present.c`, `fb_present_sdl.c` |
| `RECOMP_ASPECT=auto\|4:3\|16:9` | auto | SDL window: per-frame aspect modes. `4:3`: every frame 4:3, as before. `auto`: 4:3 unless the title declares wide 3D frames (`xbox_FramebufferWindowSetWide(1)`, e.g. Fable's widescreen patch); then frames with at least `RECOMP_ASPECT_MIN_TRIS` triangles are stretched to 16:9 (WIDE) and the rest stay 4:3, and letterboxed 16:9 movies are cropped and shown at 16:9 (ZOOM). `16:9`: the same for a title that declares wide frames, otherwise every frame stretched to 16:9; the desktop window opens at 854x480 per `RECOMP_WINDOW_SCALE`. Class changes are logged `[FBWIN] aspect NORMAL -> WIDE (...)`; screenshots (`xbox_FramebufferDumpBmp`) take the shown shape (1440x1080 WIDE -> 1920x1080), `RECOMP_FB_DUMP` stays raw. Titles read it with `xbox_AspectSetting()`. Launcher: Settings › Graphics › Aspect ratio. See [GPU Back End](../technical/gpu-backend.md#aspect-modes). | `src/video/aspect_policy.c`, `fb_present_sdl.c` |
| `RECOMP_ASPECT_MIN_TRIS=N` | 5000 | Triangles a frame needs to be shown at 16:9 when the title declared wide frames (Fable: menus 210-1622, scenes 178,000-375,000); a change of class takes two frames. | `src/video/aspect_policy.c` |
| `RECOMP_ASPECT_ZOOM=0` | on | Turns off ZOOM (letterboxed movies cropped to 16:9) when the aspect is wide. | `src/video/aspect_policy.c` |
| `RECOMP_HEADLESS` | off | No window and a dummy audio driver. | `fb_present_sdl.c`, `apu_sdl2.c` |
| `RECOMP_CLOSE_KEEPS_RUNNING=1` | off | Closing the window only closes it instead of exiting the process. | `fb_present.c` |
| `RECOMP_D3D_PRESENT` | off | Presents through a D3D11 swap chain. | `gpu_present.c` |
| `RECOMP_PS_TEXT_CACHE=0` | on | D3D11 path (Windows): compile every generated combiner pixel shader again instead of reusing the one compiled from identical HLSL text. | `src/d3d/d3d8_combiners.c` |
| `RECOMP_PRESENT_VSYNC` | on | `0` turns vsync off. The GBA, PS1 and N64 runtimes read it too (`host_window_env_vsync`, XBA-181). | `gpu_present.c`, `fb_present_sdl.c`, `src/video/host_window.c` |
| `RECOMP_PRESENT_FILTER` | default | Sampler filter for the D3D11 present. | `gpu_present.c` |
| `RECOMP_PVIDEO=0` | on | POSIX hosts: turns off the PVIDEO overlay's scan-out (films a title plays through `D3DDevice_UpdateOverlay`, composed over the last frame at 640x480 and published as a host frame each vblank a bank is pending; logged `[PVIDEO] overlay on: ...`). The STOP handshake in the NV2A ack pass stays. | `src/video/pvideo_posix.c` |
| `RECOMP_FMV_HOST` | off | Plays a movie through the host player when the title asks to. A substitute for the title's own output. | `kernel_bridge.c` |
| `RECOMP_FMV_DUMP=<prefix>` | off | Writes two frames of a movie as BMP evidence. | `video_pump.c` |
| `RECOMP_XMV=auto\|native\|skip` | auto | The runtime's XMV decoder in place of the XDK's `XMVDecoder_GetNextFrame`. At Prepare, `auto` binds it where `tools.xmv.find_api` finds the function with high confidence (XBA-199: a generated `recomp_xmv_bind.c`, no title layer) and `native` binds nothing. At run time, in a bound title: `auto` (or unset, `decode`) decodes the movie, `native` runs the title's own lifted decoder, `skip` (or `0`) ends every movie at once (`XMV_ENDOFFILE`, the old skip override). No effect where nothing is bound. See [xmv.md](xmv.md#binding-at-prepare-xba-199). | `src/video/xmv_hle.c`, `tools/xmv/find_api.py` |

## Frame generation

Only in a build configured with `-DXBOXRECOMP_LSFG=ON` (POSIX hosts); see [Frame Generation](../technical/framegen.md#pacing). With `RECOMP_LSFG=1` the SDL presenter hands every frame to the pacer and shows real and generated frames at their due times ([Presenter](../technical/framegen.md#presenter)); headless runs count them and log `[LSFG] real ... fps, shown ... fps` every 5 s. The GBA, PS1 and N64 window (`host_window`, XBA-186) reads the same keys: distinct pictures to the pacer, generated frames in place of the repeats, no cap change ([Console window](../technical/framegen.md#console-window)). On Android the launcher writes `RECOMP_LSFG`, `RECOMP_LSFG_MULT`, `RECOMP_LSFG_PERF` (Settings › Graphics › All consoles › Frame generation) and `RECOMP_LSFG_DIR` (« Choose Lossless.dll ») to `recomp.cfg`; for a console game ShellActivity exports them, with the game's own on / off (its Options) over `RECOMP_LSFG`.

| Variable | Default | What it does | Where |
|---|---|---|---|
| `RECOMP_LSFG=1` | off | Frame generation on. Under `RECOMP_PACING=auto` (the default) it waits for the title's lock verdict (25 to 50 s; frames shown as without it, `LSFG WAITING` in the overlay) and stays off for a title locked to 60 (WP4, `RECOMP_FRAME_LOCK`). Once on, the effective `RECOMP_GPU_FPS_CAP` is min(cap, display rate / multiplier) (a cap of `0` becomes display rate / multiplier: 30 at 60 Hz, 2x). `RECOMP_GPU_PRESENT=sync` is forced from the start. Both stay for the run if the pacer later falls back to pass-through. A title profile can still force it off (`env_force`, [Forced by a title](#forced-by-a-title)). | `src/framegen/framegen_policy.h`, `nv2a_pb_exec.c`, `src/gpu/nv2a_gl.c` |
| `RECOMP_LSFG_MULT=2..4` | 2 | Frames shown per real frame (2: one generated frame between two real ones). Console window: per distinct picture (2: a 30 fps game shows 60, a 20 fps one 40; 3: a 20 fps one 60). | `framegen_policy.h` |
| `RECOMP_LSFG_DISPLAY_HZ=N` | 60 | Display refresh rate used by the cap rule. Unset, the windowed presenter sets it to the display's rate when that is below 60 Hz (a faster display keeps 60); the console window does the same for its pacing (it never caps the game). | `framegen_policy.h`, `src/video/fb_present_sdl.c`, `src/framegen/framegen_console.c` |
| `RECOMP_LSFG_MAX_WIDTH=N` | 1280 | Console window: a picture wider than N (an internal resolution above 1x) is box-filtered by a whole factor to fit before generation, and every frame is shown at that size while frame generation runs (2560 x 1920 -> 1280 x 960). | `src/framegen/framegen_console.c` |
| `RECOMP_LSFG_BUDGET_MS=N` | cap period / multiplier (16.7 ms at 30 fps, 2x) | Generation cost above which a frame counts as over budget; 30 in a row (or 30 failed generations) turn frame generation off: `[LSFG] off: <reason>`. | `src/framegen/framegen_pacer.c` |
| `RECOMP_LSFG_FAKE=1` | off | Use the fake generator (CPU linear blend of the two frames, no Vulkan) instead of the LSFG bridge: headless runs and tests. | `framegen_pacer.c` |
| `RECOMP_LSFG_FAKE_MS=N` | 0 | With the fake generator: sleep N ms per generated pair, to exercise the budget fallback. | `src/framegen/framegen_fake.c` |
| `RECOMP_LSFG_DIR=<dir>` | the current cache of the default base | The LSFG bridge's shader cache directory (`<base>/<key>`, made by `tools/lsfg/lsfg_translate` or the app's "Choose Lossless.dll"). The bridge's VkPipelineCache is kept there too. Bridge builds only (`XBOXRECOMP_LSFG_VK_SRC`). | `src/framegen/lsfg_generator.c`, `lsfg_cache.h` |
| `RECOMP_LSFG_DLL=<path>` | unset | Desktop: your own Lossless.dll, extracted into the default cache base when `RECOMP_LSFG_DIR` is unset. The DXBC shaders still need one `lsfg_translate` run. | `lsfg_generator.c` |
| `RECOMP_LSFG_CACHE=<dir>` | `~/Library/Caches/xboxrecomp/lsfg` (macOS), `$XDG_CACHE_HOME/xboxrecomp/lsfg` or `~/.cache/xboxrecomp/lsfg` | The default cache base. | `lsfg_cache.c` |
| `RECOMP_LSFG_FLOW=0.25..1` | 1 | Flow scale (lower is cheaper and blurrier motion). | `lsfg_generator.c` |
| `RECOMP_LSFG_PERF=1` | off | LSFG 3.1 performance shader set (about 4x cheaper). | `lsfg_generator.c` |
| `RECOMP_LSFG_FP16=auto\|0\|1` | auto | Embedded FP16 SPIR-V when the DLL has it and the device has `shaderFloat16` (translated DXBC is always full precision). | `lsfg_generator.c` |
| `RECOMP_LSFG_GPU=<index>` | best device | Vulkan physical device index for the bridge. | `src/framegen/lsfg_bridge.cpp` |

## Custom Vulkan driver (N64 RDP)

A Vulkan driver the player supplied (Mesa Turnip on an Adreno) for the N64 Vulkan RDP, opened through libadrenotools
(`libxba_vkdriver.so`, Android arm64 only; [third_party/adrenotools](../../third_party/adrenotools/README.md)). The app
sets these before the game starts (`ShellActivity`, N64 games only), from the game's driver: its own choice (its
Options or the in-game menu: `files/settings/vk_driver_<KEY>`, a driver id or `system`), else Settings › Graphics ›
Vulkan driver (`files/settings/vk_driver`), else the system driver; the drivers are installed under
`files/vk_drivers/<id>/` with their `meta.json` ([app-shell.md « Vulkan drivers »](../android/app-shell.md#vulkan-drivers));
they can also be written to
`recomp.cfg` by hand for a test (with a driver chosen in the app its values win; with the system driver chosen the
app unsets them and such lines would apply, so remove them after the test). Chain:
custom driver, then the system driver, then the software RDP, each step logged; the start line names the driver
(`n64: Vulkan RDP on <GPU> (paraLLEl-RDP, <shaders>; driver <driverName> <driverInfo>, custom <id>|system)`).
See [consoles/README.md](../../consoles/README.md) (N64 RDP) and `consoles/n64/runtime/n64_vk_driver.h`.

| Variable | Default | What it does | Where |
|---|---|---|---|
| `RECOMP_VK_DRIVER_DIR=<dir>` | unset (system driver) | Absolute, app-private directory of the driver, `<files>/vk_drivers/<id>/` (a missing trailing `/` is added). Set: the driver is the RDP's first Vulkan loader. | `consoles/n64/runtime/n64_vk_driver.c` |
| `RECOMP_VK_DRIVER_LIB=<file>` | unset | The driver's library in that directory (`meta.json` `libraryName`, e.g. `vulkan.purple.so`); a name, no `/`. | `n64_vk_driver.c` |
| `RECOMP_VK_DRIVER_ID=<id>` | the directory's last component | The driver's id, for the log, the trial marker and `last_start`. | `n64_vk_driver.c` |
| `RECOMP_VK_HOOK_DIR=<dir>` | the directory `libmain.so` was loaded from | The app's native library directory (`ApplicationInfo.nativeLibraryDir`), where adrenotools' hook libraries are. | `n64_vk_driver.c` |
| `RECOMP_VK_DRIVER=system` | unset | Forces the system driver whatever the others say. | `n64_vk_driver.c` |
| `RECOMP_VK_CACHE_DIR=<dir>` | unset (nothing kept) | Where the Vulkan RDP keeps compiled shaders between starts (created when missing): `pipelines-<pipelineCacheUUID>.bin`, the VkPipelineCache (read before the start-up check; written when it grew, after the check, every 10 s, at pause and at exit; a truncated or foreign file is ignored and replaced), and `mesa/`, Mesa's disk cache (`MESA_SHADER_CACHE_DIR` is set to it unless already set). The app sets `<cache>/vk_cache/<driver id or system>/` for every N64 game. | `consoles/n64/runtime/n64_vk_cache.c` |

Files next to the drivers (written only when the driver directory's parent is named `vk_drivers`):
`vk_drivers/trying` holds the id while the driver is being opened and checked; found at the next start with the same
id (the driver crashed the process), the driver is skipped until the app removes the file (the player picks a driver
again). Each start's outcome goes to `vk_drivers/last_start`, one line `<id> <status>` (the last start with any
custom driver), and to `vk_drivers/<id>.status`, one word `<status>` (that driver's own last start; only for an id of
`[A-Za-z0-9_-]`, which the app's always are). Status:

| Status | Meaning |
|---|---|
| `ok` | the RDP runs on the custom driver |
| `open_failed` | it could not be opened (bad variables, no library file, `libxba_vkdriver.so` / adrenotools refused it, no `vkGetInstanceProcAddr`, or the loader answers nothing): the system driver was used |
| `unsupported` | it was opened, but paraLLEl-RDP cannot run on it (no Vulkan device, the device « lacks what paraLLEl-RDP needs », or every shader try failed): the system driver was used. The app says « Not compatible with the N64 renderer » |
| `not_loaded` | adrenotools fell back to the device's own driver (logcat tag `hook_impl`) |
| `crashed` | skipped because of the marker |
| `failed` | written by a runtime before `open_failed` / `unsupported` for either; the app still reads it |

The app reads `<id>.status` first and falls back to `last_start` when its line names that driver (a file from an older
runtime), so with per-game choices each driver keeps its own outcome.

## Audio

See the [APU](../../src/apu/README.md) and [DirectSound](../../src/audio/README.md) module references.

| Variable | Default | What it does | Where |
|---|---|---|---|
| `RECOMP_APU_TRACE` | off | Voice statistics and host playback accounting. | `apu_core.c`, `apu_vp.c`, `apu_mmio_hook.c` |
| `RECOMP_AUDIO_HOST` | off | Host playback accounting without the rest of the trace. | `apu_core.c` |
| `RECOMP_APU_RING` | off | Ring monitor: finds a streaming writer that falls behind the play cursor. | `apu_vp.c` |
| `RECOMP_APU_SOLO=<voice>` | off | Mutes every other voice, to find where a sound comes from. | `apu_core.c` |
| `RECOMP_APU_VOICE_DUMP=<voice\|all>` | off | Dumps voices as raw PCM to `RECOMP_APU_VOICE_DUMP_FILE`. | `apu_vp.c` |
| `RECOMP_AUDIO_WAV=<path>` | off | Records exactly what the device is given. | `apu_core.c` |
| `RECOMP_AUDIO_WAV_SECS=N` | unlimited | Caps the recording. | `apu_core.c` |
| `RECOMP_APU_HRTF` | on | `0` turns HRTF for 3D voices off. | `apu_vp.c` |
| `RECOMP_APU_MIXDOWN=routed\|sum\|two` | routed | How the 32 mixbins become stereo, the GP not being emulated (XBA-122): `routed` folds each bin by its DirectSound role (front unity, centre and back -3 dB into their sides, LFE -6 dB into both, XTLK as front/back, the I3DL2 send -6 dB into both when no reverb runs, FX sends even left / odd right); `sum` is the mixdown before XBA-122 (every bin at unity, even bins left, odd right: the centre in the left channel only); `two` reads bins 0/1 only. [APU DSP](../technical/apu-dsp.md). | `apu_mix.c` |
| `RECOMP_APU_MIXDOWN_ALL` | on | `0` = `RECOMP_APU_MIXDOWN=two` (bins 0/1 only); `RECOMP_APU_MIXDOWN` wins when both are set. | `apu_mix.c` |
| `RECOMP_APU_REVERB=off\|on\|<preset>` | off | Native I3DL2-style reverb fed from the I3DL2 send (mixbin 10), in place of the reverb of DirectSound's effects image, which does not run. `on` = `generic`; other names are the I3DL2 presets (`room`, `hallway`, `stonecorridor`, `cave`, `arena`, `hangar`, `sewerpipe`, `underwater`, ...). The preset is fixed: the parameters a title writes into GP memory are not read. With it on, the I3DL2 send has no dry path. About 0.8 us per 0.67 ms frame on x86-64. [APU DSP](../technical/apu-dsp.md). | `apu_mix.c`, `apu_dsp.c` |
| `RECOMP_APU_REVERB_IMAGES=<hash>[,<hash>...]` | any image | With the reverb on, runs it only while the GP holds one of these effects images (the 16-hex hashes of the `[APU-DSP] GP image loaded` lines and the status file's `apu_dsp.gp_image`); under any other image it is bypassed and counted (`reverb_bypassed`). | `apu_dsp.c` |
| `RECOMP_APU_DSP_DUMP=<dir>` | off | Writes each distinct GP/EP memory window the title loads (X/Y/P, 64 KB) to `<dir>/gp-<hash>.bin` / `ep-<hash>.bin`, for study. That is the title's data: keep it out of the repository. | `apu_dsp_watch.c` |
| `RECOMP_APU_DSP_ACK` | auto | DSP doorbell acknowledgement: `auto` derives it, `0`/`off` disables, or a list of addresses. | `apu_dsp.c` |
| `RECOMP_APU_GPRST_POLL` | on | Reads the GP reset word (GPRST, VA `0xFE83FFFC`, plain memory) back from the aperture every APU frame and pauses the doorbell ack while it holds the GP in reset (XBA-138). `0`: the title's GPRST stores are not seen, the ack answers regardless (before XBA-138). | `apu_dsp.c` |
| `RECOMP_APU_PRIORITY` | on | `0` leaves the APU frame thread at the default priority (otherwise nice -16 on Linux/Android, QoS user-interactive on macOS). | `apu_core.c` |
| `RECOMP_APU_EP_CANARY` | on | Keeps the XDK EP program's canary (`0x00CCCCCC` at EP P:6, VA `0xFE85A018`) while the APU frame thread runs; DirectSound resets the whole APU (10 ms stall) on every `DoWork` that reads anything else. `0` leaves the word alone. | `apu_dsp.c` |
| `RECOMP_AUDIO_BUF_COUNT`, `RECOMP_AUDIO_BUF_SAMPLES` | built-in | Device buffer count and size. `RECOMP_AUDIO_BUF_SAMPLES` (128..4096) is also the GBA, PS1 and N64 runtimes' device buffer in sample frames (`host_audio`, default 1024, XBA-181). | `apu_sdl2.c`, `apu_xaudio2.c`, `src/audio/host_audio.c` |

## Input

See [xbox_input](../../src/input/README.md).

| Variable | Default | What it does | Where |
|---|---|---|---|
| `RECOMP_KEYBOARD` | on in a title project (XDK default) | Keyboard acts as a controller. `0` is off. | `keyboard.c`, `usb_gamepad.c` |
| `RECOMP_PAD_MAP` | unset (the fixed mapping) | Remaps every pad's controls (XBA-131; the Android launcher's Controls screen writes it into `recomp.cfg`). `control=source` entries for the Xbox controls that differ from the fixed mapping, comma separated: controls `a b x y black white left_trigger right_trigger start back left_thumb right_thumb dpad_up dpad_down dpad_left dpad_right`, sources SDL's positional names `south east west north left_shoulder right_shoulder left_trigger right_trigger left_stick_click right_stick_click back start dpad_*`, a stick direction `left_stick_up` … `right_stick_right`, or `none`; plus `left_stick=` / `right_stick=` `left`, `right` or `none`. Example `a=east,b=south,black=left_trigger`. Unknown entries are ignored (logged). Unset or empty: exactly the fixed mapping (the A/B switch). SDL backend only. | `xinput_device.c`, `input_map.c` |
| `RECOMP_GBA_PAD_MAP`, `RECOMP_PS1_PAD_MAP`, `RECOMP_N64_PAD_MAP` | unset (the console's fixed mapping) | The GBA, PS1 and N64 runtimes' pad mapping (XBA-158 / XBA-168 / XBA-174; the Android Controls screen writes the console's into `recomp.cfg` and sets a game's own in the environment). `target=source[+source]` or `target=none`: targets GBA `a b l r start select dpad_*`, PS1 `cross circle square triangle l1 r1 l2 r2 l3 r3 start select dpad_*`, N64 `a b z l r start dpad_* c_up c_down c_left c_right`; sources as `RECOMP_PAD_MAP`'s; `left_stick=` / `right_stick=` (PS1), `stick=` (N64) `left`, `right` or `none`; N64 `range=1..127`, `deadzone=0..90` (percent, over `RECOMP_N64_STICK_RANGE` / `RECOMP_N64_DEADZONE`). Example `a=east,b=south`. Unknown entries are skipped (logged). The on-screen pad is never remapped. | `src/input/console_pad.c` |
| `RECOMP_MKB` | off | Keyboard-and-mouse layer; tuned by `RECOMP_MKB_SENS`, `_DZ`, `_ACC`, `_FULL`, `_INVERT_Y`, `_SWAP_STICKS` or an `RECOMP_MKB_INI` file. | `mkb_input.c` |
| `RECOMP_KEY_TRACE` | off | Prints the first 40 key-downs. | `keyboard_sdl.c`, `fb_present.c` |
| `RECOMP_INPUT_DIAG` | off | Reports whether the input chain is wired. | `usb_gamepad.c` |
| `RECOMP_RUMBLE` | on | `0` turns rumble off: the Xbox pads, the N64 Rumble Pak and the PS1 DualShock motors (`host_pad_rumble_env`, XBA-181). | `ohci.c`, `src/input/host_pad.c` |
| `RECOMP_USB` | on in a title project (XDK default) | Enables the USB controller model; `0` (or empty) is off. The title project calls `xbox_OhciInit()` after the memory layout (the template does). | `ohci.c` |
| `RECOMP_USB_PADS=1..4` | unset: the pads follow the controllers | Unset (XBA-123): one emulated pad per port that has a controller (`xbox_InputPortMask`: a host pad assigned to that port, or on port 1 the keyboard or the touch overlay), plugged in one at a time as controllers come and pulled out 1 s after one goes, each on its own root port, so a title's XAPI sees the insertion or removal on that controller port (`usb_hotplug.h`). With no controller at all since the start, pad 1 stays plugged (as before); the bring-up inputs (`RECOMP_PAD_PRESS`, `_SCRIPT`, `_LIVE`) keep pad 1 plugged too. Set: that many pads, always plugged, never removed: the behaviour before XBA-123 (`1` = the old default; the A/B switch). Log: `[OHCI0] pad N plugged in / pulled out on root port P (controller port N)`. | `ohci.c`, `usb_hotplug.c` |
| `RECOMP_USB_PORT=0..3` | 2 with four ports | Root port pad 1 appears on; pad n is on the n-th port after it. | `ohci.c` |
| `RECOMP_PAD_PORTS` | unset (first come) | Which controller each port prefers (XBA-123; the Android Controls screen writes it into `recomp.cfg`): `1=045e:0b13,2=2dc8:3106`, a port and a controller's USB vendor:product in hex, each entry optionally followed by `:<name>` (the app's label, ignored here). At the first look the controllers present are packed from port 1: the pinned ones in port order, then the built-in pad, then the rest; a pinned controller that is absent leaves no hole. Later a controller keeps its port across a disconnect (the port waits for it); a newcomer takes a free port nobody has used. Log: `[INPUT] port N: <name> (vvvv:pppp) connected`. SDL backend. | `xinput_device.c`, `input_ports.c` |
| `RECOMP_PAD_BUILTIN` | set by the Android app | The handheld's own pad (`vvvv:pppp`; the app sets it from the input device that is not external): port 1 unless `RECOMP_PAD_PORTS` pins another controller there. | `xinput_device.c`, `input_ports.c` |
| `RECOMP_USB_HC`, `RECOMP_USB_NDP` | model default | Host-controller selection and port count. | `ohci.c` |
| `RECOMP_OHCI_CATCHUP` | on (needs the gate and safe points) | When the controller thread is more than 16 ms overdue and the driver has written ControlListFilled or BulkListFilled, a guest thread walks the lists at its release point and runs the USB interrupt there (log: `[OHCI0] controller thread N ms overdue; lists serviced on the guest thread`). Keeps a starved host thread from letting per-frame control transfers (rumble) pile up past XAPI's descriptor budget (KOTOR, K-8c). `0` restores the controller thread alone; an A/B switch kept for one release. | `ohci.c`, `kernel_hal.c` |
| `RECOMP_USB_TRACE`, `RECOMP_USB_STATS` | off | Controller trace; completions every 5 s. | `ohci.c` |
| `RECOMP_PAD_LIVE=<file>`, `RECOMP_PAD_SCRIPT`, `RECOMP_PAD_PRESS` | off | Test input: a file read live, a script, or a periodic press. | `usb_gamepad.c` |

## Consoles (GBA, PS1, N64)

The GBA, PS1 and N64 runtimes (`consoles/*/runtime`, [consoles/README.md](../../consoles/README.md)) and their
desktop players (`gba_play`, `ps1_play`, `n64_play`). A player's command-line flag wins over the variable. Shared
keys first (XBA-181: one key for every console, the Android Settings write them; a console's own key wins over the
shared one), then each console's own. `RECOMP_PRESENT_VSYNC`, `RECOMP_AUDIO_BUF_SAMPLES` and `RECOMP_RUMBLE` above
apply to them too; the pad mappings are under [Input](#input).

| Variable | Default | What it does | Where |
|---|---|---|---|
| `RECOMP_SCALE=aspect\|integer\|stretch` | aspect | How every console's picture fills the window: `aspect` the largest rectangle of its shape, `integer` the largest whole multiple of its size (of its height for the 4:3 PS1 / N64), `stretch` the whole window. `RECOMP_GBA_SCALE`, `RECOMP_PS1_SCALE`, `RECOMP_N64_SCALE` win for their console. | `src/video/host_window.c` (`host_window_env_scale`) |
| `RECOMP_FILTER=nearest\|linear` | the console's: GBA nearest, PS1 / N64 linear | How the picture is sampled: `nearest` sharp pixels, `linear` smooth. `RECOMP_GBA_FILTER`, `RECOMP_PS1_FILTER`, `RECOMP_N64_FILTER` win for their console; every key takes both values. | `host_window.c` (`host_window_env_filter`) |
| `RECOMP_GBA_SCALE`, `RECOMP_PS1_SCALE`, `RECOMP_N64_SCALE` | `RECOMP_SCALE` | One console's scale (`aspect`, `integer`, `stretch`); `--scale` on its player. | `consoles/*/runtime/*_runtime.c` (`*_runtime_display`) |
| `RECOMP_GBA_FILTER`, `RECOMP_PS1_FILTER`, `RECOMP_N64_FILTER` | `RECOMP_FILTER` | One console's filter (`nearest`, `linear`); `--filter` on its player. | `*_runtime.c` (`*_runtime_display`) |
| `RECOMP_SHADER=none\|sharp\|scanlines\|crt\|lcd` | none | Picture shader of every console's window (XBA-195; Android: Settings › Graphics › Picture shader, a game's own in its Options), drawn at the window's resolution after upscaling and frame generation: `sharp` sharp bilinear (square pixels at a non-integer scale), `scanlines`, `crt` (scanlines and an aperture grille), `lcd` (a pixel grid). Falls back to none, logged once, when the shader does not build. Screenshots stay the raw picture. `RECOMP_GBA_SHADER`, `RECOMP_PS1_SHADER`, `RECOMP_N64_SHADER`, `RECOMP_XBOX_SHADER` win for their console. The Xbox presenter draws the same presets (XBA-203; `lcd` draws none there, scanlines follow its 480 / 576 / 720 lines, line pairs below 720). Details: [HOST_LAYER.md](../../src/platform/HOST_LAYER.md#picture-shaders-xba-195). | `present_shaders.c` (`present_shader_env`), `host_window.c`, `fb_present_sdl.c` |
| `RECOMP_XBOX_SHADER=none\|sharp\|scanlines\|crt` | `RECOMP_SHADER` | The Xbox presenter's picture shader (XBA-203), drawn inside SDL's renderer after scaling and frame generation; needs SDL's opengl / opengles2 renderer (macOS: asks for opengl). | `fb_present_sdl.c` |
| `RECOMP_SHADER_STRENGTH=0..100` | the preset's: scanlines 50, CRT 60, LCD 50 | How dark the scanlines, CRT lines and mask or LCD grid are; sharp ignores it. | `host_window.c` |
| `RECOMP_SHADER_CURVATURE=0..100` | 0 | The CRT preset's curvature (barrel and vignette; the Android app's Slight is 30, Strong 70). | `host_window.c` |
| `RECOMP_SHADER_GHOSTING=0..100` | 0 | The LCD preset's ghosting: the previous picture blended in (up to half at 100; the app's Slight is 30, Strong 60). | `host_window.c` |
| `RECOMP_SHADER_TEST_FAIL=<preset>` | unset | Test only: that preset's shader is built from broken source, to check the fallback to none (`tests/host_layer`). | `host_window.c` |
| `RECOMP_UPSCALE=1\|2\|4\|8` | 1 | Internal resolution of the PS1 and N64 (XBA-184 / XBA-185; Android: Settings › Graphics › Internal resolution, a game's own in its Options): the console draws N times its resolution each way, sharper edges, textures unchanged. PS1: 1, 2 or 4 (8 is 4, 3 is 2), the software GPU's shadow VRAM (below); `8` is for the N64 Vulkan renderer. The GBA ignores it. `RECOMP_PS1_UPSCALE` wins for the PS1. | `ps1_runtime.c` (`ps1_runtime_upscale`) |
| `RECOMP_PS1_UPSCALE=1\|2\|4` | `RECOMP_UPSCALE` | The PS1's internal resolution (`--upscale` on `ps1_play`): polygons, rectangles and lines drawn again into a shadow VRAM of 1024N x 512N (4 MB at 2x, 16 MB at 4x) that the display reads; the native VRAM the game reads back is the same as at 1x. The picture (and `--screenshot`) is N times wider and higher. About 4.5 times the GPU's work at 2x, 14 at 4x, shared with worker threads (next row); try 2x on a handheld. | `ps1_gpu.c` (`ps1_gpu_set_upscale`) |
| `RECOMP_PS1_GPU_THREADS=0..3` | by cores: 3 from 8, 2 from 4, 1 from 2 | Worker threads sharing the upscaled shadow pass of big primitives with the emulation thread (rows interleaved; the picture is the same whatever the count). `0`: the emulation thread alone. Only above 1x. | `ps1_gpu.c` (`ps1_gpu_set_threads`) |
| `RECOMP_SAVE_DIR=<dir>` | `$XDG_DATA_HOME/xrecomp/save`, else `~/.local/share/xrecomp/save` | The players' save folder when `--save-dir` is not given (the macOS launcher sets it per title): `<dir>/gba-<KEY>/`, `ps1-<KEY>/`, `n64-<KEY>/`. The Android app passes its own folder (`*_runtime_set_save_dir`). The Xbox runtime reads it too ([Files](#files-discs-and-the-xbe)). | `consoles/*/runtime/tools/*_play.c` |
| `RECOMP_HOST_GL=core` | GLES 3.0 first | Skips the OpenGL ES 3.0 context and takes the 3.2 core one (the macOS path, checked on Linux). | `host_window.c` |
| `RECOMP_FF_SPEED=2..16\|max` | 4 | Fast-forward of the GBA, PS1 and N64 runtimes (XBA-182, `recomp_states.h`): console frames per shown frame while it is on (the in-game menu's toggle, the hotkeys: Tab held on a desktop, Back + right trigger held on a pad by default, `RECOMP_HOTKEYS`), their audio averaged down by as much; `max` runs as many frames as fit in a display frame, silent. | `src/platform/recomp_states.c` |
| `RECOMP_HOTKEYS` | the usual chords | The pad hotkeys of the GBA, PS1 and N64 runtimes and the Android in-game menu (XBA-190, `src/platform/recomp_hotkeys.h`; the Android Controls screens write it into `recomp.cfg`, a game's own through its settings). `action:chord` entries for what differs, comma separated: actions `ff_hold` (default `back+right_trigger`), `ff_toggle`, `qsave` (`back+right_shoulder`), `qload` (`back+left_shoulder`), `slot_next`, `slot_prev`, `rewind` (`back+left_trigger`, held, XBA-194), `menu` (`back+start`, the Android shell's, held ½ s; an Xbox title's only hotkey), `save1`..`save4`, `load1`..`load4` (none); a chord is `modifier+button`, a button alone, or `none`, with `RECOMP_PAD_MAP`'s source names or `a b x y lb rb lt rt l3 r3 select up down left right`. A chord fires once when both its buttons are down, its buttons reach the game as nothing while it is held. Example `qsave:back+north,ff_toggle:l3+r3`. Bad entries are skipped (logged `[INPUT] hotkeys: …`). | `src/platform/recomp_hotkeys.c` |
| `RECOMP_HOTKEYS_KEYS` | `ff_hold:tab`, `rewind:r`, `save1..4:f1..f4`, `load1..4:shift+f1..f4` | The desktop players' keys for the same actions (`gba_play`, `ps1_play`, `n64_play`): `action:[ctrl+][alt+][shift+]key`, keys `a`..`z`, `0`..`9`, `f1`..`f12`, `tab space enter escape backspace insert delete home end pageup pagedown up down left right minus equals grave comma period slash semicolon apostrophe leftbracket rightbracket backslash`, or `none`. A key fires with exactly its modifiers held. Example `qsave:f5,qload:f7,ff_toggle:f9`. The PS1's F6 (next disc) stays fixed. | `src/platform/recomp_hotkeys.c` |
| `RECOMP_REWIND=0\|1` | on for the GBA and PS1, off for the N64 | Rewind of the GBA, PS1 and N64 runtimes (XBA-194, `src/platform/recomp_rewind.h`): a capture of the machine every few frames into a memory-capped ring of XOR deltas; while the `rewind` hotkey is held (Back + left trigger on a pad, R on a desktop) each shown frame steps one capture back, silent; letting go plays on from there. The N64 is off by default (a capture is a 4 / 8 MB state, 1.5 to 2 ms on a desktop, plus a Vulkan RDP drain). The Android app writes it from Settings › « All consoles » › Rewind, a game's own through its settings. | `src/platform/recomp_rewind.c` |
| `RECOMP_REWIND_MB=8..1024` | 64 | The rewind ring's memory cap in MB (the deltas, the newest capture and its coding buffer): the oldest captures go first. | `src/platform/recomp_rewind.c` |
| `RECOMP_REWIND_FRAMES=1..60` | 2 (GBA), 3 (PS1), 4 (N64) | Console frames between two rewind captures (at most one per shown frame while fast-forwarding); a step back goes that far. | `src/platform/recomp_rewind.c` |
| `RECOMP_CHEATS_FILE=<path>` | unset (no cheats) | The cheat list of the GBA, PS1 and N64 runtimes (XBA-197, `src/platform/recomp_cheats.h`): `[name]`, `enabled=1\|0`, an optional `format=` (`auto`, `raw`, `gsa`, `gsa-raw`, `arv3`, `arv3-raw`, `cb`, `gs`), the code's lines; the enabled codes run after every console frame, and the file is read again when it changes (every 30 shown frames). A missing file is no cheat until it appears. The Android app sets it to the game's `files/settings/game/<KEY>.cheats` (in-game menu « Cheats »); `--cheats FILE` on the desktop players. Formats: [consoles/README.md « Cheats »](../../consoles/README.md). | `src/platform/recomp_cheats.c` |
| `RECOMP_GBA_BIOS=<path>` | the built-in BIOS | A 16 KB GBA BIOS image (never shipped); `--bios` on `gba_play`. | `gba_runtime.c` |
| `RECOMP_GBA_FRAMES=N` | unbounded | The Android entry (`gba_runtime_start`): stop after N frames (checks). | `gba_runtime.c` |
| `RECOMP_PS1_BIOS=<path>\|hle` | the app's asset, else `build/ps1-bios/openbios.bin`, else the HLE BIOS | A 512 KB PlayStation BIOS image; `hle` the built-in HLE BIOS (no image, XBA-163). | `ps1_runtime.c`, `tools/ps1_run.c` |
| `RECOMP_PS1_FASTBOOT=0` | on | A disc boots through the BIOS shell instead of jumping to its EXE (`--no-fast-boot`). | `ps1_runtime.c` |
| `RECOMP_PS1_PAD=analog\|digital` | digital | The pad in port 1: a DualShock in analog or digital mode (`--pad`); Back + L3 + R3 switches while playing. | `ps1_runtime.c` |
| `RECOMP_PS1_PLAYERS=1\|2` | 1 | `2`: host pad 1 drives port 1, host pad 2 port 2; otherwise every pad drives port 1 (`--players`). | `ps1_runtime.c` |
| `RECOMP_PS1_FRAMES=N` | unbounded | The Android entry (`ps1_runtime_start`): stop after N frames (checks). | `ps1_runtime.c` |
| `RECOMP_N64_CROP_EDGES=0` | on (cropped) | Shows the VI's horizontal guard band (the black first 8 and last 7 VI pixels of each line, `n64_vi.c`) as thin black strips left and right. By default 8 VI pixels are cut from each side (4 columns of a 320-wide picture) and the rest fills the 4:3 shape (2.5 % wider), as a television hid them in its overscan. Window only: `--screenshot` and `n64_run --ppm` keep the whole VI picture (`--edges` on `n64_play`). | `n64_runtime.c` |
| `RECOMP_N64_PAK=controller\|rumble\|none` | controller | The accessory in the controllers: a controller pak (saved as `pakN.mpk`), a rumble pak (drives the host pad's rumble unless `RECOMP_RUMBLE=0`) or none (`--pak`). | `n64_runtime.c` |
| `RECOMP_N64_PLAYERS=1..4` | 1 | With 2 or more, host pad N drives controller N; otherwise every pad drives controller 1 (`--players`). | `n64_runtime.c` |
| `RECOMP_N64_STICK_RANGE=N` | 80 | The stick's value at full travel on one axis (the diagonals follow the octagonal gate). | `n64_runtime.c` |
| `RECOMP_N64_DEADZONE=0..90` | 8 | The stick's radial dead zone, in percent. | `n64_runtime.c` |
| `RECOMP_N64_FRAMESKIP=auto\|0\|N` | auto | `auto` skips the picture of frames the host is late for (new pictures only, never two in a row), `0` never, `N` shows one new picture in N + 1 (`--frameskip`). A skipped frame still runs and makes its sound. | `n64_runtime.c` |
| `RECOMP_N64_PRESENT_ALWAYS=1` | 0 | Make, convert and upload the VI's picture on every VI, as before; by default only when it changed (`n64_display_changed`), the window's texture drawn again otherwise. | `n64_runtime.c` |
| `RECOMP_N64_RDP=vulkan\|software\|auto` | auto: Vulkan on Android (when built in), software elsewhere | The RDP back end (`vk` / `gpu`, `sw` / `cpu` too). The Vulkan one falls back to the software one when it fails its check. | `n64_rdp_select.c` |
| `RECOMP_N64_UPSCALE=1\|2\|4\|8` | `RECOMP_UPSCALE`, else 1 | The N64's internal resolution (XBA-185): the Vulkan RDP draws at that multiple and its VI scans the picture out at it (1280 x 480 at 2x); RDRAM keeps the 1x picture. Lowered (logged) when the device's storage buffer range or memory cannot hold it (128 MB range: 4x at most). Other numbers take the power of two below. The software RDP stays 1x (logged). | `n64_rdp_select.c`, `n64_rdp_vk.cpp` |
| `RECOMP_N64_UPSCALE_DOWNSCALE=0..3` | as many as keep the picture at most 2560 wide (8x: 1) | Halvings of the upscaled scanout (filtered: super-sampling), to keep the readback small. | `n64_rdp_vk.cpp` |
| `RECOMP_N64_UPSCALE_NATIVE_TEXRECT=0` | on | Upscales texture rectangles too (by default they are drawn at 1x: 2D and HUD art made for 1:1 texels). | `n64_rdp_vk.cpp` |
| `RECOMP_N64_UPSCALE_NATIVE_LOD=1` | off | Computes texture LOD as at 1x when upscaled (games that pick textures through LOD). | `n64_rdp_vk.cpp` |
| `RECOMP_N64_UPSCALE_SSREADBACK=1` | off | The RDRAM copy the game reads is the upscaled picture averaged down, not the 1x one (not bit-exact). | `n64_rdp_vk.cpp` |
| `RECOMP_N64_RDP_THREADS=N` | 2 on Android, else `XBR_N64_RDP_THREADS`, else 1 | The software RDP's renderer threads (`--rdp-threads`): they draw while the emulation thread runs on, the result is the same for any count. | `n64_runtime.c`, `n64_rdp.c` |
| `RECOMP_N64_RDP_DEFER=0\|auto\|<percent>` | auto | The Vulkan RDP's deferred DP interrupt (`n64_core.c` « Deferred DP interrupt »): for a game that shows a picture every 2 VIs or more with one SYNC_FULL each, the DP interrupt comes after the next VI interrupt and the GPU is waited for there, not inside the frame that queued the work; with it, the upscaled scanout starts at the VI_ORIGIN change. `0` off (both), `auto` the deadline 1/16 VI after the VI interrupt, a number that offset in percent of a VI (1..100). | `n64_core.c` |
| `RECOMP_N64_PRESENT_DIRECT=0` | on | The upscaled picture through the XRGB buffer and the window's screenshot copy as before, instead of straight from the Vulkan RDP's scanout buffer to the texture (`n64_display_rgba`). | `n64_runtime.c` |
| `RECOMP_N64_SCANOUT_EARLY=0` | on | The early upscaled scanout alone off (`n64_rdp_scanout_prefetch`), to compare. | `n64_vi.c` |
| `RECOMP_N64_RDP_DEFER_SOFTWARE=1` | 0 | Tests: the software RDP's DP interrupt goes through the deferral policy too (drawn at once, only the interrupt held). | `n64_rdp_select.c` |
| `RECOMP_N64_RDP_CHECK=0` | on | Skips the Vulkan RDP's start-up draw check (only a pipeline failure then falls back). | `n64_rdp_vk.cpp` |
| `RECOMP_N64_RDP_UBERSHADER=1` | off on Qualcomm's driver | Uses the Vulkan RDP's ubershader on Qualcomm's proprietary driver anyway (games come out black there, so it goes software). | `n64_rdp_vk.cpp` |
| `RECOMP_N64_TTY=1` | off | Prints the IS-Viewer text a game writes (`--tty`). | `n64_runtime.c` |
| `RECOMP_N64_FRAMES=N` | unbounded | The Android entry (`n64_runtime_start`): stop after N frames (checks). | `n64_runtime.c` |

## Network

See [Network: a console with no cable](../technical/network.md).

| Variable | Default | What it does | Where |
|---|---|---|---|
| `RECOMP_NIC` | on | The MCPX Ethernet controller model (XBA-117): registers at 0xFEF00000 trapped and answered as a controller with no cable (PHY link down, autonegotiation never completes, transmit refused with carrier lost, no interrupt), `PhyGetLinkState` returns 0. `0` is the behaviour before: the registers are plain memory and `PhyGetLinkState` says link up. | `xbox_nic.c` |
| `RECOMP_NIC_TRACE=1` | off | Prints the first 400 NIC register accesses (offset, size, value): what a title's network stack does at boot. | `xbox_nic.c` |

## RECOMP_CFG_SKIP (XBA-188)

Comma- or space-separated variable names whose `recomp.cfg` lines `recomp_cfg_load` ignores
(`src/platform/recomp_cfg.c`). The Android launcher sets it when a game's own settings chose the runtime's default for
an option `recomp.cfg` sets (docs/android/app-shell.md « Settings per game »). Unset: every line loads.
