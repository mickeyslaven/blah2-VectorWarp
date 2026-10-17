'use strict';

const net = require('net');

const finite = value => typeof value === 'number' && Number.isFinite(value);
const normalizedHex = value => typeof value === 'string' ? value.trim().toLowerCase() : '';

class TrackerForwarder {
  constructor(config, adsbSource, {connect = options => net.createConnection(options),
    schedule = setTimeout, cancel = clearTimeout, now = Date.now,
    settings = config.network?.tracker_forward || {}, annotateAdsb = true} = {}) {
    this.enabled = settings.enabled === true;
    this.host = settings.host || '127.0.0.1';
    this.port = settings.port || 30100;
    this.delayTolerance = finite(config.truth?.adsb?.delay_tolerance) ?
      config.truth.adsb.delay_tolerance : 2;
    this.dopplerTolerance = finite(config.truth?.adsb?.doppler_tolerance) ?
      config.truth.adsb.doppler_tolerance : 5;
    this.adsbSource = adsbSource;
    this.connect = connect;
    this.schedule = schedule;
    this.cancel = cancel;
    this.now = now;
    this.annotateAdsb = annotateAdsb;
    this.socket = null;
    this.reconnectTimer = null;
    this.connected = false;
    this.active = false;
    this.queued = null;
    this.closed = false;
    this.counters = {frames: 0, forwarded: 0, overwritten: 0, matches: 0,
      unmatched: 0, invalid: 0, connectAttempts: 0};
    this.lastForwardAt = null;
    this.error = null;
    if (this.enabled) this.open();
  }

  open() {
    if (this.closed || !this.enabled || this.socket) return;
    this.counters.connectAttempts += 1;
    let socket;
    try { socket = this.connect({host: this.host, port: this.port}); }
    catch (error) { this.error = error.message; this.reconnect(); return; }
    this.socket = socket;
    socket.setNoDelay?.(true);
    socket.once('connect', () => {
      if (socket !== this.socket) return;
      this.connected = true;
      this.error = null;
      this.drain();
    });
    socket.on('error', error => {
      if (socket === this.socket) this.error = error.message;
    });
    socket.once('close', () => {
      if (socket !== this.socket) return;
      this.socket = null;
      this.connected = false;
      this.reconnect();
    });
  }

  reconnect() {
    if (this.closed || !this.enabled || this.reconnectTimer) return;
    this.reconnectTimer = this.schedule(() => {
      this.reconnectTimer = null;
      this.open();
    }, 5000);
    this.reconnectTimer.unref?.();
  }

  accept(frame) {
    if (!this.enabled) return;
    this.counters.frames += 1;
    let value = frame;
    if (typeof value === 'string') {
      try { value = JSON.parse(value); }
      catch (_) { value = null; }
    }
    if (!value || !finite(value.timestamp) || !Array.isArray(value.delay) ||
        !Array.isArray(value.doppler) || !Array.isArray(value.snr)) {
      this.counters.invalid += 1;
      return;
    }
    if (this.queued) this.counters.overwritten += 1;
    this.queued = value;
    this.drain();
  }

  async annotate(frame) {
    if (!this.annotateAdsb) {
      const annotations = (frame.delay || []).map(() => null);
      this.counters.unmatched += annotations.length;
      return {...frame, adsb: annotations};
    }
    const [feed, predictions] = await Promise.all([
      this.adsbSource.get('aircraft'), this.adsbSource.get('delayDoppler')]);
    const aircraft = new Map((feed?.aircraft || []).map(item => [normalizedHex(item?.hex), item]));
    let matches = 0;
    const annotations = (frame.delay || []).map((delay, index) => {
      const doppler = frame.doppler?.[index];
      if (!finite(delay) || !finite(doppler)) return null;
      let best = null, bestScore = Infinity;
      for (const [hex, prediction] of Object.entries(predictions || {})) {
        if (!finite(prediction?.delay) || !finite(prediction?.doppler)) continue;
        const delayError = delay - prediction.delay;
        const dopplerError = doppler - prediction.doppler;
        if (Math.abs(delayError) >= this.delayTolerance ||
            Math.abs(dopplerError) >= this.dopplerTolerance) continue;
        const score = Math.abs(delayError) / this.delayTolerance +
          Math.abs(dopplerError) / this.dopplerTolerance;
        if (score >= bestScore) continue;
        const source = aircraft.get(normalizedHex(hex)) || {};
        bestScore = score;
        best = {hex: normalizedHex(hex), flight: prediction.flight || source.flight || '',
          lat: source.lat, lon: source.lon, alt_baro: source.alt_baro,
          alt_geom: source.alt_geom, gs: source.gs, track: source.track,
          geom_rate: source.geom_rate, expected_delay: prediction.delay,
          expected_doppler: prediction.doppler, delay_residual: delayError,
          doppler_residual: dopplerError};
      }
      if (best) matches += 1;
      return best;
    });
    this.counters.matches += matches;
    this.counters.unmatched += annotations.length - matches;
    return {...frame, adsb: annotations};
  }

  async drain() {
    if (this.active || !this.connected || !this.socket || !this.queued) return;
    this.active = true;
    const frame = this.queued;
    this.queued = null;
    try {
      const annotated = await this.annotate(frame);
      if (!this.connected || !this.socket || this.closed) return;
      this.socket.write(`${JSON.stringify(annotated)}\n`);
      this.counters.forwarded += 1;
      this.lastForwardAt = this.now();
      this.error = null;
    } catch (error) {
      this.error = error.message;
    } finally {
      this.active = false;
      if (this.queued) this.drain();
    }
  }

  status() {
    return {enabled: this.enabled, connected: this.connected, host: this.host,
      port: this.port, ...this.counters, queued: this.queued !== null,
      active: this.active, lastForwardAt: this.lastForwardAt, error: this.error};
  }

  close() {
    this.closed = true;
    this.connected = false;
    if (this.reconnectTimer) this.cancel(this.reconnectTimer);
    this.reconnectTimer = null;
    this.socket?.destroy();
    this.socket = null;
  }
}

function createTrackerForwarder(config, adsbSource, dependencies) {
  const assisted = new TrackerForwarder(config, adsbSource, dependencies);
  const settings = config.network?.tracker_forward || {};
  if (settings.unassisted?.enabled !== true) return assisted;
  const unassisted = new TrackerForwarder(config, adsbSource, {
    ...dependencies, settings: settings.unassisted, annotateAdsb: false});
  return {
    accept(frame) { assisted.accept(frame); unassisted.accept(frame); },
    status() { return {...assisted.status(), unassisted: unassisted.status()}; },
    close() { assisted.close(); unassisted.close(); }
  };
}

module.exports = {TrackerForwarder, createTrackerForwarder};
