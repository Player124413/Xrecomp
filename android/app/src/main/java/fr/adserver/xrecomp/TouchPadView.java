package fr.adserver.xrecomp;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.DashPathEffect;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.util.Log;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;

/**
 * The on-screen pad over the game (XBA-42): a transparent full-screen View above SDL's surface, under the in-game
 * menu. Drawn by the app rather than in the native present pass: the window this View is in is composited over the
 * SurfaceView whatever it holds (the in-game menu's layer is there already), so a View that only redraws when a
 * finger changes something costs the game's GL frame nothing, with either renderer; the native presenter would draw
 * it into every frame, twice (GPU and software back ends). Touch goes from here straight to the overlay
 * (input_overlay.h) on the UI thread, read by the next pad report: no SDL event queue in between.
 *
 * Every pointer of every MotionEvent goes to {@link TouchPad}; the state is sent only when it changed. Released on
 * ACTION_CANCEL, when hidden, and when detached.
 *
 * <p>The touch layout editor (XBA-207, {@link TouchEditor}) uses one in edit mode ({@link #startEditing}): the fingers
 * go to a {@link TouchEdit} instead (move, pinch), nothing is sent to the game, hidden controls show as dashed ghosts,
 * the selected one is outlined and the snap grid drawn.
 */
final class TouchPadView extends View {
    interface Listener {
        void state(TouchPad.State s);
    }

    /**
     * The touch path's debug log (off by default): {@code adb shell setprop log.tag.XRecompTouch DEBUG}, then start
     * the game. Each finger down / up logs where it landed, the view's size and state, the control hit and the state
     * sent; ShellActivity logs who took the touch, the overlay sink a failed native call. Read once per view.
     */
    static final String DEBUG_TAG = "XRecompTouch";

    static boolean debugOn() {
        try {
            return Log.isLoggable(DEBUG_TAG, Log.DEBUG);
        } catch (RuntimeException e) {   // a JVM test without the property service
            return false;
        }
    }

    private final boolean debug = debugOn();

