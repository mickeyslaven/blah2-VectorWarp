'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const {JSDOM} = require('./jsdom');

const dom = new JSDOM('<nav data-radar-view-tabs hidden></nav>', {runScripts: 'outside-only',
  url: 'http://radar.invalid/'});
dom.window.eval(fs.readFileSync(path.join(__dirname, '../../html/js/common.js'), 'utf8'));
dom.window.renderRadarViewTabs('tracks');

const nav = dom.window.document.querySelector('[data-radar-view-tabs]');
assert.equal(nav.hidden, false);
assert.equal(nav.querySelectorAll('a').length, 5);
assert.equal(nav.querySelector('[aria-current="page"]').textContent, 'Tracks');
assert.deepEqual([...nav.querySelectorAll('a')].map(link => link.textContent),
  ['Live radar', 'Detections', 'Tracks', 'History', 'ADS-B truth']);
console.log('Delay/Doppler views share the Live radar tab bar.');
