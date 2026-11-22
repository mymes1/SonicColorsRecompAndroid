// Launcher shown on every start: game data status (install / repair from the disc image,
// full file check), settings, and Play. Its own small window and D3D11 device; both are
// gone before the game creates its window.
#include "settings_ui.h"
#include "music_ui.h"
#include "ui_image.h"
#include "ui_shot.h"
#include "update_check.h"
#include "../audio/audio_out.h"
#include "../config.h"
#include "../mods/mods.h"
#include "../env.h"
#include "../install/releases.h"
#include "../install/verify.h"
#include "../log.h"
#include "../platform/input.h"
#include "../platform/platform.h"
#include "../platform/ui.h"
#include "../region.h"
#include "../version.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <atomic>
#include <d3d11.h>
#include <dxgi.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#ifndef __ANDROID__
#include <format>   // on Android, platform/compat.h provides it
#endif
#include <string>
#include <string_view>
#include <thread>

namespace fs = std::filesystem;

bool InstallUI_Run(const fs::path& dest, const std::string& foundImage, const std::string& damage);

namespace
{
// test aid: SCR_UI_SHOT_KEYS (legacy BKR_*) = ImGui key names, comma-separated
// (e.g. "DownArrow,Space" or "GamepadDpadDown,GamepadFaceDown") presses those keys
// one after the other before the screenshot (from SCR_UI_SHOT_KEYS_DELAY seconds, default 1)
void ShotKeys()
{
    static std::vector<ImGuiKey> keys;
    static uint64_t start = 0;
    if (!start)
    {
        const char* delay = EnvGet("SCR_UI_SHOT_KEYS_DELAY", "BKR_UI_SHOT_KEYS_DELAY");
        start = Platform_TickMs() + (delay ? atoi(delay) : 1) * 1000ull;
        if (const char* list = EnvGet("SCR_UI_SHOT_KEYS", "BKR_UI_SHOT_KEYS"))
        {
            std::string name;
            for (const char* c = list;; c++)
            {
                if (*c && *c != ',') { name += *c; continue; }
                for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++)
                    if (name == ImGui::GetKeyName((ImGuiKey)k)) keys.push_back((ImGuiKey)k);
                name.clear();
                if (!*c) break;
            }
        }
    }
    ImGuiIO& io = ImGui::GetIO();
    if (keys.empty() || Platform_TickMs() < start) return;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    // one key every 200 ms, held for 50 ms
    static size_t next = 0;
    static bool down = false;
    static ULONGLONG at = 0;
    ULONGLONG now = Platform_TickMs();
    if (next >= keys.size() || now < at) return;
    io.AddKeyEvent(keys[next], !down);
    if (down) next++;
    at = now + (down ? 150 : 50);
    down = !down;
}

ID3D11Device* s_dev = nullptr;
ID3D11DeviceContext* s_ctx = nullptr;
IDXGISwapChain* s_swap = nullptr;
ID3D11RenderTargetView* s_rtv = nullptr;
bool s_quit = false;

void CreateRTV()
{
    ID3D11Texture2D* bb = nullptr;
    s_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    s_dev->CreateRenderTargetView(bb, nullptr, &s_rtv);
    bb->Release();
}

bool SwapchainPixelSize(uint32_t& outW, uint32_t& outH)
{
    outW = outH = 0;
    if (!s_swap) return false;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(s_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb)
        return false;
    D3D11_TEXTURE2D_DESC d{};
    bb->GetDesc(&d);
    bb->Release();
    outW = d.Width;
    outH = d.Height;
    return outW > 0 && outH > 0;
}

