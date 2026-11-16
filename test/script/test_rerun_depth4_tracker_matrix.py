"""Bounded policy/provenance tests; subprocess mocks never run IQ or C++ replay."""
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import math
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

PATH = Path(__file__).parents[2]/'script'/'rerun-depth4-tracker-matrix.py'
SPEC = importlib.util.spec_from_file_location('tracker_matrix', PATH)
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)


def hp_profile(**updates):
    profile = {
        'schema': module.PROFILE_SCHEMA, 'birth_snr_db': 5,
        'frequency_hz': 509000000, 'base_cpi_seconds': .5,
        'confirmation_m': 4, 'confirmation_n': 6, 'delete_after': 3,
        'max_acc': 10, 'adaptive_gate': True,
        'association_maximum_nis': 9.210340371976184,
        'birth_delay_half_width_bins': 1.0,
        'birth_doppler_half_width_bins': 1.0,
    }
    return dict(profile, **updates)


def record(profile=None, sequence=4, generation='1', reset_serial=0):
    profile = profile or hp_profile()
    return {
        'generation': generation, 'reset_serial': reset_serial,
        'sequence': str(sequence), 'capture_epoch': '1',
        'timestamp_ms': sequence*500, 'delay_bin_sample_rate_hz': 2400000,
        'provenance': {'processor_binary': {'sha256': 'b'*64},
                       'config_file': {'sha256': 'c'*64},
                       'loaded_revision': '1', 'config_revision': '1'},
        'native_detections': {'timestamp': sequence*500,
                              'delay': [], 'doppler': [], 'snr': []},
        'truth_delay_doppler': {},
        'search_extent': {'delay_min_km': 0, 'delay_max_km': 100},
        'windows': [{
            'depth': 4, 'latest_evaluated_sequence': str(sequence),
            'confirmation_m': profile['confirmation_m'],
            'confirmation_n': profile['confirmation_n'],
            'delete_after': profile['delete_after'],
            'base_cpi_seconds': profile['base_cpi_seconds'],
            'update_interval_seconds': 4*profile['base_cpi_seconds'],
            'latest_candidate_count': 2, 'latest_candidates_export_limit': 256,
            'latest_candidates': [
                {'delay_bins': 20, 'doppler_hz': 40, 'snr_db': snr,
                 'support_sequences': [str(n) for n in range(sequence, sequence-4, -1)],
                 'epoch': '1'} for snr in (7, 6)],
        }],
    }


def empty_run(*args, **kwargs):
    inputs = [json.loads(line) for line in kwargs['input'].splitlines()]
    frames = [{'index': row['index'], 'associations': 0, 'pair_in_gate': 0,
               'misses': 0, 'promotions': 0, 'births': 0,
               'birth_exclusive_rejected': 0, 'tracks': []} for row in inputs]
    return subprocess.CompletedProcess(args, 0, '\n'.join(map(json.dumps, frames)), '')


