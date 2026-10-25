'use strict';

const assert = require('assert');
const crypto = require('crypto');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {canonical, canonicalHash, validateNativeTruthExport, deriveNativeShadowManifoldRow,
  validateNativeShadowManifoldRow, appendNativeShadowRow} = require('./native-shadow-manifold-evidence');
const {readNativeShadowLedger} = require('./native-shadow-manifold-evidence');

const key = 'native-receipt-test-key', keyId = 'native-receipt-test-key-id';
const hmac = value => crypto.createHmac('sha256', key).update(canonical(value)).digest('hex');
const unsigned = value => { const copy = {...value}; delete copy.authentication; delete copy.export_sha256; return copy; };
function nativeExport() {
  const matrix = [[[1, 0], [0, 0]], [[0, 0], [1, 0]]];
  const row = {schema: 'vectorwarp-native-truth-export-v1', shadow_only: true,
    hardware_qualified: false, production_promotion_allowed: false, epoch_id: 'rf-epoch-7',
    pass: {id: 'pass-001', icao: 'a1b2c3', label: 'miss'},
    truth: {local_azimuth_deg: 22.5, elevation_deg: 4, position_timestamp_s: 1710000000,
      provenance: 'adsb'}, configuration: {config_fingerprint: 'config-precise-1',
      frequency_hz: 563000000, sample_rate_hz: 2400000, channels: 2,
      actual_channel_gains_db: [18, 19], channel_mapping: [0, 2]},
    sample_clock: {admitted: true, epoch_id: 'rf-epoch-7', sample_start: 100, sample_end: 199,
      source_epoch: 'source-7', source_sample_midpoint: 149.5, timestamp_ms: 1710000000000,
      time_mapping: 'admitted-source-sample-midpoints'},
    source_snapshots: [{sha256: 'a'.repeat(64), epoch_id: 'rf-epoch-7',
      source_sha256: 'b'.repeat(64), rf_sha256: 'c'.repeat(64), native_config_sha256: 'd'.repeat(64),
      native_module_sha256: 'e'.repeat(64), native_device_sha256: 'f'.repeat(64), raw_iq_sha256: 'b'.repeat(64)}],
    covariance: {epoch_id: 'rf-epoch-7', samples: 64, matrix, sha256: canonicalHash(matrix),
      state: 'native-resident-reference-covariance', estimated_from_cut: false}, target_cut: {id: 'cut-19', role: 'target',
      epoch_id: 'rf-epoch-7', raw_iq_sha256: 'b'.repeat(64),
      physical_channels: [[.5, .1], [.2, -.3]]},
    reference_weights: [[1, 0], [.4, -.2]], actual_fusion_weights: [[1, 0], [.4, -.2]],
    weight_provenance: 'native-coherent-fusion-weights',
    local_control_cells: [], local_control_scope: 'nearby physical CUTs; not certified target-free CFAR training',
    detector: {scope: 'native-cfar-before-centroid', production_cfar_recomputed: false,
      statistics: [[1, 2, 3, 4, 1]]}};
  row.export_sha256 = canonicalHash(unsigned(row));
  row.authentication = {algorithm: 'hmac-sha256', key_id: keyId, mac: hmac(unsigned(row))};
  return row;
}
const resolver = id => id === keyId ? key : null;
const source = nativeExport();
assert.doesNotThrow(() => validateNativeTruthExport(source, {resolveKey: resolver}));
const derived = deriveNativeShadowManifoldRow(source, {resolveKey: resolver, signingKey: key, keyId});
assert.doesNotThrow(() => validateNativeShadowManifoldRow(derived, {resolveKey: resolver}));
assert.equal(derived.usage.adsb, 'truth-validation-only');
assert.equal(derived.usage.shadow, 'training-only');
assert.deepEqual(derived.physical_iq_cut, source.target_cut.physical_channels);
assert.deepEqual(derived.actual_fusion_weights, source.actual_fusion_weights);

