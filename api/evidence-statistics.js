'use strict';

const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const {validateTruthWindow} = require('./capture-admission');

const SPEED_OF_LIGHT_KM_S = 299792.458;
// Detailed finalized evidence is a bounded working set; lifetime headline
// counts roll into state.rollup. Without this boundary a long-running node can
// exceed the loader's safety cap and lose operational score visibility on the
// next API restart.
const MAX_PASSES = 500;
const MAX_ELIGIBILITY_EVENTS = 250;
// First/last truth remain separately preserved on every pass. Sixteen detailed
// tail rows are enough for diagnosis without letting 500 passes approach the
// 64 MiB normal operating target.
const MAX_FINALIZED_CPI_AUDIT = 4;
// Finalized rows already preserve firstTruth, lastTruth, all stratified
// counters and any exact recording-window truth.  Keeping another expanded
// trajectory in truthHistory only duplicates tens of megabytes of data.
const MAX_FINALIZED_TRUTH_SAMPLES = 0;
const MAX_FINALIZED_EVENT_TRUTH_SAMPLES = 4;
const MAX_STATE_INPUT_BYTES = 256 * 1024 * 1024;
// Keep the trigger window permanently while retaining a recent tail for pass
// diagnostics.  A rolling-only buffer loses the ADS-B trajectory associated
// with an IQ clip when an aircraft remains eligible for more than 30 seconds.
const TRIGGER_EVENT_TRUTH_SAMPLES = 30;
// Capture windows copy their own exact truth brackets.  The parent event and
// active pass therefore only need a short recent tail for the next automatic
// clip; retaining hundreds of duplicated expanded aircraft records made the
// 250-event ledger larger than 100 MiB.
const MAX_EVENT_TRUTH_SAMPLES = TRIGGER_EVENT_TRUTH_SAMPLES;
const TRUTH_SAMPLE_MS = 500;
const MAX_ACTIVE_CAPTURE_TIMESTAMPS = 128;
const MAX_CAPTURE_ASSOCIATIONS = 64;
const MAX_TIMING_FRAMES = 2048;
const MAX_RADAR_FRAMES = 256;
const MAX_PENDING_SAMPLES = 32;
// Active passes used to retain 1,024 fully expanded CPI rows per aircraft.
// A busy wide-area ADS-B feed can keep more than one hundred aircraft active,
// turning the operational state into a 200+ MiB JSON document.  Serializing
// that document blocks the API event loop and makes every browser appear to
// freeze, even though the radar itself is healthy.  Eligibility counters,
// extents, first/last truth, capture truth windows and lifetime rollups are
// maintained separately; this is only the recent engineering tail.
const MAX_CPI_AUDIT = 16;
const TIMING_RETENTION_MS = 10 * 60 * 1000;
const PASS_GAP_MS = 120000;
const DEFAULT_PROVISIONAL_TIME_UNCERTAINTY_S = 1;
const WINDOWS = Object.freeze({hour: 3600000, day: 86400000,
  week: 7 * 86400000, lifetime: Infinity});
const SLANT_RANGE_EDGES_KM = Object.freeze([0, 10, 20, 30, 40, 50, 75, 100, 150, 250, 500]);
const ELEVATION_EDGES_DEG = Object.freeze([0, 2, 5, 10, 15, 30, 90]);
const AZIMUTH_EDGES_DEG = Object.freeze([0, 45, 90, 135, 180, 225, 270, 315, 360]);
const RADAR_COVERAGE_ALTITUDE_BANDS = Object.freeze({
  all: [-Infinity, Infinity], low: [0, 10000], medium: [10000, 20000],
  high: [20000, 30000], very_high: [30000, 40000], extreme: [40000, Infinity]
});
const RADAR_COVERAGE_BINS = 72;
const RADAR_COVERAGE_BUCKET_MS = 3600000;
const finite = value => typeof value === 'number' && Number.isFinite(value);
const normalizeHex = value => typeof value === 'string' && /^[0-9a-f]{6}$/i.test(value.trim()) ?
  value.trim().toLowerCase() : null;
const parse = value => {
  if (value && typeof value === 'object') return value;
  try { return JSON.parse(value); } catch (_) { return null; }
};
const ratio = (numerator, denominator) => denominator ? numerator / denominator : null;

function delayBandEdges(span) {
  if (!span) return [];
  return [...new Set([Math.max(span.delayMinKm, span.detectionMinDelayKm),
    10, 20, 30, 40, 50, span.delayMaxKm].filter(value => finite(value) &&
      value >= span.detectionMinDelayKm && value <= span.delayMaxKm))].sort((a, b) => a - b);
}

function delayBand(span, delay) {
  const edges = delayBandEdges(span);
  for (let index = 0; index + 1 < edges.length; ++index)
    if (delay >= edges[index] && (delay < edges[index + 1] ||
        index + 2 === edges.length && delay <= edges[index + 1]))
      return {id: `${edges[index].toFixed(3)}-${edges[index + 1].toFixed(3)}`,
        minimum_km: edges[index], maximum_km: edges[index + 1]};
  return null;
}

function fixedBand(edges, value) {
  if (!finite(value) || value < edges[0]) return null;
  for (let index = 0; index + 1 < edges.length; ++index)
    if (value >= edges[index] && value < edges[index + 1])
      return {id: `${edges[index]}-${edges[index + 1]}`,
        minimum: edges[index], maximum: edges[index + 1]};
  return {id: `${edges.at(-1)}+`, minimum: edges.at(-1), maximum: null};
}

function azimuthBand(value) {
  return fixedBand(AZIMUTH_EDGES_DEG, finite(value) ? ((value % 360) + 360) % 360 : value);
}

function adsbQuality(config) {
  const configured = Array.isArray(config.truth?.adsb?.sources) && config.truth.adsb.sources.length ?
    config.truth.adsb.sources : [config.truth?.adsb?.tar1090].filter(Boolean);
  const online = configured.some(source => typeof source === 'string' && source.startsWith('exchange:'));
  return {configured_sources: configured.length, online_source_enabled: online,
    local_only: config.truth?.adsb?.enabled === true && !online,
    warning: config.truth?.adsb?.enabled === true && !online ?
      'Local-only ADS-B can add feed and host-arrival timing uncertainty to pass statistics. Enable an online ADS-B source for independent coverage and observation-time cross-checking; this does not replace exact SDR event time.' : null};
}

function limits(config) {
  const sampleRate = config.capture?.fs;
  const ambiguity = config.process?.ambiguity || {};
  const detection = config.process?.detection || {};
  const clutter = config.process?.clutter || {};
  if (!finite(sampleRate) || sampleRate <= 0 || !finite(ambiguity.delayMin) ||
      !finite(ambiguity.delayMax) || !finite(ambiguity.dopplerMin) ||
      !finite(ambiguity.dopplerMax) || !finite(detection.minDelay)) return null;
  const kmPerBin = SPEED_OF_LIGHT_KM_S / sampleRate;
  const cpiSeconds = finite(config.process?.data?.cpi) && config.process.data.cpi > 0 ?
    config.process.data.cpi : .5;
  const configuredTimeUncertainty = config.truth?.adsb?.provisional_event_time_uncertainty_s;
  const provisionalTimeUncertaintyS = finite(configuredTimeUncertainty) ?
    Math.max(0, Math.min(5, configuredTimeUncertainty)) :
    DEFAULT_PROVISIONAL_TIME_UNCERTAINTY_S;
  return {sampleRate, frequency: config.capture?.fc, kmPerBin, cpiSeconds,
    dopplerResolutionHz: 1 / cpiSeconds,
    delayMinBin: ambiguity.delayMin, delayMaxBin: ambiguity.delayMax,
    delayMinKm: ambiguity.delayMin * kmPerBin,
    delayMaxKm: ambiguity.delayMax * kmPerBin,
    detectionMinDelayBin: detection.minDelay,
    detectionMinDelayKm: detection.minDelay * kmPerBin,
    dopplerMinHz: ambiguity.dopplerMin, dopplerMaxHz: ambiguity.dopplerMax,
    clutterMaxBin: finite(clutter.delayMax) ? clutter.delayMax : null,
    clutterMaxKm: finite(clutter.delayMax) ? clutter.delayMax * kmPerBin : null,
    matchDelayKm: config.process?.bearing?.manifold_recorder?.max_delay_error_km ?? .25,
    matchDopplerHz: config.process?.bearing?.manifold_recorder?.max_doppler_error_hz ?? 5,
    maxTruthAgeS: config.process?.bearing?.manifold_recorder?.max_truth_age_s ?? 3,
    provisionalTimeUncertaintyS};
}

function associationGates(span, prediction, timestamp, kind = 'detection',
  eventTimeUncertaintyS = 0) {
  const age = finite(prediction?.timestamp) ?
    Math.min(span.maxTruthAgeS, Math.max(0, timestamp / 1000 - prediction.timestamp)) :
    span.maxTruthAgeS;
  // A tracker estimate can lag several CPIs behind the newest measurement.
  // Account for that bounded motion without borrowing the much tighter bearing
  // calibration gate as the entire radar-association uncertainty model.
  const boundedTimeUncertainty = finite(eventTimeUncertaintyS) ?
    Math.max(0, eventTimeUncertaintyS) : 0;
  const horizon = age + span.cpiSeconds * (kind === 'tracker' ? 3 : 1) +
    boundedTimeUncertainty;
  const delayMotion = finite(prediction?.delay_rate_km_s) ?
    Math.abs(prediction.delay_rate_km_s) * horizon : 0;
  const dopplerMotion = finite(prediction?.doppler_rate_hz_s) ?
    Math.abs(prediction.doppler_rate_hz_s) * horizon : 0;
  const delayFloor = span.kmPerBin * (kind === 'tracker' ? 3 : 2);
  const dopplerFloor = span.dopplerResolutionHz * (kind === 'tracker' ? 4 : 3);
  return {
    delayKm: Math.min(Math.max(span.matchDelayKm, delayFloor + delayMotion),
      Math.max(1, span.kmPerBin * 8)),
    dopplerHz: Math.min(Math.max(span.matchDopplerHz, dopplerFloor + dopplerMotion),
      Math.max(25, span.dopplerResolutionHz * 12)),
    eventTimeUncertaintyS: boundedTimeUncertainty};
}

function siteKey(config) {
  const relevant = {frequency: config.capture?.fc, sampleRate: config.capture?.fs,
    receiver: config.location?.rx, transmitter: config.location?.tx,
    ambiguity: config.process?.ambiguity, clutter: config.process?.clutter,
    detection: config.process?.detection, tracker: config.process?.tracker,
    reference: config.process?.reference_synthesis,
    surveillanceFusion: config.process?.surveillance_fusion,
    channels: config.capture?.device?.surveillance_channels,
    mapping: config.process?.bearing?.channel_mapping,
    geometry: config.capture?.device?.array_geometry};
  return crypto.createHash('sha256').update(JSON.stringify(relevant)).digest('hex');
}

function digest(value) {
  return crypto.createHash('sha256').update(JSON.stringify(value)).digest('hex');
}

function configEpoch(config) { return digest(config); }

function receiverEpoch(config) {
  return digest({frequency: config.capture?.fc, sampleRate: config.capture?.fs,
    device: config.capture?.device || null, replay: config.capture?.replay || null});
}

function evaluationContract(span, timingPolicy = 'exact') {
  return {schema: 'vectorwarp-live-aircraft-scorecard-v1', delay_min: span?.delayMinBin,
    delay_max: span?.delayMaxBin, doppler_min: span?.dopplerMinHz,
    doppler_max: span?.dopplerMaxHz, timing_trigger: 'native-only-v2',
    exact_timing_required: timingPolicy === 'exact', timing_policy: timingPolicy,
    timing_skew_ms: 100, truth_offset_s: 1,
    provisional_event_time_uncertainty_s: timingPolicy === 'provisional' ?
      span?.provisionalTimeUncertaintyS : 0,
    source_disagreement_km: .5, required_cpis: 3, stable_track_associations: 3,
    eligibility_delay_floor_km: span?.detectionMinDelayKm,
    eligibility_semantics: 'detector-searchable-delay-doppler'};
}

