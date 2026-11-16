'use strict';

const assert = require('node:assert/strict');
const {diagnoseArrayChannels, diagnoseUpstreamArrayChannels} = require('./array-channel-health');
const {manifoldSampleFingerprint} = require('./manifold-recorder');

const report = diagnoseArrayChannels([
  {id: '0', gainDb: 3.7, rmsFraction: .122, p999Fraction: .380,
    directAnchorMagnitude: .48},
  {id: '1', gainDb: 3.7, rmsFraction: .150, p999Fraction: .459,
    directAnchorMagnitude: .52},
  {id: '2', gainDb: 15.7, rmsFraction: .148, p999Fraction: .475,
    directAnchorMagnitude: .0845},
  {id: '3', gainDb: 3.7, rmsFraction: .152, p999Fraction: .467,
    directAnchorMagnitude: .50},
  {id: '4', gainDb: 3.7, rmsFraction: .184, p999Fraction: .553,
    directAnchorMagnitude: .57}
]);
const suspect = report.channels.find(channel => channel.id === '2');
assert(suspect.directRelativeDb < -14);
assert.equal(suspect.digitizerRelativeDb > -1, true);
assert.deepEqual(suspect.flags.sort(),
  ['direct_path_attenuation', 'gain_compensation']);
assert.equal(suspect.recommendPhysicalSwapTest, true);
assert.equal(report.automaticWeightingAllowed, false);
assert.equal(report.automaticQuarantineAllowed, false);
assert.equal(report.channels.some(channel =>
  channel.flags.includes('clipping_risk')), false);

const allZero = diagnoseArrayChannels(Array.from({length: 5}, (_, id) => ({id,
  gainDb: 3.7, rmsFraction: 0, p999Fraction: 0, directAnchorMagnitude: 0})));
assert.equal(allZero.arrayUsable, false);
assert.deepEqual(allZero.arrayFlags, ['all_channels_zero_or_dropout', 'array_unusable']);
assert(allZero.channels.every(channel => channel.flags.includes('absolute_zero_or_dropout')));

const incomplete = diagnoseArrayChannels([
  {id: 'a', gainDb: 3, rmsFraction: .1, p999Fraction: .2},
  {id: 'b', gainDb: Number.NaN, rmsFraction: -1, p999Fraction: 2,
    directAnchorMagnitude: Number.POSITIVE_INFINITY},
  {id: 'c', gainDb: 3, rmsFraction: .1, p999Fraction: .2, directAnchorMagnitude: .3}
]);
assert(incomplete.channels[0].flags.includes('missing_directAnchorMagnitude'));
assert(incomplete.channels[1].flags.includes('invalid_gainDb'));
assert(incomplete.channels[1].flags.includes('invalid_rmsFraction'));
assert(incomplete.channels[1].flags.includes('invalid_p999Fraction'));
assert(incomplete.channels[1].flags.includes('invalid_directAnchorMagnitude'));

const arbitrary = diagnoseArrayChannels(Array.from({length: 7}, (_, id) => ({id,
  gainDb: 5, rmsFraction: .2, p999Fraction: .4, directAnchorMagnitude: .5})));
assert.equal(arbitrary.channels.length, 7);
assert.equal(Object.hasOwn(arbitrary.channels[0], 'phaseStabilityDeg'), false,
  'phase stability must not be invented without a measured phase input');

const config = {capture: {fs: 2400000, fc: 509000000, device: {channel_count: 3,
  surveillance_channels: [0, 1, 2], heimdall: {gain: 3.7}, array_geometry: {
    x_axis_bearing_deg_true: 0, elements: [0, 1, 2].map(channel =>
      ({daq_channel: channel, position: [channel, 0, 0]}))}}},
  location: {rx: {latitude: 1, longitude: 2, altitude: 3},
    tx: {latitude: 4, longitude: 5, altitude: 6}}, process: {bearing: {}}};
const actual = {available: true, actual: {centerFrequency: 509000000,
  sampleRate: 2400000, calibrationState: 'CONVERGED', adcHeadroom: {complete: true,
    channels: [
      {channel: 0, generation: 11, actualGainDb: 3.7, rmsFraction: .12, p999Fraction: .4},
      {channel: 1, generation: 11, actualGainDb: 9.8, rmsFraction: .13, p999Fraction: .45},
      {channel: 2, generation: 11, actualGainDb: 3.7, rmsFraction: .14, p999Fraction: .5}
    ]}}};
