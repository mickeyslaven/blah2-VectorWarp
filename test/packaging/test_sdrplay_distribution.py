"""No hardware or SDK needed: enforce source/release redistribution boundaries."""
from pathlib import Path
import json
import re
import subprocess
import unittest
import yaml

ROOT = Path(__file__).resolve().parents[2]


def artifact_matrix():
    return json.loads(subprocess.check_output(["python3", str(ROOT / "script/artifact-plan.py"), "matrix"], text=True))["include"]


def workflow_document():
    return yaml.safe_load((ROOT / '.github/workflows/release-packages.yml').read_text())


class DistributionTests(unittest.TestCase):
    def test_vendor_installer_is_not_in_source(self):
        self.assertEqual(list((ROOT / 'lib').rglob('SDRplay*.run')), [])
        self.assertEqual(list((ROOT / 'lib').rglob('libsdrplay*')), [])
        self.assertEqual(list((ROOT / 'lib').rglob('sdrplay_api*.h')), [])
        self.assertFalse((ROOT / 'script/prepare-sdrplay-build-sdk.sh').exists(),
                         'The retired vendor-SDK staging path must not return')

    def test_public_verification_does_not_take_sdk_inputs(self):
        workflow = workflow_document()
        verify = workflow['jobs']['verify']
        text = str(verify)
        self.assertIn('-DBLAH2_ENABLE_RSPDUO=OFF', text)
        self.assertNotIn('prepare-sdrplay-build-sdk.sh', text)
        self.assertNotIn('secrets.', text)
        self.assertNotIn('VECTORWARP_SDRPLAY', str(workflow.get('env', {})))

    def test_actual_packages_stage_local_rspduo_kit_without_sdk_inputs(self):
        workflow = workflow_document()
        package = workflow['jobs']['package']
        self.assertNotIn('environment', package)
        text = str(package)
        self.assertEqual(text.count('script/artifact-plan.py native-package'), 3)
        self.assertNotIn('prepare-sdrplay-build-sdk.sh', text)
        self.assertNotIn('vectorwarp-sdrplay-sdk', text)
        self.assertNotIn('VECTORWARP_SDRPLAY', text)
        self.assertNotIn('release-signing', str(package.get('runs-on')))
        self.assertNotIn('actions/cache', text)
        uploads = [step for step in package['steps'] if step.get('uses', '').startswith('actions/upload-artifact@')]
        self.assertEqual([step['with']['path'] for step in uploads], [
            'service-evidence/',
            'previous-package/resolution.txt\nprevious-package/bootstrap-*.json\nupgrade-evidence/\n',
            'dist/'
        ])
        evidence = uploads[0]
        self.assertEqual(evidence['if'], 'always()')
        self.assertTrue(evidence['with']['name'].startswith('installed-service-'))
        upgrade = uploads[1]
        self.assertEqual(upgrade['if'], 'always()')
        self.assertTrue(upgrade['with']['name'].startswith('package-upgrade-'))
        packager = (ROOT / 'script/package-native.sh').read_text()
        self.assertIn('script/artifact-plan.py" validate', packager)
        self.assertIn('--level package', packager)

    def test_pr_matrix_builds_the_same_local_kit_contract(self):
        package = workflow_document()['jobs']['package']
        matrix = artifact_matrix()
        expected = [('ubuntu' + version, 'deb', arch)
                    for version in ('22.04', '24.04', '26.04')
                    for arch in ('amd64', 'arm64')]
        expected += [('debian12', 'deb', 'arm64'),
                     ('debian13', 'deb', 'amd64'), ('debian13', 'deb', 'arm64'),
                     ('fedora43', 'rpm', 'x86_64'),
                     ('fedora44', 'rpm', 'x86_64'), ('fedora44', 'rpm', 'aarch64')]
        # Check each supported target (including Bookworm ARM64), not just a
        # count that could hide a missing target behind a duplicated entry.
        self.assertCountEqual([(row['distro'], row['format'], row['arch'])
                               for row in matrix], expected)
        pr_steps = [step for step in package['steps']
                    if 'script/artifact-plan.py native-package' in step.get('run', '')]
        self.assertEqual(len(pr_steps), 3)
        text = str(pr_steps)
        self.assertEqual(text.count('script/artifact-plan.py native-package'), 3)
        self.assertNotIn('--test-only', text)
        self.assertNotIn('vectorwarp-sdrplay-sdk', text)
        self.assertNotIn('VECTORWARP_SDRPLAY', text)
        uploads = [step for step in package['steps']
                   if step.get('uses', '').startswith('actions/upload-artifact@')]
        release_uploads = [step for step in uploads if step['with']['path'] == 'dist/']
        self.assertEqual(len(release_uploads), 1)
        self.assertNotIn('if', release_uploads[0])
        self.assertTrue(all('github.event_name' not in step.get('if', '') for step in pr_steps))

    def test_notice_files_follow_the_existing_html_payload(self):
        for name in ('plotly-LICENSE.txt', 'plotly.min.js.LICENSE.txt', 'ieee754-LICENSE.txt'):
            self.assertGreater((ROOT / 'html/lib' / name).stat().st_size, 200)

    def test_signed_asset_count_matches_build_matrix(self):
        workflow = workflow_document()
        matrix = artifact_matrix()
        signing = workflow['jobs']['sign-and-draft-release']
        collection = next(step for step in signing['steps']
                          if step.get('name') == 'Collect final signed release assets and checksums')
        count = re.search(r'(?m)^test \$\(find release-assets .+\| wc -l\) -eq ([0-9]+)$',
                          collection['run'])
        self.assertIsNotNone(count, 'signed assets must be checked before draft creation')
        self.assertEqual(int(count[1]), len(matrix),
                         'signing must accept exactly the complete package build matrix')


if __name__ == '__main__':
    unittest.main()
