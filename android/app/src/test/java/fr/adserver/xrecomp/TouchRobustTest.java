package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.graphics.drawable.ColorDrawable;
import android.os.Looper;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;
import org.robolectric.shadows.ShadowToast;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.time.Duration;

/**
 * XBA-212: « the touch controls don't work » on the AYN Thor, made impossible from every side the first investigation
 * ranked: the in-game touch layout editor left open over the game (cancelled on stop, start, resume, the menu opening,
 * Back / B; an opaque banner and a dark tint), a saved layout that hides or misplaces the controls (repaired, or set
 * aside for the defaults; « Reset touch layouts »), a physical pad's hotkey chord masking the touch buttons (the
 * overlay keeps them; the native side is tests/console_pad), and Auto hiding the pad because the Thor's own controls
 * count as a controller (a tap reveals it, the first tap sends nothing; a hint once). Same stack as TouchInGameTest:
 * the game's view, the pad, the in-game menu, every event through the activity's dispatch.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w1920dp-h1080dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class TouchRobustTest {
    @Rule
    public final TestSupport.StockTheme stockTheme = new TestSupport.StockTheme();

    private TouchInGameTest.GameActivity activity;
    private TouchControlsTest.Overlay overlay;
    private GameMenuTest.FakeHost host;
    private boolean controller;
    private File files;

    @Before
    public void setUp() {
        activity = Robolectric.buildActivity(TouchInGameTest.GameActivity.class).setup().get();
        files = activity.getFilesDir();
        TouchLayout.resetAll(files);
        TouchControls.resetHints();
        controller = false;
        activity.setContentView(new TouchControlsTest.FakeGame(activity));
    }

    private void start(TouchPad.Layout l) {
        overlay = new TouchControlsTest.Overlay();
        host = new GameMenuTest.FakeHost();
        activity.menu = new GameMenu(activity, host);
        activity.touch = new TouchControls(activity, new OverlayMixer(overlay), () -> controller);
        host.onMenuOpened = () -> activity.touch.closeEditor("menu opened");
        activity.touch.layout(l);
        activity.touch.game("GBA-TEST");
        activity.touch.attach();
        activity.menu.attach();
        activity.touch.start();
        relayout();
    }

    private void relayout() {
        View root = activity.getWindow().getDecorView();
        for (int i = 0; i < 3; i++) {
            idle(50);
            root.measure(View.MeasureSpec.makeMeasureSpec(1920, View.MeasureSpec.EXACTLY),
                         View.MeasureSpec.makeMeasureSpec(1080, View.MeasureSpec.EXACTLY));
            root.layout(0, 0, 1920, 1080);
        }
    }

    private static void idle(long ms) {
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(ms));
    }

    private void touch(int action, float x, float y) {
        long t = SystemClock.uptimeMillis();
        MotionEvent e = MotionEvent.obtain(t, t, action, x, y, 0);
        e.setSource(InputDevice.SOURCE_TOUCHSCREEN);
        activity.dispatchTouchEvent(e);
        e.recycle();
    }

    private void tap(float x, float y) {
        touch(MotionEvent.ACTION_DOWN, x, y);
        touch(MotionEvent.ACTION_UP, x, y);
    }

    private float[] screen(int id) {
        TouchPad.Control c = activity.touch.view.pad.control(id);
        int[] at = new int[2];
        activity.touch.view.getLocationInWindow(at);
        return new float[] { at[0] + c.cx, at[1] + c.cy };
    }

    private void key(int action, int code) {
        long t = SystemClock.uptimeMillis();
        KeyEvent e = new KeyEvent(t, t, action, code, 0, 0, 1, 0, 0, InputDevice.SOURCE_GAMEPAD);
        activity.dispatchKeyEvent(e);
    }

    private void write(File f, String text) throws IOException {
        f.getParentFile().mkdirs();
        Files.write(f.toPath(), text.getBytes(StandardCharsets.UTF_8));
    }

    private void settings(TouchSettings.Mode m) {
        TouchSettings s = new TouchSettings();
        s.mode = m;
        assertTrue(s.save(activity));
    }

    // ---- 1. The editor never left over the game ------------------------------------------------------------------

    private TouchEditor openEditor() {
        int[] done = { 0 };
        activity.touch.edit(() -> done[0]++);
        relayout();
        TouchEditor ed = activity.touch.editor;
        assertNotNull(ed);
        // Unmistakable: the opaque banner at the top, a dark tint.
        assertEquals(View.VISIBLE, ed.banner.getVisibility());
        assertTrue("the banner is at the top", ed.banner.getTop() < 100);
        assertEquals(TouchEditor.GAME_TINT, ((ColorDrawable)ed.getBackground()).getColor());
        assertTrue("the tint is dark", (TouchEditor.GAME_TINT >>> 24) >= 0x80);
        // A change in the editor, never saved.
        TouchPad.Control a = ed.edit.pad.control(TouchPad.A);
        ed.edit.pad.moveTo(a, 900, 500);
        return ed;
    }

    private void editorGoneLayoutKept(String when, File saved, String before) throws IOException {
        assertNull(when + ": the editor is closed", activity.touch.editor);
        assertEquals(when + ": the live pad is drawn", 1f, activity.touch.view.getAlpha(), 0f);
        assertEquals(when + ": the saved layout is untouched", before,
                     new String(Files.readAllBytes(saved.toPath()), StandardCharsets.UTF_8));
        relayout();
        float[] b = screen(TouchPad.B);
        touch(MotionEvent.ACTION_DOWN, b[0], b[1]);
        assertEquals(when + ": a touch reaches the game again", 1 << TouchPad.AN_B, overlay.analog);
        touch(MotionEvent.ACTION_UP, b[0], b[1]);
    }

    @Test
    public void theEditorClosesByItself() throws IOException {
        File saved = TouchLayout.consoleFile(files, TouchPad.Layout.XBOX);
        String before = "Y x=0.8000 y=0.3000\n";
        write(saved, before);
        start(TouchPad.Layout.XBOX);

        openEditor();
        activity.touch.stop();   // the app goes to the background
        activity.touch.start();
        editorGoneLayoutKept("background", saved, before);

        openEditor();
        activity.menu.open("panel");   // the in-game menu opens (the panel's Menu button)
        assertNull("menu opened: the editor is closed", activity.touch.editor);
        activity.menu.close();
        idle(2000);
        editorGoneLayoutKept("menu opened", saved, before);

        openEditor();
        activity.touch.closeEditor("resumed");   // ShellActivity.onResume
        editorGoneLayoutKept("resumed", saved, before);

        openEditor();
        key(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_B);
        key(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BUTTON_B);
        editorGoneLayoutKept("B", saved, before);

        openEditor();
        key(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BACK);
        key(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BACK);
        editorGoneLayoutKept("Back", saved, before);

        // The banner's Cancel too, and a second cancel does nothing.
        TouchEditor ed = openEditor();
        ed.bannerCancel.performClick();
        ed.cancel();
        editorGoneLayoutKept("banner Cancel", saved, before);
    }

    @Test
    public void neverRestoredOnStart() {
        start(TouchPad.Layout.GBA);
        openEditor();
        activity.touch.start();   // a start with the editor somehow still there
        assertNull(activity.touch.editor);
        assertEquals(1f, activity.touch.view.getAlpha(), 0f);
    }

    // ---- 2. Saved layouts that hide or misplace the controls ----------------------------------------------------

    @Test
    public void invalidEntriesAreRepaired() throws IOException {
        TouchLayout t = new TouchLayout();
        TouchLayout.Entry e;
        e = new TouchLayout.Entry();
        e.size = Float.NaN;
        t.entries.put(TouchPad.A, e);                      // NaN size
        e = new TouchLayout.Entry();
        e.size = 0f;
        t.entries.put(TouchPad.B, e);                      // zero size
        e = new TouchLayout.Entry();
        e.x = 0.01f;
        e.y = 0.5f;
        t.entries.put(TouchPad.X, e);                      // in the menu's swipe strip
        e = new TouchLayout.Entry();
        e.x = 1.7f;
        e.y = 0.4f;
        t.entries.put(TouchPad.Y, e);                      // off the screen
        e = new TouchLayout.Entry();
        e.x = 0.3f;
        e.y = Float.POSITIVE_INFINITY;
        t.entries.put(TouchPad.DPAD, e);                   // not a number
        e = new TouchLayout.Entry();
        e.x = 0.25f;
        e.y = 0.4f;
        e.size = 1.2f;
        t.entries.put(TouchPad.LSTICK, e);                 // fine
        t.entries.put(TouchPad.C_UP, new TouchLayout.Entry());   // not an Xbox control
        assertEquals(6, t.repair(TouchPad.Layout.XBOX));
        assertEquals(1f, t.entries.get(TouchPad.A).size, 0f);
        assertEquals(1f, t.entries.get(TouchPad.B).size, 0f);
        assertTrue(Float.isNaN(t.entries.get(TouchPad.X).x));
        assertTrue(Float.isNaN(t.entries.get(TouchPad.Y).x));
        assertTrue(Float.isNaN(t.entries.get(TouchPad.DPAD).y));
        assertEquals(0.25f, t.entries.get(TouchPad.LSTICK).x, 0f);
        assertFalse(t.entries.containsKey(TouchPad.C_UP));
        assertEquals("a repaired layout needs no more repair", 0, t.repair(TouchPad.Layout.XBOX));

        // From a file: what parse let through (an edge, a centre in the strip) repaired at load.
        write(TouchLayout.consoleFile(files, TouchPad.Layout.XBOX), "A x=0 y=0.5\nB x=0.5 y=1.0\nX x=0.02 y=0.4\n"
                                                                    + "Y x=0.8 y=0.3\n");
        TouchLayout.Resolved r = TouchLayout.resolve(files, null, TouchPad.Layout.XBOX);
        assertEquals(3, r.repaired);
        assertEquals(0.8f, r.layout.entries.get(TouchPad.Y).x, 0.0001f);
        start(TouchPad.Layout.XBOX);
        TouchPad pad = activity.touch.view.pad;
        assertNull(pad.fellBack);
        assertTrue(pad.control(TouchPad.X).cx > 1920 * TouchPad.EDGE_FRAC + pad.control(TouchPad.X).r);
    }

    @Test
    public void aLayoutWithNothingToPlayFallsBackToTheDefaults() throws IOException {
        // Every direction control hidden on the Xbox pad.
        write(TouchLayout.consoleFile(files, TouchPad.Layout.XBOX), "DPAD hidden\nLSTICK hidden\nRSTICK hidden\n");
        start(TouchPad.Layout.XBOX);
        TouchPad pad = activity.touch.view.pad;
        assertNotNull("set aside", pad.fellBack);
        assertTrue(pad.custom().isEmpty());
        for (TouchPad.Control c : pad.controls)
            assertFalse(c.id + " shown", c.hidden);
        assertTrue(pad.playable());
        float[] d = screen(TouchPad.DPAD);
        touch(MotionEvent.ACTION_DOWN, d[0] + pad.control(TouchPad.DPAD).r * 0.8f, d[1]);
        assertEquals(TouchPad.XB_RIGHT, overlay.buttons);
        touch(MotionEvent.ACTION_UP, d[0], d[1]);

        // The GBA's face buttons stacked under the D-pad: none can be pressed.
        setUp();
        write(TouchLayout.gameFile(files, "GBA-TEST", TouchPad.Layout.GBA),
              "DPAD x=0.5 y=0.5\nA x=0.5 y=0.5\nB x=0.5 y=0.5\n");
        start(TouchPad.Layout.GBA);
        pad = activity.touch.view.pad;
        assertNotNull(pad.fellBack);
        float[] a = screen(TouchPad.A);
        touch(MotionEvent.ACTION_DOWN, a[0], a[1]);
        assertEquals(1 << TouchPad.AN_A, overlay.analog);
        touch(MotionEvent.ACTION_UP, a[0], a[1]);

        // A playable layout is kept.
        setUp();
        write(TouchLayout.consoleFile(files, TouchPad.Layout.N64), "C_UP hidden\nZ hidden\nA size=1.5\n");
        start(TouchPad.Layout.N64);
        pad = activity.touch.view.pad;
        assertNull(pad.fellBack);
        assertTrue(pad.control(TouchPad.C_UP).hidden);
    }

    @Test
    public void theEditorDoesNotSaveAnUnplayableLayout() {
        start(TouchPad.Layout.GBA);
        TouchEditor ed = openEditor();
        ed.edit.pad.control(TouchPad.A).hidden = true;
        ed.edit.pad.control(TouchPad.B).hidden = true;
        assertFalse(ed.save());
        assertNotNull("still open: the player fixes it", activity.touch.editor);
        assertFalse(TouchLayout.consoleFile(files, TouchPad.Layout.GBA).exists());
        ed.edit.pad.control(TouchPad.B).hidden = false;
        assertTrue(ed.save());
        assertNull(activity.touch.editor);
    }

    @Test
    public void resetTouchLayouts() throws IOException {
        write(TouchLayout.consoleFile(files, TouchPad.Layout.PS1), "A hidden\n");
        write(TouchLayout.consoleFile(files, TouchPad.Layout.XBOX), "B size=2\n");
        write(TouchLayout.gameFile(files, "SLUS-00001", TouchPad.Layout.PS1), "X hidden\n");
        assertEquals(3, TouchLayout.resetAll(files));
        assertFalse(TouchLayout.dir(files).exists());
        assertTrue(TouchLayout.resolve(files, "SLUS-00001", TouchPad.Layout.PS1).layout.isEmpty());
        assertEquals(0, TouchLayout.resetAll(files));
    }

    // ---- 3. A hotkey chord on the physical pad never masks the touch buttons ---------------------------------------

    @Test
    public void aPhysicalChordLeavesTheTouchButtonsAlone() {
        settings(TouchSettings.Mode.ON);
        controller = true;   // the Thor's own controls
        start(TouchPad.Layout.PS1);
        assertTrue(activity.touch.shown());
        float[] a = screen(TouchPad.A);
        touch(MotionEvent.ACTION_DOWN, a[0], a[1]);
        assertEquals(1 << TouchPad.AN_A, overlay.analog);
        // Back + R1 (fast-forward) and Select + Start (the menu's chord, short of its hold) on the physical pad.
        key(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_SELECT);
        key(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_R1);
        assertEquals("the touch Cross still held", 1 << TouchPad.AN_A, overlay.analog);
        key(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BUTTON_R1);
        key(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_START);
        assertEquals(1 << TouchPad.AN_A, overlay.analog);
        key(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BUTTON_START);
        key(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BUTTON_SELECT);
        assertTrue(activity.touch.shown());
        assertEquals(1 << TouchPad.AN_A, overlay.analog);
        // The companion panel's buttons and the pad's merge (OverlayMixer): or'ed, neither masks the other.
        touch(MotionEvent.ACTION_UP, a[0], a[1]);
        assertEquals(0, overlay.analog);
    }

    // ---- 4. Auto with the Thor's controls: a tap reveals the pad -------------------------------------------------

    @Test
    public void autoRevealsOnATap() {
        controller = true;   // the Thor's built-in controls count as a controller
        start(TouchPad.Layout.XBOX);
        assertFalse(activity.touch.shown());
        assertEquals("the hint, once", activity.getString(R.string.TOUCH_HINT_CONTROLLER),
                     ShadowToast.getTextOfLatestToast());
        int toasts = ShadowToast.shownToastCount();

        // In the menu's swipe strip: nothing revealed.
        tap(10, 540);
        assertFalse(activity.touch.shown());

        // A tap on the game: revealed, and that tap sends nothing (it lands on A's spot).
        TouchPad d = new TouchPad();
        d.layout(1920, 1080, 1f);
        TouchPad.Control a = d.control(TouchPad.A);
        touch(MotionEvent.ACTION_DOWN, a.cx, a.cy);
        assertTrue(activity.touch.shown());
        assertTrue(activity.touch.revealed());
        assertEquals("the first tap only reveals", 0, overlay.analog | overlay.buttons);
        touch(MotionEvent.ACTION_MOVE, a.cx + 2, a.cy);
        assertEquals(0, overlay.analog);
        touch(MotionEvent.ACTION_UP, a.cx, a.cy);
        relayout();

        // Now the pad plays.
        float[] p = screen(TouchPad.A);
        touch(MotionEvent.ACTION_DOWN, p[0], p[1]);
        assertEquals(1 << TouchPad.AN_A, overlay.analog);
        // Held past the timeout: still shown.
        idle(TouchControls.REVEAL_MS + 500);
        assertTrue(activity.touch.shown());
        touch(MotionEvent.ACTION_UP, p[0], p[1]);
        assertEquals(0, overlay.analog);

        // No touch for REVEAL_MS: hidden again.
        idle(TouchControls.REVEAL_MS + 500);
        assertFalse(activity.touch.shown());
        assertFalse(overlay.on);
        assertEquals("the hint is not given again", toasts, ShadowToast.shownToastCount());

        // Revealed again, then a physical button: hidden.
        tap(960, 540);
        assertTrue(activity.touch.shown());
        key(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_A);
        assertFalse(activity.touch.shown());

        // With the menu open: a tap does not reveal.
        activity.menu.open("test");
        relayout();
        tap(1500, 540);
        assertFalse(activity.touch.shown());
        activity.menu.close();
        idle(2000);

        // Off: never revealed.
        settings(TouchSettings.Mode.OFF);
        activity.touch.stop();
        activity.touch.start();
        tap(960, 540);
        assertFalse(activity.touch.shown());
    }

    @Test
    public void thePanelHidesItATapRevealsIt() {
        start(TouchPad.Layout.N64);
        assertTrue(activity.touch.shown());
        activity.touch.panelShowing(true);   // the Thor's bottom screen shows the panel
        assertFalse(activity.touch.shown());
        assertEquals(activity.getString(R.string.TOUCH_HINT_PANEL), ShadowToast.getTextOfLatestToast());
        tap(960, 540);
        assertTrue(activity.touch.shown());
        relayout();
        float[] b = screen(TouchPad.B);
        touch(MotionEvent.ACTION_DOWN, b[0], b[1]);
        assertNotEquals(0, overlay.analog & 1 << TouchPad.AN_B);
        touch(MotionEvent.ACTION_UP, b[0], b[1]);
    }

    @Test
    public void withoutAControllerNothingChanges() {
        start(TouchPad.Layout.XBOX);
        assertTrue(activity.touch.shown());
        assertFalse(activity.touch.revealed());
        float[] a = screen(TouchPad.A);
        touch(MotionEvent.ACTION_DOWN, a[0], a[1]);
        assertEquals("a shown pad: the first touch plays", 1 << TouchPad.AN_A, overlay.analog);
        touch(MotionEvent.ACTION_UP, a[0], a[1]);
        assertEquals(0, ShadowToast.shownToastCount());
    }
}
