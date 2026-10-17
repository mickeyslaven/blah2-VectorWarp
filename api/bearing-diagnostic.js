'use strict';

const fs = require('fs');

const finite = value => typeof value === 'number' && Number.isFinite(value);
const add = (a, b) => [a[0] + b[0], a[1] + b[1]];
const subtract = (a, b) => [a[0] - b[0], a[1] - b[1]];
const multiply = (a, b) => [a[0] * b[0] - a[1] * b[1], a[0] * b[1] + a[1] * b[0]];
const conjugate = a => [a[0], -a[1]];
const scale = (a, value) => [a[0] * value, a[1] * value];
const magnitude2 = a => a[0] * a[0] + a[1] * a[1];
const divide = (a, b) => scale(multiply(a, conjugate(b)), 1 / magnitude2(b));
const polar = angle => [Math.cos(angle), Math.sin(angle)];
const normalize = value => ((value % 360) + 360) % 360;
const circularError = (left, right) => Math.abs(((left - right + 540) % 360) - 180);
const median = values => {
  const sorted = [...values].sort((a, b) => a - b), middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
};

function inverse(matrix) {
  const count = matrix.length;
  const left = matrix.map(row => row.map(value => [...value]));
  const right = Array.from({length: count}, (_, row) =>
    Array.from({length: count}, (_, column) => [row === column ? 1 : 0, 0]));
  for (let pivot = 0; pivot < count; ++pivot) {
    let choice = pivot;
    for (let row = pivot + 1; row < count; ++row)
      if (magnitude2(left[row][pivot]) > magnitude2(left[choice][pivot])) choice = row;
    if (!(magnitude2(left[choice][pivot]) > 1e-20)) return null;
    [left[pivot], left[choice]] = [left[choice], left[pivot]];
    [right[pivot], right[choice]] = [right[choice], right[pivot]];
    const divisor = left[pivot][pivot];
    for (let column = 0; column < count; ++column) {
      left[pivot][column] = divide(left[pivot][column], divisor);
      right[pivot][column] = divide(right[pivot][column], divisor);
    }
    for (let row = 0; row < count; ++row) if (row !== pivot) {
      const factor = left[row][pivot];
      for (let column = 0; column < count; ++column) {
        left[row][column] = subtract(left[row][column], multiply(factor, left[pivot][column]));
        right[row][column] = subtract(right[row][column], multiply(factor, right[pivot][column]));
      }
    }
  }
  return right;
}

function matrixVector(matrix, vector) {
  return matrix.map(row => row.reduce((sum, value, index) =>
    add(sum, multiply(value, vector[index])), [0, 0]));
}

function mappings(count) {
  const result = [];
  for (const direction of ['forward', 'reverse']) for (let rotation = 0; rotation < count; ++rotation) {
    const mapping = Array.from({length: count}, (_, channel) => direction === 'forward' ?
      (channel + rotation) % count : (rotation - channel + count * 2) % count);
    result.push({id: `${direction}-${rotation}`, direction, rotation, mapping});
  }
  return result;
}

function geometry(config) {
  const device = config.capture?.device || {};
  const selected = device.surveillance_channels || [];
  const record = device.array_geometry || {};
  const byChannel = new Map((record.elements || []).map(item => [item.daq_channel, item.position]));
  const positions = selected.map(channel => byChannel.get(channel));
  if (record.shape !== 'regular_polygon' || selected.length < 3 ||
      positions.some(position => !Array.isArray(position) || position.length !== 3 || !position.every(finite)) ||
      !finite(record.x_axis_bearing_deg_true)) throw new Error('A confirmed regular-polygon geometry is required');
  const centre = [0, 1].map(axis => positions.reduce((sum, value) => sum + value[axis], 0) / positions.length);
  return {positions: positions.map(value => [value[0] - centre[0], value[1] - centre[1]]),
    xAxis: record.x_axis_bearing_deg_true};
}

