'use strict';

const crypto = require('crypto');

const SPEED_OF_LIGHT = 299792458;
const finite = value => typeof value === 'number' && Number.isFinite(value);
const complex = (real = 0, imaginary = 0) => [real, imaginary];
const add = (a, b) => [a[0] + b[0], a[1] + b[1]];
const multiply = (a, b) => [a[0] * b[0] - a[1] * b[1], a[0] * b[1] + a[1] * b[0]];
const conjugate = value => [value[0], -value[1]];
const scale = (value, amount) => [value[0] * amount, value[1] * amount];
const magnitude2 = value => value[0] * value[0] + value[1] * value[1];
const divide = (a, b) => scale(multiply(a, conjugate(b)), 1 / magnitude2(b));
const normalizeDegrees = value => ((value % 360) + 360) % 360;

function median(values) {
  if (!values.length) return null;
  const sorted = [...values].sort((a, b) => a - b), middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
}

function transmitterBearing(receiver, transmitter) {
  if (![receiver?.latitude, receiver?.longitude, transmitter?.latitude,
    transmitter?.longitude].every(finite)) throw new Error('Receiver/transmitter coordinates are required');
  const radians = value => value * Math.PI / 180;
  const lat1 = radians(receiver.latitude), lat2 = radians(transmitter.latitude);
  const longitude = radians(transmitter.longitude - receiver.longitude);
  return normalizeDegrees(Math.atan2(Math.sin(longitude) * Math.cos(lat2),
    Math.cos(lat1) * Math.sin(lat2) - Math.sin(lat1) * Math.cos(lat2) *
    Math.cos(longitude)) * 180 / Math.PI);
}

function analysisDescriptor(config) {
  const device = config.capture?.device || {}, geometry = device.array_geometry || {};
  const selected = config.process?.reference_synthesis?.channels ||
    device.surveillance_channels || Array.from({length: device.channel_count || 0}, (_, index) => index);
  const byChannel = new Map((geometry.elements || []).map(element =>
    [element.daq_channel, element.position]));
  const positions = selected.map(channel => byChannel.get(channel));
  const options = config.process?.bearing?.frequency_resolved_shadow || {};
  if (device.type !== 'Kraken' || !Number.isInteger(device.channel_count) ||
      device.channel_count < 2 || device.channel_count > 8 || selected.length < 2 ||
      selected.length > device.channel_count || new Set(selected).size !== selected.length ||
      selected.some(channel => !Number.isInteger(channel) || channel < 0 || channel >= device.channel_count) ||
      positions.some(position => !Array.isArray(position) || position.length !== 3 ||
        !position.every(finite)) || !finite(geometry.x_axis_bearing_deg_true) ||
      !finite(config.capture?.fc) || !finite(config.capture?.fs))
    throw new Error('Frequency-resolved shadow requires complete coherent Kraken geometry');
  const centre = [0, 1, 2].map(axis => positions.reduce((sum, position) =>
    sum + position[axis], 0) / positions.length);
  const bearing = transmitterBearing(config.location?.rx, config.location?.tx);
  const descriptor = {frequency_hz: config.capture.fc, sample_rate_hz: config.capture.fs,
    capture_channels: device.channel_count, channels: selected.length,
    channel_indices: [...selected],
    positions_m: positions.map(position => position.map((value, axis) => value - centre[axis])),
    x_axis_bearing_deg_true: geometry.x_axis_bearing_deg_true,
    transmitter_bearing_deg_true: bearing,
    direct_path_local_azimuth_deg: normalizeDegrees(geometry.x_axis_bearing_deg_true - bearing),
    fft_size: options.fft_size ?? 2048, subbands: options.subbands ?? 24,
    bandwidth_fraction: options.bandwidth_fraction ?? .9,
    diagonal_loading: options.diagonal_loading ?? .01};
  if (!Number.isInteger(descriptor.fft_size) || descriptor.fft_size < 256 ||
      descriptor.fft_size > 16384 || (descriptor.fft_size & (descriptor.fft_size - 1)) ||
      !Number.isInteger(descriptor.subbands) || descriptor.subbands < 4 ||
      descriptor.subbands > 64 || !(descriptor.bandwidth_fraction > 0) ||
      descriptor.bandwidth_fraction > 1 || !(descriptor.diagonal_loading > 0) ||
      descriptor.diagonal_loading > 1)
    throw new Error('Invalid frequency-resolved shadow settings');
  return descriptor;
}

