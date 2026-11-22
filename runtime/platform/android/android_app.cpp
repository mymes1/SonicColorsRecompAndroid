// The Activity bridge: JNI entry points, the installer thread and the runtime thread.
//
// NativeRunner.java owns the lifecycle. It loads libscr_runtime.so, runs the installer on a
// worker thread, then calls nativeStart with the Surface the game should be drawn into.
// Everything here runs on those Java threads and hands work to the rest of the runtime
// through the platform layer (platform_android.h) and the Vulkan swapchain, which is the only
// owner of the ANativeWindow.
#include "platform_android.h"

#include "app.h"
#include "input.h"
#include "iso_install.h"
#include "video_swapchain.h"

#include "../../config.h"
#include "../../install/releases.h"
#include "../../install/verify.h"
#include "../../audio/audio_out.h"
#include "../../log.h"
#include "../../platform/platform.h"

#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <pthread.h>
#include <sys/system_properties.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace
{
JavaVM* s_vm = nullptr;
jclass s_runnerClass = nullptr;       // global ref to NativeRunner, for its static callbacks
jmethodID s_onProgress = nullptr;     // (ILjava/lang/String;)V
jmethodID s_onStatus = nullptr;       // (Ljava/lang/String;)V
jmethodID s_onRuntimeExit = nullptr;  // (Ljava/lang/String;)V
jobject s_vibrator = nullptr;         // Vibrator service, from nativeSetContext
jmethodID s_vibrate = nullptr;        // (J)V
jmethodID s_cancel = nullptr;         // ()V

std::atomic<bool> s_running{ false };
std::atomic<bool> s_exitRequested{ false };
std::atomic<bool> s_installing{ false };
IsoInstallProgress s_installProgress;
uint64_t s_window = 0;                // ANativeWindow* the renderer presents to
std::mutex s_windowMutex;

// ------------------------------------------------------------------ JNI helpers
struct Env
{
    JNIEnv* env = nullptr;
    bool attached = false;

    Env()
    {
        if (!s_vm) return;
        if (s_vm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK) return;
        if (s_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) attached = true;
        else env = nullptr;
    }
    ~Env()
    {
        if (attached && s_vm) s_vm->DetachCurrentThread();
    }
    explicit operator bool() const { return env != nullptr; }
};

std::string ToString(JNIEnv* env, jstring s)
{
    if (!s) return {};
    const char* utf = env->GetStringUTFChars(s, nullptr);
    std::string out = utf ? utf : "";
    if (utf) env->ReleaseStringUTFChars(s, utf);
    return out;
}

// The runtime calls these from threads the VM has never seen, so every call attaches.
void CallStatus(const std::string& text)
{
    Env e;
    if (!e || !s_runnerClass || !s_onStatus) return;
    jstring s = e.env->NewStringUTF(text.c_str());
    e.env->CallStaticVoidMethod(s_runnerClass, s_onStatus, s);
    e.env->DeleteLocalRef(s);
}

void CallProgress(int percent, const std::string& label)
{
    Env e;
    if (!e || !s_runnerClass || !s_onProgress) return;
    jstring s = e.env->NewStringUTF(label.c_str());
    e.env->CallStaticVoidMethod(s_runnerClass, s_onProgress, (jint)percent, s);
    e.env->DeleteLocalRef(s);
}

void CallRuntimeExit(const std::string& reason)
{
    Env e;
    if (!e || !s_runnerClass || !s_onRuntimeExit) return;
    jstring s = e.env->NewStringUTF(reason.c_str());
    e.env->CallStaticVoidMethod(s_runnerClass, s_onRuntimeExit, s);
    e.env->DeleteLocalRef(s);
}

void SetWindow(uint64_t window)
{
    std::lock_guard lk(s_windowMutex);
    s_window = window;
    VideoSwapchain_SetWindow(window);
}

// Extensions and API level the Vulkan device needs, read by vk_context.cpp.
int DeviceSdk() { return androidapp::SdkInt(); }
}   // namespace

