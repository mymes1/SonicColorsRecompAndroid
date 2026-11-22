#include "input.h"
#include "../input.h"
#include "platform_android.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>

namespace {
std::mutex mutex;
PadState state;
bool connected = false;
// Rumble is a discontinuous vibration on Android: the VM refreshes it between the guest's
// frames, so only the 0 -> non-zero transition starts the motor.
bool rumbling = false;
char controllerName[64] = "Android controller";

float Clamp(float value, float low, float high) {
    return std::isfinite(value) ? std::clamp(value, low, high) : 0.0f;
}
int16_t Axis(float value) {
    value = Clamp(value, -1.0f, 1.0f);
    return value < 0 ? static_cast<int16_t>(value * 32768.0f)
                     : static_cast<int16_t>(value * 32767.0f);
}
uint8_t Trigger(float value) {
    return static_cast<uint8_t>(Clamp(value, 0.0f, 1.0f) * 255.0f);
}
}

void AndroidInput_SetConnected(bool value) {
    std::lock_guard<std::mutex> lock(mutex);
    connected = value;
    if (!value) state = {};
}
void AndroidInput_SetButton(uint16_t button, bool down) {
    std::lock_guard<std::mutex> lock(mutex);
    if (down) state.buttons |= button;
    else state.buttons &= static_cast<uint16_t>(~button);
}
void AndroidInput_SetSticks(float lx, float ly, float rx, float ry) {
    std::lock_guard<std::mutex> lock(mutex);
    state.lx = Axis(lx); state.ly = Axis(ly);
    state.rx = Axis(rx); state.ry = Axis(ry);
}
void AndroidInput_SetTriggers(float left, float right) {
    std::lock_guard<std::mutex> lock(mutex);
    state.lt = Trigger(left); state.rt = Trigger(right);
}
void AndroidInput_Reset() {
    std::lock_guard<std::mutex> lock(mutex);
    state = {}; connected = false;
}
int AndroidInput_Buttons() {
    std::lock_guard<std::mutex> lock(mutex);
    return connected ? static_cast<int>(state.buttons) : -1;
}
bool Pad_Read(int index, PadState& out) {
    std::lock_guard<std::mutex> lock(mutex);
    if (index != 0 || !connected) { out = {}; return false; }
    out = state; return true;
}
void Pad_SetRumble(int, uint16_t low, uint16_t high) {
    const bool on = (low | high) != 0;
    if (on != rumbling) {
        rumbling = on;
        if (on) androidapp::Vibrate(60, (int)(std::max(low, high) >> 8));
        else androidapp::CancelVibration();
    }
}
int Pad_Count() { std::lock_guard<std::mutex> lock(mutex); return connected ? 1 : 0; }
const char* Pad_Name(int index) {
    std::lock_guard<std::mutex> lock(mutex);
    return index == 0 && connected ? controllerName : nullptr;
}
// AndroidInput.java's vibrate(int) and the controller's reported name.
void AndroidInput_RequestVibrate(int ms) { androidapp::Vibrate(ms, 255); }
void AndroidInput_SetControllerName(const char* name) {
    std::lock_guard<std::mutex> lock(mutex);
    if (name && *name) { snprintf(controllerName, sizeof(controllerName), "%s", name); }
    else snprintf(controllerName, sizeof(controllerName), "%s", "Android controller");
}
void Input_EnsurePadsProbed() {}
void Input_PollHotplug() {}
bool Key_Down(int) { return false; }
