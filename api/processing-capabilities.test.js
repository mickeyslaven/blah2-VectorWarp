'use strict';

const assert = require('assert/strict');
const {processingCapabilities, residentReferenceCompatible, regularSurveillancePolygon} = require('./processing-capabilities');
const {validateConfig} = require('./config-manager');

const load = type => require('./config-store').configurationForReceiver(type);
function coherentPair(name) {
  const config = load(name);
  config.capture.device.array_geometry = {
    shape: 'custom', units: 'm', coordinate_frame: 'array_xy_right_handed',
    mapping_confirmed: true, geometry_confirmed: true, orientation_confirmed: true,
    x_axis_bearing_deg_true: 0, bearing_channels: [0, 1],
    elements: [0, 1].map(daq_channel => ({id: `channel-${daq_channel}`,
      daq_channel, receiver_serial: `test-${daq_channel}`, position: [.2 * daq_channel, 0, 0]}))
  };
  config.process.reference_synthesis = {mode: 'array_eigenbeam', channels: [0, 1],
    analysis_samples: 32768, analysis_interval: 10, power_iterations: 12,
    covariance_smoothing: .8, diagonal_loading: .001};
  config.process.performance.acceleration = 'cpu';
  config.process.surveillance_fusion = {mode: 'coherent_beam_bank', beams: 12};
  config.process.bearing = load('Usrp').process.bearing;
  return config;
}

for (const profile of ['Usrp', 'RspDuo']) {
  const config = coherentPair(profile);
  const result = validateConfig(config);
  assert.equal(result.valid, true, `${profile}: ${result.errors.join('; ')}`);
  assert.deepEqual(processingCapabilities(config).surveillanceChannels, [0, 1]);
  for (const mutate of [
    value => { value.process.reference_synthesis.channels = [0]; },
    value => { value.process.reference_synthesis.channels = [0, 2]; },
    value => { value.process.reference_synthesis.channels = [0, 0]; },
    value => { value.capture.device.array_geometry.elements.pop(); },
    value => { value.capture.device.array_geometry.elements[1].position = [0, 0, 0]; },
    value => { value.capture.device.array_geometry.x_axis_bearing_deg_true = NaN; },
    value => { value.capture.device.array_geometry.mapping_confirmed = false; },
    value => { value.capture.device.array_geometry.geometry_confirmed = false; },
    value => { value.capture.device.array_geometry.orientation_confirmed = false; },
    value => { value.process.surveillance_fusion.mode = 'noise_whitened_beam_bank'; },
    value => { value.process.bearing.enable = true; },
    value => { value.process.bearing.manifold_recorder.enabled = true; value.truth.adsb.enabled = true; },
  ]) {
    const invalid = structuredClone(config); mutate(invalid);
    assert.equal(validateConfig(invalid).valid, false,
      `${profile} must not bypass topology, geometry, GPU, or input-metadata requirements`);
  }
}

const noncoherent = coherentPair('HackRF');
noncoherent.capture.device.serial = ['0001', '0002'];
assert.equal(processingCapabilities(noncoherent).coherent, false);
assert.equal(validateConfig(noncoherent).valid, false,
  'Synchronized sample clocks do not establish coherent inter-channel phase');
noncoherent.process.surveillance_fusion.mode = 'noncoherent';
assert.equal(validateConfig(noncoherent).valid, false,
  'A noncoherent adapter must not silently reinterpret array_eigenbeam as a dedicated reference');

const replay = coherentPair('Usrp');
replay.capture.replay = {...replay.capture.replay, state: true, channel_count: 2,
  reference_mode: 'array_eigenbeam', reference_channels: [0, 1], surveillance_channels: [0, 1]};
assert.equal(validateConfig(replay).valid, true);
const contradictoryDedicated = load('Usrp');
contradictoryDedicated.capture.replay = {...contradictoryDedicated.capture.replay,
  state: true, channel_count: 2, reference_mode: 'dedicated', reference_channel: 0,
  reference_channels: [1], surveillance_channels: [1]};
assert.equal(processingCapabilities(contradictoryDedicated).coherent, false);
assert.equal(validateConfig(contradictoryDedicated).valid, false,
  'Dedicated replay role declarations must have one canonical interpretation');
const unconfigured = load('Usrp');
assert.equal(processingCapabilities(unconfigured).coherentSurveillance, false,
  'A dedicated reference plus one surveillance input cannot form a beam bank');