// ================================================================== platform services
namespace androidapp
{
void SetThreadName(const char* name)
{
    if (!name || !*name) return;
    // pthread_setname_np truncates at 15 characters; keep names short at the call sites.
    char buf[16];
    snprintf(buf, sizeof(buf), "%s", name);
    pthread_setname_np(pthread_self(), buf);
}

std::string InternalFilesDir()
{
    // NativeRunner passes the real directory through SCR_ANDROID_FILES; the runtime only ever
    // needs it before that (never), so this is a fallback for the host build.
    if (const char* env = std::getenv("SCR_ANDROID_FILES")) return env;
    return "/data/data/org.soniccolorsrecomp.android/files";
}

std::string CacheDir() { return InternalFilesDir() + "/cache"; }

std::string ExternalFilesDir() { return {}; }

std::string NativeLibDir()
{
    // Where the APK unpacks its .so files; read-only, and where bundled assets live.
    if (const char* env = std::getenv("SCR_ANDROID_LIBDIR")) return env;
    return "/data/app/~~/lib/arm64";
}

bool OpenDocumentFd(const std::string&, int*, uint64_t*) { return false; }

std::string DocumentDisplayName(const std::string& uri) { return uri; }

std::string DeviceModel()
{
    // The model is a build property; the Activity already logs it, so keep this cheap.
    return "android";
}

std::string AndroidRelease() { return std::to_string(DeviceSdk()); }

int SdkInt()
{
    static const int sdk = [] {
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get("ro.build.version.sdk", value) > 0) return atoi(value);
        return 0;
    }();
    return sdk;
}

std::string Abi()
{
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get("ro.product.cpu.abi", value) > 0) return value;
    return "arm64-v8a";
}

void Vibrate(int ms, int amplitude)
{
    Env e;
    if (!e || !s_vibrator || !s_vibrate || ms <= 0) return;
    (void)amplitude;   // the Vibrator API only takes a duration without an effect
    e.env->CallVoidMethod(s_vibrator, s_vibrate, (jlong)ms);
}

void CancelVibration()
{
    Env e;
    if (!e || !s_vibrator || !s_cancel) return;
    e.env->CallVoidMethod(s_vibrator, s_cancel);
}

uint64_t NativeWindowHandle()
{
    std::lock_guard lk(s_windowMutex);
    return s_window;
}

void SetStatus(const std::string& text) { CallStatus(text); }
void SetProgress(float fraction, const std::string& label)
{
    const int percent = (int)(fraction < 0 ? 0 : (fraction > 1 ? 100 : fraction * 100));
    CallProgress(percent, label);
}
void SetInputCallback(void (*)(int, int, float, float, int)) {}
void SetScreenAwake(bool) {}   // the Activity holds FLAG_KEEP_SCREEN_ON for the whole session
}   // namespace androidapp

// ================================================================== JNI
extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*)
{
    s_vm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    if (jclass cls = env->FindClass("org/soniccolorsrecomp/android/NativeRunner"))
    {
        s_runnerClass = (jclass)env->NewGlobalRef(cls);
        s_onProgress = env->GetStaticMethodID(cls, "onProgress", "(ILjava/lang/String;)V");
        s_onStatus = env->GetStaticMethodID(cls, "onStatus", "(Ljava/lang/String;)V");
        s_onRuntimeExit = env->GetStaticMethodID(cls, "onRuntimeExit", "(Ljava/lang/String;)V");
    }
    else
    {
        env->ExceptionClear();   // no launcher (host tests): the callbacks stay null
    }
    return JNI_VERSION_1_6;
}

