const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const yaml = require('js-yaml');
const {getDeviceProfiles, validateConfig,
  writeConfigAtomically} = require('./config-manager.js');
const {configurationForReceiver, setupDefaults} = require('./config-store.js');

function load(name) {
  const value = name === 'config.yml' ? setupDefaults() : configurationForReceiver(name);
  if (value.capture.device.type === 'HackRF') value.capture.device.serial = ['0001', '0002'];
  return value;
}
const clone = value => JSON.parse(JSON.stringify(value));

// Native 601x1261 lane geometry; display downsampling cannot admit more work.
for (const fractional of [false, true]) for (const [delay, doppler, accepted] of
    [[1, 1, true], [2, 2, true], [2, 4, false]]) {
  const config = load('Usrp');
  config.capture.fs = 24576000; config.capture.fc = 548000000;
  config.process.data.cpi = 0.5;
  config.process.delay_reference_sample_rate_hz = 2000000;
  config.process.ambiguity = {delayMin: -10, delayMax: 400, dopplerMin: -600, dopplerMax: 600};
  config.process.clutter.followAmbiguity = true;
  config.process.detection.track_before_detect = {enable: true, max_cpis: 8,
    motion_bins: 1, delay_motion_bins: delay, doppler_motion_bins: doppler,
    fractional_delay: fractional};
  Object.assign(config.process.tracker.initiate, {M: 2, N: 9});
  config.process.tracker.delete = 7;
  config.network.ports.illuminator = 3007;
  config.process.multi_illuminator = {enabled: true, illuminators: [{
    id: 'atsc', enabled: true, standard: 'ATSC-1.0', frequency_hz: 551000000,
    occupied_bandwidth_hz: 5380000, output_sample_rate_hz: 6144000,
    filter_taps_per_phase: 64, atsc_reference_mode: 'pilot_aided',
    transmitter: {name: 'test', latitude: 40, longitude: -80, altitude: 300}}]};
  const before = JSON.stringify(config), result = validateConfig(config);
  assert.equal(result.valid, accepted, result.errors.join('; '));
  assert.equal(JSON.stringify(config), before, 'budget admission must not rewrite intent');
  if (!accepted) assert.ok(result.errors.some(error =>
    error.includes('601x1261') && error.includes('477452430 path samples')));
  config.process.detection.track_before_detect.max_cpis = 4;
  assert.equal(validateConfig(config).valid, true, 'same motion fits four-CPI path budget');
}
const arrayTemporalBudget = load('Kraken');
arrayTemporalBudget.process.data.cpi = 0.5;
arrayTemporalBudget.process.delay_reference_sample_rate_hz = 2400000;
arrayTemporalBudget.process.ambiguity = {delayMin: -10, delayMax: 500, dopplerMin: -600, dopplerMax: 600};
arrayTemporalBudget.process.clutter.followAmbiguity = true;
arrayTemporalBudget.process.detection.track_before_detect = {enable: true, max_cpis: 8,
  motion_bins: 1, delay_motion_bins: 2, doppler_motion_bins: 4, fractional_delay: true};
Object.assign(arrayTemporalBudget.process.tracker.initiate, {M: 4, N: 25});
arrayTemporalBudget.process.tracker.delete = 7;
assert.equal(validateConfig(arrayTemporalBudget).valid, true,
  validateConfig(arrayTemporalBudget).errors.join('; '));
arrayTemporalBudget.process.detection.track_before_detect.doppler_motion_bins = 16;
assert.ok(validateConfig(arrayTemporalBudget).errors.some(error =>
  error.includes('601x511') && error.includes('path samples')));

const firstRun = validateConfig(load('config.yml'));
assert.equal(firstRun.valid, false);
assert.ok(firstRun.errors.includes('capture.device.type is not supported'));

for (const name of ['HackRF', 'Usrp',
  'Kraken', 'RspDuo']) {
  const result = validateConfig(load(name));
  assert.equal(result.valid, true, `${name}: ${result.errors.join('; ')}`);
}

const krakenProfile = load('Kraken');
assert.equal(krakenProfile.network.tracker_forward?.enabled ?? false, false,
  'the generic Kraken profile must not start a disconnected external tracker feed');
assert.equal(validateConfig({...krakenProfile, network: {
  ...krakenProfile.network, tracker_forward: {enabled: false}}}).valid, true,
  'a disabled tracker feed must not require an unused external endpoint');
const incompleteEnabledTracker = clone(krakenProfile);
incompleteEnabledTracker.network.tracker_forward = {enabled: true};
assert.equal(validateConfig(incompleteEnabledTracker).valid, false,
  'enabling the tracker feed requires an explicit endpoint');

const twoFmIlluminators = clone(krakenProfile);
twoFmIlluminators.capture.fc = 90900000;
twoFmIlluminators.process.multi_illuminator = {enabled: true, illuminators: [
  {id: 'fm-90.5', enabled: true, standard: 'FM', frequency_hz: 90500000,
    occupied_bandwidth_hz: 200000, output_sample_rate_hz: 300000,
    filter_taps_per_phase: 24,
    transmitter: {name: 'FM 90.5', latitude: 40.44, longitude: -80.0, altitude: 300}},
  {id: 'fm-91.3', enabled: true, standard: 'FM', frequency_hz: 91300000,
    occupied_bandwidth_hz: 200000, output_sample_rate_hz: 300000,
    filter_taps_per_phase: 24,
    transmitter: {name: 'FM 91.3', latitude: 40.45, longitude: -80.01, altitude: 310}}
]};
assert.equal(validateConfig(twoFmIlluminators).valid, true,
  'one Kraken passband accepts two isolated FM illuminators');
for (const enabled of [false, true]) for (const clutter of [false, true])
  for (const detection of [false, true]) for (const tracker of [false, true]) {
    const candidate = clone(twoFmIlluminators);
    candidate.process.multi_illuminator.enabled = enabled;
    candidate.process.clutter.enable = clutter;
    candidate.process.detection.enable = detection;
    candidate.process.tracker.enable = tracker;
    const result = validateConfig(candidate);
    assert.equal(result.valid, !tracker || detection,
      `Stage toggles share one dependency in single/multi mode: ${JSON.stringify({enabled, clutter, detection, tracker})}; ${result.errors.join('; ')}`);
  }
