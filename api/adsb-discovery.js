'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');

const MAX_BYTES = 4 * 1024 * 1024;
const LOCAL_SOURCES = Object.freeze([
  Object.freeze({value: 'local:readsb', label: 'Local readsb',
    path: '/run/readsb/aircraft.json'}),
  Object.freeze({value: 'local:dump1090-fa', label: 'Local dump1090-fa',
    path: '/run/dump1090-fa/aircraft.json'}),
  Object.freeze({value: 'local:dump1090', label: 'Local dump1090',
    path: '/run/dump1090/aircraft.json'}),
  Object.freeze({value: 'local:dump1090-mutability', label: 'Local dump1090-mutability',
    path: '/run/dump1090-mutability/aircraft.json'})
]);
const HTTP_SOURCES = Object.freeze([
  Object.freeze({value: 'loopback:decoder-ipv4', label: 'Loopback decoder (IPv4)',
    endpoint: 'http://127.0.0.1:8080/data/aircraft.json'}),
  Object.freeze({value: 'loopback:decoder-ipv6', label: 'Loopback decoder (IPv6)',
    endpoint: 'http://[::1]:8080/data/aircraft.json'}),
  Object.freeze({value: 'loopback:tar1090-ipv4', label: 'Local tar1090 (IPv4)',
    endpoint: 'http://127.0.0.1/tar1090/data/aircraft.json'}),
  Object.freeze({value: 'loopback:tar1090-ipv6', label: 'Local tar1090 (IPv6)',
    endpoint: 'http://[::1]/tar1090/data/aircraft.json'})
]);
const SOURCE_CHOICES = Object.freeze([
  Object.freeze({value: 'auto', label: 'Discover locally', mode: 'auto'}),
  ...LOCAL_SOURCES.map(source => Object.freeze({value: source.value,
    label: source.label, mode: 'local'}))
]);

// The selection values remain stable across platforms. Locations are a local
// allowlist; the browser cannot turn ADS-B discovery into an arbitrary file read.
function localFileSources({platform = process.platform, arch = process.arch,
  home = os.homedir(), brewPrefix = process.env.HOMEBREW_PREFIX} = {}) {
  if (platform !== 'darwin') return LOCAL_SOURCES;
  const prefix = brewPrefix || (arch === 'arm64' ? '/opt/homebrew' : '/usr/local');
  if (!path.isAbsolute(prefix) || !path.isAbsolute(home))
    throw new Error('Local ADS-B directories must be absolute paths');
  return LOCAL_SOURCES.flatMap(source => {
    const decoder = source.value.slice('local:'.length);
    return [
      path.join(prefix, 'var/run', decoder, 'aircraft.json'),
      path.join(home, 'Library/Application Support/VectorWarp/adsb', decoder, 'aircraft.json')
    ].map(filename => ({...source, path: filename}));
  });
}

function withDeadline(promise, timeoutMs, message = 'ADS-B file source timed out') {
  return new Promise((resolve, reject) => {
    let settled = false;
    const timer = setTimeout(() => {
      if (!settled) { settled = true; reject(new Error(message)); }
    }, timeoutMs);
    Promise.resolve(promise).then(value => {
      if (!settled) { settled = true; clearTimeout(timer); resolve(value); }
    }, error => {
      if (!settled) { settled = true; clearTimeout(timer); reject(error); }
    });
  });
}

function classifyAdsbSource(value) {
  if (value === 'auto') return {mode: 'auto'};
  const local = LOCAL_SOURCES.find(source => source.value === value);
  return local ? {mode: 'local', source: local} : {mode: 'explicit'};
}

function validateAircraftData(data, {now = Date.now, maxAgeSeconds = 30} = {}) {
  if (!Number.isFinite(data?.now) || !Array.isArray(data?.aircraft))
    throw new Error('Invalid aircraft data');
  const age = now() / 1000 - data.now;
  if (age > maxAgeSeconds || age < -maxAgeSeconds)
    throw new Error('ADS-B aircraft data is stale or its clock is incorrect');
  return data;
}

