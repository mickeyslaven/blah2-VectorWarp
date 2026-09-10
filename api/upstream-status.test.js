'use strict';

const assert = require('assert');
const net = require('net');
const {compareKrakenStatus, getUpstreamStatus} =
  require('./upstream-status.js');

const config = {capture: {fs: 2400000, fc: 204640000, device: {
  type: 'Kraken', heimdall: {host: '127.0.0.1', port: 8091},
  channel_count: 5
}}};
const matching = {settings: {center_freq: 204640000, sample_rate: 2400000,
  gain: 40.2}, num_channels: 5, max_elements: 8,
operating_mode: 'coherent', reconfiguring: false, recovering: false,
cooldown_active: false};
assert.deepEqual(compareKrakenStatus(config, matching).issues, []);
assert.equal(compareKrakenStatus(config, {settings: {}}).matched, false);
assert.equal(compareKrakenStatus(config, {...matching, settings: {center_freq: null}}).actual.centerFrequency, null);
const mismatched = JSON.parse(JSON.stringify(matching));
mismatched.settings.center_freq = 100000000;
mismatched.num_channels = 4;
mismatched.operating_mode = 'wideband';
assert.deepEqual(compareKrakenStatus(config, mismatched).issues
  .map(issue => issue.field), ['capture.fc',
  'capture.device.channel_count', 'capture.device.heimdall']);

const server = net.createServer(socket => {
  socket.write(`${JSON.stringify(matching)}\n`);
  socket.on('data', () => { throw new Error('Observer must not send control commands'); });
});
server.listen(0, '127.0.0.1', async () => {
  try {
    const address = server.address();
    const testConfig = JSON.parse(JSON.stringify(config));
    testConfig.capture.device.heimdall.control_port = address.port;
    const result = await getUpstreamStatus(testConfig, {timeoutMs: 500});
    assert.equal(result.available, true);
    assert.equal(result.controlPort, address.port);
    assert.deepEqual(result.issues, []);
    const direct = await getUpstreamStatus({capture: {device: {type: 'Usrp'}}});
    assert.equal(direct.supported, false);
    assert.equal(direct.matched, false);
    const unavailable = await getUpstreamStatus({...config, capture: {...config.capture,
      device: {...config.capture.device, heimdall: {host: '127.0.0.1', port: 8091, control_port: 1}}}}, {timeoutMs: 100});
    assert.equal(unavailable.available, false);
    assert.ok(unavailable.message.includes('Check that Suite V2 is running'));
    for (const kind of ['silent', 'oversize', 'partial']) {
      const fixture = net.createServer(socket => {
        socket.on('error', () => {});
        if (kind === 'oversize') socket.write('x'.repeat(65537));
        if (kind === 'partial') socket.write('{"settings":{}}\n');
      });
      await new Promise(resolve => fixture.listen(0, '127.0.0.1', resolve));
      try {
        const bounded = JSON.parse(JSON.stringify(config));
        bounded.capture.device.heimdall.control_port = fixture.address().port;
        const started = Date.now();
        const observed = await getUpstreamStatus(bounded, {timeoutMs: 100});
        assert.ok(Date.now() - started < 2000, `${kind} observation must be bounded`);
        if (kind === 'partial') assert.equal(observed.matched, false);
        else assert.equal(observed.available, false);
      } finally { fixture.close(); }
    }
    console.log('Upstream receiver status tests passed.');
  } finally {
    server.close();
  }
}).on('error', error => {
  console.error(error);
  process.exitCode = 1;
});
