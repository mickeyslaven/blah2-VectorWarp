#!/usr/bin/env python3
"""Score live temporal candidates against held-out truth and a time-shift control."""

import argparse
import json
from collections import defaultdict


LIGHT_SPEED_M_S = 299792458.0


def points(frame):
    return list(zip(frame.get('delay', []), frame.get('doppler', [])))


def close(point, truth, delay_gate, doppler_gate):
    return (abs(point[0] - truth['delay']) <= delay_gate and
            abs(point[1] - truth['doppler']) <= doppler_gate)


def eligible_truth(record, args, minimum_delay, maximum_delay):
    result = []
    for value in record.get('truth_delay_doppler', {}).values():
        if not isinstance(value, dict):
            continue
        delay, doppler = value.get('delay'), value.get('doppler')
        if not isinstance(delay, (int, float)) or not isinstance(doppler, (int, float)):
            continue
        if (minimum_delay <= delay <= maximum_delay and
                args.minimum_doppler <= abs(doppler) <= args.maximum_doppler):
            result.append(value)
    return result


def qualification(records):
    """Describe whether the ledger can support a promotion decision.

    This deliberately fails closed. A useful diagnostic ledger may still be
    unqualified; that is different from evidence that the challenger lost.
    """
    reasons = []
    identities = set()
    keys = set()
    duplicate_keys = 0
    truncated_windows = 0
    generations = set()
    for record in records:
        provenance = record.get('provenance') or {}
        binary = (provenance.get('processor_binary') or {}).get('sha256')
        config = (provenance.get('config_file') or {}).get('sha256')
        revision = provenance.get('loaded_revision')
        if not all(isinstance(value, str) and value
                   for value in (binary, config, revision)):
            reasons.append('missing_immutable_runtime_provenance')
        else:
            identities.add((binary, config, revision))
        generation = str(record.get('generation', ''))
        generations.add(generation)
        for window in record.get('windows', []):
            key = (generation, int(window.get('depth', 0)),
                   str(window.get('latest_evaluated_sequence', '0')))
            if key in keys:
                duplicate_keys += 1
            keys.add(key)
            if int(window.get('truncated_candidates', 0) or 0) > 0:
                truncated_windows += 1
            latest_count = window.get('latest_candidate_count')
            if (not isinstance(latest_count, int) or latest_count < 0 or
                    latest_count > len(window.get('latest_candidates', []))):
                truncated_windows += 1
    if len(identities) > 1:
        reasons.append('mixed_runtime_provenance')
    if duplicate_keys:
        reasons.append('duplicate_evaluation_windows')
    if truncated_windows:
        reasons.append('candidate_export_truncated')
    if len(generations) > 1:
        reasons.append('multiple_observer_generations')
    # The current score statistic declares this itself. Until a frozen
    # empirical null exists, nominal CFAR rates are not promotion evidence.
    if any(record.get('null_calibrated') is not True for record in records):
        reasons.append('score_null_not_calibrated')
    reasons.append('time_shift_is_not_independent_false_track_control')
    return {
        'valid_for_diagnostic_scoring': bool(records),
        'valid_for_promotion': False,
        'invalid_reasons': sorted(set(reasons)),
        'runtime_identities': len(identities),
        'observer_generations': len(generations),
        'duplicate_windows': duplicate_keys,
        'truncated_windows': truncated_windows,
    }


