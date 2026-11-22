// The two Activity-bridge calls the probe library needs, for the device only.
//
// probe.cpp and input.cpp arrive with the rest of the platform layer: input.cpp asks for a haptic
// pulse when a rumble-enabled pad requests one, and that is the only thing the probe uses from
// androidapp::. Linking the whole of android_app.cpp instead would drag the runtime's start/stop
// machinery into a library whose entire job is to start when the runtime is broken, and using
// android_app_stub.cpp would put a host fallback inside the APK.
//
// A missing Android Context simply means no vibration; nothing else depends on it.
#include "platform_android.h"

namespace androidapp
{
void Vibrate(int ms, int amplitude)
{
    (void)ms;
    (void)amplitude;
}

void CancelVibration() {}
}   // namespace androidapp
