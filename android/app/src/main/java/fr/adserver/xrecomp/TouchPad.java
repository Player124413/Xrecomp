package fr.adserver.xrecomp;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * The on-screen pad's logic (XBA-42): where each control is and what the fingers on the screen press. Pure Java (no
 * Android), so TouchPadTest drives it with made-up pointers; TouchPadView draws it and feeds it MotionEvents.
 *
 * Layout, placed in thousandths of the screen's height and sized by that times the size setting, anchored to the
 * edges (the right side mirrors the left), the Duke's arrangement: left stick high and D-pad low on the left, A B X Y high and the
 * right stick low on the right, the triggers in the top corners, White and Black over the face buttons, Back and
 * Start at the bottom centre, the stick clicks (LS, RS) next to the sticks. Nothing within 3 % of the left edge,
 * where a swipe opens the in-game menu (MenuTriggers).
 *
 * Fingers: every pointer counts on its own. A finger that lands on a stick or the D-pad keeps it until it lifts
 * (sliding off a stick holds it at full tilt); a finger on a button can slide to the next button, as on the companion
 * panel. Sticks are analog (offset from the base's centre, a 6 % dead zone, the circle's edge is full tilt); the
 * D-pad gives eight directions; triggers are full when touched (Android gives no usable pressure).
 *
 * The other consoles (XBA-158 / XBA-168 / XBA-174) have layouts of their own ({@link Layout}): the GBA's D-pad, A, B,
 * L, R, Start and Select; the PS1's D-pad, four face buttons, L1 / R1 / L2 / R2, Start, Select (and the DualShock's
 * sticks in analog mode); the N64's stick, A, B, Z, L, R, Start and the four C buttons. Their controls still make the
 * one Xbox-shaped overlay pad, each on the Xbox control that the console's default mapping (src/input/console_pad.c)
 * reads as that button: L is Black (the left shoulder), Z the left trigger, the C buttons the right stick pushed
 * fully. The runtime maps the overlay with those defaults, never the player's remapping, so the on-screen « Cross »
 * is always the PS1's Cross.
 */
final class TouchPad {
    enum Kind { STICK, DPAD, BUTTON }

    /** Which console's controls: the Xbox's (the default), the GBA's, the PS1's (with sticks: analog), the N64's. */
    enum Layout { XBOX, GBA, PS1, PS1_ANALOG, N64 }

    /** The layout for a game of console c; ps1Analog: RECOMP_PS1_PAD=analog (the DualShock's sticks shown). */
    static Layout layoutFor(Console c, boolean ps1Analog) {
        switch (c == null ? Console.XBOX : c) {
        case GBA:
            return Layout.GBA;
        case PS1:
            return ps1Analog ? Layout.PS1_ANALOG : Layout.PS1;
        case N64:
            return Layout.N64;
        default:
            return Layout.XBOX;
        }
    }

    /** A face button's mark, drawn rather than typed (the PlayStation's shapes, the C buttons' arrows). */
    static final int GLYPH_NONE = 0, GLYPH_CROSS = 1, GLYPH_CIRCLE = 2, GLYPH_SQUARE = 3, GLYPH_TRIANGLE = 4,
                     GLYPH_UP = 5, GLYPH_DOWN = 6, GLYPH_LEFT = 7, GLYPH_RIGHT = 8;

    // Xbox pad bits (xinput_xbox.h) and analog indices.
    static final int XB_UP = 0x01, XB_DOWN = 0x02, XB_LEFT = 0x04, XB_RIGHT = 0x08, XB_START = 0x10, XB_BACK = 0x20,
                     XB_LTHUMB = 0x40, XB_RTHUMB = 0x80;
    static final int AN_A = 0, AN_B = 1, AN_X = 2, AN_Y = 3, AN_BLACK = 4, AN_WHITE = 5, AN_LT = 6, AN_RT = 7;

    static final int LSTICK = 1, RSTICK = 2, DPAD = 3, A = 4, B = 5, X = 6, Y = 7, BLACK = 8, WHITE = 9, LT = 10,
                     RT = 11, BACK = 12, START = 13, L3 = 14, R3 = 15;
    // The other consoles' own controls (on the Xbox controls their default mapping reads, see the class comment).
    static final int SHOULDER_L = 16, SHOULDER_R = 17, SHOULDER_L2 = 18, SHOULDER_R2 = 19, Z = 20, C_UP = 21,
                     C_DOWN = 22, C_LEFT = 23, C_RIGHT = 24, SELECT = 25;

