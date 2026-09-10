'use strict';

const http = require('http');
const https = require('https');
const {DelayDopplerHistory} = require('./adsb-geometry');
const {classifyAdsbSource, discoverLocalAdsb, selectLocalAdsb,
  validateAircraftData} = require('./adsb-discovery');

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

function createAdsbSource(config, {preview = false, fetchJson = readJson, now = Date.now,
  discover = discoverLocalAdsb, selectLocal = selectLocalAdsb,
  discoveryCacheMs = 30000} = {}) {
  const cache = new Map();
  const geometry = new DelayDopplerHistory(config);
  let warmup = [];
  let discoveryEntry;
  let explicitUrl;
  const configured = config.truth?.adsb?.tar1090;
  const classification = classifyAdsbSource(configured);
  let sourceState = classification.mode === 'explicit' ?
    {mode: 'explicit', kind: 'http', state: 'configured'} :
    {mode: classification.mode, state: 'configured'};
  const pollMs = Number.isFinite(config.truth?.adsb?.poll_interval) ?
    Math.max(100, Math.min(60000, config.truth.adsb.poll_interval * 1000)) : 1000;
  const discoveryOptions = {fetchJson, now, maxAgeSeconds: geometry.maxAge};
  async function resolveSource() {
    if (classification.mode === 'explicit') {
      explicitUrl ||= aircraftUrl(config);
      sourceState = {...sourceState, endpoint: explicitUrl.href};
      return {source: sourceState, read: () => fetchJson(explicitUrl)};
    }
    if (discoveryEntry && now() - discoveryEntry.at < discoveryCacheMs)
      return discoveryEntry.promise;
    const entry = {at: now()};
    sourceState = {mode: classification.mode, state: 'discovering'};
    entry.promise = (classification.mode === 'auto' ? discover(discoveryOptions) :
      selectLocal(configured, discoveryOptions)).then(selection => {
      sourceState = {...selection.source, mode: classification.mode, state: 'active'};
      return selection;
    }, error => {
      sourceState = {mode: classification.mode, state: 'error', message: error.message,
        ...(error.sources?.length ? {candidates: error.sources} : {})};
      throw error;
    });
    discoveryEntry = entry;
    return entry.promise;
  }
  async function rawAircraft() {
    const previous = cache.get('raw');
    if (previous && (previous.pending || now() - previous.at < pollMs)) return previous.promise;
    const entry = {at: now(), pending: true};
    entry.promise = resolveSource().then(selection => selection.read()).then(data =>
      validateAircraftData(data, {now, maxAgeSeconds: geometry.maxAge}))
      .finally(() => { entry.pending = false; entry.at = now(); });
    cache.set('raw', entry); return entry.promise;
  }
  async function get(kind) {
    if (!config.truth?.adsb?.enabled) throw new Error('ADS-B is disabled');
    if (preview) throw new Error('Preview: no live ADS-B connection');
    if (config.capture?.replay?.state) throw new Error('Replay: no live ADS-B connection');
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
    if (!config.truth?.adsb?.enabled) return {enabled: false, online: false,
      source: {...sourceState, state: 'disabled'}};
    const results = await Promise.allSettled([get('aircraft'), get('delayDoppler')]);
    const feeds = results.map(result => result.status === 'fulfilled' ? {available: true} :
      {available: false, message: result.reason.message});
    const online = feeds.every(feed => feed.available);
    if (online) {
      const {message, candidates, ...activeSource} = sourceState;
      sourceState = {...activeSource, state: 'active'};
    }
    else if (!preview && !config.capture?.replay?.state && sourceState.state !== 'error')
      sourceState = {...sourceState, state: 'error', message: feeds.find(feed => !feed.available)?.message};
    else if (preview || config.capture?.replay?.state)
      sourceState = {...sourceState, state: 'inactive', message: feeds[0].message};
    return {enabled: true, online, source: sourceState, aircraft: feeds[0],
      delayDoppler: {...feeds[1], warming: warmup.length,
        message: feeds[1].available && warmup.length ? 'Feed online; waiting for motion updates.' : feeds[1].message}};
  }
  return {get, status, clear: () => { cache.clear(); geometry.clear(); discoveryEntry = undefined; }};
}

module.exports = {sourceAddress, aircraftUrl, readJson, createAdsbSource};
