'use strict';

// Two independent bistatic ranges constrain a curve, not a 3-D point. This
// module retains that curve and never supplies an operational track or height.
const dot = (a, b) => a.reduce((sum, x, i) => sum + x * b[i], 0);
const norm = a => Math.hypot(...a);
const add = (a, b) => a.map((x, i) => x + b[i]);
const sub = (a, b) => a.map((x, i) => x - b[i]);
const scale = (a, s) => a.map(x => x * s);
const cross = (a, b) => [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]];
const unit = a => scale(a, 1 / norm(a));
const C = 299792.458;

function gradient(point, transmitter) {
  const offset = sub(point, transmitter);
  if (norm(point) < 1e-8 || norm(offset) < 1e-8) return null;
  return add(unit(point), unit(offset));
}

// With r=|p| and s_i=|t_i|+excess_i, each ellipsoid satisfies
// t_i.p-s_i*r=(|t_i|²-s_i²)/2. Eliminate r to obtain a plane.
// Transform the first ellipsoid to a unit sphere; its plane section is a
// circle. Transform back to retain the complete exact intersection ellipse.
function intersectionEllipse(first, second) {
  const t = first.transmitter, other = second.transmitter;
  const s = norm(t) + first.excessKm, s2 = norm(other) + second.excessKm;
  if (!(first.excessKm > 0 && second.excessKm > 0 && norm(t) > 0)) return null;
  const normal = sub(scale(t, 1/s), scale(other, 1/s2));
  const rhs = (dot(t,t)-s*s)/(2*s) - (dot(other,other)-s2*s2)/(2*s2);
  const major = s/2, minor = Math.sqrt(s*s-dot(t,t))/2, axis = unit(t);
  const transform = v => add(scale(v, minor), scale(axis, (major-minor)*dot(axis,v)));
  const center = scale(t,.5), w = transform(normal), w2 = dot(w,w);
  if (!(w2 > 1e-16)) return null;
  const offset = rhs-dot(normal,center), ratio = offset*offset/w2;
  if (!(ratio < 1-1e-12)) return null; // Tangency has singular local uncertainty.
  const n = unit(w), reference = Math.abs(n[0]) < .8 ? [1,0,0] : [0,1,0];
  const u = unit(cross(n,reference)), v = cross(n,u), radius = Math.sqrt(1-ratio);
  const origin = add(center,transform(scale(w,offset/w2)));
  const cosine = transform(scale(u,radius)), sine = transform(scale(v,radius));
  return {center:origin, cosine, sine,
    point:theta => add(origin,add(scale(cosine,Math.cos(theta)),scale(sine,Math.sin(theta))))};
}

// A 3-D covariance would be singular. Only the two observable normal
// directions have finite local information; the entire contour carries the
// unresolved tangent direction. Never report the pseudoinverse as a full fix.
function normalUncertainty(point, pair) {
  const rows = pair.map(m => gradient(point,m.transmitter));
  if (rows.some(x => !x)) return null;
  const weighted = rows.map((row,i) => scale(row,1/pair[i].sigmaKm));
  const aa=dot(weighted[0],weighted[0]), ab=dot(weighted[0],weighted[1]), bb=dot(weighted[1],weighted[1]);
  const determinant = aa*bb-ab*ab;
  const maximum = (aa+bb+Math.hypot(aa-bb,2*ab))/2;
  const minimum = determinant/maximum;
  if (!(minimum > 1e-10)) return null;
  return {sigmaKm:1/Math.sqrt(minimum), condition:Math.sqrt(maximum/minimum)};
}

