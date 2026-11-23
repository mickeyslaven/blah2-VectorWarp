'use strict';
const {geographicContext, geographicConfig} = require('./replay-geographic-sites');

const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const {validateTruthWindow} = require('./capture-admission');
const {TrackPopulationStatistics} = require('./track-population-statistics');
const truthUncertainty = require('./truth-uncertainty');
const measuredCoverage = require('../html/js/measured_track_coverage');
const {observationCoordinates} = require('./observation-coordinates');
const {effectiveDelayConfig, associationGates, assignments} =
  require('../html/js/config_profiles');

const SPEED_OF_LIGHT_KM_S = 299792.458;
const nextPhase = (now, phase, interval = 30000) =>
  Math.floor((now - phase) / interval) * interval + phase + interval;
// One pending request per live ledger, one synchronous flush per timer turn.
class StatisticsFlushScheduler {
  constructor({now = Date.now, setTimeoutFn = setTimeout, clearTimeoutFn = clearTimeout,
    spacingMs = 250} = {}) {
    this.now = now; this.setTimeout = setTimeoutFn; this.clearTimeout = clearTimeoutFn;
    this.spacingMs = spacingMs; this.pending = new Map(); this.timer = null; this.lastFlushAt = -Infinity;
  }
  request(owner, flush) { if (!this.pending.has(owner)) this.pending.set(owner, flush); this.arm(); }
  cancel(owner) { this.pending.delete(owner); if (!this.pending.size && this.timer !== null) { this.clearTimeout(this.timer); this.timer = null; } }
  arm() {
    if (this.timer !== null || !this.pending.size) return;
    this.timer = this.setTimeout(() => { this.timer = null; const [owner, flush] = this.pending.entries().next().value;
      this.pending.delete(owner); try { flush(); } finally { this.lastFlushAt = this.now(); this.arm(); } },
    Math.max(0, this.lastFlushAt + this.spacingMs - this.now())); this.timer?.unref?.();
  }
}
const sharedFlushScheduler = new StatisticsFlushScheduler();
// Detailed finalized evidence is a bounded working set; lifetime headline
// counts roll into state.rollup. Without this boundary a long-running node can
// exceed the loader's safety cap and lose operational score visibility on the
// next API restart.
const MAX_PASSES = 500;
const PASS_HISTORY_MS = 24 * 60 * 60 * 1000;
const MAX_PASS_HISTORY_BYTES = 32 * 1024 * 1024;
const MAX_ELIGIBILITY_EVENTS = 250;
// Keep three scored CPI witnesses and the recent tail within the same bounds.
// An out-of-window/stale pass exit must not erase every searchable miss.
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
// Capture windows retain exact truth brackets; parent events need only a tail.
const MAX_EVENT_TRUTH_SAMPLES = TRIGGER_EVENT_TRUTH_SAMPLES;
const TRUTH_SAMPLE_MS = 500;
const MAX_ACTIVE_CAPTURE_TIMESTAMPS = 128;
const MAX_CAPTURE_ASSOCIATIONS = 64;
const MAX_TIMING_FRAMES = 2048;
const MAX_RADAR_FRAMES = 256;
const MAX_PENDING_SAMPLES = 32;
const PERSIST_INTERVAL_MS = 30000;
// Bound the engineering tail to prevent huge ledgers blocking the event loop.
// Counters, extents, first/last truth, capture windows and rollups are separate.
const MAX_CPI_AUDIT = 16;
// Sparse searchable presence can produce thousands of real intervals during a
// normal aircraft pass. Retain at least an hour at two isolated CPIs/second;
// keep the cap explicit so a long-lived target cannot grow the ledger forever.
const MAX_COVERAGE_INTERVALS = 8192;
const MAX_TOTAL_COVERAGE_INTERVALS = 1024 * 1024;
const TIMING_RETENTION_MS = 10 * 60 * 1000;
const PASS_GAP_MS = 120000;
const DEFAULT_PROVISIONAL_TIME_UNCERTAINTY_S = 1;
// Version the eligibility semantics separately from receiver configuration so
// previously finalized exposure is never reinterpreted under a new detector
// mask, truth Doppler estimator, or observation-freshness policy.
// v6 separates scorecards collected while public truth providers shared one
// round-robin cadence from scorecards collected with per-provider cadence.
// The older denominator can contain source-age gaps and must remain preserved
// but must never be blended into current evaluation percentages.
const DETECTOR_ELIGIBILITY_VERSION = 6;
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
const percent = (numerator, denominator) => denominator ?
  100 * numerator / denominator : null;
const associationCpis = value => Math.max(0, Number(value.associationEligibleFrames ??
  ((value.eligibleFrames || 0) - (value.associationAbstainedFrames || 0))) || 0);
const CPI_COUNTERS = {eligibleFrames: 'eligibleCpis', detectedFrames: 'detectedCpis',
  trackedFrames: 'trackedCpis', associationEligibleFrames: 'associationEligibleCpis',
  associationAbstainedFrames: 'associationAbstainedCpis'};
const cpiCount = (pass, field) => field === 'associationEligibleFrames' ? associationCpis(pass) :
  Math.max(0, Number(pass[field]) || 0);
function sumCpiCounts(passes, rollup = {}) {
  return Object.fromEntries(Object.entries(CPI_COUNTERS).map(([field, archived]) => [field,
    passes.reduce((sum, pass) => sum + cpiCount(pass, field), 0) +
      (rollup[archived] ?? (field === 'associationEligibleFrames' ? rollup.eligibleCpis : 0) ?? 0)]));
}
function emptyRollup(siteKey) {
  return {siteKey, passes: 0, eligible: 0, associationEligible: 0, detected: 0, tracked: 0,
    missed: 0, outOfConfig: 0, mapEligible: 0, detectorEligible: 0, truthUnusable: 0,
    eligibleNoRadar: 0, detectedNotTracked: 0, trackingFailureClasses: {},
    ...Object.fromEntries(Object.values(CPI_COUNTERS).map(field => [field, 0])),
    cpiExposureIncompleteArchivedPasses: 0};
}

function cpiExposure({eligibleFrames = 0, detectedFrames = 0, trackedFrames = 0,
  associationAbstainedFrames = 0, associationEligibleFrames},
  cpiSeconds = .5, {complete = true, excludedArchivedPasses = 0} = {}) {
  const eligible = Math.max(0, Number(eligibleFrames) || 0);
  const detected = Math.max(0, Number(detectedFrames) || 0);
  const tracked = Math.max(0, Number(trackedFrames) || 0);
  const abstained = Math.max(0, Number(associationAbstainedFrames) || 0);
  const comparable = associationCpis({eligibleFrames: eligible, associationEligibleFrames,
    associationAbstainedFrames: abstained});
  const missed = Math.max(0, comparable - detected);
  const untracked = Math.max(0, comparable - tracked);
  return {cpi_seconds: cpiSeconds,
    ...Object.fromEntries(Object.entries({eligible, detected, association_eligible: comparable,
      association_abstained: abstained, missed_detection: missed,
      operationally_tracked: tracked, operationally_untracked: untracked})
      .flatMap(([name, count]) => [[`${name}_cpis`, count], [`${name}_seconds`, count * cpiSeconds]])),
    ...Object.fromEntries([
      ['detection', 'detected', detected], ['missed_detection', 'missed_detection', missed],
      ['operational_track', 'operationally_tracked', tracked],
      ['operationally_untracked', 'operationally_untracked', untracked]]
      .flatMap(([fraction, percentage, count]) => [[`${fraction}_fraction`, ratio(count, comparable)],
        [`${percentage}_percent`, percent(count, comparable)]])),
    counter_invariants_valid: detected <= comparable && tracked <= comparable &&
      comparable + abstained === eligible,
    complete, excluded_archived_passes: excludedArchivedPasses,
    denominator_semantics: 'yield and misses use association-eligible CPIs; eligible CPIs retain all fresh searchable aircraft presence'};
}

function observeQualityContinuity(quality, timestamp, cpiSeconds, scored, detected, tracked) {
  const empty = () => ({lastTimestamp: null,
    currentDetectedRun: 0, longestDetectedRun: 0, currentMissRun: 0, longestMissRun: 0,
    currentTrackedRun: 0, longestTrackedRun: 0, currentUntrackedRun: 0, longestUntrackedRun: 0,
    detectionReacquisitions: 0, trackReacquisitions: 0, priorDetected: null, priorTracked: null});
  const continuity = quality.continuity ||= {base: empty(), tail: [],
    preexistingObservedCpis: Math.max(0, quality.observed - Number(scored)), lateCpis: 0};
  // Truth joins may resolve out of order. Retain the bounded pending-join
  // window, then fold older observations into a compact chronological prefix.
  if (finite(continuity.base.lastTimestamp) && timestamp <= continuity.base.lastTimestamp) {
    continuity.lateCpis += 1;
    return;
  }
  continuity.tail.push({timestamp, scored, detected, tracked});
  continuity.tail.sort((left, right) => left.timestamp - right.timestamp);
  const advance = (state, row) => {
    const cpiMs = cpiSeconds * 1000;
    const adjacent = finite(state.lastTimestamp) &&
      Math.abs(row.timestamp - state.lastTimestamp - cpiMs) <= Math.max(2, cpiMs * .1);
    if (!adjacent || !row.scored) Object.assign(state, {currentDetectedRun: 0,
      currentMissRun: 0, currentTrackedRun: 0, currentUntrackedRun: 0,
      priorDetected: null, priorTracked: null});
    state.lastTimestamp = row.timestamp;
    if (!row.scored) return;
    for (const [present, hit, miss, reacquisitions] of [
      [row.detected, 'Detected', 'Miss', 'detectionReacquisitions'],
      [row.tracked, 'Tracked', 'Untracked', 'trackReacquisitions']]) {
      const run = present ? hit : miss, reset = present ? miss : hit;
      state[`current${run}Run`] += 1;
      state[`current${reset}Run`] = 0;
      state[`longest${run}Run`] = Math.max(state[`longest${run}Run`], state[`current${run}Run`]);
      if (present && state[`prior${hit}`] === false) state[reacquisitions] += 1;
      state[`prior${hit}`] = present;
    }
  };
  while (continuity.tail.length > MAX_PENDING_SAMPLES) advance(continuity.base, continuity.tail.shift());
  const current = {...continuity.base};
  for (const row of continuity.tail) advance(current, row);
  Object.assign(quality, current);
}

function observeQualityCpi(pass, detected, tracked, timestamp, cpiSeconds) {
  const quality = pass.qualityCpi ||= {observed: 0,
    preexistingEligibleCpis: Math.max(0, associationCpis(pass) - 1),
    detectedAndTracked: 0, detectedNotTracked: 0, trackedWithoutDetection: 0};
  quality.observed += 1;
  if (detected && tracked) quality.detectedAndTracked += 1;
  else if (detected) quality.detectedNotTracked += 1;
  else if (tracked) quality.trackedWithoutDetection += 1;
  observeQualityContinuity(quality, timestamp, cpiSeconds, true, detected, tracked);
}

function passExposure(pass, cpiSeconds) {
  return {sensitivity_epoch_id: pass.sensitivityEpochId || null,
    sensitivity_epoch_mixed: pass.sensitivityEpochMixed === true,
    eligible_cpis: pass.eligibleFrames, association_eligible_cpis: associationCpis(pass),
    detected_cpis: pass.detectedFrames, tracked_cpis: pass.trackedFrames,
    cpi_exposure: cpiExposure(pass, cpiSeconds), track_quality: qualityCpiSummary(pass, cpiSeconds),
    measured_track_coverage: measuredCoverage.aggregate([coverageBasis([pass])])};
}

function compactAssociationPass(value) {
  return {...Object.fromEntries(['pass_id', 'hex', 'flight', 'started_at', 'finalized_at',
    'outcome', 'channel', 'site_epoch', 'evaluation_contract_hash', 'evaluation_policy_hash',
    'timing_policy', 'sensitivity_epoch_id'].map(field => [field, value[field] ?? null])),
    sensitivity_epoch_mixed: value.sensitivity_epoch_mixed === true,
    decision_grade: value.decision_grade === true,
    eligible_cpis: Number(value.eligible_cpis) || 0,
    association_eligible_cpis: Number(value.association_eligible_cpis ?? value.eligible_cpis) || 0,
    detected_cpis: Number(value.detected_cpis) || 0,
    tracked_cpis: Number(value.tracked_cpis) || 0,
    eligible: value.eligible !== false, detected: value.detected === true,
    tracked: value.tracked === true};
}

