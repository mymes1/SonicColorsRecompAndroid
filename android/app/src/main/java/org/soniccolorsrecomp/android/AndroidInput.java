package org.soniccolorsrecomp.android;

import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

/** Controller event adapter for a future game Activity, backed by the runtime Pad API. */
final class AndroidInput {
    static { System.loadLibrary("scr_android_probe"); }
    static native void setConnected(boolean connected);
    static native void setButton(int button, boolean down);
    static native void setSticks(float lx, float ly, float rx, float ry);
    static native void setTriggers(float left, float right);
    static native void reset();
    static native int readButtonsForTest();

    private int deviceId = -1;
    void clear() { deviceId = -1; reset(); }
    void deviceRemoved(int id) { if (id == deviceId) clear(); }
    private boolean accepts(int id) {
        if (deviceId != -1 && deviceId != id) return false;
        if (deviceId == -1) { deviceId = id; setConnected(true); }
        return true;
    }
    boolean key(KeyEvent event) {
        int source = event.getSource();
        if ((source & InputDevice.SOURCE_GAMEPAD) != InputDevice.SOURCE_GAMEPAD &&
            (source & InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK) return false;
        int button;
        switch (event.getKeyCode()) {
            case KeyEvent.KEYCODE_BUTTON_A: button = 0x1000; break;
            case KeyEvent.KEYCODE_BUTTON_B: button = 0x2000; break;
            case KeyEvent.KEYCODE_BUTTON_X: button = 0x4000; break;
            case KeyEvent.KEYCODE_BUTTON_Y: button = 0x8000; break;
            case KeyEvent.KEYCODE_BUTTON_L1: button = 0x0100; break;
            case KeyEvent.KEYCODE_BUTTON_R1: button = 0x0200; break;
            case KeyEvent.KEYCODE_BUTTON_START: button = 0x0010; break;
            case KeyEvent.KEYCODE_BUTTON_SELECT: button = 0x0020; break;
            case KeyEvent.KEYCODE_DPAD_UP: button = 0x0001; break;
            case KeyEvent.KEYCODE_DPAD_DOWN: button = 0x0002; break;
            case KeyEvent.KEYCODE_DPAD_LEFT: button = 0x0004; break;
            case KeyEvent.KEYCODE_DPAD_RIGHT: button = 0x0008; break;
            default: return false;
        }
        int action = event.getAction();
        if (action != KeyEvent.ACTION_DOWN && action != KeyEvent.ACTION_UP) return false;
        if (!accepts(event.getDeviceId())) return false;
        setButton(button, action == KeyEvent.ACTION_DOWN); return true;
    }
    boolean motion(MotionEvent event) {
        if ((event.getSource() & InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK ||
            event.getActionMasked() != MotionEvent.ACTION_MOVE || !accepts(event.getDeviceId())) return false;
        setSticks(event.getAxisValue(MotionEvent.AXIS_X), -event.getAxisValue(MotionEvent.AXIS_Y),
                  event.getAxisValue(MotionEvent.AXIS_Z), -event.getAxisValue(MotionEvent.AXIS_RZ));
        setTriggers(event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_RTRIGGER));
        return true;
    }
}
