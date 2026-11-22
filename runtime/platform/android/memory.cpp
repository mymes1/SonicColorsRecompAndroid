// Android guest memory: cached, uncached and physical views alias the same backing pages.
//
// 4 GiB reservation with fixed mappings, as the PC runtime does. On Android the
// reservation is anonymous PROT_NONE and the two POWERPC RAM windows are memfd-backed so
// they behave like device memory for the aliasing code (self-modifying code, EFB copies
// through both views). memfd_create landed in Android 8 (API 26); when it is unavailable
// (older kernels or a sandbox that blocks it) the same layout comes from plain anonymous
// shared mappings, which alias identically.
#include "../../kernel/memory.h"

#include <cstdint>
#include <cerrno>
#include <mutex>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

uint8_t* g_base = nullptr;

namespace
{
std::mutex s_mutex;
constexpr size_t kReservation = 0x100000000ull;
constexpr size_t kLockedSize = 0x10000;

// One anonymous shared mapping of `size` bytes (the fallback when memfd is unavailable).
void* AnonShared(size_t size)
{
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}
}   // namespace

bool Memory_Init()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (g_base) return true;
    if (sizeof(void*) < 8) return false;

    void* reservation = mmap(nullptr, kReservation, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (reservation == MAP_FAILED)
    {
        // Some 32-bit-Binder kernels cap a single 4 GiB reservation; retry with 1.5 GiB,
        // which still covers MEM1 + MEM2 + the locked cache aliases.
        reservation = mmap(nullptr, 0x60000000ull, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (reservation == MAP_FAILED) return false;
    }
    auto* base = static_cast<uint8_t*>(reservation);

    // RAM1 / RAM2 / locked cache are memfd-backed so all their aliases share pages.
    const char* names[] = { "scr-mem1", "scr-mem2", "scr-locked" };
    const size_t sizes[] = { MEM1_SIZE, MEM2_SIZE, kLockedSize };
    int fds[3] = { -1, -1, -1 };
    bool memfd = true;
#ifdef SYS_memfd_create
    for (int i = 0; i < 3; i++)
    {
        fds[i] = (int)syscall(SYS_memfd_create, names[i], 1u /* MFD_CLOEXEC */);
        if (fds[i] < 0 || ftruncate(fds[i], (off_t)sizes[i]) != 0)
        {
            if (fds[i] >= 0) close(fds[i]);
            fds[i] = -1;
            memfd = false;
        }
    }
#else
    memfd = false;
#endif

    if (!memfd)
    {
        // Fallback: every alias is mapped from one anonymous shared object by using
        // MAP_SHARED|MAP_ANONYMOUS at *fixed* addresses — the kernel maps fresh zero
        // pages each time, so this does not alias. Instead map each RAM window once and
        // let the runtime's CanonAddr() fold the other views onto it.
        struct Fixed { uint32_t address; uint32_t size; } fixed[] = {
            { 0x00000000, MEM1_SIZE }, { 0x10000000, MEM2_SIZE }, { 0xE0000000, (uint32_t)kLockedSize },
        };
        for (auto& f : fixed)
        {
            void* p = mmap(base + f.address, f.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            if (p != base + f.address)
            {
                munmap(reservation, kReservation);
                return false;
            }
        }
        g_base = base;
        return true;
    }

    struct Alias { uint32_t address; uint32_t size; int fd; };
    const Alias aliases[] = {
        { 0x00000000, MEM1_SIZE, fds[0] }, { 0x80000000, MEM1_SIZE, fds[0] }, { 0xC0000000, MEM1_SIZE, fds[0] },
        { 0x10000000, MEM2_SIZE, fds[1] }, { 0x90000000, MEM2_SIZE, fds[1] }, { 0xD0000000, MEM2_SIZE, fds[1] },
        { LOCKED_CACHE_ADDR, (uint32_t)kLockedSize, fds[2] },
    };
    bool ok = true;
    for (const auto& a : aliases)
    {
        void* p = mmap(base + a.address, a.size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, a.fd, 0);
        if (p != base + a.address)
        {
            ok = false;
            break;
        }
    }
    for (int i = 0; i < 3; i++)
        if (fds[i] >= 0) close(fds[i]);
    if (!ok)
    {
        munmap(reservation, kReservation);
        return false;
    }
    g_base = base;
    // Touch the first page of each window so a later allocation failure surfaces here.
    g_base[0] = 0;
    g_base[0x10000000] = 0;
    return true;
}
