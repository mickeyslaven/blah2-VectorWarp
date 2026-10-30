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
assert.equal(root.querySelectorAll('.config-field').length, 6,
  'Changing disclosure depth preserves every setting');
console.log('Small settings groups render directly; larger groups stay collapsible.');
