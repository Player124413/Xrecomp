package fr.adserver.xrecomp;

import android.content.Context;
import android.os.Build;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Enumeration;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.UUID;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipException;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;

/**
 * Custom Vulkan drivers for the N64 Vulkan RDP (an adrenotools package such as Mesa Turnip): validation, storage, the
 * active choice, and what ShellActivity hands to the native runtime. Drivers come from third parties and are never
 * part of the APK; they run as native code inside the game process.
 *
 * Layout (files/ of this app; a game's own app has its own):
 * <pre>
 *   vk_drivers/&lt;id&gt;/meta.json      the package's meta.json, as shipped
 *   vk_drivers/&lt;id&gt;/&lt;libraryName&gt;  the driver library (and any other flat file of the package), read-only
 *   vk_drivers/&lt;id&gt;/origin.json    where it came from: kind, repo, tag, asset, sha256, size, installed
 *   vk_drivers/trying              written by the native runtime before it opens a custom driver: the id
 *   vk_drivers/last_start          the native runtime's last start with a custom driver: « &lt;id&gt; &lt;status&gt; »
 *   vk_drivers/&lt;id&gt;.status       the same status, per driver (the last start that used that driver)
 *   vk_drivers/index/              VkDriverIndex's cache (not a driver)
 *   vk_drivers/.tmp-*, .del-*      an install or a delete under way (removed at the next install)
 *   settings/vk_driver             the global choice: an id, or empty / missing for the system driver
 *   settings/vk_driver_&lt;KEY&gt;       a game's own choice (library key, upper case): an id, « system », or empty /
 *                                  missing to follow the global choice
 * </pre>
 * &lt;id&gt; is the first 16 hex digits (lower case) of the package zip's SHA-256: stable, never a name the package
 * chooses. Excluded from backups (res/xml) and from LogProvider.
 *
 * The native runtime (consoles/n64/runtime/n64_vk_driver.c) reads {@link #ENV_DIR} (the driver directory, absolute,
 * ending in « / »), {@link #ENV_LIB} (meta.json's libraryName), {@link #ENV_HOOK_DIR} (nativeLibraryDir) and
 * {@link #ENV_ID}; {@link #ENV_FORCE} = « system » (recomp.cfg) wins over them and is never set here.
 *
 * Compiled shaders (consoles/n64/runtime/n64_vk_cache.h): {@link #ENV_CACHE_DIR} =
 * &lt;cache&gt;/vk_cache/&lt;id or « system »&gt;/ (getCacheDir(): the system may clear it, the runtime then
 * compiles again), where the runtime keeps the Vulkan pipeline cache (pipelines-&lt;UUID&gt;.bin) and Mesa's own
 * shader cache (mesa/).
 */
final class VkDrivers {
    static final String DIR = "vk_drivers";
    static final String META = "meta.json";
    static final String ORIGIN = "origin.json";
    /** The trial marker the native runtime writes (files/vk_drivers/trying, the id inside). */
    static final String TRIAL_MARKER = "trying";
    /** The global choice, a one-line file in files/settings (the GameStore.readSettingFile pattern). */
    static final String SETTING = "vk_driver";
    /** A game's own choice: files/settings/vk_driver_&lt;KEY&gt; (as N64Pak's n64_pak_&lt;KEY&gt;). */
    static final String GAME_SETTING_PREFIX = "vk_driver_";
    /** A game's choice of the system driver (whatever the global choice is). */
    static final String SYSTEM = "system";
    /** The per-driver status file next to the drivers: vk_drivers/&lt;id&gt;.status (the native runtime writes it). */
    static final String STATUS_SUFFIX = ".status";

    static final String ENV_DIR = "RECOMP_VK_DRIVER_DIR",
                        ENV_LIB = "RECOMP_VK_DRIVER_LIB",
                        ENV_HOOK_DIR = "RECOMP_VK_HOOK_DIR",
                        ENV_ID = "RECOMP_VK_DRIVER_ID",
                        ENV_FORCE = "RECOMP_VK_DRIVER",
                        ENV_CACHE_DIR = "RECOMP_VK_CACHE_DIR";
    /** Under getCacheDir(): one directory per driver id, and « system » (SYSTEM) for the system driver. */
    static final String CACHE_DIR = "vk_cache";