for (const profile of ['Usrp', 'RspDuo',
  'HackRF', 'Kraken']) {
  for (const acceleration of ['cpu', 'auto', 'gpu']) {
    const config = load(profile);
    if (config.capture.device.type === 'HackRF') config.capture.device.serial = ['0001', '0002'];
    config.process.performance.acceleration = acceleration;
    config.process.detection.integration_cpis = 2;
    config.process.detection.integration_motion_bins = 1;
    const result = validateConfig(config);
    assert.equal(result.valid, true, `${profile}/${acceleration}: ${result.errors.join('; ')}`);
  }
  const recorded = load(profile);
  recorded.capture.replay = {...recorded.capture.replay, state: true,
    channel_count: 3, reference_mode: 'dedicated', reference_channels: [2]};
  const topology = processingCapabilities(recorded);
  assert.equal(topology.validTopology, true);
  assert.equal(topology.referenceChannel, 2);
  assert.deepEqual(topology.surveillanceChannels, [0, 1]);
  assert.equal(topology.coherent, false, 'Dedicated recording does not certify an array');
  recorded.capture.replay.reference_channel = 0;
  assert.equal(processingCapabilities(recorded).validTopology, false);
  recorded.capture.replay.reference_mode = 'array_eigenbeam';
  recorded.capture.replay.reference_channels = [2, 0, 1];
  recorded.capture.replay.surveillance_channels = [1, 2, 0];
  const array = processingCapabilities(recorded);
  assert.equal(array.coherentSurveillance, true,
    'Recorded coherent topology is independent of the dormant live adapter');
  assert.deepEqual(array.referenceChannels, [0, 1, 2]);
  assert.deepEqual(array.surveillanceChannels, [0, 1, 2]);
  assert.deepEqual(recorded.capture.replay.reference_channels, [2, 0, 1]);
}
// Geometry admission is shared by the API validator and browser. Preserve the
// native contract's physical channel order, planar/radial/angular tolerances,
// and rejection of a label that does not describe the actual coordinates.
for (const count of [3, 4, 5, 8]) {
  const channels = Array.from({length: count}, (_, channel) => channel);
  for (const direction of [-1, 1]) {
    const geometry = {shape: 'regular_polygon', elements: channels.map(daq_channel => {
      const phase = direction * 2 * Math.PI * daq_channel / count + .37;
      return {daq_channel, position: [2 + .5 * Math.cos(phase), -3 + .5 * Math.sin(phase), .8]};
    })};
    assert.equal(regularSurveillancePolygon(geometry, channels), true);
    assert.equal(regularSurveillancePolygon(geometry, [...channels].reverse()), true);
    const near = structuredClone(geometry); near.elements[0].position[0] += 1e-7;
    assert.equal(regularSurveillancePolygon(near, channels), true);
    for (const mutate of [
      value => { value.shape = 'custom'; },
      value => { value.elements.pop(); },
      value => { value.elements[0].position[0] += .001; },
      value => { value.elements[0].position[2] += .001; },
      value => { value.elements[0].position[0] = Infinity; },
      value => { value.elements[0].position = value.elements[1].position; }
    ]) {
      const invalid = structuredClone(geometry); mutate(invalid);
      assert.equal(regularSurveillancePolygon(invalid, channels), false);
    }
    assert.equal(regularSurveillancePolygon(geometry, channels.slice(0, 2)), false);
    const topology = {referenceMode: 'array_eigenbeam', coherent: true,
      referenceChannels: channels, surveillanceChannels: channels, channelCount: count};
    assert.equal(residentReferenceCompatible(topology), true);
    assert.equal(residentReferenceCompatible({...topology, coherent: false}), false);
    assert.equal(residentReferenceCompatible({...topology, referenceChannels: channels.slice(1)}), false);
    assert.equal(residentReferenceCompatible({...topology, surveillanceChannels: [...channels].reverse()}), false);
  }
}
assert.equal(regularSurveillancePolygon(undefined, []), false);
for (const reference of [0, 2]) {
  const topology = {referenceMode: 'dedicated', coherent: false, channelCount: 3,
    referenceChannels: [reference], surveillanceChannels: [0, 1, 2].filter(channel => channel !== reference)};
  assert.equal(residentReferenceCompatible(topology), true);
  assert.equal(residentReferenceCompatible({...topology, surveillanceChannels: [reference]}), false);
  assert.equal(residentReferenceCompatible({...topology, surveillanceChannels: []}), false);
}
console.log('Processing capabilities: topology, shared admission geometry, transport, gain and GPU prerequisites fail closed.');
