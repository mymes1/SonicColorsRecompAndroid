// Metadata-only bridge. The generated functions must not be executed until the
// Android runtime implements their device, kernel, scheduling and graphics hooks.
#include <jni.h>
#include "ppc_context.h"

extern const PPCRegionCode* const g_ppcRegionCode[];

extern "C" JNIEXPORT jstring JNICALL
Java_org_soniccolorsrecomp_android_RecompCode_gameId(JNIEnv* env, jclass) {
    const PPCRegionCode* code = g_ppcRegionCode[0];
    return env->NewStringUTF(code && code->gameId ? code->gameId : "");
}
