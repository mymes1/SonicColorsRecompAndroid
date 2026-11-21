"""Host-side link smoke test using synthetic, non-game generated code."""
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class RecompBridgeTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which('g++'), 'host C++ compiler required')
    def test_generated_code_links_with_fail_fast_hooks(self):
        with tempfile.TemporaryDirectory() as d:
            d = pathlib.Path(d)
            (d / 'jni.h').write_text('''#define JNIEXPORT
#define JNICALL
using jclass = void*; using jstring = const char*;
struct JNIEnv { jstring NewStringUTF(const char* s) { return s; } };
''')
            (d / 'ppc_recomp.h').write_text('PPC_EXTERN_FUNC(ovr_fake);\n')
            subprocess.run([sys.executable, str(ROOT / 'android/ci/gen_override_stubs.py'),
                            str(d / 'ppc_recomp.h'), str(d / 'stubs.cpp')], check=True)
            (d / 'fake.cpp').write_text('''#include "ppc_context.h"
PPC_EXTERN_FUNC(ovr_fake);
namespace { void f(PPCContext& c, uint8_t* b) { ovr_fake(c, b); } }
static const PPCFuncMapping map[] = {{0x80004000, f}, {0, nullptr}};
static const PPCRegionCode code = {"SNCP8P", 0, map, nullptr, 0, nullptr, 0, 0, 0};
extern const PPCRegionCode* const g_ppcRegionCode[] = {&code, nullptr};
''')
            (d / 'main.cpp').write_text('''#include "jni.h"
#include <cstring>
extern "C" jstring Java_org_soniccolorsrecomp_android_RecompCode_gameId(JNIEnv*, jclass);
int main() { JNIEnv e; return std::strcmp(Java_org_soniccolorsrecomp_android_RecompCode_gameId(&e, nullptr), "SNCP8P"); }
''')
            subprocess.run(['g++', '-std=c++20', '-I', str(d), '-I', str(ROOT / 'runtime/cpu'),
                            str(ROOT / 'android/app/src/main/cpp/recomp_bridge.cpp'),
                            str(ROOT / 'android/app/src/main/cpp/recomp_unavailable.cpp'),
                            str(d / 'stubs.cpp'), str(d / 'fake.cpp'), str(d / 'main.cpp'),
                            '-o', str(d / 'test')], check=True)
            subprocess.run([str(d / 'test')], check=True)


if __name__ == '__main__': unittest.main()
