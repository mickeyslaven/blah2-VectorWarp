const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const yaml = require('js-yaml');
const {getDeviceProfiles, validateConfig,
  writeConfigAtomically} = require('./config-manager.js');

function load(name) {
  const value = yaml.load(fs.readFileSync(path.join(__dirname, '..', 'config', name), 'utf8'));
  if (value.capture.device.type === 'HackRF') value.capture.device.serial = ['0001', '0002'];
  return value;
}

for (const name of ['config.yml', 'config-hackrf.yml', 'config-usrp.yml',
  'config-kraken.yml']) {
  const result = validateConfig(load(name));
  assert.equal(result.valid, true, `${name}: ${result.errors.join('; ')}`);
}

const sixChannel = load('config-kraken.yml');
sixChannel.capture.device.channel_count = 6;
sixChannel.capture.device.surveillance_channels = [0, 1, 2, 3, 4, 5];
sixChannel.process.reference_synthesis.channels = [0, 1, 2, 3, 4, 5];
// Shipped examples intentionally use neutral sites. This fixture exercises the
// ordinary no-advice path, so make its sites explicitly distinct.
sixChannel.location.rx = {name: 'Test receiver', latitude: 40, longitude: -75, altitude: 100};
sixChannel.location.tx = {name: 'Test transmitter', latitude: 41, longitude: -74, altitude: 120};
assert.equal(validateConfig(sixChannel).valid, true);

const clone = value => JSON.parse(JSON.stringify(value));
const automaticClutter = clone(sixChannel);
automaticClutter.process.ambiguity.delayMin = -8;
automaticClutter.process.ambiguity.delayMax = 47;
automaticClutter.process.clutter.followAmbiguity = true;
automaticClutter.process.clutter.delayMin = -2; // Ignored while following.
automaticClutter.process.clutter.delayMax = 12;
assert.equal(validateConfig(automaticClutter).valid, true,
  'followAmbiguity covers a changed ambiguity range without manual clutter tuning');
const legacyManualClutter = clone(automaticClutter);
delete legacyManualClutter.process.clutter.followAmbiguity;
assert.equal(validateConfig(legacyManualClutter).valid, true,
  'existing numeric clutter ranges remain valid without the follow flag');
const incompleteManualClutter = clone(automaticClutter);
incompleteManualClutter.process.clutter.followAmbiguity = false;
delete incompleteManualClutter.process.clutter.delayMax;
assert.equal(validateConfig(incompleteManualClutter).valid, false,
  'manual clutter range requires both half-open endpoints');
const coherentFusion = load('config-kraken.yml');
coherentFusion.process.surveillance_fusion = {mode: 'coherent_beam_bank', beams: 12};
coherentFusion.capture.device.array_geometry = {
  shape: 'regular_polygon', units: 'm', coordinate_frame: 'array_xy_right_handed',
  mapping_confirmed: false, geometry_confirmed: false, orientation_confirmed: false,
  x_axis_bearing_deg_true: 0,
  bearing_channels: [...coherentFusion.capture.device.surveillance_channels], elements:
  coherentFusion.capture.device.surveillance_channels.map((daq_channel, index) => ({
    id: `channel-${daq_channel}`, daq_channel, receiver_serial: String(1000 + daq_channel),
    position: [Math.cos(2 * Math.PI * index / 5) * .193548,
      Math.sin(2 * Math.PI * index / 5) * .193548, 0]
  }))};
assert.equal(validateConfig(coherentFusion).valid, true,
  'coherent beam-bank fusion accepts complete Kraken geometry');
const whitenedFusion = clone(coherentFusion);
whitenedFusion.process.performance.acceleration = 'gpu-resident';
whitenedFusion.process.tracker.gpu_capacity = 1024;
whitenedFusion.process.surveillance_fusion.mode = 'noise_whitened_beam_bank';
assert.equal(validateConfig(whitenedFusion).valid, true,
  'noise-whitened beam-bank fusion accepts resident Kraken geometry');
const nonresidentWhitenedFusion = clone(whitenedFusion);
nonresidentWhitenedFusion.process.performance.acceleration = 'auto';
assert.equal(validateConfig(nonresidentWhitenedFusion).valid, false,
  'noise-whitened beam-bank fusion requires resident acceleration');
