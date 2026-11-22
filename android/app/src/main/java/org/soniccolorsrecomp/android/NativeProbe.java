package org.soniccolorsrecomp.android;

/**
 * Build/device smoke test only; this is not a game runtime.
 *
 * The APK ships arm64-v8a only, so on an x86_64 emulator this library does not exist. Loading is
 * attempted once and its outcome kept, so the instrumentation test can report "this APK has no
 * library for the emulator's ABI" instead of throwing UnsatisfiedLinkError from a class
 * initializer (which would fail the whole test class, not just this test).
 */
final class NativeProbe {
    private static final String loadError;
    static {
        String error = null;
        try {
            System.loadLibrary("scr_android_probe");
        } catch (UnsatisfiedLinkError e) {
            error = e.getMessage();
        }
        loadError = error;
    }

    static native boolean checkCpuPrimitives();

    /** Null when the probe library loaded, else why it did not. */
    static String loadError() { return loadError; }

    static boolean available() { return loadError == null; }

    private NativeProbe() { }
}
