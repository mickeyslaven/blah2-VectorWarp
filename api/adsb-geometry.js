'use strict';

// Adapted from 30hours/adsb2dd (MIT, 2024; commit 1d004a0f0b741214422d55763aa672b0bbb6ae79).
// This in-process version derives delay/Doppler solely from the configured raw
// tar1090 feed; it never accepts a converter URL or request-supplied geometry.
const C = 299792458;
const finite = value => typeof value === 'number' && Number.isFinite(value);
const radians = value => value * Math.PI / 180;
const degrees = value => value * 180 / Math.PI;
const normalize = value => ((value % 360) + 360) % 360;
const median = values => { const sorted = [...values].sort((a, b) => a - b); const middle = Math.floor(sorted.length / 2); return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2; };
function ecef(latitude, longitude, altitude) {
  const rad = Math.PI / 180, a = 6378137, f = 1 / 298.257223563, esq = 2 * f - f * f;
  const lat = latitude * rad, lon = longitude * rad, n = a / Math.sqrt(1 - esq * Math.sin(lat) ** 2);
  return [(n + altitude) * Math.cos(lat) * Math.cos(lon), (n + altitude) * Math.cos(lat) * Math.sin(lon), (n * (1 - esq) + altitude) * Math.sin(lat)];
}
const distance = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
function lookGeometry(site, aircraft) {
  if (![site?.latitude, site?.longitude, site?.altitude, aircraft?.lat,
    aircraft?.lon, aircraft?.alt_geom].every(finite)) return null;
  const lat1 = radians(site.latitude), lat2 = radians(aircraft.lat);
  const dlon = radians(aircraft.lon - site.longitude);
  const y = Math.sin(dlon) * Math.cos(lat2);
  const x = Math.cos(lat1) * Math.sin(lat2) -
    Math.sin(lat1) * Math.cos(lat2) * Math.cos(dlon);
  const bearing = normalize(degrees(Math.atan2(y, x)));
  const earth = 6371008.8;
  const a = Math.sin((lat2 - lat1) / 2) ** 2 +
    Math.cos(lat1) * Math.cos(lat2) * Math.sin(dlon / 2) ** 2;
  const ground = earth * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(Math.max(0, 1 - a)));
  const height = aircraft.alt_geom * .3048 - site.altitude;
  const elevation = degrees(Math.atan2(height, Math.max(ground, 1)));
  const slant = Math.hypot(ground, height);
  return [bearing, elevation, ground, slant].every(finite) ?
    {bearing_deg_true: bearing, elevation_deg: elevation,
      ground_range_km: ground / 1000, slant_range_km: slant / 1000} : null;
}

function optionalNumber(value) { return finite(value) ? value : null; }