function siteDescriptor(config) {
  return {frequency_hz: config.capture?.fc, sample_rate_hz: config.capture?.fs,
    receiver: config.location?.rx || null, transmitter: config.location?.tx || null,
    ambiguity: config.process?.ambiguity || null, clutter: config.process?.clutter || null,
    detection: config.process?.detection || null, tracker: config.process?.tracker || null,
    reference_synthesis: config.process?.reference_synthesis || null,
    surveillance_fusion: config.process?.surveillance_fusion || null,
    surveillance_channels: config.capture?.device?.surveillance_channels || null,
    channel_mapping: config.process?.bearing?.channel_mapping || null,
    array_geometry: config.capture?.device?.array_geometry || null};
}

function passId(key, hex, at) { return digest({siteKey: key, hex, startedAt: at}); }

function emptyPass(hex, flight, at, key) {
  return {id: passId(key, hex, at), hex, flight: flight || '', siteKey: key,
    startedAt: at, lastAt: at,
    frames: 0, physicalEligibleFrames: 0, detectorEligibleFrames: 0, eligibleFrames: 0,
    detectedFrames: 0, trackedFrames: 0, belowFloorFrames: 0,
    aboveDelayFrames: 0, outsideDopplerFrames: 0, staleTruthFrames: 0,
    missingTimingFrames: 0,
    clutterCancelledFrames: 0, outsideClutterFrames: 0, minDelayKm: null, maxDelayKm: null,
    minDopplerHz: null, maxDopplerHz: null, minAltitudeGeomFt: null,
    maxAltitudeGeomFt: null, minElevationDeg: null, maxElevationDeg: null,
    minGroundRangeKm: null, maxGroundRangeKm: null,
    minSlantRangeKm: null, maxSlantRangeKm: null,
    minTransmitterRangeKm: null, maxTransmitterRangeKm: null,
    minBistaticRangeProductKm2: null, maxBistaticRangeProductKm2: null,
    maxAbsDelayRateKmS: null,
    maxAbsDopplerRateHzS: null, azimuthSectors45: Array(8).fill(0),
    firstTruth: null, lastTruth: null, truthHistory: [], cpiAudit: [],
    firstDetectionAt: null, firstTrackAt: null,
    detectionTimestamps: [], trackTimestamps: [], sampledBuckets: [],
    delayBands: {}, slantRangeBands: {}, elevationBands: {}, azimuthBands: {},
    azimuthElevationBands: {}, geometryBands: {}};
}

function summarizeBands(rows, field) {
  const output = {};
  for (const pass of rows) for (const [id, band] of Object.entries(pass[field] || {})) {
    const aggregate = output[id] ||= {minimum: band.minimum_km ?? band.minimum,
      maximum: band.maximum_km ?? band.maximum, eligible_passes: 0, detected_passes: 0,
      tracked_passes: 0, eligible_cpis: 0, detected_cpis: 0, tracked_cpis: 0};
    aggregate.eligible_cpis += band.eligibleFrames || 0;
    aggregate.detected_cpis += band.detectedFrames || 0;
    aggregate.tracked_cpis += band.trackedFrames || 0;
    if ((band.eligibleFrames || 0) >= 3) {
      aggregate.eligible_passes += 1;
      if ((band.detectedFrames || 0) >= 3) aggregate.detected_passes += 1;
      if ((band.trackedFrames || 0) >= 3) aggregate.tracked_passes += 1;
    }
  }
  for (const band of Object.values(output)) {
    band.detection_yield = ratio(band.detected_passes, band.eligible_passes);
    band.tracking_yield = ratio(band.tracked_passes, band.eligible_passes);
  }
  return output;
}

function summarizeDelayBands(rows, span) {
  return {definition: 'excess_bistatic_path_km', edges_km: delayBandEdges(span),
    finalized_passes_only: true, bands: summarizeBands(rows, 'delayBands')};
}

function primaryGeometryBand(pass) {
  const entries = Object.entries(pass.geometryBands || {});
  if (!entries.length) return null;
  return entries.sort((left, right) => (right[1].eligibleFrames || 0) -
    (left[1].eligibleFrames || 0) || left[0].localeCompare(right[0]))[0][0];
}

function standardizedComparison(current, baseline, minimumPerArm = 3) {
  const group = rows => {
    const result = new Map();
    for (const pass of rows) {
      const band = primaryGeometryBand(pass);
      if (!band) continue;
      const value = result.get(band) || {eligible: 0, detected: 0, tracked: 0};
      value.eligible += 1;
      if (pass.detectedFrames >= 3) value.detected += 1;
      if (pass.trackedFrames >= 3) value.tracked += 1;
      result.set(band, value);
    }
    return result;
  };
  const candidate = group(current), reference = group(baseline);
  const common = [...candidate.keys()].filter(key =>
    candidate.get(key).eligible >= minimumPerArm &&
    (reference.get(key)?.eligible || 0) >= minimumPerArm).sort();
  const currentCommon = common.reduce((sum, key) => sum + candidate.get(key).eligible, 0);
  const baselineCommon = common.reduce((sum, key) => sum + reference.get(key).eligible, 0);
  const pooled = common.reduce((sum, key) => sum + candidate.get(key).eligible +
    reference.get(key).eligible, 0);
  const weighted = outcome => pooled ? common.reduce((sum, key) => {
    const weight = (candidate.get(key).eligible + reference.get(key).eligible) / pooled;
    return sum + weight * ratio(candidate.get(key)[outcome], candidate.get(key).eligible);
  }, 0) : null;
  const baselineWeighted = outcome => pooled ? common.reduce((sum, key) => {
    const weight = (candidate.get(key).eligible + reference.get(key).eligible) / pooled;
    return sum + weight * ratio(reference.get(key)[outcome], reference.get(key).eligible);
  }, 0) : null;
  const coverage = Math.min(ratio(currentCommon, current.length) || 0,
    ratio(baselineCommon, baseline.length) || 0);
  return {method: 'pooled-direct-standardization-on-common-primary-geometry-strata',
    primary_stratum: 'excess-delay-band x receiver-slant-range-band',
    minimum_passes_per_arm_per_stratum: minimumPerArm, common_strata: common,
    current_passes: current.length, baseline_passes: baseline.length,
    current_common_support_passes: currentCommon,
    baseline_common_support_passes: baselineCommon, common_support_coverage: coverage,
    decision_grade: common.length > 0 && coverage >= .8,
    current: {detection_yield: weighted('detected'), tracking_yield: weighted('tracked')},
    baseline: {detection_yield: baselineWeighted('detected'),
      tracking_yield: baselineWeighted('tracked')}};
}

function compactFinalizedPass(pass) {
  if (!pass || typeof pass !== 'object') return pass;
  if (Array.isArray(pass.cpiAudit) && pass.cpiAudit.length > MAX_FINALIZED_CPI_AUDIT)
    pass.cpiAudit = pass.cpiAudit.slice(-MAX_FINALIZED_CPI_AUDIT);
  if (Array.isArray(pass.truthHistory) &&
      pass.truthHistory.length > MAX_FINALIZED_TRUTH_SAMPLES)
    pass.truthHistory = MAX_FINALIZED_TRUTH_SAMPLES ?
      pass.truthHistory.slice(-MAX_FINALIZED_TRUTH_SAMPLES) : [];
  return pass;
}

function compactActivePass(pass) {
  if (!pass || typeof pass !== 'object') return pass;
  for (const key of ['cpiAudit', 'sampledBuckets'])
    if (Array.isArray(pass[key]) && pass[key].length > MAX_CPI_AUDIT)
      pass[key] = pass[key].slice(-MAX_CPI_AUDIT);
  if (Array.isArray(pass.truthHistory)) {
    // Once eligibility exists, the event owns the recent truth tail and each
    // recording window owns its exact bracket. Do not retain a third copy.
    if (pass.eligibilityEventId) pass.truthHistory = [];
    else if (pass.truthHistory.length > MAX_EVENT_TRUTH_SAMPLES)
      pass.truthHistory = pass.truthHistory.slice(-MAX_EVENT_TRUTH_SAMPLES);
  }
  return pass;
}

function compactEligibilityEvent(event) {
  if (!Array.isArray(event?.truth_samples)) return event;
  // Once a pass is finalized, automatic recording has also finished. Exact
  // brackets live in capture_windows; the parent trajectory is redundant.
  if (event.state === 'finalized' &&
      event.truth_samples.length > MAX_FINALIZED_EVENT_TRUTH_SAMPLES)
    event.truth_samples = event.truth_samples.slice(-MAX_FINALIZED_EVENT_TRUTH_SAMPLES);
  else if (event.truth_samples.length > MAX_EVENT_TRUTH_SAMPLES)
    event.truth_samples = event.truth_samples.slice(-MAX_EVENT_TRUTH_SAMPLES);
  return event;
}

// A coverage outline is meaningful only where the radar itself produced a
// stable track association.  ADS-B/MLAT contributes the aircraft position for
// that association; it never qualifies a point by itself.  Keeping this as a
// compact radial maximum avoids sending a day of raw trajectories to phones.
function radarCoverageFromPasses(passes, {now = Date.now(), hours = 24,
  altitudeBand = 'all', bins = 72, siteKey: requiredSiteKey = null} = {}) {
  const boundedHours = Math.max(1, Math.min(168, Number(hours) || 24));
  const boundedBins = Math.max(24, Math.min(360, Math.round(Number(bins) || 72)));
  const band = RADAR_COVERAGE_ALTITUDE_BANDS[altitudeBand] ||
    RADAR_COVERAGE_ALTITUDE_BANDS.all;
  const cutoff = now - boundedHours * 3600000;
  const maxima = new Map(), supportingPasses = new Set(), aircraft = new Set();
  let associatedSamples = 0;
  for (const pass of Array.isArray(passes) ? passes : []) {
    if (!pass || pass.lastAt < cutoff ||
        (requiredSiteKey && pass.siteKey !== requiredSiteKey)) continue;
    const trackedAt = new Set((pass.cpiAudit || []).filter(row =>
      row?.stable_tracked === true && finite(row.timestamp_ms)).map(row => row.timestamp_ms));
    for (const sample of pass.truthHistory || []) {
      if (!trackedAt.has(sample?.timestamp_ms) || sample.timestamp_ms < cutoff ||
          !finite(sample.bearing_deg_true) || !finite(sample.ground_range_km) ||
          !finite(sample.latitude) || !finite(sample.longitude)) continue;
      const altitude = finite(sample.altitude_geom_ft) ? sample.altitude_geom_ft :
        sample.altitude_baro_ft;
      if (!finite(altitude) || altitude < band[0] || altitude >= band[1]) continue;
      const bearing = ((sample.bearing_deg_true % 360) + 360) % 360;
      const bin = Math.floor(bearing / 360 * boundedBins) % boundedBins;
      const prior = maxima.get(bin);
      associatedSamples += 1; supportingPasses.add(pass.id || `${pass.hex}:${pass.startedAt}`);
      if (pass.hex) aircraft.add(pass.hex);
      if (!prior || sample.ground_range_km > prior.ground_range_km) maxima.set(bin, {
        bin, bearing_deg_true: bearing, ground_range_km: sample.ground_range_km,
        latitude: sample.latitude, longitude: sample.longitude,
        altitude_ft: altitude, timestamp_ms: sample.timestamp_ms,
        pass_id: pass.id || null, hex: pass.hex || null
      });
    }
  }
  return {schema: 'vectorwarp-radar-confirmed-coverage-v1', hours: boundedHours,
    altitude_band: altitudeBand in RADAR_COVERAGE_ALTITUDE_BANDS ? altitudeBand : 'all',
    definition: 'farthest ADS-B/MLAT position associated with a confirmed radar track in each bearing bin',
    bins: boundedBins, supported_bins: maxima.size, associated_samples: associatedSamples,
    supporting_passes: supportingPasses.size, supporting_aircraft: aircraft.size,
    cutoff_ms: cutoff, updated_at: now,
    points: [...maxima.values()].sort((left, right) => left.bin - right.bin)};
}

