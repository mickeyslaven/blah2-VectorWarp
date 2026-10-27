#!/usr/bin/node
'use strict';

const fs = require('fs');
const http = require('http');
const {execFileSync} = require('child_process');
const path = require('path');
const {readConfig} = require('../api/config-store');
const {createReceiverJournal} = require('../api/receiver-journal');
const {createReceiverSynchronizer} = require('../api/receiver-sync');
const {evaluateFrameLiveness} = require('../api/frame-liveness');

const PROCESSOR = 'vectorwarp-processor.service';
const HEIMDALL = 'vectorwarp-heimdall.service';
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));

function systemctl(...args) {
  return execFileSync('/usr/bin/systemctl', args, {encoding: 'utf8', timeout: 200000}).trim();
}

function active(unit) {
  try { return systemctl('is-active', '--quiet', unit) === ''; }
  catch (_) { return false; }
}

function pid(unit) {
  try { return Number(systemctl('show', '--property=MainPID', '--value', unit)) || 0; }
  catch (_) { return 0; }
}

function readState(filename) {
  try {
    const info = fs.lstatSync(filename);
    if (!info.isFile() || info.size > 131072) throw new Error('invalid state file');
    return JSON.parse(fs.readFileSync(filename, 'utf8'));
  } catch (error) {
    if (error.code === 'ENOENT') return {schema: 'vectorwarp-frame-watchdog-v1', attempts: [], history: []};
    throw error;
  }
}

function writeState(filename, value) {
  const output = {...value, schema: 'vectorwarp-frame-watchdog-v1', updatedAt: Date.now(),
    history: (value.history || []).slice(-32)};
  fs.mkdirSync(path.dirname(filename), {recursive: true, mode: 0o750});
  const temporary = `${filename}.${process.pid}.tmp`;
  fs.writeFileSync(temporary, `${JSON.stringify(output, null, 2)}\n`, {mode: 0o640, flag: 'wx'});
  fs.renameSync(temporary, filename);
}

function apiStatus(config, timeoutMs = 5000) {
  const host = config.network?.ip;
  const port = config.network?.ports?.api;
  return new Promise((resolve, reject) => {
    const request = http.get({host, port, path: '/api/system/status', timeout: timeoutMs}, response => {
      let body = '';
      response.setEncoding('utf8');
      response.on('data', chunk => {
        body += chunk;
        if (body.length > 262144) request.destroy(new Error('status response is oversized'));
      });
      response.on('end', () => {
        if (response.statusCode !== 200) return reject(new Error(`status HTTP ${response.statusCode}`));
        try { resolve(JSON.parse(body)); } catch (error) { reject(error); }
      });
    });
    request.on('timeout', () => request.destroy(new Error('status timeout')));
    request.on('error', reject);
  });
}

function requested(config) {
  const heimdall = config.capture.device.heimdall || {};
  return {frequency: config.capture.fc, sampleRate: config.capture.fs,
    channelCount: config.capture.device.channel_count,
    ...(heimdall.gain === undefined || heimdall.gain === 'keep' ? {} : {gain: heimdall.gain}),
    ...(heimdall.channel_gains === undefined ? {} : {channelGains: [...heimdall.channel_gains]})};
}

function sameSettings(left, right) {
  return JSON.stringify(left) === JSON.stringify(right);
}

function receiverSettlement(configRevision, wanted, receipt, now = Date.now()) {
  if (typeof configRevision !== 'string' || !configRevision ||
      !sameSettings(receipt?.requested, wanted) ||
      !['synchronized', 'already-matched'].includes(receipt?.status))
    throw new Error('receiver receipt does not settle the current exact configuration');
  return {state: receipt.status, completedAt: now, receipt, configRevision,
    applicationVerified: true, reconciliationRequired: false};
}

async function synchronizeAfterRestart(config, options = {}) {
  const factory = options.factory || createReceiverSynchronizer;
  const now = options.now || Date.now;
  const sleep = options.sleep || wait;
  const timeoutMs = options.timeoutMs ?? 60000;
  let receipt = null, lastConnectionError = null;
  const receiverDeadline = now() + timeoutMs;
  while (!receipt && now() < receiverDeadline) {
    try {
      receipt = await factory().synchronize(config, config, {force: true});
    } catch (error) {
      if (!['KRAKEN_CONNECTION_FAILED', 'KRAKEN_CONNECTION_CLOSED'].includes(error?.code))
        throw error;
      lastConnectionError = error;
      await sleep(250);
    }
  }
  if (!receipt)
    throw new Error(`receiver control did not become ready after restart: ${lastConnectionError?.message || 'timeout'}`);
  return receipt;
}

