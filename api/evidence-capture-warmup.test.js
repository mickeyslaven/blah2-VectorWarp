'use strict';

const assert = require('assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {EvidenceStatistics} = require('./evidence-statistics');
const {CaptureTriggerQueue} = require('./capture-trigger-queue');

const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-capture-warmup-'));
let clock = 2_000_000_000_000;
const config = {capture: {fs: 2400000, fc: 563000000, device: {type: 'Kraken'}},
  location: {rx: {latitude: 1, longitude: 2}, tx: {latitude: 3, longitude: 4}},
  truth: {adsb: {enabled: true}}, process: {
    ambiguity: {delayMin: -10, delayMax: 500, dopplerMin: -600, dopplerMax: 600},
    clutter: {delayMax: 200}, detection: {minDelay: 5},
    bearing: {manifold_recorder: {max_truth_age_s: 3}}}};
const prediction = {delay: 10, doppler: 100, timestamp: clock / 1000,
  provenance: {observation_time_offset_s: .1,
    projection_model: 'constant_velocity_ground_track',
    maximum_pairwise_projected_disagreement_km: .1},
  geometry: {bearing_deg_true: 42, elevation_deg: 10, slant_range_km: 20}};
const queue = new CaptureTriggerQueue({now: () => clock});
let ready = false;
const statistics = new EvidenceStatistics(config, {async get() { return {}; }}, {
  filename: path.join(directory, 'statistics.json'), now: () => clock,
  setIntervalFn() { return {unref() {}}; }, clearIntervalFn() {},
  onEligibilityEvent(event) {
    return ready ? queue.acceptEligibilityEvent(event, clock) :
      {accepted: false, deferred: true, reason: 'radar-recorder-warming'};
  }});

statistics.observe('abc123', prediction, clock, null, null);
assert.equal(statistics.state.eligibilityEvents.length, 1);
assert.equal(statistics.state.eligibilityEvents[0].capture_state, 'deferred');
assert.equal(queue.clips.length, 0);
ready = true; clock += 500;
statistics.observe('abc123', {...prediction, timestamp: clock / 1000}, clock, null, null);
assert.equal(statistics.state.eligibilityEvents.length, 1,
  'Warmup recovery must preserve one eligibility-event identity');
assert.equal(statistics.state.eligibilityEvents[0].capture_state, 'trigger_pending');
assert.equal(queue.clips.length, 1);
assert.deepEqual(queue.request().eventIds, [statistics.state.eligibilityEvents[0].id]);
clock += 500;
statistics.observe('abc123', {...prediction, timestamp: clock / 1000}, clock, null, null);
assert.equal(queue.clips.length, 1, 'Accepted initial admission must never retry');
statistics.close();
fs.rmSync(directory, {recursive: true, force: true});
