// Android guest memory: cached, uncached and physical views alias the same
// backing pages. This is a 64-bit-only address-space layout like the PC runtime.
#include "../../kernel/memory.h"

#include <cstdint>
#include <mutex>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

uint8_t* g_base = nullptr;
namespace {
std::mutex memoryMutex;
constexpr size_t kReservation = 0x100000000ull;
constexpr unsigned kMemfdCloexec = 1;

int CreateMemory(const char* name, size_t size) {
#ifdef SYS_memfd_create
    int fd = static_cast<int>(syscall(SYS_memfd_create, name, kMemfdCloexec));
    if (fd >= 0 && ftruncate(fd, static_cast<off_t>(size)) != 0) {
        close(fd); return -1;
    }
    return fd;
#else
    (void)name; (void)size;
    return -1;
#endif
}
}

bool Memory_Init() {
    std::lock_guard<std::mutex> lock(memoryMutex);
    if (g_base) return true;
    if (sizeof(void*) < 8) return false;
    void* reservation = mmap(nullptr, kReservation, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (reservation == MAP_FAILED) return false;
    auto* base = static_cast<uint8_t*>(reservation);
    int mem1 = CreateMemory("scr-mem1", MEM1_SIZE);
    int mem2 = CreateMemory("scr-mem2", MEM2_SIZE);
    int locked = CreateMemory("scr-locked", 0x10000);
    bool ok = mem1 >= 0 && mem2 >= 0 && locked >= 0;
    struct Region { uint32_t address, size; int fd; };
    const Region regions[] = {
        {0x00000000, MEM1_SIZE, mem1}, {0x80000000, MEM1_SIZE, mem1},
        {0xC0000000, MEM1_SIZE, mem1}, {0x10000000, MEM2_SIZE, mem2},
        {0x90000000, MEM2_SIZE, mem2}, {0xD0000000, MEM2_SIZE, mem2},
        {LOCKED_CACHE_ADDR, 0x10000, locked},
    };
    if (ok) {
        for (const auto& region : regions) {
            if (mmap(base + region.address, region.size, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_FIXED, region.fd, 0) != base + region.address) {
                ok = false; break;
            }
        }
    }
    if (mem1 >= 0) close(mem1);
    if (mem2 >= 0) close(mem2);
    if (locked >= 0) close(locked);
    if (!ok) { munmap(reservation, kReservation); return false; }
    g_base = base;
    return true;
}
