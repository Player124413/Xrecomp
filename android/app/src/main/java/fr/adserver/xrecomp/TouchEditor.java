package fr.adserver.xrecomp;

import android.annotation.SuppressLint;
import android.content.Context;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;

/**
 * The touch layout editor (XBA-207, docs/android/app-shell.md « Touch layout editor »): a full-screen layer with the
 * console's on-screen pad in edit mode ({@link TouchPadView#startEditing}) and a small panel in the middle (drag its
 * title to move it). Drag a control to move it (on the grid when Grid is on), pinch with a second finger or use − / +
 * to resize the selected one, Hide / Show it (hidden controls stay as dashed ghosts here, and are neither drawn nor
 * hit in the game), Reset puts the defaults back, Save writes the layout ({@link TouchLayout#save}) for the console or,
 * with « This game only », for the game alone; Cancel or Back leaves without saving.
 *
 * <p>Opened from Settings › Controls › « Edit touch layout » over a plain backdrop, and from the in-game menu over the
 * held game, where the saved layout applies to the running game at once.
 *
 * <p>XBA-212: it must never pass for the game. An opaque banner across the top (« Editing touch layout », Save,
 * Cancel) and a dark tint over the held game say what it is; the game's activity cancels it (the old layout kept) when
 * it goes to the background, when the in-game menu opens or the activity resumes, and on Back / B. A layout that
 * leaves no D-pad or stick, or no face button, to press is not saved.
 */
@SuppressLint("ViewConstructor")
final class TouchEditor extends FrameLayout {
    interface Listener {
        /** The editor is done: the layout the game should use now (saved), or null when cancelled. */
        void closed(TouchLayout saved);
    }

    final TouchPad.Layout layout;
    final String gameKey;
    final TouchPadView padView;
    final TouchEdit edit;
    private final Listener listener;
    private final File filesDir;
    private final float k;
    private boolean gameOnly;
    private final LinearLayout panel;
    private final TextView status, hide, gridB, scope;
    final TextView save;
    /** The top banner (XBA-212): what this screen is, Save, Cancel. */
    final LinearLayout banner;
    final TextView bannerSave, bannerCancel;
    private boolean closed;
    /** The dark tint over the held game (XBA-212: never mistaken for the game). */
    static final int GAME_TINT = 0xA0000000;

    /**
     * @param gameKey  the running game's key (GameSettings), or null from Settings (no « This game only »)
     * @param overGame over the held game (a light scrim) rather than the launcher (a backdrop)
     */
    TouchEditor(Context c, TouchPad.Layout layout, String gameKey, TouchSettings settings, boolean overGame,
                Listener listener) {
        super(c);
        this.layout = layout;
        this.gameKey = gameKey;
        this.listener = listener;
        this.filesDir = c.getFilesDir();
        android.util.DisplayMetrics dm = c.getResources().getDisplayMetrics();
        k = Math.max(0.5f, Math.min(dm.widthPixels, dm.heightPixels) / 1080f);
        setBackgroundColor(overGame ? GAME_TINT : Theme.opaque(Theme.BG_BOTTOM));
        setClickable(true);
        setFocusable(false);

        TouchLayout.Resolved r = TouchLayout.resolve(filesDir, gameKey, layout);
        gameOnly = r.perGame && gameKey != null;
        padView = new TouchPadView(c, null);
        padView.setLayout(layout);
        padView.apply(settings == null ? new TouchSettings() : settings);
        padView.setCustom(r.layout);
        edit = padView.startEditing();
        padView.editListener = this::refresh;
        addView(padView, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));

        panel = new LinearLayout(c);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setGravity(Gravity.CENTER_HORIZONTAL);
        int pad = px(Theme.S3);
        panel.setPadding(pad, px(Theme.S2), pad, pad);
        panel.setBackground(round(Theme.alpha(Theme.opaque(Theme.PANEL), 0.94f), Theme.RADIUS, 0));
        panel.setClickable(true);   // a touch on the panel never reaches the pad under it

