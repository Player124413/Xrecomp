package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertSame;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.Activity;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Shader;
import android.os.Looper;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;

import org.junit.After;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;

import java.io.File;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;

/**
 * The in-game menu (GameMenu): its triggers (Back, Guide, Select + Start held, the edge swipe), the input routing
 * while open (nothing reaches the game), the hold through a fake native bridge, every button released in the game
 * on open and close, Quit's confirmation, Back to launcher, the quick settings; and the drawer rendered over a
 * made-up game frame to build/screenshots/menu-*.png (en, fr, 1080p, 720p).
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "en-w1920dp-h1080dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class GameMenuTest {
    @Rule
    public final TestSupport.StockTheme stockTheme = new TestSupport.StockTheme();

    private static final int PAD = 7;

    /** What would go to libmain and SDL. */
    static final class FakeHost implements GameMenu.Host {
        final List<Boolean> holds = new ArrayList<>();
        final List<KeyEvent> keys = new ArrayList<>();
        final List<MotionEvent> motions = new ArrayList<>();
        int cancels, overlayReleases, focusBacks, screenshots, shareLogs, launchers, quits, bottomChanges;
        int volume = -1;
        boolean gamepadBack, secondScreen = true;
        int[] stats = { 5994, 412 };

        boolean held() {
            return !holds.isEmpty() && holds.get(holds.size() - 1);
        }

        @Override public void hold(boolean on) { holds.add(on); }
        @Override public void sendKeyToGame(KeyEvent e) { keys.add(e); }
        @Override public void sendMotionToGame(MotionEvent e) { motions.add(MotionEvent.obtain(e)); }
        @Override public void cancelGameTouch(MotionEvent e) { cancels++; }
        @Override public void releaseOverlay() { overlayReleases++; }
        @Override public void restoreGameFocus() { focusBacks++; }
        @Override public boolean fromGamepad(KeyEvent e) { return gamepadBack && e.getDeviceId() == PAD; }
        @Override public void setVolume(int percent) { volume = percent; }
        @Override public int[] stats() { return stats; }
        @Override public boolean hasSecondScreen() { return secondScreen; }
        @Override public void bottomScreenChanged() { bottomChanges++; }
        @Override public void screenshot() { screenshots++; }
        @Override public void shareLog() { shareLogs++; }
        @Override public void backToLauncher() { launchers++; }
        @Override public void quit() { quits++; }
        /** The pictures sent live, and the « Restart to apply » restarts. */
        final List<ConsoleVideo.Picture> pictures = new ArrayList<>();
        int restarts;
        @Override public void setPicture(ConsoleVideo.Picture p) { pictures.add(p); }
        @Override public void restartGame() { restarts++; }
        Runnable onMenuOpened;    // XBA-212: what the activity does when the menu opens (closes the touch editor)
        @Override public void menuOpened() { if (onMenuOpened != null) onMenuOpened.run(); }

        Boolean touch;            // the on-screen pad: null = none
        ConsolePadMap console;    // a GBA / PS1 / N64 game's mapping, null = an Xbox game

        @Override public Boolean touchShown() { return touch; }
        @Override public void setTouchShown(boolean on) { touch = on; }
        @Override public ConsolePadMap consoleMap() { return console; }
        /** XBA-166: the discs the game has, the ones chosen, whether a change works. */
        List<Ps1Discs.Disc> discList = List.of();
        final List<Ps1Discs.Disc> chosen = new ArrayList<>();
        boolean discOk;
        @Override public List<Ps1Discs.Disc> discs() { return discList; }
        @Override public boolean changeDisc(Ps1Discs.Disc d) { chosen.add(d); return discOk; }
        /** XBA-182: save states (index 1..4), the codes the next save / load return, fast-forward. */
        boolean states;
        final long[] times = new long[GameMenu.SLOTS + 1];
        final int[][] thumbs = new int[GameMenu.SLOTS + 1][];
        final List<Integer> saved = new ArrayList<>(), loaded = new ArrayList<>();
        int saveCode, loadCode;
        boolean ff;
        @Override public boolean statesSupported() { return states; }
        @Override public long stateTime(int slot) { return times[slot]; }
        @Override public int[] stateThumb(int slot) { return thumbs[slot]; }
        @Override public int saveState(int slot) { saved.add(slot); return saveCode; }
        @Override public int loadState(int slot) { loaded.add(slot); return loadCode; }
        @Override public boolean fastForward() { return ff; }
        @Override public void setFastForward(boolean on) { ff = on; }
        /** The native side's last quick save / load / resume event, null = none. */
        int[] event;
        @Override public int[] stateEvent() { return event; }
        /** XBA-197: Add a code asked (the game's key), and the name the fake dialog then adds. */
        final List<String> cheatAdds = new ArrayList<>();
        String cheatToAdd;
        @Override public void addCheat(Console con, String key, java.util.function.Consumer<String> added) {
            cheatAdds.add(key);
            if (cheatToAdd != null) {
                Cheats c = Cheats.load(RuntimeEnvironment.getApplication().getFilesDir(), key);
                c.add(cheatToAdd, Cheats.normalize("8003A2C4 0063"), "x");
                c.save();
                added.accept(cheatToAdd);
            }
        }
    }

    /** A stand-in for the game's frame (ours: gradients and shapes), so the dimming shows in the screenshots. */
    static final class FakeGame extends View {
        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);

        FakeGame(Context c) {
            super(c);
        }

        @Override
        protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight();
            p.setShader(new LinearGradient(0, 0, 0, h, 0xFF2E6FB8, 0xFFE8B66A, Shader.TileMode.CLAMP));
            c.drawRect(0, 0, w, h, p);
            p.setShader(null);
            p.setColor(0xFF2F5D2A);
            c.drawRect(0, h * 0.72f, w, h, p);
            p.setColor(0xFFF4E3B0);
            c.drawCircle(w * 0.78f, h * 0.24f, h * 0.09f, p);
            p.setColor(0xFF7A3B2E);
            c.drawRect(w * 0.42f, h * 0.48f, w * 0.58f, h * 0.72f, p);
            p.setColor(0xFFFFFFFF);
            p.setTextSize(h * 0.05f);
            c.drawText("GAME FRAME 640 x 480", w * 0.36f, h * 0.12f, p);
        }
    }

    private Activity activity;
    private FakeHost host;
    private GameMenu menu;

    @Before
    public void setUp() {
        build();
    }

    private void build() {
        activity = Robolectric.buildActivity(Activity.class).setup().get();
        activity.setContentView(new FakeGame(activity));
        host = new FakeHost();
        menu = new GameMenu(activity, host);
        menu.setAnimate(false);
        menu.attach();
        menu.onStart();
        idle(50);
    }

    @After
    public void tearDown() {
        menu.release();
        RuntimeEnvironment.setQualifiers("en-w1920dp-h1080dp-land-mdpi");
    }

    private static void idle(long ms) {
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(ms));
    }

    private static KeyEvent key(int action, int code) {
        return key(action, code, 1);
    }

    private static KeyEvent key(int action, int code, int device) {
        long t = SystemClock.uptimeMillis();
        return new KeyEvent(t, t, action, code, 0, 0, device, 0, 0, InputDevice.SOURCE_GAMEPAD);
    }

    private boolean down(int code) {
        return menu.dispatchKey(key(KeyEvent.ACTION_DOWN, code));
    }

    private boolean up(int code) {
        return menu.dispatchKey(key(KeyEvent.ACTION_UP, code));
    }

    private void press(int code) {
        down(code);
        up(code);
        idle(20);
    }

    private static MotionEvent stick(float x, float y, float hatX, float hatY) {
        MotionEvent.PointerProperties[] pp = { new MotionEvent.PointerProperties() };
        pp[0].id = 0;
        MotionEvent.PointerCoords[] pc = { new MotionEvent.PointerCoords() };
        pc[0].setAxisValue(MotionEvent.AXIS_X, x);
        pc[0].setAxisValue(MotionEvent.AXIS_Y, y);
        pc[0].setAxisValue(MotionEvent.AXIS_HAT_X, hatX);
        pc[0].setAxisValue(MotionEvent.AXIS_HAT_Y, hatY);
        pc[0].setAxisValue(MotionEvent.AXIS_RTRIGGER, 0.9f);
        long t = SystemClock.uptimeMillis();
        return MotionEvent.obtain(t, t, MotionEvent.ACTION_MOVE, 1, pp, pc, 0, 0, 1, 1, PAD, 0,
                                  InputDevice.SOURCE_JOYSTICK, 0);
    }

    private boolean touch(int action, float x, float y) {
        long t = SystemClock.uptimeMillis();
        MotionEvent e = MotionEvent.obtain(t, t, action, x, y, 0);
        e.setSource(InputDevice.SOURCE_TOUCHSCREEN);
        return menu.dispatchTouch(e);
    }

    private File shot(String name) throws Exception {
        idle(400);
        File f = TestSupport.screenshot(activity.getWindow().getDecorView(), "menu-" + name);
        assertTrue(f.length() > 0);
        return f;
    }

    // ---- Triggers --------------------------------------------------------------------------------------------------

    @Test
    public void backOpensAndCloses() {
        assertTrue(down(KeyEvent.KEYCODE_BACK));          // never the game's
        assertTrue(menu.isOpen());
        assertEquals(List.of(true), host.holds);
        assertTrue(up(KeyEvent.KEYCODE_BACK));            // the opening press's release does not close it
        assertTrue(menu.isOpen());
        assertSame(menu.row(GameMenu.Item.RESUME), menu.focusedRow());
        press(KeyEvent.KEYCODE_BACK);
        assertFalse(menu.isOpen());
        assertEquals(List.of(true, false), host.holds);
        assertTrue(host.focusBacks >= 1);
    }

    @Test
    public void aGamepadsBackIsTheGamesSelect() {
        host.gamepadBack = true;
        assertFalse(menu.dispatchKey(key(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BACK, PAD)));
        assertFalse(menu.dispatchKey(key(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BACK, PAD)));
        assertFalse(menu.isOpen());
        assertTrue(host.holds.isEmpty());
        // The system's Back (another device) still opens it.
        assertTrue(menu.dispatchKey(key(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BACK, -1)));
        assertTrue(menu.isOpen());
    }

    @Test
    public void guideOpensAndStartResumes() {
        assertTrue(down(KeyEvent.KEYCODE_BUTTON_MODE));
        assertTrue(menu.isOpen());
        assertTrue(up(KeyEvent.KEYCODE_BUTTON_MODE));
        assertTrue(menu.isOpen());
        press(KeyEvent.KEYCODE_BUTTON_START);
        assertFalse(menu.isOpen());
        assertFalse(host.held());
    }

    @Test
    public void selectStartHeldHalfASecond() {
        assertFalse(down(KeyEvent.KEYCODE_BUTTON_SELECT));   // the game's too
        assertFalse(down(KeyEvent.KEYCODE_BUTTON_START));
        idle(MenuTriggers.COMBO_MS - 100);
        assertFalse(menu.isOpen());
        idle(150);
        assertTrue(menu.isOpen());
        // Both were seen down by the game: released there at the open, their own releases swallowed.
        assertEquals(2, host.keys.size());
        for (KeyEvent k : host.keys)
            assertEquals(KeyEvent.ACTION_UP, k.getAction());
        assertTrue(up(KeyEvent.KEYCODE_BUTTON_SELECT));
        assertTrue(up(KeyEvent.KEYCODE_BUTTON_START));
        assertTrue(menu.isOpen());                          // Start's release did not resume
        assertEquals(2, host.keys.size());
    }

    @Test
    public void selectStartTooShortOrApart() {
        down(KeyEvent.KEYCODE_BUTTON_SELECT);
        down(KeyEvent.KEYCODE_BUTTON_START);
        idle(300);
        up(KeyEvent.KEYCODE_BUTTON_START);
        idle(500);
        assertFalse(menu.isOpen());
        // Start again: the 0.5 s count starts over.
        down(KeyEvent.KEYCODE_BUTTON_START);
        idle(300);
        assertFalse(menu.isOpen());
        idle(250);
        assertTrue(menu.isOpen());
    }

    @Test
    public void comboStateMachine() {
        MenuTriggers.Combo c = new MenuTriggers.Combo();
        assertTrue(c.key(KeyEvent.KEYCODE_BUTTON_SELECT, true, 1000));
        assertFalse(c.held());
        assertFalse(c.key(KeyEvent.KEYCODE_BUTTON_A, true, 1100));
        c.key(KeyEvent.KEYCODE_BUTTON_START, true, 1200);
        assertEquals(1700, c.dueAt());
        assertFalse(c.due(1699));
        assertTrue(c.due(1700));
        c.key(KeyEvent.KEYCODE_BUTTON_SELECT, false, 1300);
        assertEquals(-1, c.dueAt());
    }

    @Test
    public void edgeSwipeOpens() {
        int w = menu.root.getWidth();
        assertEquals(1920, w);
        assertFalse(touch(MotionEvent.ACTION_DOWN, 12, 500));   // the game sees it begin
        assertFalse(touch(MotionEvent.ACTION_MOVE, 40, 505));
        assertTrue(touch(MotionEvent.ACTION_MOVE, 12 + w * 0.07f, 510));
        assertTrue(menu.isOpen());
        assertEquals(1, host.cancels);                         // and then a cancel
        assertTrue(touch(MotionEvent.ACTION_MOVE, 300, 512));
        assertTrue(touch(MotionEvent.ACTION_UP, 300, 512));
        assertNull(menu.focusedRow());                         // a touch: no focus until a key comes
        // The system's back gesture finishing that swipe does not close it.
        assertTrue(down(KeyEvent.KEYCODE_BACK));
        assertTrue(up(KeyEvent.KEYCODE_BACK));
        assertTrue(menu.isOpen());
        idle(GameMenu.SWIPE_BACK_GUARD_MS);
        press(KeyEvent.KEYCODE_BACK);
        assertFalse(menu.isOpen());
    }

    @Test
    public void notAnEdgeSwipe() {
        // From the middle of the screen.
        touch(MotionEvent.ACTION_DOWN, 900, 500);
        assertFalse(touch(MotionEvent.ACTION_MOVE, 1300, 500));
        touch(MotionEvent.ACTION_UP, 1300, 500);
        // From the edge, but up the screen.
        touch(MotionEvent.ACTION_DOWN, 10, 800);
        assertFalse(touch(MotionEvent.ACTION_MOVE, 60, 500));
        assertFalse(touch(MotionEvent.ACTION_MOVE, 200, 400));
        touch(MotionEvent.ACTION_UP, 200, 400);
        assertFalse(menu.isOpen());
        assertEquals(0, host.cancels);
    }

    @Test
    public void panelMenuButtonToggles() {
        menu.toggle("panel");
        assertTrue(menu.isOpen());
        assertNull(menu.focusedRow());
        menu.toggle("panel");
        assertFalse(menu.isOpen());
        assertEquals(List.of(true, false), host.holds);
    }

    // ---- Routing and release ---------------------------------------------------------------------------------------

    @Test
    public void nothingReachesTheGameWhileOpen() {
        down(KeyEvent.KEYCODE_BACK);
        up(KeyEvent.KEYCODE_BACK);
        // (A, B, Start act on their release: tested on their own.)
        int[] codes = { KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_BUTTON_Y, KeyEvent.KEYCODE_BUTTON_L1,
                        KeyEvent.KEYCODE_BUTTON_R2, KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_BUTTON_THUMBL,
                        KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_DOWN, KeyEvent.KEYCODE_W };
        for (int c : codes) {
            assertTrue("down " + c, down(c));
            assertTrue("up " + c, up(c));
        }
        assertTrue(menu.dispatchMotion(stick(0.9f, 0, 0, 0)));
        assertTrue(menu.dispatchMotion(stick(0, 0, 0, 0)));
        // Volume stays the system's.
        assertFalse(down(KeyEvent.KEYCODE_VOLUME_UP));
        assertTrue(host.keys.isEmpty());
        assertTrue(host.motions.isEmpty());
    }

    @Test
    public void openReleasesEverythingTheGameHeld() {
        assertFalse(down(KeyEvent.KEYCODE_BUTTON_A));       // held in the game
        assertFalse(down(KeyEvent.KEYCODE_DPAD_LEFT));
        assertFalse(up(KeyEvent.KEYCODE_DPAD_LEFT));        // already let go
        assertFalse(menu.dispatchMotion(stick(0.8f, -0.6f, 1, 0)));
        int before = host.overlayReleases;
        menu.open("test");
        assertEquals(1, host.keys.size());
        KeyEvent k = host.keys.get(0);
        assertEquals(KeyEvent.KEYCODE_BUTTON_A, k.getKeyCode());
        assertEquals(KeyEvent.ACTION_UP, k.getAction());
        assertEquals(1, host.motions.size());
        MotionEvent m = host.motions.get(0);
        assertEquals(PAD, m.getDeviceId());
        assertEquals(0f, m.getAxisValue(MotionEvent.AXIS_X), 0);
        assertEquals(0f, m.getAxisValue(MotionEvent.AXIS_Y), 0);
        assertEquals(0f, m.getAxisValue(MotionEvent.AXIS_HAT_X), 0);
        assertEquals(0f, m.getAxisValue(MotionEvent.AXIS_RTRIGGER), 0);
        assertTrue((m.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK);
        assertTrue(host.overlayReleases > before);
        // A's physical release comes while open: swallowed, it acts on nothing.
        assertTrue(up(KeyEvent.KEYCODE_BUTTON_A));
        assertTrue(menu.isOpen());
        before = host.overlayReleases;
        menu.close();
        assertTrue(host.overlayReleases > before);
        assertEquals(1, host.keys.size());                  // nothing new to release on close
        // Closed again: keys go to the game.
        assertFalse(down(KeyEvent.KEYCODE_BUTTON_A));
        assertFalse(up(KeyEvent.KEYCODE_BUTTON_A));
    }

    @Test
    public void bClosesOnItsReleaseOnly() {
        menu.open("test");
        assertTrue(down(KeyEvent.KEYCODE_BUTTON_B));
        assertTrue(menu.isOpen());
        assertTrue(up(KeyEvent.KEYCODE_BUTTON_B));
        assertFalse(menu.isOpen());
        // So the release went nowhere near the game, and the next B is the game's.
        assertFalse(down(KeyEvent.KEYCODE_BUTTON_B));
    }

    @Test
    public void stickAndHatNavigate() {
        menu.open("Back");
        assertSame(menu.row(GameMenu.Item.RESUME), menu.focusedRow());
        menu.dispatchMotion(stick(0, 0, 0, 1));             // hat down: one step at once
        assertSame(menu.row(GameMenu.Item.SETTINGS), menu.focusedRow());
        menu.dispatchMotion(stick(0, 0, 0, 1));             // the same direction again: no new step
        assertSame(menu.row(GameMenu.Item.SETTINGS), menu.focusedRow());
        idle(GameMenu.NAV_FIRST_MS + 10);                   // held: repeats (Robolectric's clock runs ahead
        Widgets.MenuRow after = menu.focusedRow();          // with the focus animations, so at least one)
        assertTrue(after != menu.row(GameMenu.Item.SETTINGS) && after != menu.row(GameMenu.Item.RESUME));
        menu.dispatchMotion(stick(0, 0, 0, 0));             // let go: no more steps
        idle(1000);
        assertSame(after, menu.focusedRow());
        menu.row(GameMenu.Item.CONTROLS).requestFocusFromTouch();
        menu.dispatchMotion(stick(0, -0.9f, 0, 0));         // left stick up
        assertSame(menu.row(GameMenu.Item.SETTINGS), menu.focusedRow());
        menu.dispatchMotion(stick(0, 0, 0, 0));
    }

    // ---- Hold, launcher, quit --------------------------------------------------------------------------------------

    @Test
    public void backToLauncherKeepsTheHoldUntilStopped() {
        menu.open("Back");
        menu.row(GameMenu.Item.LAUNCHER).performClick();
        assertFalse(menu.isOpen());
        assertEquals(1, host.launchers);
        assertTrue(host.held());                            // no frame runs between the menu and the background
        assertEquals(View.GONE, menu.drawer.getVisibility());
        menu.onStop();                                      // the background hold has taken over
        assertFalse(host.held());
        assertEquals(List.of(true, false), host.holds);
        menu.onResume();                                    // Play: back to the running game
        assertEquals(List.of(true, false), host.holds);
        assertFalse(menu.isOpen());
    }

    @Test
    public void aGameOwnAppLeavesToTheHomeScreen() {
        // A game's own app has no launcher to go back to: the row says where the player goes (ShellActivity's host
        // sends the task behind the home screen, the game held).
        android.content.pm.PackageInfo pi = shadowOf(activity.getPackageManager())
                .getInternalMutablePackageInfo(activity.getPackageName());
        android.os.Bundle meta = new android.os.Bundle();
        meta.putString(TitleApk.META_TITLE_ID, "4D530004");
        pi.applicationInfo.metaData = meta;
        TitleApp.resetMeta();
        try {
            menu.release();
            build();
            menu.open("Back");
            Widgets.MenuRow r = menu.row(GameMenu.Item.LAUNCHER);
            assertEquals(activity.getString(R.string.MENU_LEAVE), r.title);
            r.performClick();
            assertEquals(1, host.launchers);
            assertFalse(menu.isOpen());
        } finally {
            pi.applicationInfo.metaData = null;
            TitleApp.resetMeta();
        }
        menu.release();
        build();
        menu.open("Back");
        assertEquals(activity.getString(R.string.MENU_LAUNCHER), menu.row(GameMenu.Item.LAUNCHER).title);
    }

    @Test
    public void quitNeedsASecondPress() {
        menu.open("Back");
        Widgets.MenuRow q = menu.row(GameMenu.Item.QUIT);
        q.performClick();
        assertEquals(0, host.quits);
        assertTrue(menu.quitArmed());
        assertEquals(activity.getString(R.string.MENU_QUIT_CONFIRM), q.title);
        q.performClick();
        assertEquals(1, host.quits);
    }

    @Test
    public void quitArmingRunsOut() {
        menu.open("Back");
        Widgets.MenuRow q = menu.row(GameMenu.Item.QUIT);
        q.performClick();
        idle(GameMenu.QUIT_ARM_MS + 100);
        assertFalse(menu.quitArmed());
        assertEquals(activity.getString(R.string.MENU_QUIT), q.title);
        q.performClick();
        assertEquals(0, host.quits);
        assertTrue(menu.quitArmed());
        // Moving away disarms it too.
        q.requestFocusFromTouch();
        assertTrue(menu.quitArmed());
        press(KeyEvent.KEYCODE_DPAD_UP);
        assertSame(menu.row(GameMenu.Item.LAUNCHER), menu.focusedRow());
        assertFalse(menu.quitArmed());
    }

    /** A game's theme (read once at game start, ShellActivity): the menu in its colours. */
    @Test
    public void themedMenu() throws Exception {
        menu.release();
        int[] px = new int[64 * 64];
        java.util.Arrays.fill(px, 0xFF2850DC);
        Theme.apply(new Theme.Colors(GamePalette.build(GamePalette.analysePixels(px), Double.NaN).colors));
        build();
        assertEquals(0xFFA7C2FF, Theme.ACCENT);
        menu.open("Back");
        idle(50);
        shot("themed");
        menu.close();
        menu.flash(activity.getString(R.string.PANEL_SHOT_SAVED, "20261110-101500.bmp"), true);
        shot("themed-flash");
    }

    @Test
    public void screenshotClosesFirst() {
        menu.open("Back");
        menu.row(GameMenu.Item.SCREENSHOT).performClick();
        assertFalse(menu.isOpen());
        idle(50);
        assertEquals(1, host.screenshots);
        assertFalse(host.held());
        menu.flash(activity.getString(R.string.PANEL_SHOT_SAVED, "20261110-101500.bmp"), true);
        assertNotNull(menu.flashText());
        idle(GameMenu.FLASH_MS + 100);
        assertNull(menu.flashText());
    }

    /**
     * XBA-166: Change disc shows for a game with two discs or more; its page lists them (« In the drive » under the
     * one playing), picking another hands it to the host and closes the menu; a disc that does not open keeps it.
     */
    @Test
    public void changeDisc() {
        menu.open("Back");
        assertNull("one disc: no Change disc", menu.row(GameMenu.Item.DISC));
        menu.close();
        menu.release();

        activity = Robolectric.buildActivity(Activity.class).setup().get();
        activity.setContentView(new FakeGame(activity));
        host = new FakeHost();
        host.discList = List.of(new Ps1Discs.Disc("Game (Disc 1).cue", "/g/Game (Disc 1).cue", 1, true),
                                new Ps1Discs.Disc("Game (Disc 2).chd", "/g/Game (Disc 2).chd", 2, false));
        final List<Ps1Discs.Disc> chosen = host.chosen;
        menu = new GameMenu(activity, host);
        menu.setAnimate(false);
        menu.attach();
        menu.onStart();
        menu.open("Back");
        assertNotNull(menu.row(GameMenu.Item.DISC));
        menu.row(GameMenu.Item.DISC).performClick();
        List<Widgets.MenuRow> rows = menu.rowsForTest();
        assertEquals(3, rows.size());   // two discs, Back
        assertEquals("Game (Disc 1).cue", rows.get(0).title);
        assertEquals(activity.getString(R.string.MENU_DISC_PLAYING), rows.get(0).subtitle);
        assertNull(rows.get(1).subtitle);
        rows.get(1).performClick();      // cannot be opened: the menu stays, a warning
        assertEquals(1, chosen.size());
        assertTrue(menu.isOpen());
        assertEquals(activity.getString(R.string.MENU_DISC_FAILED), menu.flashText());
        host.discOk = true;
        rows.get(1).performClick();
        assertEquals(2, chosen.size());
        assertEquals("/g/Game (Disc 2).chd", chosen.get(1).spec);
        assertFalse(menu.isOpen());
        assertEquals(activity.getString(R.string.MENU_DISC_DONE, 2), menu.flashText());
        // Opened again: disc 2 is the one in the drive now; Back returns to the main page on Change disc.
        menu.open("Back");
        menu.row(GameMenu.Item.DISC).performClick();
        rows = menu.rowsForTest();
        assertNull(rows.get(0).subtitle);
        assertEquals(activity.getString(R.string.MENU_DISC_PLAYING), rows.get(1).subtitle);
        menu.row(GameMenu.Item.BACK).performClick();
        assertSame(menu.row(GameMenu.Item.DISC), menu.focusedRow());
    }

    @Test
    public void shareLogKeepsTheMenu() {
        menu.open("Back");
        assertNull("Share log is on the Settings page", menu.row(GameMenu.Item.SHARE_LOG));
        menu.row(GameMenu.Item.SETTINGS).performClick();
        menu.row(GameMenu.Item.SHARE_LOG).performClick();
        assertEquals(1, host.shareLogs);
        assertTrue(menu.isOpen());
        assertTrue(host.held());
    }

    // ---- Pages and quick settings ----------------------------------------------------------------------------------

    @Test
    public void quickSettingsByPad() {
        menu.open("Back");
        press(KeyEvent.KEYCODE_DPAD_DOWN);
        assertSame(menu.row(GameMenu.Item.SETTINGS), menu.focusedRow());
        press(KeyEvent.KEYCODE_BUTTON_A);
        assertEquals(GameMenu.Page.SETTINGS, menu.page());
        assertSame(menu.row(GameMenu.Item.PERF), menu.focusedRow());
        // Performance overlay: on, live, remembered.
        press(KeyEvent.KEYCODE_DPAD_RIGHT);
        assertTrue(menu.perfOn());
        assertTrue(GameStore.perfOverlay(activity));
        assertEquals("59.9 fps · 16.7 ms", menu.perfText());
        // Volume: left steps down, A mutes and unmutes.
        press(KeyEvent.KEYCODE_DPAD_DOWN);
        press(KeyEvent.KEYCODE_DPAD_LEFT);
        press(KeyEvent.KEYCODE_DPAD_LEFT);
        assertEquals(80, host.volume);
        assertEquals(80, GameStore.gameVolume(activity));
        press(KeyEvent.KEYCODE_BUTTON_A);
        assertEquals(0, host.volume);
        press(KeyEvent.KEYCODE_BUTTON_A);
        assertEquals(80, host.volume);
        // Bottom screen: Panel -> Black, live.
        press(KeyEvent.KEYCODE_DPAD_DOWN);
        press(KeyEvent.KEYCODE_DPAD_RIGHT);
        assertEquals(GameStore.BottomScreen.BLACK, GameStore.bottomScreen(activity));
        assertEquals(1, host.bottomChanges);
        // B: back to the main page, on Settings; B again closes.
        press(KeyEvent.KEYCODE_BUTTON_B);
        assertEquals(GameMenu.Page.MAIN, menu.page());
        assertTrue(menu.isOpen());
        assertSame(menu.row(GameMenu.Item.SETTINGS), menu.focusedRow());
        press(KeyEvent.KEYCODE_BUTTON_B);
        assertFalse(menu.isOpen());
        // The overlay stays over the game.
        assertNotNull(menu.perfText());
    }

    @Test
    public void savedSettingsApplyAtStart() {
        GameStore.setGameVolume(activity, 30);
        GameStore.setPerfOverlay(activity, true);
        menu.release();
        build();
        assertEquals(30, host.volume);
        assertTrue(menu.perfOn());
        assertNotNull(menu.perfText());
    }

    @Test
    public void noBottomScreenRowOnOneScreen() {
        host.secondScreen = false;
        menu.open("Back");
        menu.row(GameMenu.Item.SETTINGS).performClick();
        assertNull(menu.row(GameMenu.Item.BOTTOM));
        assertNotNull(menu.row(GameMenu.Item.VOLUME));
    }

    @Test
    public void noLanguageRowWithoutAGame() {
        menu.open("Back");
        menu.row(GameMenu.Item.SETTINGS).performClick();
        assertNull(menu.row(GameMenu.Item.LANGUAGE));
    }

    @Test
    public void languageRowIsPerTitle() throws Exception {
        java.io.File dir = new java.io.File(activity.getFilesDir(), "lang-game");
        TestSupport.write(new java.io.File(dir, "default.xbe"), TestSupport.xbe(0x4D5300D1, "Game"));
        GameStore.remember(activity, dir.getPath(), "Game");
        menu.release();
        build();
        menu.open("Back");
        menu.row(GameMenu.Item.SETTINGS).performClick();
        assertNotNull(menu.row(GameMenu.Item.LANGUAGE));
        assertNull(GameLanguage.titleChoice(activity, "4D5300D1"));
        menu.row(GameMenu.Item.LANGUAGE).performClick();
        assertEquals("en", GameLanguage.titleChoice(activity, "4D5300D1"));
        menu.row(GameMenu.Item.LANGUAGE).performClick();
        assertEquals("fr", GameLanguage.titleChoice(activity, "4D5300D1"));
        for (int i = 0; i < GameLanguage.CODES.length - 1; i++)
            menu.row(GameMenu.Item.LANGUAGE).performClick();
        assertNull(GameLanguage.titleChoice(activity, "4D5300D1"));
    }

    @Test
    public void noVideoRowsWithoutAGame() {
        menu.open("Back");
        menu.row(GameMenu.Item.SETTINGS).performClick();
        assertNull(menu.row(GameMenu.Item.WIDESCREEN));
        assertNull(menu.row(GameMenu.Item.P480));
    }

    /** XBA-134: Widescreen and 480p per title, as in Settings -> On -> Off -> as in Settings. */
    @Test
    public void videoRowsArePerTitle() throws Exception {
        java.io.File dir = new java.io.File(activity.getFilesDir(), "video-game");
        TestSupport.write(new java.io.File(dir, "default.xbe"), TestSupport.xbe(0x4D5300D1, "Game"));
        GameStore.remember(activity, dir.getPath(), "Game");
        menu.release();
        build();
        menu.open("Back");
        menu.row(GameMenu.Item.SETTINGS).performClick();
        GameVideo.Setting w = GameVideo.Setting.WIDESCREEN, p = GameVideo.Setting.P480;
        assertNotNull(menu.row(GameMenu.Item.WIDESCREEN));
        assertNotNull(menu.row(GameMenu.Item.P480));
        assertNull(GameVideo.titleChoice(activity, "4D5300D1", w));
        menu.row(GameMenu.Item.WIDESCREEN).performClick();
        assertEquals(Boolean.TRUE, GameVideo.titleChoice(activity, "4D5300D1", w));
        menu.row(GameMenu.Item.WIDESCREEN).performClick();
        assertEquals(Boolean.FALSE, GameVideo.titleChoice(activity, "4D5300D1", w));
        assertNull(GameVideo.titleChoice(activity, "4D5300D1", p));      // the other one untouched
        menu.row(GameMenu.Item.WIDESCREEN).performClick();
        assertNull(GameVideo.titleChoice(activity, "4D5300D1", w));
        menu.row(GameMenu.Item.P480).performClick();
        assertEquals(Boolean.TRUE, GameVideo.titleChoice(activity, "4D5300D1", p));
    }

    /**
     * XBA-181: a GBA game launched after an Xbox game. The menu keys its per-game rows on the GBA game's library key,
     * not on the Xbox game still remembered as chosen: no Language, Widescreen or 480p (the Xbox's), Scale and Filter
     * instead, saved for the GBA game (scale_&lt;KEY&gt;, filter_&lt;KEY&gt;); Volume, Perf overlay and Screenshot stay.
     */
    @Test
    public void consoleGameRowsAfterAnXboxGame() throws Exception {
        java.io.File dir = new java.io.File(activity.getFilesDir(), "xbox-then-gba");
        TestSupport.write(new java.io.File(dir, "default.xbe"), TestSupport.xbe(0x4D5300D1, "Game"));
        GameStore.remember(activity, dir.getPath(), "Game");
        String key = "gba-AXVE-1A2B3C4D";
        GameStore.rememberRomLaunch(activity, "/sdcard/roms/quest.gba", "Pocket Quest", key);
        menu.release();
        build();
        menu.open("Back");
        assertNotNull(menu.row(GameMenu.Item.SCREENSHOT));
        menu.row(GameMenu.Item.SETTINGS).performClick();
        assertNull(menu.row(GameMenu.Item.LANGUAGE));
        assertNull(menu.row(GameMenu.Item.WIDESCREEN));
        assertNull(menu.row(GameMenu.Item.P480));
        assertNotNull(menu.row(GameMenu.Item.VOLUME));
        assertNotNull(menu.row(GameMenu.Item.PERF));
        Widgets.MenuRow scale = menu.row(GameMenu.Item.SCALE), filter = menu.row(GameMenu.Item.FILTER);
        assertNotNull(scale);
        assertNotNull(filter);
        assertEquals(activity.getString(R.string.MENU_VIDEO_FOLLOW), scale.value);
        scale.performClick();
        assertEquals("aspect", ConsoleVideo.gameChoice(activity, key, ConsoleVideo.Setting.SCALE));
        scale.performClick();
        assertEquals("integer", ConsoleVideo.gameChoice(activity, key, ConsoleVideo.Setting.SCALE));
        assertEquals(activity.getString(R.string.VAL_CSCALE_INTEGER), scale.value);
        filter.performClick();
        assertEquals("nearest", ConsoleVideo.gameChoice(activity, key, ConsoleVideo.Setting.FILTER));
        assertNull("a GBA game: no internal resolution", menu.row(GameMenu.Item.UPSCALE));
        assertNull("nothing under the Xbox game's ID", ConsoleVideo.gameChoice(activity, "4D5300D1",
                                                                            ConsoleVideo.Setting.SCALE));
        assertNull(GameLanguage.titleChoice(activity, "4D5300D1"));
    }

    /**
     * An N64 game: « Vulkan driver (next start) », as in Settings (naming Settings' driver), System, each installed
     * driver, saved for this game (vk_driver_&lt;KEY&gt;); the value warns when that driver is not compatible. A GBA game
     * has no such row (only the N64 renderer uses one).
     */
    @Test
    public void n64GameVulkanDriverRow() throws Exception {
        VkDrivers vk = VkDrivers.of(activity);
        VkDrivers.Driver d = vk.install(new java.io.ByteArrayInputStream(VkDriversTest.zip(VkDriversTest.good())),
                                        VkDrivers.Origin.manual());
        String key = "n64-NZLP-B2055FBD";
        GameStore.rememberRomLaunch(activity, "/sdcard/roms/zelda.z64", "Zelda", key);
        menu.release();
        build();
        menu.open("Back");
        menu.row(GameMenu.Item.SETTINGS).performClick();
        Widgets.MenuRow up = menu.row(GameMenu.Item.UPSCALE);   // XBA-184: its internal resolution, next start
        assertNotNull(up);
        assertEquals(activity.getString(R.string.MENU_UPSCALE), up.title);
        assertEquals(activity.getString(R.string.MENU_VIDEO_FOLLOW), up.value);
        up.performClick();
        up.performClick();
        assertEquals("2", ConsoleVideo.gameChoice(activity, key, ConsoleVideo.Setting.UPSCALE));
        assertEquals(activity.getString(R.string.VAL_UPSCALE_2X), up.value);
        Widgets.MenuRow r = menu.row(GameMenu.Item.VK_DRIVER);
        assertNotNull(r);
        assertEquals(activity.getString(R.string.MENU_VK_DRIVER), r.title);
        assertEquals(activity.getString(R.string.ROM_OPT_FOLLOW, activity.getString(R.string.VKD_SYSTEM_SHORT)),
                     r.value);
        r.performClick();
        assertEquals(VkDrivers.SYSTEM, vk.gameChoice(key));
        r.performClick();
        assertEquals(d.id, vk.gameChoice(key));
        assertEquals(VkDriverStore.label(d), r.value);
        java.nio.file.Files.write(new java.io.File(vk.root(), d.id + VkDrivers.STATUS_SUFFIX).toPath(),
                                  "unsupported\n".getBytes(java.nio.charset.StandardCharsets.UTF_8));
        r.performClick();
        r.cycler.step(-1);   // back on the driver: the value says it is not compatible
        assertEquals(VkDriverStore.label(d) + " · " + activity.getString(R.string.VKD_UNSUPPORTED_BADGE), r.value);
        assertNull("Settings untouched", vk.activeId());

        GameStore.rememberRomLaunch(activity, "/sdcard/roms/quest.gba", "Pocket Quest", "gba-AXVE-1A2B3C4D");
        menu.release();
        build();
        menu.open("Back");
        menu.row(GameMenu.Item.SETTINGS).performClick();
        assertNotNull(menu.row(GameMenu.Item.SCALE));
        assertNull(menu.row(GameMenu.Item.VK_DRIVER));
    }

    /** XBA-181: an Xbox game has no console Scale / Filter rows. */
    @Test
    public void noConsoleRowsForAnXboxGame() throws Exception {
        java.io.File dir = new java.io.File(activity.getFilesDir(), "xbox-only");
        TestSupport.write(new java.io.File(dir, "default.xbe"), TestSupport.xbe(0x4D5300D1, "Game"));
        GameStore.remember(activity, dir.getPath(), "Game");
        menu.release();
        build();
        menu.open("Back");
        menu.row(GameMenu.Item.SETTINGS).performClick();
        assertNotNull(menu.row(GameMenu.Item.LANGUAGE));
        assertNull(menu.row(GameMenu.Item.SCALE));
        assertNull(menu.row(GameMenu.Item.FILTER));
        assertNull(menu.row(GameMenu.Item.UPSCALE));
    }

    @Test
    public void controlsPage() {
        menu.open("Back");
        menu.row(GameMenu.Item.CONTROLS).performClick();
        assertEquals(GameMenu.Page.CONTROLS, menu.page());
        assertSame(menu.row(GameMenu.Item.BACK), menu.focusedRow());
        press(KeyEvent.KEYCODE_BUTTON_A);
        assertEquals(GameMenu.Page.MAIN, menu.page());
        assertSame(menu.row(GameMenu.Item.CONTROLS), menu.focusedRow());
    }

    /** XBA-158 / 168 / 174: Settings' On-screen pad row (only with one) toggles it for the session. */
    @Test
    public void touchToggle() {
        menu.open("Back");
        menu.row(GameMenu.Item.SETTINGS).performClick();
        assertNull("no on-screen pad: no row", menu.row(GameMenu.Item.TOUCH));
        press(KeyEvent.KEYCODE_BUTTON_B);
        host.touch = true;
        menu.row(GameMenu.Item.SETTINGS).performClick();
        Widgets.MenuRow r = menu.row(GameMenu.Item.TOUCH);
        assertNotNull(r);
        assertEquals(activity.getString(R.string.MENU_TOUCH_SHOWN), r.value);
        r.performClick();
        assertEquals(Boolean.FALSE, host.touch);
        assertEquals(activity.getString(R.string.MENU_TOUCH_HIDDEN), r.value);
        r.cycler.step(1);
        assertEquals(Boolean.TRUE, host.touch);
        assertEquals(activity.getString(R.string.MENU_TOUCH_SHOWN), r.value);
    }

    /** A GBA game's Controls page: its buttons and the player's controls for each, not the Xbox list. */
    @Test
    public void consoleControlsPage() throws Exception {
        host.console = ConsolePadMap.parse(Console.GBA, "a=east");
        menu.open("Back");
        menu.row(GameMenu.Item.CONTROLS).performClick();
        assertEquals(GameMenu.Page.CONTROLS, menu.page());
        java.util.List<String> texts = new java.util.ArrayList<>();
        collect(menu.drawer, texts);
        assertTrue(texts.toString(), texts.contains(activity.getString(R.string.MENU_CTL_REMAPPED)));
        assertTrue(texts.toString(), texts.contains(activity.getString(R.string.CC_SELECT)));
        assertFalse("not the Xbox list", texts.contains(activity.getString(R.string.CTL_TRIGGERS)));
        assertSame(menu.row(GameMenu.Item.BACK), menu.focusedRow());
        idle(100);
        TestSupport.screenshot(menu.root, "menu-controls-gba");
    }

    /**
     * XBA-182 / XBA-183 / XBA-190: a GBA game's and an N64 game's Controls page list the hotkeys (fast-forward, quick
     * save / load, the menu chord: the usual chords); the Xbox list has only the menu's.
     */
    @Test
    public void consoleControlsListFastForward() throws Exception {
        String ff = Hotkeys.chordLabel(activity, Hotkeys.def(Hotkeys.Action.FF_HOLD));
        String qsave = Hotkeys.chordLabel(activity, Hotkeys.def(Hotkeys.Action.QSAVE));
        String menuChord = Hotkeys.chordLabel(activity, Hotkeys.def(Hotkeys.Action.MENU));
        host.console = ConsolePadMap.parse(Console.GBA, "");
        menu.open("Back");
        menu.row(GameMenu.Item.CONTROLS).performClick();
        java.util.List<String> texts = new java.util.ArrayList<>();
        collect(menu.drawer, texts);
        assertTrue(texts.toString(), texts.contains(ff));
        assertTrue(texts.toString(), texts.contains(activity.getString(R.string.HK_FF_HOLD)));
        assertTrue(texts.toString(), texts.contains(menuChord));
        press(KeyEvent.KEYCODE_BUTTON_B);
        host.console = ConsolePadMap.parse(Console.N64, "");
        menu.row(GameMenu.Item.CONTROLS).performClick();
        texts.clear();
        collect(menu.drawer, texts);
        assertTrue(texts.toString(), texts.contains(qsave));
        assertTrue(texts.toString(), texts.contains(activity.getString(R.string.HK_QSAVE)));
        assertTrue(texts.toString(), texts.contains(activity.getString(R.string.HK_QLOAD)));
        assertFalse("an unset hotkey is not listed", texts.contains(activity.getString(R.string.HK_FF_TOGGLE)));
        press(KeyEvent.KEYCODE_BUTTON_B);
        host.console = null;
        menu.row(GameMenu.Item.CONTROLS).performClick();
        texts.clear();
        collect(menu.drawer, texts);
        assertFalse(texts.toString(), texts.contains(activity.getString(R.string.HK_FF_HOLD)));
        assertTrue(texts.toString(), texts.contains(activity.getString(R.string.HK_MENU)));
    }

    // ---- Save states and fast-forward (XBA-182) --------------------------------------------------------------------

    private static int[] thumb(int w, int h, int argb) {
        int[] t = new int[2 + w * h];
        t[0] = w;
        t[1] = h;
        java.util.Arrays.fill(t, 2, t.length, argb);
        return t;
    }

    /** A console game with states: slot 2 saved now with a thumbnail, slot 4 two days ago, 1 and 3 empty. */
    private void statesGame() {
        host.states = true;
        long now = System.currentTimeMillis();
        host.times[2] = now;
        host.thumbs[2] = thumb(160, 120, 0xFF3366CC);
        host.times[4] = now - 2 * 24 * 3600_000L;
    }

    @Test
    public void stateRowsOnlyWithStates() {
        menu.open("Back");
        assertNull(menu.row(GameMenu.Item.STATE_SAVE));
        assertNull(menu.row(GameMenu.Item.STATE_LOAD));
        assertNull(menu.row(GameMenu.Item.FAST_FORWARD));
        assertSame(menu.row(GameMenu.Item.SETTINGS), menu.rowsForTest().get(1));
        menu.close();
        host.states = true;
        menu.open("Back");
        List<Widgets.MenuRow> rows = menu.rowsForTest();
        assertSame(menu.row(GameMenu.Item.RESUME), rows.get(0));
        assertSame(menu.row(GameMenu.Item.STATE_SAVE), rows.get(1));
        assertSame(menu.row(GameMenu.Item.STATE_LOAD), rows.get(2));
        assertSame(menu.row(GameMenu.Item.FAST_FORWARD), rows.get(3));
        assertSame(menu.row(GameMenu.Item.SETTINGS), rows.get(4));
        assertEquals(activity.getString(R.string.MENU_STATE_SAVE), rows.get(1).title);
        assertEquals(activity.getString(R.string.MENU_STATE_LOAD), rows.get(2).title);
        assertEquals(activity.getString(R.string.MENU_FAST_FORWARD), rows.get(3).title);
    }

    /**
     * XBA-183: an N64 game with save states has the same rows as a GBA or PS1 game: Save state, Load state and
     * Fast-forward on the main page, « Resume where I left off » in Settings beside its N64 rows.
     */
    @Test
    public void n64GameStateRows() {
        GameStore.rememberRomLaunch(activity, "/sdcard/roms/zelda.z64", "Zelda", "n64-NZLP-B2055FBD");
        menu.release();
        build();
        statesGame();
        menu.open("Back");
        List<Widgets.MenuRow> rows = menu.rowsForTest();
        assertSame(menu.row(GameMenu.Item.STATE_SAVE), rows.get(1));
        assertSame(menu.row(GameMenu.Item.STATE_LOAD), rows.get(2));
        assertSame(menu.row(GameMenu.Item.FAST_FORWARD), rows.get(3));
        menu.row(GameMenu.Item.STATE_SAVE).performClick();
        assertEquals(GameMenu.Page.SAVE_SLOTS, menu.page());
        assertEquals(5, menu.rowsForTest().size());   // four slots, Back
        press(KeyEvent.KEYCODE_BUTTON_B);
        menu.row(GameMenu.Item.SETTINGS).performClick();
        assertNotNull(menu.row(GameMenu.Item.AUTO_RESUME));
        assertNotNull(menu.row(GameMenu.Item.UPSCALE));
    }

    @Test
    public void fastForwardToggles() {
        host.states = true;
        menu.open("Back");
        Widgets.MenuRow r = menu.row(GameMenu.Item.FAST_FORWARD);
        assertEquals(activity.getString(R.string.MENU_OFF), r.value);
        r.performClick();
        assertTrue(host.ff);
        assertEquals(activity.getString(R.string.MENU_ON), r.value);
        assertTrue("the menu stays", menu.isOpen());
        assertEquals(GameMenu.Page.MAIN, menu.page());
        r.requestFocusFromTouch();
        press(KeyEvent.KEYCODE_DPAD_LEFT);
        assertFalse(host.ff);
        assertEquals(activity.getString(R.string.MENU_OFF), r.value);
        // Opened again: the value is the host's.
        host.ff = true;
        menu.close();
        menu.open("Back");
        assertEquals(activity.getString(R.string.MENU_ON), menu.row(GameMenu.Item.FAST_FORWARD).value);
    }

    @Test
    public void saveSlotsPage() throws Exception {
        statesGame();
        menu.open("Back");
        menu.row(GameMenu.Item.STATE_SAVE).performClick();
        assertEquals(GameMenu.Page.SAVE_SLOTS, menu.page());
        List<Widgets.MenuRow> rows = menu.rowsForTest();
        assertEquals(5, rows.size());   // four slots, Back
        for (int i = 0; i < 4; i++)
            assertEquals(activity.getString(R.string.MENU_STATE_SLOT, i + 1), rows.get(i).title);
        assertSame(menu.row(GameMenu.Item.BACK), rows.get(4));
        assertEquals(activity.getString(R.string.MENU_STATE_EMPTY), rows.get(0).subtitle);
        assertNull(rows.get(0).thumb);
        assertFalse("an empty slot can be saved to", rows.get(0).dim);
        String today = android.text.format.DateFormat.getTimeFormat(activity).format(new java.util.Date(host.times[2]));
        assertEquals(today, rows.get(1).subtitle);
        assertNotNull(rows.get(1).thumb);
        assertEquals(160, rows.get(1).thumb.getWidth());
        assertEquals(120, rows.get(1).thumb.getHeight());
        assertEquals(0xFF3366CC, rows.get(1).thumb.getPixel(10, 10));
        assertNull("no thumbnail from the host", rows.get(3).thumb);
        assertTrue(rows.get(3).subtitle, rows.get(3).subtitle.length() > today.length());   // a date, then the time
        assertTrue(rows.get(3).subtitle.endsWith(
                android.text.format.DateFormat.getTimeFormat(activity).format(new java.util.Date(host.times[4]))));
        assertSame("the focus on slot 1", rows.get(0), menu.focusedRow());
        idle(100);
        TestSupport.screenshot(menu.root, "menu-save-slots");
        // Slot 3: saved, the menu closes, a message.
        rows.get(2).performClick();
        assertEquals(List.of(3), host.saved);
        assertFalse(menu.isOpen());
        assertEquals(activity.getString(R.string.MENU_STATE_SAVED, 3), menu.flashText());
        // Slot info read again the next time: slot 3 now has a time.
        host.times[3] = System.currentTimeMillis();
        menu.open("Back");
        menu.row(GameMenu.Item.STATE_SAVE).performClick();
        assertFalse(activity.getString(R.string.MENU_STATE_EMPTY).equals(menu.rowsForTest().get(2).subtitle));
    }

    @Test
    public void saveErrors() {
        statesGame();
        int[][] cases = { { GameMenu.STATE_IO, R.string.MENU_STATE_ERR_WRITE },
                          { GameMenu.STATE_OTHER_GAME, R.string.MENU_STATE_ERR_OTHER_GAME },
                          { GameMenu.STATE_BAD_SLOT, R.string.MENU_STATE_SAVE_FAILED } };
        for (int[] c : cases) {
            host.saveCode = c[0];
            menu.open("Back");
            menu.row(GameMenu.Item.STATE_SAVE).performClick();
            menu.rowsForTest().get(0).performClick();
            assertFalse(menu.isOpen());
            assertEquals(activity.getString(c[1], 1), menu.flashText());
        }
        host.saveCode = GameMenu.STATE_NO_GAME;
        menu.open("Back");
        menu.row(GameMenu.Item.STATE_SAVE).performClick();
        menu.rowsForTest().get(0).performClick();
        assertEquals(activity.getString(R.string.MENU_STATE_ERR_NO_GAME), menu.flashText());
    }

    @Test
    public void loadSlotsPage() {
        statesGame();
        menu.open("Back");
        menu.row(GameMenu.Item.STATE_LOAD).performClick();
        assertEquals(GameMenu.Page.LOAD_SLOTS, menu.page());
        List<Widgets.MenuRow> rows = menu.rowsForTest();
        assertEquals(5, rows.size());
        assertTrue("empty: dim", rows.get(0).dim);
        assertFalse(rows.get(1).dim);
        assertNotNull(rows.get(1).thumb);
        // An empty slot: a message, the menu stays, nothing loaded.
        rows.get(0).performClick();
        assertTrue(menu.isOpen());
        assertEquals(GameMenu.Page.LOAD_SLOTS, menu.page());
        assertTrue(host.loaded.isEmpty());
        assertEquals(activity.getString(R.string.MENU_STATE_EMPTY_SLOT, 1), menu.flashText());
        // A filled one: loaded, closed.
        rows.get(1).performClick();
        assertEquals(List.of(2), host.loaded);
        assertFalse(menu.isOpen());
        assertEquals(activity.getString(R.string.MENU_STATE_LOADED, 2), menu.flashText());
    }

    @Test
    public void loadErrors() {
        statesGame();
        int[][] cases = { { GameMenu.STATE_OTHER_GAME, R.string.MENU_STATE_ERR_OTHER_GAME },
                          { GameMenu.STATE_OTHER_VERSION, R.string.MENU_STATE_ERR_VERSION },
                          { GameMenu.STATE_DAMAGED, R.string.MENU_STATE_ERR_DAMAGED },
                          { GameMenu.STATE_IO, R.string.MENU_STATE_ERR_READ },
                          { GameMenu.STATE_EMPTY, R.string.MENU_STATE_EMPTY_SLOT },
                          { -99, R.string.MENU_STATE_LOAD_FAILED } };
        for (int[] c : cases) {
            host.loadCode = c[0];
            menu.open("Back");
            menu.row(GameMenu.Item.STATE_LOAD).performClick();
            menu.rowsForTest().get(3).performClick();   // slot 4: filled
            assertFalse(menu.isOpen());
            assertEquals(activity.getString(c[1], 4), menu.flashText());
        }
        assertEquals(List.of(4, 4, 4, 4, 4, 4), host.loaded);
    }

    // ---- Quick save / load and resuming --------------------------------------------------------------------------

    @Test
    public void leavingSavesTheAutomaticSlot() {
        statesGame();
        menu.onStop();                                      // Back to launcher, Home, the screen off
        assertEquals(List.of(GameMenu.AUTO_SLOT), host.saved);
        menu.onStart();
        menu.open("Back");
        menu.row(GameMenu.Item.QUIT).performClick();
        assertEquals(activity.getString(R.string.MENU_QUIT_CONFIRM_KEPT), menu.row(GameMenu.Item.QUIT).subtitle);
        menu.row(GameMenu.Item.QUIT).performClick();
        assertEquals(1, host.quits);
        assertEquals(List.of(GameMenu.AUTO_SLOT, GameMenu.AUTO_SLOT), host.saved);
    }

    @Test
    public void autoResumeOffSavesNothing() {
        statesGame();
        menu.open("Back");
        menu.row(GameMenu.Item.SETTINGS).performClick();
        GameMenu.Item it = GameMenu.Item.AUTO_RESUME;
        assertNotNull(menu.row(it));
        assertEquals(activity.getString(R.string.MENU_ON), menu.row(it).value);
        menu.row(it).performClick();
        assertFalse(GameStore.autoResume(activity));
        assertEquals(activity.getString(R.string.MENU_OFF), menu.row(it).value);
        menu.close();
        menu.onStop();
        assertTrue(host.saved.isEmpty());
        GameStore.setAutoResume(activity, true);
    }

    @Test
    public void autoResumeRowOnlyWithStates() {
        menu.open("Back");
        menu.row(GameMenu.Item.SETTINGS).performClick();
        assertNull(menu.row(GameMenu.Item.AUTO_RESUME));
        menu.close();
        menu.onStop();
        assertTrue(host.saved.isEmpty());                   // an Xbox game: no automatic slot
    }

    @Test
    public void loadPageShowsWhereYouLeftOff() {
        statesGame();
        host.times[GameMenu.AUTO_SLOT] = System.currentTimeMillis();
        menu.open("Back");
        menu.row(GameMenu.Item.STATE_LOAD).performClick();
        List<Widgets.MenuRow> rows = menu.rowsForTest();
        assertEquals(6, rows.size());
        assertEquals(activity.getString(R.string.MENU_STATE_AUTO), rows.get(0).title);
        rows.get(0).performClick();
        assertEquals(List.of(GameMenu.AUTO_SLOT), host.loaded);
        assertEquals(activity.getString(R.string.MENU_STATE_RESUMED), menu.flashText());
        // The save page never offers it.
        menu.open("Back");
        menu.row(GameMenu.Item.STATE_SAVE).performClick();
        assertEquals(5, menu.rowsForTest().size());
    }

    @Test
    public void quickEventsFlashOncePerEvent() {
        statesGame();
        host.event = new int[] { 1, GameMenu.EV_RESUMED, 0, GameMenu.STATE_OK };
        idle(GameMenu.STATE_TICK_MS + 10);
        assertEquals(activity.getString(R.string.MENU_STATE_RESUMED), menu.flashText());
        idle(GameMenu.FLASH_MS + 10);
        assertNull("the same event shows once", menu.flashText());
        host.event = new int[] { 2, GameMenu.EV_SAVED, 3, GameMenu.STATE_OK };
        idle(GameMenu.STATE_TICK_MS + 10);
        assertEquals(activity.getString(R.string.MENU_STATE_SAVED, 3), menu.flashText());
        host.event = new int[] { 3, GameMenu.EV_LOADED, 1, GameMenu.STATE_EMPTY };
        idle(GameMenu.STATE_TICK_MS + 10);
        assertEquals(activity.getString(R.string.MENU_STATE_EMPTY_SLOT, 1), menu.flashText());
        host.event = new int[] { 4, GameMenu.EV_RESUMED, 0, GameMenu.STATE_OTHER_VERSION };
        idle(GameMenu.STATE_TICK_MS + 10);
        assertEquals(activity.getString(R.string.MENU_STATE_RESUME_FAILED), menu.flashText());
        // Stopped: no polling.
        menu.onStop();
        host.event = new int[] { 5, GameMenu.EV_SAVED, 2, GameMenu.STATE_OK };
        idle(GameMenu.FLASH_MS + GameMenu.STATE_TICK_MS);
        assertNull(menu.flashText());
    }

    @Test
    public void backFromSlotPagesFocusesTheOpener() {
        host.states = true;
        menu.open("Back");
        menu.row(GameMenu.Item.STATE_SAVE).performClick();
        press(KeyEvent.KEYCODE_BUTTON_B);
        assertEquals(GameMenu.Page.MAIN, menu.page());
        assertSame(menu.row(GameMenu.Item.STATE_SAVE), menu.focusedRow());
        menu.row(GameMenu.Item.STATE_LOAD).performClick();
        assertEquals(GameMenu.Page.LOAD_SLOTS, menu.page());
        menu.row(GameMenu.Item.BACK).performClick();
        assertEquals(GameMenu.Page.MAIN, menu.page());
        assertSame(menu.row(GameMenu.Item.STATE_LOAD), menu.focusedRow());
        assertTrue(menu.isOpen());
    }

    @Test
    public void thumbBitmapChecksItsInput() {
        assertNull(GameMenu.thumbBitmap(null));
        assertNull(GameMenu.thumbBitmap(new int[] { 4 }));
        assertNull(GameMenu.thumbBitmap(new int[] { 2, 2, 0, 0, 0 }));          // too short
        assertNull(GameMenu.thumbBitmap(new int[] { 0, 2 }));
        assertNull(GameMenu.thumbBitmap(thumb(161, 1, 0)));                      // too wide
        android.graphics.Bitmap b = GameMenu.thumbBitmap(new int[] { 2, 1, 0xFFFF0000, 0xFF00FF00 });
        assertEquals(2, b.getWidth());
        assertEquals(1, b.getHeight());
        assertEquals(0xFFFF0000, b.getPixel(0, 0));
        assertEquals(0xFF00FF00, b.getPixel(1, 0));
    }

    @Test
    public void slotTimeTodayOrWithDate() {
        java.util.Calendar cal = java.util.Calendar.getInstance();
        cal.set(2026, java.util.Calendar.NOVEMBER, 14, 14, 32, 0);
        long when = cal.getTimeInMillis();
        java.text.DateFormat tf = android.text.format.DateFormat.getTimeFormat(activity);
        assertEquals(tf.format(new java.util.Date(when)), GameMenu.slotTime(activity, when, when + 3600_000L));
        String other = GameMenu.slotTime(activity, when, when + 2 * 24 * 3600_000L);
        assertEquals(android.text.format.DateFormat.getMediumDateFormat(activity).format(new java.util.Date(when))
                     + " " + tf.format(new java.util.Date(when)), other);
    }

    private static void collect(View v, java.util.List<String> out) {
        if (v instanceof android.widget.TextView)
            out.add(((android.widget.TextView)v).getText().toString());
        if (v instanceof android.view.ViewGroup)
            for (int i = 0; i < ((android.view.ViewGroup)v).getChildCount(); i++)
                collect(((android.view.ViewGroup)v).getChildAt(i), out);
    }

    @Test
    public void slidesInAndOut() {
        menu.setAnimate(true);
        menu.open("Back");
        assertEquals(View.VISIBLE, menu.drawer.getVisibility());
        assertTrue(menu.drawer.getTranslationX() < 0);
        idle(GameMenu.ANIM_MS + 50);
        assertEquals(0f, menu.drawer.getTranslationX(), 0.5f);
        assertEquals(1f, menu.scrim.getAlpha(), 0.01f);
        menu.close();
        assertEquals(View.VISIBLE, menu.drawer.getVisibility());
        idle(GameMenu.ANIM_MS + 50);
        assertEquals(View.GONE, menu.drawer.getVisibility());
        assertEquals(View.GONE, menu.scrim.getVisibility());
    }

    @Test
    public void drawerIsAFlushSheet() {
        menu.open("Back");
        idle(100);
        float scale = menu.content.getScaleX();
        float visible = (menu.drawer.getRight() + menu.drawer.getTranslationX()) * scale;
        float frac = visible / menu.root.getWidth();
        assertTrue("drawer " + frac, frac > 0.27f && frac < 0.31f);   // 560 of 1920
        assertEquals(0, menu.drawer.getLeft());
    }

    @Test
    public void durationFormat() {
        assertEquals("0:00", GameMenu.hoursMinutes(5_400));
        assertEquals("0:42", GameMenu.hoursMinutes(42 * 60_000L + 30_000));
        assertEquals("1:02", GameMenu.hoursMinutes(3_723_000));
    }

    /** Calm: six text-only rows (Quit apart), B says Resume; Settings' page says Back and holds Share log. */
    @Test
    public void mainPageRows() {
        menu.open("Back");
        GameMenu.Item[] main = { GameMenu.Item.RESUME, GameMenu.Item.SETTINGS, GameMenu.Item.CONTROLS,
                                 GameMenu.Item.SCREENSHOT, GameMenu.Item.LAUNCHER, GameMenu.Item.QUIT };
        for (GameMenu.Item it : main) {
            assertNotNull(it.toString(), menu.row(it));
            assertNull(it + ": no subtitle", menu.row(it).subtitle);
        }
        assertNull(menu.row(GameMenu.Item.SHARE_LOG));
        assertEquals(activity.getString(R.string.MENU_RESUME), menu.backHintForTest());
        assertEquals(activity.getString(R.string.MENU_SETTINGS), menu.row(GameMenu.Item.SETTINGS).title);
        menu.row(GameMenu.Item.SETTINGS).performClick();
        assertNotNull(menu.row(GameMenu.Item.SHARE_LOG));
        assertEquals(activity.getString(R.string.MENU_HINT_BACK), menu.backHintForTest());
        // Quit's confirmation is the one row that says more where it is.
        press(KeyEvent.KEYCODE_BUTTON_B);
        menu.row(GameMenu.Item.QUIT).performClick();
        assertEquals(activity.getString(R.string.MENU_QUIT_CONFIRM_SUB), menu.row(GameMenu.Item.QUIT).subtitle);
    }

    // ---- Live picture, Restart to apply ----------------------------------------------------------------------------

    private ConsoleVideo.Picture lastPicture() {
        return host.pictures.isEmpty() ? null : host.pictures.get(host.pictures.size() - 1);
    }

    /**
     * A GBA game's scale, filter and shader are sent to the game at once (one picture per real change, none for a
     * choice that draws the same); none of them asks for a restart.
     */
    @Test
    public void consolePictureAppliedLive() throws Exception {
        String key = "gba-AXVE-1A2B3C4D";
        GameStore.rememberRomLaunch(activity, "/sdcard/roms/quest.gba", "Pocket Quest", key);
        menu.release();
        build();
        menu.open("Back");
        menu.row(GameMenu.Item.SETTINGS).performClick();
        assertTrue(host.pictures.isEmpty());
        Widgets.MenuRow scale = menu.row(GameMenu.Item.SCALE), filter = menu.row(GameMenu.Item.FILTER);
        scale.performClick();   // aspect, its own: what it already shows
        assertTrue(host.pictures.isEmpty());
        scale.performClick();   // integer
        assertEquals(1, host.pictures.size());
        assertEquals("integer", lastPicture().scale);
        assertEquals("nearest", lastPicture().filter);   // the GBA's own
        assertFalse(lastPicture().xbox);
        filter.performClick();  // nearest, its own: the same
        filter.performClick();  // linear
        assertEquals(2, host.pictures.size());
        assertEquals("linear", lastPicture().filter);
        Widgets.MenuRow sh = menu.row(GameMenu.Item.SHADER);
        sh.performClick();      // None, its own
        sh.performClick();      // Sharp
        assertEquals("sharp", lastPicture().shader);
        assertEquals(-1, lastPicture().strength);
        assertNotNull(menu.row(GameMenu.Item.RESTART));
        assertEquals(View.GONE, menu.row(GameMenu.Item.RESTART).getVisibility());
        assertFalse(menu.restartPending());
    }

    /**
     * An N64 game's internal resolution waits for the game to start again: « Restart to apply » shows at the top of
     * Settings, a first press arms it, a second restarts; going back to the value it started with hides it.
     */
    @Test
    public void restartToApply() throws Exception {
        String key = "n64-NZLP-B2055FBD";
        GameStore.rememberRomLaunch(activity, "/sdcard/roms/zelda.z64", "Zelda", key);
        menu.release();
        build();
        menu.open("Back");
        menu.row(GameMenu.Item.SETTINGS).performClick();
        Widgets.MenuRow restart = menu.row(GameMenu.Item.RESTART);
        assertEquals(View.GONE, restart.getVisibility());
        assertSame("hidden: not focusable", menu.row(GameMenu.Item.PERF), menu.rowsForTest().get(0));
        Widgets.MenuRow up = menu.row(GameMenu.Item.UPSCALE);
        up.performClick();   // native (1x), its own: what it started with
        assertEquals(View.GONE, restart.getVisibility());
        up.performClick();   // 2x
        assertEquals(View.VISIBLE, restart.getVisibility());
        assertSame("first of the page", restart, menu.rowsForTest().get(0));
        assertTrue(host.pictures.isEmpty());
        shot("restart-n64");
        restart.performClick();
        assertEquals(activity.getString(R.string.MENU_RESTART_CONFIRM), restart.title);
        assertEquals(0, host.restarts);
        restart.performClick();
        assertEquals(1, host.restarts);
        up.cycler.step(-1);  // native
        up.cycler.step(-1);  // as in Settings: as it started
        assertEquals(View.GONE, restart.getVisibility());
        // Back on the page later: still in step with the game.
        menu.row(GameMenu.Item.UPSCALE).performClick();
        menu.row(GameMenu.Item.UPSCALE).performClick();
        press(KeyEvent.KEYCODE_BUTTON_B);
        menu.row(GameMenu.Item.SETTINGS).performClick();
        assertEquals(View.VISIBLE, menu.row(GameMenu.Item.RESTART).getVisibility());
    }

    /** An Xbox title: its shader goes live (scale and filter none of its business), its language waits for a restart. */
    @Test
    public void xboxShaderLiveLanguageRestart() throws Exception {
        java.io.File dir = new java.io.File(activity.getFilesDir(), "live-xbox");
        TestSupport.write(new java.io.File(dir, "default.xbe"), TestSupport.xbe(0x4D5300D1, "Game"));
        GameStore.remember(activity, dir.getPath(), "Game");
        menu.release();
        build();
        menu.open("Back");
        menu.row(GameMenu.Item.SETTINGS).performClick();
        Widgets.MenuRow sh = menu.row(GameMenu.Item.SHADER);
        sh.performClick();   // None
        sh.performClick();   // Sharp
        sh.performClick();   // Scanlines
        assertEquals("scanlines", lastPicture().shader);
        assertTrue(lastPicture().xbox);
        assertNull(lastPicture().scale);
        assertEquals(View.GONE, menu.row(GameMenu.Item.RESTART).getVisibility());
        menu.row(GameMenu.Item.LANGUAGE).performClick();   // English, its own: what Android's language gave it
        assertEquals(View.GONE, menu.row(GameMenu.Item.RESTART).getVisibility());
        menu.row(GameMenu.Item.LANGUAGE).performClick();   // another
        assertEquals(View.VISIBLE, menu.row(GameMenu.Item.RESTART).getVisibility());
        // The Game settings page has it too, and its picture options are live as well.
        menu.row(GameMenu.Item.GAME_SETTINGS).performClick();
        assertEquals(View.VISIBLE, menu.row(GameMenu.Item.RESTART).getVisibility());
    }

    /**
     * The drawer is narrow: a setting whose label and widest choice do not fit side by side is stacked (the label
     * whole on its line, the value under it), so no label is cut to a few letters; screenshots in both languages.
     */
    @Test
    public void narrowRowsKeepTheirLabel() throws Exception {
        String key = "n64-NZLP-B2055FBD";
        GameStore.rememberRomLaunch(activity, "/sdcard/roms/zelda.z64", "Zelda", key);
        for (String loc : new String[] { "en", "fr" }) {
            RuntimeEnvironment.setQualifiers(loc + "-w1920dp-h1080dp-land-mdpi");
            menu.release();
            build();
            menu.open("Back");
            menu.row(GameMenu.Item.SETTINGS).performClick();
            idle(400);
            Widgets.MenuRow sh = menu.row(GameMenu.Item.SHADER);
            assertTrue("« As in Settings (None) » beside « Picture shader » does not fit: stacked", sh.stackedForTest());
            assertFalse("Volume and its percentage fit on one line", menu.row(GameMenu.Item.VOLUME).stackedForTest());
            sh.requestFocusFromTouch();
            sh.performClick();
            sh.performClick();   // Sharp, its own: value and badge under the label
            shot("settings-console-" + loc);
            for (int i = 0; i < 6; i++)
                press(KeyEvent.KEYCODE_DPAD_DOWN);
            shot("settings-console-end-" + loc);
            menu.row(GameMenu.Item.GAME_SETTINGS).performClick();
            shot("game-settings-console-" + loc);
            GameSettings.reset(activity.getFilesDir(), key);
        }
    }

    // ---- Screenshots -----------------------------------------------------------------------------------------------

    @Test
    public void screenshots() throws Exception {
        GameStore.remember(activity, "/sdcard/games/test", "Test Title: The Long Name Edition");
        String[][] sizes = { { "1080", "w1920dp-h1080dp-land-mdpi" }, { "720", "w1280dp-h720dp-land-mdpi" } };
        for (String loc : new String[] { "en", "fr" }) {
            for (String[] size : sizes) {
                RuntimeEnvironment.setQualifiers(loc + "-" + size[1]);
                menu.release();
                build();
                String sfx = loc + "-" + size[0];
                shot("closed-" + sfx);
                menu.open("Back");
                shot("main-" + sfx);
                menu.row(GameMenu.Item.QUIT).requestFocusFromTouch();
                menu.row(GameMenu.Item.QUIT).performClick();
                shot("quit-armed-" + sfx);
                menu.row(GameMenu.Item.SETTINGS).requestFocusFromTouch();
                press(KeyEvent.KEYCODE_BUTTON_A);
                press(KeyEvent.KEYCODE_DPAD_DOWN);
                press(KeyEvent.KEYCODE_DPAD_LEFT);
                press(KeyEvent.KEYCODE_DPAD_UP);
                press(KeyEvent.KEYCODE_DPAD_RIGHT);    // overlay on
                shot("settings-" + sfx);
                press(KeyEvent.KEYCODE_BUTTON_B);
                menu.row(GameMenu.Item.CONTROLS).performClick();
                shot("controls-" + sfx);
                press(KeyEvent.KEYCODE_BUTTON_B);
                press(KeyEvent.KEYCODE_BUTTON_B);
                menu.flash(activity.getString(R.string.PANEL_SHOT_SAVED, "20261110-101500.bmp"), true);
                shot("closed-overlay-" + sfx);
                GameStore.setPerfOverlay(activity, false);
                GameStore.setGameVolume(activity, 100);
            }
        }
    }

    /**
     * XBA-188: Settings › Game settings: every Settings value that applies to the running game's console, « As in
     * Settings (<value>) » then each choice, saved in the game's file for its next start, the « This game » badge when it
     * differs; Reset to Settings drops the file; B goes back to Settings.
     */
    @Test
    public void gameSettingsPage() throws Exception {
        java.io.File dir = new java.io.File(activity.getFilesDir(), "gs-game");
        TestSupport.write(new java.io.File(dir, "default.xbe"), TestSupport.xbe(0x4D5300D1, "Game"));
        GameStore.remember(activity, dir.getPath(), "Game");
        menu.release();
        build();
        menu.open("Back");
        menu.row(GameMenu.Item.SETTINGS).performClick();
        assertNotNull(menu.row(GameMenu.Item.GAME_SETTINGS));
        menu.row(GameMenu.Item.GAME_SETTINGS).performClick();
        Widgets.MenuRow cap = null, scale = null;
        for (Widgets.MenuRow r : menu.rowsForTest()) {
            if (activity.getString(R.string.OPT_GPU_FPS_CAP).equals(r.title))
                cap = r;
            if (activity.getString(R.string.OPT_CSCALE).equals(r.title))
                scale = r;
        }
        assertNotNull("an Xbox option", cap);
        assertNull("no console picture for an Xbox title", scale);
        assertEquals(activity.getString(R.string.ROM_OPT_FOLLOW, activity.getString(R.string.VAL_AUTO)), cap.value);
        assertNull(cap.badge);
        cap.performClick();   // Auto, as Settings: its own, no badge
        assertEquals("", GameSettings.load(activity, "4D5300D1").get("RECOMP_GPU_FPS_CAP"));
        assertNull(cap.badge);
        cap.performClick();   // 30
        assertEquals("30", GameSettings.load(activity, "4D5300D1").get("RECOMP_GPU_FPS_CAP"));
        assertEquals(activity.getString(R.string.VAL_FPS_30), cap.value);
        assertEquals(activity.getString(R.string.GS_BADGE), cap.badge);
        assertNotNull(menu.row(GameMenu.Item.GS_RESET));
        menu.row(GameMenu.Item.GS_RESET).performClick();
        assertFalse(GameSettings.file(activity.getFilesDir(), "4D5300D1").exists());
        menu.row(GameMenu.Item.BACK).performClick();
        assertNotNull("back on Settings", menu.row(GameMenu.Item.GAME_SETTINGS));
    }

    /**
     * XBA-206: Settings › Picture shader (next start), for an Xbox title too (XBA-203): « As in Settings (None) » then
     * each shader, this game's own RECOMP_SHADER in its GameSettings file (the Game settings page's same value).
     */
    @Test
    public void pictureShaderRow() throws Exception {
        java.io.File dir = new java.io.File(activity.getFilesDir(), "sh-game");
        TestSupport.write(new java.io.File(dir, "default.xbe"), TestSupport.xbe(0x4D5300D1, "Game"));
        GameStore.remember(activity, dir.getPath(), "Game");
        menu.release();
        build();
        menu.open("Back");
        menu.row(GameMenu.Item.SETTINGS).performClick();
        Widgets.MenuRow sh = menu.row(GameMenu.Item.SHADER);
        assertNotNull(sh);
        assertEquals(activity.getString(R.string.MENU_SHADER), sh.title);
        assertEquals(activity.getString(R.string.ROM_OPT_FOLLOW, activity.getString(R.string.VAL_CSHADER_NONE)),
                     sh.value);
        sh.performClick();   // None, its own
        sh.performClick();   // Sharp
        sh.performClick();   // Scanlines
        sh.performClick();   // CRT
        assertEquals(ConsoleVideo.SHADER_CRT, GameSettings.load(activity, "4D5300D1").get("RECOMP_SHADER"));
        assertEquals(activity.getString(R.string.VAL_CSHADER_CRT), sh.value);
        assertEquals(activity.getString(R.string.GS_BADGE), sh.badge);
        sh.cycler.step(-1);  // Scanlines
        assertEquals(ConsoleVideo.SHADER_SCANLINES, GameSettings.load(activity, "4D5300D1").get("RECOMP_SHADER"));
    }

    /**
     * XBA-197: a GBA / PS1 / N64 game's Cheats page: each cheat On / Off (written to its .cheats file at once, which the
     * runtime reads again by itself), Add a code (the host's dialog), Delete a code (A twice), B back; none for an Xbox
     * title.
     */
    @Test
    public void cheatsPage() throws Exception {
        assertNull("no game: no Cheats row", menu.row(GameMenu.Item.CHEATS));
        String key = "gba-AXVE-1A2B3C4D";
        Cheats c = Cheats.load(activity.getFilesDir(), key);
        c.add("Infinite lives", Cheats.normalize("02000010:63"), "x");
        c.add("Max money", Cheats.normalize("82000020 270F"), "x");
        c.list.get(1).enabled = false;
        assertTrue(c.save());
        GameStore.rememberRomLaunch(activity, "/sdcard/roms/quest.gba", "Pocket Quest", key);
        menu.release();
        build();
        menu.open("Back");
        assertNotNull(menu.row(GameMenu.Item.CHEATS));
        menu.row(GameMenu.Item.CHEATS).performClick();
        assertEquals(GameMenu.Page.CHEATS, menu.page());
        List<Widgets.MenuRow> rows = menu.rowsForTest();
        assertEquals("Infinite lives", rows.get(0).title);
        assertEquals(activity.getString(R.string.MENU_ON), rows.get(0).value);
        assertEquals("Max money", rows.get(1).title);
        assertEquals(activity.getString(R.string.MENU_OFF), rows.get(1).value);
        rows.get(1).performClick();
        assertTrue(Cheats.load(activity.getFilesDir(), key).list.get(1).enabled);
        assertEquals(activity.getString(R.string.MENU_ON), rows.get(1).value);
        rows.get(0).cycler.step(1);
        assertFalse(Cheats.load(activity.getFilesDir(), key).list.get(0).enabled);

        host.cheatToAdd = "Walk through walls";
        menu.row(GameMenu.Item.CHEAT_ADD).performClick();
        assertEquals(List.of(key), host.cheatAdds);
        assertEquals(3, Cheats.load(activity.getFilesDir(), key).list.size());
        assertEquals("Walk through walls", menu.rowsForTest().get(2).title);

        menu.row(GameMenu.Item.CHEAT_DELETE).performClick();
        assertEquals(GameMenu.Page.CHEATS_DELETE, menu.page());
        Widgets.MenuRow first = menu.rowsForTest().get(0);
        first.performClick();   // armed
        assertEquals(activity.getString(R.string.CHEATS_DELETE_ARM), first.value);
        assertEquals(3, Cheats.load(activity.getFilesDir(), key).list.size());
        first.performClick();   // gone
        List<Cheats.Cheat> left = Cheats.load(activity.getFilesDir(), key).list;
        assertEquals(2, left.size());
        assertEquals("Max money", left.get(0).name);
        press(KeyEvent.KEYCODE_BUTTON_B);
        assertEquals(GameMenu.Page.CHEATS, menu.page());
        press(KeyEvent.KEYCODE_BUTTON_B);
        assertEquals(GameMenu.Page.MAIN, menu.page());
        assertSame(menu.row(GameMenu.Item.CHEATS), menu.focusedRow());
    }
}