function analysisFingerprint(descriptor) {
  return crypto.createHash('sha256').update(JSON.stringify(descriptor)).digest('hex');
}

function fft(real, imaginary) {
  const count = real.length;
  if (count !== imaginary.length || count < 2 || (count & (count - 1)))
    throw new Error('FFT length must be a power of two');
  for (let index = 1, reversed = 0; index < count; ++index) {
    let bit = count >> 1;
    for (; reversed & bit; bit >>= 1) reversed ^= bit;
    reversed ^= bit;
    if (index < reversed) {
      [real[index], real[reversed]] = [real[reversed], real[index]];
      [imaginary[index], imaginary[reversed]] = [imaginary[reversed], imaginary[index]];
    }
  }
  for (let length = 2; length <= count; length <<= 1) {
    const angle = -2 * Math.PI / length, stepReal = Math.cos(angle), stepImaginary = Math.sin(angle);
    for (let start = 0; start < count; start += length) {
      let weightReal = 1, weightImaginary = 0;
      for (let offset = 0; offset < length / 2; ++offset) {
        const even = start + offset, odd = even + length / 2;
        const oddReal = real[odd] * weightReal - imaginary[odd] * weightImaginary;
        const oddImaginary = real[odd] * weightImaginary + imaginary[odd] * weightReal;
        const evenReal = real[even], evenImaginary = imaginary[even];
        real[even] = evenReal + oddReal; imaginary[even] = evenImaginary + oddImaginary;
        real[odd] = evenReal - oddReal; imaginary[odd] = evenImaginary - oddImaginary;
        const nextReal = weightReal * stepReal - weightImaginary * stepImaginary;
        weightImaginary = weightReal * stepImaginary + weightImaginary * stepReal;
        weightReal = nextReal;
      }
    }
  }
}

class SpectralAccumulator {
  constructor(descriptor) {
    this.descriptor = descriptor;
    const {channels, subbands} = descriptor;
    this.covariances = Array.from({length: subbands}, () =>
      Array.from({length: channels}, () =>
        Array.from({length: channels}, () => complex())));
    this.binCounts = new Uint32Array(subbands);
    this.blocks = 0;
    this.window = Float64Array.from({length: descriptor.fft_size}, (_, index) =>
      .5 - .5 * Math.cos(2 * Math.PI * index / (descriptor.fft_size - 1)));
  }

  addBlock(block) {
    const {channels, fft_size: size, sample_rate_hz: rate,
      bandwidth_fraction: fraction, subbands} = this.descriptor;
    if (!Array.isArray(block) || block.length !== channels ||
        block.some(channel => channel.real.length !== size || channel.imaginary.length !== size))
      throw new Error('Spectral block dimensions differ from configuration');
    const transformed = block.map(channel => {
      const real = new Float64Array(size), imaginary = new Float64Array(size);
      for (let index = 0; index < size; ++index) {
        real[index] = channel.real[index] * this.window[index];
        imaginary[index] = channel.imaginary[index] * this.window[index];
      }
      fft(real, imaginary); return {real, imaginary};
    });
    const usable = rate * fraction, first = -usable / 2;
    for (let bin = 0; bin < size; ++bin) {
      const offset = (bin < size / 2 ? bin : bin - size) * rate / size;
      if (offset < first || offset >= -first) continue;
      const band = Math.min(subbands - 1, Math.floor((offset - first) / usable * subbands));
      ++this.binCounts[band];
      for (let left = 0; left < channels; ++left) {
        const a = [transformed[left].real[bin], transformed[left].imaginary[bin]];
        for (let right = 0; right < channels; ++right) {
          const b = [transformed[right].real[bin], transformed[right].imaginary[bin]];
          this.covariances[band][left][right] = add(this.covariances[band][left][right],
            multiply(a, conjugate(b)));
        }
      }
    }
    ++this.blocks;
  }

