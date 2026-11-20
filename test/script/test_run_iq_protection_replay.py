"""Bounded configuration/CLI checks: no processor, GPU or sockets are started."""
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import os
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
            'tracker': {'initiate': {'stationary_clutter': {'enable': True}}},
        },
        'location': {'latitude': 0, 'longitude': 0},
    }


class AdmissionTruthContract(unittest.TestCase):
    def admission(self, rows=None, endpoints=None):
        return {"captures": {"valid": {"valid": True,
            "cpis": [{"timestamp_ms": value} for value in (endpoints or [10])],
            "truth_samples": rows if rows is not None else [
                {"timestamp_ms": 10, "delay_km": -0.0, "doppler_hz": 2.5}]}}}

    def test_runner_and_scorer_share_exact_admission_without_mutation(self):
        admission = self.admission()
        admission["captures"]["invalid"] = {"valid": False, "cpis": [{}]}
        admission["captures"]["valid"]["truth_samples"].extend([
            {"timestamp_ms": 20, "delay_km": 7, "doppler_hz": 8},
            {"delay_km": float("nan"), "doppler_hz": 0}])
        before = copy.deepcopy(admission)
        for accept in (module.admitted_truth, module.SCORE.admitted_truth):
            self.assertEqual(accept(admission), {10: (-0.0, 2.5)})
            self.assertEqual(admission, before)
        # Bool-as-number behavior is legacy Python behavior, not tightened here.
        admission = self.admission([{"timestamp_ms": 10, "delay_km": True, "doppler_hz": False}])
        self.assertEqual(module.admitted_truth(admission), {10: (1.0, 0.0)})

    def test_empty_messages_remain_caller_specific(self):
        for accept, message in ((module.admitted_truth,
                "map diagnostics require valid admission truth samples"),
                (module.SCORE.admitted_truth, "admission contains no valid truth samples")):
            with self.assertRaisesRegex(ValueError, "^" + message + "$"):
                accept({})

    def test_bad_endpoints_nonfinite_and_conflicting_truth_still_fail_closed(self):
        cases = [
            (self.admission(endpoints=[None]), "admitted CPI has no timestamp"),
            (self.admission(rows=[]), "admitted CPI has no exact evaluation truth sample"),
            (self.admission(rows=[{"timestamp_ms": "10", "delay_km": 1, "doppler_hz": 2}]),
             "admitted truth sample has no integer timestamp"),
        ]
        for invalid in (float("nan"), float("inf"), -float("inf")):
            cases.append((self.admission(rows=[{"timestamp_ms": 10, "delay_km": invalid,
                "doppler_hz": 2}]), "admitted CPI has no exact evaluation truth sample"))
        conflict = self.admission()
        conflict["captures"]["second"] = {"valid": True, "cpis": [{"timestamp_ms": 10}],
            "truth_samples": [{"timestamp_ms": 10, "delay_km": 9, "doppler_hz": 2.5}]}
        cases.append((conflict, "conflicting admitted truth at one CPI endpoint"))
        for admission, message in cases:
            for accept in (module.admitted_truth, module.SCORE.admitted_truth):
                with self.subTest(message=message, accept=accept.__name__), self.assertRaisesRegex(ValueError, message):
                    accept(admission)

    def test_product_timestamps_select_cpis_from_boundary_timeline(self):
        products = {name: [{'timestamp': 1503}, {'timestamp': 2003}]
                    for name in ('detection', 'track', 'timing')}
        timeline = {'frames': [
            {'timestamp_ms': 1000, 'boundary': True},
            {'timestamp_ms': 1500, 'boundary': False},
            {'timestamp_ms': 2000, 'boundary': False}]}
        self.assertEqual([row['timestamp_ms'] for row in
            module.align_product_timeline(products, timeline)], [1500, 2000])

    def test_product_timeline_join_fails_on_shift_or_product_disagreement(self):
        products = {name: [{'timestamp': 1500}]
                    for name in ('detection', 'track', 'timing')}
        with self.assertRaisesRegex(ValueError, 'more than one boundary'):
            module.align_product_timeline(products,
                {'frames': [
                    {'timestamp_ms': 500}, {'timestamp_ms': 1000},
                    {'timestamp_ms': 1500}]})
        products['track'].append({'timestamp': 2000})
        with self.assertRaisesRegex(ValueError, 'counts differ'):
            module.align_product_timeline(products,
                {'frames': [{'timestamp_ms': 1500}]})


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

    def config(self, acceleration=None, snapshot=None, profile=None):
        return module.configuration(
            self.recording, profile or self.profile, 1234, {'map': 1235},
            acceleration, resident_snapshot=snapshot)

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
            expected['performance']['acceleration'] = acceleration
            self.assertEqual(config['process'], expected)
        self.assertEqual(self.profile, before)

    def test_relative_recording_is_resolved_before_isolated_processor_start(self):
        previous = Path.cwd()
        try:
            os.chdir(self.root)
            config = module.configuration(Path('recording.mchq'), self.profile,
                1234, {'map': 1235}, 'cpu')
        finally:
            os.chdir(previous)
        self.assertEqual(config['capture']['replay']['file'], str(self.recording))
        self.assertEqual(config['save']['path'], str(self.root))

    def test_native_replay_receives_external_geographic_trust_pins(self):
        profile = copy.deepcopy(self.profile); profile.pop('location')
        for mode in ('cpu', 'gpu', 'gpu-resident'):
            config = module.configuration(self.recording, profile, 1234, {'map': 1235},
                mode,
                resident_snapshot=self.snapshot if mode == 'gpu-resident' else None,
                recording_sha256=self.source_hash, acquisition_sha256='c' * 64)
            self.assertEqual(config['capture']['replay']['recording_sha256'], self.source_hash)
            self.assertEqual(config['capture']['replay']['acquisition_sha256'], 'c' * 64)
        self.assert_cli_rejected(self.args({'--acquisition-sha256': 'untrusted'}))

    def test_default_replay_preserves_all_processing_without_translation(self):
        profile = copy.deepcopy(self.profile)
        profile['process']['reference_synthesis'].update(
            target_protection_enabled=False,
            target_protection_beams=24,
            target_protection_beam_index=9)
        profile['process']['performance'].update(surveillance_workers=3, lane_workers=2)
        before = copy.deepcopy(profile)
        config = self.config(profile=profile)
        self.assertEqual(config['process'], profile['process'])
        self.assertIsNot(config['process'], profile['process'])
        self.assertEqual(profile, before)
        expected = module.process_identity(profile['process'])
        module.require_process_identity(config, expected)
        config['process']['detection']['track_before_detect']['fractional_delay'] = False
        with self.assertRaisesRegex(ValueError, 'changed after admission'):
            module.require_process_identity(config, expected)

    def test_replay_math_override_flags_are_unavailable(self):
        for flag, value in (('--diagnostic-cpu-fusion', None), ('--diagnostic-delay-max', '4'),
                ('--diagnostic-doppler-min', '-1'), ('--diagnostic-doppler-max', '1'),
                ('--diagnostic-clutter-delay-max', '5'), ('--stationary-clutter', 'off'),
                ('--protection-beams', '12'), ('--protection-index', '1')):
            with self.subTest(flag=flag):
                self.assert_cli_rejected(self.args() + [flag] + ([] if value is None else [value]))

    def test_complex_cutout_is_explicit_bounded_and_replay_only(self):
        output = self.root/'target.vwcut'
        args = module.parse_args([
            '--binary', str(self.root/'unused'), '--recording', str(self.recording),
            '--metadata', str(self.metadata), '--profile', str(self.profile_path),
            '--timeline', str(self.root/'timeline.json'), '--sha256', self.source_hash,
            '--output', str(self.root/'result.json'), '--complex-cutout', str(output),
            '--complex-cutout-delay-km', '40.2', '--complex-cutout-doppler-hz', '-92'])
        options = module.complex_cutout_options(args)
        self.assertEqual(options, {'state': True, 'path': str(output),
            'delay_km': 40.2, 'doppler_hz': -92.0, 'delay_radius_bins': 8,
            'doppler_radius_bins': 8, 'max_frames': 128, 'max_mb': 64})
        config = module.configuration(self.recording, self.profile, 1234,
            {'map': 1235}, 'cpu',
            complex_cutout=options)
        self.assertEqual(config['save']['complex_cutout'], options)
        self.assertIsNot(config['save']['complex_cutout'], options)

    def test_partial_or_existing_complex_cutout_is_rejected(self):
        base = ['--binary', str(self.root/'unused'), '--recording', str(self.recording),
            '--metadata', str(self.metadata), '--profile', str(self.profile_path),
            '--timeline', str(self.root/'timeline.json'), '--sha256', self.source_hash,
            '--output', str(self.root/'result.json')]
        self.assert_cli_rejected(base + ['--complex-cutout', str(self.root/'new')])
        existing = self.root/'existing'; existing.write_text('preserve')
        self.assert_cli_rejected(base + ['--complex-cutout', str(existing),
            '--complex-cutout-delay-km', '1', '--complex-cutout-doppler-hz', '2'])
        self.assertEqual(existing.read_text(), 'preserve')

    def test_manifold_export_is_bounded_authenticated_and_enables_native_evidence(self):
        output = self.root/'manifold-evidence.jsonl'
        base = ['--binary', str(self.root/'unused'), '--recording', str(self.recording),
            '--metadata', str(self.metadata), '--profile', str(self.profile_path),
            '--timeline', str(self.root/'timeline.json'), '--sha256', self.source_hash,
            '--output', str(self.root/'result.json'), '--manifold-evidence-export', str(output)]
        self.assert_cli_rejected(base)
        self.assert_cli_rejected(base + ['--acquisition-sha256', 'c' * 64])
        args = module.parse_args(base + ['--acquisition-sha256', 'c' * 64,
                                        '--binary-sha256', 'd' * 64])
        options = module.manifold_export_options(args)
        self.assertEqual(options, {'path': output, 'max_bytes': 64 << 20})
        config = module.configuration(self.recording, self.profile, 1234,
            {'map': 1235, 'manifold': 1236}, 'cpu', recording_sha256=self.source_hash,
            acquisition_sha256='c' * 64, manifold_evidence_export=True)
        self.assertEqual(config['process']['bearing']['manifold_recorder']['enabled'], 'on')
        self.assertFalse(output.exists())

    def test_manifold_export_rejects_legacy_rounding_and_preserves_new_numbers(self):
        target = self.root/'new-manifold.jsonl'
        options = {'path': target, 'max_bytes': 4096}
        row = {'schema': 'vectorwarp-manifold-evidence-v2',
               'actual_gain_db': [7.699999809265137], 'response': [[1e-20, .12345678901234567]]}
        with self.assertRaisesRegex(ValueError, 'full-precision producer contract'):
            module.write_manifold_export([row], options)
        self.assertFalse(target.exists())
        row['precision_contract'] = 'binary64-json-roundtrip-v1'
        with self.assertRaisesRegex(ValueError, '64 MiB bound'):
            module.write_manifold_export([row], {**options, 'max_bytes': 1})
        payload = module.write_manifold_export([row], options)
        self.assertEqual(json.loads(payload), row)
        with self.assertRaises(FileExistsError):
            module.write_manifold_export([row], options)
        self.assertEqual(target.read_bytes(), payload)

    def test_ordinary_replay_preserves_inherited_manifold_recorder(self):
        profile = copy.deepcopy(self.profile)
        profile['process']['bearing'] = {'manifold_recorder': {'enabled': 'on', 'path': '/live/only'}}
        before = copy.deepcopy(profile)
        config = module.configuration(self.recording, profile, 1234, {'map': 1235},
            'cpu')
        self.assertEqual(config['process']['bearing'], profile['process']['bearing'])
        self.assertEqual(profile, before)

    def test_existing_or_relative_manifold_export_is_preserved_and_rejected(self):
        base = ['--binary', str(self.root/'unused'), '--recording', str(self.recording),
            '--metadata', str(self.metadata), '--profile', str(self.profile_path),
            '--timeline', str(self.root/'timeline.json'), '--sha256', self.source_hash,
            '--output', str(self.root/'result.json'), '--acquisition-sha256', 'c' * 64]
        self.assert_cli_rejected(base + ['--manifold-evidence-export', 'relative.jsonl'])
        existing = self.root/'existing-manifold.jsonl'; existing.write_text('preserve')
        self.assert_cli_rejected(base + ['--manifold-evidence-export', str(existing)])
        self.assertEqual(existing.read_text(), 'preserve')

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

    def test_offline_replay_does_not_silently_strip_or_disable_profile_fields(self):
        profile = copy.deepcopy(self.profile)
        profile['process']['detection']['live_temporal_shadow'] = {
            'enabled': True, 'artifact_only': True, 'max_cpis': 4,
        }
        profile['process']['tracker']['track_guided_shadow'] = {'enable': True}
        profile['process']['bearing'] = {
            'frequency_resolved_shadow': {'enabled': True},
            'manifold_automation': {'enabled': True}}
        profile['process']['surveillance_fusion']['detection_manifold_automation'] = {
            'enabled': True}
        profile['process']['evidence'] = {'causal_funnel': {
            'enabled': True, 'max_candidates': 32,
        }}
        before = copy.deepcopy(profile)
        config = self.config('gpu-resident', self.snapshot, profile=profile)
        self.assertEqual(config['process'], profile['process'])
        self.assertEqual(profile, before)

    def test_resident_replay_does_not_require_snapshot_observation(self):
        self.assertNotIn('resident_snapshot', self.config('gpu-resident')['capture']['replay'])
        for acceleration in ('cpu', 'gpu'):
            with self.subTest(acceleration=acceleration), self.assertRaises(ValueError):
                self.config(acceleration, self.snapshot)

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

    def test_cli_default_preserves_recorded_backend_without_export(self):
        args = module.parse_args([
            '--binary', 'unused', '--recording', 'unused', '--metadata', 'unused',
            '--profile', 'unused', '--timeline', 'unused', '--sha256', 'unused',
            '--output', 'unused'])
        self.assertIsNone(args.acceleration)
        self.assertIsNone(module.resident_snapshot_options(args))

    def test_cli_complete_export_binds_exact_source_and_rf_profile(self):
        args = module.parse_args(self.args())
        self.assertEqual(module.resident_snapshot_options(args), self.snapshot)

    def test_cli_partial_export_and_wrong_acceleration_rejected(self):
        fields = ('directory', 'max-frames', 'max-bytes', 'rf-profile', 'rf-profile-sha256')
        for name in fields:
            with self.subTest(name=name):
                self.assert_cli_rejected(self.args({'--resident-snapshot-'+name: None}))
        self.assertIsNone(module.resident_snapshot_options(module.parse_args(
            self.args({'--resident-snapshot-'+name: None for name in fields}))))
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
        with patch.object(module.SESSION, 'StatusServer') as status:
            with patch.object(module.SESSION.subprocess, 'Popen') as process:
                with self.assertRaisesRegex(ValueError, 'recording bytes'):
                    module.run(args)
                status.assert_not_called()
                process.assert_not_called()

    def test_multi_illuminator_recording_is_rejected_before_sockets(self):
        profile = copy.deepcopy(self.profile)
        profile['process']['multi_illuminator'] = {'enabled': True, 'illuminators': [
            {'id': 'one', 'enabled': True}, {'id': 'two', 'enabled': True}]}
        self.profile_path.write_text(json.dumps(profile))
        args = module.parse_args(self.args({
            '--resident-snapshot-rf-profile-sha256': module.file_sha256(self.profile_path)}))
        with patch.object(module.SESSION, 'StatusServer') as status:
            with self.assertRaisesRegex(ValueError, 'single-lane protection replay'):
                module.run(args)
            status.assert_not_called()

    def test_resident_requires_explicit_device_before_sockets_or_process(self):
        args = module.parse_args(self.args())
        for device in ('', 'auto'):
            with self.subTest(device=device), patch.dict(module.os.environ, {'BLAH2_GPU_DEVICE': device}):
                with patch.object(module.SESSION, 'StatusServer') as status:
                    with self.assertRaisesRegex(ValueError, 'BLAH2_GPU_DEVICE'):
                        module.run(args)
                    status.assert_not_called()


if __name__ == '__main__':
    unittest.main()
