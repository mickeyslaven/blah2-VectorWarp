'use strict';

const http = require('http');
const https = require('https');
const {DelayDopplerHistory} = require('./adsb-geometry');
const {fuseAircraftData} = require('./adsb-fusion');
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

function configuredAircraftUrl(value) {
  return new URL('data/aircraft.json', sourceAddress(value));
}

function exchangeSource(value) {
  if (typeof value !== 'string' || !value.startsWith('exchange:')) return null;
  const raw = value.slice('exchange:'.length);
  if (!raw || /[\s\\]/.test(raw)) throw new Error('Invalid ADS-B exchange endpoint');
  const url = new URL(raw);
  if (!['http:', 'https:'].includes(url.protocol) || !url.hostname || url.username ||
      url.password || url.search || url.hash)
    throw new Error('Use an HTTP(S) ADS-B exchange endpoint without credentials or a query');
  return {url, adapter: 'adsbx-v2', kind: 'exchange'};
}

function configuredAircraftSource(value) {
  return exchangeSource(value) ||
    {url: configuredAircraftUrl(value), adapter: 'readsb', kind: 'http'};
}

function normalizeAircraftSnapshot(data, adapter) {
  if (adapter !== 'adsbx-v2') return data;
  if (!Number.isFinite(data?.now) || !Array.isArray(data?.ac))
    throw new Error('Invalid ADS-B exchange data');
  // ADS-B Exchange-compatible APIs report epoch milliseconds while readsb
  // reports epoch seconds. Keep per-aircraft seen_pos untouched: it is already
  // relative to the snapshot time and therefore retains event-time freshness.
  const now = data.now > 100000000000 ? data.now / 1000 : data.now;
  return {now, aircraft: data.ac};
}

