'use strict';

const OPERATIONAL_STATES = new Set(['ACTIVE', 'CONFIRMED', 'COASTING']);
const MAX_FINALIZED_TRACKS = 1000;
const MAX_REFERENCE_TRACKS = 1000;
const MAX_SAMPLES_PER_TRACK = 64;
const DEFAULT_GAP_MS = 3000;
const finite = value => typeof value === 'number' && Number.isFinite(value);
const ratio = (numerator, denominator) => denominator ? numerator / denominator : null;

function quantile(values, fraction) {
  const sorted = values.filter(finite).sort((left, right) => left - right);
  if (!sorted.length) return null;
  const position = (sorted.length - 1) * fraction;
  const lower = Math.floor(position), upper = Math.ceil(position);
  if (lower === upper) return sorted[lower];
  return sorted[lower] + (sorted[upper] - sorted[lower]) * (position - lower);
}

function features(track) {
  const duration = Math.max(0, (track.last_at - track.started_at) / 1000);
  const transitions = Math.max(1, track.measured_observations - 1);
  const measured = (track.samples || []).filter(sample => sample.measured !== false &&
    finite(sample.timestamp) && finite(sample.delay_km) && finite(sample.doppler_hz));
  let delaySlopeKmS = null, delayFitRmsKm = null, meanDopplerHz = null,
    netDelayMotionKm = null;
  if (measured.length) {
    meanDopplerHz = measured.reduce((sum, sample) => sum + sample.doppler_hz, 0) /
      measured.length;
    netDelayMotionKm = Math.abs(measured.at(-1).delay_km - measured[0].delay_km);
  }
  if (measured.length >= 3) {
    const origin = measured[0].timestamp;
    const times = measured.map(sample => (sample.timestamp - origin) / 1000);
    const meanTime = times.reduce((sum, value) => sum + value, 0) / times.length;
    const meanDelay = measured.reduce((sum, sample) => sum + sample.delay_km, 0) /
      measured.length;
    const denominator = times.reduce((sum, value) => sum + (value - meanTime) ** 2, 0);
    if (denominator > 0) {
      delaySlopeKmS = times.reduce((sum, value, index) => sum +
        (value - meanTime) * (measured[index].delay_km - meanDelay), 0) / denominator;
      delayFitRmsKm = Math.sqrt(times.reduce((sum, value, index) => sum +
        (measured[index].delay_km - (meanDelay + delaySlopeKmS *
          (value - meanTime))) ** 2, 0) / times.length);
    }
  }
  return {duration_s: duration, observed_cpis: track.observations,
    measured_fraction: ratio(track.measured_observations, track.observations),
    mean_absolute_doppler_hz: ratio(track.sum_absolute_doppler_hz,
      track.measured_observations),
    delay_motion_km: track.delay_motion_km,
    doppler_motion_hz: track.doppler_motion_hz,
    net_delay_motion_km: netDelayMotionKm,
    measured_delay_slope_km_s: delaySlopeKmS,
    measured_delay_fit_rms_km: delayFitRmsKm,
    mean_signed_doppler_hz: meanDopplerHz,
    delay_step_km: track.delay_motion_km / transitions,
    doppler_step_hz: track.doppler_motion_hz / transitions};
}

function distribution(rows, field) {
  const values = rows.map(row => row.features?.[field]).filter(finite);
  return {samples: values.length, p10: quantile(values, .1), median: quantile(values, .5),
    p90: quantile(values, .9), p25: quantile(values, .25), p75: quantile(values, .75)};
}

function featureTransform(field, value) {
  if (!finite(value)) return null;
  return ['duration_s', 'observed_cpis', 'mean_absolute_doppler_hz',
    'delay_motion_km', 'doppler_motion_hz'].includes(field) ?
    Math.log1p(Math.max(0, value)) : value;
}

