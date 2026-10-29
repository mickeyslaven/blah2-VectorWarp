'use strict';

const https = require('https');

const POSITIVE_TTL_MS = 24 * 60 * 60 * 1000;
const NEGATIVE_TTL_MS = 15 * 60 * 1000;
const ERROR_TTL_MS = 60 * 1000;
const MAX_CACHE_ENTRIES = 512;

function readJson(url, {timeoutMs = 2500, maxBytes = 256 * 1024} = {}) {
  return new Promise((resolve, reject) => {
    let request;
    let response;
    let settled = false;
    const finish = (error, value) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      if (error) { response?.destroy(); request?.destroy(); reject(error); }
      else resolve(value);
    };
    const timer = setTimeout(() => finish(new Error('Aircraft photo provider timed out')), timeoutMs);
    request = https.get(url, {headers: {Accept: 'application/json', 'Accept-Encoding': 'identity',
      'User-Agent': 'VectorWarp/1.0 (+https://github.com/mickeyslaven/blah2-VectorWarp)'}}, incoming => {
      response = incoming;
      if (response.statusCode !== 200)
        return finish(new Error(`Aircraft photo provider returned HTTP ${response.statusCode}`));
      const chunks = [];
      let bytes = 0;
      response.on('data', chunk => {
        bytes += chunk.length;
        if (bytes > maxBytes) return finish(new Error('Aircraft photo response exceeds size limit'));
        chunks.push(chunk);
      });
      response.on('end', () => {
        try { finish(null, JSON.parse(Buffer.concat(chunks).toString('utf8'))); }
        catch (_) { finish(new Error('Aircraft photo provider returned invalid JSON')); }
      });
      response.on('error', error => finish(error));
      response.on('aborted', () => finish(new Error('Aircraft photo provider disconnected')));
    });
    request.on('error', error => finish(error));
  });
}

function safeProviderUrl(value, host) {
  try {
    const url = new URL(value);
    return url.protocol === 'https:' && url.hostname === host && !url.username && !url.password ?
      url.href : null;
  } catch (_) { return null; }
}

function normalizePlanespotters(payload) {
  const photo = Array.isArray(payload?.photos) ? payload.photos[0] : null;
  if (!photo) return {available: false, provider: 'Planespotters.net'};
  const thumbnail = safeProviderUrl(photo.thumbnail?.src, 't.plnspttrs.net');
  const link = safeProviderUrl(photo.link, 'www.planespotters.net');
  if (!thumbnail || !link) return {available: false, provider: 'Planespotters.net'};
  return {available: true, provider: 'Planespotters.net', thumbnail, link,
    photographer: String(photo.photographer || '').slice(0, 120)};
}

function createAircraftPhotoLookup(config, {fetchJson = readJson, now = Date.now} = {}) {
  const cache = new Map();
  function prune() {
    while (cache.size > MAX_CACHE_ENTRIES) cache.delete(cache.keys().next().value);
  }
  async function lookup(hex) {
    const normalized = String(hex || '').trim().toLowerCase();
    if (!/^[0-9a-f]{6}$/.test(normalized)) {
      const error = new Error('Aircraft ICAO address must contain six hexadecimal characters');
      error.statusCode = 400;
      throw error;
    }
    const provider = config.truth?.adsb?.aircraft_photo_provider || 'disabled';
    if (provider === 'disabled') return {available: false, enabled: false, provider: null};
    if (provider !== 'planespotters') {
      const error = new Error('Unsupported aircraft photo provider');
      error.statusCode = 400;
      throw error;
    }
    const key = `${provider}:${normalized}`;
    const prior = cache.get(key);
    if (prior && now() < prior.expiresAt) return prior.promise;
    const entry = {expiresAt: now() + ERROR_TTL_MS};
    entry.promise = fetchJson(new URL(`https://api.planespotters.net/pub/photos/hex/${normalized}`))
      .then(normalizePlanespotters).then(value => {
        entry.expiresAt = now() + (value.available ? POSITIVE_TTL_MS : NEGATIVE_TTL_MS);
        return value;
      }, () => {
        entry.expiresAt = now() + ERROR_TTL_MS;
        return {available: false, provider: 'Planespotters.net', temporarily_unavailable: true};
      });
    cache.set(key, entry);
    prune();
    return entry.promise;
  }
  return {lookup, cache};
}

module.exports = {createAircraftPhotoLookup, normalizePlanespotters, readJson};
