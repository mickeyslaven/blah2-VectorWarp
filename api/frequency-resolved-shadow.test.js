'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {EventEmitter} = require('events');
const {FrequencyResolvedShadow, modelStability} = require('./frequency-resolved-shadow');
const {analysisDescriptor, analysisFingerprint} = require('./frequency-resolved-analysis');

const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-frequency-shadow-'));
const config = {capture: {fc: 100000000, fs: 2000000, device: {type: 'Kraken',
  channel_count: 3, surveillance_channels: [0, 1, 2], heimdall: {host: '127.0.0.1', port: 8091},
  array_geometry: {mapping_confirmed: true, geometry_confirmed: true,
    orientation_confirmed: true, x_axis_bearing_deg_true: 10,
    elements: [{daq_channel: 0, position: [0, 0, 0]},
      {daq_channel: 1, position: [.5, 0, 0]}, {daq_channel: 2, position: [0, .5, 0]}]}}},
  process: {bearing: {frequency_resolved_shadow: {enabled: true, fft_size: 256,
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
    const disabled = structuredClone(config);
    disabled.process.bearing.frequency_resolved_shadow.enabled = false;
    assert.equal(new FrequencyResolvedShadow(disabled).status().enabled, false);
    console.log('Frequency-resolved shadow: isolated worker, fingerprint gate, atomic persistence, and disabled default passed.');
  } finally { fs.rmSync(directory, {recursive: true, force: true}); }
})().catch(error => { console.error(error); process.exitCode = 1; });
