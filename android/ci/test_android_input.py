"""Test the Android Pad backend with the host compiler (no Android SDK required)."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class InputTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which('g++'), 'host C++ compiler required')
    def test_pad_state(self):
        with tempfile.TemporaryDirectory() as temp:
            temp = pathlib.Path(temp)
            (temp / 'test.cpp').write_text('''
#include "runtime/platform/android/input.h"
#include "runtime/platform/input.h"
#include <cassert>
#include <limits>
int main() {
    PadState s;
    AndroidInput_Reset();
    assert(!Pad_Read(0, s));
    AndroidInput_SetConnected(true);
    AndroidInput_SetButton(0x1000, true);
    AndroidInput_SetButton(0x2000, true);
    AndroidInput_SetButton(0x1000, false);
    AndroidInput_SetSticks(-1.0f, 1.0f, 3.0f, std::numeric_limits<float>::quiet_NaN());
    AndroidInput_SetTriggers(0.5f, 2.0f);
    assert(Pad_Read(0, s) && !Pad_Read(1, s));
    assert(Pad_Read(0, s));
    assert(s.buttons == 0x2000 && s.lx == -32768 && s.ly == 32767);
    assert(s.rx == 32767 && s.ry == 0 && s.lt == 127 && s.rt == 255);
    // What AndroidInput.readButtonsForTest() reports to the instrumentation test.
    AndroidInput_Reset();
    assert(AndroidInput_Buttons() == -1);
    AndroidInput_SetConnected(true);
    AndroidInput_SetButton(0x1000, true);
    assert(AndroidInput_Buttons() == 0x1000);
    AndroidInput_SetConnected(false);
    AndroidInput_SetConnected(true);
    assert(AndroidInput_Buttons() == 0);
    AndroidInput_SetConnected(false);
    assert(!Pad_Read(0, s) && Pad_Count() == 0 && s.buttons == 0);
    AndroidInput_SetConnected(true);
    assert(Pad_Read(0, s) && s.buttons == 0 && s.lx == 0);
}
''')
            # android_app_stub.cpp is the host fallback for the JNI bridge: input.cpp calls
            # androidapp::Vibrate/CancelVibration, which only a device can implement.
            subprocess.run(['g++', '-std=c++20', '-pthread', '-I', str(ROOT),
                            str(ROOT / 'runtime/platform/android/input.cpp'),
                            str(ROOT / 'runtime/platform/android/android_app_stub.cpp'),
                            str(temp / 'test.cpp'),
                            '-o', str(temp / 'test')], check=True)
            subprocess.run([str(temp / 'test')], check=True)


if __name__ == '__main__': unittest.main()
