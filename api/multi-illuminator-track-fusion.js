'use strict';

const {inverse, destination} = require('./bistatic-geolocation');

const C_KM_S = 299792.458;
const finite = value => typeof value === 'number' && Number.isFinite(value);
const radians = degrees => degrees * Math.PI / 180;
const degrees = value => value * 180 / Math.PI;
// ASSOCIATED is still a tracker candidate in VectorWarp.  Feeding it into the
// geographic solver multiplies candidate clutter across illuminators and can
// make an unconfirmed range crossing look like a localized aircraft.
const operationalStates = new Set(['ACTIVE', 'CONFIRMED']);
const DEFAULT_LIMITS = Object.freeze({measurementsPerLane: 24, pairEvaluations: 2048,
  hypotheses: 256});
const statePriority = Object.freeze({CONFIRMED: 4, ACTIVE: 4, COASTING: 3, ASSOCIATED: 2});
const DEFAULT_GEOMETRY = Object.freeze({siteSeparationKm: .1, rangeNoiseSigmaKm: .05,
  maximumCondition: 100, maximumHorizontalSigmaKm: 2, maximumVerticalSigmaKm: 2,
  minimumAltitudeM: 0, maximumAltitudeM: 20000, maximumReceiverRangeKm: 300});
const numeric = value => value !== null && value !== '' && value !== undefined && finite(Number(value));
const clone = value => value === undefined ? undefined : JSON.parse(JSON.stringify(value));
const dot = (a, b) => a.reduce((sum, value, index) => sum + value * b[index], 0);
const norm = a => Math.hypot(...a);
const sub = (a, b) => a.map((value, index) => value - b[index]);
const validLocation = value => [value?.latitude, value?.longitude, value?.altitude].every(numeric) &&
  Math.abs(Number(value.latitude)) <= 90 && Math.abs(Number(value.longitude)) <= 180;

// All ranges are Euclidean WGS84 ECEF distances in kilometres. Config altitude
// is metres above the WGS84 ellipsoid; no ADS-B or ground-plane altitude enters.
function ecef(point) {
  const latitude = radians(Number(point.latitude)), longitude = radians(Number(point.longitude));
  const altitude = Number(point.altitude) / 1000, e2 = 6.6943799901413165e-3;
  const radius = 6378.137 / Math.sqrt(1 - e2 * Math.sin(latitude) ** 2);
  return [(radius + altitude) * Math.cos(latitude) * Math.cos(longitude),
    (radius + altitude) * Math.cos(latitude) * Math.sin(longitude),
    (radius * (1 - e2) + altitude) * Math.sin(latitude)];
}

function basis(origin) {
  const lat = radians(Number(origin.latitude)), lon = radians(Number(origin.longitude));
  return [[-Math.sin(lon), Math.cos(lon), 0],
    [-Math.sin(lat) * Math.cos(lon), -Math.sin(lat) * Math.sin(lon), Math.cos(lat)],
    [Math.cos(lat) * Math.cos(lon), Math.cos(lat) * Math.sin(lon), Math.sin(lat)]];
}

function localPoint3d(origin, point) {
  const delta = sub(ecef(point), ecef(origin));
  return basis(origin).map(axis => dot(axis, delta));
}

function geographicPoint3d(origin, point) {
  const axes = basis(origin), center = ecef(origin);
  const xyz = center.map((value, index) => value + point.reduce((sum, component, axis) =>
    sum + component * axes[axis][index], 0));
  const p = Math.hypot(xyz[0], xyz[1]), e2 = 6.6943799901413165e-3;
  let lat = Math.atan2(xyz[2], p * (1 - e2));
  for (let iteration = 0; iteration < 12; iteration += 1) {
    const radius = 6378.137 / Math.sqrt(1 - e2 * Math.sin(lat) ** 2);
    lat = Math.atan2(xyz[2] + e2 * radius * Math.sin(lat), p);
  }
  const radius = 6378.137 / Math.sqrt(1 - e2 * Math.sin(lat) ** 2);
  const altitude = Math.abs(Math.cos(lat)) > 1e-8 ? p / Math.cos(lat) - radius :
    Math.abs(xyz[2]) - radius * (1 - e2);
  return {latitude: degrees(lat), longitude: degrees(Math.atan2(xyz[1], xyz[0])), altitude: altitude * 1000};
}

const excessPath3d = (point, transmitter) => norm(point) + norm(sub(point, transmitter)) - norm(transmitter);

function inverse3(matrix) {
  const [[a, b, c], [d, e, f], [g, h, i]] = matrix;
  const determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
  const scale = Math.max(...matrix.flat().map(Math.abs));
  if (!finite(determinant) || Math.abs(determinant) < Math.max(1e-30, scale ** 3 * 1e-12)) return null;
  return {determinant, matrix: [[e * i - f * h, c * h - b * i, b * f - c * e],
    [f * g - d * i, a * i - c * g, c * d - a * f],
    [d * h - e * g, b * g - a * h, a * e - b * d]].map(row => row.map(value => value / determinant))};
}

// Three ellipsoid equations become three linear equations in [east,north,up,r]
// plus |position|² = r². Choosing the best three columns handles coplanar sites
// without assuming the receiver-to-aircraft slant range or altitude is known.
function tripleRoots(measurements) {
  const rows = measurements.map(item => [...item.transmitter, -(norm(item.transmitter) + item.excessKm)]);
  const rhs = measurements.map(item => (dot(item.transmitter, item.transmitter) -
    (norm(item.transmitter) + item.excessKm) ** 2) / 2);
  let best = null;
  for (let free = 0; free < 4; free += 1) {
    const columns = [0, 1, 2, 3].filter(index => index !== free);
    const inverse = inverse3(rows.map(row => columns.map(index => row[index])));
    if (inverse && (!best || Math.abs(inverse.determinant) > Math.abs(best.inverse.determinant)))
      best = {free, columns, inverse};
  }
  if (!best) return [];
  const a = [0, 0, 0, 0], b = [0, 0, 0, 0]; a[best.free] = 1;
  best.columns.forEach((column, index) => {
    b[column] = dot(best.inverse.matrix[index], rhs);
    a[column] = -dot(best.inverse.matrix[index], rows.map(row => row[best.free]));
  });
  const minkowski = (left, right) => dot(left.slice(0, 3), right.slice(0, 3)) - left[3] * right[3];
  const aa = minkowski(a, a), bb = 2 * minkowski(a, b), cc = minkowski(b, b);
  const discriminant = bb * bb - 4 * aa * cc;
  if (discriminant < 0 || !finite(discriminant)) return [];
  let roots;
  if (Math.abs(aa) < 1e-12) roots = Math.abs(bb) > 1e-12 ? [-cc / bb] : [];
  else {
    const q = -.5 * (bb + (bb >= 0 ? 1 : -1) * Math.sqrt(discriminant));
    roots = q === 0 ? [-bb / (2 * aa)] : [q / aa, cc / q];
  }
  return roots.map(root => a.map((value, index) => value * root + b[index]))
    .filter(value => value.every(finite) && value[3] > 0 &&
      measurements.every(item => value[3] <= norm(item.transmitter) + item.excessKm + 1e-6))
    .map(value => value.slice(0, 3));
}

function siteCatalog(definitions, receiver, minimumSeparationKm, groupingMode = 'automatic') {
  const entries = definitions.filter(item => validLocation(item.transmitter)).map(item => ({
    definition: item, point: localPoint3d(receiver, item.transmitter),
    explicit: String(item.transmitter.site_id ?? item.transmitter_site_id ?? '').trim()}));
  const parents = entries.map((_, index) => index), inconsistent = new Set();
  const root = index => parents[index] === index ? index : (parents[index] = root(parents[index]));
  for (let left = 0; left < entries.length; left += 1) for (let right = left + 1; right < entries.length; right += 1) {
    const a = entries[left], b = entries[right];
    const distance = inverse(a.definition.transmitter, b.definition.transmitter).distanceM / 1000;
    const sharedIdentity = a.explicit && a.explicit === b.explicit;
    if (sharedIdentity && norm(sub(a.point, b.point)) > minimumSeparationKm) {
      inconsistent.add(left); inconsistent.add(right);
    }
    // A matching stable ID is authoritative. Automatic mode additionally
    // groups nearby coordinates even when channel/site labels differ: carrier
    // names must never turn one physical tower into independent geometry.
    // Operators who intentionally need identity-only grouping can select the
    // explicit mode.
    if (sharedIdentity || groupingMode !== 'explicit' && distance <= minimumSeparationKm)
      parents[root(right)] = root(left);
  }
  const groups = new Map();
  entries.forEach((entry, index) => {
    const key = root(index);
    if (!groups.has(key)) groups.set(key, []);
    groups.get(key).push({entry, index});
  });
  const catalog = new Map();
  for (const members of groups.values()) {
    const identity = [...new Set(members.map(item => item.entry.explicit).filter(Boolean))].sort().join('+') ||
      members.map(item => item.entry.definition.id).sort().join('+');
    const invalid = members.some(item => inconsistent.has(item.index));
    for (const {entry} of members) catalog.set(entry.definition.id, {...entry, identity: `site:${identity}`, invalid});
  }
  return catalog;
}

