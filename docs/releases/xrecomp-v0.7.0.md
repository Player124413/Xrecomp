# XRecomp 0.7.0

Ocarina of Time holds 50 fps at 4x upscaling on an AYN Thor (Turnip driver), where 0.6.0 showed about 33. The library
now says which games are prepared, and the on-screen controls and in-game menu are sturdier.

## Faster (N64)

Measured on an AYN Thor, Ocarina of Time PAL, Vulkan renderer, upscale 4x:

| | 0.6.0 | 0.7.0 |
|---|---|---|
| Frames shown per second | ~33 | 50 |
| Pictures dropped per 10 s | ~167 | 0-3 |
| Vulkan renderer start (driver open to game start) | 1.85 s | 0.67 s |
| Shader compile stalls per launch | about 12 (up to 400 ms each) | 0 from the second launch |
| CPU time per frame, after a new Prepare | 2.8 ms | 2.4 ms |

- **A picture is made only when it changed.** Many games draw a new picture every second or third screen refresh;
  the others no longer read the picture back from the GPU, convert it and upload it again. Frame skip now decides per
  new picture, so a late frame no longer drops a picture the game drew.
- **Shader cache.** The Vulkan N64 renderer keeps its compiled pipelines per driver (in the app's cache folder) and
  Mesa's own shader cache next to them: only the first launch compiles.
- **Renderer threads in the performance session.** The Vulkan renderer's own threads join Android's performance hint
  and go back to the fast cores after a resume.
- **N64 packs.** The Prepare no longer turns compressed data into thousands of useless functions (Ocarina of Time:
  2112 functions instead of 24 880; 99 % of the code native instead of about 75 %), code the pack does not hold runs
  as fast as without a pack, and the code found while playing is recorded once per address and saved when the game
  pauses, not only at exit. **Prepare N64 games again** to get this; a pack from 0.6.0 still plays.
- PS1: the code found while playing is recorded once per address too.

## Library and Home

- **Each card says whether the game is prepared**: « Prepared » (green), « Prepared · out of date » (amber: made by an
  older version of the app, Prepare again for its fixes) or « Not prepared » (GBA, PS1 and N64 games still play, in
  the interpreter).
- A GBA, PS1 or N64 game's Home offers **Prepare again** when its pack comes from an older version of the app, and
  counts the new code found while playing again (it read 0 once the N64's record grew past 16 MB).

## In-game menu

- Scale, filter and picture shader apply live, while the game runs. Settings read once at start show « Restart to
  apply »; two presses save the automatic slot and restart the game.
- Rows whose label and value do not fit side by side are stacked; the list no longer draws over the header.

## On-screen controls

- The touch layout editor can no longer be mistaken for the game: a banner (Editing touch layout, Cancel, Save) and
  a dark tint; it is cancelled when the game stops, resumes or the menu opens.
- Saved layouts are repaired at load; one with no reachable direction control or face button falls back to the
  console's defaults. Settings › Controls › Reset touch layouts removes them all.
- Hotkey chords no longer swallow the on-screen buttons (GBA, PS1, N64).
- When the pad hides itself (Auto, companion panel), a tap on the game screen shows it again.

## Diagnostics

- The `n64: perf` line now separates the RSP from waiting on the renderer: `rdp X (queue Y, sync Z, scanout W)`.
- A new `n64: perf detail` line every 10 s: pictures per second, unchanged and skipped refreshes, full renderer
  queues, shader compile stalls, and where the slowest frame's time went.

## Known problems

- One frame in three still takes more than 20 ms at 4x in Ocarina of Time (the renderer's command queue fills up);
  it no longer shows on screen, but there is no headroom yet. Being worked on.
- Upscale 8x falls back to 4x on the Thor's Turnip driver (it needs a 512 MB buffer, the driver allows 128 MB).
- Text and 2D art (texture rectangles) are drawn at 1x when upscaling, so they look blocky;
  `RECOMP_N64_UPSCALE_NATIVE_TEXRECT=0` in `recomp.cfg` upscales them too (may show seams in some games).
- The N64's record of code found while playing grows by tens of MB per session with a pack from 0.6.0; Prepare again.
- From 0.5.0: N64 rewind costs up to a few ms per capture, hence off by default; no save states, rewind or
  fast-forward for Xbox titles.
- From 0.4.0: N64 on Qualcomm's own driver uses the software renderer; N64 upscaling needs a Turnip driver.

## Before you install

- Updates 0.6.0 and older, and `xrecomp-latest`, in place (same signing key).
- Games prepared with 0.6.0 stay playable; N64 games show « Prepared · out of date » until prepared again. Save
  states made with 0.6.0 still load.
- **Bring your own games.** XRecomp contains no game, no BIOS dump and no cover art. Use only games you dumped
  yourself.
- Bug reports: the repository's issues, with your device, Android version, the version from About, the game's
  console, the Vulkan driver for N64, and `adb logcat -s xboxrecomp SDL AndroidRuntime DEBUG`
  ([CONTRIBUTING.md](../../CONTRIBUTING.md)). Never attach game files.
