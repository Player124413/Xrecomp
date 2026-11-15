package fr.adserver.xrecomp;

import android.animation.AnimatorSet;
import android.animation.ObjectAnimator;
import android.app.Activity;
import android.app.ActivityManager;
import android.content.ActivityNotFoundException;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.ShapeDrawable;
import android.graphics.drawable.shapes.OvalShape;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.os.SystemClock;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.text.TextUtils;
import android.text.format.DateUtils;
import android.text.format.Formatter;
import android.util.Log;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.EnumMap;
import java.util.List;
import java.util.Locale;
import java.util.function.BiFunction;
import java.util.function.Function;
import java.util.function.LongSupplier;

import fr.adserver.xrecomp.Widgets.Icon;
import fr.adserver.xrecomp.Widgets.MenuRow;

/**
 * The launcher (XBA-41, with the game picker of XBA-54 / XBA-40): the app's launcher entry, in its own process
 * (":launcher"), before SDL. A controller-first screen set, built in code on a 1920 x 1080 design grid scaled to
 * the screen (Theme, Widgets), with a small state machine of screens and a back history instead of fragments:
 *
 * - WELCOME (first run, no game set up) -> SOURCE (game folder, disc image or library folder) -> FOLDER_ACCESS (All
 *   files access explained, or copy) -> IMPORT (progress ring; B cancels) -> HOME;
 * - LIBRARY: the game library's grid (every console's games, from several library folders), a context line, the
 *   Options sheet on Y (Rescan, Storage, Library folders, Settings, About); STORAGE: what the app stores, per game;
 * - HOME: the game's art and title, one line (when it was played, or its state), one row of buttons: a solid Play
 *   (or Prepare, Choose…) and three quiet pills, Settings, Controls, Manage;
 * - SETTINGS: four tabs (General, Graphics, Audio, Advanced): the launcher's preferences and a few RECOMP_* options
 *   saved to recomp.cfg (RecompConfig), applied at the next start; About ends Advanced;
 * - CONTROLS: the remapping of the pad (PadMapping, RECOMP_PAD_MAP in recomp.cfg), one row per Xbox control;
 * - GAME_DATA (« Manage »): change game, a home-screen shortcut for the game (TitleShortcut, ACTION_PLAY_TITLE),
 *   the game's own app (Create an app: TitleApp, TitleApk), delete the imported copy and the game's stored things;
 *   ABOUT (X: LICENCES,
 *   the licence texts the APK carries, assets/licences/);
 * - PREPARE: Home's layout with the on-device Prepare's progress (dimmed after two idle minutes, for OLED screens).
 *
 * The « Calm » look (docs/android/app-shell.md « Look, motion and sound »): a title-only top bar, one footer line
 * (the note left, at most three hints right), list rows of label and value with the focused row's description in a
 * quiet column. D-pad / stick move the focus (remembered per screen and per Settings tab; one highlight glides from
 * item to item), A selects, B goes back, left / right change a setting, L1 / R1 switch Settings' tabs, Start plays;
 * touch works too. Screens fade and slide in; moves, selects, backs, errors and a finished Prepare have a short sound
 * and a haptic (UiFeedback).
 * Messages go to a note line, not toasts. The choice is remembered in SharedPreferences (GameStore); ShellActivity
 * hands it to native code as RECOMP_GAME_DIR. See docs/android/app-shell.md, « The launcher ».
 */
public class LauncherActivity extends Activity {
    static final String TAG = "xboxrecomp";
    /** Open the source screen even when a game is set up (the « Change game » shortcut). */
    static final String ACTION_CHOOSE_GAME = "fr.adserver.xrecomp.CHOOSE_GAME";
    /** Open the home screen even when « Open the game directly » is on (the « Launcher » shortcut). */
    static final String ACTION_SHOW_LAUNCHER = "fr.adserver.xrecomp.SHOW_LAUNCHER";
    /**
     * A game's home-screen shortcut (TitleShortcut, EXTRA_TITLE_ID): that game is selected and played when it is
     * ready (the rules of « Open the game directly »), else its Home shows its state; a title ID no longer in the
     * library opens the library with a note.
     */
    static final String ACTION_PLAY_TITLE = "fr.adserver.xrecomp.PLAY_TITLE";
    /** The title ID (8 hex digits, the library's key) of ACTION_PLAY_TITLE. */
    static final String EXTRA_TITLE_ID = "fr.adserver.xrecomp.TITLE_ID";
    /** Set by ShellActivity when the native side found no default.xbe: the folder it looked in. */
    static final String EXTRA_NO_GAME_DATA = "fr.adserver.xrecomp.NO_GAME_DATA";
    /**
     * Set by ShellActivity when the native loader refused the title pack (PACK_INVALID / PACK_DLOPEN): why. The pack
     * is dropped and Home offers Prepare again.
     */
    static final String EXTRA_BAD_PACK = "fr.adserver.xrecomp.BAD_PACK";
    /**
     * Sent by ShellActivity when the title quick-rebooted into itself (HalReturnToFirmware after XLaunchNewImage of
     * its own default.xbe, e.g. Fable's Pause, Quit): end the game's process and start the game activity again. The
     * launch data waits in the save dir (kernel_reboot.c); the play session goes on. Explicit intents only.
     */
    static final String ACTION_RESTART_GAME = "fr.adserver.xrecomp.RESTART_GAME";
    /**
     * Set by ShellActivity when the title left by itself (HalReturnToFirmware to the dashboard, an image this build
     * cannot start, a shutdown; kernel_reboot.c xbox_title_exit): why, in the kernel's words. Home says so; the game's
     * process ends by itself right after (it is waited for, then ended), so its play session closes here.
     */
    static final String EXTRA_TITLE_EXITED = "fr.adserver.xrecomp.TITLE_EXITED";
    /**
     * Set by ShellActivity when the title launched another image of its disc that the pack names but has not prepared
     * yet (XBA-82, PendingImage): its disc path. Home offers « Prepare IMAGE »; the game's process ends by itself, as
     * after EXTRA_TITLE_EXITED.
     */
    static final String EXTRA_PREPARE_IMAGE = "fr.adserver.xrecomp.PREPARE_IMAGE";
    /** How long the launcher waits for the game's process to end by itself after EXTRA_TITLE_EXITED. */
    static final long TITLE_EXIT_WAIT_MS = 2000;

    private static final int REQ_TREE = 1;
    private static final int REQ_ISO = 2;
    private static final int REQ_ALL_FILES = 3;
    private static final long SPACE_MARGIN = 64L << 20;
    private static final long CONFIRM_MS = 5000;

    enum Screen {
        WELCOME, SOURCE, FOLDER_ACCESS, IMPORT, HOME, SETTINGS, CONTROLS, GAME_DATA, ABOUT, LICENCES, PREPARE, LIBRARY,
        STORAGE,
        /** A game's own app on its way into the game: its art, its title, one line, at most one action (gameApp). */
        DIRECT,
        /** A GBA game's Home (XBA-158): Play (the interpreter, or its pack) and its Prepare; romEntry. */
        ROM_HOME,
        /** A GBA, PS1 or N64 game's Controls (ConsolePadMap): its console's mapping or its own; romEntry. */
        ROM_CONTROLS,
        /**
         * A GBA, PS1 or N64 game's Game settings (XBA-181 / XBA-188): any Settings value its own or as in Settings, its
         * saves and Prepare files; romEntry.
         */
        ROM_OPTIONS,
        /** An Xbox title's Game settings (XBA-188): any Settings value its own or as in Settings; titleId(). */
        GAME_SETTINGS,
        /** Settings › Graphics › Vulkan driver: the system driver, the installed ones, the downloadable list. */
        DRIVERS
    }

    /** The Preparing screen dims after this long without input (OLED burn-in); any key or touch wakes it. */
    static final long DIM_AFTER_MS = 120_000;
    /** The launcher's clock for the idle dim (uptime ms); the JVM tests put a fake one here. */
    static LongSupplier clock = SystemClock::uptimeMillis;

    /**
     * The on-device Prepare for the current game (docs/android/prepare.md): OFF when this APK has no toolchain or
     * the game is not READY (the launcher then behaves as before), PLAYABLE when its pack is valid.
     */
    enum Prep { OFF, NEEDED, RUNNING, QUEUED, FAILED, CANCELLED, INTERRUPTED, PLAYABLE }

    /** Free internal storage a Prepare needs (work files, objects, the pack). */
    static final long PREPARE_MIN_FREE = 2_000_000_000L;
    /** Below this much RAM the Prepare screen warns: about what Android reports for a « 6 GB » device. */
    static final long PREPARE_RAM_WARN = (5L << 30) + (512L << 20);
    /** A « running » status.json this fresh counts as alive even before the ":prepare" process shows up. */
    private static final long PREPARE_FRESH_MS = 15000;
    private static final long PREPARE_POLL_MS = 500;
    private static final int REQ_NOTIFICATIONS = 4;
    private static final int REQ_LSFG_DLL = 5;
    private static final int REQ_LIBRARY_TREE = 6;
    static final int REQ_PACK_HANDOVER = 7;
    private static final int REQ_VK_DRIVER = 8;
    /** The library re-reads its entries' states (Prepare, queue, running game) every this many polls. */
    private static final int LIBRARY_POLL_EVERY = 4;

    /** The selected game's files: READY, ... ; LIBRARY: a library game whose files are not here (see its chip). */
    enum State { READY, NONE, MISSING, NO_ACCESS, WRONG, LIBRARY }

    private static final int NOTE_INFO = 0, NOTE_OK = 1, NOTE_WARN = 2, NOTE_ERROR = 3;

    // ---- The stage ----
    private TextView screenTitle, note, topRight;
    /** The top bar (hidden on Home and Preparing, which show the game's title big), its tabs' slot. */
    private LinearLayout topBar, topTabs;
    /** The stage's column (top bar, body, footer): its focus is blocked while a sheet is open. */
    private LinearLayout stageRoot;
    /** The open sheet (the library's Options), and the view focused before it opened. */
    private Widgets.Sheet sheet;
    private View sheetReturn;
    private FrameLayout body, host;
    private Widgets.Backdrop backdrop;
    private Widgets.Glide glide;
    private AnimatorSet screenAnim;
    private UiFeedback feedback;
    /** The panel on a second display (null until the stage is built, and when the game started at once). */
    private LauncherCompanion companion;
    /** True while a screen is being built: its first focus is not a « move ». */
    private boolean building;

    // ---- The game's identity (TitleArt) ----
    private String artTid;
    private Bitmap art;
    private int artColor = Theme.ACCENT;

    // ---- The game theme (GameTheme, docs/android/app-shell.md « Game theme ») ----
    private File heroFile;
    private Bitmap hero;
    private String themeLogged;
    /** Title IDs whose theme a worker bakes (or tried to), once per activity. */
    private final java.util.Set<String> themeBaking = new java.util.HashSet<>();
    /** Where a missing theme is baked: a thread of its own (tests run it in place: TestSupport.StockTheme). */
    static final java.util.concurrent.Executor THEME_THREAD = r -> new Thread(r, "xba-theme").start();
    static java.util.concurrent.Executor themeExecutor = THEME_THREAD;

    // ---- Home ----
    private Widgets.PillButton homePrimary;
    /** Home's status line while a Prepare runs (the poll updates it). */
    private Widgets.Chip homeStatus;

    // ---- The pack handover (a game's own app, XBA-176, PackHandover) ----
    /** The parent was asked in this process already (once per cold start). */
    static boolean handoverAsked;
    /** The copy running, or null: its title ID and progress. */
    private Handover handover;
    private Thread handoverWorker;

    private static final class Handover {
        final String tid;
        volatile long done, total;

        Handover(String tid, long total) {
            this.tid = tid;
            this.total = total;
        }

        int pct() {
            return total <= 0 ? 0 : (int)Math.min(100, done * 100 / total);
        }
    }

    // ---- The Preparing screen's idle dim ----
    private FrameLayout dimView;
    private TextView dimBig, dimSmall;
    private boolean dimmed;
    private long lastInputAt;
    private int swallowUp = -1;
    private LinearLayout bottomLeft, hints;
    private ShapeDrawable noteDot;
    private String noteText = "";
    private int noteKind;

    // ---- Screens, history, focus ----
    private Screen screen;
    private final ArrayDeque<Screen> history = new ArrayDeque<>();
    private final EnumMap<Screen, Integer> focusMemory = new EnumMap<>(Screen.class);
    private final List<View> focusables = new ArrayList<>();
    private View defaultFocus;
    private Runnable xAction, yAction, startAction, l1Action, r1Action;

    // ---- Game facts computed off the UI thread ----
    private String hashDir;
    private Boolean hashOk;
    private String sizeDir;
    private long sizeBytes = -1;
    private boolean deleting;
    private long deleteArmedAt;

    // ---- Settings ----
    private RecompConfig cfg;

    // ---- Import ----
    private Thread worker;
    /** From runWorker until its onWorkerDone (the thread may be over, its outcome not yet handled). */
    private boolean workerBusy;
    private volatile boolean cancelRequested;
    private long lastUiUpdate;
    private int importLabel;
    private Screen importReturn = Screen.SOURCE;
    private Widgets.Ring ring;
    private TextView importHead, importBytes, importFile;
    private volatile String workerNote, chosenDir, chosenTitle;
    private volatile int workerNoteKind;

    // ---- Prepare ----
    private String titleIdDir, titleIdValue;
    private final android.os.Handler poller = new android.os.Handler(android.os.Looper.getMainLooper());
    private boolean resumed;
    private Prep homePrep;
    /**
     * The GBA game whose Home (Screen.ROM_HOME) or Preparing screen shows (XBA-158), else null: never the selected
     * game (that is the Xbox's), cleared when Home, the library or the welcome screen shows.
     */
    private LibraryScreen.Entry romEntry;
    /** The Prepare state ROM_HOME was built with (its poll rebuilds it when that changes). */
    private Prep romPrepShown;
    /** Home's primary is « Prepare again » (XBA-74). */
    private boolean homeAgain;
    /** A crash round that ended while the launcher was not watching was looked at (XBA-129), once per launcher. */
    private boolean roundChecked;
    private PrepareProtocol.Status prepShown;
    private Widgets.Chip prepStatus;
    private Widgets.Meter prepMeter;
    private TextView prepPct, prepMsg, prepEta, prepCaption;
    private View prepProgress;
    private long cancelArmedAt;
    private boolean cancelSent;
    private final Runnable pollTick = this::pollPrepare;

    // ---- The game library (docs/android/app-shell.md « Game library ») ----
    /** games.json: the records; selected is the game Home shows (the launched one is in the preferences). */
    private Library.Games games = new Library.Games();
    /** index.json: what the library folder holds, as last scanned. */
    private LibraryIndex index = new LibraryIndex();
    private java.util.Map<String, List<LibraryIndex.Item>> indexGames = new java.util.HashMap<>();
    /** The library screen's entries and cards, as built. */
    private List<LibraryScreen.Entry> libEntries = new ArrayList<>();
    private final List<GameCard> libCards = new ArrayList<>();
    private final java.util.Map<String, Bitmap> libArt = new java.util.HashMap<>();
    /** The focused card's title ID on the library ("" for the folder tile). */
    private String focusTid;
    private TextView libCaption;
    private int libPolls;
    private Thread scanner;
    private volatile boolean scanCancel;
    private volatile int scanFound;
    /** The tree picker was opened for the library folder (not for a game folder). */
    private boolean pickLibrary;
    /** Title IDs whose pack the shell refused in this process (GameState's « Pack invalid »). */
    private final java.util.Set<String> droppedPacks = new java.util.HashSet<>();
    /** The title being imported from the library (its chip), and whether a Prepare follows the import. */
    private volatile String importTid;
    private boolean importFromLibrary;
    private volatile String chosenSha;
    /** « Play » while another game runs: the first press warns, the second within CONFIRM_MS ends it. */
    private long switchArmedAt;
    /** The library chip Home was built with (a game whose files are not here), for the poll. */
    private GameState.Chip homeChip;
    /** Storage: sizes measured (« tid/KIND » -> bytes), the item armed for deletion. */
    private final java.util.Map<String, Long> storageSizes = new java.util.HashMap<>();
    private Thread storageMeasurer;
    private String storageArmedKey;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        publishShortcuts();

        Intent in = getIntent();
        String action = in.getAction();
        if (ACTION_RESTART_GAME.equals(action) && savedInstanceState == null) {
            restartGame();
            return;
        }
        if (savedInstanceState == null && LibraryCheck.active(getFilesDir())) {
            // A library check runs (XBA-130): its screen, also when a checked game left by itself.
            Log.i(TAG, "launcher: a library check runs, showing it");
            startActivity(CheckActivity.intent(this).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            finish();
            return;
        }
        boolean change = ACTION_CHOOSE_GAME.equals(action);
        boolean showLauncher = ACTION_SHOW_LAUNCHER.equals(action);
        boolean playTitle = ACTION_PLAY_TITLE.equals(action);
        String noData = in.getStringExtra(EXTRA_NO_GAME_DATA);
        String badPack = in.getStringExtra(EXTRA_BAD_PACK);
        String exited = in.getStringExtra(EXTRA_TITLE_EXITED);
        String prepImage = in.getStringExtra(EXTRA_PREPARE_IMAGE);
        boolean forced = change || showLauncher || playTitle || noData != null || badPack != null || exited != null
                         || prepImage != null;
        if ((exited != null || prepImage != null) && savedInstanceState == null)
            awaitGameExit();
        // Back from the game: its play session ends here (Home's « Play time »), then the library is read: the
        // one-game state becomes its first record the first time (Library.migrate moves no file).
        long sessionStart = GameStore.sessionStartedAt(this);
        String sessionTid = GameStore.sessionTid(this);
        boolean hadLibrary = Library.gamesFile(getFilesDir()).isFile();
        long now = System.currentTimeMillis();
        // XBA-206: the session lasts the time the game was on screen (ShellActivity's PlayClock), not since launch.
        long sessionLength = PlayClock.sessionLength(PlayClock.read(PlayClock.file(getFilesDir())), sessionStart, now);
        if (savedInstanceState == null && !gameRunning())
            GameStore.sessionEnd(this, now, sessionLength);
        else
            sessionStart = 0;
        games = Library.migrate(this);
        setIndex(LibraryIndex.read(Library.indexFile(getFilesDir())));
        if (sessionStart > 0 && sessionTid != null && hadLibrary) {
            Library.sessionAdd(games, sessionTid, sessionStart, sessionLength);
            writeGames();
            Log.i(TAG, "launcher: play session of " + sessionTid + ": " + sessionLength / 1000 + " s on screen");
        }
        if (savedInstanceState == null) {
            // The launcher opens on the game last launched (the preferences say which; its record may lack).
            String launched = GameStore.launchedTitleId(this);
            if (launched != null && games.get(launched) != null && Console.ofKey(launched) == Console.XBOX)
                games.selected = launched;
        }
        // A game's home-screen shortcut: that game becomes the selected one (as A on its card would make it).
        boolean shortcutGone = playTitle && savedInstanceState == null
                               && !selectShortcutTitle(in.getStringExtra(EXTRA_TITLE_ID));
        if (badPack != null && savedInstanceState == null)
            dropBadPack(badPack);
        // A game's own app: its icon goes into the game (the DIRECT screen while something is needed first).
        direct = ownTid() != null && !change && !showLauncher;
        if (direct) {
            startDirect(savedInstanceState, noData, badPack, exited, prepImage);
            return;
        }
        boolean switchWarn = false;
        if (playTitle && !shortcutGone && savedInstanceState == null && shortcutPlayable()) {
            // Played as « Open the game directly » would: brought back when it runs, started when nothing runs.
            // Another game running: its Home, Play armed to end it (one more press), as from the library.
            boolean running = gameRunning();
            if (running && selectedIsLaunched()) {
                Log.i(TAG, "launcher: shortcut " + games.selected + ": running, bringing it back");
                resumeShell();
                return;
            }
            if (!running) {
                Log.i(TAG, "launcher: shortcut " + games.selected + ": ready, starting it");
                launchShell(false);
                return;
            }
            switchWarn = true;
        }
        if (!forced && savedInstanceState == null && gameReady() && !prepareBlocksPlay()) {
            /* The app icon while the game runs in the background: straight back to it (the « Launcher » shortcut
             * reaches this screen then). Otherwise only with « Open the game directly next time » on. */
            boolean running = gameRunning();
            if (running) {
                Log.i(TAG, "launcher: game running, bringing it back");
                resumeShell();
                return;
            }
            if (GameStore.autoStart(this) && selectedIsLaunched() && !xbeChanged()) {
                Log.i(TAG, "launcher: game ready and « open directly » on, starting it");
                launchShell(false);
                return;
            }
        }

        boolean lib = libraryOn();
        String dir = GameStore.chosenDir(this);
        Log.i(TAG, "launcher: shown (" + (noData != null ? "NO_GAME_DATA in " + noData
                                          : badPack != null ? "pack refused: " + badPack
                                          : exited != null ? "game exited: " + exited
                                          : prepImage != null ? "game launches " + prepImage + ", not prepared"
                                          : change ? "change game"
                                          : showLauncher ? "launcher shortcut"
                                          : playTitle ? "game shortcut " + in.getStringExtra(EXTRA_TITLE_ID)
                                                        + (shortcutGone ? ", not in the library" : "")
                                          : dir != null && !gameReady() ? "remembered folder unusable: " + dir
                                          : lib ? "library, " + games.games.size() + " games"
                                          : gameReady() ? "home" : "no game set up") + ")");
        Screen first;
        boolean toPrepare = false;
        if (noData != null) {
            history.push(lib ? Screen.LIBRARY : Screen.HOME);
            first = lib ? Screen.HOME : Screen.SOURCE;
        } else if (change) {
            if (!lib)
                history.push(Screen.HOME);
            first = lib ? Screen.LIBRARY : Screen.SOURCE;
        } else if (badPack != null || exited != null || prepImage != null || playTitle && !shortcutGone) {
            if (lib)
                history.push(Screen.LIBRARY);
            first = Screen.HOME;
            Prep p = prep();
            toPrepare = playTitle && (p == Prep.RUNNING || p == Prep.FAILED);   // its Prepare, as from its card
        } else if (shortcutGone) {
            first = lib ? Screen.LIBRARY : state() == State.NONE ? Screen.WELCOME : Screen.HOME;
        } else if (lib) {
            first = Screen.LIBRARY;
            Prep p = prep();
            if (p == Prep.RUNNING || p == Prep.FAILED) {   // the pending Prepare, its Home beneath
                history.push(Screen.LIBRARY);
                history.push(Screen.HOME);
                first = Screen.PREPARE;
            }
        } else if (state() == State.NONE) {
            first = Screen.WELCOME;
        } else {
            first = Screen.HOME;
            Prep p = prep();
            toPrepare = p == Prep.RUNNING || p == Prep.FAILED;
        }

        setUpWindow();
        feedback = new UiFeedback(this);
        syncTheme(first);       // the game's colours before any view is built: no flash of the stock green
        buildStage();
        // The second screen (AYN Thor, docs/android/app-shell.md « Dual screen »): Home's, the library's or the
        // Prepare's panel; its one action is the main screen's.
        companion = new LauncherCompanion(this, this::onPlay);
        companion.libraryActions(() -> {
            LibraryScreen.Entry e = focusedEntry();
            if (e != null)
                libraryPrimary(e);
        });

        show(first);
        if (noData != null)
            setNote(getString(R.string.NO_GAME_DATA, noData), NOTE_ERROR);
        else if (change && !lib && gameRunning())
            setNote(getString(R.string.GAME_RUNNING), NOTE_INFO);
        else if (badPack != null)
            setNote(getString(R.string.PACK_BAD, badPack), NOTE_ERROR);
        else if (exited != null)
            setNote(titleExitedNote(this, exited), NOTE_INFO);
        else if (prepImage != null && pendingImage() != null)
            setNote(getString(R.string.IMAGE_PENDING_NOTE, pendingImage()), NOTE_INFO);
        else if (shortcutGone)
            setNote(getString(R.string.SHORTCUT_GAME_GONE), NOTE_WARN);
        else if (switchWarn)
            confirmSwitch();   // « X is running and will close », Play armed
        if (toPrepare)
            go(Screen.PREPARE);
        // A game's own app without a current pack: XRecomp's, if it hands it over (XBA-176).
        askParentForPack();
        // A game's own app: XRecomp's Vulkan driver choice, as it is now (VkDriverShare).
        if (savedInstanceState == null)
            syncVkDriverFromParent();
        // Library folders all through the Storage Access Framework: scanned at a cold start (and on Y) only.
        if (savedInstanceState == null && lib && !Library.folders(this).isEmpty() && !Library.anyFolderPath(this))
            startScan();
    }

    @Override
    protected void onResume() {
        super.onResume();
        resumed = true;
        TitleAppReceiver.listener = () -> runOnUiThread(this::showTitleAppOutcome);
        showTitleAppOutcome();
        poller.removeCallbacks(pollTick);
        poller.postDelayed(pollTick, PREPARE_POLL_MS);
    }

    @Override
    protected void onPause() {
        resumed = false;
        TitleAppReceiver.listener = null;
        poller.removeCallbacks(pollTick);
        super.onPause();
    }

    @Override
    protected void onStart() {
        super.onStart();
        if (companion != null)
            companion.start();
    }

    @Override
    protected void onStop() {
        if (companion != null)
            companion.stop();
        super.onStop();
    }

    LauncherCompanion companionForTest() {
        return companion;
    }

    @Override
    protected void onDestroy() {
        if (feedback != null)
            feedback.release();
        super.onDestroy();
    }

    /** For the JVM unit tests: the screen shown, and the Prepare state Home was built with (null: Play). */
    Screen screenForTest() {
        return screen;
    }

    /** The tests run in touch mode, where nothing is focused: this brings the screen's focus as a D-pad press does. */
    void forceFocusForTest() {
        restoreFocus(true);
    }

    View focusForTest() {
        return focused();
    }

    /** The focused view of the stage (what the Glide highlights), or null. */
    private View focused() {
        return host == null ? getCurrentFocus() : host.findFocus();
    }

    void goForTest(Screen s) {
        go(s);
    }

    UiFeedback feedbackForTest() {
        return feedback;
    }

    AnimatorSet screenAnimatorForTest() {
        return screenAnim;
    }

    Widgets.Glide glideForTest() {
        return glide;
    }

    Widgets.Backdrop backdropForTest() {
        return backdrop;
    }

    Widgets.PillButton homePrimaryForTest() {
        return homePrimary;
    }

    boolean dimmedForTest() {
        return dimmed;
    }

    boolean hasArtForTest() {
        return art != null;
    }

    Prep homePrepForTest() {
        return homePrep;
    }

    String noteForTest() {
        return noteText;
    }

    /** The library's cards as built (the « Library folder » tile last). */
    List<GameCard> libraryCardsForTest() {
        return libCards;
    }

    // ---- Window and stage ----------------------------------------------------------------------------------------

    /** Fullscreen, immersive, drawn under the display cutout (the stage keeps its content out of it). */
    private void setUpWindow() {
        Window w = getWindow();
        w.setBackgroundDrawable(new ColorDrawable(Theme.BG_BOTTOM));
        WindowManager.LayoutParams lp = w.getAttributes();
        lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
        w.setAttributes(lp);
        hideSystemBars();
    }

    private void hideSystemBars() {
        /* PhoneWindow.getInsetsController() dereferences the decor view without creating it: called before
         * setContentView it throws a NullPointerException (the crash at start of the first launcher build).
         * getDecorView() creates the decor first. */
        getWindow().getDecorView();
        WindowInsetsController c = getWindow().getInsetsController();
        if (c != null) {
            c.hide(WindowInsets.Type.systemBars());
            c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus)
            hideSystemBars();
    }

    private static LinearLayout.LayoutParams lp(int w, int h) {
        return new LinearLayout.LayoutParams(w, h);
    }

    private static LinearLayout.LayoutParams lp(int w, int h, float weight) {
        return new LinearLayout.LayoutParams(w, h, weight);
    }

    private static final int MATCH = ViewGroup.LayoutParams.MATCH_PARENT, WRAP = ViewGroup.LayoutParams.WRAP_CONTENT;

    private LinearLayout column() {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        return l;
    }

    private LinearLayout rowLayout() {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.HORIZONTAL);
        return l;
    }

    private static View gap(android.content.Context c) {
        return new View(c);
    }

    /**
     * The parts every screen shares: the top bar (the screen's title, Settings' tabs, a quiet fact on the right), the
     * body, and one footer line (the note left, at most three hints right). Built again when the game theme changes
     * (syncTheme): it holds no state of its own but the views.
     */
    private void buildStage() {
        if (screenAnim != null)
            screenAnim.cancel();
        host = new FrameLayout(this);
        backdrop = new Widgets.Backdrop();
        backdrop.setHero(heroFor(screen));
        host.setBackground(backdrop);
        LinearLayout root = column();
        host.addView(root, new FrameLayout.LayoutParams(MATCH, MATCH));
        stageRoot = root;
        sheet = null;
        glide = new Widgets.Glide(this);
        host.addView(glide, new FrameLayout.LayoutParams(MATCH, MATCH));
        buildDim();
        host.getViewTreeObserver().addOnGlobalFocusChangeListener((was, now) -> {
            if (!building && was != null && (now instanceof Widgets.Selectable || now instanceof GameCard))
                feedback.play(UiFeedback.Cue.MOVE, now);
        });
        root.setPadding(Theme.MARGIN_X, Theme.MARGIN_TOP, Theme.MARGIN_X, Theme.MARGIN_BOTTOM);
        root.setClipChildren(false);

        topBar = rowLayout();
        topBar.setGravity(Gravity.CENTER_VERTICAL);
        screenTitle = Theme.oneLine(Theme.text(this, Theme.T_TITLE, Theme.MEDIUM, Theme.TEXT));
        topBar.addView(screenTitle, lp(WRAP, WRAP));
        topTabs = rowLayout();
        topTabs.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout.LayoutParams tl = lp(0, MATCH, 1);
        tl.leftMargin = Theme.S6 + Theme.S5;
        topBar.addView(topTabs, tl);
        topRight = Theme.oneLine(Theme.text(this, Theme.T_BODY, Theme.REGULAR, Theme.TEXT_DIM));
        topBar.addView(topRight, lp(WRAP, WRAP));
        LinearLayout.LayoutParams bl = lp(MATCH, Theme.TOP_BAR_H);
        bl.bottomMargin = Theme.S4;
        root.addView(topBar, bl);

        body = new FrameLayout(this);
        body.setClipChildren(false);
        root.addView(body, lp(MATCH, 0, 1));

        LinearLayout bottom = rowLayout();
        bottom.setGravity(Gravity.CENTER_VERTICAL);
        bottomLeft = rowLayout();
        bottomLeft.setGravity(Gravity.CENTER_VERTICAL);
        note = Theme.oneLine(Theme.text(this, Theme.T_BODY, Theme.REGULAR, Theme.TEXT_DIM));
        note.setGravity(Gravity.CENTER_VERTICAL);
        noteDot = new ShapeDrawable(new OvalShape());
        noteDot.setBounds(0, 0, 14, 14);
        note.setCompoundDrawables(noteDot, null, null, null);
        note.setCompoundDrawablePadding(Theme.S2);
        bottomLeft.addView(note, lp(WRAP, MATCH));
        bottom.addView(bottomLeft, lp(0, MATCH, 1));
        hints = rowLayout();
        hints.setGravity(Gravity.CENTER_VERTICAL | Gravity.END);
        LinearLayout.LayoutParams hl = lp(WRAP, MATCH);
        hl.leftMargin = Theme.S5;
        bottom.addView(hints, hl);
        LinearLayout.LayoutParams fl = lp(MATCH, Theme.FOOTER_H);
        fl.topMargin = Theme.S2;
        root.addView(bottom, fl);

        setContentView(new Widgets.Stage(this, host));
        setNote(null, NOTE_INFO);
    }

    /** The hero backdrop shows on Home and Preparing only; every other screen has the plain gradient and one glow. */
    private Bitmap heroFor(Screen s) {
        if (romEntry != null && (s == Screen.PREPARE || s == Screen.ROM_HOME))
            return null;   // a GBA game: no title image
        return s == Screen.HOME || s == Screen.PREPARE || s == Screen.DIRECT ? hero : null;
    }

    /** The top bar: shown with the screen's title, or hidden (Home, Preparing: the game's title is the heading). */
    private void setTopBar(boolean shown) {
        topBar.setVisibility(shown ? View.VISIBLE : View.GONE);
    }

    private void setNote(String text, int kind) {
        noteText = text == null ? "" : text;
        noteKind = kind;
        paintNote();
        if (!noteText.isEmpty() && kind >= NOTE_WARN)
            Log.i(TAG, "launcher: " + noteText);
        if (!noteText.isEmpty() && kind == NOTE_ERROR && feedback != null)
            feedback.play(UiFeedback.Cue.ERROR, host);
    }

    /** The footer's left side when there is no note: the library's context line (its dot colour, 0: no dot). */
    private String contextText = "";
    private int contextDot;

    private void setContext(String text, int dot) {
        contextText = text == null ? "" : text;
        contextDot = dot;
        paintNote();
    }

    private void paintNote() {
        if (note == null)
            return;
        if (noteText.isEmpty()) {
            note.setText(contextText);
            note.setTextColor(Theme.TEXT_DIM);
            noteDot.getPaint().setColor(contextText.isEmpty() ? 0 : contextDot);
            noteDot.invalidateSelf();
            return;
        }
        int color = noteKind == NOTE_ERROR ? Theme.ERROR : noteKind == NOTE_WARN ? Theme.WARN
                    : noteKind == NOTE_OK ? Theme.OK : Theme.TEXT_DIM;
        note.setText(noteText);
        note.setTextColor(noteKind == NOTE_INFO ? Theme.TEXT_DIM : color);
        noteDot.getPaint().setColor(color);
        noteDot.invalidateSelf();
    }

    /** At most this many hints on a screen: the footer stays one discreet line. */
    static final int MAX_HINTS = 3;

    private void addHint(String glyph, int label, Runnable action) {
        if (hints.getChildCount() >= MAX_HINTS)
            throw new IllegalStateException("more than " + MAX_HINTS + " hints on " + screen);
        Widgets.Hint h = new Widgets.Hint(this, glyph, getString(label), v -> action.run());
        LinearLayout.LayoutParams p = lp(WRAP, WRAP);
        p.leftMargin = hints.getChildCount() == 0 ? 0 : Theme.S4;
        hints.addView(h, p);
    }

    /** The hints shown (tests). */
    List<Widgets.Hint> hintsForTest() {
        List<Widgets.Hint> out = new ArrayList<>();
        for (int i = 0; i < hints.getChildCount(); i++)
            out.add((Widgets.Hint)hints.getChildAt(i));
        return out;
    }

    /** A focusable the screen registers, in order: focus memory is an index in this list. */
    private <T extends View> T focusable(T v) {
        focusables.add(v);
        return v;
    }

    private MenuRow row(String title, String description, Runnable click) {
        MenuRow r = new MenuRow(this);
        r.set(title, description);
        r.onPress = () -> feedback.play(UiFeedback.Cue.SELECT, r);
        if (click != null)
            r.setOnClickListener(v -> click.run());
        return focusable(r);
    }

    // ---- Navigation --------------------------------------------------------------------------------------------

    private void go(Screen s) {
        if (s == Screen.SETTINGS && screen != Screen.SETTINGS) {   // Settings opens on General
            settingsTab = TAB_GENERAL;
            java.util.Arrays.fill(tabFocus, -1);
            focusMemory.remove(Screen.SETTINGS);
        }
        if (screen != null && screen != Screen.IMPORT)
            history.push(screen);
        show(s);
    }

    private void back() {
        feedback.play(UiFeedback.Cue.BACK, host);
        if (screen == Screen.IMPORT) {
            requestCancel();
            return;
        }
        if (screen == Screen.PREPARE && prepShown != null && prepShown.running()) {
            armCancelPrepare();
            return;
        }
        if (screen == Screen.DIRECT) {   // a game's own app: back to Android (a Prepare goes on in its service)
            finish();
            return;
        }
        if (direct && history.peek() == Screen.DIRECT) {   // from the game choice: where things stand now
            history.pop();
            directStep(true);
            return;
        }
        if (clearSearch())   // XBA-206: B first takes the library's search away
            return;
        if (history.isEmpty()) {
            if (screen != Screen.LIBRARY && screen != Screen.WELCOME && libraryOn()) {
                show(Screen.LIBRARY, -1);   // Home of a library game: back to the library
                return;
            }
            finish();
            return;
        }
        show(history.pop(), -1);
    }

    private void show(Screen s) {
        show(s, 1);
    }

    /**
     * Builds screen s from scratch (state may have changed), clears the note, restores the screen's focus, and fades
     * it in sliding from the right (dir 1, forward), the left (-1, back), or not at all (0, a refresh).
     */
    private void show(Screen s, int dir) {
        if (screen != null && !switchingTab) {
            int i = focusables.indexOf(host.findFocus());
            if (i >= 0)
                focusMemory.put(screen, i);
        }
        closeSheet(false);
        if (s == Screen.HOME || s == Screen.LIBRARY || s == Screen.WELCOME)
            romEntry = null;
        romPrepShown = null;
        syncTheme(s);
        if (s != Screen.LIBRARY && backdrop != null)
            backdrop.setGlows(Theme.GLOW, Theme.GLOW2);   // the library's per-card glow stays there
        backdrop.setHero(heroFor(s));
        building = true;
        setDimmed(false);
        Screen was = screen;
        if (s != screen)
            lastInputAt = clock.getAsLong();
        screen = s;
        if (s != Screen.IMPORT && s != Screen.PREPARE)
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        body.removeAllViews();
        bottomLeft.removeAllViews();
        hints.removeAllViews();
        focusables.clear();
        defaultFocus = null;
        desc = null;
        licencesScroll = null;
        xAction = yAction = startAction = l1Action = r1Action = null;
        topRight.setText("");
        topTabs.removeAllViews();
        setTopBar(s != Screen.HOME && s != Screen.PREPARE && s != Screen.DIRECT && s != Screen.ROM_HOME);
        contextText = "";
        deleteArmedAt = 0;
        storageArmedKey = null;
        cancelArmedAt = 0;
        homePrep = null;
        homeAgain = false;
        prepShown = null;
        prepStatus = null;
        prepMeter = null;
        prepPct = prepMsg = prepEta = prepCaption = null;
        prepProgress = null;
        homePrimary = null;
        homeStatus = null;
        homeChip = null;
        directStatus = null;
        switchArmedAt = 0;
        libCards.clear();
        libRecentCards.clear();
        libCardEntries.clear();
        libCaption = null;
        libContent = null;
        libScroll = null;
        libSearch = null;
        libSearchPill = libSortPill = null;
        setNote(null, NOTE_INFO);
        refreshIdentity();
        switch (s) {
        case WELCOME:
            buildWelcome();
            break;
        case SOURCE:
            buildSource();
            break;
        case FOLDER_ACCESS:
            buildFolderAccess();
            break;
        case IMPORT:
            buildImport();
            break;
        case HOME:
            buildHome();
            break;
        case SETTINGS:
            buildSettings();
            break;
        case CONTROLS:
            buildControls();
            break;
        case GAME_DATA:
            buildGameData();
            break;
        case ABOUT:
            buildAbout();
            break;
        case LICENCES:
            buildLicences();
            break;
        case PREPARE:
            buildPrepare();
            break;
        case LIBRARY:
            buildLibrary();
            break;
        case STORAGE:
            buildStorage();
            break;
        case DIRECT:
            buildDirect();
            break;
        case ROM_HOME:
            buildRomHome();
            break;
        case ROM_CONTROLS:
            buildRomControls();
            break;
        case ROM_OPTIONS:
            buildRomOptions();
            break;
        case GAME_SETTINGS:
            buildXboxGameSettings();
            break;
        case DRIVERS:
            buildDrivers();
            break;
        }
        restoreFocus(false);
        if (companion != null && s != Screen.HOME && s != Screen.LIBRARY && s != Screen.PREPARE)
            companion.artOnly();   // the bottom screen: the game's art alone
        // A library folder read as files: a (stat-only) rescan each time the library opens.
        if (s == Screen.LIBRARY && was != Screen.LIBRARY && Library.anyFolderPath(this))
            startScan();
        building = false;
        if (dir != 0)
            animateIn(dir);
    }

    /** The screen change: the body fades in and slides in by Theme.ANIM_SLIDE, the title fades in. */
    private void animateIn(int dir) {
        if (screenAnim != null)
            screenAnim.cancel();
        AnimatorSet set = new AnimatorSet();
        set.playTogether(ObjectAnimator.ofFloat(body, View.ALPHA, 0, 1),
                         ObjectAnimator.ofFloat(body, View.TRANSLATION_X, dir * Theme.ANIM_SLIDE, 0),
                         ObjectAnimator.ofFloat(screenTitle, View.ALPHA, 0, 1));
        set.setDuration(Theme.ANIM_SCREEN_MS);
        set.setInterpolator(new android.view.animation.DecelerateInterpolator());
        set.start();
        screenAnim = set;
    }

    /**
     * The game theme for the current game: from the cache (GameTheme.load), or baked by a worker when missing, after
     * which the screen is shown again. No game, no title ID or the player's « Off »: the stock colours. When the
     * colours or the hero change, the tokens are applied and the stage is built again (its paints hold the colours
     * they were made with), and the bottom screen's panel too; show() then builds the body. True when it changed.
     */
    private boolean syncTheme(Screen s) {
        // The library and Storage: always the stock colours (only the backdrop's glow follows the focused card).
        boolean stock = s == Screen.LIBRARY || s == Screen.STORAGE || s == Screen.ROM_HOME
                        || s == Screen.ROM_CONTROLS || s == Screen.ROM_OPTIONS || s == Screen.PREPARE && romEntry != null;
        String dir = stock ? null : gameDir();
        File gd = dir == null ? null : new File(dir);
        String tid = null;
        if (!stock && (gd != null && GameStore.findXbe(gd) != null || dir == null && state() == State.LIBRARY))
            tid = titleId();
        GameTheme g = null;
        GameTheme.Mode mode = tid == null ? null : GameTheme.mode(this, tid);
        if (tid != null && mode != GameTheme.Mode.OFF) {
            g = GameTheme.load(this, gd, tid);
            if (g == null)
                bakeTheme(gd != null ? gd : cachedXbeDir(), tid);
        }
        Theme.Colors want = g == null ? Theme.Colors.STOCK : g.colors;
        File wantHero = g == null ? null : g.heroFile;
        String line = g != null ? g.logLine()
                                : "launcher: theme " + (tid == null ? "none" : tid) + ": stock"
                                  + (mode == null ? "" : ", mode " + mode.key);
        if (!line.equals(themeLogged)) {
            themeLogged = line;
            Log.i(TAG, line);
        }
        boolean changed = !want.equals(Theme.current()) || !java.util.Objects.equals(wantHero, heroFile);
        if (!changed)
            return false;
        Theme.apply(want);
        heroFile = wantHero;
        hero = wantHero == null ? null : BitmapFactory.decodeFile(wantHero.getPath());
        if (host != null) {
            buildStage();
            if (companion != null)
                companion.screen().rebuild();
        }
        return true;
    }

    /** Bakes the title's theme off the UI thread (an older import, a cache made stale), then shows the screen again. */
    private void bakeTheme(File gameDir, String tid) {
        if (gameDir == null || !themeBaking.add(tid))
            return;
        String title = gameTitle(gameDir());
        android.content.Context app = getApplicationContext();
        themeExecutor.execute(() -> {
            GameTheme.ensure(app, gameDir, tid, title);
            poller.post(() -> {
                if (isFinishing() || isDestroyed() || screen == null || screen == Screen.IMPORT
                    || screen == Screen.PREPARE || screen == Screen.LIBRARY || screen == Screen.STORAGE)
                    return;   // live progress, or stock colours: the theme shows on the next screen
                artTid = null;   // the title image may have come from another source than $$XTIMAGE
                String keep = noteText;
                int keepKind = noteKind;
                show(screen, 0);
                noteText = keep;   // the note on screen stays (shown again without its sound or log)
                noteKind = keepKind;
                paintNote();
            });
        });
    }

    /** The game's title image (or monogram colour) for the current game. */
    private void refreshIdentity() {
        String dir = gameDir();
        String tid = titleId();
        String key = tid != null ? tid : dir;
        if (key == null) {
            artTid = null;
            art = null;
            artColor = Theme.ACCENT;
        } else if (!key.equals(artTid)) {
            artTid = key;
            if (tid == null) {
                art = null;
            } else if (dir != null) {
                art = TitleArt.load(getFilesDir(), GameStore.findXbe(new File(dir)), tid);
            } else {   // a library game whose files are not here: the scan's cached image
                File f = TitleArt.cacheFile(getFilesDir(), tid);
                art = f.isFile() ? BitmapFactory.decodeFile(f.getPath()) : null;
            }
            if (art == null && tid != null && Console.ofKey(tid) != Console.XBOX)
                art = CoverArt.load(getFilesDir(), tid);   // a GBA / PS1 / N64 game: its cover or a captured frame
            Log.i(TAG, "launcher: title image " + (art != null ? art.getWidth() + "x" + art.getHeight() : "none")
                       + " for " + key);
            // A pinned shortcut of this game follows its image and title (the image may have come after the pin).
            if (tid != null)
                TitleShortcut.refresh(this, tid, gameTitle(dir), art);
        }
        // The art tile's glow: the game theme's (stock: the accent); the monogram keeps the title ID's colour.
        if (key != null)
            artColor = art != null ? Theme.GLOW : TitleArt.monogramColor(key);
    }

    private View focusTarget() {
        if (sheet != null)
            return sheet.rows.getChildCount() == 0 ? null : sheet.rows.getChildAt(0);
        Integer i = focusMemory.get(screen);
        if (i != null && i < focusables.size())
            return focusables.get(i);
        if (defaultFocus != null)
            return defaultFocus;
        return focusables.isEmpty() ? null : focusables.get(0);
    }

    private void restoreFocus(boolean fromTouch) {
        View v = focusTarget();
        if (v == null)
            return;
        if (fromTouch)
            v.requestFocusFromTouch();
        else
            v.requestFocus();
    }

    /**
     * Gamepad buttons, before the views: A selects the focused item, B / Back goes back (both on release, once), X,
     * Y, Start, L1, R1 do what the screen's hints say. A navigation key with nothing focused (after a touch) first
     * brings the screen's focus back. Left / right on a setting are the row's own (MenuRow.onKeyDown).
     */
    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        if (touchEditor != null)   // the touch layout editor (XBA-207) takes every key: B cancels, the rest is its own
            return touchEditor.key(e) || super.dispatchKeyEvent(e);
        if (screen == null)
            return super.dispatchKeyEvent(e);
        int k = e.getKeyCode();
        boolean down = e.getAction() == KeyEvent.ACTION_DOWN;
        boolean once = e.getAction() == KeyEvent.ACTION_UP && !e.isCanceled();
        lastInputAt = clock.getAsLong();
        if (hkCapture != null && hotkeyCaptureKey(e))   // XBA-190: « press the buttons » takes every key
            return true;
        // The dimmed Preparing screen: the first key only wakes it (its release is swallowed too).
        if (dimmed) {
            if (down) {
                setDimmed(false);
                swallowUp = k;
            }
            return true;
        }
        if (swallowUp == k && !down) {
            swallowUp = -1;
            return true;
        }
        if (sheet != null) {   // the sheet: B (or Y again) closes it, A and the D-pad act in it, the rest waits
            switch (k) {
            case KeyEvent.KEYCODE_BACK:
            case KeyEvent.KEYCODE_BUTTON_B:
            case KeyEvent.KEYCODE_ESCAPE:
            case KeyEvent.KEYCODE_BUTTON_Y:
                if (once) {
                    feedback.play(UiFeedback.Cue.BACK, host);
                    closeSheet(true);
                }
                return true;
            case KeyEvent.KEYCODE_BUTTON_X:
            case KeyEvent.KEYCODE_BUTTON_START:
            case KeyEvent.KEYCODE_BUTTON_L1:
            case KeyEvent.KEYCODE_BUTTON_R1:
                return true;
            default:
                break;
            }
        }
        if (licencesScroll != null && sheet == null
                && (k == KeyEvent.KEYCODE_DPAD_UP || k == KeyEvent.KEYCODE_DPAD_DOWN)) {
            if (down)   // repeats too: holding the D-pad keeps scrolling
                licencesScroll.smoothScrollBy(0, (k == KeyEvent.KEYCODE_DPAD_UP ? -1 : 1) * Theme.ROW_H * 2);
            return true;
        }
        switch (k) {
        case KeyEvent.KEYCODE_BACK:
        case KeyEvent.KEYCODE_BUTTON_B:
        case KeyEvent.KEYCODE_ESCAPE:
            if (once)
                back();
            return true;
        case KeyEvent.KEYCODE_BUTTON_A:
            if (down && e.getRepeatCount() == 0 && focused() == null) {
                restoreFocus(true);
                return true;
            }
            if (once && focused() != null)
                focused().performClick();
            return true;
        case KeyEvent.KEYCODE_BUTTON_X:
            return runOnce(once, xAction);
        case KeyEvent.KEYCODE_BUTTON_Y:
            return runOnce(once, yAction);
        case KeyEvent.KEYCODE_BUTTON_START:
            return runOnce(once, startAction);
        case KeyEvent.KEYCODE_BUTTON_L1:
            return runOnce(once, l1Action);
        case KeyEvent.KEYCODE_BUTTON_R1:
            return runOnce(once, r1Action);
        case KeyEvent.KEYCODE_DPAD_UP:
        case KeyEvent.KEYCODE_DPAD_DOWN:
        case KeyEvent.KEYCODE_DPAD_LEFT:
        case KeyEvent.KEYCODE_DPAD_RIGHT:
        case KeyEvent.KEYCODE_DPAD_CENTER:
        case KeyEvent.KEYCODE_ENTER:
            if (down && focused() == null && (!focusables.isEmpty() || sheet != null)) {
                restoreFocus(true);
                return true;
            }
            break;
        default:
            break;
        }
        return super.dispatchKeyEvent(e);
    }

    private static boolean runOnce(boolean once, Runnable r) {
        if (once && r != null)
            r.run();
        return true;
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent e) {
        lastInputAt = clock.getAsLong();
        if (dimmed) {
            if (e.getActionMasked() == MotionEvent.ACTION_DOWN)
                setDimmed(false);
            return true;
        }
        return super.dispatchTouchEvent(e);
    }

    // ---- Sheet -------------------------------------------------------------------------------------------------

    /** Opens a sheet over the screen: the screen's focus is kept for later and blocked while it is open. */
    private Widgets.Sheet openSheet(int title) {
        return openSheet(getString(title));
    }

    private Widgets.Sheet openSheet(String title) {
        closeSheet(false);
        sheetReturn = focused();
        sheet = new Widgets.Sheet(this, title, () -> {
            feedback.play(UiFeedback.Cue.BACK, host);
            closeSheet(true);
        });
        host.addView(sheet, host.indexOfChild(glide), new FrameLayout.LayoutParams(MATCH, MATCH));
        stageRoot.setDescendantFocusability(ViewGroup.FOCUS_BLOCK_DESCENDANTS);
        return sheet;
    }

    /** A row of the open sheet: its action runs once the sheet is closed. */
    private MenuRow sheetRow(String title, Runnable action) {
        MenuRow r = new MenuRow(this);
        r.set(title, null);
        r.onPress = () -> feedback.play(UiFeedback.Cue.SELECT, r);
        r.setOnClickListener(v -> {
            closeSheet(false);
            action.run();
        });
        sheet.addRow(r);
        return r;
    }

    /** After the rows are added: the first one takes the focus (as the D-pad would). */
    private void sheetReady() {
        sheet.addHint(new Widgets.Hint(this, "A", getString(R.string.HINT_SELECT), v -> clickFocused()));
        sheet.addHint(new Widgets.Hint(this, "B", getString(R.string.HINT_CLOSE), v -> {
            feedback.play(UiFeedback.Cue.BACK, host);
            closeSheet(true);
        }));
        View first = focusTarget();
        if (first != null && !first.requestFocus())
            first.requestFocusFromTouch();
    }

    /** Closes the open sheet; restore: the focus goes back where it was. */
    private void closeSheet(boolean restore) {
        if (sheet == null)
            return;
        host.removeView(sheet);
        sheet = null;
        stageRoot.setDescendantFocusability(ViewGroup.FOCUS_BEFORE_DESCENDANTS);
        View back = sheetReturn;
        sheetReturn = null;
        if (restore) {
            if (back != null && back.isAttachedToWindow()) {
                if (!back.requestFocus())
                    back.requestFocusFromTouch();
            } else {
                restoreFocus(false);
            }
        }
    }

    Widgets.Sheet sheetForTest() {
        return sheet;
    }

    // ---- The Preparing screen's idle dim (OLED burn-in) --------------------------------------------------------

    /** A near-black layer over the stage: the overall percentage and the step, small and grey, moved each minute. */
    private void buildDim() {
        dimView = new FrameLayout(this);
        dimView.setBackgroundColor(0xFF000000);
        dimView.setVisibility(View.GONE);
        LinearLayout c = column();
        c.setGravity(Gravity.CENTER_HORIZONTAL);
        dimBig = Theme.text(this, 120, Theme.REGULAR, Theme.alpha(Theme.TEXT_DIM, 0.55f), "");
        c.addView(dimBig, lp(WRAP, WRAP));
        dimSmall = Theme.text(this, Theme.T_BODY, Theme.REGULAR, Theme.alpha(Theme.TEXT_FAINT, 0.8f), "");
        LinearLayout.LayoutParams sl = lp(WRAP, WRAP);
        sl.topMargin = 18;
        c.addView(dimSmall, sl);
        dimView.addView(c, new FrameLayout.LayoutParams(WRAP, WRAP, Gravity.CENTER));
        host.addView(dimView, new FrameLayout.LayoutParams(MATCH, MATCH));
    }

    private void setDimmed(boolean on) {
        if (dimView == null || dimmed == on)
            return;
        dimmed = on;
        dimView.setVisibility(on ? View.VISIBLE : View.GONE);
        WindowManager.LayoutParams wlp = getWindow().getAttributes();
        wlp.screenBrightness = on ? 0.02f : WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE;
        getWindow().setAttributes(wlp);
        if (on)
            paintDim();
        else
            lastInputAt = clock.getAsLong();
        Log.i(TAG, "launcher: preparing screen " + (on ? "dimmed (idle)" : "awake"));
    }

    /** The dim layer's text, from the Preparing screen's last status; its place shifts by the minute. */
    private void paintDim() {
        PrepareProtocol.Status s = prepShown;
        float frac = s == null || s.step <= 0 ? -1 : PrepareProtocol.overall(s.step, s.n, s.pct);
        dimBig.setText(frac >= 0 ? String.format(Locale.getDefault(), "%d%%", Math.round(frac * 100)) : "…");
        String step = s == null ? getString(R.string.PREPARE_STARTING) : PrepareService.stepName(this, s.id);
        if (s != null && s.eta >= 0)
            step += " · " + getString(R.string.PREPARE_ETA, PrepareProtocol.formatEta(s.eta));
        dimSmall.setText(step);
        long minute = clock.getAsLong() / 60_000;
        View c = (View)dimBig.getParent();
        c.setTranslationX((float)(Math.sin(minute * 1.7) * 220));
        c.setTranslationY((float)(Math.cos(minute * 1.3) * 140));
    }

    /** Polled with the status: dims the running Preparing screen after DIM_AFTER_MS without input. */
    private void checkDim(boolean running) {
        if (!running) {
            setDimmed(false);
            return;
        }
        if (dimmed)
            paintDim();
        else if (clock.getAsLong() - lastInputAt >= DIM_AFTER_MS)
            setDimmed(true);
    }

    // ---- The list template (Settings, Manage, Storage) -----------------------------------------------------------

    /** The list's width, its focus plates included (its rows' text starts on the margin). */
    static final int LIST_W = 1072;
    /** Between the list's focus plates and the description column. */
    static final int DESC_GAP = 80;

    /** The description column of a list screen: the focused row's explanation, as plain text. */
    private LinearLayout desc;

    /**
     * The list template: a scrolling list on the left (its rows' text on the margin, their focus plates in the
     * margin), and a quiet column on the right with the focused row's description only. Returns the list.
     */
    private LinearLayout listTemplate() {
        LinearLayout h = rowLayout();
        ScrollView scroll = new ScrollView(this);
        scroll.setVerticalScrollBarEnabled(false);
        scroll.setClipToPadding(false);
        scroll.setClipChildren(false);
        scroll.setPadding(0, 0, 0, Theme.S3);
        LinearLayout list = column();
        list.setClipChildren(false);
        scroll.addView(list, new FrameLayout.LayoutParams(MATCH, WRAP));
        LinearLayout.LayoutParams sl = lp(LIST_W, MATCH);
        sl.leftMargin = -Theme.ROW_INSET;
        h.addView(scroll, sl);
        desc = column();
        desc.setPadding(0, Theme.S3, 0, 0);
        LinearLayout.LayoutParams dl = lp(0, MATCH, 1);
        dl.leftMargin = DESC_GAP;
        h.addView(desc, dl);
        body.addView(h, new FrameLayout.LayoutParams(MATCH, MATCH));
        return list;
    }

    /** A list row: label (and value); its description shows in the column when it is focused. */
    private MenuRow listRow(LinearLayout list, String title, String description, Runnable click) {
        MenuRow r = row(title, description, click);
        list.addView(r, lp(MATCH, Theme.ROW_H));
        onFocus(r, () -> describe(r.description));
        return r;
    }

    /** A group label in a list (a game's name in Storage): Small, sentence case. */
    private void listLabel(LinearLayout list, String text) {
        TextView t = Theme.oneLine(Theme.label(this, text));
        t.setPadding(Theme.ROW_INSET, 0, Theme.ROW_INSET, 0);
        LinearLayout.LayoutParams p = lp(MATCH, WRAP);
        p.topMargin = list.getChildCount() == 0 ? Theme.S1 : Theme.S4;
        p.bottomMargin = Theme.S1;
        list.addView(t, p);
    }

    private void describe(String text) {
        describe(text, null, 0);
    }

    /** The description column: text, and an extra paragraph in its colour (a warning, a path), either may be null. */
    private void describe(String text, String extra, int extraColor) {
        if (desc == null)
            return;
        desc.removeAllViews();
        if (text != null && !text.isEmpty())
            desc.addView(Theme.body(this, text), lp(MATCH, WRAP));
        if (extra != null && !extra.isEmpty()) {
            TextView t = Theme.body(this, extra);
            t.setTextColor(extraColor);
            LinearLayout.LayoutParams p = lp(MATCH, WRAP);
            p.topMargin = desc.getChildCount() == 0 ? 0 : Theme.S3;
            desc.addView(t, p);
        }
    }

    /** What the description column shows (tests). */
    String descriptionForTest() {
        StringBuilder b = new StringBuilder();
        for (int i = 0; desc != null && i < desc.getChildCount(); i++) {
            if (desc.getChildAt(i) instanceof TextView)   // Controls draws a pad below its text
                b.append(b.length() == 0 ? "" : "\n\n").append(((TextView)desc.getChildAt(i)).getText());
        }
        return b.toString();
    }

    /** « Folder: /sdcard/xbox »: a fact line of a description. */
    private String kv(int label, String value) {
        return getString(R.string.KV_LINE, getString(label), value);
    }

    private void onFocus(View v, Runnable fill) {
        v.setOnFocusChangeListener((view, has) -> {
            if (has)
                fill.run();
        });
    }

    // ---- WELCOME -----------------------------------------------------------------------------------------------

    /** First run: a welcome, what it takes in three plain steps, and one button. */
    private void buildWelcome() {
        screenTitle.setText(R.string.WELCOME_TITLE);
        LinearLayout h = rowLayout();
        LinearLayout left = column();
        left.setGravity(Gravity.CENTER_VERTICAL);
        left.addView(Theme.text(this, Theme.T_DISPLAY, Theme.MEDIUM, Theme.TEXT, getString(R.string.WELCOME_HERO)),
                     lp(MATCH, WRAP));
        TextView t = Theme.body(this, getString(R.string.WELCOME_TEXT));
        LinearLayout.LayoutParams tl = lp(MATCH, WRAP);
        tl.topMargin = Theme.S3;
        left.addView(t, tl);
        LinearLayout buttons = rowLayout();
        LinearLayout.LayoutParams bl = lp(MATCH, Theme.BUTTON_H);
        bl.topMargin = Theme.S5;
        left.addView(buttons, bl);
        defaultFocus = pill(buttons, Icon.ARROW, getString(R.string.WELCOME_START), true, () -> go(Screen.SOURCE));
        h.addView(left, lp(0, MATCH, 1));

        LinearLayout steps = column();
        steps.setGravity(Gravity.CENTER_VERTICAL);
        int[][] items = { { R.string.STEP1, R.string.STEP1_SUB }, { R.string.STEP2, R.string.STEP2_SUB },
                          { R.string.STEP3, R.string.STEP3_SUB } };
        for (int i = 0; i < items.length; i++) {
            TextView n = Theme.text(this, Theme.T_ROW, Theme.MEDIUM, Theme.TEXT,
                                    (i + 1) + "   " + getString(items[i][0]));
            LinearLayout.LayoutParams nl = lp(MATCH, WRAP);
            nl.topMargin = i == 0 ? 0 : Theme.S5;
            steps.addView(n, nl);
            TextView d = Theme.body(this, getString(items[i][1]));
            LinearLayout.LayoutParams dl = lp(MATCH, WRAP);
            dl.topMargin = Theme.S1;
            steps.addView(d, dl);
        }
        LinearLayout.LayoutParams sl = lp(Theme.DESC_W + Theme.S5, MATCH);
        sl.leftMargin = Theme.S6;
        h.addView(steps, sl);
        body.addView(h, new FrameLayout.LayoutParams(MATCH, MATCH));
        addHint("A", R.string.HINT_START, () -> go(Screen.SOURCE));
        addHint("B", R.string.HINT_QUIT, this::back);
    }

    // ---- SOURCE and FOLDER_ACCESS --------------------------------------------------------------------------------

    /** A choice of the list template: its label and a short value, its explanation in the description column. */
    private MenuRow choice(LinearLayout list, int title, String description, String value, Runnable click) {
        MenuRow r = listRow(list, getString(title), description, click);
        if (value != null)
            r.setValue(value);
        return r;
    }

    private void buildSource() {
        screenTitle.setText(R.string.SOURCE_TITLE);
        topRight.setText(getString(R.string.LIB_ST_FREE, size(freeSpace())));
        LinearLayout list = listTemplate();
        boolean all = Environment.isExternalStorageManager();
        MenuRow folder = choice(list, R.string.CARD_FOLDER, getString(R.string.CARD_FOLDER_SUB),
                                getString(all ? R.string.BADGE_IN_PLACE : R.string.BADGE_FOLDER_ASK),
                                this::onChooseFolder);
        choice(list, R.string.CARD_ISO, getString(R.string.CARD_ISO_SUB), null, this::openIsoPicker);
        List<Library.Folder> libFolders = Library.folders(this);
        choice(list, R.string.LIB_CARD, getString(R.string.LIB_CARD_SUB),
               libFolders.isEmpty() ? null : libFolders.size() > 1 ? foldersValue(libFolders)
               : libFolders.get(0).path != null ? libFolders.get(0).name() : getString(R.string.LIB_BADGE_SET),
               this::onChooseLibraryFolder);
        // The intro under the focused choice's own text: what any choice keeps.
        for (int i = 0; i < list.getChildCount(); i++) {
            MenuRow r = (MenuRow)list.getChildAt(i);
            onFocus(r, () -> describe(r.description, getString(R.string.SOURCE_TEXT), Theme.TEXT_DIM));
        }
        defaultFocus = folder;
        describe(folder.description, getString(R.string.SOURCE_TEXT), Theme.TEXT_DIM);
        addHint("A", R.string.HINT_CHOOSE, this::clickFocused);
        addHint("B", R.string.HINT_BACK, this::back);
    }

    private void buildFolderAccess() {
        screenTitle.setText(R.string.ALL_FILES_TITLE);
        LinearLayout list = listTemplate();
        String why = getString(pickLibrary ? R.string.LIB_ALL_FILES_EXPLAIN : R.string.ALL_FILES_EXPLAIN);
        MenuRow grant = choice(list, R.string.ALL_FILES_GRANT, getString(R.string.ALL_FILES_GRANT_SUB),
                               getString(R.string.BADGE_RECOMMENDED), this::askAllFilesAccess);
        if (pickLibrary)
            choice(list, R.string.LIB_SAF, getString(R.string.LIB_SAF_SUB), null, this::openTreePicker);
        else
            choice(list, R.string.ALL_FILES_COPY, getString(R.string.ALL_FILES_COPY_SUB), null, this::openTreePicker);
        for (int i = 0; i < list.getChildCount(); i++) {
            MenuRow r = (MenuRow)list.getChildAt(i);
            onFocus(r, () -> describe(r.description, why, Theme.TEXT_DIM));
        }
        defaultFocus = grant;
        describe(grant.description, why, Theme.TEXT_DIM);
        addHint("A", R.string.HINT_CHOOSE, this::clickFocused);
        addHint("B", R.string.HINT_BACK, this::back);
    }

    private void clickFocused() {
        View f = focused();
        if (f != null)
            f.performClick();
        else
            restoreFocus(true);
    }

    // ---- IMPORT ------------------------------------------------------------------------------------------------

    /** The import: a plain ring on the left, what is copied and how much on the right; B cancels (nothing to focus). */
    private void buildImport() {
        screenTitle.setText(R.string.IMPORT_TITLE);
        LinearLayout h = rowLayout();
        h.setGravity(Gravity.CENTER_VERTICAL);
        ring = new Widgets.Ring(this);
        h.addView(ring, lp(560, 560));

        LinearLayout c = column();
        importHead = Theme.text(this, Theme.T_TITLE, Theme.MEDIUM, Theme.TEXT, getString(importLabel));
        c.addView(importHead, lp(MATCH, WRAP));
        importBytes = Theme.text(this, Theme.T_BODY, Theme.MEDIUM, Theme.TEXT, "");
        LinearLayout.LayoutParams bl = lp(MATCH, WRAP);
        bl.topMargin = Theme.S3;
        c.addView(importBytes, bl);
        importFile = Theme.text(this, Theme.T_SMALL, Theme.REGULAR, Theme.TEXT_DIM, "");
        importFile.setSingleLine(true);
        importFile.setEllipsize(TextUtils.TruncateAt.MIDDLE);
        LinearLayout.LayoutParams fl = lp(MATCH, WRAP);
        fl.topMargin = Theme.S2;
        c.addView(importFile, fl);
        TextView free = Theme.text(this, Theme.T_SMALL, Theme.REGULAR, Theme.TEXT_DIM,
                                   getString(R.string.FREE_SPACE_APP, size(freeSpace())));
        LinearLayout.LayoutParams frl = lp(MATCH, WRAP);
        frl.topMargin = Theme.S2;
        c.addView(free, frl);
        TextView keeps = Theme.body(this, getString(R.string.IMPORT_KEEPS));
        LinearLayout.LayoutParams kl = lp(MATCH, WRAP);
        kl.topMargin = Theme.S5;
        c.addView(keeps, kl);
        LinearLayout.LayoutParams cl = lp(0, WRAP, 1);
        cl.leftMargin = Theme.S6;
        h.addView(c, cl);
        body.addView(h, new FrameLayout.LayoutParams(MATCH, MATCH));
        addHint("B", R.string.CANCEL, this::requestCancel);
        ring.set(-1, "", getString(R.string.IMPORT_STARTING), "");
    }

    private void requestCancel() {
        if (worker == null || !worker.isAlive() || cancelRequested)
            return;
        cancelRequested = true;
        setNote(getString(R.string.CANCELING), NOTE_INFO);
    }

    // ---- HOME --------------------------------------------------------------------------------------------------

    /**
     * The Home layout, shared by Home, a library game's Home and Preparing: the art tile on the left, and on the right
     * the game's title (Display) over a column the screen fills (returned). No top bar: the title is the heading.
     */
    private LinearLayout hero(String title, boolean empty) {
        return hero(title, empty, art);
    }

    private LinearLayout hero(String title, boolean empty, Bitmap art) {
        LinearLayout hero = rowLayout();
        hero.setGravity(Gravity.CENTER_VERTICAL);
        Widgets.ArtTile tile = new Widgets.ArtTile(this);
        if (empty)
            tile.setEmpty();
        else if (art != null)
            tile.setArt(art, artColor);
        else
            tile.setMonogram(TitleArt.initials(title), artColor);
        hero.addView(tile, lp(Theme.ART, Theme.ART));
        LinearLayout info = column();
        TextView big = Theme.text(this, Theme.T_DISPLAY, Theme.MEDIUM, Theme.TEXT, title);
        big.setMaxLines(2);
        big.setEllipsize(TextUtils.TruncateAt.END);
        big.setLineSpacing(0, 1.05f);
        info.addView(big, lp(MATCH, WRAP));
        LinearLayout.LayoutParams il = lp(0, WRAP, 1);
        il.leftMargin = Theme.S5;
        hero.addView(info, il);
        body.addView(hero, new FrameLayout.LayoutParams(MATCH, MATCH));
        return info;
    }

    /** The line under the title: a status (a dot in its colour) when dot is not 0, else quiet text. */
    private View heroLine(LinearLayout info, String text, int dot) {
        View v;
        if (dot != 0) {
            Widgets.Chip c = new Widgets.Chip(this);
            c.set(text, dot);
            v = c;
        } else {
            TextView t = Theme.text(this, Theme.T_BODY, Theme.REGULAR, Theme.TEXT_DIM, text);
            t.setMaxLines(2);
            t.setEllipsize(TextUtils.TruncateAt.END);
            t.setLineSpacing(0, 1.2f);
            v = t;
        }
        LinearLayout.LayoutParams p = lp(dot != 0 ? WRAP : MATCH, WRAP);
        p.topMargin = Theme.S2;
        info.addView(v, p);
        return v;
    }

    /** The row of buttons under the title: one solid primary and quiet pills. */
    private LinearLayout heroButtons(LinearLayout info) {
        LinearLayout row = new Widgets.PillRow(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setClipChildren(false);
        LinearLayout.LayoutParams p = lp(MATCH, Theme.BUTTON_H);
        p.topMargin = Theme.S5;
        info.addView(row, p);
        return row;
    }

    /** The primary button is at least this wide (Play, Prepare). */
    static final int PRIMARY_MIN_W = 300;

    private Widgets.PillButton pill(LinearLayout row, Icon icon, String label, boolean primary, Runnable click) {
        Widgets.PillButton b = new Widgets.PillButton(this);
        b.set(icon, label, primary);
        b.onPress = () -> feedback.play(UiFeedback.Cue.SELECT, b);
        if (click != null)
            b.setOnClickListener(v -> click.run());
        LinearLayout.LayoutParams p = lp(primary ? Math.max(PRIMARY_MIN_W, b.naturalWidth()) : WRAP, Theme.BUTTON_H);
        p.leftMargin = row.getChildCount() == 0 ? 0 : Theme.S2;
        row.addView(b, p);
        return focusable(b);
    }

    /** Home's quiet pills: Settings, Controls, Manage. */
    private void homePills(LinearLayout row) {
        pill(row, Icon.SLIDERS, getString(R.string.SETTINGS), false, () -> go(Screen.SETTINGS));
        if (state() != State.NONE && titleId() != null)   // XBA-188: this title's own values
            pill(row, null, getString(R.string.GAME_SETTINGS), false, () -> go(Screen.GAME_SETTINGS));
        pill(row, Icon.PAD, getString(R.string.CONTROLS), false, () -> go(Screen.CONTROLS));
        pill(row, Icon.DISC, getString(R.string.HOME_MANAGE), false, () -> go(Screen.GAME_DATA));
    }

    /** « Yesterday · 12 h 40 min played », or « Not played yet ». */
    private String homeMeta(long last, long playMs) {
        if (last <= 0 && playMs <= 0)
            return getString(R.string.HOME_META_NEW);
        return getString(R.string.HOME_META, lastPlayedText(last, System.currentTimeMillis()), playTimeText(playMs));
    }

    /**
     * Home: the game's art and title, one line (when it was played, or its state when it is not ready), and one row of
     * buttons: a solid Play (or Prepare, Choose…) and three quiet pills (Settings, Controls, Manage). Start plays too.
     */
    private void buildHome() {
        State st = state();
        if (st == State.LIBRARY) {
            buildHomeNotHere();
            return;
        }
        String dir = gameDir();
        boolean running = gameRunning();
        Prep prep = prep();
        PrepareProtocol.Status ps = prep == Prep.OFF || prep == Prep.PLAYABLE ? null : readPrepStatus();
        boolean preparing = prep != Prep.OFF && prep != Prep.PLAYABLE;
        checkHashLater(dir);
        sizeLater(dir);

        String title = st == State.NONE ? getString(R.string.NO_GAME_HEAD) : gameTitle(dir);
        LinearLayout info = hero(title, st == State.NONE);
        String tid = st == State.NONE ? null : titleId();
        SelfHeal.State heal = st == State.READY && !preparing && tid != null ? selfHeal(tid, running, prep) : null;
        // XBA-127: another default.xbe than the pack's (never run), or a pack from an older toolkit (still plays).
        if (st == State.READY)
            xbeShaLater(dir, tid);
        boolean otherXbe = st == State.READY && preparing && prep == Prep.NEEDED && otherXbe(tid);
        boolean olderToolkit = st == State.READY && prep == Prep.PLAYABLE && heal == null && olderToolkit(tid);
        // WP6: the Enhancements switch differs from the pack's: offered like a newer toolkit (Play stays first).
        boolean enhChanged = st == State.READY && prep == Prep.PLAYABLE && heal == null && !olderToolkit
                             && enhancementsChanged(tid);
        olderToolkit |= enhChanged;
        // XBA-129: a self-healing round in the background (Play stays), or one to start / offer after a good run.
        PrepareProtocol.Status healing = prep == Prep.PLAYABLE && !running ? backgroundPrepare(tid) : null;
        boolean improve = heal != null && heal.offer == SelfHeal.Offer.IMPROVE;
        if (improve)
            heal = null;   // a run that ended well: Home as usual, plus the note and (setting off) a quiet button

        // XBA-176: XRecomp's pack being copied into this game's own app (instead of « Needs preparing »).
        boolean copying = st == State.READY && copying(tid);
        // XBA-82: the game launched another image of its disc, not prepared yet: offered (Play starts default.xbe).
        String imagePending = st == State.READY && prep == Prep.PLAYABLE && !running && !copying ? pendingImage()
                                                                                                  : null;
        if (imagePending != null && PendingImage.built(Pack.packDir(getFilesDir(), tid), imagePending))
            imagePending = null;   // prepared already: Play goes on into it (launchShell)

        // The one line: the state when it is not « Ready », else when it was last played.
        String statusText = null;
        int statusColor = Theme.OK;
        if (copying) {
            statusText = getString(R.string.HANDOVER_COPYING, handover.pct());
            statusColor = Theme.ACCENT;
            View line = heroLine(info, statusText, statusColor);
            if (line instanceof Widgets.Chip)
                homeStatus = (Widgets.Chip)line;
        } else if (st == State.READY && preparing) {
            switch (prep) {
            case RUNNING:
                statusText = ps == null ? getString(R.string.PREPARE_STARTING) : progressLine(ps);
                break;
            case QUEUED:
                statusText = queuedLabel(tid);
                statusColor = Theme.ACCENT;
                break;
            case FAILED:
                statusText = getString(R.string.CHIP_PREPARE_FAILED)
                             + (ps == null || ps.code.isEmpty() ? "" : " · " + ps.code);
                statusColor = Theme.ERROR;
                break;
            default:
                statusText = otherXbe ? getString(R.string.HOME_OTHER_XBE)
                                      : getString(R.string.HOME_NEEDS_PREPARE, getString(R.string.CHIP_NEEDS_PREPARE),
                                                  estimate());
                statusColor = Theme.WARN;
                break;
            }
            View line = heroLine(info, statusText, statusColor);
            if (prep == Prep.RUNNING)
                homeStatus = (Widgets.Chip)line;
        } else if (imagePending != null) {
            statusText = getString(R.string.HOME_IMAGE_PENDING, imagePending);
            statusColor = Theme.WARN;
            heroLine(info, statusText, statusColor);
        } else if (st == State.READY && heal != null) {
            statusText = heal.offer == SelfHeal.Offer.PREPARE_AGAIN ? getString(R.string.HOME_CRASHED_FINDS, heal.seeds)
                                                                  : getString(R.string.HOME_NEEDS_FIX);
            statusColor = heal.offer == SelfHeal.Offer.PREPARE_AGAIN ? Theme.WARN : Theme.ERROR;
            heroLine(info, statusText, statusColor);
        } else if (healing != null) {
            statusText = getString(R.string.HOME_IMPROVING, progressLine(healing));
            statusColor = Theme.ACCENT;
            heroLine(info, statusText, statusColor);
        } else if (olderToolkit) {
            statusText = getString(enhChanged ? R.string.HOME_ENHANCEMENTS_CHANGED : R.string.HOME_OLDER_TOOLKIT);
            statusColor = Theme.WARN;
            heroLine(info, statusText, statusColor);
        } else if (st == State.READY) {
            Library.Record rec = rec();
            heroLine(info, homeMeta(rec != null ? rec.lastPlayed : GameStore.lastPlayed(this),
                                    rec != null ? rec.playMs : GameStore.playTime(this)), 0);
        } else {
            statusText = getString(stateLabel(st));
            statusColor = st == State.NONE ? Theme.TEXT_DIM : stateColor(st);
            if (st != State.NONE)
                heroLine(info, statusText, statusColor);
            heroLine(info, stateText(st, dir), 0);
        }

        LinearLayout buttons = heroButtons(info);
        Widgets.PillButton play;
        if (st == State.NONE || st == State.MISSING || st == State.NO_ACCESS) {
            play = pill(buttons, null, getString(st == State.NONE ? R.string.CHOOSE_GAME : R.string.CHOOSE_AGAIN),
                        true, () -> go(Screen.SOURCE));
        } else if (copying) {
            play = pill(buttons, null, getString(R.string.HANDOVER_COPYING_BUTTON), true, this::onPlay);
            play.setDim(true);
        } else if (preparing && prep == Prep.QUEUED) {
            play = pill(buttons, null, queuedLabel(tid), true, this::onPlay);
            homePrep = prep;
        } else if (preparing) {
            int t = prep == Prep.RUNNING ? R.string.PREPARING : prep == Prep.FAILED ? R.string.PREPARE_SEE_WHY
                    : otherXbe ? R.string.PREPARE_AGAIN : R.string.PREPARE;
            play = pill(buttons, null, getString(t), true, this::onPlay);
            homePrep = prep;
        } else if (imagePending != null) {
            play = pill(buttons, null, getString(R.string.PREPARE_IMAGE, imagePending), true, this::onPrepareImage);
            pill(buttons, Icon.PLAY, getString(R.string.PLAY), false, this::onPlay);
        } else if (heal != null && heal.offer == SelfHeal.Offer.PREPARE_AGAIN) {
            play = pill(buttons, null, getString(R.string.PREPARE_AGAIN), true, this::onPrepareAgain);
            homeAgain = true;
            pill(buttons, Icon.PLAY, getString(R.string.PLAY), false, this::onPlay);
        } else {
            play = pill(buttons, Icon.PLAY, getString(R.string.PLAY), true, this::onPlay);
            play.setDim(st != State.READY);
            if (olderToolkit)   // offered, never forced: Play stays first (a Prepare takes 10-30 min)
                pill(buttons, null, getString(R.string.PREPARE_AGAIN), false, this::onPrepareNewToolkit);
            else if (healing != null)
                pill(buttons, null, getString(R.string.SELF_HEAL_STOP), false, this::onStopHealing);
            else if (improve && !SelfHeal.auto(this))
                pill(buttons, null, getString(R.string.PREPARE_AGAIN), false, this::onImprove);
        }
        homePrimary = play;
        homePills(buttons);
        if (companion != null) {
            companion.art(st == State.NONE ? null : art);
            companion.home(st == State.NONE ? "" : title,
                           statusText != null ? statusText : getString(R.string.STATE_READY),
                           statusText != null ? statusColor : Theme.OK, !play.dim, play.title);
        }

        defaultFocus = play;
        startAction = homeAgain ? this::onPrepareAgain : this::onPlay;
        addHint("A", R.string.HINT_SELECT, this::clickFocused);
        addHint("B", libraryOn() && history.isEmpty() ? R.string.LIB_TITLE : history.isEmpty() ? R.string.HINT_QUIT
                                                                                                : R.string.HINT_BACK,
                this::back);
        if (st != State.NONE && rec() != null)
            favouriteHint(currentEntry());   // XBA-206: X adds the game to the favourites

        if (copying)
            setNote(getString(R.string.HANDOVER_COPYING_NOTE), NOTE_INFO);
        else if (preparing && prep == Prep.INTERRUPTED)
            setNote(getString(R.string.PREPARE_INTERRUPTED), NOTE_WARN);
        else if (otherXbe)
            setNote(getString(R.string.HOME_OTHER_XBE_NOTE), NOTE_WARN);
        else if (heal != null && heal.offer == SelfHeal.Offer.PREPARE_AGAIN)
            setNote(getString(R.string.HOME_PREPARE_AGAIN_NOTE), NOTE_WARN);
        else if (healing != null)
            setNote(getString(R.string.HOME_IMPROVING_NOTE), NOTE_INFO);
        else if (improve)
            setNote(getString(R.string.HOME_IMPROVE_NOTE, SelfHeal.read(workDir(tid)).seeds), NOTE_INFO);
        else if (heal != null)
            setNote(getString(R.string.HOME_NEEDS_FIX_NOTE, heal.blocker), NOTE_ERROR);
        else if (running && selectedIsLaunched())
            setNote(getString(R.string.GAME_RUNNING_HOME), NOTE_INFO);
        else if (running && st == State.READY)
            setNote(getString(R.string.LIB_OTHER_RUNNING, runningTitle()), NOTE_INFO);
        else if (handoverNoteText != null && st != State.NONE) {
            setNote(handoverNoteText, handoverNoteKind);
            if (handoverNoteKind == NOTE_OK)
                handoverNoteText = null;   // « copied »: said once
        } else if (olderToolkit)
            setNote(getString(enhChanged ? R.string.HOME_ENHANCEMENTS_CHANGED_NOTE : R.string.HOME_OLDER_TOOLKIT_NOTE,
                              estimate()), NOTE_INFO);
        else if (st == State.READY && !preparing && (rec() != null ? rec().lastPlayed : GameStore.lastPlayed(this)) <= 0)
            setNote(getString(R.string.HOME_LEAD_READY), NOTE_INFO);   // before the first play: where the menu is
        // XBA-129: the setting on, the launcher starts the round itself (a note says so, the Prepare can be stopped).
        boolean crashRound = heal != null && heal.offer == SelfHeal.Offer.PREPARE_AGAIN;
        if (SelfHeal.auto(this) && !running && (improve || crashRound))
            poller.post(() -> autoRound(tid));
        else if (!roundChecked && prep == Prep.PLAYABLE && healing == null && !running && tid != null
                 && SelfHeal.read(workDir(tid)).roundOpen) {
            roundChecked = true;   // its Prepare ended while the launcher was away: count it, relaunch if it helped
            poller.post(() -> {
                if (!relaunchAfterPrepare(tid) && screen == Screen.HOME)
                    show(Screen.HOME, 0);   // the round's outcome, its note included
            });
        }
    }

    private String lastPlayedText(long last, long now) {
        return last <= 0 ? getString(R.string.VAL_NEVER)
                         : DateUtils.getRelativeTimeSpanString(last, now, DateUtils.MINUTE_IN_MILLIS).toString();
    }

    /** « 45 min », « 3 h 05 », « — » for none. */
    private String playTimeText(long ms) {
        long min = ms / 60_000;
        if (min <= 0)
            return ms > 0 ? getString(R.string.PLAYTIME_MIN, 1) : getString(R.string.VAL_NONE);
        if (min < 60)
            return getString(R.string.PLAYTIME_MIN, (int)min);
        return getString(R.string.PLAYTIME_H, (int)(min / 60), String.format(Locale.ROOT, "%02d", min % 60));
    }

    /** The pack's prepared date (pack.json's built_at, else the file's date) and its library's size. */
    private String[] packFacts(String tid) {
        File dir = Pack.packDir(getFilesDir(), tid);
        File json = new File(dir, Pack.JSON);
        long when = json.lastModified();
        long bytes = -1;
        try {
            JSONObject o = new JSONObject(PrepareProtocol.readText(json, 1 << 20));
            String at = o.optString("built_at", "");
            if (!at.isEmpty()) {
                try {
                    when = java.time.Instant.parse(at).toEpochMilli();
                } catch (java.time.format.DateTimeParseException ignored) {
                }
            }
            String lib = o.optString("lib", "");
            File so = new File(dir, lib);
            if (!lib.isEmpty() && so.isFile())
                bytes = so.length();
        } catch (IOException | JSONException ignored) {
        }
        java.util.Locale loc = getResources().getConfiguration().getLocales().get(0);
        String date = when > 0 ? java.text.DateFormat.getDateInstance(java.text.DateFormat.MEDIUM, loc).format(when)
                               : "?";
        return new String[] { date, bytes >= 0 ? size(bytes) : "?" };
    }

    private void onPlay() {
        if (screen == Screen.ROM_HOME && romEntry != null) {   // the bottom screen's Play on a GBA game's Home
            launchRom(romEntry);
            return;
        }
        State st = state();
        if (st == State.LIBRARY) {
            libraryAction(currentEntry());
            return;
        }
        if (st == State.READY) {
            if (copying(titleId())) {   // XBA-176: a few seconds more
                setNote(getString(R.string.HANDOVER_BUSY), NOTE_INFO);
                return;
            }
            Prep p = prep();
            if (p == Prep.RUNNING || p == Prep.FAILED) {
                go(Screen.PREPARE);
                return;
            }
            if (p == Prep.QUEUED) {   // A takes it out of the queue (a new start would only keep its place)
                dequeuePrepare(titleId());
                return;
            }
            if (p != Prep.OFF && p != Prep.PLAYABLE) {
                startPrepare();
                return;
            }
            if (xbeChanged()) {   // XBA-127: Home now says so and offers Prepare again
                show(Screen.HOME, 0);
                return;
            }
            if (!confirmSwitch())
                return;
            launchShell(false);
            return;
        }
        if (st == State.WRONG) {
            setNote(getString(R.string.WRONG_GAME_HOME), NOTE_ERROR);
            return;
        }
        String why = stateText(st, gameDir());
        go(Screen.SOURCE);
        setNote(why, NOTE_WARN);
    }

    // ---- SETTINGS ----------------------------------------------------------------------------------------------

    /** Settings' tabs: General (the launcher, the bottom screen, the game theme, the language), Graphics, Audio, Advanced. */
    static final int TAB_GENERAL = 0, TAB_GRAPHICS = 1, TAB_AUDIO = 2, TAB_ADVANCED = 3, TABS = 4;
    private int settingsTab;
    /** The focus of each tab when it was left (an index in its focusables; -1: its first row). */
    private final int[] tabFocus = { -1, -1, -1, -1 };
    /** True while a tab switch rebuilds Settings: show() does not overwrite the new tab's focus memory. */
    private boolean switchingTab;
    /** The option rows of the tab shown. */
    private final java.util.Map<RecompConfig.Option, MenuRow> optionRows = new java.util.LinkedHashMap<>();

    int settingsTabForTest() {
        return settingsTab;
    }

    private static RecompConfig.Section tabSection(int tab) {
        switch (tab) {
        case TAB_GRAPHICS:
            return RecompConfig.Section.GRAPHICS;
        case TAB_AUDIO:
            return RecompConfig.Section.AUDIO;
        case TAB_ADVANCED:
            return RecompConfig.Section.ADVANCED;
        default:
            return RecompConfig.Section.GAME;
        }
    }

    private static final int[] TAB_LABELS = { R.string.SECTION_GENERAL, R.string.SECTION_GRAPHICS,
                                               R.string.SECTION_AUDIO, R.string.SECTION_ADVANCED };

    /**
     * Settings: four tabs (L1 / R1), each a short list of « label  value » rows (left / right change a value, A steps
     * it), the focused row's description on the right. No tab needs scrolling on a 1080p stage. Y puts the options
     * back to their defaults; « Reset all » and About end the Advanced tab.
     */
    private void buildSettings() {
        screenTitle.setText(R.string.SETTINGS);
        cfg = RecompConfig.load(GameStore.externalFilesDir(this));
        optionRows.clear();
        String[] names = new String[TABS];
        for (int i = 0; i < TABS; i++)
            names[i] = getString(TAB_LABELS[i]);
        topTabs.addView(new Widgets.Tabs(this, names, settingsTab, i -> switchTab(i - settingsTab)),
                        lp(WRAP, MATCH));
        LinearLayout list = listTemplate();

        List<RecompConfig.Option> shown = RecompConfig.shownOptions(lsfgBuilt());
        lsfgOff = lsfgForcedOff();
        if (settingsTab == TAB_GENERAL) {
            if (TitleApp.ownTitleId(this) == null)   // a game's own app always goes straight into its game
                launcherToggle(list, R.string.OPEN_DIRECTLY, R.string.OPEN_DIRECTLY_DETAIL, GameStore::autoStart,
                               GameStore::setAutoStart, R.string.OPEN_DIRECTLY_ON, R.string.OPEN_DIRECTLY_OFF);
            launcherToggle(list, R.string.COVER_DOWNLOAD, R.string.COVER_DOWNLOAD_DETAIL, CoverArt::downloadOn,
                           CoverArt::setDownloadOn, R.string.COVER_DOWNLOAD_ON, R.string.COVER_DOWNLOAD_OFF);
            launcherToggle(list, R.string.UI_SOUND, R.string.UI_SOUND_DETAIL, GameStore::uiSound,
                           GameStore::setUiSound, R.string.UI_SOUND_ON, R.string.UI_SOUND_OFF);
            launcherToggle(list, R.string.UI_HAPTICS, R.string.UI_HAPTICS_DETAIL, GameStore::uiHaptics,
                           GameStore::setUiHaptics, R.string.UI_HAPTICS_ON, R.string.UI_HAPTICS_OFF);
            if (Toolchain.available(this))   // XBA-129: only where the app prepares games
                launcherToggle(list, R.string.SELF_HEAL, R.string.SELF_HEAL_DETAIL, SelfHeal::auto, SelfHeal::setAuto,
                               R.string.SELF_HEAL_ON, R.string.SELF_HEAL_OFF);
            if (DualScreen.available(this, CompanionScreen.ownDisplayId(this)))
                bottomScreenRow(list);
            String dir = gameDir();
            if (dir != null && state() != State.NONE && GameStore.findXbe(new File(dir)) != null && titleId() != null) {
                gameThemeRow(list, titleId(), gameTitle(dir));
                // WP6: only for a game the toolkit has enhancements for, as its pack says (after a first Prepare).
                if (Toolchain.available(this) && Pack.titleLayer(getFilesDir(), titleId()) != null)
                    enhancementsRow(list, titleId(), gameTitle(dir));
            }
        }
        RecompConfig.Section section = tabSection(settingsTab);
        // XBA-181: what every console reads under « All consoles », the Xbox's own under « Xbox only ».
        List<RecompConfig.Option> tab = RecompConfig.tabOptions(shown, section);
        boolean grouped = RecompConfig.grouped(tab, section);
        // The Vulkan driver row ends « All consoles » (any console's renderer may use it; for now the N64's).
        boolean vkPending = settingsTab == TAB_GRAPHICS;
        for (int oi = 0; oi < tab.size(); oi++) {
            RecompConfig.Option o = tab.get(oi);
            if (vkPending && grouped && oi > 0 && tab.get(oi - 1).shared && !o.shared) {
                vkDriverRow(list);
                vkPending = false;
            }
            if (grouped && (oi == 0 || tab.get(oi - 1).shared != o.shared))
                listLabel(list, getString(o.shared ? R.string.SET_GROUP_ALL : R.string.SET_GROUP_XBOX));
            MenuRow r = listRow(list, getString(o.title), getString(o.detail), null);
            r.onPress = null;   // a press steps the value: the « move » tick, not « select »
            String[] labels = new String[o.choices.length + 1];
            for (int i = 0; i < o.choices.length; i++)
                labels[i] = getString(o.choices[i].label);
            labels[o.choices.length] = getString(R.string.VAL_CUSTOM);
            r.setChoices(labels);
            optionRows.put(o, r);
            refreshOptionRow(r, o);
            r.cycler = d -> stepOption(r, o, d);
            r.setOnClickListener(v -> stepOption(r, o, 1));
            onFocus(r, () -> optionDetail(o));
            if (o.needsLsfg && lsfgOff) {
                // The game's pack forces it off (pack.json env_force RECOMP_LSFG=0): the setting stays, for the
                // other games, but is dimmed and its description says it does nothing for this one.
                r.setDim(true);
                r.setDescription(getString(R.string.LSFG_TITLE_UNAVAILABLE));
            }
            if (o.needsLsfg)
                lsfgDllRow(list);
        }
        refreshRendererDims();
        if (vkPending)
            vkDriverRow(list);
        if (settingsTab == TAB_ADVANCED) {
            MenuRow reset = listRow(list, getString(R.string.RESET), getString(R.string.RESET_DETAIL),
                                    this::resetSettings);
            onFocus(reset, () -> describe(getString(R.string.RESET_DETAIL),
                                          getString(R.string.SETTINGS_FOOTER, cfg.file().getAbsolutePath()),
                                          Theme.TEXT_DIM));
            MenuRow about = listRow(list, getString(R.string.ABOUT), getString(R.string.ABOUT_TEXT),
                                    () -> go(Screen.ABOUT));
            about.setValue(version());
        }

        l1Action = () -> switchTab(-1);
        r1Action = () -> switchTab(1);
        yAction = this::resetSettings;
        addHint("A", R.string.HINT_CHANGE, this::clickFocused);
        addHint("Y", R.string.HINT_DEFAULTS, this::resetSettings);
        addHint("B", R.string.HINT_BACK, this::back);
        if (gameRunning())
            setNote(getString(R.string.SETTINGS_GAME_RUNNING), NOTE_INFO);
    }

    /** L1 / R1 (and a tap on a tab): the previous or next tab, wrapping; each tab keeps its own focus. */
    private void switchTab(int dir) {
        if (screen != Screen.SETTINGS || dir == 0)
            return;
        tabFocus[settingsTab] = focusables.indexOf(focused());
        settingsTab = ((settingsTab + dir) % TABS + TABS) % TABS;
        int f = tabFocus[settingsTab];
        if (f >= 0)
            focusMemory.put(Screen.SETTINGS, f);
        else
            focusMemory.remove(Screen.SETTINGS);
        feedback.play(UiFeedback.Cue.MOVE, host);
        switchingTab = true;
        show(Screen.SETTINGS, dir > 0 ? 1 : -1);
        switchingTab = false;
    }

    /**
     * Frame cap and Resolution only apply to the GPU renderer: dimmed (still changeable) otherwise. Auto is the GPU
     * renderer (the runtime's default on Android, XBA-119) unless the game's pack forces RECOMP_GPU=0, which wins
     * over any choice here.
     */
    private void refreshRendererDims() {
        String tid = titleId();
        boolean gpu = tid == null || !Pack.forcesOff(getFilesDir(), tid, "RECOMP_GPU");
        for (RecompConfig.Option o : RecompConfig.OPTIONS) {
            if (o.title == R.string.OPT_GPU) {
                int i = cfg.choiceIndex(o);
                if (i >= 0 && o.choices[i].label == R.string.VAL_GPU_CPU)
                    gpu = false;
            }
        }
        for (java.util.Map.Entry<RecompConfig.Option, MenuRow> e : optionRows.entrySet()) {
            int t = e.getKey().title;
            if (t == R.string.OPT_GPU_FPS_CAP || t == R.string.OPT_GPU_SCALE)
                e.getValue().setDim(!gpu);
        }
    }

    /**
     * « Bottom screen: Panel / Black / Off » (GameStore.bottomScreen), only on a device with a second display: what
     * the launcher and the game show there (docs/android/app-shell.md « Dual screen »). Applied at once.
     */
    private void bottomScreenRow(LinearLayout list) {
        GameStore.BottomScreen[] modes = GameStore.BottomScreen.values();
        int[] labels = { R.string.VAL_PANEL, R.string.VAL_BLACK, R.string.VAL_OFF };
        String[] names = new String[modes.length];
        for (int i = 0; i < modes.length; i++)
            names[i] = getString(labels[i]);
        MenuRow r = settingRow(list, getString(R.string.BOTTOM_SCREEN), getString(R.string.BOTTOM_SCREEN_DETAIL));
        r.setChoices(names);
        Runnable refresh = () -> r.setValue(names[GameStore.bottomScreen(this).ordinal()]);
        refresh.run();
        r.cycler = d -> {
            int n = modes.length, i = ((GameStore.bottomScreen(this).ordinal() + d) % n + n) % n;
            GameStore.setBottomScreen(this, modes[i]);
            refresh.run();
            feedback.play(UiFeedback.Cue.MOVE, r);
            if (companion != null)
                companion.screen().refresh();
            setNote(getString(R.string.BOTTOM_SCREEN_SAVED, names[i]), NOTE_OK);
            Log.i(TAG, "launcher: bottom screen " + modes[i]);
        };
        r.setOnClickListener(v -> r.cycler.step(1));
    }

    /**
     * Settings › General › Game theme, per title: « Art and colours », « Colours only », « Off ». Applies at once
     * here (the stage is built again in the new colours), in the game's menu at its next start.
     */
    private void gameThemeRow(LinearLayout list, String tid, String title) {
        GameTheme.Mode[] modes = GameTheme.Mode.values();
        int[] labels = { R.string.VAL_THEME_FULL, R.string.VAL_THEME_COLORS, R.string.VAL_THEME_OFF };
        String[] names = new String[modes.length];
        for (int i = 0; i < modes.length; i++)
            names[i] = getString(labels[i]);
        MenuRow r = settingRow(list, getString(R.string.GAME_THEME), getString(R.string.GAME_THEME_DETAIL, title));
        r.setChoices(names);
        r.setValue(names[GameTheme.mode(this, tid).ordinal()]);
        r.cycler = d -> {
            int n = modes.length, i = ((GameTheme.mode(this, tid).ordinal() + d) % n + n) % n;
            GameTheme.setMode(this, tid, modes[i]);
            feedback.play(UiFeedback.Cue.MOVE, r);
            Log.i(TAG, "launcher: game theme " + tid + " " + modes[i].key);
            show(screen, 0);   // syncTheme: the new colours, the focus stays on this row
            setNote(getString(R.string.GAME_THEME_SAVED, names[i]), NOTE_OK);
        };
        r.setOnClickListener(v -> r.cycler.step(1));
    }

    /**
     * Settings › General › Enhancements, per title (profile-free WP6): the toolkit's optional extras for the game
     * (profiles/titles/<id>), on by default. Off, the next Prepare passes XBA_NO_TITLE_LAYER=1; Home then offers
     * Prepare again (enhancementsChanged).
     */
    private void enhancementsRow(LinearLayout list, String tid, String title) {
        MenuRow r = settingRow(list, getString(R.string.ENHANCEMENTS), getString(R.string.ENHANCEMENTS_DETAIL, title));
        r.setChoices(new String[] { getString(R.string.VAL_ON), getString(R.string.VAL_OFF) });
        Runnable refresh = () -> r.setValue(getString(GameStore.enhancements(this, tid) ? R.string.VAL_ON
                                                                                         : R.string.VAL_OFF));
        refresh.run();
        r.cycler = d -> {
            boolean on = !GameStore.enhancements(this, tid);
            GameStore.setEnhancements(this, tid, on);
            refresh.run();
            feedback.play(UiFeedback.Cue.MOVE, r);
            setNote(getString(on ? R.string.ENHANCEMENTS_ON : R.string.ENHANCEMENTS_OFF), NOTE_OK);
            Log.i(TAG, "launcher: enhancements " + tid + " " + (on ? "on" : "off"));
        };
        r.setOnClickListener(v -> r.cycler.step(1));
    }

    /** A setting row: its press steps the value, which plays the « move » tick instead of « select ». */
    private MenuRow settingRow(LinearLayout list, String title, String description) {
        MenuRow r = listRow(list, title, description, null);
        r.onPress = null;
        return r;
    }

    /** An on / off launcher preference as a setting row. */
    private void launcherToggle(LinearLayout list, int title, int detailText,
                                Function<android.content.Context, Boolean> get,
                                BiFunction<android.content.Context, Boolean, Boolean> set, int onNote, int offNote) {
        MenuRow r = settingRow(list, getString(title), getString(detailText));
        r.setChoices(new String[] { getString(R.string.VAL_ON), getString(R.string.VAL_OFF) });
        Runnable refresh = () -> r.setValue(getString(get.apply(this) ? R.string.VAL_ON : R.string.VAL_OFF));
        refresh.run();
        r.cycler = d -> {
            set.apply(this, !get.apply(this));
            refresh.run();
            feedback.play(UiFeedback.Cue.MOVE, r);
            setNote(getString(get.apply(this) ? onNote : offNote), NOTE_OK);
            Log.i(TAG, "launcher: " + getResources().getResourceEntryName(title) + " "
                       + (get.apply(this) ? "on" : "off"));
        };
        r.setOnClickListener(v -> r.cycler.step(1));
    }

    private String choiceLabel(RecompConfig c, RecompConfig.Option o) {
        int i = c.choiceIndex(o);
        return i < 0 ? getString(R.string.VAL_CUSTOM) : getString(o.choices[i].label);
    }

    private void refreshOptionRow(MenuRow r, RecompConfig.Option o) {
        r.setValue(choiceLabel(cfg, o));
    }

    private void stepOption(MenuRow r, RecompConfig.Option o, int dir) {
        int n = o.choices.length, i = cfg.choiceIndex(o);
        int next = i < 0 ? o.defaultIndex() : ((i + dir) % n + n) % n;
        cfg.set(o, o.choices[next]);
        saveSettings();
        refreshOptionRow(r, o);
        if (o.title == R.string.OPT_GPU)
            refreshRendererDims();
        feedback.play(UiFeedback.Cue.MOVE, r);
    }

    private void saveSettings() {
        try {
            cfg.save();
            Log.i(TAG, "launcher: settings saved to " + cfg.file());
            setNote(getString(gameRunning() ? R.string.SETTINGS_SAVED_RUNNING : R.string.SETTINGS_SAVED), NOTE_OK);
        } catch (IOException e) {
            Log.w(TAG, "launcher: cannot save settings", e);
            setNote(getString(R.string.SETTINGS_SAVE_FAILED, String.valueOf(e.getMessage())), NOTE_ERROR);
        }
    }

    /** Y, or « Reset all »: every option back to its default (the launcher's own preferences stay). */
    private void resetSettings() {
        cfg.resetAll();
        saveSettings();
        for (java.util.Map.Entry<RecompConfig.Option, MenuRow> e : optionRows.entrySet())
            refreshOptionRow(e.getValue(), e.getKey());
        refreshRendererDims();
        setNote(getString(R.string.RESET_DONE), NOTE_OK);
    }

    /** The focused option's description; frame generation forced off for this game says so, in WARN. */
    private void optionDetail(RecompConfig.Option o) {
        boolean off = o.needsLsfg && lsfgOff;
        describe(getString(o.detail), off ? getString(R.string.LSFG_TITLE_UNAVAILABLE) + ". "
                                            + getString(R.string.LSFG_TITLE_UNAVAILABLE_DETAIL) : null, Theme.WARN);
    }

    // ---- Frame generation: « Choose Lossless.dll » ---------------------------------------------------------------

    /** Tests: stands in for R.bool.xba_lsfg when set (the unit tests' APK is built without frame generation). */
    static Boolean lsfgBuiltOverride;

    /** The selected game's pack forces frame generation off (read when Settings is built). */
    private boolean lsfgOff;

    /** True when this APK has frame generation (-PxbaLsfg=1 with the Vulkan bridge, docs/technical/framegen.md). */
    private boolean lsfgBuilt() {
        return lsfgBuiltOverride != null ? lsfgBuiltOverride : getResources().getBoolean(R.bool.xba_lsfg);
    }

    /**
     * The selected game's pack forces frame generation off (pack.json env_force RECOMP_LSFG=0, from its title
     * profile): the runtime sets it over recomp.cfg, so the Settings row says it is unavailable for this game.
     */
    private boolean lsfgForcedOff() {
        String tid = titleId();
        return tid != null && Pack.forcesOff(getFilesDir(), tid, "RECOMP_LSFG");
    }

    /** getFilesDir()/lsfg: the shader cache base (excluded from backups and from LogProvider). */
    private File lsfgBase() {
        return new File(getFilesDir(), "lsfg");
    }

    /**
     * The state of the imported shaders, from the cache directory recomp.cfg names (RECOMP_LSFG_DIR): one line for
     * the row's value. kind gets a NOTE_* colour.
     */
    private String lsfgStatus(int[] kind) {
        String dir = cfg == null ? null : cfg.get(RecompConfig.LSFG_DIR_KEY);
        kind[0] = NOTE_INFO;
        if (dir == null || dir.isEmpty() || !new File(dir).isDirectory())
            return getString(R.string.LSFG_NO_DLL);
        long[] m;
        try {
            m = LsfgNative.readManifest(dir);
        } catch (UnsatisfiedLinkError e) {
            return getString(R.string.LSFG_NO_DLL);
        }
        return lsfgStatusOf(m, kind);
    }

    private String lsfgStatusOf(long[] m, int[] kind) {
        if (m == null || m[LsfgNative.STATUS] != 0) {
            kind[0] = NOTE_ERROR;
            return getString(R.string.LSFG_UNSUPPORTED);
        }
        boolean full = m[LsfgNative.READY] != 0, perf = m[LsfgNative.READY + 1] != 0;
        if (full || perf) {
            kind[0] = NOTE_OK;
            return getString(full ? R.string.LSFG_READY : R.string.LSFG_READY_PERF_ONLY);
        }
        if (m[LsfgNative.DXBC] >= m[LsfgNative.NEEDED] || m[LsfgNative.DXBC + 1] >= m[LsfgNative.NEEDED + 1]) {
            kind[0] = NOTE_WARN;
            return getString(R.string.LSFG_NO_TRANSLATOR);
        }
        kind[0] = NOTE_ERROR;
        return getString(R.string.LSFG_UNSUPPORTED);
    }

    /** « Choose Lossless.dll », under the frame generation option: its state as the value. */
    private void lsfgDllRow(LinearLayout list) {
        int[] kind = new int[1];
        String status = lsfgStatus(kind);
        MenuRow r = listRow(list, getString(R.string.LSFG_CHOOSE_DLL), getString(R.string.LSFG_CHOOSE_DLL_DETAIL),
                            this::openLsfgDllPicker);
        r.setValue(status);
        onFocus(r, () -> describe(getString(R.string.LSFG_CHOOSE_DLL_DETAIL), getString(R.string.LSFG_LEGAL_FOOTER),
                                  Theme.TEXT_DIM));
    }

    private void openLsfgDllPicker() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*");
        try {
            startActivityForResult(i, REQ_LSFG_DLL);
        } catch (ActivityNotFoundException e) {
            setNote(getString(R.string.PICKER_MISSING), NOTE_ERROR);
        }
    }

    private void onLsfgDllPicked(Uri uri) {
        String name = displayName(uri);
        Log.i(TAG, "launcher: Lossless.dll picked (" + name + ")");
        if (!name.toLowerCase(Locale.ROOT).endsWith(".dll")) {
            setNote(getString(R.string.LSFG_NOT_A_DLL, name), NOTE_ERROR);
            return;
        }
        runWorker(R.string.LSFG_IMPORTING, () -> importLsfgDll(uri, name));
    }

    /**
     * Worker: the DLL is read through the picker's fd only (never copied into the app's storage); its shaders go to
     * getFilesDir()/lsfg/<key> (libxba_import.so), and recomp.cfg gets RECOMP_LSFG_DIR when they are usable.
     */
    private void importLsfgDll(Uri uri, String name) throws IOException {
        long[] m;
        String[] dir = new String[1], err = new String[1];
        try (ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r")) {
            if (pfd == null)
                throw new IOException("cannot open " + name);
            File base = lsfgBase();
            if (!(base.mkdirs() || base.isDirectory()))
                throw new IOException("cannot create " + base);
            m = LsfgNative.importDll(pfd.getFd(), base.getAbsolutePath(), dir, err);
        }
        int[] kind = new int[1];
        String status;
        if (m == null || (m[LsfgNative.STATUS] != 0 && m[LsfgNative.NEEDED] == 0)) {
            status = getString(R.string.LSFG_UNSUPPORTED);
            kind[0] = NOTE_ERROR;
            Log.w(TAG, "launcher: Lossless.dll import failed: " + err[0]);
        } else {
            status = lsfgStatusOf(new long[] { 0, m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10],
                                               m[11], m[12], m[13] }, kind);
            if (m[LsfgNative.STATUS] != 0 && kind[0] == NOTE_OK)
                kind[0] = NOTE_WARN;   // one variant usable, the other failed to translate
        }
        if (kind[0] == NOTE_OK || kind[0] == NOTE_WARN) {
            RecompConfig c = RecompConfig.load(GameStore.externalFilesDir(this));
            if (dir[0] != null && !dir[0].isEmpty()) {
                c.set(RecompConfig.LSFG_DIR_KEY, dir[0]);
                c.save();
            }
        }
        Log.i(TAG, "launcher: Lossless.dll import: " + status);
        workerNoteKind = kind[0];
        workerNote = status;
    }

    // ---- Vulkan drivers (Settings › Graphics › Vulkan driver; docs/android/app-shell.md « Vulkan drivers ») -------

    /** The downloadable list shown on DRIVERS (VkDriverIndex.cached, then a refresh), or null before it opens. */
    private VkDriverIndex.Listing vkdListing;
    private VkDriverIndex vkdIndex;
    private Thread vkdRefresher;
    /** The list was asked once (when DRIVERS first opened with a copy older than 24 h): not again by itself. */
    private boolean vkdAutoAsked;
    /** The driver row DRIVERS focuses when it is built next (just installed), or null. */
    private volatile String vkdFocusId;
    /** DRIVERS: each installed driver's row (X deletes the focused one). */
    private final java.util.Map<View, VkDrivers.Driver> vkdRows = new java.util.HashMap<>();
    private String vkdArmedId;

    /** Tests: stands in for VkDriverIndex.forContext (a fake HTTP layer). */
    static java.util.function.Function<android.content.Context, VkDriverIndex> vkdIndexFactory;

    private VkDriverIndex vkdIndex() {
        if (vkdIndex == null)
            vkdIndex = vkdIndexFactory != null ? vkdIndexFactory.apply(this) : VkDriverIndex.forContext(this);
        return vkdIndex;
    }

    /** A game's own app: no list, no download, no choice of its own (it uses XRecomp's, VkDriverShare). */
    private boolean vkdInherits() {
        return TitleApp.ownTitleId(this) != null;
    }

    /** The active driver for a value: « System », or its name and version. */
    private String vkdActiveLabel(VkDrivers.Driver d) {
        return d == null ? getString(R.string.VKD_SYSTEM_SHORT) : VkDriverStore.label(d);
    }

    /**
     * Settings › Graphics › « Vulkan driver »: the active driver as the value; A opens DRIVERS (in a game's own app,
     * the driver comes from XRecomp: a note says so).
     */
    private void vkDriverRow(LinearLayout list) {
        VkDrivers drivers = VkDrivers.of(this);
        VkDrivers.Driver d = drivers.active();
        boolean inherits = vkdInherits();
        String detail = getString(inherits ? R.string.OPT_VK_DRIVER_DETAIL_GAME_APP : R.string.OPT_VK_DRIVER_DETAIL);
        MenuRow r = listRow(list, getString(R.string.OPT_VK_DRIVER), detail,
                            inherits ? () -> setNote(getString(R.string.VKD_GAME_APP_NOTE), NOTE_INFO)
                                     : () -> go(Screen.DRIVERS));
        r.setValue(vkdActiveLabel(d));
        int warn = VkDriverStore.effectiveWarning(drivers, null);
        onFocus(r, () -> describe(detail, warn != 0 ? getString(warn) : null, Theme.WARN));
    }

    /**
     * DRIVERS: « System driver », the installed drivers (A makes one active, X twice deletes it), « Import a zip »,
     * then « Refresh the list » (its state as the value; Y too) and the drivers one can download (A downloads, then
     * the new one is focused to be chosen). A game's own app never shows it.
     */
    private void buildDrivers() {
        screenTitle.setText(R.string.VKD_TITLE);
        VkDrivers drivers = VkDrivers.of(this);
        VkDrivers.Driver active = drivers.active();
        List<VkDrivers.Driver> installed = drivers.list();
        vkdRows.clear();
        vkdArmedId = null;
        LinearLayout list = listTemplate();
        String footer = getString(R.string.VKD_FOOTER);

        listLabel(list, getString(R.string.VKD_SECTION_INSTALLED));
        MenuRow sys = listRow(list, getString(R.string.VKD_SYSTEM), getString(R.string.VKD_SYSTEM_DETAIL),
                              () -> vkdSelect(null));
        if (active == null)
            sys.setBadge(getString(R.string.VKD_ACTIVE), Theme.ACCENT);
        onFocus(sys, () -> describe(sys.description, footer, Theme.TEXT_DIM));
        MenuRow focus = null;
        String want = vkdFocusId;
        vkdFocusId = null;
        for (VkDrivers.Driver d : installed) {
            MenuRow r = vkdInstalledRow(list, drivers, d, active != null && active.id.equals(d.id), footer);
            if (d.id.equals(want))
                focus = r;
        }

        MenuRow imp = listRow(list, getString(R.string.VKD_IMPORT), getString(R.string.VKD_IMPORT_DETAIL),
                              this::openVkDriverPicker);
        onFocus(imp, () -> describe(imp.description, footer, Theme.TEXT_DIM));

        if (!vkdInherits()) {
            if (vkdListing == null)
                vkdListing = vkdIndex().cached();
            // The list is asked by itself once, when the copy is older than 24 h (or missing); Refresh asks anyway.
            if (!vkdAutoAsked && VkDriverIndex.networkAllowed(this)) {
                for (VkDriverIndex.SourceStatus s : vkdListing.status) {
                    if (s.state == VkDriverIndex.State.STALE || s.state == VkDriverIndex.State.NONE) {
                        vkdAutoAsked = true;
                        vkdRefresh(false);
                        break;
                    }
                }
            }
            listLabel(list, getString(R.string.VKD_SECTION_AVAILABLE));
            boolean refreshing = vkdRefresher != null && vkdRefresher.isAlive();
            MenuRow refresh = listRow(list, getString(R.string.VKD_REFRESH), getString(R.string.VKD_REFRESH_DETAIL),
                                      () -> vkdRefresh(true));
            refresh.setValue(VkDriverStore.listState(this, vkdListing, refreshing, false));
            String state = VkDriverStore.listState(this, vkdListing, refreshing);
            onFocus(refresh, () -> describe(refresh.description + "\n\n" + state, footer, Theme.TEXT_DIM));
            List<VkDriverIndex.Entry> avail = VkDriverStore.toDownload(vkdListing, drivers);
            List<String> names = VkDriverStore.rowNames(avail);   // the same name in several releases: its tag too
            for (int i = 0; i < avail.size(); i++)
                vkdAvailableRow(list, avail.get(i), names.get(i), footer);
            if (avail.isEmpty()) {
                TextView none = Theme.body(this, getString(vkdListing.entries.isEmpty() ? R.string.VKD_NONE_LISTED
                                                                                         : R.string.VKD_ALL_INSTALLED));
                none.setPadding(Theme.ROW_INSET, Theme.S1, 0, Theme.S2);
                list.addView(none, lp(MATCH, WRAP));
            }
            yAction = () -> vkdRefresh(true);
        }
        if (focus != null)
            focusMemory.put(Screen.DRIVERS, focusables.indexOf(focus));
        xAction = this::vkdDeleteFocused;
        addHint("A", R.string.HINT_CHOOSE, this::clickFocused);
        addHint("X", R.string.VKD_HINT_DELETE, this::vkdDeleteFocused);
        addHint("B", R.string.HINT_BACK, this::back);
        if (gameRunning())
            setNote(getString(R.string.VKD_GAME_RUNNING), NOTE_INFO);
    }

    /** An installed driver: its name, version as the value, Active or a warning as the badge; A chooses it. */
    private MenuRow vkdInstalledRow(LinearLayout list, VkDrivers drivers, VkDrivers.Driver d, boolean isActive,
                                    String footer) {
        MenuRow r = listRow(list, d.name, null, () -> vkdSelect(d));
        String version = d.driverVersion != null && !d.driverVersion.isEmpty() ? d.driverVersion : d.packageVersion;
        r.setValue(version == null ? "" : version);
        VkDriverStore.LastStart last = VkDriverStore.lastStart(drivers, d.id);
        int warn = VkDriverStore.lastStartWarning(last), badge = VkDriverStore.lastStartBadge(last, isActive);
        if (badge != 0)
            r.setBadge(getString(badge), Theme.WARN);
        else if (isActive)
            r.setBadge(getString(R.string.VKD_ACTIVE), Theme.ACCENT);
        List<String> facts = new ArrayList<>();
        if (version != null && !version.isEmpty())
            facts.add(kv(R.string.VKD_KV_VERSION, version));
        facts.add(kv(R.string.VKD_KV_SOURCE, VkDriverStore.originText(this, d)));
        facts.add(kv(R.string.VKD_KV_SIZE, size(d.sizeOnDisk())));
        String text = (d.description.isEmpty() ? "" : d.description + "\n\n") + TextUtils.join("\n", facts)
                      + "\n\n" + getString(R.string.VKD_INSTALLED_HELP);
        r.setDescription(text);
        onFocus(r, () -> describe(text, warn != 0 ? getString(warn) : footer, warn != 0 ? Theme.WARN : Theme.TEXT_DIM));
        vkdRows.put(r, d);
        return r;
    }

    /**
     * A downloadable driver: its file name without .zip / .adpkg (a release may hold several; VkDriverStore.rowNames),
     * its size as the value, « Pre-release » as the badge; A downloads it.
     */
    private void vkdAvailableRow(LinearLayout list, VkDriverIndex.Entry e, String name, String footer) {
        MenuRow r = listRow(list, name, null, () -> vkdDownload(e));
        r.setValue(e.size > 0 ? size(e.size) : "");
        if (e.prerelease)
            r.setBadge(getString(R.string.VKD_PRERELEASE), Theme.WARN);
        List<String> facts = new ArrayList<>();
        if (!e.release.trim().isEmpty())
            facts.add(e.release.trim());
        facts.add(kv(R.string.VKD_KV_SOURCE, e.source));
        facts.add(kv(R.string.VKD_KV_TAG, e.tag));
        facts.add(kv(R.string.VKD_KV_FILE, e.assetName));
        if (e.publishedAt > 0)
            facts.add(kv(R.string.VKD_KV_DATE, VkDriverStore.date(e.publishedAt)));
        if (e.size > 0)
            facts.add(kv(R.string.VKD_KV_SIZE, size(e.size)));
        if (e.prerelease)
            facts.add(getString(R.string.VKD_PRERELEASE_DETAIL));
        String text = getString(R.string.VKD_DOWNLOAD_HELP) + "\n\n" + TextUtils.join("\n", facts);
        r.setDescription(text);
        onFocus(r, () -> describe(text, footer, Theme.TEXT_DIM));
    }

    /** A on « System driver » (d null) or an installed driver: it becomes the active one. */
    private void vkdSelect(VkDrivers.Driver d) {
        VkDrivers drivers = VkDrivers.of(this);
        boolean ok = d == null ? drivers.clearActive() : drivers.setActive(d.id);
        Log.i(TAG, "launcher: vulkan driver " + (d == null ? "system" : d.name + " (" + d.id + ")")
                   + (ok ? "" : ": not saved"));
        refreshKeepingFocus();
        if (!ok)
            setNote(getString(R.string.VKD_SELECT_FAILED), NOTE_ERROR);
        else
            setNote(getString(gameRunning() ? R.string.VKD_SELECTED_RUNNING : R.string.VKD_SELECTED,
                              vkdActiveLabel(d)), NOTE_OK);
    }

    /** X on an installed driver: twice within CONFIRM_MS deletes it (the active one: back to the system driver). */
    private void vkdDeleteFocused() {
        View f = focused();
        VkDrivers.Driver d = f == null ? null : vkdRows.get(f);
        if (d == null) {
            setNote(getString(R.string.VKD_DELETE_WHICH), NOTE_INFO);
            return;
        }
        long now = SystemClock.uptimeMillis();
        if (!d.id.equals(vkdArmedId) || now - deleteArmedAt > CONFIRM_MS) {
            vkdArmedId = d.id;
            deleteArmedAt = now;
            ((MenuRow)f).setBadge(getString(R.string.VKD_DELETE_BADGE), Theme.WARN);
            setNote(getString(R.string.VKD_DELETE_CONFIRM, d.name), NOTE_WARN);
            return;
        }
        vkdArmedId = null;
        deleteArmedAt = 0;
        boolean ok = VkDrivers.of(this).delete(d.id);
        Log.i(TAG, "launcher: vulkan driver " + d.id + " deleted" + (ok ? "" : " (failed)"));
        refreshKeepingFocus();
        setNote(getString(ok ? R.string.VKD_DELETED : R.string.VKD_DELETE_FAILED, d.name), ok ? NOTE_OK : NOTE_ERROR);
    }

    /** Refresh (force) or the first look at a stale list: on a thread; the screen is rebuilt when it answers. */
    private void vkdRefresh(boolean force) {
        if (vkdInherits() || (vkdRefresher != null && vkdRefresher.isAlive()))
            return;
        VkDriverIndex index = vkdIndex();
        vkdRefresher = new Thread(() -> {
            VkDriverIndex.Listing l = index.refresh(force);
            for (VkDriverIndex.SourceStatus s : l.status)
                Log.i(TAG, "launcher: vulkan drivers list " + s.source.id() + ": " + s.state
                           + (s.detail.isEmpty() ? "" : " (" + s.detail + ")"));
            runOnUiThread(() -> {
                vkdListing = l;
                if (screen == Screen.DRIVERS && !isFinishing()) {
                    refreshKeepingFocus();
                    if (force)
                        setNote(VkDriverStore.listState(this, l, false), NOTE_INFO);
                }
            });
        }, "xba-vkd-list");
        vkdRefresher.start();
        if (screen == Screen.DRIVERS && !building)
            refreshKeepingFocus();   // « Updating… »
    }

    /** Tests: the list's thread. */
    Thread vkdRefresherForTest() {
        return vkdRefresher;
    }

    /** A on a downloadable driver: the download behind the progress screen (B cancels), then its row is focused. */
    private void vkdDownload(VkDriverIndex.Entry e) {
        VkDriverIndex index = vkdIndex();
        VkDriverStore.Adapter store = new VkDriverStore.Adapter(VkDrivers.of(this));
        Log.i(TAG, "launcher: vulkan driver download " + e.key());
        runWorker(R.string.VKD_DOWNLOADING, () -> {
            try {
                String sha = index.download(e, store, (done, total) -> progress(done, total, e.assetName),
                                            () -> cancelRequested);
                Log.i(TAG, "launcher: vulkan driver " + e.key() + " installed, sha256 " + sha);
                vkdInstalled(store.installed);
            } catch (IOException ex) {
                vkdFailed(ex);
            }
        });
    }

    /** Worker: an install went through: its row gets the focus, the note offers to make it active. */
    private void vkdInstalled(VkDrivers.Driver d) {
        if (d == null)
            return;
        vkdFocusId = d.id;
        workerNoteKind = NOTE_OK;
        workerNote = getString(R.string.VKD_INSTALLED, VkDriverStore.label(d));
    }

    /** Worker: a download or an import failed, in plain words (cancelled: said so). */
    private void vkdFailed(IOException e) {
        boolean cancel = e instanceof VkDriverIndex.DownloadException
                         && ((VkDriverIndex.DownloadException)e).reason
                            == VkDriverIndex.DownloadException.Reason.CANCELLED;
        Log.w(TAG, "launcher: vulkan driver: " + e);
        workerNoteKind = cancel ? NOTE_INFO : NOTE_ERROR;
        workerNote = VkDriverStore.errorText(this, e);
    }

    private void openVkDriverPicker() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*");
        try {
            startActivityForResult(i, REQ_VK_DRIVER);
        } catch (ActivityNotFoundException e) {
            setNote(getString(R.string.PICKER_MISSING), NOTE_ERROR);
        }
    }

    /** « Import a zip »: the picked file is checked and installed on the worker (VkDrivers validates it). */
    private void onVkDriverPicked(Uri uri) {
        String name = displayName(uri);
        Log.i(TAG, "launcher: vulkan driver zip picked (" + name + ")");
        runWorker(R.string.VKD_IMPORTING, () -> {
            try (java.io.InputStream in = getContentResolver().openInputStream(uri)) {
                if (in == null)
                    throw new IOException("cannot open " + name);
                vkdInstalled(VkDrivers.of(this).install(in, VkDrivers.Origin.manual()));
            } catch (IOException ex) {
                vkdFailed(ex);
            }
        });
    }

    /** A game's own app at its start: XRecomp's driver choice (VkDriverShare.sync), on a thread. */
    private void syncVkDriverFromParent() {
        if (TitleApp.ownParent(this) == null || !vkdInherits())
            return;
        new Thread(() -> {
            VkDriverShare.Outcome o = VkDriverShare.sync(this);
            if (o != VkDriverShare.Outcome.UNCHANGED && o != VkDriverShare.Outcome.NO_ANSWER)
                runOnUiThread(() -> {
                    if (screen == Screen.SETTINGS && !isFinishing())
                        refreshKeepingFocus();
                });
        }, "xba-vkd-sync").start();
    }

    // ---- GAME_DATA ---------------------------------------------------------------------------------------------

    /**
     * Manage (Home's third pill): the library (or Change game), deleting the imported copy, and the game's other
     * stored things, each with its size as the value. The technical facts (title ID, pack, folder) are the first
     * row's description.
     */
    private void buildGameData() {
        screenTitle.setText(R.string.HOME_MANAGE);
        String dir = gameDir();
        sizeLater(dir);
        LinearLayout list = listTemplate();
        boolean lib = libraryOn();
        String facts = manageFacts(dir);
        MenuRow change = lib ? listRow(list, getString(R.string.LIB_TITLE), null, this::openLibrary)
                             : listRow(list, getString(dir == null ? R.string.CHOOSE_GAME : R.string.CHANGE_GAME),
                                       getString(R.string.CHANGE_GAME_DETAIL), () -> go(Screen.SOURCE));
        onFocus(change, () -> describe(change.description, facts, Theme.TEXT_DIM));
        defaultFocus = change;
        // A home-screen shortcut for this game (TitleShortcut): only for a real title ID (the library's key; not a
        // folder-only record) and a home screen that takes pin requests.
        String pinTid = shortcutTid();
        if (pinTid != null && TitleShortcut.supported(this))
            listRow(list, getString(R.string.SHORTCUT_ADD), getString(R.string.SHORTCUT_ADD_DETAIL),
                    () -> onAddShortcut(pinTid));
        // A game's own app (TitleApp, TitleApk): the same title ID rule; not in a game's own app itself, nor from a
        // split install (nothing to copy). Only for a prepared game: the app takes this pack over at its first start
        // (PackHandover) and goes straight into the game; before that the row says to prepare first.
        File template = pinTid != null && TitleApp.ownTitleId(this) == null ? TitleApp.template(this) : null;
        if (template != null) {
            if (prepared(pinTid))
                listRow(list, getString(R.string.TITLE_APP_ADD),
                        getString(R.string.TITLE_APP_ADD_DETAIL, size(template.length())), () -> onCreateApp(pinTid));
            else
                listRow(list, getString(R.string.TITLE_APP_ADD), getString(R.string.TITLE_APP_ADD_UNPREPARED),
                        this::notePrepareFirst);
        }

        if (dir != null && ownedCopy(dir)) {
            MenuRow del = listRow(list, getString(R.string.DELETE_COPY), getString(R.string.DELETE_COPY_DETAIL, dir),
                                  null);
            del.setValue(sizeText(dir));
            del.setOnClickListener(v -> onDelete(del, dir));
        }
        // The game's other stored things (pack, Prepare files, caches, saves), each deleted on its own (Storage).
        String tid = titleId();
        Library.Record r = rec();
        String key = tid != null ? tid : r != null ? r.titleId : null;
        if (key != null) {
            List<Storage.Item> items = new ArrayList<>();
            for (Storage.Item it : Storage.items(storageDirs(), key, r)) {
                if (it.kind != Storage.Kind.DATA)
                    items.add(it);
            }
            for (Storage.Item it : items)
                storageRow(list, it);
            storageMeasureLater(items);
        }
        describe(change.description, facts, Theme.TEXT_DIM);
        addHint("A", R.string.HINT_SELECT, this::clickFocused);
        addHint("B", R.string.HINT_BACK, this::back);
    }

    /**
     * The title ID a shortcut of the current game carries: its library record's key when that is a title ID (what
     * ACTION_PLAY_TITLE looks up), else the XBE's; null for a game without one.
     */
    private String shortcutTid() {
        Library.Record r = rec();
        String tid = r != null && !Library.isLocalKey(r.titleId) ? r.titleId : titleId();
        return tid == null || Library.isLocalKey(tid) ? null : tid;
    }

    /** Manage › Add to home screen: the home screen shows its own dialog; the note says the request went out. */
    private void onAddShortcut(String tid) {
        refreshIdentity();   // the title image of this game (or none: the monogram)
        boolean ok = TitleShortcut.request(this, tid, gameTitle(gameDir()), art);
        Log.i(TAG, "launcher: shortcut " + TitleShortcut.id(tid) + (ok ? " requested" : " refused"));
        setNote(getString(ok ? R.string.SHORTCUT_REQUESTED : R.string.SHORTCUT_REFUSED), ok ? NOTE_OK : NOTE_ERROR);
    }

    /** tid has a valid pack built from the selected game's default.xbe: what « Create an app » needs. */
    private boolean prepared(String tid) {
        return Pack.check(getFilesDir(), tid) == Pack.Check.VALID && packMatchesSelected(tid);
    }

    private void notePrepareFirst() {
        Log.i(TAG, "launcher: game app: not prepared, nothing built");
        setNote(getString(R.string.TITLE_APP_PREPARE_FIRST, gameTitle(gameDir())), NOTE_WARN);
    }

    /**
     * Manage › Create an app (docs/android/app-shell.md « A game's own app »): this APK copied as the game's own
     * app (TitleApk), behind the progress screen, then handed to Android's installer, whose confirmation
     * TitleAppReceiver shows. Without « Install unknown apps » for this app, its settings screen first.
     */
    private void onCreateApp(String tid) {
        if (!prepared(tid)) {   // the pack went away since Manage was built
            notePrepareFirst();
            return;
        }
        if (!TitleApp.canInstall(this)) {
            Log.i(TAG, "launcher: game app: Install unknown apps is off, opening its settings");
            setNote(getString(R.string.TITLE_APP_ALLOW), NOTE_WARN);
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                                         Uri.parse("package:" + getPackageName())));
            } catch (ActivityNotFoundException e) {
                Log.w(TAG, "launcher: no Install unknown apps screen", e);
            }
            return;
        }
        File template = TitleApp.template(this);
        if (template == null)
            return;
        long need = TitleApp.spaceNeeded(template), free = getFilesDir().getUsableSpace();
        if (free < need) {
            setNote(getString(R.string.NOT_ENOUGH_SPACE, size(need), size(free)), NOTE_ERROR);
            return;
        }
        refreshIdentity();   // the title image of this game (or none: the monogram)
        String dir = gameDir();
        String title = gameTitle(dir);
        String label = TitleApk.label(title == null || title.trim().isEmpty() ? tid : title);
        Library.Record r = rec();
        String knownSha = r != null && tid.equals(r.titleId) ? r.xbeSha256 : "";
        Bitmap icon = art;
        Log.i(TAG, "launcher: game app " + TitleApk.packageName(tid) + " « " + label + " »: building from "
                   + template + " (" + template.length() + " bytes)");
        runWorker(R.string.TITLE_APP_BUILDING, () -> {
            try {
                TitleApk.Spec spec = new TitleApk.Spec();
                spec.titleId = tid;
                spec.label = label;
                spec.parent = getPackageName();
                spec.xbeSha256 = knownSha;
                if (spec.xbeSha256 == null || spec.xbeSha256.isEmpty()) {
                    File xbe = dir == null ? null : GameStore.findXbe(new File(dir));
                    spec.xbeSha256 = xbe == null ? "" : GameStore.sha256(xbe);
                }
                if (spec.xbeSha256.isEmpty())
                    Log.w(TAG, "launcher: game app " + spec.packageName() + ": no default.xbe hash, not locked");
                spec.iconPng = TitleApp.iconPng(icon, tid, label);
                spec.iconEntry = TitleApp.iconEntry(this);
                spec.iconRes = R.mipmap.xba_title_icon;
                TitleApp.Key key = TitleApp.key();
                File apk = TitleApp.build(this, spec, key, new TitleApp.Progress() {
                    @Override
                    public void update(long done, long total) {
                        progress(done, total, label);
                    }

                    @Override
                    public void signing() {
                        progressIndeterminate(R.string.TITLE_APP_SIGNING);
                    }
                }, () -> cancelRequested);
                Log.i(TAG, "launcher: game app " + spec.packageName() + ": " + apk.length() + " bytes, signed");
                progressIndeterminate(R.string.TITLE_APP_HANDING);
                TitleApp.install(this, apk, spec.packageName(), label, (done, total) -> progress(done, total, label),
                                 () -> cancelRequested);
                //noinspection ResultOfMethodCallIgnored
                apk.delete();   // the session has its own copy
                workerNoteKind = NOTE_INFO;
                workerNote = getString(R.string.TITLE_APP_CONFIRM, label);
            } catch (java.io.InterruptedIOException e) {
                TitleApp.clean(this);
                cancelled();
            } catch (IOException | RuntimeException e) {
                Log.w(TAG, "launcher: game app " + TitleApk.packageName(tid) + " failed", e);
                TitleApp.clean(this);
                fail(getString(R.string.TITLE_APP_BUILD_FAILED, String.valueOf(e.getMessage())));
            }
        });
    }

    /** The installer's last outcome for a game's own app (TitleAppReceiver), as a note; nothing when none is new. */
    private void showTitleAppOutcome() {
        String[] o = TitleApp.takeOutcome(this);
        if (o == null)
            return;
        int status = Integer.parseInt(o[0]);
        String msg = o[1], label = o[2].isEmpty() ? o[3] : o[2];
        switch (status) {
        case android.content.pm.PackageInstaller.STATUS_SUCCESS:
            setNote(getString(R.string.TITLE_APP_INSTALLED, label), NOTE_OK);
            break;
        case android.content.pm.PackageInstaller.STATUS_FAILURE_ABORTED:
            setNote(getString(R.string.TITLE_APP_CANCELLED), NOTE_INFO);
            break;
        case android.content.pm.PackageInstaller.STATUS_FAILURE_CONFLICT:
            setNote(getString(R.string.TITLE_APP_CONFLICT, label), NOTE_ERROR);
            break;
        default:
            setNote(getString(R.string.TITLE_APP_FAILED, msg.isEmpty() ? String.valueOf(status) : msg), NOTE_ERROR);
        }
    }

    /** The game's technical facts, one per line: title ID, when it was prepared and its pack's size, its files. */
    private String manageFacts(String dir) {
        List<String> l = new ArrayList<>();
        String tid = titleId();
        if (tid != null)
            l.add(kv(R.string.KV_TITLE_ID, tid));
        if (tid != null && Pack.check(getFilesDir(), tid) == Pack.Check.VALID) {
            String[] pack = packFacts(tid);
            l.add(kv(R.string.KV_PREPARED, pack[0]));
            l.add(kv(R.string.KV_PACK_SIZE, pack[1]));
            l.addAll(PrepareFacts.lines(this, getFilesDir(), tid));   // XBA-128: what its Prepare cost
        }
        if (dir == null) {
            l.add(kv(R.string.KV_STATUS, getString(stateLabel(state()))));
        } else {
            l.add(kv(R.string.KV_FOLDER, dir));
            l.add(kv(R.string.KV_SOURCE, sourceLabel(dir)));
            l.add(kv(R.string.KV_SIZE, sizeText(dir)));
        }
        l.add(kv(R.string.KV_FREE, size(freeSpace())));
        return TextUtils.join("\n", l);
    }

    /** Two presses within CONFIRM_MS: the first arms, the second deletes (on a background thread). */
    private void onDelete(MenuRow row, String dir) {
        if (deleting)
            return;
        long now = SystemClock.uptimeMillis();
        if (deleteArmedAt == 0 || now - deleteArmedAt > CONFIRM_MS) {
            deleteArmedAt = now;
            row.setBadge(getString(R.string.DELETE_CONFIRM_BADGE), Theme.WARN);
            setNote(getString(R.string.DELETE_CONFIRM), NOTE_WARN);
            return;
        }
        deleteArmedAt = 0;
        deleting = true;
        row.setBadge(getString(R.string.DELETING), Theme.TEXT_DIM);
        setNote(getString(R.string.DELETING), NOTE_INFO);
        if (dir.equals(launchedDir()))
            endRunningGame();   // only the game that runs from these files
        Log.i(TAG, "launcher: deleting the imported copy " + dir);
        boolean inLibrary = indexGames.containsKey(titleId());
        new Thread(() -> {
            String tid = Pack.xbeTitleId(GameStore.findXbe(new File(dir)));
            GameStore.deleteTree(new File(dir));
            /* Its title image, hero and colours (docs/android/app-shell.md « Game theme ») go only when the title
             * also left the library folder: the library still shows it there (docs « Game library »). */
            if (!inLibrary)
                GameTheme.clear(this, tid);
            if (dir.equals(GameStore.chosenDir(this)))
                GameStore.forget(this);
            runOnUiThread(() -> {
                deleting = false;
                sizeDir = null;
                hashDir = null;
                titleIdDir = null;
                artTid = null;
                for (Library.Record r : games.games.values()) {
                    if (dir.equals(r.dataDir)) {
                        r.dataDir = "";
                        r.dataKind = "";
                    }
                }
                writeGames();
                if (tid != null)
                    libArt.remove(tid);
                history.clear();
                focusMemory.remove(Screen.HOME);
                show(libraryOn() ? (rec() != null ? Screen.HOME : Screen.LIBRARY)
                                 : state() == State.NONE ? Screen.WELCOME : Screen.HOME);
                setNote(getString(R.string.DELETED), NOTE_OK);
            });
        }, "xba-delete").start();
    }

    // ---- PREPARE (the on-device Prepare, docs/android/prepare.md) -----------------------------------------------

    /** The title ID of the current game's default.xbe (its certificate), read once per folder; null if none. */
    private String titleId() {
        String dir = gameDir();
        if (dir == null) {
            // A library game whose files are not here: its record's key (its Prepare, art and theme go by it).
            Library.Record r = rec();
            return r != null && r.dataDir.isEmpty() && !Library.isLocalKey(r.titleId) ? r.titleId : null;
        }
        if (!dir.equals(titleIdDir)) {
            titleIdDir = dir;
            titleIdValue = Pack.xbeTitleId(GameStore.findXbe(new File(dir)));
        }
        return titleIdValue;
    }

    private PrepareProtocol.Status readPrepStatus() {
        String tid = prepTid();
        return tid == null ? null : PrepareProtocol.readStatus(PrepareService.statusFile(this, tid));
    }

    /** The ":prepare" process is there (a running status.json from a dead process means interrupted). */
    private boolean prepareProcessAlive() {
        ActivityManager am = getSystemService(ActivityManager.class);
        List<ActivityManager.RunningAppProcessInfo> procs = am == null ? null : am.getRunningAppProcesses();
        if (procs == null)
            return false;
        String name = getPackageName() + ":prepare";
        for (ActivityManager.RunningAppProcessInfo p : procs) {
            if (name.equals(p.processName))
                return true;
        }
        return false;
    }

    private boolean statusAlive(PrepareProtocol.Status s) {
        return s != null && s.running()
               && (System.currentTimeMillis() - s.updatedAt < PREPARE_FRESH_MS || prepareProcessAlive());
    }

    private Prep prep() {
        if (state() != State.READY || !Toolchain.available(this))
            return Prep.OFF;
        String tid = titleId();
        if (tid == null)
            return Prep.OFF;
        PrepareProtocol.Status s = PrepareProtocol.readStatus(PrepareService.statusFile(this, tid));
        if (statusAlive(s))   // a background one (XBA-129) leaves a valid pack playable
            return s.background && Pack.check(getFilesDir(), tid) == Pack.Check.VALID && packMatchesSelected(tid)
                   ? Prep.PLAYABLE : Prep.RUNNING;
        if (s != null && s.queued())   // waiting behind another title's Prepare; no :prepare process = cut off
            return prepareProcessAlive() ? Prep.QUEUED : Prep.INTERRUPTED;
        if (Pack.check(getFilesDir(), tid) == Pack.Check.VALID && packMatchesSelected(tid))
            return Prep.PLAYABLE;
        if (s == null)
            return Prep.NEEDED;
        switch (s.state) {
        case PrepareProtocol.FAILED:
            return Prep.FAILED;
        case PrepareProtocol.CANCELLED:
            return Prep.CANCELLED;
        case PrepareProtocol.RUNNING:
            return Prep.INTERRUPTED;
        default:
            return Prep.NEEDED;   // done, but the pack is gone or stale (another shell ABI): prepare again
        }
    }

    /** The pack of tid was built from the selected game's default.xbe (as far as both say: LibraryScreen). */
    private boolean packMatchesSelected(String tid) {
        return LibraryScreen.packMatches(getFilesDir(), tid, selectedXbeSha());
    }

    // ---- The pack handover (a game's own app, XBA-176, PackHandover) --------------------------------------------

    private static final String HANDOVER_PREFS = "handover", HANDOVER_DECLINED = "declined_parent_update";
    /** A note about the handover for Home (shown when Home has nothing more urgent to say), or null. */
    private String handoverNoteText;
    private int handoverNoteKind;
    /** The parent's lastUpdateTime when it was asked. */
    private long handoverParentUpdated = -1;

    /** This game's own app's title ID (8 upper-case hex digits); null in XRecomp itself. */
    private String ownTid() {
        return Pack.normTitleId(TitleApp.ownTitleId(this));
    }

    /** The parent's pack is being copied for tid. */
    private boolean copying(String tid) {
        return handover != null && tid != null && Pack.sameTitleId(tid, handover.tid);
    }

    /**
     * In a game's own app with no pack, or one lifted by another toolkit than its own: asks the parent (XRecomp,
     * meta-data PARENT) for its pack, once per process, through its PackHandoverActivity for a result. Not while the
     * app's own Prepare runs. A stale pack of its own that the parent did not replace: asked again only after the
     * parent was updated.
     */
    private void askParentForPack() {
        long parentUpdated = parentToAsk();
        String tid = ownTid(), parent = TitleApp.ownParent(this);
        boolean ownValid = tid != null && Pack.check(getFilesDir(), tid) == Pack.Check.VALID;
        if (parentUpdated == PARENT_MISSING) {
            handoverAsked = true;
            Log.i(TAG, "launcher: pack handover: " + parent + " is not installed");
            if (!ownValid)
                handoverNote(getString(R.string.HANDOVER_NO_PARENT), NOTE_INFO);
            return;
        }
        if (parentUpdated < 0)
            return;
        boolean filesWanted = direct && state() != State.READY;
        handoverAsked = true;
        handoverParentUpdated = parentUpdated;
        Intent i = new Intent(PackHandover.ACTION).setClassName(parent, PackHandover.ACTIVITY)
                .putExtra(PackHandover.EXTRA_TITLE_ID, tid);
        try {
            handoverWaiting = true;
            startActivityForResult(i, REQ_PACK_HANDOVER);
            overridePendingTransition(0, 0);
            Log.i(TAG, "launcher: pack handover: asking " + parent + " for " + tid
                       + (ownValid ? filesWanted ? " (where its files are)" : " (this app's pack is from another toolkit)"
                                   : " (no pack here)"));
        } catch (ActivityNotFoundException | SecurityException e) {
            handoverWaiting = false;
            Log.w(TAG, "launcher: pack handover: cannot ask " + parent + ": " + e);
            if (!ownValid)
                handoverNote(getString(R.string.HANDOVER_REFUSED, e.getClass().getSimpleName()), NOTE_WARN);
        }
    }

    /** parentToAsk(): the parent is not installed. */
    private static final long PARENT_MISSING = -2;

    /**
     * Whether the parent is to be asked now, without asking it: its lastUpdateTime when it is, PARENT_MISSING when it is
     * not installed (askParentForPack then says so once), -1 otherwise. Asked once per process, for a pack (none here,
     * or one from another toolkit than this app's, not while this app's own Prepare runs nor the game runs; a stale
     * pack of its own that the parent did not replace: only after the parent was updated) or, on the way into the
     * game (direct), for where the game's files are when this app cannot read them yet.
     */
    private long parentToAsk() {
        String tid = ownTid(), parent = TitleApp.ownParent(this);
        if (tid == null || parent == null || handoverAsked || handover != null)
            return -1;
        boolean filesWanted = direct && state() != State.READY;
        if (!filesWanted && !PackHandover.wanted(getFilesDir(), tid, Toolchain.toolkitDigest(this)))
            return -1;
        PrepareProtocol.Status s = PrepareProtocol.readStatus(PrepareService.statusFile(this, tid));
        if (!filesWanted && (statusAlive(s) || (s != null && s.queued() && prepareProcessAlive())))
            return -1;
        if (gameRunning())   // its pack is loaded: replaced at a later start
            return -1;
        long parentUpdated;
        try {
            parentUpdated = getPackageManager().getPackageInfo(parent, 0).lastUpdateTime;
        } catch (PackageManager.NameNotFoundException e) {
            return PARENT_MISSING;
        }
        boolean ownValid = Pack.check(getFilesDir(), tid) == Pack.Check.VALID;
        if (!filesWanted && ownValid
            && parentUpdated == getSharedPreferences(HANDOVER_PREFS, MODE_PRIVATE).getLong(HANDOVER_DECLINED, -2))
            return -1;
        return Math.max(0, parentUpdated);
    }

    /** The parent's answer: an offer to copy, or a refusal (a note when this app has nothing to play). */
    private void onHandoverAnswer(int resultCode, Intent data) {
        handoverWaiting = false;
        String tid = ownTid(), parent = TitleApp.ownParent(this);
        if (tid == null || parent == null || handover != null)
            return;
        // Where the parent reads the game from (told with the pack or the NO_PACK refusal): kept for the files step.
        PackHandover.GameSource src = PackHandover.sourceOf(data);
        if (src != null) {
            Log.i(TAG, "launcher: pack handover: " + parent + " reads the game from " + src);
            saveSource(src);
        }
        PackHandover.Offer offer = resultCode == RESULT_OK ? PackHandover.offer(data, parent, tid) : null;
        if (offer != null && !PackHandover.wanted(getFilesDir(), tid, Toolchain.toolkitDigest(this))) {
            Log.i(TAG, "launcher: pack handover: this app's pack is current, not copying");
            if (direct)
                directStep();
            return;
        }
        if (offer == null) {
            String r = data == null ? null : data.getStringExtra(PackHandover.EXTRA_REFUSAL);
            Log.w(TAG, "launcher: pack handover: " + parent + " refused: "
                       + (r != null ? r : resultCode == RESULT_OK ? "malformed answer" : "result " + resultCode));
            handoverDeclined();
            if (Pack.check(getFilesDir(), tid) != Pack.Check.VALID)
                handoverNote(PackHandover.Refusal.NO_PACK.name().equals(r) ? getString(R.string.HANDOVER_NO_PACK)
                             : getString(R.string.HANDOVER_REFUSED, r != null ? r : "?"),
                             PackHandover.Refusal.NO_PACK.name().equals(r) ? NOTE_INFO : NOTE_WARN);
            if (direct)
                directStep();
            return;
        }
        Log.i(TAG, "launcher: pack handover: " + parent + " offers " + offer.uris.keySet() + ", " + offer.total()
                   + " bytes");
        Handover h = new Handover(offer.titleId, offer.total());
        handover = h;
        handoverNoteText = null;
        if (direct)
            directStep();
        else if (screen == Screen.HOME)
            show(Screen.HOME, 0);
        else   // the first start's game choice: the copy goes on behind it
            setNote(getString(R.string.HANDOVER_COPYING_NOTE), NOTE_INFO);
        ContentResolver cr = getContentResolver();
        String locked = GameStore.expectedSha256(this), toolkit = Toolchain.toolkitDigest(this);
        File files = getFilesDir();
        handoverWorker = new Thread(() -> {
            PackHandover.Result r = PackHandover.take(name -> {
                java.io.InputStream in = cr.openInputStream(offer.uris.get(name));
                if (in == null)
                    throw new java.io.FileNotFoundException(name);
                return in;
            }, offer, files, locked, toolkit, (done, total) -> {
                int before = h.pct();
                h.done = done;
                if (h.pct() != before)
                    runOnUiThread(() -> handoverProgress(h));
            }, this::isDestroyed);
            runOnUiThread(() -> handoverDone(h, r));
        }, "xba-pack-handover");
        handoverWorker.start();
    }

    /** Tests: the copy's thread. */
    Thread handoverWorkerForTest() {
        return handoverWorker;
    }

    private void handoverProgress(Handover h) {
        if (handover == h && screen == Screen.HOME && homeStatus != null)
            homeStatus.set(getString(R.string.HANDOVER_COPYING, h.pct()), Theme.ACCENT);
        if (handover == h && screen == Screen.DIRECT && directStatus != null && directState == Direct.COPYING)
            directStatus.set(getString(R.string.HANDOVER_COPYING, h.pct()), Theme.ACCENT);
    }

    private void handoverDone(Handover h, PackHandover.Result r) {
        if (handover != h)
            return;
        handover = null;
        handoverWorker = null;
        Log.i(TAG, "launcher: pack handover " + h.tid + ": " + r.outcome + (r.detail.isEmpty() ? "" : " (" + r.detail + ")"));
        boolean ownValid = Pack.check(getFilesDir(), h.tid) == Pack.Check.VALID;
        String note = null;
        int kind = NOTE_WARN;
        switch (r.outcome) {
        case COPIED:
            // The parent's Enhancements choice comes with its pack (else Home would offer to prepare again).
            String tl = r.meta == null ? "" : r.meta.optString("title_layer", "");
            if ("on".equals(tl) || "off".equals(tl))
                GameStore.setEnhancements(this, h.tid, "on".equals(tl));
            note = getString(r.olderToolkit ? R.string.HANDOVER_DONE_OLDER : R.string.HANDOVER_DONE);
            kind = NOTE_OK;
            break;
        case NOT_NEWER:
            handoverDeclined();
            break;
        case CANCELLED:   // the app was left during the copy: asked again at its next start
            handoverDeclined();
            handoverAsked = false;
            break;
        case UNFIT:
            note = getString(R.string.HANDOVER_UNFIT);
            break;
        case OTHER_XBE:
            note = getString(R.string.HANDOVER_OTHER_XBE);
            break;
        case NO_SPACE:
            long need = 0;
            try {
                need = Long.parseLong(r.detail);
            } catch (NumberFormatException ignored) {
            }
            note = getString(R.string.HANDOVER_NO_SPACE, size(need));
            break;
        default:
            note = getString(R.string.HANDOVER_FAILED, r.detail);
            kind = NOTE_ERROR;
            break;
        }
        if (r.outcome != PackHandover.Outcome.COPIED && ownValid) {
            handoverDeclined();
            note = null;   // its own pack still plays: nothing to say
        }
        if (note != null)
            handoverNote(note, kind);
        else if (screen == Screen.HOME)
            refreshKeepingFocus();
        if (direct && !isFinishing()) {
            if (r.outcome == PackHandover.Outcome.COPIED)
                handoverNoteText = null;   // straight into the game: nothing to read
            directStep();
        }
    }

    /** The parent did not replace this app's stale pack: not asked again until it is updated. */
    private void handoverDeclined() {
        if (handoverParentUpdated >= 0)
            getSharedPreferences(HANDOVER_PREFS, MODE_PRIVATE).edit()
                    .putLong(HANDOVER_DECLINED, handoverParentUpdated).apply();
    }

    /** Shows a handover note: on Home (rebuilt) when Home shows, else at once. */
    private void handoverNote(String text, int kind) {
        handoverNoteText = text;
        handoverNoteKind = kind;
        if (screen == Screen.HOME)
            show(Screen.HOME, 0);
        else
            setNote(text, kind);
    }

    // ---- A game's own app, straight into the game (docs/android/app-shell.md « Straight into the game ») ---------

    /**
     * What DIRECT shows on a game's own app's way into the game: XRecomp being asked, its pack being copied, the
     * Prepare, All files access asked (to read the files XRecomp uses), those files being checked, the game to choose,
     * or an error with the one action that fixes it.
     */
    enum Direct { WAIT, COPYING, PREPARING, ASK_ACCESS, CHECKING, CHOOSE, ERROR }

    /** Where XRecomp reads the game from (its last answer: PackHandover.GameSource), in HANDOVER_PREFS. */
    private static final String HANDOVER_GAME_KIND = "game_kind", HANDOVER_GAME_PATH = "game_path";

    /**
     * This launcher is a game's own app's way into the game: started from its icon (or a game's shortcut, or the game
     * coming back), not from the « Launcher » shortcut. Home, the library and « Open the game directly » do not apply.
     */
    private boolean direct;
    /** The parent was asked and has not answered yet. */
    private boolean handoverWaiting;
    private Direct directState;
    private String directLine = "", directDetail;
    private int directColor, directActionLabel;
    private Runnable directAction;
    private boolean directShareLog;
    private Widgets.Chip directStatus;
    private Widgets.PillButton directPrimary;
    private Bitmap appArtBitmap;
    /** The files XRecomp uses were tried in this activity and not accepted. */
    private boolean directSourceFailed;
    /** All files access was asked from DIRECT (once per activity); its answer goes on with the next step. */
    private boolean directAccessAsked, directAccessPending;
    /** DIRECT started the Prepare once by itself: a second start is the player's. */
    private boolean directPrepareStarted;

    Direct directStateForTest() {
        return screen == Screen.DIRECT ? directState : null;
    }

    String directLineForTest() {
        return directLine;
    }

    String directDetailForTest() {
        return directDetail;
    }

    Widgets.PillButton directPrimaryForTest() {
        return directPrimary;
    }

    /**
     * onCreate of a game's own app (direct): the game running is brought back, a ready game starts with nothing shown;
     * otherwise DIRECT, which asks XRecomp (pack, where the files are) and goes on step by step (directStep). The
     * game's own reports (no game data, a pack the shell refused, the game ended) show there with their one action.
     */
    private void startDirect(Bundle saved, String noData, String badPack, String exited, String prepImage) {
        String tid = ownTid();
        if (games.get(tid) != null)
            games.selected = tid;
        boolean report = noData != null || badPack != null || exited != null;
        if (saved == null && prepImage != null && pendingImage() != null) {
            // XBA-82: a game's own app prepares the image at once (it has no Home to offer it on), then goes into it.
            Log.i(TAG, "launcher: game app: the game launches " + pendingImage() + ", preparing it");
            setUpWindow();
            feedback = new UiFeedback(this);
            syncTheme(Screen.DIRECT);
            buildStage();
            onPrepareImage();
            return;
        }
        if (saved == null && !report) {
            if (gameRunning()) {
                Log.i(TAG, "launcher: game app: the game is running, bringing it back");
                resumeShell();
                return;
            }
            if (parentToAsk() < 0 && directPlayable()) {
                Log.i(TAG, "launcher: game app: ready, starting the game");
                launchShell(false);
                return;
            }
        }
        Log.i(TAG, "launcher: game app: shown (" + (noData != null ? "NO_GAME_DATA in " + noData
                                                    : badPack != null ? "pack refused: " + badPack
                                                    : exited != null ? "game exited: " + exited
                                                    : "getting the game ready") + ")");
        setUpWindow();
        feedback = new UiFeedback(this);
        syncTheme(Screen.DIRECT);
        buildStage();
        if (noData != null)
            directFail(getString(R.string.STATE_MISSING), getString(R.string.NO_GAME_DATA, noData),
                       R.string.CHOOSE_AGAIN, this::directChoose, false);
        else if (badPack != null)
            directFail(getString(R.string.DIRECT_PACK_BAD), getString(R.string.PACK_BAD, badPack),
                       R.string.PREPARE_AGAIN, this::directPrepare, false);
        else if (exited != null)
            directShow(Direct.ERROR, getString(R.string.DIRECT_ENDED), Theme.WARN, titleExitedNote(this, exited),
                       R.string.PLAY, this::directRetry, false);
        else {
            askParentForPack();
            directStep(true);
        }
    }

    /** The game can start now: its files, no copy running, a pack from this default.xbe (or no Prepare here). */
    private boolean directPlayable() {
        if (state() != State.READY || copying(titleId()))
            return false;
        xbeChanged();   // hashes default.xbe once: a pack from another one is not playable
        Prep p = prep();
        return p == Prep.OFF || p == Prep.PLAYABLE;
    }

    private void directStep() {
        directStep(false);
    }

    /**
     * Where the way into the game stands, on DIRECT; the game starts as soon as nothing more is needed. Not while the
     * player is on the game choice (force: coming back from it, or a game chosen there).
     */
    private void directStep(boolean force) {
        if (!direct || isFinishing() || (!force && screen != null && screen != Screen.DIRECT))
            return;
        if (handoverWaiting) {
            directShow(Direct.WAIT, getString(R.string.DIRECT_WAIT), Theme.ACCENT, null, 0, null, false);
            return;
        }
        if (workerBusy) {
            directShow(Direct.CHECKING, getString(importLabel == 0 ? R.string.CHECKING : importLabel), Theme.ACCENT,
                       null, 0, null, false);
            return;
        }
        State st = state();
        if (st != State.READY) {
            directFiles(st);
            return;
        }
        String tid = titleId();
        if (copying(tid)) {
            directShow(Direct.COPYING, getString(R.string.HANDOVER_COPYING, handover.pct()), Theme.ACCENT, null, 0,
                       null, false);
            return;
        }
        if (directPlayable()) {
            Log.i(TAG, "launcher: game app: ready, starting the game");
            launchShell(false);
            return;
        }
        Prep p = prep();
        PrepareProtocol.Status ps = readPrepStatus();
        switch (p) {
        case RUNNING:
            directShow(Direct.PREPARING, ps == null ? getString(R.string.PREPARE_STARTING) : progressLine(ps), Theme.OK,
                       directPrepareDetail(), 0, null, false);
            return;
        case QUEUED:
            directShow(Direct.PREPARING, queuedLabel(tid), Theme.ACCENT, directPrepareDetail(), 0, null, false);
            return;
        case FAILED:
            directShow(Direct.ERROR, getString(R.string.CHIP_PREPARE_FAILED)
                                     + (ps == null || ps.code.isEmpty() ? "" : " · " + ps.code),
                       Theme.ERROR, ps == null || ps.msg.isEmpty() ? null : ps.msg, R.string.PREPARE_AGAIN,
                       this::directPrepare, true);
            return;
        default:   // NEEDED, INTERRUPTED: started once by itself; CANCELLED (the player stopped it): their choice
            if (!directPrepareStarted && p != Prep.CANCELLED) {
                directPrepare();
                return;
            }
            directShow(Direct.ERROR, getString(R.string.HOME_NEEDS_PREPARE, getString(R.string.CHIP_NEEDS_PREPARE),
                                               estimate()),
                       Theme.WARN, handoverNoteText, R.string.PREPARE, this::directPrepare, false);
        }
    }

    /** Under the Prepare's line: why it prepares (the handover's note), then that the app may be left. */
    private String directPrepareDetail() {
        String leave = getString(R.string.DIRECT_PREPARE_LEAVE);
        return handoverNoteText == null ? leave : handoverNoteText + " " + leave;
    }

    /**
     * The game's files are not usable here yet: the files XRecomp uses (All files access asked once for them), else
     * the game choice with a line saying why; an unreadable folder of its own asks for the access again.
     */
    private void directFiles(State st) {
        if (st == State.NO_ACCESS) {   // its folder, All files access turned off since
            directAskAccess(gameDir());
            return;
        }
        if (st == State.WRONG) {
            directFail(getString(R.string.STATE_WRONG), getString(R.string.WRONG_GAME_HOME), R.string.CHOOSE_AGAIN,
                       this::directChoose, false);
            return;
        }
        PackHandover.GameSource src = savedSource();
        boolean usable = src != null && src.kind != PackHandover.GameKind.NOT_SHARED && !directSourceFailed;
        boolean refused = false;
        if (usable && !Environment.isExternalStorageManager()) {
            if (!directAccessAsked) {
                directAskAccess(src.path);
                return;
            }
            usable = false;   // refused: the game choice (a copy, without the permission)
            refused = true;
        }
        if (usable) {
            directUseSource(src);
            return;
        }
        if (st == State.MISSING) {
            directFail(getString(R.string.STATE_MISSING), getString(R.string.GAME_FOLDER_GONE, gameDir()),
                       R.string.CHOOSE_AGAIN, this::directChoose, false);
            return;
        }
        int why = refused ? R.string.DIRECT_ACCESS_REFUSED
                  : src != null && src.kind == PackHandover.GameKind.NOT_SHARED ? R.string.DIRECT_NOT_SHARED
                  : directSourceFailed ? R.string.DIRECT_SOURCE_FAILED : R.string.DIRECT_CHOOSE;
        directShow(Direct.CHOOSE, getString(R.string.DIRECT_CHOOSE_LINE), Theme.WARN, getString(why),
                   R.string.CHOOSE_GAME, this::directChoose, false);
    }

    /** One line why, one button: the system page for All files access (its answer: directStep). */
    private void directAskAccess(String path) {
        directShow(Direct.ASK_ACCESS, getString(R.string.DIRECT_ACCESS_LINE), Theme.WARN,
                   getString(R.string.DIRECT_ACCESS, path), R.string.ALL_FILES_GRANT, () -> {
                       directAccessAsked = true;
                       directAccessPending = true;
                       Log.i(TAG, "launcher: game app: asking for All files access, to read " + path);
                       askAllFilesAccess();
                   }, false);
    }

    /**
     * The files XRecomp uses, read in place: a folder checked as a picked one would be (default.xbe against the
     * disc this app is locked to), an image as a picked one by path (its companion folder). Behind DIRECT's line.
     */
    private void directUseSource(PackHandover.GameSource src) {
        File f = new File(src.path);
        boolean folder = src.kind == PackHandover.GameKind.FOLDER;
        Log.i(TAG, "launcher: game app: reading the game where XRecomp does: " + src);
        if (folder ? GameStore.findXbe(f) == null : !f.isFile()) {
            directSourceFailed = true;
            directFail(getString(R.string.STATE_MISSING), getString(R.string.DIRECT_SOURCE_GONE, src.path),
                       R.string.CHOOSE_GAME, this::directChoose, false);
            return;
        }
        if (screen != Screen.DIRECT)
            directShow(Direct.CHECKING, getString(R.string.CHECKING), Theme.ACCENT, null, 0, null, false);
        if (folder)
            runWorker(R.string.CHECKING, () -> checkInPlace(f));
        else
            runWorker(R.string.CHECKING, () -> importImage(null, null, f, false));
    }

    /** The normal game choice (folder or image); a game chosen there goes on into the game (onGameChosen). */
    private void directChoose() {
        pickLibrary = false;
        go(Screen.SOURCE);
    }

    /** « Play » after the game ended by itself: the way into the game again. */
    private void directRetry() {
        directStep(true);
    }

    /** « Prepare » / « Prepare again » on DIRECT; a start refused (space, …) says why there. */
    private void directPrepare() {
        directPrepareStarted = true;
        if (!startPrepare(false)) {
            String why = noteText;
            directShow(Direct.ERROR, getString(R.string.DIRECT_PREPARE_NOT_STARTED), Theme.ERROR,
                       why.isEmpty() ? null : why, R.string.PREPARE, this::directPrepare, false);
        }
    }

    private void directFail(String head, String detail, int actionLabel, Runnable action, boolean shareLog) {
        directShow(Direct.ERROR, head, Theme.ERROR, detail, actionLabel, action, shareLog);
    }

    private void directShow(Direct st, String line, int color, String detail, int actionLabel, Runnable action,
                            boolean shareLog) {
        boolean same = screen == Screen.DIRECT;
        directState = st;
        directLine = line == null ? "" : line;
        directColor = color;
        directDetail = detail;
        directActionLabel = actionLabel;
        directAction = action;
        directShareLog = shareLog;
        if (st == Direct.ERROR)
            Log.i(TAG, "launcher: game app: " + directLine + (detail == null ? "" : " (" + detail + ")"));
        show(Screen.DIRECT, same ? 0 : 1);
    }

    /**
     * DIRECT: Home's layout without its buttons, library or settings: the game's art (its title image, else this
     * app's icon art) and title (else this app's name), one status line, a line under it when there is more to say,
     * and the one action that moves things on when the player has to act (Share log beside a failed Prepare). B
     * leaves the app (a Prepare goes on in its service).
     */
    private void buildDirect() {
        boolean ready = state() == State.READY;
        String title = ready ? gameTitle(gameDir()) : appLabel();
        LinearLayout info = hero(title, false, art != null ? art : appArt());
        View line = heroLine(info, directLine, directColor == 0 ? Theme.ACCENT : directColor);
        directStatus = line instanceof Widgets.Chip ? (Widgets.Chip)line : null;
        if (directDetail != null && !directDetail.isEmpty())
            heroLine(info, directDetail, 0);
        directPrimary = null;
        if (directAction != null) {
            LinearLayout buttons = heroButtons(info);
            Runnable act = directAction;
            directPrimary = pill(buttons, directActionLabel == R.string.PLAY ? Icon.PLAY : null,
                                 getString(directActionLabel), true, act);
            if (directShareLog)
                pill(buttons, null, getString(R.string.SHARE_LOG), false, this::shareLog);
            defaultFocus = directPrimary;
            startAction = act;
            addHint("A", R.string.HINT_SELECT, this::clickFocused);
        }
        addHint("B", R.string.HINT_QUIT, this::back);
    }

    /** This app's name (a game's own app: the game's title). */
    private String appLabel() {
        CharSequence l = getApplicationInfo().loadLabel(getPackageManager());
        return l == null || l.length() == 0 ? getString(R.string.app_name) : l.toString();
    }

    /** This app's icon art (a game's own app: the game's title image on its colour, or its monogram), or null. */
    private Bitmap appArt() {
        if (appArtBitmap == null) {
            try {
                appArtBitmap = BitmapFactory.decodeResource(getResources(), R.drawable.xba_title_icon_art);
            } catch (RuntimeException e) {
                appArtBitmap = null;
            }
        }
        return appArtBitmap;
    }

    /** Every poll on DIRECT: the Prepare's line; when it is over, the next step (the game, or what failed). */
    private void directPoll() {
        if (directState != Direct.PREPARING)
            return;
        Prep p = prep();
        if (p == Prep.RUNNING) {
            PrepareProtocol.Status s = readPrepStatus();
            if (s != null && directStatus != null)
                directStatus.set(progressLine(s), Theme.OK);
            return;
        }
        if (p == Prep.QUEUED)
            return;
        Log.i(TAG, "launcher: game app: prepare over (" + p + ")");
        directStep();
    }

    private void saveSource(PackHandover.GameSource src) {
        getSharedPreferences(HANDOVER_PREFS, MODE_PRIVATE).edit().putString(HANDOVER_GAME_KIND, src.kind.name())
                .putString(HANDOVER_GAME_PATH, src.path).apply();
    }

    /** XRecomp's last word on where it reads the game from, checked again (PackHandover.sourceOf); null for none. */
    private PackHandover.GameSource savedSource() {
        android.content.SharedPreferences p = getSharedPreferences(HANDOVER_PREFS, MODE_PRIVATE);
        Intent i = new Intent().putExtra(PackHandover.EXTRA_GAME_KIND, p.getString(HANDOVER_GAME_KIND, null))
                .putExtra(PackHandover.EXTRA_GAME_PATH, p.getString(HANDOVER_GAME_PATH, null));
        return PackHandover.sourceOf(i);
    }

    // ---- A pack that no longer matches (XBA-127, PackFreshness) ------------------------------------------------

    /**
     * The selected game's default.xbe hash as far as known without reading it: hashed in this process and unchanged
     * since (PackFreshness), else the record's (taken at import), else null.
     */
    private String selectedXbeSha() {
        String d = gameDir();
        String sha = d == null ? null : PackFreshness.knownXbeSha(GameStore.findXbe(new File(d)));
        if (sha != null)
            return sha;
        Library.Record r = rec();
        return r == null ? null : r.xbeSha256;
    }

    /** The pack of tid was built from another default.xbe than the selected game's (as far as known now). */
    private boolean otherXbe(String tid) {
        return tid != null && Toolchain.available(this)
               && PackFreshness.of(getFilesDir(), tid, selectedXbeSha(), null) == PackFreshness.Verdict.OTHER_XBE;
    }

    /** The valid pack of tid was lifted by an older toolkit than this APK's (it still plays). */
    private boolean olderToolkit(String tid) {
        return tid != null && PackFreshness.of(getFilesDir(), tid, selectedXbeSha(), Toolchain.toolkitDigest(this))
                              == PackFreshness.Verdict.OLDER_TOOLKIT;
    }

    /**
     * Before a start: hashes the selected game's default.xbe (once per change of the file, a few MB) and says whether
     * the pack was built from another one. Such a pack is never run: Home offers Prepare again.
     */
    private boolean xbeChanged() {
        String tid = titleId(), d = gameDir();
        if (tid == null || d == null || !Toolchain.available(this))
            return false;
        PackFreshness.xbeSha(GameStore.findXbe(new File(d)));
        boolean other = otherXbe(tid);
        if (other)
            Log.w(TAG, "launcher: " + tid + "'s pack was built from another default.xbe ("
                       + Pack.xbeSha256(getFilesDir(), tid) + "), not starting it");
        return other;
    }

    private String xbeHashDir;

    /** On Home: the same hash on a worker thread, so that a changed default.xbe shows before Play is pressed. */
    private void xbeShaLater(String dir, String tid) {
        if (dir == null || tid == null || dir.equals(xbeHashDir) || !Toolchain.available(this))
            return;
        File xbe = GameStore.findXbe(new File(dir));
        if (xbe == null || PackFreshness.knownXbeSha(xbe) != null)
            return;
        xbeHashDir = dir;
        new Thread(() -> {
            PackFreshness.xbeSha(xbe);
            runOnUiThread(() -> {
                xbeHashDir = null;
                if (screen == Screen.HOME && dir.equals(gameDir()) && otherXbe(tid))
                    refreshKeepingFocus();
            });
        }, "xba-xbe-sha").start();
    }

    /**
     * The valid pack of tid was built with the title's optional enhancements and the switch is now off, or the other
     * way round (pack.json title_layer, profile-free WP6). A pack that says nothing (no enhancements for the title, or
     * made before the field) never differs.
     */
    private boolean enhancementsChanged(String tid) {
        String tl = tid == null ? null : Pack.titleLayer(getFilesDir(), tid);
        return tl != null && Toolchain.available(this) && "on".equals(tl) != GameStore.enhancements(this, tid);
    }

    /** « Prepare again » for a pack lifted by an older toolkit: a plain Prepare (the game does not start after it). */
    private void onPrepareNewToolkit() {
        Log.i(TAG, "launcher: prepare again with the new toolkit " + titleId());
        startPrepare();
    }

    /**
     * The shell could not load the pack (EXTRA_BAD_PACK): drop it, so Home offers Prepare again instead of a Play that
     * fails the same way. This check is lighter than the native one (no dlopen), hence passed it.
     */
    private void dropBadPack(String why) {
        String tid = titleId();
        Log.w(TAG, "launcher: pack " + tid + " refused by the shell (" + why + "), dropped");
        if (tid != null) {
            Pack.drop(getFilesDir(), tid);
            droppedPacks.add(tid);
        }
    }

    /** « Open the game directly » does not skip a Prepare that is needed. */
    private boolean prepareBlocksPlay() {
        Prep p = prep();
        return p != Prep.OFF && p != Prep.PLAYABLE;
    }

    /** « about 20–30 min »: from the cores and the RAM; a guess until tools.prepare gives its own eta. */
    private String estimate() {
        int cores = Runtime.getRuntime().availableProcessors();
        long ram = Toolchain.totalRam(this);
        int lo, hi;
        if (ram > 0 && ram < PREPARE_RAM_WARN) {
            lo = 40;
            hi = 60;
        } else if (cores >= 8) {
            lo = 20;
            hi = 30;
        } else {
            lo = 30;
            hi = 45;
        }
        return getString(R.string.PREPARE_ESTIMATE, lo, hi);
    }

    /** « Compile · 42 % · about 12 min left ». */
    private String progressLine(PrepareProtocol.Status s) {
        String line = PrepareService.stepName(this, s.id);
        int pct = s.step > 0 ? Math.round(PrepareProtocol.overall(s.step, s.n, s.pct) * 100) : s.pct;
        line += " · " + String.format(Locale.getDefault(), "%d%%", pct);
        if (s.eta >= 0)
            line += " · " + getString(R.string.PREPARE_ETA, PrepareProtocol.formatEta(s.eta));
        return line;
    }

    private long internalFree() {
        return getFilesDir().getUsableSpace();
    }

    /** Checks the space, then starts PrepareService and shows the Preparing screen. */
    private void startPrepare() {
        if (romEntry != null) {   // a GBA game's Retry / Resume (XBA-158)
            startRomPrepare(romEntry);
            return;
        }
        startPrepare(false);
    }

    /**
     * background (XBA-129, a self-healing round after a run that ended well): the Prepare runs while Home stays, Play
     * included (the valid pack plays until the new one replaces it).
     */
    private boolean startPrepare(boolean background) {
        // SOURCE: the game's files, or, before an import, the library's cached default.xbe (tools.prepare needs
        // only the XBE).
        String tid = titleId(), dir = gameDir();
        if (dir == null && state() == State.LIBRARY) {
            File x = cachedXbeDir();
            dir = x == null ? null : x.getAbsolutePath();
        }
        if (tid == null || dir == null)
            return false;
        if (copying(tid)) {   // XBA-176: XRecomp's pack is on its way
            setNote(getString(R.string.HANDOVER_BUSY), NOTE_INFO);
            return false;
        }
        if (!direct)
            handoverNoteText = null;   // a game's own app keeps saying why it prepares
        long free = internalFree();
        if (free < PREPARE_MIN_FREE) {
            setNote(getString(R.string.PREPARE_NO_SPACE, size(PREPARE_MIN_FREE), size(free)), NOTE_ERROR);
            return false;
        }
        if (Build.VERSION.SDK_INT >= 33
            && checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS)
               != android.content.pm.PackageManager.PERMISSION_GRANTED) {
            try {
                requestPermissions(new String[] { android.Manifest.permission.POST_NOTIFICATIONS }, REQ_NOTIFICATIONS);
            } catch (RuntimeException ignored) {
            }
        }
        // A fresh status first, so the Preparing screen never shows the previous run's outcome.
        PrepareProtocol.Status s = new PrepareProtocol.Status();
        s.titleId = tid;
        s.startedAt = s.updatedAt = System.currentTimeMillis();
        s.msg = getString(R.string.PREPARE_STARTING);
        s.log = PrepareService.logFile(this, tid).getAbsolutePath();
        s.background = background;
        File sf = PrepareService.statusFile(this, tid);
        try {
            PrepareProtocol.writeStatus(sf, s);
            startForegroundService(PrepareService.startIntent(this, dir, tid, gameTitle(gameDir()))
                                           .putExtra(PrepareService.EXTRA_BACKGROUND, background));
        } catch (IOException | RuntimeException e) {
            Log.w(TAG, "launcher: cannot start the Prepare", e);
            s.state = PrepareProtocol.FAILED;
            s.code = "START_FAILED";
            s.msg = String.valueOf(e.getMessage());
            try {
                PrepareProtocol.writeStatus(sf, s);
            } catch (IOException ignored) {
            }
            setNote(getString(R.string.PREPARE_START_FAILED, String.valueOf(e.getMessage())), NOTE_ERROR);
            return false;
        }
        Log.i(TAG, "launcher: prepare started for " + tid + " (" + dir + ")"
                   + (background ? ", in the background" : ""));
        cancelSent = false;
        if (background) {
            refreshKeepingFocus();
            return true;
        }
        if (direct) {   // a game's own app: its one line follows the Prepare, then the game starts
            directStep();
            return true;
        }
        go(Screen.PREPARE);
        long ram = Toolchain.totalRam(this);
        if (ram > 0 && ram < PREPARE_RAM_WARN)
            setNote(getString(R.string.PREPARE_LOW_RAM, size(ram)), NOTE_WARN);
        return true;
    }

    /** The progress bar's width on the Preparing screen. */
    static final int PREPARE_BAR_W = 960;

    /**
     * Preparing: Home's layout with the buttons replaced by the progress (so nothing jumps): the title, « Preparing ·
     * step 5 of 6 · Compile », one bar, the percentage and the time left, and that the app can be left. Nothing to
     * focus while it runs: Y hides it, B cancels (twice). Failed or stopped: the status line says so, the reason
     * under it, and Retry (or Resume) and Share log as buttons.
     */
    private void buildPrepare() {
        String title = prepTitle();
        LinearLayout info = romEntry != null ? hero(title, false, null) : hero(title, false);
        if (companion != null)
            companion.preparing(prepTid(), title);
        prepStatus = (Widgets.Chip)heroLine(info, getString(R.string.PREPARE_STARTING), Theme.OK);

        LinearLayout progress = column();
        prepMeter = new Widgets.Meter(this);
        progress.addView(prepMeter, lp(MATCH, 12));
        LinearLayout nums = rowLayout();
        nums.setGravity(Gravity.CENTER_VERTICAL);
        prepPct = Theme.oneLine(Theme.text(this, Theme.T_TITLE, Theme.MEDIUM, Theme.TEXT, ""));
        nums.addView(prepPct, lp(0, WRAP, 1));
        prepEta = Theme.oneLine(Theme.text(this, Theme.T_BODY, Theme.REGULAR, Theme.TEXT_DIM, ""));
        nums.addView(prepEta, lp(WRAP, WRAP));
        LinearLayout.LayoutParams nl = lp(MATCH, WRAP);
        nl.topMargin = Theme.S3;
        progress.addView(nums, nl);
        LinearLayout.LayoutParams pl = lp(PREPARE_BAR_W, WRAP);
        pl.topMargin = Theme.S5;
        info.addView(progress, pl);
        prepProgress = progress;

        prepCaption = Theme.text(this, Theme.T_BODY, Theme.REGULAR, Theme.TEXT_DIM, getString(R.string.PREPARE_KEEPS));
        LinearLayout.LayoutParams cl = lp(PREPARE_BAR_W, WRAP);
        cl.topMargin = Theme.S5;
        info.addView(prepCaption, cl);
        prepMsg = Theme.text(this, Theme.T_BODY, Theme.REGULAR, Theme.TEXT, "");
        prepMsg.setMaxLines(3);
        prepMsg.setEllipsize(TextUtils.TruncateAt.END);
        prepMsg.setLineSpacing(0, 1.2f);
        LinearLayout.LayoutParams ml = lp(PREPARE_BAR_W, WRAP);
        ml.topMargin = Theme.S3;
        info.addView(prepMsg, ml);

        PrepareProtocol.Status s = readPrepStatus();
        boolean running = s == null || statusAlive(s);
        if (running) {
            prepMsg.setVisibility(View.GONE);
            yAction = this::leavePrepare;
            addHint("Y", R.string.HINT_HIDE, this::leavePrepare);
            addHint("B", R.string.CANCEL, this::armCancelPrepare);
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        } else {
            progress.setVisibility(View.GONE);
            prepCaption.setVisibility(View.GONE);
            boolean cancelled = PrepareProtocol.CANCELLED.equals(s.state) || PrepareProtocol.RUNNING.equals(s.state);
            LinearLayout buttons = heroButtons(info);
            Widgets.PillButton retry = pill(buttons, Icon.RESET,
                                            getString(cancelled ? R.string.PREPARE_RESUME : R.string.PREPARE_RETRY),
                                            true, this::startPrepare);
            pill(buttons, Icon.COPY, getString(R.string.SHARE_LOG), false, this::shareLog);
            defaultFocus = retry;
            yAction = this::shareLog;
            addHint("A", R.string.HINT_SELECT, this::clickFocused);
            addHint("Y", R.string.SHARE_LOG, this::shareLog);
            addHint("B", R.string.HINT_BACK, this::back);
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        }
        paintPrepare(s);
    }

    /** Y on the Preparing screen: back to the menu, the Prepare carries on. */
    private void leavePrepare() {
        getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (history.isEmpty())
            show(romEntry != null ? Screen.ROM_HOME : Screen.HOME);
        else
            show(history.pop());
    }

    /** Fills the Preparing screen from a status (null: just started). */
    private void paintPrepare(PrepareProtocol.Status s) {
        prepShown = s;
        if (prepStatus == null)
            return;
        boolean running = s == null || statusAlive(s);
        boolean failed = s != null && !running && PrepareProtocol.FAILED.equals(s.state);
        boolean stopped = s != null && !running && !failed;
        if (failed) {
            prepStatus.set(getString(R.string.PREPARE_FAILED_HEAD) + (s.code.isEmpty() ? "" : " · " + s.code),
                           Theme.ERROR);
            String why = s.msg;
            if (PrepareRunner.KILLED.equals(s.code))
                why = getString(R.string.PREPARE_HINT_KILLED);
            else if ("NO_SPACE".equals(s.code))
                why = getString(R.string.PREPARE_HINT_NO_SPACE, size(internalFree()))
                      + (why.isEmpty() ? "" : " " + why);
            prepMsg.setText(why);
        } else if (stopped) {
            prepStatus.set(getString(R.string.PREPARE_CANCELLED_HEAD), Theme.WARN);
            prepMsg.setText(getString(PrepareProtocol.RUNNING.equals(s.state) ? R.string.PREPARE_INTERRUPTED
                                                                              : R.string.PREPARE_CANCELLED_TEXT));
        } else {
            String step = s == null ? getString(R.string.PREPARE_STARTING) : PrepareService.stepName(this, s.id);
            prepStatus.set(s != null && s.step > 0 ? getString(R.string.PREPARE_STATUS, s.step, s.n, step)
                                                   : getString(R.string.PREPARE_STATUS_SHORT, step), Theme.OK);
            float frac = s == null ? -1 : s.step > 0 ? PrepareProtocol.overall(s.step, s.n, s.pct) : -1;
            prepMeter.set(Math.max(0, frac), Theme.ACCENT);
            prepPct.setText(frac >= 0 ? String.format(Locale.getDefault(), "%d %%", Math.round(frac * 100)) : "");
            prepEta.setText(s != null && s.eta >= 0 ? getString(R.string.PREPARE_ETA_BIG,
                                                                PrepareProtocol.formatEta(s.eta))
                                                    : getString(R.string.PREPARE_ETA_UNKNOWN));
        }
        if (dimmed)
            paintDim();
    }

    /** Twice within CONFIRM_MS: the first press arms, the second sends ACTION_CANCEL to the service. */
    private void armCancelPrepare() {
        if (cancelSent)
            return;
        long now = SystemClock.uptimeMillis();
        if (cancelArmedAt == 0 || now - cancelArmedAt > CONFIRM_MS) {
            cancelArmedAt = now;
            setNote(getString(R.string.PREPARE_CANCEL_CONFIRM), NOTE_WARN);
            return;
        }
        cancelArmedAt = 0;
        cancelSent = true;
        String tid = prepTid();
        setNote(getString(R.string.CANCELING), NOTE_INFO);
        Log.i(TAG, "launcher: prepare cancel");
        try {
            startService(PrepareService.cancelIntent(this, tid));
        } catch (RuntimeException e) {
            Log.w(TAG, "launcher: cannot reach the Prepare service", e);
        }
    }

    /** « Share log »: prepare.log through LogProvider, to whatever app the player picks. */
    private void shareLog() {
        String tid = prepTid();
        if (tid == null || !PrepareService.logFile(this, tid).isFile()) {
            setNote(getString(R.string.SHARE_LOG_NONE), NOTE_INFO);
            return;
        }
        Uri uri = LogProvider.uriFor(this, tid);
        Intent send = new Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_STREAM, uri)
                .putExtra(Intent.EXTRA_SUBJECT, getString(R.string.SHARE_LOG_SUBJECT, prepTitle(), tid))
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        send.setClipData(android.content.ClipData.newRawUri("prepare.log", uri));
        try {
            startActivity(Intent.createChooser(send, getString(R.string.SHARE_LOG)));
        } catch (ActivityNotFoundException e) {
            setNote(getString(R.string.SHARE_LOG_NO_APP), NOTE_ERROR);
        }
    }

    /** Every PREPARE_POLL_MS while resumed: status.json into the Preparing screen, or into Home's row. */
    private void pollPrepare() {
        if (!resumed)
            return;
        poller.postDelayed(pollTick, PREPARE_POLL_MS);
        if (screen == Screen.DIRECT) {
            directPoll();
        } else if (screen == Screen.PREPARE) {
            PrepareProtocol.Status s = readPrepStatus();
            boolean wasRunning = prepShown == null || statusAlive(prepShown);
            boolean isRunning = s == null || statusAlive(s);
            if (s != null && PrepareProtocol.DONE.equals(s.state)) {
                onPrepareDone();
                return;
            }
            if (wasRunning != isRunning) {
                cancelSent = false;
                show(Screen.PREPARE);   // running -> failed / cancelled: the actions change
                return;
            }
            paintPrepare(s);
            checkDim(isRunning);
        } else if (screen == Screen.ROM_HOME && romEntry != null && romPrepShown != null) {
            Prep now = romPrep(romEntry());
            if (now != romPrepShown) {
                refreshKeepingFocus();
                return;
            }
            if (now == Prep.RUNNING) {
                PrepareProtocol.Status s = readPrepStatus();
                if (s != null && homeStatus != null)
                    homeStatus.set(progressLine(s), Theme.OK);
            }
        } else if (screen == Screen.HOME && homeChip != null) {
            LibraryScreen.Entry e = ++libPolls % LIBRARY_POLL_EVERY == 0 ? currentEntry() : null;
            if (e != null && e.chip != homeChip)
                refreshKeepingFocus();
        } else if (screen == Screen.LIBRARY && !scanning()) {
            // (not while the search is typed: the field and its keyboard would go)
            if (++libPolls % LIBRARY_POLL_EVERY == 0 && (libSearch == null || !libSearch.hasFocus())
                && libraryChanged())
                refreshKeepingFocus();
        } else if (screen == Screen.HOME && homePrep != null) {
            Prep now = prep();
            if (now != homePrep) {
                if (now == Prep.PLAYABLE && relaunchAfterPrepare(titleId()))
                    return;
                refreshKeepingFocus();
                if (now == Prep.PLAYABLE)
                    setNote(getString(R.string.READY_TO_PLAY, gameTitle(gameDir())), NOTE_OK);
                return;
            }
            if (now == Prep.RUNNING) {
                PrepareProtocol.Status s = readPrepStatus();
                if (s != null && homeStatus != null)
                    homeStatus.set(progressLine(s), Theme.OK);
            }
        }
    }

    /** The Prepare finished: Home with Play focused, the « done » sound and haptic, « TITLE is ready ». */
    private void onPrepareDone() {
        getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (romEntry != null) {   // a GBA game (XBA-158): its Home, Play first
            boolean ok = Pack.check(getFilesDir(), romEntry.tid) == Pack.Check.VALID;
            Log.i(TAG, "launcher: prepare done for " + romEntry.tid + ", pack " + (ok ? "valid" : "not valid"));
            LibraryScreen.Entry e = romEntry;
            history.clear();
            history.push(Screen.LIBRARY);
            focusMemory.remove(Screen.ROM_HOME);
            romEntry = e;
            show(Screen.ROM_HOME);
            if (ok) {
                restoreFocus(true);
                feedback.play(UiFeedback.Cue.DONE, host);
                setNote(getString(R.string.ROM_PREPARED_DONE, e.title()), NOTE_OK);
            }
            return;
        }
        String tid = titleId();
        boolean ok = tid != null && Pack.check(getFilesDir(), tid) == Pack.Check.VALID;
        Log.i(TAG, "launcher: prepare done, pack " + (ok ? "valid" : "not valid"));
        if (ok && relaunchAfterPrepare(tid))
            return;
        history.clear();
        focusMemory.remove(Screen.HOME);
        show(Screen.HOME);
        if (ok) {
            restoreFocus(true);   // Play, even after a touch: the next press of A plays
            feedback.play(UiFeedback.Cue.DONE, host);
            setNote(getString(state() == State.LIBRARY ? R.string.LIB_PREPARED_IMPORT : R.string.READY_TO_PLAY,
                              gameTitle(gameDir())), NOTE_OK);
        }
    }

    // ---- A GBA game's Home and Prepare (XBA-158; docs/android/app-shell.md « Game Boy Advance games ») ------

    /** The title ID the Prepare screens follow: the GBA game's library key (gba-<KEY>), else the Xbox game's. */
    private String prepTid() {
        return romEntry != null ? romEntry.tid : titleId();
    }

    private String prepTitle() {
        return romEntry != null ? romEntry.title() : gameTitle(gameDir());
    }

    /** romEntry read again (its pack, its Prepare status, the running game). */
    private LibraryScreen.Entry romEntry() {
        romEntry = LibraryScreen.entry(getFilesDir(), romEntry.tid, games, index, libEnv());
        return romEntry;
    }

    /** The Prepare state of a GBA game: Prep.PLAYABLE once its pack is valid, NEEDED before; Play works in both. */
    private static Prep romPrep(LibraryScreen.Entry e) {
        GameState.Facts f = e.facts;
        return GameState.prep(f.toolchain, f.pack == Pack.Check.VALID, f.prepare, f.prepareFresh, f.prepareProcess);
    }

    /**
     * A GBA game's, a PS1 disc's or an N64 cartridge's Home: its title, one line (« Prepared · runs natively », « Not
     * prepared · plays in the interpreter », the Prepare's progress or failure, new code found since the pack), Play
     * first (always: the interpreter needs no pack) and the Prepare action beside it (not without the
     * toolchain, which only an N64 Home is opened without: Play and its pak then), the N64's pak last.
     */
    private void buildRomHome() {
        LibraryScreen.Entry e = romEntry();
        Prep p = romPrep(e);
        romPrepShown = p;
        PrepareProtocol.Status ps = e.facts.prepare;
        boolean prepares = romPrepares(e) && p != Prep.OFF;   // OFF: no toolchain
        int found = p == Prep.PLAYABLE ? Pack.newlyDiscovered(getFilesDir(), e.tid) : 0;
        // As an Xbox title's Home (XBA-127): a pack lifted by another toolkit than this APK's still plays, but misses
        // the lifter's fixes since, so Prepare again is offered.
        boolean older = prepares && p == Prep.PLAYABLE
                        && PackFreshness.judge(null, null, Pack.toolkitSha256OfKey(getFilesDir(), e.tid),
                                               Toolchain.toolkitDigest(this)) == PackFreshness.Verdict.OLDER_TOOLKIT;
        LinearLayout info = hero(e.title(), false, null);
        String line;
        int color;
        switch (prepares ? p : Prep.OFF) {
        case RUNNING:
            line = ps == null ? getString(R.string.PREPARE_STARTING) : progressLine(ps);
            color = Theme.OK;
            break;
        case QUEUED:
            line = queuedLabel(e.tid);
            color = Theme.ACCENT;
            break;
        case FAILED:
            line = getString(R.string.CHIP_PREPARE_FAILED) + (ps == null || ps.code.isEmpty() ? "" : " · " + ps.code);
            color = Theme.ERROR;
            break;
        case PLAYABLE:
            line = older ? getString(R.string.HOME_OLDER_TOOLKIT)
                 : found > 0 ? getString(R.string.ROM_NEW_CODE, found) : getString(R.string.ROM_PREPARED);
            color = older || found > 0 ? Theme.WARN : Theme.OK;
            break;
        default:
            line = getString(prepares ? R.string.ROM_NOT_PREPARED : R.string.ROM_PS1_LINE);
            color = Theme.TEXT_DIM;
            break;
        }
        View status = heroLine(info, line, color);
        if (p == Prep.RUNNING && status instanceof Widgets.Chip)
            homeStatus = (Widgets.Chip)status;
        if (e.record != null && (e.record.lastPlayed > 0 || e.record.playMs > 0))   // XBA-206: as an Xbox game's Home
            heroLine(info, homeMeta(e.record.lastPlayed, e.record.playMs), 0);

        LinearLayout buttons = heroButtons(info);
        // Where the player left off saved: Resume (that point) first, Start (the console's boot) beside it.
        boolean resumable = GameStore.canResume(this, e.tid);
        Widgets.PillButton play = pill(buttons, Icon.PLAY, getString(resumable ? R.string.ROM_RESUME : R.string.PLAY),
                                       true, () -> launchRom(e));
        if (resumable)
            pill(buttons, null, getString(R.string.ROM_START), false, () -> launchRom(e, true));
        switch (prepares ? p : Prep.PLAYABLE) {   // PLAYABLE without new code: no second button
        case RUNNING:
            pill(buttons, null, getString(R.string.PREPARING), false, () -> go(Screen.PREPARE));
            break;
        case QUEUED:
            pill(buttons, null, queuedLabel(e.tid), false, () -> dequeuePrepare(e.tid));
            break;
        case FAILED:
            pill(buttons, null, getString(R.string.PREPARE_SEE_WHY), false, () -> go(Screen.PREPARE));
            break;
        case PLAYABLE:
            if (older || found > 0)
                pill(buttons, null, getString(R.string.PREPARE_AGAIN), false, () -> startRomPrepare(e));
            break;
        default:
            pill(buttons, null, getString(R.string.PREPARE), false, () -> startRomPrepare(e));
            break;
        }
        boolean n64 = e.console() == Console.N64;
        if (n64) {   // XBA-174: the accessory in the controller, this game's choice (N64Pak)
            boolean rumble = n64Rumble(e.tid);
            pill(buttons, null, getString(rumble ? R.string.N64_PAK_RUMBLE : R.string.N64_PAK_CONTROLLER), false,
                 () -> {
                     N64Pak.setChoice(this, e.tid, N64Pak.toggled(rumble));
                     Log.i(TAG, "launcher: " + e.tid + " plays with the " + (rumble ? "controller" : "rumble") + " pak");
                     show(Screen.ROM_HOME, 0);
                 });
        }
        // XBA-158 / XBA-168 / XBA-174: the console's buttons remapped, for all its games or this one (ROM_CONTROLS).
        pill(buttons, Icon.PAD, getString(R.string.CONTROLS), false, () -> go(Screen.ROM_CONTROLS));
        // XBA-181: this game's picture (scale, filter) and its stored files (saves, Prepare files): ROM_OPTIONS.
        pill(buttons, null, getString(R.string.ROM_OPTIONS), false, () -> go(Screen.ROM_OPTIONS));
        homePrimary = play;
        defaultFocus = play;
        startAction = () -> launchRom(e);
        addHint("A", R.string.HINT_SELECT, this::clickFocused);
        addHint("B", R.string.LIB_TITLE, this::back);
        favouriteHint(e);   // XBA-206
        if (companion != null) {
            companion.art(null);
            companion.home(e.title(), line, color, true, play.title);
        }

        if (e.chip == GameState.Chip.RUNNING)
            setNote(getString(R.string.GAME_RUNNING_HOME), NOTE_INFO);
        else if (n64 && !prepares)
            setNote(getString(n64Rumble(e.tid) ? R.string.ROM_N64_NOTE_RUMBLE : R.string.ROM_N64_NOTE), NOTE_INFO);
        else if (!prepares)
            setNote(getString(R.string.ROM_PS1_NOTE), NOTE_INFO);
        else if (p == Prep.FAILED)
            setNote(getString(R.string.ROM_FAILED_NOTE, ps == null ? "" : ps.code), NOTE_WARN);
        else if (p == Prep.PLAYABLE)
            setNote(older ? getString(R.string.ROM_OLDER_TOOLKIT_NOTE)
                    : found > 0 ? getString(R.string.ROM_NEW_CODE_NOTE, found) : getString(R.string.ROM_PREPARED_NOTE),
                    NOTE_INFO);
        else if (p == Prep.NEEDED || p == Prep.CANCELLED || p == Prep.INTERRUPTED)
            setNote(getString(n64 ? R.string.ROM_N64_NOT_PREPARED_NOTE : R.string.ROM_NOT_PREPARED_NOTE), NOTE_INFO);
    }

    /** An N64 game plays with the rumble pak (N64Pak): its own choice, else recomp.cfg's RECOMP_N64_PAK line. */
    private boolean n64Rumble(String tid) {
        File ext = GameStore.externalFilesDir(this);
        return N64Pak.rumble(N64Pak.choice(this, tid), ext == null ? null : RecompConfig.load(ext).get(N64Pak.ENV));
    }

    /**
     * Whether a game that starts from its file has a Prepare: its console's (Console.prepares: the GBA, the PS1, the
     * N64). A PS1 .chd too (XBA-168: PrepareService.ps1Source reads its boot EXE through the native disc reader).
     */
    static boolean romPrepares(LibraryScreen.Entry e) {
        return e.console().prepares();
    }

    /**
     * Prepare (or Prepare again) for a GBA game, a PS1 disc or an N64 cartridge: PrepareService with the ROM or the
     * disc (a path, or the document URI: the service copies a ROM into the game's work folder, the sectors of a disc
     * that lead to its boot EXE) and its library key as title ID; the Preparing screen follows. The space check is the Xbox one's: a pack
     * is small, but the compiler and its temporary files are not.
     */
    private void startRomPrepare(LibraryScreen.Entry e) {
        Library.Folder folder = e.active == null ? null : Library.folderOf(Library.folders(this), e.active);
        String rom = romSpec(e.active, folder == null ? null : folder.tree);
        if (rom == null) {
            setNote(getString(R.string.LIB_SOURCE_GONE_NOTE, e.title()), NOTE_WARN);
            return;
        }
        long free = internalFree();
        if (free < PREPARE_MIN_FREE) {
            setNote(getString(R.string.PREPARE_NO_SPACE, size(PREPARE_MIN_FREE), size(free)), NOTE_ERROR);
            return;
        }
        if (Build.VERSION.SDK_INT >= 33
            && checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS)
               != android.content.pm.PackageManager.PERMISSION_GRANTED) {
            try {
                requestPermissions(new String[] { android.Manifest.permission.POST_NOTIFICATIONS }, REQ_NOTIFICATIONS);
            } catch (RuntimeException ignored) {
            }
        }
        PrepareProtocol.Status s = new PrepareProtocol.Status();
        s.titleId = e.tid;
        s.startedAt = s.updatedAt = System.currentTimeMillis();
        s.msg = getString(R.string.PREPARE_STARTING);
        s.log = PrepareService.logFile(this, e.tid).getAbsolutePath();
        File sf = PrepareService.statusFile(this, e.tid);
        try {
            PrepareProtocol.writeStatus(sf, s);
            startForegroundService(PrepareService.startIntent(this, rom, e.tid, e.title()));
        } catch (IOException | RuntimeException ex) {
            Log.w(TAG, "launcher: cannot start the Prepare", ex);
            s.state = PrepareProtocol.FAILED;
            s.code = "START_FAILED";
            s.msg = String.valueOf(ex.getMessage());
            try {
                PrepareProtocol.writeStatus(sf, s);
            } catch (IOException ignored) {
            }
            setNote(getString(R.string.PREPARE_START_FAILED, String.valueOf(ex.getMessage())), NOTE_ERROR);
            return;
        }
        Log.i(TAG, "launcher: prepare started for " + e.tid + " (" + rom + ")");
        cancelSent = false;
        romEntry = e;
        go(Screen.PREPARE);
    }

    LibraryScreen.Entry romEntryForTest() {
        return romEntry;
    }

    // ---- LIBRARY (docs/android/app-shell.md « Game library ») ------------------------------------------------

    /** The game library is on: a library folder is set, or the launcher knows two games or more. */
    private boolean libraryOn() {
        return !Library.folders(this).isEmpty() || games.games.size() >= 2;
    }

    private void setIndex(LibraryIndex x) {
        index = x;
        indexGames = x.games();
    }

    /**
     * XBA-206: a session still open when another game starts (the last one was left in the background, Back to
     * launcher): it ends now, with the time its game was on screen.
     */
    private void closeOpenSession(long now) {
        long start = GameStore.sessionStartedAt(this);
        if (start <= 0)
            return;
        String tid = GameStore.sessionTid(this);
        long d = PlayClock.sessionLength(PlayClock.read(PlayClock.file(getFilesDir())), start, now);
        GameStore.sessionEnd(this, now, d);
        if (tid != null && games != null && games.get(tid) != null) {
            Library.sessionAdd(games, tid, start, d);
            writeGames();
        }
        Log.i(TAG, "launcher: play session of " + tid + " closed at the next launch: " + d / 1000 + " s on screen");
    }

    private void writeGames() {
        try {
            Library.write(Library.gamesFile(getFilesDir()), games);
        } catch (IOException e) {
            Log.w(TAG, "launcher: library: cannot write games.json", e);
        }
    }

    private LibraryScreen.Env libEnv() {
        LibraryScreen.Env e = new LibraryScreen.Env();
        e.toolchain = Toolchain.available(this);
        // Each library folder can be read: All files access for a path, the persisted permission for a tree.
        e.accessible = true;
        e.noAccess = Library.unreadableFolders(this, Environment.isExternalStorageManager());
        e.prepareProcess = prepareProcessAlive();
        e.runningTid = gameRunning() ? launchedTid() : null;
        e.importingTid = worker != null && worker.isAlive() ? importTid : null;
        e.queue = PrepareProtocol.readQueue(PrepareProtocol.queueFile(getFilesDir()));
        e.packDropped = droppedPacks;
        e.toolkit = Toolchain.toolkitDigest(this);
        return e;
    }

    /** The selected game as a library entry (its chip), or null. */
    private LibraryScreen.Entry currentEntry() {
        Library.Record r = games.selected();
        return r == null ? null : LibraryScreen.entry(getFilesDir(), r.titleId, games, index, libEnv());
    }

    /** The library's focused card's entry, else the selected game's. */
    private LibraryScreen.Entry focusedEntry() {
        if (screen == Screen.LIBRARY) {
            for (LibraryScreen.Entry e : libEntries) {
                if (e.tid.equals(focusTid))
                    return e;
            }
            return null;
        }
        return currentEntry();
    }

    /** The folder of the selected game's cached default.xbe (the scan's copy), or null. */
    private File cachedXbeDir() {
        Library.Record r = games.selected();
        if (r == null)
            return null;
        LibraryIndex.Item it = LibraryIndex.active(indexGames.get(r.titleId), r.source);
        File x = it == null ? null : it.xbeFile(getFilesDir());
        return x == null || !x.isFile() ? null : x.getParentFile();
    }

    /** Opens the library (« Library » from Home, Game data or the bottom screen). */
    private void openLibrary() {
        history.clear();
        show(Screen.LIBRARY, -1);
    }

    /** e becomes the selected game (a record is made for a game seen in the folder only). */
    private void selectEntry(LibraryScreen.Entry e) {
        if (e.otherConsole())
            return;   // never the selected game: Home, Play and Prepare are the Xbox's
        boolean isNew = games.get(e.tid) == null;
        Library.Record r = LibraryScreen.recordFor(e, System.currentTimeMillis());
        if (isNew)
            games.put(r);
        if (isNew || !e.tid.equals(games.selected)) {
            games.selected = e.tid;
            writeGames();
            switchArmedAt = 0;
            focusMemory.remove(Screen.HOME);
        }
        titleIdDir = null;
        artTid = null;
    }

    /**
     * ACTION_PLAY_TITLE: tid becomes the selected game when Home can show it (a record with its files set, or a
     * game of the library folder; a record is made for the latter, as A on its card does). False when it cannot (no
     * title ID, deleted from the library, its source gone with no files): the library opens with a note.
     */
    private boolean selectShortcutTitle(String tid) {
        if (tid == null || tid.isEmpty())
            return false;
        Library.Record r = games.get(tid);
        boolean inFolder = indexGames.containsKey(tid);
        if (!inFolder && (r == null || r.dataDir.isEmpty()))
            return false;
        selectEntry(LibraryScreen.entry(getFilesDir(), tid, games, index, libEnv()));
        return true;
    }

    /**
     * The selected game would start from the shortcut: what « Open the game directly » asks (its files, no Prepare
     * needed or running, the pack built from this default.xbe), not a wrong game, and no crash round for Home to
     * offer (« Prepare again », « Needs a fix »). Otherwise its Home shows why.
     */
    private boolean shortcutPlayable() {
        if (state() != State.READY || prepareBlocksPlay() || xbeChanged())
            return false;
        String tid = titleId();
        SelfHeal.State heal = tid == null ? null : selfHeal(tid, gameRunning(), prep());
        return heal == null || heal.offer == SelfHeal.Offer.IMPROVE;
    }

    /**
     * A on a card: the game's Home, in its colours (a GBA game: launched at once, XBA-157; a game of another console:
     * why it does not play yet).
     */
    private void openGame(LibraryScreen.Entry e) {
        if (e.directLaunch()) {
            // Its Home: Play and Prepare with the toolchain; an N64 game's always (Play and its pak, XBA-174).
            if ((Toolchain.available(this) || e.console() == Console.N64) && e.chip != GameState.Chip.NO_FILE_ACCESS) {
                Log.i(TAG, "launcher: library: open " + e.tid + " (" + e.title() + ", " + e.chip + ")");
                romEntry = e;
                go(Screen.ROM_HOME);
                return;
            }
            launchRom(e);
            return;
        }
        if (notPlayableYet(e))
            return;
        selectEntry(e);
        Log.i(TAG, "launcher: library: open " + e.tid + " (" + e.title() + ", " + e.chip + ")");
        go(Screen.HOME);
    }

    /**
     * A game of another console than the Xbox (XBA-178): listed, not playable until its console's runtime is in;
     * A or Start on its card says so. True when e is one.
     */
    private boolean notPlayableYet(LibraryScreen.Entry e) {
        if (!e.otherConsole() || e.directLaunch())
            return false;
        Log.i(TAG, "launcher: library: " + e.tid + " (" + e.title() + ") is not playable yet");
        setNote(getString(R.string.LIB_NOT_PLAYABLE_NOTE, e.console().badge()), NOTE_INFO);
        return true;
    }

    /** Start on a card (or the bottom screen's big button): the game's primary action, without opening Home. */
    private void libraryPrimary(LibraryScreen.Entry e) {
        if (e.directLaunch()) {
            launchRom(e);
            return;
        }
        if (notPlayableYet(e))
            return;
        selectEntry(e);
        onPlay();
    }

    /**
     * A GBA game (XBA-157), a PS1 disc (XBA-168) or an N64 cartridge (XBA-174): straight into the game activity with its
     * file, no Home, Prepare, import or pack. The ROM is
     * handed over as it is (GameStore.rememberRomLaunch: a path, or the document's URI that ShellActivity opens); the
     * same ROM running is brought back, another game running is ended first (one game process at a time).
     */
    private void launchRom(LibraryScreen.Entry e) {
        launchRom(e, false);
    }

    /** The same; fresh: from the console's boot, not where the player left off (the Home's Start). */
    private void launchRom(LibraryScreen.Entry e, boolean fresh) {
        if (e.chip == GameState.Chip.NO_FILE_ACCESS) {
            onChooseLibraryFolder();
            return;
        }
        Library.Folder folder = e.active == null ? null : Library.folderOf(Library.folders(this), e.active);
        String rom = romSpec(e.active, folder == null ? null : folder.tree);
        if (rom == null) {
            setNote(getString(R.string.LIB_SOURCE_GONE_NOTE, e.title()), NOTE_WARN);
            return;
        }
        if (!fresh && rom.equals(GameStore.launchedRom(this)) && gameRunning()) {
            Log.i(TAG, "launcher: library: " + e.tid + " runs, bringing it back");
            resumeShell();
            return;
        }
        if (mainProcessPid() != 0)
            endRunningGame();
        GameStore.rememberRomLaunch(this, rom, e.title(), e.tid,
                                    romParts(e.active, folder == null ? null : folder.tree));
        // XBA-206: a play session as an Xbox game's (Last played, Play time, « Recently played »): its record holds
        // them (never the selected game: Library.Games.selected skips the other consoles).
        long now = System.currentTimeMillis();
        closeOpenSession(now);
        if (games.get(e.tid) == null)
            games.put(LibraryScreen.recordFor(e, now));
        GameStore.sessionStart(this, now, e.tid);
        Library.sessionStart(games, e.tid, now);
        writeGames();
        if (fresh)
            GameStore.requestFreshStart(this);
        Log.i(TAG, "launcher: library: " + (fresh ? "start " : "play ") + e.tid + " (" + e.title() + ") from " + rom);
        startActivity(new Intent(this, ShellActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK));
        finish();
    }

    /**
     * What the game process opens for a ROM of the library: its path (« file: » item), or the document's URI under its
     * library folder's tree (« doc: » item, Storage Access Framework); null when neither can be made.
     */
    static String romSpec(LibraryIndex.Item it, String tree) {
        if (it == null || it.loc.isEmpty())
            return null;
        File f = LibraryIndex.locFile(it.loc);
        if (f != null)
            return f.getAbsolutePath();
        if (it.loc.startsWith("doc:") && tree != null && !tree.isEmpty())
            return DocumentsContract.buildDocumentUriUsingTree(Uri.parse(tree), it.loc.substring(4)).toString();
        return null;
    }

    /**
     * The track documents of a PS1 disc's .cue seen through the Storage Access Framework (XBA-168): their URIs under the
     * folder's tree, in the index's order, which ShellActivity opens and names in a .cue of its own (Ps1Cue). Empty for
     * anything else (a .cue read as a file finds its tracks itself; a single-file image or a ROM has none).
     */
    static List<String> romParts(LibraryIndex.Item it, String tree) {
        List<String> out = new ArrayList<>();
        if (it == null || !it.loc.startsWith("doc:") || tree == null || tree.isEmpty()
            || !it.name.toLowerCase(java.util.Locale.ROOT).endsWith(".cue"))
            return out;
        for (String loc : it.parts)
            if (loc.startsWith("doc:"))
                out.add(DocumentsContract.buildDocumentUriUsingTree(Uri.parse(tree), loc.substring(4)).toString());
        return out;
    }

    /** The primary action of a library game whose files are not here (State.LIBRARY). */
    private void libraryAction(LibraryScreen.Entry e) {
        if (e == null)
            return;
        switch (e.chip) {
        case PREPARING:
            go(Screen.PREPARE);
            break;
        case QUEUED:
            dequeuePrepare(e.tid);
            break;
        case NOT_IMPORTED:
        case NEEDS_COPYING:
            importEntry(e);
            break;
        case NO_FILE_ACCESS:
            onChooseLibraryFolder();
            break;
        default:
            setNote(getString(R.string.LIB_SOURCE_GONE_NOTE, e.title()), NOTE_WARN);
            break;
        }
    }

    /** The primary action's label for e (the bottom screen's big button); null when there is none. */
    private String primaryLabel(LibraryScreen.Entry e) {
        switch (e.chip) {
        case READY:
        case RUNNING:
            return getString(R.string.PLAY);
        case NEEDS_PREPARING:
        case PACK_INVALID:
            return getString(R.string.PREPARE);
        case PREPARE_FAILED:
            return getString(R.string.PREPARE_SEE_WHY);
        case PREPARING:
            return getString(R.string.PREPARING);
        case QUEUED:
            return LibraryScreen.chipText(this, e);
        case NOT_IMPORTED:
            return getString(R.string.LIB_IMPORT);
        case NEEDS_COPYING:
            return getString(R.string.LIB_COPY);
        case NO_FILE_ACCESS:
            return getString(R.string.LIB_CHOOSE_FOLDER);
        default:
            return null;
        }
    }

    /** « Queued (2) »: the place of tid in the Prepare queue. */
    private String queuedLabel(String tid) {
        int pos = tid == null ? 0 : PrepareProtocol.readQueue(PrepareProtocol.queueFile(getFilesDir())).position(tid);
        return pos > 0 ? getString(R.string.LIB_QUEUED_N, pos) : getString(R.string.CHIP_QUEUED);
    }

    /** A on « Queued (n) »: the title leaves the Prepare queue (PrepareService marks it cancelled). */
    private void dequeuePrepare(String tid) {
        if (tid == null)
            return;
        try {
            startService(PrepareService.cancelIntent(this, tid));
        } catch (RuntimeException e) {
            Log.w(TAG, "launcher: cannot reach the Prepare service", e);
        }
        Log.i(TAG, "launcher: prepare " + tid + " taken out of the queue");
        setNote(getString(R.string.LIB_DEQUEUED), NOTE_INFO);
        Screen at = screen;
        poller.postDelayed(() -> {
            if (!isFinishing() && screen == at && (at == Screen.HOME || at == Screen.LIBRARY))
                refreshKeepingFocus();
        }, 600);
    }

    /**
     * Import a library game: its disc image extracted (a file, or a document of the library folder), or its folder
     * copied (seen through the Storage Access Framework only), into games/<TID>; a needed Prepare follows.
     */
    private void importEntry(LibraryScreen.Entry e) {
        LibraryIndex.Item it = e.active;
        Library.Folder folder = Library.folderOf(Library.folders(this), it);
        String tree = folder == null ? null : folder.tree;
        if (it == null) {
            setNote(getString(R.string.LIB_SOURCE_GONE_NOTE, e.title()), NOTE_WARN);
            return;
        }
        File f = LibraryIndex.locFile(it.loc);
        String docId = it.loc.startsWith("doc:") ? it.loc.substring(4) : null;
        if (f == null && (docId == null || tree == null)) {
            setNote(getString(R.string.LIB_SOURCE_GONE_NOTE, e.title()), NOTE_WARN);
            return;
        }
        Log.i(TAG, "launcher: library: import " + e.tid + " from " + it.loc);
        importTid = e.tid;
        importFromLibrary = true;
        if (LibraryIndex.KIND_FOLDER.equals(it.kind)) {
            if (docId == null) {   // a folder read as files is played in place: nothing to copy
                importFromLibrary = false;
                return;
            }
            Uri t = Uri.parse(tree);
            runWorker(R.string.COPYING, () -> copyTree(t, docId, false));
            return;
        }
        // XBA-126: the image is read in place where it is (a path, or its documents under the library folder's
        // tree, whose permission persists), split parts included; extracted only with RECOMP_DISC_IMAGE=0.
        List<String> locs = it.parts.isEmpty() ? Collections.singletonList(it.loc) : it.parts;
        List<Uri> uris = new ArrayList<>();
        List<String> names = new ArrayList<>();
        for (String loc : locs) {
            File lf = LibraryIndex.locFile(loc);
            names.add(lf != null ? lf.getName() : loc.substring(loc.lastIndexOf('/') + 1));
            if (lf == null && loc.startsWith("doc:") && tree != null)
                uris.add(DocumentsContract.buildDocumentUriUsingTree(Uri.parse(tree), loc.substring(4)));
        }
        runWorker(R.string.CHECKING, () -> importImage(uris, names, f, f == null));
    }

    /**
     * A library folder was picked: added to the library folders (path with All files access, else the tree; the same
     * folder again updates its entry), then every folder is scanned.
     */
    private void onLibraryTreePicked(Uri tree) {
        try {
            getContentResolver().takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException ignored) {
        }
        String path = Environment.isExternalStorageManager() ? GameStore.treeUriToPath(tree) : null;
        if (path != null && !new File(path).isDirectory())
            path = null;
        Library.addFolder(this, tree.toString(), path);
        Log.i(TAG, "launcher: library: folder " + tree + (path != null ? " = " + path : " (Storage Access Framework)")
                   + ", " + Library.folders(this).size() + " folders");
        pickLibrary = false;
        focusTid = null;
        history.clear();
        show(Screen.LIBRARY);   // a path is scanned as the library opens
        if (path == null)
            startScan();
    }

    /**
     * Takes a folder out of the library: its games leave the grid (their records, packs and saves stay, as for a
     * game gone from a folder), its read permission is released. Nothing in the folder is touched.
     */
    private void removeLibraryFolder(Library.Folder f) {
        List<Library.Folder> before = Library.folders(this);
        boolean first = !before.isEmpty() && before.get(0).tree.equals(f.tree);
        if (scanning())
            scanCancel = true;   // its result would list the folder again
        Library.removeFolder(this, f.tree);
        index.dropFolder(f.tree, first);
        try {
            LibraryIndex.write(Library.indexFile(getFilesDir()), index);
        } catch (IOException e) {
            Log.w(TAG, "launcher: library: cannot write index.json", e);
        }
        setIndex(index);
        try {
            getContentResolver().releasePersistableUriPermission(Uri.parse(f.tree),
                                                                 Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (RuntimeException ignored) {   // (not held: a path, or already gone)
        }
        Log.i(TAG, "launcher: library: folder " + f.tree + " removed, " + Library.folders(this).size() + " left");
        focusTid = null;
        refreshKeepingFocus();
        setNote(getString(R.string.LIB_FOLDER_REMOVED, f.name()), NOTE_INFO);
    }

    /** Y: scan the library folders again (or choose one). */
    private void rescanLibrary() {
        if (Library.folders(this).isEmpty()) {
            onChooseLibraryFolder();
            return;
        }
        startScan();
        setNote(getString(R.string.LIB_SCANNING_NOTE), NOTE_INFO);
    }

    /** The scan on a worker thread; the screen is built again when it ends. */
    private void startScan() {
        if (scanner != null && scanner.isAlive())
            return;
        List<LibraryScanner.Root> roots = LibraryScanner.folderRoots(this);
        if (roots.isEmpty())
            return;
        scanCancel = false;
        scanFound = 0;
        android.content.Context app = getApplicationContext();
        LibraryIndex before = index;
        scanner = new Thread(() -> {
            LibraryScanner.Result r = null;
            try {
                r = LibraryScanner.scanLibrary(app, roots, item -> {
                    scanFound++;
                    postScanProgress();
                }, () -> scanCancel);
            } catch (RuntimeException | LinkageError e) {
                Log.w(TAG, "launcher: library: scan failed", e);
            }
            LibraryScanner.Result res = r;
            runOnUiThread(() -> onScanDone(res, before));
        }, "xba-scan");
        scanner.start();
        paintScanCaption();
    }

    Thread scannerForTest() {
        return scanner;
    }

    Thread workerForTest() {
        return worker;
    }

    private volatile long scanPaintedAt;

    private void postScanProgress() {
        long now = SystemClock.uptimeMillis();
        if (now - scanPaintedAt < 250)
            return;
        scanPaintedAt = now;
        runOnUiThread(this::paintScanCaption);
    }

    private boolean scanning() {
        return scanner != null && scanner.isAlive();
    }

    private Thread coverThread;

    /**
     * Cover art for the GBA, PS1 and N64 games that have none (CoverArt: libretro-thumbnails, when « Download cover
     * art » is on), one game after the other on a worker; each one found shows at once.
     */
    private void fetchCovers(LibraryIndex idx) {
        if (!CoverArt.downloadOn(this) || !VkDriverIndex.networkAllowed(this)
            || (coverThread != null && coverThread.isAlive()))
            return;
        File files = getFilesDir();
        long now = System.currentTimeMillis();
        List<String[]> todo = new ArrayList<>();
        for (java.util.Map.Entry<String, List<LibraryIndex.Item>> g : idx.games().entrySet()) {
            LibraryIndex.Item i = g.getValue().get(0);
            if (i.otherConsole() && CoverArt.wanted(files, g.getKey(), now))
                todo.add(new String[] { g.getKey(), i.name });
        }
        if (todo.isEmpty())
            return;
        Log.i(TAG, "launcher: cover art: looking for " + todo.size() + " game(s)");
        coverThread = new Thread(() -> {
            VkDriverIndex.Http http = new VkDriverIndex.UrlHttp();
            for (String[] t : todo) {
                if (!CoverArt.fetch(http, files, t[0], t[1]))
                    continue;
                runOnUiThread(() -> {
                    libArt.remove(t[0]);
                    if (t[0].equals(artTid))
                        artTid = null;   // Home's image is read again
                    if (isFinishing() || isDestroyed())
                        return;
                    if (screen == Screen.LIBRARY || screen == Screen.HOME)
                        refreshKeepingFocus();
                });
            }
        }, "cover-art");
        coverThread.start();
    }

    private void onScanDone(LibraryScanner.Result r, LibraryIndex before) {
        if (isFinishing() || isDestroyed())
            return;
        if (r != null && !r.cancelled) {
            setIndex(r.index);
            // A title that left the folder and has no files here: its cached art and theme go (docs « Game
            // library »). Not when the folder could not be read at all (root unknown): it may only be unmounted.
            if (!r.index.root.isEmpty()) {
                for (String tid : before.games().keySet()) {
                    Library.Record rr = games.get(tid);
                    if (!indexGames.containsKey(tid) && (rr == null || !rr.hasData())) {
                        GameTheme.clear(this, tid);
                        libArt.remove(tid);
                    }
                }
            }
            libArt.values().removeIf(java.util.Objects::isNull);   // art the scan made shows now
            fetchCovers(r.index);
            if (libEntries.isEmpty())
                focusTid = null;   // the first games found: the focus goes to the selected one, not the folder tile
        }
        if (screen == Screen.LIBRARY)
            refreshKeepingFocus();
        else if (screen == Screen.HOME && state() == State.LIBRARY)
            refreshKeepingFocus();
    }

    /** Under the grid: the scan's progress, an empty library's hint, a scan cut short, the « Other files » line. */
    private void paintScanCaption() {
        if (libCaption == null)
            return;
        List<String> parts = new ArrayList<>();
        if (scanning())
            parts.add(getString(R.string.LIB_SCANNING, scanFound));
        else if (Library.folders(this).isEmpty())
            parts.add(getString(R.string.LIB_NO_FOLDER));
        else if (libEntries.isEmpty())
            parts.add(getString(R.string.LIB_EMPTY));
        if (index.truncated)
            parts.add(getString(R.string.LIB_TRUNCATED));
        List<LibraryIndex.Item> others = index.others();
        if (!others.isEmpty()) {
            List<String> names = new ArrayList<>();
            for (int i = 0; i < others.size() && i < 3; i++)
                names.add(others.get(i).name);
            parts.add(getString(R.string.LIB_OTHERS, others.size(), TextUtils.join(", ", names)
                                                                   + (others.size() > 3 ? ", …" : "")));
        }
        libCaption.setText(TextUtils.join("\n", parts));
    }

    /**
     * The library: a full-width grid of games (five columns), the focused game's state and play time as one context
     * line at the bottom left, and the rest (Rescan, Storage, the library folder, Settings, About) in the Options
     * sheet on Y. An empty library shows why and a « Library folder » button. Stock colours; the backdrop's glow
     * follows the focused card (its cached theme).
     */
    private void buildLibrary() {
        screenTitle.setText(R.string.LIB_TITLE);
        libEntries = LibraryScreen.entries(getFilesDir(), games, index, libEnv());
        LibraryView.State view = libView();
        // Games of several consoles: each card says its console (an Xbox-only library looks as before), and the top
        // bar's tabs filter them (XBA-206: All, Xbox, GBA, PS1, N64; L1 / R1).
        libConsoles = LibraryView.consoles(libEntries);
        libBadges = libConsoles.size() > 1;
        if (!libBadges || !libConsoles.contains(view.console))
            view.console = null;   // (not saved: the console's games may come back)
        if (libBadges) {
            String[] names = new String[libConsoles.size() + 1];
            names[0] = getString(R.string.LIB_ALL);
            for (int i = 0; i < libConsoles.size(); i++)
                names[i + 1] = libConsoles.get(i).badge();
            int active = view.console == null ? 0 : libConsoles.indexOf(view.console) + 1;
            topTabs.addView(new Widgets.Tabs(this, names, active, i -> stepConsole(i - active)), lp(WRAP, MATCH));
            l1Action = () -> stepConsole(-1);
            r1Action = () -> stepConsole(1);
        }
        Library.Record sel = games.selected();
        String want = focusTid != null ? focusTid : sel != null ? sel.titleId : null;

        libCaption = Theme.text(this, Theme.T_BODY, Theme.REGULAR, Theme.TEXT_DIM, "");
        libCaption.setLineSpacing(0, 1.3f);
        paintScanCaption();
        if (libEntries.isEmpty()) {
            // Nothing to show yet: why, and the one thing to do.
            LinearLayout empty = column();
            empty.addView(libCaption, lp(MATCH, WRAP));
            LinearLayout buttons = rowLayout();
            LinearLayout.LayoutParams bl = lp(MATCH, Theme.BUTTON_H);
            bl.topMargin = Theme.S5;
            empty.addView(buttons, bl);
            Widgets.PillButton folder = pill(buttons, Icon.FOLDER, getString(R.string.LIB_FOLDER_TILE), true,
                                             this::onChooseLibraryFolder);
            defaultFocus = folder;
            body.addView(empty, new FrameLayout.LayoutParams(MATCH, WRAP));
            focusTid = "";
            if (backdrop != null)
                backdrop.setGlows(Theme.GLOW, Theme.GLOW2);
            if (companion != null)
                companion.library("", getString(Library.folders(this).isEmpty() ? R.string.LIB_FOLDER_NONE
                                                                                : R.string.LIB_FOLDER_TILE),
                                  Theme.TEXT_DIM, null, null);
        } else {
            // XBA-206: the Search and Sort pills over the grid, then « Recently played » and the games.
            ScrollView scroll = LibraryScreen.scroll(this);
            libScroll = scroll;
            LinearLayout col = (LinearLayout)scroll.getChildAt(0);
            Widgets.PillRow bar = new Widgets.PillRow(this);
            libSearchPill = pill(bar, Icon.SEARCH, searchLabel(), false, this::openSearch);
            libSortPill = pill(bar, Icon.SORT, sortLabel(), false, this::stepSort);
            LinearLayout.LayoutParams bp = lp(MATCH, Theme.BUTTON_H);
            bp.bottomMargin = Theme.S3;
            col.addView(bar, bp);
            libContent = column();
            libContent.setClipChildren(false);
            col.addView(libContent, lp(MATCH, WRAP));
            LinearLayout.LayoutParams cp = lp(MATCH, WRAP);
            cp.topMargin = Theme.S2;
            col.addView(libCaption, cp);
            body.addView(scroll, new FrameLayout.LayoutParams(MATCH, MATCH));
            GameCard focusCard = fillLibrary(want);
            defaultFocus = focusCard != null ? focusCard : libSearchPill;
            LibraryScreen.Entry fe = focusCard == null ? null : cardEntry(focusCard);
            if (fe != null)
                libFocus(fe);
        }
        focusMemory.remove(Screen.LIBRARY);   // the focus follows the title, not a place in the grid

        startAction = () -> {
            LibraryScreen.Entry e = focusedEntry();
            if (e != null)
                libraryPrimary(e);
            else
                onChooseLibraryFolder();
        };
        yAction = this::openOptions;
        addHint("A", R.string.LIB_HINT_OPEN, this::clickFocused);
        if (!libEntries.isEmpty()) {   // Start (the primary action) is not hinted, as on Home
            xAction = this::toggleFocusedFavourite;
            addHint("X", R.string.LIB_HINT_FAV, xAction);
        } else {
            addHint("START", R.string.LIB_HINT_PRIMARY, startAction);
        }
        addHint("Y", R.string.HINT_OPTIONS, this::openOptions);
        if (gameRunning())
            setNote(getString(R.string.LIB_RUNNING_NOTE, runningTitle()), NOTE_INFO);
    }

    // ---- The library's view (XBA-206): sort, console filter, search, favourites, « Recently played » --------------

    private LibraryView.State libView;
    /** The grid's entries as shown (filtered, sorted, favourites first); libEntries keeps them all. */
    private List<LibraryScreen.Entry> libShown = new ArrayList<>();
    private final List<GameCard> libRecentCards = new ArrayList<>();
    private final java.util.Map<GameCard, LibraryScreen.Entry> libCardEntries = new java.util.HashMap<>();
    private List<Console> libConsoles = new ArrayList<>();
    private boolean libBadges;
    /** Under the Search and Sort pills: « Recently played », then the grid (rebuilt as the search is typed). */
    private LinearLayout libContent;
    private ScrollView libScroll;
    private Widgets.PillButton libSearchPill, libSortPill;
    private android.widget.EditText libSearch;

    private LibraryView.State libView() {
        if (libView == null)
            libView = LibraryView.load(getFilesDir());
        return libView;
    }

    LibraryView.State libraryViewForTest() {
        return libView();
    }

    List<GameCard> libraryRecentCardsForTest() {
        return libRecentCards;
    }

    android.widget.EditText librarySearchForTest() {
        return libSearch;
    }

    Widgets.PillButton librarySortPillForTest() {
        return libSortPill;
    }

    Widgets.PillButton librarySearchPillForTest() {
        return libSearchPill;
    }

    private void saveLibView() {
        if (!LibraryView.save(getFilesDir(), libView()))
            Log.w(TAG, "launcher: cannot write " + LibraryView.FILE);
    }

    private LibraryScreen.Entry cardEntry(GameCard c) {
        return libCardEntries.get(c);
    }

    /** A card of the library for e (the grid's or the « Recently played » row's). */
    private GameCard libCard(LibraryScreen.Entry e) {
        GameCard c = new GameCard(this);
        c.setGame(LibraryScreen.art(getFilesDir(), libArt, e), e.tid, e.title(), LibraryScreen.chipText(this, e),
                  LibraryScreen.chipColor(e));
        c.setChipShown(LibraryScreen.chipShown(e));
        if (libBadges)
            c.setBadge(e.console().badge());
        c.setFavourite(libView().favourite(e.tid));
        c.setDim(e.chip == GameState.Chip.SOURCE_GONE || e.chip == GameState.Chip.DATA_MISSING
                 || e.chip == GameState.Chip.NO_FILE_ACCESS);
        c.setOnClickListener(v -> {
            feedback.play(UiFeedback.Cue.SELECT, v);
            openGame(e);
        });
        onFocus(c, () -> {
            libFocus(e);
            // The first row of cards: back to the top, the Search and Sort pills in sight (not under the title).
            boolean top = libRecentCards.contains(c) || libRecentCards.isEmpty() && libCards.indexOf(c) >= 0
                          && libCards.indexOf(c) < LibraryScreen.COLUMNS;
            if (top && libScroll != null)
                libScroll.post(() -> libScroll.scrollTo(0, 0));
        });
        libCardEntries.put(c, e);
        return focusable(c);
    }

    /**
     * (Re)builds what is under the pills: « Recently played » (the last games played, any console; not while a console
     * or a search narrows the grid, nor for a library of one row), then the games as the view shows them. The card of
     * want (a grid card first), else the first one; null when nothing shows.
     */
    private GameCard fillLibrary(String want) {
        focusables.removeAll(libCards);
        focusables.removeAll(libRecentCards);
        for (GameCard c : libCards)
            libCardEntries.remove(c);
        for (GameCard c : libRecentCards)
            libCardEntries.remove(c);
        libCards.clear();
        libRecentCards.clear();
        libContent.removeAllViews();
        LibraryView.State view = libView();
        libShown = LibraryView.apply(libEntries, view);
        List<LibraryScreen.Entry> recent = view.narrowed() || libEntries.size() <= LibraryScreen.COLUMNS
                                           ? Collections.emptyList()
                                           : LibraryView.recent(libEntries, LibraryView.RECENT_MAX);
        GameCard focusCard = null, recentWant = null;
        if (!recent.isEmpty()) {
            libContent.addView(Theme.label(this, getString(R.string.LIB_RECENT)), lp(MATCH, WRAP));
            for (LibraryScreen.Entry e : recent) {
                GameCard c = libCard(e);
                libRecentCards.add(c);
                if (e.tid.equals(want) && recentWant == null)
                    recentWant = c;
            }
            LibraryScreen.addRows(this, libContent, libRecentCards);
            LinearLayout.LayoutParams p = lp(MATCH, WRAP);
            p.topMargin = Theme.S3;
            libContent.addView(Theme.label(this, getString(R.string.LIB_ALL_GAMES)), p);
        }
        for (LibraryScreen.Entry e : libShown) {
            GameCard c = libCard(e);
            libCards.add(c);
            if (e.tid.equals(want))
                focusCard = c;
        }
        if (libShown.isEmpty()) {
            String q = view.query.trim();
            TextView none = Theme.body(this, q.isEmpty() ? getString(R.string.LIB_NO_GAME_CONSOLE)
                                                         : getString(R.string.LIB_NO_MATCH, q));
            LinearLayout.LayoutParams p = lp(MATCH, WRAP);
            p.topMargin = Theme.S3;
            libContent.addView(none, p);
        } else {
            LibraryScreen.addRows(this, libContent, libCards);
        }
        if (focusCard == null)
            focusCard = recentWant;
        if (focusCard == null && !libCards.isEmpty())
            focusCard = libCards.get(0);
        if (focusCard == null && !libRecentCards.isEmpty())
            focusCard = libRecentCards.get(0);
        return focusCard;
    }

    private String searchLabel() {
        String q = libView().query.trim();
        return q.isEmpty() ? getString(R.string.LIB_SEARCH) : getString(R.string.LIB_SEARCH_Q, q);
    }

    private String sortLabel() {
        return getString(R.string.LIB_SORT, getString(libView().sort.label));
    }

    /** A on Sort: the next order (name, last played, console, most played), kept; the focus stays on the pill. */
    private void stepSort() {
        if (screen != Screen.LIBRARY || libContent == null)
            return;
        LibraryView.State view = libView();
        view.sort = view.sort.next();
        saveLibView();
        Log.i(TAG, "launcher: library: sorted by " + view.sort.name().toLowerCase(Locale.ROOT));
        fillLibrary(focusTid);
        libSortPill.set(Icon.SORT, sortLabel(), false);
        libSortPill.requestLayout();
        feedback.play(UiFeedback.Cue.MOVE, libSortPill);
    }

    /** L1 / R1 (or a tab): the previous or next console of the filter, « All » first; kept. */
    private void stepConsole(int dir) {
        if (screen != Screen.LIBRARY || !libBadges || dir == 0)
            return;
        LibraryView.State view = libView();
        int n = libConsoles.size() + 1;
        int i = view.console == null ? 0 : libConsoles.indexOf(view.console) + 1;
        i = ((i + dir) % n + n) % n;
        view.console = i == 0 ? null : libConsoles.get(i - 1);
        saveLibView();
        Log.i(TAG, "launcher: library: console " + (view.console == null ? "all" : view.console.id));
        feedback.play(UiFeedback.Cue.MOVE, host);
        refreshKeepingFocus();
    }

    /** The search field's width, in design pixels. */
    static final int SEARCH_W = 720;

    /**
     * A on Search: a text field in its place with the on-screen keyboard (not full screen: the grid stays in sight),
     * the grid following each letter; the keyboard's Search (or Enter) goes to the games. B clears the search.
     */
    private void openSearch() {
        if (screen != Screen.LIBRARY || libContent == null)
            return;
        if (libSearch == null) {
            android.widget.EditText t = new android.widget.EditText(this);
            t.setSingleLine(true);
            t.setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX, Theme.T_ROW);
            t.setTypeface(Theme.REGULAR);
            t.setTextColor(Theme.TEXT_BRIGHT);
            t.setHintTextColor(Theme.TEXT_DIM);
            t.setHint(R.string.LIB_SEARCH_HINT);
            t.setPadding(Theme.S4, 0, Theme.S4, 0);
            t.setGravity(Gravity.CENTER_VERTICAL);
            android.graphics.drawable.GradientDrawable bg = new android.graphics.drawable.GradientDrawable();
            bg.setColor(Theme.alpha(Theme.PANEL_HI, 0.9f));
            bg.setCornerRadius(Theme.BUTTON_H / 2f);
            bg.setStroke(3, Theme.ACCENT);
            t.setBackground(bg);
            t.setInputType(android.text.InputType.TYPE_CLASS_TEXT
                           | android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
            t.setImeOptions(android.view.inputmethod.EditorInfo.IME_ACTION_SEARCH
                            | android.view.inputmethod.EditorInfo.IME_FLAG_NO_EXTRACT_UI
                            | android.view.inputmethod.EditorInfo.IME_FLAG_NO_FULLSCREEN);
            t.setText(libView().query);
            t.setSelection(t.getText().length());
            t.addTextChangedListener(new android.text.TextWatcher() {
                @Override
                public void beforeTextChanged(CharSequence c, int a, int b, int d) {}

                @Override
                public void onTextChanged(CharSequence c, int a, int b, int d) {}

                @Override
                public void afterTextChanged(android.text.Editable e) {
                    onSearchTyped(e.toString());
                }
            });
            t.setOnEditorActionListener((v, action, ev) -> {
                searchDone();
                return true;
            });
            LinearLayout bar = (LinearLayout)libSearchPill.getParent();
            bar.addView(t, bar.indexOfChild(libSearchPill), lp(SEARCH_W, Theme.BUTTON_H));
            libSearchPill.setVisibility(View.GONE);
            focusables.remove(libSearchPill);
            focusables.add(0, t);
            libSearch = t;
        }
        libSearch.requestFocus();
        android.view.inputmethod.InputMethodManager imm = getSystemService(
                android.view.inputmethod.InputMethodManager.class);
        if (imm != null)
            imm.showSoftInput(libSearch, android.view.inputmethod.InputMethodManager.SHOW_IMPLICIT);
        Log.i(TAG, "launcher: library: search opened");
    }

    private void onSearchTyped(String q) {
        if (screen != Screen.LIBRARY || libContent == null)
            return;
        libView().query = q;
        fillLibrary(focusTid);
        libSearchPill.set(Icon.SEARCH, searchLabel(), false);
    }

    /** The keyboard's Search: the keyboard goes, the focus to the first game found. */
    private void searchDone() {
        hideKeyboard();
        if (!libCards.isEmpty())
            libCards.get(0).requestFocus();
    }

    private void hideKeyboard() {
        android.view.inputmethod.InputMethodManager imm = getSystemService(
                android.view.inputmethod.InputMethodManager.class);
        if (imm != null && host != null)
            imm.hideSoftInputFromWindow(host.getWindowToken(), 0);
    }

    /** B with a search typed (or its field open): the search goes, the whole library is back. */
    private boolean clearSearch() {
        if (screen != Screen.LIBRARY || (libSearch == null && libView().query.isEmpty()))
            return false;
        hideKeyboard();
        libView().query = "";
        Log.i(TAG, "launcher: library: search cleared");
        refreshKeepingFocus();
        return true;
    }

    /** X on a library card: that game in or out of the favourites (first in the grid, a star on its card). */
    private void toggleFocusedFavourite() {
        if (screen != Screen.LIBRARY || !(focused() instanceof GameCard))
            return;
        LibraryScreen.Entry e = cardEntry((GameCard)focused());
        if (e == null)
            return;
        boolean on = toggleFavourite(e);
        GameCard c = fillLibrary(e.tid);
        if (c != null) {
            c.requestFocus();
            libFocus(e);
        }
        feedback.play(UiFeedback.Cue.SELECT, host);
        setNote(getString(on ? R.string.LIB_FAV_ADDED : R.string.LIB_FAV_REMOVED, e.title()), NOTE_OK);
    }

    private boolean toggleFavourite(LibraryScreen.Entry e) {
        boolean on = libView().toggleFavourite(e.tid);
        saveLibView();
        Log.i(TAG, "launcher: library: " + e.tid + (on ? " is a favourite" : " is no longer a favourite"));
        return on;
    }

    /** X on a game's Home (XBA-206): « Add to favourites » or « Remove from favourites ». */
    private void favouriteHint(LibraryScreen.Entry e) {
        if (e == null || !libraryOn())
            return;
        xAction = () -> {
            boolean on = toggleFavourite(e);
            refreshKeepingFocus();
            setNote(getString(on ? R.string.LIB_FAV_ADDED : R.string.LIB_FAV_REMOVED, e.title()), NOTE_OK);
        };
        addHint("X", libView().favourite(e.tid) ? R.string.LIB_FAV_REMOVE : R.string.LIB_FAV_ADD, xAction);
    }

    /** Y on the library: the Options sheet (Rescan, Storage, Library folders, Settings, About). */
    private void openOptions() {
        if (screen != Screen.LIBRARY)
            return;
        openSheet(R.string.HINT_OPTIONS);
        sheetRow(getString(R.string.LIB_RESCAN), this::rescanLibrary);
        sheetRow(getString(R.string.LIB_STORAGE), () -> go(Screen.STORAGE))
                .setValue(getString(R.string.LIB_ST_FREE, size(freeSpace())));
        List<Library.Folder> folders = Library.folders(this);
        sheetRow(getString(R.string.LIB_FOLDERS), this::openFoldersSheet)
                .setValue(folders.isEmpty() ? getString(R.string.LIB_FOLDER_NONE) : foldersValue(folders));
        if (Toolchain.available(this))   // XBA-130: prepares and boots every game, a report
            sheetRow(getString(R.string.CHECK_LIBRARY), () -> startActivity(CheckActivity.intent(this)));
        sheetRow(getString(R.string.SETTINGS), () -> go(Screen.SETTINGS));
        sheetRow(getString(R.string.ABOUT), () -> go(Screen.ABOUT));
        sheetReady();
    }

    /** « xbox », « Chosen folder » or « 4 folders »: the library folders in one value. */
    private String foldersValue(List<Library.Folder> folders) {
        if (folders.size() == 1)
            return folders.get(0).path != null ? folders.get(0).name() : getString(R.string.LIB_FOLDER_SAF);
        return getString(R.string.LIB_FOLDERS_N, folders.size());
    }

    /**
     * Options › Library folders: one row per folder (its name; its path, or « Chosen folder » through the Storage
     * Access Framework), A on one asks to take it out of the library; « Add a folder » last.
     */
    private void openFoldersSheet() {
        if (screen != Screen.LIBRARY)
            return;
        openSheet(R.string.LIB_FOLDERS);
        for (Library.Folder f : Library.folders(this))
            sheetRow(f.name(), () -> confirmRemoveFolder(f)).setValue(folderWhere(f));
        sheetRow(getString(R.string.LIB_FOLDER_ADD), this::onChooseLibraryFolder);
        sheetReady();
    }

    /**
     * Where a library folder is, short enough for the sheet: « Internal storage », « SD card » (read as files), the
     * folder above it otherwise, « Chosen folder » through the Storage Access Framework.
     */
    private String folderWhere(Library.Folder f) {
        if (f.path == null)
            return getString(R.string.LIB_FOLDER_SAF);
        if (f.path.startsWith("/storage/emulated/"))
            return getString(R.string.LIB_FOLDER_INTERNAL);
        if (f.path.startsWith("/storage/"))
            return getString(R.string.LIB_FOLDER_SD);
        File parent = new File(f.path).getParentFile();
        return parent == null ? f.path : parent.getName();
    }

    /** A on a library folder: « Keep » (focused first, as B) or « Remove from the library ». */
    private void confirmRemoveFolder(Library.Folder f) {
        if (screen != Screen.LIBRARY)
            return;
        openSheet(f.name());
        sheetRow(getString(R.string.LIB_FOLDER_KEEP), this::openFoldersSheet);
        sheetRow(getString(R.string.LIB_FOLDER_REMOVE), () -> removeLibraryFolder(f))
                .setValue(getString(R.string.LIB_FOLDER_REMOVE_SUB));
        sheetReady();
    }

    /** A card got the focus: the context line, the backdrop's glow, the bottom screen. */
    private void libFocus(LibraryScreen.Entry e) {
        focusTid = e.tid;
        int[] g = LibraryScreen.glows(this, e);
        if (backdrop != null)
            backdrop.setGlows(g[0], g[1]);
        setContext(libContext(e), LibraryScreen.chipColor(e));
        if (companion != null)
            companion.library(e.title(), LibraryScreen.chipText(this, e), LibraryScreen.chipColor(e),
                              LibraryScreen.art(getFilesDir(), libArt, e), primaryLabel(e));
    }

    /** « Ready · Yesterday · 12 h 40 min played »: the focused game's state, and when it was played. */
    private String libContext(LibraryScreen.Entry e) {
        String chip = LibraryScreen.chipText(this, e);
        if (e.record == null)
            return chip;
        return chip + " · " + homeMeta(e.record.lastPlayed, e.record.playMs);
    }

    /** One line on what a library game's state means for the player. */
    private String libLead(LibraryScreen.Entry e) {
        switch (e.chip) {
        case RUNNING:
            return getString(R.string.PLAY_DETAIL_RUNNING);
        case READY:
            return getString(R.string.HOME_LEAD_READY);
        case NEEDS_PREPARING:
            return getString(R.string.HOME_LEAD_PREPARE);
        case PREPARING:
            return getString(R.string.HOME_LEAD_PREPARING);
        case PREPARE_FAILED:
            return getString(R.string.HOME_LEAD_FAILED);
        case PACK_INVALID:
            return getString(R.string.LIB_LEAD_PACK_INVALID);
        case QUEUED:
            return getString(R.string.PREPARE_QUEUED_NOTE);
        case IMPORTING:
            return getString(R.string.LIB_LEAD_IMPORTING);
        case NOT_IMPORTED:
            return getString(R.string.LIB_LEAD_NOT_IMPORTED);
        case NEEDS_COPYING:
            return getString(R.string.LIB_LEAD_NEEDS_COPYING);
        case NO_FILE_ACCESS:
            return getString(R.string.LIB_LEAD_NO_ACCESS);
        case SOURCE_GONE:
            return getString(R.string.LIB_LEAD_SOURCE_GONE);
        default:
            return getString(R.string.LIB_LEAD_DATA_MISSING);
        }
    }

    /**
     * Home of a library game whose files are not here: its art (the scan's cached image), title and state, the
     * primary action (Import, Copy, Preparing, Queued (n), Choose the folder again) and, before an import, « Prepare
     * now » from the cached default.xbe, then the quiet pills.
     */
    private void buildHomeNotHere() {
        LibraryScreen.Entry e = currentEntry();
        homeChip = e.chip;
        String title = e.title();
        String chipText = LibraryScreen.chipText(this, e);
        int chipColor = LibraryScreen.chipColor(e);

        LinearLayout info = hero(title, false);
        heroLine(info, chipText, chipColor);
        heroLine(info, libLead(e), 0);

        GameState.Facts f = e.facts;
        Prep p = GameState.prep(f.toolchain, f.pack == Pack.Check.VALID && f.packMatches, f.prepare, f.prepareFresh,
                                f.prepareProcess);
        LinearLayout buttons = heroButtons(info);
        Widgets.PillButton play;
        switch (e.chip) {
        case NOT_IMPORTED:
            play = pill(buttons, null, getString(R.string.LIB_IMPORT), true, this::onPlay);
            break;
        case NEEDS_COPYING:
            play = pill(buttons, null, getString(R.string.LIB_COPY), true, this::onPlay);
            break;
        case NO_FILE_ACCESS:
            play = pill(buttons, null, getString(R.string.LIB_CHOOSE_FOLDER), true, this::onPlay);
            break;
        case PREPARING:
            play = pill(buttons, null, getString(R.string.PREPARING), true, this::onPlay);
            break;
        case QUEUED:
            play = pill(buttons, null, queuedLabel(e.tid), true, this::onPlay);
            break;
        default:
            play = pill(buttons, null, getString(R.string.LIB_FOLDER_TILE), true, this::onChooseLibraryFolder);
            break;
        }
        homePrimary = play;
        // « Prepare now »: tools.prepare needs only default.xbe, so a game can be prepared (or queued) before its
        // gigabytes are imported.
        boolean canPrepare = f.toolchain && e.realTid() && cachedXbeDir() != null
                             && (e.chip == GameState.Chip.NOT_IMPORTED || e.chip == GameState.Chip.NEEDS_COPYING);
        if (canPrepare && (p == Prep.NEEDED || p == Prep.CANCELLED || p == Prep.INTERRUPTED || p == Prep.FAILED)) {
            boolean failed = p == Prep.FAILED;
            pill(buttons, null, getString(failed ? R.string.PREPARE_SEE_WHY : R.string.LIB_PREPARE_NOW), false,
                 failed ? () -> go(Screen.PREPARE) : this::startPrepare);
        }
        homePills(buttons);

        if (companion != null) {
            companion.art(art);
            companion.home(title, chipText, chipColor, primaryLabel(e) != null, play.title);
        }
        defaultFocus = play;
        startAction = this::onPlay;
        addHint("A", R.string.HINT_SELECT, this::clickFocused);
        addHint("B", history.isEmpty() ? R.string.LIB_TITLE : R.string.HINT_BACK, this::back);
        favouriteHint(e);   // XBA-206
    }

    /** The library's entries changed state (a Prepare moved on, the queue, a game ended): true. */
    private boolean libraryChanged() {
        List<LibraryScreen.Entry> now = LibraryScreen.entries(getFilesDir(), games, index, libEnv());
        if (now.size() != libEntries.size())
            return true;
        for (int i = 0; i < now.size(); i++) {
            LibraryScreen.Entry a = now.get(i), b = libEntries.get(i);
            if (!a.tid.equals(b.tid) || a.chip != b.chip || a.prepared != b.prepared || a.queuePos != b.queuePos)
                return true;
        }
        return false;
    }

    // ---- CONTROLS ----------------------------------------------------------------------------------------------

    /** The mapping the Controls screen edits (RECOMP_PAD_MAP in recomp.cfg, read when the screen is built). */
    private PadMapping padMap = new PadMapping();
    /**
     * XBA-188: the title whose Controls these are (null: no game set up, only the mapping of every game), and whether
     * padMap is its own (RECOMP_PAD_MAP in its GameSettings file) rather than every game's.
     */
    private String padMapTid;
    private boolean padMapGame;

    PadMapping padMapForTest() {
        return padMap;
    }

    /**
     * Controls (XBA-131): one mapping for every title and every pad, RECOMP_PAD_MAP in recomp.cfg (PadMapping), read
     * by src/input/xinput_device.c at the game's start. One row per Xbox control, its value the player's button
     * (left / right or A step it, as in Settings), grouped; the sticks; « Reset controls » (also Y). The description
     * column says what the focused row does and draws the player's pad with what each control gives the game. The
     * launcher's own navigation and the in-game menu keep the usual buttons (Android key events, not the game's pad).
     */
    private void buildControls() {
        screenTitle.setText(R.string.CONTROLS);
        cfg = RecompConfig.load(GameStore.externalFilesDir(this));
        padMapTid = state() != State.NONE ? titleId() : null;
        String own = padMapTid == null ? null : GameSettings.raw(this, padMapTid, PadMapping.KEY);
        padMapGame = own != null;
        padMap = padMapGame ? PadMapping.parse(own) : PadMapping.load(cfg);
        LinearLayout list = listTemplate();
        if (padMapTid != null)
            xboxControlsScope(list);
        PadMapping.Source[] sources = PadMapping.Source.values();
        String[] sourceLabels = new String[sources.length];
        for (int i = 0; i < sources.length; i++)
            sourceLabels[i] = getString(ControlsLabels.source(sources[i]));
        for (int g = 0; g < ControlsLabels.GROUPS.length; g++) {
            listLabel(list, getString(ControlsLabels.GROUPS[g]));
            for (PadMapping.Control c : ControlsLabels.GROUP_ROWS[g]) {
                MenuRow r = settingRow(list, getString(ControlsLabels.control(c)), "");
                r.setChoices(sourceLabels);
                r.setValue(getString(ControlsLabels.source(padMap.get(c))));
                r.cycler = d -> {
                    padMap.step(c, d);
                    r.setValue(getString(ControlsLabels.source(padMap.get(c))));
                    saveControls();
                    feedback.play(UiFeedback.Cue.MOVE, r);
                    controlDetail(c);
                };
                r.setOnClickListener(v -> r.cycler.step(1));
                onFocus(r, () -> controlDetail(c));
                if (defaultFocus == null)
                    defaultFocus = r;   // the first control, above the scope row
            }
        }
        listLabel(list, getString(R.string.CTL_GROUP_STICKS));
        PadMapping.Stick[] sticks = PadMapping.Stick.values();
        String[] stickLabels = new String[sticks.length];
        for (int i = 0; i < sticks.length; i++)
            stickLabels[i] = getString(ControlsLabels.stick(sticks[i]));
        for (int side = 0; side < 2; side++) {
            boolean left = side == 0;
            int name = left ? R.string.XB_LSTICK : R.string.XB_RSTICK;
            MenuRow r = settingRow(list, getString(name), "");
            r.setChoices(stickLabels);
            r.setValue(getString(ControlsLabels.stick(left ? padMap.leftStick : padMap.rightStick)));
            r.cycler = d -> {
                if (left)
                    padMap.leftStick = PadMapping.stepStick(padMap.leftStick, d);
                else
                    padMap.rightStick = PadMapping.stepStick(padMap.rightStick, d);
                r.setValue(getString(ControlsLabels.stick(left ? padMap.leftStick : padMap.rightStick)));
                saveControls();
                feedback.play(UiFeedback.Cue.MOVE, r);
                stickDetail(left);
            };
            r.setOnClickListener(v -> r.cycler.step(1));
            onFocus(r, () -> stickDetail(left));
        }
        controlsPorts(list);   // XBA-123
        controlsTouch(list);   // XBA-42
        controlsHotkeys(list, Console.XBOX, padMapTid,   // XBA-190: the menu chord, an Xbox title's only hotkey
                        padMapGame || Hotkeys.gameText(this, padMapTid) != null, () -> Hotkeys.sourcesOf(padMap));
        MenuRow reset = listRow(list, getString(R.string.CTL_RESET), getString(R.string.CTL_RESET_DETAIL),
                                this::resetControls);
        onFocus(reset, () -> controlsDescribe(getString(R.string.CTL_RESET_DETAIL)));
        controlsDescribe(getString(R.string.CTL_LEAD));
        changedCount();
        if (padMapGame)
            setNote(getString(R.string.CC_GAME_MAP_ON), NOTE_INFO);
        yAction = this::resetControls;
        addHint("A", R.string.HINT_CHANGE, this::clickFocused);
        addHint("Y", R.string.HINT_DEFAULTS, this::resetControls);
        addHint("B", R.string.HINT_BACK, this::back);
    }

    /**
     * Controls › Players (XBA-123): a row per port, First come or a controller pinned to it (RECOMP_PAD_PORTS,
     * PadPorts, read by src/input/input_ports.c at the game's start); the description says what the game would give
     * each port with the controllers connected now.
     */
    private void controlsPorts(LinearLayout list) {
        PadPorts ports = PadPorts.load(cfg);
        java.util.List<PadPorts.Device> present = PadDevices.connected(this);
        listLabel(list, getString(R.string.CTL_GROUP_PORTS));
        MenuRow[] rows = new MenuRow[PadPorts.PORTS];
        for (int i = 0; i < PadPorts.PORTS; i++)
            rows[i] = settingRow(list, getString(R.string.CTL_PORT, i + 1), "");
        Runnable values = () -> {   // all four: a pin moved to one port leaves another
            for (int i = 0; i < PadPorts.PORTS; i++)
                rows[i].setValue(PadPortsLabels.pin(this, ports, i, present));
        };
        values.run();
        for (int i = 0; i < PadPorts.PORTS; i++) {
            int port = i;
            MenuRow r = rows[i];
            Runnable detail = () -> describe(getString(R.string.CTL_PORT_DETAIL, port + 1) + "\n\n"
                                             + PadPortsLabels.now(this, ports, present));
            r.cycler = d -> {
                ports.step(port, d, present);
                ports.store(cfg);
                saveSettings();
                values.run();
                detail.run();
                Log.i(TAG, "launcher: controller ports " + (ports.isDefault() ? "(first come)" : ports.format()));
                feedback.play(UiFeedback.Cue.MOVE, r);
            };
            r.setOnClickListener(v -> r.cycler.step(1));
            onFocus(r, detail);
        }
    }

    /** Controls › On-screen pad (XBA-42): when it shows, its opacity, its size (TouchSettings, files/settings). */
    private void controlsTouch(LinearLayout list) {
        controlsTouch(list, getString(R.string.CTL_TOUCH_DETAIL), Console.XBOX, padMapTid);
    }

    private void controlsTouch(LinearLayout list, String modeDetail, Console con, String gameKey) {
        TouchSettings t = TouchSettings.load(this);
        listLabel(list, getString(R.string.CTL_GROUP_TOUCH));
        MenuRow mode = settingRow(list, getString(R.string.CTL_TOUCH), "");
        MenuRow opacity = settingRow(list, getString(R.string.CTL_TOUCH_OPACITY), "");
        MenuRow size = settingRow(list, getString(R.string.CTL_TOUCH_SIZE), "");
        Runnable values = () -> {
            mode.setValue(PadPortsLabels.mode(this, t.mode));
            opacity.setValue(PadPortsLabels.percent(this, t.opacity));
            size.setValue(PadPortsLabels.size(this, t.size));
        };
        values.run();
        Widgets.Cycler[] steps = {
            d -> t.mode = TouchSettings.Mode.values()[Math.floorMod(t.mode.ordinal() + d, 3)],
            d -> t.opacity = TouchSettings.step(TouchSettings.OPACITIES, t.opacity, d),
            d -> t.size = TouchSettings.step(TouchSettings.SIZES, t.size, d),
        };
        MenuRow[] rows = { mode, opacity, size };
        String[] details = { modeDetail, getString(R.string.CTL_TOUCH_OPACITY_DETAIL),
                             getString(R.string.CTL_TOUCH_SIZE_DETAIL) };
        for (int i = 0; i < rows.length; i++) {
            MenuRow r = rows[i];
            Widgets.Cycler step = steps[i];
            r.cycler = d -> {
                step.step(d);
                t.save(this);
                values.run();
                Log.i(TAG, "launcher: touch controls " + t.format());
                feedback.play(UiFeedback.Cue.MOVE, r);
            };
            r.setOnClickListener(v -> r.cycler.step(1));
            String text = details[i];
            onFocus(r, () -> describe(text));
        }
        controlsTouchEdit(list, con, gameKey);
    }

    /**
     * Controls › « Edit touch layout » (XBA-207): left / right pick the console's layout (the page's first), A opens
     * the editor (TouchEditor) over the launcher. A game's page passes its key: « This game only » for its layout.
     */
    private void controlsTouchEdit(LinearLayout list, Console con, String gameKey) {
        boolean analog = cfg != null && "analog".equalsIgnoreCase(cfg.get("RECOMP_PS1_PAD"));
        TouchPad.Layout own = TouchPad.layoutFor(con, analog);
        TouchPad.Layout[] all = TouchPad.Layout.values();
        String[] labels = new String[all.length];
        for (int i = 0; i < all.length; i++)
            labels[i] = TouchEditor.consoleName(this, all[i]);
        MenuRow edit = settingRow(list, getString(R.string.CTL_TOUCH_EDIT), "");
        edit.setChoices(labels);
        TouchPad.Layout[] pick = { own };
        Runnable detail = () -> describe(getString(R.string.CTL_TOUCH_EDIT_DETAIL, labels[pick[0].ordinal()]));
        edit.setValue(labels[own.ordinal()]);
        edit.cycler = d -> {
            pick[0] = all[Math.floorMod(pick[0].ordinal() + d, all.length)];
            edit.setValue(labels[pick[0].ordinal()]);
            detail.run();
            feedback.play(UiFeedback.Cue.MOVE, edit);
        };
        edit.setOnClickListener(v -> openTouchEditor(pick[0], pick[0] == own ? gameKey : null));
        onFocus(edit, detail);
        // XBA-212: every saved layout away at once (all consoles, every game's own), the usual layout back.
        listRow(list, getString(R.string.CTL_TOUCH_RESET), getString(R.string.CTL_TOUCH_RESET_DETAIL), () -> {
            int n = TouchLayout.resetAll(getFilesDir());
            Log.i(TAG, "launcher: touch layouts reset, " + n + " file(s) removed");
            describe(getString(R.string.CTL_TOUCH_RESET_DONE, n));
        });
    }

    /** The touch layout editor over the launcher; the focus comes back to the row that opened it. */
    void openTouchEditor(TouchPad.Layout layout, String gameKey) {
        if (touchEditor != null)
            return;
        View back = getCurrentFocus();
        feedback.play(UiFeedback.Cue.SELECT, back);
        touchEditor = new TouchEditor(this, layout, gameKey, TouchSettings.load(this), false, saved -> {
            ViewGroup parent = (ViewGroup)touchEditor.getParent();
            if (parent != null)
                parent.removeView(touchEditor);
            touchEditor = null;
            if (back != null)
                back.requestFocus();
        });
        addContentView(touchEditor, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                               ViewGroup.LayoutParams.MATCH_PARENT));
        Log.i(TAG, "launcher: touch layout editor " + layout + (gameKey != null ? " (" + gameKey + ")" : ""));
    }

    /** The touch layout editor while it is open (XBA-207), else null. */
    TouchEditor touchEditor;

    /** The focused control's line: where it comes from, and which other control shares that button. */
    private void controlDetail(PadMapping.Control c) {
        String name = getString(ControlsLabels.control(c));
        PadMapping.Source s = padMap.get(c);
        String text = s == PadMapping.Source.NONE ? getString(R.string.CTL_ROW_NONE, name)
                      : getString(R.string.CTL_ROW, name, getString(ControlsLabels.source(s)));
        java.util.List<PadMapping.Control> shared = padMap.sharing(c);
        if (!shared.isEmpty()) {
            StringBuilder b = new StringBuilder();
            for (PadMapping.Control o : shared)
                b.append(b.length() == 0 ? "" : ", ").append(getString(ControlsLabels.control(o)));
            text += " " + getString(R.string.CTL_ROW_SHARED, getString(ControlsLabels.source(s)), b);
        }
        controlsDescribe(text);
    }

    private void stickDetail(boolean left) {
        String name = getString(left ? R.string.XB_LSTICK : R.string.XB_RSTICK);
        PadMapping.Stick s = left ? padMap.leftStick : padMap.rightStick;
        controlsDescribe(s == PadMapping.Stick.NONE ? getString(R.string.CTL_STICK_NONE, name)
                                                    : getString(R.string.CTL_STICK_ROW, name,
                                                                getString(ControlsLabels.stick(s))));
    }

    /** The description column of Controls: the text, then the player's pad with what each control gives the game. */
    private void controlsDescribe(String text) {
        describe(text);
        if (desc == null)
            return;
        Widgets.PadDiagram pad = new Widgets.PadDiagram(this);
        pad.bodyFrac = 0.42f;   // room for the callouts in the narrow column
        String[] labels = ControlsLabels.diagram(this, padMap);
        System.arraycopy(labels, 0, pad.labels, 0, labels.length);
        LinearLayout.LayoutParams p = lp(MATCH, 0, 1);
        p.topMargin = Theme.S3;
        desc.addView(pad, p);
    }

    /** « 3 changed » top right while the mapping is not the usual one. */
    private void changedCount() {
        int n = padMap.changedCount();
        topRight.setText(n == 0 ? "" : getString(R.string.CTL_CHANGED, n));
    }

    private void saveControls() {
        if (padMapGame && padMapTid != null) {   // XBA-188: the title's own (« » = the usual mapping, for it alone)
            boolean ok = GameSettings.put(this, padMapTid, PadMapping.KEY, padMap.format());
            setNote(getString(!ok ? R.string.GS_SAVE_FAILED : gameRunning() ? R.string.SETTINGS_SAVED_RUNNING
                                                                             : R.string.SETTINGS_SAVED),
                    ok ? NOTE_OK : NOTE_ERROR);
        } else {
            padMap.store(cfg);
            saveSettings();
        }
        changedCount();
        Log.i(TAG, "launcher: controls (" + (padMapGame ? padMapTid : "every game") + ") "
                   + (padMap.isDefault() ? "(default)" : padMap.format()));
    }

    /**
     * XBA-188: « Mapping for: All Xbox games / This game only » at the top of an Xbox title's Controls, as a console
     * game's (ROM_CONTROLS): this game only takes a copy of every game's mapping to change on its own (its
     * GameSettings file), all games drops it.
     */
    private void xboxControlsScope(LinearLayout list) {
        String cname = ConsoleControls.consoleName(this, Console.XBOX);
        MenuRow scope = settingRow(list, getString(R.string.CC_SCOPE), "");
        String[] scopes = { getString(R.string.CC_SCOPE_CONSOLE, cname), getString(R.string.CC_SCOPE_GAME) };
        scope.setChoices(scopes);
        scope.setValue(scopes[padMapGame ? 1 : 0]);
        if (padMapGame)
            scope.setBadge(getString(R.string.GS_BADGE), Theme.ACCENT);
        String tid = padMapTid;
        scope.cycler = d -> {
            GameSettings.put(this, tid, PadMapping.KEY, padMapGame ? null : padMap.format());
            Hotkeys.storeGame(this, tid, padMapGame ? null : hotkeys);   // XBA-190: the hotkeys follow the scope
            Log.i(TAG, "launcher: " + tid + " controls " + (padMapGame ? "follow every game's" : "of its own"));
            feedback.play(UiFeedback.Cue.MOVE, scope);
            show(Screen.CONTROLS, 0);
        };
        scope.setOnClickListener(v -> scope.cycler.step(1));
        String text = getString(R.string.CC_SCOPE_DETAIL, cname);
        onFocus(scope, () -> controlsDescribe(text));
    }

    /** Y, or « Reset controls »: the usual mapping (the line goes from recomp.cfg). */
    private void resetControls() {
        padMap.reset();
        saveControls();
        show(Screen.CONTROLS, 0);
        setNote(getString(R.string.CTL_RESET_DONE), NOTE_OK);
    }

    // ---- Hotkeys (XBA-190) ----------------------------------------------------------------------------------------

    /** The hotkeys the Controls screen edits: every game's (recomp.cfg) or, hotkeysGame, the game's own. */
    private Hotkeys hotkeys = new Hotkeys();
    private boolean hotkeysGame;
    private String hotkeysKey;
    /** « Press the buttons »: the capture in progress (null: none), its action and row. */
    private Hotkeys.Capture hkCapture;
    private Hotkeys.Action hkCaptureAction;
    private MenuRow hkCaptureRow;
    /** Each hotkey row's value, refreshed together (a change can make or end a conflict elsewhere). */
    private final List<Runnable> hkRefresh = new ArrayList<>();
    /** Each hotkey row's description, said again once a capture changes its chord. */
    private final EnumMap<Hotkeys.Action, Runnable> hkDetails = new EnumMap<>(Hotkeys.Action.class);
    static final long HK_CAPTURE_MS = 5000;
    private final Runnable hkCaptureTimeout = () -> endHotkeyCapture(true);

    Hotkeys hotkeysForTest() {
        return hotkeys;
    }

    boolean hotkeyCapturingForTest() {
        return hkCapture != null;
    }

    /**
     * A Controls screen's « Hotkeys » (Hotkeys, src/platform/recomp_hotkeys.h): one row per action a game of console
     * con has (the menu's alone for an Xbox title), its value the chord; A captures a new one (« press the buttons »),
     * left / right turn it off or put the usual one back; « · conflict » after the chord when two actions share it or a
     * chord without a modifier takes a button the game uses (gameSources: the CPS bits its mapping reads); « Reset
     * hotkeys ». Saved at once: the game's own (key, its GameSettings file) when game, else recomp.cfg's line.
     */
    private void controlsHotkeys(LinearLayout list, Console con, String key, boolean game,
                                 java.util.function.IntSupplier gameSources) {
        hotkeysKey = key;
        hotkeysGame = game && key != null;
        hotkeys = hotkeysGame ? Hotkeys.parse(Hotkeys.gameText(this, key)) : Hotkeys.loadGlobal(cfg);
        hkRefresh.clear();
        hkDetails.clear();
        listLabel(list, getString(R.string.HK_GROUP));
        for (Hotkeys.Action a : Hotkeys.actions(con)) {
            MenuRow r = settingRow(list, getString(Hotkeys.label(a)), "");
            Runnable value = () -> {
                if (r == hkCaptureRow && hkCapture != null && hkCapture.result() == null)
                    return;
                String label = Hotkeys.chordLabel(this, hotkeys.get(a));   // a badge would hide the value
                r.setValue(hotkeys.hasConflict(a, con, gameSources.getAsInt())
                           ? getString(R.string.HK_VALUE_CONFLICT, label) : label);
            };
            hkRefresh.add(value);
            value.run();
            Runnable detail = () -> hotkeyDetail(a, con, gameSources.getAsInt());
            hkDetails.put(a, detail);
            r.cycler = d -> {
                Hotkeys.Chord cur = hotkeys.get(a);
                hotkeys.set(a, !cur.isNone() ? Hotkeys.Chord.NONE : Hotkeys.def(a));
                saveHotkeys();
                feedback.play(UiFeedback.Cue.MOVE, r);
                detail.run();
            };
            r.setOnClickListener(v -> startHotkeyCapture(a, r));
            onFocus(r, detail);
        }
        String resetText = getString(R.string.HK_RESET_DETAIL);
        MenuRow reset = listRow(list, getString(R.string.HK_RESET), resetText, () -> {
            hotkeys.reset();
            saveHotkeys();
            setNote(getString(R.string.HK_RESET_DONE), NOTE_OK);
        });
        onFocus(reset, () -> describe(resetText + "\n\n" + getString(R.string.HK_DETAIL_SCOPE)));
    }

    /** The focused hotkey's text: what it is, its chord, its conflicts, where it is kept. */
    private void hotkeyDetail(Hotkeys.Action a, Console con, int gameSources) {
        String name = getString(Hotkeys.label(a)), chord = Hotkeys.chordLabel(this, hotkeys.get(a));
        StringBuilder b = new StringBuilder(getString(a == Hotkeys.Action.MENU ? R.string.HK_DETAIL_MENU
                                                                               : R.string.HK_DETAIL, name, chord));
        List<Hotkeys.Action> shared = hotkeys.sharing(a, con);
        if (!shared.isEmpty()) {
            StringBuilder o = new StringBuilder();
            for (Hotkeys.Action x : shared)
                o.append(o.length() == 0 ? "" : ", ").append(getString(Hotkeys.label(x)));
            b.append("\n\n").append(getString(R.string.HK_SHARED, o));
        }
        PadMapping.Source need = hotkeys.gameNeeds(a, gameSources);
        if (need != null)
            b.append("\n\n").append(getString(R.string.HK_GAME_NEEDS, getString(ControlsLabels.source(need))));
        b.append("\n\n").append(getString(R.string.HK_DETAIL_SCOPE));
        describe(b.toString());
    }

    /** Written at once: the game's own (its GameSettings file), or recomp.cfg's line (none for the defaults). */
    private void saveHotkeys() {
        if (hotkeysGame) {
            boolean ok = Hotkeys.storeGame(this, hotkeysKey, hotkeys);
            setNote(getString(!ok ? R.string.GS_SAVE_FAILED : gameRunning() ? R.string.SETTINGS_SAVED_RUNNING
                                                                             : R.string.SETTINGS_SAVED),
                    ok ? NOTE_OK : NOTE_ERROR);
        } else {
            Hotkeys.storeGlobal(cfg, hotkeys);
            saveSettings();
        }
        for (Runnable r : hkRefresh)
            r.run();
        Log.i(TAG, "launcher: hotkeys (" + (hotkeysGame ? hotkeysKey : "every game") + ") "
                   + (hotkeys.isDefault() ? "(default)" : hotkeys.format()));
    }

    /** A, on a hotkey row: the next buttons pressed make its chord (or nothing within HK_CAPTURE_MS). */
    private void startHotkeyCapture(Hotkeys.Action a, MenuRow r) {
        endHotkeyCapture(false);
        hkCapture = new Hotkeys.Capture();
        hkCaptureAction = a;
        hkCaptureRow = r;
        r.setValue(getString(R.string.HK_PRESS));
        setNote(getString(R.string.HK_CAPTURE_NOTE, getString(Hotkeys.label(a))), NOTE_INFO);
        feedback.play(UiFeedback.Cue.MOVE, r);
        poller.postDelayed(hkCaptureTimeout, HK_CAPTURE_MS);
    }

    /** A key while capturing: true when the capture took it (every key until the chord's buttons are up again). */
    private boolean hotkeyCaptureKey(KeyEvent e) {
        Hotkeys.Capture c = hkCapture;
        if (hkCaptureRow == null || !hkCaptureRow.isAttachedToWindow()) {   // the screen went away
            endHotkeyCapture(false);
            return false;
        }
        int k = e.getKeyCode();
        boolean down = e.getAction() == KeyEvent.ACTION_DOWN;
        if (c.result() == null && (k == KeyEvent.KEYCODE_ESCAPE
                                   || (k == KeyEvent.KEYCODE_BACK && !Hotkeys.fromGamepad(e.getSource())))) {
            if (!down)
                endHotkeyCapture(true);   // a touch screen's or a keyboard's Back: nothing changes
            return true;
        }
        if (e.getRepeatCount() > 0)
            return true;
        hotkeyCaptured(c.key(k, down));
        if (!c.active())
            hkCapture = null;
        return true;
    }

    /** The trigger axes while capturing (a pad whose L2 / R2 come only as axes). */
    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent e) {
        if (hkCapture != null && (e.getSource() & android.view.InputDevice.SOURCE_CLASS_JOYSTICK) != 0) {
            Hotkeys.Capture c = hkCapture;
            float l = Math.max(e.getAxisValue(MotionEvent.AXIS_LTRIGGER), e.getAxisValue(MotionEvent.AXIS_BRAKE));
            float r = Math.max(e.getAxisValue(MotionEvent.AXIS_RTRIGGER), e.getAxisValue(MotionEvent.AXIS_GAS));
            hotkeyCaptured(c.triggers(l, r));
            if (!c.active())
                hkCapture = null;
            return true;
        }
        return super.dispatchGenericMotionEvent(e);
    }

    private void hotkeyCaptured(Hotkeys.Chord chord) {
        if (chord == null || hkCaptureAction == null)
            return;
        poller.removeCallbacks(hkCaptureTimeout);
        Hotkeys.Action a = hkCaptureAction;
        hkCaptureAction = null;
        hotkeys.set(a, chord);
        saveHotkeys();
        setNote(getString(R.string.HK_CAPTURED, getString(Hotkeys.label(a)), Hotkeys.chordLabel(this, chord)), NOTE_OK);
        Runnable detail = hkDetails.get(a);
        if (detail != null)
            detail.run();
        if (hkCaptureRow != null)
            feedback.play(UiFeedback.Cue.MOVE, hkCaptureRow);
    }

    /** Ends a capture (the timeout, Back on a touch screen, another capture); note: say nothing changed. */
    private void endHotkeyCapture(boolean note) {
        poller.removeCallbacks(hkCaptureTimeout);
        boolean was = hkCapture != null && hkCapture.result() == null;
        hkCapture = null;
        hkCaptureAction = null;
        for (Runnable r : hkRefresh)
            r.run();
        if (was && note)
            setNote(getString(R.string.HK_CAPTURE_CANCELLED), NOTE_INFO);
    }

    // ---- ROM_CONTROLS (XBA-158 / XBA-168 / XBA-174) ---------------------------------------------------------------

    /** The mapping ROM_CONTROLS edits: the console's (recomp.cfg) or, romMapGame, the game's own (ConsoleControls). */
    private ConsolePadMap romMap;
    private boolean romMapGame;

    ConsolePadMap romMapForTest() {
        return romMap;
    }

    /**
     * A GBA, PS1 or N64 game's Controls: whose mapping (all the console's games, or this game only), one row per
     * console button (its value the player's controls, left / right or A step it), the sticks (PS1, N64), the N64
     * stick's range and dead zone, the on-screen pad (shared with the Xbox's), « Reset controls » (also Y). Read by
     * the console's runtime at the game's start (src/input/console_pad.h): recomp.cfg's line for the console, the
     * game's own through the environment (ShellActivity). The launcher and the in-game menu keep the usual buttons.
     */
    private void buildRomControls() {
        screenTitle.setText(R.string.CONTROLS);
        LibraryScreen.Entry e = romEntry;
        if (e == null) {   // nothing to edit (the activity came back without its game)
            show(Screen.LIBRARY, 0);
            return;
        }
        Console con = e.console();
        String key = e.tid, cname = ConsoleControls.consoleName(this, con);
        cfg = RecompConfig.load(GameStore.externalFilesDir(this));
        ConsolePadMap own = ConsoleControls.loadGame(this, key);
        romMapGame = own != null;
        romMap = own != null ? own : ConsoleControls.loadConsole(cfg, con);
        LinearLayout list = listTemplate();

        MenuRow scope = settingRow(list, getString(R.string.CC_SCOPE), "");
        String[] scopes = { getString(R.string.CC_SCOPE_CONSOLE, cname), getString(R.string.CC_SCOPE_GAME) };
        scope.setChoices(scopes);
        scope.setValue(scopes[romMapGame ? 1 : 0]);
        scope.cycler = d -> {
            // This game only: a copy of the console's mapping to change on its own; all games: the copy goes.
            ConsoleControls.storeGame(this, key, romMapGame ? null : romMap);
            Hotkeys.storeGame(this, key, romMapGame ? null : hotkeys);   // XBA-190: the hotkeys follow the scope
            Log.i(TAG, "launcher: " + key + " controls " + (romMapGame ? "follow the console's" : "of its own"));
            feedback.play(UiFeedback.Cue.MOVE, scope);
            show(Screen.ROM_CONTROLS, 0);
        };
        scope.setOnClickListener(v -> scope.cycler.step(1));
        String scopeText = getString(R.string.CC_SCOPE_DETAIL, cname);
        onFocus(scope, () -> describe(scopeText));

        PadMapping.Source[] sources = PadMapping.Source.values();
        String[] sourceLabels = new String[sources.length];
        for (int i = 0; i < sources.length; i++)
            sourceLabels[i] = getString(ControlsLabels.source(sources[i]));
        int[] groupLabels = ConsoleControls.groupLabels(con);
        int[][] groups = ConsoleControls.groups(con);
        for (int g = 0; g < groups.length; g++) {
            listLabel(list, getString(groupLabels[g]));
            for (int t : groups[g]) {
                MenuRow r = settingRow(list, getString(ConsoleControls.targetLabel(con, t)), "");
                r.setChoices(sourceLabels);
                r.setValue(ConsoleControls.sourcesLabel(this, romMap, t));
                r.cycler = d -> {
                    romMap.step(t, d);
                    r.setValue(ConsoleControls.sourcesLabel(this, romMap, t));
                    saveRomControls(key);
                    feedback.play(UiFeedback.Cue.MOVE, r);
                    romTargetDetail(t);
                };
                r.setOnClickListener(v -> r.cycler.step(1));
                onFocus(r, () -> romTargetDetail(t));
            }
        }
        if (romMap.stick.length > 0) {
            listLabel(list, getString(con == Console.N64 ? R.string.CC_GROUP_STICK : R.string.CTL_GROUP_STICKS));
            PadMapping.Stick[] sticks = PadMapping.Stick.values();
            String[] stickLabels = new String[sticks.length];
            for (int i = 0; i < sticks.length; i++)
                stickLabels[i] = getString(ControlsLabels.stick(sticks[i]));
            for (int i = 0; i < romMap.stick.length; i++) {
                int si = i;
                String name = getString(ConsoleControls.stickLabel(con, i));
                MenuRow r = settingRow(list, name, "");
                r.setChoices(stickLabels);
                r.setValue(getString(ControlsLabels.stick(romMap.stick[i])));
                Runnable detail = () -> describe(romMap.stick[si] == PadMapping.Stick.NONE
                                                     ? getString(R.string.CTL_STICK_NONE, name)
                                                     : getString(R.string.CTL_STICK_ROW, name,
                                                                 getString(ControlsLabels.stick(romMap.stick[si]))));
                r.cycler = d -> {
                    romMap.stepStick(si, d);
                    r.setValue(getString(ControlsLabels.stick(romMap.stick[si])));
                    saveRomControls(key);
                    feedback.play(UiFeedback.Cue.MOVE, r);
                    detail.run();
                };
                r.setOnClickListener(v -> r.cycler.step(1));
                onFocus(r, detail);
            }
            if (ConsolePadMap.hasRange(con)) {
                MenuRow range = settingRow(list, getString(R.string.CC_RANGE), "");
                MenuRow dead = settingRow(list, getString(R.string.CC_DEADZONE), "");
                range.setValue(ConsoleControls.rangeLabel(this, romMap.range));
                dead.setValue(ConsoleControls.deadzoneLabel(this, romMap.deadzone));
                range.cycler = d -> {
                    romMap.range = ConsolePadMap.stepIn(ConsolePadMap.RANGES, romMap.range, d);
                    range.setValue(ConsoleControls.rangeLabel(this, romMap.range));
                    saveRomControls(key);
                    feedback.play(UiFeedback.Cue.MOVE, range);
                };
                dead.cycler = d -> {
                    romMap.deadzone = ConsolePadMap.stepIn(ConsolePadMap.DEADZONES, romMap.deadzone, d);
                    dead.setValue(ConsoleControls.deadzoneLabel(this, romMap.deadzone));
                    saveRomControls(key);
                    feedback.play(UiFeedback.Cue.MOVE, dead);
                };
                range.setOnClickListener(v -> range.cycler.step(1));
                dead.setOnClickListener(v -> dead.cycler.step(1));
                String rt = getString(R.string.CC_RANGE_DETAIL), dt = getString(R.string.CC_DEADZONE_DETAIL);
                onFocus(range, () -> describe(rt));
                onFocus(dead, () -> describe(dt));
            }
        }
        controlsTouch(list, getString(R.string.CC_TOUCH_DETAIL, cname), con, key);
        controlsHotkeys(list, con, key, romMapGame || Hotkeys.gameText(this, key) != null,   // XBA-190
                        () -> Hotkeys.sourcesOf(romMap));
        String resetText = getString(R.string.CC_RESET_DETAIL, cname,
                                     romMapGame ? e.title() : getString(R.string.CC_SCOPE_CONSOLE, cname));
        MenuRow reset = listRow(list, getString(R.string.CTL_RESET), resetText, this::resetRomControls);
        onFocus(reset, () -> describe(resetText));
        describe(getString(R.string.CC_LEAD, cname));
        romChangedCount();
        if (romMapGame)
            setNote(getString(R.string.CC_GAME_MAP_ON), NOTE_INFO);
        yAction = this::resetRomControls;
        addHint("A", R.string.HINT_CHANGE, this::clickFocused);
        addHint("Y", R.string.HINT_DEFAULTS, this::resetRomControls);
        addHint("B", R.string.HINT_BACK, this::back);
    }

    /** The focused console button's line: where it comes from, and which other button shares that control. */
    private void romTargetDetail(int t) {
        Console con = romMap.console;
        String name = getString(ConsoleControls.targetLabel(con, t));
        String from = ConsoleControls.sourcesLabel(this, romMap, t);
        String text = romMap.source[t] == 0 ? getString(R.string.CTL_ROW_NONE, name)
                                            : getString(R.string.CTL_ROW, name, from);
        java.util.List<Integer> shared = romMap.sharing(t);
        if (!shared.isEmpty()) {
            StringBuilder b = new StringBuilder();
            for (int o : shared)
                b.append(b.length() == 0 ? "" : ", ").append(getString(ConsoleControls.targetLabel(con, o)));
            text += " " + getString(R.string.CTL_ROW_SHARED, from, b);
        }
        describe(text);
    }

    private void romChangedCount() {
        int n = romMap == null ? 0 : romMap.changedCount();
        topRight.setText(n == 0 ? "" : getString(R.string.CTL_CHANGED, n));
    }

    /** Written at once: the game's own file, or recomp.cfg's line for the console (none for the usual mapping). */
    private void saveRomControls(String key) {
        if (romMapGame) {
            ConsoleControls.storeGame(this, key, romMap);
            setNote(getString(gameRunning() ? R.string.SETTINGS_SAVED_RUNNING : R.string.SETTINGS_SAVED), NOTE_OK);
        } else {
            ConsoleControls.storeConsole(cfg, romMap);
            saveSettings();
        }
        romChangedCount();
        Log.i(TAG, "launcher: " + romMap.console.id + " controls (" + (romMapGame ? key : "console") + ") "
                   + (romMap.isDefault() ? "(default)" : romMap.format()));
    }

    /** Y, or « Reset controls »: the console's usual mapping, for the scope shown. */
    private void resetRomControls() {
        if (romMap == null || romEntry == null)
            return;
        romMap.reset();
        saveRomControls(romEntry.tid);
        show(Screen.ROM_CONTROLS, 0);
        setNote(getString(R.string.CTL_RESET_DONE), NOTE_OK);
    }

    // ---- GAME_SETTINGS / ROM_OPTIONS (XBA-181, XBA-188) -------------------------------------------------------

    /** An Xbox title's Game settings (its Home's « Game settings » pill). */
    private void buildXboxGameSettings() {
        String tid = titleId(), dir = gameDir();
        if (tid == null) {   // nothing to key the values on (the game went away)
            show(Screen.HOME, 0);
            return;
        }
        buildGameSettings(tid, Console.XBOX, dir != null ? gameTitle(dir) : tid);
    }

    /** A GBA, PS1 or N64 game's Game settings (its Home's « Game settings » pill), then its stored things. */
    private void buildRomOptions() {
        LibraryScreen.Entry e = romEntry;
        if (e == null) {   // nothing to show (the activity came back without its game)
            show(Screen.LIBRARY, 0);
            return;
        }
        buildGameSettings(e.tid, Console.ofKey(e.tid), e.title());
    }

    /**
     * Game settings (XBA-188, GameSettings): every Settings option that applies to the game's console, each « As in
     * Settings (<Settings' value>) » or one of its values for this game alone (left / right, A), a « This game »
     * badge on the rows whose value differs from Settings'; the game's controls (a link to its Controls), for an Xbox
     * title its theme and enhancements, for an N64 game its Vulkan driver; « Reset to Settings » (also Y); for a
     * console game its stored things (saves, pack, Prepare files, each deleted on its own, A twice). Saved at once,
     * read by ShellActivity at the game's next start.
     */
    private void buildGameSettings(String key, Console con, String gameName) {
        screenTitle.setText(R.string.GAME_SETTINGS);
        topRight.setText(gameName);
        cfg = RecompConfig.load(GameStore.externalFilesDir(this));
        LinearLayout list = listTemplate();
        MenuRow first = null;
        RecompConfig.Section section = null;
        for (RecompConfig.Option o : GameSettings.options(con, lsfgBuilt())) {
            if (o.section != section) {
                section = o.section;
                listLabel(list, getString(TAB_LABELS[tabOf(section)]));
            }
            MenuRow r = gameOptionRow(list, key, o);
            if (first == null)
                first = r;
        }
        listLabel(list, getString(R.string.GS_GROUP_GAME));
        gameControlsRow(list, key, con);
        if (con != Console.XBOX)
            cheatsRow(list, key, con);
        if (con == Console.N64 && !vkdInherits())   // only the N64 Vulkan renderer uses one, for now
            romVkDriverRow(list, key);
        if (con == Console.XBOX) {
            gameThemeRow(list, key, gameName);
            if (Toolchain.available(this) && Pack.titleLayer(getFilesDir(), key) != null)
                enhancementsRow(list, key, gameName);
        }
        String resetText = getString(R.string.GS_RESET_DETAIL, gameName);
        MenuRow reset = listRow(list, getString(R.string.GS_RESET), resetText, () -> resetGameSettings(key, con));
        onFocus(reset, () -> describe(resetText));
        int own = GameSettings.overridden(cfg, GameSettings.load(this, key), con, lsfgBuilt()).size();
        reset.setValue(own == 0 ? getString(R.string.GS_NONE_OWN) : getString(R.string.GS_OWN_COUNT, own));
        if (con != Console.XBOX) {
            // Its stored things (XBA-181: the console's save folder, save/<console>-<KEY>/, Storage.saves).
            List<Storage.Item> items = Storage.items(storageDirs(), key, games.get(key));
            if (!items.isEmpty()) {
                listLabel(list, getString(R.string.ROM_OPT_DATA));
                for (Storage.Item it : items)
                    storageRow(list, it);
                storageMeasureLater(items);
            }
        }
        defaultFocus = first;
        yAction = () -> resetGameSettings(key, con);
        addHint("A", R.string.HINT_CHANGE, this::clickFocused);
        addHint("Y", R.string.GS_HINT_RESET, () -> resetGameSettings(key, con));
        addHint("B", R.string.HINT_BACK, this::back);
    }

    private static int tabOf(RecompConfig.Section s) {
        switch (s) {
        case GRAPHICS:
            return TAB_GRAPHICS;
        case AUDIO:
            return TAB_AUDIO;
        case ADVANCED:
            return TAB_ADVANCED;
        default:
            return TAB_GENERAL;
        }
    }

    /** The description of an option's row in Game settings: the console picture's own texts, else Settings'. */
    private String gameOptionDetail(RecompConfig.Option o) {
        if (o.title == R.string.OPT_CSCALE)
            return getString(R.string.ROM_OPT_SCALE_DETAIL);
        if (o.title == R.string.OPT_CFILTER)
            return getString(R.string.ROM_OPT_FILTER_DETAIL);
        if (o.title == R.string.OPT_UPSCALE)
            return getString(R.string.ROM_OPT_UPSCALE_DETAIL);
        if (o.title == R.string.OPT_LSFG)
            return getString(R.string.ROM_OPT_FRAMEGEN_DETAIL);
        return getString(R.string.GS_OPTION_DETAIL, getString(o.detail));
    }

    /** One option of Game settings: « As in Settings (<value>) », then each choice; the badge when it differs. */
    private MenuRow gameOptionRow(LinearLayout list, String key, RecompConfig.Option o) {
        String follow = getString(R.string.ROM_OPT_FOLLOW, choiceLabel(cfg, o));
        String[] labels = new String[o.choices.length + 1];
        labels[0] = follow;
        for (int i = 0; i < o.choices.length; i++)
            labels[i + 1] = getString(o.choices[i].label);
        MenuRow r = settingRow(list, getString(o.title), gameOptionDetail(o));
        r.setChoices(labels);
        Runnable refresh = () -> {
            RecompConfig game = GameSettings.load(this, key);
            int pos = GameSettings.position(cfg, game, o);
            r.setValue(pos < 0 ? getString(R.string.VAL_CUSTOM) : labels[pos]);
            boolean differs = GameSettings.differs(cfg, game, o);
            r.setBadge(differs ? getString(R.string.GS_BADGE) : null, Theme.ACCENT);
            r.invalidate();
        };
        refresh.run();
        r.cycler = d -> {
            RecompConfig game = GameSettings.load(this, key);
            int pos = GameSettings.step(GameSettings.position(cfg, game, o), o, d);
            GameSettings.setPosition(game, o, pos);
            if (!GameSettings.save(game)) {
                setNote(getString(R.string.SETTINGS_SAVE_FAILED, game.file().getName()), NOTE_ERROR);
                return;
            }
            refresh.run();
            feedback.play(UiFeedback.Cue.MOVE, r);
            setNote(getString(gameRunning() && key.equals(launchedTid()) ? R.string.ROM_OPT_SAVED_RUNNING
                                                                         : R.string.ROM_OPT_SAVED), NOTE_OK);
            Log.i(TAG, "launcher: " + key + " " + String.join("+", o.keys) + " "
                       + (pos == 0 ? "as in Settings" : getResources().getResourceEntryName(o.choices[pos - 1].label)));
        };
        r.setOnClickListener(v -> r.cycler.step(1));
        return r;
    }

    /** « Controls »: whose mapping the game plays with (its own or as in Settings); A opens its Controls. */
    private void gameControlsRow(LinearLayout list, String key, Console con) {
        String env = con == Console.XBOX ? PadMapping.KEY : ConsoleControls.env(con);
        boolean own = GameSettings.raw(this, key, env) != null;
        String text = getString(R.string.GS_CONTROLS_DETAIL);
        MenuRow r = listRow(list, getString(R.string.CONTROLS), text,
                            () -> go(con == Console.XBOX ? Screen.CONTROLS : Screen.ROM_CONTROLS));
        r.setValue(getString(own ? R.string.CC_SCOPE_GAME : R.string.MENU_VIDEO_FOLLOW));
        if (own)
            r.setBadge(getString(R.string.GS_BADGE), Theme.ACCENT);
        onFocus(r, () -> describe(text));
    }

    /**
     * XBA-197: Game settings › « Cheats » of a GBA, PS1 or N64 game: how many are on, A opens the list (CheatsUi.manage:
     * on / off, add, delete, saved at once; the game's in-game menu has the same page). Not touched by « Reset to
     * Settings » (the codes are the player's, in their own file).
     */
    private void cheatsRow(LinearLayout list, String key, Console con) {
        String text = getString(R.string.CHEATS_DETAIL);
        MenuRow[] r = new MenuRow[1];
        Runnable refresh = () -> {
            r[0].setValue(CheatsUi.countLabel(this, key));
            r[0].invalidate();
        };
        r[0] = listRow(list, getString(R.string.CHEATS), text, () -> CheatsUi.manage(this, con, key, refresh));
        refresh.run();
        onFocus(r[0], () -> describe(text));
    }

    /** Y, or « Reset to Settings »: the game's file goes (its N64 Vulkan driver choice too); the screen again. */
    private void resetGameSettings(String key, Console con) {
        boolean ok = GameSettings.reset(getFilesDir(), key);
        if (con == Console.N64)
            VkDrivers.of(this).setGameChoice(key, null);
        Log.i(TAG, "launcher: " + key + " game settings reset to Settings" + (ok ? "" : " (failed)"));
        show(screen, 0);   // the theme may change back too (syncTheme)
        setNote(getString(ok ? R.string.GS_RESET_DONE : R.string.GS_RESET_FAILED), ok ? NOTE_OK : NOTE_ERROR);
    }

    /**
     * ROM_OPTIONS › « Vulkan driver »: as in Settings (its driver named), the system driver, or each installed driver
     * (VkDrivers.setGameChoice; ShellActivity.applyVkDriver resolves it at the game's next start); the description
     * warns when the driver the game would start with went wrong last time.
     */
    private void romVkDriverRow(LinearLayout list, String key) {
        VkDrivers drivers = VkDrivers.of(this);
        List<String> choices = VkDriverStore.gameChoices(drivers.list());
        String[] labels = new String[choices.size()];
        for (int i = 0; i < labels.length; i++)
            labels[i] = VkDriverStore.gameChoiceLabel(this, drivers, choices.get(i));
        String detail = getString(R.string.ROM_OPT_VK_DRIVER_DETAIL);
        MenuRow r = settingRow(list, getString(R.string.OPT_VK_DRIVER), detail);
        r.setChoices(labels);
        Runnable refresh = () -> {
            r.setValue(VkDriverStore.gameChoiceLabel(this, drivers, drivers.gameChoice(key)));
            int warn = VkDriverStore.effectiveWarning(drivers, key);
            if (r.hasFocus())
                describe(detail, warn != 0 ? getString(warn) : null, Theme.WARN);
        };
        refresh.run();
        onFocus(r, () -> {
            int warn = VkDriverStore.effectiveWarning(drivers, key);
            describe(detail, warn != 0 ? getString(warn) : null, Theme.WARN);
        });
        r.cycler = d -> {
            String v = VkDriverStore.stepGameChoice(drivers, key, d);
            refresh.run();
            feedback.play(UiFeedback.Cue.MOVE, r);
            setNote(getString(gameRunning() && key.equals(launchedTid()) ? R.string.ROM_OPT_SAVED_RUNNING
                                                                         : R.string.ROM_OPT_SAVED), NOTE_OK);
            Log.i(TAG, "launcher: " + key + " vulkan driver " + (v == null ? "as in Settings" : v));
        };
        r.setOnClickListener(v -> r.cycler.step(1));
    }

    /** The Settings option whose row is titled title (a string resource), or null. */
    private static RecompConfig.Option optionTitled(int title) {
        for (RecompConfig.Option o : RecompConfig.OPTIONS) {
            if (o.title == title)
                return o;
        }
        return null;
    }

    // ---- STORAGE (docs/android/app-shell.md « Game library », Storage) -------------------------------------------

    /**
     * What the app stores, per game: the imported copy, the pack, the Prepare's files, the game's caches, the saves;
     * free space on top. Each deleted on its own, A twice within CONFIRM_MS, on a worker thread. Saves have their own
     * row and a stronger warning; the player's images and library folders are never listed.
     */
    private void buildStorage() {
        screenTitle.setText(R.string.LIB_STORAGE);
        topRight.setText(getString(R.string.LIB_ST_FREE, size(freeSpace())));
        Storage.Dirs dirs = storageDirs();
        LinearLayout list = listTemplate();
        List<Storage.Item> all = new ArrayList<>();
        for (String tid : Storage.titles(dirs, games)) {
            List<Storage.Item> items = Storage.items(dirs, tid, games.get(tid));
            if (items.isEmpty())
                continue;
            Library.Record r = games.get(tid);
            listLabel(list, r != null && !r.title.isEmpty() ? r.title : tid);
            for (Storage.Item it : items) {
                storageRow(list, it);
                all.add(it);
            }
        }
        if (all.isEmpty()) {
            TextView none = Theme.body(this, getString(R.string.LIB_ST_EMPTY));
            none.setPadding(Theme.ROW_INSET, Theme.S3, 0, 0);
            list.addView(none, lp(MATCH, WRAP));
            describe(getString(R.string.LIB_ST_FOOTER));
        }
        storageMeasureLater(all);
        addHint("A", R.string.LIB_ST_HINT_DELETE, this::clickFocused);
        addHint("B", R.string.HINT_BACK, this::back);
        if (gameRunning())
            setNote(getString(R.string.LIB_RUNNING_NOTE, runningTitle()), NOTE_INFO);
    }

    private static String storageKey(Storage.Item it) {
        return it.tid + "/" + it.kind;
    }

    private int storageLabel(Storage.Kind k) {
        switch (k) {
        case DATA:
            return R.string.LIB_ST_DATA;
        case IMAGE:
            return R.string.LIB_ST_IMAGE;
        case PACK:
            return R.string.LIB_ST_PACK;
        case WORK:
            return R.string.LIB_ST_WORK;
        case CACHES:
            return R.string.LIB_ST_CACHES;
        default:
            return R.string.LIB_ST_SAVES;
        }
    }

    private String storageSize(Storage.Item it) {
        Long n = storageSizes.get(storageKey(it));
        return n == null ? getString(R.string.SIZE_COUNTING) : size(n);
    }

    /** One stored thing of a game: its size as the value, A twice deletes it; the description says what it is. */
    private MenuRow storageRow(LinearLayout list, Storage.Item it) {
        int detailText;
        switch (it.kind) {
        case DATA:
            detailText = R.string.LIB_ST_DATA_DETAIL;
            break;
        case IMAGE:
            detailText = R.string.LIB_ST_IMAGE_DETAIL;
            break;
        case PACK:
            detailText = R.string.LIB_ST_PACK_DETAIL;
            break;
        case WORK:
            detailText = R.string.LIB_ST_WORK_DETAIL;
            break;
        case CACHES:
            detailText = R.string.LIB_ST_CACHES_DETAIL;
            break;
        default:
            detailText = R.string.LIB_ST_SAVES_DETAIL;
            break;
        }
        MenuRow r = listRow(list, getString(storageLabel(it.kind)), getString(detailText), null);
        DiscImages.Disc disc = Storage.disc(it);
        r.setValue(disc == null ? storageSize(it)
                                : getString(R.string.LIB_ST_IMAGE_VALUE, storageSize(it), size(disc.stored)));
        r.setOnClickListener(v -> onStorageDelete(r, it));
        onFocus(r, () -> {
            List<String> paths = new ArrayList<>();
            if (disc != null)   // where the image is: its path, or the document it was picked as
                paths.add(kv(R.string.KV_SOURCE, disc.viaPath() ? disc.path : DiscImages.label(disc)
                                                                  + " (" + disc.kind + ")"));
            for (File f : it.files) {
                if (f.exists())
                    paths.add(kv(R.string.KV_FOLDER, f.getAbsolutePath()));
            }
            boolean saves = it.kind == Storage.Kind.SAVES;
            String extra = saves ? getString(R.string.LIB_ST_SAVES_FOOTER) : TextUtils.join("\n", paths);
            describe(r.description + (saves && !paths.isEmpty() ? "\n\n" + TextUtils.join("\n", paths) : ""),
                     extra, saves ? Theme.WARN : Theme.TEXT_DIM);
        });
        return r;
    }

    /** The sizes of items not measured yet, on a worker (an imported copy can be gigabytes), then a refresh. */
    private void storageMeasureLater(List<Storage.Item> items) {
        if (storageMeasurer != null && storageMeasurer.isAlive())
            return;
        List<Storage.Item> todo = new ArrayList<>();
        for (Storage.Item it : items) {
            if (!storageSizes.containsKey(storageKey(it)))
                todo.add(it);
        }
        if (todo.isEmpty())
            return;
        storageMeasurer = new Thread(() -> {
            java.util.Map<String, Long> m = new java.util.HashMap<>();
            for (Storage.Item it : todo)
                m.put(storageKey(it), Storage.measure(it));
            runOnUiThread(() -> {
                storageSizes.putAll(m);
                if (screen == Screen.STORAGE || screen == Screen.GAME_DATA || screen == Screen.ROM_OPTIONS
                    || screen == Screen.GAME_SETTINGS)
                    refreshKeepingFocus();
            });
        }, "xba-storage");
        storageMeasurer.start();
    }

    /**
     * A twice within CONFIRM_MS deletes it (saves: a stronger warning first). The game that runs from these files is
     * ended first. An imported copy leaves its record's data_dir (the game stays in the library); its art and theme
     * go only when the title also left the library folder.
     */
    private void onStorageDelete(MenuRow row, Storage.Item it) {
        if (deleting)
            return;
        long now = SystemClock.uptimeMillis();
        String key = storageKey(it);
        if (!key.equals(storageArmedKey) || now - deleteArmedAt > CONFIRM_MS) {
            storageArmedKey = key;
            deleteArmedAt = now;
            row.setBadge(getString(R.string.DELETE_CONFIRM_BADGE), Theme.WARN);
            setNote(getString(it.kind == Storage.Kind.SAVES ? R.string.LIB_ST_SAVES_CONFIRM
                                                             : R.string.LIB_ST_DELETE_CONFIRM),
                    it.kind == Storage.Kind.SAVES ? NOTE_ERROR : NOTE_WARN);
            return;
        }
        storageArmedKey = null;
        deleteArmedAt = 0;
        deleting = true;
        row.setBadge(getString(R.string.DELETING), Theme.TEXT_DIM);
        setNote(getString(R.string.DELETING), NOTE_INFO);
        if (gameRunning() && it.tid.equals(launchedTid()))
            endRunningGame();
        Storage.Dirs dirs = storageDirs();
        Log.i(TAG, "launcher: storage: deleting " + key + " " + it.files);
        new Thread(() -> {
            Storage.delete(dirs, it, null);
            runOnUiThread(() -> {
                deleting = false;
                storageSizes.remove(key);
                if (it.kind == Storage.Kind.DATA || it.kind == Storage.Kind.IMAGE) {
                    String dir = it.files.get(0).getAbsolutePath();
                    for (Library.Record r : games.games.values()) {
                        if (dir.equals(r.dataDir)) {
                            r.dataDir = "";
                            r.dataKind = "";
                        }
                    }
                    writeGames();
                    if (dir.equals(GameStore.chosenDir(this)))
                        GameStore.forget(this);
                    if (!indexGames.containsKey(it.tid))
                        GameTheme.clear(this, it.tid);
                    libArt.remove(it.tid);
                    sizeDir = null;
                    hashDir = null;
                    titleIdDir = null;
                    artTid = null;
                }
                if (screen == Screen.GAME_DATA && (it.kind == Storage.Kind.DATA || it.kind == Storage.Kind.IMAGE)) {
                    history.clear();
                    show(libraryOn() ? (rec() != null ? Screen.HOME : Screen.LIBRARY)
                                     : state() == State.NONE ? Screen.WELCOME : Screen.HOME);
                } else {
                    refreshKeepingFocus();
                }
                setNote(getString(R.string.DELETED), NOTE_OK);
            });
        }, "xba-delete").start();
    }

    // ---- ABOUT -------------------------------------------------------------------------------------------------

    private void buildAbout() {
        screenTitle.setText(R.string.ABOUT);
        LinearLayout h = rowLayout();
        LinearLayout left = column();
        left.setGravity(Gravity.CENTER_VERTICAL);
        Widgets.BrandMark mark = new Widgets.BrandMark(this);
        left.addView(mark, lp(120, 120));
        TextView name = Theme.text(this, Theme.T_DISPLAY, Theme.MEDIUM, Theme.TEXT, getString(R.string.app_name));
        LinearLayout.LayoutParams nl = lp(MATCH, WRAP);
        nl.topMargin = Theme.S4;
        left.addView(name, nl);
        TextView tag = Theme.text(this, Theme.T_BODY, Theme.REGULAR, Theme.TEXT_DIM, getString(R.string.ABOUT_TAGLINE));
        LinearLayout.LayoutParams gl = lp(MATCH, WRAP);
        gl.topMargin = Theme.S2;
        left.addView(tag, gl);
        TextView t = Theme.body(this, getString(R.string.ABOUT_TEXT));
        LinearLayout.LayoutParams tl = lp(MATCH, WRAP);
        tl.topMargin = Theme.S5;
        left.addView(t, tl);
        h.addView(left, lp(0, MATCH, 1));

        LinearLayout right = column();
        right.setGravity(Gravity.CENTER_VERTICAL);
        String sha = GameStore.expectedSha256(this);
        String facts = TextUtils.join("\n", new String[] {
            kv(R.string.KV_VERSION, version()), kv(R.string.KV_BUILD, getString(R.string.xba_version_code)),
            kv(R.string.KV_APP_ID, getPackageName()),
            kv(R.string.KV_GAME_CHECK, sha.isEmpty() ? getString(R.string.GAME_CHECK_NONE) : sha.substring(0, 16) + "…"),
            kv(R.string.KV_LICENCE, getString(R.string.ABOUT_LICENCE)) });
        right.addView(Theme.body(this, facts), lp(MATCH, WRAP));
        TextView legal = Theme.body(this, getString(R.string.ABOUT_LEGAL));
        LinearLayout.LayoutParams ll = lp(MATCH, WRAP);
        ll.topMargin = Theme.S5;
        right.addView(legal, ll);
        LinearLayout.LayoutParams rl = lp(Theme.DESC_W + Theme.S6, MATCH);
        rl.leftMargin = Theme.S6;
        h.addView(right, rl);
        body.addView(h, new FrameLayout.LayoutParams(MATCH, MATCH));
        xAction = () -> go(Screen.LICENCES);
        addHint("X", R.string.LICENCES, xAction);
        addHint("B", R.string.HINT_BACK, this::back);
    }

    // ---- LICENCES ----------------------------------------------------------------------------------------------

    /** The licence texts in the order the screen shows them; the rest of assets/licences/ follows, sorted. */
    static final String[] LICENCE_ORDER = { "LICENSE.txt", "NOTICE.txt" };

    /** The Licences screen's scroller: the D-pad scrolls it (dispatchKeyEvent), L1 / R1 page. Null elsewhere. */
    private ScrollView licencesScroll;

    /**
     * Every licence text the APK carries (assets/licences/: LICENSE, NOTICE, LICENSES/*.txt, copied by the build),
     * one after the other under their file names; a short note when the folder is missing (a build without it).
     */
    static String licencesText(android.content.res.AssetManager assets) {
        List<String> names = new ArrayList<>();
        try {
            String[] all = assets.list("licences");
            if (all != null)
                names.addAll(java.util.Arrays.asList(all));
        } catch (java.io.IOException e) {
            Log.w(TAG, "licences: " + e);
        }
        java.util.Collections.sort(names);
        List<String> order = new ArrayList<>();
        for (String n : LICENCE_ORDER)
            if (names.remove(n))
                order.add(n);
        order.addAll(names);
        StringBuilder b = new StringBuilder();
        for (String n : order) {
            try (java.io.InputStream in = assets.open("licences/" + n)) {
                java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
                byte[] buf = new byte[8192];
                int k;
                while ((k = in.read(buf)) > 0)
                    out.write(buf, 0, k);
                if (b.length() > 0)
                    b.append("\n\n");
                b.append("──── ").append(n.replaceAll("\\.txt$", "")).append(" ────\n\n");
                b.append(new String(out.toByteArray(), java.nio.charset.StandardCharsets.UTF_8).trim());
            } catch (java.io.IOException e) {
                Log.w(TAG, "licences: " + n + ": " + e);
            }
        }
        return b.toString();
    }

    /** About › Licences: the texts, monospaced (NOTICE is laid out in columns), scrolled with the D-pad or a finger. */
    private void buildLicences() {
        screenTitle.setText(R.string.LICENCES);
        String text = licencesText(getAssets());
        ScrollView scroll = new ScrollView(this);
        scroll.setVerticalScrollBarEnabled(true);
        scroll.setPadding(0, 0, 0, Theme.S3);
        scroll.setClipToPadding(false);
        TextView t = Theme.text(this, Theme.T_SMALL, Theme.REGULAR, Theme.TEXT_DIM,
                                text.isEmpty() ? getString(R.string.LICENCES_MISSING) : text);
        t.setTypeface(android.graphics.Typeface.MONOSPACE);
        t.setTextIsSelectable(false);
        scroll.addView(t, new FrameLayout.LayoutParams(MATCH, WRAP));
        body.addView(scroll, new FrameLayout.LayoutParams(MATCH, MATCH));
        licencesScroll = scroll;
        l1Action = () -> scroll.smoothScrollBy(0, -scroll.getHeight() * 9 / 10);
        r1Action = () -> scroll.smoothScrollBy(0, scroll.getHeight() * 9 / 10);
        addHint("B", R.string.HINT_BACK, this::back);
    }

    /** What the Licences screen shows (tests). */
    String licencesForTest() {
        return licencesScroll == null ? null : ((TextView)licencesScroll.getChildAt(0)).getText().toString();
    }

    private String version() {
        return getString(R.string.xba_version);
    }

    // ---- Game state --------------------------------------------------------------------------------------------

    /**
     * The selected library record when it stands for a game Home can show: its files are set (present or not: a
     * missing folder is « Game data missing »), or it has a source in the library folder. Null otherwise, and the
     * launcher falls back to the one-game rules (the remembered folder, recomp.cfg, the default game/).
     */
    private Library.Record rec() {
        Library.Record r = games.selected();
        if (r == null)
            return null;
        if (!r.dataDir.isEmpty())
            return r;
        return indexGames.containsKey(r.titleId) ? r : null;
    }

    /**
     * A game is ready when the selected game's folder holds default.xbe, or, with nothing remembered, when
     * recomp.cfg sets RECOMP_GAME_DIR (the native side then decides) or default.xbe was pushed to the default game/
     * folder.
     */
    private boolean gameReady() {
        Library.Record r = rec();
        if (r != null)
            return !r.dataDir.isEmpty()
                   && (Library.KIND_CFG.equals(r.dataKind) || GameStore.findXbe(new File(r.dataDir)) != null);
        String dir = GameStore.chosenDir(this);
        if (dir != null)
            return GameStore.findXbe(new File(dir)) != null;
        if (GameStore.cfgGameDir(this) != null)
            return true;
        return GameStore.findXbe(GameStore.defaultGameDir(this)) != null;
    }

    /** The folder the selected game is read from, or null when none is set up (or its files are not here). */
    private String gameDir() {
        Library.Record r = rec();
        if (r != null)
            return r.dataDir.isEmpty() ? null : r.dataDir;
        return legacyDir();
    }

    /** The one-game rule: the remembered folder, else RECOMP_GAME_DIR, else game/ when it holds default.xbe. */
    private String legacyDir() {
        String dir = GameStore.chosenDir(this);
        if (dir != null)
            return dir;
        String cfgDir = GameStore.cfgGameDir(this);
        if (cfgDir != null)
            return cfgDir;
        File def = GameStore.defaultGameDir(this);
        return GameStore.findXbe(def) != null ? def.getAbsolutePath() : null;
    }

    /** The folder the game process runs (or last ran): the preferences, as ShellActivity reads them. */
    private String launchedDir() {
        String dir = GameStore.chosenDir(this);
        if (dir != null)
            return dir;
        String cfgDir = GameStore.cfgGameDir(this);
        if (cfgDir != null)
            return cfgDir;
        File def = GameStore.defaultGameDir(this);
        return def == null ? null : def.getAbsolutePath();
    }

    /** The selected game is the one the process runs or last ran. */
    private boolean selectedIsLaunched() {
        if (GameStore.launchedRom(this) != null)
            return false;   // a GBA ROM runs (or ran) last: never the selected Xbox game
        String d = gameDir();
        return d != null && d.equals(launchedDir());
    }

    /** The title ID of the game the process runs or last ran, or null. */
    private String launchedTid() {
        String t = GameStore.launchedTitleId(this);
        if (t != null)
            return t;
        String d = launchedDir();
        return d == null ? null : Pack.xbeTitleId(GameStore.findXbe(new File(d)));
    }

    private State state() {
        Library.Record r = rec();
        String dir;
        if (r != null) {
            if (r.dataDir.isEmpty())
                return State.LIBRARY;
            dir = r.dataDir;
            if (!Library.KIND_CFG.equals(r.dataKind) && GameStore.findXbe(new File(dir)) == null)
                return missingState(dir);
        } else {
            dir = GameStore.chosenDir(this);
            if (dir != null) {
                if (GameStore.findXbe(new File(dir)) == null)
                    return missingState(dir);
            } else if (!gameReady()) {
                return State.NONE;
            }
        }
        String d = gameDir();
        return d != null && d.equals(hashDir) && Boolean.FALSE.equals(hashOk) ? State.WRONG : State.READY;
    }

    /** A folder without default.xbe: unreadable (no All files access, outside the app) or gone. */
    private State missingState(String dir) {
        File ext = GameStore.externalFilesDir(this);
        boolean outside = ext == null || !dir.startsWith(ext.getAbsolutePath());
        return new File(dir).exists() && !Environment.isExternalStorageManager() && outside ? State.NO_ACCESS
                                                                                            : State.MISSING;
    }

    private static int stateLabel(State s) {
        switch (s) {
        case READY:
            return R.string.STATE_READY;
        case MISSING:
            return R.string.STATE_MISSING;
        case NO_ACCESS:
            return R.string.STATE_NO_ACCESS;
        case WRONG:
            return R.string.STATE_WRONG;
        case LIBRARY:
            return R.string.LIB_CHIP_NOT_IMPORTED;
        default:
            return R.string.STATE_NONE;
        }
    }

    private static int stateColor(State s) {
        return s == State.READY ? Theme.OK : s == State.WRONG ? Theme.ERROR : Theme.WARN;
    }

    private String stateText(State s, String dir) {
        switch (s) {
        case MISSING:
            return getString(R.string.GAME_FOLDER_GONE, dir);
        case NO_ACCESS:
            return getString(R.string.GAME_FOLDER_NO_ACCESS, dir);
        case WRONG:
            return getString(R.string.WRONG_GAME_HOME);
        case NONE:
            return getString(R.string.NO_GAME_SET_UP);
        default:
            return "";
        }
    }

    private String gameTitle(String dir) {
        Library.Record r = rec();
        if (r != null && !r.title.isEmpty() && (dir == null ? r.dataDir.isEmpty() : dir.equals(r.dataDir)))
            return r.title;
        String t = GameStore.chosenTitle(this);
        if (dir != null && dir.equals(GameStore.chosenDir(this)) && !t.isEmpty())
            return t;
        if (dir == null)
            return getString(R.string.app_name);
        File xbe = GameStore.findXbe(new File(dir));
        String x = xbe == null ? "" : GameStore.xbeTitle(xbe);
        return x.isEmpty() ? new File(dir).getName() : x;
    }

    private Storage.Dirs storageDirs() {
        return new Storage.Dirs(getFilesDir(), GameStore.externalFilesDir(this));
    }

    /** A copy this app made (games/<TID>, or the older game/): the only game files it ever deletes. */
    private boolean ownedCopy(String dir) {
        return Storage.ownedCopy(storageDirs(), dir) && new File(dir).isDirectory();
    }

    /** The selected game came from recomp.cfg's RECOMP_GAME_DIR. */
    private boolean fromCfg() {
        Library.Record r = rec();
        if (r != null)
            return Library.KIND_CFG.equals(r.dataKind);
        return GameStore.chosenDir(this) == null && GameStore.cfgGameDir(this) != null;
    }

    private String sourceLabel(String dir) {
        DiscImages.Disc disc = DiscImages.read(new File(dir));
        if (disc != null)   // XBA-126: the image itself, read in place
            return getString(R.string.SOURCE_IMAGE, DiscImages.label(disc), disc.kind, size(disc.stored));
        if (ownedCopy(dir))
            return getString(R.string.SOURCE_IMPORTED);
        if (fromCfg())
            return getString(R.string.SOURCE_CFG);
        return getString(R.string.SOURCE_IN_PLACE);
    }

    private String sizeText(String dir) {
        return dir.equals(sizeDir) && sizeBytes >= 0 ? size(sizeBytes) : getString(R.string.SIZE_COUNTING);
    }

    /** The folder's size, counted once per folder on a background thread; the screen is rebuilt when it is known. */
    private void sizeLater(String dir) {
        if (dir == null || dir.equals(sizeDir))
            return;
        sizeDir = dir;
        sizeBytes = -1;
        new Thread(() -> {
            long n = GameStore.treeSize(new File(dir));
            runOnUiThread(() -> {
                if (!dir.equals(sizeDir))
                    return;
                sizeBytes = n;
                if (screen == Screen.HOME || screen == Screen.GAME_DATA)
                    refreshKeepingFocus();
            });
        }, "xba-size").start();
    }

    /**
     * WRONG_GAME on HOME: default.xbe of the current folder against the hash this APK was built for, once per folder
     * on a background thread (a picked game was checked already; this catches files changed since, or a folder set
     * up through recomp.cfg or adb). No expected hash (the shell, the stub): nothing to check.
     */
    private void checkHashLater(String dir) {
        if (dir == null || dir.equals(hashDir) || GameStore.expectedSha256(this).isEmpty())
            return;
        hashDir = dir;
        hashOk = null;
        new Thread(() -> {
            Boolean ok = null;
            File xbe = GameStore.findXbe(new File(dir));
            if (xbe != null) {
                try {
                    String sha = GameStore.sha256(xbe);
                    ok = GameStore.hashMatches(this, sha);
                    Log.i(TAG, "launcher: " + xbe + " sha256 " + sha + (ok ? "" : " (WRONG_GAME)"));
                } catch (IOException e) {
                    Log.w(TAG, "launcher: cannot hash " + xbe, e);
                }
            }
            Boolean result = ok;
            runOnUiThread(() -> {
                if (!dir.equals(hashDir))
                    return;
                hashOk = result;
                if (Boolean.FALSE.equals(result) && screen == Screen.HOME)
                    refreshKeepingFocus();
            });
        }, "xba-hash").start();
    }

    /** Rebuilds the current screen (a fact computed in the background arrived), keeping its focus and note. */
    private void refreshKeepingFocus() {
        String text = noteText;
        int kind = noteKind;
        show(screen, 0);
        noteText = text;
        noteKind = kind;
        paintNote();
    }

    // ---- Shortcuts -----------------------------------------------------------------------------------------------

    /** Long-press on the app icon: « Change game » and « Launcher ». Dynamic: the package name differs per title. */
    private void publishShortcuts() {
        try {
            ShortcutManager sm = getSystemService(ShortcutManager.class);
            if (sm == null)
                return;
            int flags = Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK;
            ShortcutInfo change = new ShortcutInfo.Builder(this, "change_game")
                    .setShortLabel(getString(R.string.SHORTCUT_CHANGE_GAME))
                    .setLongLabel(getString(R.string.SHORTCUT_CHANGE_GAME_LONG))
                    .setIntent(new Intent(this, LauncherActivity.class).setAction(ACTION_CHOOSE_GAME).addFlags(flags))
                    .setRank(0)
                    .build();
            ShortcutInfo home = new ShortcutInfo.Builder(this, "launcher")
                    .setShortLabel(getString(R.string.SHORTCUT_LAUNCHER))
                    .setLongLabel(getString(R.string.SHORTCUT_LAUNCHER_LONG))
                    .setIntent(new Intent(this, LauncherActivity.class).setAction(ACTION_SHOW_LAUNCHER)
                                       .addFlags(flags))
                    .setRank(1)
                    .build();
            List<ShortcutInfo> list = new ArrayList<>();
            if (TitleApp.ownTitleId(this) == null)
                list.add(change);   // a game's own app is locked to its disc: no « Change game »
            list.add(home);
            sm.setDynamicShortcuts(list);
        } catch (RuntimeException e) {
            Log.w(TAG, "launcher: could not publish the shortcuts", e);
        }
    }

    // ---- Starting the game ----------------------------------------------------------------------------------

    // ---- Prepare again (XBA-74, docs/android/prepare.md « Prepare again ») ---------------------------------------

    private File workDir(String tid) {
        return new File(new File(getFilesDir(), "work"), tid);
    }

    /**
     * The self-healing state of tid when it has something to show on Home, else null: the last run of tid ended (no
     * game process now) on a crash, with new code found (« Prepare again », only when Prepare is possible) or not (the
     * title needs a fix).
     */
    private SelfHeal.State selfHeal(String tid, boolean running, Prep prep) {
        File work = workDir(tid);
        if (!running && mainProcessPid() == 0 && tid.equals(GameStore.launchedTitleId(this))) {
            try {
                if (SelfHeal.onExit(work, lastGameExit())) {
                    SelfHeal.State s = SelfHeal.read(work);
                    Log.i(TAG, "launcher: " + tid + "'s last run: " + s.offer
                               + (s.blocker.isEmpty() ? "" : " (" + s.blocker + ")") + ", rounds " + s.rounds);
                }
            } catch (IOException e) {
                Log.w(TAG, "launcher: cannot update " + tid + "'s selfheal.json", e);
            }
        }
        SelfHeal.State s = SelfHeal.read(work);
        if (s.offer == SelfHeal.Offer.NONE || (s.offer != SelfHeal.Offer.NEEDS_FIX && prep != Prep.PLAYABLE))
            return null;
        return s;
    }

    /** How the game's process (the main one) last ended, from the system's record; null when it has none. */
    private SelfHeal.Exit lastGameExit() {
        ActivityManager am = getSystemService(ActivityManager.class);
        if (am == null)
            return null;
        List<android.app.ApplicationExitInfo> exits;
        try {
            exits = am.getHistoricalProcessExitReasons(getPackageName(), 0, 16);
        } catch (RuntimeException e) {
            return null;
        }
        for (android.app.ApplicationExitInfo x : exits) {   // newest first
            if (!getPackageName().equals(x.getProcessName()))
                continue;
            int r = x.getReason();
            boolean crashed = r == android.app.ApplicationExitInfo.REASON_CRASH
                              || r == android.app.ApplicationExitInfo.REASON_CRASH_NATIVE
                              || r == android.app.ApplicationExitInfo.REASON_ANR;
            String why = x.getDescription();
            if (why == null || why.isEmpty())
                why = r == android.app.ApplicationExitInfo.REASON_CRASH_NATIVE ? "native crash"
                      : r == android.app.ApplicationExitInfo.REASON_ANR ? "not responding"
                      : r == android.app.ApplicationExitInfo.REASON_CRASH ? "Java exception" : "";
            return new SelfHeal.Exit(x.getTimestamp(), crashed, why);
        }
        return null;
    }

    /** « Prepare again »: one more round; the game starts again when the Prepare is done. */
    private void onPrepareAgain() {
        String tid = titleId();
        if (tid == null)
            return;
        try {
            SelfHeal.prepareAgain(workDir(tid));
        } catch (IOException e) {
            Log.w(TAG, "launcher: cannot update " + tid + "'s selfheal.json", e);
        }
        Log.i(TAG, "launcher: prepare again " + tid);
        startPrepare();
        if (screen == Screen.PREPARE)
            setNote(getString(R.string.SELF_HEAL_ROUND_NOTE, SelfHeal.read(workDir(tid)).rounds), NOTE_INFO);
    }

    /**
     * XBA-129, the setting on: the round Home offers starts by itself (Home may be built several times before this
     * runs: only an offer still standing starts one).
     */
    private void autoRound(String tid) {
        if (tid == null || !tid.equals(titleId()) || screen != Screen.HOME)
            return;
        SelfHeal.Offer o = SelfHeal.read(workDir(tid)).offer;
        Log.i(TAG, "launcher: self-healing " + tid + ": " + o);
        if (o == SelfHeal.Offer.PREPARE_AGAIN)
            onPrepareAgain();
        else if (o == SelfHeal.Offer.IMPROVE)
            onImprove();
    }

    /** XBA-129: a run that ended well found code the analysis missed: a round in the background, Play stays. */
    private void onImprove() {
        String tid = titleId();
        if (tid == null || prep() != Prep.PLAYABLE)
            return;
        try {
            SelfHeal.startRound(workDir(tid), true);
        } catch (IOException e) {
            Log.w(TAG, "launcher: cannot update " + tid + "'s selfheal.json", e);
        }
        Log.i(TAG, "launcher: self-healing round in the background for " + tid);
        startPrepare(true);
    }

    /** The background round's Stop: its Prepare is cancelled; the pack it would have replaced stays. */
    private void onStopHealing() {
        String tid = titleId();
        if (tid == null)
            return;
        Log.i(TAG, "launcher: self-healing round stopped for " + tid);
        try {
            startService(PrepareService.cancelIntent(this, tid));
        } catch (RuntimeException e) {
            Log.w(TAG, "launcher: cannot stop the Prepare", e);
        }
        setNote(getString(R.string.SELF_HEAL_STOPPED), NOTE_INFO);
    }

    /** The status of tid's background Prepare while it runs (XBA-129), else null. */
    private PrepareProtocol.Status backgroundPrepare(String tid) {
        PrepareProtocol.Status s = tid == null ? null
                                   : PrepareProtocol.readStatus(PrepareService.statusFile(this, tid));
        return s != null && s.background && statusAlive(s) ? s : null;
    }

    /**
     * A Prepare of tid ended and its pack is valid: when it was a crash round, count the round (SelfHeal.roundEnded:
     * a round that accepted no new seed stops the loop) and start the game.
     */
    private boolean relaunchAfterPrepare(String tid) {
        if (tid != null && imageAfterPrepare(tid))
            return true;
        if (tid == null || !SelfHeal.read(workDir(tid)).roundOpen)
            return false;
        PrepareProtocol.Status s = readPrepStatus();
        if (s != null && s.background)
            return false;   // PrepareService ends a background round
        boolean go;
        try {
            go = SelfHeal.roundEnded(workDir(tid), s != null && PrepareProtocol.DONE.equals(s.state));
        } catch (IOException e) {
            Log.w(TAG, "launcher: cannot update " + tid + "'s selfheal.json", e);
            return false;
        }
        if (!go || state() != State.READY) {
            Log.i(TAG, "launcher: " + tid + "'s round ended without a relaunch ("
                       + SelfHeal.read(workDir(tid)).offer + ")");
            return false;
        }
        Log.i(TAG, "launcher: prepared again, relaunching " + tid);
        launchShell(false);
        return true;
    }

    /** The image the game launched that is not prepared yet (PendingImage, XBA-82), for the selected title. */
    private String pendingImage() {
        String tid = titleId();
        return tid == null ? null : PendingImage.pending(workDir(tid));
    }

    /**
     * « Prepare IMAGE » (XBA-82): the image goes into images.want and the title's Prepare runs (the same queue); when
     * it is done, the game starts again in that image (imageAfterPrepare).
     */
    private void onPrepareImage() {
        String tid = titleId(), img = pendingImage();
        if (tid == null || img == null)
            return;
        try {
            PendingImage.want(workDir(tid), img);
        } catch (IOException e) {
            Log.w(TAG, "launcher: cannot write " + tid + "'s images.want", e);
            setNote(getString(R.string.PREPARE_START_FAILED, String.valueOf(e.getMessage())), NOTE_ERROR);
            return;
        }
        Log.i(TAG, "launcher: prepare " + img + " of " + tid);
        if (direct)
            directPrepare();
        else
            startPrepare();
    }

    /**
     * A Prepare of tid ended with a valid pack while an image of its disc waits (XBA-82): prepared, the game starts
     * again in it (true); left out by the Prepare, it is dropped with its launch data and Home says so (false).
     */
    private boolean imageAfterPrepare(String tid) {
        String img = PendingImage.pending(workDir(tid));
        if (img == null || state() != State.READY)
            return false;
        if (PendingImage.built(Pack.packDir(getFilesDir(), tid), img)) {
            Log.i(TAG, "launcher: " + img + " of " + tid + " prepared, going on into it");
            launchShell(false);
            return true;
        }
        PrepareProtocol.Status s = readPrepStatus();
        if (s != null && s.background)
            return false;   // a self-healing round's pack: the image waits for its own Prepare
        Log.w(TAG, "launcher: " + img + " of " + tid + " not in the new pack, dropped");
        PendingImage.drop(workDir(tid), GameTheme.saveDir(this));
        setNote(getString(R.string.IMAGE_NOT_PREPARED, img), NOTE_WARN);
        return false;
    }

    /** The main process, where ShellActivity and the runtime live, when it is running. */
    private int mainProcessPid() {
        ActivityManager am = getSystemService(ActivityManager.class);
        List<ActivityManager.RunningAppProcessInfo> procs = am == null ? null : am.getRunningAppProcesses();
        if (procs == null)
            return 0;
        for (ActivityManager.RunningAppProcessInfo p : procs) {
            if (p.processName.equals(getPackageName()))
                return p.pid;
        }
        return 0;
    }

    /** A task of this app whose activity is ShellActivity (the game's window). */
    private boolean shellAlive() {
        ActivityManager am = getSystemService(ActivityManager.class);
        if (am == null)
            return false;
        for (ActivityManager.AppTask t : am.getAppTasks()) {
            try {
                ActivityManager.RecentTaskInfo info = t.getTaskInfo();
                if (info.numActivities > 0 && info.baseActivity != null
                    && info.baseActivity.getClassName().equals(ShellActivity.class.getName()))
                    return true;
            } catch (IllegalArgumentException ignored) {   // the task went away meanwhile
            }
        }
        return false;
    }

    private boolean gameRunning() {
        return mainProcessPid() != 0 && shellAlive();
    }

    /**
     * Ends the game's process, if any: after a new choice (the runtime maps its memory once per process, so the new
     * game needs a fresh one) and before deleting its files. Saves are in internal storage and stay.
     */
    private void endRunningGame() {
        int pid = mainProcessPid();
        if (pid == 0)
            return;
        Log.i(TAG, "launcher: ending the running game (pid " + pid + ")");
        Process.killProcess(pid);
        long until = SystemClock.uptimeMillis() + 2000;
        while (mainProcessPid() != 0 && SystemClock.uptimeMillis() < until)
            SystemClock.sleep(50);
    }

    /**
     * Starts ShellActivity for the selected game. A live game with no new choice is only brought to the front; a
     * main process without a live ShellActivity (SDL_main returned, e.g. after NO_GAME_DATA) cannot start the
     * runtime again, so it is ended first (restart = true does the same unconditionally). Another game than the one
     * the process runs: that process is ended, the selected game becomes the launched one (GameStore.rememberLaunch,
     * the only place it is written) and the shared caches switch owner (SaveCaches).
     */
    private void launchShell(boolean restart) {
        boolean other = !selectedIsLaunched() && gameDir() != null;
        boolean fromRom = GameStore.launchedRom(this) != null;   // the process runs a GBA ROM: never this game
        int pid = mainProcessPid();
        if (pid != 0 && !restart && (other || fromRom || !shellAlive())) {
            Log.i(TAG, "launcher: main process " + pid + (other || fromRom ? " runs another game"
                                                                           : " has no game window")
                       + ", restarting it");
            restart = true;
        }
        if (restart)
            endRunningGame();
        String tid = titleId();
        Library.Record r = rec();
        if (other || fromRom || (r != null && tid != null && !tid.equals(GameStore.launchedTitleId(this)))) {
            String dir = gameDir();
            boolean cfg = r != null && Library.KIND_CFG.equals(r.dataKind);
            GameStore.rememberLaunch(this, cfg ? null : dir, gameTitle(dir), tid);
            Log.i(TAG, "launcher: launched game set: " + dir + " (" + tid + ")");
        }
        String owner = tid != null ? tid : r != null ? r.titleId : null;
        boolean resume = pid != 0 && !restart;   // a live game brought back: its play session goes on
        if (owner != null && !resume) {   // the caches move only while no game process uses them
            try {
                if (SaveCaches.switchTo(GameTheme.saveDir(this), Library.cacheOwnerFile(getFilesDir()), owner))
                    Log.i(TAG, "launcher: game caches switched to " + owner);
            } catch (IOException e) {
                Log.w(TAG, "launcher: cannot switch the game caches to " + owner, e);
            }
        }
        if (!resume && owner != null) {   // XBA-82: on into an image prepared since it was launched, or not at all
            String img = PendingImage.beforeLaunch(workDir(owner), Pack.packDir(getFilesDir(), owner),
                                                   GameTheme.saveDir(this));
            if (img != null)
                Log.i(TAG, "launcher: the game goes on in " + img);
        }
        if (!resume) {
            long now = System.currentTimeMillis();
            closeOpenSession(now);   // a game left in the background and replaced: its time counts too
            GameStore.sessionStart(this, now, owner);
            if (owner != null) {
                try {   // the next exit is this run's (XBA-74)
                    SelfHeal.launched(workDir(owner), now);
                } catch (IOException e) {
                    Log.w(TAG, "launcher: cannot update " + owner + "'s selfheal.json", e);
                }
            }
            if (owner != null) {
                Library.sessionStart(games, owner, now);
                writeGames();
            }
        }
        Log.i(TAG, "launcher: play");
        Intent i = new Intent(this, ShellActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        if (restart)
            i.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(i);
        finish();
    }

    /** The app icon while the game runs: back to it as it is (whatever game the library shows). */
    private void resumeShell() {
        startActivity(new Intent(this, ShellActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        finish();
    }

    /**
     * Play while another game runs: the first press says it will close (the button and the note), a second within CONFIRM_MS
     * goes on. True when the launch may go on.
     */
    private boolean confirmSwitch() {
        if (selectedIsLaunched() || !gameRunning())
            return true;
        long now = SystemClock.uptimeMillis();
        if (switchArmedAt != 0 && now - switchArmedAt <= CONFIRM_MS) {
            switchArmedAt = 0;
            Log.i(TAG, "launcher: switching games, ending " + runningTitle());
            return true;
        }
        switchArmedAt = now;
        if (homePrimary != null)
            homePrimary.setTitle(getString(R.string.PREPARE_CANCEL_BADGE));
        setNote(getString(R.string.LIB_SWITCH_CONFIRM, runningTitle()), NOTE_WARN);
        return false;
    }

    /** The running (or last launched) game's title, for the switch warning. */
    private String runningTitle() {
        if (GameStore.launchedRom(this) != null && !GameStore.launchedRomTitle(this).isEmpty())
            return GameStore.launchedRomTitle(this);
        String t = GameStore.chosenTitle(this);
        String tid = launchedTid();
        Library.Record r = tid == null ? null : games.get(tid);
        if (r != null && !r.title.isEmpty())
            return r.title;
        return t.isEmpty() ? getString(R.string.LIB_ANOTHER_GAME) : t;
    }

    /**
     * EXTRA_TITLE_EXITED: the game's process ends by itself once the shell has asked for this screen (the kernel's
     * recomp_quit: logs flushed, _exit). It is given TITLE_EXIT_WAIT_MS, then ended, so that Home sees no running game
     * (play session closed, the exit record read as a clean one, not a crash).
     */
    private void awaitGameExit() {
        long until = SystemClock.uptimeMillis() + TITLE_EXIT_WAIT_MS;
        while (mainProcessPid() != 0 && SystemClock.uptimeMillis() < until)
            SystemClock.sleep(50);
        if (mainProcessPid() != 0) {
            Log.w(TAG, "launcher: the game said it exited but its process is still there");
            endRunningGame();
        }
    }

    /** Home's note after the title left by itself: « The game ended: <the kernel's reason>. » */
    static String titleExitedNote(Context c, String why) {
        String w = why == null ? "" : why.trim();
        return w.isEmpty() ? c.getString(R.string.TITLE_EXITED_PLAIN) : c.getString(R.string.TITLE_EXITED, w);
    }

    /**
     * ACTION_RESTART_GAME: the runtime cannot start a title twice in one process (it maps its memory once), so the
     * game's process is ended here and ShellActivity starts in a new one, which takes the launch data back. Nothing
     * is shown and the play session is not closed: to the player it is the game going back to its own menu.
     */
    private void restartGame() {
        Log.i(TAG, "launcher: the game asked for a restart (quick reboot)");
        endRunningGame();
        startActivity(new Intent(this, ShellActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK));
        finish();
        overridePendingTransition(0, 0);
    }

    /**
     * A new game was checked (picked, imported, copied): it becomes a library record and the selected game, then
     * HOME with Play focused. The launched game (the preferences) changes only when it is played (launchShell).
     * After an import from the library, a Prepare that is needed starts at once.
     */
    private void onGameChosen(String dir, String title) {
        File d = new File(dir);
        String tid = Pack.xbeTitleId(GameStore.findXbe(d));
        if (tid == null)
            tid = Library.localKey(d.getAbsolutePath());
        long now = System.currentTimeMillis();
        Library.Record r = games.get(tid);
        if (r == null) {
            r = new Library.Record();
            r.titleId = tid;
            r.addedAt = now;
            games.put(r);
        }
        if (!title.isEmpty())
            r.title = title;
        else if (r.title.isEmpty())
            r.title = d.getName();
        r.dataDir = d.getAbsolutePath();
        r.dataKind = DiscImages.isCompanion(dir) ? Library.KIND_IMAGE
                   : Storage.ownedCopy(storageDirs(), dir) ? Library.KIND_IMPORTED : Library.KIND_IN_PLACE;
        if (Library.KIND_IN_PLACE.equals(r.dataKind) && r.source.isEmpty())
            r.source = LibraryIndex.fileLoc(d);
        if (chosenSha != null)
            r.xbeSha256 = chosenSha;
        games.selected = tid;
        writeGames();
        boolean fromLibrary = importFromLibrary;
        importFromLibrary = false;
        importTid = null;
        titleIdDir = null;   // a new game may sit in the same folder: read its title ID again
        artTid = null;
        libArt.remove(tid);
        Log.i(TAG, "launcher: game set: " + dir + " (" + title + ", " + tid + ")");
        hashDir = r.dataDir;
        hashOk = Boolean.TRUE;
        sizeDir = null;
        history.clear();
        if (direct) {   // a game's own app: on into the game (its pack, then play)
            directSourceFailed = false;
            directStep(true);
            return;
        }
        if (libraryOn())
            history.push(Screen.LIBRARY);
        focusMemory.remove(Screen.HOME);
        show(Screen.HOME);
        feedback.play(UiFeedback.Cue.DONE, host);
        setNote(getString(R.string.READY_TO_PLAY, title.isEmpty() ? d.getName() : title), NOTE_OK);
        if (fromLibrary && prep() == Prep.NEEDED)
            startPrepare();
    }

    // ---- Folder --------------------------------------------------------------------------------------------

    private void onChooseFolder() {
        pickLibrary = false;
        if (Environment.isExternalStorageManager())
            openTreePicker();
        else
            go(Screen.FOLDER_ACCESS);
    }

    /**
     * « Game library folder »: the tree picker; without All files access the FOLDER_ACCESS screen first (granted:
     * the folder is read as files; otherwise through the Storage Access Framework, ISOs imported, folders copied).
     */
    private void onChooseLibraryFolder() {
        pickLibrary = true;
        if (Environment.isExternalStorageManager())
            openTreePicker();
        else
            go(Screen.FOLDER_ACCESS);
    }

    private void askAllFilesAccess() {
        Intent i = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                              Uri.parse("package:" + getPackageName()));
        try {
            startActivityForResult(i, REQ_ALL_FILES);
        } catch (ActivityNotFoundException e) {
            try {
                startActivityForResult(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION), REQ_ALL_FILES);
            } catch (ActivityNotFoundException e2) {
                openTreePicker();
            }
        }
    }

    private void openTreePicker() {
        try {
            startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), pickLibrary ? REQ_LIBRARY_TREE
                                                                                             : REQ_TREE);
        } catch (ActivityNotFoundException e) {
            setNote(getString(R.string.PICKER_MISSING), NOTE_ERROR);
        }
    }

    private void onTreePicked(Uri tree) {
        String path = Environment.isExternalStorageManager() ? GameStore.treeUriToPath(tree) : null;
        Log.i(TAG, "launcher: folder " + tree + (path != null ? " = " + path : " (copy)"));
        File dir = path == null ? null : new File(path);
        if (dir != null && dir.list() != null) {
            if (GameStore.findXbe(dir) == null)
                setNote(getString(R.string.NOT_A_GAME_FOLDER), NOTE_ERROR);
            else
                runWorker(R.string.CHECKING, () -> checkInPlace(dir));
            return;
        }
        // No All files access, a provider that is not a storage volume, or a path we cannot list: copy.
        try {
            getContentResolver().takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException ignored) {
        }
        runWorker(R.string.COPYING, () -> copyTree(tree));
    }

    /** Worker: the folder is read in place; check default.xbe's hash, then remember it. */
    private void checkInPlace(File dir) throws IOException {
        File xbe = GameStore.findXbe(dir);
        String sha = GameStore.sha256(xbe);
        Log.i(TAG, "launcher: " + xbe + " sha256 " + sha);
        if (!GameStore.hashMatches(this, sha)) {
            fail(getString(R.string.WRONG_GAME));
            return;
        }
        chosen(dir.getAbsolutePath(), GameStore.xbeTitle(xbe));
    }

    private static final class Doc {
        final String id, path;
        final boolean dir;
        final long size;

        Doc(String id, String path, boolean dir, long size) {
            this.id = id;
            this.path = path;
            this.dir = dir;
            this.size = size;
        }
    }

    private void listTree(Uri tree, String parentId, String prefix, List<Doc> out) throws IOException {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parentId);
        String[] cols = { DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                          DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE };
        List<Doc> dirs = new ArrayList<>();
        try (Cursor c = getContentResolver().query(children, cols, null, null, null)) {
            if (c == null)
                throw new IOException("cannot list " + prefix);
            while (c.moveToNext()) {
                if (cancelRequested)
                    return;
                String name = c.getString(1);
                boolean isDir = DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(2));
                Doc d = new Doc(c.getString(0), prefix + name, isDir, c.isNull(3) ? 0 : c.getLong(3));
                out.add(d);
                if (isDir)
                    dirs.add(d);
            }
        }
        for (Doc d : dirs)
            listTree(tree, d.id, d.path + "/", out);
    }

    /** Worker: copy a picked tree into the app (fallback without All files access, XBA-40). */
    private void copyTree(Uri tree) throws IOException {
        copyTree(tree, DocumentsContract.getTreeDocumentId(tree), true);
    }

    /**
     * Worker: copy the folder rootId of tree into <external files>/games/<TID>. release: give the read permission
     * back at the end (a picked game folder; the library folder keeps it).
     */
    private void copyTree(Uri tree, String rootId, boolean release) throws IOException {
        ContentResolver cr = getContentResolver();
        try {
            List<Doc> docs = new ArrayList<>();
            listTree(tree, rootId, "", docs);
            if (cancelled())
                return;

            Doc xbe = null;
            long total = 0;
            for (Doc d : docs) {
                if (!d.dir) {
                    total += d.size;
                    if (d.path.equalsIgnoreCase("default.xbe"))
                        xbe = d;
                }
            }
            if (xbe == null) {
                fail(getString(R.string.NOT_A_GAME_FOLDER));
                return;
            }
            String sha;
            try (InputStream in = cr.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, xbe.id))) {
                if (in == null)
                    throw new IOException("cannot open default.xbe");
                sha = GameStore.sha256(in);
            }
            Log.i(TAG, "launcher: copy: default.xbe sha256 " + sha + ", " + docs.size() + " entries, " + total
                       + " bytes");
            if (!GameStore.hashMatches(this, sha)) {
                fail(getString(R.string.WRONG_GAME));
                return;
            }
            if (!enoughSpace(total))
                return;

            File staged = stagingDir();
            byte[] buf = new byte[1 << 20];
            long done = 0;
            for (Doc d : docs) {
                File dst = new File(staged, d.path);
                if (d.dir) {
                    if (!dst.mkdirs() && !dst.isDirectory())
                        throw new IOException("cannot create " + dst);
                    continue;
                }
                progress(done, total, d.path);
                try (InputStream in = cr.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, d.id));
                     OutputStream out = new FileOutputStream(dst)) {
                    if (in == null)
                        throw new IOException("cannot open " + d.path);
                    int n;
                    while ((n = in.read(buf)) > 0) {
                        if (cancelRequested)
                            break;
                        out.write(buf, 0, n);
                        done += n;
                        progress(done, total, d.path);
                    }
                }
                if (cancelRequested) {
                    GameStore.deleteTree(staged);
                    cancelled();
                    return;
                }
            }
            finishImport(staged);
        } finally {
            if (release) {
                try {
                    cr.releasePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION);
                } catch (SecurityException ignored) {
                }
            }
        }
    }

    // ---- ISO -----------------------------------------------------------------------------------------------

    private void openIsoPicker() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*")
                .putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)   // every part of a split image at once (XBA-126)
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        try {
            startActivityForResult(i, REQ_ISO);
        } catch (ActivityNotFoundException e) {
            setNote(getString(R.string.PICKER_MISSING), NOTE_ERROR);
        }
    }

    private String displayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[] { OpenableColumns.DISPLAY_NAME }, null, null,
                                                   null)) {
            if (c != null && c.moveToFirst())
                return c.getString(0);
        } catch (RuntimeException ignored) {
        }
        String last = uri.getLastPathSegment();
        return last == null ? "" : last;
    }

    /** The picker's answer: one image, or every part of a split one (XBA-126), in any order. */
    private void onIsoPicked(Intent data) {
        List<Uri> uris = new ArrayList<>();
        if (data.getClipData() != null) {
            for (int k = 0; k < data.getClipData().getItemCount(); k++)
                uris.add(data.getClipData().getItemAt(k).getUri());
        } else if (data.getData() != null) {
            uris.add(data.getData());
        }
        if (uris.isEmpty())
            return;
        List<String> names = new ArrayList<>();
        for (Uri u : uris)
            names.add(displayName(u));
        Log.i(TAG, "launcher: image " + uris + " (" + names + ")");
        for (String n : names) {
            if (!DiscImages.isImageName(n)) {
                setNote(getString(R.string.NOT_AN_ISO, n), NOTE_ERROR);
                return;
            }
        }
        int[] order = DiscImages.orderParts(names);
        if (order == null) {
            setNote(getString(R.string.SPLIT_PARTS_INCOMPLETE, TextUtils.join(", ", names)), NOTE_ERROR);
            return;
        }
        List<Uri> u2 = new ArrayList<>();
        List<String> n2 = new ArrayList<>();
        for (int k : order) {
            u2.add(uris.get(k));
            n2.add(names.get(k));
        }
        runWorker(R.string.CHECKING, () -> importImage(u2, n2, null, false));
    }

    /**
     * Worker (XBA-126): an image picked (uris, its parts in order) or from the library (file, or uris under the
     * library folder's tree: treeGranted). Read in place when the app can reach it again at every start -- a path
     * with All files access, or documents whose read permission persists -- else extracted as before. In place,
     * only its XBEs are copied, into the companion folder (DiscImages).
     */
    private void importImage(List<Uri> uris, List<String> names, File file, boolean treeGranted) throws IOException {
        DiscImages.Disc disc = new DiscImages.Disc();
        List<ParcelFileDescriptor> pfds = new ArrayList<>();
        try {
            String path = file != null ? file.getAbsolutePath()
                        : Environment.isExternalStorageManager() && uris.size() == 1
                          ? DiscImages.documentUriToPath(uris.get(0)) : null;
            if (path != null && new File(path).isFile()) {
                for (File p : DiscImages.splitParts(new File(path))) {
                    pfds.add(ParcelFileDescriptor.open(p, ParcelFileDescriptor.MODE_READ_ONLY));
                    disc.names.add(p.getName());
                }
                disc.path = path;
            } else {
                boolean persisted = treeGranted;
                for (Uri u : uris) {
                    if (treeGranted)
                        break;
                    try {
                        getContentResolver().takePersistableUriPermission(u, Intent.FLAG_GRANT_READ_URI_PERMISSION);
                        persisted = true;
                    } catch (SecurityException e) {
                        persisted = false;
                        break;
                    }
                }
                for (Uri u : uris) {
                    ParcelFileDescriptor p = getContentResolver().openFileDescriptor(u, "r");
                    if (p == null)
                        throw new IOException("cannot open " + u);
                    pfds.add(p);
                }
                if (persisted)
                    for (Uri u : uris)
                        disc.uris.add(u.toString());
                disc.names.addAll(names);
            }
            int[] fds = new int[pfds.size()];
            for (int k = 0; k < fds.length; k++)
                fds[k] = pfds.get(k).getFd();
            String name = disc.name();
            String[] title = new String[1];
            long[] p = XisoNative.probeParts(fds, title);
            if (p[0] != 0) {
                fail(getString(R.string.NOT_A_GAME_IMAGE, name + ": " + XisoNative.strerror((int)p[0])));
                return;
            }
            if (p[2] == 0) {
                fail(getString(R.string.NOT_A_GAME_IMAGE, name));
                return;
            }
            disc.kind = kindName((int)p[3]);
            disc.files = p[1];
            disc.stored = p[4];
            boolean inPlace = DiscImages.inPlaceAllowed(this) && (disc.viaPath() || !disc.uris.isEmpty());
            Log.i(TAG, "launcher: image " + name + ": " + disc.kind + ", " + fds.length + " part(s), "
                       + (inPlace ? "read in place" : "extracted"));
            if (!inPlace) {
                extractParts(fds, name, p[1]);
                return;
            }
            progressIndeterminate(R.string.READING_IN_PLACE);
            File staged = new File(DiscImages.discsDir(this), ".import");
            GameStore.deleteTree(staged);
            if (!staged.mkdirs() && !staged.isDirectory())
                throw new IOException("cannot create " + staged);
            int n = XisoNative.extractXbes(fds, staged.getAbsolutePath(), DiscImages.MAX_XBE);
            if (n < 0) {
                GameStore.deleteTree(staged);
                fail(getString(R.string.IMPORT_FAILED, XisoNative.strerror(n)));
                return;
            }
            DiscImages.write(staged, disc);
            finishImage(staged, disc);
        } finally {
            for (ParcelFileDescriptor p : pfds)
                try {
                    p.close();
                } catch (IOException ignored) {
                }
        }
    }

    private static String kindName(int kind) {
        String k = (kind & 0xFF) == XisoNative.KIND_CSO ? "cso" : (kind & 0xFF) == XisoNative.KIND_ZSO ? "zso"
                 : (kind & 0xFF) == XisoNative.KIND_CCI ? "cci" : "iso";
        return (kind & XisoNative.KIND_SPLIT) != 0 ? k + " split" : k;
    }

    /** Worker: the companion folder holds the image's XBEs and disc.json; default.xbe checked, then games' record. */
    private void finishImage(File staged, DiscImages.Disc disc) throws IOException {
        progressIndeterminate(R.string.CHECKING);
        File xbe = GameStore.findXbe(staged);
        if (xbe == null) {
            GameStore.deleteTree(staged);
            fail(getString(R.string.NOT_A_GAME_IMAGE, disc.name()));
            return;
        }
        String sha = GameStore.sha256(xbe);
        Log.i(TAG, "launcher: image default.xbe sha256 " + sha);
        if (!GameStore.hashMatches(this, sha)) {
            GameStore.deleteTree(staged);
            fail(getString(R.string.WRONG_GAME));
            return;
        }
        String title = GameStore.xbeTitle(xbe);
        String tid = Pack.xbeTitleId(xbe);
        if (tid == null)
            tid = Library.localKey(sha);
        File game = GameStore.swapInto(staged, DiscImages.companionDir(this, tid));
        if (game == null) {
            GameStore.deleteTree(staged);
            fail(getString(R.string.IMPORT_FAILED, "rename"));
            return;
        }
        Log.i(TAG, "launcher: library: " + tid + " read in place from " + (disc.viaPath() ? disc.path
                                                                                          : disc.uris.toString()));
        chosen(game.getAbsolutePath(), title);
    }

    /** Worker: extraction of an image given as its parts (RECOMP_DISC_IMAGE=0, or no lasting access to it). */
    private void extractParts(int[] fds, String name, long total) throws IOException {
        if (!enoughSpace(total))
            return;
        File staged = stagingDir();
        int rc = XisoNative.extractParts(fds, staged.getAbsolutePath(), (done, all, path) -> {
            progress(done, all, path);
            return cancelRequested;
        });
        if (rc == XisoNative.canceledCode()) {
            GameStore.deleteTree(staged);
            cancelled();
            return;
        }
        if (rc != 0) {
            GameStore.deleteTree(staged);
            if (rc == XisoNative.noSpaceCode())
                fail(getString(R.string.NOT_ENOUGH_SPACE, size(total), size(freeSpace())));
            else
                fail(getString(R.string.IMPORT_FAILED, XisoNative.strerror(rc)));
            return;
        }
        finishImport(staged);
    }

    // ---- Shared import steps -------------------------------------------------------------------------------

    /**
     * <external files dir>/games/.import, emptied: the import goes there, and becomes games/<TID> only once checked
     * (the older game/ is never an import target again: docs « Game library »).
     */
    private File stagingDir() throws IOException {
        File ext = GameStore.externalFilesDir(this);
        if (ext == null)
            throw new IOException("no external files dir");
        File games = new File(ext, Library.GAMES_SUBDIR);
        File staged = new File(games, ".import");
        GameStore.deleteTree(staged);
        GameStore.deleteTree(new File(games, ".import.partial"));
        if (!staged.mkdirs() && !staged.isDirectory())
            throw new IOException("cannot create " + staged);
        return staged;
    }

    /** The imported tree is complete: default.xbe and its hash, then it becomes game/ and the remembered choice. */
    private void finishImport(File staged) throws IOException {
        progressIndeterminate(R.string.CHECKING);
        File xbe = GameStore.findXbe(staged);
        if (xbe == null) {
            GameStore.deleteTree(staged);
            fail(getString(R.string.NOT_A_GAME_FOLDER));
            return;
        }
        String sha = GameStore.sha256(xbe);
        Log.i(TAG, "launcher: imported default.xbe sha256 " + sha);
        if (!GameStore.hashMatches(this, sha)) {
            GameStore.deleteTree(staged);
            fail(getString(R.string.WRONG_GAME));
            return;
        }
        String title = GameStore.xbeTitle(xbe);
        String tid = Pack.xbeTitleId(xbe);
        if (tid == null)
            tid = Library.localKey(sha);
        File game = GameStore.swapInto(staged, Library.importDir(GameStore.externalFilesDir(this), tid));
        if (game == null) {
            GameStore.deleteTree(staged);
            fail(getString(R.string.IMPORT_FAILED, "rename"));
            return;
        }
        Log.i(TAG, "launcher: library: imported " + tid + " into " + game);
        dropLegacyCopy(tid, game);
        chosen(game.getAbsolutePath(), title);
    }

    /**
     * Worker, after a checked import into games/<TID>: the older game/ copy of the same title goes (docs « Game
     * library », migration), unless a game process may be using it. The launched game moves to the new copy.
     */
    private void dropLegacyCopy(String tid, File game) {
        File legacy = GameStore.defaultGameDir(this);
        if (legacy == null || legacy.equals(game) || !tid.equals(Pack.xbeTitleId(GameStore.findXbe(legacy))))
            return;
        if (mainProcessPid() != 0) {
            Log.i(TAG, "launcher: library: the older copy " + legacy + " stays (a game process is running)");
            return;
        }
        String launched = GameStore.chosenDir(this);
        if (launched == null || launched.equals(legacy.getAbsolutePath()))
            GameStore.rememberLaunch(this, game.getAbsolutePath(), GameStore.xbeTitle(GameStore.findXbe(game)), tid);
        GameStore.deleteTree(legacy);
        Log.i(TAG, "launcher: library: removed the older copy " + legacy);
    }

    private long freeSpace() {
        File ext = GameStore.externalFilesDir(this);
        return ext == null ? 0 : ext.getUsableSpace();
    }

    private boolean enoughSpace(long need) {
        long free = freeSpace();
        if (free >= need + SPACE_MARGIN)
            return true;
        fail(getString(R.string.NOT_ENOUGH_SPACE, size(need), size(free)));
        return false;
    }

    private String size(long bytes) {
        return Formatter.formatShortFileSize(this, bytes);
    }

    // ---- Worker and progress -------------------------------------------------------------------------------

    private interface Work {
        void run() throws IOException;
    }

    /** Runs work on a background thread behind the IMPORT screen; its outcome is handled in onWorkerDone. */
    private void runWorker(int label, Work work) {
        if (workerBusy || (worker != null && worker.isAlive()))
            return;
        cancelRequested = false;
        workerNote = null;
        chosenDir = null;
        chosenTitle = null;
        chosenSha = null;
        importLabel = label;
        if (screen == Screen.FOLDER_ACCESS && !history.isEmpty())
            importReturn = history.pop();   // SOURCE: a failed import comes back to the choice of source
        else
            importReturn = screen == Screen.IMPORT || screen == null ? Screen.SOURCE : screen;
        int i = focusables.indexOf(focused());
        if (i >= 0 && screen != null)
            focusMemory.put(screen, i);
        if (direct && screen == Screen.DIRECT)   // a game's own app checking the files XRecomp uses: its one line
            directShow(Direct.CHECKING, getString(label), Theme.ACCENT, null, 0, null, false);
        else
            show(Screen.IMPORT);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        workerBusy = true;
        worker = new Thread(() -> {
            try {
                work.run();
            } catch (IOException | RuntimeException e) {
                Log.w(TAG, "launcher: import failed", e);
                fail(getString(R.string.IMPORT_FAILED, String.valueOf(e.getMessage())));
            } finally {
                runOnUiThread(this::onWorkerDone);
            }
        }, "xba-import");
        worker.start();
    }

    private void onWorkerDone() {
        workerBusy = false;
        getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (isFinishing())
            return;
        if (chosenDir != null) {
            onGameChosen(chosenDir, chosenTitle == null ? "" : chosenTitle);
            return;
        }
        importTid = null;
        importFromLibrary = false;
        String text = workerNote;
        int kind = workerNoteKind;
        if (direct && importReturn == Screen.DIRECT) {   // the files XRecomp uses were not accepted: the game choice
            directSourceFailed = true;
            boolean wrong = getString(R.string.WRONG_GAME).equals(text);
            directFail(getString(wrong ? R.string.STATE_WRONG : R.string.DIRECT_SOURCE_NOT_USED),
                       text != null ? text : getString(R.string.DIRECT_SOURCE_FAILED), R.string.CHOOSE_GAME,
                       this::directChoose, false);
            return;
        }
        show(importReturn);
        if (text != null)
            setNote(text, kind);
    }

    /** Worker: the game is checked; its theme is baked here, off the UI thread, before HOME shows it. */
    private void chosen(String dir, String title) {
        File d = new File(dir);
        GameTheme.ensure(this, d, Pack.xbeTitleId(GameStore.findXbe(d)), title);
        try {
            chosenSha = GameStore.sha256(GameStore.findXbe(d));
        } catch (IOException | RuntimeException e) {
            chosenSha = null;
        }
        chosenTitle = title;
        chosenDir = dir;
    }

    private void progress(long done, long total, String path) {
        long now = SystemClock.uptimeMillis();
        if (now - lastUiUpdate < 100 && done < total)
            return;
        lastUiUpdate = now;
        runOnUiThread(() -> {
            if (screen == Screen.DIRECT && directStatus != null && directState == Direct.CHECKING && total > 0)
                directStatus.set(getString(importLabel) + " · " + String.format(Locale.getDefault(), "%d%%",
                                 Math.round(Math.min(1f, done / (float)total) * 100)), Theme.ACCENT);
            if (screen != Screen.IMPORT || ring == null)
                return;
            float frac = total > 0 ? Math.min(1f, done / (float)total) : -1;
            String pct = frac >= 0 ? String.format(Locale.getDefault(), "%d%%", Math.round(frac * 100)) : "";
            ring.set(frac, pct, getString(importLabel), getString(R.string.PROGRESS_BYTES, size(done), size(total)));
            importBytes.setText(getString(R.string.PROGRESS_BYTES, size(done), size(total)));
            importFile.setText(path == null ? "" : path);
        });
    }

    private void progressIndeterminate(int label) {
        runOnUiThread(() -> {
            if (screen == Screen.DIRECT && directStatus != null && directState == Direct.CHECKING) {
                importLabel = label;
                directStatus.set(getString(label), Theme.ACCENT);
            }
            if (screen != Screen.IMPORT || ring == null)
                return;
            importLabel = label;
            importHead.setText(label);
            ring.set(-1, "", getString(label), "");
            importFile.setText("");
        });
    }

    private void fail(String text) {
        workerNoteKind = NOTE_ERROR;
        workerNote = text;
    }

    /** True (and says so) when the player pressed Cancel. */
    private boolean cancelled() {
        if (!cancelRequested)
            return false;
        Log.i(TAG, "launcher: import canceled");
        workerNoteKind = NOTE_INFO;
        workerNote = getString(R.string.CANCELED);
        return true;
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_ALL_FILES) {
            Log.i(TAG, "launcher: All files access "
                       + (Environment.isExternalStorageManager() ? "granted" : "refused"));
            if (directAccessPending) {   // asked from DIRECT, for the files XRecomp uses: on with the next step
                directAccessPending = false;
                directStep(true);
                return;
            }
            openTreePicker();   // in place when granted, copied otherwise
            return;
        }
        if (requestCode == REQ_PACK_HANDOVER) {
            onHandoverAnswer(resultCode, data);
            return;
        }
        if (resultCode != RESULT_OK || data == null || (data.getData() == null && data.getClipData() == null))
            return;
        if (requestCode == REQ_TREE)
            onTreePicked(data.getData());
        else if (requestCode == REQ_LIBRARY_TREE)
            onLibraryTreePicked(data.getData());
        else if (requestCode == REQ_ISO)
            onIsoPicked(data);
        else if (requestCode == REQ_LSFG_DLL)
            onLsfgDllPicked(data.getData());
        else if (requestCode == REQ_VK_DRIVER)
            onVkDriverPicked(data.getData());
    }
}
