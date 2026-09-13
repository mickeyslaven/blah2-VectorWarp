const express = require('express');
const net = require("net");
const fs = require('fs');
const yaml = require('js-yaml');
const dns = require('dns');
const path = require('path');
const {spawn} = require('child_process');
const {getDeviceProfiles, validateConfig, FIELD_RULES} = require('./config-manager.js');
const {readConfig, writable, saveConfig} = require('./config-store.js');
const {getUpstreamStatus} = require('./upstream-status.js');
const {installReceiverRoutes, trustedOrigins, sameReceiverOrigin} = require('./receiver-routes.js');
const {createReceiverSynchronizer, receiverAcceptanceBoundary} = require('./receiver-sync.js');
const {createReceiverJournal} = require('./receiver-journal.js');
const {invalidate: invalidateGeometry} = require('../html/js/kraken_geometry');
const {createAdsbSource} = require('./adsb-source.js');
const {checkNetworkBindings, bindMessage} = require('./network-check.js');
const {status: validateProcessorStatus, fresh: processorStatusFresh} = require('./processor-status.js');
const {createGpuSetupStatus} = require('./gpu-setup.js');
const gpuSetupStatus = createGpuSetupStatus({preview: process.env.BLAH2_PREVIEW === 'true'});

// parse config file
const configFile = path.resolve(process.argv[2] || 'config/config.yml');
const startupDocument = readConfig(configFile);
var config = startupDocument.config;
const adsbSource = createAdsbSource(config, {preview: process.env.BLAH2_PREVIEW === 'true'});
if (startupDocument.setupRequired)
  console.error('Setup required:', startupDocument.readError || startupDocument.validation.errors);
const serverId = `${process.pid}-${Date.now()}`;
const serviceErrors = new Map();
const ownedPorts = new Set();
let restartState = {state: 'idle'};
let lastFrameAt = null;
let lastTimingAt = null;
let timestampConnections = 0;
let restartGeneration = 0;
let timestampGeneration = 0;
let timingGeneration = 0;
let activeTimestampConnection = 0;
let activeTimingConnection = 0;
let upstreamCache = null;
let processorStatus = null;
let configWriteInProgress = false;
// A saved file or API restart is not evidence of receiver application.
const receiverJournal = createReceiverJournal(configFile);
let receiverSyncState = receiverJournal.recover();
let receiverReconciliationRequired = receiverSyncState.reconciliationRequired;
function receiverTimeoutOption(name, maximum) {
  if (process.env[name] === undefined) return undefined;
  const value = Number(process.env[name]);
  if (!Number.isInteger(value) || value < 50 || value > maximum)
    throw new Error(`${name} must be an integer from 50 through ${maximum}.`);
  return value;
}
const receiverSynchronizer = createReceiverSynchronizer({krakenOptions: {
  statusTimeoutMs: receiverTimeoutOption('BLAH2_RECEIVER_STATUS_TIMEOUT_MS', 10000),
  readbackTimeoutMs: receiverTimeoutOption('BLAH2_RECEIVER_READBACK_TIMEOUT_MS', 30000)
}});
const RECEIVER_SYNC_HEADER = 'X-VectorWarp-Receiver-Sync';
const RECEIVER_SYNC_INTENT = 'synchronize-v1';

function replayTruthDisabled() {
  return config.capture?.replay?.state === true ||
    (processorStatusFresh(processorStatus) && processorStatus.value.input === 'replay');
}
function replayTruthStatus() {
  return {enabled: false, online: false, disabledForReplay: true,
    message: 'Live ADS-B truth is not replayed.'};
}

let restartCommand = null;
if (process.env.BLAH2_CONFIG_RESTART_COMMAND) {
  try {
    const parsed = JSON.parse(process.env.BLAH2_CONFIG_RESTART_COMMAND);
    if (!Array.isArray(parsed) || parsed.length === 0 ||
        parsed.some(value => typeof value !== 'string' || value.length === 0))
      throw new Error('expected a JSON array of non-empty strings');
    restartCommand = parsed;
  } catch (error) {
    console.error(`Invalid BLAH2_CONFIG_RESTART_COMMAND: ${error.message}`);
  }
}

var stash_map = require('./stash/maxhold.js');
var stash_detection = require('./stash/detection.js');
var stash_iqdata = require('./stash/iqdata.js');
var stash_timing = require('./stash/timing.js');

// constants
const setupPort = Number(process.env.BLAH2_SETUP_PORT || 3000);
const PORT = process.env.BLAH2_SETUP_PORT ? setupPort : Number.isInteger(config.network?.ports?.api) &&
  config.network.ports.api > 0 && config.network.ports.api <= 65535 ?
  config.network.ports.api : setupPort;
const HOST = net.isIP(config.network?.ip || '') ? config.network.ip : '0.0.0.0';
var map = '';
var detection = '';
var track = '';
var timestamp = '';
var timing = '';
var iqdata = '';
var data_map = '';
var data_detection = '';
var data_tracker = '';
var data_timestamp = '';
var data_timing = '';
var data_iqdata = '';
var capture = false;
var captureStartedAt = null;
let captureRevision = 0;

