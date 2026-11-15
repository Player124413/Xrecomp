package fr.adserver.xrecomp;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;

/**
 * The player's on-screen pad layout (XBA-207, docs/android/app-shell.md « Touch layout editor »): what the touch
 * layout editor changed on top of a console's default placement ({@link TouchPad#layout}). Pure Java (no Android).
 *
 * <p>One entry per changed control: its centre in normalised coordinates (x over the screen's width, y over its
 * height, NaN: the default place), its size as a factor of the default (on top of the global size setting), hidden or
 * not. A control with no entry keeps its default place, size and visibility, so an empty layout is the defaults and a
 * layout survives a change of resolution (a moved control keeps its share of the screen, an untouched one its default
 * anchoring to the edges).
 *
 * <p>Files: files/settings/touch_layout/&lt;LAYOUT&gt;.txt for a console's layout (TouchPad.Layout: XBOX, GBA, PS1,
 * PS1_ANALOG, N64), files/settings/touch_layout/game/&lt;KEY&gt;.&lt;LAYOUT&gt;.txt for « This game only » (KEY: the game's
 * key as in GameSettings). A game's file wins over its console's, whatever it holds (an empty one: the defaults for
 * this game even when the console has a layout). One line per control: « A x=0.8750 y=0.4630 size=1.20 hidden ».
 * The app runs in landscape only (sensorLandscape), so there is one layout per console, not one per orientation.
 */
final class TouchLayout {
    static final String DIR = "touch_layout";
    static final String GAME_DIR = "game";
    static final String SUFFIX = ".txt";
    static final float MIN_SIZE = 0.5f, MAX_SIZE = 2.5f;
    /** The editor's grid: this many cells over the screen's height (square cells). */
    static final int GRID_ROWS = 40;

    /** The control names in the files, by TouchPad id (index 0 unused). */
    private static final String[] NAMES = { null, "LSTICK", "RSTICK", "DPAD", "A", "B", "X", "Y", "BLACK", "WHITE",
                                            "LT", "RT", "BACK", "START", "L3", "R3", "L", "R", "L2", "R2", "Z", "C_UP",
                                            "C_DOWN", "C_LEFT", "C_RIGHT", "SELECT" };

    static final class Entry {
        float x = Float.NaN, y = Float.NaN;
        float size = 1f;
        boolean hidden;

        Entry copy() {
            Entry e = new Entry();
            e.x = x;
            e.y = y;
            e.size = size;
            e.hidden = hidden;
            return e;
        }

        @Override
        public boolean equals(Object o) {
            if (!(o instanceof Entry))
                return false;
            Entry e = (Entry)o;
            return same(x, e.x) && same(y, e.y) && Math.abs(size - e.size) < 0.005f && hidden == e.hidden;
        }

        @Override
        public int hashCode() {
            return Math.round(size * 100) * 2 + (hidden ? 1 : 0);
        }

        private static boolean same(float a, float b) {
            return Float.isNaN(a) ? Float.isNaN(b) : !Float.isNaN(b) && Math.abs(a - b) < 0.0005f;
        }
    }

    /** By TouchPad control id. */
    final Map<Integer, Entry> entries = new TreeMap<>();

    boolean isEmpty() {
        return entries.isEmpty();
    }

    TouchLayout copy() {
        TouchLayout t = new TouchLayout();
        for (Map.Entry<Integer, Entry> e : entries.entrySet())
            t.entries.put(e.getKey(), e.getValue().copy());
        return t;
    }

    @Override
    public boolean equals(Object o) {
        return o instanceof TouchLayout && entries.equals(((TouchLayout)o).entries);
    }

    @Override
    public int hashCode() {
        return entries.hashCode();
    }

    static String name(int id) {
        return id > 0 && id < NAMES.length ? NAMES[id] : null;
    }

    static int id(String name) {
        for (int i = 1; i < NAMES.length; i++)
            if (NAMES[i].equalsIgnoreCase(name))
                return i;
        return -1;
    }

    // ---- Text ---------------------------------------------------------------------------------------------------

