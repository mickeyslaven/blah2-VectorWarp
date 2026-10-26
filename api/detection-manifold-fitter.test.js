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
const model = fitDetectionManifold(samples(), {corpusSha256});
assert.equal(model.schema, 'vectorwarp-detection-manifold-candidate-v2');
assert.equal(model.cells.length, 32);
assert.equal(model.responses.length, beams);
assert.equal(model.runtime_compatible, false);
assert.throws(() => denseRuntimeCandidate(model), /shadow-only/);
assert.deepEqual(model.elevation_range_deg, [-90, 90], 'fixed physical axes do not collapse with constant elevation data');
assert.deepEqual(model.responses[0].elevation_range_deg, [5, 5]);
assert.equal(responseFor(model, {truth: {local_azimuth_deg: 17, elevation_deg: 6}}), null);
assert.equal(responseFor(model, {truth: {local_azimuth_deg: 17, elevation_deg: 1000}}), null);
assert.ok(responseFor(model, {truth: {local_azimuth_deg: 17, elevation_deg: 5}}));
assert.ok(correlation(model.responses[0].ideal_response, model.responses[2].ideal_response) < .99,
  'ideal shrinkage is direction dependent');
for (const options of [{azimuthBins: 1000000}, {elevationBins: 1000000},
  {minTrainingPassesPerBin: 1}, {elevationRangeDeg: [5, 5]}])
  assert.throws(() => fitDetectionManifold(samples(), options), /two-dimensional/);

const changedPhase = samples().map((row, index) => ({...row,
  response: row.response.map(value => multiply(value, [Math.cos(index * 1.7), Math.sin(index * 1.7)]))}));
const phased = fitDetectionManifold(changedPhase);
for (let index = 0; index < beams; ++index)
  assert.ok(correlation(model.responses[index].response, phased.responses[index].response) > 1 - 1e-12,
    'complex alignment and shrinkage must be invariant to arbitrary common phase');
const orthogonal = samples(), ideal = model.responses[0].ideal_response;
const perpendicular = normalize([[-ideal[1][0], ideal[1][1]], [ideal[0][0], -ideal[0][1]], [0, 0]]);
orthogonal[0].response = perpendicular; orthogonal[1].response = perpendicular;
const ambiguous = fitDetectionManifold(orthogonal);
assert.equal(ambiguous.cells.find(cell => cell.key === model.responses[0].key).support, 'incoherent');
assert.equal(responseFor(ambiguous, orthogonal[0]), null, 'unidentifiable shrinkage phase is unsupported');

const {buildDetectionCorpus, validateDetectionSample} = require('./detection-manifold-corpus');
const ordered = samples().slice(0, 3).map((row, index) => ({...clone(row), icao: 'same-aircraft',
  radar_timestamp_ms: 1000 + index * 1000}));
assert.equal(new Set(buildDetectionCorpus([ordered[2], ordered[0], ordered[1]]).training.map(r => r._pass_id)).size, 1,
  'shuffling one pass cannot manufacture independent passes');
const epochs = clone(ordered);
epochs[1].manifold_identity.actual_gain_db[0] += 1;
epochs[1].manifold_identity.compatibility_fingerprint = compatibilityFingerprint(epochs[1].manifold_identity);
epochs[1].configuration.receiver_gain_v2.actual_channel_gains_db[0] += 1;
const current = buildDetectionCorpus(epochs);
assert.equal(current.training.length, 1, 'returning to an earlier identity begins a fresh collection epoch');
assert.notEqual(current.evidence_key, buildDetectionCorpus(epochs.slice(0, 1)).evidence_key);
assert.throws(() => validateDetectionSample({...samples()[0], icao: ''}), /aircraft/);
assert.throws(() => validateDetectionSample({...samples()[0], radar_timestamp_ms: undefined}), /timestamp/);
assert.throws(() => validateDetectionSample({...samples()[0], response: [[0, 0], [0, 0], [0, 0]]}), /nonzero/);

assert.equal(detectionBankHash([[[1, -0], [0, 1], [1, 0]]], [[1, 0], [1, 0], [1, 0]]),
  detectionBankHash([[[1, 0], [0, 1], [1, 0]]], [[1, 0], [1, 0], [1, 0]]));
const {persistCandidate, readBounded} = require('./detection-manifold-artifacts');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'detection-artifact-test-'));
try {
  const filename = path.join(temporary, 'candidate.json');
  const digest = persistCandidate(filename, model);
  const evaluated = clone(model);
  evaluated.calibration.evaluation = {evaluated_at: new Date().toISOString()};
  evaluated.calibration.independent_validation_samples += 1;
  assert.equal(persistCandidate(filename, evaluated), digest, 'evaluation changes do not mutate a model artifact');
  fs.writeFileSync(filename, '{}');
  assert.throws(() => persistCandidate(filename, model), /Immutable/);
  assert.throws(() => readBounded(filename, 1), /read limit/);
  const link = path.join(temporary, 'link.json');
  fs.symlinkSync(filename, link);
  assert.throws(() => persistCandidate(link, model), /ELOOP/);
} finally { fs.rmSync(temporary, {recursive: true, force: true}); }
console.log('Detection fitter: bounded 2-D geometry, support, phase invariance, epoch isolation and immutable artifacts passed.');
