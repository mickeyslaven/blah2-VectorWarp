'use strict';

const C_KM_S = 299792.458;
const finite = value => typeof value === 'number' && Number.isFinite(value);
const numeric = value => value !== null && value !== '' && value !== undefined && finite(Number(value));
const radians = value => value * Math.PI / 180;

const DEFAULTS = Object.freeze({cpiSeconds: .5, m: 4, n: 6, deleteAfter: 3,
  maximumSkewMs: 750, coLocationRadiusM: 100, maximumDetectionsPerSource: 32,
  maximumTracks: 256, maximumPublishedTracks: 64,
  maximumAccelerationHzS: 10, maximumCoastSeconds: 10});

function validLocation(value) {
  return [value?.latitude, value?.longitude, value?.altitude].every(numeric) &&
    Math.abs(Number(value.latitude)) <= 90 && Math.abs(Number(value.longitude)) <= 180;
}

function distanceM(left, right) {
  if (!validLocation(left) || !validLocation(right)) return Infinity;
  const latitude = radians((Number(left.latitude) + Number(right.latitude)) / 2);
  const north = radians(Number(right.latitude) - Number(left.latitude)) * 6371008.8;
  const longitudeDelta = ((Number(right.longitude) - Number(left.longitude) + 540) % 360) - 180;
  const east = radians(longitudeDelta) *
    6371008.8 * Math.cos(latitude);
  return Math.hypot(north, east, Number(right.altitude) - Number(left.altitude));
}

function compatibleGeometry(left, right, radiusM) {
  const leftRx = left?.receiver, rightRx = right?.receiver;
  const leftTx = left?.transmitter, rightTx = right?.transmitter;
  if (![leftRx, rightRx, leftTx, rightTx].every(validLocation)) return false;
  const explicitRx = String(leftRx.site_id ?? left.receiver_site_id ?? '').trim();
  const otherRx = String(rightRx.site_id ?? right.receiver_site_id ?? '').trim();
  const explicitTx = String(leftTx.site_id ?? left.transmitter_site_id ?? '').trim();
  const otherTx = String(rightTx.site_id ?? right.transmitter_site_id ?? '').trim();
  if (explicitRx && otherRx && explicitRx !== otherRx || explicitTx && otherTx && explicitTx !== otherTx)
    return false;
  return (explicitRx && explicitRx === otherRx || distanceM(leftRx, rightRx) <= radiusM) &&
    (explicitTx && explicitTx === otherTx || distanceM(leftTx, rightTx) <= radiusM);
}

function geometryCatalog(sources, radiusM = DEFAULTS.coLocationRadiusM) {
  const groups = [];
  for (const source of sources.filter(item => validLocation(item?.receiver) &&
      validLocation(item?.transmitter))) {
    let group = groups.find(item => compatibleGeometry(item.representative, source, radiusM));
    if (!group) {
      group = {id: `geometry-${groups.length + 1}`, representative: source, sources: []};
      groups.push(group);
    }
    group.sources.push(source);
  }
  const output = new Map();
  for (const group of groups) for (const source of group.sources)
    output.set(source.key, group.id);
  return output;
}

function normalizeFrame(source, frame, options = DEFAULTS) {
  const delay = frame?.delay, doppler = frame?.doppler, snr = frame?.snr;
  const timestamp = Number(frame?.timestamp), carrierHz = Number(source?.frequency_hz);
  const sampleRateHz = Number(source?.sample_rate_hz);
  if (!source?.node || !source?.key || !numeric(timestamp) || timestamp < 0 ||
      !finite(carrierHz) || carrierHz <= 0 || !finite(sampleRateHz) || sampleRateHz <= 0 ||
      ![delay, doppler, snr].every(Array.isArray) || delay.length !== doppler.length ||
      delay.length !== snr.length) return null;
  const rows = [];
  for (let index = 0; index < delay.length; index += 1) {
    if (![delay[index], doppler[index], snr[index]].every(numeric) || Number(delay[index]) <= 0)
      continue;
    rows.push({source: source.key, node: source.node, channel: source.channel,
      detectionIndex: index, timestamp, excessKm: Number(delay[index]),
      dopplerHz: Number(doppler[index]), snrDb: Number(snr[index]),
      carrierHz, sampleRateHz,
      rangeRateKmS: -Number(doppler[index]) * C_KM_S / carrierHz,
      delayResolutionKm: Math.max(C_KM_S / sampleRateHz, .01),
      rangeRateResolutionKmS: C_KM_S / carrierHz /
        Math.max(.05, Number(options.cpiSeconds) || DEFAULTS.cpiSeconds),
      provenance: {node: source.node, channel: source.channel,
        detection_index: index, timestamp, excess_path_km: Number(delay[index]),
        doppler_hz: Number(doppler[index]), snr_db: Number(snr[index]),
        bistatic_range_rate_km_s: -Number(doppler[index]) * C_KM_S / carrierHz,
        event_time: frame.eventTime ? {
          source: frame.eventTime.source ?? null, exact: frame.eventTime.exact === true,
          uncertainty_bounded: frame.eventTime.uncertaintyBounded === true,
          uncertainty_ns: frame.eventTime.uncertaintyNs ?? null,
          epoch: frame.eventTime.epoch ?? null,
          first_sample: frame.eventTime.firstSample ?? null,
          end_sample: frame.eventTime.endSample ?? null} : null}});
  }
  return {source, timestamp, rows: rows.sort((a, b) => b.snrDb - a.snrDb)
    .slice(0, options.maximumDetectionsPerSource)};
}

