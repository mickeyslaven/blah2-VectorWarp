'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const yaml = require('js-yaml');
const {getDeviceProfiles, validateConfig} =
  require('../../api/config-manager.js');
const {applyDeviceProfile, metadata, normalizeKrakenChannels, upstreamRestartError, accelerationSummary, receiverSaveMessage, receiverSaveDetails, CONFIG_SAVE_TIMEOUT_MS} =
  require('../../html/js/config_ui.js');
const {RECEIVER_SYNC_BUDGET} = require('../../api/receiver-sync.js');

assert.ok(CONFIG_SAVE_TIMEOUT_MS >= RECEIVER_SYNC_BUDGET.maxTransactionTimeoutMs + 15000,
  'Browser save must allow the entire receiver transaction plus response overhead');

const base = yaml.load(fs.readFileSync(
  path.join(__dirname, '..', '..', 'config', 'config-kraken.yml'), 'utf8'));
const clone = value => JSON.parse(JSON.stringify(value));
assert.equal(accelerationSummary({active: 'vulkan', device: 'RTX 4050'}, 'receiving'), 'GPU: RTX 4050');
assert.equal(accelerationSummary({active: 'vulkan', device: 'RTX 4050'}, 'stale'), 'Waiting for radar');
assert.equal(accelerationSummary({active: 'cpu', reason: 'Driver missing'}, 'receiving'), 'CPU — Driver missing');
assert.equal(accelerationSummary(null, 'receiving'), 'Not reported');
assert.equal(metadata(['truth'])[0], 'ADS-B');
assert.equal(metadata(['truth', 'adsb'])[0], 'ADS-B');
assert.equal(metadata(['truth', 'adsb', 'enabled'])[0], 'Show ADS-B');
assert.equal(metadata(['capture', 'device', 'serial'], 'RspDuo')[0], 'RSPduo serial');
assert.equal(metadata(['capture', 'device', 'serial'], 'HackRF')[0], 'HackRF serial numbers');
const captureSource = fs.readFileSync(path.join(
  __dirname, '..', '..', 'src', 'capture', 'Capture.cpp'), 'utf8');
const validTypes = captureSource.match(
  /Capture::VALID_TYPE\[[^\]]+\]\s*=\s*\{([^}]+)\}/)?.[1]
  .match(/"([^"]+)"/g).map(value => value.slice(1, -1));
assert.deepEqual(getDeviceProfiles().map(profile => profile.type).sort(),
  validTypes.sort(), 'Settings device profiles must match Capture::VALID_TYPE');

for (const profile of getDeviceProfiles()) {
  const manual = clone(base);
  manual.process.performance = {...manual.process.performance, acceleration: 'cpu'};
  assert.equal(applyDeviceProfile(manual, profile).process.performance.acceleration, 'cpu');
  const changed = applyDeviceProfile(clone(base), profile);
  if (profile.type === 'HackRF') changed.capture.device.serial = ['0001', '0002'];
  assert.equal(changed.capture.device.type, profile.type);
  assert.equal(changed.capture.fs, profile.sampleRate);
  assert.equal(validateConfig(changed, base).valid, true, profile.type);
}

const resized = clone(base);
resized.capture.device.channel_count = 6;
normalizeKrakenChannels(resized, 5);
assert.deepEqual(resized.capture.device.surveillance_channels,
  [0, 1, 2, 3, 4, 5]);
assert.deepEqual(resized.process.reference_synthesis.channels,
  [0, 1, 2, 3, 4, 5]);
resized.process.reference_synthesis.mode = 'dedicated';
resized.capture.device.reference_channel = 2;
normalizeKrakenChannels(resized);
assert.deepEqual(resized.capture.device.surveillance_channels,
  [0, 1, 3, 4, 5]);
assert.deepEqual(resized.process.reference_synthesis.channels, [2]);
assert.equal(validateConfig(resized).valid, true);
const customChannels = clone(base);
customChannels.capture.device.surveillance_channels = [1, 3];
customChannels.process.reference_synthesis.channels = [0, 2];
customChannels.capture.device.channel_count = 6;
normalizeKrakenChannels(customChannels, 5);
assert.deepEqual(customChannels.capture.device.surveillance_channels, [1, 3]);
assert.deepEqual(customChannels.process.reference_synthesis.channels, [0, 2]);

function inspectDescriptions(value, currentPath = []) {
  if (currentPath.join('.') === 'truth.adsb.adsb2dd') return; // migration-only external converter key
  if (value && typeof value === 'object' && !Array.isArray(value)) {
    for (const [key, child] of Object.entries(value))
      inspectDescriptions(child, [...currentPath, key]);
    return;
  }
  const [label, description] = metadata(currentPath);
  assert.ok(label && description, currentPath.join('.'));
  assert.ok(!/ setting$/i.test(description),
    `Generic description remains for ${currentPath.join('.')}`);
  assert.ok(description.length <= 90,
    `Description is too long for ${currentPath.join('.')}`);
}

for (const name of ['config.yml', 'config-kraken.yml',
  'config-usrp.yml', 'config-hackrf.yml']) {
  inspectDescriptions(yaml.load(fs.readFileSync(
    path.join(__dirname, '..', '..', 'config', name), 'utf8')));
}

assert.equal(upstreamRestartError({available: false}), null);
assert.equal(upstreamRestartError({available: true, issues: []}), null);
for (const [field, actual, expected, unit] of [['capture.fc', 527000000, 528000000, 'MHz'],
  ['capture.fs', 2400000, 2000000, 'MS/s'], ['capture.device.channel_count', 5, 6, '5']]) {
  const message = upstreamRestartError({available: true, issues: [{field, actual, expected, severity: 'error'}]});
  assert.ok(message.includes(unit) && message.includes('Receiver settings') && !message.includes(field));
}
const synchronizedMessage = receiverSaveMessage({receiverSync: {receiverType: 'Kraken', status: 'synchronized'}}, false);
assert.match(synchronizedMessage, /Suite V2 settings confirmed/);
assert.match(synchronizedMessage, /vectorwarp restart/);
assert.doesNotMatch(synchronizedMessage, /Radar restarted/);
const synchronizedDetails = receiverSaveDetails({receiverSync: {receiverType: 'Kraken', status: 'synchronized'}});
assert.match(synchronizedDetails, /accepted the command.*reported matching settings/);
assert.match(synchronizedDetails, /serial order.*calibration.*not independently checked/);
for (const receiverType of ['Kraken', 'Usrp', 'RspDuo', 'HackRF']) {
  const result = {receiverSync: {receiverType, status: 'not-required'}};
  const message = receiverSaveMessage(result, true);
  assert.equal(message, 'Settings saved. Radar restarted; new frames received.');
  assert.ok(message.length < 100);
  assert.ok(receiverSaveDetails(result).length > 0, 'Technical limits remain available in Details');
}
const replay = {receiverSync: {receiverType: 'Usrp', acceptance: {mode: 'replay'}}};
assert.equal(receiverSaveMessage(replay, true), 'Settings saved. Replay restarted; new frames received.');
assert.equal(receiverSaveDetails(replay), 'No receiver hardware was opened.');
console.log('Settings device-switch, descriptions and actionable restart-mismatch tests passed.');
