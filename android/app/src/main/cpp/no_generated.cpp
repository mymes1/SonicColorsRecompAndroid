// The generated PPC code (generated/, from tools/build_generated.py) is not part of the launcher APK:
// it is whole-disc-derived, so the APK you install is built from this repository alone and the game
// code is never shipped. The shared runtime still refers to four of its symbols, though:
//
//   g_ppcRelease, g_ppcDelta, g_ppcDeltaCount  the release table and its address offsets
//   g_ppcRegionCode                            the releases this build contains
//   ppc::fn_803E19B0                           the SDK helper runtime/perf.cpp calls
//
// Nothing here fakes game code. The region table is empty, so Region_Select fails with its ordinary
// "this build does not contain the code of the ... release" message (and the on-device installer's
// launcher asks for a disc, which is the point); running the guest raises the unimplemented symbol
// rather than continuing with wrong data. A build with SCR_COMPILE_GENERATED=ON provides the real
// definitions and does not use this file.

#include <cstdlib>

#include "ppc_context.h"

extern "C" {
uint32_t g_ppcRelease = 0;
uint32_t g_ppcDelta[1] = {};
const uint32_t g_ppcDeltaCount = 0;
}

// Terminated immediately, the way Runtime_Select expects a table it can walk.
extern const PPCRegionCode* const g_ppcRegionCode[] = { nullptr };

namespace ppc
{
// runtime/perf.cpp overrides FrameLoopWait and calls the SDK helper that publishes the VI wait
// interval and its delta. Without generated code there is nothing to call; every frame would.
PPC_GEN_FUNC(fn_803E19B0)
{
    std::abort();
}
}   // namespace ppc
