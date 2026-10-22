#!/usr/bin/env node
'use strict';

const assert = require('assert');
const crypto = require('crypto');
const fs = require('fs');
const {TrackerPublication} = require('../api/tracker-publication');

const KM_PER_BIN = 299792458 / 2400000 / 1000;

function parseArgs(values) {
  const args = {};
  for (let index = 2; index < values.length; index += 2) {
    assert(values[index].startsWith('--') && values[index + 1], `bad argument ${values[index]}`);
    args[values[index].slice(2)] = values[index + 1];
  }
  for (const required of ['replay', 'truth', 'analysis', 'output']) {
    assert(args[required], `--${required} is required`);
  }
  return args;
}

function nearest(rows, timestamp) {
  return rows.reduce((best, row) => Math.abs(row.observed_at - timestamp) <
    Math.abs(best.observed_at - timestamp) ? row : best);
}

function eligibleTruth(row) {
  const result = {};
  for (const [aircraft, point] of Object.entries(row.truth || {})) {
    if (row.observed_at - point.timestamp >= -1 && row.observed_at - point.timestamp <= 3 &&
        point.delay >= .6245676208 && point.delay <= 62.4567620833 &&
        Math.abs(point.doppler) <= 600) result[aircraft] = point;
  }
  return result;
}

function match(truth, delayKm, doppler, delayGate, dopplerGate) {
  let best = null;
  for (const [aircraft, point] of Object.entries(truth)) {
    if (Math.abs(point.delay - delayKm) > delayGate ||
        Math.abs(point.doppler - doppler) > dopplerGate) continue;
    const distance = Math.abs(point.delay - delayKm) + Math.abs(point.doppler - doppler) / 20;
    if (!best || distance < best.distance) best = {aircraft, distance};
  }
  return best?.aircraft || null;
}

function detectorMetrics(frames, truthRows, start, cpi) {
  let detections = 0;
  let unmatched = 0;
  const nearFrames = new Map();
  frames.forEach((frame, index) => {
    const truth = eligibleTruth(nearest(truthRows, start + (index + 1) * cpi));
    (frame.delay || []).forEach((delay, item) => {
      detections++;
      const aircraft = match(truth, delay, frame.doppler[item], .25, 5);
      if (!aircraft) unmatched++;
      else {
        if (!nearFrames.has(aircraft)) nearFrames.set(aircraft, new Set());
        nearFrames.get(aircraft).add(index);
      }
    });
  });
  return {frames: frames.length, detections,
    unmatched_to_eligible_adsb: unmatched,
    target_near_detections: detections - unmatched,
    target_near_aircraft: Object.fromEntries([...nearFrames].map(([id, hits]) => [id, hits.size])),
    canonical_sha256: crypto.createHash('sha256').update(JSON.stringify(frames)).digest('hex')};
}

function trackMetrics(frames, truthRows, start, cpi) {
  const ids = new Set();
  const associated = new Set();
  const confirmed = new Set();
  const targetIds = new Set();
  const targetFrames = new Map();
  let instances = 0;
  let unmatchedInstances = 0;
  frames.forEach((frame, index) => {
    const truth = eligibleTruth(nearest(truthRows, start + (index + 1) * cpi));
    for (const track of frame.data || []) {
      const id = String(track.id);
      ids.add(id);
      if (track.state === 'ASSOCIATED') associated.add(id);
      if (track.state === 'ACTIVE' || track.state === 'COASTING') confirmed.add(id);
      instances++;
      const aircraft = match(truth, track.delay * KM_PER_BIN, track.doppler, 2, 5);
      if (!aircraft) unmatchedInstances++;
      else {
        targetIds.add(id);
        if (!targetFrames.has(aircraft)) targetFrames.set(aircraft, new Set());
        targetFrames.get(aircraft).add(index);
      }
    }
  });
  const maxima = name => frames.reduce((value, frame) => Math.max(value, frame[name] || 0), 0);
  return {frames: frames.length,
    peak_tracker_population: maxima('n'),
    peak_tentative_population: maxima('nTentative'),
    observable_formed_track_ids: ids.size,
    associated_track_ids: associated.size,
    confirmed_active_or_coasting_track_ids: confirmed.size,
    track_frame_instances: instances,
    unmatched_track_frame_instances: unmatchedInstances,
    ids_never_near_eligible_adsb: [...ids].filter(id => !targetIds.has(id)).length,
    target_near_track_ids: targetIds.size,
    target_near_aircraft: Object.fromEntries([...targetFrames].map(([id, hits]) => [id, hits.size]))};
}

