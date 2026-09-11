'use strict';

// Pure configuration contract. Nothing in this module computes DoA or feeds a
// bearing to detection/tracking. Reported receiver counts are observations,
// never evidence of physical geometry or wiring. No bearing path is implemented.
const C = 299792458;
const ARRAY_CONTRACT = Object.freeze({
  channelCount: Object.freeze({min: 2, max: 8}),
  shapes: Object.freeze(['cross', 'regular_polygon', 'ula', 'custom']),
  units: 'm',
  coordinateFrame: 'array_xy_right_handed',
  ulaHalfPlanes: Object.freeze(['both', 'positive_y', 'negative_y']),
  trueBearingConvention: 'clockwise_from_true_north',
  maxLabelLength: 128,
  maxCoordinateMagnitudeM: Number.MAX_SAFE_INTEGER,
  measurementTolerance: Object.freeze({absoluteM: .001,
    crossRelative: .02, ulaCollinearRelative: .01, ulaSpacingRelative: .02,
    polygonRadiusRelative: .05, polygonAngleRad: .0873})
});

const finite = value => typeof value === 'number' && Number.isFinite(value);
const object = value => value && typeof value === 'object' && !Array.isArray(value);
const label = value => typeof value === 'string' && value.length > 0 &&
  value.length <= ARRAY_CONTRACT.maxLabelLength && value === value.trim() &&
  !/[\u0000-\u001f\u007f]/.test(value);
const coordinate = value => finite(value) &&
  Math.abs(value) <= ARRAY_CONTRACT.maxCoordinateMagnitudeM;
const position = value => Array.isArray(value) && value.length === 3 &&
  coordinate(value[0]) && coordinate(value[1]) && coordinate(value[2]);
const boundedPoints = points => Array.isArray(points) && points.length >= 2 &&
  points.length <= ARRAY_CONTRACT.channelCount.max &&
  Array.from({length: points.length}, (_, i) => position(points[i])).every(Boolean);
const distance = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
const midpoint = (a, b) => a.map((value, i) => (value + b[i]) / 2);
const vector = (a, b) => a.map((value, i) => value - b[i]);
const norm = value => Math.hypot(...value);
const dot = (a, b) => a.reduce((sum, value, i) => sum + value * b[i], 0);

function localAzimuthToTrueBearing(localDegrees, xAxisBearingDegreesTrue) {
  if (!finite(localDegrees) || !finite(xAxisBearingDegreesTrue)) return null;
  // Normalize first so two finite but very large inputs cannot overflow.
  return (((xAxisBearingDegreesTrue % 360) - (localDegrees % 360)) % 360 + 360) % 360;
}

function validateCross(points) {
  if (points.length !== 4) return false;
  const pairings = [[[0, 1], [2, 3]], [[0, 2], [1, 3]], [[0, 3], [1, 2]]];
  return pairings.some(([[a, b], [c, d]]) => {
    const first = vector(points[a], points[b]);
    const second = vector(points[c], points[d]);
    const scale = Math.max(norm(first), norm(second));
    if (scale === 0) return false;
    const tolerance = Math.max(ARRAY_CONTRACT.measurementTolerance.absoluteM,
      scale * ARRAY_CONTRACT.measurementTolerance.crossRelative);
    // The cross shorthand describes an array in the local XY plane. Preserve
    // a tilted measured aperture as custom instead of silently flattening it.
    if (Math.max(...points.map(point => point[2])) -
        Math.min(...points.map(point => point[2])) > tolerance) return false;
    const centresAgree = distance(midpoint(points[a], points[b]),
      midpoint(points[c], points[d])) <= tolerance;
    const perpendicular = Math.abs(dot(first, second)) <=
      norm(first) * norm(second) * ARRAY_CONTRACT.measurementTolerance.crossRelative;
    return centresAgree && perpendicular;
  });
}

function validateRegularPolygon(points) {
  if (points.length < 3) return false;
  const z = points.reduce((sum, point) => sum + point[2], 0) / points.length;
  const x = points.reduce((sum, point) => sum + point[0], 0) / points.length;
  const y = points.reduce((sum, point) => sum + point[1], 0) / points.length;
  const radii = points.map(point => Math.hypot(point[0] - x, point[1] - y));
  const radius = radii.reduce((sum, value) => sum + value, 0) / radii.length;
  if (radius === 0 || points.some(point => Math.abs(point[2] - z) > Math.max(.001, radius * .02)) ||
      radii.some(value => Math.abs(value - radius) > Math.max(.001, radius * .05))) return false;
  const angles = points.map(point => Math.atan2(point[1] - y, point[0] - x))
    .sort((a, b) => a - b);
  const wanted = 2 * Math.PI / points.length;
  const gaps = angles.map((angle, i) => {
    const next = i + 1 < angles.length ? angles[i + 1] : angles[0] + 2 * Math.PI;
    return next - angle;
  });
  return gaps.every(gap => Math.abs(gap - wanted) <= Math.max(.0873, wanted * .05));
}

