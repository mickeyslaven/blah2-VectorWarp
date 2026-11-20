"""Ablations must arrive as explicit profiles, never replay-only tracker rewrites."""
import copy
import importlib.util
from pathlib import Path
import tempfile
import unittest


PATH = Path(__file__).parents[2] / "script" / "run-iq-protection-replay.py"
SPEC = importlib.util.spec_from_file_location("replay_fusion", PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class TrackerAblationTest(unittest.TestCase):
    def test_all_receiver_backend_and_tracker_profiles_keep_exact_intent(self):
        with tempfile.TemporaryDirectory() as temporary:
            recording = Path(temporary) / 'fixture.iq';recording.write_bytes(b'not replayed')
            for receiver in ('Kraken', 'Usrp', 'RspDuo', 'HackRf'):
                for backend in ('cpu', 'gpu', 'gpu-resident', 'auto'):
                    for stationary in (None, False, True):
                        process = {'performance': {'acceleration': backend, 'fft_threads': 7},
                            'tracker': {'enable': True, 'initiate': {'M': 3, 'N': 17}, 'delete': 7},
                            'detection': {'track_before_detect': {'enable': True, 'max_cpis': 8,
                                                                'fractional_delay': True}}}
                        if stationary is not None:
                            process['tracker']['initiate']['stationary_clutter'] = {
                                'enable': stationary, 'persistence_cpis': 91, 'max_doppler_hz': 12.5}
                        profile = {'configuration_complete': True, 'process': process, 'capture': {
                            'source_receiver': receiver, 'channel_count': 2, 'reference_channel': 0,
                            'reference_mode': 'array', 'reference_channels': [0, 1],
                            'surveillance_channels': [1], 'sample_rate_hz': 2000,
                            'frequency_hz': 100000000, 'array_geometry': None}}
                        before = copy.deepcopy(profile)
                        config = MODULE.configuration(recording, profile, 1, {})
                        self.assertEqual(config['process'], process)
                        self.assertEqual(config['capture']['device']['reference_channels'], [0, 1])
                        self.assertEqual(profile, before)

    def test_exact_process_identity_rejects_any_post_admission_stage_drift(self):
        config = {'process': {'detection': {'enable': True}, 'tracker': {'delete': 7}}}
        expected = MODULE.process_identity(config['process'])
        MODULE.require_process_identity(config, expected)
        for section, field, value in (('detection', 'enable', False), ('tracker', 'delete', 3)):
            changed = copy.deepcopy(config);changed['process'][section][field] = value
            with self.assertRaisesRegex(ValueError, 'changed after admission'):
                MODULE.require_process_identity(changed, expected)


if __name__ == "__main__":
    unittest.main()
