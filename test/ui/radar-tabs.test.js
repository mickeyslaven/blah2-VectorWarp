'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const {JSDOM} = require('./jsdom');

const dom = new JSDOM('<body data-radar-view="range-doppler"><section id="panel"></section></body>', {runScripts: 'outside-only',
  url: 'http://radar.invalid/'});
dom.window.eval(fs.readFileSync(path.join(__dirname, '../../html/js/common.js'), 'utf8'));
const panel = dom.window.document.getElementById('panel');
dom.window.addRadarViewControl(panel, panel);

const control = panel.querySelector('.radar-view-control');
assert.ok(control);
assert.equal(control.querySelector('summary').getAttribute('aria-label'), 'Choose radar map');
assert.equal(control.querySelectorAll('a').length, 5);
assert.equal(control.querySelector('[aria-current="page"]').textContent, 'Range–Doppler');
assert.deepEqual([...control.querySelectorAll('a')].map(link => link.textContent),
  ['Live delay–Doppler', 'Decaying max-hold', 'Range over time', 'Doppler over time',
    'Range–Doppler']);
console.log('All delay/Doppler views are directly available from the plot menu.');
