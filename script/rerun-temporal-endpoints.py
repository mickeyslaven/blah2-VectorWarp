#!/usr/bin/env python3
"""Freeze-aware development scoring and matched production-CPU tracker replay.

Truth is consumed only after inference. No live configuration is modified.
Native JSON detections have display quantization; this is not exact-IQ parity.
"""
import argparse
import bisect
from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path
import random
import subprocess

C = 299792458.0


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def near(point, truth):
    return abs(point[0] - truth['delay']) <= .25 and abs(point[1] - truth['doppler']) <= 10


def truths(record):
    extent = record['search_extent']
    return {key: value for key, value in record['truth_delay_doppler'].items()
            if isinstance(value, dict)
            and isinstance(value.get('delay'), (int, float))
            and isinstance(value.get('doppler'), (int, float))
            and extent['delay_min_km'] <= value['delay'] <= extent['delay_max_km']
            and 15 <= abs(value['doppler']) <= 600}


def clean_rows(records):
    accepted, rejected = [], []
    seen = set()
    for index, record in enumerate(records):
        reasons = []
        p = record.get('provenance', {})
        if (not p.get('processor_binary', {}).get('sha256') or
                p.get('loaded_revision') != p.get('config_revision') or
                not p.get('config_file', {}).get('sha256')):
            reasons.append('provenance_incomplete')
        native = record.get('native_detections', {})
        if native.get('timestamp') != record.get('timestamp_ms'):
            reasons.append('native_endpoint_mismatch')
        if len({len(native.get(k, [])) for k in ('delay', 'doppler', 'snr')}) != 1:
            reasons.append('native_arrays_mismatch')
        windows = []
        for w in record.get('windows', []):
            issues = list(reasons)
            depth = w['depth']
            seq = int(w['latest_evaluated_sequence'])
            key = (record['generation'], int(record.get('reset_serial', 0)), depth, seq)
            if key in seen:
                issues.append('duplicate_endpoint')
            seen.add(key)
            if seq != int(record['sequence']):
                issues.append('shadow_endpoint_mismatch')
            if any(k not in w for k in ('base_cpi_seconds', 'confirmation_m',
                                       'confirmation_n', 'delete_after')):
                issues.append('pre_v9_policy_missing')
            if (w.get('latest_candidate_count') != len(w['latest_candidates']) or
                    w.get('truncated_candidates', 0) or
                    len(w['latest_candidates']) > w.get('latest_candidates_export_limit', 0)):
                issues.append('candidate_export_incomplete')
            for point in w['latest_candidates']:
                if ([int(s) for s in point.get('support_sequences', [])] !=
                        list(range(seq, seq-depth, -1)) or
                        str(point.get('epoch')) != str(record['capture_epoch'])):
                    issues.append('candidate_lineage_invalid')
                    break
            if issues:
                rejected.append({'row': index, 'generation': record['generation'],
                                 'depth': depth, 'reasons': sorted(set(issues))})
            else:
                windows.append(w)
        if windows:
            accepted.append(dict(record, windows=windows, source_index=index))
    return accepted, rejected


def bootstrap(rows, cluster, seed=1471):
    groups = defaultdict(lambda: [0, 0])
    for row in rows:
        groups[row[cluster]][0] += row['shadow'] - row['native']
        groups[row[cluster]][1] += 1
    values = list(groups.values())
    if len(values) < 2:
        return {'clusters': len(values), 'ci95_pp': None}
    rng = random.Random(seed)
    effects = []
    for _ in range(2000):
        sample = rng.choices(values, k=len(values))
        effects.append(100 * sum(v[0] for v in sample) / sum(v[1] for v in sample))
    effects.sort()
    return {'clusters': len(values), 'ci95_pp': [effects[49], effects[1949]]}


