'use strict';

// Adapted from 30hours/adsb2dd (MIT, 2024; commit 1d004a0f0b741214422d55763aa672b0bbb6ae79).
// This in-process version derives delay/Doppler solely from the configured raw
// tar1090 feed; it never accepts a converter URL or request-supplied geometry.
const C = 299792458;
const finite = value => typeof value === 'number' && Number.isFinite(value);
const median = values => { const sorted = [...values].sort((a, b) => a - b); const middle = Math.floor(sorted.length / 2); return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2; };
function ecef(latitude, longitude, altitude) {
  const rad = Math.PI / 180, a = 6378137, f = 1 / 298.257223563, esq = 2 * f - f * f;
  const lat = latitude * rad, lon = longitude * rad, n = a / Math.sqrt(1 - esq * Math.sin(lat) ** 2);
  return [(n + altitude) * Math.cos(lat) * Math.cos(lon), (n + altitude) * Math.cos(lat) * Math.sin(lon), (n * (1 - esq) + altitude) * Math.sin(lat)];
}
const distance = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);

class DelayDopplerHistory {
  constructor(config) { this.history = new Map(); this.output = new Map(); this.configure(config); }
  configure(config) {
    const adsb = config.truth?.adsb || {}, rx = config.location?.rx || {}, tx = config.location?.tx || {};
    this.window = Number.isInteger(adsb.smoothing_window) ? adsb.smoothing_window : 10;
    this.maxAge = finite(adsb.max_position_age) ? adsb.max_position_age : 30;
    this.rx = ecef(rx.latitude, rx.longitude, rx.altitude);
    this.tx = ecef(tx.latitude, tx.longitude, tx.altitude);
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
      if (typeof aircraft?.hex !== 'string' || !finite(aircraft.lat) || !finite(aircraft.lon) ||
          Math.abs(aircraft.lat) > 90 || Math.abs(aircraft.lon) > 180 || !finite(aircraft.alt_geom) ||
          !finite(aircraft.seen_pos) || aircraft.seen_pos < 0 || aircraft.seen_pos > this.maxAge) continue;
      const timestamp = now - aircraft.seen_pos;
      if (!finite(timestamp) || now - timestamp > this.maxAge) continue;
      const target = ecef(aircraft.lat, aircraft.lon, aircraft.alt_geom * .3048);
      const delay = distance(this.rx, target) + distance(this.tx, target) - this.baseline;
      if (!target.every(finite) || !finite(delay)) continue;
      const previous = this.history.get(aircraft.hex) || [];
      const last = previous.at(-1);
      if (last && (timestamp <= last.timestamp ||
          (last.lat === aircraft.lat && last.lon === aircraft.lon && last.alt === aircraft.alt_geom))) continue;
      // PR #5: timestamps are feed.now - seen_pos and duplicate coordinates are
      // compared against processing history, never the sparse output object.
      const samples = [...previous, {timestamp, delay, lat: aircraft.lat, lon: aircraft.lon, alt: aircraft.alt_geom}]
        .filter(item => timestamp - item.timestamp <= this.maxAge).slice(-Math.max(2, this.window + 1));
      this.history.set(aircraft.hex, samples);
      if (samples.length < 2) continue;
      const slopes = [];
      for (let i = 1; i < samples.length; i++) {
        const dt = samples[i].timestamp - samples[i - 1].timestamp;
        if (dt > 0) slopes.push((samples[i].delay - samples[i - 1].delay) / dt);
      }
      if (!slopes.length) continue;
      const result = {timestamp, flight: typeof aircraft.flight === 'string' ? aircraft.flight.trim() : '',
        delay: delay / 1000, doppler: -median(slopes) / (C / this.frequency)};
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
module.exports = {ecef, distance, DelayDopplerHistory};
