'use strict';

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const yaml = require('js-yaml');
const {getDeviceProfiles, validateConfig, writeConfigAtomically} = require('./config-manager');
const FIELD_RULES = require('./config-rules');
const {reconcile, recordErrors} = require('../html/js/kraken_geometry');

// Neutral first-run form, not a running radar configuration or site claim.
function setupDefaults() {
  return {
    capture: {fs: 2000000, fc: 204640000,
      device: getDeviceProfiles().find(item => item.type === 'RspDuo').device,
      replay: {state: false, loop: false, file: '/var/lib/vectorwarp/recordings/recording.blah2iq', format: 'auto'}},
    process: {
      performance: {surveillance_workers: 0, fft_threads: 0, acceleration: 'auto'},
      data: {cpi: 0.5, buffer: 2, overlap: 0},
      ambiguity: {delayMin: -10, delayMax: 400, dopplerMin: -200, dopplerMax: 200},
      clutter: {enable: true, delayMin: -10, delayMax: 400},
      detection: {enable: true, pfa: 0.00001, nGuard: 2, nTrain: 6,
        minDelay: 5, minDoppler: 15, nCentroid: 6},
      tracker: {enable: true, initiate: {M: 3, N: 5, maxAcc: 10}, delete: 10, smooth: 'none'}
    },
    network: {ip: '0.0.0.0', ports: {api: 3000, map: 3001, detection: 3002,
      track: 3003, timestamp: 4000, timing: 4001, iqdata: 4002, config: 4003}},
    truth: {adsb: {enabled: false, tar1090: 'localhost:8080', poll_interval: 1, smoothing_window: 10, max_position_age: 30},
      ais: {enabled: false, ip: '0.0.0.0', port: 30001}},
    location: {rx: {latitude: 0, longitude: 0, altitude: 0, name: 'Set receiver site'},
      tx: {latitude: 0, longitude: 0, altitude: 0, name: 'Set transmitter site'}},
    save: {iq: false, map: false, detection: false, timing: false, path: '/var/lib/vectorwarp/recordings/'}
  };
}

