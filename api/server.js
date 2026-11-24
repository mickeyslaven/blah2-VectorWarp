const express = require('express');
const net = require("net");
const http = require('http');
const fs = require('fs');
const yaml = require('js-yaml');
const crypto = require('crypto');
const path = require('path');
const os = require('os');
const zlib = require('zlib');
const {buildMetadata, compiledReceiverTypes} = require('./build-metadata');
const {execFile} = require('child_process');
const {restartCommandFromEnvironment, commandAvailable, launch: launchLifecycleRestart} = require('./lifecycle.js');
const {getDeviceProfiles, validateConfig, FIELD_RULES, publicationPolicyError,
  assessResolvedRadarPlans} = require('./config-manager.js');
const {readNativeProcessingCapabilities, readGpuDriverStatus,
  enrichNativeProcessingCapabilities} =
  require('./native-processing-capabilities.js');
const {readConfig, writable, saveConfig, migrateKnownDefaults,
  preserveConfigBytesDuringUpgrade, checkRadarPlan, readConfigMigrations} = require('./config-store.js');
const {getUpstreamStatus} = require('./upstream-status.js');
const {installReceiverRoutes, trustedOrigins, sameReceiverOrigin} = require('./receiver-routes.js');
const {createReceiverSynchronizer, receiverAcceptanceBoundary} = require('./receiver-sync.js');
const {createAutomaticGain, enabled: automaticGainEnabled, validateProbe,
  MAX_AUTOMATIC_GAIN_MS} = require('./automatic-gain.js');
const {measureGain, waitForGainSettled} = require('./kraken-gain-probe.js');
const {createReceiverHelperClient} = require('./receiver-helper-client.js');
const gainRestartPreflight = createReceiverHelperClient();
const {createGainMonitor} = require('./gain-monitor.js');
const directGain = require('./direct-gain.js');
const {createReceiverJournal} = require('./receiver-journal.js');
const gainRestart = require('./gain-restart.js');
const {invalidate: invalidateGeometry} = require('../html/js/kraken_geometry');
const {displayOnlyChange} = require('../html/js/config_profiles');
const {createAdsbSource} = require('./adsb-source.js');
const {DelayDopplerHistory} = require('./adsb-geometry.js');
const {createRemoteIdSource} = require('./remote-id-source.js');
const {createAircraftPhotoLookup} = require('./aircraft-photo.js');
const {createManifoldRecorder, siteFingerprint} = require('./manifold-recorder.js');
const {createCausalFunnelRecorder} = require('./causal-funnel-recorder.js');
const {createCausalFunnelProducer} = require('./causal-funnel-producer.js');
const {ingestTrackerFrame} = require('./tracker-ingestion.js');
const {buildArrayCalibrationStatus} = require('./array-calibration-status.js');
const {diagnoseUpstreamArrayChannels} = require('./array-channel-health.js');
const {createTrackerForwarder} = require('./tracker-forward.js');
const {buildObservationBundle} = require('./radar-observation-bundle.js');
const {createEvidenceStatisticsLanes, provisionalTimingFrame,
  defaultProvisionalEvidenceStatisticsPath, compactAssociationPass,
  federationAssociationPassTransport} =
  require('./evidence-statistics.js');
const {MultiIlluminatorEvidence, primaryRadarContext, associationStatistics} =
  require('./multi-illuminator-evidence.js');
const {kioskAssociationChannel} = require('../html/js/measured_track_coverage');
const {createOprsEventRecorder} = require('./oprs-event-recorder.js');
const {verifyCaptureIntegrity, writeAdmissionSidecar, validateProcessorSampleRanges,
  captureArtifactState, admissionsNeedTruthSettlement} = require('./capture-admission.js');
const {createCaptureTriggerQueue, optionsFromEnvironment: captureTriggerOptions} =
  require('./capture-trigger-queue.js');
const {automaticCaptureReadiness} = require('./automatic-capture-readiness.js');
const {bandPlans: illuminatorBandPlans, receiverCapabilities: illuminatorReceiverCapabilities,
  evaluateFastSweep: evaluateFastIlluminatorSweep,
  evaluateSurvey: evaluateIlluminatorSurvey} = require('./illuminator-survey.js');
const {catalogSources: illuminatorCatalogSources} = require('./illuminator-catalog.js');
const {RecordingLibrary} = require('./recording-library.js');
const {portableRecordingBytesPerSecond, manualRecordingExpired} = require('./recording-rate.js');
const {RecordingTimeline, eventTruthAt} = require('./recording-timeline.js');
const {installRecordingLibraryRoutes} = require('./recording-library-routes.js');
const {checkNetworkBindings, bindMessage} = require('./network-check.js');
const {installMapImageryRoutes} = require('./map-imagery.js');
const {status: validateProcessorStatus} = require('./processor-status.js');
const {geographicContext, geographicConfig, forGeographicLane, setReplaySiteStatusProvider} = require('./replay-geographic-sites');
const {controlsAvailable: replayControlsAvailable, updateControl: updateReplayControl} = require('./replay-control.js');
const {createGpuSetupStatus} = require('./gpu-setup.js');
const jsonFrames = require('./json-frames.js');
const {RadarSessionStore, frameTimestamp, configuredPresentationDelay} = require('./presentation-history.js');
const {MultiIlluminatorTrackFuser, validateFusionAgainstAdsb, siteCatalog} =
  require('./multi-illuminator-track-fusion.js');
const {BistaticHypothesisTracker, locationsFromBearing} = require('./bistatic-geolocation.js');
const {RadarPerformanceHealth} = require('./radar-performance-health.js');
const {installNodeFederation, nodeRole} = require('./node-federation');
const localRadar = nodeRole() === 'radar';
const gpuSetupStatus = createGpuSetupStatus({preview: process.env.BLAH2_PREVIEW === 'true'});

// parse config file
const configFile = path.resolve(process.argv[2] || 'config/config.yml');
try {
  // A package upgrade must restore a previously running API without changing
  // the operator-owned conffile.  The package activation helper sets this
  // short boot-relative lease across bounded startup retries; ordinary starts
  // retain the narrowly scoped released-default migrations below.
  if (!preserveConfigBytesDuringUpgrade(process.env, os.uptime()) && migrateKnownDefaults(configFile))
    console.error('Migrated known safe configuration defaults; the previous file is in config.yml.bak.');
} catch (error) {
  console.error(`Could not migrate known configuration defaults: ${error.message}`);
}
const startupDocument = readConfig(configFile);
var config = startupDocument.config;
const retiredPublicationError = publicationPolicyError(config);
const radarSession = new RadarSessionStore({config, onProduct: observeRadarProduct,
  onReset() {
    multiIlluminatorTrackFuser.reset();
    radarPerformanceHealth.reset();
  }});
const adsbSource = createAdsbSource(config, {preview: process.env.BLAH2_PREVIEW === 'true'});
const primaryRadar = primaryRadarContext(config, adsbSource);
const createIlluminatorAdsbGeometry = () => new Map((config.process?.multi_illuminator?.illuminators || [])
  .filter(item => item?.enabled === true).map(item => [item.id,
    new DelayDopplerHistory(forGeographicLane({...config,
      capture: {...config.capture, fc: item.frequency_hz},
      location: {...config.location, tx: item.transmitter}}, item.id))]));
const illuminatorAdsbGeometry = createIlluminatorAdsbGeometry();
const recordingIlluminatorAdsbGeometry = createIlluminatorAdsbGeometry();
const replayPrimaryAdsbGeometry = new DelayDopplerHistory(config);
const remoteIdSource = createRemoteIdSource(config, {preview: process.env.BLAH2_PREVIEW === 'true'});
const aircraftPhotoLookup = createAircraftPhotoLookup(config, {cacheFile:
  process.env.BLAH2_AIRCRAFT_PHOTO_CACHE || '/var/lib/vectorwarp-api/aircraft-photo-metadata.json'});
const manifoldRecorder = createManifoldRecorder(config, adsbSource);
// This phase-1/2 recorder is intentionally constructed without ADS-B or any
// other truth input. It remains disabled unless explicitly configured.
const causalFunnelRecorder = createCausalFunnelRecorder(config);
const trackerForwarder = createTrackerForwarder(primaryRadar.config, primaryRadar.source);
// Receiver-independent policy comes from Settings. A burst of simultaneously
// eligible aircraft still coalesces into one bounded physical recording.
const captureTriggerQueue = createCaptureTriggerQueue(captureTriggerOptions(process.env, config));
const recordingLibrary = new RecordingLibrary(() => config);
const recordingTimeline = new RecordingTimeline();
const {recordingReplayTopology} = require('./recording-replay-topology');
function recordingReplayProfile() {
  const device = config.capture?.device || {};
  const clone = value => value === undefined ? undefined : JSON.parse(JSON.stringify(value));
  const topology = recordingReplayTopology(config);
  const processProfile = clone(config.process || {});
  // Replay should reproduce the scientific pipeline, but local acceleration,
  // output files and background calibration/publication remain properties of
  // the machine doing the replay.
  delete processProfile.performance;
  if (processProfile.tracker) delete processProfile.tracker.publication;
  if (processProfile.surveillance_fusion) delete processProfile.surveillance_fusion.manifold_file;
  if (processProfile.bearing) delete processProfile.bearing.manifold_file;
  if (processProfile.bearing) delete processProfile.bearing.manifold_recorder;
  const illuminators = clone(config.process?.multi_illuminator?.illuminators || [])
    .filter(item => item?.enabled === true);
  return {schema: 'vectorwarp-replay-profile-v2', configuration_complete: true,
    created_at: Date.now(),
    capture: {sample_rate_hz: config.capture?.fs || null,
      frequency_hz: config.capture?.fc || null,
      ...topology,
      source_receiver: device.type || null,
      array_geometry: clone(device.array_geometry) || null},
    process: processProfile,
    multi_illuminator: {enabled: config.process?.multi_illuminator?.enabled === true,
      illuminators, timeline_semantics: 'per-illuminator-truth-derived-products-v3',
      map_storage: 'regenerated-from-full-passband-iq'},
    location: clone(config.location) || null};
}
const CLIP_PRE_ROLL_UNAVAILABLE = 'IQ clip pre-roll is unavailable';
const LEGACY_CLIP_PRE_ROLL_UNAVAILABLE = 'Kraken clip pre-roll is unavailable';
const clipPreRollUnavailable = value => value === CLIP_PRE_ROLL_UNAVAILABLE ||
  value === LEGACY_CLIP_PRE_ROLL_UNAVAILABLE;
// A failed native request can reset the bounded raw ring.  Give it materially
// more than the configured two-second pre-roll before issuing the one retry.
const CLIP_PRE_ROLL_RETRY_DELAY_MS = 5000;
// Exact-CPI truth is intentionally delayed by four CPIs, and the final CPI may
// still need the next ADS-B observation.  This is bounded, but must cover both.
const CAPTURE_TRUTH_SETTLE_MS = 8000;
const CAPTURE_TRUTH_SETTLE_POLL_MS = 250;
function reserveCaptureTimeline(result) {
  const clip = result?.clip;
  if (clip?.pending === true && Number.isFinite(clip.windowStart) &&
      Number.isFinite(clip.windowEnd))
    recordingTimeline.reserve(clip.windowStart, clip.windowEnd);
  return result;
}
function automaticRecorderReadiness(now = Date.now()) {
  const processor = radarSession.currentProcessor();
  return automaticCaptureReadiness({processor,
    radarFrameFresh: radarSession.fresh('timestamp',
      Math.max(10000, (config.process?.data?.cpi || 1) * 3000), now)});
}
function admitAutomaticCapture(admit) {
  const readiness = automaticRecorderReadiness();
  if (!readiness.ready)
    return {accepted: false, deferred: true, reason: readiness.reason};
  if (!recordingLibrary.canRecord().allowed)
    return {accepted: false, reason: 'recording-storage-floor'};
  return reserveCaptureTimeline(admit());
}
const automaticCaptureCallbacks = {
  onEligibilityEvent: event => admitAutomaticCapture(() =>
    captureTriggerQueue.acceptEligibilityEvent(event, Date.now())),
  onPeriodicCapture: (event, timestamp) => admitAutomaticCapture(() =>
    captureTriggerQueue.acceptPeriodicSnapshot(event, timestamp)),
  onTrackCapture: (event, timestamp) => admitAutomaticCapture(() =>
    captureTriggerQueue.acceptTrackSnapshot(event, timestamp)),
  onUnmatchedMotionCapture: (event, timestamp) => admitAutomaticCapture(() =>
    captureTriggerQueue.acceptUnmatchedMotionEvent(event, timestamp))
};
const {exact: evidenceStatistics, operational: provisionalEvidenceStatistics} =
  localRadar ? createEvidenceStatisticsLanes(primaryRadar.config, primaryRadar.source, {operational: {
    filename: defaultProvisionalEvidenceStatisticsPath(primaryRadar.config),
    // Per-lane statistics own capture callbacks in multi-illuminator mode.
    ...(primaryRadar.illuminator ? {} : automaticCaptureCallbacks)}}) : {};