    String format() {
        StringBuilder b = new StringBuilder("# XRecomp on-screen pad layout (Edit touch layout): a control not here keeps"
                                            + " its default.\n");
        for (Map.Entry<Integer, Entry> e : entries.entrySet()) {
            String n = name(e.getKey());
            if (n == null)
                continue;
            Entry t = e.getValue();
            b.append(n);
            if (!Float.isNaN(t.x))
                b.append(String.format(Locale.ROOT, " x=%.4f", t.x));
            if (!Float.isNaN(t.y))
                b.append(String.format(Locale.ROOT, " y=%.4f", t.y));
            if (Math.abs(t.size - 1f) >= 0.005f)
                b.append(String.format(Locale.ROOT, " size=%.2f", t.size));
            if (t.hidden)
                b.append(" hidden");
            b.append('\n');
        }
        return b.toString();
    }

    /** A layout from its text: unknown controls and fields, bad numbers and comments are skipped. */
    static TouchLayout parse(String text) {
        TouchLayout t = new TouchLayout();
        if (text == null)
            return t;
        for (String line : text.split("\n")) {
            line = line.trim();
            if (line.isEmpty() || line.startsWith("#"))
                continue;
            String[] f = line.split("\\s+");
            int id = id(f[0]);
            if (id < 0)
                continue;
            Entry e = new Entry();
            for (int i = 1; i < f.length; i++) {
                String k = f[i].toLowerCase(Locale.ROOT);
                try {
                    if (k.startsWith("x="))
                        e.x = unit(Float.parseFloat(k.substring(2)));
                    else if (k.startsWith("y="))
                        e.y = unit(Float.parseFloat(k.substring(2)));
                    else if (k.startsWith("size="))
                        e.size = Math.max(MIN_SIZE, Math.min(MAX_SIZE, Float.parseFloat(k.substring(5))));
                    else if (k.equals("hidden"))
                        e.hidden = true;
                } catch (NumberFormatException ignored) {
                    // the field keeps its default
                }
            }
            if (Float.isNaN(e.size))
                e.size = 1f;
            t.entries.put(id, e);
        }
        return t;
    }

    private static float unit(float v) {
        return Float.isNaN(v) || Float.isInfinite(v) ? Float.NaN : Math.max(0f, Math.min(1f, v));
    }

    // ---- The pad --------------------------------------------------------------------------------------------------

    /**
     * What the pad's controls (as the editor left them) change from the defaults at the same size: an entry for each
     * control moved (its place normalised), resized or hidden.
     */
    static TouchLayout capture(TouchPad pad) {
        TouchLayout t = new TouchLayout();
        float w = pad.width(), h = pad.height();
        if (w <= 0 || h <= 0)
            return pad.custom().copy();
        TouchPad d = new TouchPad(pad.layout);
        d.layout(w, h, pad.scale());
        for (TouchPad.Control c : pad.controls) {
            TouchPad.Control dc = d.control(c.id);
            boolean moved = Math.abs(c.cx - dc.cx) > 0.5f || Math.abs(c.cy - dc.cy) > 0.5f;
            boolean sized = Math.abs(c.size - 1f) >= 0.005f;
            if (!moved && !sized && !c.hidden)
                continue;
            Entry e = new Entry();
            if (moved) {
                e.x = c.cx / w;
                e.y = c.cy / h;
            }
            e.size = c.size;
            e.hidden = c.hidden;
            t.entries.put(c.id, e);
        }
        return t;
    }

    /** v on a grid of step (the editor's snap). */
    static float snap(float v, float step) {
        return step <= 0 ? v : Math.round(v / step) * step;
    }

    // ---- Files ----------------------------------------------------------------------------------------------------

    static File dir(File filesDir) {
        return new File(new File(filesDir, "settings"), DIR);
    }

    static File consoleFile(File filesDir, TouchPad.Layout l) {
        return new File(dir(filesDir), l.name() + SUFFIX);
    }

    /** The game's own file; null without a key. */
    static File gameFile(File filesDir, String key, TouchPad.Layout l) {
        String k = GameSettings.norm(key);
        if (k == null || k.isEmpty())
            return null;
        return new File(new File(dir(filesDir), GAME_DIR), k.replaceAll("[^A-Z0-9_.-]", "_") + "." + l.name() + SUFFIX);
    }

    /** The layout a game gets, and where from; repaired: the entries {@link #repair} dropped or fixed. */
    static final class Resolved {
        final TouchLayout layout;
        final boolean perGame;
        final int repaired;

        Resolved(TouchLayout layout, boolean perGame) {
            this(layout, perGame, 0);
        }