function readConfig(filename) {
  let raw = '';
  let exists = false;
  let parsed;
  let readError = null;
  try {
    if (fs.statSync(filename).size > 262144) throw new Error('Configuration exceeds 256 KiB.');
    raw = fs.readFileSync(filename, 'utf8');
    exists = true;
    parsed = yaml.load(raw, {schema: yaml.JSON_SCHEMA});
    if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed))
      throw new Error('The YAML root must contain configuration sections.');
    if (parsed.truth?.adsb && Object.prototype.hasOwnProperty.call(parsed.truth.adsb, 'adsb2dd'))
      delete parsed.truth.adsb.adsb2dd;
    // Detect cycles/aliases before recursive form rendering and validation.
    let nodes = 0;
    const visit = (value, parents = new Set(), depth = 0) => {
      if (++nodes > 4096) throw new Error('Too many configuration values.');
      if (!value || typeof value !== 'object') return;
      if (parents.has(value) || depth > 16) throw new Error('Recursive or deeply nested YAML is not supported.');
      const next = new Set(parents).add(value);
      Object.values(value).forEach(child => visit(child, next, depth + 1));
    };
    visit(parsed);
  } catch (error) {
    readError = error.code === 'ENOENT' ? 'No configuration file exists yet.' :
      `Configuration could not be read: ${error.message}`;
    parsed = null;
  }
  const defaults = setupDefaults();
  const profile = getDeviceProfiles().find(item => item.type === parsed?.capture?.device?.type);
  if (profile) {
    defaults.capture.device = profile.device;
    defaults.capture.fs = profile.sampleRate;
    if (profile.process) {
      Object.assign(defaults.process, profile.process);
      defaults.process.performance.acceleration = 'auto';
      // Missing optional fields must use processor defaults, not the more
      // aggressive new-device preset (which opts into array synthesis).
      defaults.process.performance.surveillance_workers = 1;
      defaults.process.performance.fft_threads = profile.type === 'Kraken' ? 1 : 4;
    }
    if (profile.type === 'Kraken') {
      const count = Number.isInteger(parsed.capture.device.channel_count) ?
        Math.max(2, Math.min(8, parsed.capture.device.channel_count)) : 5;
      const reference = parsed.capture.device.reference_channel ?? 0;
      const mode = parsed.process?.reference_synthesis?.mode ?? 'dedicated';
      const channels = Array.from({length: count}, (_, i) => i);
      defaults.process.reference_synthesis.mode = 'dedicated';
      defaults.process.reference_synthesis.channels = mode === 'array_eigenbeam' ? channels : [reference];
      defaults.capture.device.surveillance_channels = channels.filter(channel =>
        mode === 'array_eigenbeam' || channel !== reference);
    }
  }
  const suppliedDefaults = [];
  const acceptsUnionType = (base, current, prefix) => {
    const type = FIELD_RULES[prefix]?.type;
    // The form has two deliberately polymorphic settings.  Their profile
    // defaults select only one member of the valid union, so comparing to the
    // default's JavaScript type would discard a valid persisted alternative.
    if (type === 'gain') return typeof current === 'string' || typeof current === 'number';
    if (type === 'serial') return typeof current === 'string' || Array.isArray(current);
    return false;
  };
  const merge = (base, current, prefix = '') => {
    if (current === undefined) {
      if (prefix !== 'process.performance' && !prefix.startsWith('process.performance.'))
        suppliedDefaults.push(prefix);
      return base;
    }
    if (((Array.isArray(base) && !Array.isArray(current)) ||
        (base !== null && typeof base !== 'object' && typeof current !== typeof base)) &&
        !acceptsUnionType(base, current, prefix)) {
      suppliedDefaults.push(prefix);
      return base;
    }
    if (base && typeof base === 'object' && !Array.isArray(base) &&
        (!current || typeof current !== 'object' || Array.isArray(current))) {
      suppliedDefaults.push(prefix);
      return base;
    }
    if (!base || typeof base !== 'object' || Array.isArray(base) ||
        !current || typeof current !== 'object' || Array.isArray(current)) return current;
    const result = {...current};
    for (const [key, value] of Object.entries(base))
      result[key] = merge(value, current[key], prefix ? `${prefix}.${key}` : key);
    return result;
  };
  const config = parsed ? merge(defaults, parsed) : defaults;
  const validation = validateConfig(config);
  return {config, revision: crypto.createHash('sha256').update(exists ? raw : 'missing').digest('hex'),
    exists, readError, suppliedDefaults, validation,
    setupRequired: Boolean(readError || suppliedDefaults.length || !validation.valid)};
}

function writable(filename) {
  try {
    // Reject symlinks: atomic rename must not replace a symlink unexpectedly.
    if (fs.existsSync(filename)) {
      const file = fs.lstatSync(filename);
      // Root can pass access(W_OK) for chmod 0444. Respect an explicitly
      // read-only config even when the API process runs as root.
      if (!file.isFile() || !(file.mode & 0o222)) return false;
      fs.accessSync(filename, fs.constants.R_OK | fs.constants.W_OK);
    }
    if (!(fs.statSync(path.dirname(filename)).mode & 0o222)) return false;
    fs.accessSync(path.dirname(filename), fs.constants.W_OK);
    return true;
  } catch (_) { return false; }
}

function saveConfig(filename, candidate, expectedRevision) {
  const before = readConfig(filename);
  if (expectedRevision !== before.revision) {
    const error = new Error('The config file changed. Reload settings before saving; your edits have not been written.');
    error.status = 409;
    throw error;
  }
  const metadataErrors = recordErrors(candidate?.capture?.device?.array_geometry);
  if (metadataErrors.length) {
    const error = new Error(metadataErrors.join(' '));
    error.status = 422;
    throw error;
  }
  const prepared = reconcile(JSON.parse(JSON.stringify(candidate)), before.config);
  writeConfigAtomically(filename, prepared);
  return readConfig(filename);
}

module.exports = {readConfig, writable, saveConfig, setupDefaults};