const laneBirthThreshold = clone(twoFmIlluminators);
laneBirthThreshold.process.multi_illuminator.illuminators[1].tracker_min_snr_db = 5.5;
assert.equal(validateConfig(laneBirthThreshold).valid, true,
  'per-lane tracker admission may be calibrated without changing detector PFA');
assert.equal(laneBirthThreshold.process.detection.pfa, twoFmIlluminators.process.detection.pfa);
for (const threshold of ['5.5', NaN, Infinity, -101, 101]) {
  const invalid = clone(twoFmIlluminators);
  invalid.process.multi_illuminator.illuminators[0].tracker_min_snr_db = threshold;
  assert.equal(validateConfig(invalid).valid, false, 'lane birth threshold must be finite and bounded');
}
const replayIlluminator = clone(twoFmIlluminators);
replayIlluminator.capture.replay = {...replayIlluminator.capture.replay,
  state: true, sample_rate_hz: 12288000, frequency_hz: 539000000};
replayIlluminator.process.multi_illuminator.illuminators = [{
  ...replayIlluminator.process.multi_illuminator.illuminators[0], id: 'atsc',
  standard: 'ATSC', frequency_hz: 539000000,
  occupied_bandwidth_hz: 6000000, output_sample_rate_hz: 6144000}];
replayIlluminator.process.ambiguity.dopplerMin = -2400;
replayIlluminator.process.ambiguity.dopplerMax = 2400;
assert.equal(validateConfig(replayIlluminator).valid, true,
  `lane validation follows replay rate/carrier: ${validateConfig(replayIlluminator).errors.join('; ')}`);
assert.equal(replayIlluminator.capture.fs, 2400000,
  'wideband replay leaves the Kraken live sample rate untouched');
for (const standard of ['ATSC', 'ATSC-1.0', 'ATSC-3.0']) {
  for (const offlineReplay of [false, true]) {
    for (const requested of ['automatic', 'raw', 'pilot_aided']) {
      const candidate = load('Usrp');
      candidate.capture.fc = 539000000;
      candidate.capture.fs = 12288000;
      candidate.capture.replay.state = offlineReplay;
      candidate.process.ambiguity.dopplerMin = -2400;
      candidate.process.ambiguity.dopplerMax = 2400;
      candidate.process.multi_illuminator = {enabled: true, illuminators: [{
        id: 'atsc', enabled: true, standard, frequency_hz: 539000000,
        occupied_bandwidth_hz: 6000000, output_sample_rate_hz: 6144000,
        transmitter: {name: 'ATSC test', latitude: 40.44, longitude: -80, altitude: 300},
        atsc_reference_mode: requested, reference_channel: 0, surveillance_channels: [1]}]};
      const expected = requested !== 'pilot_aided' || standard !== 'ATSC-3.0';
      const result = validateConfig(candidate);
      assert.equal(result.valid, expected,
        `${standard}/${requested}/replay=${offlineReplay}: ${result.errors.join('; ')}`);
    }
  }
}
const pilotArray = clone(replayIlluminator);
pilotArray.process.multi_illuminator.illuminators[0].atsc_reference_mode = 'pilot_aided';
assert.ok(validateConfig(pilotArray).errors.some(error =>
  error.includes('pilot_aided does not support array reference')));
pilotArray.process.multi_illuminator.illuminators[0].atsc_reference_mode = 'automatic';
assert.equal(validateConfig(pilotArray).valid, true,
  'automatic array reference safely retains raw rather than forcing pilot projection');
for (const mutate of [
  value => { value.capture.replay.sample_rate_hz = 12000000; },
  value => { value.capture.replay.frequency_hz = 527000000; }
]) {
  const invalid = clone(replayIlluminator);
  mutate(invalid);
  assert.equal(validateConfig(invalid).valid, false,
    'invalid recorded passband/rate cannot borrow valid saved live geometry');
}
const adaptiveMultiIlluminator = clone(twoFmIlluminators);
adaptiveMultiIlluminator.process.tracker.initiate.acceleration_model = 'adaptive_gate';
assert.equal(validateConfig(adaptiveMultiIlluminator).valid, true,
  'multi-illuminator CPU tracking supports adaptive-gate initiation');
for (const profile of ['balanced', 'conservative', 'permissive']) {
  for (const model of [undefined, 'adaptive_gate', 'hypothesis_bank']) {
    const value = clone(twoFmIlluminators);
    value.process.tracker.profile = profile;
    if (model === undefined) delete value.process.tracker.initiate.acceleration_model;
    else value.process.tracker.initiate.acceleration_model = model;
    assert.equal(validateConfig(value).valid, true,
      `multi accepts profile ${profile} with ${model ?? 'inherited'} acceleration model`);
  }
}
const residentMultiIlluminator = clone(twoFmIlluminators);
residentMultiIlluminator.process.performance.acceleration = 'gpu-resident';
assert.equal(validateConfig(residentMultiIlluminator).valid, true,
  'multi admits bounded resident frontends and the common host product contract');
for (const source of [twoFmIlluminators, ...['HackRF', 'Usrp',
  'Kraken', 'RspDuo'].map(load)]) {
  for (const acceleration of ['cpu', 'auto', 'gpu', 'gpu-resident']) {
    for (const max_cpis of [4, 8]) {
      const value = clone(source);
      value.process.performance.acceleration = acceleration;
      value.process.detection.track_before_detect = {enable: true, max_cpis,
        motion_bins: 1, fractional_delay: true};
      value.process.tracker.initiate.stationary_clutter = {
        enable: true, persistence_cpis: 4, max_doppler_hz: 0};
      value.process.tracker.initiate.N = 1 + max_cpis * (value.process.tracker.initiate.M - 1);
      value.process.tracker.delete = Math.max(value.process.tracker.delete, max_cpis - 1);
      const result = validateConfig(value);
      assert.equal(result.valid, true, `${value.capture.device.type}/${acceleration}/${max_cpis}: ${result.errors.join('; ')}`);
    }
  }
}
const fmReferenceExperiment = clone(twoFmIlluminators);
fmReferenceExperiment.process.multi_illuminator.experimental_fm_reference_quality_filter = true;
assert.equal(validateConfig(fmReferenceExperiment).valid, true,
  `Explicit FM reference setting is available live: ${validateConfig(fmReferenceExperiment).errors.join('; ')}`);
