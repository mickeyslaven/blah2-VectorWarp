'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');
const net = require('net');
const {spawn} = require('child_process');
const yaml = require('js-yaml');
const {readConfig, saveConfig, writable, migrateKnownDefaults,
  preserveConfigBytesDuringUpgrade, configurationForReceiver, readConfigMigrations} = require('./config-store');
const {createReceiverJournal} = require('./receiver-journal');
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'blah2-recovery-'));
const file = path.join(dir, 'config.yml');

function migrationRegressions() {
  for (const [name, replay, address, preserved] of [
    ['dormant-replay-default', false, 'type=b200', true],
    ['active-replay-default', true, 'type=b200', false],
    ['changed-selector', false, 'localhost', false]
  ]) {
    const filename = path.join(dir, `${name}.yml`), config = configurationForReceiver('Usrp');
    config.capture.replay.state = replay;
    config.capture.device.address = address;
    delete config.capture.replay.provenance;
    fs.writeFileSync(filename, yaml.dump(config));
    const before = readConfig(filename), journal = createReceiverJournal(filename);
    journal.write({state: 'already-matched', applicationVerified: true,
      reconciliationRequired: false, configRevision: before.revision,
      receipt: {configRevision: before.revision}});
    assert.equal(migrateKnownDefaults(filename, {report() {}}), true);
    const after = readConfig(filename);
    assert.equal(after.validation.valid, true, after.validation.errors.join('; '));
    assert.equal(after.configMigrations[0].receiverJournalPreserved, preserved, name);
    assert.equal(journal.recover(after.revision).applicationVerified, preserved, name);
    assert.deepEqual(after.config.capture.device, {...config.capture.device, address: 'type=b200'});
    assert.equal(after.config.capture.replay.provenance, 'legacy-unqualified');
  }
  // Exact released files, not current presets with a few fields removed. Tags
  // are recorded in fixture names; tests also run outside a Git checkout.
  const fixtures = path.join(__dirname, '..', 'test', 'fixtures', 'config-upgrades');
  for (const name of fs.readdirSync(fixtures)) {
    const filename = path.join(dir, name);
    const before = yaml.load(fs.readFileSync(path.join(fixtures, name), 'utf8'));
    before.location.rx.name = 'never-publish-this-secret';
    before.truth.adsb.adsb2dd = 'never-publish-this-secret';
    fs.writeFileSync(filename, yaml.dump(before));
    const original = fs.readFileSync(filename, 'utf8');
    const preMigration = readConfig(filename);
    if (before.process.clutter.followAmbiguity === undefined)
      assert.equal(preMigration.config.process.clutter.followAmbiguity, false);
    if (before.capture.device.type === 'Kraken')
      assert.equal(preMigration.config.capture.device.heimdall.gain_control, 'fixed',
        'a legacy fixed-gain receiver must not become a measured-gain probe');
    const reports = [];
    assert.equal(migrateKnownDefaults(filename, {report: receipt => reports.push(receipt)}), true);
    const after = readConfig(filename), written = yaml.load(fs.readFileSync(filename, 'utf8'));
    assert.deepEqual(after.suppliedDefaults, [], `${name}: new optional policy must not reopen setup`);
    assert.equal(after.setupRequired, before.capture.device.type === 'HackRF',
      `${name}: only released HackRF placeholder serials need operator repair`);
    const expectedDevice = structuredClone(before.capture.device);
    if (expectedDevice.type === 'Usrp' && expectedDevice.address === 'localhost') expectedDevice.address = 'type=b200';
    assert.deepEqual(written.capture.device, expectedDevice, 'explicit receiver controls are retained');
    assert.deepEqual(written.location, before.location, 'site geometry is not a preset');
    assert.equal(written.capture.fs, before.capture.fs);
    assert.equal(written.capture.fc, before.capture.fc);
    assert.equal(written.process.detection.pfa, before.process.detection.pfa);
    assert.equal(written.process.tracker.initiate.M, before.process.tracker.initiate.M);
    assert.equal(written.truth.adsb.adsb2dd, undefined, 'truth alias is removed by the canonical sanitizer');
    assert.equal(fs.readFileSync(`${filename}.bak`, 'utf8'), original);
    assert.equal(after.configMigrations.length, 1);
    assert.equal(after.configMigrations[0].id, reports[0].id);
    assert.equal(after.configMigrations[0].configRevision, after.revision);
    assert.equal(JSON.stringify(after.configMigrations).includes('never-publish-this-secret'), false);
    assert.equal(fs.statSync(`${filename}.migrations.json`).mode & 0o777, 0o640);
    // Every changed leaf is covered, including leaves inside removed blocks.
    const changedLeaves = (left, right, prefix = '') => {
      if (left && right && typeof left === 'object' && typeof right === 'object')
        return [...new Set([...Object.keys(left), ...Object.keys(right)])].flatMap(key =>
          changedLeaves(left[key], right[key], prefix ? `${prefix}.${key}` : key));
      return JSON.stringify(left) === JSON.stringify(right) ? [] : [prefix];
    };
    for (const changed of changedLeaves(before, written))
      assert.ok(after.configMigrations[0].changes.some(change =>
        changed === change.path || changed.startsWith(`${change.path}.`)), `${name}: unreceipted change ${changed}`);
    assert.equal(migrateKnownDefaults(filename), false);
    const saved = saveConfig(filename, after.config, after.revision);
    assert.deepEqual(saved.configMigrations, after.configMigrations, 'ordinary saves retain the same one-time notice');
    assert.equal(migrateKnownDefaults(filename), false);
  }

  for (const profile of ['balanced', 'conservative', 'permissive']) {
    const filename = path.join(dir, `profile-${profile}.yml`), config = configurationForReceiver('Usrp');
    config.process.tracker.profile = profile;
    delete config.process.tracker.initiate.min_snr_db;
    delete config.process.tracker.initiate.acceleration_model;
    fs.writeFileSync(filename, yaml.dump(config));
    const loaded = readConfig(filename);
    assert.equal(loaded.config.process.tracker.initiate.min_snr_db,
      profile === 'conservative' ? 7 : profile === 'permissive' ? 4 : 5);
    assert.equal(loaded.config.process.tracker.initiate.acceleration_model,
      profile === 'balanced' ? 'adaptive_gate' : 'hypothesis_bank');
    assert.equal(migrateKnownDefaults(filename, {report() {}}), true);
    assert.deepEqual(readConfig(filename).config.process.tracker, loaded.config.process.tracker);
  }

  const filename = path.join(dir, 'transaction.yml'), sidecar = `${filename}.migrations.json`;
  const config = configurationForReceiver('Usrp');
  config.capture.device.address = 'localhost';
  fs.writeFileSync(filename, yaml.dump(config));
  const raw = fs.readFileSync(filename, 'utf8');
  fs.chmodSync(filename, 0o444);
  assert.throws(() => migrateKnownDefaults(filename, {report() {}}), /read-only/);
  assert.equal(fs.existsSync(sidecar), false);
  assert.equal(fs.readFileSync(filename, 'utf8'), raw);
  fs.chmodSync(filename, 0o640);
  const rename = fs.renameSync;
  try {
    fs.renameSync = (source, target) => {
      if (target === filename) throw new Error('simulated config rename failure');
      return rename(source, target);
    };
    assert.throws(() => migrateKnownDefaults(filename, {report() {}}), /simulated/);
  } finally { fs.renameSync = rename; }
  assert.equal(fs.readFileSync(filename, 'utf8'), raw);
  assert.deepEqual(readConfigMigrations(filename).configMigrations, [], 'unapplied pending receipts stay hidden');
  const stagedId = JSON.parse(fs.readFileSync(sidecar)).pending.id;
  let sidecarRenames = 0;
  try {
    fs.renameSync = (source, target) => {
      if (target === sidecar && ++sidecarRenames === 2) throw new Error('simulated receipt finalization failure');
      return rename(source, target);
    };
    assert.throws(() => migrateKnownDefaults(filename, {report() {}}), /simulated/);
  } finally { fs.renameSync = rename; }
  const recovered = readConfig(filename);
  assert.equal(recovered.configMigrations[0].id, stagedId, 'retry IDs depend on the transition, not wall-clock time');
  assert.equal(recovered.config.capture.device.address, 'type=b200');
  saveConfig(filename, recovered.config, recovered.revision);
  assert.equal(JSON.parse(fs.readFileSync(sidecar)).pending, null);
  assert.equal(readConfig(filename).configMigrations[0].id, stagedId);
  const olderHistory = JSON.parse(fs.readFileSync(sidecar));
  olderHistory.receipts[0].changes[0].reason = 'A previous release used this explanation.';
  olderHistory.receipts[0].private_note = 'never-publish-this-secret';
  fs.writeFileSync(sidecar, JSON.stringify(olderHistory));
  assert.equal(readConfigMigrations(filename).configMigrationError, null,
    'wording changes in a later release do not invalidate durable history');
  assert.equal(JSON.stringify(readConfigMigrations(filename)).includes('never-publish-this-secret'), false);

  for (const invalid of ['not json', '{}', 'x'.repeat(131073)]) {
    fs.writeFileSync(sidecar, invalid);
    const loaded = readConfig(filename);
    assert.equal(loaded.setupRequired, false, 'receipt recovery is independent of receiver setup');
    assert.deepEqual(loaded.configMigrations, []);
    assert.match(loaded.configMigrationError, /unreadable/);
    const candidate = {...loaded.config, process: {...loaded.config.process, data: {...loaded.config.process.data, overlap: 0}}};
    fs.writeFileSync(filename, yaml.dump(candidate));
    const bytes = fs.readFileSync(filename, 'utf8');
    assert.throws(() => migrateKnownDefaults(filename, {report() {}}), /unreadable/);
    assert.equal(fs.readFileSync(filename, 'utf8'), bytes, 'missing durable evidence must block config mutation');
  }
  fs.unlinkSync(sidecar);
  const secret = path.join(dir, 'secret.json');
  fs.writeFileSync(secret, 'never-publish-this-secret');
  fs.symlinkSync(secret, sidecar);
  assert.match(readConfig(filename).configMigrationError, /unreadable/);
  assert.throws(() => migrateKnownDefaults(filename, {report() {}}), /unreadable/);
  assert.equal(fs.readFileSync(secret, 'utf8'), 'never-publish-this-secret');
  fs.unlinkSync(sidecar);
  for (let index = 0; index < 19; index++) {
    const candidate = configurationForReceiver('Usrp');
    candidate.process.data.overlap = 0;
    candidate.location.rx.name = `Migration test ${index}`;
    fs.writeFileSync(filename, yaml.dump(candidate));
    assert.equal(migrateKnownDefaults(filename, {report() {}}), true);
  }
  const bounded = readConfigMigrations(filename);
  assert.equal(bounded.configMigrations.length, 16);
  assert.equal(bounded.configMigrationOmitted, 3);
  assert.equal(fs.statSync(sidecar).size < 131072, true);
}

