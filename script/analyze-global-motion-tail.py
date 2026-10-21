#!/usr/bin/env python3
"""Estimate the blind whole-map tail of the geometric-mean motion statistic."""
import argparse
import importlib.util
import json
import random
from pathlib import Path

HERE = Path(__file__).parent
spec = importlib.util.spec_from_file_location('robust', HERE / 'analyze-robust-motion-stats.py')
robust = importlib.util.module_from_spec(spec); spec.loader.exec_module(robust)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--replay', required=True, type=Path)
    parser.add_argument('--truth', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--samples', type=int, default=1000)
    parser.add_argument('--seed', type=int, default=20261021)
    parser.add_argument('--delay-bias-km', type=float, default=.02578255)
    parser.add_argument('--doppler-bias-hz', type=float, default=7.053549)
    args = parser.parse_args()
    frames = json.loads(args.replay.read_text())['arms']['coherent_beam_bank']['maps']
    truth = json.loads(args.truth.read_text().splitlines()[0])['truth']
    scores = robust.BASE.Scores(frames, 2, 6)
    times = [(index + 1) * .5 for index in range(len(frames))]
    velocities = [(d, f) for d in robust.BASE.values('-0.10:0.10:0.10')
                  for f in robust.BASE.values('-20:20:20')]
    accelerations = [(d, f) for d in robust.BASE.values('-0.02:0.02:0.02')
                     for f in robust.BASE.values('-4:4:4')]
    def score(anchor):
        candidates = robust.paths(scores, anchor, times, velocities, accelerations, 15)
        return max(item[0]['geometric_mean_db'] for item in candidates) if candidates else None
    target_anchors = {ident: (item['delay'] + args.delay_bias_km,
                              item['doppler'] + args.doppler_bias_hz)
                      for ident, item in truth.items()}
    targets = {ident: score(anchor) for ident, anchor in target_anchors.items()}
    rng = random.Random(args.seed)
    delay, doppler = frames[0]['delay'], frames[0]['doppler']
    sampled = []
    attempts = 0
    while len(sampled) < args.samples and attempts < args.samples * 20:
        attempts += 1
        anchor = (rng.uniform(delay[16], delay[-17]), rng.uniform(doppler[16], doppler[-17]))
        if abs(anchor[1]) < 15:
            continue
        value = score(anchor)
        if value is not None:
            sampled.append((anchor, value))
    ordered = sorted(value for _, value in sampled)
    result = {'schema': 'vectorwarp-global-motion-tail-v1', 'samples': len(sampled),
              'seed': args.seed, 'targets': {},
              'sample_p99_db': ordered[min(len(ordered)-1, int(.99*len(ordered)))],
              'sample_max_db': ordered[-1]}
    for ident, value in targets.items():
        anchor = target_anchors[ident]
        delay_step = frames[0]['delay'][1] - frames[0]['delay'][0]
        doppler_step = frames[0]['doppler'][1] - frames[0]['doppler'][0]
        neighbors = [score((anchor[0] + dc * delay_step, anchor[1] + dr * doppler_step))
                     for dr in range(-2, 3) for dc in range(-2, 3) if dr or dc]
        neighbors = [item for item in neighbors if item is not None]
        exceeding_local_maxima = 0
        for control_anchor, control in sampled:
            if control < value:
                continue
            control_neighbors = [score((control_anchor[0] + dc * delay_step,
                                        control_anchor[1] + dr * doppler_step))
                                 for dr in range(-2, 3) for dc in range(-2, 3) if dr or dc]
            if all(item is None or item < control for item in control_neighbors):
                exceeding_local_maxima += 1
        result['targets'][ident] = {'score_db': value,
            'sample_exceedances': sum(control >= value for _, control in sampled),
            'empirical_percentile': sum(control < value for _, control in sampled) / len(sampled),
            'sample_exceeding_local_maxima': exceeding_local_maxima,
            'local_neighbor_exceedances': sum(control >= value for control in neighbors),
            'local_neighbor_max_db': max(neighbors)}
    args.output.write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
