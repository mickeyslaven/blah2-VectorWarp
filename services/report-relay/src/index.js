const MAX_BODY_BYTES = 64 * 1024;
const MAX_EVENTS = 20;
const GITHUB_API_VERSION = '2026-03-10';
const IPV6_PATTERN = /(?<![A-Fa-f0-9:])(?:(?:[A-Fa-f0-9]{1,4}:){7}[A-Fa-f0-9]{1,4}|(?:[A-Fa-f0-9]{1,4}:){1,7}:|(?:[A-Fa-f0-9]{1,4}:){1,6}:[A-Fa-f0-9]{1,4}|(?:[A-Fa-f0-9]{1,4}:){1,5}(?::[A-Fa-f0-9]{1,4}){1,2}|(?:[A-Fa-f0-9]{1,4}:){1,4}(?::[A-Fa-f0-9]{1,4}){1,3}|(?:[A-Fa-f0-9]{1,4}:){1,3}(?::[A-Fa-f0-9]{1,4}){1,4}|(?:[A-Fa-f0-9]{1,4}:){1,2}(?::[A-Fa-f0-9]{1,4}){1,5}|[A-Fa-f0-9]{1,4}:(?:(?::[A-Fa-f0-9]{1,4}){1,6})|:(?:(?::[A-Fa-f0-9]{1,4}){1,7}|:))(?![A-Fa-f0-9:])/g;
const REPORT_KEYS = new Set([
  'schema', 'consent', 'submissionId', 'incident', 'build', 'system', 'receiver', 'runtime',
  'recentEvents', 'features', 'comment'
]);
const INCIDENT_TYPES = new Map(Object.entries({
  SDR_ENUMERATION_FAILED: ['receiver', 'enumerate'], SDR_OPEN_FAILED: ['receiver', 'open'],
  SDR_STREAM_LOST: ['receiver', 'stream'], RECEIVER_SYNC_FAILED: ['receiver', 'sync'],
  GAIN_MONITOR_FAILED: ['receiver', 'gain-monitor'], PROCESSOR_START_FAILED: ['processor', 'start'],
  PROCESSOR_RUNTIME_FAILED: ['processor', 'runtime'], GPU_BACKEND_FAILED: ['processor', 'acceleration'],
  RESTART_FAILED: ['lifecycle', 'restart'], CONFIG_PERSISTENCE_FAILED: ['configuration', 'persist'],
  API_UNAVAILABLE: ['api', 'connect'], ADSB_SOURCE_FAILED: ['adsb', 'connect'],
  MANUAL_REPORT: ['support', 'manual'], SERVICE_DEGRADED: ['status', 'observe']
}));
const BOOLEAN_DETAILS = new Set(['enumerate', 'apiVersion', 'open', 'init', 'permissionDenied',
  'driverLoaded', 'probeTimedOut', 'rollbackAttempted', 'rollbackSucceeded',
  'reconciliationRequired', 'helperAvailable', 'timedOut', 'receiverReady', 'streamEstablished',
  'callbackFault', 'fallbackAttempted', 'fallbackSucceeded', 'writeAttempted', 'revisionConflict',
  'syntheticLiveTest', 'overloadKnown', 'processorFresh']);
const INTEGER_DETAILS = new Set(['deviceCount', 'candidateCount', 'attemptCount', 'exitStatus',
  'framesReceived', 'lastFrameAgeMs', 'droppedFrames', 'framesProcessed', 'queueDepth', 'sourceCount',
  'healthySourceCount', 'consecutiveFailures', 'statusCode', 'timeoutMs', 'timestampConnections']);
const STRING_DETAILS = new Map(Object.entries({
  discoveryStage: ['enumerate'], driverErrorClass: ['permission', 'timeout', 'disconnected',
    'invalid-response', 'device-busy', 'unavailable'], driverErrorCode: ['DEVICE_BUSY'],
  failurePhase: ['probe', 'monitor', 'adjust'], syncState: ['failed', 'partial', 'indeterminate'],
  receiptState: ['failed'], terminationSignal: ['none', 'SIGINT', 'SIGTERM', 'SIGKILL'],
  driverState: ['failed'], errorClass: ['permission', 'timeout', 'disconnected', 'invalid-response',
    'device-busy', 'unavailable'], writeStage: ['write', 'fsync', 'rename'],
  routeName: ['status', 'config', 'restart']
}).map(([key, values]) => [key, new Set(values)]));

