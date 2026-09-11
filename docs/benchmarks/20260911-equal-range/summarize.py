#!/usr/bin/env python3
"""Reconcile paired timing CSVs and their fixed-range provenance."""
import argparse
import csv
import json
import math
from pathlib import Path
import statistics

parser = argparse.ArgumentParser()
parser.add_argument('root', type=Path)
args = parser.parse_args()

def percentile(values, fraction):
    values = sorted(values)
    index = (len(values) - 1) * fraction
    low = math.floor(index)
    return values[low] + (values[math.ceil(index)] - values[low]) * (index - low)

all_rows, receipts = [], {}
for host in ('strix', 'nvidia', 'pavilion'):
    folder = args.root / host
    if not (folder / 'summary.json').exists():
        continue
    contract = json.loads((folder / 'contract.json').read_text())
    assert (contract['delay_min'], contract['delay_max'], contract['delay_bins']) == (-10, 245, 256)
    assert contract['sample_rate_hz'] == 2400000 and contract['rf_hz'] == 527000000
    runs = json.loads((folder / 'summary.json').read_text())
    assert len(runs) == {'strix': 34, 'nvidia': 20, 'pavilion': 28}[host], (host, len(runs))
    groups = {}
    for run in runs:
        assert run['returncode'] == 0 and run['sample_clock_paced'] is True
        assert run['delay_bins'] == 256 and run['frames'] == 20 and run['steady_frames'] == 12
        config = json.loads((folder / (run['case'] + '.profile.json')).read_text())
        assert (config['delay_min'], config['delay_max'], config['clutter_min'], config['clutter_max']) == (-10, 245, -10, 200)
        with (folder / (run['label'] + '.frames.csv')).open() as source:
            frames = list(csv.DictReader(source))
        assert len(frames) == 20
        steady = [frame for frame in frames if frame['phase'] == 'steady']
        assert len(steady) == 12
        timings = [float(frame['pipeline_ms']) for frame in steady]
        assert abs(statistics.mean(timings) - run['steady_dsp_mean_ms']) < 1e-6
        assert sum(value > run['requested_cpi_ms'] for value in timings) == run['steady_dsp_deadline_misses']
        for frame in frames:
            for key in ('map_rms_relative', 'map_peak_relative', 'fusion_rms_relative', 'fusion_peak_relative'):
                assert math.isfinite(float(frame[key])) and float(frame[key]) <= 1e-4
        key = (run['case'], run['variant'], run['device'])
        group = groups.setdefault(key, dict(frames=[], repeats=[], summaries=[], config=config))
        group['frames'].extend(steady)
        group['repeats'].append(run['repeat'])
        group['summaries'].append(run)
    for (case, variant, device), group in groups.items():
        assert sorted(group['repeats']) == [1, 2]
        frames = group['frames']
        timings = [float(frame['pipeline_ms']) for frame in frames]
        cpi_ms = group['config']['cpi'] * 1000
        row = dict(host=host, case=case, variant=variant, device=device, cpi_ms=cpi_ms,
                   doppler_half_span_hz=group['config']['doppler_max'], delay_bins=256,
                   max_excess_path_km=contract['excess_path_max_km'], frames=len(frames),
                   mean_ms=statistics.mean(timings), p95_ms=percentile(timings, .95),
                   max_ms=max(timings), deadline_misses=sum(value > cpi_ms for value in timings),
                   mean_capacity=cpi_ms / statistics.mean(timings),
                   gpu_dd_frames=sum(frame['backend'] == 'vulkan' for frame in frames),
                   gpu_clutter_frames=sum(frame['clutter_backend'].startswith('vulkan_fft') for frame in frames),
                   cpu_oracle_frames=sum('cpu_oracle' in frame['clutter_backend'] for frame in frames),
                   max_complex_error=max(float(frame[key]) for frame in frames for key in
                       ('map_rms_relative', 'map_peak_relative', 'fusion_rms_relative', 'fusion_peak_relative')),
                   repeat_means_ms=[run['steady_dsp_mean_ms'] for run in sorted(group['summaries'], key=lambda item:item['repeat'])],
                   stages={key: statistics.mean(float(frame[key]) for frame in frames) for key in
                       ('extract_ms', 'reference_ms', 'spectrum_ms', 'clutter_ms', 'ambiguity_ms',
                        'fusion_ms', 'detection_ms', 'tracker_ms', 'json_ms')})
        all_rows.append(row)
    receipts[host] = dict(contract=contract, measurement_runs=len(runs),
                          complete_cpis=len(runs) * 20, measured_steady_cpis=len(runs) * 12,
                          processing_acceptance='all measured runs passed',
                          runner_exit_note='The final unsupported 40-kHz geometry probe returned expected rejection exit1; timed runs completed before that probe.')
(args.root / 'comparison.json').write_text(json.dumps(dict(receipts=receipts, rows=all_rows), indent=2))
fields = [key for key in all_rows[0] if key not in ('stages', 'repeat_means_ms')]
with (args.root / 'comparison.csv').open('w') as output:
    writer = csv.DictWriter(output, fieldnames=fields, extrasaction='ignore')
    writer.writeheader()
    writer.writerows(all_rows)
print(json.dumps(dict(hosts=list(receipts), measurement_runs=sum(r['measurement_runs'] for r in receipts.values()),
                     matched_groups=len(all_rows)), indent=2))
for row in all_rows:
    print(f"{row['host']:8} {row['case']:23} {row['variant']:16} {row['device']:15} "
          f"{row['mean_ms']:8.3f} ms p95={row['p95_ms']:8.3f} miss={row['deadline_misses']}/24 "
          f"GPUdd={row['gpu_dd_frames']} GPUclutter={row['gpu_clutter_frames']}")