function request(port, url, timeout = 500) {
  return new Promise((resolve, reject) => {
    const request = http.get(`http://127.0.0.1:${port}${url}`, {timeout}, response => {
      let data = '';
      response.on('data', chunk => { data += chunk; });
      response.on('end', () => resolve({status: response.statusCode, body: JSON.parse(data)}));
    });
    request.on('timeout', () => request.destroy(new Error(`request timed out after ${timeout}ms`)));
    request.on('error', reject);
  });
}

function availablePort(usedPorts) {
  const probe = net.createServer();
  return new Promise((resolve, reject) => {
    probe.once('error', reject);
    probe.listen(0, '127.0.0.1', () => {
      const {port} = probe.address();
      probe.close(error => {
        if (error) return reject(error);
        if (usedPorts.has(port)) return resolve(availablePort(usedPorts));
        usedPorts.add(port);
        resolve(port);
      });
    });
  });
}

(async () => {
  let child;
  const conflict = net.createServer();
  try {
    migrationRegressions();
    for (const name of ['config.yml', 'HackRF', 'Usrp', 'Kraken', 'RspDuo']) {
      const source = name === 'config.yml' ? path.join(__dirname, '..', 'config', name) : file;
      if (name !== 'config.yml') fs.writeFileSync(file, yaml.dump(configurationForReceiver(name)));
      const shipped = readConfig(source);
      assert.deepEqual(shipped.suppliedDefaults, [],
        `${name} omits required defaults: ${shipped.suppliedDefaults.join(', ')}`);
      if (name === 'config.yml') assert.equal(shipped.setupRequired, true,
        'the canonical first-run config must require an explicit receiver choice');
      else if (name !== 'HackRF') assert.equal(shipped.setupRequired, false,
        `${name} should remain a complete compatibility fixture`);
    }
    const legacyUsrp = configurationForReceiver('Usrp');
    legacyUsrp.capture.device.address = 'localhost';
    fs.writeFileSync(file, yaml.dump(legacyUsrp), {mode: 0o640});
    fs.chmodSync(file, 0o640);
    assert.equal(readConfig(file).validation.valid, false,
      'the public validator must continue rejecting a bare UHD hostname');
    assert.equal(migrateKnownDefaults(file), true);
    assert.equal(yaml.load(fs.readFileSync(file, 'utf8')).capture.device.address,
      'type=b200');
    assert.equal(yaml.load(fs.readFileSync(`${file}.bak`, 'utf8')).capture.device.address,
      'localhost');
    assert.equal(fs.statSync(file).mode & 0o777, 0o640);
    assert.equal(readConfig(file).validation.valid, true);
    assert.equal(migrateKnownDefaults(file), false,
      'the migration must be one-shot and leave current selectors untouched');
    for (const gainControl of ['measured', 'measured_per_channel']) {
      for (const selection of ['recorder', 'bearing', 'detection']) {
        const configured = configurationForReceiver('Kraken');
        Object.assign(configured.capture.device.heimdall, {gain_control: gainControl,
          gain: 14.4, channel_gains: [3.7, 3.7, 14.4, 3.7, 3.7]});
        if (selection === 'recorder') configured.process.bearing.manifold_recorder.enabled = 'on';
        if (selection === 'bearing') configured.process.bearing.manifold_file = '/tmp/measured-bearing.json';
        if (selection === 'detection') configured.process.surveillance_fusion.manifold_file = '/tmp/measured-detection.json';
        const raw = yaml.dump(configured);
        fs.writeFileSync(file, raw, {mode: 0o640});
        assert.equal(migrateKnownDefaults(file), false,
          `${gainControl}/${selection}: startup must preserve explicit hardware intent`);
        const loaded = readConfig(file);
        assert.equal(fs.readFileSync(file, 'utf8'), raw,
          'migration and read must preserve the complete configuration bytes');
        assert.deepEqual(loaded.config.capture.device.heimdall, configured.capture.device.heimdall);
        assert.equal(loaded.config.process.bearing.manifold_file, configured.process.bearing.manifold_file);
        assert.equal(loaded.config.process.surveillance_fusion.manifold_file,
          configured.process.surveillance_fusion.manifold_file);
        if (selection !== 'recorder') {
          assert.equal(loaded.validation.valid, false,
            'invalid measured-manifold gain identity requires explicit operator repair');
          assert.ok(loaded.validation.errors.some(error => /manifold.*fixed/.test(error)),
            'refusal must identify the gain identity, not an unrelated setting');
        }
      }
    }
    const automaticKraken = configurationForReceiver('Kraken');
    automaticKraken.capture.device.heimdall.gain_control = 'measured_per_channel';
    automaticKraken.capture.device.heimdall.channel_gains =
      [3.7, 3.7, 14.4, 3.7, 3.7];
    automaticKraken.process.bearing.manifold_recorder.enabled = 'off';
    fs.writeFileSync(file, yaml.dump(automaticKraken));
    assert.equal(migrateKnownDefaults(file), false,
      'automatic gain remains automatic when no measured manifold is active');
    automaticKraken.process.bearing.manifold_recorder.enabled = 'on';
    automaticKraken.capture.device.heimdall.channel_gains = [3.7, 3.7];
    fs.writeFileSync(file, yaml.dump(automaticKraken));
    assert.equal(migrateKnownDefaults(file), false,
      'an incomplete requested gain vector fails closed instead of being frozen');
    assert.equal(preserveConfigBytesDuringUpgrade(
      {VECTORWARP_PRESERVE_CONFIG_BYTES_UNTIL_UPTIME: '1120'}, 1000), true);
    for (const value of ['999', '1301', 'not-a-time', '1120.5'])
      assert.equal(preserveConfigBytesDuringUpgrade(
        {VECTORWARP_PRESERVE_CONFIG_BYTES_UNTIL_UPTIME: value}, 1000), false,
      `invalid or stale package-upgrade expiry must not suppress migration: ${value}`);
    legacyUsrp.capture.device.address = 'addr=192.168.10.2';
    legacyUsrp.truth.remote_id = {enabled: true, source: '127.0.0.1:8754',
      poll_interval: .5, source_timeout: 1.2, smoothing_window: 4,
      max_position_age: 10, wifi_enabled: false};
    fs.writeFileSync(file, yaml.dump(legacyUsrp));
    assert.equal(migrateKnownDefaults(file), false,
      'an operator-selected network USRP address must never be rewritten');
    delete legacyUsrp.truth.remote_id;
    const withoutRemoteId = yaml.dump(legacyUsrp);
    fs.writeFileSync(file, withoutRemoteId);
    const optionalRemoteId = readConfig(file);
    assert.equal(optionalRemoteId.config.truth.remote_id.enabled, false);
    assert.equal(optionalRemoteId.suppliedDefaults.some(item => item.startsWith('truth.remote_id')), false,
      'disabled Remote ID defaults must remain optional for older configurations');
    assert.equal(optionalRemoteId.setupRequired, false);
    assert.equal(migrateKnownDefaults(file), false,
      'optional Remote ID defaults must not rewrite an operator configuration');
    assert.equal(fs.readFileSync(file, 'utf8'), withoutRemoteId);
    fs.unlinkSync(file);
    const missing = readConfig(file);
    assert.equal(missing.setupRequired, true);
    assert.equal(writable(file), true);
    assert.equal(fs.existsSync(file), false, 'Viewing first-run defaults must not create a file');
    for (const raw of ['capture: [broken', 'capture: null\nnetwork: null', 'x: &cycle {x: *cycle}', '42']) {
      fs.writeFileSync(file, raw);
      const recovery = readConfig(file);
      assert.equal(recovery.setupRequired, true);
      assert.ok(recovery.config.capture.device.type);
      assert.equal(fs.readFileSync(file, 'utf8'), raw, 'Recovery must not silently rewrite YAML');
    }
    fs.writeFileSync(file, 'capture: [broken', {mode: 0o640});
    fs.chmodSync(file, 0o640);
    const broken = readConfig(file);
    const repaired = saveConfig(file,
      configurationForReceiver('RspDuo', broken.config), broken.revision);
    assert.equal(fs.readFileSync(`${file}.bak`, 'utf8'), 'capture: [broken');
    assert.equal(fs.statSync(file).mode & 0o777, 0o640);
    assert.equal(repaired.validation.valid, true);
    fs.chmodSync(file, 0o444);
    assert.equal(writable(file), false, 'Root must respect explicitly read-only settings');
    fs.chmodSync(file, 0o640);
    fs.chmodSync(dir, 0o555);
    assert.equal(writable(file), false, 'Atomic saves require an explicitly writable directory');
    fs.chmodSync(dir, 0o700);
    assert.throws(() => saveConfig(file, broken.config, broken.revision), /changed/);
    const optionalKraken = JSON.parse(JSON.stringify(repaired.config));
    optionalKraken.capture.fs = 2400000;
    optionalKraken.capture.device = {type: 'Kraken', channel_count: 8, reference_channel: 2};
    delete optionalKraken.process.performance;
    fs.writeFileSync(file, yaml.dump(optionalKraken));
    const inferred = readConfig(file).config;
    assert.equal(inferred.process.reference_synthesis.mode, 'dedicated');
    assert.deepEqual(inferred.capture.device.surveillance_channels, [0,1,3,4,5,6,7]);
    assert.deepEqual(inferred.process.reference_synthesis.channels, [2]);
    assert.equal(inferred.process.performance.surveillance_workers, 1);
    assert.equal(inferred.process.performance.fft_threads, 1);
    const legacyRsp = JSON.parse(JSON.stringify(repaired.config));
    delete legacyRsp.process.performance;
    fs.writeFileSync(file, yaml.dump(legacyRsp));
    const legacyDocument = readConfig(file);
    assert.deepEqual(legacyDocument.config.process.performance, {surveillance_workers: 1, fft_threads: 1, acceleration: 'auto'});
    assert.equal(legacyDocument.setupRequired, false, 'Valid old configs must not require setup just for optional defaults');

    // Kraken gain is a declared string/number union: a "keep" profile default
    // must not replace a persisted Suite gain while recovering a document.
    const kraken = configurationForReceiver('Kraken');
    // Suite AGC (-1) is intentionally invalid with measured-per-channel gain:
    // the latter needs stable fixed starting gains for reproducible probes.
    for (const gain of [0, 28.7, 49.6, 50]) {
      kraken.capture.device.heimdall.gain = gain;
      fs.writeFileSync(file, yaml.dump(kraken));
      const persisted = readConfig(file);
      assert.equal(persisted.config.capture.device.heimdall.gain, gain);
      assert.equal(persisted.setupRequired, false, `valid Kraken gain ${gain} must not require setup`);
      const saved = saveConfig(file, persisted.config, persisted.revision);
      assert.equal(saved.config.capture.device.heimdall.gain, gain);
      assert.equal(yaml.load(fs.readFileSync(file, 'utf8')).capture.device.heimdall.gain, gain);
    }
    kraken.capture.device.heimdall.gain = 50.1;
    fs.writeFileSync(file, yaml.dump(kraken));
    const invalidGain = readConfig(file);
    assert.equal(invalidGain.config.capture.device.heimdall.gain, 50.1,
      'Recovery must expose invalid values for repair instead of replacing them');
    assert.equal(invalidGain.validation.valid, false, 'invalid gain remains rejected by normal validation');

    await new Promise(resolve => conflict.listen(0, '0.0.0.0', resolve));
    const usedPorts = new Set([conflict.address().port]);
    const config = repaired.config;
    const apiPort = await availablePort(usedPorts);
    config.network.ports.api = apiPort;
    config.network.ports.map = conflict.address().port;
    for (const key of ['detection','track','timestamp','timing','iqdata','config'])
      config.network.ports[key] = await availablePort(usedPorts);
    delete config.process.delay_reference_sample_rate_hz;
    fs.writeFileSync(file, `${yaml.dump(config)}# operator-owned package-upgrade bytes\n`);
    fs.rmSync(`${file}.bak`, {force: true});
    const packageUpgradeBytes = fs.readFileSync(file);
    let childExit = null;
    let childOutput = '';
    const appendChildOutput = (label, chunk) => {
      childOutput = `${childOutput}${label}${chunk}`.slice(-4096);
    };
    child = spawn(process.execPath, [path.join(__dirname, 'server.js'), file],
      {stdio: ['ignore', 'pipe', 'pipe'],
        env: {...process.env, VECTORWARP_PRESERVE_CONFIG_BYTES_UNTIL_UPTIME:
          String(Math.floor(os.uptime()) + 120)}});
    child.stdout.on('data', chunk => appendChildOutput('stdout: ', chunk));
    child.stderr.on('data', chunk => appendChildOutput('stderr: ', chunk));
    child.once('error', error => { childExit = `spawn error: ${error.message}`; });
    child.once('exit', (code, signal) => { childExit = `exit ${code}${signal ? ` (${signal})` : ''}`; });
    let response;
    const deadline = Date.now() + 10000;
    while (Date.now() < deadline && childExit === null) {
      try { response = await request(apiPort, '/api/system/status'); break; }
      catch (_) { await new Promise(resolve => setTimeout(resolve, 30)); }
    }
    assert.equal(response?.status, 200, `API stays available when a radar data port cannot bind (${childExit || 'startup timed out'}; ${childOutput || 'no child output'})`);
    assert.deepEqual(fs.readFileSync(file), packageUpgradeBytes,
      'the package-upgrade API start must preserve operator configuration byte-for-byte');
    assert.equal(fs.existsSync(`${file}.bak`), false,
      'a byte-preserving package-upgrade start must not create a migration backup');
    assert.ok(response.body.errors.some(error => error.includes('map data port')));
    assert.equal((await request(apiPort, '/api/config/capabilities')).body.editable, true);
    const migrations = (await request(apiPort, '/api/config/migrations')).body;
    assert.equal(migrations.schema, 'vectorwarp-config-migrations-v1');
    assert.deepEqual(response.body.configMigrations, migrations.configMigrations);
    for (const route of ['/api/system/status?summary=1', '/api/config/capabilities'])
      assert.deepEqual((await request(apiPort, route)).body.configMigrations, migrations.configMigrations);
    assert.equal(Object.hasOwn((await request(apiPort, '/api/config')).body, 'configMigrations'), false,
      'migration metadata never becomes an editable/native YAML field');
    const pinnedRequirement = JSON.stringify({schema: 'vectorwarp-radar-plan-requirements-v1',
      semantic_sha256: '0'.repeat(64), required_features: {tracking: true}});
    fs.writeFileSync(`${file}.radar-plan.json`, pinnedRequirement);
    const reviewCapabilities = (await request(apiPort, '/api/config/capabilities')).body;
    assert.equal(reviewCapabilities.setupRequired, false, 'a pin-only rejection does not reopen first-run setup');
    assert.equal(reviewCapabilities.radarPlanReview.required, true);
    for (const route of ['/api/system/status?summary=1', '/api/config/migrations'])
      assert.deepEqual((await request(apiPort, route)).body.radarPlanReview, reviewCapabilities.radarPlanReview);
    assert.equal(fs.readFileSync(`${file}.radar-plan.json`, 'utf8'), pinnedRequirement,
      'viewing review instructions cannot install a different pin');
    fs.unlinkSync(`${file}.radar-plan.json`);
    fs.writeFileSync(file, 'not: [valid yaml');
    assert.equal((await request(apiPort, '/api/config/capabilities')).body.setupRequired, true);
    console.log('First-run, malformed-config recovery, atomic backup, permissions, stale revision and occupied-port tests passed.');
  } finally {
    if (child) child.kill('SIGTERM');
    conflict.close();
    fs.rmSync(dir, {recursive: true, force: true});
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
