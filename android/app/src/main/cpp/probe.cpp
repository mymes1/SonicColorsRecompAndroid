#include <jni.h>
#include <cstdint>
#include "ppc_context.h"

// Exercise platform-independent PPC helpers from the desktop recompiler. This
// does not initialize the game, emulate devices, or establish compatibility.
extern "C" JNIEXPORT jboolean JNICALL
Java_org_soniccolorsrecomp_android_NativeProbe_checkCpuPrimitives(JNIEnv*, jclass) {
    alignas(uint32_t) uint8_t bytes[4] = {};
    st32n(bytes, 0, 0x12345678u);
    if (bytes[0] != 0x12 || bytes[1] != 0x34 || bytes[2] != 0x56 || bytes[3] != 0x78)
        return JNI_FALSE;
    if (ld32n(bytes, 0) != 0x12345678u || ppc_rotl32(0x12345678u, 8) != 0x34567812u)
        return JNI_FALSE;
    PPCContext ctx{};
    ppc_set_cr(ctx, 0x12345678u);
    return ppc_get_cr(ctx) == 0x12345678u ? JNI_TRUE : JNI_FALSE;
}
