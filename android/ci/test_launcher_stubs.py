"""Guards the launcher build's stand-ins for the generated PPC code.

The launcher APK is built from this repository alone, so generated/ (whole-disc-derived) is not in
it. The shared runtime still refers to the generated release table and to the SDK helper that
runtime/perf.cpp calls, so a launcher build links no_generated.cpp instead: an empty region table and
an abort-on-call body, never a fake result. These tests hold that arrangement together, and check the
anonymous-namespace linkage bug that made the symbols look undefined when they were defined.
"""
import pathlib
import re
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
CMAKE = ROOT / 'android/app/src/main/cpp/CMakeLists.txt'
STUBS = ROOT / 'android/app/src/main/cpp/no_generated.cpp'
APP = ROOT / 'runtime/platform/android/app.cpp'


class LauncherStubTests(unittest.TestCase):
    def test_stub_defines_every_symbol_the_runtime_needs_from_generated_code(self):
        text = STUBS.read_text()
        # region.cpp walks g_ppcRegionCode and copies through g_ppcDelta; perf.cpp calls fn_803E19B0.
        for symbol in ('g_ppcRegionCode', 'g_ppcRelease', 'g_ppcDelta', 'g_ppcDeltaCount'):
            # A definition, not a reference: the initializer is what the shared code links against.
            self.assertRegex(text, r'(?m)^[^\n]*\b' + symbol + r'\b[^\n]*=')
        self.assertIn('PPC_GEN_FUNC(fn_803E19B0)', text)
        # The table must be empty: a release entry without its code would select and then crash.
        self.assertIn('{ nullptr }', text)

    def test_stub_compiles_with_only_the_runtime_target_include_dirs(self):
        """scr_runtime has no runtime/cpu include dir, so the header has to be spelled from here."""
        done = subprocess.run(
            ['g++', '-std=c++20', '-fsyntax-only', '-I', '.', '-I', 'runtime',
             '-I', 'runtime/platform/android', '-include', 'runtime/platform/compat.h', str(STUBS)],
            capture_output=True, text=True, cwd=str(ROOT))
        self.assertEqual(done.returncode, 0, done.stderr[-2000:])

    def test_stub_does_not_invent_code(self):
        """Anything it cannot provide aborts; nothing returns a plausible-looking value."""
        text = STUBS.read_text()
        self.assertIn('std::abort()', text)
        self.assertNotIn('return 0;', text)

    def test_cmake_uses_the_stub_only_without_generated_code(self):
        text = CMAKE.read_text()
        # The option has to be declared before the target that consults it.
        self.assertLess(text.index('option(SCR_COMPILE_GENERATED'), text.index('no_generated.cpp'))
        self.assertRegex(text, r'if\(NOT SCR_COMPILE_GENERATED\)\s*\n\s*target_sources\(scr_runtime PRIVATE no_generated\.cpp\)')

    def test_audio_suspend_is_declared_through_its_header(self):
        """A block-scope declaration inside an extern "C" JNI function takes C linkage.

        android_app.cpp declaring AudioOut_SetSuspended inside its extern "C" JNI entry points
        named a C symbol, while audio_out.cpp defines the C++ one, so the link failed with
        "undefined symbol: AudioOut_SetSuspended" and "did you mean to declare ... as extern C".
        """
        text = (ROOT / 'runtime/platform/android/android_app.cpp').read_text()
        self.assertIn('#include "../../audio/audio_out.h"', text)
        self.assertNotIn('extern void AudioOut_SetSuspended', text)

    def test_gpu_thread_is_declared_at_file_scope(self):
        """An extern inside the anonymous namespace names a hidden symbol nothing defines.

        That was a real link failure: app.cpp's BootAndRun is in an anonymous namespace, and
        `extern bool g_gpuThread;` inside it referred to (anonymous namespace)::g_gpuThread rather
        than the variable gx_fifo.cpp defines, so ld.lld reported it undefined.
        """
        text = APP.read_text()
        self.assertIn('\nextern bool g_gpuThread;\n', text)   # column 0: file scope
        for line in text.splitlines():
            if 'extern bool g_gpuThread' in line:
                self.assertEqual(line, 'extern bool g_gpuThread;',
                                 'g_gpuThread must be declared at file scope, not inside a block')
        # And the member function that used to redeclare it just uses it now.
        self.assertNotRegex(text, r'\{\s*\n\s*extern bool g_gpuThread;')

    def test_runtime_builds_for_the_emulator_abi_too(self):
        """The instrumentation job runs on an x86_64 emulator; arm64-only would test nothing.

        The switch is chosen per ABI, and the delivered APK stays arm64-v8a unless the job asks
        for the test ABI.
        """
        cmake = CMAKE.read_text()
        self.assertIn('ANDROID_ABI STREQUAL "x86_64"', cmake)
        self.assertIn('runtime/platform/linux/fiber.cpp', cmake)
        gradle = (ROOT / 'android/app/build.gradle').read_text()
        self.assertIn("project.findProperty('testAbis')", gradle)
        self.assertIn("['arm64-v8a']", gradle)

    def test_the_native_libraries_may_be_absent(self):
        """A missing library must be a no-op, not a crash in a class initializer."""
        java = ROOT / 'android/app/src/main/java/org/soniccolorsrecomp/android'
        for name in ('AndroidInput.java', 'NativeProbe.java', 'RecompCode.java'):
            text = (java / name).read_text()
            self.assertIn('UnsatisfiedLinkError', text, name)
            self.assertNotIn('static { System.loadLibrary', text, name)
        test = (ROOT / 'android/app/src/androidTest/java/org/soniccolorsrecomp/android'
                / 'MainActivityTest.java').read_text()
        # Skipped where the library is missing, so the run still reports the launcher tests.
        self.assertIn('assumeTrue', test)


if __name__ == '__main__':
    unittest.main()
