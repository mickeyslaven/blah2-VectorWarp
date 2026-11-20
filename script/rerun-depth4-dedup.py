#!/usr/bin/env python3
"""Execute frozen radar-only depth4 suppression, then evaluate unseen truth."""
import argparse
from collections import Counter, defaultdict
import importlib.util
import json
from pathlib import Path
import statistics
import subprocess

SOURCE = Path(__file__).with_name('rerun-temporal-endpoints.py')
SPEC = importlib.util.spec_from_file_location('paired', SOURCE)
paired = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(paired)
PREREG_HASH = '97130baef3d264b4066372c9a32d3b838e19d1fdc240a0736bec02e4ec996bd2'


def suppress(candidates, base_cpi, arm):
    if arm['name'] == 'raw':
        return list(candidates)
    def priority(index):
        p = candidates[index]
        return (-p['snr_db'], p['delay_bins'], p['doppler_hz'], index)
    retained = []
    for i, p in enumerate(candidates):
        keep = True
        for j, q in enumerate(candidates):
            if priority(j) >= priority(i):
                continue
            dd = abs(p['delay_bins']-q['delay_bins'])
            df = abs(p['doppler_hz']-q['doppler_hz'])*base_cpi
            if arm['boundary'] == 'inclusive':
                neighbor = (dd <= arm['delay_half_width_native_bins']+1e-9 and
                            df <= arm['doppler_half_width_native_bins']+1e-9)
            else:
                neighbor = (dd < arm['delay_half_width_native_bins'] and
                            df < arm['doppler_half_width_native_bins'])
            if neighbor:
                keep = False
                break
        if keep:
            retained.append(p)
    return retained


def infer(records, arms, executable, output_dir):
    outputs, filtered, episodes = {}, {}, []
    flat, previous = [], None
    for record in records:
        window = next((w for w in record['windows'] if w['depth'] == 4), None)
        if not window:
            continue
        reset = (previous is None or record['generation'] != previous[0]['generation'] or
                 int(record.get('reset_serial', 0)) != int(previous[0].get('reset_serial', 0)) or
                 int(window['latest_evaluated_sequence'])-int(previous[1]['latest_evaluated_sequence']) != 4)
        if reset:
            episodes.append([])
        sid = len(episodes)-1
        flat.append((sid, record, window))
        episodes[-1].append((record, window))
        previous = (record, window)
    if not flat:
        raise ValueError('no new qualified depth4 endpoints')
    for arm in arms:
        name = arm['name']
        transformed, inputs, prev_sid = [], [], None
        for index, (sid, record, window) in enumerate(flat):
            candidates = suppress(window['latest_candidates'], window['base_cpi_seconds'], arm)
            transformed.append(candidates)
            inputs.append({'index': index, 'reset': sid != prev_sid,
                'timestamp_ms': record['timestamp_ms'],
                'points': [[p['delay_bins'], p['doppler_hz'], p['snr_db']] for p in candidates],
                'm': 4, 'n': 6, 'delete_after': 3, 'base_cpi': window['base_cpi_seconds'],
                'birth_snr_db': 7, 'max_acc': 10,
                'range_resolution_m': paired.C/record['delay_bin_sample_rate_hz'],
                'wavelength_m': paired.C/527000000})
            prev_sid = sid
        encoded = ''.join(json.dumps(r, separators=(',', ':'))+'\n' for r in inputs)
        (output_dir/f'{name}-truth-free-input.jsonl').write_text(encoded)
        run = subprocess.run([executable], input=encoded, capture_output=True, text=True,
                             check=True, timeout=60)
        (output_dir/f'{name}-tracker-output.jsonl').write_text(run.stdout)
        outputs[name] = [json.loads(line) for line in run.stdout.splitlines()]
        if len(outputs[name]) != len(flat):
            raise ValueError('tracker output count mismatch')
        filtered[name] = transformed
    # No evaluation truth has been read by suppression/inference above.
    return flat, episodes, filtered, outputs


