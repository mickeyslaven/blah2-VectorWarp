'use strict';

const finite = value => typeof value === 'number' && Number.isFinite(value);
const normalizeHex = value => typeof value === 'string' && /^[0-9a-f]{6}$/i.test(value.trim()) ?
  value.trim().toLowerCase() : null;
const AIRCRAFT_METADATA_FIELDS = ['flight', 'callsign', 'r', 't', 'aircraft_type',
  'desc', 'ownOp', 'year', 'category'];
const RICH_TEXT_METADATA_FIELDS = new Set(['desc', 'ownOp']);

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

function projectAircraft(aircraft, seconds) {
  if (!finite(seconds) || Math.abs(seconds) < 1e-6) return {aircraft, projected: false};
  const speed = finite(aircraft?.gs) ? aircraft.gs * .514444 : null;
  const track = finite(aircraft?.track) ? radians(aircraft.track) : null;
  if (!finite(speed) || (!finite(track) && speed > .5))
    return {aircraft, projected: false};
  const heading = finite(track) ? track : 0;
  const north = speed * Math.cos(heading) * seconds;
  const east = speed * Math.sin(heading) * seconds;
  const latitude = aircraft.lat + north / 111320;
  const longitudeScale = 111320 * Math.max(.01, Math.cos(radians(aircraft.lat)));
  const projected = {...aircraft, lat: latitude, lon: aircraft.lon + east / longitudeScale};
  const verticalRate = finite(aircraft.geom_rate) ? aircraft.geom_rate : aircraft.baro_rate;
  if (finite(verticalRate)) {
    if (finite(projected.alt_geom)) projected.alt_geom += verticalRate * seconds / 60;
    if (finite(projected.alt_baro) && typeof projected.alt_baro === 'number')
      projected.alt_baro += verticalRate * seconds / 60;
  }
  return {aircraft: projected, projected: true};
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

function positionSource(aircraft) {
  const type = String(aircraft?.type || '').toLowerCase();
  const fields = Array.isArray(aircraft?.mlat) ? aircraft.mlat.map(value => String(value).toLowerCase()) : [];
  if (type === 'mlat' || fields.includes('lat') || fields.includes('lon')) return 'MLAT';
  if (type.startsWith('adsb')) return 'ADSB_DIRECT';
  if (type.startsWith('adsr')) return 'ADSR';
  if (type.startsWith('tisb')) return 'TISB';
  return 'OTHER';
}

function providerName(source) {
  const value = source?.endpoint || source?.id || source?.path || '';
  try { return new URL(value.replace(/^exchange:/, '')).hostname || value; }
  catch (_) { return String(value); }
}

function positionQuality(aircraft) {
  if (finite(aircraft?.nic)) return Math.max(.5, Math.min(1, .5 + aircraft.nic / 22));
  if (finite(aircraft?.nac_p)) return Math.max(.5, Math.min(1, .5 + aircraft.nac_p / 22));
  return .75;
}

function metadataValue(value) {
  if (typeof value === 'string') {
    const trimmed = value.trim();
    return trimmed ? trimmed : null;
  }
  return typeof value === 'number' && Number.isFinite(value) ? value : null;
}

function metadataObservations(snapshots) {
  const byHex = new Map();
  snapshots.forEach((snapshot, index) => {
    const source = snapshot?.source || {};
    for (const aircraft of Array.isArray(snapshot?.data?.aircraft) ? snapshot.data.aircraft : []) {
      const hex = normalizeHex(aircraft?.hex);
      if (!hex) continue;
      const rows = byHex.get(hex) || [];
      rows.push({aircraft, sourceId: sourceId(source, index)});
      byHex.set(hex, rows);
    }
  });
  return byHex;
}

function pruneMetadataCache(cache, nowSeconds, retentionSeconds) {
  if (!(cache instanceof Map)) return;
  const retention = finite(retentionSeconds) ? Math.max(60, retentionSeconds) : 86400;
  for (const [hex, entry] of cache)
    if (!finite(entry?.updatedAt) || nowSeconds - entry.updatedAt > retention) cache.delete(hex);
  if (cache.size <= 4096) return;
  const oldest = [...cache.entries()].sort((left, right) =>
    (left[1]?.updatedAt || 0) - (right[1]?.updatedAt || 0));
  for (const [hex] of oldest.slice(0, cache.size - 4096)) cache.delete(hex);
}

function retainedMetadata(hex, winner, observations, cache, nowSeconds) {
  const previous = cache instanceof Map ? cache.get(hex) : null;
  const current = [{aircraft: winner.aircraft, sourceId: winner.sourceId}, ...observations];
  const values = {};
  const sources = new Set();
  for (const field of AIRCRAFT_METADATA_FIELDS) {
    const candidates = current.map(row => ({value: metadataValue(row.aircraft?.[field]),
      sourceId: row.sourceId})).filter(row => row.value !== null);
    const prior = metadataValue(previous?.values?.[field]);
    let selected = null;
    if (RICH_TEXT_METADATA_FIELDS.has(field)) {
      const available = [...candidates, ...(prior === null ? [] : [{value: prior, sourceId: null}])];
      selected = available.sort((left, right) =>
        String(right.value).length - String(left.value).length)[0] || null;
    } else selected = candidates[0] || (prior === null ? null : {value: prior, sourceId: null});
    if (!selected) continue;
    values[field] = selected.value;
    if (selected.sourceId) sources.add(selected.sourceId);
    else for (const source of previous?.sources || []) sources.add(source);
  }
  const result = {values, sources: [...sources]};
  if (cache instanceof Map) cache.set(hex, {...result, updatedAt: nowSeconds});
  return result;
}

function candidates(snapshots, nowSeconds, maxPositionAgeSeconds, maxObservationOffsetSeconds) {
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
      if (!finite(eventTime) || Math.abs(age) > maxObservationOffsetSeconds ||
          age < -maxPositionAgeSeconds || age > maxPositionAgeSeconds) continue;
      const projection = projectAircraft(aircraft, age);
      if (!projection.projected && Math.abs(age) >= 1e-6) continue;
      const quality = sourceReliability(source) * positionQuality(aircraft) *
        Math.exp(-Math.max(0, age) / Math.max(1, maxPositionAgeSeconds));
      output.push({hex, aircraft, projectedAircraft: projection.aircraft, source,
        sourceId: id, receivedAtMs, eventTime, age, quality, reliability});
    }
  });
  return output;
}