function invalidateTimestampTelemetry() {
  activeTimestampConnection = ++timestampGeneration;
  lastFrameAt = null;
  timestamp = '';
  data_timestamp = '';
  return {restart: restartGeneration, connection: activeTimestampConnection};
}

function invalidateTimingTelemetry() {
  activeTimingConnection = ++timingGeneration;
  lastTimingAt = null;
  timing = '';
  data_timing = '';
  return {restart: restartGeneration, connection: activeTimingConnection};
}

function timestampConnectionCurrent(generation) {
  return generation.restart === restartGeneration && generation.connection === activeTimestampConnection;
}

function timingConnectionCurrent(generation) {
  return generation.restart === restartGeneration && generation.connection === activeTimingConnection;
}

function currentTimingBackend() {
  try { return JSON.parse(timing) || {}; }
  catch (_) { return {}; }
}

function currentGpuRuntime(freshness) {
  const now = Date.now();
  const backend = currentTimingBackend();
  const receiving = lastFrameAt !== null && now - lastFrameAt < freshness;
  return {acceleration: backend.acceleration, clutterAcceleration: backend.clutterAcceleration,
    fresh: receiving && lastTimingAt !== null &&
      now - lastTimingAt < freshness};
}

// api server
const app = express();
const {readSdrplayStartup} = require('./sdrplay-startup');
const {installSdrplayBuildRoutes, helperStatus} = require('./sdrplay-build');
app.use(express.json({limit: '256kb', strict: true}));
function configWriteOriginAllowed(req) {
  const origin = req.get('Origin');
  if (!origin) return true;
  try {
    const originHost = new URL(origin).hostname;
    const requestHost = new URL(`http://${req.get('Host')}`).hostname;
    return originHost === requestHost;
  } catch (_) {
    return false;
  }
}
function processorStatusOriginAllowed(req) {
  const origin = req.get('Origin');
  if (!origin) return true; // Native processor telemetry has no browser Origin.
  try { return new URL(origin).host === req.get('Host'); }
  catch (_) { return false; }
}
// header on all requests
app.use(function(req, res, next) {
  const origin = req.get('Origin');
  if (!origin || req.method === 'GET' || configWriteOriginAllowed(req)) {
    res.header("Access-Control-Allow-Origin", origin || "*");
    if (origin) res.header('Vary', 'Origin');
  }
  res.header('Access-Control-Allow-Methods', 'GET, POST, PUT, OPTIONS');
  res.header('Access-Control-Allow-Headers', `Content-Type, If-Match, ${RECEIVER_SYNC_HEADER}`);
  res.header('Access-Control-Expose-Headers', 'ETag');
  res.header('Cache-Control', 'private, no-cache, no-store, must-revalidate');
  res.header('Expires', '-1');
  res.header('Pragma', 'no-cache');
  next();
});
app.options('*', (req, res) => {
  if (req.path === '/api/config' && !configWriteOriginAllowed(req))
    return res.status(403).json({ok: false,
      errors: ['Configuration changes must come from this VectorWarp host.']});
  res.sendStatus(204);
});

function configFileWritable() {
  return writable(configFile);
}

function restartCommandAvailable() {
  if (!restartCommand) return false;
  const command = restartCommand[0];
  const candidates = command.includes(path.sep) ? [command] :
    (process.env.PATH || '').split(path.delimiter)
      .filter(Boolean).map(folder => path.join(folder, command));
  return candidates.some(candidate => {
    try { fs.accessSync(candidate, fs.constants.X_OK); return true; }
    catch (_) { return false; }
  });
}

function configuredReceiverTypes() {
  return process.env.BLAH2_RECEIVER_TYPES?.split(',').map(item => item.trim()).filter(Boolean) || null;
}
const localSdrplayHelper = process.env.BLAH2_SDRPLAY_BUILD_HELPER ||
  '/opt/vectorwarp/libexec/vectorwarp-build-sdrplay';
function localRspEnrolled() {
  return process.env.BLAH2_SDRPLAY_LOCAL_BUILD === 'true' &&
    (process.env.BLAH2_LOCAL_BUILD_RECEIVER_TYPES || '').split(',').map(item => item.trim()).includes('RspDuo');
}
async function localRspCurrent() {
  if (process.env.BLAH2_PREVIEW === 'true' || !localRspEnrolled()) return false;
  const status = await helperStatus(localSdrplayHelper);
  return status?.ok === true && status.state === 'current';
}