fmReferenceExperiment.capture.replay.state = true;
Object.assign(fmReferenceExperiment.capture.replay, {sample_rate_hz: 2400000, frequency_hz: 90900000});
assert.equal(validateConfig(fmReferenceExperiment).valid, true,
  `Explicit FM reference setting is identical in replay: ${validateConfig(fmReferenceExperiment).errors.join('; ')}`);
fmReferenceExperiment.process.multi_illuminator.illuminators.forEach(lane => { lane.standard = 'generic'; });
assert.ok(validateConfig(fmReferenceExperiment).errors.some(error =>
  error.includes('experimental FM reference quality filtering requires')));
const invalidFmSwitch = clone(twoFmIlluminators);
invalidFmSwitch.process.multi_illuminator.experimental_fm_reference_quality_filter = 'false';
assert.equal(validateConfig(invalidFmSwitch).valid, false);
const dormantFmIlluminators = clone(twoFmIlluminators);
dormantFmIlluminators.process.multi_illuminator.enabled = false;
dormantFmIlluminators.capture.fc = 509000000;
assert.equal(validateConfig(dormantFmIlluminators).valid, true,
  'saved dormant lanes do not invalidate a different active passband');
const dormantLane = clone(twoFmIlluminators);
dormantLane.process.multi_illuminator.illuminators[1].enabled = false;
dormantLane.process.multi_illuminator.illuminators[1].frequency_hz = 93000000;
assert.equal(validateConfig(dormantLane).valid, true,
  'a disabled lane may remain saved outside the active passband');
for (const mutate of [
  value => { value.process.multi_illuminator.illuminators[1].id = 'fm-90.5'; },
  value => { value.process.multi_illuminator.illuminators[1].frequency_hz = 90500000; },
  value => { value.process.multi_illuminator.illuminators[1].frequency_hz = 93000000; },
  value => { value.process.multi_illuminator.illuminators[1].output_sample_rate_hz = 280000; },
  value => { value.process.multi_illuminator.illuminators[1].occupied_bandwidth_hz = 300000; },
  value => { value.process.multi_illuminator.illuminators.forEach(item => { item.enabled = false; }); },
  value => { value.process.multi_illuminator.illuminators[1].transmitter.latitude = 91; }
]) {
  const invalid = clone(twoFmIlluminators);
  mutate(invalid);
  assert.equal(validateConfig(invalid).valid, false,
    'multi-illuminator configuration must fail closed');
}

for (const motion of [0, 1]) {
  for (const acceleration of ['cpu', 'auto', 'gpu']) {
    const integrated = clone(twoFmIlluminators);
    integrated.process.performance.acceleration = acceleration;
    integrated.process.detection.integration_cpis = 2;
    integrated.process.detection.integration_motion_bins = motion;
    const result = validateConfig(integrated);
    assert.equal(result.valid, true,
      `lane integration uses the shared detector on ${acceleration}: ${result.errors.join('; ')}`);
  }
}
for (const [cpis, motion] of [[0, 0], [3, 0], [1, 1], [2, 2]]) {
  const invalid = clone(twoFmIlluminators);
  invalid.process.detection.integration_cpis = cpis;
  invalid.process.detection.integration_motion_bins = motion;
  assert.equal(validateConfig(invalid).valid, false,
    'lane integration validates the same bounded depth and motion geometry');
}

for (const [field, mutate] of [
  ['coherent fusion', value => { value.process.surveillance_fusion.mode = 'coherent_beam_bank'; }],
  ['beam tolerance', value => { value.process.tracker.beam_tolerance = 1; }],
  ['bearing', value => { value.process.bearing.enable = true; }],
  ['bearing manifold recorder', value => {
    value.process.bearing.manifold_recorder.enabled = 'on';
  }]
]) {
  const invalid = clone(twoFmIlluminators);
  mutate(invalid);
  assert.equal(validateConfig(invalid).valid, false,
    `multi-illuminator mode must reject unsupported ${field}`);
}

const sixChannel = load('Kraken');
sixChannel.capture.device.channel_count = 6;
sixChannel.capture.device.surveillance_channels = [0, 1, 2, 3, 4, 5];
sixChannel.process.reference_synthesis.channels = [0, 1, 2, 3, 4, 5];
// Shipped examples intentionally use neutral sites. This fixture exercises the
// ordinary no-advice path, so make its sites explicitly distinct.
sixChannel.location.rx = {name: 'Test receiver', latitude: 40, longitude: -75, altitude: 100};
sixChannel.location.tx = {name: 'Test transmitter', latitude: 41, longitude: -74, altitude: 120};
assert.equal(validateConfig(sixChannel).valid, true);

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
const coherentFusion = load('Kraken');
coherentFusion.process.surveillance_fusion = {mode: 'coherent_beam_bank', beams: 12};
coherentFusion.capture.device.array_geometry = {
  shape: 'regular_polygon', units: 'm', coordinate_frame: 'array_xy_right_handed',
  mapping_confirmed: true, geometry_confirmed: true, orientation_confirmed: true,
  x_axis_bearing_deg_true: 0,
  bearing_channels: [...coherentFusion.capture.device.surveillance_channels], elements:
  coherentFusion.capture.device.surveillance_channels.map((daq_channel, index) => ({
    id: `channel-${daq_channel}`, daq_channel, receiver_serial: String(1000 + daq_channel),
    position: [Math.cos(2 * Math.PI * index / 5) * .2,
      Math.sin(2 * Math.PI * index / 5) * .2, 0]
  }))};
assert.equal(validateConfig(coherentFusion).valid, true,
  'coherent beam-bank fusion accepts complete Kraken geometry');
const coherentLanes = clone(coherentFusion);
coherentLanes.capture.fs = twoFmIlluminators.capture.fs;
coherentLanes.capture.fc = twoFmIlluminators.capture.fc;
coherentLanes.process.multi_illuminator = clone(twoFmIlluminators.process.multi_illuminator);
for (const lane of coherentLanes.process.multi_illuminator.illuminators)
  lane.transmitter_location = {latitude: 40.1, longitude: -75.2, altitude: 130};
