package fr.adserver.xrecomp;

/**
 * The in-game menu's native side (android/app/src/main/cpp/menu_jni.c, in libmain.so: the game's process only, loaded
 * by SDLActivity before ShellActivity uses it).
 */
final class MenuNative {
    private MenuNative() {}

    /**
     * Holds (true) or lets go of (false) the game: the lifecycle's MENU reason (recomp_lifecycle.h), the same hold as
     * the background: the flip parks, the title's threads stop behind it, the audio device pauses.
     */
    static native void nativeHold(boolean on);

    /** True while the MENU reason holds the game. */
    static native boolean nativeHeld();

    /** The output volume, 0..100 (apu_volume.h); 0 mutes. */
    static native void nativeSetVolume(int percent);

    /** Lets the game go, syncs the saves to disk, then recomp_quit(0). Does not return. */
    static native void nativeQuit();

    /**
     * XBA-166: the PlayStation game being played gets another disc (ps1_runtime_change_disc: the lid opens, a second
     * later the disc is in and the lid closes). path: a file path or « fd:N » (a detached descriptor, left open).
     * False when no PS1 game runs.
     */
    static native boolean nativePs1ChangeDisc(String path);

    /** XBA-182 / XBA-183: true while a GBA, PS1 or N64 game that supports save states runs (with a save folder). */
    static native boolean nativeStatesAvailable();

    /**
     * XBA-182: saves slot 1..4 (or 0, the automatic slot) between frames. 0 ok, or a negative code: -1 no console game, -2 empty slot, -3 another
     * game's state, -4 saved by another version, -5 the file cannot be written or read, -6 bad slot, -7 not a state
     * file or damaged.
     */
    static native int nativeStateSave(int slot);

    /** XBA-182: loads slot 1..4 between frames. 0 ok, or a negative code (nativeStateSave). */
    static native int nativeStateLoad(int slot);

    /** XBA-182: when slot 1..4 was saved, Unix ms; 0 when empty, unreadable, another game's or another version's. */
    static native long nativeStateTime(int slot);

    /** XBA-182: the slot's thumbnail, { w, h, ARGB pixels... } (w * h after the two), at most 160 x 120; or null. */
    static native int[] nativeStateThumb(int slot);

    /**
     * The last quick save / load (the pad chords Back + RB / Back + LB) or resume (the automatic slot, slot 0, loaded
     * at the start): { seq, kind (1 saved, 2 loaded, 3 resumed), slot, code }; seq 0 when none yet.
     */
    static native int[] nativeStateEvent();

    /** XBA-182: fast-forward on or off (the menu's toggle). */
    static native void nativeFastForward(boolean on);

    /** XBA-182: true while fast-forward is on. */
    static native boolean nativeFastForwardOn();

    /**
     * The picture changed in the menu, shown at once: a GBA / PS1 / N64 game's scale ("aspect" / "integer" /
     * "stretch"), filter ("linear" / "nearest") and shader (host_window_request_picture: from the next frame), an Xbox
     * title's shader (xbox_FramebufferWindowSetShader: the window draws the shown frame again). shader: a preset name
     * or "none"; strength -1 (the preset's own) or 0..100, curvature and ghosting 0..100. Scale and filter are
     * ignored for an Xbox title.
     */
    static native void nativeSetPicture(boolean xbox, String scale, String filter, String shader, int strength,
                                        int curvature, int ghosting);
}
