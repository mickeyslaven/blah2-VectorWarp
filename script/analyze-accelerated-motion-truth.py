#!/usr/bin/env python3
"""Offline, ADS-B-gated quadratic-path energy integration for replay maps.

This deliberately remains an analysis candidate: it cannot publish radar plots
or tracks.  It searches a small, declared acceleration bank around the
trigger-time ADS-B delay/Doppler anchor, and applies the identical bank at
nearby controls.  A candidate is therefore only interesting when it exceeds
the empirical control tail; it is never a live truth-assisted detector.
"""

import argparse
import json
import math
from pathlib import Path


def values(spec):
    start, stop, step = (float(value) for value in spec.split(':'))
    if step <= 0 or stop < start:
        raise ValueError('ranges must be start:stop:positive-step')
    return [start + index * step for index in range(int(round((stop - start) / step)) + 1)]


def nearest(axis, value):
    low, high = 0, len(axis) - 1
    while low < high:
        middle = (low + high) // 2
        if axis[middle] < value:
            low = middle + 1
        else:
            high = middle
    return low - 1 if low and abs(axis[low - 1] - value) <= abs(axis[low] - value) else low


class Scores:
    def __init__(self, frames, guard, train):
        self.frames, self.guard, self.train, self.cache = frames, guard, train, {}

    def at(self, frame_index, delay, doppler):
        frame = self.frames[frame_index]
        row, column = nearest(frame['doppler'], doppler), nearest(frame['delay'], delay)
        key = frame_index, row, column
        if key in self.cache:
            return self.cache[key]
        data = frame['data'][row]
        left = range(max(0, column - self.guard - self.train), max(0, column - self.guard))
        right = range(min(len(data), column + self.guard + 1), min(len(data), column + self.guard + self.train + 1))
        training = list(left) + list(right)
        # The exported maps hold 10*log10(amplitude); power is therefore /5.
        cut = 10 ** (data[column] / 5.0)
        noise = sum(10 ** (data[index] / 5.0) for index in training) / max(1, len(training))
        self.cache[key] = cut / max(noise, 1e-300)
        return self.cache[key]


def path_coordinates(anchor_delay, anchor_doppler, times, delay_velocity, doppler_velocity,
                     delay_acceleration, doppler_acceleration):
    return [(anchor_delay + delay_velocity * time + .5 * delay_acceleration * time * time,
             anchor_doppler + doppler_velocity * time + .5 * doppler_acceleration * time * time)
            for time in times]


def valid(frames, coordinates, min_doppler):
    delay, doppler = frames[0]['delay'], frames[0]['doppler']
    return all(delay[0] <= item_delay <= delay[-1] and doppler[0] <= item_doppler <= doppler[-1] and
               abs(item_doppler) >= min_doppler for item_delay, item_doppler in coordinates)


def best_path(scores, anchor, times, velocities, accelerations, min_doppler):
    best = None
    for delay_velocity, doppler_velocity in velocities:
        for delay_acceleration, doppler_acceleration in accelerations:
            coordinates = path_coordinates(anchor[0], anchor[1], times, delay_velocity, doppler_velocity,
                                           delay_acceleration, doppler_acceleration)
            if not valid(scores.frames, coordinates, min_doppler):
                continue
            samples = [scores.at(index, *coordinate) for index, coordinate in enumerate(coordinates)]
            candidate = (sum(samples) / len(samples), delay_velocity, doppler_velocity,
                         delay_acceleration, doppler_acceleration, samples)
            if best is None or candidate[0] > best[0]:
                best = candidate
    return best


def controls(frames, anchor):
    delay_axis, doppler_axis = frames[0]['delay'], frames[0]['doppler']
    offsets = [(delay, doppler) for delay in (-12, -8, -5, -3, 3, 5, 8, 12)
               for doppler in (-320, -240, -160, -100, -60, 60, 100, 160, 240, 320)]
    return [(anchor[0] + delay, anchor[1] + doppler) for delay, doppler in offsets
            if delay_axis[0] <= anchor[0] + delay <= delay_axis[-1] and
            doppler_axis[0] <= anchor[1] + doppler <= doppler_axis[-1]]