function median(values) {
  const sorted = [...values].sort((a, b) => a - b);
  if (!sorted.length) return null;
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
}

class CooperativeDetectionTracker {
  constructor(options = {}) {
    this.options = {...DEFAULTS, ...options};
    this.options.m = Math.max(2, Math.floor(Number(this.options.m) || DEFAULTS.m));
    this.options.n = Math.max(this.options.m, Math.floor(Number(this.options.n) || DEFAULTS.n));
    this.options.deleteAfter = Math.max(1, Math.floor(Number(this.options.deleteAfter) || DEFAULTS.deleteAfter));
    this.tracks = new Map(); this.nextId = 1; this.lastSourceTimestamp = new Map();
    this.lastResult = null; this.lastUpdateAt = null;
  }

  update(sources, frames, receivedAt = Date.now()) {
    const catalog = geometryCatalog(sources, this.options.coLocationRadiusM);
    const sourceByKey = new Map(sources.map(item => [item.key, item]));
    const normalized = [];
    for (const entry of frames) {
      const source = sourceByKey.get(entry?.source);
      const frame = normalizeFrame(source, entry?.frame, this.options);
      if (!frame || frame.timestamp <= (this.lastSourceTimestamp.get(source.key) ?? -Infinity)) continue;
      this.lastSourceTimestamp.set(source.key, frame.timestamp); normalized.push(frame);
    }
    if (!normalized.length) return this.snapshot(receivedAt);
    const timestamp = Math.max(...normalized.map(item => item.timestamp));
    const observations = [];
    for (const frame of normalized) {
      const geometry = catalog.get(frame.source.key);
      if (!geometry || timestamp - frame.timestamp > this.options.maximumSkewMs) continue;
      for (const row of frame.rows) {
        let cluster = observations.find(item => {
          if (item.geometry !== geometry) return false;
          const representative = item.representative;
          const delayGate = Math.max(representative.delayResolutionKm, row.delayResolutionKm);
          const rateGate = Math.max(representative.rangeRateResolutionKmS, row.rangeRateResolutionKmS);
          return Math.abs(representative.excessKm - row.excessKm) <= delayGate &&
            Math.abs(representative.rangeRateKmS - row.rangeRateKmS) <= rateGate;
        });
        if (cluster) cluster.contributors.push(row);
        else observations.push({geometry, representative: row, contributors: [row]});
      }
    }
    for (const observation of observations) {
      observation.excessKm = median(observation.contributors.map(item => item.excessKm));
      observation.rangeRateKmS = median(observation.contributors.map(item => item.rangeRateKmS));
      observation.nodes = [...new Set(observation.contributors.map(item => item.node))].sort();
      observation.delayResolutionKm = Math.max(...observation.contributors.map(item => item.delayResolutionKm));
      observation.rangeRateResolutionKmS = Math.max(...observation.contributors.map(item => item.rangeRateResolutionKmS));
    }
    const pairs = [];
    for (const track of this.tracks.values()) for (let index = 0; index < observations.length; index += 1) {
      const observation = observations[index];
      if (track.geometry !== observation.geometry) continue;
      const dt = (timestamp - track.timestamp) / 1000;
      if (!(dt > 0) || dt > this.options.maximumCoastSeconds) continue;
      const predicted = track.excessKm + track.rangeRateKmS * dt;
      const acceleration = C_KM_S / Math.min(...observation.contributors.map(item => item.carrierHz)) *
        this.options.maximumAccelerationHzS;
      const delayGate = Math.max(track.delayResolutionKm, observation.delayResolutionKm) +
        .5 * acceleration * dt * dt;
      const rateGate = Math.max(track.rangeRateResolutionKmS, observation.rangeRateResolutionKmS) +
        acceleration * dt;
      const delayResidual = Math.abs(observation.excessKm - predicted);
      const rateResidual = Math.abs(observation.rangeRateKmS - track.rangeRateKmS);
      if (delayResidual <= delayGate && rateResidual <= rateGate)
        pairs.push({track, index, observation,
          score: delayResidual / delayGate + rateResidual / rateGate});
    }
    pairs.sort((a, b) => a.score - b.score || a.track.id.localeCompare(b.track.id));
    const usedTracks = new Set(), usedObservations = new Set();
    const apply = (track, observation) => {
      track.excessKm = observation.excessKm; track.rangeRateKmS = observation.rangeRateKmS;
      track.delayResolutionKm = observation.delayResolutionKm;
      track.rangeRateResolutionKmS = observation.rangeRateResolutionKmS;
      track.timestamp = timestamp; track.misses = 0; track.hits.push(true);
      track.nodeHistory.push(observation.nodes); track.hits = track.hits.slice(-this.options.n);
      track.nodeHistory = track.nodeHistory.slice(-this.options.n);
      track.contributors = observation.contributors.map(item => item.provenance);
      const nodes = new Set(track.nodeHistory.flat());
      if (track.hits.filter(Boolean).length >= this.options.m && nodes.size >= 2) {
        track.confirmed = true;
        track.qualificationNodes = [...new Set([...(track.qualificationNodes || []), ...nodes])].sort();
      }
    };
    for (const pair of pairs) {
      if (usedTracks.has(pair.track.id) || usedObservations.has(pair.index)) continue;
      usedTracks.add(pair.track.id); usedObservations.add(pair.index); apply(pair.track, pair.observation);
    }
    for (const track of this.tracks.values()) if (!usedTracks.has(track.id)) {
      track.hits.push(false); track.nodeHistory.push([]); track.hits = track.hits.slice(-this.options.n);
      track.nodeHistory = track.nodeHistory.slice(-this.options.n); track.misses += 1; track.contributors = [];
    }
    for (let index = 0; index < observations.length; index += 1) if (!usedObservations.has(index)) {
      if (this.tracks.size >= this.options.maximumTracks) break;
      const observation = observations[index], id = `C${String(this.nextId++).padStart(4, '0')}`;
      const track = {id, geometry: observation.geometry, excessKm: observation.excessKm,
        rangeRateKmS: observation.rangeRateKmS, delayResolutionKm: observation.delayResolutionKm,
        rangeRateResolutionKmS: observation.rangeRateResolutionKmS, timestamp, hits: [],
        nodeHistory: [], misses: 0, confirmed: false, qualificationNodes: [], contributors: []};
      apply(track, observation); this.tracks.set(id, track);
    }
    for (const [id, track] of this.tracks) if (track.misses > this.options.deleteAfter ||
        (timestamp - track.timestamp) / 1000 > this.options.maximumCoastSeconds) this.tracks.delete(id);
    this.lastUpdateAt = receivedAt;
    const allTracks = [...this.tracks.values()].map(track => {
      const nodes = [...new Set(track.nodeHistory.flat())].sort();
      const currentNodes = [...new Set(track.contributors.map(item => item.node))].sort();
      return {id: track.id, state: track.confirmed ?
        (track.misses ? 'COOPERATIVE_COASTING' : 'COOPERATIVE_ACTIVE') : 'COOPERATIVE_CANDIDATE',
      timestamp, measurement_timestamp: track.timestamp, geometry_group: track.geometry,
      excess_path_km: track.excessKm, bistatic_range_rate_km_s: track.rangeRateKmS,
      supporting_cpis: track.hits.filter(Boolean).length, required_supporting_cpis: this.options.m,
      contributing_nodes: currentNodes, recent_supporting_nodes: nodes,
      qualification_nodes: track.qualificationNodes || [], required_nodes_for_qualification: 2,
      misses: track.misses,
      contributors: track.misses ? [] : track.contributors};
    });
    const rank = track => track.state === 'COOPERATIVE_ACTIVE' ? 4 :
      track.state === 'COOPERATIVE_COASTING' ? 3 : track.contributing_nodes.length >= 2 ? 2 : 1;
    allTracks.sort((a, b) => rank(b) - rank(a) ||
      b.supporting_cpis - a.supporting_cpis || b.measurement_timestamp - a.measurement_timestamp ||
      a.id.localeCompare(b.id));
    const tracks = allTracks.slice(0, this.options.maximumPublishedTracks);
    const operational = allTracks.some(track => track.state === 'COOPERATIVE_ACTIVE');
    this.lastResult = {schema: 'vectorwarp-cooperative-detection-tracks-v1', available: true,
      timestamp, definition: 'Same-geometry detections from local VectorWarp nodes tracked in canonical excess-path and bistatic-range-rate coordinates.',
      operational_track_claim: operational, aircraft_claim: false,
      geometry_policy: {receiver_colocation_radius_m: this.options.coLocationRadiusM,
        transmitter_colocation_radius_m: this.options.coLocationRadiusM,
        separate_geometries_merged: false},
      evidence_policy: {one_hit_per_geometry_per_cpi: true, minimum_distinct_nodes: 2,
        source_probabilities_multiplied: false, provenance_preserved: true},
      source_count: sources.length, computation: {internal_tracks: allTracks.length,
        published_tracks: tracks.length, publication_truncated: allTracks.length > tracks.length}, tracks};
    return this.lastResult;
  }

  snapshot(now = Date.now()) {
    if (!this.lastResult) return {schema: 'vectorwarp-cooperative-detection-tracks-v1',
      available: false, reason: 'Waiting for node detection frames.', tracks: []};
    const age = this.lastUpdateAt === null ? Infinity : Math.max(0, (now - this.lastUpdateAt) / 1000);
    if (age > this.options.maximumCoastSeconds) return {...this.lastResult, available: false,
      reason: 'Cooperative detection input is stale.', tracks: []};
    return {...this.lastResult, age_seconds: age};
  }
}

module.exports = {C_KM_S, distanceM, compatibleGeometry, geometryCatalog, normalizeFrame,
  CooperativeDetectionTracker};