    static final float DEAD = 0.06f, STICK_SLOP = 1.3f, BUTTON_SLOP = 1.2f, EDGE_FRAC = 0.03f;

    /** One control: a circle (cx, cy, r) or, for the triggers and Back / Start, a pill (cx, cy, half w, half h). */
    static final class Control {
        final int id;
        final Kind kind;
        final String label;
        final int bits, analog;   // wButtons bits, bAnalogButtons index (-1 = none)
        int face, glyph;          // the mark's colour (0: the Duke's, by id) and its drawn shape (GLYPH_*)
        int pushX, pushY;         // a button that pushes the right stick fully (-1, 0, 1; Y up): the C buttons
        float cx, cy, r, hw, hh;
        float size = 1f;          // the player's size for it (TouchLayout, XBA-207): r, hw and hh already include it
        boolean hidden;           // hidden by the player (TouchLayout): neither drawn nor hit

        Control(int id, Kind kind, String label, int bits, int analog) {
            this.id = id;
            this.kind = kind;
            this.label = label;
            this.bits = bits;
            this.analog = analog;
        }

        Control face(int argb, int glyph) {
            this.face = argb;
            this.glyph = glyph;
            return this;
        }

        Control push(int x, int y) {
            pushX = x;
            pushY = y;
            return this;
        }

        boolean pill() {
            return hw > 0;
        }

        /** Distance-based hit with a slop factor; pills by their rectangle grown by the same factor. */
        boolean hit(float x, float y, float slop) {
            if (pill())
                return Math.abs(x - cx) <= hw * slop && Math.abs(y - cy) <= hh * slop;
            float dx = x - cx, dy = y - cy;
            return dx * dx + dy * dy <= r * r * slop * slop;
        }
    }

    /** The pad state the fingers make, as the overlay takes it (CompanionNative.nativeOverlayPad). */
    static final class State {
        int buttons, analogMask, lt, rt, lx, ly, rx, ry;

        boolean held(Control c) {
            if (c.kind == Kind.STICK)
                return c.id == LSTICK ? (lx != 0 || ly != 0) : (rx != 0 || ry != 0);
            if (c.analog == AN_LT)
                return lt > 0;
            if (c.analog == AN_RT)
                return rt > 0;
            if (c.pushX != 0 || c.pushY != 0)
                return (c.pushX == 0 || Integer.signum(rx) == c.pushX)
                       && (c.pushY == 0 || Integer.signum(ry) == c.pushY);
            return (c.bits != 0 && (buttons & c.bits) == c.bits) || (c.analog >= 0 && (analogMask & 1 << c.analog) != 0);
        }

        @Override
        public boolean equals(Object o) {
            if (!(o instanceof State))
                return false;
            State s = (State)o;
            return buttons == s.buttons && analogMask == s.analogMask && lt == s.lt && rt == s.rt && lx == s.lx
                   && ly == s.ly && rx == s.rx && ry == s.ry;
        }

        @Override
        public int hashCode() {
            return buttons * 31 + analogMask;
        }

        @Override
        public String toString() {
            return String.format(java.util.Locale.ROOT, "buttons=%02x analog=%02x lt=%d rt=%d L=(%d,%d) R=(%d,%d)",
                                 buttons, analogMask, lt, rt, lx, ly, rx, ry);
        }
    }

    final List<Control> controls = new ArrayList<>();
    final Layout layout;
    /** The player's layout (XBA-207): moves, sizes and hides on top of the default placement; empty: the defaults. */
    private TouchLayout custom = new TouchLayout();
    private float scale = 1f;
    private final Map<Integer, float[]> pointers = new HashMap<>();   // id -> { x, y }
    private final Map<Integer, Control> captured = new HashMap<>();   // id -> the stick or D-pad it landed on
    private float width, height;
    /**
     * Whether a layout that leaves no primary control the fingers can reach ({@link #playable}) falls back to the
     * defaults (the live pad, XBA-212); off in the editor, which shows what the player made so it can be fixed.
     */
    boolean fallBackWhenUnplayable = true;
    /** Set when the player's layout was set aside for the defaults (not playable); null otherwise. */
    String fellBack;

