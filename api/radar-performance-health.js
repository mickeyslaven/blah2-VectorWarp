'use strict';

const {birthRejectionLabels} = require('./causal-funnel-contract');
const finiteCount = value => Number.isSafeInteger(value) && value >= 0 ? value : null;
const median = values => {
  if (!values.length) return 0;
  const ordered = [...values].sort((left, right) => left - right);
  const middle = Math.floor(ordered.length / 2);
  return ordered.length % 2 ? ordered[middle] :
    (ordered[middle - 1] + ordered[middle]) / 2;
};

function trackerFrame(frame) {
  if (typeof frame === 'string') {
    try { frame = JSON.parse(frame); } catch (_) { return null; }
  }
  return frame && typeof frame === 'object' ? frame : null;
}

function trackerCounts(frame) {
  frame = trackerFrame(frame);
  if (!frame) return null;
  const timestamp = finiteCount(frame.timestamp);
  const total = finiteCount(frame.n);
  const tentative = finiteCount(frame.nTentative);
  const associated = finiteCount(frame.nAssociated);
  const active = finiteCount(frame.nActive);
  const confirmed = frame.nConfirmed === undefined ? 0 : finiteCount(frame.nConfirmed);
  const coasting = finiteCount(frame.nCoasting);
  if (timestamp === null || total === null || tentative === null ||
      associated === null || active === null || confirmed === null ||
      coasting === null || tentative + associated + active + confirmed + coasting > total)
    return null;
  return {
    timestamp,
    tentative, associated, active, confirmed, coasting,
    candidates: tentative + associated,
    operational: active + confirmed,
    total
  };
}

function diagnosticCounts(value) {
  if (!value || typeof value !== 'object') return null;
  const fields = ['detections', 'birth_candidates', 'birth_snr_rejected',
    'birth_ridge_rejected', 'initiated', 'associations', 'promotions', 'misses',
    'tentative_expired', 'inactivity_deleted'];
  const result = {};
  for (const field of fields) {
    const count = finiteCount(value[field]);
    if (count === null) return null;
    result[field] = count;
  }
  if (result.birth_snr_rejected + result.birth_ridge_rejected >
      result.birth_candidates)
    return null;
  for (const field of Object.keys(birthRejectionLabels)) {
    if (field in result) continue;
    result[field] = value[field] === undefined ? null : finiteCount(value[field]);
    if (value[field] !== undefined && result[field] === null) return null;
  }
  return result;
}

function rejectionSummary(diagnostics) {
  const fields = Object.keys(birthRejectionLabels);
  const complete = diagnostics.filter(value => fields.every(field => value[field] !== null));
  return {
    rejectionAccountingCoverage: diagnostics.length ? complete.length / diagnostics.length : 0,
    rejections: Object.fromEntries(fields.map(field => [field,
      diagnostics.length && diagnostics.every(value => value[field] !== null) ?
        diagnostics.reduce((sum, value) => sum + value[field], 0) : null]))
  };
}

function observationPolicy(identity, window, minimumSamples) {
  if (identity === undefined) return {identity, window, minimumSamples}; // Legacy publication.
  if (typeof identity !== 'string' || identity.length > 32768) return null;
  let plan;
  try { plan = JSON.parse(identity); } catch (_) { return null; }
  if (plan?.schema !== 'vectorwarp-resolved-products-v1' ||
      !Number.isInteger(plan.trackerN) || plan.trackerN < 1 || plan.trackerN > 255)
    return null;
  // N already belongs to the native immutable plan. Do not rederive tracker
  // math or judge its candidate population before one confirmation opportunity.
  return {identity, window: Math.max(window, plan.trackerN),
    minimumSamples: Math.max(minimumSamples, plan.trackerN)};
}

class RadarPerformanceHealth {
  constructor({window = 12, minimumSamples = 6, candidateLimit = 29,
    candidateToOperationalLimit = 8, freshnessMs = 10000} = {}) {
    this.window = window;
    this.minimumSamples = minimumSamples;
    this.candidateLimit = candidateLimit;
    this.candidateToOperationalLimit = candidateToOperationalLimit;
    this.freshnessMs = freshnessMs;
    this.channels = new Map();
    this.policies = new Map();
    this.rejected = new Map();
    this.expectedChannels = new Set();
  }

