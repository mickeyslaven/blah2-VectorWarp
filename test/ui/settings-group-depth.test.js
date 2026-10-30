'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const {JSDOM} = require('./jsdom');

const dom = new JSDOM('<main id="root"></main>', {runScripts: 'outside-only',
  url: 'http://radar.invalid/display/configuration'});
const {window} = dom;
window.KrakenGeometry = null;
window.VectorWarpConfigProfiles = {};
window.eval(fs.readFileSync(path.join(__dirname, '../../html/js/config_ui.js'), 'utf8'));

function disclosure(fields) {
  const details = window.document.createElement('details');
  details.className = 'config-group';
  const summary = window.document.createElement('summary');
  summary.innerHTML = '<span><strong>Group</strong><small>Settings</small></span>';
  const content = window.document.createElement('div');
  content.className = 'config-fields';
  for (let index = 0; index < fields; index += 1) {
    const field = window.document.createElement('div');
    field.className = 'config-field';
    content.appendChild(field);
  }
  details.append(summary, content);
  return details;
}

const root = window.document.getElementById('root');
root.append(disclosure(1), disclosure(2), disclosure(3));
window.revealSmallSettingsGroups(root);

assert.deepEqual([...root.children].map(element => element.tagName), ['SECTION', 'SECTION', 'DETAILS']);
assert.equal(root.querySelectorAll('details.config-group').length, 1,
  'Only a group containing at least three settings remains collapsible');
assert.equal(root.querySelectorAll('.config-group-inline').length, 2,
  'One- and two-setting groups remain visible without another click');
assert.equal(root.children[0].querySelector('.config-group-heading'), null,
  'A one-setting group does not repeat a heading above its field label');
assert.ok(root.children[0].classList.contains('config-group-single'));
assert.equal(root.querySelectorAll('.config-field').length, 6,
  'Changing disclosure depth preserves every setting');
const panels = window.document.createElement('div');
panels.innerHTML = '<section id="settings-panel-capture"><details class="kraken-geometry"></details></section>' +
  '<section id="settings-panel-array"></section>';
assert.equal(window.moveArrayEditorToArrayTab(panels, true), true);
assert.ok(panels.querySelector('#settings-panel-array > .kraken-geometry'),
  'Coherent-array geometry moves into its own visual settings tab');
assert.ok(panels.querySelector('#settings-panel-array > .kraken-geometry').open,
  'The dedicated Array tab exposes the editor without another disclosure');
assert.ok(panels.querySelector('#settings-panel-array > .kraken-geometry').classList.contains(
  'kraken-geometry-standalone'));
assert.equal(panels.querySelector('#settings-panel-capture .kraken-geometry'), null,
  'Receiver is no longer burdened with the full array editor');
console.log('Single controls drop redundant headings; array geometry has its own tab.');
