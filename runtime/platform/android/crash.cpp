// Crash reports on Android: a fault handler that logs the guest function and the host frames, and
// the watchdog's guest-thread sampling.
//
// This is runtime/platform/linux/crash.cpp adapted to Bionic: `ucontext_t::uc_mcontext` has no
// `gregs` array, so the interrupted PC comes from `pc` (AArch64) or `rip` (x86-64), and
// execinfo.h/dladdr come from libc/libdl on the NDK, so symbolisation is the same. A crash on a
// phone has no console to read, which is why everything also goes through the log file the
// Activity points the player at.
#include "../../kernel/crash.h"
#include "../../kernel/memory.h"
#include "../../kernel/threads.h"
#include "../../env.h"
#include "../../log.h"
#include "../platform.h"

#include <android/log.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <ucontext.h>
#include <unistd.h>

extern bool g_noMessageBox;
void Gamepad_StopRumble();

namespace
{
std::atomic<bool> s_guestKnown{ false };
pthread_t s_guestThread;
std::atomic<uintptr_t> s_sampledPC{ 0 };
std::atomic<bool> s_sampled{ false };
const int kSampleSignal = SIGRTMIN + 3;

uintptr_t ContextPC(void* uc)
{
    ucontext_t* c = (ucontext_t*)uc;
#if defined(__aarch64__)
    return (uintptr_t)c->uc_mcontext.pc;
#elif defined(__x86_64__)
    return (uintptr_t)c->uc_mcontext.rip;
#else
    (void)c;
    return 0;
#endif
}

const char* SignalName(int sig)
{
    switch (sig)
    {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    default: return "signal";
    }
}

// Each thread that can fault with a full stack needs its own signal stack; a guest thread's stack
// overflow is exactly the case this handler has to survive.
void AddSignalStack()
{
    stack_t ss{};
    ss.ss_size = 1 << 20;
    ss.ss_sp = malloc(ss.ss_size);
    if (ss.ss_sp) sigaltstack(&ss, nullptr);
}

void CrashHandler(int sig, siginfo_t* si, void* uc)
{
    static std::atomic<bool> inHandler{ false };
    if (inHandler.exchange(true)) _exit(2);

    const uintptr_t pc = ContextPC(uc);
    const uintptr_t addr = (sig == SIGSEGV || sig == SIGBUS) ? (uintptr_t)si->si_addr : 0;
    LOG_ERROR("=== host exception {} (code {}) at pc={:x} (fault addr {:x}) ===", SignalName(sig), si->si_code, pc, addr);

    // A fault inside the 4 GiB guest window is almost always a bad guest pointer, so name it in
    // guest terms: that is the number that means something when reading a report.
    if (g_base && addr >= (uintptr_t)g_base && addr < (uintptr_t)g_base + 0x100000000ull)
        LOG_ERROR("  guest address {:08x}", (uint32_t)(addr - (uintptr_t)g_base));

    const uint32_t guestFn = Crash_GuestFunctionForHost(pc);
    LOG_ERROR("  in guest function ~fn_{:08X} (host offset +{:x})", guestFn,
              guestFn ? pc - Crash_FirstHostFunction() : 0);

    void* frames[62];
    const int n = backtrace(frames, 62);
    for (int i = 0; i < n; i++)
    {
        Dl_info di{};
        const bool ok = dladdr(frames[i], &di) != 0;
        const char* mod = ok && di.dli_fname ? di.dli_fname : "?";
        if (const char* slash = strrchr(mod, '/')) mod = slash + 1;
        const uintptr_t symOff = ok && di.dli_saddr ? (uintptr_t)frames[i] - (uintptr_t)di.dli_saddr : 0;
        const uintptr_t modOff = ok && di.dli_fbase ? (uintptr_t)frames[i] - (uintptr_t)di.dli_fbase : 0;
        LOG_ERROR("  host frame {:2}: {} {}+{:x} ({}+{:x})", i, frames[i], ok && di.dli_sname ? di.dli_sname : "?",
                  symOff, mod, modOff);
    }
    if (g_curThread) Crash_DumpContext(g_curThread->ctx);

    Log_MarkAbnormalExit("Host crash - see the log file (becomes .1.log on next launch).");
    Gamepad_StopRumble();
    // logcat is what a bug report captures, so repeat the headline there even if the game's own
    // log went to app storage the player cannot easily reach.
    __android_log_print(ANDROID_LOG_ERROR, "SonicColorsRecomp", "host crash: %s at pc=%p, guest fn_%08X",
                        SignalName(sig), (void*)pc, guestFn);
    if (!g_noMessageBox)
        Platform_ErrorBox("Sonic Colors - Recompiled",
                          "The game crashed. Details are in the log file the launcher names.");
    _exit(2);
}

void SampleHandler(int, siginfo_t*, void* uc)
{
    s_sampledPC.store(ContextPC(uc), std::memory_order_relaxed);
    s_sampled.store(true, std::memory_order_release);
}
}   // namespace

void Crash_Init()
{
    Crash_BuildMap();
    AddSignalStack();
    struct sigaction sa{};
    sa.sa_sigaction = CrashHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int sig : { SIGSEGV, SIGBUS, SIGILL, SIGFPE }) sigaction(sig, &sa, nullptr);

    struct sigaction sampling{};
    sampling.sa_sigaction = SampleHandler;
    sampling.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sampling.sa_mask);
    sigaction(kSampleSignal, &sampling, nullptr);
}

void Crash_StartSamplers()
{
    if (EnvHas("SCR_SLOW_SAMPLE", "BKR_SLOW_SAMPLE") || EnvHas("SCR_IRQ_STALLS", "BKR_IRQ_STALLS"))
        LOG_WARN("SCR_SLOW_SAMPLE / SCR_IRQ_STALLS are not available on Android");
    s_guestThread = pthread_self();
    s_guestKnown = true;
}

bool Crash_GuestThreadKnown() { return s_guestKnown; }

uintptr_t Crash_SampleGuestPC()
{
    s_sampled.store(false, std::memory_order_relaxed);
    if (pthread_kill(s_guestThread, kSampleSignal) != 0) return 0;
    for (int i = 0; i < 1000 && !s_sampled.load(std::memory_order_acquire); i++) usleep(50);
    return s_sampled.load(std::memory_order_acquire) ? s_sampledPC.load(std::memory_order_relaxed) : 0;
}

void Crash_SetGxThread() { AddSignalStack(); }

void Crash_SetWatch(uint32_t) { LOG_WARN("--watch is not available on Android"); }

void Crash_StartProfiler(int, int) { LOG_WARN("--profile is not available on Android"); }

void Crash_SampleSlowGuest() {}

bool g_profileGX = false;   // --profile-gx is a Windows-only profiler
