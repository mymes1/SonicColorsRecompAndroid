// The game window on Windows: its own thread runs the message loop.
#include "../window.h"
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
#include <thread>
#include <windows.h>

bool Overlay_WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);   // platform/win32/ui.cpp

static HWND s_hwnd = nullptr;
static std::atomic<bool> s_resized{ false };
static bool s_fullscreen = false;   // window thread only
static int s_lastWindowedW = 0, s_lastWindowedH = 0;
static bool s_maximized = false;
constexpr UINT WM_APP_FULLSCREEN = WM_APP + 1;   // wParam: 1 fullscreen, 0 windowed
constexpr UINT WM_APP_WINDOWSIZE = WM_APP + 2;   // wParam: width, lParam: height

static void RememberWindowedSize(HWND hwnd)
{
    if (s_fullscreen || IsIconic(hwnd)) return;
    s_maximized = IsZoomed(hwnd) != FALSE;
    if (s_maximized) return;
    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w >= 320 && h >= 240)
    {
        s_lastWindowedW = w;
        s_lastWindowedH = h;
    }
}

static void ApplyPauseFromFocusOrMinimize(bool appActive)
{
    if (!g_config.pauseOnFocusLoss || !s_hwnd) return;
    extern std::atomic<bool> g_pauseRequested;
    const bool pause = !appActive || IsIconic(s_hwnd);
    g_pauseRequested = pause;
    if (pause)
    {
        void Gamepad_StopRumble();
        Gamepad_StopRumble();
    }
}

// borderless fullscreen on the window's monitor, or back to the saved window rect
static void SetFullscreen(HWND hwnd, bool on)
{
    static RECT saved;
    if (on == s_fullscreen) return;
    if (!on)
        RememberWindowedSize(hwnd);
    s_fullscreen = on;
    if (on)
    {
        GetWindowRect(hwnd, &saved);
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(mon, &mi);
        SetWindowLongW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
    }
    else
    {
        SetWindowLongW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(hwnd, nullptr, saved.left, saved.top, saved.right - saved.left, saved.bottom - saved.top, SWP_FRAMECHANGED);
        RememberWindowedSize(hwnd);
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (Overlay_WndProc(hwnd, msg, wp, lp)) return 0;   // the settings overlay has the input
    switch (msg)
    {
    case WM_CLOSE:
        // Closing the window quits. Saves are written to the host NAND as the game
        // issues them, so there is nothing to flush; terminate directly rather than
        // running DLL teardown while guest/GX/audio threads are mid-operation.
        LOG_INFO("window closed, exiting");
        RememberWindowedSize(hwnd);
        Window_PersistSize();
        { void Gamepad_StopRumble(); Gamepad_StopRumble(); }
        TerminateProcess(GetCurrentProcess(), 0);
        return 0;
    case WM_SIZE:
        s_resized = true;
        if (wp == SIZE_MINIMIZED || wp == SIZE_RESTORED || wp == SIZE_MAXIMIZED)
        {
            if (wp != SIZE_MINIMIZED)
                RememberWindowedSize(hwnd);
            ApplyPauseFromFocusOrMinimize(GetForegroundWindow() == hwnd);
        }
        else
            RememberWindowedSize(hwnd);
        return 0;
    case WM_ACTIVATEAPP:
        ApplyPauseFromFocusOrMinimize(wp != FALSE);
        break;
    case WM_SETCURSOR:
        // the game draws its own Wii Remote pointer; the overlay needs the mouse cursor
        if (LOWORD(lp) == HTCLIENT)
        {
            SetCursor(Overlay_IsOpen() ? LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)) : nullptr);
            return TRUE;
        }
        break;
    case WM_APP_FULLSCREEN:
        SetFullscreen(hwnd, wp != 0);
        return 0;
    case WM_APP_WINDOWSIZE:
        if (!s_fullscreen)
        {
            RECT r{ 0, 0, (LONG)wp, (LONG)lp };
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
        }
        return 0;
    case WM_KEYDOWN:
        {
            const int vkFs = Config_HotkeyVk(g_config.hotkeyFullscreen, VK_F11);
            const int vkSet = Config_HotkeyVk(g_config.hotkeySettings, VK_F1);
            const int vkShot = Config_HotkeyVk(g_config.hotkeyScreenshot, VK_F12);
            if ((int)wp == vkFs || (wp == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000)))
            {
                SetFullscreen(hwnd, !s_fullscreen);
                g_config.fullscreen = s_fullscreen;
            }
            else if ((int)wp == vkSet)
                Overlay_Toggle();
            else if ((int)wp == vkShot)
            {
                extern void Renderer_RequestScreenshot();
                Renderer_RequestScreenshot();
            }
        }
        break;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN) { SendMessageW(hwnd, WM_KEYDOWN, VK_RETURN, lp); return 0; }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

