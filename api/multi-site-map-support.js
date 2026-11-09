'use strict';

const {cohortIdentity, localPoint3d, geographicPoint3d, excessPath3d, tripleRoots, rawDetectionMeasurements} =
  require('./multi-illuminator-track-fusion');

const {gradient, sampleMap, siteGroups, enabledDefinitions} = require('./bistatic-map-sampling');
const {makeLikelihoodEvaluator, sampleTiming} = require('./cross-frequency-likelihood');

const C_KM_S = 299792.458;
const finite = value => typeof value === 'number' && Number.isFinite(value);
const dot = (a, b) => a.reduce((sum, value, index) => sum + value * b[index], 0);
const norm = value => Math.hypot(...value);
const sub = (a, b) => a.map((value, index) => value - b[index]);
const add = (a, b) => a.map((value, index) => value + b[index]);
const scale = (a, value) => a.map(item => item * value);

function solve2(a, b) {
  const determinant = a[0][0] * a[1][1] - a[0][1] * a[1][0];
  if (!finite(determinant) || Math.abs(determinant) < 1e-10) return null;
  return [(b[0] * a[1][1] - a[0][1] * b[1]) / determinant,
    (a[0][0] * b[1] - b[0] * a[1][0]) / determinant];
}

function solve3(matrix, values) {
  const [[a,b,c],[d,e,f],[g,h,i]] = matrix;
  const determinant = a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g);
  const size = Math.max(...matrix.flat().map(Math.abs));
  if (!finite(determinant) || Math.abs(determinant) < Math.max(1e-12, size ** 3 * 1e-10)) return null;
  const inverse = [[e*i-f*h,c*h-b*i,b*f-c*e], [f*g-d*i,a*i-c*g,c*d-a*f],
    [d*h-e*g,b*g-a*h,a*e-b*d]].map(row => row.map(value => value / determinant));
  return inverse.map(row => dot(row, values));
}

function measurements(frames, config, limitPerSite = 8) {
  const result = siteGroups(config).map(group => ({...group, measurements: []}));
  const byLane = new Map(result.flatMap(group => group.definitions.map(def => [def.id, {group, def}])));
  for (const frame of frames) {
    const lane = byLane.get(frame.id);
    if (!lane) return null;
    const site = lane.group.sites.get(frame.id);
    const rows = rawDetectionMeasurements(frame, lane.def, site);
    if (!rows || (frame.map && frame.map.timestamp !== frame.timestamp)) return null;
    for (const row of rows) lane.group.measurements.push({...row,
      transmitter: site.point, transmitterSite: site.identity,
      sigmaKm: Math.max(.01, row.delayResolutionKm / Math.sqrt(12))});
  }
  for (const group of result) group.measurements = group.measurements
    .sort((a,b) => b.snrDb - a.snrDb || a.lane.localeCompare(b.lane) || a.detectionIndex - b.detectionIndex)
    .slice(0, limitPerSite);
  return result;
}

function twoSiteRoots(first, second, upKm, maximumRangeKm = 300) {
  const roots = [];
  const seeds = [];
  for (const radius of [2,5,10,20,40,80,140]) for (let bearing = 0; bearing < 360; bearing += 30) {
    const angle = bearing * Math.PI / 180;
    seeds.push([Math.sin(angle) * radius, Math.cos(angle) * radius, upKm]);
  }
  for (const seed of seeds) {
    let point = [...seed], valid = true;
    for (let iteration = 0; iteration < 16; iteration += 1) {
      const residual = [excessPath3d(point, first.transmitter) - first.excessKm,
        excessPath3d(point, second.transmitter) - second.excessKm];
      const gradients = [gradient(point, first.transmitter), gradient(point, second.transmitter)];
      if (gradients.some(value => !value)) { valid = false; break; }
      const delta = solve2(gradients.map(value => value.slice(0,2)), residual);
      if (!delta) { valid = false; break; }
      point[0] -= delta[0]; point[1] -= delta[1]; point[2] = upKm;
      if (!point.every(finite) || norm(point) > maximumRangeKm * 2) { valid = false; break; }
      if (Math.hypot(...delta) < 1e-5) break;
    }
    if (!valid || norm(point) > maximumRangeKm) continue;
    const error = Math.max(Math.abs(excessPath3d(point, first.transmitter) - first.excessKm),
      Math.abs(excessPath3d(point, second.transmitter) - second.excessKm));
    if (error > Math.max(first.sigmaKm, second.sigmaKm) * 3) continue;
    if (!roots.some(item => norm(sub(item, point)) < .05)) roots.push(point);
  }
  return roots;
}

