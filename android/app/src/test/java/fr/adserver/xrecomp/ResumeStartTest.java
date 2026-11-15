package fr.adserver.xrecomp;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import android.content.Context;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;

import java.io.File;
import java.io.FileOutputStream;

/** A GBA / PS1 / N64 game's Home: Resume when its automatic slot is saved, Start launches once from the boot. */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class ResumeStartTest {
    private final Context c = RuntimeEnvironment.getApplication();

    @Test
    public void resumeOnlyWithASavedPointAndTheSettingOn() throws Exception {
        String tid = "gba-TEST1234";
        assertFalse(GameStore.canResume(c, tid));
        File st = GameStore.resumeState(c, tid);
        assertTrue(st.getParentFile().mkdirs() || st.getParentFile().isDirectory());
        new FileOutputStream(st).close();
        assertTrue(GameStore.canResume(c, tid));
        GameStore.setAutoResume(c, false);
        assertFalse(GameStore.canResume(c, tid));
        GameStore.setAutoResume(c, true);
        assertTrue(GameStore.canResume(c, tid));
        assertFalse(GameStore.canResume(c, null));
    }

    @Test
    public void freshStartIsOneShot() {
        assertFalse(GameStore.takeFreshStart(c));
        GameStore.requestFreshStart(c);
        assertTrue(GameStore.takeFreshStart(c));
        assertFalse(GameStore.takeFreshStart(c));   // the next process (Restart, another launch) resumes again
    }
}
