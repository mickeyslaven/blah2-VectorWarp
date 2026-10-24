#!/usr/bin/env python3
import importlib.util
import json, tempfile
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('associated',ROOT/'script'/'prepare-associated-control-spatial-matrix.py')
module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
cohort={'positives':[{'job_id':'p','event_id':'e','pass_id':'pass','hex':'a','outcome':'tracked','trajectory':[{'delay_km':1,'doppler_hz':2}], 'capture':{'revision':1,'kind':'initial','file':'p.mchq','detection_associations':[{'measured_delay_km':1,'measured_doppler_hz':2,'gate_delay_km':.2,'gate_doppler_hz':5}]}}], 'misses':[{'job_id':'m','event_id':'m','pass_id':'miss','hex':'b','outcome':'no_radar_return','trajectory':[{'delay_km':3,'doppler_hz':4}], 'capture':{'revision':2,'kind':'initial','file':'m.mchq','detection_associations':[]}}, {'job_id':'overlap','event_id':'o','pass_id':'o','hex':'a','outcome':'no_radar_return','trajectory':[{'delay_km':1,'doppler_hz':2}], 'capture':{'revision':1,'kind':'initial','file':'p.mchq','detection_associations':[]}}]}
positives=[module.job(row,'positive') for row in cohort['positives']]
positive_files={row['recording'] for row in positives}; seen=set(); misses=[]
for row in cohort['misses']:
    if row['capture']['file'] in positive_files or row['capture']['file'] in seen: continue
    seen.add(row['capture']['file']); misses.append(module.job(row,'miss'))
assert len(positives)==1 and len({row['pass_id'] for row in positives})==1
assert len(misses)==1
assert all(row['association_anchor'] for row in positives)
assert all(row['association_anchor'] is None for row in misses)
assert not positive_files & {row['recording'] for row in misses}
print('PASS associated coherent matrix admission and overlap guard')