const captureEvidenceStatistics = provisionalEvidenceStatistics;
const multiIlluminatorEvidence = localRadar ? new MultiIlluminatorEvidence(config, adsbSource,
  {dependencies: automaticCaptureCallbacks}) : null;
const captureAdmissionEvidence = multiIlluminatorEvidence?.enabled ?
  multiIlluminatorEvidence : captureEvidenceStatistics;
const oprsEventRecorder = createOprsEventRecorder(primaryRadar.config,
  {filename: localRadar ? process.env.BLAH2_OPRS_EVENTS_PATH : null});

async function finalizeCaptureAdmission(revision, file, samples) {
  const records = captureTriggerQueue.eventsForRevision(revision);
  let admissions = captureAdmissionEvidence.prepareCaptureAdmissions(records, revision);
  const truthDeadline = Date.now() + CAPTURE_TRUTH_SETTLE_MS;
  while (admissionsNeedTruthSettlement(admissions) && Date.now() < truthDeadline) {
    await new Promise(resolve => setTimeout(resolve, CAPTURE_TRUTH_SETTLE_POLL_MS));
    admissions = captureAdmissionEvidence.prepareCaptureAdmissions(records, revision);
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
  const unmatchedMotionOnly = records.length > 0 && records.every(record =>
    record.kind === 'unmatched-aircraft-like-motion');
  const artifact = {schema: 'vectorwarp-capture-admission-v1', created_at: Date.now(), revision,
    state: unmatchedMotionOnly && integrity.valid ? 'unmatched-motion' :
      captureArtifactState(integrity, admissions), integrity,
    config_epoch: captureAdmissionEvidence.configEpoch, receiver_epoch: captureAdmissionEvidence.receiverEpoch,
    events: records.map(record => ({event_id: record.eventId, pass_id: record.passId,
      icao: record.hex || null, flight: record.flight || null,
      kind: record.kind || 'adsb-eligible-aircraft', track_id: record.trackId || null,
      similarity_score: record.similarityScore ?? null,
      measured_cpis: record.measuredCpis ?? null,
      duration_seconds: record.durationSeconds ?? null,
      mean_absolute_doppler_hz: record.meanAbsoluteDopplerHz ?? null,
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
  if (file) try {
    await recordingTimeline.writeReplayProfile(file, recordingReplayProfile());
  } catch (error) { console.error(`Recording replay package failed: ${error.message}`); }
  if (file && first) try {
    await recordingTimeline.settle(first.windowStart, first.windowEnd);
    await recordingTimeline.write(file, first.windowStart, first.windowEnd,
      {sample_rate_hz: integrityMetadata?.sampleRate || config.capture?.fs || null,
        frequency_hz: integrityMetadata?.frequency || config.capture?.fc || null,
        channels: integrityMetadata?.channels || config.capture?.device?.channel_count || null,
        replay_profile: recordingReplayProfile()});
  } catch (error) { console.error(`Recording ADS-B timeline failed: ${error.message}`); }
  captureTriggerQueue.setAdmission(revision, admissions, integrity);
  const recordedAdmissions = Object.fromEntries(Object.entries(admissions).map(([id, truth]) =>
    [id, {valid: truth.valid && integrity.valid,
      reasons: [...new Set([...(truth.reasons || []), ...(integrity.reasons || [])])]}]));
  if (multiIlluminatorEvidence.enabled)
    captureAdmissionEvidence.recordCaptureAdmissions(recordedAdmissions, records);
  else captureAdmissionEvidence.recordCaptureAdmissions(recordedAdmissions);
  captureAdmissionEvidence.acceptCaptureEvents(records);
}
if (startupDocument.setupRequired)
  console.error('Setup required:', startupDocument.readError || startupDocument.validation.errors);
const serverId = `${process.pid}-${Date.now()}`;
const releaseBuild = buildMetadata();
// Recorder identity is distinct from actual native connectivity. The status
// endpoint marks it wired only after admitting a native radar-only frame.
const causalFunnelProducer = createCausalFunnelProducer(causalFunnelRecorder, {
  buildId: `${releaseBuild.version}-${releaseBuild.commit}`,
  configFingerprint: crypto.createHash('sha256').update(JSON.stringify(
    config.process?.evidence?.causal_funnel || {})).digest('hex')});
const serviceErrors = new Map();
const ownedPorts = new Set();
let restartState = {state: 'idle'};
let upstreamCache = null;
setReplaySiteStatusProvider(() => radarSession.currentProcessor());
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
const RECEIVER_SYNC_HEADER = 'X-VectorWarp-Receiver-Sync';
const RECEIVER_SYNC_INTENT = 'synchronize-v1';
let gainMonitorStatus = {state: 'starting'};
const gainMonitor = createGainMonitor({
  getConfig: () => readConfig(configFile).config,
  getRevision: () => readConfig(configFile).revision,
  busy: () => configWriteInProgress || gainPublicationBlocked || capture ||
    captureTriggerQueue.status().pending ||
    !radarSession.processorFresh() ||
    startupDocument.revision !== readConfig(configFile).revision ||
    ['running', 'scheduled'].includes(restartState.state),
  isEligible: candidate => automaticGainEnabled(candidate) || directGain.enabled(candidate),
  measureGain: (candidate, options) => {
    if (!directGain.enabled(candidate)) return measureGain(candidate, options);
    const quality = radarSession.processorFresh() ?
      radarSession.processorStatus.value?.receiverHealth?.inputQuality : null;
    if (!quality)
      throw Object.assign(new Error('The live receiver has not published input-quality telemetry.'),
        {code: 'DIRECT_GAIN_TELEMETRY_UNAVAILABLE'});
    return JSON.parse(JSON.stringify(quality));
  },
  validateMeasurement: (measurement, candidate) => directGain.enabled(candidate) ?
    directGain.validateInputQuality(measurement, candidate) :
    validateProbe(measurement, candidate),
  probeSignature: (probe, candidate) => directGain.enabled(candidate) ?
    directGain.signature(probe) : 'all',
  report: value => { gainMonitorStatus = value; },
  onOverload: ({config: candidate, revision, probes}) => new Promise((resolve, reject) => {
    let gainMode = 'backoff';
    if (directGain.enabled(candidate)) {
      const result = directGain.backoff(candidate, probes);
      candidate = result.config; gainMode = 'safety-backoff';
      console.warn(`Automatic input-gain safety backoff: ${JSON.stringify(result.adjustments)}`);
    }
    const body = JSON.stringify(candidate);
    // Reuse the exact same revision-guarded, journaled Apply path. Backoff never
    // raises gain. This request cannot bypass calibration or restart checks.
    const host = HOST === '0.0.0.0' ? '127.0.0.1' : HOST === '::' ? '::1' : HOST;
    const request = http.request({host, port: PORT,
      path: `/api/config?restart=true&gain=${gainMode}`,
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
    (radarSession.processorFresh() && radarSession.processorStatus.value.input === 'replay');
}
function replayTruthStatus() {
  const geographic = geographicContext(config);
  return {enabled: false, online: false, disabledForReplay: true,
    message: geographic.available ? 'This recording does not contain an ADS-B timeline.' : geographic.reason};
}
function recordedReplayTruth(kind, illuminator) {
  const processor = radarSession.currentProcessor();
  if (processor?.input !== 'replay') return {replay: false, value: null};
  if (!geographicContext(config).available) return {replay: true, value: null};
  // A convenience timeline's old delay/Doppler projection is not acquisition
  // site evidence. Reproject its raw evaluation truth using verified sites.
  const aircraft = recordingTimeline.at(processor.file,
    processor.positionSamples, processor.sampleRate, 'aircraft', illuminator);
  if (kind !== 'delayDoppler' || !aircraft) return {replay: true, value: aircraft};
  const enabled = (config.process?.multi_illuminator?.illuminators || []).filter(item => item.enabled);
  const lane = illuminator === undefined ?
    (config.process?.multi_illuminator?.enabled === true ? enabled[0] : null) :
    enabled.find(item => item.id === illuminator);
  const geometry = lane ? recordingIlluminatorAdsbGeometry.get(lane.id) : replayPrimaryAdsbGeometry;
  if (illuminator !== undefined && !lane) return {replay: true, value: null};
  return {replay: true, value: geometry.update(aircraft, {
    frequency_hz: lane?.frequency_hz || processor.frequency,
    sample_rate_hz: lane?.output_sample_rate_hz || processor.sampleRate}).data};
}

let restartCommand = null;
try { restartCommand = restartCommandFromEnvironment(); }
catch (error) { console.error(`Invalid restart lifecycle configuration: ${error.message}`); }

const {encodeMapResponseAsync, presentMap, presentationQuery} = require('./map-presentation');

async function sendDisplayResponse(req, res, frame, query = {}) {
  if (typeof frame !== 'string') frame = JSON.stringify(frame);
  const encoded = await encodeMapResponseAsync(frame, query,
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
function receiverIndependentChange(before, after) {
  const currentRx = before?.location?.rx?.name;
  const currentTx = before?.location?.tx?.name;
  const nextRx = after?.location?.rx?.name;
  const nextTx = after?.location?.tx?.name;
  const normalized = JSON.parse(JSON.stringify(after));
  let changed = false;
  if (currentRx !== nextRx || currentTx !== nextTx) {
    if (normalized.location?.rx) normalized.location.rx.name = currentRx;
    if (normalized.location?.tx) normalized.location.tx.name = currentTx;
    changed = true;
  }
  if (JSON.stringify(before?.truth?.remote_id) !== JSON.stringify(after?.truth?.remote_id)) {
    normalized.truth ||= {};
    if (before?.truth?.remote_id === undefined) delete normalized.truth.remote_id;
    else normalized.truth.remote_id = JSON.parse(JSON.stringify(before.truth.remote_id));
    changed = true;
  }
  if (JSON.stringify(before?.truth?.adsb) !== JSON.stringify(after?.truth?.adsb)) {
    normalized.truth ||= {};
    if (before?.truth?.adsb === undefined) delete normalized.truth.adsb;
    else normalized.truth.adsb = JSON.parse(JSON.stringify(before.truth.adsb));
    changed = true;
  }
  return changed && JSON.stringify(normalized) === JSON.stringify(before);
}
function observerReceiverStateSettled(revision) {
  const status = receiverSyncState?.receipt?.status || receiverSyncState?.state;
  return receiverReconciliationRequired !== true &&
    receiverSyncState?.reconciliationRequired !== true &&
    receiverSyncState?.applicationVerified === true &&
    receiverSyncState?.configRevision === revision &&
    !gainRestart.pending(receiverSyncState) &&
    ['synchronized', 'already-matched', 'not-required'].includes(status);
}
const radarPerformanceHealth = new RadarPerformanceHealth({
  freshnessMs: Math.max(10000, (config.process?.data?.cpi || 1) * 3000)
});
const illuminatorFrames = radarSession;
const multiIlluminatorTrackFuser = new MultiIlluminatorTrackFuser(config);
const bearingHypotheses = new BistaticHypothesisTracker();
const presentationDelayMs = configuredPresentationDelay(config);
const presentationUnavailableReason = presentationDelayMs === null ?
  'Aligned presentation requires a finite positive configured CPI duration.' : null;
const presentationHistories = radarSession.histories;
function requestedPresentationTimestamp(req) {
  if (req.query.frame_timestamp !== undefined) {
    const requested = Number(req.query.frame_timestamp);
    return Number.isSafeInteger(requested) && requested >= 0 ? requested : NaN;
  }
  // Map is the visual presentation clock. Selecting it once prevents requests
  // that arrive on opposite sides of a CPI boundary from mixing adjacent radar
  // frames across heatmap, detection, tracker, timing and ADS-B layers.
  return req.query.presentation === 'aligned' ?
    presentationHistories.map.timestampAt(Date.now() - presentationDelayMs) : null;
}
function presentationFrame(req, name, current) {
  if (req.query.presentation === 'aligned') {
    const requested = requestedPresentationTimestamp(req);
    if (Number.isNaN(requested)) return '';
    if (requested !== null) return presentationHistories[name].exact(requested) ?? '';
  }
  return req.query.presentation !== 'aligned' ? current :
    presentationHistories[name].at(Date.now() - presentationDelayMs) ?? current;
}
function timingPresentationFrame(req) {
  const frame = presentationFrame(req, 'timing', radarSession.products.timing);
  if (!frame) return frame;
  try {
    const parsed = typeof frame === 'string' ? JSON.parse(frame) : frame;
    if (!parsed || typeof parsed !== 'object') return frame;
    const sampleRate = Number(config.capture?.fs);
    const backlogSamples = Array.isArray(parsed.captureBacklogSamples) ?
      parsed.captureBacklogSamples.filter(Number.isFinite) : [];
    const backlog = backlogSamples.length ? Math.max(...backlogSamples) : null;
    const alignedDelay = req.query.presentation === 'aligned' ? presentationDelayMs : 0;
    const age = Number.isFinite(parsed.timestamp) ? Math.max(0, Date.now() - parsed.timestamp) : null;
    return JSON.stringify({...parsed,
      capture_backlog_samples: backlog,
      capture_backlog_ms: backlog !== null && sampleRate > 0 ? backlog / sampleRate * 1000 : null,
      presentation_delay_ms: alignedDelay,
      frame_age_ms: age,
      frame_excess_age_ms: age === null ? null : Math.max(0, age - alignedDelay)
    });
  } catch (_) { return frame; }
}
var capture = false;
var capturePaused = false;
var captureStartedAt = null;
let captureRevision = 0;
let replayControl = {paused: false, seekSample: 0, revision: 0};
let replaySessionFile = '';
const recordingWriteBytesPerSecond = () => portableRecordingBytesPerSecond(config);
const activeRecordingFile = processor => processor?.recording === true ?
  (processor.recordingFile || '') : '';
const recordingDiskStatus = () => recordingLibrary.canRecord({
  writeBytesPerSecond: recordingWriteBytesPerSecond(), safetySeconds: 15});
// Keep the safety check cheap and independent of browser activity. A manual
// recording is asked to stop before it can consume the configured reserve;
// automatic clips are separately rejected before admission.
setInterval(async () => {
  if (capture && manualRecordingExpired(config, captureStartedAt)) {
    capture = false; capturePaused = false; captureRevision += 1; captureStartedAt = null;
    console.error('Recording stopped at the configured maximum manual duration.');
  }
  let disk = recordingDiskStatus();
  if (!disk.allowed && recordingLibrary.retention().lowSpacePolicy === 'delete_oldest') {
    const processor = radarSession.currentProcessor();
    try { await recordingLibrary.cleanup({activeFile: activeRecordingFile(processor)}); }
    catch (error) { console.error(`Low-space clip cleanup failed: ${error.message}`); }
    disk = recordingDiskStatus();
  }
  if (capture && !disk.allowed) {
    capture = false; capturePaused = false; captureRevision += 1; captureStartedAt = null;
    console.error('Recording stopped to preserve the configured free-disk reserve.');
  }
}, 15000).unref();
setInterval(async () => {
  const processor = radarSession.currentProcessor();
  try {
    const result = await recordingLibrary.cleanup({activeFile: activeRecordingFile(processor)});
    if (result.deleted?.length)
      console.error(`Recording retention removed ${result.deleted.length} completed recording(s).`);
  } catch (error) { console.error(`Recording retention failed: ${error.message}`); }
}, 15 * 60 * 1000).unref();

function invalidateTimestampTelemetry() {
  radarSession.invalidate('timestamp');
  gainRestartTimestampAt = null;
}

function currentTimingBackend() {
  try { return JSON.parse(radarSession.products.timing) || {}; }
  catch (_) { return {}; }
}

function currentGpuRuntime(freshness) {
  const now = Date.now();
  const backend = currentTimingBackend();
  const receiving = radarSession.fresh('timestamp', freshness, now);
  // Preserve each native stage's exact-string execution receipt. Capability
  // binding uses it before the legacy numeric fields, which may exceed 2^53.
  return {acceleration: backend.acceleration, clutterAcceleration: backend.clutterAcceleration,
    widebandAcceleration: backend.widebandAcceleration,
    nCpi: backend.nCpi,
    timestamp: backend.timestamp,
    processorGeneration: radarSession.generation('timing'),
    captureSamplePosition: {
      available: backend.captureSamplePositionAvailable === true,
      epoch: backend.captureEpoch,
      firstSample: backend.captureFirstEpochSample,
      endSample: backend.captureEndEpochSample,
    },
    fresh: receiving && radarSession.fresh('timing', freshness, now)};
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
      restartReceipt: readSdrplayStartup(document.revision), processorStatus: radarSession.processorStatus,
      timestampReceivedAt: gainRestartTimestampAt,
      timestampMaxAgeMs: Math.max(10000, (config.process?.data?.cpi || 1) * 3000)},
    value => receiverJournal.write(value));
    if (next) {
      receiverSyncState = next;
      gainPublicationBlocked = false;
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
const federationNodeId = process.env.VECTORWARP_NODE_ID ||
  os.hostname().toLowerCase().replace(/[^a-z0-9_-]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 32) ||
  'vectorwarp-node';
const nodeFederation = installNodeFederation(app, undefined, {config});
app.options('*', (req, res) => {
  if (['/api/config', '/api/config/validate', '/capture/toggle'].includes(req.path) && !configWriteOriginAllowed(req))
    return res.status(403).json({ok: false,
      errors: ['Configuration changes must come from this VectorWarp host.']});
  res.sendStatus(204);
});
app.use((req, res, next) => {
  if (!localRadar && ['/api/association/passes', '/api/live-scorecard',
    '/api/live-scorecard/provisional', '/api/live-scorecard/association-basis',
    '/api/live-aircraft-associations', '/api/suspicious-track-ledger',
    '/api/radar/coverage', '/api/processor/status', '/capture/request',
    '/capture/toggle', '/capture/pause'].includes(req.path))
    return res.status(409).json({available: false, node_role: 'coordinator',
      reason: 'This node coordinates radar nodes; select a connected radar node.',
      statistics_url: '/api/federation/statistics-summary'});
  if (retiredPublicationError && /^\/api\/(radar|map|detection|tracker|bearing|locations|illuminators)(\/|$)/.test(req.path))
    return res.status(503).json({available: false, data: [], reason: retiredPublicationError});
  if (req.query.presentation === 'aligned' && presentationDelayMs === null)
    return res.status(503).json({available: false, data: [],
      reason: presentationUnavailableReason});
  if ((gainPublicationBlocked || retiredPublicationError) && ['/api/map', '/api/radar/frame', '/api/detection', '/api/tracker',
    '/api/radar/observations',
    '/api/illuminators/live',
    '/api/tracker/native', '/api/bearing', '/api/locations', '/api/iqdata'].includes(req.path))
    return res.status(503).json({available: false, data: [],
      reason: retiredPublicationError || 'Receiver gain is being checked or a verified restart is pending.'});
  next();
});
app.get('/api/radar/observations', (_req, res) => {
  res.set('X-VectorWarp-Node-Id', federationNodeId).json(buildObservationBundle({
    nodeId: federationNodeId, serverSessionId: serverId,
    configRevision: startupDocument.revision, config, illuminatorFrames,
    primaryTracks: radarSession.products.tracker}));
});

function configFileWritable() {
  return writable(configFile);
}

function restartCommandAvailable() {
  return commandAvailable(restartCommand);
}

function configuredReceiverTypes() {
  return compiledReceiverTypes();
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
  radarSession.restart();
  bearingHypotheses.reset();
  gainRestartTimestampAt = null;
  restartState = {...restartState, state: 'running', startedAt: Date.now(),
    timestampConnections: radarSession.timestampConnections};
  launchLifecycleRestart(restartCommand, {onComplete(result) {
    restartState = {...restartState, state: result.ok ? 'command-complete' : 'failed',
      message: result.ok ? 'Restart request accepted. Waiting for a new radar connection.' :
        result.timeout ? 'Restart request outcome is unknown after 30 seconds. Check the service manager before retrying.' :
        result.error ? `Restart could not start: ${result.error.message}` :
        `Restart request failed (${result.signal || `exit ${result.code}`}). ${result.diagnostic || 'Check the local service manager.'}`};
  }});
}

app.get('/', (req, res, next) => {
  if (readConfig(configFile).setupRequired &&
      fs.existsSync(path.join(__dirname, '..', 'html', 'display', 'configuration', 'index.html')))
    return res.redirect(302, '/display/configuration?setup=1');
  if (fs.existsSync(path.join(__dirname, '..', 'html', 'index.html'))) return next();
  res.json({service: 'VectorWarp API', settings: '/api/config', status: '/api/system/status'});
});
function selectedIlluminator(req) {
  if (req.query.illuminator === undefined) return null;
  const id = String(req.query.illuminator);
  const configured = (config.process?.multi_illuminator?.illuminators || [])
    .some(item => item?.enabled === true && item.id === id);
  if (!configured) return undefined;
  const exact = {};
  for (const [query, field] of [['frame_timestamp', 'timestamp'],
    ['first_input_sample', 'firstInputSample'],
    ['end_input_sample', 'endInputSample']]) if (req.query[query] !== undefined) {
      const value = Number(req.query[query]);
      if (!Number.isSafeInteger(value) || value < 0) return undefined;
      exact[field] = value;
    }
  if (Object.keys(exact).length) return illuminatorFrames.exact(id, exact) || undefined;
  const live = illuminatorFrames.latest(id);
  if (live) return live;
  const processor = radarSession.currentProcessor();
  if (processor?.input !== 'replay') return undefined;
  const at = kind => recordingTimeline.at(processor.file, processor.positionSamples,
    processor.sampleRate, kind, id);
  const recorded = {id, map: at('map'), detections: at('detections'), tracks: at('tracks')};
  return recorded.map || recorded.detections || recorded.tracks ? recorded : undefined;
}
app.get('/api/illuminators/live', (req, res) => {
  const geographic = geographicContext(config);
  const resolved = geographicConfig(config);
  const multi=config.process?.multi_illuminator||{};
  const configured = multi.enabled === true ? (resolved.process?.multi_illuminator?.illuminators || [])
    .filter(item => item?.enabled === true) : localRadar ? [{id: 'primary',
      standard: 'Single channel', frequency_hz: resolved.capture?.fc,
      output_sample_rate_hz: resolved.capture?.fs, transmitter: resolved.location?.tx}] : [];
  const primary = multi.enabled === true ? null : JSON.parse(radarSession.products.tracker || 'null');
  const radius=Number.isFinite(Number(multi.co_location_radius_m))?Number(multi.co_location_radius_m)/1000:.1;
  const catalog=siteCatalog(configured,resolved.location?.rx,radius,
    multi.transmitter_site_grouping||'automatic');
  res.json({enabled: configured.length > 0,
    geographic_provenance: geographic,
    primary: configured[0]?.id || null,
    illuminators: configured.map(item => {
      const live = multi.enabled === true ? illuminatorFrames.latest(item.id) : primary;
      return {id: item.id, standard: item.standard, api_illuminator: multi.enabled === true,
        transmitter: item.transmitter,
        transmitter_site: catalog.get(item.id)?.identity || null,
        frequency_hz: item.frequency_hz,
        occupied_bandwidth_hz: item.occupied_bandwidth_hz,
        output_sample_rate_hz: item.output_sample_rate_hz,
        // Never rewrite legacy/unknown frame provenance from today's config.
        doppler_window: live ? (live.doppler_window ?? 'unknown') : null,
        configured_doppler_window:
          config.process?.multi_illuminator?.doppler_window ?? 'rectangular',
        input_roles: live?.input_roles ?? null,
        atsc_reference_mode: item.atsc_reference_mode ||
          (['ATSC', 'ATSC-1.0', 'ATSC-3.0'].includes(item.standard) ?
            'automatic' : 'raw'),
        atsc_reference: live?.atsc_reference ?? null,
        input_processing: live?.input_processing ?? null,
        acceleration: live?.acceleration ?? null,
        clutter_acceleration: live?.clutterAcceleration ?? null,
        tracker_birth: live?.tracker_birth ?? null,
        tracker_diagnostics: live?.trackerDiagnostics ?? null,
        ambiguity_gpu_executed: live?.ambiguityGpuExecuted ?? null,
        ambiguity_cpu_executed: live?.ambiguityCpuExecuted ?? null,
        clutter_gpu_executed: live?.clutterGpuExecuted ?? null,
        clutter_cpu_executed: live?.clutterCpuExecuted ?? null,
        available: Boolean(live) && (multi.enabled === true || radarSession.fresh('tracker',
          Math.max(10000, (config.process?.data?.cpi || 1) * 3000))),
        first_input_sample: live?.first_input_sample ?? null,
        end_input_sample: live?.end_input_sample ?? null,
        timestamp: live?.timestamp ?? null};
    })});
});
// One retained processor frame owns the display map, overlays and lineage;
// independent browser requests previously raced adjacent CPIs.
app.get('/api/radar/frame', async (req, res, next) => {
  if (req.query.illuminator === undefined)
    return res.status(400).json({available: false, reason: 'Select an illuminator.'});
  const selected = selectedIlluminator(req);
  if (selected === undefined) return res.status(404).json({available: false,
    reason: 'Unknown or not-yet-live illuminator.'});
  const query = presentationQuery(req);
  query.max_rows ||= '384'; query.max_cols ||= '512';
  const display = selected.presentation_frame || selected;
  const mapFrame = presentMap(display.map_json || selected.map_json || selected.map, query);
  try {
    await sendDisplayResponse(req, res, {schema: 'vectorwarp-radar-display-frame-v1',
      timestamp: display.timestamp, first_input_sample: display.first_input_sample,
      end_input_sample: display.end_input_sample, map_timestamp: mapFrame?.timestamp ?? display.timestamp,
      map: mapFrame, detections: display.detections, tracks: display.tracks});
  } catch (error) { next(error); }
});
for (const [route, name, field] of [['/api/map', 'map', 'map'],
  ['/api/detection', 'detection', 'detections'], ['/api/tracker', 'tracker', 'tracks']])
app.get(route, async (req, res, next) => {
  try {
    const selected = selectedIlluminator(req);
    if (selected === undefined) return res.status(404).json({available: false,
      reason: 'Unknown or not-yet-live illuminator.'});
    const frame = selected ? (name === 'map' &&
      (selected.presentation_frame?.map_json || selected.map_json) || JSON.stringify(selected[field])) :
      presentationFrame(req, name, radarSession.products[name]);
    if (name === 'map') await sendDisplayResponse(req, res, frame, presentationQuery(req));
    else res.type('application/json').send(frame);
  }
  catch (error) { next(error); }
});
app.get('/api/tracker/fused', async (req, res) => {
  const geographic = geographicContext(config);
  if (!geographic.available) return res.json({available: false,
    operational_localization: false, reason: geographic.reason, hypotheses: [], tracks: []});
  const ids = (config.process?.multi_illuminator?.illuminators || [])
    .filter(item => item?.enabled === true).map(item => item.id);
  const federated = nodeFederation?.geographicFusionSnapshot?.() || null;
  const localEnabled = config.process?.multi_illuminator?.enabled === true && ids.length >= 2;
  const localFused = localEnabled ?
    multiIlluminatorTrackFuser.snapshot() : null;
  const useFederated = federated?.available === true &&
    federated?.federation?.external_source_contributing === true;
  const fused = useFederated ? federated : localFused;
  if (!fused && !localEnabled)
    return res.json({schema: 'vectorwarp-multi-illuminator-track-fusion-v1',
      available: false, reason: 'Multi-illuminator tracking is not enabled.', hypotheses: []});
  if (!fused)
    return res.json({schema: 'vectorwarp-multi-illuminator-track-fusion-v1',
      available: false, reason: 'Waiting for an aligned raw-capture cohort.', hypotheses: []});
  // Fusion advances with captured cohorts, never with browser polling.
  let validation = null;
  if (fused.available && Number.isFinite(fused.timestamp) &&
      config.truth?.adsb?.enabled === true) try {
    const truth = typeof adsbSource.getPresentationAt === 'function' ?
      await adsbSource.getPresentationAt('aircraft', fused.timestamp) :
      await adsbSource.get('aircraft');
    validation = validateFusionAgainstAdsb(fused, truth);
  } catch (error) {
    validation = {schema: 'vectorwarp-multi-illuminator-fusion-adsb-validation-v1',
      held_out_truth: true, adsb_used_by_solver: false, available: false,
      error: error.message};
  }
  res.json({...fused, validation});
});
app.get('/api/tracker/native', (req, res) => {
  res.send(radarSession.products.tracker);
});
app.get('/api/evidence/causal-funnel/status', (req, res) => {
  const status=causalFunnelRecorder.status();
  res.json({...status, producer: {...causalFunnelProducer.identity, wired: status.native_received > 0}});
});
app.get('/api/statistics', async (req, res, next) => {
  try {
  // Operational statistics share the association-history lane on ordinary
  // SDRs. Exact RF timing remains an explicitly requested evaluation view.
  const timing = req.query.timing === 'exact' ? 'exact' : 'provisional';
  if (multiIlluminatorEvidence.enabled) {
    const value = multiIlluminatorEvidence.response('statistics', {
      illuminator: req.query.illuminator, timing});
    if (!value) return res.status(404).json({available: false, reason: 'Unknown illuminator.'});
    return await sendDisplayResponse(req, res, value);
  }
  const recorder = manifoldRecorder.status();
  const statistics = timing === 'exact' ? evidenceStatistics : provisionalEvidenceStatistics;
  await sendDisplayResponse(req, res, statistics.status({enabled: config.process?.bearing?.enable === true,
    calibrated: Boolean(config.process?.bearing?.manifold_file),
    manifold_file: config.process?.bearing?.manifold_file || null,
    recorder}));
  } catch (error) { next(error); }
});
app.get('/api/association/passes', async (req, res, next) => {
  try {
    const timing = req.query.timing === 'exact' ? 'exact' : 'provisional';
    const durationMs = 24 * 60 * 60 * 1000;
    const federationTransport = req.query.transport === 'federation-v1';
    const passes = multiIlluminatorEvidence.enabled ?
      multiIlluminatorEvidence.associationPasses({timing, durationMs,
        federation: federationTransport}) :
      (timing === 'exact' ? evidenceStatistics : provisionalEvidenceStatistics)
        .associationPasses({durationMs, federation: federationTransport})
        .map(pass => ({...pass, channel: 'primary'}));
    const responsePasses = federationTransport ? null :
      String(req.query.compact) === 'true' ? passes.map(compactAssociationPass) : passes;
    const statisticsWindow = multiIlluminatorEvidence.enabled ?
      multiIlluminatorEvidence.associationWindow({timing, durationMs}) :
      (timing === 'exact' ? evidenceStatistics : provisionalEvidenceStatistics).associationWindow(durationMs);
    await sendDisplayResponse(req, res, {schema: 'vectorwarp-association-passes-v1',
      scope: 'node-all-channels', timing_policy: timing,
      window: 'past-24-hours-finalized-passes', statistics_window: statisticsWindow,
      ...(federationTransport ? {transport: federationAssociationPassTransport(passes)} :
        {passes: responsePasses})});
  } catch (error) { next(error); }
});
app.get('/api/live-scorecard', async (req, res, next) => {
  try {
  if (multiIlluminatorEvidence.enabled) {
    const value = multiIlluminatorEvidence.response('scorecard', {
      illuminator: req.query.illuminator, timing: 'exact'});
    if (!value) return res.status(404).json({available: false, reason: 'Unknown illuminator.'});
    return await sendDisplayResponse(req, res, value);
  }
  await sendDisplayResponse(req, res, evidenceStatistics.liveStatus());
  } catch (error) { next(error); }
});
app.get('/api/live-scorecard/provisional', async (req, res, next) => {
  try {
  if (multiIlluminatorEvidence.enabled) {
    const value = multiIlluminatorEvidence.response('scorecard', {
      illuminator: req.query.illuminator, timing: 'provisional'});
    if (!value) return res.status(404).json({available: false, reason: 'Unknown illuminator.'});
    return await sendDisplayResponse(req, res, value);
  }
  await sendDisplayResponse(req, res, provisionalEvidenceStatistics.liveStatus());
  } catch (error) { next(error); }
});
// Small live identity feed for map markers.  The full scorecard is intentionally
// large and should not be polled by a visualization every few seconds.
app.get('/api/live-aircraft-associations', async (_req, res, next) => {
  try {
    const channels = multiIlluminatorEvidence.enabled ?
      multiIlluminatorEvidence.activeAssociations({timing: 'provisional'}) :
      [{selected_channel: 'primary',
        active_passes: provisionalEvidenceStatistics.activeAssociations()}];
    const byAircraft = new Map();
    for (const channel of channels) for (const pass of channel.active_passes || []) {
      const hex = String(pass?.hex || '').trim().toLowerCase();
      if (!hex) continue;
      const tracked = Number(pass.tracked_cpis) >= 3 ||
        String(pass.status || '').includes('tracked');
      const detected = tracked || Number(pass.detected_cpis) >= 3 ||
        String(pass.status || '').includes('detected');
      if (!detected) continue;
      const current = byAircraft.get(hex) || {hex, flight: pass.flight || '',
        detected: false, tracked: false, channels: [], detected_cpis: 0, tracked_cpis: 0,
        last_at: 0};
      current.detected = true;
      current.tracked ||= tracked;
      current.flight ||= pass.flight || '';
      current.detected_cpis += Number(pass.detected_cpis) || 0;
      current.tracked_cpis += Number(pass.tracked_cpis) || 0;
      current.last_at = Math.max(current.last_at, Number(pass.last_at) || 0);
      if (channel.selected_channel && !current.channels.includes(channel.selected_channel))
        current.channels.push(channel.selected_channel);
      byAircraft.set(hex, current);
    }
    res.json({schema: 'vectorwarp-live-aircraft-associations-v1',
      timestamp: Date.now(), associations: [...byAircraft.values()]});
  } catch (error) { next(error); }
});
app.get('/api/live-scorecard/association-basis', async (req, res, next) => {
  try {
    const timing = req.query.timing === 'exact' ? 'exact' : 'provisional';
    const compact = String(req.query.compact) === 'true';
    const kiosk = req.query.display === 'kiosk';
    if (multiIlluminatorEvidence.enabled)
      return await sendDisplayResponse(req, res,
        multiIlluminatorEvidence.associationBasis({timing, compact, kiosk}));
    const statistics = timing === 'exact' ? evidenceStatistics :
      provisionalEvidenceStatistics;
    const status = statistics.liveStatus();
    if (compact)
      return await sendDisplayResponse(req, res, {
        schema: 'vectorwarp-association-basis-v1', timing_policy: timing,
        channels: [{selected_channel: 'primary', updated_at: status.updated_at,
          association_statistics: associationStatistics(status)}]});
    if (kiosk) return await sendDisplayResponse(req, res, {
      schema: 'vectorwarp-association-basis-v1', timing_policy: timing,
      channels: [kioskAssociationChannel({selected_channel: 'primary',
        updated_at: status.updated_at, association_statistics: associationStatistics(status),
        measured_track_coverage: status.current_setup_lifetime?.measured_track_coverage,
        cpi_exposure: status.current_setup_lifetime?.cpi_exposure})]});
    const population = status.track_population || {};
    const channel = {selected_channel: 'primary',
        measured_track_basis: statistics.measuredTrackBasis(),
        updated_at: status.updated_at,
        association_statistics: associationStatistics(status),
        track_population: {...population,
          aircraft_like_candidates: (population.aircraft_like_candidates || []).slice(0, 10)},
        passes: typeof statistics.associationPasses === 'function' ?
          statistics.associationPasses() : []};
    await sendDisplayResponse(req, res, {schema: 'vectorwarp-association-basis-v1',
      timing_policy: timing, channels: [channel]});
  } catch (error) { next(error); }
});
app.get('/api/suspicious-track-ledger', async (req, res, next) => {
  try {
    if (!multiIlluminatorEvidence.enabled)
      return res.json({schema: 'vectorwarp-suspicious-track-ledger-v1', available: false,
        reason: 'Multi-illuminator tracking is not enabled.'});
    await sendDisplayResponse(req, res, multiIlluminatorEvidence.suspiciousTracks({
      includeSamples: req.query.samples === 'true'}));
  } catch (error) { next(error); }
});
function recordingPasses() {
  if (!localRadar) return [];
  const scorecard = provisionalEvidenceStatistics.liveStatus();
  const subjects = [...captureEvidenceStatistics.recordingSubjects(),
    ...(scorecard.active_passes || []), ...(scorecard.recent_finalized_passes || [])];
  return [...new Map(subjects.filter(item => item?.pass_id)
    .map(item => [item.pass_id, item])).values()];
}
installRecordingLibraryRoutes(app, {recordingLibrary, recordingTimeline, recordingPasses,
  activeRecordingFile,
  currentProcessor: () => radarSession.currentProcessor(),
  configWriteOriginAllowed});
app.get('/api/tracker-forward', (req, res) => {
  res.json(trackerForwarder.status());
});
app.get('/api/bearing', (req, res) => {
  if (config.process?.bearing?.enable !== true)
    return res.json({enabled: false, reason: 'Bearing processing is disabled.', data: []});
  if (!radarSession.products.bearing) return res.status(503).json({enabled: true, available: false,
    reason: 'No bearing frame has arrived.', data: []});
  res.type('json').send(presentationFrame(req, 'bearing', radarSession.products.bearing));
});
app.get('/api/locations', (req, res) => {
  let frame = null;
  const presentedBearing = presentationFrame(req, 'bearing', radarSession.products.bearing);
  try { frame = presentedBearing ? JSON.parse(presentedBearing) : null; }
  catch (_) { return res.status(503).json({enabled: true, available: false,
    reason: 'The current bearing frame is invalid.', data: []}); }
  const result = locationsFromBearing(frame, config, bearingHypotheses);
  res.status(result.enabled && !result.available ? 503 : 200).json(result);
});
app.get('/api/timestamp', (req, res) => {
  res.send(radarSession.products.timestamp);
});
app.get('/api/timing', (req, res) => {
  res.type('json').send(timingPresentationFrame(req));
});
app.get('/api/presentation/status', (req, res) => {
  const now = Date.now();
  const expectedIntervalMs = Number.isFinite(config.process?.data?.cpi) && config.process.data.cpi > 0 ?
    config.process.data.cpi * 1000 : null;
  const selectedTimestamp = presentationDelayMs === null ? null :
    presentationHistories.map.timestampAt(now - presentationDelayMs);
  const aligned = selectedTimestamp === null ? {} : Object.fromEntries(
    Object.entries(presentationHistories).map(([name, history]) =>
      [name, history.exact(selectedTimestamp) !== null]));
  const latest = currentTimingBackend();
  res.json({
    aligned_available: presentationDelayMs !== null,
    reason: presentationUnavailableReason,
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
  res.send(presentationFrame(req, 'iqdata', radarSession.products.iqdata));
});
app.get('/api/config', (req, res) => {
  const document = readConfig(configFile);
  res.set('ETag', `"${document.revision}"`).json(document.config);
});
app.get('/api/config/migrations', (req, res) => {
  const document = readConfig(configFile), evidence = readConfigMigrations(configFile, document.revision);
  res.json({schema: 'vectorwarp-config-migrations-v1', ...evidence,
    radarPlanReview: document.radarPlanReview,
    error: evidence.configMigrationError});
});
app.get('/api/runtime/config', (req, res) => {
  // API startup snapshot, not hardware acknowledgement. Do not relabel frames
  // with newly saved tuning/site settings until the services have restarted.
  // Display preferences apply on save; processing and site identity stay pinned.
  res.json({...config, display: readConfig(configFile).config.display});
});
app.get('/api/runtime/plan', (req, res) => {
  const document = readConfig(configFile);
  const loaded = checkRadarPlan(config, configFile), observation = radarSession.planObservation();
  res.json({loaded, saved: document.radarPlan, observation,
    execution: assessResolvedRadarPlans(loaded, observation),
    loadedRevision: startupDocument.revision, savedRevision: document.revision,
    restartRequired: startupDocument.revision !== document.revision &&
      !displayOnlyChange(startupDocument.config, document.config)});
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
  // Finish asynchronous probes before sampling the current producer. A restart
  // or reconnect while a probe is pending must invalidate the old frame here.
  const [nativeReport, driverStatus, localRspLive] = await Promise.all([
    readNativeProcessingCapabilities(), readGpuDriverStatus(), localRspCurrent()]);
  const runtimeGpu = currentGpuRuntime(Math.max(10000,
    (document.config.process?.data?.cpi || 1) * 3000));
  const nativeProcessing = enrichNativeProcessingCapabilities(nativeReport, {
      runtime: runtimeGpu.fresh ? runtimeGpu : {},
      requested: document.config.process?.performance?.acceleration || '', driverStatus});
  // Resident-only controls need eligible hardware or current frame-bound
  // execution, not merely a compiled module. Enumeration is not qualification.
  const residentSelectable = nativeProcessing.stages?.resident?.selectable === true;
  // A current local build grants only RSPduo live eligibility. It remains
  // distinct from `compiledLiveTypes`: this adapter was not shipped compiled.
  res.json({
    editable: configFileWritable(),
    restartAvailable: restartCommandAvailable(),
    maximumConfigBytes: 262144,
    changesRequireRestart: true,
    // Replay validates all file/profile combinations without loading receiver
    // SDKs. Keep every profile editable, while accurately exposing live support.
    deviceProfiles: getDeviceProfiles().map(item => ({...item,
      features: {...item.features,
        residentIntegration: item.features?.residentIntegration === true &&
          residentSelectable,
        bearing: item.features?.bearing === true &&
          residentSelectable},
      liveAvailable: !compiledLiveTypes || compiledLiveTypes.includes(item.type) ||
        (item.type === 'RspDuo' && localRspLive)})),
    processingBackends: nativeProcessing,
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
    configMigrations: document.configMigrations,
    configMigrationError: document.configMigrationError,
    configMigrationOmitted: document.configMigrationOmitted,
    radarPlanReview: document.radarPlanReview,
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
  const processor = radarSession.currentProcessor();
  const baseRadar = gainPublicationBlocked || retiredPublicationError || radarSession.lastFrameAt === null ? 'no-data' :
    radarSession.fresh('timestamp', freshness) ? 'receiving' : 'stale';
  const inputQuality = processor?.receiverHealth?.inputQuality || null;
  const inputQualityReady = inputQuality?.channels?.every(channel =>
    /^\d+$/.test(channel.sampledComponents || '') && BigInt(channel.sampledComponents) >= 65536n);
  const rfInputDegraded = inputQualityReady && inputQuality.safe === false;
  radarPerformanceHealth.expect(config.process?.multi_illuminator?.enabled === true ?
    (config.process.multi_illuminator.illuminators || [])
      .filter(item => item?.enabled === true).map(item => item.id) : ['primary']);
  const trackingPerformance = radarPerformanceHealth.snapshot();
  const evidenceHealth = multiIlluminatorEvidence.enabled ? multiIlluminatorEvidence.health() : (() => {
    const errors = [evidenceStatistics?.state?.error, provisionalEvidenceStatistics?.state?.error]
      .filter(value => typeof value === 'string' && value);
    return {status: errors.length ? 'degraded' : 'healthy', channels: [
      {id: 'primary', status: errors.length ? 'degraded' : 'healthy', errors}]};
  })();
  const radar = baseRadar === 'receiving' &&
    (rfInputDegraded || trackingPerformance.status === 'degraded' ||
      evidenceHealth.status === 'degraded') ? 'degraded' : baseRadar;
  // The header is present on every page and polls continuously. Keep its
  // response cheap: receiver discovery, GPU setup, build metadata and the full
  // ADS-B source report belong to their dedicated/settings views.
  const document = readConfig(configFile);
  if (req.query.summary === '1') {
    const headerProcessor = processor ? {
      input: processor.input, state: processor.state, error: processor.error || '',
      receiver: processor.receiver, receiverHealth: processor.receiverHealth,
      totalSamples: processor.totalSamples, positionSamples: processor.positionSamples,
      sampleRate: processor.sampleRate, file: processor.file
    } : null;
    return res.json({radar, node_role: nodeRole(), rfInputQuality: inputQuality, trackingPerformance,
      evidenceHealth,
      configMigrations: document.configMigrations,
      configMigrationError: document.configMigrationError,
      configMigrationOmitted: document.configMigrationOmitted,
      radarPlanReview: document.radarPlanReview,
      adsbEnabled: config.truth?.adsb?.enabled === true,
      remoteIdEnabled: config.truth?.remote_id?.enabled === true,
      recordedAdsb: processor?.input === 'replay' && recordingTimeline.has(processor.file),
      restart: {state: restartState.state}, gainCheckPending: gainPublicationBlocked,
      processor: headerProcessor, processorFresh: processor !== null,
      errors: [...serviceErrors.values()],
      message: radarSession.lastFrameAt === null ? 'No radar frames received.' : null});
  }
  const sdrplayStartup = activeSdrplayStartup(document.revision, document.config.capture?.device?.type);
  const persistedRestart = sdrplayStartup && sdrplayStartup.updatedAt >= (restartState.requestedAt || 0) ?
    {state: sdrplayStartup.inProgress ? 'running' : sdrplayStartup.state === 'failed' ? 'failed' : 'command-complete',
      message: sdrplayStartup.message, startedAt: sdrplayStartup.updatedAt} : restartState;
  const [gpuSetup, adsb, remoteId] = await Promise.all([
    gpuSetupStatus(() => currentGpuRuntime(freshness)), adsbSource.status(),
    config.truth?.remote_id?.enabled === true ? remoteIdSource.status() :
      Promise.resolve({enabled: false, online: false, aircraft: [], drones: []})
  ]);
  const backend = currentTimingBackend();
  const sourceRows = Array.isArray(adsb?.sources) ? adsb.sources : [];
  const positionSources = adsb?.fusion?.position_sources || {};
  const featureDiagnostics = {
    trackingPerformance, evidenceStatistics: evidenceHealth,
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
        config.save.retention.minimum_free_gb : null,
      maximumManualSeconds: Number.isSafeInteger(
        config.save?.retention?.maximum_manual_seconds) ?
        config.save.retention.maximum_manual_seconds : 300
    },
    replay: {
      active: processor?.input === 'replay', paused: replayControl.paused,
      seekable: replayControlsAvailable(processor) && Number.isSafeInteger(processor.totalSamples),
      controlsAvailable: replayControlsAvailable(processor),
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
    ...(config.truth?.remote_id?.enabled === true ? {remoteId: {
      enabled: config.truth?.remote_id?.enabled === true,
      online: remoteId?.online === true,
      drones: Number.isSafeInteger(remoteId?.drones) ? remoteId.drones : 0,
      messages: Number.isSafeInteger(remoteId?.messages) ? remoteId.messages : 0,
      bluetooth: remoteId?.transports?.bluetooth === true,
      wifi: remoteId?.transports?.wifi === true,
      ageMs: Number.isFinite(remoteId?.age_ms) ? remoteId.age_ms : null,
      error: remoteId?.error || null
    }} : {}),
    processing: {
      acceleration: backend.acceleration?.active || null,
      clutterAcceleration: backend.clutterAcceleration?.active || null,
      configuredCpiMs: Number.isFinite(config.process?.data?.cpi) ?
        Math.round(config.process.data.cpi * 1000) : null
    },
    presentation: {
      timestampConnections: radarSession.timestampConnections,
      associationLagCpis: Number.isSafeInteger(adsb?.association_lag_cpis) ? adsb.association_lag_cpis : null,
      associationLagMs: Number.isSafeInteger(adsb?.association_lag_ms) ? adsb.association_lag_ms : null
    }
  };
  res.json({serverId, node_role: nodeRole(), configRevision: document.revision,
    configMigrations: document.configMigrations,
    configMigrationError: document.configMigrationError,
    configMigrationOmitted: document.configMigrationOmitted,
    radarPlanReview: document.radarPlanReview,
    build: releaseBuild,
    system: {os: os.platform(), architecture: os.arch(), kernel: os.release()},
    uptimeSeconds: Math.floor(process.uptime()),
    acceleration: backend.acceleration || null,
    clutterAcceleration: backend.clutterAcceleration || null, gpuSetup,
    loadedRevision: startupDocument.revision, setupRequired: document.setupRequired,
    restart: persistedRestart, sdrplayStartup, receiverSynchronization: receiverSyncState,
    gainMonitor: gainMonitorStatus, adsb, remoteId, featureDiagnostics,
    lastFrameAt: radarSession.lastFrameAt,
    timestampConnections: radarSession.timestampConnections,
    adsbEnabled: config.truth?.adsb?.enabled === true,
    remoteIdEnabled: config.truth?.remote_id?.enabled === true,
    gainCheckPending: gainPublicationBlocked,
    radar, rfInputQuality: inputQuality,
    processor: processor && {...processor, receivedAt: radarSession.processorStatus.receivedAt},
    processorFresh: processor !== null,
    errors: [...serviceErrors.values()],
    message: radarSession.lastFrameAt === null ? 'No radar frames received. Check the receiver, processing service and its logs. Settings remain available.' : null});
});
// The processor runs without a browser Origin header. If a browser supplies one,
// retain the same-host restriction used by configuration writes. This telemetry is
// display state only: it never validates radio hardware or permits configuration.
app.post('/api/processor/status', (req, res) => {
  if (!processorStatusOriginAllowed(req))
    return res.status(403).json({ok: false, errors: ['Processor telemetry must come from this VectorWarp host.']});
  const checked = validateProcessorStatus(req.body);
  const previous = radarSession.processorStatus?.value || null;
  radarSession.acceptProcessorStatus(checked, req.body);
  if (!checked.valid) {
    return res.status(422).json({ok: false, errors: checked.errors});
  }
  // Retain expired status for recording finalization across processor restarts.
  const now = radarSession.processorStatus.receivedAt;
  if (checked.value.input === 'replay' && checked.value.file !== replaySessionFile) {
    replaySessionFile = checked.value.file;
    replayControl = {paused: false, seekSample: 0, revision: replayControl.revision + 1};
    radarPerformanceHealth.reset();
  } else if (checked.value.input === 'live' && replaySessionFile) {
    replaySessionFile = '';
    replayControl = {paused: false, seekSample: 0, revision: replayControl.revision + 1};
    radarPerformanceHealth.reset();
  }
  if (previous?.input === 'live' && previous.recording &&
      (!checked.value.recording || checked.value.recordingFile !== previous.recordingFile))
    void recordingTimeline.stop(previous.recordingFile, now)
      .catch(error => console.error(`Manual recording ADS-B timeline failed: ${error.message}`));
  if (checked.value.input === 'live' && checked.value.recording && checked.value.recordingFile) {
    const isNewManualRecording = previous?.recording !== true ||
      previous?.recordingFile !== checked.value.recordingFile;
    const startMs = now - checked.value.recordedSamples * 1000 / checked.value.sampleRate;
    recordingTimeline.start(checked.value.recordingFile, startMs,
      {sample_rate_hz: checked.value.sampleRate, frequency_hz: checked.value.frequency,
        channels: config.capture?.device?.channel_count || 2,
        receiver: checked.value.receiver, replay_profile: recordingReplayProfile()});
    if (isNewManualRecording)
      void recordingTimeline.writeReplayProfile(checked.value.recordingFile, recordingReplayProfile())
        .catch(error => console.error(`Recording replay package failed: ${error.message}`));
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
    if (clipPreRollUnavailable(checked.value.clipError)) {
      setTimeout(() => {
        const retry = captureTriggerQueue.retryPreRollFailure(revision);
        if (!retry)
          captureAdmissionEvidence.acceptCaptureEvents(
            captureTriggerQueue.eventsForRevision(revision));
      }, CLIP_PRE_ROLL_RETRY_DELAY_MS).unref();
    } else if (checked.value.clipError) captureAdmissionEvidence.acceptCaptureEvents(
      captureTriggerQueue.eventsForRevision(revision));
    else void finalizeCaptureAdmission(revision, checked.value.clipFile, checked.value.clipSamples)
      .catch(error => console.error(`Capture admission failed: ${error.message}`));
  }
  res.status(204).end();
});
function cachedUpstreamStatus(document) {
  if (!upstreamCache || upstreamCache.revision !== document.revision ||
      Date.now() - upstreamCache.at > 4000) {
    upstreamCache = {at: Date.now(), revision: document.revision,
      promise: getUpstreamStatus(document.config)};
  }
  return upstreamCache.promise;
}

app.get('/api/upstream/status', async (req, res) => {
  if (process.env.BLAH2_PREVIEW === 'true') return res.json({supported: true,
    available: false, matched: false, host: 'not connected', controlPort: '—',
    message: 'UI preview only. No receiver connection is opened and no live data is generated.'});
  const document = readConfig(configFile);
  const status = await cachedUpstreamStatus(document);
  res.status(status.available === false ? 503 : 200).json(status);
});
app.get('/api/array-channel-health', async (req, res) => {
  const document = readConfig(configFile);
  if (document.config.capture?.device?.type !== 'Kraken') return res.status(404).json({
    schema: 'vectorwarp-array-channel-health-v2', supported: false,
    action: 'diagnostic-only', message: 'Array-channel diagnostics require Kraken telemetry.'});
  const status = process.env.BLAH2_PREVIEW === 'true' ? {available: false} :
    await cachedUpstreamStatus(document);
  try {
    const report = diagnoseUpstreamArrayChannels({config: document.config, upstreamStatus: status,
      configRevision: document.revision,
      loadedConfigRevision: startupDocument.revision,
      configFingerprint: siteFingerprint(document.config)});
    res.status(status.available === false ? 503 : 200).json(report);
  } catch (error) {
    res.status(422).json({schema: 'vectorwarp-array-channel-health-v2',
      action: 'diagnostic-only', error: error.message});
  }
});
const receiverManagement = installReceiverRoutes(app, {
  readDocument: () => readConfig(configFile),
  port: PORT,
  allowedOrigins: receiverOrigins,
  helperExecutable: process.env.BLAH2_RECEIVER_HELPER,
  extraOrigins: (process.env.BLAH2_RECEIVER_ORIGINS || '').split(',').filter(Boolean),
  transactionBusy: () => configWriteInProgress || macRspduoBuild?.busy() || ['running', 'scheduled'].includes(restartState.state) ||
    activeSdrplayStartup(null)?.inProgress === true,
  processorStatus: () => radarSession.currentProcessor(),
  preview: process.env.BLAH2_PREVIEW === 'true',
  compiledLiveTypes: configuredReceiverTypes()
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
  const currentDocument = readConfig(configFile);
  const expectedRevision = req.get('If-Match').replace(/^"|"$/g, '');
  if (expectedRevision !== currentDocument.revision)
    return res.status(409).json({ok: false,
      errors: ['The config file changed. Reload settings before saving; your edits have not been written.']});
  const validation = validateConfig(req.body, currentDocument.config);
  if (!validation.valid)
    return res.status(422).json({ok: false, errors: validation.errors});
  if (!wantsRestart && displayOnlyChange(currentDocument.config, req.body)) {
    const saved = saveConfig(configFile, req.body, currentDocument.revision);
    return res.json({ok: true, restarting: false, displayApplied: true,
      revision: saved.revision, config: saved.config, receiverSync: receiverSyncState.receipt,
      message: 'Display saved.'});
  }
  const allowedTypes = configuredReceiverTypes();
  const replayRequested = req.body?.capture?.replay?.state === true;
  const localRspRequested = !saveLater && !replayRequested &&
    req.body?.capture?.device?.type === 'RspDuo' && allowedTypes && !allowedTypes.includes('RspDuo');
  if (!saveLater && allowedTypes && !replayRequested && !allowedTypes.includes(req.body?.capture?.device?.type) && !localRspRequested)
    return res.status(422).json({ok: false, errors: [
      'This receiver backend is unavailable for live capture in this installation. Use Save for later, or install a build containing this backend before applying it.'
    ]});
  const independentChange = receiverIndependentChange(currentDocument.config, req.body);
  const adsbChange = independentChange &&
    JSON.stringify(currentDocument.config?.truth?.adsb) !== JSON.stringify(req.body?.truth?.adsb);
  // Site labels and Remote ID metadata are immediately API-local. ADS-B owns
  // polling, association history and capture/evidence consumers constructed at
  // API startup, so an ADS-B-only disk save must remain explicitly pending.
  if (!saveLater && !wantsRestart && independentChange) {
    const verifiedPredecessor = observerReceiverStateSettled(currentDocument.revision);
    const saved = saveConfig(configFile, req.body, currentDocument.revision);
    if (adsbChange && receiverReconciliationRequired) {
      receiverSyncState = {...receiverSyncState, configRevision: saved.revision};
      receiverJournal.write(receiverSyncState);
      config = saved.config;
      upstreamCache = null;
      return res.json({ok: true, restarting: false, revision: saved.revision,
        config: saved.config, receiverSync: receiverSyncState.receipt,
        message: 'ADS-B settings saved, but an earlier receiver transaction remains unresolved. Reconcile it before Apply; no receiver command was sent.'});
    }
    const priorReceipt = receiverSyncState.receipt || {schemaVersion: 1,
      receiverType: req.body.capture.device.type, status: 'not-required', operations: []};
    const receipt = {...priorReceipt, configRevision: saved.revision,
      configPersisted: true, metadataOnly: true,
      ...(adsbChange ? {receiverMutationPerformed: false,
        apiRuntimeRestartRequired: true, operations: []} : {})};
    const settledState = !verifiedPredecessor ? receiverSyncState.state :
      ['synchronized', 'already-matched', 'not-required'].includes(
        receiverSyncState.state) ? receiverSyncState.state : receipt.status;
    receiverSyncState = {...receiverSyncState, state: settledState,
      completedAt: Date.now(), receipt, configRevision: saved.revision,
      applicationVerified: verifiedPredecessor,
      reconciliationRequired: verifiedPredecessor ? false :
        receiverSyncState.reconciliationRequired};
    receiverJournal.write(receiverSyncState);
    config = saved.config;
    upstreamCache = null;
    return res.json({ok: true, restarting: false, revision: saved.revision,
      config: saved.config, receiverSync: receipt,
      message: adsbChange ? 'ADS-B settings saved. Restart VectorWarp processing and its API to apply them; no receiver command was sent.' :
        'Presentation settings saved. Live receiver and radar processing were not restarted.'});
  }
  // A diagnostic observer neither changes nor interrogates RF hardware. Do
  // not rerun automatic gain merely because the existing receiver policy is
  // measured; that would confound the observer comparison itself.
  // Receiver-independent settings (ADS-B evaluation, Remote ID metadata, and
  // site labels) must never interrogate or mutate RF hardware.  In particular,
  // an Apply+restart of ADS-B association policy must not stop Kraken to rerun
  // automatic gain merely because the unchanged receiver profile uses measured
  // gain control.
  const measuredGain = !saveLater && !independentChange &&
    automaticGainEnabled(req.body);
  if (measuredGain && !gainRestart.installedGainRestartSupported(restartCommand))
    return res.status(409).json({ok: false, code: 'GAIN_CHECK_RESTART_UNSUPPORTED',
      errors: ['Automatic gain requires the installed Linux receiver restart helper and its verified completion receipt.']});
  if (measuredGain && !wantsRestart)
    return res.status(409).json({ok: false, code: 'GAIN_CHECK_RESTART_REQUIRED',
      errors: ['Automatic gain requires Apply and restart so processing resumes with the verified settings. Use fixed gain for a controlled measurement block.']});
  if (measuredGain && (capture || radarSession.processorStatus?.value?.recording === true))
    return res.status(409).json({ok: false, code: 'GAIN_CHECK_RECORDING_ACTIVE',
      errors: ['Finish the current recording before applying automatic gain.']});
  if (saveLater && receiverReconciliationRequired)
    return res.status(409).json({ok: false, code: 'RECEIVER_RECONCILIATION_REQUIRED',
      receiverSync: receiverSyncState.receipt,
      errors: ['An earlier receiver command has an unresolved outcome. Reconcile it before saving for later; its receipt and saved settings were preserved.']});
  if (adsbChange && receiverReconciliationRequired)
    return res.status(409).json({ok: false, code: 'RECEIVER_RECONCILIATION_REQUIRED',
      receiverSync: receiverSyncState.receipt,
      errors: ['An earlier receiver command has an unresolved outcome. Reconcile it before applying ADS-B settings. No receiver command or restart was requested.']});
  // A previous attempt may have changed Suite without saving YAML, including
  // before this API process started. Never use an unchanged file as readback.
  const observeLiveReceiver = req.body?.capture?.device?.type === 'Kraken' &&
    req.body?.capture?.replay?.state !== true && process.env.BLAH2_PREVIEW !== 'true';
  const receiverSyncRequired = !saveLater && !adsbChange && (observeLiveReceiver ||
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
    if (!adsbChange)
      localKrakenController = await ensureLocalKrakenController(req.body, currentDocument.config);
    if (measuredGain) {
      gainPublicationBlocked = true;
      invalidateTimestampTelemetry();
      gainTransaction = await automaticGain.apply(currentDocument.config, req.body,
        {increase: req.query.gain !== 'backoff'});
      req.body = gainTransaction.config;
      receiverSync = gainTransaction.receiverSync;
    } else if (adsbChange) {
      // ADS-B affects evaluation/runtime consumers only. Preserve the exact
      // verified receiver attestation across the new config revision so the
      // receiver start interlock does not mistake an API restart requirement
      // for an unsettled RF transaction.
      receiverSync = {...previousReceiverReceipt,
        schemaVersion: previousReceiverReceipt?.schemaVersion || 1,
        receiverType: req.body.capture.device.type,
        status: previousReceiverReceipt?.status || 'not-required',
        receiverMutationPerformed: false, apiRuntimeRestartRequired: true,
        operations: []};
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
    if (receiverSyncRequired || startupOwned || adsbChange) receiverReconciliationRequired = false;
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
  const illuminator = req.query.illuminator === undefined ? undefined : String(req.query.illuminator);
  if (illuminator !== undefined && !illuminatorAdsbGeometry.has(illuminator))
    return res.status(404).json({available: false, reason: 'Unknown illuminator.'});
  const query = illuminator === undefined ? '' : `?illuminator=${encodeURIComponent(illuminator)}`;
  if (replayTruthDisabled()) {
    const recorded = recordedReplayTruth('delayDoppler', illuminator);
    return recorded.value === null ? res.json(replayTruthStatus()) :
      res.json({url: `/api/adsb/delay-doppler${query}`, replay: true,
        illuminator: illuminator ?? null});
  }
  if (!config.truth?.adsb?.enabled) return res.status(400).end();
  // Compatibility link now stays on this API; no converter URL or query is accepted.
  res.json({url: `/api/adsb/delay-doppler${query}`, illuminator: illuminator ?? null});
});
for (const [route, kind] of [['/api/adsb', 'aircraft'], ['/api/adsb/delay-doppler', 'delayDoppler']]) {
  app.get(route, async (req, res) => {
    const requestedIlluminator = kind === 'delayDoppler' && req.query.illuminator !== undefined ?
      String(req.query.illuminator) : undefined;
    const recorded = recordedReplayTruth(kind, requestedIlluminator);
    if (recorded.replay && recorded.value === null) return res.json(replayTruthStatus());
    if (!recorded.replay && replayTruthDisabled()) return res.json(replayTruthStatus());
    try {
      const selectedGeometry = kind === 'delayDoppler' && req.query.illuminator !== undefined ?
        illuminatorAdsbGeometry.get(String(req.query.illuminator)) : null;
      if (kind === 'delayDoppler' && req.query.illuminator !== undefined && !selectedGeometry)
        return res.status(404).json({available: false,
          reason: 'Unknown illuminator.'});
      const presentation = req.query.presentation;
      const requestedTimestamp = presentation === 'aligned' ?
        requestedPresentationTimestamp(req) : null;
      if (Number.isNaN(requestedTimestamp))
        return res.status(400).json({error: 'frame_timestamp must be a non-negative integer'});
      const sourceKind = selectedGeometry ? 'aircraft' : kind;
      let value = recorded.replay ? recorded.value : presentation === 'aligned' && typeof adsbSource.getPresentationAt === 'function' ?
        await adsbSource.getPresentationAt(sourceKind,
          requestedTimestamp ?? Date.now() - presentationDelayMs) :
        presentation && typeof adsbSource.getPresentation === 'function' ?
          await adsbSource.getPresentation(sourceKind) : await adsbSource.get(sourceKind);
      if (selectedGeometry && !recorded.replay) value = selectedGeometry.update(value).data;
      if (requestedIlluminator !== undefined)
        res.set('X-VectorWarp-Illuminator', requestedIlluminator);
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
// A coordinator can be the sole public-exchange client for a local node fleet.
// Followers consume this readsb-compatible snapshot through the same bounded,
// cached source adapter they use for a physical receiver. Public polling stays
// paced at the coordinator and never multiplies with the number of radar nodes.
for (const route of ['/api/adsb/fused', '/api/adsb/fused/data/aircraft.json'])
app.get(route, async (_req, res) => {
  const recorded = recordedReplayTruth('aircraft');
  if (recorded.replay)
    return res.json(recorded.value === null ? replayTruthStatus() : recorded.value);
  if (replayTruthDisabled()) return res.json(replayTruthStatus());
  if (config.truth?.adsb?.enabled !== true)
    return res.status(503).json({error: 'ADS-B is disabled'});
  try {
    res.set('Cache-Control', 'private, no-store');
    res.json(await adsbSource.getPresentation('aircraft'));
  } catch (error) { res.status(503).json({error: error.message}); }
});
for (const [route, kind] of [['/api/remote-id', 'aircraft'],
  ['/api/remote-id/delay-doppler', 'delayDoppler']]) {
  app.get(route, async (req, res) => {
    if (config.truth?.remote_id?.enabled !== true)
      return res.json({enabled: false, aircraft: [], drones: []});
    try {
      const presentation = req.query.presentation;
      const requestedTimestamp = presentation === 'aligned' ?
        requestedPresentationTimestamp(req) : null;
      if (Number.isNaN(requestedTimestamp))
        return res.status(400).json({error: 'frame_timestamp must be a non-negative integer'});
      const value = presentation === 'aligned' && typeof remoteIdSource.getPresentationAt === 'function' ?
        await remoteIdSource.getPresentationAt(kind, requestedTimestamp ?? Date.now() - presentationDelayMs) :
        presentation && typeof remoteIdSource.getPresentation === 'function' ?
          await remoteIdSource.getPresentation(kind) : await remoteIdSource.get(kind);
      if (requestedTimestamp !== null)
        res.set('X-VectorWarp-Presentation-Timestamp', String(requestedTimestamp));
      if (kind === 'delayDoppler' && (req.query.presentation === 'overlay' || req.query.overlay === '1')) {
        const compact = {};
        for (const [id, drone] of Object.entries(value || {})) {
          if (!drone || !Number.isFinite(drone.delay) || !Number.isFinite(drone.doppler)) continue;
          compact[id] = drone;
        }
        return res.json(compact);
      }
      res.json(value);
    } catch (error) { res.status(503).json({error: error.message}); }
  });
}
app.get('/api/remote-id/status', async (_req, res) => {
  if (config.truth?.remote_id?.enabled !== true)
    return res.json({enabled: false, online: false, aircraft: [], drones: []});
  res.json(await remoteIdSource.status());
});
app.get('/api/radar/coverage', (req, res) => {
  if (replayTruthDisabled()) return res.json(replayTruthStatus());
  const hours = Number(req.query.hours || 24);
  const bins = Number(req.query.bins || 72);
  const altitudeBand = String(req.query.altitude || 'all');
  res.set('Cache-Control', 'private, max-age=30');
  if (multiIlluminatorEvidence.enabled) {
    const value = multiIlluminatorEvidence.response('coverage', {
      illuminator: req.query.illuminator, timing: 'provisional',
      options: {hours, bins, altitudeBand}});
    if (!value) return res.status(404).json({available: false, reason: 'Unknown illuminator.'});
    return res.json(value);
  }
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
app.get('/api/array-calibration/status', (req, res) => {
  res.json(buildArrayCalibrationStatus({config, recorderStatus: manifoldRecorder.status()}));
});

// stash API
app.get('/stash/map', async (req, res, next) => {
  try { await sendDisplayResponse(req, res, radarSession.stashes.map.get_data(), presentationQuery(req)); }
  catch (error) { next(error); }
});
app.get('/stash/detection', (req, res) => {
  res.send(radarSession.stashes.detection.get_data());
});
app.get('/stash/iqdata', (req, res) => {
  res.send(radarSession.stashes.iqdata.get_data());
});
app.get('/stash/timing', (req, res) => {
  res.send(radarSession.stashes.timing.get_data());
});

// read state of capture
app.get('/capture', (req, res) => {
  res.send(capture);
});
app.get('/capture/request', (req, res) => res.json({recording: capture,
  revision: captureRevision, clip: captureTriggerQueue.requestWithEvidence(records =>
    captureAdmissionEvidence.acceptCaptureEvents(records)), replay: replayControl}));
app.get('/api/replay/status', (req, res) => {
  const processor = radarSession.currentProcessor();
  if (processor?.input !== 'replay') return res.json({active: false});
  res.json({active: true, paused: replayControl.paused, file: processor.file,
    geographic_provenance: geographicContext(config),
    acquisition_provenance: processor.replayProvenance || null,
    controlsAvailable: replayControlsAvailable(processor),
    position_samples: processor.positionSamples, total_samples: processor.totalSamples,
    sample_rate_hz: processor.sampleRate, state: processor.state});
});
app.post('/api/replay/control', (req, res) => {
  if (!configWriteOriginAllowed(req) || req.get('X-VectorWarp-Intent') !== 'replay-control-v1')
    return res.status(403).json({error: 'Replay controls require a trusted VectorWarp origin and intent.'});
  const processor = radarSession.currentProcessor();
  const result = updateReplayControl(processor, replayControl, req.body);
  if (result.error) return res.status(result.status).json({error: result.error});
  replayControl = result.control;
  radarPerformanceHealth.reset();
  res.json({ok: true, ...replayControl});
});
app.get('/capture/status', (req, res) => {
  const now = Date.now();
  const receiving = radarSession.fresh('timestamp',
    Math.max(10000, (config.process.data.cpi || 1) * 3000), now);
  const processor = radarSession.currentProcessor();
  const replay = processor?.input === 'replay' || config.capture.replay?.state === true;
  const acknowledgement = processor && processor.input === 'live' ? processor : null;
  const previous = radarSession.processorStatus?.value;
  // Closing includes the artifact digest and can outlast telemetry freshness.
  // Retain the last observation only for that unacknowledged stop; it must not
  // attest fresh processor health or imply that finalization has completed.
  const pendingStop = !capture && previous?.input === 'live' && previous.recording === true &&
    previous.recordingRequestId !== captureRevision;
  const observed = acknowledgement || (pendingStop ? previous : null);
  const recording = observed?.recording === true;
  const recordingError = acknowledgement?.recordingError || null;
  const acknowledged = acknowledgement && acknowledgement.recordingRequestId === captureRevision &&
    acknowledgement.recording === capture;
  if (capture && recordingError && acknowledgement?.recordingRequestId === captureRevision) {
    capture = false;
    captureStartedAt = null;
  }
  const disk = recordingDiskStatus();
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
    pendingStop,
    processorFresh: acknowledgement !== null,
    recording,
    recordingFile: observed?.recordingFile || '',
    recordingError,
    recordedSamples: observed?.recordedSamples || 0,
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
  if (gainPublicationBlocked || retiredPublicationError)
    return res.status(409).json({error: retiredPublicationError || 'Wait for the gain check and processing restart before recording.'});
  const processor = radarSession.currentProcessor();
  if (processor?.input === 'replay' || config.capture.replay?.state === true)
    return res.status(409).json({error: 'Recording is unavailable while replay is active.'});
  if (!capture) {
    let disk;
    try { disk = await recordingLibrary.prepareForRecording({activeFile: activeRecordingFile(processor),
      writeBytesPerSecond: recordingWriteBytesPerSecond(), safetySeconds: 15}); }
    catch (error) { return next(error); }
    if (!disk.allowed)
      return res.status(507).json({error: 'Recording would cross the configured free-disk reserve. Clean up recordings or change the retention settings.'});
  }
  if (!capture && !radarSession.fresh('timestamp',
      Math.max(10000, (config.process.data.cpi || 1) * 3000), Date.now(), true))
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
    const disk = await recordingLibrary.prepareForRecording({
      writeBytesPerSecond: recordingWriteBytesPerSecond(), safetySeconds: 15});
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
  // The global API policy intentionally disables caching. Static assets have
  // their own validators and must not inherit legacy no-cache headers or every
  // page navigation redownloads the same UI on mobile.
  response.removeHeader('Pragma');
  response.removeHeader('Expires');
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
  if (!localRadar) return;
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
    const connection = radarSession.open(name);
    const telemetry = name === 'timestamp' || name === 'timing';
    const feed = jsonFrames((frame, parsed) => {
      if ((gainPublicationBlocked || retiredPublicationError) && !telemetry) return;
      if (onFrame(frame, connection, parsed) === false && telemetry) return;
      connection.firstFrame = false;
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
    socket.on('close', () => {
      if (telemetry && radarSession.close(connection) && name === 'timestamp')
        gainRestartTimestampAt = null;
      console.log('Connection closed.');
    });
  });
}

// One admission path publishes both legacy sockets and primary illuminator views.
// These adapters only apply product-specific consumers; the store owns retention.
function observeRadarProduct(name, frame,
    {admitHealth = true, connection, history = true, parsed = frame, publishedParsed}, published) {
  if (!localRadar) return;
  switch (name) {
  case 'map': {
    const at = frameTimestamp(parsed);
    if (Number.isSafeInteger(at))
      void recordingTimeline.track(at, Promise.all([eventTruthAt(adsbSource, 'aircraft', at),
        eventTruthAt(adsbSource, 'delayDoppler', at)])
        .then(([aircraft, delayDoppler]) => recordingTimeline.add({timestamp_ms: at,
          aircraft, delay_doppler: delayDoppler})).catch(() => {}));
    break;
  }
  case 'detection':
    trackerForwarder.accept(parsed);
    evidenceStatistics.acceptDetection(parsed);
    provisionalEvidenceStatistics.acceptDetection(parsed);
    oprsEventRecorder.acceptDetection(parsed);
    break;
  case 'tracker': {
    ingestTrackerFrame({tracker: publishedParsed ?? published, healthTracker: parsed,
      channel: 'primary',
      diagnostics: parsed?.trackerDiagnostics,
      resetOnTimestampRegression: connection?.firstFrame === true ||
        radarSession.processorStatus?.value?.input === 'replay',
      radarPerformanceHealth: admitHealth ? radarPerformanceHealth : null,
      evidenceConsumers: [evidenceStatistics, provisionalEvidenceStatistics, oprsEventRecorder]});
    break;
  }
  case 'bearing':
    if (connection?.firstFrame) bearingHypotheses.reset();
    evidenceStatistics.acceptBearing(parsed);
    break;
  case 'timing':
    if (history) {
      oprsEventRecorder.acceptTiming(parsed);
      evidenceStatistics.acceptTiming(parsed);
      provisionalEvidenceStatistics.acceptTiming(provisionalTimingFrame(parsed));
    }
    break;
  }
}
for (const [port, name] of [['map', 'map'], ['detection', 'detection'], ['track', 'tracker']])
  listenData(jsonStreamServer(port, (frame, connection, parsed) =>
    radarSession.acceptProduct(name, frame, {connection, parsed})), port);

function acceptIlluminatorFrame(frame, connection = {}, parsed) {
  const {value, map, duplicate} = radarSession.acceptIlluminator(frame, connection, parsed);
  if (!duplicate) {
    ingestTrackerFrame({tracker: value.tracks, channel: value.id,
      diagnostics: value.trackerDiagnostics,
      resetOnTimestampRegression: radarSession.processorStatus?.value?.input === 'replay',
      radarPerformanceHealth, multiIlluminatorEvidence, causalFunnelRecorder,
      causalFrame: value,
      causalMaximum: Number(config.process?.evidence?.causal_funnel?.max_candidates) || 32});
  }
  const fusionIds = (config.process?.multi_illuminator?.illuminators || [])
    .filter(item => item?.enabled === true).map(item => item.id);
  if (fusionIds.length >= 2) {
    const cohort = illuminatorFrames.latestCohort(fusionIds);
    if (cohort) {
      multiIlluminatorTrackFuser.update(cohort);
    }
  }
  if (radarSession.publishPrimary(value, duplicate, map))
    for (const name of ['map', 'detection', 'track']) serviceErrors.delete(name);
  if (duplicate) return;
  const geometry = recordingIlluminatorAdsbGeometry.get(value.id);
  const record = aircraft => recordingTimeline.addIlluminator({timestamp_ms: value.timestamp,
    illuminator: value.id, aircraft,
    delay_doppler: aircraft && geometry ? geometry.update(aircraft).data : null,
    map: value.map, detections: value.detections, tracks: value.tracks,
    doppler_window: value.doppler_window ?? 'unknown',
    atsc_reference: value.atsc_reference ?? null,
    first_input_sample: value.first_input_sample,
    end_input_sample: value.end_input_sample});
  void recordingTimeline.track(value.timestamp,
    eventTruthAt(adsbSource, 'aircraft', value.timestamp)
      .then(record).catch(() => record(null)));
}
if (config.process?.multi_illuminator?.enabled === true &&
    Number.isInteger(config.network?.ports?.illuminator)) {
  const server_illuminator = jsonStreamServer('illuminator', acceptIlluminatorFrame);
  listenData(server_illuminator, 'illuminator');
}

if (config.process?.bearing?.enable === true && Number.isInteger(config.network?.ports?.bearing)) {
  const server_bearing = jsonStreamServer('bearing', (frame, connection, parsed) =>
    radarSession.acceptProduct('bearing', frame, {connection, parsed}));
  listenData(server_bearing, 'bearing');
}

if (manifoldRecorder.status().enabled &&
    Number.isInteger(config.network?.ports?.manifold)) {
  const server_manifold = jsonStreamServer('manifold', (frame, _connection, parsed) =>
    manifoldRecorder.accept(frame, parsed));
  listenData(server_manifold, 'manifold');
}

if (causalFunnelRecorder.status().native_enabled &&
    Number.isInteger(config.network?.ports?.causal_funnel)) {
  const server_causal_funnel = jsonStreamServer('causal_funnel', (frame, _connection, parsed) =>
    causalFunnelRecorder.accept(frame, {nativeTransport: true, parsed}));
  listenData(server_causal_funnel, 'causal_funnel');
}

listenData(jsonStreamServer('timestamp', (frame, connection, parsed) => {
  const value = parsed.timestamp;
  if (!Number.isSafeInteger(value) || value < 0)
    throw new Error('Expected a non-negative safe-integer radar timestamp');
  if (!radarSession.activate(connection)) return false;
  gainRestartTimestampAt = Date.now();
  completePendingGainRestart();
  if (!gainPublicationBlocked && !retiredPublicationError) radarSession.acceptProduct('timestamp', String(value));
}), 'timestamp');

listenData(jsonStreamServer('timing', (frame, connection, parsed) => {
  if (!radarSession.activate(connection)) return false;
  // Terminal capture accounting is telemetry, not another scientific CPI.
  radarSession.acceptProduct('timing', frame,
    {history: parsed.captureTerminal !== true, parsed});
}), 'timing');

listenData(jsonStreamServer('iqdata', (frame, _connection, parsed) =>
  radarSession.acceptProduct('iqdata', frame, {parsed})), 'iqdata');
if (localRadar) gainMonitor.start();

process.on('SIGTERM', async () => {
  console.log('SIGTERM signal received.');
  trackerForwarder.close();
  evidenceStatistics?.close();
  provisionalEvidenceStatistics?.close();
  multiIlluminatorEvidence?.close();
  oprsEventRecorder.close();
  gainMonitor.stop();
  try { await recordingTimeline.stop(null, Date.now()); }
  catch (error) { console.error(`Manual recording ADS-B timeline failed: ${error.message}`); }
  process.exit(0);
});
