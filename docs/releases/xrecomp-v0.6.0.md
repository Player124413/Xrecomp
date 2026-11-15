# XRecomp 0.6.0

N64 games run faster, and the log now says where each frame's time goes. Prompted by Ocarina of Time running slowly
on an AYN Thor. Every change gives exactly the same results as before: the same pictures, sound and save states.

## Faster

- **N64 software renderer on its own threads.** The emulated CPU and RSP no longer wait while the renderer draws;
  drawing overlaps the rest of the frame. This applies when the N64 uses the software renderer (Qualcomm's own driver,
  or a Vulkan renderer that failed to start). On a desktop benchmark, a busy frame with the emulation running alongside
  goes from 27 to 15.5 ms on one renderer thread.
- **N64 RSP**: each instruction runs from a predecoded table. About 29 % fewer host instructions per RSP instruction
  on arm64 (1.2x faster on x86). This helps every game: the graphics and audio microcode run there.
- **N64 games with a pack**: code the pack does not hold (code loaded at a new address, areas not visited since the
  last Prepare) now runs nearly as fast as without a pack, instead of up to five times slower.
- **Fast cores on Android** (GBA, PS1, N64): the renderer threads now go back to the fast cores after you leave the
  app and return, like the main thread. The game tells Android how long each frame really takes (Android's
  performance hint, Android 13+), so the clocks follow the game. The emulation and renderer threads run at
  display priority.

## Diagnostics

- **N64 performance line.** Every 10 s, and once when the game is closed, the log gets a `n64: perf …` line next
  to the frame rate. It shows:
  - the speed as a percentage of full speed;
  - the time per frame spent in the CPU, RSP, renderer (and waiting for it), display, sound and sleep;
  - the slowest frame;
  - with a pack, the share of code it ran;
  - the renderer in use, the internal resolution, rewind on or off, and the CPU core and its clock.

  Please attach these lines to N64 speed reports (`adb logcat | grep n64:`).
- **On-screen controls debug log**: `adb shell setprop log.tag.XRecompTouch DEBUG` logs where each finger goes,
  which view takes it, and what reaches the game. A failure to reach the game's native code is now logged instead of
  silently ignored.

## Known problems

- Nothing in this release has been measured on a phone yet; the gains above are desktop measurements.
- On-screen controls not responding on the AYN Thor (reported with 0.5.1): not found yet; logs with the debug switch
  above help.
- The Vulkan N64 renderer's own threads are not yet moved back to the fast cores after a resume.
- From 0.5.0: N64 rewind costs up to a few ms per capture, hence off by default; no save states, rewind or
  fast-forward for Xbox titles.
- From 0.4.0: N64 on Qualcomm's own driver uses the software renderer; N64 upscaling needs a Turnip driver.

## Before you install

- Updates 0.5.1 and older, and `xrecomp-latest`, in place (same signing key).
- Games prepared with 0.5.x stay prepared, and save states made with 0.5.x still load.
- **Bring your own games.** XRecomp contains no game, no BIOS dump and no cover art. Use only games you dumped
  yourself.
- Bug reports: the repository's issues, with your device, Android version, the version from About, the game's
  console, the Vulkan driver for N64, and `adb logcat -s xboxrecomp SDL AndroidRuntime DEBUG`
  ([CONTRIBUTING.md](../../CONTRIBUTING.md)). Never attach game files.
