function ensureBrandFavicon() {
  if (!document.head || !document.createElement) return;
  const existing = document.querySelector('link[rel~="icon"]');
  if (existing) return;
  const favicon = document.createElement('link');
  favicon.rel = 'icon';
  favicon.href = '/favicon/vectorwarp-vw.svg';
  favicon.type = 'image/svg+xml';
  document.head.appendChild(favicon);
}

function renderAppHeader(active) {
  ensureBrandFavicon();
  const target = document.querySelector('.app-header');
  if (!target) return;
  const current = name => active === name ? ' aria-current="page"' : '';
  target.innerHTML = `<a class="brand" href="/"><img class="brand-logo" src="/favicon/vectorwarp-vw.svg" alt="VectorWarp"><span>VectorWarp</span></a><nav class="app-nav" aria-label="Primary navigation"><a href="/" class="${active === 'radar' ? 'active' : ''}"${current('radar')}>Live radar</a><a href="/display/locations" class="${active === 'map' ? 'active' : ''}"${current('map')}>Map</a><a href="/controller" class="${active === 'controller' ? 'active' : ''}"${current('controller')}>Displays</a><a href="/display/recordings" class="${active === 'recordings' ? 'active' : ''}"${current('recordings')}>Recordings</a><a href="/display/configuration" class="${active === 'settings' ? 'active' : ''}"${current('settings')}>Settings</a></nav><div class="service-statuses" aria-label="Service status"><div class="live-status" id="live-status" role="status" aria-live="polite"><span class="live-dot"></span>RADAR CONNECTING</div><div class="live-status" id="adsb-status" role="status" aria-live="polite" hidden><span class="live-dot"></span>ADS-B CONNECTING</div></div><div class="processor-alert" id="processor-alert" role="alert" hidden></div><a class="receiver-device-alert" id="receiver-device-alert" href="/display/configuration?setup=1" role="alert" hidden></a><div class="receiver-health-status" id="receiver-health-status" role="status" aria-live="polite" aria-atomic="true" hidden></div>`;
  refreshLiveStatus();
  if (window.blah2LiveStatusTimer)
    window.clearInterval(window.blah2LiveStatusTimer);
  const compactDevice = typeof window !== 'undefined' && window.innerWidth <= 1024;
  window.blah2LiveStatusTimer = window.setInterval(refreshLiveStatus, compactDevice ? 5000 : 2500);
  // Hardware/software discovery can launch several bounded host probes. Never
  // run it from the shared header: every radar/display tab used to repeat those
  // probes every five minutes and starve the real-time processor. Receiver
  // setup exposes an explicit "Check receiver software" action instead.
  if (window.blah2ReceiverDeviceTimer)
    window.clearInterval(window.blah2ReceiverDeviceTimer);
  window.blah2ReceiverDeviceTimer = null;
  routeApiLinks();
  addRecordingControl();
  addFullscreenControl();
}

// Discovery is read-only. A newly connected receiver is offered to the user,
// but the running receiver and saved configuration are never changed here.
async function refreshReceiverDevices() {
  const target = document.getElementById('receiver-device-alert');
  if (!target || document.hidden || window.blah2ReceiverDevicePending) return;
  window.blah2ReceiverDevicePending = true;
  try {
    const response = await fetchStatusResource(liveApiUrl('/api/receivers'), {cache: 'no-store'});
    if (!response.ok) throw new Error('Receiver discovery unavailable');
    const result = await response.json();
    const receivers = Array.isArray(result.receivers) ? result.receivers : [];
    const configured = receivers.find(receiver => receiver.capabilities?.configured)?.type;
    const alternatives = receivers.filter(receiver => receiver.capabilities?.detected === true &&
      (result.setupRequired || receiver.type !== configured));
    if (!alternatives.length) {
      target.hidden = true;
      target.textContent = '';
      return;
    }
    const labels = alternatives.map(receiver => receiver.label || receiver.type).join(', ');
    target.textContent = result.setupRequired ?
      `${labels} detected. Open Settings to finish receiver setup.` :
      `${labels} detected. Open Settings if you want to switch receivers; the active receiver was not changed.`;
    target.hidden = false;
  } catch (_) {
    target.hidden = true;
  } finally {
    window.blah2ReceiverDevicePending = false;
  }
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

let recordingState = {recording: false, requested: false, paused: false,
  acknowledged: false, startedAt: null, available: true};
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
  const pause = control.querySelector('.recording-pause');
  control.classList.toggle('recording', recordingState.recording);
  control.classList.toggle('unavailable', !recordingState.available);
  button.disabled = recordingRequestPending || !recordingState.available;
  if (pause) pause.disabled = recordingRequestPending ||
    (!recordingState.recording && !recordingState.paused);
  if (!recordingState.available) {
    status.innerHTML = '<span class="recording-dot"></span>Recording unavailable';
    copy.textContent = recordingState.reason || 'The recording control cannot reach the VectorWarp API.';
    button.textContent = 'Unavailable';
    if (pause) pause.hidden = true;
    return;
  }
  if (pause) pause.hidden = false;
  if (recordingState.recording) {
    const elapsed = recordingState.startedAt ?
      recordingDuration(Date.now() - recordingState.startedAt) : 'Recording';
    status.innerHTML = `<span class="recording-dot"></span><strong>REC</strong><time>${elapsed}</time>`;
    copy.textContent = recordingState.file ? `Writing ${recordingState.file}` : 'Recording confirmed.';
    button.textContent = 'Stop recording';
    if (pause) pause.textContent = 'Pause';
  } else if (recordingState.paused) {
    status.innerHTML = '<span class="recording-dot"></span><strong>PAUSED</strong>';
    copy.textContent = 'The previous file is safely finalized. Resume starts a new linked segment.';
    button.textContent = 'End session';
    if (pause) pause.textContent = 'Resume';
  } else if (recordingState.requested) {
    status.innerHTML = '<span class="recording-dot"></span>Starting recording';
    copy.textContent = recordingState.reason || 'Waiting for recording to start.';
    button.textContent = 'Cancel recording';
  } else {
    status.innerHTML = '<span class="recording-dot"></span>Not recording';
    copy.textContent = 'Press Space to record raw IQ. Press again to stop.';
    button.textContent = 'Start recording';
  }
}

