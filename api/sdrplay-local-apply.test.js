'use strict';
// Isolated loopback configuration path. The helper is a root-controlled fixture;
// the restart command writes only a temporary marker.
const assert = require('assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const net = require('net');
const {spawn} = require('child_process');
const yaml = require('js-yaml');
const {setupDefaults} = require('./config-store');

const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-local-rsp-'));
const filename = path.join(directory, 'config.yml');
const stateFile = path.join(directory, 'helper-state');
const helper = path.join(directory, 'local-helper.py');
const marker = path.join(directory, 'restart-marker');
const config = setupDefaults();
config.network.ip = '127.0.0.1';
let child, origin, revision, errors = '';
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const clone = value => JSON.parse(JSON.stringify(value));

async function ports(count) {
  const listeners = await Promise.all(Array.from({length: count}, () => new Promise((resolve, reject) => {
    const listener = net.createServer(); listener.once('error', reject);
    listener.listen(0, '127.0.0.1', () => resolve(listener));
  })));
  const values = listeners.map(listener => listener.address().port);
  await Promise.all(listeners.map(listener => new Promise(resolve => listener.close(resolve))));
  return values;
}
async function request(method, route, body, intent = 'synchronize-v1') {
  const response = await fetch(`${origin}${route}`, {method, headers: {
    Origin: origin, 'Content-Type': 'application/json', 'If-Match': `"${revision}"`,
    'X-VectorWarp-Receiver-Sync': intent
  }, ...(body === undefined ? {} : {body: JSON.stringify(body)})});
  return {status: response.status, body: await response.json()};
}
async function capabilities() {
  const response = await fetch(`${origin}/api/config/capabilities`);
  assert.equal(response.status, 200); return response.json();
}
async function waitForServer() {
  for (let attempt = 0; attempt < 120; attempt += 1) {
    try { const value = await capabilities(); if (value.configRevision) return value; } catch (_) {}
    await delay(25);
  }
  throw new Error(`API did not become ready: ${errors}`);
}

(async () => {
  try {
    const reserved = await ports(Object.keys(config.network.ports).length);
    Object.keys(config.network.ports).forEach((name, index) => { config.network.ports[name] = reserved[index]; });
    origin = `http://127.0.0.1:${config.network.ports.api}`;
    fs.writeFileSync(filename, yaml.dump(config));
    fs.writeFileSync(stateFile, 'missing');
    fs.writeFileSync(helper, `#!/usr/bin/python3\nimport json,pathlib,sys\nstate=pathlib.Path(${JSON.stringify(stateFile)}).read_text().strip()\nif state == 'error': sys.exit(1)\nvalue={'missing':{'ok':False,'state':'missing'},'stale-sdk':{'ok':False,'state':'stale','reason':'SDK changed'},'stale-core':{'ok':False,'state':'stale','reason':'Core changed'},'current':{'ok':True,'state':'current','kit_id':'a'*64,'cohort':'b'*64}}[state]\nprint(json.dumps(value))\n`);
    fs.chmodSync(helper, 0o755);
    child = spawn(process.execPath, [path.join(__dirname, 'server.js'), filename], {env: {...process.env,
      BLAH2_RECEIVER_TYPES: 'Usrp,HackRF,Kraken', BLAH2_SDRPLAY_LOCAL_BUILD: 'true',
      BLAH2_LOCAL_BUILD_RECEIVER_TYPES: 'RspDuo', BLAH2_SDRPLAY_BUILD_HELPER: helper,
      BLAH2_CONFIG_RESTART_COMMAND: JSON.stringify([process.execPath, '-e', `require('fs').writeFileSync(${JSON.stringify(marker)}, 'requested')`])
    }, stdio: ['ignore', 'ignore', 'pipe']});
    child.stderr.on('data', value => { errors += value; });
    const ready = await waitForServer(); revision = ready.configRevision;
    assert.equal(ready.deviceProfiles.find(item => item.type === 'RspDuo').liveAvailable, false);
    assert.deepEqual(ready.compiledLiveTypes, ['Usrp', 'HackRF', 'Kraken']);
    assert.deepEqual(ready.localBuildLiveTypes, []);

    for (const state of ['missing', 'stale-sdk', 'stale-core', 'error']) {
      fs.writeFileSync(stateFile, state);
      const before = fs.readFileSync(filename);
      const candidate = clone(config); candidate.capture.fc += 1;
      const rejected = await request('PUT', '/api/config?restart=true', candidate);
      assert.equal(rejected.status, 422, `${state}: ${JSON.stringify(rejected.body)}`);
      assert.equal(rejected.body.code, 'SDRPLAY_LOCAL_BUILD_REQUIRED');
      assert.deepEqual(fs.readFileSync(filename), before, `${state} must not save configuration`);
      assert.equal(fs.existsSync(marker), false, `${state} must not request restart`);
    }

    // Pending saves and replay remain usable without a locally compiled adapter.
    fs.writeFileSync(stateFile, 'missing');
    const pending = clone(config); pending.capture.fc += 2;
    let saved = await request('PUT', '/api/config?mode=pending&restart=false', pending, 'save-pending-v1');
    assert.equal(saved.status, 200, JSON.stringify(saved.body)); revision = saved.body.revision;
    const replay = clone(pending); replay.capture.replay.state = true;
    saved = await request('PUT', '/api/config?restart=false', replay);
    assert.equal(saved.status, 200, JSON.stringify(saved.body)); revision = saved.body.revision;

    fs.writeFileSync(stateFile, 'current');
    const currentCapabilities = await capabilities();
    assert.equal(currentCapabilities.deviceProfiles.find(item => item.type === 'RspDuo').liveAvailable, true);
    assert.deepEqual(currentCapabilities.compiledLiveTypes, ['Usrp', 'HackRF', 'Kraken']);
    assert.deepEqual(currentCapabilities.localBuildLiveTypes, ['RspDuo']);
    const live = clone(replay); live.capture.replay.state = false; live.capture.fc += 3;
    saved = await request('PUT', '/api/config?restart=true', live);
    assert.equal(saved.status, 200, JSON.stringify(saved.body)); revision = saved.body.revision;
    for (let attempt = 0; attempt < 100 && !fs.existsSync(marker); attempt += 1) await delay(25);
    assert.equal(fs.readFileSync(marker, 'utf8'), 'requested');
    child.kill('SIGTERM');
    if (child.exitCode === null) await new Promise(resolve => child.once('exit',resolve));
    const probed = path.join(directory,'preview-helper-called');
    fs.writeFileSync(helper, `import pathlib\npathlib.Path(${JSON.stringify(probed)}).write_text('called')\n`);
    child = spawn(process.execPath,[path.join(__dirname,'server.js'),filename],{env:{...process.env,
      BLAH2_RECEIVER_TYPES:'Usrp,HackRF,Kraken',BLAH2_SDRPLAY_LOCAL_BUILD:'true',
      BLAH2_LOCAL_BUILD_RECEIVER_TYPES:'RspDuo',BLAH2_SDRPLAY_BUILD_HELPER:helper,BLAH2_PREVIEW:'true'
    },stdio:['ignore','ignore','pipe']});
    child.stderr.on('data',value=>{errors+=value;});
    const preview = await waitForServer();
    assert.equal(preview.deviceProfiles.find(item=>item.type==='RspDuo').liveAvailable,false);
    const previewBuild=await request('GET','/api/sdrplay-build');
    assert.equal(previewBuild.body.buildable,false);
    assert.equal(fs.existsSync(probed),false,'Preview never executes the local build status helper.');
    console.log('Local RSPduo status gates capability and Apply without claiming compiled support.');
  } finally {
    if (child) { child.kill('SIGTERM'); if (child.exitCode === null) await new Promise(resolve => child.once('exit', resolve)); }
    fs.rmSync(directory, {recursive: true, force: true});
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