function launchRestart() {
  restartGeneration += 1;
  invalidateTimestampTelemetry();
  invalidateTimingTelemetry();
  const [command, ...args] = restartCommand;
  restartState = {...restartState, state: 'running', startedAt: Date.now(),
    timestampConnections};
  const child = spawn(command, args, {detached: true, stdio: 'ignore'});
  const timeout = setTimeout(() => {
    restartState = {...restartState, state: 'failed',
      message: 'The restart command did not finish within 30 seconds. Check the service manager before retrying.'};
  }, 30000);
  timeout.unref();
  child.on('error', error => {
    clearTimeout(timeout);
    restartState = {...restartState, state: 'failed', message: `Restart could not start: ${error.message}`};
  });
  child.on('exit', (code, signal) => {
    clearTimeout(timeout);
    restartState = {...restartState, state: code === 0 ? 'command-complete' : 'failed',
      message: code === 0 ? 'Restart command finished. Waiting for a new radar connection.' :
        `Restart command failed (${signal || `exit ${code}`}). Check the service manager.`};
  });
  child.unref();
}
app.get('/', (req, res, next) => {
  if (fs.existsSync(path.join(__dirname, '..', 'html', 'index.html'))) return next();
  res.json({service: 'VectorWarp API', settings: '/api/config', status: '/api/system/status'});
});
app.get('/api/map', (req, res) => {
  res.send(map);
});
app.get('/api/detection', (req, res) => {
  res.send(detection);
});
app.get('/api/tracker', (req, res) => {
  res.send(track);
});
app.get('/api/timestamp', (req, res) => {
  res.send(timestamp);
});
app.get('/api/timing', (req, res) => {
  res.send(timing);
});
app.get('/api/iqdata', (req, res) => {
  res.send(iqdata);
});
app.get('/api/config', (req, res) => {
  const document = readConfig(configFile);
  res.set('ETag', `"${document.revision}"`).json(document.config);
});
app.get('/api/runtime/config', (req, res) => {
  // API startup snapshot, not hardware acknowledgement. Do not relabel frames
  // with newly saved tuning/site settings until the services have restarted.
  res.json(config);
});
app.get('/api/config/capabilities', async (req, res) => {
  const document = readConfig(configFile);
  const compiledLiveTypes = configuredReceiverTypes();
  // A current local build grants only RSPduo live eligibility. It remains
  // distinct from `compiledLiveTypes`: this adapter was not shipped compiled.
  const localRspLive = await localRspCurrent();
  res.json({
    editable: configFileWritable(),
    restartAvailable: restartCommandAvailable(),
    maximumConfigBytes: 262144,
    changesRequireRestart: true,
    // Replay validates all file/profile combinations without loading receiver
    // SDKs. Keep every profile editable, while accurately exposing live support.
    deviceProfiles: getDeviceProfiles().map(item => ({...item,
      liveAvailable: !compiledLiveTypes || compiledLiveTypes.includes(item.type) ||
        (item.type === 'RspDuo' && localRspLive)})),
    compiledLiveTypes: compiledLiveTypes || getDeviceProfiles().map(item => item.type),
    localBuildLiveTypes: localRspLive ? ['RspDuo'] : [],
    fieldRules: FIELD_RULES,
    receiverSynchronization: {
      intentHeader: RECEIVER_SYNC_HEADER,
      intentValue: RECEIVER_SYNC_INTENT,
      pendingIntentValue: 'save-pending-v1',
      applicationState: receiverSyncState.state,
      currentReceiver: receiverAcceptanceBoundary(document.config)
    },
    setupRequired: document.setupRequired,
    setupMessage: document.readError,
    suppliedDefaults: document.suppliedDefaults,
    configRevision: document.revision,
    validation: document.validation,
    configPath: configFile
  });
});
app.get('/api/system/status', async (req, res) => {
  const document = readConfig(configFile);
  const sdrplayStartup = readSdrplayStartup(document.revision);
  const persistedRestart = sdrplayStartup && sdrplayStartup.updatedAt >= (restartState.requestedAt || 0) ?
    {state: sdrplayStartup.inProgress ? 'running' : sdrplayStartup.state === 'failed' ? 'failed' : 'command-complete',
      message: sdrplayStartup.message, startedAt: sdrplayStartup.updatedAt} : restartState;
  const processor = processorStatusFresh(processorStatus) ? processorStatus.value : null;
  const freshness = Math.max(10000, (config.process?.data?.cpi || 1) * 3000);
  const gpuSetup = await gpuSetupStatus(() => currentGpuRuntime(freshness));
  const backend = currentTimingBackend();
  res.json({serverId, configRevision: document.revision,
    acceleration: backend.acceleration || null,
    clutterAcceleration: backend.clutterAcceleration || null, gpuSetup,
    loadedRevision: startupDocument.revision, setupRequired: document.setupRequired,
    restart: persistedRestart, sdrplayStartup, receiverSynchronization: receiverSyncState,
    lastFrameAt, timestampConnections,
    adsbEnabled: config.truth?.adsb?.enabled === true,
    radar: lastFrameAt === null ? 'no-data' : Date.now() - lastFrameAt <
      Math.max(10000, (config.process?.data?.cpi || 1) * 3000) ? 'receiving' : 'stale',
    processor: processor && {...processor, receivedAt: processorStatus.receivedAt},
    processorFresh: processor !== null,
    errors: [...serviceErrors.values()],
    message: lastFrameAt === null ? 'No radar frames received. Check the receiver, processing service and its logs. Settings remain available.' : null});
});
// The processor runs without a browser Origin header. If a browser supplies one,
// retain the same-host restriction used by configuration writes. This telemetry is
// display state only: it never validates radio hardware or permits configuration.
app.post('/api/processor/status', (req, res) => {
  if (!processorStatusOriginAllowed(req))
    return res.status(403).json({ok: false, errors: ['Processor telemetry must come from this VectorWarp host.']});
  const checked = validateProcessorStatus(req.body);
  if (!checked.valid) return res.status(422).json({ok: false, errors: checked.errors});
  processorStatus = {value: checked.value, receivedAt: Date.now()};
  if (checked.value.input === 'replay') adsbSource.clear();
  if (checked.value.input === 'live' && checked.value.recordingError &&
      checked.value.recordingRequestId === captureRevision) {
    capture = false;
    captureStartedAt = null;
  }
  res.status(204).end();
});
app.get('/api/upstream/status', async (req, res) => {
  if (process.env.BLAH2_PREVIEW === 'true') return res.json({supported: true,
    available: false, matched: false, host: 'not connected', controlPort: '—',
    message: 'UI preview only. No receiver connection is opened and no live data is generated.'});
  const document = readConfig(configFile);
  if (!upstreamCache || upstreamCache.revision !== document.revision ||
      Date.now() - upstreamCache.at > 4000) {
    upstreamCache = {at: Date.now(), revision: document.revision,
      promise: getUpstreamStatus(document.config)};
  }
  const status = await upstreamCache.promise;
  res.status(status.available === false ? 503 : 200).json(status);
});
const receiverOrigins = trustedOrigins(PORT, (process.env.BLAH2_RECEIVER_ORIGINS || '').split(',').filter(Boolean));
const receiverManagement = installReceiverRoutes(app, {
  readDocument: () => readConfig(configFile),
  port: PORT,
  allowedOrigins: receiverOrigins,
  helperExecutable: process.env.BLAH2_RECEIVER_HELPER,
  extraOrigins: (process.env.BLAH2_RECEIVER_ORIGINS || '').split(',').filter(Boolean),
  transactionBusy: () => configWriteInProgress || ['running', 'scheduled'].includes(restartState.state) ||
    readSdrplayStartup(null)?.inProgress === true,
  processorStatus: () => processorStatusFresh(processorStatus) ? processorStatus.value : null,
  preview: process.env.BLAH2_PREVIEW === 'true',
  compiledLiveTypes: process.env.BLAH2_RECEIVER_TYPES ?
    process.env.BLAH2_RECEIVER_TYPES.split(',').map(value => value.trim()).filter(Boolean) : null
});
installSdrplayBuildRoutes(app, {allowedOrigins: receiverOrigins,
  helper: process.env.BLAH2_SDRPLAY_BUILD_HELPER || '/opt/vectorwarp/libexec/vectorwarp-build-sdrplay'});
