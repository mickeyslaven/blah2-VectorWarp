#!/usr/bin/env python3
"""Prepare production-coherent-beam jobs from the exact-associated cohort."""
import argparse, hashlib, json, math
from pathlib import Path

FAMILY_TRIALS=15  # frozen suite variants; each has its charged 45-cell uncertainty grid.
def job(row, role):
    cap=row['capture']; assoc=(cap.get('detection_associations') or [None])[0]
    tr=row['trajectory']; delays=[p['delay_km'] for p in tr]; dops=[p['doppler_hz'] for p in tr]
    # The production cutout is centered on an independently retained detector
    # association, while scoring remains on the ADS-B trajectory plus the fixed
    # uncertainty grid in evaluate-spatial-cutout-suite.py.
    dc=assoc['measured_delay_km'] if assoc else (min(delays)+max(delays))/2
    fc=assoc['measured_doppler_hz'] if assoc else (min(dops)+max(dops))/2
    dr=max(abs(x-dc) for x in delays)+(assoc['gate_delay_km'] if assoc else .5)
    fr=max(abs(x-fc) for x in dops)+(assoc['gate_doppler_hz'] if assoc else 25)
    return {'job_id':row['job_id'],'event_id':row['event_id'],'pass_id':row['pass_id'],
      'hex':row['hex'],'flight':row.get('flight',''),'outcome':row['outcome'],'role':role,
      'revision':cap['revision'],'kind':cap['kind'],'recording':cap['file'],
      'truth_source':'associated_bracketed_event_time_interpolation','trajectory':tr,
      'association_anchor':assoc,'family_trials':FAMILY_TRIALS,
      'control_quantile':1-.01/FAMILY_TRIALS,
      'cutout':{'delay_km':dc,'doppler_hz':fc,
       'delay_radius_bins':math.ceil(dr/.12491352416666666)+4,
       'doppler_radius_bins':math.ceil(fr/2)+4,'max_frames':len(tr)+4}}
def main():
 p=argparse.ArgumentParser();p.add_argument('--cohort',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
 raw=a.cohort.read_bytes(); x=json.loads(raw)
 positives=[job(r,'positive') for r in x['positives']]
 pos_files={j['recording'] for j in positives}; misses=[]
 for r in x['misses']:
  # Preserve every aircraft/event label.  The runner caches replay by
  # recording path, but a shared clip can validly contain multiple targets.
  if r['capture']['file'] in pos_files: continue
  misses.append(job(r,'miss'))
 out={'schema':'vectorwarp-associated-production-coherent-matrix-v1',
  'cohort_sha256':hashlib.sha256(raw).hexdigest(),'status':'positive_calibration_pending',
  'production_method':'coherent_beam_bank_complex_cutouts', 'familywise_trial_count':FAMILY_TRIALS,
  'jobs':positives,'deferred_clean_misses':misses,
  'acceptance':f"Leave-one-pass-out calibration over {len({j['pass_id'] for j in positives})} independent positive passes must show useful held-out discrimination before deferred misses execute."}
 a.output.write_text(json.dumps(out,indent=2)+'\n'); print(json.dumps({'positives':len(positives),'passes':len({j['pass_id'] for j in positives}),'clean_misses':len(misses)}))
if __name__=='__main__': main()
