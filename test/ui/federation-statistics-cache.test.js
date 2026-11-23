'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
let now = 1000;
const context = vm.createContext({URL, URLSearchParams, console,
  Date: class extends Date {static now() {return now;}},
  window: {location: new URL('http://display.invalid:3000/?scope=all')}});
vm.runInContext(fs.readFileSync(require.resolve('../../html/js/common.js'), 'utf8'), context);
const fleet = {nodes: [{id:'display', role:'coordinator', local:true},
  {id:'alpha',label:'Alpha'}, {id:'beta',label:'Beta'}]};
const modes = new Map();
let reads = 0;
context.fetchCachedJsonResource = async (url, options, policy) => {
  reads++;
  assert.equal(policy.staleMs, 0, 'Freshness must not be concealed by the generic JSON cache');
  assert.ok(!url.includes('/nodes/display/'));
  const id = url.includes('/nodes/alpha/') ? 'alpha' : 'beta';
  const mode = modes.get(id);
  if (mode === 'offline') throw new Error('transport timeout');
  return url.includes('/system/status') ? {processorFresh: mode !== 'stopped',
    processor: {state: mode === 'stopped' ? 'stopped' : 'live'}, radar: 'degraded',
    evidenceHealth: {status: mode === 'evidence-error' ? 'degraded' : 'healthy'}} :
    {channels: [{id, updated_at: now}]};
};
const read = () => context.readFederatedRadar('/api/live-scorecard/association-basis?display=kiosk', fleet);
(async () => {
  for (const path of ['/api/live-aircraft-associations',
    '/api/federation/live-aircraft-associations',
    '/api/federation/nodes/alpha/api/live-aircraft-associations']) {
    const policy=context.clientJsonCachePolicy(path);
    assert.equal(policy.staleMs,0,'Current association flags must not replay a five-minute stale snapshot');
    assert.equal(policy.storage,null);
  }
  let rows = await read();
  assert.equal(rows.length, 2);
  assert.equal(reads, 4, 'Coordinator is not queried as a radar');
  const initial = rows[0].value;
  modes.set('alpha','offline'); now += 30000;
  rows = await read();
  assert.equal(rows.length, 2);
  assert.equal(rows[0].stale, true);
  assert.equal(rows[0].value, initial, 'A missed 30-second refresh keeps the exact last-good snapshot');
  now += 30000;
  assert.equal((await read())[0].stale_at, 1000, 'Repeated failure never rebases freshness');
  now += 31000;
  assert.deepEqual(Array.from(await read(), row => row.node.id), ['beta'], 'Expired nodes are removed');
  modes.clear(); now += 1000; await read();
  modes.set('alpha','stopped');
  rows = await read();
  assert.deepEqual(Array.from(rows, row => row.node.id), ['beta']);
  assert.deepEqual(Array.from(rows.unavailableNodes), ['alpha']);
  modes.set('alpha','offline');
  assert.equal((await read()).length, 1, 'An explicit stop invalidates cached evidence before a subsequent outage');
  modes.clear();
  assert.ok((await read()).every(row => !row.stale), 'Recovery restores current rows');
  modes.set('alpha','evidence-error');
  rows = await read();
  assert.deepEqual(Array.from(rows, row => row.node.id), ['beta']);
  assert.deepEqual(Array.from(rows.statisticsUnavailableNodes), ['alpha'],
    'A live processor cannot hide a failed statistics ledger behind zero counters');
  console.log('Federation statistics cache: bounded last-good, explicit stop, coordinator, expiry and recovery passed.');
})().catch(error => {console.error(error);process.exitCode=1;});