const adaptiveTracker = clone(whitenedFusion);
adaptiveTracker.process.tracker.initiate.acceleration_model = 'adaptive_gate';
assert.equal(validateConfig(adaptiveTracker).valid, true,
  'adaptive tracker initiation accepts resident acceleration');
const nonresidentAdaptiveTracker = clone(adaptiveTracker);
nonresidentAdaptiveTracker.process.performance.acceleration = 'auto';
assert.equal(validateConfig(nonresidentAdaptiveTracker).valid, false,
  'adaptive tracker initiation requires resident acceleration');
const stationaryClutterTracker = clone(adaptiveTracker);
stationaryClutterTracker.process.tracker.initiate.stationary_clutter = {
  enable: true, persistence_cpis: 8, max_doppler_hz: 15};
assert.equal(validateConfig(stationaryClutterTracker).valid, true,
  'persistent clutter initiation accepts bounded resident settings');
const fullDopplerClutterTracker = clone(stationaryClutterTracker);
delete fullDopplerClutterTracker.process.tracker.initiate.stationary_clutter.max_doppler_hz;
assert.equal(validateConfig(fullDopplerClutterTracker).valid, true,
  'persistent clutter initiation defaults to the configured Doppler span');
const instantStationaryClutter = clone(stationaryClutterTracker);
instantStationaryClutter.process.tracker.initiate.stationary_clutter.persistence_cpis = 1;
assert.equal(validateConfig(instantStationaryClutter).valid, false,
  'persistent clutter initiation requires temporal evidence');
const nonresidentStationaryClutter = clone(stationaryClutterTracker);
nonresidentStationaryClutter.process.performance.acceleration = 'auto';
assert.equal(validateConfig(nonresidentStationaryClutter).valid, false,
  'persistent clutter initiation requires resident acceleration');
const guidedShadowTracker = clone(stationaryClutterTracker);
guidedShadowTracker.process.detection.track_before_detect = {
  enable: true, max_cpis: 8, motion_bins: 1};
guidedShadowTracker.process.tracker.track_guided_shadow = {enable: true, max_tracks: 64};
assert.equal(validateConfig(guidedShadowTracker).valid, true,
  'bounded track-guided shadow accepts resident rolling history');
const promotedGuidedTracker = clone(guidedShadowTracker);
promotedGuidedTracker.process.tracker.track_guided_shadow.promote = true;
assert.equal(validateConfig(promotedGuidedTracker).valid, true,
  'track-guided promotion accepts the qualified evaluator path');
const promotionWithoutShadow = clone(guidedShadowTracker);
promotionWithoutShadow.process.tracker.track_guided_shadow = {enable: false,
  promote: true, max_tracks: 64};
assert.equal(validateConfig(promotionWithoutShadow).valid, false,
  'track-guided promotion cannot bypass the evaluator');
const unboundedGuidedShadow = clone(guidedShadowTracker);
unboundedGuidedShadow.process.tracker.track_guided_shadow.max_tracks = 1025;
assert.equal(validateConfig(unboundedGuidedShadow).valid, false,
  'track-guided shadow cannot exceed tracker capacity');
const noHistoryGuidedShadow = clone(guidedShadowTracker);
noHistoryGuidedShadow.process.detection.track_before_detect.enable = false;
assert.equal(validateConfig(noHistoryGuidedShadow).valid, false,
  'track-guided shadow requires rolling track-before-detect history');
const nonresidentGuidedShadow = clone(guidedShadowTracker);
nonresidentGuidedShadow.process.performance.acceleration = 'auto';
assert.equal(validateConfig(nonresidentGuidedShadow).valid, false,
  'track-guided shadow requires resident GPU execution');
const mismatchedCalibration = clone(coherentFusion);
mismatchedCalibration.process.reference_synthesis.channels = [0, 1, 2, 3];
assert.equal(validateConfig(mismatchedCalibration).valid, false,
  'coherent fusion rejects reference/surveillance channel mismatch');
const legacyCalibratedName = clone(coherentFusion);
legacyCalibratedName.process.surveillance_fusion.mode = 'calibrated_coherent_beam_bank';
assert.equal(validateConfig(legacyCalibratedName).valid, false,
  'removed calibrated mode name cannot select a second coherent path');
const missingFusionElement = clone(coherentFusion);
missingFusionElement.capture.device.array_geometry.elements.pop();
assert.equal(validateConfig(missingFusionElement).valid, false,
  'coherent beam-bank fusion requires every surveillance element');
