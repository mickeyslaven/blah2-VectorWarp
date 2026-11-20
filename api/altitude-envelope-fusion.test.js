'use strict';
const assert=require('assert/strict');
const {intersectionEllipse}=require('./altitude-envelope-fusion');
const {localPoint3d,excessPath3d,fuseTrackCohort,MultiIlluminatorTrackFuser}=require('./multi-illuminator-track-fusion');
const C=299792.458;
// Exact currently configured site coordinates; all input returns below are
// synthetic radar measurements, never a claim about observed field accuracy.
const receiver={latitude:40.4421967375442,longitude:-80.05656014497772,altitude:349.65};
const towerA={latitude:40.495278,longitude:-80.004444,altitude:691.8};
const towerB={latitude:40.46325,longitude:-80.0043056,altitude:615};
const definitions=[['TX-A',509e6,towerA],['TX-B',515e6,towerA],['TX-C',527e6,towerB]].map(
  ([id,frequency_hz,transmitter])=>({id,frequency_hz,transmitter,enabled:true,output_sample_rate_hz:6.144e6}));
const config={location:{rx:receiver},process:{data:{cpi:1},multi_illuminator:{enabled:true,illuminators:definitions},
  tracker:{initiate:{M:3,N:5}}}};
const target={latitude:40.48,longitude:-80.12,altitude:5000};
const base=localPoint3d(receiver,target), velocity=[.08,.03,.005];
function frames(index=0,configuration=config,point=base,v=velocity) {
  return configuration.process.multi_illuminator.illuminators.map(d=>{
    const tx=localPoint3d(receiver,d.transmitter), p=point.map((x,i)=>x+v[i]*index);
    const dtx=p.map((x,i)=>x-tx[i]);
    const rate=p.reduce((sum,x,i)=>sum+(x/Math.hypot(...p)+dtx[i]/Math.hypot(...dtx))*v[i],0);
    return {id:d.id,frequency_hz:d.frequency_hz,sample_rate_hz:d.output_sample_rate_hz,
      capture_sample_position_available:true,capture_epoch:5,capture_first_input_sample:1000+index*1000,
      capture_end_input_sample:2000+index*1000,timestamp:100000+index*1000,
      tracks:{timestamp:100000+index*1000,data:[{id:`${d.id}-track`,state:'ACTIVE',n:10,
        delay:excessPath3d(p,tx)/(C/d.output_sample_rate_hz),doppler:-rate*d.frequency_hz/C}]}};
  });
}
const result=fuseTrackCohort(frames(),config);
assert.equal(result.independent_transmitter_sites,2);
assert.ok(result.position_candidates.length>0,'Two sites produce usable geographic envelopes');
assert.equal(result.operational_localization,false);
assert.equal(result.hypotheses.length,0);
for(const candidate of result.position_candidates) {
  assert.equal(candidate.altitude,null);assert.equal(candidate.resolved,false);
  assert.equal(candidate.geometric_support,2);assert.equal(candidate.lane_support,3);
  assert.equal(candidate.position_envelope.confidence_level,null);
  assert.equal(candidate.position_envelope.unresolved_dimensions,1);
  assert.ok(Number.isFinite(candidate.latitude));assert.ok(Number.isFinite(candidate.longitude));
  const curve=candidate.position_envelope.exact_contour_enu_km;
  for(let i=0;i<40;i++) {
    const angle=2*Math.PI*i/40,p=curve.center.map((x,j)=>x+curve.cosine[j]*Math.cos(angle)+curve.sine[j]*Math.sin(angle));
    for(const d of [definitions[0],definitions[2]]) {
      const tx=localPoint3d(receiver,d.transmitter);
      assert.ok(Math.abs(excessPath3d(p,tx)-excessPath3d(base,tx))<1e-9,'Analytic contour satisfies both exact 3-D ranges');
    }
  }
}
const allVertices=result.position_candidates.flatMap(c=>c.position_envelope.vertices);
const truthDistance=Math.min(...allVertices.map(p=>Math.hypot(p.local_east_km-base[0],p.local_north_km-base[1],p.local_up_km-base[2])));
assert.ok(truthDistance<=Math.max(...result.position_candidates.map(c=>c.position_envelope.sampling_cover_radius_km)),
  'Sampling cover retains the actual target without assuming its altitude');
const duplicateConfig=structuredClone(config);
duplicateConfig.process.multi_illuminator.illuminators.push({...definitions[0],id:'HP-primary'});
const duplicate=fuseTrackCohort(frames(0,duplicateConfig),duplicateConfig);
assert.equal(duplicate.position_candidates.length,result.position_candidates.length);
assert.equal(duplicate.position_candidates[0].position_envelope.normal_sigma_km,
  result.position_candidates[0].position_envelope.normal_sigma_km,'Duplicate illumination never shrinks range uncertainty');

