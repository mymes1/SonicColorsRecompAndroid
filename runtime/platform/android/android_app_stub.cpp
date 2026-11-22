// Host (CI) fallback for the Android Activity bridge: neutral values, so the complete
// runtime links and runs on a Linux host for unit tests and the shader corpus checks.
#include "platform_android.h"

namespace androidapp
{
void SetThreadName(const char*) {}
std::string InternalFilesDir() { return "/tmp/scr_android/files"; }
std::string CacheDir() { return "/tmp/scr_android/cache"; }
std::string ExternalFilesDir() { return {}; }
std::string NativeLibDir() { return "/tmp/scr_android/lib"; }
bool OpenDocumentFd(const std::string&, int*, uint64_t*) { return false; }
std::string DocumentDisplayName(const std::string& uri) { return uri; }
std::string DeviceModel() { return "host"; }
std::string AndroidRelease() { return "host"; }
int SdkInt() { return 0; }
std::string Abi() { return "host"; }
void Vibrate(int, int) {}
void CancelVibration() {}
uint64_t NativeWindowHandle() { return 0; }
void SetStatus(const std::string& text) { (void)text; }
void SetProgress(float, const std::string&) {}
void SetInputCallback(void (*)(int, int, float, float, int)) {}
void SetScreenAwake(bool) {}
}
