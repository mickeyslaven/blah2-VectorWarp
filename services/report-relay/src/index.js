const MAX_BODY_BYTES = 64 * 1024;
const MAX_EVENTS = 20;
const GITHUB_API_VERSION = '2026-03-10';
const IPV6_PATTERN = /(?<![A-Fa-f0-9:])(?:(?:[A-Fa-f0-9]{1,4}:){7}[A-Fa-f0-9]{1,4}|(?:[A-Fa-f0-9]{1,4}:){1,7}:|(?:[A-Fa-f0-9]{1,4}:){1,6}:[A-Fa-f0-9]{1,4}|(?:[A-Fa-f0-9]{1,4}:){1,5}(?::[A-Fa-f0-9]{1,4}){1,2}|(?:[A-Fa-f0-9]{1,4}:){1,4}(?::[A-Fa-f0-9]{1,4}){1,3}|(?:[A-Fa-f0-9]{1,4}:){1,3}(?::[A-Fa-f0-9]{1,4}){1,4}|(?:[A-Fa-f0-9]{1,4}:){1,2}(?::[A-Fa-f0-9]{1,4}){1,5}|[A-Fa-f0-9]{1,4}:(?:(?::[A-Fa-f0-9]{1,4}){1,6})|:(?:(?::[A-Fa-f0-9]{1,4}){1,7}|:))(?![A-Fa-f0-9:])/g;
const REPORT_KEYS = new Set([
  'schema', 'consent', 'submissionId', 'incident', 'build', 'system', 'receiver', 'runtime',
  'recentEvents', 'comment'
]);
const SENSITIVE_KEY = /(serial|token|secret|password|credential|location|latitude|longitude|hostname|address|endpoint|path|file|url|apikey|authorization|adsb|flight|aircraft|icao|callsign|hex)/;

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
    .replace(/\b[a-z][a-z0-9+.-]*:\/\/[^\s<>"']+/gi, '[URL redacted]')
    .replace(/\b[\w.+-]+@[\w.-]+\.[A-Za-z]{2,}\b/g, '[email redacted]')
    .replace(/\b(?:\d{1,3}\.){3}\d{1,3}\b/g, '[IP redacted]')
    .replace(IPV6_PATTERN, '[IP redacted]')
    .replace(/\b(?:authorization\s*[:=]\s*)?bearer\s+[A-Za-z0-9._~+\/-]+={0,2}/gi,
      'Bearer [redacted]')
    .replace(/["']?(?:serial(?:[\s_-]*number)?|access[_-]?token(?:value)?|api[_-]?key|token|password|secret|hostname|host|endpoint|address|aircraft[\s_-]*hex|hex|icao|callsign|flight)["']?\s*(?::|=|\s)\s*(?:"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|[^\s,;]+)/gi,
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
    const canonical = key.replace(/[^A-Za-z0-9]/g, '').toLowerCase();
    if (!/^[a-z][a-zA-Z0-9_]{0,47}$/.test(key) || SENSITIVE_KEY.test(canonical) ||
        /^(?:lat(?:deg|itude)?|lon(?:deg|gitude)?|ip)$/.test(canonical))
      throw new ReportError(`${name} contains a sensitive or unsupported field`);
    if (typeof item === 'boolean') result[key] = item;
    else if (typeof item === 'number' && Number.isFinite(item)) result[key] = item;
    else if (typeof item === 'string') result[key] = redactText(item, 256, `${name}.${key}`);
    else if (item === null) result[key] = null;
    else throw new ReportError(`${name}.${key} must be a scalar value`);
  }
  return result;
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
  const code = redactText(value.code, 64, `${prefix}.code`, true);
  if (!/^[A-Z][A-Z0-9_]{2,63}$/.test(code))
    throw new ReportError(`${prefix}.code must be a stable uppercase code`);
  return {
    code,
    component: identifier(value.component, 64, `${prefix}.component`, true),
    stage: identifier(value.stage, 64, `${prefix}.stage`),
    severity: enumValue(value.severity, new Set(['fatal', 'error', 'degraded']),
      `${prefix}.severity`, 'error'),
    message: redactText(value.message, 1024, `${prefix}.message`, true),
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
      os: redactText(value.system.os, 64, 'system.os', true),
      architecture: redactText(value.system.architecture, 32, 'system.architecture', true),
      kernel: redactText(value.system.kernel, 128, 'system.kernel'),
      browser: redactText(value.system.browser, 128, 'system.browser')
    },
    receiver: {
      type: enumValue(receiver.type,
        new Set(['RspDuo', 'Kraken', 'Usrp', 'HackRF', 'Replay', 'Unknown']),
        'receiver.type', 'Unknown'),
      model: redactText(receiver.model, 96, 'receiver.model'),
      driverVersion: redactText(receiver.driverVersion, 64, 'receiver.driverVersion'),
      apiVersion: redactText(receiver.apiVersion, 64, 'receiver.apiVersion'),
      firmwareVersion: redactText(receiver.firmwareVersion, 64, 'receiver.firmwareVersion'),
      sampleRateHz: boundedInteger(receiver.sampleRateHz, 1, 1000000000, 'receiver.sampleRateHz'),
      centerFrequencyHz: boundedInteger(receiver.centerFrequencyHz, 1, 100000000000, 'receiver.centerFrequencyHz'),
      channelCount: boundedInteger(receiver.channelCount, 1, 64, 'receiver.channelCount'),
      transport: redactText(receiver.transport, 64, 'receiver.transport'),
      startupStages: safeDetails(receiver.startupStages, 'receiver.startupStages'),
      health: safeDetails(receiver.health, 'receiver.health')
    },
    runtime: {
      processorState: redactText(runtime.processorState, 48, 'runtime.processorState'),
      restartState: redactText(runtime.restartState, 48, 'runtime.restartState'),
      backend: redactText(runtime.backend, 64, 'runtime.backend'),
      configRevision: redactText(runtime.configRevision, 128, 'runtime.configRevision'),
      uptimeSeconds: boundedInteger(runtime.uptimeSeconds, 0, Number.MAX_SAFE_INTEGER,
        'runtime.uptimeSeconds'),
      details: safeDetails(runtime.details, 'runtime.details')
    },
    recentEvents: recentEvents.map((event, index) => normalizeIncident(event,
      `recentEvents[${index}]`)),
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
