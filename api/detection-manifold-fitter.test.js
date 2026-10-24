'use strict';

const assert = require('assert');
const crypto = require('crypto');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {spawnSync} = require('child_process');
const {compatibilityFingerprint} = require('./manifold-identity');
const {BANK_HASH_CONTRACT, detectionBankHash, fitDetectionManifold,
  denseRuntimeCandidate, responseFor} = require('./detection-manifold-fitter');

const beams = 8, frequency = 539000000, directAzimuth = 26;
const positions = [[0, 0, 0], [.2, 0, 0], [0, .2, 0]];
const identity = {frequency_hz: frequency, sample_rate_hz: 2400000,
  channel_mapping: [0, 1, 2], requested_gain_db: [12, 12, 12],
  actual_gain_db: [12.1, 11.9, 12], positions_m: positions,
  receiver_location: [40, -80, 300], transmitter_location: [40.1, -80, 400],
  x_axis_bearing_deg_true: directAzimuth};
identity.compatibility_fingerprint = compatibilityFingerprint(identity);
const canonical = value => Array.isArray(value) ? `[${value.map(canonical).join(',')}]` :
  value && typeof value === 'object' ? `{${Object.keys(value).sort().map(key =>
    `${JSON.stringify(key)}:${canonical(value[key])}`).join(',')}}` : JSON.stringify(value);
const hash = value => crypto.createHash('sha256').update(canonical(value)).digest('hex');
const clone = value => JSON.parse(JSON.stringify(value));
const multiply = (a, b) => [a[0] * b[0] - a[1] * b[1], a[0] * b[1] + a[1] * b[0]];
const divide = (a, b) => {
  const norm = b[0] ** 2 + b[1] ** 2;
  return [(a[0] * b[0] + a[1] * b[1]) / norm, (a[1] * b[0] - a[0] * b[1]) / norm];
};
const normalize = vector => {
  const norm = Math.sqrt(vector.reduce((sum, value) => sum + value[0] ** 2 + value[1] ** 2, 0));
  return vector.map(value => value.map(part => part / norm));
};
const response = (azimuth, commonPhase = 0) => positions.map(([x, y]) => {
  const rad = azimuth * Math.PI / 180;
  const phase = commonPhase - 2 * Math.PI * frequency / 299792458 *
    (x * Math.cos(rad) + y * Math.sin(rad));
  return [Math.cos(phase) / Math.sqrt(positions.length), Math.sin(phase) / Math.sqrt(positions.length)];
});
const correlation = (left, right) => {
  let real = 0, imaginary = 0;
  left.forEach((value, index) => {
    real += value[0] * right[index][0] + value[1] * right[index][1];
    imaginary += value[0] * right[index][1] - value[1] * right[index][0];
  });
  return real ** 2 + imaginary ** 2;
};
function samples(elevations = () => 5) {
  const rows = [];
  for (let beam = 0; beam < beams; ++beam) for (let pass = 0; pass < 2; ++pass) {
    const azimuth = beam * 360 / beams + 17, commonPhase = beam * .13 + pass * .7;
    rows.push({schema: 'vectorwarp-manifold-sample-v2', frequency_hz: frequency,
      sample_rate_hz: 2400000, channels: positions.length, config_fingerprint: 'fixed-installed-state',
      manifold_identity: clone(identity), icao: `aircraft-${beam}-${pass}`, radar_timestamp_ms: 1000000,
      direct_path_local_azimuth_deg: directAzimuth,
      truth: {local_azimuth_deg: azimuth, elevation_deg: elevations(beam, pass)},
      detection: {snr_db: 12}, response: response(azimuth, commonPhase),
      reference_weights: response(directAzimuth, commonPhase),
      configuration: {frequency_hz: frequency, sample_rate_hz: 2400000,
        config_fingerprint: 'fixed-installed-state',
        receiver_gain_v2: {requested_common_gain: 12, requested_channel_gains: null,
          actual_channel_gains_db: [...identity.actual_gain_db]}},
      support: {adsb_label_only: true, covariance_samples: 0, local_nuisance_cells: 0},
      uncertainty: {truth_age_s: 0, delay_error_km: 0, doppler_error_hz: 0, normalized_match_error: 0}});
  }
  return rows;
}

const corpusSha256 = hash(samples());
const sparse = fitDetectionManifold(samples(), {corpusSha256});
const dense = denseRuntimeCandidate(sparse);
assert.equal(dense.direct_path_beam, 1, 'off-grid transmitter uses the rounded native beam');
assert.equal(sparse.responses.find(row => row.azimuth_bin === 1).training_passes, 2);
assert.ok(correlation(dense.direct_anchor.response, response(directAzimuth)) > 1 - 1e-12,
  'the fixed-transmitter anchor is retained separately from aircraft beams');
assert.ok(correlation(dense.responses[1], response(62)) > 1 - 1e-12,
  'the aircraft beam nearest the transmitter remains available for detection');
assert.ok(correlation(sparse.responses.find(row => row.azimuth_bin === 1).response,
  response(62)) > 1 - 1e-12, 'diagnostic aircraft templates are not overwritten by the direct anchor');
const targetBeam = 2, targetAzimuth = targetBeam * 45 + 17;
// This is CoherentBeamBank's unwhitened ratio calibration. It must recover the
// measured target vector up to common phase even when neither angle is on-grid.
const nativeSteering = normalize(dense.responses[targetBeam].map((value, channel) =>
  multiply(response(directAzimuth)[channel], divide(value, dense.direct_anchor.response[channel]))));
