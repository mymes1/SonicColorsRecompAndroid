"""Guards the launcher build's stand-ins for the generated PPC code.

The launcher APK is built from this repository alone, so generated/ (whole-disc-derived) is not in
it. The shared runtime still refers to the generated release table and to the SDK helper that
runtime/perf.cpp calls, so a launcher build links no_generated.cpp instead: an empty region table and
an abort-on-call body, never a fake result. These tests hold that arrangement together, and check the
anonymous-namespace linkage bug that made the symbols look undefined when they were defined.
"""
import pathlib
import re
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


if __name__ == '__main__':
    unittest.main()
