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
const percentile = (values, fraction) => {
  if (!values.length) return null;
  const sorted = [...values].sort((a, b) => a - b);
  return sorted[Math.max(0, Math.min(sorted.length - 1, Math.ceil(fraction * sorted.length) - 1))];
};
const radians = value => value * Math.PI / 180;
const degrees = value => value * 180 / Math.PI;

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

function siteBearing(config) {
  const rx = config.location?.rx, tx = config.location?.tx;
  if (![rx?.latitude, rx?.longitude, tx?.latitude, tx?.longitude].every(finite)) return null;
  const start = radians(rx.latitude), end = radians(tx.latitude);
  const delta = radians(tx.longitude - rx.longitude);
  const y = Math.sin(delta) * Math.cos(end);
  const x = Math.cos(start) * Math.sin(end) -
    Math.sin(start) * Math.cos(end) * Math.cos(delta);
  return normalize(degrees(Math.atan2(y, x)));
}

function expectedReference(config, frequency, mapping, headingDegTrue, anchorChannel) {
  const {positions} = geometry(config), transmitterBearing = siteBearing(config);
  if (!finite(frequency) || !(frequency > 0) || !finite(transmitterBearing)) return null;
  const direct = radians(normalize(headingDegTrue - transmitterBearing));
  const wave = 2 * Math.PI * frequency / 299792458;
  const values = positions.map((_, channel) => {
    const position = positions[mapping[channel]];
    return polar(wave * (position[0] * Math.cos(direct) + position[1] * Math.sin(direct)));
  });
  const anchor = values[anchorChannel];
  return values.map(value => divide(value, anchor));
}

function referenceRow(row, channels) {
  const relative = row?.relative_eigenvector;
  if (row?.schema !== 'vectorwarp-reference-drift-v1' || !finite(row.frequency_hz) ||
      !Number.isInteger(relative?.anchor_channel) || relative.anchor_channel < 0 ||
      relative.anchor_channel >= channels || !Array.isArray(relative.channels) ||
      relative.channels.length !== channels || relative.channels.some(channel =>
        !finite(channel?.phase_deg) || !finite(channel?.amplitude_db))) return null;
  const observed = relative.channels.map(channel =>
    scale(polar(radians(channel.phase_deg)), 10 ** (channel.amplitude_db / 20)));
  const coherent = row.reference_state?.coherent_fraction;
  return {frequency: row.frequency_hz, anchor: relative.anchor_channel, observed,
    coherent: finite(coherent) ? coherent : null,
    amplitudeSpan: Math.max(...relative.channels.map(channel => channel.amplitude_db)) -
      Math.min(...relative.channels.map(channel => channel.amplitude_db)),
    timestamp: row.radar_timestamp_ms, fingerprint: row.config_fingerprint};
}

function referenceFit(rows, config, mapping, heading) {
  const errors = [], byChannel = Array.from({length: mapping.length}, () => []);
  for (const row of rows) {
    const expected = expectedReference(config, row.frequency, mapping, heading, row.anchor);
    if (!expected) continue;
    const rowErrors = [];
    for (let channel = 0; channel < row.observed.length; ++channel) if (channel !== row.anchor) {
      const error = circularError(degrees(Math.atan2(row.observed[channel][1], row.observed[channel][0])),
        degrees(Math.atan2(expected[channel][1], expected[channel][0])));
      if (finite(error)) { rowErrors.push(error); byChannel[channel].push(error); }
    }
    if (rowErrors.length) errors.push(median(rowErrors));
  }
  return {updates: errors.length, median_phase_error_deg: errors.length ? median(errors) : null,
    p95_phase_error_deg: percentile(errors, .95),
    channel_median_phase_error_deg: byChannel.map(values => values.length ? median(values) : null)};
}

