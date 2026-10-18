import importlib.util
import pathlib
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'script/vectorwarp-gpu-link-watchdog.py'


class GpuLinkWatchdogTest(unittest.TestCase):
    def test_speed_parser_is_strict(self):
        spec = importlib.util.spec_from_file_location('gpu_link_watchdog', SCRIPT)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        self.assertEqual(module.speed('8.0 GT/s PCIe'), 8.0)
        with self.assertRaises(module.Refused):
            module.speed('8 GT/s; touch /tmp/no')

    def test_healthy_link_never_contacts_api_or_recovers(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            device = root / 'sys/bus/pci/devices/0000:01:00.0'
            device.mkdir(parents=True)
            (device / 'current_link_speed').write_text('8.0 GT/s PCIe\n')
            (device / 'max_link_speed').write_text('8.0 GT/s PCIe\n')
            result = subprocess.run(
                [sys.executable, str(SCRIPT), '--bdf', '0000:01:00.0',
                 '--sysfs', str(root / 'sys'), '--state-dir', str(root / 'state'),
                 '--timing-url', 'http://127.0.0.1:1/never'],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('link healthy', result.stdout)

    def test_bad_bdf_is_refused(self):
        result = subprocess.run(
            [sys.executable, str(SCRIPT), '--bdf', '../../remove', '--sysfs', '/tmp'],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
        self.assertEqual(result.returncode, 1)
        self.assertIn('Invalid GPU PCI BDF', result.stderr)


if __name__ == '__main__':
    unittest.main()
