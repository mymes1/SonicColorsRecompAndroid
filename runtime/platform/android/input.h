#pragma once
#include <cstdint>

// Input producer API for an Android Activity. The runtime polls Pad_Read on
// its guest thread; producers may update state from the Android UI thread.
void AndroidInput_SetConnected(bool connected);
void AndroidInput_SetButton(uint16_t button, bool down);
void AndroidInput_SetSticks(float lx, float ly, float rx, float ry);
void AndroidInput_SetTriggers(float left, float right);
void AndroidInput_Reset();
