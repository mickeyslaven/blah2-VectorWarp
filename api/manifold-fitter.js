'use strict';

const finite = value => typeof value === 'number' && Number.isFinite(value);
const add = (a, b) => [a[0] + b[0], a[1] + b[1]];
const scale = (a, value) => [a[0] * value, a[1] * value];
const multiply = (a, b) => [a[0] * b[0] - a[1] * b[1], a[0] * b[1] + a[1] * b[0]];
const conjugate = a => [a[0], -a[1]];
const magnitude = a => Math.hypot(a[0], a[1]);
const inner = (a, b) => a.reduce((sum, value, index) => add(sum,
  multiply(conjugate(value), b[index])), [0, 0]);
const normalizeVector = vector => {
  const norm = Math.sqrt(vector.reduce((sum, value) => sum + magnitude(value) ** 2, 0));
  return norm > 0 && finite(norm) ? vector.map(value => scale(value, 1 / norm)) : null;
};
const median = values => {
  const sorted = [...values].sort((a, b) => a - b), middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
};
const normalizeAngle = value => ((value % 360) + 360) % 360;

function stableBucket(value, buckets) {
  let hash = 2166136261;
  for (const character of value) {
    hash ^= character.charCodeAt(0);
    hash = Math.imul(hash, 16777619);
  }
  return (hash >>> 0) % buckets;
}
const passIsHoldout = passId => stableBucket(passId, 5) === 0;

// A flight pass, rather than a processor frame, is the independent unit of
// calibration evidence. This prevents one slow or repeatedly detected target
// from dominating the installed-site response.
function labelPasses(samples, gapMs = 15 * 60 * 1000) {
  const state = new Map();
  return samples.map((sample, index) => {
    const icao = typeof sample.icao === 'string' && sample.icao.trim() ?
      sample.icao.trim().toLowerCase() : `unknown-${index}`;
    const timestamp = finite(sample.radar_timestamp_ms) ? sample.radar_timestamp_ms : index;
    const prior = state.get(icao);
    const sequence = !prior || timestamp < prior.timestamp || timestamp - prior.timestamp > gapMs ?
      (prior?.sequence || 0) + 1 : prior.sequence;
    state.set(icao, {timestamp, sequence});
    return {...sample, _pass_id: `${icao}:${sequence}`};
  });
}

function summarizeCoverage(samples, sectors = 8) {
  const labelled = labelPasses(samples);
  const rows = Array.from({length: sectors}, (_, index) => ({
    from_deg: index * 360 / sectors, to_deg: (index + 1) * 360 / sectors,
    samples: 0, passes: new Set(), aircraft: new Set()
  }));
  for (const sample of labelled) {
    const sector = Math.floor(normalizeAngle(sample.truth.local_azimuth_deg) / 360 * sectors) % sectors;
    const row = rows[sector];
    ++row.samples;
    row.passes.add(sample._pass_id);
    if (typeof sample.icao === 'string' && sample.icao.trim())
      row.aircraft.add(sample.icao.trim().toLowerCase());
  }
  return rows.map(row => ({...row, passes: row.passes.size, aircraft: row.aircraft.size}));
}

function validateSample(sample) {
  if (!sample || sample.schema !== 'vectorwarp-manifold-sample-v1' ||
      !finite(sample.frequency_hz) || !Number.isInteger(sample.channels) ||
      !finite(sample.truth?.local_azimuth_deg) || !finite(sample.truth?.elevation_deg) ||
      !finite(sample.direct_path_local_azimuth_deg) ||
      !finite(sample.detection?.snr_db) || !Array.isArray(sample.response) ||
      sample.response.length !== sample.channels || sample.response.some(value =>
        !Array.isArray(value) || value.length !== 2 || !value.every(finite)) ||
      !Array.isArray(sample.reference_weights) || sample.reference_weights.length !== sample.channels ||
      sample.reference_weights.some(value => !Array.isArray(value) || value.length !== 2 ||
        !value.every(finite)))
    throw new Error('Invalid VectorWarp manifold sample');
  return sample;
}

function align(vector, template) {
  const phase = inner(template, vector);
  const length = magnitude(phase);
  return length > 0 ? vector.map(value => multiply(value, [phase[0] / length, -phase[1] / length])) : vector;
}

