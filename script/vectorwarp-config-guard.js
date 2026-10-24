#!/usr/bin/node
'use strict';

const fs = require('fs');
const path = require('path');
const {execFileSync} = require('child_process');
const apiRoot = path.join(__dirname, fs.existsSync(path.join(__dirname, '../current/api/config-store.js')) ? '../current/api' : '../api');
const {readConfig} = require(path.join(apiRoot, 'config-store'));
const {createReceiverJournal} = require(path.join(apiRoot, 'receiver-journal'));
const {evaluateConfigRevision} = require(path.join(apiRoot, 'config-revision-guard'));

function writeDiagnostic(filename, value) {
  fs.mkdirSync(path.dirname(filename), {recursive: true, mode: 0o750});
  const temporary = `${filename}.${process.pid}.tmp`;
  fs.writeFileSync(temporary, `${JSON.stringify(value, null, 2)}\n`, {mode: 0o640, flag: 'wx'});
  fs.renameSync(temporary, filename);
}

function main(argv = process.argv.slice(2)) {
  if (argv.length !== 2) throw new Error('usage: vectorwarp-config-guard CONFIG DIAGNOSTIC');
  const [configFile, diagnosticFile] = argv;
  const document = readConfig(configFile);
  const journal = createReceiverJournal(configFile).load();
  const decision = evaluateConfigRevision(document, journal);
  if (!decision.quiesce) return;
  let processorPid = 0;
  try {
    processorPid = Number(execFileSync('/usr/bin/systemctl',
      ['show', '--property=MainPID', '--value', 'vectorwarp-processor.service'],
      {encoding: 'utf8', timeout: 5000}).trim()) || 0;
  } catch (_) {}
  execFileSync('/usr/bin/systemctl', ['stop', 'vectorwarp-processor.service'],
    {encoding: 'utf8', timeout: 30000});
  const diagnostic = {schema: 'vectorwarp-config-guard-v1', detectedAt: Date.now(),
    reason: decision.reason, processorPid, configRevision: document.revision || null,
    journalRevision: journal.configRevision || null,
    journalState: journal.state || null, reconciliationRequired: journal.reconciliationRequired !== false,
    action: 'processor-stopped', recovery: 'Use the revision-guarded Settings Apply path or vectorwarp-restart.service; do not restart only the API.'};
  writeDiagnostic(diagnosticFile, diagnostic);
  console.error(`VECTORWARP_CONFIG_ALERT ${decision.reason}; processor stopped pending exact receiver reconciliation`);
}

if (require.main === module) {
  try { main(); } catch (error) { console.error(`VECTORWARP_CONFIG_ALERT ${error.stack}`); process.exitCode = 1; }
}

module.exports = {main, writeDiagnostic};