function evaluateReferenceDirection(referenceUpdates, config) {
  let positions;
  try { positions = geometry(config).positions; }
  catch (error) { return {available: false, reason: error.message}; }
  const transmitterBearing = siteBearing(config);
  if (!finite(transmitterBearing)) return {available: false,
    reason: 'Configured receiver and transmitter coordinates are required.'};
  const parsed = referenceUpdates.map(row => referenceRow(row, positions.length)).filter(Boolean);
  if (!parsed.length) return {available: false,
    reason: 'No replay-capable reference eigenbeam updates have been recorded yet.'};
  const latestFingerprint = [...parsed].reverse().find(row => row.fingerprint)?.fingerprint;
  const rows = latestFingerprint ? parsed.filter(row => row.fingerprint === latestFingerprint) : parsed;
  const configuredMapping = config.process?.bearing?.channel_mapping ||
    Array.from({length: positions.length}, (_, index) => index);
  if (!Array.isArray(configuredMapping) || configuredMapping.length !== positions.length ||
      new Set(configuredMapping).size !== positions.length || configuredMapping.some(value =>
        !Number.isInteger(value) || value < 0 || value >= positions.length)) return {available: false,
    reason: 'The configured bearing channel mapping is invalid.'};
  const configuredHeading = geometry(config).xAxis;
  const mappingResults = mappings(positions.length).map(candidate => ({...candidate,
    ...referenceFit(rows, config, candidate.mapping, configuredHeading)})).filter(item => item.updates);
  mappingResults.sort((left, right) => left.median_phase_error_deg - right.median_phase_error_deg ||
    left.p95_phase_error_deg - right.p95_phase_error_deg);
  const configured = referenceFit(rows, config, configuredMapping, configuredHeading);
  const headingResults = Array.from({length: 360}, (_, heading) => ({heading_deg_true: heading,
    ...referenceFit(rows, config, configuredMapping, heading)}));
  headingResults.sort((left, right) => left.median_phase_error_deg - right.median_phase_error_deg ||
    left.p95_phase_error_deg - right.p95_phase_error_deg);
  const bestHeading = headingResults[0];
  const correction = ((bestHeading.heading_deg_true - configuredHeading + 540) % 360) - 180;
  const coherent = rows.map(row => row.coherent).filter(finite);
  const amplitudeSpans = rows.map(row => row.amplitudeSpan).filter(finite);
  const timestamps = rows.map(row => row.timestamp).filter(finite).sort((a, b) => a - b);
  const coherentMedian = coherent.length ? median(coherent) : null;
  const amplitudeSpanMedian = amplitudeSpans.length ? median(amplitudeSpans) : null;
  const reliable = rows.length >= 10 && finite(coherentMedian) && coherentMedian >= .8 &&
    finite(amplitudeSpanMedian) && amplitudeSpanMedian <= 6;
  return {available: true, shadow_only: true, auto_apply: false,
    source: {name: config.location?.tx?.name || 'configured transmitter',
      bearing_deg_true: transmitterBearing},
    configured: {heading_deg_true: configuredHeading,
      direct_path_local_azimuth_deg: normalize(configuredHeading - transmitterBearing),
      mapping: configuredMapping, ...configured},
    best_mapping_at_configured_heading: mappingResults[0] && {
      id: mappingResults[0].id, mapping: mappingResults[0].mapping,
      median_phase_error_deg: mappingResults[0].median_phase_error_deg,
      p95_phase_error_deg: mappingResults[0].p95_phase_error_deg,
      channel_median_phase_error_deg: mappingResults[0].channel_median_phase_error_deg},
    heading_with_configured_mapping: {...bestHeading, signed_correction_deg: correction},
    quality: {updates: rows.length,
      reliable,
      first_timestamp_ms: timestamps[0] ?? null,
      last_timestamp_ms: timestamps[timestamps.length - 1] ?? null,
      coherent_fraction_median: coherentMedian,
      coherent_fraction_min: coherent.length ? Math.min(...coherent) : null,
      amplitude_span_db_median: amplitudeSpanMedian,
      amplitude_span_db_p95: percentile(amplitudeSpans, .95)},
    interpretation: 'Observed reference eigenbeam phases are compared with the configured transmitter as an independent known-angle source.',
    limitation: 'Array symmetry leaves mapping/heading aliases, and multipath or per-channel phase bias can move the best fit. This diagnostic never changes live geometry.'};
}