class DelayDopplerHistory {
  constructor(config, {truthKey = 'adsb'} = {}) {
    this.truthKey = truthKey;
    this.history = new Map(); this.output = new Map(); this.configure(config);
  }
  configure(config) {
    const adsb = config.truth?.[this.truthKey] || {}, rx = config.location?.rx || {}, tx = config.location?.tx || {};
    this.window = Number.isInteger(adsb.smoothing_window) ? adsb.smoothing_window : 10;
    this.maxAge = finite(adsb.max_position_age) ? adsb.max_position_age : 30;
    this.rx = ecef(rx.latitude, rx.longitude, rx.altitude);
    this.tx = ecef(tx.latitude, tx.longitude, tx.altitude);
    this.rxSite = {...rx}; this.txSite = {...tx};
    this.baseline = distance(this.rx, this.tx); this.frequency = config.capture.fc;
    this.valid = this.rx.every(finite) && this.tx.every(finite) && finite(this.baseline) &&
      finite(this.frequency) && this.frequency > 0;
    this.history.clear(); this.output.clear(); // Restart/config change cannot mix geometries or frequencies.
  }
  clear() { this.history.clear(); this.output.clear(); }
  update(feed) {
    const now = feed?.now, output = {}, warming = [];
    if (!this.valid || !finite(now) || !Array.isArray(feed?.aircraft)) return {data: output, warming};
    for (const [hex, result] of this.output)
      if (now - result.timestamp > this.maxAge) this.output.delete(hex);
    for (const aircraft of feed.aircraft) {
      const altitudeFeet = finite(aircraft?.alt_geom) ? aircraft.alt_geom :
        finite(aircraft?.alt_baro) ? aircraft.alt_baro : null;
      if (typeof aircraft?.hex !== 'string' || !finite(aircraft.lat) || !finite(aircraft.lon) ||
          Math.abs(aircraft.lat) > 90 || Math.abs(aircraft.lon) > 180 || !finite(altitudeFeet) ||
          !finite(aircraft.seen_pos) || aircraft.seen_pos < 0 || aircraft.seen_pos > this.maxAge) continue;
      const timestamp = now - aircraft.seen_pos;
      if (!finite(timestamp) || now - timestamp > this.maxAge) continue;
      const target = ecef(aircraft.lat, aircraft.lon, altitudeFeet * .3048);
      const delay = distance(this.rx, target) + distance(this.tx, target) - this.baseline;
      if (!target.every(finite) || !finite(delay)) continue;
      const previous = this.history.get(aircraft.hex) || [];
      const last = previous.at(-1);
      if (last && (timestamp <= last.timestamp ||
          (last.lat === aircraft.lat && last.lon === aircraft.lon && last.alt === aircraft.alt_geom))) continue;
      // PR #5: timestamps are feed.now - seen_pos and duplicate coordinates are
      // compared against processing history, never the sparse output object.
      const samples = [...previous, {timestamp, delay, lat: aircraft.lat, lon: aircraft.lon, alt: altitudeFeet}]
        .filter(item => timestamp - item.timestamp <= this.maxAge).slice(-Math.max(2, this.window + 1));
      this.history.set(aircraft.hex, samples);
      if (samples.length < 2) continue;
      const slopes = [];
      for (let i = 1; i < samples.length; i++) {
        const dt = samples[i].timestamp - samples[i - 1].timestamp;
        if (dt > 0) slopes.push((samples[i].delay - samples[i - 1].delay) / dt);
      }
      if (!slopes.length) continue;
      const look = lookGeometry(this.rxSite, aircraft);
      const transmitterRange = distance(this.tx, target) / 1000;
      const result = {timestamp,
        hex: typeof aircraft.hex === 'string' ? aircraft.hex.trim().toLowerCase() : '',
        flight: typeof aircraft.flight === 'string' ? aircraft.flight.trim() : '',
        r: typeof aircraft.r === 'string' ? aircraft.r.trim() : '',
        t: typeof aircraft.t === 'string' ? aircraft.t.trim() : '',
        type: typeof aircraft.type === 'string' ? aircraft.type : '',
        position_source: typeof aircraft.position_source === 'string' ? aircraft.position_source :
          typeof aircraft.vectorwarp?.position_source === 'string' ? aircraft.vectorwarp.position_source : 'OTHER',
        source_fields: Array.isArray(aircraft.source_fields) ? aircraft.source_fields : [],
        desc: typeof aircraft.desc === 'string' ? aircraft.desc : '',
        ownOp: typeof aircraft.ownOp === 'string' ? aircraft.ownOp : '',
        year: typeof aircraft.year === 'string' || Number.isFinite(aircraft.year) ? aircraft.year : '',
        delay: delay / 1000, doppler: -median(slopes) / (C / this.frequency),
        frequency_hz: this.frequency,
        delay_definition: 'excess_bistatic_path_km',
        position: {latitude: aircraft.lat, longitude: aircraft.lon,
          altitude_geom_ft: aircraft.alt_geom, altitude_baro_ft: optionalNumber(aircraft.alt_baro),
          altitude_source: finite(aircraft.alt_geom) ? 'GEOMETRIC' : 'BAROMETRIC_FALLBACK',
          ground_speed_kt: optionalNumber(aircraft.gs), track_deg_true: optionalNumber(aircraft.track),
          geom_rate_fpm: optionalNumber(aircraft.geom_rate), baro_rate_fpm: optionalNumber(aircraft.baro_rate),
          category: typeof aircraft.category === 'string' ? aircraft.category : null,
          squawk: typeof aircraft.squawk === 'string' ? aircraft.squawk : null,
          rssi_dbfs: optionalNumber(aircraft.rssi), seen_s: optionalNumber(aircraft.seen),
          seen_pos_s: aircraft.seen_pos},
        provenance: aircraft.vectorwarp && typeof aircraft.vectorwarp === 'object' ?
          {...aircraft.vectorwarp} : null,
        geometry: look ? {...look, transmitter_range_km: transmitterRange} : null};
      const priorResult = this.output.get(aircraft.hex);
      const rateDt = priorResult && timestamp - priorResult.timestamp;
      result.delay_rate_km_s = rateDt > 0 ? (result.delay - priorResult.delay) / rateDt : null;
      result.doppler_rate_hz_s = rateDt > 0 ? (result.doppler - priorResult.doppler) / rateDt : null;
      if (!finite(result.delay) || !finite(result.doppler)) continue;
      this.output.set(aircraft.hex, result); output[aircraft.hex] = result;
    }
    for (const [hex, samples] of this.history) if (!samples.length || now - samples.at(-1).timestamp > this.maxAge) this.history.delete(hex);
    for (const [hex, samples] of this.history)
      if (samples.length < 2 && now - samples.at(-1).timestamp <= this.maxAge) warming.push(hex);
    for (const [hex, result] of this.output) output[hex] = result;
    return {data: output, warming};
  }
}
module.exports = {ecef, distance, lookGeometry, DelayDopplerHistory};
