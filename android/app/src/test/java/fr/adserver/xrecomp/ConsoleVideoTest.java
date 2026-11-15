package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.content.Context;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

/**
 * XBA-181: the shared console options. A GBA, PS1 or N64 game's own scale and filter (scale_&lt;KEY&gt;,
 * filter_&lt;KEY&gt;), the environment ShellActivity hands to the runtime (the game's choice, else recomp.cfg's line,
 * else unset; the shared vertical sync, audio buffer and rumble), Settings' « All consoles » / « Xbox only » grouping,
 * and the running game's title ID (a console game after an Xbox game is not the Xbox game).
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class ConsoleVideoTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private static final String GBA = "gba-AXVE-1A2B3C4D";

    @Test
    public void contractNames() {
        assertEquals("RECOMP_SCALE", ConsoleVideo.ENV_SCALE);
        assertEquals("RECOMP_FILTER", ConsoleVideo.ENV_FILTER);
        assertEquals(List.of("aspect", "integer", "stretch"), List.of(ConsoleVideo.Setting.SCALE.values));
        assertEquals(List.of("nearest", "linear"), List.of(ConsoleVideo.Setting.FILTER.values));
        assertEquals("RECOMP_UPSCALE", ConsoleVideo.ENV_UPSCALE);
        assertEquals(List.of("1", "2", "4", "8"), List.of(ConsoleVideo.Setting.UPSCALE.values));
        assertEquals("upscale_", ConsoleVideo.Setting.UPSCALE.filePrefix);
        assertEquals(List.of("RECOMP_PRESENT_VSYNC", "RECOMP_AUDIO_BUF_SAMPLES", "RECOMP_RUMBLE"),
                     List.of(ConsoleVideo.SHARED_ENV));
        assertEquals("RECOMP_RUMBLE", RecompConfig.ENV_RUMBLE);
    }

    @Test
    public void choiceIsPerGame() {
        Context c = RuntimeEnvironment.getApplication();
        ConsoleVideo.Setting s = ConsoleVideo.Setting.SCALE, f = ConsoleVideo.Setting.FILTER;
        assertNull(ConsoleVideo.gameChoice(c, GBA, s));
        assertTrue(ConsoleVideo.setGameChoice(c, GBA.toLowerCase(), s, "Integer"));
        assertEquals("integer", ConsoleVideo.gameChoice(c, GBA, s));
        assertTrue(GameSettings.file(c.getFilesDir(), GBA).isFile());   // XBA-188: the game's file
        assertNull("the filter untouched", ConsoleVideo.gameChoice(c, GBA, f));
        assertNull("another game", ConsoleVideo.gameChoice(c, "gba-BPEE-00000000", s));
        assertTrue(ConsoleVideo.setGameChoice(c, GBA, f, "linear"));
        assertEquals("linear", ConsoleVideo.gameChoice(c, GBA, f));
        assertTrue(ConsoleVideo.setGameChoice(c, GBA, f, "bilinear"));   // not a value: follows Settings
        assertNull(ConsoleVideo.gameChoice(c, GBA, f));
        assertTrue(ConsoleVideo.setGameChoice(c, GBA, s, null));
        assertNull(ConsoleVideo.gameChoice(c, GBA, s));
        assertNull(ConsoleVideo.gameChoice(c, null, s));
        assertFalse(ConsoleVideo.setGameChoice(c, "", s, "aspect"));
    }

    @Test
    public void cycle() {
        ConsoleVideo.Setting s = ConsoleVideo.Setting.SCALE;
        assertEquals("aspect", ConsoleVideo.next(s, null, 1));
        assertEquals("integer", ConsoleVideo.next(s, "aspect", 1));
        assertEquals("stretch", ConsoleVideo.next(s, "integer", 1));
        assertNull(ConsoleVideo.next(s, "stretch", 1));
        assertEquals("stretch", ConsoleVideo.next(s, null, -1));
        assertEquals("linear", ConsoleVideo.next(ConsoleVideo.Setting.FILTER, null, -1));
        assertEquals(R.string.MENU_VIDEO_FOLLOW, ConsoleVideo.label(s, null));
        assertEquals(R.string.VAL_CSCALE_STRETCH, ConsoleVideo.label(s, "stretch"));
        assertEquals(R.string.VAL_CFILTER_NEAREST, ConsoleVideo.label(ConsoleVideo.Setting.FILTER, "nearest"));
    }

    private static String value(List<String[]> env, String key) {
        for (String[] kv : env)
            if (kv[0].equals(key))
                return kv[1];
        throw new AssertionError(key + " not in the environment");
    }

    private static List<String> keys(List<String[]> env) {
        List<String> out = new ArrayList<>();
        for (String[] kv : env)
            out.add(kv[0]);
        return out;
    }

    @Test
    public void runEnvGameThenSettingsThenUnset() throws Exception {
        File dir = tmp.newFolder("ext");
        RecompConfig cfg = RecompConfig.load(dir);
        // Nothing anywhere: everything unset (the runtime's defaults), and an unset value clears a previous game's.
        List<String[]> env = ConsoleVideo.runEnv(Console.PS1, null, null, cfg);
        assertEquals(List.of("RECOMP_SCALE", "RECOMP_FILTER", "RECOMP_UPSCALE", "RECOMP_PRESENT_VSYNC",
                             "RECOMP_AUDIO_BUF_SAMPLES", "RECOMP_RUMBLE", "RECOMP_LSFG", "RECOMP_LSFG_MULT",
                             "RECOMP_LSFG_PERF", "RECOMP_LSFG_DIR"), keys(env));
        for (String[] kv : env)
            assertNull(kv[0], kv[1]);
        // Settings' lines.
        cfg.set("RECOMP_SCALE", "stretch");
        cfg.set("RECOMP_FILTER", " nearest ");
        cfg.set("RECOMP_PRESENT_VSYNC", "0");
        cfg.set("RECOMP_AUDIO_BUF_SAMPLES", "2048");
        cfg.set("RECOMP_RUMBLE", "0");
        env = ConsoleVideo.runEnv(Console.N64, null, null, cfg);
        assertEquals("stretch", value(env, "RECOMP_SCALE"));
        assertEquals("nearest", value(env, "RECOMP_FILTER"));
        assertEquals("0", value(env, "RECOMP_PRESENT_VSYNC"));
        assertEquals("2048", value(env, "RECOMP_AUDIO_BUF_SAMPLES"));
        assertEquals("0", value(env, "RECOMP_RUMBLE"));
        // The game's own choices win.
        env = ConsoleVideo.runEnv(Console.GBA, "integer", "linear", cfg);
        assertEquals("integer", value(env, "RECOMP_SCALE"));
        assertEquals("linear", value(env, "RECOMP_FILTER"));
        assertEquals("0", value(env, "RECOMP_PRESENT_VSYNC"));
        // An Xbox game: scale, filter, internal resolution and frame generation unset, the shared ones left to
        // recomp.cfg (which the Xbox runtime reads itself).
        env = ConsoleVideo.runEnv(Console.XBOX, "integer", "linear", "4", cfg);
        assertEquals(List.of("RECOMP_SCALE", "RECOMP_FILTER", "RECOMP_UPSCALE", "RECOMP_LSFG"), keys(env));
        assertNull(value(env, "RECOMP_LSFG"));
        assertNull(value(env, "RECOMP_SCALE"));
        assertNull(value(env, "RECOMP_FILTER"));
        assertNull(value(env, "RECOMP_UPSCALE"));
        // No recomp.cfg at all (no external storage).
        env = ConsoleVideo.runEnv(Console.GBA, null, "nearest", null);
        assertNull(value(env, "RECOMP_SCALE"));
        assertEquals("nearest", value(env, "RECOMP_FILTER"));
    }

    /** XBA-186: frame generation for a console game: its own on / off (framegen_&lt;KEY&gt;) over Settings' RECOMP_LSFG;
     *  the multiplier, performance set and shader cache always Settings'; an Xbox game gets RECOMP_LSFG unset. */
    @Test
    public void framegen() throws Exception {
        File dir = tmp.newFolder("ext-fg");
        RecompConfig cfg = RecompConfig.load(dir);
        assertNull(value(ConsoleVideo.runEnv(Console.PS1, null, null, null, null, cfg), "RECOMP_LSFG"));
        cfg.set("RECOMP_LSFG", "1");
        cfg.set("RECOMP_LSFG_MULT", "2");
        cfg.set("RECOMP_LSFG_PERF", "1");
        cfg.set("RECOMP_LSFG_DIR", "/data/x/files/lsfg/abc");
        List<String[]> env = ConsoleVideo.runEnv(Console.N64, null, null, null, null, cfg);
        assertEquals("1", value(env, "RECOMP_LSFG"));
        assertEquals("2", value(env, "RECOMP_LSFG_MULT"));
        assertEquals("1", value(env, "RECOMP_LSFG_PERF"));
        assertEquals("/data/x/files/lsfg/abc", value(env, "RECOMP_LSFG_DIR"));
        // The game's own choice wins; the rest stays Settings'.
        env = ConsoleVideo.runEnv(Console.GBA, null, null, null, "0", cfg);
        assertEquals("0", value(env, "RECOMP_LSFG"));
        assertEquals("2", value(env, "RECOMP_LSFG_MULT"));
        assertNull(value(ConsoleVideo.runEnv(Console.XBOX, null, null, null, "1", cfg), "RECOMP_LSFG"));
        // Stored per game key, cycled « as in Settings » -> On -> Off -> « as in Settings ».
        Context app = RuntimeEnvironment.getApplication();
        String key = "PS1-SCUS94163";
        assertNull(ConsoleVideo.gameChoice(app, key, ConsoleVideo.Setting.FRAMEGEN));
        String v = ConsoleVideo.next(ConsoleVideo.Setting.FRAMEGEN, null, 1);
        assertEquals("1", v);
        assertTrue(ConsoleVideo.setGameChoice(app, key, ConsoleVideo.Setting.FRAMEGEN, v));
        assertEquals("1", ConsoleVideo.gameChoice(app, key, ConsoleVideo.Setting.FRAMEGEN));
        assertEquals("0", ConsoleVideo.next(ConsoleVideo.Setting.FRAMEGEN, "1", 1));
        assertNull(ConsoleVideo.next(ConsoleVideo.Setting.FRAMEGEN, "0", 1));
        assertEquals(R.string.VAL_OFF, ConsoleVideo.label(ConsoleVideo.Setting.FRAMEGEN, "0"));
        assertTrue(ConsoleVideo.applies(ConsoleVideo.Setting.FRAMEGEN, Console.GBA));
        assertFalse(ConsoleVideo.applies(ConsoleVideo.Setting.FRAMEGEN, Console.XBOX));
    }

    /** XBA-184: the internal resolution, RECOMP_UPSCALE: a PS1 or N64 game's own (upscale_&lt;KEY&gt;), else Settings',
     *  else unset (the runtimes' 1x); a GBA game has no own choice (nothing to upscale), Settings' line passes. */
    @Test
    public void upscale() throws Exception {
        Context c = RuntimeEnvironment.getApplication();
        ConsoleVideo.Setting u = ConsoleVideo.Setting.UPSCALE;
        assertTrue(ConsoleVideo.applies(u, Console.PS1));
        assertTrue(ConsoleVideo.applies(u, Console.N64));
        assertFalse(ConsoleVideo.applies(u, Console.GBA));
        assertFalse(ConsoleVideo.applies(u, Console.XBOX));
        assertFalse(ConsoleVideo.applies(u, null));
        assertTrue(ConsoleVideo.applies(ConsoleVideo.Setting.SCALE, Console.GBA));
        String ps1 = "ps1-SLUS-00001", n64 = "n64-NZLP-B2055FBD";
        assertNull(ConsoleVideo.gameChoice(c, ps1, u));
        assertTrue(ConsoleVideo.setGameChoice(c, ps1, u, "4"));
        assertEquals("4", ConsoleVideo.gameChoice(c, ps1, u));
        assertTrue(GameSettings.file(c.getFilesDir(), ps1).isFile());
        assertTrue(ConsoleVideo.setGameChoice(c, ps1, u, "3"));   // not a value: follows Settings
        assertNull(ConsoleVideo.gameChoice(c, ps1, u));
        assertEquals("1", ConsoleVideo.next(u, null, 1));
        assertEquals("8", ConsoleVideo.next(u, "4", 1));
        assertNull(ConsoleVideo.next(u, "8", 1));
        assertEquals("8", ConsoleVideo.next(u, null, -1));
        assertEquals(R.string.VAL_UPSCALE_NATIVE, ConsoleVideo.label(u, "1"));
        assertEquals(R.string.VAL_UPSCALE_8X, ConsoleVideo.label(u, "8"));
        assertEquals(R.string.MENU_VIDEO_FOLLOW, ConsoleVideo.label(u, null));

        RecompConfig cfg = RecompConfig.load(tmp.newFolder("up"));
        assertNull(value(ConsoleVideo.runEnv(Console.PS1, null, null, null, cfg), "RECOMP_UPSCALE"));
        cfg.set("RECOMP_UPSCALE", "2");
        assertEquals("2", value(ConsoleVideo.runEnv(Console.N64, null, null, null, cfg), "RECOMP_UPSCALE"));
        assertEquals("the game's own", "4",
                     value(ConsoleVideo.runEnv(Console.PS1, null, null, "4", cfg), "RECOMP_UPSCALE"));
        assertEquals("8", value(ConsoleVideo.runEnv(Console.N64, null, null, "8", cfg), "RECOMP_UPSCALE"));
        assertEquals("a GBA game: Settings' line, never its own", "2",
                     value(ConsoleVideo.runEnv(Console.GBA, null, null, "4", cfg), "RECOMP_UPSCALE"));
        assertEquals("the 4-argument form: Settings'", "2",
                     value(ConsoleVideo.runEnv(Console.PS1, null, null, cfg), "RECOMP_UPSCALE"));
        assertNull(value(ConsoleVideo.runEnv(Console.PS1, null, null, null, null), "RECOMP_UPSCALE"));
    }

    private static List<Integer> titles(List<RecompConfig.Option> l) {
        List<Integer> out = new ArrayList<>();
        for (RecompConfig.Option o : l)
            out.add(o.title);
        return out;
    }

    /** Settings › Graphics and Audio: the options every console reads first, then the Xbox's own. */
    @Test
    public void settingsGroups() {
        List<RecompConfig.Option> shown = RecompConfig.shownOptions(false);
        List<RecompConfig.Option> g = RecompConfig.tabOptions(shown, RecompConfig.Section.GRAPHICS);
        assertTrue(RecompConfig.grouped(g, RecompConfig.Section.GRAPHICS));
        // XBA-195: the picture shader's four rows after the internal resolution.
        assertEquals(List.of(R.string.OPT_VSYNC, R.string.OPT_CSCALE, R.string.OPT_CFILTER, R.string.OPT_UPSCALE,
                             R.string.OPT_CSHADER, R.string.OPT_CSHADER_STRENGTH, R.string.OPT_CSHADER_CURVE,
                             R.string.OPT_CSHADER_GHOST),
                     titles(g).subList(0, 8));
        for (int i = 8; i < g.size(); i++)
            assertFalse(g.get(i).title + " is the Xbox's", g.get(i).shared);
        List<Integer> xbox = titles(g).subList(8, g.size());
        for (int t : new int[] { R.string.OPT_GPU, R.string.OPT_GPU_FPS_CAP, R.string.OPT_GPU_SCALE,
                                 R.string.OPT_ASPECT, R.string.OPT_WIDESCREEN, R.string.OPT_480P, R.string.OPT_TEX })
            assertTrue(xbox.contains(t));
        List<RecompConfig.Option> a = RecompConfig.tabOptions(shown, RecompConfig.Section.AUDIO);
        assertEquals(List.of(R.string.OPT_AUDIO_BUF, R.string.OPT_HRTF), titles(a));
        assertTrue(RecompConfig.grouped(a, RecompConfig.Section.AUDIO));
        // General and Advanced: as listed, no labels.
        List<RecompConfig.Option> gen = RecompConfig.tabOptions(shown, RecompConfig.Section.GAME);
        assertFalse(RecompConfig.grouped(gen, RecompConfig.Section.GAME));
        assertTrue(titles(gen).contains(R.string.OPT_RUMBLE));
        assertFalse(RecompConfig.grouped(RecompConfig.tabOptions(shown, RecompConfig.Section.ADVANCED),
                                         RecompConfig.Section.ADVANCED));
    }

    /** The Settings rows write the contract's values; the default writes no line. */
    @Test
    public void settingsValues() throws Exception {
        RecompConfig cfg = RecompConfig.load(tmp.newFolder("cfg"));
        for (RecompConfig.Option o : RecompConfig.OPTIONS) {
            if (o.title == R.string.OPT_CSCALE) {
                assertEquals(0, o.defaultIndex());
                cfg.set(o, o.choices[2]);
                assertEquals("stretch", cfg.get("RECOMP_SCALE"));
                cfg.set(o, o.choices[1]);
                assertEquals("integer", cfg.get("RECOMP_SCALE"));
            } else if (o.title == R.string.OPT_CFILTER) {
                cfg.set(o, o.choices[1]);
                assertEquals("nearest", cfg.get("RECOMP_FILTER"));
                cfg.set(o, o.choices[2]);
                assertEquals("linear", cfg.get("RECOMP_FILTER"));
                cfg.set(o, o.choices[0]);
                assertNull("Console default", cfg.get("RECOMP_FILTER"));
            } else if (o.title == R.string.OPT_UPSCALE) {
                assertEquals(0, o.defaultIndex());
                assertEquals(4, o.choices.length);
                cfg.set(o, o.choices[1]);
                assertEquals("2", cfg.get("RECOMP_UPSCALE"));
                cfg.set(o, o.choices[3]);
                assertEquals("8", cfg.get("RECOMP_UPSCALE"));
                cfg.set(o, o.choices[0]);
                assertNull("Native writes no line", cfg.get("RECOMP_UPSCALE"));
            } else if (o.title == R.string.OPT_RUMBLE) {
                cfg.set(o, o.choices[1]);
                assertEquals("0", cfg.get("RECOMP_RUMBLE"));
            }
        }
    }

    /** The bug of XBA-181: during a GBA game launched after an Xbox game, the title ID is the GBA game's key. */
    @Test
    public void runningTitleIdAfterAnXboxGame() throws Exception {
        Context c = RuntimeEnvironment.getApplication();
        File dir = new File(c.getFilesDir(), "xbox-game");
        TestSupport.write(new File(dir, "default.xbe"), TestSupport.xbe(0x4D5300D1, "Game"));
        GameStore.remember(c, dir.getPath(), "Game");
        assertEquals("4D5300D1", GameLanguage.runningTitleId(c));
        assertTrue(GameStore.rememberRomLaunch(c, "/sdcard/roms/quest.gba", "Pocket Quest", GBA));
        assertEquals("the chosen Xbox folder stays", dir.getPath(), GameStore.chosenDir(c));
        assertEquals(GBA, GameLanguage.runningTitleId(c));
        assertEquals("4D5300D1", GameLanguage.currentTitleId(c));
        GameStore.rememberLaunch(c, dir.getPath(), "Game", "4D5300D1");   // an Xbox launch clears the ROM
        assertEquals("4D5300D1", GameLanguage.runningTitleId(c));
    }

    /**
     * The picture the in-game menu sends live: the game's value, else recomp.cfg's, else the runtime's default (scale
     * aspect, the GBA's filter nearest and the others linear, no shader, the preset's own strength); a game's « »
     * is the default even over recomp.cfg; an Xbox title has no scale, filter or ghosting.
     */
    @Test
    public void livePicture() throws Exception {
        RecompConfig cfg = RecompConfig.load(tmp.newFolder("live"));
        ConsoleVideo.Picture p = ConsoleVideo.livePicture(Console.GBA, cfg, null);
        assertEquals("aspect", p.scale);
        assertEquals("nearest", p.filter);
        assertEquals("none", p.shader);
        assertEquals(-1, p.strength);
        assertEquals("linear", ConsoleVideo.livePicture(Console.PS1, cfg, null).filter);
        cfg.set("RECOMP_SCALE", "integer");
        cfg.set("RECOMP_SHADER", "crt");
        cfg.set("RECOMP_SHADER_STRENGTH", "75");
        cfg.set("RECOMP_SHADER_GHOSTING", "40");
        RecompConfig game = RecompConfig.loadFile(new File(tmp.newFolder("g"), "g.cfg"), "#");
        game.set("RECOMP_FILTER", "linear");
        game.set("RECOMP_SCALE", "");   // the game's « default »: aspect, whatever Settings say
        p = ConsoleVideo.livePicture(Console.GBA, cfg, game);
        assertEquals("aspect", p.scale);
        assertEquals("linear", p.filter);
        assertEquals("crt", p.shader);
        assertEquals(75, p.strength);
        assertEquals(40, p.ghosting);
        game.set("RECOMP_SHADER", "bogus");
        assertEquals("none", ConsoleVideo.livePicture(Console.GBA, cfg, game).shader);
        ConsoleVideo.Picture x = ConsoleVideo.livePicture(Console.XBOX, cfg, null);
        assertTrue(x.xbox);
        assertNull(x.scale);
        assertNull(x.filter);
        assertEquals(0, x.ghosting);
        assertEquals("crt", x.shader);
        assertEquals(x, ConsoleVideo.livePicture(Console.XBOX, cfg, null));
        assertFalse(x.equals(p));
        assertTrue(ConsoleVideo.isLive("RECOMP_SHADER"));
        assertFalse(ConsoleVideo.isLive("RECOMP_UPSCALE"));
        assertFalse(ConsoleVideo.isLive("RECOMP_LANGUAGE"));
    }
}