function radarCoverageBand(altitude) {
  if (!finite(altitude) || altitude < 0) return null;
  if (altitude < 10000) return 'low';
  if (altitude < 20000) return 'medium';
  if (altitude < 30000) return 'high';
  if (altitude < 40000) return 'very_high';
  return 'extreme';
}

function rememberRadarCoverage(state, pass, sample) {
  if (!state || !pass || !sample || !finite(sample.timestamp_ms) ||
      !finite(sample.bearing_deg_true) || !finite(sample.ground_range_km) ||
      !finite(sample.latitude) || !finite(sample.longitude)) return;
  const altitude = finite(sample.altitude_geom_ft) ? sample.altitude_geom_ft :
    sample.altitude_baro_ft;
  const altitudeBand = radarCoverageBand(altitude);
  if (!altitudeBand) return;
  state.radarCoverage ||= {schema: 'vectorwarp-radar-coverage-accumulator-v1', buckets: {}};
  state.radarCoverage.buckets ||= {};
  const bucketAt = Math.floor(sample.timestamp_ms / RADAR_COVERAGE_BUCKET_MS) *
    RADAR_COVERAGE_BUCKET_MS;
  const bucket = state.radarCoverage.buckets[bucketAt] ||= {at: bucketAt, samples: 0,
    pass_ids: [], aircraft: [], bands: {all: {}, low: {}, medium: {}, high: {},
      very_high: {}, extreme: {}}};
  bucket.samples += 1;
  const passIdValue = pass.id || `${pass.hex}:${pass.startedAt}`;
  if (!bucket.pass_ids.includes(passIdValue)) bucket.pass_ids.push(passIdValue);
  if (pass.hex && !bucket.aircraft.includes(pass.hex)) bucket.aircraft.push(pass.hex);
  const bearing = ((sample.bearing_deg_true % 360) + 360) % 360;
  const bin = Math.floor(bearing / 360 * RADAR_COVERAGE_BINS) % RADAR_COVERAGE_BINS;
  const point = {bin, bearing_deg_true: bearing, ground_range_km: sample.ground_range_km,
    latitude: sample.latitude, longitude: sample.longitude, altitude_ft: altitude,
    timestamp_ms: sample.timestamp_ms, pass_id: pass.id || null, hex: pass.hex || null};
  for (const name of ['all', altitudeBand]) {
    const prior = bucket.bands[name][bin];
    if (!prior || point.ground_range_km > prior.ground_range_km)
      bucket.bands[name][bin] = point;
  }
  const oldest = sample.timestamp_ms - 8 * 24 * RADAR_COVERAGE_BUCKET_MS;
  for (const [key, value] of Object.entries(state.radarCoverage.buckets))
    if (!finite(value?.at) || value.at < oldest) delete state.radarCoverage.buckets[key];
}

function radarCoverageFromAccumulator(accumulator, {now = Date.now(), hours = 24,
  altitudeBand = 'all'} = {}) {
  const boundedHours = Math.max(1, Math.min(168, Number(hours) || 24));
  const selectedBand = altitudeBand in RADAR_COVERAGE_ALTITUDE_BANDS ? altitudeBand : 'all';
  const cutoff = now - boundedHours * RADAR_COVERAGE_BUCKET_MS;
  const maxima = new Map(), passes = new Set(), aircraft = new Set();
  let associatedSamples = 0;
  for (const bucket of Object.values(accumulator?.buckets || {})) {
    if (!finite(bucket?.at) || bucket.at + RADAR_COVERAGE_BUCKET_MS < cutoff) continue;
    associatedSamples += bucket.samples || 0;
    for (const id of bucket.pass_ids || []) passes.add(id);
    for (const hex of bucket.aircraft || []) aircraft.add(hex);
    for (const point of Object.values(bucket.bands?.[selectedBand] || {})) {
      if (!point || point.timestamp_ms < cutoff) continue;
      const prior = maxima.get(point.bin);
      if (!prior || point.ground_range_km > prior.ground_range_km) maxima.set(point.bin, point);
    }
  }
  return {schema: 'vectorwarp-radar-confirmed-coverage-v1', hours: boundedHours,
    altitude_band: selectedBand,
    definition: 'farthest ADS-B/MLAT position associated with a confirmed radar track in each bearing bin',
    bins: RADAR_COVERAGE_BINS, supported_bins: maxima.size,
    associated_samples: associatedSamples, supporting_passes: passes.size,
    supporting_aircraft: aircraft.size, cutoff_ms: cutoff, updated_at: now,
    points: [...maxima.values()].sort((left, right) => left.bin - right.bin)};
}

function accumulateRollup(state, key, old) {
  const eligible = old.eligibleFrames >= 3;
  const mapEligible = old.physicalEligibleFrames >= 3;
  const detectorEligible = (old.detectorEligibleFrames ?? old.physicalEligibleFrames) >= 3;
  if (old.siteKey !== key) return;
  state.rollup.passes += 1;
  if (mapEligible) state.rollup.mapEligible = (state.rollup.mapEligible || 0) + 1;
  if (detectorEligible) state.rollup.detectorEligible =
    (state.rollup.detectorEligible || 0) + 1;
  if (!detectorEligible) state.rollup.outOfConfig += 1;
  if (detectorEligible && !eligible) state.rollup.truthUnusable += 1;
  if (eligible) state.rollup.eligible += 1;
  if (eligible && old.detectedFrames >= 3) state.rollup.detected += 1;
  if (eligible && old.trackedFrames >= 3) state.rollup.tracked += 1;
  if (eligible && old.trackedFrames < 3) state.rollup.missed += 1;
  if (eligible && old.detectedFrames < 3 && old.trackedFrames < 3)
    state.rollup.eligibleNoRadar += 1;
  if (eligible && old.detectedFrames >= 3 && old.trackedFrames < 3)
    state.rollup.detectedNotTracked += 1;
}

function associationTimestamp(value) {
  return finite(value?.frame_timestamp_ms) ? value.frame_timestamp_ms :
    finite(value) ? value : null;
}

function appendAssociation(values, association) {
  if (!association || !finite(association.frame_timestamp_ms)) return;
  const duplicate = values.some(value => value.frame_timestamp_ms === association.frame_timestamp_ms &&
    value.cell_index === association.cell_index && value.kind === association.kind);
  if (!duplicate) values.push(association);
  if (values.length > MAX_CAPTURE_ASSOCIATIONS)
    values.splice(0, values.length - MAX_CAPTURE_ASSOCIATIONS);
}

function updateCaptureWindowEvidence(event, pass, detected = null, tracked = null) {
  if (!event || !pass) return;
  event.first_detection_at = finite(pass.firstDetectionAt) ? pass.firstDetectionAt : null;
  event.first_track_at = finite(pass.firstTrackAt) ? pass.firstTrackAt : null;
  const windows = Array.isArray(event.capture_windows) ? event.capture_windows : [];
  for (const window of windows) {
    if (!finite(window.window_start) || !finite(window.window_end) ||
        window.window_end < window.window_start) continue;
    for (const [association, count, first, samples] of
      [[detected, 'detected_frames', 'first_detection_at', 'detection_associations'],
        [tracked, 'tracked_frames', 'first_track_at', 'tracker_associations']]) {
      const timestamp = associationTimestamp(association);
      if (!finite(timestamp) || timestamp < window.window_start || timestamp > window.window_end) continue;
      window[count] = (window[count] || 0) + 1;
      window[first] ??= timestamp;
      if (association && typeof association === 'object') {
        if (!Array.isArray(window[samples])) window[samples] = [];
        appendAssociation(window[samples], association);
      }
    }
  }
  const initial = windows.find(window => window.kind === 'initial') || windows[0];
  if (!initial) return;
  event.capture_window_start = initial.window_start;
  event.capture_window_end = initial.window_end;
  event.capture_detected_frames = initial.detected_frames || 0;
  event.capture_tracked_frames = initial.tracked_frames || 0;
  event.capture_first_detection_at = initial.first_detection_at ?? null;
  event.capture_first_track_at = initial.first_track_at ?? null;
}

function upsertCaptureWindow(event, record, pass) {
  if (!event || !record) return;
  if (!Array.isArray(event.capture_windows)) event.capture_windows = [];
  const index = event.capture_windows.findIndex(item => item.capture_id === record.captureId);
  const prior = index < 0 ? {} : event.capture_windows[index];
  const canRecount = Array.isArray(pass?.detectionTimestamps) &&
    Array.isArray(pass?.trackTimestamps);
  const detections = canRecount ? pass.detectionTimestamps.filter(timestamp =>
    timestamp >= record.windowStart && timestamp <= record.windowEnd) : [];
  const tracks = canRecount ? pass.trackTimestamps.filter(timestamp =>
    timestamp >= record.windowStart && timestamp <= record.windowEnd) : [];
  const detectionAssociations = Array.isArray(pass?.detectionAssociations) ?
    pass.detectionAssociations.filter(value => value.frame_timestamp_ms >= record.windowStart &&
      value.frame_timestamp_ms <= record.windowEnd) : (prior.detection_associations || []);
  const trackerAssociations = Array.isArray(pass?.trackerAssociations) ?
    pass.trackerAssociations.filter(value => value.frame_timestamp_ms >= record.windowStart &&
      value.frame_timestamp_ms <= record.windowEnd) : (prior.tracker_associations || []);
  const window = {...prior, capture_id: record.captureId, kind: record.kind || 'initial',
    revision: record.revision, requested_event_time: record.requestedEventTime,
    window_start: record.windowStart, window_end: record.windowEnd,
    state: record.state, file: record.file || '', samples: record.samples || 0,
    failure: record.failure || null,
    admission_state: record.admissionState || prior.admission_state || 'pending',
    admission_reasons: record.admissionReasons || prior.admission_reasons || [],
    truth_samples: record.truthSamples || prior.truth_samples || [],
    cpi_truth: record.cpiTruth || prior.cpi_truth || [],
    integrity: record.integrity || prior.integrity || null,
    detected_frames: canRecount ? detections.length : (prior.detected_frames || 0),
    tracked_frames: canRecount ? tracks.length : (prior.tracked_frames || 0),
    first_detection_at: canRecount ? (detections[0] ?? null) : (prior.first_detection_at ?? null),
    first_track_at: canRecount ? (tracks[0] ?? null) : (prior.first_track_at ?? null),
    detection_associations: detectionAssociations.slice(-MAX_CAPTURE_ASSOCIATIONS),
    tracker_associations: trackerAssociations.slice(-MAX_CAPTURE_ASSOCIATIONS)};
  if (index < 0) event.capture_windows.push(window);
  else event.capture_windows[index] = window;
  updateCaptureWindowEvidence(event, pass);
}

function updateLegacyCapture(event, window) {
  if (!event || !window) return;
  event.capture_state = window.state === 'acknowledged' ? 'acknowledged' :
    window.state === 'failed' ? 'failed' : window.state === 'dropped' ? 'dropped' :
      'trigger_pending';
  if (window.failure) event.capture_failure = window.failure;
  else delete event.capture_failure;
  if (window.file) event.capture_file = window.file;
  else delete event.capture_file;
  if (Number.isSafeInteger(window.samples) && window.samples > 0)
    event.capture_samples = window.samples;
  else delete event.capture_samples;
}

function appendBounded(timestamps, timestamp) {
  timestamps.push(timestamp);
  if (timestamps.length > MAX_ACTIVE_CAPTURE_TIMESTAMPS)
    timestamps.splice(0, timestamps.length - MAX_ACTIVE_CAPTURE_TIMESTAMPS);
}

