#include "log.h"
#include "platform/compat.h"
#include "platform/platform.h"

#include <cstdlib>
#include <filesystem>
#ifndef __ANDROID__
#include <format>   // on Android, platform/compat.h provides it
#endif
#include <mutex>

namespace fs = std::filesystem;

static std::mutex s_logMutex;
static FILE* s_logFile = nullptr;
static std::string s_logPath;
int g_logLevel = 1;
bool g_noMessageBox = false;

namespace
{
// Keep the previous N runs as SonicColorsRecomp.1.log … .N.log (newest = .1).
constexpr int kLogKeep = 5;

void RotatePrevious(const fs::path& path)
{
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return;
    const fs::path dir = path.has_parent_path() ? path.parent_path() : fs::path(".");
    const std::string stem = path.stem().string();   // SonicColorsRecomp
    const std::string ext = path.extension().string();   // .log
    fs::remove(dir / std::format("{}.{}{}", stem, kLogKeep, ext), ec);
    for (int i = kLogKeep - 1; i >= 1; --i)
    {
        const fs::path from = dir / std::format("{}.{}{}", stem, i, ext);
        const fs::path to = dir / std::format("{}.{}{}", stem, i + 1, ext);
        if (fs::is_regular_file(from, ec))
            fs::rename(from, to, ec);
    }
    fs::rename(path, dir / std::format("{}.1{}", stem, ext), ec);
}
}   // namespace

void Log_Open(const char* path)
{
    s_logPath = path ? path : "";
    if (!s_logPath.empty())
        RotatePrevious(fs::path(s_logPath));
    // shared so the log can be read while the game is running
    s_logFile = _fsopen(path, "w", _SH_DENYWR);
}

const std::string& Log_Path() { return s_logPath; }

namespace
{
fs::path AbnormalExitPath()
{
    if (s_logPath.empty()) return {};
    return fs::path(s_logPath).parent_path() / "last_abnormal_exit.txt";
}
}   // namespace

void Log_MarkAbnormalExit(const char* reason)
{
    const fs::path p = AbnormalExitPath();
    if (p.empty()) return;
    FILE* f = _fsopen(p.string().c_str(), "w", _SH_DENYWR);
    if (!f) return;
    fprintf(f, "%s\n", reason && reason[0] ? reason : "abnormal exit");
    fflush(f);
    fclose(f);
}

bool Log_TakeAbnormalExit(std::string& reason)
{
    reason.clear();
    const fs::path p = AbnormalExitPath();
    if (p.empty()) return false;
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return false;
    FILE* f = _fsopen(p.string().c_str(), "r", _SH_DENYWR);
    if (!f)
    {
        fs::remove(p, ec);
        return false;
    }
    char buf[512] = {};
    if (fgets(buf, sizeof(buf), f))
    {
        reason = buf;
        while (!reason.empty() && (reason.back() == '\n' || reason.back() == '\r'))
            reason.pop_back();
    }
    fclose(f);
    fs::remove(p, ec);
    if (reason.empty()) reason = "Previous session ended abnormally.";
    return true;
}

void Log_Write(int level, const std::string& s)
{
    if (level < g_logLevel)
        return;
    static const char* kTag[] = { "D", "I", "W", "E" };
    std::lock_guard<std::mutex> lk(s_logMutex);
    fprintf(stdout, "[%s] %s\n", kTag[level & 3], s.c_str());
    fflush(stdout);
    if (s_logFile)
    {
        fprintf(s_logFile, "[%s] %s\n", kTag[level & 3], s.c_str());
        fflush(s_logFile);
    }
}

void Fatal(const std::string& s)
{
    Log_Write(3, "FATAL: " + s);
    fflush(stdout);
    Log_MarkAbnormalExit(("FATAL: " + s).c_str());
    if (!g_noMessageBox) Platform_ErrorBox("Sonic Colors - Recompiled — fatal error", s);
    { void Gamepad_StopRumble(); Gamepad_StopRumble(); }
    Platform_ExitProcess(1);
}