def summarize(rows):
    count = len(rows)
    native = sum(r['native'] for r in rows)
    shadow = sum(r['shadow'] for r in rows)
    return {'opportunities': count, 'native_hits': native, 'shadow_hits': shadow,
            'shadow_only': sum(r['shadow'] and not r['native'] for r in rows),
            'native_only': sum(r['native'] and not r['shadow'] for r in rows),
            'native_recall': native/count if count else None,
            'shadow_recall': shadow/count if count else None,
            'paired_delta_pp': 100*(shadow-native)/count if count else None,
            'generation_cluster_bootstrap': bootstrap(rows, 'generation'),
            'aircraft_cluster_bootstrap': bootstrap(rows, 'aircraft'),
            'uncertainty_scope': 'development fragments; neither cluster scheme makes these complete independent passes'}


def candidate_effects(records):
    opportunities = defaultdict(list)
    controls = defaultdict(lambda: defaultdict(list))
    overlap_exclusions = Counter()
    cohort_windows = Counter()
    candidate_counts = defaultdict(Counter)
    by_generation = defaultdict(list)
    for record in records:
        by_generation[record['generation']].append(record)
    for generation, group in by_generation.items():
        times = [r['timestamp_ms'] for r in group]
        for record in group:
            current_truth = truths(record)
            bin_km = C / record['delay_bin_sample_rate_hz'] / 1000
            native = list(zip(record['native_detections']['delay'], record['native_detections']['doppler']))
            common = {w['depth'] for w in record['windows']} == {2, 4, 8}
            for window in record['windows']:
                depth = str(window['depth'])
                candidates = [(p['delay_bins']*bin_km, p['doppler_hz']) for p in window['latest_candidates']]
                candidate_counts[depth]['native_exported_candidates'] += len(native)
                candidate_counts[depth]['shadow_exported_candidates'] += len(candidates)
                for truth in current_truth.values():
                    nearby = [p for p in window['latest_candidates'] if near(
                        (p['delay_bins']*bin_km, p['doppler_hz']), truth)]
                    if nearby:
                        candidate_counts[depth]['truth_hits_with_shadow_candidate_at_least_7db'] += any(
                            p['snr_db'] >= 7 for p in nearby)
                cohorts = ['depth_endpoints'] + (['all_depths_common_endpoints'] if common else [])
                for cohort in cohorts:
                    cohort_windows[(cohort, depth)] += 1
                    for aircraft, truth in current_truth.items():
                        opportunities[(cohort, depth)].append({
                            'generation': generation, 'aircraft': aircraft,
                            'native': int(any(near(p, truth) for p in native)),
                            'shadow': int(any(near(p, truth) for p in candidates))})
                # Fixed prespecified offsets, no wrapping or crossing generations.
                # These are dependent sensitivity controls, not independent nuisance data.
                for shift in (17, 31, 61):
                    target = record['timestamp_ms'] + shift*1000
                    at = bisect.bisect_left(times, target)
                    choices = [i for i in (at-1, at) if 0 <= i < len(group)]
                    if not choices:
                        continue
                    at = min(choices, key=lambda i: abs(times[i]-target))
                    if abs(times[at]-target) > 600:
                        continue
                    for aircraft, truth in truths(group[at]).items():
                        if any(abs(truth['delay']-v['delay']) <= .5 and
                               abs(truth['doppler']-v['doppler']) <= 20
                               for v in current_truth.values()):
                            overlap_exclusions[(depth, str(shift))] += 1
                            continue
                        controls[depth][str(shift)].append({
                            'generation': generation, 'aircraft': aircraft,
                            'native': int(any(near(p, truth) for p in native)),
                            'shadow': int(any(near(p, truth) for p in candidates))})
    result = {}
    for cohort in ('depth_endpoints', 'all_depths_common_endpoints'):
        result[cohort] = {}
        for depth in ('2', '4', '8'):
            rows = opportunities[(cohort, depth)]
            result[cohort][depth] = dict(summarize(rows), windows=cohort_windows[(cohort, depth)],
                by_generation={g: summarize([r for r in rows if r['generation'] == g])
                               for g in by_generation})
    result['shift_controls'] = {d: {s: dict(summarize(rows),
        present_truth_overlap_excluded=overlap_exclusions[(d, s)])
        for s, rows in shifts.items()} for d, shifts in controls.items()}
    result['control_scope'] = 'Three temporal offsets from the same ledger; no wrap/reset crossing; overlap with current truth excluded; not independent false-track exposure.'
    result['candidate_budgets_and_birth_support'] = dict(candidate_counts)
    return result


