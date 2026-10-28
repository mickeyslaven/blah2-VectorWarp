'use strict';
const assert = require('assert/strict');
const fs = require('fs');
const path = require('path');
const {JSDOM} = require('./jsdom');

const html = fs.readFileSync(path.join(__dirname, '../../html/display/recordings/index.html'), 'utf8')
  .replace(/<script[\s\S]*?<\/script>/g, '');
const dom = new JSDOM(html, {url: 'http://radar.local/display/recordings/', runScripts: 'outside-only'});
const {window} = dom;
Object.defineProperty(window, 'innerWidth', {value: 390, configurable: true});
window.liveApiUrl = value => value;
window.recordingDuration = milliseconds => `00:00:${String(Math.floor(milliseconds / 1000)).padStart(2, '0')}`;
window.scrollTo = () => {};
window.confirm = () => true;
window.alert = message => { throw new Error(message); };
const calls = [];
const item = index => ({id: `id-${index}`, filename: `recording-${index}.blah2iq`,
  replay_file: `/recordings/recording-${index}.blah2iq`, title: `TEST${index}`,
  created_at: Date.now() - index, duration_seconds: 4, size_bytes: 1024,
  frequency_hz: 509000000, receiver: 'Kraken', channels: 5, kind: 'manual',
  complete: true, aircraft: []});
window.fetchStatusResource = async (url, options = {}) => {
  calls.push({url: String(url), options});
  if (String(url).startsWith('/api/recordings?')) {
    const query = new URL(String(url), 'http://radar.local').searchParams;
    const offset = Number(query.get('offset'));
    return {ok: true, json: async () => ({total: 15, offset, limit: 10,
      indexing: false, metadata_indexing: false,
      storage: {recording_bytes: 15360, free_bytes: 999999},
      retention: {automaticCleanup: false, lowSpacePolicy: 'stop_recording', minimumFreeGb: 5},
      recordings: Array.from({length: Math.min(10, 15 - offset)}, (_, i) => item(offset + i))})};
  }
  if (String(url).startsWith('/api/recordings/import'))
    return {ok: true, json: async () => ({filename: 'import.blah2iq', size_bytes: 4})};
  if (options.method === 'DELETE') return {ok: true, json: async () => ({removed: true})};
  throw new Error(`Unexpected request ${url}`);
};
window.eval(fs.readFileSync(path.join(__dirname, '../../html/js/recordings.js'), 'utf8'));

(async () => {
  window.renderRecordings();
  await new Promise(resolve => setImmediate(resolve));
  assert.match(calls[0].url, /limit=10/, 'Mobile requests a bounded compact page');
  assert.equal(window.document.querySelectorAll('.recording-card').length, 10);
  assert.equal(window.document.querySelector('.recording-card').open, false,
    'Mobile details start collapsed');
  assert.equal(window.document.body.textContent.includes('Download'), false,
    'Local browser does not offer redundant downloads');
  window.document.getElementById('recording-next').click();
  await new Promise(resolve => setImmediate(resolve));
  assert.match(calls.at(-1).url, /offset=10/);
  assert.equal(window.document.querySelectorAll('.recording-card').length, 5);
  assert.equal(window.document.getElementById('recording-next').disabled, true);
  const remove = window.document.querySelector('.recording-delete');
  remove.click(); await new Promise(resolve => setImmediate(resolve));
  assert.ok(calls.some(call => call.options.method === 'DELETE'));
  console.log('Recording browser mobile pagination, compact layout and deletion passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
