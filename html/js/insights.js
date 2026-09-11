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
    legend: {orientation: 'h', x: 0, y: 1.08, bgcolor: 'rgba(0,0,0,0)'}
  };
  let refreshing = false;

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
    return fetchJson(liveApiUrl(path));
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

  async function emptyPlot(message, layout = {}) {
    await Plotly.react('data', [], {...plotBase, ...layout, annotations: [{
      text: message, showarrow: false, font: {color: '#ac9e94', size: 14}
    }]}, plotConfig);
  }

  async function renderOverview() {
    const [config, detection, tracker, timing] = await Promise.all([
      getConfig(), apiJson('/api/detection'), apiJson('/api/tracker'), apiJson('/api/timing')
    ]);
    const detections = detectionRows(detection);
    const tracks = array(tracker?.data);
    const strongest = [...detections].sort((left, right) => (right.snr ?? -Infinity) - (left.snr ?? -Infinity))[0];
    const hop = finite(config?.process?.data?.cpi) === null ? null : config.process.data.cpi * 1000;
    const load = hop && finite(timing?.cpi) !== null ? timing.cpi / hop * 100 : null;
    const receiver = config?.location?.rx?.name || 'Receiver site';
    const transmitter = config?.location?.tx?.name || 'Illuminator site';
    root.innerHTML = `<div class="insight-stats insight-stats-four">
      ${card(detections.length, 'Detections in latest CPI')}
      ${card(tracks.length, 'Persistent tracks')}
      ${card(strongest ? `${number(strongest.snr, 1)} dB` : 'None', 'Strongest detection')}
      ${card(load === null ? '—' : `${number(load, 0)}%`, 'Processing budget', load > 100 ? 'bad' : load > 80 ? 'warning' : 'good')}
    </div><div class="overview-grid">
      <section class="info-card"><h2>Radar</h2><dl><div><dt>Receiver</dt><dd>${escapeHtml(receiver)}</dd></div><div><dt>Illuminator</dt><dd>${escapeHtml(transmitter)}</dd></div><div><dt>Radio</dt><dd>${escapeHtml(config?.capture?.device?.type || 'Receiver')} · ${escapeHtml(frequencyLabel(config))}</dd></div><div><dt>Inputs</dt><dd>${escapeHtml(channelCount(config) ?? 'Unknown')} channels</dd></div></dl></section>
      <section class="info-card"><h2>Current activity</h2>${strongest ? `<dl><div><dt>Bistatic range</dt><dd>${number(strongest.delay, 2)} km</dd></div><div><dt>Doppler</dt><dd>${number(strongest.doppler, 1)} Hz</dd></div><div><dt>Signal-to-noise ratio</dt><dd>${number(strongest.snr, 1)} dB</dd></div><div><dt>Frame</dt><dd>${escapeHtml(timeLabel(detection?.timestamp))}</dd></div></dl>` : empty('No detections in the latest CPI', 'The radar is running; no return currently passes the configured detector.')}</section>
      <section class="info-card overview-links"><h2>Open a view</h2><a href="/display/locations">Possible locations <b>→</b></a><a href="/display/detections">Current detections <b>→</b></a><a href="/display/tracks">Tracks <b>→</b></a><a href="/display/health">System health <b>→</b></a><a href="/display/site">Site geometry <b>→</b></a></section>
    </div>`;
    setState(`Updated ${timeLabel(timing?.timestamp)}`, 'good');
  }

  async function renderDetections() {
    const detection = await apiJson('/api/detection');
    const rows = detectionRows(detection).sort((left, right) => (right.snr ?? -Infinity) - (left.snr ?? -Infinity));
    const strongest = rows[0];
    const body = rows.map(item => `<tr><td>${item.index + 1}</td><td>${number(item.delay, 2)}</td><td>${number(item.doppler, 1)}</td><td>${number(item.snr, 1)}</td></tr>`);
    root.innerHTML = `<div class="insight-stats">${card(rows.length, 'Latest CPI')}${card(strongest ? `${number(strongest.snr, 1)} dB` : 'None', 'Strongest return')}</div>${rows.length ? table(['#', 'Bistatic range (km)', 'Doppler (Hz)', 'SNR (dB)'], body) : empty('No detections in the latest CPI', 'This is a valid live result; nothing currently passes the configured detector.')}`;
    setState(`Frame ${timeLabel(detection?.timestamp)}`, 'good');
  }

  function healthRow(name, value, detail, tone) {
    return `<article class="health-row"><span class="health-indicator ${tone}"></span><div><strong>${escapeHtml(name)}</strong><small>${escapeHtml(detail)}</small></div><b>${escapeHtml(value)}</b></article>`;
  }

  async function renderHealth() {
    const [config, timing, tracker] = await Promise.all([getConfig(), apiJson('/api/timing'), apiJson('/api/tracker')]);
    const adsb = await adsbHealth(config);
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
    const stages = Object.entries(timing || {}).filter(([key, value]) => finite(value) !== null && !['timestamp', 'nCpi', 'uptime_s', 'uptime_days', 'cpi_data_ms', 'cpi_hop_ms', 'cpi_overlap_fraction', 'capture_backlog_ms', 'cpi'].includes(key)).sort((left, right) => Number(right[1]) - Number(left[1]));
    root.innerHTML = `<div class="insight-stats">${card(overall, 'Overall state', overallTone)}${card(load === null ? '—' : `${number(load, 0)}%`, 'Processing budget', loadTone)}${card(`${channelCount(config) ?? '—'}`, 'Configured channels')}${card(timing?.uptime_s === undefined ? '—' : `${number(timing.uptime_s / 3600, 1)} h`, 'Processor uptime')}</div><div class="health-grid"><section class="health-list">${healthRow('Radar frames', ageMs === null ? 'Unavailable' : ageMs < 1000 ? 'Live' : `${number(ageMs / 1000, 1)} s old`, `Expected update every ${number(hop, 0)} ms`, freshnessTone)}${healthRow('Processing load', load === null ? 'Unavailable' : `${number(load, 0)}%`, processMs === null ? 'No timing data' : `${number(processMs, 1)} ms work per ${number(hop, 1)} ms update`, loadTone)}${healthRow('Capture backlog', backlog === null ? 'Unavailable' : `${number(backlog, 1)} ms`, 'Buffered capture data waiting for processing', backlogTone)}${healthRow('Detection', config?.process?.detection?.enable === false ? 'Disabled' : 'Enabled', 'Produces the current detection stream', config?.process?.detection?.enable === false ? 'neutral' : 'good')}${healthRow('Tracking', trackerEnabled ? 'Enabled' : 'Disabled', trackerEnabled ? `${integer(tracker?.n ?? 0)} total track hypotheses` : 'Optional processing is off', trackerTone)}${healthRow('ADS-B evaluation', adsb.disabledForReplay ? 'Not replayed' : !adsb.enabled ? 'Disabled' : adsb.online ? 'Online' : 'Offline', adsb.disabledForReplay ? 'Live truth is hidden during replay.' : adsb.enabled ? 'Independent evaluation truth; never a radar inference input' : 'Optional evaluation feed is off', adsbTone)}</section><section class="info-card"><h2>Processing stages</h2>${table(['Stage', 'Latest time (ms)'], stages.map(([name, value]) => `<tr><td>${escapeHtml(humanize(name))}</td><td>${number(value, 2)}</td></tr>`))}</section></div>`;
    setState(`Updated ${timeLabel(timing?.timestamp)}`, overallTone);
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
    document.getElementById('view-summary').innerHTML = `${card(latest.detections ?? 0, 'Latest detections')}${card(latest.tracks ?? 0, 'Persistent tracks')}${card(Number.isFinite(latest.snr) ? `${number(latest.snr, 1)} dB` : 'None', 'Strongest return')}${card(activityHistory.length, 'Samples this browser session')}`;
    document.getElementById('view-detail').innerHTML = `<div class="view-note">History is collected in this browser tab and is not written to the radar server.</div>`;
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
      document.getElementById('view-summary').innerHTML = card('Unavailable', 'Site coordinates');
      document.getElementById('view-detail').innerHTML = empty('Site geometry is unavailable', 'Configure receiver and illuminator latitude and longitude in Settings.');
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
    document.getElementById('view-summary').innerHTML = `${card(`${number(baseline, 2)} km`, 'Site baseline')}${card(resolution === null ? '—' : `${number(resolution, 3)} km`, 'Range-bin resolution')}${card(frequencyLabel(config), 'Carrier frequency')}`;
    document.getElementById('view-detail').innerHTML = `<section class="info-card"><h2>Configured sites</h2><dl><div><dt>Receiver</dt><dd>${escapeHtml(receiver.name || 'Receiver')} · ${number(receiver.latitude, 5)}, ${number(receiver.longitude, 5)} · ${number(receiver.altitude, 0)} m</dd></div><div><dt>Illuminator</dt><dd>${escapeHtml(transmitter.name || 'Illuminator')} · ${number(transmitter.latitude, 5)}, ${number(transmitter.longitude, 5)} · ${number(transmitter.altitude, 0)} m</dd></div><div><dt>Contours</dt><dd>Constant excess-path range derived from the configured delay window</dd></div></dl></section>`;
    await Plotly.react('data', traces, {...plotBase, xaxis: {title: 'East / west from receiver (km)', gridcolor: '#47362e', zerolinecolor: '#47362e'}, yaxis: {title: 'North / south from receiver (km)', gridcolor: '#47362e', zerolinecolor: '#47362e', scaleanchor: 'x', scaleratio: 1}}, plotConfig);
    setState('From active configuration', 'good');
  }

  let locationMapViewport = null;
  let locationMapDragging = false;
  let locationMapHoldUntil = 0;

  function bindLocationMapInteraction() {
    const map = document.getElementById('data');
    if (!map?.addEventListener || !window.addEventListener || map.dataset.locationInputBound) return;
    map.dataset.locationInputBound = 'true';
    map.addEventListener('pointerdown', () => { locationMapDragging = true; });
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
    return locationMapDragging || Date.now() < locationMapHoldUntil;
  }

  function renderAircraftOverlays(aircraft) {
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
        map.on('move', () => layer._updatePositions?.());
        map.on('resize', () => layer._updatePositions?.());
      }
      layer.replaceChildren();
      const markers = aircraft.map(item => {
        const marker = document.createElement('div');
        marker.className = 'location-aircraft-marker';
        Object.assign(marker.style, {position: 'absolute', width: '42px', height: '52px',
          transform: 'translate(-50%, -50%)', filter: 'drop-shadow(0 2px 3px rgba(0,0,0,.9))'});
        const heading = finite(item.track) ?? 0;
        const label = escapeHtml(String(item.flight || item.hex || 'Aircraft').trim());
        marker.innerHTML = `<svg viewBox="0 0 24 24" width="38" height="38" style="display:block;transform:rotate(${heading}deg)" aria-hidden="true"><path fill="#4dd4b0" stroke="#10251f" stroke-width="1.2" d="M21 16v-2l-8-5V3.5C13 2.67 12.33 2 11.5 2S10 2.67 10 3.5V9l-8 5v2l8-2.5V19l-2 1.5V22l3.5-1 3.5 1v-1.5L13 19v-5.5L21 16z"/></svg><span style="position:absolute;top:34px;left:50%;transform:translateX(-50%);padding:1px 4px;border-radius:4px;background:rgba(16,20,18,.84);color:#d9fff3;font:700 10px/1.3 Inter,system-ui,sans-serif;white-space:nowrap">${label}</span>`;
        layer.appendChild(marker);
        return {item, marker};
      });
      layer._updatePositions = () => markers.forEach(({item, marker}) => {
        const point = map.project([Number(item.lon), Number(item.lat)]);
        marker.style.left = `${point.x}px`;
        marker.style.top = `${point.y}px`;
      });
      layer._updatePositions();
    });
  }

  async function renderLocations() {
    setupPlotScaffold('insight-content-wide');
    bindLocationMapInteraction();
    if (locationMapIsBeingNavigated()) return;
    const [config, detection, tracker] = await Promise.all([
      getConfig(), apiJson('/api/detection'), apiJson('/api/tracker')
    ]);
    const receiver = config?.location?.rx || {};
    const transmitter = config?.location?.tx || {};
    if ([receiver.latitude, receiver.longitude, transmitter.latitude, transmitter.longitude]
      .some(value => finite(value) === null)) {
      document.getElementById('view-summary').innerHTML = card('Unavailable', 'Site coordinates');
      document.getElementById('view-detail').innerHTML = empty('Possible locations are unavailable', 'Configure receiver and illuminator coordinates in Settings.');
      await emptyPlot('Receiver and illuminator coordinates are required.');
      setState('Configuration incomplete', 'warning');
      return;
    }

    const resolution = rangeResolutionKm(config) ?? .1;
    const rawDetections = detectionRows(detection).filter(item => item.delay !== null && item.delay > 0);
    const currentTrackStates = new Set(['ACTIVE', 'ASSOCIATED', 'CONFIRMED']);
    const tracks = array(tracker?.data).map(track => ({
      id: track.id,
      state: track.state,
      delay: trackRangeKm(track.delay, config)
    })).filter(track => track.delay !== null && track.delay > 0 &&
      currentTrackStates.has(String(track.state || '').toUpperCase())).slice(0, 12);
    const hiddenCoastingTracks = array(tracker?.data).filter(track =>
      String(track?.state || '').toUpperCase() === 'COASTING').length;

    let aircraft = [];
    let aircraftError = null;
    if (config?.truth?.adsb?.enabled === true) {
      try {
        const adsb = await apiJson('/api/adsb');
        aircraft = adsb?.disabledForReplay ? [] : array(adsb?.aircraft).filter(item =>
          finite(item.lat) !== null && finite(item.lon) !== null && (finite(item.seen_pos) ?? 0) < 120);
      } catch (_) { aircraftError = 'ADS-B aircraft positions unavailable'; }
    }

    const tx = siteOffset(receiver, transmitter);
    const baseline = Math.hypot(tx.x, tx.y);
    const heading = Math.atan2(tx.y, tx.x);
    const ellipseTrace = item => {
      const path = ellipse(tx.x / 2, tx.y / 2, baseline, item.delay, heading);
      const geographic = localPathToCoordinates(receiver, path);
      const label = `Radar track ${item.id}`;
      return {
        type: 'scattermapbox', mode: 'lines', name: `${label} · ${number(item.delay, 2)} km`,
        lat: geographic.latitude, lon: geographic.longitude,
        line: {color: '#ff8754', width: 4},
        hovertemplate: `${escapeHtml(label)}<br>${escapeHtml(humanize(item.state))}<br>${number(item.delay, 2)} km bistatic range<br>Every point matches this delay<extra></extra>`
      };
    };
    const traces = tracks.map(item => ellipseTrace(item));
    traces.push({
      type: 'scattermapbox', mode: 'markers+text', name: 'Radar sites',
      lat: [receiver.latitude, transmitter.latitude], lon: [receiver.longitude, transmitter.longitude],
      text: [receiver.name || 'Receiver', transmitter.name || 'Illuminator'],
      textposition: ['bottom right', 'top right'],
      marker: {size: [13, 13], color: ['#f3eee9', '#f7c75f']},
      hovertemplate: '%{text}<br>%{lat:.5f}, %{lon:.5f}<extra></extra>'
    });
    if (aircraft.length) traces.push({
      type: 'scattermapbox', mode: 'markers', name: 'ADS-B evaluation truth',
      lat: aircraft.map(item => item.lat), lon: aircraft.map(item => item.lon),
      marker: {size: 24, color: 'rgba(77,212,176,.32)'},
      customdata: aircraft.map(item => [String(item.flight || item.hex || 'Aircraft').trim(), item.alt_geom ?? item.alt_baro ?? null, item.seen_pos]),
      hovertemplate: '<b>%{customdata[0]}</b><br>ADS-B evaluation truth<br>Altitude %{customdata[1]} ft<br>Position age %{customdata[2]:.1f} s<extra></extra>'
    });

    const configuredDelayBins = Math.max(0, finite(config?.process?.ambiguity?.delayMax) ?? 0);
    const configuredExcessKm = configuredDelayBins * resolution;
    const evaluationRadiusKm = Math.max(0, finite(config?.truth?.adsb?.display_range_km) ?? 0);
    const viewportKey = [receiver.latitude, receiver.longitude, transmitter.latitude,
      transmitter.longitude, configuredExcessKm, evaluationRadiusKm].join(':');
    if (!locationMapViewport || locationMapViewport.key !== viewportKey) {
      const spanKm = Math.max(10, baseline + configuredExcessKm, evaluationRadiusKm * 2);
      locationMapViewport = {
        key: viewportKey,
        center: {lat: (receiver.latitude + transmitter.latitude) / 2,
          lon: (receiver.longitude + transmitter.longitude) / 2},
        zoom: Math.max(4, Math.min(12, 13 - Math.log2(spanKm)))
      };
    }
    document.getElementById('view-summary').innerHTML = `${card(tracks.length, 'Current radar tracks')}${card(rawDetections.length, 'Unconfirmed returns hidden')}${card(hiddenCoastingTracks, 'Coasting tracks hidden')}${card(aircraft.length, 'ADS-B aircraft shown')}`;
    document.getElementById('view-detail').innerHTML = `<div class="view-note">Orange ellipses show current radar tracks. Green markers show ADS-B planes.</div>`;
    if (locationMapIsBeingNavigated()) return;
    await Plotly.react('data', traces, {
      ...plotBase, margin: {l: 8, r: 8, t: 8, b: 8},
      uirevision: locationMapViewport.key,
      mapbox: {style: 'open-street-map', center: locationMapViewport.center,
        zoom: locationMapViewport.zoom, uirevision: locationMapViewport.key},
      legend: {orientation: 'h', x: 0, y: 1.03, bgcolor: 'rgba(20,16,14,.82)'}
    }, plotConfig);
    renderAircraftOverlays(aircraft);
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
    const aircraft = feed && typeof feed === 'object' && !replayTruthDisabled ?
      Object.entries(feed).map(([id, item]) => ({id, ...item})) : [];
    const radar = detectionRows(detection);
    const truth = config?.truth?.adsb || {};
    document.getElementById('view-summary').innerHTML = `${card(replayTruthDisabled ? 'Not replayed' : enabled ? (feedError ? 'Offline' : 'Online') : 'Disabled', 'ADS-B evaluation feed', feedError ? 'bad' : enabled && !replayTruthDisabled ? 'good' : '')}${card(aircraft.length, 'Aircraft in current feed')}${card(radar.length, 'Radar detections')}${card(finite(truth.display_range_km) === null ? 'Configured source range' : `${number(truth.display_range_km, 0)} km`, 'Evaluation radius')}`;
    const detail = document.getElementById('view-detail');
    if (!enabled) {
      detail.innerHTML = empty('ADS-B evaluation is disabled', 'Enable the ADS-B evaluation feed in Settings to use this page.', '<a class="button-link" href="/display/configuration">Open Settings</a>');
      await emptyPlot('ADS-B evaluation is disabled.');
      setState('Disabled', 'warning');
      return;
    }
    if (replayTruthDisabled) {
      detail.innerHTML = empty('ADS-B truth is not replayed', 'Live aircraft and delay–Doppler truth are hidden during replay.');
      await emptyPlot('Live ADS-B truth is hidden during replay.');
      setState('Not replayed', 'warning');
      return;
    }
    if (feedError) {
      detail.innerHTML = empty('ADS-B evaluation feed is unavailable', 'Radar processing remains independent and continues normally.');
      await emptyPlot('ADS-B feed unavailable; radar detections are not affected.');
      setState('ADS-B offline', 'bad');
      return;
    }
    const aircraftRows = aircraft.map(item => {
      const age = sourceAgeSeconds(item.timestamp);
      return `<tr><td>${escapeHtml(item.flight || item.callsign || item.hex || item.id)}</td><td>${number(item.delay, 2)}</td><td>${number(item.doppler, 1)}</td><td>${age === null ? '—' : `${number(age, 1)} s`}</td></tr>`;
    });
    detail.innerHTML = aircraft.length ? table(['Aircraft', 'Bistatic range (km)', 'Doppler (Hz)', 'Position age'], aircraftRows) : empty('Feed online; no aircraft in the current result', 'The configured source returned a valid empty set.');
    const traces = [
      {x: radar.map(item => item.delay), y: radar.map(item => item.doppler), type: 'scatter', mode: 'markers', name: 'Radar detections', marker: {size: 10, color: colors[0]}, customdata: radar.map(item => item.snr), hovertemplate: 'Radar<br>Range %{x:.2f} km<br>Doppler %{y:.1f} Hz<br>SNR %{customdata:.1f} dB<extra></extra>'},
      {x: aircraft.map(item => finite(item.delay)), y: aircraft.map(item => finite(item.doppler)), type: 'scatter', mode: 'markers+text', name: 'ADS-B evaluation truth', text: aircraft.map(item => item.flight || item.callsign || ''), textposition: 'top center', marker: {size: 10, color: colors[1], symbol: 'circle-open', line: {width: 2}}, hovertemplate: '%{text}<br>Range %{x:.2f} km<br>Doppler %{y:.1f} Hz<extra></extra>'}
    ];
    await Plotly.react('data', traces, {...plotBase, xaxis: {title: 'Bistatic range (km)', gridcolor: '#47362e'}, yaxis: {title: 'Bistatic Doppler (Hz)', gridcolor: '#47362e'}}, plotConfig);
    setState(`Evaluation feed online · ${timeLabel(detection?.timestamp)}`, 'good');
  }

  async function renderTracks() {
    setupPlotScaffold();
    const [config, tracker] = await Promise.all([getConfig(), apiJson('/api/tracker')]);
    const enabled = config?.process?.tracker?.enable === true;
    const tracks = array(tracker?.data);
    document.getElementById('view-summary').innerHTML = `${card(tracker?.nActive ?? 0, 'Active')}${card(tracker?.nAssociated ?? 0, 'Associated')}${card(tracker?.nCoasting ?? 0, 'Coasting')}${card(tracker?.nTentative ?? 0, 'Tentative')}`;
    const detail = document.getElementById('view-detail');
    if (!enabled) {
      detail.innerHTML = empty('Tracking is disabled', 'Enable the tracker in Settings and restart radar processing.', '<a class="button-link" href="/display/configuration">Open Settings</a>');
      await emptyPlot('Tracking is disabled.');
      setState('Disabled', 'warning');
      return;
    }
    const rows = tracks.map(track => `<tr><td>${escapeHtml(track.id)}</td><td><span class="state-pill ${escapeHtml(String(track.state || '').toLowerCase())}">${escapeHtml(humanize(track.state))}</span></td><td>${number(trackRangeKm(track.delay, config), 2)}</td><td>${number(track.doppler, 1)}</td><td>${integer(track.n)}</td></tr>`);
    detail.innerHTML = tracks.length ? table(['Track', 'State', 'Bistatic range (km)', 'Doppler (Hz)', 'Observations'], rows) : empty(tracker?.nTentative ? 'Tracks are being initiated' : 'No persistent tracks', tracker?.nTentative ? `${tracker.nTentative} tentative hypothesis${tracker.nTentative === 1 ? '' : 'es'} have not yet met the configured promotion rule.` : 'No detection sequence currently meets the configured tracking rules.');
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

  const renderers = {overview: renderOverview, detections: renderDetections, health: renderHealth, activity: renderActivity, site: renderSite, locations: renderLocations, evaluation: renderEvaluation, tracks: renderTracks};

  async function refresh() {
    if (refreshing || !renderers[view]) return;
    refreshing = true;
    try {
      await renderers[view]();
    } catch (error) {
      window.blah2ViewLastError = error?.message || String(error);
      setState('Data unavailable', 'bad');
      if (!root.dataset.ready || !root.children.length) root.innerHTML = empty('This view cannot reach its live data', error.message || 'Check the radar API connection.');
    } finally {
      refreshing = false;
    }
  }

  startRadarUpdates(refresh);
}());
