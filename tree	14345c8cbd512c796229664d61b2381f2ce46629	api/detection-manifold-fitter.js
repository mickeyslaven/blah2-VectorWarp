'use strict';

const crypto = require('crypto');
const {buildDetectionCorpus} = require('./detection-manifold-corpus');
const {validIdentity} = require('./manifold-identity');

const finite = value => typeof value === 'number' && Number.isFinite(value);
const add = (a, b) => [a[0] + b[0], a[1] + b[1]];
const multiply = (a, b) => [a[0] * b[0] - a[1] * b[1], a[0] * b[1] + a[1] * b[0]];
const conjugate = a => [a[0], -a[1]];
const inner = (a, b) => a.reduce((sum, value, index) =>
  add(sum, multiply(conjugate(value), b[index])), [0, 0]);
const power = value => value[0] ** 2 + value[1] ** 2;
const normalize = vector => {
  const norm = Math.sqrt(vector.reduce((sum, value) => sum + power(value), 0));
  return norm > 0 && finite(norm) ? vector.map(value => [value[0] / norm, value[1] / norm]) : null;
};
const angle = value => ((value % 360) + 360) % 360;
const azimuthBin = (value, bins) => Math.round(angle(value) / 360 * bins) % bins;
const canonical = value => Array.isArray(value) ? `[${value.map(canonical).join(',')}]` :
  value && typeof value === 'object' ? `{${Object.keys(value).sort().map(key =>
    `${JSON.stringify(key)}:${canonical(value[key])}`).join(',')}}` : JSON.stringify(value);
const hash = value => crypto.createHash('sha256').update(canonical(value)).digest('hex');
const clone = value => JSON.parse(JSON.stringify(value));
const BANK_HASH_CONTRACT = 'vectorwarp-detection-bank-ieee754-v1';

// Hash the semantic bank, not a language-specific JSON rendering. Node,
// Python and C++ can all reproduce this exact little-endian binary contract.
function detectionBankHash(responses, directResponse) {
  const beams = responses?.length, channels = directResponse?.length;
  if (!Number.isInteger(beams) || beams < 1 || !Number.isInteger(channels) || channels < 1 ||
      responses.some(row => !Array.isArray(row) || row.length !== channels) ||
      [directResponse, ...responses].some(row => row.some(value =>
        !Array.isArray(value) || value.length !== 2 || !value.every(finite))))
    throw new Error('Invalid detection bank hash input');
  const digest = crypto.createHash('sha256');
  digest.update(Buffer.from(BANK_HASH_CONTRACT, 'ascii'));
  const u32 = value => { const bytes = Buffer.allocUnsafe(4); bytes.writeUInt32LE(value); digest.update(bytes); };
  // JSON does not preserve the sign bit of zero; make that equivalence explicit
  // so a written and reloaded bank retains the same semantic identity.
  const f64 = value => { const bytes = Buffer.allocUnsafe(8); bytes.writeDoubleLE(value === 0 ? 0 : value); digest.update(bytes); };
  u32(beams); u32(channels);
  for (const row of [directResponse, ...responses]) for (const value of row) {
    f64(value[0]); f64(value[1]);
  }
  return digest.digest('hex');
}

function aligned(vector, reference) {
  const phase = inner(reference, vector), size = Math.sqrt(power(phase));
  return size > 0 ? vector.map(value => multiply(value, [phase[0] / size, -phase[1] / size])) : vector;
}

function passBalancedResponse(rows, field) {
  const reference = normalize(rows[0][field]);
  if (!reference) throw new Error('Detection manifold response has zero or nonfinite energy');
  const passes = new Map();
  for (const row of rows) {
    if (!passes.has(row._pass_id)) passes.set(row._pass_id, []);
    passes.get(row._pass_id).push(row);
  }
  let sum = reference.map(() => [0, 0]);
  for (const passRows of passes.values()) {
    let pass = reference.map(() => [0, 0]);
    for (const row of passRows) {
      const normalized = normalize(row[field]);
      if (!normalized) throw new Error('Detection manifold response has zero or nonfinite energy');
      const vector = aligned(normalized, reference);
      pass = pass.map((value, index) => add(value, vector[index]));
    }
    pass = normalize(pass);
    if (!pass) throw new Error('Detection manifold pass has zero coherent energy');
    sum = sum.map((value, index) => add(value, pass[index]));
  }
  const response = normalize(sum);
  if (!response) throw new Error('Detection manifold fit has zero coherent energy');
  return {response, training_passes: passes.size, training_samples: rows.length};
}

