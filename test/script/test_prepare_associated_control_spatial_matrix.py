#!/usr/bin/env python3
import importlib.util
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('associated',ROOT/'script'/'prepare-associated-control-spatial-matrix.py')
module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
cohort=json.loads((ROOT/'results'/'20261022-associated-control-cohort.json').read_text())
positives=[module.job(row,'positive') for row in cohort['positives']]
positive_files={row['recording'] for row in positives}; seen=set(); misses=[]
for row in cohort['misses']:
    if row['capture']['file'] in positive_files or row['capture']['file'] in seen: continue
    seen.add(row['capture']['file']); misses.append(module.job(row,'miss'))
assert len(positives)==8 and len({row['pass_id'] for row in positives})==7
assert len(misses)==51  # 52 source-unique misses includes one positive recording.
assert all(row['association_anchor'] for row in positives)
assert all(row['association_anchor'] is None for row in misses)
assert not positive_files & {row['recording'] for row in misses}
print('PASS associated coherent matrix admission and overlap guard')