function ulaProjection(points) {
  if (points.length < 2) return null;
  let endpoints = [points[0], points[1]], span = distance(...endpoints);
  for (let i = 0; i < points.length; i++) {
    for (let j = i + 1; j < points.length; j++) {
      const candidate = distance(points[i], points[j]);
      if (candidate > span) { span = candidate; endpoints = [points[i], points[j]]; }
    }
  }
  if (span === 0) return null;
  const axis = vector(endpoints[1], endpoints[0]).map(value => value / span);
  const offsets = points.map(point => {
    const relative = vector(point, endpoints[0]);
    const along = dot(relative, axis);
    const residual = relative.map((value, i) => value - along * axis[i]);
    return {along, perpendicular: norm(residual)};
  });
  const tolerance = Math.max(ARRAY_CONTRACT.measurementTolerance.absoluteM,
    span * ARRAY_CONTRACT.measurementTolerance.ulaCollinearRelative);
  const coordinates = offsets.map(item => item.along).sort((a, b) => a - b);
  const meanSpacing = span / (points.length - 1);
  const spacingTolerance = Math.max(ARRAY_CONTRACT.measurementTolerance.absoluteM,
    meanSpacing * ARRAY_CONTRACT.measurementTolerance.ulaSpacingRelative);
  return {span, axis, collinear: offsets.every(item => item.perpendicular <= tolerance),
    uniformlySpaced: coordinates.slice(1).every((value, i) => {
      const gap = value - coordinates[i];
      return gap > 0 && Math.abs(gap - meanSpacing) <= spacingTolerance;
    }), coordinates};
}

function spacingAssessment(points, shape, frequency) {
  const unassessed = {assessed: false, aliasRisk: null, aliasAssessment: 'unresolved'};
  if (!boundedPoints(points) || !finite(frequency) || frequency <= 0)
    return unassessed;
  const halfWavelengthM = C / frequency / 2;
  if (!finite(halfWavelengthM) || halfWavelengthM <= 0) return unassessed;
  let spacings;
  if (shape === 'ula') {
    const projected = ulaProjection(points);
    if (!projected?.collinear || !projected.uniformlySpaced) return unassessed;
    spacings = projected.coordinates.slice(1)
      .map((value, i) => value - projected.coordinates[i]);
  } else {
    spacings = points.map((point, i) => Math.min(...points
      .filter((_, j) => i !== j).map(other => distance(point, other))));
  }
  const thresholdReached = spacings.some(value => value >= halfWavelengthM);
  return {assessed: true, halfWavelengthM,
    maximumRelevantSpacingM: Math.max(...spacings),
    method: shape === 'ula' ? 'ula-adjacent-spacing' : 'nearest-neighbor-heuristic',
    spacingThresholdReached: thresholdReached,
    // A small nearest-neighbor distance does not bound the other baselines.
    // Even a short ULA retains directional ambiguity; no manifold is certified.
    aliasRisk: thresholdReached ? true : null,
    aliasAssessment: thresholdReached ? 'possible' : 'unresolved'};
}