app.post('/api/config/validate', async (req, res) => {
  if (!configWriteOriginAllowed(req))
    return res.status(403).json({valid: false,
      errors: ['Configuration checks must come from this VectorWarp host.']});
  const validation = validateConfig(req.body, readConfig(configFile).config);
  if (validation.valid) {
    validation.errors.push(...await checkNetworkBindings(req.body, config, ownedPorts));
    validation.valid = validation.errors.length === 0;
  }
  res.status(validation.valid ? 200 : 422).json(validation);
});
app.put('/api/config', async (req, res) => {
  if (!configWriteOriginAllowed(req))
    return res.status(403).json({ok: false,
      errors: ['Configuration changes must come from this VectorWarp host.']});
  if (!configFileWritable())
    return res.status(503).json({ok: false,
      errors: ['The active configuration file is read-only.']});
  const saveLater = req.query.mode === 'pending';
  if (req.query.mode !== undefined && !saveLater)
    return res.status(422).json({ok: false, errors: ['Unknown configuration save mode.']});
  const wantsRestart = req.query.restart === 'true';
  if (wantsRestart && req.body?.capture?.device?.type === 'RspDuo' &&
      req.body?.capture?.replay?.state !== true &&
      (req.get(RECEIVER_SYNC_HEADER) !== RECEIVER_SYNC_INTENT || !sameReceiverOrigin(req, receiverOrigins)))
    return res.status(428).json({ok: false, code: 'SDRPLAY_START_INTENT_REQUIRED',
      errors: ['Apply RSPduo settings from the trusted VectorWarp page. Starting installed SDRplay requires explicit same-origin Apply intent.']});
  if (readSdrplayStartup(null)?.inProgress)
    return res.status(409).json({ok: false, errors: ['A receiver restart is already in progress. Wait for its result.']});
  if (saveLater && (req.query.restart !== 'false' ||
      req.get(RECEIVER_SYNC_HEADER) !== 'save-pending-v1' || !sameReceiverOrigin(req, receiverOrigins)))
    return res.status(428).json({ok: false, code: 'PENDING_SAVE_INTENT_REQUIRED',
      errors: ['Save for later requires its explicit same-origin intent and restart=false. No settings were saved.']});
  if (wantsRestart && !restartCommandAvailable())
    return res.status(503).json({ok: false,
      errors: ['A safe restart command is not configured on this server.']});
  if (!req.get('If-Match'))
    return res.status(428).json({ok: false, errors: ['Reload settings before saving (configuration revision required).']});
  if (restartState.state === 'running' || restartState.state === 'scheduled')
    return res.status(409).json({ok: false, errors: ['A restart is already in progress. Wait for its result.']});
  if (configWriteInProgress || receiverManagement.busy())
    return res.status(409).json({ok: false,
      errors: ['Another settings transaction is in progress. Wait for its result.']});
  const allowedTypes = configuredReceiverTypes();
  const replayRequested = req.body?.capture?.replay?.state === true;
  const localRspRequested = !saveLater && !replayRequested &&
    req.body?.capture?.device?.type === 'RspDuo' && allowedTypes && !allowedTypes.includes('RspDuo');
  if (!saveLater && allowedTypes && !replayRequested && !allowedTypes.includes(req.body?.capture?.device?.type) && !localRspRequested)
    return res.status(422).json({ok: false, errors: [
      'This receiver backend is unavailable for live capture in this installation. Use Save for later, or install a build containing this backend before applying it.'
    ]});
  const currentDocument = readConfig(configFile);
  const expectedRevision = req.get('If-Match').replace(/^"|"$/g, '');
  if (expectedRevision !== currentDocument.revision)
    return res.status(409).json({ok: false,
      errors: ['The config file changed. Reload settings before saving; your edits have not been written.']});
  const validation = validateConfig(req.body, currentDocument.config);
  if (!validation.valid)
    return res.status(422).json({ok: false, errors: validation.errors});
  if (saveLater && receiverReconciliationRequired)
    return res.status(409).json({ok: false, code: 'RECEIVER_RECONCILIATION_REQUIRED',
      receiverSync: receiverSyncState.receipt,
      errors: ['An earlier receiver command has an unresolved outcome. Reconcile it before saving for later; its receipt and saved settings were preserved.']});
  // A previous attempt may have changed Suite without saving YAML, including
  // before this API process started. Never use an unchanged file as readback.
  const observeLiveReceiver = req.body?.capture?.device?.type === 'Kraken' &&
    req.body?.capture?.replay?.state !== true && process.env.BLAH2_PREVIEW !== 'true';
  const receiverSyncRequired = !saveLater && (observeLiveReceiver ||
    receiverSynchronizer.requires(currentDocument.config, req.body));
  if (receiverSyncRequired && process.env.BLAH2_PREVIEW === 'true')
    return res.status(409).json({ok: false, code: 'RECEIVER_SYNC_DISABLED_IN_PREVIEW',
      errors: ['UI preview cannot send settings to a receiver. Use an isolated simulated Suite V2 service for receiver tests.']});
  if (receiverSyncRequired && req.get(RECEIVER_SYNC_HEADER) !== RECEIVER_SYNC_INTENT)
    return res.status(428).json({ok: false, code: 'RECEIVER_SYNC_INTENT_REQUIRED',
      errors: [`Receiver-controlled settings require the ${RECEIVER_SYNC_HEADER}: ${RECEIVER_SYNC_INTENT} intent header.`]});
  configWriteInProgress = true;
  let receiverSync = null;
  const previousReceiverReceipt = receiverSyncState.receipt || null;
  try {
    // Hold the write flag across this bounded read-only query. A stale local
    // adapter cannot race another Apply into a save or restart.
    if (localRspRequested && !await localRspCurrent())
      return res.status(422).json({ok: false, code: 'SDRPLAY_LOCAL_BUILD_REQUIRED',
        errors: ['Build SDRplay support in Settings after installing the SDRplay API and headers, then apply live RSPduo settings.']});
    const networkErrors = await checkNetworkBindings(req.body, config, ownedPorts);
    if (networkErrors.length) return res.status(422).json({ok: false, errors: networkErrors});
    // Another request can finish while the asynchronous bind checks run.
    if (restartState.state === 'running' || restartState.state === 'scheduled' || readSdrplayStartup(null)?.inProgress)
      return res.status(409).json({ok: false, errors: ['A restart is already in progress. Wait for its result.']});
    if (readConfig(configFile).revision !== currentDocument.revision)
      return res.status(409).json({ok: false,
        errors: ['The config file changed during validation. Reload settings; no receiver command was sent.']});
    // Write-ahead state survives API death before a command ACK or YAML rename.
    // A failed journal write prevents every receiver/config mutation below.
    receiverJournal.write({state: 'in-progress', startedAt: Date.now(),
      previousRevision: currentDocument.revision, reconciliationRequired: true,
      receiverType: req.body.capture.device.type, saveLater,
      receipt: receiverSyncState.receipt || null});
    if (saveLater) {
      // Explicit disk-only transaction. Never probe, retune, invoke the helper,
      // restart, or clear an uncertain receiver receipt in this branch.
      let saved;
      try { saved = saveConfig(configFile, req.body, currentDocument.revision); }
      catch (error) {
        // A directory fsync can fail after atomic rename. Do not promise that
        // the old file survived merely because persistence returned an error.
        console.error(`Unable to save pending configuration: ${error.message}`);
        const receipt = {schemaVersion: 1, receiverType: req.body.capture.device.type,
          status: 'pending-save-unknown', configPersisted: null, receiverApplied: false,
          applicationVerified: false, hardwareVerified: false, operations: []};
        receiverSyncState = {state: 'unknown', receipt, applicationVerified: false,
          reconciliationRequired: true};
        receiverReconciliationRequired = true;
        receiverJournal.write(receiverSyncState);
        return res.status(error.status || 500).json({ok: false,
          code: 'PENDING_SAVE_PERSISTENCE_FAILED', receiverSync: receipt,
          errors: ['File save outcome is unknown. Reload the saved file before retrying; check configuration-directory permissions, disk space and the API log. No receiver commands or restart were requested.']});
      }
      const receipt = {schemaVersion: 1, receiverType: req.body.capture.device.type,
        status: 'saved-pending', configPersisted: true, configRevision: saved.revision,
        receiverApplied: false, applicationVerified: false, hardwareVerified: false,
        operations: []};
      receiverSyncState = {state: 'saved-pending', completedAt: Date.now(), receipt,
        configRevision: saved.revision, reconciliationRequired: receiverReconciliationRequired};
      try { receiverJournal.write(receiverSyncState); }
      catch (error) { error.receiverSync = receipt; throw error; }
      upstreamCache = null;
      return res.json({ok: true, restarting: false, revision: saved.revision,
        config: saved.config, receiverSync: receipt,
        message: 'Settings saved for later. No receiver commands or restart were requested. Application remains pending; use Apply when the receiver is available.'});
    }
    receiverSyncState = {state: receiverSyncRequired ? 'synchronizing' : 'not-required',
      startedAt: Date.now(), receipt: null};
    receiverSync = await receiverSynchronizer.synchronize(currentDocument.config, req.body,
      {force: observeLiveReceiver});
    if (receiverSync.operations?.some(operation => operation.commandSent))
      invalidateGeometry(req.body?.capture?.device?.array_geometry);
    let saved;
    try {
      saved = saveConfig(configFile, req.body, currentDocument.revision);
    } catch (error) {
      if (receiverSyncRequired) {
        const changedUpstream = receiverSync.operations?.length > 0;
        const revisionConflict = error.status === 409;
        error.receiverSync = {...receiverSync,
          status: changedUpstream ? 'partial' : 'receiver-confirmed-config-not-saved',
          configPersisted: false,
          persistenceError: 'Receiver confirmation completed, but the VectorWarp config was not saved.'};
        error.code = revisionConflict ? 'RECEIVER_SYNC_PERSISTENCE_CONFLICT' :
          'RECEIVER_SYNC_PERSISTENCE_FAILED';
        error.status = revisionConflict ? 409 : 500;
        error.message = revisionConflict ?
          `${error.message} Suite V2 may already have applied the receiver settings; reload and reconcile before retrying.` :
          'Suite V2 confirmed the receiver settings, but VectorWarp could not save its config. The receiver may already be changed; inspect the API log, then reload and reconcile before retrying.';
      }
      throw error;
    }
    receiverSync = {...receiverSync, configPersisted: true, configRevision: saved.revision};
    // Direct backends apply through their SDK at processor startup. A deliberate
    // Apply can repair a disk-only failure there, but must not claim to have
    // reconciled a previous, different upstream receiver.
    const startupOwned = req.body.capture.device.type !== 'Kraken' || req.body.capture.replay?.state === true;
    if (startupOwned && receiverReconciliationRequired)
      receiverSync = {...receiverSync, priorOutcomeUnverified: true,
        priorReceiverReceipt: previousReceiverReceipt};
    if (receiverSyncRequired || startupOwned) receiverReconciliationRequired = false;
    receiverSyncState = {state: receiverSync.status, completedAt: Date.now(),
      receipt: receiverSync, configRevision: saved.revision,
      reconciliationRequired: receiverReconciliationRequired};
    try { receiverJournal.write(receiverSyncState); }
    catch (error) {
      error.receiverSync = {...receiverSync, status: 'config-saved-journal-unknown'};
      throw error;
    }
    upstreamCache = null;
    res.json({ok: true, restarting: wantsRestart, revision: saved.revision, config: saved.config,
      receiverSync,
      message: wantsRestart ? 'Configuration saved. Restart requested.' :
        'Configuration saved. Restart VectorWarp processing and its API to apply it.'});
    if (wantsRestart) {
      restartState = {state: 'scheduled', requestedAt: Date.now(), revision: saved.revision};
      setTimeout(launchRestart, 150).unref();
    }
  } catch (error) {
    console.error(`Unable to save configuration: ${error.message}`);
    if (error.code === 'RECEIVER_JOURNAL_PERSISTENCE_FAILED') receiverReconciliationRequired = true;
    if (receiverSyncRequired) upstreamCache = null;
    if (error.receiverSync) {
      if (error.receiverSync.operations?.some(operation => operation.commandSent &&
          operation.commandOutcome !== 'rejected') ||
          ['RECEIVER_SYNC_PERSISTENCE_CONFLICT', 'RECEIVER_SYNC_PERSISTENCE_FAILED'].includes(error.code))
        receiverReconciliationRequired = true;
      error.receiverSync = {...error.receiverSync,
        configPersisted: error.receiverSync.configPersisted === true};
      receiverSyncState = {state: error.receiverSync.status || 'failed',
        completedAt: Date.now(), receipt: error.receiverSync,
        reconciliationRequired: receiverReconciliationRequired};
    } else if (receiverSyncRequired)
      receiverSyncState = {state: 'failed', completedAt: Date.now(),
        receipt: null, code: error.code || 'RECEIVER_SYNC_FAILED'};
    receiverSyncState = {...receiverSyncState,
      reconciliationRequired: receiverReconciliationRequired};
    try { receiverJournal.write(receiverSyncState); }
    catch (_) { receiverReconciliationRequired = true; }
    res.status(error.status || 500).json({ok: false,
      ...(error.code ? {code: error.code} : {}),
      ...(error.receiverSync ? {receiverSync: error.receiverSync} : {}),
      errors: [error.status ? error.message : 'Unable to save safely. Check file ownership, directory permissions and free disk space.']});
  } finally {
    configWriteInProgress = false;
  }
});
app.get('/api/adsb2dd', (req, res) => {
  if (replayTruthDisabled()) return res.json(replayTruthStatus());
  if (!config.truth?.adsb?.enabled) return res.status(400).end();
  // Compatibility link now stays on this API; no converter URL or query is accepted.
  res.json({url: '/api/adsb/delay-doppler'});
});
for (const [route, kind] of [['/api/adsb', 'aircraft'], ['/api/adsb/delay-doppler', 'delayDoppler']]) {
  app.get(route, async (req, res) => {
    if (replayTruthDisabled()) return res.json(replayTruthStatus());
    try { res.json(await adsbSource.get(kind)); }
    catch (error) { res.status(503).json({error: error.message}); }
  });
}
app.get('/api/adsb/status', async (req, res) => {
  if (replayTruthDisabled()) return res.json(replayTruthStatus());
  res.json(await adsbSource.status());
});

