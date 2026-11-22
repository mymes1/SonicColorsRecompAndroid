package org.soniccolorsrecomp.android;

import android.app.Activity;
import android.content.Context;
import android.view.SurfaceView;
import android.view.View;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Owns the native runtime: loads {@code libscr_runtime.so}, runs the installer, then starts
 * the game on a SurfaceView.
 *
 * One library provides the installer and the game, so installing never executes guest
 * code. The runtime pushes progress/status back through the static callbacks below and
 * reads the game data from app storage (chosen with the document picker, copied here
 * because content:// URIs are not seekable files).
 */
public final class NativeRunner {
    private static final String TAG = "SonicColors";
    private static final String LIB = "scr_runtime";

    // Mirrors the C++ side (runtime/platform/android/app.cpp and install_bridge.cpp)
    private static final int STATE_IDLE = 0;
    private static final int STATE_INSTALLING = 1;
    private static final int STATE_INSTALLED = 2;
    private static final int STATE_RUNNING = 3;
    private static final int STATE_FAILED = 4;

    // Progress is one 0..1000 scale for the UI: copying the document into app storage takes
    // the first quarter, the native extraction (which reports 0..100) the rest.
    private static final int COPY_PROGRESS_MAX = 250;

    private static boolean loaded;
    private static String loadError;
    private static volatile int state = STATE_IDLE;
    private static volatile int progress;
    private static volatile String statusText = "";
    private static Context appContext;
    private static volatile boolean exitRequested;

    private final Activity activity;
    private SurfaceView surfaceView;
    private android.view.Surface surface;
    private Thread gameThread;
    private volatile boolean running;

    public NativeRunner(Activity activity) {
        this.activity = activity;
        appContext = activity.getApplicationContext();
        if (load()) nativeSetContext(appContext);
    }

    // ---------------------------------------------------------------- library state
    static synchronized boolean load() {
        if (loaded) return true;
        if (loadError != null) return false;
        try {
            System.loadLibrary(LIB);
            loaded = true;
        } catch (UnsatisfiedLinkError e) {
            loadError = e.getMessage();
            android.util.Log.e(TAG, "cannot load " + LIB, e);
        }
        return loaded;
    }

    /** The APK ships a metadata library even when the runtime was not built. */
    private static void fallbackLoad() {
        try {
            System.loadLibrary("scr_android_probe");
        } catch (UnsatisfiedLinkError ignored) {
        }
    }

    public static boolean isLoaded() { return loaded; }
    public static String loadError() { return loadError; }

    static File filesDir(Context c) { return new File(c.getFilesDir(), "runtime"); }
    static File dataDir(Context c) { return new File(c.getFilesDir(), "game"); }
    static File logFile(Context c) { return new File(filesDir(c), "SonicColorsRecomp.log"); }

    public static boolean dataInstalled() {
        if (appContext == null) return false;
        File boot = new File(new File(dataDir(appContext), "sys"), "boot.bin");
        return boot.isFile() && boot.length() > 0;
    }

    int state() { return state; }
    boolean isInstalling() { return state == STATE_INSTALLING; }
    int progress() { return progress; }
    String statusText() { return statusText; }

    // ---------------------------------------------------------------- install
    void install(android.net.Uri uri) {
        if (!loaded) {
            state = STATE_FAILED;
            statusText = "This build has no native runtime (" + loadError + ")";
            return;
        }
        if (state == STATE_INSTALLING) return;
        state = STATE_INSTALLING;
        progress = 0;
        statusText = "Preparing…";
        final Context ctx = activity;
        Thread installer = new Thread(() -> {
            File stage = new File(filesDir(ctx), "disc.tmp");
            try {
                filesDir(ctx).mkdirs();
                copyToStage(ctx, uri, stage);
                statusText = "Extracting game data…";
                int rc = nativeInstall(stage.getAbsolutePath(), dataDir(ctx).getAbsolutePath(),
                        filesDir(ctx).getAbsolutePath(), keysHint(ctx));
                if (rc == 0) {
                    state = STATE_INSTALLED;
                    progress = 1000;
                    statusText = "Game data installed.";
                } else {
                    state = STATE_FAILED;
                    if (statusText == null || statusText.isEmpty())
                        statusText = installError(rc);
                }
            } catch (IOException | SecurityException e) {
                state = STATE_FAILED;
                statusText = "Cannot read the disc image: " + e.getMessage();
            } finally {
                //noinspection ResultOfMethodCallIgnored
                stage.delete();
            }
        }, "scr-installer");
        installer.start();
    }

    /**
     * Where the player's keys.txt is expected. The native side also looks next to the app's
     * files directory; this is the path shown in the UI so nobody has to guess.
     */
    static java.io.File keysFile(Context c) {
        return new java.io.File(c.getFilesDir(), "keys.txt");
    }

    private static String keysHint(Context c) {
        java.io.File f = keysFile(c);
        return f.isFile() ? f.getAbsolutePath() : "";
    }

    static boolean hasKeys(Context c) {
        return keysFile(c).isFile();
    }

    private static String installError(int rc) {
        switch (rc) {
            case 1: return "The disc image could not be opened. Choose a plain ISO, or a ZIP holding one.";
            case 2: return "This is not Sonic Colors PAL SNCP8P.";
            case 3: return "This release's main.dol is not the one this build was made from.";
            case 4: return "Not enough free space, or the storage is read-only.";
            case 5: return "Install canceled.";
            default: return "Install failed (code " + rc + ").";
        }
    }

    /** Streams the chosen document into app storage: the installer needs a seekable path. */
    private void copyToStage(Context ctx, android.net.Uri uri, File dest) throws IOException {
        try (InputStream in = ctx.getContentResolver().openInputStream(uri)) {
            if (in == null) throw new IOException("cannot open the document");
            long size = 0;
            try (android.database.Cursor c = ctx.getContentResolver()
                    .query(uri, new String[]{android.provider.OpenableColumns.SIZE}, null, null, null)) {
                if (c != null && c.moveToFirst() && !c.isNull(0)) size = c.getLong(0);
            }
            long free = dest.getParentFile() == null ? 0 : dest.getParentFile().getUsableSpace();
            // Extracted data is about 4.3 GiB for PAL; keep room for the image and the result.
            if (free > 0 && free < size + (4L << 30)) {
                throw new IOException("not enough free space (need about " + ((size + (4L << 30)) >> 30)
                        + " GB, have " + (free >> 30) + " GB)");
            }
            try (OutputStream out = new FileOutputStream(dest)) {
                byte[] buf = new byte[1 << 20];
                long total = 0;
                int n;
                while ((n = in.read(buf)) > 0) {
                    if (exitRequested) throw new IOException("canceled");
                    out.write(buf, 0, n);
                    total += n;
                    if (size > 0) progress = (int) Math.min(COPY_PROGRESS_MAX, total * COPY_PROGRESS_MAX / size);
                }
            }
        }
        progress = COPY_PROGRESS_MAX + 10;
        statusText = "Checking the disc header…";
    }

    // ---------------------------------------------------------------- game
    View start(Activity activity) {
        if (!loaded) {
            fallbackLoad();
            return null;
        }
        surfaceView = new SurfaceView(activity);
        surfaceView.getHolder().addCallback(new android.view.SurfaceHolder.Callback() {
            @Override public void surfaceCreated(android.view.SurfaceHolder holder) {
                surface = holder.getSurface();
                if (running) nativeSurfaceCreated(surface);
                else maybeStartGame();
            }
            @Override public void surfaceChanged(android.view.SurfaceHolder holder, int format, int width, int height) {
                surface = holder.getSurface();
                if (running) nativeResize(width, height);
            }
            @Override public void surfaceDestroyed(android.view.SurfaceHolder holder) {
                surface = null;
                if (running) nativeSurfaceDestroyed();
            }
        });
        return surfaceView;
    }

    private void maybeStartGame() {
        if (running || surface == null) return;
        running = true;
        state = STATE_RUNNING;
        final Activity a = activity;
        final android.view.Surface s = surface;
        gameThread = new Thread(() -> {
            setDataDir(dataDir(a).getAbsolutePath());
            nativeStart(filesDir(a).getAbsolutePath(), s);
            running = false;
            if (!exitRequested && state == STATE_RUNNING) {
                state = STATE_FAILED;
                if (statusText == null || statusText.isEmpty())
                    statusText = "The game runtime stopped. Log: " + logFile(a).getAbsolutePath();
            }
            a.runOnUiThread(() -> a.recreate());
        }, "scr-runtime");
        gameThread.start();
    }

    /** Stops an install in progress; the installer returns code 5 when it notices. */
    void cancelInstall() {
        exitRequested = true;
        if (state == STATE_INSTALLING) nativeCancelInstall();
    }

    void onHostPause() { if (running) nativePause(); }
    void onHostResume() { if (running) nativeResume(); }

    void setAutoStart(boolean on) {
        activity.getSharedPreferences("launcher", Context.MODE_PRIVATE).edit().putBoolean("autostart", on).apply();
    }

    void stop() {
        exitRequested = true;
        activity.getSharedPreferences("launcher", Context.MODE_PRIVATE).edit().putBoolean("autostart", false).apply();
        if (running) nativeRequestExit();
    }

    static void requestExit(Activity activity) {
        exitRequested = true;
        try {
            nativeRequestExit();
        } catch (UnsatisfiedLinkError ignored) {
        }
    }

    static boolean isControllerConnected(Context context) {
        for (int id : android.view.InputDevice.getDeviceIds()) {
            android.view.InputDevice d = android.view.InputDevice.getDevice(id);
            if (d == null) continue;
            int src = d.getSources();
            if ((src & android.view.InputDevice.SOURCE_GAMEPAD) != 0
                    || (src & android.view.InputDevice.SOURCE_JOYSTICK) != 0) return true;
        }
        return false;
    }

    static String describeControllers(Context context) {
        StringBuilder b = new StringBuilder();
        int n = 0;
        for (int id : android.view.InputDevice.getDeviceIds()) {
            android.view.InputDevice d = android.view.InputDevice.getDevice(id);
            if (d == null) continue;
            int src = d.getSources();
            if ((src & android.view.InputDevice.SOURCE_GAMEPAD) == 0
                    && (src & android.view.InputDevice.SOURCE_JOYSTICK) == 0) continue;
            if (n++ > 0) b.append('\n');
            b.append("• ").append(d.getName());
        }
        return n == 0 ? "No gamepad detected yet. Bluetooth and USB controllers work; touch controls are always available."
                : b.toString();
    }

    // ---------------------------------------------------------------- JNI callbacks
    /** Installer progress, 0..100, from the native thread. */
    @SuppressWarnings("unused")
    static void onProgress(int percent, String label) {
        progress = COPY_PROGRESS_MAX + Math.max(0, Math.min(100, percent)) * (1000 - COPY_PROGRESS_MAX) / 100;
        if (label != null && !label.isEmpty()) statusText = label;
    }

    @SuppressWarnings("unused")
    static void onStatus(String text) {
        if (text != null) statusText = text;
        android.util.Log.i(TAG, "status: " + text);
    }

    @SuppressWarnings("unused")
    static void onRuntimeExit(String reason) {
        if (reason != null && !reason.isEmpty()) statusText = reason;
        if (state == STATE_RUNNING) state = STATE_FAILED;
    }

    // ---------------------------------------------------------------- native
    static native int nativeInstall(String imagePath, String dataDir, String filesDir, String keysPath);
    static native void nativeCancelInstall();
    static native void nativeStart(String filesDir, android.view.Surface surface);
    static native void nativeResize(int width, int height);
    static native void nativeSurfaceCreated(android.view.Surface surface);
    static native void nativeSurfaceDestroyed();
    static native void nativePause();
    static native void nativeResume();
    static native void nativeRequestExit();
    static native void nativeSetContext(android.content.Context context);

    /** The runtime reads the installed data from here (SCR_ANDROID_DATA). */
    private static void setDataDir(String path) {
        // The native side reads the directory from the environment; setting it here keeps
        // one place that knows the layout.
        try {
            android.system.Os.setenv("SCR_ANDROID_DATA", path, true);
        } catch (Throwable ignored) {
            // API 21+ has Os.setenv; a failure only means the default path is used.
        }
    }
}
