package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import org.json.JSONObject;
import org.junit.Assume;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.io.File;

/** Pack validity, one test per reason, and the XBE title ID reader. */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class PackTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private static final String TID = "4D530004";

    private File files() {
        return tmp.getRoot();
    }

    private void edit(String key, Object value) throws Exception {
        File json = new File(Pack.packDir(files(), TID), "pack.json");
        JSONObject o = new JSONObject(PrepareProtocol.readText(json, 1 << 16));
        if (value == null)
            o.remove(key);
        else
            o.put(key, value);
        TestSupport.write(json, o.toString());
    }

    @Test
    public void valid() throws Exception {
        TestSupport.writePack(files(), TID, 1234);
        assertEquals(Pack.Check.VALID, Pack.check(files(), TID));
        assertEquals(Pack.Check.VALID, Pack.check(files(), TID));
    }

    @Test
    public void validWithAnotherSpellingOfTheTitleId() throws Exception {
        TestSupport.writePack(files(), TID, 10);
        edit("title_id", "0x4d530004");
        assertEquals(Pack.Check.VALID, Pack.check(files(), TID));
    }

    @Test
    public void drop() throws Exception {
        File dir = TestSupport.writePack(files(), TID, 10);
        Pack.drop(files(), "0x4d530004");
        assertFalse(dir.exists());
        assertEquals(Pack.Check.NO_PACK, Pack.check(files(), TID));
        Pack.drop(files(), TID);                  // nothing there: no error
        Pack.drop(files(), "../..");              // not a title ID: nothing removed
        assertTrue(files().isDirectory());
    }

    @Test
    public void noTitleId() {
        assertEquals(Pack.Check.NO_TITLE_ID, Pack.check(files(), null));
        assertEquals(Pack.Check.NO_TITLE_ID, Pack.check(files(), ""));
    }

    @Test
    public void noPack() {
        assertEquals(Pack.Check.NO_PACK, Pack.check(files(), TID));
    }

    @Test
    public void badJson() throws Exception {
        TestSupport.writePack(files(), TID, 10);
        TestSupport.write(new File(Pack.packDir(files(), TID), "pack.json"), "{ not json");
        assertEquals(Pack.Check.BAD_JSON, Pack.check(files(), TID));
    }

    @Test
    public void badFormat() throws Exception {
        TestSupport.writePack(files(), TID, 10);
        edit("format", 2);
        assertEquals(Pack.Check.BAD_FORMAT, Pack.check(files(), TID));
        edit("format", null);
        assertEquals(Pack.Check.BAD_FORMAT, Pack.check(files(), TID));
    }

    @Test
    public void wrongTitle() throws Exception {
        TestSupport.writePack(files(), TID, 10);
        edit("title_id", "4D530005");
        assertEquals(Pack.Check.WRONG_TITLE, Pack.check(files(), TID));
    }

    @Test
    public void wrongShellAbi() throws Exception {
        TestSupport.writePack(files(), TID, 10);
        edit("shell_abi", 2);
        assertEquals(Pack.Check.WRONG_SHELL_ABI, Pack.check(files(), TID));
    }

    @Test
    public void noLib() throws Exception {
        File dir = TestSupport.writePack(files(), TID, 10);
        assertTrue(new File(dir, "libtitle.so").delete());
        assertEquals(Pack.Check.NO_LIB, Pack.check(files(), TID));
        TestSupport.writePack(files(), TID, 10);
        edit("lib", "../../evil.so");
        assertEquals(Pack.Check.NO_LIB, Pack.check(files(), TID));
    }

    @Test
    public void libSize() throws Exception {
        TestSupport.writePack(files(), TID, 10);
        edit("lib_size", 11);
        assertEquals(Pack.Check.LIB_SIZE, Pack.check(files(), TID));
        edit("lib_size", null);
        assertEquals(Pack.Check.LIB_SIZE, Pack.check(files(), TID));
    }

    @Test
    public void titleIdFromSyntheticXbe() throws Exception {
        File x = tmp.newFile("default.xbe");
        TestSupport.write(x, TestSupport.xbe(0x4D530004, "Test"));
        assertEquals("4D530004", Pack.xbeTitleId(x));
        assertEquals("Test", GameStore.xbeTitle(x));
    }

    @Test
    public void titleIdFromConformanceXbe() {
        // tools/conformance/test.xbe: our synthetic XBE (no game content); tests run from android/app.
        File x = new File("../../tools/conformance/test.xbe");
        Assume.assumeTrue(x.isFile());
        assertEquals("12345678", Pack.xbeTitleId(x));
    }

    @Test
    public void notAnXbe() throws Exception {
        File x = tmp.newFile("x.xbe");
        TestSupport.write(x, "not an xbe at all, but long enough? no".getBytes());
        assertNull(Pack.xbeTitleId(x));
        byte[] b = TestSupport.xbe(1, "x");
        TestSupport.le32(b, 0x118, 0x0FFF0);   // certificate below the base
        TestSupport.write(x, b);
        assertNull(Pack.xbeTitleId(x));
        assertNull(Pack.xbeTitleId(null));
    }

    @Test
    public void titleIdSpellings() {
        assertTrue(Pack.sameTitleId("4D530004", "4d530004"));
        assertTrue(Pack.sameTitleId("0x4D530004", "4D530004"));
        assertFalse(Pack.sameTitleId("4D530004", "4D530005"));
        assertFalse(Pack.sameTitleId("zz", "zz"));
        assertFalse(Pack.sameTitleId("", ""));
    }

    @Test
    public void xbeHashOfThePack() throws Exception {
        assertNull(Pack.xbeSha256(files(), TID));                      // no pack
        assertTrue(Pack.builtFrom(files(), TID, "ab"));                // unknown is not a mismatch
        TestSupport.writePack(files(), TID, 16);
        String h = "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF";
        edit("xbe_sha256", h);
        assertEquals(h.toLowerCase(java.util.Locale.ROOT), Pack.xbeSha256(files(), TID));
        assertEquals(h.toLowerCase(java.util.Locale.ROOT), Pack.xbeSha256(files(), "0x4d530004"));
        assertTrue(Pack.builtFrom(files(), TID, h.toLowerCase(java.util.Locale.ROOT)));
        assertTrue(Pack.builtFrom(files(), TID, h));
        assertFalse("another version of the game", Pack.builtFrom(files(), TID, "ff" + h.substring(2)));
        assertTrue(Pack.builtFrom(files(), TID, null));
        assertTrue(Pack.builtFrom(files(), TID, ""));
        edit("xbe_sha256", null);                                      // an older pack.json
        assertNull(Pack.xbeSha256(files(), TID));
        assertTrue(Pack.builtFrom(files(), TID, h));
        TestSupport.write(new File(Pack.packDir(files(), TID), "pack.json"), "{");
        assertNull(Pack.xbeSha256(files(), TID));
        assertNull(Pack.xbeSha256(files(), "zz"));
    }

    /** pack.json's env_force (title.json's, copied by prepare): read in order, refused entries left out. */
    @Test
    public void envForce() throws Exception {
        assertTrue(Pack.envForce(files(), TID).isEmpty());             // no pack
        assertFalse(Pack.forcesOff(files(), TID, "RECOMP_LSFG"));
        TestSupport.writePack(files(), TID, 16);
        assertTrue(Pack.envForce(files(), TID).isEmpty());             // a pack that forces nothing
        JSONObject env = new JSONObject();
        env.put("RECOMP_LSFG", "0");
        env.put("RECOMP_GPU_FPS_CAP", "60");
        env.put("LD_PRELOAD", "x");                                    // not a RECOMP_* name
        env.put("RECOMP_NUM", 1);                                      // not a string
        edit("env_force", env);
        java.util.Map<String, String> m = Pack.envForce(files(), "0x4d530004");
        assertEquals(2, m.size());
        assertEquals("0", m.get("RECOMP_LSFG"));
        assertEquals("60", m.get("RECOMP_GPU_FPS_CAP"));
        assertTrue(Pack.forcesOff(files(), TID, "RECOMP_LSFG"));
        assertFalse(Pack.forcesOff(files(), TID, "RECOMP_GPU_FPS_CAP"));
        assertFalse(Pack.forcesOff(files(), TID, "RECOMP_GPU"));
        assertEquals(Pack.Check.VALID, Pack.check(files(), TID));      // env_force does not change validity
        env.put("RECOMP_LSFG", "1");
        edit("env_force", env);
        assertFalse(Pack.forcesOff(files(), TID, "RECOMP_LSFG"));
        edit("env_force", "RECOMP_LSFG=0");                            // not an object
        assertTrue(Pack.envForce(files(), TID).isEmpty());
        TestSupport.write(new File(Pack.packDir(files(), TID), "pack.json"), "{");
        assertTrue(Pack.envForce(files(), TID).isEmpty());
        assertTrue(Pack.envForce(files(), "zz").isEmpty());
    }

    /**
     * XBA-158: a GBA game's Prepare goes by its library key (« gba-&lt;KEY&gt; »), and check() takes it: the pack in
     * packs/gba-&lt;KEY&gt; with console gba and that title_key. newlyDiscovered counts the runtime's addresses beyond
     * the ones the pack was made with.
     */
    @Test
    public void gbaPackByLibraryKey() throws Exception {
        File files = files();
        String key = "gba-AXVE-1A2B3C4D";
        assertEquals(Pack.Check.NO_PACK, Pack.check(files, key));
        File dir = new File(new File(files, "packs"), key);
        TestSupport.write(new File(dir, "libtitle.so"), new byte[10]);
        JSONObject o = new JSONObject().put("format", 1).put("console", "gba").put("title_key", "AXVE-1A2B3C4D")
                .put("abi", "arm64-v8a").put("shell_abi", Pack.SHELL_ABI).put("lib", "libtitle.so").put("lib_size", 10)
                .put("rom_sha256", "00").put("entry", "gba_recomp_title_table").put("discovered", 1);
        TestSupport.write(new File(dir, "pack.json"), o.toString());
        assertEquals(Pack.Check.VALID, Pack.check(files, key));
        assertEquals(Pack.Check.VALID, Pack.check(files, Console.GBA, "AXVE-1A2B3C4D"));
        assertEquals(Pack.Check.NO_PACK, Pack.check(files, "gba-AXVE-00000000"));
        TestSupport.write(new File(dir, "pack.json"), o.put("shell_abi", Pack.SHELL_ABI + 1).toString());
        assertEquals(Pack.Check.WRONG_SHELL_ABI, Pack.check(files, key));
        TestSupport.write(new File(dir, "pack.json"), o.put("shell_abi", Pack.SHELL_ABI).toString());

        assertEquals(0, Pack.newlyDiscovered(files, key));                       // no file yet
        File disc = Pack.discoveredFile(files, key);
        assertEquals(new File(new File(new File(files, "work"), key), "discovered.txt"), disc);
        TestSupport.write(disc, "0x08000000\n# by hand\n\n0x08000101\n0x080001F0\n");
        assertEquals(3, Pack.countAddresses(disc));
        assertEquals(2, Pack.newlyDiscovered(files, key));                       // the pack had 1
        TestSupport.write(new File(dir, "pack.json"), o.put("discovered", 3).toString());
        assertEquals(0, Pack.newlyDiscovered(files, key));
        assertEquals(0, Pack.newlyDiscovered(files, "gba-AXVE-00000000"));       // no pack
    }

    @Test
    public void countsALargeN64DiscoveredFileLineByLine() throws Exception {
        File files = tmp.newFolder("files");
        String key = "n64-NZLP-B2055FBD";
        File disc = Pack.discoveredFile(files, key);
        disc.getParentFile().mkdirs();
        // The N64's records carry their code bytes: 20 000 records of ~1.2 KB is past the old 16 MB read cap.
        StringBuilder bytes = new StringBuilder();
        for (int i = 0; i < 1200; i++)
            bytes.append('A');
        try (java.io.Writer w = new java.io.BufferedWriter(new java.io.FileWriter(disc))) {
            w.write("# discovered\n");
            for (int i = 0; i < 20000; i++)
                w.write(String.format(java.util.Locale.ROOT, "%08X %016X %s\n", 0x80000000L + 4L * i, (long)i, bytes));
        }
        assertTrue(disc.length() > (16 << 20));
        assertEquals(20000, Pack.countAddresses(disc));
    }

    @Test
    public void readsTheToolkitDigestOfAConsolePack() throws Exception {
        File files = tmp.newFolder("files");
        String key = "n64-NZLP-B2055FBD";
        assertEquals(null, Pack.toolkitSha256OfKey(files, key));                  // no pack
        File dir = new File(new File(files, "packs"), key);
        TestSupport.write(new File(dir, "pack.json"), new JSONObject().put("console", "n64").toString());
        assertEquals(null, Pack.toolkitSha256OfKey(files, key));                  // a pack that does not say
        String sha = "99A89F952E3AEA1439384C960410BA6DF759B4948967973B749EC2182A04B40F";
        TestSupport.write(new File(dir, "pack.json"),
                          new JSONObject().put("console", "n64").put("toolkit_sha256", sha).toString());
        assertEquals(sha.toLowerCase(java.util.Locale.ROOT), Pack.toolkitSha256OfKey(files, key));
        assertEquals(PackFreshness.Verdict.OLDER_TOOLKIT,
                     PackFreshness.judge(null, null, Pack.toolkitSha256OfKey(files, key),
                                         "6cc36f77a67aa0999511c4e8349eca5a60664b061fbcd9b4159eea3b1e5ed5d0"));
        assertEquals(PackFreshness.Verdict.CURRENT,
                     PackFreshness.judge(null, null, Pack.toolkitSha256OfKey(files, key), sha.toLowerCase()));
    }
}