// Doppler supplies two projections of velocity, not its third component.
// Minimize speed over the allowed two-dimensional range-rate rectangle. If
// even this minimum exceeds the aircraft speed envelope, no 3-D velocity can
// support the sample. Position/sampling error widens the projection bounds.
function minimumSpeed(point,pair,cpi,padding,maxSpeedKmS) {
  const rows=pair.map(m=>gradient(point,m.transmitter));
  if(rows.some(row=>!row)) return 0;
  const a=dot(rows[0],rows[0]),b=dot(rows[0],rows[1]),d=dot(rows[1],rows[1]),det=a*d-b*b;
  if(!(det>1e-12)) return 0;
  const bounds=pair.map(m=>{
    const near=Math.min(norm(point),norm(sub(point,m.transmitter)))-padding;
    const error=C/m.carrierHz/cpi+(near>0?4*maxSpeedKmS*padding/near:4*maxSpeedKmS);
    return [m.rangeRateKmS-error,m.rangeRateKmS+error];
  });
  const clamp=(x,interval)=>Math.max(interval[0],Math.min(interval[1],x));
  const trials=[[clamp(0,bounds[0]),clamp(0,bounds[1])]];
  for(const x of bounds[0]) trials.push([x,clamp(b*x/a,bounds[1])]);
  for(const y of bounds[1]) trials.push([clamp(b*y/d,bounds[0]),y]);
  return Math.sqrt(Math.max(0,Math.min(...trials.map(([x,y])=>(d*x*x-2*b*x*y+a*y*y)/det))));
}

function circularRuns(samples) {
  if (samples.every(Boolean)) return [samples];
  const gap = samples.findIndex(x => !x), runs = []; let run = [];
  for (let step=1; step<=samples.length; step++) {
    const value = samples[(gap+step)%samples.length];
    if (value) run.push(value);
    else if (run.length) { runs.push(run); run=[]; }
  }
  if (run.length) runs.push(run);
  return runs;
}

const source = m => ({illuminator:m.lane, track_id:m.trackId, transmitter_site:m.transmitterSite,
  carrier_frequency_hz:m.carrierHz, sample_rate_hz:m.sampleRateHz,
  excess_path_km:m.excessKm, bistatic_range_rate_km_s:m.rangeRateKmS,
  range_sigma_km:m.sigmaKm, provenance:m.provenance, source_false_alarm:m.falseAlarm});

function rateGate(a,b,cpi) { return Math.max(C/a.carrierHz,C/b.carrierHz)/cpi; }

function siteObservations(group,cpi) {
  const observations=[];
  for (const measurement of group.measurements) {
    // Only effectively identical range observations are collapsed. Near but
    // conflicting carrier returns remain separate association hypotheses.
    const prior=observations.find(item =>
      norm(sub(item.measurement.transmitter,measurement.transmitter)) < 1e-6 &&
      Math.abs(item.measurement.excessKm-measurement.excessKm) < .01*Math.min(item.measurement.sigmaKm,measurement.sigmaKm) &&
      Math.abs(item.measurement.rangeRateKmS-measurement.rangeRateKmS) <= rateGate(item.measurement,measurement,cpi));
    if (prior) prior.sources.push(measurement);
    else observations.push({measurement,sources:[measurement]});
  }
  return observations;
}

