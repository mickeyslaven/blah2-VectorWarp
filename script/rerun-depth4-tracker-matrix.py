#!/usr/bin/env python3
"""Truth-blind depth-4 tracker-policy replay with an explicitly pinned profile.

--profile is a vectorwarp-depth4-tracker-profile-v1 JSON object, not a native
runtime config. Every field in PROFILE_FIELDS and frequency_hz or wavelength_m
must be supplied; --profile-sha256 pins its exact bytes. There are no site
defaults. Existing temporal accumulation is not a new HP detector statistic.
"""
import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re
import subprocess

BASE = Path(__file__).with_name('rerun-depth4-dedup.py')
SPEC = importlib.util.spec_from_file_location('depth4_base', BASE)
base = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(base)
paired = base.paired

PROFILE_SCHEMA = 'vectorwarp-depth4-tracker-profile-v1'
PROFILE_FIELDS = {
    'schema', 'birth_snr_db', 'base_cpi_seconds', 'confirmation_m',
    'confirmation_n', 'delete_after', 'max_acc', 'adaptive_gate',
    'association_maximum_nis', 'birth_delay_half_width_bins',
    'birth_doppler_half_width_bins',
}


def finite_number(value, name, positive=False):
    if (type(value) not in (int, float) or not math.isfinite(value) or
            (positive and value <= 0)):
        raise ValueError(f'invalid {name}')
    return value


def validate_profile(profile):
    if not isinstance(profile, dict):
        raise ValueError('profile must be an object')
    missing = PROFILE_FIELDS - profile.keys()
    unknown = profile.keys() - PROFILE_FIELDS - {'frequency_hz', 'wavelength_m'}
    if missing or unknown:
        raise ValueError(f'profile fields missing={sorted(missing)} unknown={sorted(unknown)}')
    if profile['schema'] != PROFILE_SCHEMA:
        raise ValueError('unsupported profile schema')
    result = dict(profile)
    for key in ('base_cpi_seconds', 'association_maximum_nis',
                'birth_delay_half_width_bins', 'birth_doppler_half_width_bins'):
        finite_number(result[key], key, positive=True)
    if not -100 <= finite_number(result['birth_snr_db'], 'birth_snr_db') <= 100:
        raise ValueError('birth_snr_db outside tracker bounds')
    if finite_number(result['max_acc'], 'max_acc') < 0:
        raise ValueError('max_acc must be nonnegative')
    for key in ('confirmation_m', 'confirmation_n', 'delete_after'):
        if type(result[key]) is not int or not 1 <= result[key] <= 255:
            raise ValueError(f'invalid {key}')
    if result['confirmation_m'] > result['confirmation_n']:
        raise ValueError('confirmation_m exceeds confirmation_n')
    # The current CPU adapter is adaptive-only; never silently accept a
    # non-adaptive profile and then execute a different gate implementation.
    if result['adaptive_gate'] is not True:
        raise ValueError('tracker adapter requires adaptive_gate=true')
    if not {'frequency_hz', 'wavelength_m'} & result.keys():
        raise ValueError('frequency_hz or wavelength_m is required')
    for key in ('frequency_hz', 'wavelength_m'):
        if key in result:
            finite_number(result[key], key, positive=True)
    if 'frequency_hz' in result:
        wavelength = paired.C / result['frequency_hz']
        finite_number(wavelength, 'derived wavelength_m', positive=True)
        if 'wavelength_m' in result and not math.isclose(
                result['wavelength_m'], wavelength, rel_tol=1e-12, abs_tol=0):
            raise ValueError('frequency_hz and wavelength_m disagree')
        result['wavelength_m'] = wavelength
    else:
        result['frequency_hz'] = paired.C / result['wavelength_m']
        finite_number(result['frequency_hz'], 'derived frequency_hz', positive=True)
    return result


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f'duplicate profile field: {key}')
        result[key] = value
    return result


def load_profile(path, expected_sha256):
    if not re.fullmatch('[0-9a-f]{64}', expected_sha256 or ''):
        raise ValueError('profile SHA-256 must be supplied as 64 lowercase hex digits')
    data = Path(path).read_bytes()
    actual = hashlib.sha256(data).hexdigest()
    if actual != expected_sha256:
        raise ValueError('profile SHA-256 mismatch')
    return validate_profile(json.loads(data, object_pairs_hook=unique_object))


def matrix_arms(profile):
    circle = {'legacy_rectangle': False,
              'association_maximum_nis': profile['association_maximum_nis'],
              'birth_exclusivity': False}
    return {
        'legacy_rectangle': dict(circle, legacy_rectangle=True),
        'equal_area_circle': dict(circle, association_maximum_nis=4/math.pi),
        'current_circle': dict(circle),
        'current_circle_birth_exclusive': dict(
            circle, birth_exclusivity=True,
            birth_delay_half_width_bins=profile['birth_delay_half_width_bins'],
            birth_doppler_half_width_bins=profile['birth_doppler_half_width_bins']),
    }


def endpoints(records):
    result, previous, episode = [], None, -1
    for record in records:
        window = next((w for w in record['windows'] if w['depth'] == 4), None)
        if not window:
            continue
        reset = (previous is None or record['generation'] != previous[0]['generation'] or
                 int(record.get('reset_serial', 0)) != int(previous[0].get('reset_serial', 0)) or
                 int(window['latest_evaluated_sequence']) -
                 int(previous[1]['latest_evaluated_sequence']) != 4)
        if reset:
            episode += 1
        result.append((episode, record, window))
        previous = (record, window)
    if not result:
        raise ValueError('no qualified depth4 endpoints')
    return result