for (const backend of ['cpu', 'auto', 'gpu', 'gpu-resident']) {
  const configured = clone(coherentLanes);
  configured.process.performance.acceleration = backend;
  configured.process.tracker.gpu_capacity = 128;
  const result = validateConfig(configured);
  assert.equal(result.valid, true, `${backend} coherent lanes: ${result.errors.join('; ')}`);
}
for (const backend of ['gpu-resident', 'auto']) for (const feature of ['noise-whitened', 'bearing', 'beam-history']) {
  const configured = clone(coherentLanes);
  configured.process.performance.acceleration = backend;
  configured.process.tracker.gpu_capacity = 128;
  if (feature === 'noise-whitened') configured.process.surveillance_fusion.mode = 'noise_whitened_beam_bank';
  if (feature === 'bearing') {
    configured.process.bearing.enable = true;
    configured.process.detection.track_before_detect = {enable: true, max_cpis: 4, motion_bins: 1};
    configured.process.tracker.initiate.N = 1 + 4 * (configured.process.tracker.initiate.M - 1);
    configured.process.tracker.delete = Math.max(configured.process.tracker.delete, 3);
  }
  if (feature === 'beam-history') {
    configured.process.detection.integration_cpis = 2;
    configured.process.tracker.beam_tolerance = 1;
  }
  const result = validateConfig(configured);
  if (feature === 'beam-history') {
    assert.equal(result.valid, false);
    assert.ok(result.errors.some(error => error.includes('Two-CPI beam compatibility history is unsupported')),
      result.errors.join('; '));
    for (const depth of [4, 8]) {
      const supported = clone(configured);
      supported.process.detection.track_before_detect = {enable: true, max_cpis: depth, motion_bins: 1};
      supported.process.tracker.initiate.N = 1 + depth * (supported.process.tracker.initiate.M - 1);
      supported.process.tracker.delete = Math.max(supported.process.tracker.delete, depth - 1);
      const accepted = validateConfig(supported);
      assert.equal(accepted.valid, true, `${depth}-CPI beam-history coherent lanes: ${accepted.errors.join('; ')}`);
    }
  } else assert.equal(result.valid, true, `${feature} coherent lanes: ${result.errors.join('; ')}`);
}
for (const depth of [1, 2, 4, 8]) for (const channelized of [false, true]) {
  const base = clone(channelized ? coherentLanes : coherentFusion);
  base.process.performance.acceleration = 'gpu-resident';
  base.process.tracker.gpu_capacity = 128;
  base.process.detection.integration_cpis = Math.min(depth, 2);
  base.process.detection.track_before_detect = {enable: depth > 2, max_cpis: Math.max(depth, 4), motion_bins: 1};
  base.process.tracker.initiate.N = 1 + depth * (base.process.tracker.initiate.M - 1);
  base.process.tracker.delete = Math.max(base.process.tracker.delete, depth - 1);
  for (const [beams, scan, enabled, accepted] of [
    [31, 60, false, true], [32, 60, false, depth <= 2],
    [127, 127, true, depth > 2], [128, 128, true, false]
  ]) {
    const value = clone(base);
    value.process.surveillance_fusion.beams = beams;
    Object.assign(value.process.bearing, {enable: enabled, scan_beams: scan});
    const before = JSON.stringify(value), result = validateConfig(value);
    assert.equal(result.valid, accepted, `${depth}/${channelized}/${beams}/${enabled}: ${result.errors.join('; ')}`);
    assert.equal(JSON.stringify(value), before, 'refinement admission must not clamp or rewrite');
    if (depth > 2 && !accepted) assert.ok(result.errors.some(error => error.includes('at most 31')));
    if (depth <= 2 && enabled) assert.ok(result.errors.some(error => error.includes('four/eight-CPI track-before-detect')));
  }
  if (depth > 2) for (const backend of ['cpu', 'auto', 'gpu']) {
    const portable = clone(base);
    portable.process.performance.acceleration = backend;
    portable.process.surveillance_fusion.beams = 360;
    portable.process.bearing.enable = false;
    const result = validateConfig(portable);
    assert.equal(result.valid, true, `${backend} temporal grid remains supported: ${result.errors.join('; ')}`);
  }
}
const missingLaneSite = clone(coherentLanes);
for(const backend of ['cpu','auto','gpu','gpu-resident'])for(const multi of [false,true]) {
  const beam=clone(multi?coherentLanes:coherentFusion);
  beam.process.performance.acceleration=backend;
  beam.process.detection.integration_cpis=1;
  beam.process.detection.track_before_detect={enable:false};
  beam.process.tracker.beam_tolerance=1;
  assert.ok(validateConfig(beam).errors.some(error=>error.includes('active resident coherent temporal')),
    `${backend}/${multi} must not ignore one-CPI beam tolerance`);
}
delete missingLaneSite.process.multi_illuminator.illuminators[0].transmitter_location;
assert.equal(validateConfig(missingLaneSite).valid, false, 'coherent lane never borrows a global transmitter site');
for (const site of [{latitude: 91, longitude: 0, altitude: 0},
  {latitude: 1, longitude: 181, altitude: 0}, {latitude: 1, longitude: 1}]) {
  const invalid = clone(coherentLanes);
  invalid.process.multi_illuminator.illuminators[0].transmitter_location = site;
  assert.equal(validateConfig(invalid).valid, false, 'lane site coordinates must be explicit and finite');
}
const coherentDedicatedLane = clone(coherentLanes);
coherentDedicatedLane.process.multi_illuminator.illuminators[0].reference_channel = 0;
coherentDedicatedLane.process.multi_illuminator.illuminators[0].surveillance_channels = [1, 2, 3, 4];
assert.equal(validateConfig(coherentDedicatedLane).valid, false,
  'coherent calibration requires matching array-reference roles');
const correlatedBeamTrials = clone(coherentFusion);
correlatedBeamTrials.process.surveillance_fusion.effective_beam_trials = 5;
assert.equal(validateConfig(correlatedBeamTrials).valid, true,
  'effective trials may be lower than the correlated beam-bank size');
const excessiveBeamTrials = clone(coherentFusion);
excessiveBeamTrials.process.surveillance_fusion.effective_beam_trials = 13;
assert.equal(validateConfig(excessiveBeamTrials).valid, false,
  'effective trials cannot exceed the beam-bank size');
const defaultBeamTrials = clone(coherentFusion);
delete defaultBeamTrials.process.surveillance_fusion.beams;
defaultBeamTrials.process.surveillance_fusion.effective_beam_trials = 5;
assert.equal(validateConfig(defaultBeamTrials).valid, true,
  'reduced effective trials also work with the default twelve-beam bank');
defaultBeamTrials.process.surveillance_fusion.effective_beam_trials = 13;
assert.equal(validateConfig(defaultBeamTrials).valid, false,
  'effective trials cannot exceed the implicit twelve-beam bank');
