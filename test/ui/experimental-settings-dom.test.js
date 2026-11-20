'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {JSDOM} = require('./jsdom');
const {configurationForReceiver} = require('../../api/config-store');
const {getDeviceProfiles, FIELD_RULES, validateConfig} = require('../../api/config-manager');
const {experimentalSettingCapability} = require('../../html/js/config_ui');

async function check(receiver, platform) {
  const config = configurationForReceiver(receiver);
  const dom = new JSDOM('<div id="configuration"></div>', {
    url: 'http://fixture/display/configuration/', runScripts: 'outside-only'});
  const w = dom.window;
  w.liveApiUrl = value => value;
  w.fetchStatusResource = (url, options) => w.fetch(url, options);
  w.rememberApiPort = () => {}; w.setInterval = () => 0;
  w.fetch = async (url, options = {}) => {
    assert.ok(!options.method || options.method === 'GET');
    return {ok: true, status: 200, headers: {get: () => null}, json: async () =>
      url === '/api/config' ? structuredClone(config) : url === '/api/config/capabilities' ? {
        editable: true, restartAvailable: false, platform, fieldRules: FIELD_RULES,
        deviceProfiles: getDeviceProfiles()} : {}};
  };
  try {
    for (const name of ['processing_capabilities', 'config_profiles', 'kraken_geometry', 'config_ui'])
      w.eval(fs.readFileSync(path.join(__dirname, '../../html/js', `${name}.js`), 'utf8'));
    await w.renderConfiguration();
    for (const key of ['save.complex_cutout.state',
      'process.tracker.initiate.doppler_ridge_clutter.enable']) {
      const field = w.document.querySelector(`[data-path="${key}"]`);
      assert.ok(field?.closest('.experimental-settings'), `${platform}/${receiver}: ${key} visible in Experimental`);
      const control = field.querySelector('input, select');
      assert.equal(control.disabled, !experimentalSettingCapability(key, config).available);
    }
    assert.equal(config.save.complex_cutout.state, false);
    assert.equal(config.process.tracker.initiate.doppler_ridge_clutter.enable, false);
    for (const acceleration of ['cpu', 'gpu', 'auto', 'gpu-resident']) {
      const draft = structuredClone(config);
      draft.capture.replay.state = true;
      draft.process.performance.acceleration = acceleration;
      assert.equal(experimentalSettingCapability('save.complex_cutout.state', draft).available,
        acceleration === 'cpu' || acceleration === 'gpu');
    }
    for (const key of ['doppler_window', 'input_phase_correction']) {
      assert.deepEqual(FIELD_RULES[`process.multi_illuminator.${key}`].choices,
        [key === 'doppler_window' ? 'rectangular' : 'off']);
      const draft = structuredClone(config);
      draft.process.multi_illuminator[key] =
        key === 'doppler_window' ? 'hann' : 'automatic';
      assert.equal(validateConfig(draft).valid, false);
    }
  } finally { w.close(); }
}
(async () => {
  for (const platform of ['linux', 'darwin'])
    for (const receiver of ['Kraken', 'Usrp', 'HackRF', 'RspDuo']) await check(receiver, platform);
  console.log('Experimental controls: all four receiver profiles × Linux/macOS UI, defaults and route rejection passed (source/DOM evidence only).');
})().catch(error => { console.error(error); process.exitCode = 1; });