const responseHeaders = {
  'Access-Control-Allow-Origin': '*',
  'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
  'Access-Control-Allow-Headers': 'Content-Type, X-VectorWarp-Report',
  'Access-Control-Max-Age': '86400',
  'Cache-Control': 'no-store',
  'Content-Type': 'application/json; charset=utf-8',
  'Referrer-Policy': 'no-referrer',
  'X-Content-Type-Options': 'nosniff'
};

function json(status, value, extra = {}) {
  return new Response(JSON.stringify(value), {
    status, headers: {...responseHeaders, ...extra}
  });
}

function isObject(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function onlyKeys(value, allowed, name) {
  if (!isObject(value)) throw new ReportError(`${name} must be an object`);
  const unexpected = Object.keys(value).find(key => !allowed.has(key));
  if (unexpected) throw new ReportError(`${name} contains an unsupported field`);
}

function redactText(value, maximum, name, required = false) {
  if (value === undefined || value === null || value === '') {
    if (required) throw new ReportError(`${name} is required`);
    return null;
  }
  if (typeof value !== 'string') throw new ReportError(`${name} must be text`);
  let result = value.normalize('NFKC')
    .replace(/[\u0000-\u001f\u007f]+/g, ' ')
    .replace(/`/g, "'")
    .replace(/\\(["'])/g, '$1')
    .replace(/\b[a-z][a-z0-9+.-]*:\/\/[^\s<>"']+/gi, '[URL redacted]')
    .replace(/\b[\w.+-]+@[\w.-]+\.[A-Za-z]{2,}\b/g, '[email redacted]')
    .replace(/\b(?:\d{1,3}\.){3}\d{1,3}\b/g, '[IP redacted]')
    .replace(IPV6_PATTERN, '[IP redacted]')
    .replace(/\b(?:authorization\s*[:=]\s*)?bearer\s+[A-Za-z0-9._~+\/-]+={0,2}/gi,
      'Bearer [redacted]')
    .replace(/\b(?:authorization\s*[:=]\s*)?basic\s+[A-Za-z0-9+/]+={0,2}/gi,
      'Basic [redacted]')
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
      (_, prefix) => `${prefix}[path redacted]`)
    .replace(/\s+/g, ' ').trim();
  if (!result && required) throw new ReportError(`${name} is required`);
  return result.slice(0, maximum) || null;
}

function boundedInteger(value, minimum, maximum, name, required = false) {
  if (value === undefined || value === null) {
    if (required) throw new ReportError(`${name} is required`);
    return null;
  }
  if (!Number.isSafeInteger(value) || value < minimum || value > maximum)
    throw new ReportError(`${name} is invalid`);
  return value;
}

function booleanOrNull(value, name) {
  if (value === undefined || value === null) return null;
  if (typeof value !== 'boolean') throw new ReportError(`${name} must be boolean`);
  return value;
}

function timestamp(value, name) {
  if (value === undefined || value === null || value === '') return null;
  if (typeof value !== 'string' || value.length > 40 || !Number.isFinite(Date.parse(value)))
    throw new ReportError(`${name} must be an ISO timestamp`);
  return new Date(value).toISOString();
}

function enumValue(value, choices, name, fallback = null) {
  if (value === undefined || value === null || value === '') return fallback;
  if (!choices.has(value)) throw new ReportError(`${name} is unsupported`);
  return value;
}

function identifier(value, maximum, name, required = false) {
  const result = redactText(value, maximum, name, required);
  if (result !== null && !/^[A-Za-z0-9][A-Za-z0-9_.-]*$/.test(result))
    throw new ReportError(`${name} must be a stable identifier`);
  return result;
}

function releaseIdentifier(value, maximum, name, required = false) {
  const result = redactText(value, maximum, name, required);
  if (result !== null && !/^[A-Za-z0-9][A-Za-z0-9_.+-]*$/.test(result))
    throw new ReportError(`${name} must be a stable release identifier`);
  return result;
}

function safeDetails(value, name) {
  if (value === undefined || value === null) return {};
  if (!isObject(value)) throw new ReportError(`${name} must be an object`);
  const entries = Object.entries(value);
  if (entries.length > 40) throw new ReportError(`${name} has too many fields`);
  const result = {};
  for (const [key, item] of entries) {
    if (BOOLEAN_DETAILS.has(key) && typeof item === 'boolean') result[key] = item;
    else if (INTEGER_DETAILS.has(key) && Number.isSafeInteger(item) && item >= 0) result[key] = item;
    else if (STRING_DETAILS.get(key)?.has(item)) result[key] = item;
    else if ((BOOLEAN_DETAILS.has(key) || INTEGER_DETAILS.has(key) || STRING_DETAILS.has(key)) && item === null)
      result[key] = null;
    else throw new ReportError(`${name} contains an unsupported diagnostic field or value`);
  }
  return result;
}

function diagnosticObject(value, allowed, name) {
  value = value ?? {};
  onlyKeys(value, allowed, name);
  return value;
}

function normalizeFeatures(value) {
  value = diagnosticObject(value, new Set(['recording', 'replay', 'adsb', 'processing', 'presentation']), 'features');
  const recording = diagnosticObject(value.recording, new Set(['requested', 'paused', 'active',
    'errorPresent', 'recordedSamples', 'clipRevision', 'clipErrorPresent', 'clipSamples',
    'timelineCaptureAvailable', 'automaticCleanup', 'lowSpacePolicy', 'maximumAgeDays',
    'maximumTotalGb', 'minimumFreeGb']), 'features.recording');
  const replay = diagnosticObject(value.replay, new Set(['active', 'paused', 'seekable',
    'positionSamples', 'totalSamples', 'sampleRateHz', 'timelineAvailable']), 'features.replay');
  const adsb = diagnosticObject(value.adsb, new Set(['enabled', 'online', 'degraded', 'sourceCount',
    'healthySourceCount', 'aircraftCount', 'directAircraftCount', 'mlatAircraftCount',
    'associationLagCpis', 'associationLagMs']), 'features.adsb');
  const processing = diagnosticObject(value.processing, new Set(['acceleration',
    'clutterAcceleration', 'configuredCpiMs']), 'features.processing');
  const presentation = diagnosticObject(value.presentation, new Set(['timestampConnections',
    'associationLagCpis', 'associationLagMs']), 'features.presentation');
  const positive = (entry, name) => boundedInteger(entry, 0, Number.MAX_SAFE_INTEGER, name);
  return {
    recording: {requested: booleanOrNull(recording.requested, 'features.recording.requested'),
      paused: booleanOrNull(recording.paused, 'features.recording.paused'),
      active: booleanOrNull(recording.active, 'features.recording.active'),
      errorPresent: booleanOrNull(recording.errorPresent, 'features.recording.errorPresent'),
      recordedSamples: positive(recording.recordedSamples, 'features.recording.recordedSamples'),
      clipRevision: positive(recording.clipRevision, 'features.recording.clipRevision'),
      clipErrorPresent: booleanOrNull(recording.clipErrorPresent, 'features.recording.clipErrorPresent'),
      clipSamples: positive(recording.clipSamples, 'features.recording.clipSamples'),
      timelineCaptureAvailable: booleanOrNull(recording.timelineCaptureAvailable, 'features.recording.timelineCaptureAvailable'),
      automaticCleanup: booleanOrNull(recording.automaticCleanup, 'features.recording.automaticCleanup'),
      lowSpacePolicy: enumValue(recording.lowSpacePolicy, new Set(['stop_recording', 'delete_oldest']),
        'features.recording.lowSpacePolicy'),
      maximumAgeDays: positive(recording.maximumAgeDays, 'features.recording.maximumAgeDays'),
      maximumTotalGb: positive(recording.maximumTotalGb, 'features.recording.maximumTotalGb'),
      minimumFreeGb: positive(recording.minimumFreeGb, 'features.recording.minimumFreeGb')},
    replay: {active: booleanOrNull(replay.active, 'features.replay.active'),
      paused: booleanOrNull(replay.paused, 'features.replay.paused'),
      seekable: booleanOrNull(replay.seekable, 'features.replay.seekable'),
      positionSamples: positive(replay.positionSamples, 'features.replay.positionSamples'),
      totalSamples: positive(replay.totalSamples, 'features.replay.totalSamples'),
      sampleRateHz: positive(replay.sampleRateHz, 'features.replay.sampleRateHz'),
      timelineAvailable: booleanOrNull(replay.timelineAvailable, 'features.replay.timelineAvailable')},
    adsb: {enabled: booleanOrNull(adsb.enabled, 'features.adsb.enabled'),
      online: booleanOrNull(adsb.online, 'features.adsb.online'),
      degraded: booleanOrNull(adsb.degraded, 'features.adsb.degraded'),
      sourceCount: positive(adsb.sourceCount, 'features.adsb.sourceCount'),
      healthySourceCount: positive(adsb.healthySourceCount, 'features.adsb.healthySourceCount'),
      aircraftCount: positive(adsb.aircraftCount, 'features.adsb.aircraftCount'),
      directAircraftCount: positive(adsb.directAircraftCount, 'features.adsb.directAircraftCount'),
      mlatAircraftCount: positive(adsb.mlatAircraftCount, 'features.adsb.mlatAircraftCount'),
      associationLagCpis: positive(adsb.associationLagCpis, 'features.adsb.associationLagCpis'),
      associationLagMs: positive(adsb.associationLagMs, 'features.adsb.associationLagMs')},
    processing: {acceleration: identifier(processing.acceleration, 64, 'features.processing.acceleration'),
      clutterAcceleration: identifier(processing.clutterAcceleration, 64, 'features.processing.clutterAcceleration'),
      configuredCpiMs: positive(processing.configuredCpiMs, 'features.processing.configuredCpiMs')},
    presentation: {timestampConnections: positive(presentation.timestampConnections, 'features.presentation.timestampConnections'),
      associationLagCpis: positive(presentation.associationLagCpis, 'features.presentation.associationLagCpis'),
      associationLagMs: positive(presentation.associationLagMs, 'features.presentation.associationLagMs')}
  };
}

class ReportError extends Error {
  constructor(message, status = 422, ambiguous = false) {
    super(message); this.status = status; this.ambiguous = ambiguous;
  }
}

function normalizeIncident(value, prefix = 'incident') {
  const allowed = new Set([
    'code', 'component', 'stage', 'severity', 'message', 'firstSeenAt',
    'lastSeenAt', 'occurrences', 'recovered'
  ]);
  onlyKeys(value, allowed, prefix);
  const code = identifier(value.code, 64, `${prefix}.code`, true);
  const kind = INCIDENT_TYPES.get(code);
  if (!kind || value.component !== kind[0] || value.stage !== kind[1])
    throw new ReportError(`${prefix} is not a supported diagnostic incident`);
  if (typeof value.message !== 'string' || value.message.length > 4096)
    throw new ReportError(`${prefix}.message must be bounded text`);
  return {
    code,
    component: kind[0],
    stage: kind[1],
    severity: enumValue(value.severity, new Set(['fatal', 'error', 'degraded']),
      `${prefix}.severity`, 'error'),
    message: `VectorWarp detected ${code} during ${kind[0]} ${kind[1]}.`,
    firstSeenAt: timestamp(value.firstSeenAt, `${prefix}.firstSeenAt`),
    lastSeenAt: timestamp(value.lastSeenAt, `${prefix}.lastSeenAt`),
    occurrences: boundedInteger(value.occurrences, 1, 1000000,
      `${prefix}.occurrences`) ?? 1,
    recovered: booleanOrNull(value.recovered, `${prefix}.recovered`)
  };
}

export function normalizeReport(value) {
  onlyKeys(value, REPORT_KEYS, 'report');
  if (value.schema !== 1) throw new ReportError('report.schema must be 1');
  if (value.consent !== true) throw new ReportError('report.consent must be true');
  if (typeof value.submissionId !== 'string' ||
      !/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i.test(value.submissionId))
    throw new ReportError('report.submissionId must be a UUID');

  onlyKeys(value.build, new Set(['version', 'commit', 'package']), 'build');
  const commit = redactText(value.build.commit, 40, 'build.commit', true);
  if (!/^[a-f0-9]{7,40}$/i.test(commit)) throw new ReportError('build.commit is invalid');

  onlyKeys(value.system, new Set(['os', 'architecture', 'kernel', 'browser']), 'system');
  const receiver = value.receiver ?? {};
  onlyKeys(receiver, new Set([
    'type', 'model', 'driverVersion', 'apiVersion', 'firmwareVersion',
    'sampleRateHz', 'centerFrequencyHz', 'channelCount', 'transport',
    'startupStages', 'health'
  ]), 'receiver');
  const runtime = value.runtime ?? {};
  onlyKeys(runtime, new Set([
    'processorState', 'restartState', 'backend', 'configRevision',
    'uptimeSeconds', 'details'
  ]), 'runtime');

  const recentEvents = value.recentEvents ?? [];
  if (!Array.isArray(recentEvents) || recentEvents.length > MAX_EVENTS)
    throw new ReportError(`recentEvents must contain at most ${MAX_EVENTS} incidents`);

  return {
    schema: 1,
    submissionId: value.submissionId.toLowerCase(),
    incident: normalizeIncident(value.incident),
    build: {
      version: releaseIdentifier(value.build.version, 64, 'build.version', true),
      commit: commit.toLowerCase(),
      package: releaseIdentifier(value.build.package, 64, 'build.package')
    },
    system: {
      os: enumValue(value.system.os, new Set(['Linux', 'Darwin', 'Windows', 'linux', 'darwin',
        'win32', 'macOS', 'unknown']), 'system.os', 'unknown'),
      architecture: identifier(value.system.architecture, 32, 'system.architecture', true),
      kernel: releaseIdentifier(value.system.kernel, 128, 'system.kernel'), browser: null
    },
    receiver: {
      type: enumValue(receiver.type,
        new Set(['RspDuo', 'Kraken', 'Usrp', 'HackRF', 'Replay', 'Unknown']),
        'receiver.type', 'Unknown'),
      model: null,
      driverVersion: releaseIdentifier(receiver.driverVersion, 64, 'receiver.driverVersion'),
      apiVersion: releaseIdentifier(receiver.apiVersion, 64, 'receiver.apiVersion'),
      firmwareVersion: releaseIdentifier(receiver.firmwareVersion, 64, 'receiver.firmwareVersion'),
      sampleRateHz: boundedInteger(receiver.sampleRateHz, 1, 1000000000, 'receiver.sampleRateHz'),
      centerFrequencyHz: boundedInteger(receiver.centerFrequencyHz, 1, 100000000000, 'receiver.centerFrequencyHz'),
      channelCount: boundedInteger(receiver.channelCount, 1, 64, 'receiver.channelCount'),
      transport: enumValue(receiver.transport, new Set(['usb', 'network', 'replay', 'unknown']),
        'receiver.transport'),
      startupStages: safeDetails(receiver.startupStages, 'receiver.startupStages'),
      health: safeDetails(receiver.health, 'receiver.health')
    },
    runtime: {
      processorState: identifier(runtime.processorState, 48, 'runtime.processorState'),
      restartState: identifier(runtime.restartState, 48, 'runtime.restartState'),
      backend: identifier(runtime.backend, 64, 'runtime.backend'),
      configRevision: typeof runtime.configRevision === 'string' &&
        /^(?:sha256:)?[a-f0-9]{3,64}$/i.test(runtime.configRevision) ? runtime.configRevision : null,
      uptimeSeconds: boundedInteger(runtime.uptimeSeconds, 0, Number.MAX_SAFE_INTEGER,
        'runtime.uptimeSeconds'),
      details: safeDetails(runtime.details, 'runtime.details')
    },
    recentEvents: recentEvents.map((event, index) => normalizeIncident(event,
      `recentEvents[${index}]`)), features: normalizeFeatures(value.features),
    comment: redactText(value.comment, 2000, 'comment')
  };
}

async function digest(value) {
  const bytes = new TextEncoder().encode(value);
  const result = await crypto.subtle.digest('SHA-256', bytes);
  return [...new Uint8Array(result)].map(byte => byte.toString(16).padStart(2, '0')).join('');
}

export async function reportFingerprint(report) {
  const source = [report.incident.code, report.incident.component,
    report.incident.stage || '', report.build.version, report.receiver.type].join('\n');
  return (await digest(source)).slice(0, 16);
}

export async function submissionReceiptId(report) {
  return (await digest(`vectorwarp-submission\n${report.submissionId}`)).slice(0, 24);
}

export function githubIssue(report, fingerprint, reportId, labels = '') {
  const title = `[VectorWarp report] ${report.incident.code} in ${report.incident.component}`
    .slice(0, 256);
  const body = [
    '> Anonymous, user-approved diagnostic report submitted by VectorWarp.',
    '',
    `**Incident:** ${report.incident.code}`,
    `**Component:** ${report.incident.component}`,
    `**Severity:** ${report.incident.severity}`,
    `**Build:** ${report.build.version} (${report.build.commit.slice(0, 12)})`,
    `**Receiver:** ${report.receiver.type}`,
    `**Fingerprint:** \`${fingerprint}\``,
    `**Submission:** \`${reportId}\``,
    '',
    '### Diagnostic report',
    '',
    '```json',
    JSON.stringify(report, null, 2),
    '```'
  ].join('\n');
  const result = {title, body};
  const parsedLabels = labels.split(',').map(value => value.trim()).filter(Boolean).slice(0, 10);
  if (parsedLabels.length) result.labels = parsedLabels;
  return result;
}

async function parseBody(request) {
  const contentType = request.headers.get('content-type') || '';
  if (!/^application\/json(?:\s*;|$)/i.test(contentType))
    throw new ReportError('Content-Type must be application/json', 415);
  const declared = Number(request.headers.get('content-length'));
  if (Number.isFinite(declared) && declared > MAX_BODY_BYTES)
    throw new ReportError('Report is too large', 413);
  if (!request.body?.getReader) throw new ReportError('Report body is required', 400);
  const reader = request.body.getReader();
  const chunks = []; let size = 0;
  while (true) {
    const {done, value} = await reader.read();
    if (done) break;
    size += value.byteLength;
    if (size > MAX_BODY_BYTES) {
      await reader.cancel('Report is too large').catch(() => {});
      throw new ReportError('Report is too large', 413);
    }
    chunks.push(value);
  }
  const body = new Uint8Array(size); let offset = 0;
  for (const chunk of chunks) { body.set(chunk, offset); offset += chunk.byteLength; }
  let text;
  try { text = new TextDecoder('utf-8', {fatal: true}).decode(body); }
  catch (_) { throw new ReportError('Report is not valid UTF-8', 400); }
  try { return JSON.parse(text); }
  catch (_) { throw new ReportError('Report is not valid JSON', 400); }
}

async function rateLimit(request, env) {
  if (!env.REPORT_RATE_LIMITER?.limit)
    throw new ReportError('Reporting service is not configured', 503);
  const source = request.headers.get('cf-connecting-ip') || 'unknown';
  const key = await digest(`vectorwarp-report:${source}`);
  return (await env.REPORT_RATE_LIMITER.limit({key})).success;
}

export async function createIssue(report, env, fetcher) {
  if (!env.GITHUB_TOKEN || !/^[\w.-]+\/[\w.-]+$/.test(env.GITHUB_REPOSITORY || ''))
    throw new ReportError('Reporting service is not configured', 503);
  const fingerprint = await reportFingerprint(report);
  const reportId = await submissionReceiptId(report);
  const issue = githubIssue(report, fingerprint, reportId, env.REPORT_LABELS || '');
  let response;
  try {
    response = await fetcher(`https://api.github.com/repos/${env.GITHUB_REPOSITORY}/issues`, {
      method: 'POST',
      headers: {
      'Accept': 'application/vnd.github+json',
      'Authorization': `Bearer ${env.GITHUB_TOKEN}`,
      'Content-Type': 'application/json',
      'User-Agent': 'vectorwarp-report-relay',
      'X-GitHub-Api-Version': GITHUB_API_VERSION
      },
      body: JSON.stringify(issue)
    });
  } catch (_) {
    throw new ReportError('Report destination is temporarily unavailable', 502, true);
  }
  if (!response.ok)
    throw new ReportError('Report destination is temporarily unavailable', 502, response.status >= 500);
  let result;
  try { result = await response.json(); }
  catch (_) { throw new ReportError('Report destination returned an invalid response', 502, true); }
  if (!Number.isSafeInteger(result.number) || typeof result.html_url !== 'string' ||
      !result.html_url.startsWith('https://github.com/'))
    throw new ReportError('Report destination returned an invalid response', 502, true);
  return {reportId};
}

async function reconcileIssue(report, env, fetcher) {
  if (!env.GITHUB_TOKEN || !/^[\w.-]+\/[\w.-]+$/.test(env.GITHUB_REPOSITORY || ''))
    throw new ReportError('Reporting service is not configured', 503);
  const reportId = await submissionReceiptId(report);
  const query = encodeURIComponent(`repo:${env.GITHUB_REPOSITORY} is:issue "${reportId}" in:body`);
  let response;
  try {
    response = await fetcher(`https://api.github.com/search/issues?q=${query}&per_page=10`, {headers: {
      'Accept': 'application/vnd.github+json', 'Authorization': `Bearer ${env.GITHUB_TOKEN}`,
      'User-Agent': 'vectorwarp-report-relay', 'X-GitHub-Api-Version': GITHUB_API_VERSION
    }});
  } catch (_) { throw new ReportError('Report status is temporarily unavailable', 503, true); }
  if (!response.ok) throw new ReportError('Report status is temporarily unavailable', 503, true);
  let value;
  try { value = await response.json(); }
  catch (_) { throw new ReportError('Report status is temporarily unavailable', 503, true); }
  const marker = `**Submission:** \`${reportId}\``;
  return Array.isArray(value.items) && value.items.some(item => typeof item?.body === 'string' &&
    item.body.includes(marker) && typeof item.html_url === 'string' &&
    item.html_url.startsWith(`https://github.com/${env.GITHUB_REPOSITORY}/issues/`));
}

export class ReportDeduplicator {
  constructor(state, env) { this.state = state; this.env = env; this.inflight = null; }
  async fetch(request) {
    try {
      const report = await request.json();
      const reportId = await submissionReceiptId(report);
      const existing = await this.state.storage.get('submission');
      if (existing?.phase === 'complete' && existing.reportId === reportId)
        return json(200, {ok: true, reportId});
      if (!this.inflight) this.inflight = (async () => {
        if (existing?.phase === 'pending') {
          if (existing.reportId !== reportId)
            throw new ReportError('Submission identity conflict', 409);
          if (await reconcileIssue(report, this.env, globalThis.fetch)) {
            await this.state.storage.put('submission', {phase: 'complete', reportId});
            return {reportId, reconciled: true};
          }
          throw new ReportError('Report submission is pending; retry later', 503, true);
        }
        await this.state.storage.put('submission', {phase: 'pending', reportId});
        let result;
        try {
          result = await createIssue(report, this.env, globalThis.fetch);
        } catch (error) {
          if (!error?.ambiguous) await this.state.storage.delete('submission');
          throw error;
        }
        await this.state.storage.put('submission', {phase: 'complete', reportId});
        return result;
      })().finally(() => { this.inflight = null; });
      const result = await this.inflight;
      return json(result.reconciled ? 200 : 201, {ok: true, reportId: result.reportId});
    } catch (error) {
      if (error instanceof ReportError) return json(error.status, {ok: false, error: error.message});
      return json(500, {ok: false, error: 'Report could not be submitted'});
    }
  }
}

async function deduplicatedIssue(report, env, dependencies) {
  if (dependencies.deduplicate) return dependencies.deduplicate(report, env,
    dependencies.fetch || globalThis.fetch);
  if (!env.REPORT_DEDUPLICATOR?.idFromName || !env.REPORT_DEDUPLICATOR?.get)
    throw new ReportError('Reporting service is not configured', 503);
  const reportId = await submissionReceiptId(report);
  const stub = env.REPORT_DEDUPLICATOR.get(env.REPORT_DEDUPLICATOR.idFromName(reportId));
  const response = await stub.fetch('https://report-deduplicator.internal/', {
    method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(report)
  });
  const result = await response.json().catch(() => ({}));
  if (!response.ok || result.ok !== true || result.reportId !== reportId)
    throw new ReportError(result.error || 'Report destination is temporarily unavailable', response.status || 502);
  return {reportId};
}

export async function handleRequest(request, env = {}, dependencies = {}) {
  const url = new URL(request.url);
  if (request.method === 'OPTIONS') return new Response(null, {status: 204, headers: responseHeaders});
  if (request.method === 'GET' && url.pathname === '/health')
    return json(200, {ok: true, service: 'vectorwarp-report-relay', schema: 1});
  if (url.pathname !== '/v1/reports') return json(404, {ok: false, error: 'Not found'});
  if (request.method !== 'POST')
    return json(405, {ok: false, error: 'Method not allowed'}, {Allow: 'POST, OPTIONS'});
  try {
    if (request.headers.get('x-vectorwarp-report') !== '1')
      throw new ReportError('VectorWarp report header is required', 403);
    if (!await rateLimit(request, env))
      return json(429, {ok: false, error: 'Too many reports; try again later'}, {'Retry-After': '60'});
    const report = normalizeReport(await parseBody(request));
    const created = await deduplicatedIssue(report, env, dependencies);
    return json(201, {ok: true, ...created});
  } catch (error) {
    if (error instanceof ReportError)
      return json(error.status, {ok: false, error: error.message});
    return json(500, {ok: false, error: 'Report could not be submitted'});
  }
}

export default {
  fetch(request, env) { return handleRequest(request, env); }
};