function envelopeCandidates(groups, receiver, geometry, cohort, toGeographic, {cpiSeconds=.5,
  pairLimit=32, candidateLimit=32, totalSampleLimit=8192}={}) {
  const output={position_candidates:[], envelope_computation:{pair_evaluations:0,sampled_points:0,truncated:false},
    position_candidate_semantics:{model:'two-site-range-intersection-altitude-envelope',
      unique_3d_position:false, altitude_inferred:false, operational_position:false,
      adsb_used_by_solver:false, terrain_constraint_used:false, bearing_constraint_used:false,
      qualification:'diagnostic; nominal-range contour with uncalibrated local noise tube; live false-association qualification required'}};
  if (groups.length!==2 || !(geometry.maximumAltitudeM>geometry.minimumAltitudeM)) return output;
  const cpi=Math.max(.001,cpiSeconds), sites=groups.map(g=>siteObservations(g,cpi));
  outer: for (const first of sites[0]) for (const second of sites[1]) {
    const computation=output.envelope_computation;
    if (computation.pair_evaluations>=pairLimit || output.position_candidates.length>=candidateLimit) {
      computation.truncated=true; break outer;
    }
    computation.pair_evaluations++;
    const pair=[first.measurement,second.measurement], curve=intersectionEllipse(...pair);
    if (!curve) continue;
    const derivativeBound=norm(curve.cosine)+norm(curve.sine);
    const count=Math.min(720,Math.max(128,Math.ceil(Math.PI*derivativeBound/.1)));
    if (computation.sampled_points+count>totalSampleLimit) { computation.truncated=true; break outer; }
    computation.sampled_points+=count;
    // Every exact contour point is <= this distance from one sample. This
    // explicit pad also keeps narrow altitude-envelope crossings represented.
    const cover=derivativeBound*Math.PI/count;
    const samples=Array.from({length:count},(_,index)=>{
      const theta=index*2*Math.PI/count, point=curve.point(theta), geographic=toGeographic(receiver,point);
      const uncertainty=normalUncertainty(point,pair);
      if (!uncertainty) return null;
      const padding=3*uncertainty.sigmaKm+cover;
      if (geographic.altitude+padding*1000<geometry.minimumAltitudeM ||
          geographic.altitude-padding*1000>geometry.maximumAltitudeM ||
          norm(point)-padding>geometry.maximumReceiverRangeKm) return null;
      const speed=minimumSpeed(point,pair,cpi,padding,geometry.maximumSpeedMps/1000);
      if(speed>geometry.maximumSpeedMps/1000) return null;
      return {...geographic,point,theta,normal_sigma_km:uncertainty.sigmaKm,
        jacobian_condition:uncertainty.condition,minimum_speed_m_s:speed*1000};
    });
    const runs=circularRuns(samples);
    for (const run of runs) {
      if (output.position_candidates.length>=candidateLimit) { computation.truncated=true; break outer; }
      const representative=run[Math.floor(run.length/2)];
      const normalSigma=Math.max(...run.map(p=>p.normal_sigma_km));
      const radius=Math.max(...run.map(p=>norm(sub(p.point,representative.point))))+3*normalSigma+cover;
      const corroborating=[];
      for (let siteIndex=0;siteIndex<2;siteIndex++) for (const measurement of groups[siteIndex].measurements) {
        const selected=pair[siteIndex];
        // Same-site RF evidence must agree in path and wavelength-normalized
        // Doppler. It adds provenance, never geometric rank or sqrt(N) gain.
        if (Math.abs(measurement.excessKm-selected.excessKm)<=3*Math.hypot(measurement.sigmaKm,selected.sigmaKm)+
              2*norm(sub(measurement.transmitter,selected.transmitter)) &&
            Math.abs(measurement.rangeRateKmS-selected.rangeRateKmS)<=rateGate(measurement,selected,cpi))
          corroborating.push(source(measurement));
      }
      const contributors=pair.map(source), signature=contributors.map(m=>`${m.illuminator}:${m.track_id}`).sort().join('|');
      output.position_candidates.push({latitude:representative.latitude,longitude:representative.longitude,altitude:null,
        representative_altitude_m:representative.altitude, representative_is_fix:false,
        altitude_model:'wgs84-altitude-envelope-unresolved',state:'ENVELOPE_CANDIDATE',resolved:false,
        operational_position:false,geometric_support:2,support:2,lane_support:new Set(corroborating.map(m=>m.illuminator)).size,
        contributors,corroborating_contributors:corroborating,signature,
        branch_index:runs.indexOf(run),branch_count:runs.length,
        position_envelope:{kind:'sampled-three-dimensional-curve-with-local-noise-tube',
          closed:run.length===count,
          altitude_datum:'WGS84-ellipsoid',minimum_altitude_m:geometry.minimumAltitudeM,maximum_altitude_m:geometry.maximumAltitudeM,
          maximum_speed_m_s:geometry.maximumSpeedMps,
          horizontal_radius_km:radius,normal_sigma_km:normalSigma,sampling_cover_radius_km:cover,
          calibrated:false,confidence_level:null,unresolved_dimensions:1,
          vertices:run.map(({point,...p})=>({...p,local_east_km:point[0],local_north_km:point[1],local_up_km:point[2]})),
          exact_contour_enu_km:{center:curve.center,cosine:curve.cosine,sine:curve.sine}},
        diagnostic_reasons:['two-site-altitude-continuum','unqualified-range-noise-and-false-association',
          ...(!cohort.capture_epoch_verified?['unverified-capture-epoch']:[])]});
    }
  }
  if (output.envelope_computation.truncated) for (const c of output.position_candidates)
    c.diagnostic_reasons.push('incomplete-bounded-search');
  for(const candidate of output.position_candidates) {
    const keys=new Set(candidate.contributors.map(c=>`${c.illuminator}:${c.track_id}`));
    const rivals=output.position_candidates.filter(other=>other!==candidate &&
      other.contributors.some(c=>keys.has(`${c.illuminator}:${c.track_id}`)));
    candidate.branch_count=1+rivals.length;
    if(rivals.length) candidate.diagnostic_reasons.push('mutually-exclusive-contributor-branches');
  }
  return output;
}

