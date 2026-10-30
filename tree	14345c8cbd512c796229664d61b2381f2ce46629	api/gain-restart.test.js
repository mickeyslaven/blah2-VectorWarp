'use strict';

const assert = require('assert/strict');
const {test} = require('node:test');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {pending, publicationBlocked, completedState, complete,
  installedGainRestartSupported} = require('./gain-restart');
const {createReceiverJournal} = require('./receiver-journal');

function fixture() {
  const revision = 'a'.repeat(64);
  return {now: 10000, revision, loadedRevision: revision, serverId: 'new-api',
    config: {capture: {fc: 527000000, fs: 2400000, device: {type: 'Kraken'}}},
    state: {state: 'unknown', configRevision: revision, reconciliationRequired: false,
      receipt: {configRevision: revision, automaticGain: {status: 'verified',
        restartPendingAt: 8000, restartOriginServerId: 'original-api'}}},
    restartReceipt: {state: 'complete', inProgress: false, updatedAt: 9000,
      configRevision: revision},
    processorStatus: {receivedAt: 9500, value: {input: 'live', receiver: 'Kraken',
      state: 'live', frequency: 527000000, sampleRate: 2400000, error: ''}},
    timestampReceivedAt: 9600};
}

test('pending marker independently survives a successful journal and API restart', () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'gain-restart-'));
  try {
    const journal = createReceiverJournal(path.join(directory, 'config.yml'));
    const options = fixture();
    journal.write({...options.state, state: 'synchronized'});
    const recovered = journal.recover();
    assert.equal(recovered.state, 'unknown');
    assert.equal(recovered.reconciliationRequired, false);
    assert.equal(publicationBlocked(recovered), true);
    const next = complete({...options, state: recovered}, value => journal.write(value));
    assert.equal(pending(next), false);
    assert.equal(publicationBlocked(journal.recover()), false,
      'A later API restart remains usable after durable completion');
    assert.equal(next.receipt.automaticGain.restartVerification.helperCompletedAt, 9000);
    assert.equal(next.receipt.automaticGain.status, 'verified');
  } finally { fs.rmSync(directory, {recursive: true, force: true}); }
});

test('failed durable clear leaves original pending state blocked', () => {
  const options = fixture();
  const before = structuredClone(options.state);
  assert.throws(() => complete(options, () => { throw new Error('disk write failed'); }),
    /disk write failed/);
  assert.deepEqual(options.state, before);
  assert.equal(publicationBlocked(options.state), true);
});

test('no pending restart preserves ordinary publication and interrupted gain blocks', () => {
  assert.equal(publicationBlocked({reconciliationRequired: false}), false);
  assert.equal(publicationBlocked({reconciliationRequired: true, automaticGain: true}), true);
  assert.equal(publicationBlocked({reconciliationRequired: true,
    receipt: {automaticGain: {status: 'failed'}}}), true);
  const options = fixture();
  delete options.state.receipt.automaticGain.restartPendingAt;
  assert.equal(publicationBlocked(options.state), false);
  assert.equal(completedState(options), null);
});

const refusals = {
  'missing helper receipt': o => { o.restartReceipt = null; },
  'helper still in progress': o => { o.restartReceipt.inProgress = true; },
  'helper only ready': o => { o.restartReceipt.state = 'ready'; },
  'helper failed': o => { o.restartReceipt.state = 'failed'; },
  'helper predates pending marker': o => { o.restartReceipt.updatedAt = 7999; },
  'helper completion in future': o => { o.restartReceipt.updatedAt = 10001; },
  'helper wrong revision': o => { o.restartReceipt.configRevision = 'b'.repeat(64); },
  'API started from previous revision': o => { o.loadedRevision = 'b'.repeat(64); },
  'saved file changed again': o => { o.revision = 'b'.repeat(64); },
  'journal revision differs': o => { o.state.configRevision = 'b'.repeat(64); },
  'receiver receipt revision differs': o => { o.state.receipt.configRevision = 'b'.repeat(64); },
  'original API with same config revision': o => { o.serverId = 'original-api'; },
  'missing original process identity': o => { delete o.state.receipt.automaticGain.restartOriginServerId; },
  'missing current process identity': o => { delete o.serverId; },
  'malformed pending marker': o => { o.state.receipt.automaticGain.restartPendingAt = null; },
  'future pending marker': o => { o.state.receipt.automaticGain.restartPendingAt = 10001; },
  'receiver still requires reconciliation': o => { o.state.reconciliationRequired = true; },
  'missing processor telemetry': o => { o.processorStatus = null; },
  'old processor status before helper completion': o => { o.processorStatus.receivedAt = 8999; },
  'processor status no longer fresh': o => { o.now = 15001; },
  'future processor telemetry': o => { o.processorStatus.receivedAt = 10001; },
  'replay processor': o => { o.processorStatus.value.input = 'replay'; },
  'other receiver processor': o => { o.processorStatus.value.receiver = 'RspDuo'; },
  'stopped processor': o => { o.processorStatus.value.state = 'stopped'; },
  'processor reports error': o => { o.processorStatus.value.error = 'failed'; },
  'wrong processor frequency': o => { o.processorStatus.value.frequency = 539000000; },
  'wrong processor rate': o => { o.processorStatus.value.sampleRate = 1920000; },
  'replay configuration': o => { o.config.capture.replay = {state: true}; },
  'other receiver configuration': o => { o.config.capture.device.type = 'RspDuo'; },
  'no current timestamp connection': o => { o.timestampReceivedAt = null; },
  'timestamp before helper completion': o => { o.timestampReceivedAt = 8999; },
  'future timestamp': o => { o.timestampReceivedAt = 10001; },
  'timestamp no longer fresh': o => { o.timestampMaxAgeMs = 300; }
};
for (const [name, change] of Object.entries(refusals)) test(`keeps gate closed: ${name}`, () => {
  const options = fixture(); change(options);
  let wrote = false;
  assert.equal(complete(options, () => { wrote = true; }), null);
  assert.equal(wrote, false);
  assert.equal(publicationBlocked(options.state), true);
});

test('timestamp arriving before root completion can retry on a later current frame', () => {
  const options = fixture(); options.timestampReceivedAt = 8999;
  assert.equal(completedState(options), null);
  options.timestampReceivedAt = 9999;
  assert.ok(completedState(options));
});

test('only the installed Linux restart-helper contract supports automatic gain', () => {
  const command = ['/opt/vectorwarp/libexec/vectorwarp-receiver-helper', 'request-restart'];
  assert.equal(installedGainRestartSupported(command, {platform: 'linux'}), true);
  for (const platform of ['darwin', 'win32'])
    assert.equal(installedGainRestartSupported(command, {platform}), false);
  for (const invalid of [null, [], ['vectorwarp-receiver-helper', 'request-restart'],
    ['/bin/true'], ['/bin/other', 'request-restart'], [command[0], 'status'],
    [...command, '--extra'], [123, 'request-restart']])
    assert.equal(installedGainRestartSupported(invalid, {platform: 'linux'}), false);
});