function weights(sample, config, mapping, beams) {
  const {positions} = geometry(config);
  const channels = positions.length;
  const frequency = sample.frequency_hz;
  const direct = sample.direct_path_local_azimuth_deg * Math.PI / 180;
  const wave = 2 * Math.PI * frequency / 299792458;
  const reference = sample.reference_weights;
  const norm = reference.reduce((sum, value) => sum + magnitude2(value), 0);
  if (!(norm > 0)) throw new Error('Reference weights have zero norm');
  const output = [];
  const nullFraction = 1 - 10 ** (-(config.process?.surveillance_fusion?.direct_path_null_db || 0) / 20);
  for (let beam = 0; beam < beams; ++beam) {
    const angle = 2 * Math.PI * beam / beams;
    const steering = [];
    for (let channel = 0; channel < channels; ++channel) {
      const position = positions[mapping[channel]];
      const relative = wave * (position[0] * (Math.cos(angle) - Math.cos(direct)) +
        position[1] * (Math.sin(angle) - Math.sin(direct)));
      steering.push(multiply(reference[channel], polar(relative)));
    }
    if (nullFraction > 0) {
      const projection = scale(steering.reduce((sum, value, channel) =>
        add(sum, multiply(conjugate(reference[channel]), value)), [0, 0]), nullFraction / norm);
      for (let channel = 0; channel < channels; ++channel)
        steering[channel] = subtract(steering[channel], multiply(projection, reference[channel]));
    }
    output.push(steering.map(value => scale(conjugate(value), 1 / Math.sqrt(norm))));
  }
  return output;
}

function score(response, steering, covarianceInverse) {
  const filtered = covarianceInverse ? matrixVector(covarianceInverse, response) : response;
  const numerator = steering.reduce((sum, value, channel) =>
    add(sum, multiply(value, filtered[channel])), [0, 0]);
  if (!covarianceInverse) return magnitude2(numerator);
  const denominatorVector = matrixVector(covarianceInverse, steering.map(conjugate));
  const denominator = steering.reduce((sum, value, channel) =>
    add(sum, multiply(value, denominatorVector[channel])), [0, 0]);
  return magnitude2(numerator) / Math.max(Math.abs(denominator[0]), 1e-12);
}

function bestBearing(sample, config, candidate) {
  const beams = sample.bearing_replay?.scan_beams || config.process?.bearing?.scan_beams;
  if (!Number.isInteger(beams) || beams < 4 || beams > 127) throw new Error('Invalid bearing scan beam count');
  const bank = weights(sample, config, candidate.mapping, beams);
  const covarianceInverse = sample.bearing_replay?.method === 'covariance_mvdr' ? inverse(sample.covariance) : null;
  if (sample.bearing_replay?.method === 'covariance_mvdr' && !covarianceInverse)
    throw new Error('Recorded covariance is singular');
  let best = 0, bestScore = -1;
  for (let beam = 0; beam < beams; ++beam) {
    const value = score(sample.response, bank[beam], covarianceInverse);
    if (value > bestScore) { bestScore = value; best = beam; }
  }
  return normalize(geometry(config).xAxis - best * 360 / beams);
}

function balanced(errors) {
  const byAircraft = new Map();
  for (const row of errors) {
    const key = row.icao || row.flight || 'unknown';
    if (!byAircraft.has(key)) byAircraft.set(key, []);
    byAircraft.get(key).push(row.error);
  }
  const perAircraft = [...byAircraft.values()].map(median);
  return {aircraft: byAircraft.size, samples: errors.length,
    median_error_deg: perAircraft.length ? median(perAircraft) : null,
    within_45_fraction: perAircraft.length ? perAircraft.filter(value => value <= 45).length / perAircraft.length : null};
}

