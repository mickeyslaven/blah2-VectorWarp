(function () {
  'use strict';

  const root = document.getElementById('view-root');
  const state = document.getElementById('view-state');
  const view = document.body.dataset.insightView;
  const colors = ['#ff8754', '#4dd4b0', '#70a7ff', '#f7c75f', '#d995ff', '#ff8298'];
  const plotConfig = {responsive: true, displayModeBar: false, scrollZoom: true};
  const plotBase = {
    autosize: true,
    paper_bgcolor: 'rgba(0,0,0,0)',
    plot_bgcolor: 'rgba(0,0,0,0)',
    font: {color: '#f3eee9', family: 'Inter, system-ui, sans-serif'},
    margin: {l: 72, r: 38, t: 24, b: 58},
    hoverlabel: {namelength: 24},
    legend: {orientation: 'h', x: 0, y: 1.08, bgcolor: 'rgba(0,0,0,0)'},
    // Plotly.react is used for the live insight charts. A stable revision keeps
    // an operator's zoom/pan while fresh traces arrive.
    uirevision: view
  };
  let refreshing = false;
  let nextRefreshAt = 0;
  let consecutiveRefreshFailures = 0;
  const minimumRefreshIntervalMs = 2000;

  function finite(value) {
    if (value === null || value === undefined || value === '' || typeof value === 'boolean') return null;
    const number = Number(value);
    return Number.isFinite(number) ? number : null;
  }

  function array(value) {
    return Array.isArray(value) ? value : [];
  }

  function escapeHtml(value) {
    return String(value ?? '').replace(/[&<>'"]/g, character => ({
      '&': '&amp;', '<': '&lt;', '>': '&gt;', "'": '&#39;', '"': '&quot;'
    })[character]);
  }

  function humanize(value) {
    return String(value ?? 'Unavailable').replace(/[_-]+/g, ' ')
      .replace(/\b\w/g, character => character.toUpperCase());
  }

  function number(value, digits = 1, fallback = '—') {
    const parsed = finite(value);
    return parsed === null ? fallback : parsed.toLocaleString(undefined, {
      minimumFractionDigits: digits, maximumFractionDigits: digits
    });
  }

  function integer(value, fallback = '—') {
    const parsed = finite(value);
    return parsed === null ? fallback : Math.round(parsed).toLocaleString();
  }

  function percent(value, digits = 0) {
    const parsed = finite(value);
    if (parsed === null) return '—';
    const normalized = Math.abs(parsed) <= 1 ? parsed * 100 : parsed;
    return `${number(normalized, digits)}%`;
  }

  function timeLabel(timestamp) {
    const parsed = finite(timestamp);
    return parsed === null ? 'No timestamp' : new Date(parsed).toLocaleTimeString();
  }

  function sourceAgeSeconds(timestamp) {
    const parsed = finite(timestamp);
    if (parsed === null) return null;
    return Math.max(0, parsed > 1e12 ? (Date.now() - parsed) / 1000 : Date.now() / 1000 - parsed);
  }

  function setState(label, tone = '') {
    if (!state) return;
    state.textContent = label;
    state.className = `view-state ${tone}`.trim();
  }

  async function fetchJson(url, options = {}) {
    const response = await fetchStatusResource(url, {cache: 'no-store', ...options});
    if (!response.ok) throw new Error(`${url} returned ${response.status}`);
    const text = await response.text();
    if (!text.trim()) return null;
    return JSON.parse(text);
  }

  async function apiJson(path) {
    const aligned = /^\/api\/(?:adsb(?:\/delay-doppler)?|detection|tracker|bearing|locations|timing|iqdata)(?:[/?]|$)/.test(path) ?
      radarPresentationUrl(path) : path;
    return fetchJson(liveApiUrl(aligned));
  }

  async function getConfig() {
    return getRadarRuntimeConfig();
  }

  async function getAdsb() {
    return apiJson('/api/adsb/delay-doppler');
  }

  async function adsbHealth(config) {
    if (config?.truth?.adsb?.enabled !== true) return {enabled: false, online: null};
    try {
      return await apiJson('/api/adsb/status');
    } catch (_) {
      return {enabled: true, online: false};
    }
  }

  function channelCount(config) {
    const device = config?.capture?.device || {};
    const explicit = finite(device.channel_count);
    if (explicit !== null) return explicit;
    if (array(device.gain).length) return array(device.gain).length;
    if (array(device.surveillance_channels).length) return array(device.surveillance_channels).length;
    return ['RspDuo', 'Usrp', 'HackRF'].includes(device.type) ? 2 : null;
  }

  function rangeResolutionKm(config) {
    const sampleRate = finite(config?.capture?.fs);
    return sampleRate && sampleRate > 0 ? 299792458 / sampleRate / 1000 : null;
  }

  function frequencyLabel(config) {
    const frequency = finite(config?.capture?.fc);
    return frequency === null ? 'Frequency unavailable' : `${number(frequency / 1e6, 3)} MHz`;
  }

  function card(value, label, tone = '') {
    return `<article class="metric-card ${tone}"><strong>${escapeHtml(value)}</strong><span>${escapeHtml(label)}</span></article>`;
  }

  function empty(title, detail, action = '') {
    return `<div class="view-empty"><strong>${escapeHtml(title)}</strong><span>${escapeHtml(detail)}</span>${action}</div>`;
  }

  // Polling views used to assign root.innerHTML on every frame. Besides making
  // mobile browsers visibly flash, that discards the focused table/scroll node
  // and any browser-managed state inside it. Reconcile the small HTML views in
  // place instead; plot views already keep a stable scaffold and use Plotly.react.
  function renderHtml(target, markup) {
    if (!target || target.innerHTML === markup) return;
    // Keep the lightweight non-DOM smoke-test fixture compatible.
    if (!document.createElement || !target.childNodes) {
      target.innerHTML = markup;
      return;
    }
    const template = document.createElement('template');
    template.innerHTML = markup;
    const patchNode = (current, next) => {
      if (current.nodeType !== next.nodeType ||
          (current.nodeType === 1 && current.tagName !== next.tagName))
        return next.cloneNode(true);
      if (current.nodeType === 3) {
        if (current.nodeValue !== next.nodeValue) current.nodeValue = next.nodeValue;
        return current;
      }
      if (current.nodeType !== 1) return current;
      // Expanded/collapsed details are user interface state, not server data.
      // Reapplying the markup's `open` attribute on every poll closed menus the
      // operator had opened (or reopened menus they had deliberately closed).
      const preserveDetailsOpen = current.tagName === 'DETAILS';
      for (const attribute of Array.from(current.attributes)) {
        if (preserveDetailsOpen && attribute.name === 'open') continue;
        if (!next.hasAttribute(attribute.name)) current.removeAttribute(attribute.name);
      }
      for (const attribute of Array.from(next.attributes)) {
        if (preserveDetailsOpen && attribute.name === 'open') continue;
        if (current.getAttribute(attribute.name) !== attribute.value)
          current.setAttribute(attribute.name, attribute.value);
      }
      const oldChildren = Array.from(current.childNodes);
      const newChildren = Array.from(next.childNodes);
      for (let index = 0; index < newChildren.length; index++) {
        const oldChild = oldChildren[index];
        if (!oldChild) current.appendChild(newChildren[index].cloneNode(true));
        else {
          const patched = patchNode(oldChild, newChildren[index]);
          if (patched !== oldChild) current.replaceChild(patched, oldChild);
        }
      }
      while (current.childNodes.length > newChildren.length)
        current.removeChild(current.lastChild);
      return current;
    };
    const oldChildren = Array.from(target.childNodes);
    const newChildren = Array.from(template.content.childNodes);
    for (let index = 0; index < newChildren.length; index++) {
      const oldChild = oldChildren[index];
      if (!oldChild) target.appendChild(newChildren[index].cloneNode(true));
      else {
        const patched = patchNode(oldChild, newChildren[index]);
        if (patched !== oldChild) target.replaceChild(patched, oldChild);
      }
    }
    while (target.childNodes.length > newChildren.length)
      target.removeChild(target.lastChild);
  }

  function renderDetail(markup) {
    renderHtml(document.getElementById('view-detail'), markup);
  }

  function table(headers, rows) {
    return `<div class="data-table-wrap" tabindex="0" role="region" aria-label="Data table"><table class="data-table"><thead><tr>${headers.map(item => `<th>${escapeHtml(item)}</th>`).join('')}</tr></thead><tbody>${rows.length ? rows.join('') : `<tr><td colspan="${headers.length}" class="table-empty">No current rows</td></tr>`}</tbody></table></div>`;
  }

  function detectionRows(data) {
    const delays = array(data?.delay);
    return delays.map((delay, index) => ({
      index,
      delay: finite(delay),
      doppler: finite(data?.doppler?.[index]),
      snr: finite(data?.snr?.[index]),
    }));
  }

  function trackRangeKm(value, config) {
    const bins = finite(value);
    const resolution = rangeResolutionKm(config);
    return bins === null || resolution === null ? null : bins * resolution;
  }

  function setupPlotScaffold(extraClass = '') {
    if (root.dataset.ready) return;
    root.innerHTML = `<div class="insight-stats" id="view-summary"></div><section class="insight-content ${extraClass}"><div class="panel insight-plot-card"><div class="visualization insight-plot" id="data"></div></div><div id="view-detail"></div></section>`;
    root.dataset.ready = 'true';
    addRecordingControl();
    addFullscreenControl();
  }

  function setupLocationMapScaffold() {
    if (root.dataset.ready) return;
    root.innerHTML = `<section class="insight-content insight-content-wide location-map-layout"><div class="panel insight-plot-card location-map-panel"><div class="visualization insight-plot" id="data"></div><details class="location-map-sidebar"><summary><span class="map-sidebar-title">Map</span><span class="map-sidebar-selected" id="map-sidebar-selected">Layers & aircraft</span></summary><div class="location-map-sidebar-body"><details class="map-controls-panel"><summary>Layers and controls</summary><div class="map-controls-body"><label class="map-auto-select"><input type="checkbox" id="show-radar-tracks-on-map"><span><strong>Show radar tracks</strong><small>Confirmed tracks use the 3D uncertainty cage.</small></span></label><label class="map-auto-select"><input type="checkbox" id="show-radar-coverage-on-map"><span><strong>24 h radar-confirmed coverage</strong><small>Online feed-only positions never expand it.</small></span></label><label class="map-coverage-filter"><span>Coverage altitude</span><select id="radar-coverage-altitude"><option value="all">All altitudes</option><option value="low">0–10,000 ft</option><option value="medium">10–20,000 ft</option><option value="high">20–30,000 ft</option><option value="very_high">30–40,000 ft</option><option value="extreme">40,000+ ft</option></select></label><label class="map-auto-select"><input type="checkbox" id="auto-select-radar-aircraft"><span><strong>Auto-select radar matches</strong><small>Keep the current radar-associated aircraft selected.</small></span></label><div class="aircraft-altitude-legend" aria-label="Aircraft altitude colors"><strong>Altitude</strong><span class="aircraft-altitude-gradient"></span><span>Ground</span><span>0</span><span>10k</span><span>20k</span><span>30k</span><span>40k+ ft</span></div></div></details><div class="insight-stats" id="view-summary"></div><div id="selected-aircraft-detail" aria-live="polite"></div><div id="view-detail"></div></div></details></div></section>`;
    root.dataset.ready = 'true';
    const coverageLabel = root.querySelector?.('#show-radar-coverage-on-map')?.closest?.('label');
    if (coverageLabel) {
      coverageLabel.querySelector('strong').textContent = '24 h observed radar reach';
      coverageLabel.querySelector('small').textContent = 'Confirmed track sectors only; gaps mean no evidence.';
    }
    const sidebar = root.querySelector?.('.location-map-sidebar');
    if (sidebar) sidebar.open = !window.matchMedia?.('(max-width:760px)')?.matches;
    const showTracks = document.getElementById('show-radar-tracks-on-map');
    const showCoverage = document.getElementById('show-radar-coverage-on-map');
    const coverageAltitude = document.getElementById('radar-coverage-altitude');
    const autoSelect = document.getElementById('auto-select-radar-aircraft');
    const syncTrackControls = () => {
      if (showTracks) showTracks.checked = showRadarTracksOnMap;
      if (showCoverage) showCoverage.checked = showRadarCoverageOnMap;
      if (coverageAltitude) {
        coverageAltitude.value = radarCoverageAltitudeBand;
        coverageAltitude.disabled = !showRadarCoverageOnMap;
      }
      if (autoSelect) {
        autoSelect.checked = autoSelectRadarAircraft;
        autoSelect.disabled = !showRadarTracksOnMap;
        autoSelect.closest?.('label')?.classList?.toggle('disabled', !showRadarTracksOnMap);
      }
    };
    syncTrackControls();
    showTracks?.addEventListener?.('change', () => {
      showRadarTracksOnMap = showTracks.checked;
      try { localStorage.setItem('vectorwarp-show-radar-tracks-on-map', String(showRadarTracksOnMap)); }
      catch (_) { /* Persistence is optional. */ }
      syncTrackControls();
      renderLocations().catch(error => { window.blah2ViewLastError = error?.message || String(error); });
    });
    showCoverage?.addEventListener?.('change', () => {
      showRadarCoverageOnMap = showCoverage.checked;
      try { localStorage.setItem('vectorwarp-show-radar-coverage-on-map', String(showRadarCoverageOnMap)); }
      catch (_) { /* Persistence is optional. */ }
      syncTrackControls();
      renderLocations().catch(error => { window.blah2ViewLastError = error?.message || String(error); });
    });
    coverageAltitude?.addEventListener?.('change', () => {
      radarCoverageAltitudeBand = coverageAltitude.value || 'all';
      radarCoverageCache = null;
      try { localStorage.setItem('vectorwarp-radar-coverage-altitude', radarCoverageAltitudeBand); }
      catch (_) { /* Persistence is optional. */ }
      renderLocations().catch(error => { window.blah2ViewLastError = error?.message || String(error); });
    });
    if (autoSelect) {
      autoSelect.addEventListener?.('change', () => {
        autoSelectRadarAircraft = autoSelect.checked;
        try { localStorage.setItem('vectorwarp-auto-select-radar-aircraft', String(autoSelectRadarAircraft)); }
        catch (_) { /* Persistence is optional. */ }
        const layer = document.querySelector?.('.location-aircraft-layer');
        const rows = layer?._aircraftSnapshot || [];
        selectedAircraftId = selectRadarAssociatedAircraft(selectedAircraftId, rows, autoSelectRadarAircraft);
        selectedAircraftSnapshot = rows.find(row => aircraftIdentity(row) === selectedAircraftId) || selectedAircraftSnapshot;
        renderSelectedAircraftCard(rows);
        layer?._updatePositions?.();
      });
    }
    addRecordingControl();
    addFullscreenControl();
  }

  async function emptyPlot(message, layout = {}) {
    await Plotly.react('data', [], {...plotBase, ...layout, annotations: [{
      text: message, showarrow: false, font: {color: '#ac9e94', size: 14}
    }]}, plotConfig);
  }

  async function renderOverview() {
    const [config, detection, tracker, timing] = await Promise.all([
      getConfig(), apiJson('/api/detection'), apiJson('/api/tracker'), apiJson('/api/timing')
    ]);
    const adsb = await adsbHealth(config);
    const detections = detectionRows(detection);
    const tracks = array(tracker?.data);
    const strongest = [...detections].sort((left, right) => (right.snr ?? -Infinity) - (left.snr ?? -Infinity))[0];
    const hop = finite(config?.process?.data?.cpi) === null ? null : config.process.data.cpi * 1000;
    const load = hop && finite(timing?.cpi) !== null ? timing.cpi / hop * 100 : null;
    const receiver = config?.location?.rx?.name || 'Receiver site';
    const transmitter = config?.location?.tx?.name || 'Illuminator site';
    const health = healthSnapshot(config, timing, tracker, adsb);
    renderHtml(root, `<div class="insight-stats insight-stats-four">
      ${card(detections.length, 'Detections in latest CPI')}
      ${card(tracks.length, 'Persistent tracks')}
      ${card(strongest ? `${number(strongest.snr, 1)} dB` : 'None', 'Strongest detection')}
      ${card(load === null ? '—' : `${number(load, 0)}%`, 'Load', load > 100 ? 'bad' : load > 80 ? 'warning' : 'good')}
    </div><div class="overview-grid">
      <section class="info-card"><h2>Radar</h2><dl><div><dt>Receiver</dt><dd>${escapeHtml(receiver)}</dd></div><div><dt>Illuminator</dt><dd>${escapeHtml(transmitter)}</dd></div><div><dt>Radio</dt><dd>${escapeHtml(config?.capture?.device?.type || 'Receiver')} · ${escapeHtml(frequencyLabel(config))}</dd></div><div><dt>Inputs</dt><dd>${escapeHtml(channelCount(config) ?? 'Unknown')} channels</dd></div></dl></section>
      <section class="info-card"><h2>Current activity</h2>${strongest ? `<dl><div><dt>Bistatic range</dt><dd>${number(strongest.delay, 2)} km</dd></div><div><dt>Doppler</dt><dd>${number(strongest.doppler, 1)} Hz</dd></div><div><dt>Signal-to-noise ratio</dt><dd>${number(strongest.snr, 1)} dB</dd></div><div><dt>Frame</dt><dd>${escapeHtml(timeLabel(detection?.timestamp))}</dd></div></dl>` : empty('No detections in the latest CPI', 'The radar is running; no return currently passes the configured detector.')}</section>
      <section class="info-card overview-links"><h2>Open a view</h2><a href="/display/locations">Map <b>→</b></a><a href="/display/detections">Current detections <b>→</b></a><a href="/display/tracks">Tracks <b>→</b></a><a href="/display/health">Detailed diagnostics <b>→</b></a><a href="/display/site">Site geometry <b>→</b></a></section>
      <section class="overview-health"><header><div><h2>System health</h2><p>Everything a new operator needs to confirm before using the radar.</p></div><span class="state-pill ${health.overallTone === 'good' ? 'active' : health.overallTone === 'warning' ? 'coasting' : ''}">${escapeHtml(health.overall)}</span></header><div class="health-list">${health.rows}</div><a class="button-link" href="/display/health">Open stage timings and engineering diagnostics</a></section>
    </div>`);
    setState(`Updated ${timeLabel(timing?.timestamp)}`, health.overallTone);
  }

  async function renderDetections() {
    const detection = await apiJson('/api/detection');
    const rows = detectionRows(detection).sort((left, right) => (right.snr ?? -Infinity) - (left.snr ?? -Infinity));
    const strongest = rows[0];
    const body = rows.map(item => `<tr><td>${item.index + 1}</td><td>${number(item.delay, 2)}</td><td>${number(item.doppler, 1)}</td><td>${number(item.snr, 1)}</td></tr>`);
    renderHtml(root, `<div class="insight-stats">${card(rows.length, 'Latest CPI')}${card(strongest ? `${number(strongest.snr, 1)} dB` : 'None', 'Strongest return')}</div>${rows.length ? table(['#', 'Bistatic range (km)', 'Doppler (Hz)', 'SNR (dB)'], body) : empty('No detections in the latest CPI', 'This is a valid live result; nothing currently passes the configured detector.')}`);
    setState(`Frame ${timeLabel(detection?.timestamp)}`, 'good');
  }

  function healthRow(name, value, detail, tone) {
    return `<article class="health-row"><span class="health-indicator ${tone}"></span><div><strong>${escapeHtml(name)}</strong><small>${escapeHtml(detail)}</small></div><b>${escapeHtml(value)}</b></article>`;
  }

  function healthSnapshot(config, timing, tracker, adsb) {
    const configuredCpi = finite(config?.process?.data?.cpi);
    const hop = configuredCpi === null ? null : configuredCpi * 1000;
    const processMs = finite(timing?.cpi);
    const load = processMs !== null && hop > 0 ? processMs / hop * 100 : null;
    const backlog = finite(timing?.capture_backlog_ms);
    const ageMs = finite(timing?.timestamp) === null ? null : Math.max(0, Date.now() - timing.timestamp);
    const freshLimit = Math.max(2500, (hop ?? 0) * 3);
    const freshnessTone = ageMs === null || ageMs > 10000 ? 'bad' : ageMs > freshLimit ? 'warning' : 'good';
    const loadTone = load === null ? 'neutral' : load > 100 ? 'bad' : load > 80 ? 'warning' : 'good';
    const backlogTone = backlog === null || hop === null ? 'neutral' : backlog > hop ? 'bad' : backlog > Math.max(10, hop * .25) ? 'warning' : 'good';
    const trackerEnabled = config?.process?.tracker?.enable === true;
    const trackerTone = trackerEnabled ? (tracker ? 'good' : 'bad') : 'neutral';
    const adsbTone = !adsb.enabled ? 'neutral' : adsb.online ? 'good' : 'bad';
    const tones = [freshnessTone, loadTone, backlogTone, trackerTone];
    if (adsb.enabled) tones.push(adsbTone);
    const overall = tones.includes('bad') ? 'Attention needed' : tones.includes('warning') ? 'Monitor' : 'Healthy';
    const overallTone = tones.includes('bad') ? 'bad' : tones.includes('warning') ? 'warning' : 'good';
    const rows = healthRow('Radar frames', ageMs === null ? 'Unavailable' : ageMs < 1000 ? 'Live' : `${number(ageMs / 1000, 1)} s old`, `Updates every ${number(hop, 0)} ms`, freshnessTone) +
      healthRow('Processing load', load === null ? 'Unavailable' : `${number(load, 0)}%`, processMs === null ? 'No timing data' : `${number(processMs, 1)} ms per ${number(hop, 1)} ms update`, loadTone) +
      healthRow('Capture backlog', backlog === null ? 'Unavailable' : `${number(backlog, 1)} ms`, 'Captured data waiting to be processed', backlogTone) +
      healthRow('Tracking', trackerEnabled ? 'Enabled' : 'Disabled', trackerEnabled ? `${integer(tracker?.n ?? 0)} track candidates` : 'Optional processing is off', trackerTone) +
      healthRow('ADS-B', adsb.disabledForReplay ? 'Not replayed' : !adsb.enabled ? 'Disabled' : adsb.online ? 'Online' : 'Offline', adsb.disabledForReplay ? 'Not shown during replay.' : adsb.enabled ? 'For comparison only; not used by radar.' : 'Optional feed is off', adsbTone);
    return {hop, processMs, load, backlog, overall, overallTone, rows};
  }

  async function renderHealth() {
    const [config, timing, tracker] = await Promise.all([getConfig(), apiJson('/api/timing'), apiJson('/api/tracker')]);
    const adsb = await adsbHealth(config);
    const health = healthSnapshot(config, timing, tracker, adsb);
    const stages = Object.entries(timing || {}).filter(([key, value]) => typeof value === 'number' && finite(value) !== null && !['timestamp', 'nCpi', 'uptime_s', 'uptime_days', 'cpi_data_ms', 'cpi_hop_ms', 'cpi_overlap_fraction', 'capture_backlog_ms', 'cpi'].includes(key)).sort((left, right) => Number(right[1]) - Number(left[1]));
    renderHtml(root, `<div class="insight-stats">${card(health.overall, 'Overall state', health.overallTone)}${card(health.load === null ? '—' : `${number(health.load, 0)}%`, 'Load', health.load > 100 ? 'bad' : health.load > 80 ? 'warning' : 'good')}${card(`${channelCount(config) ?? '—'}`, 'Channels')}${card(timing?.uptime_s === undefined ? '—' : `${number(timing.uptime_s / 3600, 1)} h`, 'Uptime')}</div><div class="health-grid"><section class="health-list">${health.rows}${healthRow('Detection', config?.process?.detection?.enable === false ? 'Disabled' : 'Enabled', 'Shows current detections', config?.process?.detection?.enable === false ? 'neutral' : 'good')}</section><section class="info-card"><h2>Stage times</h2>${table(['Stage', 'Latest time (ms)'], stages.map(([name, value]) => `<tr><td>${escapeHtml(humanize(name))}</td><td>${number(value, 2)}</td></tr>`))}</section></div>`);
    setState(`Updated ${timeLabel(timing?.timestamp)}`, health.overallTone);
  }

  let activityHistory = [];
  try { activityHistory = JSON.parse(sessionStorage.getItem('blah2-activity-history') || '[]').filter(item => Date.now() - item.time < 3600000); } catch (_) { activityHistory = []; }

  async function renderActivity() {
    setupPlotScaffold();
    const [config, detection, tracker, timing] = await Promise.all([getConfig(), apiJson('/api/detection'), apiJson('/api/tracker'), apiJson('/api/timing')]);
    const rows = detectionRows(detection);
    const timestamp = finite(detection?.timestamp) ?? Date.now();
    const hop = finite(config?.process?.data?.cpi) === null ? null : config.process.data.cpi * 1000;
    const load = hop && finite(timing?.cpi) !== null ? timing.cpi / hop * 100 : null;
    if (!activityHistory.length || activityHistory[activityHistory.length - 1].time !== timestamp) {
      activityHistory.push({time: timestamp, detections: rows.length, tracks: array(tracker?.data).length, snr: rows.reduce((best, item) => Math.max(best, item.snr ?? -Infinity), -Infinity), load});
      activityHistory = activityHistory.slice(-300);
      try { sessionStorage.setItem('blah2-activity-history', JSON.stringify(activityHistory)); } catch (_) { /* Storage is optional. */ }
    }
    const latest = activityHistory[activityHistory.length - 1] || {};
    renderHtml(document.getElementById('view-summary'), `${card(latest.detections ?? 0, 'Latest detections')}${card(latest.tracks ?? 0, 'Persistent tracks')}${card(Number.isFinite(latest.snr) ? `${number(latest.snr, 1)} dB` : 'None', 'Strongest return')}${card(activityHistory.length, 'Samples this browser session')}`);
    renderDetail(`<div class="view-note">History is collected in this browser tab and is not written to the radar server.</div>`);
    const x = activityHistory.map(item => new Date(item.time));
    const traces = [
      {x, y: activityHistory.map(item => item.detections), name: 'Detections', type: 'scatter', mode: 'lines', line: {color: colors[0], width: 2}, yaxis: 'y'},
      {x, y: activityHistory.map(item => item.tracks), name: 'Persistent tracks', type: 'scatter', mode: 'lines', line: {color: colors[1], width: 2}, yaxis: 'y'},
      {x, y: activityHistory.map(item => Number.isFinite(item.snr) ? item.snr : null), name: 'Strongest SNR', type: 'scatter', mode: 'lines', line: {color: colors[2], width: 2}, yaxis: 'y2'},
      {x, y: activityHistory.map(item => item.load), name: 'Processing load', type: 'scatter', mode: 'lines', line: {color: colors[3], width: 2}, yaxis: 'y3'}
    ];
    await Plotly.react('data', traces, {...plotBase, margin: {l: 72, r: 45, t: 34, b: 55}, xaxis: {gridcolor: '#47362e'}, yaxis: {title: 'Count', domain: [.7, 1], rangemode: 'tozero', gridcolor: '#47362e'}, yaxis2: {title: 'SNR (dB)', domain: [.35, .62], gridcolor: '#47362e'}, yaxis3: {title: 'Load (%)', domain: [0, .27], rangemode: 'tozero', gridcolor: '#47362e'}}, plotConfig);
    setState(`Collecting · ${timeLabel(timestamp)}`, 'good');
  }

  function siteOffset(origin, point) {
    const latitude = (finite(origin?.latitude) + finite(point?.latitude)) / 2 * Math.PI / 180;
    return {x: (point.longitude - origin.longitude) * 111.32 * Math.cos(latitude), y: (point.latitude - origin.latitude) * 110.574};
  }

  function ellipse(centerX, centerY, focusDistance, excess, heading) {
    const a = (focusDistance + excess) / 2;
    const c = focusDistance / 2;
    const b = Math.sqrt(Math.max(0, a * a - c * c));
    const x = [], y = [];
    for (let index = 0; index <= 180; index++) {
      const angle = index * Math.PI * 2 / 180;
      const along = a * Math.cos(angle);
      const across = b * Math.sin(angle);
      x.push(centerX + along * Math.cos(heading) - across * Math.sin(heading));
      y.push(centerY + along * Math.sin(heading) + across * Math.cos(heading));
    }
    return {x, y};
  }

  function localPathToCoordinates(origin, path) {
    const latitudeScale = 110.574;
    const longitudeScale = 111.32 * Math.cos(finite(origin.latitude) * Math.PI / 180);
    return {
      latitude: path.y.map(value => origin.latitude + value / latitudeScale),
      longitude: path.x.map(value => origin.longitude + value / longitudeScale)
    };
  }

  async function renderSite() {
    setupPlotScaffold('insight-content-wide');
    const config = await getConfig();
    const receiver = config?.location?.rx || {};
    const transmitter = config?.location?.tx || {};
    if ([receiver.latitude, receiver.longitude, transmitter.latitude, transmitter.longitude].some(value => finite(value) === null)) {
      renderHtml(document.getElementById('view-summary'), card('Unavailable', 'Site coordinates'));
      renderDetail(empty('Site geometry is unavailable', 'Configure receiver and illuminator latitude and longitude in Settings.'));
      await emptyPlot('Receiver and illuminator coordinates are required.');
      setState('Configuration incomplete', 'warning');
      return;
    }
    const tx = siteOffset(receiver, transmitter);
    const baseline = Math.hypot(tx.x, tx.y);
    const heading = Math.atan2(tx.y, tx.x);
    const resolution = rangeResolutionKm(config);
    const delayBins = Math.max(0, finite(config?.process?.ambiguity?.delayMax) ?? 0);
    const maximumExcess = resolution === null ? 0 : delayBins * resolution;
    const contourValues = [maximumExcess * .25, maximumExcess * .5, maximumExcess].filter(value => value > .01);
    const traces = contourValues.map((value, index) => {
      const path = ellipse(tx.x / 2, tx.y / 2, baseline, value, heading);
      return {x: path.x, y: path.y, type: 'scatter', mode: 'lines', name: `${number(value, 1)} km bistatic range`, line: {color: colors[(index + 2) % colors.length], width: 1, dash: 'dot'}, hoverinfo: 'name'};
    });
    traces.push({x: [0, tx.x], y: [0, tx.y], type: 'scatter', mode: 'lines+markers+text', name: 'Sites', text: [receiver.name || 'Receiver', transmitter.name || 'Illuminator'], textposition: ['bottom center', 'top center'], marker: {size: [13, 13], color: [colors[0], colors[1]], symbol: ['circle', 'diamond']}, line: {color: '#ac9e94', width: 2}});
    renderHtml(document.getElementById('view-summary'), `${card(`${number(baseline, 2)} km`, 'Site baseline')}${card(resolution === null ? '—' : `${number(resolution, 3)} km`, 'Range-bin resolution')}${card(frequencyLabel(config), 'Carrier frequency')}`);
    renderDetail(`<section class="info-card"><h2>Configured sites</h2><dl><div><dt>Receiver</dt><dd>${escapeHtml(receiver.name || 'Receiver')} · ${number(receiver.latitude, 5)}, ${number(receiver.longitude, 5)} · ${number(receiver.altitude, 0)} m</dd></div><div><dt>Illuminator</dt><dd>${escapeHtml(transmitter.name || 'Illuminator')} · ${number(transmitter.latitude, 5)}, ${number(transmitter.longitude, 5)} · ${number(transmitter.altitude, 0)} m</dd></div><div><dt>Contours</dt><dd>Constant excess-path range derived from the configured delay window</dd></div></dl></section>`);
    await Plotly.react('data', traces, {...plotBase, xaxis: {title: 'East / west from receiver (km)', gridcolor: '#47362e', zerolinecolor: '#47362e'}, yaxis: {title: 'North / south from receiver (km)', gridcolor: '#47362e', zerolinecolor: '#47362e', scaleanchor: 'x', scaleratio: 1}}, plotConfig);
    setState('From active configuration', 'good');
  }

  let locationMapViewport = null;
  let locationMapDragging = false;
  let locationMapEngaged = false;
  let locationMapHoldUntil = 0;
  let locationPlotSignature = null;
  let selectedAircraftId = null;
  let selectedAircraftSnapshot = null;
  let autoSelectRadarAircraft = false;
  let showRadarTracksOnMap = false;
  let showRadarCoverageOnMap = true;
  let radarCoverageAltitudeBand = 'all';
  let radarCoverageCache = null;
  try { autoSelectRadarAircraft = localStorage.getItem('vectorwarp-auto-select-radar-aircraft') === 'true'; }
  catch (_) { /* Persistence is optional. */ }
  try { showRadarTracksOnMap = localStorage.getItem('vectorwarp-show-radar-tracks-on-map') === 'true'; }
  catch (_) { /* Persistence is optional. */ }
  try {
    const stored = localStorage.getItem('vectorwarp-show-radar-coverage-on-map');
    showRadarCoverageOnMap = stored === null ? true : stored === 'true';
    radarCoverageAltitudeBand = localStorage.getItem('vectorwarp-radar-coverage-altitude') || 'all';
  } catch (_) { /* Persistence is optional. */ }
  const aircraftTrackHistories = new Map();
  try {
    const saved = JSON.parse(localStorage.getItem('vectorwarp-aircraft-trails-v1') || '{}');
    const cutoff = Date.now() - 15 * 60 * 1000;
    for (const [id, points] of Object.entries(saved)) {
      const recent = Array.isArray(points) ? points.filter(point => Number(point.at) >= cutoff &&
        Number.isFinite(Number(point.lat)) && Number.isFinite(Number(point.lon))).slice(-180) : [];
      if (recent.length) aircraftTrackHistories.set(id, recent);
    }
  } catch (_) { /* Trail continuity is an optional browser cache. */ }
  let aircraftTrailPersistAt = 0;
  const aircraftPhotoCache = new Map();
  const aircraftPhotoInFlight = new Map();
  const aircraftPhotoQueue = [];
  const queuedAircraftPhotos = new Set();
  let aircraftPhotoPumpActive = false;

  async function radarCoverage() {
    if (!showRadarCoverageOnMap) return null;
    const key = radarCoverageAltitudeBand;
    const now = Date.now();
    if (radarCoverageCache?.key === key && now - radarCoverageCache.at < 30000)
      return radarCoverageCache.value;
    if (radarCoverageCache?.key === key && radarCoverageCache.promise)
      return radarCoverageCache.promise;
    const entry = {key, at: now, value: radarCoverageCache?.key === key ? radarCoverageCache.value : null};
    entry.promise = apiJson(`/api/radar/coverage?hours=24&bins=72&altitude=${encodeURIComponent(key)}`)
      .then(value => { entry.value = value; entry.at = Date.now(); return value; })
      .finally(() => { entry.promise = null; });
    radarCoverageCache = entry;
    return entry.promise;
  }

  function aircraftValue(value, suffix = '') {
    return value === null || value === undefined || value === '' ? '—' : `${escapeHtml(value)}${suffix}`;
  }

  function selectedAircraftMarkup(item, photo = null) {
    if (!item) return `<section class="info-card aircraft-detail-card aircraft-detail-empty"><h2>Aircraft details</h2><div class="aircraft-detail-prompt">${aircraftSvgMarkup({})}<p>Tap an aircraft on the map to inspect its live ADS-B and radar geometry.</p></div></section>`;
    const info = aircraftInfo(item);
    const photoMarkup = photo?.available ?
      `<figure class="aircraft-photo"><img src="${escapeHtml(photo.thumbnail)}" alt="${escapeHtml(info.label)} aircraft" loading="lazy" referrerpolicy="no-referrer"><figcaption>Photo by ${escapeHtml(photo.photographer || 'contributor')} · <a href="${escapeHtml(photo.link)}" target="_blank" rel="noopener noreferrer">Planespotters.net</a></figcaption></figure>` :
      `<div class="aircraft-photo aircraft-photo-fallback">${aircraftSvgMarkup(item)}<span>${photo?.loading ? 'Looking for a photo…' : photo?.enabled === false ? 'Aircraft photos are off in ADS-B settings' : 'No photo available'}</span></div>`;
    return `<section class="info-card aircraft-detail-card"><header><div><span class="aircraft-detail-kind">${escapeHtml(humanize(aircraftKind(item)))}</span><h2>${escapeHtml(info.label)}</h2><span class="aircraft-detail-subtitle">${aircraftValue(info.registration)} · ${aircraftValue(info.type)}</span></div><button type="button" class="button-secondary aircraft-detail-close" aria-label="Close aircraft details">Close</button></header><div class="aircraft-primary-metrics">
      <div><strong>${info.altitude === null ? '—' : integer(info.altitude)}</strong><span>ft altitude</span></div><div><strong>${info.speed === null ? '—' : integer(info.speed)}</strong><span>kt speed</span></div><div><strong>${info.heading === null ? '—' : `${integer(info.heading)}°`}</strong><span>heading</span></div><div><strong>${info.verticalRate === null ? '—' : integer(info.verticalRate)}</strong><span>ft/min</span></div>
    </div>${photoMarkup}<details class="aircraft-detail-section"><summary>Aircraft and source</summary><dl>
      <div><dt>Description</dt><dd>${aircraftValue(info.description)}</dd></div><div><dt>ICAO / category</dt><dd>${aircraftValue(info.hex)} · ${aircraftValue(info.category)}</dd></div>
      <div><dt>Squawk / signal</dt><dd>${aircraftValue(info.squawk)} · ${info.signal === null ? '—' : `${number(info.signal, 1)} dBFS`}</dd></div><div><dt>Position</dt><dd>${aircraftValue(info.positionSource)}${info.provider ? ` · ${escapeHtml(info.provider)}` : ''} · ${info.age === null ? '—' : `${number(info.age, 1)} s old`}</dd></div>
      <div><dt>Corroborated by</dt><dd>${info.corroboratedProviders.length ? info.corroboratedProviders.map(escapeHtml).join(', ') : 'No second fresh source'}</dd></div>
    </dl></details><details class="aircraft-detail-section"><summary>Radar geometry</summary><dl>
      <div><dt>Coordinates</dt><dd>${number(item.lat, 4)}, ${number(item.lon, 4)}</dd></div><div><dt>Excess path / Doppler</dt><dd>${info.delay === null ? '—' : `${number(info.delay, 2)} km`} · ${info.doppler === null ? '—' : `${number(info.doppler, 1)} Hz`}</dd></div>
      <div><dt>Radar association</dt><dd>${item.radar_track_id ? `Track ${escapeHtml(item.radar_track_id)} · ${escapeHtml(humanize(item.radar_track_state))}` : 'No confirmed match'}</dd></div>
    </dl></details></section>`;
  }

  function loadAircraftPhoto(hex) {
    if (!hex) return Promise.resolve(null);
    if (aircraftPhotoCache.has(hex) && !aircraftPhotoCache.get(hex)?.loading)
      return Promise.resolve(aircraftPhotoCache.get(hex));
    if (aircraftPhotoInFlight.has(hex)) return aircraftPhotoInFlight.get(hex);
    aircraftPhotoCache.set(hex, {loading: true});
    const request = fetchJson(liveApiUrl(`/api/adsb/aircraft-photo/${encodeURIComponent(hex)}`))
      .then(value => value || {available: false}, () => ({available: false}))
      .then(value => { aircraftPhotoCache.set(hex, value); return value; })
      .finally(() => aircraftPhotoInFlight.delete(hex));
    aircraftPhotoInFlight.set(hex, request);
    return request;
  }

  function prefetchAircraftPhotos(aircraft = []) {
    aircraft.map(item => aircraftInfo(item).hex.toLowerCase())
      .filter(hex => /^[0-9a-f]{6}$/.test(hex) && !aircraftPhotoCache.has(hex) &&
        !queuedAircraftPhotos.has(hex)).slice(0, 24).forEach(hex => {
        queuedAircraftPhotos.add(hex); aircraftPhotoQueue.push(hex);
      });
    if (aircraftPhotoPumpActive || !aircraftPhotoQueue.length) return;
    const pump = async () => {
      const hex = aircraftPhotoQueue.shift();
      if (!hex) { aircraftPhotoPumpActive = false; return; }
      aircraftPhotoPumpActive = true;
      await loadAircraftPhoto(hex);
      queuedAircraftPhotos.delete(hex);
      setTimeout(pump, 250);
    };
    const run = () => pump().catch(() => { aircraftPhotoPumpActive = false; });
    if (window.requestIdleCallback) window.requestIdleCallback(run, {timeout: 1500});
    else setTimeout(run, 0);
  }

  function renderSelectedAircraftCard(aircraft = []) {
    const target = document.getElementById('selected-aircraft-detail');
    if (!target) return;
    const current = aircraft.find(item => {
      const info = aircraftInfo(item);
      return (info.hex || info.label) === selectedAircraftId;
    });
    if (current) selectedAircraftSnapshot = current;
    const item = current || (selectedAircraftId ? selectedAircraftSnapshot : null);
    target.closest?.('.location-map-sidebar')?.classList.toggle('has-aircraft-selection', Boolean(item));
    const sidebarLabel = document.getElementById('map-sidebar-selected');
    if (sidebarLabel) sidebarLabel.textContent = item ? aircraftInfo(item).label : 'Layers & aircraft';
    const hex = item ? aircraftInfo(item).hex.toLowerCase() : '';
    renderHtml(target, selectedAircraftMarkup(item, hex ? aircraftPhotoCache.get(hex) : null));
    target.querySelector?.('.aircraft-detail-close')?.addEventListener('click', () => {
      selectedAircraftId = null; selectedAircraftSnapshot = null;
      renderSelectedAircraftCard(aircraft);
      document.querySelectorAll?.('.location-aircraft-marker.selected').forEach(marker =>
        marker.classList.remove('selected'));
      document.querySelector?.('.location-aircraft-layer')?._updatePositions?.();
    });
    if (!hex || (aircraftPhotoCache.has(hex) && !aircraftPhotoCache.get(hex)?.loading)) return;
    renderHtml(target, selectedAircraftMarkup(item, {loading: true}));
    loadAircraftPhoto(hex).finally(() => {
        if (selectedAircraftId === (aircraftInfo(item).hex || aircraftInfo(item).label))
          renderSelectedAircraftCard(aircraft);
    });
  }

  function bindLocationMapInteraction() {
    const map = document.getElementById('data');
    if (!map?.addEventListener || !window.addEventListener || map.dataset.locationInputBound) return;
    map.dataset.locationInputBound = 'true';
    // Plotly.react can reconcile the Mapbox scene whenever live track traces
    // change.  Do not do that in the middle of an operator's map session: on
    // desktop, one click should keep pan/zoom active until the pointer leaves
    // the map.  Touch gestures retain the shorter release grace because they
    // do not reliably produce pointerleave on every browser.
    map.addEventListener('pointerdown', () => {
      locationMapDragging = true;
      locationMapEngaged = true;
    });
    map.addEventListener('pointerleave', event => {
      if (event.pointerType !== 'touch') locationMapEngaged = false;
    });
    // Touch screens have no durable hover/leave lifecycle. Keep the map
    // engaged across successive pan/pinch gestures and release it only when
    // the operator taps elsewhere on the page.
    document.addEventListener?.('pointerdown', event => {
      if (event.pointerType === 'touch' && !map.contains?.(event.target))
        locationMapEngaged = false;
    }, true);
    const release = () => {
      locationMapDragging = false;
      locationMapHoldUntil = Date.now() + 900;
    };
    window.addEventListener('pointerup', release);
    window.addEventListener('pointercancel', release);
    map.addEventListener('wheel', () => {
      locationMapHoldUntil = Date.now() + 1200;
    }, {passive: true});
  }

  function locationMapIsBeingNavigated() {
    return locationMapEngaged || locationMapDragging || Date.now() < locationMapHoldUntil;
  }

  function renderAircraftOverlays(aircraft, sites = [], radarTracks = [], coverage = null) {
    const schedule = window.requestAnimationFrame || (callback => callback());
    schedule(() => {
      const graph = document.getElementById('data');
      const map = graph?._fullLayout?.mapbox?._subplot?.map;
      const container = map?.getContainer?.();
      if (!container || !document.createElement) return;
      let layer = container.querySelector?.('.location-aircraft-layer');
      if (!layer) {
        layer = document.createElement('div');
        layer.className = 'location-aircraft-layer';
        Object.assign(layer.style, {position: 'absolute', inset: '0', zIndex: '8',
          pointerEvents: 'none', overflow: 'hidden'});
        container.appendChild(layer);
        if (document.createElementNS) {
          const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
          svg.setAttribute('class', 'location-aircraft-trails');
          svg.setAttribute('aria-hidden', 'true');
          const path = className => {
            const value = document.createElementNS('http://www.w3.org/2000/svg', 'path');
            value.setAttribute('class', className);
            svg.appendChild(value);
            return value;
          };
          layer._radarTrackGroup = document.createElementNS('http://www.w3.org/2000/svg', 'g');
          layer._radarTrackGroup.setAttribute('class', 'location-radar-tracks');
          layer._coveragePath = document.createElementNS('http://www.w3.org/2000/svg', 'path');
          layer._coveragePath.setAttribute('class', 'location-radar-coverage-path');
          svg.appendChild(layer._coveragePath);
          svg.appendChild(layer._radarTrackGroup);
          layer._groundTrail = path('location-aircraft-ground-trail');
          layer._flightTrail = document.createElementNS('http://www.w3.org/2000/svg', 'g');
          layer._flightTrail.setAttribute('class', 'location-aircraft-flight-trail');
          svg.appendChild(layer._flightTrail);
          layer._altitudeConnector = path('location-aircraft-altitude-connector');
          layer.appendChild(svg);
        }
        const modelCanvas = document.createElement('canvas');
        modelCanvas.className = 'location-3d-model-layer';
        modelCanvas.setAttribute('aria-hidden', 'true');
        layer.appendChild(modelCanvas);
        layer._modelCanvas = modelCanvas;
        layer._modelRenderer = window.VectorWarp3D?.createRenderer?.(modelCanvas) || null;
        // Mobile Safari can dispatch many map transform events inside one
        // display frame during pinch zoom.  Mutating dozens of SVG trail legs
        // for each event leaves stale compositor tiles behind.  Coalesce all
        // transforms into the map's next animation frame instead.
        layer._schedulePositionUpdate = () => {
          if (layer._positionFrame) return;
          layer._positionFrame = schedule(() => {
            layer._positionFrame = 0;
            layer._updatePositions?.();
          });
        };
        map.on('move', layer._schedulePositionUpdate);
        map.on('resize', layer._schedulePositionUpdate);
        const endMapTransform = () => layer._schedulePositionUpdate();
        ['moveend', 'zoomend', 'rotateend', 'pitchend'].forEach(name =>
          map.on(name, endMapTransform));
      }
      layer._aircraftMarkers ||= new Map();
      layer._siteMarkers ||= new Map();
      layer._radarTrackPaths ||= new Map();
      layer._aircraftSnapshot = aircraft;
      layer._radarCoverage = coverage;
      const seenRadarTracks = new Set();
      for (const track of radarTracks) {
        const id = String(track.id);
        seenRadarTracks.add(id);
        let record = layer._radarTrackPaths.get(id);
        if (!record) {
          const group = document.createElementNS('http://www.w3.org/2000/svg', 'g');
          group.setAttribute('class', 'location-radar-track-cage');
          const makePath = className => {
            const value = document.createElementNS('http://www.w3.org/2000/svg', 'path');
            value.setAttribute('class', className); group.appendChild(value); return value;
          };
          const path = makePath('location-radar-track-path location-radar-track-ground');
          // Delay-only radar tracks do not measure altitude. The cage spans
          // the practical civil-aircraft envelope so high traffic does not
          // appear above it; an ADS-B match still gets its own exact slice.
          const slices = [10000, 20000, 30000, 40000, 60000].map(() =>
            makePath('location-radar-track-slice'));
          const ribs = makePath('location-radar-track-ribs');
          const associated = makePath('location-radar-track-associated');
          layer._radarTrackGroup?.appendChild(group);
          record = {group, path, slices, ribs, associated, item: track};
          layer._radarTrackPaths.set(id, record);
        }
        record.item = track;
      }
      for (const [id, record] of layer._radarTrackPaths)
        if (!seenRadarTracks.has(id)) { record.group.remove(); layer._radarTrackPaths.delete(id); }
      const seenAircraft = new Set();
      for (const item of aircraft) {
        const info = aircraftInfo(item);
        const heading = info.heading;
        const aircraftId = info.hex || `${info.label}:${item.lat}:${item.lon}`;
        seenAircraft.add(aircraftId);
        let record = layer._aircraftMarkers.get(aircraftId);
        if (!record) {
          const marker = document.createElement('div');
          marker.className = 'location-aircraft-marker';
          Object.assign(marker.style, {position: 'absolute', width: '42px', height: '52px',
            transform: 'translate(-50%, -50%)', pointerEvents: 'none'});
          marker.tabIndex = -1;
          marker.innerHTML = '<span class="location-aircraft-icon"></span><span class="location-aircraft-label"></span>';
          record = {marker, icon: marker.children[0], label: marker.children[1],
            item, heading, signature: '', isAircraft: true};
          record.icon.tabIndex = 0;
          record.icon.setAttribute('role', 'button');
          const selectAircraft = event => {
            if (event.target?.closest?.('a')) return;
            event.stopPropagation?.();
            const currentInfo = aircraftInfo(record.item);
            const currentId = currentInfo.hex || `${currentInfo.label}:${record.item.lat}:${record.item.lon}`;
            selectedAircraftId = selectedAircraftId === currentId ? null : currentId;
            if (selectedAircraftId) selectedAircraftSnapshot = record.item;
            layer.querySelectorAll?.('.location-aircraft-marker').forEach(candidate =>
              candidate.classList?.toggle('selected', candidate === marker && selectedAircraftId === currentId));
            const sidebar = document.querySelector?.('.location-map-sidebar');
            if (selectedAircraftId && sidebar) sidebar.open = true;
            renderSelectedAircraftCard(layer._aircraftSnapshot || []);
            layer._updatePositions?.();
          };
          record.icon.addEventListener?.('click', selectAircraft);
          record.icon.addEventListener?.('keydown', event => {
            if (event.key === 'Enter' || event.key === ' ') { event.preventDefault(); selectAircraft(event); }
          });
          layer.appendChild(marker);
          layer._aircraftMarkers.set(aircraftId, record);
        }
        record.item = item;
        record.heading = heading;
        record.flightPathAngle = aircraftFlightPathAngle(info.verticalRate, info.speed);
        const marker = record.marker;
        const color = aircraftAltitudeColor(item);
        const radarTracked = Boolean(item.radar_track_id);
        marker.classList?.toggle('selected', selectedAircraftId === aircraftId);
        marker.classList?.toggle('snapshot-stale', item._snapshot_stale === true);
        marker.classList?.toggle('mlat-position', info.positionSource === 'MLAT');
        marker.classList?.toggle('radar-tracked', radarTracked);
        marker.classList?.toggle('radar-active', radarTracked &&
          ['ACTIVE', 'CONFIRMED'].includes(String(item.radar_track_state).toUpperCase()));
        const signature = [aircraftKind(item), color, heading, info.label,
          info.positionSource, radarTracked].join('|');
        if (signature !== record.signature) {
          record.icon.replaceChildren();
          record.label.textContent = info.label;
          record.label.style.borderColor = color;
          record.icon.setAttribute('aria-label', `${info.label}. Select for aircraft details.`);
          record.signature = signature;
        }
      }
      for (const [id, record] of layer._aircraftMarkers)
        if (!seenAircraft.has(id)) { record.marker.remove(); layer._aircraftMarkers.delete(id); }
      // Plotly's raster OSM style has no glyph source. Keep site labels in
      // the existing HTML overlay instead of requesting unsupported symbols.
      const seenSites = new Set();
      for (const site of sites) {
        const id = `${site.name}:${site.latitude}:${site.longitude}`;
        seenSites.add(id);
        let record = layer._siteMarkers.get(id);
        if (!record) {
          const marker = document.createElement('div');
          marker.className = 'location-site-label';
          marker.innerHTML = '<span class="location-site-icon"></span><span class="location-site-name"></span>';
          layer.appendChild(marker);
          record = {marker, item: null, isAircraft: false};
          layer._siteMarkers.set(id, record);
        }
        record.marker.querySelector('.location-site-icon').replaceChildren();
        record.marker.querySelector('.location-site-name').textContent = site.name;
        record.marker.classList.toggle('receiver-site', site.kind !== 'transmitter');
        record.marker.classList.toggle('transmitter-site', site.kind === 'transmitter');
        record.item = {lat: site.latitude, lon: site.longitude, kind: site.kind};
      }
      for (const [id, record] of layer._siteMarkers)
        if (!seenSites.has(id)) { record.marker.remove(); layer._siteMarkers.delete(id); }
      const altitudeLiftFor = item => {
        const pitch = finite(map.getPitch?.()) ?? 0;
        if (pitch <= 0) return 0;
        const altitudeFeet = Number.isFinite(item.altitude) ? item.altitude : aircraftInfo(item).altitude;
        const groundMeters = finite(sites[0]?.altitude) ?? 0;
        const heightMeters = altitudeFeet === null ? 0 : Math.max(0, altitudeFeet * .3048 - groundMeters);
        const zoom = finite(map.getZoom?.()) ?? 0;
        const metersPerPixel = Math.cos(Number(item.lat) * Math.PI / 180) * 2 * Math.PI * 6378137 /
          (512 * Math.pow(2, zoom));
        return Math.min(240, heightMeters / Math.max(.01, metersPerPixel) *
          Math.sin(pitch * Math.PI / 180));
      };
      layer._updatePositions = () => {
        const line = points => points.map((point, index) =>
          `${index ? 'L' : 'M'}${point.x.toFixed(1)} ${point.y.toFixed(1)}`).join(' ');
        const coverageRows = Array.isArray(layer._radarCoverage?.points) ?
          layer._radarCoverage.points.map(point => ({bin: Number(point.bin),
            point: map.project([point.longitude, point.latitude])}))
            .filter(row => Number.isInteger(row.bin)).sort((left, right) => left.bin - right.bin) : [];
        const coverageRuns = [];
        for (const row of coverageRows) {
          const run = coverageRuns.at(-1);
          if (run && row.bin === run.at(-1).bin + 1) run.push(row);
          else coverageRuns.push([row]);
        }
        const binCount = Math.max(1, Number(layer._radarCoverage?.bins) || 72);
        if (coverageRuns.length > 1 && coverageRuns[0][0]?.bin === 0 &&
            coverageRuns.at(-1).at(-1)?.bin === binCount - 1) {
          const wrapped = coverageRuns.pop().concat(coverageRuns.shift());
          coverageRuns.unshift(wrapped);
        }
        const receiverPoint = sites[0] ? map.project([sites[0].longitude, sites[0].latitude]) : null;
        // Never close or smooth across unsupported bearings. Each contiguous
        // run becomes its own observed sector; an isolated bin is an honest
        // radial observation rather than a fictional omnidirectional limit.
        const coveragePath = receiverPoint ? coverageRuns.map(run => {
          const radial = `M${receiverPoint.x.toFixed(1)} ${receiverPoint.y.toFixed(1)}` +
            run.map(row => `L${row.point.x.toFixed(1)} ${row.point.y.toFixed(1)}`).join('');
          return run.length > 1 ? `${radial}Z` : radial;
        }).join('') : '';
        layer._coveragePath?.setAttribute('d', coveragePath);
        for (const {item, path: radarPath, slices, ribs, associated} of layer._radarTrackPaths.values()) {
          const ground = item.latitude.map((latitude, index) => ({latitude,
            point: map.project([item.longitude[index], latitude])}));
          radarPath.setAttribute('d', line(ground.map(value => value.point)));
          const sliceAltitudes = [10000, 20000, 30000, 40000, 60000];
          const lifted = altitude => ground.map(value => ({x: value.point.x,
            y: value.point.y - altitudeLiftFor({lat: value.latitude, altitude})}));
          slices.forEach((slice, index) => slice.setAttribute('d', line(lifted(sliceAltitudes[index]))));
          const top = lifted(sliceAltitudes.at(-1));
          const ribIndices = [0, .25, .5, .75].map(ratio =>
            Math.min(ground.length - 1, Math.round((ground.length - 1) * ratio)));
          ribs.setAttribute('d', ribIndices.map(index =>
            `M${ground[index].point.x.toFixed(1)} ${ground[index].point.y.toFixed(1)}` +
            `L${top[index].x.toFixed(1)} ${top[index].y.toFixed(1)}`).join(''));
          const associatedAltitude = finite(item.associatedAltitude);
          if (associatedAltitude === null) associated.setAttribute('d', '');
          else {
            associated.setAttribute('d', line(lifted(associatedAltitude)));
            associated.setAttribute('stroke', item.associatedColor || '#5be7ff');
          }
        }
        const modelInstances = [];
        const aircraftLabelCandidates = [];
        const mapWidth = container.clientWidth || 0;
        const mapHeight = container.clientHeight || 0;
        const mapZoom = finite(map.getZoom?.()) ?? 0;
        [...layer._aircraftMarkers.values(), ...layer._siteMarkers.values()].forEach(
          ({item, marker, icon, heading, flightPathAngle, isAircraft}) => {
        const point = map.project([Number(item.lon), Number(item.lat)]);
        const altitudeLift = isAircraft ? altitudeLiftFor(item) : 0;
        marker.style.left = `${point.x}px`;
        marker.style.top = `${point.y - altitudeLift}px`;
        if (isAircraft && icon) {
          const aircraftId = aircraftInfo(item).hex || aircraftInfo(item).label;
          aircraftLabelCandidates.push({marker, item, x: point.x, y: point.y - altitudeLift,
            selected: selectedAircraftId === aircraftId,
            radarTracked: Boolean(item.radar_track_id)});
          const rotation = aircraftScreenRotation(heading, map.getBearing?.());
          const yaw = rotation === null ? 0 : rotation;
          const pitch = Number.isFinite(flightPathAngle) ? flightPathAngle : 0;
          icon.dataset.flightPath = pitch > .5 ? 'climbing' : pitch < -.5 ? 'descending' : 'level';
          const modelSize = mapZoom < 6 ? 11 : mapZoom < 7.5 ? 13.5 : 16.5;
          modelInstances.push({model: aircraftKind(item), x: point.x, y: point.y - altitudeLift,
            yaw, pitch, size: modelSize, color: aircraftAltitudeColor(item)});
        } else {
          modelInstances.push({model: item.kind === 'transmitter' ? 'transmitter' : 'receiver',
            x: point.x, y: point.y - 23, yaw: -(finite(map.getBearing?.()) ?? 0), pitch: 0,
            size: item.kind === 'transmitter' ? 15 : 14, color: item.kind === 'transmitter' ? '#ff8754' : '#8fe7df'});
        }
        });
        // Aircraft labels are HTML overlays rather than Mapbox symbols, so
        // Mapbox cannot perform collision avoidance for them.  Apply a stable
        // screen-space declutter pass on every camera update.  Selected and
        // radar-associated aircraft win, then fresh positions nearest the map
        // centre.  Icons remain visible; only overlapping labels are hidden.
        const occupied = [];
        for (const {item} of layer._siteMarkers.values()) {
          const point = map.project([Number(item.lon), Number(item.lat)]);
          occupied.push(item.kind === 'transmitter' ?
            {left: point.x + 18, right: point.x + 164, top: point.y - 54, bottom: point.y - 18} :
            {left: point.x - 164, right: point.x - 18, top: point.y + 8, bottom: point.y + 38});
        }
        const mobile = mapWidth <= 760;
        const labelLimit = mapZoom < 6 ? (mobile ? 12 : 28) :
          mapZoom < 7.5 ? (mobile ? 22 : 48) : (mobile ? 36 : 90);
        const overlaps = box => occupied.some(other => !(box.right + 4 <= other.left ||
          box.left >= other.right + 4 || box.bottom + 3 <= other.top || box.top >= other.bottom + 3));
        aircraftLabelCandidates.sort((left, right) =>
          Number(right.selected) - Number(left.selected) ||
          Number(right.radarTracked) - Number(left.radarTracked) ||
          Number(left.item?._snapshot_stale === true) - Number(right.item?._snapshot_stale === true) ||
          Math.hypot(left.x - mapWidth / 2, left.y - mapHeight / 2) -
            Math.hypot(right.x - mapWidth / 2, right.y - mapHeight / 2));
        let visibleLabels = 0;
        for (const candidate of aircraftLabelCandidates) {
          const text = candidate.marker.querySelector?.('.location-aircraft-label')?.textContent || '';
          const width = Math.min(86, Math.max(34, text.length * 6.3 + 10));
          const box = {left: candidate.x - width / 2, right: candidate.x + width / 2,
            top: candidate.y + 8, bottom: candidate.y + 24};
          const onScreen = box.right >= 0 && box.left <= mapWidth && box.bottom >= 0 && box.top <= mapHeight;
          const visible = onScreen && visibleLabels < labelLimit && !overlaps(box);
          candidate.marker.classList.toggle('label-hidden', !visible);
          if (visible) { occupied.push(box); visibleLabels++; }
        }
        layer._modelRenderer?.render(modelInstances);
        const history = selectedAircraftId ? aircraftTrackHistories.get(selectedAircraftId) || [] : [];
        const points = history.map(point => {
          const projected = map.project([point.lon, point.lat]);
          return {x: projected.x, groundY: projected.y,
            flightY: projected.y - altitudeLiftFor(point), altitude: point.altitude};
        });
        const path = (values, y) => values.map((point, index) =>
          `${index ? 'L' : 'M'}${point.x.toFixed(1)} ${point[y].toFixed(1)}`).join(' ');
        layer._groundTrail?.setAttribute('d', points.length > 1 ? path(points, 'groundY') : '');
        if (layer._flightTrail) {
          // A path cannot vary its stroke color along an arbitrary rise and
          // descent. Reuse one short path per recorded leg instead. Frequent
          // position samples plus the continuous palette make climbs and
          // descents read as a smooth altitude gradient without canvas work or
          // extra data transfer. Reusing nodes keeps CPI-paced mobile updates
          // cheap even at the full 180-point history limit.
          const legs = Math.max(0, points.length - 1);
          while (layer._flightTrail.children.length < legs) {
            const segment = document.createElementNS('http://www.w3.org/2000/svg', 'path');
            layer._flightTrail.appendChild(segment);
          }
          while (layer._flightTrail.children.length > legs)
            layer._flightTrail.lastElementChild.remove();
          for (let index = 0; index < legs; index++) {
            const from = points[index], to = points[index + 1];
            const altitudeValues = [from.altitude, to.altitude].filter(Number.isFinite);
            const altitude = altitudeValues.length ?
              altitudeValues.reduce((sum, value) => sum + value, 0) / altitudeValues.length : null;
            const segment = layer._flightTrail.children[index];
            segment.setAttribute('d', `M${from.x.toFixed(1)} ${from.flightY.toFixed(1)}L${to.x.toFixed(1)} ${to.flightY.toFixed(1)}`);
            segment.setAttribute('stroke', aircraftAltitudeColor({altitude}));
          }
        }
        const last = points[points.length - 1];
        layer._altitudeConnector?.setAttribute('stroke', aircraftAltitudeColor({altitude: last?.altitude}));
        layer._altitudeConnector?.setAttribute('d', last && Math.abs(last.groundY - last.flightY) > 1 ?
          `M${last.x.toFixed(1)} ${last.groundY.toFixed(1)}L${last.x.toFixed(1)} ${last.flightY.toFixed(1)}` : '');
      };
      layer._schedulePositionUpdate?.();
    });
  }

  async function renderLocations() {
    setupLocationMapScaffold();
    bindLocationMapInteraction();
    const [config, detection, tracker, coverage] = await Promise.all([
      getConfig(), apiJson('/api/detection'), apiJson('/api/tracker'),
      radarCoverage().catch(() => radarCoverageCache?.value || null)
    ]);
    const receiver = config?.location?.rx || {};
    const transmitter = config?.location?.tx || {};
    if ([receiver.latitude, receiver.longitude, transmitter.latitude, transmitter.longitude]
      .some(value => finite(value) === null)) {
      renderHtml(document.getElementById('view-summary'), card('Unavailable', 'Site coordinates'));
      renderDetail(empty('Map is unavailable', 'Configure receiver and illuminator coordinates in Settings.'));
      await emptyPlot('Receiver and illuminator coordinates are required.');
      setState('Configuration incomplete', 'warning');
      return;
    }

    const resolution = rangeResolutionKm(config) ?? .1;
    const rawDetections = detectionRows(detection).filter(item => item.delay !== null && item.delay > 0);
    // Operational map overlays must not present pre-confirmation ASSOCIATED
    // candidates as aircraft tracks. They remain visible on Tracks.
    const currentTrackStates = new Set(['ACTIVE', 'CONFIRMED']);
    const trackerRows = array(tracker?.data);
    const tracks = trackerRows.map(track => ({
      id: track.id,
      state: track.state,
      delay: trackRangeKm(track.delay, config),
      doppler: finite(track.doppler)
    })).filter(track => track.delay !== null && track.delay > 0 &&
      currentTrackStates.has(String(track.state || '').toUpperCase()));
    const hiddenCoastingTracks = trackerRows.filter(track =>
      String(track?.state || '').toUpperCase() === 'COASTING').length;
    const hiddenTentativeTracks = trackerRows.filter(track =>
      String(track?.state || '').toUpperCase() === 'TENTATIVE').length;
    const hiddenAssociatedTracks = trackerRows.filter(track =>
      String(track?.state || '').toUpperCase() === 'ASSOCIATED').length;
    let bearingLocations = null;
    if (config?.process?.bearing?.enable === true) {
      try { bearingLocations = await apiJson('/api/locations'); }
      catch (_) { bearingLocations = {enabled: true, available: false, data: []}; }
    }

    let aircraft = [];
    let aircraftError = null;
    if (config?.truth?.adsb?.enabled === true) {
      try {
        const [adsb, geometry] = await Promise.all([
          apiJson('/api/adsb'), apiJson('/api/adsb/delay-doppler?overlay=1')
        ]);
        aircraft = adsb?.disabledForReplay ? [] : array(adsb?.aircraft).filter(item =>
          finite(item.lat) !== null && finite(item.lon) !== null && (finite(item.seen_pos) ?? 0) < 120);
        const byHex = new Map(Object.entries(geometry || {}).map(([id, item]) =>
          [String(item?.hex || id || '').toUpperCase(), item]));
        aircraft = aircraft.map(item => ({...item,
          ...(byHex.get(String(item.hex || '').toUpperCase()) || {})}));
        aircraft = stabilizeAircraftSnapshots(aircraft, {scope: 'map', graceMs: 10000});
        const delayGate = finite(config?.process?.bearing?.manifold_recorder?.max_delay_error_km) ?? .25;
        const dopplerGate = finite(config?.process?.bearing?.manifold_recorder?.max_doppler_error_hz) ?? 5;
        const candidates = [];
        const associationTracks = showRadarTracksOnMap ? tracks : [];
        aircraft.forEach((item, aircraftIndex) => associationTracks.forEach(track => {
          const delayError = Math.abs((finite(item.delay) ?? Infinity) - track.delay);
          const dopplerError = Math.abs((finite(item.doppler) ?? Infinity) - track.doppler);
          if (delayError <= delayGate && dopplerError <= dopplerGate)
            candidates.push({aircraftIndex, track, score: delayError / delayGate + dopplerError / dopplerGate});
        }));
        const assignedAircraft = new Set(), assignedTracks = new Set();
        candidates.sort((left, right) => left.score - right.score).forEach(candidate => {
          if (assignedAircraft.has(candidate.aircraftIndex) || assignedTracks.has(candidate.track.id)) return;
          assignedAircraft.add(candidate.aircraftIndex); assignedTracks.add(candidate.track.id);
          aircraft[candidate.aircraftIndex] = {...aircraft[candidate.aircraftIndex],
            radar_track_id: candidate.track.id, radar_track_state: candidate.track.state};
        });
        updateAircraftTrackHistories(aircraftTrackHistories, aircraft);
        if (Date.now() - aircraftTrailPersistAt > 5000) {
          aircraftTrailPersistAt = Date.now();
          try {
            localStorage.setItem('vectorwarp-aircraft-trails-v1', JSON.stringify(
              Object.fromEntries([...aircraftTrackHistories].slice(-200))));
          } catch (_) { /* A full/private browser cache must not affect the map. */ }
        }
        prefetchAircraftPhotos(aircraft);
        selectedAircraftId = selectRadarAssociatedAircraft(selectedAircraftId, aircraft,
          autoSelectRadarAircraft && showRadarTracksOnMap);
        selectedAircraftSnapshot = aircraft.find(item => aircraftIdentity(item) === selectedAircraftId) ||
          selectedAircraftSnapshot;
      } catch (_) {
        aircraft = stabilizeAircraftSnapshots([], {scope: 'map', graceMs: 10000});
        aircraftError = 'ADS-B aircraft positions temporarily unavailable';
      }
    }

    const tx = siteOffset(receiver, transmitter);
    const baseline = Math.hypot(tx.x, tx.y);
    const heading = Math.atan2(tx.y, tx.x);
    const radarTrackOverlay = item => {
      const path = ellipse(tx.x / 2, tx.y / 2, baseline, item.delay, heading);
      const geographic = localPathToCoordinates(receiver, path);
      const associatedAircraft = aircraft.find(candidate =>
        String(candidate.radar_track_id ?? '') === String(item.id));
      const associatedAltitude = associatedAircraft ? aircraftInfo(associatedAircraft).altitude : null;
      return {id: item.id, state: item.state, delay: item.delay,
        latitude: geographic.latitude, longitude: geographic.longitude,
        associatedAltitude, associatedColor: associatedAircraft ?
          aircraftAltitudeColor(associatedAircraft) : null};
    };
    const showStationary = new URLSearchParams(window.location.search).get('showStationary') === '1';
    const stationaryIds = new Set(array(bearingLocations?.data).filter(item => item?.stationary_clutter).map(item => item.id));
    const visibleTracks = !showRadarTracksOnMap ? [] : showStationary ? tracks :
      tracks.filter(item => !stationaryIds.has(item.id));
    const radarTrackOverlays = visibleTracks.map(item => radarTrackOverlay(item));
    const traces = [];
    // The map is also a bearing diagnostic: show every tracker state and every
    // finite estimate. Resolution remains a separate strict signal used by
    // downstream consumers.
    const allBearingRows = array(bearingLocations?.data).filter(item => item &&
      (item.location || item.estimate || array(item.candidates).length));
    const hiddenStationary = showStationary ? 0 : allBearingRows.filter(item => item.publishable === false).length;
    const bearingRows = showStationary ? allBearingRows : allBearingRows.filter(item => item.publishable !== false);
    for (const item of bearingRows) {
      if (array(item.uncertainty_corridor).length > 1) traces.push({
        type: 'scattermapbox', mode: 'lines', name: `Bearing uncertainty ${item.id}`,
        lat: item.uncertainty_corridor.map(point => point.latitude),
        lon: item.uncertainty_corridor.map(point => point.longitude),
        line: {color: 'rgba(247,199,95,.75)', width: 7},
        hovertemplate: `Track ${escapeHtml(item.id)}<br>Bearing uncertainty ±${number(item.uncertainty_deg, 1)}°<extra></extra>`
      });
      if (item.location) traces.push({
        type: 'scattermapbox', mode: 'markers', name: `Bearing fix ${item.id}`,
        lat: [item.location.latitude], lon: [item.location.longitude],
        // Plotly's bundled open-street-map style has no sprite/glyph source;
        // use native circle layers so live updates do not fail in Mapbox GL.
        marker: {size: 14, color: '#f7c75f', opacity: .95},
        hovertemplate: `Track ${escapeHtml(item.id)}<br>${number(item.bearing_deg_true, 1)}° true<br>±${number(item.uncertainty_deg, 1)}°<br>${number(item.excess_path_km, 2)} km excess path<extra></extra>`
      });
      if (!item.location && item.estimate) traces.push({
        type: 'scattermapbox', mode: 'markers', name: `Unresolved estimate ${item.id}`,
        lat: [item.estimate.latitude], lon: [item.estimate.longitude],
        marker: {size: 12, color: '#b99cff', opacity: .58},
        hovertemplate: `Track ${escapeHtml(item.id)}<br>${escapeHtml(humanize(item.state || 'unknown'))}<br>${number(item.bearing_deg_true, 1)}° true · unresolved<br>±${number(item.uncertainty_deg, 1)}°<br>Peak separation ${number(item.peak_separation_db, 1)} dB<br>Temporal consistency ${number(item.temporal_consistency, 2)}<br>Diagnostic estimate only<extra></extra>`
      });
      if (!item.location && item.candidates?.length) traces.push({
        type: 'scattermapbox', mode: 'markers', name: `Bearing candidates ${item.id}`,
        lat: item.candidates.map(point => point.latitude), lon: item.candidates.map(point => point.longitude),
        marker: {size: 10, color: 'rgba(247,199,95,.55)', opacity: .65},
        hovertemplate: `Unresolved track ${escapeHtml(item.id)}<br>Candidate only<extra></extra>`
      });
    }
    traces.push({
      type: 'scattermapbox', mode: 'markers', name: 'Radar sites',
      lat: [receiver.latitude, transmitter.latitude], lon: [receiver.longitude, transmitter.longitude],
      text: [receiver.name || 'Receiver', transmitter.name || 'Illuminator'],
      marker: {size: [13, 13], color: ['#f3eee9', '#f7c75f']},
      hovertemplate: '%{text}<br>%{lat:.5f}, %{lon:.5f}<extra></extra>'
    });
    const configuredDelayBins = Math.max(0, finite(config?.process?.ambiguity?.delayMax) ?? 0);
    const configuredExcessKm = configuredDelayBins * resolution;
    const viewportKey = [receiver.latitude, receiver.longitude, transmitter.latitude,
      transmitter.longitude, configuredExcessKm].join(':');
    if (!locationMapViewport || locationMapViewport.key !== viewportKey) {
      // Online ADS-B radius describes feed acquisition, not radar reach. Frame
      // a fresh map from the durable 24-hour radar-confirmed envelope instead.
      // Do this only at initialization/config change so new tracks never yank
      // an operator's active pan or zoom.
      const coverageRadiusKm = Math.max(0, ...array(coverage?.points)
        .map(point => finite(point?.ground_range_km) ?? 0));
      const spanKm = Math.max(10, baseline + configuredExcessKm, coverageRadiusKm * 2);
      locationMapViewport = {
        key: viewportKey,
        center: {lat: (receiver.latitude + transmitter.latitude) / 2,
          lon: (receiver.longitude + transmitter.longitude) / 2},
        zoom: Math.max(4, Math.min(12, 13 - Math.log2(spanKm)))
      };
    }
    const resolvedBearings = bearingRows.filter(item => item.location).length;
    const unresolvedBearings = bearingRows.length - resolvedBearings;
    renderHtml(document.getElementById('view-summary'), `${card(tracks.length, 'Radar tracks')}${card(aircraft.length, 'Aircraft')}${card(rawDetections.length, 'Detections')}${showRadarCoverageOnMap ? card(coverage?.supporting_passes ?? 0, 'Coverage passes') : card(bearingRows.length, 'Bearing estimates')}`);
    renderDetail(`<details class="info-card location-map-notes"><summary>Map details</summary><div class="map-diagnostic-counts"><span><b>${bearingRows.length}</b> Bearing estimates</span><span><b>${resolvedBearings}</b> Resolved fixes</span><span><b>${unresolvedBearings}</b> Unresolved shown</span><span><b>${hiddenAssociatedTracks + hiddenTentativeTracks + hiddenCoastingTracks}</b> Hidden candidates</span></div><div class="view-note">${showRadarTracksOnMap ? `Orange 3D uncertainty cages show all ${visibleTracks.length} visible confirmed active delay-only tracks. A highlighted altitude slice means ADS-B/MLAT identified the tracked aircraft; the radar did not independently measure altitude.` : `Confirmed radar tracks are currently kept on the main radar display only; enable “Show radar tracks on this map” above to add their 3D uncertainty cages here.`} ${showRadarCoverageOnMap ? `The cyan 24-hour boundary contains only positions associated with confirmed radar tracks (${coverage?.supporting_passes ?? 0} supporting passes across ${coverage?.supported_bins ?? 0}/${coverage?.bins ?? 72} bearing bins); online ADS-B/MLAT reach cannot expand it.` : ''} Aircraft icons are colored by altitude; a cyan ring means the aligned ADS-B prediction was uniquely matched to a confirmed native radar track inside the configured delay/Doppler gates. Bright gold fixes and corridors passed the strict bearing confidence test. Purple markers are best unresolved estimates, and translucent gold points are their alternate lobes. ${hiddenAssociatedTracks} associated candidates, ${hiddenTentativeTracks} tentative rows, and ${hiddenCoastingTracks} coasting rows are hidden from operational cages. Inspect candidates on Tracks. ${hiddenStationary} persistent stationary bearing row${hiddenStationary === 1 ? ' is' : 's are'} hidden by default but remain in the raw API and can be inspected with <code>?showStationary=1</code>. ${rawDetections.length} raw untracked detection${rawDetections.length === 1 ? ' is' : 's are'} hidden because delay alone does not define a unique map position. Array wiring and known-angle RF diagnostics are under Array calibration.</div></details>`);
    renderSelectedAircraftCard(aircraft);
    renderAircraftOverlays(aircraft, [
      {...receiver, kind: 'receiver', name: receiver.name || 'Receiver'},
      {...transmitter, kind: 'transmitter', name: transmitter.name || 'Illuminator'}
    ], radarTrackOverlays, showRadarCoverageOnMap ? coverage : null);
    if (locationMapIsBeingNavigated()) {
      setState(aircraftError || (tracks.length ? `${tracks.length} current radar track${tracks.length === 1 ? '' : 's'}` : 'No current radar tracks'), aircraftError ? 'warning' : tracks.length ? 'good' : 'warning');
      return;
    }
    // ADS-B uses the keyed icon overlay below. Do not also send it through
    // Plotly: that duplicate path rebuilt the entire Mapbox scene every CPI.
    const plotSignature = JSON.stringify(traces.map(trace => [trace.name, trace.lat,
      trace.lon, trace.line, trace.marker]));
    if (plotSignature !== locationPlotSignature || !document.getElementById('data')?._fullLayout?.mapbox) {
      await Plotly.react('data', traces, {
        ...plotBase, margin: {l: 8, r: 8, t: 8, b: 8},
        uirevision: locationMapViewport.key,
        mapbox: {style: 'open-street-map', center: locationMapViewport.center,
          zoom: locationMapViewport.zoom, uirevision: locationMapViewport.key},
        legend: {orientation: 'h', x: 0, y: 1.03, bgcolor: 'rgba(20,16,14,.82)'}
      }, plotConfig);
      locationPlotSignature = plotSignature;
    }
    setState(aircraftError || (tracks.length ? `${tracks.length} current radar track${tracks.length === 1 ? '' : 's'}` : 'No current radar tracks'), aircraftError ? 'warning' : tracks.length ? 'good' : 'warning');
  }

  async function renderEvaluation() {
    setupPlotScaffold();
    const [config, detection] = await Promise.all([getConfig(), apiJson('/api/detection')]);
    const enabled = config?.truth?.adsb?.enabled === true;
    let feed = null;
    let feedError = null;
    if (enabled) {
      try { feed = await getAdsb(); } catch (error) { feedError = error; }
    }
    const replayTruthDisabled = feed?.disabledForReplay === true;
    let aircraft = feed && typeof feed === 'object' && !replayTruthDisabled ?
      Object.entries(feed).map(([id, item]) => ({id, ...item})) : [];
    aircraft = stabilizeAircraftSnapshots(aircraft,
      {scope: 'evaluation', graceMs: enabled && !replayTruthDisabled ? 10000 : 0});
    const radar = detectionRows(detection);
    const truth = config?.truth?.adsb || {};
    renderHtml(document.getElementById('view-summary'), `${card(replayTruthDisabled ? 'Not replayed' : enabled ? (feedError ? 'Offline' : 'Online') : 'Disabled', 'ADS-B', feedError ? 'bad' : enabled && !replayTruthDisabled ? 'good' : '')}${card(aircraft.length, 'Aircraft')}${card(radar.length, 'Radar detections')}${card(finite(truth.display_range_km) === null ? 'Source range' : `${number(truth.display_range_km, 0)} km`, 'Range')}`);
    const detail = document.getElementById('view-detail');
    if (!enabled) {
      renderHtml(detail, empty('ADS-B is disabled', 'Enable ADS-B in Settings to use this page.', '<a class="button-link" href="/display/configuration">Open Settings</a>'));
      await emptyPlot('ADS-B is disabled.');
      setState('Disabled', 'warning');
      return;
    }
    if (replayTruthDisabled) {
      renderHtml(detail, empty('ADS-B is not replayed', 'Live ADS-B overlays are hidden during replay.'));
      await emptyPlot('Live ADS-B is hidden during replay.');
      setState('Not replayed', 'warning');
      return;
    }
    if (feedError) {
      renderHtml(detail, empty('ADS-B is unavailable', 'Radar continues without it.'));
      await emptyPlot('ADS-B feed unavailable; radar detections are not affected.');
      setState('ADS-B offline', 'bad');
      return;
    }
    const aircraftRows = aircraft.map(item => {
      const age = sourceAgeSeconds(item.timestamp);
      return `<tr><td>${escapeHtml(item.flight || item.callsign || item.hex || item.id)}</td><td>${number(item.delay, 2)}</td><td>${number(item.doppler, 1)}</td><td>${age === null ? '—' : `${number(age, 1)} s`}</td></tr>`;
    });
    renderHtml(detail, aircraft.length ? table(['Aircraft', 'Bistatic range (km)', 'Doppler (Hz)', 'Position age'], aircraftRows) : empty('No aircraft in the feed', 'ADS-B is connected.'));
    const traces = [
      {x: radar.map(item => item.delay), y: radar.map(item => item.doppler), type: 'scatter', mode: 'markers', name: 'Radar detections', marker: {size: 10, color: colors[0]}, customdata: radar.map(item => item.snr), hovertemplate: 'Radar<br>Range %{x:.2f} km<br>Doppler %{y:.1f} Hz<br>SNR %{customdata:.1f} dB<extra></extra>'},
      {x: aircraft.map(item => finite(item.delay)), y: aircraft.map(item => finite(item.doppler)), type: 'scatter', mode: 'markers+text', name: 'ADS-B', text: aircraft.map(aircraftLabel), hovertext: aircraft.map(item => aircraftHoverText(item, {delay: finite(item.delay), doppler: finite(item.doppler)})), textposition: 'top center', marker: {size: 11, color: colors[1], symbol: aircraft.map(aircraftPlotSymbol), line: {width: 2}}, hovertemplate: '%{hovertext}<extra></extra>'}
    ];
    await Plotly.react('data', traces, {...plotBase, xaxis: {title: 'Bistatic range (km)', gridcolor: '#47362e'}, yaxis: {title: 'Bistatic Doppler (Hz)', gridcolor: '#47362e'}}, plotConfig);
    setState(`ADS-B online · ${timeLabel(detection?.timestamp)}`, 'good');
  }

  async function renderTracks() {
    setupPlotScaffold();
    const [config, tracker] = await Promise.all([getConfig(), apiJson('/api/tracker')]);
    const enabled = config?.process?.tracker?.enable === true;
    const tracks = array(tracker?.data);
    renderHtml(document.getElementById('view-summary'), `${card(tracker?.nActive ?? 0, 'Confirmed active')}${card(tracker?.nAssociated ?? 0, 'Associated candidates')}${card(tracker?.nCoasting ?? 0, 'Confirmed coasting')}${card(tracker?.nTentative ?? 0, 'Tentative candidates')}`);
    const detail = document.getElementById('view-detail');
    if (!enabled) {
      renderHtml(detail, empty('Tracking is disabled', 'Enable the tracker in Settings and restart radar processing.', '<a class="button-link" href="/display/configuration">Open Settings</a>'));
      await emptyPlot('Tracking is disabled.');
      setState('Disabled', 'warning');
      return;
    }
    const rows = tracks.map(track => `<tr><td>${escapeHtml(track.id)}</td><td><span class="state-pill ${escapeHtml(String(track.state || '').toLowerCase())}">${escapeHtml(humanize(track.state))}</span></td><td>${number(trackRangeKm(track.delay, config), 2)}</td><td>${number(track.doppler, 1)}</td><td>${integer(track.n)}</td></tr>`);
    const stateGuide = '<p class="view-note"><strong>State guide:</strong> Tentative and associated rows are candidates still completing the M-of-N confirmation rule. Active is confirmed. Coasting was confirmed but has temporarily missed detections. Only confirmed active tracks appear on the operational Map.</p>';
    renderHtml(detail, stateGuide + (tracks.length ? table(['Track', 'State', 'Bistatic range (km)', 'Doppler (Hz)', 'Observations'], rows) : empty(tracker?.nTentative ? 'Waiting for track confirmation' : 'No confirmed tracks', tracker?.nTentative ? `${tracker.nTentative} tentative track${tracker.nTentative === 1 ? ' needs' : 's need'} more detections.` : 'No detection sequence meets the tracking rules.')));
    const traces = [];
    tracks.forEach((track, index) => {
      const ranges = array(track.associated_delay).map(value => trackRangeKm(value, config));
      const dopplers = array(track.associated_doppler).map(value => finite(value));
      const color = colors[index % colors.length];
      traces.push({x: ranges, y: dopplers, type: 'scatter', mode: 'lines+markers', name: `Track ${track.id}`, line: {color, width: 2}, marker: {size: ranges.map((_, point) => point === ranges.length - 1 ? 11 : 5), color}, customdata: array(track.associated_state), hovertemplate: `Track ${escapeHtml(track.id)}<br>Range %{x:.2f} km<br>Doppler %{y:.1f} Hz<br>%{customdata}<extra></extra>`});
    });
    if (traces.length) await Plotly.react('data', traces, {...plotBase, xaxis: {title: 'Bistatic range (km)', gridcolor: '#47362e'}, yaxis: {title: 'Bistatic Doppler (Hz)', gridcolor: '#47362e'}}, plotConfig);
    else await emptyPlot(tracker?.nTentative ? 'Tentative tracks are forming.' : 'No persistent tracks are currently available.', {xaxis: {title: 'Bistatic range (km)', gridcolor: '#47362e'}, yaxis: {title: 'Bistatic Doppler (Hz)', gridcolor: '#47362e'}});
    setState(`Frame ${timeLabel(tracker?.timestamp)}`, 'good');
  }

  async function renderStatistics() {
    const [authoritative, provisional] = await Promise.all([
      apiJson('/api/live-scorecard'), apiJson('/api/live-scorecard/provisional')]);
    const authoritativeFrames = authoritative?.data_quality?.exact_timed_cpi_frames || 0;
    const statistics = authoritativeFrames > 0 ? authoritative : provisional;
    const decisionGrade = statistics?.decision_grade !== false;
    const windows = statistics?.windows || {};
    const periods = [['Hour', windows.hour], ['Day', windows.day],
      ['Week', windows.week], ['Lifetime', windows.lifetime]];
    const lifetime = windows.lifetime || {};
    const hour = windows.hour || {};
    const limits = statistics?.limits || {};
    const scoreRows = periods.map(([label, item = {}]) => `<tr><td>${label}</td><td>${integer(item.eligible_passes)}</td><td>${percent(item.detection_yield, 1)}</td><td>${percent(item.tracking_yield, 1)}</td></tr>`);
    const outcomeRows = periods.map(([label, item = {}]) => `<tr><td>${label}</td><td>${integer(item.out_of_config_passes)}</td><td>${integer(item.eligible_no_radar_return)}</td><td>${integer(item.detected_not_tracked)}</td><td>${integer(item.tracked_passes)}</td></tr>`);
    const exclusionRows = periods.map(([label, item = {}]) => `<tr><td>${label}</td><td>${integer(item.out_of_span?.below_detection_floor)}</td><td>${integer(item.out_of_span?.above_delay_ceiling)}</td><td>${integer(item.out_of_span?.outside_doppler_span)}</td><td>${integer(item.out_of_span?.stale_truth)}</td><td>${integer(item.out_of_span?.missing_exact_timing)}</td><td>${integer(item.out_of_span?.beyond_clutter_cancellation)}</td></tr>`);
    const activeRows = array(statistics.active_passes).map(item => `<tr><td>${escapeHtml(item.flight || item.hex)}</td><td>${escapeHtml(humanize(item.status))}</td><td>${integer(item.eligible_cpis)}</td><td>${integer(item.detected_cpis)}</td><td>${integer(item.tracked_cpis)}</td><td>${number(item.last_truth?.delay_km, 2)}</td><td>${number(item.last_truth?.doppler_hz, 1)}</td></tr>`);
    const finalizedRows = array(statistics.recent_finalized_passes).map(item => `<tr><td>${escapeHtml(item.flight || item.hex)}</td><td>${escapeHtml(humanize(item.outcome))}</td><td>${integer(item.eligible_cpis)}</td><td>${integer(item.detected_cpis)}</td><td>${integer(item.tracked_cpis)}</td><td>${timeLabel(item.finalized_at)}</td></tr>`);
    const quality = statistics.data_quality || {};
    const mix = statistics.mix_adjusted_previous_hour_comparison || {};
    const yieldFraction = value => Math.max(0, Math.min(1, finite(value) ?? 0));
    const yieldChart = (rows, label) => rows.length ? `<div class="statistics-yield-chart" role="img" aria-label="${escapeHtml(label)}">
      ${rows.map(([name, item = {}]) => `<div class="statistics-yield-row"><span>${escapeHtml(name)}</span><div class="statistics-yield-bars" title="Detection ${percent(item.detection_yield, 1)}; tracking ${percent(item.tracking_yield, 1)}"><i class="detected" style="width:${(yieldFraction(item.detection_yield) * 100).toFixed(1)}%"></i><i class="tracked" style="width:${(yieldFraction(item.tracking_yield) * 100).toFixed(1)}%"></i></div><b>${percent(item.detection_yield, 0)} / ${percent(item.tracking_yield, 0)}</b><small>n=${integer(item.eligible_passes)}</small></div>`).join('')}
      <div class="statistics-yield-legend"><span class="detected">Detected</span><span class="tracked">Tracked</span><small>Percentages · eligible sample count</small></div></div>` : '';
    const bandEntries = bands => Object.entries(bands || {}).map(([name, item]) => [name, item]);
    const bandChart = (groups, label) => {
      const rows = groups.flatMap(([prefix, bands]) => bandEntries(bands).map(([name, item]) =>
        [`${prefix} ${name}`, item]));
      return yieldChart(rows, label);
    };
    const jointEntries = Object.entries(hour.azimuth_elevation_strata?.bands || {});
    const bandOrder = value => finite(String(value).match(/-?\d+(?:\.\d+)?/)?.[0]) ?? Infinity;
    const azimuthBands = [...new Set(jointEntries.map(([name, item]) =>
      item.azimuth_band || name.split('|')[0]))].sort((a, b) => bandOrder(a) - bandOrder(b));
    const elevationBands = [...new Set(jointEntries.map(([name, item]) =>
      item.elevation_band || name.split('|')[1]))].sort((a, b) => bandOrder(a) - bandOrder(b));
    const jointByBand = new Map(jointEntries.map(([name, item]) => [
      `${item.azimuth_band || name.split('|')[0]}|${item.elevation_band || name.split('|')[1]}`, item]));
    const angleMatrix = jointEntries.length ? `<div class="statistics-matrix-wrap"><div class="statistics-matrix" style="--matrix-columns:${elevationBands.length}"><span class="matrix-corner">Azimuth ↓ / elevation →</span>${elevationBands.map(label => `<b>${escapeHtml(label)}°</b>`).join('')}${azimuthBands.map(azimuth => `<strong>${escapeHtml(azimuth)}°</strong>${elevationBands.map(elevation => {
      const item = jointByBand.get(`${azimuth}|${elevation}`);
      const tracking = yieldFraction(item?.tracking_yield);
      return `<span class="statistics-matrix-cell${item ? '' : ' empty'}" style="--matrix-strength:${tracking.toFixed(3)}" title="${escapeHtml(azimuth)}° azimuth, ${escapeHtml(elevation)}° elevation: detection ${percent(item?.detection_yield, 1)}, tracking ${percent(item?.tracking_yield, 1)}, n=${integer(item?.eligible_passes)}"><i>D ${percent(item?.detection_yield, 0)}</i><i>T ${percent(item?.tracking_yield, 0)}</i><small>${item ? `n=${integer(item.eligible_passes)}` : '—'}</small></span>`;
    }).join('')}`).join('')}</div><p class="statistics-visual-note">Color intensity follows tracking yield. Every cell also shows detection, tracking, and its eligible-pass denominator.</p></div>` : '';
    const mixRows = [['Current hour', mix.current], ['Previous hour', mix.baseline]]
      .map(([label, item = {}]) => `<tr><td>${label}</td><td>${percent(item.detection_yield, 1)}</td><td>${percent(item.tracking_yield, 1)}</td></tr>`);
    const stratumRows = (definition, bands) => Object.entries(bands || {}).map(([label, item]) =>
      `<tr><td>${escapeHtml(definition)}</td><td>${escapeHtml(label)}</td><td>${integer(item.eligible_passes)}</td><td>${integer(item.detected_passes)}</td><td>${integer(item.tracked_passes)}</td><td>${percent(item.tracking_yield, 1)}</td></tr>`);
    const rangeRows = [
      ...stratumRows('Excess delay km', hour.delay_strata?.bands),
      ...stratumRows('Receiver slant km', hour.receiver_slant_range_strata?.bands)];
    const angleRows = [
      ...stratumRows('Elevation deg', hour.elevation_strata?.bands),
      ...stratumRows('True azimuth deg', hour.azimuth_strata?.bands)];
    const jointRows = Object.entries(hour.azimuth_elevation_strata?.bands || {}).map(([label, item]) =>
      `<tr><td>${escapeHtml(item.azimuth_band || label.split('|')[0])}</td><td>${escapeHtml(item.elevation_band || label.split('|')[1])}</td><td>${integer(item.eligible_passes)}</td><td>${percent(item.detection_yield, 1)}</td><td>${percent(item.tracking_yield, 1)}</td></tr>`);
    const adsbWarning = statistics.adsb_quality?.warning;
    renderHtml(root, `<div class="statistics-page">${adsbWarning ? `<div class="evaluation-note"><strong>ADS-B data note:</strong> ${escapeHtml(adsbWarning)}</div>` : ''}<div class="insight-stats insight-stats-four">
      ${card(integer(lifetime.eligible_passes), 'Eligible aircraft passes')}
      ${card(percent(lifetime.detection_yield, 1), 'Lifetime detection yield')}
      ${card(percent(lifetime.tracking_yield, 1), 'Lifetime tracking yield')}
      ${card(integer(statistics.active_passes?.length), 'Aircraft live now')}
    </div>
    <section class="statistics-section"><header><h2>Performance</h2><p>Eligible aircraft only; live passes are finalized after they leave the searchable region.</p></header><div class="statistics-grid">
      <section class="info-card statistics-score"><h2>Detection and tracking yield</h2>${yieldChart(periods, 'Detection and tracking yield by time window')}${table(['Window', 'Eligible passes', 'Detection yield', 'Tracking yield'], scoreRows)}<p class="view-note">Each credited pass needs at least three unambiguous delay–Doppler matches. These raw yields describe observed traffic; use the controlled comparison for hour-to-hour changes.</p></section>
      <section class="info-card"><h2>Traffic-mix controlled comparison</h2>${table(['Period', 'Detection yield', 'Tracking yield'], mixRows)}<dl><div><dt>Comparable traffic coverage</dt><dd>${percent(mix.common_support_coverage, 1)}</dd></div><div><dt>Shared geometry groups</dt><dd>${integer(mix.common_strata?.length)}</dd></div><div><dt>Interpretation</dt><dd>${mix.decision_grade ? 'Suitable for comparison' : 'Descriptive only'}</dd></div></dl><p class="view-note">Both hours are standardized to the same excess-delay and receiver-range mix, so a change in aircraft distance alone does not look like a detector change.</p></section>
    </div></section>
    <section class="statistics-section"><header><h2>Current-hour coverage</h2><p>Where eligible aircraft tested the detector and tracker.</p></header><div class="statistics-grid">
      <section class="info-card"><h2>Angle bands</h2>${angleRows.length ? `${bandChart([['Elevation', hour.elevation_strata?.bands], ['Azimuth', hour.azimuth_strata?.bands]], 'Detection and tracking yield by elevation and azimuth')}${table(['Angle', 'Band', 'Eligible', 'Detected', 'Tracked', 'Tracking yield'], angleRows)}` : empty('Angle coverage collecting', 'Finalized eligible passes will populate azimuth and elevation bands.')}</section>
      <section class="info-card"><h2>Azimuth × elevation</h2>${jointRows.length ? `${angleMatrix}<details class="statistics-table-details"><summary>Exact matrix values</summary>${table(['Azimuth', 'Elevation', 'Eligible', 'Detection yield', 'Tracking yield'], jointRows)}</details>` : empty('Joint angle coverage collecting', 'Finalized eligible passes will populate the array coverage matrix.')}</section>
      <section class="info-card statistics-wide"><h2>Range bands</h2>${rangeRows.length ? `${bandChart([['Delay', hour.delay_strata?.bands], ['Slant', hour.receiver_slant_range_strata?.bands]], 'Detection and tracking yield by range')}${table(['Range measure', 'Band', 'Eligible', 'Detected', 'Tracked', 'Tracking yield'], rangeRows)}` : empty('Range coverage collecting', 'Finalized eligible passes will populate delay and receiver-range bands.')}</section>
    </div></section>
    <section class="statistics-section"><header><h2>Aircraft evidence</h2><p>Live opportunities and the most recently completed passes.</p></header><div class="statistics-grid">
      <section class="info-card"><h2>Aircraft live now</h2>${activeRows.length ? table(['Aircraft', 'Status', 'Eligible', 'Detected', 'Tracked', 'Delay km', 'Doppler Hz'], activeRows) : empty('No eligible aircraft live', 'Waiting for an aircraft inside the searchable region.')}</section>
      <section class="info-card"><h2>Recent finalized passes</h2>${finalizedRows.length ? table(['Aircraft', 'Outcome', 'Eligible', 'Detected', 'Tracked', 'Finalized'], finalizedRows) : empty('No finalized passes yet', 'A pass finalizes after the aircraft leaves the eligible region.')}</section>
    </div></section>
    <details class="statistics-details"><summary>Eligibility and scoring details</summary><div class="statistics-grid">
      <section class="info-card"><h2>Pass outcomes</h2>${table(['Window', 'Out of config', 'No radar return', 'Detected, not tracked', 'Tracked'], outcomeRows)}<p class="view-note">Out-of-config passes never enter the performance denominator. No radar return means neither a strict detection nor a stable track was credited.</p></section>
      <section class="info-card"><h2>Excluded opportunities</h2>${table(['Window', 'Below range', 'Above range', 'Outside Doppler', 'Stale truth', 'Unusable alignment', 'Beyond clutter'], exclusionRows)}<p class="view-note">Invalid or unusable truth is excluded, never counted as a miss. Beyond-clutter opportunities remain searchable but are reported separately.</p></section>
      <section class="info-card"><h2>Search and match limits</h2><dl><div><dt>Physical delay span</dt><dd>${number(limits.delayMinKm, 3)}–${number(limits.delayMaxKm, 3)} km</dd></div><div><dt>Doppler span</dt><dd>${number(limits.dopplerMinHz, 0)} to ${number(limits.dopplerMaxHz, 0)} Hz</dd></div><div><dt>Clutter cancellation</dt><dd>Through ${number(limits.clutterMaxKm, 3)} km</dd></div><div><dt>Strict match gate</dt><dd>±${number(limits.matchDelayKm, 2)} km / ±${number(limits.matchDopplerHz, 1)} Hz</dd></div></dl></section>
      <section class="info-card"><h2>Scoring method</h2><dl><div><dt>Scorecard use</dt><dd>${decisionGrade ? 'Decision-capable' : 'Operational and descriptive'}</dd></div><div><dt>Shared frame hold</dt><dd>${integer(statistics.association_lag_cpis)} CPIs · ${integer(statistics.association_lag_ms)} ms</dd></div><div><dt>Contract</dt><dd>${escapeHtml(String(statistics.evaluation_contract_hash || '').slice(0, 12))}</dd></div><div><dt>Nuisance accounting</dt><dd>${escapeHtml(humanize(quality.nuisance_accounting))}</dd></div></dl><p class="view-note">Radar detections, tracks and aircraft truth are joined at one CPI timestamp. Live states are provisional until a pass finalizes. Tracking credit requires at least three ACTIVE, measurement-supported associations. Held, stale, or contract-disagreeing truth is excluded instead of scored as a miss.</p></section>
    </div></details></div>`);
    setState(statistics.error ? `Collection warning · ${statistics.error}` :
      statistics.updated_at ? `Updated ${timeLabel(statistics.updated_at)}` : 'Waiting for eligible ADS-B evidence',
    statistics.error ? 'warning' : statistics.updated_at ? 'good' : 'warning');
  }

  function bearingEngineeringDiagnostics(bearingDiagnostic) {
    if (!bearingDiagnostic) return `<section class="info-card"><h2>Array engineering diagnostics</h2><p class="view-note">Known-angle and wiring evidence is not available from this receiver.</p></section>`;
    const reference = bearingDiagnostic.known_angle_reference;
    const referenceMarkup = !reference ? '' : reference.available ?
      `<section class="info-card"><h2>Known-angle reference · shadow only</h2><dl>
        <div><dt>RF source</dt><dd>${escapeHtml(reference.source?.name || 'Configured transmitter')} · ${number(reference.source?.bearing_deg_true, 3)}° true</dd></div>
        <div><dt>Configured fit</dt><dd>Heading ${number(reference.configured?.heading_deg_true, 1)}° · phase error ${number(reference.configured?.median_phase_error_deg, 1)}° median / ${number(reference.configured?.p95_phase_error_deg, 1)}° p95</dd></div>
        <div><dt>Best mapping at configured heading</dt><dd>${escapeHtml(array(reference.best_mapping_at_configured_heading?.mapping).join(' → ') || 'Unavailable')} · phase error ${number(reference.best_mapping_at_configured_heading?.median_phase_error_deg, 1)}°</dd></div>
        <div><dt>Fitted phase axis</dt><dd>${number(reference.heading_with_configured_mapping?.heading_deg_true, 0)}° true (${number(reference.heading_with_configured_mapping?.signed_correction_deg, 0)}° model offset) · phase error ${number(reference.heading_with_configured_mapping?.median_phase_error_deg, 1)}°</dd></div>
        <div><dt>RF evidence</dt><dd>${integer(reference.quality?.updates)} updates · coherence ${number(reference.quality?.coherent_fraction_median, 3)} median · amplitude span ${number(reference.quality?.amplitude_span_db_median, 1)} dB median</dd></div>
      </dl>${reference.quality?.reliable === false ? '<p class="view-note">This source is excluded from multi-reference calibration by the RF quality gate.</p>' : ''}<p class="view-note">${escapeHtml(reference.limitation)}</p></section>` :
      `<section class="info-card"><h2>Known-angle reference</h2><p class="view-note">${escapeHtml(reference.reason)}</p></section>`;
    const wiringMarkup = bearingDiagnostic.available ?
      `<section class="info-card"><h2>Bearing wiring diagnostic · shadow only</h2><dl>
        <div><dt>Evidence</dt><dd>${integer(bearingDiagnostic.samples)} samples · ${integer(bearingDiagnostic.aircraft)} aircraft · ${integer(bearingDiagnostic.sectors)}/8 sectors</dd></div>
        <div><dt>Active mapping</dt><dd>${escapeHtml(array(bearingDiagnostic.active?.mapping).join(' → ') || 'Unavailable')} · median error ${number(bearingDiagnostic.active?.median_error_deg, 1)}°</dd></div>
        <div><dt>Best replay</dt><dd>${escapeHtml(array(bearingDiagnostic.best?.mapping).join(' → ') || 'Unavailable')} · median error ${number(bearingDiagnostic.best?.median_error_deg, 1)}°</dd></div>
        <div><dt>Heading-only correction</dt><dd>${number(bearingDiagnostic.global_heading_bias?.signed_correction_deg, 0)}° · median error ${number(bearingDiagnostic.global_heading_bias?.median_error_deg, 1)}°</dd></div>
        <div><dt>Decision</dt><dd>${bearingDiagnostic.evidence_ready ? 'Evidence threshold met for review' : 'Still collecting coverage'}; changes are never applied automatically.</dd></div>
      </dl><p class="view-note">${escapeHtml(bearingDiagnostic.identifiability)}</p></section>` :
      `<section class="info-card"><h2>Bearing wiring diagnostic</h2><p class="view-note">${escapeHtml(bearingDiagnostic.reason)}</p></section>`;
    return `<details class="statistics-details calibration-engineering"><summary>Array engineering diagnostics</summary><div class="statistics-grid">${wiringMarkup}${referenceMarkup}</div></details>`;
  }

  async function renderCalibration() {
    setupPlotScaffold('insight-content-wide');
    const campaign = await apiJson('/api/array-calibration/status');
    let bearingDiagnostic = null;
    try { bearingDiagnostic = await apiJson('/api/bearing/diagnostic'); }
    catch (_) { /* Optional on receivers without bearing support. */ }
    const bearingCampaign = campaign?.bearing || {};
    const progress = bearingCampaign.progress || {};
    const automation = bearingCampaign.automation || {};
    const recorder = campaign?.corpus || {};
    const reference = campaign?.reference || {};
    const detectionCampaign = campaign?.detection || {};
    const detectionProgress = detectionCampaign.progress || {};
    const detectionAutomation = detectionCampaign.automation || {};
    const sectors = array(progress.sectors);
    const requirements = array(progress.requirements);
    const completion = finite(progress.evidence_completion_fraction) ?? 0;
    renderHtml(document.getElementById('view-summary'),
      `${card(percent(completion, 0), 'Collection gate progress', progress.collection_ready ? 'good' : 'warning')}` +
      `${card(integer(progress.independent_passes), 'Independent passes')}` +
      `${card(integer(progress.unique_aircraft), 'Unique aircraft')}` +
      `${card(`${integer(progress.covered_sectors)}/${integer(progress.required_covered_sectors)}`, 'Covered sectors')}`);
    const requirementRows = requirements.map(item => `<tr><td>${escapeHtml(item.label)}</td><td>${item.comparison === 'at_most' ? `≤ ${number(item.required, 1)}` : integer(item.required)}</td><td>${item.comparison === 'at_most' ? number(item.current, 1) : integer(item.current)}</td><td><span class="state-pill ${item.complete ? 'active' : 'coasting'}">${item.complete ? 'Ready' : 'Collecting'}</span></td></tr>`);
    const sectorRows = sectors.map(item => `<tr><td>${integer(item.from_deg)}–${integer(item.to_deg)}°</td><td>${integer(item.samples)}</td><td>${integer(item.passes)}</td><td>${integer(item.aircraft)}</td><td><span class="state-pill ${item.ready ? 'active' : 'coasting'}">${item.ready ? 'Covered' : `${integer(item.passes_needed)} passes / ${integer(item.aircraft_needed)} aircraft needed`}</span></td></tr>`);
    const blockers = array(progress.next_actions);
    const failureText = array(automation.failures).filter(Boolean);
    const detectionFailureText = array(detectionAutomation.failures).filter(Boolean);
    const active = Boolean(bearingCampaign.active_model);
    const frequencyHz = finite(campaign.frequency_hz);
    renderDetail(`<div class="overview-grid">
      <section class="info-card"><h2>Campaign state</h2><dl>
        <div><dt>Frequency</dt><dd>${frequencyHz === null ? '—' : `${number(frequencyHz / 1e6, 3)} MHz`}</dd></div>
        <div><dt>Shared corpus recorder</dt><dd>${recorder.enabled ? (recorder.automatic ? 'Collecting automatically' : 'Collecting — explicitly enabled') : 'Disabled'} · ${integer(progress.samples)} retained samples</dd></div>
        <div><dt>Fitter</dt><dd>${escapeHtml(humanize(automation.state))}${automation.running ? ' · running now' : ''}</dd></div>
        <div><dt>Candidate</dt><dd>${automation.qualified ? 'Qualified on held-out passes' : 'Not qualified'}</dd></div>
        <div><dt>Live bearing manifold</dt><dd>${active ? escapeHtml(bearingCampaign.active_model) : 'Ideal steering remains active'}</dd></div>
        <div><dt>Last sample</dt><dd>${progress.last_sample_at ? escapeHtml(timeLabel(progress.last_sample_at)) : 'Waiting for accepted evidence'}</dd></div>
      </dl><p class="view-note">${escapeHtml(progress.limitation || 'Collection and production qualification are separate.')}</p></section>
      <section class="info-card"><h2>Next evidence needed</h2>${blockers.length ? `<ul class="calibration-blockers">${blockers.map(item => `<li>${escapeHtml(item)}</li>`).join('')}</ul>` : '<p class="view-note">Collection gates are complete. Held-out performance gates determine whether the candidate can activate.</p>'}${failureText.length ? `<p class="view-note evaluation-note">Latest fit: ${escapeHtml(failureText.join(' '))}</p>` : ''}</section>
      <section class="info-card"><h2>Reference stability</h2><dl>
        <div><dt>Reference samples</dt><dd>${integer(reference.samples)}</dd></div>
        <div><dt>Latest phase step</dt><dd>${number(reference.maximum_phase_step_deg, 2)}°</dd></div>
        <div><dt>Latest amplitude step</dt><dd>${number(reference.maximum_amplitude_step_db, 2)} dB</dd></div>
        <div><dt>Recorder errors</dt><dd>${recorder.error ? escapeHtml(recorder.error) : 'None'}</dd></div>
      </dl></section>
      <section class="info-card"><h2>Detection manifold shadow</h2><dl>
        <div><dt>Automation</dt><dd>${detectionAutomation.enabled ? escapeHtml(humanize(detectionAutomation.state)) : 'Disabled by default'}</dd></div>
        <div><dt>Positive-control samples</dt><dd>${integer(detectionProgress.accepted_samples)}</dd></div>
        <div><dt>Positive-control fit</dt><dd>${detectionProgress.positive_control?.fit_ready ? 'Ready' : 'Collecting detected cells'}</dd></div>
        <div><dt>Truth-selected replay diagnostic</dt><dd>${detectionProgress.validation?.ready ? 'Coverage ready' : 'Coverage incomplete'} · ${integer(detectionProgress.validation_passes)} passes / ${integer(detectionProgress.miss_passes)} misses</dd></div>
        <div><dt>Production full-bank receipts</dt><dd>${integer(detectionProgress.production_replay?.imported_passes)} imported · receipt harness ready / native export required</dd></div>
        <div><dt>Supported sectors / bins</dt><dd>${integer(detectionProgress.covered_sectors)} / ${integer(detectionProgress.fit_bins)}</dd></div>
        <div><dt>Candidate</dt><dd>Diagnostic only — production qualification disabled</dd></div>
        <div><dt>Runtime compatibility</dt><dd>${detectionAutomation.runtime_candidate_path ? 'Replay-only dense artifact available' : 'None — sparse offline model'}</dd></div>
        <div><dt>Promotion</dt><dd>Never automatic</dd></div>
        <div><dt>Errors</dt><dd>${detectionAutomation.error ? escapeHtml(detectionAutomation.error) : 'None'}</dd></div>
      </dl><p class="view-note">${escapeHtml(detectionProgress.limitation || 'Detection-manifold fitting is an isolated, diagnostic-only shadow path.')}</p>${detectionFailureText.length ? `<p class="view-note evaluation-note">Qualification blocked: ${escapeHtml(detectionFailureText.join(' '))}</p>` : ''}</section>
    </div><section class="info-card"><h2>Qualification gates</h2>${table(['Requirement', 'Target', 'Current', 'State'], requirementRows)}</section>
    <section class="info-card"><h2>Azimuth-sector corpus</h2>${table(['Local azimuth', 'Samples', 'Passes', 'Aircraft', 'Coverage'], sectorRows)}</section>${bearingEngineeringDiagnostics(bearingDiagnostic)}`);
    const labels = sectors.map(item => `${integer(item.from_deg)}–${integer(item.to_deg)}°`);
    await Plotly.react('data', [
      {x: labels, y: sectors.map(item => item.passes), name: 'Independent passes', type: 'bar', marker: {color: colors[0]}},
      {x: labels, y: sectors.map(item => item.aircraft), name: 'Unique aircraft', type: 'bar', marker: {color: colors[1]}}
    ], {...plotBase, barmode: 'group', xaxis: {title: 'Local array azimuth sector', gridcolor: '#47362e'}, yaxis: {title: 'Independent evidence', rangemode: 'tozero', gridcolor: '#47362e'}}, plotConfig);
    const tone = recorder.error || automation.error || detectionAutomation.error ? 'bad' : automation.qualified ? 'good' : 'warning';
    setState(recorder.error || automation.error || detectionAutomation.error ? `Calibration warning · ${recorder.error || automation.error || detectionAutomation.error}` :
      automation.qualified ? 'Qualified measured manifold' : `Collecting · ${percent(completion, 0)} of minimum evidence gates`, tone);
  }

  const renderers = {overview: renderOverview, detections: renderDetections, health: renderHealth, activity: renderActivity, site: renderSite, locations: renderLocations, evaluation: renderEvaluation, tracks: renderTracks, statistics: renderStatistics, calibration: renderCalibration};

  async function refresh() {
    const now = Date.now();
    if (refreshing || now < nextRefreshAt || !renderers[view]) return;
    nextRefreshAt = now + minimumRefreshIntervalMs;
    refreshing = true;
    if (root.dataset.ready !== 'true' && root.dataset.loading !== 'true') {
      root.dataset.loading = 'true';
      root.setAttribute?.('aria-busy', 'true');
      root.innerHTML = '<div class="page-loading" role="status"><span class="loading-spinner" aria-hidden="true"></span><strong>Loading live data…</strong><small>The last valid view will remain visible during later refreshes.</small></div>';
    }
    try {
      await renderers[view]();
      // Every renderer, including HTML-only views, has now produced a valid
      // snapshot.  A later transient failure must never erase it.
      root.dataset.ready = 'true';
      root.dataset.loading = 'false';
      root.setAttribute?.('aria-busy', 'false');
      consecutiveRefreshFailures = 0;
      window.blah2ViewLastError = null;
    } catch (error) {
      window.blah2ViewLastError = error?.message || String(error);
      consecutiveRefreshFailures++;
      const hasSnapshot = root.dataset.ready === 'true' && root.children.length > 0;
      if (!hasSnapshot) {
        setState('Data unavailable', 'bad');
        root.innerHTML = empty('This view cannot reach its live data', error.message || 'Check the radar API connection.');
      } else if (consecutiveRefreshFailures >= 3) {
        // Preserve the last valid snapshot.  One or two missed polls are
        // intentionally silent so routine mobile jitter does not flash an
        // alarming state between healthy frames.
        setState('Refresh delayed · showing last update', 'warning');
      }
    } finally {
      refreshing = false;
    }
  }

  startRadarUpdates(refresh);
}());
