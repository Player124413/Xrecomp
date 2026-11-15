# Android app shell

The APK every title starts from (XBA-18): `android/` (Gradle, AGP 8.7.3) plus `android/app/src/main/cpp`.

```bash
docker build -f tools/docker/android.Dockerfile --target apk-out --build-arg XBA_VERSION=$(git rev-parse --short HEAD) \
    --output type=local,dest=out .       # → out/xboxrecomp-shell.apk
adb install -r out/xboxrecomp-shell.apk
adb logcat -s xboxrecomp SDL
```

The CI builds it on every push to `main`. The latest one is always the prerelease **shell-latest** of the repo
(XBA-20), on the maintainers' Gitea only: players install the XRecomp APK from the
[releases](https://github.com/MisterAlex95/Xrecomp/releases) instead.

This plain shell has its own application id, `fr.adserver.xrecomp.dev`, and shows as « XRecomp shell »
(XBA-134): it installs next to the XRecomp APK of `xrecomp-latest` (`fr.adserver.xrecomp.shell`, the app a
player uses) and never replaces it. The check lists below name `fr.adserver.xrecomp.shell`, the XRecomp APK's;
with the plain shell, use `fr.adserver.xrecomp.dev` in their `adb` paths and `run-as`.

## How it is put together

| Piece | What |
|---|---|
| `LauncherActivity` | The launcher entry ([The launcher](#the-launcher)): welcome, game choice and import, home (the game's art and title, one line, a solid Play / Prepare and three quiet pills: Settings, Controls, Manage), in its own process `:launcher`. `Theme.java` and `Widgets.java` draw it, `TitleArt.java` decodes the game's title image, `GameArt`, `GamePalette` and `GameTheme` make the [game theme](#game-theme) from it, `UiFeedback.java` plays its sounds and haptics; `RecompConfig.java` writes its settings to `recomp.cfg`. With a valid game and « Open the game directly next time » on, it starts `ShellActivity` at once and shows nothing. |
| `ShellActivity` | Subclass of SDL's `SDLActivity`, which loads `libSDL2.so` and then `libmain.so`. SDL's Java side is compiled from the pinned sources (`$SDL2_SRC/android-project`), not copied here. Not exported: started by the launcher, after it sets `RECOMP_GAME_DIR`. Holds the [in-game menu](#in-game-menu) (`GameMenu`), a View layer over SDL's surface. |
| `libxba_import.so` | The launcher's ISO import: `xiso_jni.c` around the static `xiso` target (`src/xiso`); and « Choose Lossless.dll » ([Frame generation](#frame-generation)): `lsfg_jni.c` around the shader cache (and the DXBC translator, in every build with frame generation: the default). Loaded only in `:launcher`. |
| `libmain.so` | `shell_main.c` + the **whole** toolkit (`--whole-archive`) + the title, when one is linked (see [A title in the shell](#a-title-in-the-shell)). Loading it on a device proves that bionic resolves every symbol the runtime uses. Four symbols that a title provides are weak stubs in `shell_main.c`, and so is the title's hook `recomp_title_main`. |
| logcat | `src/platform/android_log.c`: stdout (INFO) and stderr (WARN) go to the tag `xboxrecomp`, one entry per line. `xbox_android_log_init()` is the first call of `SDL_main`. |
| Window | Black, fullscreen, landscape (`sensorLandscape` + `SDL_HINT_ORIENTATIONS`). |
| Back | Opens and closes the [in-game menu](#in-game-menu) (ShellActivity takes it before SDL); quitting is in the menu (see [Quit](#quit)). A gamepad's own Back key stays that pad's Select for the game, as before. SDL still traps Back (`SDL_HINT_ANDROID_TRAP_BACK_BUTTON`); its old Back-twice path (`QUIT_PROMPT` toast, twice within 2 s quits) is only reached where nothing above SDL takes Back. |
| Manifest | minSdk 30, target 35, arm64-v8a only, GLES 3.0 required, `allowNativeHeapPointerTagging="false"`, `isGame`. |
| Frame generation | On by default, sources fetched at pinned commits ([Frame generation](#frame-generation)); `-PxbaLsfg=0` leaves it out. |
| Per title | `-PxbaTitleDir=…` (the title's project), `-PxbaApplicationId=…`, `-PxbaLabel=…`, `-PxbaVersionCode=…`, `-PxbaXbeSha256=…` (the expected `default.xbe`, [WRONG_GAME](#wrong_game)). The title is linked next to `shell_main.c`, which stays the entry (XBA-52). |
| Assets | `selftest.xbe`: the synthetic XBE of `tools/conformance` (ours, no game content), copied in at build time for the [self-test](#self-test-in-the-app-process). |
| Signing | Debug key generated once per toolchain image, so updates install in place while the image is cached. The release key will be a build secret, never in the repo. |

## Check list on the device (Alex)

1. `adb install -r xboxrecomp-shell.apk`, then `adb logcat -c && adb logcat -s xboxrecomp SDL AndroidRuntime DEBUG`.
2. Launch « XRecomp ». Expected: a black screen, fullscreen, landscape. logcat shows
   `xboxrecomp ready (build <commit>, SDL 2.32.10, <w>x<h>)`. Without game data the launcher's welcome screen shows
   instead ([The launcher](#the-launcher)); push any `default.xbe` to `files/game/` first and press Play on the
   launcher's home: the shell then shows a « Game data found … This APK has no recompiled game in it » message box
   (`NO_TITLE`, logcat `(no title linked)`); OK, and the black screen stays.
3. Press Back once. The in-game menu slides in from the left; logcat: `shell: menu open (Back)`. The app stays open.
4. Quit game, then Quit game again (the second press confirms). logcat: `shell: menu: quit (confirmed)`, then
   `clean exit`, and the app closes.
5. Check there is no tombstone: `adb shell ls /data/tombstones` (root) or no `DEBUG` / `F libc` lines in logcat.
6. Send a screenshot (`adb exec-out screencap -p > shell.png`) and the logcat lines.

## RECOMP_* options (recomp.cfg)

There is no shell environment on Android. The shell therefore reads `recomp.cfg` from the app's external files dir
before anything starts, and puts each line in the environment (`src/platform/recomp_cfg.c`, XBA-26):

```bash
printf 'RECOMP_WAIT_LOG=1\n# comments and blank lines are fine\n' > recomp.cfg
adb push recomp.cfg /sdcard/Android/data/fr.adserver.xrecomp.shell/files/recomp.cfg
```

logcat then shows one line: `recomp.cfg: 1 option(s) from …: RECOMP_WAIT_LOG=1`. Without the file it shows
`recomp.cfg: none (read from … when present)`, which is not an error. A variable that is already in the environment
wins over the file. The list of switches: `docs/runtime/environment-variables.md`. The launcher's Settings screen
writes a few of them into the same file and keeps the lines written by hand ([Settings](#settings)).

## Game data and saves

Right after `recomp.cfg`, the shell resolves two directories (`src/platform/recomp_storage.c`, XBA-23) and hands them to
`xbox_path_init(game_dir, save_dir)`, which maps `D:\` and `\Device\CdRom0\` to the first and the save drives and
partition images to the second:

| What | Where | Why there |
|---|---|---|
| Game data (`default.xbe` and the disc's files) | The folder chosen in the [launcher](#choosing-the-game), else `/sdcard/Android/data/<app id>/files/game/` (where the launcher's imports go too), or `RECOMP_GAME_DIR` from `recomp.cfg` | Reachable with `adb push` and a file manager, no permission needed; uninstalling the app removes it. |
| Saves, `Partition0-5.img` | `/data/data/<app id>/files/save/` | Private to the app, kept when it is killed or updated. The cache partition images are sparse: they take no space until written. |

Both are created at start, so the `game/` folder is there to push into. The lookup of `default.xbe` is
case-insensitive, like the kernel's. logcat shows `storage: game=… save=… (default.xbe found|missing)` and the kernel's
`Path init: game=…, save=…, user data=…`.

When `default.xbe` is missing, logcat shows `NO_GAME_DATA: default.xbe not found in …` and, instead of starting
anything (with or without a title linked), the shell opens the launcher on its game choice, naming that folder
(`ShellActivity.openGamePicker`, called from `shell_main.c` over JNI) and finishes. If that call fails it shows the
old « No game data found » box with the folder to copy the disc to. The launcher normally catches a missing game before
the shell starts; this is the path for a folder that went away in between, or `RECOMP_GAME_DIR` in `recomp.cfg`
pointing nowhere.

Device check (XBA-23):

1. Start the shell once with nothing pushed. Expected: the launcher's welcome screen, and logcat
   `launcher: shown (no game set up)`; nothing from the runtime, which has not started.
2. Push any file named `default.xbe` (one of our nxdk samples will do):
   `adb push default.xbe /sdcard/Android/data/fr.adserver.xrecomp.shell/files/game/`, then start the app again.
   Expected: the launcher's home, chip « Ready »; Play: logcat `storage: … (default.xbe found)` and
   `game data: …/game/default.xbe`.
3. `adb shell run-as fr.adserver.xrecomp.shell ls -ls files/save` (debug APK). Expected: `TitleData`,
   `UserData`, `Cache`, `SystemData` and `Partition0.img` … `Partition5.img`, the last ones taking about 0 blocks.
4. Kill the app (`adb shell am force-stop fr.adserver.xrecomp.shell`), restart it, run step 3 again: the same
   files, still there.
5. Send the logcat lines and the `ls` output.

### Extracting a disc image in the app (XBA-40)

When the player picks an Xbox disc image (XISO) and it cannot be read in place
([Disc images read in place](#disc-images-read-in-place-xba-126), the default since XBA-126), the app extracts it
once into the game folder. It does this with `src/xiso` (target `xiso`, header `xiso.h`, libc only), which takes the fd that
the ContentResolver opened and only needs `pread` on it:

- `xiso_probe_fd` reads the volume, checks the whole directory tree and returns the title from `default.xbe`'s
  certificate, the total size, and whether the root holds a `default.xbe`. The app can show these and check the free
  space before it writes anything.
- `xiso_extract_fd(fd, out_dir, progress, user)` writes into `out_dir.partial`. Once every file is written, it
  replaces `out_dir` as a whole (the old one goes to `out_dir.old`, which is then deleted). If the run is cancelled
  (the callback returns nonzero) or fails, `out_dir` is untouched and `.partial` is removed. Progress is reported per
  file and every 4 MB.
- Images: a bare game partition (extract-xiso, xdvdfs) or a redump full-disc image (game partition at 0x18300000).
  If the image is corrupt (truncated, offsets out of range, cycles in the tree, names like `..` or with `/`), the call
  returns `XISO_ECORRUPT` and nothing is written outside `out_dir`. `xiso_strerror` gives a message for each code.

On a desktop, `xiso_extract image.iso outdir` (built by `tests/xiso`, `tools/xiso/README.md`) does the same.

In the app the launcher calls it into `files/game.import`, never into `files/game` itself, and swaps the result in
once checked ([Choose disc image (ISO)](#choose-disc-image-iso)).

## The launcher

(XBA-41, around the game picker of XBA-54 and the import of XBA-40.) The app's launcher entry is
`LauncherActivity`, in its own process `:launcher`: it never loads `libSDL2.so` or the runtime, and after a new choice
it can end the game's process and start a fresh one (the runtime maps its memory once per process). It is made for a
gamepad first, in landscape, and works by touch too.

How it is built: framework `Activity` and `View`s only (no AndroidX, no Material, no new Gradle dependency), drawn in
code (`Theme.java` for colours, type, spacing and sizes; `Widgets.java` for the custom views: list rows, pill
buttons, tabs, the Options sheet, the focus highlight, the import ring, status dots, button hints). Everything is laid out on a 1920 x 1080 design grid that `Widgets.Stage` scales to the
window (wider or taller screens get a wider or taller grid, never a stretched one), so it looks the same from 1280 x 720
to 1920 x 1080. The window is fullscreen and immersive (system bars hidden, back with a swipe), drawn under the display
cutout, and the stage keeps its content out of the cutout's insets. System fonts only (`sans-serif`,
`sans-serif-medium`); the look is our own (deep green-black, one vivid green accent, warm off-white text), with no
console or game maker's logo or name in the art.

### Screens

One activity, a small state machine of screens (`LauncherActivity.Screen`) with a back history instead of fragments:

| Screen | What it shows | Reached from |
|---|---|---|
| Library | The [game library](#game-library): a full-width grid of the games (five columns), the focused game's state and play time as one context line at the bottom left; Rescan, Storage, Library folders, Settings and About in the **Options** sheet (Y). Over the grid the **Search** and **Sort** pills, « Recently played » and the favourites first, the console tabs in the top bar ([Library view](#library-view-xba-206)). | Every start when the library is on, B from a game's Home. |
| Storage | What the app stores per game, each deleted on its own ([Storage](#storage)). | Options (Y) on the library. |
| Welcome | What the app is (a recompiled game, the game not included: bring your own copy), three plain steps, a « Get started » button. | First start, when no game is set up at all. |
| Choose your game (source) | A list: **Game folder** (read in place with All files access, otherwise copied; the value says which), **Disc image (ISO)** (extracted once; the free space in the top bar), **Game library folder**; the focused choice explained on the right. | Welcome, Manage › Change game, Play with no usable game, the « Change game » shortcut, `NO_GAME_DATA`. |
| Read the game in place? | Only without All files access: **Allow all files access** (recommended; opens the system page, then the folder picker) and **Copy into the app** (the folder picker), with the explanation on the right (in place: no copy, fine for a 4 GB disc, only that folder is read; copy: as much free space again, a few minutes, removed with the app). | Game folder. |
| Preparing the game (import) | A plain progress ring (percent), the step (« Checking », « Copying », « Extracting »), « X of Y », the current file, the free space. The screen stays on; B cancels (nothing to focus). | A picked folder or image. |
| Home | No top bar: the game's title image (or a monogram) in the [game theme](#game-theme)'s colours, its title (the XBE certificate's name, else the folder's name), **one line** — when it was played (« Yesterday · 12 h 40 min played », « Not played yet »), or its state when it is not ready (« Needs preparing · once, about 20–30 min », « Prepare failed · CODE », the Prepare's progress, « Queued (n) », « Game data missing » with the folder, …) — and **one row of buttons**: a solid **Play** (or **Prepare**, **Preparing…**, **See what failed**, **Choose a game**) and three quiet pills, **Settings**, **Controls**, **Manage**. Before the first play the note says where the in-game menu is. | A finished import (Play focused), every start when « Start in the game » is off, the « Launcher » shortcut, B from the screens above. |
| Settings | Four tabs (General, Graphics, Audio, Advanced; L1 / R1) of [options](#settings) as « label  value » rows, left / right (or A) to change, the focused option's description on the right. About ends Advanced. | Home, Options. |
| Vulkan drivers | « System driver », the installed drivers, « Import a zip », « Refresh the list », the drivers one can download ([Vulkan drivers](#vulkan-drivers)). A chooses, X twice deletes, Y refreshes. Not in a game's own app. | Settings › Graphics › Vulkan driver. |
| Controls | The player's **remapping** ([Controls and remapping](#controls-and-remapping)): one row per Xbox control (A B X Y, Black, White, the triggers; Back, Start, the stick presses; the D-pad; the two sticks), its value the player's button, changed like a setting; « Reset controls » (also Y). The description column says what the focused row does and draws the handheld with, next to each control, what the game gets from it. | Home. |
| Controls (GBA, PS1, N64) | The console's buttons remapped, for all its games or this game only ([Controls for the other consoles](#controls-for-the-other-consoles)): Mapping for, one row per console button, the sticks, the N64 stick's range and dead zone, the on-screen pad, « Reset controls » (also Y). | A GBA / PS1 / N64 game's Home (its « Controls » pill). |
| Manage | **Change game** (another folder or image, or the same one again to re-import; **Library** when the [game library](#game-library) is on); **Delete the imported copy** (only when the game is the app's own copy, `files/games/<TID>/` or the older `files/game/`; its size as the value; press A twice within 5 s; saves are kept); **Add to home screen** (a [shortcut for this game](#a-game-on-the-home-screen), only when the home screen takes pin requests); **Create an app** (the game's [own app](#a-games-own-app), made and installed on the device; its size as the description; **only for a prepared game** (a valid pack built from this disc): before that the row says « Prepare this game first » and pressing it only repeats that in the note, so the app it makes always takes the pack over and starts straight into the game; not in a game's own app); the game's pack, Prepare files, caches and saves with their sizes, each deleted on its own ([Storage](#storage)). The technical facts (title ID, prepared date, pack size, folder, source, size, free space) are the first row's description. | Home. |
| About | Version (`-PxbaVersion`), build (`-PxbaVersionCode`), app id, the game check (the first digits of `-PxbaXbeSha256`, or none), a one-line licence summary, and that no game data is included. **X**: Licences. | The end of Settings › Advanced, Options on the library. |
| Licences | XBA-100. Every licence text the APK carries, monospaced, one after the other: `LICENSE`, `NOTICE`, then `LICENSES/*.txt` (copied into the assets `licences/` at build time by `xbaLicencesAsset`). D-pad up / down scroll, L1 / R1 page, a finger scrolls too. | X on About. |

Messages and errors (`NOT_A_GAME_FOLDER`, `WRONG_GAME`, `NOT_ENOUGH_SPACE`, « Import canceled », « Saved », ...) go to
the left of the footer line (the hints are on its right), with a dot coloured by kind; there are no toasts and no dialogs. logcat (tag `xboxrecomp`):
`launcher: shown (...)` naming why, then one `launcher:` line per step (folder, hash, copy, import, game set, play,
settings saved, errors).

### When it shows

| Case | What happens |
|---|---|
| A usable game and « Start in the game » off (the default) | Home, Play focused. |
| A usable game and « Start in the game » on | `ShellActivity` starts at once; nothing shows. (A game's own app has no such setting: it always does this.) |
| The game is running in the background and the app icon is pressed | Back to the game at once, whatever « Start in the game » says. |
| Nothing remembered, but `RECOMP_GAME_DIR` in `recomp.cfg`, or `default.xbe` pushed to `files/game/` | A usable game, as above: the adb workflow still works (source « Set in recomp.cfg » or « Imported copy »). |
| Nothing set up | Welcome. |
| The remembered folder is gone (deleted, SD card out) | Home, « Game data missing » and the old path on its line (`GAME_FOLDER_GONE`), « Choose the game again » focused; the previous choice stays until a new one is made. |
| The folder is there but unreadable because All files access was turned off | Home, « No file access » and why on its line (`GAME_FOLDER_NO_ACCESS`). |
| `default.xbe` there but not the one this APK was built for (files changed since the choice, or set up through `recomp.cfg` / adb) | Home, « Wrong game » on its line, Play dimmed: it says `WRONG_GAME` instead of starting. Checked once per folder in the background, only when the APK has an expected hash. |
| The native side found no `default.xbe` (`NO_GAME_DATA`) | `shell_main.c` calls `ShellActivity.openGamePicker(dir)`; the launcher opens on Choose your game with `NO_GAME_DATA` and that folder in the note; B goes to Home. |
| Long-press on the app icon, « Change game » | Choose your game, even with a game set up; B goes to Home. If the game is running, the note says it will close when another game is chosen; saves are in internal storage and stay. |
| Long-press on the app icon, « Launcher » | Home, even with « Start in the game » on or the game running. |
| A game's own app (Create an app), its icon | Straight into the game: see [Straight into the game](#straight-into-the-game) (no Home; « Start in the game » does not apply; no « Change game » shortcut). |
| A game's home-screen shortcut, the game ready | That game becomes the selected one and starts (the rules of « Start in the game »: its files, no Prepare needed or running, a pack from this `default.xbe`, not a wrong game, no crash round to offer), whatever « Start in the game » says. Already running: back to it. Another game running: its Home, the note says that game will close, one press of Play goes on. |
| A game's home-screen shortcut, the game not ready | That game's Home with its state (Needs preparing, Not imported, Game data missing, …), B to the library; its Prepare screen when one runs or failed. Nothing starts by itself (no Prepare, no import). |
| A game's home-screen shortcut, the game no longer in the library | The library (or Home / Welcome without one), the note « The game of this shortcut is no longer in the library ». |

The two shortcuts are dynamic (the package name differs per title, which a static `shortcuts.xml` cannot follow),
published each time the launcher runs, so they appear after the first start.

### A game on the home screen

Manage › **Add to home screen** asks the home screen to pin a shortcut for the game shown (`ShortcutManager.
requestPinShortcut`; `TitleShortcut`); the home screen shows its own dialog, the note says « Shortcut requested ».
The row is hidden when `isRequestPinShortcutSupported()` is false, or when the game has no title ID.

| What | Value |
|---|---|
| Id | `game_<TID>` (one per game; asking again updates it) |
| Label | The game's title: up to 12 characters under the icon (cut at a word, « Burnout 3… »), 25 in the long label |
| Icon | Adaptive, 432 x 432 (108 dp at xxxhdpi): the [title image](#game-theme) (`files/art/<TID>.png`) centred at two thirds, so the mask barely cuts it, on the average colour of its outer pixels (the dark launcher green when they are transparent); without an image the monogram (initials on the title ID's hue, as on the library's cards) |
| Intent | Explicit to `LauncherActivity`, action `fr.adserver.xrecomp.PLAY_TITLE`, extra `fr.adserver.xrecomp.TITLE_ID` = the title ID, a fresh task (as the app icon's shortcuts) |
| Update | When the launcher loads that game's title image (Home, after a scan or a theme bake), a pinned `game_<TID>` gets the current icon and title (`updateShortcuts`); nothing when it is not pinned |

Pinned, not dynamic: the long-press menu keeps « Change game » and « Launcher ». What a tap does is in
[When it shows](#when-it-shows). logcat: `launcher: shortcut game_<TID> requested`, then at a tap `launcher: shown (game
shortcut <TID>)` or `launcher: shortcut <TID>: ready, starting it`. Tests: `LauncherShortcutTest` (the row and the
request, the three cases of a tap, the icon → `build/screenshots/shortcut-icon-*.png`, the labels).

### Device check (home-screen shortcut), for Alex

`adb logcat -s xboxrecomp` throughout; a library with Fable (prepared) and Burnout 3 (not imported), and a stock or
Pixel-like home screen (AYN Thor's launcher, then another one if installed).

1. Fable › Home › Manage: the row **Add to home screen** is there, its description on the right. Press A: the home
   screen's dialog shows Fable's logo and « Fable »; the launcher's note says « Shortcut requested… ». Accept.
2. The home screen: the icon is Fable's title image, whole (not cut by the mask), on a matching background; the
   label is « Fable ». Send a screenshot.
3. Close the app (recents › swipe away). Tap the shortcut: Fable starts straight away, no launcher screen (logcat
   `launcher: shortcut 4D5300D1: ready, starting it`).
4. Home button while Fable runs, tap the shortcut again: back into the running game, no restart.
5. Burnout 3 › Manage › Add to home screen, accept; tap its shortcut: Burnout 3's Home with **Not imported** (or its
   state), nothing starts by itself; B goes to the library.
6. With Fable running in the background, tap Burnout 3's shortcut after it was imported and prepared: its Home, the
   note says Fable is running and will close; Play once more starts Burnout 3.
7. A game without a title image (or a test XBE): its shortcut shows the monogram (two letters on a colour), legible.
8. Delete a game from the library (Storage › its imported copy, and its file from the library folder, then Y), tap
   its shortcut: the library opens with « The game of this shortcut is no longer in the library », no crash.
9. Long-press the app icon: still only « Change game » and « Launcher ».
10. In French: the row « Ajouter à l'écran d'accueil », the notes in French.

A new game, once checked, becomes a record of the [game library](#game-library) and the selected game; the launcher
goes to Home with Play focused and « TITLE is ready » in the note, instead of starting the game by itself. It
becomes the launched game (the preferences ShellActivity reads) and a running other game is ended only when it is
played.

### A game's own app

Manage › **Create an app** makes, on the phone itself, a real app for the game shown: its own package, the game's
name under the icon, the game's title image as the icon, locked to that disc. Android's installer installs it (its
own dialog). The [home-screen shortcut](#a-game-on-the-home-screen) stays; [A title's own APK](#a-titles-own-apk) is
the same idea built on a host. The row is there for a game with a title ID, not in a game's own app, and not when
XRecomp itself is installed as split APKs; its description gives the size the app will take (it is a copy of the
whole of XRecomp, toolchain included).

| What | Value |
|---|---|
| Package | `fr.adserver.xrecomp.t<titleid>` (lower case, `4d5300d1`), as `tools/android/title_apk.sh` names it |
| Label | The game's title (XBE certificate, else folder), whole up to 50 characters: a literal in the manifest |
| Icon | `mipmap/xba_title_icon`, an adaptive icon whose background layer is `drawable-nodpi/xba_title_icon_art.png`: the generator puts there `TitleShortcut.icon` (432 x 432, the title image on its border's colour, else the monogram), the same picture as the shortcut's |
| Lock | Manifest meta-data `fr.adserver.xrecomp.XBE_SHA256` = the library record's `xbe_sha256` (else the hash of `default.xbe`): `GameStore.expectedSha256` reads it before the `xba_xbe_sha256` resource, so another disc gets [WRONG_GAME](#wrong_game). Also `…TITLE_ID` (the title ID) and `…PARENT` (XRecomp's package) |
| Version | XRecomp's own `versionCode` / `versionName` (a copy) |
| Signature | APK Signature Scheme v2 only (minSdk 30: no v1; v3's key rotation is not needed), by apksig (`com.android.tools.build:apksig`, AOSP, Apache-2.0) |
| Key | EC P-256 in the AndroidKeyStore, alias `xba_title_apps`, made at the first « Create an app », never exported; the same for every game's app, so **creating it again updates the installed one**. It lives as long as XRecomp's install: after an uninstall of XRecomp (an update keeps it), an app made before must be uninstalled before it can be made again (« signed by another key ») |

**How it is made** (`TitleApk`, `ApkZip`, `BinaryXml`; no build tools on the device). The template is XRecomp's
installed APK (`ApplicationInfo.sourceDir`), copied entry by entry into `files/title-app/` without recompressing:

1. `AndroidManifest.xml` (binary XML) patched: `package`; `android:label` as a string; `android:icon` and
   `android:roundIcon` to the slot (`R.mipmap.xba_title_icon`, the same id since the template is this APK); every
   provider authority (`<package>.logs`) and package-derived name (permissions, task affinity, process) moved to the
   new package; XRecomp-only permissions left out (`INTERNET`, see
   [Vulkan drivers](#vulkan-drivers)); component class names written in full against the old package (AGP's merged manifest has them in
   full already), so they stay `fr.adserver.xrecomp.*`; the three meta-data added (replaced when present).
   Attributes stay sorted by resource id; a manifest read and written back unchanged gives the same bytes.
2. The slot's PNG replaced (its path is what `Resources.getValue` says, `res/drawable-nodpi-v4/…`, so a release build
   with shortened resource paths works too); `resources.arsc` is not touched.
3. `META-INF/*.SF`, `.RSA`, `.DSA`, `.EC`, `MANIFEST.MF` and `stamp-cert-sha256` left out.
4. Stored entries aligned (4 bytes; a stored `.so` 16 KB; this app's libraries are compressed,
   `useLegacyPackaging`), then apksig signs into a second file and realigns. Free space needed: twice the APK plus
   64 MB (checked first: « Not enough space »).

**Installing.** Without « Install unknown apps » for XRecomp (`canRequestPackageInstalls`), the row opens that
settings screen for XRecomp and the note says to come back and press it again. Otherwise the progress screen
(Creating the app, with the bytes; Signing the app; Handing the app to Android; Cancel works until the commit), then a
`PackageInstaller` session written from the signed file (deleted right after) and committed with `TitleAppReceiver`
as its status receiver: `STATUS_PENDING_USER_ACTION` opens Android's dialog; the outcome becomes a note on the
launcher (at once, or at its next resume): « … is installed », « The app was not installed (cancelled) », « An app …
signed by another key is installed », « The app was not installed: <installer message> ».

The game's app is XRecomp with one game: its own private storage, its own settings, and **its icon goes straight
into the game** ([Straight into the game](#straight-into-the-game)): at its first start it reads the game where XRecomp
does (or asks for it once) and refuses another disc. **It does not prepare the game
again** when XRecomp has (XBA-176, [prepare.md « Handing a pack to a game's own app »](prepare.md#handing-a-pack-to-a-games-own-app)):
at its start, with no pack (or one from another toolkit than its own), it asks XRecomp (`…PARENT`) for its pack
through a translucent activity of XRecomp started for a result. XRecomp answers only `fr.adserver.xrecomp.t<tid>`
for that very title, signed by the game apps' key (its own AndroidKeyStore certificate, `GET_SIGNING_CERTIFICATES`),
and grants read access to the pack's files one URI each through its non-exported `PackProvider`
(`<package>.packs`, which `TitleApk` moves to the game's app's package like `.logs`). The game's app checks the pack
fits (format, title, ABI, shell ABI, its disc), copies it into `packs/<TID>.partial/` while checking `lib_size` and
`lib_sha256`, and publishes it atomically, its progress on the app's one screen (« Copying the prepared game from
XRecomp… 42 % »), then the game starts. XRecomp missing, without a pack, refusing, or a pack that does not fit: the
Prepare, started by itself on that screen, saying why.
The copy is the app's own: it keeps playing after XRecomp is uninstalled.

logcat: `launcher: game app fr.adserver.xrecomp.t<tid> « <label> »: building from <apk> (<n> bytes)`, `…: <n>
bytes, signed`, `… handed to the installer (session <id>)`, then `launcher: game app <pkg>: installer status <n>`.
Tests: `TitleApkTest` (the binary manifest round trip and patch, the zip copy and its alignment, the v2 signature
checked by apksig's `ApkVerifier`, a regeneration keeping the certificate; the template is the build's resource APK,
`android_resource_apk`), `LauncherTitleAppTest` (the row and its size, the settings screen first, the build handed to
a session, the installer's outcomes as notes, a game's app locked by its meta-data and without the row). The signed
copy is left in `app/build/title-app/title-app.apk`, for `aapt2 dump badging` (package, label, icon
`res/mipmap-anydpi-v26/xba_title_icon.xml`) and `apksigner verify` (v2) in the build image.

The app carries the game's icon and, once prepared, code made from the player's own disc: it is for the player's own
use and is never distributed.

### Straight into the game

A game's own app behaves like the game itself: **its icon goes straight into the game** (`ShellActivity`), whatever
« Open the game directly » says. Its launcher runs with no Home, no library and no buttons of its own; what has to
happen before the first play happens by itself on one game-branded screen (`LauncherActivity.Screen.DIRECT`): the
title image (before the game's files are known, the app's own icon art, `xba_title_icon_art`) and the title (else the
app's name), the game theme's colours and backdrop, **one status line**, a line under it when there is more to say,
and a button only when the player has to act. B leaves the app. The launcher's Home stays reachable through the
long-press shortcut **Launcher** (Settings, Controls, Manage, as before); « Change game » is not published (the app is
locked to its disc). The normal XRecomp app is unchanged.

| Case (a game's own app, its icon) | What happens |
|---|---|
| The game runs in the background | Back to it at once (as before). |
| Its files and a playable pack are here (every start after the first) | `ShellActivity` starts at once: nothing shows. |
| First start (no pack, no files) | DIRECT, « Getting the game ready… »: XRecomp is asked (pack handover). Its answer also says **where XRecomp reads the game from** (below). The pack is copied (« Copying the prepared game from XRecomp… 42 % »), the files are used in place, then the game starts by itself. |
| XRecomp reads the game from a folder, or a disc image by path | With All files access: that path is used in place, `default.xbe` checked against the app's lock (`XBE_SHA256`), as a folder or image picked by hand would be. Without it: « Allow file access to play from your game files » and the path, one button **Allow all files access** (the system page); back from it: on with the game, or, refused, the game choice (copied into the app). Asked once per start. |
| XRecomp only has its own imported copy (its private storage), or reaches the image only through a document grant | « Where is the game? », « XRecomp keeps its own copy of this game, which this app cannot read… », one button **Choose a game**: the usual game choice (folder or image), once. Once chosen, on into the game. |
| Not the disc the app is locked to | « Wrong game », the `WRONG_GAME` text, **Choose a game**. |
| The folder or image XRecomp named is gone | « Game data missing », where it was, **Choose a game**. |
| XRecomp missing, without a pack, refusing, or the copy failing | The Prepare starts by itself on the same screen: its progress line (« Compile · 42 % · about 12 min left »), why it prepares and that the app may be left (the Prepare runs in its service; the next tap shows its progress); when it is done the game starts by itself. |
| The Prepare failed | « Prepare failed · CODE », its message, **Prepare again** and **Share log**. Stopped from the notification: **Prepare**. |
| The shell found no game data / refused the pack | « Game data missing » + **Choose the game again** / « The prepared game could not be loaded » (the pack is dropped) + **Prepare again**. |
| The game ended by itself (dashboard, shutdown) | « The game ended », the kernel's reason, **Play**. |
| In the game: in-game menu › **Back to the home screen** (« Back to launcher » in XRecomp) | The task goes behind Android's home screen, the game held as in the background; the icon brings it back. The companion panel's Launcher does the same. |
| In the game: **Quit game** (or the panel's held Quit) | The app's task is removed (`finishAndRemoveTask`), then `recomp_quit(0)`: Android's home screen, never the launcher. |
| Long-press › **Launcher** | Home, as in XRecomp (Settings, Controls, Manage). |

**Where the game is.** XRecomp's `PackHandoverActivity` adds `HANDOVER_GAME_KIND` (`FOLDER`, `IMAGE`, `NOT_SHARED`) and
`HANDOVER_GAME_PATH` to its answer, with the pack or with its `NO_PACK` refusal, **only after the caller check**
([prepare.md « Handing a pack to a game's own app »](prepare.md#handing-a-pack-to-a-games-own-app)): from its library
record (in place: the folder; image read in place by path: the image's path; a library game not imported: its file in
the library folder; imported, or a path in XRecomp's own data or `Android/data` directories: `NOT_SHARED`, no path).
The game's app keeps the last answer in its `handover` preferences and takes only an absolute path without `.` or
`..`; it never trusts the path beyond reading it: the same checks as a hand-picked folder or image (`default.xbe`, its
SHA-256 against the lock). It asks XRecomp when it has no pack (as before) and also, on the way into the game, when its
files are not usable yet.

logcat (`xboxrecomp`): `launcher: game app: ready, starting the game` / `…: the game is running, bringing it back` /
`…: shown (getting the game ready)`, `pack handover: <parent> reads the game from FOLDER /storage/…`, `game app:
reading the game where XRecomp does: …`, `game app: asking for All files access, to read …`, `game app: <error line>`,
`game app: prepare over (…)`; in XRecomp `pack <TID> handed over to <pkg>, the game read from FOLDER /storage/…`; in the
game `shell: game app: to the home screen (the game waits)`. Code: `LauncherActivity` (`startDirect`, `directStep`,
`buildDirect`), `PackHandover.GameSource`, `GameMenu` (the row's label), `ShellActivity` (`openLauncher`, `quit`).
Tests: `LauncherGameAppTest` (no screen when ready, the folder read in place, All files access asked then granted or
refused, XRecomp's private copy, another disc, a folder gone, the game ending, the « Launcher » shortcut, XRecomp
unchanged), `LauncherPackHandoverTest` (copy then the game; no pack, a refusal, a damaged copy: the Prepare then the
game; a failed Prepare), `PackHandoverTest.theCheckedCallerIsToldWhereTheGameIs`, `GameMenuTest.aGameOwnAppLeavesToTheHomeScreen`;
screenshots `build/screenshots/game-app-*.png`, `handover-*.png`.

### Device check (straight into the game), for Alex

`adb logcat -s xboxrecomp` throughout. XRecomp (a build with this change) with Fable in the library **read in place
from a folder**, prepared and played once. Uninstall any earlier Fable app first
(`adb uninstall fr.adserver.xrecomp.t4d5300d1`), then Fable › Manage › **Create an app** and install it.

1. Tap the new Fable icon (fresh install). Expected: one screen with Fable's logo and title, « Getting the game
   ready… », then « Copying the prepared game from XRecomp… n % » (a few seconds), possibly one screen « Allow file
   access to play from your game files » with Fable's folder path: press **Allow all files access**, turn it on, Back.
   Then **Fable starts by itself** — no Welcome, no Home, no game choice, no Play button. logcat: `reads the game from
   FOLDER /storage/…`, `pack handover 4D5300D1: COPIED`, `game app: ready, starting the game`. Note how long it took.
2. Home button, then tap the Fable icon: back into the running game (no restart).
3. In the game: Back › **Quit game** twice: Android's home screen (not a launcher screen, not XRecomp). Tap the icon:
   the game starts at once, no screen at all (`game app: ready, starting the game`).
4. In the game: Back › the row now reads **Back to the home screen**: Android's home screen, the game waits; the icon
   brings it back where it was.
5. Long-press the Fable icon: only **Launcher** (no « Change game »). Launcher: Fable's Home with Settings, Controls,
   Manage. Change a setting, Play: the game starts.
6. **ISO source**: in XRecomp, make Fable read from its disc image (Manage › Change game › the .iso, read in place
   with All files access). Uninstall the Fable app, Create an app again, install, tap: as step 1 (the image's path in
   the access line if asked); logcat `reads the game from IMAGE /storage/…iso`; the game starts.
7. **Imported copy**: in XRecomp, import Fable as a copy (Change game › folder › « Copy into the app », or an image
   with `RECOMP_DISC_IMAGE=0`). Uninstall/Create/install the Fable app, tap: « Where is the game? » with « XRecomp
   keeps its own copy of this game, which this app cannot read… » and **Choose a game**. Choose Fable's folder or
   image once: the game starts by itself. Quit, tap again: the game at once.
8. **Wrong disc**: uninstall the Fable app's data (`adb shell pm clear fr.adserver.xrecomp.t4d5300d1`), tap the
   icon, refuse All files access if asked, then on « Where is the game? » choose **another game's** folder: the usual
   choice screen says `WRONG_GAME`. (With XRecomp pointing at another region's Fable: the Fable app shows « Wrong game »
   and **Choose a game**.) No crash, no blank screen.
9. **Refused access**: `pm clear` the Fable app, tap, on the access screen press Allow but leave it off, Back: « Where
   is the game? » saying a copy will be made, **Choose a game** works.
10. **No pack in XRecomp**: delete Fable's pack in XRecomp (Manage) and `pm clear` the Fable app, tap: after the files
    step, the Prepare's progress line on the same screen with « XRecomp has not prepared this game… »; leave the app
    and come back: the progress again; when done, the game starts by itself.
11. In French: the lines and buttons above in French (« Préparation du lancement… », « Où est le jeu ? », « Retour à
    l'écran d'accueil »).
12. XRecomp itself: its icon still opens Home (or the game with « Open the game directly »), its long press still has
    « Change game » and « Launcher », its in-game menu still says « Back to launcher » and opens the launcher.

### Device check (game's own app), for Alex

`adb logcat -s xboxrecomp` throughout; XRecomp (xrecomp-latest) with Fable in the library, prepared and played once.

0. A game not prepared yet › Manage: the row **Create an app** says « Prepare this game first »; pressing it builds nothing, the note says to prepare first.
1. Fable › Home › Manage: the row **Create an app**, its description with the size (about the APK's size). Press A.
   If « Install unknown apps » is off for XRecomp, Android's screen for it opens and the note says to come back:
   allow it, Back, press **Create an app** again.
2. The progress screen: « Creating the app » with the bytes going up, then « Signing the app » (some seconds), then
   « Handing the app to Android ». Note how long the whole thing takes. Android's install dialog then shows
   « Fable » with Fable's logo: Install.
3. Back on Manage, the note says « Fable is installed… ». In the app drawer: an app **Fable** with Fable's title
   image as its icon, whole (not cut by the mask), next to XRecomp's own icon, which has not changed. Screenshot.
4. Open Fable (the new app): it goes into the game ([Straight into the game](#straight-into-the-game) and its
   device check). Long-press › Launcher › Settings › About shows the game check with Fable's hash and the app id
   `fr.adserver.xrecomp.t4d5300d1`.

### Device check (pack handover, XBA-176), for Alex

`adb logcat -s xboxrecomp` throughout; XRecomp (xrecomp-latest) with Fable prepared and played once; the Fable app not
installed yet (uninstall it if it is: `adb uninstall fr.adserver.xrecomp.t4d5300d1`). Since « Straight into the game »
the icon shows the copy on the game's one screen and starts the game itself (no Home, no Play): steps 1–3 now read that
way; Home's copy line is what the long-press « Launcher » shows.

1. Fable › Manage › **Create an app**, install it (as above). Open the new Fable app and choose Fable's folder or
   image. Home shows « Copying the prepared game from XRecomp… » with the percentage going up and a dimmed « Copying… »
   button, not « Needs preparing ». XRecomp's own screen does not show (at most a blink). Note how long the copy takes
   and the pack's size (Manage › Pack size).
2. Then the note « The prepared game was copied from XRecomp: press Play » and **Play**: Fable starts, no Prepare.
   logcat: `pack handover: asking fr.adserver.xrecomp.shell for 4D5300D1`, `pack 4D5300D1 handed over to
   fr.adserver.xrecomp.t4d5300d1` (XRecomp's process), `pack 4D5300D1 published from the handover`,
   `pack handover 4D5300D1: COPIED`.
3. Back to the app's Home, leave it (swipe it away), open it again: no copy this time, Play at once; logcat has no new
   `asking` line.
4. Uninstall XRecomp (`adb uninstall fr.adserver.xrecomp.shell`; this loses its key: keep its saves first if
   needed). Open the Fable app: Play, the game starts (the copy is its own).
5. Reinstall XRecomp, prepare Fable there, then install a newer XRecomp build whose toolkit changed (its About shows a
   newer build; or ask the agent for one), prepare Fable in it, and **Create an app** again (after an uninstall of
   XRecomp the old Fable app must be uninstalled first: « signed by another key »). Open the Fable app: it copies
   again (the stale pack is replaced), then Play; Home has no « Prepared by an older version of the app » line.
6. A non-generated app cannot get the pack:
   `adb shell am start -W -n fr.adserver.xrecomp.shell/fr.adserver.xrecomp.PackHandoverActivity --es
   fr.adserver.xrecomp.extra.HANDOVER_TITLE_ID 4D5300D1` → nothing shows, logcat `pack handover refused to null
   (4D5300D1): NO_CALLER`. And `adb shell content read --uri
   content://fr.adserver.xrecomp.shell.packs/4D5300D1/pack.json` → refused (Permission Denial: not exported).
7. In XRecomp, delete Fable's pack (Manage › the pack's row), then in the Fable app delete its pack too and open it
   again: the note « XRecomp has not prepared this game: prepare it here… » and Prepare, as before XBA-176.
6. In XRecomp, Manage › Create an app once more for Fable: Android's dialog says it is an **update**; after it, the
   new app keeps its data (still prepared, the folder still chosen).
7. Uninstall the game's app (long-press › Uninstall): XRecomp is untouched (its library, Fable's pack, saves).
8. A game without a title image: Create an app gives an app with the monogram as its icon, legible.
9. In French: the row « Créer une app », the progress labels and the notes in French.
10. `adb shell pm dump fr.adserver.xrecomp.t4d5300d1 | grep -E "versionCode|signatures"`: the same versionCode as
    XRecomp's; send the line.

### Controls

| Input | Does |
|---|---|
| D-pad, left stick | Move the focus (Android turns the stick and the hat into D-pad keys). Each screen (and each Settings tab) remembers its focused item. Home's buttons are one row: left / right. |
| A (`BUTTON_A`), D-pad centre, Enter | Select the focused item; on a setting, the next value. |
| B (`BUTTON_B`), Back, Escape | Back one screen; on Home or Welcome, leave the app (on a library game's Home: the library); during an import or a Prepare, Cancel (a Prepare: twice); with the Options sheet open, close it. |
| Left / right on a setting | Previous / next value. |
| L1 / R1 | Settings: previous / next tab (wrapping). Library: previous / next console of the filter (All, Xbox, GBA, PS1, N64; with games of several consoles). |
| X | Library: the focused game in or out of the favourites. A game's Home: Add to / Remove from favourites. Drivers: delete. About: Licences. |
| Y | Library: the **Options** sheet (Rescan, Storage, Library folders, Settings, About; B or Y closes it, the focus comes back). Settings: the defaults. Preparing: hide it (the Prepare carries on). A failed Prepare: Share log. |
| Start | Home: Play (not shown as a hint). Library: the focused game's primary action (not shown as a hint either since XBA-206, except in an empty library). |
| Touch | Tap an item to select it, the arrows of a setting to step it, a button hint to do what it says. After a touch, the first D-pad or A press brings the focus back. |

A and B are taken on release, once, so a held button does not repeat. Dimmed rows and buttons (Play without a usable
game) stay focusable so that pressing them says why. Each screen shows **three hints at most**, on the right of the
footer line (`LauncherActivity.MAX_HINTS`: a fourth is a bug and throws). « Start in the game » is in Settings ›
General only.

### Look, motion and sound

« Calm » (the 2026-11 redesign): fewer things on screen, one way to show the focus, secondary text quiet.

- **Type**: five sizes, two weights (`Theme`): Display 64 (Home's title), Title 40 (a screen's title), Row 34 (a
  row's label, a button), Body 28 (values, lines, descriptions), Small 24 (group labels); nothing smaller (hints 26),
  no Light weight, no spaced capitals. Spacing on an 8-pixel scale (8, 16, 24, 32, 48, 96); 96 px side margins.
- **Colour**: the tokens of `GamePalette` (and the [game theme](#game-theme)) unchanged. All secondary text is
  `TEXT_DIM` (8.5:1 on the backdrop); `TEXT_FAINT` only says « disabled ». The backdrop is the gradient with one soft
  glow (7 %), or, on Home and Preparing only, the game's hero instead; no dot grid, bezel, corner ticks, rules or
  panel outlines. A flat `PANEL` surface only for overlays (the Options sheet, the in-game drawer).
- **Chrome**: a top bar with the screen's title only (Settings adds its tabs, Storage the free space on the right;
  Home and Preparing have none), and one 72 px footer line: the note on the left, at most three hints on the right
  (outlined glyphs, 26 px labels).
- **Focus**: one highlight for the screen (`Widgets.Glide`): a flat `ACCENT` plate at 14 % with a 2.5 px `ACCENT`
  outline (radius 16) around a row or a quiet button, a 4 px ring around a game card's art; the solid Play is its own
  highlight. It glides from the item it leaves to the item it reaches in 150 ms; the focused item grows to 102 %.
  Contrast (stock and every hue, `CalmContrastTest`): `TEXT_DIM` on the plate 4.5:1 or more, `TEXT_BRIGHT` 7:1, the
  outline 3:1 on the backdrop, the ink on Play 4.5:1.
- **Rows**: label left, value right (`ACCENT` when focused), the « < > » arrows of a setting only on the focused row;
  no subtitles: the focused row's description shows as plain text in a quiet column on the right.
- **Screens** fade in and slide in from the side they come from (forward from the right, back from the left), 150 ms;
  a Settings tab slides in from its side.
- **Settings**: a value pulses when it changes (160 ms). A setting row keeps the width of its widest choice for the
  value, and a label too long for what is left ends in « … » (measured with the row's own paint).
- **Sounds and haptics** (`UiFeedback`): five short sounds, made with `soundcraft-cli` from our own synth recipes
  (`tools/android/ui_sounds.sh`, WAVs in `res/raw`, 16-bit 22.05 kHz, about 115 kB in all): **move** (focus or a
  value), **select**, **back**, **error** (an error note) and **done** (a game set up, a Prepare finished), played
  through SoundPool at a low volume, each with a haptic (`performHapticFeedback`: clock tick, keyboard tap, reject,
  confirm; Android's own « touch feedback » setting applies). Settings › General › **Sounds** and **Vibration** turn
  them off (`game` SharedPreferences, `ui_sound`, `ui_haptics`).
- **Preparing screen**: Home's layout with the buttons replaced by the progress, so nothing jumps: « Preparing · step
  5 of 6 · Compile », one 12 px bar, the percentage and the time left, and that the app can be left. When the Prepare
  ends: Home with Play focused, the « done » sound and haptic. After two minutes without input it dims (OLED
  burn-in): a black layer with the percentage and the step in grey, moved a little each minute, and the window's
  brightness lowered; any key or touch wakes it and does nothing else.

### Device check (Calm launcher), for Alex

`adb logcat -s xboxrecomp` throughout; Fable and Burnout 3 in the library, the Thor.

1. Home (Fable): no top bar; the art, the title, one line « <when> · <time> played », a solid Play and three quiet
   pills (Settings, Controls, Manage) in one row; hints A Select, B Library only. Left / right moves along the row
   (the plate glides, Play stays solid). Start still plays. Screenshot.
2. A game that needs preparing (Burnout 3): an amber dot and « Needs preparing · once, about … », the button reads
   Prepare. Start a Prepare: the same layout, the bar moving, « Preparing · step n of 6 · <step> », the time left;
   no row to focus; Y hides it, B twice cancels. Screenshot of each.
3. Settings: the tabs in the top bar, L1 / R1 switch them (and wrap), each tab keeps its focus, no tab scrolls;
   only the focused row shows its arrows; its description on the right, plain. Graphics: Frame cap and Resolution
   dimmed until Renderer is GPU. Advanced ends with Reset all and About. A long French label is cut with « … » and
   never runs under its value. Screenshot of each tab, in English and French.
4. Library: five columns, « Ready » not written under a ready game, the others with a dot and their state
   (« Not imported » grey); the focused game's line at the bottom left. Y opens Options from the right: Rescan,
   Storage (free space), Library folders (its name), Settings, About; D-pad stays in it; B closes it and the focus is
   back on the card. X does nothing any more. Screenshot with and without the sheet.
5. Storage (from Options): « … free » in the top bar, the games' names as small labels, sizes as values, A twice
   deletes (« Press A again » replaces the size); paths in the description. Manage (Home): the title ID, prepared
   date, pack size and folder in the first row's description.
6. In-game menu: a flat sheet flush with the left edge (about 30 % of the screen), the title and « Paused · 0:42
   played », six rows (Quit apart), B Resume; Settings holds Perf overlay, Volume, Bottom screen, Language, Widescreen,
   480p and Share log. Screenshot.
7. Bottom screen (Thor): pure black, the art, the title, one pill (Play, Prepare, Import…) and the battery; on the
   library it follows the focused card, on Settings or Storage only the art shows. While preparing: the big
   percentage, the bar, « <step> · about … left », Hold to cancel. Nothing in the bottom centre.
8. Contrast and comfort: the dim text readable over the hero (Fable) at arm's length; the focused row's value and
   description readable on the plate; say if anything looks too faint or too small at 1080p on the Thor.
9. Send the screenshots (`adb exec-out screencap -p > x.png`) of Home, Preparing, the four Settings tabs, the
   library with and without Options, Storage, the in-game menu and both bottom-screen states.

### Game theme

The launcher takes its colours from the game: Home shows the game's own title image, and the accent (the focus,
values, the Play button, the tabs' underline), its glow and the neutrals' hue come from it, with a faint blurred
copy of the image washing in from the right of the backdrop. The text colours and the **status colours never change**:
`Theme.OK` (ready, done, connected: the stock green), `WARN` (amber), `ERROR` (red). Nothing of a game is in the APK or
the repo: everything is read from the player's own copy and kept in app-private storage.

**Where the image comes from** (`GameArt`, first found): default.xbe's `$$XTIMAGE` section (an XPR0 resource file
holding one texture, usually 128 x 128 DXT1), its `$$XSIMAGE` (the save image), a loose `TitleImage.xbx` then
`SaveImage.xbx` in the game folder (any case; its root first, then a walk at most 3 folders deep, 2000 entries, files
up to 4 MB), then `<internal files>/save/UDATA/<title id>/TitleImage.xbx`, the copy the game writes next to its saves.
Only default.xbe is read (dashupdate.xbe and update.xbe hold dashboard media). XPR0 and the textures are decoded in
Java (`TitleArt`: DXT1 / DXT3 / DXT5, and swizzled or linear A8R8G8B8 / X8R8G8B8 / R5G6B5); formats from public
documentation (xboxdevwiki « Xbe » and « XPR ») and `src/d3d` (format codes, swizzle).

**The colours** (`GamePalette`, pure Java, a 1:1 port of `tools/android/theme_preview/palette.py`): the image is
box-downsampled to 64 x 64 and its pixels taken to OKLCH; greys, near-black and near-white carry no hue. Two 36-bin
hue histograms (« vivid »: chroma squared, dark pixels count less; « area »: pixel count) give the peaks. The accent
takes the most vivid hue, the neutrals the hue covering the most area, the second glow the next peak 40° or more away.
Every token keeps the stock token's OKLCH lightness: the neutrals take a little chroma (at most 0.035) in the game's
hue, the accent keeps 7:1 on `PANEL_HI` (its lightness raised when needed), and an accent within 18° of `ERROR`'s hue
is pushed out of it. `GamePalette.WARN_AVOID_DEG` would do the same around `WARN`'s amber for warm games: 0 (off) for
now, set it to 15 if an orange accent reads like the amber « Needs preparing » chip on the device. A 360-hue sweep
keeps every contrast pair the UI relies on (TEXT on PANEL_HI 7:1, TEXT_DIM 4.5:1, TEXT_FAINT on BG_TOP 3:1, the
accent on its surfaces 7:1, ...). Fable: accent `#A7C2FF` (periwinkle), blue-black neutrals, a rose second glow.

**The hero backdrop** (`GamePalette.bakeHero`): 48 px on its long side, Gaussian-blurred (σ 3 px), then a gain in
linear light so the 95th-percentile luminance is 0.012 and nothing above it: small text keeps its contrast over the
brightest pixel (a render test checks TEXT_FAINT 3:1 and TEXT_DIM 4.5:1). `Widgets.Backdrop` draws it cover-fit,
on Home and Preparing only, gone left of 30 % of the width and in the bottom 40 % (the buttons and hints stay on the
plain gradient), at most 85 % opaque; no glow over it; one bitmap and cached shaders, nothing per frame.

**Cache** (`GameTheme`), per title ID in `<internal files>/art/`: `<TID>.png` (the title image), `<TID>.hero.png`,
`<TID>.theme` (text: `v=<GamePalette.VERSION>`, `xbe=<length>:<mtime>`, `source=`, `hues=`, `hero=`, one
`TOKEN=AARRGGBB` per colour). Baked on the import worker before Home shows, or lazily on a worker for a game imported
before (then the screen is shown again); stale when the version or default.xbe changes. « Delete the imported copy »
deletes it. Each bake logs `launcher: theme <TID>: source xbe:$$XTIMAGE, accent #A7C2FF, hues 265.9/265.9/-, mode
full`, and the launcher logs the same line when it applies a theme (`launcher: theme <TID>: stock, mode off` without).

**Where it applies**: the launcher (applied before any view is built, so no flash of the stock green; when the game or
the setting changes, the stage is built again in the new colours), the bottom screen's panel (it also
shows the title image; the panel's background stays black), and in the game process the in-game menu and the
companion panel (`ShellActivity` reads the cached tokens once at game start: no bitmaps). Without a usable image
(none, or an unknown format) Home shows a monogram (the title's initials on a gradient whose hue comes from the title
ID) and the colours take that hue; a grey image keeps its picture and hero with the title ID's hue. No title ID, no
game: the stock colours.

**Settings › General › Game theme** (only with a game that has a title ID): Art and colours (default), Colours only
(no hero), Off (the stock green), per title in `files/settings/theme_<TID>`; applies at once in the launcher, at the
next game start in the in-game menu.

**Previews on a Mac** (`tools/android/theme_preview/`, Python 3 and Pillow; no art in the repo): `xart.py <xbe> <out>`
extracts the title and save images from your own XBE (with `tools/title_art`'s decoder), `palette.py <png>` prints the palette and its contrast checks, `mock.py`
draws mock-ups, `golden.py` makes `GamePaletteTest`'s golden vectors. With a game folder of your own,
`gradle testDebugUnitTest -PxbaPreviewGameDir=/path/to/game --tests '*ThemePreviewTest'` renders the real launcher in
its theme to `build/screenshots/preview-*.png` (git-ignored); without the property the test is skipped.

### Device check (game theme), for Alex

`adb logcat -s xboxrecomp` throughout; Burnout 3 already imported, Fable's folder at hand.

1. Open the launcher with Burnout 3: one line `launcher: theme <TID>: source …, accent #…, hues …, mode full` (send
   it, and whether the source is `xbe:$$XTIMAGE`). The first start after the update bakes it on a worker: Home shows
   again in the game's colours within a second.
2. Home: the accent (focus, Play) in the game's colour, a faint wash of the title image on the right of the backdrop,
   no banding in it; a not-ready state's dot keeps its status colour (amber, red).
3. The dim line under the title stays easy to read over the wash.
4. Settings › General › Game theme: Colours only (the wash goes, the colours stay), Off (the stock green at once),
   Art and colours again; the description names the game.
5. Bottom screen (Thor): black background, the title image, Play in the game's colour; it dims after 10 s.
6. Play: the in-game menu in the game's colours, the screenshot flash green.
7. Change game to Fable (import or folder): periwinkle (#A7C2FF) without restarting the app.
8. Force-stop the app and open it again: no flash of the stock green before the game's colours.
9. Manage › Delete the imported copy: no `files/art/<TID>.*` left (`adb shell run-as <app id> ls files/art`),
   unless the game is still in the library folder (the library keeps showing its art).
10. Screenshots of Home with both games (and of a warm-coloured game next to the amber « Needs preparing » chip, if
    one is at hand: the accent is not kept away from the amber yet, `GamePalette.WARN_AVOID_DEG`).

### Play time

« Last played » and « Play time » (Home, the library's context line, the « Recently played » row and the Last played /
Most played sorts): a session starts when the launcher starts `ShellActivity` (not when it only brings a running game
back), for an Xbox game and, since XBA-206, for a GBA, PS1 or N64 game too (its library record holds its times; it is
never the selected game: `Library.Games.selected` skips the other consoles). It ends when the launcher shows again
with the game no longer running, or when another game is started while it waits in the background.

Its length is the time the game was **on screen** (XBA-206): `ShellActivity` counts from `onStart` to `onStop` (and
Quit) in `files/play_clock.properties` (`PlayClock`: the session it belongs to, the closed on-screen time, when the
current stretch began, the last write), written at each change and once a minute while the game shows, so a game left
in the background does not count and a crash loses a minute at most. A game process the system restarts within the
same session goes on with its count. A session the game never counted (an older game process) falls back to the wall
clock since launch; any session counts for at most 6 hours. The totals: the record's `last_played` / `play_ms` in
`games.json`, and the one-game `game` SharedPreferences (`last_played`, `play_ms`, `session_start`, `session_tid`).
logcat: `launcher: play session of <TID>: N s on screen`.

`LauncherScreensTest` renders every screen and Home state in English and French at 1280 x 720 and 1920 x 1080, each
with its focused row, to `build/screenshots/ui-<state>-<en|fr>-<720|1080>.png` (`--target app-unit-tests-out`);
`TitleArtTest` checks the XBE section, the XPR0 container and the texel decoders on blobs it builds; `GamePaletteTest`
and `GameArtTest` (plain JUnit) the palette against `palette.py`'s golden vectors, the contrast sweep, the hero cap and
the image sources; `GameThemeTest` the cache; `LauncherScreensTest` also renders Home in a blue, a warm and a grey
game's theme (`ui-home-themed-*`) and the Game theme setting (`ui-settings-theme-*`).

### Settings

The Settings screen has four tabs — **General**, **Graphics**, **Audio**, **Advanced** — switched with L1 / R1; it
writes a few `RECOMP_*` options to `recomp.cfg` (the file the runtime already reads,
[RECOMP_* options](#recomp_-options-recompcfg)), and they apply the next time the game starts. Only options that
exist in the runtime and mean something to a player are offered. Graphics and Audio group their rows (XBA-181):
**All consoles** first (what the GBA, PS1 and N64 runtimes read as well as the Xbox's), then **Xbox only**; see
[Settings per console](#settings-per-console).

| Row | Choices (default first or marked) | Line in `recomp.cfg` |
|---|---|---|
| General › Start in the game | Off (default), On | none: a launcher preference (`game` SharedPreferences, `auto_start`) |
| General › Sounds | On (default), Off | none: `ui_sound` |
| General › Vibration | On (default), Off | none: `ui_haptics` |
| General › Bottom screen (only with a second display) | Panel (default), Black, Off | none: [Dual screen](#dual-screen-ayn-thor), `GameStore.bottomScreen` |
| General › Game theme (only with a game that has a title ID) | Art and colours (default), Colours only, Off | none: [Game theme](#game-theme), `files/settings/theme_<TID>` |
| General › Enhancements (only for a game whose pack has `title_layer`: the toolkit has optional enhancements for it) | On (default), Off | none: `files/settings/enhancements_<TID>`; off, the game's next Prepare runs with `XBA_NO_TITLE_LAYER=1`, and Home offers Prepare again while the pack differs ([profiles.md](../technical/profiles.md#optional-by-construction), [prepare.md](prepare.md#packjson) `title_layer`) |
| General › Language | Same as Android (default), English, Français, Deutsch, Español, Italiano, Português, 日本語, 한국어, 中文 | `RECOMP_LANGUAGE=…`; « Same as Android » writes nothing ([Game language](#game-language)) |
| General › Controller rumble (all consoles) | On (default), Off | `RECOMP_RUMBLE=0` when off: no motor for the Xbox's emulated USB pad, the PS1 DualShock or the N64 Rumble Pak |
| General › Rewind (all consoles; GBA, PS1, N64 only) | On (not N64) (default), On, Off | XBA-194: `RECOMP_REWIND=1` / `0`; the default writes nothing (on for the GBA and the PS1, off for the N64, whose captures cost more: [consoles/README.md « Rewind »](../../consoles/README.md)). Hold the rewind hotkey (Select + L2) to go back; a game can have its own (an N64 game can turn it on) |
| General › Rewind memory (all consoles; GBA, PS1, N64 only) | 32 MB, 64 MB (default), 128 MB, 256 MB | `RECOMP_REWIND_MB`: the history's cap, the oldest moments go first |
| Graphics › Vertical sync (all consoles) | On (default), Off | `RECOMP_PRESENT_VSYNC=0` when off |
| Graphics › Scale (all consoles; GBA, PS1, N64 only) | Keep shape (default), Whole pixels, Stretch | `RECOMP_SCALE=integer` / `stretch`; Keep shape writes nothing (the runtimes' `aspect`). A game can have its own ([Settings per console](#settings-per-console)) |
| Graphics › Picture filter (all consoles; GBA, PS1, N64 only) | Console default (default: GBA sharp, PS1 and N64 smooth), Sharp, Smooth | `RECOMP_FILTER=nearest` / `linear`; Console default writes nothing. A game can have its own |
| Graphics › Internal resolution (all consoles; PS1 and N64 only) | Native (default), 2x, 4x, 8x (N64 Vulkan) | `RECOMP_UPSCALE=2` / `4` / `8`; Native writes nothing (the runtimes' 1x). XBA-184 / XBA-185: the console draws N times its resolution; a PS1 game stops at 4x, 8x is the N64 Vulkan renderer's. A PS1 or N64 game can have its own |
| Graphics › Picture shader (all consoles) | None (default), Sharp bilinear, Scanlines, CRT, LCD grid | `RECOMP_SHADER=sharp` / `scanlines` / `crt` / `lcd`; None writes nothing. XBA-195: drawn by the console window at the screen's resolution, after the internal resolution and frame generation ([HOST_LAYER.md](../../src/platform/HOST_LAYER.md#picture-shaders-xba-195)); XBA-203: an Xbox game too (its presenter, after its resolution and frame generation; LCD grid draws none there). A game can have its own |
| Graphics › Shader strength (all consoles) | Shader default (default: scanlines and LCD 50 %, CRT 60 %), 25 %, 50 %, 75 %, 100 % | `RECOMP_SHADER_STRENGTH=25` / `50` / `75` / `100`. A game can have its own |
| Graphics › CRT curvature (all consoles) | Off (default), Slight, Strong | `RECOMP_SHADER_CURVATURE=30` / `70`; only the CRT shader reads it. A game can have its own |
| Graphics › LCD ghosting (all consoles; GBA, PS1, N64 only) | Off (default), Slight, Strong | `RECOMP_SHADER_GHOSTING=30` / `60`; only the LCD grid shader reads it. A game can have its own |
| Graphics › Renderer | Auto (default: GPU, unless the title's pack forces `RECOMP_GPU=0`), Software, GPU | `RECOMP_GPU=0` / `1`; Auto writes nothing and the runtime's Android default (GPU, CPU rasteriser when GLES 3.0 cannot start) applies; see [GPU back end](../technical/gpu-backend.md#android) |
| Graphics › Frame cap (dimmed when the renderer is Software) | Auto (default: `RECOMP_PACING=auto`, the title's own rate or a steady lower one the device sustains, never dividing a title locked to its rate; WP4), 30, 60, Off | Auto writes nothing; `RECOMP_GPU_FPS_CAP=30` / `60` / `0` (a fixed cap, auto off) |
| Graphics › Resolution (dimmed when the renderer is Software) | Auto (default: the title profile's choice, 1× for now), 1× (original), 2×, 3×, Fit the screen | `RECOMP_GPU_SCALE=1` / `2` / `3` / `auto` (Fit: the screen's height / 480, at most 3); see [GPU back end](../technical/gpu-backend.md#internal-resolution) |
| Graphics › Aspect ratio | Auto (default), 4:3 (original), 16:9 | `RECOMP_ASPECT=4:3` / `16:9`. Auto: 16:9 for the frames a title draws wide (a title patch declares them; Fable on Android) with menus and videos kept in their shape, 4:3 otherwise; 16:9 stretches every frame of a title that draws no wide frames; see [GPU back end](../technical/gpu-backend.md#aspect-modes) |
| Graphics › Widescreen | Off (default), On | `RECOMP_WIDESCREEN=1`: the console's widescreen setting (EEPROM `XC_VIDEO` and the AV capabilities, XBA-121); a title that supports it draws 16:9 frames, shown full width with Aspect ratio 16:9 |
| Graphics › 480p | Off (default), On | `RECOMP_480P=1`: the console's 480p setting (same places); a title that offers progressive output picks it |
| Graphics › Frame generation (all consoles; only in a [frame generation](#frame-generation) APK) | Off (default), 2×, 2× performance | `RECOMP_LSFG=1`, `RECOMP_LSFG_MULT=2`, plus `RECOMP_LSFG_PERF=1` for 2× performance. XBA-186: the GBA, PS1 and N64 read it too; a console game can have its own on / off |
| Graphics › Choose Lossless.dll (all consoles; same) | a document picker; the row shows the shaders' state | `RECOMP_LSFG_DIR=<files>/lsfg/<key>` once the shaders are there (not removed by « Reset all ») |
| Graphics › Texture filtering | Sharp (default), Smooth, Anisotropic 8×, Anisotropic 16× | `RECOMP_TEX_BILINEAR=1`, `RECOMP_TEX_ANISO=8` / `16` |
| Graphics › Vulkan driver (under « All consoles »; N64 Vulkan renderer only, for now) | the default driver: **System** (default) or its name and version; a game can have its own in its Options; A opens the [Drivers screen](#vulkan-drivers). In a game's own app: XRecomp's driver, nothing to choose | not in `recomp.cfg`: `files/settings/vk_driver` (a game's own: `vk_driver_<KEY>`), turned into `RECOMP_VK_DRIVER_*` by `ShellActivity` for an N64 game |
| Audio › 3D audio (HRTF) (Xbox only) | On (default), Off | `RECOMP_APU_HRTF=0` when off |
| Audio › Audio buffer (all consoles) | Short (256), Normal (default, 512), Long (1024), Very long (2048) | `RECOMP_AUDIO_BUF_SAMPLES=…` |
| Advanced › Drawing threads | Auto (default), 1, 2, 4, 6, 8 | `RECOMP_RASTER_THREADS=…` |
| Advanced › Fast cores | On (default on Android), Off | `RECOMP_BIG_CORES=0` when off |
| Advanced › Self-test | Off (default), On | `RECOMP_SHELL_SELFTEST=1` |

#### Game language

The console language the runtime reports to the title (`RECOMP_LANGUAGE`, XBA-77, `GameLanguage.java`). First found
wins: the title's own choice (in-game menu › Settings › **Language**, kept per title ID in
`files/settings/language_<TITLEID>`), then the `RECOMP_LANGUAGE` line of `recomp.cfg` (Settings › General › Language),
then the Android language when the Xbox has it, then English. `ShellActivity` sets the variable before the runtime
starts for the first and the third (the environment wins over `recomp.cfg`), and clears it otherwise; logcat says
« shell: game language … ». A title only has the languages on its disc: a US disc is often English whatever is
chosen.

How the file is written (`RecompConfig.java`): it is parsed as `recomp_cfg.c` does (one `KEY=VALUE` per line, blanks
and `#` comments skipped, keys and values trimmed, the first line for a key wins). A change rewrites the key's first
line in place and removes its later duplicates; a default value removes the key's lines (the runtime default then
applies); a new key is appended under the comment « Written by the launcher's Settings screen ». Every other line
(comments, other variables, `RECOMP_GAME_DIR`) is kept as written. The file goes through `recomp.cfg.tmp` and a
rename; no file is created while every option is at its default. A value the screen does not offer (written by hand)
shows as « Custom » and stays until that row is changed. « Reset all » (the end of Advanced, or Y on any tab)
removes the lines of these options only. Frame generation forced off by the game's pack (`env_force` RECOMP_LSFG=0)
stays changeable (it is global) but is dimmed, and its description says, in amber, that it is unavailable for this
title. No shipped profile forces it since WP4: the runtime keeps frame generation off by itself for a game judged
locked to 60 ([framegen.md](../technical/framegen.md#titles-locked-to-the-flip-rate)); the row does not know that
verdict.

Asked for in XBA-41 but left out, because no runtime option backs them on Android: the Xbox's output scaling and its
filter (the SDL presenter always letterboxes the 640 x 480 frame with nearest-neighbour sampling, `fb_present_sdl.c`;
`RECOMP_WINDOW_SCALE` only sizes a desktop window and `RECOMP_PRESENT_FILTER` is the D3D11 presenter's; Scale and
Picture filter are the other consoles'), an fps cap for the software renderer (only the GPU renderer has one,
`RECOMP_GPU_FPS_CAP`), a stats overlay (no such switch; the in-game menu's Perf overlay; control remapping has its own
screen since XBA-131, [Controls and remapping](#controls-and-remapping)). Bring-up and debug switches stay out on
purpose.

#### Settings per console

XBA-181. Which settings reach which console's game (the native side reads them in `src/host` and
`consoles/*/runtime`). Since XBA-188 every row can also be a game's own ([Settings per game](#settings-per-game)); the
last column says where it could be set per game before, its files now moved into the game's file:

| Setting | Xbox | GBA, PS1, N64 | Per game |
|---|---|---|---|
| General › Language | yes | no | Xbox: in-game menu (`language_<TITLEID>`) |
| General › Controller rumble (`RECOMP_RUMBLE`) | the emulated USB pad | PS1 DualShock, N64 Rumble Pak | no (an N64 game's pak: its Home, `n64_pak_<KEY>`) |
| General › Rewind, Rewind memory (`RECOMP_REWIND`, `_MB`, XBA-194) | no | yes (N64 off by default) | since XBA-188 only |
| Graphics › Vertical sync (`RECOMP_PRESENT_VSYNC`) | yes | yes | no |
| Graphics › Scale (`RECOMP_SCALE`) | no | yes | console game: its Home › Options, or the in-game menu (`scale_<KEY>`) |
| Graphics › Picture filter (`RECOMP_FILTER`) | no | yes | console game: the same (`filter_<KEY>`) |
| Graphics › Internal resolution (`RECOMP_UPSCALE`) | no | PS1, N64 (the GBA ignores it) | PS1 / N64 game: the same (`upscale_<KEY>`) |
| Graphics › Picture shader, Shader strength, CRT curvature (`RECOMP_SHADER*`, XBA-195) | yes (XBA-203; LCD grid draws none) | yes | any game: its Home › Game settings |
| Graphics › LCD ghosting (`RECOMP_SHADER_GHOSTING`) | no | yes | console game: its Home › Game settings |
| Graphics › Frame generation (`RECOMP_LSFG*`), Choose Lossless.dll | yes | yes (XBA-186: generated frames in place of the repeats) | console game: its Home › Options (`framegen_<KEY>`, on / off) |
| Graphics › Renderer, Frame cap, Resolution, Aspect ratio, Widescreen, 480p, Texture filtering | yes | no | Widescreen, 480p: Xbox game, in-game menu |
| Audio › Audio buffer (`RECOMP_AUDIO_BUF_SAMPLES`) | yes | yes | no |
| Audio › 3D audio (HRTF) | yes | no | no |
| Advanced › Drawing threads, Self-test | yes | no | no |
| Advanced › Fast cores | yes | yes (the app's start, `recomp_app_start`) | no |

**A console game's own scale and filter** (`ConsoleVideo.java`). Kept per library key in `files/settings/scale_<KEY>`
and `filter_<KEY>` (as `pad_map_<KEY>`; `aspect` / `integer` / `stretch`, `nearest` / `linear`; empty or absent:
follows Settings). Set from the game's Home › **Options** (a pill after Controls: « This game's picture », Scale and
Picture filter, « As in Settings (<Settings' value>) » then each value) or from the in-game menu › Settings
(**Scale** / **Filter**, shown at once: [Live picture](#live-picture-and-restart-to-apply)). Before the runtime starts, `ShellActivity.applyConsoleVideo`
sets, for a GBA, PS1 or N64 game, `RECOMP_SCALE` and `RECOMP_FILTER` to the game's value, else to recomp.cfg's line,
else unsets them (the runtime's default: aspect; GBA nearest, PS1 / N64 linear; `RECOMP_<CONSOLE>_SCALE` /
`_FILTER` still win there), and `RECOMP_PRESENT_VSYNC`, `RECOMP_AUDIO_BUF_SAMPLES`, `RECOMP_RUMBLE` from recomp.cfg
(unset without a line). An Xbox game gets `RECOMP_SCALE` / `RECOMP_FILTER` unset. Logcat: « shell: console video
RECOMP_SCALE=… RECOMP_FILTER=… …, game <KEY> ».

**A PS1 or N64 game's own internal resolution** (XBA-184, `ConsoleVideo.Setting.UPSCALE`). The same way:
`files/settings/upscale_<KEY>` (`1`, `2`, `4`, `8`; empty or absent: follows Settings), an **Internal resolution**
row after Picture filter in the game's Options (« As in Settings (<Settings' value>) », Native, 2x, 4x, 8x (N64
Vulkan)) and « Resolution » in the in-game menu › Settings (after « Restart to apply »); a GBA game has neither (its 240 x 160 picture
is drawn as it is). `ShellActivity` exports `RECOMP_UPSCALE` with the scale and filter: the game's value (PS1 / N64),
else recomp.cfg's line, else unset; unset for an Xbox game. The PS1 caps 8 at 4 (`ps1_runtime_upscale`).

**A console game's own frame generation** (XBA-186, `ConsoleVideo.Setting.FRAMEGEN`). In an APK with frame generation
(`R.bool.xba_lsfg`) only: `files/settings/framegen_<KEY>` (`1`, `0`; empty or absent: follows Settings), a **Frame
generation** row at the end of the picture rows in the game's Options (« As in Settings (<Settings' value>) », On,
Off; not in the in-game menu). `ShellActivity` exports `RECOMP_LSFG` (the game's value, else recomp.cfg's line, else
unset) and `RECOMP_LSFG_MULT`, `RECOMP_LSFG_PERF`, `RECOMP_LSFG_DIR` from recomp.cfg for a GBA, PS1 or N64 game, and
unsets `RECOMP_LSFG` for an Xbox game (its runtime reads recomp.cfg's line). On sets no multiplier: Settings' or 2×.

**An N64 game's own Vulkan driver.** The same Options screen, for an N64 game (only the N64 Vulkan renderer uses a
driver, for now), has a **Vulkan driver** row after the picture: « As in Settings (<Settings' driver>) », **System**,
then each installed driver; the description warns, in amber, when the driver the game would start with went wrong
last time (« Not compatible with the N64 renderer … », crashed, could not be opened, not loaded). The in-game menu ›
Settings has the same row, « Vulkan driver » (after « Restart to apply »), with a short warning in the value (« … · Not compatible »).
Not in a game's own app. Details in [Vulkan drivers](#vulkan-drivers).

**Game data of a console game.** The same Options screen lists the game's stored things (`Storage.items`), each
with its size, A twice to delete (saves: the stronger warning): **Saves** = `<internal>/save/<console>-<KEY>/`
(`game.sav`, `card1.mcr`, `game.eep`, `pak1.mpk`...; `Storage.saves`), the pack and the Prepare's files when there
are any. Storage (the library's) lists a console game's save folder too.

**The running game's title ID** (`GameLanguage.runningTitleId`). A GBA, PS1 or N64 launch
(`GameStore.rememberRomLaunch`) keeps the chosen Xbox folder (`game_dir`), so the XBE there is not the running game:
the in-game menu and `ShellActivity` (title video, language, game theme) key their per-game choices on the launched
game's library key while `rom` is set, the Xbox game's title ID otherwise. Before XBA-181 a console game played
after an Xbox game showed the Xbox game's Language / Widescreen / 480p rows and got its settings and theme.

#### Settings per game

XBA-188 (`GameSettings.java`). Any option of Settings can follow Settings or have its own value for one game, an Xbox
title or a GBA / PS1 / N64 game alike: graphics (renderer, frame cap, resolution, aspect ratio, widescreen, 480p,
texture filtering, vertical sync, the console picture's scale, filter and internal resolution, frame generation),
audio (buffer, 3D audio), pacing, rumble, drawing threads, fast cores, language; with the game's controls and an N64
game's controller pak. Only the developer's Self-test stays global.

**Where.** One file per game, `files/settings/game/<KEY>.cfg` (KEY: the Xbox title ID or the library key, upper
case), in `recomp.cfg`'s syntax:

- a key the file does not set follows Settings;
- `KEY=VALUE` is the game's own value;
- `KEY=` (empty) is the runtime's default for this game even when `recomp.cfg` sets another (the « Off » / « Auto »
  choices that write no line in `recomp.cfg`);
- an option of several keys (frame generation: `RECOMP_LSFG`, `_MULT`, `_PERF`) is the game's own when the file sets
  any of them; the keys it does not set keep Settings' values;
- besides the options: `RECOMP_PAD_MAP` (an Xbox title's own mapping), `RECOMP_<GBA|PS1|N64>_PAD_MAP` (a console
  game's), `RECOMP_N64_PAK`, and `XRECOMP_THEME` (the launcher's [game theme](#game-theme), never exported).

**Migration.** The per-game files of earlier versions (`language_`, `widescreen_`, `480p_`, `theme_`, `scale_`,
`filter_`, `upscale_`, `framegen_`, `pad_map_`, `n64_pak_` + KEY in `files/settings`) are moved into the game's file
the first time any game's settings are read (`GameSettings.migrate`), then deleted; a key the game's file already sets
wins. Off / native / aspect / frame generation off become `KEY=`, frame generation on becomes Settings' « 2x ».
`enhancements_<TITLEID>`, `vk_driver_*` and the launcher's own preferences stay where they were.

**Which value a game gets**, first found wins: the game's file, `recomp.cfg`, the runtime's default. One code path for
every console, `ShellActivity.applyGameSettings` before the runtime starts (`GameSettings.runEnv`):

- a key the game's file sets (and that applies to its console) is exported: the environment wins over `recomp.cfg`
  (`recomp_cfg.c`);
- its `KEY=` unsets the variable and names it in `RECOMP_CFG_SKIP` (comma-separated), whose lines
  `recomp_cfg_load` then ignores;
- every other key the launcher manages is unset, so `recomp.cfg` decides and nothing a previous game of the same
  process chose stays;
- `RECOMP_LANGUAGE`: an Xbox title's own, its « System » (the Android language over `recomp.cfg`'s), else as before
  ([Game language](#game-language)).

One logcat line at the start: « shell: game settings <KEY> (<console>): RECOMP_GPU_FPS_CAP=60, RECOMP_APU_HRTF=(default)
… » or « … none (as in Settings) ».

**Game settings screen.** From an Xbox title's Home (**Game settings**, the pill after Settings) and from a console
game's Home (**Game settings**, after Controls; it was « Options »): every option that applies to the game's console
(the [table](#settings-per-console): « All consoles » ones for every game, the Xbox's own for Xbox titles, scale,
filter, rewind and its memory for the consoles, internal resolution for the PS1 and the N64, frame generation in an APK with it), under
General / Graphics / Audio / Advanced. Each row: « As in Settings (<Settings' value>) », then each of Settings'
choices (left / right, A); a **This game** badge when the game's value differs from Settings'. Then « This game »:
**Controls** (« As in Settings » or « This game only »; A opens its Controls), for an Xbox title its Game theme and
Enhancements, for an N64 game its Vulkan driver; **Reset to Settings** (also Y: the file goes, its N64 Vulkan driver
choice too; the value says how many of its own); for a console game, its stored things. Saved at once; a running game
gets them at its next start.

**In-game menu.** Settings › **Game settings**: the same option rows for the running game, saved at once; the picture
options are shown at once, the others wait for « Restart to apply » (the page says so), and Reset to Settings. The Language, Widescreen, 480p, Scale, Filter, Resolution and Vulkan driver
rows of Settings stay; they write the same file.

**Xbox per-game controls.** An Xbox title's Controls has « Mapping for: All Xbox games / This game only » at the top,
as a console game's: this game only copies every game's mapping into its file (`RECOMP_PAD_MAP`) to change on its own,
all games drops it. The in-game menu › Controls shows the title's own mapping when it has one.

**Limits.** A title built before XBA-188 ignores `RECOMP_CFG_SKIP`: a game's « default » then does not beat a
`recomp.cfg` line (its own values still do). Players, the on-screen pad and the launcher's preferences stay global.

### Controls and remapping

XBA-131. One mapping for every title and every pad, `RECOMP_PAD_MAP` in `recomp.cfg` (`PadMapping`), which
`src/input/xinput_device.c` reads when the game starts (`[INPUT] RECOMP_PAD_MAP: …` in the log). No line is the
usual mapping: SDL's game controller layout by position (south = A as Android names it), L1 = **Black**, R1 =
**White**, L2 / R2 = the triggers, Select = **Back**, the stick presses = the thumbstick buttons. The text is
`control=source` entries for what differs, `left_stick=` / `right_stick=` (`left`, `right`, `none`), e.g.
`a=east,b=south,black=left_trigger,right_stick=none`; the grammar is `xbox_InputMap_PadMapParse`
([environment variables](../runtime/environment-variables.md#input)).

The **Controls** screen (from Home) is a Settings-like list, pad only: the D-pad moves, left / right or A change the
focused row's button (every source in turn: A B X Y, L1 R1 L2 R2, L3 R3, Select, Start, the D-pad's four, each stick's
four directions, Not used), Y or « Reset controls » puts the usual mapping back (the line goes), B goes back. Each
change is written at once (the « Saved » note; a running game takes it at its next start). The top right says how many
controls differ. The description column says where the focused control comes from (and which other control the same
button also gives: allowed, one press then gives both), and draws the handheld with each of the player's buttons and
what the game gets from it.

What it does not touch: the launcher's own navigation and the [in-game menu](#in-game-menu) (Android key events: A
selects, B goes back, Select + Start held opens the menu, whatever the mapping), the companion panel's Back / Start /
Black / White (Xbox buttons already, `input_overlay.h`), the keyboard (`RECOMP_KEYBOARD`, its own bindings) and the
Windows backend. Deadzones stay as before (none added); triggers keep their travel, and a trigger or a stick direction
used as a button counts as pressed from half way.

**Towards the touch pad (XBA-42).** A phone without the Thor's second screen needs an on-screen pad. What it reuses:
`input_overlay.h` (`xbox_InputOverlaySet`, merged into a real pad's state, a stand-in pad 0 without one; the
companion panel already drives it through `CompanionNative.nativeOverlay`), `CompanionPanel`'s pointer handling (each
pointer on its own, release on cancel and detach) and `PanelView`'s drawing, and `PadMapping.Control` with
`ControlsLabels` for the buttons' names. The overlay's buttons are Xbox controls already, so the remapping does not
apply to them (it maps a physical pad's buttons); the overlay needs analog sticks in `nativeOverlay` (today buttons
only) and a layout of its own drawn over the game.

### Device check (Controls, XBA-131), for Alex

1. Home › Controls: the list starts on A = A; the right column draws the Thor with « L1 · Black », « R1 · White »,
   « L2 · LT », « A B X Y ». Only the D-pad and A / B / Y move and change things. Screenshot.
2. On A press right once: A = B, « 1 changed » top right, the note « Saved ». On B press left once: B = A.
   `adb shell cat /sdcard/Android/data/<app id>/files/recomp.cfg` ends with `RECOMP_PAD_MAP=a=east,b=south`.
3. Play: in the game, the right face button confirms and the bottom one goes back (logcat
   `[INPUT] RECOMP_PAD_MAP: a=east,b=south`). The launcher and the in-game menu still use A to select.
4. In-game menu › Controls: « Your mapping » first, then « B · A » and « A · B ».
5. Back in Controls, Black = L2 (A twice on Black): in a game that uses Black, L2 does it, and L2 still pulls the
   left trigger too.
6. Right stick: Not used: the game's camera stick no longer moves. Then Y: everything back, the `RECOMP_PAD_MAP` line
   is gone from recomp.cfg, the game plays as before.
7. With an external pad paired (Bluetooth): the same mapping applies to it.
8. French: the screen in French (« Commandes », « Noir », « Blanc »).

### Players: four pads from the connected controllers (XBA-123)

The emulated USB pads (`RECOMP_USB`) follow the controllers: one per port that has one, plugged into the root hub as
it arrives and pulled out a second after it leaves, each on its own port, so the title's XAPI sees the insertion or
removal on that controller port (`src/usb/usb_hotplug.h`, `ohci.c`; `RECOMP_USB_PADS=n` keeps the old fixed count).
Which controller is which port is `src/input/input_ports.h`: at the game's start the controllers present are packed
from port 1 (pinned ones, then the handheld's built-in pad, then the rest), and a controller keeps its port across a
disconnect. Rumble goes to the controller of the port the game vibrates. A title that asks for a memory unit finds
none, as before (no hub, no memory unit model; saves stay in UDATA).

- `ShellActivity` sets `RECOMP_PAD_BUILTIN` (the gamepad Android reports as not external, `PadDevices`) before the
  runtime starts: the Thor's own pad is port 1 unless pinned otherwise.
- Controls › **Players**: Port 1 to 4, each First come (default) or a controller connected now (`PadPorts`, written
  as `RECOMP_PAD_PORTS=1=045e:0b13:Xbox Wireless Controller` in recomp.cfg; read at the next start). A controller
  pinned to one port leaves the other. The description says what each port would get with the controllers
  connected now (the same rule as the runtime; `PadPortsTest` and `tests/input_pad_ports` share the cases). A pinned
  controller that is not connected shows « (not connected) » and leaves no hole at the start.
- The mapping (`RECOMP_PAD_MAP`) stays **global**: one mapping for every port. A mapping belongs to a kind of
  controller rather than to a player, and a per-port one would follow the port when players swap pads; per-controller
  profiles can come later. (It now applies at all: before XBA-123 nothing called `xbox_InputInit`, so the variable
  was never read in a real run.)
- logcat: `[INPUT] port N: <name> (vvvv:pppp) connected` / `disconnected (the port waits for it)`,
  `[OHCI0] pad N plugged in / pulled out on root port P (controller port N)`, `shell: controllers [...]`.

### On-screen pad (XBA-42)

For a phone with one screen and no controller: both sticks (analog), the D-pad (eight ways), A B X Y, Black and
White, the triggers (full when touched: Android gives no usable pressure), Back and Start, LS / RS (stick presses),
translucent over the game. `TouchPad` (the layout and what the fingers press, pure Java), `TouchPadView` (draws it,
takes every pointer), `TouchControls` (shows or hides it), `OverlayMixer` (merges it with the companion panel's
buttons into the one overlay pad, `CompanionNative.nativeOverlayPad`, port 0).

- **Drawn by the app**, a View over SDL's surface under the in-game menu, not in the native present pass: the
  window is composited over the SurfaceView anyway (the menu's layer is there), and a View that redraws only when a
  finger changes something costs the game's frame nothing with either renderer, where the present pass would draw it
  every frame in two back ends. Touch goes from the UI thread to `input_overlay.h` directly (no SDL event queue), read
  by the next pad report.
- **Fingers**: each pointer on its own; a finger that lands on a stick or the D-pad keeps it until it lifts (off the
  circle = full tilt); a finger on a button can slide to the next one; everything is released on cancel, when hidden,
  on detach and in `onStop`. Sticks: 6 % dead zone, rescaled so the edge is full tilt. Nothing within 3 % of the left
  edge (the in-game menu's swipe). A light haptic tick when a button goes down.
- **When**: Controls › On-screen pad › Show: *Without a controller* (default: shown while Android lists no game
  controller, hidden as soon as a controller sends a key or a motion, back when the controllers are gone or the
  screen is touched with none connected), *Always*, *Never*. **Never while the companion panel is up** on a second
  screen (the Thor). Opacity 30 / 50 (default) / 70 / 100 %, size Small / Normal / Large. One line in
  `files/settings/touch_controls` (`AUTO 50 100`), read at each game start. Where each control sits: the defaults in
  `TouchPad.layout`, the player's changes on top ([Touch layout editor](#touch-layout-editor-xba-207)).
- logcat: `shell: touch controls shown|hidden (<why>, <mode>)`.
- Tests: `TouchPadTest` (two sticks and buttons at once, the D-pad's eight ways, sliding, a stick kept off the
  circle, triggers, cancel, the layout on screen and clear of the edge at every size, visibility, the mixer),
  `TouchControlsTest` (real multi-pointer MotionEvents, Auto / Always / Never, a controller used, the panel; screenshots
  `touch-1-default`, `touch-2-four-fingers`, `touch-3-large-opaque`, `touch-4-small-faint`), `LauncherPlayersTest`
  (`controls-4-players`, `controls-5-touch`).

### Touch layout editor (XBA-207)

Move, resize and hide the on-screen pad's controls, per console layout and optionally per game. `TouchLayout` (the
changes, their file, which one a game gets; pure Java), `TouchEdit` (the editor's gestures; pure Java), `TouchEditor`
(the full-screen layer: the pad in edit mode, `TouchPadView.startEditing`, and a small panel).

- **Opened from** Controls › On-screen pad › **Edit touch layout** (the Xbox Controls screen and a GBA / PS1 / N64
  game's Controls): left / right pick the layout (Xbox, Game Boy Advance, PlayStation, PlayStation (analog),
  Nintendo 64; the page's console first), A opens the editor over the launcher. And from the in-game menu ›
  Settings › **Edit touch layout** (shown with the On-screen pad row): the menu closes, the game stays held under the
  editor (the live pad hidden), and Save puts the new layout on the running game at once.
- **Editing**: drag a control to move it (its centre snaps to a grid of 1/40 of the screen's height while Grid is
  on); with a control selected, a second finger pinches its size, or − / + step it by 10 % (50 to 250 % of its
  default, on top of the global Size setting); Hide / Show; Reset puts every control back; Cancel or Back / B leaves
  without saving. Hidden controls stay in the editor as dashed ghosts (select one to show it again); in the game they
  are neither drawn nor hit-tested. Controls stay on screen and clear of the menu's left-edge swipe. Drag the panel's
  title to move the panel out of the way. Every pad control can be edited (sticks, D-pad, face buttons, shoulders and
  triggers, Start / Select / Back, stick presses, the C buttons); the in-game menu has no on-screen button (it opens
  with the edge swipe).
- **Scope**: *For all games* writes `files/settings/touch_layout/<LAYOUT>.txt` (`XBOX`, `GBA`, `PS1`, `PS1_ANALOG`,
  `N64`); *This game only* (offered when the editor knows the game: in game, or from a game's Controls) writes
  `files/settings/touch_layout/game/<KEY>.<LAYOUT>.txt` (KEY as in [Settings per game](#settings-per-game)), a sibling
  of the game's settings file. A game's file wins over its console's (`TouchLayout.resolve`); saving for all games
  removes the game's file. Nothing is written until the player saves, and a reset saved for all games removes the
  console's file: the defaults are unchanged until edited.
- **Format**: one line per changed control, `A x=0.8750 y=0.4630 size=1.20 hidden`: the centre normalised (x over
  the width, y over the height), so a layout survives a resolution change; a control with no line (or no x / y) keeps
  its default place, anchored to the edges. The app is landscape only (`sensorLandscape`): one layout per console, not
  per orientation. The companion panel on a second screen is not affected (the on-screen pad never shows with it).
- logcat: `touch layout: <LAYOUT> [for <KEY>] saved, N control(s) changed`, `shell: touch layout <LAYOUT> (this
  game's), N control(s) changed` at the game's start, `shell: menu: edit touch layout`.
- Tests: `TouchLayoutTest` (the format both ways, normalisation across sizes, the game's file over the console's,
  hidden controls not hit, drag on the grid, pinch, − / +, reset), `TouchEditorTest` (drag and save, This game only,
  B cancels; screenshots `ui-touch-editor-<layout>-<en|fr>`, `ui-touch-editor-hint-<en|fr>`).

### Touch robustness (XBA-212)

« The touch controls don't work » (AYN Thor, 0.5.1) with no regression in the touch → native path: every way the pad
can look broken is closed.

- **The in-game editor never passes for the game**: an opaque banner centred at the top (« Editing touch layout »,
  Cancel, Save) and a dark tint (`TouchEditor.GAME_TINT`) over the held game. It is cancelled (the old layout kept)
  when the game goes to the background (`onStop`), at each start (never restored), on `onResume`, when the in-game
  menu opens (the panel's Menu button) and on Back / B. A layout with no D-pad / stick or no face button left to press
  is not saved (the editor says so).
- **Saved layouts are repaired at load** (`TouchLayout.repair`): entries of controls the layout has not are dropped; a
  size that is NaN / zero / negative goes back to 100 %; a place off the screen, on an edge, NaN or with its centre in
  the menu's swipe strip goes back to the default place. A layout that still leaves no reachable direction control or
  face button (hidden, or stacked under another control: `TouchPad.playable`) is set aside for the console's defaults
  (`shell: touch layout <LAYOUT> set aside for the defaults: ...`). Settings › Controls › **Reset touch layouts**
  removes every saved layout (all consoles, every game's own).
- **Hotkey chords never mask the touch buttons**: the GBA / PS1 / N64 runtimes hide a held chord's buttons with
  `console_pad_swallow`, from the physical pad's part only; the overlay's targets (`ConsolePadOut.overlay_buttons`)
  always reach the game.
- **Auto on the Thor**: its built-in controls count as a controller, so Auto hides the pad (and the companion panel
  hides it on the top screen). A tap on the game's screen (outside the menu's swipe strip, menu closed) reveals it;
  that first tap sends nothing; it hides again after 10 s without a touch (`TouchControls.REVEAL_MS`) or when a
  physical button is pressed (or a stick / trigger pushed half way). Hidden that way, a hint once per game process
  (logcat and toast: « Touch pad hidden: ... »).
- A failed native overlay call is logged (PR #159) and said once in a toast.
- Tests: `TouchRobustTest` (editor auto-close and banner, repair / fallback, the editor refusing an unplayable layout,
  reset, a physical chord with a touch button held, reveal on tap, the panel), `tests/console_pad` (`test_swallow`).

### Device check (touch robustness, XBA-212), for Alex

1. Thor, Show *Without a controller*, Bottom screen Panel: start a game; a toast says the touch pad is hidden. Tap the
   middle of the top screen: the pad appears and the game got no button. Play with it; leave it 10 s untouched: it
   goes. Tap again, then press a physical button: it goes.
2. In-game menu › Settings › Edit touch layout: the banner « Editing touch layout » is at the top and the game is dark
   behind it. Press Home, come back: the editor is gone, the pad plays. Open it again, press B: gone.
3. Edit touch layout, hide the D-pad and both sticks, Save: refused with a message.
4. Settings › Controls › Reset touch layouts: « Touch layouts reset (N removed) »; games show the usual layout.
5. A PS1 / GBA game, Show *Always*: hold the touch Select while holding Select + right trigger (fast-forward) on
   the Thor's pad: the game still sees Select pressed (before: it was hidden while the chord was held).

### Device check (touch layout editor, XBA-207), for Alex

On the Thor: Bottom screen **Off** and Controls › On-screen pad › Show **Always** (as for XBA-42).

1. Controls › On-screen pad › Edit touch layout: left / right cycle Xbox, Game Boy Advance, PlayStation, PlayStation
   (analog), Nintendo 64; A opens the editor with that layout over a dark backdrop with a grid.
2. Drag A to the middle of the screen: it follows the finger and snaps to the grid; Grid: off, drag again: it moves
   freely. Drag a control towards the left edge: it stops short of it.
3. Select the left stick, then pinch with a second finger: it grows / shrinks; − / + step it by 10 %; the panel shows
   its name and size.
4. Hide Select (or Back): it turns into a dashed ghost. Save. Start a game of that console: A where you put it, Select
   gone, and a touch where Select was presses nothing.
5. In-game menu › Settings › Edit touch layout: the game stays frozen under the editor; move Start, Save: the game
   resumes with Start in its new place at once. Back / B in the editor leaves without changes.
6. In game, editor › *This game only* › move a button › Save. Another game of the same console keeps the console's
   layout; this game keeps its own after a restart.
7. Editor › Reset › Save (For all games): the usual layout is back.
8. Bottom screen back to Panel: the companion panel is unchanged and no on-screen pad shows on the top screen.
9. Drag the editor panel by its title to uncover a control under it.

### Device check (players and on-screen pad, XBA-123 / XBA-42), for Alex

A title with `RECOMP_USB` (its pack sets it) and two pads: the Thor's and a Bluetooth one.

1. Start the game with the Bluetooth pad off. logcat: `[INPUT] port 1: <Thor pad> (vvvv:pppp) connected`,
   `[OHCI0] pad 1 plugged in on root port 3 (controller port 1)`. Send `shell: controllers [...]`.
2. Turn the Bluetooth pad on in game: `port 2: … connected`, then `pad 2 plugged in on root port 4`. In a title with
   local multiplayer, player 2 can join with it (« Press Start » on the second slot).
3. Turn it off: after a second `pad 2 pulled out`; the title notices (a « reconnect controller 2 » message, or player 2
   drops). Turn it on again: same port (`port 2`), player 2 again.
4. Rumble: a title that vibrates player 2's pad vibrates the Bluetooth pad, not the Thor.
5. Controls › Players: Port 1 = the Bluetooth pad. Start the game: the Bluetooth pad is player 1, the Thor player 2.
   First come again afterwards.
6. A title that offers a memory unit: it finds none and saves to the console as before.

On-screen pad. What the Thor can check (its pad is always connected, so: Bottom screen **Off**, and Controls ›
On-screen pad › Show **Always**):

7. The pad over the game on the top screen; screenshot. Both thumbs on both sticks move the game's two sticks at once
   (camera and movement); A while steering; LT / RT; Back and Start; LS / RS; the D-pad's diagonals.
8. No added lag felt compared with the physical pad (press A on screen and on the pad, same response).
9. Swipe from the very left edge: the in-game menu opens, and the stick does not stay held after it closes.
10. Opacity 100 % and Large, then 30 % and Small: the pad follows at the next start.
11. Bottom screen back to Panel: the on-screen pad is gone from the top screen, whatever Show says.

On a phone without a controller (not possible on the Thor): with Show at its default, the pad shows by itself at game
start; pair a Bluetooth pad and press a button: it hides; unpair: it comes back.

### Controls for the other consoles

XBA-158 / XBA-168 / XBA-174. A GBA, PS1 or N64 game gets what an Xbox game has: its buttons remapped, per console
and per game, and the on-screen pad with the console's own buttons.

**Mapping layer** (`src/input/console_pad.h`, part of the host layer, pure C, `tests/console_pad`). Each console has
targets (GBA: A B L R Start Select, the D-pad; PS1: Cross Circle Square Triangle L1 R1 L2 R2 L3 R3 Start Select, the
D-pad, two sticks; N64: A B Z L R Start, the D-pad, C up / down / left / right, the stick). Each target takes a set of
the pad's sources (the names of `RECOMP_PAD_MAP`: `south`, `east`, `left_trigger`, `right_stick_up` …; a trigger or a
stick direction counts past the console's threshold), each stick a host stick. The defaults are the fixed mappings
the runtimes had (the GBA's A = A + Y, the N64's C-up = Y + right stick up …), so no line changes nothing. Text:
`target=source[+source]` or `=none`, `left_stick=` / `right_stick=` (PS1) and `stick=` (N64) `left|right|none`,
`range=1..127` and `deadzone=0..90` (N64, percent; absent: `RECOMP_N64_STICK_RANGE` / `RECOMP_N64_DEADZONE`), only what
differs, e.g. `RECOMP_GBA_PAD_MAP=a=east,b=south`. The runtime reads `RECOMP_GBA_PAD_MAP` / `RECOMP_PS1_PAD_MAP` /
`RECOMP_N64_PAD_MAP` at start (logcat `[INPUT] gba pad map: a=east,b=south`, or `(default)`) and maps each pad through it
(`gba_runtime_keys_of`, `ps1_runtime_pad_of`, `n64_runtime_buttons_of`; the old `*_pad_buttons` functions are the
defaults). PS1 digital mode: the mapped left stick is the D-pad too, as before.

**Home › Controls** (`ROM_CONTROLS`, the « Controls » pill last on a GBA / PS1 / N64 Home; `ConsolePadMap`,
`ConsoleControls`). Pad only, like the Xbox Controls: *Mapping for* · All <console> games / This game only, one row
per console button (its value the player's controls, « A + Y » for a default with two; left / right or A step it
through every source one at a time, then Not used), the sticks (PS1, N64), the N64's Stick range and Dead zone
(« Usual », then values), the On-screen pad rows (shared with the Xbox's), « Reset controls » (also Y: the usual
mapping for the scope shown). Written at once: all games → recomp.cfg's line; this game only →
`files/settings/pad_map_<KEY>` (« map: » + the text), which `ShellActivity` sets in the environment as
`RECOMP_<CONSOLE>_PAD_MAP` (it wins over recomp.cfg; no file: unset, recomp.cfg decides; logcat `shell: gba controls of
the game: …` / `of the console (recomp.cfg)`). Switching to *This game only* copies the console's mapping; back to all
games drops the copy. The in-game menu's Controls page lists the console's buttons and where each comes from.

**On-screen pad.** `TouchPad.Layout` per console (`layoutFor`, set by `ShellActivity`): GBA D-pad, A, B, L, R, Select,
Start; PS1 D-pad, Cross / Circle / Square / Triangle (drawn shapes), L1 / R1 / L2 / R2, Select, Start, and with
`RECOMP_PS1_PAD=analog` both sticks and L3 / R3; N64 stick, A, B, Z, L, R, Start, the four C buttons. Each button
presses the Xbox control the console's default mapping reads as it (L = Black, Z = left trigger, a C button = the
right stick pushed fully). The overlay goes through `CompanionNative.nativeOverlayPad` as before; `companion_jni.c`
now also hands it to `host_pad_overlay_set` (a host pad, SDL layout), and `console_pad_read` merges it into pad 1
**through the console's defaults**, never the player's remapping: the on-screen Cross is always Cross. Shown
automatically without a controller (Controls › On-screen pad › Show), never with the companion panel up; the in-game
menu's Settings › **On-screen pad** (Shown / Hidden) overrides that for the session (logcat `shell: touch controls
shown|hidden (menu: …)`). The companion panel's Back / Start / Black / White reach the console games the same way.

Tests: `tests/console_pad` (defaults = the old mappings, text, thresholds, the overlay merge), the console runtime tests
(the old `*_pad_buttons` cases unchanged), `ConsolePadMapTest`, `LauncherConsoleControlsTest` (screenshots
`console-controls-*`), `TouchPadTest` (layouts on screen at every size, nothing overlapping, what each button presses),
`TouchControlsTest` (`touch-5-gba` … `touch-8-n64`), `GameMenuTest` (`touchToggle`, `consoleControlsPage`).

### Hotkeys

XBA-190. The pad chords of fast-forward, quick save / load, the quick slot, rewind (XBA-194) and the in-game menu are the player's
(`Hotkeys.java`; the runtime's side: [consoles/README.md « Save states and fast-forward »](../../consoles/README.md),
`src/platform/recomp_hotkeys.h`). The text is `RECOMP_HOTKEYS` (`action:modifier+button`, only what differs from the
defaults): **every game's** in `recomp.cfg`, a **game's own** in its settings file (`files/settings/game/<KEY>.cfg`,
[Settings per game](#settings-per-game)), which `GameSettings.runEnv` exports (an Xbox title's too, for its menu
chord; « » = the defaults for this game, `recomp.cfg`'s line skipped).

| Action | Default | |
|---|---|---|
| Fast-forward (hold) | Select + R2 | while held |
| Fast-forward on / off | none | the menu's toggle |
| Quick save / Quick load | Select + R1 / Select + L1 | the quick slot |
| Next / Previous quick slot | none | around 1..4, the menu says « Quick slot: N » |
| Rewind (hold) | Select + L2 | XBA-194: back in time while held, one capture per frame, silent; let go to play on ([Rewind](#rewind)) |
| Game menu (held ½ s) | Select + Start | the only one an Xbox title has (no save states there, decision 2026-11-14) |

**Controls › Hotkeys** (the end of a GBA / PS1 / N64 game's Controls and of the Xbox Controls, before « Reset
controls »): one row per action (the menu's alone for an Xbox title), its value the chord (« Select + R1 », « None »).
**A** starts the capture: the row says « Press the buttons… »; the first button pressed is the modifier, the second
makes the chord, one pressed and released alone is a chord of that button (triggers as keys or as axes past half);
the releases that follow are eaten (the chord's A or B does not click or go back); nothing within 5 s, or Back on a
touch screen: « Nothing changed ». **Left / right** turn the hotkey off, or put the usual one back. **Conflicts**: the
value reads « Select + Y · conflict » when two actions have the same buttons (either order: both would happen) or a
chord of one button takes a button the game's mapping uses (it would no longer reach the game); the description says
which. « **Reset hotkeys** » puts every one back. Saved at once, in the scope shown: *Mapping for* · this game only
keeps them in the game's file (switching to it copies them, back to all games drops the copy), otherwise in
`recomp.cfg` for every game of every console. Logcat `launcher: hotkeys (every game) qsave:back+north`.

**In-game menu.** `MenuTriggers.Combo` listens to the game's menu chord (Select + Start by default; a chord of one
button: that button held; none: only Guide, Back and the swipe open the menu; a chord with a stick direction keeps
Select + Start). The Controls page lists Back, the menu chord, then each hotkey set for the game (none for an Xbox
title but the menu's).

Tests: `tests/recomp_states` (`test_hotkey_*`: text forms, remapped chords and keys per frame, what they run),
`HotkeysTest` (model, conflicts, capture, combo, storage and export), `LauncherConsoleControlsTest.gbaHotkeys`
(screenshot `console-controls-5-hotkeys`), `LauncherControlsTest` (the Xbox's menu row), `GameMenuTest`.

### Rewind

XBA-194. A GBA, PS1 or N64 game goes back in time while the **Rewind (hold)** hotkey is held (Select + L2 by default,
a [hotkey](#hotkeys) like the others: Controls › Hotkeys changes it, the in-game menu's Controls page lists it): each
frame shows one capture further back, without sound; letting go plays on from that moment (what came after is
forgotten). The runtime keeps the history ([consoles/README.md « Rewind »](../../consoles/README.md)): a capture every
2 frames on the GBA, 3 on the PS1, 4 on the N64, in at most **Settings › General › Rewind memory** (64 MB by default:
a minute or more of a GBA game, tens of seconds of a busy PS1 game). **Settings › General › Rewind**: « On (not N64) »
(default: the N64's captures cost more, 2 ms and up every 4 frames), On, Off; both rows are « All consoles » ones and
a game can have its own in its Game settings (an N64 game can turn rewind on there). Frame generation starts a fresh
history at each step. The quick save / load and the slots are unaffected: a slot loaded is a point the rewind can go
back past, to before the load.

Tests: `HotkeysTest.rewindAction` (the action, its default and text form, no clash with the other defaults, shown for
every console with save states and not for an Xbox title), `GameSettingsTest.rowsPerConsole` (rewind rows for the
GBA, PS1 and N64, not for an Xbox title); the runtime's in `tests/recomp_rewind` and the consoles' `*_rewind_*`.

### Cheats

XBA-197. A GBA, PS1 or N64 game's cheat codes, in two places that edit the same list:

- **In-game menu › Cheats** (under Fast-forward): each cheat with « On » / « Off » (A or left / right), **Add a code**
  and **Delete a code** (A on a cheat arms it, « A again to delete », A again within 4 s deletes). A change is written
  at once and the running game takes it within half a second of play (the runtime reads the file again when it
  changes): no restart.
- **Game settings › Cheats** (the game's Home › Game settings, « This game » group; not for an Xbox title): « 2 on of
  5 » or « No cheat code yet »; A opens the list: a check box per cheat (saved at each tap), « Add a code », « Delete a
  code » (a list, tap one).

« Add a code » is a dialog with the keyboard (the one place of the app that types text): a name (« Cheat N » when left
blank) and the code, one line per code line (`8003A2C4 0063`). The app checks the shape only: hex digits, `AAAAAAAA
VVVV` or `ADDRESS:VALUE` (and `AAAAAAAA VVVVVVVV` on the GBA, for GameShark / Action Replay codes); the runtime
decodes the code when it reads the file and logs why one it cannot use is not applied (`<console>: cheat [on] "..."
not applied: line 1: ...`). The format is detected (GBA: raw, GameShark / Action Replay v1-v2, Action Replay v3,
CodeBreaker; PS1 / N64: GameShark); the list of code types and what is not supported: [consoles/README.md
« Cheats »](../../consoles/README.md).

`Cheats` keeps the list in `files/settings/game/<KEY>.cheats` (the game settings' folder, a file of its own: « Reset to
Settings » leaves the codes), written through a temporary file and a rename; `ShellActivity` sets
`RECOMP_CHEATS_FILE` to it for a GBA / PS1 / N64 game (even before it exists) and unsets it for an Xbox title.
Cheats are not part of save states: a state loaded keeps the cheats that are on. Button codes (GBA GameShark 8,
N64 88 / 89) apply once each time the cheat is turned on.

Tests: `CheatsTest` (the file and its place, the native format both ways, names, codes cleaned up and checked per
console, add / toggle / delete / save), `GameMenuTest.cheatsPage` (rows for a GBA game, none without a console game,
On / Off written at once, Add through the host's dialog, Delete armed then done, B back).

### Device check (cheats, XBA-197), for Alex

On the AYN Thor, a GBA, a PS1 and an N64 game of yours in the library, codes for them from a source you trust (one
constant write each, e.g. infinite lives).

1. GBA game › Home › Game settings: « Cheats · No cheat code yet » in the « This game » group. A: the list says « No
   cheat code yet »; Add a code: the keyboard shows. Type a name and the code, Add: the list shows it checked; Done:
   the row reads « 1 on of 1 ». Screenshot.
2. Add a code with « 1234 » only: the dialog stays, the code field says what a line looks like. Cancel.
3. Play the game: logcat `gba: cheats: 1 from .../settings/game/GBA-....cheats, 1 on` and `gba: cheat [on] "..."
   (<format>, N ops)`; the cheat works in the game.
4. In-game menu (Select + Start) › Cheats: the cheat « On »; left: « Off »; Resume: within a second the game is
   normal again (lives go down); logcat lists the cheat « [off] ». Menu › Cheats › right: « On », the cheat is back.
5. Menu › Cheats › Add a code: add a second code; it shows « On » and works after Resume.
6. Menu › Cheats › Delete a code › A on the second: « A again to delete »; A: « Deleted: <name> », the page lists one
   cheat. B, B: back on the main page, Cheats focused.
7. Save state with the cheat on, turn it off, load the state: the game runs, the cheat stays off.
8. Same as 1 and 3 with the PS1 game (an `80XXXXXX YYYY` code) and the N64 game (`81XXXXXX YYYY`); logcat `ps1:` /
   `n64: cheat [on] ...`.
9. An Xbox title: no Cheats row in its Game settings, none in its in-game menu.
10. French: « Codes de triche », « Ajouter un code », « Supprimer un code », « A de nouveau pour supprimer ».
11. If you have a GBA pack (Prepare) and an Action Replay ROM patch code: turn it on, logcat says « cheats patch the
    cartridge: recompiled code set aside »; off: « recompiled code back ».

### Device check (hotkeys, XBA-190), for Alex

On the AYN Thor, a GBA game and an Xbox title of yours in the library.

1. GBA game › Controls: the Hotkeys section shows Fast-forward (hold) « Select + R2 », Quick save « Select + R1 »,
   Quick load « Select + L1 », Game menu « Select + Start », the others « None ». Screenshot.
2. Quick save › A, then hold Select and press Y: the row reads « Select + Y », the note « Quick save: Select + Y »; the
   screen did not go back or click anything. `recomp.cfg` has `RECOMP_HOTKEYS=qsave:back+north`.
3. Play: Select + Y says « Saved to slot N »; Select + R1 no longer saves (R1 reaches the game). Logcat
   `[INPUT] hotkeys: pad qsave:back+north`.
4. Fast-forward on / off › A, then Select + Y: both rows read « · conflict »; left on it: « None », the conflict goes.
5. Next quick slot › A, press L3 alone: in the game L3 says « Quick slot: 2 », then a quick save goes to slot 2.
   Next quick slot › A, press A alone: « A · conflict », the description says the game uses A; right puts « None ».
6. Fast-forward (hold) › A, hold L2 then press R2 (the Thor's analog triggers): « L2 + R2 ». In the game L2 + R2 held
   fast-forwards, neither trigger reaches the game while both are held.
7. Mapping for → « This game only »: the hotkeys come along; Reset hotkeys there: this game is back to the usual chords,
   another GBA game keeps Select + Y.
8. Game menu › A, press L1 then R1: in the game L1 + R1 held ½ s opens the menu; Select + Start no longer does.
   The menu's Controls page lists « L1 + R1 · Game menu (held ½ s) » and the hotkeys set.
9. Xbox title › Controls: Hotkeys has only Game menu. Change it (L1 + R1) and check it opens the menu in the game.
10. French: the section reads « Raccourcis », « Appuyez sur les boutons… », « · conflit ».
11. Reset hotkeys everywhere you changed them; `recomp.cfg` loses the line.

### Device check (controls for the other consoles, XBA-158 / XBA-168 / XBA-174), for Alex

On the AYN Thor, a GBA game, a PS1 disc and an N64 cartridge of yours in the library.

1. GBA game › Home: the pills end with **Controls**; open it. Screenshot. Rows A … D-pad right, A = « A + Y »,
   Mapping for = « All Game Boy Advance games ».
2. On A press right once: A = B, « 1 changed », « Saved ». `adb shell cat /sdcard/Android/data/<app id>/files/recomp.cfg`
   has `RECOMP_GBA_PAD_MAP=a=east`.
3. Play: the right face button is the GBA's A, the bottom one does nothing for A (logcat
   `[INPUT] gba pad map: a=east`). The launcher and the in-game menu still select with the bottom button.
4. In-game menu › Controls: « Your mapping », then each GBA button with your control (A · B).
5. Back in Controls: Mapping for → « This game only » (note « This game has its own mapping »); L → L2 alone. Play this
   game: L2 is L, L1 no longer (logcat `shell: gba controls of the game: a=east,l=left_trigger`). Another GBA game:
   L1 still L, A still on B (the console's mapping).
6. Y in this game's Controls: its mapping back to the usual one (A = A + Y) while the other GBA games keep A = B.
   Mapping for → « All … games »: the copy goes. Y again: recomp.cfg's line goes.
7. PS1 disc › Controls: Cross … R3, Left / Right stick; Cross → B: in the game Cross is the right button. Reset.
8. N64 cartridge › Controls: C buttons, Analog stick, Stick range, Dead zone. Dead zone 20 %: the stick needs a larger
   push before Mario moves; Stick range 64: he no longer runs at full tilt. Reset.
9. On-screen pad, the Thor's built-in controls standing in for « no controller »: Bottom screen **Off**, Controls ›
   On-screen pad › Show **Always** (the Thor's pad always counts as connected). Each game shows its console's layout
   (screenshots): GBA D-pad / A / B / L / R / Select / Start; PS1 the four shapes, L1 L2 R2 R1; N64 stick, C buttons,
   Z bottom left. Every button works, two at once (stick + A, C-up + C-right), Start pauses.
10. With A remapped to B (step 2), the on-screen A is still the GBA's A.
11. In-game menu › Settings › On-screen pad: Hidden → the pad goes for the session; Shown → back. Bottom screen back to
    Panel: no on-screen pad on the top screen; the panel's Start pauses the GBA game.
12. On a phone without a controller: Show at its default, the console's pad appears at game start, hides when a
    Bluetooth pad is used.

### Frame generation

Lossless Scaling frame generation (LSFG 3.1, [Frame Generation](../technical/framegen.md)) is in every APK by default
(XBA-187): a plain `gradle assembleDebug` / `assembleRelease` builds the Vulkan bridge into `libmain.so` and the DXBC
translator into `libxba_import.so`, over sources CMake fetches at pinned commits (framegen.md « Build »; kept in
`third_party/lsfg-sources` or `$XBOXRECOMP_DEPS_DIR`, the Android image has them already).

| Gradle property | Environment | What |
|---|---|---|
| `-PxbaLsfg=0` | | Leaves frame generation out (offline builds): no bridge, no translator, the launcher hides the rows (`R.bool.xba_lsfg` false). Default `1`. |
| `-PlsfgVkSrc=<dir>` | `LSFG_VK_SRC` | A GameNative/lsfg-vk-android checkout (d33caceb, with its `thirdparty/volk` submodule) instead of the pinned archive. |
| `-PdxvkSrc=<dir>` | `DXVK_SRC` | A DXVK v2.6.2 checkout (with `include/spirv`) instead of the pinned archive. A build without the translator (none today) only extracts at import; a cache translated on a desktop (`tools/lsfg/lsfg_translate --dir`) can be pushed into `<files>/lsfg/<key>`. |

In the launcher, Settings › Graphics:

- **Frame generation** (under « All consoles » since XBA-186): Off / 2× / 2× performance (above). An Xbox game then
  runs at 30 real fps (cap rule) and shows 60; the overlay's `LSFG` line gives real / shown fps and generation time.
  A GBA, PS1 or N64 game keeps its own rate: the pictures it shows twice or three times are replaced by generated
  ones (a 30 fps game shows 60; one at 60 is left as it is; [Console window](../technical/framegen.md#console-window)),
  and the log has `[LSFG] console: ...` every 5 s. Any failure (no Vulkan, no shaders, too slow
  30 frames in a row) logs `[LSFG] off: <reason>`, shows one toast, and the game goes on with normal frames.
- **Choose Lossless.dll**: `ACTION_OPEN_DOCUMENT`; the picked file's fd goes to `LsfgNative.importDll(fd,
  <files>/lsfg, …)` on the import worker (the ring screen, « Preparing the frame generation shaders… »). The DLL is
  read through the fd only, never copied into the app's storage; its shaders land in `<files>/lsfg/<key>` (key: the
  first 16 hex digits of its SHA-256). The row then says **Shaders ready (LSFG 3.1)**, **This Lossless.dll version is
  not supported**, or **… (translator missing)** (DXBC found, APK built without `-PdxvkSrc`); in the first and last
  case `RECOMP_LSFG_DIR` goes into `recomp.cfg`.
- `<files>/lsfg` is kept out of backups and device transfers (`res/xml/backup_rules.xml`,
  `data_extraction_rules.xml`; `allowBackup` is off anyway) and `LogProvider` refuses any path under it.

Device check: [framegen.md](../technical/framegen.md) and the Device checklist of the frame generation work.

### Vulkan drivers

The N64 Vulkan RDP can run on a Mesa Turnip driver instead of the system one (only the N64 Vulkan renderer, for
now). **No driver is part of the app**: the player picks one from a list of public GitHub releases (or imports a
zip), and it is downloaded into the app's own storage. `VkDriverIndex` makes the list and the download, `VkDrivers`
keeps the drivers and the choice, `VkDriverStore` joins them (the `VkDriverIndex.Store` adapter, the readable
texts), `LauncherActivity` draws the screen, `VkDriverShare` / `VkDriverProvider` hand the choice to a game's own
app.

**Settings › Graphics › Vulkan driver** (the last row of « All consoles »). The value is the default driver, the one
every game uses unless it has its own: **System** (the default), or its name and version (« Turnip … (Mesa 26.3.0) »);
the description says it applies to the N64 Vulkan renderer for now, that a game can override it in its Options, and
that a driver that does not work falls back to the device's driver, then the software renderer; when the default
driver's last start went wrong, the description says how (below). A opens the Drivers screen.

**A game's own driver** (per game, N64 games only for now): its Home › Options › **Vulkan driver** and the in-game
menu › Settings › **Vulkan driver** cycle through « As in Settings (<the default's name>) » (the
default), **System** (the device's driver whatever Settings says), and each installed driver. Saved at once in
`files/settings/vk_driver_<KEY>` (the library key upper case, as `n64_pak_<KEY>`: a driver id, `system`, or
empty / absent to follow Settings); it applies at the game's next start. The warning shown is the one of the driver
that game would start with. `ShellActivity.applyVkDriver` resolves the driver with `VkDrivers.resolve(key)`: the
game's choice (`system`: none; an installed id: that driver; an id no longer installed: as if unset), else Settings'
choice, else the system driver.

**The Drivers screen** (`Screen.DRIVERS`, the list template, every row reachable with the D-pad; hints A Choose,
X Delete, B Back; Y refreshes):

| Row | Value / badge | A | Description |
|---|---|---|---|
| On this device › **System driver** | « Active » when no driver is chosen | back to the system driver | the device's own driver; the footer below |
| one row per installed driver (by name) | `driverVersion` (else `packageVersion`); « Active » (Settings' default), or « Not compatible » / « Active · not compatible » (`unsupported`), or « Failed last time » / « Active · failed last time » (any other failure) | makes it the default one (and clears the trial marker) | its description, version, source (« GitHub · owner/repo · tag », « Imported zip », « From XRecomp »), size on disk; the last start's warning in amber |
| **Import a zip** | | the system document picker (`ACTION_OPEN_DOCUMENT`, `REQ_VK_DRIVER`), then the package is checked and installed on the worker | what a package is |
| To download › **Refresh the list** | the list's state: « List from <date> », « <date> (old) », « Updating… », « No connection · List from <date> », « GitHub limit reached, retry after <time> · … », « GitHub did not answer (HTTP n) », « Not fetched yet » | asks GitHub now (every source; on a thread, the screen is rebuilt when it answers, the note repeats the state) | which repositories, and that this is the only network use |
| one row per listed driver not installed (same sha256, or the id that sha256 gives), by file name without `.zip` then `.adpkg` (« turnip_mrpurple_T29-toasted »; `VkDriverStore.rowNames`, with « · <release tag> » when two listed files give the same name) | its size; « Pre-release » | downloads it behind the progress screen (percent, bytes; B cancels), then installs it | release title, source, tag, file, date, size, pre-release; the footer |
| (no row) | | | « No list yet. Refresh … » or « Every listed driver is installed. » as a line of text |

X twice within 5 s on an installed driver deletes it (the badge says « Press X again »; the default one: Settings
back to the system driver; every game that had chosen it follows Settings again, its `vk_driver_<KEY>` removed); X
elsewhere only says how. After a download or an import the new driver's row has the focus and the
note says « … installed. Press A to use it. »: it is not made active by itself. The footer (in the description
column): drivers come from third parties, are never part of the app, and run as native code inside it. A choice
made while a game runs applies at its next start (the note says so). When the screen opens and a source's copy is
older than 24 h (or missing), the list is asked once by itself; nothing else is fetched.

Last start (the native runtime's `vk_drivers/trying`, `vk_drivers/<id>.status` and `vk_drivers/last_start`,
[environment variables](../runtime/environment-variables.md#custom-vulkan-driver-n64-rdp)). Per driver: the runtime
writes each start's status both to `last_start` (`<id> <status>`, the last start with any driver) and to
`<id>.status` (that driver's own); the app reads `<id>.status` first, else `last_start` when its line names the driver
(a file from an older runtime). With per-game choices two games can use two drivers, and `last_start` alone would
only know the last one. The trial marker or `crashed` → « crashed the game at its last start: skipped until you
choose it again » (choosing it again, in Settings or for a game, clears the marker); `unsupported` → « Not compatible
with the N64 renderer: … loaded, but paraLLEl-RDP cannot run on it » (no device, « lacks what paraLLEl-RDP needs »,
or every shader try failed; K11MCH1's `Turnip_v26.0.0_R8` on the Thor); `open_failed` → « could not be opened »;
`failed` (an older runtime's word for both) → « could not be opened, or the renderer could not run on it »;
`not_loaded` → « was not loaded (Android kept the device's own driver) ». `ok`, another driver's line, or no file:
nothing. Deleting a driver deletes its `<id>.status`.

Refusals in plain words (`VkDriverStore.errorText`, nothing kept in every case): not a zip; not an adrenotools
package (entries, `meta.json`, the library); over 256 MB; not a 64-bit ARM library; needs a newer Android; not the
file GitHub lists (size or sha256, from the store or the download); a disk error (with its detail); an address not
allowed; an HTTP status; no connection; cancelled.

**Stored where** (app-private `files/`, excluded from backups and from `LogProvider`): `vk_drivers/<id>/` (the
package's files, read-only, and `origin.json`; a directory without `origin.json`, put there by hand, counts as an
imported zip), `vk_drivers/index/` (the list's cache), `vk_drivers/trying`, `last_start` and `<id>.status` (the
runtime's), `settings/vk_driver` (Settings' choice: an id, or empty for the system driver), `settings/vk_driver_<KEY>`
(a game's own: an id, `system`, or empty / absent to follow Settings). `ShellActivity.applyVkDriver` turns the
resolved driver into `RECOMP_VK_DRIVER_DIR` / `_LIB` / `_ID` / `RECOMP_VK_HOOK_DIR` for an N64 game (logcat
`shell: vulkan driver <name> (<id>) (as in Settings|the game's own choice), game <key>`, or `system …`).

**A game's own app** has no network permission, no list, no download, no import and no choice of its own: its
Graphics row shows the driver it took from XRecomp and pressing it says the driver is chosen in XRecomp. At each
start (its launcher, on a thread; and `ShellActivity` before an N64 game, synchronously) it asks XRecomp through
`VkDriverProvider` (`<XRecomp's package>.vkdrivers`, exported; every call is checked like the pack handover:
`PackHandover.checkCaller`, package `fr.adserver.xrecomp.t<tid>` signed by the game apps' key):
`call("active", <tid>)` gives the id of the driver that title starts with in XRecomp (`VkDrivers.resolve(<tid>)`: its
own choice in XRecomp, else Settings'; empty: system); a new id is read from `content://…vkdrivers/<id>`
(`VkDrivers.exportZip` streamed through a pipe; only the calling title's driver is served), installed with
`Origin.inherited` under XRecomp's id and made active; the system driver clears the choice; inherited drivers no
longer chosen are deleted; an unchanged choice writes nothing (the runtime's trial marker stays). Limits: XRecomp
not installed, older than this provider, or refusing → the app keeps what it had; a change in XRecomp reaches the
game app at its next start, not while it runs; the game app keeps one choice (its title's, stored as its own
`settings/vk_driver`) and has no per-game row; a game's own app exists only for Xbox titles today, so the inherited
driver has no effect until an N64 game can have one.

**Network use.** This is the only place XRecomp uses the network, hence `android.permission.INTERNET` in the manifest
(no `ACCESS_NETWORK_STATE`; cleartext stays off). Requests are made only when the Drivers screen asks: nothing at
start, nothing in the background. A game's own app does not get the permission (`TitleApk.PARENT_ONLY_PERMISSIONS`
drops its `<uses-permission>` from the copy), and `VkDriverIndex.networkAllowed` is false there: no request, state
`NO_NETWORK`.

| Source | Listed |
|---|---|
| `MrPurple666/purple-turnip` | every `.zip` asset |
| `K11MCH1/AdrenoToolsDrivers` | `.zip` assets whose name contains « turnip » (case ignored) |

Both: published releases only (no drafts; pre-releases listed and flagged), the asset URL on `https://github.com/`
under that repository's `releases/download/`, a plain file name; an asset, release title or tag naming « qualcomm »
is never listed (K11MCH1 also hosts Qualcomm's proprietary drivers: `Qualcomm_*_adpkg.zip`, `8Elite2-*.zip`). The
same file (sha256) in two releases is listed once, in the newest. Each entry: source, tag, release title, date
(`published_at`), file name, size, sha256 (GitHub's asset `digest`, when present), URL, install state (same sha256 as
an installed driver).

**The list.** One request per source: `GET https://api.github.com/repos/<owner>/<repo>/releases?per_page=30`,
unauthenticated, `User-Agent: XRecomp` (nothing about the player or the device), `If-None-Match` with the saved
ETag. Cache: `files/vk_drivers/index/<owner>_<repo>.json` (format, ETag, fetch time, every `.zip` asset; the filter
applies when read, so a new filter in an update applies to an old copy). A copy younger than 24 h is used as is;
Refresh asks anyway. 304 keeps the copy. A source's state for the screen's note:

| State | Meaning |
|---|---|
| `CACHED` / `UPDATED` / `NOT_MODIFIED` | the list is current (copy < 24 h, fetched now, unchanged) |
| `STALE` | (cached list only) older than 24 h: the next refresh asks |
| `OFFLINE` | no connection: the cached list, « list from <date> » |
| `RATE_LIMITED` | GitHub's 60 requests/hour reached (403/429, `x-ratelimit-remaining: 0`): the cached list, retry after `x-ratelimit-reset`; no automatic request before then |
| `FAILED` | another HTTP status or an unreadable answer: the cached list |
| `NO_NETWORK` | the app may not use the network (a game's own app) |
| `NONE` | nothing cached and nothing asked yet |

No exception reaches the UI from `refresh`; with no cache the list is empty (the screen still shows installed
drivers and the import row).

**The download** (`VkDriverIndex.download`, on the launcher's worker, progress and cancel callbacks): https only,
`github.com` then at most 5 redirects to `*.githubusercontent.com` (anything else refused), into
`getCacheDir()/vkd-<asset>.part`; the size must equal the release's, the sha256 its `digest` when GitHub gives one.
The file then goes to the driver store with its sha256 (it validates the package); the temporary file is deleted in
every case. A mismatch is reported (`DownloadException`, reason `SIZE` / `SHA256`) and nothing is kept. The hash
proves the file is the one GitHub lists, not that the build is trustworthy: a driver is third-party native code.

Tests: `VkDriverIndexTest` (recorded releases pages of both sources trimmed to the fields used,
`src/test/resources/vkdriver/`; T30 with its date, size and sha256; the Turnip filter; ETag, 24 h, 304; limit,
offline, errors; verified download with a fake HTTP layer, no driver binary), `TitleApkTest` (INTERNET not in a
game's own app), `VkDriversTest` (packages, storage, choice), `VkDriverStoreTest` (the adapter, a download through
it, last start, texts, a driver without `origin.json`), `VkDriverPerGameTest` (a game's choice: storage, resolution
game → Settings → system, the reset on delete, `<id>.status` before `last_start`, the status words, the warnings and
badges, the row's cycle and labels, download names), `VkDriverShareTest` (a game's own app following XRecomp, its
title's own choice included), `LauncherDriversTest` (the Graphics row and its place in « All consoles », the Drivers
screen with a fake GitHub: choose, delete, offline, limit, names without `.zip` / `.adpkg`; screenshots
`build/screenshots/ui-drivers-*.png`), `LauncherConsoleControlsTest.n64OptionsVulkanDriver` (the Options row,
screenshot `console-options-2-n64-vkdriver.png`), `GameMenuTest.n64GameVulkanDriverRow`. Native:
`consoles/n64/tests` ctest `n64_vk_driver` and `n64_vk_driver_wrapper` (a fake `libxba_vkdriver.so`: `ok`,
`unsupported`, `not_loaded` after an open, `<id>.status`).

#### Device check (Vulkan drivers), for Alex

1. Settings › Graphics (R1): « Vulkan driver » is the last row of « All consoles » (before « Xbox only »), value
   « System » on a fresh install; its description says a game can override it in its Options.
2. A: the Drivers screen; « System driver » is « Active »; the list is asked once (« Updating… », then « List from
   <today> ») and lists purple-turnip and K11MCH1's Turnip builds, no Qualcomm build.
3. A on a listed driver: the progress screen (percent), then back on the screen with the new driver focused and
   « … installed. Press A to use it. »; A: it shows « Active ».
4. Start an N64 game: `adb logcat -d | grep "shell: vulkan driver"` names the driver and its id; the `n64:` start
   line says `custom <id>`.
5. Airplane mode, Y: « No connection · List from … », the list still there.
6. X, X on the active driver: deleted, « System driver » active again.
7. « Import a zip »: pick a Turnip `.zip` from Downloads: installed as « Imported zip »; pick any other zip: a plain
   refusal, nothing kept.
8. With K11MCH1 `Turnip_v26.0.0_R8` and purple-turnip T30 installed and Settings on T30: an N64 game's Home ›
   Options › Vulkan driver « As in Settings (… T30 …) »; set it to R8, start the game: logcat
   `shell: vulkan driver … (e634db0f929e2205) (the game's own choice)`; back in Options, the description says « Not
   compatible with the N64 renderer », the Drivers screen shows R8 « Not compatible »;
   `adb exec-out run-as fr.adserver.xrecomp.shell cat files/vk_drivers/e634db0f929e2205.status` → `unsupported`.
   Another N64 game (following Settings) starts on T30 (`custom f65b2d3353fd4aa7`), and the first game's warning stays.
9. In-game menu › Settings of an N64 game: « Vulkan driver » cycles the same choices.
10. Delete R8 (X, X): the first game's Options row is « As in Settings » again.
11. Download names in the list have no `.zip` / `.adpkg` (« turnip_mrpurple_T29-toasted »).

## Choosing the game

(XBA-54, and the import half of XBA-40.) The runtime reads the game with POSIX calls (`open`, `opendir`, the kernel's
case-insensitive lookup), so it needs a real path, not a `content://` URI. The launcher's source screens therefore
end with a folder path the runtime can open: the folder itself, or the app's `files/game/` after a copy or an
extraction. A disc image is the exception (XBA-126): the runtime reads it itself, by path or through fds the game
process opens from its documents ([Disc images read in place](#disc-images-read-in-place-xba-126)).

### Choose game folder

1. Without All files access (`MANAGE_EXTERNAL_STORAGE`), the « Read the game in place? » screen explains the choice:
   with it the game is read in place from the folder (phone or SD card, no copy, fine for a 4 GB disc, only that folder
   is read); without it the folder is copied into the app's storage (as much free space again, a few minutes, removed
   with the app). « Allow all files access » opens the system's settings page for the app; on return the folder
   picker opens either way. « Copy into the app » opens it directly. With the permission already granted, the folder
   picker opens straight from the Game folder card.
2. The system folder picker (`ACTION_OPEN_DOCUMENT_TREE`).
3. **With the permission**, the tree URI is mapped to a path: the external storage provider's `primary:Games/Halo`
   is `/storage/emulated/0/Games/Halo`, `1A2B-3C4D:Halo` is `/storage/1A2B-3C4D/Halo` (SD card), `home:X` is
   `Documents/X`; the downloads provider's `raw:/path` is a path already. No `default.xbe` there: `NOT_A_GAME_FOLDER`
   in the note, and the card can be chosen again. Otherwise the [hash check](#wrong_game) (the import screen shows
   « Checking the game »), then the folder is remembered as is.
4. **Without the permission** (or a provider that is not a storage volume, or a path that cannot be listed): the tree
   is listed through `DocumentsContract`, `default.xbe` checked (`NOT_A_GAME_FOLDER`) and hashed from the provider
   before anything is copied, the free space checked against the total, then everything is copied to
   `files/game.import/` with progress and Cancel, checked again and swapped in place of `files/game/` (the old
   `game/` becomes `game.old/` and is removed once the new one is in). `files/game` is remembered.

An error or a Cancel ends on Choose your game with the reason in the note; the previous game is untouched.

### Choose disc image (ISO)

1. The system file picker (`ACTION_OPEN_DOCUMENT`, any type: providers rarely know a MIME type for `.iso`; several
   files at once, for the parts of a split image); a name that is not an image (`.iso .xiso .cso .ciso .zso .cci` or
   a split part) gives `NOT_AN_ISO`, several files that are not one split set from its first part
   `SPLIT_PARTS_INCOMPLETE`.
2. `ParcelFileDescriptor`s are opened on its parts (with All files access and a storage-volume document: its path,
   the other parts found by name), and their fds go to `libxba_import.so` (`XisoNative`, JNI in
   `android/app/src/main/cpp/xiso_jni.c`): `probeParts` gives the title, the total size of the files, whether
   `default.xbe` is in the root, the kind (iso, cso, split...) and the bytes stored. Not an XISO, or no `default.xbe`:
   `NOT_A_GAME_IMAGE` with the reason (`xiso_strerror`).
3. **Read in place** (the default, XBA-126) when the image can be reached at every start: a path, or documents whose
   read permission persisted (`takePersistableUriPermission`). See
   [Disc images read in place](#disc-images-read-in-place-xba-126). Otherwise, or with `RECOMP_DISC_IMAGE=0` in
   `recomp.cfg`, it is extracted:
4. Free space in the external files dir must be the total plus 64 MB, or `NOT_ENOUGH_SPACE` names both.
5. `extractParts` extracts into `games/.import/` (through `.partial`, renamed at the end) on the background
   thread; its progress callback calls back into Java (at each file start and at most ten times a second), which
   updates the ring and returns Cancel. A cancel or an error leaves the previous game untouched.
6. The extracted `default.xbe` is hashed ([WRONG_GAME](#wrong_game)), then `.import/` is swapped in as
   `games/<TID>/` and remembered.

### Disc images read in place (XBA-126)

An image is not extracted when the app can open it again at every game start. Only its XBE files (`extractXbes`: every
`*.xbe`, at most 16 of 32 MB) are copied, with their directories, into a **companion folder** `files/discs/<TID>/`,
next to `disc.json` (`DiscImages`): `path` (with All files access) or `uris` (the documents, in part order), `names`,
`kind`, `stored`, `files`. The folder is checked like an import ([WRONG_GAME](#wrong_game) on its `default.xbe`), staged
in `files/discs/.import/` and swapped in; it is the record's `data_dir` (`data_kind` `image`). Everything that reads a
game folder works on it: title, art and theme, hash, the Prepare (the other images included, XBA-82), Home.

At each game start `ShellActivity` reads `disc.json` and sets `RECOMP_GAME_DIR` to the image's path, or to
`fd:N[,M...]` from the documents opened there (`openFileDescriptor`, `detachFd`: the fds live as long as the game
process). A quick reboot or a hand-over to another image restarts the process, which opens them again. The runtime
mounts the image ([disc-images.md](../technical/disc-images.md)); the pack loader reads the title ID out of it.
Unreachable (SD card out, permission revoked): `fd:-1`, and the shell shows its no-game-data message; with a path,
Home already says the source is gone (`SOURCE_GONE`). logcat: `launcher: image <name>: cso split, 2 part(s), read in
place`, `shell: disc image <name> (<kind>) -> <spec>`, then `[DISC] mounted ...`.

Storage and Manage show the row **Disc image, read in place**: what the app holds (a few MB) against the image's
size, the image's path or name in the description. Deleting it forgets the image (the companion folder goes; the
record stays in the library); the image itself is never touched. The library's **Import** of an image does the same
(its documents under the library folder's tree, whose permission persists, or its path).

Why a separate JNI library rather than `libmain.so`: `libmain.so` needs `libSDL2.so` loaded first and brings the
whole runtime (`--whole-archive` toolkit, the title) into a process that would never run it. `libxba_import.so` is
`xiso_jni.c` plus the static `xiso` target (libc only) and `liblog`.

### WRONG_GAME

A title's APK is made for one disc. `-PxbaXbeSha256=<sha256sum of default.xbe>` bakes the expected hash in (string
resource `xba_xbe_sha256`, 64 hex digits, checked by Gradle); empty, as for the shell and the stub, means no check.
The launcher computes the SHA-256 of `default.xbe` of the chosen folder (in place), of the provider's file (before a
copy) or of the extracted tree (after an extraction, before the swap). A mismatch shows `WRONG_GAME` and changes
nothing: the previous choice is kept, `files/game/` is untouched, the staging folder is removed. Home checks the
current folder once more in the background (chip « Wrong game »). logcat names the hash:
`launcher: ... sha256 <hex>`.

### How the path reaches the runtime

The choice is in the app's SharedPreferences (`game`: `game_dir`, `title`), written with `commit()` before the
game's process starts. `ShellActivity.onCreate` calls `Os.setenv("RECOMP_GAME_DIR", dir)` before
`super.onCreate()`: `SDL_main` runs later on SDLThread, and `recomp_app_start` → `recomp_storage_resolve` reads the
variable (the process environment is libc's, so `getenv` sees it). An environment variable wins over `recomp.cfg`,
so a remembered choice wins over a `RECOMP_GAME_DIR` line there. Saves stay in internal storage whatever the game
folder.

### Device check (XBA-41, with XBA-54 and XBA-40), for Alex

Use the stub APK (**shell-latest**) and its `stub-default.xbe` as the « game »: a folder holding only it is enough
for the stub title. `adb logcat -c && adb logcat -s xboxrecomp SDL AndroidRuntime DEBUG` in a terminal throughout.
Use the Thor's gamepad for everything unless a step says touch.

1. Install the APK with nothing configured (`adb uninstall` first). Expected: the Welcome screen, landscape,
   fullscreen (no status or navigation bar), nothing hidden under the camera cutout, « Get started » focused with a
   green outline; logcat `launcher: shown (no game set up)`.
2. Look at the layout: texts not cut, nothing overlapping, the button hints at the bottom right (A Start, B Quit).
   Send a screenshot (`adb exec-out screencap -p > welcome.png`).
3. A on « Get started »: Choose your game, Game folder focused. D-pad down / up moves between the choices;
   B goes back to Welcome; A again.
4. Copy `stub-default.xbe` as `default.xbe` into a folder of internal storage (e.g. `Download/stubgame/`). A on Game
   folder: « Read the game in place? ». A on « Allow all files access », turn it on, come back: the folder picker
   opens. Pick the folder. Expected: Home, title of the stub (or the folder's name), chip « Ready », Play focused, the
   note « … is ready »; logcat `launcher: game set: /storage/emulated/0/Download/stubgame`.
5. A on Play: the stub title starts (`storage: game=/storage/emulated/0/Download/stubgame ... (default.xbe found)`,
   `stub title: all 5 lifted functions OK`), and « The title finished (code 0) » on the black screen.
6. Press Home on the Thor, then the app icon: back to the game at once (logcat `launcher: game running, bringing it
   back`). Back, Quit game twice to quit it, start the app again: Home with Play focused (« Open directly » is off).
7. Settings: L1 / R1 to the Graphics tab, D-pad through the rows, left / right change Vertical sync and Texture
   filtering, the description on the right follows the focus. Then
   `adb shell cat /sdcard/Android/data/fr.adserver.xrecomp.shell/files/recomp.cfg`: the comment line and
   `RECOMP_PRESENT_VSYNC=0` / `RECOMP_TEX_BILINEAR=1` (or what you chose). Play: logcat `recomp.cfg: N option(s)
   from …` lists them and `[FBWIN] … vsync off`. Back in Settings, Y: the lines are gone.
8. Hand-written lines survive: `adb shell "echo RECOMP_WAIT_LOG=1 >> /sdcard/Android/data/fr.adserver.xrecomp.shell/files/recomp.cfg"`,
   change a setting, `cat` again: `RECOMP_WAIT_LOG=1` is still there.
9. Settings › General › « Start in the game » on; quit and start the app: the game starts directly.
   Long-press the icon › « Launcher »: Home. Turn it off again in Settings.
10. Long-press the app icon › « Change game »: Choose your game, the note says the running game will close (if it
    runs). Pick an empty folder: `NOT_A_GAME_FOLDER` in the note, nothing changed; B goes to Home.
11. Folder gone: rename the folder of step 4 with a file manager, start the app. Expected: Home, « Game data
    missing » and the old path on its line (`GAME_FOLDER_GONE`), no crash; A on « Choose the game again » opens
    Choose your game.
12. Copy fallback: Settings › Apps › the app › All files access off. Game folder › « Copy into the app », pick the
    folder of step 4 (renamed back). Expected: the progress ring, then Home with Play focused, Manage showing
    « Imported copy » and the folder `/storage/emulated/0/Android/data/<app id>/files/game`.
13. ISO: make an XISO of our own test data (e.g. `extract-xiso -c stubgame stub.iso`, or any image dumped from your own
    disc), push it to `Download/`. Disc image (ISO), pick it. Expected: the ring with the percentage, « X of Y », file
    names, the screen stays on, then Home. Do it once more and press B (or Cancel) halfway: « Import canceled » in the
    note on Choose your game, and the previous game still plays. Re-import your own disc's image too (it worked
    before this change).
14. Pick a file that is not an ISO (e.g. a photo): `NOT_AN_ISO` in the note.
15. Manage › Delete the imported copy: the first A shows « Press A again », the second deletes; then Welcome (or
    Home without a game) and « The imported copy was deleted ». Saves: `adb shell run-as fr.adserver.xrecomp.shell ls files/save`
    still lists them.
16. `NO_GAME_DATA` (after step 15, so that no game is remembered): `adb shell mkdir /sdcard/Download/empty`, then
    `adb shell "echo RECOMP_GAME_DIR=/sdcard/Download/empty >> /sdcard/Android/data/fr.adserver.xrecomp.shell/files/recomp.cfg"`.
    Start the app: Home; Manage says « Set in recomp.cfg ». Play: the native side finds no `default.xbe` and hands over;
    the launcher opens on Choose your game with `NO_GAME_DATA` and the folder in the note. Remove the line afterwards.
17. WRONG_GAME needs an APK built with `-PxbaXbeSha256=` of another XBE (ask for one, or build one): picking the stub
    folder then shows `WRONG_GAME`, and the previous choice still plays.
18. Touch: tap the choices, the buttons and rows, a setting's arrows, the Settings tabs, the hints (B Back). Everything works without the gamepad,
    and the next D-pad press shows the focus again.
19. Switch the Thor to French: every launcher text is in French (screens, settings, notes, shortcuts).
20. Send screenshots of Welcome, Choose your game, Read the game in place?, the import ring, Home, Settings, Manage
    and About, and the logcat lines (`launcher:`, `import:`, `recomp.cfg:`, `storage: game=...`). Say if the A /
    B buttons are swapped on the Thor (its « Xbox » / « Nintendo » layout setting changes the key codes the app sees).

## Game library

One APK, several games. **Library folders** (several since XBA-178, say one per console: `/sdcard/xbox`,
`/sdcard/gba`, `/sdcard/ps1`, `/sdcard/n64`) hold disc images (`*.iso`, `*.xiso`, `*.cso`, `*.zso`, `*.cci`, split
sets such as `Game.1.iso` + `Game.2.iso`) and extracted game folders (a folder with a `default.xbe`), and the games
of the other consoles (see [Other consoles](#other-consoles-xba-178)); the launcher lists them all in one grid, and
the player imports the Xbox ones he plays. An image is read in place
([Disc images read in place](#disc-images-read-in-place-xba-126): only its XBEs are copied, into
`files/discs/<TID>/`), or extracted into `<external files>/games/<TID>/` with `RECOMP_DISC_IMAGE=0`; a game folder
read as files is played in place (no copy). Prepare packs, Prepare status, the title image, the game theme,
the language and the saves were already per title ID; the few caches that are not (see below) change hands when the
launched game changes.

### Turning it on

Choose your game › **Game library folder** (third card), or later Library › Options › **Library folders** ›
**Add a folder**. With All files access a folder is read as files (the tree URI resolved to a path, `java.io.File`,
images opened with `ParcelFileDescriptor`); without it, FOLDER_ACCESS offers **Allow all files access** or **Use the
file picker** (Storage Access Framework: images are imported from their document, folders must be copied, chip
« Needs copying »). Each folder's read permission on its tree is kept (`takePersistableUriPermission`; released when
the folder is taken out of the library). Each folder is read its own way: some by path, others through SAF.

Preferences (`game`): `library_folders`, a JSON array in the order the folders were added, `[{"tree":
"content://…/tree/primary%3Axbox", "path": "/storage/emulated/0/xbox"}, {"tree": "…%3Agba"}]` (`path` absent: SAF),
and `library_scanned_at`. The tree URI is the folder's identity. **Migration:** the one folder of an older version
(`library_tree`, `library_path`) becomes the first entry the first time the folders are read (`Library.folders`),
written once; the old keys are removed. Picking a folder that is already in the list updates its entry in place (say,
All files access granted since).

The library is **on** when a library folder is set, or the launcher knows two games or more. Otherwise everything is
as before (Welcome, Home of the one game, Change game): the one-game launcher and its tests are unchanged.

### Scan

`LibraryScanner` on a worker walks every library folder in turn into one index; per folder: at most four levels,
5,000 entries (a full folder does not hide the next one's games); names starting with « . » and `Android/` skipped; a
folder holding `default.xbe` is a game folder (not descended). Each item records its folder's tree URI (`folder`; an
item of an older index has none and belongs to the first folder); a file seen through two overlapping folders is
listed once. A folder that cannot be read leaves the others' items in, and keeps the art and themes of the games it
no longer lists (it may only be unmounted). A split set is one item (its first part; the parts'
locations in the index's `parts`, their sizes summed); a later part alone is not listed. Each new or changed image
gives its `default.xbe` only (`XisoNative.extractFileParts` over its parts, at most 32 MB) into `files/library/xbe/<TID>-<sha8>/` (art, theme, hash, and the Prepare's
source before an import); an unchanged one (location, size, modification time) is not opened again. The title image
and the game theme are cached there and then (`files/art/<TID>.png`, `<TID>.theme`). Files that are not games (not
an Xbox image, no or a bad `default.xbe`, a game file of another console without a title key, unreadable) make the
dimmed « Other files (n) » line under the grid. They are not reused forever (XBA-192): an unreadable one is opened
again by every scan, the others by the first scan of a new identification (the index's `"identify"`:
`LibraryScanner.IDENTIFY_REV` and the APK's install / update time), so after an update a file an older build could
not read (a `.chd` before the native CHD reader) becomes a game without being touched. The index is
`files/library/index.json` (rebuildable). Rescans: each
time the library opens (when some folder is read by path), Y, a new folder; with every folder through SAF only at a
cold start and on Y. logcat: `launcher: library: scan of F folders: N items, M games, T ms (…)`.

### Library view (XBA-206)

What the library shows and in which order, `LibraryView` (pure; the launcher hands it the entries):

| | |
|---|---|
| **Sort** pill | Name → Last played → Console (Xbox, GBA, PS1, N64, by name in each) → Most played, A for the next; the focus stays on the pill. Names compare without accents (« Écran » with the E's). Never played: last, by name. |
| Console tabs | In the top bar with games of several consoles: **All**, then the consoles present; L1 / R1 (or a tap). An Xbox-only library has none. A console saved while its games were there and gone now shows All. |
| **Search** pill | A on it: a text field in its place with the on-screen keyboard (not full screen: `IME_FLAG_NO_EXTRACT_UI`), the grid follows each letter; every word typed must be in the title, any case, accents not needed (« pokemon » finds « Pokémon »). The keyboard's Search key goes to the first game found. B clears the search (before leaving the library); the pill shows « *query* » while one is typed. The library's poll does not rebuild the screen while the field has the focus. |
| Favourites | X on a card (or on a game's Home: **Add to favourites** / **Remove from favourites**): first in the grid in every sort, a star in the art's top right corner; note « *Game* is a favourite: first in the library ». The focus follows the card. |
| **Recently played** | A row of the last five games played, any console, above **All games**; not while a console or a search narrows the grid, nor in a library of one row (five games or fewer). A on a card there opens the game as in the grid. |

The sort, the console and the favourites are this device's: `files/library_view.properties` (`sort`, `console`,
`favourites`); the search is not kept. The hints are A Open, X Favourite, Y Options (Start still does the primary
action). logcat: `launcher: library: sorted by …`, `console …`, `<key> is a favourite`, `search opened`, `search
cleared`. Tests: `LibraryViewTest` (the sorts, the filter, the search, favourites first, the file), `PlayClockTest`,
`LauncherLibraryViewTest` (X on a card and on Home, the Sort pill, L1 / R1, the search, « Recently played », the
session closed with the game's count; `library-view-*.png`).

### Device check (library view, XBA-206), for Alex

A library with Xbox and GBA (or PS1 / N64) games, more than five in all, a few of them played before.

1. Open the library: the **Search** and **Sort: Name** pills over the grid, « Recently played » with the games last
   played (any console, most recent first), then « All games »; the hints A Open, X Favourite, Y Options.
2. Focus a game, press **X**: it moves first with a star in its top right corner, the focus with it, a note says so.
   X again: back in its place, no star. Close and reopen the app: the favourites are still first.
3. Open a game's Home (an Xbox one, then a GBA one): the hint X « Add to favourites »; press it, the hint becomes
   « Remove from favourites »; B: the game is first in the library with its star.
4. Press A on **Sort** several times: Last played, Console, Most played, Name; the grid reorders, the focus stays on the
   pill. Close and reopen the app: the same sort.
5. **L1 / R1**: the tabs All, Xbox, GBA, … in the top bar filter the grid (the « Recently played » row goes while a
   console is chosen). Close and reopen the app: the same console.
6. A on **Search**: a field with the keyboard, the grid still visible; type a few letters: the grid narrows at each
   letter (try a name without its accent). The keyboard's Search key: the focus goes to the first game. A word that
   matches nothing: « No game matches … ». **B**: the search is cleared, the whole library is back (the app stays).
7. Play a GBA game 2-3 minutes, Back to launcher (the game waits) for a few minutes, then go back and Quit: its Home
   says « Just now · 3 min played » (the minutes in the background are not counted); it is first in « Recently
   played ». The same with an Xbox game.
8. In a game, open the menu › **Settings**: the row « Picture shader »; choose CRT: the CRT shader is on at once
   (logcat `picture applied live`), and still on after the game starts again; « As in Settings » puts Settings'
   shader back.
9. French: every new label (Rechercher, Tri : Nom, Joués récemment, Tous les jeux, Favori, Tous) is in French.

### Other consoles (XBA-178)

The scan also lists the games of the consoles XRecomp is growing into (`Console`, XBA-147), identified from their
content the way the native side names their packs (`Console.identify`, mirroring `recomp_pack_rom_identify`):

| Console | Files | Title key |
|---|---|---|
| Game Boy Advance | `*.gba` | game code + CRC32 of the file, `AXVE-1A2B3C4D` |
| PlayStation | a `.cue` and its tracks (the `.cue` stands for the disc; the tracks it names are not listed again), a lone `.bin`, a `.chd`, an `.iso` that is no Xbox image (`ENOTXISO`) but an ISO 9660 « PLAYSTATION » volume | the serial of `SYSTEM.CNF`'s BOOT line, `SLUS-00594` |
| Nintendo 64 | `*.z64`, `*.v64`, `*.n64` (any byte order) | game code + header CRC1, `NSME-635A2BFF` |

Such a game is keyed `<console>-<title key>` (`gba-AXVE-1A2B3C4D`, the pack folder's name, `Console.packDirName`) in
the index's `tid` and in `games.json`; Xbox games keep their title ID, so every older `games.json` / `index.json`
record stays valid. Index items of another console carry `"kind": "rom"`, `"console"`, `"key"` (absent for Xbox:
an Xbox item is written as before). A `.cue`'s fingerprint is its own and its tracks' (sizes summed, the newest
time), so the stat-only rescan covers a changed track. A file named like a game whose content gives no title key is
an « other file ». A `.chd` (« MComprHD »: compressed hunks, zlib / LZMA / zstd / FLAC) is read by native code:
`Console.chdReader` calls `Ps1Native.titleKey` in `libxba_import.so` (the launcher's process already loads it for
`XisoNative`; no SDL), which runs `recomp_pack_rom_identify` with the PS1 disc reader (`ps1_disc.c` + libchdr) as its
disc hook, on the file's path or the document's `fd:N`, so the key is the one the game process computes (XBA-159).
Without the library (a JVM test, an APK built without it) a `.chd` stays an « other file ». A lone `.bin` that is no
PlayStation disc (firmware, a dump) is ignored: kept in the index for the stat-only rescan, shown nowhere.

GBA games play: see [Game Boy Advance games](#game-boy-advance-games-xba-157); PS1 games too:
[PlayStation games](#playstation-games-xba-168); and N64 games: [Nintendo 64 games](#nintendo-64-games-xba-174). A
console whose runtime does not exist yet would show its games in the grid with the chip **Not playable yet** (a neutral
grey dot): no Prepare, no Import, no Home, A or Start on the card says so in the note line (« … games are listed but
not playable yet… ») and selects nothing; since XBA-174 every listed console plays. When the library holds games of more than one console, every card has a small badge in its art's top left
corner (**Xbox**, **GBA**, **PS1**, **N64**); an Xbox-only library looks as before. There is no console filter.

### Game Boy Advance games (XBA-157)

The GBA runtime (`consoles/gba/runtime`, an interpreter: `gba_core.h` on the host layer) plays a ROM as it is: no
Import, and no pack needed. With the on-device toolchain, a Prepare can translate the game for the device (a title
pack, XBA-158: [prepare.md « GBA packs »](prepare.md#gba-packs-xba-157-xba-158)) so that it runs natively; Play never
waits for it. A GBA card is **Ready** (no chip) while its file is in a library folder, **Preparing** / **Queued** while
its Prepare runs or waits, **Running** while it runs, **No file access** when its folder cannot be read (A then asks
for the folder again).

- **Home (XBA-158, with the toolchain).** A on the card opens the game's own Home (`Screen.ROM_HOME`, the launcher's
  `romEntry`; never the selected game, which stays the Xbox's): the title, one line, **Play** first and the Prepare
  action beside it. The line: « Not prepared · plays in the interpreter » (Prepare), « Prepared · runs natively »,
  « Prepared · N new addresses found · prepare again » (Prepare again, when the runs found code since the pack:
  `Pack.newlyDiscovered`), the Prepare's progress (« Compile · 42 % · … »; Preparing… opens the Preparing screen),
  « Queued » (A takes it out of the queue) or « Prepare failed · CODE » (See what failed: the Preparing screen with
  Retry and Share log). A note under it says what Play and Prepare do. B goes back to the library. Prepare is the Xbox
  flow: the space check, the notification permission, `PrepareService` with the ROM (a path, or the document's URI,
  which the service copies to `<files>/work/gba-<KEY>/rom.gba`) and the library key `gba-<KEY>` as title ID, so
  `status.json`, `prepare.log`, the queue, the notification and the Preparing screen are the Xbox ones (stock colours,
  the title's monogram). When it ends, the game's Home again, « … is prepared: it now runs natively ». Without the
  toolchain there is no Home: A launches, as Start does.
- **Launch.** Start on the card, A without the toolchain, Play on its Home (or the bottom screen's Play) start the game
  activity at once (`LauncherActivity.launchRom`): the ROM becomes the launched game (`GameStore.rememberRomLaunch`: preference `rom`,
  the library key `gba-<KEY>` as `title_id`; the Xbox game's `game_dir` stays, so the launcher's one-game rules are
  unchanged and any Xbox Play clears `rom`). The same ROM running is brought back; another game running is ended
  first (one game process at a time). The launcher finishes, as for Play.
- **The ROM reaches the runtime** as `RECOMP_GAME_DIR`: its path for a folder read as files, or, for a folder seen
  through the Storage Access Framework, its document URI (`<tree>/document/<id>`), which `ShellActivity` opens and
  hands over as `fd:N` (detached; the runtime reads it from offset 0; unreachable: `fd:-1`). `shell_main.c` sees a
  game of another console (`xba_pack_rom_console`, which reads `fd:N` too), turns off the Xbox disc mount for that
  process (`RECOMP_DISC_IMAGE=0`: the mount would close the descriptor) and calls `xba_pack_load_rom`; without a pack
  that is `PACK_NONE gba <KEY> (not prepared, the interpreter runs it)` and `gba_runtime_start(rom, NULL, key)`.
  With a pack: `PACK_LOADED gba <KEY> (<N> functions)`, or `PACK_STALE gba <KEY> (<why>), running interpreted` when
  it is not this ROM's, this shell's or does not load (the game always starts). At exit the addresses the interpreter
  had to run go to `<files>/work/gba-<KEY>/discovered.txt` for the next Prepare (the discovery loop).
- **Picture.** 240 x 160 RGB555 converted to RGBA and presented through `host_window` (GL ES 3.0), letterboxed at
  the largest size of the GBA's aspect, sharp pixels (nearest). `RECOMP_GBA_SCALE=integer` (recomp.cfg) keeps every
  pixel the same size (6x on the Thor's 1080p screen: 1440 x 960), `RECOMP_GBA_FILTER=linear` smooths.
- **Controls.** Every connected pad drives the GBA (the Thor's own controls and an external pad alike), Xbox layout:
  A → A, B → B (and Y → A, X → B, a second pair under the thumb), LB / RB → L / R (the triggers too), Start → Start,
  Back / Select → Select, D-pad and left stick → D-pad. Back on the device opens the in-game menu, as for an Xbox
  game. These are the defaults: Home › Controls remaps them, and the on-screen pad has a GBA layout ([Controls for
  the other consoles](#controls-for-the-other-consoles)).
- **Sound.** 48 kHz stereo through `host_audio`, pulled from `gba_audio_pull` each frame (silence until the APU
  lands behind the same call).
- **Pacing.** The GBA's own 59.73 Hz with absolute deadlines (no drift), nudged ±2 % by the audio queue once there is
  sound; vsync on; a stall of more than three frames resets the clock instead of running fast to catch up.
- **Saves.** `<internal files>/save/gba-<KEY>/game.sav` (the title key: game code + CRC32 of the ROM, so two dumps
  of one game keep separate saves). Read before the first frame; written a second after the game last wrote it
  (`gba_save_take_dirty`), at once when the app pauses (Home, the in-game menu: from the thread that pauses, since
  Android parks the game thread in the background), and at exit (also from the menu's Quit, through the exit hook).
  Written through `game.sav.tmp` + rename, and only when it changed since it was read or last written.
- **Lifecycle.** Home pauses the game (no frame runs, audio paused) and resumes it where it was; the in-game menu
  holds it the same way; its Quit ends the process (« clean exit »). A ROM that cannot be read (moved, permission
  gone) sends the player back to the launcher with « The game ended: the game file cannot be read ».
- **Options** (recomp.cfg): `RECOMP_GBA_BIOS=/path/to/bios.bin` uses a 16 KB BIOS image of your own instead of the
  built-in one (never shipped with the app); `RECOMP_GBA_SCALE`, `RECOMP_GBA_FILTER` as above; `RECOMP_GBA_FRAMES=N`
  stops after N frames (checks).

On a desktop the same runtime plays a ROM with `gba_play` (consoles/README.md), which is where a game is checked
before the device.

### Device check (GBA games, XBA-157), for Alex

On the AYN Thor, `adb logcat -s xboxrecomp SDL` throughout. The homebrew ROMs come from
`tools/consoles/build_homebrew.sh gba out/hb` (Docker; `out/hb/gba/pad.gba`, `gfx.gba`, `hello.gba`...). Copy them to
the device's own GBA folder: `adb shell mkdir -p /sdcard/gba && adb push out/hb/gba/pad.gba out/hb/gba/gfx.gba
out/hb/gba/hello.gba /sdcard/gba/` (or over USB file transfer into `Internal storage/gba`).

1. Install the APK. If `/sdcard/gba` is not a library folder yet: Y › Library folders › Add a folder › `gba`.
   Expected: the GBA cards (« pad », « gfx », « hello »: the file names) with the **GBA** badge when Xbox games are
   listed too, and **no** chip (Ready). PS1 cards play too (XBA-168), and N64 cards (XBA-174).
2. A on the **pad** card. Expected: the game starts at once, fullscreen, landscape, no Home screen; « XBA PAD + SAVE »
   at the top, the frame counter `F…` moving. logcat: `launcher: library: play gba-XBAT-… from /storage/…/pad.gba`,
   `PACK_NONE gba XBAT-…`, `gba: window on (aspect, nearest), audio on, pads 1` (or more with an external pad).
   Screenshot.
3. Press each control and check its line shows `> NAME` while held: A → A, B → B, Y → A, X → B, LB → L, RB → R,
   LT → L, RT → R, Start → START, Select → SELECT, D-pad → UP / DOWN / LEFT / RIGHT; the left stick moves the `*`
   like the D-pad. Note any control that does nothing or the wrong thing.
4. Speed: the counter climbs about 60 per second (time 10 s with a stopwatch: about 597). logcat at quit gives the
   exact rate (`gba: N frames in S s (59.7x fps)`).
5. Press A five times: the count reads 5. Home, wait 5 s, reopen XRecomp from recents: the game is where it was, the
   counter did not move while away. logcat: `gba: paused at frame …`, `gba: save written (32768 bytes, pause) …`,
   `gba: resumed`.
6. Back: the in-game menu opens (the game holds); Quit game, confirm. logcat: `clean exit`. A on the pad card again:
   the count reads 5 (`gba: save loaded (32768 bytes) from /data/user/0/…/files/save/gba-XBAT-…/game.sav`).
7. While the pad game runs, open the launcher (Home, then the XRecomp icon if it shows the launcher) and start an Xbox
   game: the GBA game ends, the Xbox game starts as before. Then A on the pad card: the Xbox game ends, the GBA one
   starts.
8. A on the **gfx** card: a colour gradient with a white and a black rectangle and a magenta diagonal after a moment
   (a tiles-and-sprites scene first). Screenshot. Try `RECOMP_GBA_SCALE=integer` in recomp.cfg: smaller, black
   borders all round, crisp pixels.
9. A library folder added with the file picker (no All files access): the pad game starts the same (logcat
   `shell: ROM document content://… -> fd:N`).
10. One of your own cartridge dumps (`.gba`, kept outside every repository): copy it to `/sdcard/gba`, Y (rescan), A
    on its card. Note: does it boot, is the picture right, do the controls work, does it hold 60 fps (logcat at
    quit), and, after saving in the game, quitting and starting again, is the save there (`save loaded`). Sound
    stays silent until the APU work lands; some games may still crash or show wrong graphics (the core is new):
    send the logcat and a screenshot.

With an APK that carries the toolchain, A on a GBA card opens its Home (step 2 is then A on the card, A on Play;
Start on the card still starts it at once).

### Device check (GBA Prepare, XBA-158), for Alex

An APK with the toolchain, the GBA homebrew in `/sdcard/gba` as above, `adb logcat -s xboxrecomp SDL` throughout.
Until the GBA lifter lands (`gba_tools.recomp`), step 3 ends with `LIFT_FAILED`: note it and stop after step 4.

1. A on the **pad** card. Expected: its Home, « Not prepared · plays in the interpreter », **Play** focused,
   **Prepare** beside it, a note on what each does. Screenshot. B: back to the library.
2. A on Play: the game starts at once (logcat `PACK_NONE gba XBAT-… (not prepared, …)`). Quit from the in-game menu.
   logcat: `gba: N address(es) the pack did not cover, N new, in /data/user/0/…/files/work/gba-XBAT-…/discovered.txt`
   (N may be 0 until the core records them).
3. A on the card, right, A on **Prepare**: the Preparing screen (« Install tools » the first time, then Identify,
   Analyse, Translate, Compile, Pack), the notification. Note the total time (a homebrew: well under a minute after
   the tools are installed). Expected: back on the game's Home, « Pocket… is prepared », « Prepared · runs natively ».
   Screenshot. `adb shell run-as fr.adserver.xrecomp.shell ls -l files/packs/gba-XBAT-*` shows `libtitle.so` and
   `pack.json`.
4. Play: logcat `PACK_LOADED gba XBAT-… (<N> functions)`; the game plays as in the interpreter (same picture, same
   controls, the counter and the save still work). Quit.
5. Play while a Prepare of the same game runs (start Prepare, press Y, then Play): the game starts at once in the
   interpreter (or with the previous pack); the Prepare goes on. The card shows « Preparing » in the library meanwhile.
6. After a run that found new code (step 2's line with « new »), the game's Home says « Prepared · N new addresses
   found · prepare again » with **Prepare again** beside Play; it runs and the line goes back to « Prepared ».
7. A stale pack: `adb shell run-as fr.adserver.xrecomp.shell sh -c 'echo x >> files/packs/gba-XBAT-*/libtitle.so'`,
   Play. Expected: the game still starts, logcat `PACK_STALE gba XBAT-… (<reason: lib_size>),
   running interpreted`; Home says « Not prepared »; Prepare repairs it.
8. A library folder added with the file picker (no All files access): Prepare works the same (logcat
   `rom: content://… copied to …/work/gba-XBAT-…/rom.gba`).
9. Switch the Thor to French: the game's Home lines and notes in French.

### PlayStation games (XBA-168)

The PS1 runtime (`consoles/ps1/runtime`, an interpreter: `ps1_core.h` on the host layer, consoles/README.md
« Playing a PS1 game ») plays a disc as it is, the GBA's way: no Import, no pack needed. With the on-device
toolchain a Prepare translates it for the device (a title pack: [prepare.md « PS1 packs »](prepare.md#ps1-packs-xba-168))
so that it runs natively; Play never waits for it. A PS1 card is **Ready** (no chip) while its disc is in a library
folder, **Preparing** / **Queued** while its Prepare runs or waits, **Running** while it runs, **No file access** when
its folder cannot be read.

- **Home (XBA-168, with the toolchain).** A on the card opens the game's Home, the GBA's (`Screen.ROM_HOME`): the
  title, one line, **Play** first and the Prepare action beside it: « Not prepared · plays in the interpreter »
  (Prepare), « Prepared · runs natively », « Prepared · N new addresses found · prepare again » (the runtime's
  discovered records beyond the pack's `discovered`: overlays the pack did not have), the Prepare's progress,
  « Queued », « Prepare failed · CODE ». Prepare starts `PrepareService` with the disc (the `.cue` or image path, or
  its document URI) and the library key `ps1-<SERIAL>`; status, log, queue, notification and Preparing screen are the
  Xbox ones. A document disc is not copied: the service copies the sectors that lead to the boot EXE into
  `<files>/work/ps1-<KEY>/disc.iso` (`Ps1Disc`, a sparse image of a few hundred KB; a `.cue`'s data track found among
  the track documents the library index lists, `PrepareService.cueParts`). A `.chd` (a path or a document) is read
  by the native disc reader (`Ps1Native.openDisc`: `ps1_disc.c` + libchdr in `libxba_import.so`, every codec) and its
  boot EXE's sectors go to the same `disc.iso` (XBA-168; [prepare.md « PS1 packs »](prepare.md#ps1-packs-xba-168)).
  Without the toolchain, A launches at once; Start on the card always does.
- **Launch** (`LauncherActivity.launchRom`, as for the GBA): the disc becomes the launched game (`rom`, the library key
  `ps1-<SERIAL>` as `title_id`); the same disc running is brought back, another game ending first.
- **The disc reaches the runtime** as `RECOMP_GAME_DIR`. A folder read as files: the `.cue`'s path (the runtime opens
  its tracks beside it), or the `.bin` / `.iso`'s. A folder seen through the Storage Access Framework: a single-file
  image (`.bin`, `.iso`) goes over as `fd:N` like a GBA ROM; a `.cue` cannot (its tracks have no paths), so the
  launcher hands the game activity the track documents the scan found (`GameStore` `rom_parts`,
  `LauncherActivity.romParts`), and `ShellActivity` opens the `.cue` and each track (detached descriptors, kept for
  the process's life) and writes `<cache>/ps1/disc.cue` whose FILE lines name each track as `/proc/self/fd/N`
  (`Ps1Cue`): no copy of the disc, and the runtime and the title key reader open those paths like files. logcat
  `shell: cue sheet content://… (N track file(s)) -> /data/user/0/…/cache/ps1/disc.cue`; a track missing or
  unreadable: `fd:-1` and back to the launcher. `shell_main.c` routes it like a GBA ROM (`xba_pack_rom_console`,
  `RECOMP_DISC_IMAGE=0`), `pack_loader.c` logs `PACK_NONE ps1 <SERIAL> (not prepared, the interpreter runs it)`
  and calls `ps1_runtime_start`; with a pack: `PACK_LOADED ps1 <SERIAL> (<N> functions)`, or `PACK_STALE ps1 <SERIAL>
  (<why>), running interpreted` when it is not this disc's (the SHA-256 of its boot EXE), this shell's or does not
  load (the game always starts). At exit (and Quit) the code the interpreter had to run goes to
  `<files>/work/ps1-<KEY>/discovered.txt` for the next Prepare (address, hash and bytes: overlays get lifted). A `.chd` goes over as `fd:N`
  too; `shell_main.c` installs the PS1 disc reader as `recomp_pack_rom_identify`'s disc hook (`ps1_identify_install`)
  before routing, so it is keyed by its serial like a `.bin` (a pack prepared from it or from its `.cue` is taken).
- **Change disc (XBA-166).** For a multi-disc game the in-game menu has **Change disc** (after Screenshot), shown when
  at least two discs of the game are found beside the one playing (`Ps1Discs`, from `ShellActivity`: a path's
  folder listing, or a document's folder listed through its library tree, else the library index's items of that
  folder): a `.m3u` playlist there that names the disc playing gives its discs in its order (only the file names of
  its lines count); otherwise the disc images (`.cue`, `.chd`, `.iso`, `.bin`, `.img`) whose names differ only by
  their disc marker (« (Disc 1) », « Disc 2 », « [Disc 3] », « CD1 », « (CD 2) », « Disk 1 », « (Disc 1 of 2) »),
  one per disc number (a `.cue`'s tracks are not discs), in disc order. Its page lists them, « In the drive » under
  the one playing; choosing another closes the menu: `MenuNative.nativePs1ChangeDisc` hands it to
  `ps1_runtime_change_disc` (a path as it is; a document as `fd:N`; a `.cue` document as a `.cue` of our own naming
  its track descriptors, `Ps1Cue`, in `<cache>/ps1/disc<N>/`), the lid opens (the game sees it: INT5, the shell bit),
  a second later the new disc is in and the lid closes; a flash « Disc N: the lid opens, the disc goes in ». logcat
  `shell: menu: 2 discs: …`, `shell: menu: change disc to Game (Disc 2).chd (…)`, `ps1: lid opened at frame N
  (changing the disc to …)`, `ps1: lid closed at frame N+60, disc … in`. A disc that cannot be opened: « The disc
  cannot be opened », the menu stays. The game is still keyed by the disc it started from (memory cards, pack).
- **BIOS.** OpenBIOS (MIT, PCSX-Redux's nugget tree), inside the APK as `assets/ps1-bios/openbios.bin`: the `ps1-bios`
  stage of `tools/docker/android.Dockerfile` builds it from its pinned source, sha256 checked, and gradle takes it with
  `-PxbaPs1Bios=FILE` (the same check); its licence is `LICENSES/MIT-OpenBIOS.txt` (About › Licences, NOTICE « The
  Android app »). The runtime reads the asset through SDL. Never a Sony BIOS. `RECOMP_PS1_BIOS=/path` (recomp.cfg)
  uses another 512 KB image, `RECOMP_PS1_BIOS=hle` the HLE BIOS (no image, `consoles/ps1/HLE_BIOS.md`, XBA-163). An APK
  built without the asset runs PS1 games on the HLE BIOS (logcat `ps1: … BIOS HLE (no image found)`).
- **Picture.** The GPU's display area (256..640 x 240 / 480, 15- or 24-bit) through `host_window` at 4:3, letterboxed
  (1440 x 1080 on the Thor's 1080p screen), smoothed (linear); `RECOMP_PS1_FILTER=nearest` keeps sharp pixels,
  `RECOMP_PS1_SCALE=integer` whole multiples of the height (4x a 240-line picture: 1280 x 960).
- **Controls.** Every connected pad drives port 1, Xbox layout: A → Cross, B → Circle, X → Square, Y → Triangle,
  LB / RB → L1 / R1, LT / RT → L2 / R2, Start → Start, Back / Select → Select, stick clicks → L3 / R3, D-pad →
  D-pad. A DualShock (XBA-167), plugged in in digital mode by default (the left stick drives the D-pad too);
  `RECOMP_PS1_PAD=analog` plugs it in in analog mode (both sticks). A game may switch it itself through the pad's
  config mode (logcat `ps1: the game set port 1 analog (DualShock) at frame N`) and lock it; Back + L3 + R3 together
  are its ANALOG button (logcat `ps1: port 1 is now analog (DualShock)`, or `stays … (locked by the game)`). Its
  rumble motors, when the game maps them, drive the pad's rumble (`host_pad_rumble`: large motor → low frequency,
  small → high); `RECOMP_PS1_PLAYERS=2`: the first two pads drive ports 1 and 2. Back on the device opens the
  in-game menu. These are the defaults: Home › Controls remaps them, and the on-screen pad has a PS1 layout
  ([Controls for the other consoles](#controls-for-the-other-consoles)).
- **Sound.** 44.1 kHz stereo from the SPU (`ps1_audio_pull`, CD audio and XA mixed in) through `host_audio`.
- **Pacing.** The PS1's own field rate (NTSC about 60 Hz, PAL 50 Hz, from the cycles each frame ran), absolute
  deadlines nudged ±2 % by the audio queue; vsync on; a stall of more than three frames resets the clock.
- **Memory cards.** `<internal files>/save/ps1-<KEY>/card1.mcr` (and `card2.mcr` once a game writes slot 2): 128 KB
  raw images, the `.mcr` other emulators read and write, so a card can be copied in or out with `adb`. Read before the
  first frame (none: a blank formatted card); written a second after the game's last write to it (at most five
  seconds after its first), at once when the app pauses (Home, the in-game menu) and at exit (the menu's Quit too),
  through `.tmp` + rename, only when it changed.
- **Lifecycle.** As for the GBA: Home pauses (no frame runs, audio paused) and resumes; the in-game menu holds it;
  Quit ends the process. A disc that cannot be read sends the player back to the launcher with the reason above.
- **Options** (recomp.cfg): `RECOMP_PS1_BIOS`, `RECOMP_PS1_SCALE`, `RECOMP_PS1_FILTER`, `RECOMP_PS1_PAD`,
  `RECOMP_PS1_PLAYERS` as above; `RECOMP_PS1_FRAMES=N` stops after N frames (checks).

On a desktop the same runtime plays a disc with `ps1_play` (consoles/README.md), where a game is checked first.

### Device check (PS1 Prepare, XBA-168), for Alex

An APK with the toolchain, our homebrew discs in `/sdcard/ps1` as above plus the overlay disc (`adb push
out/hb/ps1/overlay.cue out/hb/ps1/overlay.bin /sdcard/ps1/`), `adb logcat -s xboxrecomp SDL` throughout.

1. A on the **overlay** card. Expected: its Home, « Not prepared · plays in the interpreter », **Play** focused,
   **Prepare** beside it, a note on what each does. Screenshot.
2. Right, A on **Prepare**: the Preparing screen (« Install tools » the first time, then Identify, Analyse, Translate,
   Compile, Pack) and the notification. Note the total time (the homebrew: about a minute after the tools are
   installed). Expected: back on the Home, « overlay is prepared », « Prepared · runs natively ». `adb shell run-as
   fr.adserver.xrecomp.shell ls -l files/packs/ps1-PSX-4009E1ED` shows `libtitle.so` and `pack.json`.
3. Play: logcat `PACK_LOADED ps1 PSX-4009E1ED (69 functions)` (about that many); after the BIOS screen the program
   runs as before (its result screen says PASS, as in the interpreter). Quit from the in-game menu after 30 s. logcat
   `ps1: code at N address(es) the pack did not cover, M new record(s), in /data/user/0/…/files/work/ps1-PSX-4009E1ED/discovered.txt`
   (the two overlays it loads from the disc; about 14 addresses, 21 records).
4. A on the card: « Prepared · N new addresses found · prepare again », **Prepare again** beside Play. Screenshot.
   Prepare again, then Play: `PACK_LOADED ps1 PSX-4009E1ED (86 functions)` (more than in step 3), PASS, and at Quit no
   « did not cover » line (or no new record). The Home says « Prepared » again.
5. gfx and hello: Prepare each, Play: the same pictures as interpreted (step 2 / 5 of « PS1 games »), `PACK_LOADED`;
   note the frame rate at Quit next to the interpreted one.
6. A stale pack: `adb shell run-as fr.adserver.xrecomp.shell sh -c 'echo x >> files/packs/ps1-PSX-4009E1ED/libtitle.so'`,
   Play. Expected: the game still starts, logcat `PACK_STALE ps1 PSX-4009E1ED (…), running interpreted`; the Home
   says « Not prepared »; Prepare repairs it.
7. A library folder added with the file picker (no All files access), holding overlay.cue + overlay.bin: Prepare
   works the same; `prepare.log` (Share log on the Preparing screen, or `run-as … cat files/work/ps1-PSX-4009E1ED/prepare.log`)
   has `disc: content://…overlay.bin -> …/work/ps1-PSX-4009E1ED/disc.iso (boot \OVERLAY.EXE;1, 16384 bytes, … sectors
   copied)`, and `run-as … du -k files/work/ps1-PSX-4009E1ED/disc.iso` is small (tens of KB).
8. One of your own disc dumps (`.cue` + `.bin` tracks): Prepare (note the time and the pack size in `pack.json`),
   Play (`PACK_LOADED`, picture, sound, speed vs. interpreted), quit after a few minutes of play, Prepare again when
   the Home offers it, play again. Send the logcat and `prepare.log` if a step fails.
9. A `.chd` of that disc (`chdman createcd -i Game.cue -o Game.chd`, kept outside every repository) in the library
   folder: its Home now has **Prepare** beside Play (XBA-168). Prepare it, once from a folder with All files access
   and once from a folder added with the file picker: `prepare.log` has `disc: … (chd) -> …/disc.iso (boot …)`, the
   pack's key and `source_sha256` in `pack.json` are those of the `.cue`'s pack, and Play logs `PACK_LOADED`.
10. Switch the Thor to French: the Home lines and notes in French.

### Device check (PS1 games, XBA-168), for Alex

On the AYN Thor, `adb logcat -s xboxrecomp SDL` throughout. Our homebrew: `tools/consoles/build_homebrew.sh ps1
out/hb` (Docker; `out/hb/ps1/gfx.cue` + `gfx.bin`, `hello.cue` + `hello.bin`, `timer_irq.*`…; the `.exe` files are
for the desktop). Copy the discs to the device's PS1 folder: `adb shell mkdir -p /sdcard/ps1 && adb push
out/hb/ps1/gfx.cue out/hb/ps1/gfx.bin out/hb/ps1/hello.cue out/hb/ps1/hello.bin /sdcard/ps1/`.

1. Install the APK (CI's xrecomp-latest: it carries OpenBIOS). If `/sdcard/ps1` is not a library folder yet: Y ›
   Library folders › Add a folder › `ps1`. Expected: the cards « gfx » and « hello » with the **PS1** badge (when
   other consoles' games are listed too), **no** chip. About › Licences lists `MIT-OpenBIOS.txt`.
2. A on **gfx** (with the toolchain: its Home, « Not prepared · plays in the interpreter », Play and Prepare; screenshot; A on
   Play). Expected: about 6 s of the BIOS's black 640 x 480 screen (OpenBIOS boots the disc), then a dark grey 4:3
   picture: a red rectangle, a green triangle, a colour gradient square, a magenta / white checkerboard and a small
   textured square, black bars left and right. Screenshot. logcat: `launcher: library: play ps1-PSX-… from
   /storage/…/gfx.cue`, `PACK_NONE ps1 PSX-…`, `ps1: … game gfx.cue (disc, BIOS boot), key PSX-…, BIOS asset
   ps1-bios/openbios.bin`, `ps1: window on (4:3, aspect, linear), audio on, pads 1, port 1 digital`, `ps1: display
   320 x 240 at frame …`.
3. Speed: Back › Quit game after about 30 s. logcat `ps1: N frames in S s (59.9x fps), 0 late resets` (around 60 fps
   with a few late resets at most). Note the rate if lower.
4. `RECOMP_PS1_SCALE=integer` in recomp.cfg, A on gfx again: a smaller picture (1280 x 960) with black borders all
   round. `RECOMP_PS1_FILTER=nearest`: sharp pixels. Remove both.
5. A on **hello**: after the BIOS screen, « XBA HELLO PS1 » and « HOMEBREW CORPUS XBA-149 » in white on dark blue.
   Screenshot. Home, wait 5 s, reopen XRecomp from recents:
   the game is where it was. logcat `ps1: paused at frame …`, `ps1: resumed`.
6. Back: the in-game menu, the game holds; Quit game. logcat `clean exit`. While a PS1 game runs, start an Xbox or a
   GBA game from the launcher: the PS1 one ends, the other starts.
7. A library folder added with the file picker (no All files access), holding gfx.cue + gfx.bin: A on gfx plays the
   same. logcat `shell: cue sheet content://… (1 track file(s)) -> /data/user/0/…/cache/ps1/disc.cue`. A lone
   `gfx.bin` there (rename the .cue away, Y to rescan) plays too (`shell: ROM document … -> fd:N`).
8. One of your own disc dumps (`.cue` + `.bin` tracks, or a single `.bin` / `.iso`; kept outside every repository):
   copy it to `/sdcard/ps1`, Y, A on its card (named after the file; its key is the serial, `ps1-SLUS-…`). Note: does
   the BIOS boot it, is the picture right (and the 4:3 shape), sound, controls (Cross / Circle / Square / Triangle,
   shoulders, triggers, Start / Select, D-pad and left stick), speed (logcat at quit). Try Back + L3 + R3 in a game
   that uses the sticks (logcat `port 1 is now analog`); a game that switches to analog itself shows `the game set
   port 1 analog`, and one with vibration makes the pad rumble. Save to the memory card in the game, quit, start again: the
   save is there (`ps1: memory card 1 loaded from …/save/ps1-SLUS-…/card1.mcr`). `adb shell run-as
   fr.adserver.xrecomp.shell ls -l files/save/ps1-*` shows `card1.mcr`, 131072 bytes. The core is new: send the
   logcat and a screenshot for any game that does not boot, shows wrong graphics or runs slowly.
9. Switch the Thor to French: the PS1 Home's line and note in French.
10. A `.chd` of one of your own discs (`chdman createcd -i Game.cue -o Game.chd` on a computer; kept outside every
    repository) in the library folder, Y: its card is a PS1 game keyed by its serial (`ps1-SLUS-…`, the same key as
    its `.cue`), not an « other file »; once with All files access, once through a folder added with the file picker.
    A on it plays it (logcat `PACK_NONE ps1 SLUS-…`).
11. A game that switches the DualShock to analog itself (one that needs the sticks, started with the default
    digital pad): logcat `ps1: the game set port 1 analog (DualShock)`; if it locks the mode, Back + L3 + R3 logs
    `stays … (locked by the game)`. A game with vibration (in its options): the controller rumbles when the game
    says so, and stops.
12. Change disc (XBA-166), with our homebrew first: copy `gfx.cue` / `gfx.bin` as `Test (Disc 1).cue` / `.bin`
    (edit the `.cue`'s FILE line) and `hello.cue` / `hello.bin` as `Test (Disc 2).cue` / `.bin` into `/sdcard/ps1`,
    Y, play « Test (Disc 1) ». Back: the menu has **Change disc**; its page lists both, « In the drive » under disc 1.
    Choose disc 2: the menu closes, the flash « Disc 2: … », logcat `shell: menu: change disc to Test (Disc 2).cue`,
    `ps1: lid opened at frame …`, `ps1: lid closed …, disc … in`. (The homebrew does not react to the swap; the
    disc is in the drive.) A single-disc game (gfx alone): no Change disc in the menu.
13. A multi-disc game of your own (`Game (Disc 1).chd`, `Game (Disc 2).chd`…; or `.cue` / `.bin`): play to the
    point where it asks for the next disc, Back › Change disc › disc 2. Expected: the game sees the lid open and
    close and goes on from disc 2 (note what it shows); then a `.m3u` in the same folder (`Game.m3u` listing the
    discs, one file name per line): the page lists them in the playlist's order. Repeat once through a folder
    added with the file picker. Send the logcat for any game that does not take the new disc.

### Nintendo 64 games (XBA-174)

The N64 runtime (`consoles/n64/runtime`, an interpreter: `n64_core.h` with the LLE RSP and the software RDP on the
host layer, consoles/README.md « Playing an N64 game ») plays a cartridge dump as it is, the PS1's way: no Import, no
pack needed, no BIOS (the PIF is high-level emulated). With the on-device toolchain a Prepare translates it for the
device (a title pack: [prepare.md « N64 packs »](prepare.md#n64-packs-xba-174)) so that it runs natively; Play never
waits for it. An N64 card is **Ready** (no chip) while its file is in a library folder, **Preparing** / **Queued**
while its Prepare runs or waits, **Running** while it runs, **No file access** when its folder cannot be read.

- **Home.** A on the card opens the game's Home (`Screen.ROM_HOME`), with or without the toolchain: the title, one
  line, **Play** first, then (with the toolchain, XBA-174) the Prepare action, then the pak button: **Controller pak**
  (the default) or **Rumble pak**, what the controller holds for this game; A on it switches to the other one
  (`N64Pak`, a file per game, `files/settings/n64_pak_<KEY>`). The line and the Prepare action are the PS1's: « Not
  prepared · plays in the interpreter » (**Prepare**), « Prepared · runs natively », « Prepared · N new addresses found
  · prepare again » (**Prepare again**: the runtime's discovered records beyond the pack's `discovered`), the
  Prepare's progress, « Queued », « Prepare failed · CODE » (**See what failed**). Prepare starts `PrepareService` with
  the cartridge (its path, read in place in any byte order; a document of a folder seen through the Storage Access
  Framework is copied to `<files>/work/n64-<KEY>/rom.z64` first, 64 MB at most, `PrepareService.romSource`) and the
  library key `n64-<CODE>-<CRC1>`; status, log (Share log), queue, notification and Preparing screen are the Xbox
  ones. Without the toolchain: « Plays in the interpreter », Play and the pak, a note on what each does. Start on the
  card (and the bottom screen's Play) launches at once.
- **Launch** (`LauncherActivity.launchRom`, as for the GBA and the PS1): the cartridge becomes the launched game
  (`rom`, the library key `n64-<CODE>-<CRC1>` as `title_id`); the same game running is brought back, another game
  ending first.
- **The cartridge reaches the runtime** as `RECOMP_GAME_DIR`: its path, or `fd:N` for a document of a folder seen
  through the Storage Access Framework (the GBA's way). `ShellActivity` sets `RECOMP_N64_PAK` to the game's pak choice
  (logcat `shell: n64 pak rumble, game n64-…`), or unsets it when it has none (a `RECOMP_N64_PAK` line of recomp.cfg,
  then the controller pak, decide). `shell_main.c` routes it like a GBA ROM; `pack_loader.c` logs `PACK_NONE n64 <KEY>
  (not prepared, the interpreter runs it)` and calls `n64_runtime_start`; with a pack: `PACK_LOADED n64 <KEY> (<N>
  functions)` (after `n64: ROM identity <16 hex>... (<ms> ms)`, the time the ROM's SHA-256 took), or `PACK_STALE n64
  <KEY> (<why>), running interpreted` when it is not this cartridge's (the SHA-256 of the ROM in big-endian order),
  this shell's or does not load (the game always starts). At exit (and Quit) the code the interpreter had to run goes
  to `<files>/work/n64-<KEY>/discovered.txt` for the next Prepare (address, hash and bytes). A file that cannot be
  read sends the player back to the launcher with « The game ended: the game file cannot be read ».
- **Picture.** The VI's visible area at framebuffer resolution (320 x 240 for most games, 640 x 480 interlaced for a
  few, PAL 288 / 576 lines) through `host_window` at 4:3, letterboxed (1440 x 1080 on the Thor's 1080p screen),
  smoothed (linear); `RECOMP_N64_FILTER=nearest` keeps sharp pixels, `RECOMP_N64_SCALE=integer` whole multiples.
- **Controls.** Every connected pad drives controller 1, Xbox layout: left stick → the analog stick (8 % radial dead
  zone, ±80 at full travel, the diagonals about ±68 like the real stick's octagonal gate), A → A, B and X → B, right
  stick → the C buttons (C-up / down / left / right, past half its travel), Y → C-up, LB → L, RB → R, LT or RT → Z,
  Start → Start, D-pad → D-pad. With the rumble pak, the game's motor drives the pad's rumble (`host_pad_rumble`).
  `RECOMP_N64_PLAYERS=2..4`: host pad N drives controller N. Back on the device opens the in-game menu. These are
  the defaults: Home › Controls remaps them (and the stick's range and dead zone), and the on-screen pad has an N64
  layout ([Controls for the other consoles](#controls-for-the-other-consoles)).
- **Sound.** 48 kHz stereo from the AI (`n64_audio_pull`) through `host_audio`.
- **Pacing.** The VI's own rate (NTSC about 60 Hz, PAL 50 Hz, from the cycles each frame ran), absolute deadlines
  nudged ±2 % by the audio queue; vsync on; a stall of more than three frames resets the clock. The RDP is what a
  frame spends most of its time in on a phone. By default it is the Vulkan RDP (paraLLEl-RDP on the GPU, XBA-173,
  consoles/README.md « N64 RDP »): logcat `n64: Vulkan RDP on Adreno (TM) 740 (paraLLEl-RDP, default shaders)`. If
  the device has no usable Vulkan, or its driver cannot compile paraLLEl-RDP's shaders (checked at start, then without
  subgroups, then with the ubershader), it falls back to the software RDP (logged). `RECOMP_N64_RDP=software` (or
  `vulkan`) in recomp.cfg forces one. The software RDP draws on 2 renderer threads (while the emulation thread runs on) (`RECOMP_N64_RDP_THREADS=N`, 1..8, changes it).
  logcat says the rate reached every 10 s (`n64: 58.3 fps over
  the last 10 s (VI 60.0 Hz), 0 late resets so far`) and at quit (`n64: N frames in S s (… fps), N late resets`).
- **Saves.** `<internal files>/save/n64-<KEY>/`: `game.eep` (EEPROM 4K, 512 bytes, or 16K, 2 KB), `game.sra` (SRAM,
  32 KB), `game.fla` (FlashRAM, 128 KB), the one the cartridge uses (the ROM header's save type, else the game's first
  access), and `pak1.mpk` (the controller pak, 32 KB, once the game writes it): raw images, the files mupen64plus and
  ares read and write, so one can be copied in or out with `adb` (renamed). Read before the first frame (a cartridge
  save also fixes the save type: an `.eep`'s size picks 4K or 16K unless the header says); written a second after the
  game's last write (at most five seconds after its first), at once when the app pauses (Home, the in-game menu) and
  at exit (the menu's Quit too), through `.tmp` + rename, only when it changed.
- **Lifecycle.** As for the GBA: Home pauses (no frame runs, audio paused, rumble stopped) and resumes; the in-game
  menu holds it; Quit ends the process.
- **Options** (recomp.cfg): `RECOMP_N64_PAK` (`controller`, `rumble`, `none`: every game without its own choice),
  `RECOMP_N64_SCALE`, `RECOMP_N64_FILTER`, `RECOMP_N64_PLAYERS`, `RECOMP_N64_RDP_THREADS`, `RECOMP_N64_DEADZONE`
  (percent, default 8), `RECOMP_N64_STICK_RANGE` (default 80), `RECOMP_N64_TTY=1` (the game's IS-Viewer debug text
  in logcat), `RECOMP_N64_FRAMES=N` (stops after N frames, checks).

On a desktop the same runtime plays a cartridge with `n64_play` (consoles/README.md), where a game is checked first.

### Device check (N64 Prepare, XBA-174), for Alex

On the AYN Thor, an APK with the toolchain, our homebrew in `/sdcard/n64` as in « N64 games » below (gfx.z64,
hello.z64, cpu.z64), `adb logcat -s xboxrecomp SDL` throughout. Our homebrew all share the key `N___-00000000` (no
game code, no CRC in their headers): one pack folder for the three, the last one prepared is the one that loads.

1. A on **gfx**. Expected: its Home, « Not prepared · plays in the interpreter », **Play** focused, then **Prepare**,
   then **Controller pak**; the note says what Play, Prepare and the pak button do. Screenshot.
2. Right, A on **Prepare**: the Preparing screen (« Install tools » the first time, then Identify, Analyse, Translate,
   Compile, Pack) and the notification. Note the total time and the Compile step's time (27 files; a desktop core
   compiles them in about 33 s of CPU, so expect one to three minutes). Expected: back on the Home, « gfx is
   prepared », « Prepared · runs natively », **Play** then **Controller pak** (no Prepare button). `adb shell run-as
   fr.adserver.xrecomp.shell ls -l files/packs/n64-N___-00000000` shows `libtitle.so` (about 3 MB) and `pack.json`.
3. Play: logcat `n64: ROM identity e080322a138d2a58... (N ms)` (note N: the SHA-256 of the ROM with the ARMv8
   instructions; well under 10 ms for this 224 KB ROM), then `PACK_LOADED n64 N___-00000000 (1203 functions)` (about
   that many). The picture is the one of « N64 games » step 2. Leave it 30 s, note the three `fps over the last 10 s`
   lines, then Back › Quit game. logcat `n64: code at 2 address(es) the pack did not cover, 2 new record(s), in
   /data/user/0/…/files/work/n64-N___-00000000/discovered.txt` (libdragon's trampolines at the top of RDRAM) and the
   quit line `n64: N frames in S s (… fps)`. Compare the frame rate with « N64 games » step 3 (interpreted).
4. A on the card: « Prepared · 2 new addresses found · prepare again », **Prepare again** beside Play. Screenshot.
   Prepare again (note the time: only the files that changed compile, 2 of 27), then Play: `PACK_LOADED n64
   N___-00000000 (1205 functions)`, and at Quit no « did not cover » line (or 0 new records). The Home says
   « Prepared » again.
5. A stale pack: A on **hello**, Play. Expected: logcat `PACK_STALE n64 N___-00000000 (built from another ROM),
   running interpreted` (gfx's pack is in the shared folder), hello runs as in the interpreter. Its Home says
   « Prepared » all the same (the launcher checks the key and the files, the runtime the ROM's hash; real games do not
   share keys). Prepare hello from its Home (Prepare again is not offered: remove the pack with `run-as … rm -r
   files/packs/n64-N___-00000000` first): hello's own pack replaces gfx's, and gfx goes stale in turn.
6. A damaged pack: `adb shell run-as fr.adserver.xrecomp.shell sh -c 'echo x >> files/packs/n64-N___-00000000/libtitle.so'`,
   Play. Expected: the game still starts, `PACK_STALE … (libtitle.so is … bytes, pack.json says …)` or `(… sha256
   mismatch)`; the Home says « Not prepared »; Prepare repairs it.
7. A library folder added with the file picker (no All files access) holding gfx.z64: Prepare works the same;
   `prepare.log` (Share log on the Preparing screen, or `run-as … cat files/work/n64-N___-00000000/prepare.log`) has
   `rom: content://… copied to …/work/n64-N___-00000000/rom.z64 (229376 bytes)`. The byte-swapped `gfx.v64` (« N64
   games » step 6) gets the same key and is « Prepared » by gfx.z64's pack (same identity).
8. With the pack, the rumble pak still works (« N64 games » step 7): the pak button is last, Play logs `rumble pak`.
9. One of your own cartridge dumps (kept outside every repository): Prepare (note the total time, the file count of
   the Compile step and `lib_size` in `pack.json`), Play (`n64: ROM identity … (N ms)`: note N for this ROM's size,
   `PACK_LOADED`, picture, sound, the frame rate next to the interpreted one), play a few minutes (menus, a level, a
   save), Quit, Prepare again when the Home offers it, play again: fewer or no new records each round. Send the logcat
   and `prepare.log` if a step fails, and the frame rates.
10. Switch the Thor to French: the Home lines, the Prepare buttons and the notes in French.

### Device check (N64 games, XBA-174), for Alex

On the AYN Thor, `adb logcat -s xboxrecomp SDL` throughout. Our homebrew: `tools/consoles/build_homebrew.sh n64
out/hb` (Docker; `out/hb/n64/gfx.z64`, `hello.z64`, `cpu.z64`, `timer_irq.z64`). Copy them to the device's N64 folder:
`adb shell mkdir -p /sdcard/n64 && adb push out/hb/n64/gfx.z64 out/hb/n64/hello.z64 out/hb/n64/cpu.z64 /sdcard/n64/`.

1. Install the APK. If `/sdcard/n64` is not a library folder yet: Y › Library folders › Add a folder › `n64`.
   Expected: the cards « gfx », « hello », « cpu » with the **N64** badge (when other consoles' games are listed too),
   **no** chip (no more « Not playable yet »).
2. A on **gfx**. Expected: its Home, **Play** focused (without the toolchain: « Plays in the interpreter »,
   **Controller pak** beside it, a note on what Play and the pak button do; with it: « Not prepared · plays in the
   interpreter », **Prepare** then **Controller pak**: « N64 Prepare » below). Screenshot. A on Play: the game
   starts at once, fullscreen,
   landscape: on black, a red rectangle top left, a blue one under it, a small and a large magenta / white
   checkerboard, a colour-gradient triangle at the bottom; black bars left and right (4:3). Screenshot. logcat:
   `launcher: library: play n64-N___-00000000 from /storage/…/gfx.z64`, `PACK_NONE n64 N___-00000000 (not prepared,
   …)`, `n64: n64_runtime 0.1.0, ROM gfx.z64 (z64, "XBA gfx", …), key N___-00000000, CIC libdragon, NTSC (60 Hz)`,
   `n64: window on (4:3, aspect, linear), audio on, pads 1, controller pak, RDP vulkan, threads 1`, `n64: display 320 x 237
   at frame 3`.
3. **Frame rate** (the risk on a phone: the software RDP). Leave gfx running 30 s: logcat every 10 s
   `n64: … fps over the last 10 s (VI 59.8 Hz), N late resets so far`. Note the three values, then Back › Quit game:
   `n64: N frames in S s (… fps), N late resets`. Then with `RECOMP_N64_RDP_THREADS=1` and `=4` in recomp.cfg (one
   line each time, removed after, with `RECOMP_N64_RDP=software`), start gfx again and note the rate each time (the
   line `RDP software, threads N` confirms it).
4. A on **hello**: « XBA HELLO N64 » and « homebrew corpus XBA-149 » in white on dark blue. Screenshot. Home, wait 5 s, reopen XRecomp from
   recents: the game is where it was. logcat `n64: paused at frame …`, `n64: resumed`. Back: the in-game menu, the
   game holds; Quit game. logcat `clean exit`.
5. While an N64 game runs, start a GBA, PS1 or Xbox game from the launcher: the N64 one ends, the other starts; then A
   on gfx, Play: the other ends.
6. A library folder added with the file picker (no All files access) holding gfx.z64: it plays the same (logcat
   `shell: ROM document content://… -> fd:N`). The same ROM byte-swapped (`dd if=gfx.z64 of=gfx.v64 conv=swab` on a
   computer) plays the same and has the same key.
7. On gfx's Home, right, A on **Controller pak**: it reads **Rumble pak**, the note says the pad vibrates. Play:
   logcat `shell: n64 pak rumble, game n64-N___-00000000`, `n64: window on (…), rumble pak, …`. Quit, A on the card,
   A on **Rumble pak**: back to Controller pak (logcat `shell: n64 pak controller` at the next Play).
8. One of your own cartridge dumps (`.z64`, `.v64` or `.n64`; kept outside every repository): copy it to `/sdcard/n64`,
   Y (rescan), A on its card (named after the file; its key is `n64-<game code>-<CRC1>`), Play. Note: does it boot, is
   the picture right (and the 4:3 shape; the `display W x H` lines), sound, controls (the stick: full range, walking
   vs. running, diagonals; A, B, the C buttons on the right stick, Y = C-up, Z on LT / RT, L, R, Start, D-pad), and
   **the frame rate** (step 3's lines: around 60 for NTSC, 50 for PAL, and how many late resets). Send the logcat for
   a game that does not boot, shows wrong graphics or runs slowly.
9. Saves with that game: save in the game, wait 2 s (logcat `n64: save written (… bytes, written by the game) to
   /data/user/0/…/files/save/n64-…/game.eep` or `.sra` / `.fla`; `cartridge save type …` says which), quit, start again:
   the save is there (`n64: save loaded (…)`). A game that saves to the controller pak (its own menu says so): the
   same with `pak1.mpk` (`n64: controller pak 1 loaded from …`). `adb shell run-as fr.adserver.xrecomp.shell ls -l
   files/save/n64-*` lists them. Optionally copy a save of yours from mupen64plus in (`run-as … cp`, renamed to
   `game.eep` / `game.sra` / `game.fla` / `pak1.mpk`) and check the game finds it.
10. A game with rumble support, set to Rumble pak on its Home: the controller vibrates when the game says so (a hit,
    an explosion) and stops; Home while it vibrates stops it.
11. Switch the Thor to French: the N64 Home's buttons (« Controller Pak » / « Rumble Pak ») and note in French.
12. **Vulkan RDP vs software RDP** (XBA-173). gfx, default settings: logcat `n64: Vulkan RDP on Adreno (TM) 740
    (paraLLEl-RDP)` and `RDP vulkan, threads 1` (no `software RDP instead` line). The picture is step 2's, and the
    triangle's edges may be a pixel thinner (the Vulkan RDP follows angrylion). Leave it 30 s and note the three
    `… fps over the last 10 s` lines and the late resets. Then add `RECOMP_N64_RDP=software` to recomp.cfg, start
    gfx again (`RDP software, threads 2`) and note the same, then remove the line.
13. The same with your commercial dump (step 8): fps with the Vulkan RDP and with `RECOMP_N64_RDP=software`, 30 s
    each, in a busy scene (3D, several characters). Note any picture difference between the two: missing or extra
    geometry, wrong colours or textures, flicker, black frames, a frame that lags behind. Screenshots of both when
    they differ. Send the logcat if the game stops or the Vulkan RDP logs `[ERROR]` lines (tag `Granite`).
14. With frame generation on (an Xbox game with LSFG enabled, then an N64 game, then the Xbox game again): both
    still work. Each uses its own Vulkan device.

### The library screen

The grid (the full width, five columns of 288 px art, scrolling): one card per title ID — title image or monogram,
one-line title, and its state as a dot and a label only when it is not **Ready** (« Not imported » is a neutral
grey dot) — sorted by title. The focused game's state and play time are one context line at the bottom left
(« Ready · Yesterday · 12 h 40 min played »). D-pad between cards (a ring around the art glides to them), **A** the
game's Home, **Start** its primary action without opening Home, **Y** the **Options** sheet — **Rescan**, **Storage**
(the free space), **Library folders** (the folder's name, or « 4 folders »), **Settings**, **About** — and **B**
leaves the app. **Library folders** opens a second sheet: one row per folder (its name; « Internal storage », « SD
card » or « Chosen folder » for SAF), then **Add a folder** (the tree picker, as above). A on a folder: **Keep it**
(focused first) or **Remove from the library** — its games leave the grid and the index at once (their records,
packs and saves stay, as for a game gone from a folder), its permission is released, nothing in it is deleted. An empty library shows why (no folder yet, nothing found, the scan's progress) and a **Library folder** button.
The scan's progress, a scan cut at 5,000 entries and the « Other files (n) » line go under the grid. The library is
always in the stock colours; only the backdrop's glow follows the focused card (from its cached `<TID>.theme`).
Opening a game's Home applies its theme; back to the library resets it. The bottom screen shows the focused game:
its art, title and primary action.

States (`GameState`, first that applies): **Running**, **Preparing**, **Queued (n)**, **Importing**, then with the
game's files present **Prepare failed**, **Pack invalid** (the pack was built from another `default.xbe` —
`Pack.xbeSha256` against the record's — or the shell refused it), **Ready**, **Needs preparing**; without them
**Not imported**, **Needs copying**, **No file access** (per folder: a path without All files access, a tree whose
permission is gone), else **Source gone** (an in-place folder that vanished) or
**Game data missing**. A game with neither files nor source leaves the grid (its pack and saves stay in Storage).
With the on-device toolchain a **Ready** card says how prepared the game is (`GameState.prepared`): **Prepared** (green
dot), **Prepared · out of date** (amber: its pack.json `toolkit_sha256` is not this APK's toolkit digest —
`PackFreshness` OLDER_TOOLKIT, as Home's « Prepared by an older version of the app »; it still plays), or, for a
GBA / PS1 / N64 game without a valid pack, **Not prepared** (neutral grey: it plays in the interpreter). « New code
found » stays on Home only (most played games have some). Without the toolchain a Ready card shows no chip.
Variants: images with the same title ID are one game; the active one is the record's source, else the most recently
modified.

### Home of a library game

Home shows the **selected** game (`Library.Games.selected`): its folder, state, title ID, title, play time. B goes
back to the library. With its files: as before (Play, Prepare, Preparing, See what failed), plus **Queued (n)** (A
takes it out of the queue) and, when another game runs, Play warns first (« X is running and will close », a second
press within 5 s ends it). Without them: **Import** (the image extracted, or the SAF folder copied, into
`games/<TID>`; a needed Prepare starts right after), **Copy into the app**, **Choose the folder again**, and
**Prepare now**: tools.prepare needs only `default.xbe`, so the cached copy is the Prepare's source and several games
can be prepared (queued) before their gigabytes are imported.

**Selected vs launched.** The preferences `game_dir`, `title` and the new `title_id` keep meaning « the game the
process runs or last ran » (ShellActivity, quick reboot, the game's language read them). Opening another game's Home
writes nothing there; `GameStore.rememberLaunch` runs only in `launchShell()`. The launcher opens on the last
launched game; « Open the last game directly » starts it.

**Caches.** Saves (`UDATA/<tid>`, `TDATA/<tid>`) are per title, and so is Z: (`save/Cache/<tid>`, since K-9; the
stash below still moves `save/Cache` as a whole, which is harmless). Entries of `save/Cache` from before K-9 belong
to the owner: `SaveCaches.migrateLegacyCache` renames them into `Cache/<owner>` when that title is launched (owner
unchanged, or its stash restored), once and without replacing anything, so Fable does not unpack its data again. `save/Cache` (Z: and the cache partitions),
`save/TitleCache` (E:\CACHE), `Partition3-5.img` and `launch_data.bin` are not: `files/library/cache_owner` names their
title, and `launchShell()` calls `SaveCaches.switchTo` (only while no game process runs): the live set moves by rename
into `save/.stash/<owner>/`, the new title's stash comes back, `launch_data.bin` (a quick-reboot hand-off) is
dropped. Nothing is wiped: Fable's unpacked cache does not have to be rebuilt after a game of Burnout 3.

**Play time** is per record (`last_played`, `play_ms`): a session opens at launch (`session_tid`) and closes when the
launcher comes back.

### Cover art for the other consoles

GBA, PS1 and N64 games carry no title image (an XBE does, `TitleArt`), so their cards and Home showed a monogram.
`CoverArt.java` gives them one, kept on the device only in TitleArt's cache (`files/art/<console>-<KEY>.png`):

- **Downloaded**, when Settings › General › « Download cover art » is on (default): after each library scan, a
  worker asks libretro-thumbnails (`raw.githubusercontent.com/libretro-thumbnails/<system>/master/Named_Boxarts/`,
  the box art RetroArch uses) for each game without a cover. The name is the game's file name (No-Intro / Redump
  dumps are named so), then without dumper tags (`[!]`, `(EDC)`, `(Track 1)`), then the bare title with the usual
  regions; libretro's characters `&*/:`<>?\|"` become `_`. Only the file's name goes out, in the URL. A game with
  no art is remembered (`<key>.miss`) and asked again after 7 days; a network error is retried at the next scan.
  Each cover found shows at once. Logcat « launcher: cover art <key>: <name> » or « none for … ».
- **Captured**, otherwise: 45 s into a game without a cover (`ShellActivity`, menu closed), a frame of the game is
  kept as `<key>.shot.png`. A downloaded cover, when one comes later, wins over it.

Nothing of it is in the APK or the repo. « Delete the imported copy » removes both with the rest of the art.

### Storage

Options › Storage on the library: the free space in the top bar, then per game (its name as a small label) what the
app stores — **Imported copy**,
**Pack**, **Prepare files** (`files/work/<TID>`), **Game caches** (live or stashed), **Saves** — with their sizes.
Each is deleted on its own (A twice within 5 s, on a worker, after ending the game that runs from it). Saves have
their own row and a stronger warning; nothing else deletes them. The sizes are the rows' values, the paths are in
the focused row's description. Manage shows the same rows for the selected game.
Only what the app made is ever deleted: never a folder read in place nor anything in the library folder. Deleting
the imported copy keeps the game in the library (import it again); its art and theme go only when the title also
left the library folder.

### Files

| What | Where |
|---|---|
| Records (launcher only; temporary file, then rename) | `files/library/games.json`: `selected`, per title ID `title`, `source`, `data_dir`, `data_kind` (`imported`, `in_place`, `cfg`, `image`), `xbe_sha256`, `added_at`, `last_played`, `play_ms` |
| Scan index (rebuildable) | `files/library/index.json` (per item its `folder`; `console`, `key` for another console) |
| Cached `default.xbe` | `files/library/xbe/<TID>-<sha8>/default.xbe` |
| Imported games | `<external files>/games/<TID>/` (staged in `games/.import`, swapped once checked) |
| Disc images read in place | `files/discs/<TID>/`: the image's XBEs and `disc.json` (staged in `discs/.import`) |
| Owner of the shared caches, stashes | `files/library/cache_owner`, `files/save/.stash/<TID>/` |

**Migration.** The first start of this version (`Library.migrate`, no file moves): the game a launcher without the
library would run (the chosen folder, else `RECOMP_GAME_DIR`, else `files/game/` with a `default.xbe`) becomes the
first record and the owner of the caches, `title_id` is written; logcat `launcher: library: migrated <TID> (<dir>)`.
`files/game/` stays that game's data and is never an import target again; a later import of the same title into
`games/<TID>` removes it once the new copy is checked (and no game process runs). An older APK still finds its
preferences and `files/game/`. The adb workflow (`RECOMP_GAME_DIR`, pushing to `files/game/`) still works.

### A test library

`tools/android/make_test_library.sh OUT_DIR [COUNT]` builds a synthetic library: images made with
`tools/xiso/mkxiso.py` around the conformance XBE with patched title IDs, names, regions and versions (two versions
of one game), one game folder, one nested image, a non-Xbox `notes.iso`, and a hidden and an `Android/` image that
the scan skips. `adb push OUT_DIR /sdcard/xbox-test`, then choose it as the library folder. The games import in a
second; they do not run (the XBE is not a game).

Tests: `LibraryScreenTest`, `LibraryScannerTest`, `StorageTest` (plain JUnit; the scanner over in-memory folders:
several folders and every console, per-folder caps, an unreadable or overlapping folder), `LibraryStoreTest` and
`LibraryContextTest` (older `index.json` / `games.json` still valid, the folder list and its migration, a scan of two
folders by path), `LauncherLibraryTest` (routing, grid with 0 / 1 / 7 / 20 games in en / fr at 720p / 1080p →
`build/screenshots/library-*.png`, navigation, theme, launched game written at Play only, caches switched, not
imported, Prepare now, queue, Storage, two folders with a GBA game and Options › Library folders →
`library-consoles.png`, `library-folders.png`), `LauncherPanelsTest` (`companion-library-*`).

### Device check (game library), for Alex

`adb logcat -s xboxrecomp` throughout. Library folder `/sdcard/xbox` with Burnout 3 and Fable.

1. Install over the current APK and open it: Fable's Home as before (same game, play time, saves). logcat
   `launcher: library: migrated 4D5300D1 (<dir>)` once (not at the next start).
2. Home › Manage › Library is not there yet (one game): « Change game » as before. Choose your game: a third card
   **Game library folder**. Choose `/sdcard/xbox`: the library opens, the grid fills; logcat
   `launcher: library: scan N items, M games, T ms (…)`. Send the line.
3. Burnout 3 shows once (one card even with several images of it, « n versions » in the details), with its title
   image; Fable shows **Ready**; the other files of the folder, if any, on the « Other files (n) » line.
4. D-pad between the cards (the highlight glides, the backdrop's glow takes each game's colour), A on Burnout 3: its
   Home in its colours, chip **Not imported**; B: back to the library in the stock green, the focus on Burnout 3.
5. Y: the rescan is quick (`… 0 opened, N unchanged`). Copy another ISO into `/sdcard/xbox`, Y: it appears.
6. Put a non-Xbox `.iso` (any file renamed) in the folder, Y: « Other files (1) » under the grid, no card.
7. Burnout 3 › Import: the progress ring; Cancel once (back to its Home, nothing left in `games/`), then Import again
   to the end: Home, a Prepare starts by itself (if the APK has the toolchain). `adb shell ls
   /sdcard/Android/data/<app id>/files/games/` shows its title ID.
8. While Burnout 3 prepares, Fable or another game › Prepare: its chip reads **Queued (1)**; A on it takes it out;
   start it again and let it run after the first one.
9. On a game not imported: **Prepare now** — the Prepare runs from the cached `default.xbe` (logcat `prepare
   started for <TID> (…/files/library/xbe/<TID>-…)`); it ends « … is prepared: import it to play ».
10. Play Fable, back to the launcher, play Burnout 3 (Play warns « Fable is running and will close », press again):
    logcat `launcher: game caches switched to <TID>`; back on Fable later, it starts without the long cache rebuild
    (`run-as <app id> ls files/save/.stash` shows the other game's caches).
11. In Fable, Pause › Quit (quick reboot): Fable comes back, not Burnout 3.
12. Library › Options (Y) › Storage lists both games; delete Burnout 3's Prepare files (A twice), then its imported copy:
    the game stays in the grid as **Not imported**, its saves row is still there; your ISO in `/sdcard/xbox` is
    untouched.
13. Rename `/sdcard/xbox` (or remove the SD card) and open the launcher: no crash; games read in place show
    **Source gone**, imported ones still play; rename it back, Y.
14. Bottom screen (Thor): on the library the focused game's art, title and action (Play, Import, …); on a game's
    Home its Play (or Prepare); on Settings and Storage the art alone.
15. In French: the library, the chips (« Non importé », « En attente (1) »), Storage.
16. Turn All files access off (system settings), choose the folder again with **Use the file picker**: the games
    are listed; folders read « À copier » / « Needs copying », images import.

### Device check (library folders, XBA-178), for Alex

`adb logcat -s xboxrecomp` throughout. Folders `/sdcard/xbox` (your Xbox images), `/sdcard/gba`, `/sdcard/ps1`,
`/sdcard/n64` with a few games each (your own dumps or the homebrew corpus of C0-5).

1. Install over the current APK (library folder `/sdcard/xbox` already set): the library opens as before, same
   games, no badge on the cards; logcat `launcher: library: folder … migrated to library_folders` once.
2. Y › **Library folders**: one row « xbox » (« Internal storage »), then **Add a folder**. Add `/sdcard/gba`, then
   `/sdcard/ps1` and `/sdcard/n64` the same way: after each, the library rescans (logcat `launcher: library: scan of
   N folders: …`) and the new games appear, sorted by title among the Xbox ones.
3. Every card now has a badge (Xbox, GBA, PS1, N64); PS1 / N64 games read **Not playable yet** (grey dot; GBA games
   play since XBA-157, see its own check); a PS1 disc as `.cue` + `.bin` tracks is one card; the same N64 game in
   `.z64` and `.v64` is one card.
4. A on a PS1 card: the note « PS1 games are listed but not playable yet… », no Home; Start the same. The Xbox games
   open, prepare and play as before.
5. Y twice (rescan): quick, `… 0 opened, N unchanged`.
6. Y › Library folders: four rows. A on « gba » › **Keep it** (focused first): nothing changes. A on « gba » ›
   **Remove from the library**: the GBA cards leave at once, the note « gba is no longer in the library… »; the files
   in `/sdcard/gba` are still there. Close and reopen the app: still three folders, no GBA card.
7. Without All files access (system settings off), add a folder with **Use the file picker**: its games are listed;
   the folders read by path show **No file access** on their Xbox games until access is back.
8. In French: « Dossiers de la bibliothèque », « Ajouter un dossier », « Pas encore jouable ».
9. Bottom screen (Thor): on a PS1 card its title and « Not playable yet », no action button; on a GBA card, Play.

## Prepare on the device (the app side)

The on-device Prepare turns the player's game into a title pack (`<files dir>/packs/<TITLEID>/{libtitle.so,
pack.json}`); the contract (command, progress lines, error codes, pack.json) is
[prepare.md](prepare.md). This section is the app's half: `PrepareService`, `PrepareRunner`, `PrepareProtocol`,
`Pack`, `Toolchain`, `LogProvider` and the launcher's Prepare screens.

**Hidden without the toolchain.** The feature shows only when the APK carries `libxbapython.so` and `libxbacc.so` in
`nativeLibraryDir` and the three tool zips in its assets. Without them (the shell as built today) the launcher behaves
exactly as before. Native libraries are extracted at install (`jniLibs.useLegacyPackaging = true`), so they can be
exec'd and `libmain.so` can be handed to the linker.

**Tool assets** (`Toolchain`): `assets/prepare/python-stdlib.zip`, `sysroot.zip`, `toolkit.zip` (any zip in
`prepare/` or the root whose name contains `stdlib`, `sysroot`, `toolkit` also works), each optionally with a sidecar
`<zip>.sha256` (checked before extracting; a mismatch fails with `ASSET_HASH`). Each is extracted once into
`<files dir>/python-<v>`, `sysroot-<v>`, `toolkit-<v>`, where `<v>` is the first 12 hex digits of the zip's SHA-256,
through a `.tmp` directory renamed when complete (`.xba-complete` holds the hash); other versions are deleted. The
hashes are cached per APK install (`lastUpdateTime`), so a second start opens nothing. This is the service's
« Install tools » phase (status `id=install`, `step=0`), on its worker thread.

**The service** (`PrepareService`, process `:prepare`, foreground type `specialUse`: `dataSync` is capped at 6 h a day
on Android 15). The launcher starts it with the game folder and the title ID (from `default.xbe`'s certificate,
`cert + 0x08`). It takes a partial wake lock (6 h at most), installs the tools, then runs, in its own process group
(`setsid`):

```
<nativeLibraryDir>/libxbapython.so -m tools.prepare <game dir> --android --work <files>/work/<TID>
    --pack-dir <files>/packs --cc <nativeLibraryDir>/libxbacc.so --cc-prefix-arg clang --sysroot <files>/sysroot-<v>
    --shell-lib <nativeLibraryDir>/libmain.so --jobs N --opt 2 --shell-abi 1
cwd <files>/toolkit-<v>; PYTHONHOME=<files>/python-<v> PYTHONPATH=<files>/toolkit-<v>
PYTHONPYCACHEPREFIX=<cache>/pycache TMPDIR=<cache>/tmp HOME=<files> LIBCAPSTONE_PATH=LD_LIBRARY_PATH=<nativeLibraryDir>
PYTHONUNBUFFERED=1 XBA_PYTHON=<nativeLibraryDir>/libxbapython.so
```

`N` = (MemAvailable − 3 GB) / 1 GB, between 1 and min(5, cores). One Prepare at a time (a second start is ignored).
The notification shows the step, a progress bar, the time left and **Cancel**. Cancel (notification or launcher)
sends SIGTERM to the group, then SIGKILL 5 s later. A child killed by a signal it did not ask for (Android's
phantom-process killer, low memory) runs once more with half the jobs; a second kill fails with `KILLED`. After an
« ok », the pack is checked (`Pack.check`); a pack that does not pass fails with `PACK_INVALID`. Starting again after
any end runs the same command: tools.prepare's stamps skip what was done.

**Files**, per title in `<files dir>/prepare/<TID>/`:

- `status.json`, written atomically (temp file, rename) at most every 250 ms and at each step change:
  `state` (`running`, `done`, `failed`, `cancelled`), `step`, `n`, `id` (`install`, `identify`, `analyse`, `lift`,
  `compile`, `pack`), `pct` (per step), `eta` (s, −1 = unknown), `msg`, `code`, `started_at`, `updated_at` (ms since
  the epoch), `log`, plus `jobs`, `attempt`, `title_id`.
- `prepare.log`: the command's whole stdout and stderr, one header per attempt, the outcome, and at the end the tail
  (2 MB) of tools.prepare's own `<work>/prepare.log`. « Share log » sends it through `LogProvider`
  (`content://<app id>.logs/prepare/<TID>/prepare.log`, read-only grant).

Codes the app sets itself: `KILLED`, `CANCELLED`, `SPAWN_FAILED`, `NO_RESULT` (exit without a result line),
`PACK_INVALID`, `INSTALL_FAILED`, `NO_TOOLS`, `ASSET_HASH`, `START_FAILED`; the others come from tools.prepare.

**A pack is valid** when `pack.json` parses, `format` is 1, `title_id` is the game's, `shell_abi` is the app's
`Pack.SHELL_ABI` (1), and `lib` exists with `lib_size` bytes. The native loader checks the same and `abi`, then
dlopens it (« Title packs » below); neither checks `lib_sha256` at each start. A valid pack can still be stale
(XBA-127, [prepare.md « When a pack is stale »](prepare.md#when-a-pack-is-stale)): built from another `default.xbe`
(another revision or region under the same title ID: never run, Home offers **Prepare again**), or lifted by an older
toolkit than the APK's (it still plays; Home keeps Play and offers **Prepare again** beside it with a note).

**The launcher.** With a game ready and the toolchain present:

| Case | Home | Preparing screen |
|---|---|---|
| No valid pack, nothing run | **Prepare** instead of Play, « about 20–30 min » (30–45 below 8 cores, 40–60 under 6 GB of RAM), « Needs preparing · once, about … » with an amber dot. Prepare refuses under 2 GB of free internal space, and warns (a note) under 6 GB of memory. | |
| Running | « Preparing… », the line « Compile · 42 % · about 12 min left » | Home's layout: « Preparing · step 5 of 6 · Compile », one bar, the percentage, the time left, « You can leave the app ». Nothing to focus: B twice cancels; Y hides it and the Prepare carries on. Polls `status.json` twice a second; the screen stays on. |
| Failed | **See what failed**, the line « Prepare failed · CODE » with a red dot | « Preparation failed · CODE » with a red dot, the message (a hint for `KILLED` and `NO_SPACE`), **Retry** and **Share log** (also Y) as buttons. |
| Cancelled, or interrupted (a `running` status older than 15 s with no `:prepare` process) | « Prepare » carries on where it stopped | « Preparation stopped » with an amber dot, why, **Resume** and **Share log**. |
| Pack valid | **Play**, as before | Done: back to Home, « … is ready ». |

The app opens on the Preparing screen while a Prepare runs or after one failed. « Open the game directly » does not
skip a Prepare that is needed. On Android 13+ the first Prepare asks for the notification permission (refused: it
runs anyway, without the notification's progress).

**Tests** (JVM, Robolectric 4.14.1, `android/app/src/test`): the line parser, `status.json`, every pack check,
the asset install, the runner against scripted fake commands (progress → done, an error, a hang → cancel with
SIGKILL, a kill → retry with half the jobs), the service, and the launcher's states rendered to
`android/app/build/screenshots/prepare-*.png`:

```
cd android && gradle testDebugUnitTest -x externalNativeBuildDebug     # in xba-android:local, the repo mounted
docker build -f tools/docker/android.Dockerfile --target app-unit-tests .                 # the same, as CI runs it
```

### Device check (Prepare), for Alex

Needs an APK with the toolchain (the embedded-prepare branch merged) and the pack loader; until then steps 1 and 13.

1. The shell APK as built today (no toolchain): the launcher is unchanged: Home shows **Play**, no « Needs
   preparing » chip, no Prepare notification channel in the app's settings.
2. With the toolchain APK and your game chosen: Home shows **Prepare** and « Needs preparing · once, about
   20–30 min » with an amber dot. Screenshot.
3. A on Prepare. On Android 13+ the notification permission is asked once: allow it. Expected: the Preparing screen,
   « Preparing · Install tools » for a few seconds the first time; then « step n of 6 » with Identify, Analyse,
   Translate, Compile, Pack in turn; the bar, the percentage and the time left move. logcat `prepare: started for <TID> ... N jobs`
   (N = 4 or 5 on the Thor).
4. Pull down the notification shade: « Preparing <title> », the step, the bar, the time left, **Cancel**. Screenshot.
5. Press Y: Home shows « Preparing… » and its line with the step and percent updating. Press Home on the Thor, use another app
   for a minute, turn the screen off for a few minutes: the notification keeps moving; back in the app, the Preparing
   screen shows the current step.
6. B once: « Press again » on Cancel; B again: within a few seconds « Preparation stopped », **Resume** and **Share
   log**. `adb shell ps -A | grep -E 'xbacc|xbapython'`: nothing left.
7. Resume: it starts again and skips the steps already done (Identify / Analyse / Translate are quick, Compile
   compiles only what was missing). Let it finish: Home with **Play** and « … is ready »; the notification says
   « Ready ». Write down the whole time and the compile time (« Measurements » in prepare.md asks for them).
8. Play: the game starts from the pack (`PACK_LOADED`, « Title packs » below). Quit, start the app again: Home
   with Play, no Prepare.
9. Second start of a Prepare (e.g. after `adb shell run-as <app id> rm -r files/packs`): no « Install tools » phase
   this time (the tools are installed once per APK version); `files/` holds one `python-*`, `sysroot-*`,
   `toolkit-*` each.
10. Failure: `adb shell run-as <app id> rm -r files/packs`, start a Prepare and, during Compile,
    `adb shell run-as <app id> sh -c 'kill -9 $(pidof libxbapython.so)'`. Expected: it restarts by itself with half
    the jobs (logcat `prepare: killed by signal 9, retrying with N jobs`). Kill it again: « Preparation failed »,
    code `KILLED` with the hint; Share log opens the share sheet with `prepare-<TID>.log`; send it to yourself and
    check it holds the command, the progress lines and the end of tools.prepare's log.
11. Interrupted: during a Prepare, `adb shell am kill <app id>` is not enough (foreground); use
    `adb shell run-as <app id> kill -9 $(adb shell pidof <app id>:prepare)`. Open the app after 20 s: Home says
    « Interrupted », Prepare resumes.
12. Low space: fill internal storage under 2 GB free, A on Prepare: the note says how much is needed and free, and
    nothing starts.
13. Switch the Thor to French: Prepare, Preparing and Failed screens, the notification and the note in French.

### Device check (stale packs, XBA-127), for Alex

A toolchain APK and one prepared game (Play on Home).

1. `adb shell run-as <app id> cat files/packs/<TID>/pack.json`: it has `toolkit_sha256` (64 hex digits) once the game
   was prepared with this APK; `unzip -p <apk> assets/prepare/toolkit.zip.id` prints the same value.
2. Another XBE: with the game read in place (or imported), replace its `default.xbe` by another revision or region of
   the same disc (same title ID; keep the original aside). Open the app: within a second Home says « Another version
   of the game · prepare it again » with an amber dot, the primary is **Prepare again**, and the note says saves are
   kept. Screenshot. A on it starts a Prepare (from scratch: logcat `prepare: started`). Before it ends, nothing
   starts the old pack (turn on « Open the game directly », restart the app: Home, no game).
3. Put the original `default.xbe` back: Home shows **Play** again (the old pack matches it again, or the new one does
   once the Prepare ended). Your saves in-game are all there.
4. Older toolkit: install an APK built from a commit whose toolkit tree changed (any change under `tools/recomp/`) over
   the current one, without clearing data. Home: « Prepared by an older version of the app », **Play** first and a
   quiet **Prepare again** next to it, a note about the update's fixes. Play starts the game at once, as before.
   Screenshot.
5. Install an APK whose commit changed only Java (or docs): no « older version » line; Play as before.
6. From step 4, **Prepare again**: a full Prepare runs; when it ends, Home shows Play without the line, and the game
   does not start by itself.

## Library check (XBA-130)

« Check my library » (library › Options (Y), shown with the on-device toolchain) gives the state of the whole library
on the device: it prepares every title that needs it, boots each one for 3 minutes with the compat runner's pad script
and status file, grades it with the compat runner's own rules, and writes one line per title to
`<files>/check/compat-results.jsonl`, the desktop schema ([compat-runner.md](../technical/compat-runner.md)) plus the
device's fields. **Share report** sends that file (`content://<app id>.logs/check/compat-results.jsonl`, as Share
log). On a desktop: `python -m tools.compat report compat-results.jsonl --matrix` and `python -m tools.compat plane
compat-results.jsonl` read it (a Device column appears).

| Piece | What |
|---|---|
| `CheckActivity` (`:launcher`) | The screen: what a check would do (games, how many to prepare and to start, about how long; **Check all n**, **Check <selected game> only**), a check under way (n of N, the title and its step, the last results, the stage counts; **Pause** / **Continue**, **Stop** (twice), **Share report**), the summary when done. Keeps the screen on, dimmed. The launcher opens it instead of itself while a check runs (also when a checked game leaves by itself). |
| `CheckService` (`:launcher`, foreground `specialUse`) | Keeps the launcher's process alive while the game runs in front of it, holds a partial wake lock, ticks the driver twice a second on its own thread; the notification shows where the check is, with **Stop**. |
| `CheckDriver` | The steps (a state machine, every device effect behind `Platform`, so the JVM tests drive it with fakes and a fake clock). |
| `CheckPlatform` | The device: PrepareService, ShellActivity in a fresh main process, ActivityManager (the game's process, `ApplicationExitInfo`), `/proc/<pid>/status` (VmHWM), logcat of the game's PID, the thermal status and the battery, and the embedded CPython for the grade. |
| `LibraryCheck` | `check.json` (the state), `current.json` (the run the game process belongs to), the plan, `needsPrepare`. |

**Per title.** Needs a Prepare when it has no valid pack, or one Home's pack freshness (XBA-127) does not call current: built
from another `default.xbe` (« disc changed ») or by an older toolkit than this APK's, by content digest (« toolkit
changed »; Home keeps Play there, the check prepares, since it measures this APK's toolkit). The Prepare
is PrepareService's, as from Home (its notification, its `status.json`); failed → the title's line is stage `none`,
blocker `prepare failed: CODE: msg`; interrupted (killed, cancelled by the system) → started again, at most 3 times.
A disc image the library scan found and never imported is prepared from and booted from the image itself when the
app can read it by its path (XBA-126: D:\ served from the image, nothing extracted). A title whose files are not in
the app otherwise (an image reached only through a document) is prepared from the cached `default.xbe` and not
started: stage `prepares`, blocker `not booted: the game's files are not in the app`. Then the boot:

1. `current.json` (title ID, `<files>/check/run/<TID>/`, until when); the launcher's launch steps (the launched game,
   the shared caches, the self-heal stamp); ShellActivity with `NEW_TASK | CLEAR_TASK`, a new main process.
2. ShellActivity puts the run's environment in place before SDL starts (`LibraryCheck.applyRunEnv`):
   `RECOMP_STATUS=<run>/status.jsonl`, `RECOMP_STATUS_MS=1000`, `RECOMP_PAD_LIVE=<run>/pad-live.txt`, `RECOMP_USB=1`
   (recomp.cfg's value when it has one): `tools.compat`'s `boot()` minus `RECOMP_HEADLESS` and the screenshots. The
   game is on screen, headed, as played. A quick reboot keeps it (same title, same `current.json`).
3. The pad: `start` three times from 8 s, then `a`, every 2 s, on the **status clock** (the runtime's first status
   line gives its origin: the time from tap to runtime start is not counted, as on a desktop).
4. The watchdog: after 180 s the check screen is brought to the front (allowed while the game is still visible), the
   game's process is killed by PID, its logcat dumped to `<run>/run.log`. A process that ends earlier is read from
   `ApplicationExitInfo` (exit code, signal, native crash, Java exception, ANR, low memory); a window that closes (no
   game data, a refused pack) ends the run too.
5. The grade: `libxbapython.so -m tools.compat grade` (tools/compat/device.py) with the status file, the log, the press
   times, how it ended, the work folder (`ident.json`, `discovered.jsonl`) and the device's fields; it appends the
   line. If the Python cannot run, the app writes a short line itself (`"graded_by": "app"`, stage `prepares` or
   `none`).

**The line** (no game data): the desktop's `source` (the certificate's title name), `title_id`, `title`, `xdk`,
`xdk_libraries` (each XDK library's build), `engines` (names), `layers`, `stage`, `best`, `blocker`, `runs` (the
status numbers), `toolkit`, `time`, `prepare_s`; plus `host` `android`, `device` (« AYN Thor »), `android`, `api`,
`soc`, `app_build`, `renderer` (what really drew, from the runtime's `[GPUGL]` lines: `GL: <GL_RENDERER>`; `CPU rasteriser (<why>)` for `RECOMP_GPU=0`, no GL context, a base shader that does not compile or a GPU given up after resets; `GPU selected (no frame drawn)`),
`seconds`, `peak_mem_mb` (the game process's VmHWM). The status files and logs stay in `<files>/check/run/`.

**Resumable.** `check.json` is written at every step. After a crash of the app or a reboot, opening the app shows the
check (the launcher hands over) and it goes on: a Prepare it waited for is waited for or started again (the stamps
skip what was done); a boot it was in the middle of is graded from what that run left, blocker « the check was
interrupted during this boot », and the check goes on with the next title (a title that takes the device down does not
loop).

**Pauses.** Android lets the app start an activity, or a foreground service, only from its screen: the check screen
must be in front for the next Prepare or boot (« Paused: come back to this screen »; the notification opens it).
The game leaving the screen during its boot (Home, the screen off) ends that run, which is done again on return
(a game in the background is held by the runtime: its numbers would mean nothing); after 3 such interruptions the title
is graded as it stands. A severe thermal status (`PowerManager`, `THERMAL_STATUS_SEVERE`) cancels a running Prepare
and starts nothing until it is back to moderate (the Prepare then resumes from its stamps). Not charging: Start
warns and asks again; below 20 % and not charging the check pauses until it is charging or at 30 %. A pause during a
boot lets the boot finish.

**Tests.** `CheckDriverTest` (the steps with a fake device: Prepare → boot → grade, the pad on the status clock, the
watchdog, a crash, a failed and an interrupted Prepare, a title not imported, leaving during a boot, thermal, battery,
pause, stop, a restart in the middle of a boot), `LibraryCheckTest` (files, plan, stale packs, exit reasons, the
grade's arguments, the app's own line, Share report's URI), `CheckActivityTest` (the screens →
`build/screenshots/check-*.png`, Start, Stop, the launcher's hand-over); `tools/compat/test_device.py` (the grade and
the schema, report / matrix / plane on a device file) and `test_e2e.py::test_a_check_run_as_the_device_makes_it`
(`XBA_E2E=1`: the synthetic title booted with the device's environment, headed, pressed on the status clock, killed by
PID, graded: same stage and blocker as `tools.compat run`).

### Device check (library check), for Alex

Needs the toolchain APK, the library on (`/sdcard/xbox`), the Thor plugged in. `adb logcat -s xboxrecomp` throughout.

1. Library › Options (Y): **Check my library**. The screen says how many games, how many to prepare and to start, about
   how long, and the games not imported (prepared, not started). Screenshot.
2. Select Fable in the library, Y › Check my library › **Check Fable only**. Expected: Fable's Prepare if needed (its
   notification), then the game on screen, Start and A pressed by themselves from about 8 s in, then after 3 minutes
   back to the check screen with Fable's line (`check: 4D5300D1: <stage> ...` in logcat). Note the stage and blocker.
3. The same for Burnout 3, Black and KOTOR, one at a time. During one of them press Home within the 3 minutes:
   logcat `check: ... left the screen`, the screen says « Paused: come back... »; open the app: the check screen
   comes back and boots that game again.
4. **Share report**: the share sheet with `compat-results.jsonl`; send it to yourself. Open it: one line per run, no
   game data (title ID, name, XDK, engines, stage, blocker, numbers, device fields).
5. During a Prepare: `adb shell am kill fr.adserver.xrecomp.shell` is not enough (foreground); kill the launcher's
   process: `adb shell run-as <app id> kill -9 $(adb shell pidof <app id>:launcher)`. Open the app: the check screen,
   carrying on.
6. During a boot, reboot the Thor (`adb reboot`). Open the app: the check carries on with the next title; the
   interrupted one's line says « the check was interrupted during this boot ».
7. The whole library: **Check all n**, leave it overnight on the check screen, plugged in. In the morning: « n games
   checked » and the stage counts; Share report and send the file. Also send `adb shell dumpsys thermalservice | head
   -20` if the screen said « too hot » at any point.
8. On the desktop: `python -m tools.compat report compat-results.jsonl --matrix` shows the matrix with a Device column.

## Dual screen (AYN Thor)

The AYN Thor has two screens: the top one (6", 1920 x 1080, 120 Hz) is the main display, the bottom one (3.92",
1240 x 1080, touch) is a second `Display`. The launcher and the game stay on the display they start on (the top one);
a **companion panel** goes on the other one. On a single-screen device nothing of this runs.

### How it is put together

- `DualScreen.java`: the decisions, pure functions over plain display records (JUnit-tested over fake lists).
  `choose()`: a display of `DisplayManager.getDisplays(DISPLAY_CATEGORY_PRESENTATION)` other than ours, not private,
  not off; failing that, any other public display that is on. `decide()`: show, move or dismiss from the setting, the
  chosen display and the one shown on. Every display is logged once per change (`<tag>: displays:` then one line
  each: id, name, flags, size, refresh, state).
- `CompanionScreen.java`: one `android.app.Presentation` per process, shown from the activity's `onStart`, dismissed
  in `onStop`, with a `DisplayListener` for displays that come, go or change state. Pure black window. ShellActivity
  owns one (tag `shell`, the in-game panel); `LauncherCompanion` owns the launcher's (tag `launcher`).
- The panels are plain Views drawn on a 1240 x 1080 design grid scaled to fit (`PanelView`): `CompanionPanel` in the
  game, `LauncherPanel` and `PreparingPanel` in the launcher. No animation: a 1 Hz tick (stats, battery, thermal,
  dimming) that redraws only when something changed, and redraws while a hold button is held (0.8 s at most), so
  the second window costs the game next to nothing.
- Burn-in (AMOLED): pure black background; after 10 s without a touch the panel dims to faint outlines (the
  Preparing panel: after 30 s, to the percentage alone); the whole layout moves by a few pixels each minute. The touch
  that wakes a dimmed panel presses pad buttons but fires no action.

### The setting « Bottom screen »

`GameStore.bottomScreen(ctx)` / `GameStore.setBottomScreen(ctx, GameStore.BottomScreen.PANEL | BLACK | OFF)`:
the panel (default), a black screen (an empty black Presentation, hides what the system shows there), or nothing.
Stored in `<files>/settings/bottom_screen` rather than in the game SharedPreferences: the launcher writes it while
the game's process may be alive, and SharedPreferences caches per process; each process reads the file at its next
`onStart`. The Settings row is the launcher's (another change): show it only when
`DualScreen.available(activity, displayId)` is true, and call `setBottomScreen` on change.

### In the game

```
  fps · frame time              battery                  thermal
  [ Back  ]   [Launcher] [Screenshot] [ Perf ]   [ White ]
  [ Start ]   [Share log] [  Menu  ] [ Quit  ]   [ Black ]
                                     (hold)
               (Perf details)       nothing to touch bottom-centre (palms)
```

- **Back / Start / White / Black**, in the edge columns next to the thumbs: duplicates of Select, Start, R1 and L1
  (`xinput_device.c` maps L1 to Black and R1 to White), there so the player finds them. Each pointer is tracked on its
  own (two thumbs on two buttons work, sliding from one to the next works); everything is released on
  `ACTION_CANCEL`, when the panel is dismissed or detached, and in `ShellActivity.onStop`.
- **Launcher** opens the launcher (the game waits in the background, as with Home). **Menu** opens or closes the
  [in-game menu](#in-game-menu) on the top screen. **Quit** needs a hold of 0.8 s (it fills in red), then ends the game
  (`recomp_quit(0)`). **Screenshot** saves the last frame as
  `<external files>/screenshots/<date>.bmp` (24-bit BMP of the frame shown, `xbox_FramebufferDumpBmp`: 640 x 480,
  or the GPU renderer's own size with an internal resolution above 1×, e.g. 1440 x 1080 at Fit the screen on a
  1080p display) and says « Saved … » in the status strip. **Perf** shows more figures on the panel (draws per frame,
  battery temperature); it does not draw over the game. **Share log** dumps this process's logcat (the runtime's
  stdout and stderr go there) to `<files>/logs/game.log` and opens the share sheet (`LogProvider`,
  `content://<app id>.logs/game/game.log`).
- Status strip, once a second: the fps the window computes for its title bar (`xbox_FramebufferWindowGetStats`) and
  the matching frame time, the battery (`BatteryManager`), the thermal status (`PowerManager.getCurrentThermalStatus`:
  Cool, Warm, Hot…, coloured from moderate on).

The buttons reach the pad through the touch overlay (`src/input/input_overlay.c`):
`xbox_InputOverlaySet(port, pad)` stores a whole pad under a mutex; the SDL backend's `xbox_InputGetState` merges it
into port 0 (`xbox_InputMap_PadMerge`, after the keyboard), and with no gamepad attached it stands in for one
(success on port 0 while the panel is up). Each change bumps the packet number. JNI:
`android/app/src/main/cpp/companion_jni.c` (`CompanionNative`: `nativeOverlay`, `nativeOverlayOff`, `nativeStats`,
`nativeScreenshot`, `nativeQuit`). Test: `tests/input_overlay` (Linux, fake SDL: merge over a pad, release, no pad,
keyboard stand-in, a concurrent writer).

### Focus: Plan A and Plan B

A second window must not take the gamepad away from SDLActivity: on Android, keys go to the focused window, and
touching a focusable window on another display can move the focus there.

- **Plan A** (default): the Presentation is `FLAG_NOT_FOCUSABLE` (set before `show()`). Touch still works; the focus
  never leaves the game.
- **Plan B**: the panel's window is focusable and hands every key and motion event it receives to ShellActivity
  (`dispatchKeyEvent` / `dispatchGenericMotionEvent`, which SDL's surface handles as usual); ShellActivity ignores
  losing the focus while the panel is up (logcat `shell: focus went to the companion panel, ignored`). Switch it on
  without a rebuild with `RECOMP_COMPANION_FOCUS=forward` in `recomp.cfg` (wins), or `GameStore.setCompanionFocusPref`.
  The launcher's panel always uses Plan A.

logcat at each show: `shell: companion panel on display 2 "…" … (not focusable)` or `(focusable, keys forwarded)`.

### In the launcher (wiring)

`LauncherCompanion` does it in a few calls, wired in `LauncherActivity` like this (the panel's one action is the
main screen's: Play goes through the launcher's own `onPlay`, so a Prepare still comes first):

```java
private LauncherCompanion companion;

// onCreate, after buildStage() (not on the early launchShell() return):
companion = new LauncherCompanion(this, this::onPlay);
companion.libraryActions(() -> libraryPrimary(focusedEntry()));

@Override protected void onStart() { super.onStart(); if (companion != null) companion.start(); }
@Override protected void onStop() { if (companion != null) companion.stop(); super.onStop(); }

// where Home is built (its state, whether its button can run, and its label: Play, Prepare, …):
companion.home(title, statusText, statusColor, !play.dim, play.title);
// on the library, for the focused card:
companion.library(e.title(), chipText, chipColor, art, primaryLabel(e));
// where the Preparing screen is built:
companion.preparing(titleId(), gameTitle(dir));
// on every other screen:
companion.artOnly();
```

Home and the library show one panel (`LauncherPanel`): pure black, the game's art (440 px), its title, one pill
(Play, Prepare, Import…; y 716–844, the bottom centre left empty for the palms) and the battery small in the corner;
no card, status or other shortcuts. The other screens show the art alone. Preparing shows the overall percentage
big, one bar, « Compile · about 12 min left », the title and **Hold to cancel** (it sends `PrepareService`'s cancel
itself); it reads `status.json` once a second. Buttons are borderless pills (the in-game panel's too, rounded).

### Tests and screenshots

`DualScreenTest` (chooser over one, two, private and off displays; decisions; focus mode), `CompanionPanelTest`
(multi-touch, slide, cancel, detach, taps, hold-to-quit, dimming, shift; through a fake sink), `CompanionScreenTest`
(Robolectric `ShadowDisplayManager.addDisplay` / `removeDisplay`: shown, moved, dismissed, Off / Black, `onStop`,
Plan A not focusable and Plan B focusable and forwarding, the launcher's panel switch), `LauncherPanelsTest`. The
panels are rendered at 1240 x 1080 to `build/screenshots/companion-*.png` (`--target app-unit-tests-out`).

### Device check (dual screen), for Alex

On the Thor, with the shell APK of this change (and the stub or a title for steps 5 to 9):

1. Start the app: the launcher on the top screen. Until the launcher wiring lands the bottom screen shows the system's
   own content; after it, the launcher panel (the art, the title, one Play pill). logcat
   `launcher: displays:` lists both screens: send those lines (id, name, flags, size, refresh).
2. Start the game: the bottom screen turns to the in-game panel (black first, no white flash). logcat
   `shell: companion panel on display N … (not focusable)`. Photo of both screens.
3. **Focus, Plan A**: while playing with the built-in pad, touch the bottom screen anywhere (an empty spot, then
   Perf): the pad keeps driving the game, no pause, no `onWindowFocusChanged(): false` in logcat.
4. **Focus, Plan B**: `RECOMP_COMPANION_FOCUS=forward` in recomp.cfg, restart the game. Repeat 3: the pad still drives
   the game; logcat may show `focus went to the companion panel, ignored`. Say which plan behaves better (keys lost,
   doubled, a pause, the sticks).
5. **Buttons**: hold White on the panel (the game sees R1's action), then Start; both thumbs at once (White + Back).
   Slide a thumb from Back to Start. Same results as the physical L1 / R1 / Select / Start.
6. **Stuck keys**: hold White, and while holding press Home (or pull the notification shade, or let the screen turn
   off). Come back: White is not held. Same with two fingers on the panel and a palm landing on the screen.
7. **fps**: logcat prints `shell: fps N, M draws, companion shown|off` every 10 s. In the same scene, a minute with
   the panel (Perf on, so it redraws every second), then set Bottom screen to Off
   (`adb shell run-as <app id> sh -c 'mkdir -p files/settings; echo OFF > files/settings/bottom_screen'`, Home and
   back to the game) and a minute again. Expected: no measurable difference. Send both sets of lines.
8. **Dimming**: leave the panel 10 s: it dims to faint outlines; touch it: it wakes, and that first touch does not
   fire Launcher or Screenshot. Watch for a minute: the layout moves by a few pixels.
9. **Actions**: Screenshot (the strip says « Saved … »; `adb shell ls /sdcard/Android/data/<app id>/files/screenshots`,
   pull the BMP), Share log (share sheet with game.log), Perf, Launcher (the launcher comes up on top, the game
   waits), Quit: a short tap does nothing, a 1 s hold quits (`clean exit`).
10. **Off and Black**: `echo BLACK` / `echo OFF` into `files/settings/bottom_screen` as in 7 (`echo PANEL` to come
    back): Black keeps the bottom screen black, Off leaves the system's content there, nothing else changes.
11. **Single screen**: on a one-screen device (a phone): no panel, no error, logcat `displays:` lists one display.

## In-game menu

A drawer that slides in from the left over the game (Eden / BananaNative style) with XRecomp's own options: resume,
settings (with share log), controls, screenshot, back to the launcher, quit. `GameMenu.java` (the drawer and its
input), `MenuTriggers.java` (the edge swipe and the gamepad combo as plain state machines), `MenuNative.java` /
`android/app/src/main/cpp/menu_jni.c` (hold, volume, quit), wired in `ShellActivity`.

```
  ┌──────────────────────┐
  │ <game title>         │   the game's frame, dimmed,
  │ Paused · 0:42 played │   stays behind (held, not redrawn)
  │                      │
  │ Resume               │                        59.9 fps · 16.7 ms
  │ Settings             │                        (performance overlay,
  │ Controls             │                         also with the menu closed)
  │ Screenshot           │
  │ Back to launcher     │
  │                      │
  │ Quit game            │
  │ (A) Select (B) Resume│
  └──────────────────────┘
```

**How it is put together.** A View layer (`FrameLayout`: a dimming scrim, then the launcher's `Widgets.Stage` with the
drawer, the focus `Glide`, the performance overlay and a message line) added over SDL's layout with
`addContentView` in `ShellActivity.onCreate`. SDL's surface is never recreated or resized. The drawer is a flat sheet
flush with the left edge, 560 of the 1920 design pixels (29 %), over a light scrim: the game's title, « Paused · 0:42
played », text-only 88 px rows (no icons, no subtitles; Quit stands apart), the gliding focus, its sounds and haptics
(`UiFeedback`); it slides in in 200 ms (decelerating) while the scrim fades in. The Stage scales it to the screen
less the display cutout, so 720p and 1080p look the same.

**Opens by** (logcat `shell: menu open (<why>)`):

| Trigger | Notes |
|---|---|
| Android Back (the navigation bar, the back gesture, a keyboard) | Opens; Back again (or B) closes. A Back key **from a gamepad** SDL reads (`SDLControllerManager.isDeviceSDLJoystick`) is that pad's Select for the game, as it always was (SDL maps `KEYCODE_BACK` from a pad to its Back button), so it does not open the menu. |
| Guide / Mode (`KEYCODE_BUTTON_MODE`) | The Xbox pad has no such button, so no game uses it: the menu takes it whole. Many handhelds keep it for the system; then it never reaches the app. |
| Select + Start held 0.5 s (the « Game menu » [hotkey](#hotkeys): another chord, one button, or none) | Both still reach the game while held (each alone is a game button: Back and Start); when the menu opens, the game gets their release. Xbox games read Back and Start each on its own, so holding both half a second is free. |
| A swipe in from the left edge (top screen) | Down within 3 % of the width from the left edge, then 6 % of the width to the right, mostly sideways. The game sees the touch begin and gets an `ACTION_CANCEL` when the swipe is recognised. With Android's gesture navigation the same swipe may also be the system's back gesture: a Back within 0.6 s of a swipe-open is ignored, so the menu stays open. |
| « Menu » on the companion panel (second screen) | Opens or closes it. |

**While open.**

- **The game is held**: `MenuNative.nativeHold(true)` → `recomp_lifecycle_pause_for(RECOMP_HOLD_MENU)`, the same
  hold as the background ([Background and resume](#background-and-resume)): the flip parks, the title's threads stop
  behind it, the audio device pauses at once. The window keeps showing the last frame, dimmed. Closing calls
  `nativeHold(false)`. The two reasons are kept apart, so Home with the menu open, then back, leaves the game paused
  under the menu.
- **Input goes to the menu only**: `ShellActivity.dispatchKeyEvent` / `dispatchGenericMotionEvent` /
  `dispatchTouchEvent` ask `GameMenu` first, and what it takes never reaches SDL. D-pad, hat and left stick move the
  focus (left / right step a setting), A selects, B or Back goes back (closes from the main page), Start or Guide
  resumes. A, B and Start act on their **release**, and only for a press that went down while the menu was open: the
  release of the key that opened it does nothing, and the key that closes it never reaches the game. Volume keys stay
  the system's. Touch: tap a row, tap the dimmed game to close.
- **No stuck buttons**: when the menu opens it hands SDL the release of every key the game saw go down (`KeyEvent`
  copies, sent straight to `SDLActivity.handleKeyEvent`), a centred event for every joystick that moved (sticks,
  triggers and hat at rest, to SDL's surface), and releases the companion panel's buttons (`input_overlay`: still on,
  nothing held); it releases the panel's again when it closes.

**Items.**

| Item | Does |
|---|---|
| Resume | Closes the menu. |
| Save state, Load state | Only for a GBA, PS1 or N64 game whose runtime can save states (XBA-182, XBA-183: `Host.statesSupported`: `Console.hasSaveStates`, then `MenuNative.nativeStatesAvailable`); an Xbox game does not show them. Each opens a page of four slots, « Slot 1 » to « Slot 4 », then Back. Under each slot: « Empty », or when it was saved (the time alone for today, « 14:32 »; the date and the time for another day, in the device's format), and its thumbnail at the right of the row. The slots are read each time the page shows, so a fresh save is there the next time. **Save state**: choosing a slot saves to it (over what it held), closes the menu and says « Saved to slot N » at the top of the screen. **Load state**: an empty slot is dimmed; choosing it says « Slot N is empty » and the menu stays; a saved one loads, closes the menu and says « Slot N loaded ». A failure closes the menu with a warning that names it: the slot holds another game's state, it was saved by another version of the app, it is damaged, it cannot be written or read. B (or Back) returns to the main page on the row that opened the page. |
| Fast-forward | Same condition: On / Off, a setting row (A or left / right toggles it, the menu stays open). On, the game runs faster until it is turned off. The pad chord **Back + RT, held** does the same while held by default ([Hotkeys](#hotkeys); Controls lists it for a GBA, PS1 or N64 game). |
| Cheats | A GBA, PS1 or N64 game (XBA-197): the game's cheat codes, each On / Off, Add a code (a keyboard dialog), Delete a code (A twice); a change takes effect at once ([Cheats](#cheats)). |
| Quick save / load (pad chords) | Same condition, without opening the menu: by default **Back + RB** saves and **Back + LB** loads the quick slot, the last slot saved or loaded (at the start, the most recently saved, else slot 1); the player's [hotkeys](#hotkeys) may move them and add next / previous quick slot. The runtime reads the chords itself; the menu polls `MenuNative.nativeStateEvent` every 250 ms while the game shows and says « Saved to slot N », « Slot N loaded », « Quick slot: N » or the error at the top of the screen. Controls lists the hotkeys. |
| Resume where I left off | Same condition: Settings › « Resume where I left off », On by default (`files/settings/auto_resume`). On, the menu saves the **automatic slot** (`auto.st`, slot 0) when the player leaves the game: Quit (its confirmation then says « Your place is kept for next time »), Back to launcher, Home or the screen off (`GameMenu.onStop`). At the next start `ShellActivity` sets `RECOMP_STATE_RESUME=1` and the runtime loads it before the first frame; the menu says « Resumed where you left off » (or that it could not, and the game starts from the beginning). Load state lists it first as « Where you left off » when there is one; Save state never offers it. The game's Home then shows **Resume** (that point) first and **Start** beside it: Start launches once from the console's boot (`files/settings/fresh_start`, deleted by the game process as it reads it, so Restart and the next launch resume again); without a saved point the Home shows Play. |
| Settings | **Perf overlay** On / Off: fps and frame time (`xbox_FramebufferWindowGetStats`, twice a second) in the top right corner, in the menu's layer, not in the game frame; stays with the menu closed. **Volume** 0–100 % in steps of 10, A mutes / unmutes: one gain on the mixed APU output (`apu_volume.c`, the square of the fraction, after the WAV capture), set through JNI. **Bottom screen** Panel / Black / Off (only with a second screen): `GameStore.setBottomScreen` and the companion is refreshed at once. **On-screen pad** Shown / Hidden (only when the game has one, XBA-158): for this session, whatever Controls › On-screen pad › Show says (never with the companion panel up). These apply **now** (the rows say so) and are remembered (`files/settings/perf_overlay`, `game_volume`, `bottom_screen`); the volume is applied again at the next start. **Language** (only for an Xbox game whose XBE gives a title ID): As in Settings, then the nine languages; this game's own [language](#game-language), applied at the next start. **Widescreen** and **480p** (same condition, XBA-134): As in Settings / On / Off, this game's own console video setting (`GameVideo.java`, kept in `files/settings/widescreen_<TITLEID>` and `480p_<TITLEID>`), applied at the next start: `ShellActivity` sets `RECOMP_WIDESCREEN` / `RECOMP_480P` to `1` or `0` (the environment wins over the `recomp.cfg` line of Settings › Graphics) and unsets them when the game follows Settings; logcat « shell: title video RECOMP_WIDESCREEN=… RECOMP_480P=…, title … ». A GBA, PS1 or N64 game (XBA-181) has none of these three: **Scale** and **Filter** instead, As in Settings then each value, this game's own ([Settings per console](#settings-per-console)), shown at once. Every game with a title ID (Xbox too, XBA-203) has **Picture shader** (XBA-206): As in Settings (its value), None, Sharp, Scanlines, CRT, LCD, this game's own `RECOMP_SHADER` in its [Settings per game](#settings-per-game) file, the same value as the Game settings page's row, shown at once. Language, Widescreen, 480p, Resolution and Vulkan driver are read at the start: **Restart to apply** shows at the top of the page once one differs from what the game started with ([Live picture and Restart to apply](#live-picture-and-restart-to-apply)). Perf overlay, Volume and Screenshot are the same rows (the native side backs them for the consoles through the same JNI calls). An Xbox game's screen filter and scaling are not offered: the SDL window's are fixed at start (`fb_present_sdl.c`: nearest, logical size 640 x 480). |
| Controls | The usual mapping, compact (plus Back and the menu chord, Select + Start unless the [hotkeys](#hotkeys) say otherwise, and for a GBA / PS1 / N64 game each hotkey set); with a remapping in `recomp.cfg`, first « Your mapping » and each changed control (your button, the Xbox control). A GBA / PS1 / N64 game: each of its buttons and the controls it comes from ([Controls for the other consoles](#controls-for-the-other-consoles)). |
| Screenshot | Closes the menu first, then saves the game's last frame as `<external files>/screenshots/<date>.bmp` (`xbox_FramebufferDumpBmp`, the companion panel's), and says « Saved … » at the top of the screen (and on the panel). |
| Change disc | Only for a PlayStation game with two discs or more found beside it (XBA-166, [PlayStation games](#playstation-games-xba-168)): a page of its discs, « In the drive » under the one playing; choosing another closes the menu and the lid opens, a second later the new disc is in. |
| Settings › Share log | This process's logcat to `files/logs/game.log`, then the share sheet (the companion panel's `LogProvider` path). The menu stays open. |
| Back to launcher | **The game stays alive** (the runtime maps its guest memory once per process: starting it again means a new process), held: the drawer goes at once, the launcher comes up (`ACTION_SHOW_LAUNCHER`), and the menu's hold is let go only in `onStop`, after SDL's pause has put the background hold in place, so no frame runs in between. The launcher's Play brings this task back and the game goes on (`launcher: game running, bringing it back`). In a game's own app the row is **Back to the home screen**: the task goes behind Android's home screen (`moveTaskToBack`), held the same way; its icon brings it back. |
| Quit game | The first press turns the row into « Press again to quit » (with « Progress since the last save is lost » under it, the only row that says more) for 4 s (moving away cancels it); the second press quits: `nativeQuit` lets the guest go, `sync()`s the saves, then `recomp_quit(0)` ([Quit](#quit)). |

**Save states and fast-forward, native side (XBA-182, XBA-183).** `menu_jni.c` calls the shared interface
`src/platform/recomp_states.h`: `nativeStatesAvailable` (a GBA, PS1 or N64 runtime is attached with a save folder),
`nativeStateSave / Load(slot)` (the runtime saves / loads between frames under its frame lock; the menu holds the game
then, so it never waits for a frame), `nativeStateTime / Thumb(slot)` (the slot file's header and thumbnail: 0 / null
for an empty, damaged or other-version slot), `nativeFastForward(on)` / `nativeFastForwardOn` (the toggle; the Back +
RT chord is read by the runtime itself). Slots are `<internal files>/save/<console>-<KEY>/state1.st` .. `state4.st`,
beside the game's saves; a slot from another game is refused by the core when loaded (« holds another game's
state »), as is one written by another build of the app (« saved by another version »). Formats, tests and the
desktop keys: [consoles/README.md « Save states and fast-forward »](../../consoles/README.md). Logcat: « gba: state slot
N saved (… bytes, frame …) », « ps1: state slot N loaded », « n64: state slot N loaded », « gba: fast-forward on at
frame … ». An N64 state is 1 to 10 MB (RDRAM's all-zero pages are left out); the Vulkan RDP's upscaled picture
(XBA-185) is not in it: after a load what the game draws again is upscaled again, a picture it never redraws shows at
1x until then.

The header shows the game's title (the launcher's choice) and « Paused · h:mm played », the time since the game
started (this process). Hints: A Select, B Resume on the main page, B Back on the other pages.

**Tests and screenshots.** `GameMenuTest` (Robolectric, a fake `GameMenu.Host` standing in for libmain and SDL): Back,
a gamepad's Back, Guide, Select + Start timing (0.4 s no, 0.55 s yes, released early no), the edge swipe and what is
not one, the panel's Menu; nothing reaching the game while open; the releases on open (keys, a centred joystick
event, the overlay) and on close; B acting on release; stick and hat navigation with repeat; Back to launcher holding
until `onStop`; Quit's confirmation and its timeout; the quick settings by pad (live, remembered, applied at start);
the slide; the drawer's width; the save state rows (only with states), the slot pages (« Empty » or the time, the
thumbnail, the dimmed empty slot on Load), saving and loading with each error message, Fast-forward's toggle, Back to
the opener, the Fast-forward line in a GBA game's Controls. Rendered over a made-up game frame to `build/screenshots/menu-*.png`: closed, main,
quit armed, quick settings, controls, closed with the overlay and a message; en and fr at 1080p and 720p; and the
save slots in English at 1080p (`menu-save-slots.png`) (`--target app-unit-tests-out`). C: `tests/recomp_lifecycle` (the reasons) and `tests/apu_volume`.

### Live picture and Restart to apply

The picture keys (`ConsoleVideo.LIVE_KEYS`: `RECOMP_SCALE`, `RECOMP_FILTER`, `RECOMP_SHADER` and its strength,
curvature and ghosting) take effect while the game runs. After each change in the menu (Settings or Game settings),
when the menu opens and when the game comes back from the launcher, `GameMenu` resolves the picture
(`ConsoleVideo.livePicture`: the game's value, else recomp.cfg's, else the runtime's default; the GBA's filter
nearest, the others linear) and, when it differs from the one shown, hands it to `MenuNative.nativeSetPicture`:

- a GBA, PS1 or N64 game: `host_window_request_picture` (`src/video/host_window.c`), any thread; the presenting
  thread takes it at its next present (`live_take`), so the picture never changes in the middle of a frame. While
  the menu holds the game no frame is presented: the change shows as the menu closes.
- an Xbox title: `xbox_FramebufferWindowSetShader` (`src/video/fb_present_sdl.c`); the window's loop takes it at its
  next turn and draws the shown frame again, so it shows at once, behind the open menu too. LCD is drawn as none
  (no pixel grid for an Xbox frame); a preset whose shader failed to build stays off.

Logcat: « shell: menu: picture applied live (menu): scale integer, filter nearest, shader crt (…) », then
« [HOST] window: picture changed live: … » or « [FBWIN] picture shader changed live: … ».

Everything else is read once at the start (the language and the console video settings the Xbox kernel answers
with, which a title asks for once; the internal resolution, whose GPU buffers are made at that size; the Vulkan
driver, loaded in the process; most Game settings). `GameMenu` keeps what the game started with of these (its
environment, `GameSettings.runEnv`, without the picture keys, an unset key as recomp.cfg has it, and an N64 game's
Vulkan driver) and compares after each change: while they differ, **Restart to apply** (« Some changes are waiting
for it ») is the first row of Settings and Game settings; going back to the starting values hides it. A first press
arms it (« Press again to restart », and « The game picks up where you are » when Resume where I left off applies,
else « Progress since the last save is lost »), a second within 4 s saves the automatic slot (GBA / PS1 / N64) and
asks the launcher to start the game again in a new process (`ShellActivity.restartGame`, the title's quick reboot
path: `LauncherActivity.ACTION_RESTART_GAME`).

**Rows in the narrow drawer.** A setting row whose label and widest choice do not fit side by side is stacked
(`Widgets.MenuRow`): the label whole on its line, the value under it, the arrows at the right edge; a setting's
« This game » badge sits left of its value (or after it when stacked) instead of hiding it. The list is clipped
between the page title and the hints when it scrolls.

### Device check (live picture, Restart to apply), for Alex

1. A GBA game › menu › Settings: every row's name reads whole (Scale, Filter, Picture shader, Game settings), in
   French and in English.
2. Scale → Integer, close the menu: the picture is integer-scaled at once. Filter → Linear: soft at once, Nearest:
   sharp again.
3. Picture shader → CRT, close the menu: CRT at once. LCD: the grid at once. As in Settings: back to Settings' shader.
4. An Xbox title › menu › Settings › Picture shader → Scanlines: the scanlines show behind the open menu.
5. Same title: Language → another language: « Restart to apply » appears at the top. Back to the first language: it
   goes away.
6. Language changed again, press « Restart to apply »: it asks to press again; press again: the game starts again in
   the new language.
7. An N64 game: Resolution → 2x: « Restart to apply »; press twice: the game comes back where it was (with Resume where
   I left off on) at 2x.
8. A long Settings list (N64 game, 720p device): scroll to the bottom; no row draws over « Paused · 0:42 played » or
   over the A / B hints.
9. Launcher › a game › Game settings › Picture shader → Sharp while the game waits in the background; Play: the game
   shows Sharp at once.

### Device check (in-game menu), for Alex

With the shell APK of this change and the stub or a title (and the Thor for 5 and 9):

1. **Back**: in the game, press Android Back (the navigation key or the back gesture). The menu slides in from the
   left over the dimmed game, Resume focused. logcat `shell: menu open (Back)`, `lifecycle: paused (menu)`. Back
   again: it slides out, `shell: menu closed`, `lifecycle: resumed after N ms`.
2. **Paused, with sound**: open the menu during a scene with music or motion: the picture freezes and the sound
   stops at once; Resume: both come back where they were. Then open the menu, press Home, wait 10 s, come back: still
   paused under the menu (`lifecycle: still paused (menu)`), Resume continues.
3. **Swipe**: swipe from the very left edge of the top screen to the right: the menu opens (`menu open (swipe)`), and
   it stays open even if the system's back gesture fires with it. Tap the dimmed game: it closes.
4. **Combo and Guide**: hold Select + Start: after about half a second the menu opens (`menu open (Select+Start)`);
   press each alone in the game: the game still gets them. If the pad has a Guide / Home button the app receives,
   it opens the menu (`menu open (Guide)`); say whether the Thor's does. Also say what the Thor's own Back button
   does: opens the menu, or acts as Select in the game (logcat shows which).
5. **Bottom panel**: tap « Menu » on the companion panel: the menu opens on the top screen; tap it again: it closes.
6. **No stuck buttons**: hold A (or a direction, or the right trigger) in the game and open the menu with Back while
   holding; let go inside the menu, close it: nothing is held in the game. Same while holding White on the bottom
   panel. Close the menu with B and with A on Resume: the game does not get that B or A.
7. **Navigation**: D-pad, hat and left stick move the focus (held: it repeats); A selects; B goes back from Settings
   and Controls; Start resumes from anywhere.
8. **Settings**: Perf overlay On: the fps and frame time show in the top right corner, also after Resume
   (compare with the companion's fps). Volume: left / right change the game's volume in steps (audible after
   Resume), A mutes; quit and start again: the volume is remembered. Bottom screen (Thor): Panel / Black / Off switch
   the bottom screen at once.
9. **Screenshot**: Screenshot closes the menu and « Saved <date>.bmp » shows at the top;
   `adb shell ls /sdcard/Android/data/<app id>/files/screenshots`, pull the BMP: the game's frame, without the menu.
10. **Settings › Share log**: the share sheet opens with game.log; cancel it: the menu is still there, the game still paused.
11. **Back to launcher**: the launcher comes up and the game waits (no sound; logcat `paused (menu)` then
    `still paused (background)` and no `resumed` in between). Play: the game is back where it was, running
    (`launcher: game running, bringing it back`).
12. **Quit**: Quit game once: the row asks « Press again to quit »; wait 4 s: it goes back to « Quit game ». Press it
    twice: `shell: menu: quit (confirmed)`, `quit from the in-game menu`, `clean exit`; start the game again: the
    saves are there.
13. **French**: with the device in French, every row, hint and message of the menu is in French. Photos of the menu
    (main page, Settings) at least once, in French.
14. **Notch / 720p**: on a phone with a cutout, the drawer's text stays clear of it; on a 720p screen the menu looks
    the same as at 1080p.

### Device check (game language, XBA-77), for Alex

With a title from a European (multi-language) disc, Fable's PAL disc for instance:

1. **Same as Android**: device in French, Settings › Game › Language on « Comme Android » (the default): start the
   game. logcat `shell: game language fr, title 4D5300D1`; the game's menus are in French.
2. **Settings**: Settings › Game › Language → English; quit the game and start it again: logcat
   `shell: game language en (recomp.cfg)`, menus in English.
3. **This game**: in the game, in-game menu › Settings › Language → Deutsch; quit and start again: logcat
   `shell: game language de`, menus in German (and the Settings line still says English).
4. **Back to Settings**: same row → « Comme les Réglages »; next start: English again.
5. **US disc** (if you have one): choose French: the game stays in English (its disc has nothing else), nothing
   breaks.
6. Say which languages Fable's disc offers, for the profile notes.

### Device check (this game's widescreen and 480p, XBA-134), for Alex

1. Settings › Graphics › Widescreen On. Start a game: logcat `shell: title video RECOMP_WIDESCREEN=(Settings)
   RECOMP_480P=(Settings), title …` and the runtime's `recomp.cfg: … RECOMP_WIDESCREEN=1`.
2. In-game menu › Settings: the rows **Widescreen** and **480p** read « As in Settings ». Widescreen → On → Off;
   quit and start again: logcat `RECOMP_WIDESCREEN=0`, `recomp.cfg: … RECOMP_WIDESCREEN (kept from the
   environment)`, the game in 4:3 although Settings says On.
3. Another game: its rows still read « As in Settings », and it starts wide.
4. Back in the first game: Widescreen → « As in Settings »; next start: wide again.

## Full screen

The game window hides the status bar and the navigation (gesture pill); a swipe from an edge shows them for a moment
(`SystemBars.java`, XBA-99). SDLActivity hides them with the old `systemUiVisibility` flags, which a focus change
clears: on the AYN Thor the companion Presentation on the second display took the focus, the bars came back and SDL's
surface was 1920x970. `ShellActivity` applies `WindowInsetsController.hide(systemBars())` with
`BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE` and the cutout mode `shortEdges` in `onCreate`, `onResume`, on every focus
gain, and 300 ms after `onStart` (once the companion screen is up).

Device check (Alex, Thor): 1. start a game with the bottom screen on « Panel »: no status bar, no pill, logcat
`SDL: ... 1920x1080`; 2. swipe up from the bottom edge: the pill shows, then hides again; 3. open and close the in-game
menu, then Home and back to the game: still full screen.


Frame times (XBA-29): every 10 s the presenter logs `[FBWIN] frame times over 10 s: N presents, mean X ms (F fps), sd
S ms, worst W ms` (intervals over 250 ms, a pause or a load, are left out). Device check on the Thor's 120 Hz screen:
a title at 30 fps and one at 60 fps each show sd under 2 ms in-game.

## Background and resume

(`src/platform/recomp_lifecycle.c`, XBA-24.) On Android SDL already blocks the thread that pumps its events while the
app is in the background, and pauses its audio there. The title's own threads would keep running, so:

- an SDL event watch (in the title's window, `fb_present_sdl.c`, and in the shell) calls `recomp_lifecycle_pause()` on
  `SDL_APP_WILLENTERBACKGROUND` and `recomp_lifecycle_resume()` on `SDL_APP_DIDENTERFOREGROUND`;
- the flip (`xbox_FramebufferWindowPresent`) holds there while paused, on a condition variable: no CPU. The title's
  other threads then stop behind it, as they wait for the GPU;
- the SDL audio device is paused by a listener at once, not at the next flip, and plays its queue on at resume;
- if the GL context was lost (`SDL_RENDER_DEVICE_RESET`), the window remakes its texture from the last frame;
- logcat shows `lifecycle: paused (background)` and `lifecycle: resumed after N ms`.
- the pause has reasons (`RECOMP_HOLD_BACKGROUND`, `RECOMP_HOLD_MENU`: `recomp_lifecycle_pause_for` /
  `resume_for`): the guest stays held while any is set, so the [in-game menu](#in-game-menu) open across a trip to
  the background keeps the game paused when the app comes back (`lifecycle: still paused (menu)`). Listeners (the
  audio device) see the first reason set and the last one cleared. Test: `tests/recomp_lifecycle`.

Guest time is not frozen: after a long pause the title sees the time that passed, as after a long frame. Most titles
clamp their frame time; if one does not, that becomes its own ticket.

Device check (XBA-24), shell part (the in-game part waits for a title, XBA-35):

1. Start the shell, press Home. logcat: `lifecycle: paused (background)`.
2. `adb shell top -b -n 1 | grep xboxrecomp`: under 2 % CPU.
3. Wait 5 minutes, bring the app back. logcat: `lifecycle: resumed after ~300000 ms`; the black screen is back, no
   crash, no tombstone.
4. Do it once more with a notification pulled down and with the screen turned off and on.
5. Back: the in-game menu; Quit game twice: `clean exit`.
6. Send the logcat lines and the `top` line.

## Fast cores (big.LITTLE)

Right after `recomp.cfg`, the shell calls `recomp_host_cpus_init()` (`src/platform/host_cpus.c`, XBA-25). It restricts
the main thread to the cores whose cpufreq maximum is above the slowest cluster's, before any other thread exists, so
every thread inherits that mask: guest threads, the rasteriser pool, SDL's. On the Thor (Snapdragon 8 Gen 2) that means
cpus 3-7 and not the three A510 cores. logcat: `host cpus: runtime on the fast cores 3-7 of 0-7 (...)`.
`RECOMP_BIG_CORES=0` in `recomp.cfg` turns it off. On desktop it is off unless `RECOMP_BIG_CORES=1`.


After a pass through the background (XBA-50): Android moves the app between cpusets (top-app, background), and on
kernels before 6.2 (the Thor runs 5.15) that move overwrites the mask each thread set for itself and does not put it
back. The runtime's threads note their mask when they start (the shim's CreateThread, the raster pool, the guest
threads' one-core pin, the main thread) and `recomp_lifecycle_resume` sets them again
(`recomp_host_cpus_restore`, one logcat line `host cpus: N runtime thread(s) back on their cores after resume`).
Threads that are not ours (ART, binder, SDL's) are left alone.

Device check (Alex, Thor): 1. start a game, note the `host cpus` line; 2. `for t in /proc/PID/task/*; do taskset -p
${t##*/}; done`; 3. Home, 10 s, back; logcat shows the `back on their cores` line and step 2 gives the same masks
as before.

## Fault handler (trapped MMIO and real crashes)

Before SDL starts, the shell calls `xbox_PosixInstallMmioFaultHandler()` (XBA-22), and calls it again after
`SDL_Init`. The handler emulates guest accesses to trapped hardware registers (APU, AC'97, USB OHCI). Every other
fault goes to the handler that was there before, debuggerd (and ART in front of it). When the previous action is the
default one, the handler restores it and the instruction faults again, so a real crash gives the usual tombstone at
the right place. If something replaced the handler in the meantime, the second call puts it back in front and chains
to the newcomer.

Device check: `RECOMP_SHELL_CRASH_TEST=1` in `recomp.cfg` makes the shell write to NULL right after the ready line
(shell without a title, or without game data). Expected: the app closes and logcat shows
`RECOMP_SHELL_CRASH_TEST: writing to NULL now`, then `Fatal signal 11 (SIGSEGV)` with a backtrace that goes through
`SDL_main` in `libmain.so`. Remove the line afterwards. The shell no longer waits before the write: the line is there
because the handler drains the log pipes first ([Last lines before a crash](#last-lines-before-a-crash)).

## Self-test in the app process

(XBA-51.) The device test bundle ([device-tests.md](device-tests.md)) runs from `adb shell`: SELinux domain `shell`, no
ART, no libsigchain, an almost empty address space. An app runs as `untrusted_app` under ART, with libsigchain in front
of `sigaction` and hundreds of libraries mapped, so a pass in the bundle does not prove the app. With
`RECOMP_SHELL_SELFTEST=1` in `recomp.cfg`, the shell runs the runtime's risky pieces in its own process, right after
`SDL_Init` and the second fault-handler install, before the window or a title:

| Check | What it proves in the app |
|---|---|
| `layout` | `xbox_MemoryLayoutInit` on `selftest.xbe` (the APK asset, read with `SDL_LoadFile`; the layout takes the bytes, no copy on disk); the base is host-page aligned. |
| `window` | The whole 4 GB guest window is held (`RECOMP_GUEST_WINDOW`, on by default on Android) and `/proc/self/maps` covers all of it. On Android, a refused 4 GB reservation (the layout then falls back to fixed addresses) fails. |
| `mirrors` | A write and a read through each of the 28 RAM mirrors (memfd views, XBA-21), seen at the base, and the other way round. |
| `tiled_alias` | The same between the tiled aperture and the contiguous window (two views of the other memfd). |
| `mmio_handler` | The toolkit's SIGSEGV handler is still in front, and one host page of guest RAM traps (`PAGE_NOACCESS`, routed by `xbox_PosixMmioSetTestWindow` to a test device). |
| `mmio_imm`, `mmio_extend`, `mmio_regoff`, `mmio_writeback`, `mmio_unscaled`, `mmio_pair`, `mmio_simd` | One trapped access of each A64 decoder class through that page, by the real chain: libsigchain, the toolkit's handler, `mmio_decode_a64.h`, the register write-back into the signal frame. `mmio_simd` is `ldr q` / `str q` / `ldr d`: the V register written in the frame's `fpsimd_context`, and Zn in its `sve_context` when the thread has live SVE state (XBA-45, XBA-189). (On x86-64 Linux: `mmio_mov`, `mmio_extend`, `mmio_immstore`, `mmio_xmm_scalar`, `mmio_simd` = `movups`.) |
| `exec_alloc` | `NtAllocateVirtualMemory` with `PAGE_EXECUTE_READWRITE` succeeds, read / write, and the mapping is not executable. |
| `exec_guest` | `MmSetAddressProtect` with `PAGE_EXECUTE_READ` (read-only) and `PAGE_EXECUTE_READWRITE` on guest RAM (a memfd view) takes effect, without `PROT_EXEC`. |
| `save_dir` | A file created, written, `fsync`ed, read back and removed in the save directory. |
| `reinit` | Shutdown, a second init and shutdown again: a title maps its own layout after the self-test, in the same process. |

Each prints `selftest NAME: PASS` (sometimes with a note) or `selftest NAME: FAIL (reason)`, then
`selftest: N passed, M failed`. The shell then carries on as without the option (its window, or the title). The checks
never fault on purpose: whether an address is mapped is asked of the kernel (a pipe write or read returns `EFAULT`).
A trapped access the decoder cannot service is a crash, as in a title; the fault handler drains logcat first, so the
last `PASS` line says which check came next. The same code is the CMake test `tests/recomp_selftest` (CI on x86-64,
arm64 under qemu-user, and the device bundle), so a check that fails only in the app is an Android difference.
`mmio_simd` used to fail under qemu-user, which always puts an SVE record in the signal frame and restores Z from it:
the decoder now writes that record too (XBA-189), and the check is gated in the arm64 job like the others.

**No executable guest memory.** The POSIX Win32 shim (`prot_from_page` in `src/platform/win32_compat.c`) no longer maps
`PAGE_EXECUTE*` to `PROT_EXEC`: read / write are kept, `PAGE_EXECUTE` alone becomes `PROT_READ`. Guest code is
recompiled ahead of time and never runs from guest memory, and the runtime has no JIT, while an executable anonymous
or memfd mapping can be refused to an app (SELinux `execmem`). Every guest protection reaches the host through that
function (`VirtualAlloc`, `VirtualProtect`; file-mapping views were never executable), and `VirtualQuery` already
reported every mapping as `PAGE_READWRITE`.

### Device check (XBA-51), for Alex

1. Install the shell APK (**shell-latest**, « XRecomp shell »), push a `recomp.cfg` with `RECOMP_SHELL_SELFTEST=1`:
   `printf 'RECOMP_SHELL_SELFTEST=1\n' > recomp.cfg`,
   `adb push recomp.cfg /sdcard/Android/data/fr.adserver.xrecomp.dev/files/recomp.cfg`.
2. Start the app, then `adb logcat -d -s xboxrecomp`.
3. Expected: every `selftest` line `PASS` and the summary `selftest: 16 passed, 0 failed`; no tombstone
   (`adb logcat -d -b crash` empty).
4. Run it a second time after a reboot (the address space layout changes).
5. Send the logcat lines (both runs); remove the option (`adb shell rm
   /sdcard/Android/data/fr.adserver.xrecomp.dev/files/recomp.cfg`).

## A title in the shell

(XBA-52.) A title does not copy or replace `shell_main.c`. It defines one function, and the shell calls it:

```c
#include "platform/recomp_storage.h"
int recomp_title_main(const recomp_storage *store);   /* store->game_dir, ->save_dir, ->xbe */
```

1. `SDL_main` calls `recomp_app_start()` (`src/platform/recomp_app.c`): logcat, `recomp.cfg`, the Android defaults
   below, storage and `xbox_path_init`, fast cores, the fault handler. A title gets all of them by being linked in.
2. `SDL_Init`, the fault handler checked again, `xboxrecomp ready (build …, SDL …, title)`.
3. When `default.xbe` is found and a title is linked, `xbox_HostWindowMain(title_boot)`: the window on the main
   thread, `recomp_title_main(&store)` on a 64 MB guest thread, the lifecycle watch; quitting is the
   [in-game menu](#in-game-menu)'s. When the guest returns, the window stays on its last frame
   (`[FBWIN] guest returned (N); the window stays`): an app does not vanish on its own.
4. No title linked (the weak `recomp_title_main` is NULL) and `default.xbe` found: the title pack for its title ID
   (« Title packs » below), run the same way.
5. No title and no pack, or no game data: the shell's own black window, as before.

`templates/new-game` is such a title. Built through the app, its `CMakeLists.txt` sees `XBOXRECOMP_SHELL_TITLE` and
makes the object library `xba_title` instead of an executable (the toolkit is the shell's), and its `main.c` defines
`recomp_title_main` instead of `main`, taking the XBE path, `game_dir` and `save_dir` from `store` instead of the
`YOUR_GAME_*` constants. The rest of its boot (memory layout, kernel, bridge, `xbe_entry_point`) is unchanged.

```bash
# the title project: templates/new-game + its generated C in src/recomp/gen (docs/GETTING_STARTED.md)
cd android && gradle assembleDebug -PxbaTitleDir=/path/to/my_xbox_game \
    -PxbaApplicationId=fr.adserver.xrecomp.mygame -PxbaLabel="My game"
# or the NDK alone: cmake -S android/app/src/main/cpp -DXBA_TITLE_DIR=/path/to/my_xbox_game (+ the NDK toolchain)
```

### A title's own APK

The launcher with one game's name, its own package and **the game's title image as its icon**, locked to that disc
(`-PxbaXbeSha256`, [WRONG_GAME](#wrong_game)). One command from a host with the Android build environment
(`tools/docker/android.Dockerfile`):

```bash
tools/android/title_apk.sh /path/to/game            # a disc image, a folder with default.xbe, or default.xbe
tools/android/title_apk.sh /path/to/game --title-dir /path/to/my_xbox_game -- -PxbaVersionCode=2
tools/android/title_apk.sh /path/to/game --dry-run  # print the Gradle command
```

| Step | What |
|---|---|
| Identify | `tools.prepare.identify`: title ID, name (certificate), `default.xbe` extracted from an image. |
| Icon | `python -m tools.title_art default.xbe --android-res DIR`: the `$$XTIMAGE` texture (else `$$XSIMAGE`) decoded like `TitleArt.java`, as the foreground layer of an adaptive icon (432 px, the image centred at 72/108 of it, the part every launcher mask shows); the background is the average colour of the image's border. No title image: XRecomp's glyph on the title's colour (`TitleArt.monogramColor`). |
| Build | Gradle with `-PxbaApplicationId=fr.adserver.xrecomp.t<titleid>`, `-PxbaLabel=<name>`, `-PxbaXbeSha256=…`, `-PxbaIconRes=DIR` (+ `-PxbaTitleDir` with `--title-dir`, + everything after `--`). `-PxbaIconRes` goes into the build types' resource source sets, so its `mipmap/ic_launcher` overrides main's. |

Without `--title-dir` the APK runs the title from the pack its own Prepare builds (add `-PxbaToolchainDir=…` after
`--`). The extracted `default.xbe` and the icon go to `~/.cache/xrecomp/title-apk/<TITLEID>` (`--work`; the script
refuses a folder inside the repository): they come from the player's disc, like the APK itself, which is for their
own use and is not distributed.

### Title packs (the loader)

The one app runs any title from the pack the on-device Prepare built for it
(`<internal files dir>/packs/<TITLEID>/{libtitle.so, pack.json}`, [prepare.md](prepare.md)). Code:
`android/app/src/main/cpp/pack_loader.{c,h}` (forwarders, `xba_shell_abi`, `xba_pack_load`) and
`src/platform/recomp_pack.{c,h}` (title ID, `pack.json`, checks, dlopen; no Android in it).

**When.** In `SDL_main`, after `SDL_Init` and the self-test, when no title is linked into `libmain.so`
(`recomp_title_main` is NULL) and `default.xbe` was found: the title ID from its certificate (base at `0x104`,
certificate address at `0x118`, title ID at certificate + `0x08`), then `<SDL_AndroidGetInternalStoragePath()>/packs/
<TITLEID>/pack.json`.

**Checks** (the same as `tools/prepare/pack.py` `check` and the launcher's `Pack.check`, plus `abi`): every required
field, `format` 1, `title_id` = the XBE's, `abi` = `arm64-v8a`, `shell_abi` = the exported `xba_shell_abi`, `lib` a
plain file name, present, of `lib_size` bytes. **Not `lib_sha256`**: the pack sits in the app's private storage,
published atomically by prepare which hashed it, and hashing a 100–250 MB library costs about a second at every
start on a phone. `lib_size` catches a truncated file; an import from outside (below) is where the hash is checked.

**dlopen** with `RTLD_NOW | RTLD_LOCAL`. The pack has `DT_NEEDED libmain.so` (already loaded by SDLActivity: bionic
matches it by soname) and binds its calls to itself (`-Bsymbolic`), so nothing later needs its symbols in the global
scope (`RTLD_GLOBAL` is not needed); `RTLD_NOW` fails a missing symbol at start, not mid-game. The app namespace lets
`libmain.so` dlopen from the app's data directory. Then `dlsym` of `entry` (`recomp_title_main`, required),
`recomp_lookup`, `recomp_lookup_manual`, `fe_menu_get_pb_state`, `g_fe_cursor` (optional). `dlsym` on a handle also
searches the pack's dependencies, so a name the pack does not define comes back as `libmain.so`'s own forwarder:
each result is kept only if `dladdr` puts it inside the pack (else the forwarder would call itself).

**Routing: weak forwarders, strong titles.** The toolkit inside `libmain.so` calls `recomp_lookup` and
`recomp_lookup_manual` (thread start, APCs, DPCs, the USB and APU callbacks) and the pushbuffer replay calls
`fe_menu_get_pb_state` and reads `g_fe_cursor`. Those references were bound when `libmain.so` was loaded; a library
dlopened later cannot take them over. So `pack_loader.c` defines the four as **weak forwarders**:

```c
__attribute__((weak)) recomp_func_t recomp_lookup(uint32_t va) { return s_lookup ? s_lookup(va) : 0; }
```

and `xba_pack_load` sets `s_lookup` & co. from the pack before the title starts (NULL = the old stand-in result,
0). `g_fe_cursor` is data: `libmain.so`'s copy is refreshed from the pack's in each `fe_menu_get_pb_state` call
(the replay asks for the menu state, then reads the cursor, in the same frame). A title linked in statically
(`-PxbaTitleDir`, the stub APK) defines these **strong**: the static link takes them over the weak forwarders, and the
loader never runs. The stub APK's CI check (`llvm-nm`, `T recomp_title_main`) still holds.

**`xba_shell_abi`**: `const int` = 1, exported by `libmain.so` (`llvm-nm -D libmain.so | grep xba_shell_abi`). It
equals `Pack.SHELL_ABI` and the `--shell-abi` the app passes to prepare; bump all of them (and `tools/prepare/pack.py`
`SHELL_ABI`) together when what a pack links against in `libmain.so` changes incompatibly. A pack built against
another value is refused (`PACK_INVALID shell_abi …`) and the launcher offers Prepare again.

**logcat** (stdout, tag `xboxrecomp`):

| Line | Then |
|---|---|
| `PACK_LOADED <TID> <path> (<bytes> bytes, "<title>", shell_abi 1; exports: …)` | `xboxrecomp ready (… title)`, the title runs as a linked one would. |
| `PACK_MISSING <TID> (no …/packs/<TID>/pack.json)` | As before: the black window and the NO_TITLE message. (The launcher normally shows Prepare before Play gets here.) |
| `PACK_INVALID <reason>` | `ShellActivity.openLauncherBadPack(reason)`: the launcher opens with `EXTRA_BAD_PACK`, deletes `packs/<TID>`, and Home shows **Prepare** with « The prepared game could not be loaded (…). Prepare it again. » Its own lighter check passed the pack, hence this round trip. A message box if the JNI call fails. |
| `PACK_DLOPEN <dlerror>` | The same. |

NO_GAME_DATA is unchanged (no `default.xbe`: the game picker).

**Import drop: later.** prepare.md describes `<external files dir>/packs/<TID>/` as an import drop (copied in after
checking `lib_sha256`, never loaded in place: external storage is `noexec`). Not done yet: the copy of a 100+ MB
library belongs on a worker thread with progress in the launcher, not in `SDL_main`; for now a pack is made on the
device (or pushed with `adb shell run-as`, below).

**Tests.** `tests/recomp_pack` (Linux CI, and the device-tests bundle): the title ID, `pack.json` parsing (escapes,
unknown and nested fields, wrong types, truncation), every check, and the routing with three small libraries built
like the app's: a fake shell (`pack_loader.c` + `recomp_pack.c` + a stand-in for the toolkit calling
`recomp_lookup` & co. from inside it), a pack linked `-Bsymbolic --no-undefined` against it, a pack with its entry
only (the `dladdr` guard), and a shell with a title linked in (strong symbols win). `PackTest.drop` and
`LauncherPrepareTest.badPackFromShellOffersPrepareAgain` (Robolectric) cover the launcher side.

### Device check (title packs), for Alex

Needs the shell APK from this branch. Steps 1–5 need no toolchain in the APK (a pack made on a host, pushed); with the
toolchain, step 6.

1. Shell APK, no pack, your game chosen: logcat `PACK_MISSING <TID> (no /data/user/0/fr.adserver.xrecomp/files/
   packs/<TID>/pack.json)`, then the NO_TITLE message as before. `adb logcat -s xboxrecomp`.
2. A pack for the stub XBE made on a host (prepare.md « Checking on a host », with `--shell-lib` = the `libmain.so`
   out of **this** APK: `unzip -p xboxrecomp-shell.apk lib/arm64-v8a/libmain.so > libmain.so`). Push the stub's
   `default.xbe` as the game (`/sdcard/Android/data/fr.adserver.xrecomp/files/game/`), then the pack:
   `adb push packs/12345678 /data/local/tmp/` and
   `adb shell run-as fr.adserver.xrecomp sh -c 'mkdir -p files/packs && cp -r /data/local/tmp/12345678 files/packs/'`.
3. Start the app, Play. Expected: `PACK_LOADED 12345678 …/files/packs/12345678/libtitle.so (… bytes, "…",
   shell_abi 1; exports: recomp_lookup …)`, `xboxrecomp ready (… title)`, the five `stub title: …` lines and
   `stub title: all 5 lifted functions OK`, then `[FBWIN] guest returned (0)`. Back, Quit game twice quits;
   `adb logcat -b crash` empty.
4. Unloadable pack, same size (the launcher's check passes it):
   `adb shell run-as fr.adserver.xrecomp dd if=/dev/zero of=files/packs/12345678/libtitle.so bs=64 count=1 conv=notrunc`
   (zeroes the ELF header). Play: `PACK_DLOPEN …` (bionic's « not a valid ELF » or similar), the launcher comes back
   with the red note « The prepared game could not be loaded (…). Prepare it again. » and Home shows **Prepare**
   (with the toolchain) or **Play**; `files/packs/12345678` is gone (`run-as … ls files/packs`). Screenshot.
5. Wrong ABI: edit `pack.json` to `"shell_abi": 2` (the launcher then shows Prepare: `Pack.check` refuses it too).
   Then set it back to 1 and `"abi": "x86_64"`: Play → `PACK_INVALID abi x86_64, this shell is arm64-v8a`, back to the
   launcher.
6. With the toolchain APK: Prepare your game, Play: `PACK_LOADED <TID>`, the game boots as the per-title APK did
   (same logcat up to the first frames). Note the time from tapping Play to `PACK_LOADED` (dlopen of the real pack,
   ~150 MB, with `RTLD_NOW`).
7. The stub APK (static title) from the same commit: unchanged (no `PACK_` line at all), the « Device check
   (XBA-52) » steps 1 and 3.
8. Send the logcat lines.

### The stub title

`tools/android/stub_title.sh OUT` makes a title from `templates/new-game` and the synthetic XBE of `tools/conformance`
(ours: five leaf functions, no game content), lifted by the pipeline, plus an `xbe_entry_point` that calls the five
and checks what they return. The CI builds its APK (`--target stub-apk` of `tools/docker/android.Dockerfile`) on every
push to `main`, after the shell, and attaches `xboxrecomp-stub-<commit>.apk` and `stub-default.xbe` to the
**shell-latest** prerelease. On Linux the same project builds as the template's executable and prints the same lines
(x86-64, and arm64 under qemu).

### Android defaults

After `recomp.cfg`, for what neither it nor the environment sets (`recomp_app_set_defaults`):

| Variable | Default on Android | Why |
|---|---|---|
| `RECOMP_FB_WINDOW` | `1` | Without it `xbox_HostWindowMain` runs the guest on SDL's thread: no event loop (no pause, no Back) and a 1 MB stack. |
| `RECOMP_PB_EXEC` | `1` | The pushbuffer executor: its flips are what the window shows. |

`RECOMP_HEADLESS=1` still keeps the window off. `RECOMP_FB_WINDOW=0` or `RECOMP_PB_EXEC=0` in `recomp.cfg` turn either
off (the kernel reads `RECOMP_PB_EXEC` by presence, so the `=0` is removed from the environment). logcat:
`defaults: RECOMP_FB_WINDOW=1 RECOMP_PB_EXEC=1 (Android; …)`.

### Quit

**Quit game** in the [in-game menu](#in-game-menu) (pressed twice: the second press confirms) lets the guest go
(`recomp_lifecycle_resume_for(RECOMP_HOLD_ALL)`), `sync()`s the saves the title wrote (plain files under
`<internal files>/save`) and calls `recomp_quit(0)` (`menu_jni.c`); the companion panel's Quit (a hold) calls
`recomp_quit(0)` too. In a game's own app both first remove the app's task (`finishAndRemoveTask`): the player lands on
Android's home screen, not on a launcher. Where nothing above SDL takes Back (not the app), Back twice in the title's
window does the same.
`recomp_quit(0)` (`src/platform/recomp_quit.c`) runs the exit hook (the kernel's
missing-file summary), `xboxrecomp: clean exit`, `fflush` of stdout and stderr, the log pump drained (at most
500 ms), then `_exit(0)`. Not `exit()`: the guest's threads are still running, and atexit handlers and destructors must
not run under them. The other atexit reports (`RECOMP_PROFILE`, the ICALL feedback dump, a WAV capture) are desktop
diagnostics and are skipped on Android. On desktop hosts `recomp_quit` is `exit()`, as before.

### Quick reboot

A title that quick-reboots into its own `default.xbe` (`HalReturnToFirmware(2)` after `XLaunchNewImage`; Fable's
Pause → Quit → Confirm, which goes back to its main menu) is restarted, not ended
([docs/runtime/README.md](../runtime/README.md), « Quick reboot »). The kernel writes the launch data to
`<internal files dir>/save/launch_data.bin` and calls the restart handler, which `shell_main.c` sets before the title
starts (`restart_via_launcher`): it calls `ShellActivity.restartGame` through JNI from the guest thread, which sends
`LauncherActivity` the explicit intent `fr.adserver.xrecomp.RESTART_GAME`. The launcher, in its own `:launcher`
process, does what it already does after a new game choice: `endRunningGame()` (`Process.killProcess` on the main
process, waiting at most 2 s), then starts `ShellActivity` with `NEW_TASK | CLEAR_TASK` in a new main process, and
finishes without showing anything; the play session stays open. A new process is needed because the runtime maps its
memory once per process. The new process's kernel takes the launch data back, and the title starts where it asked to
(Fable: its main menu, no logos). The native side waits to be ended; if it is still alive after 10 s it exits by itself
(`recomp_quit(0)`), leaving the launch data for the next start. If the JNI call fails, the title exits as before.
Logcat: `[KERNEL] quick reboot: launch data in …`, `xboxrecomp: quick reboot: launcher asked to restart the game`,
`shell: quick reboot, asking the launcher to restart the game`, `launcher: the game asked for a restart (quick
reboot)`, `launcher: ending the running game (pid …)`, then the new process's `Launch data: quick reboot page at …`.
Past 3 restarts in 30 s the kernel refuses and the title exits.

### The title leaving by itself

A title that ends its own run — `HalReturnToFirmware` to the dashboard (XapiBootToDash, an empty launch path; a failed
start-up check such as KOTOR's before the DVD security sector existed), to an image this build cannot start, a
restart refused by the cap, or `HalInitiateShutdown` — goes back to the launcher, which says why. It used to call
`exit()` from the guest thread while the window, SDL and the other guest threads still ran: atexit handlers and
destructors pulled mutexes from under them, `FORTIFY: pthread_mutex_lock called on a destroyed mutex`, SIGABRT, and a
crash report (and a « Prepare again » offer) for a title that had merely quit.

`xbox_title_exit` (`src/kernel/kernel_reboot.c`, [docs/runtime/README.md](../runtime/README.md) « The title leaving »)
calls the exit handler `shell_main.c` sets before the title starts (`exit_via_launcher`). It calls
`ShellActivity.titleExited(why)` through JNI from the guest thread; that starts `LauncherActivity` with
`EXTRA_TITLE_EXITED` (the kernel's reason, English) and `NEW_TASK | CLEAR_TASK`, finishes the activity, and returns
once the UI thread has done so (at most 2 s). The kernel then ends the process with `recomp_quit(0)` (flush, log drain,
`_exit`): no atexit handlers, no tombstone, `ApplicationExitInfo` says the process exited by itself. The launcher waits
up to 2 s for the game's process to be gone (else ends it), so the play session closes and the exit is not taken for a
crash, then opens Home (Library beneath when the library is on) with the note « The game ended: the game returned to
the dashboard (routine 2). »; « Open the game directly » does not start it again. If the JNI call fails, the process
still ends through `recomp_quit`.

Logcat: `[EXIT] the game returned to the dashboard (routine 2)`, `shell: the game exited by itself (…), back to the
launcher`, `xboxrecomp: title exit: launcher opened (…)`, `xboxrecomp: clean exit`, then in `:launcher`
`launcher: shown (game exited: …)`. The quick reboot and the other image of the disc (XBA-82) are handled before any
of this and are unchanged. Test: `LauncherPrepareTest.titleExitShowsWhyOnHome` (Robolectric).

An image of the disc that is not prepared yet (XBA-82, [prepare.md « Images »](prepare.md#images-xba-82)) leaves the
same way through `ShellActivity.prepareImage(image)` (`EXTRA_PREPARE_IMAGE`; the shell wrote `work/<TID>/
image.pending` first). Home then says « Goes on in <image> · not prepared yet », its first button is **Prepare
<image>** and Play is second, with the note « The game goes on in <image>, another program of the disc, which is not
prepared yet. Prepare it (a few minutes) to go on where you were; Play starts the game from the beginning. ». Once
the Prepare is done the game starts again in that image (`launchShell` moves `image.pending` to `image.next`); Play,
or an image the Prepare left out, drops `image.pending` and `launch_data.bin`. A game's own app starts that Prepare at
once on its DIRECT screen. Logcat: `PACK_IMAGE_LAZY <image>`, `shell: the game launches <image>, not prepared yet`,
`launcher: prepare <image> of <TID>`, `launcher: the game goes on in <image>`. Test: `LauncherImagePrepareTest`
(Robolectric; screenshot `prepare-17-image-pending.png`).

### Last lines before a crash

stdout and stderr reach logcat through pipes read by two pump threads (`src/platform/android_log.c`); a line still in
a pipe when the process dies is lost. So before the fault handler hands a fault that is not guest MMIO to debuggerd,
it calls `xbox_log_drain(200)`: it waits, at most 200 ms, until both pipes are empty and the pump threads have written
what they read, and ends a half-written line with a `\n` so that it goes out too. That runs in a signal handler, so it
only uses lock-free atomics and plain system calls (`ioctl(FIONREAD)`, `clock_gettime`, `nanosleep`, a 1-byte write
into an empty pipe); it never logs by itself (`__android_log_write` takes locks the crashing thread may hold), it only
waits for the pump threads to do it. Trade-offs: a pump thread stuck behind the crashing thread is not waited for past
200 ms (its lines are lost, the crash is not held up); a crash on a pump thread does not wait for itself; a second
fault during the drain returns at once; a partial stdout line still in stdio's buffer (no `\n`, no `fflush`) is not
flushed, as `fflush` is not async-signal-safe. When the pipes are empty the drain returns at once, so it costs nothing
on the way to a tombstone in the usual case. `tests/mmio_fault_chain_posix` (`drain`) checks it on Linux with a slow
sink.

### Device check (XBA-52), for Alex

1. From **shell-latest**, install the stub APK and push its XBE, then start « XRecomp stub »:
   `adb install -r xboxrecomp-stub-<commit>.apk`,
   `adb push stub-default.xbe /sdcard/Android/data/fr.adserver.xrecomp.stub/files/game/default.xbe`,
   `adb logcat -c && adb logcat -s xboxrecomp SDL DEBUG AndroidRuntime`. Expected in logcat: `defaults: RECOMP_FB_WINDOW=1
   RECOMP_PB_EXEC=1`, `xboxrecomp ready (… title)`, `starting the title`, the boot lines (`xbox_MemoryLayoutInit:
   complete`, `Starting game...`), five `stub title: …` lines and `stub title: all 5 lifted functions OK`, then
   `[FBWIN] guest returned (0); the window stays`. The screen is black (the stub draws nothing), with a « The title
   finished (code 0). Press Back for the menu (Quit). » toast.
2. Home, wait 10 s, come back: `lifecycle: paused (background)`, then `lifecycle: resumed after ~10000 ms`.
3. Back, Quit game, Quit game: `shell: menu: quit (confirmed)`, `xboxrecomp: clean exit`, the app closes;
   `adb logcat -b crash` is empty.
4. Crash lines: push a `recomp.cfg` with `RECOMP_SHELL_CRASH_TEST=1` to the **shell** app (no title) and start it: the
   `writing to NULL now` line is in logcat above the `Fatal signal 11` and the backtrace. Remove the file afterwards.
5. Send the logcat lines.
