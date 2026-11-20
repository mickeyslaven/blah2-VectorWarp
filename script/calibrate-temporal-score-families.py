#!/usr/bin/env python3
"""Freeze per-depth empirical score thresholds on disjoint nuisance passes."""
import argparse
from collections import defaultdict
import json
import math
from pathlib import Path


def calibrate(rows, tail_probability, minimum_samples):
    pass_splits = {}
    samples = defaultdict(lambda: defaultdict(list))
    for row in rows:
        required = ('pass_id', 'split', 'adjudication', 'score_family',
                    'depth', 'score_db')
        if any(key not in row for key in required):
            raise ValueError('calibration row missing required identity')
        if row['split'] not in ('calibration', 'holdout') or \
                row['adjudication'] != 'nuisance':
            raise ValueError('only adjudicated nuisance calibration/holdout rows are accepted')
        score = float(row['score_db'])
        depth = int(row['depth'])
        if depth not in (2, 4, 8) or not math.isfinite(score):
            raise ValueError('invalid depth or score')
        prior = pass_splits.setdefault(row['pass_id'], row['split'])
        if prior != row['split']:
            raise ValueError('whole-pass split leakage')
        samples[(row['score_family'], depth)][row['split']].append(score)
    result = {}
    for (family, depth), split in sorted(samples.items()):
        calibration = sorted(split['calibration'])
        holdout = split['holdout']
        if len(calibration) < minimum_samples or len(holdout) < minimum_samples:
            raise ValueError('insufficient independent calibration or holdout samples')
        # Freeze on calibration only. This is a candidate-score tail threshold,
        # not a cell PFA or false-track rate; the holdout measures transfer.
        rank = max(0, min(len(calibration)-1,
                         math.ceil((1-tail_probability)*len(calibration))-1))
        threshold = calibration[rank]
        result[f'{family}:depth{depth}'] = {
            'threshold_db': threshold,
            'calibration_samples': len(calibration),
            'holdout_samples': len(holdout),
            'requested_candidate_tail_probability': tail_probability,
            'holdout_exceedances': sum(value >= threshold for value in holdout),
            'holdout_exceedance_fraction': sum(value >= threshold for value in holdout)/len(holdout),
            'minimum_resolvable_calibration_tail': 1/(len(calibration)+1),
        }
    if not result:
        raise ValueError('no score strata')
    return {'schema': 'vectorwarp-temporal-score-calibration-v1',
            'pass_split_count': {split: sum(value == split for value in pass_splits.values())
                                 for split in ('calibration', 'holdout')},
            'strata': result,
            'scope': 'candidate-score calibration only; false-track acceptance requires separate adjudicated hours'}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('input_jsonl')
    parser.add_argument('--tail-probability', type=float, default=.01)
    parser.add_argument('--minimum-samples', type=int, default=32)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    if not 0 < args.tail_probability < 1 or args.minimum_samples < 2:
        raise ValueError('invalid calibration bounds')
    rows = [json.loads(line) for line in Path(args.input_jsonl).read_text().splitlines()]
    report = calibrate(rows, args.tail_probability, args.minimum_samples)
    Path(args.output).write_text(json.dumps(report, indent=2)+'\n')


if __name__ == '__main__':
    main()
