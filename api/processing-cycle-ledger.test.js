'use strict';

// Real-filesystem regression for observation-only cycle retention and sealing.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const crypto = require('node:crypto');
const {ProcessingCycleLedger} = require('./processing-cycle-ledger');
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-cycle-ledger-'));
let serial = 0;
const capture = (accepted = '9007199254740993') => ({available: true, epoch: '7',
  discontinuities: '0', requalifications: '0', retiredCpis: '0', retiredSamples: '0',
  lanes: [{acceptedSamples: accepted, droppedSamples: '0', discardedSamples: '0'}]});
const record = (sequence = 1) => ({sequence: String(sequence),
  startMonoNs: String(1000000 + sequence * 1000000),
  endMonoNs: String(1500000 + sequence * 1000000), drainBatches: '0', outcome: 'completed',
  captureBefore: capture(String(9007199254740992n + BigInt(sequence))),
  captureAfter: capture(String(9007199254740993n + BigInt(sequence)))});
const frame = (...records) => ({cycleEvidence: {schema: 'vectorwarp-processing-cycles-v1',
  anchorMonoNs: '1000000', anchorUtcNs: '1795500000000000001', fault: '', records}});
const create = options => new ProcessingCycleLedger({directory: path.join(root, String(++serial)),
  minimumFreeBytes: 1, ...options});
