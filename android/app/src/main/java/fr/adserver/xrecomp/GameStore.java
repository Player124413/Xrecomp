package fr.adserver.xrecomp;

import android.content.Context;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Environment;
import android.provider.DocumentsContract;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Locale;

/**
 * The game the player chose (XBA-54): where it is, remembered in SharedPreferences, and the checks on it
 * (default.xbe present, its SHA-256 against the one baked into a per-title APK, the title name from the XBE
 * certificate), and « Open the game directly next time ». Shared by LauncherActivity (process ":launcher") and ShellActivity (the main process): the
 * preferences are written with commit() before the main process starts, so it reads them fresh.
 */
final class GameStore {
    static final String PREFS = "game";
    static final String KEY_DIR = "game_dir";
    static final String KEY_TITLE = "title";
    /** « Open the game directly next time » (XBA-41): off by default, the launcher's home shows at each start. */
    static final String KEY_AUTO_START = "auto_start";

    /** Where an imported game (copied folder or extracted ISO) lives, and the native default. */
    static final String GAME_SUBDIR = "game";

    private GameStore() {}

    static SharedPreferences prefs(Context c) {
        return c.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    /** The remembered game folder, or null when none was chosen. */
    static String chosenDir(Context c) {
        String d = prefs(c).getString(KEY_DIR, null);
        return d == null || d.isEmpty() ? null : d;
    }

    static String chosenTitle(Context c) {
        return prefs(c).getString(KEY_TITLE, "");
    }

    static boolean remember(Context c, String dir, String title) {
        return prefs(c).edit().putString(KEY_DIR, dir).putString(KEY_TITLE, title == null ? "" : title).commit();
    }

    /** Forgets the chosen game (after its imported copy was deleted); the other preferences stay. */
    static boolean forget(Context c) {
        return prefs(c).edit().remove(KEY_DIR).remove(KEY_TITLE).commit();
    }

    static boolean autoStart(Context c) {
        return prefs(c).getBoolean(KEY_AUTO_START, false);
    }

    static boolean setAutoStart(Context c, boolean on) {
        return prefs(c).edit().putBoolean(KEY_AUTO_START, on).commit();
    }

    /**
     * « Bottom screen » on a device with a second display (AYN Thor, docs/android/app-shell.md « Dual screen »):
     * the companion panel (default), a black screen, or nothing (the system's own content stays there).
     */
    enum BottomScreen { PANEL, BLACK, OFF }

    /*
     * These two live in small files next to the preferences rather than in the SharedPreferences above: the launcher
     * (":launcher") writes them while the game's process may be alive, and SharedPreferences caches a file per process
     * (a running game would never see the change). A file is read fresh at every onStart.
     */
    static final String BOTTOM_SCREEN_FILE = "bottom_screen", COMPANION_FOCUS_FILE = "companion_focus";

    static BottomScreen bottomScreen(Context c) {
        String v = readSmall(c, BOTTOM_SCREEN_FILE);
        for (BottomScreen b : BottomScreen.values()) {
            if (b.name().equals(v))
                return b;
        }
        return BottomScreen.PANEL;
    }

    static boolean setBottomScreen(Context c, BottomScreen b) {
        return writeSmall(c, BOTTOM_SCREEN_FILE, b.name());
    }

    /** How the companion panel takes input focus (DualScreen.FOCUS_*), or null: the default. */
    static String companionFocusPref(Context c) {
        return readSmall(c, COMPANION_FOCUS_FILE);
    }

    static boolean setCompanionFocusPref(Context c, String mode) {
        return writeSmall(c, COMPANION_FOCUS_FILE, mode == null ? "" : mode);
    }

    /*
     * The in-game menu's Settings page (GameMenu, docs/android/app-shell.md « In-game menu »), written by the game's
     * process and read again at its next start: the performance overlay and the game's volume (0..100, 0 = muted).
     */
    static final String PERF_OVERLAY_FILE = "perf_overlay", GAME_VOLUME_FILE = "game_volume";

    static boolean perfOverlay(Context c) {
        return "1".equals(readSmall(c, PERF_OVERLAY_FILE));
    }

    static boolean setPerfOverlay(Context c, boolean on) {
        return writeSmall(c, PERF_OVERLAY_FILE, on ? "1" : "0");
    }

    /*
     * « Resume where I left off » (GBA / PS1 / N64 games, the in-game menu's Settings): on by default. On, the menu
     * saves the automatic slot when the player leaves the game (Quit, Back to launcher, the background) and the shell
     * passes RECOMP_STATE_RESUME=1 so the runtime loads it before its first frame (recomp_states.h).
     */
    static final String AUTO_RESUME_FILE = "auto_resume";

    static boolean autoResume(Context c) {
        return !"0".equals(readSmall(c, AUTO_RESUME_FILE));
    }

    static boolean setAutoResume(Context c, boolean on) {
        return writeSmall(c, AUTO_RESUME_FILE, on ? "1" : "0");
    }

    /*
     * A game's Home « Start » (GBA / PS1 / N64): this one launch begins from the console's boot, not where the player
     * left off. A one-shot file the launcher writes and the next game process deletes as it reads it, so a later
     * launch (or the in-game menu's Restart, a new process) resumes again.
     */
    static final String FRESH_START_FILE = "fresh_start";

    static void requestFreshStart(Context c) {
        writeSmall(c, FRESH_START_FILE, "1");
    }

    /** True once after {@link #requestFreshStart}: the file is deleted as it is read. */
    static boolean takeFreshStart(Context c) {
        File f = smallFile(c, FRESH_START_FILE);
        boolean on = "1".equals(readSmall(c, FRESH_START_FILE));
        f.delete();
        return on;
    }

    /** The automatic slot (« where you left off ») of a GBA / PS1 / N64 game: save/&lt;console&gt;-&lt;KEY&gt;/auto.st. */
    static File resumeState(Context c, String tid) {
        return new File(new File(GameTheme.saveDir(c), tid), "auto.st");
    }

    /** Whether a game's Home offers Resume: « Resume where I left off » on and its automatic slot saved. */
    static boolean canResume(Context c, String tid) {
        return tid != null && autoResume(c) && resumeState(c, tid).isFile();
    }

    static int gameVolume(Context c) {
        String v = readSmall(c, GAME_VOLUME_FILE);
        try {
            return v == null ? 100 : Math.max(0, Math.min(100, Integer.parseInt(v)));
        } catch (NumberFormatException e) {
            return 100;
        }
    }

    static boolean setGameVolume(Context c, int percent) {
        return writeSmall(c, GAME_VOLUME_FILE, Integer.toString(Math.max(0, Math.min(100, percent))));
    }

    /*
     * Per title: the toolkit's optional enhancements (profiles/titles/<id>: a 16:9 mod, a front-end rate...; profile-free
     * WP6), on by default. A file, as the Prepare runs in its own process (":prepare"), which passes
     * XBA_NO_TITLE_LAYER=1 to tools.prepare when they are off. The title runs without them.
     */
    static final String ENHANCEMENTS_FILE = "enhancements_";

    static boolean enhancements(Context c, String titleId) {
        String id = Pack.normTitleId(titleId);
        return id == null || !"off".equals(readSmall(c, ENHANCEMENTS_FILE + id));
    }

    static boolean setEnhancements(Context c, String titleId, boolean on) {
        String id = Pack.normTitleId(titleId);
        return id != null && writeSmall(c, ENHANCEMENTS_FILE + id, on ? "on" : "off");
    }

    /** A one-line setting in files/settings (GameLanguage's per-title choice); null when missing or blank. */
    static String readSettingFile(Context c, String name) {
        return readSmall(c, name);
    }

    static boolean writeSettingFile(Context c, String name, String value) {
        return writeSmall(c, name, value);
    }

    private static File smallFile(Context c, String name) {
        return new File(new File(c.getFilesDir(), "settings"), name);
    }

    static String readSmall(Context c, String name) {
        File f = smallFile(c, name);
        if (!f.isFile())
            return null;
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            String s = r.readLine();
            return s == null || s.trim().isEmpty() ? null : s.trim();
        } catch (IOException e) {
            return null;
        }
    }

