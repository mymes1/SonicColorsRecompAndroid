"""Guards the shaderc prerequisite that the Vulkan renderer cannot do without.

The NDK does not ship shaderc prebuilt: the build has to compile it from
`sources/third_party/shaderc` first, and NDK r27 removed those sources. These tests hold the two
halves of that together — the workflow runs the script before Gradle, and the script refuses an NDK
that cannot provide the library instead of letting CMake fail later with a missing header.
"""
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'android/ci/build_shaderc.sh'
CMAKE = ROOT / 'android/app/src/main/cpp/CMakeLists.txt'

try:
    import yaml
except ImportError:  # pragma: no cover - the runner normally has PyYAML
    yaml = None


class ShadercBuildTests(unittest.TestCase):
    def test_the_cmake_file_links_the_ndk_build_of_shaderc(self):
        text = CMAKE.read_text()
        self.assertIn('sources/third_party/shaderc', text)
        # It must be built into target_link_libraries, not merely found.
        self.assertRegex(text, r'target_link_libraries\(scr_runtime PRIVATE\s+\$\{SCR_SHADERC_LIB\}')

    def test_cmake_no_longer_requires_the_library_to_be_in_the_ndk(self):
        # The old block demanded a prebuilt library with REQUIRED and failed on r26/r27; the new one
        # documents how to build it instead.
        text = CMAKE.read_text()
        self.assertNotRegex(text, r'find_library\(SCR_SHADERC_LIB shaderc REQUIRED\)')
        self.assertIn('android/ci/build_shaderc.sh', text)

    def test_script_is_executable_and_parses(self):
        self.assertTrue(os.access(SCRIPT, os.X_OK), 'the workflows invoke it as a script')
        subprocess.run(['bash', '-n', str(SCRIPT)], check=True)

    def test_script_explains_an_ndk_without_shaderc_sources(self):
        """r27 dropped shaderc; the failure must name the cause, not just exit 1."""
        with tempfile.TemporaryDirectory() as ndk:
            pathlib.Path(ndk, 'source.properties').write_text('Pkg.Revision = 27.2.12479018\n')
            done = subprocess.run(['bash', str(SCRIPT)], capture_output=True, text=True,
                                  env={'ANDROID_NDK': ndk, 'PATH': os.environ['PATH']})
            self.assertNotEqual(done.returncode, 0)
            self.assertIn('r27', done.stderr)
            self.assertIn('sources/third_party/shaderc', done.stderr)

    def test_script_reports_a_missing_ndk_instead_of_crashing(self):
        with tempfile.TemporaryDirectory() as empty:
            env = {k: v for k, v in os.environ.items()
                   if k not in ('ANDROID_NDK', 'ANDROID_NDK_HOME', 'ANDROID_NDK_ROOT')}
            env['ANDROID_HOME'] = empty
            env['PATH'] = os.environ['PATH']
            # Gradle may name an NDK that is not installed; the script must still explain itself.
            done = subprocess.run(['bash', str(SCRIPT)], capture_output=True, text=True, env=env,
                                  cwd=str(ROOT))
            self.assertNotEqual(done.returncode, 0)
            self.assertIn('no NDK found', done.stderr)


@unittest.skipUnless(yaml is not None, 'PyYAML required to read the workflow')
class ShadercWorkflowTests(unittest.TestCase):
    WORKFLOWS = [ROOT / '.github/workflows/build.yml',
                 ROOT / '.github/workflows/android-apk.yml']

    def test_every_apk_build_builds_shaderc_before_gradle(self):
        for workflow in self.WORKFLOWS:
            if workflow.name == 'build.yml':
                # Renamed before; a missing file here means this test stopped checking anything.
                self.assertTrue(workflow.is_file(), f'{workflow} is missing')
            if not workflow.is_file():
                continue
            document = yaml.safe_load(workflow.read_text())
            jobs = document['jobs']
            for name, job in jobs.items():
                runs = [str(step.get('run', '')) for step in job.get('steps', [])]
                gradle = [i for i, run in enumerate(runs) if 'gradlew' in run or 'gradle ' in run]
                shaderc = [i for i, run in enumerate(runs) if 'build_shaderc.sh' in run]
                if not gradle:
                    continue
                self.assertTrue(shaderc, f'{workflow.name}:{name} links shaderc but never builds it')
                self.assertLess(min(shaderc), min(gradle),
                                f'{workflow.name}:{name} runs Gradle before building shaderc')


if __name__ == '__main__':
    unittest.main()
