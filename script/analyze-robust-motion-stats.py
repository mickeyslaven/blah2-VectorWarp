#!/usr/bin/env python3
"""Compare blind temporal path statistics on an identical motion bank.

Truth anchors are used only to score the offline target/control experiment.
Every statistic is defined solely from a path's map samples, so deployment can
scan it over all anchors and the same velocity/acceleration bank without ADS-B.
"""
import argparse
import json
import math
from pathlib import Path

import importlib.util

SPEC = importlib.util.spec_from_file_location(
    'accelerated', Path(__file__).with_name('analyze-accelerated-motion-truth.py'))
BASE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BASE)


def db(value):
    return 10 * math.log10(max(value, 1e-300))


def quantile(items, fraction):
    ordered = sorted(items)
    position = (len(ordered) - 1) * fraction
    low, high = int(position), math.ceil(position)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def longest_run(items, threshold):
    run = best = 0
    for value in items:
        run = run + 1 if value >= threshold else 0
        best = max(best, run)
    return best


def metrics(samples):
    logs = [db(value) for value in samples]
    trimmed = sorted(logs)[len(logs) // 5: len(logs) - len(logs) // 5]
    # Scores with a count component receive a small amplitude tie-breaker.
    # This makes the bank search deterministic without changing count ranking.
    output = {
        'mean_power_db': db(sum(samples) / len(samples)),
        'geometric_mean_db': sum(logs) / len(logs),
        'median_db': quantile(logs, .5),
        'q25_db': quantile(logs, .25),
        'q75_db': quantile(logs, .75),
        'trimmed_mean_db': sum(trimmed) / len(trimmed),
        'signed_rank_db': sum((index + 1) * (1 if value >= 0 else -1)
                              for index, value in enumerate(sorted(logs, key=abs))) / len(logs),
    }
    for fraction in (.10, .15, .20, .25, .30, .35, .40):
        output[f'q{int(fraction * 100):02d}_db'] = quantile(logs, fraction)
    for fraction in (.10, .15, .20, .25, .30, .35, .40):
        width = int(len(logs) * fraction)
        retained = sorted(logs)[width:len(logs) - width]
        output[f'trim{int(fraction * 100):02d}_mean_db'] = sum(retained) / len(retained)
    for threshold in (0, 2, 4, 6):
        count = sum(value >= threshold for value in logs)
        output[f'count_ge_{threshold}_db'] = count + sum(max(0, value - threshold) for value in logs) / 1000
        output[f'run_ge_{threshold}_db'] = longest_run(logs, threshold) + sum(max(0, value - threshold) for value in logs) / 1000
    return output


def paths(scores, anchor, times, velocities, accelerations, min_doppler):
    output = []
    for delay_velocity, doppler_velocity in velocities:
        for delay_acceleration, doppler_acceleration in accelerations:
            coordinates = BASE.path_coordinates(anchor[0], anchor[1], times, delay_velocity, doppler_velocity,
                                                 delay_acceleration, doppler_acceleration)
            if BASE.valid(scores.frames, coordinates, min_doppler):
                samples = [scores.at(index, *coordinate) for index, coordinate in enumerate(coordinates)]
                output.append((metrics(samples), samples, (delay_velocity, doppler_velocity,
                               delay_acceleration, doppler_acceleration)))
    return output


def analyze(frames, truth, cpi, delay_velocity, doppler_velocity, delay_acceleration,
            doppler_acceleration, guard, train, min_doppler, delay_bias, doppler_bias):
    scores = BASE.Scores(frames, guard, train)
    times = [(index + 1) * cpi for index in range(len(frames))]
    velocities = [(delay, doppler) for delay in BASE.values(delay_velocity) for doppler in BASE.values(doppler_velocity)]
    accelerations = [(delay, doppler) for delay in BASE.values(delay_acceleration) for doppler in BASE.values(doppler_acceleration)]
    aircraft = {}
    for ident, item in truth.items():
        anchor = item['delay'] + delay_bias, item['doppler'] + doppler_bias
        target_paths = paths(scores, anchor, times, velocities, accelerations, min_doppler)
        control_paths = [paths(scores, control, times, velocities, accelerations, min_doppler)
                         for control in BASE.controls(frames, anchor)]
        names = target_paths[0][0].keys()
        report = {}
        for name in names:
            target = max(target_paths, key=lambda item: item[0][name])
            controls = [max(path_set, key=lambda item: item[0][name])[0][name]
                        for path_set in control_paths if path_set]
            control_p99 = BASE.percentile99(controls)
            report[name] = {
                'target': target[0][name], 'control_p99': control_p99,
                'margin': target[0][name] - control_p99,
                'best_motion': dict(zip(('delay_velocity_km_s', 'doppler_velocity_hz_s',
                                          'delay_acceleration_km_s2', 'doppler_acceleration_hz_s2'), target[2])),
                'target_per_frame_local_ratio_db': [db(value) for value in target[1]],
            }
        aircraft[ident] = {'flight': item.get('flight', ''), 'statistics': report}
    return {'schema': 'vectorwarp-robust-motion-statistics-v1', 'frames': len(frames),
            'cpi_seconds': cpi, 'bias': {'delay_km': delay_bias, 'doppler_hz': doppler_bias},
            'search': {'path_trials': len(velocities) * len(accelerations), 'guard': guard, 'train': train,
                       'min_doppler': min_doppler}, 'aircraft': aircraft}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--replay', required=True, type=Path); parser.add_argument('--truth', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path); parser.add_argument('--arm', default='coherent_beam_bank')
    parser.add_argument('--cpi', type=float, default=.5); parser.add_argument('--delay-velocity', default='-0.10:0.10:0.10')
    parser.add_argument('--doppler-velocity', default='-20:20:20'); parser.add_argument('--delay-acceleration', default='-0.02:0.02:0.02')
    parser.add_argument('--doppler-acceleration', default='-4:4:4'); parser.add_argument('--guard', type=int, default=2)
    parser.add_argument('--train', type=int, default=6); parser.add_argument('--min-doppler', type=float, default=15)
    parser.add_argument('--delay-bias-km', type=float, default=.02578255); parser.add_argument('--doppler-bias-hz', type=float, default=7.053549)
    args = parser.parse_args()
    replay = json.loads(args.replay.read_text())
    truth = json.loads(args.truth.read_text().splitlines()[0])['truth']
    result = analyze(replay['arms'][args.arm]['maps'], truth, args.cpi, args.delay_velocity, args.doppler_velocity,
                     args.delay_acceleration, args.doppler_acceleration, args.guard, args.train, args.min_doppler,
                     args.delay_bias_km, args.doppler_bias_hz)
    result.update({'replay': str(args.replay), 'truth': str(args.truth), 'candidate_only': True,
                   'deployment_note': 'Blind scan statistic: ADS-B/truth anchors are evaluation-only.'})
    args.output.write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
