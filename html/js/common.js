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
  target.innerHTML = `<a class="brand" href="/"><img class="brand-logo" src="/favicon/vectorwarp-vw.svg" alt="VectorWarp"><span>VectorWarp</span></a><nav class="app-nav" aria-label="Primary navigation"><a href="/" class="${active === 'radar' ? 'active' : ''}">Live radar</a><a href="/controller" class="${active === 'controller' ? 'active' : ''}">Displays</a><a href="/display/configuration" class="${active === 'settings' ? 'active' : ''}">Settings</a></nav><div class="service-statuses" aria-label="Service status"><div class="live-status" id="live-status"><span class="live-dot"></span>RADAR CONNECTING</div><div class="live-status" id="adsb-status" hidden><span class="live-dot"></span>ADS-B CONNECTING</div></div><div class="processor-alert" id="processor-alert" role="alert" hidden></div><a class="receiver-device-alert" id="receiver-device-alert" href="/display/configuration?setup=1" role="alert" hidden></a><div class="receiver-health-status" id="receiver-health-status" role="status" aria-live="polite" aria-atomic="true" hidden></div>`;
  refreshLiveStatus();
  if (window.blah2LiveStatusTimer)
    window.clearInterval(window.blah2LiveStatusTimer);
  window.blah2LiveStatusTimer = window.setInterval(refreshLiveStatus, 2000);
  refreshReceiverDevices();
  if (window.blah2ReceiverDeviceTimer)
    window.clearInterval(window.blah2ReceiverDeviceTimer);
  window.blah2ReceiverDeviceTimer = window.setInterval(refreshReceiverDevices, 30000);
  routeApiLinks();
  addRecordingControl();
  addFullscreenControl();
}

// Discovery is read-only. A newly connected receiver is offered to the user,
// but the running receiver and saved configuration are never changed here.
async function refreshReceiverDevices() {
  const target = document.getElementById('receiver-device-alert');
  if (!target || window.blah2ReceiverDevicePending) return;
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
    copy.textContent = recordingState.file ? `Writing ${recordingState.file}` : 'Recording confirmed.';
    button.textContent = 'Stop recording';
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
      {method: 'POST', headers: {'X-VectorWarp-Intent': 'recording-toggle-v1'}, cache: 'no-store'});
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
  control.innerHTML = '<div class="recording-status"><span class="recording-dot"></span>Checking recording…</div><div class="recording-copy">Press Space to start. Press again to stop.</div><button class="recording-toggle" type="button">Start recording</button>';
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
  const fullscreenElement = () => document.fullscreenElement || document.webkitFullscreenElement;
  const fallbackActive = () => panel.classList.contains('fullscreen-fallback');
  const setFallback = active => {
    panel.classList.toggle('fullscreen-fallback', active);
    document.documentElement.classList.toggle('fullscreen-page', active);
    document.body.classList.toggle('fullscreen-page', active);
    button.textContent = active ? 'Exit fullscreen' : 'Fullscreen';
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
    button.textContent = fullscreenElement() ? 'Exit fullscreen' : 'Fullscreen';
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
  if (!radarLabel || window.blah2LiveStatusPending) return;
  window.blah2LiveStatusPending = true;
  try {
    const response = await fetchStatusResource(liveApiUrl('/api/system/status'), {cache: 'no-store'});
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
    renderReceiverHealth(null);
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

function startRadarUpdates(update, minimumIntervalMs = 16) {
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
  // A full heatmap is materially heavier than a status card.  Tablets and
  // phones should display fresh radar data without continuously parsing and
  // repainting faster than their browser can keep up.
  const minimumIntervalMs = typeof window !== 'undefined' && window.innerWidth <= 1024 ? 1000 : 100;
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

if (typeof module !== 'undefined')
  module.exports = {configuredApiPort, liveApiUrl,
    recordingDuration, rememberApiPort, radarFrameIntervalMs, startRadarUpdates,
    startRadarPlot, getRadarRuntimeConfig, discoverApiEndpoint, fetchStatusResource,
    refreshLiveStatus, refreshReceiverDevices};
