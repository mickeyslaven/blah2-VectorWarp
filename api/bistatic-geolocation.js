'use strict';

const C = 299792458;
const R = 6371008.8;
const finite = value => typeof value === 'number' && Number.isFinite(value);
const radians = degrees => degrees * Math.PI / 180;
const degrees = value => value * 180 / Math.PI;
const normalize = value => ((value % 360) + 360) % 360;

function inverse(a, b) {
  const lat1 = radians(a.latitude), lat2 = radians(b.latitude);
  const dLat = lat2 - lat1, dLon = radians(b.longitude - a.longitude);
  const h = Math.sin(dLat / 2) ** 2 + Math.cos(lat1) * Math.cos(lat2) * Math.sin(dLon / 2) ** 2;
  const distanceM = 2 * R * Math.asin(Math.min(1, Math.sqrt(h)));
  const y = Math.sin(dLon) * Math.cos(lat2);
  const x = Math.cos(lat1) * Math.sin(lat2) - Math.sin(lat1) * Math.cos(lat2) * Math.cos(dLon);
  return {distanceM, bearingDeg: normalize(degrees(Math.atan2(y, x)))};
}

function destination(origin, bearingDeg, distanceM) {
  const angular = distanceM / R, bearing = radians(bearingDeg), lat1 = radians(origin.latitude);
  const lon1 = radians(origin.longitude);
  const lat2 = Math.asin(Math.sin(lat1) * Math.cos(angular) +
    Math.cos(lat1) * Math.sin(angular) * Math.cos(bearing));
  const lon2 = lon1 + Math.atan2(Math.sin(bearing) * Math.sin(angular) * Math.cos(lat1),
    Math.cos(angular) - Math.sin(lat1) * Math.sin(lat2));
  return {latitude: degrees(lat2), longitude: ((degrees(lon2) + 540) % 360) - 180};
}

function locate(rx, tx, excessPathM, bearingDeg) {
  if (![rx?.latitude, rx?.longitude, tx?.latitude, tx?.longitude, excessPathM, bearingDeg].every(finite) || excessPathM <= 0) return null;
  const baseline = inverse(rx, tx);
  if (!(baseline.distanceM > 0)) return null;
  const theta = radians(normalize(bearingDeg - baseline.bearingDeg));
  const numerator = excessPathM * (2 * baseline.distanceM + excessPathM);
  const denominator = 2 * (baseline.distanceM + excessPathM - baseline.distanceM * Math.cos(theta));
  if (!(denominator > 0)) return null;
  const rangeM = numerator / denominator;
  return finite(rangeM) && rangeM > 0 ? {...destination(rx, bearingDeg, rangeM), rangeM, excessPathM} : null;
}

function separationM(a, b) { return inverse(a, b).distanceM; }

