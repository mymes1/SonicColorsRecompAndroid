package org.soniccolorsrecomp.android;

/** Build/device smoke test only; this is not a game runtime. */
final class NativeProbe {
    static { System.loadLibrary("scr_android_probe"); }
    static native boolean checkCpuPrimitives();
    private NativeProbe() { }
}
