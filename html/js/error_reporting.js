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
      .replace(/\bhttps?:\/\/[^\s<>]+/gi, '[URL redacted]')
      .replace(/\b[\w.+-]+@[\w.-]+\.[A-Za-z]{2,}\b/g, '[email redacted]')
      .replace(/\b(?:\d{1,3}\.){3}\d{1,3}\b/g, '[IP redacted]')
      .replace(IPV6_PATTERN, '[IP redacted]')
      .replace(/\b(?:serial|token|password|secret|hostname|host|endpoint|address)\s*[:=]\s*[^\s,;]+/gi,
        match => `${match.split(/\s*[:=]\s*/)[0]}=[redacted]`)
      .replace(/\b(?:lat(?:itude)?|lon(?:gitude)?)\s*[:=]\s*-?\d+(?:\.\d+)?/gi,
        match => `${match.split(/\s*[:=]\s*/)[0]}=[redacted]`)
      .replace(/[A-Za-z]:\\(?:[^\s<>:"|?*]+\\)*[^\s<>:"|?*]*/g, '[path redacted]')
      .replace(/(^|[\s("'=])\/(?:Users|home|etc|var|opt|private|tmp|run)\/[^\s"'<>]*/g,
        (_, prefix) => `${prefix}[path redacted]`).replace(/\s+/g, ' ').trim().slice(0, maximum) || null;
  }
  function integer(value, fallback = null) {
    return Number.isSafeInteger(value) && value >= 0 ? value : fallback;
  }
  function scalars(value = {}) {
    const result = {};
    for (const [key, item] of Object.entries(value).slice(0, 40)) {
      if (!/^[a-z][a-zA-Z0-9_]{0,47}$/.test(key) ||
          /(serial|token|secret|password|location|latitude|longitude|hostname|address|endpoint|path|file|url|ip)$/i.test(key)) continue;
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
  function classify(status = {}) {
    if (status.sdrplayStartup?.state === 'failed') return {code: 'SDR_OPEN_FAILED', component: 'receiver', stage: 'open', message: status.sdrplayStartup.message};
    if (['failed', 'partial', 'indeterminate'].includes(status.receiverSynchronization?.applicationState))
      return {code: 'RECEIVER_SYNC_FAILED', component: 'receiver', stage: 'sync', message: status.receiverSynchronization.message || 'Receiver synchronization failed'};
    if (status.restart?.state === 'failed') return {code: 'RESTART_FAILED', component: 'lifecycle', stage: 'restart', message: status.restart.message};
    if (status.processor?.state === 'error') return {code: 'PROCESSOR_RUNTIME_FAILED', component: 'processor', stage: 'runtime', message: status.processor.error};
    if (status.acceleration?.state === 'error' || status.clutterAcceleration?.state === 'error')
      return {code: 'GPU_BACKEND_FAILED', component: 'processor', stage: 'acceleration', message: status.acceleration?.reason || status.clutterAcceleration?.reason};
    if (status.errors?.length) return {code: 'PROCESSOR_RUNTIME_FAILED', component: 'processor', stage: 'runtime', message: status.errors.join(' ')};
    return null;
  }
  function buildReport({incident, status = {}, config = {}, comment = '', now = Date.now()}) {
    const browser = typeof navigator === 'undefined' ? {} : navigator;
    const type = receiverType(status, config);
    const processor = status.processor || {};
    const frequency = config?.capture?.fc;
    const sampleRate = config?.capture?.fs;
    const channels = config?.capture?.device?.channel_count;
    const message = clean(incident.message || 'User requested diagnostic report', 1024);
    return {schema: 1, consent: true, incident: {
      code: incident.code, component: incident.component, stage: incident.stage,
      severity: incident.severity || 'error', message,
      firstSeenAt: new Date(incident.firstSeenAt || now).toISOString(),
      lastSeenAt: new Date(now).toISOString(), occurrences: integer(incident.occurrences, 1),
      recovered: incident.recovered === true
    }, build: {version: clean(status.build?.version || '0.1.29', 64),
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
    container.querySelector('.report-context-action')?.remove(); const action = button('Send report'); action.classList.add('report-context-action'); action.onclick = () => open(context()); container.append(action); return action;
  }
  return {ENDPOINT, STORAGE_KEY, clean, scalars, classify, buildReport, submit, pending, addSettingsEntry, addContextAction, open};
});
