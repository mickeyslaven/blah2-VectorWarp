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
    if (!finite(re) || !finite(im)) return false;
    if (i === j) { if (!(re > 0) || Math.abs(im) > 1e-9) return false; lower[i][j] = [Math.sqrt(re), 0]; }
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
  return validatePhysical(value);
}

function validatePhysical(value) {
  const pass = value.pass || value.selection, truth = value.truth || {};
  const config = value.configuration || {}, clock = value.sample_clock || {};
  const cut = value.target_cut || {}, covariance = value.covariance || {};
  const channels = config.channels;
  const norm = vector => { const n = Math.hypot(...vector.flat()); return finite(n) && n > 1e-12; };
  const start = clock.sample_start ?? clock.first_epoch_sample, end = clock.sample_end ?? clock.end_epoch_sample;
  const controls = value.local_control_cells;
  if (value.shadow_only !== true || value.hardware_qualified !== false ||
      value.production_promotion_allowed !== false || !value.epoch_id || typeof value.epoch_id !== 'string' ||
      !pass || typeof pass.id !== 'string' || !pass.id.trim() || typeof pass.icao !== 'string' ||
      !pass.icao.trim() || !['detected', 'miss', 'subthreshold'].includes(pass.label) ||
      truth.provenance !== 'adsb' || !finite(truth.local_azimuth_deg) ||
      !finite(truth.elevation_deg) || truth.elevation_deg < -90 || truth.elevation_deg > 90 ||
      !finite(truth.position_timestamp_ms ?? (truth.position_timestamp_s * 1000)) ||
      (truth.position_timestamp_ms ?? truth.position_timestamp_s * 1000) < 0 ||
      // Admission enforces the configured (usually much tighter) truth age.
      // The schema still rejects a different minute/hour time base outright.
      Math.abs((truth.position_timestamp_ms ?? truth.position_timestamp_s * 1000) - clock.timestamp_ms) > 300000 ||
      typeof config.config_fingerprint !== 'string' || !config.config_fingerprint.trim() ||
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
      start < 0 ||
      (clock.sample_start !== undefined && clock.first_epoch_sample !== undefined && clock.sample_start !== clock.first_epoch_sample) ||
      (clock.sample_end !== undefined && clock.end_epoch_sample !== undefined && clock.sample_end !== clock.end_epoch_sample) ||
      (clock.sample_end ?? clock.end_epoch_sample) <= (clock.sample_start ?? clock.first_epoch_sample) ||
      !finite(clock.source_sample_midpoint) || !finite(clock.timestamp_ms) || clock.timestamp_ms < 0 ||
      Math.abs(clock.source_sample_midpoint - ((clock.sample_start ?? clock.first_epoch_sample) +
        (clock.sample_end ?? clock.end_epoch_sample)) / 2) > 1e-9 ||
      clock.time_mapping !== 'admitted-source-sample-midpoints' || !clock.source_epoch ||
      !['number', 'string'].includes(typeof clock.source_epoch) ||
      (typeof clock.source_epoch === 'number' && (!Number.isSafeInteger(clock.source_epoch) || clock.source_epoch <= 0)) ||
      (config.cpi_seconds !== undefined && (!finite(config.cpi_seconds) || config.cpi_seconds <= 0 ||
        Math.abs(config.cpi_seconds * config.sample_rate_hz - (end - start)) > 1e-6)) ||
      (config.fusion_mode !== undefined && !['coherent_beam_bank', 'noise_whitened_beam_bank'].includes(config.fusion_mode)) ||
      !Array.isArray(value.source_snapshots) || !value.source_snapshots.length || value.source_snapshots.length > 8 ||
      value.source_snapshots.some(row => !sha256(row?.sha256) || row.epoch_id !== value.epoch_id ||
        !sha256(row.source_sha256) || !sha256(row.rf_sha256) || !sha256(row.native_config_sha256) ||
        !sha256(row.native_module_sha256) || !sha256(row.native_device_sha256) || !sha256(row.raw_iq_sha256)) ||
      covariance.epoch_id !== value.epoch_id || !sha256(covariance.sha256) ||
      covariance.state !== 'native-resident-reference-covariance' ||
      covariance.estimated_from_cut !== false ||
      !Number.isInteger(covariance.samples) || covariance.samples < 1 ||
      !complexMatrix(covariance.matrix, channels) || !hermitianPositiveDiagonal(covariance.matrix) ||
      cut.role !== 'target' || typeof cut.id !== 'string' || !cut.id || cut.epoch_id !== value.epoch_id ||
      !sha256(cut.raw_iq_sha256) || !complexVector(cut.physical_channels, channels) ||
      !(Number.isFinite(Math.hypot(...cut.physical_channels.flat())) && Math.hypot(...cut.physical_channels.flat()) > 1e-12) ||
      !complexVector(value.reference_weights, channels) || !norm(value.reference_weights) ||
      !complexBank(value.actual_fusion_weights, channels) ||
      value.weight_provenance !== 'native-coherent-fusion-weights' ||
      value.detector?.scope !== 'native-cfar-before-centroid' ||
      value.detector.production_cfar_recomputed !== false ||
      !Array.isArray(value.detector.statistics) || !value.detector.statistics.length || value.detector.statistics.length > 4 ||
      value.detector.statistics.some(row => !Array.isArray(row) || row.length !== 5 || !row.every(finite) ||
        row.slice(0, 4).some(number => number < 0) || ![1, 3].includes(row[4])) ||
      (pass.label === 'detected') !== value.detector.statistics.some(row => row[4] === 3) ||
      !Array.isArray(controls) || controls.length > 24 ||
      value.local_control_scope !== 'nearby physical CUTs; not certified target-free CFAR training' ||
      new Set(controls.map(cell => cell?.id)).size !== controls.length || controls.some(cell =>
        typeof cell?.id !== 'string' || !cell.id || cell.id === cut.id || !complexVector(cell.physical_channels, channels)) ||
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
  validatePhysical({...row, shadow_only: true, hardware_qualified: false, production_promotion_allowed: false,
    target_cut: {id: row.target_cut_id, role: 'target', epoch_id: row.epoch_id,
      raw_iq_sha256: row.raw_iq_sha256, physical_channels: row.physical_iq_cut}});
  if (row.version !== 1 || row.usage?.adsb !== 'truth-validation-only' || row.usage?.shadow !== 'training-only' ||
      !sha256(row.native_export_sha256) || row.hardware_qualified === true || row.production_promotion_allowed === true ||
      row.promotion_allowed === true || row.runtime_compatible === true ||
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
    actual_fusion_weights: source.actual_fusion_weights, weight_provenance: source.weight_provenance, detector: source.detector,
    target_cut_id: source.target_cut.id,
    local_control_cells: controls || [], local_control_scope: source.local_control_scope || 'nearby physical CUTs; not certified target-free CFAR training'};
  return signDerived(row, signingKey, keyId);
}

function appendNativeShadowRow(filename, row, options = {}) {
  return appendNativeShadowRows(filename, [row], options).at(-1);
}
function appendNativeShadowRows(filename, rows, options = {}) {
  if (!Array.isArray(rows) || !rows.length) return [];
  const lock = `${filename}.lock`; let lockFd;
  try {
    lockFd = fs.openSync(lock, fs.constants.O_CREAT | fs.constants.O_EXCL | fs.constants.O_WRONLY | fs.constants.O_NOFOLLOW, 0o600);
    const existing = readNativeShadowLedger(filename, options); let previous = existing.tail_sha256;
    if (existing.rows.length + rows.length > 20000) throw new Error('Native shadow ledger exceeds row bound');
    const payload = rows.map(row => { if (row.previous_row_sha256 !== previous) throw new Error('Append-only corpus predecessor does not match ledger'); validateNativeShadowManifoldRow(row, options); previous = row.row_sha256; return `${JSON.stringify(row)}\n`; }).join('');
    const oldBytes = fs.existsSync(filename) ? fs.statSync(filename).size : 0;
    if (oldBytes + Buffer.byteLength(payload) > 64 * 1024 * 1024) throw new Error('Native shadow ledger exceeds byte bound');
  // Canonical form is for signatures only. Persist ordinary JSON so the next
  // process sees numbers rather than canonical f64 wrapper objects.
    const fd = fs.openSync(filename, fs.constants.O_WRONLY | fs.constants.O_APPEND | fs.constants.O_CREAT |
    fs.constants.O_NOFOLLOW, 0o600);
    try {
      const stat = fs.fstatSync(fd);
      if (!stat.isFile() || stat.nlink !== 1 || stat.size !== oldBytes || stat.size + Buffer.byteLength(payload) > 64 * 1024 * 1024)
        throw new Error('Native shadow ledger changed or is not an unaliased bounded regular file');
      fs.writeFileSync(fd, payload, 'utf8'); fs.fsyncSync(fd);
    } finally { fs.closeSync(fd); }
    return rows.map(row => row.row_sha256);
  } finally { if (lockFd !== undefined) { fs.closeSync(lockFd); fs.unlinkSync(lock); } }
}

module.exports = {canonical, canonicalHash, validateNativeTruthExport, deriveNativeShadowManifoldRow,
  validateNativeShadowManifoldRow, readNativeShadowLedger, appendNativeShadowRow, appendNativeShadowRows};