  accept(frame, {channel = 'primary', diagnostics = null,
    resetOnTimestampRegression = false} = {}) {
    frame = trackerFrame(frame);
    const counts = trackerCounts(frame);
    const policy = observationPolicy(frame?.resolved_product_plan, this.window, this.minimumSamples);
    const previousPolicy = this.policies.get(channel);
    if (!counts || !policy || (previousPolicy?.identity !== undefined && policy.identity === undefined)) {
      const prior = this.rejected.get(channel) || {count: 0, at: 0};
      this.rejected.set(channel, {count: prior.count + 1, at: Date.now()});
      return false;
    }
    let samples = this.channels.get(channel) || [];
    if (resetOnTimestampRegression && samples.length &&
        counts.timestamp < samples.at(-1).timestamp) {
      this.reset(channel);
      samples = [];
    }
    if (samples.length && counts.timestamp <= samples.at(-1).timestamp) {
      const prior = this.rejected.get(channel) || {count: 0, at: 0};
      this.rejected.set(channel, {count: prior.count + 1, at: Date.now()});
      return false;
    }
    if (previousPolicy && previousPolicy.identity !== policy.identity) {
      this.reset(channel);
      samples = [];
    }
    samples.push({...counts, diagnostics: diagnosticCounts(diagnostics), acceptedAt: Date.now()});
    if (samples.length > policy.window) samples.splice(0, samples.length - policy.window);
    this.channels.set(channel, samples);
    this.policies.set(channel, policy);
    return true;
  }

  reset(channel = null) {
    if (channel === null) {
      this.channels.clear();
      this.policies.clear();
      this.rejected.clear();
    } else {
      this.channels.delete(channel);
      this.policies.delete(channel);
      this.rejected.delete(channel);
    }
  }

  expect(channels) {
    this.expectedChannels = new Set((channels || []).filter(channel =>
      typeof channel === 'string' && channel.length));
  }