function eligibilityOutcome(pass) {
  if (pass.eligibleFrames < 3) return pass.physicalEligibleFrames >= 3 ?
    'truth_unusable' : 'insufficient_opportunities';
  if (pass.trackedFrames >= 3) return 'tracked';
  if (pass.detectedFrames >= 3) return 'detected_not_tracked';
  return 'no_radar_return';
}

function updateExtent(pass, delay, doppler) {
  pass.minDelayKm = pass.minDelayKm === null ? delay : Math.min(pass.minDelayKm, delay);
  pass.maxDelayKm = pass.maxDelayKm === null ? delay : Math.max(pass.maxDelayKm, delay);
  pass.minDopplerHz = pass.minDopplerHz === null ? doppler : Math.min(pass.minDopplerHz, doppler);
  pass.maxDopplerHz = pass.maxDopplerHz === null ? doppler : Math.max(pass.maxDopplerHz, doppler);
}

function extent(pass, minimum, maximum, value) {
  if (!finite(value)) return;
  pass[minimum] = pass[minimum] === null || pass[minimum] === undefined ? value : Math.min(pass[minimum], value);
  pass[maximum] = pass[maximum] === null || pass[maximum] === undefined ? value : Math.max(pass[maximum], value);
}

function truthSnapshot(prediction, timestamp) {
  const position = prediction.position || {}, geometry = prediction.geometry || {};
  return {timestamp_ms: timestamp, position_timestamp_s: prediction.timestamp,
    latitude: position.latitude, longitude: position.longitude,
    altitude_geom_ft: position.altitude_geom_ft, altitude_baro_ft: position.altitude_baro_ft,
    ground_speed_kt: position.ground_speed_kt, track_deg_true: position.track_deg_true,
    geom_rate_fpm: position.geom_rate_fpm, baro_rate_fpm: position.baro_rate_fpm,
    category: position.category, squawk: position.squawk, rssi_dbfs: position.rssi_dbfs,
    seen_s: position.seen_s, seen_pos_s: position.seen_pos_s,
    bearing_deg_true: geometry.bearing_deg_true, elevation_deg: geometry.elevation_deg,
    ground_range_km: geometry.ground_range_km, slant_range_km: geometry.slant_range_km,
    transmitter_range_km: geometry.transmitter_range_km,
    delay_km: prediction.delay, doppler_hz: prediction.doppler,
    frequency_hz: prediction.frequency_hz,
    delay_definition: prediction.delay_definition || 'excess_bistatic_path_km',
    delay_rate_km_s: prediction.delay_rate_km_s,
    doppler_rate_hz_s: prediction.doppler_rate_hz_s,
    provenance: prediction.provenance && typeof prediction.provenance === 'object' ?
      {...prediction.provenance} : null};
}

function truthIntegrity(prediction) {
  const provenance = prediction?.provenance;
  if (!provenance || typeof provenance !== 'object') return {valid: false, reason: 'missing-provenance'};
  const offset = provenance.observation_time_offset_s;
  if (!finite(offset) || Math.abs(offset) > 1) return {valid: false, reason: 'stale-observation'};
  if (!['identity', 'constant_velocity_ground_track'].includes(provenance.projection_model))
    return {valid: false, reason: 'invalid-projection'};
  const disagreement = provenance.maximum_pairwise_projected_disagreement_km;
  if (finite(disagreement) && disagreement > .5)
    return {valid: false, reason: 'source-disagreement'};
  return {valid: true, reason: null};
}

function frameAt(history, timestamp) {
  const exact = history.get(timestamp);
  if (exact) return exact;
  let nearest = null, distance = Infinity;
  for (const [at, frame] of history) if (Math.abs(at - timestamp) < distance) {
    nearest = frame; distance = Math.abs(at - timestamp);
  }
  return distance <= 100 ? nearest : null;
}

function rememberFrame(history, frame) {
  if (!finite(frame?.timestamp)) return;
  history.set(frame.timestamp, frame);
  while (history.size > MAX_RADAR_FRAMES) history.delete(history.keys().next().value);
}

function timingAt(history, timestamp) {
  let nearest = null, distance = Infinity;
  for (const item of history) if (Math.abs(item.timestamp_ms - timestamp) < distance) {
    nearest = item; distance = Math.abs(item.timestamp_ms - timestamp);
  }
  return distance <= 100 ? nearest : null;
}

function exactEventTime(frame) {
  const value = frame?.eventTime;
  return Boolean(value && value.exact === true &&
    value.uncertaintyBounded === true && value.uncertaintyNs === 0);
}

function updateTruth(pass, prediction, timestamp) {
  const snapshot = truthSnapshot(prediction, timestamp);
  if (!Array.isArray(pass.truthHistory)) pass.truthHistory = [];
  pass.truthHistory.push(snapshot);
  pass.truthHistory.sort((left, right) => left.timestamp_ms - right.timestamp_ms);
  if (pass.truthHistory.length > MAX_EVENT_TRUTH_SAMPLES)
    pass.truthHistory.splice(0, pass.truthHistory.length - MAX_EVENT_TRUTH_SAMPLES);
  pass.firstTruth = pass.truthHistory[0];
  pass.lastTruth = pass.truthHistory.at(-1);
  extent(pass, 'minAltitudeGeomFt', 'maxAltitudeGeomFt', snapshot.altitude_geom_ft);
  extent(pass, 'minElevationDeg', 'maxElevationDeg', snapshot.elevation_deg);
  extent(pass, 'minGroundRangeKm', 'maxGroundRangeKm', snapshot.ground_range_km);
  extent(pass, 'minSlantRangeKm', 'maxSlantRangeKm', snapshot.slant_range_km);
  extent(pass, 'minTransmitterRangeKm', 'maxTransmitterRangeKm', snapshot.transmitter_range_km);
  if (finite(snapshot.slant_range_km) && finite(snapshot.transmitter_range_km))
    extent(pass, 'minBistaticRangeProductKm2', 'maxBistaticRangeProductKm2',
      snapshot.slant_range_km * snapshot.transmitter_range_km);
  if (finite(snapshot.delay_rate_km_s)) pass.maxAbsDelayRateKmS = Math.max(
    pass.maxAbsDelayRateKmS || 0, Math.abs(snapshot.delay_rate_km_s));
  if (finite(snapshot.doppler_rate_hz_s)) pass.maxAbsDopplerRateHzS = Math.max(
    pass.maxAbsDopplerRateHzS || 0, Math.abs(snapshot.doppler_rate_hz_s));
  if (finite(snapshot.bearing_deg_true)) {
    if (!Array.isArray(pass.azimuthSectors45) || pass.azimuthSectors45.length !== 8)
      pass.azimuthSectors45 = Array(8).fill(0);
    pass.azimuthSectors45[Math.floor(((snapshot.bearing_deg_true % 360) + 360) % 360 / 45)] += 1;
  }
}

function cells(frame, delayScale = 1) {
  const value = parse(frame);
  if (!value) return [];
  if (Array.isArray(value.delay)) return value.delay.map((delay, index) =>
    finite(delay) && finite(value.doppler?.[index]) ?
      {index, rawDelay: delay, delay: delay * delayScale, doppler: value.doppler[index],
        snr: finite(value.snr?.[index]) ? value.snr[index] : null} : null).filter(Boolean);
  return (value.data || []).map((item, index) => finite(item?.delay) && finite(item?.doppler) ?
    {index, rawDelay: item.delay, delay: item.delay * delayScale, doppler: item.doppler,
      tracker: {id: item.id ?? null, state: item.state ?? null, source: item.source ?? null,
        confidence: item.confidence ?? null, associations: item.n ?? null}} : null).filter(Boolean);
}

function assignments(points, predictions, gatesForPrediction, kind, frameTimestamp,
  diagnostics = null) {
  const result = new Map();
  const predictionByHex = new Map(predictions);
  for (const point of points) {
    if (diagnostics) diagnostics.points = (diagnostics.points || 0) + 1;
    const candidates = predictions.map(([hex, prediction]) => {
      const gates = gatesForPrediction(prediction);
      const delayError = Math.abs(point.delay - prediction.delay);
      const dopplerError = Math.abs(point.doppler - prediction.doppler);
      return {hex, delayError, dopplerError, gates,
        score: (delayError / gates.delayKm) ** 2 + (dopplerError / gates.dopplerHz) ** 2};
    }).filter(item => item.delayError <= item.gates.delayKm &&
      item.dopplerError <= item.gates.dopplerHz)
      .sort((left, right) => left.score - right.score);
    if (diagnostics) diagnostics.candidate_matches =
      (diagnostics.candidate_matches || 0) + candidates.length;
    if (!candidates.length) {
      if (diagnostics) diagnostics.unmatched_points = (diagnostics.unmatched_points || 0) + 1;
      continue;
    }
    if (candidates.length > 1 && candidates[1].score <= candidates[0].score * 1.15 + .05) {
      if (diagnostics) diagnostics.ambiguous_points = (diagnostics.ambiguous_points || 0) + 1;
      continue;
    }
    const selected = candidates[0], prediction = predictionByHex.get(selected.hex);
    const prior = result.get(selected.hex);
    if (prior && prior.normalized_gate_score <= selected.score) continue;
    result.set(selected.hex, {kind, frame_timestamp_ms: frameTimestamp,
      cell_index: point.index, measured_delay_raw: point.rawDelay,
      measured_delay_km: point.delay, measured_doppler_hz: point.doppler,
      measured_snr_db: point.snr ?? null, tracker: point.tracker ?? null,
      predicted_delay_km: prediction?.delay ?? null,
      predicted_doppler_hz: prediction?.doppler ?? null,
      delay_residual_km: point.delay - (prediction?.delay ?? point.delay),
      doppler_residual_hz: point.doppler - (prediction?.doppler ?? point.doppler),
      gate_delay_km: selected.gates.delayKm, gate_doppler_hz: selected.gates.dopplerHz,
      event_time_uncertainty_s: selected.gates.eventTimeUncertaintyS || 0,
      normalized_gate_score: selected.score});
    if (diagnostics) diagnostics.selected_points = (diagnostics.selected_points || 0) + 1;
  }
  return result;
}

class EvidenceStatistics {
  constructor(config, adsbSource, {filename, fileSystem = fs, now = Date.now,
    setIntervalFn = setInterval, clearIntervalFn = clearInterval,
    onEligibilityEvent = () => {}, onPeriodicCapture = () => {},
    onTrackCapture = () => {}, timingPolicy = 'exact'} = {}) {
    this.config = config;
    this.adsbSource = adsbSource;
    this.exactTimingRequired = typeof adsbSource?.getAt === 'function';
    if (!['exact', 'provisional'].includes(timingPolicy))
      throw new Error('Evidence timing policy must be exact or provisional');
    this.timingPolicy = timingPolicy;
    this.fs = fileSystem;
    this.now = now;
    const recorderPath = config.process?.bearing?.manifold_recorder?.path;
    this.filename = filename || process.env.BLAH2_STATISTICS_PATH ||
      path.join(recorderPath ? path.dirname(recorderPath) : '/var/lib/vectorwarp-api',
        'evidence-statistics.json');
    this.key = siteKey(config);
    this.configEpoch = configEpoch(config);
    this.receiverEpoch = receiverEpoch(config);
    this.limits = limits(config);
    this.state = {schema: 'vectorwarp-evidence-statistics-v2', createdAt: now(),
      updatedAt: null, sites: {[this.key]: siteDescriptor(config)}, passes: [], active: {},
      eligibilityEvents: [], rollup: {siteKey: this.key,
        passes: 0, eligible: 0,
        detected: 0, tracked: 0, missed: 0, outOfConfig: 0,
        mapEligible: 0, detectorEligible: 0, truthUnusable: 0,
        eligibleNoRadar: 0, detectedNotTracked: 0},
      radarCoverage: {schema: 'vectorwarp-radar-coverage-accumulator-v1', buckets: {}},
      captureAdmission: {checked: 0, admitted: 0, quarantined: 0,
        missingAfterAdmission: 0, reasons: {}}, error: null};
    this.latest = {detection: null, tracker: null, bearing: null};
    this.detectionHistory = new Map(); this.trackerHistory = new Map();
    this.lastTimingObservedAt = null;
    this.eventTimeRejections = 0;
    this.lastEventTimeRejection = null;
    this.recentTiming = [];
    this.lastDetectionFrameTimestamp = null; this.lastTrackerFrameTimestamp = null;
    this.running = 0; this.pendingBuckets = new Set(); this.completedBuckets = new Set();
    this.lastPersistAt = 0; this.closed = false;
    this.clearInterval = clearIntervalFn;
    this.onEligibilityEvent = onEligibilityEvent;
    this.onPeriodicCapture = onPeriodicCapture;
    this.onTrackCapture = onTrackCapture;
    this.load();
    this.scoreContractHash = digest(evaluationContract(this.limits, this.timingPolicy));
    if (this.state.liveScoreContract?.hash !== this.scoreContractHash) {
      for (const hex of Object.keys(this.state.active)) this.finish(hex);
      this.state.liveScoreContract = {hash: this.scoreContractHash, startedAt: now()};
      this.persist();
    }
    this.sampler = null;
    if (this.limits && this.config.truth?.adsb?.enabled === true) {
      this.sampler = setIntervalFn(() => {
        const timestamp = this.now();
        const staleAfter = Math.max(TRUTH_SAMPLE_MS * 2, this.limits.cpiSeconds * 2000);
        if (!this.exactTimingRequired &&
            (!finite(this.lastTimingObservedAt) || timestamp - this.lastTimingObservedAt > staleAfter))
          this.queue(timestamp);
      }, TRUTH_SAMPLE_MS);
      if (typeof this.sampler?.unref === 'function') this.sampler.unref();
    }
  }