// Federation needs pass identity and outcome to deduplicate aircraft observed
// by several nodes, but it must not repeatedly transport the full evidence
// ledger. A small columnar contract removes repeated field names and channel /
// outcome strings while retaining every finalized pass used by the network
// scorecard. Detailed provenance remains available from the node-local API.
function federationAssociationPassTransport(values) {
  const channels = [], outcomes = [];
  const channelIndex = new Map(), outcomeIndex = new Map();
  const intern = (value, dictionary, indexes) => {
    const key = String(value ?? '');
    if (!indexes.has(key)) { indexes.set(key, dictionary.length); dictionary.push(key); }
    return indexes.get(key);
  };
  const rows = (Array.isArray(values) ? values : []).map(value => {
    const pass = compactAssociationPass(value);
    const flags = Number(pass.eligible !== false) |
      (Number(pass.detected === true) << 1) |
      (Number(pass.tracked === true) << 2) |
      (Number(pass.sensitivity_epoch_mixed === true) << 3);
    return [String(pass.hex || ''), String(pass.flight || ''),
      Number(pass.started_at) || 0, Number(pass.finalized_at) || 0,
      intern(pass.outcome, outcomes, outcomeIndex),
      intern(pass.channel, channels, channelIndex),
      Number(pass.eligible_cpis) || 0, Number(pass.detected_cpis) || 0,
      Number(pass.tracked_cpis) || 0, flags];
  });
  return {schema: 'vectorwarp-federation-association-passes-v1',
    fields: ['hex', 'flight', 'started_at', 'finalized_at', 'outcome_index',
      'channel_index', 'eligible_cpis', 'detected_cpis', 'tracked_cpis', 'flags'],
    dictionaries: {channels, outcomes}, rows};
}

function expandFederationAssociationPassTransport(value) {
  if (value?.schema !== 'vectorwarp-federation-association-passes-v1' ||
      !Array.isArray(value.rows)) return Array.isArray(value?.passes) ? value.passes : [];
  const channels = Array.isArray(value.dictionaries?.channels) ?
    value.dictionaries.channels : [];
  const outcomes = Array.isArray(value.dictionaries?.outcomes) ?
    value.dictionaries.outcomes : [];
  return value.rows.map(row => {
    const flags = Number(row?.[9]) || 0;
    return {hex: String(row?.[0] || ''), flight: String(row?.[1] || ''),
      started_at: Number(row?.[2]) || 0, finalized_at: Number(row?.[3]) || 0,
      outcome: outcomes[Number(row?.[4])] || null,
      channel: channels[Number(row?.[5])] || 'primary',
      eligible_cpis: Number(row?.[6]) || 0, detected_cpis: Number(row?.[7]) || 0,
      tracked_cpis: Number(row?.[8]) || 0, eligible: Boolean(flags & 1),
      detected: Boolean(flags & 2), tracked: Boolean(flags & 4),
      sensitivity_epoch_mixed: Boolean(flags & 8)};
  });
}

function coverageBasis(passes) {
  const eligible = passes.filter(pass => pass.eligibleFrames > 0);
  return {version: 1, complete: eligible.every(pass => pass.measuredCoverage?.complete === true),
    excluded_passes: eligible.filter(pass => !pass.measuredCoverage).length,
    aircraft: eligible.filter(pass => pass.measuredCoverage).map(pass => ({hex: pass.hex,
      eligible: pass.measuredCoverage.eligible, scored: pass.measuredCoverage.scored,
      measured: pass.measuredCoverage.measured}))};
}

function boundMeasuredCoverage(passes, maximum = MAX_TOTAL_COVERAGE_INTERVALS) {
  const fields = ['eligible', 'scored', 'measured'];
  const count = pass => fields.reduce((sum, field) => sum + (pass.measuredCoverage?.[field]?.length || 0), 0);
  let retained = passes.reduce((sum, pass) => sum + count(pass), 0), changed = false;
  if (retained <= maximum) return false;
  // Share the bound across the ledger: a few long passes can retain much more
  // detail without allowing hundreds of long passes to exceed reload limits.
  for (const pass of [...passes].sort((left, right) => (left.lastAt || 0) - (right.lastAt || 0))) {
    if (retained <= maximum) break;
    const size = count(pass);
    if (!size) continue;
    const fraction = Math.max(0, size - (retained - maximum)) / size;
    for (const field of fields) {
      const values = pass.measuredCoverage[field] || [], keep = Math.floor(values.length * fraction);
      pass.measuredCoverage[field] = keep ? values.slice(-keep) : [];
    }
    pass.measuredCoverage.complete = false;
    retained -= size - count(pass); changed = true;
  }
  return changed;
}

const exactUint64 = value => typeof value === 'string' && /^(0|[1-9][0-9]{0,19})$/.test(value) &&
  BigInt(value) <= 18446744073709551615n;

function sourceCpiInterval(frame) {
  if (!finite(frame?.timestamp)) return null;
  const event = frame.eventTime;
  if (!exactUint64(event?.firstSample) || !exactUint64(event?.endSample) ||
      !finite(event.sampleRateHz) || event.sampleRateHz <= 0) return null;
  const samples = BigInt(event.endSample) - BigInt(event.firstSample);
  if (samples <= 0n || samples > BigInt(Number.MAX_SAFE_INTEGER)) return null;
  const halfMs = Number(samples) * 500 / event.sampleRateHz;
  if (!finite(halfMs) || halfMs <= 0) return null;
  return [frame.timestamp - halfMs, frame.timestamp + halfMs];
}

// A temporal aperture is credited only through actual source-frame identities.
// Depth alone cannot locate its support in time, especially across resets/loss.
function measurementSupport(association, history) {
  const track = association?.tracker, evidence = track?.measurement_evidence;
  if (track?.measured !== true || track.source === 'prediction' ||
      track.state !== 'ACTIVE' || !evidence) return null;
  const sequences = evidence.support_sequences_exact;
  if (!exactUint64(evidence.epoch_exact) || !exactUint64(evidence.newest_sequence_exact) ||
      ![1, 2, 4, 8].includes(evidence.cpis) || !Array.isArray(sequences) ||
      sequences.length !== evidence.cpis || !sequences.every(exactUint64) ||
      sequences.some((sequence, index) => sequence === '0' ||
        index > 0 && BigInt(sequences[index - 1]) - BigInt(sequence) !== 1n) ||
      sequences[0] !== evidence.newest_sequence_exact)
    return null;
  const frames = new Map();
  for (const frame of history.values()) for (const item of [frame.evidence_identity,
    ...(Array.isArray(frame.evidence) ? frame.evidence : [])].filter(Boolean))
    if (item.epoch_exact === evidence.epoch_exact && exactUint64(item.newest_sequence_exact)) {
      const prior = frames.get(item.newest_sequence_exact);
      const interval = sourceCpiInterval(frame);
      if (prior !== undefined && (prior.timestamp !== frame.timestamp ||
          JSON.stringify(prior.interval) !== JSON.stringify(interval))) return null;
      frames.set(item.newest_sequence_exact, {timestamp: frame.timestamp, interval});
    }
  if (frames.get(sequences[0])?.timestamp !== association.frame_timestamp_ms) return null;
  const intervals = sequences.map(sequence => frames.get(sequence)?.interval);
  return intervals.every(Boolean) ? measuredCoverage.union(intervals) : null;
}

function observeMeasuredCoverage(pass, timestamp, cpiSeconds, scored, support, sourceInterval) {
  const value = pass.measuredCoverage ||= {complete: pass.eligibleFrames === 1,
    eligible: [], scored: [], measured: []};
  // An incomplete prefix must stay marked incomplete, but it must not prevent
  // later measurements from being collected. Older capped ledgers resume here
  // without claiming that their discarded evidence has been recovered.
  // Native eventTime timestamps identify the CPI midpoint (EventTime.h).
  const interval = sourceInterval || [timestamp - cpiSeconds * 500, timestamp + cpiSeconds * 500];
  measuredCoverage.extendUnion(value.eligible, [interval]);
  if (scored) measuredCoverage.extendUnion(value.scored, [interval]);
  if (scored && support) measuredCoverage.extendUnion(value.measured, support);
  if (['eligible', 'scored', 'measured'].some(field => value[field].length > MAX_COVERAGE_INTERVALS)) {
    value.complete = false;
    for (const field of ['eligible', 'scored', 'measured']) value[field] = value[field].slice(-MAX_COVERAGE_INTERVALS);
  }
}

function qualityCpiSummary(pass, cpiSeconds = .5) {
  const quality = pass.qualityCpi || {};
  const observed = quality.observed || 0;
  const continuity = quality.continuity;
  const run = field => continuity ? quality[field] || 0 : null;
  const latency = at => finite(at) ? Math.max(0, at -
    (finite(pass.firstEligibleAt) ? pass.firstEligibleAt : pass.startedAt)) / 1000 : null;
  return {observed_cpis: observed,
    complete_for_pass: (quality.preexistingEligibleCpis || 0) === 0 &&
      observed === associationCpis(pass),
    preexisting_uninstrumented_eligible_cpis: quality.preexistingEligibleCpis || 0,
    continuity_complete_for_pass: Boolean(continuity && !continuity.preexistingObservedCpis &&
      !continuity.lateCpis && !quality.preexistingEligibleCpis && observed === associationCpis(pass)),
    continuity_semantics: 'adjacent-scored-cpis-at-configured-cadence',
    detected_and_operationally_tracked_cpis: quality.detectedAndTracked || 0,
    detected_but_not_operationally_tracked_cpis: quality.detectedNotTracked || 0,
    operationally_tracked_without_current_detection_cpis:
      quality.trackedWithoutDetection || 0,
    ...Object.fromEntries(Object.entries({detected: 'Detected', missed_detection: 'Miss',
      operationally_tracked: 'Tracked', operationally_untracked: 'Untracked'})
      .flatMap(([name, field]) => [[`longest_consecutive_${name}_cpis`, run(`longest${field}Run`)],
        [`longest_${name}_seconds`, continuity ? run(`longest${field}Run`) * cpiSeconds : null]])),
    detection_reacquisitions: run('detectionReacquisitions'),
    operational_track_reacquisitions: run('trackReacquisitions'),
    latency_origin: finite(pass.firstEligibleAt) ? 'first_eligible_cpi' : 'legacy_pass_start',
    first_detection_latency_seconds: latency(pass.firstDetectionAt),
    first_operational_track_latency_seconds: latency(pass.firstTrackAt),
    detection_to_operational_track_latency_seconds:
      finite(pass.firstDetectionAt) && finite(pass.firstTrackAt) ?
        Math.max(0, pass.firstTrackAt - pass.firstDetectionAt) / 1000 : null};
}

function provisionalTimingFrame(value,
  source = 'radar_host_arrival_estimate') {
  const parsed = parse(value);
  if (!parsed || !finite(parsed.timestamp) || parsed.eventTime?.exact === true ||
      parsed.eventTime?.estimateAvailable === true) return parsed || value;
  return {...parsed, eventTime: {source, exact: false, estimateAvailable: true,
    uncertaintyBounded: true,
    reason: 'RF device time is not UTC-mapped; using bounded host-arrival timing'}};
}

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

function limits(config, frames = []) {
  const coordinates = observationCoordinates(config, frames);
  if (!coordinates) return null;
  config = {...config, capture: {...config.capture,
    fs: coordinates.sampleRate, fc: coordinates.frequency}};
  config = effectiveDelayConfig(config);
  const sampleRate = config.capture?.fs;
  const ambiguity = config.process?.ambiguity || {};
  const detection = config.process?.detection || {};
  const clutter = config.process?.clutter || {};
  const trackerInitiate = config.process?.tracker?.initiate || {};
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
    observationCoordinates: coordinates,
    dopplerResolutionHz: 1 / cpiSeconds,
    delayMinBin: ambiguity.delayMin, delayMaxBin: ambiguity.delayMax,
    delayMinKm: ambiguity.delayMin * kmPerBin,
    delayMaxKm: ambiguity.delayMax * kmPerBin,
    detectionMinDelayBin: detection.minDelay,
    detectionMinDelayKm: detection.minDelay * kmPerBin,
    detectionMinAbsDopplerHz: finite(detection.minDoppler) ?
      Math.max(0, detection.minDoppler) : 0,
    dopplerMinHz: ambiguity.dopplerMin, dopplerMaxHz: ambiguity.dopplerMax,
    clutterMaxBin: finite(clutter.delayMax) ? clutter.delayMax : null,
    clutterMaxKm: finite(clutter.delayMax) ? clutter.delayMax * kmPerBin : null,
    matchDelayKm: config.process?.bearing?.manifold_recorder?.max_delay_error_km ?? .25,
    matchDopplerHz: config.process?.bearing?.manifold_recorder?.max_doppler_error_hz ?? 5,
    maxTruthAgeS: config.process?.bearing?.manifold_recorder?.max_truth_age_s ?? 3,
    trackerInitiateM: Math.max(1, trackerInitiate.M ?? 3),
    trackerInitiateN: Math.max(1, trackerInitiate.N ?? 6),
    trackerInitiateMinSnrDb: finite(trackerInitiate.min_snr_db) ?
      trackerInitiate.min_snr_db : -Infinity,
    provisionalTimeUncertaintyS,
    truthUncertaintyPolicy: truthUncertainty.policy(config.truth?.adsb?.uncertainty_association)};
}