def tracker_inputs(flat, profile):
    """Validate the entire cohort before writing inputs or starting any arm."""
    profile = validate_profile(profile)
    if not flat:
        raise ValueError('no qualified depth4 endpoints')
    rows, prior = [], None
    for index, (episode, record, window) in enumerate(flat):
        for key in ('confirmation_m', 'confirmation_n', 'delete_after'):
            if type(window.get(key)) is not int or window[key] != profile[key]:
                raise ValueError(f'endpoint {index}: profile/ledger {key} mismatch')
        for key, expected in (
                ('base_cpi_seconds', profile['base_cpi_seconds']),
                ('update_interval_seconds', 4*profile['base_cpi_seconds'])):
            actual = finite_number(window.get(key), key, positive=True)
            if not math.isclose(actual, expected, rel_tol=1e-9, abs_tol=0):
                raise ValueError(f'endpoint {index}: profile/ledger {key} mismatch')
        for key in ('frequency_hz', 'wavelength_m'):
            if key in record and not math.isclose(
                    finite_number(record[key], key, positive=True), profile[key],
                    rel_tol=1e-9, abs_tol=0):
                raise ValueError(f'endpoint {index}: profile/ledger {key} mismatch')
        rate = finite_number(record.get('delay_bin_sample_rate_hz'),
                             'delay_bin_sample_rate_hz', positive=True)
        row = {'index': index, 'reset': episode != prior,
               'timestamp_ms': record['timestamp_ms'],
               'points': [[p['delay_bins'], p['doppler_hz'], p['snr_db']]
                          for p in window['latest_candidates']],
               'm': profile['confirmation_m'], 'n': profile['confirmation_n'],
               'delete_after': profile['delete_after'],
               'base_cpi': profile['base_cpi_seconds'],
               'birth_snr_db': profile['birth_snr_db'],
               'max_acc': profile['max_acc'],
               'range_resolution_m': paired.C/rate,
               'wavelength_m': profile['wavelength_m']}
        rows.append(row)
        prior = episode
    return rows


def infer(flat, executable, output, profile):
    rows = tracker_inputs(flat, profile)
    reports, observations = {}, {}
    for name, policy in matrix_arms(profile).items():
        encoded = ''.join(json.dumps(dict(row, **policy), separators=(',', ':'),
                                    allow_nan=False)+'\n' for row in rows)
        (output/f'{name}-truth-free-input.jsonl').write_text(encoded)
        run = subprocess.run([executable], input=encoded, text=True,
                             capture_output=True, check=True, timeout=120)
        (output/f'{name}-tracker-output.jsonl').write_text(run.stdout)
        observed = [json.loads(line) for line in run.stdout.splitlines()]
        if len(observed) != len(flat):
            raise ValueError('tracker output count mismatch')
        observations[name] = observed
    # Complete every radar-only arm before evaluating any truth.
    for name, policy in matrix_arms(profile).items():
        observed = observations[name]
        metrics, _ = base.tracker_metrics(flat, observed)
        metrics['pair_in_gate'] = sum(row.get('pair_in_gate', 0) for row in observed)
        metrics['birth_exclusive_rejected'] = sum(
            row.get('birth_exclusive_rejected', 0) for row in observed)
        reports[name] = {'policy': policy, 'metrics': metrics}
    return reports


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument('ledger')
    parser.add_argument('--tracker', required=True)
    parser.add_argument('--profile', required=True,
                        help=f'Explicit {PROFILE_SCHEMA} JSON; no site defaults')
    parser.add_argument('--profile-sha256', required=True)
    parser.add_argument('--output-directory', required=True)
    args = parser.parse_args(argv)
    profile = load_profile(args.profile, args.profile_sha256)
    tracker_hash = paired.digest(args.tracker)
    output = Path(args.output_directory)
    if output.exists() and any(output.iterdir()):
        raise ValueError('output directory must be empty')
    source_data = Path(args.ledger).read_bytes()
    raw = [json.loads(line) for line in source_data.splitlines()]
    clean, quarantined = paired.clean_rows(raw)
    flat = endpoints(clean)
    tracker_inputs(flat, profile)
    output.mkdir(parents=True, exist_ok=True)
    report = {'schema': 'vectorwarp-depth4-tracker-matrix-v2',
              'source_sha256': hashlib.sha256(source_data).hexdigest(),
              'profile': {'path': str(Path(args.profile).resolve()),
                          'sha256': args.profile_sha256, 'resolved': profile},
              'tracker': {'path': str(Path(args.tracker).resolve()),
                          'sha256': tracker_hash},
              'runner_sha256': paired.digest(__file__),
              'qualified_records': len(clean), 'qualified_endpoints': len(flat),
              'quarantined_windows': quarantined,
              'arms': infer(flat, args.tracker, output, profile),
              'limitations': [
                  'Geometric equal area is not equal empirical nuisance burden.',
                  'No false-track rate is claimed without independent adjudicated nuisance exposure.',
                  'A pinned replay profile is not proof of native CPU/GPU parity.',
                  'Truth is used only after all four radar-only inference arms complete.']}
    (output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({'qualified_endpoints': len(flat),
                      'report': str(output/'report.json')}))


if __name__ == '__main__':
    main()
