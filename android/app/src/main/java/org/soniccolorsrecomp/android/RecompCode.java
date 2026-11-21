package org.soniccolorsrecomp.android;

/** Metadata access only; the linked guest code is not safe to execute. */
final class RecompCode {
    static { System.loadLibrary("scr_recomp_code"); }
    static native String gameId();
    private RecompCode() { }
}