function siteCoordinates(site = {}) {
  return {latitude: site.latitude ?? null, longitude: site.longitude ?? null,
    altitude: site.altitude ?? null};
}

function scientificDevice(device = {}) {
  const heimdall = device.heimdall && typeof device.heimdall === 'object' ?
    Object.fromEntries(Object.entries(device.heimdall).filter(([key]) =>
      !['host', 'port', 'control_port'].includes(key))) : device.heimdall;
  const geometry = device.array_geometry && typeof device.array_geometry === 'object' ?
    Object.fromEntries(Object.entries(device.array_geometry).filter(([key]) =>
      key !== 'operator_notes')) : device.array_geometry;
  return {...device, heimdall, array_geometry: geometry};
}

function siteKey(config, timingPolicy = 'exact') {
  config = geographicConfig(config);
  const relevant = {detectorEligibilityVersion: DETECTOR_ELIGIBILITY_VERSION,
    evaluationContract: evaluationContract(limits(config), timingPolicy),
    ...(config.capture?.replay?.state === true ? {replayRecordingIdentity:
      {recording: config.capture.replay.recording_sha256 || config.capture.replay.qualification?.acquisition?.recording_sha256 || null,
        acquisition: config.capture.replay.acquisition_sha256 || config.capture.replay.qualification?.acquisition?.acquisition_sha256 || null}} : {}),
    frequency: config.capture?.fc, sampleRate: config.capture?.fs,
    receiver: siteCoordinates(config.location?.rx),
    transmitter: siteCoordinates(config.location?.tx),
    device: scientificDevice(config.capture?.device),
    data: config.process?.data,
    delayReferenceSampleRateHz: config.process?.delay_reference_sample_rate_hz,
    ambiguity: config.process?.ambiguity, clutter: config.process?.clutter,
    detection: config.process?.detection, tracker: config.process?.tracker,
    reference: config.process?.reference_synthesis,
    surveillanceFusion: config.process?.surveillance_fusion,
    bearing: config.process?.bearing,
    adsbEvaluation: {
      maxPositionAge: config.truth?.adsb?.max_position_age,
      associationLagCpis: config.truth?.adsb?.association_lag_cpis,
      delayTolerance: config.truth?.adsb?.delay_tolerance,
      dopplerTolerance: config.truth?.adsb?.doppler_tolerance,
      provisionalEventTimeUncertaintyS:
        config.truth?.adsb?.provisional_event_time_uncertainty_s,
      ...(config.truth?.adsb?.uncertainty_association?.enable === true ?
        {uncertaintyAssociation: truthUncertainty.policy(config.truth.adsb.uncertainty_association)} : {})}};
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
  return {schema: 'vectorwarp-live-aircraft-scorecard-v2',
    detector_eligibility_version: DETECTOR_ELIGIBILITY_VERSION, delay_min: span?.delayMinBin,
    delay_max: span?.delayMaxBin, doppler_min: span?.dopplerMinHz,
    doppler_max: span?.dopplerMaxHz, timing_trigger: 'native-only-v2',
    exact_timing_required: timingPolicy === 'exact', timing_policy: timingPolicy,
    timing_skew_ms: 100, truth_offset_s: span?.truthUncertaintyPolicy?.maxProjectionS ?? 1,
    ...(span?.truthUncertaintyPolicy ? {association_family: 'uncertainty-ellipse-v1',
      uncertainty_policy: span.truthUncertaintyPolicy, null_calibrated: false} : {}),
    provisional_event_time_uncertainty_s: timingPolicy === 'provisional' ?
      span?.provisionalTimeUncertaintyS : 0,
    source_disagreement_km: .5, required_cpis: 3, stable_track_associations: 3,
    eligibility_delay_floor_km: span?.detectionMinDelayKm,
    eligibility_min_absolute_doppler_hz: span?.detectionMinAbsDopplerHz,
    truth_doppler_policy: span?.truthUncertaintyPolicy ? 'uncertainty-kinematic-v1' :
      'reported-kinematic-with-position-fit-fallback-v1',
    truth_freshness_policy: 'original-observation-offset-plus-prediction-age-v1',
    truth_source_availability_policy: 'independent-provider-cadence-v1',
    max_truth_age_s: span?.maxTruthAgeS,
    association_base_delay_km: span?.matchDelayKm,
    association_base_doppler_hz: span?.matchDopplerHz,
    eligibility_semantics: 'fresh-aircraft-presence-in-detector-searchable-delay-doppler',
    association_abstention_excludes_presence: false};
}

