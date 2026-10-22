#!/usr/bin/env python3
"""Build the strict post-association positive/miss corpus for matrix calibration."""
import argparse, hashlib, importlib.util, json
from pathlib import Path

BASE=Path(__file__).with_name('build-fresh-postfix-cohort.py')
SPEC=importlib.util.spec_from_file_location('fresh',BASE); FRESH=importlib.util.module_from_spec(SPEC); SPEC.loader.exec_module(FRESH)

def build(statistics,policy):
    statistics=dict(statistics)
    statistics['eligibility_events']=statistics.get('eligibility_events',statistics.get('eligibilityEvents',[]))
    base=FRESH.build(statistics,policy,0,[],None)
    positives=[]; misses=[]; rejected=list(base['rejected'])
    for row in base['admitted']:
        capture=row['capture']; detections=capture.get('detection_associations') or []
        trackers=capture.get('tracker_associations') or []
        item={**row,'job_id':hashlib.sha256((row['event_id']+'|'+capture['file']).encode()).hexdigest()[:20]}
        if row['role']=='positive':
            if detections:
                item['validation_associations']=detections; positives.append(item)
            else:
                item['reasons']=['positive lacks aircraft-specific detection association']; rejected.append(item)
        elif row['role']=='geometry_eligible_miss' and not detections and not trackers:
            misses.append(item)
    positive_files={x['capture']['file'] for x in positives}; miss_files={x['capture']['file'] for x in misses}
    return {'schema':'vectorwarp-associated-control-cohort-v1','policy':policy,
            'positives':positives,'misses':misses,'rejected':rejected,
            'summary':{'positive_labels':len(positives),'positive_unique_clips':len(positive_files),
              'positive_passes':len({x['pass_id'] for x in positives}),'miss_labels':len(misses),
              'miss_unique_clips':len(miss_files),'miss_passes':len({x['pass_id'] for x in misses}),
              'positive_miss_clip_overlap':len(positive_files & miss_files),'rejected':len(rejected)}}

def main():
    p=argparse.ArgumentParser(); p.add_argument('--statistics',required=True,type=Path); p.add_argument('--policy',required=True,type=Path); p.add_argument('--output',required=True,type=Path); a=p.parse_args()
    value=build(json.loads(a.statistics.read_text()),json.loads(a.policy.read_text())); a.output.write_text(json.dumps(value,indent=2)+'\n'); print(json.dumps(value['summary'],sort_keys=True))
if __name__=='__main__': main()