async function readRecordingState() {
  try {
    let response = await fetchStatusResource(liveApiUrl('/capture/status?summary=1'),
      {cache: 'no-store'});
    if (response.ok) {
      const state = await response.json();
      recordingState = {recording: state.recording === true, requested: state.requested === true,
        paused: state.paused === true,
        acknowledged: state.acknowledged === true,
        startedAt: Number.isFinite(state.startedAt) ? state.startedAt : null,
        available: state.supported !== false && state.available !== false, reason: state.recordingError || state.reason,
        file: state.recordingFile || ''};
    } else {
      response = await fetchStatusResource(liveApiUrl('/capture'), {cache: 'no-store'});
      if (!response.ok) throw new Error('Recording status unavailable');
      const active = (await response.text()).trim() === 'true';
      recordingState = {recording: active, requested: active, acknowledged: false,
        paused: false, startedAt: active ? recordingState.startedAt : null, available: true};
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
      {method: 'POST', headers: {'X-VectorWarp-Intent': 'recording-toggle-v1'}, cache: 'no-store'});
    if (!response.ok) throw new Error('Recording control unavailable');
    let state = null;
    try { state = await response.json(); } catch (_) { /* Older API. */ }
    if (typeof state?.requested === 'boolean') {
      recordingState = {recording: state.recording === true, requested: state.requested,
        paused: state.paused === true,
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

async function pauseRecording() {
  if (recordingRequestPending || (!recordingState.recording && !recordingState.paused)) return;
  recordingRequestPending = true; renderRecordingState();
  try {
    const response = await fetchStatusResource(liveApiUrl('/capture/pause'), {method: 'POST',
      headers: {'Content-Type': 'application/json', 'X-VectorWarp-Intent': 'recording-pause-v1'},
      body: JSON.stringify({paused: !recordingState.paused}), cache: 'no-store'});
    if (!response.ok) throw new Error('Recording pause unavailable');
    const state = await response.json();
    recordingState = {...recordingState, requested: state.requested === true,
      paused: state.paused === true, recording: false,
      startedAt: Number.isFinite(state.startedAt) ? state.startedAt : null};
  } catch (_) { await readRecordingState(); }
  finally { recordingRequestPending = false; renderRecordingState(); }
}

function addRecordingControl() {
  // Starting/stopping raw-IQ capture is an operational action, not a chart
  // feature. Only explicitly opted-in live radar pages may expose it.
  if (document.body?.dataset.captureControls !== 'true') return;
  const visual = document.getElementById('data');
  if (!visual || document.getElementById('recording-control')) return;
  const panel = visual.closest('.panel') || visual.parentElement;
  const control = document.createElement('div');
  control.id = 'recording-control';
  control.className = 'recording-control';
  control.setAttribute('role', 'status');
  control.setAttribute('aria-live', 'polite');
  control.innerHTML = '<div class="recording-status"><span class="recording-dot"></span>Checking recording…</div><div class="recording-copy">Press Space to start. Press again to stop. <a href="/display/recordings">Browse recordings</a></div><button class="recording-pause button-secondary" type="button" hidden>Pause</button><button class="recording-toggle" type="button">Start recording</button>';
  control.querySelector('.recording-pause').addEventListener('click', pauseRecording);
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
  const labelFullscreen = active => {
    const label = active ? 'Exit fullscreen' : 'Fullscreen';
    button.textContent = active ? '×' : '⛶';
    button.setAttribute('aria-label', label);
    button.setAttribute('title', label);
  };
  labelFullscreen(false);
  const fullscreenElement = () => document.fullscreenElement || document.webkitFullscreenElement;
  const fallbackActive = () => panel.classList.contains('fullscreen-fallback');
  const setFallback = active => {
    panel.classList.toggle('fullscreen-fallback', active);
    document.documentElement.classList.toggle('fullscreen-page', active);
    document.body.classList.toggle('fullscreen-page', active);
    labelFullscreen(active);
  };
  button.addEventListener('click', async () => {
    if (fallbackActive()) {
      setFallback(false);
      return;
    }
    const exitFullscreen = document.exitFullscreen || document.webkitExitFullscreen;
    const requestFullscreen = panel.requestFullscreen || panel.webkitRequestFullscreen;
    try {
      if (fullscreenElement() && exitFullscreen) await exitFullscreen.call(document);
      else if (requestFullscreen) await requestFullscreen.call(panel);
      else setFallback(true);
    } catch (_) {
      // iPhone Safari does not expose element fullscreen. Keep the radar usable
      // with an in-page fullscreen mode instead of leaving a dead control.
      setFallback(true);
    }
  });
  panel.classList.add('fullscreen-target');
  panel.classList.add('has-fullscreen');
  panel.appendChild(button);
  let resizePending = false;
  let resizeInFlight = false;
  let resizeRequested = false;
  let waitingForMap = false;
  const resizePlot = () => {
    if (resizePending) return;
    if (resizeInFlight) { resizeRequested = true; return; }
    resizePending = true;
    window.requestAnimationFrame(() => {
      resizePending = false;
      if (!window.Plotly || !visual._fullLayout) return;
      // ResizeObserver can fire while Plotly is still creating its Mapbox
      // style. A concurrent relayout then tries to add sources before load.
      const mapbox = visual._fullLayout.mapbox;
      const map = mapbox?._subplot?.map;
      if (mapbox && (!map || !map.isStyleLoaded())) {
        if (!waitingForMap && visual.once) {
          waitingForMap = true;
          visual.once('plotly_afterplot', () => {
            waitingForMap = false;
            resizePlot();
          });
        }
        return;
      }
      const bounds = visual.getBoundingClientRect();
      if (bounds.width < 1 || bounds.height < 1) return;
      const width = Math.floor(bounds.width), height = Math.floor(bounds.height);
      if (visual.layout?.width === width && visual.layout?.height === height) return;
      try {
        resizeInFlight = true;
        Promise.resolve(window.Plotly.relayout(visual, {
          width,
          height,
          autosize: true
        })).catch(error => {
          window.blah2PlotResizeError = error?.message || String(error);
        }).finally(() => {
          resizeInFlight = false;
          if (resizeRequested) { resizeRequested = false; resizePlot(); }
        });
      } catch (error) {
        resizeInFlight = false;
        window.blah2PlotResizeError = error?.message || String(error);
      }
    });
  };
  if (window.ResizeObserver) {
    const observer = new window.ResizeObserver(resizePlot);
    observer.observe(panel);
    observer.observe(visual);
  }
  window.addEventListener('resize', resizePlot);
  const fullscreenChanged = () => {
    labelFullscreen(Boolean(fullscreenElement()));
    resizePlot();
    window.setTimeout(resizePlot, 120);
    window.setTimeout(resizePlot, 360);
  };
  document.addEventListener('fullscreenchange', fullscreenChanged);
  document.addEventListener('webkitfullscreenchange', fullscreenChanged);
  document.addEventListener('keydown', event => {
    if (event.key === 'Escape' && fallbackActive()) setFallback(false);
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

// Mobile browsers and remote LAN/VPN routes can briefly take longer than a
// local desktop request without the radar being unavailable.  Keep a bounded
// timeout, but do not turn normal network jitter into a page-wide failure.
async function readResponse(url, options = {}, timeoutMs = 5000) {
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

async function fetchStatusResource(url, options = {}, timeoutMs = 5000) {
  const parsed = new URL(url, pageOrigin());
  const internal = /^\/(api|stash|capture)(\/|$)/.test(parsed.pathname) &&
    parsed.hostname === new URL(pageOrigin()).hostname;
  if (!internal) return readResponse(url, options, timeoutMs);
  await discoverApiEndpoint();
  try {
    // URLs created before discovery/restart must also follow the selected API.
    return await readResponse(liveApiUrl(parsed.pathname + parsed.search), options, timeoutMs);
  } catch (error) {
    // A slow response does not invalidate a successfully discovered endpoint.
    // Clearing it here caused every timeout to start another discovery request,
    // amplifying a brief mobile-network delay into a refresh storm.  Real
    // transport failures still trigger discovery on the next request.  Writes
    // are never replayed in either case.
    if (error?.message !== 'Request timed out') apiEndpoint = null;
    throw error;
  }
}

// The SDK reports overload events, not measured clipping or overall RF quality.
// Counts keep events visible even if detection and recovery occur between polls.
function rspDuoOverloadStatus(status, expected = false, now = Date.now()) {
  const processor = status?.processor;
  const unavailable = message => ({visible: true, severity: 'unavailable', message, channels: []});
  if (processor?.receiver !== 'RspDuo') return expected ? unavailable(processor ?
    'RSPduo is not the running receiver. Overload status unavailable.' :
    'No current RSPduo status. Overload status unavailable.') : {visible: false};
  if (processor.input === 'replay')
    return unavailable('Replay is running. Live RSPduo overload status unavailable.');
  if (status.processorFresh !== true)
    return unavailable('RSPduo status is stale. Current overload state is unknown.');
  if (['scheduled', 'running'].includes(status.restart?.state))
    return unavailable('Receiver restarting. Current overload state is unknown.');
  if (processor.input !== 'live' || processor.state !== 'live')
    return unavailable(processor.state === 'stopped' ?
      'Receiver stopped. Current overload state is unknown.' :
      'Receiver is not capturing live input. Current overload state is unknown.');
  const health = processor.receiverHealth;
  const count = value => Number.isSafeInteger(value) && value >= 0;
  const validChannel = channel => channel && typeof channel.active === 'boolean' &&
    ['detectedCount', 'correctedCount', 'lastDetectedAt', 'lastCorrectedAt'].every(key => count(channel[key])) &&
    channel.correctedCount <= channel.detectedCount &&
    channel.detectedCount - channel.correctedCount === (channel.active ? 1 : 0) &&
    (channel.detectedCount === 0) === (channel.lastDetectedAt === 0) &&
    (channel.correctedCount === 0) === (channel.lastCorrectedAt === 0);
  if (health?.schema !== 1 || health.receiver !== 'RspDuo' ||
      !count(health.ackFailures) || !['A', 'B'].every(key => validChannel(health.overload?.[key])))
    return unavailable('RSPduo overload telemetry unavailable. An older installed receiver module may need a rebuild.');
  if (health.monitoring !== true)
    return unavailable('RSPduo overload monitoring is not active. Current overload state is unknown.');
  const channels = ['A', 'B'].map(key => {
    const channel = health.overload[key];
    const recovered = channel.correctedCount > 0;
    const recent = recovered && channel.lastCorrectedAt > 0 &&
      now >= channel.lastCorrectedAt && now - channel.lastCorrectedAt < 60000;
    return {...channel, label: `${key} (${key === 'A' ? 'reference' : 'surveillance'})`,
      state: channel.active ? 'Overload active' : recent ? 'Recently recovered' :
        recovered ? 'Recovered' : channel.detectedCount > 0 ? 'Overload reported earlier' : 'No overload reported'};
  });
  const severity = channels.some(channel => channel.active) ? 'bad' :
    health.ackFailures || channels.some(channel => channel.detectedCount || channel.correctedCount) ? 'warning' : 'neutral';
  const message = channels.map(channel => `${channel.label}: ${channel.state.toLowerCase()}` +
    (channel.detectedCount ? ` (${channel.detectedCount} detected)` : '')).join(' · ') +
    (health.ackFailures ? ` · SDK acknowledgement failures: ${health.ackFailures}; overload reports may be incomplete.` : '');
  return {visible: true, severity, message, channels, ackFailures: health.ackFailures};
}

function renderReceiverHealth(status, target = document.getElementById('receiver-health-status'), detailed = false) {
  if (!target) return;
  const expected = detailed || (!status?.processor && target.dataset.receiver === 'RspDuo');
  const view = rspDuoOverloadStatus(status, expected);
  target.dataset.receiver = view.visible ? 'RspDuo' : '';
  target.hidden = !view.visible;
  // Avoid re-announcing identical status every two seconds to screen readers.
  const signature = JSON.stringify(view);
  if (target.dataset.healthView === signature) return;
  target.dataset.healthView = signature;
  target.dataset.severity = view.severity || '';
  target.replaceChildren();
  if (!view.visible) return;
  const message = document.createElement(detailed ? 'p' : 'span');
  message.textContent = detailed ? view.message : `RSPduo overload · ${view.message}`;
  if (detailed) {
    const heading = document.createElement('h3');
    heading.textContent = 'RSPduo overload check';
    message.setAttribute('role', 'status');
    message.setAttribute('aria-live', 'polite');
    target.append(heading, message);
    const channels = document.createElement('div');
    channels.className = 'receiver-health-channels';
    const eventTime = timestamp => timestamp ? new Date(timestamp).toLocaleString() : 'None reported';
    for (const channel of view.channels) {
      const card = document.createElement('div');
      const title = document.createElement('strong');
      title.textContent = channel.label;
      const state = document.createElement('p');
      state.textContent = channel.state;
      const counts = document.createElement('p');
      counts.textContent = `Detected: ${channel.detectedCount} · Recovered: ${channel.correctedCount}`;
      const times = document.createElement('p');
      times.textContent = `Last detected: ${eventTime(channel.lastDetectedAt)}. Last recovered: ${eventTime(channel.lastCorrectedAt)}.`;
      card.append(title, state, counts, times); channels.appendChild(card);
    }
    target.appendChild(channels);
    const help = document.createElement('p');
    help.textContent = 'Counts reset when receiver capture starts. SDRplay overload events do not prove continuous ADC clipping. “No overload reported” does not guarantee RF quality. ' +
      'If overload repeats, increase the affected channel’s gain reduction with AGC off, or choose a higher LNA state to reduce RF gain. With AGC on, try a more negative AGC target. Use Save & Restart, then check both channels again.';
    target.appendChild(help);
  } else {
    target.appendChild(message);
    const link = document.createElement('a');
    link.href = '/display/configuration';
    link.textContent = 'Receiver gain settings';
    target.appendChild(link);
  }
}

async function refreshLiveStatus() {
  const radarLabel = document.getElementById('live-status');
  const adsbLabel = document.getElementById('adsb-status');
  const processorAlert = document.getElementById('processor-alert');
  if (!radarLabel || document.hidden || window.blah2LiveStatusPending) return;
  window.blah2LiveStatusPending = true;
  try {
    const response = await fetchStatusResource(liveApiUrl('/api/system/status?summary=1'), {cache: 'no-store'});
    if (!response.ok) throw new Error('Radar status unavailable');
    const status = await response.json();
    renderReceiverHealth(status);
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
    updateLiveStatusLabel(radarLabel, 'RADAR', label, label === 'ONLINE');
    radarLabel.title = processor?.error || status.errors?.[0] || status.message || '';
    if (processorAlert) {
      processorAlert.textContent = processor?.error || '';
      processorAlert.hidden = !processor?.error;
    }
    window.blah2AdsbEnabled = status.adsbEnabled === true && !replay;
    if (adsbLabel) {
      adsbLabel.hidden = !window.blah2AdsbEnabled;
      const adsbDue = !window.blah2AdsbStatusAt || Date.now() - window.blah2AdsbStatusAt >= 10000;
      if (window.blah2AdsbEnabled && adsbDue) {
        try {
          const adsbResponse = await fetchStatusResource(liveApiUrl('/api/adsb/status'), {cache: 'no-store'});
          if (!adsbResponse.ok) throw new Error('ADS-B status unavailable');
          const feeds = await adsbResponse.json();
          window.blah2AdsbStatusAt = Date.now();
          const label = feeds.online ? 'ONLINE' : feeds.aircraft?.available || feeds.delayDoppler?.available ? 'PARTIAL' : 'OFFLINE';
          updateLiveStatusLabel(adsbLabel, 'ADS-B', label, feeds.online);
          adsbLabel.title = [feeds.aircraft?.message, feeds.delayDoppler?.message].filter(Boolean).join(' · ');
        } catch (_) {
          updateLiveStatusLabel(adsbLabel, 'ADS-B', 'OFFLINE', false);
        }
      }
    }
  } catch (_) {
    renderReceiverHealth(null);
    updateLiveStatusLabel(radarLabel, 'RADAR', 'OFFLINE', false);
    if (adsbLabel && window.blah2AdsbEnabled) {
      adsbLabel.hidden = false;
      updateLiveStatusLabel(adsbLabel, 'ADS-B', 'OFFLINE', false);
    }
    if (processorAlert) processorAlert.hidden = true;
  } finally {
    window.blah2LiveStatusPending = false;
  }
}

// Do not rebuild status DOM on every poll. Keeping the same nodes avoids
// unnecessary layout/paint work in the shared header and prevents a status
// tick from disturbing an open mobile menu or the browser's scroll anchor.
function updateLiveStatusLabel(target, prefix, label, healthy) {
  if (!target) return;
  const signature = `${prefix}:${label}:${healthy ? 'healthy' : 'degraded'}`;
  if (target._vectorwarpStatusSignature === signature) return;
  target._vectorwarpStatusSignature = signature;
  target.innerHTML = `<span class="live-dot${healthy ? '' : ' offline'}"></span>${prefix} ${label}`;
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

function radarPresentationUrl(path) {
  const url = new URL(path, typeof window === 'undefined' ?
    'http://vectorwarp.invalid' : window.location.href);
  url.searchParams.set('presentation', 'aligned');
  return path.startsWith('http://') || path.startsWith('https://') ? url.href :
    `${url.pathname}${url.search}`;
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

function startRadarUpdates(update, minimumIntervalMs = 16) {
  let stopped = false;
  let running = false;
  let timer;
  let due = 0;
  let interval = null;
  let loadedConfig = null;
  async function tick() {
    if (stopped || running) return;
    // Background tabs cannot paint a useful radar frame.  Pausing here avoids
    // redundant heatmap compression, transfer and Plotly work while another
    // visible tab continues normally. Resume immediately when shown again.
    if (typeof document !== 'undefined' && document.hidden) {
      timer = window.setTimeout(tick, 1000);
      return;
    }
    running = true;
    try {
      try { loadedConfig = await getRadarRuntimeConfig(); } catch (_) { /* Retry after an API restart. */ }
      if (stopped) return;
      const nextInterval = Math.max(minimumIntervalMs, radarFrameIntervalMs(loadedConfig));
      if (interval !== nextInterval) { interval = nextInterval; due = 0; }
      if (Date.now() >= due) {
        due = Date.now() + interval;
        await update(loadedConfig);
        window.blah2FrameError = null;
      }
    } catch (error) {
      window.blah2FrameError = error?.message || String(error);
    } finally {
      running = false;
      // Account for request/render time; never run overlapping updates or queue
      // missed frames. If a slow network/render ran past the deadline, start a
      // fresh interval now instead of issuing catch-up requests back-to-back.
      // Check for restarted config even during very long CPIs.
      if (interval !== null && due <= Date.now()) due = Date.now() + interval;
      if (!stopped) timer = window.setTimeout(tick,
        Math.max(16, Math.min(2000, due - Date.now())));
    }
  }
  const visibilityChanged = () => {
    if (stopped || document.hidden) return;
    due = 0;
    window.clearTimeout(timer);
    void tick();
  };
  if (typeof document !== 'undefined')
    document.addEventListener?.('visibilitychange', visibilityChanged);
  tick();
  return {stop() {
    stopped = true;
    window.clearTimeout(timer);
    if (typeof document !== 'undefined')
      document.removeEventListener?.('visibilitychange', visibilityChanged);
  }};
}

function startRadarPlot(url, render) {
  let renderedTimestamp;
  let consecutiveFailures = 0;
  const target = typeof document === 'undefined' ? null : document.getElementById('data');
  const panel = target?.closest?.('.panel') || target?.parentElement;
  let status = panel?.querySelector?.('.plot-state');
  if (panel && !status) {
    status = document.createElement('div');
    status.className = 'plot-state loading';
    status.setAttribute('role', 'status');
    status.setAttribute('aria-live', 'polite');
    status.textContent = 'Loading live data…';
    panel.appendChild(status);
  }
  target?.setAttribute?.('aria-busy', 'true');
  // A full heatmap is materially heavier than a status card.  Tablets and
  // phones should display fresh radar data without continuously parsing and
  // repainting faster than their browser can keep up.
  // The compact map keeps mobile rendering bounded; cadence should still
  // follow the running CPI instead of imposing a separate one-second clock.
  const minimumIntervalMs = 100;
  return startRadarUpdates(async () => {
    try {
      const data = await fetchRadarJson(url);
      if (!data || typeof data !== 'object' || Array.isArray(data)) {
        if (status && renderedTimestamp === undefined) {
          status.className = 'plot-state waiting';
          status.textContent = 'Waiting for the first radar frame…';
        }
        target?.setAttribute?.('aria-busy', 'false');
        return;
      }
      const stamp = data.frameTimestamp ?? data.timestamp;
      if (stamp === undefined || stamp === null) return;
      const key = JSON.stringify(stamp);
      if (key === renderedTimestamp) return;
      const complete = await render(data);
      // Failed fetches/renders can be retried on the next tick.
      if (complete !== false) {
        renderedTimestamp = key;
        consecutiveFailures = 0;
        target?.setAttribute?.('aria-busy', 'false');
        if (status) status.hidden = true;
      }
    } catch (error) {
      consecutiveFailures++;
      if (status && consecutiveFailures >= 3) {
        status.hidden = false;
        status.className = 'plot-state error';
        status.textContent = renderedTimestamp === undefined ?
          'Live radar data is unavailable. Retrying automatically…' :
          'Refresh delayed. Showing the last valid radar frame while retrying…';
      }
      throw error;
    }
  }, minimumIntervalMs);
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
        `Enabled${Number.isFinite(Number(adsb.display_range_km)) ? ` · ${adsb.display_range_km} km range` : ''} · comparison only` : 'Disabled in settings',
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

// Original, license-neutral aircraft classification shared by every ADS-B view.
// ICAO emitter categories are preferred; type-code/description fallbacks keep
// mixed tar1090 and public-exchange feeds useful without a separate database.
function aircraftKind(aircraft = {}) {
  const category = String(aircraft.category || '').toUpperCase();
  const type = String(aircraft.t || aircraft.aircraft_type || '').toUpperCase();
  const description = String(aircraft.desc || '').toLowerCase();
  if (category === 'A7' || /^H\d/.test(type) || /helicopter|rotorcraft/.test(description)) return 'rotorcraft';
  if (category === 'B6' || /(?:^|\W)(?:uav|drone)(?:\W|$)/.test(description)) return 'drone';
  if (/^(?:GLID|GLIM)$/.test(type) || /glider|sailplane/.test(description)) return 'glider';
  if (/^C[12]$/.test(category) || /ground|surface|vehicle/.test(description)) return 'ground';
  if (category === 'A5' || /heavy/.test(description)) return 'heavy';
  if (/^A[34]$/.test(category) || /large|wide.?body/.test(description)) return 'airliner';
  if (/^(?:A[12]|B[134])$/.test(category) || /light|small|ultralight/.test(description)) return 'light';
  return 'aircraft';
}

function aircraftPlotSymbol(aircraft) {
  return ({rotorcraft: 'star', drone: 'diamond', glider: 'triangle-up-open', ground: 'square',
    heavy: 'triangle-up', airliner: 'triangle-up', light: 'triangle-up-open', aircraft: 'circle-open'})[aircraftKind(aircraft)];
}

function aircraftLabel(aircraft = {}) {
  return String(aircraft.flight || aircraft.callsign || aircraft.r || aircraft.hex || 'Aircraft').trim();
}

function aircraftInfo(aircraft = {}) {
  const finiteValue = value => value === null || value === undefined || value === '' ? null :
    Number.isFinite(Number(value)) ? Number(value) : null;
  const position = aircraft.position && typeof aircraft.position === 'object' ? aircraft.position : {};
  const altitude = finiteValue(aircraft.alt_geom) ?? finiteValue(aircraft.alt_baro) ??
    finiteValue(position.altitude_geom_ft) ?? finiteValue(position.altitude_baro_ft);
  const verticalRate = finiteValue(aircraft.geom_rate) ?? finiteValue(aircraft.baro_rate) ??
    finiteValue(position.geom_rate_fpm) ?? finiteValue(position.baro_rate_fpm);
  const rawPositionSource = String(aircraft.position_source ||
    aircraft.vectorwarp?.position_source || aircraft.provenance?.position_source || '').toUpperCase();
  const positionSource = rawPositionSource === 'MLAT' || String(aircraft.type || '').toLowerCase() === 'mlat' ?
    'MLAT' : rawPositionSource === 'ADSB_DIRECT' ? 'ADS-B' : rawPositionSource || 'ADS-B';
  const provider = String(aircraft.vectorwarp?.provider || aircraft.provenance?.provider ||
    aircraft.vectorwarp?.source_id || aircraft.provenance?.source_id || '').replace(/^exchange:/, '');
  const selectedSource = String(aircraft.vectorwarp?.source_id ||
    aircraft.provenance?.source_id || '');
  const sourceLabel = value => {
    const raw = String(value || '').replace(/^exchange:/, '');
    try { return new URL(raw).hostname || raw; } catch (_) { return raw; }
  };
  const corroboratedProviders = [...new Set((aircraft.vectorwarp?.agreeing_sources ||
    aircraft.provenance?.agreeing_sources || []).filter(source => String(source) !== selectedSource)
    .map(sourceLabel).filter(Boolean))];
  return {
    label: aircraftLabel(aircraft), registration: String(aircraft.r || '').trim(),
    type: String(aircraft.t || aircraft.aircraft_type || '').trim(),
    description: String(aircraft.desc || '').trim(), category: String(aircraft.category || position.category || '').trim(),
    hex: String(aircraft.hex || aircraft.id || '').trim().toUpperCase(), altitude,
    speed: finiteValue(aircraft.gs) ?? finiteValue(position.ground_speed_kt),
    // Ground track is the direction of travel and is therefore the correct
    // orientation for a moving map marker. Fall back to true/magnetic heading
    // only when the feed does not provide a track.
    heading: finiteValue(aircraft.track) ?? finiteValue(position.track_deg_true) ??
      finiteValue(aircraft.true_heading) ?? finiteValue(position.true_heading_deg) ??
      finiteValue(aircraft.mag_heading) ?? finiteValue(position.mag_heading_deg), verticalRate,
    squawk: String(aircraft.squawk || position.squawk || '').trim(),
    signal: finiteValue(aircraft.rssi) ?? finiteValue(position.rssi_dbfs),
    age: finiteValue(aircraft.seen_pos) ?? finiteValue(position.seen_pos_s), delay: finiteValue(aircraft.delay),
    doppler: finiteValue(aircraft.doppler), positionSource, provider,
    corroboratedProviders
  };
}

function aircraftScreenRotation(trackDegTrue, mapBearingDeg = 0) {
  if (trackDegTrue === null || trackDegTrue === undefined || trackDegTrue === '') return null;
  const track = Number(trackDegTrue);
  const bearing = Number(mapBearingDeg);
  if (!Number.isFinite(track)) return null;
  const relative = track - (Number.isFinite(bearing) ? bearing : 0);
  return ((relative % 360) + 360) % 360;
}

// ADS-B supplies direction of travel rather than pitch attitude. Derive a
// bounded flight-path angle so the map can show climbs and descents in 3D.
function aircraftFlightPathAngle(verticalRateFpm, groundSpeedKnots) {
  const vertical = Number(verticalRateFpm);
  const speed = Number(groundSpeedKnots);
  if (!Number.isFinite(vertical)) return 0;
  const horizontalFpm = Number.isFinite(speed) && speed > 0 ? speed * 6076.11549 / 60 : 0;
  const angle = horizontalFpm > 1 ? Math.atan2(vertical, horizontalFpm) * 180 / Math.PI :
    Math.sign(vertical) * Math.min(20, Math.abs(vertical) / 100);
  return Math.max(-20, Math.min(20, angle));
}

function aircraftIdentity(aircraft = {}) {
  const info = aircraftInfo(aircraft);
  return info.hex || info.label || null;
}

// Keep a compact browser-side trail without asking the live API for historical
// positions. Repeated CPIs at the same position do not grow the history.
function updateAircraftTrackHistories(histories, rows = [], options = {}) {
  const target = histories && typeof histories.get === 'function' &&
    typeof histories.set === 'function' ? histories : new Map();
  const now = Number.isFinite(options.now) ? options.now : Date.now();
  const maxAgeMs = Number.isFinite(options.maxAgeMs) ? Math.max(1000, options.maxAgeMs) : 15 * 60 * 1000;
  const maxPoints = Number.isFinite(options.maxPoints) ? Math.max(2, options.maxPoints) : 180;
  for (const row of Array.isArray(rows) ? rows : []) {
    const id = aircraftIdentity(row);
    const lat = Number(row?.lat), lon = Number(row?.lon);
    if (!id || !Number.isFinite(lat) || !Number.isFinite(lon) || row?._snapshot_stale) continue;
    const altitude = aircraftInfo(row).altitude;
    const history = target.get(id) || [];
    const previous = history[history.length - 1];
    if (!previous || previous.lat !== lat || previous.lon !== lon || previous.altitude !== altitude)
      history.push({lat, lon, altitude, at: now});
    target.set(id, history.filter(point => now - point.at <= maxAgeMs).slice(-maxPoints));
  }
  for (const [id, history] of target) {
    const recent = history.filter(point => now - point.at <= maxAgeMs).slice(-maxPoints);
    if (recent.length) target.set(id, recent); else target.delete(id);
  }
  return target;
}

function selectRadarAssociatedAircraft(currentId, rows = [], enabled = false) {
  if (!enabled) return currentId || null;
  const associated = (Array.isArray(rows) ? rows : []).filter(row => row?.radar_track_id);
  if (associated.some(row => aircraftIdentity(row) === currentId)) return currentId;
  associated.sort((left, right) => {
    const priority = row => ['ACTIVE', 'CONFIRMED'].includes(String(row?.radar_track_state).toUpperCase()) ? 0 : 1;
    return priority(left) - priority(right) ||
      Number(Boolean(left?._snapshot_stale)) - Number(Boolean(right?._snapshot_stale)) ||
      (aircraftInfo(left).age ?? Infinity) - (aircraftInfo(right).age ?? Infinity);
  });
  return associated.length ? aircraftIdentity(associated[0]) : (currentId || null);
}

// Warm near the surface, then pink/purple/indigo with increasing altitude.
// Ground and unknown-altitude targets remain neutral instead of implying data.
function aircraftAltitudeColor(aircraft = {}) {
  const info = aircraftInfo(aircraft);
  // Track-history points use the normalized `altitude` field while live feed
  // rows use alt_baro/alt_geom. Accept both so markers, legends, and trails
  // always share one continuous altitude palette.
  const normalizedAltitude = aircraft.altitude !== null && aircraft.altitude !== undefined &&
    aircraft.altitude !== '' && Number.isFinite(Number(aircraft.altitude)) ? Number(aircraft.altitude) : null;
  const rawAltitude = aircraft.alt_baro ?? aircraft.alt_geom ?? aircraft.altitude ??
    aircraft.position?.altitude_baro_ft;
  if (aircraftKind(aircraft) === 'ground' || String(rawAltitude).toLowerCase() === 'ground')
    return '#8f9994';
  const colorAltitude = info.altitude ?? normalizedAltitude;
  if (!Number.isFinite(colorAltitude)) return '#a7b0ac';
  const stops = [[0,[243,154,74]],[5000,[242,184,75]],[10000,[227,106,114]],
    [20000,[196,95,202]],[30000,[143,102,229]],[40000,[102,88,211]],
    [50000,[224,90,122]]];
  const altitude = Math.max(0, Math.min(50000, colorAltitude));
  let upper = stops.findIndex(stop => altitude <= stop[0]);
  if (upper <= 0) upper = 1;
  const [lowAltitude, low] = stops[upper - 1];
  const [highAltitude, high] = stops[upper];
  const ratio = (altitude - lowAltitude) / (highAltitude - lowAltitude);
  const channel = index => Math.round(low[index] + (high[index] - low[index]) * ratio)
    .toString(16).padStart(2, '0');
  return `#${channel(0)}${channel(1)}${channel(2)}`;
}

const aircraftSnapshotCaches = new Map();
function stabilizeAircraftSnapshots(rows = [], options = {}) {
  const scope = String(options.scope || 'default');
  const now = Number.isFinite(options.now) ? options.now : Date.now();
  const graceMs = Number.isFinite(options.graceMs) ? Math.max(0, options.graceMs) : 10000;
  let cache = aircraftSnapshotCaches.get(scope);
  if (!cache) {
    cache = new Map();
    try {
      const stored = JSON.parse(sessionStorage.getItem(`vectorwarp-aircraft-${scope}`) || '[]');
      for (const entry of stored) if (entry?.id && entry?.seenAt && entry?.row)
        cache.set(entry.id, {seenAt: entry.seenAt, row: entry.row});
    } catch (_) { /* Session storage is optional. */ }
    aircraftSnapshotCaches.set(scope, cache);
  }
  for (const row of Array.isArray(rows) ? rows : []) {
    if (!row || typeof row !== 'object') continue;
    const info = aircraftInfo(row);
    const id = info.hex || String(row.id || info.label || '').trim();
    if (!id) continue;
    const previous = cache.get(id)?.row || {};
    cache.set(id, {seenAt: now, row: {...previous, ...row, _snapshot_stale: false}});
  }
  for (const [id, entry] of cache)
    if (now - entry.seenAt > Math.max(graceMs, 30 * 60 * 1000)) cache.delete(id);
  const visible = [...cache.values()].filter(entry => now - entry.seenAt <= graceMs)
    .map(entry => ({...entry.row, _snapshot_stale: now > entry.seenAt,
      _snapshot_age_ms: now - entry.seenAt}));
  try {
    sessionStorage.setItem(`vectorwarp-aircraft-${scope}`, JSON.stringify([...cache.entries()]
      .map(([id, entry]) => ({id, ...entry}))));
  } catch (_) { /* Browsers may disable or fill session storage. */ }
  return visible;
}

function aircraftPlotData(aircraft = {}) {
  const info = aircraftInfo(aircraft);
  return [info.label, info.registration, info.type, info.description, info.hex,
    info.altitude, info.speed, info.heading, info.verticalRate, info.squawk,
    info.signal, info.age, info.category, info.positionSource, info.provider];
}

function aircraftHoverText(aircraft = {}, geometry = {}) {
  const info = aircraftInfo(aircraft);
  const safe = value => String(value ?? '').replace(/[&<>"']/g, character => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
  })[character]);
  const value = (input, digits = 0, suffix = '') => Number.isFinite(input) ?
    `${Number(input).toLocaleString(undefined, {minimumFractionDigits: digits,
      maximumFractionDigits: digits})}${suffix}` : '—';
  const lines = [`<b>${safe(info.label)}</b>`,
    `Registration ${safe(info.registration || '—')} · Type ${safe(info.type || '—')}`,
    info.description ? safe(info.description) : null,
    `ICAO ${safe(info.hex || '—')} · Category ${safe(info.category || '—')}`,
    `Altitude ${value(info.altitude, 0, ' ft')} · Speed ${value(info.speed, 0, ' kt')}`,
    `Heading ${value(info.heading, 0, '°')} · Vertical rate ${value(info.verticalRate, 0, ' ft/min')}`,
    `Squawk ${safe(info.squawk || '—')} · Signal ${value(info.signal, 1, ' dBFS')}`,
    `Position ${safe(info.positionSource)}${info.provider ? ` · ${safe(info.provider)}` : ''}`,
    info.corroboratedProviders.length ?
      `Corroborated by ${safe(info.corroboratedProviders.join(', '))}` : null,
    `Position age ${value(info.age, 1, ' s')}`];
  if (Number.isFinite(geometry.delay) || Number.isFinite(geometry.doppler))
    lines.push(`Bistatic range ${value(geometry.delay, 2, ' km')} · Doppler ${value(geometry.doppler, 1, ' Hz')}`);
  return lines.filter(Boolean).join('<br>');
}

const aircraftHoverTemplate = '<b>%{customdata[0]}</b><br>' +
  'Registration %{customdata[1]} · Type %{customdata[2]}<br>' +
  '%{customdata[3]}<br>ICAO %{customdata[4]} · Category %{customdata[12]}<br>' +
  'Altitude %{customdata[5]:,.0f} ft · Speed %{customdata[6]:.0f} kt<br>' +
  'Heading %{customdata[7]:.0f}° · Vertical rate %{customdata[8]:+.0f} ft/min<br>' +
  'Squawk %{customdata[9]} · Signal %{customdata[10]:.1f} dBFS<br>' +
  'Position age %{customdata[11]:.1f} s<extra></extra>';

function aircraftSvgMarkup(aircraft, color = '#4dd4b0') {
  const kind = aircraftKind(aircraft);
  const layered = (shape, options = {}) => {
    const attributes = options.attributes || '';
    return `<svg class="aircraft-3d-svg" viewBox="0 0 ${options.viewBox || '100 100'}" width="38" height="38" aria-hidden="true"><g class="aircraft-3d-depth" color="${color}" fill="${options.fill || color}" stroke="${options.stroke || color}" ${attributes} transform="translate(4 6)">${shape}</g><g class="aircraft-3d-top" color="${color}" fill="${options.fill || color}" stroke="${options.stroke || color}" ${attributes}>${shape}</g><path class="aircraft-3d-highlight" d="M50 5 53 24 50 20 47 24Z"/></svg>`;
  };
  if (kind === 'rotorcraft') {
    // Original VectorWarp planform: a compact, single-color rotor silhouette
    // that remains recognizable at map scale. The straight boom deliberately
    // avoids the split-tail "chicken bone" shape of generic helicopter glyphs.
    return layered('<path d="M10 8 90 70M90 8 10 70" fill="none" stroke-width="5"/><path d="M50 17c10 0 15 8 14 21-1 10-5 17-11 21v35h-6V59c-7-4-10-11-11-21-1-13 4-21 14-21Z"/><path d="M39 90h22v6H39z"/><circle cx="50" cy="39" r="5"/>',
      {attributes: 'stroke-linecap="round" stroke-linejoin="round"'});
  }
  // Original VectorWarp top-down planforms. These stay license-neutral while
  // preserving recognisable proportions at the map's normal 38 px scale.
  const planforms = {
    airliner: '<path d="M47 4Q50 0 53 4L56 33 91 54 90 61 56 49 55 77 69 89 68 94 51 88 32 94 31 89 45 77 44 49 10 61 9 54 44 33Z"/><path d="M31 48q-6 0-7 6t8 7l5-2-1-9zm38 0q6 0 7 6t-8 7l-5-2 1-9z" opacity=".55"/>',
    aircraft: '<path d="M47 4Q50 0 53 4L56 33 91 54 90 61 56 49 55 77 69 89 68 94 51 88 32 94 31 89 45 77 44 49 10 61 9 54 44 33Z"/>',
    heavy: '<path d="M46 3Q50-1 54 3L58 31 97 53 96 61 57 49 56 77 72 89 71 95 50 88 29 95 28 89 44 77 43 49 4 61 3 53 42 31Z"/><path d="M18 51q-5 0-6 5t7 6l4-2v-8zm18-6q-5 0-6 6t7 6l4-2-1-9zm46 6q5 0 6 5t-7 6l-4-2v-8zm-18-6q5 0 6 6t-7 6l-4-2 1-9z" opacity=".55"/>',
    light: '<path d="M46 17Q50 10 54 17L56 37 88 44 89 53 57 51 56 75 67 86 66 91 50 86 34 91 33 86 44 75 43 51 11 53 12 44 44 37Z"/><path d="M48 5h4v10h-4zM31 8h38v4H31z"/>',
    glider: '<path d="M47 7Q50 2 53 7L54 40c13 1 29 3 45 7v6c-16-1-31-2-45-1l1 26 13 10-2 5-16-6-16 6-2-5 13-10 1-26c-14-1-29 0-45 1v-6c16-4 32-6 45-7Z"/><path d="M47 17q3-7 6 0v18h-6Z" opacity=".55"/>',
    drone: '<path d="M47 8Q50 3 53 8L55 37 94 48v7l-39-5 1 27 16 11-4 6-18-9-18 9-4-6 16-11 1-27-39 5v-7l39-11Z"/><path d="M46 25h8l3 17-7 8-7-8Z" opacity=".55"/><path d="M29 83h42M50 74v24m-6-23-15 13m27-13 15 13" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round"/>'
  };
  if (planforms[kind])
    return layered(planforms[kind], {attributes: 'stroke-width="2" stroke-linecap="round" stroke-linejoin="round"'});
  // Ground vehicles retain the compact MIT-licensed Tabler glyph.
  const paths = {
    ground: '<path d="M5 17a2 2 0 1 0 4 0 2 2 0 1 0-4 0M15 17a2 2 0 1 0 4 0 2 2 0 1 0-4 0M5 17H3V6a1 1 0 0 1 1-1h9v12M9 17h6M19 17h2v-6h-8M13 6h5l3 5"/>',
  };
  return `<svg viewBox="0 0 24 24" width="38" height="38" aria-hidden="true"><g fill="none" stroke="${color}" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">${paths[kind] || paths.ground}</g></svg>`;
}

function siteSvgMarkup(kind = 'receiver') {
  if (kind === 'transmitter') return '<svg class="site-3d-svg" viewBox="0 0 64 72" width="42" height="48" aria-hidden="true"><path class="site-3d-shadow" d="m17 64 18-9 18 9-18 7Z"/><path class="site-3d-side" d="M32 8 49 62 35 69 35 18Z"/><path class="site-3d-front" d="M32 8 15 62 29 69 32 18ZM21 46h22M18 58h28M25 34h14"/><path class="site-3d-accent" d="M32 8v-5M19 12a18 18 0 0 0 0 25M45 12a18 18 0 0 1 0 25M13 7a26 26 0 0 0 0 35M51 7a26 26 0 0 1 0 35"/></svg>';
  return '<svg class="site-3d-svg" viewBox="0 0 64 72" width="42" height="48" aria-hidden="true"><path class="site-3d-shadow" d="m10 61 23-11 23 11-23 10Z"/><path class="site-3d-side" d="m33 50 23 11-23 10Z"/><path class="site-3d-front" d="m10 61 23-11v21Z"/><path class="site-3d-mast" d="M30 54V22h6v32M18 34l15-8 15 8-15 8ZM24 46l9-5 9 5-9 5Z"/><circle class="site-3d-accent-fill" cx="33" cy="17" r="5"/><path class="site-3d-accent" d="M22 17a11 11 0 0 1 22 0M16 17a17 17 0 0 1 34 0"/></svg>';
}

if (typeof module !== 'undefined')
  module.exports = {configuredApiPort, liveApiUrl,
    recordingDuration, rememberApiPort, radarFrameIntervalMs, startRadarUpdates,
    startRadarPlot, getRadarRuntimeConfig, radarPresentationUrl, discoverApiEndpoint, fetchStatusResource,
    refreshLiveStatus, refreshReceiverDevices, updateLiveStatusLabel, aircraftKind, aircraftPlotSymbol,
    aircraftLabel, aircraftInfo, aircraftIdentity, aircraftScreenRotation, aircraftFlightPathAngle,
    aircraftPlotData, aircraftHoverText, aircraftHoverTemplate, aircraftSvgMarkup, aircraftAltitudeColor,
    siteSvgMarkup,
    stabilizeAircraftSnapshots, updateAircraftTrackHistories, selectRadarAssociatedAircraft};
