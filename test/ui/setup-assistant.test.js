const assert = require('node:assert/strict');
const test = require('node:test');
const {JSDOM} = require('./jsdom');
const {render, recordingEstimate, searchWindow, gpuActive} = require('../../html/js/setup_assistant.js');

test('recording estimates distinguish compact Kraken clips from portable recordings', () => {
  const config = {capture: {fs: 2_400_000, device: {type: 'Kraken', channel_count: 5},
    aircraft_clips: {pre_seconds: 2, post_seconds: 2}}};
  const estimate = recordingEstimate(config);
  assert.equal(estimate.manualPerSecond, 96_000_000);
  assert.equal(estimate.manualHour, 345_600_000_000);
  assert.equal(estimate.clipPerSecond, 24_000_000);
  assert.equal(estimate.clip, 96_000_000);
  assert.equal(estimate.clipLabel, 'Compact native 8-bit Kraken IQ');
});

test('automatic search window conservatively covers range and target speed', () => {
  const result = searchWindow({capture: {fs: 2_400_000, fc: 509_000_000}}, 50, 300);
  assert.equal(result.delayMax, 801);
  assert.equal(result.dopplerMax, 1020);
  assert.ok(Math.abs(result.kmPerBin - 0.124913524) < 1e-9);
});

test('automatic search window rejects incomplete setup', () => {
  assert.equal(searchWindow({capture: {}}, 50, 300), null);
});

test('GPU status recognizes both staged and fully resident Vulkan processing', () => {
  assert.equal(gpuActive({active: 'vulkan', state: 'ready'}), true);
  assert.equal(gpuActive({active: 'vulkan-resident', state: 'ready'}), true);
  assert.equal(gpuActive({active: 'cpu', state: 'ready'}), false);
});

test('assistant renders a lightweight beginner workflow and applies draft-only helpers', () => {
  const dom = new JSDOM('<div id="assistant"></div>');
  const config = {capture: {fc: 509_000_000, fs: 2_400_000,
    device: {type: 'Kraken', channel_count: 5, heimdall: {gain_control: 'fixed'}},
    aircraft_clips: {pre_seconds: 2, post_seconds: 2}},
  process: {ambiguity: {delayMin: 0, delayMax: 100, dopplerMin: -100, dopplerMax: 100},
    clutter: {followAmbiguity: false}}, location: {rx: {}, tx: {}}};
  const writes = [], modes = [], target = dom.window.document.getElementById('assistant');
  render(target, {getConfig: () => config, mode: () => 'simple', setMode: value => modes.push(value),
    openTab() {}, set: (path, value) => { writes.push([path.join('.'), value]); },
    refresh() {}, refreshEditor() {}});
  assert.equal(target.querySelectorAll('.setup-card').length, 9);
  assert.match(target.textContent, /GPU processing is active|whether qualified GPU processing is active/i);
  target.querySelector('[data-mode="advanced"]').click();
  assert.deepEqual(modes, ['advanced']);
  target.querySelector('[data-auto-gain]').click();
  assert.deepEqual(writes.shift(), ['capture.device.heimdall.gain_control', 'measured']);
  target.querySelector('[data-window]').click();
  assert.ok(writes.some(([path, value]) => path === 'process.ambiguity.delayMax' && value === 801));
  assert.match(target.querySelector('[data-window-result]').textContent, /Save & Restart/);
});

test('advanced mode collapses the beginner workflow instead of duplicating two settings systems', () => {
  const dom = new JSDOM('<div id="assistant"></div>');
  const target = dom.window.document.getElementById('assistant');
  render(target, {getConfig: () => ({capture: {device: {type: 'Kraken'}}}),
    mode: () => 'advanced', setMode() {}, openTab() {}, set() {}, refresh() {}, refreshEditor() {}});
  assert.equal(target.querySelectorAll('.setup-card').length, 0);
  assert.match(target.textContent, /All receiver, DSP, service, and diagnostic settings are shown below/);
  assert.ok(target.querySelector('[data-mode="simple"]'), 'The operator can always return to Simple mode');
});
