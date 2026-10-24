const express = require('express');
const net = require("net");
const http = require('http');
const fs = require('fs');
const yaml = require('js-yaml');
const crypto = require('crypto');
const dns = require('dns');
const path = require('path');
const {execFile} = require('child_process');
const {restartCommandFromEnvironment, commandAvailable, launch: launchLifecycleRestart} = require('./lifecycle.js');
const {getDeviceProfiles, validateConfig, FIELD_RULES} = require('./config-manager.js');
const {readConfig, writable, saveConfig} = require('./config-store.js');
const {getUpstreamStatus} = require('./upstream-status.js');
const {installReceiverRoutes, trustedOrigins, sameReceiverOrigin} = require('./receiver-routes.js');
const {createReceiverSynchronizer, receiverAcceptanceBoundary} = require('./receiver-sync.js');
const {createAutomaticGain, enabled: automaticGainEnabled, MAX_AUTOMATIC_GAIN_MS} = require('./automatic-gain.js');
const {measureGain, waitForGainSettled} = require('./kraken-gain-probe.js');
const {createReceiverHelperClient} = require('./receiver-helper-client.js');
const gainRestartPreflight = createReceiverHelperClient();
const {createGainMonitor} = require('./gain-monitor.js');
const {createReceiverJournal} = require('./receiver-journal.js');
const gainRestart = require('./gain-restart.js');
const {invalidate: invalidateGeometry} = require('../html/js/kraken_geometry');
const {createAdsbSource} = require('./adsb-source.js');
const {createManifoldRecorder, siteFingerprint} = require('./manifold-recorder.js');
const {createManifoldAutomation} = require('./manifold-automation.js');
const {createBearingDiagnosticCache} = require('./bearing-diagnostic-cache.js');
const {createFrequencyResolvedShadow} = require('./frequency-resolved-shadow.js');
const {createTrackerForwarder} = require('./tracker-forward.js');
const {createTrackerPublication} = require('./tracker-publication.js');
const {createEvidenceStatistics} = require('./evidence-statistics.js');
const {createOprsEventRecorder} = require('./oprs-event-recorder.js');
const {verifyCaptureIntegrity, writeAdmissionSidecar, validateProcessorSampleRanges,
  captureArtifactState, admissionsNeedTruthSettlement} = require('./capture-admission.js');
const {createTrackGuidedShadowRecorder} = require('./track-guided-shadow-recorder.js');
const {createCaptureTriggerQueue, optionsFromEnvironment: captureTriggerOptions} =
  require('./capture-trigger-queue.js');
const {bandPlans: illuminatorBandPlans, receiverCapabilities: illuminatorReceiverCapabilities,
  evaluateFastSweep: evaluateFastIlluminatorSweep,
  evaluateSurvey: evaluateIlluminatorSurvey} = require('./illuminator-survey.js');
const {catalogSources: illuminatorCatalogSources} = require('./illuminator-catalog.js');
const {checkNetworkBindings, bindMessage} = require('./network-check.js');
const {status: validateProcessorStatus, fresh: processorStatusFresh} = require('./processor-status.js');
const {createGpuSetupStatus} = require('./gpu-setup.js');
const jsonFrames = require('./json-frames.js');
const {BistaticHypothesisTracker, locationsFromBearing} = require('./bistatic-geolocation.js');
const gpuSetupStatus = createGpuSetupStatus({preview: process.env.BLAH2_PREVIEW === 'true'});

// parse config file
const configFile = path.resolve(process.argv[2] || 'config/config.yml');
const startupDocument = readConfig(configFile);
var config = startupDocument.config;
const adsbSource = createAdsbSource(config, {preview: process.env.BLAH2_PREVIEW === 'true'});
const manifoldRecorder = createManifoldRecorder(config, adsbSource);
const manifoldAutomation = createManifoldAutomation(config, manifoldRecorder,
  {onQualified: promoteQualifiedManifold});
manifoldAutomation.start();
const bearingDiagnostic = createBearingDiagnosticCache(config);
bearingDiagnostic.start();
const frequencyResolvedShadow = createFrequencyResolvedShadow(config);
frequencyResolvedShadow.start();
const trackerForwarder = createTrackerForwarder(config, adsbSource);
const trackerPublication = createTrackerPublication(config);
// The native recorder copies roughly 96 MB for each four-second five-channel
// clip.  Space physical claims so a burst of simultaneously eligible aircraft
// coalesces instead of saturating the one-copy raw ring.
const captureTriggerQueue = createCaptureTriggerQueue({...captureTriggerOptions(process.env, config),
  minimumExposureIntervalMs: 5000});
const CLIP_PRE_ROLL_UNAVAILABLE = 'Kraken clip pre-roll is unavailable';
// A failed native request can reset the bounded raw ring.  Give it materially
// more than the configured two-second pre-roll before issuing the one retry.
const CLIP_PRE_ROLL_RETRY_DELAY_MS = 5000;
// Exact-CPI truth is intentionally delayed by four CPIs, and the final CPI may
// still need the next ADS-B observation.  This is bounded, but must cover both.
const CAPTURE_TRUTH_SETTLE_MS = 8000;
const CAPTURE_TRUTH_SETTLE_POLL_MS = 250;
const evidenceStatistics = createEvidenceStatistics(config, adsbSource,
  {onEligibilityEvent: event => captureTriggerQueue.acceptEligibilityEvent(event, Date.now()),
    onPeriodicCapture: (event, timestamp) => captureTriggerQueue.acceptPeriodicSnapshot(event, timestamp),
    onTrackCapture: (event, timestamp) => captureTriggerQueue.acceptTrackSnapshot(event, timestamp)});
