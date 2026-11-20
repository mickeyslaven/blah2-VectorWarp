'use strict';

// CI DOM acceptance: every rendered settings control has a canonical owner,
// an accessible name and a wired interaction in every supported OS/SDR/backend
// combination. Specific hardware actions remain mocked in their dedicated tests.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {JSDOM} = require('./jsdom');
const {configurationForReceiver} = require('../../api/config-store');
const {getDeviceProfiles, FIELD_RULES, validateConfig} = require('../../api/config-manager');
const {copyIssues} = require('../helpers/ui-acceptance');
const sources = ['processing_capabilities', 'config_profiles', 'kraken_geometry', 'setup_assistant', 'config_ui']
  .map(name => fs.readFileSync(path.join(__dirname, '../../html/js', `${name}.js`), 'utf8'));
const profiles = getDeviceProfiles();
const platforms = ['linux', 'darwin'];
const backends = ['cpu', 'gpu', 'auto', 'gpu-resident'];
const inventories = new Map(), fields = new Set(), actions = new Set();
const clone = value => JSON.parse(JSON.stringify(value));
const flush = () => new Promise(resolve => setImmediate(resolve));
const valueAt = (value, dotted) => dotted.split('.').reduce((item, key) => item?.[key], value);

async function check(platform, type, backend, editable = true) {
  const identity = `${platform}/${type}/${backend}/${editable ? 'editable' : 'read-only'}`;
  let saved = configurationForReceiver(type), revision = 1, writes = 0;
  saved.process.performance.acceleration = backend;
  if (type === 'HackRF') saved.capture.device.serial = ['0000000000000001', '0000000000000002'];
  const dom = new JSDOM('<div id="configuration"></div>', {
    url: 'http://fixture/display/configuration/', runScripts: 'outside-only'});
  const w = dom.window, handlers = new WeakMap();
  const add = w.EventTarget.prototype.addEventListener;
  w.EventTarget.prototype.addEventListener = function (event, ...args) {
    if (!handlers.has(this)) handlers.set(this, new Set());
    handlers.get(this).add(event); return add.call(this, event, ...args);
  };
  w.liveApiUrl = value => value; w.rememberApiPort = () => {}; w.setInterval = () => 0;
  w.fetchStatusResource = (url, options) => w.fetch(url, options);
  w.fetch = async (url, options = {}) => {
    let body = {};
    if (url === '/api/config') body = saved;
    else if (url === '/api/config/capabilities') body = {
      platform, editable, restartAvailable: false, configRevision: String(revision),
      fieldRules: FIELD_RULES, deviceProfiles: profiles,
      processingBackends: {stages: {resident: {selectable: backend === 'gpu-resident',
        qualified: false, executed: false, reason: 'Fixture backend capability'}}}
    };
    else if (url === '/api/config/validate') body = validateConfig(JSON.parse(options.body), saved);
    else if (url.startsWith('/api/config?')) {
      assert.equal(editable, true, `${identity}: read-only configuration cannot save`);
      assert.equal(options.headers['If-Match'], `"${revision}"`);
      saved = JSON.parse(options.body); writes++; revision++;
      body = {ok: true, restarting: false, revision: String(revision)};
    } else assert.ok(!options.method || options.method === 'GET', `${identity}: no hardware actions`);
    return {ok: true, status: 200, headers: {get: key => key === 'ETag' ? `"${revision}"` : null},
      json: async () => clone(body)};
  };
  try {
    sources.forEach((source, index) => w.eval(source + (index === sources.length - 1 ?
      '\nwindow.fixtureDraft = () => JSON.parse(JSON.stringify(activeConfig));' : '')));
    await w.renderConfiguration(); await flush();
    const document = w.document;
    const tabs = [...document.querySelectorAll('[role="tab"]')];
    assert.ok(tabs.length >= 6, `${identity}: all shared settings categories`);
    const inventory = [];
    for (const tab of tabs) {
      tab.click();
      assert.equal(tab.getAttribute('aria-selected'), 'true', `${identity}: ${tab.dataset.tab} selected`);
      assert.equal(document.querySelectorAll('[role="tabpanel"]:not([hidden])').length, 1);
      const panel = document.getElementById(tab.getAttribute('aria-controls'));
      panel.querySelectorAll('details').forEach(item => { item.open = true; });
      for (const field of panel.querySelectorAll('.config-field[data-path]')) {
        const key = field.dataset.path;
        const controls = [...field.querySelectorAll('input:not([type="hidden"]),select,textarea')];
        if (!controls.length) continue;
        assert.ok(FIELD_RULES[key] || key === 'location.rx' || key === 'location.tx',
          `${identity}: ${key} must map to the canonical configuration schema`);
        fields.add(key);
        inventory.push([key, controls.map(control => ({tag: control.tagName,
          type: control.type, disabled: control.disabled,
          options: control.tagName === 'SELECT' ? [...control.options].map(option => [option.value, option.disabled]) : undefined}))]);
        for (const control of controls) {
          const label = control.getAttribute('aria-label') || control.labels?.[0]?.textContent ||
            document.getElementById(control.getAttribute('aria-labelledby'))?.textContent;
          assert.ok(label?.trim(), `${identity}: ${key} ${control.id} needs an accessible label`);
          if (!editable) assert.equal(control.disabled, true, `${identity}: ${key} cannot edit`);
          if (control.disabled) continue;
          let target = control, wired = false;
          while (target) {
            wired ||= ['input', 'change', 'click'].some(event => handlers.get(target)?.has(event) ||
              typeof target[`on${event}`] === 'function');
            target = target.parentNode;
          }
          assert.ok(wired, `${identity}: ${key} ${control.outerHTML} has no input/change handler`);
        }
      }
    }
    for (const button of document.querySelectorAll('button')) {
      const name = button.getAttribute('aria-label') || button.textContent;
      assert.ok(name.trim(), `${identity}: every action has an accessible name`);
      actions.add(name.trim());
      if (!editable && button.closest('.config-field[data-path]'))
        assert.equal(button.disabled, true, `${identity}: setting action ${name} must be disabled`);
      if (button.disabled) continue;
      let target = button, wired = false;
      while (target) { wired ||= handlers.get(target)?.has('click') || typeof target.onclick === 'function'; target = target.parentNode; }
      assert.ok(wired || button.type === 'submit' && button.form,
        `${identity}: action ${name} has no handler`);
    }
    const ids = [...document.querySelectorAll('[id]')].map(element => element.id);
    assert.equal(new Set(ids).size, ids.length, `${identity}: unique control IDs`);
    assert.deepEqual(copyIssues(document.body.textContent), [], `${identity}: useful settings copy`);
    // OS must not create another configuration schema or processing UI.
    const key = `${type}/${backend}/${editable}`;
    if (inventories.has(key)) assert.deepEqual(inventory, inventories.get(key), `${identity}: canonical OS parity`);
    else inventories.set(key, inventory);
    if (!editable) { assert.equal(writes, 0); return; }
    const cpi = document.querySelector('[data-path="process.data.cpi"] input');
    cpi.value = '0.75'; cpi.dispatchEvent(new w.Event('input', {bubbles: true}));
    assert.equal(valueAt(w.fixtureDraft(), 'process.data.cpi'), 0.75, `${identity}: canonical field update`);
    // Unsupported backend settings can be retained even when the full saved
    // configuration requires repair. They must never be silently rewritten.
    assert.equal(valueAt(w.fixtureDraft(), 'process.performance.acceleration'), backend);
    const validation = validateConfig(w.fixtureDraft(), saved);
    assert.equal(validation.valid, true, `${identity}: ${validation.errors.join('; ')}`);
    await w.saveConfiguration();
    assert.equal(writes, 1, `${identity}: save action persists`);
    assert.equal(saved.process.data.cpi, 0.75);
    await w.renderConfiguration();
    assert.equal(document.querySelector('[data-path="process.data.cpi"] input').value, '0.75', `${identity}: reload parity`);
  } finally { w.close(); }
}
(async () => {
  let cases = 0;
  for (const platform of platforms) for (const profile of profiles) {
    for (const backend of backends) { await check(platform, profile.type, backend); cases++; }
    await check(platform, profile.type, 'cpu', false); cases++;
  }
  console.log(`Settings acceptance: ${cases} OS/receiver/backend/access cases, ${fields.size} canonical fields, ${actions.size} named actions; DOM evidence only.`);
})().catch(error => { console.error(error); process.exitCode = 1; });