def analyze(records, args):
    by_depth = defaultdict(lambda: defaultdict(int))
    if len(records) <= args.control_shift:
        raise ValueError('not enough records for requested negative-control shift')
    truth = []
    maximum_delays = []
    for record in records:
        extent = record.get('search_extent') or {}
        minimum_delay = extent.get('delay_min_km')
        if minimum_delay is None:
            minimum_delay = args.minimum_delay
        maximum_delay = extent.get('delay_max_km') or args.maximum_delay
        if (not isinstance(minimum_delay, (int, float)) or minimum_delay < 0 or
                not isinstance(maximum_delay, (int, float)) or
                maximum_delay <= minimum_delay):
            raise ValueError('searchable delay limits are required; unbounded truth denominators are invalid')
        maximum_delays.append(maximum_delay)
        truth.append(eligible_truth(record, args, minimum_delay, maximum_delay))
    for index, record in enumerate(records):
        native = points(record.get('native_detections', {}))
        sample_rate = (record.get('delay_bin_sample_rate_hz') or
                       record.get('output_sample_rate_hz') or
                       args.sample_rate_hz)
        if not isinstance(sample_rate, (int, float)) or sample_rate <= 0:
            raise ValueError('delay-bin sample rate is required')
        bin_km = LIGHT_SPEED_M_S / (sample_rate * 1000.0)
        control = truth[(index + args.control_shift) % len(records)]
        for window in record.get('windows', []):
            depth = int(window.get('depth', 0))
            if depth not in (2, 4, 8):
                continue
            candidates = [(item['delay_bins'] * bin_km, item['doppler_hz'])
                          for item in window.get('latest_candidates', [])
                          if isinstance(item.get('delay_bins'), (int, float)) and
                          isinstance(item.get('doppler_hz'), (int, float))]
            lane = by_depth[depth]
            lane['windows'] += 1
            lane['exported_candidates'] += len(candidates)
            for item in truth[index]:
                lane['truth_opportunities'] += 1
                native_hit = any(close(point, item, args.delay_gate,
                                       args.doppler_gate) for point in native)
                shadow_hit = any(close(point, item, args.delay_gate,
                                       args.doppler_gate) for point in candidates)
                lane['native_hits'] += native_hit
                lane['shadow_hits'] += shadow_hit
                lane['shadow_only_hits'] += shadow_hit and not native_hit
            for item in control:
                lane['control_opportunities'] += 1
                lane['time_shift_control_hits'] += any(close(
                    point, item, args.delay_gate, args.doppler_gate)
                    for point in candidates)
    result = {'schema': 'vectorwarp-live-temporal-pair-analysis-v1',
              'records': len(records), 'null_calibrated': False,
              'promotion_authorized': False,
              'qualification': qualification(records),
              'gates': {'delay_km': args.delay_gate,
                        'doppler_hz': args.doppler_gate,
                        'control_shift_records': args.control_shift,
                        'delay_extent_authority': 'ledger_search_extent_or_explicit_cli',
                        'delay_bin_scale': 'lane_output_sample_rate_hz'},
              'depths': {}}
    for depth, counts in sorted(by_depth.items()):
        counts = dict(counts)
        opportunities = counts.get('truth_opportunities', 0)
        controls = counts.get('control_opportunities', 0)
        counts['shadow_hit_rate'] = (counts.get('shadow_hits', 0) / opportunities
                                     if opportunities else None)
        counts['native_hit_rate'] = (counts.get('native_hits', 0) / opportunities
                                     if opportunities else None)
        counts['control_hit_rate'] = (counts.get('time_shift_control_hits', 0) / controls
                                      if controls else None)
        result['depths'][str(depth)] = counts
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('ledger')
    parser.add_argument('--delay-gate', type=float, default=.25,
                        help='delay gate in kilometers')
    parser.add_argument('--doppler-gate', type=float, default=10)
    parser.add_argument('--control-shift', type=int, default=17)
    parser.add_argument('--sample-rate-hz', type=float, default=0,
                        help='explicit delay-bin rate for legacy ledgers')
    parser.add_argument('--minimum-delay', type=float, default=-1,
                        help='explicit searchable minimum delay for legacy ledgers')
    parser.add_argument('--maximum-delay', type=float, default=0,
                        help='explicit searchable delay limit for legacy ledgers')
    parser.add_argument('--minimum-doppler', type=float, default=15)
    parser.add_argument('--maximum-doppler', type=float, default=600)
    args = parser.parse_args()
    if min(args.delay_gate, args.doppler_gate, args.control_shift) <= 0:
        parser.error('gates and control shift must be positive')
    with open(args.ledger) as stream:
        records = [json.loads(line) for line in stream if line.strip()]
    print(json.dumps(analyze(records, args), indent=2, allow_nan=False))


if __name__ == '__main__':
    main()