    static final int MAX_ENTRIES = 16;
    static final long MAX_TOTAL = 256L << 20;      // uncompressed, all entries together
    static final long MAX_ZIP = 256L << 20;        // the package file itself
    static final int MAX_META = 64 << 10;
    static final int SCHEMA_VERSION = 1;
    static final int ID_HEX = 16;

    private static final Pattern NAME = Pattern.compile("[A-Za-z0-9._-]+");
    private static final Pattern LIB = Pattern.compile("[A-Za-z0-9._-]+\\.so");
    private static final Pattern ID = Pattern.compile("[0-9a-f]{" + ID_HEX + "}");
    private static final Pattern SHA256 = Pattern.compile("[0-9a-fA-F]{64}");
    private static final Object LOCK = new Object();

    /** Why a package was refused. Nothing of it is kept. */
    enum Reason {
        NOT_A_ZIP, TOO_MANY_ENTRIES, BAD_ENTRY_NAME, DUPLICATE_ENTRY, TOO_LARGE, NO_META, BAD_META, NO_LIBRARY,
        BAD_ELF, MIN_API, SHA256_MISMATCH, SIZE_MISMATCH, BAD_ID, IO
    }

    static final class InstallException extends IOException {
        final Reason reason;

        InstallException(Reason reason, String detail) {
            super(reason + (detail != null ? ": " + detail : ""));
            this.reason = reason;
        }

        InstallException(Reason reason, String detail, Throwable cause) {
            this(reason, detail);
            initCause(cause);
        }
    }

    /**
     * Where a package comes from, written to origin.json. {@link #sha256} / {@link #size}, when set, are what the
     * package must match (VkDriverIndex: the GitHub asset's digest and size); origin.json then records the actual
     * values either way.
     */
    static final class Origin {
        static final String MANUAL = "manual", GITHUB = "github", INHERITED = "inherited";
        final String kind;
        final String repo, tag, asset;
        final String sha256;
        final long size;
        /** INHERITED only: the parent's id, kept so the trial marker and the choice line up. */
        final String id;

        private Origin(String kind, String repo, String tag, String asset, String sha256, long size, String id) {
            this.kind = kind;
            this.repo = repo;
            this.tag = tag;
            this.asset = asset;
            this.sha256 = sha256;
            this.size = size;
            this.id = id;
        }

        /** A zip the player picked (SAF import); nothing to match. */
        static Origin manual() {
            return new Origin(MANUAL, null, null, null, null, -1, null);
        }

        /** A GitHub release asset: repo « owner/name », tag, asset name, expected sha256 (or null) and size (or -1). */
        static Origin github(String repo, String tag, String asset, String sha256, long size) {
            return new Origin(GITHUB, repo, tag, asset, sha256, size, null);
        }

        /** A game's own app taking XRecomp's driver: the parent's id and its origin's description. */
        static Origin inherited(String parentId, String repo, String tag, String asset) {
            return new Origin(INHERITED, repo, tag, asset, null, -1, parentId);
        }
    }

    /** An installed driver: meta.json's fields and origin.json's. */
    static final class Driver {
        final String id;
        final File dir;
        final String name, description, author, packageVersion, vendor, driverVersion, libraryName;
        final int minApi;
        final String originKind, repo, tag, asset, sha256;
        final long zipSize, installed;

        Driver(String id, File dir, JSONObject meta, JSONObject origin) {
            this.id = id;
            this.dir = dir;
            name = meta.optString("name");
            description = meta.optString("description");
            author = meta.optString("author");
            packageVersion = meta.optString("packageVersion");
            vendor = meta.optString("vendor");
            driverVersion = meta.optString("driverVersion");
            libraryName = meta.optString("libraryName");
            minApi = meta.optInt("minApi", 0);
            JSONObject o = origin != null ? origin : new JSONObject();
            originKind = o.optString("kind", Origin.MANUAL);
            repo = o.optString("repo", null);
            tag = o.optString("tag", null);
            asset = o.optString("asset", null);
            sha256 = o.optString("sha256", null);
            zipSize = o.optLong("size", -1);
            installed = o.optLong("installed", 0);
        }