function localPoint(origin, point) {
  const polar = inverse(origin, point), bearing = radians(polar.bearingDeg);
  return {x: Math.sin(bearing) * polar.distanceM / 1000,
    y: Math.cos(bearing) * polar.distanceM / 1000};
}

function geographicPoint(origin, point) {
  const distanceKm = Math.hypot(point.x, point.y);
  const bearing = (degrees(Math.atan2(point.x, point.y)) + 360) % 360;
  return destination(origin, bearing, distanceKm * 1000);
}

function excessPath(point, transmitter) {
  return Math.hypot(point.x, point.y) +
    Math.hypot(point.x - transmitter.x, point.y - transmitter.y) -
    Math.hypot(transmitter.x, transmitter.y);
}

function ellipse(transmitter, excessKm) {
  const baseline = Math.hypot(transmitter.x, transmitter.y);
  if (!(baseline > 0) || !(excessKm > 0)) return null;
  const a = (baseline + excessKm) / 2, c = baseline / 2;
  const b2 = a * a - c * c;
  if (!(b2 > 0)) return null;
  const ux = transmitter.x / baseline, uy = transmitter.y / baseline;
  return {point(theta) {
    const major = a * Math.cos(theta), minor = Math.sqrt(b2) * Math.sin(theta);
    return {x: transmitter.x / 2 + ux * major - uy * minor,
      y: transmitter.y / 2 + uy * major + ux * minor};
  }};
}

function ellipseIntersections(first, second, toleranceKm, samples = 720) {
  const curve = ellipse(first.transmitter, first.excessKm);
  if (!curve) return [];
  const residual = theta => excessPath(curve.point(theta), second.transmitter) - second.excessKm;
  const roots = [];
  const add = theta => {
    const point = curve.point(theta);
    if (Math.abs(residual(theta)) > toleranceKm) return;
    if (!roots.some(candidate => Math.hypot(candidate.x - point.x, candidate.y - point.y) <=
        Math.max(.01, toleranceKm * 2))) roots.push(point);
  };
  let priorTheta = 0, prior = residual(0);
  for (let index = 1; index <= samples; index += 1) {
    const theta = 2 * Math.PI * index / samples, value = residual(theta);
    if (Math.abs(value) <= toleranceKm / 2) add(theta);
    if (finite(prior) && finite(value) && prior * value < 0) {
      let low = priorTheta, high = theta, lowValue = prior;
      for (let iteration = 0; iteration < 40; iteration += 1) {
        const middle = (low + high) / 2, middleValue = residual(middle);
        if (lowValue * middleValue <= 0) high = middle;
        else { low = middle; lowValue = middleValue; }
      }
      add((low + high) / 2);
    }
    priorTheta = theta; prior = value;
  }
  return roots;
}

function laneMeasurements(frame, definition, receiver, limit = DEFAULT_LIMITS.measurementsPerLane,
    site = null, geometry = DEFAULT_GEOMETRY) {
  const sampleRate = Number(frame?.sample_rate_hz ?? definition?.output_sample_rate_hz);
  const carrierHz = Number(frame?.frequency_hz ?? definition?.frequency_hz);
  const transmitter = definition?.transmitter;
  if (!finite(sampleRate) || !(sampleRate > 0) || !finite(carrierHz) || !(carrierHz > 0) ||
      frame?.sample_rate_hz !== undefined && sampleRate !== Number(definition?.output_sample_rate_hz) ||
      frame?.frequency_hz !== undefined && carrierHz !== Number(definition?.frequency_hz) ||
      !validLocation(receiver) || !validLocation(transmitter) || site?.invalid) return [];
  const tx = localPoint3d(receiver, transmitter), resolutionKm = C_KM_S / sampleRate;
  const transmitterSite = site?.identity || `site:${transmitter.site_id || definition.id}`;
  return (Array.isArray(frame?.tracks?.data) ? frame.tracks.data : [])
    .filter(track => operationalStates.has(String(track?.state || '').toUpperCase()) &&
      numeric(track?.delay) && numeric(track?.doppler) && Number(track.delay) > 0 &&
      track.id !== undefined && track.id !== null)
    .map(track => ({lane: frame.id, trackId: String(track.id), state: String(track.state).toUpperCase(),
      delayBins: Number(track.delay), excessKm: Number(track.delay) * resolutionKm,
      dopplerHz: Number(track.doppler), rangeRateKmS: -Number(track.doppler) * C_KM_S / carrierHz,
      carrierHz, sampleRateHz: sampleRate, resolutionKm, transmitter: tx,
      sigmaKm: Math.hypot(resolutionKm / Math.sqrt(12), geometry.rangeNoiseSigmaKm,
        numeric(track.delay_sigma_bins) && Number(track.delay_sigma_bins) > 0 ?
          Number(track.delay_sigma_bins) * resolutionKm : 0),
      transmitterSite,
      provenance: {illuminator: frame.id, track_id: String(track.id),
        frame: clone(frame?.provenance) ?? null, track: clone(track?.provenance) ?? null},
      falseAlarm: clone(track?.false_alarm ?? track?.false_alarm_probability ?? track?.pfa) ?? null,
      historyLength: Number(track.n) || 0}))
    .sort((left, right) => (statePriority[right.state] || 0) - (statePriority[left.state] || 0) ||
      right.historyLength - left.historyLength || left.trackId.localeCompare(right.trackId))
    .slice(0, Math.max(1, limit));
}

function cohortIdentity(frames) {
  if (!Array.isArray(frames) || frames.length < 2) return null;
  if (new Set(frames.map(frame => frame?.id)).size !== frames.length || frames.some(frame => !frame?.id)) return null;
  const first = frames[0];
  const captureAvailable = first.capture_sample_position_available === true;
  const epoch = captureAvailable ? first.capture_epoch : null;
  const start = first.capture_sample_position_available === true ?
    first.capture_first_input_sample : first.first_input_sample;
  const end = first.capture_sample_position_available === true ?
    first.capture_end_input_sample : first.end_input_sample;
  if (![start, end].every(Number.isSafeInteger) || start < 0 || end <= start) return null;
  if (captureAvailable && !(Number.isSafeInteger(epoch) && epoch >= 0 ||
      typeof epoch === 'string' && epoch.trim().length)) return null;
  const timestamp = Number(first.timestamp);
  const trackTimestamp = Number(first.tracks?.timestamp ?? first.timestamp);
  if (![first.timestamp, first.tracks?.timestamp ?? first.timestamp].every(numeric) ||
      ![timestamp, trackTimestamp].every(value => finite(value) && value >= 0)) return null;
  if (!frames.every(frame => {
    const frameEpoch = frame.capture_sample_position_available === true ? frame.capture_epoch : null;
    const frameStart = frame.capture_sample_position_available === true ?
      frame.capture_first_input_sample : frame.first_input_sample;
    const frameEnd = frame.capture_sample_position_available === true ?
      frame.capture_end_input_sample : frame.end_input_sample;
    return (frame.capture_sample_position_available === true) === captureAvailable &&
      frameEpoch === epoch && frameStart === start && frameEnd === end &&
      Number(frame.timestamp) === timestamp &&
      Number(frame.tracks?.timestamp ?? frame.timestamp) === trackTimestamp;
  })) return null;
  return {capture_epoch: epoch, capture_first_input_sample: start,
    capture_end_input_sample: end, timestamp: trackTimestamp,
    capture_epoch_verified: captureAvailable,
    key: `${typeof epoch}:${epoch ?? 'legacy'}:${start}:${end}`};
}