    TouchPad() {
        this(Layout.XBOX);
    }

    TouchPad(Layout layout) {
        this.layout = layout;
        switch (layout) {
        case GBA:
            controls.add(new Control(DPAD, Kind.DPAD, "", 0, -1));
            controls.add(new Control(A, Kind.BUTTON, "A", 0, AN_A).face(0xFFB9A6F0, GLYPH_NONE));
            controls.add(new Control(B, Kind.BUTTON, "B", 0, AN_B).face(0xFFB9A6F0, GLYPH_NONE));
            controls.add(new Control(SHOULDER_L, Kind.BUTTON, "L", 0, AN_BLACK));
            controls.add(new Control(SHOULDER_R, Kind.BUTTON, "R", 0, AN_WHITE));
            controls.add(new Control(SELECT, Kind.BUTTON, "SELECT", XB_BACK, -1));
            controls.add(new Control(START, Kind.BUTTON, "START", XB_START, -1));
            return;
        case PS1:
        case PS1_ANALOG:
            controls.add(new Control(DPAD, Kind.DPAD, "", 0, -1));
            if (layout == Layout.PS1_ANALOG) {
                controls.add(new Control(LSTICK, Kind.STICK, "", 0, -1));
                controls.add(new Control(RSTICK, Kind.STICK, "", 0, -1));
                controls.add(new Control(L3, Kind.BUTTON, "L3", XB_LTHUMB, -1));
                controls.add(new Control(R3, Kind.BUTTON, "R3", XB_RTHUMB, -1));
            }
            controls.add(new Control(A, Kind.BUTTON, "", 0, AN_A).face(0xFF8FB4F0, GLYPH_CROSS));
            controls.add(new Control(B, Kind.BUTTON, "", 0, AN_B).face(0xFFF07A7A, GLYPH_CIRCLE));
            controls.add(new Control(X, Kind.BUTTON, "", 0, AN_X).face(0xFFF0A0D8, GLYPH_SQUARE));
            controls.add(new Control(Y, Kind.BUTTON, "", 0, AN_Y).face(0xFF5ED2A6, GLYPH_TRIANGLE));
            controls.add(new Control(SHOULDER_L, Kind.BUTTON, "L1", 0, AN_BLACK));
            controls.add(new Control(SHOULDER_R, Kind.BUTTON, "R1", 0, AN_WHITE));
            controls.add(new Control(SHOULDER_L2, Kind.BUTTON, "L2", 0, AN_LT));
            controls.add(new Control(SHOULDER_R2, Kind.BUTTON, "R2", 0, AN_RT));
            controls.add(new Control(SELECT, Kind.BUTTON, "SELECT", XB_BACK, -1));
            controls.add(new Control(START, Kind.BUTTON, "START", XB_START, -1));
            return;
        case N64:
            controls.add(new Control(LSTICK, Kind.STICK, "", 0, -1));
            controls.add(new Control(A, Kind.BUTTON, "A", 0, AN_A).face(0xFF5B8CF0, GLYPH_NONE));
            controls.add(new Control(B, Kind.BUTTON, "B", 0, AN_B).face(0xFF4FC75F, GLYPH_NONE));
            controls.add(new Control(Z, Kind.BUTTON, "Z", 0, AN_LT));
            controls.add(new Control(SHOULDER_L, Kind.BUTTON, "L", 0, AN_BLACK));
            controls.add(new Control(SHOULDER_R, Kind.BUTTON, "R", 0, AN_WHITE));
            controls.add(new Control(START, Kind.BUTTON, "START", XB_START, -1));
            controls.add(new Control(C_UP, Kind.BUTTON, "", 0, -1).face(0xFFF0CC40, GLYPH_UP).push(0, 1));
            controls.add(new Control(C_DOWN, Kind.BUTTON, "", 0, -1).face(0xFFF0CC40, GLYPH_DOWN).push(0, -1));
            controls.add(new Control(C_LEFT, Kind.BUTTON, "", 0, -1).face(0xFFF0CC40, GLYPH_LEFT).push(-1, 0));
            controls.add(new Control(C_RIGHT, Kind.BUTTON, "", 0, -1).face(0xFFF0CC40, GLYPH_RIGHT).push(1, 0));
            return;
        default:
            break;
        }
        controls.add(new Control(LSTICK, Kind.STICK, "", 0, -1));
        controls.add(new Control(RSTICK, Kind.STICK, "", 0, -1));
        controls.add(new Control(DPAD, Kind.DPAD, "", 0, -1));
        controls.add(new Control(A, Kind.BUTTON, "A", 0, AN_A));
        controls.add(new Control(B, Kind.BUTTON, "B", 0, AN_B));
        controls.add(new Control(X, Kind.BUTTON, "X", 0, AN_X));
        controls.add(new Control(Y, Kind.BUTTON, "Y", 0, AN_Y));
        controls.add(new Control(WHITE, Kind.BUTTON, "", 0, AN_WHITE));
        controls.add(new Control(BLACK, Kind.BUTTON, "", 0, AN_BLACK));
        controls.add(new Control(LT, Kind.BUTTON, "LT", 0, AN_LT));
        controls.add(new Control(RT, Kind.BUTTON, "RT", 0, AN_RT));
        controls.add(new Control(BACK, Kind.BUTTON, "", XB_BACK, -1));
        controls.add(new Control(START, Kind.BUTTON, "", XB_START, -1));
        controls.add(new Control(L3, Kind.BUTTON, "LS", XB_LTHUMB, -1));
        controls.add(new Control(R3, Kind.BUTTON, "RS", XB_RTHUMB, -1));
    }

