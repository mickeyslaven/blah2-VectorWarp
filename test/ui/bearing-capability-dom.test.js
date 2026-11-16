'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {JSDOM} = require('./jsdom');
const {configurationForReceiver} = require('../../api/config-store');
const {getDeviceProfiles, FIELD_RULES} = require('../../api/config-manager');
const {presetRecord} = require('../../html/js/kraken_geometry');

async function check(advertised, editable = true) {
  const config = configurationForReceiver('Kraken');
  config.process.bearing.enable = true;
  config.process.bearing.method = 'covariance_mvdr';
  config.capture.device.array_geometry = {...presetRecord(config, 'pentagon5').geometry,
    mapping_confirmed:true,geometry_confirmed:true,orientation_confirmed:true,x_axis_bearing_deg_true:0};
  config.process.surveillance_fusion.mode = 'coherent_beam_bank';
  const original = JSON.stringify(config);
  const dom = new JSDOM('<div id="configuration"></div>', {
    url:'http://fixture/display/configuration/', runScripts:'outside-only'});
  const w = dom.window;
  w.liveApiUrl = value => value;
  w.fetchStatusResource = (url, options) => w.fetch(url, options);
  w.rememberApiPort = () => {};
  w.setInterval = () => 0;
  w.fetch = async (url, options = {}) => {
    assert.ok(!options.method || options.method === 'GET', 'Rendering must not save or change the receiver');
    return {ok:true, status:200, headers:{get:()=>null}, json:async()=>
      url === '/api/config' ? JSON.parse(original) : url === '/api/config/capabilities' ? {
        editable, restartAvailable:false, fieldRules:FIELD_RULES,
        processingBackends:{stages:{resident:{selectable:advertised}}},
        deviceProfiles:getDeviceProfiles().map(profile => ({...profile,
          features:{...profile.features, bearing:advertised}}))
      } : {}};
  };
  try {
    for (const name of ['processing_capabilities', 'config_profiles', 'kraken_geometry', 'config_ui'])
      w.eval(fs.readFileSync(path.join(__dirname, '../../html/js', `${name}.js`), 'utf8'));
    await w.renderConfiguration();
    const input = key => w.document.querySelector(`[data-path="process.bearing.${key}"] input, [data-path="process.bearing.${key}"] select`);
    const enabled = input('enable'), method = input('method');
    assert.ok(enabled && method, 'Saved bearing settings remain visible');
    assert.equal(enabled.checked, true, 'Unavailable bearing does not erase the saved enable value');
    assert.equal(method.value, config.process.bearing.method, 'Saved method is preserved');
    for (const control of [enabled, method]) {
      assert.equal(control.disabled, advertised !== true || !editable);
      if (advertised !== true) {
        assert.equal(control.dataset.originalDisabled, 'true', 'Save completion cannot re-enable an unavailable control');
        assert.match(control.getAttribute('aria-describedby'), /processing-capability-note/);
      }
    }
    const note = w.document.getElementById('processing-capability-note');
    if (advertised !== true) assert.match(note.textContent, /unavailable.*current backend.*Saved values are preserved/);
    assert.equal(JSON.stringify(config), original);
    assert.equal(w.document.getElementById('config-save').disabled, true, 'Viewing unavailable values does not dirty settings');
  } finally { w.close(); }
}

(async () => {
  for (const advertised of [false, true, undefined]) await check(advertised);
  await check(true, false);
  console.log('Bearing resident capability false/true/missing/read-only DOM gates passed; saved values preserved.');
})().catch(error => { console.error(error); process.exitCode = 1; });