  load() {
    try {
      const stat = this.fs.statSync(this.filename);
      if (!stat.isFile() || stat.size > MAX_STATE_INPUT_BYTES)
        throw new Error('Statistics file is invalid or exceeds the 256 MiB migration limit');
      const value = JSON.parse(this.fs.readFileSync(this.filename, 'utf8'));
      if (!['vectorwarp-evidence-statistics-v1', this.state.schema].includes(value?.schema) || !Array.isArray(value.passes) ||
          !value.active || typeof value.active !== 'object') throw new Error('Statistics file has an invalid schema');
      this.state = {...this.state, ...value, schema: this.state.schema,
        eligibilityEvents: Array.isArray(value.eligibilityEvents) ? value.eligibilityEvents : [],
        sites: {...(value.sites || {}), [this.key]: siteDescriptor(this.config)}, error: null};
      this.state.captureAdmission ||= {checked: 0, admitted: 0, quarantined: 0,
        missingAfterAdmission: 0, reasons: {}};
      this.state.associationDiagnostics ||= {detection: {}, tracker: {}};
      this.state.radarCoverage ||= {schema: 'vectorwarp-radar-coverage-accumulator-v1', buckets: {}};
      this.state.radarCoverage.buckets ||= {};
      this.state.captureAdmission.missingAfterAdmission ||= 0;
      // Capture requests are intentionally process-local and bounded. After an
      // API restart no native recorder can still acknowledge the old queue, so
      // never leave its evidence ledger permanently claiming a pending clip.
      for (const event of this.state.eligibilityEvents) {
        if (event.capture_state !== 'trigger_pending') continue;
        event.capture_state = 'failed';
        event.capture_failure = 'API restarted before capture acknowledgement';
        for (const window of event.capture_windows || [])
          if (window.state === 'pending') {
            window.state = 'failed'; window.failure = event.capture_failure;
          }
      }
      for (const pass of [...this.state.passes, ...Object.values(this.state.active)])
        if (!pass.id) pass.id = passId(pass.siteKey, pass.hex, pass.startedAt);
      for (const pass of Object.values(this.state.active)) compactActivePass(pass);
      for (const event of this.state.eligibilityEvents) compactEligibilityEvent(event);
      this.trimEligibilityEvents();
      if (this.state.rollup?.siteKey !== this.key) this.state.rollup = {
        siteKey: this.key, passes: 0, eligible: 0, detected: 0, tracked: 0,
        missed: 0, outOfConfig: 0, mapEligible: 0, detectorEligible: 0, truthUnusable: 0,
        eligibleNoRadar: 0, detectedNotTracked: 0};
      for (const pass of this.state.passes) compactFinalizedPass(pass);
      while (this.state.passes.length > MAX_PASSES)
        accumulateRollup(this.state, this.key, this.state.passes.shift());
      this.trimEligibilityEvents();
    } catch (error) {
      if (error.code !== 'ENOENT') this.state.error = error.message;
    }
  }

  acceptDetection(frame) {
    this.latest.detection = parse(frame);
    rememberFrame(this.detectionHistory, this.latest.detection);
    if (!this.exactTimingRequired)
      this.queue(finite(this.latest.detection?.timestamp) ? this.latest.detection.timestamp : this.now());
  }
  acceptTracker(frame) {
    this.latest.tracker = parse(frame); rememberFrame(this.trackerHistory, this.latest.tracker);
  }
  acceptBearing(frame) { this.latest.bearing = parse(frame); }

  acceptTiming(frame) {
    const value = parse(frame);
    const exact = exactEventTime(value);
    const provisionalEstimate = value?.eventTime?.exact === false &&
      value.eventTime.estimateAvailable === true && finite(value.timestamp);
    if (!exact) {
      this.eventTimeRejections += 1;
      this.lastEventTimeRejection = {
        timestamp_ms: finite(value?.timestamp) ? value.timestamp : null,
        source: typeof value?.eventTime?.source === 'string' ?
          value.eventTime.source.slice(0, 128) : 'invalid',
        reason: typeof value?.eventTime?.reason === 'string' ?
          value.eventTime.reason.slice(0, 240) :
          'Event-time provenance is missing or not exact'};
      if (this.timingPolicy !== 'provisional' || !provisionalEstimate) return;
    }
    if (!finite(value?.timestamp) || value.captureSamplePositionAvailable !== true ||
        !Number.isSafeInteger(value.captureEpoch) ||
        !Number.isSafeInteger(value.captureFirstEpochSample) ||
        !Number.isSafeInteger(value.captureEndEpochSample) ||
        value.captureEndEpochSample <= value.captureFirstEpochSample) return;
    const item = {timestamp_ms: value.timestamp, epoch: value.captureEpoch,
      first_epoch_sample: value.captureFirstEpochSample,
      end_epoch_sample: value.captureEndEpochSample,
      exact_event_time: exact,
      event_time_source: typeof value?.eventTime?.source === 'string' ?
        value.eventTime.source.slice(0, 128) : (exact ? 'legacy_exact_contract' : 'unknown'),
      event_time_uncertainty_bounded: value?.eventTime?.uncertaintyBounded === true,
      event_time_uncertainty_s: exact ? 0 : this.limits.provisionalTimeUncertaintyS,
      event_time_uncertainty_model: exact ? 'exact' :
        'bounded-configured-nuisance-radius',
      capture_dropped_samples: Array.isArray(value.captureDroppedSamples) ?
        value.captureDroppedSamples.filter(Number.isSafeInteger) : []};
    const duplicate = this.recentTiming.some(existing =>
      existing.timestamp_ms === item.timestamp_ms && existing.epoch === item.epoch &&
      existing.first_epoch_sample === item.first_epoch_sample);
    if (!duplicate) this.recentTiming.push(item);
    this.lastTimingObservedAt = this.now();
    const cutoff = value.timestamp - TIMING_RETENTION_MS;
    this.recentTiming = this.recentTiming.filter(existing => existing.timestamp_ms >= cutoff)
      .slice(-MAX_TIMING_FRAMES);
    this.queue(item.timestamp_ms);
  }

  queue(timestamp = this.now()) {
    if (this.closed || !this.limits || this.config.truth?.adsb?.enabled !== true) return;
    const bucket = Math.floor(timestamp / TRUTH_SAMPLE_MS);
    if (this.pendingBuckets.has(bucket) || this.completedBuckets.has(bucket) ||
        this.running >= MAX_PENDING_SAMPLES) return;
    this.pendingBuckets.add(bucket);
    void this.sample(timestamp);
  }

  async sample(timestamp = this.now()) {
    this.running += 1;
    const bucket = Math.floor(timestamp / TRUTH_SAMPLE_MS);
    try {
      const predictions = this.adsbSource.getAt ?
        await this.adsbSource.getAt('delayDoppler', timestamp) :
        await this.adsbSource.get('delayDoppler');
      const detectionFrame = frameAt(this.detectionHistory, timestamp);
      const trackerFrame = frameAt(this.trackerHistory, timestamp);
      const detectionTimestamp = detectionFrame?.timestamp;
      const detectionCells = detectionFrame ? cells(detectionFrame, 1) : [];
      const trackerTimestamp = trackerFrame?.timestamp;
      const trackerCells = trackerFrame ? cells(trackerFrame, this.limits.kmPerBin)
        .filter(point => point.tracker?.state === 'ACTIVE' &&
          Number(point.tracker?.associations) >= 3) : [];
      const valid = Object.entries(predictions || {}).map(([rawHex, prediction]) =>
        [normalizeHex(rawHex), prediction]).filter(([hex, prediction]) => hex &&
          finite(prediction?.delay) && finite(prediction?.doppler));
      const timing = timingAt(this.recentTiming, timestamp);
      const timingUncertaintyS = this.timingPolicy === 'provisional' &&
        timing?.exact_event_time === false ? timing.event_time_uncertainty_s : 0;
      const detectionDiagnostics = {}, trackerDiagnostics = {};
      const detected = assignments(detectionCells, valid, prediction =>
        associationGates(this.limits, prediction, timestamp, 'detection',
          timingUncertaintyS),
      'detection', detectionTimestamp, detectionDiagnostics);
      const tracked = assignments(trackerCells, valid, prediction =>
        associationGates(this.limits, prediction, timestamp, 'tracker',
          timingUncertaintyS),
      'tracker', trackerTimestamp, trackerDiagnostics);
      this.state.associationDiagnostics ||= {detection: {}, tracker: {}};
      for (const [kind, values] of [['detection', detectionDiagnostics],
        ['tracker', trackerDiagnostics]]) for (const [name, value] of Object.entries(values))
        this.state.associationDiagnostics[kind][name] =
          (this.state.associationDiagnostics[kind][name] || 0) + value;
      for (const [hex, prediction] of valid) {
        this.observe(hex, prediction, timestamp,
          detected.get(hex) || null, tracked.get(hex) || null,
          timing);
      }
      this.expire(timestamp);
      this.state.updatedAt = this.now(); this.state.error = null;
      if (this.now() - this.lastPersistAt >= 30000) this.persist();
    } catch (error) { this.state.error = error.message; }
    finally {
      this.running -= 1; this.pendingBuckets.delete(bucket); this.completedBuckets.add(bucket);
      while (this.completedBuckets.size > MAX_RADAR_FRAMES)
        this.completedBuckets.delete(this.completedBuckets.values().next().value);
    }
  }

