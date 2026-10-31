'use strict';

const assert = require('assert/strict');
const fs = require('fs');
const path = require('path');
const {JSDOM} = require('./jsdom');
const {configurationForReceiver} = require('../../api/config-store');
const {getDeviceProfiles, FIELD_RULES, validateConfig} = require('../../api/config-manager');

const dom = new JSDOM('<header class="app-header"></header><div id="configuration"></div>', {
  url: 'http://127.0.0.1:3000/display/configuration/', runScripts: 'outside-only', pretendToBeVisual: true
});
const {window} = dom;
const saved = configurationForReceiver('RspDuo');
const channel = () => ({active: false, detectedCount: 0, correctedCount: 0,
  lastDetectedAt: 0, lastCorrectedAt: 0});
const live = () => ({radar: 'receiving', errors: [], processorFresh: true,
  processor: {receiver: 'RspDuo', input: 'live', state: 'live', receiverHealth: {
    schema: 1, receiver: 'RspDuo', monitoring: true,
    overload: {A: channel(), B: channel()}, ackFailures: 0}}});
let status = live(), statusFails = false, writeCalls = 0;
window.setInterval = () => 0;
window.fetch = async (url, options = {}) => {
  let body;
  if (options.method && options.method !== 'GET' && url !== '/api/config/validate') writeCalls++;
  if (url === '/api/config') body = saved;
  else if (url === '/api/config/capabilities')
    body = {editable: true, deviceProfiles: getDeviceProfiles(), fieldRules: FIELD_RULES};
  else if (url === '/api/config/validate') body = validateConfig(JSON.parse(options.body), saved);
  else if (url.startsWith('/api/system/status')) {
    if (statusFails) throw new Error('Connection lost');
    body = status;
  } else if (url === '/api/upstream/status') body = {supported: false};
  else throw new Error(`Unexpected ${url}`);
  return {ok: true, status: 200, headers: {get: () => '"' + 'a'.repeat(64) + '"'},
    json: async () => JSON.parse(JSON.stringify(body))};
};
for (const file of ['common.js', 'config_profiles.js', 'kraken_geometry.js', 'config_ui.js'])
  window.eval(fs.readFileSync(path.join(__dirname, '../../html/js', file), 'utf8'));
window.liveApiUrl = value => value;
window.fetchStatusResource = (url, options) => window.fetch(url, options);
window.rememberApiPort = () => {};
const element = id => window.document.getElementById(id);
const update = async () => {
  await window.refreshLiveStatus();
  await window.refreshConfigDiagnostics();
};
const assertBoth = pattern => {
  assert.match(element('receiver-health-status').textContent, pattern);
  assert.match(element('receiver-health').textContent, pattern);
};

