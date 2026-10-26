'use strict';

// Focused state-machine fixture for a pre-journal SDRplay installation. It
// records only hashes and state labels: user configuration is never printed.
const assert = require('assert/strict');
const crypto = require('crypto');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {readConfig} = require('../../api/config-store');
const {createReceiverJournal} = require('../../api/receiver-journal');
const {evaluateConfigRevision} = require('../../api/config-revision-guard');

const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-upgrade-state-'));
try {
  const filename = path.join(directory, 'config.yml');
  // Model an existing configured SDRplay installation. config.yml is now the
  // deliberately invalid, unconfigured first-run form, which must remain a
  // higher-priority config-invalid failure rather than reach journal checks.
  fs.copyFileSync(path.join(__dirname, '../../config/config-pi4-rspduo.yml'), filename);
  fs.appendFileSync(filename, '\n# operator-owned pre-journal configuration\n');
  const hash = () => crypto.createHash('sha256').update(fs.readFileSync(filename)).digest('hex');
  const before = hash();
  const document = readConfig(filename);
  assert.equal(document.config.capture.device.type, 'RspDuo');
  assert.equal(document.validation.valid, true);
  const journal = createReceiverJournal(filename);
  assert.deepEqual(evaluateConfigRevision(document, journal.load()),
    {quiesce: true, reason: 'receiver-journal-missing'});

  // This is the exact settled state produced only after a deliberate Settings
  // Apply succeeds. It is matching, durable, and makes the guard pass once;
  // the YAML bytes are untouched by reconciliation.
  journal.write({state: 'synchronized', configRevision: document.revision,
    applicationVerified: true, reconciliationRequired: false,
    receipt: {schemaVersion: 1, receiverType: 'RspDuo', status: 'synchronized'}});
  assert.deepEqual(evaluateConfigRevision(document, journal.load()),
    {quiesce: false, reason: 'revision-settled'});
  assert.equal(hash(), before, 'receiver reconciliation must not rewrite user config');
  const guard = fs.readFileSync(path.join(__dirname, '../../script/vectorwarp-config-guard.js'), 'utf8');
  assert.match(guard, /Use the revision-guarded Settings Apply path/);
  assert.match(guard, /processor stopped pending exact receiver reconciliation/);
  console.log('Upgrade state: config preserved; one explicit matching Apply settles the receiver journal.');
} finally {
  fs.rmSync(directory, {recursive: true, force: true});
}