const noncoherentBeamTrials = clone(coherentFusion);
noncoherentBeamTrials.process.surveillance_fusion.mode = 'noncoherent';
noncoherentBeamTrials.process.surveillance_fusion.effective_beam_trials = 5;
assert.equal(validateConfig(noncoherentBeamTrials).valid, false,
  'noncoherent fusion cannot accept a coherent-bank trial correction');
const measuredFusion = clone(coherentFusion);
Object.assign(measuredFusion.capture.device.array_geometry,
  {mapping_confirmed: true, geometry_confirmed: true, orientation_confirmed: true});
measuredFusion.capture.device.heimdall.gain = 14;
measuredFusion.capture.device.heimdall.gain_control = 'fixed';
measuredFusion.process.surveillance_fusion.manifold_file = '/tmp/detection-manifold.json';
assert.equal(validateConfig(measuredFusion).valid, true,
  'coherent beam-bank fusion accepts a distinct detection manifold path');
for (const manifold of ['', '   ', 7, null]) {
  const invalid = clone(coherentFusion);
  invalid.process.surveillance_fusion.manifold_file = manifold;
  assert.equal(validateConfig(invalid).valid, false,
    `reject invalid detection manifold path ${JSON.stringify(manifold)}`);
}
const noncoherentMeasuredFusion = clone(measuredFusion);
noncoherentMeasuredFusion.process.surveillance_fusion.mode = 'noncoherent';
assert.equal(validateConfig(noncoherentMeasuredFusion).valid, false,
  'detection manifold cannot be attached to noncoherent fusion');
const whitenedFusion = clone(coherentFusion);
whitenedFusion.process.performance.acceleration = 'gpu-resident';
whitenedFusion.process.tracker.gpu_capacity = 1024;
whitenedFusion.process.surveillance_fusion.mode = 'noise_whitened_beam_bank';
assert.equal(validateConfig(whitenedFusion).valid, true,
  'noise-whitened beam-bank fusion accepts resident Kraken geometry');
const residentMeasuredFusion = clone(whitenedFusion);
Object.assign(residentMeasuredFusion.capture.device.array_geometry,
  {mapping_confirmed: true, geometry_confirmed: true, orientation_confirmed: true});
residentMeasuredFusion.capture.device.heimdall.gain = 14;
residentMeasuredFusion.capture.device.heimdall.gain_control = 'fixed';
residentMeasuredFusion.process.surveillance_fusion.mode = 'coherent_beam_bank';
residentMeasuredFusion.process.surveillance_fusion.manifold_file = '/tmp/detection-manifold.json';
assert.equal(validateConfig(residentMeasuredFusion).valid, true,
  'detection manifold is accepted by the resident coherent GPU path');
const residentWhitenedMeasuredFusion = clone(whitenedFusion);
Object.assign(residentWhitenedMeasuredFusion.capture.device.array_geometry,
  {mapping_confirmed: true, geometry_confirmed: true, orientation_confirmed: true});
residentWhitenedMeasuredFusion.capture.device.heimdall.gain = 14;
residentWhitenedMeasuredFusion.capture.device.heimdall.gain_control = 'fixed';
residentWhitenedMeasuredFusion.process.surveillance_fusion.manifold_file =
  '/tmp/detection-manifold.json';
assert.equal(validateConfig(residentWhitenedMeasuredFusion).valid, true,
  'detection manifold is accepted by the resident noise-whitened GPU path');
for (const mutate of [
  value => { value.capture.device.heimdall.gain = 'keep'; },
  value => { value.capture.device.heimdall.gain_control = 'measured'; },
  value => { value.capture.device.array_geometry.orientation_confirmed = false; }
]) {
  const invalid = clone(residentWhitenedMeasuredFusion);
  mutate(invalid);
  assert.equal(validateConfig(invalid).valid, false,
    'measured resident manifold must fail closed when its identity is not fixed');
}
const nonresidentWhitenedFusion = clone(whitenedFusion);
nonresidentWhitenedFusion.process.performance.acceleration = 'auto';
assert.equal(validateConfig(nonresidentWhitenedFusion).valid, true,
  'AUTO selects required noise-whitened resident products');
assert.equal(nonresidentWhitenedFusion.process.performance.acceleration, 'auto');
for (const backend of ['cpu', 'gpu']) {
  const invalid = clone(nonresidentWhitenedFusion);
  invalid.process.performance.acceleration = backend;
  assert.equal(validateConfig(invalid).valid, false, 'Explicit nonresident modes cannot discard whitening');
}
const adaptiveTracker = clone(whitenedFusion);
adaptiveTracker.process.tracker.initiate.acceleration_model = 'adaptive_gate';
assert.equal(validateConfig(adaptiveTracker).valid, true,
  'adaptive tracker initiation accepts resident acceleration');
const nonresidentAdaptiveTracker = clone(adaptiveTracker);
nonresidentAdaptiveTracker.process.performance.acceleration = 'auto';
assert.equal(validateConfig(nonresidentAdaptiveTracker).valid, true,
  'AUTO retains required beam-bank features with adaptive tracking');
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
assert.equal(validateConfig(nonresidentStationaryClutter).valid, true,
  'AUTO retains required beam-bank features with stationary-clutter tracking');
const temporalTracker = clone(stationaryClutterTracker);
temporalTracker.process.detection.track_before_detect = {
  enable: true, max_cpis: 8, motion_bins: 1};
temporalTracker.process.tracker.initiate.N = 1 + 8 * (temporalTracker.process.tracker.initiate.M - 1);
temporalTracker.process.tracker.delete = 7;
assert.equal(validateConfig(temporalTracker).valid, true,
  'canonical temporal detection accepts resident rolling history');
for (const mode of [undefined, 'cpu', 'auto', 'gpu']) {
  const invalid = clone(temporalTracker);
  if (mode === undefined) delete invalid.process.performance;
  else invalid.process.performance.acceleration = mode;
  const result = validateConfig(invalid);
  assert.ok(!result.errors.some(error => /stationary-clutter.*requires gpu-resident/.test(error)),
    `host recovery is not backend-gated for ${mode ?? 'missing performance'}`);
}
const inactiveStationaryTracker = clone(stationaryClutterTracker);
inactiveStationaryTracker.process.tracker.enable = false;
assert.ok(validateConfig(inactiveStationaryTracker).errors.some(error =>
  error.includes('stationary-clutter initiation filtering requires enabled tracking')));
