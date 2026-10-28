'use strict';
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const net = require('net');
const {spawnSync} = require('child_process');
const {createReceiverJournal} = require('./receiver-journal');
const {checkReceiverStart, RECEIVER_START_BLOCKED_EXIT_STATUS} = require('./receiver-start-check');
const {createKrakenControlClient} = require('./receiver-sync');

async function main() {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-journal-'));
  let suite;
  try {
    const filename = path.join(directory, 'config.yml');
    const journal = createReceiverJournal(filename);
    assert.strictEqual(journal.load().exists, false);
    journal.write({state: 'in-progress', reconciliationRequired: true,
      receipt: {operations: [{commandSent: true, commandOutcome: 'unknown'}]}});
    assert.strictEqual(fs.statSync(journal.filename).gid, fs.statSync(directory).gid);
    assert.strictEqual(fs.statSync(journal.filename).mode & 0o777, 0o640);
    const recovered = createReceiverJournal(filename).recover();
    assert.strictEqual(recovered.state, 'interrupted');
    assert(recovered.reconciliationRequired);
    assert(recovered.receipt.operations[0].commandSent);
    journal.write({state: 'already-matched', reconciliationRequired: false, configRevision: 'a'});
    assert.strictEqual(journal.recover().state, 'unknown');
    assert.strictEqual(journal.recover().applicationVerified, false);
    journal.write({state: 'already-matched', reconciliationRequired: false,
      applicationVerified: true, configRevision: 'a'});
    assert.strictEqual(journal.recover('a').state, 'already-matched');
    assert.strictEqual(journal.recover('a').applicationVerified, true,
      'An exact-revision managed Apply attestation must survive an API restart');
    assert.strictEqual(journal.recover('b').applicationVerified, false,
      'A receipt for another revision must never attest the loaded application');
    const before = fs.readFileSync(journal.filename, 'utf8');
    const failing = createReceiverJournal(filename, {io: {...fs,
      renameSync() { throw new Error('injected rename failure'); }}});
    assert.throws(() => failing.write({state: 'in-progress', reconciliationRequired: true}),
      error => error.code === 'RECEIVER_JOURNAL_PERSISTENCE_FAILED');
    assert.strictEqual(fs.readFileSync(journal.filename, 'utf8'), before);
    fs.writeFileSync(journal.filename, '{truncated');
    assert(journal.recover().reconciliationRequired);
    fs.writeFileSync(journal.filename, JSON.stringify({schemaVersion: 1, state: 'in-progress', reconciliationRequired: false}));
    assert(journal.recover().reconciliationRequired, 'Malformed state cannot clear the startup interlock');
    fs.unlinkSync(journal.filename);
    fs.symlinkSync(filename, journal.filename);
    assert(journal.recover().reconciliationRequired);
    fs.unlinkSync(journal.filename);

    const document = {exists: true, validation: {valid: true}, revision: 'a',
      config: {capture: {device: {type: 'HackRF'}, replay: {state: false}}}};
    const options = {readConfig: () => document, journal};
    journal.write({state: 'saved-pending', reconciliationRequired: false, configRevision: 'a'});
    await assert.rejects(checkReceiverStart(filename, options), /pending or unresolved/);
    journal.write({state: 'in-progress', reconciliationRequired: true});
    await assert.rejects(checkReceiverStart(filename, options), /pending or unresolved/);
    journal.write({state: 'not-required', reconciliationRequired: false, configRevision: 'b'});
    await assert.rejects(checkReceiverStart(filename, options), /pending or unresolved/);
    journal.write({state: 'not-required', reconciliationRequired: false, configRevision: 'a'});
    assert.strictEqual((await checkReceiverStart(filename, options)).state, 'sdk-startup-required');
    document.config.capture.device.type = 'Kraken';
    await assert.rejects(checkReceiverStart(filename, {...options, client: {async verify() {
      journal.write({state: 'in-progress', reconciliationRequired: true}); return {};
    }}}), /changed during startup/);

    let commands = 0;
    suite = net.createServer(socket => {
      socket.on('data', () => { commands += 1; });
      socket.end(`${JSON.stringify({settings: {center_freq: 527000000, sample_rate: 2400000},
        num_channels: 5, max_elements: 8, reconfiguring: false, operating_mode: 'coherent'})}\n`);
    });
    await new Promise(resolve => suite.listen(0, '127.0.0.1', resolve));
    const config = {capture: {fc: 527000000, fs: 2400000,
      device: {type: 'Kraken', channel_count: 5,
        heimdall: {host: '127.0.0.1', control_port: suite.address().port}}}};
    const client = createKrakenControlClient();
    assert.strictEqual((await client.verify(config)).status, 'already-matched');
    config.capture.fc += 1000000;
    await assert.rejects(client.verify(config), error => error.code === 'KRAKEN_STARTUP_MISMATCH');
    assert.strictEqual(commands, 0, 'Startup readback must never retune a receiver');
    document.config.capture.replay.state = true;
    assert.strictEqual((await checkReceiverStart(filename, options)).state, 'replay');
    const blocked = spawnSync(process.execPath,
      [path.join(__dirname, 'receiver-start-check.js'), path.join(directory, 'missing.yml')],
      {encoding: 'utf8'});
    assert.strictEqual(blocked.status, RECEIVER_START_BLOCKED_EXIT_STATUS,
      'A receiver readiness block must not trigger the processor crash restart policy');
    assert.match(blocked.stderr, /Receiver startup blocked:/);
    console.log('PASS durable receiver journal and read-only startup interlock');
  } finally {
    if (suite) await new Promise(resolve => suite.close(resolve));
    fs.rmSync(directory, {recursive: true, force: true});
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
