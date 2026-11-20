'use strict';

const assert = require('assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {EventEmitter} = require('events');
const {spawnSync} = require('child_process');
const {LIMITS, readBoundedFile, readBoundedJsonl, writeBoundedCandidate, runBoundedWorker} =
  require('./manifold-fit-limits');

(async () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'manifold-offline-limits-'));
  try {
    const samplePath = path.join(directory, 'samples.jsonl');
    fs.writeFileSync(samplePath, '{}\n');
    assert.equal(readBoundedJsonl(samplePath).rows.length, 1);
    fs.writeFileSync(samplePath, '{}\n'.repeat(LIMITS.input_rows + 1));
    assert.throws(() => readBoundedJsonl(samplePath), /20000 rows/);
    const fd = fs.openSync(samplePath, 'w');
    fs.ftruncateSync(fd, LIMITS.input_bytes + 1); fs.closeSync(fd);
    assert.throws(() => readBoundedJsonl(samplePath), /64 MiB/);
    assert.throws(() => readBoundedFile(samplePath, LIMITS.config_bytes, 'config'), /regular file/);
    const configPath = path.join(directory, 'config.json');
    fs.writeFileSync(configPath, '{}');
    const candidatePath = path.join(directory, 'candidate.json');
    const oversized = spawnSync(process.execPath, [path.join(__dirname, 'fit-detection-manifold.js'),
      '--enable-offline-experiment', '--input', samplePath, '--config', configPath,
      '--output', candidatePath], {encoding: 'utf8', timeout: 35000});
    assert.notEqual(oversized.status, 0);
    assert.match(oversized.stderr, /64 MiB/);
    assert.equal(fs.existsSync(candidatePath), false);
    fs.writeFileSync(samplePath, 'not-json\n');
    assert.throws(() => readBoundedJsonl(samplePath), /Invalid manifold input line 1/);
    await writeBoundedCandidate(candidatePath, {candidate: true});
    const saved = fs.readFileSync(candidatePath, 'utf8');
    await assert.rejects(() => writeBoundedCandidate(candidatePath, {candidate: false}), /EEXIST/);
    assert.equal(fs.readFileSync(candidatePath, 'utf8'), saved);
    await assert.rejects(() => writeBoundedCandidate(path.join(directory, 'large.json'),
      {oversize: 'x'.repeat(LIMITS.output_bytes)}), /2 MiB/);
    fs.writeFileSync(`${candidatePath}.experiment.tmp`, 'preserve-stale');
    await assert.rejects(() => writeBoundedCandidate(candidatePath, {}), /EEXIST/);
    assert.equal(fs.readFileSync(`${candidatePath}.experiment.tmp`, 'utf8'), 'preserve-stale');
    let killed = 0, workerOptions;
    class Worker extends EventEmitter {
      constructor(filename, options) {
        super(); workerOptions = options;
        if (filename === 'exit') setImmediate(() => this.emit('exit', 0));
        if (filename === 'error') setImmediate(() => this.emit('error', new Error('fit failed')));
        if (filename === 'malformed') setImmediate(() => this.emit('message', {}));
        if (filename === 'success') setImmediate(() => this.emit('message', {ok: true, result: {fitted: true}}));
      }
      terminate() { ++killed; return Promise.resolve(0); }
    }
    await assert.rejects(() => runBoundedWorker(Worker, 'timeout', {}, 5), /deadline/);
    await assert.rejects(() => runBoundedWorker(Worker, 'exit', {}), /without a result/);
    await assert.rejects(() => runBoundedWorker(Worker, 'error', {}), /fit failed/);
    await assert.rejects(() => runBoundedWorker(Worker, 'malformed', {}), /Malformed/);
    assert.deepEqual(await runBoundedWorker(Worker, 'success', {}), {fitted: true});
    assert.equal(killed, 5);
    assert.equal(workerOptions.resourceLimits.maxOldGenerationSizeMb, 256);
    assert.equal(workerOptions.resourceLimits.maxYoungGenerationSizeMb, 16);
    console.log('Offline manifold limits: input/output caps, no overwrite, worker deadline and failures passed.');
  } finally { fs.rmSync(directory, {recursive: true}); }
})().catch(error => { console.error(error); process.exitCode = 1; });