  finish() {
    if (!this.blocks || [...this.binCounts].some(count => !count))
      throw new Error('Insufficient spectral observations');
    return this.covariances.map((matrix, band) => matrix.map(row => row.map(value =>
      scale(value, 1 / this.binCounts[band]))));
  }
}

function matrixVector(matrix, vector) {
  return matrix.map(row => row.reduce((sum, value, index) =>
    add(sum, multiply(value, vector[index])), complex()));
}

function inner(left, right) {
  return left.reduce((sum, value, index) => add(sum,
    multiply(conjugate(value), right[index])), complex());
}

function principal(matrix, iterations = 24) {
  const count = matrix.length;
  let vector = Array.from({length: count}, () => [1 / Math.sqrt(count), 0]);
  for (let iteration = 0; iteration < iterations; ++iteration) {
    const next = matrixVector(matrix, vector);
    const norm = Math.sqrt(next.reduce((sum, value) => sum + magnitude2(value), 0));
    if (!(norm > 0) || !finite(norm)) throw new Error('Subband covariance has no principal vector');
    vector = next.map(value => scale(value, 1 / norm));
  }
  const anchor = vector[0];
  if (!(magnitude2(anchor) > 1e-20)) throw new Error('Subband principal vector has a zero anchor');
  const phase = divide([Math.sqrt(magnitude2(anchor)), 0], anchor);
  vector = vector.map(value => multiply(value, phase));
  const eigenvalue = inner(vector, matrixVector(matrix, vector))[0];
  const trace = matrix.reduce((sum, row, index) => sum + row[index][0], 0);
  return {vector, eigenvalue, trace, rank1Fraction: eigenvalue / Math.max(trace, 1e-30)};
}

function inverse(matrix) {
  const count = matrix.length;
  const left = matrix.map(row => row.map(value => [...value]));
  const right = Array.from({length: count}, (_, row) =>
    Array.from({length: count}, (_, column) => [row === column ? 1 : 0, 0]));
  for (let pivot = 0; pivot < count; ++pivot) {
    let choice = pivot;
    for (let row = pivot + 1; row < count; ++row)
      if (magnitude2(left[row][pivot]) > magnitude2(left[choice][pivot])) choice = row;
    if (!(magnitude2(left[choice][pivot]) > 1e-24)) return null;
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
        left[row][column] = add(left[row][column], scale(multiply(factor, left[pivot][column]), -1));
        right[row][column] = add(right[row][column], scale(multiply(factor, right[pivot][column]), -1));
      }
    }
  }
  return right;
}

function idealVector(descriptor, frequency) {
  const angle = descriptor.direct_path_local_azimuth_deg * Math.PI / 180;
  const wave = 2 * Math.PI * frequency / SPEED_OF_LIGHT;
  const vector = descriptor.positions_m.map(position => {
    const phase = wave * (position[0] * Math.cos(angle) + position[1] * Math.sin(angle));
    return [Math.cos(phase), Math.sin(phase)];
  });
  const norm = Math.sqrt(vector.length);
  return vector.map(value => scale(value, 1 / norm));
}

function unwrap(values) {
  const result = [values[0]];
  for (let index = 1; index < values.length; ++index) {
    let value = values[index];
    while (value - result[index - 1] > Math.PI) value -= 2 * Math.PI;
    while (value - result[index - 1] < -Math.PI) value += 2 * Math.PI;
    result.push(value);
  }
  return result;
}

