'use strict';

// Native deployment acceptance: one API/static-web process on a disposable port.
// It uses only a temporary config and local fixture, never receiver hardware.
const assert = require('assert');
const fs = require('fs');
const http = require('http');
const net = require('net');
const os = require('os');
const path = require('path');
const vm = require('vm');
const {spawn} = require('child_process');
const yaml = require('js-yaml');
const {setupDefaults} = require('../../api/config-store');

function fetchHttp(url, options = {}) {
  return new Promise((resolve, reject) => {
    const request = http.request(url, {method: options.method || 'GET', headers: options.headers,
      signal: options.signal, timeout: 3000}, response => {
      let body = '';
      response.setEncoding('utf8');
      response.on('data', chunk => { body += chunk; });
      response.on('end', () => resolve({ok: response.statusCode >= 200 && response.statusCode < 300,
        status: response.statusCode, headers: {get: name => response.headers[name.toLowerCase()]},
        text: async () => body, json: async () => JSON.parse(body)}));
      response.on('error', reject);
    });
    request.on('error', reject);
    request.on('timeout', () => request.destroy(new Error('HTTP test timeout')));
    if (options.body) request.write(options.body);
    request.end();
  });
}

(async () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'blah2-web-acceptance-'));
  let unavailable = false;
  const fixture = http.createServer((req, res) => {
    res.setHeader('Content-Type', 'application/json');
    if (unavailable) { res.writeHead(503); return res.end('{}'); }
    res.end(JSON.stringify(req.url.startsWith('/api/dd') ?
      {abc123: {timestamp: Date.now() / 1000, delay: 1, doppler: 2}} :
      {now: Date.now() / 1000, aircraft: [{hex: 'abc123', lat: 1, lon: 2, seen_pos: 0}]}));
  });
  await new Promise(resolve => fixture.listen(0, '127.0.0.1', resolve));
  const config = setupDefaults();
  config.network.ip = '127.0.0.1';
  // Reserve all fixture ports together, then release immediately before spawn.
  // Never connect to or alter a real preview/API at the default service ports.
  const reservations = [];
  for (const name of Object.keys(config.network.ports)) {
    const reservation = net.createServer();
    await new Promise((resolve, reject) => {
      reservation.once('error', reject);
      reservation.listen(0, '127.0.0.1', resolve);
    });
    reservations.push(reservation);
    config.network.ports[name] = reservation.address().port;
  }
  const base = `http://127.0.0.1:${config.network.ports.api}`;
  config.truth.adsb = {enabled: true, tar1090: `127.0.0.1:${fixture.address().port}`,
    poll_interval: 0.1, smoothing_window: 2, max_position_age: 30};
  const file = path.join(directory, 'config.yml');
  fs.writeFileSync(file, yaml.dump(config));
  const env = {...process.env};
  delete env.BLAH2_PREVIEW;
  delete env.BLAH2_CONFIG_RESTART_COMMAND;
  delete env.BLAH2_SETUP_PORT;
  await Promise.all(reservations.map(server => new Promise(resolve => server.close(resolve))));
  const child = spawn(process.execPath, ['api/server.js', file], {cwd: path.resolve(__dirname, '../..'), env, stdio: 'pipe'});
  let logs = '';
  child.stderr.on('data', chunk => { logs += chunk; });
  child.stdout.resume();
  try {
    let ready = false;
    for (let i = 0; i < 50 && !ready; i++) {
      try {
        const response = await fetchHttp(base + '/api/system/status');
        const status = await response.json();
        ready = response.ok && typeof status.serverId === 'string' &&
          status.serverId.startsWith(`${child.pid}-`);
      }
      catch (_) { await new Promise(resolve => setTimeout(resolve, 40)); }
    }
    assert.ok(ready, logs || 'Isolated API did not start');
    for (const origin of [base]) {
      const html = await fetchHttp(origin + '/display/configuration/');
      assert.equal(html.status, 200, origin);
      assert.ok((await html.text()).includes('renderConfiguration'));
      const script = await fetchHttp(origin + '/js/common.js');
      assert.equal(script.status, 200);
      const calls = [];
      const labels = {'live-status': {}, 'adsb-status': {}};
      const context = vm.createContext({URL, URLSearchParams, AbortController,
        window: {location: new URL(origin), setTimeout, clearTimeout},
        document: {getElementById: id => labels[id], querySelectorAll: () => []},
        fetch: (url, options = {}) => {
          const absolute = new URL(url, origin).href;
          calls.push(absolute);
          return fetchHttp(absolute, {...options, headers: {...options.headers, Origin: origin}});
        }});
      vm.runInContext(await script.text(), context);
      const saved = await context.fetchStatusResource('/api/config');
      const candidate = await saved.json();
      candidate.location.rx.name = 'Acceptance test site';
      const written = await context.fetchStatusResource('/api/config?restart=false', {method: 'PUT',
        headers: {'Content-Type': 'application/json', 'If-Match': saved.headers.get('etag')}, body: JSON.stringify(candidate)});
      assert.equal(written.status, 200, origin + ': config writes must survive proxy origin checks');
      assert.equal((await (await context.fetchStatusResource('/api/adsb')).json()).aircraft[0].hex, 'abc123');
      assert.deepEqual(await (await context.fetchStatusResource('/api/adsb/delay-doppler')).json(), {},
        'the first raw position is valid motion warmup');
      assert.equal((await context.fetchStatusResource('/capture/status')).status, 200);
      await context.refreshLiveStatus();
      assert.ok(labels['live-status'].innerHTML.endsWith('RADAR OFFLINE'));
      assert.ok(labels['adsb-status'].innerHTML.endsWith('ADS-B ONLINE'));
      assert.ok(calls.every(url => url.startsWith(origin)), 'Native clients must stay same-origin');
    }
    unavailable = true;
    await new Promise(resolve => setTimeout(resolve, 1100));
    const failed = await fetchHttp(base + '/api/adsb');
    assert.equal(failed.status, 503, 'native API must preserve upstream errors');
    assert.ok((await failed.json()).error);

    const apiUnit = fs.readFileSync(path.resolve(__dirname,
      '../../contrib/systemd/vectorwarp-api.service.in'), 'utf8');
    const processorUnit = fs.readFileSync(path.resolve(__dirname,
      '../../contrib/systemd/vectorwarp-processor.service.in'), 'utf8');
    const restartUnit = fs.readFileSync(path.resolve(__dirname,
      '../../contrib/systemd/vectorwarp-restart.service.in'), 'utf8');
    const sudoers = fs.readFileSync(path.resolve(__dirname,
      '../../contrib/systemd/vectorwarp.sudoers.in'), 'utf8');
    const helper = fs.readFileSync(path.resolve(__dirname,
      '../../script/vectorwarp-restart'), 'utf8');
    const installer = fs.readFileSync(path.resolve(__dirname,
      '../../script/install-native.sh'), 'utf8');
    assert.match(apiUnit, /User=vectorwarp-api/);
    assert.match(apiUnit, /BLAH2_RECEIVER_TYPES=@RECEIVER_TYPES@/);
    assert.match(processorUnit, /User=vectorwarp/);
    assert.doesNotMatch(apiUnit + processorUnit, /User=root/);
    assert.match(apiUnit, /BLAH2_CONFIG_RESTART_COMMAND=.*systemctl.*--no-block.*vectorwarp-restart\.service/);
    assert.match(restartUnit, /Type=oneshot/);
    assert.match(restartUnit, /ExecStart=@PREFIX@\/libexec\/vectorwarp-restart/);
    assert.match(sudoers, /systemctl --no-block start vectorwarp-restart\.service/);
    assert.match(helper, /systemctl stop vectorwarp-processor\.service/);
    assert.match(helper, /systemctl restart vectorwarp-api\.service/);
    assert.match(helper, /vectorwarp-wait-api\.js/);
    assert.match(helper, /systemctl start vectorwarp-processor\.service/);
    assert.match(helper, /"\$#" -ne 0/);
    assert.match(installer, /PREFIX=\/opt\/vectorwarp/);
    assert.match(installer, /no VectorWarp service was enabled or started/);
    console.log('Native deployment acceptance passed: one-origin UI/API, config save, raw ADS-B, error propagation and scoped non-root units.');
  } finally {
    child.kill('SIGTERM');
    await new Promise(resolve => child.exitCode !== null ? resolve() : child.once('exit', resolve));
    await new Promise(resolve => fixture.close(resolve));
    fs.rmSync(directory, {recursive: true});
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