// Keep the DXGI backbuffer in lock-step with the window's drawable pixels.
// Missing this on Wayland/high-DPI leaves ImGui rendering at a stale resolution while
// the compositor stretches the previous buffer → persistent blur after resize.
void SyncSwapchainToWindow(uint32_t wantW, uint32_t wantH)
{
    if (!s_swap || wantW == 0 || wantH == 0) return;
    uint32_t curW = 0, curH = 0;
    if (SwapchainPixelSize(curW, curH) && curW == wantW && curH == wantH)
        return;
    if (s_rtv) { s_rtv->Release(); s_rtv = nullptr; }
    // Passing explicit pixel size (not 0,0) so DXVK/SDL high-DPI surfaces match ImGui's
    // DisplaySize * DisplayFramebufferScale framebuffer.
    HRESULT hr = s_swap->ResizeBuffers(0, wantW, wantH, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr))
        LOG_WARN("launcher: ResizeBuffers {}x{} failed {:08x}", wantW, wantH, (uint32_t)hr);
    CreateRTV();
}

struct State
{
    fs::path data;
    DataStatus status = DataStatus::Missing;
    std::string problem;
    std::string release;            // "USA (RENE8P)" once installed
    std::string releaseError;       // this build cannot play the installed release
    bool promptInstall = false;     // ask for the disc image after the window is shown
    std::thread verifyThread;
    VerifyProgress verify;
    std::atomic<bool> verifying{ false };
    std::string verifyResult;
};

void Check(State& st)
{
    st.status = Install_QuickCheck(st.data, st.problem);
    st.release.clear();
    st.releaseError.clear();
    if (st.status == DataStatus::Ok)
    {
        std::string id = Release_InstalledId(st.data);
        if (const GameRelease* r = Release_Find(id)) st.release = std::string(r->name) + " (" + id + ")";
        Region_Select(id, st.releaseError);
        SettingsUI_SetRelease(id, st.data.string());
    }
    LOG_INFO("launcher: game data {} {}", st.data.string(),
             st.status == DataStatus::Ok ? "ok" : st.status == DataStatus::Missing ? "not installed" : "damaged: " + st.problem);
}

void StartVerify(State& st)
{
    if (st.verifying) return;
    if (st.verifyThread.joinable()) st.verifyThread.join();
    st.verify.cancel = false;
    st.verifying = true;
    st.verifyResult.clear();
    st.verifyThread = std::thread([&st] {
        std::string problem;
        DataStatus r = Install_FullCheck(st.data, st.verify, problem);
        if (st.verify.cancel) st.verifyResult = "Check canceled.";
        else if (r == DataStatus::Ok) st.verifyResult = "All game files are intact.";
        else
        {
            st.verifyResult = "Damaged: " + problem;
            st.status = r;
            st.problem = problem;
        }
        st.verifying = false;
    });
}