// A native bearing row alone has no calibration/CPI-association contract and
// cannot secretly collapse the range curve. Optional qualified angular bounds
// are explicitly tied to this target's radar contributors and capture cohort.
const bearingConfig=structuredClone(config);
bearingConfig.process.multi_illuminator.localization={bearingConstraintEnabled:true};
const nativeBearing={bearing_deg_true:270,uncertainty_deg:1,candidates:[{bearing_deg_true:270,support:1}]};
const refused=fuseTrackCohort(frames(),bearingConfig,{}, {bearingConstraints:[nativeBearing]});
assert.equal(refused.bearing_constraint_status.accepted_constraints,0);
assert.equal(refused.bearing_constraint_status.rejected_constraints,1);
assert.deepEqual(refused.position_candidates,result.position_candidates);
const angle=(Math.atan2(base[0],base[1])*180/Math.PI+360)%360;
const constraint={schema:'vectorwarp-radar-bearing-constraint-v1',radar_only:true,complete_candidate_set:true,
  cohort_key:result.key,timestamp:result.timestamp,receiver,
  association:{method:'radar-delay-doppler',unique:true,
    contributor_keys:result.position_candidates[0].contributors.map(c=>`${c.illuminator}:${c.track_id}`)},
  calibration:{validated:true,id:'synthetic-angular-bound-only',artifact_sha256:'a'.repeat(64),maximum_error_deg:.5},
  candidates:[{bearing_deg_true:angle,half_width_deg:.5}]};
const constrained=fuseTrackCohort(frames(),bearingConfig,{}, {bearingConstraints:[constraint]});
assert.ok(constrained.bearing_constraint_status.accepted_constraints>0);
assert.ok(constrained.position_candidates.length>0);
assert.ok(constrained.position_candidates.every(c=>c.bearing_constraint));
assert.ok(constrained.position_candidates.reduce((n,c)=>n+c.position_envelope.vertices.length,0)<allVertices.length);
assert.equal(constrained.operational_localization,false);
assert.ok(constrained.position_candidates.every(c=>c.altitude===null));
const qualified=constrained.position_candidates.filter(c=>c.bearing_constraint.horizontally_bounded);
for(const c of qualified) {
  assert.equal(c.branch_count,1);assert.ok(c.position_envelope.horizontal_radius_km<=2);
  assert.equal(c.position_qualification,'bounded-horizontal-bearing-and-two-site-candidate');
}
const permissiveBearingConfig=structuredClone(bearingConfig);
permissiveBearingConfig.process.multi_illuminator.localization.maximumHorizontalSigmaKm=20;
assert.ok(fuseTrackCohort(frames(),permissiveBearingConfig,{}, {bearingConstraints:[constraint]})
  .position_candidates.some(c=>c.bearing_constraint.horizontally_bounded),'Explicit horizontal bound gates geometric qualification');
const noOptIn=fuseTrackCohort(frames(),config,{}, {bearingConstraints:[constraint]});
assert.equal(noOptIn.bearing_constraint_status.accepted_constraints,0);
for(const mutate of [c=>{c.radar_only=false;},c=>{c.complete_candidate_set=false;},
  c=>{c.cohort_key='old';},c=>{c.receiver={...receiver,altitude:0};},
  c=>{c.association={...c.association,method:'adsb'};},c=>{c.calibration={...c.calibration,validated:false};}]) {
  const altered=structuredClone(constraint);mutate(altered);
  assert.equal(fuseTrackCohort(frames(),bearingConfig,{}, {bearingConstraints:[altered]})
    .bearing_constraint_status.accepted_constraints,0);
}
const bearingFuser=new MultiIlluminatorTrackFuser(bearingConfig);
assert.ok(bearingFuser.update(frames(),{bearingConstraints:[constraint]})
  .position_candidates.some(c=>c.bearing_constraint),'Optional bearing contract is wired through the public fuser');
const disagreement=frames();disagreement[1].tracks.data[0].doppler+=1000;
const disagreeResult=fuseTrackCohort(disagreement,config);
assert.ok(disagreeResult.position_candidates.every(c=>c.lane_support===2),
  'Same-site incompatible Doppler is a separate hypothesis, never corroboration');
const missing=frames();missing[2].tracks.data=[];
assert.equal(fuseTrackCohort(missing,config).position_candidates.length,0,'One physical site cannot produce geographic candidates');
const impossibleDoppler=frames();impossibleDoppler.forEach(f=>{f.tracks.data[0].doppler=1e6;});
assert.equal(fuseTrackCohort(impossibleDoppler,config).position_candidates.length,0,
  'Two instantaneous Doppler projections must permit a velocity inside the speed envelope');
