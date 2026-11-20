import hashlib
import importlib.util
import pathlib
import struct
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    'live_weak_evidence', ROOT / 'script' / 'inspect-live-weak-evidence.py')
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def packed(fmt, *values):
    return struct.pack('<' + fmt, *values)


def header():
    return (b'VWWEAK01' + packed('IIIIQI', 1, 3, 2, 1, 1 << 20, 3) +
            packed('III', 0, 2, 12) + packed('ddddd', .001, .75, 4., .1, .9) +
            packed('III', 8, 2, 4) + packed('Iii', 4, -1, 0) +
            packed('ddd', -2., 0., 2.))


def record(sequence=1, encoding=2, generation=0, map_rate=8192):
    # Resident delay-major rows: row-major displayed row zero maps to native row 2.
    values = [float(index) * .125 for index in range(12)]
    size = 120 + 6 * (8 if encoding == 2 else 16)
    first = 9007199254740993 + (sequence - 1) * 4096
    return (b'FRMWEAK1' + packed('Q', size) +
            packed('QQQQQQQQIIIIQ', 19, sequence, first, first + 4096,
                   527000000, 1, generation, 1794528000000 + sequence,
                   8192, map_rate, encoding, 0, 6) +
            packed(('f' if encoding == 2 else 'd') * 12, *values) +
            b'ENDWEAK1' + packed('Q', size))


class WeakEvidenceArtifactTests(unittest.TestCase):
    def inspect(self, data, cells=()):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'evidence.vww'
            path.write_bytes(data)
            return MODULE.inspect(path, cells)

    def test_full_precision_native_layout_and_exact_unsigned_identity(self):
        data = header() + record() + record(2)
        result = self.inspect(data, [(0, 1), (1, 0)])
        self.assertEqual(result['sha256'], hashlib.sha256(data).hexdigest())
        self.assertEqual(result['frames'][0]['first_sample'], '9007199254740993')
        self.assertEqual(result['frames'][0]['cells'][0]['complex_by_channel'], [[1.25, 1.375]])
        self.assertEqual(result['frames'][0]['cells'][1]['complex_by_channel'], [[0., .125]])
        self.assertFalse(result['frames'][0]['continuous_with_previous'])
        self.assertTrue(result['frames'][1]['continuous_with_previous'])
        self.assertFalse(result['detector_replay'])
        self.assertFalse(result['null_calibrated'])

    def test_cpu_layout_does_not_use_native_fft_shift(self):
        result = self.inspect(header() + record(encoding=1), [(0, 1), (1, 0)])
        cells = result['frames'][0]['cells']
        self.assertEqual(cells[0]['complex_by_channel'], [[.25, .375]])
        self.assertEqual(cells[1]['complex_by_channel'], [[.5, .625]])

    def test_gaps_and_invalidated_generations_never_claim_continuity(self):
        result = self.inspect(header() + record() + record(3) + record(4, generation=1))
        self.assertTrue(all(not frame['continuous_with_previous'] for frame in result['frames']))

    def test_capture_and_map_sample_rates_are_distinct(self):
        result = self.inspect(header() + record(map_rate=2048) + record(2, map_rate=4096), [(0, 1)])
        first = result['frames'][0]
        self.assertEqual(first['capture_sample_rate_hz'], 8192)
        self.assertEqual(first['map_sample_rate_hz'], 2048)
        self.assertEqual(first['cells'][0]['delay_samples'], 0)
        self.assertFalse(result['frames'][1]['continuous_with_previous'])
        with self.assertRaises(MODULE.ArtifactError):
            self.inspect(header() + record(map_rate=0))

    def test_truncation_empty_archive_and_malformed_footer_fail_closed(self):
        valid = header() + record()
        for corrupted in (header(), valid[:-1], valid[:-16], valid[:-64],
                          valid[:-16] + b'CORRUPT!' + valid[-8:], valid + b'garbage'):
            with self.subTest(size=len(corrupted)):
                with self.assertRaises(MODULE.ArtifactError):
                    self.inspect(corrupted)

    def test_unbounded_shape_payload_overflow_and_nonfinite_input_fail_closed(self):
        valid = bytearray(header() + record())
        malformed = []
        shape = valid.copy(); struct.pack_into('<I', shape, 12, 0xffffffff); malformed.append(shape)
        count = valid.copy(); struct.pack_into('<Q', count, len(header()) + 96, 0xffffffffffffffff)
        malformed.append(count)
        length = valid.copy(); struct.pack_into('<Q', length, len(header()) + 8, 0xffffffffffffffff)
        malformed.append(length)
        nonfinite = valid.copy(); struct.pack_into('<f', nonfinite, len(header()) + 104, float('nan'))
        malformed.append(nonfinite)
        for corrupted in malformed:
            with self.assertRaises(MODULE.ArtifactError):
                self.inspect(corrupted)
        with self.assertRaises(MODULE.ArtifactError):
            self.inspect(valid, [(3, 0)])


if __name__ == '__main__':
    unittest.main()
