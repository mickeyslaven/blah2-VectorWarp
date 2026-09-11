'use strict';

const assert = require('assert');
const {ARRAY_CONTRACT, evaluateKrakenArray, localAzimuthToTrueBearing} =
  require('./kraken-array');

const clone = value => JSON.parse(JSON.stringify(value));
const point = (radius, degrees, z = 0) => {
  const angle = degrees * Math.PI / 180;
  return [radius * Math.cos(angle), radius * Math.sin(angle), z];
};
const element = (id, daq, position) =>
  ({id, daq_channel: daq, receiver_serial: String(1000 + daq), position});
const baseConfig = () => ({capture: {fc: 100000000, device: {
  type: 'Kraken', channel_count: 5, reference_channel: 2,
  surveillance_channels: [4, 0, 3, 1],
  array_geometry: {
    shape: 'cross', units: 'm', coordinate_frame: ARRAY_CONTRACT.coordinateFrame,
    mapping_confirmed: true, geometry_confirmed: true,
    orientation_confirmed: true, x_axis_bearing_deg_true: 30,
    bearing_channels: [4, 0, 3, 1],
    elements: [
      element('east', 0, [.5, 0, 0]), element('reference', 2, [2, 2, .2]),
      element('west', 3, [-.5, 0, 0]), element('south', 1, [0, -.5, 0]),
      element('north', 4, [0, .5, 0])
    ]
  }
}}, process: {reference_synthesis: {mode: 'dedicated', channels: [2]}}});
const codes = result => result.issues.map(issue => issue.code);

const cross = evaluateKrakenArray(baseConfig(), {num_channels: 5,
  receiver_serials: ['spoofed'], array_geometry: {matched: true}});
assert.equal(cross.valid, true);
assert.equal(cross.state, 'configured-unverified');
assert.equal(cross.bearingImplemented, false);
assert.equal(cross.relativeBearingEligible, false);
assert.equal(cross.worldBearingEligible, false);
assert.equal(cross.verification.physicalAgreement.state, 'unverified');
assert.equal(cross.verification.daqElementMapping.state, 'operator-asserted');
assert.equal(cross.trackerAllowed, false);
assert.deepEqual(cross.mapping.filter(item => item.bearing).map(item => item.daqChannel),
  [0, 3, 1, 4], 'Element order does not rewrite the explicit DAQ mapping');
assert.equal(cross.mapping.find(item => item.reference).bearing, false,
  'Dedicated reference is excluded from DoA while retaining its correlation role');
assert.equal(cross.verification.receiverSerials.state, 'operator-metadata',
  'Suite status cannot elevate serial/config echoes to verified wiring');
assert.equal(localAzimuthToTrueBearing(90, 30), 300,
  'Local CCW azimuth is converted to clockwise-from-true-north bearing');

const unknown = baseConfig();
delete unknown.capture.device.array_geometry;
const unknownResult = evaluateKrakenArray(unknown, {num_channels: 5});
assert.equal(unknownResult.valid, true);
assert.equal(unknownResult.state, 'unknown');
assert.equal(unknownResult.worldBearingEligible, false,
  'Legacy config remains valid without inventing pentagon or other geometry');

for (let count = 2; count <= 8; count++) {
  const config = baseConfig();
  config.capture.device.channel_count = count;
  config.capture.device.reference_channel = 0;
  config.capture.device.surveillance_channels = Array.from({length: count}, (_, i) => i);
  config.process.reference_synthesis = {mode: 'array_eigenbeam',
    channels: Array.from({length: count}, (_, i) => i)};
  Object.assign(config.capture.device.array_geometry, {shape: 'custom',
    bearing_channels: Array.from({length: count}, (_, i) => i),
    elements: Array.from({length: count}, (_, i) => element(`element-${i}`, i,
      [i, i % 2 ? .1 : 0, 0]))});
  assert.equal(evaluateKrakenArray(config, {num_channels: count}).valid, true,
    `General ${count}-channel explicit geometry is accepted`);
}

// A five-position physical pentagon may dedicate one vertex as reference. Its
// remaining four-element bearing manifold is explicitly custom, not silently
// relabelled as a five-bearing regular polygon.
const pentagonWithReference = baseConfig();
Object.assign(pentagonWithReference.capture.device.array_geometry, {
  shape: 'custom', bearing_channels: [1, 2, 3, 4],
  elements: Array.from({length: 5}, (_, i) => element(`pentagon-${i}`, i,
    point(.5, i * 72)))
});
pentagonWithReference.capture.device.reference_channel = 0;
pentagonWithReference.capture.device.surveillance_channels = [1, 2, 3, 4];
pentagonWithReference.process.reference_synthesis.channels = [0];
assert.equal(evaluateKrakenArray(pentagonWithReference, {num_channels: 5}).valid,
  true, 'Five physical pentagon positions can include one dedicated reference');