const lowNoiseConfig=structuredClone(bearingConfig);
lowNoiseConfig.process.multi_illuminator.localization.rangeNoiseSigmaKm=.001;
assert.ok(fuseTrackCohort(frames(),lowNoiseConfig,{}, {bearingConstraints:[constraint]})
  .position_candidates.some(c=>c.bearing_constraint.horizontally_bounded),
  'Synthetic lower-noise ranges and qualified bearing can pass the default 2-km conditional geometric gate');
const bounded=fuseTrackCohort(frames(),config,{pairEvaluations:1,hypotheses:1});
assert.ok(bounded.position_candidates.length<=1);
const malformed=frames();malformed[1].timestamp++;
assert.equal(fuseTrackCohort(malformed,config).available,false);

const fuser=new MultiIlluminatorTrackFuser(config);let tracked;
for(let i=0;i<3;i++) tracked=fuser.update(frames(i));
assert.ok(tracked.position_candidates.some(c=>c.state==='ENVELOPE_SUPPORTED'),
  'Distinct causal range/Doppler-consistent CPIs can support an envelope');
assert.equal(tracked.tracks.length,0);assert.equal(tracked.operational_localization,false);
const replay=fuser.update(frames(2));
assert.equal(replay,tracked,'Duplicate CPI cannot increase temporal evidence');
const old=fuser.update(frames(1));assert.equal(old,tracked);
const receiptAt=fuser.lastReceiptAt, realNow=Date.now;
try {
  Date.now=()=>receiptAt+3000;
  assert.ok(fuser.update(frames(2)).position_candidates.every(c=>c.state==='ENVELOPE_STALE'),
    'Replaying the latest accepted CPI cannot revive stale envelope support');
  assert.ok(fuser.update(frames(1)).position_candidates.every(c=>c.state==='ENVELOPE_STALE'),
    'Replaying an older accepted CPI cannot revive stale envelope support');
  assert.equal(fuser.lastReceiptAt,receiptAt,'Duplicate evidence cannot refresh its receipt age');
} finally { Date.now=realNow; }
const stale=fuser.snapshot(fuser.lastReceiptAt+3000);
assert.ok(stale.position_candidates.every(c=>c.state==='ENVELOPE_STALE'));
assert.equal(fuser.snapshot(fuser.lastReceiptAt+11000).position_candidates.length,0);
const jump=frames(3);jump.forEach(f=>{f.tracks.data[0].delay+=1000;});
assert.ok(fuser.update(jump).position_candidates.every(c=>c.supporting_cpis===1),
  'Impossible path jumps cannot retain temporal support');
const epoch=frames();epoch.forEach(f=>{f.capture_epoch=6;});
assert.ok(fuser.update(epoch).position_candidates.every(c=>c.supporting_cpis===1));

const truthRuns=[[],[{lat:target.latitude,lon:target.longitude,alt_geom:1e6}],
  [{lat:-20,lon:30,alt_geom:0}]].map(truth=>{
  const f=new MultiIlluminatorTrackFuser({...config,truth:{adsb:{aircraft:truth}}});let r;
  for(let i=0;i<3;i++){const rows=frames(i);rows.forEach(row=>{row.adsb=truth;});r=f.update(rows);}
  return JSON.stringify(r);
});
assert.equal(truthRuns[0],truthRuns[1]);assert.equal(truthRuns[1],truthRuns[2]);

// Sampled nominal ranges cover targets on both sides of the site baseline and
// at different nonzero heights. No zero-altitude layer is silently substituted.
for(const latitude of [40.40,40.48,40.54]) for(const longitude of [-80.15,-79.99]) for(const altitude of [500,5000,12000]) {
  const point=localPoint3d(receiver,{latitude,longitude,altitude});
  const r=fuseTrackCohort(frames(0,config,point,[0,0,0]),config);
  const close=r.position_candidates.some(c=>c.position_envelope.vertices.some(p=>
    Math.hypot(p.local_east_km-point[0],p.local_north_km-point[1],p.local_up_km-point[2])<=
      c.position_envelope.sampling_cover_radius_km+1e-6));
  assert.ok(close,`Coverage at ${latitude},${longitude},${altitude}`);
}

assert.equal(intersectionEllipse({transmitter:[1,0,0],excessKm:2},
  {transmitter:[1,0,0],excessKm:2}),null,'Duplicate ellipsoids do not manufacture a finite contour');
console.log('Two-site geographic altitude-envelope, causal support, and truth-isolation tests passed.');
console.log(JSON.stringify({synthetic_live_geometry:{envelope_candidates:result.position_candidates.length,
  unconstrained_radius_km:result.position_candidates.map(c=>c.position_envelope.horizontal_radius_km),
  with_synthetic_half_degree_bearing_radius_km:constrained.position_candidates.map(c=>c.position_envelope.horizontal_radius_km),
  default_2km_bearing_gate_passed:qualified.length>0}}));