function publicFrames(nativeFrames, mode, minimum = 5) {
  let now = 10000;
  const config = {capture: {fs: 2400000}, process: {tracker: {publication: {
    mode, stale_after_s: 3, native_min_associations: minimum,
    delay_tolerance: 2, doppler_tolerance: 5}}}};
  const publication = new TrackerPublication(config, {now: () => now});
  return nativeFrames.map(frame => {
    publication.acceptNative(JSON.stringify(frame));
    publication.acceptInput({timestamp: frame.timestamp}, true);
    const result = JSON.parse(publication.frame());
    now += 500;
    return result;
  });
}

function analyze(replay, truthRows, priorAnalysis, armName = 'coherent_beam_bank') {
  const arm = replay.arms[armName];
  assert(arm, `missing replay arm ${armName}`);
  const start = priorAnalysis.signal_start_epoch;
  const cpi = priorAnalysis.cpi_seconds;
  assert(Number.isFinite(start) && Number.isFinite(cpi) && cpi > 0, 'invalid time alignment');
  const detector = detectorMetrics(arm.detections, truthRows, start, cpi);
  const nativeFrames = publicFrames(arm.tracks, 'native');
  const hybridFrames = publicFrames(arm.tracks, 'hybrid');
  const persistence = 60;
  const retirement = 100;
  return {schema: 'vectorwarp-tracker-publication-shadow-ablation-v1', arm: armName,
    replay: {signal_start_epoch: start, cpi_seconds: cpi,
      detector_input_reused_for_all_arms: true},
    raw_detector: detector,
    stationary_clutter: {
      mode: 'fresh-map bounded proof; resident execution unavailable on this host',
      persistence_cpis: persistence, retirement_history_samples: retirement,
      frames: arm.detections.length,
      admission_suppression_reachable: arm.detections.length >= persistence,
      retirement_reachable: arm.tracks.length >= retirement,
      off_observed_native: trackMetrics(arm.tracks, truthRows, start, cpi),
      on_shadow_native: arm.detections.length < persistence && arm.tracks.length < retirement ?
        {expected_equal_to_off: true, reason: 'at most one persistence credit per cell per CPI; both thresholds exceed clip length'} :
        {expected_equal_to_off: null, reason: 'clip is long enough that resident replay is required'}},
    publication: {
      native: trackMetrics(nativeFrames, truthRows, start, cpi),
      hybrid_no_external_events: trackMetrics(hybridFrames, truthRows, start, cpi),
      hybrid_policy: {native_min_associations: 5, input_fresh: true,
        external_events: 0, semantics: 'exact TrackerPublication output over identical saved native frames'}}};
}

if (require.main === module) {
  const args = parseArgs(process.argv);
  const replay = JSON.parse(fs.readFileSync(args.replay));
  const truth = fs.readFileSync(args.truth, 'utf8').trim().split(/\n+/).map(JSON.parse);
  const analysis = JSON.parse(fs.readFileSync(args.analysis));
  const result = analyze(replay, truth, analysis, args.arm || 'coherent_beam_bank');
  fs.writeFileSync(args.output, `${JSON.stringify(result, null, 2)}\n`);
  process.stdout.write(`${JSON.stringify(result, null, 2)}\n`);
}

module.exports = {analyze, detectorMetrics, trackMetrics, publicFrames};
