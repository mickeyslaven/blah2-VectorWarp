'use strict';

// Local jsdom round trips for every editable settings area.  The fetch fixture
// validates candidates in-process; it never contacts a receiver or network.
const assert = require('assert/strict');
const fs = require('fs');
const path = require('path');
const {JSDOM} = require('jsdom');
const {getDeviceProfiles, validateConfig, FIELD_RULES} = require('../../api/config-manager');
const {setupDefaults} = require('../../api/config-store');
const clone = value => JSON.parse(JSON.stringify(value));
const dom = new JSDOM('<div id="configuration"></div>', {
  url: 'http://127.0.0.1:3000/display/configuration/', runScripts: 'outside-only'
});
const {window} = dom;
let saved = setupDefaults();
// Optional legacy block size has a real control whenever it is present in a
// saved profile. Keep it present so both a valid value and its replay contract
// are exercised, instead of treating an optional UI path as untestable.
saved.capture.replay.legacy_block_samples = 1024;
let revision = 'matrix-0';
let writes = 0;
const covered = new Set();
window.liveApiUrl = value => value;
window.fetchStatusResource = (url, options) => window.fetch(url, options);
window.rememberApiPort = () => {};
window.fetch = async (url, options = {}) => {
  let body;
  if (url === '/api/config/validate') body = validateConfig(JSON.parse(options.body), saved);
  else if (url.startsWith('/api/config?')) {
    assert.equal(options.headers['If-Match'], `"${revision}"`);
    saved = JSON.parse(options.body); revision = `matrix-${++writes}`;
    body = {ok: true, restarting: false, revision, message: 'Saved locally.'};
  } else if (url === '/api/config') body = saved;
  else if (url === '/api/config/capabilities') body = {editable: true, restartAvailable: false,
    configRevision: revision, fieldRules: FIELD_RULES,
    deviceProfiles: getDeviceProfiles().map(profile => ({...profile, liveAvailable: false}))};
  else if (url === '/api/upstream/status') body = {supported: false, available: false, matched: false,
    message: 'Not contacted by this DOM fixture.'};
  else if (url === '/api/system/status') body = {serverId: 'matrix', configRevision: revision,
    loadedRevision: revision, radar: 'no-data', errors: []};
  else throw new Error(`Unexpected URL: ${url}`);
  return {ok: true, status: 200, headers: {get: key => key === 'ETag' ? `"${revision}"` : null},
    json: async () => clone(body)};
};
window.eval(fs.readFileSync(path.join(__dirname, '../../html/js/config_ui.js'), 'utf8'));
const field = key => window.document.querySelector(`[data-path="${key}"]`);
const control = key => {
  const result = field(key);
  assert.ok(result, `Missing editable control: ${key}`);
  return result.querySelector('input, select');
};
function change(input, value, event = input?.tagName === 'SELECT' || input?.type === 'checkbox' ? 'change' : 'input') {
  if (input.type === 'checkbox') input.checked = Boolean(value);
  else input.value = String(value);
  input.dispatchEvent(new window.Event(event, {bubbles: true}));
}
function setScalar(key, value) { change(control(key), value); }
function renderedValue(key, expected) {
  const box = field(key);
  assert.ok(box, `Persisted field is rendered: ${key}`);
  if (key === 'truth.adsb.tar1090') return box.querySelector('input').value;
  if (key === 'capture.device.heimdall.gain') return Number(box.querySelector('input').value);
  if (key === 'capture.fc') return [...box.querySelectorAll('input')].reduce((total, input, index) =>
    total + Number(input.value) * [1000000, 1000, 1][index], 0);
  if (['capture.device.surveillance_channels', 'process.reference_synthesis.channels'].includes(key))
    return [...box.querySelectorAll('input:checked')].map(input => Number(input.value));
  const inputs = [...box.querySelectorAll('input, select')].filter(input => !input.disabled);
  const normalize = (input, expectedValue) => input.type === 'checkbox' ? input.checked :
    (typeof expectedValue === 'boolean' ? input.value === 'true' :
      (input.type === 'number' || typeof expectedValue === 'number' ? Number(input.value) : input.value));
  if (inputs.length > 1) return inputs.map((input, index) => normalize(input, expected[index]));
  const input = inputs[0];
  return normalize(input, expected);
}
async function saveAndAssert(label, expected) {
  assert.equal(await window.validateActiveConfiguration(), true, `${label}: valid browser draft`);
  await window.saveConfiguration();
  for (const [key, value] of Object.entries(expected)) {
    const actual = key.split('.').reduce((target, part) => target?.[part], saved);
    assert.deepEqual(actual, value, `${label}: ${key} round trips`);
  }
  await window.renderConfiguration();
  for (const [key, value] of Object.entries(expected)) {
    if (key.startsWith('location.')) {
      assert.match(field(key.split('.').slice(0, 2).join('.')).textContent, new RegExp(String(value)));
    } else assert.deepEqual(renderedValue(key, value), value, `${label}: ${key} reloads into the form`);
    covered.add(key);
  }
}