class BistaticHypothesisTracker {
  constructor({history = 8, maxSpeedMps = 450, maxAccelerationMps2 = 80} = {}) {
    this.history = history; this.maxSpeedMps = maxSpeedMps; this.maxAccelerationMps2 = maxAccelerationMps2;
    this.tracks = new Map();
  }
  select(id, timestamp, candidates) {
    if (!id || !finite(timestamp) || !candidates.length) return null;
    const old = this.tracks.get(id) || [];
    if (old.length && timestamp <= old[old.length - 1].timestamp) {
      const same = old.find(frame => frame.timestamp === timestamp); return same?.selected || null;
    }
    const frames = [...old, {timestamp, candidates}].slice(-this.history);
    const first = frames[0]; let states = first.candidates.map((candidate, index) => ({
      previous: -1, current: index, score: Math.log(Math.max(candidate.support || 1e-6, 1e-6)), path: [index]}));
    for (let frame = 1; frame < frames.length; frame += 1) {
      const dt = (frames[frame].timestamp - frames[frame - 1].timestamp) / 1000;
      if (!(dt > 0) || dt > 30) { states = frames[frame].candidates.map((candidate, index) => ({previous: -1, current: index, score: Math.log(Math.max(candidate.support || 1e-6, 1e-6)), path: [index]})); continue; }
      const next = [];
      for (const state of states) for (let current = 0; current < frames[frame].candidates.length; current += 1) {
        const from = frames[frame - 1].candidates[state.current], to = frames[frame].candidates[current];
        const speed = separationM(from, to) / dt; if (!finite(speed) || speed > this.maxSpeedMps) continue;
        let penalty = .5 * (speed / this.maxSpeedMps) ** 2;
        if (frame > 1 && state.previous >= 0) {
          const before = frames[frame - 2].candidates[state.previous];
          const priorDt = (frames[frame - 1].timestamp - frames[frame - 2].timestamp) / 1000;
          if (priorDt > 0) { const priorSpeed = separationM(before, from) / priorDt;
            const acceleration = Math.abs(speed - priorSpeed) / Math.max(dt, priorDt);
            if (acceleration > this.maxAccelerationMps2) continue;
            penalty += .5 * (acceleration / this.maxAccelerationMps2) ** 2; }
        }
        const candidate = {previous: state.current, current,
          score: state.score + Math.log(Math.max(to.support || 1e-6, 1e-6)) - penalty,
          path: [...state.path, current]};
        const equivalent = next.findIndex(value => value.previous === candidate.previous && value.current === current);
        if (equivalent < 0) next.push(candidate); else if (candidate.score > next[equivalent].score) next[equivalent] = candidate;
      }
      if (!next.length) next.push(...frames[frame].candidates.map((candidate, index) => ({previous: -1, current: index,
        score: Math.log(Math.max(candidate.support || 1e-6, 1e-6)), path: [index]})));
      states = next;
    }
    const best = states.reduce((left, right) => right.score > left.score ? right : left);
    const selected = {...frames[frames.length - 1].candidates[best.current],
      geometry_consistent_samples: best.path.length, selection: 'bistatic_multi_scan'};
    frames[frames.length - 1].selected = selected; this.tracks.set(id, frames);
    for (const [trackId, history] of this.tracks) if (timestamp - history[history.length - 1].timestamp > 60000) this.tracks.delete(trackId);
    return selected;
  }
}

function locationsFromBearing(frame, config, hypothesisTracker = null) {
  const enabled = config?.process?.bearing?.enable === true;
  const fs = config?.capture?.fs, rx = config?.location?.rx, tx = config?.location?.tx;
  if (!enabled) return {enabled: false, reason: 'Bearing processing is disabled.', timestamp: frame?.timestamp ?? null, data: []};
  if (!finite(fs) || fs <= 0 || !rx || !tx)
    return {enabled: true, available: false, reason: 'Receiver, transmitter, or sample-rate geometry is incomplete.', timestamp: frame?.timestamp ?? null, data: []};
  const data = (Array.isArray(frame?.data) ? frame.data : []).map(track => {
    const excessPathM = Number(track.delay) * C / fs;
    const candidates = (Array.isArray(track.candidates) ? track.candidates : []).map(candidate => {
      const point = locate(rx, tx, excessPathM, candidate.bearing_deg_true);
      return point ? {...point, bearing_deg_true: candidate.bearing_deg_true, support: candidate.support} : null;
    }).filter(Boolean);
    // Keep `location` as the strict, downstream-safe fix contract, while also
    // exposing the best estimate for diagnostic displays even when confidence
    // is insufficient to resolve the bearing.
    const selected = hypothesisTracker?.select(track.id, Number(frame?.timestamp), candidates);
    const currentEstimate = locate(rx, tx, excessPathM, track.bearing_deg_true);
    const estimate = selected || currentEstimate;
    const point = track.resolved ? currentEstimate : null;
    const uncertainty = finite(track.uncertainty_deg) ? Math.min(180, Math.max(0, track.uncertainty_deg)) : 180;
    const corridor = point ? [locate(rx, tx, excessPathM, normalize(track.bearing_deg_true - uncertainty)),
      point, locate(rx, tx, excessPathM, normalize(track.bearing_deg_true + uncertainty))].filter(Boolean) : [];
    return {...track, excess_path_km: excessPathM / 1000, estimate, location: point,
      geometry_consistent_samples: selected?.geometry_consistent_samples || 0,
      uncertainty_corridor: corridor, candidates};
  });
  return {enabled: true, available: true, timestamp: frame?.timestamp ?? null,
    method: frame?.method || 'coherent_beam_bank', data};
}

module.exports = {inverse, destination, locate, BistaticHypothesisTracker, locationsFromBearing};
