// Shared evaluation-only interval mathematics for the API and statistics view.
// ADS-B identifies the denominator; none of these values feed radar inference.
(function(root, factory) {
  if (typeof module !== 'undefined' && module.exports) module.exports = factory();
  else root.VectorWarpMeasuredCoverage = factory();
})(typeof globalThis !== 'undefined' ? globalThis : this, function() {
  'use strict';
  const valid = interval => Array.isArray(interval) && interval.length === 2 &&
    interval.every(Number.isFinite) && interval[1] > interval[0];
  function union(intervals = []) {
    const result = [];
    for (const [start, end] of intervals.filter(valid).sort((a, b) => a[0] - b[0] || a[1] - b[1])) {
      const last = result[result.length - 1];
      if (last && start <= last[1]) last[1] = Math.max(last[1], end);
      else result.push([start, end]);
    }
    return result;
  }
  // Extend a canonical sorted union in place. Live CPI ingestion normally
  // touches only its tail; temporal support can reach back a few intervals.
  function extendUnion(intervals, additions = []) {
    for (const interval of additions) {
      if (!valid(interval)) continue;
      let [start, end] = interval, lower = 0, upper = intervals.length;
      while (lower < upper) {
        const middle = (lower + upper) >>> 1;
        if (intervals[middle][1] < start) lower = middle + 1;
        else upper = middle;
      }
      let next = lower;
      while (next < intervals.length && intervals[next][0] <= end) {
        start = Math.min(start, intervals[next][0]); end = Math.max(end, intervals[next][1]); ++next;
      }
      intervals.splice(lower, next - lower, [start, end]);
    }
    return intervals;
  }
  function intersect(left, right) {
    const result = []; let i = 0, j = 0;
    while (i < left.length && j < right.length) {
      const start = Math.max(left[i][0], right[j][0]), end = Math.min(left[i][1], right[j][1]);
      if (end > start) result.push([start, end]);
      if (left[i][1] < right[j][1]) ++i; else ++j;
    }
    return result;
  }
  const milliseconds = intervals => intervals.reduce((sum, [start, end]) => sum + end - start, 0);
  const quantile = (sorted, fraction) => sorted.length ? sorted[Math.ceil(sorted.length * fraction) - 1] : null;

  // Lossless wire projection for portrait dashboards. Sparse exposure breaks
  // remain breaks: no time rounding, gap filling, interval sampling or row cap.
  function compactBasis(source) {
    if (source?.version !== 1 || !Array.isArray(source.aircraft)) return source;
    const aircraft = [];
    for (const row of source.aircraft) {
      if (!row || !['eligible', 'scored', 'measured'].every(field =>
        Array.isArray(row[field]) && row[field].every(valid))) return source;
      const origin = row.eligible[0]?.[0] ?? row.scored[0]?.[0] ?? row.measured[0]?.[0] ?? 0;
      const encoded = {hex: row.hex, origin};
      for (const field of ['eligible', 'scored', 'measured']) {
        if (field === 'scored' && JSON.stringify(row.scored) === JSON.stringify(row.eligible)) {
          encoded.scored = 'eligible'; continue;
        }
        let previous = origin;
        encoded[field] = [];
        for (const [start, end] of row[field]) {
          const gap = start - previous, duration = end - start;
          // Preserve IEEE-754 values exactly, including fractional endpoints.
          if (previous + gap !== start || start + duration !== end) return source;
          encoded[field].push(gap, duration); previous = end;
        }
      }
      aircraft.push(encoded);
    }
    return {...source, version: 2, encoding: 'delta-duration-ms-v1', aircraft};
  }

  function expandBasis(source) {
    if (source?.version !== 2 || source.encoding !== 'delta-duration-ms-v1') return source;
    if (!Array.isArray(source.aircraft)) return null;
    const aircraft = [];
    for (const row of source.aircraft) {
      if (!Number.isFinite(row?.origin)) return null;
      const decoded = {hex: row.hex};
      for (const field of ['eligible', 'scored', 'measured']) {
        if (field === 'scored' && row.scored === 'eligible') {
          decoded.scored = decoded.eligible; continue;
        }
        const values = row[field];
        if (!Array.isArray(values) || values.length % 2 || !values.every(Number.isFinite)) return null;
        let previous = row.origin;
        decoded[field] = [];
        for (let index = 0; index < values.length; index += 2) {
          const start = previous + values[index], end = start + values[index + 1];
          if (!valid([start, end])) return null;
          decoded[field].push([start, end]); previous = end;
        }
      }
      aircraft.push(decoded);
    }
    const {encoding, ...metadata} = source;
    return {...metadata, version: 1, aircraft};
  }

  // Sources carry actual intervals, never additive per-channel percentages or
  // guessed timestamps reconstructed from historical CPI counts.
  function aggregate(sources = [], {from = -Infinity, to = Infinity} = {}) {
    const aircraft = new Map(); let complete = sources.length > 0, excluded = 0;
    for (const input of sources) {
      const source = expandBasis(input);
      if (source?.version !== 1 || !Array.isArray(source.aircraft)) { complete = false; continue; }
      complete &&= source.complete === true;
      excluded += source.excluded_passes || 0;
      for (const row of source.aircraft) {
        if (!row || !/^[0-9a-f]{6}$/i.test(row.hex || '')) { complete = false; continue; }
        const value = aircraft.get(row.hex.toLowerCase()) || {eligible: [], scored: [], measured: []};
        for (const field of Object.keys(value)) {
          if (!Array.isArray(row[field]) || row[field].some(interval => !valid(interval))) complete = false;
          value[field].push(...(Array.isArray(row[field]) ? row[field] : []).filter(valid)
            .map(([start, end]) => [Math.max(from, start), Math.min(to, end)]).filter(valid));
        }
        aircraft.set(row.hex.toLowerCase(), value);
      }
    }
    let eligibleMs = 0, scoredMs = 0, measuredMs = 0; const gaps = [];
    for (const value of aircraft.values()) {
      const eligible = union(value.eligible);
      const scored = intersect(eligible, union(value.scored));
      const measured = intersect(scored, union(value.measured));
      eligibleMs += milliseconds(eligible); scoredMs += milliseconds(scored); measuredMs += milliseconds(measured);
      // Only fully observed internal gaps count. Eligibility breaks, unscored
      // time, and left/right-censored edges cannot become continuity claims.
      for (const interval of scored) {
        const support = intersect(measured, [interval]);
        for (let i = 1; i < support.length; ++i) gaps.push((support[i][0] - support[i - 1][1]) / 1000);
      }
    }
    gaps.sort((a, b) => a - b);
    const fullyScored = complete && eligibleMs === scoredMs;
    const continuityComplete = fullyScored && scoredMs > 0;
    return {schema: 'vectorwarp-measured-track-coverage-v1',
      scope: 'retained-eligible-aircraft-time', aggregation: 'aircraft-time-interval-union',
      eligible_seconds: eligibleMs / 1000, scored_seconds: scoredMs / 1000,
      unscored_seconds: (eligibleMs - scoredMs) / 1000, measured_seconds: measuredMs / 1000,
      untracked_seconds: (scoredMs - measuredMs) / 1000,
      fraction: fullyScored && eligibleMs ? measuredMs / eligibleMs : null,
      scored_fraction: scoredMs ? measuredMs / scoredMs : null,
      complete: fullyScored, excluded_passes: excluded,
      continuity_complete: continuityComplete,
      observed_internal_gap_count: gaps.length,
      gap_count: continuityComplete ? gaps.length : null,
      gap_p50_seconds: continuityComplete ? quantile(gaps, .5) : null,
      gap_p95_seconds: continuityComplete ? quantile(gaps, .95) : null,
      gap_max_seconds: continuityComplete && gaps.length ? gaps[gaps.length - 1] : null,
      gap_semantics: 'internal-unmeasured-intervals-within-contiguous-scored-exposure;nearest-rank-quantiles'};
  }
  // Keep every pass and exact interval while omitting display-unused diagnostics.
  function kioskAssociationChannel(channel) {
    if (channel.measured_track_coverage || channel.cpi_exposure)
      return {...Object.fromEntries(['identity', 'selected_channel', 'updated_at',
        'association_statistics', 'measured_track_coverage', 'cpi_exposure']
        .filter(key => key in channel).map(key => [key, channel[key]]))};
    const exposureFields = ['eligible_cpis', 'association_eligible_cpis',
      'detected_cpis', 'operationally_tracked_cpis', 'eligible_seconds',
      'association_eligible_seconds'];
    const cpiExposure = (channel.passes || []).reduce((sum, pass) => {
      for (const field of exposureFields)
        sum[field] += Number(pass?.cpi_exposure?.[field]) || 0;
      return sum;
    }, Object.fromEntries(exposureFields.map(field => [field, 0])));
    return {...Object.fromEntries(['identity', 'selected_channel', 'updated_at',
      'association_statistics'].filter(key => key in channel).map(key => [key, channel[key]])),
    measured_track_coverage: aggregate([channel.measured_track_basis]),
    cpi_exposure: cpiExposure};
  }
  return {union, extendUnion, intersect, aggregate, compactBasis, expandBasis, kioskAssociationChannel};
});
