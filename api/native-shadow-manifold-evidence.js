'use strict';

// Strict import boundary for native truth exports.  This module deliberately
// does not turn beam powers into complex data: only the native physical-IQ CUT
// and native fusion weights are admitted.
const crypto = require('crypto');
const fs = require('fs');
const {readBounded} = require('./detection-manifold-artifacts');

const finite = value => typeof value === 'number' && Number.isFinite(value);
const sha256 = value => typeof value === 'string' && /^[a-f0-9]{64}$/.test(value);
// Match IntegrationShadowQueue receipts exactly.  Decimal JSON is not a stable
// cross-runtime representation for signed RF measurements.
const canonical = value => typeof value === 'number' ? (() => {
  if (!Number.isFinite(value)) throw new TypeError('canonical native evidence requires finite numbers');
  const bytes = Buffer.allocUnsafe(8); bytes.writeDoubleBE(value === 0 ? 0 : value);
  return `{"$f64":"${bytes.toString('hex')}"}`;
})() : Array.isArray(value) ? `[${value.map(canonical).join(',')}]` :
  value && typeof value === 'object' ? `{${Object.keys(value).sort().map(key =>
    `${JSON.stringify(key)}:${canonical(value[key])}`).join(',')}}` : JSON.stringify(value);
const canonicalHash = value => crypto.createHash('sha256').update(canonical(value)).digest('hex');
const hmac = (key, value) => crypto.createHmac('sha256', key).update(canonical(value)).digest('hex');
const complex = value => Array.isArray(value) && value.length === 2 && value.every(finite);
const complexVector = (value, channels) => Array.isArray(value) && value.length === channels &&
  value.every(complex);
const complexBank = (value, channels) => Array.isArray(value) && value.length >= channels &&
  value.length <= 360 * channels && value.length % channels === 0 && value.every(complex);
const complexMatrix = (value, channels) => Array.isArray(value) && value.length === channels &&
  value.every(row => complexVector(row, channels));
const finiteVector = (value, length) => Array.isArray(value) && value.length === length &&
  value.every(finite);

function without(value, ...keys) {
  const copy = {...value}; for (const key of keys) delete copy[key]; return copy;
}

function hermitianPositiveDiagonal(matrix) {
  for (let row = 0; row < matrix.length; ++row) {
    if (!(matrix[row][row][0] > 0) || matrix[row][row][1] !== 0) return false;
    for (let column = row + 1; column < matrix.length; ++column)
      if (Math.abs(matrix[row][column][0] - matrix[column][row][0]) > 1e-9 ||
          Math.abs(matrix[row][column][1] + matrix[column][row][1]) > 1e-9) return false;
  }
  // Complex Cholesky: positive diagonal alone is not positive definite.
  const n = matrix.length, lower = Array.from({length: n}, () => Array(n));
  for (let i = 0; i < n; ++i) for (let j = 0; j <= i; ++j) {
    let re = matrix[i][j][0], im = matrix[i][j][1];
    for (let k = 0; k < j; ++k) { const a = lower[i][k], b = lower[j][k];
      re -= a[0] * b[0] + a[1] * b[1]; im -= -a[0] * b[1] + a[1] * b[0]; }
    if (i === j) { if (!(re > 1e-12) || Math.abs(im) > 1e-9) return false; lower[i][j] = [Math.sqrt(re), 0]; }
    else { const d = lower[j][j][0]; if (!(d > 0)) return false; lower[i][j] = [re / d, im / d]; }
  }
  return true;
}

function verifyAuthentication(value, resolveKey) {
  const auth = value?.authentication;
  if (!auth || auth.algorithm !== 'hmac-sha256' || typeof auth.key_id !== 'string' ||
      !auth.key_id || !sha256(auth.mac) || typeof resolveKey !== 'function')
    throw new Error('Native export is missing authenticated provenance');
  const key = resolveKey(auth.key_id);
  if (!(typeof key === 'string' || Buffer.isBuffer(key)) || !key.length ||
      !crypto.timingSafeEqual(Buffer.from(auth.mac, 'hex'), Buffer.from(hmac(key,
        without(value, 'authentication', 'export_sha256')), 'hex')))
    throw new Error('Native export authentication failed');
}

