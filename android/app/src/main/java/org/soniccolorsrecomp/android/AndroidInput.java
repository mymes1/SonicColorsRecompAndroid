package org.soniccolorsrecomp.android;

/**
 * Bridge between the Android UI thread and the runtime's Pad_Read state
 * (runtime/platform/android/input.cpp).
 *
 * Buttons use the tokens the native side expects: the four face buttons, the D-pad,
 * plus/minus (Start and the Wii "-" button), 1/2 (Nunchuk C/Z equivalents) and Home.
 * Everything is a plain int key code so a gamepad's KEYCODE_BUTTON_* values and the
 * touch controls can share one path.
 */
public final class AndroidInput {
    // Buttons (match scrub input.cpp's key table)
    public static final int KEY_A = 1;
    public static final int KEY_B = 2;
    public static final int KEY_X = 3;
    public static final int KEY_Y = 4;
    public static final int KEY_L1 = 5;
    public static final int KEY_R1 = 6;
    public static final int KEY_L2 = 7;
    public static final int KEY_R2 = 8;
    public static final int KEY_UP = 9;
    public static final int KEY_DOWN = 10;
    public static final int KEY_LEFT = 11;
    public static final int KEY_RIGHT = 12;
    public static final int KEY_PLUS = 13;   // Start
    public static final int KEY_MINUS = 14;  // Select
    public static final int KEY_HOME = 15;
    public static final int KEY_VIEW = 16;   // opens the in-game overlay
    public static final int KEY_1 = 17;
    public static final int KEY_2 = 18;
    public static final int KEY_THUMBL = 19;
    public static final int KEY_THUMBR = 20;

    private static String connectedDevice = "";

    private AndroidInput() {}

    // Implemented in native/src/input_bridge.cpp; see the token table there.
    public static native void setButton(int key, boolean down);
    public static native void setStick(float x, float y);
    public static native void setConnected(boolean connected);
    public static native void reset();
    public static native String controllerName();
    public static native void vibrate(int ms);
    /** Names the pad in the log and the controller overlay. */
    static native void setDeviceName(String name);

    static void press(int key) { setButton(key, true); }
    static void release(int key) { setButton(key, false); }

    /** Maps an Android KeyEvent key code to the runtime's token, or 0. */
    public static int fromAndroidKeyCode(int keyCode) {
        switch (keyCode) {
            case android.view.KeyEvent.KEYCODE_BUTTON_A: return KEY_A;
            case android.view.KeyEvent.KEYCODE_BUTTON_B: return KEY_B;
            case android.view.KeyEvent.KEYCODE_BUTTON_X: return KEY_X;
            case android.view.KeyEvent.KEYCODE_BUTTON_Y: return KEY_Y;
            case android.view.KeyEvent.KEYCODE_BUTTON_L1: return KEY_L1;
            case android.view.KeyEvent.KEYCODE_BUTTON_R1: return KEY_R1;
            case android.view.KeyEvent.KEYCODE_BUTTON_L2: return KEY_L2;
            case android.view.KeyEvent.KEYCODE_BUTTON_R2: return KEY_R2;
            case android.view.KeyEvent.KEYCODE_BUTTON_START: return KEY_PLUS;
            case android.view.KeyEvent.KEYCODE_BUTTON_SELECT: return KEY_MINUS;
            case android.view.KeyEvent.KEYCODE_BUTTON_MODE: return KEY_HOME;
            case android.view.KeyEvent.KEYCODE_BUTTON_THUMBL: return KEY_THUMBL;
            case android.view.KeyEvent.KEYCODE_BUTTON_THUMBR: return KEY_THUMBR;
            case android.view.KeyEvent.KEYCODE_DPAD_UP: return KEY_UP;
            case android.view.KeyEvent.KEYCODE_DPAD_DOWN: return KEY_DOWN;
            case android.view.KeyEvent.KEYCODE_DPAD_LEFT: return KEY_LEFT;
            case android.view.KeyEvent.KEYCODE_DPAD_RIGHT: return KEY_RIGHT;
            // Keyboard-ish fallbacks so a USB keyboard also plays.
            case android.view.KeyEvent.KEYCODE_SPACE: return KEY_A;
            case android.view.KeyEvent.KEYCODE_J: return KEY_B;
            case android.view.KeyEvent.KEYCODE_ENTER: return KEY_PLUS;
            case android.view.KeyEvent.KEYCODE_ESCAPE: return KEY_VIEW;
            default: return 0;
        }
    }

    static void setDeviceNameFromActivity(String name) {
        connectedDevice = name == null ? "" : name;
        setDeviceName(connectedDevice);
    }

    /** Human-readable name of the pad the Activity last saw ("" when only touch is used). */
    static String lastDeviceName() { return connectedDevice; }
}