        TextView title = text(c.getString(R.string.TE_TITLE, consoleName(c, layout)), Theme.T_BODY, Theme.TEXT_BRIGHT);
        title.setTypeface(Theme.MEDIUM);
        dragHandle(title);
        panel.addView(title);
        status = text("", Theme.T_SMALL, Theme.TEXT_DIM);
        status.setPadding(0, px(Theme.S1), 0, px(Theme.S2));
        status.setMaxWidth(px(640));   // the hint wraps: the panel stays narrow, clear of the face buttons
        panel.addView(status);

        LinearLayout row1 = row(c);
        row1.addView(button(c.getString(R.string.TE_SMALLER), v -> act(() -> edit.grow(-1))));
        row1.addView(button(c.getString(R.string.TE_BIGGER), v -> act(() -> edit.grow(1))));
        hide = button("", v -> act(edit::toggleHidden));
        row1.addView(hide);
        panel.addView(row1);

        LinearLayout row2 = row(c);
        gridB = button("", v -> act(() -> edit.grid = !edit.grid));
        row2.addView(gridB);
        scope = button("", v -> act(() -> gameOnly = !gameOnly));
        if (gameKey != null)
            row2.addView(scope);
        panel.addView(row2);

        LinearLayout row3 = row(c);
        row3.addView(button(c.getString(R.string.TE_RESET), v -> act(edit::reset)));
        row3.addView(button(c.getString(R.string.TE_CANCEL), v -> cancel()));
        save = button(c.getString(R.string.TE_SAVE), v -> save());
        row3.addView(save);
        panel.addView(row3);

