"""Exercise verifier argument boundaries without mounting or starting an API."""
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'packaging/pi/verify-pi4-image.sh'


class PiImageVerifierTests(unittest.TestCase):
    def invoke(self, *args):
        return subprocess.run(['bash', str(SCRIPT), *args], capture_output=True,
                              text=True, timeout=10)

    def test_help_and_syntax(self):
        subprocess.run(['bash', '-n', str(SCRIPT)], check=True)
        result = self.invoke('--help')
        self.assertEqual(result.returncode, 0)
        self.assertIn('--image', result.stdout)
        self.assertIn('--root', result.stdout)

    def test_requires_one_input_and_workdir(self):
        for args in ((), ('--image',), ('--root', '/unused'),
                     ('--workdir', '/unused'),
                     ('--image', 'unused.img.xz', '--root', '/unused', '--workdir', '/unused'),
                     ('--unknown',)):
            with self.subTest(args=args):
                self.assertNotEqual(self.invoke(*args).returncode, 0)

    def test_verifies_profile_groups_and_service_policy(self):
        source = SCRIPT.read_text()
        for contract in ('VECTORWARP_RSPDUO_COUNTER_SCALE=auto',
                         'VECTORWARP_LINUX_SERVICE=1',
                         'grep -qx plugdev'):
            self.assertIn(contract, source)
        self.assertIn('! id -nG vectorwarp-api', source)


if __name__ == '__main__':
    unittest.main()
