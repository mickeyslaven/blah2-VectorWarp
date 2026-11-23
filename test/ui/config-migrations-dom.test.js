'use strict';

// Upgrade notices share the header on every page; acknowledgement never writes config.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const {JSDOM} = require('./jsdom');
const source = fs.readFileSync(require.resolve('../../html/js/common.js'), 'utf8');
const receipts = [{schema: 'vectorwarp-config-migration-v1', id: 'migration-one',
  createdAt: '2026-11-23T15:00:00.000Z', standard: 'Current unified radar pipeline',
  previousRevision: 'before', configRevision: 'after', changes: [
    {path: 'process.clutter.enable', before: false, after: true, beforePresent: true,
      afterPresent: true, reason: 'Use the unified clutter stage.'},
    {path: 'process.tracker.confirmation', before: null, after: 3, beforePresent: false,
      afterPresent: true, reason: 'Three measurements confirm a track.'},
    {path: 'process.legacy', before: null, after: null, beforePresent: true,
      afterPresent: false, reason: '<img src=x onerror="throw new Error()"> removed.'}
  ]}];
const clone = value => JSON.parse(JSON.stringify(value));
function page(storage = new Map(), url = 'https://radar.invalid/display/locations/') {
  const dom = new JSDOM('<span id="live-status"></span><div id="history"></div>',
    {url, runScripts: 'outside-only', pretendToBeVisual: true});
  const {window} = dom;
  Object.defineProperty(window, 'localStorage', {value: {
    getItem: key => storage.get(key), setItem: (key, value) => storage.set(key, value)
  }});
  window.eval(source);
  const calls = [];
  window.fetchStatusResource = async (url, options) => {
    calls.push({url, options});
    return {ok: true, json: async () => ({radar: 'no-data', configMigrations: clone(receipts)})};
  };
  return {dom, window, calls, storage, dialog: () => window.document.querySelector('#config-migration-dialog')};
}