        File library() {
            return new File(dir, libraryName);
        }

        /** The bytes the driver takes on disk (its directory's files). */
        long sizeOnDisk() {
            long n = 0;
            File[] fs = dir.listFiles();
            if (fs != null)
                for (File f : fs)
                    n += f.length();
            return n;
        }

        @Override
        public String toString() {
            return name + " (" + id + ")";
        }
    }

    private final File filesDir;
    private final int sdkInt;

    VkDrivers(File filesDir, int sdkInt) {
        this.filesDir = filesDir;
        this.sdkInt = sdkInt;
    }

    static VkDrivers of(Context c) {
        return new VkDrivers(c.getFilesDir(), Build.VERSION.SDK_INT);
    }

    File root() {
        return new File(filesDir, DIR);
    }

    static boolean validId(String id) {
        return id != null && ID.matcher(id).matches();
    }

    // ---- Install ---------------------------------------------------------------------------------------------------

    /** Copies the stream to a temporary file under vk_drivers/ (at most MAX_ZIP bytes), then installs it. */
    Driver install(InputStream zip, Origin origin) throws IOException {
        synchronized (LOCK) {
            File tmp = newTmp();
            try {
                if (!tmp.mkdirs())
                    throw new InstallException(Reason.IO, "cannot create " + tmp);
                File f = new File(tmp, "package.zip");
                try (OutputStream o = new FileOutputStream(f)) {
                    byte[] buf = new byte[1 << 16];
                    long n = 0;
                    for (int r; (r = zip.read(buf)) > 0; ) {
                        n += r;
                        if (n > MAX_ZIP)
                            throw new InstallException(Reason.TOO_LARGE, "package over " + MAX_ZIP + " bytes");
                        o.write(buf, 0, r);
                    }
                }
                return installLocked(f, origin);
            } finally {
                deleteTree(tmp);
            }
        }
    }

    /**
     * Installs a package zip (VkDriverIndex's verified download, or a manual import's copy); the file itself is left
     * where it is. Returns the installed driver; a package already installed (same id) is returned as it is. Throws an
     * {@link InstallException} naming why when it is refused, and keeps nothing of it then.
     */
    Driver install(File zip, Origin origin) throws IOException {
        synchronized (LOCK) {
            return installLocked(zip, origin);
        }
    }

    private Driver installLocked(File zip, Origin origin) throws IOException {
        if (origin == null)
            origin = Origin.manual();
        cleanLeftovers(zip.getParentFile());
        long size = zip.length();
        if (size > MAX_ZIP)
            throw new InstallException(Reason.TOO_LARGE, "package over " + MAX_ZIP + " bytes");
        if (origin.size >= 0 && origin.size != size)
            throw new InstallException(Reason.SIZE_MISMATCH, size + " bytes, expected " + origin.size);
        String sha = sha256(zip);
        if (origin.sha256 != null) {
            String want = stripDigest(origin.sha256);
            if (!SHA256.matcher(want).matches() || !want.equalsIgnoreCase(sha))
                throw new InstallException(Reason.SHA256_MISMATCH, sha + ", expected " + origin.sha256);
        }
        String id = origin.id != null ? origin.id : sha.substring(0, ID_HEX);
        if (!validId(id))
            throw new InstallException(Reason.BAD_ID, id);
        File dst = new File(root(), id);
        Driver existing = read(id);
        if (existing != null)
            return existing;
        File tmp = newTmp();
        try {
            if (!tmp.mkdirs())
                throw new InstallException(Reason.IO, "cannot create " + tmp);
            JSONObject meta = extract(zip, tmp);
            JSONObject o = new JSONObject();
            try {
                o.put("kind", origin.kind);
                o.putOpt("repo", origin.repo);
                o.putOpt("tag", origin.tag);
                o.putOpt("asset", origin.asset);
                o.put("sha256", sha);
                o.put("size", size);
                o.put("installed", System.currentTimeMillis());
            } catch (JSONException e) {
                throw new InstallException(Reason.IO, "origin.json", e);
            }
            writeFile(new File(tmp, ORIGIN), o.toString().getBytes(StandardCharsets.UTF_8));
            File[] fs = tmp.listFiles();
            if (fs != null)
                for (File f : fs)
                    f.setWritable(false, false);
            if (dst.exists())
                deleteTree(dst);   // a broken leftover (read() refused it)
            if (!tmp.renameTo(dst))
                throw new InstallException(Reason.IO, "cannot rename to " + dst);
            Driver d = read(id);
            if (d == null)
                throw new InstallException(Reason.IO, "installed but unreadable: " + dst);
            return d;
        } finally {
            if (tmp.exists())
                deleteTree(tmp);
        }
    }

