// The parts of the runtime that need a desktop (Dear ImGui over D3D11, WinHTTP/libcurl,
// libarchive, the file dialogs) are not built for Android: the Activity owns the UI, and the APK
// ships no mod browser or updater. This file supplies the symbols so the shared runtime links, and
// every entry point either reports "not available" or does nothing. Gameplay never calls them; the
// launcher/settings/overlay code that does is not compiled into the APK (see
// android/app/src/main/cpp/CMakeLists.txt).
//
// Not stubbed here because Android has real implementations: image.cpp (PNG codec), audio_out.cpp
// (AAudio), platform.cpp, fiber.cpp, memory.cpp, input.cpp, renderer_vk.cpp, video.cpp.
//
// The one exception is the in-game overlay's home button and toasts: the gamepad code polls
// them every frame, so they are real (a boolean) rather than no-ops.
#include "../../ui/overlay.h"
#include "../http.h"
#include "../image.h"
#include "../ui.h"
#include "../window.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// ================================================================== in-game overlay
namespace
{
std::atomic<bool> g_overlayOpen{ false };
std::atomic<bool> g_homePressed{ false };
}

void Overlay_Init(void*, ID3D11Device*, ID3D11DeviceContext*) {}   // void* is the ANativeWindow
bool Overlay_Ready() { return false; }
void Overlay_Render(float, float) {}
bool Overlay_ShotPending() { return false; }
void Overlay_Toggle() { g_overlayOpen = !g_overlayOpen; }
bool Overlay_IsOpen() { return g_overlayOpen; }
void Overlay_Toast(const char*, uint32_t) {}
bool Overlay_TakeHomePress() { return g_homePressed.exchange(false); }

// The Activity's pause menu asks for the same thing the desktop overlay does.
void Overlay_PressHome() { g_homePressed = true; }

// ================================================================== HTTPS
// Nothing in the APK downloads: the mod browser, update check and GameBanana integration are
// desktop features. Failing here (rather than silently succeeding) is what the callers expect
// when a connection cannot be made.
bool Http_Fetch(const std::string&, const std::function<bool(const uint8_t*, size_t)>&, std::string& error,
                HttpProgress*, uint64_t)
{
    error = "network access is not available in this build";
    return false;
}

// ================================================================== images
// Texture packs are PNG files; without a decoder the packs are skipped and the game uses the
// disc's own textures, which is the same behaviour as a desktop build with no pack installed.

// ================================================================== launcher UI
void UiBackend_Init(void*) {}
void UiBackend_NewFrame() {}
void UiBackend_Shutdown() {}
void UiBackend_ReplayInput() {}
void* UiWindow_Create(const char*, int, int) { return nullptr; }
void UiWindow_Show() {}
bool UiWindow_Pump(uint32_t&, uint32_t&) { return false; }
bool UiWindow_GetPixelSize(uint32_t&, uint32_t&) { return false; }
void UiWindow_Destroy() {}
void Shell_OpenUrl(const std::string&) {}
void Shell_OpenFolder(const std::filesystem::path&) {}
bool Dialog_PickDiscImage(std::filesystem::path&) { return false; }
bool Dialog_PickWavFile(std::filesystem::path&) { return false; }
bool Dialog_SaveWavFile(std::filesystem::path&, const std::string&) { return false; }

// ================================================================== game window
// The Activity's Surface is the window; see platform_android.h.
bool Window_Create(int, int, bool) { return true; }
void* Window_Handle() { return (void*)1; }   // non-null: "the game has a window"
void Window_ClientSize(int& width, int& height)
{
    width = 1920;
    height = 1080;
}
bool Window_TakeResized() { return false; }
void Window_SetFullscreen(bool) {}
void Window_SetSize(int, int) {}
void Window_PersistSize() {}
void Window_SetMaximized(bool) {}
// The game runs in the foreground; touch and gamepad input are always live, and rumble is
// requested by Pad_SetRumble without asking whether the app is focused.
bool Window_Focused() { return true; }
bool Window_Cursor(int& x, int& y)
{
    x = y = 0;
    return false;
}
bool Window_CursorInClient(int, int, float&, float&) { return false; }
float Window_DisplayRefreshHz() { return 60.0f; }