const projectedBearing = clone(coherentFusion);
Object.assign(projectedBearing.capture.replay, {state: true, format: 'blah2',
  file: '/tmp/five-channel-replay.blah2'});
projectedBearing.capture.device.channel_count = 5;
projectedBearing.capture.device.surveillance_channels = [0, 1, 2, 3, 4];
projectedBearing.process.reference_synthesis = {mode: 'array_eigenbeam',
  channels: [0, 1, 2, 3, 4], analysis_samples: 4096, analysis_interval: 1,
  power_iterations: 12, covariance_smoothing: 0, diagonal_loading: .01};
projectedBearing.process.clutter.enable = true;
projectedBearing.process.performance.acceleration = 'gpu-resident';
projectedBearing.process.tracker.gpu_capacity = 4096;
Object.assign(projectedBearing.process.surveillance_fusion,
  {mode: 'coherent_beam_bank', direct_path_null_db: 0});
Object.assign(projectedBearing.process.bearing, {enable: true,
  method: 'covariance_mvdr', reference_anchor: 'fixed_site_projected'});
delete projectedBearing.process.bearing.manifold_file;
Object.assign(projectedBearing.capture.device.array_geometry,
  {mapping_confirmed: true, geometry_confirmed: true, orientation_confirmed: true});
const projectedBearingValidation = validateConfig(projectedBearing);
assert.equal(projectedBearingValidation.valid, true,
  `projected fixed-site model accepts only the explicit native replay experiment: ${projectedBearingValidation.errors.join('; ')}`);
for (const mutate of [
  value => { value.capture.replay.state = false; },
  value => { value.capture.replay.format = 'auto'; },
  value => { value.capture.device.surveillance_channels = [0, 2, 1, 3, 4]; },
  value => { value.process.reference_synthesis.mode = 'channel'; },
  value => { value.process.clutter.enable = false; },
  value => { value.process.performance.acceleration = 'cpu'; },
  value => { value.process.bearing.method = 'coherent_beam_bank'; },
  value => { value.process.bearing.manifold_file = '/tmp/measured.json'; },
  value => { value.process.surveillance_fusion.direct_path_null_db = 20; }
]) {
  const invalid = clone(projectedBearing);
  mutate(invalid);
  assert.equal(validateConfig(invalid).valid, false,
    'projected fixed-site model must fail closed outside its replay contract');
}
for (const beams of [3, 361, 12.5]) {
  const invalid = clone(coherentFusion);
  invalid.process.surveillance_fusion.beams = beams;
  assert.equal(validateConfig(invalid).valid, false, `reject coherent beam count ${beams}`);
}
// Sample-rate limits belong to the receiver, not to the shared DSP pipeline.
// USRP and dual HackRF can request 6 MS/s; Kraken and coherent RSPduo cannot.
for (const [name, accepted] of [['config-usrp.yml', true],
  ['config-hackrf.yml', true], ['config-kraken.yml', false], ['config.yml', false]]) {
  const value = load(name);
  value.capture.fs = 6000000;
  value.process.data.cpi = .2;
  value.process.ambiguity = {delayMin: -10, delayMax: 245,
    dopplerMin: -2400, dopplerMax: 2400};
  const result = validateConfig(value);
  assert.equal(result.valid, accepted, `${name} at 6 MS/s: ${result.errors.join('; ')}`);
  if (!accepted) assert.ok(result.errors.some(error => error.includes('capture.fs')));
}
for (const source of ['auto', 'local:readsb', 'local:dump1090-fa',
  'local:dump1090', 'local:dump1090-mutability', '192.0.2.10:8080',
  'https://adsb.example/tar1090', '[2001:db8::1]:8080/tar1090']) {
  const value = clone(sixChannel);
  value.truth.adsb.tar1090 = source;
  assert.equal(validateConfig(value).valid, true, `ADS-B source: ${source}`);
  assert.equal(value.truth.adsb.tar1090, source, 'Validation must not replace the selected source');
}
for (const source of ['local:/etc/passwd', 'local:unknown', 'auto ',
  'file:///run/readsb/aircraft.json', 'https://user:pass@adsb.example']) {
  const value = clone(sixChannel);
  value.truth.adsb.tar1090 = source;
  assert.equal(validateConfig(value).valid, false, `Reject ADS-B source: ${source}`);
}
for (const name of ['config.yml', 'config-hackrf.yml', 'config-usrp.yml', 'config-kraken.yml']) {
  for (const mode of ['auto', 'cpu', 'gpu', 'cuda', '', 1, true, null]) {
    const value = load(name);
    value.process.performance = {...value.process.performance, acceleration: mode};
    assert.equal(validateConfig(value).valid, ['auto', 'cpu', 'gpu'].includes(mode),
      `${name}: acceleration ${JSON.stringify(mode)}`);
  }
}
const profiles = getDeviceProfiles();
assert.deepEqual(profiles.map(profile => profile.type).sort(),
  ['HackRF', 'Kraken', 'RspDuo', 'Usrp']);

