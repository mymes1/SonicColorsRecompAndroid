// Android process, paths, time, threads and dialogs.
//
// This file is deliberately free of Android-only headers: it only uses <filesystem>,
// POSIX threads and <cstdio>, so the *same* source builds on device (Bionic) and on a
// Linux CI host. The few services that genuinely need the platform (thread priority,
// timers, dialogs) are routed through small helpers in platform_android_host.cpp.
#include "platform_android.h"

#include "../../log.h"
#include "../../platform/platform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

std::vector<std::string> g_androidArgs;
std::string g_androidDataDir;
std::string g_androidIsoPath;

namespace
{
std::string s_userDir;
std::string s_exeDir;
std::atomic<bool> s_portable{ false };

uint64_t NowNs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

bool Writable(const std::string& dir)
{
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    const fs::path probe = fs::path(dir) / ".scr_write_test";
    FILE* f = fopen(probe.c_str(), "wb");
    if (!f) return false;
    fclose(f);
    fs::remove(probe, ec);
    return true;
}
}   // namespace

// ------------------------------------------------------------------ process
void Platform_Init(int argc, char** argv)
{
    g_androidArgs.clear();
    for (int i = 0; i < argc; i++) g_androidArgs.push_back(argv[i]);
    androidapp::SetThreadName("main");
}

void Platform_RunGame(void (*game)())
{
    // The caller (android_main) already runs on the game thread; guest code uses the
    // host stack, and the fiber stack is allocated large by Threads_Init.
    game();
}

void Platform_LogHost()
{
    LOG_INFO("android: {} {} | device {} | ABI {}", androidapp::SdkInt() > 0 ? "Android" : "host",
             androidapp::AndroidRelease().c_str(), androidapp::DeviceModel().c_str(), androidapp::Abi().c_str());
}

void Platform_Terminate(int code)
{
    fflush(stdout);
    fflush(stderr);
    LOG_INFO("exiting ({})", code);
    _exit(code);
}

void Platform_ExitProcess(int code) { Platform_Terminate(code); }

void Platform_Restart(bool, bool terminate)
{
    // Restarting the guest means rebooting the VM in-process; not supported on Android.
    LOG_WARN("restart requested: the app will exit instead (settings apply on next launch)");
    Platform_Terminate(0);
    (void)terminate;
}

std::string Platform_AppImagePath() { return {}; }

void Platform_RelaunchExecutable(const std::string&)
{
    LOG_WARN("relaunch is not supported on Android");
    Platform_Terminate(0);
}

uint32_t Platform_ProcessId() { return (uint32_t)getpid(); }

bool Platform_SpawnDetached(const std::vector<std::string>&) { return false; }

// ------------------------------------------------------------------ paths
fs::path Platform_ExeDir()
{
    // The APK's native library directory: read-only, and where bundled read-only assets
    // (shipped shader cache) live.
    if (!s_exeDir.empty()) return s_exeDir;
    s_exeDir = androidapp::NativeLibDir();
    if (s_exeDir.empty()) s_exeDir = "/data/local/tmp";
    return s_exeDir;
}

void Platform_RequestPortable() { s_portable = true; }
bool Platform_IsPortable() { return s_portable.load(); }
bool Platform_IsInstalledLayout() { return false; }

fs::path Platform_UserDir()
{
    if (!s_userDir.empty() && (s_portable.load() || Writable(s_userDir))) return s_userDir;
    std::error_code ec;
    // Portable: next to the (writable) data directory.
    if (s_portable.load())
    {
        fs::path dir = fs::path(g_androidDataDir.empty() ? androidapp::InternalFilesDir() : g_androidDataDir) / "userdata";
        fs::create_directories(dir, ec);
        if (Writable(dir.string())) { s_userDir = dir.string(); return s_userDir; }
    }
    // Default: files/internal/userdata, which the app owns and no permission governs.
    fs::path dir = fs::path(androidapp::InternalFilesDir()) / "userdata";
    fs::create_directories(dir, ec);
    if (Writable(dir.string()))
    {
        s_userDir = dir.string();
        return s_userDir;
    }
    // Last resort: the cache dir (always writable, may be evicted).
    s_userDir = androidapp::CacheDir();
    return s_userDir;
}