struct WindowParams { int width, height; bool fullscreen; std::atomic<int> ready{ 0 }; };

static void WindowThread(WindowParams* p)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = L"SonicColorsRecomp";
    RegisterClassW(&wc);
    RECT r{ 0, 0, p->width, p->height };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    // Window class keeps the legacy name for ABI; the title is user-visible branding + build.
    const std::string titleUtf8 = std::format("Sonic Colors - Recompiled ({})", std::string_view{g_version});
    std::wstring title(MultiByteToWideChar(CP_UTF8, 0, titleUtf8.data(), (int)titleUtf8.size(), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, titleUtf8.data(), (int)titleUtf8.size(), title.data(), (int)title.size());
    s_hwnd = CreateWindowW(L"SonicColorsRecomp", title.c_str(), WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (s_hwnd)
    {
        s_lastWindowedW = p->width;
        s_lastWindowedH = p->height;
        RememberWindowedSize(s_hwnd);
        if (p->fullscreen) SetFullscreen(s_hwnd, true);
    }
    p->ready = s_hwnd ? 1 : -1;
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

bool Window_Create(int width, int height, bool fullscreen)
{
    static WindowParams p;
    p.width = width;
    p.height = height;
    p.fullscreen = fullscreen;
    std::thread(WindowThread, &p).detach();
    while (p.ready == 0) Sleep(1);
    if (p.ready < 0)
    {
        LOG_ERROR("CreateWindow failed ({})", GetLastError());
        return false;
    }
    return true;
}

void* Window_Handle() { return s_hwnd; }

void Window_ClientSize(int& width, int& height)
{
    RECT rc;
    GetClientRect(s_hwnd, &rc);
    width = rc.right - rc.left;
    height = rc.bottom - rc.top;
}

bool Window_TakeResized() { return s_resized.exchange(false); }

void Window_SetFullscreen(bool on) { if (s_hwnd) PostMessageW(s_hwnd, WM_APP_FULLSCREEN, on ? 1 : 0, 0); }
void Window_SetSize(int w, int h) { if (s_hwnd) PostMessageW(s_hwnd, WM_APP_WINDOWSIZE, (WPARAM)w, (LPARAM)h); }

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
    if (!s_fullscreen && g_config.windowMaximized != s_maximized)
    {
        g_config.windowMaximized = s_maximized;
        dirty = true;
    }
    if (dirty) Config_Save();
}

void Window_SetMaximized(bool on)
{
    if (!s_hwnd || s_fullscreen) return;
    ShowWindow(s_hwnd, on ? SW_MAXIMIZE : SW_RESTORE);
}

bool Window_Focused() { return GetForegroundWindow() == s_hwnd; }

bool Window_Cursor(int& x, int& y)
{
    POINT pt;
    if (!GetCursorPos(&pt)) return false;
    x = pt.x;
    y = pt.y;
    return true;
}

float Window_DisplayRefreshHz()
{
    if (!s_hwnd) return 0.0f;
    HMONITOR mon = MonitorFromWindow(s_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return 0.0f;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return 0.0f;
    return dm.dmDisplayFrequency > 0 ? (float)dm.dmDisplayFrequency : 0.0f;
}

bool Window_CursorInClient(int x, int y, float& u, float& v)
{
    POINT pt{ x, y };
    RECT rc;
    if (ScreenToClient(s_hwnd, &pt) && GetClientRect(s_hwnd, &rc) && rc.right > 0 && rc.bottom > 0 && pt.x >= 0 && pt.y >= 0 &&
        pt.x < rc.right && pt.y < rc.bottom)
    {
        u = (float)pt.x / rc.right;
        v = (float)pt.y / rc.bottom;
        return true;
    }
    return false;
}
