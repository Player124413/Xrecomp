package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.Activity;
import android.os.Looper;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;

import java.io.File;
import java.time.Duration;

/**
 * The on-screen pad as the game's activity stacks it (ShellActivity): the game's view, the pad, the in-game menu's
 * layer over both, every touch through the activity's dispatchTouchEvent (the menu's swipe first, as in
 * ShellActivity). For each console's default layout (no saved layout), a finger on each control reaches the overlay
 * sink (what goes to input_overlay.h / host_pad's overlay); then the same after the menu was opened and closed and after
 * the in-game touch layout editor was opened and saved.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w1920dp-h1080dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class TouchInGameTest {
    @Rule
    public final TestSupport.StockTheme stockTheme = new TestSupport.StockTheme();

    /** ShellActivity's touch and key routing without SDL: the editor or a reveal, then the menu, then the views. */
    public static class GameActivity extends Activity {
        TouchControls touch;
        GameMenu menu;

        @Override
        public boolean dispatchTouchEvent(MotionEvent e) {
            if (touch != null && touch.takeTouch(e, menu != null && menu.isOpen()))
                return true;
            if (menu != null && menu.dispatchTouch(e))
                return true;
            return super.dispatchTouchEvent(e);
        }

        @Override
        public boolean dispatchKeyEvent(android.view.KeyEvent e) {
            if (touch != null && touch.editor != null)
                return touch.editor.key(e) || super.dispatchKeyEvent(e);
            if (touch != null)
                touch.onKey(e);
            if (menu != null && menu.dispatchKey(e))
                return true;
            return super.dispatchKeyEvent(e);
        }
    }

    private GameActivity activity;
    private TouchControlsTest.Overlay overlay;
    private GameMenuTest.FakeHost host;

    @Before
    public void setUp() {
        activity = Robolectric.buildActivity(GameActivity.class).setup().get();
        deleteTree(TouchLayout.dir(activity.getFilesDir()));
        activity.setContentView(new TouchControlsTest.FakeGame(activity));
    }

    private void start(TouchPad.Layout l, String gameKey) {
        overlay = new TouchControlsTest.Overlay();
        host = new GameMenuTest.FakeHost();
        activity.menu = new GameMenu(activity, host);
        activity.touch = new TouchControls(activity, new OverlayMixer(overlay), () -> false);
        activity.touch.layout(l);
        activity.touch.game(gameKey);
        activity.touch.attach();
        activity.menu.attach();
        activity.touch.start();
        relayout();
        assertTrue(activity.touch.shown());
        assertTrue("a connected pad 0 before any touch", overlay.on);
    }

    private void relayout() {
        View root = activity.getWindow().getDecorView();
        for (int i = 0; i < 3; i++) {
            shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(50));
            root.measure(View.MeasureSpec.makeMeasureSpec(1920, View.MeasureSpec.EXACTLY),
                         View.MeasureSpec.makeMeasureSpec(1080, View.MeasureSpec.EXACTLY));
            root.layout(0, 0, 1920, 1080);
        }
    }

    private void touch(int action, float x, float y) {
        long t = SystemClock.uptimeMillis();
        MotionEvent e = MotionEvent.obtain(t, t, action, x, y, 0);
        e.setSource(InputDevice.SOURCE_TOUCHSCREEN);
        activity.dispatchTouchEvent(e);
        e.recycle();
    }

    /** Where the control is on the screen (the pad's view may not start at the window's corner). */
    private float[] screen(TouchPad.Control c) {
        int[] at = new int[2];
        activity.touch.view.getLocationInWindow(at);
        return new float[] { at[0] + c.cx, at[1] + c.cy };
    }

    /** A finger on each visible control's centre (a stick: off-centre, so it tilts): something reaches the overlay. */
    private void everyControlReachesTheGame(String when) {
        TouchPadView v = activity.touch.view;
        assertEquals(when + ": the pad fills the screen", 1920, v.getWidth());
        for (TouchPad.Control c : v.pad.controls) {
            float[] p = screen(c);
            float x = p[0], y = p[1];
            if (c.kind == TouchPad.Kind.STICK || c.kind == TouchPad.Kind.DPAD)
                x += c.r * 0.8f;   // to the right: a stick tilts, the D-pad presses Right
            touch(MotionEvent.ACTION_DOWN, x, y);
            int sum = overlay.buttons | overlay.analog | overlay.lt | overlay.rt | overlay.lx | overlay.ly | overlay.rx
                      | overlay.ry;
            assertNotEquals(when + ": " + v.pad.layout + " control " + c.id + " (" + c.label + ") reaches the game", 0,
                            sum);
            touch(MotionEvent.ACTION_UP, x, y);
            assertEquals(when + ": released", 0, overlay.buttons | overlay.analog | overlay.lt | overlay.rt
                                                 | overlay.lx | overlay.rx);
        }
    }

    @Test
    public void everyLayoutInTheGameStack() {
        for (TouchPad.Layout l : TouchPad.Layout.values()) {
            setUp();
            start(l, "GBA-TEST");
            everyControlReachesTheGame("defaults");
        }
    }

    @Test
    public void afterTheMenuAndTheEditor() {
        for (TouchPad.Layout l : TouchPad.Layout.values()) {
            setUp();
            start(l, "GBA-TEST");
            activity.menu.open("test");
            relayout();
            activity.menu.close();
            shadowOf(Looper.getMainLooper()).idleFor(Duration.ofSeconds(2));
            relayout();
            everyControlReachesTheGame("after the menu");

            activity.touch.edit(() -> {});
            relayout();
            assertTrue(activity.touch.editor != null);
            activity.touch.editor.save();
            relayout();
            assertEquals(null, activity.touch.editor);
            assertEquals(1f, activity.touch.view.getAlpha(), 0f);
            everyControlReachesTheGame("after the editor");
        }
    }

    private static void deleteTree(File f) {
        File[] ls = f.listFiles();
        if (ls != null)
            for (File x : ls)
                deleteTree(x);
        f.delete();
    }
}