function similarity(row, reference, recurrence = 1) {
  if ((row.measured_observations || 0) < 3)
    return {score: null, shape_score: null, recurrence_count: recurrence,
      compared_features: 0, band: 'insufficient-evidence'};
  const parts = [];
  // Historical ADS-B association events contain measured tracker points but
  // not every intervening coast. Score persistence one-sided (lasting longer
  // is not a mismatch), omit that ledger-dependent measured fraction, and use
  // motion per measured transition rather than cumulative motion.
  const scoringFields = ['duration_s', 'observed_cpis',
    'mean_absolute_doppler_hz', 'delay_step_km', 'doppler_step_hz'];
  for (const field of scoringFields) {
    const bounds = reference[field];
    if (!bounds) continue;
    const value = featureTransform(field, row.features?.[field]);
    const median = featureTransform(field, bounds.median);
    const lower = featureTransform(field, bounds.p25);
    const upper = featureTransform(field, bounds.p75);
    if (!finite(value) || bounds.samples < 5 || !finite(median) ||
        !finite(lower) || !finite(upper))
      continue;
    const scale = Math.max(Math.abs(upper - lower), Math.abs(median) * .1, 1e-6);
    const robustDistance = ['duration_s', 'observed_cpis'].includes(field) && value >= median ?
      0 : Math.abs(value - median) / scale;
    parts.push(Math.exp(-.5 * robustDistance * robustDistance));
  }
  if (parts.length < 4) return {score: null, shape_score: null,
    recurrence_count: recurrence, compared_features: parts.length,
    band: 'insufficient-reference'};
  // A geometric mean prevents one broad feature from hiding a strong mismatch.
  // Repeated returns in the same range/Doppler cell are negative evidence for
  // aircraft motion, whereas a one-off dynamic track receives no penalty.
  const shapeScore = Math.exp(parts.reduce((sum, value) =>
    sum + Math.log(Math.max(value, 1e-9)), 0) / parts.length);
  const score = shapeScore / Math.sqrt(Math.max(1, recurrence));
  return {score, shape_score: shapeScore, recurrence_count: recurrence,
    compared_features: parts.length,
    band: score >= .6 ? 'high-similarity' : score >= .3 ? 'moderate-similarity' :
      'low-similarity'};
}

function publicTrack(track) {
  return {track_key: track.key, track_id: track.id, started_at: track.started_at,
    last_at: track.last_at, active: track.active === true,
    observations: track.observations, measured_observations: track.measured_observations,
    state_counts: {...track.state_counts}, associated_aircraft: [...track.associated_aircraft],
    associated_with_adsb: track.associated_aircraft.length > 0,
    delay_km: {minimum: track.min_delay_km, maximum: track.max_delay_km},
    doppler_hz: {minimum: track.min_doppler_hz, maximum: track.max_doppler_hz},
    features: features(track)};
}

function recurrenceCell(track) {
  const delay = (track.delay_km.minimum + track.delay_km.maximum) / 2;
  const doppler = (track.doppler_hz.minimum + track.doppler_hz.maximum) / 2;
  return {key: `${Math.floor(delay / .25)}:${Math.floor(doppler / 5)}`,
    delay_bin_km: Math.floor(delay / .25) * .25,
    doppler_bin_hz: Math.floor(doppler / 5) * 5};
}

function recurrenceEligible(track) {
  // Recurrence is only nuisance evidence when the track itself remains close
  // to a fixed cell. A moving aircraft may cross the same coarse cell on many
  // different passes and must not be penalized for that traffic pattern.
  return (track.features?.delay_motion_km || 0) <= .25 &&
    (track.features?.doppler_motion_hz || 0) <= 10;
}

class TrackPopulationStatistics {
  constructor(state = null, {gapMs = DEFAULT_GAP_MS} = {}) {
    this.state = state && typeof state === 'object' ? state : {};
    this.state.schema = 'vectorwarp-track-population-v2';
    this.state.active ||= {};
    this.state.finalized = Array.isArray(this.state.finalized) ? this.state.finalized : [];
    this.state.reference = Array.isArray(this.state.reference) ? this.state.reference : [];
    // v1 mixed historical ADS-B examples into the bounded live population,
    // so nuisance tracks eventually evicted the very reference used to score
    // them. Migrate those examples into an independent reservoir.
    const seeded = this.state.finalized.filter(track => track.seeded_reference === true ||
      String(track.key || '').startsWith('associated:'));
    this.state.finalized = this.state.finalized.filter(track =>
      track.seeded_reference !== true && !String(track.key || '').startsWith('associated:'));
    for (const track of seeded) this.addReference(track);
    this.gapMs = gapMs;
  }

  addReference(track) {
    if (!track?.key || this.state.reference.some(item => item.key === track.key)) return false;
    this.state.reference.push({...track, active: false});
    if (this.state.reference.length > MAX_REFERENCE_TRACKS)
      this.state.reference.splice(0, this.state.reference.length - MAX_REFERENCE_TRACKS);
    return true;
  }

