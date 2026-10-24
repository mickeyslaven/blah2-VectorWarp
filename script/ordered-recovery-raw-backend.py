#!/usr/bin/env python3
"""Raw-IQ backend for the ordered recovery matrix (offline only).

It materializes/reuses the existing MCHQ waveform primitives per recording.
The primitive score vectors are translated-trajectory controls, so the output
is never based on a label or a detector scalar alone.  Spatial requests use
the already-generated complex-cutout score vectors when supplied in
``VECTORWARP_SPATIAL_RESULTS``; otherwise they fail closed.
"""
import argparse, hashlib, importlib.util, json, math, os, statistics
from pathlib import Path
import numpy as np

ROOT = Path(__file__).parent
def load(name):
    spec=importlib.util.spec_from_file_location(name, ROOT/name); mod=importlib.util.module_from_spec(spec); spec.loader.exec_module(mod); return mod
LOADED=load('analyze-loaded-reciprocal-mchq.py'); SPECTRAL=load('analyze-spectral-interference-rejection.py'); NOISY=load('analyze-noisy-reference-glrt.py')

def atomic(path, value):
    part=path.with_suffix(path.suffix+'.part'); part.write_text(json.dumps(value,separators=(',',':'))+'\n'); part.replace(path)
def cached(path, build):
    if path.exists(): return json.loads(path.read_text())
    value=build(); atomic(path,value); return value
def trajectory(job, directory):
    path=directory/'trajectory.json'
    if not path.exists(): atomic(path, {'trajectory':job['trajectory']})
    return path
def waveform(job, directory):
    t=trajectory(job,directory); raw=job['recording']
    if not Path(raw).exists():
        staged=Path(os.environ.get('VECTORWARP_RAW_ROOT',''))/Path(raw).name
        if not staged.exists() and job.get('revision') is not None:
            staged=Path(os.environ.get('VECTORWARP_RAW_ROOT',''))/(f"r{job['revision']}.mchq")
        if staged.exists(): raw=str(staged)
        else: raise FileNotFoundError('raw IQ must be staged on compute host: '+str(raw))
    common=dict(recording=raw,trajectory_file=t,cpi=.5,fft_size=1024,max_delay_km=64,max_doppler_hz=800,guard=2,training=8,controls_stride=8)
    return (cached(directory/'loaded.json',lambda: LOADED.analyze(loadings=[.01,.1,1.],**common)),
            cached(directory/'spectral.json',lambda: SPECTRAL.analyze(loadings=[.25,1.,4.],subbands=24,**common)),
            cached(directory/'noisy.json',lambda: NOISY.analyze(recording=raw,trajectory_file=t,cpi=.5,fft_size=1024,max_delay_km=64,max_doppler_hz=800,guard=2,controls_stride=8)))
def materialized(job, directory):
    """Cache actual per-CPI CAF/GLRT fields for ordered A--D processing."""
    path=directory/'ordered-fields.npz'; t=trajectory(job,directory); raw=job['recording']
    if not Path(raw).exists():
        staged=Path(os.environ.get('VECTORWARP_RAW_ROOT',''))/Path(raw).name
        if not staged.exists() and job.get('revision') is not None:
            staged=Path(os.environ.get('VECTORWARP_RAW_ROOT',''))/(f"r{job['revision']}.mchq")
        if staged.exists(): raw=str(staged)
        else: raise FileNotFoundError('raw IQ must be staged on compute host: '+str(raw))
    if path.exists():
        z=np.load(path); return {k:z[k] for k in z.files}
    truth=json.loads(t.read_text())['trajectory']; fields={k:[] for k in ('s25','s1','l01','l1','l10','known','unknown')}; axis=None
    n=int(.5*LOADED.SAMPLE_RATE); maxdelay=int(math.ceil(64*LOADED.SAMPLE_RATE/LOADED.C_KM_S))+1
    for index,iq in enumerate(LOADED.frames(raw,n)):
        if index>=len(truth): break
        axis,spec,_=SPECTRAL.spectral_caf(iq,1024,maxdelay,800,[.25,1.],24)
        _,loaded,_=LOADED.caf_frame(iq,1024,maxdelay,800,[.01,.1,1.])
        _,glrt,_=NOISY.caf_sufficient_statistics(iq,1024,maxdelay,800)
        for key,value in (('s25',spec[.25]),('s1',spec[1.]),('l01',loaded[.01]),('l1',loaded[.1]),('l10',loaded[1.]),('known',glrt['glrt_known_estimated_noise']),('unknown',glrt['glrt_unknown_noise'])): fields[key].append(value.astype(np.float32))
    if not fields['s25']: raise ValueError('no complete raw CPI for ordered fields')
    arrays={k:np.asarray(v) for k,v in fields.items()}; arrays['doppler']=axis.astype(np.float32); np.savez_compressed(path,**arrays); return arrays
