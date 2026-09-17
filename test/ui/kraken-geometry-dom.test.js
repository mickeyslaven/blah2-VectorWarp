'use strict';
// Real disposable API + YAML store, with a browser DOM. Never calls a live API.
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {spawn} = require('child_process');
const {once} = require('events');
const http = require('http');
const net = require('net');
const {randomInt} = require('crypto');
const {JSDOM} = require('jsdom');
const {getDeviceProfiles, writeConfigAtomically} = require('../../api/config-manager');
const {setupDefaults, readConfig} = require('../../api/config-store');
const {applyDeviceProfile} = require('../../html/js/config_ui');
const root = path.resolve(__dirname, '../..');
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'kraken-geometry-dom-'));
const filename = path.join(directory, 'config.yml');
const config = applyDeviceProfile(setupDefaults(), getDeviceProfiles()[0]);
config.truth.adsb.enabled = false;
config.network.ip = '127.0.0.1';
config.capture.device.reference_channel = 2;
config.capture.device.surveillance_channels = [0, 1, 3, 4];
config.process.reference_synthesis.mode = 'dedicated';
config.process.reference_synthesis.channels = [2];
async function reservePorts(count) {
  const listeners = [];
  const first = 20000 + randomInt(10000); // Below Linux's usual ephemeral range.
  const release = () => Promise.all(listeners.map(listener => new Promise((resolve, reject) =>
    listener.close(error => error ? reject(error) : resolve()))));
  try {
    for (let offset = 0; offset < 10000 && listeners.length < count; offset++) {
      const port = 20000 + (first - 20000 + offset) % 10000;
      const listener = net.createServer();
      const reserved = await new Promise(resolve => {
        listener.once('error', () => resolve(false));
        listener.listen(port, '127.0.0.1', () => resolve(true));
      });
      if (reserved) listeners.push(listener);
    }
    if (listeners.length !== count) throw new Error('Could not reserve distinct disposable test ports');
    return {ports: listeners.map(listener => listener.address().port), release};
  } catch (error) {
    await release();
    throw error;
  }
}