const angularDifference=(a,b)=>Math.abs(((a-b+540)%360)-180);
const bearingOf=p=>(Math.atan2(p.local_east_km,p.local_north_km)*180/Math.PI+360)%360;

// This contract deliberately cannot accept a bare /api/bearing row. A future
// cross-node adapter must establish radar-only association, common receiver,
// CPI timing and a validated COMPLETE angular candidate set first. In
// particular, choosing a lobe with ADS-B does not meet this contract.
function constrainWithBearing(candidates,constraints,receiver,cohort,geometry,enabled=false) {
  const status={enabled,accepted_constraints:0,rejected_constraints:0,
    reason:enabled?'No qualified radar bearing constraint supplied.':'Bearing constraint path is disabled.',
    operational_position:false};
  if (!enabled) return {candidates,status};
  const valid=(Array.isArray(constraints)?constraints:[]).filter(constraint=>{
    const ok=constraint?.schema==='vectorwarp-radar-bearing-constraint-v1' &&
      constraint.radar_only===true && constraint.complete_candidate_set===true &&
      constraint.cohort_key===cohort.key && constraint.timestamp===cohort.timestamp &&
      ['latitude','longitude','altitude'].every(key=>constraint.receiver?.[key]===receiver[key]) &&
      constraint.association?.method==='radar-delay-doppler' &&
      constraint.association?.unique===true && Array.isArray(constraint.association.contributor_keys) &&
      constraint.association.contributor_keys.length>=2 &&
      constraint.calibration?.validated===true && typeof constraint.calibration.id==='string' &&
      constraint.calibration.id.length>0 && typeof constraint.calibration.artifact_sha256==='string' &&
      /^[a-f0-9]{64}$/.test(constraint.calibration.artifact_sha256) &&
      Number.isFinite(constraint.calibration.maximum_error_deg) &&
      constraint.calibration.maximum_error_deg>0 && constraint.calibration.maximum_error_deg<=30 &&
      Array.isArray(constraint.candidates) && constraint.candidates.length>0 && constraint.candidates.length<=32 &&
      constraint.candidates.every(c=>Number.isFinite(c.bearing_deg_true) && c.bearing_deg_true>=0 && c.bearing_deg_true<360 &&
        Number.isFinite(c.half_width_deg) && c.half_width_deg>=constraint.calibration.maximum_error_deg && c.half_width_deg<=90);
    if(!ok) status.rejected_constraints++;return ok;
  });
  const bounded=[];
  for(const candidate of candidates) {
    const keys=candidate.contributors.map(c=>`${c.illuminator}:${c.track_id}`);
    const matching=valid.filter(c=>keys.every(key=>c.association.contributor_keys.includes(key)));
    // Conflicting/duplicated association constraints must not silently be
    // intersected as independent evidence or resolved by arbitrary order.
    if(matching.length!==1) { bounded.push(candidate);continue; }
    const constraint=matching[0], envelope=candidate.position_envelope;
    const kept=envelope.vertices.map(p=>{
      const range=Math.hypot(p.local_east_km,p.local_north_km);
      const padKm=3*p.normal_sigma_km+envelope.sampling_cover_radius_km;
      const padDeg=range>padKm?Math.asin(padKm/range)*180/Math.PI:180;
      return constraint.candidates.some(c=>angularDifference(bearingOf(p),c.bearing_deg_true)<=c.half_width_deg+padDeg)?p:null;
    });
    // The original altitude-clipped arc need not be closed. Adding a null gap
    // prevents falsely joining its end points when the bearing splits it.
    const runs=circularRuns([...kept,null]);
    status.accepted_constraints++;
    for(const vertices of runs) {
      const representative=vertices[Math.floor(vertices.length/2)];
      const radius=Math.max(...vertices.map(p=>Math.hypot(p.local_east_km-representative.local_east_km,
        p.local_north_km-representative.local_north_km)))+3*Math.max(...vertices.map(p=>p.normal_sigma_km))+
        envelope.sampling_cover_radius_km;
      bounded.push({...candidate,latitude:representative.latitude,longitude:representative.longitude,
        representative_altitude_m:representative.altitude,
        position_envelope:{...envelope,vertices,closed:false,horizontal_radius_km:radius,
          normal_sigma_km:Math.max(...vertices.map(p=>p.normal_sigma_km))},
        bearing_constraint:{calibration_id:constraint.calibration.id,artifact_sha256:constraint.calibration.artifact_sha256,
          candidate_intervals:constraint.candidates,radar_only:true,complete_candidate_set:true,
          branch_resolved:false,horizontally_bounded:false},
        diagnostic_reasons:[...candidate.diagnostic_reasons,'bearing-conditioned-estimated-envelope']});
    }
  }
  // Qualification is geometric and conditional, never operational validation.
  // A point marker is permitted only when no contributor-sharing branch
  // remains and the complete retained horizontal envelope meets the bound.
  for(const candidate of bounded.filter(c=>c.bearing_constraint)) {
    const keys=new Set(candidate.contributors.map(c=>`${c.illuminator}:${c.track_id}`));
    const rivals=bounded.filter(other=>other!==candidate && other.contributors.some(c=>keys.has(`${c.illuminator}:${c.track_id}`)));
    const qualified=rivals.length===0 && candidate.position_envelope.horizontal_radius_km<=geometry.maximumHorizontalSigmaKm &&
      cohort.capture_epoch_verified===true && !candidate.diagnostic_reasons.includes('incomplete-bounded-search');
    candidate.bearing_constraint.branch_resolved=rivals.length===0;
    candidate.bearing_constraint.horizontally_bounded=qualified;
    candidate.position_qualification=qualified?'bounded-horizontal-bearing-and-two-site-candidate':'ambiguous-or-broad-bearing-conditioned-envelope';
    candidate.branch_count=1+rivals.length;
    candidate.diagnostic_reasons=candidate.diagnostic_reasons.filter(reason=>reason!=='mutually-exclusive-contributor-branches');
    if(rivals.length) candidate.diagnostic_reasons.push('mutually-exclusive-contributor-branches');
  }
  if(status.accepted_constraints) status.reason='Radar bearing conditioned the contour; altitude and uncalibrated range-noise limits remain explicit.';
  return {candidates:bounded,status};
}