def percentile99(items):
    return sorted(items)[min(len(items) - 1, int(.99 * len(items)))] if items else None


def analyze(frames, truth, cpi, delay_velocity, doppler_velocity, delay_acceleration,
            doppler_acceleration, guard=2, train=6, min_doppler=15,
            delay_bias=0, doppler_bias=0):
    scores = Scores(frames, guard, train)
    times = [(index + 1) * cpi for index in range(len(frames))]
    velocities = [(delay, doppler) for delay in values(delay_velocity) for doppler in values(doppler_velocity)]
    accelerations = [(delay, doppler) for delay in values(delay_acceleration)
                     for doppler in values(doppler_acceleration)]
    result = {}
    for ident, item in truth.items():
        anchor = item['delay'] + delay_bias, item['doppler'] + doppler_bias
        best = best_path(scores, anchor, times, velocities, accelerations, min_doppler)
        if best is None:
            raise ValueError(f'no valid path for {ident}')
        background = [best_path(scores, control, times, velocities, accelerations, min_doppler)
                      for control in controls(frames, anchor)]
        background = [candidate[0] for candidate in background if candidate]
        p99 = percentile99(background)
        score_db = 10 * math.log10(max(best[0], 1e-300))
        p99_db = 10 * math.log10(max(p99, 1e-300)) if p99 is not None else None
        result[ident] = {'flight': item.get('flight', ''), 'frames': len(frames),
            'integrated_local_ratio_db': score_db,
            'control_paths': len(background), 'control_p99_local_ratio_db': p99_db,
            'margin_over_control_p99_db': score_db - p99_db if p99_db is not None else None,
            'best_delay_velocity_km_s': best[1], 'best_doppler_velocity_hz_s': best[2],
            'best_delay_acceleration_km_s2': best[3],
            'best_doppler_acceleration_hz_s2': best[4],
            'per_frame_local_ratio_db': [10 * math.log10(max(value, 1e-300)) for value in best[5]],
            'candidate_only': True}
    return {'schema': 'vectorwarp-accelerated-motion-analysis-v1', 'frames': len(frames),
        'cpi_seconds': cpi, 'search': {'delay_velocity_km_s': delay_velocity,
          'doppler_velocity_hz_s': doppler_velocity,
          'delay_acceleration_km_s2': delay_acceleration,
          'doppler_acceleration_hz_s2': doppler_acceleration,
          'path_trials': len(velocities) * len(accelerations), 'guard': guard, 'train': train},
        'bias': {'delay_km': delay_bias, 'doppler_hz': doppler_bias},
        'aircraft': result}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--replay', required=True, type=Path)
    parser.add_argument('--truth', required=True, type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--arm', default='coherent_beam_bank')
    parser.add_argument('--cpi', type=float, default=.5)
    parser.add_argument('--delay-velocity', default='-0.10:0.10:0.10')
    parser.add_argument('--doppler-velocity', default='-20:20:20')
    parser.add_argument('--delay-acceleration', default='-0.02:0.02:0.02')
    parser.add_argument('--doppler-acceleration', default='-4:4:4')
    parser.add_argument('--guard', type=int, default=2); parser.add_argument('--train', type=int, default=6)
    parser.add_argument('--min-doppler', type=float, default=15)
    parser.add_argument('--delay-bias-km', type=float, default=0)
    parser.add_argument('--doppler-bias-hz', type=float, default=0)
    args = parser.parse_args()
    replay = json.loads(args.replay.read_text())
    truth_document = json.loads(args.truth.read_text().splitlines()[0])
    output = analyze(replay['arms'][args.arm]['maps'], truth_document['truth'], args.cpi,
        args.delay_velocity, args.doppler_velocity, args.delay_acceleration,
        args.doppler_acceleration, args.guard, args.train, args.min_doppler,
        args.delay_bias_km, args.doppler_bias_hz)
    output['replay'] = str(args.replay); output['truth'] = str(args.truth)
    encoded = json.dumps(output, indent=2) + '\n'
    if args.output: args.output.write_text(encoded)
    print(encoded, end='')


if __name__ == '__main__':
    main()