async function readJsonFile(filename, {maxBytes = MAX_BYTES, timeoutMs = 500} = {}) {
  const operation = (async () => {
    let handle;
    try {
      const flags = fs.constants.O_RDONLY | fs.constants.O_NONBLOCK |
        (fs.constants.O_NOFOLLOW || 0);
      handle = await fs.promises.open(filename, flags);
      // Inspect the opened descriptor, not the path: a path replacement cannot
      // turn this bounded regular-file read into a pipe or device read.
      const stat = await handle.stat();
      if (!stat.isFile()) throw new Error('ADS-B source is not a regular file');
      if (stat.size > maxBytes) throw new Error('ADS-B response exceeds size limit');
      // Read one byte beyond the limit so a file that grows after fstat cannot
      // evade the bound.
      const buffer = Buffer.alloc(maxBytes + 1);
      let bytesRead = 0;
      while (bytesRead < buffer.length) {
        const result = await handle.read(buffer, bytesRead, buffer.length - bytesRead, bytesRead);
        if (!result.bytesRead) break;
        bytesRead += result.bytesRead;
      }
      if (bytesRead > maxBytes) throw new Error('ADS-B response exceeds size limit');
      let data;
      try { data = JSON.parse(buffer.subarray(0, bytesRead).toString('utf8')); }
      catch (_) { throw new Error('ADS-B source returned invalid JSON'); }
      return {data, identity: `file:${stat.dev}:${stat.ino}`};
    } finally {
      await handle?.close();
    }
  })();
  return withDeadline(operation, timeoutMs);
}

function discoveryError(message, sources = []) {
  const error = new Error(message);
  error.sources = sources;
  return error;
}

function oneShotReader(sample, reader) {
  let first = sample;
  return async () => {
    if (first !== undefined) {
      const value = first;
      first = undefined;
      return value;
    }
    return reader();
  };
}

async function probeFile(candidate, options) {
  const first = await readJsonFile(candidate.path, options);
  validateAircraftData(first.data, options);
  const source = {mode: 'local', kind: 'file', value: candidate.value,
    label: candidate.label, path: candidate.path};
  return {identity: first.identity, source, read: oneShotReader(first.data, async () => {
    const next = await readJsonFile(candidate.path, options);
    return validateAircraftData(next.data, options);
  })};
}

async function probeHttp(candidate, {fetchJson, ...options}) {
  if (typeof fetchJson !== 'function') throw new Error('HTTP discovery requires a JSON reader');
  const url = new URL(candidate.endpoint);
  const first = validateAircraftData(await fetchJson(url, options), options);
  const source = {mode: 'auto', kind: 'http', value: candidate.value,
    label: candidate.label, endpoint: url.href};
  return {identity: `http:${url.href}`, source, read: oneShotReader(first, async () =>
    validateAircraftData(await fetchJson(url, options), options))};
}

async function healthyUnique(candidates, probe) {
  const results = await Promise.all(candidates.map(candidate =>
    probe(candidate).catch(error => ({error}))));
  const unique = new Map();
  for (const result of results) {
    if (!result.error && !unique.has(result.identity)) unique.set(result.identity, result);
  }
  return [...unique.values()];
}

function requireOne(sources, absentMessage) {
  if (!sources.length) throw discoveryError(absentMessage);
  if (sources.length > 1) throw discoveryError(
    'Multiple local ADS-B sources are healthy; select one explicitly.',
    sources.map(result => result.source));
  return sources[0];
}

async function selectLocalAdsb(value, {fileSources = localFileSources(), ...options} = {}) {
  const candidates = fileSources.filter(candidate => candidate.value === value);
  if (!candidates.length) throw new Error('Unknown local ADS-B source');
  const source = candidates[0];
  try {
    if (candidates.length === 1) return await probeFile(source, options);
    const healthy = await healthyUnique(candidates, candidate => probeFile(candidate, options));
    if (healthy.length > 1) throw discoveryError(
      `Multiple ${source.label} files are healthy; stop the duplicate decoder or select its HTTP address.`,
      healthy.map(result => result.source));
    if (healthy.length === 1) return healthy[0];
    return await probeFile(source, options);
  }
  catch (error) {
    if (error.sources) throw error;
    throw discoveryError(`${source.label} is unavailable: ${error.message}`,
      [{mode: 'local', kind: 'file', value: source.value,
        label: source.label, path: source.path}]);
  }
}

async function discoverLocalAdsb({fileSources = localFileSources(),
  httpSources = HTTP_SOURCES, ...options} = {}) {
  // Decoder JSON is authoritative and does not require a tar1090 frontend.
  const files = await healthyUnique(fileSources, candidate => probeFile(candidate, options));
  if (files.length) return requireOne(files);
  const http = await healthyUnique(httpSources, candidate => probeHttp(candidate, options));
  return requireOne(http, 'No healthy local ADS-B decoder source was discovered.');
}

module.exports = {LOCAL_SOURCES, HTTP_SOURCES, SOURCE_CHOICES, localFileSources, classifyAdsbSource,
  withDeadline,
  validateAircraftData, readJsonFile, selectLocalAdsb, discoverLocalAdsb};
