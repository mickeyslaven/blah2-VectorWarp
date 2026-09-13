'use strict';
// Real loopback API/config transactions; the restart command is a marker only.
const assert = require('assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {spawn} = require('child_process');
const yaml = require('js-yaml');
const {setupDefaults} = require('./config-store');
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-sdrplay-api-'));
const filename = path.join(directory, 'config.yml');
const marker = path.join(directory, 'restart-marker');
const config = setupDefaults();
config.network.ip = '127.0.0.1';
const port = 33000 + process.pid % 10000;
Object.keys(config.network.ports).forEach((name, i) => { config.network.ports[name] = port + i; });
fs.writeFileSync(filename, yaml.dump(config));
const original = fs.readFileSync(filename);
const origin = `http://127.0.0.1:${port}`;
const child = spawn(process.execPath, [path.join(__dirname, 'server.js'), filename], {
  env: {...process.env, BLAH2_RECEIVER_TYPES: 'RspDuo,Kraken',
    BLAH2_CONFIG_RESTART_COMMAND: JSON.stringify([process.execPath, '-e',
      `require('fs').writeFileSync(${JSON.stringify(marker)},'restart requested')`])}, stdio: 'pipe'});
let errors = '';
child.stderr.on('data', value => { errors += value; }); child.stdout.resume();
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
let revision;
async function save(headers, suffix = '?restart=true') {
  return fetch(`${origin}/api/config${suffix}`, {method: 'PUT',
    headers: {'Content-Type': 'application/json', 'If-Match': `"${revision}"`, ...headers}, body: JSON.stringify(config)});
}
(async () => {
  try {
    let ready;
    for (let i = 0; i < 120; i++) {
      try { const r = await fetch(`${origin}/api/config/capabilities`); if (r.ok) { ready = await r.json(); break; } } catch (_) {}
      await delay(50);
    }
    assert.ok(ready, `API did not become ready: ${errors}`); revision = ready.configRevision;
    for (const headers of [{}, {'X-VectorWarp-Receiver-Sync': 'synchronize-v1'},
      {Origin: origin}, {Origin: origin, 'X-VectorWarp-Receiver-Sync': 'wrong'},
      {Origin: `http://127.0.0.1:${port + 1}`, 'X-VectorWarp-Receiver-Sync': 'synchronize-v1'}]) {
      const response = await save(headers);
      assert.equal(response.status, 428, JSON.stringify(await response.json()));
      assert.deepEqual(fs.readFileSync(filename), original);
      assert.equal(fs.existsSync(marker), false);
    }
    const pending = await save({Origin: origin, 'X-VectorWarp-Receiver-Sync': 'save-pending-v1'}, '?mode=pending&restart=false');
    assert.equal(pending.status, 200); const saved = await pending.json(); revision = saved.revision;
    assert.equal(saved.restarting, false); assert.equal(fs.existsSync(marker), false);
    const apply = await save({Origin: origin, 'X-VectorWarp-Receiver-Sync': 'synchronize-v1'});
    assert.equal(apply.status, 200, JSON.stringify(await apply.clone().json()));
    assert.equal((await apply.json()).restarting, true);
    for (let i = 0; i < 100 && !fs.existsSync(marker); i++) await delay(50);
    assert.equal(fs.readFileSync(marker, 'utf8'), 'restart requested');
    console.log('RSPduo Apply origin/intent, disk-only save and simulated restart API tests passed.');
  } finally {
    child.kill('SIGTERM');
    if (child.exitCode === null) await new Promise(resolve => child.once('exit', resolve));
    fs.rmSync(directory, {recursive: true, force: true});
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