function selectMeasurements(point, lanes, gateSigma = 3) {
  const selected = [];
  for (const lane of lanes) {
    const best = lane.measurements.map(measurement => ({measurement,
      residualKm: excessPath3d(point, measurement.transmitter) - measurement.excessKm}))
      .sort((a, b) => Math.abs(a.residualKm / a.measurement.sigmaKm) -
        Math.abs(b.residualKm / b.measurement.sigmaKm) || a.measurement.trackId.localeCompare(b.measurement.trackId))[0];
    if (best && Math.abs(best.residualKm) <= best.measurement.sigmaKm * gateSigma) selected.push(best);
  }
  return selected;
}

// Carrier lanes at one physical transmitter are alternative observations of
// the same bistatic range, never independent votes. Grouping before seed
// generation and residual selection gives a transmitter geometry at most one
// hit per capture CPI while still allowing the selected carrier to change on
// every update as returns appear intermittently across frequencies.
function geometryGroups(lanes) {
  const groups = new Map();
  for (const lane of lanes) for (const measurement of lane.measurements) {
    if (!groups.has(measurement.transmitterSite))
      groups.set(measurement.transmitterSite, {site: measurement.transmitterSite, measurements: []});
    groups.get(measurement.transmitterSite).measurements.push(measurement);
  }
  return [...groups.values()].map(group => ({...group,
    measurements: group.measurements.sort((left, right) =>
      (statePriority[right.state] || 0) - (statePriority[left.state] || 0) ||
      right.historyLength - left.historyLength || left.lane.localeCompare(right.lane) ||
      left.trackId.localeCompare(right.trackId))}));
}

function independentMeasurements(selected) {
  const sites = new Map();
  for (const item of selected) {
    const prior = sites.get(item.measurement.transmitterSite);
    if (!prior || item.measurement.sigmaKm < prior.measurement.sigmaKm)
      sites.set(item.measurement.transmitterSite, item);
  }
  return [...sites.values()];
}

function information(point, selected) {
  const matrix = Array.from({length: 3}, () => [0, 0, 0]), gradient = [0, 0, 0];
  let score = 0;
  for (const {measurement} of independentMeasurements(selected)) {
    const fromTx = sub(point, measurement.transmitter), rxRange = norm(point), txRange = norm(fromTx);
    if (!(rxRange > 1e-6) || !(txRange > 1e-6)) return null;
    const derivative = point.map((value, index) => value / rxRange + fromTx[index] / txRange);
    const residual = excessPath3d(point, measurement.transmitter) - measurement.excessKm;
    const weight = 1 / measurement.sigmaKm ** 2;
    score += residual * residual * weight;
    for (let row = 0; row < 3; row += 1) {
      gradient[row] += derivative[row] * residual * weight;
      for (let column = 0; column < 3; column += 1) matrix[row][column] += derivative[row] * derivative[column] * weight;
    }
  }
  return {matrix, gradient, score};
}

function eigenvalues3(matrix) {
  const copy = matrix.map(row => [...row]);
  for (let iteration = 0; iteration < 30; iteration += 1) {
    let p = 0, q = 1;
    for (const [row, column] of [[0, 2], [1, 2]]) if (Math.abs(copy[row][column]) > Math.abs(copy[p][q])) { p = row; q = column; }
    if (Math.abs(copy[p][q]) < 1e-12) break;
    const theta = .5 * Math.atan2(2 * copy[p][q], copy[q][q] - copy[p][p]);
    const c = Math.cos(theta), s = Math.sin(theta), pp = copy[p][p], qq = copy[q][q], pq = copy[p][q];
    copy[p][p] = c * c * pp - 2 * s * c * pq + s * s * qq;
    copy[q][q] = s * s * pp + 2 * s * c * pq + c * c * qq;
    copy[p][q] = copy[q][p] = 0;
    for (let index = 0; index < 3; index += 1) if (index !== p && index !== q) {
      const ip = copy[index][p], iq = copy[index][q];
      copy[index][p] = copy[p][index] = c * ip - s * iq;
      copy[index][q] = copy[q][index] = s * ip + c * iq;
    }
  }
  return [copy[0][0], copy[1][1], copy[2][2]].sort((a, b) => a - b);
}

function refinePoint(seed, lanes, acquisitionGateSigma = 3) {
  let point = seed;
  for (let iteration = 0; iteration < 16; iteration += 1) {
    const selected = selectMeasurements(point, lanes, acquisitionGateSigma);
    if (independentMeasurements(selected).length < 3) return null;
    const info = information(point, selected), inverse = info && inverse3(info.matrix);
    if (!inverse) return {point, selected, info, uncertainty: null};
    const step = inverse.matrix.map(row => -dot(row, info.gradient));
    if (!step.every(finite) || norm(step) > 50) return null;
    if (norm(step) < 1e-7) break;
    let next = null;
    for (let scale = 1; scale >= 1 / 128; scale /= 2) {
      const candidate = point.map((value, index) => value + step[index] * scale);
      const trial = information(candidate, selected);
      if (trial && trial.score <= info.score + 1e-12) { next = candidate; break; }
    }
    if (!next) break;
    point = next;
  }
  const selected = selectMeasurements(point, lanes), support = independentMeasurements(selected).length;
  if (support < 3) return null;
  const info = information(point, selected), inverse = info && inverse3(info.matrix);
  if (!inverse) return {point, selected, info, uncertainty: null};
  const eigenvalues = eigenvalues3(info.matrix), inflation = Math.max(1, info.score / Math.max(1, support - 3));
  if (!(eigenvalues[0] > 0)) return {point, selected, info, uncertainty: null};
  const covariance = inverse.matrix.map(row => row.map(value => value * inflation));
  const uncertainty = {kind: 'estimated', calibrated: false,
    method: 'linearized-range-jacobian-with-quantization-and-assumed-noise',
    covariance_enu_km2: covariance, horizontal_sigma_km: Math.sqrt(covariance[0][0] + covariance[1][1]),
    vertical_sigma_km: Math.sqrt(covariance[2][2]),
    jacobian_condition: Math.sqrt(eigenvalues[2] / eigenvalues[0]),
    independent_range_count: support, residual_degrees_of_freedom: Math.max(0, support - 3),
    residual_variance_inflation: inflation,
    range_sigma_km_by_site: independentMeasurements(selected).map(item => ({
      transmitter_site: item.measurement.transmitterSite, sigma_km: item.measurement.sigmaKm}))};
  return {point, selected, info, uncertainty};
}