    /** Checks the zip and writes its entries into dir; returns meta.json. */
    JSONObject extract(File zip, File dir) throws IOException {
        ZipFile zf;
        try {
            zf = new ZipFile(zip);
        } catch (ZipException e) {
            throw new InstallException(Reason.NOT_A_ZIP, e.getMessage(), e);
        }
        try {
            List<ZipEntry> entries = new ArrayList<>();
            Set<String> names = new HashSet<>(), folded = new HashSet<>();
            Enumeration<? extends ZipEntry> en = zf.entries();
            while (en.hasMoreElements()) {
                ZipEntry e = en.nextElement();
                if (entries.size() >= MAX_ENTRIES)
                    throw new InstallException(Reason.TOO_MANY_ENTRIES, "over " + MAX_ENTRIES);
                String n = e.getName();
                if (e.isDirectory() || !safeName(n))
                    throw new InstallException(Reason.BAD_ENTRY_NAME, n);
                if (!folded.add(n.toLowerCase(Locale.ROOT)) || !names.add(n))
                    throw new InstallException(Reason.DUPLICATE_ENTRY, n);
                if (e.getSize() > MAX_TOTAL)
                    throw new InstallException(Reason.TOO_LARGE, n);
                entries.add(e);
            }
            ZipEntry metaEntry = zf.getEntry(META);
            if (metaEntry == null || !names.contains(META))
                throw new InstallException(Reason.NO_META, null);
            if (metaEntry.getSize() > MAX_META)
                throw new InstallException(Reason.BAD_META, "meta.json over " + MAX_META + " bytes");
            JSONObject meta;
            try (InputStream in = zf.getInputStream(metaEntry)) {
                meta = parseMeta(readAll(in, MAX_META));
            }
            String lib = meta.optString("libraryName");
            if (!names.contains(lib) || zf.getEntry(lib) == null)
                throw new InstallException(Reason.NO_LIBRARY, lib);
            long[] total = {0};
            for (ZipEntry e : entries) {
                File out = new File(dir, e.getName());
                if (!out.getParentFile().equals(dir))
                    throw new InstallException(Reason.BAD_ENTRY_NAME, e.getName());
                try (InputStream in = zf.getInputStream(e); OutputStream o = new FileOutputStream(out)) {
                    copyCapped(in, o, total);
                }
            }
            checkElf(new File(dir, lib));
            return meta;
        } finally {
            zf.close();
        }
    }

    /** A flat file name of the package: [A-Za-z0-9._-]+, not « . » or « .. », not a name of ours. */
    static boolean safeName(String n) {
        return n != null && n.length() <= 128 && NAME.matcher(n).matches() && !n.equals(".") && !n.equals("..")
               && !n.equals(ORIGIN);
    }

    /** meta.json checked: schemaVersion 1, a name, libraryName a plain .so name, minApi (when set) &lt;= sdkInt. */
    JSONObject parseMeta(byte[] bytes) throws InstallException {
        JSONObject m;
        try {
            m = new JSONObject(new String(bytes, StandardCharsets.UTF_8));
        } catch (JSONException e) {
            throw new InstallException(Reason.BAD_META, "not JSON", e);
        }
        if (m.optInt("schemaVersion", -1) != SCHEMA_VERSION)
            throw new InstallException(Reason.BAD_META, "schemaVersion " + m.opt("schemaVersion"));
        if (m.optString("name").trim().isEmpty())
            throw new InstallException(Reason.BAD_META, "no name");
        String lib = m.optString("libraryName");
        if (!LIB.matcher(lib).matches() || !safeName(lib))
            throw new InstallException(Reason.BAD_META, "libraryName " + lib);
        if (m.has("minApi")) {
            int min = m.optInt("minApi", Integer.MAX_VALUE);
            if (min > sdkInt)
                throw new InstallException(Reason.MIN_API, "minApi " + m.opt("minApi") + ", device " + sdkInt);
        }
        return m;
    }