function configuredSources(config) {
  const values = config.truth?.adsb?.sources;
  return Array.isArray(values) && values.length ? [...values] : [config.truth?.adsb?.tar1090];
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
        headers: {Accept: 'application/json', 'Accept-Encoding': 'identity',
          'User-Agent': 'VectorWarp/1.0 ADS-B client'}
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
  let fusionState = null;
  const sourceTimeoutMs = Number.isFinite(config.truth?.adsb?.source_timeout) ?
    Math.max(100, Math.min(10000, config.truth.adsb.source_timeout * 1000)) : 1500;
  const slots = configuredSources(config).map((configured, index) => {
    const classification = classifyAdsbSource(configured);
    const exchange = classification.mode === 'explicit' ? exchangeSource(configured) : null;
    return {configured, classification, discoveryEntry: undefined, explicitUrl: undefined,
      state: {id: configured, mode: classification.mode,
        ...(classification.mode === 'explicit' ? {
          kind: exchange?.kind || 'http', adapter: exchange?.adapter || 'readsb',
          authoritative: !exchange,
          reliability: exchange ? .65 : Math.max(.8, 1 - index * .05)
        } : {}),
        state: 'configured', healthy: null, consecutive_failures: 0,
        consecutive_successes: 0, configured_index: index}};
  });
  const pollMs = Number.isFinite(config.truth?.adsb?.poll_interval) ?
    Math.max(100, Math.min(60000, config.truth.adsb.poll_interval * 1000)) : 1000;
  const discoveryOptions = {fetchJson, now, maxAgeSeconds: geometry.maxAge,
    timeoutMs: sourceTimeoutMs};
  async function resolveSource(slot) {
    if (slot.classification.mode === 'explicit') {
      const selected = configuredAircraftSource(slot.configured);
      slot.explicitUrl ||= selected.url;
      slot.state = {...slot.state, endpoint: slot.explicitUrl.href};
      return {source: slot.state,
        read: async () => normalizeAircraftSnapshot(
          await fetchJson(slot.explicitUrl, {timeoutMs: sourceTimeoutMs}), selected.adapter)};
    }
    if (slot.discoveryEntry && now() - slot.discoveryEntry.at < discoveryCacheMs)
      return slot.discoveryEntry.promise;
    const entry = {at: now()};
    slot.state = {...slot.state, state: 'discovering'};
    entry.promise = (slot.classification.mode === 'auto' ? discover(discoveryOptions) :
      selectLocal(slot.configured, discoveryOptions)).then(selection => {
      slot.state = {...slot.state, ...selection.source, id: slot.configured,
        mode: slot.classification.mode, state: 'active'};
      return selection;
    }, error => {
      slot.state = {...slot.state, state: 'error', healthy: false, message: error.message,
        ...(error.sources?.length ? {candidates: error.sources} : {})};
      throw error;
    });
    slot.discoveryEntry = entry;
    return entry.promise;
  }
  async function readSource(slot) {
    const startedAt = now();
    try {
      const selection = await resolveSource(slot);
      const data = validateAircraftData(await selection.read(),
        {now, maxAgeSeconds: geometry.maxAge});
      const receivedAtMs = now();
      const ages = data.aircraft.filter(aircraft => Number.isFinite(aircraft?.lat) &&
        Number.isFinite(aircraft?.lon) && Number.isFinite(aircraft?.seen_pos) &&
        aircraft.seen_pos >= 0).map(aircraft => receivedAtMs / 1000 -
          (data.now - aircraft.seen_pos)).filter(Number.isFinite);
      const usable = ages.filter(age => age >= 0 && age <= geometry.maxAge);
      const truthAge = Math.min(geometry.maxAge,
        config.process?.bearing?.manifold_recorder?.max_truth_age_s ?? 3);
      const fresh = ages.filter(age => age >= 0 && age <= truthAge);
      const {message, candidates, ...prior} = slot.state;
      const selectedSource = {...selection.source};
      delete selectedSource.message; delete selectedSource.candidates;
      slot.state = {...prior, ...selectedSource, id: slot.configured,
        mode: slot.classification.mode, state: 'active', healthy: true,
        consecutive_failures: 0,
        consecutive_successes: (slot.state.consecutive_successes || 0) + 1,
        last_success_at: receivedAtMs, latency_ms: Math.max(0, receivedAtMs - startedAt),
        freshness: {snapshot_age_s: receivedAtMs / 1000 - data.now,
          positioned: ages.length, usable_positions: usable.length,
          fresh_positions: fresh.length, stale_positions: ages.length - usable.length,
          truth_age_limit_s: truthAge, position_age_limit_s: geometry.maxAge,
          freshest_position_age_s: fresh.length ? Math.min(...fresh) : null}};
      return {data, source: slot.state, receivedAtMs};
    } catch (error) {
      const failedAt = now();
      slot.state = {...slot.state, state: 'error', healthy: false, message: error.message,
        consecutive_failures: (slot.state.consecutive_failures || 0) + 1,
        consecutive_successes: 0, last_error_at: failedAt,
        latency_ms: Math.max(0, failedAt - startedAt)};
      throw error;
    }
  }
  async function rawAircraft() {
    const previous = cache.get('raw');
    if (previous && (previous.pending || now() - previous.at < pollMs)) return previous.promise;
    const entry = {at: now(), pending: true};
    entry.promise = Promise.allSettled(slots.map(readSource)).then(results => {
      const healthy = results.filter(result => result.status === 'fulfilled')
        .map(result => result.value);
      if (!healthy.length) throw new Error(`All ADS-B sources failed: ${results.map((result, index) =>
        `${slots[index].configured}: ${result.reason?.message || 'unavailable'}`).join('; ')}`);
      const fusedAt = now();
      const data = fuseAircraftData(healthy, {nowMs: fusedAt,
        maxPositionAgeSeconds: geometry.maxAge});
      fusionState = {...data.vectorwarp_fusion, fused_at: fusedAt,
        healthy_sources: healthy.length, failed_sources: slots.length - healthy.length};
      return data;
    })
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
    if (!config.truth?.adsb?.enabled) {
      const sources = slots.map(slot => ({...slot.state, state: 'disabled'}));
      return {enabled: false, online: false, degraded: false,
        source: sources[0], sources, fusion: null};
    }
    const results = await Promise.allSettled([get('aircraft'), get('delayDoppler')]);
    const feeds = results.map(result => result.status === 'fulfilled' ? {available: true} :
      {available: false, message: result.reason.message});
    const online = feeds.every(feed => feed.available);
    if (preview || config.capture?.replay?.state)
      for (const slot of slots) slot.state = {...slot.state, state: 'inactive', message: feeds[0].message};
    const sources = slots.map(slot => ({...slot.state}));
    const degraded = online && sources.some(source => source.healthy === false);
    return {enabled: true, online, degraded, source: sources[0], sources,
      fusion: fusionState, aircraft: feeds[0],
      delayDoppler: {...feeds[1], warming: warmup.length,
        message: feeds[1].available && warmup.length ? 'Feed online; waiting for motion updates.' : feeds[1].message}};
  }
  return {get, status, clear: () => {
    cache.clear(); geometry.clear(); fusionState = null;
    for (const slot of slots) slot.discoveryEntry = undefined;
  }};
}

module.exports = {sourceAddress, aircraftUrl, configuredSources, configuredAircraftSource,
  normalizeAircraftSnapshot, readJson, createAdsbSource};
