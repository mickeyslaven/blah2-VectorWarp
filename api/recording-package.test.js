'use strict';

const assert = require('assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {PassThrough} = require('stream');
const {descriptor, streamPackage, extractPackage, MAGIC} = require('./recording-package');

async function main() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-package-'));
  const source = path.join(root, 'source'); const destination = path.join(root, 'destination');
  fs.mkdirSync(source); fs.mkdirSync(destination);
  const recording = path.join(source, 'TEST123.mchq');
  fs.writeFileSync(recording, Buffer.from([0, 1, 2, 3, 4, 5]));
  fs.writeFileSync(`${recording}.json`, JSON.stringify({channels: 5, sampleRate: 2400000}));
  fs.writeFileSync(`${recording}.replay.json`, JSON.stringify({schema: 'vectorwarp-replay-profile-v2'}));
  fs.writeFileSync(`${recording}.timeline.json`, JSON.stringify({schema: 'vectorwarp-recording-timeline-v1', frames: []}));
  const value = await descriptor(recording);
  assert.deepEqual(value.entries.map(entry => entry.name),
    ['TEST123.mchq', 'TEST123.mchq.timeline.json', 'TEST123.mchq.replay.json', 'TEST123.mchq.json']);
  assert.equal(value.downloadName, 'TEST123.mchq.vwr');
  const output = path.join(root, 'shared.vwr');
  const writable = fs.createWriteStream(output);
  await streamPackage(value, writable);
  await new Promise(resolve => writable.once('close', resolve));
  assert.equal(fs.statSync(output).size, value.size, 'descriptor provides the exact streaming content length');
  const imported = await extractPackage(output, destination);
  assert.equal(imported.filename, 'TEST123.mchq');
  assert.deepEqual(imported.files,
    ['TEST123.mchq', 'TEST123.mchq.timeline.json', 'TEST123.mchq.replay.json', 'TEST123.mchq.json']);
  assert.deepEqual(fs.readFileSync(path.join(destination, imported.filename)), fs.readFileSync(recording));
  assert.deepEqual(JSON.parse(fs.readFileSync(path.join(destination, `${imported.filename}.replay.json`))),
    {schema: 'vectorwarp-replay-profile-v2'});
  fs.writeFileSync(path.join(destination, 'TEST123-1.mchq.json'), 'orphan must not be overwritten');
  const duplicate = await extractPackage(output, destination);
  assert.equal(duplicate.filename, 'TEST123-2.mchq',
    'imports never overwrite an existing recording or orphaned sidecar');
  assert.equal(fs.readFileSync(path.join(destination, 'TEST123-1.mchq.json'), 'utf8'),
    'orphan must not be overwritten');

  const bad = path.join(root, 'bad.vwr');
  const header = Buffer.from(JSON.stringify({name: '../escape.mchq', size: 1}));
  const prefix = Buffer.alloc(4); prefix.writeUInt32BE(header.length);
  fs.writeFileSync(bad, Buffer.concat([MAGIC, prefix, header, Buffer.from([1]), Buffer.alloc(4)]));
  await assert.rejects(extractPackage(bad, destination), /unsafe/);
  assert.equal(fs.existsSync(path.join(root, 'escape.mchq')), false);

  const truncated = path.join(root, 'truncated.vwr');
  fs.writeFileSync(truncated, fs.readFileSync(output).subarray(0, value.size - 3));
  await assert.rejects(extractPackage(truncated, destination), /truncated|trailing/);
  fs.rmSync(root, {recursive: true, force: true});
  console.log('Recording package streaming, complete replay import and traversal rejection passed.');
}

main().catch(error => { console.error(error); process.exitCode = 1; });