function cluster(rows, disagreementKm) {
  const groups = [];
  for (const row of rows) {
    let group = groups.find(candidate => candidate.some(item =>
      separationKm(item.projectedAircraft, row.projectedAircraft) <= disagreementKm));
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
  maxObservationOffsetSeconds = 1, disagreementKm = .5, metadataCache = null,
  metadataRetentionSeconds = 86400} = {}) {
  const nowSeconds = nowMs / 1000;
  pruneMetadataCache(metadataCache, nowSeconds, metadataRetentionSeconds);
  const metadataByHex = metadataObservations(snapshots);
  const rows = candidates(snapshots, nowSeconds, maxPositionAgeSeconds,
    maxObservationOffsetSeconds);
  const contributions = new Map();
  snapshots.forEach((snapshot, index) => {
    const id = sourceId(snapshot?.source || {}, index);
    contributions.set(id, {source_id: id, provider: providerName(snapshot?.source || {}),
      candidate_observations: 0, position_wins: 0, agreeing_aircraft: 0,
      disagreement_aircraft: 0, observation_offsets_s: []});
  });
  for (const row of rows) {
    const entry = contributions.get(row.sourceId);
    if (!entry) continue;
    entry.candidate_observations += 1;
    entry.observation_offsets_s.push(row.age);
  }
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
    const winner = [...observations].sort((left, right) =>
      right.eventTime - left.eventTime || right.quality - left.quality)[0];
    const accepted = (groups.find(group => group.includes(winner)) || [winner])
      .sort((left, right) => right.eventTime - left.eventTime || right.quality - left.quality);
    const rejected = observations.filter(item => !accepted.includes(item));
    contributions.get(winner.sourceId).position_wins += 1;
    for (const id of new Set(accepted.map(item => item.sourceId)))
      contributions.get(id).agreeing_aircraft += 1;
    for (const id of new Set(rejected.map(item => item.sourceId)))
      contributions.get(id).disagreement_aircraft += 1;
    const observationOffset = nowSeconds - winner.eventTime;
    const observationAge = Math.max(0, observationOffset);
    const effectiveEventTime = nowSeconds;
    const pairwise = [];
    for (let i = 0; i < accepted.length; i++) for (let j = i + 1; j < accepted.length; j++)
      pairwise.push(separationKm(accepted[i].projectedAircraft, accepted[j].projectedAircraft));
    const sourceFields = Array.isArray(winner.aircraft?.mlat) ?
      winner.aircraft.mlat.map(value => String(value)) : [];
    const sourceClass = positionSource(winner.aircraft);
    const metadata = retainedMetadata(hex, winner, metadataByHex.get(hex) || [],
      metadataCache, nowSeconds);
    aircraft.push({...winner.projectedAircraft, ...metadata.values, hex, seen_pos: 0,
      position_source: sourceClass, source_fields: sourceFields,
      vectorwarp: {
        position_source: sourceClass,
        source_fields: sourceFields,
        provider: providerName(winner.source),
        source_id: winner.sourceId,
        source_kind: winner.source.kind || winner.source.mode || 'http',
        source_mode: winner.source.mode || null,
        source_endpoint: winner.source.endpoint || null,
        source_path: winner.source.path || null,
        metadata_sources: metadata.sources,
        received_at_ms: winner.receivedAtMs,
        snapshot_timestamp_s: winner.eventTime + winner.aircraft.seen_pos,
        observation_position_timestamp_s: winner.eventTime,
        observation_position_age_s: observationAge,
        observation_time_offset_s: observationOffset,
        position_timestamp_s: effectiveEventTime,
        position_age_s: Math.max(0, nowSeconds - effectiveEventTime),
        projected_seconds: observationOffset,
        projection_model: Math.abs(observationOffset) < 1e-6 ? 'identity' :
          'constant_velocity_ground_track',
        quality_score: winner.quality,
        agreeing_sources: [...new Set(accepted.map(item => item.sourceId))],
        rejected_sources: [...new Set(rejected.map(item => item.sourceId))],
        maximum_pairwise_projected_disagreement_km: pairwise.length ? Math.max(...pairwise) : null,
        source_observations: observations.map(item => ({source_id: item.sourceId,
          event_time_s: item.eventTime, observation_time_offset_s: item.age,
          projected_latitude: item.projectedAircraft.lat,
          projected_longitude: item.projectedAircraft.lon,
          accepted: accepted.includes(item)})),
        disagreement: rejected.length > 0
      }});
  }
  aircraft.sort((left, right) => left.hex.localeCompare(right.hex));
  const positionSources = {};
  for (const item of aircraft)
    positionSources[item.position_source] = (positionSources[item.position_source] || 0) + 1;
  const sourceContributions = [...contributions.values()].map(entry => {
    const offsets = [...entry.observation_offsets_s].sort((left, right) => left - right);
    const median = offsets.length ? offsets[Math.floor(offsets.length / 2)] : null;
    const {observation_offsets_s, ...summary} = entry;
    return {...summary, median_observation_offset_s: median};
  });
  return {now: nowSeconds, aircraft, vectorwarp_fusion: {
    schema: 'vectorwarp-adsb-fusion-v1', sources: snapshots.length,
    observations: rows.length, aircraft: aircraft.length, position_sources: positionSources,
    selection_policy: 'newest_position_event_then_quality',
    source_contributions: sourceContributions}};
}

module.exports = {separationKm, projectAircraft, positionSource, providerName, fuseAircraftData};