function evaluateKrakenArray(config, liveStatus = null) {
  const device = config?.capture?.device || {};
  const geometry = device.array_geometry;
  const base = {
    supported: device.type === 'Kraken', configured: geometry !== undefined,
    experimental: true, trackerAllowed: false, bearingImplemented: false,
    relativeBearingEligible: false, worldBearingEligible: false,
    verification: {
      channelCount: {authority: 'heimdall', state: 'unreported'},
      physicalAgreement: {authority: 'physical-survey-and-runtime-mapping', state: 'unverified'},
      statusFreshness: {state: 'unverified'},
      receiverSerials: {authority: 'operator', state: 'unverified'},
      daqElementMapping: {authority: 'operator', state: 'unconfirmed'},
      physicalGeometry: {authority: 'operator', state: 'unconfirmed'},
      trueNorthOrientation: {authority: 'operator', state: 'unconfirmed'}
    },
    issues: []
  };
  if (!base.supported)
    return {...base, configured: false, valid: true, state: 'not-applicable',
      relativeBearingEligible: false, worldBearingEligible: false, mapping: []};
  if (geometry === undefined)
    return {...base, valid: true, state: 'unknown', relativeBearingEligible: false,
      worldBearingEligible: false, mapping: []};
  const issue = (code, severity, message, details = {}) =>
    base.issues.push({code, severity, message, ...details});
  if (!object(geometry)) {
    issue('geometry-type', 'error', 'array_geometry must be an object.');
    return {...base, valid: false, state: 'invalid', relativeBearingEligible: false,
      worldBearingEligible: false, mapping: []};
  }

  const count = device.channel_count;
  if (!Number.isInteger(count) || count < ARRAY_CONTRACT.channelCount.min ||
      count > ARRAY_CONTRACT.channelCount.max) {
    issue('channel-count', 'error', 'Kraken channel_count must be an integer from 2 through 8.');
    return {...base, valid: false, state: 'invalid', mapping: []};
  }
  // Reject oversized containers before any traversal, copying, or allocation.
  // In particular, Array#every/some skip holes and cannot validate sparse input.
  for (const [name, values] of [
    ['surveillance_channels', device.surveillance_channels],
    ['reference_synthesis.channels', config?.process?.reference_synthesis?.channels],
    ['bearing_channels', geometry.bearing_channels], ['elements', geometry.elements]
  ]) {
    if (Array.isArray(values) && values.length > ARRAY_CONTRACT.channelCount.max) {
      issue('array-size', 'error', `${name} cannot contain more than eight entries.`);
      return {...base, valid: false, state: 'invalid', mapping: []};
    }
  }
  const reference = device.reference_channel;
  const surveillance = Array.isArray(device.surveillance_channels) ?
    [...device.surveillance_channels] : [];
  if (!Number.isInteger(reference) || reference < 0 || reference >= count)
    issue('reference-channel', 'error', 'reference_channel must identify an active DAQ channel.');
  if (!Array.isArray(device.surveillance_channels))
    issue('surveillance-type', 'error', 'surveillance_channels must be an array.');
  if (new Set(surveillance).size !== surveillance.length ||
      surveillance.some(channel => !Number.isInteger(channel) || channel < 0 || channel >= count))
    issue('surveillance-channels', 'error', 'Surveillance channels must be unique active DAQ channels.');

  const synthesis = config?.process?.reference_synthesis || {};
  if (!['dedicated', 'array_eigenbeam'].includes(synthesis.mode))
    issue('synthesis-mode', 'error', 'Reference synthesis mode must be dedicated or array_eigenbeam.');
  if (synthesis.mode === 'array_eigenbeam' && synthesis.channels !== undefined) {
    const selected = Array.isArray(synthesis.channels) ? [...synthesis.channels] : null;
    if (!selected || selected.length === 1 || new Set(selected).size !== selected.length ||
        selected.some(channel => !Number.isInteger(channel) || channel < 0 || channel >= count))
      issue('synthesis-channels', 'error',
        'Array-reference channels must be unique active channels; an omitted or empty list uses all channels.');
  }
  if (synthesis.mode === 'dedicated') {
    if (surveillance.includes(reference))
      issue('dedicated-role-overlap', 'error', 'A dedicated reference cannot also be a surveillance channel.');
    if (!Array.isArray(synthesis.channels) || synthesis.channels.length !== 1 ||
        synthesis.channels[0] !== reference)
      issue('dedicated-synthesis-mismatch', 'error',
        'Dedicated synthesis channels must contain exactly reference_channel.');
  }

  if (!ARRAY_CONTRACT.shapes.includes(geometry.shape))
    issue('shape', 'error', `Array shape must be one of ${ARRAY_CONTRACT.shapes.join(', ')}.`);
  if (geometry.units !== ARRAY_CONTRACT.units)
    issue('units', 'error', 'Array positions must use metres (units: m).');
  if (geometry.coordinate_frame !== ARRAY_CONTRACT.coordinateFrame)
    issue('coordinate-frame', 'error',
      `coordinate_frame must be ${ARRAY_CONTRACT.coordinateFrame}.`);

  for (const [field, label] of [['mapping_confirmed', 'DAQ-to-element mapping'],
    ['geometry_confirmed', 'Physical geometry'], ['orientation_confirmed', 'True-north orientation']]) {
    if (geometry[field] !== undefined && typeof geometry[field] !== 'boolean')
      issue(`${field}-type`, 'error', `${field} must be true or false.`);
    if (geometry[field] !== true)
      issue(`${field}-unconfirmed`, 'warning', `${label} is not operator-confirmed.`);
  }
  if (geometry.x_axis_bearing_deg_true != null &&
      (!finite(geometry.x_axis_bearing_deg_true) || geometry.x_axis_bearing_deg_true < 0 ||
       geometry.x_axis_bearing_deg_true >= 360))
    issue('orientation-value', 'error',
      'x_axis_bearing_deg_true must be at least 0 and less than 360 degrees.');
  if (geometry.orientation_confirmed === true && !finite(geometry.x_axis_bearing_deg_true))
    issue('orientation-missing', 'error',
      'A confirmed orientation requires x_axis_bearing_deg_true.');

  const bearingChannels = Array.isArray(geometry.bearing_channels) ?
    [...geometry.bearing_channels] : [];
  if (!Array.isArray(geometry.bearing_channels) || bearingChannels.length < 2 ||
      new Set(bearingChannels).size !== bearingChannels.length ||
      bearingChannels.some(channel => !Number.isInteger(channel) || channel < 0 || channel >= count))
    issue('bearing-channels', 'error',
      'bearing_channels must contain at least two unique active DAQ channels.');
  if (bearingChannels.some(channel => !surveillance.includes(channel)))
    issue('bearing-role', 'error', 'Every bearing channel must also be a surveillance channel.');
  if (synthesis.mode === 'dedicated' && bearingChannels.includes(reference))
    issue('bearing-reference-overlap', 'error',
      'The dedicated reference must be excluded from bearing_channels.');

  const elements = Array.isArray(geometry.elements) ? geometry.elements : [];
  if (!Array.isArray(geometry.elements) || elements.length !== count)
    issue('element-count', 'error', 'elements must contain exactly one entry per active DAQ channel.');
  const ids = [], channels = [], serials = [], positions = new Map();
  for (const [index, element] of elements.entries()) {
    if (!object(element)) { issue('element-type', 'error', `Element ${index} must be an object.`); continue; }
    if (!label(element.id))
      issue('element-id', 'error', `Element ${index} requires a trimmed id of 1–128 characters without control characters.`);
    else ids.push(element.id);
    if (!Number.isInteger(element.daq_channel) || element.daq_channel < 0 || element.daq_channel >= count)
      issue('element-channel', 'error', `Element ${index} has an invalid daq_channel.`);
    else channels.push(element.daq_channel);
    if (element.receiver_serial !== undefined) {
      if (!label(element.receiver_serial))
        issue('receiver-serial', 'error', `Element ${index} receiver_serial requires a trimmed string of 1–128 characters without control characters.`);
      else serials.push(element.receiver_serial);
    }
    if (!position(element.position))
      issue('element-position', 'error', `Element ${index} requires three finite metre coordinates within the safe numeric range.`);
    else {
      const key = element.position.map(value => Object.is(value, -0) ? 0 : value).join(',');
      if (positions.has(key))
        issue('duplicate-position', 'error',
          `Elements ${positions.get(key)} and ${index} have the same position.`);
      else positions.set(key, index);
    }
  }
  if (new Set(ids).size !== ids.length)
    issue('duplicate-element-id', 'error', 'Element ids must be unique.');
  if (new Set(channels).size !== channels.length)
    issue('duplicate-element-channel', 'error', 'Each DAQ channel may appear only once in elements.');
  if (Number.isInteger(count) &&
      Array.from({length: count}, (_, channel) => channel).some(channel => !channels.includes(channel)))
    issue('missing-element-channel', 'error', 'Every active DAQ channel must appear in elements.');
  if (new Set(serials).size !== serials.length)
    issue('duplicate-receiver-serial', 'error', 'Configured receiver serials must be unique.');

  const byChannel = new Map(elements.filter(element => object(element) &&
    Number.isInteger(element.daq_channel)).map(element => [element.daq_channel, element]));
  const bearingPoints = bearingChannels.map(channel => byChannel.get(channel)?.position)
    .filter(position);
  if (bearingChannels.length >= 2 && bearingPoints.length === bearingChannels.length) {
    if (geometry.shape === 'cross' && !validateCross(bearingPoints))
      issue('cross-geometry', 'error',
        'A cross requires four bearing elements in two centred perpendicular pairs in the local XY plane.');
    if (geometry.shape === 'regular_polygon' && !validateRegularPolygon(bearingPoints))
      issue('polygon-geometry', 'error',
        'regular_polygon requires three or more bearing elements at measured regular-polygon positions.');
    if (geometry.shape === 'ula') {
      const projection = ulaProjection(bearingPoints);
      if (!projection?.collinear)
        issue('ula-geometry', 'error', 'ULA bearing elements must be collinear.');
      else if (!projection.uniformlySpaced)
        issue('ula-spacing', 'error',
          'ULA bearing elements must have equal adjacent spacing; record a nonuniform linear array as custom.');
      const halfPlane = geometry.ula_half_plane ?? 'both';
      if (!ARRAY_CONTRACT.ulaHalfPlanes.includes(halfPlane))
        issue('ula-half-plane', 'error',
          `ula_half_plane must be one of ${ARRAY_CONTRACT.ulaHalfPlanes.join(', ')}.`);
      else if (halfPlane === 'both')
        issue('ula-front-back-ambiguity', 'warning',
          'A ULA has unresolved directional/front-back ambiguity. No bearing output is implemented.');
      else
        issue('ula-half-plane-constraint', 'warning',
          'The selected local half-plane is operator metadata, not a measured ambiguity resolution or an implemented Suite output selection.');
    }
    const horizontal = ulaProjection(bearingPoints.map(point => [point[0], point[1], 0]));
    if (!horizontal)
      issue('azimuth-unobservable', 'warning',
        'All bearing elements have the same horizontal position; this aperture provides no azimuth information.');
    else if (horizontal.collinear && geometry.shape !== 'ula')
      issue('horizontal-mirror-ambiguity', 'warning',
        'The horizontal aperture is collinear; the shape label does not remove directional mirror ambiguity.');
    const spacing = spacingAssessment(bearingPoints, geometry.shape, config?.capture?.fc);
    if (!spacing.assessed)
      issue('spacing-unassessed', 'warning',
        'Inter-element spacing could not be assessed at the configured RF frequency.');
    else if (spacing.aliasRisk)
      issue('spacing-alias-risk', 'warning',
        'One or more checked spacings reach half a wavelength; spatial aliasing is possible.', spacing);
    else
      issue('aliasing-unresolved', 'warning',
        'Small checked spacings do not certify an unambiguous manifold; spatial aliasing remains unassessed.', spacing);
  }

  const rawCount = Number.isInteger(liveStatus?.num_channels) ? liveStatus.num_channels : null;
  const normalizedCount = Number.isInteger(liveStatus?.actual?.channels) ? liveStatus.actual.channels : null;
  const reportedCount = rawCount ?? normalizedCount;
  if (rawCount !== null && normalizedCount !== null && rawCount !== normalizedCount) {
    base.verification.channelCount = {authority: 'heimdall', state: 'conflict',
      configured: count, rawReported: rawCount, normalizedReported: normalizedCount};
    issue('live-channel-count-conflict', 'warning',
      'Raw and normalized count reports conflict; neither establishes agreement.');
  } else if (reportedCount !== null) {
    base.verification.channelCount = {authority: 'heimdall',
      state: liveStatus?.available === false ? 'unavailable' :
        reportedCount === count ? 'reported-match' : 'reported-mismatch', configured: count,
      reported: reportedCount};
    if (reportedCount !== count)
      issue('live-channel-count', 'warning',
        'Reported Heimdall channel count differs from channel_count; compatible acquisition is unconfirmed.',
        {configured: count, reported: reportedCount});
  }
  issue('bearing-unimplemented', 'warning',
    'Geometry is metadata only. Selected-channel bearing processing and Suite coordinate conversion are not implemented.');
  issue('physical-agreement-unverified', 'warning',
    'Operator attestations and status counts do not verify physical geometry, cable wiring, or current serial order.');
  base.verification.daqElementMapping.state = geometry.mapping_confirmed === true ?
    'operator-asserted' : 'unconfirmed';
  base.verification.physicalGeometry.state = geometry.geometry_confirmed === true ?
    'operator-asserted' : 'unconfirmed';
  base.verification.trueNorthOrientation.state = geometry.orientation_confirmed === true ?
    'operator-asserted' : 'unconfirmed';
  if (serials.length === count)
    base.verification.receiverSerials.state = 'operator-metadata';

  const errors = base.issues.filter(item => item.severity === 'error');
  const valid = errors.length === 0;
  const mapping = elements.filter(object).map(element => ({
    id: label(element.id) ? element.id : null,
    daqChannel: Number.isInteger(element.daq_channel) && element.daq_channel >= 0 &&
      element.daq_channel < count ? element.daq_channel : null,
    receiverSerial: label(element.receiver_serial) ? element.receiver_serial : null,
    positionM: position(element.position) ? [...element.position] : null,
    reference: element.daq_channel === reference,
    surveillance: surveillance.includes(element.daq_channel),
    bearing: bearingChannels.includes(element.daq_channel)
  }));
  return {...base, valid, state: valid ? 'configured-unverified' : 'invalid', mapping};
}

module.exports = {ARRAY_CONTRACT, evaluateKrakenArray,
  localAzimuthToTrueBearing, spacingAssessment};
