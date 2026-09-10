'use strict';

const http = require('http');
const https = require('https');
const {DelayDopplerHistory} = require('./adsb-geometry');

function sourceAddress(value) {
  if (typeof value !== 'string' || !value || /[\s\\]/.test(value)) throw new Error('Invalid source address');
  const url = new URL(/^https?:\/\//i.test(value) ? value : `http://${value}`);
  if (!['http:', 'https:'].includes(url.protocol) || !url.hostname || url.username || url.password ||
      url.search || url.hash || value.includes('://') && !/^https?:\/\//i.test(value))
    throw new Error('Use a hostname, host:port or HTTP(S) address without credentials or a query');
  url.pathname = url.pathname.replace(/\/$/, '') + '/';
  return url;
}

function aircraftUrl(config) {
  return new URL('data/aircraft.json', sourceAddress(config.truth.adsb.tar1090));
}


function readJson(url, {timeoutMs = 1500, maxBytes = 4 * 1024 * 1024} = {}) {
  return new Promise((resolve, reject) => {
    let request;
    let response;
    let timer;
    let settled = false;
    const finish = (error, value) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      if (error) { response?.destroy(); request?.destroy(); reject(error); }
      else resolve(value);
    };
    timer = setTimeout(() => finish(new Error('ADS-B source timed out')), timeoutMs);
    try {
      request = (url.protocol === 'https:' ? https : http).get(url, {
        headers: {Accept: 'application/json', 'Accept-Encoding': 'identity'}
      }, incoming => {
        response = incoming;
        // Do not forward arbitrary redirects/credentials or accept an HTML login
        // page as a healthy source. Sources are selected from config, never query parameters.
        if (response.statusCode !== 200) return finish(new Error(`ADS-B source returned HTTP ${response.statusCode}`));
        let bytes = 0;
        const chunks = [];
        response.on('data', chunk => {
          bytes += chunk.length;
          if (bytes > maxBytes) return finish(new Error('ADS-B response exceeds size limit'));
          chunks.push(chunk);
        });
        response.on('end', () => {
          try { finish(null, JSON.parse(Buffer.concat(chunks).toString('utf8'))); }
          catch (_) { finish(new Error('ADS-B source returned invalid JSON')); }
        });
        response.on('error', error => finish(error));
        response.on('aborted', () => finish(new Error('ADS-B source disconnected')));
      });
      request.on('error', error => finish(error));
    } catch (error) { finish(error); }
  });
}

function createAdsbSource(config, {preview = false, fetchJson = readJson, now = Date.now} = {}) {
  const cache = new Map();
  const geometry = new DelayDopplerHistory(config);
  let warmup = [];
  const pollMs = Number.isFinite(config.truth?.adsb?.poll_interval) ?
    Math.max(100, Math.min(60000, config.truth.adsb.poll_interval * 1000)) : 1000;
  async function rawAircraft() {
    const previous = cache.get('raw');
    if (previous && (previous.pending || now() - previous.at < pollMs)) return previous.promise;
    const entry = {at: now(), pending: true};
    entry.promise = fetchJson(aircraftUrl(config)).then(data => {
      if (!Number.isFinite(data?.now) || !Array.isArray(data?.aircraft)) throw new Error('Invalid aircraft data');
      if (now() / 1000 - data.now > geometry.maxAge || data.now - now() / 1000 > geometry.maxAge)
        throw new Error('ADS-B aircraft data is stale or its clock is incorrect');
      return data;
    }).finally(() => { entry.pending = false; entry.at = now(); });
    cache.set('raw', entry); return entry.promise;
  }
  async function get(kind) {
    if (!config.truth?.adsb?.enabled) throw new Error('ADS-B is disabled');
    if (preview) throw new Error('Preview: no live ADS-B connection');
    const previous = cache.get(kind);
    if (previous && (previous.pending || now() - previous.at < pollMs)) return previous.promise;
    const entry = {at: now(), pending: true};
    entry.promise = rawAircraft().then(data => {
      if (kind === 'aircraft') return data;
      const result = geometry.update(data); warmup = result.warming; return result.data;
    })
      .finally(() => { entry.pending = false; entry.at = now(); });
    cache.set(kind, entry);
    return entry.promise;
  }
  async function status() {
    if (!config.truth?.adsb?.enabled) return {enabled: false, online: false};
    const results = await Promise.allSettled([get('aircraft'), get('delayDoppler')]);
    const feeds = results.map(result => result.status === 'fulfilled' ? {available: true} :
      {available: false, message: result.reason.message});
    return {enabled: true, online: feeds.every(feed => feed.available), aircraft: feeds[0],
      delayDoppler: {...feeds[1], warming: warmup.length,
        message: feeds[1].available && warmup.length ? 'Feed online; waiting for motion updates.' : feeds[1].message}};
  }
  return {get, status, clear: () => { cache.clear(); geometry.clear(); }};
}

module.exports = {sourceAddress, aircraftUrl, readJson, createAdsbSource};