function velocityFor(point, selected, fixedVertical = null) {
  const rows = selected.map(item => gradient(point, item.transmitter));
  if (rows.some(value => !value)) return null;
  if (fixedVertical !== null && selected.length >= 2) {
    const a = rows.slice(0,2).map(row => row.slice(0,2));
    const b = selected.slice(0,2).map((item,index) => item.rangeRateKmS - rows[index][2] * fixedVertical);
    const horizontal = solve2(a,b);
    return horizontal ? [horizontal[0], horizontal[1], fixedVertical] : null;
  }
  const normal = Array.from({length:3}, () => [0,0,0]), values = [0,0,0];
  for (let index = 0; index < selected.length; index += 1) for (let row = 0; row < 3; row += 1) {
    values[row] += rows[index][row] * selected[index].rangeRateKmS;
    for (let column = 0; column < 3; column += 1)
      normal[row][column] += rows[index][row] * rows[index][column];
  }
  return solve3(normal, values);
}

function evaluateState(frames, config, point, velocity, groups = siteGroups(config), offsets = null) {
  const definitions = new Map(enabledDefinitions(config).map(item => [item.id,item]));
  const receiver = config.location.rx, scores = [];
  for (const frame of frames) {
    const definition = definitions.get(frame.id);
    if (!definition) continue;
    const transmitter = localPoint3d(receiver, definition.transmitter);
    const atLane = offsets ? add(point, scale(velocity, offsets.get(frame.id))) : point;
    const direction = gradient(atLane, transmitter);
    if (!direction) continue;
    const delayKm = excessPath3d(atLane, transmitter);
    const rangeRate = dot(direction, velocity);
    const dopplerHz = -rangeRate * Number(definition.frequency_hz) / C_KM_S;
    const sample = sampleMap(frame.map, delayKm, dopplerHz);
    if (sample) scores.push({illuminator: frame.id,
      transmitter_site: groups.find(group => group.definitions.some(item => item.id === frame.id))?.id,
      frequency_hz: definition.frequency_hz, ...sample});
  }
  // Descriptive rank only: lanes share a receiver, nearby cells and often a
  // transmitter. Neither their z scores nor their tail ranks are independent.
  const bySite = groups.map(group => scores.filter(row => row.transmitter_site === group.id));
  const siteMeans = bySite.filter(rows => rows.length).map(rows =>
    rows.reduce((sum, row) => sum + row.robust_z, 0) / rows.length);
  return {channels: scores, contributing_channels: scores.length,
    descriptive_rank: siteMeans.length ? siteMeans.reduce((a,b) => a+b,0) / siteMeans.length : null,
    strong_channels: scores.filter(item => item.empirical_upper_tail_p <= .05).length,
    calibration: 'local-annulus-shadow-unqualified', phase_coherent_sum: false,
    combination: 'unqualified-site-balanced-rank-not-a-likelihood'};

}