function evaluateBearing(samples, config) {
  const fusion = config.process?.surveillance_fusion?.mode;
  if (config.process?.bearing?.manifold_file) return {available: false,
    reason: 'Channel-order replay is disabled while a measured manifold is active; its channel identities are already site-specific.'};
  if (!['coherent_beam_bank'].includes(fusion)) return {available: false,
    reason: fusion === 'noise_whitened_beam_bank' ?
      'Exact mapping replay needs the reference whitening covariance.' :
      'Mapping replay requires coherent regular-polygon fusion.'};
  const valid = samples.filter(sample => sample?.schema === 'vectorwarp-manifold-sample-v1' &&
    Array.isArray(sample.response) && Array.isArray(sample.reference_weights) &&
    sample.bearing_replay && (sample.bearing_replay.method !== 'covariance_mvdr' || Array.isArray(sample.covariance)) &&
    finite(sample.truth?.bearing_deg_true));
  if (!valid.length) return {available: false, reason: 'No replay-capable bearing samples have been recorded yet.'};
  const identities = new Set(valid.map(sample => sample.icao || sample.flight).filter(Boolean));
  const candidates = mappings(valid[0].channels);
  const results = candidates.map(candidate => {
    const errors = [];
    for (const sample of valid) try {
      const bearing = bestBearing(sample, config, candidate);
      errors.push({icao: sample.icao, flight: sample.flight,
        truth: sample.truth.bearing_deg_true,
        error: circularError(bearing, sample.truth.bearing_deg_true), bearing});
    } catch (_) { /* A malformed row cannot decide a mapping. */ }
    return {...candidate, ...balanced(errors), errors};
  }).filter(result => result.samples);
  results.sort((left, right) => left.median_error_deg - right.median_error_deg ||
    right.within_45_fraction - left.within_45_fraction);
  const activeMapping = valid[valid.length - 1].bearing_replay.channel_mapping;
  const active = results.find(item => item.mapping.every((value, index) => value === activeMapping[index])) || null;
  let bias = null;
  if (active) {
    const trials = Array.from({length: 360}, (_, correction) => {
      const errors = active.errors.map(row => ({...row,
        error: circularError(row.bearing + correction, row.truth)}));
      return {correction_deg: correction, ...balanced(errors)};
    }).sort((left, right) => left.median_error_deg - right.median_error_deg);
    bias = {...trials[0], signed_correction_deg: trials[0].correction_deg > 180 ?
      trials[0].correction_deg - 360 : trials[0].correction_deg};
  }
  const sectors = new Set(valid.map(sample => Math.floor(normalize(sample.truth.bearing_deg_true) / 45))).size;
  const best = results[0] || null, runnerUp = results[1] || null;
  const evidenceReady = valid.length >= 100 && identities.size >= 5 && sectors >= 4;
  return {available: true, shadow_only: true, auto_apply: false,
    method: valid[valid.length - 1].bearing_replay.method,
    samples: valid.length, aircraft: identities.size, sectors,
    evidence_ready: evidenceReady,
    best: best && {id: best.id, mapping: best.mapping, median_error_deg: best.median_error_deg,
      within_45_fraction: best.within_45_fraction},
    active: active && {id: active.id, mapping: active.mapping, median_error_deg: active.median_error_deg,
      within_45_fraction: active.within_45_fraction},
    runner_up: runnerUp && {id: runnerUp.id, mapping: runnerUp.mapping,
      median_error_deg: runnerUp.median_error_deg},
    global_heading_bias: bias,
    identifiability: 'For a symmetric regular polygon, cyclic channel rotations and equal heading offsets are mathematically degenerate. Use a known-angle source or physical channel verification to distinguish them.',
    interpretation: 'Actual complex channels and the recorded covariance are replayed. No result changes live geometry.'};
}

function readSamples(filename, {maximumBytes = 64 * 1024 * 1024, maximumRows = 20000} = {}) {
  const stat = fs.statSync(filename);
  if (stat.size > maximumBytes) throw new Error('Bearing sample file exceeds the diagnostic read limit');
  return fs.readFileSync(filename, 'utf8').split(/\r?\n/).filter(Boolean).slice(-maximumRows).map((line, index) => {
    try { return JSON.parse(line); }
    catch (error) { throw new Error(`Invalid bearing sample line ${index + 1}: ${error.message}`); }
  });
}

function createBearingDiagnostic(config, {fileSystem = fs} = {}) {
  const filename = config.process?.bearing?.manifold_recorder?.path;
  let cache = null;
  return {status() {
    if (!filename) return {available: false, reason: 'No manifold recorder path is configured.'};
    try {
      const stat = fileSystem.statSync(filename);
      if (!cache || cache.size !== stat.size || cache.mtimeMs !== stat.mtimeMs)
        cache = {size: stat.size, mtimeMs: stat.mtimeMs,
          value: evaluateBearing(readSamples(filename), config)};
      return cache.value;
    } catch (error) {
      return {available: false, reason: error.code === 'ENOENT' ?
        'No bearing samples have been recorded yet.' : error.message};
    }
  }};
}

module.exports = {evaluateBearing, createBearingDiagnostic, readSamples, mappings, inverse,
  steeringWeights: weights};
