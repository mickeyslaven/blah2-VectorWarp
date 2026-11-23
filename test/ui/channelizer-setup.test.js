'use strict';
// Neutral setup/upgrade regressions for the shared RF-channel editor and plan.
// No receivers, network, operational files or native compilation are involved.
const assert = require('node:assert/strict');
const test = require('node:test');
const fs = require('node:fs');
const path = require('node:path');
const {JSDOM} = require('./jsdom');
const {configurationForReceiver} = require('../../api/config-store');
const {getDeviceProfiles, validateConfig, FIELD_RULES, effectiveRadarPlan} = require('../../api/config-manager');
const {channelizerPreview, channelizerStandards, scaledInputNumber} = require('../../html/js/config_profiles');
const {render} = require('../../html/js/setup_assistant');
const root = path.resolve(__dirname, '../..');
const clone = value => JSON.parse(JSON.stringify(value));
function configured(type = 'Usrp', rate = 24576000) {
  const config = configurationForReceiver(type);
  config.capture.fs = rate; config.capture.fc = 650000000;
  config.process.ambiguity.dopplerMin = -5000; config.process.ambiguity.dopplerMax = 5000;
  config.process.ambiguity.delayMin = -2; config.process.ambiguity.delayMax = 20;
  config.process.clutter.followAmbiguity = true;
  if (type === 'HackRF') config.capture.device.serial = ['0001', '0002'];
  config.network.ports.illuminator = 3007;
  config.process.multi_illuminator = {enabled: false, illuminators: []};
  return config;
}
function lane(overrides = {}) {
  return {id: 'neutral-channel', enabled: true, standard: 'DVB-T2', frequency_hz: 650000000,
    occupied_bandwidth_hz: 7600000, output_sample_rate_hz: 8192000,
    filter_taps_per_phase: 96, atsc_reference_mode: 'raw',
    transmitter: {name: 'Test transmitter', latitude: -33.9, longitude: 151.2, altitude: 30}, ...overrides};
}
function valid(config) {
  const result = validateConfig(config); assert.equal(result.valid, true, result.errors.join('; '));
}
test('decimal unit entry preserves odd whole Hz without rounding actual fractional Hz', () => {
  for (const scale of [1000, 1000000])
    for (let hz = 2000000; hz < 2010000; hz++)
      assert.equal(scaledInputNumber(String(hz / scale), scale), hz);
  assert.equal(scaledInputNumber('2.000001e0', 1e6), 2000001);
  assert.equal(scaledInputNumber('2000001e-6', 1e6), 2000001);
  assert.equal(scaledInputNumber('2.0000011', 1e6), 2000001.1);
  assert.equal(scaledInputNumber('2000.0011', 1000), 2000001.1);
});
test('B210 declares dual-input limits, preserves arbitrary exact rates and refuses impossible mapping/rates', () => {
  const profile = getDeviceProfiles().find(item => item.type === 'Usrp');
  assert.equal(profile.sampleRates.max, 30720000);
  for (const rate of [2000000, 6000000, 12288000, 24576000, 30720000]) valid(configured('Usrp', rate));
  const config = configured('Usrp', 30720001);
  assert.match(validateConfig(config).errors.join(' '), /30.72 MS\/s/);
  config.capture.fs = 2000000;
  for (const mapping of ['A:A', 'A:A A:A', 'A:A A:B A:C', 'B:A B:B']) {
    config.capture.device.subdev = mapping;
    assert.match(validateConfig(config).errors.join(' '), /map both B210 inputs/);
  }
  config.capture.device.subdev = 'A:B A:A'; valid(config);
  Object.assign(config.capture.replay, {state: true, sample_rate_hz: 40000000,
    frequency_hz: 650000000, channel_count: 2, reference_mode: 'dedicated',
    reference_channel: 0, reference_channels: [0], surveillance_channels: [1]});
  valid(config); // Recorded topology/rate is not limited by the dormant live adapter.
});
test('non-6-MHz extraction is neutral across B210 and another adapter and preserves plan semantics', () => {
  const a = configured('Usrp', 16000000), b = configured('HackRF', 16000000);
  for (const config of [a, b]) {
    config.process.multi_illuminator = {enabled: true, illuminators: [lane({output_sample_rate_hz: 8000000})]};
    valid(config);
    const preview = channelizerPreview(config, config.process.multi_illuminator.illuminators[0]);
    assert.deepEqual(preview.errors, []); assert.equal(preview.lowerHz, 646200000);
    assert.equal(preview.upperHz, 653800000); assert.equal(preview.decimation, 2);
    assert.equal(preview.filterTaps, 193); assert.equal(preview.transitionHz, 200000);
  }
  assert.equal(effectiveRadarPlan(a).semantic_sha256, effectiveRadarPlan(b).semantic_sha256);
});
test('channel extraction rejects fractional decimation, Nyquist clipping, overlap and excessive FIR allocation', () => {
  const config = configured(); config.process.multi_illuminator.enabled = true;
  for (const patch of [{output_sample_rate_hz: 8000000}, {occupied_bandwidth_hz: 8192000},
    {frequency_hz: 662000000}, {occupied_bandwidth_hz: 1, output_sample_rate_hz: 2}]) {
    config.process.multi_illuminator.illuminators = [lane(patch)];
    assert.equal(validateConfig(config).valid, false);
    assert.ok(channelizerPreview(config, config.process.multi_illuminator.illuminators[0]).errors.length);
  }
  config.process.multi_illuminator.illuminators = [lane(), lane({id: 'overlap', frequency_hz: 651000000})];
  assert.match(validateConfig(config).errors.join(' '), /overlaps enabled channel/);
  config.process.multi_illuminator.illuminators = [lane({filter_taps_per_phase: 5})];
  assert.equal(channelizerPreview(config, config.process.multi_illuminator.illuminators[0]).filterTaps, 17,
    'native FIR design rounds an even requested length up to an odd tap count');
  for (const malformed of [null, false, 42, 'invalid']) {
    config.process.multi_illuminator.illuminators = [lane(), malformed];
    assert.equal(validateConfig(config).valid, false,
      'malformed adjacent lanes return validation errors instead of breaking configuration/status reads');
  }
});
test('version-specific reference capabilities do not conflate raw extraction and demodulation', () => {
  const config = configured(); config.process.multi_illuminator.enabled = true;
  const item = lane({standard: 'ATSC-1.0', occupied_bandwidth_hz: 5380000, atsc_reference_mode: 'pilot_aided'});
  config.process.multi_illuminator.illuminators = [item]; valid(config);
  item.standard = 'ATSC-3.0';
  assert.match(validateConfig(config).errors.join(' '), /raw physical reference.*synthesis is unavailable/);
  item.atsc_reference_mode = 'raw'; valid(config);
  for (const standard of ['DVB-T', 'DVB-T2', 'DVB-T/T2', 'ISDB-T', 'DTMB', 'CUSTOM']) {
    item.standard = standard; valid(config);
  }
  const native = fs.readFileSync(path.join(root, 'src/blah2.cpp'), 'utf8');
  for (const standard of channelizerStandards) assert.ok(native.includes(`"${standard}"`), standard);
});
function editor(config) {
  const dom = new JSDOM('<div id="fixture"></div>', {runScripts: 'outside-only'});
  const window = dom.window;
  window.TextEncoder = TextEncoder;
  for (const name of ['processing_capabilities', 'config_profiles', 'kraken_geometry', 'transmitter_catalog', 'config_ui'])
    window.eval(fs.readFileSync(path.join(root, `html/js/${name}.js`), 'utf8') +
      (name === 'config_ui' ? '\nwindow.setFixture = (config, caps) => {activeConfig = config; capabilities = caps;};' : ''));
  window.setFixture(config, {editable: true, fieldRules: FIELD_RULES, deviceProfiles: getDeviceProfiles()});
  window.setValue = (names, value) => {
    const parent = names.slice(0, -1).reduce((value, key) => value[key], config); parent[names.at(-1)] = value;
  };
  window.renderEditor = () => window.document.getElementById('fixture').replaceChildren(
    window.illuminatorListInput(config.process.multi_illuminator.illuminators, ['process', 'multi_illuminator', 'illuminators']));
  window.renderEditor(); return dom;
}
test('DOM preserves upgrade widths/filters on standard changes; presets are explicit and raw remains selectable', () => {
  const config = configured(); config.process.multi_illuminator.illuminators = [lane()];
  const dom = editor(config), {window} = dom, document = window.document;
  try {
    const choose = value => {
      const input = document.querySelector('[data-lane-field="standard"]'); input.value = value;
      input.dispatchEvent(new window.Event('change'));
    };
    choose('ATSC-3.0');
    let item = config.process.multi_illuminator.illuminators[0];
    assert.equal(item.occupied_bandwidth_hz, 7600000); assert.equal(item.filter_taps_per_phase, 96);
    assert.equal(item.output_sample_rate_hz, 8192000); assert.equal(item.atsc_reference_mode, 'raw');
    assert.match(document.querySelector('.channelizer-preview').textContent, /646.2–653.8 MHz/);
    document.querySelector('[data-channelizer-preset]').click();
    item = config.process.multi_illuminator.illuminators[0];
    assert.equal(item.occupied_bandwidth_hz, 5832844); assert.equal(item.filter_taps_per_phase, 128);
    assert.equal(item.output_sample_rate_hz, 8192000);
    choose('FM');
    const reference = document.querySelector('[data-lane-field="atsc_reference_mode"]');
    assert.equal(reference.disabled, false); assert.equal(reference.querySelector('[value="raw"]').disabled, false);
    const rate = window.typedInput(config.capture.fs, ['capture', 'fs']).querySelector('input');
    assert.equal(rate.max, '30.72'); assert.equal(rate.getAttribute('list'), 'capture-rate-suggestions');
    rate.value = '2.000001'; rate.dispatchEvent(new window.Event('input'));
    assert.equal(config.capture.fs, 2000001, 'the main editor uses exact decimal unit conversion too');
  } finally { window.close(); }
});
test('wizard consumes adapter sample-rate capability and preserves exact Hz precision', async () => {
  const config = configured(), dom = new JSDOM('<div id="assistant"></div>');
  const target = dom.window.document.getElementById('assistant');
  try {
    const api = {getConfig: () => config, receiverProfiles: getDeviceProfiles,
      set: (names, value) => { config.capture[names[1]] = value; }, saveStep: async () => true,
      discoverReceivers: async () => [], setMode() {}, mode: () => 'advanced', refresh: () => render(target, api)};
    render(target, api);
    target.querySelector('[data-start-setup]').click(); await new Promise(resolve => setImmediate(resolve));
    target.querySelector('[data-wizard-step="1"]').click();
    const rate = target.querySelector('[data-wizard-fs]');
    assert.equal(rate.max, '30.72'); assert.equal(rate.step, '0.000001');
    assert.equal(rate.value, '24.576');
    rate.value = '2.000001';
    target.querySelector('[data-wizard-fc]').value = '650.000001';
    target.querySelector('[data-wizard-next]').click();
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(config.capture.fs, 2000001);
    assert.equal(config.capture.fc, 650000001);
  } finally { dom.window.close(); }
});