class PinnedProfile(unittest.TestCase):
    def test_every_required_field_is_explicit(self):
        for key in module.PROFILE_FIELDS:
            with self.subTest(key=key):
                profile = hp_profile()
                del profile[key]
                with self.assertRaises(ValueError):
                    module.validate_profile(profile)

    def test_no_frequency_or_wavelength_defaults(self):
        profile = hp_profile()
        del profile['frequency_hz']
        with self.assertRaises(ValueError):
            module.validate_profile(profile)

    def test_wavelength_only_and_consistent_pair(self):
        profile = hp_profile(wavelength_m=module.paired.C/509000000)
        expected = module.validate_profile(profile)
        del profile['frequency_hz']
        resolved = module.validate_profile(profile)
        self.assertTrue(math.isclose(resolved.pop('frequency_hz'),
                                     expected.pop('frequency_hz'), rel_tol=1e-12))
        self.assertEqual(resolved, expected)

    def test_inconsistent_frequency_wavelength_rejected(self):
        with self.assertRaisesRegex(ValueError, 'disagree'):
            module.validate_profile(hp_profile(wavelength_m=module.paired.C/527000000))

    def test_invalid_profile_values(self):
        bad = [('schema', 'old'), ('birth_snr_db', True), ('birth_snr_db', 101),
               ('base_cpi_seconds', 0), ('base_cpi_seconds', math.inf),
               ('frequency_hz', 0), ('frequency_hz', '509000000'),
               ('frequency_hz', math.nan), ('wavelength_m', -1),
               ('confirmation_m', 0), ('confirmation_m', 7),
               ('confirmation_n', 256), ('confirmation_n', 6.0),
               ('delete_after', False), ('delete_after', 0),
               ('max_acc', -1), ('max_acc', math.nan),
               ('adaptive_gate', False), ('adaptive_gate', 1),
               ('association_maximum_nis', 0), ('association_maximum_nis', math.nan),
               ('birth_delay_half_width_bins', 0),
               ('birth_doppler_half_width_bins', math.inf)]
        for key, value in bad:
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                module.validate_profile(hp_profile(**{key: value}))

    def test_unknown_truth_or_typo_fields_rejected(self):
        for key in ('birth_snr', 'target_delay', 'truth', 'adsb'):
            with self.subTest(key=key), self.assertRaises(ValueError):
                module.validate_profile(hp_profile(**{key: 5}))

    def test_exact_bytes_hash_and_duplicate_keys(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'profile.json'
            data = json.dumps(hp_profile()).encode()
            path.write_bytes(data)
            digest = hashlib.sha256(data).hexdigest()
            self.assertEqual(module.load_profile(path, digest)['birth_snr_db'], 5)
            for expected in (None, '', '0'*64, 'not-a-hash'):
                with self.subTest(expected=expected), self.assertRaises(ValueError):
                    module.load_profile(path, expected)
            path.write_bytes(data+b'\n')
            with self.assertRaisesRegex(ValueError, 'mismatch'):
                module.load_profile(path, digest)
            duplicated = data[:-1]+b', "birth_snr_db": 7}'
            path.write_bytes(duplicated)
            with self.assertRaisesRegex(ValueError, 'duplicate'):
                module.load_profile(path, hashlib.sha256(duplicated).hexdigest())

    def test_cli_requires_both_profile_and_hash(self):
        common = ['missing-ledger', '--tracker', 'unused', '--output-directory', 'unused']
        for extra in ([], ['--profile', 'missing'], ['--profile-sha256', '0'*64]):
            with self.subTest(extra=extra), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as result:
                    module.main(common+extra)
                self.assertEqual(result.exception.code, 2)


class TrackerPolicyPropagation(unittest.TestCase):
    def test_hp_values_and_full_raw_candidates_preserved(self):
        original = record()
        before = copy.deepcopy(original)
        row = module.tracker_inputs(module.endpoints([original]), hp_profile())[0]
        self.assertEqual(original, before)
        self.assertEqual(row['birth_snr_db'], 5)
        self.assertEqual(row['wavelength_m'], module.paired.C/509000000)
        self.assertEqual(row['range_resolution_m'], module.paired.C/2400000)
        self.assertEqual(row['base_cpi'], .5)
        self.assertEqual((row['m'], row['n'], row['delete_after']), (4, 6, 3))
        self.assertEqual(row['points'], [[20, 40, 7], [20, 40, 6]])

    def test_all_explicit_overrides_propagate(self):
        profile = hp_profile(birth_snr_db=8.5, frequency_hz=600000000,
                             base_cpi_seconds=.25, confirmation_m=2,
                             confirmation_n=5, delete_after=7, max_acc=12,
                             association_maximum_nis=2.5,
                             birth_delay_half_width_bins=.75,
                             birth_doppler_half_width_bins=1.25)
        row = module.tracker_inputs(module.endpoints([record(profile)]), profile)[0]
        self.assertEqual((row['m'], row['n'], row['delete_after']), (2, 5, 7))
        self.assertEqual((row['birth_snr_db'], row['base_cpi'], row['max_acc']), (8.5, .25, 12))
        self.assertEqual(row['wavelength_m'], module.paired.C/600000000)
        arms = module.matrix_arms(profile)
        self.assertEqual(arms['current_circle']['association_maximum_nis'], 2.5)
        exclusive = arms['current_circle_birth_exclusive']
        self.assertEqual(exclusive['association_maximum_nis'], 2.5)
        self.assertEqual(exclusive['birth_delay_half_width_bins'], .75)
        self.assertEqual(exclusive['birth_doppler_half_width_bins'], 1.25)
        self.assertEqual(arms['equal_area_circle']['association_maximum_nis'], 4/math.pi)

    def test_historical_homeportal_policy_requires_explicit_values(self):
        profile = hp_profile(birth_snr_db=7, frequency_hz=527000000,
                             base_cpi_seconds=.49995296223958335)
        row = module.tracker_inputs(module.endpoints([record(profile)]), profile)[0]
        self.assertEqual(row['birth_snr_db'], 7)
        self.assertEqual(row['wavelength_m'], module.paired.C/527000000)
        self.assertEqual(row['base_cpi'], profile['base_cpi_seconds'])
        self.assertEqual(module.matrix_arms(profile)['current_circle']['association_maximum_nis'],
                         9.210340371976184)

    def test_policy_or_cadence_drift_rejected_before_any_arm(self):
        for key, value in [('confirmation_m', 3), ('confirmation_n', 7),
                           ('delete_after', 4), ('base_cpi_seconds', .4),
                           ('update_interval_seconds', .5)]:
            with self.subTest(key=key), tempfile.TemporaryDirectory() as directory:
                first, second = record(), record(sequence=8)
                second['windows'][0][key] = value
                with patch.object(module.subprocess, 'run') as run:
                    with self.assertRaisesRegex(ValueError, 'mismatch'):
                        module.infer(module.endpoints([first, second]), 'unused',
                                     Path(directory), hp_profile())
                    run.assert_not_called()
                    self.assertEqual(list(Path(directory).iterdir()), [])

    def test_missing_ledger_policy_and_invalid_geometry_rejected(self):
        for key in ('confirmation_m', 'confirmation_n', 'delete_after',
                    'base_cpi_seconds', 'update_interval_seconds'):
            with self.subTest(key=key), self.assertRaises(ValueError):
                item = record()
                del item['windows'][0][key]
                module.tracker_inputs(module.endpoints([item]), hp_profile())
        for key, value in [('frequency_hz', 527000000), ('wavelength_m', 1),
                           ('delay_bin_sample_rate_hz', 0)]:
            with self.subTest(key=key), self.assertRaises(ValueError):
                item = dict(record(), **{key: value})
                module.tracker_inputs(module.endpoints([item]), hp_profile())

    def test_generation_reset_serial_and_gaps_reset_every_arm(self):
        records = [record(), record(sequence=8), record(sequence=12, reset_serial=1),
                   record(sequence=20, reset_serial=1),
                   record(sequence=24, generation='2', reset_serial=1)]
        rows = module.tracker_inputs(module.endpoints(records), hp_profile())
        self.assertEqual([r['reset'] for r in rows], [True, False, True, True, True])

    def test_same_inputs_all_arms_and_truth_only_after_inference(self):
        flat = module.endpoints([record(), record(sequence=8)])
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            with patch.object(module.subprocess, 'run', side_effect=empty_run) as run:
                def score(*args):
                    self.assertEqual(run.call_count, 4)
                    return {}, []
                with patch.object(module.base, 'tracker_metrics', side_effect=score):
                    reports = module.infer(flat, 'unused', output, hp_profile())
            self.assertEqual(len(reports), 4)
            common = module.tracker_inputs(flat, hp_profile())
            for name in reports:
                inputs = [json.loads(line) for line in
                          (output/f'{name}-truth-free-input.jsonl').read_text().splitlines()]
                self.assertEqual([{k: row[k] for k in common[0]} for row in inputs], common)
                self.assertNotIn('truth', json.dumps(inputs))

    def test_main_records_pinned_resolved_profile_and_binary(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            profile = root/'profile.json'
            profile.write_text(json.dumps(hp_profile()))
            digest = module.paired.digest(profile)
            ledger, tracker, output = root/'ledger.jsonl', root/'tracker', root/'output'
            ledger.write_text(json.dumps(record())+'\n')
            tracker.write_bytes(b'fake executable, never run')
            with patch.object(module.subprocess, 'run', side_effect=empty_run) as run:
                with contextlib.redirect_stdout(io.StringIO()):
                    module.main([str(ledger), '--tracker', str(tracker),
                                 '--profile', str(profile), '--profile-sha256', digest,
                                 '--output-directory', str(output)])
            report = json.loads((output/'report.json').read_text())
            self.assertEqual(run.call_count, 4)
            self.assertEqual(report['schema'], 'vectorwarp-depth4-tracker-matrix-v2')
            self.assertEqual(report['profile']['sha256'], digest)
            self.assertEqual(report['profile']['resolved'], module.validate_profile(hp_profile()))
            self.assertEqual(report['tracker']['sha256'], module.paired.digest(tracker))
            self.assertEqual(report['source_sha256'], module.paired.digest(ledger))
            self.assertEqual(report['runner_sha256'], module.paired.digest(PATH))


if __name__ == '__main__':
    unittest.main()
