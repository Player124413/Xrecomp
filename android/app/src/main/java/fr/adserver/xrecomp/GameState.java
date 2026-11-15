package fr.adserver.xrecomp;

/**
 * A library game's state, for its card's chip and Home's primary action: a pure function of what the launcher knows
 * about it (record, scan item, pack, Prepare status, the running game, the toolchain). No I/O here: the caller
 * gathers Facts, so the JVM tests cover every combination.
 *
 * Chip priority (first that applies): Running; Preparing; Queued; Importing; then, with the game's files present,
 * Prepare failed, Pack invalid, Ready, Needs preparing (without the on-device toolchain, Ready: the shell runs the
 * game its own way); without them, Not imported / Needs copying / No file access (a source is there), else
 * Source gone (an in-place folder that vanished), Data missing (a recomp.cfg folder that vanished), or GONE: neither
 * files nor source, the game leaves the grid (its pack and saves stay visible in Storage).
 *
 * A game of another console than the Xbox (XBA-178): Not playable yet while it is in a library folder (no Prepare, no
 * launch until its console's runtime exists), else GONE. A console that plays its files as they are (the GBA,
 * XBA-157): Running, No file access, Preparing / Queued while its Prepare runs or waits (XBA-158: a pack speeds it up,
 * it is never needed), else Ready while it is in a library folder, else GONE.
 *
 * A Ready game whose Prepare is in this APK also says how prepared it is (prepared(), for the card's label): Prepared,
 * Prepared but out of date (its pack was lifted by an older toolkit than this APK's: PackFreshness OLDER_TOOLKIT), or,
 * for a console that plays without a pack, Not prepared.
 */
final class GameState {
    private GameState() {}

    enum Chip {
        RUNNING, PREPARING, QUEUED, IMPORTING, PREPARE_FAILED, PACK_INVALID, READY, NEEDS_PREPARING, NOT_IMPORTED,
        NEEDS_COPYING, NO_FILE_ACCESS, SOURCE_GONE, DATA_MISSING, GONE, NOT_PLAYABLE_YET
    }

    /** The active source in the library folder. */
    enum Source {
        /** Readable: an image to import, or a folder read in place. */
        PRESENT,
        /** A folder seen through the Storage Access Framework only: it must be copied to be played. */
        NEEDS_COPY,
        /** Listed, but the app lost its access (permission revoked, All files access off). */
        NO_ACCESS,
        /** Not in the library folder (or no library folder). */
        MISSING
    }

    static final class Facts {
        String titleId = "";
        /** The game's console (Console.ofKey of its key). */
        Console console = Console.XBOX;
        /** The record's data_kind (Library.KIND_*), "" without a record. */
        String dataKind = "";
        /** The record's data_dir holds a default.xbe. */
        boolean hasData;
        Source source = Source.MISSING;
        /** The on-device Prepare is in this APK (Toolchain.available). */
        boolean toolchain;
        Pack.Check pack = Pack.Check.NO_PACK;
        /** False when the pack was built from another XBE than the active one (Pack.builtFrom). */
        boolean packMatches = true;
        /** The valid pack was lifted by an older toolkit than this APK's (PackFreshness.Verdict.OLDER_TOOLKIT). */
        boolean packOlderToolkit;
        /** The shell refused the pack (EXTRA_BAD_PACK) and the launcher dropped it. */
        boolean packDropped;
        /** status.json of this title, or null. */
        PrepareProtocol.Status prepare;
        /** status.json was updated recently (a live Prepare writes it every few seconds). */
        boolean prepareFresh;
        /** The ":prepare" process is alive. */
        boolean prepareProcess;
        /** The title ID of the game running now, or null. */
        String runningTid;
        /** The title ID being imported now, or null. */
        String importingTid;
    }

