# Prepare on the device: the title pack

The app prepares a title from the player's own disc, BananaNative style: pick the disc, tap **Prepare**. The app
runs `tools.prepare` with an embedded CPython (identify, analyse, lift: the same Python pipeline as on a desktop),
compiles the generated C with an embedded clang + lld (LLVM 19, one multicall binary `libxbacc.so`) into a **title
pack** (`libtitle.so` + `pack.json`), and dlopens it next to the shell's `libmain.so`.

This page is the contract between the three parts. Field names, flags and line formats here are frozen: change them
only together with the app (service, loader) and the toolchain.

| Part | Owner | Code |
|---|---|---|
| `tools.prepare --android`, compile driver, pack writer | this page | `tools/prepare/{android,compile,pack,progress}.py` |
| Toolchain: CPython, `libxbacc.so` (clang, ld.lld), sysroot, builtins | embedded-prepare | assets + `jniLibs` |
| Service that runs prepare, `status.json`, pack loader | prepare-app | `android/app` |

Nothing from the game leaves the device: the pack is built from the player's disc, on the player's device.

## Where packs live

```
<files dir>/packs/<TITLEID>/libtitle.so
<files dir>/packs/<TITLEID>/pack.json
```

`<files dir>` is the app's **internal** storage (`Context.getFilesDir()`). External storage
(`/sdcard/Android/data/<app>/files`) is mounted `noexec`: dlopen of a library there fails. An external `packs/` folder
is only an import/export drop: the app copies a pack in from there after checking `lib_sha256`, never loads it in
place (planned; not implemented yet, see « The loader »). A game's own app (app-shell.md « A game's own app ») has
its own private storage: it takes XRecomp's pack over instead of preparing again
([Handing a pack to a game's own app](#handing-a-pack-to-a-games-own-app)).

`TITLEID` is the XBE's title ID in 8 upper-case hex digits (`4D5300D1`). A Game Boy Advance game's pack is
`packs/gba-<KEY>/` ([GBA packs](#gba-packs-xba-157-xba-158)), a PlayStation disc's `packs/ps1-<KEY>/`
([PS1 packs](#ps1-packs-xba-168)), a Nintendo 64 cartridge's `packs/n64-<KEY>/` ([N64 packs](#n64-packs-xba-174)).

**Atomic publish.** A pack is built in `packs/<TITLEID>.partial/`; `pack.json` is written last (temp file, fsync,
rename), then the directory is renamed to `packs/<TITLEID>`. An existing pack is first renamed to
`packs/<TITLEID>.old` and removed after the switch (POSIX cannot rename a directory over a non-empty one); the next
prepare puts `.old` back if a kill fell between the two renames. So a reader sees a whole pack or none:

- the loader only looks at `packs/<TITLEID>` (8 hex digits, no suffix) that has a `pack.json`;
- `*.partial` and `*.old` belong to prepare: the app ignores them and may delete them when no prepare runs.

## pack.json

```json
{"format": 1, "title_id": "4D5300D1", "title": "...", "xbe_sha256": "...", "abi": "arm64-v8a", "shell_abi": 1,
 "toolkit": "<commit>", "toolkit_sha256": "...", "lib": "libtitle.so", "lib_sha256": "...", "lib_size": 123456,
 "entry": "recomp_title_main", "cc": "clang 19.0.1 -O1", "built_at": "2026-11-10T21:10:35Z",
 "env_force": {"RECOMP_LSFG": "0"}}
```

| Field | Type | Meaning |
|---|---|---|
| `format` | int | 1. A loader refuses a format it does not know. |
| `title_id` | string | 8 upper-case hex digits, = the directory name. |
| `title` | string | The XBE's title name, for display. |
| `xbe_sha256` | hex | The XBE the pack was built from. The app compares it with the game folder's `default.xbe` before a start ([When a pack is stale](#when-a-pack-is-stale)). |
| `abi` | string | `arm64-v8a`. |
| `shell_abi` | int | The shell ABI the pack was linked against. The shell exports its own integer; **the loader refuses a mismatch** (the app then offers to prepare again). |
| `toolkit` | string | The toolkit commit (`git rev-parse HEAD`, or `tools/VERSION` in the assets), `unknown` if neither. Informative: the commit of the run that last relinked the pack. |
| `toolkit_sha256` | hex | The toolkit digest (`tools/prepare/toolkit.py`) the pack was lifted with (XBA-127). The app compares it with its own ([When a pack is stale](#when-a-pack-is-stale)). Packs from before it lack it. |
| `lib` | string | `libtitle.so`, relative to the pack directory. |
| `lib_sha256`, `lib_size` | hex, int | Of `lib`. The loader checks both before dlopen. |
| `entry` | string | `recomp_title_main`: `int recomp_title_main(const recomp_storage *)` (templates/new-game/src/main.c). |
| `cc` | string | Compiler and level, informative (`clang 19.0.1 -O1`). |
| `built_at` | string | ISO 8601, UTC (`Z`). |
| `env_force` | object | Optional; only when the title profile declares it. `title.json`'s `"env_force"`, checked and copied: `RECOMP_[A-Z0-9_]+` names to strings (printable ASCII), 16 at most. The loader sets each one over `recomp.cfg` before the dlopen (log `[TITLE] <TID> forces KEY=VALUE (title profile)`); the launcher shows a forced `RECOMP_LSFG=0` on the frame generation row. Not a validity check: a pack without it, or with entries the loader refuses, still loads. See [Forced by a title](../runtime/environment-variables.md#forced-by-a-title). |

| `title_layer` | string | Optional (profile-free WP6): only for a title the toolkit has optional enhancements for (`profiles/titles/<id>`). `on`: they went in; `off`: they were left out (the app's Settings › General › Enhancements was off for the title, so its Prepare had `XBA_NO_TITLE_LAYER=1`). When it differs from the switch, Home says « Enhancements changed · prepare again » and offers Prepare again next to Play, as for an older toolkit. The row is shown only for a pack that has the field. |

| `images` | object | Optional (XBA-82): the disc's other images that `default.xbe` names (at most 8), disc path to library: `{"game.xbe": "libimage-1.so", "intro.xbe": ""}`. `""`: named, not prepared yet; it is prepared on its first launch ([Images](#images-xba-82)). An image built is one sub-run in `<work>/images/<path>/`, shown inside step 4 as « game.xbe: ... »; one whose analysis or compile fails is left out (« left out » line): default.xbe still runs. Shells from before ignore it. See [multi-xbe.md](../technical/multi-xbe.md#android). |

| `prepare` | object | Optional (XBA-128): what the Prepare that wrote this pack cost, measured as it ran (`tools/prepare/metrics.py`). See below. Informative: no check reads it. |

Readers ignore fields they do not know. `tools/prepare/pack.py` (`read`, `check`) is the reference for the checks.

**`prepare`** (written by the run that links the pack; a run that finds the pack up to date leaves it as it was):

```json
"prepare": {"total_s": 372.4, "steps_s": {"identify": 2.1, "analyse": 61.0, "lift": 48.2, "compile": 255.0,
            "images": 0.0, "pack": 6.1}, "skipped": ["parse", "disasm"], "jobs": 4, "opt": "2", "files": 163,
            "compiled": 163, "c_mb": 256.0, "obj_mb": 149.0, "pack_mb": 61.2, "cpus": 8,
            "peak_rss_mb": {"max": 589, "p90": 302, "median": 163}, "tools_peak_mb": 900, "held_s": 40,
            "thermal": {"samples": 74, "max": 3, "severe_s": 95, "jobs_min": 1, "temp_c_max": 41.5,
                        "curve": [[0, 1], [60, 2], [120, 3]]}}
```

| Key | Meaning |
|---|---|
| `total_s`, `steps_s` | Wall time of this run, and of each step. A resumed run counts only itself; `skipped` names the stamps it found done (`parse`, `disasm`, `func_id`, `abi`, `lift`, `link`). |
| `jobs`, `opt`, `files`, `compiled` | The compile: jobs started with, `-O`, C files, how many compiled now (the rest came from the object cache). |
| `c_mb`, `obj_mb`, `pack_mb`, `cpus` | Sizes of the C, the objects, the libraries in the pack; the CPU count. |
| `peak_rss_mb` | Each compile's own peak resident set (`wait4`'s `ru_maxrss`, one per clang): max, p90, median. Absent where the host has no `wait4`. |
| `tools_peak_mb` | The largest pipeline tool (disasm, lift: `RUSAGE_CHILDREN` before the compile). |
| `held_s` | Wall time the device log held every compile (`jobs` 0: critical heat, or a background Prepare while the game ran). |
| `thermal` | From the device log (below): samples, the hottest status, seconds at severe or above, the fewest jobs allowed, the battery's hottest temperature, up to 24 points of the curve `[seconds into the run, status]`. Absent without a device log (a host). |

The launcher shows it in Manage (« Preparation took 6 min 12 s · 4 jobs · -O2 », « Memory per compile job », « Hottest
during it »; `PrepareFacts`), and the library check adds it to each title's report line (`prepare_s` when the check did
not time a Prepare itself, `prepare_peak_mb`, `pack_mb`, `prepare_jobs`, `prepare_opt`, `prepare_thermal_max`,
`prepare_temp_c_max`, `prepare_steps_s`; `peak_mem_mb` stays the game process's own peak).

### What a pack exports

`libtitle.so` is linked with `-Bsymbolic` (the title's calls to itself bind inside it) and `--no-undefined` against
`libmain.so`, so every symbol it uses is exported by the shell or by libc/libm/libdl/liblog. Its dynamic symbols
include `recomp_title_main` and the symbols the shell defines as weak forwarders (`recomp_lookup`,
`recomp_lookup_manual`, and per title `g_fe_cursor`, `fe_menu_get_pb_state`). The toolkit inside `libmain.so` calls
`recomp_lookup` (thread start, APCs); a library dlopened later does not override `libmain.so`'s own weak definition,
so the loader fetches these with `dlsym` and the forwarders call through them (« The loader » below).

## The command

```
python -m tools.prepare SOURCE --android --work DIR --pack-dir DIR --cc PATH [--cc-prefix-arg clang]
       --sysroot DIR --shell-lib PATH [--jobs N] [--opt 2] [--shell-abi N]
       [--ld PATH [--ld-prefix-arg ARG]] [--builtins PATH] [--python PATH]
       [--extra-src FILE.c]... [--include-dir DIR]... [--device-log FILE] [--force]
       [--image PATH]... [--all-images]
```

| Flag | Device value |
|---|---|
| `SOURCE` | The game folder (holds `default.xbe`; the app's ISO import extracted it), or a `default.xbe`, or an image (`.iso`/`.xiso`: only `default.xbe` is read out). |
| `--work DIR` | Per title, internal storage, e.g. `<files dir>/prepare/<TITLEID>`. Holds the copy of the XBE, the analysis, the C project, the object cache, `stamp.json`, `prepare.log`, and `discovered.jsonl`, which the title's runs write ([discovery.md](../technical/discovery.md)). Keep it between runs: it is what makes a rerun cheap. Several hundred MB for a large title (the C alone is ~250 MB for one). |
| `--pack-dir DIR` | `<files dir>/packs` |
| `--cc PATH` | `<nativeLibraryDir>/libxbacc.so` |
| `--cc-prefix-arg clang` | The multicall applet. With a prefix arg, the linker is `PATH ld.lld` unless `--ld` says otherwise. On a host: the NDK's `clang` and no prefix (ld.lld is taken next to it). |
| `--sysroot DIR` | The NDK-layout sysroot from the assets (`usr/include`, `usr/lib/aarch64-linux-android/30/{crtbegin_so.o,crtend_so.o,libc.so,...}`), plus `lib/clang/19/`: the embedded clang's resource directory (headers, builtins, libunwind), which it cannot find by itself; `-resource-dir` then points there ([toolchain.md](toolchain.md#the-on-device-toolchain-embedded-prepare)). |
| `--builtins PATH` | `libclang_rt.builtins-aarch64-android.a`; default: from `<sysroot>/lib/clang/<v>/lib/linux/` when the sysroot carries it, else asked from the compiler (`-print-libgcc-file-name`). |
| `--shell-lib PATH` | `<nativeLibraryDir>/libmain.so`: the pack links against it (DT_NEEDED `libmain.so`). |
| `--shell-abi N` | The integer the shell exports (default 1), written to `pack.json`. |
| `--jobs N` | Parallel compiles; 0 or absent = `min(5, (MemAvailable − 3 GB) / 512 MB)`, at least 1, at most the CPU count (512 MB per job since XBA-209: see Measurements). |
| `--opt N` | clang `-O` level, default 2 (see Measurements). |
| `--python PATH` | The interpreter for the pipeline steps; default `$XBA_PYTHON`, else `sys.executable`. |
| `--extra-src`, `--include-dir` | Development: an extra C file in the pack, an extra `-I`. |
| `--device-log FILE` | `<work>/device.jsonl`, the app's device log ([below](#the-device-log-xba-128)): compiles allowed now, thermal samples. Absent: `--jobs` all the way. |
| `--force` | Redo every step (the object cache still applies). |
| `--image PATH` | Also build this other image of the disc now (its disc path, `game.xbe`). The app does not pass it: it writes `<work>/images.want` ([Images](#images-xba-82)). |
| `--all-images` | Build every other image `default.xbe` names now (up front, as before XBA-82's lazy prepare). Not used by the app. |

The steps, each one subprocess (`python -m tools.<tool>`) or a pool of compiler processes, never the
`multiprocessing` module (no `sem_open` on Android):

| # | id | Does | Stamp |
|---|---|---|---|
| 1 | `identify` | finds `default.xbe`, copies it to `<work>/xbe/`, `xbe_parser` → title ID, title profile; `xbe_ident` → XDK version, engines, profile layers in `<work>/ident.json` (shown in `msg`; never fails the step) | `parse` |
| 2 | `analyse` | `disasm --lean` (no ASM listing, compact JSON), `func_id`, `abi_analysis` | `disasm`, `func_id`, `abi` |
| 3 | `lift` | the project from `templates/new-game` + the profile layers (`profiles/`, [profiles.md](../technical/profiles.md)), then `tools.recomp --split-bytes 1000000` (below) | `lift` (= profile digest) |
| 4 | `compile` | every C file to an object, largest first, cached | the object cache |
| 5 | `pack` | `ld.lld` → `packs/<TID>.partial/libtitle.so`, `pack.json`, publish | `link` |

Stamps live in `<work>/stamp.json` with the XBE's sha256 and the toolkit digest (`toolkit_sha256`, not the commit:
an app update that changed only Java or the shell keeps the steps done; XBA-127): another XBE or another toolkit
starts over; a redone step drops the stamps after it. Objects are written to a temp name and renamed. So a run killed
at any point (SIGKILL included) loses at most the sub-step or the objects in flight.

### Progress lines

On stdout, one line per event (everything else, the tools' own output included, goes to `<work>/prepare.log`):

```
XBA_PROGRESS step=<i>/<n> id=<identify|analyse|lift|compile|pack> pct=<0-100> [eta=<s>] msg=<rest of line>
XBA_RESULT ok pack=<dir>
XBA_RESULT error code=<UPPER_SNAKE> msg=<text>
```

- `msg` is last and greedy: it takes the rest of the line, spaces and `=` included. Other values hold no spaces.
- Unknown keys are ignored (new ones may appear before `msg`). `eta` (seconds) is optional.
- `pct` is per step. For `analyse` and `lift` it is a time-based guess in v1 (the tools print no counts): it moves
  towards the end of the sub-step and jumps when it ends. For `compile` it is files done / total; `eta` comes from the
  bytes compiled so far.
- The last line is exactly one `XBA_RESULT`. `pack=` is the published directory.
- `tools/prepare/progress.py` has the writer and a reference reader (`parse_line`).

Example (the conformance stub):

```
XBA_PROGRESS step=1/5 id=identify pct=100 msg=SYNTH (title ID 12345678), XDK 5849, no known engine
XBA_PROGRESS step=2/5 id=analyse pct=55 msg=disasm done in 0 s
XBA_PROGRESS step=4/5 id=compile pct=40 eta=12 msg=2/5 files (2 compiled)
XBA_PROGRESS step=5/5 id=pack pct=100 msg=pack 12345678, 19272 bytes
XBA_RESULT ok pack=/data/user/0/<app>/files/packs/12345678
```

| Error code | When |
|---|---|
| `BAD_ARGS` | a required flag is missing (`--work`, `--pack-dir`, `--cc`, `--sysroot`, `--shell-lib`) |
| `NO_XBE` | no `default.xbe` in the source |
| `NO_ROM`, `BAD_ROM` | a GBA ROM that cannot be read, or is empty or over 32 MB ([GBA packs](#gba-packs-xba-157-xba-158)) |
| `NO_DISC`, `BAD_DISC` | a PS1 disc that cannot be read, or whose boot EXE is missing or not one, or a `.chd` hunk the reader cannot decode ([PS1 packs](#ps1-packs-xba-168)) |
| `NO_ROM`, `BAD_ROM` (N64) | an N64 ROM that cannot be read, or is under 4 KB or over 64 MB ([N64 packs](#n64-packs-xba-174)) |
| `BAD_XBE` | `xbe_parser` cannot read it |
| `ANALYSE_FAILED`, `LIFT_FAILED` | a pipeline tool failed; details in `prepare.log` |
| `COMPILE_FAILED` | a C file did not compile; `msg` = file and first error |
| `LINK_FAILED` | ld.lld failed; `msg` = the first undefined symbols, if that is why |
| `PACK_FAILED` | writing or publishing the pack failed |
| `NO_SPACE` | ENOSPC anywhere |
| `CANCELLED` | SIGTERM or SIGINT |
| `BAD_PROFILE` | the title profile's `title.json` has an `env_force` entry that is not allowed |
| `INTERNAL` | anything else (a bug); the traceback is in `prepare.log` |

Exit status: 0 ok, 1 error, 130 cancelled. On SIGTERM/SIGINT prepare kills its children, prints
`XBA_RESULT error code=CANCELLED`, and exits 130; stamps and finished objects stay, the published pack is untouched.
The app should still start prepare in its own process group and signal the group, so that a SIGKILL leaves nothing
behind either.

## Compile and link

The same driver runs on the device and on hosts/CI (with the NDK's clang): no CMake. Per file:

```
<cc> [clang] --target=aarch64-linux-android30 --sysroot=<sysroot> -O<opt> -g0 -fPIC -std=gnu11 -w
     -DXBOXRECOMP_SHELL_TITLE=1 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DNDEBUG [-DXBA_TITLE_PROFILE=1]
     -I<project>/src -I<project>/src/recomp -I<project>/src/recomp/gen
     -I<toolkit>/include -I<toolkit>/src -I<toolkit>/src/{kernel,platform,audio,apu,nv2a,d3d,input,usb,video,selftest}
     -c <file.c> -o <work>/obj/<key>.o
```

`WIN32_LEAN_AND_MEAN`, `NOMINMAX` and `NDEBUG` are what the CMake build gives the same files (the template's
definitions, a Release-type build). Sources: `src/main.c`, `src/recomp_manual.c`, `src/title/*.c` (the profile;
`XBA_TITLE_PROFILE` when there is one), `src/extra/*.c`, `src/recomp/gen/*.c`.

**Object cache.** `key = sha256(source path, source bytes, flags, compiler id, headers digest)`; compiler id = the
`--version` line + the binary's size and mtime; headers digest = every `.h` under the include directories. A second
run with nothing changed compiles 0 files. After a successful link, objects not used by it are deleted (storage).

**A re-lift recompiles what changed (XBA-209).** The discovery loop re-prepares a title with a few more functions
([discovery.md](../technical/discovery.md)); until XBA-209 that recompiled all of it. Two things make it cheap:

- *Stable chunks.* `tools.recomp --split-bytes 1000000` cuts the C into chunks of 0.75–1.5 MB (one function larger
  than that is a chunk alone), ending a chunk after a function chosen by a hash of its address (content-defined
  chunking, `translator.stable_chunks`): a new function changes its own chunk, and the boundaries after it fall back
  in place within a chunk or so (`--split 250`, every 250 functions, shifted every chunk after it). Each chunk is
  self-contained: `#include "recomp_types.h"` and the declarations of the functions it names (all `void name(void)`),
  not the 1 MB `recomp_funcs.h`.
- *The key of the lifted C.* For `src/recomp/gen/*.c` the headers digest counts `recomp_funcs.h` without its
  `void name(void);` lines: a generated file that compiled names only declared functions, all with that signature,
  so another function's declaration cannot change its object. `main.c`, `src/title` and `src/extra` keep the whole
  header in their key.

On the synthetic title of « Measurements », one seeded function: 160 of 160 files recompiled before, 4 of 97 now
(its chunk, the dispatch table, the stubs, the entry): 4.5 s of compile instead of ~80 s. The desktop Prepare (CMake) gains the same: its chunks no longer include `recomp_funcs.h`, and
unchanged ones keep their bytes and mtime (`sync_generated`).

**Link**, ld.lld directly, what the NDK's `clang -shared` does plus our options:

```
ld.lld -EL --fix-cortex-a53-843419 -z now -z relro -z max-page-size=16384 -z noexecstack --hash-style=gnu
       --eh-frame-hdr --build-id=sha1 -m aarch64linux -shared -soname libtitle.so -Bsymbolic --no-undefined
       -o libtitle.so <sysroot>/usr/lib/aarch64-linux-android/30/crtbegin_so.o <objects>
       -L<api libdir> -L<libdir> -L<shell dir> -l:libmain.so -lm -ldl -llog -lc <builtins> [libunwind.a] -ldl
       <sysroot>/usr/lib/aarch64-linux-android/30/crtend_so.o
```

`max-page-size=16384`: 16 KB-page devices (Android 15+). `--no-undefined` turns a symbol the shell does not export
into a link error on the device rather than a dlopen failure at start.

## Images (XBA-82)

A disc whose `default.xbe` launches another XBE of the disc ([multi-xbe.md](../technical/multi-xbe.md)) needs one
library per image. The on-device Prepare builds them **lazily**: the first Prepare stays as fast as for a one-image
disc.

- **First Prepare**: `default.xbe` only. Step 4 (`other_images`) lists the images `default.xbe` names and finds on the
  disc, and writes each one into `pack.json` `images` with library `""` (« N more image(s) prepared on their first
  launch »).
- **What a Prepare builds** (`Run.wanted_images`): the images asked with `--image`, the lines of `<work>/images.want`
  (disc paths, any case, `\` or `/`), and the images the current pack already has a library for (a Prepare again, for
  a new toolkit or a self-healing round, keeps them). `--all-images` builds all of them.
- **First launch of an image not prepared**: the kernel finds it in `images.map` and writes the launch data; the
  shell's image handler (`shell_main.c`) sees `RECOMP_PACK_IMAGE_LAZY` (`xba_pack_image_state`), writes
  `<work>/image.pending` and calls `ShellActivity.prepareImage`, which opens the launcher (`EXTRA_PREPARE_IMAGE`);
  logcat `PACK_IMAGE_LAZY <path>`. The process then ends with the launch data left in the save dir.
- **Home** says « Goes on in <image> · not prepared yet » with **Prepare <image>** first and Play second. Prepare
  adds the image to `images.want` and starts the title's Prepare (the same queue as any Prepare, `PendingImage`).
  When it is done and the image is in the pack, the game starts again in it: `image.pending` becomes `image.next`
  (pack_loader.c loads that image's library) and the launch data reaches it, as after a quick reboot. Play instead
  (or any other start of the title) drops `image.pending` and the launch data: `default.xbe` starts from the
  beginning. An image the Prepare left out is dropped the same way, with a note.
- **A game's own app** has no Home to offer it on: it starts the image's Prepare at once and goes into the image.

Why not up front: a launcher disc's other images are often as large as `default.xbe` (a second game, a menu), so
building them all would double the first Prepare for an image the player may never reach. Up front stays available
from the command line (`--all-images`), not as an app setting.

## The device log (XBA-128)

While a Prepare runs, `PrepareMonitor` (in the `:prepare` process) appends a line to `<work>/device.jsonl` every 5 s
(the file is new for each Prepare):

```
{"t": 1731456000, "thermal": 3, "jobs": 1, "temp_c": 41.5}
```

`t` epoch seconds, `thermal` PowerManager's thermal status (0 none … 6 shutdown), `jobs` the compiles allowed from now
on, `temp_c` the battery's temperature, `game: true` when a background Prepare holds for the game. tools.prepare reads
the last whole line before each compile starts (`metrics.jobs_now`, never above `--jobs`); running compiles are never
killed. `DeviceGuard` (shared with the library check, XBA-130) decides:

| Thermal status | Compiles at once |
|---|---|
| none, light, moderate (0–2) | all the jobs (`--jobs`) |
| severe (3) | 1 |
| critical and above (4–6) | 0: held, the running ones finish, none starts until it cools down |
| any, a background Prepare while the game runs | 0 |

So a hot device keeps going slowly instead of being stopped. The library check no longer cancels a Prepare when the
device is hot: it pauses (nothing new starts after it) and lets the Prepare drop its own jobs.

## When the Prepare's process dies (XBA-128)

What a kill costs, step by step: identify, analyse (`disasm`, `func_id`, `abi`) and link have their stamps, so a
finished sub-step is never redone; the compile keeps every finished object (cache, temp name + rename), so only the
files in flight are lost; **the lift is one step**: a kill during it redoes the whole lift (it writes all the C at
once; on the real title of « Measurements » the lift is under a minute of a 10–30 min Prepare).

The lift stays one step on purpose (XBA-138). `tools.recomp` translates every function in memory first and writes
the C only at the end (`translator.py`: `recomp_funcs.h`, then the chunks, through `write_if_changed`), so a kill
almost always lands before the first file and a per-file resume would save only the write phase, seconds of the
lift's ~50 s (48.2 s of 372 s in the `pack.json` example above: 13 % of the Prepare, at most one lift lost per kill).
Resuming the translation itself would mean checkpointing the translator's whole-program state (labels, indirect-call
tables, the header that lists every function), for less than a minute. And nothing is lost downstream: a redone lift
writes the same bytes, and the object cache is keyed by content, so the compile that follows finds every object it
had. What the lift does guarantee: its stamp is dropped before it empties `gen/` and written only after
`tools.recomp` succeeds, so a half-written `gen/` is never taken as a finished lift, even when a later run comes back
to the stamp's earlier key (the « Enhancements » switch turned off, then on again;
`test_android_killed_lift_never_passes_as_up_to_date`). The desktop Prepare lifts into `gen.new` and syncs, same
guarantee.

The app's side:

- The service returns `START_REDELIVER_INTENT`: when the system kills `:prepare` (low memory, the phantom process
  killer), Android starts it again with the same intent, and the Prepare goes on from its stamps. `status.json` counts
  `resumes`; the log of the killed attempt is kept and appended to (« resumed after the Prepare's process died »).
  After `MAX_RESUMES` (3) the title is failed with `KILLED`. A redelivered start of a Prepare that is not `running`
  any more does nothing. If the system does not let it go to the foreground then, it stops, and the launcher shows the
  Prepare as interrupted (« Resume », as before).
- tools.prepare runs under `setsid`, out of the app's process group: a kill of the app's process can leave it and its
  compilers running. Its group id is written to `<files>/prepare/<TID>/pgid` when it starts; the next Prepare of the
  title ends that group before anything else (`PrepareRunner.killOrphans`).
- A killed compiler (not the whole process) is the runner's retry with half the jobs, as before.

## The quick first build (XBA-128): a switch, off

With `<files>/prepare/quick-first` present (`adb shell run-as fr.adserver.xrecomp.shell touch
files/prepare/quick-first`), a Prepare that is not a background one builds at `-O1`; when its pack is published, an
`-O2` rebuild of the same title is queued **in the background** (Play is not blocked; it holds while the game runs).
Off by default: on the x86 host, `-O1` saves 4–6 % of the compile and costs a second full compile afterwards (see
« Measurements »). To be kept only if the device's numbers say otherwise.

## What the app provides

**Environment** of the prepare process (set by the service):

| Variable | Value |
|---|---|
| `PYTHONHOME` | the CPython stdlib in the assets (extracted to internal storage) |
| `PYTHONPYCACHEPREFIX` | a writable cache dir (`<cache dir>/pycache`): the assets are read-only |
| `TMPDIR` | `<cache dir>/tmp` (clang and Python temp files) |
| `HOME` | `<files dir>` |
| `LIBCAPSTONE_PATH` | the directory holding `libcapstone.so` (the capstone binding loads it from there) |
| `XBA_PYTHON` | optional: the interpreter for the step subprocesses, if `sys.executable` is not one |

The working directory does not matter; `python -m tools.prepare` needs the toolkit root on `sys.path`
(`PYTHONPATH=<toolkit root>`).

**Toolkit tree** in the assets (read-only): `tools/` (Python, plus the data files the tools read: `tools/recomp/data/`, `tools/xbe_ident/engines.json`), `include/`, the headers of `src/` (`src/**/*.h`),
`templates/new-game/`, `templates/runtime/` (the `recomp_types.h` the lifter copies next to the generated C), `profiles/`, `tools/VERSION` (one line: the commit; written by the asset build, git-ignored), and `tools/TOOLKIT_SHA256` (the toolkit digest, written by the asset build). The patterns are in
`tools/prepare/toolkit_tree.json`, read by both `android/app/build.gradle` (the zip) and `tools/prepare/toolkit.py`
(the digest on a host).

**status.json**: the service writes it from the progress lines for the UI and for a restart after the app was killed.
Its format belongs to the app (prepare-app branch, `android/app`), not to this page; prepare never reads it.
One Prepare runs at a time: a start for another title while one runs waits in `files/prepare/queue.json` (that
title's status says `queued`) and starts when the running one ends, whatever its outcome (`PrepareService`).

## When a pack is stale

A valid pack (the checks above) can still be the wrong one (XBA-127). The launcher (`PackFreshness`) tells two cases
apart:

| Case | How it is seen | What the player gets |
|---|---|---|
| **Another XBE**: the folder now holds another revision or region of the disc under the same title ID | `xbe_sha256` ≠ SHA-256 of the game folder's `default.xbe` | The pack is **never run**. Home: « Another version of the game · prepare it again », the primary is **Prepare again**, a note says saves are kept. « Open the game directly » does not start it either. |
| **Older toolkit**: the app was updated and its toolkit changed since the pack was lifted | `toolkit_sha256` ≠ the APK's digest (`assets/prepare/toolkit.zip.id`), or no `toolkit_sha256` at all | The pack **still plays** (its `shell_abi` matches, or it would not be valid). Home: « Prepared by an older version of the app », **Play** stays the primary and a quiet **Prepare again** sits next to it, with a note (about 20–30 min, saves kept). Never queued by itself: a Prepare blocks Play while it runs. |

When both hold, another XBE wins. An unknown side is not a mismatch: a pack.json without `xbe_sha256` (or one that is not
64 hex digits), an APK without the toolchain or without `toolkit.zip.id`. Saves are never touched in either case
(they live in `files/save/`, the Prepare writes only `work/` and `packs/`); the old pack stays until the new one replaces
it (atomic publish).

**The XBE hash.** `default.xbe` only (a few MB; game data is never hashed), once per change of the file: the hash is
kept for the process by path, size and mtime. Home hashes it on a worker thread and redraws when it does not match;
Play and « Open the game directly » hash it on the spot when Home has not yet. The record's `xbe_sha256` (taken at
import) is the fallback before that. The native loader does not compare it.

**What « the toolkit changed » means.** Not the commit: a new APK whose commit changed only Java, the docs, the tests
or the shell's `.c` files would otherwise send every player back to a 10–30 min Prepare for nothing. The toolkit digest
is SHA-256 over the **toolkit tree** (above: the pipeline's Python, `tools/recomp/data`, the profiles, the title
template, `include/` and the headers of `src/`), each file as `path NUL size NUL bytes` in path order; this is what
ends up in a pack's bytes besides the player's XBE. What the shell's `.c` files change reaches packs without a Prepare
(they call into `libmain.so`), and a change of that contract bumps `shell_abi`, which makes the pack invalid instead.
The tree also holds Python that is not on the lift path (`tools/compat`, `tools/diff`, …): a change there asks for a
Prepare that was not strictly needed; acceptable, and the tree stays one simple rule. The compiler (`libxbacc.so`, the
sysroot) is not in it: a new clang does not make an old pack wrong.

`android/app/build.gradle` computes the digest when it zips the toolkit and writes it into the zip
(`tools/TOOLKIT_SHA256`) and beside it (`prepare/toolkit.zip.id`, which the launcher reads without opening the zip).
`tools.prepare` reads `tools/TOOLKIT_SHA256` when there is one (the device), else computes the same digest from the
checkout (`tools/prepare/toolkit.py`; `pytest tools/prepare/test_toolkit.py` checks the selection), writes it into
`pack.json` and keys its stamps on it.

## Handing a pack to a game's own app (XBA-176)

A game's own app (Manage › Create an app, [app-shell.md](app-shell.md#a-games-own-app)) is XRecomp with one game and
its own private storage. Instead of preparing its game again (20–30 min), it takes XRecomp's pack over in seconds.
Code: `PackHandover` (both sides), `PackHandoverActivity` and `PackProvider` (XRecomp's side), `LauncherActivity`
(the game's app's Home). Tests: `PackHandoverTest`, `LauncherPackHandoverTest`.

**When the game's app asks.** At its launcher's start, once per process, when it has no valid pack for its title
(manifest meta-data `TITLE_ID`) or a valid one lifted by another toolkit than its own (`toolkit_sha256` against
`prepare/toolkit.zip.id`, [When a pack is stale](#when-a-pack-is-stale)); not while its own Prepare runs or waits,
nor while its game runs. On its way into the game it also asks when its game's files are not usable yet: XRecomp
then says where they are.
A stale pack of its own that XRecomp did not replace: it asks again only after XRecomp was updated (its
`lastUpdateTime`, kept in the `handover` preferences). « Open the game directly » or a shortcut that starts a
playable game skips the launcher, and so the question, until the launcher shows.

**The exchange.**

1. The game's app starts `<PARENT>/fr.adserver.xrecomp.PackHandoverActivity` (meta-data `PARENT`, XRecomp's
   package; component class names stay `fr.adserver.xrecomp.*` in a game's app) with `startActivityForResult`,
   extra `fr.adserver.xrecomp.extra.HANDOVER_TITLE_ID` = its title ID. Its manifest's `<queries>` names XRecomp's
   package (`TitleApk` leaves `<package>` as it is, so in the copy it names the parent).
2. XRecomp's activity (translucent, no view, `noHistory`, `excludeFromRecents`, in `:launcher`) checks the caller and
   finishes at once with its answer: the player sees nothing of XRecomp.
3. Accepted: `RESULT_OK`, extras `HANDOVER_FILES` (file names, `pack.json` first, then `lib`, then the `images`),
   `HANDOVER_SIZES`, one URI per file in the `ClipData` (`content://<XRecomp's package>.packs/<TITLEID>/<file>`) and
   `FLAG_GRANT_READ_URI_PERMISSION`. Refused: `RESULT_CANCELED` with `HANDOVER_REFUSAL` = the reason. To a caller
   that passed the checks (accepted, or refused with `NO_PACK` only), also where XRecomp reads the game from:
   `HANDOVER_GAME_KIND` (`FOLDER`, `IMAGE`, `NOT_SHARED`) and `HANDOVER_GAME_PATH` (an absolute path; none for
   `NOT_SHARED`: XRecomp's imported copy, an image reached by document grant, a path in XRecomp's own storage), so
   the game's app reads the same files in place ([app-shell.md « Straight into the game »](app-shell.md#straight-into-the-game)).
4. The game's app copies (below) on a worker thread, its progress on its one screen (« Copying the prepared game from
   XRecomp… 42 % »), then its game starts. Opened through the « Launcher » shortcut, Home shows the copy instead
   (a dimmed « Copying… »; Play and Prepare wait), then Play.

**Security model.** XRecomp hands its pack only to a caller that Android names and that it can check:

| Check | How | Refusal |
|---|---|---|
| XRecomp is not itself a game's app | its own meta-data has no `TITLE_ID` | `NOT_A_PARENT` |
| The caller is known | `getCallingPackage()`: set only for `startActivityForResult`; `adb shell am start` or a plain `startActivity` gives none | `NO_CALLER` |
| The title ID is one | 1–8 hex digits | `BAD_TITLE` |
| The caller is that title's game app | package = `fr.adserver.xrecomp.t<titleid>` (lower case) for the title ID it asks for | `WRONG_PACKAGE` |
| XRecomp ever made a game app | the AndroidKeyStore holds `xba_title_apps` (`TitleApp.existingCert`, which never makes it) | `NO_KEY` |
| The caller is signed by that key | `GET_SIGNING_CERTIFICATES`: one signer, its certificate byte-for-byte that key's certificate | `WRONG_CERT` |
| There is a whole pack | `Pack.check` = `VALID`, every file `pack.json` names present | `NO_PACK` |

Only the game apps XRecomp signed itself pass (the key never leaves its keystore), each for its own title. The pack's
files are read through `PackProvider` (`${applicationId}.packs`), **not exported**, `grantUriPermissions`: only the URIs
the activity granted, one per file, are readable, read-only, by that caller, while its activity lives. The provider
serves only `<TITLEID>/<name>` where the title ID is 8 upper-case hex digits and the name is `pack.json` or a file it
names (`lib`, `images`), inside that pack directory. Its authority is package-derived, so `TitleApk` moves it to
`fr.adserver.xrecomp.t<tid>.packs` in a game's app: no clash with XRecomp's (and a game's app serves nothing, as
it refuses with `NOT_A_PARENT`). The game's app trusts the package its meta-data names as its parent: it was made by
it and installs only next to it.

**The copy** (`PackHandover.take`, the checks of `Pack.check` and `tools/prepare/pack.py`):

1. `pack.json` read (1 MB at most) and checked: `format` 1, `title_id` the app's, `abi` `arm64-v8a`, `shell_abi` the
   app's, `lib` a plain name that was offered, `lib_size` > 0, `lib_sha256` a SHA-256, each `images` name offered.
   Else **UNFIT** (another version of the app made it). `xbe_sha256` against the disc the app is locked to (meta-data
   `XBE_SHA256`): else **OTHER_XBE**.
2. Freshness: a valid pack of the app's own is replaced only by one lifted with the app's own toolkit, and only when
   its own is not (else **NOT_NEWER**: kept, nothing copied). With no pack of its own, the app takes XRecomp's even
   from another toolkit (same `shell_abi`: it loads and plays), and Home offers Prepare again with the usual
   « Prepared by an older version of the app » note — better than 20–30 min before the first play.
3. Free space: the files' sizes plus 32 MB on internal storage, else **NO_SPACE** (the note gives the size).
4. Copy into `packs/<TITLEID>.partial/` (only what `pack.json` names: `lib`, then the `images`), each file synced; the
   library hashed while it is copied: its size must be `lib_size` and its SHA-256 `lib_sha256`, an image's size the
   offered one, else **DAMAGED** and the `.partial` is removed. `pack.json` written last (`pack.json.tmp`, sync,
   rename).
5. Publish as `pack.py` does: a leftover `.old` put back or removed first (`recover`), an existing pack renamed to
   `<TITLEID>.old`, `.partial` renamed to `<TITLEID>`, `.old` removed; `Pack.check` must then say `VALID`.
6. The pack's `title_layer` (`on` / `off`) becomes the app's Enhancements switch for the title (XRecomp's choice comes
   with its pack; else Home would offer to prepare again at once).

**Falling back.** Nothing is lost: when the game's app gets no pack, Home is the usual « Needs preparing » with
Prepare, and a note says why — XRecomp is not installed, has not prepared this game, refused (the reason), its pack
does not fit this version of the app, comes from another disc, there is not enough space, or the copy failed (open the
app again to retry). A game's app with a valid pack of its own says nothing and keeps playing it.

logcat (`xboxrecomp`): in the game's app `launcher: pack handover: asking <parent> for <TID> (no pack here)`, `…
offers [pack.json, libtitle.so] , <n> bytes`, `launcher: pack <TID> published from the handover`, `launcher: pack
handover <TID>: COPIED`, or `… refused: <REASON>` / `… is not installed` / `…: UNFIT (shell_abi 2, this app is 1)`; in
XRecomp `launcher: pack <TID> handed over to <package>` or `launcher: pack handover refused to <package> (<TID>):
<REASON>`.

## Self-healing (XBA-74, XBA-129)

A run writes what the static analysis missed to `<work>/discovered.jsonl`
([discovery.md](../technical/discovery.md)) and the next Prepare seeds the analysis with it. The app closes the loop
by itself, with no question asked (`SelfHeal.java`, state in `<work>/selfheal.json`):

- PrepareService records the file's length when a Prepare begins (`offset`): what the runs add after it is new.
- The launcher records each launch (`launched_at`). Back on Home with no game process, it reads the system's record
  of how the game's process last ended (`ActivityManager.getHistoricalProcessExitReasons`, the main process only).
  An exit before the launch, or already looked at (`handled`), changes nothing.
- **A crash** (`REASON_CRASH`, `REASON_CRASH_NATIVE`, `REASON_ANR`) with new `icall`, `thread` or `thread_ctx`
  entries: a round at once, the Preparing screen with a note (« The game crashed on code the preparation had missed:
  preparing it again with it (round N), then the game starts by itself. B stops it; Settings › General ›
  Self-healing turns this off. »), then the game starts again when the pack is ready.
- **A run that ended well** with such entries (a skipped indirect call does not crash: the title goes on, maybe
  wrong; the runtime says the first one in its log and counts them in the status line's `seeds`): a round **in the
  background**. The Prepare runs with `background: true` in `status.json` (and in the queue): Home keeps **Play**
  as the primary (the valid pack plays until the new one replaces it, atomic publish), its line says « Improving in
  the background · Compile · 68 % », a note explains, and a quiet **Stop** cancels it. Nothing starts by itself
  afterwards; the next start uses the new pack. PrepareService ends a background round, the launcher the others.
- **What a Prepare cannot fix**: a crash with a guest exception nothing handled (`exception`), a guest stack switch
  the runtime could not carry (`stack_switch`) or an untranslated instruction (`unimpl`), named in that order, even
  when the run also found new code; or a crash with nothing new (« crash (…), nothing new found », with « ; calls stub
  ordinal N (Name) » when the run called stub kernel routes). Home says « Crashed · needs a fix », the blocker in the
  note. No round.
- **The cap is on rounds without progress**, not on rounds. A round's progress is `discovered-seeds.json` growing
  (the seeds tools.prepare took through seed_from_log's gates). A crash round whose Prepare took nothing stops the
  loop at once (its pack is the same: no relaunch; « needs a fix », « the Prepare could not use what the run found
  (N addresses, none of them code) »). `MAX_IDLE_ROUNDS` (2) such rounds in a row and new finds no longer start a
  round by themselves until a run ends well with nothing new. On the device a crash is known only by the system's
  words for it, so « the same blocker twice » is a crash that brought nothing new: the discovery file never writes
  an address twice, a repeat of the same miss adds nothing. 12 rounds in a row is a safety net only.
- **The player sees and stops it**: the notes, Stop (background) or B / Cancel (Preparing screen), and **Settings ›
  General › Self-healing** (on by default, only with the toolchain). Off: a crash with new code offers **Prepare
  again** as the primary (the button of XBA-74); a good run with new code offers a quiet **Prepare again** next to
  Play (a background round). A stopped round starts nothing.
- A crash round whose Prepare ended while the launcher was away is counted when Home next shows (and relaunches the
  game if it took new code in).

The same rules on a desktop: `python -m tools.compat heal` ([compat-runner.md](../technical/compat-runner.md#self-healing-rounds-and-recall-xba-129)),
which `tools/compat/test_e2e.py::test_the_self_healing_loop` runs on a synthetic title (three rounds with progress,
the fourth without: stop). Tests on the app side: `SelfHealTest`, `LauncherSelfHealTest` (screenshots
`prepare-11` … `prepare-15`).

## Measurements

One real title's generated C (Alex's own disc, kept outside the repository; numbers only): 163 files, 256 MB of C
(`--split 250`), the largest 5.1 MB. NDK r28c clang 19 for `aarch64-linux-android30`, `-g0`, the flags above;
3 jobs in a container limited to 3 CPUs on the x86-64 host (Ryzen AI Max+ 395, Zen 5), 2026-11-10.

| | -O1 | -O2 |
|---|---|---|
| Wall time, 3 jobs | 240 s | 254 s |
| CPU time, all files (sum of per-file wall) | 717 s | 759 s |
| Per file: median / largest | 3.2 s / 23.5 s | 3.6 s / 23.2 s |
| Peak RSS per job: median / p90 / max | 163 / 304 / 597 MB | 163 / 302 / 589 MB |
| Objects, total | 150.5 MB | 149.0 MB |

**`--opt`: default 2.** -O2 costs 6 % more compile time than -O1 and the same memory, for the code the game then
runs all the time. -O1 stays an option for a low-end device.

**-O1 then -O2 (XBA-128), measured again through the Prepare's own driver** (`compile_all` with its new per-job
`wait4` peak RSS), on the generated C of a synthetic title of our own (40,000 small functions in a chain: 164 files,
77.4 MB of C, the largest 2.0 MB), NDK r28c clang 19, 3 jobs in a container limited to 3 CPUs on the same host,
2026-11-13:

| | -O1 | -O2 |
|---|---|---|
| Wall time, 3 jobs | 43.1 s | 44.8 s |
| Peak RSS per job: median / max | 86 / 121 MB | 86 / 120 MB |
| Objects | 24.8 MB | 24.8 MB |

Same story as the real title: -O1 is 4 % (synthetic) to 6 % (real) faster to first play and needs the same memory,
and the background -O2 rebuild then costs a whole compile again (time, heat, battery). Not the default; the switch
above is there to measure it on the Thor.

**Jobs: 1 GB per job instead of 1.5.** The worst file peaks at 0.6 GB, 90 % of them under 0.3 GB, and the files are
compiled largest first, so the big ones never run all at once. `min(5, (MemAvailable − 3 GB) / 1 GB)` gives a
12 GB device with ~7 GB available 4 jobs (1.5 GB would give 2). The 3 GB reserve stays: the app's UI, the system,
and room for a title whose largest file is twice this one. **Since XBA-209: 512 MB** — the 5 MB file came from the
250-function split; chunks are now at most ~1.5 MB of C and one compile peaks at ~170 MB on the synthetic title
below, so a device with 5.5 GB available gets the 5 jobs (it got 2).

**On a Thor (Snapdragon 8 Gen 2: 1× X3 3.2 GHz, 4× A715/A710 2.8 GHz, 3× A510), a rough estimate.** Per thread,
clang runs at about 0.6× a Zen 5 core on the X3 and 0.4× on an A715/A710 (published SPECint-class ratios; clang is
branchy integer code). Five jobs land on the X3 and the four middle cores: ~2.2 Zen 5 cores of throughput, so 760 s
of CPU at -O2 is ~6 min, plus 20–30 % for sustained clocks under thermal limits: **7–8 min** for the compile with
the NDK's clang. The embedded clang is a plain build of LLVM 19 (the NDK's is PGO + BOLT + LTO, ~20–30 % faster):
count **8–10 min**. With 2 jobs (the 1.5 GB formula on a 12 GB device) it would be 15–20 min. The largest file alone
takes ~1 min on a middle core; it starts first. To be measured on the device (« Device » check list).

**Where a Prepare spends its time (XBA-209).** A title-size synthetic title of our own: `tools.bench lift --copies
3000` (the bench corpus 3000 times: 39,013 functions, 2.3 MB of code, 96 MB of C), `tools.prepare --android` with
NDK r28c clang 19 as the stand-in for the embedded clang, `--jobs 5` in a container limited to 5 CPUs of the x86-64
host (a VM shared with other builds: wall times vary by ±15 % run to run, so the compile was also compared A/B on the
same CPUs, alternating), 2026-11-15. Seconds:

| Step | Before | After |
|---|---|---|
| analyse (disasm / func_id / abi) | 23.3 (20 / 1 / 2) | 22.0 (19 / 1 / 2); 23 MB written instead of 113 MB (`--lean`) |
| lift (`tools.recomp`) | 85.2 | 69.5 (one core alone: 103 → 77) |
| compile, 5 jobs, cold | 70.1 (160 files, median 2.1 s, largest 3.6 s) | the same: A/B 63.4 / 63.3 and 67.2 / 67.3 s wall, 313 / 313 s CPU (97 files, largest 1.5 MB) |
| link | 0.1 | 0.1 |
| re-prepare after one seeded function: compile | 80.0, 160 of 160 files compiled | 4.5, 4 of 97 |

The lift is the largest single cost on this title (a real one has bigger functions: more C per function, and the
compile weighs more, ~760 s of CPU for 256 MB of C above). The compile is linear in the C (3.1 s/MB of `-O2` on one
host core whatever the chunk size); `-O1` saves ~6 %, the vectorizers off ~5–10 % (both change the code the game
runs, so neither is taken); the headers are ~0.08 s of each chunk. Peak RSS per job: median 128 MB, max 171 MB.
The desktop Prepare (gcc, CMake, the toolkit built too, 5 CPUs): lift 98 → 71 s, build 121 → 91 s; a re-prepare
after one seeded function rewrote 79 of 160 chunks and rebuilt for 62 s, now 3 of 97 chunks and 3 s.

## GBA packs (XBA-157, XBA-158)

A Game Boy Advance game plays in the interpreter with no pack (consoles/README.md); a pack makes it run natively. The
same command, the same compile driver and the same publish build it from the player's ROM: `tools/prepare/gba.py`,
taken whenever SOURCE is a `.gba` (or any file with a GBA cartridge header). Contract of the lifted code:
`consoles/gba/runtime/gba_recomp.h`.

```
<files dir>/packs/gba-<KEY>/libtitle.so
<files dir>/packs/gba-<KEY>/pack.json
<files dir>/work/gba-<KEY>/                  --work: rom.gba, gen/, obj/, stamp.json, prepare.log, discovered.txt
```

`KEY` is the title key (`recomp_pack_rom_identify`, `gba_runtime_key`, `Console.identify`): game code + CRC32 of the
whole ROM (`AXVE-1A2B3C4D`), so two dumps of one game have two packs. The app's Prepare goes by the library key
`gba-<KEY>`: `status.json` in `<files>/prepare/gba-<KEY>/`, the queue, `Pack.check`.

**The steps** (same progress lines and ids, `n` = 5):

| # | id | Does | Stamp |
|---|---|---|---|
| 1 | `identify` | reads the ROM (32 MB at most: `BAD_ROM`; unreadable: `NO_ROM`), title key, SHA-256, header title; copies it to `<work>/rom.gba` | `rom` + `toolkit_sha256` (another ROM or toolkit starts over) |
| 2 | `analyse` | the addresses earlier runs had to interpret: `<work>/discovered.txt` (plus `--discovered FILE`), merged into `<work>/discovered-seeds.txt` | (their digest, in the lift's key) |
| 3 | `lift` | `python -m gba_tools.recomp <work>/rom.gba <work>/gen.new [--discovered seeds]`, then `gen.new` → `gen` (the stamp is dropped first: a half-written lift never passes) | `lift` |
| 4 | `compile` | each `gen/*.c` with the flags below, the object cache of « Compile and link » | the object cache |
| 5 | `pack` | `libtitle.so`, `pack.json`, atomic publish as `packs/gba-<KEY>` (`.partial`, `.old`, same rules) | `link` |

Compile flags: `--target=aarch64-linux-android30 --sysroot=<sysroot> [-resource-dir=…] -O1 -fno-tree-slp-vectorize -g0
-fPIC -std=gnu11 -w -DNDEBUG -I<work>/gen -I<toolkit>/consoles/gba/runtime` (`gba.compile_flags`). `--opt 2` and `3`
compile at -O1 for a GBA ROM (`gba_opt`; `pack.json` `cc` says `-O1`): lifted GBA code runs no faster at -O2, which
costs ~10 % more compile time. Jobs, when `--jobs` is 0: `default_jobs` with 256 MB per job and a 1 GB reserve (one
compile peaks at ~150 MB), still at most 5 and the CPU count, and the device log's limit as for an Xbox title. The
link is the Xbox pack's (`ld.lld … -shared -soname libtitle.so -Bsymbolic --no-undefined … -l:libmain.so`): every
core function the lifted code calls must be exported by `libmain.so`, which links the whole GBA core (`--whole-archive
gba_core`, android/app/src/main/cpp/CMakeLists.txt).

**Compile time (XBA-151).** The lifter writes blocks with one event check per segment instead of a label, a test and
an exit per instruction (consoles/gba/tools/README.md « Blocks and event checks »), files whose names and contents
stay the same when the discovery loop adds functions (the object cache recompiles the files that changed: ~1 per
function found), and Prepare drops the SLP vectorizer and -O2. Measured on the synthetic ROM of
`consoles/gba/tests/bench` (`build_rom.sh`: 1858 functions of game-like Thumb code, 200,550 instructions;
`compile_time.py`), CPU time of all compiles (sum of user time), 6 jobs, on the Zen 5 host of « Measurements »,
2026-11-14:

| | before (one label per instruction, -O2) | after (-O1) |
|---|---|---|
| NDK r28c clang 19, aarch64-linux-android30 | 730 s (3.64 s per 1k instructions) | 147 s (0.73 s per 1k) |
| Debian clang 19, x86-64 | 545 s (2.72 s per 1k) | 184 s (0.92 s per 1k) |
| Wall time, 6 jobs (NDK) | 127 s, largest file 28 s | 26 s, largest file 3.7 s |
| `pad.gba` (5.6k instructions, one 1555-instruction ARM function), NDK | 56 s | 10 s |

On a phone, with the « Measurements » model (5 jobs ≈ 2.2 Zen 5 cores, +25 % sustained clocks, +25 % for the
embedded clang: ×0.71 of the CPU time): 200k instructions **~1.7 min** (was ~8.6 min), 300k ~2.6 min, 500k ~4.4 min,
plus the lift (~6 s per 200k on the host) and the link. A commercial game's reachable code is 100k-500k instructions.
Run time is unchanged: frames per second with the pack, before / after (`gba_run --frames`, best of 7 interleaved
runs): the synthetic ROM 1091 / 1144, `cpu.gba` 10,040 / 10,020, `gfx.gba` 5,770 / 5,810, jsmolka `arm.gba` 5,820 /
5,760, `thumb.gba` 5,800 / 5,750 (the test ROMs idle after their checks: a branch to itself and the PPU).

The toolkit tree ships what the lift and the compile need: `consoles/gba/tools/**/*.py` (the lifter, `gba_tools`; on
`PYTHONPATH` for step 3) and `consoles/gba/runtime/*.h` (`gba_recomp.h`, `gba_core.h`, the lifted code's own header);
both count in the toolkit digest. `XBA_GBA_RECOMP` (a module name) and `XBA_GBA_TOOLS_DIR` replace the lifter, for the
tests (`tools/prepare/tests/fake_gba_recomp.py`, a table with the right ABI and `rom_sha256`).

**pack.json** of a GBA pack (format 1; the same reader rules: unknown fields ignored):

```json
{"format": 1, "console": "gba", "title_key": "AXVE-1A2B3C4D", "title": "POCKET QUEST", "rom_sha256": "...",
 "abi": "arm64-v8a", "shell_abi": 1, "toolkit": "<commit>", "toolkit_sha256": "...", "lib": "libtitle.so",
 "lib_sha256": "...", "lib_size": 123456, "entry": "gba_recomp_title_table", "cc": "clang 19.0.1 -O2",
 "built_at": "2026-11-14T21:10:35Z", "discovered": 12, "prepare": {...}}
```

| Field | Meaning |
|---|---|
| `console`, `title_key` | `gba` and the title key, in place of `title_id` (XBA-147). |
| `title` | The header's 12-character title (else the file name), for display. |
| `rom_sha256` | The ROM it was built from. The runtime compares it with the ROM it plays, and with the table's own `rom_sha256`. |
| `entry` | `gba_recomp_title_table`: `const gba_recomp_table *gba_recomp_title_table(void)`. |
| `discovered` | How many discovered addresses the lift was given. The app compares it with the file's count to offer Prepare again (below). |

The other fields are the Xbox pack's. `tools/prepare/gba.py` (`read`, `check`) is the reference.

**The loader.** `pack_loader.c` (`xba_pack_load_rom`) never refuses to start a GBA game:

1. `packs/gba-<KEY>/pack.json` missing → `PACK_NONE gba <KEY> (not prepared, the interpreter runs it)`.
2. `recomp_pack_find_key` refuses it (format, console, title_key, abi, shell_abi = `xba_shell_abi`, lib, lib_size) →
   `PACK_STALE gba <KEY> (<reason>), running interpreted`.
3. Else `gba_runtime_start(rom, <pack>/libtitle.so, key)`; the runtime (`consoles/gba/runtime/gba_pack.c`) checks
   again with what only it knows: `rom_sha256` = SHA-256 of the ROM it read, `lib_sha256` (hashed at each start: a GBA
   pack is a few MB), `entry`; then `dlopen(RTLD_NOW | RTLD_LOCAL)`, the table, its `abi` (`gba_attach_recomp`) and its
   `rom_sha256`. `PACK_LOADED gba <KEY> (<N> functions)`, or `PACK_STALE gba <KEY> (<why>), running interpreted`.

On a desktop, `gba_runtime_opts.pack_lib` does the same (a library with no `pack.json` beside it: the table's checks
only); `gba_play` exports the core (`gba_core_exported`, `ENABLE_EXPORTS` + the whole archive).

**The discovery loop.** At exit (also the in-game menu's Quit, through the exit hook) the runtime merges
`gba_recomp_missed` (the ROM addresses the interpreter had to start running that the table did not cover, bit 0 =
Thumb) into `<files>/work/gba-<KEY>/discovered.txt`: one `0x%08X` per line, sorted, no repeats, at most 65536 lines,
written through a temporary file; nothing new, nothing written. Log: `gba: <N> address(es) the pack did not cover, <A>
new, in <path>`. The next Prepare runs with `--work <files>/work/gba-<KEY>`, finds the file, and the lift takes them
(`--discovered`). Home compares the file's count with the pack's `discovered`: more → « Prepared · N new addresses
found · prepare again », Play stays first (app-shell.md « Game Boy Advance games »).

**On a desktop** (Linux x86-64 or arm64, macOS): `python3 -m tools.prepare game.gba` builds a pack for `gba_play` with
the host's `cc` (or `--cc`, `$CC`): the same steps, `-shared -fPIC -Wl,-Bsymbolic`, the core left undefined (resolved
against `gba_play` at load), `"abi"` the host's (`x86_64`, `arm64-v8a`). Work and packs default to
`$XDG_DATA_HOME/xrecomp/work/gba-<KEY>` and `…/packs/gba-<KEY>` (beside `gba_play`'s saves); `--work`, `--pack-dir`,
`--jobs`, `--opt`, `--force`, `--discovered` as above. With `--android` and the NDK it is the device's build (« Checking
on a host »). Tests: `python3 -m pytest tools/prepare/test_gba_prepare.py` (the NDK case runs where
`ANDROID_NDK_HOME` is set), `consoles/gba/tests` `gba_pack`, `tests/recomp_pack`.

## PS1 packs (XBA-168)

A PlayStation disc plays in the interpreter with no pack (consoles/README.md « Playing a PS1 game »); a pack makes it
run natively. It is the GBA's path for a disc: the same command, steps, compile driver and publish,
`tools/prepare/ps1.py` (a subclass of `gba.py`'s run), taken whenever SOURCE is a `.cue`, a `.chd`, a `.bin` / `.iso`
/ `.img` whose ISO 9660 volume says `PLAYSTATION`, or a PS-X EXE. Contract of the lifted code:
`consoles/ps1/runtime/ps1_recomp.h`; the lifter: `consoles/ps1/tools` (`ps1_tools`).

```
<files dir>/packs/ps1-<KEY>/libtitle.so
<files dir>/packs/ps1-<KEY>/pack.json
<files dir>/work/ps1-<KEY>/      --work: boot.exe, gen/, obj/, stamp.json, prepare.log, discovered.txt (disc.iso)
```

`KEY` is the title key (`recomp_pack_rom_identify`, `Console.identify`, `ps1.disc_key`): the serial of SYSTEM.CNF's
`BOOT` line (`SLUS-00594`), else `PSX-<CRC32 of the 2048 bytes of the volume descriptor>`; a bare EXE (desktop):
`EXE-<CRC32 of the file>`. The app's Prepare goes by the library key `ps1-<KEY>`.

**The source identity** (pack.json `source_sha256`) is the SHA-256 of the **boot EXE** (SYSTEM.CNF's `BOOT` file,
else `PSX.EXE`; for an EXE, the file), not of the image:

- reading a whole 700 MB image on a phone takes tens of seconds, at every Prepare and at every start; the boot EXE is
  2 MB at most, a few sectors away from the volume descriptor;
- it is what the lifted table records itself (`ps1_recomp_table.source_sha256`), so the runtime checks one value
  against pack.json and against the library;
- it is the same for the `.cue` / `.bin`, the `.iso` and the `.chd` of one disc (a pack built from one plays the
  others), and differs for another release or a patched EXE;
- what it does not cover cannot run wrong code: overlays and anything else loaded at run time are matched by content
  in RAM, entry by entry (ps1_recomp.h), so a disc whose other files differ only runs more in the interpreter.

**The steps** (same progress lines and ids, `n` = 5):

| # | id | Does | Stamp |
|---|---|---|---|
| 1 | `identify` | reads the disc with the lifter's own reader (`ps1_tools.exe`: ISO 9660 on cooked or raw sectors, a `.cue`'s first data track, a `.chd`: see below): title key, the boot EXE (over 2 MB + 2 KB, missing, not a PS-X EXE, a CHD hunk it cannot decode: `BAD_DISC`; unreadable: `NO_DISC`), its SHA-256; copies it to `<work>/boot.exe` | `source` + `toolkit_sha256` (another EXE or toolkit starts over) |
| 2 | `analyse` | the code earlier runs had to interpret: `<work>/discovered.txt` (plus `--discovered FILE`), one record kept per address + hash, sorted, into `<work>/discovered-seeds.txt` | (their digest, in the lift's key) |
| 3 | `lift` | `python -m ps1_tools.recomp <work>/boot.exe <work>/gen.new [--discovered seeds]`, then `gen.new` → `gen` (the stamp is dropped first) | `lift` |
| 4 | `compile` | each `gen/*.c` with the flags below, the object cache of « Compile and link » | the object cache |
| 5 | `pack` | `libtitle.so`, `pack.json`, atomic publish as `packs/ps1-<KEY>` | `link` |

The lift reads the copied EXE, not the image: the image is opened once, in step 1. Compile flags: the GBA's with
`-I<toolkit>/consoles/ps1/runtime` (`--target=aarch64-linux-android30 --sysroot=<sysroot> [-resource-dir=…] -O1
-fno-tree-slp-vectorize -g0 -fPIC -std=gnu11 -w -DNDEBUG -I<work>/gen`, `ps1.compile_flags`); `--opt 2` and `3`
compile at -O1 for a PS1 title too (`ps1_opt`; `pack.json` `cc` says `-O1`). Jobs, when `--jobs` is 0: `default_jobs`
with 256 MB per job and a 1 GB reserve (one compile peaks at ~160 MB), at most 5 and the CPU count, and the device
log's limit. The link is the Xbox pack's (`ld.lld … -shared -soname libtitle.so
-Bsymbolic --no-undefined … -l:libmain.so`): every core function the lifted code calls (`ps1_rc_*`, ps1_recomp_rt.h)
must be exported by `libmain.so`, which links the whole PS1 core (`--whole-archive ps1_core`,
android/app/src/main/cpp/CMakeLists.txt). The toolkit tree ships `consoles/ps1/tools/**/*.py` (on `PYTHONPATH` for
step 3; step 1 imports its disc reader) and `consoles/ps1/runtime/*.h`; both count in the toolkit digest.
`XBA_PS1_RECOMP` (a module name) and `XBA_PS1_TOOLS_DIR` replace the lifter for the tests
(`tools/prepare/tests/fake_ps1_recomp.py`).

**Compile time (XBA-161).** What was done for GBA code (« Compile time (XBA-151) » above), for MIPS: the lifter
writes blocks with one scheduler check per segment instead of a label, a check and an exit per instruction; an access
whose address is not a known IO register is a RAM access, main RAM and the scratchpad inline and anything else
handed to the interpreter with the rest of its block (one exit per access, no call into the core); a function keeps
in locals only the guest registers it uses, and writes back only the ones it writes (consoles/ps1/tools/README.md
« Lifting »). Files are cut by a hash of function identities and named after their first function, so a discovery
round recompiles ~1 file per function found, and Prepare drops the SLP vectorizer and -O2. Measured on the synthetic
program of `consoles/ps1/tests/bench` (`build_exe.sh`: 1102 functions of game-like C built by PSn00bSDK, 222,094
instructions; `compile_time.py`), CPU time of all compiles (sum of user + system time), 6 jobs, on the Zen 5 host of
« Measurements », 2026-11-14 (the host was shared and loaded: before and after were compiled interleaved in one pool):

| | before (one label and check per instruction) | after (-O1) |
|---|---|---|
| NDK r28c clang 19, aarch64-linux-android30, -O1 | 880 s (3.96 s per 1k instructions; 3.54 on a quieter host) | 142 s (0.64 s per 1k; 0.61 alone) |
| NDK clang, -O2 (Prepare's level before) | 1219 s (5.49 s per 1k) | (-O1) |
| Debian clang 19, x86-64, -O1 | 659 s (2.97 s per 1k) | 150 s (0.67 s per 1k) |
| Wall time, 6 jobs (NDK) | 138 s, largest file 30 s | 23 s, largest file 4.2 s |
| Peak memory of one compile (NDK) | 363 MB | 160 MB |
| Files compiled after a discovery round (8 functions found) | all 70 | 3 of 88 |

On a phone, with the « Measurements » model (×0.71 of the host's NDK CPU time; the loaded host's figures, so on the
safe side): 200k instructions **~1.5 min** (was ~9.4 min at -O1, ~13 min at Prepare's -O2), 400k ~3 min (was ~19 /
~26 min), plus the lift (~3 s per 220k on the host) and the link. A game's main EXE and overlays are 200k-400k instructions. Run time is not worse: frames per
second with the pack, before / after (`ps1_run --frames`, best of 5 interleaved runs, host cc -O1): the synthetic
program 334 / 360 (its timer IRQ every 2000 cycles lands inside blocks: the interpreter finishes them), ps1-tests
cpu/cop 2,818 / 3,599, cpu/code-in-io 2,702 / 3,636, gte/test-all 885 / 1,020, dma/otc-test 2,669 / 3,283, timers
2,655 / 3,371, our homebrew within ±4 % (they idle after their checks). The share of instructions run natively in
the lockstep tests drops from 99.9 % to 99.1-99.9 % (IO through a pointer, events inside blocks), for less time per
frame. -O2 runs the synthetic program no faster (346 / 351 fps at -O2 / -O1).

**A disc behind the Storage Access Framework.** tools.prepare opens files by path; a library folder added with the
file picker gives documents. `PrepareService.ps1Source` then copies, from the disc's data track (a single-file `.bin`
/ `.iso` document, or a `.cue`'s first FILE among the track documents the library index lists for it), exactly the
sectors that lead to the boot EXE (the volume descriptor, the root directory, SYSTEM.CNF, the directories on the
EXE's path, the EXE) to their place in `<work>/disc.iso`, a cooked image with holes everywhere else (`Ps1Disc`: a
sparse file, a few hundred KB on disk whatever the disc's size), and passes that as SOURCE. The rules are
`ps1_tools.exe`'s, so the key and the EXE found there are the disc's. A path (All files access) goes as it is: a
`.cue` finds its tracks beside it.

**A `.chd`** (XBA-168) is prepared like the `.cue` of the same disc, with the same key and source identity (the boot
EXE's SHA-256), so either pack plays either image. Two readers, one per side:

- `ps1_tools` has its own CHD v5 reader (`consoles/ps1/tools/ps1_tools/chd.py`, pure Python): the Huffman-coded map,
  then only the hunks that hold the volume descriptor, the directories, SYSTEM.CNF and the boot EXE (a few dozen of a
  disc's tens of thousands). Codecs: `cdzl` (deflate) and `cdlz` (LZMA) through CPython's `zlib` and `lzma`, which the
  app's embedded CPython 3.13 ships (`lib-dynload/zlib…so` against the system's libz, `_lzma…so` with liblzma built
  in: checked in `python-stdlib.zip`); `cdzs` (zstd) through `compression.zstd`, CPython 3.14 and later only (not the
  app's); `cdfl` (FLAC) is never decoded: chdman uses it for audio hunks, which hold no code. A data hunk the reader
  cannot decode (FLAC, zstd on Python 3.13, a diff CHD's parent) fails with `BAD_DISC` saying which. chdman's default
  codecs for a CD are `cdlz`, `cdzl`, `cdfl`: every chdman CD image of a PlayStation disc reads.
- the app does not depend on that: `PrepareService.ps1Source` opens a `.chd` (a path or a document, as `fd:N`) with
  the native disc reader the game uses (`Ps1Native.openDisc`: `ps1_disc.c` and libchdr in `libxba_import.so`, every
  codec) and `Ps1Disc.extract` copies the boot EXE's sectors into the sparse `<work>/disc.iso` as for a `.bin`
  document, so tools.prepare reads a plain ISO. Without the native library a path goes to tools.prepare as it is.

The Home of a `.chd` offers Prepare like any disc. Tests: `consoles/ps1/tools/ps1_tools/test_chd.py` (CHDs written
in chdman's shapes, every codec, raw and repeated hunks, run-length maps, errors; chdman's own images when chdman is
installed), `tools/prepare/test_ps1_prepare.py` (`.chd` keys and EXEs, the Prepare end to end from a `.chd` of the
overlay homebrew), ctest `ps1_hb_chd_overlay_cdlz` / `_cdzl` (a CHD `ps1_tools.chd` wrote, booted through libchdr: the
two readers agree).

**pack.json** of a PS1 pack (format 1; unknown fields ignored):

```json
{"format": 1, "console": "ps1", "title_key": "SLUS-00594", "title": "Space Race", "source_sha256": "...",
 "boot": "SLUS_005.94", "abi": "arm64-v8a", "shell_abi": 1, "toolkit": "<commit>", "toolkit_sha256": "...",
 "lib": "libtitle.so", "lib_sha256": "...", "lib_size": 123456, "entry": "ps1_recomp_title_table",
 "cc": "clang 19.0.1 -O2", "built_at": "2026-11-14T21:10:35Z", "discovered": 21, "prepare": {...}}
```

| Field | Meaning |
|---|---|
| `console`, `title_key` | `ps1` and the title key, in place of `title_id`. |
| `title` | The image's file name without its extension, for display. |
| `source_sha256` | The boot EXE it was built from (above). The runtime compares it with the disc it plays and with the table's own `source_sha256`. |
| `boot` | The boot file's path on the disc (information). |
| `entry` | `ps1_recomp_title_table`: `const ps1_recomp_table *ps1_recomp_title_table(void)`. |
| `discovered` | How many discovered records the lift was given. The app compares it with the file's line count to offer Prepare again. |

The other fields are the Xbox pack's. `tools/prepare/ps1.py` (`read`, `check`) is the reference.

**The loader.** `pack_loader.c` (`xba_pack_load_rom`) never refuses to start a PS1 game:

1. `packs/ps1-<KEY>/pack.json` missing → `PACK_NONE ps1 <KEY> (not prepared, the interpreter runs it)`.
2. `recomp_pack_find_key` refuses it (format, console, title_key, abi, shell_abi, lib, lib_size) → `PACK_STALE ps1
   <KEY> (<reason>), running interpreted`.
3. Else `ps1_runtime_start(disc, <pack>/libtitle.so, key)`; the runtime (`consoles/ps1/runtime/ps1_pack.c`) checks
   again with what only it knows: `source_sha256` = the SHA-256 of the boot EXE it reads from the disc
   (`ps1_pack_disc_boot_sha256`: ISO 9660 through `ps1_disc`, so `.cue`, `.bin`, `.iso`, `.chd`, `fd:N` and the
   `/proc/self/fd` cue of a SAF disc alike; the same rules as `ps1_tools.exe`), `lib_sha256` (hashed at each start),
   `entry`; then `dlopen(RTLD_NOW | RTLD_LOCAL)`, the table, its `abi` (`ps1_attach_recomp`) and its `source_sha256`.
   `PACK_LOADED ps1 <KEY> (<N> functions)`, or `PACK_STALE ps1 <KEY> (<why>), running interpreted` (a boot EXE that
   cannot be read is one: nothing to check the pack against).

On a desktop, `ps1_runtime_opts.pack_lib` (`ps1_play --pack`) does the same (a library with no `pack.json` beside it:
the table's checks only); `ps1_play` exports the core (`ps1_core_exported`: `ENABLE_EXPORTS` + the whole archive).

**The discovery loop.** At exit (also the in-game menu's Quit, through the exit hook) the runtime appends the code
the interpreter had to start running while the pack was attached and the table did not cover
(`ps1_recomp_write_missed`) to `<files>/work/ps1-<KEY>/discovered.txt`: one record per line, `ADDR HASH HEXBYTES`
(the address, the content hash, up to 2 KB of the code bytes as they were in RAM), a record the file already holds
(same address and hash) never written twice, the folder made first. The bytes are what lets the next Prepare lift
code that is not in the EXE: overlays read from the disc at run time, several of them at one address. Log: `ps1: code
at <N> address(es) the pack did not cover, <A> new record(s), in <path>`. The next Prepare runs with `--work
<files>/work/ps1-<KEY>`, finds the file, and the lift takes it (`--discovered`). Home compares the file's line count
with the pack's `discovered`: more → « Prepared · N new addresses found · prepare again », Play stays first
(app-shell.md « PlayStation games »). Without a pack nothing is recorded: the first Prepare lifts what the EXE
holds, the runs with its pack find the rest.

**On a desktop** (Linux x86-64 or arm64, macOS): `python3 -m tools.prepare game.cue` builds a pack for `ps1_play`
with the host's `cc` (or `--cc`, `$CC`), the core left undefined (resolved against `ps1_play` at load), `"abi"` the
host's; work and packs default to `$XDG_DATA_HOME/xrecomp/work/ps1-<KEY>` and `…/packs/ps1-<KEY>`. With `--android`
and the NDK it is the device's build. Checked on 2026-11-14 with our homebrew overlay disc
(`tools/consoles/build_homebrew.sh ps1`):

```sh
python3 -m tools.prepare build/homebrew/ps1/overlay.cue --work /tmp/w --pack-dir /tmp/p --opt 1
build/ps1/ps1_runtime/ps1_play build/homebrew/ps1/overlay.cue --frames 3000 --pack /tmp/p/ps1-PSX-4009E1ED/libtitle.so \
    --missed /tmp/w/discovered.txt            # PACK_LOADED ps1 PSX-4009E1ED (69 functions); 21 new records
python3 -m tools.prepare build/homebrew/ps1/overlay.cue --work /tmp/w --pack-dir /tmp/p --opt 1   # discovered 21
build/ps1/ps1_runtime/ps1_play …                 # PACK_LOADED (86 functions): both overlays native, nothing new
build/ps1/ps1_runtime/ps1_run build/homebrew/ps1/overlay.cue --pack /tmp/p/ps1-PSX-4009E1ED/libtitle.so --lockstep \
    --result --frames 3000                       # LOCKSTEP OK, result PASS
```

The device's build, checked the same day: `--android` with the NDK's clang, then with the shipped toolchain itself
(`libxbacc.so` under qemu-aarch64, the extracted `sysroot.zip`, `--cc-prefix-arg clang`), both against the real
`libmain.so` built from `android/app/src/main/cpp`: `XBA_RESULT ok` (69 s under qemu, 3 files at -O2, the real
`ps1_tools`); `libtitle.so` AArch64 DYN, NEEDED libmain / libm / libdl / liblog / libc, `ps1_recomp_title_table`
exported, its four undefined core symbols (`ps1_rc_lw`, `ps1_rc_read`, `ps1_rc_write`, `ps1_rc_swlr`; more for code
with GTE commands) all exported by `libmain.so`; `"cc": "clang 19.1.7 -O2"`.

Tests: `python3 -m pytest tools/prepare/test_ps1_prepare.py` (a fake lifter on images the test makes; the real
`ps1_tools` on the homebrew disc when it is built; the NDK case where `ANDROID_NDK_HOME` is set),
`consoles/ps1/tests` `ps1_pack`, `tests/recomp_pack`, the app's `Ps1DiscTest`, `PrepareServiceTest.ps1Source`,
`LauncherLibraryTest.ps1HomePrepares`.

## N64 packs (XBA-174)

An N64 cartridge plays in the interpreter with no pack (consoles/README.md « Playing an N64 game »); a pack makes its
CPU code run natively (the RSP, the RDP and the rest of the RCP are the same code either way). It is the PS1's path
for a ROM: the same command, steps, compile driver and publish, `tools/prepare/n64.py` (its `Run` is `ps1.Run` with a
cartridge in place of a disc: `ps1.Run` reads the lifter, flags, checks and pack.json from a console module,
`Run.cm`), taken whenever SOURCE's first word is an N64 ROM's in one of its three byte orders (`.z64`, `.v64`, `.n64`;
the extension does not matter; checked before the GBA and PS1 paths). Contract of the lifted code:
`consoles/n64/runtime/n64_recomp.h`; the lifter: `consoles/n64/tools` (`n64_tools`).

```
<files dir>/packs/n64-<KEY>/libtitle.so
<files dir>/packs/n64-<KEY>/pack.json
<files dir>/work/n64-<KEY>/      --work: gen/, obj/, stamp.json, prepare.log, discovered.txt (rom.z64)
```

`KEY` is the title key (`recomp_pack_rom_identify`, `Console.identify`, `n64.title_key`, `n64_tools` `Rom.title_key`):
the header's game code (0x3B, `[A-Z0-9]` else `_`) and CRC1 (0x10), `NSME-635A2BFF`, the same from every byte order.
Our libdragon homebrew all have `N___-00000000` (no game code, no CRC): they share a key and a pack folder, and the
source identity tells them apart (the last one prepared is the one that loads; the others run interpreted, `PACK_STALE
… built from another ROM`). The app's Prepare goes by the library key `n64-<KEY>`.

**The source identity** (pack.json `source_sha256`) is the SHA-256 of the **whole ROM in the console's big-endian
order** (z64; a v64 has its byte pairs swapped back, an n64 its words reversed, a trailing part that is not a whole
pair or word as it is: `n64_rom_normalize`'s rule):

- a `.z64`, a `.v64` and a `.n64` dump of one cartridge are one game with one pack; a hack, a translation or another
  revision is another ROM;
- it is what `n64_tools` records in the table (`n64_recomp_table.source_sha256`, `Rom.sha256`), so the runtime checks
  one value against pack.json and against the library;
- the Prepare reads the ROM once in 1 MB pieces, normalising and hashing as it goes (no copy of up to 64 MB in the
  work folder: the lifter reads the file in any byte order itself);
- at each start the runtime has the whole ROM in memory already (`n64_create` needs it), and hashes it only when a
  pack is there (`n64_pack_rom_sha256`, streaming through a 64 KB buffer, the ROM untouched). Measured: 64 MB in
  0.30 s in plain C on the Zen 5 host of « Measurements »; on arm64 the ARMv8 SHA-2 instructions are used when the
  CPU has them (`getauxval(AT_HWCAP) & HWCAP_SHA2`, every Snapdragon; `N64_PACK_SHA_C=1` forces plain C), which hash
  well over 1 GB/s on a Cortex-X3 class core, so tens of milliseconds for the largest cartridge (the runtime logs it:
  `n64: ROM identity <16 hex>... (<ms> ms)`; under qemu both paths give the same hashes, the speed is not
  representative). A cache keyed by path, size and time was not needed, and would not cover `fd:N` documents.
  Known-answer tests run on both paths (`consoles/n64/tests` `n64_pack`, under qemu-aarch64 too).

**The steps** (same progress lines and ids, `n` = 5):

| # | id | Does | Stamp |
|---|---|---|---|
| 1 | `identify` | reads the ROM (under 4 KB, over 64 MB, not an N64 byte order: `BAD_ROM`; unreadable: `NO_ROM`): title key, header title, byte order, the source identity | `source` + `toolkit_sha256` (another ROM or toolkit starts over; the same ROM in another byte order does not) |
| 2 | `analyse` | the code earlier runs had to interpret: `<work>/discovered.txt` (plus `--discovered FILE`), one record kept per address + hash, sorted, into `<work>/discovered-seeds.txt` (the PS1's reader: the format is the same) | (their digest, in the lift's key) |
| 3 | `lift` | `python -m n64_tools.recomp ROM <work>/gen.new [--discovered seeds]`, then `gen.new` → `gen` (the stamp is dropped first) | `lift` |
| 4 | `compile` | each `gen/*.c` with the flags below, the object cache of « Compile and link » | the object cache |
| 5 | `pack` | `libtitle.so`, `pack.json`, atomic publish as `packs/n64-<KEY>` | `link` |

Compile flags: the PS1's with `-I<toolkit>/consoles/n64/runtime` (`-O1 -fno-tree-slp-vectorize -g0 -fPIC -std=gnu11
-w -DNDEBUG -I<work>/gen`, `n64.compile_flags`; `--opt 2` and `3` compile at -O1, `n64.lifted_opt`). Jobs, when
`--jobs` is 0: 256 MB per job and a 1 GB reserve (files of 1000..4000 instructions, like the PS1's). `n64_tools` cuts
files by a hash of function identity and names them after their first function, so a discovery round recompiles
the table and the files that gained functions (checked on gfx.z64: the 2 records of a run lifted, 2 of 27 files compiled again, 1205 functions instead of 1203).
The link is the Xbox pack's (`--no-undefined … -l:libmain.so`): every core function the lifted code calls
(`n64_rc_*`, n64_recomp_rt.h) must be exported by `libmain.so`, which links the whole N64 core (`--whole-archive
n64_core n64_rsp n64_rdp`, android/app/src/main/cpp/CMakeLists.txt). The toolkit tree ships
`consoles/n64/tools/**/*.py` (on `PYTHONPATH` for step 3) and `consoles/n64/runtime/*.h`; both count in the toolkit
digest. `XBA_N64_RECOMP` (a module name) and `XBA_N64_TOOLS_DIR` replace the lifter for the tests
(`tools/prepare/tests/fake_n64_recomp.py`).

**pack.json** of an N64 pack (format 1; unknown fields ignored):

```json
{"format": 1, "console": "n64", "title_key": "NSME-635A2BFF", "title": "SUPER MARIO 64", "source_sha256": "...",
 "rom_order": "z64", "abi": "arm64-v8a", "shell_abi": 1, "toolkit": "<commit>", "toolkit_sha256": "...",
 "lib": "libtitle.so", "lib_sha256": "...", "lib_size": 3064944, "entry": "n64_recomp_title_table",
 "cc": "clang 19.1.7 -O1", "built_at": "2026-11-14T07:07:54Z", "discovered": 2, "prepare": {...}}
```

| Field | Meaning |
|---|---|
| `console`, `title_key` | `n64` and the title key, in place of `title_id`. |
| `title` | The header's name (0x20, 20 characters), else the file name without its extension, for display. |
| `source_sha256` | The ROM it was built from, in big-endian order (above). The runtime compares it with the ROM it plays and with the table's own `source_sha256`. |
| `rom_order` | The byte order of the file prepared (information; the identity is the same for all three). |
| `entry` | `n64_recomp_title_table`: `const n64_recomp_table *n64_recomp_title_table(void)`. |
| `discovered` | How many discovered records the lift was given. The app compares it with the file's line count to offer Prepare again. |

The other fields are the Xbox pack's. `tools/prepare/n64.py` (`read`, `check`) is the reference.

**The loader.** `pack_loader.c` (`xba_pack_load_rom`) never refuses to start an N64 game: no `pack.json` →
`PACK_NONE n64 <KEY> (not prepared, the interpreter runs it)`; `recomp_pack_find_key` refuses it → `PACK_STALE n64
<KEY> (<reason>), running interpreted`; else `n64_runtime_start(rom, <pack>/libtitle.so, key)`, and the runtime
(`consoles/n64/runtime/n64_pack.c`, the PS1's `ps1_pack.c` for a cartridge) checks again with what only it knows:
`source_sha256` = the ROM's, `lib_sha256` (hashed at each start), `entry`; then `dlopen(RTLD_NOW | RTLD_LOCAL)`, the
table, its `abi` (`n64_attach_recomp`) and its `source_sha256`. `PACK_LOADED n64 <KEY> (<N> functions)`, or
`PACK_STALE n64 <KEY> (<why>), running interpreted`. On a desktop `n64_runtime_opts.pack_lib` (`n64_play --pack`)
does the same (a library with no `pack.json` beside it: the table's checks only); `n64_play` exports the core
(`n64_core_exported`), as `n64_run` does.

**The discovery loop.** When the app pauses (a session usually ends killed in the background, after the pause), every
three minutes of play and at exit (also the in-game menu's Quit, through the exit hook) the runtime appends the code
the interpreter had to start running while the pack was attached, after the boot PC, that no entry matched (a branch
taken or jump into it, not code that follows in sequence; `n64_recomp_write_missed`) to
`<files>/work/n64-<KEY>/discovered.txt`: `ADDR HASH HEXBYTES` per line (the virtual address, the content hash, up to
2 KB of the code as it was in RDRAM, never past a mapped page; a record inside an earlier one with the same bytes holds
its first 16 words only), one record per code at an address (records that agree over their common length are the
same code; files written before 2026-11-15 hold the same code several times, the lift merges them), a record the file
already holds never written twice, the folder made first; 65536 records and 64 MB of code per run at most. Log, when
something was added: `n64: code at <N> address(es) the pack did not cover, <A> new record(s), in <path>`. The next Prepare finds the file in its work folder and the lift takes it, bytes
included: code loaded by DMA from computed addresses, decompressed or TLB-mapped code gets lifted. Home compares the
file's line count with the pack's `discovered` (app-shell.md « Nintendo 64 games »).

**RDP writes.** The core's write tracking (per-4 KB-page counters of RDRAM, n64_recomp.h) covers CPU stores, lifted
stores, PI, SI and SP DMA, and since XBA-174 the RDP's rendering: `n64_rdp.c` reports the rows of the colour and Z
images each command list drew through the bus's `wrote` callback (after the pixels are written, on the CPU's thread;
with scanline-band threads when the batch is flushed), so code drawn over by the RDP is checked again like code
overwritten by DMA. Output and speed unchanged (`n64_rdp` golden hashes, `n64_rdp_bench`).

**On a desktop** (Linux x86-64 or arm64, macOS): `python3 -m tools.prepare game.z64` builds a pack for `n64_play` /
`n64_run` with the host's `cc`, the core left undefined (resolved against the program at load), `"abi"` the host's;
work and packs default to `$XDG_DATA_HOME/xrecomp/work/n64-<KEY>` and `…/packs/n64-<KEY>`. Checked on 2026-11-14
with our homebrew (`tools/consoles/build_homebrew.sh n64`):

```sh
python3 -m tools.prepare build/homebrew/n64/gfx.z64 --work /tmp/w --pack-dir /tmp/p    # 27 files, 8 s, 5 jobs
build/n64t/n64_runtime/n64_play build/homebrew/n64/gfx.z64 --headless --frames 300 --pack \
    /tmp/p/n64-N___-00000000/libtitle.so --missed /tmp/w/discovered.txt
    # n64: ROM identity e080322a138d2a58... (1 ms); PACK_LOADED n64 N___-00000000 (1203 functions);
    # n64: code at 2 address(es) the pack did not cover, 2 new record(s) (libdragon's trampolines at 0x807FFEA0)
build/n64t/n64_runtime/n64_run build/homebrew/n64/gfx.z64 --pack /tmp/p/n64-N___-00000000/libtitle.so \
    --lockstep --stats --frames 300                      # LOCKSTEP OK
python3 -m tools.prepare build/homebrew/n64/gfx.z64 --work /tmp/w --pack-dir /tmp/p    # discovered 2: 2 of 27 files compiled
build/n64t/n64_runtime/n64_run … --lockstep --missed /tmp/w/m2.txt   # LOCKSTEP OK, 0 new missed records
```

The device's build, checked the same day against the real `libmain.so` of the debug APK (`assembleDebug`, NDK r28c):
`--android` with the NDK's clang (19.0.1) and with the shipped toolchain itself (`libxbacc.so` under qemu-aarch64,
clang 19.1.7, the extracted `sysroot.zip`, `--cc-prefix-arg clang`: 27 files in 88 s under qemu, 6 jobs): `XBA_RESULT
ok` both times; `libtitle.so` AArch64 DYN (3.2 MB), NEEDED libmain / libm / libdl / liblog / libc,
`n64_recomp_title_table` exported, its six undefined core symbols (`n64_rc_read`, `n64_rc_write`,
`n64_rc_read_partial`, `n64_rc_write_masked`, `n64_rc_cop1`, `n64_rc_ctc1`) all exported by `libmain.so` (138 `n64_`
symbols, the whole core); `"cc": "clang 19.1.7 -O1"`.

Tests: `python3 -m pytest tools/prepare/test_n64_prepare.py` (a fake lifter on ROMs the test makes in the three byte
orders; the real `n64_tools` on gfx.z64 when the homebrew is built, run by `n64_run --pack --lockstep` when it is built
too; the NDK case where `ANDROID_NDK_HOME` is set), `consoles/n64/tests` `n64_pack` and `n64_rdp` (the `wrote`
ranges), `tests/recomp_pack`, the app's `PrepareServiceTest.n64Source`, `LauncherLibraryTest.n64HomePrepares`.

## The loader

The app shell's side (`android/app/src/main/cpp/pack_loader.{c,h}`, `src/platform/recomp_pack.{c,h}`; details and
logcat lines in [app-shell.md](app-shell.md) « Title packs »):

1. Only when no title is linked into `libmain.so` and `default.xbe` is found. Title ID from the XBE's certificate.
2. `<files dir>/packs/<TITLEID>/pack.json`; absent → `PACK_MISSING <TID>`, the shell goes on as before.
3. Checks: required fields, `format` 1, `title_id`, `abi` (`arm64-v8a`), `shell_abi` = `libmain.so`'s exported
   `xba_shell_abi` (1), `lib` a plain name, present, `lib_size` bytes. **`lib_sha256` is not checked at load**
   (private storage, atomic publish by prepare which hashed it; ~1 s per start for a large pack); the import drop,
   when it exists, checks it. Failure → `PACK_INVALID <reason>`.
4. `dlopen(lib, RTLD_NOW | RTLD_LOCAL)`; `dlsym` of `entry` (required), `recomp_lookup`, `recomp_lookup_manual`,
   `fe_menu_get_pb_state`, `g_fe_cursor`, each kept only if it lies in the pack (`dladdr`; `dlsym` on a handle also
   searches `libmain.so`). Failure → `PACK_DLOPEN <dlerror>`.
   Then `env_force`, if any: each option set over `recomp.cfg` (`recomp_pack_apply_env_force`).
5. The pointers go behind `libmain.so`'s weak forwarders; the title runs from `entry` on the guest thread, as a linked
   title does. On `PACK_INVALID` / `PACK_DLOPEN` the shell hands back to the launcher, which deletes the pack and
   offers Prepare again.

`tests/recomp_pack` checks it on Linux with a fake shell and fake packs linked the way `compile.py` links.

## Checking on a host

In `xba-android:local` (plus `python3-capstone`), with the real `libmain.so` from `android/app/src/main/cpp`
(`cmake -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a
-DANDROID_PLATFORM=30 --target main`) and the stub title's entry (`xbe_entry.c` out of
`tools/android/stub_title.sh`):

```bash
B=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64
python3 -m tools.prepare tools/conformance/test.xbe --android --work /tmp/aw --pack-dir /tmp/packs \
    --cc $B/bin/clang --sysroot $B/sysroot --shell-lib build/libmain.so --extra-src xbe_entry.c
$B/bin/llvm-readelf -d /tmp/packs/12345678/libtitle.so | grep NEEDED      # libmain.so, libm, libdl, liblog, libc
```

Checked on 2026-11-10: NEEDED `libmain.so`; every undefined symbol of `libtitle.so` (45 at -O2, 25 of them from the
toolkit) is exported by the real `libmain.so` or libc/libm/libdl/liblog; `recomp_title_main`, `xbe_entry_point`,
`recomp_lookup` exported; a second run compiles 0 files and keeps the pack; SIGTERM mid-compile exits 130 with
`CANCELLED`, leaves no compiler running and the previous pack in place, and the rerun compiles only the files
that were missing. Tests: `python -m pytest tools/prepare`.
