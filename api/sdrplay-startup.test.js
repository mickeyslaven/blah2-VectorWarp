'use strict';
const assert = require('assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const crypto = require('crypto');
const yaml = require('js-yaml');
const {prepare, readRegular, trustedTree, finishReport} = require('../script/vectorwarp-prepare-sdrplay');
const {setupDefaults} = require('./config-store');
const {validateConfig} = require('./config-manager');
const {createReceiverJournal} = require('./receiver-journal');
const {readSdrplayStartup} = require('./sdrplay-startup');

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-sdrplay-'));
const filename = path.join(dir, 'config.yml');
const journal = createReceiverJournal(filename);
let starts = 0, reports = [], beforeStart = () => {};
const options = {yaml, validateConfig, journal,
  start: () => { starts++; beforeStart(); return 'SDRplay API service started.'; },
  report: value => reports.push(value)};
const sha = raw => crypto.createHash('sha256').update(raw).digest('hex');
function reset(config = setupDefaults()) {
  starts = 0; reports = []; beforeStart = () => {};
  const raw = yaml.dump(config);
  fs.writeFileSync(filename, raw);
  journal.write({state: 'not-required', reconciliationRequired: false, configRevision: sha(raw)});
  return config;
}
function blocked(config, expected) {
  reset(config);
  assert.throws(() => prepare(filename, options), expected);
  assert.equal(starts, 0);
  assert.equal(reports.at(-1).state, 'failed');
}
try {
  reset();
  assert.equal(prepare(filename, options).state, 'ready');
  assert.equal(starts, 1);
  assert.deepEqual(reports.map(value => value.state), ['starting', 'ready']);
  for (const type of ['Kraken', 'Usrp', 'HackRF']) {
    const config = setupDefaults(); config.capture.device.type = type; reset(config);
    assert.equal(prepare(filename, options).state, 'not-required'); assert.equal(starts, 0);
  }
  const replay = setupDefaults(); replay.capture.replay.state = true; reset(replay);
  assert.equal(prepare(filename, options).state, 'not-required'); assert.equal(starts, 0);
  const absentReplay = setupDefaults(); delete absentReplay.capture.replay;
  blocked(absentReplay, /explicit RSPduo/);
  const bad = setupDefaults(); bad.capture.fc = -1;
  blocked(bad, /valid, explicit/);
  for (const state of ['saved-pending', 'unknown', 'in-progress', 'journal-unreadable']) {
    reset(); journal.write({state, reconciliationRequired: false, configRevision: sha(fs.readFileSync(filename))});
    assert.throws(() => prepare(filename, options), /pending or unresolved/); assert.equal(starts, 0);
  }
  for (const entry of [{state: 'not-required', reconciliationRequired: true},
    {state: 'not-required', reconciliationRequired: false, configRevision: 'wrong'}]) {
    reset(); journal.write(entry);
    assert.throws(() => prepare(filename, options), /pending or unresolved/); assert.equal(starts, 0);
  }
  reset(); fs.unlinkSync(journal.filename);
  assert.throws(() => prepare(filename, options), /pending or unresolved/); assert.equal(starts, 0);
  reset(); beforeStart = () => fs.appendFileSync(filename, '\n# concurrent edit');
  assert.throws(() => prepare(filename, options), /vendor service may already be running/);
  assert.equal(starts, 1); assert.equal(reports.at(-1).state, 'failed');
  reset(); beforeStart = () => journal.write({state: 'saved-pending', reconciliationRequired: false});
  assert.throws(() => prepare(filename, options), /changed during/); assert.equal(starts, 1);
  reset(); options.report = value => {
    reports.push(value);
    if (value.state === 'starting') fs.appendFileSync(filename, '\n# changed before command');
  };
  assert.throws(() => prepare(filename, options), /changed during/); assert.equal(starts, 0);
  options.report = value => reports.push(value);
  reset(); beforeStart = () => { throw new Error('SDRplay was not found. https://sdrplay.com/hardware-api/'); };
  assert.throws(() => prepare(filename, options), /SDRplay was not found/);
  assert.match(reports.at(-1).message, /https:\/\/sdrplay.com\/hardware-api\//);
  for (const raw of ['capture: [', 'foo: &x [*x]', 'null', '[]', 'capture: {device: {type: RspDuo}}']) {
    reset(); fs.writeFileSync(filename, raw);
    assert.throws(() => prepare(filename, options)); assert.equal(starts, 0);
  }
  reset(); fs.unlinkSync(filename);
  assert.throws(() => prepare(filename, options)); assert.equal(starts, 0);
  fs.symlinkSync(journal.filename, filename);
  assert.throws(() => readRegular(filename)); fs.unlinkSync(filename);
  fs.mkdirSync(filename); assert.throws(() => readRegular(filename)); fs.rmdirSync(filename);
  fs.writeFileSync(filename, Buffer.alloc(262145)); assert.throws(() => readRegular(filename));

  const receiptFile = path.join(dir, 'status.json');
  const io = {...fs, lstatSync: file => ({...fs.lstatSync(file), uid: 0, mode: 0o755, isDirectory: () => true}),
    fstatSync: fd => { const value = fs.fstatSync(fd); return {uid: 0, mode: 0o644, size: value.size, isFile: () => value.isFile()}; }};
  const receipt = {state: 'failed', inProgress: false, configRevision: 'a'.repeat(64), updatedAt: 1000, message: 'Missing SDRplay API'};
  assert.equal(finishReport(receipt, '1').message, 'Missing SDRplay API');
  assert.equal(finishReport({...receipt, inProgress: true, state: 'ready'}, '17').state, 'failed');
  assert.equal(finishReport({...receipt, inProgress: true, state: 'ready'}, '17').inProgress, false);
  assert.equal(finishReport({...receipt, inProgress: true, state: 'ready'}, '0').state, 'complete');
  fs.writeFileSync(receiptFile, JSON.stringify(receipt));
  const readOptions = {filename: receiptFile, now: 2000, io};
  assert.deepEqual(readSdrplayStartup(receipt.configRevision, readOptions), receipt);
  assert.equal(readSdrplayStartup('b'.repeat(64), readOptions), null);
  assert.equal(readSdrplayStartup(receipt.configRevision, {...readOptions, now: 200000}).state, 'failed');
  fs.writeFileSync(receiptFile, JSON.stringify({...receipt, state: 'ready', inProgress: true}));
  assert.equal(readSdrplayStartup(null, readOptions).inProgress, true, 'The operation lock is not tied to current config revision.');
  assert.equal(readSdrplayStartup(receipt.configRevision, readOptions).inProgress, true, 'SDRplay ready does not mean processor start completed.');
  const expired = readSdrplayStartup(null, {...readOptions, now: 200000});
  assert.equal(expired.state, 'failed'); assert.equal(expired.inProgress, false);
  fs.writeFileSync(receiptFile, JSON.stringify(receipt));
  assert.equal(readSdrplayStartup(receipt.configRevision, {...readOptions, now: 0}), null);
  for (const patch of [{state: 'bogus'}, {message: {}}, {updatedAt: '1000'}, {message: 'x'.repeat(1801)}]) {
    fs.writeFileSync(receiptFile, JSON.stringify({...receipt, ...patch}));
    assert.equal(readSdrplayStartup(receipt.configRevision, readOptions), null);
  }
  fs.writeFileSync(receiptFile, JSON.stringify(receipt));
  assert.equal(readSdrplayStartup(receipt.configRevision, {...readOptions,
    io: {...io, lstatSync: () => ({isDirectory: () => true, uid: 1000, mode: 0o755})}}), null);
  fs.unlinkSync(receiptFile); fs.symlinkSync(filename, receiptFile);
  assert.equal(readSdrplayStartup(receipt.configRevision, readOptions), null);
  const code = path.join(dir, 'code'); fs.mkdirSync(code);
  const nested = path.join(code, 'nested'); fs.mkdirSync(nested);
  const safe = file => { if (fs.statSync(file).mode & 0o022) throw new Error('Writable code rejected'); };
  for (const name of ['module.js', 'package.json']) {
    const item = path.join(nested, name); fs.writeFileSync(item, '{}', {mode: 0o644});
    trustedTree(code, safe);
    fs.chmodSync(item, 0o666); assert.throws(() => trustedTree(code, safe), /Writable code/);
    fs.chmodSync(item, 0o644);
  }
  const target = path.join(dir, 'writable.js'); fs.writeFileSync(target, '{}'); fs.chmodSync(target, 0o666);
  fs.symlinkSync(target, path.join(nested, 'linked.js'));
  assert.throws(() => trustedTree(code, safe), /Writable code/);
  fs.chmodSync(target, 0o644);
  assert.throws(() => trustedTree(code, safe), /outside its reviewed code tree/);
  console.log('SDRplay saved-config startup and persistent error tests passed.');
} finally { fs.rmSync(dir, {recursive: true, force: true}); }