def normalize(surface, axis, strength):
    # Two actual noncommuting operators.  spectral normalizes each Doppler
    # row; loaded normalizes each delay column.  Their order is therefore
    # observable and cached at the per-CPI surface level.
    floor=np.median(surface,axis=axis,keepdims=True); return surface/(floor*strength+1e-20)
def ordered_surface(fields, arm, p):
    s=fields['s25' if p.get('spectral_profile')=='robust_notch_v1' else 's1']; l=fields[{.01:'l01',.1:'l1',1.:'l10'}.get(p.get('reciprocal_loading',.1),'l1')]
    if arm=='A': return normalize(normalize(s,2,1.),1,1.+p.get('reciprocal_loading',.1))
    if arm=='B': return normalize(normalize(l,1,1.+p.get('reciprocal_loading',.1)),2,1.)
    h=np.percentile(s, 99 if p.get('hdr_mode')=='clip_6db' else 99.8,axis=(1,2),keepdims=True); hdr=np.minimum(s,h)
    g=fields['known' if p.get('glrt_noise')=='known_noise' else 'unknown']
    if arm=='C': return normalize(np.minimum(normalize(hdr,2,1.),np.percentile(g,99,axis=(1,2),keepdims=True)),1,1.)
    return np.minimum(normalize(normalize(g,1,1.),2,1.),np.percentile(hdr,99,axis=(1,2),keepdims=True))
def logadd(a,b): return max(a,b)+math.log(math.exp(a-max(a,b))+math.exp(b-max(a,b)))
def hmm_score(values):
    """Two-state (present/absent) forward recursion over per-CPI evidence."""
    absent,present=0.,-4.
    for value,baseline in values:
        e=max(-8.,min(8.,math.log(max(float(value),1e-30)/max(float(baseline),1e-30))))
        next_absent=logadd(absent+math.log(.98),present+math.log(.20))
        next_present=logadd(absent+math.log(.02)+e,present+math.log(.80)+e)
        absent,present=next_absent,next_present
    return present-absent
def tbd_hmm(surface, fields, trajectory, stack, frame_indices=None):
    doppler,delay=fields['doppler'],np.arange(surface.shape[2])*LOADED.C_KM_S/LOADED.SAMPLE_RATE
    rows=[int(np.argmin(abs(doppler-x['doppler_hz'])) )for x in trajectory[:len(surface)]]; cols=[int(np.argmin(abs(delay-x['delay_km']))) for x in trajectory[:len(surface)]]
    offsets=[(r,c) for r in range(-min(rows),len(doppler)-max(rows),8) for c in range(-min(cols),len(delay)-max(cols),8) if abs(r)>2 or abs(c)>2]
    indices=range(len(surface)) if frame_indices is None else frame_indices
    def path(ro,co):
        vals=[]
        for frame in indices:
            r,c=rows[frame],cols[frame]
            cell=surface[frame,r+ro,c+co]; base=np.median(surface[frame,max(0,r+ro-3):r+ro+4,max(0,c+co-3):c+co+4]); vals.append((cell,base))
        return hmm_score(vals)
    target=path(0,0); controls=[path(r,c) for r,c in offsets]
    return target,controls