function fuseTrackCohort(frames, config, requestedLimits = {}) {
  const limits = Object.fromEntries(Object.entries(DEFAULT_LIMITS).map(([key, maximum]) =>
    [key, Number.isSafeInteger(requestedLimits[key]) && requestedLimits[key] > 0 ?
      Math.min(maximum, requestedLimits[key]) : maximum]));
  const geometry = {...DEFAULT_GEOMETRY};
  const configuredGeometry = config?.process?.multi_illuminator?.localization || {};
  for (const key of Object.keys(geometry)) if (numeric(configuredGeometry[key]) &&
      (key === 'minimumAltitudeM' || Number(configuredGeometry[key]) > 0)) geometry[key] = Number(configuredGeometry[key]);
  // The separation floor prevents coordinate rounding from becoming geometry.
  const configuredRadiusM = Number(config?.process?.multi_illuminator?.co_location_radius_m);
  geometry.siteSeparationKm = finite(configuredRadiusM) && configuredRadiusM >= 0 ?
    configuredRadiusM / 1000 : Math.max(.1, geometry.siteSeparationKm);
  const groupingMode = config?.process?.multi_illuminator?.transmitter_site_grouping || 'automatic';
  const cohort = cohortIdentity(frames), receiver = config?.location?.rx;
  const enabled = (config?.process?.multi_illuminator?.illuminators || []).filter(item => item?.enabled === true);
  const definitions = new Map(enabled.map(item => [item.id, item]));
  if (!cohort || !validLocation(receiver) || definitions.size < 2 || definitions.size !== enabled.length ||
      frames.some(frame => !definitions.has(frame.id)))
    return {available: false, operational_localization: false,
      reason: 'Aligned capture epoch, timestamps and valid receiver/transmitter geometry are required.', hypotheses: []};
  const sites = siteCatalog(enabled, receiver, geometry.siteSeparationKm, groupingMode);
  const lanes = frames.map(frame => ({frame, measurements:
    laneMeasurements(frame, definitions.get(frame.id), receiver, limits.measurementsPerLane,
      sites.get(frame.id), geometry)})).filter(lane => lane.measurements.length);
  const groups = geometryGroups(lanes), hypotheses = [], siteCount = groups.length;
  // Truncating measurements can hide another branch; bounded work may only
  // produce diagnostics when the complete eligible search was not examined.
  let truncated = frames.some(frame => (frame.tracks?.data || []).filter(track =>
    operationalStates.has(String(track?.state || '').toUpperCase())).length > limits.measurementsPerLane);
  let tripleEvaluations = 0;
  tripleLoop: for (let left = 0; left < groups.length; left += 1) for (let middle = left + 1; middle < groups.length; middle += 1)
    for (let right = middle + 1; right < groups.length; right += 1)
      for (const first of groups[left].measurements) for (const second of groups[middle].measurements)
        for (const third of groups[right].measurements) {
          if (tripleEvaluations >= limits.pairEvaluations || hypotheses.length >= limits.hypotheses) {
            truncated = true; break tripleLoop;
          }
          tripleEvaluations += 1;
          // Preserve the seed's solution under the final measurement gate
          // before trying wider acquisition. A fourth site's residual may
          // be outside 3 sigma but inside 12 sigma: letting it move a valid
          // three-site mirror must never erase that competing branch.
          for (const seed of tripleRoots([first, second, third])) for (const acquisitionGateSigma of [3, 12]) {
            if (norm(seed) > geometry.maximumReceiverRangeKm * 2) continue;
            const refined = refinePoint(seed, groups, acquisitionGateSigma);
            if (!refined) continue;
            const {point, selected, info, uncertainty} = refined;
            const geographic = geographicPoint3d(receiver, point);
            if (uncertainty) {
              const normal = basis(geographic)[2], altitudeGradient = basis(receiver).map(axis => dot(axis, normal));
              uncertainty.altitude_sigma_m = 1000 * Math.sqrt(dot(altitudeGradient,
                uncertainty.covariance_enu_km2.map(row => dot(row, altitudeGradient))));
            }
            const contributors = selected.map(({measurement, residualKm}) => ({illuminator: measurement.lane,
              track_id: measurement.trackId, state: measurement.state, doppler_hz: measurement.dopplerHz,
              carrier_frequency_hz: measurement.carrierHz, sample_rate_hz: measurement.sampleRateHz,
              bistatic_range_rate_km_s: measurement.rangeRateKmS,
              transmitter_site: measurement.transmitterSite, excess_path_km: measurement.excessKm,
              physical_delay_definition: 'excess-bistatic-path-km',
              range_rate_definition: 'time-derivative-of-excess-bistatic-path-km-per-second',
              residual_km: Math.abs(residualKm), range_sigma_km: measurement.sigmaKm,
              provenance: measurement.provenance, source_false_alarm: measurement.falseAlarm}));
            const support = new Set(contributors.map(item => item.transmitter_site)).size;
            const signature = contributors.map(item => `${item.illuminator}:${item.track_id}`).sort().join('|');
            const bounded = uncertainty && [uncertainty.horizontal_sigma_km, uncertainty.vertical_sigma_km,
              uncertainty.jacobian_condition].every(finite) && uncertainty.horizontal_sigma_km <= geometry.maximumHorizontalSigmaKm &&
              uncertainty.vertical_sigma_km <= geometry.maximumVerticalSigmaKm && uncertainty.jacobian_condition <= geometry.maximumCondition;
            const admissible = geographic.altitude >= geometry.minimumAltitudeM &&
              geographic.altitude <= geometry.maximumAltitudeM && norm(point) <= geometry.maximumReceiverRangeKm;
            const altitudeMarginM = uncertainty ? uncertainty.altitude_sigma_m * 3 : Infinity;
            const rangeMarginKm = uncertainty ? 3 * Math.hypot(uncertainty.horizontal_sigma_km, uncertainty.vertical_sigma_km) : Infinity;
            const mayBeAdmissible = geographic.altitude + altitudeMarginM >= geometry.minimumAltitudeM &&
              geographic.altitude - altitudeMarginM <= geometry.maximumAltitudeM && norm(point) - rangeMarginKm <= geometry.maximumReceiverRangeKm;
            const reasons = [];
            if (!admissible) reasons.push('outside-aircraft-envelope');
            if (!bounded) reasons.push('ill-conditioned-or-unbounded-uncertainty');
            if (!cohort.capture_epoch_verified) reasons.push('unverified-capture-epoch');
            const candidate = {...geographic, local_east_km: point[0], local_north_km: point[1], local_up_km: point[2],
              contributors, support, geometric_support: support, lane_support: contributors.length,
              normalized_range_score: info?.score ?? null, altitude_model: 'three-dimensional-wgs84-ellipsoid',
              signature, uncertainty, physically_admissible: admissible, uncertainty_overlaps_aircraft_envelope: mayBeAdmissible,
              diagnostic_reasons: reasons, resolved: false};
            const duplicate = hypotheses.find(existing => existing.signature === signature &&
              norm(sub([existing.local_east_km, existing.local_north_km, existing.local_up_km], point)) < 1e-4);
            if (!duplicate) {
              if (hypotheses.length >= limits.hypotheses) { truncated = true; break tripleLoop; }
              hypotheses.push(candidate);
            }
            else if (candidate.normalized_range_score < duplicate.normalized_range_score) Object.assign(duplicate, candidate);
          }
        }
  for (const item of hypotheses) {
    const contributorKeys = new Set(item.contributors.map(value => `${value.illuminator}:${value.track_id}`));
    const competing = hypotheses.filter(other => other !== item && other.uncertainty_overlaps_aircraft_envelope &&
      other.contributors.some(value => contributorKeys.has(`${value.illuminator}:${value.track_id}`)));
    item.branch_count = 1 + competing.length;
    if (competing.length) item.diagnostic_reasons.push('mutually-exclusive-contributor-branches');
    if (truncated) item.diagnostic_reasons.push('incomplete-bounded-search');
    item.resolved = item.diagnostic_reasons.length === 0;
  }
  hypotheses.sort((left, right) => Number(right.resolved) - Number(left.resolved) ||
    Number(right.physically_admissible) - Number(left.physically_admissible) || right.support - left.support ||
    left.normalized_range_score - right.normalized_range_score);
  return {schema: 'vectorwarp-multi-illuminator-track-fusion-v1', available: true,
    ...cohort, lanes: frames.map(frame => frame.id), independent_transmitter_sites: siteCount,
    definition: 'Independent bistatic ranges solved in three dimensions using receiver and transmitter WGS84 coordinates.',
    adsb_used_by_solver: false, operational_localization: hypotheses.some(item => item.resolved),
    localization_limit: 'Uniqueness is conditional on the configured aircraft envelope and estimated range-noise model; uncertainty is not field-calibrated.',
    diagnostic_reasons: [...(siteCount < 3 ? ['fewer-than-three-independent-transmitter-sites'] : []),
      ...(hypotheses.length ? [] : ['no-admissible-well-conditioned-three-dimensional-solution']),
      ...(!cohort.capture_epoch_verified ? ['unverified-capture-epoch'] : []),
      ...(sites.size !== enabled.length ? ['missing-transmitter-altitude-or-coordinates'] : []),
      ...([...sites.values()].some(site => site.invalid) ? ['inconsistent-transmitter-site-identity'] : []),
      ...(truncated ? ['incomplete-bounded-search'] : [])],
    aircraft_envelope: {minimum_altitude_m: geometry.minimumAltitudeM, maximum_altitude_m: geometry.maximumAltitudeM,
      maximum_receiver_range_km: geometry.maximumReceiverRangeKm, altitude_datum: 'WGS84-ellipsoid'},
    uncertainty_model: {calibrated: false, assumed_range_noise_sigma_km: geometry.rangeNoiseSigmaKm,
      quantization: 'uniform within one delay bin', colocated_lanes_counted_once: true},
    measurement_semantics: {canonical_delay: 'excess-bistatic-path-km',
      canonical_range_rate: 'time-derivative-of-excess-bistatic-path-km-per-second',
      transmitter_site_grouping: groupingMode,
      automatic_co_location_radius_m: geometry.siteSeparationKm * 1000,
      explicit_site_ids_authoritative: true,
      geometry_group_vote_limit_per_cpi: 1,
      cross_frequency_combination: 'alternative-observations-not-independent-votes',
      false_alarm: 'source-lane metadata preserved; probabilities are not multiplied or treated as independent'},
    computation: {pair_evaluations: tripleEvaluations, triple_evaluations: tripleEvaluations,
      measurement_limit_per_illuminator: limits.measurementsPerLane, hypothesis_limit: limits.hypotheses, truncated}, hypotheses};
}

