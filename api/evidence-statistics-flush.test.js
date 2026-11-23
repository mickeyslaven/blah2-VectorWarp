'use strict';
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {EvidenceStatistics, StatisticsFlushScheduler, nextPhase} = require('./evidence-statistics');

let clock = 1000, serial = 0, timers = new Map();
const scheduler = new StatisticsFlushScheduler({now: () => clock,
  setTimeoutFn: (fn, delay) => { const id = ++serial; timers.set(id, {fn, at: clock + delay}); return id; },
  clearTimeoutFn: id => timers.delete(id)});
const advance = () => { const [id, item] = [...timers].sort((a, b) => a[1].at - b[1].at)[0];
  timers.delete(id); clock = item.at; item.fn(); };
const calls = [], owners = Array.from({length: 4}, () => ({}));
for (const owner of owners) {
  scheduler.request(owner, () => { calls.push(clock); clock += 20; });
  scheduler.request(owner, () => assert.fail('duplicate queued request'));
}
while (timers.size) advance();
assert.deepEqual(calls, [1000, 1270, 1540, 1810], 'collisions flush in distinct turns with post-write spacing');
scheduler.request(owners[0], () => assert.fail('cancelled write')); scheduler.cancel(owners[0]);
assert.equal(timers.size, 0);
assert.equal(nextPhase(30123, 123), 60123);
assert.equal(nextPhase(91789, 123), 120123, 'late writes do not drift the anchored phase');

const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'statistics-flush-'));
try {
  const filename = path.join(directory, 'ledger.json');
  const config = {}, source = {async get() { return {}; }};
  const live = new EvidenceStatistics(config, source, {filename, now: () => clock, flushScheduler: scheduler});
  live.state.future = {preserved: true};
  live.state.associationDiagnostics = {detection: {}, tracker: {}};
  live.persist();
  assert.equal(fs.readFileSync(filename, 'utf8'), `${JSON.stringify(live.state)}\n`,
    'existing legacy JSON bytes and schema stay unchanged');
  const recovered = new EvidenceStatistics(config, source, {filename, now: () => clock, flushScheduler: scheduler});
  assert.equal(JSON.stringify(recovered.state), JSON.stringify(live.state), 'restart state equivalence');
  assert.deepEqual(recovered.status(), live.status(), 'public status equivalence');
  recovered.close();
  const before = fs.readFileSync(filename, 'utf8');
  live.fs = {...fs, renameSync() { throw new Error('atomic rename rejected'); }};
  live.persist();
  assert.equal(fs.readFileSync(filename, 'utf8'), before, 'failed rename preserves prior durable ledger');
  assert.match(live.state.error, /rename rejected/); live.fs = fs;
  live.state.future = {latest: 3};
  scheduler.request(live, () => assert.fail('shutdown must cancel queued callback'));
  live.close();
  assert.equal(timers.size, 0);
  assert.equal(JSON.parse(fs.readFileSync(filename, 'utf8')).future.latest, 3,
    'close synchronously persists latest state');
} finally { fs.rmSync(directory, {recursive: true, force: true}); }
console.log('PASS phase-anchored shared statistics flush, legacy bytes/restart, atomic failure and synchronous shutdown');