(async () => {
  const first = page();
  const untouched = JSON.stringify(receipts);
  await first.window.refreshLiveStatus();
  let dialog = first.dialog();
  assert.ok(dialog?.open, 'The shared status poll announces migration without visiting Settings');
  assert.equal(dialog.getAttribute('aria-labelledby'), 'config-migration-title');
  assert.equal(first.window.document.activeElement.id, 'config-migration-title',
    'Long upgrade notices initially focus their explanation, not the bottom acknowledgement');
  assert.equal(dialog.scrollTop, 0);
  assert.match(dialog.textContent, /Current unified radar pipeline/);
  assert.match(dialog.textContent, /process.clutter.enablefalse → true/);
  assert.match(dialog.textContent, /process.tracker.confirmationnot set → 3/);
  assert.match(dialog.textContent, /process.legacynull → not set/);
  assert.equal(dialog.querySelector('img'), null, 'Receipt values and reasons are rendered as text');
  assert.match(dialog.querySelector('a').href, /\/api\/config\/migrations$/);
  assert.equal(first.storage.size, 0, 'An unacknowledged notice remains pending after navigation');
  await first.window.refreshLiveStatus();
  assert.equal(first.dialog(), dialog, 'Polling retains the open notice');
  dialog.querySelector('button').click();
  await first.window.refreshLiveStatus();
  assert.equal(first.dialog(), null, 'A dismissed receipt is not shown every poll');
  assert.ok(first.calls.every(call => !call.options?.method || call.options.method === 'GET'));
  assert.equal(JSON.stringify(receipts), untouched, 'Notice and acknowledgement do not change configuration evidence');
  first.dom.window.close();

  const next = page(first.storage, 'https://radar.invalid/display/configuration/');
  await next.window.refreshLiveStatus();
  assert.equal(next.dialog(), null, 'Acknowledgement survives navigation and a backend restart');
  const history = next.window.document.getElementById('history');
  next.window.renderConfigMigrationHistory(history, receipts);
  assert.equal(history.querySelector('button').textContent, 'Upgrade changes');
  history.querySelector('button').click();
  assert.match(next.dialog().textContent, /Upgrade changes/);
  assert.equal(next.dialog().querySelectorAll('li').length, 3, 'History keeps every exact field change');
  next.dialog().dispatchEvent(new next.window.Event('cancel', {cancelable: true}));
  assert.equal(next.dialog(), null, 'Escape acknowledges the notice');
  const secondReceipt = {...clone(receipts[0]), id: 'migration-two'};
  next.window.showConfigMigrations([...receipts, secondReceipt]);
  assert.equal(next.dialog().querySelectorAll('h3').length, 1, 'A subsequent upgrade shows only its new receipt');
  next.dialog().querySelector('button').click();
  next.window.showConfigMigrations([...receipts, secondReceipt]);
  assert.equal(next.dialog(), null);
  next.dom.window.close();

  const remote = page(first.storage, 'https://radar.invalid/?federationNode=peer');
  remote.window.showConfigMigrations(receipts);
  assert.ok(remote.dialog(), 'Acknowledging a local installation does not hide a peer receipt');
  assert.match(remote.dialog().querySelector('a').href, /\/api\/federation\/nodes\/peer\/api\/config\/migrations$/);
  remote.dom.window.close();

  const blocked = page();
  blocked.window.localStorage.getItem = () => { throw new Error('Storage disabled'); };
  blocked.window.localStorage.setItem = () => { throw new Error('Storage disabled'); };
  const other = blocked.window.document.createElement('dialog'); other.setAttribute('open', '');
  blocked.window.document.body.appendChild(other);
  blocked.window.showConfigMigrations(receipts);
  assert.equal(blocked.dialog(), null, 'A migration notice waits for an existing dialog');
  other.remove(); blocked.window.showConfigMigrations(receipts);
  assert.ok(blocked.dialog()); blocked.dialog().querySelector('button').click();
  blocked.window.showConfigMigrations(receipts);
  assert.equal(blocked.dialog(), null, 'Disabled storage does not cause repeated notices during polling');
  blocked.window.showConfigMigrations(undefined);
  blocked.window.renderConfigMigrationHistory(blocked.window.document.getElementById('history'), []);
  assert.equal(blocked.window.document.querySelector('#config-migration-history'), null);
  blocked.window.renderConfigMigrationHistory(blocked.window.document.getElementById('history'), [],
    {configMigrationError: 'Migration receipt is invalid'});
  blocked.window.document.querySelector('#config-migration-history').click();
  assert.match(blocked.dialog().textContent, /Upgrade history unavailable: Migration receipt is invalid/);
  blocked.dialog().querySelector('button').click();
  blocked.window.showConfigMigrations(receipts, true, {configMigrationOmitted: 2});
  assert.match(blocked.dialog().textContent, /2 earlier receipts are no longer retained/);
  blocked.dom.window.close();
  const reviewPage = page();
  const radarPlanReview = {required: true, id: 'old-pin-review',
    message: 'The saved receiver configuration is retained. Do not repeat receiver setup.',
    contractFile: '/operator/config.yml.radar-plan.json',
    previewCommand: ['node', 'api/config-store.js', 'pin-preview', 'config.yml', 'tracking=true'],
    acceptCommand: ['node', 'api/config-store.js', 'accept', 'config.yml', 'REVIEWED_REQUIREMENT.json'],
    instructions: 'Review the preview independently, verify with accept, then install the reviewed file. Neither command installs or weakens a pin.'};
  reviewPage.window.fetchStatusResource = async () => ({ok: true,
    json: async () => ({radar: 'no-data', configMigrations: [], radarPlanReview})});
  await reviewPage.window.refreshLiveStatus();
  assert.match(reviewPage.dialog().textContent, /Radar plan review required/);
  assert.match(reviewPage.dialog().textContent, /Do not repeat receiver setup/);
  assert.match(reviewPage.dialog().textContent, /pin-preview/);
  assert.match(reviewPage.dialog().textContent, /tracking=true/);
  assert.match(reviewPage.dialog().textContent, /REVIEWED_REQUIREMENT.json/);
  assert.match(reviewPage.dialog().textContent, /Neither command installs or weakens a pin/);
  reviewPage.dialog().querySelector('button').click();
  await reviewPage.window.refreshLiveStatus();
  assert.equal(reviewPage.dialog(), null, 'plan review is announced once per exact revision and pin');
  reviewPage.window.renderConfigMigrationHistory(reviewPage.window.document.getElementById('history'), [], {radarPlanReview});
  assert.equal(reviewPage.window.document.querySelector('#config-migration-history').textContent, 'Review radar plan');
  reviewPage.window.document.querySelector('#config-migration-history').click();
  assert.ok(reviewPage.dialog(), 'review instructions remain available in Settings after acknowledgement');
  reviewPage.dom.window.close();
  console.log('Config migration notice, exact evidence, acknowledgement and node-scope tests passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
