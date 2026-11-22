package org.soniccolorsrecomp.android;

/**
 * Metadata access only; the linked guest code is not safe to execute.
 *
 * This library only exists in a build made with compileGenerated (see BuildConfig.RECOMP_INCLUDED),
 * and only for an ABI it was built for, so its absence is normal and must not throw from a class
 * initializer. gameId() reports "" when it is not there.
 */
final class RecompCode {
    private static final boolean loaded;
    static {
        boolean ok;
        try {
            System.loadLibrary("scr_recomp_code");
            ok = true;
        } catch (UnsatisfiedLinkError e) {
            ok = false;
        }
        loaded = ok;
    }

    static native String gameId();

    static boolean available() { return loaded; }

    private RecompCode() { }
}
