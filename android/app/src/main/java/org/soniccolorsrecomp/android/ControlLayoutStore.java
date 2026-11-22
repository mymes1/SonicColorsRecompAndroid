package org.soniccolorsrecomp.android;

import android.content.Context;
import android.content.SharedPreferences;

import java.util.Map;

/** Persistence for the touch control layout (positions as fractions of the view). */
final class ControlLayoutStore {
    private static final String PREFS = "touch_controls";
    private final SharedPreferences prefs;

    ControlLayoutStore(Context context) {
        prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    int getMode() { return prefs.getInt("mode", GameInputView.MODE_STICK); }
    void putMode(int mode) { prefs.edit().putInt("mode", mode).apply(); }

    float getFloat(String key, float fallback) { return prefs.getFloat(key, fallback); }
    void putFloat(String key, float value) { prefs.edit().putFloat(key, value).apply(); }

    boolean getBoolean(String key, boolean fallback) { return prefs.getBoolean(key, fallback); }
    void putBoolean(String key, boolean value) { prefs.edit().putBoolean(key, value).apply(); }

    /** No-op hook kept for the load path: defaults live in GameInputView.resetLayout(). */
    void load(Map<String, GameInputView.Control> controls) {}
}
