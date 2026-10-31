'use strict';

const http = require('http');
const https = require('https');
const {DelayDopplerHistory} = require('./adsb-geometry');

const agents = {
  'http:': new http.Agent({keepAlive: true, maxSockets: 2, scheduling: 'lifo'}),
  'https:': new https.Agent({keepAlive: true, maxSockets: 2, scheduling: 'lifo'})
};

function sourceUrl(value) {
  if (typeof value !== 'string' || !value || /[\s\\]/.test(value))
    throw new Error('Remote ID source must be an HTTP(S) address');
  const url = new URL(/^https?:\/\//i.test(value) ? value : `http://${value}`);
  if (!['http:', 'https:'].includes(url.protocol) || !url.hostname || url.username ||
      url.password || url.search || url.hash)
    throw new Error('Use a Remote ID HTTP(S) address without credentials or a query');
  if (!/\/data\/remoteid\.json\/?$/.test(url.pathname))
    url.pathname = `${url.pathname.replace(/\/$/, '')}/data/remoteid.json`;
  return url;
}

function readJson(url, {timeoutMs = 1200, maxBytes = 1024 * 1024} = {}) {
  return new Promise((resolve, reject) => {
    let request, response, settled = false;
    const timer = setTimeout(() => finish(new Error('Remote ID source timed out')), timeoutMs);
    const finish = (error, value) => {
      if (settled) return;
      settled = true; clearTimeout(timer);
      if (error) { response?.destroy(); request?.destroy(); reject(error); }
      else resolve(value);
    };
    try {
      request = (url.protocol === 'https:' ? https : http).get(url, {
        agent: agents[url.protocol], headers: {Accept: 'application/json',
          'Accept-Encoding': 'identity', 'User-Agent': 'VectorWarp/1.0 Remote-ID client'}
      }, incoming => {
        response = incoming;
        if (response.statusCode !== 200)
          return finish(new Error(`Remote ID source returned HTTP ${response.statusCode}`));
        let bytes = 0; const chunks = [];
        response.on('data', chunk => {
          bytes += chunk.length;
          if (bytes > maxBytes) return finish(new Error('Remote ID response exceeds size limit'));
          chunks.push(chunk);
        });
        response.on('end', () => {
          try { finish(null, JSON.parse(Buffer.concat(chunks).toString('utf8'))); }
          catch (_) { finish(new Error('Remote ID source returned invalid JSON')); }
        });
        response.on('error', finish);
        response.on('aborted', () => finish(new Error('Remote ID source disconnected')));
      });
      request.on('error', finish);
    } catch (error) { finish(error); }
  });
}

function remoteIdentity(value) {
  return String(value || '').trim().slice(0, 128);
}

function normalizeFeed(feed, {source = 'remote-id'} = {}) {
  if (feed?.schema_version !== 1 || !Number.isFinite(feed?.now) || !Array.isArray(feed?.drones))
    throw new Error('Remote ID source returned an unsupported feed');
  const aircraft = [];
  for (const drone of feed.drones) {
    const id = remoteIdentity(drone?.id);
    if (!id || !Number.isFinite(drone?.seen) || drone.seen < 0) continue;
    const seenPos = Number.isFinite(drone.seen_pos) ? drone.seen_pos : drone.seen;
    const operator = drone.operator && typeof drone.operator === 'object' ? {
      latitude: Number.isFinite(drone.operator.lat) ? drone.operator.lat : null,
      longitude: Number.isFinite(drone.operator.lon) ? drone.operator.lon : null,
      location_type: String(drone.operator.location_type || ''),
      id: remoteIdentity(drone.operator.id),
      altitude_takeoff_ft: Number.isFinite(drone.operator.alt_takeoff_ft) ?
        drone.operator.alt_takeoff_ft : null,
      seen_s: Number.isFinite(drone.operator.seen) ? drone.operator.seen : null
    } : null;
    aircraft.push({
      hex: `rid:${id}`, flight: id, r: id, t: 'UAS', type: 'remote_id',
      desc: String(drone.ua_type || 'Remote ID drone'), category: 'B6',
      lat: Number.isFinite(drone.lat) ? drone.lat : undefined,
      lon: Number.isFinite(drone.lon) ? drone.lon : undefined,
      alt_geom: Number.isFinite(drone.alt_geom_ft) ? drone.alt_geom_ft : undefined,
      gs: Number.isFinite(drone.gs) ? drone.gs : undefined,
      track: Number.isFinite(drone.track) ? drone.track : undefined,
      geom_rate: Number.isFinite(drone.geom_rate) ? drone.geom_rate : undefined,
      rssi: Number.isFinite(drone.rssi) ? drone.rssi : undefined,
      seen: drone.seen, seen_pos: seenPos,
      position_source: 'REMOTE_ID',
      remote_id: {id, id_type: String(drone.id_type || 'unknown'),
        ua_type: String(drone.ua_type || ''), self_id: String(drone.self_id || ''),
        source: String(drone.rid_source || ''), sources: Array.isArray(drone.rid_sources) ?
          drone.rid_sources.map(String).slice(0, 8) : [], operator},
      vectorwarp: {source_id: source, provider: source, position_source: 'REMOTE_ID',
        transport: String(drone.rid_source || '')}
    });
  }
  return {now: feed.now, aircraft, messages: Number(feed.messages) || 0};
}

function createRemoteIdSource(config, {fetchJson = readJson, now = Date.now,
  preview = false} = {}) {
  const settings = config.truth?.remote_id || {};
  const endpoint = sourceUrl(settings.source || 'http://127.0.0.1:8754');
  const pollMs = Number.isFinite(settings.poll_interval) ?
    Math.max(100, Math.min(10000, settings.poll_interval * 1000)) : 500;
  const timeoutMs = Number.isFinite(settings.source_timeout) ?
    Math.max(100, Math.min(10000, settings.source_timeout * 1000)) : 1200;
  const geometry = new DelayDopplerHistory(config, {truthKey: 'remote_id'});
  const presentationGeometry = new DelayDopplerHistory(config, {truthKey: 'remote_id'});
  let cached = null, pending = null, lastError = null, lastAttemptAt = 0, lastSuccessAt = 0;
  let warming = [], messages = 0;

  async function snapshot() {
    if (pending) return pending;
    if (cached && now() - lastAttemptAt < pollMs) return cached;
    lastAttemptAt = now();
    pending = fetchJson(endpoint, {timeoutMs}).then(value => {
      cached = normalizeFeed(value, {source: endpoint.host});
      messages = cached.messages; lastSuccessAt = now(); lastError = null;
      return cached;
    }).catch(error => {
      lastError = error;
      if (cached) return cached;
      throw error;
    }).finally(() => { pending = null; });
    return pending;
  }

  async function get(kind, {presentation = false} = {}) {
    if (settings.enabled !== true) throw new Error('Remote ID is disabled');
    if (preview) throw new Error('Preview: no live Remote ID connection');
    if (config.capture?.replay?.state) throw new Error('Replay: no live Remote ID connection');
    const data = await snapshot();
    if (kind === 'aircraft') return data;
    const result = (presentation ? presentationGeometry : geometry).update(data);
    warming = result.warming; return result.data;
  }

  async function status() {
    if (settings.enabled !== true) return {enabled: false, online: false, source: endpoint.href};
    if (!preview && !config.capture?.replay?.state) {
      try { await snapshot(); } catch (_) { /* status below contains the error */ }
    }
    const ageMs = lastSuccessAt ? now() - lastSuccessAt : null;
    const online = Boolean(cached && ageMs <= Math.max(5000, pollMs * 4));
    return {enabled: true, online, source: endpoint.href, messages,
      drones: cached?.aircraft?.length || 0, warming: warming.length,
      last_success_at: lastSuccessAt || null, age_ms: ageMs,
      error: lastError?.message || null,
      transports: {bluetooth: true, wifi: settings.wifi_enabled === true}};
  }

  return {
    get: kind => get(kind),
    getPresentation: kind => get(kind, {presentation: true}),
    getPresentationAt: kind => get(kind, {presentation: true}),
    status,
    clear() { cached = null; pending = null; lastError = null; geometry.clear();
      presentationGeometry.clear(); warming = []; messages = 0; }
  };
}

module.exports = {sourceUrl, readJson, normalizeFeed, createRemoteIdSource};