function evaluateReferenceSurvey(sources, config) {
  let channelCount;
  try { channelCount = geometry(config).positions.length; }
  catch (error) { return {available: false, reason: error.message}; }
  if (!Array.isArray(sources) || !sources.length) return {available: false,
    reason: 'At least one known-angle reference source is required.'};
  const prepared = sources.map((source, index) => {
    const sourceConfig = JSON.parse(JSON.stringify(config));
    sourceConfig.location = sourceConfig.location || {};
    sourceConfig.location.tx = {name: source.name || `source-${index + 1}`,
      latitude: source.latitude, longitude: source.longitude, altitude: source.altitude};
    const parsed = (source.reference_updates || []).map(row => referenceRow(row, channelCount)).filter(Boolean);
    const latestFingerprint = [...parsed].reverse().find(row => row.fingerprint)?.fingerprint;
    const rows = (latestFingerprint ? parsed.filter(row => row.fingerprint === latestFingerprint) : parsed)
      .slice(-256).map(row => ({...row, frequency: source.frequency_hz || row.frequency}));
    const coherent = rows.map(row => row.coherent).filter(finite);
    const spans = rows.map(row => row.amplitudeSpan).filter(finite);
    const quality = {updates: rows.length,
      coherent_fraction_median: coherent.length ? median(coherent) : null,
      amplitude_span_db_median: spans.length ? median(spans) : null};
    quality.reliable = quality.updates >= 10 && finite(quality.coherent_fraction_median) &&
      quality.coherent_fraction_median >= .8 && finite(quality.amplitude_span_db_median) &&
      quality.amplitude_span_db_median <= 6;
    return {name: sourceConfig.location.tx.name, config: sourceConfig, rows,
      bearing_deg_true: siteBearing(sourceConfig), quality};
  });
  const reliable = prepared.filter(source => source.quality.reliable && finite(source.bearing_deg_true));
  if (reliable.length < 2) return {available: false,
    reason: 'At least two references must pass the RF quality gate (10 updates, coherence >= 0.8, amplitude span <= 6 dB).',
    sources: prepared.map(({name, bearing_deg_true, quality}) => ({name, bearing_deg_true, quality}))};
  const candidates = [];
  for (const candidate of mappings(channelCount)) for (let heading = 0; heading < 360; ++heading) {
    const fits = reliable.map(source => ({name: source.name,
      ...referenceFit(source.rows, source.config, candidate.mapping, heading)}));
    candidates.push({id: candidate.id, direction: candidate.direction, rotation: candidate.rotation,
      mapping: candidate.mapping, fitted_phase_axis_heading_deg_true: heading,
      median_phase_error_deg: median(fits.map(fit => fit.median_phase_error_deg)), sources: fits});
  }
  const rank = values => [...values].sort((left, right) =>
    left.median_phase_error_deg - right.median_phase_error_deg);
  const configuredMapping = config.process?.bearing?.channel_mapping ||
    Array.from({length: channelCount}, (_, index) => index);
  const configuredId = mappings(channelCount).find(candidate =>
    candidate.mapping.every((value, index) => value === configuredMapping[index]))?.id;
  const sourceSummary = prepared.map(({name, bearing_deg_true, quality}) =>
    ({name, bearing_deg_true, quality}));
  return {available: true, shadow_only: true, auto_apply: false,
    physical_ch0_heading_deg_true: geometry(config).xAxis,
    quality_gate: {minimum_updates: 10, minimum_coherence: .8, maximum_amplitude_span_db: 6},
    sources: sourceSummary, reliable_sources: reliable.map(source => source.name),
    configured_mapping: rank(candidates.filter(candidate => candidate.id === configuredId))[0],
    best_forward_family: rank(candidates.filter(candidate => candidate.direction === 'forward'))[0],
    best_reverse_family: rank(candidates.filter(candidate => candidate.direction === 'reverse'))[0],
    limitation: 'The fitted phase-axis heading is not the physical CH0/+X heading. Cyclic channel rotations remain exactly degenerate with equal regular-polygon phase-axis offsets; physical channel identity selects the representative. This survey never changes live geometry.'};
}

