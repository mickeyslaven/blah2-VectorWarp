'use strict';

const crypto = require('crypto');
const {buildDetectionCorpus} = require('./detection-manifold-corpus');

const finite = value => typeof value === 'number' && Number.isFinite(value);
const add = (a, b) => [a[0] + b[0], a[1] + b[1]];
const multiply = (a, b) => [a[0] * b[0] - a[1] * b[1], a[0] * b[1] + a[1] * b[0]];
const conjugate = a => [a[0], -a[1]];
const inner = (a, b) => a.reduce((sum, value, index) =>
  add(sum, multiply(conjugate(value), b[index])), [0, 0]);
const power = value => value[0] ** 2 + value[1] ** 2;
const normalize = vector => {
  const norm = Math.sqrt(vector.reduce((sum, value) => sum + power(value), 0));
  return norm > 0 ? vector.map(value => [value[0] / norm, value[1] / norm]) : null;
};
const angle = value => ((value % 360) + 360) % 360;

function aligned(vector, reference) {
  const phase = inner(reference, vector), size = Math.sqrt(power(phase));
  return size > 0 ? vector.map(value => multiply(value, [phase[0] / size, -phase[1] / size])) : vector;
}

function fitDetectionManifold(rawSamples, {azimuthBins = 8, elevationBins = 1,
  minTrainingPassesPerBin = 2, corpusSha256 = null} = {}) {
  const corpus = buildDetectionCorpus(rawSamples, {corpusSha256});
  if (!corpus.training.length) throw new Error('No pass-disjoint detection training evidence');
  const elevations = corpus.accepted.map(row => row.truth.elevation_deg);
  const elevationRange = [Math.min(...elevations), Math.max(...elevations)];
  const elevationWidth = Math.max(1e-9, elevationRange[1] - elevationRange[0]);
  const bins = new Map();
  for (const row of corpus.training) {
    const azimuth = Math.floor(angle(row.truth.local_azimuth_deg) / 360 * azimuthBins) % azimuthBins;
    const elevation = Math.min(elevationBins - 1,
      Math.floor((row.truth.elevation_deg - elevationRange[0]) / elevationWidth * elevationBins));
    const key = `${azimuth}:${elevation}`;
    if (!bins.has(key)) bins.set(key, []);
    bins.get(key).push(row);
  }
  const responses = [];
  for (const [key, rows] of bins) {
    const passes = new Map();
    for (const row of rows) {
      if (!passes.has(row._pass_id)) passes.set(row._pass_id, []);
      passes.get(row._pass_id).push(row);
    }
    if (passes.size < minTrainingPassesPerBin) continue;
    let reference = normalize(rows[0].response), sum = reference.map(() => [0, 0]);
    for (const passRows of passes.values()) {
      let pass = reference.map(() => [0, 0]);
      for (const row of passRows) {
        const vector = aligned(normalize(row.response), reference);
        pass = pass.map((value, index) => add(value, vector[index]));
      }
      pass = normalize(pass);
      sum = sum.map((value, index) => add(value, pass[index]));
    }
    responses.push({key, azimuth_bin: Number(key.split(':')[0]),
      elevation_bin: Number(key.split(':')[1]), training_passes: passes.size,
      training_samples: rows.length, response: normalize(sum)});
  }
  if (!responses.length) throw new Error('No detection manifold bin has sufficient independent training passes');
  return {schema: 'vectorwarp-detection-manifold-candidate-v1', runtime_compatible: false,
    intended_use: 'offline-shadow-validation-only',
    frequency_hz: corpus.accepted[0].frequency_hz,
    channels: corpus.accepted[0].channels, config_fingerprint: corpus.fingerprint,
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
  const azimuth = Math.floor(angle(sample.truth.local_azimuth_deg) / 360 * model.azimuth_bins) % model.azimuth_bins;
  const width = Math.max(1e-9, model.elevation_range_deg[1] - model.elevation_range_deg[0]);
  const elevation = Math.min(model.elevation_bins - 1, Math.max(0,
    Math.floor((sample.truth.elevation_deg - model.elevation_range_deg[0]) / width * model.elevation_bins)));
  return model.responses.find(row => row.azimuth_bin === azimuth && row.elevation_bin === elevation)?.response || null;
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
      !Array.isArray(model.elevation_range_deg) || model.elevation_range_deg.length !== 2 ||
      !model.elevation_range_deg.every(finite))
    throw new Error('Sparse detection candidate is not exactly convertible to the 2-D runtime bank');
  const byAzimuth = new Map();
  for (const row of model.responses || []) {
    if (!Number.isInteger(row?.azimuth_bin) || row.azimuth_bin < 0 ||
        row.azimuth_bin >= model.azimuth_bins || row.elevation_bin !== 0 ||
        byAzimuth.has(row.azimuth_bin) || !Array.isArray(row.response) ||
        row.response.length !== model.channels || row.response.some(value =>
          !Array.isArray(value) || value.length !== 2 || !value.every(finite)))
      throw new Error('Sparse detection candidate has duplicate, invalid, or unsupported bins');
    byAzimuth.set(row.azimuth_bin, row.response);
  }
  if (byAzimuth.size !== model.azimuth_bins)
    throw new Error('Dense runtime conversion requires every azimuth bin without interpolation');
  const responses = Array.from({length: model.azimuth_bins}, (_, index) => byAzimuth.get(index));
  const canonical = value => Array.isArray(value) ? `[${value.map(canonical).join(',')}]` :
    value && typeof value === 'object' ? `{${Object.keys(value).sort().map(key =>
      `${JSON.stringify(key)}:${canonical(value[key])}`).join(',')}}` : JSON.stringify(value);
  return {schema: 'vectorwarp-detection-manifold-runtime-v1', runtime_compatible: true,
    intended_use: 'offline-production-equivalent-replay-only', frequency_hz: model.frequency_hz,
    channels: model.channels, beams: model.azimuth_bins,
    elevation_range_deg: [...model.elevation_range_deg], responses,
    bank_sha256: crypto.createHash('sha256').update(canonical(responses)).digest('hex'),
    source_candidate_sha256: crypto.createHash('sha256').update(JSON.stringify(model)).digest('hex'),
    positive_corpus_sha256: model.calibration?.positive_corpus_sha256 || null};
}

module.exports = {fitDetectionManifold, denseRuntimeCandidate, responseFor, statistic, cfarThreshold};