  accept(frame, delayScale = 1) {
    if (!frame || !finite(frame.timestamp) || !Array.isArray(frame.data)) return;
    const seen = new Set();
    for (const item of frame.data) {
      if (!item || !OPERATIONAL_STATES.has(String(item.state).toUpperCase()) ||
          item.id === null || item.id === undefined || !finite(item.delay) ||
          !finite(item.doppler)) continue;
      const id = String(item.id), delay = item.delay * delayScale;
      seen.add(id);
      let track = this.state.active[id];
      if (!track || frame.timestamp - track.last_at > this.gapMs) {
        if (track) this.finalize(id);
        track = this.state.active[id] = {key: `${id}:${frame.timestamp}`, id,
          started_at: frame.timestamp, last_at: frame.timestamp, active: true,
          observations: 0, measured_observations: 0, state_counts: {},
          associated_aircraft: [], samples: [], sum_absolute_doppler_hz: 0,
          delay_motion_km: 0, doppler_motion_hz: 0,
          min_delay_km: delay, max_delay_km: delay,
          min_doppler_hz: item.doppler, max_doppler_hz: item.doppler};
      }
      if (track.samples.at(-1)?.timestamp === frame.timestamp) continue;
      track.last_at = frame.timestamp; track.observations += 1;
      const state = String(item.state).toUpperCase();
      track.state_counts[state] = (track.state_counts[state] || 0) + 1;
      track.min_delay_km = Math.min(track.min_delay_km, delay);
      track.max_delay_km = Math.max(track.max_delay_km, delay);
      track.min_doppler_hz = Math.min(track.min_doppler_hz, item.doppler);
      track.max_doppler_hz = Math.max(track.max_doppler_hz, item.doppler);
      if (item.measured !== false) {
        const prior = [...track.samples].reverse().find(sample => sample.measured);
        if (prior) {
          track.delay_motion_km += Math.abs(delay - prior.delay_km);
          track.doppler_motion_hz += Math.abs(item.doppler - prior.doppler_hz);
        }
        track.measured_observations += 1;
        track.sum_absolute_doppler_hz += Math.abs(item.doppler);
      }
      track.samples.push({timestamp: frame.timestamp, delay_km: delay,
        doppler_hz: item.doppler, measured: item.measured !== false});
      if (track.samples.length > MAX_SAMPLES_PER_TRACK)
        track.samples.splice(0, track.samples.length - MAX_SAMPLES_PER_TRACK);
    }
    for (const [id, track] of Object.entries(this.state.active))
      if (!seen.has(id) && frame.timestamp - track.last_at > this.gapMs) this.finalize(id);
  }

  associate(association, aircraft) {
    const id = association?.tracker?.id;
    if (id === null || id === undefined || !aircraft) return false;
    const track = this.state.active[String(id)];
    if (!track) return false;
    const value = String(aircraft).toLowerCase();
    if (!track.associated_aircraft.includes(value)) track.associated_aircraft.push(value);
    return true;
  }

  seedAssociated(passId, associations, aircraft) {
    if (!passId || !aircraft || !Array.isArray(associations)) return 0;
    const groups = new Map();
    for (const association of associations) {
      const id = association?.tracker?.id;
      if (id === null || id === undefined || !finite(association.frame_timestamp_ms) ||
          !finite(association.measured_delay_km) || !finite(association.measured_doppler_hz))
        continue;
      const rows = groups.get(String(id)) || [];
      rows.push(association); groups.set(String(id), rows);
    }
    let added = 0;
    for (const [id, rows] of groups) {
      rows.sort((left, right) => left.frame_timestamp_ms - right.frame_timestamp_ms);
      if (rows.length < 3) continue;
      const key = `associated:${passId}:${id}`;
      if (this.state.reference.some(track => track.key === key)) continue;
      const first = rows[0], track = {key, id, seeded_reference: true,
        started_at: first.frame_timestamp_ms,
        last_at: rows.at(-1).frame_timestamp_ms, active: false, observations: 0,
        measured_observations: 0, state_counts: {},
        associated_aircraft: [String(aircraft).toLowerCase()], samples: [],
        sum_absolute_doppler_hz: 0, delay_motion_km: 0, doppler_motion_hz: 0,
        min_delay_km: first.measured_delay_km, max_delay_km: first.measured_delay_km,
        min_doppler_hz: first.measured_doppler_hz,
        max_doppler_hz: first.measured_doppler_hz};
      let prior = null;
      for (const row of rows) {
        track.observations += 1; track.measured_observations += 1;
        const state = String(row.tracker?.state || 'ACTIVE').toUpperCase();
        track.state_counts[state] = (track.state_counts[state] || 0) + 1;
        track.sum_absolute_doppler_hz += Math.abs(row.measured_doppler_hz);
        track.min_delay_km = Math.min(track.min_delay_km, row.measured_delay_km);
        track.max_delay_km = Math.max(track.max_delay_km, row.measured_delay_km);
        track.min_doppler_hz = Math.min(track.min_doppler_hz, row.measured_doppler_hz);
        track.max_doppler_hz = Math.max(track.max_doppler_hz, row.measured_doppler_hz);
        if (prior) {
          track.delay_motion_km += Math.abs(row.measured_delay_km - prior.measured_delay_km);
          track.doppler_motion_hz += Math.abs(row.measured_doppler_hz - prior.measured_doppler_hz);
        }
        prior = row;
      }
      if (this.addReference(track)) added += 1;
    }
    return added;
  }

  resetSeededReference() {
    this.state.reference = [];
  }

