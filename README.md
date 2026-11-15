<p align="center"><img src="docs/images/xrecomp-icon.svg" width="128" alt="XRecomp icon"></p>

<h1 align="center">XRecomp</h1>

<p align="center"><b>Your console games, rebuilt as native apps.</b><br>
No emulator. No JIT. The game's machine code becomes C, then a real executable for your phone or computer.</p>

<p align="center"><a href="https://github.com/MisterAlex95/Xrecomp/actions/workflows/linux.yml"><img src="https://github.com/MisterAlex95/Xrecomp/actions/workflows/linux.yml/badge.svg" alt="Linux tests"></a> <a href="https://github.com/MisterAlex95/Xrecomp/releases/latest"><img src="https://img.shields.io/github/v/release/MisterAlex95/Xrecomp?label=release" alt="Latest release"></a> <img src="https://img.shields.io/badge/licence-MIT-blue" alt="MIT licence"> <img src="https://img.shields.io/badge/consoles-Xbox%20%C2%B7%20N64%20%C2%B7%20PS1%20%C2%B7%20GBA-green" alt="Consoles"></p>

<p align="center">
<a href="#-try-it-in-five-minutes">Try it</a> ·
<a href="#-whats-running-today">What runs</a> ·
<a href="#-how-it-works">How it works</a> ·
<a href="#-jump-in">Contribute</a> ·
<a href="docs/INDEX.md">Docs</a>
</p>

---

**Ocarina of Time at full speed on an Android handheld.** Crash Bandicoot. Pokémon Pinball. Fable booting into
Oakvale with music. Not through an emulator interpreting instructions one by one, but as programs **compiled ahead
of time** from the original game code, running against a runtime that stands in for the console's hardware and OS.

And you don't need a PC for it: point the Android app at a game you dumped, press **Prepare**, wait 20–30 minutes
once, press **Play**. The whole recompiler (Python, capstone, LLVM) runs on the phone.

<table>
  <tr>
    <td width="50%" align="center"><img src="docs/images/screens/n64-ocarina-of-time.jpg" alt="The Legend of Zelda: Ocarina of Time, Link meets Saria in the Kokiri Forest"><br><sub><b>Ocarina of Time</b> · N64 · Vulkan RDP, upscaled</sub></td>
    <td width="50%" align="center"><img src="docs/images/screens/ps1-crash-bandicoot.jpg" alt="Crash Bandicoot on N. Sanity Beach, upscaled"><br><sub><b>Crash Bandicoot</b> · PS1 · 4× internal resolution</sub></td>
  </tr>
  <tr>
    <td width="50%" align="center"><img src="docs/images/screens/xbox-kotor.jpg" alt="Star Wars: Knights of the Old Republic, in game on the Endar Spire"><br><sub><b>Star Wars: KotOR</b> · Original Xbox · in game</sub></td>
    <td width="50%" align="center"><img src="docs/images/screens/gba-pokemon-pinball.png" alt="Pokémon Pinball: Ruby &amp; Sapphire, Ruby table"><br><sub><b>Pokémon Pinball: Ruby &amp; Sapphire</b> · GBA · 60 fps</sub></td>
  </tr>
  <tr>
    <td width="50%" align="center"><img src="docs/images/screens/android-library.jpg" alt="The XRecomp library on Android: Xbox, PS1 and GBA games side by side"><br><sub><b>The launcher</b> · every console in one library</sub></td>
    <td width="50%" align="center"><img src="docs/images/screens/android-companion.png" alt="The companion panel on the AYN Thor's second screen: fps, battery, screenshot, quick menu"><br><sub><b>Dual screen</b> · companion panel on the Thor's second screen</sub></td>
  </tr>
</table>

<p align="center"><sub>All captured on an AYN Thor (Snapdragon 8 Gen 2) from games dumped from original discs and cartridges.</sub></p>

## ✨ Why it's interesting

- 🔁 **Static recompilation, four CPUs.** x86 (Xbox), MIPS R4300i (N64), MIPS R3000A (PS1) and ARM7TDMI (GBA) all
  lift to portable C. When the static pass misses code, an interpreter takes over and records the address so the
  next Prepare compiles it too.
- 📱 **Prepare on the device.** Analysis, lifting and compilation happen inside the app. No toolchain to install, no
  desktop step.
- 🎮 **A real launcher, not a demo.** Library with cover art, gamepad-first UI, dual-screen support, per-game
  options, save states with thumbnails, quick save / load on the pad, *resume where I left off* (or start from the
  boot), fast-forward.
- 🖼️ **Better than the original.** PS1 and N64 at 2×, 4× or 8× internal resolution, frame generation (LSFG) on every
  console, custom Vulkan drivers (Mesa Turnip) downloaded from the app.