  snapshot() {
    const channels = [...new Set([...this.expectedChannels, ...this.channels.keys()])].map(channel => {
      const samples = this.channels.get(channel) || [];
      if (!samples.length) return {channel, samples: 0, latest: null,
        rollingMedian: {candidates: 0, operational: 0, candidateToOperationalRatio: 0},
        input: {status: 'missing', rejected: this.rejected.get(channel)?.count || 0},
        candidateBurden: {status: 'collecting', excessive: false},
        conversion: {status: 'unknown', diagnosticCoverage: 0, birthCandidates: 0,
          initiated: 0, promotions: 0, associations: 0, initiationHypothesesPerCandidate: null,
          ...rejectionSummary([])}, excessive: false};
      const latest = samples.at(-1);
      const policy = this.policies.get(channel);
      const candidates = median(samples.map(sample => sample.candidates));
      const operational = median(samples.map(sample => sample.operational));
      const ratio = operational > 0 ? candidates / operational :
        candidates > 0 ? null : 0;
      const enough = samples.length >= policy.minimumSamples;
      const excessive = enough && candidates >= this.candidateLimit &&
        (operational === 0 || ratio >= this.candidateToOperationalLimit);
      const diagnostics = samples.map(sample => sample.diagnostics).filter(Boolean);
      const diagnosticCoverage = diagnostics.length / samples.length;
      const birthCandidates = diagnostics.reduce((sum, value) => sum + value.birth_candidates, 0);
      const initiated = diagnostics.reduce((sum, value) => sum + value.initiated, 0);
      const promotions = diagnostics.reduce((sum, value) => sum + value.promotions, 0);
      const associations = diagnostics.reduce((sum, value) => sum + value.associations, 0);
      // A candidate can seed several acceleration hypotheses. Capacity failures
      // may also count hypotheses, so neither counter is a unique-birth rate.
      const initiationHypothesesPerCandidate = birthCandidates ? initiated / birthCandidates : null;
      const conversionAttention = enough && diagnosticCoverage === 1 &&
        birthCandidates >= 24 && initiationHypothesesPerCandidate < .1;
      const rejection = this.rejected.get(channel);
      const inputStatus = rejection && Date.now() - rejection.at <= this.freshnessMs ?
        'degraded' : 'healthy';
      const conversionStatus = !enough ? 'collecting' : diagnosticCoverage < 1 ? 'unknown' :
        conversionAttention ? 'attention' : birthCandidates ? 'healthy' : 'empty';
      // Producer timestamps describe signal/replay ordering and can be
      // historical. Liveness is based on when this process accepted a frame.
      const stale = Date.now() - latest.acceptedAt > this.freshnessMs;
      return {channel, samples: samples.length, latest,
        observationWindow: {samples: policy.window, minimumSamples: policy.minimumSamples,
          spanMs: latest.timestamp - samples[0].timestamp},
        rollingMedian: {candidates, operational,
          candidateToOperationalRatio: ratio},
        input: {status: stale ? 'stale' : inputStatus,
          rejected: rejection?.count || 0},
        candidateBurden: {status: excessive ? 'degraded' : enough ? 'healthy' : 'collecting',
          excessive},
        conversion: {status: conversionStatus, diagnosticCoverage, birthCandidates,
          initiated, promotions, associations, initiationHypothesesPerCandidate,
          ...rejectionSummary(diagnostics)},
        excessive: excessive || inputStatus === 'degraded' || stale};
    }).sort((left, right) => left.channel.localeCompare(right.channel));
    const excessive = channels.filter(channel => channel.excessive);
    const unknown = channels.some(channel => channel.conversion.status === 'unknown' ||
      channel.input.status === 'missing');
    const attention = channels.some(channel => channel.conversion.status === 'attention');
    return {
      schema: 'vectorwarp-radar-performance-health-v3',
      status: excessive.length ? 'degraded' :
        !channels.length || channels.some(channel => channel.candidateBurden.status === 'collecting') ?
          'collecting' : unknown ? 'unknown' : attention ? 'attention' : 'healthy',
      limits: {candidateBurden: this.candidateLimit,
        candidateToOperationalRatio: this.candidateToOperationalLimit,
        minimumSamples: Math.max(this.minimumSamples, ...channels.map(channel =>
          channel.observationWindow?.minimumSamples || 0)),
        window: Math.max(this.window, ...channels.map(channel => channel.observationWindow?.samples || 0))},
      channels,
      reasons: excessive.flatMap(channel => {
        const reasons = [];
        if (channel.input.status === 'degraded') reasons.push(
          `${channel.channel} rejected malformed, duplicate, or out-of-order tracker frames.`);
        if (channel.input.status === 'stale') reasons.push(
          `${channel.channel} tracker diagnostics are stale.`);
        if (channel.candidateBurden.excessive) reasons.push(
          `${channel.channel} sustained ${channel.rollingMedian.candidates} candidates for ` +
          `${channel.rollingMedian.operational} operational tracks.`);
        return reasons;
      }).concat(channels.filter(channel => channel.conversion.status === 'attention').map(channel => {
        const conversion = channel.conversion;
        const rejected = Object.entries(conversion.rejections).filter(([, count]) => count > 0)
          .map(([field, count]) => `${birthRejectionLabels[field]} ${count}`).join(', ') || 'none reported';
        return `${channel.channel} initiated ${conversion.initiated} track hypotheses from ` +
          `${conversion.birthCandidates} unmatched birth candidates; rejection counters: ${rejected}.` +
          (conversion.rejectionAccountingCoverage < 1 ?
            ` Complete rejection accounting is available for ${Math.floor(conversion.rejectionAccountingCoverage * 100)}% of diagnostic frames.` : '');
      }))
    };
  }
}

module.exports = {RadarPerformanceHealth, trackerCounts, diagnosticCounts};
