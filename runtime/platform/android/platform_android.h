#pragma once
// Android platform surface shared by the runtime glue and the JNI entry points.
//
// Two build contexts compile against this:
//  * on device (NDK/Bionic) with SCR_ANDROID_APP defined, where android_app.cpp talks to
//    the Activity/service through JNI, and
//  * on a Linux CI host (no Android headers), where android_app_stub.cpp returns neutral
//    values so the whole runtime still compiles and the unit tests can run.
#include <cstdint>
#include <string>
#include <vector>

// Command line assembled by the Activity (see runtime/platform/android/main.cpp).
extern std::vector<std::string> g_androidArgs;
// Game data directory (extracted DATA inside the app's files dir) and disc image.
extern std::string g_androidDataDir;
extern std::string g_androidIsoPath;

struct ANativeActivity;
struct ANativeWindow;

namespace androidapp
{
// Thread naming without pulling <android/log.h> into shared code.
void SetThreadName(const char* name);
// Native-activity glue (valid only when the APK uses the native activity; the Java
// activity path leaves these null and sets the window through JNI instead).
void SetActivity(struct ANativeActivity* activity);
void SetNativeWindow(struct ANativeWindow* window);
// Directories owned by the app.
std::string InternalFilesDir();
std::string CacheDir();
std::string ExternalFilesDir();     // null/empty string when unavailable
std::string NativeLibDir();
// Storage access framework handle ("/proc/self/fd/N" alternative is /sdcard paths).
bool OpenDocumentFd(const std::string& uri, int* fd, uint64_t* size);
std::string DocumentDisplayName(const std::string& uri);
// Device identity for logs and Mali tuning.
std::string DeviceModel();
std::string AndroidRelease();
int SdkInt();
std::string Abi();
// Haptics (Wii Remote rumble) and the surface the renderer presents to.
void Vibrate(int ms, int amplitude);
void CancelVibration();
uint64_t NativeWindowHandle();
// Lets the runtime publish status text (install progress, errors) to the Activity.
void SetStatus(const std::string& text);
void SetProgress(float fraction, const std::string& label);
// Register the C function the Activity calls with touch/gamepad state (0 = off).
void SetInputCallback(void (*cb)(int, int, float, float, int));
// Wake locks are held while a game session runs.
void SetScreenAwake(bool on);
}