(async () => {
  try {
    await window.renderConfiguration();
    // These legacy/reserved values must survive saves but have no misleading UI.
    for (const [key, rule] of Object.entries(FIELD_RULES))
      if (rule.readOnly) assert.equal(field(key), null, `${key} is intentionally not editable`);

    // Browser validation combines native constraints with API cross-field
    // checks, including replay-format requirements that native inputs cannot
    // express alone.
    setScalar('process.detection.pfa', 1);
    assert.equal(await window.validateActiveConfiguration(), false, 'PFA must remain below one');
    setScalar('process.detection.pfa', .00001);
    setScalar('capture.replay.legacy_block_samples', '');
    change(control('capture.replay.format'), 'usrp-blocks');
    assert.equal(await window.validateActiveConfiguration(), false, 'USRP-block replay requires its legacy block-size setting');
    setScalar('capture.replay.legacy_block_samples', 2048);
    assert.equal(await window.validateActiveConfiguration(), true, 'Positive legacy block size enables USRP-block replay');
    change(control('capture.replay.format'), 'auto');
    assert.equal(await window.validateActiveConfiguration(), true);

    // Common settings cover recording/replay, CPU/performance, map/detection/
    // tracker controls, ADS-B, paths, ports, and location dialog validation.
    setScalar('capture.replay.state', true);
    setScalar('capture.replay.loop', true);
    setScalar('capture.replay.file', '/tmp/matrix.blah2iq');
    change(control('capture.replay.format'), 'blah2');
    const frequency = field('capture.fc').querySelectorAll('input');
    change(frequency[0], 205); change(frequency[1], 640); change(frequency[2], 123);
    setScalar('process.performance.surveillance_workers', 1);
    setScalar('process.performance.fft_threads', 2);
    change(control('process.performance.acceleration'), 'cpu');
    setScalar('process.data.cpi', .75);
    setScalar('process.data.buffer', 3);
    setScalar('process.ambiguity.delayMin', -9);
    setScalar('process.ambiguity.delayMax', 399);
    setScalar('process.ambiguity.dopplerMin', -150);
    setScalar('process.ambiguity.dopplerMax', 150);
    setScalar('process.clutter.enable', false);
    setScalar('process.clutter.delayMin', -8);
    setScalar('process.clutter.delayMax', 390);
    setScalar('process.detection.enable', false);
    setScalar('process.detection.pfa', .00002);
    setScalar('process.detection.nGuard', 3);
    setScalar('process.detection.nTrain', 8);
    setScalar('process.detection.minDelay', 6);
    setScalar('process.detection.minDoppler', 20);
    setScalar('process.detection.nCentroid', 7);
    setScalar('process.tracker.enable', false);
    setScalar('process.tracker.enable', true);
    setScalar('process.tracker.initiate.M', 2);
    setScalar('process.tracker.initiate.N', 4);
    setScalar('process.tracker.initiate.maxAcc', 12);
    setScalar('process.tracker.delete', 11);
    setScalar('truth.adsb.enabled', true);
    const adsb = field('truth.adsb.tar1090');
    change(adsb.querySelector('select'), 'server');
    change(adsb.querySelector('input'), 'https://adsb.example/tar1090');
    setScalar('truth.adsb.poll_interval', 2);
    setScalar('truth.adsb.smoothing_window', 12);
    setScalar('truth.adsb.max_position_age', 40);
    setScalar('save.map', true); setScalar('save.detection', true);
    setScalar('save.path', '/tmp/vectorwarp-matrix/');
    setScalar('network.ip', '127.0.0.1');
    for (const [name, port] of Object.entries({api: 3100, map: 3101, detection: 3102, track: 3103,
      timestamp: 4100, timing: 4101, iqdata: 4102}))
      setScalar(`network.ports.${name}`, port);
    window.document.querySelector('[data-site-role=rx][data-site-action=add]').click();
    const dialog = window.document.getElementById('site-dialog');
    for (const [name, value] of Object.entries({name: 'Matrix RX', latitude: 40.1, longitude: -74.2, altitude: 14}))
      change(dialog.querySelector(`[name=${name}]`), value);
    dialog.querySelector('form').dispatchEvent(new window.Event('submit', {bubbles: true, cancelable: true}));
    window.document.querySelector('[data-site-role=tx][data-site-action=add]').click();
    const txDialog = window.document.getElementById('site-dialog');
    for (const [name, value] of Object.entries({name: 'Matrix TX', latitude: 41.2, longitude: -73.3, altitude: 15}))
      change(txDialog.querySelector(`[name=${name}]`), value);
    txDialog.querySelector('form').dispatchEvent(new window.Event('submit', {bubbles: true, cancelable: true}));
    await saveAndAssert('common settings', {
      'capture.replay.state': true, 'capture.replay.loop': true, 'capture.replay.file': '/tmp/matrix.blah2iq',
      'capture.replay.format': 'blah2', 'capture.replay.legacy_block_samples': 2048, 'capture.fc': 205640123,
      'capture.device.type': 'RspDuo', 'process.performance.surveillance_workers': 1,
      'process.performance.fft_threads': 2, 'process.performance.acceleration': 'cpu',
      'process.data.cpi': .75, 'process.data.buffer': 3, 'process.ambiguity.delayMin': -9,
      'process.ambiguity.delayMax': 399, 'process.ambiguity.dopplerMin': -150,
      'process.ambiguity.dopplerMax': 150, 'process.clutter.enable': false,
      'process.clutter.delayMin': -8, 'process.clutter.delayMax': 390, 'process.detection.enable': false,
      'process.detection.pfa': .00002, 'process.detection.nGuard': 3, 'process.detection.nTrain': 8,
      'process.detection.minDelay': 6, 'process.detection.minDoppler': 20,
      'process.detection.nCentroid': 7, 'process.tracker.enable': true,
      'process.tracker.initiate.M': 2, 'process.tracker.initiate.N': 4,
      'process.tracker.initiate.maxAcc': 12, 'process.tracker.delete': 11, 'truth.adsb.enabled': true,
      'truth.adsb.tar1090': 'https://adsb.example/tar1090', 'truth.adsb.poll_interval': 2,
      'truth.adsb.smoothing_window': 12, 'truth.adsb.max_position_age': 40, 'save.map': true,
      'save.detection': true, 'save.path': '/tmp/vectorwarp-matrix/', 'network.ip': '127.0.0.1',
      'network.ports.api': 3100, 'network.ports.map': 3101, 'network.ports.detection': 3102,
      'network.ports.track': 3103, 'network.ports.timestamp': 4100, 'network.ports.timing': 4101,
      'network.ports.iqdata': 4102, 'location.rx.name': 'Matrix RX', 'location.rx.latitude': 40.1,
      'location.rx.longitude': -74.2, 'location.rx.altitude': 14, 'location.tx.name': 'Matrix TX',
      'location.tx.latitude': 41.2, 'location.tx.longitude': -73.3, 'location.tx.altitude': 15});

    window.switchDevice('RspDuo');
    change(control('capture.fs'), '1000000');
    change(control('capture.device.bandwidthNumber'), '50');
    setScalar('capture.device.agcSetPoint', -30);
    setScalar('capture.device.serial', 'matrix-rsp');
    change(control('capture.device.lnaState'), '2');
    const reductions = field('capture.device.gainReduction').querySelectorAll('select');
    change(reductions[0], '30'); change(reductions[1], '31');
    setScalar('capture.device.dabNotch', true); setScalar('capture.device.rfNotch', true);
    await saveAndAssert('RspDuo', {'capture.fs': 1000000, 'capture.device.bandwidthNumber': 50,
      'capture.device.agcSetPoint': -30, 'capture.device.serial': 'matrix-rsp', 'capture.device.lnaState': 2,
      'capture.device.gainReduction': [30, 31], 'capture.device.dabNotch': true,
      'capture.device.rfNotch': true});

    window.switchDevice('HackRF');
    const serials = field('capture.device.serial').querySelectorAll('input');
    change(serials[0], '00000000000000000000000000000001');
    change(serials[1], '00000000000000000000000000000002');
    const lna = field('capture.device.gain_lna').querySelectorAll('select');
    const vga = field('capture.device.gain_vga').querySelectorAll('select');
    const amps = field('capture.device.amp_enable').querySelectorAll('select');
    change(lna[0], '16'); change(lna[1], '24'); change(vga[0], '10'); change(vga[1], '12');
    change(amps[0], 'true'); change(amps[1], 'true');
    await saveAndAssert('HackRF', {'capture.device.serial': ['00000000000000000000000000000001',
      '00000000000000000000000000000002'], 'capture.device.gain_lna': [16, 24],
      'capture.device.gain_vga': [10, 12], 'capture.device.amp_enable': [true, true],
      'capture.device.type': 'HackRF'});

    window.switchDevice('Usrp');
    setScalar('capture.device.address', 'type=b200,serial=matrix');
    setScalar('capture.device.subdev', 'A:0 A:1');
    const antennas = field('capture.device.antenna').querySelectorAll('input');
    const gains = field('capture.device.gain').querySelectorAll('input');
    change(antennas[0], 'RX2'); change(antennas[1], 'TX/RX'); change(gains[0], 20); change(gains[1], 21);
    await saveAndAssert('USRP', {'capture.device.address': 'type=b200,serial=matrix',
      'capture.device.subdev': 'A:0 A:1', 'capture.device.antenna': ['RX2', 'TX/RX'],
      'capture.device.gain': [20, 21], 'capture.device.type': 'Usrp'});

    window.switchDevice('Kraken');
    setScalar('capture.device.heimdall.host', '127.0.0.1');
    setScalar('capture.device.heimdall.port', 8091);
    setScalar('capture.device.heimdall.control_port', 8092);
    const gain = field('capture.device.heimdall.gain');
    change(gain.querySelector('select'), 'manual'); change(gain.querySelector('input'), 25.5);
    change(control('capture.device.channel_count'), '4');
    change(control('process.reference_synthesis.mode'), 'array_eigenbeam');
    assert.equal(control('capture.device.reference_channel').disabled, true,
      'Dedicated-reference setting disables when synthesis owns it');
    const surveillance = field('capture.device.surveillance_channels').querySelectorAll('input');
    change(surveillance[3], false);
    const synthesized = field('process.reference_synthesis.channels').querySelectorAll('input');
    change(synthesized[3], false);
    await saveAndAssert('Kraken array synthesis', {'capture.device.type': 'Kraken',
      'capture.device.channel_count': 4, 'capture.device.surveillance_channels': [0, 1, 2],
      'process.reference_synthesis.mode': 'array_eigenbeam',
      'process.reference_synthesis.channels': [0, 1, 2]});
    change(control('process.reference_synthesis.mode'), 'dedicated');
    assert.equal(control('capture.device.reference_channel').disabled, false);
    change(control('capture.device.reference_channel'), '1');
    setScalar('process.reference_synthesis.analysis_samples', 65536);
    setScalar('process.reference_synthesis.analysis_interval', 3);
    setScalar('process.reference_synthesis.power_iterations', 8);
    setScalar('process.reference_synthesis.covariance_smoothing', .25);
    setScalar('process.reference_synthesis.diagonal_loading', .001);
    await saveAndAssert('Kraken', {'capture.device.heimdall.gain': 25.5,
      'capture.device.channel_count': 4, 'capture.device.reference_channel': 1,
      'process.reference_synthesis.mode': 'dedicated', 'process.reference_synthesis.analysis_samples': 65536,
      'process.reference_synthesis.analysis_interval': 3, 'process.reference_synthesis.power_iterations': 8,
      'process.reference_synthesis.covariance_smoothing': .25,
      'process.reference_synthesis.diagonal_loading': .001, 'capture.device.heimdall.host': '127.0.0.1',
      'capture.device.heimdall.port': 8091, 'capture.device.heimdall.control_port': 8092,
      'capture.device.type': 'Kraken', 'capture.device.surveillance_channels': [2],
      'process.reference_synthesis.channels': [1]});
    const requiredCoverage = [
      'capture.fc', 'capture.fs', 'capture.device.type', 'capture.replay.state', 'capture.replay.loop',
      'capture.replay.file', 'capture.replay.format', 'capture.replay.legacy_block_samples',
      'process.performance.surveillance_workers', 'process.performance.fft_threads',
      'process.performance.acceleration', 'process.data.cpi', 'process.data.buffer',
      'process.ambiguity.delayMin', 'process.ambiguity.delayMax', 'process.ambiguity.dopplerMin',
      'process.ambiguity.dopplerMax', 'process.clutter.enable', 'process.clutter.delayMin',
      'process.clutter.delayMax', 'process.detection.enable', 'process.detection.pfa',
      'process.detection.nGuard', 'process.detection.nTrain', 'process.detection.minDelay',
      'process.detection.minDoppler', 'process.detection.nCentroid', 'process.tracker.enable',
      'process.tracker.initiate.M', 'process.tracker.initiate.N', 'process.tracker.initiate.maxAcc',
      'process.tracker.delete', 'truth.adsb.enabled', 'truth.adsb.tar1090', 'truth.adsb.poll_interval',
      'truth.adsb.smoothing_window', 'truth.adsb.max_position_age', 'save.map', 'save.detection',
      'save.path', 'network.ip', 'network.ports.api', 'network.ports.map', 'network.ports.detection',
      'network.ports.track', 'network.ports.timestamp', 'network.ports.timing', 'network.ports.iqdata',
      'location.rx.name', 'location.rx.latitude', 'location.rx.longitude', 'location.rx.altitude',
      'location.tx.name', 'location.tx.latitude', 'location.tx.longitude', 'location.tx.altitude',
      'capture.device.serial', 'capture.device.agcSetPoint', 'capture.device.bandwidthNumber',
      'capture.device.lnaState', 'capture.device.gainReduction', 'capture.device.dabNotch',
      'capture.device.rfNotch', 'capture.device.gain_lna', 'capture.device.gain_vga',
      'capture.device.amp_enable', 'capture.device.address', 'capture.device.subdev',
      'capture.device.antenna', 'capture.device.gain', 'capture.device.channel_count',
      'capture.device.reference_channel', 'capture.device.surveillance_channels',
      'capture.device.heimdall.host', 'capture.device.heimdall.port',
      'capture.device.heimdall.control_port', 'capture.device.heimdall.gain',
      'process.reference_synthesis.mode', 'process.reference_synthesis.channels',
      'process.reference_synthesis.analysis_samples', 'process.reference_synthesis.analysis_interval',
      'process.reference_synthesis.power_iterations', 'process.reference_synthesis.covariance_smoothing',
      'process.reference_synthesis.diagonal_loading'
    ];
    for (const key of requiredCoverage) assert.ok(covered.has(key), `Coverage gate: ${key} was saved and re-rendered`);
    assert.ok(writes >= 6, 'Common and each receiver/synthesis profile round-tripped.');
    console.log('Settings matrix DOM: common, replay, location, network, ADS-B, performance, detector/tracker, and all receiver profiles round-trip locally.');
  } finally { window.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
