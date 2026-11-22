#include "config.h"
#include "audio/audio_out.h"
#include "log.h"
#include "platform/platform.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#ifndef __ANDROID__
#include <format>   // on Android, platform/compat.h provides it
#endif
#include <fstream>
#include <sstream>
#include <vector>

Config g_config;
static std::string s_path;

static std::string Trim(std::string s);
static std::string Lower(std::string s);
static bool ParseBool(const std::string& v);
static int ParseLanguage(const std::string& v);

// order matches Config::bind
const char* const kBindActions[12] = { "jump", "slash", "wii_b", "shake_nunchuk", "guard", "nunchuk_c",
                                       "plus", "minus", "button_1", "button_2", "thrust", "pull_back" };

// order matches Config::keys
const char* const kKeyActions[21] = { "jump", "slash", "wii_b", "shake_nunchuk", "guard", "nunchuk_c", "plus", "minus",
                                      "button_1", "button_2", "thrust", "pull_back", "home", "dpad_up", "dpad_down",
                                      "dpad_left", "dpad_right", "move_up", "move_down", "move_left", "move_right" };

static const char* const kLanguages[] = { "japanese", "english", "german", "french", "spanish", "italian", "dutch" };

static const char* Bool(bool b) { return b ? "true" : "false"; }

// The whole file, with comments, for the given settings (written on first run and
// whenever the launcher or the in-game settings change something).
std::string Config_Text(const Config& c)
{
    std::string s;
    s += "; SonicColorsRecomp settings (also editable in the launcher and in game: View button / F1)\n\n";
    s += "[Video]\n";
    s += "; 30 = original (wait 2). 60 = true 60 Hz guest (wait=1) with spring cooldown gate.\n";
    s += "; 0 = uncapped present (sim fixed 60 Hz). See docs/FRAME_RATE.md\n";
    s += std::format("frame_rate = {}\n", c.frameRate);
    s += "; when frame_rate is 0: max re-present rate (0 ≈ unlimited; sim stays 60 Hz)\n";
    s += std::format("host_fps_cap = {}\n", c.hostFpsCap);
    s += "; internal resolution multiplier (1 = 640x528, 3 = 1920x1584)\n";
    s += std::format("internal_scale = {}\n", c.internalScale);
    s += std::format("window_width = {}\nwindow_height = {}\n", c.windowWidth, c.windowHeight);
    s += "; restore maximized windowed state on next launch (ignored while fullscreen)\n";
    s += std::format("window_maximized = {}\n", Bool(c.windowMaximized));
    s += "; F11 or Alt+Enter toggles fullscreen at runtime\n";
    s += std::format("fullscreen = {}\n", Bool(c.fullscreen));
    s += "; exclusive = monitor mode change (Linux); off = borderless desktop fullscreen\n";
    s += std::format("exclusive_fullscreen = {}\n", Bool(c.exclusiveFullscreen));
    s += std::format("vsync = {}\nwidescreen = {}\n", Bool(c.vsync), Bool(c.widescreen));
    s += "; sharp pixels: largest integer multiple of 640×360 (16:9) or 640×480 (4:3) that fits\n";
    s += std::format("integer_scale = {}\n", Bool(c.integerScale));
    s += "; force anisotropic texture filtering: 1 (off), 2, 4, 8, 16\n";
    s += std::format("anisotropy = {}\n", c.anisotropy);
    s += "; process graphics on a second CPU core (like Dolphin's dual core)\n";
    s += std::format("gpu_thread = {}\n", Bool(c.gpuThread));
    s += "; compile new shaders in the background: no hitches, objects may appear a frame late\n";
    s += std::format("async_shaders = {}\n", Bool(c.asyncShaders));
    s += "; Dolphin-format HD texture packs: <dir>/<game ID>/**/tex1_*.png|dds. Empty dir searches\n";
    s += "; textures/ next to the exe, game/textures and Documents/Dolphin Emulator/Load/Textures\n";
    s += std::format("texture_pack = {}\ntexture_pack_dir = {}\n\n", Bool(c.texturePack), c.texturePackDir);
    s += "[System]\n";
    s += "; english, german, french, spanish, italian, dutch, japanese\n";
    s += std::format("language = {}\n", kLanguages[std::clamp(c.language, 0, 6)]);
    s += "; freeze the game while its window is in the background or minimized\n";
    s += std::format("pause_on_focus_loss = {}\n", Bool(c.pauseOnFocusLoss));
    s += "; draw an FPS counter while playing (host present rate)\n";
    s += std::format("show_fps_overlay = {}\n", Bool(c.showFpsOverlay));
    s += "; show Jump/Slash/Pause pad+key prompts while playing\n";
    s += std::format("show_input_hints = {}\n", Bool(c.showInputHints));
    s += "; copy the save tree into save_backups/ each time the game starts (Play)\n";
    s += std::format("auto_backup_saves_on_launch = {}\n", Bool(c.autoBackupSavesOnLaunch));
    s += "; copy after the game writes save data (stage clear / menu save); debounced + 60s cooldown\n";
    s += std::format("auto_backup_saves_on_write = {}\n", Bool(c.autoBackupSavesOnWrite));
    s += "; how many timestamped save_backups/ folders to keep (0 = unlimited)\n";
    s += std::format("save_backup_keep = {}\n", std::clamp(c.saveBackupKeep, 0, 200));
    s += "; how many screenshots/*.png to keep (0 = unlimited); oldest removed after F12\n";
    s += std::format("screenshot_keep = {}\n", std::clamp(c.screenshotKeep, 0, 1000));
    s += "; launcher and in-game settings UI scale (0.75–1.50)\n";
    s += std::format("ui_scale = {:.2f}\n", c.uiScale);
    s += "; host hotkeys (F1–F12). Escape still closes the settings overlay.\n";
    s += std::format("hotkey_settings = {}\n", c.hotkeySettings);
    s += std::format("hotkey_screenshot = {}\n", c.hotkeyScreenshot);
    s += std::format("hotkey_fullscreen = {}\n", c.hotkeyFullscreen);
    s += "; last launcher tab: 0=Play 1=Settings 2=Mods 3=Music\n";
    s += std::format("launcher_tab = {}\n", std::clamp(c.launcherTab, 0, 3));
    s += "; last settings sub-tab: 0=Video 1=Input 2=Audio 3=System\n";
    s += std::format("settings_tab = {}\n", std::clamp(c.settingsTab, 0, 3));
    s += "; last launcher window size (logical pixels)\n";
    s += std::format("launcher_width = {}\nlauncher_height = {}\n", c.launcherWidth, c.launcherHeight);
    s += "; last build version shown on the Play What's new banner (empty until first launch)\n";
    s += std::format("last_seen_version = {}\n", c.lastSeenVersion);
    s += "; local time of the last Play / --skip-launcher session start\n";
    s += std::format("last_played_at = {}\n\n", c.lastPlayedAt);
    s += "[Audio]\n";
    s += "; host volume scales (0.0–1.0). Music and SFX share the Wii mix — only a master\n";
    s += "; control is available without changing original game mixing.\n";
    s += std::format("master_volume = {:.3f}\n", c.masterVolume);
    s += "; Wii Remote speaker level relative to the TV mix (still multiplied by master)\n";
    s += std::format("speaker_volume = {:.3f}\n", c.speakerVolume);
    s += "; playback device name (empty = system default). Linux: SDL name; Windows: WASAPI endpoint.\n";
    s += std::format("audio_device = {}\n\n", c.audioDevice);
    s += "[Input]\n";
    s += std::format("rumble = {}\n", Bool(c.rumble));
    s += "; play sounds the game sends to the Wii Remote speaker\n";
    s += std::format("remote_speaker = {}\n", Bool(c.remoteSpeaker));
    s += "; analog stick dead zones as a fraction of full throw (0.0–0.5). Defaults ≈ XInput.\n";
    s += std::format("stick_deadzone = {:.3f}\n", c.stickDeadzone);
    s += std::format("stick_deadzone_right = {:.3f}\n", c.stickDeadzoneRight);
    s += "; analog trigger dead zone as a fraction of full pull (0.0–0.5). Default ≈ XInput.\n";
    s += std::format("trigger_deadzone = {:.3f}\n", c.triggerDeadzone);
    s += "; controller bindings: A B X Y LB RB LT RT START BACK LS RS UP DOWN LEFT RIGHT NONE\n";
    s += "; (BACK, the View button, opens the settings overlay; Back+Start together is HOME;\n";
    s += ";  the right stick also flicks slashes)\n";
    for (int i = 0; i < 12; i++) s += std::format("{} = {}\n", kBindActions[i], c.bind[i]);
    s += "\n[Keyboard]\n";
    s += "; keyboard and mouse bindings, comma-separated: A-Z 0-9 F2-F10 F12 SPACE ENTER BACKSPACE TAB\n";
    s += "; LSHIFT RSHIFT LCTRL RCTRL LALT RALT UP DOWN LEFT RIGHT INSERT DELETE HOME_KEY END PAGEUP\n";
    s += "; PAGEDOWN NUMPAD0-NUMPAD9 MOUSE1 (left) MOUSE2 (right) MOUSE3 (middle) MOUSE4 MOUSE5 NONE\n";
    s += "; (F1 opens the settings, F11 toggles fullscreen; the mouse also moves the pointer)\n";
    for (int i = 0; i < 21; i++) s += std::format("{} = {}\n", kKeyActions[i], c.keys[i]);
    s += "\n[Mods]\n";
    s += "; mod folders in load order, one per line: on = <folder> loads it, off = <folder> skips it.\n";
    s += "; When two enabled mods replace the same file, the one listed first wins. Mods not listed\n";
    s += "; here load after these, enabled. Also editable in the Mods tab.\n";
    for (auto& m : c.mods) s += std::format("{} = {}\n", m.enabled ? "on" : "off", m.folder);
    s += "\n[Paths]\n";
    s += "; leave empty to search next to the executable (game/extracted/DATA, game/*.iso;\n";
    s += "; mods: mods/ next to the executable and game/mods)\n";
    s += std::format("data = {}\niso = {}\nmods = {}\n", c.dataDir, c.isoPath, c.modsDir);
    s += "\n[Music]\n";
    s += "; custom stage WAV replacements (launcher Music Mods tab). Empty dir → mods/music\n";
    s += std::format("enabled = {}\nmusic_dir = {}\n", Bool(c.musicModsEnabled), c.musicModsDir);
    s += "; Music Library filter combos (0 = All). Search text is not persisted.\n";
    s += std::format("filter_cat = {}\nfilter_var = {}\nfilter_repl = {}\nfilter_world = {}\n",
                     std::clamp(c.musicFilterCat, 0, 7), std::clamp(c.musicFilterVar, 0, 2),
                     std::clamp(c.musicFilterRepl, 0, 2), std::clamp(c.musicFilterWorld, 0, 7));
    return s;
}

