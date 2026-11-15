package fr.adserver.xrecomp;

import android.app.Activity;
import android.content.Context;
import android.hardware.input.InputManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Toast;

/**
 * The on-screen pad in the game (XBA-42): ShellActivity owns one. It puts a {@link TouchPadView} over SDL's surface
 * (under the in-game menu), shows it per {@link TouchSettings#visible} — Auto: while no controller is connected
 * (InputManager's device list, watched) and none has been used since — and sends its state through the
 * {@link OverlayMixer} to input_overlay.h. Never shown while the companion panel is on the second screen.
 *
 * <p>XBA-212: when Auto (or the panel) hides it — the AYN Thor's built-in controls count as a controller — a tap on
 * the game's screen (outside the in-game menu's swipe strip) reveals it ({@link #takeTouch}); that first tap only
 * reveals, nothing reaches the game. It stays until no finger touched the screen for {@link #REVEAL_MS} or a physical
 * button is pressed. Hidden that way, the player gets a hint once per process (log and toast). The touch layout
 * editor never outlives the moment: it is cancelled on start, stop, resume and when the in-game menu opens.
 */
final class TouchControls implements InputManager.InputDeviceListener {
    /** Whether a game controller is connected now (PadDevices in the game, a fake in the tests). */
    interface Devices {
        boolean any();
    }

    private final Activity activity;
    private final OverlayMixer mixer;
    private final Devices devices;
    final TouchPadView view;
    private TouchSettings settings = new TouchSettings();
    private boolean used, panel, shown, checked;
    /** The in-game menu's toggle for this session: shown or hidden whatever the mode, null: the mode decides. */
    private Boolean forced;
    /** The running game's key (GameSettings), for its own touch layout (XBA-207); null: the console's. */
    private String gameKey;
    private TouchPad.Layout padLayout = TouchPad.Layout.XBOX;
    /** The touch layout editor while it is open over the game, else null. */
    TouchEditor editor;

    /** How long a pad revealed by a tap stays without a touch (XBA-212). */
    static final long REVEAL_MS = 10_000;
    /** Shown by a tap while Auto or the panel would hide it; the rest of the revealing gesture is swallowed. */
    private boolean revealed, swallowing;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable revealTimeout = new Runnable() {
        @Override
        public void run() {
            if (!revealed)
                return;
            if (view.pad.fingers() > 0) {   // still held: wait for the fingers to lift
                handler.postDelayed(this, REVEAL_MS);
                return;
            }
            revealed = false;
            update("no touch for " + REVEAL_MS / 1000 + " s");
        }
    };
    /** The hints already given in this process (XBA-212): once each. */
    private static boolean hintedController, hintedPanel;

    TouchControls(Activity a, OverlayMixer mixer, Devices devices) {
        this.activity = a;
        this.mixer = mixer;
        this.devices = devices;
        view = new TouchPadView(a, mixer::touch);
        view.setVisibility(View.GONE);
        view.fallBackListener = why -> Log.w(LauncherActivity.TAG, "shell: touch layout " + padLayout
                                                                   + " set aside for the defaults: " + why);
    }

    /** The console's layout (XBA-158 / XBA-168 / XBA-174): its own buttons, set before start. */
    void layout(TouchPad.Layout l) {
        padLayout = l;
        view.setLayout(l);
    }

    /** The running game's key: its own touch layout wins over the console's (TouchLayout.resolve). */
    void game(String key) {
        gameKey = key;
    }

    /** The player's layout for this console and game (XBA-207), on the pad. */
    private void loadLayout() {
        TouchLayout.Resolved r = TouchLayout.resolve(activity.getFilesDir(), gameKey, padLayout);
        view.setCustom(r.layout);
        if (!r.layout.isEmpty())
            Log.i(LauncherActivity.TAG, "shell: touch layout " + padLayout + (r.perGame ? " (this game's)" : "")
                                        + ", " + r.layout.entries.size() + " control(s) changed");
        if (r.repaired > 0)
            Log.w(LauncherActivity.TAG, "shell: touch layout " + padLayout + ": " + r.repaired
                                        + " invalid entr" + (r.repaired == 1 ? "y" : "ies") + " repaired");
    }

    /**
     * Cancels the touch layout editor if it is open (the layout before it kept): the game going to the background,
     * starting, resuming, the in-game menu opening (XBA-212: it must never be left over the game).
     */
    void closeEditor(String why) {
        if (editor != null)
            editor.cancel(why);
        view.setAlpha(1f);   // whatever happened to the editor, the live pad is drawn again
    }

