'use strict';

const finiteCount = value => Number.isSafeInteger(value) && value >= 0 ? value : 0;
const median = values => {
  if (!values.length) return 0;
  const ordered = [...values].sort((left, right) => left - right);
  const middle = Math.floor(ordered.length / 2);
  return ordered.length % 2 ? ordered[middle] :
    (ordered[middle - 1] + ordered[middle]) / 2;
};

function trackerCounts(frame) {
  if (typeof frame === 'string') {
    try { frame = JSON.parse(frame); } catch (_) { return null; }
  }
  if (!frame || typeof frame !== 'object') return null;
  const tentative = finiteCount(frame.nTentative);
  const associated = finiteCount(frame.nAssociated);
  const active = finiteCount(frame.nActive);
  const confirmed = finiteCount(frame.nConfirmed);
  const coasting = finiteCount(frame.nCoasting);
  return {
    timestamp: Number.isSafeInteger(frame.timestamp) ? frame.timestamp : Date.now(),
    tentative, associated, active, confirmed, coasting,
    candidates: tentative + associated,
    operational: active + confirmed,
    total: finiteCount(frame.n) || tentative + associated + active + confirmed + coasting
  };
}

class RadarPerformanceHealth {
  constructor({window = 12, minimumSamples = 6, candidateLimit = 29,
    candidateToOperationalLimit = 8} = {}) {
    this.window = window;
    this.minimumSamples = minimumSamples;
    this.candidateLimit = candidateLimit;
    this.candidateToOperationalLimit = candidateToOperationalLimit;
    this.channels = new Map();
  }

  accept(frame, {channel = 'primary'} = {}) {
    const counts = trackerCounts(frame);
    if (!counts) return false;
    const samples = this.channels.get(channel) || [];
    samples.push(counts);
    if (samples.length > this.window) samples.splice(0, samples.length - this.window);
    this.channels.set(channel, samples);
    return true;
  }

  snapshot() {
    const channels = [...this.channels.entries()].map(([channel, samples]) => {
      const latest = samples.at(-1);
      const candidates = median(samples.map(sample => sample.candidates));
      const operational = median(samples.map(sample => sample.operational));
      const ratio = operational > 0 ? candidates / operational :
        candidates > 0 ? null : 0;
      const enough = samples.length >= this.minimumSamples;
      const excessive = enough && candidates >= this.candidateLimit &&
        (operational === 0 || ratio >= this.candidateToOperationalLimit);
      return {channel, samples: samples.length, latest,
        rollingMedian: {candidates, operational,
          candidateToOperationalRatio: ratio}, excessive};
    }).sort((left, right) => left.channel.localeCompare(right.channel));
    const excessive = channels.filter(channel => channel.excessive);
    return {
      schema: 'vectorwarp-radar-performance-health-v1',
      status: excessive.length ? 'degraded' :
        !channels.length || channels.some(channel => channel.samples < this.minimumSamples) ?
          'collecting' : 'healthy',
      limits: {candidateBurden: this.candidateLimit,
        candidateToOperationalRatio: this.candidateToOperationalLimit,
        minimumSamples: this.minimumSamples, window: this.window},
      channels,
      reasons: excessive.map(channel =>
        `${channel.channel} sustained ${channel.rollingMedian.candidates} candidates for ` +
        `${channel.rollingMedian.operational} operational tracks.`)
    };
  }
}

module.exports = {RadarPerformanceHealth, trackerCounts};