  observe(hex, prediction, timestamp, detectedAssociation, trackedAssociation, timing = null) {
    let pass = this.state.active[hex];
    if (pass && (timestamp - pass.lastAt > PASS_GAP_MS || pass.siteKey !== this.key)) {
      this.finish(hex); pass = null;
    }
    if (!pass) pass = this.state.active[hex] = emptyPass(hex, prediction.flight, timestamp, this.key);
    const bucket = Math.floor(timestamp / TRUTH_SAMPLE_MS);
    pass.sampledBuckets ||= [];
    if (pass.sampledBuckets.includes(bucket)) return;
    pass.sampledBuckets.push(bucket);
    if (pass.sampledBuckets.length > MAX_CPI_AUDIT) pass.sampledBuckets.shift();
    pass.startedAt = Math.min(pass.startedAt, timestamp);
    pass.lastAt = Math.max(pass.lastAt, timestamp); pass.frames += 1;
    const delay = prediction.delay, doppler = prediction.doppler;
    updateExtent(pass, delay, doppler);
    updateTruth(pass, prediction, timestamp);
    if (trackedAssociation) rememberRadarCoverage(this.state, pass, pass.lastTruth);
    const delayBin = delay / this.limits.kmPerBin;
    const delayInMap = delayBin >= this.limits.delayMinBin && delayBin <= this.limits.delayMaxBin;
    const aboveFloor = delayBin >= this.limits.detectionMinDelayBin;
    const dopplerInMap = doppler >= this.limits.dopplerMinHz && doppler <= this.limits.dopplerMaxHz;
    const physical = delayInMap && dopplerInMap;
    const detectorEligible = physical && aboveFloor;
    if (physical) pass.physicalEligibleFrames += 1;
    if (detectorEligible) pass.detectorEligibleFrames = (pass.detectorEligibleFrames || 0) + 1;
    if (!aboveFloor) pass.belowFloorFrames += 1;
    if (!delayInMap && delayBin > this.limits.delayMaxBin) pass.aboveDelayFrames += 1;
    if (!dopplerInMap) pass.outsideDopplerFrames += 1;
    if (this.limits.clutterMaxBin !== null && delayBin <= this.limits.clutterMaxBin)
      pass.clutterCancelledFrames += 1;
    else if (physical) pass.outsideClutterFrames += 1;
    const integrity = truthIntegrity(prediction);
    const age = finite(prediction.timestamp) ? Math.max(0, timestamp / 1000 - prediction.timestamp) : Infinity;
    pass.cpiAudit ||= [];
    pass.cpiAudit.push({timestamp_ms: timestamp, timing, truth_valid: integrity.valid,
      truth_reason: integrity.reason, predicted_delay_km: delay, predicted_doppler_hz: doppler,
      map_eligible: physical, detector_eligible: detectorEligible,
      physical_eligible: physical, detected: Boolean(detectedAssociation),
      stable_tracked: Boolean(trackedAssociation)});
    pass.cpiAudit.sort((left, right) => left.timestamp_ms - right.timestamp_ms);
    if (pass.cpiAudit.length > MAX_CPI_AUDIT) pass.cpiAudit.shift();
    // Once a capture has been triggered, retain fresh event-time truth through
    // its full raw-IQ window even if the target crosses a delay/Doppler boundary.
    // Eligibility still controls scoring; this only prevents the recorder's
    // post-roll from becoming unverifiable at the edge of the processing map.
    let event = pass.eligibilityEventId ?
      this.state.eligibilityEvents.find(item => item.id === pass.eligibilityEventId) : null;
    if (event && age <= this.limits.maxTruthAgeS) {
      if (!Array.isArray(event.truth_samples)) event.truth_samples = [];
      if (event.truth_samples.at(-1)?.timestamp_ms !== timestamp)
        event.truth_samples.push(truthSnapshot(prediction, timestamp));
      event.truth_samples.sort((left, right) => left.timestamp_ms - right.timestamp_ms);
      if (event.truth_samples.length > MAX_EVENT_TRUTH_SAMPLES)
        event.truth_samples.splice(0, event.truth_samples.length - MAX_EVENT_TRUTH_SAMPLES);
      for (const window of event.capture_windows || []) window.truth_samples =
        event.truth_samples.filter(sample => sample.timestamp_ms >= window.window_start - 3000 &&
          sample.timestamp_ms <= window.window_end + 3000);
    }
    if (age > this.limits.maxTruthAgeS || !integrity.valid) pass.staleTruthFrames += 1;
    if (this.exactTimingRequired && !timing) pass.missingTimingFrames += 1;
    if (!detectorEligible || age > this.limits.maxTruthAgeS || !integrity.valid ||
        (this.exactTimingRequired && !timing)) return;
    pass.eligibleFrames += 1;
    const bandDescriptor = delayBand(this.limits, delay);
    if (bandDescriptor) {
      pass.delayBands ||= {};
      const band = pass.delayBands[bandDescriptor.id] ||= {...bandDescriptor,
        eligibleFrames: 0, detectedFrames: 0, trackedFrames: 0};
      band.eligibleFrames += 1;
      if (detectedAssociation) band.detectedFrames += 1;
      if (trackedAssociation) band.trackedFrames += 1;
    }
    const slantDescriptor = fixedBand(SLANT_RANGE_EDGES_KM, pass.lastTruth?.slant_range_km);
    if (slantDescriptor) {
      pass.slantRangeBands ||= {};
      const band = pass.slantRangeBands[slantDescriptor.id] ||= {...slantDescriptor,
        eligibleFrames: 0, detectedFrames: 0, trackedFrames: 0};
      band.eligibleFrames += 1;
      if (detectedAssociation) band.detectedFrames += 1;
      if (trackedAssociation) band.trackedFrames += 1;
    }
    if (bandDescriptor && slantDescriptor) {
      pass.geometryBands ||= {};
      const id = `${bandDescriptor.id}|${slantDescriptor.id}`;
      const band = pass.geometryBands[id] ||= {delay_band: bandDescriptor.id,
        slant_range_band: slantDescriptor.id, eligibleFrames: 0,
        detectedFrames: 0, trackedFrames: 0};
      band.eligibleFrames += 1;
      if (detectedAssociation) band.detectedFrames += 1;
      if (trackedAssociation) band.trackedFrames += 1;
    }
    const elevationDescriptor = fixedBand(ELEVATION_EDGES_DEG, pass.lastTruth?.elevation_deg);
    if (elevationDescriptor) {
      pass.elevationBands ||= {};
      const band = pass.elevationBands[elevationDescriptor.id] ||= {...elevationDescriptor,
        eligibleFrames: 0, detectedFrames: 0, trackedFrames: 0};
      band.eligibleFrames += 1;
      if (detectedAssociation) band.detectedFrames += 1;
      if (trackedAssociation) band.trackedFrames += 1;
    }
    const azimuthDescriptor = azimuthBand(pass.lastTruth?.bearing_deg_true);
    if (azimuthDescriptor) {
      pass.azimuthBands ||= {};
      const band = pass.azimuthBands[azimuthDescriptor.id] ||= {...azimuthDescriptor,
        eligibleFrames: 0, detectedFrames: 0, trackedFrames: 0};
      band.eligibleFrames += 1;
      if (detectedAssociation) band.detectedFrames += 1;
      if (trackedAssociation) band.trackedFrames += 1;
    }
    if (azimuthDescriptor && elevationDescriptor) {
      pass.azimuthElevationBands ||= {};
      const id = `${azimuthDescriptor.id}|${elevationDescriptor.id}`;
      const band = pass.azimuthElevationBands[id] ||= {
        azimuth_band: azimuthDescriptor.id, elevation_band: elevationDescriptor.id,
        eligibleFrames: 0, detectedFrames: 0, trackedFrames: 0};
      band.eligibleFrames += 1;
      if (detectedAssociation) band.detectedFrames += 1;
      if (trackedAssociation) band.trackedFrames += 1;
    }
    if (!pass.eligibilityEventId) this.emitEligibility(pass, prediction, timestamp);
    event = this.state.eligibilityEvents.find(item => item.id === pass.eligibilityEventId);
    if (event) {
      if (!Array.isArray(event.truth_samples)) event.truth_samples = [];
      if (event.truth_samples.at(-1)?.timestamp_ms !== timestamp)
        event.truth_samples.push(truthSnapshot(prediction, timestamp));
      event.truth_samples.sort((left, right) => left.timestamp_ms - right.timestamp_ms);
      if (event.truth_samples.length > MAX_EVENT_TRUTH_SAMPLES)
        event.truth_samples.splice(0, event.truth_samples.length - MAX_EVENT_TRUTH_SAMPLES);
      for (const window of event.capture_windows || []) window.truth_samples =
        event.truth_samples.filter(sample => sample.timestamp_ms >= window.window_start - 3000 &&
          sample.timestamp_ms <= window.window_end + 3000);
    }
    const detectedAt = associationTimestamp(detectedAssociation);
    const trackedAt = associationTimestamp(trackedAssociation);
    if (finite(detectedAt)) {
      pass.detectedFrames += 1;
      pass.firstDetectionAt = finite(pass.firstDetectionAt) ?
        Math.min(pass.firstDetectionAt, detectedAt) : detectedAt;
      if (!Array.isArray(pass.detectionTimestamps)) pass.detectionTimestamps = [];
      appendBounded(pass.detectionTimestamps, detectedAt);
      if (!Array.isArray(pass.detectionAssociations)) pass.detectionAssociations = [];
      appendAssociation(pass.detectionAssociations, detectedAssociation);
    }
    const firstTrack = finite(trackedAt) && !finite(pass.firstTrackAt);
    if (finite(trackedAt)) {
      pass.trackedFrames += 1;
      pass.firstTrackAt = finite(pass.firstTrackAt) ? Math.min(pass.firstTrackAt, trackedAt) : trackedAt;
      if (!Array.isArray(pass.trackTimestamps)) pass.trackTimestamps = [];
      appendBounded(pass.trackTimestamps, trackedAt);
      if (!Array.isArray(pass.trackerAssociations)) pass.trackerAssociations = [];
      appendAssociation(pass.trackerAssociations, trackedAssociation);
    }
    updateCaptureWindowEvidence(event, pass, detectedAssociation, trackedAssociation);
    if (firstTrack) this.emitTrackCapture(event, pass, trackedAt);
    this.emitPeriodicCapture(event, pass, timestamp);
  }

  emitEligibility(pass, prediction, timestamp) {
    const geometry = prediction.geometry || {};
    const event = {id: `eligibility:${pass.id}`, pass_id: pass.id, hex: pass.hex,
      flight: pass.flight, state: 'pending', outcome: null, observed_at: timestamp,
      finalized_at: null, receiver_epoch: this.receiverEpoch,
      config_epoch: this.configEpoch, site_epoch: this.key,
      receiver: {type: this.config.capture?.device?.type || null,
        serial: this.config.capture?.device?.serial || null},
      predicted: {delay_km: prediction.delay, doppler_hz: prediction.doppler,
        bearing_deg_true: geometry.bearing_deg_true ?? null,
        elevation_deg: geometry.elevation_deg ?? null,
        ground_range_km: geometry.ground_range_km ?? null,
        slant_range_km: geometry.slant_range_km ?? null,
        transmitter_range_km: geometry.transmitter_range_km ?? null},
      truth_samples: (pass.truthHistory || []).slice(),
      capture_windows: [],
      capture_state: 'not_integrated', eligible_frames: 1,
      detected_frames: 0, tracked_frames: 0};
    pass.eligibilityEventId = event.id;
    this.state.eligibilityEvents.push(event);
    this.trimEligibilityEvents();
    try {
      const capture = this.onEligibilityEvent({...event});
      if (capture?.accepted === true) {
        event.capture_state = capture.dropped === true ? 'dropped' : 'trigger_pending';
        if (capture.dropped === true) event.capture_failure = capture.reason || 'unspecified';
        if (capture.clip?.clipRevision) {
          event.capture_clip_revisions = [capture.clip.clipRevision];
          event.capture_window_start = capture.clip.windowStart;
          event.capture_window_end = capture.clip.windowEnd;
          upsertCaptureWindow(event, {captureId: capture.clip.captureIds?.[event.id], kind: 'initial',
            revision: capture.clip.clipRevision, requestedEventTime: capture.clip.requestedEventTime,
            windowStart: capture.clip.windowStart, windowEnd: capture.clip.windowEnd,
            state: capture.dropped === true ? 'dropped' : 'pending', failure: capture.reason || null,
            file: '', samples: 0}, pass);
        }
      }
    }
    catch (error) { this.state.error = error.message; }
  }

