#include "music_tool.h"
#include "music_deps.h"
#include "../platform/platform.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

std::filesystem::path MusicTool_Find()
{
    if (const char* env = getenv("SCR_MUSIC_ASSET"))
    {
        fs::path p(env);
        std::error_code ec;
        if (fs::exists(p, ec)) return fs::weakly_canonical(p, ec);
    }

    // Prefer the install tree over cwd so packaged tools/music_asset is found when
    // the launcher is started from a different working directory (common on Windows).
    const fs::path roots[] = {
        Platform_ExeDir(),
        Platform_ExeDir() / "..",
        Platform_ExeDir() / "../..",
        fs::current_path(),
    };

#ifdef _WIN32
    const char* names[] = {
        "music_asset.exe",
        "music_asset.cmd",
        "music_asset.bat",
        "music_asset",
    };
#else
    const char* names[] = { "music_asset" };
#endif

    for (const fs::path& base : roots)
    {
        for (const char* name : names)
        {
            std::error_code ec;
            // Prefer beside the launcher first (dev out-windows / music_asset.exe + winpthread).
            // Packaged builds still find tools/music_asset.exe on the next check.
            fs::path p = base / name;
            if (fs::exists(p, ec)) return fs::weakly_canonical(p, ec);
            p = base / "tools" / name;
            if (fs::exists(p, ec)) return fs::weakly_canonical(p, ec);
        }
    }
    return {};
}

bool MusicTool_Run(const std::vector<std::string>& args,
                   const fs::path& stdoutPath,
                   const fs::path& stderrPath,
                   int& exitCode)
{
    const fs::path tool = MusicTool_Find();
    if (tool.empty())
    {
        exitCode = -1;
        if (!stderrPath.empty())
        {
            std::error_code ec;
            fs::create_directories(stderrPath.parent_path(), ec);
            FILE* f = std::fopen(stderrPath.string().c_str(), "wb");
            if (f)
            {
                std::fprintf(f, "MusicTool_Run: music_asset helper not found "
                                "(checked SCR_MUSIC_ASSET and tools/ beside the app).\n");
                std::fclose(f);
            }
        }
        return false;
    }
    // Point music_asset at the user-writable drop-in dir (survives updates).
    // On Windows, CreateProcess inherits the Win32 env block — SetEnvironmentVariableW
    // is required; _putenv_s alone does not update what child processes see.
    const fs::path userBin = Platform_MusicToolsBinDir();
#ifdef SCR_ANDROID
    // The managed-dependency downloader (audio/music_deps.cpp) is a Windows feature and is not
    // built for the APK; Android never runs this tool at all (see MusicMod_Init).
#else
    MusicDeps_ApplyEnv();
#endif
#ifdef _WIN32
    SetEnvironmentVariableW(L"SCR_MUSIC_BIN", userBin.wstring().c_str());
    // Narrow CRT copy for in-process getenv; Win32 block above is what CreateProcess inherits.
    const std::string binUtf8 = userBin.string();
    _putenv_s("SCR_MUSIC_BIN", binUtf8.c_str());
#else
    setenv("SCR_MUSIC_BIN", userBin.string().c_str(), 1);
#endif
    std::vector<std::string> argv;
    argv.reserve(args.size() + 1);
    argv.push_back(tool.string());
    argv.insert(argv.end(), args.begin(), args.end());
    const bool ok = Platform_RunProcess(argv, stdoutPath, stderrPath, exitCode);
    if (!ok && !stderrPath.empty())
    {
        // Platform_RunProcess may already have written a CreateProcess diagnostic.
        std::error_code ec;
        const auto sz = fs::file_size(stderrPath, ec);
        if (ec || sz == 0)
        {
            FILE* f = std::fopen(stderrPath.string().c_str(), "wb");
            if (f)
            {
                std::fprintf(f,
                             "MusicTool_Run: failed to start \"%s\" (platform error %u).\n",
                             tool.string().c_str(), Platform_LastRunProcessError());
                std::fclose(f);
            }
        }
    }
    return ok;
}
