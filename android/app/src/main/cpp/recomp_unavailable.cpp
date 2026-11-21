// Explicit fail-fast hooks for a *non-playable* code-bearing APK. Never silently
// emulate hardware or return fake results; no guest code should be invoked yet.
#include "ppc_context.h"
#include <cstdlib>

extern "C" {
volatile uint32_t g_ppcIrqPending = 0;
uint32_t g_tasProbe[16] = {};
uint32_t g_cfgFrameRate = 30;
PPCFunc ppc_lookup(uint32_t) { std::abort(); }
void ppc_call_indirect(PPCContext&, uint8_t*, uint32_t) { std::abort(); }
void ppc_check_irq(PPCContext&, uint8_t*) { std::abort(); }
uint32_t mmio_read8(uint32_t) { std::abort(); }
uint32_t mmio_read16(uint32_t) { std::abort(); }
uint32_t mmio_read32(uint32_t) { std::abort(); }
void mmio_write8(uint32_t, uint32_t) { std::abort(); }
void mmio_write16(uint32_t, uint32_t) { std::abort(); }
void mmio_write32(uint32_t, uint32_t) { std::abort(); }
uint32_t ppc_mfspr(PPCContext&, uint32_t) { std::abort(); }
void ppc_mtspr(PPCContext&, uint8_t*, uint32_t, uint32_t) { std::abort(); }
uint32_t ppc_mftb(PPCContext&, uint32_t) { std::abort(); }
void ppc_mtmsr(PPCContext&, uint8_t*, uint32_t) { std::abort(); }
void ppc_sc(PPCContext&, uint8_t*, uint32_t) { std::abort(); }
void ppc_rfi(PPCContext&, uint8_t*) { std::abort(); }
void ppc_trap(PPCContext&, uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t) { std::abort(); }
void ppc_illegal(PPCContext&, uint8_t*, uint32_t, uint32_t) { std::abort(); }
void ppc_lswi(PPCContext&, uint8_t*, uint32_t, uint32_t, uint32_t) { std::abort(); }
void ppc_stswi(PPCContext&, uint8_t*, uint32_t, uint32_t, uint32_t) { std::abort(); }
void ppc_lswx(PPCContext&, uint8_t*, uint32_t, uint32_t) { std::abort(); }
void ppc_stswx(PPCContext&, uint8_t*, uint32_t, uint32_t) { std::abort(); }
double ppc_fres(double) { std::abort(); }
double ppc_frsqrte(double) { std::abort(); }
uint32_t Perf_WaitFields() { std::abort(); }
uint32_t Perf_DtFields() { std::abort(); }
}
