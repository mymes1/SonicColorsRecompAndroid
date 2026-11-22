#pragma once
#include <cstdint>

// The overlay is Dear ImGui drawn by the Direct3D 11 backend, so its init takes a device and a
// context. On Android the type is only named, never used: the Activity owns the UI, and
// platform/android/desktop_stubs.cpp supplies these entry points so shared code still links.
#ifndef SCR_ANDROID
#include <d3d11.h>
#else
struct ID3D11Device;
struct ID3D11DeviceContext;
#endif

// In-game settings overlay (Dear ImGui), opened with the controller's View button or F1.
// The game keeps running underneath; while the overlay is open the game sees no input.
// The window's input reaches it through the platform (platform/*/ui.cpp).
void Overlay_Init(void* window, ID3D11Device* device, ID3D11DeviceContext* ctx);
bool Overlay_Ready();   // initialised (never during TAS runs)
// Render thread, once per presented frame, with the target (w x h) bound.
void Overlay_Render(float w, float h);
// Test aid: a SCR_UI_SHOT_OVERLAY (legacy BKR_*) screenshot is due (may draw off screen).
bool Overlay_ShotPending();
void Overlay_Toggle();
bool Overlay_IsOpen();
// Brief on-screen notice (screenshot saved, controller unplug, …). No-op until Overlay_Init.
void Overlay_Toast(const char* msg, uint32_t durationMs = 2500);
// Asks for a Wii HOME button press (overlay button); read by the gamepad code.
bool Overlay_TakeHomePress();
