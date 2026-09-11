'use strict';

// Observation only. Never open an SDR, install a package, or change a service.
const fs = require('fs').promises;
const path = require('path');
const {execFile} = require('child_process');
const {getUpstreamStatus} = require('./upstream-status');

const SERVICE_UNITS = Object.freeze({
  'kraken-suite-v2': ['vectorwarp-heimdall.service', 'krakensdr-suite-v2.service', 'heimdall-v2.service'],
  'sdrplay-api': ['vectorwarp-sdrplay.service', 'sdrplay.service', 'sdrplay_apiService.service']
});
const COMMANDS = Object.freeze({
  libraries: ['/usr/sbin/ldconfig', '/sbin/ldconfig'],
  service: ['/usr/bin/systemctl', '/bin/systemctl']
});

async function smallFile(file, limit = 1024) {
  let handle;
  try {
    handle = await fs.open(file, 'r');
    const buffer = Buffer.alloc(limit + 1);
    const {bytesRead} = await handle.read(buffer, 0, buffer.length, 0);
    if (bytesRead > limit) throw new Error('Receiver metadata exceeds its size limit.');
    return buffer.subarray(0, bytesRead).toString('utf8').trim();
  } catch (error) {
    if (error.code === 'ENOENT' || error.code === 'ENOTDIR') return null;
    throw error;
  } finally {
    if (handle) await handle.close();
  }
}

function runCommand(file, args, {signal, timeoutMs = 1500} = {}) {
  return new Promise((resolve, reject) => {
    execFile(file, args, {
      signal, timeout: Math.min(timeoutMs, 3000), maxBuffer: 262144,
      encoding: 'utf8', windowsHide: true, cwd: '/',
      env: {PATH: '/usr/sbin:/usr/bin:/sbin:/bin', LANG: 'C', LC_ALL: 'C'}
    }, (error, stdout) => {
      if (error) {
        // Do not relay command output or paths into the browser error message.
        const failure = new Error('The read-only receiver software check failed.');
        failure.code = error.code;
        reject(failure);
      } else resolve(stdout);
    });
  });
}

function dependenciesFromCache(cache) {
  if (typeof cache !== 'string' || Buffer.byteLength(cache) > 262144)
    throw new Error('Invalid runtime-library inventory.');
  const result = {
    Kraken: {state: 'unknown'}, RspDuo: {state: 'unknown'},
    Usrp: {state: 'unknown'}, HackRF: {state: 'unknown'}
  };
  // The linker cache proves presence, not successful hardware initialization.
  if (/^\s*libhackrf\.so(?:\.[\d.]+)?\s/m.test(cache))
    result.HackRF = {state: 'installed'};
  const sdrplay = /^\s*libsdrplay_api\.so\.(3\.15(?:\.\d+)?)\s/m.exec(cache);
  if (sdrplay) result.RspDuo = {state: 'installed', version: sdrplay[1]};
  const uhd = [...cache.matchAll(/^\s*libuhd\.so\.(\d+)\.(\d+)([.\d]*)\s/gm)]
    .find(match => Number(match[1]) > 4 || (Number(match[1]) === 4 && Number(match[2]) >= 8));
  if (uhd) result.Usrp = {state: 'installed', version: `${uhd[1]}.${uhd[2]}${uhd[3]}`};
  // An absent cache entry is not proof of absence: SDKs can use private RPATHs.
  return result;
}

function serviceFromProperties(properties) {
  if (typeof properties !== 'string' || properties.length > 65536)
    throw new Error('Invalid service observation.');
  const sections = properties.trim().split(/\n\s*\n/);
  const loaded = sections.filter(section => /^LoadState=loaded$/m.test(section));
  if (loaded.some(section => /^ActiveState=active$/m.test(section))) return {state: 'running'};
  if (loaded.length) return {state: 'stopped'};
  return {state: 'unknown'};
}

function createReceiverProbes(config, options = {}) {
  const snapshot = JSON.parse(JSON.stringify(config));
  // Options are dependency injection for tests, never request parameters.
  const read = options.readFile || smallFile;
  const list = options.readdir || (directory => fs.readdir(directory));
  const run = options.run || runCommand;
  const upstream = options.upstream || getUpstreamStatus;
  const exists = options.exists || (async file => {
    try { await fs.access(file, require('fs').constants.X_OK); return true; }
    catch (_) { return false; }
  });

  async function command(kind, args, context) {
    for (const file of COMMANDS[kind]) {
      if (await exists(file)) return run(file, args, context);
    }
    throw new Error('The host does not provide this read-only receiver check.');
  }

  return {
    async usbInventory(_request, {signal} = {}) {
      const directory = '/sys/bus/usb/devices';
      const names = await list(directory);
      if (!Array.isArray(names) || names.length > 512)
        throw new Error('USB inventory exceeds its device limit.');
      const devices = [];
      for (const name of names) {
        if (signal?.aborted) throw new Error('Receiver discovery was cancelled.');
        if (typeof name !== 'string' || !/^[A-Za-z0-9_.+-]+$/.test(name) || name === '.' || name === '..') continue;
        const base = path.join(directory, name);
        const product = await read(path.join(base, 'product'));
        if (!product) continue;
        const manufacturer = await read(path.join(base, 'manufacturer'));
        const serial = await read(path.join(base, 'serial'));
        devices.push({manufacturer, product, serial});
        if (devices.length > 64) throw new Error('USB inventory exceeds its device limit.');
      }
      return devices;
    },
    async dependencyInventory(_request, context) {
      return dependenciesFromCache(await command('libraries', ['-p'], context));
    },
    async serviceStatus({serviceId}, context) {
      const units = SERVICE_UNITS[serviceId];
      if (!units) throw new Error('Unknown receiver service.');
      return serviceFromProperties(await command('service',
        ['show', '--no-pager', '--property=LoadState', '--property=ActiveState', '--', ...units], context));
    },
    async configuredUpstreamStatus(endpoint, context) {
      const saved = snapshot.capture?.device?.heimdall;
      if (!saved || endpoint.host !== saved.host || endpoint.dataPort !== saved.port ||
          endpoint.controlPort !== (saved.control_port ?? 8092))
        throw new Error('Receiver endpoint changed; reload receiver setup.');
      const observed = await upstream(snapshot, {timeoutMs: context?.timeoutMs});
      return {
        available: observed.available === true,
        ...(typeof observed.matched === 'boolean' ? {matched: observed.matched} : {}),
        // No unverified runtime setters are advertised by this status adapter.
        capabilities: [],
        ...(observed.message ? {message: observed.message.slice(0, 160)} : {})
      };
    }
  };
}

module.exports = {createReceiverProbes, dependenciesFromCache, serviceFromProperties};
