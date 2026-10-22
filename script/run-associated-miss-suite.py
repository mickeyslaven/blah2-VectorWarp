#!/usr/bin/env python3
import json, os, subprocess, sys, traceback
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
def main():
 cohort=json.load(open(sys.argv[1])); out=Path(sys.argv[2]).resolve(); out.mkdir(parents=True,exist_ok=True)
 src='/root/wwat563-accel/wwat563-spatial-src'; py='/root/wwat563-accel/venv/bin/python'; bin='/root/wwat563-accel/.cache/vectorwarp-calibrated-build/bin/blah2'; cfg='/root/wwat563-accel/wwat-current-config.json'; raw='/root/ordered-recovery-benchmark/associated-misses'
 def run(j):
  cap=j['capture']; t=j['trajectory']; d=sum(x['delay_km'] for x in t)/len(t); f=sum(x['doppler_hz'] for x in t)/len(t); jid=j['job_id']; job={'job_id':jid,'event_id':j['event_id'],'hex':j['hex'],'flight':j.get('flight',''),'outcome':j['outcome'],'role':'miss','revision':cap['revision'],'kind':cap['kind'],'recording':cap['file'],'truth_source':'strict_v2','trajectory':t,'family_trials':15,'control_quantile':1-.01/15}; (out/f'{jid}-job.json').write_text(json.dumps(job)); rp=out/f'{jid}-replay.json'; res=out/f'{jid}-result.json'; rec=f'{raw}/{Path(cap["file"]).name}'
  if res.exists(): return {'job_id': jid, 'state': 'already_complete'}
  # The full strict trajectory must remain within every frame's cutout.  The
  # previous 18x12 crop clipped legitimate trajectory endpoints (notably r8),
  # causing the evaluator to fail closed.  This fixed, predeclared 40x40 crop
  # is shared by every label and still leaves the evaluator's local controls.
  a=[py,f'{src}/script/replay-fusion-ab-cass.py','--binary',bin,'--config',cfg,'--recording',rec,'--harness',f'{src}/test/recording/processor_replay_test.py','--output',str(rp),'--timeout','900','--modes','coherent_beam_bank','--disable-tracker','--complex-cutout-delay-km',str(d),'--complex-cutout-doppler-hz',str(f),'--complex-cutout-delay-radius-bins','40','--complex-cutout-doppler-radius-bins','40','--complex-cutout-max-frames','24']
  env=os.environ | {'LD_LIBRARY_PATH':'/root/wwat563-accel/.cache/vectorwarp-calibrated-build/lib:/root/wwat563-accel/.cache/vectorwarp-calibrated-build/bin'}
  log=out/f'{jid}-error.log'
  try:
   subprocess.run(a,cwd=src,env=env,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
   subprocess.run([py,f'{src}/script/evaluate-spatial-cutout-suite.py','--replay',str(rp),'--config',cfg,'--job',str(out/f'{jid}-job.json'),'--output',str(res)],cwd=src,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
   log.unlink(missing_ok=True)
   return {'job_id': jid, 'state': 'complete'}
  except subprocess.CalledProcessError as error:
   log.write_text(error.stderr or str(error))
   return {'job_id': jid, 'state': 'failed', 'returncode': error.returncode}
 with ThreadPoolExecutor(max_workers=6) as ex:
  for x in ex.map(run,cohort['misses']): print(json.dumps(x),flush=True)
if __name__=='__main__': main()
