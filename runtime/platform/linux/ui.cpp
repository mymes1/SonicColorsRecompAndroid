// Launcher window, overlay input and installer dialogs on Linux: SDL3 windows, ImGui's
// SDL3 backend, SDL's file dialog (desktop portal or zenity) and a small progress window.
#include "../ui.h"
#include "../platform.h"
#include "../window.h"
#include "sdl.h"
#include "../../config.h"
#include "../../env.h"
#include "../../log.h"
#include "../../ui/overlay.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#ifndef __ANDROID__
#include <format>   // on Android, platform/compat.h provides it
#endif
#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------- ImGui platform backend
void UiBackend_Init(void* window)
{
    ImGui_ImplSDL3_InitForD3D((SDL_Window*)window);
    // in game the event loop shows and hides the cursor (platform/linux/window.cpp)
    if (window == Window_Handle()) ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
}

void UiBackend_NewFrame() { ImGui_ImplSDL3_NewFrame(); }
void UiBackend_Shutdown() { ImGui_ImplSDL3_Shutdown(); }

// The overlay's input: events from the main thread, replayed on the render thread
namespace
{
struct QueuedEvent
{
    SDL_Event e;
    std::string text;   // SDL owns the text of a text input event only until the next poll
};
std::mutex s_eventMutex;
std::vector<QueuedEvent> s_events;

void Queue(const SDL_Event& e)
{
    QueuedEvent q{ e, {} };
    if (e.type == SDL_EVENT_TEXT_INPUT && e.text.text) q.text = e.text.text;
    std::lock_guard lk(s_eventMutex);
    s_events.push_back(std::move(q));
}
}   // namespace

bool Overlay_HandleEvent(const SDL_Event& e)
{
    if (!Overlay_Ready()) return false;
    switch (e.type)
    {
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
        Queue(e);   // ImGui's gamepad navigation keeps its controller list up to date
        return false;
    case SDL_EVENT_KEY_DOWN:
        if (!e.key.repeat)
        {
            const int vkSet = Config_HotkeyVk(g_config.hotkeySettings, 0x70);
            const SDL_Keycode kSet = (SDL_Keycode)(SDLK_F1 + (vkSet - 0x70));
            if (e.key.key == kSet || (e.key.key == SDLK_ESCAPE && Overlay_IsOpen()))
            {
                Overlay_Toggle();
                return true;
            }
        }
        [[fallthrough]];
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
        if (!Overlay_IsOpen()) return false;
        Queue(e);
        return true;   // the game sees no keys or clicks while it is open
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        if (Overlay_IsOpen()) Queue(e);
        return false;
    default:
        return false;
    }
}

void UiBackend_ReplayInput()
{
    std::vector<QueuedEvent> events;
    {
        std::lock_guard lk(s_eventMutex);
        events.swap(s_events);
    }
    for (auto& q : events)
    {
        if (q.e.type == SDL_EVENT_TEXT_INPUT) q.e.text.text = q.text.c_str();
        ImGui_ImplSDL3_ProcessEvent(&q.e);
    }
}

// ---------------------------------------------------------------- launcher window
static SDL_Window* s_launcher = nullptr;

void* UiWindow_Create(const char* title, int w, int h)
{
    // Test aid: SCR_UI_SIZE=WxH (legacy BKR_*) overrides launcher size for screenshots.
    if (const char* size = EnvGet("SCR_UI_SIZE", "BKR_UI_SIZE"))
    {
        int tw = 0, th = 0;
        if (sscanf(size, "%dx%d", &tw, &th) == 2 && tw >= 320 && th >= 240)
        {
            w = tw;
            h = th;
        }
    }
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable))
    {
        w = std::min(w, usable.w * 9 / 10);
        h = std::min(h, usable.h * 9 / 10);
    }
    s_launcher = SDL_CreateWindow(title, w, h, SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!s_launcher) LOG_WARN("launcher: SDL_CreateWindow failed ({})", SDL_GetError());
    else
    {
        // Keep chrome + Music Library usable; smaller sizes scroll inside tab pages.
        SDL_SetWindowMinimumSize(s_launcher, 800, 560);
    }
    return s_launcher;
}

void UiWindow_Show() { SDL_ShowWindow(s_launcher); }

