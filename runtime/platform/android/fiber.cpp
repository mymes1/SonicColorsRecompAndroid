// Guest thread fibers on AArch64 (Android, and any 64-bit ARM Linux CI host).
//
// A switch saves the callee-saved general registers plus the FP control state (FPCR and
// the callee-saved halves of the SIMD registers v8-v15), which is exactly what the AAPCS64
// requires across a call. Same design as runtime/platform/linux/fiber.cpp, whose x86-64
// version explains the reasoning.
#include "../fiber.h"
#include "../../log.h"

#include <sys/mman.h>
#include <unistd.h>

namespace
{
struct Fiber
{
    void* sp = nullptr;
    void* stack = nullptr;
    size_t mapped = 0;
    FiberProc fn = nullptr;
    void* arg = nullptr;
};

thread_local Fiber* t_current = nullptr;
}   // namespace

// bkr_fiber_switch(void** saveSp, void* newSp)
//   x0 = where to store the old stack pointer, x1 = the stack pointer to resume.
asm(R"(
    .text
    .globl bkr_fiber_switch
    .type bkr_fiber_switch, %function
bkr_fiber_switch:
    .cfi_startproc
    // 176-byte frame: x19-x30, d8-d15, fpcr
    sub sp, sp, #176
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    stp d8,  d9,  [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    mrs x2, fpcr
    str x2, [sp, #160]
    mov x2, sp
    str x2, [x0]
    mov sp, x1
    ldr x2, [sp, #160]
    msr fpcr, x2
    ldp d14, d15, [sp, #144]
    ldp d12, d13, [sp, #128]
    ldp d10, d11, [sp, #112]
    ldp d8,  d9,  [sp, #96]
    ldp x29, x30, [sp, #80]
    ldp x27, x28, [sp, #64]
    ldp x25, x26, [sp, #48]
    ldp x23, x24, [sp, #32]
    ldp x21, x22, [sp, #16]
    ldp x19, x20, [sp, #0]
    add sp, sp, #176
    ret
    .cfi_endproc
    .size bkr_fiber_switch, .-bkr_fiber_switch

    .globl bkr_fiber_start
    .type bkr_fiber_start, %function
bkr_fiber_start:
    // Reached by the first bkr_fiber_switch into a new fiber: x19 = fn, x20 = arg.
    mov x0, x20
    blr x19
    brk #0
    .size bkr_fiber_start, .-bkr_fiber_start
)");

extern "C" void bkr_fiber_switch(void** saveSp, void* newSp);
extern "C" void bkr_fiber_start();

extern "C" void bkr_fiber_entry(Fiber* f)
{
    f->fn(f->arg);
    FATAL("a guest thread fiber returned");
}

namespace
{
constexpr size_t kAlign = 16;
}

void* Fiber_FromThread()
{
    if (t_current) return t_current;
    auto* f = new Fiber();
    t_current = f;
    return f;
}

void* Fiber_Create(size_t stackSize, FiberProc fn, void* arg)
{
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    const size_t committed = (stackSize + page - 1) / page * page;
    const size_t mapped = committed + page;
    void* mem = mmap(nullptr, mapped, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
    if (mem == MAP_FAILED)
    {
        LOG_ERROR("fiber: cannot allocate a {} KiB stack", stackSize / 1024);
        return nullptr;
    }
    mprotect(mem, page, PROT_NONE);   // overflow trap at the low end
    auto* f = new Fiber();
    f->stack = mem;
    f->mapped = mapped;
    f->fn = fn;
    f->arg = arg;
    uint8_t* top = (uint8_t*)mem + mapped;
    // Initial frame as bkr_fiber_switch expects to load it: 176 bytes, x19 = entry,
    // x20 = argument, x30 = bkr_fiber_start (the switch's `ret` target).
    uint64_t* sp = (uint64_t*)((uintptr_t)(top - 176) & ~(uintptr_t)(kAlign - 1));
    for (int i = 0; i < 22; i++) sp[i] = 0;
    sp[2] = (uint64_t)(uintptr_t)&bkr_fiber_entry;   // x19
    sp[3] = (uint64_t)(uintptr_t)f;                  // x20
    sp[11] = (uint64_t)(uintptr_t)&bkr_fiber_start;   // x30
    f->sp = sp;
    return f;
}

void Fiber_Switch(void* to)
{
    auto* target = (Fiber*)(to ? to : Fiber_FromThread());
    Fiber* from = t_current;
    if (!from)
    {
        // Switching from a plain thread the first time: adopt it as a fiber.
        from = (Fiber*)Fiber_FromThread();
    }
    if (from == target) return;
    t_current = target;
    bkr_fiber_switch(&from->sp, target->sp);
    t_current = from;
}

void Fiber_Delete(void* fiber)
{
    auto* f = (Fiber*)fiber;
    if (!f) return;
    if (f->stack) munmap(f->stack, f->mapped);
    delete f;
}