function fitDetectionManifold(rawSamples, {azimuthBins = 8, elevationBins = 1,
  minTrainingPassesPerBin = 2, corpusSha256 = null} = {}) {
  if (!Number.isInteger(azimuthBins) || azimuthBins < 4 || azimuthBins > 360 ||
      !Number.isInteger(elevationBins) || elevationBins < 1 ||
      !Number.isInteger(minTrainingPassesPerBin) || minTrainingPassesPerBin < 1)
    throw new Error('Invalid detection manifold fit settings');
  const corpus = buildDetectionCorpus(rawSamples, {corpusSha256});
  if (!corpus.training.length) throw new Error('No pass-disjoint detection training evidence');
  const first = corpus.training[0], identity = clone(first.manifold_identity);
  const directAzimuth = angle(first.direct_path_local_azimuth_deg);
  if (corpus.training.some(row => canonical(row.manifold_identity) !== canonical(identity) ||
      Math.abs(angle(row.direct_path_local_azimuth_deg) - directAzimuth) > 1e-6))
    throw new Error('Detection training requires one exact installed identity and fixed-transmitter anchor');
  // Fit support only from the pass-disjoint training partition. Using held-out
  // or replay rows here leaks evaluation geometry into the runtime candidate.
  const elevations = corpus.training.map(row => row.truth.elevation_deg);
  const elevationRange = [Math.min(...elevations), Math.max(...elevations)];
  const elevationWidth = Math.max(1e-9, elevationRange[1] - elevationRange[0]);
  const bins = new Map();
  for (const row of corpus.training) {
    const azimuth = azimuthBin(row.truth.local_azimuth_deg, azimuthBins);
    const elevation = Math.min(elevationBins - 1,
      Math.floor((row.truth.elevation_deg - elevationRange[0]) / elevationWidth * elevationBins));
    const key = `${azimuth}:${elevation}`;
    if (!bins.has(key)) bins.set(key, []);
    bins.get(key).push(row);
  }
  const responses = [];
  for (const [key, rows] of bins) {
    if (new Set(rows.map(row => row._pass_id)).size < minTrainingPassesPerBin) continue;
    const supportElevations = rows.map(row => row.truth.elevation_deg);
    responses.push({key, azimuth_bin: Number(key.split(':')[0]),
      elevation_bin: Number(key.split(':')[1]),
      elevation_range_deg: [Math.min(...supportElevations), Math.max(...supportElevations)],
      ...passBalancedResponse(rows, 'response')});
  }
  if (!responses.length) throw new Error('No detection manifold bin has sufficient independent training passes');
  // CoherentBeamBank divides each target response by this rounded beam before
  // multiplying by the current reference. It must contain the measured fixed
  // illuminator response, not an aircraft response near the transmitter angle.
  const directAnchor = {local_azimuth_deg: directAzimuth,
    beam: azimuthBin(directAzimuth, azimuthBins),
    ...passBalancedResponse(corpus.training, 'reference_weights')};
  return {schema: 'vectorwarp-detection-manifold-candidate-v1', runtime_compatible: false,
    intended_use: 'offline-shadow-validation-only',
    frequency_hz: first.frequency_hz, sample_rate_hz: first.sample_rate_hz,
    channels: first.channels, channel_mapping: [...identity.channel_mapping],
    config_fingerprint: corpus.fingerprint, manifold_identity: identity,
    manifold_identity_sha256: hash(identity),
    direct_path_local_azimuth_deg: directAzimuth, direct_path_beam: directAnchor.beam,
    direct_anchor: directAnchor, azimuth_binning: 'nearest-rounded-runtime-beam',
    azimuth_bins: azimuthBins, elevation_bins: elevationBins, elevation_range_deg: elevationRange,
    responses, calibration: {method: 'pass-balanced-same-iq-complex-mean-v1',
      positive_corpus_sha256: corpusSha256,
      training_samples: corpus.training.length,
      training_passes: new Set(corpus.training.map(row => row._pass_id)).size,
      independent_validation_samples: corpus.validation.length,
      independent_validation_passes: new Set(corpus.validation.map(row => row._pass_id)).size,
      independent_miss_passes: corpus.miss_passes,
      adsb_role: corpus.labels}};
}

function responseFor(model, sample) {
  const azimuth = azimuthBin(sample.truth.local_azimuth_deg, model.azimuth_bins);
  const width = Math.max(1e-9, model.elevation_range_deg[1] - model.elevation_range_deg[0]);
  const elevation = Math.min(model.elevation_bins - 1, Math.max(0,
    Math.floor((sample.truth.elevation_deg - model.elevation_range_deg[0]) / width * model.elevation_bins)));
  const row = model.responses.find(row => row.azimuth_bin === azimuth && row.elevation_bin === elevation);
  return row && sample.truth.elevation_deg >= row.elevation_range_deg[0] &&
    sample.truth.elevation_deg <= row.elevation_range_deg[1] ? row.response : null;
}

function statistic(steering, vector, covariance) {
  const numerator = power(inner(steering, vector));
  const applied = covariance.map(row => row.reduce((sum, value, index) =>
    add(sum, multiply(value, steering[index])), [0, 0]));
  const denominator = inner(steering, applied)[0];
  return denominator > 0 && finite(denominator) ? numerator / denominator : null;
}

