package fr.adserver.xrecomp;

import android.content.Context;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * The picture of a GBA, PS1 or N64 game (XBA-181): its scale on the screen (RECOMP_SCALE: aspect, integer, stretch)
 * and its filter (RECOMP_FILTER: nearest, linear), read by every console runtime (RECOMP_GBA_SCALE and the other
 * per-console variables still win there); for a PS1 or N64 game, its internal resolution (XBA-184,
 * RECOMP_UPSCALE: 1, 2, 4 or 8 times the console's; 8 is the N64 Vulkan renderer's alone, the PS1 stops at 4); and
 * frame generation on or off (XBA-186, RECOMP_LSFG: offered only by an APK built with it, R.bool.xba_lsfg; its
 * multiplier, performance set and shader cache stay Settings', {@link #LSFG_ENV}). Which one applies, first found
 * wins:
 * <ol>
 *   <li>the game's own choice (its Home › Game settings, or the in-game menu), in the game's own settings
 *       ({@link GameSettings}, XBA-188: the RECOMP_* keys themselves; files scale_, filter_, upscale_, framegen_
 *       + KEY before), as {@link ConsoleControls} keeps the game's mapping;</li>
 *   <li>Settings › Graphics, a line in recomp.cfg;</li>
 *   <li>the runtime's default (scale: aspect; filter: the console's own, GBA nearest, PS1 and N64 linear).</li>
 * </ol>
 * {@link ShellActivity} puts the result in the environment before the runtime starts, with the settings shared by
 * every console (vertical sync, audio buffer, rumble; {@link #SHARED_ENV}), and unsets what has no value, so nothing
 * a previous game of the same process chose stays. An Xbox game gets neither (its runtime ignores them).
 */
final class ConsoleVideo {
    static final String ENV_SCALE = "RECOMP_SCALE";
    static final String ENV_FILTER = "RECOMP_FILTER";
    static final String ENV_UPSCALE = "RECOMP_UPSCALE";
    static final String SCALE_ASPECT = "aspect", SCALE_INTEGER = "integer", SCALE_STRETCH = "stretch";
    static final String FILTER_NEAREST = "nearest", FILTER_LINEAR = "linear";
    static final String UPSCALE_NATIVE = "1", UPSCALE_2X = "2", UPSCALE_4X = "4", UPSCALE_8X = "8";
    /**
     * XBA-195: the picture shader (src/video/present_shaders.c present_shader_env; none when unset), its strength,
     * the CRT's curvature and the LCD grid's ghosting (0..100). Settings' « All consoles » rows and any game's own
     * (GameSettings), exported by ShellActivity like every option; since XBA-203 an Xbox game reads them too
     * (fb_present_sdl.c: no LCD grid there, so no ghosting row for an Xbox game).
     */
    static final String ENV_SHADER = "RECOMP_SHADER";
    static final String ENV_SHADER_STRENGTH = "RECOMP_SHADER_STRENGTH";
    static final String ENV_SHADER_CURVATURE = "RECOMP_SHADER_CURVATURE";
    static final String ENV_SHADER_GHOSTING = "RECOMP_SHADER_GHOSTING";
    static final String SHADER_SHARP = "sharp", SHADER_SCANLINES = "scanlines", SHADER_CRT = "crt", SHADER_LCD = "lcd";
    /** XBA-186: frame generation for a console game (src/video/host_window.c, docs/technical/framegen.md). */
    static final String ENV_LSFG = "RECOMP_LSFG";
    static final String LSFG_ON = "1", LSFG_OFF = "0";
    /** Frame generation's other settings, Settings' (recomp.cfg's lines) for every game, exported for a console game. */
    static final String[] LSFG_ENV = { "RECOMP_LSFG_MULT", "RECOMP_LSFG_PERF", RecompConfig.LSFG_DIR_KEY };

    /** Settings shared with the Xbox that the console runtimes read too (recomp.cfg's line, exported as it is). */
    static final String[] SHARED_ENV = { "RECOMP_PRESENT_VSYNC", "RECOMP_AUDIO_BUF_SAMPLES", "RECOMP_RUMBLE" };

    enum Setting {
        SCALE(ENV_SCALE, "scale_", new String[] { SCALE_ASPECT, SCALE_INTEGER, SCALE_STRETCH },
              new int[] { R.string.VAL_CSCALE_ASPECT, R.string.VAL_CSCALE_INTEGER, R.string.VAL_CSCALE_STRETCH }),
        FILTER(ENV_FILTER, "filter_", new String[] { FILTER_NEAREST, FILTER_LINEAR },
               new int[] { R.string.VAL_CFILTER_NEAREST, R.string.VAL_CFILTER_LINEAR }),
        /** XBA-184: PS1 and N64 games only ({@link #applies}). */
        UPSCALE(ENV_UPSCALE, "upscale_", new String[] { UPSCALE_NATIVE, UPSCALE_2X, UPSCALE_4X, UPSCALE_8X },
                new int[] { R.string.VAL_UPSCALE_NATIVE, R.string.VAL_UPSCALE_2X, R.string.VAL_UPSCALE_4X,
                            R.string.VAL_UPSCALE_8X }),
        /** XBA-186: every console game, in an APK with frame generation (the caller checks R.bool.xba_lsfg). */
        FRAMEGEN(ENV_LSFG, "framegen_", new String[] { LSFG_ON, LSFG_OFF },
                 new int[] { R.string.VAL_ON, R.string.VAL_OFF });

        final String env;
        final String filePrefix;
        /** The values the runtime takes, in the order the rows cycle through them. */
        final String[] values;
        final int[] labels;

        Setting(String env, String filePrefix, String[] values, int[] labels) {
            this.env = env;
            this.filePrefix = filePrefix;
            this.values = values;
            this.labels = labels;
        }

        /** The index of v in values (case and spaces ignored), or -1. */
        int indexOf(String v) {
            if (v == null)
                return -1;
            String t = v.trim().toLowerCase(Locale.ROOT);
            for (int i = 0; i < values.length; i++)
                if (values[i].equals(t))
                    return i;
            return -1;
        }
    }

    private ConsoleVideo() {}

    /** Whether a game of console con has setting s: scale and filter every console game, the internal resolution the
     *  PS1 and the N64 (the GBA draws its own 240 x 160, nothing to upscale). */
    static boolean applies(Setting s, Console con) {
        if (con == null || con == Console.XBOX)
            return false;
        return s != Setting.UPSCALE || con == Console.PS1 || con == Console.N64;
    }

    /** The Settings keys a setting writes in the game's file (XBA-188, GameSettings): frame generation's three. */
    static String[] gameKeys(Setting s) {
        return s == Setting.FRAMEGEN ? new String[] { ENV_LSFG, "RECOMP_LSFG_MULT", "RECOMP_LSFG_PERF" }
                                     : new String[] { s.env };
    }

    /**
     * What the game's file holds for value v of s (XBA-188): Settings' choice of the same meaning, « » for the
     * runtime's default (scale aspect, native resolution, frame generation off); frame generation on is Settings'
     * « 2x » (RECOMP_LSFG=1, RECOMP_LSFG_MULT=2).
     */
    static List<String[]> gameValues(Setting s, String v) {
        List<String[]> out = new ArrayList<>();
        switch (s) {
        case SCALE:
            out.add(new String[] { s.env, SCALE_ASPECT.equals(v) ? "" : v });
            break;
        case UPSCALE:
            out.add(new String[] { s.env, UPSCALE_NATIVE.equals(v) ? "" : v });
            break;
        case FRAMEGEN: {
            boolean on = LSFG_ON.equals(v);
            String[] k = gameKeys(s);
            out.add(new String[] { k[0], on ? LSFG_ON : "" });
            out.add(new String[] { k[1], on ? "2" : "" });
            out.add(new String[] { k[2], "" });
            break;
        }
        default:
            out.add(new String[] { s.env, v });
            break;
        }
        return out;
    }

    /** The game's own choice (one of s.values), or null when it follows Settings (GameSettings' file). */
    static String gameChoice(Context c, String key, Setting s) {
        if (key == null || key.isEmpty())
            return null;
        String v = GameSettings.raw(c, key, s.env);
        if (v == null)
            return null;
        if (v.isEmpty())
            return s == Setting.SCALE ? SCALE_ASPECT : s == Setting.UPSCALE ? UPSCALE_NATIVE
                   : s == Setting.FRAMEGEN ? LSFG_OFF : null;
        int i = s.indexOf(v);
        return i < 0 ? null : s.values[i];
    }

    /** Sets the game's own choice; null (or a value the setting does not have) goes back to following Settings. */
    static boolean setGameChoice(Context c, String key, Setting s, String v) {
        if (key == null || key.isEmpty())
            return false;
        int i = s.indexOf(v);
        RecompConfig g = GameSettings.load(c, key);
        if (i < 0) {
            for (String k : gameKeys(s))
                g.set(k, null);
        } else {
            for (String[] kv : gameValues(s, s.values[i]))
                g.set(kv[0], kv[1]);
        }
        return GameSettings.save(g);
    }

    /** The per-game cycle: as in Settings, then each value, then as in Settings again. */
    static String next(Setting s, String cur, int dir) {
        int n = s.values.length + 1, i = s.indexOf(cur) + 1;
        int j = ((i + dir) % n + n) % n;
        return j == 0 ? null : s.values[j - 1];
    }

    /** What goes in the variable: the game's choice, else recomp.cfg's line (trimmed, as written), else null (unset). */
    static String resolve(String gameChoice, String cfg) {
        if (gameChoice != null)
            return gameChoice;
        String t = cfg == null ? null : cfg.trim();
        return t == null || t.isEmpty() ? null : t;
    }

    static List<String[]> runEnv(Console con, String gameScale, String gameFilter, RecompConfig cfg) {
        return runEnv(con, gameScale, gameFilter, null, cfg);
    }

    /**
     * The environment for a game of console con: { variable, value or null to unset }. A console game gets its scale,
     * filter and internal resolution (gameScale / gameFilter / gameUpscale: its own choices, may be null; the
     * resolution's only for a PS1 or N64 game, {@link #applies}) and the shared settings from cfg (may be null); an
     * Xbox game gets the three unset (the shared ones stay recomp.cfg's, which the runtime reads).
     */
    static List<String[]> runEnv(Console con, String gameScale, String gameFilter, String gameUpscale,
                                 RecompConfig cfg) {
        return runEnv(con, gameScale, gameFilter, gameUpscale, null, cfg);
    }

    /**
     * The same with the game's frame generation choice (XBA-186, gameFramegen: "1", "0" or null): a console game gets
     * RECOMP_LSFG (its choice, else Settings') and {@link #LSFG_ENV} from cfg; an Xbox game gets RECOMP_LSFG unset
     * (the runtime reads recomp.cfg's line itself), so a console game's own choice does not stay in the process.
     */
    static List<String[]> runEnv(Console con, String gameScale, String gameFilter, String gameUpscale,
                                 String gameFramegen, RecompConfig cfg) {
        List<String[]> out = new ArrayList<>();
        boolean console = con != null && con != Console.XBOX;
        out.add(new String[] { ENV_SCALE, console ? resolve(gameScale, cfg == null ? null : cfg.get(ENV_SCALE)) : null });
        out.add(new String[] { ENV_FILTER, console ? resolve(gameFilter, cfg == null ? null : cfg.get(ENV_FILTER))
                                                   : null });
        String up = applies(Setting.UPSCALE, con) ? gameUpscale : null;
        out.add(new String[] { ENV_UPSCALE, console ? resolve(up, cfg == null ? null : cfg.get(ENV_UPSCALE)) : null });
        if (console) {
            for (String k : SHARED_ENV)
                out.add(new String[] { k, resolve(null, cfg == null ? null : cfg.get(k)) });
        }
        out.add(new String[] { ENV_LSFG, console ? resolve(gameFramegen, cfg == null ? null : cfg.get(ENV_LSFG))
                                                 : null });
        if (console) {
            for (String k : LSFG_ENV)
                out.add(new String[] { k, resolve(null, cfg == null ? null : cfg.get(k)) });
        }
        return out;
    }

    /** The label of the game's choice: « As in Settings » or the value's. */
    static int label(Setting s, String choice) {
        int i = s.indexOf(choice);
        return i < 0 ? R.string.MENU_VIDEO_FOLLOW : s.labels[i];
    }

    // ---- Live picture (the in-game menu) ----------------------------------------------------------------------------

    /**
     * The keys a running game takes at once (MenuNative.nativeSetPicture, src/video/host_window.c
     * host_window_request_picture and fb_present_sdl.c xbox_FramebufferWindowSetShader): the console window's scale
     * and filter, and the picture shader with its strength, curvature and ghosting. Every other key waits for the game
     * to start again.
     */
    static final String[] LIVE_KEYS = { ENV_SCALE, ENV_FILTER, ENV_SHADER, ENV_SHADER_STRENGTH, ENV_SHADER_CURVATURE,
                                        ENV_SHADER_GHOSTING };

    static boolean isLive(String key) {
        for (String k : LIVE_KEYS)
            if (k.equals(key))
                return true;
        return false;
    }

    /** True when every key of option o is taken live ({@link #LIVE_KEYS}). */
    static boolean isLive(RecompConfig.Option o) {
        for (String k : o.keys)
            if (!isLive(k))
                return false;
        return true;
    }

    /** What the running game's picture is: every value resolved (the game's, else Settings', else the default). */
    static final class Picture {
        /** Scale and filter: null for an Xbox game (its presenter has neither). Shader: a preset name, « none ». */
        final String scale, filter, shader;
        /** Strength -1 (the preset's own), curvature and ghosting 0..100. */
        final int strength, curvature, ghosting;
        final boolean xbox;

        Picture(boolean xbox, String scale, String filter, String shader, int strength, int curvature, int ghosting) {
            this.xbox = xbox;
            this.scale = scale;
            this.filter = filter;
            this.shader = shader;
            this.strength = strength;
            this.curvature = curvature;
            this.ghosting = ghosting;
        }

        @Override
        public boolean equals(Object o) {
            if (!(o instanceof Picture))
                return false;
            Picture p = (Picture)o;
            return xbox == p.xbox && java.util.Objects.equals(scale, p.scale)
                   && java.util.Objects.equals(filter, p.filter) && shader.equals(p.shader)
                   && strength == p.strength && curvature == p.curvature && ghosting == p.ghosting;
        }

        @Override
        public int hashCode() {
            return java.util.Objects.hash(xbox, scale, filter, shader, strength, curvature, ghosting);
        }

        @Override
        public String toString() {
            return (xbox ? "" : "scale " + scale + ", filter " + filter + ", ") + "shader " + shader + " (strength "
                   + strength + ", curvature " + curvature + ", ghosting " + ghosting + ")";
        }
    }

    /**
     * The picture a game of console con plays with, from Settings (global, may be null) and its own settings (game,
     * may be null), as ShellActivity's environment and the runtime's defaults would make it: scale aspect, filter the
     * console's own (GBA nearest, the others linear), shader none, strength the preset's own (-1).
     */
    static Picture livePicture(Console con, RecompConfig global, RecompConfig game) {
        boolean xbox = con == null || con == Console.XBOX;
        String scale = null, filter = null;
        if (!xbox) {
            scale = known(Setting.SCALE, GameSettings.effective(global, game, ENV_SCALE), SCALE_ASPECT);
            filter = known(Setting.FILTER, GameSettings.effective(global, game, ENV_FILTER),
                           con == Console.GBA ? FILTER_NEAREST : FILTER_LINEAR);
        }
        String sh = GameSettings.effective(global, game, ENV_SHADER);
        sh = sh == null ? "none" : sh.trim().toLowerCase(Locale.ROOT);
        if (!sh.equals(SHADER_SHARP) && !sh.equals(SHADER_SCANLINES) && !sh.equals(SHADER_CRT)
            && !sh.equals(SHADER_LCD))
            sh = "none";
        return new Picture(xbox, scale, filter, sh,
                           percent(GameSettings.effective(global, game, ENV_SHADER_STRENGTH), -1),
                           percent(GameSettings.effective(global, game, ENV_SHADER_CURVATURE), 0),
                           xbox ? 0 : percent(GameSettings.effective(global, game, ENV_SHADER_GHOSTING), 0));
    }

    private static String known(Setting s, String v, String def) {
        int i = s.indexOf(v);
        return i < 0 ? def : s.values[i];
    }

    /** present_shaders.c env_percent: a number, clamped to 0..100; def when unset or not a number. */
    private static int percent(String v, int def) {
        if (v == null)
            return def;
        try {
            long n = Long.parseLong(v.trim());
            return (int)Math.max(0, Math.min(100, n));
        } catch (NumberFormatException e) {
            return def;
        }
    }
}
