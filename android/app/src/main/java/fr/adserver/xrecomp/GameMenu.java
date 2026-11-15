package fr.adserver.xrecomp;

import android.animation.Animator;
import android.animation.AnimatorListenerAdapter;
import android.animation.ValueAnimator;
import android.app.Activity;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.animation.DecelerateInterpolator;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.EnumMap;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.function.LongSupplier;

/**
 * The in-game menu (docs/android/app-shell.md « In-game menu »): a drawer that slides in from the left over the game
 * (Eden / BananaNative style), in a View layer added over SDL's surface with addContentView (the surface is never
 * recreated). The launcher's look: a flush, flat sheet, text-only MenuRows with the gliding focus, its sounds and
 * haptics.
 *
 * <ul>
 *   <li>Opens by: Android Back (not a gamepad's: that one is the pad's Select for the game, as before), the
 *       gamepad's Guide / Mode button (the Xbox pad has none, so no game uses it), Select + Start held
 *       {@link MenuTriggers#COMBO_MS}, a swipe in from the left edge, or the companion panel's « Menu ».</li>
 *   <li>While open the game is held ({@link Host#hold}: the lifecycle's MENU reason, as in the background: the flip
 *       parks, the audio device pauses), and every key and joystick event goes to the drawer only. Opening releases
 *       in the game every key it saw go down and centres every stick it saw move, and the companion's buttons;
 *       closing releases the companion's again. A key whose down came while open acts on its release, so nothing of
 *       it reaches the game.</li>
 *   <li>Items: Resume, for a GBA, PS1 or N64 game Save state, Load state (four slots each) and Fast-forward
 *       (XBA-182, XBA-183) and Cheats (XBA-197: each code On / Off, Add a code, Delete a code; written at once),
 *       Settings (performance overlay, game volume, bottom screen, a console game's scale and filter
 *       and the picture shader (XBA-206): all live; this game's language, widescreen and 480p, internal resolution,
 *       Vulkan driver and most Game settings: read at the start, so « Restart to apply » shows at the top once one
 *       of them changed (a second press restarts the game in a new process); Share log), Controls, Screenshot, Back to launcher (the game stays alive and held in the
 *       background; « Back to the home screen » in a game's own app, which has no launcher to show), Quit game (a second press within {@link #QUIT_ARM_MS} confirms). The header shows the game's
 *       title and « Paused · 0:42 played ».</li>
 * </ul>
 *
 * Everything native goes through {@link Host}, so the JVM tests drive it with a fake.
 */
final class GameMenu {
    /** What the menu needs from the game's activity. */
    interface Host {
        /** Holds (true) or lets go of (false) the game: MenuNative.nativeHold. */
        void hold(boolean on);

        /** Hands a (made-up) key event to SDL as if it had come from the device: releases. */
        void sendKeyToGame(KeyEvent e);

        /** Hands a (made-up) joystick event to SDL: centred sticks. */
        void sendMotionToGame(MotionEvent e);

        /** The menu took a touch gesture the game had seen begin: SDL gets an ACTION_CANCEL. */
        void cancelGameTouch(MotionEvent e);

        /** The companion panel's buttons released (input_overlay: nothing held). */
        void releaseOverlay();

        /** SDL's surface takes the input focus back. */
        void restoreGameFocus();

        /** True when the key event comes from a gamepad SDL reads (its Back is the pad's Select). */
        boolean fromGamepad(KeyEvent e);

        void setVolume(int percent);

        /** { fps x 100, draws }, or null. */
        int[] stats();

        boolean hasSecondScreen();

        void bottomScreenChanged();

        void screenshot();

        void shareLog();

        void backToLauncher();

        void quit();

        /** The on-screen pad (XBA-42): null when the game has none, else whether it shows now. */
        default Boolean touchShown() {
            return null;
        }

        /** Shows or hides the on-screen pad for the rest of the session (the menu's toggle). */
        default void setTouchShown(boolean on) {}

        /** Opens the touch layout editor over the held game (XBA-207); the menu has closed. */
        default void editTouchLayout() {}

        /** The menu opens (XBA-212: a touch layout editor left open over the game is cancelled first). */
        default void menuOpened() {}

        /** The launched GBA, PS1 or N64 game's mapping (ConsoleControls.effective), or null for an Xbox game. */
        default ConsolePadMap consoleMap() {
            return null;
        }

        /**
         * XBA-166: the discs of the PlayStation game being played (Ps1Discs: the current one marked), empty for any
         * other game or a single disc; « Change disc » shows with two or more.
         */
        default List<Ps1Discs.Disc> discs() {
            return java.util.Collections.emptyList();
        }

        /** XBA-166: puts that disc in the drive (MenuNative.nativePs1ChangeDisc); false when it cannot. */
        default boolean changeDisc(Ps1Discs.Disc d) {
            return false;
        }

        /**
         * XBA-182 / XBA-183: a GBA, PS1 or N64 game with save states runs (MenuNative.nativeStatesAvailable): the state
         * rows show.
         */
        default boolean statesSupported() {
            return false;
        }

        /** XBA-182: when slot 1..4 was saved, Unix ms, or 0 when it has nothing loadable. */
        default long stateTime(int slot) {
            return 0;
        }

        /** XBA-182: the slot's thumbnail, { w, h, ARGB pixels... }, or null. */
        default int[] stateThumb(int slot) {
            return null;
        }

        /** XBA-182: saves the slot; 0 or a negative code (MenuNative.nativeStateSave). */
        default int saveState(int slot) {
            return STATE_NO_GAME;
        }

        /** XBA-182: loads the slot; 0 or a negative code. */
        default int loadState(int slot) {
            return STATE_NO_GAME;
        }

        /** XBA-182: whether fast-forward is on. */
        default boolean fastForward() {
            return false;
        }

        default void setFastForward(boolean on) {}

        /** The last quick save / load / resume: { seq, kind, slot, code } (MenuNative.nativeStateEvent), or null. */
        default int[] stateEvent() {
            return null;
        }

        /**
         * XBA-197: « Add a code » of the Cheats page: the dialog ({@link CheatsUi#add}); added(name) once the code is
         * saved.
         */
        default void addCheat(Console console, String key, java.util.function.Consumer<String> added) {}

        /**
         * The picture changed (its scale, filter or shader: {@link ConsoleVideo#LIVE_KEYS}), taken by the game at
         * once (MenuNative.nativeSetPicture).
         */
        default void setPicture(ConsoleVideo.Picture p) {}

        /**
         * « Restart to apply »: the game starts again in a new process (the launcher ends this one, as after the
         * title's own quick reboot), so the settings it reads once at its start take effect.
         */
        default void restartGame() {}
    }

    /** The automatic slot (« Where you left off ») and the kinds of nativeStateEvent. */
    static final int AUTO_SLOT = 0, EV_SAVED = 1, EV_LOADED = 2, EV_RESUMED = 3, EV_SLOT = 4;

    /** XBA-182: the save state slots, and the native side's codes (menu_jni.c). */
    static final int SLOTS = 4;
    static final int STATE_OK = 0, STATE_NO_GAME = -1, STATE_EMPTY = -2, STATE_OTHER_GAME = -3, STATE_OTHER_VERSION = -4,
                     STATE_IO = -5, STATE_BAD_SLOT = -6, STATE_DAMAGED = -7;
    /** The largest thumbnail the native side hands over (anything bigger is not one). */
    static final int THUMB_MAX_W = 160, THUMB_MAX_H = 120;

    static final long ANIM_MS = 200, QUIT_ARM_MS = 4000, FLASH_MS = 3000, PERF_TICK_MS = 500, SESSION_TICK_MS = 1000,
                      STATE_TICK_MS = 250;
    /** A Back within this long after a swipe opened the menu is the system's back gesture ending: ignored. */
    static final long SWIPE_BACK_GUARD_MS = 600;
    static final long NAV_FIRST_MS = 380, NAV_REPEAT_MS = 130;
    /** The drawer's width on the 1920 x 1080 design grid: a flush sheet against the left edge. */
    static final int DRAWER_W = 560;
    /** How far the panel reaches past the left edge (none: it is flush). */
    static final int HIDDEN = 0;
    static final int ROW_H = Theme.ROW_H;
    static final int VOLUME_STEP = 10;

    enum Page { MAIN, SETTINGS, CONTROLS, DISCS, SAVE_SLOTS, LOAD_SLOTS, GAME_SETTINGS, CHEATS, CHEATS_DELETE }

    enum Item { RESUME, SETTINGS, CONTROLS, SCREENSHOT, SHARE_LOG, LAUNCHER, QUIT, PERF, VOLUME, BOTTOM, LANGUAGE,
                WIDESCREEN, P480, TOUCH, BACK, DISC, SCALE, FILTER, UPSCALE, VK_DRIVER, STATE_SAVE, STATE_LOAD,
                FAST_FORWARD, AUTO_RESUME, GAME_SETTINGS, GS_RESET, CHEATS, CHEAT_ADD, CHEAT_DELETE, SHADER,
                TOUCH_EDIT, RESTART }

    private final Activity activity;
    private final Host host;
    private final LongSupplier clock;
    private final long startedAt;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final UiFeedback feedback;

    final FrameLayout root;
    final View scrim;
    final FrameLayout content, drawer;
    final Widgets.Glide glide;
    private final LinearLayout list;
    private final ScrollView scroll;
    private final TextView titleV, sessionV, pageV, perfV;
    private final LinearLayout hints;
    private final FrameLayout flashBox;
    private final Widgets.Chip flashV;
    private final Map<Item, Widgets.MenuRow> rows = new EnumMap<>(Item.class);

    private boolean open, started, animate = true, launcherPending;
    private Page page = Page.MAIN;
    private List<Ps1Discs.Disc> discList;
    private String openedBy = "";
    private long openedAt, quitArmedAt = -1;
    /** XBA-197: the Delete a code row armed (its index, -1: none) and when. */
    private int cheatArmed = -1;
    private long cheatArmedAt;
    private boolean perfOn;
    /** The game's title ID for the per-title language (titleId()). */
    private boolean titleIdRead;
    private String titleIdValue;
    /** XBA-190: the game's hotkeys (its own, else every game's), read once; the combo listens to its menu chord. */
    private Hotkeys hotkeys;
    private int volume, volumeBeforeMute = 100;
    private ValueAnimator anim;
    /** The picture the game shows (what it started with, then each live change): a change is sent once. */
    private ConsoleVideo.Picture livePicture;
    /** What the game started with of the settings that are not live ({@link #restartSnapshot}). */
    private String restartBase;
    private long restartArmedAt = -1;