const std::string& Config_Path() { return s_path; }

int Config_HotkeyVk(const std::string& name, int fallbackVk)
{
    std::string u;
    for (unsigned char ch : name)
        if (ch != ' ') u.push_back((char)std::toupper(ch));
    if (u.size() >= 2 && u[0] == 'F')
    {
        int n = std::atoi(u.c_str() + 1);
        if (n >= 1 && n <= 12) return 0x70 + (n - 1);   // VK_F1..VK_F12
    }
    return fallbackVk;
}

std::string Config_HotkeyName(const std::string& name, const char* fallbackName)
{
    int vk = Config_HotkeyVk(name, 0);
    if (vk >= 0x70 && vk <= 0x7B) return std::format("F{}", vk - 0x70 + 1);
    return fallbackName ? fallbackName : "F1";
}

bool Config_BackupCurrent(std::string* outPath)
{
    if (s_path.empty()) return false;
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_regular_file(s_path, ec)) return false;
    fs::path dir = Platform_UserDir() / "config_backups";
    fs::create_directories(dir, ec);
    if (ec) { LOG_WARN("config: cannot create {}: {}", dir.string(), ec.message()); return false; }

    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d_%02d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    fs::path dest = dir / std::format("config_{}.ini", stamp);
    fs::copy_file(s_path, dest, fs::copy_options::overwrite_existing, ec);
    if (ec) { LOG_WARN("config: backup failed: {}", ec.message()); return false; }
    if (outPath) *outPath = dest.string();
    LOG_INFO("config: backed up to {}", dest.string());
    // Keep the newest 20 config_*.ini files (timestamp names sort lexicographically).
    {
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(dir, ec))
        {
            if (ec || !e.is_regular_file()) continue;
            if (e.path().extension() != ".ini") continue;
            files.push_back(e.path());
        }
        std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
            return a.filename() > b.filename();
        });
        for (size_t i = 20; i < files.size(); i++)
        {
            fs::remove(files[i], ec);
            ec.clear();
        }
    }
    return true;
}

