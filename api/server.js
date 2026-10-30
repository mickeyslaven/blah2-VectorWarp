const express = require('express');
const net = require("net");
const http = require('http');
const fs = require('fs');
const yaml = require('js-yaml');
const crypto = require('crypto');
const dns = require('dns');
const path = require('path');
const os = require('os');
const zlib = require('zlib');
const {pipeline} = require('stream/promises');
const {buildMetadata} = require('./build-metadata');
const {execFile} = require('child_process');
const {restartCommandFromEnvironment, commandAvailable, launch: launchLifecycleRestart} = require('./lifecycle.js');
const {getDeviceProfiles, validateConfig, FIELD_RULES} = require('./config-manager.js');
const {readConfig, writable, saveConfig, migrateKnownDefaults} = require('./config-store.js');
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
const {createAircraftPhotoLookup} = require('./aircraft-photo.js');
const {createManifoldRecorder, siteFingerprint} = require('./manifold-recorder.js');
const {createManifoldAutomation} = require('./manifold-automation.js');
const {DetectionManifoldAutomation} = require('./detection-manifold-automation.js');
const {buildArrayCalibrationStatus} = require('./array-calibration-status.js');
const {createBearingDiagnosticCache} = require('./bearing-diagnostic-cache.js');
const {createFrequencyResolvedShadow} = require('./frequency-resolved-shadow.js');
const {createTrackerForwarder} = require('./tracker-forward.js');
const {createTrackerPublication} = require('./tracker-publication.js');
const {createEvidenceStatisticsLanes} = require('./evidence-statistics.js');
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
const {RecordingLibrary} = require('./recording-library.js');
const {RecordingTimeline} = require('./recording-timeline.js');
const {checkNetworkBindings, bindMessage} = require('./network-check.js');
const {installMapImageryRoutes} = require('./map-imagery.js');
const {status: validateProcessorStatus, fresh: processorStatusFresh} = require('./processor-status.js');
const {createGpuSetupStatus} = require('./gpu-setup.js');
const jsonFrames = require('./json-frames.js');
const {PresentationHistory, frameTimestamp} = require('./presentation-history.js');
const {BistaticHypothesisTracker, locationsFromBearing} = require('./bistatic-geolocation.js');
const gpuSetupStatus = createGpuSetupStatus({preview: process.env.BLAH2_PREVIEW === 'true'});

// parse config file
const configFile = path.resolve(process.argv[2] || 'config/config.yml');
try {
  if (migrateKnownDefaults(configFile))
    console.error('Migrated the invalid v0.1.28 USRP selector localhost to type=b200; the previous file is in config.yml.bak.');
} catch (error) {
  console.error(`Could not migrate the invalid v0.1.28 USRP selector: ${error.message}`);
}
const startupDocument = readConfig(configFile);
var config = startupDocument.config;
const adsbSource = createAdsbSource(config, {preview: process.env.BLAH2_PREVIEW === 'true'});
const aircraftPhotoLookup = createAircraftPhotoLookup(config, {cacheFile:
  process.env.BLAH2_AIRCRAFT_PHOTO_CACHE || '/var/lib/vectorwarp-api/aircraft-photo-metadata.json'});
const manifoldRecorder = createManifoldRecorder(config, adsbSource);
const manifoldAutomation = createManifoldAutomation(config, manifoldRecorder,
  {onQualified: promoteQualifiedManifold});
manifoldAutomation.start();
const detectionManifoldAutomation = new DetectionManifoldAutomation(config);
detectionManifoldAutomation.start();
const bearingDiagnostic = createBearingDiagnosticCache(config);
bearingDiagnostic.start();
const frequencyResolvedShadow = createFrequencyResolvedShadow(config);
frequencyResolvedShadow.start();
const trackerForwarder = createTrackerForwarder(config, adsbSource);
const trackerPublication = createTrackerPublication(config);
// Receiver-independent policy comes from Settings. A burst of simultaneously
// eligible aircraft still coalesces into one bounded physical recording.
const captureTriggerQueue = createCaptureTriggerQueue(captureTriggerOptions(process.env, config));
const recordingLibrary = new RecordingLibrary(() => config);
const recordingTimeline = new RecordingTimeline();
function recordingReplayProfile() {
  const device = config.capture?.device || {};
  return {schema: 'vectorwarp-replay-profile-v1',
    capture: {sample_rate_hz: config.capture?.fs || null,
      frequency_hz: config.capture?.fc || null,
      channel_count: device.channel_count || (device.type === 'Kraken' ? null : 2),
      reference_channel: device.reference_channel ?? 0,
      surveillance_channels: Array.isArray(device.surveillance_channels) ?
        [...device.surveillance_channels] : [1],
      source_receiver: device.type || null,
      array_geometry: device.array_geometry || null},
    process: {reference_synthesis: config.process?.reference_synthesis || null,
      surveillance_fusion: config.process?.surveillance_fusion || null,
      bearing: config.process?.bearing || null},
    location: config.location || null};
}
const CLIP_PRE_ROLL_UNAVAILABLE = 'Kraken clip pre-roll is unavailable';
// A failed native request can reset the bounded raw ring.  Give it materially
// more than the configured two-second pre-roll before issuing the one retry.
const CLIP_PRE_ROLL_RETRY_DELAY_MS = 5000;
// Exact-CPI truth is intentionally delayed by four CPIs, and the final CPI may
// still need the next ADS-B observation.  This is bounded, but must cover both.
const CAPTURE_TRUTH_SETTLE_MS = 8000;
const CAPTURE_TRUTH_SETTLE_POLL_MS = 250;
const {exact: evidenceStatistics, operational: provisionalEvidenceStatistics} =
  createEvidenceStatisticsLanes(config, adsbSource, {operational: {
    filename: process.env.BLAH2_PROVISIONAL_STATISTICS_PATH ||
      '/var/lib/vectorwarp-api/evidence-statistics-provisional.json',
    onEligibilityEvent: event => recordingLibrary.canRecord().allowed ?
      captureTriggerQueue.acceptEligibilityEvent(event, Date.now()) :
      ({accepted: false, reason: 'recording-storage-floor'}),
    onPeriodicCapture: (event, timestamp) => recordingLibrary.canRecord().allowed ?
      captureTriggerQueue.acceptPeriodicSnapshot(event, timestamp) :
      ({accepted: false, reason: 'recording-storage-floor'}),
    onTrackCapture: (event, timestamp) => recordingLibrary.canRecord().allowed ?
      captureTriggerQueue.acceptTrackSnapshot(event, timestamp) :
      ({accepted: false, reason: 'recording-storage-floor'})}});
const captureEvidenceStatistics = provisionalEvidenceStatistics;
const oprsEventRecorder = createOprsEventRecorder(config,
  {filename: process.env.BLAH2_OPRS_EVENTS_PATH});

