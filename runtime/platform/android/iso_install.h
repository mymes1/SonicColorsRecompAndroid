#pragma once
// Installing the game data from a disc image on the device itself.
//
// The desktop installer (runtime/install/install.cpp) reads images with nod, a Rust library
// that has no Android build. This is the Android equivalent: a plain ISO reader that
// decrypts the DATA partition with the player's own keys.txt, the layout Dolphin and
// DolphinTool accept:
//
//   # comment
//   common-key-0 = <32 hex digits>      (also: common0, rvl-key-retail)
//   common-key-1 = ...                  (Korean; only needed for a Korean disc)
//
// No key ships with the app: docs/dolphin-dependency.md records why (the Wii common key is
// why the desktop build links nod instead of decrypting itself), so the app asks for the
// player's own key file instead of embedding one. A decrypted ISO needs no key at all
// (Systems/ or `nodtool convert` output).
//
// Output is the same tree Install_Run writes, so everything downstream (Disc_Open, the
// integrity check, mods) is unchanged:
//   <dest>/sys/{boot.bin,bi2.bin,apploader.img,main.dol,fst.bin}
//   <dest>/files/...            the FST's files
//   <dest>/{ticket,tmd,cert,h3}.bin
//   <dest>/disc/{header,region,unencrypted}.bin
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

// Installer exit codes, mirrored by NativeRunner.installError() in Java.
enum class IsoInstallResult
{
    Ok,
    OpenFailed,      // not readable, not an ISO, or the key was missing/wrong
    WrongGame,       // a readable disc, but not a supported release
    WrongVersion,    // supported game, but main.dol is not the one this build was built from
    WriteFailed,     // out of space / permissions / rename
    Canceled,
};
int IsoInstallResultCode(IsoInstallResult r);   // the Java-facing 0..5

// Where the player's keys.txt is looked up: the given path first (may be empty), then
// <filesDir>/keys.txt and <filesDir>/../keys.txt. Returns an empty path when none exists.
std::filesystem::path Keys_Find(const std::string& preferred, const std::string& filesDir);
// Reads one common key (index 0 = retail, 1 = Korean) as 16 bytes; false when absent.
bool Keys_ReadCommonKey(const std::filesystem::path& keysFile, int index, uint8_t key[16]);

struct IsoInstallProgress
{
    std::atomic<uint64_t> done{ 0 };
    std::atomic<uint64_t> total{ 0 };
    std::atomic<bool> cancel{ false };
};

struct IsoInstallOptions
{
    std::filesystem::path image;
    std::filesystem::path dest;
    std::filesystem::path keysFile;    // empty: disc is assumed decrypted
    // Player-facing progress text and errors; called from the installer thread.
    std::function<void(int percent, const std::string& text)> progress;
    std::function<void(const std::string& text)> status;
};

// Runs an install. `error` explains a failure in player terms. Blocks; run it on a worker
// thread. Progress/status callbacks may be empty.
IsoInstallResult IsoInstall_Run(const IsoInstallOptions& options, IsoInstallProgress& progress,
                                std::string& error);

// Cheap check of an image without installing it: the game ID from the first sector, or an
// empty string. Sets `needsKey` when the disc's partition data is encrypted.
std::string IsoInstall_GameId(const std::filesystem::path& image, bool* needsKey);

#if defined(SCR_ISO_INSTALL_TEST)
// Test-only: replaces the main.dol SHA-1 the release manifest demands (tools/iso_install_test
// builds a synthetic disc, which cannot contain the real game's executable). Never defined in
// a shipping build.
void IsoInstall_SetExpectedDolHashForTest(const char* sha1Hex);
#endif