function validateFusionAgainstAdsb(result, value, {maximumErrorKm = 5} = {}) {
  const aircraft = (Array.isArray(value?.aircraft) ? value.aircraft :
    Object.entries(value || {}).filter(([id]) => id !== 'now' && id !== 'vectorwarp_fusion')
      .map(([hex, item]) => ({hex, ...item})))
    .map(item => ({hex: item?.hex || null, flight: item?.flight || '',
      latitude: Number(item?.latitude ?? item?.lat),
      longitude: Number(item?.longitude ?? item?.lon),
      altitude: numeric(item?.alt_geom) ? Number(item.alt_geom) * .3048 :
        numeric(item?.altitude) ? Number(item.altitude) : null}))
    .filter(item => [item.latitude, item.longitude].every(finite));
  const operational = (result?.tracks || []).filter(item =>
    ['FUSED_ACTIVE', 'FUSED_COASTING'].includes(String(item?.state || '').toUpperCase()));
  const candidates = operational.length ? operational : (result?.hypotheses || []);
  const pairs = [];
  for (let candidateIndex = 0; candidateIndex < candidates.length; candidateIndex += 1) {
    const candidate = candidates[candidateIndex];
    if (![candidate?.latitude, candidate?.longitude].every(finite)) continue;
    for (let aircraftIndex = 0; aircraftIndex < aircraft.length; aircraftIndex += 1)
      pairs.push({candidateIndex, aircraftIndex,
        errorKm: inverse(candidate, aircraft[aircraftIndex]).distanceM / 1000});
  }
  pairs.sort((left, right) => left.errorKm - right.errorKm);
  const usedCandidates = new Set(), usedAircraft = new Set(), matches = [];
  for (const pair of pairs) {
    if (pair.errorKm > maximumErrorKm || usedCandidates.has(pair.candidateIndex) ||
        usedAircraft.has(pair.aircraftIndex)) continue;
    usedCandidates.add(pair.candidateIndex); usedAircraft.add(pair.aircraftIndex);
    const candidate = candidates[pair.candidateIndex], truth = aircraft[pair.aircraftIndex];
    matches.push({candidate_id: candidate.id || null, candidate_index: pair.candidateIndex,
      support: Number(candidate.support) || 0, state: candidate.state || null,
      adsb_hex: truth.hex, adsb_flight: truth.flight, error_km: pair.errorKm,
      altitude_error_m: finite(candidate.altitude) && finite(truth.altitude) ?
        Math.abs(candidate.altitude - truth.altitude) : null});
  }
  const errors = matches.map(item => item.error_km).sort((a, b) => a - b);
  const percentile = fraction => errors.length ? errors[Math.min(errors.length - 1,
    Math.floor((errors.length - 1) * fraction))] : null;
  return {schema: 'vectorwarp-multi-illuminator-fusion-adsb-validation-v1',
    held_out_truth: true, adsb_used_by_solver: false,
    timestamp: result?.timestamp ?? null,
    target_kind: operational.length ? 'operational-fused-tracks' : 'fusion-hypotheses',
    candidate_count: candidates.length, adsb_aircraft_count: aircraft.length,
    maximum_match_error_km: maximumErrorKm, matched_candidates: matches.length,
    unmatched_candidates: Math.max(0, candidates.length - matches.length),
    error_km: {minimum: errors[0] ?? null, median: percentile(.5),
      p90: percentile(.9), maximum: errors.at(-1) ?? null}, matches};
}

const DEFAULT_CROSS_FREQUENCY_SUPPORT = Object.freeze({maximumCoastSeconds: 10,
  maxTracks: 256, maxMeasurementsPerSite: 64});

function rawDetectionMeasurements(frame, definition, site) {
  const sampleRate = Number(frame?.sample_rate_hz), carrierHz = Number(frame?.frequency_hz);
  if (!site || site.invalid || !finite(sampleRate) || !(sampleRate > 0) ||
      !finite(carrierHz) || !(carrierHz > 0) ||
      sampleRate !== Number(definition?.output_sample_rate_hz) ||
      carrierHz !== Number(definition?.frequency_hz)) return null;
  const delay = frame?.detections?.delay, doppler = frame?.detections?.doppler;
  const snr = frame?.detections?.snr;
  if (!numeric(frame?.detections?.timestamp) ||
      Number(frame.detections.timestamp) !== Number(frame.timestamp) ||
      ![delay, doppler, snr].every(Array.isArray) || delay.length !== doppler.length ||
      delay.length !== snr.length) return null;
  const output = [];
  for (let index = 0; index < delay.length; index += 1) {
    if (![delay[index], doppler[index], snr[index]].every(numeric) || Number(delay[index]) <= 0)
      return null;
    const sourceFalseAlarm = Array.isArray(frame.detections.false_alarm) ?
      frame.detections.false_alarm[index] : frame.detections.false_alarm ??
      frame.detections.false_alarm_probability ?? frame.detections.pfa ?? null;
    output.push({site: site.identity, lane: frame.id, detectionIndex: index,
      excessKm: Number(delay[index]), dopplerHz: Number(doppler[index]), snrDb: Number(snr[index]),
      rangeRateKmS: -Number(doppler[index]) * C_KM_S / carrierHz,
      carrierHz, sampleRateHz: sampleRate,
      // Native multi-lane detection JSON is serialized to two decimal places;
      // retain at least that 10 m quantization even when a very high sample
      // rate makes one delay bin narrower.
      delayResolutionKm: Math.max(C_KM_S / sampleRate, .01),
      provenance: {illuminator: frame.id, detection_index: index,
        capture_epoch: frame.capture_epoch,
        capture_first_input_sample: frame.capture_first_input_sample,
        capture_end_input_sample: frame.capture_end_input_sample,
        frame: clone(frame?.provenance) ?? null,
        detection: Array.isArray(frame.detections.provenance) ?
          clone(frame.detections.provenance[index]) ?? null :
          clone(frame.detections.provenance) ?? null},
      falseAlarm: clone(sourceFalseAlarm)});
  }
  return output;
}