def localized_indices(job, count, width):
    t=job.get('timing',{})
    if t.get('status')!='available': raise ValueError('time-localized arm requires an available provenance anchor')
    center=int((t['anchor_ms']-t['window_start_ms'])//t['cpi_ms'])
    if not 0 <= center < count: raise ValueError('provenance anchor maps outside retained CPI sequence')
    radius=(width-1)//2
    return list(range(max(0,center-radius),min(count,center+radius+1)))
def vector(method, parameter=None):
    # Native primitive artifacts retain raw target and each translated path.
    if 'methods' in method and parameter is None: # noisy
        item=method['methods']['glrt_unknown_noise']; candidates=method['candidate_scores_ranked']; key='glrt_unknown_noise'
        return item['target_score'], [x['scores'][key] for x in candidates if x['kind']!='target']
    item=method['scores'][str(parameter)]
    return item['target'], [10**(x/10) for x in item['control_log_ratio_db']]
def empirical(target, controls):
    return (1+sum(x>=target for x in controls))/(1+len(controls))
def combine(parts, policy):
    n=min(len(x[1]) for x in parts); target=[empirical(a,b) for a,b in parts]
    ctls=[]
    for i in range(n):
        ps=[empirical(part[1][i],part[1]) for part in parts]
        ctls.append(max(ps) if policy=='max' else max(ps)) # conjunction: one calibrated p, never a sum
    return max(target), ctls
def spatial_vector(job, method):
    """Return the native complex-cutout vector, never a summary margin."""
    root=Path(os.environ.get('VECTORWARP_SPATIAL_RESULTS',''))
    path=root/(job['job_id']+'-result.json')
    if not root or not path.exists(): raise ValueError('spatial score-vector cache unavailable')
    package=json.loads(path.read_text())['gain_treatments']['inverse_hardware_gain']
    vector=package.get('score_vectors',{}).get(method)
    if not vector or not vector.get('controls'):
        raise ValueError('native spatial score vector unavailable for '+method)
    return vector
def spatial(job, directory, method):
    vector=spatial_vector(job,method)
    return vector['target']['raw_score'], [x['raw_score'] for x in vector['controls']]
def late_spatial_sparse(job, directory, fusion):
    """Fuse aligned native paths once; correlated evidence is never summed."""
    spatial_v,sparse_v=spatial_vector(job,'spatial_glrt'),spatial_vector(job,'sparse_beam')
    def key(x): return x['doppler_offset_bins'],x['delay_offset_bins']
    sparse_controls={key(x):x['raw_score'] for x in sparse_v['controls']}
    common=[(x,sparse_controls[key(x)]) for x in spatial_v['controls'] if key(x) in sparse_controls]
    if not common: raise ValueError('spatial/sparse vectors have no aligned translated controls')
    # Each component is transformed to its own empirical tail probability;
    # max is a conservative union statistic calibrated against that same
    # max on every matched control. conditional_residual deliberately uses
    # the residual evidence only after the spatial path is below its median.
    a,b=spatial_v['target']['raw_score'],sparse_v['target']['raw_score']
    ac=[x['raw_score'] for x,_ in common]; bc=[x for _,x in common]
    pa,pb=empirical(a,ac),empirical(b,bc)
    def fused(x,y):
        px,py=empirical(x,ac),empirical(y,bc)
        return max(-math.log10(px),-math.log10(py)) if fusion=='max_calibrated' else (-math.log10(px)+max(0.,-math.log10(py)-statistics.median([-math.log10(empirical(v,bc)) for v in bc])))
    return fused(a,b),[fused(item['raw_score'],y) for item,y in common]
def phasors(recording, frames=20):
    values=[]
    for i, iq in enumerate(LOADED.frames(recording, int(.5*LOADED.SAMPLE_RATE))):
        if i>=frames: break
        # Measured direct-path cross-channel phasor, normalized per CPI.
        x=np.vdot(iq[0],iq[1]); values.append([float(x.real/abs(x)),float(x.imag/abs(x))] if abs(x) else [0.,0.])
    return values
def cached_phasors(recording, directory):
    return cached(directory/'direct-phasors.json',
                  lambda:{'values':phasors(recording)})['values']
def score(request):
    job, trial= request['job'],request['trial']; directory=Path(request['cache_dir']); directory.mkdir(parents=True,exist_ok=True)
    loaded,spectral,noisy=waveform(job,directory)
    raw=loaded['recording']
    arm=trial['arm']; p=trial['parameters']
    l=vector(loaded,p.get('reciprocal_loading',.1)); s=vector(spectral,.25 if p.get('spectral_profile')=='robust_notch_v1' else 1.) ; n=vector(noisy)
    if arm in ('A','B','C','D','I'):
        fields=materialized(job,directory)
        if arm=='I':
            surface=ordered_surface(fields,'A',p); indices=localized_indices(job,len(surface),p['local_window_cpis'])
            target,controls=tbd_hmm(surface,fields,job['trajectory'],'local',indices)
        else: target,controls=tbd_hmm(ordered_surface(fields,arm,p),fields,job['trajectory'],p.get('tbd_hmm','hmm'))
    elif arm in ('E','F','G'): target, controls=spatial(job,directory,'post_doppler_stap' if arm=='E' else 'spatial_glrt')
    else:
        target, controls=late_spatial_sparse(job,directory,p['fusion'])
    rank=1+sum(x>=target for x in controls)
    result={'raw_target_score':float(target),'control_scores':[float(x) for x in controls],'rank':rank,
            'score_domain':'natural_log_likelihood' if arm in ('A','B','C','D','I') else 'positive_power',
            'direct_path_phasors':cached_phasors(raw,directory),
            'provenance':{'raw_iq':raw,'translated_controls':len(controls),'arm':arm,
                          'bandwidth_note':'2.4 MHz retained slice; not a full 6 MHz ATSC reconstruction'}}
    # Keep spatial diagnostics out of manifold-independent arms.  Spatial
    # arms retain the fixed before/after comparison for their later rerun.
    if arm in ('E','F','G','H'):
        before,_=vector(loaded,.01); after,_=spatial(job,directory,'spatial_glrt')
        result.update(coherent_before_spatial=float(before),coherent_after_spatial=float(after))
    return result
def main():
    p=argparse.ArgumentParser(); p.add_argument('--request',required=True,type=Path); p.add_argument('--output',required=True,type=Path); a=p.parse_args()
    atomic(a.output,score(json.loads(a.request.read_text())))
if __name__=='__main__': main()
