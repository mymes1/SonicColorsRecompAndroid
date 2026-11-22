#include "settings_ui.h"
#include "music_ui.h"
#include "overlay.h"
#include "update_check.h"
#include "../platform/platform.h"
#include "../config.h"
#include "../audio/audio_out.h"
#include "../env.h"
#include "../gx/video.h"
#include "../perf.h"
#include "../input/gamepad.h"
#include "../ios/nand_paths.h"
#include "../log.h"
#include "mods_ui.h"
#include "../version.h"
#include "../platform/input.h"
#include "../platform/ui.h"

#include <chrono>
#include <deque>
#include <fstream>
#include <ostream>

#include <imgui.h>

#include <algorithm>
#include <filesystem>
#ifndef __ANDROID__
#include <format>   // on Android, platform/compat.h provides it
#endif
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

extern bool g_asyncShaders;
void Renderer_ResetSamplers();

namespace
{
bool s_restartRequested = false;
// keyboard/mouse rebinding: which binding waits for a key, and whether it adds or replaces
int s_capture = -1;
bool s_captureAdd = false;
bool s_captureArmed = false;   // all keys were released since "Set" was clicked
bool s_askRestart = false;          // open the "needs a restart" prompt next frame
const char* s_help = nullptr;      // description of the hovered setting

// Applies settings that can change while the game runs (called on the render thread).
void ApplyLive()
{
    g_videoConfig.vsync = g_config.vsync;
    g_videoConfig.widescreen = g_config.widescreen;
    g_videoConfig.integerScale = g_config.integerScale;
    g_asyncShaders = g_config.asyncShaders;
    // Re-apply fullscreen so exclusive↔borderless mode changes take effect live.
    Video_SetFullscreen(g_config.fullscreen);
    Video_SetWindowSize(g_config.windowWidth, g_config.windowHeight);
    Renderer_ResetSamplers();      // anisotropy
    Gamepad_ReloadBindings();
    AudioOut_ApplyVolumes(g_config.masterVolume, g_config.speakerVolume);
    AudioOut_SetDevice(g_config.audioDevice);
    // rumble, remote speaker and pause-on-focus-loss read g_config directly
}

// A setting changed: save it, then apply it live or (in game) offer a restart.
void Changed(SettingsHost host, bool needsRestart)
{
    Config_Save();
    if (host != SettingsHost::InGame) return;
    if (needsRestart) s_askRestart = true;
    else ApplyLive();
}

void Help(const char* text)
{
    if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) s_help = text;
}

const char* RestartTag(SettingsHost host, bool needsRestart)
{
    return host == SettingsHost::InGame && needsRestart ? " (restart)" : "";
}

bool Check(SettingsHost host, const char* label, bool& v, bool needsRestart, const char* help)
{
    std::string l = std::string(label) + RestartTag(host, needsRestart);
    bool changed = ImGui::Checkbox(l.c_str(), &v);
    Help(help);
    if (changed) Changed(host, needsRestart);
    return changed;
}

template <typename T>
bool Combo(SettingsHost host, const char* label, T& value, const T* values, const char* const* names, int count,
           bool needsRestart, const char* help)
{
    int cur = 0;
    for (int i = 0; i < count; i++)
        if (values[i] == value) cur = i;
    std::string l = std::string(label) + RestartTag(host, needsRestart);
    bool changed = false;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    if (ImGui::BeginCombo(l.c_str(), names[cur]))
    {
        for (int i = 0; i < count; i++)
            if (ImGui::Selectable(names[i], i == cur) && i != cur)
            {
                value = values[i];
                changed = true;
            }
        ImGui::EndCombo();
    }
    Help(help);
    return changed;   // the caller saves (Changed) once the new value is in g_config
}