    Control control(int id) {
        for (Control c : controls)
            if (c.id == id)
                return c;
        throw new IllegalArgumentException("no control " + id);
    }

    boolean has(int id) {
        for (Control c : controls)
            if (c.id == id)
                return true;
        return false;
    }

    /** Places the controls on a w x h screen; scale is the size setting (1 = normal). Releases every finger. */
    void layout(float w, float h, float scale) {
        width = w;
        height = h;
        this.scale = scale;
        // Where things are: o (a thousandth of the height). How big they are, and the spacing inside a group: u (o
        // times the size setting), so a larger pad grows in place rather than running into its neighbours.
        float o = h / 1000f, u = o * scale;
        // Never closer to the left edge than the in-game menu's swipe zone allows, whatever the size.
        float minLeft = w * EDGE_FRAC;
        if (layout != Layout.XBOX) {
            layoutConsole(w, h, o, u, minLeft);
            applyCustom();
            cancel();
            return;
        }
        circle(LSTICK, left(300 * o, 150 * STICK_SLOP * u, minLeft), 400 * o, 150 * u);
        circle(DPAD, left(390 * o, 125 * 1.1f * u, minLeft), h - 230 * o, 125 * u);
        circle(L3, left(130 * o, 50 * BUTTON_SLOP * u, minLeft), 640 * o, 50 * u);
        pill(LT, left(165 * o, 110 * BUTTON_SLOP * u, minLeft), 95 * o, 110 * u, 55 * u);
        float fx = w - 300 * o, fy = 400 * o, gap = 105 * u, br = 62 * u;
        circle(A, fx, fy + gap, br);
        circle(B, fx + gap, fy, br);
        circle(X, fx - gap, fy, br);
        circle(Y, fx, fy - gap, br);
        circle(WHITE, fx - 130 * u, fy - 175 * o, 42 * u);
        circle(BLACK, fx + 130 * u, fy - 175 * o, 42 * u);
        circle(RSTICK, w - 410 * o, h - 225 * o, 150 * u);
        circle(R3, w - 130 * o, 640 * o, 50 * u);
        pill(RT, w - 165 * o, 95 * o, 110 * u, 55 * u);
        pill(BACK, w / 2 - 110 * u, h - 60 * o, 70 * u, 32 * u);
        pill(START, w / 2 + 110 * u, h - 60 * o, 70 * u, 32 * u);
        applyCustom();
        cancel();
    }

    // ---- The player's layout (XBA-207) ------------------------------------------------------------------------