function siteDescriptor(config) {
  config = geographicConfig(config);
  return {detector_eligibility_version: DETECTOR_ELIGIBILITY_VERSION,
    frequency_hz: config.capture?.fc, sample_rate_hz: config.capture?.fs,
    delay_reference_sample_rate_hz: config.process?.delay_reference_sample_rate_hz ?? null,
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

function emptyPass(hex, flight, at, key, evaluationContractHash) {
  return {id: passId(key, hex, at), hex, flight: flight || '', siteKey: key, evaluationContractHash,
    startedAt: at, lastAt: at,
    frames: 0, physicalEligibleFrames: 0, detectorEligibleFrames: 0, eligibleFrames: 0,
    detectedFrames: 0, trackedFrames: 0, belowFloorFrames: 0,
    aboveDelayFrames: 0, outsideDopplerFrames: 0, dopplerNotchFrames: 0, staleTruthFrames: 0,
    missingTimingFrames: 0, associationEligibleFrames: 0, associationAbstainedFrames: 0,
    clutterCancelledFrames: 0, outsideClutterFrames: 0, minDelayKm: null, maxDelayKm: null,
    minDopplerHz: null, maxDopplerHz: null, minAltitudeGeomFt: null,
    maxAltitudeGeomFt: null, minElevationDeg: null, maxElevationDeg: null,
    minGroundRangeKm: null, maxGroundRangeKm: null,
    minSlantRangeKm: null, maxSlantRangeKm: null,
    minTransmitterRangeKm: null, maxTransmitterRangeKm: null,
    minBistaticRangeProductKm2: null, maxBistaticRangeProductKm2: null,
    maxAbsDelayRateKmS: null,
    maxAbsDopplerRateHzS: null, azimuthSectors45: Array(8).fill(0),
    truthDopplerDiagnostics: {comparableFrames: 0, maximumAbsDifferenceHz: 0,
      searchableDisagreements: 0},
    firstTruth: null, lastTruth: null, truthHistory: [], cpiAudit: [],
    firstEligibleAt: null, lastEligibleAt: null,
    firstDetectionAt: null, firstTrackAt: null,
    detectionTimestamps: [], trackTimestamps: [], sampledBuckets: [],
    qualityCpi: {observed: 0, preexistingEligibleCpis: 0,
      detectedAndTracked: 0, detectedNotTracked: 0, trackedWithoutDetection: 0,
      currentDetectedRun: 0, longestDetectedRun: 0, currentMissRun: 0,
      longestMissRun: 0, currentTrackedRun: 0, longestTrackedRun: 0,
      currentUntrackedRun: 0, longestUntrackedRun: 0,
      detectionReacquisitions: 0, trackReacquisitions: 0,
      priorDetected: null, priorTracked: null},
    delayBands: {}, slantRangeBands: {}, elevationBands: {}, azimuthBands: {},
    azimuthElevationBands: {}, geometryBands: {}};
}

function summarizeBands(rows, field) {
  const output = {};
  for (const pass of rows) for (const [id, band] of Object.entries(pass[field] || {})) {
    const aggregate = output[id] ||= {minimum: band.minimum_km ?? band.minimum,
      maximum: band.maximum_km ?? band.maximum, eligible_passes: 0, detected_passes: 0,
      tracked_passes: 0, eligible_cpis: 0, association_eligible_cpis: 0,
      association_eligible_passes: 0, detected_cpis: 0, tracked_cpis: 0};
    aggregate.eligible_cpis += band.eligibleFrames || 0;
    aggregate.association_eligible_cpis += associationCpis(band);
    aggregate.detected_cpis += band.detectedFrames || 0;
    aggregate.tracked_cpis += band.trackedFrames || 0;
    if ((band.eligibleFrames || 0) >= 3) aggregate.eligible_passes += 1;
    if (associationCpis(band) >= 3) {
      aggregate.association_eligible_passes += 1;
      if ((band.detectedFrames || 0) >= 3) aggregate.detected_passes += 1;
      if ((band.trackedFrames || 0) >= 3) aggregate.tracked_passes += 1;
    }
  }
  for (const band of Object.values(output)) {
    band.detection_yield = ratio(band.detected_passes, band.association_eligible_passes);
    band.tracking_yield = ratio(band.tracked_passes, band.association_eligible_passes);
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
  return entries.sort((left, right) => associationCpis(right[1]) -
    associationCpis(left[1]) || left[0].localeCompare(right[0]))[0][0];
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
  const weighted = (arm, outcome) => pooled ? common.reduce((sum, key) => {
    const weight = (candidate.get(key).eligible + reference.get(key).eligible) / pooled;
    return sum + weight * ratio(arm.get(key)[outcome], arm.get(key).eligible);
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
    current: {detection_yield: weighted(candidate, 'detected'), tracking_yield: weighted(candidate, 'tracked')},
    baseline: {detection_yield: weighted(reference, 'detected'),
      tracking_yield: weighted(reference, 'tracked')}};
}

function compactCpiAudit(rows, maximum) {
  if (!Array.isArray(rows) || rows.length <= maximum) return rows;
  // Selection is independent of detected/tracked outcome. This is a bounded
  // diagnostic sample, never a replacement for whole-pass replay evidence.
  const scored = rows.filter(row => row.scored === true).slice(-3);
  const keep = new Set(scored);
  for (let index = rows.length - 1; index >= 0 && keep.size < maximum; --index)
    keep.add(rows[index]);
  return rows.filter(row => keep.has(row));
}

function compactFinalizedPass(pass) {
  if (!pass || typeof pass !== 'object') return pass;
  pass.cpiAudit = compactCpiAudit(pass.cpiAudit, MAX_FINALIZED_CPI_AUDIT);
  if (Array.isArray(pass.truthHistory) &&
      pass.truthHistory.length > MAX_FINALIZED_TRUTH_SAMPLES)
    pass.truthHistory = MAX_FINALIZED_TRUTH_SAMPLES ?
      pass.truthHistory.slice(-MAX_FINALIZED_TRUTH_SAMPLES) : [];
  return pass;
}

// Keep window accounting after diagnostic detail rolls into lifetime totals.
// These rows retain counters, strata and interval unions, not CPI witnesses or
// repeated truth/association payloads. They live in the existing atomic ledger.
function compactPassSummary(pass) {
  const summary = {...pass};
  for (const field of ['cpiAudit', 'truthHistory', 'firstTruth', 'lastTruth',
    'detectionAssociations', 'trackerAssociations', 'detectionTimestamps',
    'trackTimestamps', 'sampledBuckets']) delete summary[field];
  if (pass.qualityCpi?.continuity) summary.qualityCpi = {...pass.qualityCpi,
    continuity: {preexistingObservedCpis: pass.qualityCpi.continuity.preexistingObservedCpis,
      lateCpis: pass.qualityCpi.continuity.lateCpis}};
  return summary;
}

function compactActivePass(pass) {
  if (!pass || typeof pass !== 'object') return pass;
  pass.cpiAudit = compactCpiAudit(pass.cpiAudit, MAX_CPI_AUDIT);
  if (Array.isArray(pass.sampledBuckets) && pass.sampledBuckets.length > MAX_CPI_AUDIT)
    pass.sampledBuckets = pass.sampledBuckets.slice(-MAX_CPI_AUDIT);
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
  const comparable = associationCpis(old) >= 3;
  const mapEligible = old.physicalEligibleFrames >= 3;
  const detectorEligible = (old.detectorEligibleFrames ?? old.physicalEligibleFrames) >= 3;
  if (old.siteKey !== key) return;
  state.rollup.passes += 1;
  for (const [field, archived] of Object.entries(CPI_COUNTERS))
    state.rollup[archived] = (state.rollup[archived] || 0) + cpiCount(old, field);
  state.rollup.dopplerNotchFrames = (state.rollup.dopplerNotchFrames || 0) +
    (old.dopplerNotchFrames || 0);
  if (mapEligible) state.rollup.mapEligible = (state.rollup.mapEligible || 0) + 1;
  if (detectorEligible) state.rollup.detectorEligible =
    (state.rollup.detectorEligible || 0) + 1;
  if (!detectorEligible) state.rollup.outOfConfig += 1;
  if (detectorEligible && !eligible) state.rollup.truthUnusable += 1;
  if (eligible) state.rollup.eligible += 1;
  if (comparable) state.rollup.associationEligible = (state.rollup.associationEligible || 0) + 1;
  if (eligible && old.detectedFrames >= 3) state.rollup.detected += 1;
  if (eligible && old.trackedFrames >= 3) state.rollup.tracked += 1;
  if (comparable && old.trackedFrames < 3) state.rollup.missed += 1;
  if (comparable && old.detectedFrames < 3 && old.trackedFrames < 3)
    state.rollup.eligibleNoRadar += 1;
  if (eligible && old.detectedFrames >= 3 && old.trackedFrames < 3)
    state.rollup.detectedNotTracked += 1;
  if (old.trackingFailureClass) {
    state.rollup.trackingFailureClasses ||= {};
    state.rollup.trackingFailureClasses[old.trackingFailureClass] =
      (state.rollup.trackingFailureClasses[old.trackingFailureClass] || 0) + 1;
  }
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

// Explain detector-to-track loss using only evidence retained for the pass.
// The class is diagnostic: it never changes association or tracker behavior.
function trackingFailureClass(pass, scoringLimits) {
  if (!pass || pass.eligibleFrames < 3 || pass.detectedFrames < 3 ||
      pass.trackedFrames >= 3) return null;
  const detections = (pass.detectionAssociations || [])
    .filter(item => finite(item?.frame_timestamp_ms))
    .sort((left, right) => left.frame_timestamp_ms - right.frame_timestamp_ms);
  if (pass.trackedFrames > 0 || (pass.trackerAssociations || []).length > 0)
    return 'operational_track_support_below_pass_threshold';
  if (detections.length < pass.detectedFrames)
    return 'insufficient_diagnostic_evidence';
  const unique = detections.filter((item, index) => index === 0 ||
    item.frame_timestamp_ms !== detections[index - 1].frame_timestamp_ms);
  const threshold = scoringLimits?.trackerInitiateMinSnrDb ?? -Infinity;
  const birthEligible = item => finite(item.measured_snr_db) &&
    item.measured_snr_db >= threshold;
  const required = scoringLimits?.trackerInitiateM ?? 3;
  if (!unique.some(birthEligible)) return unique.every(item => finite(item.measured_snr_db)) ?
    'below_tracker_initiation_snr' : 'insufficient_diagnostic_evidence';
  const windowMs = Math.max(0, ((scoringLimits?.trackerInitiateN ?? 6) - 1) *
    (scoringLimits?.cpiSeconds ?? .5) * 1000 + 1);
  let continuous = false;
  for (let first = 0; first < unique.length; ++first) {
    if (!birthEligible(unique[first])) continue;
    let last = first;
    while (last + 1 < unique.length &&
      unique[last + 1].frame_timestamp_ms - unique[first].frame_timestamp_ms <= windowMs) ++last;
    if (last - first + 1 >= required) { continuous = true; break; }
  }
  if (!continuous) return unique.some(item => !finite(item.measured_snr_db)) ?
    'insufficient_diagnostic_evidence' : 'insufficient_observed_scorecard_continuity';
  return 'unresolved_after_observed_support';
}

function countTrackingFailureClasses(passes) {
  const counts = {};
  for (const pass of passes) {
    const name = pass.trackingFailureClass || 'unclassified_retained_legacy';
    counts[name] = (counts[name] || 0) + 1;
  }
  return counts;
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
  if (associationCpis(pass) < 3) return 'association_unresolved';
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
    doppler_method: prediction.doppler_method ?? null,
    doppler_position_fit_hz: prediction.doppler_position_fit_hz ?? null,
    doppler_kinematic_hz: prediction.doppler_kinematic_hz ?? null,
    doppler_estimator_difference_hz: prediction.doppler_estimator_difference_hz ?? null,
    frequency_hz: prediction.frequency_hz,
    delay_definition: prediction.delay_definition || 'excess_bistatic_path_km',
    delay_rate_km_s: prediction.delay_rate_km_s,
    doppler_rate_hz_s: prediction.doppler_rate_hz_s,
    ...(prediction.truth_uncertainty ? {truth_uncertainty: prediction.truth_uncertainty} : {}),
    provenance: prediction.provenance && typeof prediction.provenance === 'object' ?
      {...prediction.provenance} : null};
}

function truthIntegrity(prediction, uncertaintyPolicy = null, timestamp = null) {
  const provenance = prediction?.provenance;
  if (!provenance || typeof provenance !== 'object') return {valid: false, reason: 'missing-provenance'};
  // A cached prediction's original observation continues to age even when its
  // stored projection offset does not. It cannot extend the presence horizon.
  const predictionAge = finite(timestamp) && finite(prediction.timestamp) ?
    timestamp / 1000 - prediction.timestamp : 0;
  const offset = finite(provenance.observation_time_offset_s) ?
    provenance.observation_time_offset_s + predictionAge : NaN;
  if (!finite(offset) || Math.abs(offset) > (uncertaintyPolicy?.maxProjectionS ?? 1))
    return {valid: false, reason: 'stale-observation'};
  if (uncertaintyPolicy && provenance.disagreement === true)
    return {valid: false, reason: 'source-disagreement'};
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
  // The bounded history is only a recent diagnostic tail.  Preserve the true
  // pass endpoints independently so trimming (or an out-of-order observation)
  // cannot rewrite the beginning/end of the pass.
  if (!pass.firstTruth || snapshot.timestamp_ms < pass.firstTruth.timestamp_ms)
    pass.firstTruth = snapshot;
  if (!pass.lastTruth || snapshot.timestamp_ms > pass.lastTruth.timestamp_ms)
    pass.lastTruth = snapshot;
  if (!Array.isArray(pass.truthHistory)) pass.truthHistory = [];
  pass.truthHistory.push(snapshot);
  pass.truthHistory.sort((left, right) => left.timestamp_ms - right.timestamp_ms);
  if (pass.truthHistory.length > MAX_EVENT_TRUTH_SAMPLES)
    pass.truthHistory.splice(0, pass.truthHistory.length - MAX_EVENT_TRUTH_SAMPLES);
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
  return snapshot;
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
        confidence: item.confidence ?? null, associations: item.n ?? null,
        measured: item.measured, measurement_evidence: item.measurement_evidence ?? null}} : null).filter(Boolean);
}

class EvidenceStatistics {
  constructor(config, adsbSource, {filename, fileSystem = fs, now = Date.now,
    setIntervalFn = setInterval, clearIntervalFn = clearInterval,
    flushScheduler = sharedFlushScheduler,
    onEligibilityEvent = () => {}, onPeriodicCapture = () => {},
    onTrackCapture = () => {}, onUnmatchedMotionCapture = () => {},
    timingPolicy = 'exact'} = {}) {
    this.config = config;
    this.adsbSource = adsbSource;
    this.exactTimingRequired = typeof adsbSource?.getAt === 'function';
    if (!['exact', 'provisional'].includes(timingPolicy))
      throw new Error('Evidence timing policy must be exact or provisional');
    this.timingPolicy = timingPolicy;
    this.fs = fileSystem;
    this.now = now;
    this.filename = filename || defaultEvidenceStatisticsPath(config);
    this.flushScheduler = flushScheduler;
    this.persistPhase = parseInt(digest(this.filename).slice(0, 8), 16) % PERSIST_INTERVAL_MS;
    this.key = siteKey(config, this.timingPolicy);
    this.configEpoch = configEpoch(config);
    this.receiverEpoch = receiverEpoch(config);
    this.limits = limits(config);
    this.state = {schema: 'vectorwarp-evidence-statistics-v2', createdAt: now(),
      updatedAt: null, sites: {[this.key]: siteDescriptor(config)}, passes: [], active: {},
      passHistory: [], passHistoryStartedAt: now(),
      eligibilityEvents: [], rollup: emptyRollup(this.key),
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
    this.onUnmatchedMotionCapture = onUnmatchedMotionCapture;
    const unmatched = config.capture?.aircraft_clips?.unmatched_motion || {};
    const setting = (name, fallback, valid = finite) =>
      valid(unmatched[name]) ? unmatched[name] : fallback;
    this.unmatchedMotion = {enabled: unmatched.enabled === true,
      minimumSimilarity: setting('minimum_similarity', .9),
      allowKinematicFallback: unmatched.allow_kinematic_fallback !== false,
      minimumMeasuredFraction: setting('minimum_measured_fraction', .5),
      minimumDelayMotionKm: setting('minimum_delay_motion_km', .25),
      minimumNetDelayMotionKm: setting('minimum_net_delay_motion_km', .05),
      maximumDelayFitRmsKm: setting('maximum_delay_fit_rms_km', .05),
      maximumKinematicErrorMps: setting('maximum_kinematic_error_mps', 20),
      minimumMeasuredCpis: setting('minimum_measured_cpis', 6, Number.isInteger),
      minimumDurationSeconds: setting('minimum_duration_seconds', 5),
      minimumAbsoluteDopplerHz: setting('minimum_absolute_doppler_hz', 30),
      minimumIntervalMs: setting('minimum_interval_seconds', 60) * 1000,
      nextAt: 0, nextScanAt: 0, emitted: new Set()};
    this.passHistoryBytes = 0;
    this.load();
    this.trackPopulation = new TrackPopulationStatistics(this.state.trackPopulation);
    this.state.trackPopulation = this.trackPopulation.state;
    this.trackPopulation.resetSeededReference();
    this.trackPopulation.seedAssociatedEvents(
      [...this.state.passes, ...Object.values(this.state.active)].flatMap(pass =>
        (pass.trackerAssociations || []).map(association => ({association,
          aircraft: pass.hex || pass.flight}))));
    this.scoreContract = evaluationContract(this.limits, this.timingPolicy);
    this.scoreContractHash = digest(this.scoreContract);
    // Different channels have different searchable geometry. Their additive
    // opportunities still require the same truth, timing and scoring policy.
    this.scorePolicyHash = digest(Object.fromEntries(Object.entries(this.scoreContract)
      .filter(([field]) => !['delay_min', 'delay_max', 'doppler_min', 'doppler_max',
        'eligibility_delay_floor_km', 'eligibility_min_absolute_doppler_hz'].includes(field))));
    if (this.state.liveScoreContract?.hash !== this.scoreContractHash) {
      for (const hex of Object.keys(this.state.active)) this.finish(hex);
      this.state.liveScoreContract = {hash: this.scoreContractHash, startedAt: now()};
      this.persist();
    }
    // Multiple exact/provisional and per-channel ledgers share one API event
    // loop.  Writing all of their multi-megabyte bounded snapshots on the same
    // frame creates a periodic multi-second stop-the-world burst.  Preserve the
    // 30-second nominal cadence with fixed wall-clock phases. A shared queue
    // separates overdue/colliding lanes by 250 ms after each completed write;
    // manual and shutdown persistence remain immediate and synchronous.
    this.nextPersistAt = nextPhase(this.now(), this.persistPhase, PERSIST_INTERVAL_MS);
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
        passHistory: Array.isArray(value.passHistory) ? value.passHistory : [],
        // Previously discarded rows cannot be recovered from lifetime totals.
        // Start an honest coverage clock when migrating the older ledger.
        passHistoryStartedAt: finite(value.passHistoryStartedAt) ? value.passHistoryStartedAt : this.now(),
        eligibilityEvents: Array.isArray(value.eligibilityEvents) ? value.eligibilityEvents : [],
        sites: {...(value.sites || {}),
          [this.key]: {...value.sites?.[this.key], ...siteDescriptor(this.config)}}, error: null};
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
      if (this.state.rollup?.siteKey !== this.key) {
        // A contract/configuration rollover starts a new cohort, not a rewrite
        // of archived pass counts (including those already compacted to rollup).
        if (this.state.rollup?.siteKey) {
          const previous = this.state.sites[this.state.rollup.siteKey] ||= {};
          previous.finalized_rollup = this.state.rollup;
        }
        this.state.rollup = emptyRollup(this.key);
      }
      this.state.rollup.trackingFailureClasses ||= {};
      const hasArchivedCpiExposure = ['eligibleCpis', 'detectedCpis', 'trackedCpis']
        .every(field => finite(this.state.rollup[field]));
      if (!hasArchivedCpiExposure) {
        this.state.rollup.cpiExposureIncompleteArchivedPasses =
          Math.max(this.state.rollup.cpiExposureIncompleteArchivedPasses || 0,
            this.state.rollup.passes || 0);
        this.state.rollup.eligibleCpis ||= 0;
        this.state.rollup.detectedCpis ||= 0;
        this.state.rollup.trackedCpis ||= 0;
      }
      this.state.rollup.cpiExposureIncompleteArchivedPasses ||= 0;
      this.state.rollup.associationEligible ??= this.state.rollup.eligible;
      this.state.rollup.associationEligibleCpis ??= this.state.rollup.eligibleCpis;
      this.state.rollup.associationAbstainedCpis ||= 0;
      for (const pass of this.state.passes) compactFinalizedPass(pass);
      const coveragePasses = [...this.state.passHistory, ...this.state.passes, ...Object.values(this.state.active)];
      for (const pass of coveragePasses) if (pass.measuredCoverage)
        for (const field of ['eligible', 'scored', 'measured']) {
          const values = pass.measuredCoverage[field];
          if (!Array.isArray(values) || values.some(interval => !Array.isArray(interval) ||
              interval.length !== 2 || !interval.every(Number.isFinite) || interval[1] <= interval[0]))
            pass.measuredCoverage.complete = false;
          pass.measuredCoverage[field] = measuredCoverage.union(Array.isArray(values) ? values : []);
        }
      boundMeasuredCoverage(coveragePasses);
      this.passHistoryBytes = this.state.passHistory.reduce((sum, pass) =>
        sum + Buffer.byteLength(JSON.stringify(pass)), 0);
      while (this.state.passes.length > MAX_PASSES)
        this.archivePass(this.state.passes.shift());
      this.trimPassHistory();
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
    const nativeLimits = limits(this.config, this.latest.tracker);
    if (nativeLimits) this.trackPopulation.accept(this.latest.tracker, nativeLimits.kmPerBin);
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
    const prior = this.state.sensitivityEpoch;
    if (!prior || item.timestamp_ms > prior.last_at) {
      if (!prior || item.epoch !== prior.capture_epoch ||
          item.first_epoch_sample < prior.first_sample) {
        this.state.sensitivityEpoch = {id: digest([this.key, item.timestamp_ms,
          item.epoch, item.first_epoch_sample]), started_at: item.timestamp_ms};
      }
      Object.assign(this.state.sensitivityEpoch, {capture_epoch: item.epoch,
        first_sample: item.first_epoch_sample, end_sample: item.end_epoch_sample,
        last_at: item.timestamp_ms});
    }
    item.sensitivity_epoch_id = item.timestamp_ms >= this.state.sensitivityEpoch.started_at ?
      this.state.sensitivityEpoch.id : null;
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
      const geographic = geographicContext(this.config);
      if (!geographic.available) throw new Error(geographic.reason);
      const detectionFrame = frameAt(this.detectionHistory, timestamp);
      const trackerFrame = frameAt(this.trackerHistory, timestamp);
      const nativeFrames = [detectionFrame, trackerFrame].filter(Boolean);
      const span = limits(this.config, nativeFrames);
      if (!span) throw new Error('Invalid or contradictory radar observation coordinates; scoring unavailable.');
      const predictions = this.adsbSource.getAt ?
        await this.adsbSource.getAt('delayDoppler', timestamp, nativeFrames) :
        await this.adsbSource.get('delayDoppler');
      const detectionTimestamp = detectionFrame?.timestamp;
      const detectionCells = detectionFrame ? cells(detectionFrame, 1) : [];
      const trackerTimestamp = trackerFrame?.timestamp;
      const trackerCells = trackerFrame ? cells(trackerFrame, span.kmPerBin)
        .filter(point => point.tracker?.state === 'ACTIVE' &&
          point.tracker?.measured !== false && Number(point.tracker?.associations) >= 3) : [];
      const valid = Object.entries(predictions || {}).map(([rawHex, prediction]) =>
        [normalizeHex(rawHex), prediction]).filter(([hex, prediction]) => hex &&
          finite(prediction?.delay) && finite(prediction?.doppler));
      if (valid.some(([, prediction]) => prediction.frequency_hz !== undefined ?
          !finite(prediction.frequency_hz) || prediction.frequency_hz !== span.frequency :
          span.observationCoordinates.frequencySource !== 'validated-live-config-fallback'))
        throw new Error('Truth carrier differs from native observation metadata; scoring unavailable.');
      const timing = timingAt(this.recentTiming, timestamp);
      const uncertaintyPolicy = span.truthUncertaintyPolicy;
      const timingUncertaintyS = this.timingPolicy === 'provisional' &&
        timing?.exact_event_time === false ? timing.event_time_uncertainty_s : 0;
      const eligiblePredictions = uncertaintyPolicy ? valid.filter(([, prediction]) =>
        truthIntegrity(prediction, uncertaintyPolicy, timestamp).valid) : valid;
      // Preserve the existing operational/provisional frame-alignment contract
      // for every SDR without introducing a new receiver prerequisite.
      const timingContext = frameTimestamp => ({declared: finite(timingUncertaintyS),
      model: timing?.exact_event_time === true ? 'exact' : 'existing-operational-provisional',
      frameOffsetS: finite(frameTimestamp) ? (frameTimestamp - timestamp) / 1000 : NaN});
      const detectionDiagnostics = {}, trackerDiagnostics = {};
      const detected = assignments(detectionCells, eligiblePredictions, prediction =>
        associationGates(span, prediction, timestamp, 'detection',
          timingUncertaintyS, timingContext(detectionTimestamp)),
      'detection', detectionTimestamp, detectionDiagnostics);
      const tracked = assignments(trackerCells, eligiblePredictions, prediction =>
        associationGates(span, prediction, timestamp, 'tracker',
          timingUncertaintyS, timingContext(trackerTimestamp)),
      'tracker', trackerTimestamp, trackerDiagnostics);
      for (const [hex, association] of tracked)
        this.trackPopulation.associate(association, hex);
      this.state.associationDiagnostics ||= {detection: {}, tracker: {}};
      for (const [kind, values] of [['detection', detectionDiagnostics],
        ['tracker', trackerDiagnostics]]) for (const [name, value] of Object.entries(values))
        this.state.associationDiagnostics[kind][name] =
          (this.state.associationDiagnostics[kind][name] || 0) + value;
      for (const [hex, prediction] of valid) {
        const association = tracked.get(hex) || null;
        const support = measurementSupport(association, this.detectionHistory);
        const interval = sourceCpiInterval(trackerFrame || detectionFrame);
        const measuredFrameKnown = Array.isArray(trackerFrame?.data) && trackerFrame.data.every(row =>
          typeof row.measured === 'boolean' && !(row.measured && row.source === 'prediction'));
        this.observe(hex, prediction, timestamp,
          detected.get(hex) || null, association,
          timing, uncertaintyPolicy ? associationGates(span, prediction, timestamp,
            'detection', timingUncertaintyS, timingContext(timestamp)) : null, span,
          {scored: measuredFrameKnown && interval !== null && (!association || support !== null), support, interval});
      }
      this.emitUnmatchedMotionCapture(timestamp);
      this.expire(timestamp);
      this.state.updatedAt = this.now(); this.state.error = null;
      if (this.now() >= this.nextPersistAt)
        this.flushScheduler.request(this, () => { if (!this.closed) this.persist(); });
    } catch (error) { this.state.error = error.message; }
    finally {
      this.running -= 1; this.pendingBuckets.delete(bucket); this.completedBuckets.add(bucket);
      while (this.completedBuckets.size > MAX_RADAR_FRAMES)
        this.completedBuckets.delete(this.completedBuckets.values().next().value);
    }
  }

  emitUnmatchedMotionCapture(timestamp = this.now()) {
    const policy = this.unmatchedMotion;
    if (!policy.enabled || this.timingPolicy !== 'provisional' ||
        timestamp < policy.nextAt || timestamp < policy.nextScanAt) return;
    // A missing or rejected candidate is the normal case.  Do not rebuild the
    // complete retained-track population on every CPI merely because no clip
    // was accepted; five seconds still catches a qualifying moving track well
    // inside the configured minimum capture duration.
    policy.nextScanAt = timestamp + 5000;
    const frequencyHz = Number(this.config.capture?.fc);
    const kinematicError = track => finite(frequencyHz) && frequencyHz > 0 &&
      finite(track.features?.measured_delay_slope_km_s) &&
      finite(track.features?.mean_signed_doppler_hz) ? Math.abs(
        track.features.measured_delay_slope_km_s * 1000 +
        track.features.mean_signed_doppler_hz * 299792458 / frequencyHz) : Infinity;
    const candidates = this.trackPopulation.snapshot({includeCaptureCandidates: true})
      .capture_candidates
      .filter(track => track.active === true && track.associated_with_adsb !== true &&
        track.features?.measured_fraction >= policy.minimumMeasuredFraction &&
        track.features?.delay_motion_km >= policy.minimumDelayMotionKm &&
        track.features?.net_delay_motion_km >= policy.minimumNetDelayMotionKm &&
        track.features?.measured_delay_fit_rms_km <= policy.maximumDelayFitRmsKm &&
        kinematicError(track) <= policy.maximumKinematicErrorMps &&
        ((finite(track.similarity?.score) &&
          track.similarity.score >= policy.minimumSimilarity) ||
         (!finite(track.similarity?.score) && policy.allowKinematicFallback &&
          true)) &&
        track.measured_observations >= policy.minimumMeasuredCpis &&
        track.features?.duration_s >= policy.minimumDurationSeconds &&
        track.features?.mean_absolute_doppler_hz >= policy.minimumAbsoluteDopplerHz &&
        !policy.emitted.has(track.track_key))
      .sort((left, right) => (right.similarity.score || 0) -
        (left.similarity.score || 0) ||
        (right.features?.delay_motion_km || 0) - (left.features?.delay_motion_km || 0) ||
        right.measured_observations - left.measured_observations);
    const track = candidates[0];
    if (!track) return;
    const id = `unmatched-motion:${this.configEpoch.slice(0, 12)}:${track.track_key}`;
    const event = {id, pass_id: id, kind: 'unmatched-aircraft-like-motion',
      track_id: track.track_id, state: 'pending', observed_at: timestamp,
      similarity_score: track.similarity.score,
      scoring_basis: finite(track.similarity.score) ? 'empirical-aircraft-similarity' :
        'kinematic-motion-fallback',
      measured_cpis: track.measured_observations,
      measured_fraction: track.features.measured_fraction,
      delay_motion_km: track.features.delay_motion_km,
      net_delay_motion_km: track.features.net_delay_motion_km,
      delay_fit_rms_km: track.features.measured_delay_fit_rms_km,
      kinematic_error_mps: kinematicError(track),
      duration_seconds: track.features.duration_s,
      mean_absolute_doppler_hz: track.features.mean_absolute_doppler_hz,
      recurrence_count: track.similarity.recurrence_count,
      receiver_epoch: this.receiverEpoch, config_epoch: this.configEpoch,
      site_epoch: this.key, frequency_hz: this.config.capture?.fc || null};
    try {
      const capture = this.onUnmatchedMotionCapture(event, this.now());
      if (capture?.accepted !== true) return;
      policy.emitted.add(track.track_key);
      while (policy.emitted.size > 4096)
        policy.emitted.delete(policy.emitted.values().next().value);
      policy.nextAt = this.now() + policy.minimumIntervalMs;
    } catch (error) { this.state.error = error.message; }
  }

  observe(hex, prediction, timestamp, detectedAssociation, trackedAssociation, timing = null,
    uncertaintyAdmission = null, span = this.limits, coverage = null) {
    let pass = this.state.active[hex];
    const coordinates = {sample_rate_hz: span.sampleRate, frequency_hz: span.frequency};
    if (pass && (timestamp - pass.lastAt > PASS_GAP_MS || pass.siteKey !== this.key ||
        pass.observation_coordinates && JSON.stringify(pass.observation_coordinates) !== JSON.stringify(coordinates))) {
      this.finish(hex); pass = null;
    }
    const sensitivityEpoch = timing?.sensitivity_epoch_id || null;
    if (!pass) {
      pass = this.state.active[hex] = emptyPass(hex, prediction.flight,
        timestamp, this.key, this.scoreContractHash);
      pass.sensitivityEpochId = sensitivityEpoch;
    } else if (pass.sensitivityEpochId !== sensitivityEpoch) {
      // Operational whole-pass totals survive a restart, but a pass spanning
      // detector runs (or legacy unknown provenance) is not a paired epoch.
      pass.sensitivityEpochId = null; pass.sensitivityEpochMixed = true;
    }
    pass.observation_coordinates = coordinates;
    const bucket = Math.floor(timestamp / TRUTH_SAMPLE_MS);
    pass.sampledBuckets ||= [];
    if (pass.sampledBuckets.includes(bucket)) return;
    pass.sampledBuckets.push(bucket);
    if (pass.sampledBuckets.length > MAX_CPI_AUDIT) pass.sampledBuckets.shift();
    pass.startedAt = Math.min(pass.startedAt, timestamp);
    pass.lastAt = Math.max(pass.lastAt, timestamp); pass.frames += 1;
    const delay = prediction.delay, doppler = prediction.doppler;
    updateExtent(pass, delay, doppler);
    const currentTruth = updateTruth(pass, prediction, timestamp);
    const delayBin = delay / span.kmPerBin;
    const delayInMap = delayBin >= span.delayMinBin && delayBin <= span.delayMaxBin;
    const aboveFloor = delayBin >= span.detectionMinDelayBin;
    const dopplerInMap = doppler >= span.dopplerMinHz && doppler <= span.dopplerMaxHz;
    const physical = delayInMap && dopplerInMap;
    const outsideNotch = Math.abs(doppler) >= span.detectionMinAbsDopplerHz;
    const notchExcluded = physical && aboveFloor && !outsideNotch;
    const detectorEligible = physical && aboveFloor && outsideNotch;
    if (finite(prediction.doppler_position_fit_hz) &&
        finite(prediction.doppler_kinematic_hz)) {
      const diagnostics = pass.truthDopplerDiagnostics ||= {comparableFrames: 0,
        maximumAbsDifferenceHz: 0, searchableDisagreements: 0};
      diagnostics.comparableFrames += 1;
      diagnostics.maximumAbsDifferenceHz = Math.max(diagnostics.maximumAbsDifferenceHz,
        Math.abs(prediction.doppler_kinematic_hz - prediction.doppler_position_fit_hz));
      const searchable = value => value >= span.dopplerMinHz && value <= span.dopplerMaxHz &&
        Math.abs(value) >= span.detectionMinAbsDopplerHz;
      if (searchable(prediction.doppler_kinematic_hz) !==
          searchable(prediction.doppler_position_fit_hz))
        diagnostics.searchableDisagreements += 1;
    }
    if (physical) pass.physicalEligibleFrames += 1;
    if (detectorEligible) pass.detectorEligibleFrames = (pass.detectorEligibleFrames || 0) + 1;
    if (!aboveFloor) pass.belowFloorFrames += 1;
    if (!delayInMap && delayBin > span.delayMaxBin) pass.aboveDelayFrames += 1;
    if (!dopplerInMap) pass.outsideDopplerFrames += 1;
    if (notchExcluded) pass.dopplerNotchFrames = (pass.dopplerNotchFrames || 0) + 1;
    if (span.clutterMaxBin !== null && delayBin <= span.clutterMaxBin)
      pass.clutterCancelledFrames += 1;
    else if (physical) pass.outsideClutterFrames += 1;
    const integrity = truthIntegrity(prediction, span.truthUncertaintyPolicy, timestamp);
    // Presence establishes exposure; covariance and alignment establish whether
    // a radar association can be credited. Abstention must not erase exposure.
    const associationValid = integrity.valid && (!span.truthUncertaintyPolicy ||
      uncertaintyAdmission?.valid === true);
    const associationReason = !integrity.valid ? integrity.reason : associationValid ? null :
      uncertaintyAdmission?.reason || 'uncertainty-admission-unavailable';
    if (!associationValid) detectedAssociation = trackedAssociation = null;
    const age = finite(prediction.timestamp) ? Math.max(0, timestamp / 1000 - prediction.timestamp) : Infinity;
    pass.cpiAudit ||= [];
    pass.cpiAudit.push({timestamp_ms: timestamp, timing, observation_coordinates: coordinates,
      prediction_timestamp_s: prediction.timestamp,
      observation_time_offset_s: prediction.provenance?.observation_time_offset_s ?? null,
      doppler_position_fit_hz: prediction.doppler_position_fit_hz ?? null,
      doppler_kinematic_hz: prediction.doppler_kinematic_hz ?? null,
      delay_rate_km_s: prediction.delay_rate_km_s ?? null,
      doppler_rate_hz_s: prediction.doppler_rate_hz_s ?? null,
      scored: detectorEligible && associationValid && age <= span.maxTruthAgeS &&
        (!this.exactTimingRequired || Boolean(timing)),
      truth_valid: integrity.valid,
      association_valid: associationValid, association_reason: associationReason,
      truth_reason: integrity.reason, predicted_delay_km: delay, predicted_doppler_hz: doppler,
      map_eligible: physical, detector_eligible: detectorEligible,
      detection_doppler_notch_excluded: notchExcluded,
      physical_eligible: physical, detected: Boolean(detectedAssociation),
      stable_tracked: Boolean(trackedAssociation)});
    pass.cpiAudit.sort((left, right) => left.timestamp_ms - right.timestamp_ms);
    pass.cpiAudit = compactCpiAudit(pass.cpiAudit, MAX_CPI_AUDIT);
    // Once a capture has been triggered, retain fresh event-time truth through
    // its full raw-IQ window even if the target crosses a delay/Doppler boundary.
    // Eligibility still controls scoring; this only prevents the recorder's
    // post-roll from becoming unverifiable at the edge of the processing map.
    let event = pass.eligibilityEventId ?
      this.state.eligibilityEvents.find(item => item.id === pass.eligibilityEventId) : null;
    if (event && age <= span.maxTruthAgeS) {
      if (!Array.isArray(event.truth_samples)) event.truth_samples = [];
      const sample = truthSnapshot(prediction, timestamp);
      if (event.truth_samples.at(-1)?.timestamp_ms !== timestamp)
        event.truth_samples.push(sample);
      event.truth_samples.sort((left, right) => left.timestamp_ms - right.timestamp_ms);
      if (event.truth_samples.length > MAX_EVENT_TRUTH_SAMPLES)
        event.truth_samples.splice(0, event.truth_samples.length - MAX_EVENT_TRUTH_SAMPLES);
      for (const window of event.capture_windows || []) {
        if (timestamp < window.window_start - 3000 || timestamp > window.window_end + 3000)
          continue;
        if (!Array.isArray(window.truth_samples)) window.truth_samples = [];
        if (window.truth_samples.at(-1)?.timestamp_ms !== timestamp)
          window.truth_samples.push(sample);
        if (window.truth_samples.length > MAX_ACTIVE_CAPTURE_TIMESTAMPS)
          window.truth_samples.splice(0,
            window.truth_samples.length - MAX_ACTIVE_CAPTURE_TIMESTAMPS);
      }
    }
    if (age > span.maxTruthAgeS || !integrity.valid) pass.staleTruthFrames += 1;
    if (this.exactTimingRequired && !timing) pass.missingTimingFrames += 1;
    if (!detectorEligible || age > span.maxTruthAgeS || !integrity.valid ||
        (this.exactTimingRequired && !timing)) {
      if (pass.qualityCpi) observeQualityContinuity(pass.qualityCpi, timestamp, span.cpiSeconds, false);
      return;
    }
    pass.associationEligibleFrames = associationCpis(pass) + Number(associationValid);
    pass.eligibleFrames += 1;
    observeMeasuredCoverage(pass, timestamp, span.cpiSeconds,
      associationValid && coverage?.scored === true, coverage?.support, coverage?.interval);
    if (!associationValid) pass.associationAbstainedFrames = (pass.associationAbstainedFrames || 0) + 1;
    if (trackedAssociation) rememberRadarCoverage(this.state, pass, currentTruth);
    pass.firstEligibleAt = finite(pass.firstEligibleAt) ?
      Math.min(pass.firstEligibleAt, timestamp) : timestamp;
    pass.lastEligibleAt = finite(pass.lastEligibleAt) ?
      Math.max(pass.lastEligibleAt, timestamp) : timestamp;
    if (associationValid) observeQualityCpi(pass, Boolean(detectedAssociation),
      Boolean(trackedAssociation), timestamp, span.cpiSeconds);
    else if (pass.qualityCpi)
      observeQualityContinuity(pass.qualityCpi, timestamp, span.cpiSeconds, false);
    const bandDescriptor = delayBand(span, delay);
    const slantDescriptor = fixedBand(SLANT_RANGE_EDGES_KM, currentTruth.slant_range_km);
    const elevationDescriptor = fixedBand(ELEVATION_EDGES_DEG, currentTruth.elevation_deg);
    const azimuthDescriptor = azimuthBand(currentTruth.bearing_deg_true);
    for (const [field, descriptor] of [
      ['delayBands', bandDescriptor], ['slantRangeBands', slantDescriptor],
      ['elevationBands', elevationDescriptor], ['azimuthBands', azimuthDescriptor],
      ['geometryBands', bandDescriptor && slantDescriptor && {
        id: `${bandDescriptor.id}|${slantDescriptor.id}`, delay_band: bandDescriptor.id,
        slant_range_band: slantDescriptor.id}],
      ['azimuthElevationBands', azimuthDescriptor && elevationDescriptor && {
        id: `${azimuthDescriptor.id}|${elevationDescriptor.id}`, azimuth_band: azimuthDescriptor.id,
        elevation_band: elevationDescriptor.id}]
    ]) {
      if (!descriptor) continue;
      pass[field] ||= {};
      const band = pass[field][descriptor.id] ||= {...descriptor,
        eligibleFrames: 0, associationEligibleFrames: 0, detectedFrames: 0, trackedFrames: 0};
      band.associationEligibleFrames = associationCpis(band) + Number(associationValid);
      band.eligibleFrames += 1;
      if (detectedAssociation) band.detectedFrames += 1;
      if (trackedAssociation) band.trackedFrames += 1;
    }
    if (!associationValid) return; // Presence alone does not change automatic capture admission.
    const eligibilityCreated = !pass.eligibilityEventId;
    if (eligibilityCreated) this.emitEligibility(pass, prediction, timestamp);
    event = this.state.eligibilityEvents.find(item => item.id === pass.eligibilityEventId);
    // Preserve one pass/event identity across recorder warmup. Only an
    // explicitly deferred initial admission may be retried on a later eligible
    // observation; accepted and ordinarily rejected events are never replayed.
    if (!eligibilityCreated && event?.capture_state === 'deferred')
      this.attemptInitialCapture(event, pass);
    if (event) {
      if (!Array.isArray(event.truth_samples)) event.truth_samples = [];
      const sample = truthSnapshot(prediction, timestamp);
      if (event.truth_samples.at(-1)?.timestamp_ms !== timestamp)
        event.truth_samples.push(sample);
      event.truth_samples.sort((left, right) => left.timestamp_ms - right.timestamp_ms);
      if (event.truth_samples.length > MAX_EVENT_TRUTH_SAMPLES)
        event.truth_samples.splice(0, event.truth_samples.length - MAX_EVENT_TRUTH_SAMPLES);
      for (const window of event.capture_windows || []) {
        if (timestamp < window.window_start - 3000 || timestamp > window.window_end + 3000)
          continue;
        if (!Array.isArray(window.truth_samples)) window.truth_samples = [];
        if (window.truth_samples.at(-1)?.timestamp_ms !== timestamp)
          window.truth_samples.push(sample);
        if (window.truth_samples.length > MAX_ACTIVE_CAPTURE_TIMESTAMPS)
          window.truth_samples.splice(0,
            window.truth_samples.length - MAX_ACTIVE_CAPTURE_TIMESTAMPS);
      }
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
    if (firstTrack) this.emitCapture(event, pass, trackedAt, 'track');
    this.emitCapture(event, pass, timestamp, 'periodic');
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
    this.attemptInitialCapture(event, pass);
  }

  attemptInitialCapture(event, pass) {
    try {
      const capture = this.onEligibilityEvent({...event});
      if (capture?.deferred === true) {
        event.capture_state = 'deferred';
        event.capture_failure = capture.reason || 'capture-admission-deferred';
        return;
      }
      if (capture?.accepted === true) {
        event.capture_state = capture.dropped === true ? 'dropped' : 'trigger_pending';
        delete event.capture_failure;
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

  emitCapture(event, pass, timestamp, kind) {
    if (!event || event.state !== 'pending') return;
    try {
      const callback = kind === 'track' ? this.onTrackCapture : this.onPeriodicCapture;
      const capture = callback.call(this, {...event}, timestamp);
      if (!capture?.accepted) return;
      if (capture.dropped && capture.window) {
        upsertCaptureWindow(event, capture.window, pass);
        return;
      }
      if (!capture.clip?.clipRevision) return;
      event.capture_clip_revisions = [...new Set([...(event.capture_clip_revisions || []),
        capture.clip.clipRevision])];
      upsertCaptureWindow(event, {captureId: capture.clip.captureIds?.[event.id], kind,
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
      const storedWindow = (event?.capture_windows || []).find(item =>
        item.capture_id === window.captureId);
      const truth = (storedWindow?.truth_samples || event?.truth_samples || []).filter(sample =>
        sample.timestamp_ms >= window.windowStart - 3000 && sample.timestamp_ms <= window.windowEnd + 3000);
      const processorCpis = this.recentTiming.filter(item =>
        item.timestamp_ms >= window.windowStart && item.timestamp_ms <= window.windowEnd);
      const admission = validateTruthWindow(truth, window.windowStart, window.windowEnd,
        {cpiMs, cpiTimestampsMs: processorCpis.map(item => item.timestamp_ms),
          allowPartialCpiCoverage: true,
          minimumCoveredCpis: Math.min(3, processorCpis.length)});
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
    pass.trackingFailureClass = trackingFailureClass(pass, this.limits);
    delete pass.detectionTimestamps;
    delete pass.trackTimestamps;
    compactFinalizedPass(pass);
    this.state.passes.push(pass); delete this.state.active[hex];
    while (this.state.passes.length > MAX_PASSES) {
      this.archivePass(this.state.passes.shift());
    }
  }

  archivePass(pass) {
    accumulateRollup(this.state, this.key, pass);
    if (pass.lastAt >= this.now() - PASS_HISTORY_MS) {
      const summary = compactPassSummary(pass);
      this.state.passHistory.push(summary);
      this.passHistoryBytes = (this.passHistoryBytes || 0) + Buffer.byteLength(JSON.stringify(summary));
    }
    this.trimPassHistory();
  }

  trimPassHistory() {
    const cutoff = this.now() - PASS_HISTORY_MS;
    this.state.passHistory = this.state.passHistory.filter(pass => {
      if (pass.lastAt >= cutoff) return true;
      this.passHistoryBytes -= Buffer.byteLength(JSON.stringify(pass));
      return false;
    });
    while (this.passHistoryBytes > MAX_PASS_HISTORY_BYTES && this.state.passHistory.length) {
      const pass = this.state.passHistory.shift();
      this.passHistoryBytes -= Buffer.byteLength(JSON.stringify(pass));
      this.state.passHistoryStartedAt = Math.max(this.state.passHistoryStartedAt, pass.lastAt + 1);
    }
  }

  retainedPasses() {
    return this.state.passHistory.filter(pass => pass.lastAt >= this.now() - PASS_HISTORY_MS)
      .concat(this.state.passes);
  }

  associationWindow(durationMs = PASS_HISTORY_MS, timestamp = this.now()) {
    const startedAt = Math.max(this.state.passHistoryStartedAt, timestamp - PASS_HISTORY_MS);
    const requestedStart = durationMs === Infinity ? this.state.createdAt : timestamp - durationMs;
    return {duration_ms: Number.isFinite(durationMs) ? durationMs : null,
      requested_started_at: requestedStart, retained_since: startedAt, ended_at: timestamp,
      complete: requestedStart >= startedAt,
      definition: 'finalized whole passes selected by last observation; active passes excluded'};
  }

  expire(timestamp) {
    for (const [hex, pass] of Object.entries(this.state.active))
      if (timestamp - pass.lastAt > PASS_GAP_MS) this.finish(hex);
  }

  persist() {
    this.flushScheduler.cancel(this);
    try {
      this.trimPassHistory();
      // Compact before stringify as well as during load.  This bounds both the
      // live heap and the synchronous atomic-write pause if a source briefly
      // adds aircraft faster than normal expiry can finalize them.
      for (const pass of Object.values(this.state.active)) compactActivePass(pass);
      for (const event of this.state.eligibilityEvents) compactEligibilityEvent(event);
      if (boundMeasuredCoverage([...this.state.passHistory, ...this.state.passes, ...Object.values(this.state.active)]))
        this.passHistoryBytes = this.state.passHistory.reduce((sum, pass) => sum + Buffer.byteLength(JSON.stringify(pass)), 0);
      this.fs.mkdirSync(path.dirname(this.filename), {recursive: true});
      const temporary = `${this.filename}.tmp-${process.pid}`;
      this.fs.writeFileSync(temporary, `${JSON.stringify(this.state)}\n`, {mode: 0o640});
      this.fs.renameSync(temporary, this.filename); this.lastPersistAt = this.now();
      this.nextPersistAt = nextPhase(this.lastPersistAt, this.persistPhase, PERSIST_INTERVAL_MS);
    } catch (error) { this.state.error = error.message; }
  }

  isLivePass(pass) {
    return pass.siteKey === this.key && (pass.evaluationContractHash ?
      pass.evaluationContractHash === this.scoreContractHash :
      pass.startedAt >= (this.state.liveScoreContract?.startedAt ?? -Infinity));
  }

  summarize(duration, timestamp, currentContractOnly = false) {
    const cutoff = duration === Infinity ? -Infinity : timestamp - duration;
    const included = pass => pass.siteKey === this.key && pass.lastAt >= cutoff &&
      (!pass.evaluationContractHash || pass.evaluationContractHash === this.scoreContractHash) &&
      (!currentContractOnly || this.isLivePass(pass));
    const useRollup = duration === Infinity && !currentContractOnly;
    const rows = (useRollup ? this.state.passes : this.retainedPasses()).filter(included);
    const active = Object.values(this.state.active).filter(included);
    const observed = rows.concat(active);
    const eligible = rows.filter(pass => pass.eligibleFrames >= 3);
    const comparable = eligible.filter(pass => associationCpis(pass) >= 3);
    const detected = comparable.filter(pass => pass.detectedFrames >= 3);
    const tracked = comparable.filter(pass => pass.trackedFrames >= 3);
    const detectorFrames = pass => pass.detectorEligibleFrames ?? pass.physicalEligibleFrames ?? 0;
    const outOfConfig = rows.filter(pass => detectorFrames(pass) < 3);
    const truthUnusable = rows.filter(pass => detectorFrames(pass) >= 3 &&
      pass.eligibleFrames < 3);
    const eligibleNoRadar = comparable.filter(pass => pass.detectedFrames < 3 &&
      pass.trackedFrames < 3);
    const detectedNotTracked = comparable.filter(pass => pass.detectedFrames >= 3 &&
      pass.trackedFrames < 3);
    const base = duration === Infinity && !currentContractOnly ?
      this.state.rollup : emptyRollup(this.key);
    const eligibleCount = eligible.length + base.eligible;
    const comparableCount = comparable.length + (base.associationEligible ?? base.eligible);
    const detectedCount = detected.length + base.detected;
    const trackedCount = tracked.length + base.tracked;
    const retainedCpiCounts = sumCpiCounts(observed);
    const exposure = cpiExposure(sumCpiCounts(observed, base), this.limits?.cpiSeconds || .5, {
      complete: !(base.cpiExposureIncompleteArchivedPasses > 0),
      excludedArchivedPasses: base.cpiExposureIncompleteArchivedPasses || 0});
    const liveEligible = observed.filter(pass => pass.eligibleFrames >= 3);
    const liveComparable = liveEligible.filter(pass => associationCpis(pass) >= 3);
    const liveDetected = liveComparable.filter(pass => pass.detectedFrames >= 3);
    const liveTracked = liveComparable.filter(pass => pass.trackedFrames >= 3);
    const trackingFailureClasses = countTrackingFailureClasses(detectedNotTracked);
    for (const [name, count] of Object.entries(base.trackingFailureClasses || {}))
      trackingFailureClasses[name] = (trackingFailureClasses[name] || 0) + count;
    const classifiedRollup = Object.values(base.trackingFailureClasses || {})
      .reduce((sum, count) => sum + count, 0);
    const unclassifiedRollup = Math.max(0, (base.detectedNotTracked || 0) - classifiedRollup);
    if (unclassifiedRollup)
      trackingFailureClasses.unclassified_legacy_rollup = unclassifiedRollup;
    const liveStrata = (definition, edges, field) => ({definition, edges,
      finalized_passes_only: false, bands: summarizeBands(observed, field)});
    return {passes: rows.length + base.passes, active_passes: active.length,
      finalized_pass_window: this.associationWindow(duration, timestamp),
      provisional_eligible_passes: active.filter(pass => pass.eligibleFrames >= 3).length,
      eligible_passes: eligibleCount,
      association_eligible_passes: comparableCount,
      association_unresolved_passes: eligibleCount - comparableCount,
      out_of_config_passes: outOfConfig.length + (base.outOfConfig || 0),
      truth_unusable_passes: truthUnusable.length + (base.truthUnusable || 0),
      detected_passes: detectedCount, tracked_passes: trackedCount,
      missed_passes: comparableCount - trackedCount,
      eligible_no_radar_return: eligibleNoRadar.length + (base.eligibleNoRadar || 0),
      detected_not_tracked: detectedNotTracked.length + (base.detectedNotTracked || 0),
      detected_not_tracked_classes: trackingFailureClasses,
      detection_yield: ratio(detectedCount, comparableCount),
      tracking_yield: ratio(trackedCount, comparableCount),
      denominator_semantics: 'eligible passes have >=3 fresh searchable presence CPIs; yield and misses require >=3 association-eligible CPIs',
      cpi_exposure: exposure,
      measured_track_coverage: measuredCoverage.aggregate([coverageBasis(observed)], {from: cutoff, to: timestamp}),
      live_to_date: {eligible_passes: liveEligible.length,
        association_eligible_passes: liveComparable.length,
        association_unresolved_passes: liveEligible.length - liveComparable.length,
        detected_passes: liveDetected.length, tracked_passes: liveTracked.length,
        detection_yield: ratio(liveDetected.length, liveComparable.length),
        tracking_yield: ratio(liveTracked.length, liveComparable.length),
        cpi_exposure: cpiExposure(retainedCpiCounts,
          this.limits?.cpiSeconds || .5),
        delay_strata: liveStrata('excess_bistatic_path_km', delayBandEdges(this.limits),
          'delayBands'),
        receiver_slant_range_strata: liveStrata('receiver_to_aircraft_3d_slant_range_km',
          SLANT_RANGE_EDGES_KM, 'slantRangeBands'),
        elevation_strata: liveStrata('receiver_geometric_elevation_deg',
          ELEVATION_EDGES_DEG, 'elevationBands'),
        azimuth_strata: liveStrata('receiver_true_azimuth_deg',
          AZIMUTH_EDGES_DEG, 'azimuthBands'),
        azimuth_elevation_strata: liveStrata(
          'receiver_true_azimuth_x_geometric_elevation', [], 'azimuthElevationBands')},
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
        inside_detection_doppler_notch: observed.reduce((sum, pass) =>
          sum + (pass.dopplerNotchFrames || 0), base.dopplerNotchFrames || 0),
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
      timing_policy: this.timingPolicy,
      collecting: this.config.truth?.adsb?.enabled === true && Boolean(this.limits) && geographicContext(this.config).available,
      geographic_provenance: geographicContext(this.config),
      error: this.state.error, limits: this.limits,
      eligibility_event_count: this.state.eligibilityEvents.length,
      capture_admission: this.state.captureAdmission,
      event_time: {rejections: this.eventTimeRejections,
        last_rejection: this.lastEventTimeRejection},
      track_population: this.trackPopulation.snapshot(),
      windows: Object.fromEntries(Object.entries(WINDOWS).map(([name, duration]) =>
        [name, this.summarize(duration, timestamp)])),
      bearing: {...bearing, live: {tracks: live.length,
        publishable: live.filter(item => item.publishable === true).length,
        resolved: live.filter(item => item.resolved === true).length,
        stationary: live.filter(item => item.stationary_clutter === true).length}}};
  }

  associationQuality(timestamp) {
    const audits = [...this.state.passes.filter(pass => this.isLivePass(pass)).slice(-50),
      ...Object.values(this.state.active).filter(pass => this.isLivePass(pass))]
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
    const warning = this.timingPolicy === 'provisional' ?
      'Operational association uses an inexact receiver time estimate; do not use for acceptance decisions.' :
      (!decisionGrade && this.eventTimeRejections > 0 ?
        'Authoritative scoring is blocked because the receiver has not supplied exact RF event time.' :
        (!decisionGrade ? 'Authoritative scoring is waiting for an exact-timed CPI.' : null));
    return {audits, exactTiming, joinFrames, decisionGrade, warning};
  }

  associationStatus() {
    const timestamp = this.now();
    const quality = this.associationQuality(timestamp);
    return {updated_at: this.state.updatedAt, timing_policy: this.timingPolicy,
      decision_grade: quality.decisionGrade, warning: quality.warning,
      evaluation_contract_hash: this.scoreContractHash,
      evaluation_policy_hash: this.scorePolicyHash,
      evaluation_contract_started_at: this.state.liveScoreContract?.startedAt || timestamp,
      sensitivity_epoch: this.state.sensitivityEpoch || null,
      current_setup_lifetime: this.summarize(Infinity, timestamp),
      data_quality: {audited_aircraft_cpis: quality.audits.length,
        exact_timed_aircraft_cpis: quality.exactTiming}};
  }

  liveStatus() {
    const timestamp = this.now();
    const contractStartedAt = this.state.liveScoreContract?.startedAt || timestamp;
    const active = Object.values(this.state.active).filter(pass => this.isLivePass(pass))
      .map(pass => ({pass_id: pass.id, hex: pass.hex, flight: pass.flight,
        started_at: pass.startedAt, last_at: pass.lastAt,
        status: pass.trackedFrames >= 3 ? 'provisional_tracked' :
          pass.detectedFrames >= 3 ? 'provisional_detected' :
            associationCpis(pass) >= 3 ? 'provisional_miss' :
              pass.eligibleFrames >= 3 ? 'provisional_association_unresolved' : 'collecting',
        ...passExposure(pass, this.limits?.cpiSeconds || .5),
        last_truth: pass.lastTruth ? {delay_km: pass.lastTruth.delay_km,
          doppler_hz: pass.lastTruth.doppler_hz,
          bearing_deg_true: pass.lastTruth.bearing_deg_true,
          ground_range_km: pass.lastTruth.ground_range_km,
          elevation_deg: pass.lastTruth.elevation_deg} : null}));
    const finalizedPasses = this.retainedPasses().filter(pass => this.isLivePass(pass))
      .sort((left, right) => right.lastAt - left.lastAt || right.startedAt - left.startedAt);
    const passSummary = pass => ({pass_id: pass.id, hex: pass.hex,
        flight: pass.flight, started_at: pass.startedAt, finalized_at: pass.lastAt,
        outcome: eligibilityOutcome(pass), ...passExposure(pass, this.limits?.cpiSeconds || .5),
        tracking_failure_class: pass.trackingFailureClass ||
          (pass.detectedFrames >= 3 && pass.trackedFrames < 3 ?
            'unclassified_retained_legacy' : null),
        delay_km: {minimum: pass.minDelayKm, maximum: pass.maxDelayKm},
        receiver_slant_range_km: {minimum: pass.minSlantRangeKm,
          maximum: pass.maxSlantRangeKm},
        transmitter_range_km: {minimum: pass.minTransmitterRangeKm,
          maximum: pass.maxTransmitterRangeKm},
        bistatic_range_product_km2: {minimum: pass.minBistaticRangeProductKm2,
          maximum: pass.maxBistaticRangeProductKm2}});
    const finalized = finalizedPasses.slice(0, 50).map(passSummary);
    const scoredFinalized = finalizedPasses.filter(pass => associationCpis(pass) >= 3)
      .slice(0, 50).map(passSummary);
    const {audits, exactTiming, joinFrames, decisionGrade, warning} =
      this.associationQuality(timestamp);
    const sensitivityEpoch = this.state.sensitivityEpoch || null;
    const sameRun = pass => sensitivityEpoch && !pass.sensitivityEpochMixed &&
      pass.sensitivityEpochId === sensitivityEpoch.id;
    const hour = end => this.retainedPasses().filter(pass => this.isLivePass(pass) && sameRun(pass) &&
      pass.lastAt >= end - WINDOWS.hour && pass.lastAt < end && associationCpis(pass) >= 3);
    const mixAdjusted = standardizedComparison(hour(timestamp), hour(timestamp - WINDOWS.hour));
    return {schema: 'vectorwarp-live-aircraft-scorecard-v1', updated_at: this.state.updatedAt,
      timing_policy: this.timingPolicy, decision_grade: decisionGrade, warning,
      adsb_quality: adsbQuality(this.config),
      collecting: this.config.truth?.adsb?.enabled === true && Boolean(this.limits) && geographicContext(this.config).available,
      geographic_provenance: geographicContext(this.config),
      error: this.state.error, config_epoch: this.configEpoch,
      receiver_epoch: this.receiverEpoch, site_epoch: this.key,
      evaluation_contract_hash: this.scoreContractHash,
      evaluation_policy_hash: this.scorePolicyHash,
      evaluation_contract: {...this.scoreContract},
      evaluation_contract_started_at: contractStartedAt,
      sensitivity_epoch: sensitivityEpoch,
      association_lag_cpis: this.config.truth?.adsb?.association_lag_cpis || 0,
      association_lag_ms: (this.config.truth?.adsb?.association_lag_cpis || 0) *
        (this.limits?.cpiSeconds || .5) * 1000,
      provisional_event_time_uncertainty_s: this.timingPolicy === 'provisional' ?
        this.limits?.provisionalTimeUncertaintyS : 0,
      limits: {...this.limits, delay_definition: 'excess_bistatic_path_km',
        geometry_model: 'three-dimensional-WGS84-bistatic-slant-path'},
      windows: Object.fromEntries(Object.entries(WINDOWS).map(
        ([name, duration]) => [name, this.summarize(duration, timestamp, true)])),
      current_setup_lifetime: this.summarize(Infinity, timestamp),
      mix_adjusted_previous_hour_comparison: {...mixAdjusted,
        sensitivity_epoch_id: sensitivityEpoch?.id || null,
        excluded_cross_run_or_unknown_passes: finalizedPasses.filter(pass =>
          associationCpis(pass) >= 3 && !sameRun(pass)).length,
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
      track_population: this.trackPopulation.snapshot({since: contractStartedAt}),
      active_passes: active, recent_finalized_passes: finalized,
      recent_scored_passes: scoredFinalized};
  }

  // Live maps need only the identity and current radar-support counters.  Do
  // not build the full scorecard (historical windows, interval unions and
  // track-population quantiles) for their one-second presentation poll.
  activeAssociations() {
    return Object.values(this.state.active).filter(pass => this.isLivePass(pass))
      .map(pass => ({hex: pass.hex, flight: pass.flight || '',
        detected_cpis: pass.detectedFrames || 0, tracked_cpis: pass.trackedFrames || 0,
        last_at: pass.lastAt || 0}));
  }

  associationPasses({durationMs = Infinity, sensitivityEpochId = null,
    federation = false} = {}) {
    const cutoff = durationMs === Infinity ? -Infinity : this.now() - durationMs;
    const bands = (values, axes = ['minimum', 'maximum']) => Object.fromEntries(Object.entries(values || {})
      .filter(([, band]) => associationCpis(band) >= 3)
      .map(([id, band]) => [id, {...Object.fromEntries(axes.map(key => [key, band[key]])),
        eligible: true, detected: (band.detectedFrames || 0) >= 3,
        tracked: (band.trackedFrames || 0) >= 3}]));
    return this.retainedPasses().filter(pass => this.isLivePass(pass) && pass.lastAt >= cutoff &&
      (!sensitivityEpochId || !pass.sensitivityEpochMixed && pass.sensitivityEpochId === sensitivityEpochId) &&
      associationCpis(pass) >= 3).map(pass => {
      const shared = {pass_id: pass.id || null, hex: pass.hex || null, flight: pass.flight || '',
      site_epoch: pass.siteKey, evaluation_contract_hash: pass.evaluationContractHash || null,
      evaluation_policy_hash: this.scorePolicyHash, timing_policy: this.timingPolicy,
      decision_grade: this.timingPolicy === 'exact' && pass.missingTimingFrames === 0 &&
        Boolean(pass.sensitivityEpochId) && pass.sensitivityEpochMixed !== true,
      started_at: pass.startedAt, finalized_at: pass.lastAt};
      if (federation) return {...shared, outcome: eligibilityOutcome(pass),
        sensitivity_epoch_id: pass.sensitivityEpochId || null,
        sensitivity_epoch_mixed: pass.sensitivityEpochMixed === true,
        eligible_cpis: pass.eligibleFrames, association_eligible_cpis: associationCpis(pass),
        detected_cpis: pass.detectedFrames, tracked_cpis: pass.trackedFrames,
        eligible: true, detected: pass.detectedFrames >= 3, tracked: pass.trackedFrames >= 3};
      return {...shared,
      ...passExposure(pass, this.limits?.cpiSeconds || .5),
      detected: pass.detectedFrames >= 3, tracked: pass.trackedFrames >= 3,
      truth_doppler_diagnostics: {
        comparable_cpis: pass.truthDopplerDiagnostics?.comparableFrames || 0,
        maximum_absolute_difference_hz:
          pass.truthDopplerDiagnostics?.maximumAbsDifferenceHz || 0,
        searchable_disagreement_cpis:
          pass.truthDopplerDiagnostics?.searchableDisagreements || 0},
      delay_bands: bands(pass.delayBands),
      receiver_slant_range_bands: bands(pass.slantRangeBands),
      elevation_bands: bands(pass.elevationBands),
      azimuth_bands: bands(pass.azimuthBands),
      azimuth_elevation_bands: bands(pass.azimuthElevationBands, ['azimuth_band', 'elevation_band'])};
    });
  }

  measuredTrackBasis() {
    return {...coverageBasis([...this.retainedPasses(), ...Object.values(this.state.active)]
      .filter(pass => this.isLivePass(pass))),
    timing_policy: this.timingPolicy};
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
    // A service restart is not the end of every aircraft pass. Persist active
    // passes so a brief API restart cannot manufacture a batch of finalized
    // misses. Normal gap expiry, a changed scoring contract, or changed
    // observation coordinates will finalize them after recovery.
    this.persist();
  }
}

function createEvidenceStatistics(config, adsbSource, dependencies) {
  return new EvidenceStatistics(config, adsbSource, dependencies);
}

function defaultEvidenceStatisticsPath(config, environment = process.env) {
  if (environment.BLAH2_STATISTICS_PATH) return environment.BLAH2_STATISTICS_PATH;
  const recorderPath = config.process?.bearing?.manifold_recorder?.path;
  if (recorderPath) return path.join(path.dirname(recorderPath), 'evidence-statistics.json');
  if (environment.VECTORWARP_MACOS_STATE)
    return path.join(environment.VECTORWARP_MACOS_STATE, 'evidence-statistics.json');
  return '/var/lib/vectorwarp-api/evidence-statistics.json';
}

function defaultProvisionalEvidenceStatisticsPath(config, environment = process.env) {
  if (environment.BLAH2_PROVISIONAL_STATISTICS_PATH)
    return environment.BLAH2_PROVISIONAL_STATISTICS_PATH;
  const exact = path.parse(defaultEvidenceStatisticsPath(config, environment));
  return path.join(exact.dir, `${exact.name}-provisional${exact.ext}`);
}

// Exactly one lane owns operational side effects. It uses qualified exact
// timing (and zero expansion) when present, otherwise configured provisional
// uncertainty. The parallel exact scorecard never creates a duplicate capture.
function createEvidenceStatisticsLanes(config, adsbSource, {exact = {}, operational = {}} = {}) {
  return {
    exact: new EvidenceStatistics(config, adsbSource, {...exact, timingPolicy: 'exact',
      onEligibilityEvent: () => {}, onPeriodicCapture: () => {}, onTrackCapture: () => {},
      onUnmatchedMotionCapture: () => {}}),
    operational: new EvidenceStatistics(config, adsbSource, {...operational, timingPolicy: 'provisional'})
  };
}

module.exports = {SPEED_OF_LIGHT_KM_S, MAX_ELIGIBILITY_EVENTS, MAX_EVENT_TRUTH_SAMPLES,
  nextPhase, StatisticsFlushScheduler, compactAssociationPass,
  federationAssociationPassTransport, expandFederationAssociationPassTransport,
  TRIGGER_EVENT_TRUTH_SAMPLES, TRUTH_SAMPLE_MS, MAX_ACTIVE_CAPTURE_TIMESTAMPS, MAX_CAPTURE_ASSOCIATIONS,
  limits, associationGates, assignments, delayBandEdges, delayBand, adsbQuality,
  trackingFailureClass,
  measurementSupport, coverageBasis, observeMeasuredCoverage, sourceCpiInterval, boundMeasuredCoverage,
  ELEVATION_EDGES_DEG, AZIMUTH_EDGES_DEG,
  RADAR_COVERAGE_ALTITUDE_BANDS, radarCoverageFromPasses,
  radarCoverageFromAccumulator, rememberRadarCoverage,
  primaryGeometryBand, standardizedComparison,
  siteKey, siteDescriptor, EvidenceStatistics,
  provisionalTimingFrame, defaultEvidenceStatisticsPath,
  defaultProvisionalEvidenceStatisticsPath,
  createEvidenceStatistics, createEvidenceStatisticsLanes};