// This tracker intentionally has no receiver/transmitter geographic solver.
// Its state is meaningful only inside one physical transmitter geometry and
// exists to show recurring cross-carrier evidence below per-lane track
// promotion. Separate sites can interact only in fuseTrackCohort's ellipsoid
// hypotheses after their own lane trackers become operational.
class CrossFrequencySupportTracker {
  constructor(config, options = {}) {
    this.config = config;
    this.options = {...DEFAULT_CROSS_FREQUENCY_SUPPORT};
    for (const [key, maximum] of Object.entries(this.options)) if (numeric(options[key]) && Number(options[key]) > 0)
      this.options[key] = Math.min(maximum, Number(options[key]));
    this.options.maxTracks = Math.max(1, Math.floor(this.options.maxTracks));
    this.options.maxMeasurementsPerSite = Math.max(1,
      Math.floor(this.options.maxMeasurementsPerSite));
    this.delayGateOverrideKm = numeric(options.delayGateKm) && Number(options.delayGateKm) > 0 ?
      Number(options.delayGateKm) : null;
    this.rangeRateGateOverrideKmS = numeric(options.rangeRateGateKmS) &&
      Number(options.rangeRateGateKmS) > 0 ? Number(options.rangeRateGateKmS) : null;
    this.rangeAccelerationOverrideKmS2 = numeric(options.maximumRangeAccelerationKmS2) &&
      Number(options.maximumRangeAccelerationKmS2) >= 0 ?
      Number(options.maximumRangeAccelerationKmS2) : null;
    this.cpiSeconds = Number(config?.process?.data?.cpi);
    this.maximumAccelerationHzS = Number(config?.process?.tracker?.initiate?.maxAcc);
    this.m = Math.min(255, Math.max(2, Math.floor(Number(options.m) ||
      Number(config?.process?.tracker?.initiate?.M) || 3)));
    this.n = Math.min(255, Math.max(this.m, Math.floor(Number(options.n) ||
      Number(config?.process?.tracker?.initiate?.N) || 6)));
    this.deleteAfter = Math.max(1, Math.floor(Number(options.deleteAfter) ||
      Number(config?.process?.tracker?.delete) || 3));
    this.tracks = new Map(); this.nextId = 1; this.lastKey = null;
    this.lastTimestamp = null; this.lastEpoch = null; this.lastCaptureEnd = null;
    this.seenCpis = new Set(); this.lastResult = null; this.lastReceiptAt = null;
  }
  invalid(reason, cohort = null) {
    return {schema: 'vectorwarp-cross-frequency-support-v1', available: false,
      diagnostic_only: true, geographic_claim: false, operational_track_claim: false,
      reason, timestamp: cohort?.timestamp ?? null, tracks: []};
  }
  publicTrack(track, timestamp) {
    const lanes = [...new Set(track.laneHistory.flat())].sort();
    const carriers = new Set(track.carrierHistory.flat());
    const hitCount = track.hits.filter(Boolean).length;
    const crossFrequency = carriers.size >= 2;
    return {id: track.id, state: track.confirmed ?
      (track.misses ? 'SUPPORT_COASTING' : 'SUPPORT_ACTIVE') : 'SUPPORT_CANDIDATE',
      diagnostic_only: true, geographic_claim: false, transmitter_site: track.site,
      timestamp, measurement_timestamp: track.timestamp,
      excess_path_km: track.excessKm, bistatic_range_rate_km_s: track.rangeRateKmS,
      physical_delay_definition: 'excess-bistatic-path-km',
      range_rate_definition: 'time-derivative-of-excess-bistatic-path-km-per-second',
      supporting_cpis: hitCount, required_supporting_cpis: this.m,
      distinct_carriers: carriers.size, distinct_lanes: lanes,
      cross_frequency_supported: crossFrequency, misses: track.misses,
      vote_count_this_cpi: track.misses ? 0 : 1,
      contributors: track.misses ? [] : track.contributors};
  }
  update(frames) {
    const cohort = cohortIdentity(frames);
    if (!cohort || !cohort.capture_epoch_verified ||
        !Number.isSafeInteger(cohort.capture_epoch) || cohort.capture_epoch < 0)
      return this.invalid('Verified aligned capture-CPI provenance is required.', cohort);
    const enabled = (this.config?.process?.multi_illuminator?.illuminators || [])
      .filter(item => item?.enabled === true);
    const definitions = new Map(enabled.map(item => [item.id, item]));
    const receiver = this.config?.location?.rx;
    if (!validLocation(receiver) || !finite(this.cpiSeconds) || !(this.cpiSeconds > 0) ||
        !finite(this.maximumAccelerationHzS) || this.maximumAccelerationHzS < 0 ||
        frames.some(frame => !definitions.has(frame.id)))
      return this.invalid('Receiver or illuminator provenance is invalid.', cohort);
    const configuredRadiusM = Number(this.config?.process?.multi_illuminator?.co_location_radius_m);
    const configuredSeparation = Number(this.config?.process?.multi_illuminator?.localization?.siteSeparationKm);
    const siteSeparationKm = finite(configuredRadiusM) && configuredRadiusM >= 0 ? configuredRadiusM / 1000 :
      finite(configuredSeparation) && configuredSeparation > 0 ? configuredSeparation : DEFAULT_GEOMETRY.siteSeparationKm;
    const groupingMode = this.config?.process?.multi_illuminator?.transmitter_site_grouping || 'automatic';
    const sites = siteCatalog(enabled, receiver, siteSeparationKm, groupingMode);
    const measurements = [];
    for (const frame of frames) {
      const rows = rawDetectionMeasurements(frame, definitions.get(frame.id), sites.get(frame.id));
      if (!rows) return this.invalid('Detection units, carrier, sample rate, or transmitter-site provenance is invalid.', cohort);
      measurements.push(...rows);
    }
    const cpiKey = `${typeof cohort.capture_epoch}:${cohort.capture_epoch}:` +
      `${cohort.capture_first_input_sample}:${cohort.capture_end_input_sample}`;
    if (cpiKey === this.lastKey || this.seenCpis.has(cpiKey))
      return this.lastResult || this.invalid('Capture CPI was already consumed.', cohort);
    if (this.lastTimestamp !== null && cohort.capture_epoch === this.lastEpoch &&
        (cohort.timestamp <= this.lastTimestamp ||
         cohort.capture_first_input_sample < this.lastCaptureEnd))
      return this.invalid('Out-of-order or replayed capture CPI was rejected.', cohort);
    if (this.lastEpoch !== null && cohort.capture_epoch !== this.lastEpoch) {
      if (cohort.capture_epoch <= this.lastEpoch)
        return this.invalid('A previously consumed capture epoch cannot be replayed.', cohort);
      this.tracks.clear(); this.seenCpis.clear(); this.lastTimestamp = null;
      this.lastCaptureEnd = null;
    }
    const bySite = new Map();
    for (const measurement of measurements) {
      if (!bySite.has(measurement.site)) bySite.set(measurement.site, []);
      bySite.get(measurement.site).push(measurement);
    }
    let truncated = false;
    const observations = [];
    for (const [site, rows] of bySite) {
      rows.sort((left, right) => right.snrDb - left.snrDb || left.excessKm - right.excessKm ||
        left.rangeRateKmS - right.rangeRateKmS || left.lane.localeCompare(right.lane) ||
        left.detectionIndex - right.detectionIndex);
      if (rows.length > this.options.maxMeasurementsPerSite) truncated = true;
      const bounded = rows.slice(0, this.options.maxMeasurementsPerSite);
      const clusters = [];
      for (const row of bounded) {
        row.rangeRateResolutionKmS = C_KM_S / row.carrierHz / this.cpiSeconds;
        row.maximumRangeAccelerationKmS2 = this.rangeAccelerationOverrideKmS2 ??
          C_KM_S / row.carrierHz * this.maximumAccelerationHzS;
        const existing = clusters.find(cluster => {
          const representative = cluster.representative;
          const delayGate = this.delayGateOverrideKm ??
            Math.max(representative.delayResolutionKm, row.delayResolutionKm);
          const rateGate = this.rangeRateGateOverrideKmS ??
            Math.max(representative.rangeRateResolutionKmS, row.rangeRateResolutionKmS);
          return Math.abs(representative.excessKm - row.excessKm) <= delayGate &&
            Math.abs(representative.rangeRateKmS - row.rangeRateKmS) <= rateGate;
        });
        if (existing) existing.sources.push(row);
        else clusters.push({site, representative: row, sources: [row]});
      }
      observations.push(...clusters);
    }
    const pairs = [];
    for (const track of this.tracks.values()) for (let index = 0; index < observations.length; index += 1) {
      const observation = observations[index];
      if (track.site !== observation.site) continue;
      const dt = (cohort.timestamp - track.timestamp) / 1000;
      if (!(dt > 0) || dt > this.options.maximumCoastSeconds) continue;
      const predicted = track.excessKm + track.rangeRateKmS * dt;
      const baseDelayGate = this.delayGateOverrideKm ??
        Math.max(track.delayResolutionKm, observation.representative.delayResolutionKm);
      const baseRateGate = this.rangeRateGateOverrideKmS ??
        Math.max(track.rangeRateResolutionKmS, observation.representative.rangeRateResolutionKmS);
      const acceleration = this.rangeAccelerationOverrideKmS2 ??
        Math.max(track.maximumRangeAccelerationKmS2,
          observation.representative.maximumRangeAccelerationKmS2);
      const delayGate = baseDelayGate + .5 * acceleration * dt * dt;
      const rateGate = baseRateGate + acceleration * dt;
      const delayResidual = Math.abs(observation.representative.excessKm - predicted);
      const rateResidual = Math.abs(observation.representative.rangeRateKmS - track.rangeRateKmS);
      if (delayResidual <= delayGate && rateResidual <= rateGate)
        pairs.push({track, index, observation, score: delayResidual / delayGate + rateResidual / rateGate});
    }
    pairs.sort((left, right) => left.score - right.score || left.track.id.localeCompare(right.track.id) ||
      left.observation.representative.lane.localeCompare(right.observation.representative.lane));
    const usedTracks = new Set(), usedObservations = new Set();
    const contributors = observation => observation.sources.map(source => ({illuminator: source.lane,
      detection_index: source.detectionIndex, carrier_frequency_hz: source.carrierHz,
      sample_rate_hz: source.sampleRateHz, delay_km: source.excessKm,
      doppler_hz: source.dopplerHz, bistatic_range_rate_km_s: source.rangeRateKmS,
      snr_db: source.snrDb, provenance: source.provenance,
      source_false_alarm: source.falseAlarm}));
    for (const pair of pairs) {
      if (usedTracks.has(pair.track.id) || usedObservations.has(pair.index)) continue;
      usedTracks.add(pair.track.id); usedObservations.add(pair.index);
      const source = pair.observation.representative;
      pair.track.excessKm = source.excessKm; pair.track.rangeRateKmS = source.rangeRateKmS;
      pair.track.delayResolutionKm = source.delayResolutionKm;
      pair.track.rangeRateResolutionKmS = source.rangeRateResolutionKmS;
      pair.track.maximumRangeAccelerationKmS2 = source.maximumRangeAccelerationKmS2;
      pair.track.timestamp = cohort.timestamp; pair.track.misses = 0;
      pair.track.hits.push(true); pair.track.hits = pair.track.hits.slice(-this.n);
      pair.track.laneHistory.push([...new Set(pair.observation.sources.map(item => item.lane))]);
      pair.track.carrierHistory.push([...new Set(pair.observation.sources.map(item => item.carrierHz))]);
      pair.track.laneHistory = pair.track.laneHistory.slice(-this.n);
      pair.track.carrierHistory = pair.track.carrierHistory.slice(-this.n);
      if (pair.track.hits.filter(Boolean).length >= this.m &&
          new Set(pair.track.carrierHistory.flat()).size >= 2) pair.track.confirmed = true;
      pair.track.contributors = contributors(pair.observation);
    }
    for (const track of this.tracks.values()) if (!usedTracks.has(track.id)) {
      track.hits.push(false); track.hits = track.hits.slice(-this.n); track.misses += 1;
      track.laneHistory.push([]); track.laneHistory = track.laneHistory.slice(-this.n);
      track.carrierHistory.push([]); track.carrierHistory = track.carrierHistory.slice(-this.n);
      track.contributors = [];
    }
    for (let index = 0; index < observations.length; index += 1) if (!usedObservations.has(index)) {
      if (this.tracks.size >= this.options.maxTracks) { truncated = true; continue; }
      const observation = observations[index], source = observation.representative;
      const id = `X${String(this.nextId++).padStart(4, '0')}`;
      this.tracks.set(id, {id, site: observation.site, excessKm: source.excessKm,
        rangeRateKmS: source.rangeRateKmS, delayResolutionKm: source.delayResolutionKm,
        rangeRateResolutionKmS: source.rangeRateResolutionKmS,
        maximumRangeAccelerationKmS2: source.maximumRangeAccelerationKmS2,
        timestamp: cohort.timestamp,
        hits: [true], misses: 0, confirmed: false,
        laneHistory: [[...new Set(observation.sources.map(item => item.lane))]],
        carrierHistory: [[...new Set(observation.sources.map(item => item.carrierHz))]],
        contributors: contributors(observation)});
    }
    for (const [id, track] of this.tracks) if (track.misses > this.deleteAfter ||
        (cohort.timestamp - track.timestamp) / 1000 > this.options.maximumCoastSeconds)
      this.tracks.delete(id);
    this.lastKey = cpiKey; this.seenCpis.add(cpiKey);
    while (this.seenCpis.size > 128) this.seenCpis.delete(this.seenCpis.values().next().value);
    this.lastTimestamp = cohort.timestamp; this.lastEpoch = cohort.capture_epoch;
    this.lastCaptureEnd = cohort.capture_end_input_sample; this.lastReceiptAt = Date.now();
    const tracks = [...this.tracks.values()].map(track => this.publicTrack(track, cohort.timestamp))
      .sort((left, right) => left.id.localeCompare(right.id));
    this.lastResult = {schema: 'vectorwarp-cross-frequency-support-v1', available: true,
      diagnostic_only: true, geographic_claim: false, operational_track_claim: false,
      ...cohort, definition: 'Same-transmitter cross-frequency detection persistence in canonical bistatic path coordinates.',
      measurement_semantics: {geometry_group_vote_limit_per_cpi: 1,
        false_alarm: 'source-lane metadata preserved; coincident lanes form one vote and probabilities are not combined',
        separate_transmitter_sites_raw_merged: false,
        default_gate_model: 'one-delay-bin-and-one-doppler-bin-plus-configured-tracker-acceleration',
        held_out_null_calibrated: false},
      computation: {measurement_limit_per_site: this.options.maxMeasurementsPerSite,
        track_limit: this.options.maxTracks, truncated}, tracks};
    return this.lastResult;
  }
  snapshot(now = Date.now()) {
    if (!this.lastResult) return null;
    const ageSeconds = this.lastReceiptAt === null ? Infinity :
      Math.max(0, (now - this.lastReceiptAt) / 1000);
    if (ageSeconds <= this.options.maximumCoastSeconds) return this.lastResult;
    return {...this.lastResult, available: false,
      reason: 'Cross-frequency detection input stopped; diagnostic support expired.', tracks: []};
  }
}

