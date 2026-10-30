(function(root, factory) {
  const api = factory();
  if (typeof module !== 'undefined') module.exports = api;
  if (root) root.VectorWarpReports = api;
})(typeof window !== 'undefined' ? window : globalThis, function() {
  'use strict';
  const ENDPOINT = 'https://vectorwarp-report-relay.vectorwarp-reports.workers.dev/v1/reports';
  const STORAGE_KEY = 'vectorwarp-pending-diagnostic-report-v1';
  const IPV6_PATTERN = /(?<![A-Fa-f0-9:])(?:(?:[A-Fa-f0-9]{1,4}:){7}[A-Fa-f0-9]{1,4}|(?:[A-Fa-f0-9]{1,4}:){1,7}:|(?:[A-Fa-f0-9]{1,4}:){1,6}:[A-Fa-f0-9]{1,4}|(?:[A-Fa-f0-9]{1,4}:){1,5}(?::[A-Fa-f0-9]{1,4}){1,2}|(?:[A-Fa-f0-9]{1,4}:){1,4}(?::[A-Fa-f0-9]{1,4}){1,3}|(?:[A-Fa-f0-9]{1,4}:){1,3}(?::[A-Fa-f0-9]{1,4}){1,4}|(?:[A-Fa-f0-9]{1,4}:){1,2}(?::[A-Fa-f0-9]{1,4}){1,5}|[A-Fa-f0-9]{1,4}:(?:(?::[A-Fa-f0-9]{1,4}){1,6})|:(?:(?::[A-Fa-f0-9]{1,4}){1,7}|:))(?![A-Fa-f0-9:])/g;
  const allowedReceivers = new Set(['RspDuo', 'Kraken', 'Usrp', 'HackRF', 'Replay', 'Unknown']);
  const incidentTypes = new Map(Object.entries({
    SDR_ENUMERATION_FAILED: ['receiver', 'enumerate'], SDR_OPEN_FAILED: ['receiver', 'open'],
    SDR_STREAM_LOST: ['receiver', 'stream'], RECEIVER_SYNC_FAILED: ['receiver', 'sync'],
    GAIN_MONITOR_FAILED: ['receiver', 'gain-monitor'], PROCESSOR_START_FAILED: ['processor', 'start'],
    PROCESSOR_RUNTIME_FAILED: ['processor', 'runtime'], GPU_BACKEND_FAILED: ['processor', 'acceleration'],
    RESTART_FAILED: ['lifecycle', 'restart'], CONFIG_PERSISTENCE_FAILED: ['configuration', 'persist'],
    API_UNAVAILABLE: ['api', 'connect'], ADSB_SOURCE_FAILED: ['adsb', 'connect'],
    MANUAL_REPORT: ['support', 'manual'], SERVICE_DEGRADED: ['status', 'observe']
  }));
  const booleanDetails = new Set(['enumerate', 'apiVersion', 'open', 'init', 'permissionDenied',
    'driverLoaded', 'probeTimedOut', 'rollbackAttempted', 'rollbackSucceeded',
    'reconciliationRequired', 'helperAvailable', 'timedOut', 'receiverReady', 'streamEstablished',
    'callbackFault', 'fallbackAttempted', 'fallbackSucceeded', 'writeAttempted', 'revisionConflict',
    'syntheticLiveTest', 'overloadKnown', 'processorFresh']);
  const integerDetails = new Set(['deviceCount', 'candidateCount', 'attemptCount', 'exitStatus',
    'framesReceived', 'lastFrameAgeMs', 'droppedFrames', 'framesProcessed', 'queueDepth', 'sourceCount',
    'healthySourceCount', 'consecutiveFailures', 'statusCode', 'timeoutMs', 'timestampConnections']);
  const stringDetails = new Map(Object.entries({discoveryStage: ['enumerate'],
    driverErrorClass: ['permission', 'timeout', 'disconnected', 'invalid-response', 'device-busy', 'unavailable'],
    driverErrorCode: ['DEVICE_BUSY'], failurePhase: ['probe', 'monitor', 'adjust'],
    syncState: ['failed', 'partial', 'indeterminate'], receiptState: ['failed'],
    terminationSignal: ['none', 'SIGINT', 'SIGTERM', 'SIGKILL'], driverState: ['failed'],
    errorClass: ['permission', 'timeout', 'disconnected', 'invalid-response', 'device-busy', 'unavailable'],
    writeStage: ['write', 'fsync', 'rename'], routeName: ['status', 'config', 'restart']
  }).map(([key, values]) => [key, new Set(values)]));
  const incidentTracker = {active: null, generations: new Map()};
  const memory = new Map();
  const memoryStorage = {setItem: (key, value) => memory.set(key, value),
    getItem: key => memory.get(key) || null, removeItem: key => memory.delete(key)};
  function reportStorage() {
    try { return typeof localStorage === 'undefined' ? memoryStorage : localStorage; }
    catch (_) { return memoryStorage; }
  }

  function clean(value, maximum = 256) {
    if (value === undefined || value === null) return null;
    return String(value).normalize('NFKC').replace(/[\u0000-\u001f\u007f`]+/g, ' ')
      .replace(/\\(["'])/g, '$1')
      .replace(/\b[a-z][a-z0-9+.-]*:\/\/[^\s<>"']+/gi, '[URL redacted]')
      .replace(/\b[\w.+-]+@[\w.-]+\.[A-Za-z]{2,}\b/g, '[email redacted]')
      .replace(/\b(?:\d{1,3}\.){3}\d{1,3}\b/g, '[IP redacted]')
      .replace(IPV6_PATTERN, '[IP redacted]')
      .replace(/\b(?:authorization\s*[:=]\s*)?bearer\s+[A-Za-z0-9._~+\/-]+={0,2}/gi, 'Bearer [redacted]')
      .replace(/\b(?:authorization\s*[:=]\s*)?basic\s+[A-Za-z0-9+/]+={0,2}/gi, 'Basic [redacted]')
      .replace(/\{[^}\r\n]{0,1024}\}/g, '[structured text redacted]')
      .replace(/\[[^\]\r\n]{0,512}\]/g, '[structured text redacted]')
      .replace(/["'“”]?(?:serial(?:[\s_-]*number)?|access[_-]?token(?:value)?|api[_-]?key|token|password|secret|hostname|host|endpoint|address|aircraft[\s_-]*hex|hex|icao|callsign|flight)["'“”]?\s*(?::|=|\s)\s*(?:["“](?:\\.|[^"”\\])*["”]|'(?:\\.|[^'\\])*'|[^\s,;]+)/gi,
        '[sensitive value redacted]')
      .replace(/["']?(?:lat(?:itude)?|lon(?:gitude)?)["']?\s*[:=]\s*["']?-?\d+(?:\.\d+)?["']?/gi,
        match => `${match.split(/\s*[:=]\s*/)[0]}=[redacted]`)
      .replace(/\(\s*-?(?:[0-8]?\d(?:\.\d+)?|90(?:\.0+)?)\s*,\s*-?(?:1[0-7]\d(?:\.\d+)?|\d?\d(?:\.\d+)?|180(?:\.0+)?)\s*\)/g,
        '[coordinates redacted]')
      .replace(/\\\\[^\s\\/]+[\\/][^,;"'<>\r\n]*/g, '[path redacted]')
      .replace(/[A-Za-z]:\\(?:[^<>:"|?*\r\n]+\\)*[^,;<>:"|?*\r\n]*/g, '[path redacted]')
      .replace(/(^|[\s("'=])\/(?:Users|Volumes|home|mnt|etc|var|opt|private|tmp|run)\/[^,;"'<>\r\n]*/g,
        (_, prefix) => `${prefix}[path redacted]`).replace(/\s+/g, ' ').trim().slice(0, maximum) || null;
  }
  function integer(value, fallback = null) {
    return Number.isSafeInteger(value) && value >= 0 ? value : fallback;
  }
  function scalars(value = {}) {
    const result = {};
    for (const [key, item] of Object.entries(value).slice(0, 40)) {
      if (booleanDetails.has(key) && typeof item === 'boolean') result[key] = item;
      else if (integerDetails.has(key) && Number.isSafeInteger(item) && item >= 0) result[key] = item;
      else if (stringDetails.get(key)?.has(item)) result[key] = item;
      else if ((booleanDetails.has(key) || integerDetails.has(key) || stringDetails.has(key)) && item === null)
        result[key] = null;
    }
    return result;
  }
  function receiverType(status, config) {
    const value = status?.processor?.receiver || config?.capture?.device?.type || 'Unknown';
    return allowedReceivers.has(value) ? value : 'Unknown';
  }
  function errorClass(message = '') {
    if (/permission|denied/i.test(message)) return 'permission';
    if (/timeout|timed out/i.test(message)) return 'timeout';
    if (/disconnect/i.test(message)) return 'disconnected';
    if (/invalid|malformed|parse/i.test(message)) return 'invalid-response';
    if (/busy/i.test(message)) return 'device-busy';
    return 'unavailable';
  }
  function classify(status = {}, tracker = incidentTracker) {
    const processor = status.processor || {};
    const startup = processor.receiverStartup;
    const stages = startup?.sdk?.stages || {};
    let incident = null;
    if (processor.state === 'error' && startup && stages.enumerate === false)
      incident = {code: 'SDR_ENUMERATION_FAILED', component: 'receiver', stage: 'enumerate', message: processor.error,
        details: {deviceCount: startup.selected ? 1 : 0, permissionDenied: /permission|denied/i.test(processor.error || ''),
          driverLoaded: stages.open === true || stages.apiVersion === true, discoveryStage: 'enumerate'}};
    else if (status.sdrplayStartup?.state === 'failed' || processor.state === 'error' && startup && stages.open === false)
      incident = {code: 'SDR_OPEN_FAILED', component: 'receiver', stage: 'open', message: status.sdrplayStartup?.message || processor.error,
        details: {driverErrorClass: errorClass(status.sdrplayStartup?.message || processor.error), candidateCount: startup?.selected ? 1 : 0}};
    else if (status.gainMonitor?.status === 'error')
      incident = {code: 'GAIN_MONITOR_FAILED', component: 'receiver', stage: 'gain-monitor', message: status.gainMonitor.error?.message || 'Gain monitor failed',
        details: {probeTimedOut: status.gainMonitor.error?.code === 'TIMEOUT', attemptCount: status.gainMonitor.monitor?.consecutiveUnsafe || 0,
          failurePhase: status.gainMonitor.phase || 'probe'}};
    else if (['failed', 'partial', 'indeterminate'].includes(status.receiverSynchronization?.applicationState || status.receiverSynchronization?.state))
      incident = {code: 'RECEIVER_SYNC_FAILED', component: 'receiver', stage: 'sync', message: status.receiverSynchronization.message || 'Receiver synchronization failed',
        details: {syncState: status.receiverSynchronization.applicationState || status.receiverSynchronization.state,
          rollbackAttempted: status.receiverSynchronization.rollbackAttempted === true,
          rollbackSucceeded: status.receiverSynchronization.rollbackSucceeded === true,
          reconciliationRequired: status.receiverSynchronization.reconciliationRequired === true}};
    else if (status.restart?.state === 'failed')
      incident = {code: 'RESTART_FAILED', component: 'lifecycle', stage: 'restart', message: status.restart.message,
        details: {helperAvailable: status.restart.helperAvailable !== false, receiptState: status.restart.state,
          timedOut: status.restart.timedOut === true, exitStatus: integer(status.restart.exitStatus)}};
    else if (processor.state === 'error' && (processor.totalSamples === 0 || processor.loops === 0))
      incident = {code: 'PROCESSOR_START_FAILED', component: 'processor', stage: 'start', message: processor.error,
        details: {exitStatus: integer(processor.exitStatus), terminationSignal: clean(processor.terminationSignal, 32), receiverReady: Boolean(startup)}};
    else if ((processor.state === 'live' || processor.state === 'error') && status.radar === 'stale' ||
        processor.state === 'error' && /stream|callback|disconnect/i.test(processor.error || ''))
      incident = {code: 'SDR_STREAM_LOST', component: 'receiver', stage: 'stream', message: processor.error || status.message || 'Receiver stream stopped',
        details: {framesReceived: integer(processor.loops || processor.totalSamples, 0),
          lastFrameAgeMs: integer(status.lastFrameAt ? Math.max(0, Date.now() - status.lastFrameAt) : null),
          droppedFrames: integer(processor.trailingSamples, 0)}};
    else if (processor.state === 'error')
      incident = {code: 'PROCESSOR_RUNTIME_FAILED', component: 'processor', stage: 'runtime', message: processor.error,
        details: {framesProcessed: integer(processor.loops || processor.totalSamples, 0), queueDepth: integer(processor.queueDepth)}};
    else if (status.acceleration?.state === 'error' || status.clutterAcceleration?.state === 'error')
      incident = {code: 'GPU_BACKEND_FAILED', component: 'processor', stage: 'acceleration', message: status.acceleration?.reason || status.clutterAcceleration?.reason,
        details: {driverState: 'failed', fallbackAttempted: true, fallbackSucceeded: status.acceleration?.active === 'cpu'}};
    else if (status.adsb?.enabled && status.adsb.online === false && status.adsb.sources?.some(source => source.consecutive_failures > 0)) {
      const sources = status.adsb.sources; const message = sources.find(source => source.message)?.message || 'No healthy ADS-B source';
      incident = {code: 'ADSB_SOURCE_FAILED', component: 'adsb', stage: 'connect', severity: 'degraded', message,
        details: {sourceCount: sources.length, healthySourceCount: sources.filter(source => source.healthy).length,
          consecutiveFailures: Math.max(...sources.map(source => integer(source.consecutive_failures, 0))), errorClass: errorClass(message)}};
    } else if (status.errors?.length)
      incident = {code: 'PROCESSOR_RUNTIME_FAILED', component: 'processor', stage: 'runtime', message: status.errors.join(' ')};
    if (!incident) { if (tracker.active) tracker.generations.set(tracker.active, (tracker.generations.get(tracker.active) || 0) + 1); tracker.active = null; return null; }
    const base = [incident.code, incident.component, incident.stage,
      processor.receiver || status.receiverType || 'Unknown'].join('|');
    if (tracker.active && tracker.active !== base)
      tracker.generations.set(tracker.active, (tracker.generations.get(tracker.active) || 0) + 1);
    tracker.active = base; incident._fingerprint = base;
    incident._recoveryGeneration = tracker.generations.get(base) || 0;
    return incident;
  }
  function classifyFailure(kind, evidence = {}) {
    const message = evidence.message || 'VectorWarp operation failed';
    if (kind === 'restart') return {code: 'RESTART_FAILED', component: 'lifecycle', stage: 'restart', message,
      details: scalars(evidence.details)};
    if (kind === 'config-persistence') return {code: 'CONFIG_PERSISTENCE_FAILED', component: 'configuration', stage: 'persist', message,
      details: scalars(evidence.details)};
    if (kind === 'receiver-sync') return {code: 'RECEIVER_SYNC_FAILED', component: 'receiver', stage: 'sync', message,
      details: scalars(evidence.details)};
    if (kind === 'api') return {code: 'API_UNAVAILABLE', component: 'api', stage: 'connect', severity: 'degraded', message,
      details: scalars(evidence.details)};
    return null;
  }
  function uuid() {
    if (typeof crypto !== 'undefined' && typeof crypto.randomUUID === 'function') return crypto.randomUUID();
    const bytes = new Uint8Array(16); (typeof crypto !== 'undefined' ? crypto : require('node:crypto').webcrypto).getRandomValues(bytes);
    bytes[6] = bytes[6] & 15 | 64; bytes[8] = bytes[8] & 63 | 128;
    return [...bytes].map((value, index) => ([4, 6, 8, 10].includes(index) ? '-' : '') + value.toString(16).padStart(2, '0')).join('');
  }
  function stable(value, expression, maximum, fallback = null) {
    return typeof value === 'string' && value.length <= maximum && expression.test(value) ? value : fallback;
  }
  const release = (value, maximum = 64) => stable(value,
    /^[A-Za-z0-9][A-Za-z0-9_.+-]*$/, maximum);
  const identifierValue = (value, maximum = 64) => stable(value,
    /^[A-Za-z0-9][A-Za-z0-9_.-]*$/, maximum);
  const osValues = new Set(['Linux', 'Darwin', 'Windows', 'linux', 'darwin', 'win32', 'macOS',
    'unknown']);
  const severityValues = new Set(['fatal', 'error', 'degraded']);
  const transportValues = new Set(['usb', 'network', 'replay', 'unknown']);
  function iso(value, fallback) {
    const milliseconds = typeof value === 'string' || typeof value === 'number' ? Date.parse(value) : NaN;
    return Number.isFinite(milliseconds) ? new Date(milliseconds).toISOString() : new Date(fallback).toISOString();
  }
  function booleanValue(value) { return typeof value === 'boolean' ? value : null; }
  function diagnosticInteger(value) { return Number.isSafeInteger(value) && value >= 0 ? value : null; }
  function normalizeFeatures(value = {}) {
    const recording = value.recording || {}; const replay = value.replay || {};
    const adsb = value.adsb || {}; const processing = value.processing || {};
    const presentation = value.presentation || {};
    const lowSpacePolicies = new Set(['stop_recording', 'delete_oldest']);
    return {
      recording: {requested: booleanValue(recording.requested), paused: booleanValue(recording.paused),
        active: booleanValue(recording.active), errorPresent: booleanValue(recording.errorPresent),
        recordedSamples: diagnosticInteger(recording.recordedSamples),
        clipRevision: diagnosticInteger(recording.clipRevision),
        clipErrorPresent: booleanValue(recording.clipErrorPresent),
        clipSamples: diagnosticInteger(recording.clipSamples),
        timelineCaptureAvailable: booleanValue(recording.timelineCaptureAvailable),
        automaticCleanup: booleanValue(recording.automaticCleanup),
        lowSpacePolicy: lowSpacePolicies.has(recording.lowSpacePolicy) ? recording.lowSpacePolicy : null,
        maximumAgeDays: diagnosticInteger(recording.maximumAgeDays),
        maximumTotalGb: diagnosticInteger(recording.maximumTotalGb),
        minimumFreeGb: diagnosticInteger(recording.minimumFreeGb)},
      replay: {active: booleanValue(replay.active), paused: booleanValue(replay.paused),
        seekable: booleanValue(replay.seekable), positionSamples: diagnosticInteger(replay.positionSamples),
        totalSamples: diagnosticInteger(replay.totalSamples), sampleRateHz: diagnosticInteger(replay.sampleRateHz),
        timelineAvailable: booleanValue(replay.timelineAvailable)},
      adsb: {enabled: booleanValue(adsb.enabled), online: booleanValue(adsb.online),
        degraded: booleanValue(adsb.degraded), sourceCount: diagnosticInteger(adsb.sourceCount),
        healthySourceCount: diagnosticInteger(adsb.healthySourceCount),
        aircraftCount: diagnosticInteger(adsb.aircraftCount),
        directAircraftCount: diagnosticInteger(adsb.directAircraftCount),
        mlatAircraftCount: diagnosticInteger(adsb.mlatAircraftCount),
        associationLagCpis: diagnosticInteger(adsb.associationLagCpis),
        associationLagMs: diagnosticInteger(adsb.associationLagMs)},
      processing: {acceleration: identifierValue(processing.acceleration, 64),
        clutterAcceleration: identifierValue(processing.clutterAcceleration, 64),
        configuredCpiMs: diagnosticInteger(processing.configuredCpiMs)},
      presentation: {timestampConnections: diagnosticInteger(presentation.timestampConnections),
        associationLagCpis: diagnosticInteger(presentation.associationLagCpis),
        associationLagMs: diagnosticInteger(presentation.associationLagMs)}
    };
  }
  function normalizedIncident(value = {}, now = Date.now()) {
    const code = incidentTypes.has(value.code) ? value.code : 'MANUAL_REPORT';
    const [component, stage] = incidentTypes.get(code);
    return {code, component, stage,
      severity: severityValues.has(value.severity) ? value.severity : 'error',
      message: `VectorWarp detected ${code} during ${component} ${stage}.`,
      firstSeenAt: iso(value.firstSeenAt, now), lastSeenAt: iso(value.lastSeenAt, now),
      occurrences: integer(value.occurrences, 1), recovered: value.recovered === true};
  }
  function normalizeReport(value, now = Date.now()) {
    if (!value || typeof value !== 'object' || Array.isArray(value) || value.schema !== 1 ||
        value.consent !== true || typeof value.submissionId !== 'string' ||
        !/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i.test(value.submissionId))
      throw new Error('Stored diagnostic report is incompatible');
    const build = value.build || {}; const system = value.system || {};
    const receiver = value.receiver || {}; const runtime = value.runtime || {};
    return {schema: 1, consent: true, submissionId: value.submissionId.toLowerCase(),
      incident: normalizedIncident(value.incident, now),
      build: {version: release(build.version) || 'development',
        commit: /^[a-f0-9]{7,40}$/i.test(build.commit || '') ? build.commit.toLowerCase() : '0000000',
        package: release(build.package) || 'unknown'},
      system: {os: osValues.has(system.os) ? system.os : 'unknown',
        architecture: identifierValue(system.architecture, 32) || 'unknown',
        kernel: release(system.kernel, 128), browser: null},
      receiver: {type: allowedReceivers.has(receiver.type) ? receiver.type : 'Unknown', model: null,
        driverVersion: release(receiver.driverVersion), apiVersion: release(receiver.apiVersion),
        firmwareVersion: release(receiver.firmwareVersion), sampleRateHz: integer(receiver.sampleRateHz),
        centerFrequencyHz: integer(receiver.centerFrequencyHz), channelCount: integer(receiver.channelCount),
        transport: transportValues.has(receiver.transport) ? receiver.transport : null,
        startupStages: scalars(receiver.startupStages), health: scalars(receiver.health)},
      runtime: {processorState: identifierValue(runtime.processorState, 48),
        restartState: identifierValue(runtime.restartState, 48), backend: identifierValue(runtime.backend),
        configRevision: /^(?:sha256:)?[a-f0-9]{3,64}$/i.test(runtime.configRevision || '') ?
          runtime.configRevision : null,
        uptimeSeconds: integer(runtime.uptimeSeconds), details: scalars(runtime.details)},
      recentEvents: Array.isArray(value.recentEvents) ? value.recentEvents.slice(0, 20)
        .map(event => normalizedIncident(event, now)) : [],
      features: normalizeFeatures(value.features), comment: clean(value.comment, 2000)};
  }
  function buildReport({incident, status = {}, config = {}, comment = '', now = Date.now()}) {
    const type = receiverType(status, config);
    const processor = status.processor || {};
    const frequency = config?.capture?.fc;
    const sampleRate = config?.capture?.fs;
    const channels = config?.capture?.device?.channel_count;
    const code = incidentTypes.has(incident.code) ? incident.code : 'MANUAL_REPORT';
    const [component, stage] = incidentTypes.get(code);
    const osValue = osValues.has(status.system?.os) ? status.system.os : 'unknown';
    return {schema: 1, consent: true, submissionId: uuid(), incident: {
      code, component, stage,
      severity: severityValues.has(incident.severity) ? incident.severity : 'error',
      message: `VectorWarp detected ${code} during ${component} ${stage}.`,
      firstSeenAt: new Date(incident.firstSeenAt || now).toISOString(),
      lastSeenAt: new Date(now).toISOString(), occurrences: integer(incident.occurrences, 1),
      recovered: incident.recovered === true
    }, build: {version: release(status.build?.version) || 'development',
      commit: /^[a-f0-9]{7,40}$/i.test(status.build?.commit || '') ? status.build.commit : '0000000',
      package: release(status.build?.package) || 'unknown'},
    system: {os: osValue, architecture: identifierValue(status.system?.architecture, 32) || 'unknown',
      kernel: release(status.system?.kernel, 128), browser: null},
    receiver: {type, model: null,
      driverVersion: release(processor.driverVersion), apiVersion: release(processor.apiVersion),
      firmwareVersion: release(processor.firmwareVersion), sampleRateHz: integer(processor.sampleRate || sampleRate),
      centerFrequencyHz: integer(processor.frequency || frequency), channelCount: integer(processor.channels || channels),
      transport: transportValues.has(processor.transport) ? processor.transport : null,
      startupStages: scalars(processor.receiverStartup?.sdk?.stages),
      health: scalars({streamEstablished: status.radar === 'receiving', callbackFault: Boolean(processor.error)})},
    runtime: {processorState: identifierValue(processor.state || status.radar, 48),
      restartState: identifierValue(status.restart?.state, 48),
      backend: identifierValue(status.acceleration?.active || 'cpu'),
      configRevision: /^(?:sha256:)?[a-f0-9]{3,64}$/i.test(status.configRevision || '') ? status.configRevision : null,
      uptimeSeconds: integer(status.uptimeSeconds),
      details: scalars({...incident.details, lastFrameAgeMs: integer(status.lastFrameAt ? Math.max(0, now - status.lastFrameAt) : null),
        timestampConnections: integer(status.timestampConnections), processorFresh: status.processorFresh === true})},
    recentEvents: [], features: normalizeFeatures(status.featureDiagnostics),
    comment: clean(comment, 2000)};
  }
  function retain(report, storage = reportStorage()) { storage.setItem(STORAGE_KEY, JSON.stringify(report)); }
  function clear(storage = reportStorage()) { storage.removeItem(STORAGE_KEY); }
  function pending(storage = reportStorage()) {
    try {
      const value = storage.getItem(STORAGE_KEY); if (!value) return null;
      const report = normalizeReport(JSON.parse(value)); retain(report, storage); return report;
    } catch (_) { clear(storage); return null; }
  }
  async function submit(report, fetchImpl = fetch, storage = reportStorage()) {
    const outgoing = normalizeReport(report); retain(outgoing, storage);
    const response = await fetchImpl(ENDPOINT, {method: 'POST', headers: {'Content-Type': 'application/json', 'X-VectorWarp-Report': '1'}, body: JSON.stringify(outgoing)});
    const result = await response.json().catch(() => ({}));
    if (!response.ok || result.ok !== true || typeof result.reportId !== 'string') throw new Error(result.error || 'Report could not be sent');
    clear(storage); return result.reportId;
  }
  function button(label, className = 'button-secondary') { const value = document.createElement('button'); value.type = 'button'; value.className = className; value.textContent = label; return value; }
  async function open({incident, status, config, comment = '', report: retainedReport = null}) {
    document.getElementById('report-dialog')?.remove();
    const report = normalizeReport(retainedReport || buildReport({incident, status, config, comment}));
    const dialog = document.createElement('dialog'); dialog.id = 'report-dialog'; dialog.className = 'site-dialog report-dialog';
    dialog.innerHTML = '<h2>Send diagnostic report?</h2><p>VectorWarp will send diagnostic information to the project maintainer for analysis and bug fixing. This includes a controlled error category, VectorWarp and system versions, receiver measurements, service state, and aggregate recording, replay, ADS-B, GPU, clutter, and presentation-timing health.</p><p>It will not include your location, network addresses, receiver serial numbers, full configuration, aircraft identities, recording names or contents, raw IQ, or credentials. Your optional description is sanitized before it is sent; do not include passwords or other secrets.</p><p>Select View report to inspect the exact data. If you have privacy concerns or want complete control over what is shared, cancel and file a <a href="https://github.com/mickeyslaven/blah2-VectorWarp/issues/new" target="_blank" rel="noreferrer">GitHub issue</a> instead.</p>';
    const commentLabel = document.createElement('label');
    commentLabel.className = 'report-comment'; commentLabel.textContent = 'What happened?';
    const commentInput = document.createElement('textarea');
    commentInput.rows = 5; commentInput.maxLength = 2000;
    commentInput.placeholder = 'Describe what you were doing, what you expected, and what went wrong.';
    commentInput.value = report.comment || '';
    commentLabel.append(commentInput);
    const preview = document.createElement('pre'); preview.className = 'report-preview'; preview.hidden = true; preview.textContent = JSON.stringify(report, null, 2);
    const state = document.createElement('p'); state.className = 'report-state'; state.setAttribute('role', 'status');
    const actions = document.createElement('div'); actions.className = 'site-dialog-actions';
    const view = button('View report'); const copy = button('Copy report'); copy.hidden = true;
    const cancel = button('Cancel'); const send = button('Send report', 'button-primary');
    const updateComment = () => { report.comment = clean(commentInput.value, 2000); preview.textContent = JSON.stringify(report, null, 2); };
    commentInput.addEventListener('input', updateComment);
    view.onclick = () => { updateComment(); preview.hidden = !preview.hidden; copy.hidden = preview.hidden; view.textContent = preview.hidden ? 'View report' : 'Hide report'; };
    copy.onclick = async () => { await navigator.clipboard.writeText(preview.textContent); state.textContent = 'Report copied.'; };
    const close = () => { dialog.close?.(); dialog.remove(); };
    cancel.onclick = close; dialog.addEventListener('cancel', event => { event.preventDefault(); close(); });
    send.onclick = async () => { updateComment(); send.disabled = true; state.textContent = 'Sending…'; try { const id = await submit(report); state.textContent = `Report sent. Report ID: ${id}`; send.textContent = 'Sent'; cancel.textContent = 'Close'; } catch (error) { retain(report); state.textContent = `${error.message}. The report is retained in this browser; retry or copy it.`; send.disabled = false; send.textContent = 'Retry'; preview.hidden = false; copy.hidden = false; } };
    actions.append(view, copy, cancel, send); dialog.append(commentLabel, preview, state, actions); document.body.append(dialog); dialog.showModal?.(); if (!dialog.open) dialog.setAttribute('open', '');
  }
  function addSettingsEntry(container, context) {
    const retainedReport = pending();
    const strip = document.createElement('section'); strip.className = 'report-support'; strip.innerHTML = '<div><strong>Having trouble with VectorWarp?</strong><span>Send a sanitized diagnostic report.</span></div>';
    const send = button(retainedReport ? 'Retry report' : 'Report an issue');
    send.onclick = () => open(retainedReport ? {report: retainedReport} : context());
    strip.append(send); container.append(strip); return strip;
  }
  function addContextAction(container, context) {
    const existing = container.querySelector('.report-context-action');
    if (!context) { existing?.remove(); return null; }
    const incident = context(); const key = `${incident.incident?._fingerprint || incident.incident?.code}:${incident.incident?._recoveryGeneration || 0}`;
    if (existing?.dataset.incidentKey === key) { existing.onclick = () => open(context()); return existing; }
    existing?.remove(); const action = button('Send report'); action.classList.add('report-context-action');
    action.dataset.incidentKey = key; action.onclick = () => open(context()); container.append(action); return action;
  }
  return {ENDPOINT, STORAGE_KEY, clean, scalars, classify, classifyFailure, buildReport,
    normalizeReport, normalizeFeatures, submit, pending, addSettingsEntry, addContextAction, open};
});