    static final int EM_AARCH64 = 183, ET_DYN = 3;

    /** ELF, 64-bit, little-endian, ET_DYN, EM_AARCH64. */
    static void checkElf(File f) throws IOException {
        byte[] h = new byte[20];
        int n = 0;
        try (InputStream in = new FileInputStream(f)) {
            for (int r; n < h.length && (r = in.read(h, n, h.length - n)) > 0; )
                n += r;
        }
        if (n < h.length || h[0] != 0x7f || h[1] != 'E' || h[2] != 'L' || h[3] != 'F')
            throw new InstallException(Reason.BAD_ELF, "not an ELF file");
        if (h[4] != 2)
            throw new InstallException(Reason.BAD_ELF, "not 64-bit");
        if (h[5] != 1)
            throw new InstallException(Reason.BAD_ELF, "not little-endian");
        int type = (h[16] & 0xff) | (h[17] & 0xff) << 8, machine = (h[18] & 0xff) | (h[19] & 0xff) << 8;
        if (type != ET_DYN)
            throw new InstallException(Reason.BAD_ELF, "e_type " + type + ", not a shared object");
        if (machine != EM_AARCH64)
            throw new InstallException(Reason.BAD_ELF, "e_machine " + machine + ", not aarch64");
    }

    // ---- What is installed -----------------------------------------------------------------------------------------

    /** The installed drivers, by name. Broken directories are left out. */
    List<Driver> list() {
        List<Driver> out = new ArrayList<>();
        String[] ns = root().list();
        if (ns == null)
            return out;
        Arrays.sort(ns);
        for (String n : ns) {
            Driver d = validId(n) ? read(n) : null;
            if (d != null)
                out.add(d);
        }
        out.sort((a, b) -> a.name.compareToIgnoreCase(b.name));
        return out;
    }

    /** The driver id, or null when it is not installed or its directory is broken. */
    Driver read(String id) {
        if (!validId(id))
            return null;
        File dir = new File(root(), id);
        File mf = new File(dir, META);
        if (!mf.isFile() || mf.length() > MAX_META)
            return null;
        try {
            JSONObject meta;
            try (InputStream in = new FileInputStream(mf)) {
                meta = new JSONObject(new String(readAll(in, MAX_META), StandardCharsets.UTF_8));
            }
            String lib = meta.optString("libraryName");
            if (!LIB.matcher(lib).matches() || !new File(dir, lib).isFile())
                return null;
            JSONObject origin = null;
            File of = new File(dir, ORIGIN);
            if (of.isFile() && of.length() <= MAX_META)
                try (InputStream in = new FileInputStream(of)) {
                    origin = new JSONObject(new String(readAll(in, MAX_META), StandardCharsets.UTF_8));
                }
            return new Driver(id, dir, meta, origin);
        } catch (IOException | JSONException e) {
            return null;
        }
    }

    /** Whether a driver with this zip SHA-256 (VkDriverIndex: an asset's digest, « sha256:… » or bare) is installed. */
    boolean installedSha256(String sha256) {
        if (sha256 == null)
            return false;
        String want = stripDigest(sha256);
        for (Driver d : list())
            if (d.sha256 != null && d.sha256.equalsIgnoreCase(want))
                return true;
        return false;
    }

