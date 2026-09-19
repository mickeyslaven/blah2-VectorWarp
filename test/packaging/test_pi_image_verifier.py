"""Contract gates for the Pi image verifier; no image or namespace is created."""
from pathlib import Path
import subprocess
import unittest
ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'packaging/pi/verify-pi4-image.sh'
class PiImageVerifierTests(unittest.TestCase):
    def test_syntax_and_isolation_contract(self):
        subprocess.run(['bash', '-n', str(SCRIPT)], check=True)
        text = SCRIPT.read_text()
        for value in ('--image', '--root', '--workdir', 'losetup --find --show --partscan',
                      'mount -o ro,nosuid,nodev', 'unshare --mount --net --pid --fork',
                      'timeout 30s unshare', 'BLAH2_SETUP_PORT=39081', '/api/config', '/api/system/status',
                      '--groups=vectorwarp-config', 'mesa-vulkan-drivers', 'libsdrplay_api.so*',
                      'raspberrypi-sys-mods', 'readelf -h'):
            self.assertIn(value, text)
        self.assertNotIn('BLAH2_PREVIEW=true', text)
        self.assertNotIn('/dev/sd', text)
if __name__ == '__main__': unittest.main()