def replay(records, executable, output_dir):
    result = {}
    for depth in (2, 4, 8):
        endpoints = [(r, w) for r in records for w in r['windows'] if w['depth'] == depth]
        segments, previous, segment = [], None, -1
        for r, w in endpoints:
            reset = (previous is None or r['generation'] != previous[0]['generation'] or
                     int(r.get('reset_serial', 0)) != int(previous[0].get('reset_serial', 0)) or
                     int(w['latest_evaluated_sequence'])-int(previous[1]['latest_evaluated_sequence']) != depth)
            if reset:
                segment += 1
                segments.append([])
            segments[-1].append((r, w))
            previous = (r, w)
        flat = [(sid, r, w) for sid, rows in enumerate(segments) for r, w in rows]
        first_window = endpoints[0][1]
        step = first_window['update_interval_seconds']
        m, n, deletion = (first_window[k] for k in ('confirmation_m', 'confirmation_n', 'delete_after'))
        result[str(depth)] = {'policy': {'m': m, 'n': n, 'delete_after': deletion,
            'birth_snr_db': 7, 'base_cpi_seconds': first_window['base_cpi_seconds'],
            'update_interval_seconds': step, 'first_seed_to_earliest_confirmation_s': (m-1)*step,
            'confirmation_evidence_horizon_s': m*step, 'n_window_s': n*step,
            'deletion_miss_horizon_s': (deletion+1)*step,
            'ridge_birth_filter': False, 'normalized_gate': True, 'adaptive_gate': True,
            'doppler_resolution_hz': 1/first_window['base_cpi_seconds'],
            'wavelength_frequency_hz': 527000000, 'max_acc_hz_s': 10},
            'episodes': [{'id': i, 'generation': rows[0][0]['generation'], 'endpoints': len(rows),
                          'span_s': (rows[-1][0]['timestamp_ms']-rows[0][0]['timestamp_ms'])/1000,
                          'at_least_m_endpoints': len(rows) >= m,
                          'warmup_confirmation_tail_80basecpi_floor': len(rows)*depth >= 80}
                         for i, rows in enumerate(segments)],
            'arms': {}}
        for arm in ('native', 'shadow'):
            inputs = []
            previous_segment = None
            for index, (sid, record, window) in enumerate(flat):
                scale = C/record['delay_bin_sample_rate_hz']/1000
                native = record['native_detections']
                points = ([[d/scale, f, s] for d,f,s in zip(native['delay'], native['doppler'], native['snr'])]
                          if arm == 'native' else [[p['delay_bins'], p['doppler_hz'], p['snr_db']]
                          for p in window['latest_candidates']])
                inputs.append({'index': index, 'reset': sid != previous_segment,
                    'timestamp_ms': record['timestamp_ms'], 'points': points,
                    'm': m, 'n': n, 'delete_after': deletion,
                    'base_cpi': window['base_cpi_seconds'], 'birth_snr_db': 7,
                    'max_acc': 10, 'range_resolution_m': scale*1000,
                    'wavelength_m': C/527000000})
                previous_segment = sid
            text = ''.join(json.dumps(row, separators=(',', ':'))+'\n' for row in inputs)
            (output_dir/f'depth-{depth}-{arm}-input.jsonl').write_text(text)
            completed = subprocess.run([executable], input=text, text=True, capture_output=True,
                                       check=True, timeout=90)
            (output_dir/f'depth-{depth}-{arm}-output.jsonl').write_text(completed.stdout)
            outputs = [json.loads(line) for line in completed.stdout.splitlines()]
            if len(outputs) != len(flat):
                raise ValueError('replay output count mismatch')
            counts = Counter()
            pass_events = defaultdict(list)
            active_track_ids = set()
            track_birth = {}
            confirmed = set()
            seed_delays = []
            truth_run = Counter()
            truth_previous = {}
            active_seconds = 0
            for (sid, record, window), output in zip(flat, outputs):
                truth = truths(record)
                scale = C/record['delay_bin_sample_rate_hz']/1000
                for key in ('births', 'associations', 'misses', 'promotions'):
                    counts[key] += output[key]
                active = [t for t in output['tracks'] if t['state'] == 'ACTIVE']
                for track in output['tracks']:
                    key = (sid, track['id'])
                    track_birth.setdefault(key, record['timestamp_ms'])
                    if track['state'] == 'ACTIVE' and key not in confirmed:
                        seed_delays.append((record['timestamp_ms']-track_birth[key])/1000)
                        confirmed.add(key)
                counts['active_track_endpoints'] += len(active)
                counts['coasting_track_endpoints'] += sum(t['state'] == 'COASTING' for t in output['tracks'])
                counts['truth_opportunities'] += len(truth)
                for track in active:
                    active_track_ids.add((sid, track['id']))
                    if not any(near((track['delay_bins']*scale, track['doppler_hz']), v) for v in truth.values()):
                        counts['unmatched_active_track_endpoints_unlabeled'] += 1
                for aircraft, value in truth.items():
                    matched = [t['id'] for t in active if near((t['delay_bins']*scale, t['doppler_hz']), value)]
                    counts['measured_active_truth_hits'] += bool(matched)
                    counts['duplicate_active_truth_track_endpoints'] += max(0, len(matched)-1)
                    key = (sid, aircraft)
                    if key in truth_previous and record['timestamp_ms']-truth_previous[key] > step*1500:
                        truth_run[key] += 1
                    truth_previous[key] = record['timestamp_ms']
                    pass_events[(sid, aircraft, truth_run[key])].append((record['timestamp_ms'], matched))
                active_seconds += step
            fragments, delays, continuity = [], [], []
            for (sid, aircraft, truth_episode), events in pass_events.items():
                segments_count, previous_ids, run, longest = 0, set(), 0, 0
                for timestamp, matched in events:
                    matched = set(matched)
                    if matched:
                        if not previous_ids.intersection(matched):
                            segments_count += 1
                        run += 1
                        longest = max(longest, run)
                    else:
                        run = 0
                    previous_ids = matched
                fragments.append(segments_count)
                continuity.append(longest*step)
                hits = [timestamp for timestamp, matched in events if matched]
                if hits:
                    delays.append((hits[0]-events[0][0])/1000)
            counts['missed_eligible_truth_endpoints'] = counts['truth_opportunities'] - counts['measured_active_truth_hits']
            result[str(depth)]['arms'][arm] = dict(counts,
                replayed_endpoint_exposure_s=active_seconds,
                eligible_segment_aircraft_fragments=len(pass_events),
                matched_fragments=sum(x > 0 for x in fragments),
                active_track_ids=len(active_track_ids),
                truth_track_segments=sum(fragments),
                fragmentation_extra_segments=sum(max(0,x-1) for x in fragments),
                first_eligible_to_active_delays_s=delays,
                measured_seed_to_confirmation_delays_s=seed_delays,
                maximum_measured_active_continuity_s=max(continuity, default=0),
                false_tracks_per_hour=None,
                false_track_reason='unmatched is unlabeled; no adjudicated nuisance exposure',
                whole_pass_acceptance=False)
            if arm == 'shadow':
                parity = Counter()
                previous = None
                gap_seen = False
                for (_, record, window), output in zip(flat, outputs):
                    if previous is None or previous[0]['generation'] != record['generation']:
                        gap_seen = False
                    elif int(window['latest_evaluated_sequence'])-int(previous[1]['latest_evaluated_sequence']) != depth:
                        gap_seen = True
                    previous = (record, window)
                    if gap_seen:
                        continue
                    parity['endpoints_without_prior_collection_gap'] += 1
                    actual = (window['tracks'], window['active_tracks'], window['coasting_tracks'])
                    observed = (len(output['tracks']),
                        sum(t['state'] == 'ACTIVE' for t in output['tracks']),
                        sum(t['state'] == 'COASTING' for t in output['tracks']))
                    parity['exact_live_track_state_counts_agree'] += actual == observed
                result[str(depth)]['shadow_live_count_parity'] = dict(parity)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('ledger')
    parser.add_argument('--tracker', required=True)
    parser.add_argument('--output-directory', required=True)
    args = parser.parse_args()
    output_dir = Path(args.output_directory)
    output_dir.mkdir(exist_ok=True)
    records = [json.loads(line) for line in Path(args.ledger).read_text().splitlines()]
    clean, quarantined = clean_rows(records)
    root = Path(__file__).resolve().parents[1]
    code_files = ['tools/TemporalEndpointReplay.cpp', 'src/process/tracker/Tracker.cpp',
                  'src/process/tracker/Tracker.h', 'src/process/tracker/NormalizedInnovationGate.h',
                  'src/data/Track.cpp', 'src/data/Track.h', 'src/data/Detection.cpp', 'src/data/Detection.h']
    report = {'schema': 'temporal-endpoint-development-rerun-v1',
        'source': str(Path(args.ledger).resolve()), 'source_sha256': digest(args.ledger),
        'script_sha256': digest(__file__), 'tracker_sha256': digest(args.tracker),
        'code_provenance': {name: digest(root/name) for name in code_files},
        'source_records': len(records), 'accepted_records': len(clean),
        'quarantined_windows': quarantined,
        'gates': {'delay_km': .25, 'doppler_hz': 10, 'minimum_abs_doppler': 15,
                  'maximum_abs_doppler': 600, 'delay_extent': 'per-record immutable search_extent'},
        'promotion_authorized': False, 'validity': 'development_only',
        'candidate_effects': candidate_effects(clean),
        'matched_tracker_replay': replay(clean, args.tracker, output_dir),
        'limitations': ['Incomplete pass fragments and frequent observer resets.',
            'Temporal statistic families lack independent null calibration.',
            'Shift controls share data and do not replace independent nuisance exposure.',
            'Native detections are quantized display exports; replay is production CPU code, not device parity.',
            'Both tracker arms use ridge-disabled shadow birth policy, not full live native inference policy.',
            'Unmatched tracks are unlabeled, not false tracks.']}
    (output_dir/'report.json').write_text(json.dumps(report, indent=2, allow_nan=False)+'\n')
    print(json.dumps({k:report[k] for k in ('source_sha256','source_records','accepted_records','quarantined_windows')}))
    for depth, row in report['candidate_effects']['depth_endpoints'].items():
        print('candidate', depth, json.dumps({k:row[k] for k in ('opportunities','native_hits','shadow_hits','paired_delta_pp','generation_cluster_bootstrap','aircraft_cluster_bootstrap')}))
    for depth, row in report['matched_tracker_replay'].items():
        print('tracker',depth,json.dumps({arm:{k:values.get(k,0) for k in (
            'truth_opportunities','measured_active_truth_hits','unmatched_active_track_endpoints_unlabeled',
            'duplicate_active_truth_track_endpoints','missed_eligible_truth_endpoints',
            'maximum_measured_active_continuity_s')} for arm,values in row['arms'].items()}))


if __name__ == '__main__':
    main()