(async () => {
  try {
    window.renderAppHeader('settings');
    await new Promise(resolve => setImmediate(resolve));
    await window.renderConfiguration();
    await update();
    assertBoth(/A \(reference\): no overload reported.*B \(surveillance\): no overload reported/i);
    assert.equal(element('receiver-health-status').getAttribute('role'), 'status');
    assert.equal(element('receiver-health-status').getAttribute('aria-live'), 'polite');
    assert.equal(element('receiver-health-status').hidden, false);
    assert.match(element('receiver-health').textContent, /No overload reported” does not guarantee RF quality/i);
    assert.match(element('receiver-health').textContent, /gain reduction with AGC off/);
    assert.match(element('receiver-health').textContent, /more negative AGC target/);
    assert.equal(element('receiver-health-status').querySelector('a').getAttribute('href'), '/display/configuration');
    const unchanged = element('receiver-health-status').firstChild;
    await update();
    assert.equal(element('receiver-health-status').firstChild, unchanged,
      'Identical polling must not replace or re-announce the live status');

    const now = Date.now();
    Object.assign(status.processor.receiverHealth.overload.A,
      {active: true, detectedCount: 1, lastDetectedAt: now - 1000});
    await update();
    assertBoth(/A \(reference\): overload active \(1 detected\)/i);
    assert.equal(element('receiver-health-status').dataset.severity, 'bad');
    assert.match(element('receiver-health').textContent, /Detected: 1 · Recovered: 0/);

    // B detects and recovers entirely between browser polls. Its count and
    // callback timestamps must remain visible although active was never seen.
    Object.assign(status.processor.receiverHealth.overload.A,
      {active: false, correctedCount: 1, lastCorrectedAt: now});
    Object.assign(status.processor.receiverHealth.overload.B,
      {detectedCount: 4, correctedCount: 4, lastDetectedAt: now - 500, lastCorrectedAt: now});
    await update();
    assertBoth(/A \(reference\): recently recovered.*B \(surveillance\): recently recovered \(4 detected\)/i);
    assert.equal(element('receiver-health-status').dataset.severity, 'warning');
    assert.match(element('receiver-health').textContent, /Detected: 4 · Recovered: 4/);
    assert.match(element('receiver-health').textContent, /Last detected:.*Last recovered:/);
    const later = window.rspDuoOverloadStatus(status, false, now + 60001);
    assert.match(later.message, /B \(surveillance\): recovered \(4 detected\)/);
    assert.doesNotMatch(later.message, /recently/);

    status = live();
    status.processor.receiverHealth.ackFailures = 2;
    await update();
    assertBoth(/SDK acknowledgement failures: 2; overload reports may be incomplete/);
    assert.equal(element('receiver-health-status').dataset.severity, 'warning');
    Object.assign(status.processor.receiverHealth.overload.B,
      {active: true, detectedCount: 1, lastDetectedAt: now});
    await update();
    assertBoth(/A \(reference\): no overload reported.*B \(surveillance\): overload active/i);
    assert.equal(element('receiver-health-status').dataset.severity, 'bad');

    for (const change of [
      value => { value.processorFresh = false; },
      value => { value.processor.state = 'stopped'; },
      value => { value.processor.state = 'error'; value.processor.error = 'Device lost'; },
      value => { value.processor.input = 'replay'; },
      value => { value.processor.receiverHealth.monitoring = false; },
      value => { value.restart = {state: 'running'}; }
    ]) {
      status = live(); change(status); await update();
      assertBoth(/unknown|unavailable/i);
      assert.equal(element('receiver-health-status').dataset.severity, 'unavailable');
      assert.equal(element('receiver-health').querySelectorAll('.receiver-health-channels > div').length, 0,
        'Unavailable status must not display previous channel readings as current');
    }

    for (const change of [
      value => { delete value.processor.receiverHealth; },
      value => { value.processor.receiverHealth.schema = 2; },
      value => { delete value.processor.receiverHealth.overload.B; },
      value => { value.processor.receiverHealth.overload.A.active = 'false'; },
      value => { value.processor.receiverHealth.overload.A.active = true; },
      value => { value.processor.receiverHealth.overload.A.correctedCount = 1; },
      value => { value.processor.receiverHealth.overload.A.detectedCount = 1; },
      value => { value.processor.receiverHealth.overload.A.lastDetectedAt = now; },
      value => { value.processor.receiverHealth.overload.A.lastCorrectedAt = now; },
      value => { Object.assign(value.processor.receiverHealth.overload.A,
        {active: true, detectedCount: 1}); },
      value => { Object.assign(value.processor.receiverHealth.overload.A,
        {detectedCount: 1, correctedCount: 1, lastDetectedAt: now}); }
    ]) {
      status = live(); change(status); await update();
      assertBoth(/telemetry unavailable.*older installed receiver module may need a rebuild/i);
    }

    status = live(); await update();
    statusFails = true; await update();
    assertBoth(/No current RSPduo status.*unavailable/);
    statusFails = false;
    status = live(); status.processor.receiver = 'Kraken'; await update();
    assert.equal(element('receiver-health-status').hidden, true,
      'Other receivers must not inherit RSPduo health');
    assert.match(element('receiver-health').textContent, /not the running receiver/);
    status = live(); await update();
    assertBoth(/no overload reported/i);
    assert.doesNotMatch(element('receiver-health').textContent, /Detected: 4/,
      'A new receiver run must use its own counters');
    assert.equal(writeCalls, 0, 'Health polling must not change settings or receiver gain');
    console.log('RSPduo overload header, Settings, recovery, freshness and compatibility DOM tests passed.');
  } finally { window.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
