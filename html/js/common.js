function renderAppHeader(active) {
  const target = document.querySelector('.app-header');
  if (!target) return;
  target.innerHTML = `<a class="brand" href="/"><img class="brand-logo" src="/favicon/favicon-128x128.png" alt="30hours"><span>VectorWarp</span></a><nav class="app-nav" aria-label="Primary navigation"><a href="/" class="${active === 'radar' ? 'active' : ''}">Live radar</a><a href="/controller" class="${active === 'controller' ? 'active' : ''}">Displays</a><a href="/display/configuration" class="${active === 'settings' ? 'active' : ''}">Settings</a></nav><div class="service-statuses" aria-label="Service status"><div class="live-status" id="live-status"><span class="live-dot"></span>RADAR CONNECTING</div><div class="live-status" id="adsb-status" hidden><span class="live-dot"></span>ADS-B CONNECTING</div></div><div class="processor-alert" id="processor-alert" role="alert" hidden></div>`;
  refreshLiveStatus();
  if (window.blah2LiveStatusTimer)
    window.clearInterval(window.blah2LiveStatusTimer);
  window.blah2LiveStatusTimer = window.setInterval(refreshLiveStatus, 2000);
  routeApiLinks();
  addRecordingControl();
  addFullscreenControl();
}

function routeApiLinks() {
  document.querySelectorAll('a[href^="/api/"], a[data-api-path]').forEach(link => {
    const path = link.dataset.apiPath || new URL(link.getAttribute('href'), pageOrigin()).pathname;
    link.dataset.apiPath = path;
    link.href = liveApiUrl(path);
    link.target = '_blank';
    link.rel = 'noopener';
  });
}

let recordingState = {recording: false, requested: false, acknowledged: false, startedAt: null, available: true};
let recordingRequestPending = false;