    /**
     * Opens the touch layout editor over the game (the in-game menu's « Edit touch layout »): the live pad hidden
     * under it, the saved layout on the pad at once. done runs when it closes (the activity lets the game go).
     */
    void edit(Runnable done) {
        if (editor != null)
            return;
        editor = new TouchEditor(activity, padLayout, gameKey, settings, true, saved -> {
            if (saved != null)
                view.setCustom(saved);
            ViewGroup parent = (ViewGroup)editor.getParent();
            if (parent != null)
                parent.removeView(editor);
            editor = null;
            view.setAlpha(1f);
            done.run();
        });
        view.setAlpha(0f);   // the editor draws its own copy
        view.releaseAll();
        activity.addContentView(editor, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                                   ViewGroup.LayoutParams.MATCH_PARENT));
    }

    /** Adds the view (before the in-game menu's, so the menu draws above it). */
    void attach() {
        activity.addContentView(view, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                                 ViewGroup.LayoutParams.MATCH_PARENT));
    }

    /** onStart: the settings again (the launcher may have changed them), the device watch, the decision. */
    void start() {
        closeEditor("game start");   // never restored on a start
        revealed = swallowing = false;
        handler.removeCallbacks(revealTimeout);
        settings = TouchSettings.load(activity);
        view.apply(settings);
        loadLayout();
        InputManager im = (InputManager)activity.getSystemService(Context.INPUT_SERVICE);
        if (im != null)
            im.registerInputDeviceListener(this, new Handler(Looper.getMainLooper()));
        update("start");
    }

    /** onStop: hidden, released, the watch off. */
    void stop() {
        closeEditor("background");
        revealed = swallowing = false;
        handler.removeCallbacks(revealTimeout);
        InputManager im = (InputManager)activity.getSystemService(Context.INPUT_SERVICE);
        if (im != null)
            im.unregisterInputDeviceListener(this);
        show(false, "stop");
    }

    /** The companion panel came or went on the second screen. */
    void panelShowing(boolean on) {
        if (panel != on) {
            panel = on;
            update(on ? "panel shown" : "panel gone");
        }
    }

    /**
     * A library check's run (XBA-130): never shown, whatever the setting. The check drives pad 1 from its own script
     * (RECOMP_PAD_LIVE) and measures the game as played with a controller; an on-screen pad would add a pad 0 of its
     * own and cover the screen the check grades.
     */
    void checkRun(boolean on) {
        if (checked != on) {
            checked = on;
            update(on ? "library check" : "not a library check");
        }
    }

    /** A key or a motion reached the activity: a controller's hides an Auto pad until no controller is left. */
    void onInput(int source) {
        boolean pad = (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                      || (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
        if (pad && !used) {
            used = true;
            update("controller used");
        }
    }

    void onKey(KeyEvent e) {
        if (e.getAction() == KeyEvent.ACTION_DOWN && physical(e.getSource()))
            unreveal("button pressed");
        onInput(e.getSource());
    }

    void onMotion(MotionEvent e) {
        if (physical(e.getSource()) && pushed(e))
            unreveal("stick or trigger pushed");
        onInput(e.getSource());
    }

    private static boolean physical(int source) {
        return (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
               || (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
               || (source & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD;
    }

    /** A joystick motion that is a real push (half way), not a resting stick's noise. */
    private static boolean pushed(MotionEvent e) {
        int[] axes = { MotionEvent.AXIS_X, MotionEvent.AXIS_Y, MotionEvent.AXIS_Z, MotionEvent.AXIS_RZ,
                       MotionEvent.AXIS_HAT_X, MotionEvent.AXIS_HAT_Y, MotionEvent.AXIS_LTRIGGER,
                       MotionEvent.AXIS_RTRIGGER, MotionEvent.AXIS_BRAKE, MotionEvent.AXIS_GAS };
        for (int a : axes)
            if (Math.abs(e.getAxisValue(a)) > 0.5f)
                return true;
        return false;
    }

    /** A pad revealed by a tap hidden again (a physical control used). */
    private void unreveal(String why) {
        if (!revealed)
            return;
        revealed = false;
        handler.removeCallbacks(revealTimeout);
        update(why);
    }

    /** Whether a tap may reveal the pad now: hidden by Auto or the panel, not Off, not hidden from the menu. */
    private boolean mayReveal() {
        return !checked && !shown && settings.mode != TouchSettings.Mode.OFF && !Boolean.FALSE.equals(forced);
    }

    boolean revealed() {
        return revealed;
    }

    /**
     * The game activity's dispatchTouchEvent, first (XBA-212): true when the event is taken here, so it goes no
     * further. The touch layout editor takes everything while open. A finger down on the game's screen while Auto or
     * the panel hides the pad reveals it (outside the in-game menu's swipe strip, menuOpen false), and the rest of
     * that gesture is swallowed: the first tap only reveals. A touch while revealed keeps it up.
     */
    boolean takeTouch(MotionEvent e, boolean menuOpen) {
        if (editor != null) {
            editor.dispatchTouchEvent(e);
            return true;
        }
        int a = e.getActionMasked();
        if (a == MotionEvent.ACTION_DOWN) {
            onTouch();
            swallowing = false;
            if (!menuOpen && mayReveal() && !inMenuStrip(e.getX())) {
                revealed = true;
                update("tap: revealed");
                swallowing = shown;
            }
        }
        if (revealed && (a == MotionEvent.ACTION_DOWN || a == MotionEvent.ACTION_POINTER_DOWN)) {
            handler.removeCallbacks(revealTimeout);
            handler.postDelayed(revealTimeout, REVEAL_MS);
        }
        if (swallowing) {
            if (a == MotionEvent.ACTION_UP || a == MotionEvent.ACTION_CANCEL)
                swallowing = false;
            return true;
        }
        return false;
    }

    /** The in-game menu's swipe strip on the left edge (MenuTriggers.EdgeSwipe: 3 % of the width, at least 20 dp). */
    private boolean inMenuStrip(float x) {
        View root = activity.getWindow().getDecorView();
        float density = activity.getResources().getDisplayMetrics().density;
        return x <= Math.max(20 * density, root.getWidth() * MenuTriggers.EdgeSwipe.EDGE_FRAC);
    }

    /** A touch on the screen with no controller connected brings an Auto pad back after a controller was used. */
    void onTouch() {
        if (used && !devices.any()) {
            used = false;
            update("touch, no controller");
        }
    }

    boolean shown() {
        return shown;
    }

    TouchSettings settings() {
        return settings;
    }

    @Override
    public void onInputDeviceAdded(int id) {
        update("device added");
    }

    @Override
    public void onInputDeviceRemoved(int id) {
        if (!devices.any())
            used = false;
        update("device removed");
    }

    @Override
    public void onInputDeviceChanged(int id) {
        update("device changed");
    }

    /** The in-game menu's toggle: shown or hidden for the rest of the session (never with the panel up). */
    void force(boolean on) {
        forced = on;
        update(on ? "menu: shown" : "menu: hidden");
    }

    void update(String why) {
        boolean auto = forced != null ? forced && !panel
                                      : TouchSettings.visible(settings.mode, devices.any(), used, panel);
        boolean want = auto || (revealed && settings.mode != TouchSettings.Mode.OFF && !Boolean.FALSE.equals(forced));
        if (!want)
            revealed = false;
        show(!checked && want, why);
        if (!checked && !want && forced == null && settings.mode != TouchSettings.Mode.OFF)
            hint();
    }

    /** Once per process: why the pad is not there, and how to bring it (XBA-212). */
    private void hint() {
        boolean byPanel = panel;
        if (byPanel ? hintedPanel : hintedController)
            return;
        if (byPanel)
            hintedPanel = true;
        else
            hintedController = true;
        String text = activity.getString(byPanel ? R.string.TOUCH_HINT_PANEL : R.string.TOUCH_HINT_CONTROLLER);
        Log.i(LauncherActivity.TAG, "shell: " + text);
        try {
            Toast.makeText(activity, text, Toast.LENGTH_LONG).show();
        } catch (RuntimeException e) {
            Log.w(LauncherActivity.TAG, "shell: touch hint: no toast", e);
        }
    }

    /** For the tests: the hints may be given again. */
    static void resetHints() {
        hintedController = hintedPanel = false;
    }

    private void show(boolean on, String why) {
        if (on == shown && (on ? view.getVisibility() == View.VISIBLE : view.getVisibility() != View.VISIBLE))
            return;
        shown = on;
        view.setVisibility(on ? View.VISIBLE : View.GONE);   // hiding releases every finger
        if (on)
            mixer.touch(view.state());   // on: a connected pad 0 even before a touch
        else
            mixer.touchOff();
        Log.i(LauncherActivity.TAG, "shell: touch controls " + (on ? "shown" : "hidden") + " (" + why + ", "
                                    + settings.mode + ")");
    }
}
