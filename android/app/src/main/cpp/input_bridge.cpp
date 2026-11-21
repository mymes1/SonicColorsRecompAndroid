#include <jni.h>
#include "input.h"
#include "platform/input.h"

extern "C" JNIEXPORT void JNICALL
Java_org_soniccolorsrecomp_android_AndroidInput_setConnected(JNIEnv*, jclass, jboolean value) {
    AndroidInput_SetConnected(value);
}
extern "C" JNIEXPORT void JNICALL
Java_org_soniccolorsrecomp_android_AndroidInput_setButton(JNIEnv*, jclass, jint button, jboolean down) {
    AndroidInput_SetButton(static_cast<uint16_t>(button), down);
}
extern "C" JNIEXPORT void JNICALL
Java_org_soniccolorsrecomp_android_AndroidInput_setSticks(JNIEnv*, jclass, jfloat lx, jfloat ly, jfloat rx, jfloat ry) {
    AndroidInput_SetSticks(lx, ly, rx, ry);
}
extern "C" JNIEXPORT void JNICALL
Java_org_soniccolorsrecomp_android_AndroidInput_setTriggers(JNIEnv*, jclass, jfloat lt, jfloat rt) {
    AndroidInput_SetTriggers(lt, rt);
}
extern "C" JNIEXPORT void JNICALL
Java_org_soniccolorsrecomp_android_AndroidInput_reset(JNIEnv*, jclass) { AndroidInput_Reset(); }
extern "C" JNIEXPORT jint JNICALL
Java_org_soniccolorsrecomp_android_AndroidInput_readButtonsForTest(JNIEnv*, jclass) {
    PadState out;
    return Pad_Read(0, out) ? out.buttons : -1;
}