async function rejected(change, reason, options) {
  const ledger = create(options);
  const value = frame(record());
  change(value, ledger);
  ledger.accept(value);
  const status = await ledger.close();
  assert.equal(status.state, 'rejected');
  assert.match(status.fault, reason);
  if (fs.existsSync(path.join(ledger.directory, 'status.json')))
    assert.notEqual(JSON.parse(fs.readFileSync(path.join(ledger.directory, 'status.json'))).state, 'complete');
}
(async () => {
  try {
    const ledger = create();
    assert.equal(ledger.accept(frame(record())), true);
    assert.equal(ledger.accept(frame(record(), record(2))), true);
    assert.equal((await ledger.close()).state, 'complete');
    const raw = fs.readFileSync(path.join(ledger.directory, 'cycles.jsonl'));
    const seal = JSON.parse(fs.readFileSync(path.join(ledger.directory, 'status.json')));
    assert.equal(raw.toString().trim().split('\n').length, 2);
    assert.equal(seal.sha256, crypto.createHash('sha256').update(raw).digest('hex'));
    assert.equal(JSON.parse(raw.toString().split('\n')[0]).captureBefore.lanes[0].acceptedSamples,
      '9007199254740993');
    assert.equal(ledger.accept(frame(record(3))), false);
    const restart = new ProcessingCycleLedger({directory: ledger.directory, minimumFreeBytes: 1});
    await restart.close();
    assert.equal(restart.status().state, 'rejected');
    assert.equal(fs.readFileSync(path.join(ledger.directory, 'cycles.jsonl')).equals(raw), true);
    await rejected(value => { value.cycleEvidence.records = [record(2)]; }, /Missing processing cycle/);
    await rejected((value, live) => { live.accept(value); value.cycleEvidence.anchorMonoNs = '2'; }, /session reset/);
    await rejected((value, live) => { live.accept(value); value.cycleEvidence.records[0].outcome = 'warmup'; }, /duplicate/);
    await rejected(value => { value.cycleEvidence.records[0].captureAfter.epoch = '8'; }, /epoch/);
    await rejected(value => { value.cycleEvidence.records[0].captureAfter.lanes[0].acceptedSamples = '1'; }, /counter reset/);
    await rejected(value => { value.cycleEvidence.records[0].captureAfter.lanes[0].acceptedSamples = 9007199254740993; }, /Exact uint64/);
    await rejected(value => { value.cycleEvidence.records[0].endMonoNs = '1'; }, /interval/);
    await rejected(value => { value.cycleEvidence.fault = 'utc-clock-drift'; }, /Native cycle evidence fault/);
    await rejected(value => { value.cycleEvidence.records = [record(), record(2)]; }, /queue overflow/, {maxPendingRecords: 1});
    await rejected(() => {}, /capacity/, {maxBytes: 8});
    await rejected(value => { value.cycleEvidence.records[0].captureAfter.lanes = [
      {acceptedSamples: '18446744073709551615', droppedSamples: '0', discardedSamples: '0'},
      {acceptedSamples: '1', droppedSamples: '0', discardedSamples: '0'}];
      value.cycleEvidence.records[0].captureBefore.lanes = structuredClone(value.cycleEvidence.records[0].captureAfter.lanes);
    }, /overflow/);
    await rejected(() => {}, /disk floor/, {io: {...fs.promises, statfs: async () => ({bavail: 0n, bsize: 1n})}});
    await rejected(() => {}, /injected write/, {io: {...fs.promises, open: async (...args) => {
      const file = await fs.promises.open(...args);
      if (args[0].endsWith('cycles.jsonl')) file.writeFile = async () => { throw new Error('injected write'); };
      return file;
    }}});
    await rejected(() => {}, /injected fsync/, {io: {...fs.promises, open: async (...args) => {
      const file = await fs.promises.open(...args);
      if (args[0].endsWith('cycles.jsonl')) file.sync = async () => { throw new Error('injected fsync'); };
      return file;
    }}});
    const unsealed = create();
    unsealed.accept(frame(record()));
    await unsealed.pending;
    assert.equal(JSON.parse(fs.readFileSync(path.join(unsealed.directory, 'status.json'))).state, 'checkpoint');
    await unsealed.close();
    let releaseWrite, enteredWrite;
    const held = new Promise(resolve => { releaseWrite = resolve; });
    const entered = new Promise(resolve => { enteredWrite = resolve; });
    let writeCalls = 0;
    const checkpointed = create({io: {...fs.promises, open: async (...args) => {
      const file = await fs.promises.open(...args);
      if (args[0].endsWith('cycles.jsonl')) {
        const write = file.writeFile.bind(file);
        file.writeFile = async (...values) => {
          if (++writeCalls === 2) { enteredWrite(); await held; }
          return write(...values);
        };
      }
      return file;
    }}});
    checkpointed.accept(frame(record()));
    await checkpointed.pending;
    const checkpointPath = path.join(checkpointed.directory, 'status.json');
    assert.equal(JSON.parse(fs.readFileSync(checkpointPath)).state, 'checkpoint');
    checkpointed.accept(frame(record(2)));
    await entered;
    assert.equal(JSON.parse(fs.readFileSync(checkpointPath)).state, 'recording',
      'The old checkpoint must be durably revoked before appending');
    releaseWrite();
    await checkpointed.pending;
    assert.equal(JSON.parse(fs.readFileSync(checkpointPath)).throughSequence, '2');
    checkpointed.accept(frame(record(4)));
    await checkpointed.invalidating;
    assert.equal(JSON.parse(fs.readFileSync(checkpointPath)).state, 'rejected');
    await checkpointed.close();
    let releaseRename, enteredRename;
    const renameHeld = new Promise(resolve => { releaseRename = resolve; });
    const renameEntered = new Promise(resolve => { enteredRename = resolve; });
    let inspectedStaleRename = false;
    const raced = create({io: {...fs.promises, rename: async (source, destination) => {
      const staged = JSON.parse(await fs.promises.readFile(source));
      if (staged.state === 'checkpoint') {
        enteredRename();
        await renameHeld;
        await fs.promises.rename(source, destination);
        // Inspect before writeStatus resumes, not after eventual invalidation.
        const visible = JSON.parse(fs.readFileSync(path.join(path.dirname(source), 'status.json')));
        assert.notEqual(visible.state, 'checkpoint', 'A paused stale rename must not publish a clean checkpoint');
        assert.notEqual(visible.state, 'complete');
        inspectedStaleRename = true;
      } else await fs.promises.rename(source, destination);
    }}});
    raced.accept(frame(record()));
    await renameEntered;
    assert.equal(raced.accept(frame(record(3))), false);
    assert.match(raced.fault, /Missing processing cycle/);
    releaseRename();
    await raced.pending;
    await raced.invalidating;
    assert.equal(inspectedStaleRename, true);
    assert.equal(JSON.parse(fs.readFileSync(path.join(raced.directory, 'status.json'))).state, 'rejected');
    assert.equal(fs.existsSync(path.join(raced.directory, 'status.ready')), false);
    assert.equal((await raced.close()).state, 'rejected');
    console.log('Processing-cycle ledger: exact uint64, deduplication, sequence/reset/clock/overflow/disk/write/fsync rejection passed.');
  } finally { fs.rmSync(root, {recursive: true, force: true}); }
})().catch(error => { console.error(error); process.exitCode = 1; });