const fractionalDelayTracker = clone(temporalTracker);
fractionalDelayTracker.process.detection.track_before_detect.fractional_delay = true;
assert.equal(validateConfig(fractionalDelayTracker).valid, true,
  'explicit fractional delay accepts resident TBD');
for (const bad of ['true', 1, null]) {
  const invalid = clone(fractionalDelayTracker);
  invalid.process.detection.track_before_detect.fractional_delay = bad;
  assert.equal(validateConfig(invalid).valid, false, 'fractional delay requires a boolean');
}
const fractionalWithoutTbd = clone(fractionalDelayTracker);
fractionalWithoutTbd.process.detection.track_before_detect.enable = false;
assert.equal(validateConfig(fractionalWithoutTbd).valid, false,
  'fractional delay cannot silently bypass TBD');
for (const depth of [1, 2, 4, 8]) for (const acceleration of ['cpu', 'gpu', 'auto', 'gpu-resident']) {
  const candidate = clone(temporalTracker);
  candidate.process.performance.acceleration = acceleration;
  candidate.process.surveillance_fusion.mode = 'coherent_beam_bank';
  candidate.process.detection.track_before_detect.enable = depth > 2;
  candidate.process.detection.track_before_detect.max_cpis = Math.max(4, depth);
  candidate.process.detection.integration_cpis = Math.min(2, depth);
  Object.assign(candidate.process.tracker.initiate, {M: 4, N: 1 + depth * 3});
  candidate.process.tracker.delete = Math.max(1, depth - 1);
  const snapshot = JSON.stringify(candidate);
  const admission = validateConfig(candidate);
  assert.equal(admission.valid, true, `feasible ${depth}-CPI ${acceleration}: ${admission.errors.join('; ')}`);
  assert.equal(JSON.stringify(candidate), snapshot, 'validation must not migrate settings');
  if (depth > 1) {
    candidate.process.tracker.initiate.N--;
    assert.ok(validateConfig(candidate).errors.some(error => error.includes(`N must be >= ${1 + depth * 3}`)),
      'one-short confirmation window must fail with the derived recommendation');
    candidate.process.tracker.initiate.N++;
    candidate.process.tracker.delete = depth - 2;
    assert.ok(validateConfig(candidate).errors.some(error => error.includes(`delete must be >= ${depth - 1}`)),
      'short coast horizon must fail before startup');
  }
}
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
projectedBearing.process.detection.track_before_detect = {enable: true, max_cpis: 4, motion_bins: 1};
projectedBearing.process.tracker.initiate.N = 1 + 4 * (projectedBearing.process.tracker.initiate.M - 1);
projectedBearing.process.tracker.delete = Math.max(projectedBearing.process.tracker.delete, 3);
Object.assign(projectedBearing.process.surveillance_fusion,
  {mode: 'coherent_beam_bank', direct_path_null_db: 0});
Object.assign(projectedBearing.process.bearing, {enable: true,
  method: 'covariance_mvdr', reference_anchor: 'fixed_site_projected'});
delete projectedBearing.process.bearing.manifold_file;
Object.assign(projectedBearing.capture.device.array_geometry,
  {mapping_confirmed: true, geometry_confirmed: true, orientation_confirmed: true});
const projectedBearingValidation = validateConfig(projectedBearing);
assert.equal(projectedBearingValidation.valid, true,
  `projected fixed-site model accepts its explicit geometry/backend contract: ${projectedBearingValidation.errors.join('; ')}`);
const automaticProjectedBearing = clone(projectedBearing);
automaticProjectedBearing.process.performance.acceleration = 'auto';
assert.equal(validateConfig(automaticProjectedBearing).valid, true,
  'AUTO selects the required resident fixed-site projection');