function weights(sample, config, mapping, beams) {
  const {positions} = geometry(config);
  const channels = positions.length;
  const frequency = sample.frequency_hz;
  const direct = sample.direct_path_local_azimuth_deg * Math.PI / 180;
  const wave = 2 * Math.PI * frequency / 299792458;
  const reference = sample.reference_weights;
  const fixedSite = sample.bearing_replay?.reference_anchor === 'fixed_site';
  const anchor = positions.map((_, channel) => {
    if (!fixedSite) return reference[channel];
    const position = positions[mapping[channel]];
    return polar(wave * (position[0] * Math.cos(direct) + position[1] * Math.sin(direct)));
  });
  const norm = anchor.reduce((sum, value) => sum + magnitude2(value), 0);
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
      steering.push(multiply(anchor[channel], polar(relative)));
    }
    if (nullFraction > 0) {
      const projection = scale(steering.reduce((sum, value, channel) =>
        add(sum, multiply(conjugate(anchor[channel]), value)), [0, 0]), nullFraction / norm);
      for (let channel = 0; channel < channels; ++channel)
        steering[channel] = subtract(steering[channel], multiply(projection, anchor[channel]));
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

function bestMeasuredBearing(sample, config, manifold) {
  const beams = manifold?.calibration?.beams;
  if (!Number.isInteger(beams) || !Array.isArray(manifold.responses) ||
      manifold.responses.length !== beams || manifold.responses.some(row =>
        !Array.isArray(row) || row.length !== sample.channels))
    throw new Error('Invalid measured manifold for replay');
  const directBin = manifold.calibration.direct_path_bin;
  if (!Number.isInteger(directBin) || directBin < 0 || directBin >= beams)
    throw new Error('Measured manifold has no direct-path bin');
  const fixedSite = sample.bearing_replay?.reference_anchor === 'fixed_site';
  const anchor = fixedSite ? manifold.responses[directBin] : sample.reference_weights;
  const anchorNorm = anchor.reduce((sum, value) => sum + magnitude2(value), 0);
  if (!(anchorNorm > 0)) throw new Error('Measured manifold reference has zero norm');
  const nullFraction = 1 - 10 ** (-(config.process?.surveillance_fusion?.direct_path_null_db || 0) / 20);
  const covarianceInverse = sample.bearing_replay?.method === 'covariance_mvdr' ? inverse(sample.covariance) : null;
  if (sample.bearing_replay?.method === 'covariance_mvdr' && !covarianceInverse)
    throw new Error('Recorded covariance is singular');
  let best = 0, bestScore = -1;
  for (let beam = 0; beam < beams; ++beam) {
    let steering = manifold.responses[beam].map((value, channel) => fixedSite ? value :
      multiply(sample.reference_weights[channel], divide(value, manifold.responses[directBin][channel])));
    if (nullFraction > 0) {
      const projection = scale(steering.reduce((sum, value, channel) =>
        add(sum, multiply(conjugate(anchor[channel]), value)), [0, 0]), nullFraction / anchorNorm);
      steering = steering.map((value, channel) => subtract(value, multiply(projection, anchor[channel])));
    }
    const weights = steering.map(conjugate);
    const value = score(sample.response, weights, covarianceInverse);
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
    reference_anchor: valid[valid.length - 1].bearing_replay.reference_anchor || 'live_eigenbeam',
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

function readReferenceUpdates(filename, options) { return readSamples(filename, options); }

function createBearingDiagnostic(config, {fileSystem = fs} = {}) {
  const filename = config.process?.bearing?.manifold_recorder?.path;
  const referenceFilename = filename ? `${filename}.reference.jsonl` : null;
  let cache = null;
  return {status() {
    if (!filename) return {available: false, reason: 'No manifold recorder path is configured.'};
    try {
      let stat = null, referenceStat = null;
      try { stat = fileSystem.statSync(filename); }
      catch (error) { if (error.code !== 'ENOENT') throw error; }
      try { referenceStat = fileSystem.statSync(referenceFilename); }
      catch (error) { if (error.code !== 'ENOENT') throw error; }
      if (!cache || cache.size !== stat?.size || cache.mtimeMs !== stat?.mtimeMs ||
          cache.referenceSize !== referenceStat?.size || cache.referenceMtimeMs !== referenceStat?.mtimeMs) {
        const value = stat ? evaluateBearing(readSamples(filename), config) :
          {available: false, reason: 'No bearing samples have been recorded yet.'};
        value.known_angle_reference = evaluateReferenceDirection(referenceStat ?
          readReferenceUpdates(referenceFilename, {maximumRows: 4096}) : [], config);
        cache = {size: stat?.size, mtimeMs: stat?.mtimeMs,
          referenceSize: referenceStat?.size, referenceMtimeMs: referenceStat?.mtimeMs, value};
      }
      return cache.value;
    } catch (error) {
      return {available: false, reason: error.message};
    }
  }};
}

module.exports = {evaluateBearing, evaluateReferenceDirection, evaluateReferenceSurvey, createBearingDiagnostic,
  readSamples, readReferenceUpdates, mappings, inverse,
  bestBearing, bestMeasuredBearing, circularError,
  steeringWeights: weights};
