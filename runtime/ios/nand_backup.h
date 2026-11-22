#pragma once
// Save-tree backup/restore without linking IOS / platform (for unit tests and tooling).
#include <algorithm>
#include <chrono>
#include <filesystem>
#ifndef __ANDROID__
#include <format>   // on Android, platform/compat.h provides it
#endif
#include <string>
#include <vector>

namespace nand_backup
{
namespace fs = std::filesystem;

inline std::string TimestampName()
{
    using clock = std::chrono::system_clock;
    auto t = clock::to_time_t(clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return std::format("{:04d}{:02d}{:02d}_{:02d}{:02d}{:02d}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                       tm.tm_hour, tm.tm_min, tm.tm_sec);
}

inline bool BackupTree(const fs::path& root, fs::path& outDir, std::string& error)
{
    std::error_code ec;
    if (!fs::exists(root, ec))
    {
        error = "No save data folder yet (" + root.string() + ")";
        return false;
    }
    outDir = root / "save_backups" / TimestampName();
    fs::create_directories(outDir, ec);
    if (ec)
    {
        error = "Cannot create " + outDir.string() + ": " + ec.message();
        return false;
    }
    bool any = false;
    for (const auto& e : fs::directory_iterator(root, ec))
    {
        if (ec) break;
        if (e.path().filename() == "save_backups") continue;
        fs::copy(e.path(), outDir / e.path().filename(),
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            error = "Copy failed: " + ec.message();
            return false;
        }
        any = true;
    }
    if (!any)
    {
        error = "Save folder is empty — nothing to back up";
        fs::remove_all(outDir, ec);
        return false;
    }
    return true;
}

inline bool LatestBackupIn(const fs::path& root, fs::path& out)
{
    fs::path dir = root / "save_backups";
    std::error_code ec;
    if (!fs::exists(dir, ec)) return false;
    fs::path best;
    for (const auto& e : fs::directory_iterator(dir, ec))
    {
        if (ec || !e.is_directory()) continue;
        if (best.empty() || e.path().filename() > best.filename()) best = e.path();
    }
    if (best.empty()) return false;
    out = best;
    return true;
}

// Newest-first list of save_backups/* directories (timestamp names sort lexicographically).
inline std::vector<fs::path> ListBackupsIn(const fs::path& root)
{
    std::vector<fs::path> out;
    fs::path dir = root / "save_backups";
    std::error_code ec;
    if (!fs::exists(dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir, ec))
    {
        if (ec || !e.is_directory()) continue;
        out.push_back(e.path());
    }
    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) {
        return a.filename() > b.filename();
    });
    return out;
}

// Keep the newest `keep` timestamp folders under save_backups/. 0 = unlimited.
// Returns how many directories were removed.
inline size_t PruneBackupsIn(const fs::path& root, int keep)
{
    if (keep <= 0) return 0;
    auto listed = ListBackupsIn(root);   // newest-first
    if ((int)listed.size() <= keep) return 0;
    size_t removed = 0;
    std::error_code ec;
    for (size_t i = (size_t)keep; i < listed.size(); i++)
    {
        fs::remove_all(listed[i], ec);
        if (!ec) removed++;
        ec.clear();
    }
    return removed;
}

inline bool RestoreTree(const fs::path& root, const fs::path& backup, std::string& error)
{
    if (backup.empty() || !fs::is_directory(backup))
    {
        error = "Backup folder missing";
        return false;
    }
    std::error_code ec;
    fs::create_directories(root, ec);
    for (const auto& e : fs::directory_iterator(root, ec))
    {
        if (ec) break;
        if (e.path().filename() == "save_backups") continue;
        fs::remove_all(e.path(), ec);
        if (ec)
        {
            error = "Could not clear existing save: " + ec.message();
            return false;
        }
    }
    for (const auto& e : fs::directory_iterator(backup, ec))
    {
        if (ec) break;
        fs::copy(e.path(), root / e.path().filename(),
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            error = "Restore copy failed: " + ec.message();
            return false;
        }
    }
    return true;
}
}   // namespace nand_backup