void VideoTab(SettingsHost host)
{
    Config& c = g_config;
    ImGui::SeparatorText("Quick presets");
    {
        // Match by the knobs a preset owns (does not touch frame rate / window size).
        auto isPerf = [&] {
            return c.internalScale == 2 && c.anisotropy == 1 && c.asyncShaders && c.gpuThread &&
                   !c.vsync && !c.integerScale;
        };
        auto isBalanced = [&] {
            return c.internalScale == 3 && c.anisotropy == 4 && c.asyncShaders && c.gpuThread &&
                   !c.vsync && !c.integerScale;
        };
        auto isQuality = [&] {
            return c.internalScale == 4 && c.anisotropy == 16 && c.asyncShaders && c.gpuThread &&
                   c.vsync && !c.integerScale;
        };
        int cur = isPerf() ? 0 : isBalanced() ? 1 : isQuality() ? 2 : 3;
        const char* names[] = { "Performance", "Balanced", "Quality", "Custom" };
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
        if (ImGui::BeginCombo("Graphics preset", names[cur]))
        {
            for (int i = 0; i < 3; i++)
            {
                if (ImGui::Selectable(names[i], i == cur) && i != cur)
                {
                    if (i == 0)
                    {
                        c.internalScale = 2;
                        c.anisotropy = 1;
                        c.asyncShaders = true;
                        c.gpuThread = true;
                        c.vsync = false;
                        c.integerScale = false;
                    }
                    else if (i == 1)
                    {
                        c.internalScale = 3;
                        c.anisotropy = 4;
                        c.asyncShaders = true;
                        c.gpuThread = true;
                        c.vsync = false;
                        c.integerScale = false;
                    }
                    else
                    {
                        c.internalScale = 4;
                        c.anisotropy = 16;
                        c.asyncShaders = true;
                        c.gpuThread = true;
                        c.vsync = true;
                        c.integerScale = false;
                    }
                    Changed(host, true); // resolution / vsync need a restart prompt path
                }
            }
            ImGui::EndCombo();
        }
        Help("Applies internal resolution, anisotropic filtering, VSync and related options. "
             "Frame rate and window size are unchanged. Further tweaks switch to Custom.");
    }

    ImGui::SeparatorText("Video profiles");
    Help("Save or load display/rendering sets under user_data/video_profiles/ (includes frame rate and window size).");
    {
        static char profileName[48] = "default";
        static int selected = 0;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
        ImGui::InputText("Profile name##video", profileName, sizeof(profileName));
        if (ImGui::Button("Save video profile"))
        {
            std::string path;
            if (Config_SaveVideoProfile(profileName, &path))
            {
                Overlay_Toast(std::format("Saved video profile {}", fs::path(path).stem().string()).c_str());
                Changed(host, false);
            }
            else
                Overlay_Toast("Could not save video profile");
        }
        ImGui::SameLine();
        if (ImGui::Button("Open profiles folder##video"))
            Shell_OpenFolder(Config_VideoProfilesDir());
        std::vector<std::string> profiles = Config_ListVideoProfiles();
        if (profiles.empty())
            ImGui::TextDisabled("No saved video profiles yet.");
        else
        {
            if (selected >= (int)profiles.size()) selected = 0;
            std::vector<const char*> items;
            items.reserve(profiles.size());
            for (auto& p : profiles) items.push_back(p.c_str());
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
            ImGui::Combo("Load video profile", &selected, items.data(), (int)items.size());
            ImGui::SameLine();
            if (ImGui::Button("Load##videoprofile"))
            {
                if (Config_LoadVideoProfile(profiles[selected]))
                {
                    Overlay_Toast(std::format("Loaded video profile {}", profiles[selected]).c_str());
                    Changed(host, true);
                }
                else
                    Overlay_Toast("Could not load video profile");
            }
        }
    }

    ImGui::SeparatorText("Display");
    Check(host, "Fullscreen", c.fullscreen, false,
          "Enter fullscreen. Host fullscreen hotkey (default F11) or Alt+Enter also toggle it.");
    Check(host, "Exclusive fullscreen", c.exclusiveFullscreen, false,
          "Linux: change the monitor mode (lower latency on some setups). Off = borderless desktop fullscreen. Windows stays borderless.");
    Check(host, "VSync", c.vsync, false,
          "Synchronise Present with the host display refresh (60/120/165/…), not emulated VI. "
          "Off allows faster presentation (tearing possible). Restart may be needed after changing.");
    Check(host, "Widescreen (16:9)", c.widescreen, true, "16:9 like the Wii's widescreen setting. Off: 4:3.");
    Check(host, "Integer scaling", c.integerScale, false,
          "Present at the largest whole-number multiple of 640×360 (16:9) or 640×480 (4:3) that fits; letterbox the rest. Off = fill the window while keeping aspect.");
    static const int sizes[][2] = { { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 } };
    int sizeIdx = 0;
    const int sizeIds[] = { 0, 1, 2, 3, 4 };
    const char* sizeNames[] = { "1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160" };
    for (int i = 0; i < 5; i++)
        if (sizes[i][0] == c.windowWidth && sizes[i][1] == c.windowHeight) sizeIdx = i;
    if (Combo(host, "Window size", sizeIdx, sizeIds, sizeNames, 5, false, "Size of the window when not fullscreen."))
    {
        c.windowWidth = sizes[sizeIdx][0];
        c.windowHeight = sizes[sizeIdx][1];
        Changed(host, false);
    }

    ImGui::SeparatorText("Rendering");
    const int fps[] = { 30, 60, 0 };
    const char* fpsNames[] = { "30 (original)", "60", "Uncapped" };
    if (Combo(host, "Frame rate", c.frameRate, fps, fpsNames, 3, true,
              "30 = native wait=2. 60 = true 60 Hz guest (wait=1) with spring cooldown gate. "
              "0 = uncapped present (sim stays 60 Hz). Optional present cap below."))
        Changed(host, true);
    {
        const int caps[] = { 0, 60, 120, 144, 165, 240 };
        const char* capNames[] = { "Off (display Hz)", "60", "120", "144", "165", "240" };
        if (Combo(host, "Uncapped present FPS limit", c.hostFpsCap, caps, capNames, 6, false,
                  "Only when Frame rate is Uncapped: max Present calls/sec of the latest XFB. "
                  "Off = match the host display refresh (not a 1000 Hz spam). Sim stays 60 Hz. "
                  "present_calls ≠ unique game frames. 30/60 modes ignore this."))
        {
            Perf_SetHostFpsCap(c.hostFpsCap);
            Changed(host, false);
        }
    }
    const int scales[] = { 1, 2, 3, 4, 5, 6 };
    const char* scaleNames[] = { "1x (640 x 528)", "2x (1280 x 1056)", "3x (1920 x 1584)", "4x (2560 x 2112)", "5x (3200 x 2640)", "6x (3840 x 3168)" };
    if (Combo(host, "Internal resolution", c.internalScale, scales, scaleNames, 6, true, "Resolution the game is rendered at."))
        Changed(host, true);
    const int anis[] = { 1, 2, 4, 8, 16 };
    const char* anisNames[] = { "Off (as the game sets it)", "2x", "4x", "8x", "16x" };
    if (Combo(host, "Anisotropic filtering", c.anisotropy, anis, anisNames, 5, false, "Sharper textures at grazing angles."))
        Changed(host, false);
    Check(host, "Compile shaders in the background", c.asyncShaders, false,
          "Avoids hitches when new effects appear; an object may then show up a frame late.");
    Check(host, "Graphics on a second CPU core", c.gpuThread, true, "Like Dolphin's dual core mode. Leave on unless debugging.");
    Check(host, "HD texture packs", c.texturePack, true,
          "Dolphin-format packs in textures/<game ID> (e.g. RENE8P) next to the game, game/textures or Dolphin's Load/Textures.");
}

