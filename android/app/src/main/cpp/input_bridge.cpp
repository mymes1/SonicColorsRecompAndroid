// JNI surface of AndroidInput.java: the UI thread's controller, touch and keyboard state is
// pushed here and read by the runtime's Pad_Read (runtime/platform/android/input.cpp).
//
// The Java side talks in opaque key tokens (AndroidInput.KEY_*) because one path carries
// gamepad buttons, touch controls and a USB keyboard; this file is the only place that
// knows what they mean, and it translates them into the XInput-style PAD_* bits the rest of
// the runtime already uses (runtime/input/gamepad.h). Tokens are matched by number, so the
// two files have to change together.
#include <jni.h>

#include <android/log.h>

#include "input.h"
#include "gamepad.h"
#include "platform/input.h"

#include <string>

namespace
{
enum JavaKey : int
{
    KEY_A = 1, KEY_B, KEY_X, KEY_Y,
    KEY_L1, KEY_R1, KEY_L2, KEY_R2,
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_PLUS, KEY_MINUS, KEY_HOME, KEY_VIEW,
    KEY_1, KEY_2, KEY_THUMBL, KEY_THUMBR,
};

// Token → PAD_* bit. 0 for tokens the runtime has no use for (the Activity consumes those:
// KEY_VIEW opens the overlay, KEY_HOME is the pad's mode button).
uint16_t PadBit(int token)
{
    switch (token)
    {
    case KEY_A: return PAD_A;
    case KEY_B: return PAD_B;
    case KEY_X: return PAD_X;
    case KEY_Y: return PAD_Y;
    // Classic Controller L1/R1 mirror the shoulders; L2/R2 are the analog triggers, which the
    // Wii inputs treat as the same two digital buttons.
    case KEY_L1: return PAD_LB;
    case KEY_R1: return PAD_RB;
    case KEY_L2: return PAD_LB;
    case KEY_R2: return PAD_RB;
    case KEY_UP: return PAD_DPAD_UP;
    case KEY_DOWN: return PAD_DPAD_DOWN;
    case KEY_LEFT: return PAD_DPAD_LEFT;
    case KEY_RIGHT: return PAD_DPAD_RIGHT;
    case KEY_PLUS: return PAD_START;
    case KEY_MINUS: return PAD_BACK;
    case KEY_HOME: return PAD_HOME;
    case KEY_1: return PAD_LB;
    case KEY_2: return PAD_RB;
    case KEY_THUMBL: return PAD_LTHUMB;
    case KEY_THUMBR: return PAD_RTHUMB;
    default: return 0;
    }
}

// Buttons from setButton (a real D-pad or the touch D-pad), and the left stick, which also
// counts as the D-pad past half deflection so menus and pointer-less Wii Remote schemes work
// with an analog stick alone.
uint16_t g_pressed = 0;
float g_stickX = 0.0f, g_stickY = 0.0f;
constexpr float kStickDpadThreshold = 0.5f;

uint16_t Compose()
{
    uint16_t bits = g_pressed;
    if (g_stickY > kStickDpadThreshold) bits |= PAD_DPAD_UP;
    if (g_stickY < -kStickDpadThreshold) bits |= PAD_DPAD_DOWN;
    if (g_stickX < -kStickDpadThreshold) bits |= PAD_DPAD_LEFT;
    if (g_stickX > kStickDpadThreshold) bits |= PAD_DPAD_RIGHT;
    return bits;
}

void Publish()
{
    // The producer only has per-bit setters, so push the whole mask every time.
    const uint16_t bits = Compose();
    for (uint16_t bit = 1; bit; bit = (uint16_t)(bit << 1))
        AndroidInput_SetButton(bit, (bits & bit) != 0);
}

const char* g_controllerName = "Android controller";
}

extern "C" JNIEXPORT void JNICALL
Java_org_soniccolorsrecomp_android_AndroidInput_setConnected(JNIEnv*, jclass, jboolean value)
{
    AndroidInput_SetConnected(value != 0);
}

extern "C" JNIEXPORT void JNICALL
Java_org_soniccolorsrecomp_android_AndroidInput_setButton(JNIEnv*, jclass, jint token, jboolean down)
{
    const uint16_t bit = PadBit(token);
    if (!bit) return;
    if (down) g_pressed = (uint16_t)(g_pressed | bit);
    else g_pressed = (uint16_t)(g_pressed & ~bit);
    Publish();
}

extern "C" JNIEXPORT void JNICALL
Java_org_soniccolorsrecomp_android_AndroidInput_setStick(JNIEnv*, jclass, jfloat x, jfloat y)
{
    // Java sends -1..1 with up positive; the runtime wants -32768..32767.
    auto clamp1 = [](float v) { return v != v ? 0.0f : (v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v)); };
    g_stickX = clamp1(x);
    g_stickY = clamp1(y);
    AndroidInput_SetSticks(g_stickX, g_stickY, 0.0f, 0.0f);
    Publish();
}

extern "C" JNIEXPORT void JNICALL
Java_org_soniccolorsrecomp_android_AndroidInput_setTriggers(JNIEnv*, jclass, jfloat left, jfloat right)
{
    AndroidInput_SetTriggers(left, right);
}

extern "C" JNIEXPORT void JNICALL
Java_org_soniccolorsrecomp_android_AndroidInput_reset(JNIEnv*, jclass)
{
    g_pressed = 0;
    g_stickX = g_stickY = 0.0f;
    AndroidInput_Reset();
    // Touch controls are always available, so the pad never disappears entirely.
    AndroidInput_SetConnected(true);
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_soniccolorsrecomp_android_AndroidInput_controllerName(JNIEnv* env, jclass)
{
    // Filled in by setDeviceName() below when the Activity sees a pad; the runtime only uses
    // this for the log and the controller overlay.
    return env->NewStringUTF(g_controllerName);
}

extern "C" JNIEXPORT void JNICALL
Java_org_soniccolorsrecomp_android_AndroidInput_setDeviceName(JNIEnv* env, jclass, jstring name)
{
    static std::string storage;
    if (!name) { storage.clear(); return; }
    const char* utf = env->GetStringUTFChars(name, nullptr);
    storage = utf ? utf : "";
    if (utf) env->ReleaseStringUTFChars(name, utf);
    g_controllerName = storage.c_str();
}

extern "C" JNIEXPORT void JNICALL
Java_org_soniccolorsrecomp_android_AndroidInput_vibrate(JNIEnv*, jclass, jint ms)
{
    // Wii Remote rumble reaches this; the Activity owns the Vibrator, so the platform layer
    // reports the request and the Java haptics (touch feedback, see GameInputView) stay
    // separate. Pad_SetRumble calls this through androidapp::Vibrate.
    extern void AndroidInput_RequestVibrate(int ms);
    AndroidInput_RequestVibrate(ms);
}