    /** The player's layout (null: the defaults), placed at once when the pad has a size. */
    void setCustom(TouchLayout t) {
        custom = t == null ? new TouchLayout() : t.copy();
        if (width > 0)
            layout(width, height, scale);
    }

    TouchLayout custom() {
        return custom;
    }

    float scale() {
        return scale;
    }

    /** Each entry of the player's layout over the default placement: its place (normalised), its size, hidden. */
    private void applyCustom() {
        for (Control c : controls) {
            c.size = 1f;
            c.hidden = false;
        }
        for (Map.Entry<Integer, TouchLayout.Entry> e : custom.entries.entrySet()) {
            if (!has(e.getKey()))
                continue;   // another layout's control (a PS1 analog file read for the digital pad)
            Control c = control(e.getKey());
            TouchLayout.Entry t = e.getValue();
            resize(c, t.size);
            if (!Float.isNaN(t.x))
                c.cx = t.x * width;
            if (!Float.isNaN(t.y))
                c.cy = t.y * height;
            c.hidden = t.hidden;
            clamp(c);
        }
        fellBack = null;
        if (fallBackWhenUnplayable && !custom.isEmpty() && !playable()) {
            String why = "the layout leaves no " + (directionReachable() ? "face button" : "D-pad or stick")
                         + " to press";
            custom = new TouchLayout();
            layout(width, height, scale);   // the defaults (custom is empty now: no recursion past this)
            fellBack = why;
        }
    }

    /**
     * Whether the pad can be played (XBA-212): at least one direction control (the D-pad or a stick) and one face
     * button the fingers can reach: shown, on the screen, and the control a finger on its centre lands on (not under
     * another one).
     */
    boolean playable() {
        return directionReachable() && faceReachable();
    }

    private boolean directionReachable() {
        return reachable(DPAD) || reachable(LSTICK) || reachable(RSTICK);
    }

    private boolean faceReachable() {
        return reachable(A) || reachable(B) || reachable(X) || reachable(Y);
    }

    private boolean reachable(int id) {
        if (!has(id))
            return false;
        Control c = control(id);
        if (c.hidden || !(c.r > 0) || Float.isNaN(c.cx) || Float.isNaN(c.cy))
            return false;
        if (c.cx < 0 || c.cy < 0 || c.cx > width || c.cy > height)
            return false;
        return at(c.cx, c.cy, true) == c;
    }

    /** Resizes one control to size times its default (clamped to TouchLayout's range), kept on screen. */
    void resize(Control c, float size) {
        size = Math.max(TouchLayout.MIN_SIZE, Math.min(TouchLayout.MAX_SIZE, size));
        float k = size / c.size;
        c.r *= k;
        c.hw *= k;
        c.hh *= k;
        c.size = size;
        clamp(c);
    }

    /** Moves one control's centre to (x, y), kept on screen and clear of the menu's edge swipe. */
    void moveTo(Control c, float x, float y) {
        c.cx = x;
        c.cy = y;
        clamp(c);
    }

    private void clamp(Control c) {
        float rx = c.pill() ? c.hw : c.r, ry = c.pill() ? c.hh : c.r;
        float minX = width * EDGE_FRAC + rx, maxX = Math.max(minX, width - rx);
        c.cx = Math.max(minX, Math.min(maxX, c.cx));
        c.cy = Math.max(ry, Math.min(Math.max(ry, height - ry), c.cy));
    }

    /** The control under (x, y) for the editor: hidden ones too, the nearest centre first. */
    Control pick(float x, float y) {
        Control best = null;
        float bestD = Float.MAX_VALUE;
        for (Control c : controls) {
            if (!c.hit(x, y, BUTTON_SLOP))
                continue;
            float dx = x - c.cx, dy = y - c.cy, d = (dx * dx + dy * dy) / (c.r * c.r);
            if (d < bestD) {
                bestD = d;
                best = c;
            }
        }
        return best;
    }

