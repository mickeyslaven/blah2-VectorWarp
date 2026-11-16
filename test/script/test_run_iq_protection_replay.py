"""Bounded configuration/CLI checks: no processor, GPU or sockets are started."""
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    'run_iq_protection_replay', ROOT/'script'/'run-iq-protection-replay.py')
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)


def profile_fixture():
    return {
        'configuration_complete': True,
        'capture': {'source_receiver': 'Kraken', 'sample_rate_hz': 2400000,
                    'frequency_hz': 509000000, 'channel_count': 5,
                    'reference_channel': 0, 'surveillance_channels': [0, 1, 2, 3, 4],
                    'array_geometry': {'kind': 'fixture'}},
        'process': {
            'performance': {'acceleration': 'gpu-resident', 'fft_threads': 8},
            'surveillance_fusion': {'mode': 'noise_whitened_beam_bank', 'beams': 12},
            'reference_synthesis': {'mode': 'eigenbeam'},
            'detection': {'track_before_detect': {'enable': True, 'max_cpis': 8,
                                                 'fractional_delay': True}},
            'tracker': {'track_guided_shadow': {'enable': True, 'promote': False},
                        'initiate': {'stationary_clutter': {'enable': True}}},
        },
        'location': {'latitude': 0, 'longitude': 0},
    }


