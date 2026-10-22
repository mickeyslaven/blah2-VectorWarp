'use strict';

const finite = value => typeof value === 'number' && Number.isFinite(value);
const normalizeHex = value => typeof value === 'string' && /^[0-9a-f]{6}$/i.test(value.trim()) ?
  value.trim().toLowerCase() : null;

function radians(value) { return value * Math.PI / 180; }

function separationKm(left, right) {
  if (![left?.lat, left?.lon, right?.lat, right?.lon].every(finite)) return Infinity;
  const earthKm = 6371.0088;
  const dlat = radians(right.lat - left.lat);
  const dlon = radians(right.lon - left.lon);
  const a = Math.sin(dlat / 2) ** 2 + Math.cos(radians(left.lat)) *
    Math.cos(radians(right.lat)) * Math.sin(dlon / 2) ** 2;
  return earthKm * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(Math.max(0, 1 - a)));
}

function sourceId(source, index) {
  for (const value of [source?.id, source?.value, source?.endpoint, source?.path])
    if (typeof value === 'string' && value) return value;
  return `source-${index + 1}`;
}

function sourceReliability(source) {
  if (finite(source?.reliability)) return Math.max(0, Math.min(1, source.reliability));
  if (source?.kind === 'file' || source?.mode === 'local') return 1;
  if (source?.authenticated === true) return .9;
  return .7;
}

function positionQuality(aircraft) {
  if (finite(aircraft?.nic)) return Math.max(.5, Math.min(1, .5 + aircraft.nic / 22));
  if (finite(aircraft?.nac_p)) return Math.max(.5, Math.min(1, .5 + aircraft.nac_p / 22));
  return .75;
}

function candidates(snapshots, nowSeconds, maxPositionAgeSeconds) {
  const output = [];
  snapshots.forEach((snapshot, index) => {
    const data = snapshot?.data;
    if (!finite(data?.now) || !Array.isArray(data?.aircraft)) return;
    const receivedAtMs = finite(snapshot.receivedAtMs) ? snapshot.receivedAtMs : nowSeconds * 1000;
    const source = snapshot.source || {};
    const id = sourceId(source, index);
    const reliability = sourceReliability(source);
    for (const aircraft of data.aircraft) {
      const hex = normalizeHex(aircraft?.hex);
      if (!hex || !finite(aircraft.lat) || !finite(aircraft.lon) ||
          Math.abs(aircraft.lat) > 90 || Math.abs(aircraft.lon) > 180 ||
          !finite(aircraft.seen_pos) || aircraft.seen_pos < 0) continue;
      const eventTime = data.now - aircraft.seen_pos;
      const age = nowSeconds - eventTime;
      if (!finite(eventTime) || age < -maxPositionAgeSeconds || age > maxPositionAgeSeconds) continue;
      const quality = sourceReliability(source) * positionQuality(aircraft) *
        Math.exp(-Math.max(0, age) / Math.max(1, maxPositionAgeSeconds));
      output.push({hex, aircraft, source, sourceId: id, receivedAtMs, eventTime, age,
        quality, reliability});
    }
  });
  return output;
}

function cluster(rows, disagreementKm) {
  const groups = [];
  for (const row of rows) {
    let group = groups.find(candidate => candidate.some(item =>
      separationKm(item.aircraft, row.aircraft) <= disagreementKm));
    if (!group) { group = []; groups.push(group); }
    group.push(row);
  }
  return groups.sort((left, right) => {
    const sources = group => new Set(group.map(item => item.sourceId)).size;
    const count = sources(right) - sources(left);
    if (count) return count;
    return Math.max(...right.map(item => item.quality)) -
      Math.max(...left.map(item => item.quality));
  });
}

/**
 * Fuse dump1090/readsb-compatible snapshots without inventing a blended point.
 * Event time remains `snapshot.now - seen_pos`; `seen_pos` is rewritten against
 * the returned local `now`, so consumers retain the true position age.
 */
function fuseAircraftData(snapshots, {nowMs = Date.now(), maxPositionAgeSeconds = 30,
  disagreementKm = 3} = {}) {
  const nowSeconds = nowMs / 1000;
  const rows = candidates(snapshots, nowSeconds, maxPositionAgeSeconds);
  const byAircraft = new Map();
  for (const row of rows) {
    const key = `${row.hex}:${row.sourceId}`;
    const previous = byAircraft.get(key);
    if (!previous || row.eventTime > previous.eventTime) byAircraft.set(key, row);
  }
  const byHex = new Map();
  for (const row of byAircraft.values()) {
    const list = byHex.get(row.hex) || [];
    list.push(row); byHex.set(row.hex, list);
  }
  const aircraft = [];
  for (const [hex, observations] of byHex) {
    const groups = cluster(observations, disagreementKm);
    const accepted = groups[0].sort((left, right) => right.quality - left.quality ||
      right.eventTime - left.eventTime);
    const winner = accepted[0];
    const rejected = groups.slice(1).flat();
    aircraft.push({...winner.aircraft, hex, seen_pos: Math.max(0, nowSeconds - winner.eventTime),
      vectorwarp: {
        source_id: winner.sourceId,
        source_kind: winner.source.kind || winner.source.mode || 'http',
        source_mode: winner.source.mode || null,
        source_endpoint: winner.source.endpoint || null,
        source_path: winner.source.path || null,
        received_at_ms: winner.receivedAtMs,
        snapshot_timestamp_s: winner.eventTime + winner.aircraft.seen_pos,
        position_timestamp_s: winner.eventTime,
        position_age_s: Math.max(0, nowSeconds - winner.eventTime),
        quality_score: winner.quality,
        agreeing_sources: [...new Set(accepted.map(item => item.sourceId))],
        rejected_sources: [...new Set(rejected.map(item => item.sourceId))],
        disagreement: rejected.length > 0
      }});
  }
  aircraft.sort((left, right) => left.hex.localeCompare(right.hex));
  return {now: nowSeconds, aircraft, vectorwarp_fusion: {
    schema: 'vectorwarp-adsb-fusion-v1', sources: snapshots.length,
    observations: rows.length, aircraft: aircraft.length}};
}

module.exports = {separationKm, fuseAircraftData};
