"""No hardware or SDK needed: enforce source/release redistribution boundaries."""
from pathlib import Path
import json
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]


def workflow_document():
    # Reuse the API's existing YAML dependency; no extra Python package needed.
    result = subprocess.run(['node', '-e',
        "const fs=require('fs'),yaml=require('js-yaml'); "
        "process.stdout.write(JSON.stringify(yaml.load(fs.readFileSync(process.argv[1],'utf8'))));",
        str(ROOT / '.github/workflows/release-packages.yml')],
        cwd=str(ROOT / 'api'), check=True, text=True, capture_output=True, timeout=10)
    return json.loads(result.stdout)


class DistributionTests(unittest.TestCase):
    def test_vendor_installer_is_not_in_source(self):
        self.assertEqual(list((ROOT / 'lib').rglob('SDRplay*.run')), [])
        self.assertEqual(list((ROOT / 'lib').rglob('libsdrplay*')), [])
        self.assertEqual(list((ROOT / 'lib').rglob('sdrplay_api*.h')), [])

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
        self.assertEqual(package['environment'], "${{ github.event_name == 'pull_request' && 'pr-packaging' || 'release-signing' }}")
        text = str(package)
        self.assertEqual(text.count('--backend all'), 6)
        self.assertNotIn('prepare-sdrplay-build-sdk.sh', text)
        self.assertNotIn('vectorwarp-sdrplay-sdk', text)
        self.assertNotIn('VECTORWARP_SDRPLAY', text)
        self.assertNotIn('release-signing', str(package.get('runs-on')))
        self.assertNotIn('actions/cache', text)
        uploads = [step for step in package['steps'] if step.get('uses', '').startswith('actions/upload-artifact@')]
        self.assertEqual([step['with']['path'] for step in uploads], ['service-evidence/', 'dist/'])
        evidence = uploads[0]
        self.assertEqual(evidence['if'], 'always()')
        self.assertTrue(evidence['with']['name'].startswith('installed-service-'))
        packager = (ROOT / 'script/package-native.sh').read_text()
        self.assertIn('release artifact must not contain the SDRplay vendor SDK or runtime', packager)

    def test_pr_matrix_builds_the_same_local_kit_contract(self):
        package = workflow_document()['jobs']['package']
        matrix = package['strategy']['matrix']['include']
        expected = [('ubuntu' + version, 'deb', arch)
                    for version in ('22.04', '24.04', '26.04')
                    for arch in ('amd64', 'arm64')]
        expected += [('debian12', 'deb', 'arm64'),
                     ('debian13', 'deb', 'amd64'), ('debian13', 'deb', 'arm64'),
                     ('fedora44', 'rpm', 'x86_64'), ('fedora44', 'rpm', 'aarch64')]
        # Check each supported target (including Bookworm ARM64), not just a
        # count that could hide a missing target behind a duplicated entry.
        self.assertCountEqual([(row['distro'], row['format'], row['arch'])
                               for row in matrix], expected)
        pr_steps = [step for step in package['steps']
                    if 'PR verification' in step.get('name', '')]
        self.assertEqual(len(pr_steps), 3)
        text = str(pr_steps)
        self.assertEqual(text.count('--backend all'), 3)
        self.assertNotIn('--test-only', text)
        self.assertNotIn('vectorwarp-sdrplay-sdk', text)
        self.assertNotIn('VECTORWARP_SDRPLAY', text)
        uploads = [step for step in package['steps']
                   if step.get('uses', '').startswith('actions/upload-artifact@')]
        release_uploads = [step for step in uploads if step['with']['path'] == 'dist/']
        self.assertEqual(len(release_uploads), 1)
        self.assertEqual(release_uploads[0]['if'], "github.event_name != 'pull_request'")

    def test_notice_files_follow_the_existing_html_payload(self):
        for name in ('plotly-LICENSE.txt', 'plotly.min.js.LICENSE.txt', 'ieee754-LICENSE.txt'):
            self.assertGreater((ROOT / 'html/lib' / name).stat().st_size, 200)


if __name__ == '__main__':
    unittest.main()