class ResidentSnapshotConfiguration(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.directory = self.root/'snapshots'
        self.directory.mkdir()
        self.profile = profile_fixture()
        self.profile_path = self.root/'profile.json'
        self.profile_path.write_text(json.dumps(self.profile))
        self.recording = self.root/'recording.mchq'
        self.recording.write_bytes(b'unit-test bytes, not replayed')
        self.source_hash = module.file_sha256(self.recording)
        self.rf_hash = module.file_sha256(self.profile_path)
        self.metadata = self.root/'metadata.json'
        self.metadata.write_text(json.dumps({'dataSha256': self.source_hash}))
        self.snapshot = {
            'enabled': True, 'output_directory': str(self.directory),
            'max_frames': 20, 'max_bytes': 512 << 20,
            'source_sha256': self.source_hash,
            'rf_profile_file': str(self.profile_path),
            'rf_profile_sha256': self.rf_hash,
        }

    def config(self, acceleration='cpu', snapshot=None, diagnostic=False,
               protection_beams=0, profile=None):
        return module.configuration(
            self.recording, profile or self.profile, 1234, {'map': 1235},
            acceleration, protection_beams, 0, diagnostic, None, None,
            None, None, resident_snapshot=snapshot)

    def args(self, overrides=None, acceleration='gpu-resident'):
        flags = {
            '--binary': str(self.root/'not-executed'),
            '--recording': str(self.recording), '--metadata': str(self.metadata),
            '--profile': str(self.profile_path), '--timeline': str(self.root/'timeline.json'),
            '--sha256': self.source_hash, '--output': str(self.root/'result.json'),
            '--acceleration': acceleration,
            '--resident-snapshot-directory': str(self.directory),
            '--resident-snapshot-max-frames': '20',
            '--resident-snapshot-max-bytes': str(512 << 20),
            '--resident-snapshot-rf-profile': str(self.profile_path),
            '--resident-snapshot-rf-profile-sha256': self.rf_hash,
        }
        flags.update(overrides or {})
        return [part for key, value in flags.items() if value is not None
                for part in (key, str(value))]

    def assert_cli_rejected(self, arguments):
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as result:
                module.parse_args(arguments)
        self.assertEqual(result.exception.code, 2)

    def test_default_cpu_and_gpu_config_unchanged_and_no_export_key(self):
        before = copy.deepcopy(self.profile)
        for acceleration in ('cpu', 'gpu'):
            config = self.config(acceleration)
            self.assertNotIn('resident_snapshot', config['capture']['replay'])
            self.assertEqual(config['capture']['replay'], {
                'state': True, 'loop': False, 'file': str(self.recording),
                'format': 'auto', 'provenance': 'legacy-unqualified'})
            expected = copy.deepcopy(self.profile['process'])
            expected['performance'] = {'surveillance_workers': 1, 'fft_threads': 1,
                                       'acceleration': acceleration}
            self.assertEqual(config['process'], expected)
        self.assertEqual(self.profile, before)

    def test_existing_cpu_fusion_diagnostic_behavior_preserved(self):
        process = self.config(diagnostic=True)['process']
        self.assertEqual(process['surveillance_fusion']['mode'], 'coherent_beam_bank')
        self.assertFalse(process['detection']['track_before_detect']['enable'])
        self.assertFalse(process['detection']['track_before_detect']['fractional_delay'])
        self.assertFalse(process['tracker']['track_guided_shadow']['enable'])
        self.assertFalse(process['tracker']['initiate']['stationary_clutter']['enable'])

    def test_resident_export_is_exact_and_does_not_disable_native_statistics(self):
        before = copy.deepcopy(self.profile)
        config = self.config('gpu-resident', self.snapshot)
        self.assertEqual(config['capture']['replay']['resident_snapshot'], self.snapshot)
        self.assertIsNot(config['capture']['replay']['resident_snapshot'], self.snapshot)
        self.assertTrue(config['capture']['replay']['state'])
        self.assertFalse(config['capture']['replay']['loop'])
        for field in ('surveillance_fusion', 'detection', 'tracker', 'reference_synthesis'):
            self.assertEqual(config['process'][field], self.profile['process'][field])
        self.assertEqual(self.profile, before)

    def test_offline_replay_disables_only_live_artifact_observer(self):
        profile = copy.deepcopy(self.profile)
        profile['process']['detection']['live_temporal_shadow'] = {
            'enabled': True, 'artifact_only': True, 'max_cpis': 4,
        }
        profile['process']['evidence'] = {'causal_funnel': {
            'enabled': True, 'max_candidates': 32,
        }}
        before = copy.deepcopy(profile)
        config = self.config('gpu-resident', self.snapshot, profile=profile)
        shadow = config['process']['detection']['live_temporal_shadow']
        self.assertFalse(shadow['enabled'])
        self.assertFalse(shadow['artifact_only'])
        self.assertEqual(shadow['max_cpis'], 4)
        funnel = config['process']['evidence']['causal_funnel']
        self.assertFalse(funnel['enabled'])
        self.assertEqual(funnel['max_candidates'], 32)
        self.assertEqual(profile, before)

    def test_resident_config_requires_export_and_export_requires_resident(self):
        with self.assertRaises(ValueError):
            self.config('gpu-resident')
        for acceleration in ('cpu', 'gpu'):
            with self.subTest(acceleration=acceleration), self.assertRaises(ValueError):
                self.config(acceleration, self.snapshot)

    def test_resident_config_rejects_cpu_only_overrides(self):
        with self.assertRaises(ValueError):
            self.config('gpu-resident', self.snapshot, diagnostic=True)
        with self.assertRaises(ValueError):
            self.config('gpu-resident', self.snapshot, protection_beams=12)

    def test_resident_rejects_incomplete_or_multi_lane_profile(self):
        for complete in (False, None):
            profile = copy.deepcopy(self.profile)
            profile['configuration_complete'] = complete
            with self.subTest(complete=complete), self.assertRaises(ValueError):
                self.config('gpu-resident', self.snapshot, profile=profile)
        profile = copy.deepcopy(self.profile)
        profile['process']['multi_illuminator'] = {'enabled': True}
        with self.assertRaises(ValueError):
            self.config('gpu-resident', self.snapshot, profile=profile)

    def test_every_export_field_required_and_disabled_not_accepted(self):
        for key in self.snapshot:
            snapshot = dict(self.snapshot)
            del snapshot[key]
            with self.subTest(key=key), self.assertRaises(ValueError):
                module.validate_resident_snapshot(snapshot)
        for updates in ({'enabled': False}, {'enabled': 1}, {'unknown': 1}):
            with self.subTest(updates=updates), self.assertRaises(ValueError):
                module.validate_resident_snapshot(dict(self.snapshot, **updates))

    def test_native_frame_and_per_frame_byte_limits(self):
        for key, values in {
            'max_frames': (None, False, 0, -1, 129, 2.0),
            'max_bytes': (None, True, 0, 2047, (512 << 20)+1, 2048.0),
        }.items():
            for value in values:
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    module.validate_resident_snapshot(dict(self.snapshot, **{key: value}))
        for frames, size in ((1, 2048), (128, 512 << 20)):
            module.validate_resident_snapshot(dict(self.snapshot, max_frames=frames, max_bytes=size))

    def test_output_directory_must_exist_be_absolute_and_empty(self):
        for directory in ('relative-snapshots', str(self.root/'missing'), str(self.profile_path)):
            with self.subTest(directory=directory), self.assertRaises(ValueError):
                module.validate_resident_snapshot(dict(self.snapshot, output_directory=directory))
        self.assertFalse((self.root/'missing').exists())
        marker = self.directory/'preserved'
        marker.write_text('existing data')
        with self.assertRaisesRegex(ValueError, 'empty'):
            module.validate_resident_snapshot(self.snapshot)
        self.assertEqual(marker.read_text(), 'existing data')

    def test_source_and_rf_hashes_fail_closed(self):
        for key in ('source_sha256', 'rf_profile_sha256'):
            for value in ('', '0'*64, 'x'*64, 'a'*63, None):
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    module.validate_resident_snapshot(dict(self.snapshot, **{key: value}))
        with self.assertRaisesRegex(ValueError, 'mismatch'):
            module.validate_resident_snapshot(dict(self.snapshot, rf_profile_sha256='a'*64))

    def test_rf_profile_must_exist_and_be_absolute(self):
        for path in ('profile.json', str(self.root/'missing.json'), str(self.directory)):
            with self.subTest(path=path), self.assertRaises(ValueError):
                module.validate_resident_snapshot(dict(self.snapshot, rf_profile_file=path))

    def test_cli_defaults_remain_cpu_without_export(self):
        args = module.parse_args([
            '--binary', 'unused', '--recording', 'unused', '--metadata', 'unused',
            '--profile', 'unused', '--timeline', 'unused', '--sha256', 'unused',
            '--output', 'unused'])
        self.assertEqual(args.acceleration, 'cpu')
        self.assertIsNone(module.resident_snapshot_options(args))

    def test_cli_complete_export_binds_exact_source_and_rf_profile(self):
        args = module.parse_args(self.args())
        self.assertEqual(module.resident_snapshot_options(args), self.snapshot)

    def test_cli_partial_export_and_wrong_acceleration_rejected(self):
        fields = ('directory', 'max-frames', 'max-bytes', 'rf-profile', 'rf-profile-sha256')
        for name in fields:
            with self.subTest(name=name):
                self.assert_cli_rejected(self.args({'--resident-snapshot-'+name: None}))
        self.assert_cli_rejected(self.args({'--resident-snapshot-'+name: None for name in fields}))
        for acceleration in ('cpu', 'gpu'):
            self.assert_cli_rejected(self.args(acceleration=acceleration))
        self.assert_cli_rejected(self.args()+['--diagnostic-cpu-fusion'])
        self.assert_cli_rejected(self.args()+['--protection-beams', '12'])

    def test_cli_rf_profile_must_match_profile_actually_used(self):
        unrelated = self.root/'unrelated.json'
        unrelated.write_text('{}')
        self.assert_cli_rejected(self.args({
            '--resident-snapshot-rf-profile': str(unrelated),
            '--resident-snapshot-rf-profile-sha256': module.file_sha256(unrelated)}))
        copy_path = self.root/'byte-identical.json'
        copy_path.write_bytes(self.profile_path.read_bytes())
        args = module.parse_args(self.args({'--resident-snapshot-rf-profile': str(copy_path)}))
        self.assertEqual(module.resident_snapshot_options(args)['rf_profile_file'], str(copy_path))

    def test_cli_validation_never_launches_processor(self):
        with patch.object(module, 'run') as run:
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                module.main(self.args({'--resident-snapshot-max-frames': '129'}))
            run.assert_not_called()

    def test_changed_source_fails_before_sockets_or_process(self):
        args = module.parse_args(self.args())
        self.recording.write_bytes(b'changed')
        with patch.object(module.HARNESS, 'StatusServer') as status:
            with patch.object(module.subprocess, 'Popen') as process:
                with self.assertRaisesRegex(ValueError, 'recording bytes'):
                    module.run(args)
                status.assert_not_called()
                process.assert_not_called()

    def test_resident_requires_explicit_device_before_sockets_or_process(self):
        args = module.parse_args(self.args())
        for device in ('', 'auto'):
            with self.subTest(device=device), patch.dict(module.os.environ, {'BLAH2_GPU_DEVICE': device}):
                with patch.object(module.HARNESS, 'StatusServer') as status:
                    with self.assertRaisesRegex(ValueError, 'BLAH2_GPU_DEVICE'):
                        module.run(args)
                    status.assert_not_called()


if __name__ == '__main__':
    unittest.main()
