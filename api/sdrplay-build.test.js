'use strict';
const assert = require('assert/strict');
const express = require('express');
const http = require('http');
const EventEmitter = require('events');
const {installSdrplayBuildRoutes, INTENT, progress, startBuild} = require('./sdrplay-build');
const origin = 'http://127.0.0.1:3000';
const kit = 'a'.repeat(64), cohort = 'b'.repeat(64);

function child(code = 0) { const value = new EventEmitter(); value.kill = () => {}; process.nextTick(() => value.emit('close', code)); return value; }
async function route(options, run) {
  const app = express(); app.use(express.json({strict: false})); installSdrplayBuildRoutes(app, {allowedOrigins: new Set([origin]), ...options});
  const server = app.listen(0, '127.0.0.1'); await new Promise(resolve => server.once('listening', resolve));
  const call = (method, headers = {}, body = '') => new Promise((resolve, reject) => {
    const request = http.request({port: server.address().port, path: '/api/sdrplay-build', method,
      headers: {Host: '127.0.0.1:3000', ...headers}}, response => { let text = ''; response.on('data', part => text += part); response.on('end', () => resolve({status: response.statusCode, body: JSON.parse(text)})); });
    request.once('error', reject); request.end(body);
  });
  try { await run(call); } finally { await new Promise(resolve => server.close(resolve)); }
}

(async () => {
  let started = 0, observed = 0;
  const base = {enabled: true, status: async () => { observed++; return {ok: true, state: 'missing', kit_id: kit, cohort}; }, start: async () => { started++; }};
  await route(base, async call => {
    assert.equal((await call('GET', {Origin: origin})).status, 200);
    assert.equal(started, 0, 'GET is observation only.');
    assert.equal((await call('POST', {Origin: 'http://evil.invalid', 'Content-Type': 'application/json', 'X-VectorWarp-Intent': INTENT}, '{}')).status, 403);
    assert.equal((await call('POST', {Origin: origin, 'Content-Type': 'application/json'}, '{}')).status, 403);
    assert.equal((await call('POST', {Origin: origin, 'Content-Type': 'application/json', 'X-VectorWarp-Intent': INTENT}, 'true')).status, 422);
    assert.equal((await call('POST', {Origin: origin, 'Content-Type': 'application/json', 'X-VectorWarp-Intent': INTENT}, '[]')).status, 422);
    assert.equal(started, 0, 'Rejected requests must not request a system action.');
    assert.equal((await call('POST', {Origin: origin, 'Content-Type': 'application/json', 'X-VectorWarp-Intent': INTENT}, '{}')).status, 202);
    assert.equal(started, 1); assert.ok(observed >= 1);
  });
  await route({enabled: true, status: async () => ({ok: true, state: 'current', kit_id: kit, cohort}), start: async () => assert.fail('must not start')}, async call => {
    assert.equal((await call('POST', {Origin: origin, 'Content-Type': 'application/json', 'X-VectorWarp-Intent': INTENT}, '{}')).status, 409);
  });
  await route({enabled: true, status: async () => ({ok: false, state: 'unavailable', reason: 'status unavailable'}), start: async () => assert.fail('must not start')}, async call => {
    assert.equal((await call('POST', {Origin: origin, 'Content-Type': 'application/json', 'X-VectorWarp-Intent': INTENT}, '{}')).status, 409);
  });
  await route({enabled: false, status: async () => assert.fail('disabled GET must not execute helper'), start: async () => assert.fail('must not start')}, async call => {
    const result = await call('GET', {Origin: origin}); assert.equal(result.status, 200); assert.equal(result.body.buildable, false);
  });
  await route({enabled:true, preview:true, status:async () => assert.fail('preview must not probe helper'), start:async () => assert.fail('preview must not start build')}, async call => {
    const result=await call('GET',{Origin:origin});
    assert.equal(result.status,200); assert.equal(result.body.buildable,false);
    assert.equal(result.body.progress,null);
    assert.equal((await call('POST',{Origin:origin,'Content-Type':'application/json','X-VectorWarp-Intent':INTENT},'{}')).status,409);
  });
  await route({enabled: true, status: async () => ({ok: true, state: 'missing', kit_id: kit, cohort}), start: async () => { throw new Error('sudo rejected'); }}, async call => {
    const result = await call('POST', {Origin: origin, 'Content-Type': 'application/json', 'X-VectorWarp-Intent': INTENT}, '{}');
    assert.equal(result.status, 503); assert.match(result.body.errors[0], /did not accept/);
  });
  await assert.rejects(startBuild(() => child(1)), /rejected/);
  let concurrentStarts = 0;
  await route({enabled:true, status:async () => {
    await new Promise(resolve => setTimeout(resolve, 20));
    return {ok:true, state:'missing', kit_id:kit, cohort};
  }, start:async () => { concurrentStarts++; await new Promise(resolve => setTimeout(resolve, 30)); }}, async call => {
    const headers={Origin:origin,'Content-Type':'application/json','X-VectorWarp-Intent':INTENT};
    const results=await Promise.all([call('POST',headers,'{}'),call('POST',headers,'{}')]);
    assert.deepEqual(results.map(value=>value.status).sort(),[202,409]);
    assert.equal(concurrentStarts,1);
  });
  const commands = [];
  await startBuild((file, args, options) => { commands.push({file, args, options}); return child(0); });
  assert.deepEqual(commands[0].args, ['-n', '/usr/bin/systemctl', 'start', '--no-block', 'vectorwarp-sdrplay-build.service']);
  assert.equal(commands[0].file, '/usr/bin/sudo'); assert.equal(commands[0].options.cwd, '/');
  const rootDirectory = {isDirectory: () => true, uid: 0, mode: 0o755};
  const regular = {isFile: () => false, uid: 0, mode: 0o644, size: 0};
  let openFlags;
  const fakeIo = {lstatSync: () => rootDirectory, openSync: (_file, flags) => { openFlags = flags; return 3; }, fstatSync: () => regular,
    readSync: (_fd, buffer) => { const text = JSON.stringify({schema: 1, state: 'running', updatedAt: 10, kit_id: kit}); buffer.write(text); return Buffer.byteLength(text); }, closeSync: () => {}};
  assert.equal(progress('/run/vectorwarp-sdrplay-build/status.json', {io: fakeIo, now: 20}), null, 'A non-regular progress object (including a FIFO) is ignored.');
  assert.ok(openFlags & require('fs').constants.O_NOFOLLOW);
  assert.ok(openFlags & require('fs').constants.O_NONBLOCK, 'A FIFO cannot block status reads.');
  regular.isFile = () => true;
  assert.equal(progress('/run/vectorwarp-sdrplay-build/status.json', {io:fakeIo,now:200011}).state,'failed');
  console.log('SDRplay local build route tests passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
