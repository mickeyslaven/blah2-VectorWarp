import hashlib
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import unittest

RECORDER = Path(__file__).with_name("record_iq.py")


def fixture(phase=4, samples=4096):
    return (struct.pack(">8I", 0x4D434851, 5, samples, phase, 0, 1, 0, 0)
            + struct.pack("<10f", *([527000000.0, 49.6] * 5))
            + bytes(range(256)) * (samples * 10 // 256))


class RecorderTest(unittest.TestCase):
    def run_capture(self, payload):
        with tempfile.TemporaryDirectory() as directory, socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen()
            listener.settimeout(10)
            def serve():
                with listener.accept()[0] as client:
                    # Exercise arbitrary TCP fragmentation, not just packet-aligned reads.
                    for offset in range(0, len(payload), 317):
                        client.sendall(payload[offset:offset + 317])
            worker = threading.Thread(target=serve)
            worker.start()
            output = Path(directory) / "test.mchq"
            result = subprocess.run([sys.executable, str(RECORDER), "--port",
                str(listener.getsockname()[1]), "--seconds", ".003", "--channels", "5",
                "--frequency", "527000000", "--output", str(output)], capture_output=True, timeout=15)
            worker.join(10)
            manifest = json.loads(Path(str(output) + ".json").read_text())
            saved = output.read_bytes() if output.exists() else None
            return result, manifest, saved

    def test_exact_raw_capture(self):
        payload = fixture() * 2
        result, manifest, saved = self.run_capture(payload)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(manifest["complete"])
        self.assertEqual(saved, payload)
        self.assertEqual(manifest["samples_per_channel"], 8192)
        self.assertEqual(manifest["sha256"], hashlib.sha256(payload).hexdigest())

    def test_truncation_rejected(self):
        result, manifest, saved = self.run_capture(fixture() + fixture()[:-1])
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(manifest["complete"])
        self.assertIsNone(saved)

    def test_timed_raw_capture_preserves_host_anchor_without_claiming_adc_utc(self):
        packets = []
        for sequence in range(2):
            legacy = bytearray(fixture()); legacy[3] = ord('T')
            extension = struct.pack('>4I10Q', 1, 96, 1, 0, 1, sequence * 4096,
                sequence, 1000000 + sequence * 1706667, 0, 1800000000000000000,
                100, 0, sequence, (sequence + 1) * 4096)
            packets.append(legacy[:72] + extension + legacy[72:])
        payload = b''.join(packets)
        result, manifest, saved = self.run_capture(payload)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(saved, payload)
        self.assertTrue(manifest['source_sequence_counter_available'])
        self.assertFalse(manifest['first_header']['ingest_timing']['hardware_utc'])

    def test_calibration_change_rejected(self):
        result, manifest, saved = self.run_capture(fixture() + fixture(phase=3))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Calibration", manifest["error"])
        self.assertIsNone(saved)


if __name__ == "__main__":
    unittest.main()