function linearPhase(offsets, phases, weights) {
  const total = weights.reduce((sum, value) => sum + value, 0);
  const x = offsets.reduce((sum, value, index) => sum + value * weights[index], 0) / total;
  const y = phases.reduce((sum, value, index) => sum + value * weights[index], 0) / total;
  let numerator = 0, denominator = 0;
  for (let index = 0; index < offsets.length; ++index) {
    numerator += weights[index] * (offsets[index] - x) * (phases[index] - y);
    denominator += weights[index] * (offsets[index] - x) ** 2;
  }
  const slope = numerator / denominator, intercept = y - slope * x;
  const residual = phases.map((value, index) => value - (intercept + slope * offsets[index]));
  return {slope, residual, rms: Math.sqrt(residual.reduce((sum, value, index) =>
    sum + weights[index] * value * value, 0) / total)};
}

function summarizeCovariances(covariances, descriptor, metadata = {}) {
  if (!Array.isArray(covariances) || covariances.length !== descriptor.subbands)
    throw new Error('Subband covariance count differs from configuration');
  const usable = descriptor.sample_rate_hz * descriptor.bandwidth_fraction;
  const offsets = Array.from({length: descriptor.subbands}, (_, band) =>
    -usable / 2 + usable * (band + .5) / descriptor.subbands);
  const rows = covariances.map((raw, band) => {
    const matrix = raw.map(row => row.map(value => [...value]));
    const p = principal(matrix), ideal = idealVector(descriptor, descriptor.frequency_hz + offsets[band]);
    const correlation = Math.sqrt(magnitude2(inner(ideal, p.vector)));
    const loaded = matrix.map((row, index) => row.map((value, column) =>
      index === column ? add(value, [descriptor.diagonal_loading * p.trace / descriptor.channels, 0]) : [...value]));
    const inv = inverse(loaded);
    let mvdr = null;
    if (inv) {
      const candidate = matrixVector(inv, ideal), denominator = inner(ideal, candidate);
      if (magnitude2(denominator) > 1e-24) {
        const weights = candidate.map(value => divide(value, denominator));
        mvdr = {weights, noise_gain: weights.reduce((sum, value) => sum + magnitude2(value), 0),
          received_power: inner(weights, matrixVector(matrix, weights))[0]};
      }
    }
    return {offset_hz: offsets[band], rank1_fraction: p.rank1Fraction,
      ideal_correlation: correlation, received_power: p.trace,
      principal_response: p.vector, direct_constrained_mvdr: mvdr};
  });
  const phase = [];
  for (let channel = 1; channel < descriptor.channels; ++channel) {
    const phases = unwrap(rows.map(row => Math.atan2(row.principal_response[channel][1],
      row.principal_response[channel][0])));
    const weights = rows.map(row => row.received_power);
    const fit = linearPhase(offsets, phases, weights);
    phase.push({channel, phase_excursion_deg: (phases.at(-1) - phases[0]) * 180 / Math.PI,
      relative_group_delay_ns: -fit.slope / (2 * Math.PI) * 1e9,
      linear_fit_rms_deg: fit.rms * 180 / Math.PI,
      curvature_peak_to_peak_deg: (Math.max(...fit.residual) - Math.min(...fit.residual)) * 180 / Math.PI});
  }
  const mvdrRows = rows.map(row => row.direct_constrained_mvdr).filter(Boolean);
  return {schema: 'vectorwarp-frequency-resolved-reference-v1', generated_at: new Date().toISOString(),
    config_fingerprint: analysisFingerprint(descriptor), descriptor, observations: metadata,
    summary: {rank1_fraction: {min: Math.min(...rows.map(row => row.rank1_fraction)),
      median: median(rows.map(row => row.rank1_fraction)), max: Math.max(...rows.map(row => row.rank1_fraction))},
    ideal_correlation: {min: Math.min(...rows.map(row => row.ideal_correlation)),
      median: median(rows.map(row => row.ideal_correlation)), max: Math.max(...rows.map(row => row.ideal_correlation))},
    mvdr_noise_gain: mvdrRows.length ? {median: median(mvdrRows.map(row => row.noise_gain)),
      max: Math.max(...mvdrRows.map(row => row.noise_gain))} : null,
    channel_frequency_phase: phase}, subbands: rows};
}

module.exports = {SPEED_OF_LIGHT, analysisDescriptor, analysisFingerprint, fft,
  SpectralAccumulator, summarizeCovariances, idealVector, transmitterBearing};