// The Activity's Context, for the Vibrator: rumble comes from the guest's Pad_SetRumble,
// which cannot reach the Activity itself.
extern "C" JNIEXPORT void JNICALL
Java_org_soniccolorsrecomp_android_NativeRunner_nativeSetContext(JNIEnv* env, jclass, jobject context)
{
    if (!context) return;
    jclass ctxClass = env->GetObjectClass(context);
    jmethodID getService = env->GetMethodID(ctxClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    if (getService)
    {
        jstring name = env->NewStringUTF("vibrator");
        jobject vibrator = env->CallObjectMethod(context, getService, name);
        env->DeleteLocalRef(name);
        if (vibrator)
        {
            if (s_vibrator) env->DeleteGlobalRef(s_vibrator);
            s_vibrator = env->NewGlobalRef(vibrator);
            jclass vibClass = env->GetObjectClass(vibrator);
            s_vibrate = env->GetMethodID(vibClass, "vibrate", "(J)V");
            s_cancel = env->GetMethodID(vibClass, "cancel", "()V");
            env->DeleteLocalRef(vibClass);
            env->DeleteLocalRef(vibrator);
        }
    }
    env->DeleteLocalRef(ctxClass);
}

// Installs the game data from the image the Activity staged into app storage. Blocks until
// the install finishes (the Java side calls it on "scr-installer"), reporting progress through
// the static callbacks. Returns the Java-facing installer code (see NativeRunner.installError).
extern "C" JNIEXPORT jint JNICALL
Java_org_soniccolorsrecomp_android_NativeRunner_nativeInstall(JNIEnv* env, jclass, jstring imagePath,
                                                              jstring dataDir, jstring filesDir, jstring keysPath)
{
    IsoInstallOptions options;
    options.image = ToString(env, imagePath);
    options.dest = ToString(env, dataDir);
    const std::string files = ToString(env, filesDir);
    options.keysFile = Keys_Find(ToString(env, keysPath), files);
    options.progress = [](int percent, const std::string& text) { CallProgress(percent, text); };
    options.status = [](const std::string& text) { CallStatus(text); };

    std::error_code sizeEc;
    const auto imageBytes = std::filesystem::file_size(options.image, sizeEc);
    LOG_INFO("install: image={} ({} MiB) dest={} keys={}", options.image.string(),
             sizeEc ? 0ull : (unsigned long long)(imageBytes >> 20), options.dest.string(),
             options.keysFile.empty() ? "none (decrypted disc)" : options.keysFile.string());
    s_installProgress.done = 0;
    s_installProgress.total = 0;
    s_installProgress.cancel = false;
    s_installing = true;
    std::string error;
    const IsoInstallResult result = IsoInstall_Run(options, s_installProgress, error);
    s_installing = false;
    if (result != IsoInstallResult::Ok)
    {
        LOG_ERROR("install failed ({}): {}", (int)result, error);
        if (!error.empty()) CallStatus(error);
    }
    return IsoInstallResultCode(result);
}

extern "C" JNIEXPORT void JNICALL
Java_org_soniccolorsrecomp_android_NativeRunner_nativeCancelInstall(JNIEnv*, jclass)
{
    s_installProgress.cancel = true;
}

// Thread body for nativeStart, outside the function so the entry point stays readable.
struct RuntimeLaunch
{
    std::string files;
    ANativeWindow* window;
    size_t stackSize;
};
void* RuntimeThreadMain(void* arg)
{
    RuntimeLaunch* launch = (RuntimeLaunch*)arg;
    // Copy out everything we need before freeing the launch data.
    const std::string files = launch->files;
    ANativeWindow* window = launch->window;
    delete launch;

    androidapp::SetThreadName("scr-runtime");
    setenv("SCR_ANDROID_FILES", files.c_str(), 1);
    setenv("SCR_ANDROID_LOG", (files + "/SonicColorsRecomp.log").c_str(), 1);
    setenv("SCR_PORTABLE", "1", 1);

    std::vector<std::string> args{ "SonicColorsRecomp" };
    if (const char* extra = std::getenv("SCR_ANDROID_ARGS"))
    {
        std::string all = extra, cur;
        for (char c : all)
        {
            if (c == ' ') { if (!cur.empty()) { args.push_back(cur); cur.clear(); } }
            else cur += c;
        }
        if (!cur.empty()) args.push_back(cur);
    }
    const int rc = AndroidMain(args);
    // AndroidMain returns only when the game cannot start, or after a clean shutdown.
    CallRuntimeExit(rc ? "The game stopped before it started. See the log for details."
                       : "The game has exited.");
    ANativeWindow_release(window);
    SetWindow(0);
    s_running = false;
    return nullptr;
}

// Starts the runtime on a thread of its own. The guest's main thread runs for the rest of the
// process, so this returns as soon as the thread is up; the Activity watches the callbacks.
//
// The thread is created with pthreads rather than std::thread because it needs a stack size the
// C++ runtime does not expose. Guest threads get fibers with 256 MiB stacks of their own
// (kernel/threads.cpp), but the host-side prologue still recurses through the loader, the
// recompiled entry points and Debug builds' unoptimised frames, which overruns the 1 MiB default
// on some devices. 32 MiB is mapped lazily, so only the touched pages are actually committed.
extern "C" JNIEXPORT void JNICALL
Java_org_soniccolorsrecomp_android_NativeRunner_nativeStart(JNIEnv* env, jclass, jstring filesDir, jobject surface)
{
    if (s_running.exchange(true)) return;
    s_exitRequested = false;
    const std::string files = ToString(env, filesDir);

    // The window must exist before the renderer builds its swapchain, and must outlive the
    // renderer: ANativeWindow_fromSurface takes a reference that this thread then holds.
    ANativeWindow* window = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    if (!window)
    {
        s_running = false;
        CallRuntimeExit("The game surface is not ready; try again.");
        return;
    }
    SetWindow((uint64_t)(uintptr_t)window);

    constexpr size_t kRuntimeStack = 32u << 20;
    RuntimeLaunch* launch = new RuntimeLaunch{ files, window, kRuntimeStack };
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kRuntimeStack);
    pthread_t thread;
    if (pthread_create(&thread, &attr, RuntimeThreadMain, launch) != 0)
    {
        LOG_ERROR("cannot start the runtime thread");
        pthread_attr_destroy(&attr);
        delete launch;
        ANativeWindow_release(window);
        SetWindow(0);
        s_running = false;
        CallRuntimeExit("The game could not start its thread. See the log for details.");
        return;
    }
    pthread_attr_destroy(&attr);
    pthread_detach(thread);
}