    /** Keys whose down went to the game and that are still held: released in the game when the menu opens. */
    private final Map<Integer, KeyEvent> held = new HashMap<>();
    /** Keys that went down while the menu was open: only their release acts. */
    private final Set<Integer> downWhileOpen = new HashSet<>();
    /** The last joystick event per device that went to the game: centred when the menu opens. */
    private final Map<Integer, MotionEvent> lastMotion = new HashMap<>();
    private final MenuTriggers.EdgeSwipe swipe;
    private final MenuTriggers.Combo combo = new MenuTriggers.Combo();
    private final Runnable comboFire = () -> {
        if (combo.held())
            open("Select+Start");
    };
    private int navDir;
    private final Runnable navRepeat = new Runnable() {
        @Override
        public void run() {
            if (open && navDir != 0) {
                navStep(navDir);
                handler.postDelayed(this, NAV_REPEAT_MS);
            }
        }
    };
    private final Runnable sessionTick = new Runnable() {
        @Override
        public void run() {
            updateSession();
            if (open)
                handler.postDelayed(this, SESSION_TICK_MS);
        }
    };
    private final Runnable perfTick = new Runnable() {
        @Override
        public void run() {
            updatePerf();
            if (perfOn && started)
                handler.postDelayed(this, PERF_TICK_MS);
        }
    };
    /** The quick save / load chords and the resume at the start: a message for each new native event. */
    private int stateSeq;
    private final Runnable stateTick = new Runnable() {
        @Override
        public void run() {
            pollStateEvent();
            if (started)
                handler.postDelayed(this, STATE_TICK_MS);
        }
    };
    private final Runnable flashEnd = this::hideFlash;
    private final Runnable quitDisarm = this::disarmQuit;
    private final Runnable restartDisarm = this::disarmRestart;

    GameMenu(Activity activity, Host host, LongSupplier clock) {
        this.activity = activity;
        this.host = host;
        this.clock = clock;
        this.startedAt = clock.getAsLong();
        this.feedback = new UiFeedback(activity);
        float density = activity.getResources().getDisplayMetrics().density;
        swipe = new MenuTriggers.EdgeSwipe(20 * density);

        root = new FrameLayout(activity);
        scrim = new View(activity);
        scrim.setBackground(new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
                                                 new int[] { 0xA6000000, 0x66000000, 0x4D000000 }));
        scrim.setOnClickListener(v -> {
            feedback.play(UiFeedback.Cue.BACK, v);
            close();
        });
        scrim.setVisibility(View.GONE);
        root.addView(scrim, match());

        content = new FrameLayout(activity);
        content.setClipChildren(false);
        root.addView(new Widgets.Stage(activity, content), match());

        // The drawer: a flat sheet, flush with the left edge (dark under it too, over a bright game).
        drawer = new FrameLayout(activity);
        GradientDrawable under = new GradientDrawable();
        under.setColor(Theme.alpha(Theme.BG_BOTTOM, 0.7f));
        drawer.setBackground(new android.graphics.drawable.LayerDrawable(
                new android.graphics.drawable.Drawable[] { under, new Widgets.Panel(0) }));
        drawer.setClickable(true);   // touches on it never reach the game
        drawer.setPadding(HIDDEN + 64, Theme.S6 - Theme.S2, Theme.S4, Theme.MARGIN_BOTTOM);
        FrameLayout.LayoutParams dlp = new FrameLayout.LayoutParams(DRAWER_W + HIDDEN, ViewGroup.LayoutParams.MATCH_PARENT);
        dlp.leftMargin = -HIDDEN;
        drawer.setVisibility(View.GONE);
        content.addView(drawer, dlp);

        LinearLayout col = new LinearLayout(activity);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setClipChildren(false);
        drawer.addView(col, match());

        // The header: the game's title, then « Paused · 0:42 played ».
        titleV = Theme.oneLine(Theme.text(activity, Theme.T_ROW, Theme.MEDIUM, Theme.TEXT));
        col.addView(titleV, new LinearLayout.LayoutParams(-1, -2));
        sessionV = Theme.oneLine(Theme.text(activity, Theme.T_SMALL, Theme.REGULAR, Theme.TEXT_DIM));
        col.addView(sessionV, lpTop(-1, -2, Theme.S2));
        pageV = Theme.oneLine(Theme.label(activity, ""));
        LinearLayout.LayoutParams plp = lpTop(-1, -2, Theme.S5);
        col.addView(pageV, plp);

        // The rows overflow sideways (their focus plates reach into the margin) but never up or down: a scrolled list
        // stays between the page title and the hints instead of running over them.
        scroll = new ScrollView(activity) {
            @Override
            protected void dispatchDraw(android.graphics.Canvas c) {
                c.save();
                c.clipRect(-getLeft(), getScrollY(), getWidth() + Theme.S6, getScrollY() + getHeight());
                super.dispatchDraw(c);
                c.restore();
            }
        };
        scroll.setVerticalScrollBarEnabled(false);
        scroll.setOverScrollMode(View.OVER_SCROLL_NEVER);
        scroll.setClipChildren(false);
        scroll.setClipToPadding(false);
        list = new LinearLayout(activity);
        list.setOrientation(LinearLayout.VERTICAL);
        list.setClipChildren(false);
        scroll.addView(list, new FrameLayout.LayoutParams(-1, -2));
        LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(-1, 0, 1);
        slp.topMargin = Theme.S5 - Theme.S1;
        slp.leftMargin = -Theme.ROW_INSET;   // the rows' text on the header's edge, their focus plates in the margin
        col.addView(scroll, slp);

        hints = new LinearLayout(activity);
        hints.setOrientation(LinearLayout.HORIZONTAL);
        hints.setGravity(Gravity.CENTER_VERTICAL);
        hints.addView(new Widgets.Hint(activity, "A", s(R.string.MENU_HINT_SELECT), v -> clickFocused()));
        LinearLayout.LayoutParams hlp = new LinearLayout.LayoutParams(-2, -2);
        hlp.leftMargin = Theme.S4;
        hints.addView(new Widgets.Hint(activity, "B", s(R.string.MENU_RESUME), v -> back()), hlp);
        col.addView(hints, new LinearLayout.LayoutParams(-1, Theme.FOOTER_H));

        glide = new Widgets.Glide(activity);
        content.addView(glide, match());