const candidate = {frequency_hz: 509000000, sample_rate_hz: 2400000,
  config_fingerprint: manifoldSampleFingerprint(config, [3.7, 9.8, 3.7]), channel_mapping: [0, 1, 2],
  direct_anchor: {response: [[.4, 0], [.1, 0], [.5, 0]]}};
const adapted = diagnoseUpstreamArrayChannels({config, upstreamStatus: actual, candidate,
  configRevision: 'persisted-config', loadedConfigRevision: 'persisted-config',
  configFingerprint: 'settings-only-fingerprint'});
assert.equal(adapted.channels.length, 3);
assert.equal(adapted.channels[1].id, '1');
assert(adapted.channels[1].flags.includes('direct_path_attenuation'));
assert.equal(adapted.epoch.configRevision, 'persisted-config');
assert.equal(adapted.epoch.loadedConfigRevision, 'persisted-config');
assert.equal(adapted.epoch.configFingerprint, 'settings-only-fingerprint');
assert.deepEqual(adapted.epoch.upstream.adcGeneration, [11, 11, 11]);
assert.equal(adapted.epoch.directAnchor.available, true);
const reorderedActual = JSON.parse(JSON.stringify(actual));
reorderedActual.actual.adcHeadroom.channels.reverse();
assert.equal(diagnoseUpstreamArrayChannels({config, upstreamStatus: reorderedActual,
  candidate}).epoch.directAnchor.available, true,
'upstream transport order must not change measured manifold identity');
assert.deepEqual(adapted.channels[1].directAnchorResponse, [.1, 0]);
assert.equal(adapted.channels[1].directAnchorPhaseDeg, 0);

const retuned = JSON.parse(JSON.stringify(actual));
retuned.actual.centerFrequency += 100000;
const acquisitionMismatch = diagnoseUpstreamArrayChannels({config, upstreamStatus: retuned, candidate,
  configFingerprint: 'settings-only-fingerprint'});
assert.equal(acquisitionMismatch.arrayUsable, false,
  'ADC/direct evidence from a different tuning must fail closed');
assert(acquisitionMismatch.arrayFlags.includes('upstream_acquisition_mismatch'));
assert(acquisitionMismatch.channels.every(channel =>
  channel.flags.includes('upstream_acquisition_mismatch')));
assert.equal(acquisitionMismatch.epoch.upstream.acquisitionMatchesConfig, false);
assert.equal(acquisitionMismatch.epoch.directAnchor.available, false);
assert.equal(acquisitionMismatch.epoch.directAnchor.reason, 'upstream_acquisition_mismatch');

const rateMismatch = JSON.parse(JSON.stringify(actual));
rateMismatch.actual.sampleRate -= 1;
const sampleRateMismatch = diagnoseUpstreamArrayChannels({config, upstreamStatus: rateMismatch,
  candidate, configFingerprint: 'settings-only-fingerprint'});
assert.equal(sampleRateMismatch.arrayUsable, false);
assert.equal(sampleRateMismatch.epoch.directAnchor.reason, 'upstream_acquisition_mismatch');

const splitGenerations = JSON.parse(JSON.stringify(actual));
splitGenerations.actual.adcHeadroom.channels[1].generation = 12;
const incoherent = diagnoseUpstreamArrayChannels({config, upstreamStatus: splitGenerations, candidate,
  configFingerprint: 'settings-only-fingerprint'});
assert.equal(incoherent.arrayUsable, false,
  'mixed ADC generations must not form a coherent array report');
assert(incoherent.arrayFlags.includes('incoherent_adc_generation'));
assert(incoherent.channels.every(channel => channel.flags.includes('incoherent_adc_generation')));
assert.equal(incoherent.epoch.upstream.adcGenerationCoherent, false);
assert.equal(incoherent.epoch.directAnchor.available, false);
assert.equal(incoherent.epoch.directAnchor.reason, 'incoherent_adc_generation');