    /**
     * Deletes a driver; the global choice goes back to the system driver when it was the active one, and every game
     * that had chosen it follows the global choice again.
     */
    boolean delete(String id) {
        if (!validId(id))
            return false;
        synchronized (LOCK) {
            if (id.equals(activeId()))
                clearActive();
            resetGameChoices(id);
            if (id.equals(trialMarker()))
                new File(root(), TRIAL_MARKER).delete();
            new File(root(), id + STATUS_SUFFIX).delete();
            File dir = new File(root(), id);
            if (!dir.exists())
                return false;
            File gone = new File(root(), ".del-" + UUID.randomUUID());
            File target = dir.renameTo(gone) ? gone : dir;
            deleteTree(target);
            return !dir.exists();
        }
    }

    // ---- The active driver -----------------------------------------------------------------------------------------

    private File settingFile() {
        return new File(new File(filesDir, "settings"), SETTING);
    }

    /** The id the setting names (maybe no longer installed), or null for the system driver. */
    String activeId() {
        File f = settingFile();
        if (!f.isFile())
            return null;
        try (InputStream in = new FileInputStream(f)) {
            String s = new String(readAll(in, 256), StandardCharsets.UTF_8).trim();
            return validId(s) ? s : null;
        } catch (IOException e) {
            return null;
        }
    }

    /** The active driver, or null for the system driver (none chosen, or the chosen one is gone or broken). */
    Driver active() {
        String id = activeId();
        return id != null ? read(id) : null;
    }

    /** Makes an installed driver the active one (and forgets a failed trial of it). False when it is not installed. */
    boolean setActive(String id) {
        if (read(id) == null)
            return false;
        new File(root(), TRIAL_MARKER).delete();
        return writeSetting(id);
    }

    /** Back to the system driver. */
    boolean clearActive() {
        new File(root(), TRIAL_MARKER).delete();
        return writeSetting("");
    }

    private boolean writeSetting(String v) {
        return writeLine(settingFile(), v);
    }

