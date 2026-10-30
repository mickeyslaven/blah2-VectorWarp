'use strict';

const assert = require('assert/strict');
const {isLocalDeviceRequest, reveal} = require('./recording-share');

const interfaces = {lo: [{address: '127.0.0.1'}, {address: '::1'}],
  tailscale0: [{address: '100.126.186.65'}]};
assert.equal(isLocalDeviceRequest({socket: {remoteAddress: '::ffff:127.0.0.1'}}, interfaces), true);
assert.equal(isLocalDeviceRequest({socket: {remoteAddress: '100.126.186.65'}}, interfaces), true,
  'a browser using this machine\'s own network address is local');
assert.equal(isLocalDeviceRequest({socket: {remoteAddress: '100.90.37.77'}}, interfaces), false,
  'another tailnet device receives a download instead of opening the server file manager');

const calls = [];
const execFile = (file, args, options, done) => { calls.push({file, args, options}); done(null); };
(async () => {
  assert.deepEqual(await reveal('/recordings/clip.mchq', {platform: 'darwin', execFile}),
    {selected: true, method: 'finder'});
  assert.equal(calls[0].file, '/usr/bin/open');
  assert.deepEqual(calls[0].args, ['-R', '/recordings/clip.mchq']);
  calls.length = 0;
  assert.deepEqual(await reveal('C:\\recordings\\clip.mchq', {platform: 'win32', execFile}),
    {selected: true, method: 'explorer'});
  assert.equal(calls[0].file, 'explorer.exe');
  console.log('Recording share locality and native file selection passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