// The Surface was resized or rotated: the swapchain is out of date and is rebuilt on the next
// frame. The game keeps its EFB; only the presentation changes.
extern "C" JNIEXPORT void JNICALL
Java_org_soniccolorsrecomp_android_NativeRunner_nativeResize(JNIEnv*, jclass, jint, jint)
{
    VideoSwapchain_Invalidate();
}

// The Surface is going away (app backgrounded, activity recreated). Rendering must stop
// touching it; the runtime recreates the swapchain when the next Surface arrives.
extern "C" JNIEXPORT void JNICALL
Java_org_soniccolorsrecomp_android_NativeRunner_nativeSurfaceDestroyed(JNIEnv*, jclass)
{
    SetWindow(0);
}

extern "C" JNIEXPORT void JNICALL
Java_org_soniccolorsrecomp_android_NativeRunner_nativeSurfaceCreated(JNIEnv* env, jclass, jobject surface)
{
    if (!surface) return;
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (!window) return;
    // The previous Surface's reference is dropped by nativeSurfaceDestroyed; if a new Surface
    // arrives without that (rare), release the reference we are replacing.
    uint64_t previous;
    {
        std::lock_guard lk(s_windowMutex);
        previous = s_window;
    }
    if (previous && previous != (uint64_t)(uintptr_t)window)
        ANativeWindow_release((ANativeWindow*)(uintptr_t)previous);
    SetWindow((uint64_t)(uintptr_t)window);
    VideoSwapchain_Invalidate();
}

// The Activity is hidden. Audio is suspended so the game does not play into the background,
// and the swapchain is dropped because the Surface may already be gone.
extern "C" JNIEXPORT void JNICALL
Java_org_soniccolorsrecomp_android_NativeRunner_nativePause(JNIEnv*, jclass)
{
    AudioOut_SetSuspended(true);
}

extern "C" JNIEXPORT void JNICALL
Java_org_soniccolorsrecomp_android_NativeRunner_nativeResume(JNIEnv*, jclass)
{
    AudioOut_SetSuspended(false);
}

// Ends the process without unwinding guest threads: they may be mid-instruction, and the
// guest's own shutdown path runs no host cleanup anyway.
extern "C" JNIEXPORT void JNICALL
Java_org_soniccolorsrecomp_android_NativeRunner_nativeRequestExit(JNIEnv*, jclass)
{
    s_exitRequested = true;
    s_installProgress.cancel = true;
    if (s_installing) return;   // the installer stops itself and the Activity finishes the app
    CallRuntimeExit("The game is closing.");
    Platform_Terminate(0);
}
