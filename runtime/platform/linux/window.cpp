// The game window on Linux: an SDL3 window, created and serviced on the main thread
// (Platform_RunGame runs the game elsewhere). dxvk-native presents into it.
#include "../window.h"
#include "../platform.h"
#include "sdl.h"
#include "../../audio/audio_out.h"
#include "../../config.h"
#include "../../log.h"
#include "../../ui/overlay.h"
#include "../../version.h"

#include <atomic>
#ifndef __ANDROID__
#include <format>   // on Android, platform/compat.h provides it
#endif
#include <string>
#include <string_view>

static SDL_Window* s_window = nullptr;
static std::atomic<bool> s_resized{ false };
static std::atomic<bool> s_focused{ false };
static std::atomic<int> s_pixelW{ 0 }, s_pixelH{ 0 };
static std::atomic<int> s_mouseX{ -1 }, s_mouseY{ -1 };
static std::atomic<bool> s_mouseInside{ false };
static int s_windowW = 1, s_windowH = 1;   // in window coordinates (mouse), main thread
static int s_lastWindowedW = 0, s_lastWindowedH = 0;
static bool s_maximized = false;

static void UpdateSize()
{
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(s_window, &w, &h);
    s_pixelW = w;
    s_pixelH = h;
    SDL_GetWindowSize(s_window, &s_windowW, &s_windowH);
    const Uint32 flags = SDL_GetWindowFlags(s_window);
    s_maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
    if (!(flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MINIMIZED)) && !s_maximized &&
        s_windowW >= 320 && s_windowH >= 240)
    {
        s_lastWindowedW = s_windowW;
        s_lastWindowedH = s_windowH;
    }
}

static void ApplyPauseFromFocusOrMinimize()
{
    if (!g_config.pauseOnFocusLoss || !s_window) return;
    extern std::atomic<bool> g_pauseRequested;
    const Uint32 flags = SDL_GetWindowFlags(s_window);
    const bool minimized = (flags & SDL_WINDOW_MINIMIZED) != 0;
    const bool pause = minimized || !s_focused;
    g_pauseRequested = pause;
    if (pause)
    {
        void Gamepad_StopRumble();
        Gamepad_StopRumble();
    }
}

struct CreateParams { int width, height; bool fullscreen; bool ok = false; };

// Borderless desktop (NULL mode) vs exclusive display mode from config.
static void ApplyFullscreenMode()
{
    if (!s_window) return;
    if (!g_config.exclusiveFullscreen)
    {
        SDL_SetWindowFullscreenMode(s_window, nullptr);
        return;
    }
    SDL_DisplayID disp = SDL_GetDisplayForWindow(s_window);
    const SDL_DisplayMode* desk = SDL_GetDesktopDisplayMode(disp);
    if (!desk)
    {
        LOG_WARN("exclusive fullscreen: no desktop mode ({})", SDL_GetError());
        SDL_SetWindowFullscreenMode(s_window, nullptr);
        return;
    }
    SDL_DisplayMode closest{};
    const SDL_DisplayMode* mode = desk;
    if (SDL_GetClosestFullscreenDisplayMode(disp, desk->w, desk->h, desk->refresh_rate, true, &closest))
        mode = &closest;
    if (!SDL_SetWindowFullscreenMode(s_window, mode))
    {
        LOG_WARN("exclusive fullscreen mode failed ({}); using borderless", SDL_GetError());
        SDL_SetWindowFullscreenMode(s_window, nullptr);
    }
}

