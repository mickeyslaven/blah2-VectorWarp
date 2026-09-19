"""Fast contract gates for the destructive Pi-image assembly entry point."""

from pathlib import Path
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "packaging/pi/build-pi4-image.sh"


class PiImageBuilderTests(unittest.TestCase):
    def test_shell_syntax_and_explicit_inputs(self):
        subprocess.run(["bash", "-n", str(SCRIPT)], check=True)
        help_text = subprocess.run(["bash", str(SCRIPT), "--help"], check=True,
                                   capture_output=True, text=True).stdout
        for option in ("--base", "--base-sha256", "--deb", "--output", "--source-revision", "--allow-network"):
            self.assertIn(option, help_text)
        self.assertIn("never flashes or writes a host disk", " ".join(help_text.split()))

    def test_only_a_new_loop_device_is_written_and_cleanup_is_trapped(self):
        text = SCRIPT.read_text()
        self.assertIn('loop=$(losetup --find --show --partscan "$raw_image")', text)
        self.assertIn('cat "/sys/class/block/${loop##*/}/loop/backing_file"', text)
        self.assertIn('realpath "$(cat ', text)
        self.assertIn('trap cleanup EXIT INT TERM', text)
        self.assertIn('retaining %s for recovery', text)
        self.assertIn('losetup -d "$loop"', text)
        self.assertNotIn('/dev/sd', text)
        self.assertNotIn('dd of=', text)

    def test_image_contract_preserves_imager_and_sanitises_identity(self):
        text = SCRIPT.read_text()
        self.assertIn('VERSION_CODENAME=bookworm', text)
        self.assertIn('ID=debian', text)
        self.assertIn('NetworkManager', text)
        self.assertIn('vectorwarp-api.service', text)
        self.assertIn('vectorwarp-processor.service', text)
        self.assertIn('getent passwd 1000', text)
        self.assertIn('getent passwd testPi', text)
        self.assertIn('libsdrplay_api.so*', text)
        self.assertIn('raspberry_pi_imager_user_ssh_wifi', text)
        self.assertIn('xz -T2 -6 --memlimit-compress=512MiB --stdout "$raw_image" >"$work/image.img.xz"', text)
        self.assertIn('output.provenance.json', text)
        self.assertIn('libopenblas0-pthread', text)
        self.assertIn('config-pi4-rspduo.yml', text)
        self.assertIn('fc: 100000000', text)
        self.assertIn('pi4-rspduo-performance.conf', text)
        self.assertIn('brcm,bcm2711', text)


if __name__ == "__main__":
    unittest.main()
