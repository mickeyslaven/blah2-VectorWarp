'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const {getDeviceProfiles, validateConfig, FIELD_RULES} =
  require('../../api/config-manager.js');
const {applyDeviceProfile, metadata: fieldMetadata, normalizeKrakenChannels, receiverFeatureCapabilities,
  receiverSpecificProcessPath, adsbExchangeId, adsbPublicSource, upstreamRestartError,
  accelerationSummary, receiverSaveMessage, receiverSaveDetails, CONFIG_SAVE_TIMEOUT_MS,
  MEASURED_GAIN_SAVE_TIMEOUT_MS} =
  require('../../html/js/config_ui.js');
const metadata = (path, receiver) => fieldMetadata(path, receiver, FIELD_RULES);
const {RECEIVER_SYNC_BUDGET} = require('../../api/receiver-sync.js');
const {MAX_AUTOMATIC_GAIN_MS} = require('../../api/automatic-gain.js');
const configProfiles = require('../../html/js/config_profiles.js');

assert.ok(CONFIG_SAVE_TIMEOUT_MS >= RECEIVER_SYNC_BUDGET.maxTransactionTimeoutMs + 15000,
  'Browser save must allow the entire receiver transaction plus response overhead');
assert.ok(MEASURED_GAIN_SAVE_TIMEOUT_MS >= MAX_AUTOMATIC_GAIN_MS + 15000,
  'Measured gain save must allow the complete search and rollback budget');

const base = require('../../api/config-store').configurationForReceiver('Kraken');
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
const configUiSource = fs.readFileSync(path.join(
  __dirname, '..', '..', 'html', 'js', 'config_ui.js'), 'utf8');
const groupCopy = require('vm').runInNewContext('(' +
  configUiSource.match(/const CONFIG_GROUP_META = (\{[\s\S]*?\n\});/)[1] + ')');
assert.equal(Object.keys(groupCopy).filter(key => FIELD_RULES[key]).length, 0,
  'Persisted field copy belongs to the canonical contract, not a parallel UI table');
for (const [key, rule] of Object.entries(FIELD_RULES))
  if (rule.presentation && key !== 'capture.fs')
    assert.deepEqual(metadata(key.split('.')), rule.presentation, key);
assert.doesNotMatch(configUiSource, /NUMERIC_PRESENTATION|let settingsMode|simpleSettingHidden/,
  'Unit descriptors and the retired second settings mode cannot regain parallel ownership');
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
  assert.deepEqual(changed.process, base.process, 'Receiver selection preserves radar intent');
  assert.equal(validateConfig(changed, base).valid, profile.type === 'Kraken',
    'Incompatible existing array input roles must remain visible for repair: ' + profile.type);
  const features = receiverFeatureCapabilities(profile.type, getDeviceProfiles());
  assert.equal(Boolean(features.coherentArray), profile.type === 'Kraken');
  if (['Usrp', 'RspDuo', 'HackRF'].includes(profile.type))
    assert.equal(features.residentIntegration, true,
      `${profile.type} must not inherit the Kraken-only beamforming gate for two-CPI integration`);
  for (const mode of ['two', 'two_motion']) {
    const recovery = require('../../api/config-store').configurationForReceiver(profile.type);
    if (profile.type === 'HackRF') recovery.capture.device.serial = ['0001', '0002'];
    configProfiles.applyWeakTargetMode(recovery, mode);
    const result = validateConfig(recovery, base);
    assert.equal(result.valid, true,
      `${profile.type}/${mode}: ${result.errors.join('; ')}`);
    assert.equal(recovery.process.reference_synthesis?.mode ?? 'dedicated',
      profile.type === 'Kraken' ? recovery.process.reference_synthesis.mode : 'dedicated',
      `${profile.type}/${mode} changed receiver topology`);
  }
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
krakenOnlyEnabled.process.bearing.manifold_recorder.enabled = true;
const rspProfile = getDeviceProfiles().find(profile => profile.type === 'RspDuo');
applyDeviceProfile(krakenOnlyEnabled, rspProfile);
assert.equal(krakenOnlyEnabled.capture.aircraft_clips.enabled, true,
  'Automatic aircraft clips remain enabled when changing receivers');
assert.equal(krakenOnlyEnabled.process.detection.integration_cpis, 4,
  'Paired receiver keeps resident temporal integration capability');
assert.equal(krakenOnlyEnabled.process.detection.track_before_detect.enable, true);
assert.equal(krakenOnlyEnabled.process.bearing.enable, true);
assert.equal(krakenOnlyEnabled.process.bearing.manifold_recorder.enabled, true);
assert.equal(validateConfig(krakenOnlyEnabled).valid, false, 'Receiver switch refuses incompatible intent');

for (const profile of getDeviceProfiles()) {
  const replay = clone(base);
  replay.capture.replay = {...replay.capture.replay,state:true,channel_count:5,
    reference_mode:'array_eigenbeam',reference_channel:0,
    reference_channels:[0,1,2,3,4],surveillance_channels:[0,1,2,3,4]};
  replay.capture.device.array_geometry = {mapping_confirmed:true,geometry_confirmed:true,
    orientation_confirmed:true,elements:[]};
  const processBefore = clone(replay.process), recordingBefore = clone(replay.capture.replay);
  const geometryBefore = clone(replay.capture.device.array_geometry), rate = replay.capture.fs;
  const previous = clone(replay);
  applyDeviceProfile(replay,{...profile,features:{coherentArray:false,residentIntegration:false}});
  require('../../html/js/kraken_geometry').reconcile(replay,previous);
  assert.deepEqual(replay.process,processBefore,'Dormant live receiver switch preserves recorded processing');
  assert.deepEqual(replay.capture.replay,recordingBefore);
  assert.deepEqual(replay.capture.device.array_geometry,geometryBefore,'Recorded geometry confirmations survive');
  assert.equal(replay.capture.fs,rate);
}

const resized = clone(base);
resized.capture.device.channel_count = 6;
normalizeKrakenChannels(resized, 5);
assert.deepEqual(resized.capture.device.surveillance_channels,
  [0, 1, 2, 3, 4]);
assert.deepEqual(resized.process.reference_synthesis.channels,
  [0, 1, 2, 3, 4]);
resized.process.reference_synthesis.mode = 'dedicated';
resized.capture.device.reference_channel = 2;
normalizeKrakenChannels(resized);
assert.deepEqual(resized.capture.device.surveillance_channels,
  [0, 1, 2, 3, 4]);
assert.deepEqual(resized.process.reference_synthesis.channels, [0, 1, 2, 3, 4]);
assert.equal(validateConfig(resized).valid, false, 'An invalid reference edit must not synthesize a new input assignment');
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

inspectDescriptions(require('../../api/config-store').setupDefaults());
for (const profile of getDeviceProfiles())
  inspectDescriptions(require('../../api/config-store').configurationForReceiver(profile.type));

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