static void CreateOnMain(void* userdata)
{
    CreateParams* p = (CreateParams*)userdata;
    if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_InitSubSystem(SDL_INIT_VIDEO))
    {
        LOG_ERROR("SDL video init failed ({})", SDL_GetError());
        return;
    }
    // Create windowed first, then apply mode + fullscreen — exclusive needs a window/display.
    const std::string title = std::format("Sonic Colors - Recompiled ({})", std::string_view{g_version});
    s_window = SDL_CreateWindow(title.c_str(), p->width, p->height,
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!s_window)
    {
        LOG_ERROR("SDL_CreateWindow failed ({})", SDL_GetError());
        return;
    }
    ApplyFullscreenMode();
    if (p->fullscreen)
        SDL_SetWindowFullscreen(s_window, true);
    SDL_HideCursor();   // the game draws its own Wii Remote pointer
    UpdateSize();
    s_lastWindowedW = p->width;
    s_lastWindowedH = p->height;
    s_focused = (SDL_GetWindowFlags(s_window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    p->ok = true;
}

bool Window_Create(int width, int height, bool fullscreen)
{
    CreateParams p{ width, height, fullscreen };
    if (!SDL_RunOnMainThread(CreateOnMain, &p, true))
    {
        LOG_ERROR("cannot create the window on the main thread ({})", SDL_GetError());
        return false;
    }
    return p.ok;
}

void* Window_Handle() { return s_window; }

void Window_ClientSize(int& width, int& height)
{
    width = s_pixelW;
    height = s_pixelH;
}

bool Window_TakeResized() { return s_resized.exchange(false); }

static void SetFullscreenOnMain(void* on)
{
    if (!s_window) return;
    ApplyFullscreenMode();
    SDL_SetWindowFullscreen(s_window, on != nullptr);
}

void Window_SetFullscreen(bool on) { SDL_RunOnMainThread(SetFullscreenOnMain, on ? (void*)1 : nullptr, false); }

static int s_requestW, s_requestH;
static void SetSizeOnMain(void*)
{
    if (s_window && !(SDL_GetWindowFlags(s_window) & SDL_WINDOW_FULLSCREEN)) SDL_SetWindowSize(s_window, s_requestW, s_requestH);
}

void Window_SetSize(int w, int h)
{
    s_requestW = w;
    s_requestH = h;
    SDL_RunOnMainThread(SetSizeOnMain, nullptr, false);
}

void Window_PersistSize()
{
    bool dirty = false;
    if (s_lastWindowedW >= 320 && s_lastWindowedH >= 240 &&
        (g_config.windowWidth != s_lastWindowedW || g_config.windowHeight != s_lastWindowedH))
    {
        g_config.windowWidth = s_lastWindowedW;
        g_config.windowHeight = s_lastWindowedH;
        dirty = true;
    }
    if (!s_window || !(SDL_GetWindowFlags(s_window) & SDL_WINDOW_FULLSCREEN))
    {
        if (g_config.windowMaximized != s_maximized)
        {
            g_config.windowMaximized = s_maximized;
            dirty = true;
        }
    }
    if (dirty) Config_Save();
}

static void SetMaximizedOnMain(void* on)
{
    if (!s_window || (SDL_GetWindowFlags(s_window) & SDL_WINDOW_FULLSCREEN)) return;
    if (on)
        SDL_MaximizeWindow(s_window);
    else
        SDL_RestoreWindow(s_window);
}

void Window_SetMaximized(bool on) { SDL_RunOnMainThread(SetMaximizedOnMain, on ? (void*)1 : nullptr, false); }

bool Window_Focused() { return s_window && s_focused; }

bool Window_Cursor(int& x, int& y)
{
    if (!s_window) return false;
    x = s_mouseX;
    y = s_mouseY;
    return true;
}

float Window_DisplayRefreshHz()
{
    if (!s_window) return 0.0f;
    SDL_DisplayID disp = SDL_GetDisplayForWindow(s_window);
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(disp);
    if (!mode || mode->refresh_rate <= 0.0f)
        mode = SDL_GetDesktopDisplayMode(disp);
    return (mode && mode->refresh_rate > 0.0f) ? (float)mode->refresh_rate : 0.0f;
}

bool Window_CursorInClient(int x, int y, float& u, float& v)
{
    if (!s_mouseInside || x < 0 || y < 0 || x >= s_windowW || y >= s_windowH) return false;
    u = (float)x / s_windowW;
    v = (float)y / s_windowH;
    return true;
}

// ---------------------------------------------------------------- main thread
static void HandleEvent(const SDL_Event& e)
{
    Input_HandleEvent(e);   // controllers coming and going
    AudioOut_HandleEvent(&e);   // output device unplug / replug
    if (Overlay_HandleEvent(e)) return;   // the settings overlay has the input
    switch (e.type)
    {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        // Closing the window quits. Saves are written to the host NAND as the game issues
        // them, so there is nothing to flush (as on Windows).
        LOG_INFO("window closed, exiting");
        Window_PersistSize();
        { void Gamepad_StopRumble(); Gamepad_StopRumble(); }
        Platform_Terminate(0);
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        UpdateSize();
        s_resized = true;
        break;
    case SDL_EVENT_WINDOW_MINIMIZED:
    case SDL_EVENT_WINDOW_RESTORED:
        ApplyPauseFromFocusOrMinimize();
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        s_focused = e.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
        ApplyPauseFromFocusOrMinimize();
        break;
    case SDL_EVENT_MOUSE_MOTION:
        s_mouseX = (int)e.motion.x;
        s_mouseY = (int)e.motion.y;
        s_mouseInside = true;
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        s_mouseInside = false;
        break;
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
        s_mouseInside = true;
        break;
    case SDL_EVENT_KEY_DOWN:
        if (e.key.repeat) break;
        {
            const int vkFs = Config_HotkeyVk(g_config.hotkeyFullscreen, 0x7A);   // VK_F11
            const int vkSet = Config_HotkeyVk(g_config.hotkeySettings, 0x70);    // VK_F1
            const int vkShot = Config_HotkeyVk(g_config.hotkeyScreenshot, 0x7B); // VK_F12
            const SDL_Keycode kFs = (SDL_Keycode)(SDLK_F1 + (vkFs - 0x70));
            const SDL_Keycode kSet = (SDL_Keycode)(SDLK_F1 + (vkSet - 0x70));
            const SDL_Keycode kShot = (SDL_Keycode)(SDLK_F1 + (vkShot - 0x70));
            if (e.key.key == kFs || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT)))
            {
                bool on = !(SDL_GetWindowFlags(s_window) & SDL_WINDOW_FULLSCREEN);
                ApplyFullscreenMode();
                SDL_SetWindowFullscreen(s_window, on);
                g_config.fullscreen = on;
            }
            else if (e.key.key == kSet)
                Overlay_Toggle();
            else if (e.key.key == kShot)
            {
                extern void Renderer_RequestScreenshot();
                Renderer_RequestScreenshot();
            }
        }
        break;
    }
}

void Window_RunEventLoop()
{
    Input_ProbeConnectedPads();
    bool cursorShown = false;
    for (;;)
    {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 50))
        {
            HandleEvent(e);
            while (SDL_PollEvent(&e)) HandleEvent(e);
        }
        // the overlay needs the mouse cursor; the game draws its own pointer
        bool show = Overlay_IsOpen();
        if (s_window && show != cursorShown)
        {
            cursorShown = show;
            if (show) SDL_ShowCursor();
            else SDL_HideCursor();
        }
    }
}