const invalidGeneration = JSON.parse(JSON.stringify(actual));
invalidGeneration.actual.adcHeadroom.channels[1].generation = '11';
const invalidGenerationReport = diagnoseUpstreamArrayChannels({config, upstreamStatus: invalidGeneration,
  candidate, configFingerprint: 'candidate-config'});
assert.equal(invalidGenerationReport.arrayUsable, false);
assert(invalidGenerationReport.arrayFlags.includes('incoherent_adc_generation'));

const recovering = JSON.parse(JSON.stringify(actual));
recovering.actual.calibrationState = 'RECOVERING';
recovering.matched = false;
recovering.actual.adcHeadroom.complete = false;
const recoveringReport = diagnoseUpstreamArrayChannels({config, upstreamStatus: recovering,
  candidate, configRevision: 'new', loadedConfigRevision: 'old',
  configFingerprint: 'settings-only-fingerprint'});
assert.equal(recoveringReport.arrayUsable, false);
assert.equal(recoveringReport.epoch.directAnchor.available, false);
assert.equal(recoveringReport.epoch.directAnchor.reason, 'upstream_epoch_not_ready');
assert(recoveringReport.arrayFlags.includes('calibration_not_converged'));
assert(recoveringReport.arrayFlags.includes('config_revision_mismatch'));
assert(recoveringReport.arrayFlags.includes('upstream_status_mismatch'));

const staleCandidate = diagnoseUpstreamArrayChannels({config, upstreamStatus: actual,
  candidate: {...candidate, config_fingerprint: 'different-manifold-identity'},
  configFingerprint: 'settings-only-fingerprint'});
assert.equal(staleCandidate.epoch.directAnchor.available, false);
assert.equal(staleCandidate.epoch.directAnchor.reason, 'candidate_config_identity_mismatch');

const gainIdentityUnavailable = JSON.parse(JSON.stringify(actual));
delete gainIdentityUnavailable.actual.adcHeadroom.channels[1].actualGainDb;
const noIdentityReport = diagnoseUpstreamArrayChannels({config, upstreamStatus: gainIdentityUnavailable,
  candidate});
assert.equal(noIdentityReport.epoch.directAnchor.available, false);
assert.equal(noIdentityReport.epoch.directAnchor.reason, 'candidate_epoch_identity_unavailable');

const complexTelemetry = JSON.parse(JSON.stringify(actual));
complexTelemetry.actual.complexChannelTelemetry = {schema: 'vectorwarp-complex-channel-telemetry-v1',
  frequency_hz: 509000000, sample_rate_hz: 2400000, channels: [
    {channel: 0, generation: 11, response: [1, 0], phase_stability_deg: 1.5},
    {channel: 1, generation: 11, response: [0, 1], phase_stability_deg: 18},
    {channel: 2, generation: 11, response: [-1, 0], phase_stability_deg: 2}]};
const complexReport = diagnoseUpstreamArrayChannels({config, upstreamStatus: complexTelemetry, candidate});
assert.deepEqual(complexReport.channels[1].complexResponse, [0, 1]);
assert.equal(complexReport.channels[1].phaseStabilityDeg, 18);
assert(complexReport.channels[1].flags.includes('phase_instability'));
assert.equal(complexReport.epoch.complexChannelTelemetry.available, true);
const staleComplex = JSON.parse(JSON.stringify(complexTelemetry));
staleComplex.actual.complexChannelTelemetry.channels[1].generation = 12;
const staleComplexReport = diagnoseUpstreamArrayChannels({config, upstreamStatus: staleComplex, candidate});
assert.equal(Object.hasOwn(staleComplexReport.channels[1], 'phaseStabilityDeg'), false,
  'complex telemetry from another generation must not become a phase measurement');
assert.equal(staleComplexReport.epoch.complexChannelTelemetry.reason,
  'complex_channel_telemetry_invalid');

const unavailable = diagnoseUpstreamArrayChannels({config, upstreamStatus: {available: false},
  configRevision: 'config'});
assert.equal(unavailable.arrayUsable, false);
assert(unavailable.channels.every(channel =>
  channel.flags.includes('upstream_measurement_unavailable') &&
  channel.flags.includes('direct_anchor_evidence_unavailable')));

assert.throws(() => diagnoseArrayChannels([], {}), /one to 64/);
process.stdout.write('array channel health diagnostics passed\n');
