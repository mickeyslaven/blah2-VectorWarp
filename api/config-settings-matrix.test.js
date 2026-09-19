'use strict';
const assert = require('assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const yaml = require('js-yaml');
const {FIELD_RULES, validateConfig, getDeviceProfiles} = require('./config-manager');
const {readConfig, saveConfig} = require('./config-store');
const clone = value => JSON.parse(JSON.stringify(value));
const root = path.join(__dirname, '..');
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-settings-matrix-'));
function leaves(value, prefix = []) { return Array.isArray(value) || value === null || typeof value !== 'object' ? [{path: prefix.join('.')}]
  : Object.entries(value).flatMap(([key, child]) => leaves(child, [...prefix, key])); }
function shipped(name) { const value = yaml.load(fs.readFileSync(path.join(root, 'config', name), 'utf8'));
  if (value.capture.device.type === 'HackRF') value.capture.device.serial = ['0000000000000001', '0000000000000002']; return value; }
function get(value, key) { return key.split('.').reduce((item, name) => item?.[name], value); }
function put(value, key, replacement) { const names = key.split('.'); const parent = names.slice(0, -1).reduce((item, name) => item[name], value); parent[names.at(-1)] = replacement; }
function invalidFor(rule) { return rule.type === 'boolean' ? 'false' : rule.type === 'number' || rule.type === 'gain' ? 'invalid' : rule.type === 'array' ? 'invalid' : 7; }
function validDifferent(key, current, rule) {
  const explicit = {
    'capture.fs': 2000001, 'capture.fc': 204640001, 'capture.device.heimdall.host': '127.0.0.2',
    'capture.device.heimdall.port': 8093, 'capture.device.heimdall.control_port': 8094,
    'capture.device.heimdall.gain': -1, 'capture.device.address': 'type=b200,serial=0000000000000001',
    'capture.device.subdev': 'A:B A:A', 'capture.device.serial': ['0000000000000003','0000000000000004'],
    'capture.device.antenna': ['TX/RX','TX/RX'], 'capture.device.gain': [21,21],
    'capture.device.gain_lna': [16,16], 'capture.device.gain_vga': [20,20], 'capture.device.gainReduction': [51,51],
    'capture.device.amp_enable': [true,true], 'capture.replay.file': '/tmp/changed.blah2iq',
    'capture.replay.legacy_block_samples': 4096, 'network.ip': '127.0.0.1',
    'truth.adsb.tar1090': 'auto', 'save.path': '/tmp/vectorwarp/', 'location.rx.name': 'Changed receiver', 'location.tx.name': 'Changed transmitter',
    'process.reference_synthesis.channels': undefined
  };
  if (explicit[key] !== undefined) return explicit[key];
  if (rule.choices) return rule.choices.find(value => JSON.stringify(value) !== JSON.stringify(current));
  if (rule.type === 'boolean') return !current;
  if (rule.type === 'number') return typeof current === 'number' ? current + (Number.isInteger(current) ? 1 : .01) : 1;
  if (rule.type === 'string') return `${current}-changed`;
  return undefined;
}
try {
  const covered = new Set();
  for (const name of ['config.yml', 'config-hackrf.yml', 'config-usrp.yml', 'config-kraken.yml']) {
    const source = shipped(name); const validation = validateConfig(source);
    assert.equal(validation.valid, true, `${name}: ${validation.errors.join('; ')}`);
    const filename = path.join(directory, name); fs.writeFileSync(filename, yaml.dump(source));
    const saved = saveConfig(filename, source, readConfig(filename).revision);
    assert.deepEqual(readConfig(filename).config, saved.config, `${name} failed save/load round trip`);
    for (const leaf of leaves(saved.config)) if (FIELD_RULES[leaf.path]) covered.add(leaf.path);
  }
  const base = shipped('config-kraken.yml');
  for (const profile of getDeviceProfiles()) {
    const candidate = clone(base); candidate.capture.fs = profile.sampleRate; candidate.capture.device = clone(profile.device);
    if (profile.type === 'HackRF') candidate.capture.device.serial = ['0000000000000001', '0000000000000002'];
    for (const [key, value] of Object.entries(profile.process || {})) candidate.process[key] = clone(value);
    const result = validateConfig(candidate, base); assert.equal(result.valid, true, `${profile.type}: ${result.errors.join('; ')}`);
    for (const leaf of leaves(candidate)) if (FIELD_RULES[leaf.path]) covered.add(leaf.path);
  }
  const missing = Object.keys(FIELD_RULES).filter(key => !covered.has(key) && !FIELD_RULES[key].optional && !FIELD_RULES[key].readOnly);
  assert.deepEqual(missing, [], `schema fields missing from shipped/profile matrix: ${missing.join(', ')}`);
  // Exercise scalar validation against shipped documents. HTTP rejection and
  // unchanged-file/revision guarantees are covered by config-server.test.js;
  // config-store intentionally expects its caller to validate before writing.
  let rejected = 0;
  let changed = 0;
  const edited = new Set();
  const sources = ['config.yml', 'config-hackrf.yml', 'config-usrp.yml', 'config-kraken.yml'].map(shipped);
  for (const [key, rule] of Object.entries(FIELD_RULES)) {
    if (rule.readOnly) continue;
    const source = sources.find(value => get(value, key) !== undefined);
    if (!source) continue; // Absent legacy default; config-recovery supplies it.
    const replacement = validDifferent(key, get(source, key), rule);
    if (replacement !== undefined) {
      const filename = path.join(directory, `changed-${changed}.yml`); fs.writeFileSync(filename, yaml.dump(source));
      const baseline = readConfig(filename); const candidate = clone(baseline.config); put(candidate, key, replacement);
      const checked = validateConfig(candidate, source);
      if (checked.valid) {
        assert.notDeepEqual(get(source, key), replacement, `${key} mutation must differ from baseline`);
        const saved = saveConfig(filename, candidate, baseline.revision);
        assert.deepEqual(readConfig(filename).config, candidate, `${key} save/load changed unrelated settings`);
        assert.deepEqual(get(readConfig(filename).config, key), replacement, `${key} valid edit did not persist`);
        assert.deepEqual(get(saved.config, key), replacement); changed++; edited.add(key);
      }
    }
    const candidate = clone(source); put(candidate, key, invalidFor(rule));
    assert.equal(validateConfig(candidate, source).valid, false, `${key} invalid value was accepted`);
    rejected++;
  }
  // Optional USRP-block replay setting is a real persisted edit with a
  // cross-field dependency, rather than merely a schema inventory entry.
  const legacyBase = shipped('config-usrp.yml'); const legacyFile = path.join(directory, 'legacy-block.yml'); fs.writeFileSync(legacyFile, yaml.dump(legacyBase));
  const legacy = clone(readConfig(legacyFile).config); legacy.capture.replay = {...legacy.capture.replay, state: true, format: 'usrp-blocks', legacy_block_samples: 4096};
  assert.equal(validateConfig(legacy).valid, true);
  const invalidLegacy = clone(legacy); invalidLegacy.capture.replay.legacy_block_samples = 0;
  assert.equal(validateConfig(invalidLegacy).valid, false);
  rejected++;
  { const filename = legacyFile;
    saveConfig(filename, legacy, readConfig(filename).revision);
    assert.deepEqual(readConfig(filename).config, legacy, 'legacy block setting did not persist'); }
  edited.add('capture.replay.legacy_block_samples');
  // Cross-field settings need coherent groups, rather than scalar mutation.
  const persistGroup = (name, baseline, candidate, keys) => {
    const result = validateConfig(candidate); assert.equal(result.valid, true, `${name}: ${result.errors.join('; ')}`);
    const filename = path.join(directory, `${name}.yml`); fs.writeFileSync(filename, yaml.dump(baseline));
    const saved = saveConfig(filename, candidate, readConfig(filename).revision);
    assert.deepEqual(readConfig(filename).config, candidate, `${name} save/load changed unrelated settings`);
    for (const key of keys) { assert.notDeepEqual(get(baseline, key), get(candidate, key), `${name} did not change ${key}`); assert.deepEqual(get(saved.config, key), get(candidate, key), `${name} did not persist ${key}`); edited.add(key); }
  };
  const krakenBase = shipped('config-kraken.yml'); const kraken = clone(krakenBase);
  kraken.capture.device.channel_count = 6; kraken.capture.device.reference_channel = 1;
  kraken.capture.device.surveillance_channels = [0,2,3,4,5];
  kraken.process.reference_synthesis = {...kraken.process.reference_synthesis, mode: 'dedicated', channels: [1]};
  kraken.process.performance = {...kraken.process.performance, acceleration: 'cpu', surveillance_workers: 2, fft_threads: 2};
  Object.assign(kraken.network.ports, {api: 3100, map: 3101, detection: 3102, track: 3103, timestamp: 3104, timing: 3105, iqdata: 3106});
  persistGroup('kraken-coherent-edit', krakenBase, kraken, ['capture.device.channel_count','capture.device.reference_channel','capture.device.surveillance_channels','process.reference_synthesis.mode','process.reference_synthesis.channels','process.performance.acceleration','process.performance.surveillance_workers','process.performance.fft_threads','network.ports.api','network.ports.map','network.ports.detection','network.ports.track','network.ports.timestamp','network.ports.timing','network.ports.iqdata']);
  const rspBase = shipped('config-kraken.yml'); const rsp = clone(rspBase); const rspProfile = getDeviceProfiles().find(profile => profile.type === 'RspDuo');
  rsp.capture.fs = rspProfile.sampleRate; rsp.capture.device = clone(rspProfile.device); rsp.capture.device.serial = '0000000000000001';
  delete rsp.process.reference_synthesis; rsp.process.performance = {...clone(rspProfile.process.performance), acceleration: 'auto'};
  persistGroup('rsp-profile-edit', rspBase, rsp, ['capture.fs','capture.device.type','capture.device.serial']);
  const remaining = Object.keys(FIELD_RULES).filter(key => !FIELD_RULES[key].readOnly && !edited.has(key));
  assert.deepEqual(remaining, [], `editable fields lack a persisted valid non-default case: ${remaining.join(', ')}`);
  console.log(`Configuration inventory covered ${covered.size}/${Object.keys(FIELD_RULES).length}; validated ${rejected} invalid values; persisted non-default edits for all ${edited.size} editable fields (${changed} scalar plus grouped cases).`);
} finally { fs.rmSync(directory, {recursive: true, force: true}); }