    private static boolean writeLine(File f, String v) {
        File tmp = new File(f.getPath() + ".tmp");
        File d = f.getParentFile();
        if (d != null && !(d.mkdirs() || d.isDirectory()))
            return false;
        try (OutputStream o = new FileOutputStream(tmp)) {
            o.write((v + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            return false;
        }
        return tmp.renameTo(f);
    }

    // ---- A game's own choice ---------------------------------------------------------------------------------------

    /** A library key usable in a file name (n64-NZLP-B2055FBD), upper case, or null. */
    static String gameFileKey(String key) {
        if (key == null)
            return null;
        String k = key.trim().toUpperCase(Locale.ROOT);
        return k.isEmpty() || k.length() > 96 || !NAME.matcher(k).matches() || k.startsWith(".") ? null : k;
    }

    private File gameSettingFile(String key) {
        String k = gameFileKey(key);
        return k == null ? null : new File(new File(filesDir, "settings"), GAME_SETTING_PREFIX + k);
    }

    /**
     * A game's own choice: {@link #SYSTEM}, a driver id (maybe no longer installed), or null when it follows the global
     * choice (no file, an empty one, or a line that is neither).
     */
    String gameChoice(String key) {
        File f = gameSettingFile(key);
        if (f == null || !f.isFile())
            return null;
        try (InputStream in = new FileInputStream(f)) {
            String s = new String(readAll(in, 256), StandardCharsets.UTF_8).trim();
            return SYSTEM.equals(s) || validId(s) ? s : null;
        } catch (IOException e) {
            return null;
        }
    }

    /**
     * Sets a game's own choice: null (or empty) to follow the global choice, {@link #SYSTEM}, or an installed driver's
     * id (choosing it again forgets a failed trial of it, as setActive). False when the id is not installed, the key
     * cannot name a file, or the write fails.
     */
    boolean setGameChoice(String key, String choice) {
        File f = gameSettingFile(key);
        if (f == null)
            return false;
        String v = choice == null ? "" : choice.trim();
        if (!v.isEmpty() && !SYSTEM.equals(v)) {
            if (read(v) == null)
                return false;
            if (v.equals(trialMarker()))
                new File(root(), TRIAL_MARKER).delete();
        }
        return writeLine(f, v);
    }

    /**
     * The driver a game starts with: its own choice (system: null; an installed driver: that one), else the global
     * choice ({@link #active}). A game's choice naming a driver no longer installed follows the global one. key null:
     * the global choice.
     */
    Driver resolve(String key) {
        String g = key == null ? null : gameChoice(key);
        if (SYSTEM.equals(g))
            return null;
        if (g != null) {
            Driver d = read(g);
            if (d != null)
                return d;
        }
        return active();
    }

    /** Every game's choice of id goes back to following the global choice (the driver is being deleted). */
    private void resetGameChoices(String id) {
        File[] fs = new File(filesDir, "settings").listFiles();
        if (fs == null)
            return;
        for (File f : fs) {
            String n = f.getName();
            if (!n.startsWith(GAME_SETTING_PREFIX) || n.endsWith(".tmp") || !f.isFile())
                continue;
            if (id.equals(gameChoice(n.substring(GAME_SETTING_PREFIX.length()))))
                f.delete();
        }
    }

    /** The id in the native runtime's trial marker (a start that never reached its shader check), or null. */
    String trialMarker() {
        File f = new File(root(), TRIAL_MARKER);
        if (!f.isFile())
            return null;
        try (InputStream in = new FileInputStream(f)) {
            String s = new String(readAll(in, 256), StandardCharsets.UTF_8).trim();
            return validId(s) ? s : null;
        } catch (IOException e) {
            return null;
        }
    }

    /** Whether this driver failed at the last start (the native runtime skips it until it is chosen again). */
    boolean failedLastStart(String id) {
        return id != null && id.equals(trialMarker());
    }

    /** The native runtime's report of the last start that used a custom driver (files/vk_drivers/last_start). */
    static final String LAST_START = "last_start";

    /**
     * The status the native runtime wrote for this driver's last start (« ok », « unsupported », « open_failed »,
     * « not_loaded », « crashed », or « failed » from a runtime before those two), or null when it has not been used
     * yet. vk_drivers/&lt;id&gt;.status first (one per driver, so a game with its own choice does not hide another
     * driver's), else last_start when its line names this driver (written by a runtime without the per-driver file).
     */
    String lastStartStatus(String id) {
        if (!validId(id))
            return null;
        String own = readWord(new File(root(), id + STATUS_SUFFIX), null);
        return own != null ? own : readWord(new File(root(), LAST_START), id);
    }

    /** The status word of a status file: « &lt;status&gt; » (id null) or « &lt;id&gt; &lt;status&gt; » naming id. */
    static String parseStatus(String text, String id) {
        if (text == null)
            return null;
        String t = text.trim();
        if (t.isEmpty())
            return null;
        String[] w = t.split("\\s+");
        if (id == null)
            return w.length == 1 && STATUS_WORD.matcher(w[0]).matches() ? w[0] : null;
        return w.length == 2 && w[0].equals(id) && STATUS_WORD.matcher(w[1]).matches() ? w[1] : null;
    }

    private static final Pattern STATUS_WORD = Pattern.compile("[a-z_]{1,32}");

    private static String readWord(File f, String id) {
        if (!f.isFile())
            return null;
        try (InputStream in = new FileInputStream(f)) {
            return parseStatus(new String(readAll(in, 256), StandardCharsets.UTF_8), id);
        } catch (IOException e) {
            return null;
        }
    }

    // ---- To the native runtime -------------------------------------------------------------------------------------

    /**
     * The variables for the game process, as ConsoleControls.runEnv: {name, value}, value null = unset. With a driver,
     * all four set; without (system driver, or not an N64 game), all four unset. RECOMP_VK_DRIVER is left alone.
     */
    static List<String[]> runEnv(Driver d, String hookDir) {
        boolean on = d != null && hookDir != null;
        String dir = on ? d.dir.getAbsolutePath() : null;
        if (dir != null && !dir.endsWith("/"))
            dir += "/";
        List<String[]> out = new ArrayList<>();
        out.add(new String[] {ENV_DIR, dir});
        out.add(new String[] {ENV_LIB, on ? d.libraryName : null});
        out.add(new String[] {ENV_HOOK_DIR, on ? hookDir : null});
        out.add(new String[] {ENV_ID, on ? d.id : null});
        return out;
    }

    /**
     * The compiled-shader cache for the game process: {ENV_CACHE_DIR, &lt;cacheDir&gt;/vk_cache/&lt;d.id or
     * « system »&gt;/}, value null (unset) without a cache directory (not an N64 game). One directory per driver: a
     * driver never reads another's files (the runtime also keys the file by the device's pipelineCacheUUID).
     */
    static String[] cacheEnv(Driver d, File cacheDir) {
        if (cacheDir == null)
            return new String[] {ENV_CACHE_DIR, null};
        String id = d != null && ID.matcher(d.id).matches() ? d.id : SYSTEM;
        return new String[] {ENV_CACHE_DIR, new File(new File(cacheDir, CACHE_DIR), id).getAbsolutePath() + "/"};
    }

    // ---- For a game's own app (inheriting XRecomp's choice) --------------------------------------------------------

    /**
     * Writes an installed driver back as a package zip (its files without origin.json, stored), for a game's own app
     * to install with {@link Origin#inherited} and the same id. False when it is not installed.
     */
    boolean exportZip(String id, OutputStream out) throws IOException {
        Driver d = read(id);
        if (d == null)
            return false;
        File[] fs = d.dir.listFiles();
        if (fs == null)
            return false;
        Arrays.sort(fs);
        ZipOutputStream z = new ZipOutputStream(out);
        byte[] buf = new byte[1 << 16];
        for (File f : fs) {
            if (!f.isFile() || f.getName().equals(ORIGIN))
                continue;
            z.putNextEntry(new ZipEntry(f.getName()));
            try (InputStream in = new FileInputStream(f)) {
                for (int r; (r = in.read(buf)) > 0; )
                    z.write(buf, 0, r);
            }
            z.closeEntry();
        }
        z.finish();
        return true;
    }

    // ---- Helpers ---------------------------------------------------------------------------------------------------

    private File newTmp() {
        return new File(root(), ".tmp-" + UUID.randomUUID());
    }

    /** Removes what an install or a delete cut short left behind, but keep (callers hold LOCK). */
    private void cleanLeftovers(File keep) {
        File[] fs = root().listFiles();
        if (fs == null)
            return;
        for (File f : fs)
            if ((f.getName().startsWith(".tmp-") || f.getName().startsWith(".del-")) && !f.equals(keep))
                deleteTree(f);
    }

    static String stripDigest(String s) {
        s = s.trim();
        return s.regionMatches(true, 0, "sha256:", 0, 7) ? s.substring(7) : s;
    }

    static String sha256(File f) throws IOException {
        MessageDigest md;
        try {
            md = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new IOException(e);
        }
        try (InputStream in = new FileInputStream(f)) {
            byte[] buf = new byte[1 << 16];
            for (int r; (r = in.read(buf)) > 0; )
                md.update(buf, 0, r);
        }
        StringBuilder sb = new StringBuilder();
        for (byte b : md.digest())
            sb.append(String.format(Locale.ROOT, "%02x", b));
        return sb.toString();
    }

    /** Copies in to o, counting the bytes actually inflated (never the header's word for it) against MAX_TOTAL. */
    private static void copyCapped(InputStream in, OutputStream o, long[] total) throws IOException {
        byte[] buf = new byte[1 << 16];
        for (int r; (r = in.read(buf)) > 0; ) {
            total[0] += r;
            if (total[0] > MAX_TOTAL)
                throw new InstallException(Reason.TOO_LARGE, "over " + MAX_TOTAL + " bytes uncompressed");
            o.write(buf, 0, r);
        }
    }

    private static byte[] readAll(InputStream in, int max) throws IOException {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        for (int r; (r = in.read(buf)) > 0; ) {
            if (b.size() + r > max)
                throw new InstallException(Reason.BAD_META, "over " + max + " bytes");
            b.write(buf, 0, r);
        }
        return b.toByteArray();
    }

    private static void writeFile(File f, byte[] bytes) throws IOException {
        try (OutputStream o = new FileOutputStream(f)) {
            o.write(bytes);
        }
    }

    static void deleteTree(File f) {
        File[] fs = f.isDirectory() ? f.listFiles() : null;
        if (fs != null)
            for (File c : fs)
                deleteTree(c);
        f.delete();
    }
}