    static Chip chip(Facts f) {
        if (f.console != Console.XBOX) {
            if (f.source == Source.MISSING)
                return Chip.GONE;
            if (!f.console.directLaunch())
                return Chip.NOT_PLAYABLE_YET;
            if (f.titleId.equals(f.runningTid))
                return Chip.RUNNING;
            if (f.source == Source.NO_ACCESS)
                return Chip.NO_FILE_ACCESS;
            // XBA-158: its Prepare shows on the card while it runs or waits; prepared or not, it plays (READY).
            LauncherActivity.Prep p = prep(f.toolchain, f.pack == Pack.Check.VALID, f.prepare, f.prepareFresh,
                                           f.prepareProcess);
            return p == LauncherActivity.Prep.RUNNING ? Chip.PREPARING
                   : p == LauncherActivity.Prep.QUEUED ? Chip.QUEUED : Chip.READY;
        }
        if (f.titleId.equals(f.runningTid))
            return Chip.RUNNING;
        boolean packValid = f.pack == Pack.Check.VALID && f.packMatches;
        LauncherActivity.Prep p = prep(f.toolchain, packValid, f.prepare, f.prepareFresh, f.prepareProcess);
        if (p == LauncherActivity.Prep.RUNNING)
            return Chip.PREPARING;
        if (p == LauncherActivity.Prep.QUEUED)
            return Chip.QUEUED;
        if (f.titleId.equals(f.importingTid))
            return Chip.IMPORTING;
        if (f.hasData) {
            if (!f.toolchain)
                return Chip.READY;
            if (p == LauncherActivity.Prep.FAILED)
                return Chip.PREPARE_FAILED;
            boolean packPresent = f.pack != Pack.Check.NO_PACK && f.pack != Pack.Check.NO_TITLE_ID;
            if (f.packDropped || (packPresent && !packValid))
                return Chip.PACK_INVALID;
            return p == LauncherActivity.Prep.PLAYABLE ? Chip.READY : Chip.NEEDS_PREPARING;
        }
        switch (f.source) {
        case PRESENT:
            return Chip.NOT_IMPORTED;
        case NEEDS_COPY:
            return Chip.NEEDS_COPYING;
        case NO_ACCESS:
            return Chip.NO_FILE_ACCESS;
        default:
            break;
        }
        if (Library.KIND_IN_PLACE.equals(f.dataKind) || Library.KIND_IMAGE.equals(f.dataKind))
            return Chip.SOURCE_GONE;
        if (Library.KIND_CFG.equals(f.dataKind))
            return Chip.DATA_MISSING;
        return Chip.GONE;
    }

    /**
     * How prepared a Ready game is, for its card: NONE when the chip is not READY or without the toolchain (nothing to
     * say: the card shows no state); else PREPARED with a valid pack from this APK's toolkit, OUTDATED with a valid pack
     * from an older one (it plays, Prepare again gets the fixes), NOT_PREPARED for a console game without a valid pack
     * (it plays in the interpreter). A Ready Xbox title always has a valid pack (else NEEDS_PREPARING or PACK_INVALID).
     *
     * « New code found » (Pack.newlyDiscovered) is left to Home: playing finds new code nearly every session, so it would
     * mark most played games, and it is not the app being newer than the pack.
     */
    enum Prepared { NONE, PREPARED, OUTDATED, NOT_PREPARED }

    static Prepared prepared(Facts f, Chip c) {
        if (c != Chip.READY || !f.toolchain)
            return Prepared.NONE;
        boolean valid = f.pack == Pack.Check.VALID && (f.console != Console.XBOX || f.packMatches);
        if (!valid)
            return f.console == Console.XBOX ? Prepared.NONE : Prepared.NOT_PREPARED;
        return f.packOlderToolkit ? Prepared.OUTDATED : Prepared.PREPARED;
    }

    /** The game can start now (the launcher may still have to end another running game first). */
    static boolean playable(Chip c) {
        return c == Chip.READY || c == Chip.RUNNING;
    }

    /** The game shows in the library grid. */
    static boolean inGrid(Chip c) {
        return c != Chip.GONE;
    }

    /**
     * The Prepare state of a title whose files are present (LauncherActivity.prep, plus the queue): OFF without the
     * toolchain; RUNNING while a live Prepare reports; QUEUED while it waits behind another (a « queued » status with
     * no ":prepare" process was cut off: INTERRUPTED); PLAYABLE with a valid pack built from the active XBE; else from
     * the last outcome.
     */
    static LauncherActivity.Prep prep(boolean toolchain, boolean packValid, PrepareProtocol.Status s,
                                      boolean statusFresh, boolean prepareProcess) {
        if (!toolchain)
            return LauncherActivity.Prep.OFF;
        if (s != null && s.running() && (statusFresh || prepareProcess))
            return LauncherActivity.Prep.RUNNING;
        if (s != null && s.queued())
            return prepareProcess ? LauncherActivity.Prep.QUEUED : LauncherActivity.Prep.INTERRUPTED;
        if (packValid)
            return LauncherActivity.Prep.PLAYABLE;
        if (s == null)
            return LauncherActivity.Prep.NEEDED;
        switch (s.state) {
        case PrepareProtocol.FAILED:
            return LauncherActivity.Prep.FAILED;
        case PrepareProtocol.CANCELLED:
            return LauncherActivity.Prep.CANCELLED;
        case PrepareProtocol.RUNNING:
            return LauncherActivity.Prep.INTERRUPTED;
        default:
            return LauncherActivity.Prep.NEEDED;
        }
    }
}
