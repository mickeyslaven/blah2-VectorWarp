#!/usr/bin/env python3
"""Measure temporal evidence near radar-owned track states.

The track states are radar output, not truth.  This analysis asks only whether
an unpromoted temporal lane could sustain an already-existing track when the
native detector has no nearby candidate.  Time-shifted track states provide a
negative control for persistent clutter and chance proximity.
"""

import argparse
import json
import math
from collections import defaultdict


STATES = ('ASSOCIATED', 'ACTIVE', 'COASTING')
LIGHT_SPEED_M_S = 299792458.0


def close(point, track, delay_gate, doppler_gate):
    return (abs(point[0] - track['delay']) <= delay_gate and
            abs(point[1] - track['doppler']) <= doppler_gate)


def points(items):
    return [(item['delay_bins'], item['doppler_hz']) for item in items
            if isinstance(item.get('delay_bins'), (int, float)) and
            isinstance(item.get('doppler_hz'), (int, float))]


def native_points(frame, sample_rate):
    delay, doppler = frame.get('delay', []), frame.get('doppler', [])
    if len(delay) != len(doppler):
        return []
    bin_km = LIGHT_SPEED_M_S / (sample_rate * 1000.0)
    return [(value / bin_km, doppler[index])
            for index, value in enumerate(delay)]


def tracks(frame):
    return [item for item in frame.get('data', [])
            if item.get('state') in STATES and
            isinstance(item.get('delay'), (int, float)) and
            isinstance(item.get('doppler'), (int, float))]


def rate(numerator, denominator):
    return numerator / denominator if denominator else None


def analyze(records, args):
    series = defaultdict(list)
    for record in records:
        sample_rate = record.get('output_sample_rate_hz') or args.sample_rate_hz
        if not isinstance(sample_rate, (int, float)) or sample_rate <= 0:
            raise ValueError('output sample rate is required for delay conversion')
        for window in record.get('windows', []):
            depth = int(window.get('depth', 0))
            if depth in (2, 4, 8):
                series[depth].append({
                    'candidates': points(window.get('latest_candidates', [])),
                    'native': native_points(
                        record.get('native_detections', {}), sample_rate),
                    'tracks': tracks(record.get('native_tracks', {})),
                })
    result = {
        'schema': 'vectorwarp-live-temporal-track-support-v1',
        'radar_tracks_only': True,
        'truth_used': False,
        'promotion_authorized': False,
        'gates': {'delay_bins': args.delay_gate,
                  'doppler_hz': args.doppler_gate,
                  'control_shifts': args.control_shift},
        'depths': {},
    }
    for depth, frames in sorted(series.items()):
        lane = defaultdict(int)
        by_state = {state: defaultdict(int) for state in STATES}
        for index, frame in enumerate(frames):
            for track in frame['tracks']:
                state = by_state[track['state']]
                native = any(close(point, track, args.delay_gate,
                                   args.doppler_gate)
                             for point in frame['native'])
                temporal = any(close(point, track, args.delay_gate,
                                     args.doppler_gate)
                               for point in frame['candidates'])
                for counts in (lane, state):
                    counts['opportunities'] += 1
                    counts['native_support'] += native
                    counts['temporal_support'] += temporal
                    counts['temporal_only_support'] += temporal and not native
            for shift in args.control_shift:
                control = frames[(index + shift) % len(frames)]['tracks']
                for track in control:
                    lane[f'control_{shift}_opportunities'] += 1
                    lane[f'control_{shift}_support'] += any(close(
                        point, track, args.delay_gate, args.doppler_gate)
                        for point in frame['candidates'])
        def finish(counts):
            value = dict(counts)
            value['native_support_rate'] = rate(
                value.get('native_support', 0), value.get('opportunities', 0))
            value['temporal_support_rate'] = rate(
                value.get('temporal_support', 0), value.get('opportunities', 0))
            value['temporal_only_support_rate'] = rate(
                value.get('temporal_only_support', 0),
                value.get('opportunities', 0))
            return value
        output = finish(lane)
        output['states'] = {state: finish(counts)
                            for state, counts in by_state.items()}
        output['controls'] = {
            str(shift): {
                'opportunities': lane.get(f'control_{shift}_opportunities', 0),
                'support': lane.get(f'control_{shift}_support', 0),
                'support_rate': rate(
                    lane.get(f'control_{shift}_support', 0),
                    lane.get(f'control_{shift}_opportunities', 0)),
            } for shift in args.control_shift
        }
        result['depths'][str(depth)] = output
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('ledger')
    parser.add_argument('--delay-gate', type=float, default=2)
    parser.add_argument('--doppler-gate', type=float, default=5)
    parser.add_argument('--control-shift', type=int, action='append',
                        default=[])
    parser.add_argument('--sample-rate-hz', type=float, default=0,
                        help='fallback lane output rate for legacy ledgers')
    args = parser.parse_args()
    if args.delay_gate <= 0 or args.doppler_gate <= 0:
        parser.error('gates must be positive')
    if not args.control_shift:
        args.control_shift = [7, 17, 31]
    if any(shift <= 0 for shift in args.control_shift):
        parser.error('control shifts must be positive')
    with open(args.ledger) as stream:
        records = [json.loads(line) for line in stream if line.strip()]
    if not records or any(shift >= len(records) for shift in args.control_shift):
        parser.error('ledger is too short for requested controls')
    print(json.dumps(analyze(records, args), indent=2, allow_nan=False))


if __name__ == '__main__':
    main()
