#!/usr/bin/env python3
"""Replay one associated positive through the production-equivalent processor."""
import argparse, json, os, statistics, subprocess
from pathlib import Path

def main():
    p=argparse.ArgumentParser(); p.add_argument('--cohort',required=True,type=Path); p.add_argument('--job-id',required=True)
    p.add_argument('--raw-root',required=True,type=Path); p.add_argument('--source',required=True,type=Path)
    p.add_argument('--binary',required=True,type=Path); p.add_argument('--config',required=True,type=Path)
    p.add_argument('--python',required=True,type=Path); p.add_argument('--output',required=True,type=Path); a=p.parse_args()
    cohort=json.loads(a.cohort.read_text()); job=next(x for x in cohort['positives'] if x['job_id']==a.job_id)
    associations=job['validation_associations']; raw=a.raw_root/Path(job['capture']['file']).name
    replay=a.output.with_suffix('.replay.json'); center_delay=statistics.median(x['measured_delay_km'] for x in associations)
    center_doppler=statistics.median(x['measured_doppler_hz'] for x in associations)
    command=[str(a.python),str(a.source/'script/replay-fusion-ab-cass.py'),'--binary',str(a.binary),'--config',str(a.config),
      '--recording',str(raw),'--harness',str(a.source/'test/recording/processor_replay_test.py'),'--output',str(replay),
      '--timeout','900','--modes','coherent_beam_bank','--complex-cutout-delay-km',str(center_delay),
      '--complex-cutout-doppler-hz',str(center_doppler),'--complex-cutout-delay-radius-bins','12',
      '--complex-cutout-doppler-radius-bins','12','--complex-cutout-max-frames','24']
    env={**os.environ,'LD_LIBRARY_PATH':'/root/wwat563-accel/.cache/vectorwarp-calibrated-build/lib:/root/wwat563-accel/.cache/vectorwarp-calibrated-build/bin'}
    subprocess.run(command,cwd=a.source,env=env,check=True,stdout=subprocess.DEVNULL)
    arm=json.loads(replay.read_text())['arms']['coherent_beam_bank']; frames=arm['detections']; matches=[]
    for association in associations:
        nominal=int((association['frame_timestamp_ms']-job['capture']['window_start'])//500)-1
        best=None
        for frame in range(max(0,nominal-1),min(len(frames),nominal+2)):
            row=frames[frame]
            for delay,doppler,snr in zip(row.get('delay',[]),row.get('doppler',[]),row.get('snr',[])):
                dr=abs(delay-association['measured_delay_km']); fr=abs(doppler-association['measured_doppler_hz'])
                candidate={'frame_index':frame,'delay_km':delay,'doppler_hz':doppler,'snr_db':snr,
                  'delay_residual_km':dr,'doppler_residual_hz':fr,'matched':dr<=.25 and fr<=5}
                if best is None or (dr/.25+fr/5)<(best['delay_residual_km']/.25+best['doppler_residual_hz']/5): best=candidate
        matches.append({'association':association,'replay':best})
    reproduced=sum(bool(x['replay'] and x['replay']['matched']) for x in matches)
    result={'schema':'vectorwarp-production-positive-replay-v1','job_id':job['job_id'],'pass_id':job['pass_id'],'hex':job['hex'],
      'recording':str(raw),'association_count':len(matches),'reproduced_associations':reproduced,
      'all_associations_reproduced':reproduced==len(matches),'detection_frames':len(frames),'matches':matches}
    a.output.write_text(json.dumps(result,indent=2)+'\n'); replay.unlink(); print(json.dumps(result,sort_keys=True))
if __name__=='__main__': main()
