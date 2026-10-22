#!/usr/bin/env python3
"""Score exact detector-associated frames from production coherent-beam cutouts."""
import argparse,json,math
from pathlib import Path
OFFSETS=((r,c) for r in range(-2,3) for c in range(-2,3))
def nearest(a,x): return min(range(len(a)),key=lambda i:abs(a[i]-x))
def c(v): return complex(v[0],v[1])
def mapper(cap,a,n): return max(0,min(n-1,int((a['frame_timestamp_ms']-cap['window_start'])//500)-1))
def score(frame,delay,doppler):
 r=nearest(frame['doppler_hz'],doppler); q=nearest(frame['delay_km'],delay)
 z=[c(ch[r][q]) for ch in frame['channels']]; w=[c(x) for x in frame['reference_weights']]
 beam=abs(sum(x*complex(y.real,-y.imag) for x,y in zip(z,w)))**2
 power=sum(abs(x)**2 for x in z); glrt=beam/max(power,1e-12)
 stap=beam/max(sum(abs(x-sum(z)/len(z))**2 for x in z),1e-12)
 sparse=sum(sorted((abs(x)**2 for x in z),reverse=True)[:max(1,len(z)//2)])
 # These are one-frame algebraic diagnostics only.  Full GLRT/STAP/sparse
 # labels are reserved for evaluate-spatial-cutout-suite.py's 20-CPI methods.
 return {'diagnostic_coherent_beam':beam,'diagnostic_normalized_beam':glrt,
         'diagnostic_residual_ratio':stap,'diagnostic_top_channel_power':sparse}
def main():
 p=argparse.ArgumentParser();p.add_argument('--cohort',type=Path,required=True);p.add_argument('--replay-dir',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args(); cohort=json.loads(a.cohort.read_text()); rows=[]
 for job in cohort['positives']:
  if job['capture']['revision']==90: continue
  rp=a.replay_dir/f"{job['job_id']}-replay.json"
  if not rp.exists(): continue
  arm=json.loads(rp.read_text())['arms']['coherent_beam_bank']; cuts=arm['complex_cutouts']; det=arm['detections']
  for assoc in job['validation_associations']:
   nominal=mapper(job['capture'],assoc,len(det)); best=None
   for idx in range(max(0,nominal-1),min(len(det),nominal+2)):
    for d,f in zip(det[idx].get('delay',[]),det[idx].get('doppler',[])):
     err=abs(d-assoc['measured_delay_km'])/.25+abs(f-assoc['measured_doppler_hz'])/5
     if best is None or err<best[0]: best=(err,idx)
   if best is None or best[0]>2: continue
   frame=cuts[best[1]]; target=score(frame,assoc['measured_delay_km'],assoc['measured_doppler_hz']); controls=[]
   for dr,dc in OFFSETS:
    if dr or dc: controls.append(score(frame,assoc['measured_delay_km']+dc*.125,assoc['measured_doppler_hz']+dr*2))
   for method,value in target.items():
    cv=[x[method] for x in controls]; rank=1+sum(x>=value for x in cv); p99=sorted(cv)[math.ceil(.99*len(cv))-1]
    rows.append({'pass_id':job['pass_id'],'event_id':job['event_id'],'revision':job['capture']['revision'],'method':method,'frame_index':best[1],'nominal_frame':nominal,'raw_score':value,'rank':rank,'control_count':len(cv),'margin_db':10*math.log10(max(value,1e-300)/max(p99,1e-300))})
 out={'schema':'vectorwarp-exact-associated-frame-score-v1','status':'exploratory_positive_only','rows':rows,'passes':len({x['pass_id'] for x in rows}),'quarantined_revisions':[90],'control_policy':'same-frame local translated offsets; 24 controls per method'};a.output.write_text(json.dumps(out,indent=2)+'\n');print(json.dumps({'rows':len(rows),'passes':out['passes']}))
if __name__=='__main__': main()