let child;
let stderr = '';
let childFailure = '';
let port;
let base;
let dom;
const request = (url, options = {}) => new Promise((resolve, reject) => {
  const req = http.request(`${base}${url}`, {method: options.method || 'GET',
    headers: {...(['PUT', 'POST'].includes(options.method) ? {Origin: base} : {}), ...options.headers},
    timeout: options.timeout || 500}, response => {
    let body = '';
    response.setEncoding('utf8'); response.on('data', value => { body += value; });
    response.on('end', () => resolve({ok: response.statusCode >= 200 && response.statusCode < 300,
      status: response.statusCode, headers: {get: name => response.headers[name.toLowerCase()] || null},
      json: async () => JSON.parse(body)}));
  });
  req.on('timeout', () => req.destroy(new Error('Disposable API request timed out')));
  req.on('error', reject); if (options.body) req.write(options.body); req.end();
});
(async () => {
  let reservation;
  try {
    const portNames = Object.keys(config.network.ports);
    reservation = await reservePorts(portNames.length);
    portNames.forEach((key, index) => { config.network.ports[key] = reservation.ports[index]; });
    port = config.network.ports.api;
    base = `http://127.0.0.1:${port}`;
    writeConfigAtomically(filename, config);
    await reservation.release();
    reservation = null;
    child = spawn(process.execPath, [path.join(root, 'api/server.js'), filename], {
      env: {...process.env, BLAH2_PREVIEW: 'true', BLAH2_RECEIVER_TYPES: 'Kraken', BLAH2_CONFIG_RESTART_COMMAND: ''},
      stdio: ['ignore', 'ignore', 'pipe']
    });
    child.stderr.on('data', value => { stderr = `${stderr}${value}`.slice(-4096); });
    child.once('error', error => { childFailure = `spawn error: ${error.message}`; });
    child.once('exit', (code, signal) => { childFailure = `exit ${code}${signal ? ` (${signal})` : ''}`; });
    let ready = false;
    const deadline = Date.now() + 10000;
    while (Date.now() < deadline && !childFailure) {
      try { if ((await request('/api/config')).ok) { ready = true; break; } } catch (_) { /* Isolated startup. */ }
      await new Promise(resolve => setTimeout(resolve, 50));
    }
    assert.ok(ready, stderr || `Disposable API did not start (${childFailure || 'startup timed out'})`);
    dom = new JSDOM('<div id="configuration"></div>', {url: `${base}/display/configuration/`, runScripts: 'outside-only', pretendToBeVisual: true});
    const window = dom.window;
    window.liveApiUrl = value => value; window.rememberApiPort = () => {};
    let upstream = {supported: true, available: false, matched: false, message: 'Fixture has no receiver'};
    window.fetch = async (url, options = {}) => {
      if (url === '/api/upstream/status') return {ok: true, json: async () => upstream};
      return request(url, options);
    };
    window.fetchStatusResource = (url, options) => window.fetch(url, options);
    for (const file of ['html/js/kraken_geometry.js', 'html/js/config_ui.js']) window.eval(fs.readFileSync(path.join(root, file), 'utf8'));
    const doc = window.document;
    const query = key => doc.querySelector(`[data-path="${key}"] input, [data-path="${key}"] select`);
    const g = key => query(`capture.device.array_geometry.${key}`);
    const change = (control, value) => {
      assert.ok(control, 'Expected input exists');
      if (control.type === 'checkbox') control.checked = value; else control.value = String(value);
      control.dispatchEvent(new window.Event(control.tagName === 'SELECT' || control.type === 'checkbox' ? 'change' : 'input', {bubbles: true}));
    };
    const click = text => { const button = [...doc.querySelectorAll('.kraken-geometry button')].find(item => item.textContent === text); assert.ok(button, text); button.click(); };
    const current = () => readConfig(filename).config;
    const save = async () => {
      assert.equal(await window.validateActiveConfiguration(), true, doc.getElementById('config-message').textContent);
      await window.saveConfiguration();
      assert.ok(!doc.getElementById('config-message').textContent.includes('Unable'), doc.getElementById('config-message').textContent);
      assert.equal(doc.getElementById('config-state').textContent, 'Matches saved file');
      const response = await request('/api/config');
      assert.deepEqual(await response.json(), current());
    };
    await window.renderConfiguration();
    assert.ok(doc.querySelector('#settings-panel-capture .kraken-geometry'));
    assert.equal(doc.querySelectorAll('[role=tab]').length, 6);
    assert.equal(current().capture.device.array_geometry, undefined);
    click('Record antenna layout');
    assert.equal(g('elements.0.daq_channel').value, '', 'No DAQ wiring guess');
    assert.equal(g('elements.0.position.0').value, '', 'No position guess');
    assert.equal(g('mapping_confirmed').checked, false);
    await save();
    assert.equal(current().capture.device.array_geometry.elements.length, 5);
    const mapping = [3, 2, 0, 4, 1];
    const points = [[-.5, 0, 0], [2, 2, .2], [.5, 0, 0], [0, .5, 0], [0, -.5, 0]];
    change(g('entry_template'), 'cross');
    for (let index = 0; index < 5; index++) {
      change(g(`elements.${index}.daq_channel`), mapping[index]);
      change(g(`elements.${index}.id`), `measured-${index}`);
      change(g(`elements.${index}.receiver_serial`), `recorded-${index}`);
      for (let axis = 0; axis < 3; axis++) change(g(`elements.${index}.position.${axis}`), points[index][axis]);
      if (mapping[index] !== 2) change(g(`elements.${index}.bearing`), true);
    }
    assert.equal(g('elements.1.bearing').disabled, true, 'Arbitrary dedicated reference excluded');
    change(g('x_axis_bearing_deg_true'), 30);
    await save();
    let geometry = current().capture.device.array_geometry;
    assert.deepEqual(geometry.elements.map(element => element.daq_channel), mapping);
    assert.deepEqual(geometry.elements.map(element => element.position), points);
    assert.deepEqual(geometry.bearing_channels, [3, 0, 4, 1]);
    assert.equal(geometry.shape, 'cross');
    assert.equal(await window.validateActiveConfiguration(), true);
    assert.ok(doc.getElementById('geometry-assessment').textContent.includes('Record complete'));
    for (const name of ['mapping_confirmed', 'geometry_confirmed', 'orientation_confirmed']) change(g(name), true);
    await save();
    assert.equal(current().capture.device.array_geometry.mapping_confirmed, true);
    assert.equal(g('elements.0.position.0').disabled, false, 'Save must restore editing');
    change(g('elements.0.position.0'), -.55);
    assert.equal(g('mapping_confirmed').checked, false);
    await save();
    assert.equal(current().capture.device.array_geometry.mapping_confirmed, false);
    await window.renderConfiguration();
    assert.equal(g('elements.0.position.0').value, '-0.55');
    const measurements = current().capture.device.array_geometry.elements;
    change(query('capture.device.channel_count'), 2);
    assert.equal(doc.querySelectorAll('.geometry-element').length, 5, 'Shrinking count preserves measurements');
    assert.equal(await window.validateActiveConfiguration(), true, 'One surveillance channel remains valid passive radar');
    change(query('capture.device.channel_count'), 5);
    assert.equal(g('mapping_confirmed').checked, false, 'Restoring count never restores statements');
    change(query('capture.device.reference_channel'), 4);
    assert.equal(g('elements.3.bearing').disabled, true);
    assert.ok(doc.querySelector('.kraken-geometry').textContent.includes('conflicts with the current role'));
    assert.equal(g('elements.0.position.0').value, '-0.55');
    change(query('process.reference_synthesis.mode'), 'array_eigenbeam');
    assert.equal(g('mapping_confirmed').checked, false);
    window.switchDevice('RspDuo');
    assert.ok(doc.querySelector('.kraken-geometry').textContent.includes('Saved Kraken layout (inactive)'));
    assert.equal(doc.querySelectorAll('.kraken-geometry input, .kraken-geometry select').length, 0);
    change(query('capture.replay.state'), true);
    await save();
    assert.deepEqual(current().capture.device.array_geometry.elements, measurements, 'Other profiles retain dormant record');
    window.switchDevice('Kraken');
    assert.equal(g('elements.0.position.0').value, '-0.55');
    change(g('entry_template'), 'pentagon');
    assert.equal(g('shape').value, 'regular_polygon');
    change(query('process.reference_synthesis.mode'), 'dedicated');
    change(g('entry_template'), 'pentagon');
    assert.equal(g('shape').value, 'custom', 'Dedicated vertex leaves custom subset');
    assert.equal(g('elements.0.position.0').value, '-0.55', 'Guide never replaces measurements');
    change(g('entry_template'), 'ula');
    assert.equal(g('shape').value, 'ula');
    change(g('entry_template'), 'regular_polygon');
    assert.equal(g('shape').value, 'regular_polygon');
    await save();
    for (const status of [
      {supported: false}, {supported: true, available: false, num_channels: 5},
      {supported: true, available: true, matched: true, checkedAt: 0, num_channels: 5, expected: {channels: 5}, actual: {channels: 5}},
      {supported: true, available: true, matched: true, num_channels: 5, expected: {channels: 5}, actual: {channels: 4}}
    ]) {
      upstream = status; await window.refreshUpstreamStatus();
      const section = doc.querySelector('.kraken-geometry');
      assert.ok(section.textContent.includes('physical agreement unverified'));
      assert.ok(section.textContent.includes('Bearing unavailable'));
      assert.equal(section.querySelector('.good, .verified'), null);
    }
    const original = current();
    const raw = await request('/api/config'); const revision = raw.headers.get('etag');
    const invalid = JSON.parse(JSON.stringify(original)); invalid.capture.device.array_geometry.units = 'mm';
    for (const [url, method] of [['/api/config/validate', 'POST'], ['/api/config?restart=false', 'PUT']]) {
      const response = await request(url, {method, headers: {'Content-Type': 'application/json', 'If-Match': revision,
        'X-VectorWarp-Intent': 'config-write-v1'}, body: JSON.stringify(invalid)});
      assert.equal(response.status, 422);
    }
    assert.deepEqual(current(), original, 'Invalid API record must not write');
    const custom = JSON.parse(JSON.stringify(original));
    custom.capture.device.array_geometry.operator_notes = {fixture: 'Preserve this unknown field'};
    const accepted = await request('/api/config?restart=false', {method: 'PUT', headers: {'Content-Type': 'application/json', 'If-Match': revision,
      'X-VectorWarp-Intent': 'config-write-v1'}, body: JSON.stringify(custom)});
    assert.equal(accepted.status, 200);
    await window.renderConfiguration();
    change(g('elements.0.position.2'), .12);
    await save();
    assert.deepEqual(current().capture.device.array_geometry.operator_notes, custom.capture.device.array_geometry.operator_notes);
    assert.equal(current().capture.device.array_geometry.elements[0].position[2], .12);
    click('Add measured element');
    assert.equal(doc.querySelectorAll('.geometry-element').length, 6);
    click('Remove element 6');
    assert.equal(doc.querySelectorAll('.geometry-element').length, 5);
    assert.equal(g('elements.0.position.2').value, '0.12');
    // Dynamic save locks also protect geometry buttons, not just text inputs.
    const section = window.KrakenGeometry.render(doc, current(), () => { throw new Error('Locked edit'); }, () => false);
    section.querySelectorAll('button').forEach(button => button.click());
    assert.ok([...section.querySelectorAll('input, select, button')].every(input => input.disabled));
    // Unknown enum values remain visible and invalid until explicitly changed.
    const unknown = current();
    unknown.capture.device.array_geometry.shape = 'future_spiral';
    unknown.capture.device.array_geometry.entry_template = 'future_template';
    unknown.capture.device.array_geometry.ula_half_plane = 'future_plane';
    const unknownSection = window.KrakenGeometry.render(doc, unknown, () => {}, true);
    for (const [field, value] of [['shape', 'future_spiral'], ['entry_template', 'future_template'], ['ula_half_plane', 'future_plane']]) {
      const control = unknownSection.querySelector(`[data-path="capture.device.array_geometry.${field}"] select`);
      assert.equal(control.value, value);
      assert.ok(control.selectedOptions[0].textContent.includes('Unsupported saved value'));
      assert.equal(control.checkValidity(), false);
      assert.equal(unknown.capture.device.array_geometry[field], value, 'Rendering must preserve the unknown value');
    }
    // A malformed record has a confirmed draft removal path for every profile.
    let removals = 0;
    for (const type of ['Kraken', 'RspDuo']) {
      for (const malformed of [null, 'invalid', {elements: 'invalid'}, {elements: [], bearing_channels: {bad: true}}]) {
        const candidate = current(); candidate.capture.device.type = type; candidate.capture.device.array_geometry = malformed;
        const retained = JSON.stringify(candidate.capture.device.array_geometry);
        const repair = window.KrakenGeometry.render(doc, candidate, () => { removals++; }, true);
        const remove = [...repair.querySelectorAll('button')].find(button => button.textContent === 'Remove layout record');
        assert.ok(remove);
        window.confirm = () => false; remove.click();
        assert.equal(JSON.stringify(candidate.capture.device.array_geometry), retained, 'Cancel preserves the full record');
        window.confirm = () => true; remove.click();
        assert.equal(candidate.capture.device.array_geometry, undefined);
      }
    }
    assert.equal(removals, 8);
    // Real profile switch, remove, discard, then save: file changes only on Save.
    await window.renderConfiguration();
    window.switchDevice('RspDuo');
    const beforeRemoval = current().capture.device.array_geometry;
    window.confirm = () => true; click('Remove layout record');
    assert.deepEqual(current().capture.device.array_geometry, beforeRemoval);
    doc.getElementById('config-reset').click();
    assert.ok(g('elements.0.position.0'));
    assert.deepEqual(current().capture.device.array_geometry, beforeRemoval);
    click('Remove layout record'); await save();
    assert.equal(current().capture.device.array_geometry, undefined);
    const imported = current(); imported.capture.device.array_geometry = beforeRemoval;
    imported.capture.device.array_geometry.shape = 'unsupported_imported_shape';
    writeConfigAtomically(filename, imported);
    await window.renderConfiguration();
    assert.equal(g('shape').value, 'unsupported_imported_shape');
    assert.equal(await window.validateActiveConfiguration(), false);
    assert.equal(g('shape').getAttribute('aria-invalid'), 'true');
    assert.ok(g('shape').closest('.config-field').querySelector('.config-field-error').textContent);
    assert.equal(current().capture.device.array_geometry.shape, 'unsupported_imported_shape');
    imported.capture.device.array_geometry = null;
    writeConfigAtomically(filename, imported);
    await window.renderConfiguration();
    assert.ok(doc.querySelector('.kraken-geometry').textContent.includes('Unsupported saved layout'));
    window.switchDevice('RspDuo'); change(query('capture.replay.state'), true);
    assert.equal(await window.validateActiveConfiguration(), false);
    assert.ok(doc.querySelector('.kraken-geometry').textContent.includes('Saved Kraken layout (inactive)'));
    window.confirm = () => true; click('Remove layout record'); await save();
    assert.equal(current().capture.device.type, 'RspDuo');
    assert.equal(current().capture.device.array_geometry, undefined);
    console.log('Kraken Receiver editor + real disposable API + YAML save/reload + status isolation passed.');
  } finally {
    if (dom) dom.window.close();
    if (reservation) await reservation.release();
    if (child?.exitCode === null) { child.kill('SIGTERM'); await once(child, 'exit'); }
    fs.rmSync(directory, {recursive: true, force: true});
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