// Keep useful live association visible for receivers that can preserve coherent
// sample position but cannot provide hardware UTC (currently Kraken/RTL2832).
// This lane is deliberately separate from the fail-closed decision-grade
// scorecard and has no capture side effects.
const provisionalEvidenceStatistics = createEvidenceStatistics(config, adsbSource, {
  filename: process.env.BLAH2_PROVISIONAL_STATISTICS_PATH ||
    '/var/lib/vectorwarp-api/evidence-statistics-provisional.json',
  timingPolicy: 'provisional'});
const oprsEventRecorder = createOprsEventRecorder(config,
  {filename: process.env.BLAH2_OPRS_EVENTS_PATH});

async function finalizeCaptureAdmission(revision, file, samples) {
  const records = captureTriggerQueue.eventsForRevision(revision);
  let admissions = evidenceStatistics.prepareCaptureAdmissions(records, revision);
  const truthDeadline = Date.now() + CAPTURE_TRUTH_SETTLE_MS;
  while (admissionsNeedTruthSettlement(admissions) && Date.now() < truthDeadline) {
    await new Promise(resolve => setTimeout(resolve, CAPTURE_TRUTH_SETTLE_POLL_MS));
    admissions = evidenceStatistics.prepareCaptureAdmissions(records, revision);
  }
  const first = records.flatMap(record => record.captureWindows || [])
    .find(window => window.revision === revision);
  let integrity;
  try {
    for (let attempt = 0; attempt < 5; ++attempt) {
      integrity = await verifyCaptureIntegrity(file, {revision, samples,
        windowStart: first?.windowStart, windowEnd: first?.windowEnd});
      if (integrity.valid || !(integrity.reasons || []).some(reason =>
        ['recording-sidecar-invalid', 'recording-size-mismatch'].includes(reason))) break;
      await new Promise(resolve => setTimeout(resolve, 250));
    }
  } catch (error) { integrity = {valid: false, reasons: ['recording-integrity-error'], error: error.message}; }
  const {metadata: integrityMetadata, ...integritySummary} = integrity;
  integrity = integritySummary;
  admissions = Object.fromEntries(Object.entries(admissions).map(([id, admission]) =>
    [id, validateProcessorSampleRanges(admission, integrityMetadata)]));
  let admissionSidecar = null;
  const artifact = {schema: 'vectorwarp-capture-admission-v1', created_at: Date.now(), revision,
    state: captureArtifactState(integrity, admissions), integrity,
    config_epoch: evidenceStatistics.configEpoch, receiver_epoch: evidenceStatistics.receiverEpoch,
    events: records.map(record => ({event_id: record.eventId, pass_id: record.passId,
      capture_ids: (record.captureWindows || []).filter(window => window.revision === revision)
        .map(window => window.captureId)})), captures: Object.fromEntries(Object.entries(admissions)
      .map(([id, truth]) => [id, {...truth,
        admission_state: truth.valid && integrity.valid ? 'admitted' : 'quarantined'}]))};
  if (file) try { admissionSidecar = await writeAdmissionSidecar(file, artifact); }
  catch (error) {
    integrity = {...integrity, valid: false,
      reasons: [...new Set([...(integrity.reasons || []), 'admission-sidecar-write'])], error: error.message};
  }
  integrity = {...integrity, admission_sidecar: admissionSidecar};
  captureTriggerQueue.setAdmission(revision, admissions, integrity);
  evidenceStatistics.recordCaptureAdmissions(Object.fromEntries(Object.entries(admissions).map(([id, truth]) =>
    [id, {valid: truth.valid && integrity.valid, reasons: [...new Set([...(truth.reasons || []), ...(integrity.reasons || [])])]}])));
  evidenceStatistics.acceptCaptureEvents(captureTriggerQueue.eventsForRevision(revision));
}
const trackGuidedShadowRecorder = createTrackGuidedShadowRecorder(config, adsbSource);
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
let timestampConnectionSequence = 0;
let timingConnectionSequence = 0;
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
async function measureRawAdcHeadroom(candidate) {
  const status = await getUpstreamStatus(candidate, {timeoutMs: 3000});
  if (!status.available || !status.actual?.adcHeadroom)
    throw Object.assign(new Error('Suite did not report pre-calibration ADC headroom.'),
      {code: 'AUTOMATIC_GAIN_RAW_ADC_UNAVAILABLE'});
  return status.actual.adcHeadroom;
}
const automaticGain = createAutomaticGain({synchronizer: receiverSynchronizer,
  measureGain, waitForGainSettled, measureRawHeadroom: measureRawAdcHeadroom});
