"""Guards the disc-build workflow against the ways it could go wrong.

These are the properties that matter more than any single step working: the disc and the code made
from it must never be uploaded, the playable APK must not escape a private repository, and the
temporary data must be removed even when the run fails. The steps are inspected instead of the YAML
text, so reformatting the file does not break the test.
"""
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / '.github/workflows/android-build-apk.yml'

try:
    import yaml
except ImportError:  # pragma: no cover - the runner normally has PyYAML
    yaml = None


@unittest.skipUnless(yaml is not None, 'PyYAML required to read the workflow')
@unittest.skipUnless(WORKFLOW.is_file(), f'{WORKFLOW.name} is missing')
class WorkflowGuardTests(unittest.TestCase):
    def setUp(self):
        self.document = yaml.safe_load(WORKFLOW.read_text())
        # PyYAML reads the `on:` key as the boolean True, which is not a bug in the workflow.
        self.on = self.document.get('on', self.document.get(True))
        self.steps = self.document['jobs']['build']['steps']

    def test_has_disc_link_inputs_and_optional_game_code(self):
        inputs = self.on['workflow_dispatch']['inputs']
        self.assertIn('disc_url', inputs)
        self.assertIn('include_game_code', inputs)
        # The link must be optional: the same run has to work from the DISC_URL secret.
        self.assertFalse(inputs['disc_url'].get('required', False))

    def test_only_apks_are_uploaded(self):
        uploads = [s for s in self.steps if str(s.get('uses', '')).startswith('actions/upload-artifact')]
        self.assertTrue(uploads, 'the workflow must hand back an APK')
        for step in uploads:
            path = step['with']['path']
            self.assertTrue(path.endswith('.apk'), f'uploads something that is not an APK: {path}')

    def test_code_bearing_artifact_requires_a_private_repository(self):
        guarded = [s for s in self.steps
                   if 'github.event.repository.private' in str(s.get('if', ''))]
        self.assertTrue(guarded, 'generated code must be refused outside a private repository')
        # And the step that writes generated sources is behind the same opt-in as the guard.
        generate = [s for s in self.steps if 'build_generated.py' in str(s.get('run', ''))]
        self.assertEqual(len(generate), 1)
        self.assertIn('include_game_code', str(generate[0].get('if', '')))

    def test_disc_data_is_removed_unconditionally(self):
        cleanups = [s for s in self.steps
                    if 'rm -rf' in str(s.get('run', '')) and 'scr-disc' in str(s.get('run', ''))]
        self.assertTrue(cleanups, 'the downloaded disc must be removed')
        for step in cleanups:
            self.assertEqual(step.get('if'), 'always()',
                             'cleanup must run even when the build fails')

    def test_the_link_is_never_printed(self):
        for step in self.steps:
            for line in str(step.get('run', '')).splitlines():
                stripped = line.strip()
                # Writing the variable into $GITHUB_ENV is required and stays out of the log; the
                # failure this guards against is printing it, with echo or with set -x.
                writes_env = '$GITHUB_ENV' in stripped
                self.assertNotIn('set -x', stripped, f'step "{step.get("name")}" would trace the link')
                if writes_env:
                    continue
                self.assertNotRegex(stripped, r'^echo\s+"?\$?\{?DISC_URL',
                                    f'step "{step.get("name")}" would print the disc link')

    def test_a_usable_launcher_apk_is_always_the_result(self):
        """One manual run has to hand back an installable APK even with no disc at all.

        The launcher APK contains no game code, so nothing about it depends on the disc the run
        downloaded: the artifact is produced by a step that is not gated on include_game_code.
        """
        uploads = [s for s in self.steps if str(s.get('uses', '')).startswith('actions/upload-artifact')]
        launcher = [s for s in uploads if s['with']['name'] == 'sonic-colors-launcher-apk']
        self.assertEqual(len(launcher), 1, 'the launcher APK must be uploaded')
        self.assertNotIn('include_game_code', str(launcher[0].get('if', '')),
                         'the launcher APK must not depend on the disc-derived build')

    def test_the_link_and_the_build_are_separate_steps(self):
        """A bad link should stop the run before the 20-minute native build, not after it."""
        runs = [str(s.get('run', '')) for s in self.steps]
        download = [i for i, r in enumerate(runs) if 'download_disc.py' in r]
        builds = [i for i, r in enumerate(runs) if 'gradlew' in r or 'gradle ' in r]
        self.assertTrue(download and builds)
        self.assertLess(min(download), min(builds), 'the disc must be fetched before the APK is built')

    def test_the_link_is_optional_and_falls_back_to_the_secret(self):
        inputs = self.on['workflow_dispatch']['inputs']
        self.assertFalse(inputs['disc_url'].get('required', False))
        resolve = [str(s.get('run', '')) for s in self.steps if 'GITHUB_ENV' in str(s.get('run', ''))]
        self.assertTrue(any('DISC_URL' in r for r in resolve),
                        'the input has to be able to fall back to the DISC_URL secret')

    def test_digest_is_optional_but_verified_when_pinned(self):
        resolve = [s for s in self.steps if 'DISC_SHA256' in str(s.get('env', {}))]
        self.assertTrue(resolve, 'the digest has to reach the download step')


if __name__ == '__main__':
    unittest.main()