bool UiWindow_Pump(uint32_t& resizeW, uint32_t& resizeH)
{
    resizeW = resizeH = 0;
    bool open = true;
    SDL_Event e;
    while (SDL_PollEvent(&e))
    {
        Input_HandleEvent(e);
        if (ImGui::GetCurrentContext()) ImGui_ImplSDL3_ProcessEvent(&e);
        if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) open = false;
        if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
        {
            resizeW = (uint32_t)std::max(1, e.window.data1);
            resizeH = (uint32_t)std::max(1, e.window.data2);
        }
    }
    // Always report the current drawable size so the swapchain can catch up even if a
    // PIXEL_SIZE_CHANGED event was coalesced or missed (common under Wayland live-resize).
    if (s_launcher)
    {
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(s_launcher, &pw, &ph);
        if (pw > 0 && ph > 0)
        {
            resizeW = (uint32_t)pw;
            resizeH = (uint32_t)ph;
        }
    }
    return open;
}

bool UiWindow_GetPixelSize(uint32_t& outW, uint32_t& outH)
{
    outW = outH = 0;
    if (!s_launcher) return false;
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(s_launcher, &pw, &ph);
    if (pw <= 0 || ph <= 0) return false;
    outW = (uint32_t)pw;
    outH = (uint32_t)ph;
    return true;
}

void UiWindow_Destroy()
{
    if (s_launcher) SDL_DestroyWindow(s_launcher);
    s_launcher = nullptr;
}

// ---------------------------------------------------------------- the desktop
// SDL_OpenURL runs xdg-open (or the desktop portal)
void Shell_OpenUrl(const std::string& url)
{
    if (!SDL_OpenURL(url.c_str())) LOG_WARN("cannot open {} ({})", url, SDL_GetError());
}

void Shell_OpenFolder(const std::filesystem::path& dir)
{
    std::string url = "file://";
    for (unsigned char c : std::filesystem::absolute(dir).string())
        if (isalnum(c) || strchr("/-._~", c)) url += (char)c;
        else url += std::format("%{:02X}", c);
    Shell_OpenUrl(url);
}

// ---------------------------------------------------------------- installer dialogs
namespace
{
struct PickState
{
    std::atomic<bool> done{ false };
    bool ok = false;
    std::string path, error;
};

void SDLCALL OnPicked(void* userdata, const char* const* files, int)
{
    PickState* st = (PickState*)userdata;
    if (!files) st->error = SDL_GetError();
    else if (files[0])
    {
        st->path = files[0];
        st->ok = true;
    }
    st->done = true;
}
}   // namespace

static bool Dialog_PickFile(std::filesystem::path& out, const SDL_DialogFileFilter* filters, int nfilters,
                            const char* title)
{
    PickState st;
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, (void*)filters);
    SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, nfilters);
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, s_launcher);
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, title);
    if (const char* home = getenv("HOME")) SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, home);
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFILE, OnPicked, &st, props);
    while (!st.done)
    {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 50)) Input_HandleEvent(e);
    }
    SDL_DestroyProperties(props);
    if (!st.error.empty())
    {
        LOG_WARN("file dialog: {}", st.error);
        Platform_MessageBox("SonicColorsRecomp",
                            "No file dialog is available (it needs the desktop portal, xdg-desktop-portal, or zenity).",
                            MsgButtons::Ok, MsgIcon::Error);
        return false;
    }
    if (st.ok) out = st.path;
    return st.ok;
}

bool Dialog_PickDiscImage(std::filesystem::path& out)
{
    static const SDL_DialogFileFilter filters[] = {
        { "Wii disc images", "iso;rvz;wia;wbfs;ciso;gcz;nfs" },
        { "All files", "*" },
    };
    return Dialog_PickFile(out, filters, 2, "Select your Sonic Colors disc image (USA, Europe or Japan)");
}

bool Dialog_PickWavFile(std::filesystem::path& out)
{
    static const SDL_DialogFileFilter filters[] = {
        { "WAV audio", "wav" },
        { "All files", "*" },
    };
    return Dialog_PickFile(out, filters, 2, "Select a WAV music file");
}