// stash API
app.get('/stash/map', (req, res) => {
  res.send(stash_map.get_data_map());
});
app.get('/stash/detection', (req, res) => {
  res.send(stash_detection.get_data_detection());
});
app.get('/stash/iqdata', (req, res) => {
  res.send(stash_iqdata.get_data_iqdata());
});
app.get('/stash/timing', (req, res) => {
  res.send(stash_timing.get_data_timing());
});

// read state of capture
app.get('/capture', (req, res) => {
  res.send(capture);
});
app.get('/capture/request', (req, res) => res.json({recording: capture, revision: captureRevision}));
app.get('/capture/status', (req, res) => {
  const now = Date.now();
  const receiving = lastFrameAt !== null && now - lastFrameAt <
    Math.max(10000, (config.process.data.cpi || 1) * 3000);
  const processor = processorStatusFresh(processorStatus) ? processorStatus.value : null;
  const replay = processor?.input === 'replay' || config.capture.replay?.state === true;
  const acknowledgement = processor && processor.input === 'live' ? processor : null;
  const recording = acknowledgement?.recording === true;
  const recordingError = acknowledgement?.recordingError || null;
  const acknowledged = acknowledgement && acknowledgement.recordingRequestId === captureRevision &&
    acknowledgement.recording === capture;
  if (capture && recordingError && acknowledgement?.recordingRequestId === captureRevision) {
    capture = false;
    captureStartedAt = null;
  }
  res.json({
    supported: !replay,
    available: !replay && (capture ? acknowledgement !== null : receiving),
    reason: replay ? 'Recording is unavailable while replay is active.' : recordingError ||
      (!receiving && !capture ? 'Start live radar and wait for frames before recording.' : null),
    requested: capture,
    revision: captureRevision,
    acknowledged: acknowledged === true,
    recording,
    recordingFile: acknowledgement?.recordingFile || '',
    recordingError,
    recordedSamples: acknowledgement?.recordedSamples || 0,
    startedAt: recording ? captureStartedAt : null,
    elapsedSeconds: recording && captureStartedAt ?
      Math.max(0, Math.floor((now - captureStartedAt) / 1000)) : 0
  });
});
// toggle state of capture
app.get('/capture/toggle', (req, res) => {
  if (!configWriteOriginAllowed(req)) return res.status(403).json({error: 'Use this VectorWarp host to control recording.'});
  const processor = processorStatusFresh(processorStatus) ? processorStatus.value : null;
  if (processor?.input === 'replay' || config.capture.replay?.state === true)
    return res.status(409).json({error: 'Recording is unavailable while replay is active.'});
  if (!capture && (lastFrameAt === null || Date.now() - lastFrameAt >
      Math.max(10000, (config.process.data.cpi || 1) * 3000)))
    return res.status(409).json({error: 'No live radar frames. Start the radar before recording.'});
  capture = !capture;
  captureRevision += 1;
  captureStartedAt = capture ? Date.now() : null;
  res.json({requested: capture, revision: captureRevision, recording: processor?.recording === true,
    acknowledged: processor?.input === 'live' && processor.recordingRequestId === captureRevision && processor.recording === capture,
    startedAt: captureStartedAt});
});
app.use((error, req, res, next) => {
  if (error instanceof SyntaxError || error.type === 'entity.too.large')
    return res.status(error.type === 'entity.too.large' ? 413 : 400)
      .json({ok: false, errors: ['The configuration request is not valid JSON.']});
  next(error);
});
// When serving the UI directly, advertise this actual API port. A static web
// server can keep using ?apiPort=PORT or a same-origin reverse proxy.
app.use((req, res, next) => {
  if (req.method !== 'GET' || req.path.startsWith('/api/')) return next();
  const relative = req.path.endsWith('/') ? `${req.path}index.html` :
    path.extname(req.path) ? req.path : `${req.path}/index.html`;
  if (!relative.endsWith('.html')) return next();
  const root = path.resolve(__dirname, '..', 'html');
  const filename = path.resolve(root, `.${relative}`);
  if (!filename.startsWith(`${root}${path.sep}`) || !fs.existsSync(filename)) return next();
  const preview = process.env.BLAH2_PREVIEW === 'true' ?
    '<aside class="preview-notice">UI preview · No live radar · Saves affect only this temporary preview</aside>' : '';
  const html = fs.readFileSync(filename, 'utf8')
    .replace('<head>', `<head><script>window.BLAH2_API_PORT=${JSON.stringify(String(PORT))};</script>`)
    .replace(/<body([^>]*)>/, `<body$1>${preview}`);
  res.type('html').send(html);
});
app.use(express.static(path.join(__dirname, '..', 'html')));
app.use((error, req, res, next) => {
  console.error(`API request failed: ${error.message}`);
  res.status(500).json({ok: false, errors: ['The server could not complete this request. Check the VectorWarp API log.']});
});
app.listen(PORT, HOST, () => {
  ownedPorts.add(PORT);
  console.log(`Running on http://${HOST}:${PORT}`);
}).on('error', error => {
  console.error(`Settings API cannot listen on ${HOST}:${PORT}: ${error.code}. Choose a free API port or BLAH2_SETUP_PORT.`);
});

