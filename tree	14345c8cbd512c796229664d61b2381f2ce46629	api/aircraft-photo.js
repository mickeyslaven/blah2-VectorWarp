'use strict';

const https = require('https');
const fs = require('fs');
const path = require('path');

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

function createAircraftPhotoLookup(config, {fetchJson = readJson, now = Date.now,
  cacheFile = null} = {}) {
  const cache = new Map();
  const disk = new Map();
  if (cacheFile) {
    try {
      const saved = JSON.parse(fs.readFileSync(cacheFile, 'utf8'));
      for (const [key, entry] of Object.entries(saved.entries || {}))
        if (entry && Number.isFinite(entry.expiresAt) && entry.value)
          disk.set(key, entry);
    } catch (_) { /* An absent or old cache must never block the live map. */ }
  }
  let writePending = false;
  function persist() {
    if (!cacheFile || writePending) return;
    writePending = true;
    setTimeout(async () => {
      writePending = false;
      try {
        const entries = Object.fromEntries([...disk].filter(([, entry]) => entry.expiresAt > now())
          .slice(-MAX_CACHE_ENTRIES));
        await fs.promises.mkdir(path.dirname(cacheFile), {recursive: true});
        const temporary = `${cacheFile}.${process.pid}.tmp`;
        await fs.promises.writeFile(temporary, JSON.stringify({version: 1, entries}), {mode: 0o600});
        await fs.promises.rename(temporary, cacheFile);
      } catch (_) { /* Photo metadata caching is optional. */ }
    }, 25).unref?.();
  }
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
    const saved = disk.get(key);
    if (saved && now() < saved.expiresAt) {
      const promise = Promise.resolve(saved.value);
      cache.set(key, {expiresAt: saved.expiresAt, promise});
      return promise;
    }
    const entry = {expiresAt: now() + ERROR_TTL_MS};
    entry.promise = fetchJson(new URL(`https://api.planespotters.net/pub/photos/hex/${normalized}`))
      .then(normalizePlanespotters).then(value => {
        entry.expiresAt = now() + (value.available ? POSITIVE_TTL_MS : NEGATIVE_TTL_MS);
        disk.set(key, {expiresAt: entry.expiresAt, value}); persist();
        return value;
      }, () => {
        entry.expiresAt = now() + ERROR_TTL_MS;
        const value = {available: false, provider: 'Planespotters.net', temporarily_unavailable: true};
        disk.set(key, {expiresAt: entry.expiresAt, value}); persist();
        return value;
      });
    cache.set(key, entry);
    prune();
    return entry.promise;
  }
  return {lookup, cache, disk};
}

module.exports = {createAircraftPhotoLookup, normalizePlanespotters, readJson};