// An interrupted gain check must not publish frames from an uncertain receiver.
// Successful checks also remain blocked until the normal API/processor restart.
let gainPublicationBlocked = gainRestart.publicationBlocked(receiverSyncState);
let gainRestartTimestampAt = null;
if (gainPublicationBlocked) void frequencyResolvedShadow.pause();
const RECEIVER_SYNC_HEADER = 'X-VectorWarp-Receiver-Sync';
const RECEIVER_SYNC_INTENT = 'synchronize-v1';
let gainMonitorStatus = {state: 'starting'};
const gainMonitor = createGainMonitor({
  getConfig: () => readConfig(configFile).config,
  getRevision: () => readConfig(configFile).revision,
  busy: () => configWriteInProgress || gainPublicationBlocked || capture ||
    !processorStatusFresh(processorStatus) ||
    startupDocument.revision !== readConfig(configFile).revision ||
    ['running', 'scheduled'].includes(restartState.state) ||
    frequencyResolvedShadow.status().active,
  measureGain,
  report: value => { gainMonitorStatus = value; },
  onOverload: ({config: candidate, revision}) => new Promise((resolve, reject) => {
    const body = JSON.stringify(candidate);
    // Reuse the exact same revision-guarded, journaled Apply path. Backoff never
    // raises gain. This request cannot bypass calibration or restart checks.
    const host = HOST === '0.0.0.0' ? '127.0.0.1' : HOST === '::' ? '::1' : HOST;
    const request = http.request({host, port: PORT, path: '/api/config?restart=true&gain=backoff',
      method: 'PUT', headers: {'Content-Type': 'application/json',
        Origin: `http://${net.isIP(host) === 6 ? `[${host}]` : host}:${PORT}`,
        'Content-Length': Buffer.byteLength(body), 'If-Match': `"${revision}"`,
        'X-VectorWarp-Intent': 'config-write-v1',
        [RECEIVER_SYNC_HEADER]: RECEIVER_SYNC_INTENT}}, response => {
      response.resume();
      response.on('end', () => response.statusCode === 200 ? resolve() :
        reject(new Error(`Automatic overload backoff returned HTTP ${response.statusCode}.`)));
    });
    request.setTimeout(MAX_AUTOMATIC_GAIN_MS + 15000, () =>
      request.destroy(new Error('Automatic overload backoff outcome is unknown.')));
    request.on('error', reject); request.end(body);
  })
});

function replayTruthDisabled() {
  return config.capture?.replay?.state === true ||
    (processorStatusFresh(processorStatus) && processorStatus.value.input === 'replay');
}
function replayTruthStatus() {
  return {enabled: false, online: false, disabledForReplay: true,
    message: 'Live ADS-B truth is not replayed.'};
}

let restartCommand = null;
try { restartCommand = restartCommandFromEnvironment(); }
catch (error) { console.error(`Invalid restart lifecycle configuration: ${error.message}`); }

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
const receiverOrigins = trustedOrigins(PORT, (process.env.BLAH2_RECEIVER_ORIGINS || '').split(',').filter(Boolean));
const CONFIG_INTENT = 'config-write-v1';
const RECORDING_INTENT = 'recording-toggle-v1';
var map = '';
var detection = '';
var track = '';
var bearing = '';
const bearingHypotheses = new BistaticHypothesisTracker();
var timestamp = '';
var timing = '';
var iqdata = '';
var capture = false;
var captureStartedAt = null;
let captureRevision = 0;

function invalidateTimestampTelemetry() {
  timestampGeneration += 1;
  activeTimestampConnection = 0;
  gainRestartTimestampAt = null;
  lastFrameAt = null;
  timestamp = '';
}

function invalidateTimingTelemetry() {
  timingGeneration += 1;
  activeTimingConnection = 0;
  lastTimingAt = null;
  timing = '';
}

function timestampConnectionEligible(connection) {
  return connection.restart === restartGeneration && connection.generation === timestampGeneration;
}

function timingConnectionEligible(connection) {
  return connection.restart === restartGeneration && connection.generation === timingGeneration;
}

function timestampConnectionCurrent(connection) {
  return timestampConnectionEligible(connection) && connection.id === activeTimestampConnection;
}

function timingConnectionCurrent(connection) {
  return timingConnectionEligible(connection) && connection.id === activeTimingConnection;
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
function completePendingGainRestart() {
  if (!gainPublicationBlocked || !gainRestart.pending(receiverSyncState)) return;
  try {
    const document = readConfig(configFile);
    const next = gainRestart.complete({state: receiverSyncState, config,
      revision: document.revision, loadedRevision: startupDocument.revision, serverId,
      restartReceipt: readSdrplayStartup(document.revision), processorStatus,
      timestampReceivedAt: gainRestartTimestampAt,
      timestampMaxAgeMs: Math.max(10000, (config.process?.data?.cpi || 1) * 3000)},
    value => receiverJournal.write(value));
    if (next) {
      receiverSyncState = next;
      gainPublicationBlocked = false;
      frequencyResolvedShadow.resume?.();
      serviceErrors.delete('gain-restart');
    }
  } catch (error) {
    serviceErrors.set('gain-restart', `Gain restart verification could not be saved: ${error.message}`);
  }
}
const {installSdrplayBuildRoutes, helperStatus} = require('./sdrplay-build');
const {installMacRspduoBuildRoutes, createMacRspduoBuild} = require('./macos-rspduo-build');
const macRspduoBuild = process.platform === 'darwin' ? createMacRspduoBuild() : null;
app.use(express.json({limit: '256kb', strict: true}));
function configWriteOriginAllowed(req) {
  return sameReceiverOrigin(req, receiverOrigins);
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
  if (origin && configWriteOriginAllowed(req)) {
    res.header('Access-Control-Allow-Origin', origin);
    res.header('Vary', 'Origin');
  }
  res.header('Access-Control-Allow-Methods', 'GET, POST, PUT, OPTIONS');
  res.header('Access-Control-Allow-Headers', `Content-Type, If-Match, ${RECEIVER_SYNC_HEADER}, X-VectorWarp-Intent`);
  res.header('Access-Control-Expose-Headers', 'ETag');
  res.header('Cache-Control', 'private, no-cache, no-store, must-revalidate');
  res.header('Expires', '-1');
  res.header('Pragma', 'no-cache');
  next();
});
app.options('*', (req, res) => {
  if (['/api/config', '/api/config/validate', '/capture/toggle'].includes(req.path) && !configWriteOriginAllowed(req))
    return res.status(403).json({ok: false,
      errors: ['Configuration changes must come from this VectorWarp host.']});
  res.sendStatus(204);
});
app.use((req, res, next) => {
  if (gainPublicationBlocked && ['/api/map', '/api/detection', '/api/tracker',
    '/api/tracker/native', '/api/bearing', '/api/locations', '/api/iqdata'].includes(req.path))
    return res.status(503).json({available: false, data: [],
      reason: 'Receiver gain is being checked or a verified restart is pending.'});
  next();
});