    TouchPad pad = new TouchPad();
    private final Listener listener;
    private TouchPad.State last = new TouchPad.State();
    private float scale = 1f;
    private int opacity = 50;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), stroke = new Paint(Paint.ANTI_ALIAS_FLAG),
                        text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF rect = new RectF();
    private final Path path = new Path();
    /** The editor's gestures, or null: the live pad. */
    TouchEdit edit;
    private final Paint ghost = new Paint(Paint.ANTI_ALIAS_FLAG), grid = new Paint();

    TouchPadView(Context c, Listener listener) {
        super(c);
        this.listener = listener;
        stroke.setStyle(Paint.Style.STROKE);
        text.setTextAlign(Paint.Align.CENTER);
        text.setTypeface(Theme.MEDIUM);
        setHapticFeedbackEnabled(true);
        ghost.setStyle(Paint.Style.STROKE);
        grid.setStrokeWidth(1f);
    }

    /**
     * Whether an unplayable saved layout falls back to the defaults (TouchPad.fallBackWhenUnplayable): the live pad
     * yes, the editor no (it shows what the player made).
     */
    private boolean fallBack = true;
    /** Told once per layout when the player's layout was set aside for the defaults (the reason). */
    java.util.function.Consumer<String> fallBackListener;
    private String reported;

    /** Edit mode (the touch layout editor): touches move and resize controls, the game gets nothing. */
    TouchEdit startEditing() {
        fallBack = false;
        pad.fallBackWhenUnplayable = false;
        pad.cancel();
        send();
        edit = new TouchEdit(pad);
        invalidate();
        return edit;
    }

    /** The player's layout (TouchLayout, XBA-207) on this pad: nothing held. */
    void setCustom(TouchLayout t) {
        reported = null;
        pad.setCustom(t);
        reportFallBack();
        send();
        invalidate();
    }

    /** The console's layout (TouchPad.Layout): a new pad, nothing held. */
    void setLayout(TouchPad.Layout l) {
        if (pad.layout == l)
            return;
        pad = new TouchPad(l);
        pad.fallBackWhenUnplayable = fallBack;
        if (getWidth() > 0)
            pad.layout(getWidth(), getHeight(), scale);
        reportFallBack();
        send();
        invalidate();
    }

    void apply(TouchSettings s) {
        scale = s.size / 100f;
        opacity = s.opacity;
        if (getWidth() > 0)
            pad.layout(getWidth(), getHeight(), scale);
        reportFallBack();
        send();
        invalidate();
    }

    private void reportFallBack() {
        String why = pad.fellBack;
        if (why == null || why.equals(reported))
            return;
        reported = why;
        if (fallBackListener != null)
            fallBackListener.accept(why);
    }

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        super.onSizeChanged(w, h, ow, oh);
        if (edit != null && pad.width() > 0) {
            edit.cancel();
            pad.setCustom(TouchLayout.capture(pad));   // the edits so far, normalised, on the new size
        }
        pad.layout(w, h, scale);
        reportFallBack();
        send();
    }

    @Override
    public void setVisibility(int v) {
        if (v != VISIBLE)
            releaseAll();
        super.setVisibility(v);
    }

    @Override
    protected void onDetachedFromWindow() {
        releaseAll();
        super.onDetachedFromWindow();
    }

    void releaseAll() {
        pad.cancel();
        send();
        invalidate();
    }

    TouchPad.State state() {
        return last;
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int a = e.getActionMasked(), idx = e.getActionIndex();
        if (edit != null) {
            editTouch(e, a, idx);
            return true;
        }
        switch (a) {
        case MotionEvent.ACTION_DOWN:
        case MotionEvent.ACTION_POINTER_DOWN:
            pad.down(e.getPointerId(idx), e.getX(idx), e.getY(idx));
            break;
        case MotionEvent.ACTION_MOVE:
            for (int i = 0; i < e.getPointerCount(); i++)
                pad.move(e.getPointerId(i), e.getX(i), e.getY(i));
            break;
        case MotionEvent.ACTION_UP:
        case MotionEvent.ACTION_POINTER_UP:
            pad.up(e.getPointerId(idx));
            break;
        case MotionEvent.ACTION_CANCEL:
            pad.cancel();
            break;
        default:
            return true;
        }
        send();
        if (debug && a != MotionEvent.ACTION_MOVE)
            debugLog(e, a, idx);
        return true;   // a touch on an empty spot does nothing; the game has no use for it either
    }

    private void debugLog(MotionEvent e, int a, int idx) {
        float x = e.getX(idx), y = e.getY(idx);
        TouchPad.Control c = pad.at(x, y, true);
        Log.d(DEBUG_TAG, "pad: " + MotionEvent.actionToString(a) + " pointer " + e.getPointerId(idx) + " at "
                         + Math.round(x) + "," + Math.round(y) + " in " + getWidth() + "x" + getHeight() + " (pad "
                         + Math.round(pad.width()) + "x" + Math.round(pad.height()) + ", " + pad.layout + ", visibility "
                         + getVisibility() + ", alpha " + getAlpha() + ", shown " + isShown() + ") on "
                         + (c == null ? "nothing" : c.id + (c.label.isEmpty() ? "" : " " + c.label)
                            + (c.hidden ? " (hidden)" : "")) + ", fingers " + pad.fingers() + ", sent " + last);
    }

    private void editTouch(MotionEvent e, int a, int idx) {
        TouchPad.Control before = edit.selected;
        switch (a) {
        case MotionEvent.ACTION_DOWN:
        case MotionEvent.ACTION_POINTER_DOWN:
            edit.down(e.getPointerId(idx), e.getX(idx), e.getY(idx));
            break;
        case MotionEvent.ACTION_MOVE:
            for (int i = 0; i < e.getPointerCount(); i++)
                edit.move(e.getPointerId(i), e.getX(i), e.getY(i));
            break;
        case MotionEvent.ACTION_UP:
        case MotionEvent.ACTION_POINTER_UP:
            edit.up(e.getPointerId(idx));
            break;
        case MotionEvent.ACTION_CANCEL:
            edit.cancel();
            break;
        default:
            return;
        }
        if (edit.selected != before && edit.selected != null)
            performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
        if (editListener != null)
            editListener.run();
        invalidate();
    }

    /** Called after each edit gesture (the editor's panel follows the selection). */
    Runnable editListener;

    /** The state to the listener when it changed; a click of feedback when a button goes down. */
    private void send() {
        TouchPad.State s = pad.state();
        if (s.equals(last))
            return;
        boolean pressed = (s.buttons & ~last.buttons) != 0 || (s.analogMask & ~last.analogMask) != 0
                          || (s.lt > 0 && last.lt == 0) || (s.rt > 0 && last.rt == 0);
        last = s;
        if (pressed)
            performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
        if (listener != null)
            listener.state(s);
        invalidate();
    }

    // ---- Drawing -------------------------------------------------------------------------------------------------

    private int a(int argb, float k) {
        int alpha = Math.round(((argb >>> 24) & 0xFF) * k * opacity / 100f);
        return (Math.max(0, Math.min(255, alpha)) << 24) | (argb & 0xFFFFFF);
    }

    @Override
    protected void onDraw(Canvas c) {
        TouchPad.State s = last;
        int live = opacity;
        if (edit != null) {
            drawGrid(c);
            opacity = Math.max(opacity, 70);   // the editor shows the controls clearly, whatever the setting
        }
        for (TouchPad.Control k : pad.controls) {
            if (k.hidden && edit == null)
                continue;
            int shown = opacity;
            if (k.hidden)
                opacity = Math.round(opacity * 0.35f);
            drawControl(c, k, s);
            opacity = shown;
            if (edit != null)
                drawEditMarks(c, k);
        }
        opacity = live;
    }

    private void drawGrid(Canvas c) {
        if (!edit.grid)
            return;
        float step = edit.gridStep() * 4;   // a line every fourth cell: the snap is finer than what is drawn
        if (step < 4)
            return;
        grid.setColor(0x26FFFFFF);
        for (float x = 0; x <= getWidth(); x += step)
            c.drawLine(x, 0, x, getHeight(), grid);
        for (float y = 0; y <= getHeight(); y += step)
            c.drawLine(0, y, getWidth(), y, grid);
    }

    /** A hidden control's dashed ring, the selected one's outline in the accent colour. */
    private void drawEditMarks(Canvas c, TouchPad.Control k) {
        boolean sel = edit.selected == k;
        if (!sel && !k.hidden)
            return;
        float grow = Math.max(6f, k.r * 0.12f);
        ghost.setStrokeWidth(Math.max(3f, k.r * (sel ? 0.07f : 0.04f)));
        ghost.setColor(sel ? Theme.ACCENT | 0xFF000000 : 0xC0FFFFFF);
        ghost.setPathEffect(k.hidden ? new DashPathEffect(new float[] { grow * 1.5f, grow }, 0) : null);
        if (k.pill()) {
            rect.set(k.cx - k.hw - grow, k.cy - k.hh - grow, k.cx + k.hw + grow, k.cy + k.hh + grow);
            c.drawRoundRect(rect, k.hh + grow, k.hh + grow, ghost);
        } else {
            c.drawCircle(k.cx, k.cy, k.r + grow, ghost);
        }
    }

    private void drawControl(Canvas c, TouchPad.Control k, TouchPad.State s) {
        switch (k.kind) {
        case STICK:
            drawStick(c, k, s);
            break;
        case DPAD:
            drawDpad(c, k, s.buttons);
            break;
        default:
            drawButton(c, k, s.held(k));
            break;
        }
    }

    private void base(Canvas c, float cx, float cy, float r, boolean held) {
        fill.setColor(a(held ? 0xC0FFFFFF : 0x70202020, 1f));
        c.drawCircle(cx, cy, r, fill);
        stroke.setStrokeWidth(Math.max(2f, r * 0.05f));
        stroke.setColor(a(0xE0FFFFFF, held ? 1f : 0.8f));
        c.drawCircle(cx, cy, r, stroke);
    }

    private void drawStick(Canvas c, TouchPad.Control k, TouchPad.State s) {
        int vx = k.id == TouchPad.LSTICK ? s.lx : s.rx, vy = k.id == TouchPad.LSTICK ? s.ly : s.ry;
        fill.setColor(a(0x50202020, 1f));
        c.drawCircle(k.cx, k.cy, k.r, fill);
        stroke.setStrokeWidth(Math.max(2f, k.r * 0.03f));
        stroke.setColor(a(0xB0FFFFFF, 1f));
        c.drawCircle(k.cx, k.cy, k.r, stroke);
        float kx = k.cx + vx / 32767f * k.r * 0.6f, ky = k.cy - vy / 32767f * k.r * 0.6f;
        boolean held = vx != 0 || vy != 0;
        base(c, kx, ky, k.r * 0.42f, held);
    }

    private void drawDpad(Canvas c, TouchPad.Control k, int buttons) {
        float r = k.r, w = r * 0.36f;
        int[][] arms = { { TouchPad.XB_UP, 0, -1 }, { TouchPad.XB_DOWN, 0, 1 }, { TouchPad.XB_LEFT, -1, 0 },
                         { TouchPad.XB_RIGHT, 1, 0 } };
        for (int[] arm : arms) {
            boolean held = (buttons & arm[0]) != 0;
            float x0 = k.cx + (arm[1] == 0 ? -w : arm[1] < 0 ? -r : w * 0.9f);
            float x1 = k.cx + (arm[1] == 0 ? w : arm[1] < 0 ? -w * 0.9f : r);
            float y0 = k.cy + (arm[2] == 0 ? -w : arm[2] < 0 ? -r : w * 0.9f);
            float y1 = k.cy + (arm[2] == 0 ? w : arm[2] < 0 ? -w * 0.9f : r);
            rect.set(x0, y0, x1, y1);
            fill.setColor(a(held ? 0xC0FFFFFF : 0x70202020, 1f));
            c.drawRoundRect(rect, w * 0.4f, w * 0.4f, fill);
            stroke.setStrokeWidth(Math.max(2f, r * 0.03f));
            stroke.setColor(a(0xE0FFFFFF, held ? 1f : 0.8f));
            c.drawRoundRect(rect, w * 0.4f, w * 0.4f, stroke);
            // The arm's arrow.
            float ax = k.cx + arm[1] * r * 0.68f, ay = k.cy + arm[2] * r * 0.68f, t = w * 0.45f;
            path.reset();
            path.moveTo(ax + arm[1] * t, ay + arm[2] * t);
            path.lineTo(ax - arm[1] * t * 0.6f + arm[2] * t, ay - arm[2] * t * 0.6f + arm[1] * t);
            path.lineTo(ax - arm[1] * t * 0.6f - arm[2] * t, ay - arm[2] * t * 0.6f - arm[1] * t);
            path.close();
            fill.setColor(a(held ? 0xFF101010 : 0xE0FFFFFF, 1f));
            c.drawPath(path, fill);
        }
    }

    /**
     * The face buttons in the Duke's colours, White and Black as discs, the rest as labelled outlines; the other
     * consoles' marks in their colours (Control.face), the PlayStation's shapes and the C buttons' arrows drawn.
     */
    private void drawButton(Canvas c, TouchPad.Control k, boolean held) {
        int face = k.face;
        if (face == 0 && pad.layout == TouchPad.Layout.XBOX)
            switch (k.id) {
        case TouchPad.A: face = 0xFF3FB34F; break;
        case TouchPad.B: face = 0xFFE0413A; break;
        case TouchPad.X: face = 0xFF3B7FE0; break;
        case TouchPad.Y: face = 0xFFE8C433; break;
        default: break;
            }
        if (k.pill()) {
            rect.set(k.cx - k.hw, k.cy - k.hh, k.cx + k.hw, k.cy + k.hh);
            fill.setColor(a(held ? 0xC0FFFFFF : 0x70202020, 1f));
            c.drawRoundRect(rect, k.hh, k.hh, fill);
            stroke.setStrokeWidth(Math.max(2f, k.hh * 0.08f));
            stroke.setColor(a(0xE0FFFFFF, held ? 1f : 0.8f));
            c.drawRoundRect(rect, k.hh, k.hh, stroke);
        } else if (k.id == TouchPad.WHITE || k.id == TouchPad.BLACK) {
            fill.setColor(a(k.id == TouchPad.WHITE ? 0xF0F2F2F2 : 0xF0080808, held ? 1f : 0.85f));
            c.drawCircle(k.cx, k.cy, k.r, fill);
            stroke.setStrokeWidth(Math.max(2f, k.r * (held ? 0.14f : 0.07f)));
            stroke.setColor(a(held ? 0xFFFFFFFF : 0xB0FFFFFF, 1f));
            c.drawCircle(k.cx, k.cy, k.r, stroke);
            return;
        } else {
            base(c, k.cx, k.cy, k.r, held);
        }
        if (k.glyph != TouchPad.GLYPH_NONE) {
            drawGlyph(c, k, held ? 0xFF101010 : a(face, 0.95f));
            return;
        }
        if ((k.id == TouchPad.BACK || k.id == TouchPad.START) && k.label.isEmpty()) {
            float d = k.id == TouchPad.BACK ? -1 : 1, t = k.hh * 0.45f;
            path.reset();
            path.moveTo(k.cx + d * t, k.cy);
            path.lineTo(k.cx - d * t * 0.7f, k.cy - t);
            path.lineTo(k.cx - d * t * 0.7f, k.cy + t);
            path.close();
            fill.setColor(a(held ? 0xFF101010 : 0xE0FFFFFF, 1f));
            c.drawPath(path, fill);
            return;
        }
        float size = k.pill() ? k.hh * (k.label.length() > 3 ? 0.62f : 0.9f)
                              : k.r * (k.label.length() > 1 ? 0.62f : 0.9f);
        text.setTextSize(size);
        text.setColor(face != 0 ? a(face, held ? 1f : 0.95f) : a(held ? 0xFF101010 : 0xF0FFFFFF, 1f));
        if (held && face != 0)
            text.setColor(a(0xFF101010, 1f));
        c.drawText(k.label, k.cx, k.cy + size * 0.35f, text);
    }

    /** A drawn mark: the PlayStation's cross, circle, square and triangle, an arrow for the C buttons. */
    private void drawGlyph(Canvas c, TouchPad.Control k, int color) {
        float g = k.r * 0.42f;
        stroke.setColor(color);
        stroke.setStrokeWidth(Math.max(2f, k.r * 0.11f));
        switch (k.glyph) {
        case TouchPad.GLYPH_CROSS:
            c.drawLine(k.cx - g, k.cy - g, k.cx + g, k.cy + g, stroke);
            c.drawLine(k.cx - g, k.cy + g, k.cx + g, k.cy - g, stroke);
            return;
        case TouchPad.GLYPH_CIRCLE:
            c.drawCircle(k.cx, k.cy, g, stroke);
            return;
        case TouchPad.GLYPH_SQUARE:
            c.drawRect(k.cx - g * 0.85f, k.cy - g * 0.85f, k.cx + g * 0.85f, k.cy + g * 0.85f, stroke);
            return;
        case TouchPad.GLYPH_TRIANGLE:
            path.reset();
            path.moveTo(k.cx, k.cy - g);
            path.lineTo(k.cx + g * 1.05f, k.cy + g * 0.75f);
            path.lineTo(k.cx - g * 1.05f, k.cy + g * 0.75f);
            path.close();
            c.drawPath(path, stroke);
            return;
        default: {   // an arrow, pointing (dx, dy)
            int dx = k.glyph == TouchPad.GLYPH_LEFT ? -1 : k.glyph == TouchPad.GLYPH_RIGHT ? 1 : 0;
            int dy = k.glyph == TouchPad.GLYPH_UP ? -1 : k.glyph == TouchPad.GLYPH_DOWN ? 1 : 0;
            path.reset();
            path.moveTo(k.cx + dx * g, k.cy + dy * g);
            path.lineTo(k.cx - dx * g * 0.6f + dy * g, k.cy - dy * g * 0.6f + dx * g);
            path.lineTo(k.cx - dx * g * 0.6f - dy * g, k.cy - dy * g * 0.6f - dx * g);
            path.close();
            fill.setColor(color);
            c.drawPath(path, fill);
            return;
        }
        }
    }
}
