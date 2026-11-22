// Android runtime entry point.
//
// The Activity (org.soniccolorsrecomp.android.MainActivity) owns the NativeActivity
// lifecycle: it starts this app once a disc is installed, passes the install/data
// directories and the Surface through the process environment, and forwards touch,
// gamepad and lifecycle events with JNI calls handled by input_bridge.cpp and
// android_app.cpp. Nothing here is reached until the surface exists.
#include "app.h"

#include "../../config.h"
#include "../../gx/video.h"
#include "../../kernel/boot.h"
#include "../../cpu/cpu.h"
#include "../../kernel/scheduler.h"
#include "../../kernel/crash.h"
#include "../../kernel/memory.h"
#include "../../kernel/threads.h"
#include "../../hw/hw.h"
#include "../../ios/disc.h"
#include "../../ios/ios.h"
#include "../../ios/nand_paths.h"
#include "../../install/install.h"
#include "../../install/releases.h"
#include "../../install/verify.h"
#include "../../audio/music_mod.h"
#include "../../log.h"
#include "../../platform/platform.h"
#include "../../region.h"
#include "../../version.h"
#include "../../audio/audio_out.h"
#include "input.h"
#include "platform_android.h"

#include <atomic>
#include <cstdio>
#include <string_view>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

void NAND_InitSystemFiles();
void Renderer_Init();
void Crash_Init();
void Crash_StartWatchdog();
extern bool g_noMessageBox;
extern int g_logLevel;
extern "C" uint32_t g_cfgFrameRate;

bool g_vkValidation = false;

namespace
{
std::string s_dataDir, s_nand;

void BootAndRun()
{
    if (!Memory_Init()) FATAL("memory init failed");
    Crash_Init();
    Cpu_Init();
    Sched_Init();
    Threads_Init();
    HW_Init();
    Renderer_Init();     // creates the Vulkan backend on the Surface's window
    AudioOut_Init();     // AAudio output
    AudioOut_ApplyVolumes(g_config.masterVolume, g_config.speakerVolume);
    MusicMod_Init();
    {
        extern bool g_gpuThread;
        if (g_gpuThread) GX_StartThread();
    }
    if (!Disc_Open(s_dataDir, g_androidIsoPath, Mods_Loaded())) FATAL("cannot open disc data");
    std::filesystem::create_directories(s_nand);
    IOS_Init(s_nand);
    NAND_InitSystemFiles();
    const uint32_t entry = Boot_Setup(s_dataDir);
    if (!entry) FATAL("boot failed");
    Crash_StartWatchdog();
    androidapp::SetStatus("Running");
    Threads_RunMain(entry);   // never returns
}
}   // namespace

int AndroidMain(std::vector<std::string> args)
{
    g_androidArgs = std::move(args);
    // Environment the Activity sets (see MainActivity.startRuntime): data dir, disc, log.
    const char* dataEnv = std::getenv("SCR_ANDROID_DATA");
    const char* isoEnv = std::getenv("SCR_ANDROID_ISO");
    const char* logEnv = std::getenv("SCR_ANDROID_LOG");
    const char* portableEnv = std::getenv("SCR_PORTABLE");
    if (portableEnv && portableEnv[0] == '1') Platform_RequestPortable();

    std::string userDir = Platform_UserDir().string();
    Log_Open(((logEnv && *logEnv) ? std::string(logEnv) : userDir + "/SonicColorsRecomp.log").c_str());
    LOG_INFO("SonicColorsRecomp {} (android), {} {} | API {}", std::string_view(g_version),
             androidapp::DeviceModel(), androidapp::Abi(), androidapp::SdkInt());
    Platform_LogHost();
    if (const char* v = std::getenv("SCR_ANDROID_VULKAN_VALIDATION"); v && v[0] == '1') g_vkValidation = true;

    Config_Load(userDir + "/config.ini");
    if (dataEnv && *dataEnv) g_androidDataDir = dataEnv;
    if (isoEnv && *isoEnv) g_androidIsoPath = isoEnv;

    // The Activity launches the runtime only after installing the data, but a reinstall
    // can remove it: report the state instead of crashing in Disc_Open.
    if (g_androidDataDir.empty()) g_androidDataDir = (std::filesystem::path(androidapp::InternalFilesDir()) / "game").string();
    std::string problem;
    const DataStatus st = Install_QuickCheck(g_androidDataDir, problem);
    if (st != DataStatus::Ok)
    {
        androidapp::SetStatus(std::string("Game data ") + (st == DataStatus::Missing ? "missing" : "damaged") +
                              ": " + problem);
        LOG_ERROR("game data {} at {}: {}", st == DataStatus::Missing ? "missing" : "damaged", g_androidDataDir, problem);
        return 1;
    }
    s_dataDir = g_androidDataDir;
    std::string gameId = Release_InstalledId(s_dataDir);
    {
        std::string error;
        if (!Region_Select(gameId, error)) FATAL("cannot start the game: {}", error);
    }
    Mods_SetLoaded(Mods_Scan(Mods_Roots(g_config.modsDir), g_config.mods, gameId));

    // Saves live in files/nand so they survive app updates and are included in backups.
    s_nand = (std::filesystem::path(userDir)).string();
    if (const char* nd = std::getenv("SCR_TEST_NAND_DIR"); nd && *nd) s_nand = nd;

    g_cfgFrameRate = (uint32_t)g_config.frameRate;
    g_videoConfig.internalScale = g_config.internalScale;
    g_videoConfig.widescreen = g_config.widescreen;
    g_videoConfig.vsync = g_config.vsync;
    {
        extern bool g_gpuThread;
        g_gpuThread = g_config.gpuThread;
    }
    LOG_INFO("data={} nand={} fps={} scale={}", s_dataDir, s_nand, g_cfgFrameRate, g_videoConfig.internalScale);
    BootAndRun();
    return 0;
}