static std::string SanitizeProfileName(const std::string& name)
{
    std::string out;
    for (unsigned char ch : name)
    {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == ' ')
            out.push_back(ch == ' ' ? '_' : (char)ch);
        if (out.size() >= 48) break;
    }
    while (!out.empty() && out.front() == '_') out.erase(out.begin());
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? "default" : out;
}

std::filesystem::path Config_InputProfilesDir()
{
    return Platform_UserDir() / "input_profiles";
}

std::vector<std::string> Config_ListInputProfiles()
{
    namespace fs = std::filesystem;
    std::vector<std::string> names;
    std::error_code ec;
    fs::path dir = Config_InputProfilesDir();
    if (!fs::is_directory(dir, ec)) return names;
    for (auto& e : fs::directory_iterator(dir, ec))
    {
        if (!e.is_regular_file(ec)) continue;
        if (e.path().extension() != ".ini") continue;
        names.push_back(e.path().stem().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string Config_InputProfileText(const Config& c)
{
    std::string s;
    s += "; Sonic Colors Recomp input profile (controller + keyboard + dead zones)\n\n";
    s += "[Input]\n";
    s += std::format("rumble = {}\n", Bool(c.rumble));
    s += std::format("remote_speaker = {}\n", Bool(c.remoteSpeaker));
    s += std::format("stick_deadzone = {:.3f}\n", c.stickDeadzone);
    s += std::format("stick_deadzone_right = {:.3f}\n", c.stickDeadzoneRight);
    s += std::format("trigger_deadzone = {:.3f}\n", c.triggerDeadzone);
    s += std::format("hotkey_settings = {}\n", c.hotkeySettings);
    s += std::format("hotkey_screenshot = {}\n", c.hotkeyScreenshot);
    s += std::format("hotkey_fullscreen = {}\n", c.hotkeyFullscreen);
    for (int i = 0; i < 12; i++) s += std::format("{} = {}\n", kBindActions[i], c.bind[i]);
    s += "\n[Keyboard]\n";
    for (int i = 0; i < 21; i++) s += std::format("{} = {}\n", kKeyActions[i], c.keys[i]);
    return s;
}

bool Config_ApplyInputProfileText(const std::string& text, Config& c)
{
    std::istringstream in(text);
    std::string line, section;
    bool any = false;
    while (std::getline(in, line))
    {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[')
        {
            section = Lower(Trim(line.substr(1, line.find(']') - 1)));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Lower(Trim(line.substr(0, eq)));
        std::string val = Trim(line.substr(eq + 1));
        if (section == "input")
        {
            any = true;
            if (key == "rumble") c.rumble = ParseBool(val);
            else if (key == "remote_speaker") c.remoteSpeaker = ParseBool(val);
            else if (key == "stick_deadzone")
                c.stickDeadzone = std::clamp(std::strtof(val.c_str(), nullptr), 0.0f, 0.5f);
            else if (key == "stick_deadzone_right")
                c.stickDeadzoneRight = std::clamp(std::strtof(val.c_str(), nullptr), 0.0f, 0.5f);
            else if (key == "trigger_deadzone")
                c.triggerDeadzone = std::clamp(std::strtof(val.c_str(), nullptr), 0.0f, 0.5f);
            else if (key == "hotkey_settings") c.hotkeySettings = Config_HotkeyName(val, "F1");
            else if (key == "hotkey_screenshot") c.hotkeyScreenshot = Config_HotkeyName(val, "F12");
            else if (key == "hotkey_fullscreen") c.hotkeyFullscreen = Config_HotkeyName(val, "F11");
            for (int i = 0; i < 12; i++)
                if (key == kBindActions[i]) c.bind[i] = val;
        }
        else if (section == "keyboard")
        {
            any = true;
            for (int i = 0; i < 21; i++)
                if (key == kKeyActions[i]) c.keys[i] = val;
        }
    }
    return any;
}

bool Config_SaveInputProfile(const std::string& name, std::string* outPath)
{
    namespace fs = std::filesystem;
    const std::string safe = SanitizeProfileName(name);
    std::error_code ec;
    fs::path dir = Config_InputProfilesDir();
    fs::create_directories(dir, ec);
    if (ec) { LOG_WARN("config: cannot create {}: {}", dir.string(), ec.message()); return false; }
    fs::path dest = dir / (safe + ".ini");
    fs::path tmp = dest;
    tmp += ".tmp";
    {
        std::ofstream f(tmp);
        if (!f) return false;
        f << Config_InputProfileText(g_config);
    }
    if (!Platform_ReplaceFile(tmp.string(), dest.string()))
    {
        fs::remove(tmp, ec);
        return false;
    }
    if (outPath) *outPath = dest.string();
    LOG_INFO("config: saved input profile {}", dest.string());
    return true;
}

bool Config_LoadInputProfile(const std::string& name)
{
    namespace fs = std::filesystem;
    const std::string safe = SanitizeProfileName(name);
    fs::path path = Config_InputProfilesDir() / (safe + ".ini");
    std::ifstream f(path);
    if (!f) { LOG_WARN("config: input profile not found: {}", path.string()); return false; }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (!Config_ApplyInputProfileText(ss.str(), g_config)) return false;
    LOG_INFO("config: loaded input profile {}", path.string());
    return true;
}

std::filesystem::path Config_ModProfilesDir()
{
    return Platform_UserDir() / "mod_profiles";
}

std::vector<std::string> Config_ListModProfiles()
{
    namespace fs = std::filesystem;
    std::vector<std::string> names;
    std::error_code ec;
    fs::path dir = Config_ModProfilesDir();
    if (!fs::is_directory(dir, ec)) return names;
    for (auto& e : fs::directory_iterator(dir, ec))
    {
        if (!e.is_regular_file(ec)) continue;
        if (e.path().extension() != ".ini") continue;
        names.push_back(e.path().stem().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string Config_ModProfileText(const Config& c)
{
    std::string s;
    s += "; Sonic Colors Recomp mod profile (enable + load order)\n\n[Mods]\n";
    s += "; on = <folder> enables; off = <folder> disables. Order is load order.\n";
    for (auto& m : c.mods) s += std::format("{} = {}\n", m.enabled ? "on" : "off", m.folder);
    return s;
}

bool Config_ApplyModProfileText(const std::string& text, Config& c)
{
    std::istringstream in(text);
    std::string line, section;
    std::vector<ModSetting> mods;
    while (std::getline(in, line))
    {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[')
        {
            section = Lower(Trim(line.substr(1, line.find(']') - 1)));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Lower(Trim(line.substr(0, eq)));
        std::string val = Trim(line.substr(eq + 1));
        if (section == "mods" && !val.empty() && (key == "on" || key == "off"))
            mods.push_back({ val, key == "on" });
    }
    if (mods.empty() && text.find("[Mods]") == std::string::npos) return false;
    c.mods = std::move(mods);
    return true;
}

bool Config_SaveModProfile(const std::string& name, std::string* outPath)
{
    namespace fs = std::filesystem;
    const std::string safe = SanitizeProfileName(name);
    std::error_code ec;
    fs::path dir = Config_ModProfilesDir();
    fs::create_directories(dir, ec);
    if (ec) { LOG_WARN("config: cannot create {}: {}", dir.string(), ec.message()); return false; }
    fs::path dest = dir / (safe + ".ini");
    fs::path tmp = dest;
    tmp += ".tmp";
    {
        std::ofstream f(tmp);
        if (!f) return false;
        f << Config_ModProfileText(g_config);
    }
    if (!Platform_ReplaceFile(tmp.string(), dest.string()))
    {
        fs::remove(tmp, ec);
        return false;
    }
    if (outPath) *outPath = dest.string();
    LOG_INFO("config: saved mod profile {}", dest.string());
    return true;
}

bool Config_LoadModProfile(const std::string& name)
{
    namespace fs = std::filesystem;
    const std::string safe = SanitizeProfileName(name);
    fs::path path = Config_ModProfilesDir() / (safe + ".ini");
    std::ifstream f(path);
    if (!f) { LOG_WARN("config: mod profile not found: {}", path.string()); return false; }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (!Config_ApplyModProfileText(ss.str(), g_config)) return false;
    LOG_INFO("config: loaded mod profile {}", path.string());
    return true;
}

std::filesystem::path Config_VideoProfilesDir()
{
    return Platform_UserDir() / "video_profiles";
}

std::vector<std::string> Config_ListVideoProfiles()
{
    namespace fs = std::filesystem;
    std::vector<std::string> names;
    std::error_code ec;
    fs::path dir = Config_VideoProfilesDir();
    if (!fs::is_directory(dir, ec)) return names;
    for (auto& e : fs::directory_iterator(dir, ec))
    {
        if (!e.is_regular_file(ec)) continue;
        if (e.path().extension() != ".ini") continue;
        names.push_back(e.path().stem().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string Config_VideoProfileText(const Config& c)
{
    std::string s;
    s += "; Sonic Colors Recomp video profile (display + rendering)\n\n[Video]\n";
    s += std::format("frame_rate = {}\n", c.frameRate);
    s += std::format("host_fps_cap = {}\n", c.hostFpsCap);
    s += std::format("internal_scale = {}\n", c.internalScale);
    s += std::format("window_width = {}\nwindow_height = {}\n", c.windowWidth, c.windowHeight);
    s += "; restore maximized windowed state on next launch (ignored while fullscreen)\n";
    s += std::format("window_maximized = {}\n", Bool(c.windowMaximized));
    s += std::format("fullscreen = {}\n", Bool(c.fullscreen));
    s += std::format("exclusive_fullscreen = {}\n", Bool(c.exclusiveFullscreen));
    s += std::format("vsync = {}\n", Bool(c.vsync));
    s += std::format("widescreen = {}\n", Bool(c.widescreen));
    s += std::format("integer_scale = {}\n", Bool(c.integerScale));
    s += std::format("anisotropy = {}\n", c.anisotropy);
    s += std::format("gpu_thread = {}\n", Bool(c.gpuThread));
    s += std::format("async_shaders = {}\n", Bool(c.asyncShaders));
    s += std::format("texture_pack = {}\n", Bool(c.texturePack));
    return s;
}

bool Config_ApplyVideoProfileText(const std::string& text, Config& c)
{
    std::istringstream in(text);
    std::string line, section;
    bool any = false;
    while (std::getline(in, line))
    {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[')
        {
            section = Lower(Trim(line.substr(1, line.find(']') - 1)));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Lower(Trim(line.substr(0, eq)));
        std::string val = Trim(line.substr(eq + 1));
        if (section != "video") continue;
        any = true;
        if (key == "frame_rate")
        {
            int v = atoi(val.c_str());
            c.frameRate = (v <= 0) ? 0 : (v >= 60 ? 60 : 30);
        }
        else if (key == "host_fps_cap") c.hostFpsCap = std::clamp(atoi(val.c_str()), 0, 1000);
        else if (key == "internal_scale") c.internalScale = std::clamp(atoi(val.c_str()), 1, 8);
        else if (key == "window_width") c.windowWidth = std::max(320, atoi(val.c_str()));
        else if (key == "window_height") c.windowHeight = std::max(240, atoi(val.c_str()));
        else if (key == "window_maximized") c.windowMaximized = ParseBool(val);
        else if (key == "fullscreen") c.fullscreen = ParseBool(val);
        else if (key == "exclusive_fullscreen") c.exclusiveFullscreen = ParseBool(val);
        else if (key == "vsync") c.vsync = ParseBool(val);
        else if (key == "widescreen") c.widescreen = ParseBool(val);
        else if (key == "integer_scale") c.integerScale = ParseBool(val);
        else if (key == "anisotropy") c.anisotropy = std::clamp(atoi(val.c_str()), 1, 16);
        else if (key == "gpu_thread") c.gpuThread = ParseBool(val);
        else if (key == "async_shaders") c.asyncShaders = ParseBool(val);
        else if (key == "texture_pack") c.texturePack = ParseBool(val);
    }
    return any;
}

bool Config_SaveVideoProfile(const std::string& name, std::string* outPath)
{
    namespace fs = std::filesystem;
    const std::string safe = SanitizeProfileName(name);
    std::error_code ec;
    fs::path dir = Config_VideoProfilesDir();
    fs::create_directories(dir, ec);
    if (ec) { LOG_WARN("config: cannot create {}: {}", dir.string(), ec.message()); return false; }
    fs::path dest = dir / (safe + ".ini");
    fs::path tmp = dest;
    tmp += ".tmp";
    {
        std::ofstream f(tmp);
        if (!f) return false;
        f << Config_VideoProfileText(g_config);
    }
    if (!Platform_ReplaceFile(tmp.string(), dest.string()))
    {
        fs::remove(tmp, ec);
        return false;
    }
    if (outPath) *outPath = dest.string();
    LOG_INFO("config: saved video profile {}", dest.string());
    return true;
}

bool Config_LoadVideoProfile(const std::string& name)
{
    namespace fs = std::filesystem;
    const std::string safe = SanitizeProfileName(name);
    fs::path path = Config_VideoProfilesDir() / (safe + ".ini");
    std::ifstream f(path);
    if (!f) { LOG_WARN("config: video profile not found: {}", path.string()); return false; }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (!Config_ApplyVideoProfileText(ss.str(), g_config)) return false;
    LOG_INFO("config: loaded video profile {}", path.string());
    return true;
}

bool Config_Save()
{
    if (s_path.empty()) return false;
    // write-then-rename so a crash never leaves a truncated file
    std::string tmp = s_path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) { LOG_WARN("config: cannot write {}", tmp); return false; }
    std::string text = Config_Text(g_config);
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = fclose(f) == 0 && ok;
    if (ok) ok = Platform_ReplaceFile(tmp, s_path);
    if (!ok) LOG_WARN("config: cannot save {}", s_path);
    return ok;
}

void Config_StampLastPlayed()
{
    using clock = std::chrono::system_clock;
    const auto t = clock::to_time_t(clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    g_config.lastPlayedAt = std::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}", tm.tm_year + 1900, tm.tm_mon + 1,
                                        tm.tm_mday, tm.tm_hour, tm.tm_min);
    Config_Save();
}

static std::string Trim(std::string s)
{
    auto ws = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && ws(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && ws(s[i])) i++;
    return s.substr(i);
}

static std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}

static bool ParseBool(const std::string& v)
{
    std::string l = Lower(v);
    return l == "1" || l == "true" || l == "yes" || l == "on";
}

static int ParseLanguage(const std::string& v)
{
    std::string l = Lower(v);
    for (int i = 0; i < 7; i++)
        if (l == kLanguages[i]) return i;
    if (!l.empty() && isdigit((unsigned char)l[0])) return std::clamp(atoi(l.c_str()), 0, 6);
    LOG_WARN("config: unknown language '{}', using english", v);
    return 1;
}

void Config_Load(const std::string& path)
{
    s_path = path;
    std::ifstream f(path);
    if (!f)
    {
        Config_Save();   // first run: write the defaults
        AudioOut_ApplyVolumes(g_config.masterVolume, g_config.speakerVolume);
        return;
    }
    std::string line, section;
    while (std::getline(f, line))
    {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[')
        {
            section = Lower(Trim(line.substr(1, line.find(']') - 1)));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Lower(Trim(line.substr(0, eq)));
        std::string val = Trim(line.substr(eq + 1));
        Config& c = g_config;
        if (section == "video")
        {
            if (key == "frame_rate")
            {
                int v = atoi(val.c_str());
                c.frameRate = (v <= 0) ? 0 : (v >= 60 ? 60 : 30);
            }
            else if (key == "host_fps_cap")
                c.hostFpsCap = std::clamp(atoi(val.c_str()), 0, 1000);
            else if (key == "internal_scale") c.internalScale = std::clamp(atoi(val.c_str()), 1, 8);
            else if (key == "window_width") c.windowWidth = std::max(320, atoi(val.c_str()));
            else if (key == "window_height") c.windowHeight = std::max(240, atoi(val.c_str()));
            else if (key == "window_maximized") c.windowMaximized = ParseBool(val);
            else if (key == "fullscreen") c.fullscreen = ParseBool(val);
            else if (key == "exclusive_fullscreen") c.exclusiveFullscreen = ParseBool(val);
            else if (key == "vsync") c.vsync = ParseBool(val);
            else if (key == "widescreen") c.widescreen = ParseBool(val);
            else if (key == "integer_scale") c.integerScale = ParseBool(val);
            else if (key == "texture_pack") c.texturePack = ParseBool(val);
            else if (key == "gpu_thread") c.gpuThread = ParseBool(val);
            else if (key == "async_shaders") c.asyncShaders = ParseBool(val);
            else if (key == "anisotropy") c.anisotropy = std::clamp(atoi(val.c_str()), 1, 16);
            else if (key == "texture_pack_dir") c.texturePackDir = val;
        }
        else if (section == "system")
        {
            if (key == "language") c.language = ParseLanguage(val);
            else if (key == "pause_on_focus_loss") c.pauseOnFocusLoss = ParseBool(val);
            else if (key == "show_fps_overlay") c.showFpsOverlay = ParseBool(val);
            else if (key == "show_input_hints") c.showInputHints = ParseBool(val);
            else if (key == "auto_backup_saves_on_launch") c.autoBackupSavesOnLaunch = ParseBool(val);
            else if (key == "auto_backup_saves_on_write") c.autoBackupSavesOnWrite = ParseBool(val);
            else if (key == "save_backup_keep") c.saveBackupKeep = std::clamp(atoi(val.c_str()), 0, 200);
            else if (key == "screenshot_keep") c.screenshotKeep = std::clamp(atoi(val.c_str()), 0, 1000);
            else if (key == "ui_scale")
                c.uiScale = std::clamp(std::strtof(val.c_str(), nullptr), 0.75f, 1.50f);
            else if (key == "hotkey_settings") c.hotkeySettings = Config_HotkeyName(val, "F1");
            else if (key == "hotkey_screenshot") c.hotkeyScreenshot = Config_HotkeyName(val, "F12");
            else if (key == "hotkey_fullscreen") c.hotkeyFullscreen = Config_HotkeyName(val, "F11");
            else if (key == "launcher_tab") c.launcherTab = std::clamp(atoi(val.c_str()), 0, 3);
            else if (key == "settings_tab") c.settingsTab = std::clamp(atoi(val.c_str()), 0, 3);
            else if (key == "launcher_width") c.launcherWidth = std::clamp(atoi(val.c_str()), 800, 7680);
            else if (key == "launcher_height") c.launcherHeight = std::clamp(atoi(val.c_str()), 560, 4320);
            else if (key == "last_seen_version") c.lastSeenVersion = val;
            else if (key == "last_played_at") c.lastPlayedAt = val;
        }
        else if (section == "audio")
        {
            if (key == "master_volume") c.masterVolume = std::clamp(std::strtof(val.c_str(), nullptr), 0.0f, 1.0f);
            else if (key == "speaker_volume") c.speakerVolume = std::clamp(std::strtof(val.c_str(), nullptr), 0.0f, 1.0f);
            else if (key == "audio_device") c.audioDevice = val;
        }
        else if (section == "input")
        {
            if (key == "rumble") c.rumble = ParseBool(val);
            else if (key == "remote_speaker") c.remoteSpeaker = ParseBool(val);
            else if (key == "stick_deadzone")
                c.stickDeadzone = std::clamp(std::strtof(val.c_str(), nullptr), 0.0f, 0.5f);
            else if (key == "stick_deadzone_right")
                c.stickDeadzoneRight = std::clamp(std::strtof(val.c_str(), nullptr), 0.0f, 0.5f);
            else if (key == "trigger_deadzone")
                c.triggerDeadzone = std::clamp(std::strtof(val.c_str(), nullptr), 0.0f, 0.5f);
            for (int i = 0; i < 12; i++)
                if (key == kBindActions[i]) c.bind[i] = val;
        }
        else if (section == "keyboard")
        {
            for (int i = 0; i < 21; i++)
                if (key == kKeyActions[i]) c.keys[i] = val;
        }
        else if (section == "mods")
        {
            if (!val.empty() && (key == "on" || key == "off")) c.mods.push_back({ val, key == "on" });
        }
        else if (section == "paths")
        {
            if (key == "data") c.dataDir = val;
            else if (key == "iso") c.isoPath = val;
            else if (key == "mods") c.modsDir = val;
        }
        else if (section == "music")
        {
            if (key == "enabled") c.musicModsEnabled = ParseBool(val);
            else if (key == "music_dir") c.musicModsDir = val;
            else if (key == "filter_cat") c.musicFilterCat = std::clamp(atoi(val.c_str()), 0, 7);
            else if (key == "filter_var") c.musicFilterVar = std::clamp(atoi(val.c_str()), 0, 2);
            else if (key == "filter_repl") c.musicFilterRepl = std::clamp(atoi(val.c_str()), 0, 2);
            else if (key == "filter_world") c.musicFilterWorld = std::clamp(atoi(val.c_str()), 0, 7);
        }
    }
    AudioOut_ApplyVolumes(g_config.masterVolume, g_config.speakerVolume);
    // Device open happens in AudioOut_Init / ApplyLive (needs SDL audio subsystem).
}