  emitPeriodicCapture(event, pass, timestamp) {
    if (!event || event.state !== 'pending') return;
    try {
      const capture = this.onPeriodicCapture({...event}, timestamp);
      if (!capture?.accepted) return;
      if (capture.dropped && capture.window) {
        upsertCaptureWindow(event, capture.window, pass);
        return;
      }
      if (!capture.clip?.clipRevision) return;
      event.capture_clip_revisions = [...new Set([...(event.capture_clip_revisions || []),
        capture.clip.clipRevision])];
      upsertCaptureWindow(event, {captureId: capture.clip.captureIds?.[event.id], kind: 'periodic',
        revision: capture.clip.clipRevision, requestedEventTime: capture.clip.requestedEventTime,
        windowStart: capture.clip.windowStart, windowEnd: capture.clip.windowEnd,
        state: 'pending', failure: null, file: '', samples: 0}, pass);
    } catch (error) { this.state.error = error.message; }
  }

  emitTrackCapture(event, pass, timestamp) {
    if (!event || event.state !== 'pending') return;
    try {
      const capture = this.onTrackCapture({...event}, timestamp);
      if (!capture?.accepted) return;
      if (capture.dropped && capture.window) {
        upsertCaptureWindow(event, capture.window, pass);
        return;
      }
      if (!capture.clip?.clipRevision) return;
      event.capture_clip_revisions = [...new Set([...(event.capture_clip_revisions || []),
        capture.clip.clipRevision])];
      upsertCaptureWindow(event, {captureId: capture.clip.captureIds?.[event.id], kind: 'track',
        revision: capture.clip.clipRevision, requestedEventTime: capture.clip.requestedEventTime,
        windowStart: capture.clip.windowStart, windowEnd: capture.clip.windowEnd,
        state: 'pending', failure: null, file: '', samples: 0}, pass);
    } catch (error) { this.state.error = error.message; }
  }

  trimEligibilityEvents() {
    while (this.state.eligibilityEvents.length > MAX_ELIGIBILITY_EVENTS) {
      const finalized = this.state.eligibilityEvents.findIndex(event => event.state === 'finalized');
      this.state.eligibilityEvents.splice(finalized >= 0 ? finalized : 0, 1);
    }
  }

  acceptCaptureEvents(records) {
    if (!Array.isArray(records)) return;
    const byId = new Map(records.map(record => [record?.eventId, record]));
    for (const event of this.state.eligibilityEvents) {
      const record = byId.get(event.id);
      if (!record) continue;
      event.capture_clip_revisions = Array.isArray(record.clipRevisions) ?
        [...record.clipRevisions] : [];
      const pass = Object.values(this.state.active).find(item => item.id === event.pass_id) ||
        this.state.passes.find(item => item.id === event.pass_id);
      for (const window of record.captureWindows || []) upsertCaptureWindow(event, window, pass);
      updateCaptureWindowEvidence(event, pass);
      const initial = (record.captureWindows || []).find(window => window.kind === 'initial');
      if (initial) updateLegacyCapture(event, initial);
      else updateLegacyCapture(event, record);
    }
    this.state.updatedAt = this.now();
    this.persist();
  }

  prepareCaptureAdmissions(records, revision = null) {
    const result = {};
    const cpiMs = (this.limits?.cpiSeconds || .5) * 1000;
    for (const record of records || []) for (const window of record.captureWindows || []) {
      if (!window.captureId || result[window.captureId] || window.revision === null ||
          (revision !== null && window.revision !== revision)) continue;
      const event = this.state.eligibilityEvents.find(item => item.id === record.eventId);
      const truth = (event?.truth_samples || []).filter(sample =>
        sample.timestamp_ms >= window.windowStart - 3000 && sample.timestamp_ms <= window.windowEnd + 3000);
      const processorCpis = this.recentTiming.filter(item =>
        item.timestamp_ms >= window.windowStart && item.timestamp_ms <= window.windowEnd);
      const admission = validateTruthWindow(truth, window.windowStart, window.windowEnd,
        {cpiMs, cpiTimestampsMs: processorCpis.map(item => item.timestamp_ms)});
      const exactTiming = processorCpis.length > 0 &&
        processorCpis.every(item => item.exact_event_time === true);
      result[window.captureId] = {...admission,
        timing_policy: exactTiming ? 'exact' : 'provisional',
        decision_grade: admission.valid && exactTiming,
        event_time_uncertainty_s: exactTiming ? 0 : this.limits.provisionalTimeUncertaintyS,
        processor_cpis: processorCpis.map(item => {
        const bracket = admission.cpis.find(cpi => cpi.timestamp_ms === item.timestamp_ms) || {};
        return {...item, truth_left_timestamp_ms: bracket.left_timestamp_ms ?? null,
          truth_right_timestamp_ms: bracket.right_timestamp_ms ?? null,
          truth_bracket_span_ms: bracket.bracket_span_ms ?? null,
          max_raw_observation_offset_s: bracket.max_raw_observation_offset_s ?? null};
      })};
    }
    return result;
  }

  recordCaptureAdmissions(admissions) {
    for (const admission of Object.values(admissions || {})) {
      this.state.captureAdmission.checked += 1;
      if (admission.valid) this.state.captureAdmission.admitted += 1;
      else {
        this.state.captureAdmission.quarantined += 1;
        for (const reason of admission.reasons || [])
          this.state.captureAdmission.reasons[reason] = (this.state.captureAdmission.reasons[reason] || 0) + 1;
      }
    }
    this.persist();
  }

  auditRetainedCaptures() {
    for (const event of this.state.eligibilityEvents.slice(-250)) for (const window of event.capture_windows || []) {
      if (window.admission_state !== 'admitted' || !window.file) continue;
      if (this.fs.existsSync(window.file) && this.fs.existsSync(`${window.file}.json`) &&
          this.fs.existsSync(`${window.file}.admission.json`)) continue;
      window.admission_state = 'quarantined';
      window.admission_reasons = [...new Set([...(window.admission_reasons || []),
        'recording-missing-after-admission'])];
      this.state.captureAdmission.missingAfterAdmission += 1;
      this.state.captureAdmission.quarantined += 1;
      this.state.captureAdmission.reasons['recording-missing-after-admission'] =
        (this.state.captureAdmission.reasons['recording-missing-after-admission'] || 0) + 1;
    }
  }

  finish(hex) {
    const pass = this.state.active[hex];
    if (!pass) return;
    delete pass.sampledBuckets;
    const event = pass.eligibilityEventId && this.state.eligibilityEvents
      .find(item => item.id === pass.eligibilityEventId);
    if (event) {
      Object.assign(event, {state: 'finalized', outcome: eligibilityOutcome(pass),
        finalized_at: this.now(), eligible_frames: pass.eligibleFrames,
        detected_frames: pass.detectedFrames, tracked_frames: pass.trackedFrames});
      updateCaptureWindowEvidence(event, pass);
    }
    delete pass.detectionTimestamps;
    delete pass.trackTimestamps;
    compactFinalizedPass(pass);
    this.state.passes.push(pass); delete this.state.active[hex];
    while (this.state.passes.length > MAX_PASSES) {
      accumulateRollup(this.state, this.key, this.state.passes.shift());
    }
  }

  expire(timestamp) {
    for (const [hex, pass] of Object.entries(this.state.active))
      if (timestamp - pass.lastAt > PASS_GAP_MS) this.finish(hex);
  }

  persist() {
    try {
      // Compact before stringify as well as during load.  This bounds both the
      // live heap and the synchronous atomic-write pause if a source briefly
      // adds aircraft faster than normal expiry can finalize them.
      for (const pass of Object.values(this.state.active)) compactActivePass(pass);
      for (const event of this.state.eligibilityEvents) compactEligibilityEvent(event);
      this.fs.mkdirSync(path.dirname(this.filename), {recursive: true});
      const temporary = `${this.filename}.tmp-${process.pid}`;
      this.fs.writeFileSync(temporary, `${JSON.stringify(this.state)}\n`, {mode: 0o640});
      this.fs.renameSync(temporary, this.filename); this.lastPersistAt = this.now();
    } catch (error) { this.state.error = error.message; }
  }

  summarize(duration, timestamp, minimumStartedAt = -Infinity) {
    const cutoff = duration === Infinity ? -Infinity : timestamp - duration;
    const rows = this.state.passes
      .filter(pass => pass.siteKey === this.key && pass.lastAt >= cutoff &&
        pass.startedAt >= minimumStartedAt);
    const active = Object.values(this.state.active)
      .filter(pass => pass.siteKey === this.key && pass.lastAt >= cutoff &&
        pass.startedAt >= minimumStartedAt);
    const observed = rows.concat(active);
    const eligible = rows.filter(pass => pass.eligibleFrames >= 3);
    const detected = eligible.filter(pass => pass.detectedFrames >= 3);
    const tracked = eligible.filter(pass => pass.trackedFrames >= 3);
    const detectorFrames = pass => pass.detectorEligibleFrames ?? pass.physicalEligibleFrames ?? 0;
    const outOfConfig = rows.filter(pass => detectorFrames(pass) < 3);
    const truthUnusable = rows.filter(pass => detectorFrames(pass) >= 3 &&
      pass.eligibleFrames < 3);
    const eligibleNoRadar = eligible.filter(pass => pass.detectedFrames < 3 &&
      pass.trackedFrames < 3);
    const detectedNotTracked = eligible.filter(pass => pass.detectedFrames >= 3 &&
      pass.trackedFrames < 3);
    const base = duration === Infinity && minimumStartedAt === -Infinity ? this.state.rollup : {passes: 0, eligible: 0,
      detected: 0, tracked: 0, missed: 0, outOfConfig: 0, truthUnusable: 0,
      mapEligible: 0, detectorEligible: 0, eligibleNoRadar: 0, detectedNotTracked: 0};
    const eligibleCount = eligible.length + base.eligible;
    const detectedCount = detected.length + base.detected;
    const trackedCount = tracked.length + base.tracked;
    return {passes: rows.length + base.passes, active_passes: active.length,
      provisional_eligible_passes: active.filter(pass => pass.eligibleFrames >= 3).length,
      eligible_passes: eligibleCount,
      out_of_config_passes: outOfConfig.length + (base.outOfConfig || 0),
      truth_unusable_passes: truthUnusable.length + (base.truthUnusable || 0),
      detected_passes: detectedCount, tracked_passes: trackedCount,
      missed_passes: eligibleCount - trackedCount,
      eligible_no_radar_return: eligibleNoRadar.length + (base.eligibleNoRadar || 0),
      detected_not_tracked: detectedNotTracked.length + (base.detectedNotTracked || 0),
      detection_yield: ratio(detectedCount, eligibleCount),
      tracking_yield: ratio(trackedCount, eligibleCount),
      denominator_semantics: 'finalized passes with >=3 detector-searchable, fresh-truth CPIs',
      map_eligible_passes: rows.filter(pass => pass.physicalEligibleFrames >= 3).length +
        (base.mapEligible || 0),
      detector_eligible_passes: rows.filter(pass => detectorFrames(pass) >= 3).length +
        (base.detectorEligible || 0),
      delay_strata: summarizeDelayBands(rows, this.limits),
      receiver_slant_range_strata: {definition: 'receiver_to_aircraft_3d_slant_range_km',
        edges_km: SLANT_RANGE_EDGES_KM, finalized_passes_only: true,
        bands: summarizeBands(rows, 'slantRangeBands')},
      elevation_strata: {definition: 'receiver_geometric_elevation_deg',
        edges_deg: ELEVATION_EDGES_DEG, finalized_passes_only: true,
        bands: summarizeBands(rows, 'elevationBands')},
      azimuth_strata: {definition: 'receiver_true_azimuth_deg',
        edges_deg: AZIMUTH_EDGES_DEG, finalized_passes_only: true,
        bands: summarizeBands(rows, 'azimuthBands')},
      azimuth_elevation_strata: {definition: 'receiver_true_azimuth_x_geometric_elevation',
        finalized_passes_only: true, bands: summarizeBands(rows, 'azimuthElevationBands')},
      eligible_aircraft: new Set(eligible.map(pass => pass.hex)).size,
      opportunity_frames: observed.reduce((sum, pass) => sum + pass.frames, 0),
      eligible_frames: observed.reduce((sum, pass) => sum + pass.eligibleFrames, 0),
      out_of_span: {
        below_detection_floor: observed.reduce((sum, pass) => sum + pass.belowFloorFrames, 0),
        above_delay_ceiling: observed.reduce((sum, pass) => sum + pass.aboveDelayFrames, 0),
        outside_doppler_span: observed.reduce((sum, pass) => sum + pass.outsideDopplerFrames, 0),
        stale_truth: observed.reduce((sum, pass) => sum + pass.staleTruthFrames, 0),
        missing_exact_timing: observed.reduce((sum, pass) => sum + (pass.missingTimingFrames || 0), 0),
        beyond_clutter_cancellation: observed.reduce((sum, pass) =>
          sum + (pass.outsideClutterFrames || 0), 0)} };
  }