const fiveBearingPlusReference = clone(pentagonWithReference);
fiveBearingPlusReference.capture.device.channel_count = 6;
fiveBearingPlusReference.capture.device.surveillance_channels = [1, 2, 3, 4, 5];
Object.assign(fiveBearingPlusReference.capture.device.array_geometry, {
  shape: 'regular_polygon', bearing_channels: [1, 2, 3, 4, 5],
  elements: [element('reference', 0, [2, 2, .2]),
    ...Array.from({length: 5}, (_, i) => element(`bearing-${i}`, i + 1,
      point(.5, i * 72)))]
});
assert.equal(evaluateKrakenArray(fiveBearingPlusReference,
  {num_channels: 6}).valid, true,
'Five bearing elements plus a distinct dedicated reference requires and accepts six channels');
const impossibleFive = clone(fiveBearingPlusReference);
impossibleFive.capture.device.channel_count = 5;
assert.equal(evaluateKrakenArray(impossibleFive, {num_channels: 5}).valid, false);

for (const mutate of [
  config => { config.capture.device.array_geometry.elements[1].daq_channel = 0; },
  config => { config.capture.device.array_geometry.elements.pop(); },
  config => { config.capture.device.array_geometry.elements[1].id = 'east'; },
  config => { config.capture.device.array_geometry.elements[1].position = [.5, 0, 0]; },
  config => { config.capture.device.array_geometry.elements[1].position = [NaN, 0, 0]; },
  config => { config.capture.device.array_geometry.bearing_channels = [4, 4, 3, 1]; }
]) {
  const malformed = baseConfig(); mutate(malformed);
  assert.equal(evaluateKrakenArray(malformed, {num_channels: 5}).valid, false,
    'Repeated, missing, nonfinite, or duplicate geometry/mapping is rejected');
}

const countMismatch = evaluateKrakenArray(baseConfig(), {num_channels: 4});
assert.ok(codes(countMismatch).includes('live-channel-count'));
assert.equal(countMismatch.worldBearingEligible, false);
assert.equal(evaluateKrakenArray(baseConfig(), {actual: {channels: 5}})
  .verification.channelCount.state, 'reported-match',
'Normalized upstream status is only a count observation, without freshness or physical agreement');

const unconfirmed = baseConfig();
unconfirmed.capture.device.array_geometry.mapping_confirmed = false;
unconfirmed.capture.device.array_geometry.geometry_confirmed = false;
unconfirmed.capture.device.array_geometry.orientation_confirmed = false;
unconfirmed.capture.device.array_geometry.x_axis_bearing_deg_true = null;
const unconfirmedResult = evaluateKrakenArray(unconfirmed, {num_channels: 5});
assert.equal(unconfirmedResult.valid, true);
assert.equal(unconfirmedResult.relativeBearingEligible, false);
assert.equal(unconfirmedResult.worldBearingEligible, false);

const ula = baseConfig();
Object.assign(ula.capture.device.array_geometry, {shape: 'ula', ula_half_plane: 'both',
  bearing_channels: [4, 0, 3, 1], elements: [element('reference', 2, [0, 1, 0]),
    element('u0', 4, [0, 0, 0]), element('u1', 0, [2, 0, 0]),
    element('u2', 3, [4, 0, 0]), element('u3', 1, [6, 0, 0])]});
const ulaResult = evaluateKrakenArray(ula, {num_channels: 5});
assert.equal(ulaResult.valid, true, 'Wide but collinear real ULA remains valid');
assert.ok(codes(ulaResult).includes('ula-front-back-ambiguity'));
assert.ok(codes(ulaResult).includes('spacing-alias-risk'),
  'Wide spacing warns about grating lobes instead of rejecting the layout');
const constrainedUla = clone(ula);
constrainedUla.capture.device.array_geometry.ula_half_plane = 'positive_y';
assert.ok(codes(evaluateKrakenArray(constrainedUla, {num_channels: 5}))
  .includes('ula-half-plane-constraint'),
'Selected ULA lobe remains labelled an operator constraint');
const bentUla = clone(ula);
bentUla.capture.device.array_geometry.elements.find(item => item.id === 'u2').position[1] = 1;
assert.equal(evaluateKrakenArray(bentUla, {num_channels: 5}).valid, false);

const withAdsb = baseConfig();
withAdsb.truth = {adsb: {tar1090: 'http://truth.example/', aircraft: [{bearing: 30}]}};
assert.deepEqual(evaluateKrakenArray(withAdsb, {num_channels: 5}), cross,
  'ADS-B truth cannot influence geometry acceptance or resolve ambiguity');

const misleadingDedicated = baseConfig();
misleadingDedicated.process.reference_synthesis.channels = [0, 1];
assert.ok(codes(evaluateKrakenArray(misleadingDedicated, {num_channels: 5}))
  .includes('dedicated-synthesis-mismatch'),
'Hand-edited dedicated synthesis cannot claim channels that the C++ path ignores');

console.log('Kraken 2-8 channel geometry, mapping, role, orientation, ambiguity and truth-boundary tests passed.');