function recordingDuration(milliseconds) {
  const seconds = Math.max(0, Math.floor(milliseconds / 1000));
  const hours = Math.floor(seconds / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  const remainder = seconds % 60;
  return [hours, minutes, remainder]
    .map(value => String(value).padStart(2, '0')).join(':');
}

function renderRecordingState() {
  const control = document.getElementById('recording-control');
  if (!control) return;
  const status = control.querySelector('.recording-status');
  const copy = control.querySelector('.recording-copy');
  const button = control.querySelector('.recording-toggle');
  control.classList.toggle('recording', recordingState.recording);
  control.classList.toggle('unavailable', !recordingState.available);
  button.disabled = recordingRequestPending || !recordingState.available;
  if (!recordingState.available) {
    status.innerHTML = '<span class="recording-dot"></span>Recording unavailable';
    copy.textContent = recordingState.reason || 'The recording control cannot reach the VectorWarp API.';
    button.textContent = 'Unavailable';
    return;
  }
  if (recordingState.recording) {
    const elapsed = recordingState.startedAt ?
      recordingDuration(Date.now() - recordingState.startedAt) : 'Recording';
    status.innerHTML = `<span class="recording-dot"></span><strong>REC</strong><time>${elapsed}</time>`;
    copy.textContent = recordingState.file ? `Writing ${recordingState.file}` : 'Recording acknowledged by the processor.';
    button.textContent = 'Stop recording';
  } else if (recordingState.requested) {
    status.innerHTML = '<span class="recording-dot"></span>Starting recording';
    copy.textContent = recordingState.reason || 'Waiting for processor acknowledgement.';
    button.textContent = 'Cancel recording';
  } else {
    status.innerHTML = '<span class="recording-dot"></span>Not recording';
    copy.textContent = 'Press Space once to record raw IQ. Press again to stop.';
    button.textContent = 'Start recording';
  }
}

async function readRecordingState() {
  try {
    let response = await fetchStatusResource(liveApiUrl('/capture/status'),
      {cache: 'no-store'});
    if (response.ok) {
      const state = await response.json();
      recordingState = {recording: state.recording === true, requested: state.requested === true,
        acknowledged: state.acknowledged === true,
        startedAt: Number.isFinite(state.startedAt) ? state.startedAt : null,
        available: state.supported !== false && state.available !== false, reason: state.recordingError || state.reason,
        file: state.recordingFile || ''};
    } else {
      response = await fetchStatusResource(liveApiUrl('/capture'), {cache: 'no-store'});
      if (!response.ok) throw new Error('Recording status unavailable');
      const active = (await response.text()).trim() === 'true';
      recordingState = {recording: active, requested: active, acknowledged: false,
        startedAt: active ? recordingState.startedAt : null, available: true};
    }
  } catch (_) {
    recordingState.available = false;
  }
  renderRecordingState();
}

async function toggleRecording() {
  if (recordingRequestPending || !recordingState.available) return;
  recordingRequestPending = true;
  renderRecordingState();
  try {
    const response = await fetchStatusResource(liveApiUrl('/capture/toggle'),
      {cache: 'no-store'});
    if (!response.ok) throw new Error('Recording control unavailable');
    let state = null;
    try { state = await response.json(); } catch (_) { /* Older API. */ }
    if (typeof state?.requested === 'boolean') {
      recordingState = {recording: state.recording === true, requested: state.requested,
        acknowledged: state.acknowledged === true,
        startedAt: state.recording && Number.isFinite(state.startedAt) ? state.startedAt : null,
        available: true};
    } else {
      await readRecordingState();
    }
  } catch (_) {
    recordingState.available = false;
  } finally {
    recordingRequestPending = false;
    renderRecordingState();
  }
}

function addRecordingControl() {
  const visual = document.getElementById('data');
  if (!visual || document.getElementById('recording-control')) return;
  const panel = visual.closest('.panel') || visual.parentElement;
  const control = document.createElement('div');
  control.id = 'recording-control';
  control.className = 'recording-control';
  control.setAttribute('role', 'status');
  control.setAttribute('aria-live', 'polite');
  control.innerHTML = '<div class="recording-status"><span class="recording-dot"></span>Checking recording…</div><div class="recording-copy">Press Space once to start. Press again to stop.</div><button class="recording-toggle" type="button">Start recording</button>';
  control.querySelector('.recording-toggle')
    .addEventListener('click', toggleRecording);
  panel.classList.add('recording-panel');
  panel.appendChild(control);
  if (!window.blah2RecordingKeyboardInstalled) {
    window.blah2RecordingKeyboardInstalled = true;
    document.addEventListener('keydown', event => {
      const tag = event.target?.tagName?.toLowerCase();
      if (event.code !== 'Space' || event.repeat || event.altKey ||
          event.ctrlKey || event.metaKey || event.shiftKey ||
          ['input', 'textarea', 'select', 'button'].includes(tag) ||
          event.target?.isContentEditable) return;
      event.preventDefault();
      toggleRecording();
    });
  }
  readRecordingState();
  if (window.blah2RecordingRenderTimer)
    window.clearInterval(window.blah2RecordingRenderTimer);
  window.blah2RecordingRenderTimer = window.setInterval(
    renderRecordingState, 1000);
  if (window.blah2RecordingSyncTimer)
    window.clearInterval(window.blah2RecordingSyncTimer);
  window.blah2RecordingSyncTimer = window.setInterval(
    readRecordingState, 4000);
}

function addFullscreenControl() {
  const visual = document.getElementById('data');
  if (!visual || document.querySelector('.fullscreen-button')) return;
  const panel = visual.closest('.panel') || visual.parentElement;
  const button = document.createElement('button');
  button.className = 'fullscreen-button';
  button.type = 'button';
  button.textContent = 'Fullscreen';
  button.addEventListener('click', async () => {
    try {
      if (document.fullscreenElement) await document.exitFullscreen();
      else await panel.requestFullscreen();
    } catch (_) { /* Browser policy or unsupported fullscreen. */ }
  });
  panel.classList.add('fullscreen-target');
  panel.classList.add('has-fullscreen');
  panel.appendChild(button);
  let resizePending = false;
  const resizePlot = () => {
    if (resizePending) return;
    resizePending = true;
    window.requestAnimationFrame(() => {
      resizePending = false;
      if (!window.Plotly || !visual._fullLayout) return;
      const bounds = visual.getBoundingClientRect();
      if (bounds.width < 1 || bounds.height < 1) return;
      try {
        window.Plotly.relayout(visual, {
          width: Math.floor(bounds.width),
          height: Math.floor(bounds.height),
          autosize: true
        });
      } catch (_) { /* Plot may still be initializing. */ }
    });
  };
  if (window.ResizeObserver) {
    const observer = new window.ResizeObserver(resizePlot);
    observer.observe(panel);
    observer.observe(visual);
  }
  window.addEventListener('resize', resizePlot);
  document.addEventListener('fullscreenchange', () => {
    button.textContent = document.fullscreenElement ? 'Exit fullscreen' : 'Fullscreen';
    resizePlot();
    window.setTimeout(resizePlot, 120);
    window.setTimeout(resizePlot, 360);
  });
}

function configuredApiPort() {
  try {
    const requested = new URLSearchParams(window.location.search).get('apiPort');
    if (/^\d{1,5}$/.test(requested || '') &&
        Number(requested) >= 1 && Number(requested) <= 65535) {
      window.localStorage.setItem('blah2-api-port', requested);
      return requested;
    }
    if (/^\d{1,5}$/.test(window.BLAH2_API_PORT || '')) return String(window.BLAH2_API_PORT);
    const saved = window.localStorage.getItem('blah2-api-port');
    if (/^\d{1,5}$/.test(saved || '') &&
        Number(saved) >= 1 && Number(saved) <= 65535) return saved;
  } catch (_) { /* Storage can be disabled by browser policy. */ }
  return '3000';
}

function rememberApiPort(config) {
  const port = Number(config?.network?.ports?.api);
  if (!Number.isInteger(port) || port < 1 || port > 65535) return;
  try { window.localStorage.setItem('blah2-api-port', String(port)); }
  catch (_) { /* Storage can be disabled by browser policy. */ }
  apiEndpoint = null;
}

let apiEndpoint = null;
let apiDiscovery = null;

function liveApiUrl(path) {
  return apiEndpoint ? `${apiEndpoint}${path}` : path;
}

function pageOrigin() {
  return window.location.origin || `${window.location.protocol}//${window.location.hostname}${window.location.port ? `:${window.location.port}` : ''}`;
}

async function readResponse(url, options = {}, timeoutMs = 1800) {
  const controller = new AbortController();
  let timeout;
  try {
    return await Promise.race([
      (async () => {
        const response = await fetch(url, {...options, signal: controller.signal});
        const body = await response.text();
        return {ok: response.ok, status: response.status, headers: response.headers,
          text: async () => body, json: async () => JSON.parse(body)};
      })(),
      new Promise((_, reject) => {
        timeout = window.setTimeout(() => {
          controller.abort();
          reject(new Error('Request timed out'));
        }, timeoutMs);
      })
    ]);
  } finally {
    window.clearTimeout(timeout);
  }
}

async function discoverApiEndpoint() {
  if (apiEndpoint !== null) return apiEndpoint;
  if (!apiDiscovery) apiDiscovery = (async () => {
    const origin = pageOrigin();
    const direct = new URL(origin);
    direct.port = configuredApiPort();
    const requested = new URLSearchParams(window.location.search).get('apiPort');
    const explicitPort = /^\d+$/.test(requested || '') && Number(requested) > 0 && Number(requested) <= 65535;
    const candidates = window.location.protocol === 'https:' ? [origin] :
      explicitPort ? [direct.origin, origin] : [origin, direct.origin];
    for (const base of new Set(candidates)) {
      try {
        const response = await readResponse(`${base}/api/system/status`, {cache: 'no-store'});
        if (!response.ok) continue;
        const status = await response.json();
        if (typeof status.serverId !== 'string' || typeof status.radar !== 'string') continue;
        apiEndpoint = base === origin ? '' : base;
        if (typeof document !== 'undefined' && document.querySelectorAll) routeApiLinks();
        return apiEndpoint;
      } catch (_) { /* A static web server need not serve the API itself. */ }
    }
    throw new Error('VectorWarp API unavailable. Check its address or reverse-proxy routes.');
  })().finally(() => { apiDiscovery = null; });
  return apiDiscovery;
}

async function fetchStatusResource(url, options = {}, timeoutMs = 1800) {
  const parsed = new URL(url, pageOrigin());
  const internal = /^\/(api|stash|capture)(\/|$)/.test(parsed.pathname) &&
    parsed.hostname === new URL(pageOrigin()).hostname;
  if (!internal) return readResponse(url, options, timeoutMs);
  await discoverApiEndpoint();
  try {
    // URLs created before discovery/restart must also follow the selected API.
    return await readResponse(liveApiUrl(parsed.pathname + parsed.search), options, timeoutMs);
  } catch (error) {
    apiEndpoint = null; // Rediscover on the next request; never replay a write/toggle.
    throw error;
  }
}

async function refreshLiveStatus() {
  const radarLabel = document.getElementById('live-status');
  const adsbLabel = document.getElementById('adsb-status');
  const processorAlert = document.getElementById('processor-alert');
  if (!radarLabel || window.blah2LiveStatusPending) return;
  window.blah2LiveStatusPending = true;
  try {
    const response = await fetchStatusResource(liveApiUrl('/api/system/status'), {cache: 'no-store'});
    if (!response.ok) throw new Error('Radar status unavailable');
    const status = await response.json();
    const processor = status.processorFresh ? status.processor : null;
    const replay = processor?.input === 'replay' ? processor : null;
    const restarting = ['scheduled', 'running'].includes(status.restart?.state);
    let label;
    if (processor?.state === 'error') {
      label = replay ? 'REPLAY ERROR' : 'INPUT ERROR';
    } else if (replay) {
      const progress = replay.totalSamples > 0 ? ` ${Math.min(100, Math.floor(100 * replay.positionSamples / replay.totalSamples))}%` : '';
      label = replay.state === 'error' ? 'REPLAY ERROR' : replay.state === 'complete' ? 'REPLAY COMPLETE' :
        replay.state === 'stopped' ? 'REPLAY STOPPED' : `REPLAY ${replay.state.toUpperCase()}${progress}`;
    } else label = restarting ? 'RESTARTING' : status.errors?.length ? 'ERROR' :
      status.radar === 'receiving' ? 'ONLINE' : status.radar === 'stale' ? 'STALE' : 'OFFLINE';
    radarLabel.innerHTML = `<span class="live-dot${label === 'ONLINE' ? '' : ' offline'}"></span>RADAR ${label}`;
    radarLabel.title = processor?.error || status.errors?.[0] || status.message || '';
    if (processorAlert) {
      processorAlert.textContent = processor?.error || '';
      processorAlert.hidden = !processor?.error;
    }
    window.blah2AdsbEnabled = status.adsbEnabled === true && !replay;
    if (adsbLabel) {
      adsbLabel.hidden = !window.blah2AdsbEnabled;
      if (window.blah2AdsbEnabled) {
        try {
          const adsbResponse = await fetchStatusResource(liveApiUrl('/api/adsb/status'), {cache: 'no-store'});
          if (!adsbResponse.ok) throw new Error('ADS-B status unavailable');
          const feeds = await adsbResponse.json();
          const label = feeds.online ? 'ONLINE' : feeds.aircraft?.available || feeds.delayDoppler?.available ? 'PARTIAL' : 'OFFLINE';
          adsbLabel.innerHTML = `<span class="live-dot${feeds.online ? '' : ' offline'}"></span>ADS-B ${label}`;
          adsbLabel.title = [feeds.aircraft?.message, feeds.delayDoppler?.message].filter(Boolean).join(' · ');
        } catch (_) {
          adsbLabel.innerHTML = '<span class="live-dot offline"></span>ADS-B OFFLINE';
        }
      }
    }
  } catch (_) {
    radarLabel.innerHTML = '<span class="live-dot offline"></span>RADAR OFFLINE';
    if (adsbLabel && window.blah2AdsbEnabled) {
      adsbLabel.hidden = false;
      adsbLabel.innerHTML = '<span class="live-dot offline"></span>ADS-B OFFLINE';
    }
    if (processorAlert) processorAlert.hidden = true;
  } finally {
    window.blah2LiveStatusPending = false;
  }
}

// Only the API's loaded configuration controls display cadence. An editor draft
// or a saved file awaiting restart must not change how old frames are displayed.
let radarRuntimeCache = null;
let radarRuntimeReadAt = -Infinity;
let radarRuntimePending = null;

async function fetchRadarJson(url) {
  const response = await fetchStatusResource(url, {cache: 'no-store'});
  if (!response.ok) throw new Error(`Data request failed (${response.status})`);
  const body = await response.text();
  return body.trim() ? JSON.parse(body) : null;
}

async function getRadarRuntimeConfig() {
  if (radarRuntimeCache && Date.now() - radarRuntimeReadAt < 2000) return radarRuntimeCache;
  if (!radarRuntimePending) {
    radarRuntimePending = fetchRadarJson(liveApiUrl('/api/runtime/config')).then(config => {
      if (!config || typeof config !== 'object') throw new Error('Running configuration unavailable');
      radarRuntimeCache = config;
      radarRuntimeReadAt = Date.now();
      return config;
    }).finally(() => { radarRuntimePending = null; });
  }
  return radarRuntimePending;
}

function radarFrameIntervalMs(config) {
  const seconds = config?.process?.data?.cpi;
  // Browsers cannot paint arbitrarily fast; do not flood the API for tiny CPIs.
  return typeof seconds === 'number' && Number.isFinite(seconds) && seconds > 0 ?
    Math.min(2147483647, Math.max(16, seconds * 1000)) : 1000;
}

function startRadarUpdates(update) {
  let stopped = false;
  let timer;
  let due = 0;
  let interval = null;
  let loadedConfig = null;
  async function tick() {
    if (stopped) return;
    try {
      try { loadedConfig = await getRadarRuntimeConfig(); } catch (_) { /* Retry after an API restart. */ }
      if (stopped) return;
      const nextInterval = radarFrameIntervalMs(loadedConfig);
      if (interval !== nextInterval) { interval = nextInterval; due = 0; }
      if (Date.now() >= due) {
        due = Date.now() + interval;
        await update(loadedConfig);
      }
    } catch (error) {
      window.blah2FrameError = error?.message || String(error);
    } finally {
      // Account for request/render time; never run overlapping updates or queue
      // missed frames. Check for restarted config even during very long CPIs.
      if (!stopped) timer = window.setTimeout(tick,
        Math.max(16, Math.min(2000, due - Date.now())));
    }
  }
  tick();
  return {stop() { stopped = true; window.clearTimeout(timer); }};
}

function startRadarPlot(url, render) {
  let renderedTimestamp;
  return startRadarUpdates(async () => {
    const data = await fetchRadarJson(url);
    if (!data || typeof data !== 'object' || Array.isArray(data)) return;
    const stamp = data.frameTimestamp ?? data.timestamp;
    if (stamp === undefined || stamp === null) return;
    const key = JSON.stringify(stamp);
    if (key === renderedTimestamp) return;
    const complete = await render(data);
    // Failed fetches/renders can be retried on the next tick.
    if (complete !== false) renderedTimestamp = key;
  });
}

async function renderRuntimeSummary() {
  const target = document.getElementById('runtime-summary');
  try {
    const config = await getRadarRuntimeConfig();
    const capture = config.capture || {};
    const device = capture.device || {};
    const process = config.process || {};
    const channels = device.channel_count || device.gain?.length ||
      device.surveillance_channels?.length ||
      (['RspDuo', 'Usrp', 'HackRF'].includes(device.type) ? 2 : '?');
    const cpi = process.data?.cpi;
    const ambiguity = process.ambiguity || {};
    const detection = process.detection || {};
    const tracker = process.tracker || {};
    const synthesis = process.reference_synthesis || {};
    const referenceInputs = synthesis.channels?.length || channels;
    const reference = synthesis.mode === 'array_eigenbeam' ?
      `Synthesized reference · ${referenceInputs} inputs` :
      `Reference channel ${device.reference_channel ?? 0}`;
    const frequency = Number.isFinite(Number(capture.fc)) ?
      `${(Number(capture.fc) / 1000000).toFixed(3)} MHz` : 'Frequency unavailable';
    const hardware = device.type || 'Receiver';
    const receiver = config.location?.rx?.name || 'Receiver site';
    const transmitter = config.location?.tx?.name || 'Illuminator site';
    const spectrum = process.spectrum?.enable === false ?
      'Disabled in settings' : `${reference} spectrum history`;
    const adsb = config.truth?.adsb || {};
    const values = {
      'runtime-summary': `${hardware} · ${frequency} · ${channels} channels`,
      'display-overview-meta': `${receiver} · ${frequency}`,
      'display-detections-meta': detection.enable === false ? 'Detector disabled' :
        `Detector enabled${Number.isFinite(Number(detection.pfa)) ? ` · PFA ${detection.pfa}` : ''}`,
      'display-tracks-meta': tracker.enable === true ?
        `Enabled · ${tracker.initiate?.M ?? '—'} of ${tracker.initiate?.N ?? '—'} frames to initiate` : 'Disabled in settings',
      'display-health-meta': cpi ? `${Number(cpi)} s per frame · ${channels} channels` : `${hardware} · ${channels} channels`,
      'display-site-meta': `${receiver} · ${transmitter}`,
      'display-locations-meta': `Live delay ellipses · ${receiver} and ${transmitter}`,
      'display-evaluation-meta': adsb.enabled === true ?
        `Enabled${Number.isFinite(Number(adsb.display_range_km)) ? ` · ${adsb.display_range_km} km radius` : ''} · evaluation only` : 'Disabled in settings',
      'display-live-meta': `${hardware} · ${frequency} · ${channels} channels`,
      'display-maxhold-meta': `${ambiguity.delayMin ?? '—'}–${ambiguity.delayMax ?? '—'} delay bins · ${ambiguity.dopplerMin ?? '—'}–${ambiguity.dopplerMax ?? '—'} Hz`,
      'display-spectrum-meta': spectrum,
      'display-range-meta': `${receiver} receiver · ${transmitter} illuminator`,
      'display-doppler-meta': `${ambiguity.dopplerMin ?? '—'}–${ambiguity.dopplerMax ?? '—'} Hz search window`,
      'display-scatter-meta': `${ambiguity.delayMin ?? '—'}–${ambiguity.delayMax ?? '—'} bins · ${ambiguity.dopplerMin ?? '—'}–${ambiguity.dopplerMax ?? '—'} Hz`,
      'display-timing-meta': cpi ? `${Number(cpi)} s per frame` : 'Timing from active configuration',
      'display-config-meta': `${hardware} · ${channels} channels · ${reference}`
    };
    Object.entries(values).forEach(([id, value]) => {
      const element = document.getElementById(id);
      if (element) element.textContent = value;
    });
  } catch (_) {
    if (target) target.textContent = 'Live receiver configuration unavailable';
  }
}

if (typeof module !== 'undefined')
  module.exports = {configuredApiPort, liveApiUrl,
    recordingDuration, rememberApiPort, radarFrameIntervalMs, startRadarUpdates,
    startRadarPlot, getRadarRuntimeConfig, discoverApiEndpoint, fetchStatusResource, refreshLiveStatus};