void DrawHome(State& st, bool& play, const std::string& isoPath, const fs::path& installDest)
{
    UpdateCheck_Start(false);   // one background tag check per launcher session
    ImGui::Spacing();
    {
        static bool versionInit = false;
        if (!versionInit)
        {
            versionInit = true;
            if (g_config.lastSeenVersion.empty())
            {
                g_config.lastSeenVersion = g_version;
                Config_Save();
            }
        }
        if (g_config.lastSeenVersion != g_version)
        {
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.95f, 1.f), "Updated to %s", g_version);
            ImGui::TextDisabled("Previously running %s", g_config.lastSeenVersion.c_str());
            if (ImGui::SmallButton("Dismiss##whatsnew"))
            {
                g_config.lastSeenVersion = g_version;
                Config_Save();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Open Releases##whatsnew"))
                UpdateCheck_OpenReleases();
            ImGui::Spacing();
        }
    }
    {
        static std::string abnormal;
        static bool abnormalChecked = false;
        if (!abnormalChecked)
        {
            abnormalChecked = true;
            Log_TakeAbnormalExit(abnormal);
        }
        if (!abnormal.empty())
        {
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.f), "Previous session ended abnormally.");
            ImGui::TextWrapped("%s", abnormal.c_str());
            if (ImGui::SmallButton("Open log folder##crash"))
            {
                const std::string& logPath = Log_Path();
                if (!logPath.empty())
                {
                    fs::path p(logPath);
                    Shell_OpenFolder(p.has_parent_path() ? p.parent_path() : p);
                }
                else
                    Shell_OpenFolder(Platform_UserDir());
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Save diagnostic report##crash"))
            {
                std::string err;
                const std::string path = SettingsUI_WriteDiagnosticReport(err);
                if (!path.empty())
                {
                    ImGui::SetClipboardText(path.c_str());
                    fs::path p(path);
                    Shell_OpenFolder(p.has_parent_path() ? p.parent_path() : p);
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Dismiss##crash"))
                abnormal.clear();
            ImGui::TextDisabled("Crash details are usually in SonicColorsRecomp.1.log after the restart.");
            ImGui::Spacing();
        }
    }
    if (UpdateCheck_NewerAvailable())
    {
        ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.35f, 1.f), "%s", UpdateCheck_Message().c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Open Releases"))
            UpdateCheck_OpenReleases();
        ImGui::BeginDisabled(!UpdateCheck_CanDownload() || UpdateCheck_DownloadBusy());
        if (ImGui::SmallButton("Download package"))
            UpdateCheck_StartDownload();
        ImGui::EndDisabled();
        const std::string notes = UpdateCheck_Notes();
        if (!notes.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.75f, 0.70f, 1.f));
            ImGui::TextWrapped("%s", notes.c_str());
            ImGui::PopStyleColor();
        }
        if (UpdateCheck_DownloadBusy() || UpdateCheck_DownloadDone())
        {
            const float p = UpdateCheck_DownloadProgress();
            if (p >= 0.f) ImGui::ProgressBar(p, ImVec2(-1, 0));
            const std::string dl = UpdateCheck_DownloadMessage();
            if (!dl.empty()) ImGui::TextWrapped("%s", dl.c_str());
            if (UpdateCheck_DownloadOk())
            {
                if (UpdateCheck_DownloadedPackageIsAppImage())
                {
                    if (ImGui::SmallButton("Open AppImage"))
                        UpdateCheck_OpenDownloadedPackage();
                }
                else if (ImGui::SmallButton("Reveal package"))
                    UpdateCheck_OpenDownloadedPackage();
                ImGui::SameLine();
                if (ImGui::SmallButton("Open downloads folder"))
                    UpdateCheck_OpenDownloadFolder();
                if (UpdateCheck_CanApplyUpdate())
                {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Install & relaunch"))
                        ImGui::OpenPopup("Install update?");
                }
                if (ImGui::BeginPopupModal("Install update?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
                {
                    ImGui::TextWrapped(
                        "Replace this install with the downloaded package and relaunch?\n\n"
                        "AppImage: overwrites the running file, then restarts.\n"
                        "Windows zip: a helper waits for exit, extracts, installs, then restarts.\n\n"
                        "Config, saves, mods, and other user data are kept.");
                    static std::string applyErr;
                    if (ImGui::Button("Install & relaunch", ImVec2(160, 0)))
                    {
                        if (!UpdateCheck_ApplyUpdateAndRelaunch(applyErr) && !applyErr.empty())
                            /* stay open with error */;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel", ImVec2(100, 0)))
                    {
                        applyErr.clear();
                        ImGui::CloseCurrentPopup();
                    }
                    if (!applyErr.empty())
                        ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.35f, 1.f), "%s", applyErr.c_str());
                    ImGui::EndPopup();
                }
            }
        }
        else
            ImGui::TextDisabled("Downloads to user_data/downloads/ — open the package, then quit this build.");
        ImGui::Spacing();
    }
    const ImVec4 green(0.45f, 0.80f, 0.40f, 1), red(0.90f, 0.40f, 0.30f, 1);
    switch (st.status)
    {
    case DataStatus::Ok: ImGui::TextColored(green, "Game data installed: %s", st.release.c_str()); break;
    case DataStatus::Missing: ImGui::TextColored(red, "Game data not installed"); break;
    case DataStatus::Corrupt: ImGui::TextColored(red, "Game data damaged: %s", st.problem.c_str()); break;
    }
    if (!g_config.lastPlayedAt.empty())
        ImGui::TextDisabled("Last played: %s", g_config.lastPlayedAt.c_str());
    if (!st.releaseError.empty()) ImGui::TextColored(red, "%s", st.releaseError.c_str());
    ImGui::TextDisabled("%s", st.data.string().c_str());
    {
        const fs::path user = Platform_UserDir();
        const bool portable = Platform_IsPortable();
        if (portable)
            ImGui::TextDisabled("Portable mode · user data next to the executable");
        else
            ImGui::TextDisabled("User data: %s", user.string().c_str());
    }
    if (st.status == DataStatus::Missing)
    {
        ImGui::TextWrapped("Install Sonic Colors from your own Wii disc image to play. "
                           "Supported: %s. Formats: ISO, RVZ, WIA, WBFS, GCZ. "
                           "USA (SNCE8P) and Japan (SNCJ8P) are not supported yet.",
                           kReleaseNames);
        ImGui::Spacing();
        ImGui::BulletText("Click Install from disc image… below (or use a nearby ISO if found).");
        ImGui::BulletText("Wait for extraction to finish, then press Play.");
        ImGui::BulletText("Optional: open Settings for graphics, audio, and controller dead zones.");
        if (!isoPath.empty())
            ImGui::TextDisabled("Disc image ready: %s", isoPath.c_str());
        else
            ImGui::TextDisabled("No disc image found nearby — you will be asked to pick one (ISO/RVZ/WIA/WBFS/GCZ).");
        {
            std::error_code ec;
            fs::path probe = installDest.has_parent_path() ? installDest.parent_path() : installDest;
            while (!probe.empty() && !fs::exists(probe, ec))
            {
                if (!probe.has_parent_path() || probe.parent_path() == probe) break;
                probe = probe.parent_path();
            }
            const fs::space_info si = fs::space(probe.empty() ? fs::path(".") : probe, ec);
            if (!ec)
            {
                const double gb = (double)si.available / (1024.0 * 1024.0 * 1024.0);
                if (si.available < (4ull << 30))
                    ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.35f, 1.f),
                                       "Free space at install location: %.1f GB (under 4 GB — install may fail).",
                                       gb);
                else
                    ImGui::TextDisabled("Free space at install location: %.1f GB.", gb);
            }
        }
        ImGui::Spacing();
        if (ImGui::Button("Open Releases page##missing"))
            Shell_OpenUrl("https://github.com/VoltTMC/SonicColorsRecomp/releases");
        ImGui::TextDisabled("Need a newer build of this recomp? Releases has packages (not the Wii disc).");
    }
    else if (st.status == DataStatus::Corrupt)
    {
        ImGui::TextWrapped("Use \"Reinstall from disc image...\" below (preferred) or \"Check all game files\". "
                           "Supported release: %s. A wit EXTRACT folder also works if main.dol matches.",
                           kReleaseNames);
        ImGui::Spacing();
        if (ImGui::Button("Open log folder"))
        {
            const std::string& logPath = Log_Path();
            if (!logPath.empty())
            {
                fs::path p(logPath);
                Shell_OpenFolder(p.has_parent_path() ? p.parent_path() : p);
            }
            else
                Shell_OpenFolder(Platform_UserDir());
        }
        ImGui::SameLine();
        if (ImGui::Button("Open Releases page"))
            Shell_OpenUrl("https://github.com/VoltTMC/SonicColorsRecomp/releases");
        ImGui::TextDisabled("Logs help diagnose a bad install; Releases has the latest builds.");
    }
    if (st.status == DataStatus::Ok)
    {
        // mods load on top of the installed files; the file checks only look at the latter
        std::vector<std::string> mods = SettingsUI_ActiveMods();
        std::string list;
        for (auto& m : mods) list += (list.empty() ? "" : ", ") + m;
        if (mods.empty()) ImGui::TextDisabled("Mods: none (see the Mods tab)");
        else ImGui::TextWrapped("Mods active (%d): %s", (int)mods.size(), list.c_str());
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Current settings");
    {
        const char* fps =
            g_config.frameRate == 30 ? "30 FPS" : g_config.frameRate == 60 ? "60 FPS" : "Uncapped";
        ImGui::Text("Display: %s · internal %dx · %s · UI %.0f%%", fps, g_config.internalScale,
                    g_config.vsync ? "VSync on" : "VSync off", g_config.uiScale * 100.0f);
        const std::string& dev = AudioOut_CurrentDeviceName();
        const char* bak =
            g_config.autoBackupSavesOnLaunch && g_config.autoBackupSavesOnWrite ? " · save backup on Play+write"
            : g_config.autoBackupSavesOnLaunch                                 ? " · save backup on Play"
            : g_config.autoBackupSavesOnWrite                                  ? " · save backup on write"
                                                                               : "";
        ImGui::Text("Audio: %s · master %.0f%%%s",
                    dev.empty() ? "system default" : dev.c_str(), g_config.masterVolume * 100.0f, bak);
        const int pads = Pad_Count();
        if (pads <= 0)
            ImGui::TextDisabled("Controllers: none connected");
        else
        {
            ImGui::Text("Controllers: %d", pads);
            for (int i = 0; i < 4; i++)
                if (const char* n = Pad_Name(i))
                    ImGui::BulletText("P%d: %s", i + 1, n);
        }
        ImGui::TextDisabled("Change these under Settings. Dead zones and FPS overlay are there too.");
    }
    ImGui::Spacing();

    bool ready = st.status == DataStatus::Ok && st.releaseError.empty() && !st.verifying;
    ImGui::BeginDisabled(!ready);
    if (ImGui::Button("  Play  ", ImVec2(ImGui::GetFontSize() * 10, ImGui::GetFontSize() * 2.4f)))
    {
        Config_StampLastPlayed();
        play = true;
    }
    ImGui::EndDisabled();
    if (!ready && st.status != DataStatus::Ok && !st.verifying)
        ImGui::TextDisabled("Play is unavailable until game data is installed.");
    else if (!ready && !st.releaseError.empty())
        ImGui::TextDisabled("Play is unavailable: this build cannot use the installed release.");
    if (ready && !ImGui::IsAnyItemActive() && ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);

    ImGui::Spacing();
    ImGui::SeparatorText("Game data");
    ImGui::BeginDisabled(st.verifying);
    if (ImGui::Button(st.status == DataStatus::Ok ? "Reinstall from disc image..." : "Install from disc image..."))
    {
        if (st.status == DataStatus::Ok)
            ImGui::OpenPopup("Reinstall game data?");
        else
        {
            fs::path dest = st.status == DataStatus::Missing ? installDest : st.data;
            if (InstallUI_Run(dest, isoPath, st.status == DataStatus::Corrupt ? st.problem : ""))
                st.data = dest;
            Check(st);
        }
    }
    if (ImGui::BeginPopupModal("Reinstall game data?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped(
            "This replaces the installed game files from your disc image. Saves, mods, and settings are not touched. Continue?");
        if (ImGui::Button("Reinstall", ImVec2(120, 0)))
        {
            ImGui::CloseCurrentPopup();
            fs::path dest = st.data.empty() ? installDest : st.data;
            if (InstallUI_Run(dest, isoPath, ""))
                st.data = dest;
            Check(st);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(st.status == DataStatus::Missing);
    if (ImGui::Button("Check all game files")) StartVerify(st);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (st.verifying)
    {
        uint64_t total = st.verify.total ? st.verify.total.load() : 1;
        ImGui::ProgressBar((float)((double)st.verify.done / total), ImVec2(ImGui::GetFontSize() * 20, 0), "Checking...");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) st.verify.cancel = true;
    }
    else if (!st.verifyResult.empty())
        ImGui::TextWrapped("%s", st.verifyResult.c_str());
    ImGui::TextDisabled("Install reads your own disc image; it is not needed afterwards.");
    ImGui::TextDisabled("File checks look at the installed files only; mods load on top of them.");

    ImGui::Spacing();
    ImGui::SeparatorText("Quick folders");
    ImGui::BeginDisabled(st.status == DataStatus::Missing);
    if (ImGui::Button("Open game data"))
    {
        std::error_code ec;
        fs::create_directories(st.data, ec);
        Shell_OpenFolder(st.data);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(st.status == DataStatus::Missing);
    if (ImGui::SmallButton("Copy##gamedata"))
        ImGui::SetClipboardText(st.data.string().c_str());
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Open user data"))
        Shell_OpenFolder(Platform_UserDir());
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy##userdata"))
        ImGui::SetClipboardText(Platform_UserDir().string().c_str());
    ImGui::SameLine();
    if (ImGui::Button("Open mods"))
    {
        fs::path mods = Mods_MainDir(g_config.modsDir);
        std::error_code ec;
        fs::create_directories(mods, ec);
        Shell_OpenFolder(mods);
    }
    ImGui::SameLine();
    if (ImGui::Button("Open screenshots"))
    {
        fs::path shots = Platform_UserDir() / "screenshots";
        std::error_code ec;
        fs::create_directories(shots, ec);
        Shell_OpenFolder(shots);
    }
    ImGui::SameLine();
    if (ImGui::Button("Open downloads"))
        UpdateCheck_OpenDownloadFolder();
    ImGui::TextDisabled("Game data, config/saves/logs, mods, F12 screenshots, and update packages. Copy pastes the path.");

    ImGui::Spacing();
    ImGui::SeparatorText("Controls");
    ImGui::TextDisabled("In game: View (Back) or %s opens the settings. View + Start is the Wii HOME button.",
                        Config_HotkeyName(g_config.hotkeySettings, "F1").c_str());
    ImGui::Spacing();
    ImGui::TextDisabled("%s", g_version);
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy version"))
        ImGui::SetClipboardText(g_version);
}
}   // namespace

// Returns true to start the game, with dataDir set to the installed data.
bool Launcher_Run(std::string& dataDir, const std::string& isoPath, const fs::path& installDest)
{
    // room for the mod browser (within the screen)
    const int launchW = std::clamp(g_config.launcherWidth, 800, 3840);
    const int launchH = std::clamp(g_config.launcherHeight, 560, 2160);
    const std::string launchTitle = std::format("Sonic Colors - Recompiled ({})", std::string_view{g_version});
    HWND hwnd = (HWND)UiWindow_Create(launchTitle.c_str(), launchW, launchH);
    if (!hwnd)
    {
        // UI screenshot / automated GUI tests must not fall through into the game.
        if (EnvHas("SCR_UI_SHOT", "BKR_UI_SHOT"))
        {
            LOG_WARN("launcher: no window (SCR_UI_SHOT) — aborting without starting the game");
            return false;
        }
        LOG_WARN("launcher: no window, starting the game directly");
        return true;
    }
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd,
                                             &s_swap, &s_dev, nullptr, &s_ctx)))
    {
        if (EnvHas("SCR_UI_SHOT", "BKR_UI_SHOT"))
        {
            LOG_WARN("launcher: no D3D11 (SCR_UI_SHOT) — aborting without starting the game");
            UiWindow_Destroy();
            return false;
        }
        LOG_WARN("launcher: no D3D11 device, starting the game directly");
        UiWindow_Destroy();
        return true;
    }
    CreateRTV();
    UiWindow_Show();

    IMGUI_CHECKVERSION();
    ImGuiContext* imgui = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    std::string font = Platform_UiFont();
    if (!font.empty()) io.Fonts->AddFontFromFileTTF(font.c_str(), 18.0f);
    SettingsUI_AddFallbackFonts();
    SettingsUI_ApplyStyle();
    UiBackend_Init(hwnd);
    ImGui_ImplDX11_Init(s_dev, s_ctx);
    UiImage_SetDevice(s_dev);

    State st;
    st.data = dataDir.empty() ? installDest : fs::path(dataDir);
    Check(st);
    // test aid: SCR_UI_SHOT=<file.bmp> (legacy BKR_*) saves the launcher
    // (SCR_UI_SHOT_TAB=settings|mods|music) and quits; SCR_UI_SHOT_WAIT=<seconds> waits first
    const char* shot = EnvGet("SCR_UI_SHOT", "BKR_UI_SHOT");
    const char* shotWait = EnvGet("SCR_UI_SHOT_WAIT", "BKR_UI_SHOT_WAIT");
    ULONGLONG shotAt = Platform_TickMs() + (shotWait ? atoi(shotWait) : 0) * 1000ull;
    const char* shotTab = EnvGet("SCR_UI_SHOT_TAB", "BKR_UI_SHOT_TAB");
    st.promptInstall = st.status != DataStatus::Ok && !shot;

    bool play = false;
    s_quit = false;
    bool quitConfirm = false;
    int frames = 0;
    auto requestQuit = [&] {
        if (st.verifying)
            quitConfirm = true;
        else
            s_quit = true;
    };
    while (!s_quit && !play)
    {
        uint32_t resizeW, resizeH;
        if (!UiWindow_Pump(resizeW, resizeH)) requestQuit();
        if (s_quit) break;
        // Sync every frame (Pump now reports current pixel size continuously).
        if (resizeW && resizeH)
            SyncSwapchainToWindow(resizeW, resizeH);
        // first start, deleted or damaged data: ask for the disc image once the window is up
        if (st.promptInstall && frames++ > 2)
        {
            st.promptInstall = false;
            fs::path dest = st.status == DataStatus::Missing ? installDest : st.data;
            if (InstallUI_Run(dest, isoPath, st.status == DataStatus::Corrupt ? st.problem : "")) st.data = dest;
            Check(st);
        }

        ImGui_ImplDX11_NewFrame();
        UiBackend_NewFrame();
        // Match font raster density to the drawable scale so text stays sharp after DPI/resize.
        {
            float dpi = std::max(io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
            if (dpi < 0.5f) dpi = 1.f;
            ImGui::GetStyle().FontScaleDpi = 1.f; // layout stays in logical px; density via framebuffer
            ImGui::GetStyle().FontScaleMain = std::clamp(g_config.uiScale, 0.75f, 1.50f);
            // Ensure swapchain matches what ImGui will actually draw into this frame.
            uint32_t drawW = (uint32_t)std::lround(io.DisplaySize.x * io.DisplayFramebufferScale.x);
            uint32_t drawH = (uint32_t)std::lround(io.DisplaySize.y * io.DisplayFramebufferScale.y);
            if (drawW > 0 && drawH > 0)
                SyncSwapchainToWindow(drawW, drawH);
        }
        if (shot) ShotKeys();
        if (shot)
        {
            // screenshots run minimized (no client area): lay out for the back buffer
            uint32_t bw = 0, bh = 0;
            if (SwapchainPixelSize(bw, bh))
            {
                io.DisplaySize = ImVec2((float)bw, (float)bh);
                io.DisplayFramebufferScale = ImVec2(1.f, 1.f);
            }
        }
        ImGui::NewFrame();
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::Begin("launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.6f);
        ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "Sonic Colors");
        ImGui::PopFont();
        ImGui::TextDisabled("Recompiled for PC");
        // Reserve footer (Quit + version) without going negative on tiny windows.
        float bottom = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2
                       + ImGui::GetTextLineHeightWithSpacing();
        float tabsH = ImGui::GetContentRegionAvail().y - bottom;
        if (tabsH < ImGui::GetFrameHeightWithSpacing() * 4)
            tabsH = ImGui::GetFrameHeightWithSpacing() * 4;
        ImGui::BeginChild("tabs", ImVec2(0, tabsH), ImGuiChildFlags_NavFlattened);
        if (ImGui::BeginTabBar("launcherTabs"))
        {
            static int selected = -1;
            if (selected < 0)
                selected = std::clamp(g_config.launcherTab, 0, 3);
            int pick = SettingsUI_PadTab(selected, 4, true);
            auto remember = [&](int tab) {
                if (selected == 3 && tab != 3) MusicUI_Shutdown();
                if (selected != tab)
                {
                    selected = tab;
                    g_config.launcherTab = tab;
                    Config_Save();
                }
            };
            // Only force the remembered/shot tab for the first two frames. `frames` is not a
            // reliable boot counter (it only advances on promptInstall / SCR_UI_SHOT), so a
            // stuck force would re-assert ImGuiTabItemFlags_SetSelected every frame and cancel
            // mouse clicks on other tabs.
            static int bootFrames = 0;
            const bool forceTab = bootFrames < 2;
            if (bootFrames < 2) bootFrames++;
            auto force = [&](int tab, const char* shotName) {
                if (shotTab && shotName && !strcmp(shotTab, shotName)) return forceTab;
                return forceTab && !shotTab && selected == tab;
            };
            if (SettingsUI_TabItem("Play", 0, pick, force(0, nullptr)))
            {
                remember(0);
                DrawHome(st, play, isoPath, installDest);
                ImGui::EndTabItem();
            }
            if (SettingsUI_TabItem("Settings", 1, pick, force(1, "settings")))
            {
                remember(1);
                SettingsUI_Draw(SettingsHost::Launcher);
                ImGui::EndTabItem();
            }
            if (SettingsUI_TabItem("Mods", 2, pick, force(2, "mods")))
            {
                remember(2);
                SettingsUI_DrawMods(SettingsHost::Launcher);
                ImGui::EndTabItem();
            }
            if (SettingsUI_TabItem("Music", 3, pick, force(3, "music")))
            {
                remember(3);
                SettingsUI_DrawMusic(SettingsHost::Launcher);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
        if (ImGui::Button("Quit")) requestQuit();
        SettingsUI_DrawVersion();
        if (quitConfirm)
        {
            ImGui::OpenPopup("Quit while checking?");
            quitConfirm = false;
        }
        if (ImGui::BeginPopupModal("Quit while checking?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextWrapped("A game-file check is still running. Quit anyway?");
            if (ImGui::Button("Quit", ImVec2(120, 0)))
            {
                st.verify.cancel = true;
                s_quit = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Keep checking", ImVec2(140, 0)))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::End();

        ImGui::Render();
        const float clear[4] = { 0.06f, 0.06f, 0.05f, 1 };
        s_ctx->OMSetRenderTargets(1, &s_rtv, nullptr);
        s_ctx->ClearRenderTargetView(s_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (shot && ++frames > 5 && Platform_TickMs() >= shotAt)
        {
            UI_SaveBoundTarget(s_dev, s_ctx, shot);
            LOG_INFO("launcher: SCR_UI_SHOT saved to {} — quitting without starting the game", shot);
            s_quit = true;
        }
        s_swap->Present(1, 0);
    }

    st.verify.cancel = true;
    if (st.verifyThread.joinable()) st.verifyThread.join();
    MusicUI_Shutdown();
    {
        const int saveW = (int)ImGui::GetIO().DisplaySize.x;
        const int saveH = (int)ImGui::GetIO().DisplaySize.y;
        if (saveW >= 800 && saveH >= 560 &&
            (saveW != g_config.launcherWidth || saveH != g_config.launcherHeight))
        {
            g_config.launcherWidth = saveW;
            g_config.launcherHeight = saveH;
            Config_Save();
        }
    }
    UiImage_SetDevice(nullptr);
    ImGui_ImplDX11_Shutdown();
    UiBackend_Shutdown();
    ImGui::DestroyContext(imgui);
    if (s_rtv) s_rtv->Release();
    s_swap->Release();
    s_ctx->Release();
    s_dev->Release();
    s_rtv = nullptr;
    UiWindow_Destroy();
    if (play) dataDir = st.data.string();
    LOG_INFO("launcher: {}", play ? "play" : "quit");
    return play;
}