function hypotheses(frames, config, {measurementLimitPerSite = 8, evaluationLimit = 2048,
  altitudeLayersM = [500,1000,2000,4000,6000,8000,10000,12000]} = {}) {
  const groups = measurements(frames, config, measurementLimitPerSite);
  if (!groups) return {available:false, reason:'Invalid frame or detector provenance.', hypotheses:[]};
  const populated = groups.filter(group => group.measurements.length), receiver = config.location.rx;
  if (populated.length < 2) return {available:true, reason:'Fewer than two sites have detections.', hypotheses:[]};
  const candidates = []; let evaluations = 0, truncated = false;
  const addCandidate = (point, selected, altitudeModel) => {
    const velocity = velocityFor(point, selected, altitudeModel === 'fixed-altitude-layer' ? 0 : null);
    if (!velocity || norm(velocity) > .45) return;
    const geographic = geographicPoint3d(receiver, point);
    if (geographic.altitude < 0 || geographic.altitude > 20000 || norm(point) > 300) return;
    const evidence = evaluateState(frames, config, point, velocity, groups);
    const signature = selected.map(item => `${item.lane}:${item.detectionIndex}`).sort().join('|');
    if (candidates.some(item => norm(sub(item.point,point)) < .1 && norm(sub(item.velocity,velocity)) < .02)) return;
    candidates.push({point, velocity, geographic, evidence, signature,
      independent_transmitter_sites: new Set(selected.map(item => item.transmitterSite)).size,
      contributors: selected.map(item => ({illuminator:item.lane, detection_index:item.detectionIndex,
        transmitter_site:item.transmitterSite, excess_path_km:item.excessKm,
        bistatic_range_rate_km_s:item.rangeRateKmS, snr_db:item.snrDb,
        provenance:item.provenance, source_false_alarm:item.falseAlarm})), altitude_model:altitudeModel});
  };
  if (populated.length >= 3) {
    outer: for (let a=0;a<populated.length-2;a++) for (let b=a+1;b<populated.length-1;b++)
      for (let c=b+1;c<populated.length;c++) for (const first of populated[a].measurements)
        for (const second of populated[b].measurements) for (const third of populated[c].measurements) {
          if (++evaluations > evaluationLimit) { truncated=true; break outer; }
          for (const point of tripleRoots([first,second,third])) addCandidate(point,[first,second,third],
            'three-dimensional-three-site');
        }
  } else {
    outer2: for (const first of populated[0].measurements) for (const second of populated[1].measurements)
      for (const altitudeM of altitudeLayersM) {
        if (++evaluations > evaluationLimit) { truncated=true; break outer2; }
        const upKm = (altitudeM - Number(receiver.altitude)) / 1000;
        for (const point of twoSiteRoots(first,second,upKm)) addCandidate(point,[first,second],
          'fixed-altitude-layer');
      }
  }
  candidates.sort((a,b) => (b.evidence.descriptive_rank ?? -Infinity) -
    (a.evidence.descriptive_rank ?? -Infinity));
  return {available:true, independent_transmitter_sites:populated.length,
    computation:{evaluations,truncated,measurement_limit_per_site:measurementLimitPerSite},
    hypotheses:candidates.slice(0,256)};
}