    static boolean writeSmall(Context c, String name, String value) {
        File f = smallFile(c, name), tmp = new File(f.getPath() + ".tmp");
        File d = f.getParentFile();
        if (d != null && !(d.mkdirs() || d.isDirectory()))
            return false;
        try (java.io.FileOutputStream o = new java.io.FileOutputStream(tmp)) {
            o.write((value + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            return false;
        }
        return tmp.renameTo(f);
    }

    /** The launcher's UI sounds and haptics (Settings › Launcher), on by default. */
    static final String KEY_UI_SOUND = "ui_sound", KEY_UI_HAPTICS = "ui_haptics";

    static boolean uiSound(Context c) {
        return prefs(c).getBoolean(KEY_UI_SOUND, true);
    }

    static boolean uiHaptics(Context c) {
        return prefs(c).getBoolean(KEY_UI_HAPTICS, true);
    }

    static boolean setUiSound(Context c, boolean on) {
        return prefs(c).edit().putBoolean(KEY_UI_SOUND, on).commit();
    }

    static boolean setUiHaptics(Context c, boolean on) {
        return prefs(c).edit().putBoolean(KEY_UI_HAPTICS, on).commit();
    }

    // ---- Play time (Home's « Last played » and « Play time »), kept simple ----------------------------------------
    // A session starts when the launcher starts ShellActivity and ends when the launcher shows again with the game no
    // longer running. A session left open longer than SESSION_MAX (the launcher never came back: the app was swiped
    // away or the device rebooted) counts for SESSION_MAX.
    static final String KEY_LAST_PLAYED = "last_played", KEY_PLAY_MS = "play_ms", KEY_SESSION_START = "session_start",
            KEY_SESSION_TID = "session_tid";
    static final long SESSION_MAX = 6 * 3600_000L;

    /** When the game was last started (System.currentTimeMillis), 0 if never. */
    static long lastPlayed(Context c) {
        return prefs(c).getLong(KEY_LAST_PLAYED, 0);
    }

    /** Total play time in ms, closed sessions only. */
    static long playTime(Context c) {
        return prefs(c).getLong(KEY_PLAY_MS, 0);
    }

    static void sessionStart(Context c, long now) {
        prefs(c).edit().putLong(KEY_LAST_PLAYED, now).putLong(KEY_SESSION_START, now).commit();
    }

    /** sessionStart() of the game library: also the title the session belongs to (session_tid, its record). */
    static void sessionStart(Context c, long now, String titleId) {
        SharedPreferences.Editor e = prefs(c).edit().putLong(KEY_LAST_PLAYED, now).putLong(KEY_SESSION_START, now);
        if (titleId == null)
            e.remove(KEY_SESSION_TID);
        else
            e.putString(KEY_SESSION_TID, titleId);
        e.commit();
    }

    /** When the open session started, 0 when none is open. */
    static long sessionStartedAt(Context c) {
        return prefs(c).getLong(KEY_SESSION_START, 0);
    }

    /** The title ID of the open (or last) session, or null. */
    static String sessionTid(Context c) {
        String s = prefs(c).getString(KEY_SESSION_TID, null);
        return s == null || s.isEmpty() ? null : s;
    }

    /**
     * The game the process will run, written at launch only (docs « Game library »: the launcher's selected game is
     * another thing): game_dir, title and title_id. A null dir forgets the folder (recomp.cfg's RECOMP_GAME_DIR or
     * the default game/ then apply).
     */
    static boolean rememberLaunch(Context c, String dir, String title, String titleId) {
        SharedPreferences.Editor e = prefs(c).edit().remove(KEY_ROM).remove(KEY_ROM_TITLE);
        if (dir == null)
            e.remove(KEY_DIR).remove(KEY_TITLE);
        else
            e.putString(KEY_DIR, dir).putString(KEY_TITLE, title == null ? "" : title);
        if (titleId == null || Library.isLocalKey(titleId))
            e.remove(Library.PREF_TITLE_ID);
        else
            e.putString(Library.PREF_TITLE_ID, titleId);
        return e.commit();
    }

    /**
     * A game started from its file as it is (a GBA ROM, XBA-157): the file the game process runs, a path or a
     * « content: » document URI (ShellActivity opens it and hands « fd:N » to the runtime); game_dir and title stay
     * the Xbox game's (the launcher's one-game rules keep working), title_id becomes the ROM's library key. Any Xbox
     * launch (rememberLaunch) clears it.
     */
    static final String KEY_ROM = "rom", KEY_ROM_TITLE = "rom_title";

    static boolean rememberRomLaunch(Context c, String rom, String title, String key) {
        return rememberRomLaunch(c, rom, title, key, null);
    }

    /**
     * The same with the track documents of a PS1 disc's .cue read through the Storage Access Framework (XBA-168,
     * LauncherActivity.romParts; null or empty: none), which ShellActivity opens beside the .cue (Ps1Cue).
     */
    static final String KEY_ROM_PARTS = "rom_parts";

    static boolean rememberRomLaunch(Context c, String rom, String title, String key, java.util.List<String> parts) {
        android.content.SharedPreferences.Editor e = prefs(c).edit().putString(KEY_ROM, rom)
                .putString(KEY_ROM_TITLE, title == null ? "" : title).putString(Library.PREF_TITLE_ID, key);
        if (parts == null || parts.isEmpty())
            e.remove(KEY_ROM_PARTS);
        else
            e.putString(KEY_ROM_PARTS, String.join("\n", parts));
        return e.commit();
    }

    /** The track documents of the launched ROM (rememberRomLaunch), in order; empty when it has none. */
    static java.util.List<String> launchedRomParts(Context c) {
        java.util.List<String> out = new java.util.ArrayList<>();
        for (String p : prefs(c).getString(KEY_ROM_PARTS, "").split("\n"))
            if (!p.isEmpty())
                out.add(p);
        return out;
    }

    /** The ROM the game process runs (or last ran), or null when that is an Xbox game. */
    static String launchedRom(Context c) {
        String s = prefs(c).getString(KEY_ROM, null);
        return s == null || s.isEmpty() ? null : s;
    }

    static String launchedRomTitle(Context c) {
        return prefs(c).getString(KEY_ROM_TITLE, "");
    }

    /** The title ID of the game last launched (title_id), or null. */
    static String launchedTitleId(Context c) {
        String s = prefs(c).getString(Library.PREF_TITLE_ID, null);
        return s == null || s.isEmpty() ? null : s;
    }

    /** Closes an open session (adds its length to the play time); nothing when none is open. */
    static void sessionEnd(Context c, long now) {
        long start = prefs(c).getLong(KEY_SESSION_START, 0);
        if (start <= 0)
            return;
        sessionEnd(c, now, Math.max(0, Math.min(SESSION_MAX, now - start)));
    }

    /** The same with the session's length already known (PlayClock.sessionLength: the time the game was on screen). */
    static void sessionEnd(Context c, long now, long length) {
        if (prefs(c).getLong(KEY_SESSION_START, 0) <= 0)
            return;
        long d = Math.max(0, Math.min(SESSION_MAX, length));
        prefs(c).edit().putLong(KEY_PLAY_MS, playTime(c) + d).remove(KEY_SESSION_START).commit();
    }

    static File externalFilesDir(Context c) {
        return c.getExternalFilesDir(null);
    }

    /** <external files dir>/game: recomp_storage.c's default and the import target. */
    static File defaultGameDir(Context c) {
        File ext = externalFilesDir(c);
        return ext == null ? null : new File(ext, GAME_SUBDIR);
    }

    /** default.xbe in dir, any case (the kernel's lookup is case-insensitive too); null when missing or unreadable. */
    static File findXbe(File dir) {
        if (dir == null)
            return null;
        File[] list = dir.listFiles();
        if (list == null)
            return null;
        for (File f : list) {
            if (f.getName().equalsIgnoreCase("default.xbe") && f.isFile() && f.canRead())
                return f;
        }
        return null;
    }

    /** RECOMP_GAME_DIR from recomp.cfg (the native side reads the file too), or null. */
    static String cfgGameDir(Context c) {
        return cfgValue(c, "RECOMP_GAME_DIR");
    }

    /** A variable's value in recomp.cfg (the first NAME= line), or null when absent or empty. */
    static String cfgValue(Context c, String name) {
        File ext = externalFilesDir(c);
        if (ext == null)
            return null;
        File cfg = new File(ext, "recomp.cfg");
        if (!cfg.isFile())
            return null;
        try (BufferedReader r = new BufferedReader(new FileReader(cfg))) {
            String line;
            while ((line = r.readLine()) != null) {
                line = line.trim();
                if (line.startsWith(name + "=")) {
                    String v = line.substring(name.length() + 1).trim();
                    return v.isEmpty() ? null : v;
                }
            }
        } catch (IOException ignored) {
        }
        return null;
    }

    /**
     * The SHA-256 of default.xbe this APK is for, lower case; "" = no check. A game's own app made on the device
     * (« Create an app », TitleApk) says it in its manifest's meta-data; a title APK built on a host in its
     * resources (-PxbaXbeSha256).
     */
    static String expectedSha256(Context c) {
        String own = TitleApp.ownXbeSha256(c).trim();
        if (!own.isEmpty())
            return own.toLowerCase(Locale.ROOT);
        return c.getString(R.string.xba_xbe_sha256).trim().toLowerCase(Locale.ROOT);
    }

    /** True when there is no expected hash or it matches. */
    static boolean hashMatches(Context c, String actual) {
        String want = expectedSha256(c);
        return want.isEmpty() || want.equals(actual);
    }

    static String sha256(InputStream in) throws IOException {
        MessageDigest md;
        try {
            md = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new IOException(e);
        }
        byte[] buf = new byte[1 << 16];
        int n;
        while ((n = in.read(buf)) > 0)
            md.update(buf, 0, n);
        StringBuilder sb = new StringBuilder(64);
        for (byte b : md.digest())
            sb.append(String.format(Locale.ROOT, "%02x", b & 0xff));
        return sb.toString();
    }

    static String sha256(File f) throws IOException {
        try (InputStream in = new FileInputStream(f)) {
            return sha256(in);
        }
    }

    /**
     * The title name from the XBE certificate (UTF-16LE, 40 characters at certificate + 0x0C), or "" when the file
     * is not an XBE. Header: "XBEH", base address at 0x104, certificate address at 0x118.
     */
    static String xbeTitle(File xbe) {
        try (RandomAccessFile f = new RandomAccessFile(xbe, "r")) {
            byte[] hdr = new byte[0x11c];
            f.readFully(hdr);
            if (hdr[0] != 'X' || hdr[1] != 'B' || hdr[2] != 'E' || hdr[3] != 'H')
                return "";
            long base = le32(hdr, 0x104), cert = le32(hdr, 0x118);
            long off = cert - base + 0x0c;
            if (cert < base || off + 80 > f.length())
                return "";
            byte[] name = new byte[80];
            f.seek(off);
            f.readFully(name);
            int len = 0;
            while (len < 80 && (name[len] != 0 || name[len + 1] != 0))
                len += 2;
            return new String(name, 0, len, StandardCharsets.UTF_16LE).trim();
        } catch (IOException e) {
            return "";
        }
    }

    private static long le32(byte[] b, int o) {
        return (b[o] & 0xffL) | (b[o + 1] & 0xffL) << 8 | (b[o + 2] & 0xffL) << 16 | (b[o + 3] & 0xffL) << 24;
    }

    /**
     * The real path of a folder picked with ACTION_OPEN_DOCUMENT_TREE, readable with POSIX calls once the app has
     * All files access; null when the provider is not a storage volume (the folder is then copied instead).
     * externalstorage: "primary:Games/Halo" is /storage/emulated/0/Games/Halo, "1A2B-3C4D:Halo" is
     * /storage/1A2B-3C4D/Halo, "home:X" is Documents/X on the primary volume. The downloads provider's "raw:/path"
     * ids are a path already.
     */
    static String treeUriToPath(Uri tree) {
        String auth = tree.getAuthority();
        String id;
        try {
            id = DocumentsContract.getTreeDocumentId(tree);
        } catch (IllegalArgumentException e) {
            return null;
        }
        if ("com.android.providers.downloads.documents".equals(auth) && id.startsWith("raw:"))
            return id.substring(4);
        if (!"com.android.externalstorage.documents".equals(auth))
            return null;
        int colon = id.indexOf(':');
        if (colon < 0)
            return null;
        String vol = id.substring(0, colon), rel = id.substring(colon + 1);
        String root;
        if (vol.equals("primary"))
            root = Environment.getExternalStorageDirectory().getAbsolutePath();
        else if (vol.equals("home"))
            root = new File(Environment.getExternalStorageDirectory(), Environment.DIRECTORY_DOCUMENTS).getAbsolutePath();
        else
            root = "/storage/" + vol;
        while (rel.startsWith("/"))
            rel = rel.substring(1);
        while (rel.endsWith("/"))
            rel = rel.substring(0, rel.length() - 1);
        return rel.isEmpty() ? root : root + "/" + rel;
    }

    static void deleteTree(File f) {
        if (f == null || !f.exists())
            return;
        File[] list = f.isDirectory() ? f.listFiles() : null;
        if (list != null) {
            for (File c : list)
                deleteTree(c);
        }
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }

    static long treeSize(File f) {
        if (f == null || !f.exists())
            return 0;
        if (!f.isDirectory())
            return f.length();
        long n = 0;
        File[] list = f.listFiles();
        if (list != null) {
            for (File c : list)
                n += treeSize(c);
        }
        return n;
    }

    /**
     * Puts an imported tree in place of <external files dir>/game: game -> game.old, staged -> game, game.old
     * removed; on a failed rename the old one goes back. Returns the new game dir or null.
     */
    static File swapIntoGameDir(Context c, File staged) {
        return swapInto(staged, defaultGameDir(c));
    }

    /**
     * Puts staged in place of game (the game library's games/<TID>): game -> <name>.old, staged -> game, the old one
     * removed; on a failed rename the old one goes back. Returns game or null.
     */
    static File swapInto(File staged, File game) {
        if (game == null)
            return null;
        File parent = game.getParentFile();
        if (parent != null && !(parent.mkdirs() || parent.isDirectory()))
            return null;
        File old = new File(parent, game.getName() + ".old");
        deleteTree(old);
        boolean hadOld = game.exists();
        if (hadOld && !game.renameTo(old))
            return null;
        if (!staged.renameTo(game)) {
            if (hadOld)
                //noinspection ResultOfMethodCallIgnored
                old.renameTo(game);
            return null;
        }
        deleteTree(old);
        return game;
    }
}