bool Dialog_SaveWavFile(std::filesystem::path& out, const std::string& suggestedName)
{
    static const SDL_DialogFileFilter filters[] = {
        { "WAV audio", "wav" },
        { "All files", "*" },
    };
    PickState st;
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, (void*)filters);
    SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 2);
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, s_launcher);
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Export original music as WAV");
    // LOCATION may be a folder or a full default file path.
    std::string location;
    if (const char* home = getenv("HOME"))
    {
        std::filesystem::path dir = std::filesystem::path(home) / "Music";
        if (!std::filesystem::exists(dir)) dir = home;
        location = suggestedName.empty() ? dir.string() : (dir / suggestedName).string();
    }
    else if (!suggestedName.empty())
        location = suggestedName;
    if (!location.empty())
        SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, location.c_str());
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_SAVEFILE, OnPicked, &st, props);
    while (!st.done)
    {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 50)) Input_HandleEvent(e);
    }
    SDL_DestroyProperties(props);
    if (!st.error.empty())
    {
        LOG_WARN("save dialog: {}", st.error);
        return false;
    }
    if (!st.ok) return false;
    out = st.path;
    if (out.extension().empty())
        out += ".wav";
    return true;
}

// A small window with two lines of text, a progress bar and a Cancel button.
namespace
{
struct Progress
{
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    std::string line1, line2;
    bool cancel = false;
    SDL_FRect button{};
};
constexpr int kW = 560, kH = 170;
}   // namespace

ProgressDialog::ProgressDialog(const std::string& title, const std::string& line1, const std::string& line2) : impl(new Progress())
{
    Progress* p = (Progress*)impl;
    p->line1 = line1;
    p->line2 = line2.size() > 64 ? "..." + line2.substr(line2.size() - 61) : line2;
    if (!SDL_CreateWindowAndRenderer(title.c_str(), kW, kH, 0, &p->window, &p->renderer))
        LOG_WARN("progress window: {}", SDL_GetError());
    p->button = { kW - 110.0f, kH - 46.0f, 90.0f, 30.0f };
}

ProgressDialog::~ProgressDialog()
{
    Progress* p = (Progress*)impl;
    if (p->renderer) SDL_DestroyRenderer(p->renderer);
    if (p->window) SDL_DestroyWindow(p->window);
    delete p;
}

bool ProgressDialog::Update(uint64_t done, uint64_t total)
{
    Progress* p = (Progress*)impl;
    SDL_Event e;
    while (SDL_PollEvent(&e))
    {
        Input_HandleEvent(e);
        if (!p->window || SDL_GetWindowFromEvent(&e) != p->window) continue;
        if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) p->cancel = true;
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) p->cancel = true;
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
        {
            SDL_FPoint pt{ e.button.x, e.button.y };
            if (SDL_PointInRectFloat(&pt, &p->button)) p->cancel = true;
        }
    }
    if (p->renderer)
    {
        SDL_Renderer* r = p->renderer;
        SDL_SetRenderDrawColor(r, 16, 16, 14, 255);
        SDL_RenderClear(r);
        SDL_SetRenderScale(r, 1.5f, 1.5f);
        SDL_SetRenderDrawColor(r, 230, 230, 225, 255);
        SDL_RenderDebugText(r, 14, 14, p->line1.c_str());
        SDL_SetRenderDrawColor(r, 150, 150, 145, 255);
        SDL_RenderDebugText(r, 14, 30, p->line2.c_str());
        SDL_SetRenderScale(r, 1.0f, 1.0f);
        SDL_FRect bar{ 20, 76, kW - 40.0f, 22 };
        SDL_SetRenderDrawColor(r, 60, 60, 58, 255);
        SDL_RenderFillRect(r, &bar);
        bar.w *= total ? (float)((double)done / total) : 0.0f;
        SDL_SetRenderDrawColor(r, 40, 110, 230, 255);
        SDL_RenderFillRect(r, &bar);
        SDL_SetRenderDrawColor(r, 80, 80, 78, 255);
        SDL_RenderFillRect(r, &p->button);
        SDL_SetRenderDrawColor(r, 230, 230, 225, 255);
        SDL_RenderDebugText(r, p->button.x + 21, p->button.y + 11, p->cancel ? "...." : "Cancel");
        SDL_RenderPresent(r);
    }
    return p->cancel;
}