function validateNativeTruthExport(value, {resolveKey} = {}) {
  if (!value || value.schema !== 'vectorwarp-native-truth-export-v1' ||
      !sha256(value.export_sha256) ||
      value.export_sha256 !== canonicalHash(without(value, 'authentication', 'export_sha256')))
    throw new Error('Native export has an invalid deterministic hash');
  verifyAuthentication(value, resolveKey);
  const pass = value.pass || value.selection, truth = value.truth || {};
  const config = value.configuration || {}, clock = value.sample_clock || {};
  const cut = value.target_cut || {}, covariance = value.covariance || {};
  const channels = config.channels;
  if (value.shadow_only !== true || value.hardware_qualified !== false ||
      value.production_promotion_allowed !== false || !value.epoch_id || typeof value.epoch_id !== 'string' ||
      !pass || typeof pass.id !== 'string' || !pass.id || typeof pass.icao !== 'string' ||
      !pass.icao || !['detected', 'miss', 'subthreshold'].includes(pass.label) ||
      truth.provenance !== 'adsb' || !finite(truth.local_azimuth_deg) ||
      !finite(truth.elevation_deg) || truth.elevation_deg < -90 || truth.elevation_deg > 90 ||
      !finite(truth.position_timestamp_ms ?? (truth.position_timestamp_s * 1000)) ||
      typeof config.config_fingerprint !== 'string' || !config.config_fingerprint ||
      !finite(config.frequency_hz) || config.frequency_hz <= 0 ||
      !finite(config.sample_rate_hz) || config.sample_rate_hz <= 0 ||
      !Number.isInteger(channels) || channels < 2 || channels > 8 ||
      !finiteVector(config.actual_channel_gains_db, channels) ||
      !Array.isArray(config.channel_mapping) || config.channel_mapping.length !== channels ||
      config.channel_mapping.some(v => !Number.isInteger(v) || v < 0) ||
      new Set(config.channel_mapping).size !== channels ||
      clock.admitted !== true || clock.epoch_id !== value.epoch_id ||
      !Number.isSafeInteger(clock.sample_start ?? clock.first_epoch_sample) ||
      !Number.isSafeInteger(clock.sample_end ?? clock.end_epoch_sample) ||
      (clock.sample_end ?? clock.end_epoch_sample) <= (clock.sample_start ?? clock.first_epoch_sample) ||
      !finite(clock.source_sample_midpoint) || !finite(clock.timestamp_ms) || clock.timestamp_ms < 0 ||
      Math.abs(clock.source_sample_midpoint - ((clock.sample_start ?? clock.first_epoch_sample) +
        (clock.sample_end ?? clock.end_epoch_sample)) / 2) > 1e-9 ||
      clock.time_mapping !== 'admitted-source-sample-midpoints' || !clock.source_epoch ||
      !Array.isArray(value.source_snapshots) || !value.source_snapshots.length ||
      value.source_snapshots.some(row => !sha256(row?.sha256) || row.epoch_id !== value.epoch_id ||
        !sha256(row.source_sha256) || !sha256(row.rf_sha256) || !sha256(row.native_config_sha256) ||
        !sha256(row.native_module_sha256) || !sha256(row.native_device_sha256) || !sha256(row.raw_iq_sha256)) ||
      covariance.epoch_id !== value.epoch_id || !sha256(covariance.sha256) ||
      covariance.state !== 'native-resident-reference-covariance' ||
      !Number.isInteger(covariance.samples) || covariance.samples < 1 ||
      !complexMatrix(covariance.matrix, channels) || !hermitianPositiveDiagonal(covariance.matrix) ||
      cut.role !== 'target' || !cut.id || cut.epoch_id !== value.epoch_id ||
      !sha256(cut.raw_iq_sha256) || !complexVector(cut.physical_channels, channels) ||
      !(Number.isFinite(Math.hypot(...cut.physical_channels.flat())) && Math.hypot(...cut.physical_channels.flat()) > 1e-12) ||
      !complexVector(value.reference_weights, channels) || !(Math.hypot(...value.reference_weights.flat()) > 1e-12) ||
      !complexBank(value.actual_fusion_weights, channels) ||
      value.weight_provenance !== 'native-coherent-fusion-weights' ||
      value.detector?.scope !== 'native-cfar-before-centroid' ||
      !Array.isArray(value.detector.statistics) || !value.detector.statistics.length ||
      value.beam_powers !== undefined || value.anchor === true || value.is_anchor === true ||
      value.reference_anchor === true || value.power_only_response === true)
    throw new Error('Native export lacks required same-epoch physical-IQ, RF, gain, covariance, or pass evidence');
  if (covariance.sha256 !== canonicalHash(covariance.matrix))
    throw new Error('Native export covariance hash is invalid');
  if (!value.source_snapshots.some(snapshot => snapshot.raw_iq_sha256 === cut.raw_iq_sha256))
    throw new Error('Native export raw physical-IQ hash is not bound to a native snapshot');
  return value;
}

