"""Exercise image-builder input rejection without root, mounts, or downloads."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'packaging/pi/build-pi4-image.sh'
PROFILE = ROOT / 'contrib/systemd/pi4-rspduo-performance.conf'


class PiImageBuilderTests(unittest.TestCase):
    def invoke(self, *args):
        return subprocess.run(['bash', str(SCRIPT), *args], capture_output=True,
                              text=True, timeout=10)

    def test_help_and_syntax(self):
        subprocess.run(['bash', '-n', str(SCRIPT)], check=True)
        result = self.invoke('--help')
        self.assertEqual(result.returncode, 0)
        for option in ('--base', '--base-sha256', '--deb', '--output',
                       '--source-revision', '--allow-network'):
            self.assertIn(option, result.stdout)

    def test_required_inputs_fail_before_work(self):
        for args in ((), ('--base',), ('--unknown',)):
            with self.subTest(args=args):
                self.assertNotEqual(self.invoke(*args).returncode, 0)

    @unittest.skipUnless(sys.platform.startswith('linux'), 'builder requires Linux realpath')
    def test_unsafe_or_existing_outputs_are_preserved(self):
        # Real regular input files are sufficient: all of these cases must fail
        # before root/tool checks, decompression, or package inspection.
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            base = folder / 'base.img.xz'; base.write_bytes(b'not-an-image')
            deb = folder / 'vectorwarp.deb'; deb.write_bytes(b'not-a-package')
            output = folder / 'candidate.img.xz'
            common = ['--base', str(base), '--base-sha256', 'a' * 64,
                      '--deb', str(deb), '--output', str(output),
                      '--source-revision', 'b' * 40, '--allow-network']
            output.write_bytes(b'existing artifact')
            result = self.invoke(*common)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('refusing to replace --output', result.stderr)
            self.assertEqual(output.read_bytes(), b'existing artifact')
            output.unlink()
            provenance = Path(str(output) + '.provenance.json')
            provenance.write_bytes(b'existing provenance')
            result = self.invoke(*common)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('refusing to replace output provenance', result.stderr)
            self.assertEqual(provenance.read_bytes(), b'existing provenance')
            self.assertFalse(output.exists())
            malformed = common.copy(); malformed[3] = 'wrong'
            result = self.invoke(*malformed)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('SHA-256 hex digest', result.stderr)
            self.assertEqual(sorted(p.name for p in folder.iterdir()),
                             ['base.img.xz', 'candidate.img.xz.provenance.json', 'vectorwarp.deb'])

    def test_network_must_be_explicit(self):
        result = self.invoke('--base', 'unused.img.xz', '--base-sha256', 'a' * 64,
                             '--deb', 'unused.deb', '--output', 'unused.img.xz',
                             '--source-revision', 'b' * 40)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('--allow-network', result.stderr)

    def test_reviewed_profile_and_service_access_contract(self):
        settings = {line for line in PROFILE.read_text().splitlines()
                    if line.startswith('Environment=')}
        self.assertEqual(settings, {
            'Environment=OPENBLAS_NUM_THREADS=1',
            'Environment=OMP_NUM_THREADS=1',
            'Environment=VECTORWARP_FFTW_PLAN=measure',
            'Environment=VECTORWARP_CLUTTER_WORKERS=2',
            'Environment=VECTORWARP_RSPDUO_CPI_QUEUE=1',
            'Environment=VECTORWARP_RSPDUO_USB_MODE=bulk',
            'Environment=VECTORWARP_RSPDUO_COUNTER_SCALE=auto',
        })
        source = SCRIPT.read_text()
        self.assertIn("getent group plugdev >/dev/null", source)
        self.assertIn("groups plugdev vectorwarp", source)
        self.assertIn("VECTORWARP_LINUX_SERVICE=1", source)
        self.assertIn("'counter_scale_policy': 'auto'", source)


if __name__ == '__main__':
    unittest.main()