fs::path Platform_MusicToolsBinDir()
{
    fs::path dir = Platform_UserDir() / "tools" / "bin";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

fs::path Platform_DolphinTextureDir()
{
    fs::path dir = Platform_UserDir() / "Load" / "Textures";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

bool Platform_ReplaceFile(const std::string& from, const std::string& to)
{
    // rename() is atomic within one filesystem; fall back to copy + rename for SAF volumes.
    if (rename(from.c_str(), to.c_str()) == 0) return true;
    std::error_code ec;
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    fs::remove(from, ec);
    return true;
}

bool Platform_RunProcess(const std::vector<std::string>&, const fs::path&, const fs::path&, int& exitCode)
{
    exitCode = -1;
    return false;   // no process spawning on Android
}

unsigned Platform_LastRunProcessError() { return 0; }

std::string Platform_UiFont() { return {}; }
std::vector<std::string> Platform_UiFallbackFonts() { return {}; }

std::string Platform_Utf8(const wchar_t* s)
{
    if (!s) return {};
    std::string out;
    for (const wchar_t* p = s; *p; p++)
    {
        uint32_t cp = (uint32_t)*p;
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800)
        {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
        else
        {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

// ------------------------------------------------------------------ time
uint64_t Platform_TickMs() { return NowNs() / 1000000; }
uint64_t Platform_PerfCounter() { return NowNs(); }
uint64_t Platform_PerfFrequency() { return 1000000000ull; }

uint64_t Platform_ProcessRssMb()
{
    FILE* f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    long pages = 0, resident = 0;
    if (fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
    fclose(f);
    return (uint64_t)resident * (uint64_t)(sysconf(_SC_PAGESIZE) / 1024) / 1024;
}

void Platform_SleepMs(uint32_t ms)
{
    if (!ms) return;
    timespec ts{ (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, nullptr);
}

// ------------------------------------------------------------------ threads
void Thread_SetName(const char* name)
{
    if (!name) return;
    androidapp::SetThreadName(name);
}

void Thread_SetPriority(ThreadPriority p)
{
    // Big.LITTLE: "below normal" work (shader compile) should not land on the big cores
    // and starve the guest. Setting the nice value is enough on Android (permission for
    // SCHED_FIFO is not granted to apps).
    if (p == ThreadPriority::BelowNormal) setpriority(PRIO_PROCESS, (id_t)gettid(), 10);
    else sched_yield();
}

struct AndroidEvent
{
    std::mutex m;
    std::condition_variable cv;
    bool set = false;
};

PlatformEvent::PlatformEvent() : impl(new AndroidEvent()) {}
PlatformEvent::~PlatformEvent() { delete (AndroidEvent*)impl; }

void PlatformEvent::Set()
{
    auto* e = (AndroidEvent*)impl;
    {
        std::lock_guard lk(e->m);
        e->set = true;
    }
    e->cv.notify_one();
}

void PlatformEvent::Wait(uint32_t ms)
{
    auto* e = (AndroidEvent*)impl;
    std::unique_lock lk(e->m);
    if (e->set)
    {
        e->set = false;
        return;
    }
    if (ms == 0xFFFFFFFFu)
    {
        e->cv.wait(lk, [&] { return e->set; });
        e->set = false;
        return;
    }
    e->cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return e->set; });
    e->set = false;
}

void Platform_TimerThreadSetup()
{
    // The scheduler's timer thread benefits from "audio"-like priority, which apps cannot
    // request; a normal-priority thread with clock_nanosleep is what we can do.
    setpriority(PRIO_PROCESS, (id_t)gettid(), -2);
}

void Platform_TimerWait(PlatformEvent& wake, uint64_t hns)
{
    // Absolute CLOCK_MONOTONIC deadline (no drift under load), woken early by the event:
    // a short first slice checks `wake`, the rest is one plain sleep.
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const uint64_t nowNs = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
    const uint64_t first = std::min<uint64_t>(hns, 1000000ull);
    timespec ts{};
    uint64_t at = nowNs + first;
    ts.tv_sec = (time_t)(at / 1000000000ull);
    ts.tv_nsec = (long)(at % 1000000000ull);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {}
    wake.Wait(0);
    if (hns > first)
    {
        at = nowNs + hns;
        ts.tv_sec = (time_t)(at / 1000000000ull);
        ts.tv_nsec = (long)(at % 1000000000ull);
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {}
    }
}

// ------------------------------------------------------------------ memory
void* Platform_AllocPages(size_t bytes)
{
    void* p = std::calloc(1, bytes);
    return p;
}

// ------------------------------------------------------------------ dialogs
MsgResult Platform_MessageBox(const std::string& title, const std::string& text, MsgButtons, MsgIcon)
{
    LOG_ERROR("{}: {}", title, text);
    return MsgResult::Ok;
}

void Platform_ErrorBox(const std::string& title, const std::string& text)
{
    LOG_ERROR("{}: {}", title, text);
}