- 📦 **One game, one app.** Turn a prepared game into its own standalone Android or macOS app.
- 🧪 **Tested like a compiler.** Linux x86-64 and arm64 CI on every push, a conformance suite that checks lifted C
  against a real x86 CPU, and the community test ROMs of each console (n64-systemtest, jsmolka/gba-tests, …).

## 🚀 Try it in five minutes

> **Bring your own games.** XRecomp ships no game, no BIOS and no cover art. Use games you dumped from your own
> discs and cartridges. Nothing you prepare leaves your machine.

**Android** (arm64, Android 11+)

1. Install `xrecomp-v<version>.apk` from the [latest release](https://github.com/MisterAlex95/Xrecomp/releases/latest)
   (or [`xrecomp-latest`](https://github.com/MisterAlex95/Xrecomp/releases/tag/xrecomp-latest), rebuilt from every
   push; both update each other in place).
2. Add a game: an ISO or extracted Xbox folder, a `.gba`, a PS1 `.cue` / `.bin` / `.iso`, a `.z64` / `.n64` / `.v64`.
3. **Prepare**, then **Play**.

For N64 on Adreno GPUs, pick a Turnip driver in Settings › Graphics › Vulkan driver: the app lists and downloads them
([details](docs/android/app-shell.md#vulkan-drivers)).

**macOS** (Apple Silicon / Intel)

Download `xrecomp-v<version>-macos-arm64.zip` from the [latest release](https://github.com/MisterAlex95/Xrecomp/releases/latest),
unzip, run `xattr -dr com.apple.quarantine XRecomp` (the app is not notarized) and open `XRecomp/XRecomp.app`, kept
in its folder. It sets up its own Python environment and prepares and plays your library
([launcher guide](docs/macos/launcher.md)). Or build it: `bash macos/scripts/build-app.sh`.

**Linux** (x86-64 / arm64), or any terminal

```bash
bash tools/linux/install_deps.sh            # apt / pacman / dnf  (macOS: bash tools/macos/setup.sh)
python3 -m pip install capstone
python3 -m tools.prepare /path/to/game.iso  # → <title id>-work/ with the native executable
```

`--check` tells you whether a game is prepared and up to date, `--help` lists the options.

**Windows** follows upstream's manual route (MSVC, D3D11), not yet tested by this fork:
[GETTING_STARTED.md](docs/GETTING_STARTED.md) walks through each pipeline stage, which is also the way to bring up
a new game on any host.

## 🎮 What's running today

| Console | CPU | Status | Highlights |
|---|---|---|---|
| **Nintendo 64** | MIPS R4300i | 🧪 Playable | *The Legend of Zelda: Ocarina of Time* (PAL) at full speed on an AYN Thor. LLE RSP, RDP on Vulkan (paraLLEl-RDP) with upscaling, software fallback, emulated PIF, no symbols needed |
| **PlayStation** | MIPS R3000A | 🧪 Playable | *Crash Bandicoot*. No BIOS image needed (HLE BIOS in C, OpenBIOS optional), overlays recompiled, save states, upscaling |
| **Game Boy Advance** | ARM7TDMI | 🧪 Playable | *Pokémon Pinball: Ruby & Sapphire*. ARM + Thumb, all save types, save states |
| **Original Xbox** | x86 (Pentium III) | ⚙️ Booting games | *Fable: The Lost Chapters* reaches Oakvale with music and pad. Several games boot, a few are playable; D3D8 / NV2A → OpenGL (ES) or D3D11 |

| Host | Play | Prepare | Front end |
|---|---|---|---|
| **Android** arm64, GLES 3.0 | ✅ | ✅ on the device | XRecomp app (AYN Thor first): launcher, gamepad UI, dual screen |
| **macOS** | ✅ | ✅ | XRecomp.app, or the command line |
| **Linux** x86-64 / arm64 | ✅ | ✅ | command line |
| **Windows** x86-64 | ⚠️ upstream route | ⚠️ manual | command line |

> **Status: experimental, moving fast.** Expect crashes and the odd re-Prepare after an update. Per-game status
> lives in [`profiles/titles/*/`](profiles/titles/) (`title.json`, `NOTES.md`); what changed is in the
> [release notes](docs/releases/README.md).

## 🔧 How it works

```
 your disc ─► executable ─► parse ─► disassemble ─► find functions ─► calling conventions ─► lift to C
                                        (tools/, Python + capstone)                              │
                                                                                                 ▼
      native game ◄── link ◄── compile (clang / gcc / MSVC) ◄──────── C project + title profile
          │
          └─ runs on the runtime (src/): kernel · GPU → OpenGL (ES) / Vulkan / D3D11 · audio · input
                                      └─ on a console-agnostic host layer (src/host)
```

- **Pipeline** (`tools/`, `consoles/*/tools/`): Python. `tools.prepare` runs it end to end
  ([tools/README.md](tools/README.md)).
- **Runtime** (`src/`, `consoles/*/runtime/`): static C libraries that replace the console's kernel, GPU, audio and
  input, on one host layer for Android, POSIX and Windows ([src/README.md](src/README.md)).
- **Title profiles** (`profiles/`): per-game fixes and switches, picked at prepare time. Game-specific code lives
  only here ([profiles.md](docs/technical/profiles.md)).

Want the deep end? [Register model](docs/technical/register-model.md) ·
[Indirect calls](docs/technical/indirect-calls.md) · [GPU back end](docs/technical/gpu-backend.md) ·
[Pushbuffer executor](docs/technical/pushbuffer-executor.md) · [Kernel replacement](docs/technical/kernel-replacement.md) ·
[Other consoles](consoles/README.md)

## 🙌 Jump in

There's room for you whether you like compilers, GPUs, Android or just testing games you own.

| If you like… | Good places to start |
|---|---|
| **Trying games** | Run a game you own, report how far it gets (device, version from About, logcat). [Candidate games](docs/technical/candidate-games.md) lists likely next targets |
| **Compilers & reverse engineering** | The lifters in `tools/` and `consoles/*/tools/`, indirect calls, function discovery. Start with [GETTING_STARTED.md](docs/GETTING_STARTED.md) |
| **Graphics** | NV2A / D3D8 on GLES, N64 upscaling, PS1 GPU. [GPU back end](docs/technical/gpu-backend.md) |
| **Android / macOS apps** | Editable touch layouts, CHD support, launcher polish. [App shell](docs/android/app-shell.md), [Mac launcher](docs/macos/launcher.md) |
| **Emulation accuracy** | Save states for Xbox and N64, kernel and APU behaviour, test ROM coverage |
| **Windows** | Bring the Windows host up to par with Linux and macOS |

Build and test locally:

```bash
cmake -S . -B build && cmake --build build                      # runtime libraries
python3 -m pytest tools/ consoles/                               # pipeline tests, no game files needed
docker build -f tools/docker/linux.Dockerfile --target test .    # exactly what CI runs
```

Then read [CONTRIBUTING.md](CONTRIBUTING.md) (« This fork » first) and open an
[issue](https://github.com/MisterAlex95/Xrecomp/issues) or a pull request. More on tests (conformance suite, arm64
qemu, Android build): [docs/upstream/README.md](docs/upstream/README.md#running-the-tests),
[docs/android/README.md](docs/android/README.md).

<details>
<summary><b>Repository map</b></summary>

| Path | What |
|---|---|
| `tools/` | Xbox pipeline: `xbe_parser`, `disasm`, `func_id`, `abi_analysis`, `recomp`, `prepare`, `compat`, … |
| `src/`, `include/` | Runtime: kernel, d3d, nv2a, apu, audio, input, usb, video, platform, host |
| `consoles/` | GBA, PS1, N64: runtime, recompiler pipeline and tests for each |
| `profiles/` | Per-title and per-engine layers applied by Prepare |
| `templates/new-game/` | The game project Prepare (or you) fills with generated C |
| `android/` | Android app (launcher, embedded Prepare) |
| `macos/` | macOS launcher (SwiftUI) |
| `tests/` | C tests, one CMake project each (exit 77 = skipped) |
| `docs/` | Everything else, with a by-symptom table: [docs/INDEX.md](docs/INDEX.md) |

</details>

## 💛 Standing on shoulders

XRecomp started as a fork of [**sp00nznet/xboxrecomp**](https://github.com/sp00nznet/xboxrecomp): the Xbox runtime
and recompilation pipeline are the work of sp00nz and its contributors ([CONTRIBUTORS.md](CONTRIBUTORS.md)). This
fork follows upstream (`tools/sync_upstream.sh`) and adds the Android host, the launchers, Prepare, the other consoles
and the per-title work. Please report problems with XRecomp here, not upstream. Upstream's own README, FAQ and
community ports: [docs/upstream/README.md](docs/upstream/README.md).

The APU and NV2A code derives from [xemu](https://xemu.app); the N64 RDP uses paraLLEl-RDP; the apps bundle SDL2,
CPython, capstone and LLVM. `XBA-nn` references in commits are tickets of the maintainers' tracker; the reasoning is
always in the commit message and linked doc.

## ⚖️ Legal and licence

XRecomp is not affiliated with or endorsed by Microsoft, Nintendo, Sony or any game publisher; all trademarks belong
to their owners. Use it only with games you own. Issues or pull requests containing game code, executables, disc or
ROM images, BIOS dumps, game data or C generated from a game are deleted.

MIT ([LICENSE](LICENSE)). Third-party parts are listed in [NOTICE](NOTICE) and [LICENSES/](LICENSES/) (the
xemu-derived APU and NV2A code is LGPL-2.1+); the apps show them under About › Licences. Security issues:
[SECURITY.md](SECURITY.md).