        addView(panel, new LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT, Gravity.CENTER));

        // The banner, centred at the top (clear of the shoulder buttons in the corners): opaque, never the game's.
        banner = new LinearLayout(c);
        banner.setOrientation(LinearLayout.HORIZONTAL);
        banner.setGravity(Gravity.CENTER_VERTICAL);
        banner.setPadding(px(Theme.S3), px(6), px(Theme.S2), px(6));
        banner.setBackground(round(Theme.opaque(Theme.PANEL), Theme.RADIUS, Theme.ACCENT | 0xFF000000));
        banner.setClickable(true);   // a touch on the banner never reaches the pad under it
        TextView bannerText = text(c.getString(R.string.TE_BANNER), Theme.T_BODY, Theme.TEXT_BRIGHT);
        bannerText.setTypeface(Theme.MEDIUM);
        bannerText.setPadding(0, 0, px(Theme.S2), 0);
        banner.addView(bannerText);
        bannerCancel = button(c.getString(R.string.TE_CANCEL), v -> cancel());
        banner.addView(bannerCancel);
        bannerSave = button(c.getString(R.string.TE_SAVE), v -> save());
        banner.addView(bannerSave);
        LayoutParams blp = new LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT,
                                            Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        blp.topMargin = px(Theme.S1);
        addView(banner, blp);
        refresh();
    }

    // ---- Actions ------------------------------------------------------------------------------------------------

    private void act(Runnable r) {
        r.run();
        refresh();
        padView.invalidate();
    }

    boolean gameOnly() {
        return gameOnly;
    }

    /**
     * Writes the layout (the console's, or this game's only) and hands it to the listener; a layout with no direction
     * control or no face button to press (TouchPad.playable) is not saved: the editor says so and stays open.
     */
    boolean save() {
        if (closed)
            return false;
        if (!edit.pad.playable()) {
            Log.i(LauncherActivity.TAG, "touch layout: " + layout + " NOT saved: nothing left to play with");
            status.setText(getContext().getString(R.string.TE_UNPLAYABLE));
            return false;
        }
        closed = true;
        TouchLayout t = edit.layout();
        boolean ok = TouchLayout.save(filesDir, gameKey, layout, t, gameOnly && gameKey != null);
        Log.i(LauncherActivity.TAG, "touch layout: " + layout + (gameOnly ? " for " + gameKey : "") + " "
                                    + (ok ? "saved, " + t.entries.size() + " control(s) changed" : "NOT saved"));
        if (listener != null)
            listener.closed(ok ? TouchLayout.resolve(filesDir, gameKey, layout).layout : null);
        return ok;
    }

    /** Leaves without saving (the layout before the editor stays). Once: a second call does nothing. */
    void cancel() {
        cancel("cancelled");
    }

    void cancel(String why) {
        if (closed)
            return;
        closed = true;
        Log.i(LauncherActivity.TAG, "touch layout: " + layout + " editor " + why + ", nothing saved");
        if (listener != null)
            listener.closed(null);
    }

    /** A key while the editor is open: Back / B / Escape cancel; the rest moves the focus through the panel. */
    boolean key(KeyEvent e) {
        int code = e.getKeyCode();
        if (code == KeyEvent.KEYCODE_BACK || code == KeyEvent.KEYCODE_BUTTON_B || code == KeyEvent.KEYCODE_ESCAPE) {
            if (e.getAction() == KeyEvent.ACTION_UP && !e.isCanceled())
                cancel();
            return true;
        }
        if (findFocus() == null && e.getAction() == KeyEvent.ACTION_DOWN) {
            save.requestFocus();
            return true;
        }
        return false;
    }

    /** The panel's texts after a change: the selection, its size, the toggles. */
    void refresh() {
        Context c = getContext();
        TouchPad.Control s = edit.selected;
        if (s == null) {
            status.setText(c.getString(R.string.TE_HINT));
        } else {
            String line = c.getString(R.string.TE_SELECTED, c.getString(controlName(layout, s.id)),
                                      Math.round(s.size * 100));
            if (s.hidden)
                line += " · " + c.getString(R.string.TE_HIDDEN);
            status.setText(line);
        }
        hide.setText(c.getString(s != null && s.hidden ? R.string.TE_SHOW : R.string.TE_HIDE));
        hide.setEnabled(s != null);
        hide.setAlpha(s != null ? 1f : 0.5f);
        gridB.setText(c.getString(edit.grid ? R.string.TE_GRID_ON : R.string.TE_GRID_OFF));
        scope.setText(c.getString(gameOnly ? R.string.TE_SCOPE_GAME : R.string.TE_SCOPE_CONSOLE));
    }

    // ---- Names --------------------------------------------------------------------------------------------------

    static String consoleName(Context c, TouchPad.Layout l) {
        switch (l) {
        case GBA:
            return c.getString(R.string.CC_CONSOLE_GBA);
        case PS1:
            return c.getString(R.string.CC_CONSOLE_PS1);
        case PS1_ANALOG:
            return c.getString(R.string.TE_LAYOUT_PS1_ANALOG);
        case N64:
            return c.getString(R.string.CC_CONSOLE_N64);
        default:
            return c.getString(R.string.TE_LAYOUT_XBOX);
        }
    }

    /** The control's name as the console calls it (Cross on a PlayStation, L1, C up...). */
    static int controlName(TouchPad.Layout l, int id) {
        boolean ps = l == TouchPad.Layout.PS1 || l == TouchPad.Layout.PS1_ANALOG;
        switch (id) {
        case TouchPad.LSTICK: return l == TouchPad.Layout.N64 ? R.string.CC_STICK : R.string.XB_LSTICK;
        case TouchPad.RSTICK: return R.string.XB_RSTICK;
        case TouchPad.DPAD: return R.string.CTL_GROUP_DPAD;
        case TouchPad.A: return ps ? R.string.CC_CROSS : R.string.XB_A;
        case TouchPad.B: return ps ? R.string.CC_CIRCLE : R.string.XB_B;
        case TouchPad.X: return ps ? R.string.CC_SQUARE : R.string.XB_X;
        case TouchPad.Y: return ps ? R.string.CC_TRIANGLE : R.string.XB_Y;
        case TouchPad.BLACK: return R.string.XB_BLACK;
        case TouchPad.WHITE: return R.string.XB_WHITE;
        case TouchPad.LT: return R.string.XB_LT;
        case TouchPad.RT: return R.string.XB_RT;
        case TouchPad.BACK: return R.string.XB_BACK;
        case TouchPad.START: return R.string.XB_START;
        case TouchPad.L3: return ps ? R.string.CC_L3 : R.string.XB_LTHUMB;
        case TouchPad.R3: return ps ? R.string.CC_R3 : R.string.XB_RTHUMB;
        case TouchPad.SHOULDER_L: return ps ? R.string.CC_L1 : R.string.CC_L;
        case TouchPad.SHOULDER_R: return ps ? R.string.CC_R1 : R.string.CC_R;
        case TouchPad.SHOULDER_L2: return R.string.CC_L2;
        case TouchPad.SHOULDER_R2: return R.string.CC_R2;
        case TouchPad.Z: return R.string.CC_Z;
        case TouchPad.C_UP: return R.string.CC_C_UP;
        case TouchPad.C_DOWN: return R.string.CC_C_DOWN;
        case TouchPad.C_LEFT: return R.string.CC_C_LEFT;
        case TouchPad.C_RIGHT: return R.string.CC_C_RIGHT;
        default: return R.string.CC_SELECT;
        }
    }

    // ---- Views --------------------------------------------------------------------------------------------------

    private int px(float design) {
        return Math.round(design * k);
    }

    private GradientDrawable round(int color, float radius, int strokeColor) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(color);
        g.setCornerRadius(px(radius));
        if (strokeColor != 0)
            g.setStroke(Math.max(2, px(3)), strokeColor);
        return g;
    }

    private TextView text(String s, float size, int color) {
        TextView t = new TextView(getContext());
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_PX, size * k);
        t.setTextColor(color);
        t.setTypeface(Theme.REGULAR);
        t.setGravity(Gravity.CENTER);
        return t;
    }

    private LinearLayout row(Context c) {
        LinearLayout l = new LinearLayout(c);
        l.setOrientation(LinearLayout.HORIZONTAL);
        l.setGravity(Gravity.CENTER);
        return l;
    }

    private TextView button(String label, View.OnClickListener click) {
        TextView b = text(label, Theme.T_SMALL, Theme.TEXT);
        b.setTypeface(Theme.MEDIUM);
        b.setMinWidth(px(120));
        b.setPadding(px(Theme.S3), px(12), px(Theme.S3), px(12));
        StateListDrawable bg = new StateListDrawable();
        bg.addState(new int[] { android.R.attr.state_pressed },
                    round(Theme.opaque(Theme.PANEL_HI), 999, Theme.ACCENT | 0xFF000000));
        bg.addState(new int[] { android.R.attr.state_focused },
                    round(Theme.opaque(Theme.PANEL_HI), 999, Theme.ACCENT | 0xFF000000));
        bg.addState(new int[0], round(Theme.opaque(Theme.PANEL_HI), 999, Theme.alpha(Theme.LINE, 1f)));
        b.setBackground(bg);
        b.setFocusable(true);
        b.setClickable(true);
        b.setOnClickListener(click);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                                                                     LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.setMargins(px(6), px(6), px(6), px(6));
        b.setLayoutParams(lp);
        return b;
    }

    /** Dragging the panel's title moves the panel (out of the way of a control under it). */
    @SuppressLint("ClickableViewAccessibility")
    private void dragHandle(View title) {
        final float[] start = new float[4];
        title.setOnTouchListener((v, e) -> {
            switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                start[0] = e.getRawX();
                start[1] = e.getRawY();
                start[2] = panel.getTranslationX();
                start[3] = panel.getTranslationY();
                return true;
            case MotionEvent.ACTION_MOVE: {
                float maxX = Math.max(0, (getWidth() - panel.getWidth()) / 2f);
                float maxY = Math.max(0, (getHeight() - panel.getHeight()) / 2f);
                panel.setTranslationX(clamp(start[2] + e.getRawX() - start[0], maxX));
                panel.setTranslationY(clamp(start[3] + e.getRawY() - start[1], maxY));
                return true;
            }
            default:
                return true;
            }
        });
    }

    private static float clamp(float v, float max) {
        return Math.max(-max, Math.min(max, v));
    }
}
