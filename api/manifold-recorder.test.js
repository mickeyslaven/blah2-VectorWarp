'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {ManifoldRecorder, validateEvidence, automaticallyEligible, siteFingerprint} = require('./manifold-recorder');

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
  const promotedConfig = JSON.parse(JSON.stringify(automaticConfig));
  promotedConfig.process.bearing.manifold_file = `${filename}.qualified.json`;
  assert.equal(siteFingerprint(promotedConfig), siteFingerprint(automaticConfig),
    'Selecting an automatically fitted model must not manufacture a new physical site');
  const fixedGainConfig = JSON.parse(JSON.stringify(automaticConfig));
  fixedGainConfig.capture.device.heimdall = {gain: 14, gain_control: 'fixed'};
  fixedGainConfig.process.bearing.manifold_recorder.path = path.join(directory, 'gain-samples.jsonl');
  const measuredPolicyConfig = JSON.parse(JSON.stringify(fixedGainConfig));
  measuredPolicyConfig.capture.device.heimdall.gain_control = 'measured';
  assert.equal(siteFingerprint(fixedGainConfig), siteFingerprint(measuredPolicyConfig),
    'Policy alone is not a changed RF gain');
  const changedGainConfig = JSON.parse(JSON.stringify(measuredPolicyConfig));
  changedGainConfig.capture.device.heimdall.gain = 1;
  assert.notEqual(siteFingerprint(fixedGainConfig), siteFingerprint(changedGainConfig),
    'Qualification cannot carry across requested gain changes');
  const firstGainRecorder = new ManifoldRecorder(fixedGainConfig, source);
  await firstGainRecorder.record(frame);
  const nextGainRecorder = new ManifoldRecorder(changedGainConfig, source);
  await nextGainRecorder.record(frame);
  const gainPath = changedGainConfig.process.bearing.manifold_recorder.path;
  const oldGainRows = fs.readFileSync(`${gainPath}.site-previous`, 'utf8').trim().split('\n').map(JSON.parse);
  const newGainRows = fs.readFileSync(gainPath, 'utf8').trim().split('\n').map(JSON.parse);
  assert.equal(oldGainRows.length, 1);
  assert.equal(newGainRows.length, 1);
  assert.equal(oldGainRows[0].config_fingerprint, firstGainRecorder.fingerprint);
  assert.equal(newGainRows[0].config_fingerprint, nextGainRecorder.fingerprint);
  assert.notEqual(oldGainRows[0].config_fingerprint, newGainRows[0].config_fingerprint,
    'Old and new gain evidence must not mix in the active collection');
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
  const adaptive = validateEvidence({schema: 'vectorwarp-manifold-evidence-v2', timestamp: 100000,
    frequency_hz: 539000000, sample_rate_hz: 2400000, channels: 2,
    reference_weights: [[1, 0], [0, 1]], bearing_replay: {method: 'covariance_mvdr',
      reference_anchor: 'fixed_site',
      scan_beams: 60, guard_radius: 0, training_radius: 1, diagonal_loading: .01,
      channel_mapping: [0, 1]},
    reference_state: {frames: 20, updates: 2, coherent_fraction: .8,
      coherent_gain_db: 5.5, scale: 1.2},
    data: [{cell: 5, delay_km: 10.5, doppler_hz: 18, snr_db: 9,
      response: [[1, 2], [3, 4]], covariance_samples: 8,
      covariance: [[[2, 0], [.5, .25]], [[.5, -.25], [3, 0]]]}]});
  const firstUpdate = JSON.parse(JSON.stringify(adaptive));
  firstUpdate.reference_state.frames = 0; firstUpdate.reference_state.updates = 1;
  assert.doesNotThrow(() => validateEvidence(firstUpdate),
    'The first analysis update precedes the first completed frame counter');
  const adaptiveFile = path.join(directory, 'adaptive.jsonl');
  const adaptiveConfig = JSON.parse(JSON.stringify(config));
  adaptiveConfig.process.bearing.manifold_recorder.path = adaptiveFile;
  const adaptiveRecorder = new ManifoldRecorder(adaptiveConfig, source);
  await adaptiveRecorder.record(adaptive);
  const adaptiveRow = JSON.parse(fs.readFileSync(adaptiveFile, 'utf8').trim());
  assert.equal(adaptiveRow.bearing_replay.method, 'covariance_mvdr');
  assert.equal(adaptiveRow.bearing_replay.reference_anchor, 'fixed_site');
  assert.deepEqual(adaptiveRow.covariance, adaptive.data[0].covariance);
  assert.equal(adaptiveRow.reference_state.updates, 2);
  const referenceFile = `${adaptiveFile}.reference.jsonl`;
  let referenceRows = fs.readFileSync(referenceFile, 'utf8').trim().split('\n').map(JSON.parse);
  assert.equal(referenceRows.length, 1);
  assert.equal(referenceRows[0].relative_eigenvector.anchor_channel, 0);
  assert.equal(referenceRows[0].relative_eigenvector.channels[1].phase_deg, 90);
  const moved = JSON.parse(JSON.stringify(adaptive));
  moved.timestamp += 5000; moved.reference_state.frames += 10; moved.reference_state.updates += 1;
  moved.reference_weights[1] = [Math.SQRT1_2, Math.SQRT1_2];
  await adaptiveRecorder.record(moved);
  referenceRows = fs.readFileSync(referenceFile, 'utf8').trim().split('\n').map(JSON.parse);
  assert.equal(referenceRows.length, 2);
  assert.ok(Math.abs(referenceRows[1].change_from_previous.max_phase_step_deg - 45) < 1e-9);
  assert.equal(adaptiveRecorder.status().referenceSamples, 2);
  fs.rmSync(directory, {recursive: true});
  console.log('Manifold recorder: automatic eligibility, site reset, sparse matching, and portable labelled JSONL passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