async function recover(configFile, baselineFrame) {
  const before = readConfig(configFile);
  if (!before.exists || before.readError || !before.validation.valid)
    throw new Error('active configuration is not valid');
  if (before.config.capture?.replay?.state === true ||
      before.config.capture?.device?.type !== 'Kraken')
    throw new Error('automatic receiver recovery is limited to live Kraken input');
  const wanted = requested(before.config);
  systemctl('stop', PROCESSOR);
  systemctl('restart', HEIMDALL);
  // systemctl reports the native process as started before its control socket
  // is listening. Retry only transport readiness; receiver/protocol failures
  // still fail closed immediately.
  const receipt = await synchronizeAfterRestart(before.config);
  const afterSync = readConfig(configFile);
  if (afterSync.revision !== before.revision)
    throw new Error('configuration revision changed during recovery; processor remains stopped');
  if (!sameSettings(receipt.requested, wanted) || !sameSettings(requested(afterSync.config), wanted))
    throw new Error('receiver recovery did not preserve the exact requested RF settings');
  createReceiverJournal(configFile).write(
    receiverSettlement(before.revision, wanted, receipt));

  const deadline = Date.now() + 150000;
  let lastError = null;
  while (Date.now() < deadline) {
    try { systemctl('start', PROCESSOR); } catch (error) { lastError = error; }
    if (active(PROCESSOR)) break;
    await wait(5000);
  }
  if (!active(PROCESSOR)) throw new Error(`processor did not start after calibration: ${lastError || 'blocked'}`);
  while (Date.now() < deadline) {
    try {
      const status = await apiStatus(afterSync.config);
      if (Number.isFinite(status.lastFrameAt) && status.lastFrameAt > (baselineFrame || 0) &&
          Math.abs(Date.now() - status.lastFrameAt) < 30000) {
        const final = readConfig(configFile);
        if (final.revision !== before.revision)
          throw new Error('configuration revision changed before frame recovery completed');
        return {ok: true, configRevision: before.revision, requested: wanted,
          receiverStatus: receipt.status, operations: receipt.operations,
          processorPid: pid(PROCESSOR), firstRecoveredFrameAt: status.lastFrameAt};
      }
    } catch (error) { lastError = error; }
    await wait(2000);
  }
  throw new Error(`processor started but frames did not advance: ${lastError || 'timeout'}`);
}

async function main(argv = process.argv.slice(2)) {
  if (argv.length !== 2) throw new Error('usage: vectorwarp-health-watchdog CONFIG STATE');
  const [configFile, stateFile] = argv;
  const document = readConfig(configFile);
  const previous = readState(stateFile);
  const processorActive = active(PROCESSOR);
  const processorPid = pid(PROCESSOR);
  let status = null;
  try { status = await apiStatus(document.config); delete previous.lastProbeError; }
  catch (error) { previous.lastProbeError = String(error.message).slice(0, 240); }
  const decision = evaluateFrameLiveness({now: Date.now(), processorActive, processorPid,
    lastFrameAt: status?.lastFrameAt ?? null}, previous);
  const next = {...previous, ...decision.state, configRevision: document.revision,
    lastProbeAt: Date.now(), lastProbeReason: decision.reason};
  if (decision.action !== 'recover') {
    writeState(stateFile, next);
    if (decision.reason === 'restart-loop-blocked') {
      console.error('VECTORWARP_LIVENESS_ALERT restart-loop-blocked; manual repair required');
      process.exitCode = 1;
    }
    return;
  }
  const incident = {detectedAt: Date.now(), reason: decision.reason,
    processorPid, lastFrameAt: status?.lastFrameAt ?? null,
    configRevision: document.revision};
  next.history = [...(next.history || []), incident];
  writeState(stateFile, next);
  console.error(`VECTORWARP_LIVENESS_ALERT ${decision.reason}; starting bounded exact-config recovery`);
  try {
    incident.recovery = await recover(configFile, status?.lastFrameAt ?? null);
    incident.completedAt = Date.now();
    next.status = 'recovered'; next.processorPid = incident.recovery.processorPid;
    next.processorSeenAt = Date.now(); next.lastFrameAt = incident.recovery.firstRecoveredFrameAt;
    next.unhealthySince = null;
  } catch (error) {
    incident.failedAt = Date.now(); incident.error = String(error.message).slice(0, 500);
    next.status = 'recovery-failed';
    console.error(`VECTORWARP_LIVENESS_ALERT recovery-failed: ${incident.error}`);
    process.exitCode = 1;
  }
  writeState(stateFile, next);
}

if (require.main === module)
  main().catch(error => { console.error(`VECTORWARP_LIVENESS_ALERT ${error.stack}`); process.exitCode = 1; });

module.exports = {main, receiverSettlement, requested, sameSettings, synchronizeAfterRestart};