def tracker_metrics(flat, output):
    counts, events, previous_truth, truth_episode = Counter(), defaultdict(list), {}, Counter()
    track_birth, confirmed, delays = {}, set(), []
    active_rows = []
    step = flat[0][2]['update_interval_seconds']
    for (sid, record, window), frame in zip(flat, output):
        truth = paired.truths(record)
        scale = paired.C/record['delay_bin_sample_rate_hz']/1000
        for key in ('births', 'associations', 'misses', 'promotions'):
            counts[key] += frame[key]
        active = [t for t in frame['tracks'] if t['state'] == 'ACTIVE']
        counts['windows_with_any_active_truth'] += any(
            paired.near((t['delay_bins']*scale,t['doppler_hz']),value)
            for t in active for value in truth.values())
        counts['active_track_endpoints'] += len(active)
        counts['coasting_track_endpoints'] += sum(t['state'] == 'COASTING' for t in frame['tracks'])
        counts['truth_opportunities'] += len(truth)
        for track in frame['tracks']:
            key = (sid, track['id'])
            track_birth.setdefault(key, record['timestamp_ms'])
            if track['state'] == 'ACTIVE' and key not in confirmed:
                confirmed.add(key)
                delays.append((record['timestamp_ms']-track_birth[key])/1000)
        for track in active:
            if not any(paired.near((track['delay_bins']*scale, track['doppler_hz']), v) for v in truth.values()):
                counts['unmatched_active_track_endpoints_unlabeled'] += 1
        for aircraft, value in truth.items():
            matched = {t['id'] for t in active if paired.near((t['delay_bins']*scale, t['doppler_hz']), value)}
            counts['active_truth_endpoints'] += bool(matched)
            counts['duplicate_active_truth_track_endpoints'] += max(0, len(matched)-1)
            key = (sid, aircraft)
            if key in previous_truth and record['timestamp_ms']-previous_truth[key] > step*1500:
                truth_episode[key] += 1
            previous_truth[key] = record['timestamp_ms']
            events[(sid, aircraft, truth_episode[key])].append((record['timestamp_ms'], matched))
            active_rows.append({'generation': record['generation'], 'aircraft': aircraft, 'hit': bool(matched)})
    fragments, continuous, first_delays = [], [], []
    for episode in events.values():
        last, segments, run, longest = set(), 0, 0, 0
        for timestamp, matched in episode:
            if matched:
                segments += not bool(last.intersection(matched))
                run += 1
                longest = max(longest, run)
            else:
                run = 0
            last = matched
        fragments.append(segments)
        continuous.append(longest*step)
        hits = [time for time, matched in episode if matched]
        if hits:
            first_delays.append((hits[0]-episode[0][0])/1000)
    counts['missed_eligible_truth_endpoints'] = counts['truth_opportunities']-counts['active_truth_endpoints']
    result = {key: counts[key] for key in ('births', 'associations', 'misses', 'promotions',
        'active_track_endpoints', 'coasting_track_endpoints', 'truth_opportunities',
        'active_truth_endpoints', 'duplicate_active_truth_track_endpoints',
        'unmatched_active_track_endpoints_unlabeled', 'missed_eligible_truth_endpoints',
        'windows_with_any_active_truth')}
    result.update(eligible_truth_fragments=len(events), matched_truth_fragments=sum(x > 0 for x in fragments),
        measured_truth_track_segments=sum(fragments), fragmentation_extra_segments=sum(max(0,x-1) for x in fragments),
        longest_measured_active_continuity_s=max(continuous, default=0),
        mean_fragment_longest_measured_active_continuity_s=statistics.mean(continuous) if continuous else None,
        seed_to_confirmation_median_s=statistics.median(delays) if delays else None,
        first_eligible_to_active_delays_s=first_delays,
        false_tracks_per_hour=None, false_track_reason='unmatched is unlabeled; no adjudicated nuisance exposure')
    return result, active_rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--snapshot', required=True)
    parser.add_argument('--preregistration', required=True)
    parser.add_argument('--tracker', required=True)
    parser.add_argument('--output-directory', required=True)
    args = parser.parse_args()
    if paired.digest(args.preregistration) != PREREG_HASH:
        raise ValueError('preregistration changed')
    spec = json.loads(Path(args.preregistration).read_text())
    old = spec['old_scored_snapshot']
    old_bytes = Path(old['path']).read_bytes()
    new_bytes = Path(args.snapshot).read_bytes()
    if paired.digest(old['path']) != old['sha256'] or not new_bytes.startswith(old_bytes):
        raise ValueError('new source does not retain exact old prefix')
    raw = [json.loads(line) for line in new_bytes[len(old_bytes):].splitlines()]
    untouched = [r for r in raw if r['timestamp_ms'] > old['last_timestamp_ms']
                 and r['generation'] not in old['exclude_post_v9_generations']]
    clean, quarantined = paired.clean_rows(untouched)
    for record in clean:
        if (record['provenance']['loaded_revision'] != spec['configuration_sha256'] or
                record['provenance']['config_file']['sha256'] != spec['configuration_sha256']):
            raise ValueError('configuration changed after preregistration')
        for w in record['windows']:
            if (w['confirmation_m'], w['confirmation_n'], w['delete_after']) != (4,6,3):
                raise ValueError('tracker policy changed')
    output_dir = Path(args.output_directory)
    output_dir.mkdir(exist_ok=True)
    (output_dir/'untouched-qualified-raw.jsonl').write_text(''.join(json.dumps(r,separators=(',', ':'))+'\n' for r in clean))
    flat, episodes, filtered, outputs = infer(clean, spec['arms'], args.tracker, output_dir)
    report = {'schema': 'vectorwarp-depth4-dedup-development-rerun-v1',
        'source_sha256': paired.digest(args.snapshot), 'preregistration_sha256': PREREG_HASH,
        'script_sha256': paired.digest(__file__), 'shared_analysis_sha256': paired.digest(SOURCE),
        'tracker_sha256': paired.digest(args.tracker),
        'new_source_records_after_old_prefix': len(raw), 'untouched_records': len(untouched),
        'qualified_records': len(clean), 'qualified_depth4_endpoints': len(flat),
        'old_generation_rows_excluded': len(raw)-len(untouched), 'quarantined': quarantined,
        'first_timestamp_ms': clean[0]['timestamp_ms'], 'last_timestamp_ms': clean[-1]['timestamp_ms'],
        'generations': dict(Counter(r['generation'] for r in clean)),
        'episodes': [{'generation': rows[0][0]['generation'], 'endpoints': len(rows),
                      'span_s': (rows[-1][0]['timestamp_ms']-rows[0][0]['timestamp_ms'])/1000,
                      'confirmation_eligible': len(rows) >= 4, 'nominal_80_base_cpi_floor': len(rows) >= 20}
                     for rows in episodes],
        'policy': spec['tracker_policy'], 'arms': {}, 'promotion_authorized': False,
        'validity': 'untouched_new_temporal_cohort_development_only',
        'limitations': ['All arms remain uncalibrated candidate families.',
                       'No certified whole passes or independent nuisance exposure.',
                       'Shared aircraft may appear in both chronological snapshots; this is not aircraft-pass-disjoint acceptance.',
                       'Temporal controls are dependent sensitivity tests.']}
    baseline_active = None
    by_source = {r['source_index']: i for i, (_, r, _) in enumerate(flat)}
    for arm in spec['arms']:
        name = arm['name']
        scoring = []
        for record in clean:
            index = by_source.get(record['source_index'])
            if index is None:
                scoring.append(dict(record, windows=[]))
                continue
            window = flat[index][2]
            scale = paired.C/record['delay_bin_sample_rate_hz']/1000
            raw_candidates = window['latest_candidates']
            native = {'delay':[p['delay_bins']*scale for p in raw_candidates],
                      'doppler':[p['doppler_hz'] for p in raw_candidates],
                      'snr':[p['snr_db'] for p in raw_candidates]}
            scoring.append(dict(record, native_detections=native,
                windows=[dict(window, latest_candidates=filtered[name][index])]))
        candidate = paired.candidate_effects(scoring)
        truth_hit_windows = 0
        eligible_truth_windows = 0
        for record in scoring:
            if not record['windows']:
                continue
            truth = paired.truths(record)
            eligible_truth_windows += bool(truth)
            scale = paired.C/record['delay_bin_sample_rate_hz']/1000
            truth_hit_windows += any(paired.near((p['delay_bins']*scale,p['doppler_hz']),value)
                for p in record['windows'][0]['latest_candidates'] for value in truth.values())
        metrics, active = tracker_metrics(flat, outputs[name])
        if baseline_active is None:
            baseline_active = active
        effects = [dict(generation=a['generation'],aircraft=a['aircraft'],
                        native=int(b['hit']),shadow=int(a['hit'])) for a,b in zip(active, baseline_active)]
        report['arms'][name] = {
            'retained_candidates':sum(map(len,filtered[name])),
            'windows_with_eligible_truth':eligible_truth_windows,
            'truth_hit_windows':truth_hit_windows,
            'suppressed_candidates':sum(len(w['latest_candidates']) for _,_,w in flat)-sum(map(len,filtered[name])),
            'candidate_effect_vs_raw':candidate['depth_endpoints']['4'],
            'controls':candidate['shift_controls'].get('4',{}),
            'tracker':metrics, 'active_truth_effect_vs_raw':paired.summarize(effects),
            'all_candidate_values_preserved':all(p in flat[i][2]['latest_candidates'] for i,ps in enumerate(filtered[name]) for p in ps)}
    report['artifact_sha256'] = {p.name:paired.digest(p) for p in sorted(output_dir.glob('*.jsonl'))}
    (output_dir/'report.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n')
    print(json.dumps({k:report[k] for k in ('source_sha256','qualified_records','qualified_depth4_endpoints','old_generation_rows_excluded')}))
    for name, arm in report['arms'].items():
        print(name,json.dumps({'candidates':arm['retained_candidates'],
            'candidate_hits':arm['candidate_effect_vs_raw']['shadow_hits'],
            'candidate_opportunities':arm['candidate_effect_vs_raw']['opportunities'],
            'tracker':arm['tracker']}))


if __name__ == '__main__':
    main()