        Resolved(TouchLayout layout, boolean perGame, int repaired) {
            this.layout = layout;
            this.perGame = perGame;
            this.repaired = repaired;
        }
    }

    /**
     * The game's file if it has one, else its console's, else the defaults (an empty layout); what it holds repaired
     * ({@link #repair}) for the console's layout l.
     */
    static Resolved resolve(File filesDir, String key, TouchPad.Layout l) {
        File g = gameFile(filesDir, key, l);
        boolean perGame = g != null && g.isFile();
        File c = consoleFile(filesDir, l);
        TouchLayout t = perGame ? parse(read(g)) : c.isFile() ? parse(read(c)) : new TouchLayout();
        int fixed = t.repair(l);
        return new Resolved(t, perGame, fixed);
    }

    /**
     * Repairs a loaded layout in place for the console's layout l (XBA-212): drops the entries of controls l has not;
     * a size that is not a number, zero or negative goes back to the default (1), any other is kept in range; a place
     * that is not a number, off the screen or with its centre on an edge or in the in-game menu's swipe strip
     * (TouchPad.EDGE_FRAC from the left) goes back to the control's default place. Returns how many entries changed.
     */
    int repair(TouchPad.Layout l) {
        TouchPad pad = new TouchPad(l);
        int fixed = 0;
        java.util.Iterator<Map.Entry<Integer, Entry>> it = entries.entrySet().iterator();
        while (it.hasNext()) {
            Map.Entry<Integer, Entry> me = it.next();
            if (!pad.has(me.getKey())) {
                it.remove();
                fixed++;
                continue;
            }
            Entry e = me.getValue();
            boolean bad = false;
            if (Float.isNaN(e.size) || Float.isInfinite(e.size) || e.size <= 0f) {
                e.size = 1f;
                bad = true;
            } else if (e.size < MIN_SIZE || e.size > MAX_SIZE) {
                e.size = Math.max(MIN_SIZE, Math.min(MAX_SIZE, e.size));
                bad = true;
            }
            boolean placed = !Float.isNaN(e.x) || !Float.isNaN(e.y);
            if (placed && (!inside(e.x, TouchPad.EDGE_FRAC) || !inside(e.y, 0f))) {
                e.x = e.y = Float.NaN;
                bad = true;
            }
            if (bad)
                fixed++;
        }
        return fixed;
    }

    /** A normalised coordinate strictly inside (min, 1): NaN (the default) counts as inside. */
    private static boolean inside(float v, float min) {
        return Float.isNaN(v) || (!Float.isInfinite(v) && v > min && v < 1f);
    }

    /**
     * Settings › Controls › « Reset touch layouts » (XBA-212): every saved layout removed, the consoles' and the
     * games' own. Returns how many files went.
     */
    static int resetAll(File filesDir) {
        return deleteTree(dir(filesDir));
    }

    private static int deleteTree(File f) {
        int n = 0;
        File[] kids = f.listFiles();
        if (kids != null)
            for (File k : kids)
                n += deleteTree(k);
        if (f.isFile() && f.delete())
            return n + 1;
        f.delete();
        return n;
    }

    /**
     * Saves the editor's layout: for this game only (its own file, written even when empty) or for the console (the
     * game's file, if any, removed; an empty layout removes the console's file: the defaults).
     */
    static boolean save(File filesDir, String key, TouchPad.Layout l, TouchLayout t, boolean gameOnly) {
        File g = gameFile(filesDir, key, l);
        if (gameOnly) {
            return g != null && write(g, t.format());
        }
        if (g != null && g.isFile() && !g.delete())
            return false;
        File c = consoleFile(filesDir, l);
        if (t.isEmpty())
            return !c.isFile() || c.delete();
        return write(c, t.format());
    }

    private static String read(File f) {
        try {
            return new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8);
        } catch (IOException e) {
            return null;
        }
    }

    private static boolean write(File f, String text) {
        File parent = f.getParentFile(), tmp = new File(f.getPath() + ".tmp");
        if (parent != null && !parent.isDirectory() && !parent.mkdirs())
            return false;
        try (OutputStream o = new FileOutputStream(tmp)) {
            o.write(text.getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            tmp.delete();
            return false;
        }
        return tmp.renameTo(f);
    }
}
