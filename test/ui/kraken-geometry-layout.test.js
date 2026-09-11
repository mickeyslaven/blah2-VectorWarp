'use strict';
// Limited fallback: jsdom checks CSS/DOM constraints, NOT rendered dimensions.
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {JSDOM} = require('jsdom');
const {getDeviceProfiles} = require('../../api/config-manager');
const {setupDefaults} = require('../../api/config-store');
const {applyDeviceProfile} = require('../../html/js/config_ui');
const helper = require('../../html/js/kraken_geometry');
const root = path.resolve(__dirname, '../..');
const evidence = fs.mkdtempSync(path.join(os.tmpdir(), 'kraken-geometry-layout-evidence-'));
const css = ['html/lib/blah2.css', 'html/lib/kraken_geometry.css'].map(file => fs.readFileSync(path.join(root, file), 'utf8')).join('\n');
const results = [];
for (const width of [1440, 390, 320]) {
  const config = applyDeviceProfile(setupDefaults(), getDeviceProfiles()[0]);
  config.capture.device.channel_count = 8;
  config.capture.device.reference_channel = 6;
  config.capture.device.surveillance_channels = [0, 1, 2, 3, 4, 5, 7];
  config.process.reference_synthesis.mode = 'dedicated';
  config.process.reference_synthesis.channels = [6];
  const geometry = helper.emptyRecord(8);
  geometry.shape = 'unsupported_saved_shape';
  geometry.elements.forEach((element, index) => {
    element.daq_channel = index;
    element.receiver_serial = 'operator-recorded-serial-'.repeat(4);
    element.position = [index / 10, 0, 0];
  });
  config.capture.device.array_geometry = geometry;
  const dom = new JSDOM(`<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1"><style>${css}</style></head><body><main class="app-shell config-shell" style="max-width:${width}px"><p>CSS/DOM fixture only — render this file in a real browser to inspect layout.</p><div class="config-tab-panel"><details class="config-group" open><summary>Receiver</summary><div id="geometry-root"></div></details></div></main></body></html>`, {pretendToBeVisual: true});
  const doc = dom.window.document;
  doc.getElementById('geometry-root').appendChild(helper.render(doc, config, () => {}, true));
  assert.equal(doc.querySelectorAll('.geometry-element').length, 8);
  const invalid = doc.querySelector('[data-path="capture.device.array_geometry.shape"]');
  invalid.classList.add('invalid');
  const input = invalid.querySelector('select'); input.setAttribute('aria-invalid', 'true');
  invalid.querySelector('.config-field-error').textContent = 'Unsupported saved shape: choose a supported value. The original operator record remains unchanged.';
  for (const field of doc.querySelectorAll('.geometry-element .config-field')) {
    const style = dom.window.getComputedStyle(field);
    assert.equal(style.gridTemplateColumns, 'minmax(0, 1fr)', 'Element cells must override the parent two-column minimum');
    assert.equal(style.minWidth, '0');
    const control = field.querySelector('input,select');
    const controlStyle = dom.window.getComputedStyle(control);
    assert.equal(controlStyle.minWidth, '0');
    assert.equal(controlStyle.maxWidth, '100%');
    assert.equal(controlStyle.boxSizing, 'border-box');
    assert.ok(field.querySelector('label').htmlFor);
    if (control.type !== 'checkbox') assert.equal(controlStyle.width, '100%');
    else assert.equal(controlStyle.width, '18px');
  }
  const elementGrid = dom.window.getComputedStyle(doc.querySelector('.geometry-element')).gridTemplateColumns;
  assert.equal(elementGrid, 'repeat(auto-fit, minmax(min(100%, 180px), 1fr))');
  assert.equal(dom.window.getComputedStyle(invalid.querySelector('.config-field-error')).overflowWrap, 'anywhere');
  const filename = path.join(evidence, `geometry-8ch-${width}-css-dom-fixture.html`);
  fs.writeFileSync(filename, dom.serialize());
  results.push({intendedViewportWidth: width, elementRows: 8, errorLabel: true,
    elementFieldColumns: 'minmax(0, 1fr)', boundedControls: true, artifact: filename,
    renderedBrowser: false, pixelDimensionsMeasured: false});
  dom.window.close();
}
fs.writeFileSync(path.join(evidence, 'evidence.json'), JSON.stringify({kind: 'CSS/DOM fallback only', results}, null, 2));
console.log(JSON.stringify({passed: true, evidence, renderedBrowser: false, pixelDimensionsMeasured: false}));