function signDerived(row, signingKey, keyId) {
  if (!(typeof signingKey === 'string' || Buffer.isBuffer(signingKey)) || !signingKey.length ||
      typeof keyId !== 'string' || !keyId) throw new Error('Derived corpus signing key is required');
  const unsigned = without(row, 'authentication', 'row_sha256');
  const row_sha256 = canonicalHash(unsigned);
  const authentication = {algorithm: 'hmac-sha256', key_id: keyId,
    mac: hmac(signingKey, {...unsigned, row_sha256})};
  return {...unsigned, row_sha256, authentication};
}

function validateNativeShadowManifoldRow(row, {resolveKey} = {}) {
  if (!row || row.schema !== 'vectorwarp-native-shadow-manifold-row-v1' || !sha256(row.row_sha256) ||
      row.row_sha256 !== canonicalHash(without(row, 'authentication', 'row_sha256')))
    throw new Error('Shadow corpus row hash is invalid');
  const auth = row.authentication;
  if (!auth || auth.algorithm !== 'hmac-sha256' || typeof auth.key_id !== 'string' ||
      !sha256(auth.mac) || typeof resolveKey !== 'function')
    throw new Error('Shadow corpus row lacks authentication');
  const key = resolveKey(auth.key_id);
  if (!(typeof key === 'string' || Buffer.isBuffer(key)) || !key.length || !crypto.timingSafeEqual(
    Buffer.from(auth.mac, 'hex'), Buffer.from(hmac(key, {...without(row, 'authentication'),
      row_sha256: row.row_sha256}), 'hex')))
    throw new Error('Shadow corpus authentication failed');
  if (!['detected', 'miss', 'subthreshold'].includes(row.pass?.label) ||
      typeof row.pass?.id !== 'string' || !row.pass.id || typeof row.pass?.icao !== 'string' || !row.pass.icao ||
      typeof row.epoch_id !== 'string' || !row.epoch_id || !finite(row.truth?.local_azimuth_deg) ||
      !finite(row.truth?.elevation_deg) || !finite(row.truth?.position_timestamp_ms) ||
      row.truth?.provenance !== 'adsb' || typeof row.configuration?.config_fingerprint !== 'string' ||
      !finite(row.configuration?.frequency_hz) || row.configuration.frequency_hz <= 0 ||
      !finite(row.configuration?.sample_rate_hz) || row.configuration.sample_rate_hz <= 0 ||
      !Number.isInteger(row.configuration?.channels) || row.configuration.channels < 2 ||
      !finiteVector(row.configuration.actual_channel_gains_db, row.configuration.channels) ||
      !Array.isArray(row.configuration.channel_mapping) || row.configuration.channel_mapping.length !== row.configuration.channels ||
      row.sample_clock?.epoch_id !== row.epoch_id || row.sample_clock?.admitted !== true ||
      row.usage?.adsb !== 'truth-validation-only' || row.usage?.shadow !== 'training-only' ||
      !sha256(row.native_export_sha256) || !sha256(row.raw_iq_sha256) ||
      !Array.isArray(row.physical_iq_cut) || !Array.isArray(row.reference_weights) ||
      !Array.isArray(row.actual_fusion_weights) ||
      row.weight_provenance !== 'native-coherent-fusion-weights' ||
      row.previous_row_sha256 !== null && !sha256(row.previous_row_sha256))
    throw new Error('Shadow corpus row violates the training-only native evidence contract');
  return row;
}