        // Over the game, with the menu closed too: the performance figures and short messages.
        perfV = Theme.text(activity, Theme.T_SMALL, Theme.MEDIUM, Theme.TEXT);
        perfV.setBackground(pill());
        perfV.setPadding(22, 12, 22, 12);
        perfV.setVisibility(View.GONE);
        FrameLayout.LayoutParams flp = new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.END);
        flp.topMargin = 28;
        flp.rightMargin = 32;
        content.addView(perfV, flp);

        flashBox = new FrameLayout(activity);
        flashBox.setBackground(pill());
        flashBox.setPadding(6, 6, 6, 6);
        flashV = new Widgets.Chip(activity);
        flashBox.addView(flashV, new FrameLayout.LayoutParams(-2, -2));
        flashBox.setVisibility(View.GONE);
        FrameLayout.LayoutParams xlp = new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        xlp.topMargin = 36;
        content.addView(flashBox, xlp);

        root.getViewTreeObserver().addOnGlobalFocusChangeListener((was, now) -> {
            if (!open)
                return;
            if (was != null && now instanceof Widgets.MenuRow)
                feedback.play(UiFeedback.Cue.MOVE, now);
            if (quitArmedAt >= 0 && now != rows.get(Item.QUIT))
                disarmQuit();
            if (restartArmedAt >= 0 && now != rows.get(Item.RESTART))
                disarmRestart();
        });

        perfOn = GameStore.perfOverlay(activity);
        volume = GameStore.gameVolume(activity);
        if (volume > 0)
            volumeBeforeMute = volume;
        host.setVolume(volume);
        updatePerf();
        livePicture = currentPicture();
        restartBase = restartSnapshot();
    }

    GameMenu(Activity activity, Host host) {
        this(activity, host, SystemClock::uptimeMillis);
    }

    /** Puts the layer over the activity's content (SDL's layout): from onCreate, after SDLActivity's. */
    void attach() {
        activity.addContentView(root, match());
    }

    // ---- State -------------------------------------------------------------------------------------------------

    boolean isOpen() {
        return open;
    }

    Page page() {
        return page;
    }

    boolean perfOn() {
        return perfOn;
    }

    int volume() {
        return volume;
    }

    boolean quitArmed() {
        return quitArmedAt >= 0;
    }

    /** Tests: no slide (the drawer is in place at once). */
    void setAnimate(boolean on) {
        animate = on;
    }

    Widgets.MenuRow row(Item item) {
        return rows.get(item);
    }

    // ---- Lifecycle (from the activity) -------------------------------------------------------------------------

    void onStart() {
        started = true;
        syncPicture("start");   // back from the launcher, where its Game settings may have changed the picture
        handler.removeCallbacks(perfTick);
        if (perfOn)
            handler.post(perfTick);
        handler.removeCallbacks(stateTick);
        handler.post(stateTick);
    }

    /** After SDLActivity.onStop (the background hold is in place by then): a pending Back to launcher lets go. */
    void onStop() {
        started = false;
        handler.removeCallbacks(perfTick);
        handler.removeCallbacks(stateTick);
        autoSave("background");   // Back to launcher, Home, the screen off: the next start resumes here
        settleLauncher();
    }

    void onResume() {
        settleLauncher();
    }

    private void settleLauncher() {
        if (!launcherPending)
            return;
        launcherPending = false;
        host.hold(false);
    }

    void release() {
        handler.removeCallbacksAndMessages(null);
        feedback.release();
        for (MotionEvent m : lastMotion.values())
            m.recycle();
        lastMotion.clear();
    }

    // ---- Open / close ------------------------------------------------------------------------------------------

    void toggle(String why) {
        if (open)
            close();
        else
            open(why);
    }

    void open(String why) {
        if (open)
            return;
        open = true;
        openedBy = why;
        openedAt = clock.getAsLong();
        downWhileOpen.clear();
        Log.i(LauncherActivity.TAG, "shell: menu open (" + why + ")");
        host.menuOpened();
        releaseGameInput();
        host.hold(true);
        launcherPending = false;
        disarmQuit();
        syncPicture("open");   // changed in the launcher's Game settings while the game waited: shown now
        flashBox.setVisibility(View.GONE);
        titleV.setText(gameTitle());
        updateSession();
        handler.removeCallbacks(sessionTick);
        handler.postDelayed(sessionTick, SESSION_TICK_MS);
        showPage(Page.MAIN, null);
        scrim.setVisibility(View.VISIBLE);
        drawer.setVisibility(View.VISIBLE);
        slide(true, null);
        feedback.play(UiFeedback.Cue.SELECT, drawer);
        // From a key: the focus on Resume. From a touch (swipe, panel): no focus until a key comes.
        if (!"swipe".equals(why) && !"panel".equals(why))
            focus(rows.get(Item.RESUME));
    }

    void close() {
        close(true);
    }

    private void close(boolean slideOut) {
        if (!open)
            return;
        open = false;
        Log.i(LauncherActivity.TAG, "shell: menu closed");
        stopNav();
        disarmQuit();
        handler.removeCallbacks(sessionTick);
        downWhileOpen.clear();
        host.releaseOverlay();
        host.restoreGameFocus();
        host.hold(false);
        if (slideOut) {
            slide(false, null);
        } else {
            if (anim != null)
                anim.cancel();
            hideNow();
        }
    }

    private void hideNow() {
        drawer.setVisibility(View.GONE);
        scrim.setVisibility(View.GONE);
        drawer.setTranslationX(0);
        scrim.setAlpha(1);
    }

    private void slide(boolean in, Runnable then) {
        if (anim != null)
            anim.cancel();
        if (!animate) {
            if (in) {
                drawer.setTranslationX(0);
                scrim.setAlpha(1);
            } else {
                hideNow();
            }
            if (then != null)
                then.run();
            return;
        }
        float from = in ? 0 : 1, to = in ? 1 : 0;
        float start = drawer.getVisibility() == View.VISIBLE && anim != null ? progress() : from;
        anim = ValueAnimator.ofFloat(start, to);
        anim.setDuration(ANIM_MS);
        anim.setInterpolator(new DecelerateInterpolator(1.6f));
        anim.addUpdateListener(a -> {
            float t = (float)a.getAnimatedValue();
            drawer.setTranslationX(-(DRAWER_W + HIDDEN) * (1 - t));
            scrim.setAlpha(t);
        });
        anim.addListener(new AnimatorListenerAdapter() {
            private boolean cancelled;

            @Override
            public void onAnimationCancel(Animator a) {
                cancelled = true;
            }

            @Override
            public void onAnimationEnd(Animator a) {
                if (cancelled)
                    return;
                if (!in)
                    hideNow();
                if (then != null)
                    then.run();
            }
        });
        drawer.setTranslationX(-(DRAWER_W + HIDDEN) * (1 - start));
        scrim.setAlpha(start);
        anim.start();
    }

    private float progress() {
        return 1 + drawer.getTranslationX() / (DRAWER_W + HIDDEN);
    }

    /**
     * Before the game is held: every key the game saw go down gets its release, every stick it saw move is centred,
     * the companion's buttons are let go, and a Select + Start in progress is forgotten.
     */
    private void releaseGameInput() {
        long now = SystemClock.uptimeMillis();
        for (KeyEvent d : held.values())
            host.sendKeyToGame(KeyEvent.changeAction(KeyEvent.changeTimeRepeat(d, now, 0), KeyEvent.ACTION_UP));
        held.clear();
        for (MotionEvent m : lastMotion.values()) {
            MotionEvent z = centred(m, now);
            host.sendMotionToGame(z);
            z.recycle();
            m.recycle();
        }
        lastMotion.clear();
        combo.reset();
        handler.removeCallbacks(comboFire);
        host.releaseOverlay();
    }

    /** A joystick event from m's device with every axis at rest (sticks centred, triggers and hat released). */
    static MotionEvent centred(MotionEvent m, long now) {
        MotionEvent.PointerProperties[] pp = { new MotionEvent.PointerProperties() };
        pp[0].id = 0;
        pp[0].toolType = MotionEvent.TOOL_TYPE_UNKNOWN;
        MotionEvent.PointerCoords[] pc = { new MotionEvent.PointerCoords() };
        return MotionEvent.obtain(m.getDownTime(), now, MotionEvent.ACTION_MOVE, 1, pp, pc, 0, 0, 1f, 1f,
                                  m.getDeviceId(), 0, m.getSource(), 0);
    }

    // ---- Input -------------------------------------------------------------------------------------------------

    /**
     * Every key event, before SDL (ShellActivity.dispatchKeyEvent). True: the menu took it and SDL must not see it;
     * false: on to SDL as usual.
     */
    boolean dispatchKey(KeyEvent e) {
        int k = e.getKeyCode();
        int action = e.getAction();
        boolean down = action == KeyEvent.ACTION_DOWN, up = action == KeyEvent.ACTION_UP;
        if (k == KeyEvent.KEYCODE_VOLUME_UP || k == KeyEvent.KEYCODE_VOLUME_DOWN || k == KeyEvent.KEYCODE_VOLUME_MUTE)
            return false;   // the system's
        if (!open) {
            boolean back = k == KeyEvent.KEYCODE_BACK && !host.fromGamepad(e);
            if (back || k == KeyEvent.KEYCODE_BUTTON_MODE) {
                if (down && e.getRepeatCount() == 0)
                    open(back ? "Back" : "Guide");
                return true;   // down and up: never the game's
            }
            if (down) {
                if (e.getRepeatCount() == 0)
                    held.put(k, new KeyEvent(e));   // the framework recycles e after dispatch
            } else if (up) {
                held.remove(k);
            }
            // A gamepad's Back is its Select for SDL (and so for the combo).
            int ck = k == KeyEvent.KEYCODE_BACK ? KeyEvent.KEYCODE_BUTTON_SELECT : k;
            hotkeys();   // the combo's chord (XBA-190), set on the first key
            if (e.getRepeatCount() == 0 && combo.key(ck, down, clock.getAsLong())) {
                handler.removeCallbacks(comboFire);
                if (combo.held())
                    handler.postDelayed(comboFire, MenuTriggers.COMBO_MS);
            }
            return false;
        }
        if (down) {
            if (k == KeyEvent.KEYCODE_BACK && "swipe".equals(openedBy)
                && clock.getAsLong() - openedAt < SWIPE_BACK_GUARD_MS)
                return true;   // the system's back gesture finishing the swipe that opened the menu
            if (e.getRepeatCount() == 0)
                downWhileOpen.add(k);
            keyDown(k);
        } else if (up) {
            if (downWhileOpen.remove(k) && !e.isCanceled())
                keyUp(k);
        }
        return true;
    }

    private void keyDown(int k) {
        switch (k) {
        case KeyEvent.KEYCODE_DPAD_UP:
            move(-1);
            break;
        case KeyEvent.KEYCODE_DPAD_DOWN:
            move(1);
            break;
        case KeyEvent.KEYCODE_DPAD_LEFT:
            stepFocused(-1);
            break;
        case KeyEvent.KEYCODE_DPAD_RIGHT:
            stepFocused(1);
            break;
        default:
            break;
        }
    }

    private void keyUp(int k) {
        switch (k) {
        case KeyEvent.KEYCODE_BUTTON_A:
        case KeyEvent.KEYCODE_DPAD_CENTER:
        case KeyEvent.KEYCODE_ENTER:
        case KeyEvent.KEYCODE_NUMPAD_ENTER:
        case KeyEvent.KEYCODE_SPACE:
            clickFocused();
            break;
        case KeyEvent.KEYCODE_BUTTON_B:
        case KeyEvent.KEYCODE_BACK:
        case KeyEvent.KEYCODE_ESCAPE:
            back();
            break;
        case KeyEvent.KEYCODE_BUTTON_START:
        case KeyEvent.KEYCODE_BUTTON_MODE:
            feedback.play(UiFeedback.Cue.BACK, drawer);
            close();
            break;
        default:
            break;
        }
    }

    /**
     * Every generic motion event (ShellActivity.dispatchGenericMotionEvent). Closed: joystick events go on to the game
     * and are remembered (to centre them at the next open). Open: the hat and the left stick move the focus (and
     * step a setting sideways), and nothing reaches the game.
     */
    boolean dispatchMotion(MotionEvent e) {
        boolean joy = (e.getSource() & InputDevice.SOURCE_CLASS_JOYSTICK) != 0;
        if (!open) {
            if (joy) {
                MotionEvent old = lastMotion.put(e.getDeviceId(), MotionEvent.obtain(e));
                if (old != null)
                    old.recycle();
            }
            return false;
        }
        if (joy && e.getActionMasked() == MotionEvent.ACTION_MOVE)
            joystick(e.getAxisValue(MotionEvent.AXIS_HAT_X), e.getAxisValue(MotionEvent.AXIS_HAT_Y),
                     e.getAxisValue(MotionEvent.AXIS_X), e.getAxisValue(MotionEvent.AXIS_Y));
        return true;
    }

    /** 1 up, 2 down, 3 left, 4 right; a direction steps at once, then repeats while held. */
    void joystick(float hatX, float hatY, float x, float y) {
        float vx = Math.abs(hatX) > Math.abs(x) ? hatX : x, vy = Math.abs(hatY) > Math.abs(y) ? hatY : y;
        int d = 0;
        if (Math.abs(vy) >= 0.5f && Math.abs(vy) >= Math.abs(vx))
            d = vy < 0 ? 1 : 2;
        else if (Math.abs(vx) >= 0.5f)
            d = vx < 0 ? 3 : 4;
        if (d == navDir)
            return;
        handler.removeCallbacks(navRepeat);
        navDir = d;
        if (d != 0) {
            navStep(d);
            handler.postDelayed(navRepeat, NAV_FIRST_MS);
        }
    }

    private void stopNav() {
        navDir = 0;
        handler.removeCallbacks(navRepeat);
    }

    private void navStep(int d) {
        if (d == 1)
            move(-1);
        else if (d == 2)
            move(1);
        else
            stepFocused(d == 3 ? -1 : 1);
    }

    /**
     * Every touch event (ShellActivity.dispatchTouchEvent). Closed: a swipe in from the left edge opens the menu (the
     * game's touch is cancelled); anything else goes on. Open: the drawer's views take it (a tap on the dimmed game
     * closes the menu).
     */
    boolean dispatchTouch(MotionEvent e) {
        if (open && !swipe.taken())
            return false;
        MenuTriggers.Touch t = swipe.onTouch(e.getActionMasked(), e.getX(), e.getY(), root.getWidth());
        if (t == MenuTriggers.Touch.TAKE) {
            host.cancelGameTouch(e);
            open("swipe");
            return true;
        }
        return t == MenuTriggers.Touch.CONSUME;
    }

    // ---- Navigation --------------------------------------------------------------------------------------------

    private List<Widgets.MenuRow> focusables() {
        List<Widgets.MenuRow> out = new ArrayList<>();
        for (int i = 0; i < list.getChildCount(); i++) {
            View v = list.getChildAt(i);
            if (v instanceof Widgets.MenuRow && v.getVisibility() == View.VISIBLE)
                out.add((Widgets.MenuRow)v);
        }
        return out;
    }

    private Widgets.MenuRow focused() {
        View f = root.findFocus();
        return f instanceof Widgets.MenuRow ? (Widgets.MenuRow)f : null;
    }

    Widgets.MenuRow focusedRow() {
        return focused();
    }

    private void focus(View v) {
        if (v != null && !v.requestFocusFromTouch())
            v.requestFocus();
    }

    private void move(int dir) {
        List<Widgets.MenuRow> fs = focusables();
        if (fs.isEmpty())
            return;
        Widgets.MenuRow f = focused();
        int i = fs.indexOf(f);
        if (i < 0) {
            focus(fs.get(0));
            return;
        }
        int j = Math.max(0, Math.min(fs.size() - 1, i + dir));
        if (j != i)
            focus(fs.get(j));
    }

    private void stepFocused(int dir) {
        Widgets.MenuRow f = focused();
        if (f != null && f.cycler != null)
            f.cycler.step(dir);
    }

    private void clickFocused() {
        Widgets.MenuRow f = focused();
        if (f == null) {
            move(0);
            return;
        }
        f.performClick();
    }

    private void back() {
        if (!open)
            return;
        feedback.play(UiFeedback.Cue.BACK, drawer);
        if (page != Page.MAIN) {
            if (page == Page.GAME_SETTINGS) {
                showPage(Page.SETTINGS, Item.GAME_SETTINGS);
                return;
            }
            if (page == Page.CHEATS_DELETE) {
                showPage(Page.CHEATS, Item.CHEAT_DELETE);
                return;
            }
            Item from = page == Page.SETTINGS ? Item.SETTINGS : page == Page.DISCS ? Item.DISC
                        : page == Page.SAVE_SLOTS ? Item.STATE_SAVE : page == Page.LOAD_SLOTS ? Item.STATE_LOAD
                        : page == Page.CHEATS ? Item.CHEATS : Item.CONTROLS;
            showPage(Page.MAIN, from);
            return;
        }
        close();
    }

    // ---- Pages -------------------------------------------------------------------------------------------------

    /** The hints under the list: A Select, and B Resume on the main page, B Back on the others. */
    private void setBackHint(int label) {
        if (hints.getChildCount() > 1)
            hints.removeViewAt(1);
        LinearLayout.LayoutParams hlp = new LinearLayout.LayoutParams(-2, -2);
        hlp.leftMargin = Theme.S4;
        hints.addView(new Widgets.Hint(activity, "B", s(label), v -> back()), hlp);
    }

    /** The B hint's label (tests). */
    /** The rows of the page shown, in order (tests). */
    List<Widgets.MenuRow> rowsForTest() {
        return focusables();
    }

    String backHintForTest() {
        return ((Widgets.Hint)hints.getChildAt(1)).labelForTest();
    }

    private void showPage(Page p, Item focusItem) {
        boolean hadFocus = focused() != null;
        page = p;
        list.removeAllViews();
        rows.clear();
        disarmQuit();
        disarmRestart();
        setBackHint(p == Page.MAIN ? R.string.MENU_RESUME : R.string.MENU_HINT_BACK);
        switch (p) {
        case MAIN:
            pageV.setVisibility(View.GONE);
            row(Item.RESUME, s(R.string.MENU_RESUME));
            if (statesSupported()) {   // XBA-182 / XBA-183: a GBA, PS1 or N64 game
                row(Item.STATE_SAVE, s(R.string.MENU_STATE_SAVE));
                row(Item.STATE_LOAD, s(R.string.MENU_STATE_LOAD));
                Widgets.MenuRow ff = row(Item.FAST_FORWARD, s(R.string.MENU_FAST_FORWARD));
                ff.setChoices(new String[] { s(R.string.MENU_ON), s(R.string.MENU_OFF) });
                ff.cycler = dir -> toggleFastForward();
                updateValues();
            }
            if (cheatsSupported())   // XBA-197: a GBA, PS1 or N64 game's cheat codes
                row(Item.CHEATS, s(R.string.CHEATS));
            row(Item.SETTINGS, s(R.string.MENU_SETTINGS));
            row(Item.CONTROLS, s(R.string.MENU_CONTROLS));
            row(Item.SCREENSHOT, s(R.string.MENU_SCREENSHOT));
            if (discs().size() >= 2)   // XBA-166: a multi-disc PlayStation game
                row(Item.DISC, s(R.string.MENU_DISC));
            // A game's own app has no launcher to go back to: the row leaves to Android's home screen (the game
            // waits there as it would behind the launcher; the « Launcher » app shortcut still opens the settings).
            row(Item.LAUNCHER, s(TitleApp.ownTitleId(activity) != null ? R.string.MENU_LEAVE : R.string.MENU_LAUNCHER));
            list.addView(new View(activity), new LinearLayout.LayoutParams(-1, Theme.S3));   // Quit stands apart
            row(Item.QUIT, s(R.string.MENU_QUIT));
            break;
        case SETTINGS: {
            pageV.setText(s(R.string.MENU_SETTINGS));
            pageV.setVisibility(View.VISIBLE);
            restartRow();
            Widgets.MenuRow r = row(Item.PERF, s(R.string.MENU_PERF));
            r.setChoices(new String[] { s(R.string.MENU_ON), s(R.string.MENU_OFF) });
            r.cycler = dir -> setPerf(!perfOn);
            r = row(Item.VOLUME, s(R.string.MENU_VOLUME));
            r.setChoices(new String[] { volumeLabel(100), s(R.string.MENU_MUTED) });
            r.cycler = dir -> setVolume(volume + dir * VOLUME_STEP);
            if (statesSupported()) {   // a GBA, PS1 or N64 game: the automatic slot
                r = row(Item.AUTO_RESUME, s(R.string.MENU_RESUME_AUTO));
                r.setChoices(new String[] { s(R.string.MENU_ON), s(R.string.MENU_OFF) });
                r.cycler = dir -> setAutoResume(!GameStore.autoResume(activity));
            }
            if (host.hasSecondScreen()) {
                r = row(Item.BOTTOM, s(R.string.MENU_BOTTOM));
                r.setChoices(new String[] { s(R.string.VAL_PANEL), s(R.string.VAL_BLACK), s(R.string.MENU_SCREEN_OFF) });
                r.cycler = this::stepBottom;
            }
            if (host.touchShown() != null) {   // the on-screen pad, for this session (XBA-158 / 168 / 174)
                r = row(Item.TOUCH, s(R.string.MENU_TOUCH));
                r.setChoices(new String[] { s(R.string.MENU_TOUCH_SHOWN), s(R.string.MENU_TOUCH_HIDDEN) });
                r.cycler = dir -> toggleTouch();
                row(Item.TOUCH_EDIT, s(R.string.MENU_TOUCH_EDIT));   // XBA-207: this game's layout, live
            }
            if (titleId() != null && Console.ofKey(titleId()) != Console.XBOX) {
                // XBA-181: a GBA, PS1 or N64 game's own scale and filter (ConsoleVideo), shown at once.
                r = row(Item.SCALE, s(R.string.MENU_SCALE));
                r.setChoices(consoleLabels(ConsoleVideo.Setting.SCALE));
                r.cycler = dir -> stepConsoleVideo(ConsoleVideo.Setting.SCALE, dir);
                r = row(Item.FILTER, s(R.string.MENU_FILTER));
                r.setChoices(consoleLabels(ConsoleVideo.Setting.FILTER));
                r.cycler = dir -> stepConsoleVideo(ConsoleVideo.Setting.FILTER, dir);
                if (ConsoleVideo.applies(ConsoleVideo.Setting.UPSCALE, Console.ofKey(titleId()))) {
                    // XBA-184: a PS1 or N64 game's internal resolution, after « Restart to apply ».
                    r = row(Item.UPSCALE, s(R.string.MENU_UPSCALE));
                    r.setChoices(consoleLabels(ConsoleVideo.Setting.UPSCALE));
                    r.cycler = dir -> stepConsoleVideo(ConsoleVideo.Setting.UPSCALE, dir);
                }
                if (vkDriverShown()) {   // the N64 Vulkan renderer's driver, this game's own (VkDrivers)
                    r = row(Item.VK_DRIVER, s(R.string.MENU_VK_DRIVER));
                    VkDrivers drivers = VkDrivers.of(activity);
                    List<String> choices = VkDriverStore.gameChoices(drivers.list());
                    String[] labels = new String[choices.size()];
                    for (int i = 0; i < labels.length; i++)
                        labels[i] = VkDriverStore.gameChoiceLabel(activity, drivers, choices.get(i));
                    r.setChoices(labels);
                    r.cycler = this::stepVkDriver;
                }
            } else if (titleId() != null) {   // the Xbox's own settings: none of them means anything to a console
                r = row(Item.LANGUAGE, s(R.string.MENU_LANGUAGE));
                String[] labels = new String[GameLanguage.MENU_ORDER.length + 1];
                labels[0] = s(R.string.MENU_LANGUAGE_FOLLOW);
                for (int i = 0; i < GameLanguage.MENU_ORDER.length; i++)
                    labels[i + 1] = GameLanguage.NAMES[GameLanguage.MENU_ORDER[i]];
                r.setChoices(labels);
                r.cycler = this::stepLanguage;
                // XBA-134: this game's widescreen and 480p, as in Settings or its own.
                String[] video = { s(R.string.MENU_VIDEO_FOLLOW), s(R.string.MENU_ON), s(R.string.MENU_OFF) };
                r = row(Item.WIDESCREEN, s(R.string.MENU_WIDESCREEN));
                r.setChoices(video);
                r.cycler = dir -> stepVideo(GameVideo.Setting.WIDESCREEN, dir);
                r = row(Item.P480, s(R.string.MENU_480P));
                r.setChoices(video);
                r.cycler = dir -> stepVideo(GameVideo.Setting.P480, dir);
            }
            if (titleId() != null) {
                // XBA-206: the picture shader (XBA-195 / XBA-203: every console's window and the Xbox presenter), this
                // game's own in its GameSettings file, shown at once (MenuNative.nativeSetPicture).
                RecompConfig.Option sh = shaderOption();
                if (sh != null)
                    rows.put(Item.SHADER, gameOptionRow(sh, s(R.string.MENU_SHADER)));
            }
            if (titleId() != null)   // XBA-188: every Settings value, this game's own or as in Settings
                row(Item.GAME_SETTINGS, s(R.string.MENU_GAME_SETTINGS));
            row(Item.SHARE_LOG, s(R.string.MENU_SHARE_LOG));
            row(Item.BACK, s(R.string.MENU_BACK));
            updateValues();
            break;
        }
        case GAME_SETTINGS:
            pageV.setText(s(R.string.MENU_GAME_SETTINGS));
            pageV.setVisibility(View.VISIBLE);
            restartRow();
            kvRow(s(R.string.MENU_GS_NEXT_START), "");
            for (RecompConfig.Option o : GameSettings.options(Console.ofKey(titleId()), lsfgBuilt()))
                gameOptionRow(o, s(o.title));
            row(Item.GS_RESET, s(R.string.GS_RESET));
            row(Item.BACK, s(R.string.MENU_BACK));
            break;
        case CONTROLS:
            pageV.setText(s(R.string.MENU_CONTROLS));
            pageV.setVisibility(View.VISIBLE);
            int[][] kv = { { R.string.CTL_FACE, R.string.CTL_FACE_X },
                           { R.string.CTL_L1, R.string.CTL_L1_X }, { R.string.CTL_R1, R.string.CTL_R1_X },
                           { R.string.CTL_TRIGGERS, R.string.CTL_TRIGGERS_X },
                           { R.string.CTL_START, R.string.CTL_START_X }, { R.string.CTL_SELECT, R.string.CTL_SELECT_X },
                           { R.string.CTL_DPAD, R.string.CTL_DPAD_X }, { R.string.CTL_STICKS, R.string.CTL_STICKS_X } };
            ConsolePadMap cm = host.consoleMap();
            if (cm != null) {   // a GBA, PS1 or N64 game: its buttons and where they come from (XBA-158 / 168 / 174)
                hotkeyRows(cm.console);   // XBA-190: the menu chord and, with save states, the other hotkeys
                if (!cm.isDefault())
                    kvRow(s(R.string.MENU_CTL_REMAPPED),
                          activity.getString(R.string.MENU_CTL_REMAPPED_X, cm.changedCount()));
                for (int t = 0; t < cm.targetCount(); t++)
                    kvRow(ConsoleControls.sourcesLabel(activity, cm, t), s(ConsoleControls.targetLabel(cm.console, t)));
                for (int i = 0; i < cm.stick.length; i++)
                    kvRow(s(ControlsLabels.stick(cm.stick[i])), s(ConsoleControls.stickLabel(cm.console, i)));
                row(Item.BACK, s(R.string.MENU_BACK));
                break;
            }
            // XBA-131: the player's remapping (the launcher's Controls), first: each changed control, then the usual list.
            String own = GameSettings.raw(activity, titleId(), PadMapping.KEY);   // XBA-188: the title's own first
            PadMapping map = own != null ? PadMapping.parse(own)
                                         : PadMapping.load(RecompConfig.load(GameStore.externalFilesDir(activity)));
            if (!map.isDefault()) {
                kvRow(s(R.string.MENU_CTL_REMAPPED),
                      activity.getString(R.string.MENU_CTL_REMAPPED_X, map.changedCount()));
                for (PadMapping.Control c : PadMapping.Control.values()) {
                    if (map.get(c) != c.def)
                        kvRow(s(ControlsLabels.source(map.get(c))), s(ControlsLabels.control(c)));
                }
            }
            hotkeyRows(Console.XBOX);   // XBA-190: the menu chord only (no save states for an Xbox title)
            for (int[] r : kv)
                kvRow(s(r[0]), s(r[1]));
            row(Item.BACK, s(R.string.MENU_BACK));
            break;
        case CHEATS:
        case CHEATS_DELETE: {
            boolean del = p == Page.CHEATS_DELETE;
            pageV.setText(s(del ? R.string.CHEATS_DELETE : R.string.CHEATS));
            pageV.setVisibility(View.VISIBLE);
            cheatArmed = -1;
            Cheats cheats = Cheats.load(activity.getFilesDir(), titleId());
            kvRow(s(cheats.list.isEmpty() ? R.string.CHEATS_NONE : R.string.CHEATS_LIVE), "");
            for (int i = 0; i < cheats.list.size(); i++)
                cheatRow(cheats.list.get(i), i, del);
            if (!del) {
                row(Item.CHEAT_ADD, s(R.string.CHEATS_ADD));
                if (!cheats.list.isEmpty())
                    row(Item.CHEAT_DELETE, s(R.string.CHEATS_DELETE));
            }
            row(Item.BACK, s(R.string.MENU_BACK));
            break;
        }
        case DISCS:
            pageV.setText(s(R.string.MENU_DISC));
            pageV.setVisibility(View.VISIBLE);
            for (Ps1Discs.Disc d : discs())
                discRow(d);
            row(Item.BACK, s(R.string.MENU_BACK));
            break;
        case SAVE_SLOTS:
        case LOAD_SLOTS:
            pageV.setText(s(p == Page.SAVE_SLOTS ? R.string.MENU_STATE_SAVE : R.string.MENU_STATE_LOAD));
            pageV.setVisibility(View.VISIBLE);
            if (p == Page.LOAD_SLOTS && host.stateTime(AUTO_SLOT) > 0)   // « Where you left off », when there is one
                slotRow(AUTO_SLOT, false);
            for (int slot = 1; slot <= SLOTS; slot++)
                slotRow(slot, p == Page.SAVE_SLOTS);
            row(Item.BACK, s(R.string.MENU_BACK));
            break;
        }
        scroll.scrollTo(0, 0);
        if (hadFocus || focusItem != null) {
            Widgets.MenuRow target = focusItem != null ? rows.get(focusItem) : null;
            if (target == null) {
                List<Widgets.MenuRow> fs = focusables();
                target = p == Page.CONTROLS ? rows.get(Item.BACK) : fs.isEmpty() ? null : fs.get(0);
            }
            if (hadFocus || p == Page.MAIN)
                focus(target);
        }
    }

    private Widgets.MenuRow row(Item item, String title) {
        Widgets.MenuRow r = new Widgets.MenuRow(activity);
        r.set(title, null);
        r.onPress = () -> feedback.play(UiFeedback.Cue.SELECT, r);
        r.setOnClickListener(v -> onItem(item));
        rows.put(item, r);
        list.addView(r, new LinearLayout.LayoutParams(-1, ROW_H));
        return r;
    }

    /** XBA-166: the discs, read once (the folder is listed the first time the menu opens). */
    private List<Ps1Discs.Disc> discs() {
        if (discList == null) {
            List<Ps1Discs.Disc> d;
            try {
                d = host.discs();
            } catch (RuntimeException e) {
                Log.w(LauncherActivity.TAG, "shell: menu: the discs cannot be listed", e);
                d = null;
            }
            discList = d != null ? d : new ArrayList<>();
            if (discList.size() >= 2)
                Log.i(LauncherActivity.TAG, "shell: menu: " + discList.size() + " discs: " + discList);
        }
        return discList;
    }

    /** A disc of the Change disc page: its file name, « In the drive » under the one playing. */
    private void discRow(Ps1Discs.Disc d) {
        Widgets.MenuRow r = new Widgets.MenuRow(activity);
        r.set(d.name, null);
        if (d.current)
            r.setSubtitle(s(R.string.MENU_DISC_PLAYING));
        r.onPress = () -> feedback.play(UiFeedback.Cue.SELECT, r);
        r.setOnClickListener(v -> chooseDisc(d));
        list.addView(r, new LinearLayout.LayoutParams(-1, ROW_H));
    }

    private void chooseDisc(Ps1Discs.Disc d) {
        if (d.current) {
            close();
            return;
        }
        Log.i(LauncherActivity.TAG, "shell: menu: change disc to " + d.name + " (" + d.spec + ")");
        if (!host.changeDisc(d)) {
            Log.w(LauncherActivity.TAG, "shell: menu: change disc failed: " + d.spec);
            flash(s(R.string.MENU_DISC_FAILED), false);
            return;
        }
        List<Ps1Discs.Disc> now = new ArrayList<>();
        for (Ps1Discs.Disc x : discList)
            now.add(new Ps1Discs.Disc(x.name, x.spec, x.number, x == d));
        discList = now;
        close();
        flash(activity.getString(R.string.MENU_DISC_DONE, d.number), true);
    }

    // ---- Cheats (XBA-197) ----------------------------------------------------------------------------------------

    /** A GBA, PS1 or N64 game runs (not an Xbox title): its cheat codes can be changed while it plays. */
    private boolean cheatsSupported() {
        String key = titleId();
        return key != null && Console.ofKey(key) != Console.XBOX;
    }

    /**
     * One cheat of the Cheats page (On / Off: A or left / right, written at once, the runtime reads the file again by
     * itself), or of Delete a code (A arms, A again within {@link #QUIT_ARM_MS} deletes).
     */
    private void cheatRow(Cheats.Cheat c, int index, boolean delete) {
        Widgets.MenuRow r = new Widgets.MenuRow(activity);
        r.set(c.name, null);
        r.setSubtitle(c.lines.isEmpty() ? null : c.lines.get(0) + (c.lines.size() > 1 ? " …" : ""));
        r.onPress = () -> feedback.play(UiFeedback.Cue.SELECT, r);
        if (delete) {
            r.setOnClickListener(v -> {
                if (cheatArmed != index || clock.getAsLong() - cheatArmedAt > QUIT_ARM_MS) {
                    cheatArmed = index;
                    cheatArmedAt = clock.getAsLong();
                    r.setValue(s(R.string.CHEATS_DELETE_ARM));
                    r.invalidate();
                    return;
                }
                Cheats cheats = Cheats.load(activity.getFilesDir(), titleId());
                cheats.remove(index);
                boolean ok = cheats.save();
                Log.i(LauncherActivity.TAG, "shell: menu: cheat \"" + c.name + "\" deleted for " + titleId()
                                            + (ok ? "" : " (not saved)"));
                showPage(cheats.list.isEmpty() ? Page.CHEATS : Page.CHEATS_DELETE, null);
                flash(ok ? activity.getString(R.string.CHEATS_DELETED, c.name) : s(R.string.CHEATS_SAVE_FAILED), ok);
            });
        } else {
            r.setChoices(new String[] { s(R.string.MENU_ON), s(R.string.MENU_OFF) });
            r.setValue(s(c.enabled ? R.string.MENU_ON : R.string.MENU_OFF));
            Runnable toggle = () -> {
                Cheats cheats = Cheats.load(activity.getFilesDir(), titleId());
                cheats.toggle(index);
                boolean ok = cheats.save();
                boolean on = index < cheats.list.size() && cheats.list.get(index).enabled;
                r.setValue(s(on ? R.string.MENU_ON : R.string.MENU_OFF));
                r.invalidate();
                Log.i(LauncherActivity.TAG, "shell: menu: cheat \"" + c.name + "\" " + (on ? "on" : "off") + " for "
                                            + titleId() + (ok ? "" : " (not saved)"));
                if (!ok)
                    flash(s(R.string.CHEATS_SAVE_FAILED), false);
            };
            r.cycler = dir -> {
                toggle.run();
                feedback.play(UiFeedback.Cue.MOVE, r);
            };
            r.setOnClickListener(v -> toggle.run());
        }
        list.addView(r, new LinearLayout.LayoutParams(-1, ROW_H));
    }

    // ---- Save states and fast-forward (XBA-182) ----------------------------------------------------------------

    private boolean statesSupported() {
        try {
            return host.statesSupported();
        } catch (RuntimeException e) {
            Log.w(LauncherActivity.TAG, "shell: menu: save states unknown", e);
            return false;
        }
    }

    /**
     * A slot of the Save state or Load state page: « Slot N », the time it was saved or « Empty » under it, its
     * thumbnail at the right. Read each time the page shows, so a fresh save is there the next time.
     */
    private void slotRow(int slot, boolean save) {
        long when = host.stateTime(slot);
        Widgets.MenuRow r = new Widgets.MenuRow(activity);
        r.set(slot == AUTO_SLOT ? s(R.string.MENU_STATE_AUTO) : activity.getString(R.string.MENU_STATE_SLOT, slot),
              null);
        r.setSubtitle(when > 0 ? slotTime(activity, when, System.currentTimeMillis()) : s(R.string.MENU_STATE_EMPTY));
        if (when > 0)
            r.setThumb(thumbBitmap(host.stateThumb(slot)));
        else if (!save)
            r.setDim(true);   // nothing to load
        r.onPress = () -> feedback.play(UiFeedback.Cue.SELECT, r);
        r.setOnClickListener(v -> {
            if (save)
                saveSlot(slot);
            else
                loadSlot(slot, when > 0, r);
        });
        list.addView(r, new LinearLayout.LayoutParams(-1, ROW_H));
    }

    private void saveSlot(int slot) {
        int code = host.saveState(slot);
        Log.i(LauncherActivity.TAG, "shell: menu: save state slot " + slot + ": " + code);
        close();
        if (code == STATE_OK)
            flash(activity.getString(R.string.MENU_STATE_SAVED, slot), true);
        else
            flash(stateError(code, slot, true), false);
    }

    private void loadSlot(int slot, boolean filled, Widgets.MenuRow r) {
        if (!filled) {
            feedback.play(UiFeedback.Cue.ERROR, r);
            flash(activity.getString(R.string.MENU_STATE_EMPTY_SLOT, slot), false);
            return;
        }
        int code = host.loadState(slot);
        Log.i(LauncherActivity.TAG, "shell: menu: load state slot " + slot + ": " + code);
        close();
        if (code == STATE_OK)
            flash(slot == AUTO_SLOT ? s(R.string.MENU_STATE_RESUMED)
                                    : activity.getString(R.string.MENU_STATE_LOADED, slot), true);
        else
            flash(stateError(code, slot, false), false);
    }

    // ---- Quick save / load and resuming ------------------------------------------------------------------------

    /** « Resume where I left off » applies: a GBA, PS1 or N64 game with states, and the setting on. */
    private boolean autoResumes() {
        return GameStore.autoResume(activity) && statesSupported();
    }

    /** Saves the automatic slot when resuming applies (leaving the game: quit, background, back to launcher). */
    void autoSave(String why) {
        if (!autoResumes())
            return;
        int code = host.saveState(AUTO_SLOT);
        Log.i(LauncherActivity.TAG, "shell: menu: automatic slot saved (" + why + "): " + code);
    }

    void setAutoResume(boolean on) {
        GameStore.setAutoResume(activity, on);
        Log.i(LauncherActivity.TAG, "shell: menu: resume where I left off " + (on ? "on" : "off"));
        updateValues();
    }

    /** A message for each new quick save / load (the pad chords) or resume (the start), from the native side. */
    void pollStateEvent() {
        int[] ev;
        try {
            ev = host.stateEvent();
        } catch (RuntimeException e) {
            return;
        }
        if (ev == null || ev.length < 4 || ev[0] == stateSeq)
            return;
        stateSeq = ev[0];
        int kind = ev[1], slot = ev[2], code = ev[3];
        Log.i(LauncherActivity.TAG, "shell: state event " + kind + " slot " + slot + ": " + code);
        if (kind == EV_SLOT)   // XBA-190: the slot_next / slot_prev hotkeys
            flash(activity.getString(R.string.HK_SLOT_EVENT, slot), true);
        else if (kind == EV_RESUMED)
            flash(s(code == STATE_OK ? R.string.MENU_STATE_RESUMED : R.string.MENU_STATE_RESUME_FAILED),
                  code == STATE_OK);
        else if (code == STATE_OK)
            flash(activity.getString(kind == EV_SAVED ? R.string.MENU_STATE_SAVED : R.string.MENU_STATE_LOADED, slot),
                  true);
        else
            flash(stateError(code, slot, kind == EV_SAVED), false);
    }

    /** The message for a native code that is not STATE_OK. */
    private String stateError(int code, int slot, boolean save) {
        switch (code) {
        case STATE_EMPTY:
            return activity.getString(R.string.MENU_STATE_EMPTY_SLOT, slot);
        case STATE_OTHER_GAME:
            return activity.getString(R.string.MENU_STATE_ERR_OTHER_GAME, slot);
        case STATE_OTHER_VERSION:
            return activity.getString(R.string.MENU_STATE_ERR_VERSION, slot);
        case STATE_IO:
            return activity.getString(save ? R.string.MENU_STATE_ERR_WRITE : R.string.MENU_STATE_ERR_READ, slot);
        case STATE_DAMAGED:
            return activity.getString(R.string.MENU_STATE_ERR_DAMAGED, slot);
        case STATE_NO_GAME:
            return s(R.string.MENU_STATE_ERR_NO_GAME);
        default:
            return activity.getString(save ? R.string.MENU_STATE_SAVE_FAILED : R.string.MENU_STATE_LOAD_FAILED, slot);
        }
    }

    /** A slot's save time: the time alone today (« 14:32 »), else the date and the time, in the device's format. */
    static String slotTime(android.content.Context c, long when, long now) {
        java.util.Calendar a = java.util.Calendar.getInstance(), b = java.util.Calendar.getInstance();
        a.setTimeInMillis(when);
        b.setTimeInMillis(now);
        java.util.Date d = new java.util.Date(when);
        String time = android.text.format.DateFormat.getTimeFormat(c).format(d);
        if (a.get(java.util.Calendar.YEAR) == b.get(java.util.Calendar.YEAR)
            && a.get(java.util.Calendar.DAY_OF_YEAR) == b.get(java.util.Calendar.DAY_OF_YEAR))
            return time;
        return android.text.format.DateFormat.getMediumDateFormat(c).format(d) + " " + time;
    }

    /** The native thumbnail { w, h, ARGB pixels... } as a bitmap; null when there is none or it does not add up. */
    static android.graphics.Bitmap thumbBitmap(int[] t) {
        if (t == null || t.length < 2)
            return null;
        int w = t[0], h = t[1];
        if (w <= 0 || h <= 0 || w > THUMB_MAX_W || h > THUMB_MAX_H || t.length < 2 + w * h)
            return null;
        return android.graphics.Bitmap.createBitmap(t, 2, w, w, h, android.graphics.Bitmap.Config.ARGB_8888);
    }

    private void toggleFastForward() {
        boolean on = !host.fastForward();
        host.setFastForward(on);
        Log.i(LauncherActivity.TAG, "shell: menu: fast-forward " + (on ? "on" : "off"));
        updateValues();
    }

    /** A line of the Controls table: the device's control, what the game gets. */
    /**
     * XBA-190: the game's hotkeys (Hotkeys: its own, else every game's), read once. The combo listens to their menu
     * chord from then on.
     */
    Hotkeys hotkeys() {
        if (hotkeys == null) {
            try {
                hotkeys = Hotkeys.effective(activity, GameStore.externalFilesDir(activity), titleId());
            } catch (RuntimeException e) {
                hotkeys = new Hotkeys();
            }
            combo.setChord(hotkeys.get(Hotkeys.Action.MENU));
        }
        return hotkeys;
    }

    /** The Controls page's hotkey lines: Back, the menu chord, then each hotkey a game of console c has and uses. */
    private void hotkeyRows(Console c) {
        kvRow(s(R.string.CTL_MENU), s(R.string.CTL_MENU_X));
        Hotkeys h = hotkeys();
        for (Hotkeys.Action a : Hotkeys.actions(c)) {
            Hotkeys.Chord ch = h.get(a);
            if (!ch.isNone())
                kvRow(Hotkeys.chordLabel(activity, ch), s(Hotkeys.label(a)));
        }
    }

    private void kvRow(String key, String value) {
        if (value == null || value.isEmpty()) {   // a note alone (Game settings' first line): whole, two lines at most
            TextView k = Theme.text(activity, Theme.T_SMALL, Theme.REGULAR, Theme.TEXT_DIM, key);
            k.setMaxLines(2);
            k.setEllipsize(android.text.TextUtils.TruncateAt.END);
            k.setLineSpacing(0, 1.15f);
            k.setPadding(Theme.ROW_INSET, Theme.S1, Theme.ROW_INSET, Theme.S2);
            list.addView(k, new LinearLayout.LayoutParams(-1, -2));
            return;
        }
        LinearLayout l = new LinearLayout(activity);
        l.setOrientation(LinearLayout.HORIZONTAL);
        l.setGravity(Gravity.CENTER_VERTICAL);
        l.setPadding(Theme.ROW_INSET, Theme.S1, Theme.ROW_INSET, Theme.S1);
        l.setMinimumHeight(60);
        TextView k = Theme.oneLine(Theme.text(activity, Theme.T_BODY, Theme.MEDIUM, Theme.TEXT, key));
        l.addView(k, new LinearLayout.LayoutParams(0, -2, 1));
        TextView v = Theme.text(activity, Theme.T_SMALL, Theme.REGULAR, Theme.TEXT_DIM, value);
        v.setMaxLines(2);
        v.setEllipsize(android.text.TextUtils.TruncateAt.END);
        v.setLineSpacing(0, 1.15f);
        v.setGravity(Gravity.END);
        l.addView(v, new LinearLayout.LayoutParams(0, -2, 1.6f));
        list.addView(l, new LinearLayout.LayoutParams(-1, -2));
    }

    private void onItem(Item item) {
        switch (item) {
        case RESUME:
            close();
            break;
        case SETTINGS:
            showPage(Page.SETTINGS, Item.PERF);
            break;
        case CONTROLS:
            showPage(Page.CONTROLS, Item.BACK);
            break;
        case DISC:
            showPage(Page.DISCS, null);
            break;
        case SCREENSHOT:
            // Closed first: the player sees the game the picture is of (the BMP is the game's frame alone anyway).
            Log.i(LauncherActivity.TAG, "shell: menu: screenshot");
            close();
            handler.postDelayed(host::screenshot, animate ? ANIM_MS + 20 : 0);
            break;
        case SHARE_LOG:
            Log.i(LauncherActivity.TAG, "shell: menu: share log");
            host.shareLog();
            break;
        case LAUNCHER:
            backToLauncher();
            break;
        case QUIT:
            quitPressed();
            break;
        case PERF:
            setPerf(!perfOn);
            break;
        case VOLUME:
            setVolume(volume == 0 ? volumeBeforeMute : 0);
            break;
        case BOTTOM:
            stepBottom(1);
            break;
        case LANGUAGE:
            stepLanguage(1);
            break;
        case WIDESCREEN:
            stepVideo(GameVideo.Setting.WIDESCREEN, 1);
            break;
        case P480:
            stepVideo(GameVideo.Setting.P480, 1);
            break;
        case SCALE:
            stepConsoleVideo(ConsoleVideo.Setting.SCALE, 1);
            break;
        case FILTER:
            stepConsoleVideo(ConsoleVideo.Setting.FILTER, 1);
            break;
        case UPSCALE:
            stepConsoleVideo(ConsoleVideo.Setting.UPSCALE, 1);
            break;
        case VK_DRIVER:
            stepVkDriver(1);
            break;
        case TOUCH:
            toggleTouch();
            break;
        case TOUCH_EDIT:
            Log.i(LauncherActivity.TAG, "shell: menu: edit touch layout");
            close(false);
            host.editTouchLayout();
            break;
        case STATE_SAVE:
            showPage(Page.SAVE_SLOTS, null);
            break;
        case STATE_LOAD:
            showPage(Page.LOAD_SLOTS, null);
            break;
        case FAST_FORWARD:
            toggleFastForward();
            break;
        case AUTO_RESUME:
            setAutoResume(!GameStore.autoResume(activity));
            break;
        case GAME_SETTINGS:
            showPage(Page.GAME_SETTINGS, null);
            break;
        case CHEATS:
            showPage(Page.CHEATS, null);
            break;
        case CHEAT_ADD: {
            String key = titleId();
            Console con = Console.ofKey(key);
            Log.i(LauncherActivity.TAG, "shell: menu: add a cheat code for " + key);
            host.addCheat(con, key, name -> {
                showPage(Page.CHEATS, Item.CHEAT_ADD);
                flash(activity.getString(R.string.CHEATS_ADDED, name), true);
            });
            break;
        }
        case CHEAT_DELETE:
            showPage(Page.CHEATS_DELETE, null);
            break;
        case GS_RESET:
            GameSettings.reset(activity.getFilesDir(), titleId());
            Log.i(LauncherActivity.TAG, "shell: menu: game settings of " + titleId() + " reset to Settings");
            afterChange();
            showPage(Page.GAME_SETTINGS, Item.GS_RESET);
            flash(s(R.string.GS_RESET_DONE), true);
            break;
        case RESTART:
            restartPressed();
            break;
        case BACK:
            back();
            break;
        }
    }

    /**
     * The game stays alive (the runtime maps its memory once per process: a new start would mean a new process) and
     * held: the drawer goes at once, the MENU hold stays until onStop, where the background hold has taken over, so
     * the game never runs in between. The launcher's Play brings this task back, and the game goes on.
     */
    private void backToLauncher() {
        Log.i(LauncherActivity.TAG, "shell: menu: back to launcher (the game waits)");
        open = false;
        stopNav();
        disarmQuit();
        handler.removeCallbacks(sessionTick);
        downWhileOpen.clear();
        if (anim != null)
            anim.cancel();
        hideNow();
        host.releaseOverlay();
        host.restoreGameFocus();
        launcherPending = true;
        host.backToLauncher();
    }

    private void quitPressed() {
        long now = clock.getAsLong();
        if (quitArmedAt >= 0 && now - quitArmedAt <= QUIT_ARM_MS) {
            Log.i(LauncherActivity.TAG, "shell: menu: quit (confirmed)");
            host.releaseOverlay();
            autoSave("quit");
            host.quit();
            return;
        }
        quitArmedAt = now;
        Widgets.MenuRow r = rows.get(Item.QUIT);
        if (r != null) {
            r.setTitle(s(R.string.MENU_QUIT_CONFIRM));
            r.setSubtitle(s(autoResumes() ? R.string.MENU_QUIT_CONFIRM_KEPT : R.string.MENU_QUIT_CONFIRM_SUB));
        }
        feedback.play(UiFeedback.Cue.ERROR, r);
        handler.removeCallbacks(quitDisarm);
        handler.postDelayed(quitDisarm, QUIT_ARM_MS);
    }

    private void disarmQuit() {
        if (quitArmedAt < 0)
            return;
        quitArmedAt = -1;
        handler.removeCallbacks(quitDisarm);
        Widgets.MenuRow r = rows.get(Item.QUIT);
        if (r != null) {
            r.setTitle(s(R.string.MENU_QUIT));
            r.setSubtitle(null);
        }
    }

    // ---- Settings page -----------------------------------------------------------------------------------------

    void setPerf(boolean on) {
        perfOn = on;
        GameStore.setPerfOverlay(activity, on);
        Log.i(LauncherActivity.TAG, "shell: menu: performance overlay " + (on ? "on" : "off"));
        handler.removeCallbacks(perfTick);
        updatePerf();
        if (on && started)
            handler.postDelayed(perfTick, PERF_TICK_MS);
        updateValues();
    }

    void setVolume(int v) {
        v = Math.max(0, Math.min(100, v));
        if (v == volume)
            return;
        volume = v;
        if (v > 0)
            volumeBeforeMute = v;
        host.setVolume(v);
        GameStore.setGameVolume(activity, v);
        updateValues();
    }

    private void stepBottom(int dir) {
        GameStore.BottomScreen[] all = GameStore.BottomScreen.values();
        GameStore.BottomScreen cur = GameStore.bottomScreen(activity);
        GameStore.BottomScreen next = all[((cur.ordinal() + dir) % all.length + all.length) % all.length];
        GameStore.setBottomScreen(activity, next);
        Log.i(LauncherActivity.TAG, "shell: menu: bottom screen " + next);
        host.bottomScreenChanged();
        updateValues();
    }

    /** True when this APK has frame generation (R.bool.xba_lsfg; LauncherActivity's test override too). */
    private boolean lsfgBuilt() {
        return LauncherActivity.lsfgBuiltOverride != null ? LauncherActivity.lsfgBuiltOverride
                                                          : activity.getResources().getBoolean(R.bool.xba_lsfg);
    }

    /**
     * XBA-188: one option of Game settings (GameSettings): « As in Settings (<value>) », then each choice, saved at
     * once; a picture option (ConsoleVideo.LIVE_KEYS) is shown at once, any other waits for « Restart to apply »;
     * the « This game » badge when it differs from Settings'.
     */
    private Widgets.MenuRow gameOptionRow(RecompConfig.Option o, String title) {
        RecompConfig cfg = RecompConfig.load(GameStore.externalFilesDir(activity));
        int gi = GameSettings.globalIndex(cfg, o);
        String[] labels = new String[o.choices.length + 1];
        labels[0] = activity.getString(R.string.ROM_OPT_FOLLOW, s(gi < 0 ? R.string.VAL_CUSTOM : o.choices[gi].label));
        for (int i = 0; i < o.choices.length; i++)
            labels[i + 1] = s(o.choices[i].label);
        Widgets.MenuRow r = new Widgets.MenuRow(activity);
        r.set(title, null);
        r.setChoices(labels);
        Runnable refresh = () -> {
            RecompConfig game = GameSettings.load(activity, titleId());
            int pos = GameSettings.position(cfg, game, o);
            r.setValue(pos < 0 ? s(R.string.VAL_CUSTOM) : labels[pos]);
            r.setBadge(GameSettings.differs(cfg, game, o) ? s(R.string.GS_BADGE) : null, Theme.ACCENT);
            r.invalidate();
        };
        refresh.run();
        r.cycler = dir -> {
            RecompConfig game = GameSettings.load(activity, titleId());
            int pos = GameSettings.step(GameSettings.position(cfg, game, o), o, dir);
            GameSettings.setPosition(game, o, pos);
            GameSettings.save(game);
            refresh.run();
            feedback.play(UiFeedback.Cue.MOVE, r);
            Log.i(LauncherActivity.TAG, "shell: menu: " + String.join("+", o.keys) + " " + (pos == 0 ? "as in Settings"
                                        : activity.getResources().getResourceEntryName(o.choices[pos - 1].label))
                                        + " for " + titleId() + (ConsoleVideo.isLive(o) ? " (live)" : " (restart)"));
            afterChange();
        };
        r.setOnClickListener(v -> r.cycler.step(1));
        list.addView(r, new LinearLayout.LayoutParams(-1, ROW_H));
        return r;
    }

    // ---- Live picture, Restart to apply -------------------------------------------------------------------------

    /** After a setting changed: the picture's keys go to the game at once, the others show « Restart to apply ». */
    private void afterChange() {
        syncPicture("menu");
        updateRestart();
    }

    /** The picture the game's settings give now (ConsoleVideo.livePicture). */
    private ConsoleVideo.Picture currentPicture() {
        String key = titleId();
        Console con = key == null ? Console.XBOX : Console.ofKey(key);
        return ConsoleVideo.livePicture(con, RecompConfig.load(GameStore.externalFilesDir(activity)),
                                        key == null ? null : GameSettings.load(activity, key));
    }

    /** Sends the picture to the game when it is not the one it shows. */
    private void syncPicture(String why) {
        ConsoleVideo.Picture p = currentPicture();
        if (p.equals(livePicture))
            return;
        livePicture = p;
        Log.i(LauncherActivity.TAG, "shell: menu: picture applied live (" + why + "): " + p);
        host.setPicture(p);
    }

    /**
     * What the game read at its start of everything that is not live: its environment (GameSettings.runEnv) but the
     * picture keys, an unset key as recomp.cfg has it (the runtime reads that file at its start; an empty value and
     * none are the same default), and an N64 game's
     * Vulkan driver. « Restart to apply » shows while it differs from the one taken when the game started.
     */
    private String restartSnapshot() {
        String key = titleId();
        if (key == null)
            return "";
        Console con = Console.ofKey(key);
        RecompConfig cfg = RecompConfig.load(GameStore.externalFilesDir(activity));
        RecompConfig game = GameSettings.load(activity, key);
        GameSettings.Env env = GameSettings.runEnv(con, cfg, game, Locale.getDefault(), lsfgBuilt());
        StringBuilder b = new StringBuilder();
        for (String[] kv : env.vars) {
            if (ConsoleVideo.isLive(kv[0]) || kv[0].equals(GameSettings.SKIP_ENV))
                continue;
            // Unset: the game's « default », else recomp.cfg's line; « » and none are both the runtime's default.
            String v = kv[1] != null ? kv[1] : game.has(kv[0]) || cfg == null ? null : cfg.get(kv[0]);
            if (v != null && v.trim().isEmpty())
                v = null;
            b.append(kv[0]).append('=').append(v).append('\n');
        }
        if (vkDriverShown())
            b.append("vk=").append(VkDrivers.of(activity).gameChoice(key));
        return b.toString();
    }

    /** True while a setting the game reads at its start differs from what it started with. */
    boolean restartPending() {
        return restartBase != null && !restartBase.equals(restartSnapshot());
    }

    /** « Restart to apply », at the top of Settings and Game settings, shown while a change waits for it. */
    private void restartRow() {
        if (titleId() == null)
            return;
        Widgets.MenuRow r = row(Item.RESTART, s(R.string.MENU_RESTART_APPLY));
        r.setSubtitle(s(R.string.MENU_RESTART_APPLY_SUB));
        r.setVisibility(restartPending() ? View.VISIBLE : View.GONE);
    }

    private void updateRestart() {
        Widgets.MenuRow r = rows.get(Item.RESTART);
        if (r == null)
            return;
        boolean on = restartPending();
        if (!on)
            disarmRestart();
        r.setVisibility(on ? View.VISIBLE : View.GONE);
    }

    /** A second press within {@link #QUIT_ARM_MS} restarts (the automatic slot saved first when resuming applies). */
    private void restartPressed() {
        long now = clock.getAsLong();
        if (restartArmedAt >= 0 && now - restartArmedAt <= QUIT_ARM_MS) {
            Log.i(LauncherActivity.TAG, "shell: menu: restart to apply (confirmed)");
            host.releaseOverlay();
            autoSave("restart");
            host.restartGame();
            return;
        }
        restartArmedAt = now;
        Widgets.MenuRow r = rows.get(Item.RESTART);
        if (r != null) {
            r.setTitle(s(R.string.MENU_RESTART_CONFIRM));
            r.setSubtitle(s(autoResumes() ? R.string.MENU_RESTART_KEPT : R.string.MENU_QUIT_CONFIRM_SUB));
        }
        feedback.play(UiFeedback.Cue.ERROR, r);
        handler.removeCallbacks(restartDisarm);
        handler.postDelayed(restartDisarm, QUIT_ARM_MS);
    }

    private void disarmRestart() {
        if (restartArmedAt < 0)
            return;
        restartArmedAt = -1;
        handler.removeCallbacks(restartDisarm);
        Widgets.MenuRow r = rows.get(Item.RESTART);
        if (r != null) {
            r.setTitle(s(R.string.MENU_RESTART_APPLY));
            r.setSubtitle(s(R.string.MENU_RESTART_APPLY_SUB));
        }
    }

    /** Settings' « Picture shader » option (RECOMP_SHADER), or null. */
    static RecompConfig.Option shaderOption() {
        for (RecompConfig.Option o : RecompConfig.OPTIONS)
            if (o.title == R.string.OPT_CSHADER)
                return o;
        return null;
    }

    /**
     * The running game's title ID, read once: a GBA, PS1 or N64 game's library key, else the Xbox game's from its XBE
     * (GameLanguage.runningTitleId, XBA-181); null hides the per-game rows (nothing to key a choice on).
     */
    private String titleId() {
        if (!titleIdRead) {
            titleIdRead = true;
            titleIdValue = GameLanguage.runningTitleId(activity);
        }
        return titleIdValue;
    }

    private String[] consoleLabels(ConsoleVideo.Setting which) {
        String[] out = new String[which.values.length + 1];
        out[0] = s(R.string.MENU_VIDEO_FOLLOW);
        for (int i = 0; i < which.values.length; i++)
            out[i + 1] = s(which.labels[i]);
        return out;
    }

    /**
     * A console game's scale, filter (XBA-181) or internal resolution (XBA-184): remembered per game; scale and filter
     * shown at once, the resolution after « Restart to apply ».
     */
    private void stepConsoleVideo(ConsoleVideo.Setting which, int dir) {
        String v = ConsoleVideo.next(which, ConsoleVideo.gameChoice(activity, titleId(), which), dir);
        ConsoleVideo.setGameChoice(activity, titleId(), which, v);
        Log.i(LauncherActivity.TAG, "shell: menu: " + which.env + " " + (v == null ? "as in Settings" : v)
                                    + " for " + titleId() + (ConsoleVideo.isLive(which.env) ? " (live)" : " (restart)"));
        updateValues();
        afterChange();
    }

    /** An N64 game in XRecomp (a game's own app follows XRecomp's choice, VkDriverShare: no row there). */
    private boolean vkDriverShown() {
        return titleId() != null && Console.ofKey(titleId()) == Console.N64 && TitleApp.ownTitleId(activity) == null;
    }

    /** This N64 game's Vulkan driver (VkDrivers.setGameChoice): remembered per game, after « Restart to apply ». */
    private void stepVkDriver(int dir) {
        String v = VkDriverStore.stepGameChoice(VkDrivers.of(activity), titleId(), dir);
        Log.i(LauncherActivity.TAG, "shell: menu: vulkan driver " + (v == null ? "as in Settings" : v)
                                    + " for " + titleId() + " (restart)");
        updateValues();
        afterChange();
    }

    /** The Vulkan driver row's value: the game's choice, and a short warning when that driver went wrong last time. */
    private String vkDriverLabel() {
        VkDrivers drivers = VkDrivers.of(activity);
        String label = VkDriverStore.gameChoiceLabel(activity, drivers, drivers.gameChoice(titleId()));
        VkDrivers.Driver d = drivers.resolve(titleId());
        int badge = d == null ? 0 : VkDriverStore.lastStartBadge(VkDriverStore.lastStart(drivers, d.id), false);
        return badge == 0 ? label : label + " · " + s(badge);
    }

    /** The Language row's position: 0 = as in Settings, then GameLanguage.MENU_ORDER. */
    private int languagePos() {
        int idx = GameLanguage.indexOf(GameLanguage.titleChoice(activity, titleId()));
        for (int i = 0; i < GameLanguage.MENU_ORDER.length; i++) {
            if (GameLanguage.MENU_ORDER[i] == idx)
                return i + 1;
        }
        return 0;
    }

    /** This game's language (XBA-77): remembered per title ID, after « Restart to apply » (read at the start). */
    private void stepLanguage(int dir) {
        int n = GameLanguage.MENU_ORDER.length + 1;
        int pos = ((languagePos() + dir) % n + n) % n;
        String code = pos == 0 ? null : GameLanguage.CODES[GameLanguage.MENU_ORDER[pos - 1]];
        GameLanguage.setTitleChoice(activity, titleId(), code);
        Log.i(LauncherActivity.TAG, "shell: menu: language " + (code == null ? "as in Settings" : code)
                                    + " for " + titleId() + " (restart)");
        updateValues();
        afterChange();
    }

    /** This game's widescreen or 480p (XBA-134): remembered per title ID, after « Restart to apply ». */
    private void stepVideo(GameVideo.Setting which, int dir) {
        Boolean on = GameVideo.next(GameVideo.titleChoice(activity, titleId(), which), dir);
        GameVideo.setTitleChoice(activity, titleId(), which, on);
        Log.i(LauncherActivity.TAG, "shell: menu: " + which.env + " "
                                    + (on == null ? "as in Settings" : on ? "on" : "off")
                                    + " for " + titleId() + " (restart)");
        updateValues();
        afterChange();
    }

    private String videoLabel(GameVideo.Setting which) {
        Boolean on = GameVideo.titleChoice(activity, titleId(), which);
        return s(on == null ? R.string.MENU_VIDEO_FOLLOW : on ? R.string.MENU_ON : R.string.MENU_OFF);
    }

    private void toggleTouch() {
        Boolean on = host.touchShown();
        if (on == null)
            return;
        host.setTouchShown(!on);
        Log.i(LauncherActivity.TAG, "shell: menu: on-screen pad " + (on ? "hidden" : "shown"));
        updateValues();
    }

    private void updateValues() {
        Widgets.MenuRow r = rows.get(Item.PERF);
        if (r != null)
            r.setValue(s(perfOn ? R.string.MENU_ON : R.string.MENU_OFF));
        r = rows.get(Item.VOLUME);
        if (r != null)
            r.setValue(volume == 0 ? s(R.string.MENU_MUTED) : volumeLabel(volume));
        r = rows.get(Item.BOTTOM);
        if (r != null) {
            GameStore.BottomScreen b = GameStore.bottomScreen(activity);
            int label = b == GameStore.BottomScreen.PANEL ? R.string.VAL_PANEL
                        : b == GameStore.BottomScreen.BLACK ? R.string.VAL_BLACK : R.string.MENU_SCREEN_OFF;
            r.setValue(s(label));
        }
        r = rows.get(Item.LANGUAGE);
        if (r != null) {
            int pos = languagePos();
            r.setValue(pos == 0 ? s(R.string.MENU_LANGUAGE_FOLLOW)
                                : GameLanguage.NAMES[GameLanguage.MENU_ORDER[pos - 1]]);
        }
        r = rows.get(Item.WIDESCREEN);
        if (r != null)
            r.setValue(videoLabel(GameVideo.Setting.WIDESCREEN));
        r = rows.get(Item.P480);
        if (r != null)
            r.setValue(videoLabel(GameVideo.Setting.P480));
        for (ConsoleVideo.Setting cv : ConsoleVideo.Setting.values()) {
            if (cv == ConsoleVideo.Setting.FRAMEGEN)
                continue;   // Home › Options only (XBA-186): no in-game row
            r = rows.get(cv == ConsoleVideo.Setting.SCALE ? Item.SCALE
                         : cv == ConsoleVideo.Setting.FILTER ? Item.FILTER : Item.UPSCALE);
            if (r != null)
                r.setValue(s(ConsoleVideo.label(cv, ConsoleVideo.gameChoice(activity, titleId(), cv))));
        }
        r = rows.get(Item.VK_DRIVER);
        if (r != null)
            r.setValue(vkDriverLabel());
        r = rows.get(Item.FAST_FORWARD);
        if (r != null)
            r.setValue(s(host.fastForward() ? R.string.MENU_ON : R.string.MENU_OFF));
        r = rows.get(Item.AUTO_RESUME);
        if (r != null)
            r.setValue(s(GameStore.autoResume(activity) ? R.string.MENU_ON : R.string.MENU_OFF));
        r = rows.get(Item.TOUCH);
        Boolean touch = host.touchShown();
        if (r != null && touch != null)
            r.setValue(s(touch ? R.string.MENU_TOUCH_SHOWN : R.string.MENU_TOUCH_HIDDEN));
    }

    private String volumeLabel(int v) {
        return activity.getString(R.string.MENU_PERCENT, v);
    }

    // ---- Header, overlay, messages -----------------------------------------------------------------------------

    private String gameTitle() {
        // A GBA, PS1 or N64 game: its own title. The chosen Xbox game's (chosenTitle) stays remembered while one
        // runs and is not this game's.
        String t = GameStore.launchedRom(activity) != null ? GameStore.launchedRomTitle(activity)
                                                           : GameStore.chosenTitle(activity);
        return t == null || t.isEmpty() ? s(R.string.app_name) : t;
    }

    private void updateSession() {
        sessionV.setText(activity.getString(R.string.MENU_STATUS, hoursMinutes(clock.getAsLong() - startedAt)));
    }

    /** « 0:42 », « 1:05 »: hours and minutes. */
    static String hoursMinutes(long ms) {
        long m = Math.max(0, ms) / 60_000;
        return String.format(Locale.ROOT, "%d:%02d", m / 60, m % 60);
    }

    private void updatePerf() {
        if (!perfOn) {
            perfV.setVisibility(View.GONE);
            return;
        }
        int[] st = host.stats();
        String text;
        if (st == null || st[0] <= 0) {
            text = s(R.string.PANEL_FPS_NONE);
        } else {
            float fps = st[0] / 100f;
            text = activity.getString(R.string.PANEL_FPS, String.format(Locale.ROOT, "%.1f", fps),
                                      String.format(Locale.ROOT, "%.1f", 1000f / fps));
        }
        perfV.setText(text);
        perfV.setVisibility(View.VISIBLE);
    }

    String perfText() {
        return perfV.getVisibility() == View.VISIBLE ? perfV.getText().toString() : null;
    }

    /** A short message at the top of the screen (« Saved … »), for FLASH_MS, over the game or the menu. */
    void flash(String text, boolean ok) {
        flashV.set(text, ok ? Theme.OK : Theme.WARN);
        flashBox.setVisibility(View.VISIBLE);
        handler.removeCallbacks(flashEnd);
        handler.postDelayed(flashEnd, FLASH_MS);
        if (ok)
            feedback.play(UiFeedback.Cue.DONE, null);
    }

    private void hideFlash() {
        flashBox.setVisibility(View.GONE);
    }

    String flashText() {
        return flashBox.getVisibility() == View.VISIBLE ? flashV.label() : null;
    }

    // ---- Helpers -----------------------------------------------------------------------------------------------

    private String s(int id) {
        return activity.getString(id);
    }

    private static FrameLayout.LayoutParams match() {
        return new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
    }

    private static LinearLayout.LayoutParams lpTop(int w, int h, int top) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(w, h);
        p.topMargin = top;
        return p;
    }

    private static GradientDrawable pill() {
        GradientDrawable g = new GradientDrawable();
        g.setColor(Theme.alpha(Theme.BG_BOTTOM, 0.8f));
        g.setCornerRadius(30);
        return g;
    }
}
