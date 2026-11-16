#!/usr/bin/env python3
"""Certify declared whole-pass intervals against a temporal shadow ledger."""
import argparse
from collections import defaultdict
import json
from pathlib import Path


def validate(records, manifest):
    indexed = defaultdict(dict)
    for record in records:
        identity = (str(record.get('generation', '')), int(record.get('reset_serial', 0)))
        for window in record.get('windows', []):
            indexed[(identity, int(window['depth']))][int(window['latest_evaluated_sequence'])] = (record, window)
    certified, rejected = [], []
    seen_passes = {}
    declared_intervals = []
    for declared in manifest['passes']:
        pass_id = declared['pass_id']
        split = declared['split']
        if pass_id in seen_passes and seen_passes[pass_id] != split:
            raise ValueError('whole-pass split leakage')
        seen_passes[pass_id] = split
        depth = int(declared.get('depth', 4))
        first, last = int(declared['first_sequence']), int(declared['last_sequence'])
        identity = (str(declared['generation']), int(declared['reset_serial']))
        if first > last or first <= 0:
            raise ValueError('invalid whole-pass sequence interval')
        for prior_identity, prior_depth, prior_first, prior_last, prior_pass in declared_intervals:
            if (identity == prior_identity and
                    max(first, prior_first) <= min(last, prior_last) and
                    prior_pass != pass_id):
                raise ValueError('aliased or overlapping source interval across pass identities')
        declared_intervals.append((identity, depth, first, last, pass_id))
        expected = list(range(first, last+1, depth))
        rows = indexed.get((identity, depth), {})
        reasons = []
        if any(sequence not in rows for sequence in expected):
            reasons.append('endpoint_gap')
        selected = [rows[s] for s in expected if s in rows]
        if any(w.get('truncated_candidates', 0) or
               w.get('latest_candidate_count') != len(w.get('latest_candidates', []))
               for _, w in selected):
            reasons.append('candidate_export_incomplete')
        if len(expected)*depth < 80:
            reasons.append('shorter_than_80_base_cpi_floor')
        revisions = {(r.get('provenance', {}).get('loaded_revision'),
                      r.get('provenance', {}).get('processor_binary', {}).get('sha256'))
                     for r, _ in selected}
        if (len(revisions) != 1 or not selected or
                not all(revision and binary for revision, binary in revisions)):
            reasons.append('runtime_identity_changed_or_missing')
        result = dict(declared, endpoints=len(expected), base_cpis=len(expected)*depth)
        (rejected if reasons else certified).append(dict(result, reasons=reasons))
    split_pass_ids = {split: {p['pass_id'] for p in certified if p['split'] == split}
                      for split in ('calibration', 'evaluation', 'nuisance')}
    split_counts = {split: len(ids) for split, ids in split_pass_ids.items()}
    return {'schema': 'vectorwarp-temporal-whole-pass-validation-v1',
            'certified': certified, 'rejected': rejected,
            'certified_counts': split_counts,
            'effect_study_ready': split_counts['evaluation'] >= 30,
            'initial_target_passes': 30,
            'false_track_hours': manifest.get('adjudicated_nuisance_hours', 0),
            'false_track_rate_95pct_upper_if_zero':
                (3/manifest['adjudicated_nuisance_hours']
                 if manifest.get('adjudicated_nuisance_hours', 0) else None)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('ledger')
    parser.add_argument('manifest')
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    records = [json.loads(line) for line in Path(args.ledger).read_text().splitlines()]
    manifest = json.loads(Path(args.manifest).read_text())
    Path(args.output).write_text(json.dumps(validate(records, manifest), indent=2)+'\n')


if __name__ == '__main__':
    main()