    /**
     * The GBA, PS1 and N64 layouts, on the Xbox's grid: shoulders in the top corners, Start (and Select) at the bottom
     * centre, the D-pad or the stick on the left, the face buttons on the right.
     */
    private void layoutConsole(float w, float h, float o, float u, float minLeft) {
        float shoulderX = left(165 * o, 110 * BUTTON_SLOP * u, minLeft);
        pill(SHOULDER_L, shoulderX, 95 * o, 110 * u, 55 * u);
        pill(SHOULDER_R, w - 165 * o, 95 * o, 110 * u, 55 * u);
        switch (layout) {
        case GBA: {
            circle(DPAD, left(330 * o, 140 * 1.1f * u, minLeft), 560 * o, 140 * u);
            float fx = w - 300 * o, fy = 560 * o;
            circle(A, fx + 75 * u, fy - 45 * u, 72 * u);   // the GBA's slant: A up and right of B
            circle(B, fx - 85 * u, fy + 50 * u, 72 * u);
            pill(SELECT, w / 2 - 110 * u, h - 60 * o, 70 * u, 32 * u);
            pill(START, w / 2 + 110 * u, h - 60 * o, 70 * u, 32 * u);
            break;
        }
        case PS1:
        case PS1_ANALOG: {
            boolean sticks = layout == Layout.PS1_ANALOG;
            // L2 / R2 inward of L1 / R1 on the top edge (no room under them for the face buttons on a 4:3 screen).
            pill(SHOULDER_L2, shoulderX + 240 * u, 95 * o, 110 * u, 55 * u);
            pill(SHOULDER_R2, w - 165 * o - 240 * u, 95 * o, 110 * u, 55 * u);
            float dy = sticks ? 430 * o : 560 * o;
            circle(DPAD, left(330 * o, 125 * 1.1f * u, minLeft), dy, 125 * u);
            float fx = w - 300 * o, fy = dy, gap = 105 * u, br = 62 * u;
            circle(A, fx, fy + gap, br);   // Cross
            circle(B, fx + gap, fy, br);   // Circle
            circle(X, fx - gap, fy, br);   // Square
            circle(Y, fx, fy - gap, br);   // Triangle
            if (sticks) {   // the DualShock's, low and inward, clear of Select / Start
                circle(LSTICK, left(440 * o, 110 * STICK_SLOP * u, minLeft), h - 250 * o, 110 * u);
                circle(RSTICK, w - 440 * o, h - 250 * o, 110 * u);
                circle(L3, left(130 * o, 45 * BUTTON_SLOP * u, minLeft), h - 250 * o, 45 * u);
                circle(R3, w - 130 * o, h - 250 * o, 45 * u);
            }
            pill(SELECT, w / 2 - 110 * u, h - 60 * o, 70 * u, 32 * u);
            pill(START, w / 2 + 110 * u, h - 60 * o, 70 * u, 32 * u);
            break;
        }
        default: {   // N64
            circle(LSTICK, left(330 * o, 150 * STICK_SLOP * u, minLeft), 560 * o, 150 * u);
            pill(Z, left(330 * o, 100 * BUTTON_SLOP * u, minLeft), h - 110 * o, 100 * u, 55 * u);
            circle(A, w - 330 * o, 640 * o, 72 * u);
            circle(B, w - 470 * o, 510 * o, 62 * u);
            float cx = w - 190 * o, cy = 390 * o, gap = 88 * u, cr = 44 * u;
            circle(C_UP, cx, cy - gap, cr);
            circle(C_DOWN, cx, cy + gap, cr);
            circle(C_LEFT, cx - gap, cy, cr);
            circle(C_RIGHT, cx + gap, cy, cr);
            pill(START, w / 2, h - 60 * o, 70 * u, 32 * u);
            break;
        }
        }
    }

    /** A left-side x, pushed right so that x - reach stays out of the menu's edge zone. */
    private static float left(float x, float reach, float minLeft) {
        return Math.max(x, minLeft + reach);
    }

    private void circle(int id, float cx, float cy, float r) {
        Control c = control(id);
        c.cx = cx;
        c.cy = cy;
        c.r = r;
        c.hw = c.hh = 0;
    }

    private void pill(int id, float cx, float cy, float hw, float hh) {
        Control c = control(id);
        c.cx = cx;
        c.cy = cy;
        c.hw = hw;
        c.hh = hh;
        c.r = Math.min(hw, hh);
    }

    float width() {
        return width;
    }