test('dedicated per-lane reference override refreshes pilot choices and imported station review', () => {
  const config = configured('Kraken');
  config.process.surveillance_fusion.mode = 'noncoherent';
  config.process.multi_illuminator.illuminators = [lane({standard: 'ATSC-1.0',
    occupied_bandwidth_hz: 5380000, atsc_reference_mode: 'pilot_aided'})];
  const dom = editor(config), {window} = dom, document = window.document;
  try {
    const pilot = () => document.querySelector('[data-lane-field="atsc_reference_mode"] [value="pilot_aided"]');
    assert.equal(pilot().disabled, true, 'Array-derived reference cannot apply pilot suppression');
    const role = [...document.querySelectorAll('select')].find(input => input.title.includes('explicitly choose'));
    role.value = '0'; role.dispatchEvent(new window.Event('change'));
    assert.equal(pilot().disabled, false, 'Choosing a physical per-lane reference refreshes capability controls');
    const panel = document.querySelector('.station-import');
    panel.querySelector('textarea').value = JSON.stringify({schema: 1, id: 'physical-reference',
      provider: {id: 'fixture', region: 'EX', source: 'survey', retrieved_at: '2026-11-23T00:00:00Z', effective_at: null},
      callsign: 'TEST', city: 'Example', state: 'Example', rf_channel: null, frequency_hz: 650000000,
      channel_bandwidth_hz: 6000000, occupied_bandwidth_hz: 5380000,
      waveform: {standard: 'ATSC', version: '1.0'}, transmitter: lane().transmitter,
      provenance: Object.fromEntries(['frequency_hz', 'channel_bandwidth_hz', 'occupied_bandwidth_hz',
        'waveform', 'coordinates'].map(key => [key, 'fixture']))});
    [...panel.querySelectorAll('button')].find(button => button.textContent === 'Preview imported stations').click();
    assert.equal([...panel.querySelectorAll('button')].find(button => button.textContent === 'Confirm station changes').disabled,
      false, 'Catalog review uses the explicit physical input, not the global array reference');
  } finally { window.close(); }
});

