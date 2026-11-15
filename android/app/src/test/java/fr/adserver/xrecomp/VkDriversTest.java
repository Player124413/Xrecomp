package fr.adserver.xrecomp;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/**
 * Custom Vulkan driver packages (VkDrivers): validation, storage under files/vk_drivers, the active choice and the
 * variables for the N64 runtime. Packages are built here from bytes (a fake ELF header); never a real driver.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class VkDriversTest {
    @Rule public TemporaryFolder tmp = new TemporaryFolder();

    private File files;
    private VkDrivers vk;

    @Before
    public void setUp() throws IOException {
        files = tmp.newFolder("files");
        vk = new VkDrivers(files, 33);   // the Thor: Android 13
    }

    // ---- Package builders ------------------------------------------------------------------------------------------

    /** A minimal ELF64 header: magic, class, data, version, then e_type and e_machine at 16 / 18. */
    static byte[] elf(int cls, int data, int type, int machine, int extra) {
        byte[] b = new byte[64 + extra];
        b[0] = 0x7f;
        b[1] = 'E';
        b[2] = 'L';
        b[3] = 'F';
        b[4] = (byte) cls;
        b[5] = (byte) data;
        b[6] = 1;
        b[16] = (byte) type;
        b[17] = (byte) (type >> 8);
        b[18] = (byte) machine;
        b[19] = (byte) (machine >> 8);
        return b;
    }

    static byte[] goodElf() {
        return elf(2, 1, VkDrivers.ET_DYN, VkDrivers.EM_AARCH64, 1000);
    }

    /** meta.json shaped like purple-turnip T30's. */
    static String meta(String lib, int minApi) {
        return "{\"schemaVersion\":1,\"name\":\"Turnip Test T1\",\"description\":\"test\",\"author\":\"tests\","
               + "\"packageVersion\":\"T1\",\"vendor\":\"Mesa\",\"driverVersion\":\"26.3.0-test\",\"minApi\":" + minApi
               + ",\"libraryName\":\"" + lib + "\"}";
    }

    static byte[] zip(Map<String, byte[]> entries) throws IOException {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        try (ZipOutputStream z = new ZipOutputStream(b)) {
            for (Map.Entry<String, byte[]> e : entries.entrySet()) {
                z.putNextEntry(new ZipEntry(e.getKey()));
                z.write(e.getValue());
                z.closeEntry();
            }
        }
        return b.toByteArray();
    }

    static Map<String, byte[]> good() {
        Map<String, byte[]> m = new LinkedHashMap<>();
        m.put("meta.json", meta("vulkan.test.so", 30).getBytes(StandardCharsets.UTF_8));
        m.put("vulkan.test.so", goodElf());
        return m;
    }

    private File file(byte[] bytes) throws IOException {
        File f = tmp.newFile();
        Files.write(f.toPath(), bytes);
        return f;
    }

    private void refused(byte[] zip, VkDrivers.Origin o, VkDrivers.Reason want) throws IOException {
        try {
            vk.install(file(zip), o);
            fail("installed, expected " + want);
        } catch (VkDrivers.InstallException e) {
            assertEquals(e.getMessage(), want, e.reason);
        }
        assertTrue(vk.list().isEmpty());
        String[] left = vk.root().list();
        if (left != null)
            for (String n : left)
                assertFalse("leftover " + n, n.startsWith(".tmp-") || VkDrivers.validId(n));
    }

    private void refused(Map<String, byte[]> entries, VkDrivers.Reason want) throws IOException {
        refused(zip(entries), VkDrivers.Origin.manual(), want);
    }

    // ---- Install ---------------------------------------------------------------------------------------------------

    @Test
    public void validPackageInstalls() throws IOException {
        byte[] z = zip(good());
        File f = file(z);
        String sha = VkDrivers.sha256(f);
        VkDrivers.Driver d = vk.install(f, VkDrivers.Origin.github("MrPurple666/purple-turnip", "T1", "t.adpkg.zip",
                                                                    "sha256:" + sha, z.length));
        assertEquals(sha.substring(0, 16), d.id);
        assertEquals(new File(files, "vk_drivers/" + d.id), d.dir);
        assertEquals("Turnip Test T1", d.name);
        assertEquals("26.3.0-test", d.driverVersion);
        assertEquals("vulkan.test.so", d.libraryName);
        assertEquals(30, d.minApi);
        assertEquals("github", d.originKind);
        assertEquals("MrPurple666/purple-turnip", d.repo);
        assertEquals("T1", d.tag);
        assertEquals(sha, d.sha256);
        assertEquals(z.length, d.zipSize);
        assertTrue(d.installed > 0);
        assertArrayEquals(goodElf(), Files.readAllBytes(d.library().toPath()));
        assertTrue(new File(d.dir, "meta.json").isFile());
        assertTrue(new File(d.dir, "origin.json").isFile());
        // read-only: check the mode bits, canWrite() is always true for root (CI runs the tests as root)
        assertFalse(Files.getPosixFilePermissions(d.library().toPath())
                .contains(java.nio.file.attribute.PosixFilePermission.OWNER_WRITE));
        assertTrue(d.sizeOnDisk() >= goodElf().length);
        assertEquals(1, vk.list().size());
        assertTrue(vk.installedSha256(sha.toUpperCase()));
        assertTrue(vk.installedSha256("sha256:" + sha));
        assertFalse(vk.installedSha256("00" + sha.substring(2)));
        // No temporary directory left; installing again returns the same driver.
        for (String n : vk.root().list())
            assertFalse(n, n.startsWith(".tmp-"));
        assertEquals(d.id, vk.install(f, VkDrivers.Origin.manual()).id);
        assertEquals(1, vk.list().size());
        assertEquals("github", vk.list().get(0).originKind);   // the first install's origin kept
        assertTrue(f.isFile());                                  // the zip given is left where it was
    }

    @Test
    public void installFromStream() throws IOException {
        byte[] z = zip(good());
        VkDrivers.Driver d = vk.install(new ByteArrayInputStream(z), VkDrivers.Origin.manual());
        assertEquals("manual", d.originKind);
        assertNull(d.repo);
        assertEquals(z.length, d.zipSize);
        for (String n : vk.root().list())
            assertFalse(n, n.startsWith(".tmp-"));
    }

    @Test
    public void leftoversOfACutInstallAreRemoved() throws IOException {
        File stale = new File(vk.root(), ".tmp-old/x");
        assertTrue(stale.getParentFile().mkdirs());
        assertTrue(stale.createNewFile());
        vk.install(file(zip(good())), null);
        assertFalse(stale.getParentFile().exists());
    }

    @Test
    public void refusesPathInAnEntryName() throws IOException {
        for (String bad : new String[] {"../evil.so", "lib/vulkan.test.so", "/abs.so", "..", "a\\b.so",
                                        "origin.json", "sp ace.so"}) {
            Map<String, byte[]> m = good();
            m.put(bad, new byte[] {1});
            refused(m, VkDrivers.Reason.BAD_ENTRY_NAME);
        }
        // A directory entry.
        Map<String, byte[]> m = good();
        m.put("dir/", new byte[0]);
        refused(m, VkDrivers.Reason.BAD_ENTRY_NAME);
        assertFalse(new File(files, "evil.so").exists());
    }

    @Test
    public void refusesMissingMeta() throws IOException {
        Map<String, byte[]> m = good();
        m.remove("meta.json");
        refused(m, VkDrivers.Reason.NO_META);
    }

    @Test
    public void refusesBadMeta() throws IOException {
        String[] bad = {"not json", "{\"schemaVersion\":2,\"name\":\"x\",\"libraryName\":\"vulkan.test.so\"}",
                        "{\"name\":\"x\",\"libraryName\":\"vulkan.test.so\"}",
                        "{\"schemaVersion\":1,\"name\":\" \",\"libraryName\":\"vulkan.test.so\"}",
                        "{\"schemaVersion\":1,\"name\":\"x\",\"libraryName\":\"../vulkan.test.so\"}",
                        "{\"schemaVersion\":1,\"name\":\"x\",\"libraryName\":\"vulkan.test.txt\"}",
                        "{\"schemaVersion\":1,\"name\":\"x\"}"};
        for (String s : bad) {
            Map<String, byte[]> m = good();
            m.put("meta.json", s.getBytes(StandardCharsets.UTF_8));
            refused(m, VkDrivers.Reason.BAD_META);
        }
        Map<String, byte[]> m = good();
        byte[] big = new byte[VkDrivers.MAX_META + 1];
        java.util.Arrays.fill(big, (byte) ' ');
        m.put("meta.json", big);
        refused(m, VkDrivers.Reason.BAD_META);
    }

    @Test
    public void refusesLibraryAbsentFromTheZip() throws IOException {
        Map<String, byte[]> m = good();
        m.put("meta.json", meta("vulkan.other.so", 30).getBytes(StandardCharsets.UTF_8));
        refused(m, VkDrivers.Reason.NO_LIBRARY);
    }

    @Test
    public void refusesWrongElf() throws IOException {
        int[][] bad = {{2, 1, VkDrivers.ET_DYN, 40},     // EM_ARM
                       {2, 1, VkDrivers.ET_DYN, 62},     // x86-64
                       {1, 1, VkDrivers.ET_DYN, 183},    // 32-bit
                       {2, 2, VkDrivers.ET_DYN, 183},    // big-endian
                       {2, 1, 2, 183}};                   // ET_EXEC
        for (int[] e : bad) {
            Map<String, byte[]> m = good();
            m.put("vulkan.test.so", elf(e[0], e[1], e[2], e[3], 10));
            refused(m, VkDrivers.Reason.BAD_ELF);
        }
        Map<String, byte[]> m = good();
        m.put("vulkan.test.so", "#!/bin/sh\necho not a library\n".getBytes(StandardCharsets.UTF_8));
        refused(m, VkDrivers.Reason.BAD_ELF);
        m.put("vulkan.test.so", new byte[] {0x7f, 'E', 'L', 'F'});   // too short
        refused(m, VkDrivers.Reason.BAD_ELF);
    }

    @Test
    public void refusesMinApiTooHigh() throws IOException {
        Map<String, byte[]> m = good();
        m.put("meta.json", meta("vulkan.test.so", 34).getBytes(StandardCharsets.UTF_8));
        refused(m, VkDrivers.Reason.MIN_API);
        // Equal is fine, and so is no minApi at all.
        m.put("meta.json", meta("vulkan.test.so", 33).getBytes(StandardCharsets.UTF_8));
        assertNotNull(vk.install(file(zip(m)), null));
    }

    @Test
    public void refusesTooManyEntries() throws IOException {
        Map<String, byte[]> m = good();
        for (int i = 0; m.size() <= VkDrivers.MAX_ENTRIES; i++)
            m.put("extra" + i + ".txt", new byte[] {1});
        refused(m, VkDrivers.Reason.TOO_MANY_ENTRIES);
    }

    @Test
    public void refusesDuplicateEntries() throws IOException {
        Map<String, byte[]> m = good();
        m.put("META.json", new byte[] {1});   // same name but for case
        refused(m, VkDrivers.Reason.DUPLICATE_ENTRY);
    }

    @Test
    public void refusesOversize() throws IOException {
        // A zip bomb in spirit: deflated zeros, small on disk, inflating past the cap; the cap counts the bytes
        // actually inflated.
        File big = tmp.newFile("big.zip");
        long half = VkDrivers.MAX_TOTAL / 2 + 1;
        try (ZipOutputStream z = new ZipOutputStream(new FileOutputStream(big))) {
            z.putNextEntry(new ZipEntry("meta.json"));
            z.write(meta("vulkan.test.so", 30).getBytes(StandardCharsets.UTF_8));
            z.closeEntry();
            z.putNextEntry(new ZipEntry("vulkan.test.so"));
            z.write(goodElf());
            z.closeEntry();
            byte[] zeros = new byte[1 << 20];
            for (String n : new String[] {"pad1.bin", "pad2.bin"}) {
                z.putNextEntry(new ZipEntry(n));   // deflated zeros: tiny on disk
                for (long w = 0; w < half; w += zeros.length)
                    z.write(zeros, 0, (int) Math.min(zeros.length, half - w));
                z.closeEntry();
            }
        }
        assertTrue(big.length() < (4L << 20));
        refused(Files.readAllBytes(big.toPath()), VkDrivers.Origin.manual(), VkDrivers.Reason.TOO_LARGE);
    }

    @Test
    public void refusesOversizeStream() throws IOException {
        // A package stream over the zip cap is refused while it is copied, and nothing is kept.
        InputStream endless = new InputStream() {
            long n;

            @Override
            public int read() {
                return n++ <= VkDrivers.MAX_ZIP ? 0 : -1;
            }

            @Override
            public int read(byte[] buf, int off, int len) {
                if (n > VkDrivers.MAX_ZIP)
                    return -1;
                n += len;
                return len;
            }
        };
        try {
            vk.install(endless, VkDrivers.Origin.manual());
            fail();
        } catch (VkDrivers.InstallException e) {
            assertEquals(VkDrivers.Reason.TOO_LARGE, e.reason);
        }
        for (String n : vk.root().list())
            assertFalse(n, n.startsWith(".tmp-"));
        assertNotNull(vk.install(new ByteArrayInputStream(zip(good())), null));
    }

    @Test
    public void refusesWrongSha256AndSize() throws IOException {
        byte[] z = zip(good());
        String other = "0000000000000000000000000000000000000000000000000000000000000000";
        refused(z, VkDrivers.Origin.github("o/r", "t", "a.zip", other, -1), VkDrivers.Reason.SHA256_MISMATCH);
        refused(z, VkDrivers.Origin.github("o/r", "t", "a.zip", "sha256:" + other, -1),
                VkDrivers.Reason.SHA256_MISMATCH);
        refused(z, VkDrivers.Origin.github("o/r", "t", "a.zip", "nonsense", -1), VkDrivers.Reason.SHA256_MISMATCH);
        refused(z, VkDrivers.Origin.github("o/r", "t", "a.zip", null, z.length + 1), VkDrivers.Reason.SIZE_MISMATCH);
    }

    @Test
    public void refusesNotAZip() throws IOException {
        refused("hello".getBytes(StandardCharsets.UTF_8), VkDrivers.Origin.manual(), VkDrivers.Reason.NOT_A_ZIP);
    }

    // ---- The active driver, delete ---------------------------------------------------------------------------------

    @Test
    public void activeAndDelete() throws IOException {
        assertNull(vk.active());
        assertNull(vk.activeId());
        VkDrivers.Driver d = vk.install(file(zip(good())), null);
        assertFalse(vk.setActive("0123456789abcdef"));   // not installed
        assertFalse(vk.setActive("../../etc"));
        assertNull(vk.active());
        assertTrue(vk.setActive(d.id));
        assertEquals(d.id, vk.active().id);
        assertEquals(d.id + "\n", new String(Files.readAllBytes(new File(files, "settings/vk_driver").toPath()),
                                             StandardCharsets.UTF_8));
        assertTrue(vk.clearActive());
        assertNull(vk.active());
        assertTrue(vk.setActive(d.id));
        // Deleting the active one goes back to the system driver.
        assertTrue(vk.delete(d.id));
        assertFalse(d.dir.exists());
        assertNull(vk.activeId());
        assertNull(vk.active());
        assertTrue(vk.list().isEmpty());
        assertFalse(vk.delete(d.id));
        assertFalse(vk.delete("../settings"));
    }

    @Test
    public void deletingAnotherDriverKeepsTheChoice() throws IOException {
        VkDrivers.Driver a = vk.install(file(zip(good())), null);
        Map<String, byte[]> m = good();
        m.put("README.txt", "b".getBytes(StandardCharsets.UTF_8));
        VkDrivers.Driver b = vk.install(file(zip(m)), null);
        assertFalse(a.id.equals(b.id));
        assertEquals(2, vk.list().size());
        assertTrue(vk.setActive(a.id));
        assertTrue(vk.delete(b.id));
        assertEquals(a.id, vk.active().id);
    }

    @Test
    public void aBrokenDriverIsNotActive() throws IOException {
        VkDrivers.Driver d = vk.install(file(zip(good())), null);
        assertTrue(vk.setActive(d.id));
        d.dir.setWritable(true);
        assertTrue(d.library().delete());
        assertNull(vk.active());          // gone: the system driver
        assertTrue(vk.list().isEmpty());
        // Installing the same package again replaces the broken directory.
        assertEquals(d.id, vk.install(file(zip(good())), null).id);
        assertEquals(d.id, vk.active().id);
    }

    @Test
    public void trialMarker() throws IOException {
        VkDrivers.Driver d = vk.install(file(zip(good())), null);
        assertTrue(vk.setActive(d.id));
        assertFalse(vk.failedLastStart(d.id));
        Files.write(new File(vk.root(), "trying").toPath(), (d.id + "\n").getBytes(StandardCharsets.UTF_8));
        assertTrue(vk.failedLastStart(d.id));
        assertEquals(d.id, vk.active().id);   // still chosen; the runtime skips it
        assertTrue(vk.setActive(d.id));       // choosing it again forgets the failure
        assertFalse(vk.failedLastStart(d.id));
        assertFalse(new File(vk.root(), "trying").exists());
    }

    @Test
    public void listIgnoresWhatIsNotADriver() throws IOException {
        assertTrue(new File(vk.root(), "index").mkdirs());
        assertTrue(new File(vk.root(), "trying").createNewFile());
        assertTrue(new File(vk.root(), "NOT-AN-ID").mkdirs());
        assertTrue(vk.list().isEmpty());
        vk.install(file(zip(good())), null);
        assertEquals(1, vk.list().size());
        assertTrue(new File(vk.root(), "index").isDirectory());   // VkDriverIndex's cache untouched
    }

    // ---- To the native runtime -------------------------------------------------------------------------------------

    @Test
    public void runEnvSetsOrUnsetsTheFourVariables() throws IOException {
        VkDrivers.Driver d = vk.install(file(zip(good())), null);
        List<String[]> on = VkDrivers.runEnv(d, "/data/app/x/lib/arm64");
        assertEquals(4, on.size());
        Map<String, String> env = new LinkedHashMap<>();
        for (String[] kv : on)
            env.put(kv[0], kv[1]);
        assertEquals(d.dir.getAbsolutePath() + "/", env.get("RECOMP_VK_DRIVER_DIR"));
        assertTrue(env.get("RECOMP_VK_DRIVER_DIR").endsWith("/vk_drivers/" + d.id + "/"));
        assertEquals("vulkan.test.so", env.get("RECOMP_VK_DRIVER_LIB"));
        assertEquals("/data/app/x/lib/arm64", env.get("RECOMP_VK_HOOK_DIR"));
        assertEquals(d.id, env.get("RECOMP_VK_DRIVER_ID"));
        assertFalse(env.containsKey("RECOMP_VK_DRIVER"));   // the system override is recomp.cfg's, never ours
        for (String[] kv : VkDrivers.runEnv(null, "/data/app/x/lib/arm64")) {
            assertTrue(env.containsKey(kv[0]));
            assertNull(kv[0], kv[1]);
        }
    }

    @Test
    public void cacheEnvIsPerDriverUnderTheCacheDir() throws IOException {
        File cache = tmp.newFolder("cache");
        VkDrivers.Driver d = vk.install(file(zip(good())), null);
        String[] custom = VkDrivers.cacheEnv(d, cache);
        assertEquals("RECOMP_VK_CACHE_DIR", custom[0]);
        assertEquals(cache.getAbsolutePath() + "/vk_cache/" + d.id + "/", custom[1]);
        String[] system = VkDrivers.cacheEnv(null, cache);
        assertEquals("RECOMP_VK_CACHE_DIR", system[0]);
        assertEquals(cache.getAbsolutePath() + "/vk_cache/system/", system[1]);
        assertFalse(custom[1].equals(system[1]));
        // not an N64 game: unset, so a previous game's value does not stay in the process
        String[] off = VkDrivers.cacheEnv(d, null);
        assertEquals("RECOMP_VK_CACHE_DIR", off[0]);
        assertNull(off[1]);
        // the runtime creates the directory itself: nothing is made here
        assertFalse(new File(cache, "vk_cache").exists());
        // never one of runEnv's four (those are unset without a custom driver; this one is set for the system one)
        for (String[] kv : VkDrivers.runEnv(d, "/data/app/x/lib/arm64"))
            assertFalse(kv[0].equals("RECOMP_VK_CACHE_DIR"));
    }

    // ---- A game's own app ------------------------------------------------------------------------------------------

    @Test
    public void exportThenInheritKeepsTheId() throws IOException {
        VkDrivers.Driver d = vk.install(file(zip(good())), null);
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        assertTrue(vk.exportZip(d.id, b));
        assertFalse(vk.exportZip("0123456789abcdef", new ByteArrayOutputStream()));
        VkDrivers clone = new VkDrivers(tmp.newFolder("clone-files"), 33);
        VkDrivers.Driver c = clone.install(new ByteArrayInputStream(b.toByteArray()),
                                           VkDrivers.Origin.inherited(d.id, d.repo, d.tag, d.asset));
        assertEquals(d.id, c.id);
        assertEquals("inherited", c.originKind);
        assertEquals(d.name, c.name);
        assertArrayEquals(goodElf(), Files.readAllBytes(c.library().toPath()));
        try {
            clone.install(new ByteArrayInputStream(b.toByteArray()),
                          VkDrivers.Origin.inherited("../x", null, null, null));
            fail();
        } catch (VkDrivers.InstallException e) {
            assertEquals(VkDrivers.Reason.BAD_ID, e.reason);
        }
    }

    @Test
    public void contract() {
        assertEquals("vk_drivers", VkDrivers.DIR);
        assertEquals("trying", VkDrivers.TRIAL_MARKER);
        assertEquals("vk_driver", VkDrivers.SETTING);
        assertEquals("RECOMP_VK_DRIVER", VkDrivers.ENV_FORCE);
        assertTrue(VkDrivers.validId("f65b2d3353fd4aa7"));   // purple-turnip T30's id
        assertFalse(VkDrivers.validId("F65B2D3353FD4AA7"));
        assertFalse(VkDrivers.validId("f65b2d3353fd4aa"));
    }
}
