'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const {JSDOM} = require('./jsdom');

const dom = new JSDOM('<!doctype html><body></body>', {
  url: 'http://127.0.0.1:3000/display/configuration/', runScripts: 'outside-only'
});
const window = dom.window;
window.KrakenGeometry = {};
window.VectorWarpConfigProfiles = {};
window.eval(fs.readFileSync(path.join(__dirname, '../../html/js/config_ui.js'), 'utf8'));

const tabs = window.document.createElement('div');
tabs.innerHTML = '<button role="tab" data-tab="process" aria-selected="true">Radar</button>' +
  '<button role="tab" data-tab="capture" aria-selected="false">Receiver</button>';
const panels = window.document.createElement('div');
panels.innerHTML = '<section role="tabpanel" data-tab="process">Bearing is configured in Array setup.</section>' +
  '<section role="tabpanel" data-tab="capture" hidden>' +
    '<details data-config-group="capture.device">' +
      '<summary>Receiver</summary>' +
      '<details data-config-group="capture.device.array_geometry">' +
        '<summary>Array setup</summary>' +
        '<details data-config-group="process.bearing">' +
          '<summary>Bearing</summary>' +
          '<div class="config-field" data-path="process.bearing.min_consistent">' +
            'Bearing consistency samples<input>' +
          '</div>' +
        '</details>' +
      '</details>' +
    '</details>' +
  '</section>';
const activate = (selectedKey, remember = true) => {
  tabs.querySelectorAll('[role="tab"]').forEach(tab =>
    tab.setAttribute('aria-selected', String(tab.dataset.tab === selectedKey)));
  panels.querySelectorAll('[role="tabpanel"]').forEach(panel => {
    panel.hidden = panel.dataset.tab !== selectedKey;
  });
  if (remember) window.sessionStorage.setItem('blah2-settings-tab', selectedKey);
};
activate('process');
const toolbar = window.makeSettingsFilter(tabs, panels, activate);
window.document.body.append(toolbar, tabs, panels);

(async () => {
  try {
    await new Promise(resolve => window.setTimeout(resolve, 0));
    const field = panels.querySelector('[data-path="process.bearing.min_consistent"]');
    const ancestors = [...panels.querySelectorAll('details')];
    let scrollOptions = null;
    field.scrollIntoView = options => { scrollOptions = options; };
    const input = toolbar.querySelector('input[type="search"]');
    input.value = 'Bearing';
    input.dispatchEvent(new window.Event('input', {bubbles: true}));

    assert.equal(tabs.querySelector('[aria-selected="true"]').dataset.tab, 'capture',
      'Search activates the tab containing the nested result');
    assert.ok(ancestors.every(details => details.open),
      'Search opens every nested details disclosure containing the result');
    assert.ok(field.classList.contains('settings-search-match'),
      'Search highlights the matching setting');
    assert.deepEqual(scrollOptions, {block: 'center', inline: 'nearest'},
      'Search scrolls the newly revealed setting into view');
    assert.equal(window.sessionStorage.getItem('blah2-settings-tab'), 'process',
      'Automatic search navigation preserves the saved tab');

    input.value = '';
    input.dispatchEvent(new window.Event('input', {bubbles: true}));
    assert.equal(tabs.querySelector('[aria-selected="true"]').dataset.tab, 'process',
      'Clearing search restores the selected tab');
    assert.ok(ancestors.every(details => !details.open),
      'Clearing search restores nested disclosure state');
    assert.ok(!field.classList.contains('settings-search-match'),
      'Clearing search removes the match highlight');
    console.log('Settings search DOM: nested tab/disclosures, highlight, scroll and state restoration passed.');
  } finally { window.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
