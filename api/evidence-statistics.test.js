'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {EvidenceStatistics, limits} = require('./evidence-statistics');

const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-statistics-'));
const filename = path.join(directory, 'statistics.json');
let clock = 1_800_000_000_000;
const config = {capture: {fs: 2400000, fc: 563000000},
  location: {rx: {latitude: 1, longitude: 2}, tx: {latitude: 3, longitude: 4}},
  truth: {adsb: {enabled: true}}, process: {
    ambiguity: {delayMin: -10, delayMax: 500, dopplerMin: -600, dopplerMax: 600},
    clutter: {delayMax: 200}, detection: {minDelay: 5},
    bearing: {channel_mapping: [0, 1], manifold_recorder: {
      max_delay_error_km: .25, max_doppler_error_hz: 5, max_truth_age_s: 3}}}};
const prediction = {delay: 10, doppler: 100, timestamp: clock / 1000, flight: 'TEST1'};
const source = {async get() { return {abc123: {...prediction, timestamp: clock / 1000}}; }};

(async () => {
  const span = limits(config);
  assert.equal(span.delayMaxKm.toFixed(3), '62.457');
  assert.equal(span.detectionMinDelayKm.toFixed(3), '0.625');
  const stats = new EvidenceStatistics(config, source, {filename, now: () => clock});
  for (let index = 0; index < 3; ++index) {
    stats.acceptTracker({timestamp: clock, data: [{delay: 10 / span.kmPerBin, doppler: 100}]});
    stats.acceptDetection({timestamp: clock, delay: [10.1], doppler: [102]});
    while (stats.running) await new Promise(resolve => setImmediate(resolve));
    clock += 1000;
  }
  let result = stats.status();
  assert.equal(result.windows.hour.active_passes, 1);
  assert.equal(result.windows.hour.eligible_passes, 0,
    'An in-progress pass must not be prematurely classified');
  clock += 121000;
  source.get = async () => ({});
  stats.acceptDetection({timestamp: clock, delay: [], doppler: []});
  while (stats.running) await new Promise(resolve => setImmediate(resolve));
  result = stats.status();
  assert.equal(result.windows.hour.eligible_passes, 1);
  assert.equal(result.windows.hour.detected_passes, 1);
  assert.equal(result.windows.hour.tracked_passes, 1);
  assert.equal(result.windows.hour.tracking_yield, 1);
  clock += 1000;

  source.get = async () => ({def456: {delay: 70, doppler: 800,
    timestamp: clock / 1000, flight: 'OUT'}});
  for (let index = 0; index < 3; ++index) {
    stats.acceptDetection({timestamp: clock, delay: [], doppler: []});
    while (stats.running) await new Promise(resolve => setImmediate(resolve));
    clock += 1000;
  }
  clock += 121000;
  source.get = async () => ({});
  stats.acceptDetection({timestamp: clock, delay: [], doppler: []});
  while (stats.running) await new Promise(resolve => setImmediate(resolve));
  result = stats.status();
  assert.equal(result.windows.hour.eligible_passes, 1,
    'Out-of-span aircraft must not enter the tracking denominator');
  assert.equal(result.windows.hour.out_of_config_passes, 1);
  assert.equal(result.windows.hour.out_of_span.above_delay_ceiling, 3);
  assert.equal(result.windows.hour.out_of_span.outside_doppler_span, 3);
  stats.close();
  assert(fs.existsSync(filename));
  const recovered = new EvidenceStatistics(config, source, {filename, now: () => clock});
  assert.equal(recovered.status().windows.lifetime.tracked_passes, 1);
  recovered.close();
  fs.rmSync(directory, {recursive: true, force: true});
  console.log('PASS eligibility-filtered persistent evidence statistics');
})().catch(error => { console.error(error); process.exitCode = 1; });