function configFileWritable() {
  return writable(configFile);
}

function restartCommandAvailable() {
  return commandAvailable(restartCommand);
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
  if (process.env.BLAH2_PREVIEW === 'true' || (!macRspduoBuild && !localRspEnrolled())) return false;
  const status = macRspduoBuild ? macRspduoBuild.status() : await helperStatus(localSdrplayHelper);
  return status?.ok === true && status.state === 'current';
}

function launchRestart() {
  restartGeneration += 1;
  invalidateTimestampTelemetry();
  invalidateTimingTelemetry();
  restartState = {...restartState, state: 'running', startedAt: Date.now(),
    timestampConnections};
  launchLifecycleRestart(restartCommand, {onComplete(result) {
    restartState = {...restartState, state: result.ok ? 'command-complete' : 'failed',
      message: result.ok ? 'Restart request accepted. Waiting for a new radar connection.' :
        result.timeout ? 'Restart request outcome is unknown after 30 seconds. Check the service manager before retrying.' :
        result.error ? `Restart could not start: ${result.error.message}` :
        `Restart request failed (${result.signal || `exit ${result.code}`}). ${result.diagnostic || 'Check the local service manager.'}`};
  }});
}

async function promoteQualifiedManifold(candidate) {
  if (candidate.fingerprint !== manifoldRecorder.fingerprint)
    return {active: false, pending: false, reason: 'The qualified model belongs to an earlier site configuration.'};
  if (config.process?.bearing?.manifold_file === candidate.path)
    return {active: true, pending: false, reason: 'The qualified model is active.'};
  if (gainPublicationBlocked || configWriteInProgress || restartState.state === 'running' || restartState.state === 'scheduled')
    return {active: false, pending: true, reason: 'Waiting for the current settings operation to finish.'};
  const current = readConfig(configFile);
  if (!current.exists || !writable(configFile))
    return {active: false, pending: true, reason: 'The qualified model is ready, but the configuration is read-only.'};
  if (siteFingerprint(current.config) !== candidate.fingerprint)
    return {active: false, pending: false, reason: 'Site settings changed during qualification; collecting a new model.'};
  if (current.config.process?.bearing?.manifold_file === candidate.path)
    return {active: false, pending: true,
      reason: 'Qualified model is selected and will activate at the next VectorWarp restart.'};
  const updated = JSON.parse(JSON.stringify(current.config));
  updated.process.bearing.manifold_file = candidate.path;
  const saved = saveConfig(configFile, updated, current.revision);
  if (!restartCommandAvailable())
    return {active: false, pending: true, revision: saved.revision,
      reason: 'Qualified model saved. It will activate at the next VectorWarp restart.'};
  restartState = {state: 'scheduled', requestedAt: Date.now(), revision: saved.revision,
    reason: 'automatic-manifold-promotion'};
  setTimeout(launchRestart, 150).unref();
  return {active: false, pending: true, revision: saved.revision,
    reason: 'Qualified model saved; a controlled restart was requested.'};
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
  res.send(trackerPublication.frame());
});
app.get('/api/tracker/native', (req, res) => {
  res.send(trackerPublication.nativeOnly());
});
app.get('/api/tracker/publication', (req, res) => {
  res.json(trackerPublication.status());
});
app.get('/api/statistics', (req, res) => {
  const diagnostic = bearingDiagnostic.status();
  const recorder = manifoldRecorder.status();
  const automation = manifoldAutomation.status();
  res.json(evidenceStatistics.status({enabled: config.process?.bearing?.enable === true,
    calibrated: Boolean(config.process?.bearing?.manifold_file),
    manifold_file: config.process?.bearing?.manifold_file || null,
    diagnostic, recorder, automation,
    frequency_resolved: frequencyResolvedShadow.status()}));
});
app.get('/api/live-scorecard', (req, res) => {
  res.json(evidenceStatistics.liveStatus());
});
app.get('/api/live-scorecard/provisional', (req, res) => {
  res.json(provisionalEvidenceStatistics.liveStatus());
});
app.get('/api/tracker-forward', (req, res) => {
  res.json(trackerForwarder.status());
});
app.get('/api/tracker/guided-shadow', (req, res) => {
  res.json(trackGuidedShadowRecorder.status());
});
app.get('/api/bearing', (req, res) => {
  if (config.process?.bearing?.enable !== true)
    return res.json({enabled: false, reason: 'Bearing processing is disabled.', data: []});
  if (!bearing) return res.status(503).json({enabled: true, available: false,
    reason: 'No bearing frame has arrived.', data: []});
  res.type('json').send(bearing);
});
app.get('/api/locations', (req, res) => {
  let frame = null;
  try { frame = bearing ? JSON.parse(bearing) : null; }
  catch (_) { return res.status(503).json({enabled: true, available: false,
    reason: 'The current bearing frame is invalid.', data: []}); }
  const result = locationsFromBearing(frame, config, bearingHypotheses);
  res.status(result.enabled && !result.available ? 503 : 200).json(result);
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
app.get('/api/receiver/gain', (req, res) => res.json({
  policy: config.capture?.device?.heimdall?.gain_control || 'fixed',
  selectedGainDb: config.capture?.device?.heimdall?.gain,
  pendingRestart: gainPublicationBlocked,
  selection: receiverSyncState.receipt?.automaticGain || null,
  monitor: gainMonitorStatus
}));
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
      measuredGainSupported: gainRestart.installedGainRestartSupported(restartCommand),
      measuredGainMaximumMs: MAX_AUTOMATIC_GAIN_MS,
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
app.get('/api/illuminator-survey', (req, res) => {
  const document = readConfig(configFile);
  res.json({schema: 'vectorwarp-illuminator-survey-capabilities-v1',
    capabilities: illuminatorReceiverCapabilities(document.config),
    band_plans: illuminatorBandPlans(), catalog_sources: illuminatorCatalogSources(),
    catalog_schema: 'vectorwarp-illuminator-transmitter-catalog-v1',
    catalog_runtime: {live_network_fetch: false, pinned_snapshot_required: true}});
});
app.post('/api/illuminator-survey/evaluate', (req, res) => {
  try {
    const document = readConfig(configFile);
    res.json(evaluateIlluminatorSurvey(req.body?.candidates, document.config,
      req.body?.limits));
  } catch (error) {
    res.status(422).json({ok: false, errors: [error.message]});
  }
});
app.post('/api/illuminator-survey/fast-evaluate', (req, res) => {
  try {
    const document = readConfig(configFile);
    res.json(evaluateFastIlluminatorSweep(req.body?.candidates, document.config,
      req.body?.limits));
  } catch (error) {
    res.status(422).json({ok: false, errors: [error.message]});
  }
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
    gainCheckPending: gainPublicationBlocked,
    radar: gainPublicationBlocked || lastFrameAt === null ? 'no-data' : Date.now() - lastFrameAt <
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
  if (!checked.valid) {
    // A supplied health sample supersedes previous overload claims, even when
    // malformed. Do not display the earlier healthy reading as current.
    if (req.body?.receiverHealth !== undefined) processorStatus = null;
    return res.status(422).json({ok: false, errors: checked.errors});
  }
  processorStatus = {value: checked.value, receivedAt: Date.now()};
  completePendingGainRestart();
  if (checked.value.input === 'replay') adsbSource.clear();
  if (checked.value.input === 'live' && checked.value.recordingError &&
      checked.value.recordingRequestId === captureRevision) {
    capture = false;
    captureStartedAt = null;
  }
  if (checked.value.input === 'live' && checked.value.clipRevision !== null &&
      captureTriggerQueue.acknowledge(checked.value.clipRevision, checked.value.clipError,
        {file: checked.value.clipFile, samples: checked.value.clipSamples})) {
    const revision = checked.value.clipRevision;
    if (checked.value.clipError === CLIP_PRE_ROLL_UNAVAILABLE) {
      setTimeout(() => {
        const retry = captureTriggerQueue.retryPreRollFailure(revision);
        if (!retry)
          evidenceStatistics.acceptCaptureEvents(
            captureTriggerQueue.eventsForRevision(revision));
      }, CLIP_PRE_ROLL_RETRY_DELAY_MS).unref();
    } else if (checked.value.clipError) evidenceStatistics.acceptCaptureEvents(
      captureTriggerQueue.eventsForRevision(revision));
    else void finalizeCaptureAdmission(revision, checked.value.clipFile, checked.value.clipSamples)
      .catch(error => console.error(`Capture admission failed: ${error.message}`));
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
const receiverManagement = installReceiverRoutes(app, {
  readDocument: () => readConfig(configFile),
  port: PORT,
  allowedOrigins: receiverOrigins,
  helperExecutable: process.env.BLAH2_RECEIVER_HELPER,
  extraOrigins: (process.env.BLAH2_RECEIVER_ORIGINS || '').split(',').filter(Boolean),
  transactionBusy: () => configWriteInProgress || macRspduoBuild?.busy() || ['running', 'scheduled'].includes(restartState.state) ||
    readSdrplayStartup(null)?.inProgress === true,
  processorStatus: () => processorStatusFresh(processorStatus) ? processorStatus.value : null,
  preview: process.env.BLAH2_PREVIEW === 'true',
  compiledLiveTypes: process.env.BLAH2_RECEIVER_TYPES ?
    process.env.BLAH2_RECEIVER_TYPES.split(',').map(value => value.trim()).filter(Boolean) : null
});
if (macRspduoBuild) installMacRspduoBuildRoutes(app, {allowedOrigins: receiverOrigins, build: macRspduoBuild,
  transactionBusy: () => configWriteInProgress || receiverManagement.busy() ||
    ['running', 'scheduled'].includes(restartState.state) || readSdrplayStartup(null)?.inProgress === true});
else installSdrplayBuildRoutes(app, {allowedOrigins: receiverOrigins,
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
function localKrakenProfile(value) {
  return value?.capture?.device?.type === 'Kraken' && value.capture?.replay?.state !== true &&
    value.capture?.device?.heimdall?.host === '127.0.0.1';
}
function krakenEndpoint(value) {
  const heimdall = value?.capture?.device?.heimdall || {};
  return [heimdall.host, heimdall.port, heimdall.control_port ?? 8092].join(':');
}
async function runLocalKrakenLauncher(action, environment = process.env, timeout = 70000) {
  const launcher = process.env.VECTORWARP_MACOS_LAUNCHER;
  await new Promise((resolve, reject) => execFile(launcher, [action], {
    cwd: '/', timeout, maxBuffer: 65536, env: environment
  }, error => error ? reject(error) : resolve()));
}
async function ensureLocalKrakenController(candidate, current) {
  if (process.platform !== 'darwin' || process.env.VECTORWARP_MACOS_LIFECYCLE !== '1') return;
  const requestedKraken = candidate?.capture?.device?.type === 'Kraken' && candidate.capture?.replay?.state !== true;
  if (requestedKraken && candidate.capture.device.heimdall?.host !== '127.0.0.1') {
    const error = new Error('macOS local Kraken capture requires the 127.0.0.1 Heimdall host.');
    error.code = 'LOCAL_KRAKEN_REMOTE_ENDPOINT'; error.status = 422; throw error;
  }
  if (!localKrakenProfile(candidate)) return;
  const launcher = process.env.VECTORWARP_MACOS_LAUNCHER;
  if (typeof launcher !== 'string' || !path.isAbsolute(launcher)) {
    const error = new Error('The local Kraken launcher is unavailable. Start VectorWarp through vectorwarp-macos.');
    error.code = 'LOCAL_KRAKEN_LAUNCHER_UNAVAILABLE'; error.status = 503; throw error;
  }
  const environment = {...process.env};
  let candidateFile = null;
  // Switching from another receiver needs a controller profile before the
  // existing synchronizer can send its first bounded control transaction.
  // This private candidate is not the saved YAML and survives only while its
  // exact controller record needs it for an owned stop/restart.
  const replaceOwnedController = !localKrakenProfile(current) || krakenEndpoint(candidate) !== krakenEndpoint(current);
  if (replaceOwnedController) {
    const state = process.env.VECTORWARP_MACOS_STATE;
    if (typeof state !== 'string' || !path.isAbsolute(state)) {
      const error = new Error('The local Kraken state directory is unavailable.');
      error.code = 'LOCAL_KRAKEN_STATE_UNAVAILABLE'; error.status = 503; throw error;
    }
    const directory = path.join(state, 'kraken-controller', 'candidates');
    fs.mkdirSync(directory, {recursive: true, mode: 0o700});
    candidateFile = path.join(directory, `${crypto.randomBytes(24).toString('hex')}.yml`);
    fs.writeFileSync(candidateFile, yaml.dump(candidate), {encoding: 'utf8', mode: 0o600, flag: 'wx'});
    environment.VECTORWARP_MACOS_KRAKEN_CONFIG = candidateFile;
  }
  try {
    if (replaceOwnedController && localKrakenProfile(current))
      await runLocalKrakenLauncher('stop-kraken', process.env, 20000);
    await runLocalKrakenLauncher('start-kraken', environment);
  } catch (error) {
    if (candidateFile) {
      try { await runLocalKrakenLauncher('stop-kraken', process.env, 20000); }
      catch (_) { /* launcher reports its own owned-record validation */ }
    }
    if (candidateFile) fs.rmSync(candidateFile, {force: true});
    const failure = new Error('The local Kraken controller did not become ready. Check Receiver setup and the local launcher log.');
    failure.code = 'LOCAL_KRAKEN_START_FAILED'; failure.status = 503;
    throw failure;
  }
  return {candidateFile};
}
async function discardLocalKrakenCandidate(controller) {
  if (!controller?.candidateFile) return;
  try {
    await runLocalKrakenLauncher('stop-kraken', process.env, 20000);
  } catch (error) {
    console.error(`Unable to stop failed local Kraken candidate: ${error.message}`);
  } finally {
    fs.rmSync(controller.candidateFile, {force: true});
  }
}
app.put('/api/config', async (req, res) => {
  if (!configWriteOriginAllowed(req) || req.get('X-VectorWarp-Intent') !== CONFIG_INTENT)
    return res.status(403).json({ok: false,
      errors: ['Configuration changes require a trusted VectorWarp origin and write intent.']});
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
  // A failed final journal write can leave both the restart marker and an
  // unresolved receiver outcome. Allow explicit Apply to reconcile that case;
  // publication remains blocked until a successful transaction and restart.
  if (gainRestart.pending(receiverSyncState) && !receiverReconciliationRequired)
    return res.status(409).json({ok: false, code: 'GAIN_CHECK_RESTART_PENDING',
      errors: ['The verified gain is awaiting a confirmed processor and API restart. Complete that restart before changing settings.']});
  if (configWriteInProgress || receiverManagement.busy() || macRspduoBuild?.busy())
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
  const measuredGain = !saveLater && automaticGainEnabled(req.body);
  if (measuredGain && !gainRestart.installedGainRestartSupported(restartCommand))
    return res.status(409).json({ok: false, code: 'GAIN_CHECK_RESTART_UNSUPPORTED',
      errors: ['Automatic gain requires the installed Linux receiver restart helper and its verified completion receipt.']});
  if (measuredGain && !wantsRestart)
    return res.status(409).json({ok: false, code: 'GAIN_CHECK_RESTART_REQUIRED',
      errors: ['Automatic gain requires Apply and restart so processing resumes with the verified settings. Use fixed gain for a controlled measurement block.']});
  if (measuredGain && (capture || processorStatus?.value?.recording === true))
    return res.status(409).json({ok: false, code: 'GAIN_CHECK_RECORDING_ACTIVE',
      errors: ['Finish the current recording before applying automatic gain.']});
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
  let localKrakenController = null;
  let gainTransaction = null;
  const previousReceiverReceipt = receiverSyncState.receipt || null;
  try {
    // Hold the write flag across this bounded read-only query. A stale local
    // adapter cannot race another Apply into a save or restart.
    if (localRspRequested && !await localRspCurrent())
      return res.status(422).json({ok: false, code: 'SDRPLAY_LOCAL_BUILD_REQUIRED',
        errors: ['Build SDRplay support in Settings after installing the SDRplay API and headers, then apply live RSPduo settings.']});
    if (measuredGain) {
      // Check the actual installed broker/policy/manifest before any journal,
      // hardware or config mutation. A matching helper command alone is not
      // proof that this installation can accept its managed restart.
      let readiness;
      try { readiness = await gainRestartPreflight({verb: 'discover'}); }
      catch (error) { readiness = {ok: false, code: error.code}; }
      if (readiness?.ok !== true)
        return res.status(503).json({ok: false, code: 'GAIN_CHECK_RESTART_UNAVAILABLE',
          cause: readiness?.code || 'MANAGEMENT_UNAVAILABLE',
          errors: ['Automatic gain cannot start because the installed receiver restart helper is unavailable or its release metadata is invalid. Repair the installed release and retry; no receiver setting was changed.']});
    }
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
      automaticGain: measuredGain,
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
    localKrakenController = await ensureLocalKrakenController(req.body, currentDocument.config);
    if (measuredGain) {
      gainPublicationBlocked = true;
      invalidateTimestampTelemetry();
      await frequencyResolvedShadow.pause();
      gainTransaction = await automaticGain.apply(currentDocument.config, req.body,
        {increase: req.query.gain !== 'backoff'});
      req.body = gainTransaction.config;
      receiverSync = gainTransaction.receiverSync;
    } else {
      receiverSync = await receiverSynchronizer.synchronize(currentDocument.config, req.body,
        {force: observeLiveReceiver});
    }
    if (receiverSync.operations?.some(operation => operation.commandSent &&
        (!measuredGain || !['set_gain', 'reset_lag_compensation'].includes(operation.operation))))
      invalidateGeometry(req.body?.capture?.device?.array_geometry);
    let saved;
    try {
      saved = saveConfig(configFile, req.body, currentDocument.revision);
    } catch (error) {
      if (gainTransaction) {
        // A save can fail after rename. Restore hardware only when the file is
        // still exactly the starting document; never overwrite a concurrent edit.
        if (readConfig(configFile).revision === currentDocument.revision) {
          try { receiverSync = await gainTransaction.rollback(); }
          catch (rollbackError) { receiverSync = rollbackError.receiverSync; }
        }
        error.receiverSync = receiverSync;
      }
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
    if (measuredGain) receiverSync = {...receiverSync, automaticGain: {
      ...receiverSync.automaticGain, restartPendingAt: Date.now(), restartOriginServerId: serverId}};
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
    await discardLocalKrakenCandidate(localKrakenController);
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
app.get('/api/bearing/manifold-recorder', (req, res) => {
  res.json(manifoldRecorder.status());
});
app.get('/api/bearing/manifold-automation', (req, res) => {
  res.json(manifoldAutomation.status());
});
app.get('/api/bearing/manifold-progress', (req, res) => {
  // Evaluation is signature-cached and runs off the API thread. Polling this
  // view refreshes a changed corpus promptly without loading the radar
  // processor or repeatedly fitting unchanged data.
  void manifoldAutomation.evaluate();
  const automation = manifoldAutomation.status();
  res.json({schema: 'vectorwarp-manifold-campaign-status-v1', generated_at: Date.now(),
    frequency_hz: config.capture?.fc || null,
    active_model: config.process?.bearing?.manifold_file || null,
    recorder: manifoldRecorder.status(),
    automation: {enabled: automation.enabled, state: automation.state,
      running: automation.running, qualified: automation.qualified,
      failures: automation.failures || [], error: automation.error,
      last_evaluation_at: automation.lastEvaluationAt || null,
      promotion: automation.promotion || null},
    progress: automation.progress,
    frequency_resolved: frequencyResolvedShadow.status()});
});
app.get('/api/bearing/diagnostic', (req, res) => {
  if (req.query.refresh === 'true') void bearingDiagnostic.refresh({force: true});
  res.json(bearingDiagnostic.status());
});
app.get('/api/bearing/frequency-resolved-shadow', (req, res) => {
  res.json(frequencyResolvedShadow.status());
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
app.get('/capture/request', (req, res) => res.json({recording: capture,
  revision: captureRevision, clip: captureTriggerQueue.request(true)}));
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
    clip: captureTriggerQueue.status(),
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
app.post('/capture/toggle', (req, res) => {
  if (!configWriteOriginAllowed(req) || req.get('X-VectorWarp-Intent') !== RECORDING_INTENT)
    return res.status(403).json({error: 'Recording changes require a trusted VectorWarp origin and intent.'});
  if (gainPublicationBlocked)
    return res.status(409).json({error: 'Wait for the gain check and processing restart before recording.'});
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

function jsonStreamServer(name, onFrame) {
  return net.createServer(socket => {
    const feed = jsonFrames(frame => {
      if (gainPublicationBlocked) return;
      onFrame(frame);
      // A new complete frame proves this listener and connection recovered.
      serviceErrors.delete(name);
    });
    socket.on('data', chunk => {
      try { feed(chunk); }
      catch (error) {
        serviceErrors.set(name, `${name} connection: ${error.message}`);
        console.error(`${name} connection rejected: ${error.message}`);
        socket.destroy();
      }
    });
    socket.on('close', () => console.log('Connection closed.'));
  });
}

// Each JSON payload stream has per-connection framing. Timestamp stays plain.
const server_map = jsonStreamServer('map', frame => {
  map = frame;
  stash_map.update_data(map);
});
listenData(server_map, 'map');

const server_detection = jsonStreamServer('detection', frame => {
  detection = frame;
  stash_detection.update_data(detection);
  trackerForwarder.accept(frame);
  trackGuidedShadowRecorder.accept(frame);
  trackerPublication.acceptInput(frame, trackerForwarder.status().connected);
  evidenceStatistics.acceptDetection(frame);
  provisionalEvidenceStatistics.acceptDetection(frame);
  oprsEventRecorder.acceptDetection(frame);
});
listenData(server_detection, 'detection');

const server_tracker = jsonStreamServer('track', frame => {
  track = frame;
  trackerPublication.acceptNative(frame);
  const published = trackerPublication.frame();
  evidenceStatistics.acceptTracker(published);
  provisionalEvidenceStatistics.acceptTracker(published);
  oprsEventRecorder.acceptTracker(published);
});
listenData(server_tracker, 'track');

if (config.process?.bearing?.enable === true && Number.isInteger(config.network?.ports?.bearing)) {
  const server_bearing = jsonStreamServer('bearing', frame => {
    bearing = frame;
    evidenceStatistics.acceptBearing(frame);
  });
  listenData(server_bearing, 'bearing');
}

if (manifoldRecorder.status().enabled &&
    Number.isInteger(config.network?.ports?.manifold)) {
  const server_manifold = jsonStreamServer('manifold', frame => manifoldRecorder.accept(frame));
  listenData(server_manifold, 'manifold');
}

// tcp listener timestamp
const server_timestamp = net.createServer((socket)=>{
  const connection = {restart: restartGeneration, generation: timestampGeneration,
    id: ++timestampConnectionSequence};
  timestampConnections += 1;
  socket.on("data",(msg)=>{
    const value = msg.toString();
    if (!/^\s*\d{10,}(?:\.\d+)?\s*$/.test(value)) {
      serviceErrors.set('timestamp', 'timestamp connection: Expected numeric radar timestamp');
      socket.destroy();
      return;
    }
    if (!timestampConnectionEligible(connection)) return;
    activeTimestampConnection = connection.id;
    serviceErrors.delete('timestamp');
    gainRestartTimestampAt = Date.now();
    completePendingGainRestart();
    if (gainPublicationBlocked) return;
    lastFrameAt = Date.now();
    timestamp = value;
  });
  socket.on("close",()=>{
      if (timestampConnectionCurrent(connection)) {
        activeTimestampConnection = 0;
        gainRestartTimestampAt = null;
        lastFrameAt = null;
        timestamp = '';
      }
      console.log("Connection closed.");
  })
});
listenData(server_timestamp, 'timestamp');

const server_timing = net.createServer(socket => {
  const connection = {restart: restartGeneration, generation: timingGeneration,
    id: ++timingConnectionSequence};
  const feed = jsonFrames(frame => {
    if (!timingConnectionEligible(connection)) return;
    activeTimingConnection = connection.id;
    timing = frame;
    oprsEventRecorder.acceptTiming(frame);
    evidenceStatistics.acceptTiming(frame);
    provisionalEvidenceStatistics.acceptTiming(frame);
    lastTimingAt = Date.now();
    stash_timing.update_data(timing);
    serviceErrors.delete('timing');
  });
  socket.on('data', chunk => {
    try { feed(chunk); }
    catch (error) {
      serviceErrors.set('timing', `timing connection: ${error.message}`);
      console.error(`timing connection rejected: ${error.message}`);
      socket.destroy();
    }
  });
  socket.on('close', () => {
    if (timingConnectionCurrent(connection)) {
      activeTimingConnection = 0;
      lastTimingAt = null;
      timing = '';
    }
    console.log('Connection closed.');
  });
});
listenData(server_timing, 'timing');

const server_iqdata = jsonStreamServer('iqdata', frame => {
  iqdata = frame;
  stash_iqdata.update_data(iqdata);
});
listenData(server_iqdata, 'iqdata');
gainMonitor.start();

process.on('SIGTERM', () => {
  console.log('SIGTERM signal received.');
  trackerForwarder.close();
  trackerPublication.close();
  evidenceStatistics.close();
  provisionalEvidenceStatistics.close();
  oprsEventRecorder.close();
  bearingDiagnostic.close();
  gainMonitor.stop();
  process.exit(0);
});