for (const profile of profiles) {
  const switched = clone(sixChannel);
  switched.capture.fs = profile.sampleRate;
  switched.capture.device = clone(profile.device);
  if (profile.type === 'HackRF') switched.capture.device.serial = ['0001', '0002'];
  for (const key of ['performance', 'reference_synthesis']) {
    if (profile.process?.[key] !== undefined)
      switched.process[key] = clone(profile.process[key]);
    else
      delete switched.process[key];
  }
  const result = validateConfig(switched, sixChannel);
  assert.equal(result.valid, true,
    `${profile.type} switch: ${result.errors.join('; ')}`);
  assert.deepEqual(switched.process.performance, {surveillance_workers: 0, fft_threads: 0});
  for (const threads of [0, 1, 3, 256]) {
    switched.process.performance.fft_threads = threads;
    assert.equal(validateConfig(switched).valid, true, `${profile.type}: ${threads} FFT threads`);
  }
  for (const threads of [-1, .5, 257, 'auto', null]) {
    switched.process.performance.fft_threads = threads;
    assert.equal(validateConfig(switched).valid, false, `${profile.type}: reject ${threads} FFT threads`);
  }
}

const tooManyChannels = clone(sixChannel);
tooManyChannels.capture.device.channel_count = 9;
assert.equal(validateConfig(tooManyChannels).valid, false);

const duplicateChannel = clone(sixChannel);
duplicateChannel.capture.device.surveillance_channels = [0, 1, 1];
assert.equal(validateConfig(duplicateChannel).valid, false);

const invalidTiming = clone(sixChannel);
invalidTiming.process.data.buffer = 0.9;
assert.equal(validateConfig(invalidTiming).valid, false);

const missingSetting = clone(sixChannel);
delete missingSetting.network.ports.map;
assert.equal(validateConfig(missingSetting, sixChannel).valid, false);

const wrongType = clone(sixChannel);
wrongType.process.data.cpi = '0.5';
assert.equal(validateConfig(wrongType, sixChannel).valid, false);

const unsupportedDevice = clone(sixChannel);
unsupportedDevice.capture.device.type = 'RTL-SDR';
assert.equal(validateConfig(unsupportedDevice, sixChannel).valid, false);

const extraDeviceSetting = clone(sixChannel);
extraDeviceSetting.capture.device.unused = true;
assert.equal(validateConfig(extraDeviceSetting, sixChannel).valid, false);

function switchedTo(type) {
  const profile = profiles.find(item => item.type === type);
  const config = clone(sixChannel);
  config.capture.fs = profile.sampleRate;
  config.capture.device = clone(profile.device);
  if (type === 'HackRF') config.capture.device.serial = ['0001', '0002'];
  for (const key of ['performance', 'reference_synthesis']) {
    if (profile.process?.[key] !== undefined)
      config.process[key] = clone(profile.process[key]);
    else delete config.process[key];
  }
  return config;
}

for (const mutate of [
  config => { config.capture.device.gain_lna[0] = 7; },
  config => { config.capture.device.gain_lna[0] = 48; },
  config => { config.capture.device.gain_vga[1] = 31; },
  config => { config.capture.device.gain_vga[1] = 64; }
]) {
  const invalidHackRf = switchedTo('HackRF');
  mutate(invalidHackRf);
  assert.equal(validateConfig(invalidHackRf).valid, false);
}

