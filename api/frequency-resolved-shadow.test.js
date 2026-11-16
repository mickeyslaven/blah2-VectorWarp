'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {EventEmitter} = require('events');
const {FrequencyResolvedShadow, modelStability,
  phaseOnlyEqualizationCandidate} = require('./frequency-resolved-shadow');
const {analysisDescriptor, analysisFingerprint} = require('./frequency-resolved-analysis');

const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-frequency-shadow-'));
const config = {capture: {fc: 100000000, fs: 2000000, device: {type: 'Kraken',
  channel_count: 3, surveillance_channels: [0, 1, 2], heimdall: {host: '127.0.0.1', port: 8091},
  array_geometry: {mapping_confirmed: true, geometry_confirmed: true,
    orientation_confirmed: true, x_axis_bearing_deg_true: 10,
    elements: [{daq_channel: 0, position: [0, 0, 0]},
      {daq_channel: 1, position: [.5, 0, 0]}, {daq_channel: 2, position: [0, .5, 0]}]}}},
  process: {reference_synthesis: {mode: 'array_eigenbeam', channels: [0, 1, 2]},
    bearing: {frequency_resolved_shadow: {enabled: true, fft_size: 256,
    subbands: 4, bandwidth_fraction: .8, diagonal_loading: .02, interval_s: 300,
    path: path.join(directory, 'candidate.json')}}},
  location: {rx: {latitude: 40, longitude: -80}, tx: {latitude: 41, longitude: -79}}};
const descriptor = analysisDescriptor(config), fingerprint = analysisFingerprint(descriptor);
assert.deepEqual(modelStability({config_fingerprint: fingerprint, subbands: [
  {principal_response: [[1, 0]], direct_constrained_mvdr: {weights: [[1, 0]]}}]},
{config_fingerprint: fingerprint, subbands: [
  {principal_response: [[0, 1]], direct_constrained_mvdr: {weights: [[0, 1]]}}]}),
{principal_response_correlation: {min: 1, median: 1},
  direct_constrained_weight_correlation: {min: 1, median: 1}});
const response = [[.5, 0], [.3, .4], [-.2, .6]];
const stableSummary = {rank1_fraction: {min: .99}, stability_from_previous:
  {principal_response_correlation: {min: .995}}};
const previous = {config_fingerprint: fingerprint,
  history: Array.from({length: 7}, () => structuredClone(stableSummary)),
  subbands: [0, 1].map(offset_hz => ({offset_hz, principal_response: response}))};
const current = {config_fingerprint: fingerprint,
  summary: {rank1_fraction: {min: .99}},
  subbands: [0, 1].map(offset_hz => ({offset_hz, principal_response: response}))};
const correction = phaseOnlyEqualizationCandidate(previous, current);
assert.equal(correction.ready_for_shadow_replay, true);
assert.equal(correction.production_authorized, false);
assert.equal(correction.maximum_noise_gain, 1);
for (const band of correction.subbands) for (const weight of band.weights)
  assert.ok(Math.abs(weight[0] ** 2 + weight[1] ** 2 - 1) < 1e-12);
const unstable = structuredClone(previous);
unstable.history.at(-1).stability_from_previous.principal_response_correlation.min = .9;
assert.equal(phaseOnlyEqualizationCandidate(unstable, current).ready_for_shadow_replay, false);
class FakeWorker extends EventEmitter {
  constructor(_filename, options) {
    super(); assert.deepEqual(options.workerData.descriptor, descriptor);
    setImmediate(() => this.emit('message', {ok: true, result: {schema: 'test',
      config_fingerprint: fingerprint}}));
  }
}

(async () => {
  try {
    const shadow = new FrequencyResolvedShadow(config, {WorkerClass: FakeWorker, initialDelayMs: 100000});
    assert.equal(shadow.status().enabled, true);
    await shadow.run();
    const status = shadow.status();
    assert.equal(status.analyses, 1); assert.equal(status.error, null);
    const saved = JSON.parse(fs.readFileSync(status.path));
    assert.equal(saved.config_fingerprint, fingerprint); assert.equal(saved.sequence, 1);
    assert.equal(saved.history.length, 1);
    clearTimeout(shadow.timer);
    await shadow.pause();
    shadow.start();
    await shadow.run();
    assert.equal(shadow.timer, null);
    assert.equal(shadow.status().analyses, 1, 'Paused shadow must not collect under changing gain');
    const disabled = structuredClone(config);
    disabled.process.bearing.frequency_resolved_shadow.enabled = false;
    assert.equal(new FrequencyResolvedShadow(disabled).status().enabled, false);
    for (const mutate of [
      value => { value.capture.replay = {state: true}; },
      value => { value.capture.device.type = 'Usrp'; },
      value => { delete value.capture.device.heimdall; },
      value => { value.capture.device.array_geometry.mapping_confirmed = false; },
      value => { value.capture.device.array_geometry.elements.pop(); }
    ]) {
      const invalid = structuredClone(config); mutate(invalid);
      const state = new FrequencyResolvedShadow(invalid).status();
      assert.equal(state.enabled, false);
      assert.match(state.error, /live MCHQ endpoint and confirmed coherent reference geometry/);
    }
    console.log('Frequency-resolved shadow: isolated worker, fingerprint gate, atomic persistence, and disabled default passed.');
  } finally { fs.rmSync(directory, {recursive: true, force: true}); }
})().catch(error => { console.error(error); process.exitCode = 1; });