  status(bearing = {}) {
    this.auditRetainedCaptures();
    const timestamp = this.now();
    const live = this.latest.bearing?.data || [];
    return {schema: this.state.schema, site_key: this.key, updated_at: this.state.updatedAt,
      collecting: this.config.truth?.adsb?.enabled === true && Boolean(this.limits),
      error: this.state.error, limits: this.limits,
      eligibility_event_count: this.state.eligibilityEvents.length,
      capture_admission: this.state.captureAdmission,
      event_time: {rejections: this.eventTimeRejections,
        last_rejection: this.lastEventTimeRejection},
      eligibility_events: this.state.eligibilityEvents.slice(-250),
      windows: Object.fromEntries(Object.entries(WINDOWS).map(([name, duration]) =>
        [name, this.summarize(duration, timestamp)])),
      bearing: {...bearing, live: {tracks: live.length,
        publishable: live.filter(item => item.publishable === true).length,
        resolved: live.filter(item => item.resolved === true).length,
        stationary: live.filter(item => item.stationary_clutter === true).length}}};
  }

  liveStatus() {
    const timestamp = this.now();
    const contractStartedAt = this.state.liveScoreContract?.startedAt || timestamp;
    const active = Object.values(this.state.active).filter(pass => pass.siteKey === this.key &&
      pass.startedAt >= contractStartedAt)
      .map(pass => ({pass_id: pass.id, hex: pass.hex, flight: pass.flight,
        started_at: pass.startedAt, last_at: pass.lastAt,
        status: pass.trackedFrames >= 3 ? 'provisional_tracked' :
          pass.detectedFrames >= 3 ? 'provisional_detected' :
            pass.eligibleFrames >= 3 ? 'provisional_miss' : 'collecting',
        eligible_cpis: pass.eligibleFrames, detected_cpis: pass.detectedFrames,
        tracked_cpis: pass.trackedFrames,
        last_truth: pass.lastTruth ? {delay_km: pass.lastTruth.delay_km,
          doppler_hz: pass.lastTruth.doppler_hz,
          bearing_deg_true: pass.lastTruth.bearing_deg_true,
          ground_range_km: pass.lastTruth.ground_range_km,
          elevation_deg: pass.lastTruth.elevation_deg} : null}));
    const finalized = this.state.passes.filter(pass => pass.siteKey === this.key &&
      pass.startedAt >= contractStartedAt)
      .slice(-50).reverse().map(pass => ({pass_id: pass.id, hex: pass.hex,
        flight: pass.flight, started_at: pass.startedAt, finalized_at: pass.lastAt,
        outcome: eligibilityOutcome(pass), eligible_cpis: pass.eligibleFrames,
        detected_cpis: pass.detectedFrames, tracked_cpis: pass.trackedFrames,
        delay_km: {minimum: pass.minDelayKm, maximum: pass.maxDelayKm},
        receiver_slant_range_km: {minimum: pass.minSlantRangeKm,
          maximum: pass.maxSlantRangeKm},
        transmitter_range_km: {minimum: pass.minTransmitterRangeKm,
          maximum: pass.maxTransmitterRangeKm},
        bistatic_range_product_km2: {minimum: pass.minBistaticRangeProductKm2,
          maximum: pass.maxBistaticRangeProductKm2}}));
    const audits = [...this.state.passes.filter(pass => pass.siteKey === this.key &&
      pass.startedAt >= contractStartedAt).slice(-50),
      ...Object.values(this.state.active).filter(pass => pass.siteKey === this.key &&
        pass.startedAt >= contractStartedAt)]
      .flatMap(pass => pass.cpiAudit || []);
    const exactTiming = audits.filter(item => item.timing?.epoch !== undefined &&
      item.timing?.exact_event_time !== false).length;
    const joinFrames = new Map();
    for (const item of audits) {
      const row = joinFrames.get(item.timestamp_ms) || {timestamp_ms: item.timestamp_ms,
        aircraft: 0,
        exact_timing: Boolean(item.timing) && item.timing?.exact_event_time !== false,
        timing_quality: item.timing?.exact_event_time === false ? 'provisional' :
          (item.timing ? 'exact' : 'missing'),
        timing_source: item.timing?.event_time_source ?? null,
        timing_timestamp_ms: item.timing?.timestamp_ms ?? null};
      row.aircraft += 1; joinFrames.set(item.timestamp_ms, row);
    }
    const decisionGrade = this.timingPolicy === 'exact' && exactTiming > 0;
    const finalizedCurrentHour = this.state.passes.filter(pass => pass.siteKey === this.key &&
      pass.startedAt >= contractStartedAt && pass.lastAt >= timestamp - WINDOWS.hour &&
      pass.lastAt < timestamp && pass.eligibleFrames >= 3);
    const finalizedPreviousHour = this.state.passes.filter(pass => pass.siteKey === this.key &&
      pass.startedAt >= contractStartedAt && pass.lastAt >= timestamp - 2 * WINDOWS.hour &&
      pass.lastAt < timestamp - WINDOWS.hour && pass.eligibleFrames >= 3);
    const mixAdjusted = standardizedComparison(finalizedCurrentHour, finalizedPreviousHour);
    const warning = this.timingPolicy === 'provisional' ?
      'Operational association uses an inexact receiver time estimate; do not use for acceptance decisions.' :
      (!decisionGrade && this.eventTimeRejections > 0 ?
        'Authoritative scoring is blocked because the receiver has not supplied exact RF event time.' :
        (!decisionGrade ? 'Authoritative scoring is waiting for an exact-timed CPI.' : null));
    return {schema: 'vectorwarp-live-aircraft-scorecard-v1', updated_at: this.state.updatedAt,
      timing_policy: this.timingPolicy, decision_grade: decisionGrade, warning,
      adsb_quality: adsbQuality(this.config),
      collecting: this.config.truth?.adsb?.enabled === true && Boolean(this.limits),
      error: this.state.error, config_epoch: this.configEpoch,
      receiver_epoch: this.receiverEpoch, site_epoch: this.key,
      evaluation_contract_hash: this.scoreContractHash,
      evaluation_contract_started_at: contractStartedAt,
      association_lag_cpis: this.config.truth?.adsb?.association_lag_cpis || 0,
      association_lag_ms: (this.config.truth?.adsb?.association_lag_cpis || 0) *
        (this.limits?.cpiSeconds || .5) * 1000,
      provisional_event_time_uncertainty_s: this.timingPolicy === 'provisional' ?
        this.limits?.provisionalTimeUncertaintyS : 0,
      limits: {...this.limits, delay_definition: 'excess_bistatic_path_km',
        geometry_model: 'three-dimensional-WGS84-bistatic-slant-path'},
      windows: Object.fromEntries(Object.entries(WINDOWS).map(
        ([name, duration]) => [name, this.summarize(duration, timestamp, contractStartedAt)])),
      mix_adjusted_previous_hour_comparison: {...mixAdjusted,
        decision_grade: decisionGrade && mixAdjusted.decision_grade,
        timing_decision_grade: decisionGrade,
        note: 'Use this common-support comparison, not the raw one-hour percentage, for regression claims.'},
      data_quality: {audited_cpis: audits.length, exact_timing_cpis: exactTiming,
        missing_timing_cpis: audits.length - exactTiming,
        audited_aircraft_cpis: audits.length,
        exact_timed_aircraft_cpis: exactTiming,
        unique_cpi_frames: joinFrames.size,
        exact_timed_cpi_frames: [...joinFrames.values()].filter(item => item.exact_timing).length,
        pending_delayed_cpis: this.running,
        event_time_rejections: this.eventTimeRejections,
        last_event_time_rejection: this.lastEventTimeRejection,
        nuisance_accounting: this.timingPolicy === 'provisional' ?
          'bounded-event-time-rate-expanded-association-gates' : 'not-required',
        association_diagnostics: {scope: 'current-config-epoch-cumulative',
          ...(this.state.associationDiagnostics || {detection: {}, tracker: {}})},
        recent_cpi_joins: [...joinFrames.values()].sort((left, right) =>
          left.timestamp_ms - right.timestamp_ms).slice(-20)},
      active_passes: active, recent_finalized_passes: finalized};
  }

  recordingSubjects() {
    return this.state.eligibilityEvents.map(event => ({pass_id: event.pass_id,
      hex: event.hex || null, flight: event.flight || null}));
  }

  radarCoverage(options = {}) {
    const retained = radarCoverageFromPasses([
      ...this.state.passes,
      ...Object.values(this.state.active)
    ], {...options, now: this.now(), siteKey: this.key});
    const accumulated = radarCoverageFromAccumulator(this.state.radarCoverage,
      {...options, now: this.now()});
    const points = new Map();
    for (const point of [...retained.points, ...accumulated.points]) {
      const prior = points.get(point.bin);
      if (!prior || point.ground_range_km > prior.ground_range_km) points.set(point.bin, point);
    }
    return {...accumulated, supported_bins: points.size,
      associated_samples: Math.max(retained.associated_samples, accumulated.associated_samples),
      supporting_passes: Math.max(retained.supporting_passes, accumulated.supporting_passes),
      supporting_aircraft: Math.max(retained.supporting_aircraft, accumulated.supporting_aircraft),
      points: [...points.values()].sort((left, right) => left.bin - right.bin)};
  }

  close() {
    this.closed = true;
    if (this.sampler !== null) this.clearInterval(this.sampler);
    this.sampler = null;
    this.expire(Infinity); this.persist();
  }
}

function createEvidenceStatistics(config, adsbSource, dependencies) {
  return new EvidenceStatistics(config, adsbSource, dependencies);
}

// Exactly one lane owns operational side effects. It uses qualified exact
// timing (and zero expansion) when present, otherwise configured provisional
// uncertainty. The parallel exact scorecard never creates a duplicate capture.
function createEvidenceStatisticsLanes(config, adsbSource, {exact = {}, operational = {}} = {}) {
  return {
    exact: new EvidenceStatistics(config, adsbSource, {...exact, timingPolicy: 'exact',
      onEligibilityEvent: () => {}, onPeriodicCapture: () => {}, onTrackCapture: () => {}}),
    operational: new EvidenceStatistics(config, adsbSource, {...operational, timingPolicy: 'provisional'})
  };
}

module.exports = {SPEED_OF_LIGHT_KM_S, MAX_ELIGIBILITY_EVENTS, MAX_EVENT_TRUTH_SAMPLES,
  TRIGGER_EVENT_TRUTH_SAMPLES, TRUTH_SAMPLE_MS, MAX_ACTIVE_CAPTURE_TIMESTAMPS, MAX_CAPTURE_ASSOCIATIONS,
  limits, associationGates, assignments, delayBandEdges, delayBand, adsbQuality,
  ELEVATION_EDGES_DEG, AZIMUTH_EDGES_DEG,
  RADAR_COVERAGE_ALTITUDE_BANDS, radarCoverageFromPasses,
  radarCoverageFromAccumulator, rememberRadarCoverage,
  primaryGeometryBand, standardizedComparison,
  siteKey, siteDescriptor, EvidenceStatistics,
  createEvidenceStatistics, createEvidenceStatisticsLanes};