function cfarThreshold(cfar) {
  const perTrialPfa = 1 - Math.pow(1 - cfar.pfa, 1 / cfar.effective_trials);
  return cfar.training_cells * (Math.pow(perTrialPfa, -1 / cfar.training_cells) - 1);
}

function denseRuntimeCandidate(model) {
  if (!model || model.schema !== 'vectorwarp-detection-manifold-candidate-v1' ||
      model.runtime_compatible !== false || model.elevation_bins !== 1 ||
      !Number.isInteger(model.azimuth_bins) || model.azimuth_bins < 4 ||
      !Number.isInteger(model.channels) || model.channels < 2 ||
      !validIdentity(model.manifold_identity, model.frequency_hz, model.sample_rate_hz, model.channels) ||
      model.manifold_identity_sha256 !== hash(model.manifold_identity) ||
      canonical(model.channel_mapping) !== canonical(model.manifold_identity.channel_mapping) ||
      !Array.isArray(model.elevation_range_deg) || model.elevation_range_deg.length !== 2 ||
      !model.elevation_range_deg.every(finite))
    throw new Error('Sparse detection candidate is not exactly convertible to the 2-D runtime bank');
  const byAzimuth = new Map();
  for (const row of model.responses || []) {
    if (!Number.isInteger(row?.azimuth_bin) || row.azimuth_bin < 0 ||
        row.azimuth_bin >= model.azimuth_bins || row.elevation_bin !== 0 ||
        byAzimuth.has(row.azimuth_bin) || !Array.isArray(row.response) ||
        row.response.length !== model.channels || row.response.some(value =>
          !Array.isArray(value) || value.length !== 2 || !value.every(finite) || power(value) <= 1e-12) ||
        !Array.isArray(row.elevation_range_deg) || row.elevation_range_deg.length !== 2 ||
        !row.elevation_range_deg.every(finite) || row.elevation_range_deg[0] > row.elevation_range_deg[1])
      throw new Error('Sparse detection candidate has duplicate, invalid, or unsupported bins');
    byAzimuth.set(row.azimuth_bin, row);
  }
  if (byAzimuth.size !== model.azimuth_bins)
    throw new Error('Dense runtime conversion requires every azimuth bin without interpolation');
  const anchor = model.direct_anchor;
  if (!anchor || !finite(anchor.local_azimuth_deg) || anchor.local_azimuth_deg < 0 ||
      anchor.local_azimuth_deg >= 360 || anchor.local_azimuth_deg !== model.direct_path_local_azimuth_deg ||
      anchor.beam !== azimuthBin(anchor.local_azimuth_deg, model.azimuth_bins) ||
      anchor.beam !== model.direct_path_beam || !Array.isArray(anchor.response) ||
      anchor.response.length !== model.channels || anchor.response.some(value =>
        !Array.isArray(value) || value.length !== 2 || !value.every(finite) || power(value) <= 1e-12))
    throw new Error('Dense runtime conversion requires the measured fixed-transmitter direct anchor');
  const aircraftSupport = [...byAzimuth.values()];
  const elevationRange = [Math.max(...aircraftSupport.map(row => row.elevation_range_deg[0])),
    Math.min(...aircraftSupport.map(row => row.elevation_range_deg[1]))];
  if (elevationRange[0] > elevationRange[1])
    throw new Error('Dense runtime conversion requires common observed elevation support across aircraft beams');
  // Keep the fixed-transmitter anchor out-of-band. Replacing the aircraft row
  // at the rounded transmitter beam silently blinds that whole azimuth sector.
  const responses = Array.from({length: model.azimuth_bins}, (_, index) =>
    clone(byAzimuth.get(index).response));
  return {schema: 'vectorwarp-detection-manifold-runtime-v1', runtime_compatible: true,
    intended_use: 'offline-production-equivalent-replay-only', frequency_hz: model.frequency_hz,
    sample_rate_hz: model.sample_rate_hz, channels: model.channels,
    channel_mapping: [...model.channel_mapping], config_fingerprint: model.config_fingerprint,
    manifold_identity: clone(model.manifold_identity), manifold_identity_sha256: model.manifold_identity_sha256,
    direct_path_local_azimuth_deg: anchor.local_azimuth_deg, direct_path_beam: anchor.beam,
    direct_anchor: clone(anchor), beams: model.azimuth_bins,
    elevation_range_deg: elevationRange,
    aircraft_support: aircraftSupport.map(row => ({beam: row.azimuth_bin,
      elevation_range_deg: [...row.elevation_range_deg]})), responses,
    bank_hash_contract: BANK_HASH_CONTRACT,
    bank_sha256: detectionBankHash(responses, anchor.response),
    source_candidate_sha256: crypto.createHash('sha256').update(JSON.stringify(model)).digest('hex'),
    positive_corpus_sha256: model.calibration?.positive_corpus_sha256 || null};
}

module.exports = {BANK_HASH_CONTRACT, detectionBankHash, fitDetectionManifold,
  denseRuntimeCandidate, responseFor, statistic, cfarThreshold};
