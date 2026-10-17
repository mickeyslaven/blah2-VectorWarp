'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {ManifoldRecorder, validateEvidence, automaticallyEligible} = require('./manifold-recorder');

(async () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-manifold-'));
  const filename = path.join(directory, 'samples.jsonl');
  const config = {capture: {fc: 539000000, fs: 2400000, device: {
    surveillance_channels: [0, 1], array_geometry: {x_axis_bearing_deg_true: 80}}},
  location: {rx: {latitude: 40, longitude: -80, altitude: 300},
    tx: {latitude: 40.1, longitude: -80, altitude: 400}},
  process: {bearing: {manifold_recorder: {enabled: true, path: filename,
    min_snr_db: 5, min_elevation_deg: -10, max_elevation_deg: 30,
    max_delay_error_km: 2, max_doppler_error_hz: 10, max_truth_age_s: 3,
    max_file_mb: 1, max_files: 2}}}};
  const source = {get: async kind => kind === 'aircraft' ? {aircraft: [{hex: 'abc123',
    flight: 'TEST1 ', lat: 40, lon: -79.9, alt_geom: 10000}]} :
    {abc123: {timestamp: 100, delay: 10, doppler: 20}}};
  const recorder = new ManifoldRecorder(config, source);
  const frame = validateEvidence({schema: 'vectorwarp-manifold-evidence-v1', timestamp: 100000,
    frequency_hz: 539000000, sample_rate_hz: 2400000, channels: 2,
    reference_weights: [[1, 0], [0, 1]], data: [{cell: 5, delay_km: 10.5,
      doppler_hz: 18, snr_db: 9, response: [[1, 2], [3, 4]]}]});
  await recorder.record(frame);
  const rows = fs.readFileSync(filename, 'utf8').trim().split('\n').map(JSON.parse);
  assert.equal(rows.length, 1);
  assert.equal(rows[0].icao, 'abc123');
  assert.equal(rows[0].flight, 'TEST1');
  assert.ok(Math.abs(rows[0].truth.bearing_deg_true - 90) < .1);
  assert.ok(Math.abs(rows[0].truth.local_azimuth_deg - 350) < .1);
  assert.ok(Number.isFinite(rows[0].direct_path_local_azimuth_deg));
  assert.deepEqual(rows[0].response, [[1, 2], [3, 4]]);
  assert.equal(recorder.status().samples, 1);
  const stale = JSON.parse(JSON.stringify(frame)); stale.timestamp = 110000;
  await recorder.record(stale);
  assert.equal(recorder.status().samples, 1, 'stale ADS-B truth must not label a manifold sample');
  assert.ok(recorder.status().staleTruth > 0);
  const automaticConfig = JSON.parse(JSON.stringify(config));
  automaticConfig.process.bearing.manifold_recorder.enabled = 'auto';
  Object.assign(automaticConfig.capture.device, {type: 'Kraken'});
  Object.assign(automaticConfig.capture.device.array_geometry, {
    mapping_confirmed: true, geometry_confirmed: true, orientation_confirmed: true});
  automaticConfig.process.surveillance_fusion = {mode: 'coherent_beam_bank'};
  automaticConfig.process.detection = {enable: true};
  automaticConfig.truth = {adsb: {enabled: true}};
  assert.equal(automaticallyEligible(automaticConfig), true);
  const changedSite = JSON.parse(JSON.stringify(automaticConfig));
  changedSite.location.rx.longitude = -80.01;
  const second = new ManifoldRecorder(changedSite, source);
  assert.equal(second.status().enabled, true);
  await second.record(frame);
  assert.ok(fs.existsSync(`${filename}.site-previous`),
    'A site fingerprint change must archive the previous active collection');
  const current = fs.readFileSync(filename, 'utf8').trim().split('\n').map(JSON.parse);
  assert.ok(current.length === 1 && current[0].config_fingerprint === second.fingerprint,
    'A site fingerprint change must begin a clean active collection');
  assert.ok(second.status().siteResetAt,
    'Site reset must be visible in recorder health');
  const thirdSite = JSON.parse(JSON.stringify(changedSite));
  thirdSite.location.rx.longitude = -80.02;
  const third = new ManifoldRecorder(thirdSite, source);
  await third.record(frame);
  assert.ok(fs.existsSync(`${filename}.site-previous.1`));
  assert.ok(!fs.existsSync(`${filename}.site-previous.2`),
    'Site archives plus the active collection must obey max_files');
  fs.rmSync(directory, {recursive: true});
  console.log('Manifold recorder: automatic eligibility, site reset, sparse matching, and portable labelled JSONL passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