  seedAssociatedEvents(records) {
    const groups = new Map();
    for (const record of records || []) {
      const association = record?.association, id = association?.tracker?.id;
      if (id === null || id === undefined || !finite(association?.frame_timestamp_ms)) continue;
      const rows = groups.get(String(id)) || [];
      rows.push(record); groups.set(String(id), rows);
    }
    let added = 0;
    for (const [id, rows] of groups) {
      rows.sort((left, right) => left.association.frame_timestamp_ms -
        right.association.frame_timestamp_ms);
      let cohort = [];
      const flush = () => {
        if (cohort.length < 3) { cohort = []; return; }
        const started = cohort[0].association.frame_timestamp_ms;
        const finished = cohort.at(-1).association.frame_timestamp_ms;
        const aircraft = [...new Set(cohort.map(row => row.aircraft).filter(Boolean))];
        const existing = [...this.state.finalized, ...Object.values(this.state.active)]
          .find(track => track.id === id && track.started_at <= finished + this.gapMs &&
            track.last_at >= started - this.gapMs);
        if (existing) {
          existing.associated_aircraft = [...new Set([...(existing.associated_aircraft || []),
            ...aircraft.map(value => String(value).toLowerCase())])];
          cohort = []; return;
        }
        added += this.seedAssociated(`history:${id}:${started}`,
          cohort.map(row => row.association), aircraft[0]);
        const key = `associated:history:${id}:${started}:${id}`;
        const track = this.state.reference.find(item => item.key === key);
        if (track) track.associated_aircraft = aircraft.map(value =>
          String(value).toLowerCase());
        cohort = [];
      };
      for (const row of rows) {
        if (cohort.length && row.association.frame_timestamp_ms -
            cohort.at(-1).association.frame_timestamp_ms > this.gapMs) flush();
        cohort.push(row);
      }
      flush();
    }
    return added;
  }

  finalize(id) {
    const track = this.state.active[id];
    if (!track) return;
    delete this.state.active[id]; track.active = false;
    this.state.finalized.push(track);
    if (track.associated_aircraft?.length) this.addReference({...track,
      key: `live-reference:${track.key}`});
    if (this.state.finalized.length > MAX_FINALIZED_TRACKS)
      this.state.finalized.splice(0, this.state.finalized.length - MAX_FINALIZED_TRACKS);
  }

  snapshot({since = -Infinity, includeCaptureCandidates = false} = {}) {
    const tracks = [...this.state.finalized, ...Object.values(this.state.active)]
      .filter(track => track.last_at >= since && track.observations >= 1).map(publicTrack);
    const associated = tracks.filter(track => track.associated_with_adsb);
    const unassociated = tracks.filter(track => !track.associated_with_adsb);
    const referenceTracks = [...new Map([
      ...this.state.reference.map(publicTrack), ...associated
    ].map(track => [`${track.track_id}:${track.started_at}:${track.last_at}`, track])).values()];
    const fields = ['duration_s', 'observed_cpis', 'measured_fraction',
      'mean_absolute_doppler_hz', 'delay_motion_km', 'doppler_motion_hz',
      'delay_step_km', 'doppler_step_hz'];
    const reference = Object.fromEntries(fields.map(field =>
      [field, distribution(referenceTracks, field)]));
    const comparison = Object.fromEntries(fields.map(field => [field, {
      associated: reference[field], unassociated: distribution(unassociated, field)}]));
    const recurrence = new Map();
    for (const track of unassociated.filter(recurrenceEligible)) {
      const cell = recurrenceCell(track);
      recurrence.set(cell.key, (recurrence.get(cell.key) || 0) + 1);
    }
    const scored = unassociated.map(track => {
      const cell = recurrenceCell(track);
      return {...track, recurrence_cell: cell,
        similarity: similarity(track, reference, recurrenceEligible(track) ?
          recurrence.get(cell.key) || 1 : 1)};
    });
    const bands = {'high-similarity': 0, 'moderate-similarity': 0,
      'low-similarity': 0, 'insufficient-reference': 0, 'insufficient-evidence': 0};
    for (const track of scored) bands[track.similarity.band] += 1;
    const result = {schema: 'vectorwarp-track-population-summary-v2',
      definition: 'Operational radar-track cohorts retained by this channel.',
      retained_tracks: tracks.length, adsb_associated_tracks: associated.length,
      unassociated_tracks: unassociated.length,
      empirical_reference_tracks: referenceTracks.length,
      similarity_bands: bands, feature_comparison: comparison,
      aircraft_like_candidates: scored.filter(track =>
        track.similarity.band === 'high-similarity')
        .sort((left, right) => right.similarity.score - left.similarity.score ||
          right.last_at - left.last_at).slice(0, 25)};
    if (includeCaptureCandidates)
      result.capture_candidates = scored.filter(track => track.active === true);
    return result;
  }
}

module.exports = {OPERATIONAL_STATES, TrackPopulationStatistics, quantile, features, similarity};