for (const serial of [['0001', '001'], ['ABCD', 'BCD']]) {
  const overlappingHackRf = switchedTo('HackRF');
  overlappingHackRf.capture.device.serial = serial;
  const result = validateConfig(overlappingHackRf);
  assert.equal(result.valid, false);
  assert.ok(result.errors.some(error => error.includes('serial suffixes')));
}
const distinctShortHackRf = switchedTo('HackRF');
distinctShortHackRf.capture.device.serial = ['a1', 'b2'];
assert.equal(validateConfig(distinctShortHackRf).valid, true,
  'unique short suffixes remain valid and are resolved against hardware at native startup');

for (const mutate of [
  config => { config.capture.fs = 1500000; },
  config => { config.capture.fc = 2000000001; },
  config => { config.capture.device.agcSetPoint = -73; },
  config => { config.capture.device.bandwidthNumber = 10; },
  config => { config.capture.device.gainReduction[0] = 19; },
  config => { config.capture.device.lnaState = 10; }
]) {
  const invalidRspDuo = switchedTo('RspDuo');
  mutate(invalidRspDuo);
  assert.equal(validateConfig(invalidRspDuo).valid, false);
}

for (const [frequency, maximum] of [[1000000, 6], [59999999, 6],
  [60000000, 9], [999999999, 9], [1000000000, 8], [2000000000, 8]]) {
  const config = switchedTo('RspDuo');
  config.capture.fc = frequency;
  for (const state of [0, maximum]) {
    config.capture.device.lnaState = state;
    assert.equal(validateConfig(config).valid, true, `RSPduo LNA ${state} at ${frequency}`);
  }
  config.capture.device.lnaState = maximum + 1;
  assert.equal(validateConfig(config).valid, false, `RSPduo LNA limit at ${frequency}`);
}

const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'blah2-config-test-'));
const filename = path.join(directory, 'config.yml');
try {
  fs.writeFileSync(filename, 'original: true\n');
  writeConfigAtomically(filename, sixChannel);
  assert.deepEqual(yaml.load(fs.readFileSync(filename, 'utf8')), sixChannel);
  assert.equal(fs.readFileSync(`${filename}.bak`, 'utf8'), 'original: true\n');
} finally {
  fs.rmSync(directory, {recursive: true});
}

console.log('Configuration validation tests passed.');
const ordinary = validateConfig(sixChannel);
assert.ok(ordinary.estimatedMemoryBytes > 512 * 1048576);
assert.deepEqual(ordinary.warnings, [], 'Normal memory use must not produce warnings');
assert.deepEqual(ordinary.notices, []);
const sameSites = clone(sixChannel);
sameSites.location.tx = clone(sameSites.location.rx);
const siteAdvice = validateConfig(sameSites);
assert.equal(siteAdvice.valid, true);
assert.equal(siteAdvice.notices.length, 1);
assert.equal(siteAdvice.notices[0].field, 'location');
const previousBudget = process.env.BLAH2_CONFIG_MEMORY_LIMIT_MB;
try {
  process.env.BLAH2_CONFIG_MEMORY_LIMIT_MB = '16';
  assert.equal(validateConfig(sixChannel).valid, false, 'Configured memory-budget violations still block saving');
} finally {
  if (previousBudget === undefined) delete process.env.BLAH2_CONFIG_MEMORY_LIMIT_MB;
  else process.env.BLAH2_CONFIG_MEMORY_LIMIT_MB = previousBudget;
}

