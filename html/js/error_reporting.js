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
  const sensitiveKey = /(serial|token|secret|password|credential|location|latitude|longitude|hostname|address|endpoint|path|file|url|apikey|authorization|adsb|flight|aircraft|icao|callsign|hex)/;
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
      .replace(/\b[a-z][a-z0-9+.-]*:\/\/[^\s<>"']+/gi, '[URL redacted]')
      .replace(/\b[\w.+-]+@[\w.-]+\.[A-Za-z]{2,}\b/g, '[email redacted]')
      .replace(/\b(?:\d{1,3}\.){3}\d{1,3}\b/g, '[IP redacted]')
      .replace(IPV6_PATTERN, '[IP redacted]')
      .replace(/\b(?:authorization\s*[:=]\s*)?bearer\s+[A-Za-z0-9._~+\/-]+={0,2}/gi, 'Bearer [redacted]')
      .replace(/["']?(?:serial(?:[\s_-]*number)?|access[_-]?token(?:value)?|api[_-]?key|token|password|secret|hostname|host|endpoint|address|aircraft[\s_-]*hex|hex|icao|callsign|flight)["']?\s*(?::|=|\s)\s*(?:"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|[^\s,;]+)/gi,
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
      const canonical = key.replace(/[^A-Za-z0-9]/g, '').toLowerCase();
      if (!/^[a-z][a-zA-Z0-9_]{0,47}$/.test(key) || sensitiveKey.test(canonical) ||
          /^(?:lat(?:deg|itude)?|lon(?:deg|gitude)?|ip)$/.test(canonical)) continue;
      if (typeof item === 'boolean' || (typeof item === 'number' && Number.isFinite(item)) || item === null)
        result[key] = item;
      else if (typeof item === 'string') result[key] = clean(item);
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
  function buildReport({incident, status = {}, config = {}, comment = '', now = Date.now()}) {
    const browser = typeof navigator === 'undefined' ? {} : navigator;
    const type = receiverType(status, config);
    const processor = status.processor || {};
    const frequency = config?.capture?.fc;
    const sampleRate = config?.capture?.fs;
    const channels = config?.capture?.device?.channel_count;
    const message = clean(incident.message || 'User requested diagnostic report', 1024);
    return {schema: 1, consent: true, submissionId: uuid(), incident: {
      code: incident.code, component: incident.component, stage: incident.stage,
      severity: incident.severity || 'error', message,
      firstSeenAt: new Date(incident.firstSeenAt || now).toISOString(),
      lastSeenAt: new Date(now).toISOString(), occurrences: integer(incident.occurrences, 1),
      recovered: incident.recovered === true
    }, build: {version: clean(status.build?.version || 'development', 64),
      commit: /^[a-f0-9]{7,40}$/i.test(status.build?.commit || '') ? status.build.commit : '0000000',
      package: clean(status.build?.package || 'unknown', 64)},
    system: {os: clean(status.system?.os || browser.platform || 'unknown', 64),
      architecture: clean(status.system?.architecture || 'unknown', 32),
      kernel: clean(status.system?.kernel, 128), browser: clean(browser.userAgent, 128)},
    receiver: {type, model: clean(status.processor?.receiverModel, 96),
      driverVersion: clean(processor.driverVersion, 64), apiVersion: clean(processor.apiVersion, 64),
      firmwareVersion: clean(processor.firmwareVersion, 64), sampleRateHz: integer(processor.sampleRate || sampleRate),
      centerFrequencyHz: integer(processor.frequency || frequency), channelCount: integer(processor.channels || channels),
      transport: clean(processor.transport, 64), startupStages: scalars(processor.receiverStartup?.sdk?.stages),
      health: scalars({streamEstablished: status.radar === 'receiving', callbackFault: Boolean(processor.error)})},
    runtime: {processorState: clean(processor.state || status.radar, 48),
      restartState: clean(status.restart?.state, 48), backend: clean(status.acceleration?.active || 'cpu', 64),
      configRevision: clean(status.configRevision, 128), uptimeSeconds: integer(status.uptimeSeconds),
      details: scalars({...incident.details, lastFrameAgeMs: integer(status.lastFrameAt ? Math.max(0, now - status.lastFrameAt) : null),
        timestampConnections: integer(status.timestampConnections), processorFresh: status.processorFresh === true})},
    recentEvents: [], comment: clean(comment, 2000)};
  }
  function retain(report, storage = reportStorage()) { storage.setItem(STORAGE_KEY, JSON.stringify(report)); }
  function clear(storage = reportStorage()) { storage.removeItem(STORAGE_KEY); }
  function pending(storage = reportStorage()) { try { return JSON.parse(storage.getItem(STORAGE_KEY)); } catch (_) { return null; } }
  async function submit(report, fetchImpl = fetch, storage = reportStorage()) {
    retain(report, storage);
    const response = await fetchImpl(ENDPOINT, {method: 'POST', headers: {'Content-Type': 'application/json', 'X-VectorWarp-Report': '1'}, body: JSON.stringify(report)});
    const result = await response.json().catch(() => ({}));
    if (!response.ok || result.ok !== true || typeof result.reportId !== 'string') throw new Error(result.error || 'Report could not be sent');
    clear(storage); return result.reportId;
  }
  function button(label, className = 'button-secondary') { const value = document.createElement('button'); value.type = 'button'; value.className = className; value.textContent = label; return value; }
  async function open({incident, status, config, comment = '', report: retainedReport = null}) {
    document.getElementById('report-dialog')?.remove();
    const report = retainedReport || buildReport({incident, status, config, comment});
    const dialog = document.createElement('dialog'); dialog.id = 'report-dialog'; dialog.className = 'site-dialog report-dialog';
    dialog.innerHTML = '<h2>Send diagnostic report?</h2><p>VectorWarp will send diagnostic information to the project maintainer for analysis and bug fixing. This includes the error, VectorWarp and system versions, receiver type and operating details, service state, and a small sanitized diagnostic history.</p><p>It will not include your location, network addresses, receiver serial numbers, full configuration, ADS-B data, recordings, raw IQ, or credentials.</p><p>Select View report to inspect the exact data. If you have privacy concerns or want complete control over what is shared, cancel and file a <a href="https://github.com/mickeyslaven/blah2-VectorWarp/issues/new" target="_blank" rel="noreferrer">GitHub issue</a> instead.</p>';
    const preview = document.createElement('pre'); preview.className = 'report-preview'; preview.hidden = true; preview.textContent = JSON.stringify(report, null, 2);
    const state = document.createElement('p'); state.className = 'report-state'; state.setAttribute('role', 'status');
    const actions = document.createElement('div'); actions.className = 'site-dialog-actions';
    const view = button('View report'); const copy = button('Copy report'); copy.hidden = true;
    const cancel = button('Cancel'); const send = button('Send report', 'button-primary');
    view.onclick = () => { preview.hidden = !preview.hidden; copy.hidden = preview.hidden; view.textContent = preview.hidden ? 'View report' : 'Hide report'; };
    copy.onclick = async () => { await navigator.clipboard.writeText(preview.textContent); state.textContent = 'Report copied.'; };
    const close = () => { dialog.close?.(); dialog.remove(); };
    cancel.onclick = close; dialog.addEventListener('cancel', event => { event.preventDefault(); close(); });
    send.onclick = async () => { send.disabled = true; state.textContent = 'Sending…'; try { const id = await submit(report); state.textContent = `Report sent. Report ID: ${id}`; send.textContent = 'Sent'; cancel.textContent = 'Close'; } catch (error) { retain(report); state.textContent = `${error.message}. The report is retained in this browser; retry or copy it.`; send.disabled = false; send.textContent = 'Retry'; preview.hidden = false; copy.hidden = false; } };
    actions.append(view, copy, cancel, send); dialog.append(preview, state, actions); document.body.append(dialog); dialog.showModal?.(); if (!dialog.open) dialog.setAttribute('open', '');
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
  return {ENDPOINT, STORAGE_KEY, clean, scalars, classify, classifyFailure, buildReport, submit, pending, addSettingsEntry, addContextAction, open};
});