test('offline station selection previews provenance and non-6-MHz width; only explicit confirmation changes the lane', () => {
  const config = configured(); config.process.multi_illuminator.illuminators = [lane()];
  const dom = editor(config), {window} = dom, document = window.document;
  const record = {schema: 1, id: 'neutral-station', provider: {id: 'fixture-provider', region: 'XX',
    source: 'offline surveyed assignment', retrieved_at: '2026-11-23T00:00:00Z', effective_at: null},
    callsign: 'TEST', city: 'Example', state: 'Region', rf_channel: null,
    frequency_hz: 649000000, channel_bandwidth_hz: 8000000, occupied_bandwidth_hz: 7500000,
    waveform: {standard: 'DVB-T2', version: 'T2'},
    transmitter: {name: 'Example transmitter', latitude: 20, longitude: -120, altitude: 80},
    provenance: Object.fromEntries(['frequency_hz', 'channel_bandwidth_hz', 'occupied_bandwidth_hz',
      'waveform', 'coordinates'].map(key => [key, 'explicit test survey']))};
  try {
    const before = clone(config), panel = document.querySelector('.station-import');
    panel.querySelector('textarea').value = JSON.stringify(record);
    [...panel.querySelectorAll('button')].find(button => button.textContent === 'Preview imported stations').click();
    assert.deepEqual(config, before);
    assert.match(panel.textContent, /fixture-provider.*source channel 8 MHz/);
    const confirm = [...panel.querySelectorAll('button')].find(button => button.textContent === 'Confirm station changes');
    assert.equal(confirm.disabled, false); confirm.click();
    const item = config.process.multi_illuminator.illuminators[0];
    assert.equal(item.frequency_hz, record.frequency_hz); assert.equal(item.occupied_bandwidth_hz, 7500000);
    assert.equal(item.standard, 'DVB-T2'); assert.equal(item.output_sample_rate_hz, 8192000);
    assert.equal(item.filter_taps_per_phase, 96); assert.deepEqual(config.capture, before.capture);
    assert.equal(item.transmitter_location.altitude, 80);
  } finally { window.close(); }
});