async function finalizeCaptureAdmission(revision, file, samples) {
  const records = captureTriggerQueue.eventsForRevision(revision);
  let admissions = captureEvidenceStatistics.prepareCaptureAdmissions(records, revision);
  const truthDeadline = Date.now() + CAPTURE_TRUTH_SETTLE_MS;
  while (admissionsNeedTruthSettlement(admissions) && Date.now() < truthDeadline) {
    await new Promise(resolve => setTimeout(resolve, CAPTURE_TRUTH_SETTLE_POLL_MS));
    admissions = captureEvidenceStatistics.prepareCaptureAdmissions(records, revision);
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
    config_epoch: captureEvidenceStatistics.configEpoch, receiver_epoch: captureEvidenceStatistics.receiverEpoch,
    events: records.map(record => ({event_id: record.eventId, pass_id: record.passId,
      icao: record.hex || null, flight: record.flight || null,
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
  if (file && first) try {
    await recordingTimeline.write(file, first.windowStart, first.windowEnd,
      {sample_rate_hz: integrityMetadata?.sampleRate || config.capture?.fs || null,
        frequency_hz: integrityMetadata?.frequency || config.capture?.fc || null,
        channels: integrityMetadata?.channels || config.capture?.device?.channel_count || null,
        replay_profile: recordingReplayProfile()});
  } catch (error) { console.error(`Recording ADS-B timeline failed: ${error.message}`); }
  captureTriggerQueue.setAdmission(revision, admissions, integrity);
  captureEvidenceStatistics.recordCaptureAdmissions(Object.fromEntries(Object.entries(admissions).map(([id, truth]) =>
    [id, {valid: truth.valid && integrity.valid, reasons: [...new Set([...(truth.reasons || []), ...(integrity.reasons || [])])]}])));
  captureEvidenceStatistics.acceptCaptureEvents(captureTriggerQueue.eventsForRevision(revision));
}
const trackGuidedShadowRecorder = createTrackGuidedShadowRecorder(config, adsbSource);
if (startupDocument.setupRequired)
  console.error('Setup required:', startupDocument.readError || startupDocument.validation.errors);
const serverId = `${process.pid}-${Date.now()}`;
const releaseBuild = buildMetadata();
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
let receiverSyncState = receiverJournal.recover(startupDocument.revision);
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
    message: 'This recording does not contain an ADS-B timeline.'};
}
function recordedReplayTruth(kind) {
  const processor = processorStatusFresh(processorStatus) ? processorStatus.value : null;
  if (processor?.input !== 'replay') return {replay: false, value: null};
  return {replay: true, value: recordingTimeline.at(processor.file,
    processor.positionSamples, processor.sampleRate,
    kind === 'delayDoppler' ? 'delay_doppler' : 'aircraft')};
}

let restartCommand = null;
try { restartCommand = restartCommandFromEnvironment(); }
catch (error) { console.error(`Invalid restart lifecycle configuration: ${error.message}`); }

var stash_map = require('./stash/maxhold.js');
var stash_detection = require('./stash/detection.js');
var stash_iqdata = require('./stash/iqdata.js');
var stash_timing = require('./stash/timing.js');
const {encodeMapResponseAsync, presentationQuery} = require('./map-presentation');

async function sendMapResponse(req, res, frame) {
  const encoded = await encodeMapResponseAsync(frame, presentationQuery(req),
    req.get('Accept-Encoding') || '');
  res.type('application/json').vary('Accept-Encoding');
  if (encoded.contentEncoding) res.set('Content-Encoding', encoded.contentEncoding);
  res.send(encoded.body);
}

// constants
const setupPort = Number(process.env.BLAH2_SETUP_PORT || 3000);
const PORT = process.env.BLAH2_SETUP_PORT ? setupPort : Number.isInteger(config.network?.ports?.api) &&
  config.network.ports.api > 0 && config.network.ports.api <= 65535 ?
  config.network.ports.api : setupPort;
const HOST = net.isIP(config.network?.ip || '') ? config.network.ip : '0.0.0.0';
const receiverOrigins = trustedOrigins(PORT, (process.env.BLAH2_RECEIVER_ORIGINS || '').split(',').filter(Boolean));
const CONFIG_INTENT = 'config-write-v1';
const RECORDING_INTENT = 'recording-toggle-v1';
function siteNamesOnlyChange(before, after) {
  const currentRx = before?.location?.rx?.name;
  const currentTx = before?.location?.tx?.name;
  const nextRx = after?.location?.rx?.name;
  const nextTx = after?.location?.tx?.name;
  if (currentRx === nextRx && currentTx === nextTx) return false;
  const normalized = JSON.parse(JSON.stringify(after));
  if (normalized.location?.rx) normalized.location.rx.name = currentRx;
  if (normalized.location?.tx) normalized.location.tx.name = currentTx;
  return JSON.stringify(normalized) === JSON.stringify(before);
}
var map = '';
var detection = '';
var track = '';
var bearing = '';
const bearingHypotheses = new BistaticHypothesisTracker();
var timestamp = '';
var timing = '';
var iqdata = '';
const presentationDelayMs = Math.max(0,
  (Number.isInteger(config.truth?.adsb?.association_lag_cpis) ?
    config.truth.adsb.association_lag_cpis : 0) *
  (Number.isFinite(config.process?.data?.cpi) ? config.process.data.cpi * 1000 : 0));
const presentationHistories = Object.fromEntries(['map', 'detection', 'tracker',
  'bearing', 'timing', 'iqdata'].map(name => [name, new PresentationHistory()]));
function presentationCutoff(req) {
  return req.query.presentation === 'aligned' ? Date.now() - presentationDelayMs : null;
}
function requestedPresentationTimestamp(req) {
  if (req.query.frame_timestamp !== undefined) {
    const requested = Number(req.query.frame_timestamp);
    return Number.isSafeInteger(requested) && requested >= 0 ? requested : NaN;
  }
  const cutoff = presentationCutoff(req);
  if (cutoff === null) return null;
  // Map is the visual presentation clock. Selecting it once prevents requests
  // that arrive on opposite sides of a CPI boundary from mixing adjacent radar
  // frames across heatmap, detection, tracker, timing and ADS-B layers.
  return presentationHistories.map.timestampAt(cutoff);
}
function presentationFrame(req, name, current) {
  if (req.query.presentation === 'aligned') {
    const requested = requestedPresentationTimestamp(req);
    if (Number.isNaN(requested)) return '';
    if (requested !== null) return presentationHistories[name].exact(requested) ?? '';
  }
  const cutoff = presentationCutoff(req);
  return cutoff === null ? current : presentationHistories[name].at(cutoff) ?? current;
}
var capture = false;
var capturePaused = false;
var captureStartedAt = null;
let captureRevision = 0;
let replayControl = {paused: false, seekSample: 0, revision: 0};
let replaySessionFile = '';
let adsbSnapshotPending = false;
// Keep the safety check cheap and independent of browser activity. A manual
// recording is asked to stop before it can consume the configured reserve;
// automatic clips are separately rejected before admission.
setInterval(async () => {
  let disk = recordingLibrary.canRecord();
  if (!disk.allowed && recordingLibrary.retention().lowSpacePolicy === 'delete_oldest') {
    const processor = processorStatusFresh(processorStatus) ? processorStatus.value : null;
    try { await recordingLibrary.cleanup({activeFile: processor?.recordingFile || ''}); }
    catch (error) { console.error(`Low-space clip cleanup failed: ${error.message}`); }
    disk = recordingLibrary.canRecord();
  }
  if (capture && !disk.allowed) {
    capture = false; capturePaused = false; captureRevision += 1; captureStartedAt = null;
    console.error('Recording stopped to preserve the configured free-disk reserve.');
  }
}, 15000).unref();
setInterval(async () => {
  const processor = processorStatusFresh(processorStatus) ? processorStatus.value : null;
  try {
    const result = await recordingLibrary.cleanup({activeFile: processor?.recordingFile || ''});
    if (result.deleted?.length)
      console.error(`Recording retention removed ${result.deleted.length} completed recording(s).`);
  } catch (error) { console.error(`Recording retention failed: ${error.message}`); }
}, 15 * 60 * 1000).unref();

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
// SDRplay's durable startup receipt is receiver-specific.  A stale receipt
// from an earlier RSPduo session must never make Kraken, USRP or HackRF look
// like their restart failed (or block their settings transactions).
function activeSdrplayStartup(revision = null, receiverType = config.capture?.device?.type) {
  return receiverType === 'RspDuo' ? readSdrplayStartup(revision) : null;
}
function completePendingGainRestart() {
  if (!gainPublicationBlocked || !gainRestart.pending(receiverSyncState)) return;
  try {
    const document = readConfig(configFile);
    const next = gainRestart.complete({state: receiverSyncState, config,
      revision: document.revision, loadedRevision: startupDocument.revision, serverId,
      // The installed Linux restart helper writes this legacy-named receipt
      // for automatic Kraken gain restarts too.  Keep it for the gain
      // publication interlock; only operator-facing generic restart state is
      // gated by receiver type below.
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
  res.header('Access-Control-Allow-Methods', 'GET, POST, PUT, DELETE, OPTIONS');
  res.header('Access-Control-Allow-Headers', `Content-Type, If-Match, ${RECEIVER_SYNC_HEADER}, X-VectorWarp-Intent`);
  res.header('Access-Control-Expose-Headers', 'ETag');
  res.header('Cache-Control', 'private, no-cache, no-store, must-revalidate');
  res.header('Expires', '-1');
  res.header('Pragma', 'no-cache');
  next();
});
installMapImageryRoutes(app);
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
  if (readConfig(configFile).setupRequired &&
      fs.existsSync(path.join(__dirname, '..', 'html', 'display', 'configuration', 'index.html')))
    return res.redirect(302, '/display/configuration?setup=1');
  if (fs.existsSync(path.join(__dirname, '..', 'html', 'index.html'))) return next();
  res.json({service: 'VectorWarp API', settings: '/api/config', status: '/api/system/status'});
});
app.get('/api/map', async (req, res, next) => {
  try { await sendMapResponse(req, res, presentationFrame(req, 'map', map)); }
  catch (error) { next(error); }
});
app.get('/api/detection', (req, res) => {
  res.send(presentationFrame(req, 'detection', detection));
});
app.get('/api/tracker', (req, res) => {
  res.send(presentationFrame(req, 'tracker', trackerPublication.frame()));
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
  res.json(captureEvidenceStatistics.status({enabled: config.process?.bearing?.enable === true,
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
function recordingPasses() {
  const scorecard = provisionalEvidenceStatistics.liveStatus();
  const subjects = [...captureEvidenceStatistics.recordingSubjects(),
    ...(scorecard.active_passes || []), ...(scorecard.recent_finalized_passes || [])];
  return [...new Map(subjects.filter(item => item?.pass_id)
    .map(item => [item.pass_id, item])).values()];
}
function publicRecording(item) {
  const {path: replay_file, ...value} = item;
  return {...value, replay_file};
}
app.get('/api/recordings', async (req, res, next) => {
  try {
  const processor = processorStatusFresh(processorStatus) ? processorStatus.value : null;
  const offset = Number.parseInt(req.query.offset, 10);
  const limit = Number.parseInt(req.query.limit, 10);
  const result = await recordingLibrary.list({activeFile: processor?.recordingFile || '',
    passes: recordingPasses(), search: req.query.search, kind: req.query.kind,
    receiver: req.query.receiver, frequency: req.query.frequency, sort: req.query.sort,
    offset: Number.isInteger(offset) ? offset : 0,
    limit: Number.isInteger(limit) ? limit : undefined, waitForIndex: false});
  res.json({...result, recordings: result.recordings.map(publicRecording)});
  } catch (error) { next(error); }
});
app.get('/api/recordings/directories', async (req, res, next) => {
  try { res.json(await recordingLibrary.directories()); }
  catch (error) { next(error); }
});
app.get('/api/recordings/:id/replay-profile', async (req, res) => {
  const filename = recordingLibrary.resolve(req.params.id);
  if (!filename) return res.status(404).json({error: 'Recording not found.'});
  const timeline = recordingTimeline.load(filename);
  let metadata = {};
  try { metadata = JSON.parse(await fs.promises.readFile(`${filename}.json`, 'utf8')); }
  catch (_) { /* Portable recordings may only have a provenance sidecar. */ }
  if (!metadata.channels) try {
    const provenance = JSON.parse(await fs.promises.readFile(`${filename}.provenance.json`, 'utf8'));
    metadata = {...metadata, channels: provenance.channels,
      sampleRate: provenance.sampleRateHz, frequency: provenance.frequencyHz,
      receiver: provenance.receiver};
  } catch (_) { /* Header validation remains the final replay authority. */ }
  const channels = timeline?.channels ?? metadata.channels ?? null;
  const source = timeline?.replay_profile?.capture || {};
  const referenceMode = timeline?.replay_profile?.process?.reference_synthesis?.mode ||
    (channels > 2 ? 'array_eigenbeam' : 'dedicated');
  const all = Number.isInteger(channels) ? Array.from({length: channels}, (_, index) => index) : [];
  res.json({file: filename,
    sample_rate_hz: timeline?.sample_rate_hz ?? metadata.sampleRate ?? null,
    frequency_hz: timeline?.frequency_hz ?? metadata.frequency ?? null,
    channel_count: channels,
    reference_channel: source.reference_channel ?? 0,
    surveillance_channels: source.surveillance_channels ||
      (referenceMode === 'array_eigenbeam' ? all : all.filter(index => index !== 0)),
    reference_mode: referenceMode,
    reference_channels: timeline?.replay_profile?.process?.reference_synthesis?.channels ||
      (referenceMode === 'array_eigenbeam' ? all : [])});
});
app.post('/api/recordings/directories', async (req, res, next) => {
  if (!configWriteOriginAllowed(req) || req.get('X-VectorWarp-Intent') !== 'recording-directory-v1')
    return res.status(403).json({error: 'Recording-folder changes require a trusted VectorWarp origin and intent.'});
  try { res.json(await recordingLibrary.createDirectory(req.body?.name)); }
  catch (error) { next(error); }
});
app.get('/api/recordings/:id/file', (req, res) => {
  const filename = recordingLibrary.resolve(req.params.id);
  if (!filename) return res.status(404).json({error: 'Recording not found.'});
  res.type('application/octet-stream');
  res.set('Content-Disposition', `attachment; filename="${path.basename(filename).replace(/["\\]/g, '_')}"`);
  res.sendFile(filename);
});
app.delete('/api/recordings/:id', async (req, res, next) => {
  if (!configWriteOriginAllowed(req) || req.get('X-VectorWarp-Intent') !== 'recording-delete-v1')
    return res.status(403).json({error: 'Deleting a recording requires a trusted VectorWarp origin and intent.'});
  const processor = processorStatusFresh(processorStatus) ? processorStatus.value : null;
  try {
    const result = await recordingLibrary.remove(req.params.id,
      {activeFile: processor?.recordingFile || ''});
    if (!result.removed) return res.status(result.reason === 'active' ? 409 : 404).json(result);
    res.json(result);
  } catch (error) { next(error); }
});
app.post('/api/recordings/import', async (req, res, next) => {
  if (!configWriteOriginAllowed(req) || req.get('X-VectorWarp-Intent') !== 'recording-import-v1')
    return res.status(403).json({error: 'Importing a recording requires a trusted VectorWarp origin and intent.'});
  const original = path.basename(String(req.query.filename || ''));
  const extension = path.extname(original).toLowerCase();
  if (!original || !['.blah2iq', '.mchq'].includes(extension))
    return res.status(400).json({error: 'Choose a .blah2iq or .mchq recording.'});
  let temporary = null;
  try {
    const disk = await recordingLibrary.prepareForRecording({});
    const length = Number(req.get('Content-Length'));
    if (!disk.allowed || (Number.isFinite(length) && disk.free_bytes !== null &&
        disk.free_bytes - length <= disk.required_free_bytes))
      return res.status(507).json({error: 'The import would cross the configured free-disk reserve.'});
    const directory = recordingLibrary.directory();
    await fs.promises.mkdir(directory, {recursive: true, mode: 0o770});
    const stem = path.basename(original, extension).replace(/[^A-Za-z0-9._-]+/g, '-').slice(0, 120) || 'recording';
    let filename = path.join(directory, `${stem}${extension}`);
    if (fs.existsSync(filename)) filename = path.join(directory, `${stem}-${Date.now()}${extension}`);
    temporary = path.join(directory, `.import-${crypto.randomUUID()}.partial`);
    await pipeline(req, fs.createWriteStream(temporary, {flags: 'wx', mode: 0o660}));
    const stat = await fs.promises.stat(temporary);
    if (!stat.size) throw Object.assign(new Error('The selected recording was empty.'), {status: 400});
    await fs.promises.rename(temporary, filename); temporary = null;
    recordingLibrary.cache = null;
    res.status(201).json({imported: true, filename: path.basename(filename), size_bytes: stat.size});
  } catch (error) {
    if (temporary) await fs.promises.rm(temporary, {force: true}).catch(() => {});
    next(error);
  }
});
app.post('/api/recordings/cleanup', async (req, res, next) => {
  if (!configWriteOriginAllowed(req) || req.get('X-VectorWarp-Intent') !== 'recording-cleanup-v1')
    return res.status(403).json({error: 'Recording cleanup requires a trusted VectorWarp origin and intent.'});
  const processor = processorStatusFresh(processorStatus) ? processorStatus.value : null;
  try { res.json(await recordingLibrary.cleanup({activeFile: processor?.recordingFile || '', force: true})); }
  catch (error) { next(error); }
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
  res.type('json').send(presentationFrame(req, 'bearing', bearing));
});
app.get('/api/locations', (req, res) => {
  let frame = null;
  const presentedBearing = presentationFrame(req, 'bearing', bearing);
  try { frame = presentedBearing ? JSON.parse(presentedBearing) : null; }
  catch (_) { return res.status(503).json({enabled: true, available: false,
    reason: 'The current bearing frame is invalid.', data: []}); }
  const result = locationsFromBearing(frame, config, bearingHypotheses);
  res.status(result.enabled && !result.available ? 503 : 200).json(result);
});
app.get('/api/timestamp', (req, res) => {
  res.send(timestamp);
});
app.get('/api/timing', (req, res) => {
  res.send(presentationFrame(req, 'timing', timing));
});
app.get('/api/presentation/status', (req, res) => {
  const now = Date.now();
  const expectedIntervalMs = Number.isFinite(config.process?.data?.cpi) ?
    config.process.data.cpi * 1000 : null;
  const selectedTimestamp = presentationHistories.map.timestampAt(now - presentationDelayMs);
  const aligned = selectedTimestamp === null ? {} : Object.fromEntries(
    Object.entries(presentationHistories).map(([name, history]) =>
      [name, history.exact(selectedTimestamp) !== null]));
  const latest = currentTimingBackend();
  res.json({
    presentation_delay_cpis: Number.isInteger(config.truth?.adsb?.association_lag_cpis) ?
      config.truth.adsb.association_lag_cpis : 0,
    presentation_delay_ms: presentationDelayMs,
    expected_frame_interval_ms: expectedIntervalMs,
    selected_frame_timestamp: selectedTimestamp,
    selected_frame_age_ms: selectedTimestamp === null ? null : now - selectedTimestamp,
    aligned_layers: aligned,
    relative_alignment_ms: selectedTimestamp === null ? null : 0,
    cadence: presentationHistories.map.diagnostics(expectedIntervalMs),
    event_time: latest?.eventTime ? {
      source: latest.eventTime.source,
      exact: latest.eventTime.exact === true,
      uncertainty_bounded: latest.eventTime.uncertaintyBounded === true,
      reason: latest.eventTime.reason || null
    } : null
  });
});
app.get('/api/iqdata', (req, res) => {
  res.send(presentationFrame(req, 'iqdata', iqdata));
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
  const freshness = Math.max(10000, (config.process?.data?.cpi || 1) * 3000);
  const processor = processorStatusFresh(processorStatus) ? processorStatus.value : null;
  const radar = gainPublicationBlocked || lastFrameAt === null ? 'no-data' :
    Date.now() - lastFrameAt < freshness ? 'receiving' : 'stale';
  // The header is present on every page and polls continuously. Keep its
  // response cheap: receiver discovery, GPU setup, build metadata and the full
  // ADS-B source report belong to their dedicated/settings views.
  if (req.query.summary === '1') {
    const headerProcessor = processor ? {
      input: processor.input, state: processor.state, error: processor.error || '',
      receiver: processor.receiver, receiverHealth: processor.receiverHealth,
      totalSamples: processor.totalSamples, positionSamples: processor.positionSamples,
      sampleRate: processor.sampleRate, file: processor.file
    } : null;
    return res.json({radar, adsbEnabled: config.truth?.adsb?.enabled === true,
      recordedAdsb: processor?.input === 'replay' && recordingTimeline.has(processor.file),
      restart: {state: restartState.state}, gainCheckPending: gainPublicationBlocked,
      processor: headerProcessor, processorFresh: processor !== null,
      errors: [...serviceErrors.values()],
      message: lastFrameAt === null ? 'No radar frames received.' : null});
  }
  const document = readConfig(configFile);
  const sdrplayStartup = activeSdrplayStartup(document.revision, document.config.capture?.device?.type);
  const persistedRestart = sdrplayStartup && sdrplayStartup.updatedAt >= (restartState.requestedAt || 0) ?
    {state: sdrplayStartup.inProgress ? 'running' : sdrplayStartup.state === 'failed' ? 'failed' : 'command-complete',
      message: sdrplayStartup.message, startedAt: sdrplayStartup.updatedAt} : restartState;
  const gpuSetup = await gpuSetupStatus(() => currentGpuRuntime(freshness));
  const backend = currentTimingBackend();
  const adsb = await adsbSource.status();
  const sourceRows = Array.isArray(adsb?.sources) ? adsb.sources : [];
  const positionSources = adsb?.fusion?.position_sources || {};
  const featureDiagnostics = {
    recording: {
      requested: capture, paused: capturePaused,
      active: processor?.input === 'live' && processor.recording === true,
      errorPresent: Boolean(processor?.recordingError),
      recordedSamples: Number.isSafeInteger(processor?.recordedSamples) ? processor.recordedSamples : 0,
      clipRevision: Number.isSafeInteger(processor?.clipRevision) ? processor.clipRevision : 0,
      clipErrorPresent: Boolean(processor?.clipError),
      clipSamples: Number.isSafeInteger(processor?.clipSamples) ? processor.clipSamples : 0,
      timelineCaptureAvailable: true,
      automaticCleanup: config.save?.retention?.automatic_cleanup === true,
      lowSpacePolicy: config.save?.retention?.low_space_policy || null,
      maximumAgeDays: Number.isSafeInteger(config.save?.retention?.maximum_age_days) ?
        config.save.retention.maximum_age_days : null,
      maximumTotalGb: Number.isSafeInteger(config.save?.retention?.maximum_total_gb) ?
        config.save.retention.maximum_total_gb : null,
      minimumFreeGb: Number.isSafeInteger(config.save?.retention?.minimum_free_gb) ?
        config.save.retention.minimum_free_gb : null
    },
    replay: {
      active: processor?.input === 'replay', paused: replayControl.paused,
      seekable: processor?.input === 'replay' && Number.isSafeInteger(processor.totalSamples),
      positionSamples: Number.isSafeInteger(processor?.positionSamples) ? processor.positionSamples : null,
      totalSamples: Number.isSafeInteger(processor?.totalSamples) ? processor.totalSamples : null,
      sampleRateHz: Number.isSafeInteger(processor?.sampleRate) ? processor.sampleRate : null,
      timelineAvailable: processor?.input === 'replay' && recordingTimeline.has(processor.file)
    },
    adsb: {
      enabled: config.truth?.adsb?.enabled === true, online: adsb?.online === true,
      degraded: adsb?.degraded === true, sourceCount: sourceRows.length,
      healthySourceCount: sourceRows.filter(source => source?.healthy === true).length,
      aircraftCount: Number.isSafeInteger(adsb?.fusion?.aircraft) ? adsb.fusion.aircraft : null,
      directAircraftCount: Number.isSafeInteger(positionSources.ADSB_DIRECT) ? positionSources.ADSB_DIRECT : 0,
      mlatAircraftCount: Number.isSafeInteger(positionSources.MLAT) ? positionSources.MLAT : 0,
      associationLagCpis: Number.isSafeInteger(adsb?.association_lag_cpis) ? adsb.association_lag_cpis : null,
      associationLagMs: Number.isSafeInteger(adsb?.association_lag_ms) ? adsb.association_lag_ms : null
    },
    processing: {
      acceleration: backend.acceleration?.active || null,
      clutterAcceleration: backend.clutterAcceleration?.active || null,
      configuredCpiMs: Number.isFinite(config.process?.data?.cpi) ?
        Math.round(config.process.data.cpi * 1000) : null
    },
    presentation: {
      timestampConnections,
      associationLagCpis: Number.isSafeInteger(adsb?.association_lag_cpis) ? adsb.association_lag_cpis : null,
      associationLagMs: Number.isSafeInteger(adsb?.association_lag_ms) ? adsb.association_lag_ms : null
    }
  };
  res.json({serverId, configRevision: document.revision,
    build: releaseBuild,
    system: {os: os.platform(), architecture: os.arch(), kernel: os.release()},
    uptimeSeconds: Math.floor(process.uptime()),
    acceleration: backend.acceleration || null,
    clutterAcceleration: backend.clutterAcceleration || null, gpuSetup,
    loadedRevision: startupDocument.revision, setupRequired: document.setupRequired,
    restart: persistedRestart, sdrplayStartup, receiverSynchronization: receiverSyncState,
    gainMonitor: gainMonitorStatus, adsb, featureDiagnostics,
    lastFrameAt, timestampConnections,
    adsbEnabled: config.truth?.adsb?.enabled === true,
    gainCheckPending: gainPublicationBlocked,
    radar,
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
  const previous = processorStatusFresh(processorStatus) ? processorStatus.value : null;
  const now = Date.now();
  processorStatus = {value: checked.value, receivedAt: now};
  if (checked.value.input === 'replay' && checked.value.file !== replaySessionFile) {
    replaySessionFile = checked.value.file;
    replayControl = {paused: false, seekSample: 0, revision: replayControl.revision + 1};
  } else if (checked.value.input === 'live' && replaySessionFile) {
    replaySessionFile = '';
    replayControl = {paused: false, seekSample: 0, revision: replayControl.revision + 1};
  }
  if (previous?.input === 'live' && previous.recording &&
      (!checked.value.recording || checked.value.recordingFile !== previous.recordingFile))
    void recordingTimeline.stop(previous.recordingFile, now)
      .catch(error => console.error(`Manual recording ADS-B timeline failed: ${error.message}`));
  if (checked.value.input === 'live' && checked.value.recording && checked.value.recordingFile) {
    const startMs = now - checked.value.recordedSamples * 1000 / checked.value.sampleRate;
    recordingTimeline.start(checked.value.recordingFile, startMs,
      {sample_rate_hz: checked.value.sampleRate, frequency_hz: checked.value.frequency,
        channels: config.capture?.device?.channel_count || 2,
        receiver: checked.value.receiver, replay_profile: recordingReplayProfile()});
  }
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
          captureEvidenceStatistics.acceptCaptureEvents(
            captureTriggerQueue.eventsForRevision(revision));
      }, CLIP_PRE_ROLL_RETRY_DELAY_MS).unref();
    } else if (checked.value.clipError) captureEvidenceStatistics.acceptCaptureEvents(
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
    activeSdrplayStartup(null)?.inProgress === true,
  processorStatus: () => processorStatusFresh(processorStatus) ? processorStatus.value : null,
  preview: process.env.BLAH2_PREVIEW === 'true',
  compiledLiveTypes: process.env.BLAH2_RECEIVER_TYPES ?
    process.env.BLAH2_RECEIVER_TYPES.split(',').map(value => value.trim()).filter(Boolean) : null
});
if (macRspduoBuild) installMacRspduoBuildRoutes(app, {allowedOrigins: receiverOrigins, build: macRspduoBuild,
  transactionBusy: () => configWriteInProgress || receiverManagement.busy() ||
    ['running', 'scheduled'].includes(restartState.state) || activeSdrplayStartup(null)?.inProgress === true});
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
  if (activeSdrplayStartup(null, req.body?.capture?.device?.type)?.inProgress)
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
  // Site labels are presentation metadata. They do not retune the receiver,
  // change radar geometry, or alter processing, so do not force automatic-gain
  // measurement or stop a healthy processor merely to rename a map marker.
  if (!saveLater && !wantsRestart && siteNamesOnlyChange(currentDocument.config, req.body)) {
    const saved = saveConfig(configFile, req.body, currentDocument.revision);
    const priorReceipt = receiverSyncState.receipt || {schemaVersion: 1,
      receiverType: req.body.capture.device.type, status: 'not-required', operations: []};
    const receipt = {...priorReceipt, configRevision: saved.revision,
      configPersisted: true, metadataOnly: true};
    const settledState = ['synchronized', 'already-matched', 'not-required'].includes(
      receiverSyncState.state) ? receiverSyncState.state : receipt.status;
    receiverSyncState = {...receiverSyncState, state: settledState,
      completedAt: Date.now(), receipt, configRevision: saved.revision,
      applicationVerified: true, reconciliationRequired: false};
    receiverJournal.write(receiverSyncState);
    upstreamCache = null;
    return res.json({ok: true, restarting: false, revision: saved.revision,
      config: saved.config, receiverSync: receipt,
      message: 'Site names saved. Live receiver and radar processing were not restarted.'});
  }
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
    if (restartState.state === 'running' || restartState.state === 'scheduled' ||
        activeSdrplayStartup(null, req.body?.capture?.device?.type)?.inProgress)
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
    // Operator confirmations describe the physical array installation. RF
    // retunes, calibration resets and gain writes change the scientific
    // identity, but do not move elements or remap DAQ channels. Only a
    // topology-changing receiver command invalidates those attestations.
    if (receiverSync.operations?.some(operation => operation.commandSent &&
        operation.operation === 'set_num_elements'))
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
    const applicationVerified = ['synchronized', 'already-matched', 'not-required'].includes(
      receiverSync.status) && receiverReconciliationRequired === false;
    receiverSyncState = {state: receiverSync.status, completedAt: Date.now(),
      receipt: receiverSync, configRevision: saved.revision,
      applicationVerified,
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
  if (replayTruthDisabled()) {
    const recorded = recordedReplayTruth('delayDoppler');
    return recorded.value === null ? res.json(replayTruthStatus()) :
      res.json({url: '/api/adsb/delay-doppler', replay: true});
  }
  if (!config.truth?.adsb?.enabled) return res.status(400).end();
  // Compatibility link now stays on this API; no converter URL or query is accepted.
  res.json({url: '/api/adsb/delay-doppler'});
});
for (const [route, kind] of [['/api/adsb', 'aircraft'], ['/api/adsb/delay-doppler', 'delayDoppler']]) {
  app.get(route, async (req, res) => {
    const recorded = recordedReplayTruth(kind);
    if (recorded.replay && recorded.value === null) return res.json(replayTruthStatus());
    if (!recorded.replay && replayTruthDisabled()) return res.json(replayTruthStatus());
    try {
      const presentation = req.query.presentation;
      const requestedTimestamp = presentation === 'aligned' ?
        requestedPresentationTimestamp(req) : null;
      if (Number.isNaN(requestedTimestamp))
        return res.status(400).json({error: 'frame_timestamp must be a non-negative integer'});
      const value = recorded.replay ? recorded.value : presentation === 'aligned' && typeof adsbSource.getPresentationAt === 'function' ?
        await adsbSource.getPresentationAt(kind,
          requestedTimestamp ?? Date.now() - presentationDelayMs) :
        presentation && typeof adsbSource.getPresentation === 'function' ?
          await adsbSource.getPresentation(kind) : await adsbSource.get(kind);
      if (requestedTimestamp !== null)
        res.set('X-VectorWarp-Presentation-Timestamp', String(requestedTimestamp));
      if (kind === 'delayDoppler' &&
          (req.query.presentation === 'overlay' || req.query.overlay === '1')) {
        const compact = {};
        for (const [id, aircraft] of Object.entries(value || {})) {
          if (!aircraft || !Number.isFinite(aircraft.delay) || !Number.isFinite(aircraft.doppler)) continue;
          const position = aircraft.position && typeof aircraft.position === 'object' ? aircraft.position : {};
          compact[id] = {...Object.fromEntries([
            'delay', 'doppler', 'hex', 'flight', 'r', 't', 'type', 'desc', 'category',
            'position_source', 'source_fields',
            'alt_baro', 'alt_geom', 'gs', 'track', 'geom_rate', 'baro_rate',
            'squawk', 'rssi', 'seen_pos'
          ].filter(key => aircraft[key] !== undefined).map(key => [key, aircraft[key]])),
          hex: aircraft.hex || id,
          category: aircraft.category ?? position.category,
          alt_baro: aircraft.alt_baro ?? position.altitude_baro_ft,
          alt_geom: aircraft.alt_geom ?? position.altitude_geom_ft,
          gs: aircraft.gs ?? position.ground_speed_kt,
          track: aircraft.track ?? position.track_deg_true,
          geom_rate: aircraft.geom_rate ?? position.geom_rate_fpm,
          baro_rate: aircraft.baro_rate ?? position.baro_rate_fpm,
          squawk: aircraft.squawk ?? position.squawk,
          rssi: aircraft.rssi ?? position.rssi_dbfs,
          seen_pos: aircraft.seen_pos ?? position.seen_pos_s};
        }
        return res.json(compact);
      }
      res.json(value);
    }
    catch (error) { res.status(503).json({error: error.message}); }
  });
}
app.get('/api/adsb/status', async (req, res) => {
  const recorded = recordedReplayTruth('aircraft');
  if (recorded.replay) return res.json(recorded.value === null ? replayTruthStatus() :
    {enabled: true, online: true, replay: true, message: 'Recorded ADS-B timeline'});
  if (replayTruthDisabled()) return res.json(replayTruthStatus());
  res.json(await adsbSource.status());
});
app.get('/api/radar/coverage', (req, res) => {
  if (replayTruthDisabled()) return res.json(replayTruthStatus());
  const hours = Number(req.query.hours || 24);
  const bins = Number(req.query.bins || 72);
  const altitudeBand = String(req.query.altitude || 'all');
  res.set('Cache-Control', 'private, max-age=30');
  res.json(provisionalEvidenceStatistics.radarCoverage({hours, bins, altitudeBand}));
});
app.get('/api/adsb/aircraft-photo/:hex', async (req, res) => {
  try {
    const value = await aircraftPhotoLookup.lookup(req.params.hex);
    res.set('Cache-Control', value.available ? 'private, max-age=86400' : 'private, max-age=900');
    res.json(value);
  } catch (error) {
    res.status(error.statusCode || 503).json({available: false, error: error.message});
  }
});
app.get('/api/bearing/manifold-recorder', (req, res) => {
  res.json(manifoldRecorder.status());
});
app.get('/api/bearing/manifold-automation', (req, res) => {
  res.json(manifoldAutomation.status());
});
function arrayCalibrationStatus() {
  const bearing = manifoldAutomation.status();
  const detection = detectionManifoldAutomation.status();
  return buildArrayCalibrationStatus({config,
    recorderStatus: manifoldRecorder.status(), bearingStatus: bearing,
    detectionStatus: detection,
    frequencyResolvedStatus: frequencyResolvedShadow.status()});
}
app.get('/api/array-calibration/status', (req, res) => {
  // Both evaluators are signature-cached and run away from the API thread.
  void manifoldAutomation.evaluate();
  void detectionManifoldAutomation.evaluate();
  res.json(arrayCalibrationStatus());
});
app.get('/api/bearing/manifold-progress', (req, res) => {
  // Evaluation is signature-cached and runs off the API thread. Polling this
  // view refreshes a changed corpus promptly without loading the radar
  // processor or repeatedly fitting unchanged data.
  void manifoldAutomation.evaluate();
  void detectionManifoldAutomation.evaluate();
  const automation = manifoldAutomation.status();
  const detectionAutomation = detectionManifoldAutomation.status();
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
    detection: {active_model: config.process?.surveillance_fusion?.manifold_file || null,
      automation: {enabled: detectionAutomation.enabled, state: detectionAutomation.state,
        running: detectionAutomation.running, qualified: detectionAutomation.qualified,
        promotion_allowed: false, failures: detectionAutomation.failures || [],
        validation_path: detectionAutomation.validationPath || null,
        candidate_path: detectionAutomation.candidatePath || null,
        runtime_candidate_path: detectionAutomation.runtimeCandidatePath || null,
        error: detectionAutomation.error,
        last_evaluation_at: detectionAutomation.lastEvaluationAt || null},
      progress: detectionAutomation.progress},
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
app.get('/stash/map', async (req, res, next) => {
  try { await sendMapResponse(req, res, stash_map.get_data_map()); }
  catch (error) { next(error); }
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
  revision: captureRevision, clip: captureTriggerQueue.request(true), replay: replayControl}));
app.get('/api/replay/status', (req, res) => {
  const processor = processorStatusFresh(processorStatus) ? processorStatus.value : null;
  if (processor?.input !== 'replay') return res.json({active: false});
  res.json({active: true, paused: replayControl.paused, file: processor.file,
    position_samples: processor.positionSamples, total_samples: processor.totalSamples,
    sample_rate_hz: processor.sampleRate, state: processor.state});
});
app.post('/api/replay/control', (req, res) => {
  if (!configWriteOriginAllowed(req) || req.get('X-VectorWarp-Intent') !== 'replay-control-v1')
    return res.status(403).json({error: 'Replay controls require a trusted VectorWarp origin and intent.'});
  const processor = processorStatusFresh(processorStatus) ? processorStatus.value : null;
  if (processor?.input !== 'replay') return res.status(409).json({error: 'No recording is being replayed.'});
  const action = String(req.body?.action || '');
  if (action === 'pause' || action === 'play') replayControl.paused = action === 'pause';
  else if (action === 'seek' || action === 'skip') {
    const seconds = Number(req.body?.seconds);
    if (!Number.isFinite(seconds)) return res.status(400).json({error: 'Replay position must be finite.'});
    const base = action === 'skip' ? processor.positionSamples : 0;
    const target = Math.max(0, Math.min(processor.totalSamples,
      Math.round(base + seconds * processor.sampleRate)));
    replayControl = {...replayControl, seekSample: target, revision: replayControl.revision + 1};
  } else return res.status(400).json({error: 'Choose play, pause, seek, or skip.'});
  res.json({ok: true, ...replayControl});
});
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
  const disk = recordingLibrary.canRecord();
  const status = {
    supported: !replay,
    available: !replay && disk.allowed && (capture ? acknowledgement !== null : receiving),
    reason: replay ? 'Recording is unavailable while replay is active.' : recordingError ||
      (!disk.allowed ? 'Recording paused to preserve the configured free-disk reserve.' :
        (!receiving && !capture ? 'Start live radar and wait for frames before recording.' : null)),
    requested: capture,
    paused: capturePaused,
    revision: captureRevision,
    acknowledged: acknowledged === true,
    recording,
    recordingFile: acknowledgement?.recordingFile || '',
    recordingError,
    recordedSamples: acknowledgement?.recordedSamples || 0,
    startedAt: recording ? captureStartedAt : null,
    elapsedSeconds: recording && captureStartedAt ?
      Math.max(0, Math.floor((now - captureStartedAt) / 1000)) : 0,
    storage: disk
  };
  // Header controls only need manual-recording state.  The full automatic-clip
  // ledger can be several megabytes and must not be transferred every four
  // seconds by every open display.
  if (req.query.summary !== '1') status.clip = captureTriggerQueue.status();
  res.json(status);
});
// toggle state of capture
app.post('/capture/toggle', async (req, res, next) => {
  if (!configWriteOriginAllowed(req) || req.get('X-VectorWarp-Intent') !== RECORDING_INTENT)
    return res.status(403).json({error: 'Recording changes require a trusted VectorWarp origin and intent.'});
  if (gainPublicationBlocked)
    return res.status(409).json({error: 'Wait for the gain check and processing restart before recording.'});
  const processor = processorStatusFresh(processorStatus) ? processorStatus.value : null;
  if (processor?.input === 'replay' || config.capture.replay?.state === true)
    return res.status(409).json({error: 'Recording is unavailable while replay is active.'});
  if (!capture) {
    let disk;
    try { disk = await recordingLibrary.prepareForRecording({activeFile: processor?.recordingFile || ''}); }
    catch (error) { return next(error); }
    if (!disk.allowed)
      return res.status(507).json({error: 'Recording would cross the configured free-disk reserve. Clean up recordings or change the retention settings.'});
  }
  if (!capture && (lastFrameAt === null || Date.now() - lastFrameAt >
      Math.max(10000, (config.process.data.cpi || 1) * 3000)))
    return res.status(409).json({error: 'No live radar frames. Start the radar before recording.'});
  if (capturePaused) { capture = false; capturePaused = false; }
  else capture = !capture;
  captureRevision += 1;
  captureStartedAt = capture ? Date.now() : null;
  res.json({requested: capture, paused: capturePaused, revision: captureRevision, recording: processor?.recording === true,
    acknowledged: processor?.input === 'live' && processor.recordingRequestId === captureRevision && processor.recording === capture,
    startedAt: captureStartedAt});
});
app.post('/capture/pause', async (req, res, next) => {
  if (!configWriteOriginAllowed(req) || req.get('X-VectorWarp-Intent') !== 'recording-pause-v1')
    return res.status(403).json({error: 'Recording changes require a trusted VectorWarp origin and intent.'});
  const pause = req.body?.paused === true;
  if (pause && !capture)
    return res.status(409).json({error: 'Start a manual recording before pausing it.'});
  if (!pause && !capturePaused)
    return res.status(409).json({error: 'No paused manual recording is available to resume.'});
  if (!pause) {
    const disk = await recordingLibrary.prepareForRecording({});
    if (!disk.allowed)
      return res.status(507).json({error: 'Recording cannot resume without crossing the free-disk reserve.'});
  }
  capture = !pause;
  capturePaused = pause;
  captureRevision += 1;
  captureStartedAt = capture ? Date.now() : null;
  // A safe pause finalizes the current portable recording. Resume starts a new
  // segment so no discontinuous sample epochs are silently joined.
  res.json({requested: capture, paused: capturePaused, revision: captureRevision,
    startedAt: captureStartedAt, segmented: true});
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
let compressedPlotly = null;
app.get('/lib/plotly-2.20.0.min.js', (req, res, next) => {
  if (!/(^|,)\s*gzip\b/.test(req.get('Accept-Encoding') || '')) return next();
  try {
    if (!compressedPlotly) {
      const filename = path.join(__dirname, '..', 'html', 'lib', 'plotly-2.20.0.min.js');
      compressedPlotly = zlib.gzipSync(fs.readFileSync(filename), {level: 6});
    }
    res.set('Content-Encoding', 'gzip');
    res.set('Vary', 'Accept-Encoding');
    res.set('Cache-Control', 'public, max-age=31536000, immutable');
    res.type('application/javascript').send(compressedPlotly);
  } catch (_) { next(); }
});
app.use(express.static(path.join(__dirname, '..', 'html'), {setHeaders(response, filename) {
  const extension = path.extname(filename).toLowerCase();
  if (path.basename(filename) === 'plotly-2.20.0.min.js')
    response.setHeader('Cache-Control', 'public, max-age=31536000, immutable');
  else if (['.js', '.css', '.svg', '.png', '.woff', '.woff2'].includes(extension))
    response.setHeader('Cache-Control', 'public, max-age=3600, must-revalidate');
}}));
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
  presentationHistories.map.add(frame);
  stash_map.update_data(map);
  const at = frameTimestamp(frame);
  if (Number.isSafeInteger(at) && !adsbSnapshotPending &&
      typeof adsbSource.getPresentationAt === 'function') {
    adsbSnapshotPending = true;
    Promise.all([adsbSource.getPresentationAt('aircraft', at),
      adsbSource.getPresentationAt('delayDoppler', at)])
      .then(([aircraft, delayDoppler]) => recordingTimeline.add({timestamp_ms: at,
        aircraft, delay_doppler: delayDoppler}))
      .catch(() => {}).finally(() => { adsbSnapshotPending = false; });
  }
});
listenData(server_map, 'map');

const server_detection = jsonStreamServer('detection', frame => {
  detection = frame;
  presentationHistories.detection.add(frame);
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
  presentationHistories.tracker.add(published);
  evidenceStatistics.acceptTracker(published);
  provisionalEvidenceStatistics.acceptTracker(published);
  oprsEventRecorder.acceptTracker(published);
});
listenData(server_tracker, 'track');

if (config.process?.bearing?.enable === true && Number.isInteger(config.network?.ports?.bearing)) {
  const server_bearing = jsonStreamServer('bearing', frame => {
    bearing = frame;
    presentationHistories.bearing.add(frame);
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
  const feed = jsonFrames(frame => {
    const parsed = JSON.parse(frame);
    const value = parsed?.timestamp;
    if (!Number.isSafeInteger(value) || value < 0)
      throw new Error('Expected a non-negative safe-integer radar timestamp');
    if (!timestampConnectionEligible(connection)) return;
    activeTimestampConnection = connection.id;
    serviceErrors.delete('timestamp');
    gainRestartTimestampAt = Date.now();
    completePendingGainRestart();
    if (gainPublicationBlocked) return;
    lastFrameAt = Date.now();
    timestamp = String(value);
  });
  socket.on("data",(msg)=>{
    try { feed(msg); }
    catch (error) {
      serviceErrors.set('timestamp', `timestamp connection: ${error.message}`);
      socket.destroy();
    }
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
    presentationHistories.timing.add(frame);
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
  presentationHistories.iqdata.add(frame);
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