function template(samples) {
  let current = samples[0].vector;
  for (let iteration = 0; iteration < 5; ++iteration) {
    let sum = current.map(() => [0, 0]);
    for (const sample of samples) {
      const aligned = align(sample.vector, current);
      sum = sum.map((value, channel) => add(value, scale(aligned[channel], sample.weight)));
    }
    current = normalizeVector(sum);
    if (!current) throw new Error('A manifold bin has zero coherent energy');
  }
  const correlations = samples.map(sample => magnitude(inner(current, sample.vector)));
  const centre = median(correlations), mad = median(correlations.map(value => Math.abs(value - centre)));
  const threshold = Math.max(.5, centre - 3 * Math.max(mad, .01));
  const retained = samples.filter((sample, index) => correlations[index] >= threshold);
  if (retained.length !== samples.length && retained.length >= 2) return template(retained);
  return {response: current, retained: retained.length, rejected: samples.length - retained.length,
    medianCorrelation: median(retained.map(sample => magnitude(inner(current, sample.vector))))};
}

function interpolate(left, right, fraction) {
  const aligned = align(right, left);
  return normalizeVector(left.map((value, channel) => add(scale(value, 1 - fraction),
    scale(aligned[channel], fraction))));
}

function fitManifold(rawSamples, {beams = 60, minSamples = 20, minPasses = 1,
  minCoverage = .5, maxGapDeg = 90, maxElevationDeg = 15,
  maxDelayErrorKm = .25, maxDopplerErrorHz = 5, maxTruthAgeS = 3,
  holdoutPredicate = passIsHoldout} = {}) {
  if (!Number.isInteger(beams) || beams < 4 || beams > 127 ||
      !Number.isInteger(minSamples) || minSamples < 2 ||
      !Number.isInteger(minPasses) || minPasses < 1)
    throw new Error('Invalid manifold fit settings');
  const validated = rawSamples.map(validateSample);
  const rejected = {elevation: 0, delay: 0, doppler: 0, stale_truth: 0};
  const samples = validated.filter(sample => {
    if (sample.truth.elevation_deg > maxElevationDeg) { ++rejected.elevation; return false; }
    if (finite(sample.match?.delay_error_km) &&
        Math.abs(sample.match.delay_error_km) > maxDelayErrorKm) { ++rejected.delay; return false; }
    if (finite(sample.match?.doppler_error_hz) &&
        Math.abs(sample.match.doppler_error_hz) > maxDopplerErrorHz) { ++rejected.doppler; return false; }
    if (finite(sample.radar_timestamp_ms) && finite(sample.truth.position_timestamp_s) &&
        Math.abs(sample.radar_timestamp_ms / 1000 - sample.truth.position_timestamp_s) > maxTruthAgeS) {
      ++rejected.stale_truth; return false;
    }
    return true;
  });
  if (!samples.length) throw new Error('No manifold samples were supplied');
  const frequency = samples[0].frequency_hz, channels = samples[0].channels;
  const fingerprint = samples[0].config_fingerprint;
  if (samples.some(sample => Math.abs(sample.frequency_hz - frequency) > Math.max(1, frequency * 1e-6) ||
      sample.channels !== channels || sample.config_fingerprint !== fingerprint ||
      Math.abs(sample.direct_path_local_azimuth_deg - samples[0].direct_path_local_azimuth_deg) > 1e-6))
    throw new Error('Samples from different frequencies, channel layouts, or site configurations cannot be mixed');
  const labelled = labelPasses(samples);
  const bins = Array.from({length: beams}, () => []), step = 360 / beams;
  labelled.forEach(sample => {
    const vector = normalizeVector(sample.response);
    if (!vector) return;
    const bin = Math.round(normalizeAngle(sample.truth.local_azimuth_deg) / step) % beams;
    const snr = Math.max(1, Math.min(20, 10 ** (sample.detection.snr_db / 20)));
    bins[bin].push({vector, snr, passId: sample._pass_id,
      holdout: holdoutPredicate(sample._pass_id)});
  });
  // Equal total weight per independent pass within each azimuth bin. SNR still
  // ranks frames inside a pass, but cannot turn one aircraft into many votes.
  for (const bin of bins) {
    const totals = new Map();
    for (const sample of bin) totals.set(sample.passId,
      (totals.get(sample.passId) || 0) + sample.snr);
    for (const sample of bin) sample.weight = sample.snr / totals.get(sample.passId);
  }
  const responses = new Array(beams), statistics = new Array(beams);
  for (let bin = 0; bin < beams; ++bin) {
    const training = bins[bin].filter(sample => !sample.holdout);
    const trainingPasses = new Set(training.map(sample => sample.passId)).size;
    if (training.length < minSamples || trainingPasses < minPasses) continue;
    const result = template(training);
    responses[bin] = result.response;
    const holdout = bins[bin].filter(sample => sample.holdout)
      .map(sample => magnitude(inner(result.response, sample.vector)));
    statistics[bin] = {samples: bins[bin].length,
      passes: new Set(bins[bin].map(sample => sample.passId)).size,
      training: training.length, training_passes: trainingPasses,
      retained: result.retained, rejected: result.rejected,
      training_median_correlation: result.medianCorrelation,
      holdout_samples: holdout.length,
      holdout_median_correlation: holdout.length ? median(holdout) : null};
  }
  const directBin = Math.round(normalizeAngle(samples[0].direct_path_local_azimuth_deg) / step) % beams;
  const directSamples = labelled.filter(sample => !holdoutPredicate(sample._pass_id))
    .map(sample => ({vector: normalizeVector(sample.reference_weights), passId: sample._pass_id}))
    .filter(sample => sample.vector);
  if (!directSamples.length) throw new Error('No training passes remain after holdout selection');
  const directPassCounts = new Map();
  for (const sample of directSamples)
    directPassCounts.set(sample.passId, (directPassCounts.get(sample.passId) || 0) + 1);
  for (const sample of directSamples) sample.weight = 1 / directPassCounts.get(sample.passId);
  const directResult = template(directSamples);
  const aircraftAtDirect = responses[directBin];
  const directAgreement = aircraftAtDirect ? magnitude(inner(aircraftAtDirect, directResult.response)) : null;
  responses[directBin] = directResult.response;
  statistics[directBin] = {...(statistics[directBin] || {}), direct_path: true,
    direct_reference_samples: directSamples.length,
    direct_reference_median_correlation: directResult.medianCorrelation,
    aircraft_to_direct_reference_correlation: directAgreement};
  const observed = responses.map((value, index) => value ? index : null).filter(value => value !== null);
  if (observed.length / beams < minCoverage)
    throw new Error(`Only ${observed.length}/${beams} azimuth bins meet the sample requirement`);
  let largestGap = 0;
  for (let i = 0; i < observed.length; ++i) {
    const gap = (observed[(i + 1) % observed.length] - observed[i] + beams) % beams;
    largestGap = Math.max(largestGap, gap * step);
  }
  if (largestGap > maxGapDeg)
    throw new Error(`Largest unsupported azimuth gap is ${largestGap.toFixed(1)} degrees`);
  for (let bin = 0; bin < beams; ++bin) if (!responses[bin]) {
    let distanceLeft = 1, distanceRight = 1;
    while (!responses[(bin - distanceLeft + beams) % beams]) ++distanceLeft;
    while (!responses[(bin + distanceRight) % beams]) ++distanceRight;
    const left = (bin - distanceLeft + beams) % beams, right = (bin + distanceRight) % beams;
    responses[bin] = interpolate(responses[left], responses[right],
      distanceLeft / (distanceLeft + distanceRight));
    statistics[bin] = {samples: bins[bin].length, interpolated: true,
      between_bins: [left, right]};
  }
  if (responses.some(row => row.some(value => magnitude(value) <= 1e-6)))
    throw new Error('Fitted manifold contains a near-zero channel response');
  const elevations = samples.map(sample => sample.truth.elevation_deg);
  const holdout = statistics.filter(Boolean).map(item => item.holdout_median_correlation)
    .filter(finite);
  return {schema: 'vectorwarp-bearing-manifold-v1', frequency_hz: frequency,
    responses, calibration: {method: 'adsb_supervised_complex_projective_mean-v1',
      config_fingerprint: fingerprint, beams, channels,
      input_samples: samples.length, supplied_samples: validated.length,
      rejected_samples: rejected,
      direct_path_local_azimuth_deg: samples[0].direct_path_local_azimuth_deg,
      direct_path_bin: directBin,
      required_samples_per_observed_bin: minSamples,
      required_passes_per_observed_bin: minPasses,
      independent_passes: new Set(labelled.map(sample => sample._pass_id)).size,
      unique_aircraft: new Set(samples.map(sample => sample.icao).filter(Boolean)).size,
      sector_coverage: summarizeCoverage(samples), observed_bins: observed.length,
      interpolated_bins: beams - observed.length, largest_observed_gap_deg: largestGap,
      elevation_range_deg: [Math.min(...elevations), Math.max(...elevations)],
      holdout_bins: holdout.length,
      holdout_median_correlation: holdout.length ? median(holdout) : null,
      bins: statistics}};
}

module.exports = {validateSample, labelPasses, summarizeCoverage, passIsHoldout, fitManifold};