for (const mutate of [row => { row.configuration.actual_channel_gains_db[0] += 1; },
  row => { row.target_cut.raw_iq_sha256 = '0'.repeat(64); },
  row => { row.covariance.epoch_id = 'other-epoch'; },
  row => { row.configuration.frequency_hz += 1; },
  row => { row.configuration.sample_rate_hz += 1; },
  row => { row.sample_clock.source_epoch = ''; },
  row => { row.target_cut.physical_channels = [[0, 0], [0, 0]]; },
  row => { row.sample_clock.admitted = false; },
  row => { row.anchor = true; }, row => { row.beam_powers = [1, 2]; }]) {
  const bad = JSON.parse(JSON.stringify(source)); mutate(bad);
  assert.throws(() => validateNativeTruthExport(bad, {resolveKey: resolver}), /invalid|failed|lacks/i);
}
const tampered = JSON.parse(JSON.stringify(derived)); tampered.truth.local_azimuth_deg += 1;
assert.throws(() => validateNativeShadowManifoldRow(tampered, {resolveKey: resolver}), /hash/i);
// Valid signatures are necessary but not sufficient: malformed semantics must
// also fail when a trusted exporter signs an incomplete or mislabeled payload.
for (const mutate of [row => { delete row.covariance.samples; },
  row => { row.covariance.state = 'estimated-from-target'; },
  row => { row.covariance.matrix = [[[1, 0], [2, 0]], [[2, 0], [1, 0]]]; row.covariance.sha256 = canonicalHash(row.covariance.matrix); },
  row => { row.pass.id = ' '; }, row => { delete row.configuration.actual_channel_gains_db; },
  row => { row.sample_clock.source_sample_midpoint += 1; },
  row => { row.sample_clock.time_mapping = 'wall-clock'; },
  row => { row.sample_clock.timestamp_ms += 600000; },
  row => { row.truth.elevation_deg = 100; }, row => { row.target_cut.raw_iq_sha256 = '0'.repeat(64); },
  row => { row.reference_weights = [[1e308, 1e308], [1e308, 1e308]]; },
  row => { delete row.local_control_cells; }, row => { row.detector.statistics[0][4] = 3; }]) {
  const bad = JSON.parse(JSON.stringify(source)); mutate(bad);
  bad.export_sha256 = canonicalHash(unsigned(bad));
  bad.authentication.mac = hmac(unsigned(bad));
  assert.throws(() => validateNativeTruthExport(bad, {resolveKey: resolver}), /required|invalid|bound/i);
}
for (const mutate of [row => { delete row.covariance.samples; },
  row => { row.sample_clock.source_sample_midpoint += 1; },
  row => { row.sample_clock.time_mapping = 'wall-clock'; }, row => { row.truth.elevation_deg = 100; },
  row => { delete row.local_control_cells; }, row => { delete row.detector; },
  row => { row.physical_iq_cut[0][0] = 1e308; row.physical_iq_cut[0][1] = 1e308; row.physical_iq_cut[1] = [1e308, 1e308]; }]) {
  const bad = JSON.parse(JSON.stringify(derived)); mutate(bad);
  delete bad.row_sha256; delete bad.authentication;
  bad.row_sha256 = canonicalHash(bad);
  bad.authentication = {algorithm: 'hmac-sha256', key_id: keyId, mac: hmac(bad)};
  assert.throws(() => validateNativeShadowManifoldRow(bad, {resolveKey: resolver}), /required|invalid|contract/i);
}
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'native-shadow-ledger-'));
const ledger = path.join(directory, 'shadow.jsonl');
assert.equal(appendNativeShadowRow(ledger, derived, {resolveKey: resolver}), derived.row_sha256);
const second = deriveNativeShadowManifoldRow(source, {resolveKey: resolver, signingKey: key, keyId,
  previousRowSha256: derived.row_sha256});
assert.equal(appendNativeShadowRow(ledger, second, {resolveKey: resolver}), second.row_sha256);
assert.equal(readNativeShadowLedger(ledger, {resolveKey: resolver, expectedTailSha256: second.row_sha256})
  .rows.length, 2, 'persisted JSON ledger must authenticate on a fresh read');
const lines = fs.readFileSync(ledger, 'utf8').trim().split('\n');
fs.writeFileSync(ledger, `${lines[1]}\n${lines[0]}\n`);
assert.throws(() => readNativeShadowLedger(ledger, {resolveKey: resolver}), /chain/i,
  'reordered signed rows must fail chain continuity');
fs.writeFileSync(ledger, `${lines[0]}\n`);
assert.throws(() => readNativeShadowLedger(ledger, {resolveKey: resolver,
  expectedTailSha256: second.row_sha256}), /truncated|expected tail/i,
  'a known ledger tail must detect truncation');
fs.writeFileSync(ledger, `${lines.join('\n')}\n`);
const corrupted = JSON.parse(lines[1]); corrupted.physical_iq_cut[0][0] += .01;
fs.writeFileSync(ledger, `${lines[0]}\n${JSON.stringify(corrupted)}\n`);
assert.throws(() => readNativeShadowLedger(ledger, {resolveKey: resolver}), /hash|authentication/i,
  'target or nuisance-like complex-vector tampering must fail authentication');
assert.throws(() => appendNativeShadowRow(ledger, derived, {resolveKey: resolver}), /predecessor|hash|authentication/i);
fs.writeFileSync(ledger, `${lines.join('\n')}\n`);
fs.writeFileSync(`${ledger}.lock`, 'another writer owns this lock');
assert.throws(() => appendNativeShadowRow(ledger, derived, {resolveKey: resolver}), /EEXIST/);
assert.equal(fs.readFileSync(`${ledger}.lock`, 'utf8'), 'another writer owns this lock',
  'failed lock acquisition must not remove another writer lock');
fs.rmSync(directory, {recursive: true});
console.log('Native shadow manifold evidence: authenticated physical-IQ-only append-only adapter passed.');