function readNativeShadowLedger(filename, {expectedTailSha256 = null, ...options} = {}) {
  if (!fs.existsSync(filename)) return {rows: [], tail_sha256: null};
  const lines = readBounded(filename).toString('utf8').trim().split('\n').filter(Boolean);
  if (lines.length > 20000) throw new Error('Native shadow ledger exceeds row bound');
  const rows = []; let previous = null;
  for (const line of lines) {
    let row; try { row = JSON.parse(line); } catch (_) { throw new Error('Native shadow ledger contains invalid JSON'); }
    row = validateNativeShadowManifoldRow(row, options);
    if (row.previous_row_sha256 !== previous) throw new Error('Native shadow ledger chain is discontinuous');
    rows.push(row); previous = row.row_sha256;
  }
  if (expectedTailSha256 !== null && previous !== expectedTailSha256)
    throw new Error('Native shadow ledger is truncated or does not match the expected tail');
  return {rows, tail_sha256: previous};
}

function deriveNativeShadowManifoldRow(nativeExport, {resolveKey, signingKey, keyId,
  previousRowSha256 = null} = {}) {
  const source = validateNativeTruthExport(nativeExport, {resolveKey});
  if (previousRowSha256 !== null && !sha256(previousRowSha256))
    throw new Error('Append-only predecessor hash is invalid');
  const truth = source.truth;
  const controls = source.local_control_cells;
  if (controls !== undefined && (!Array.isArray(controls) || controls.length > 24 ||
      source.local_control_scope !== 'nearby physical CUTs; not certified target-free CFAR training' ||
      new Set(controls.map(cell => cell?.id)).size !== controls.length ||
      controls.some(cell => !cell?.id || cell.id === source.target_cut.id ||
        !complexVector(cell.physical_channels, source.configuration.channels))))
    throw new Error('Native local-control evidence is invalid');
  const row = {schema: 'vectorwarp-native-shadow-manifold-row-v1', version: 1,
    previous_row_sha256: previousRowSha256, native_export_sha256: source.export_sha256,
    pass: {id: source.pass?.id ?? source.selection.id, icao: source.pass?.icao ?? source.selection.icao,
      label: source.pass?.label ?? source.selection.label},
    usage: {adsb: 'truth-validation-only', shadow: 'training-only'}, epoch_id: source.epoch_id,
    ...(source.collection_epoch ? {collection_epoch: source.collection_epoch} : {}),
    truth: {local_azimuth_deg: truth.local_azimuth_deg, elevation_deg: truth.elevation_deg,
      position_timestamp_ms: truth.position_timestamp_ms ?? truth.position_timestamp_s * 1000,
      provenance: 'adsb'}, configuration: source.configuration,
    sample_clock: source.sample_clock, source_snapshots: source.source_snapshots,
    covariance: source.covariance, raw_iq_sha256: source.target_cut.raw_iq_sha256,
    physical_iq_cut: source.target_cut.physical_channels, reference_weights: source.reference_weights,
    actual_fusion_weights: source.actual_fusion_weights, weight_provenance: source.weight_provenance,
    target_cut_id: source.target_cut.id,
    ...(controls ? {local_control_cells: controls, local_control_scope: source.local_control_scope} : {})};
  return signDerived(row, signingKey, keyId);
}

function appendNativeShadowRow(filename, row, options = {}) {
  const existing = readNativeShadowLedger(filename, options);
  const previous = existing.tail_sha256;
  if (row.previous_row_sha256 !== previous) throw new Error('Append-only corpus predecessor does not match ledger');
  validateNativeShadowManifoldRow(row, options);
  // Canonical form is for signatures only. Persist ordinary JSON so the next
  // process sees numbers rather than canonical f64 wrapper objects.
  const fd = fs.openSync(filename, fs.constants.O_WRONLY | fs.constants.O_APPEND | fs.constants.O_CREAT |
    fs.constants.O_NOFOLLOW, 0o600);
  try {
    fs.writeSync(fd, `${JSON.stringify(row)}\n`, null, 'utf8');
    if (fs.fstatSync(fd).size > 64 * 1024 * 1024) throw new Error('Native shadow ledger exceeds byte bound');
  } finally { fs.closeSync(fd); }
  return row.row_sha256;
}

module.exports = {canonical, canonicalHash, validateNativeTruthExport, deriveNativeShadowManifoldRow,
  validateNativeShadowManifoldRow, readNativeShadowLedger, appendNativeShadowRow};