function listenData(server, name) {
  server.on('error', error => {
    const message = bindMessage(`${name} data port`, error.code);
    serviceErrors.set(name, message);
    console.error(message);
  });
  server.on('connection', socket => socket.on('error', error => {
    serviceErrors.set(name, `${name} connection: ${error.code}`);
  }));
  const port = config.network?.ports?.[name];
  if (!Number.isInteger(port) || port < 1 || port > 65535 || port === PORT) {
    serviceErrors.set(name, `${name} has an invalid or conflicting port. Correct it in Settings.`);
    return;
  }
  server.on('close', () => ownedPorts.delete(port));
  server.listen(port, HOST, () => {
    ownedPorts.add(port);
    serviceErrors.delete(name);
  });
}

// tcp listener map
const server_map = net.createServer((socket)=>{
    socket.on("data",(msg)=>{
        data_map = data_map + msg.toString();
        if (data_map.slice(-1) === "}")
        {
          map = data_map;
          stash_map.update_data(map);
          data_map = '';
        }
    });
    socket.on("close",()=>{
        console.log("Connection closed.");
    })
});
listenData(server_map, 'map');

// tcp listener detection
const server_detection = net.createServer((socket)=>{
  socket.on("data",(msg)=>{
      data_detection = data_detection + msg.toString();
      if (data_detection.slice(-1) === "}")
      {
        detection = data_detection;
        stash_detection.update_data(detection);
        data_detection = '';
      }
  });
  socket.on("close",()=>{
      console.log("Connection closed.");
  })
});
listenData(server_detection, 'detection');