// Causal temporal support is a necessary range/Doppler/motion consistency
// check. It does not select an altitude or remove competing feasible branches.
class EnvelopePersistence {
  constructor({required=3,maxSpeedKmS=.45,maxAccelerationKmS2=.08,maximumGapSeconds=10,cpiSeconds=.5}={}) {
    Object.assign(this,{required,maxSpeedKmS,maxAccelerationKmS2,maximumGapSeconds,cpiSeconds}); this.reset();
  }
  reset() { this.previous=[]; this.timestamp=null; this.nextId=1; }
  update(candidates,timestamp,eligible) {
    const dt=(timestamp-this.timestamp)/1000, old=this.previous;
    const next=candidates.map(candidate=>{
      const matches=eligible && dt>0 && dt<=this.maximumGapSeconds ? old.filter(prior=>{
        if (!candidate.contributors.every(current=>{
          const previous=prior.contributors.find(m=>m.transmitter_site===current.transmitter_site);
          if (!previous) return false;
          const sigma=3*Math.hypot(current.range_sigma_km,previous.range_sigma_km);
          const rangeError=Math.abs(current.excess_path_km-previous.excess_path_km-
            .5*(current.bistatic_range_rate_km_s+previous.bistatic_range_rate_km_s)*dt);
          const rateError=Math.abs(current.bistatic_range_rate_km_s-previous.bistatic_range_rate_km_s);
          const rateResolution=Math.max(C/current.carrier_frequency_hz,C/previous.carrier_frequency_hz)/this.cpiSeconds;
          return rangeError<=sigma+this.maxAccelerationKmS2*dt*dt &&
            rateError<=rateResolution+2*this.maxAccelerationKmS2*dt;
        })) return false;
        // Enclosing balls yield a conservative necessary speed check. Their
        // overlap cannot establish a unique correspondence or target identity.
        const a=candidate.position_envelope.vertices, b=prior.position_envelope.vertices;
        const center=rows=>rows[Math.floor(rows.length/2)];
        const p=center(a),q=center(b);
        return Math.hypot(p.local_east_km-q.local_east_km,p.local_north_km-q.local_north_km,p.local_up_km-q.local_up_km)<=
          candidate.position_envelope.horizontal_radius_km+prior.position_envelope.horizontal_radius_km+this.maxSpeedKmS*dt;
      }) : [];
      const supporting=matches.length===1 ? matches[0].supporting_cpis+1 : 1;
      const supported=eligible && supporting>=this.required;
      return {...candidate,id:matches.length===1?matches[0].id:`E${String(this.nextId++).padStart(4,'0')}`,
        timestamp,state:supported?'ENVELOPE_SUPPORTED':'ENVELOPE_CANDIDATE',supporting_cpis:supporting,
        temporal_support:{causal:true,independent_cpi_votes:supporting,required_cpis:this.required,
          branch_resolved:false,unique_predecessor:matches.length===1,
          method:'range-rate-consistency-and-conservative-maximum-speed-envelope',
          qualification:'necessary consistency only; overlapping CPI evidence and ambiguous predecessors cannot confirm'}};
    });
    // Splits sharing a predecessor remain separate branches; persistence never
    // counts them as independent confirmation or silently chooses one.
    const counts=new Map(); for (const c of next) counts.set(c.id,(counts.get(c.id)||0)+1);
    for (const c of next) if (counts.get(c.id)>1) { c.id=`E${String(this.nextId++).padStart(4,'0')}`;
      c.supporting_cpis=1;c.state='ENVELOPE_CANDIDATE';c.temporal_support.independent_cpi_votes=1;
      c.temporal_support.unique_predecessor=false; }
    this.previous=eligible?next:[];this.timestamp=timestamp;return next;
  }
}

module.exports={intersectionEllipse,normalUncertainty,minimumSpeed,envelopeCandidates,constrainWithBearing,EnvelopePersistence};
