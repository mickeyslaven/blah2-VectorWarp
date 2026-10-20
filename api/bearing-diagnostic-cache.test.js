'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {EventEmitter} = require('events');
const {BearingDiagnosticCache} = require('./bearing-diagnostic-cache');

const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'bearing-cache-'));
const samplePath = path.join(directory, 'samples.jsonl');
fs.writeFileSync(samplePath, '{}\n');
const config = {capture: {fs: 2400000, fc: 563000000}, process: {
  ambiguity: {delayMin: -10, delayMax: 500, dopplerMin: -600, dopplerMax: 600},
  detection: {minDelay: 5}, bearing: {manifold_recorder: {path: samplePath}}}};
let child;
const fakeFork = () => {
  child = new EventEmitter();
  child.pid = process.pid;
  child.send = () => setImmediate(() => child.emit('message', {ok: true,
    value: {available: true, samples: 123}}));
  child.disconnect = () => {};
  child.kill = () => {};
  return child;
};

(async () => {
  const dependencies = {fork: fakeFork, setPriority: () => {}, initialDelayMs: 1e9};
  const cache = new BearingDiagnosticCache(config, dependencies);
  assert.equal(cache.status().available, false);
  assert.equal(await cache.refresh(), true);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(cache.status().samples, 123);
  assert.equal(cache.status().cache.state, 'cached');
  assert.equal(await cache.refresh(), false, 'unchanged evidence must not launch another worker');
  cache.close();
  const restored = new BearingDiagnosticCache(config, dependencies);
  assert.equal(restored.status().samples, 123);
  restored.close();
  fs.rmSync(directory, {recursive: true, force: true});
  console.log('bearing diagnostic cache tests passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
