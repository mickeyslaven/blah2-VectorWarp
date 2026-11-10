'use strict';

// Offline shadow evaluator. No profile is installed by the service. Profile
// probability tables must correspond to this setup AND this exact feature.
const {createHash} = require('node:crypto');
const {localPoint3d, excessPath3d} = require('./multi-illuminator-track-fusion');
const {gradient, sampleMap, siteGroups} = require('./bistatic-map-sampling');

const C = 299792.458;
const finite = Number.isFinite;
const probability = value => finite(value) && value >= 0 && value <= 1;
const vector = x => Array.isArray(x) && x.length === 3 && x.every(finite);
const canonical = x => x && typeof x === 'object' ? (Array.isArray(x) ? x.map(canonical) :
  Object.fromEntries(Object.keys(x).sort().map(k => [k, canonical(x[k])]))) : x;
const digest = x => createHash('sha256').update(JSON.stringify(canonical(x))).digest('hex');
// Truth, held-out aircraft, wall clocks and UI configuration are not solver inputs.
const setupDigest = config => digest({capture:config.capture, process:config.process, location:config.location});
const frameMode = frame => digest({roles:frame.input_roles && {source:frame.input_roles.source,
  reference:frame.input_roles.reference_channel, surveillance:frame.input_roles.surveillance_channels},
  window:frame.doppler_window, reference:frame.atsc_reference?.effective_mode ?? null,
  projection:frame.atsc_reference?.projection_applied ?? false,
  phase_correction:frame.input_phase_correction?.applied ?? false});
const gridDigests = new WeakMap();
function gridDigest(map) {
  if (!map || typeof map !== 'object' || !Array.isArray(map.delay) || !Array.isArray(map.doppler)) return null;
  if (!gridDigests.has(map)) gridDigests.set(map, digest({delay:map.delay,doppler:map.doppler}));
  return gridDigests.get(map);
}
const feature = 'map-db-minus-radius4-annulus-median-v1';
const table = (values, count, positive) => Array.isArray(values) && values.length === count &&
  values.every(x => probability(x) && (!positive || x > 0)) &&
  Math.abs(values.reduce((a,b) => a+b, 0)-1) < 1e-9;

// Half-open capture interval ties frames to the same recorder epoch. Native
// lane first/end positions are inclusive positions on that raw sample clock.
function sampleTiming(frame, cohort, captureRate) {
  const first = frame.first_input_sample, end = frame.end_input_sample;
  if (![first,end].every(Number.isSafeInteger) || first < 0 || end < first ||
      !finite(captureRate) || captureRate <= 0) return null;
  const start = cohort.capture_first_input_sample, stop = cohort.capture_end_input_sample;
  // Lane filters can shift a CPI. A disjoint lane CPI cannot be attributed to
  // this publication without additional lineage.
  if (end < start || first >= stop) return null;
  return ((first-start) + (end-stop)) / (2*captureRate);
}

