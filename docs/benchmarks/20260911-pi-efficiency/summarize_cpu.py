#!/usr/bin/env python3
"""Rebuild Pi CPU timing summaries from all paired per-frame receipts."""
import argparse
import csv
import json
import math
from pathlib import Path
import statistics

parser = argparse.ArgumentParser()
parser.add_argument('folder', type=Path)
args = parser.parse_args()
root = args.folder
contract = json.loads((root/'contract.json').read_text())
assert (contract['fs'],contract['rf_hz'],contract['delay_min'],contract['delay_max']) == (2400000,527000000,-10,245)
runs = json.loads((root/'summary.json').read_text())
assert len(runs)==24
groups = {}
for run in runs:
    assert run['frames']==20 and run['steady_frames']==12
    with (root/(run['label']+'.frames.csv')).open() as source:
        frames = list(csv.DictReader(source))
    assert len(frames)==20
    for frame in frames:
        assert frame['backend']=='cpu'
        for key in ('map_rms_relative','map_peak_relative','fusion_rms_relative','fusion_peak_relative'):
            assert math.isfinite(float(frame[key])) and float(frame[key])<=1e-4
    steady = [frame for frame in frames if frame['phase']=='steady']
    assert len(steady)==12
    assert abs(statistics.mean(float(frame['pipeline_ms']) for frame in steady)-run['steady_dsp_mean_ms'])<1e-6
    variant = {'fast':'vectorwarp-cpu','upstream':'regular-blah2','offworld':'offworld-blah2-arm'}[run['variant']]
    group = groups.setdefault((run['case'],variant),dict(frames=[],repeats=[]))
    group['frames'].extend(steady)
    group['repeats'].append(run['repeat'])
rows = []
for (case,variant),group in groups.items():
    assert sorted(group['repeats'])==[1,2]
    frames = group['frames']
    values = sorted(float(frame['pipeline_ms']) for frame in frames)
    p = (len(values)-1)*.95
    config = json.loads((root/(case+'.profile.json')).read_text())
    row = dict(case=case,variant=variant,frames=len(values),cpi_ms=200,
        doppler_half_span_hz=config['doppler_max'],clutter_min=config['clutter_min'],
        clutter_max=config['clutter_max'],max_excess_path_km=contract['max_excess_path_km'],
        mean_ms=statistics.mean(values),p95_ms=values[math.floor(p)]+(values[math.ceil(p)]-values[math.floor(p)])*(p-math.floor(p)),
        max_ms=max(values),deadline_misses=sum(v>200 for v in values),
        stages={key:statistics.mean(float(frame[key]) for frame in frames) for key in
            ('extract_ms','reference_ms','spectrum_ms','clutter_ms','ambiguity_ms','fusion_ms','detection_ms','tracker_ms','json_ms')})
    rows.append(row)
for row in rows:
    upstream = next(item for item in rows if item['case']==row['case'] and item['variant']=='regular-blah2')
    offworld = next(item for item in rows if item['case']==row['case'] and item['variant']=='offworld-blah2-arm')
    row['speed_vs_blah2'] = upstream['mean_ms']/row['mean_ms']
    row['less_processing_time_percent'] = (1-row['mean_ms']/upstream['mean_ms'])*100
    row['less_time_vs_offworld_percent'] = (1-row['mean_ms']/offworld['mean_ms'])*100
(root/'comparison.json').write_text(json.dumps(dict(contract=contract,runs=24,complete_cpis=480,rows=rows),indent=2))
fields = [key for key in rows[0] if key!='stages']
with (root/'comparison.csv').open('w') as output:
    writer = csv.DictWriter(output,fieldnames=fields,extrasaction='ignore',lineterminator='\n')
    writer.writeheader(); writer.writerows(rows)
print('PASS: 24 three-way runs / 480 CPIs / 12 groups')
for row in rows:
    print(f"{row['case']} {row['variant']}: {row['mean_ms']:.3f} ms p95={row['p95_ms']:.3f} misses={row['deadline_misses']}/24 reduction={row['less_processing_time_percent']:.1f}%")
