#pragma once
#include <cstdint>

// Input producer API for an Android Activity. The runtime polls Pad_Read on
// its guest thread; producers may update state from the Android UI thread.
void AndroidInput_SetConnected(bool connected);
void AndroidInput_SetButton(uint16_t button, bool down);
void AndroidInput_SetSticks(float lx, float ly, float rx, float ry);
void AndroidInput_SetTriggers(float left, float right);
void AndroidInput_Reset();
// A haptic pulse of `ms` milliseconds, for a rumble-enabled pad (Wii Remote). Declared here rather
// than inline in the JNI bridge: input.cpp is C++, so this has C++ linkage and a function-local
// `extern` in an extern "C" block would ask the linker for an unmangled symbol that never exists.
void AndroidInput_RequestVibrate(int ms);