class MultiSiteMapSupportTracker {
  constructor(config, options={}) {
    this.config=config; this.options=options; this.lastKey=null; this.lastResult=null;
    this.nextId=1; this.tracks=new Map(); this.maximumCoastSeconds=Number(options.maximumCoastSeconds)||10;
    this.maxTracks=Math.max(8,Number(options.maxTracks)||64);
    this.maxNewTracksPerCpi=Math.max(1,Number(options.maxNewTracksPerCpi)||16);
    this.lastReceiptAt=null; this.lastCohort=null;
    // Deliberately an offline constructor argument, never a live configuration knob.
    this.likelihood = options.likelihoodProfile ? makeLikelihoodEvaluator(config, options.likelihoodProfile) : null;
  }
  invalid(reason) {
    this.tracks.clear(); this.lastKey=null; this.lastReceiptAt=Date.now();
    return this.lastResult={schema:'vectorwarp-multi-site-map-support-v1',available:false,
      diagnostic_only:true,operational_track_claim:false,geographic_claim:false,adsb_used_by_tracker:false,
      reason,tracks:[]};
  }
  update(frames) {
    const cohort=cohortIdentity(frames), fs=this.config?.capture?.fs;
    if (!cohort?.capture_epoch_verified || !Number.isSafeInteger(cohort.capture_epoch) ||
        !finite(fs) || fs<=0) return this.invalid('Aligned verified capture cohorts and a capture rate are required.');
    const enabled=(this.config?.process?.multi_illuminator?.illuminators || []).filter(x=>x?.enabled===true);
    if (frames.length!==enabled.length) return this.invalid('Incomplete enabled-lane cohort.');
    const prior=this.lastCohort;
    if (prior && (cohort.capture_epoch<prior.capture_epoch || (cohort.capture_epoch===prior.capture_epoch &&
        cohort.key!==prior.key && cohort.capture_first_input_sample<prior.capture_end_input_sample)))
      return this.invalid('Overlapping, replayed or regressed capture interval.');
    const sampleOffsets=frames.map(frame=>sampleTiming(frame,cohort,fs));
    if (sampleOffsets.some(x=>x===null)) return this.invalid('Native lane sample-time lineage missing.');
    const channels=this.likelihood?.prepare(frames,cohort);
    if (this.likelihood && !channels) return this.invalid('Mode, carrier, lane timing or map profile mismatch.');
    const frameOffsets=new Map(frames.map((f,i)=>[f.id,sampleOffsets[i]]));
    const offsets=channels ? [...channels.values()].map(c=>c.offset) : sampleOffsets;
    if (cohort.key===this.lastKey) return measurements(frames,this.config,this.options.measurementLimitPerSite) ?
      this.lastResult : this.invalid('Invalid duplicate cohort provenance.');
    if (prior && prior.key===cohort.key) return this.invalid('Previously consumed CPI.');
    const generated=hypotheses(frames,this.config,this.options), timestamp=cohort.timestamp;
    if (!generated.available) return this.invalid(generated.reason);
    if (!prior || cohort.capture_epoch!==prior.capture_epoch) this.tracks.clear();
    const midpoint=cohort.capture_first_input_sample+
      (cohort.capture_end_input_sample-cohort.capture_first_input_sample)/2;
    const seedOffset=offsets.reduce((a,b)=>a+b,0)/offsets.length;
    // Existing root solver requires simultaneous measurements. Offset map
    // lookups can continue a prior branch even when fresh roots cannot be used.
    const cotimed=Math.max(...offsets)-Math.min(...offsets)<=1/fs;
    generated.simultaneous_seed_geometry=cotimed;
    if (!cotimed) generated.hypotheses=[];
    const candidates=generated.hypotheses.map(c=>({...c,point:add(c.point,scale(c.velocity,-seedOffset))}));
    const predictions=new Map(), pairs=[];
    for (const [id,track] of this.tracks) {
      const dt=(midpoint-track.midpoint)/fs, age=(midpoint-track.lastSupportSample)/fs;
      if (!(dt>0) || age>this.maximumCoastSeconds) { this.tracks.delete(id); continue; }
      const point=add(track.point,scale(track.velocity,dt));
      // The predicted point is fixed BEFORE inspecting this CPI's roots.
      const evidence=this.likelihood ? this.likelihood.score(channels,point,track.velocity,
        (midpoint-track.lastFitSample)/fs,track.fitTimeError) :
        evaluateState(frames,this.config,point,track.velocity,undefined,frameOffsets);
      const hit=this.likelihood ? evidence?.hit===true :
        evidence.strong_channels>=1 && evidence.descriptive_rank>=3;
      predictions.set(id,{track,dt,point,evidence,hit});
      if (!hit) continue;
      for (let index=0;index<candidates.length;index++) {
        const candidate=candidates[index],distance=norm(sub(point,candidate.point));
        if (distance<=Math.max(.5,.45*dt)) pairs.push({id,index,distance});
      }
    }
    pairs.sort((a,b)=>a.distance-b.distance);
    const usedTracks=new Map(),usedCandidates=new Set();
    for (const pair of pairs) if (!usedTracks.has(pair.id)&&!usedCandidates.has(pair.index)) {
      usedTracks.set(pair.id,candidates[pair.index]);usedCandidates.add(pair.index);
    }
    const fitTimeError=channels ? Math.max(...[...channels.values()].map(c=>c.lane.error+Math.abs(c.offset-seedOffset))) : 0;
    for (const {track,point,evidence,hit} of predictions.values()) {
      const candidate=usedTracks.get(track.id);
      if (candidate) {
        track.point=add(point,scale(sub(candidate.point,point),.24));
        track.velocity=add(scale(track.velocity,.82),scale(candidate.velocity,.18));
        track.lastCandidate=candidate; track.lastFitSample=midpoint; track.fitTimeError=fitTimeError;
      } else track.point=point;
      track.lastEvidence=evidence; track.midpoint=midpoint;
      track.hits.push(hit);track.hits=track.hits.slice(-6);
      track.history.push({key:cohort.key,hit,ratio:evidence?.ratio??null,
        lanes:evidence?.channels?.filter(c=>c.observed??true).map(c=>c.illuminator)??[]});
      track.history=track.history.slice(-6);
      if (hit) {track.lastSupportSample=midpoint;track.lastSupportTimestamp=timestamp;track.misses=0;}
      else track.misses++;
    }
    let created=0;
    for (let index=0;index<candidates.length && created<this.maxNewTracksPerCpi;index++) if (!usedCandidates.has(index)) {
      const candidate=candidates[index],id='M'+String(this.nextId++).padStart(4,'0');
      this.tracks.set(id,{id,point:candidate.point,velocity:candidate.velocity,midpoint,
        lastSupportSample:midpoint,lastSupportTimestamp:null,lastFitSample:midpoint,fitTimeError,
        seedKey:cohort.key,seedContributors:candidate.contributors,
        hits:[],history:[],misses:0,lastEvidence:null,lastCandidate:candidate});
      created++;
    }
    for (const [id,track] of this.tracks) if (track.misses>3) this.tracks.delete(id);
    if(this.tracks.size>this.maxTracks) for(const track of [...this.tracks.values()]
      .sort((a,b)=>a.hits.filter(Boolean).length-b.hits.filter(Boolean).length||
        b.misses-a.misses||a.lastSupportSample-b.lastSupportSample).slice(0,this.tracks.size-this.maxTracks))
      this.tracks.delete(track.id);
    const tracks=[...this.tracks.values()].map(track=>({id:track.id,
      state:track.hits.filter(Boolean).length>=3?'PERSISTENT_AMBIGUOUS':'MAP_SUPPORT_CANDIDATE',
      diagnostic_only:true,operational_track_claim:false,geographic_claim:false,
      timestamp,measurement_timestamp:track.lastSupportTimestamp,supporting_cpis:track.hits.filter(Boolean).length,
      required_supporting_cpis:3,misses:track.misses,seed_cpi:track.seedKey,seed_contributors:track.seedContributors,
      evidence_history:track.history.slice(),position_hypothesis:{...geographicPoint3d(this.config.location.rx,track.point),
        ambiguity:'Radar-only branch; no geographic or aircraft claim.'},
      velocity_enu_km_s:track.velocity,map_evidence:track.lastEvidence,
      independent_transmitter_sites:track.lastCandidate?.independent_transmitter_sites||null,
      contributors:track.lastCandidate?.contributors||[]}));
    this.lastCohort=cohort;
    this.lastKey=cohort.key;this.lastReceiptAt=Date.now();this.lastResult={schema:'vectorwarp-multi-site-map-support-v1',available:true,
      diagnostic_only:true,operational_track_claim:false,geographic_claim:false,adsb_used_by_tracker:false,...cohort,
      likelihood_profile:this.likelihood?.metadata??null,
      definition:'Common radar-only trajectory hypotheses projected into every channel delay-Doppler map; evidence combines noncoherently across arbitrary transmitter sites.',
      caveat:'Descriptive ranks and offline replay likelihoods are diagnostic. No aircraft or operational position is claimed.',
      generation:generated,tracks};
    return this.lastResult;
  }
  snapshot(now=Date.now()) {
    if(!this.lastResult)return null;
    const age=(now-this.lastReceiptAt)/1000;
    if(age<=this.maximumCoastSeconds)return {...this.lastResult,age_seconds:age};
    return {...this.lastResult,available:false,reason:'Aligned map-support cohort is stale.',
      age_seconds:age,generation:null,tracks:[]};
  }
}

module.exports={gradient,sampleMap,siteGroups,measurements,twoSiteRoots,velocityFor,evaluateState,
  hypotheses,MultiSiteMapSupportTracker};