// Every leaf in every supported receiver profile rejects incorrect types,
// including with no baseline (first-run setup).
let checked = 0;
function leaves(value, prefix = []) {
  return value && typeof value === 'object' && !Array.isArray(value) ?
    Object.entries(value).flatMap(([key, child]) => leaves(child, [...prefix, key])) :
    [{keys: prefix, value}];
}
function set(config, keys, value) {
  const parent = keys.slice(0, -1).reduce((item, key) => item[key], config);
  parent[keys[keys.length - 1]] = value;
}
for (const profile of profiles) {
  const base = switchedTo(profile.type);
  for (const {keys, value} of leaves(base)) {
    if (keys.join('.') === 'truth.adsb.adsb2dd') continue; // retired converter address is migration-only.
    const invalid = clone(base);
    set(invalid, keys, typeof value === 'number' ? String(value) : null);
    assert.equal(validateConfig(invalid).valid, false, `Type check: ${keys.join('.')}`);
    checked++;
  }
}
for (const [key, value] of [
  ['process.data.cpi', 0.0001], ['process.data.cpi', 1e12],
  ['process.data.buffer', 1e12], ['process.detection.nGuard', 128],
  ['process.detection.nTrain', 0], ['process.detection.pfa', 1],
  ['process.detection.pfa', 0], ['process.ambiguity.dopplerMax', 1000000],
  ['process.ambiguity.delayMax', 65524], ['process.clutter.delayMax', 2147483647],
  ['process.tracker.initiate.M', 256], ['process.tracker.initiate.N', 1],
  ['process.tracker.initiate.maxAcc', 1e9], ['network.ip', 'http://localhost'],
  ['process.tracker.initiate.acceleration_model', 'unbounded'],
  ['process.bearing.min_peak_separation_db', .99],
  ['process.bearing.min_peak_separation_db', 15.01],
  ['truth.adsb.tar1090', 'file:///etc/passwd'], ['save.path', '/tmp/no-slash'],
  ['capture.replay.format', 'unsupported'], ['process.reference_synthesis.diagonal_loading', -1]
]) {
  const invalid = clone(sixChannel);
  if (key.startsWith('process.clutter.delay'))
    invalid.process.clutter.followAmbiguity = false;
  set(invalid, key.split('.'), value);
  assert.equal(validateConfig(invalid).valid, false, `${key}=${value}`);
}
const narrow = clone(sixChannel);
narrow.process.ambiguity.dopplerMin = -1;
narrow.process.ambiguity.dopplerMax = 1;
assert.equal(validateConfig(narrow).valid, false, 'Correlation FFT uint16 overflow');
const wide = clone(sixChannel);
wide.process.data.cpi = .2;
wide.process.ambiguity.dopplerMin = -10000;
wide.process.ambiguity.dopplerMax = 10000;
wide.process.ambiguity.delayMin = -2;
wide.process.ambiguity.delayMax = 20;
const wideResult = validateConfig(wide);
assert.equal(wideResult.valid, true, `Independent Doppler buffer: ${wideResult.errors.join('; ')}`);
for (const [minimum, maximum, valid] of [[-198, 198, true], [-199, 198, false],
  [-198, 199, false], [-10, 245, false]]) {
  const boundary = clone(sixChannel);
  boundary.capture.fs = 2400000;
  boundary.process.data.cpi = .2;
  Object.assign(boundary.process.ambiguity, {delayMin: minimum, delayMax: maximum,
    dopplerMin: -6000, dopplerMax: 6000});
  const result = validateConfig(boundary);
  assert.equal(result.valid, valid, `Signed lag ${minimum}..${maximum}: ${result.errors.join('; ')}`);
  if (!valid) assert(result.errors.some(error => error.includes('delay limits must be within -198 to 198')));
}
const guardZero = clone(sixChannel);
guardZero.process.detection.nGuard = 0;
guardZero.process.reference_synthesis.diagonal_loading = 0;
assert.equal(validateConfig(guardZero).valid, true);
const twoSeconds = clone(sixChannel);
twoSeconds.process.data.cpi = 2;
twoSeconds.process.data.buffer = 1;
assert.equal(validateConfig(twoSeconds).valid, true, 'Buffer is an interval count, not seconds');
for (let count = 2; count <= 8; count++) {
  const changed = clone(sixChannel);
  changed.capture.device.channel_count = count;
  changed.capture.device.surveillance_channels = Array.from({length: count}, (_, i) => i);
  changed.process.reference_synthesis.channels = [...changed.capture.device.surveillance_channels];
  assert.equal(validateConfig(changed).valid, true, `${count} channels`);
}
for (const profile of profiles) {
  const replay = switchedTo(profile.type);
  replay.capture.replay = {...replay.capture.replay, state: true, loop: true,
    file: `/tmp/${profile.type}.iq`, format: 'auto'};
  assert.equal(validateConfig(replay).valid, true, `${profile.type} replay`);
}
const legacyUsrp = switchedTo('Usrp');
legacyUsrp.capture.replay = {...legacyUsrp.capture.replay, state: true,
  format: 'usrp-blocks', legacy_block_samples: 4096};
assert.equal(validateConfig(legacyUsrp).valid, true, 'USRP legacy replay block size');
delete legacyUsrp.capture.replay.legacy_block_samples;
assert.equal(validateConfig(legacyUsrp).valid, false, 'USRP legacy replay requires block size');
console.log(`Exhaustive type checks passed for ${checked} device-setting leaves; boundary and cross-field tests passed.`);
