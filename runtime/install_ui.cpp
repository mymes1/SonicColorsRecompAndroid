// First-run installer: asks for the disc image and installs the game data from it
#include "install/install.h"
#include "install/releases.h"
#include "config.h"
#include "log.h"
#include "platform/platform.h"
#include "platform/ui.h"

#include <atomic>
#include <filesystem>
#ifndef __ANDROID__
#include <format>   // on Android, platform/compat.h provides it
#endif
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace
{
constexpr const char* kTitle = "Sonic Colors - Recompiled";

std::string Utf8(const fs::path& p)
{
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

InstallResult RunWithProgress(const fs::path& image, const fs::path& dest, std::string& error)
{
    InstallProgress progress;
    InstallResult result = InstallResult::Canceled;
    ProgressDialog dlg(kTitle, "Installing the game data from your disc image...", Utf8(image));
    std::atomic<bool> finished{ false };
    std::thread worker([&] {
        result = Install_Run(image, dest, progress, error);
        finished = true;
    });
    for (;;)
    {
        for (int i = 0; i < 10 && !finished; i++) Platform_SleepMs(10);
        if (finished) break;
        if (dlg.Update(progress.done, progress.total ? progress.total.load() : 1)) progress.cancel = true;
    }
    worker.join();
    return result;
}
}   // namespace

// Returns true once the data is installed in dest. foundImage: a disc image found next to
// the game, offered first. damage: why the existing install needs replacing (empty when
// nothing is installed yet).
bool InstallUI_Run(const fs::path& dest, const std::string& foundImage, const std::string& damage)
{
    std::string why = damage.empty()
        ? "Sonic Colors game data is not installed yet."
        : "The installed Sonic Colors game data is damaged (" + damage + ") and needs reinstalling.";
    // Supported releases come from runtime/install/releases.cpp (currently Europe SNCP8P).
    std::string freeHint;
    {
        std::error_code ec;
        fs::path probe = dest.has_parent_path() ? dest.parent_path() : dest;
        while (!probe.empty() && !fs::exists(probe, ec))
        {
            if (!probe.has_parent_path() || probe.parent_path() == probe) break;
            probe = probe.parent_path();
        }
        const fs::space_info si = fs::space(probe.empty() ? fs::path(".") : probe, ec);
        if (!ec)
        {
            const double gb = (double)si.available / (1024.0 * 1024.0 * 1024.0);
            freeHint = std::format("\nFree space at install location: {:.1f} GB.", gb);
            if (si.available < (4ull << 30))
                freeHint += " (Warning: under 4 GB — install may fail.)";
        }
    }
    std::string need = std::string("Supported release: ") + kReleaseNames +
        ".\nUSA (SNCE8P) and Japan (SNCJ8P) discs are not supported yet.\n\n"
        "Select your own Wii disc image of Sonic Colors (ISO, RVZ, WIA, WBFS, or GCZ). "
        "Installation takes about a minute and needs roughly 4 GB of free space"
        " (about 8 GB free if reinstalling over existing data); "
        "the disc image is not needed afterwards." +
        freeHint;
    bool installed = false;
    fs::path image;
    std::string offer = foundImage;
    // Fall back to the last successfully used disc path from config.ini.
    if (offer.empty() && !g_config.isoPath.empty() && fs::exists(g_config.isoPath) &&
        Install_LooksLikeDiscImage(g_config.isoPath) && Release_Find(Install_GameId(g_config.isoPath)))
        offer = g_config.isoPath;
    if (!offer.empty() && Release_Find(Install_GameId(offer)))
    {
        const char* kind = (offer == foundImage) ? "found next to the game" : "used last time";
        std::string msg = why + "\n\nInstall from the disc image " + kind + "?\n\n" + offer +
                          "\n\nChoose No to pick a different image.";
        MsgResult r = Platform_MessageBox(kTitle, msg, MsgButtons::YesNoCancel, MsgIcon::Question);
        if (r == MsgResult::Yes) image = offer;
        else if (r == MsgResult::Cancel) return false;
    }
    else
    {
        if (!foundImage.empty())
        {
            // An image was found but is not a supported Colors release (wrong game / region).
            std::string id = Install_GameId(foundImage);
            why += "\n\nA disc image was found at:\n" + foundImage + "\n\n" + Release_UnsupportedMessage(id);
        }
        MsgResult r = Platform_MessageBox(kTitle, why + "\n\n" + need, MsgButtons::OkCancel, MsgIcon::Info);
        if (r != MsgResult::Ok) return false;
    }
    for (;;)
    {
        if (image.empty() && !Dialog_PickDiscImage(image)) break;
        LOG_INFO("installing game data from {} to {}", image.string(), dest.string());
        std::string error;
        InstallResult r = RunWithProgress(image, dest, error);
        if (r == InstallResult::Ok)
        {
            LOG_INFO("game data installed");
            g_config.isoPath = image.string();
            Config_Save();
            installed = true;
            break;
        }
        if (r == InstallResult::Canceled) break;
        LOG_ERROR("install failed: {}", error);
        std::string msg = "Could not install Sonic Colors game data:\n\n" + error +
                          "\n\nSupported release: " + kReleaseNames;
        if (r == InstallResult::WriteFailed)
        {
            msg += "\n\nCheck that the destination has enough free space and that you can write to it.";
            Platform_MessageBox(kTitle, msg, MsgButtons::Ok, MsgIcon::Error);
            break;
        }
        msg += "\n\nSelect a different disc image?";
        if (Platform_MessageBox(kTitle, msg, MsgButtons::OkCancel, MsgIcon::Error) != MsgResult::Ok) break;
        image.clear();
    }
    return installed;
}