assert.equal(automaticProjectedBearing.process.performance.acceleration, 'auto');
for (const state of [false, true]) for (const format of ['auto', 'blah2']) {
  const candidate = clone(projectedBearing);
  Object.assign(candidate.capture.replay, {state, format});
  const result = validateConfig(candidate);
  assert.equal(result.valid, true, `Projected bearing live/replay parity: ${result.errors.join('; ')}`);
  assert.deepEqual(candidate.process, projectedBearing.process, 'Validation cannot translate radar settings');
}
for (const profile of ['Usrp', 'RspDuo', 'HackRF', 'Kraken']) {
  const recorded = load(profile);
  recorded.process = clone(projectedBearing.process);
  recorded.network = clone(projectedBearing.network);
  recorded.capture.device.array_geometry = clone(projectedBearing.capture.device.array_geometry);
  recorded.capture.replay = {...clone(projectedBearing.capture.replay), channel_count: 5,
    sample_rate_hz: projectedBearing.capture.fs, frequency_hz: projectedBearing.capture.fc,
    reference_mode: 'array_eigenbeam', reference_channels: [4, 3, 2, 1, 0],
    surveillance_channels: [2, 0, 4, 1, 3]};
  // The recorded topology owns roles even if the dormant live profile is dedicated.
  recorded.process.reference_synthesis.mode = 'dedicated';
  recorded.process.reference_synthesis.channels = [0];
  const result = validateConfig(recorded);
  assert.equal(result.valid, true,
    `projected replay uses recorded topology under ${profile}: ${result.errors.join('; ')}`);
}
for (const mutate of [
  value => { value.capture.device.surveillance_channels = [0, 1, 2, 3]; },
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
for (const [name, accepted] of [['Usrp', true],
  ['HackRF', true], ['Kraken', false], ['config.yml', false]]) {
  const value = name === 'config.yml' ?
    configurationForReceiver('RspDuo', load(name)) : load(name);
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
{
  const value = clone(sixChannel);
  value.truth.adsb.sources = ['local:readsb', 'https://adsb-backup.example/tar1090'];
  value.truth.adsb.source_timeout = .75;
  assert.equal(validateConfig(value).valid, true, 'Accept bounded multi-source ADS-B configuration');

  const unchanged = clone(value);
  assert.equal(validateConfig(unchanged, value).valid, true,
    'An unchanged ADS-B source array must survive an unrelated Apply');
  unchanged.truth.adsb.sources[1] = 'https://different.example/tar1090';
  assert.equal(validateConfig(unchanged, value).valid, true,
    'The structured ADS-B source editor must be able to update its backing array');
}
for (const sources of [[], Array(9).fill('local:readsb'),
  ['local:readsb', 'local:readsb'], ['auto', 'https://adsb.example'],
  ['receiver.example:8080', 'http://receiver.example:8080/'],
  ['local:readsb', 'file:///etc/passwd'], ['local:readsb', 7]]) {
  const value = clone(sixChannel); value.truth.adsb.sources = sources;
  assert.equal(validateConfig(value).valid, false,
    `Reject ADS-B sources: ${JSON.stringify(sources)}`);
}
for (const timeout of [0, 10.1, '1']) {
  const value = clone(sixChannel); value.truth.adsb.source_timeout = timeout;
  assert.equal(validateConfig(value).valid, false, `Reject ADS-B source timeout: ${timeout}`);
}
for (const name of ['config.yml', 'HackRF', 'Usrp', 'Kraken']) {
  for (const mode of ['auto', 'cpu', 'gpu', 'cuda', '', 1, true, null]) {
    const value = name === 'config.yml' ?
      configurationForReceiver('RspDuo', load(name)) : load(name);
    value.process.performance = {...value.process.performance, acceleration: mode};
    assert.equal(validateConfig(value).valid, ['auto', 'cpu', 'gpu'].includes(mode),
      `${name}: acceleration ${JSON.stringify(mode)}`);
  }
}
const profiles = getDeviceProfiles();
assert.deepEqual(profiles.map(profile => profile.type).sort(),
  ['HackRF', 'Kraken', 'RspDuo', 'Usrp']);
const usrpProfile = profiles.find(profile => profile.type === 'Usrp');
assert.equal(usrpProfile.device.address, 'type=b200',
  'The B210 profile must use a UHD USB selector, not a hostname-like bare token');
assert.equal(load('Usrp').capture.device.address, 'type=b200',
  'The installed USRP example must select the B200 family');
for (const type of ['Usrp', 'RspDuo']) {
  const paired = configurationForReceiver(type, load('config.yml'));
  delete paired.process.reference_synthesis;
  assert.equal(validateConfig(paired).valid, true,
    `${type} legacy configuration defaults to dedicated input zero`);
  paired.process.reference_synthesis = {mode: 'dedicated', channels: [7]};
  assert.equal(validateConfig(paired).valid, false,
    `${type} rejects a reference input outside its coherent channel count`);
}
for (const profile of ['Usrp', 'RspDuo']) {
  const residentDedicated = load(profile);
  residentDedicated.process.performance.acceleration = 'gpu-resident';
  residentDedicated.process.tracker.gpu_capacity = 256;
  assert.equal(validateConfig(residentDedicated).valid, true,
    `${profile} admits the shared resident pipeline with a dedicated reference`);
  for (const channels of [[], [0, 1], [2]]) {
    const invalid = clone(residentDedicated);
    invalid.process.reference_synthesis = {mode: 'dedicated', channels};
    assert.equal(validateConfig(invalid).valid, false,
      `${profile} rejects malformed dedicated-reference resident topology`);
  }
}
const residentHackRfReversed = load('HackRF');
residentHackRfReversed.process.performance.acceleration = 'gpu-resident';
residentHackRfReversed.process.tracker.gpu_capacity = 256;
residentHackRfReversed.process.reference_synthesis = {
  mode: 'dedicated', channels: [1], analysis_samples: 32768,
  analysis_interval: 10, power_iterations: 12,
  covariance_smoothing: .8, diagonal_loading: .001};
assert.equal(validateConfig(residentHackRfReversed).valid, true,
  'HackRF dedicated roles are explicit even though array coherence is unavailable');
const residentHackRfOutOfRange = clone(residentHackRfReversed);
residentHackRfOutOfRange.process.reference_synthesis.channels = [8];
assert.equal(validateConfig(residentHackRfOutOfRange).valid, false,
  'HackRF rejects a dedicated reference outside its two captured inputs');

const residentPartialArray = clone(sixChannel);
residentPartialArray.process.performance.acceleration = 'gpu-resident';
residentPartialArray.process.tracker.gpu_capacity = 256;
residentPartialArray.process.reference_synthesis.channels = [0, 1];
residentPartialArray.capture.device.surveillance_channels = [0, 1];
assert.equal(validateConfig(residentPartialArray).valid, false,
  'resident array reference must map every canonical capture input identically');
for (const address of ['localhost', 'b200', 'type=', '=b200', 'type=b200,serial',
  'type=b\t200', 'type=b200,type=b210']) {
  const invalid = load('Usrp');
  invalid.capture.device.address = address;
  assert.equal(validateConfig(invalid).valid, false,
    `Reject malformed UHD device arguments: ${address}`);
}
for (const address of ['type=b200', 'serial=31ABC42', 'addr=192.0.2.1',
  'type=b200,serial=31ABC42']) {
  const valid = load('Usrp');
  valid.capture.device.address = address;
  assert.equal(validateConfig(valid).valid, true,
    `Accept UHD key=value device arguments: ${address}`);
}

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
const rspDuoKrakenReplay = configurationForReceiver('RspDuo', load('config.yml'));
const rspDuoPerformance = clone(rspDuoKrakenReplay.process.performance);
rspDuoKrakenReplay.process = clone(coherentFusion.process);
rspDuoKrakenReplay.process.performance = rspDuoPerformance;
rspDuoKrakenReplay.capture.device.array_geometry = clone(coherentFusion.capture.device.array_geometry);
rspDuoKrakenReplay.capture.replay = {...rspDuoKrakenReplay.capture.replay, state: true,
  file: '/tmp/kraken-recording.blah2iq', format: 'auto', sample_rate_hz: 2400000,
  frequency_hz: 509000000, channel_count: 5, reference_channel: 0,
  surveillance_channels: [0, 1, 2, 3, 4], reference_channels: [0, 1, 2, 3, 4],
  reference_mode: 'array_eigenbeam'};
assert.equal(validateConfig(rspDuoKrakenReplay).valid, true,
  `Kraken replay on an RSPduo host: ${validateConfig(rspDuoKrakenReplay).errors.join('; ')}`);

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

const ownershipDirectory = fs.mkdtempSync(path.join(os.tmpdir(), 'blah2-config-owner-test-'));
const ownershipFilename = path.join(ownershipDirectory, 'config.yml');
const originalStatSync = fs.statSync;
const originalChownSync = fs.chownSync;
const originalGetuid = process.getuid;
try {
  fs.writeFileSync(ownershipFilename, 'original: true\n', {mode: 0o660});
  const original = originalStatSync(ownershipFilename);
  const calls = [];
  fs.statSync = target => target === ownershipFilename ?
    {...original, uid: 0, gid: original.gid} : originalStatSync(target);
  process.getuid = () => 12345;
  fs.chownSync = (target, uid, gid) => {
    calls.push([target, uid, gid]);
    if (uid === 0) {
      const error = new Error('operation not permitted'); error.code = 'EPERM'; throw error;
    }
    return originalChownSync(target, originalGetuid(), gid);
  };
  writeConfigAtomically(ownershipFilename, sixChannel);
  assert.deepEqual(calls.map(([, uid, gid]) => [uid, gid]),
    [[0, original.gid], [12345, original.gid]],
    'EPERM retaining root ownership must fall back to the service owner in the same group');
  const replaced = originalStatSync(ownershipFilename);
  assert.equal(replaced.gid, original.gid);
  assert.equal(replaced.mode & 0o777, original.mode & 0o777);
  assert.deepEqual(yaml.load(fs.readFileSync(ownershipFilename, 'utf8')), sixChannel);

  fs.chownSync = () => {
    const error = new Error('permission denied'); error.code = 'EACCES'; throw error;
  };
  const rejectedOwnerChange = clone(sixChannel); rejectedOwnerChange.save.map = true;
  assert.throws(() => writeConfigAtomically(ownershipFilename, rejectedOwnerChange),
    error => error.code === 'EACCES', 'Unrelated ownership errors must fail closed');
  assert.equal(yaml.load(fs.readFileSync(ownershipFilename, 'utf8')).save.map, sixChannel.save.map);
} finally {
  fs.statSync = originalStatSync;
  fs.chownSync = originalChownSync;
  process.getuid = originalGetuid;
  fs.rmSync(ownershipDirectory, {recursive: true});
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
  // This test's signed-lag limits are native 2.4 MHz samples, not the
  // independently configured physical coordinates of a fresh profile.
  boundary.process.delay_reference_sample_rate_hz = boundary.capture.fs;
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
for (const provenance of ['legacy-unqualified', 'decision-grade']) {
  const replay = clone(sixChannel); replay.capture.replay.provenance = provenance;
  assert.equal(validateConfig(replay).valid, true, provenance);
}
const invalidProvenance = clone(sixChannel);
invalidProvenance.capture.replay.provenance = 'best-effort';
assert.equal(validateConfig(invalidProvenance).valid, false,
  'Replay must not silently invent an unqualified provenance mode');
const residentSnapshotReplay = clone(projectedBearing);
residentSnapshotReplay.capture.replay.state = true;
residentSnapshotReplay.process.performance.acceleration = 'gpu-resident';
residentSnapshotReplay.process.tracker.gpu_capacity = 256;
residentSnapshotReplay.capture.replay.resident_snapshot = {enabled: true,
  output_directory: '/var/tmp/vectorwarp-resident-snapshots', max_frames: 20,
  max_bytes: 268435456, source_sha256: 'a'.repeat(64),
  rf_profile_sha256: 'B'.repeat(64), rf_profile_file: '/var/tmp/replay.json'};
assert.equal(validateConfig(residentSnapshotReplay).valid, true,
  'Bounded resident snapshot export is accepted for offline GPU-resident replay');
const automaticSnapshotReplay = clone(residentSnapshotReplay);
automaticSnapshotReplay.process.performance.acceleration = 'auto';
assert.equal(validateConfig(automaticSnapshotReplay).valid, true,
  'AUTO selects required resident snapshot support without altering replay constraints');
for (const mutate of [
  value => { value.capture.replay.state = false; },
  value => { value.process.performance.acceleration = 'cpu'; },
  value => { value.capture.replay.resident_snapshot.output_directory = 'relative'; },
  value => { value.capture.replay.resident_snapshot.source_sha256 = 'abc'; },
  value => { value.capture.replay.resident_snapshot.rf_profile_file = 'relative'; },
  value => { value.capture.replay.resident_snapshot.max_frames = 129; },
  value => { value.capture.replay.resident_snapshot.max_bytes = 536870913; },
  value => { value.process.surveillance_fusion.mode = 'noncoherent'; },
  value => { value.process.multi_illuminator.enabled = true; }
]) {
  const invalid = clone(residentSnapshotReplay); mutate(invalid);
  assert.equal(validateConfig(invalid).valid, false,
    'Resident snapshot export fails closed outside its replay/provenance bounds');
}
const excessiveAircraftClip = clone(sixChannel);
excessiveAircraftClip.capture.aircraft_clips = {enabled: true, pre_seconds: 6, post_seconds: 5};
assert.equal(validateConfig(excessiveAircraftClip).valid, false,
  'Automatic aircraft IQ clips must fit the bounded ten-second recorder');
const portableAircraftClip = configurationForReceiver('Usrp', load('config.yml'));
portableAircraftClip.capture.aircraft_clips = {enabled: true, pre_seconds: 2,
  post_seconds: 3, minimum_slant_range_km: 5, maximum_slant_range_km: 80,
  minimum_elevation_deg: 1, maximum_elevation_deg: 45,
  minimum_interval_seconds: 10};
assert.equal(validateConfig(portableAircraftClip).valid, true,
  'Automatic aircraft IQ clips are valid for a non-Kraken receiver');
console.log(`Exhaustive type checks passed for ${checked} device-setting leaves; boundary and cross-field tests passed.`);

for (const name of ['HackRF', 'Usrp',
  'Kraken', 'RspDuo']) {
  const candidate = load(name);
  candidate.process.tracker.initiate.doppler_ridge_clutter.enable = true;
  candidate.process.performance.acceleration = 'gpu-resident';
  assert.equal(validateConfig(candidate).valid, true,
    `${name} resident frontend still executes the host ridge birth filter`);
  candidate.process.tracker.enable = false;
  assert.ok(validateConfig(candidate).errors.includes(
    'Doppler ridge birth filtering requires enabled detection and tracking'));
}
for (const base of [coherentFusion, coherentLanes, residentSnapshotReplay]) {
  const candidate = clone(base);
  candidate.process.performance.acceleration = 'gpu-resident';
  candidate.process.tracker.initiate.doppler_ridge_clutter.enable = true;
  assert.ok(validateConfig(candidate).errors.includes(
    'Doppler ridge birth filter is unavailable in resident products'));
}