class MultiIlluminatorTrackFuser {
  constructor(config, {maxSpeedMps = 450, maxAccelerationMps2 = 80, maximumCoastSeconds = 10} = {}) {
    this.config = config; this.maxSpeedKmS = maxSpeedMps / 1000;
    this.maxAccelerationKmS2 = maxAccelerationMps2 / 1000;
    this.maximumCoastSeconds = maximumCoastSeconds;
    this.m = Math.max(2, Number(config?.process?.tracker?.initiate?.M) || 3);
    this.n = Math.max(this.m, Number(config?.process?.tracker?.initiate?.N) || 6);
    this.deleteAfter = Math.max(1, Number(config?.process?.tracker?.delete) || 3);
    this.tracks = new Map(); this.nextId = 1; this.lastKey = null; this.lastResult = null;
    this.seenEpochs = new Set();
    this.seenCaptureIntervals = new Set(); this.lastCaptureEnd = null;
    this.lastReceiptAt = null;
    this.crossFrequencySupport = new CrossFrequencySupportTracker(config);
  }
  snapshot(now = Date.now()) {
    if (!this.lastResult) return null;
    const crossFrequencySupport = this.crossFrequencySupport.snapshot(now) ||
      this.lastResult.cross_frequency_support;
    const ageSeconds = this.lastReceiptAt === null ? Infinity :
      Math.max(0, (now - this.lastReceiptAt) / 1000);
    const staleAfterSeconds = Math.max(.1,
      2 * (Number(this.config?.process?.data?.cpi) || .5));
    if (ageSeconds <= staleAfterSeconds) return {...this.lastResult,
      cross_frequency_support: crossFrequencySupport};
    if (ageSeconds > this.maximumCoastSeconds) return {...this.lastResult,
      available: false, operational_localization: false,
      reason: 'Fused input cohorts stopped; stale tracks expired.', tracks: [],
      cross_frequency_support: crossFrequencySupport,
      hypotheses: this.lastResult.hypotheses.map(item => ({...item, resolved: false,
        diagnostic_reasons: [...new Set([...(item.diagnostic_reasons || []), 'stale-input-cohort'])]}))};
    return {...this.lastResult, operational_localization: false,
      reason: 'Fused input cohorts are stale; last tracks are diagnostic coasts.',
      cross_frequency_support: crossFrequencySupport,
      tracks: this.lastResult.tracks.map(item => ({...item,
        state: item.state === 'FUSED_CANDIDATE' ? item.state : 'FUSED_COASTING',
        contributors: [], support: 0, geometric_support: 0,
        stale_input_seconds: ageSeconds}))};
  }
  update(frames) {
    const crossFrequencySupport = this.crossFrequencySupport.update(frames);
    const result = fuseTrackCohort(frames, this.config);
    if (!result.available || result.key === this.lastKey) return result.key === this.lastKey ?
      this.lastResult : {...result, tracks: [], cross_frequency_support: crossFrequencySupport};
    const timestamp = result.timestamp;
    if (this.lastResult && this.lastResult.capture_epoch !== result.capture_epoch) {
      if (this.seenEpochs.has(result.capture_epoch)) return this.lastResult;
      this.tracks.clear(); this.lastResult = null;
      this.seenCaptureIntervals.clear(); this.lastCaptureEnd = null;
    }
    if (this.lastResult && finite(this.lastResult.timestamp) &&
        timestamp <= this.lastResult.timestamp) return this.lastResult;
    const interval = `${result.capture_first_input_sample}:${result.capture_end_input_sample}`;
    // Raw capture evidence can vote only once, regardless of later presentation
    // timestamps. The end watermark also rejects old intervals after eviction
    // from the bounded ledger and prevents overlapping windows reusing samples.
    if (this.seenCaptureIntervals.has(interval) || this.lastCaptureEnd !== null &&
        result.capture_first_input_sample < this.lastCaptureEnd) return this.lastResult;
    this.seenCaptureIntervals.add(interval);
    if (this.seenCaptureIntervals.size > 128)
      this.seenCaptureIntervals.delete(this.seenCaptureIntervals.values().next().value);
    this.lastCaptureEnd = result.capture_end_input_sample;
    this.seenEpochs.add(result.capture_epoch);
    for (const [id, track] of this.tracks) if ((timestamp - track.timestamp) / 1000 > this.maximumCoastSeconds)
      this.tracks.delete(id);
    const hypotheses = result.hypotheses.map((item, index) => ({...item, index,
      diagnostic_reasons: [...item.diagnostic_reasons]}));
    const pairs = [];
    for (const track of this.tracks.values()) for (const hypothesis of hypotheses.filter(item => item.resolved)) {
      const dt = (timestamp - track.timestamp) / 1000;
      const point = [hypothesis.local_east_km, hypothesis.local_north_km, hypothesis.local_up_km];
      const velocity = sub(point, track.point).map(value => value / dt);
      const predicted = track.point.map((value, index) => value + track.velocity[index] * dt);
      // Each secant velocity belongs to its measurement interval's midpoint.
      // Unequal scan gaps therefore separate the two velocities by half the
      // sum of their intervals, not by the current interval alone.
      const velocitySeparation = (track.velocityIntervalSeconds + dt) / 2;
      const speed = norm(velocity), acceleration = track.velocityKnown ?
        norm(sub(velocity, track.velocity)) / velocitySeparation : 0;
      const motionValid = dt > 0 && speed <= this.maxSpeedKmS && acceleration <= this.maxAccelerationKmS2;
      const keys = new Set(track.hypothesis.contributors.map(item => `${item.illuminator}:${item.track_id}`));
      const sharedContributor = hypothesis.contributors.some(item => keys.has(`${item.illuminator}:${item.track_id}`));
      if (!motionValid && sharedContributor) {
        hypothesis.motion_rejected = true;
        if (!hypothesis.diagnostic_reasons.includes('physically-impossible-track-motion'))
          hypothesis.diagnostic_reasons.push('physically-impossible-track-motion');
      }
      if (motionValid) pairs.push({track, hypothesis, point, velocity, dt, distance: norm(sub(point, predicted))});
    }
    pairs.sort((left, right) => left.distance - right.distance);
    const usedTracks = new Set(), usedHypotheses = new Set();
    for (const pair of pairs) {
      if (pair.hypothesis.motion_rejected || usedTracks.has(pair.track.id) || usedHypotheses.has(pair.hypothesis.index)) continue;
      usedTracks.add(pair.track.id); usedHypotheses.add(pair.hypothesis.index);
      pair.track.velocity = pair.velocity; pair.track.velocityKnown = true;
      pair.track.velocityIntervalSeconds = pair.dt;
      pair.track.point = pair.point;
      pair.track.timestamp = timestamp; pair.track.misses = 0;
      pair.track.hits.push(true);
      pair.track.hits = pair.track.hits.slice(-this.n);
      pair.track.hypothesis = pair.hypothesis;
      if (pair.track.hits.filter(Boolean).length >= this.m) pair.track.confirmed = true;
    }
    for (const track of this.tracks.values()) if (!usedTracks.has(track.id)) {
      track.hits.push(false); track.hits = track.hits.slice(-this.n); track.misses += 1;
    }
    for (const hypothesis of hypotheses) if (hypothesis.resolved && !hypothesis.motion_rejected && !usedHypotheses.has(hypothesis.index)) {
      const id = `F${String(this.nextId++).padStart(4, '0')}`;
      this.tracks.set(id, {id, point: [hypothesis.local_east_km, hypothesis.local_north_km, hypothesis.local_up_km],
        velocity: [0, 0, 0], velocityKnown: false, velocityIntervalSeconds: null,
        timestamp, hits: [true], misses: 0,
        confirmed: false, hypothesis});
    }
    for (const [id, track] of this.tracks)
      if (track.misses > this.deleteAfter) this.tracks.delete(id);
    const receiver = this.config?.location?.rx;
    const tracks = [...this.tracks.values()].map(track => {
      const elapsed = (timestamp - track.timestamp) / 1000;
      const point = track.point.map((value, index) => value + track.velocity[index] * elapsed);
      const position = geographicPoint3d(receiver, point);
      // Relative to the predicted centre, a target bounded only by maximum
      // speed can travel in the opposite direction to the estimated velocity.
      const motionEnvelope = (this.maxSpeedKmS + norm(track.velocity)) * elapsed;
      const covariance = track.hypothesis.uncertainty.covariance_enu_km2.map((row, rowIndex) => row.map((value, columnIndex) =>
        value + (rowIndex === columnIndex ? motionEnvelope ** 2 : 0)));
      const normal = basis(position)[2], altitudeGradient = basis(receiver).map(axis => dot(axis, normal));
      const uncertainty = {...track.hypothesis.uncertainty,
        kind: track.misses ? 'propagated' : 'estimated', covariance_enu_km2: covariance,
        horizontal_sigma_km: Math.sqrt(covariance[0][0] + covariance[1][1]), vertical_sigma_km: Math.sqrt(covariance[2][2]),
        altitude_sigma_m: 1000 * Math.sqrt(dot(altitudeGradient, covariance.map(row => dot(row, altitudeGradient)))),
        propagation_seconds: elapsed, propagation_model: 'constant-velocity-with-maximum-speed-position-envelope',
        additional_position_envelope_km: motionEnvelope};
      return {id: track.id, ...position, local_east_km: point[0], local_north_km: point[1], local_up_km: point[2],
        timestamp, measurement_timestamp: track.timestamp,
        velocity_east_km_s: track.velocity[0], velocity_north_km_s: track.velocity[1], velocity_up_km_s: track.velocity[2],
        speed_m_s: norm(track.velocity) * 1000,
        state: track.confirmed ? (track.misses ? 'FUSED_COASTING' : 'FUSED_ACTIVE') :
          'FUSED_CANDIDATE',
        supporting_frames: track.hits.filter(Boolean).length,
        required_supporting_frames: this.m, misses: track.misses,
        contributors: track.misses ? [] : track.hypothesis.contributors,
        support: track.misses ? 0 : track.hypothesis.support,
        last_measured_support: track.hypothesis.support,
        geometric_support: track.misses ? 0 : track.hypothesis.support,
        altitude_model: 'three-dimensional-wgs84-ellipsoid', uncertainty};
    });
    this.lastKey = result.key;
    this.lastResult = {...result, operational_localization: tracks.some(item => item.state === 'FUSED_ACTIVE'),
      hypotheses: hypotheses.map(({index, ...item}) => ({...item, resolved: item.resolved && !item.motion_rejected})), tracks};
    this.lastResult.cross_frequency_support = crossFrequencySupport;
    this.lastReceiptAt = Date.now();
    return this.lastResult;
  }
}

module.exports = {localPoint, geographicPoint, excessPath, ellipseIntersections,
  localPoint3d, geographicPoint3d, excessPath3d, tripleRoots,
  cohortIdentity, siteCatalog, laneMeasurements, geometryGroups, fuseTrackCohort, validateFusionAgainstAdsb,
  rawDetectionMeasurements, CrossFrequencySupportTracker, MultiIlluminatorTrackFuser};