function makeLikelihoodEvaluator(config, profile) {
  profile = structuredClone(profile);
  const groups = siteGroups(config), captureRate = config?.capture?.fs;
  if (!profile || profile.schema !== 'vectorwarp-cross-frequency-profile-v1' ||
      profile.feature !== feature || !profile.id?.trim?.() || !profile.fit_id?.trim?.() ||
      !profile.null_id?.trim?.() || profile.setup_sha256 !== setupDigest(config) ||
      !groups.length || !(finite(captureRate) && captureRate > 0) ||
      !(finite(profile.relative_clock_error_ppm) && profile.relative_clock_error_ppm >= 0 &&
        profile.relative_clock_error_ppm < 1e6) ||
      !(finite(profile.hit_ratio) && profile.hit_ratio > 1))
    throw new Error('Invalid or mismatched cross-frequency replay profile.');
  const lanes = groups.flatMap(group => group.definitions.map(def => {
    const m = profile.lanes?.[def.id], edges = m?.edges_db;
    if (!Array.isArray(edges) || !edges.length || !edges.every((e,i) => finite(e) &&
        (!i || e > edges[i-1])) || !table(m.null_mass, edges.length+1, true) ||
        !table(m.visible_mass, edges.length+1, false) || !probability(m.visibility) ||
        !probability(m.reliability) || typeof m.mode_sha256 !== 'string' ||
        !/^[a-f0-9]{64}$/.test(m.mode_sha256) || !/^[a-f0-9]{64}$/.test(m.grid_sha256) || !finite(m.lane_time_bias_s) ||
        !finite(m.lane_time_error_s) || m.lane_time_error_s < 0)
      throw new Error('Invalid likelihood table, timing model or processing identity for '+def.id);
    const ratio = m.visible_mass.map((p,i) => 1 + m.visibility * m.reliability *
      (p / m.null_mass[i] - 1));
    if (!ratio.every(x => finite(x) && x >= 0)) throw new Error('Non-finite likelihood table');
    return {id:def.id, site:group.id, transmitter:localPoint3d(config.location.rx,def.transmitter),
      carrier:def.frequency_hz, sampleRate:def.output_sample_rate_hz,
      edges:[...edges], ratio, mode:m.mode_sha256, grid:m.grid_sha256, bias:m.lane_time_bias_s, error:m.lane_time_error_s};
  }));
  if (Object.keys(profile.lanes).length !== lanes.length) throw new Error('Profile lane set differs from setup');
  const laneById = new Map(lanes.map(x => [x.id,x]));
  const metadata = {profile_id:profile.id, fit_id:profile.fit_id, null_id:profile.null_id,
    setup_sha256:profile.setup_sha256, profile_sha256:digest(profile), feature,
    combination:'fixed equal site weights, equal carrier weights within site; no independence assumed',
    geographic_claim:false, operational_track_claim:false, adsb_used_by_tracker:false};
  function prepare(frames, cohort) {
    if (frames.length !== lanes.length) return null;
    const channels = new Map();
    for (const frame of frames) {
      const lane = laneById.get(frame.id);
      const offset = sampleTiming(frame, cohort, captureRate);
      if (!lane || offset === null || frame.frequency_hz !== lane.carrier ||
          frame.sample_rate_hz !== lane.sampleRate || !frame.doppler_window ||
          !Number.isInteger(frame.input_roles?.reference_channel) ||
          !Array.isArray(frame.input_roles?.surveillance_channels) || frameMode(frame) !== lane.mode ||
          (frame.map && (gridDigest(frame.map) !== lane.grid || frame.map.timestamp !== frame.timestamp))) return null;
      channels.set(lane.id,{lane,frame,offset:offset+lane.bias});
    }
    return channels.size === lanes.length ? channels : null;
  }
  function score(channels, point, velocity, secondsSinceMeasurement, fitTimeError = 0) {
    if (!channels || !vector(point) || !vector(velocity) || !finite(secondsSinceMeasurement) ||
        secondsSinceMeasurement < 0 || !finite(fitTimeError) || fitTimeError < 0) return null;
    const results = [];
    for (const {lane,frame,offset} of channels.values()) {
      // Marginalize an explicitly bounded uniform relative-time error by
      // three-point Gauss-Legendre quadrature. All terms are averaged, never
      // maximized over the timing gate. Absolute UTC error cancels here.
      const bound = fitTimeError + lane.error + (secondsSinceMeasurement + Math.abs(offset)) *
        profile.relative_clock_error_ppm / 1e6;
      const trials = [];
      for (const [node, weight] of (bound > 0 ? [[-Math.sqrt(3/5),5/18],[0,4/9],
          [Math.sqrt(3/5),5/18]] : [[0,1]])) {
        const dt = offset + node * bound;
        const p = point.map((x,i) => x + velocity[i]*dt);
        const g = gradient(p,lane.transmitter);
        const range = g && excessPath3d(p,lane.transmitter);
        const hz = g && -lane.carrier / C * g.reduce((s,x,i) => s+x*velocity[i],0);
        const cell = g && sampleMap(frame.map,range,hz);
        const bin = cell ? lane.edges.filter(edge => cell.residual_db >= edge).length : null;
        trials.push({weight, offset_s:dt, excess_km:range, doppler_hz:hz,
          row:cell?.row ?? null, column:cell?.column ?? null, residual_db:cell?.residual_db ?? null,
          feature_bin:bin, ratio:bin === null ? 1 : lane.ratio[bin]});
      }
      results.push({illuminator:lane.id, site:lane.site, carrier_hz:lane.carrier,
        sample_rate_hz:lane.sampleRate, mode_sha256:lane.mode, grid_sha256:lane.grid,
        first_input_sample:frame.first_input_sample, end_input_sample:frame.end_input_sample,
        event_time:frame.map?.eventTime ?? frame.detections?.eventTime ?? null,
        frame_provenance:frame.provenance ?? null, timing_bound_s:bound,
        observed:trials.some(t => t.feature_bin !== null),
        ratio:trials.reduce((s,t) => s+t.weight*t.ratio,0), trials});
    }
    const sites = groups.map(group => {
      const rows = results.filter(x => x.site === group.id);
      return {site:group.id, ratio:rows.reduce((s,item) => s+item.ratio,0)/rows.length};
    });
    const ratio = sites.reduce((s,site) => s+site.ratio,0)/sites.length;
    return {ratio, hit:ratio >= profile.hit_ratio && results.some(x => x.observed),
      comparison_threshold:profile.hit_ratio, contributing_lanes:results.filter(x => x.observed).length,
      sites, channels:results, ...metadata};
  }
  return {metadata, prepare, score};
}

module.exports = {makeLikelihoodEvaluator, setupDigest, frameMode, gridDigest, sampleTiming, feature};