// tcp listener tracker
const server_tracker = net.createServer((socket)=>{
  socket.on("data",(msg)=>{
      data_tracker = data_tracker + msg.toString();
      if (data_tracker.slice(-1) === "}")
      {
        track = data_tracker;
        data_tracker = '';
      }
  });
  socket.on("close",()=>{
      console.log("Connection closed.");
  })
});
listenData(server_tracker, 'track');

// tcp listener timestamp
const server_timestamp = net.createServer((socket)=>{
  const generation = invalidateTimestampTelemetry();
  timestampConnections += 1;
  socket.on("data",(msg)=>{
    if (!timestampConnectionCurrent(generation)) return;
    lastFrameAt = Date.now();
    data_timestamp = data_timestamp + msg.toString();
    timestamp = data_timestamp;
    data_timestamp = '';
  });
  socket.on("close",()=>{
      if (timestampConnectionCurrent(generation)) invalidateTimestampTelemetry();
      console.log("Connection closed.");
  })
});
listenData(server_timestamp, 'timestamp');

// tcp listener timing
const server_timing = net.createServer((socket)=>{
  const generation = invalidateTimingTelemetry();
  socket.on("data",(msg)=>{
    if (!timingConnectionCurrent(generation)) return;
    data_timing = data_timing + msg.toString();
    if (data_timing.slice(-1) === "}")
    {
      timing = data_timing;
      lastTimingAt = Date.now();
      stash_timing.update_data(timing);
      data_timing = '';
    }
  });
  socket.on("close",()=>{
      if (timingConnectionCurrent(generation)) invalidateTimingTelemetry();
      console.log("Connection closed.");
  })
});
listenData(server_timing, 'timing');

// tcp listener iqdata metadata
const server_iqdata = net.createServer((socket)=>{
  socket.on("data",(msg)=>{
    data_iqdata = data_iqdata + msg.toString();
    if (data_iqdata.slice(-1) === "}")
    {
      iqdata = data_iqdata;
      stash_iqdata.update_data(iqdata);
      data_iqdata = '';
    }
  });
  socket.on("close",()=>{
      console.log("Connection closed.");
  })
});
listenData(server_iqdata, 'iqdata');

process.on('SIGTERM', () => {
  console.log('SIGTERM signal received.');
  process.exit(0);
});