assert.ok(correlation(nativeSteering, response(targetAzimuth)) > 1 - 1e-12);
assert.deepEqual(dense.elevation_range_deg, [5, 5]);
assert.equal(dense.sample_rate_hz, identity.sample_rate_hz);
assert.deepEqual(dense.channel_mapping, identity.channel_mapping);
assert.deepEqual(dense.manifold_identity, identity);
assert.equal(dense.manifold_identity_sha256, hash(identity));
assert.equal(dense.config_fingerprint, 'fixed-installed-state');
assert.equal(dense.positive_corpus_sha256, corpusSha256);
assert.equal(dense.bank_hash_contract, BANK_HASH_CONTRACT);
assert.equal(dense.bank_sha256, detectionBankHash(dense.responses,
  dense.direct_anchor.response));
assert.equal(detectionBankHash([[[1, -0], [0, 1], [1, 0]]], [[1, 0], [1, 0], [1, 0]]),
  detectionBankHash([[[1, 0], [0, 1], [1, 0]]], [[1, 0], [1, 0], [1, 0]]),
  'bank identity must survive JSON signed-zero normalization');

const repeated = samples();
repeated[0].reference_weights = response(directAzimuth + 5);
const originalAnchor = fitDetectionManifold(repeated).direct_anchor;
const duplicated = repeated.flatMap((row, index) => index === 0 ? Array.from({length: 40}, () => clone(row)) : [row]);
const repeatedAnchor = fitDetectionManifold(duplicated).direct_anchor;
assert.equal(repeatedAnchor.training_passes, originalAnchor.training_passes);
assert.ok(correlation(originalAnchor.response, repeatedAnchor.response) > 1 - 1e-12,
  'repeated frames from one pass do not change the direct anchor vote');

const disjoint = fitDetectionManifold(samples(beam => beam === 0 ? 0 : 80));
assert.throws(() => denseRuntimeCandidate(disjoint), /common observed elevation support/);
const shared = denseRuntimeCandidate(fitDetectionManifold(samples((beam, pass) =>
  pass === 0 ? beam : 20 - beam)));
assert.deepEqual(shared.elevation_range_deg, [7, 13],
  'support is the intersection of every retained aircraft beam');
assert.equal(shared.aircraft_support.length, beams);
assert.equal(responseFor(sparse, {truth: {local_azimuth_deg: targetAzimuth, elevation_deg: 40}}), null,
  'diagnostic lookup also refuses unobserved elevation extrapolation');
const brokenAnchor = clone(sparse); brokenAnchor.direct_anchor.beam = 0;
assert.throws(() => denseRuntimeCandidate(brokenAnchor), /fixed-transmitter direct anchor/);
const brokenIdentity = clone(sparse); brokenIdentity.manifold_identity.actual_gain_db[0] += 1;
assert.throws(() => denseRuntimeCandidate(brokenIdentity), /not exactly convertible/);

// Optional native acceptance uses the same generated fitter artifact. The
// normal JS suite needs no compiled tool; CI/review can additionally supply it.
const nativeIndex = process.argv.indexOf('--native');
if (nativeIndex !== -1) {
  const binary = path.resolve(process.argv[nativeIndex + 1]);
  const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'detection-fitter-native-'));
  try {
    const baseline = {frequency_hz: frequency, beams, positions_m: positions,
      direct_path_azimuth_rad: directAzimuth * Math.PI / 180, direct_path_null_db: 0};
    const grid = {schema: 'vectorwarp-detection-manifold-post-caf-grid-input-v1', same_iq: false,
      production_equivalent: false, truth_selected_beam: false,
      configuration: {...baseline, channels: positions.length, fusion_mode: 'coherent_beam_bank'},
      banks: {baseline: {sha256: hash(baseline), beams},
        candidate: {...dense, sha256: dense.bank_sha256,
          direct_response: dense.direct_anchor.response}},
      reference_weights: response(directAzimuth),
      detector: {contract: 'vectorwarp-production-ca-cfar-full-bank-v1', guard_cells: 0,
        training_cells_per_side: 1, integration_cpis: 1, motion_trials: 1, detector_pfa: .01},
      truth_label: {selection: 'target-cut-only-no-beam-selection', target_cut_id: 'target',
        local_azimuth_deg: targetAzimuth, elevation_deg: 5},
      cuts: ['target', 'nuisance'].map(role => ({id: role, role,
        cut: response(targetAzimuth), training: [-1, 1].map(offset => ({offset,
          channels: [[.1, 0], [0, .1], [.1, .1]]}))})), provenance: {}};
    const input = path.join(temporary, 'input.json'), output = path.join(temporary, 'output.json');
    fs.writeFileSync(input, JSON.stringify(grid));
    const run = spawnSync(binary, [input, output], {encoding: 'utf8'});
    assert.equal(run.status, 0, run.stderr);
    const emitted = JSON.parse(fs.readFileSync(output, 'utf8'));
    const target = emitted.cuts.find(row => row.id === 'target');
    assert.ok(Math.abs(target.candidate.cut_power_by_beam[targetBeam] - 1) < 1e-10,
      'native calibration preserves the perfect off-grid measured response');
    assert.ok(Math.max(...target.candidate.cut_power_by_beam) >
      Math.max(...target.baseline.cut_power_by_beam) + .01);
  } finally {
    fs.rmSync(temporary, {recursive: true, force: true});
  }
}
console.log('Detection fitter: fixed direct anchor, pass balancing, joint elevation support, and exact identity passed.');
