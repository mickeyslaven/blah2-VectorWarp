'use strict';
// Portal actions use an in-memory API; never discover or change real nodes.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {JSDOM} = require('./jsdom');
const {copyIssues} = require('../helpers/ui-acceptance');
const dom = new JSDOM('<div id="configuration"></div>', {url: 'http://fixture/display/configuration/', runScripts: 'outside-only'});
const w = dom.window, calls = [];
let nodes = [], failRemove = true, selected = 'local', confirm = true;
w.selectedNode = () => selected; w.liveApiUrl = value => value; w.confirm = () => confirm;
w.fetchStatusResource = async (url, options = {}) => {
  calls.push({url, ...options});
  if (options.method === 'POST') {
    assert.equal(options.headers['X-VectorWarp-Intent'], 'local-node-add-v1');
    assert.deepEqual(JSON.parse(options.body), {host: '192.0.2.1', port: 3001, label: 'Roof receiver'});
    nodes = [{id: 'roof/id', label: 'Roof receiver', local: false}];
  } else if (options.method === 'DELETE') {
    assert.equal(url, '/api/federation/nodes/roof%2Fid');
    assert.equal(options.headers['X-VectorWarp-Intent'], 'local-node-remove-v1');
    if (failRemove) throw new Error('Connection interrupted; retry removal.');
    nodes = [];
  }
  return {ok: true, json: async () => ({nodes: [{id: 'local', local: true}, ...nodes]})};
};
w.eval(fs.readFileSync(path.join(__dirname, '../../html/js/local_nodes.js'), 'utf8'));
const flush = async () => { await new Promise(resolve => setImmediate(resolve)); };
(async () => {
  try {
    await w.renderLocalNodes(); await flush();
    const panel = w.document.getElementById('local-radar-nodes');
    assert.match(panel.textContent, /No additional local VectorWarp nodes/);
    const form = panel.querySelector('form');
    form.elements.host.value = '192.0.2.1'; form.elements.port.value = '3001'; form.elements.label.value = 'Roof receiver';
    form.dispatchEvent(new w.Event('submit', {bubbles: true, cancelable: true})); await flush();
    assert.equal(form.elements.host.value, ''); assert.equal(form.elements.port.value, '3000');
    assert.equal(panel.querySelector('.local-node-list strong').textContent, 'Roof receiver');
    const remove = panel.querySelector('.local-node-list button');
    confirm = false; remove.click(); await flush();
    assert.equal(calls.filter(call => call.method === 'DELETE').length, 0, 'Cancel preserves the node');
    confirm = true; remove.click(); await flush();
    assert.equal(remove.disabled, false, 'Failed removal can be retried without reloading Settings');
    assert.match(panel.querySelector('[role="status"]').textContent, /Connection interrupted/);
    failRemove = false; remove.click(); await flush();
    assert.match(panel.textContent, /No additional local VectorWarp nodes/);
    assert.equal(panel.querySelector('[role="status"]').textContent, 'Node removed.', 'A successful retry clears the stale error');
    assert.equal(calls.filter(call => call.method === 'DELETE').length, 2);
    assert.deepEqual(copyIssues(panel.textContent), []);
    panel.remove(); selected = 'remote';
    const before = calls.length; await w.renderLocalNodes();
    assert.equal(w.document.getElementById('local-radar-nodes'), null, 'Remote node cannot manage this portal');
    assert.equal(calls.length, before);
    console.log('Local node actions: empty state, add, cancel, remove failure/retry and remote unavailability passed.');
  } finally { w.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
