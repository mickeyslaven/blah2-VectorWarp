'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const {createRequire} = require('module');
// Resolve UI-test dependencies from the API package that declares and locks
// them, independent of the caller's cwd or NODE_PATH.
const yaml = createRequire(path.join(__dirname, '..', '..', 'api', 'package.json'))('js-yaml');
const {getDeviceProfiles, validateConfig} =
  require('../../api/config-manager.js');
const {applyDeviceProfile, metadata, normalizeKrakenChannels, receiverFeatureCapabilities,
  receiverSpecificProcessPath, adsbExchangeId, adsbPublicSource, upstreamRestartError,
  accelerationSummary, receiverSaveMessage, receiverSaveDetails, CONFIG_SAVE_TIMEOUT_MS,
  MEASURED_GAIN_SAVE_TIMEOUT_MS} =
  require('../../html/js/config_ui.js');
const {RECEIVER_SYNC_BUDGET} = require('../../api/receiver-sync.js');
const {MAX_AUTOMATIC_GAIN_MS} = require('../../api/automatic-gain.js');

assert.ok(CONFIG_SAVE_TIMEOUT_MS >= RECEIVER_SYNC_BUDGET.maxTransactionTimeoutMs + 15000,
  'Browser save must allow the entire receiver transaction plus response overhead');
assert.ok(MEASURED_GAIN_SAVE_TIMEOUT_MS >= MAX_AUTOMATIC_GAIN_MS + 15000,
  'Measured gain save must allow the complete search and rollback budget');

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
assert.equal(adsbExchangeId('exchange:https://opendata.adsb.fi/api/v3/lat/1/lon/2/dist/60'), 'adsb-fi');
assert.equal(adsbExchangeId('exchange:https://api.adsb.lol/v2/point/1/2/60'), 'adsb-lol');
assert.equal(adsbExchangeId('http://receiver.local/tar1090'), null);
assert.equal(adsbPublicSource('adsb-fi', {location: {rx: {latitude: 40.5, longitude: -80.1}}}),
  'exchange:https://opendata.adsb.fi/api/v3/lat/40.5/lon/-80.1/dist/60');
assert.equal(adsbPublicSource('adsb-lol', {location: {rx: {latitude: 40.5, longitude: -80.1}}}),
  'exchange:https://api.adsb.lol/v2/point/40.5/-80.1/60');
assert.equal(metadata(['capture', 'device', 'serial'], 'RspDuo')[0], 'RSPduo serial');
assert.equal(metadata(['capture', 'device', 'serial'], 'HackRF')[0], 'HackRF serial numbers');
assert.match(metadata(['capture', 'fs'], 'Kraken')[1], /2\.4 MS\/s/);
assert.match(metadata(['capture', 'fs'], 'RspDuo')[1], /62\.5 kS\/s to 2 MS\/s/);
assert.doesNotMatch(metadata(['capture', 'fs'], 'Usrp')[1], /Kraken/);
assert.equal(metadata(['capture', 'aircraft_clips', 'enabled'])[0],
  'Record eligible aircraft automatically');
assert.equal(metadata(['process', 'clutter', 'followAmbiguity'])[0], 'Follow radar delay range');
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
  const features = receiverFeatureCapabilities(profile.type, getDeviceProfiles());
  assert.equal(Boolean(features.coherentArray), profile.type === 'Kraken');
}
assert.equal(receiverSpecificProcessPath('process.surveillance_fusion.mode'), true);
assert.equal(receiverSpecificProcessPath('process.bearing.manifold_recorder.enabled'), true);
assert.equal(receiverSpecificProcessPath('process.detection.integration_cpis'), true);
assert.equal(receiverSpecificProcessPath('process.detection.pfa'), false);
assert.equal(receiverSpecificProcessPath('process.ambiguity.delayMax'), false);
const krakenOnlyEnabled = clone(base);
krakenOnlyEnabled.capture.aircraft_clips.enabled = true;
krakenOnlyEnabled.process.detection.integration_cpis = 4;
krakenOnlyEnabled.process.detection.track_before_detect.enable = true;
krakenOnlyEnabled.process.bearing.enable = true;
krakenOnlyEnabled.process.bearing.frequency_resolved_shadow.enabled = true;
krakenOnlyEnabled.process.bearing.manifold_recorder.enabled = true;
krakenOnlyEnabled.process.tracker.track_guided_shadow.enable = true;
const rspProfile = getDeviceProfiles().find(profile => profile.type === 'RspDuo');
applyDeviceProfile(krakenOnlyEnabled, rspProfile);
assert.equal(krakenOnlyEnabled.capture.aircraft_clips.enabled, true,
  'Automatic aircraft clips remain enabled when changing receivers');
assert.equal(krakenOnlyEnabled.process.detection.integration_cpis, 1);
assert.equal(krakenOnlyEnabled.process.detection.track_before_detect.enable, false);
assert.equal(krakenOnlyEnabled.process.bearing.enable, false);
assert.equal(krakenOnlyEnabled.process.bearing.frequency_resolved_shadow.enabled, false);
assert.equal(krakenOnlyEnabled.process.bearing.manifold_recorder.enabled, false);
assert.equal(krakenOnlyEnabled.process.tracker.track_guided_shadow.enable, false);

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
assert.match(upstreamRestartError({available: true, issues: [{label: 'Calibration',
  severity: 'error', actual: 'FAILED'}]}), /calibration failed.*restart to retry/);
assert.equal(upstreamRestartError({available: true, issues: [{label: 'Calibration',
  severity: 'warning', actual: 'PENDING'}]}), null);
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
