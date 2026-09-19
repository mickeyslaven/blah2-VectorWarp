'use strict';
// Real loopback browser request proving the Darwin default is the external,
// whole-stack launcher command rather than an API-internal processor spawn.
if (process.platform !== 'darwin') {
  console.log('macOS browser lifecycle dispatch test skipped outside Darwin.');
  process.exit(0);
}
const assert = require('assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const net = require('net');
const {spawn} = require('child_process');
const yaml = require('js-yaml');
const {setupDefaults} = require('./config-store');
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-macos-api-'));
const filename = path.join(directory, 'config.yml');
const launcher = path.join(directory, 'vectorwarp-macos');
const marker = path.join(directory, 'restart-argument');
let child;
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
async function ports(count) {
  const listeners = await Promise.all(Array.from({length: count}, () => new Promise((resolve, reject) => {
    const listener = net.createServer(); listener.once('error', reject);
    listener.listen(0, '127.0.0.1', () => resolve(listener));
  })));
  const values = listeners.map(listener => listener.address().port);
  await Promise.all(listeners.map(listener => new Promise(resolve => listener.close(resolve))));
  return values;
}
(async () => {
  try {
    const config = setupDefaults(); config.capture.replay.state = true; config.network.ip = '127.0.0.1';
    const values = await ports(Object.keys(config.network.ports).length);
    Object.keys(config.network.ports).forEach((name, index) => { config.network.ports[name] = values[index]; });
    const origin = `http://127.0.0.1:${config.network.ports.api}`;
    fs.writeFileSync(filename, yaml.dump(config));
    fs.writeFileSync(launcher, `#!/bin/sh\nprintf '%s' "$1" > ${JSON.stringify(marker)}\n`);
    fs.chmodSync(launcher, 0o755);
    child = spawn(process.execPath, [path.join(__dirname, 'server.js'), filename], {stdio: 'pipe', env: {
      ...process.env, BLAH2_CONFIG_RESTART_COMMAND: '', VECTORWARP_MACOS_LIFECYCLE: '1',
      VECTORWARP_MACOS_LAUNCHER: launcher, BLAH2_RECEIVER_TYPES: 'HackRF,Usrp,Kraken'}});
    let capabilities;
    for (let attempt = 0; attempt < 120; attempt += 1) {
      try { const response = await fetch(`${origin}/api/config/capabilities`); if (response.ok) { capabilities = await response.json(); break; } }
      catch (_) {} await delay(50);
    }
    assert.equal(capabilities?.restartAvailable, true, 'Darwin launcher was not exposed to the browser');
    const response = await fetch(`${origin}/api/config?restart=true`, {method: 'PUT', headers: {
      'Content-Type': 'application/json', Origin: origin, 'If-Match': `"${capabilities.configRevision}"`,
      'X-VectorWarp-Intent': 'config-write-v1', 'X-VectorWarp-Receiver-Sync': 'synchronize-v1'}, body: JSON.stringify(config)});
    assert.equal(response.status, 200, JSON.stringify(await response.clone().json()));
    for (let attempt = 0; attempt < 120 && !fs.existsSync(marker); attempt += 1) await delay(25);
    assert.equal(fs.readFileSync(marker, 'utf8'), 'restart');
    console.log('macOS browser Save & Restart dispatches the whole-stack launcher.');
  } finally {
    if (child) { child.kill('SIGTERM'); if (child.exitCode === null) await new Promise(resolve => child.once('exit', resolve)); }
    fs.rmSync(directory, {recursive: true, force: true});
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