    float height() {
        return height;
    }

    // ---- Fingers -------------------------------------------------------------------------------------------------

    void down(int pointer, float x, float y) {
        pointers.put(pointer, new float[] { x, y });
        Control c = at(x, y, true);
        if (c != null && c.kind != Kind.BUTTON)
            captured.put(pointer, c);
    }

    void move(int pointer, float x, float y) {
        float[] p = pointers.get(pointer);
        if (p == null)
            return;
        p[0] = x;
        p[1] = y;
    }

    void up(int pointer) {
        pointers.remove(pointer);
        captured.remove(pointer);
    }

    /** Everything released (ACTION_CANCEL, the view hidden or detached, the game stopping). */
    void cancel() {
        pointers.clear();
        captured.clear();
    }

    int fingers() {
        return pointers.size();
    }

    /** The control under (x, y): sticks and the D-pad only when landing (sliding never takes one). */
    Control at(float x, float y, boolean landing) {
        Control best = null;
        float bestD = Float.MAX_VALUE;
        for (Control c : controls) {
            if (c.hidden || (!landing && c.kind != Kind.BUTTON))
                continue;
            float slop = c.kind == Kind.STICK ? STICK_SLOP : c.kind == Kind.DPAD ? 1.1f : BUTTON_SLOP;
            if (!c.hit(x, y, slop))
                continue;
            float dx = x - c.cx, dy = y - c.cy, d = (dx * dx + dy * dy) / (c.r * c.r);
            if (d < bestD) {
                bestD = d;
                best = c;
            }
        }
        return best;
    }

    /** The pad as the fingers hold it now. */
    State state() {
        State s = new State();
        for (Map.Entry<Integer, float[]> e : pointers.entrySet()) {
            float x = e.getValue()[0], y = e.getValue()[1];
            Control c = captured.get(e.getKey());
            if (c == null)
                c = at(x, y, false);
            if (c == null)
                continue;
            switch (c.kind) {
            case STICK:
                stick(s, c, x, y);
                break;
            case DPAD:
                s.buttons |= dpad(c, x, y);
                break;
            default:
                s.buttons |= c.bits;
                if (c.pushX != 0)
                    s.rx = c.pushX * 32767;
                if (c.pushY != 0)
                    s.ry = c.pushY * 32767;
                if (c.analog == AN_LT)
                    s.lt = 255;
                else if (c.analog == AN_RT)
                    s.rt = 255;
                else if (c.analog >= 0)
                    s.analogMask |= 1 << c.analog;
                break;
            }
        }
        return s;
    }

    /** A stick: the finger's offset from the centre over the radius, clamped to the circle, Y up. */
    static void stick(State s, Control c, float x, float y) {
        float dx = (x - c.cx) / c.r, dy = (c.cy - y) / c.r;
        float m = (float)Math.sqrt(dx * dx + dy * dy);
        int vx = 0, vy = 0;
        if (m > DEAD) {
            // The dead zone is taken off and the rest rescaled, so the edge is still full tilt.
            float scaled = (Math.min(1f, m) - DEAD) / (1f - DEAD);
            vx = Math.round(dx / m * scaled * 32767f);
            vy = Math.round(dy / m * scaled * 32767f);
        }
        if (c.id == LSTICK) {
            s.lx = vx;
            s.ly = vy;
        } else {
            s.rx = vx;
            s.ry = vy;
        }
    }

    /** Eight directions: each arm covers 135 degrees, so the diagonals press two; the centre fifth presses none. */
    static int dpad(Control c, float x, float y) {
        float dx = x - c.cx, dy = y - c.cy;
        if (dx * dx + dy * dy < c.r * c.r * 0.04f)
            return 0;
        final float t = 2.4142f;   // tan(67.5 degrees)
        int b = 0;
        if (dx > 0 && Math.abs(dy) < dx * t)
            b |= XB_RIGHT;
        if (dx < 0 && Math.abs(dy) < -dx * t)
            b |= XB_LEFT;
        if (dy < 0 && Math.abs(dx) < -dy * t)
            b |= XB_UP;
        if (dy > 0 && Math.abs(dx) < dy * t)
            b |= XB_DOWN;
        return b;
    }
}