void InputTab(SettingsHost host)
{
    Config& c = g_config;
    ImGui::SeparatorText("Controller");
    Check(host, "Rumble", c.rumble, false, "Vibrate the controller where the Wii Remote would rumble.");
    ImGui::TextDisabled("View (Back) opens these settings. View + Start is the Wii HOME button.");

    ImGui::SeparatorText("Stick dead zones");
    float lDz = c.stickDeadzone * 100.0f;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    if (ImGui::SliderFloat("Left stick dead zone", &lDz, 0.0f, 50.0f, "%.0f%%"))
    {
        c.stickDeadzone = lDz / 100.0f;
        Changed(host, false);
    }
    Help("Ignores small left-stick motion. ~24% matches the XInput default.");
    float rDz = c.stickDeadzoneRight * 100.0f;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    if (ImGui::SliderFloat("Right stick dead zone", &rDz, 0.0f, 50.0f, "%.0f%%"))
    {
        c.stickDeadzoneRight = rDz / 100.0f;
        Changed(host, false);
    }
    Help("Ignores small right-stick motion (pointer / tilt / slash flicks). ~26% ≈ XInput default.");
    float tDz = c.triggerDeadzone * 100.0f;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    if (ImGui::SliderFloat("Trigger dead zone", &tDz, 0.0f, 50.0f, "%.0f%%"))
    {
        c.triggerDeadzone = tDz / 100.0f;
        Changed(host, false);
    }
    Help("Ignores light LT/RT pulls (thrust / pull-back). ~12% ≈ XInput default.");
    if (ImGui::Button("Reset stick / trigger dead zones"))
    {
        Config defaults;
        c.stickDeadzone = defaults.stickDeadzone;
        c.stickDeadzoneRight = defaults.stickDeadzoneRight;
        c.triggerDeadzone = defaults.triggerDeadzone;
        Changed(host, false);
    }

    // Live hotplug status (updates every frame while this tab is open).
    const int pads = Pad_Count();
    ImGui::Text("Connected controllers: %d / 4", pads);
    for (int i = 0; i < 4; i++)
    {
        const char* name = Pad_Name(i);
        if (name)
            ImGui::BulletText("Player %d: %s", i + 1, name);
        else if (i == 0)
            ImGui::BulletText("Player 1: keyboard / mouse (no pad in this slot)");
    }
    if (pads == 0)
        ImGui::TextDisabled("Plug in a controller any time — it is picked up automatically.");
    else
        ImGui::TextDisabled("Unplugging packs remaining pads into the lowest player slots.");

    ImGui::SeparatorText("Button bindings");
    static const char* actions[12] = { "Jump / homing attack (Wii A)", "Slash (swing the Remote)", "Soul Surge (Wii B)",
                                       "Shake Nunchuk", "Guard / talk (Z)", "Nunchuk C", "Pause (Wii +)",
                                       "Wii - (unused by the game)", "Wii 1", "Wii 2", "Thrust Remote forward",
                                       "Pull Remote back" };
    static const std::string buttons[] = { "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "START", "BACK",
                                           "LS", "RS", "UP", "DOWN", "LEFT", "RIGHT", "NONE" };
    static const char* names[] = { "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Start", "View (Back, also opens settings)",
                                   "Left stick click", "Right stick click", "D-pad up", "D-pad down", "D-pad left",
                                   "D-pad right", "None" };
    for (int i = 0; i < 12; i++)
    {
        ImGui::PushID(i);
        std::string v = c.bind[i];
        for (char& ch : v) ch = (char)toupper((unsigned char)ch);
        if (Combo(host, actions[i], v, buttons, names, 17, false, "Controller button for this Wii action."))
        {
            c.bind[i] = v;
            Changed(host, false);
        }
        ImGui::PopID();
    }
    if (ImGui::Button("Restore default bindings"))
    {
        Config defaults;
        for (int i = 0; i < 12; i++) c.bind[i] = defaults.bind[i];
        Changed(host, false);
    }

    ImGui::SeparatorText("Keyboard and mouse");
    static const char* keyActions[21] = { "Jump / homing attack (Wii A)", "Slash (swing the Remote)", "Soul Surge (Wii B)",
                                          "Shake Nunchuk", "Guard / talk (Z)", "Nunchuk C", "Pause (Wii +)", "Wii -",
                                          "Wii 1", "Wii 2", "Thrust Remote forward", "Pull Remote back", "Wii HOME button",
                                          "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Move forward",
                                          "Move back", "Move left", "Move right" };
    if (s_capture >= 0)
    {
        // waiting for a key: wait until everything is released (the click on "Set"), then
        // take the next key or mouse button; Escape cancels
        int held = Gamepad_HeldKey();
        if (Key_Down(VK_ESCAPE)) s_capture = -1;
        else if (!s_captureArmed) s_captureArmed = held == 0;
        else if (held)
        {
            std::string name = Gamepad_KeyName(held);
            std::string& cur = c.keys[s_capture];
            if (s_captureAdd && _stricmp(cur.c_str(), "NONE") != 0 && !cur.empty())
            {
                bool dup = false;
                for (int vk : Gamepad_ParseKeys(cur)) dup |= vk == held;
                if (!dup) cur += ", " + name;
            }
            else
                cur = name;
            s_capture = -1;
            Changed(host, false);
        }
    }
    if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_SizingStretchProp))
    {
        for (int i = 0; i < 21; i++)
        {
            ImGui::PushID(100 + i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(keyActions[i]);
            ImGui::TableNextColumn();
            if (s_capture == i)
                ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_CheckMark], "Press a key or mouse button (Esc cancels)");
            else
            {
                if (ImGui::Button("Set")) { s_capture = i; s_captureAdd = false; s_captureArmed = false; }
                Help("Replace this binding: click, then press a key or mouse button.");
                ImGui::SameLine();
                if (ImGui::Button("+")) { s_capture = i; s_captureAdd = true; s_captureArmed = false; }
                Help("Add another key or mouse button for this action.");
                ImGui::SameLine();
                if (ImGui::Button("Clear")) { c.keys[i] = "NONE"; Changed(host, false); }
                ImGui::SameLine();
                ImGui::TextUnformatted(c.keys[i].c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Restore default keyboard and mouse"))
    {
        Config defaults;
        for (int i = 0; i < 21; i++) c.keys[i] = defaults.keys[i];
        Changed(host, false);
    }

    ImGui::SeparatorText("Input profiles");
    Help("Save or load controller bindings, keyboard bindings, rumble and dead zones as named profiles "
         "under user_data/input_profiles/.");
    {
        static char profileName[48] = "default";
        static int selected = 0;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
        ImGui::InputText("Profile name", profileName, sizeof(profileName));
        if (ImGui::Button("Save profile"))
        {
            std::string path;
            if (Config_SaveInputProfile(profileName, &path))
            {
                Overlay_Toast(std::format("Saved input profile {}",
                                          std::filesystem::path(path).stem().string())
                                  .c_str());
                Changed(host, false);
            }
            else
                Overlay_Toast("Could not save input profile");
        }
        ImGui::SameLine();
        if (ImGui::Button("Open profiles folder"))
            Shell_OpenFolder(Config_InputProfilesDir());
        std::vector<std::string> profiles = Config_ListInputProfiles();
        if (profiles.empty())
            ImGui::TextDisabled("No saved profiles yet.");
        else
        {
            if (selected >= (int)profiles.size()) selected = 0;
            std::vector<const char*> items;
            items.reserve(profiles.size());
            for (auto& p : profiles) items.push_back(p.c_str());
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
            ImGui::Combo("Load profile", &selected, items.data(), (int)items.size());
            ImGui::SameLine();
            if (ImGui::Button("Load"))
            {
                if (Config_LoadInputProfile(profiles[selected]))
                {
                    Gamepad_ReloadBindings();
                    Overlay_Toast(std::format("Loaded input profile {}", profiles[selected]).c_str());
                    Changed(host, true);
                }
                else
                    Overlay_Toast("Could not load input profile");
            }
        }
    }

    // Warn when the same key/mouse button is bound to more than one action.
    {
        std::unordered_map<int, std::vector<int>> owners;
        for (int i = 0; i < 21; i++)
            for (int vk : Gamepad_ParseKeys(c.keys[i]))
                if (vk) owners[vk].push_back(i);
        int shown = 0;
        for (auto& [vk, acts] : owners)
        {
            if (acts.size() < 2 || shown >= 4) continue;
            std::string msg = Gamepad_KeyName(vk) + " is used by: ";
            for (size_t n = 0; n < acts.size(); n++)
            {
                if (n) msg += ", ";
                msg += keyActions[acts[n]];
            }
            ImGui::TextColored(ImVec4(1.f, 0.75f, 0.35f, 1.f), "%s", msg.c_str());
            shown++;
        }
        if (shown)
            Help("Shared keys fire every matching action together. Clear or rebind if that is not intended.");
    }
    ImGui::TextDisabled("%s opens the settings, %s toggles fullscreen, %s saves a screenshot; the mouse also moves the Wii Remote pointer.",
                        Config_HotkeyName(c.hotkeySettings, "F1").c_str(),
                        Config_HotkeyName(c.hotkeyFullscreen, "F11").c_str(),
                        Config_HotkeyName(c.hotkeyScreenshot, "F12").c_str());
}

void AudioTab(SettingsHost host)
{
    Config& c = g_config;
    ImGui::SeparatorText("Volume");
    ImGui::TextDisabled("Music and sound effects share the original Wii mix — only a master scale is applied.");
    float masterPct = c.masterVolume * 100.0f;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    if (ImGui::SliderFloat("Master volume", &masterPct, 0.0f, 100.0f, "%.0f%%"))
    {
        c.masterVolume = masterPct / 100.0f;
        Changed(host, false);
    }
    Help("Scales the whole game mix (music + SFX) on the PC. Does not change in-game mixing.");
    float spkPct = c.speakerVolume * 100.0f;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    if (ImGui::SliderFloat("Remote speaker level", &spkPct, 0.0f, 100.0f, "%.0f%%"))
    {
        c.speakerVolume = spkPct / 100.0f;
        Changed(host, false);
    }
    Help("Relative loudness of Wii Remote speaker sounds (still multiplied by master volume).");

    ImGui::SeparatorText("Devices");
    {
        auto devices = AudioOut_ListDevices();
        int cur = 0;
        for (int i = 0; i < (int)devices.size(); i++)
            if (devices[i].name == c.audioDevice) cur = i;
        std::vector<std::string> labels;
        labels.reserve(devices.size());
        for (auto& d : devices)
            labels.push_back(d.name.empty() ? "System default" : d.name);
        const char* preview = labels.empty() ? "System default" : labels[cur].c_str();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
        if (ImGui::BeginCombo("Output device", preview))
        {
            for (int i = 0; i < (int)devices.size(); i++)
            {
                bool selected = i == cur;
                if (ImGui::Selectable(labels[i].c_str(), selected) && !selected)
                {
                    c.audioDevice = devices[i].name;
                    Changed(host, false);
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        Help("Linux: pick a playback device (SDL). Windows uses the system default output.");
    }
    Check(host, "Wii Remote speaker sounds", c.remoteSpeaker, false,
          "Play the sounds the game sends to the Wii Remote speaker through the PC.");
}

void SystemTab(SettingsHost host)
{
    Config& c = g_config;
    ImGui::SeparatorText("System");
    const int langs[] = { 1, 2, 3, 4, 5, 6, 0 };
    const char* langNames[] = { "English", "German", "French", "Spanish", "Italian", "Dutch", "Japanese" };
    if (Combo(host, "Language", c.language, langs, langNames, 7, true, "Language of the game's text and menus (the Wii system language)."))
        Changed(host, true);
    Check(host, "Pause when the window is in the background", c.pauseOnFocusLoss, false,
          "Freeze the game while another window has focus or the game is minimized. Audio is muted and rumble stops until focus returns.");
    Check(host, "Show FPS overlay", c.showFpsOverlay, false,
          "Draw FPS, frame time, Present ms, draws/XFB, guest Hz, memory and audio underruns while playing.");
    Check(host, "Show input hints", c.showInputHints, false,
          "Corner prompts with pad/key glyphs for Jump / Slash / Pause while playing.");
    Check(host, "Backup saves when starting the game", c.autoBackupSavesOnLaunch, false,
          "Before each Play, copy the save tree into save_backups/<timestamp>/. Skipped for --nand test runs.");
    Check(host, "Backup saves after the game writes them", c.autoBackupSavesOnWrite, false,
          "After a stage clear or menu save (NAND flush), copy the save tree. Debounced; at most once per minute.");
    {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
        if (ImGui::SliderInt("Keep save backups", &c.saveBackupKeep, 0, 50, "%d (0 = all)"))
            Changed(host, false);
        Help("Oldest timestamped folders under save_backups/ are removed after a new backup. 0 = keep all.");
    }
    {
        float pct = c.uiScale * 100.0f;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
        if (ImGui::SliderFloat("UI scale", &pct, 75.0f, 150.0f, "%.0f%%"))
        {
            c.uiScale = pct / 100.0f;
            Changed(host, false);
        }
        Help("Scales launcher and in-game settings text (75%–150%). Applies immediately.");
    }
    ImGui::SeparatorText("Host hotkeys");
    {
        static const std::string fkeys[] = { "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12" };
        static const char* flabels[] = { "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12" };
        auto pick = [&](const char* label, std::string& slot, const char* help) {
            std::string v = Config_HotkeyName(slot, slot.c_str());
            if (Combo(host, label, v, fkeys, flabels, 12, false, help))
            {
                slot = v;
                Changed(host, false);
            }
        };
        pick("Open settings", c.hotkeySettings, "Toggle the settings overlay (View / Back still works). Escape closes it.");
        pick("Screenshot", c.hotkeyScreenshot, "Save a full-resolution PNG under screenshots/.");
        pick("Fullscreen", c.hotkeyFullscreen, "Toggle fullscreen. Alt+Enter also works.");
        if (ImGui::Button("Restore default hotkeys"))
        {
            Config defaults;
            c.hotkeySettings = defaults.hotkeySettings;
            c.hotkeyScreenshot = defaults.hotkeyScreenshot;
            c.hotkeyFullscreen = defaults.hotkeyFullscreen;
            Changed(host, false);
        }
        const int a = Config_HotkeyVk(c.hotkeySettings, 0x70);
        const int b = Config_HotkeyVk(c.hotkeyScreenshot, 0x7B);
        const int d = Config_HotkeyVk(c.hotkeyFullscreen, 0x7A);
        if (a == b || a == d || b == d)
            ImGui::TextColored(ImVec4(1.f, 0.75f, 0.35f, 1.f),
                               "Two host hotkeys share the same key — pick distinct F-keys.");
    }
    ImGui::Spacing();
    ImGui::TextWrapped("User data: %s", Platform_UserDir().string().c_str());
    Help("Config, log, screenshots and default saves. Windows installer → %APPDATA%\\SonicColorsRecomp; "
         "AppImage → ~/.local/share/SonicColorsRecomp; portable ZIP / --portable → next to the exe.");
    if (ImGui::Button("Open user data folder"))
        Shell_OpenFolder(Platform_UserDir());
    Help("Opens the folder that holds config.ini, the log, screenshots, and (by default) save backups.");
    ImGui::SameLine();
    if (ImGui::Button("Open config backups"))
    {
        fs::path dir = Platform_UserDir() / "config_backups";
        std::error_code ec;
        fs::create_directories(dir, ec);
        Shell_OpenFolder(dir);
    }
    Help("Timestamped copies of config.ini written before Settings Reset (newest 20 kept).");
    ImGui::SameLine();
    if (ImGui::Button("Open shader cache"))
    {
        fs::path dir = Platform_UserDir() / "shadercache";
        std::error_code ec;
        fs::create_directories(dir, ec);
        Shell_OpenFolder(dir);
    }
    Help("Compiled host shaders. Safe to delete if a graphics update leaves the game black or crashing on boot.");
    ImGui::SameLine();
    if (ImGui::Button("Clear shader cache…"))
        ImGui::OpenPopup("Clear shader cache?");
    {
        static std::string clearMsg;
        if (ImGui::BeginPopupModal("Clear shader cache?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextWrapped(
                "Delete compiled shaders under your user data shadercache folder?\n\n"
                "The next run (or new areas) will recompile them — first visits may hitch briefly. "
                "Shipped AppImage/package keys are not touched.");
            if (ImGui::Button("Clear", ImVec2(120, 0)))
            {
                fs::path dir = Platform_UserDir() / "shadercache";
                std::error_code ec;
                size_t removed = 0;
                if (fs::is_directory(dir, ec))
                {
                    for (auto it = fs::directory_iterator(dir, ec); it != fs::directory_iterator(); it.increment(ec))
                    {
                        fs::remove_all(it->path(), ec);
                        if (!ec) ++removed;
                    }
                }
                fs::create_directories(dir, ec);
                clearMsg = removed ? std::format("Removed {} item(s) from shadercache.", removed)
                                   : "Shader cache was already empty.";
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0)))
            {
                clearMsg.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (!clearMsg.empty())
            ImGui::TextDisabled("%s", clearMsg.c_str());
    }
    ImGui::TextDisabled("Settings file: %s", Config_Path().c_str());

    ImGui::SeparatorText("Reset");
    if (ImGui::Button("Reset settings to defaults…"))
        ImGui::OpenPopup("Reset settings");
    Help("Restores Video, Audio, Input and System defaults. Keeps Paths, the Mods list, and Music Library folder settings. "
         "A timestamped copy of config.ini is written under user_data/config_backups/ first.");
    if (ImGui::BeginPopupModal("Reset settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped("Reset Video, Audio, Input and System options to their built-in defaults?\n\n"
                           "A backup of the current config.ini is saved under config_backups/ first.\n"
                           "Paths, installed-mod order, and Music Library folder settings are kept. "
                           "Some options (frame rate, resolution, language) need a restart.");
        if (ImGui::Button("Reset", ImVec2(120, 0)))
        {
            std::string backupPath;
            if (Config_BackupCurrent(&backupPath))
                Overlay_Toast(std::format("Settings backed up to {}",
                                          std::filesystem::path(backupPath).filename().string())
                                  .c_str());
            else
                Overlay_Toast("Could not back up config.ini (reset continues)");
            Config keepPaths = c;
            Config defaults;
            defaults.dataDir = keepPaths.dataDir;
            defaults.isoPath = keepPaths.isoPath;
            defaults.modsDir = keepPaths.modsDir;
            defaults.mods = keepPaths.mods;
            defaults.musicModsEnabled = keepPaths.musicModsEnabled;
            defaults.musicModsDir = keepPaths.musicModsDir;
            defaults.musicFilterCat = keepPaths.musicFilterCat;
            defaults.musicFilterVar = keepPaths.musicFilterVar;
            defaults.musicFilterRepl = keepPaths.musicFilterRepl;
            defaults.musicFilterWorld = keepPaths.musicFilterWorld;
            defaults.texturePackDir = keepPaths.texturePackDir;
            // Keep the auto-backup preference across Reset (it is a safety setting).
            defaults.autoBackupSavesOnLaunch = keepPaths.autoBackupSavesOnLaunch;
            defaults.autoBackupSavesOnWrite = keepPaths.autoBackupSavesOnWrite;
            defaults.saveBackupKeep = keepPaths.saveBackupKeep;
            defaults.screenshotKeep = keepPaths.screenshotKeep;
            defaults.launcherTab = keepPaths.launcherTab;
            defaults.settingsTab = keepPaths.settingsTab;
            defaults.launcherWidth = keepPaths.launcherWidth;
            defaults.launcherHeight = keepPaths.launcherHeight;
            c = defaults;
            AudioOut_ApplyVolumes(c.masterVolume, c.speakerVolume);
            AudioOut_SetDevice(c.audioDevice);
            Perf_SetHostFpsCap(c.hostFpsCap);
            Changed(host, true);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::SeparatorText("Updates");
    {
        ImGui::TextDisabled("This build: %s", g_version);
        ImGui::BeginDisabled(UpdateCheck_Busy());
        if (ImGui::Button("Check for updates"))
            UpdateCheck_Start(true);
        ImGui::EndDisabled();
        Help("Asks GitHub for the latest published release tag.");
        ImGui::SameLine();
        if (ImGui::Button("Open Releases page"))
            UpdateCheck_OpenReleases();
        ImGui::BeginDisabled(!UpdateCheck_CanDownload() || UpdateCheck_DownloadBusy());
        if (ImGui::Button("Download update package"))
            UpdateCheck_StartDownload();
        ImGui::EndDisabled();
        Help("Saves the AppImage or Windows zip under user_data/downloads/ (keeps the newest 3; AppImages are marked executable). "
             "Install & relaunch applies AppImage in-place or schedules a Windows zip extract after exit.");
        ImGui::SameLine();
        if (ImGui::Button("Open downloads folder"))
            UpdateCheck_OpenDownloadFolder();
        if (UpdateCheck_DownloadOk())
        {
            ImGui::SameLine();
            if (UpdateCheck_DownloadedPackageIsAppImage())
            {
                if (ImGui::Button("Open AppImage"))
                    UpdateCheck_OpenDownloadedPackage();
            }
            else if (ImGui::Button("Reveal package"))
                UpdateCheck_OpenDownloadedPackage();
            if (UpdateCheck_CanApplyUpdate())
            {
                ImGui::SameLine();
                if (ImGui::Button("Install & relaunch"))
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
        const std::string updateMsg = UpdateCheck_Message();
        if (!updateMsg.empty()) ImGui::TextWrapped("%s", updateMsg.c_str());
        if (UpdateCheck_NewerAvailable())
        {
            const std::string notes = UpdateCheck_Notes();
            if (!notes.empty())
            {
                ImGui::SeparatorText("What's new");
                ImGui::TextWrapped("%s", notes.c_str());
            }
        }
        if (UpdateCheck_DownloadBusy() || UpdateCheck_DownloadDone())
        {
            const float p = UpdateCheck_DownloadProgress();
            if (p >= 0.f)
                ImGui::ProgressBar(p, ImVec2(-1, 0));
            const std::string dl = UpdateCheck_DownloadMessage();
            if (!dl.empty()) ImGui::TextWrapped("%s", dl.c_str());
        }
    }

    ImGui::SeparatorText("Diagnostics");
    {
        static std::string diagMsg;
        const std::string& logPath = Log_Path();
        if (!logPath.empty())
        {
            ImGui::TextWrapped("%s", logPath.c_str());
            if (ImGui::Button("Open log folder"))
            {
                fs::path p(logPath);
                Shell_OpenFolder(p.has_parent_path() ? p.parent_path() : p);
            }
            Help("Opens the folder with SonicColorsRecomp.log plus the last few runs (.1.log … .5.log).");
            ImGui::SameLine();
            if (ImGui::Button("Copy log path"))
            {
                ImGui::SetClipboardText(logPath.c_str());
                diagMsg = "Log path copied to clipboard";
            }
        }
        else
            ImGui::TextDisabled("Log path not available yet.");
        ImGui::SameLine();
        if (ImGui::Button("Open screenshots folder"))
        {
            fs::path shots = Platform_UserDir() / "screenshots";
            std::error_code ec;
            fs::create_directories(shots, ec);
            Shell_OpenFolder(shots);
        }
        Help("Host screenshot hotkey (default F12) saves a full-resolution PNG into this folder.");
        if (ImGui::Button("Copy user data path"))
        {
            ImGui::SetClipboardText(Platform_UserDir().string().c_str());
            diagMsg = "User data path copied to clipboard";
        }
        Help("Copies the config/log/saves folder path for bug reports.");
        {
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::SliderInt("Keep screenshots", &c.screenshotKeep, 0, 200, "%d (0 = all)"))
                Changed(host, false);
            Help("Oldest screenshots/*.png are removed after each new shot. 0 = keep all.");
        }
        if (ImGui::Button("Save diagnostic report"))
        {
            std::string err;
            std::string path = SettingsUI_WriteDiagnosticReport(err);
            diagMsg = path.empty() ? err : ("Wrote " + path);
            if (!path.empty())
            {
                ImGui::SetClipboardText(path.c_str());
                fs::path p(path);
                Shell_OpenFolder(p.has_parent_path() ? p.parent_path() : p);
                diagMsg += " (path copied to clipboard)";
            }
        }
        Help("Writes version, paths, free space, config.ini, current + previous (.1) log tails to diagnostics/. Copies the file path to the clipboard. Also: --save-diagnostics.");
        if (!diagMsg.empty()) ImGui::TextWrapped("%s", diagMsg.c_str());
    }

    ImGui::SeparatorText("Save data");
    const auto nandRoot = nand_paths::ActiveRoot();
    ImGui::TextWrapped("%s", nandRoot.string().c_str());
    static std::string saveMsg;
    if (ImGui::Button("Open save folder"))
    {
        std::error_code ec;
        fs::create_directories(nandRoot, ec);
        Shell_OpenFolder(nandRoot);
        saveMsg = "Opened save folder";
    }
    Help("Opens the host NAND folder (save files live under title/…).");
    ImGui::SameLine();
    if (ImGui::Button("Backup now"))
    {
        fs::path dest;
        std::string err;
        if (nand_paths::BackupNow(dest, err))
            saveMsg = "Backup saved to " + dest.string();
        else
            saveMsg = err;
    }
    Help("Copies the current save tree into save_backups/<timestamp>/ next to your saves.");
    auto backups = nand_paths::ListBackups();
    if (backups.empty())
        ImGui::TextDisabled("No save backups yet.");
    else if (g_config.saveBackupKeep > 0)
        ImGui::TextDisabled("%d save backup(s) · keep %d", (int)backups.size(), g_config.saveBackupKeep);
    else
        ImGui::TextDisabled("%d save backup(s) · keep unlimited", (int)backups.size());
    static int restoreIdx = 0;
    if (restoreIdx >= (int)backups.size()) restoreIdx = 0;
    ImGui::BeginDisabled(backups.empty());
    if (!backups.empty())
    {
        // Newest-first labels for the combo.
        std::vector<std::string> labels;
        labels.reserve(backups.size());
        for (auto& p : backups) labels.push_back(p.filename().string());
        std::vector<const char*> items;
        items.reserve(labels.size());
        for (auto& s : labels) items.push_back(s.c_str());
        ImGui::SetNextItemWidth(220);
        ImGui::Combo("##restore_backup", &restoreIdx, items.data(), (int)items.size());
        Help("Pick which timestamped backup to restore.");
        ImGui::SameLine();
    }
    if (ImGui::Button("Restore backup…"))
        ImGui::OpenPopup("Restore save backup");
    ImGui::EndDisabled();
    static std::string restoreHelp;
    restoreHelp = backups.empty() ? "No backups yet — use Backup now first."
                                  : "Replace current saves with the selected backup.";
    Help(restoreHelp.c_str());
    if (ImGui::BeginPopupModal("Restore save backup", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        fs::path chosen = (!backups.empty() && restoreIdx >= 0 && restoreIdx < (int)backups.size())
                              ? backups[(size_t)restoreIdx]
                              : fs::path{};
        ImGui::TextWrapped("Replace all current save data with backup %s?\n\n"
                           "This cannot be undone except by making another backup first. "
                           "If the game is running, restart afterward so it reloads the files.",
                           chosen.empty() ? "?" : chosen.filename().string().c_str());
        if (ImGui::Button("Restore", ImVec2(120, 0)))
        {
            std::string err;
            if (!chosen.empty() && nand_paths::RestoreFrom(chosen, err))
                saveMsg = "Restored from " + chosen.string() + " — restart the game to reload";
            else
                saveMsg = err.empty() ? "No backup selected" : err;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (!saveMsg.empty()) ImGui::TextWrapped("%s", saveMsg.c_str());
}

void ModsTab(SettingsHost host)
{
    const char* help = nullptr;
    if (ModsUI_Draw(host, help)) Changed(host, true);
    if (help) s_help = help;
}

void MusicTab(SettingsHost host)
{
    const char* help = nullptr;
    MusicUI_Draw(host, help);
    if (help) s_help = help;
}
}   // namespace

namespace
{
void AppendLogTail(std::ostream& out, const fs::path& logPath, size_t maxLines)
{
    std::ifstream in(logPath);
    if (!in)
    {
        out << "(cannot read " << logPath.string() << ")\n";
        return;
    }
    std::deque<std::string> tail;
    std::string line;
    while (std::getline(in, line))
    {
        tail.push_back(line);
        if (tail.size() > maxLines) tail.pop_front();
    }
    for (auto& l : tail) out << l << "\n";
}
}   // namespace

std::string SettingsUI_WriteDiagnosticReport(std::string& error)
{
    fs::path dir = Platform_UserDir() / "diagnostics";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec)
    {
        error = "Cannot create diagnostics folder: " + ec.message();
        return {};
    }
    using clock = std::chrono::system_clock;
    auto t = clock::to_time_t(clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    fs::path path = dir / std::format("scr_diag_{:04d}{:02d}{:02d}_{:02d}{:02d}{:02d}.txt", tm.tm_year + 1900,
                                      tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    std::ofstream out(path);
    if (!out)
    {
        error = "Cannot write " + path.string();
        return {};
    }
    const fs::path user = Platform_UserDir();
    const bool portable = Platform_IsPortable();
    out << "Sonic Colors Recomp — diagnostic report\n";
    out << "version: " << g_version << "\n";
#ifdef _WIN32
    out << "host: windows\n";
#else
    out << "host: linux\n";
#endif
    out << "user_dir: " << user.string() << (portable ? " (portable)\n" : "\n");
    out << "exe_dir: " << Platform_ExeDir().string() << "\n";
    {
        const fs::space_info si = fs::space(user, ec);
        if (!ec)
            out << "free_space: " << std::format("{:.1f} GB", (double)si.available / (1024.0 * 1024.0 * 1024.0))
                << "\n";
        else
            out << "free_space: (unavailable)\n";
    }
    out << "config: " << Config_Path() << "\n";
    out << "log: " << Log_Path() << "\n";
    out << "nand: " << nand_paths::ActiveRoot().string() << "\n";
    out << "data: " << g_config.dataDir << "\n";
    Input_EnsurePadsProbed();
    out << "pads: " << Pad_Count() << "\n";
    for (int i = 0; i < 4; i++)
    {
        if (const char* name = Pad_Name(i))
            out << "pad" << i << ": " << name << "\n";
    }
    {
        const std::string& dev = AudioOut_CurrentDeviceName();
        out << "audio_device: " << (dev.empty() ? "(default)" : dev) << "\n";
    }
    out << "master_volume: " << g_config.masterVolume << "\n";
    out << "frame_rate: " << g_config.frameRate << " host_fps_cap: " << g_config.hostFpsCap << "\n";
    out << "save_backups: " << nand_paths::ListBackups().size()
        << " (keep " << g_config.saveBackupKeep << ")\n";
    out << "screenshot_keep: " << g_config.screenshotKeep << "\n";
    {
        fs::path mark = user / "last_abnormal_exit.txt";
        if (fs::is_regular_file(mark, ec))
        {
            std::ifstream in(mark);
            std::string line;
            std::getline(in, line);
            out << "abnormal_exit_marker: " << (line.empty() ? "(present)" : line) << "\n";
        }
        else
            out << "abnormal_exit_marker: (none)\n";
    }
    if (UpdateCheck_HasResult())
    {
        out << "update_check: " << UpdateCheck_Message() << "\n";
        if (!UpdateCheck_DownloadMessage().empty())
            out << "update_download: " << UpdateCheck_DownloadMessage() << "\n";
        if (!UpdateCheck_DownloadPath().empty())
            out << "update_package: " << UpdateCheck_DownloadPath() << "\n";
    }
    out << "\n--- config.ini ---\n";
    out << Config_Text(g_config);
    out << "\n--- log (last 200 lines) ---\n";
    if (!Log_Path().empty())
        AppendLogTail(out, Log_Path(), 200);
    else
        out << "(log not open)\n";
    if (!Log_Path().empty())
    {
        fs::path prev = fs::path(Log_Path()).parent_path() / "SonicColorsRecomp.1.log";
        if (fs::is_regular_file(prev, ec))
        {
            out << "\n--- previous log .1.log (last 200 lines) ---\n";
            AppendLogTail(out, prev, 200);
        }
    }
    out.close();
    if (!out)
    {
        error = "Write failed for " + path.string();
        return {};
    }
    // Keep the newest 20 diagnostic reports.
    {
        std::vector<fs::path> reports;
        for (const auto& e : fs::directory_iterator(dir, ec))
        {
            if (ec || !e.is_regular_file()) continue;
            const auto name = e.path().filename().string();
            if (name.starts_with("scr_diag_") && name.ends_with(".txt"))
                reports.push_back(e.path());
        }
        std::sort(reports.begin(), reports.end(), [](const fs::path& a, const fs::path& b) {
            return a.filename().string() > b.filename().string();
        });
        for (size_t i = 20; i < reports.size(); i++)
        {
            fs::remove(reports[i], ec);
            ec.clear();
        }
    }
    return path.string();
}

void SettingsUI_SetRelease(const std::string& gameId, const std::string& dataDir) { ModsUI_SetRelease(gameId, dataDir); }
std::vector<std::string> SettingsUI_ActiveMods() { return ModsUI_ActiveMods(); }

namespace
{
// A page with the description of the hovered item below it, and the restart question
void Page(SettingsHost host, void (*draw)(SettingsHost), float helpH)
{
    // flattened: Down from the tabs goes straight to the first setting
    ImGui::BeginChild("page", ImVec2(0, -helpH), ImGuiChildFlags_NavFlattened);
    draw(host);
    static const bool shotBottom = EnvHas("SCR_UI_SHOT_BOTTOM", "BKR_UI_SHOT_BOTTOM");
    if (shotBottom) ImGui::SetScrollY(ImGui::GetScrollMaxY());
    ImGui::EndChild();
}

void Footer()
{
    ImGui::Separator();
    ImGui::TextWrapped("%s", s_help ? s_help : "");

    if (s_askRestart)
    {
        ImGui::OpenPopup("Restart needed");
        s_askRestart = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Restart needed", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("This setting needs a restart. Restart the game?");
        ImGui::TextDisabled("Your progress is saved whenever the game saves; unsaved progress is lost.");
        ImGui::Spacing();
        if (ImGui::Button("Restart now"))
        {
            s_restartRequested = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Later")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

float HelpHeight() { return ImGui::GetTextLineHeightWithSpacing() * 3; }
}   // namespace

void SettingsUI_Draw(SettingsHost host)
{
    s_help = nullptr;
    if (ImGui::BeginTabBar("settings"))
    {
        struct Tab { const char* name; void (*draw)(SettingsHost); };
        static const Tab tabs[] = { { "Video", VideoTab }, { "Input", InputTab }, { "Audio", AudioTab }, { "System", SystemTab } };
        // test aid: SCR_UI_SHOT_SUBTAB=<tab name> (legacy BKR_*) opens that tab first
        static const char* shotTab = EnvGet("SCR_UI_SHOT_SUBTAB", "BKR_UI_SHOT_SUBTAB");
        static int shotFrames = 0;
        static int bootFrames = 0;
        bool shot = shotTab && shotFrames++ < 2;
        const bool forceRemembered = !shotTab && bootFrames++ < 2;
        static int selected = -1;
        if (selected < 0) selected = std::clamp(g_config.settingsTab, 0, 3);
        int pick = SettingsUI_PadTab(selected, (int)std::size(tabs), false);
        for (int i = 0; i < (int)std::size(tabs); i++)
        {
            const bool force = (shot && !_stricmp(shotTab, tabs[i].name)) || (forceRemembered && i == selected);
            if (SettingsUI_TabItem(tabs[i].name, i, pick, force))
            {
                if (selected != i)
                {
                    selected = i;
                    g_config.settingsTab = i;
                    Config_Save();
                }
                Page(host, tabs[i].draw, HelpHeight());
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    Footer();
}

void SettingsUI_DrawMods(SettingsHost host)
{
    s_help = nullptr;
    Page(host, ModsTab, HelpHeight());
    Footer();
}

void SettingsUI_DrawMusic(SettingsHost host)
{
    s_help = nullptr;
    // Music Library needs vertical room for list + preview; keep help strip short.
    Page(host, MusicTab, ImGui::GetTextLineHeightWithSpacing() * 1.5f);
    Footer();
}

int SettingsUI_PadTab(int selected, int count, bool outer)
{
    if (count <= 0 || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) return -1;
    ImGuiKey prev = outer ? ImGuiKey_GamepadL1 : ImGuiKey_GamepadL2;
    ImGuiKey next = outer ? ImGuiKey_GamepadR1 : ImGuiKey_GamepadR2;
    int step = (int)ImGui::IsKeyPressed(next, false) - (int)ImGui::IsKeyPressed(prev, false);
    if (!step) return -1;
    return (selected + step + count) % count;
}

bool SettingsUI_TabItem(const char* name, int index, int pick, bool select)
{
    // a tab the controller/keyboard cursor moved onto opens at once (like a game menu)
    static ImGuiID s_focusedTab = 0;
    ImGuiID id = ImGui::GetID(name);
    bool open = ImGui::BeginTabItem(name, nullptr,
                                    index == pick || select || id == s_focusedTab ? ImGuiTabItemFlags_SetSelected : 0);
    if (id == s_focusedTab) s_focusedTab = 0;
    if (index == pick) ImGui::SetKeyboardFocusHere(-1);
    else if (!open && ImGui::IsItemFocused() && ImGui::GetIO().NavVisible) s_focusedTab = id;
    return open;
}

bool SettingsUI_ShotMods()
{
    const char* tab = EnvGet("SCR_UI_SHOT_SUBTAB", "BKR_UI_SHOT_SUBTAB");
    return tab && !_stricmp(tab, "Mods");
}

bool SettingsUI_TakeRestartRequest()
{
    bool r = s_restartRequested;
    s_restartRequested = false;
    return r;
}

void SettingsUI_DrawVersion()
{
    ImGui::SameLine();
    float pad = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(g_version).x;
    if (pad > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad);
    ImGui::TextDisabled("%s", g_version);
}

void SettingsUI_AddFallbackFonts()
{
    // after the main font: Chinese, Japanese and Korean names and symbols (mod titles) from
    // the system's fonts where they exist; glyphs load as text needs them
    ImGuiIO& io = ImGui::GetIO();
    if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();
    ImFontConfig cfg;
    cfg.MergeMode = true;
    for (const std::string& path : Platform_UiFallbackFonts()) io.Fonts->AddFontFromFileTTF(path.c_str(), 18.0f, &cfg);
}

void SettingsUI_ApplyStyle()
{
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 6;
    s.FrameRounding = 4;
    s.GrabRounding = 4;
    s.TabRounding = 4;
    s.WindowPadding = ImVec2(14, 12);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(10, 8);
    ImVec4* c = s.Colors;
    const ImVec4 blue(0.12f, 0.44f, 0.92f, 1.00f), blueLight(0.23f, 0.51f, 0.96f, 1.00f),
        blueDim(0.10f, 0.30f, 0.60f, 1.00f);
    c[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.067f, 0.086f, 0.94f);
    c[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.08f, 0.10f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.24f, 0.30f, 0.40f, 0.60f);
    c[ImGuiCol_FrameBg] = ImVec4(0.11f, 0.13f, 0.17f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.14f, 0.20f, 0.30f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.15f, 0.25f, 0.40f, 1.00f);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = ImVec4(0.07f, 0.08f, 0.11f, 1.00f);
    c[ImGuiCol_CheckMark] = blue;
    c[ImGuiCol_SliderGrab] = blue;
    c[ImGuiCol_SliderGrabActive] = blueLight;
    c[ImGuiCol_Button] = ImVec4(0.13f, 0.17f, 0.24f, 1.00f);
    c[ImGuiCol_ButtonHovered] = blueLight;
    c[ImGuiCol_ButtonActive] = blue;
    c[ImGuiCol_Header] = blueDim;
    c[ImGuiCol_HeaderHovered] = blueLight;
    c[ImGuiCol_HeaderActive] = blue;
    c[ImGuiCol_Tab] = ImVec4(0.09f, 0.11f, 0.15f, 1.00f);
    c[ImGuiCol_TabHovered] = blueLight;
    c[ImGuiCol_TabSelected] = blue;
    c[ImGuiCol_TabSelectedOverline] = blueLight;
    c[ImGuiCol_SeparatorHovered] = blueLight;
    c[ImGuiCol_Text] = ImVec4(0.90f, 0.93f, 0.95f, 1.00f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.58f, 0.62f, 1.00f);
    c[ImGuiCol_NavCursor] = blue;
    c[ImGuiCol_PlotHistogram] = blue;
}
