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
      for (const attribute of Array.from(current.attributes)) {
        if (!next.hasAttribute(attribute.name)) current.removeAttribute(attribute.name);
      }
      for (const attribute of Array.from(next.attributes)) {
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
    renderHtml(root, `<div class="insight-stats insight-stats-four">
      ${card(detections.length, 'Detections in latest CPI')}
      ${card(tracks.length, 'Persistent tracks')}
      ${card(strongest ? `${number(strongest.snr, 1)} dB` : 'None', 'Strongest detection')}
      ${card(load === null ? '—' : `${number(load, 0)}%`, 'Load', load > 100 ? 'bad' : load > 80 ? 'warning' : 'good')}
    </div><div class="overview-grid">
      <section class="info-card"><h2>Radar</h2><dl><div><dt>Receiver</dt><dd>${escapeHtml(receiver)}</dd></div><div><dt>Illuminator</dt><dd>${escapeHtml(transmitter)}</dd></div><div><dt>Radio</dt><dd>${escapeHtml(config?.capture?.device?.type || 'Receiver')} · ${escapeHtml(frequencyLabel(config))}</dd></div><div><dt>Inputs</dt><dd>${escapeHtml(channelCount(config) ?? 'Unknown')} channels</dd></div></dl></section>
      <section class="info-card"><h2>Current activity</h2>${strongest ? `<dl><div><dt>Bistatic range</dt><dd>${number(strongest.delay, 2)} km</dd></div><div><dt>Doppler</dt><dd>${number(strongest.doppler, 1)} Hz</dd></div><div><dt>Signal-to-noise ratio</dt><dd>${number(strongest.snr, 1)} dB</dd></div><div><dt>Frame</dt><dd>${escapeHtml(timeLabel(detection?.timestamp))}</dd></div></dl>` : empty('No detections in the latest CPI', 'The radar is running; no return currently passes the configured detector.')}</section>
      <section class="info-card overview-links"><h2>Open a view</h2><a href="/display/locations">Possible locations <b>→</b></a><a href="/display/detections">Current detections <b>→</b></a><a href="/display/tracks">Tracks <b>→</b></a><a href="/display/health">System health <b>→</b></a><a href="/display/site">Site geometry <b>→</b></a></section>
    </div>`);
    setState(`Updated ${timeLabel(timing?.timestamp)}`, 'good');
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
    const stages = Object.entries(timing || {}).filter(([key, value]) => typeof value === 'number' && finite(value) !== null && !['timestamp', 'nCpi', 'uptime_s', 'uptime_days', 'cpi_data_ms', 'cpi_hop_ms', 'cpi_overlap_fraction', 'capture_backlog_ms', 'cpi'].includes(key)).sort((left, right) => Number(right[1]) - Number(left[1]));
    renderHtml(root, `<div class="insight-stats">${card(overall, 'Overall state', overallTone)}${card(load === null ? '—' : `${number(load, 0)}%`, 'Load', loadTone)}${card(`${channelCount(config) ?? '—'}`, 'Channels')}${card(timing?.uptime_s === undefined ? '—' : `${number(timing.uptime_s / 3600, 1)} h`, 'Uptime')}</div><div class="health-grid"><section class="health-list">${healthRow('Radar frames', ageMs === null ? 'Unavailable' : ageMs < 1000 ? 'Live' : `${number(ageMs / 1000, 1)} s old`, `Updates every ${number(hop, 0)} ms`, freshnessTone)}${healthRow('Load', load === null ? 'Unavailable' : `${number(load, 0)}%`, processMs === null ? 'No timing data' : `${number(processMs, 1)} ms per ${number(hop, 1)} ms update`, loadTone)}${healthRow('Capture backlog', backlog === null ? 'Unavailable' : `${number(backlog, 1)} ms`, 'Captured data waiting to be processed', backlogTone)}${healthRow('Detection', config?.process?.detection?.enable === false ? 'Disabled' : 'Enabled', 'Shows current detections', config?.process?.detection?.enable === false ? 'neutral' : 'good')}${healthRow('Tracking', trackerEnabled ? 'Enabled' : 'Disabled', trackerEnabled ? `${integer(tracker?.n ?? 0)} track candidates` : 'Optional processing is off', trackerTone)}${healthRow('ADS-B', adsb.disabledForReplay ? 'Not replayed' : !adsb.enabled ? 'Disabled' : adsb.online ? 'Online' : 'Offline', adsb.disabledForReplay ? 'Not shown during replay.' : adsb.enabled ? 'For comparison only; not used by radar.' : 'Optional feed is off', adsbTone)}</section><section class="info-card"><h2>Stage times</h2>${table(['Stage', 'Latest time (ms)'], stages.map(([name, value]) => `<tr><td>${escapeHtml(humanize(name))}</td><td>${number(value, 2)}</td></tr>`))}</section></div>`);
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

  function renderAircraftOverlays(aircraft, sites = []) {
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
      // Plotly's raster OSM style has no glyph source. Keep site labels in
      // the existing HTML overlay instead of requesting unsupported symbols.
      for (const site of sites) {
        const marker = document.createElement('div');
        marker.className = 'location-site-label';
        marker.textContent = site.name;
        Object.assign(marker.style, {position: 'absolute', margin: '10px 0 0 12px',
          padding: '2px 5px', borderRadius: '4px', color: '#f3eee9',
          background: 'rgba(16,20,18,.84)', font: '600 12px/1.3 Inter,system-ui,sans-serif',
          whiteSpace: 'nowrap'});
        layer.appendChild(marker);
        markers.push({item: {lat: site.latitude, lon: site.longitude}, marker});
      }
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
      renderHtml(document.getElementById('view-summary'), card('Unavailable', 'Site coordinates'));
      renderDetail(empty('Possible locations are unavailable', 'Configure receiver and illuminator coordinates in Settings.'));
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
    let bearingLocations = null;
    let bearingDiagnostic = null;
    if (config?.process?.bearing?.enable === true) {
      const [locationsResult, diagnosticResult] = await Promise.allSettled([
        apiJson('/api/locations'), apiJson('/api/bearing/diagnostic')]);
      bearingLocations = locationsResult.status === 'fulfilled' ? locationsResult.value :
        {enabled: true, available: false, data: []};
      bearingDiagnostic = diagnosticResult.status === 'fulfilled' ? diagnosticResult.value : null;
    }

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
    const showStationary = new URLSearchParams(window.location.search).get('showStationary') === '1';
    const stationaryIds = new Set(array(bearingLocations?.data).filter(item => item?.stationary_clutter).map(item => item.id));
    const visibleTracks = showStationary ? tracks : tracks.filter(item => !stationaryIds.has(item.id));
    const traces = visibleTracks.map(item => ellipseTrace(item));
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
    if (aircraft.length) traces.push({
      type: 'scattermapbox', mode: 'markers', name: 'ADS-B',
      lat: aircraft.map(item => item.lat), lon: aircraft.map(item => item.lon),
      marker: {size: 24, color: 'rgba(77,212,176,.32)'},
      customdata: aircraft.map(item => [String(item.flight || item.hex || 'Aircraft').trim(), item.alt_geom ?? item.alt_baro ?? null, item.seen_pos]),
      hovertemplate: '<b>%{customdata[0]}</b><br>ADS-B<br>Altitude %{customdata[1]} ft<br>Position age %{customdata[2]:.1f} s<extra></extra>'
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
    const resolvedBearings = bearingRows.filter(item => item.location).length;
    const unresolvedBearings = bearingRows.length - resolvedBearings;
    renderHtml(document.getElementById('view-summary'), `${card(bearingRows.length, 'Bearing estimates')}${card(resolvedBearings, 'Resolved fixes')}${card(unresolvedBearings, 'Unresolved shown')}${card(hiddenStationary, 'Stationary hidden')}${card(aircraft.length, 'ADS-B')}`);
    const reference = bearingDiagnostic?.known_angle_reference;
    const referenceDiagnostic = !reference ? '' : reference.available ?
      `<section class="info-card"><h2>Known-angle reference · shadow only</h2><dl>
        <div><dt>RF source</dt><dd>${escapeHtml(reference.source?.name || 'Configured transmitter')} · ${number(reference.source?.bearing_deg_true, 3)}° true</dd></div>
        <div><dt>Configured fit</dt><dd>Heading ${number(reference.configured?.heading_deg_true, 1)}° · phase error ${number(reference.configured?.median_phase_error_deg, 1)}° median / ${number(reference.configured?.p95_phase_error_deg, 1)}° p95</dd></div>
        <div><dt>Best mapping at configured heading</dt><dd>${escapeHtml(array(reference.best_mapping_at_configured_heading?.mapping).join(' → ') || 'Unavailable')} · phase error ${number(reference.best_mapping_at_configured_heading?.median_phase_error_deg, 1)}°</dd></div>
        <div><dt>Fitted phase axis with configured mapping</dt><dd>${number(reference.heading_with_configured_mapping?.heading_deg_true, 0)}° true (${number(reference.heading_with_configured_mapping?.signed_correction_deg, 0)}° model offset) · phase error ${number(reference.heading_with_configured_mapping?.median_phase_error_deg, 1)}°</dd></div>
        <div><dt>RF evidence</dt><dd>${integer(reference.quality?.updates)} updates · coherence ${number(reference.quality?.coherent_fraction_median, 3)} median · amplitude span ${number(reference.quality?.amplitude_span_db_median, 1)} dB median</dd></div>
      </dl>${reference.quality?.reliable === false ? '<p class="view-note">This source is excluded from multi-reference calibration by the RF quality gate.</p>' : ''}<p class="view-note">${escapeHtml(reference.limitation)}</p></section>` :
      `<section class="info-card"><h2>Known-angle reference</h2><p>${escapeHtml(reference.reason)}</p></section>`;
    const diagnostic = !bearingDiagnostic ? '' : bearingDiagnostic.available ?
      `<section class="info-card"><h2>Bearing wiring diagnostic · shadow only</h2><dl>
        <div><dt>Evidence</dt><dd>${integer(bearingDiagnostic.samples)} samples · ${integer(bearingDiagnostic.aircraft)} aircraft · ${integer(bearingDiagnostic.sectors)}/8 sectors</dd></div>
        <div><dt>Active mapping</dt><dd>${escapeHtml(array(bearingDiagnostic.active?.mapping).join(' → ') || 'Unavailable')} · median error ${number(bearingDiagnostic.active?.median_error_deg, 1)}°</dd></div>
        <div><dt>Best replay</dt><dd>${escapeHtml(array(bearingDiagnostic.best?.mapping).join(' → ') || 'Unavailable')} · median error ${number(bearingDiagnostic.best?.median_error_deg, 1)}°</dd></div>
        <div><dt>Heading-only correction</dt><dd>${number(bearingDiagnostic.global_heading_bias?.signed_correction_deg, 0)}° · median error ${number(bearingDiagnostic.global_heading_bias?.median_error_deg, 1)}°</dd></div>
        <div><dt>Decision</dt><dd>${bearingDiagnostic.evidence_ready ? 'Evidence threshold met for review' : 'Still collecting coverage'}; changes are never applied automatically.</dd></div>
      </dl><p class="view-note">${escapeHtml(bearingDiagnostic.identifiability)}</p></section>` :
      `<section class="info-card"><h2>Bearing wiring diagnostic</h2><p>${escapeHtml(bearingDiagnostic.reason)}</p></section>`;
    renderDetail(`<div class="view-note">Orange ellipses show current delay-only tracks. Bright gold fixes and corridors passed the strict bearing confidence test. Purple markers are best unresolved estimates, and translucent gold points are their alternate lobes. Persistent stationary clutter is hidden by default but remains in the raw API and can be inspected with <code>?showStationary=1</code>. Green ADS-B markers are validation overlays and never select a radar fix. Raw untracked detections remain hidden.</div>${diagnostic}${referenceDiagnostic}`);
    if (locationMapIsBeingNavigated()) return;
    await Plotly.react('data', traces, {
      ...plotBase, margin: {l: 8, r: 8, t: 8, b: 8},
      uirevision: locationMapViewport.key,
      mapbox: {style: 'open-street-map', center: locationMapViewport.center,
        zoom: locationMapViewport.zoom, uirevision: locationMapViewport.key},
      legend: {orientation: 'h', x: 0, y: 1.03, bgcolor: 'rgba(20,16,14,.82)'}
    }, plotConfig);
    renderAircraftOverlays(aircraft, [
      {...receiver, name: receiver.name || 'Receiver'},
      {...transmitter, name: transmitter.name || 'Illuminator'}
    ]);
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
      {x: aircraft.map(item => finite(item.delay)), y: aircraft.map(item => finite(item.doppler)), type: 'scatter', mode: 'markers+text', name: 'ADS-B', text: aircraft.map(item => item.flight || item.callsign || ''), textposition: 'top center', marker: {size: 10, color: colors[1], symbol: 'circle-open', line: {width: 2}}, hovertemplate: '%{text}<br>Range %{x:.2f} km<br>Doppler %{y:.1f} Hz<extra></extra>'}
    ];
    await Plotly.react('data', traces, {...plotBase, xaxis: {title: 'Bistatic range (km)', gridcolor: '#47362e'}, yaxis: {title: 'Bistatic Doppler (Hz)', gridcolor: '#47362e'}}, plotConfig);
    setState(`ADS-B online · ${timeLabel(detection?.timestamp)}`, 'good');
  }

  async function renderTracks() {
    setupPlotScaffold();
    const [config, tracker] = await Promise.all([getConfig(), apiJson('/api/tracker')]);
    const enabled = config?.process?.tracker?.enable === true;
    const tracks = array(tracker?.data);
    renderHtml(document.getElementById('view-summary'), `${card(tracker?.nActive ?? 0, 'Active')}${card(tracker?.nAssociated ?? 0, 'Associated')}${card(tracker?.nCoasting ?? 0, 'Coasting')}${card(tracker?.nTentative ?? 0, 'Tentative')}`);
    const detail = document.getElementById('view-detail');
    if (!enabled) {
      renderHtml(detail, empty('Tracking is disabled', 'Enable the tracker in Settings and restart radar processing.', '<a class="button-link" href="/display/configuration">Open Settings</a>'));
      await emptyPlot('Tracking is disabled.');
      setState('Disabled', 'warning');
      return;
    }
    const rows = tracks.map(track => `<tr><td>${escapeHtml(track.id)}</td><td><span class="state-pill ${escapeHtml(String(track.state || '').toLowerCase())}">${escapeHtml(humanize(track.state))}</span></td><td>${number(trackRangeKm(track.delay, config), 2)}</td><td>${number(track.doppler, 1)}</td><td>${integer(track.n)}</td></tr>`);
    renderHtml(detail, tracks.length ? table(['Track', 'State', 'Bistatic range (km)', 'Doppler (Hz)', 'Observations'], rows) : empty(tracker?.nTentative ? 'Waiting for track confirmation' : 'No confirmed tracks', tracker?.nTentative ? `${tracker.nTentative} tentative track${tracker.nTentative === 1 ? ' needs' : 's need'} more detections.` : 'No detection sequence meets the tracking rules.'));
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
    const trackingRows = periods.map(([label, item = {}]) => `<tr><td>${label}</td><td>${integer(item.out_of_config_passes)}</td><td>${integer(item.eligible_passes)}</td><td>${integer(item.eligible_no_radar_return)}</td><td>${integer(item.detected_not_tracked)}</td><td>${integer(item.tracked_passes)}</td><td>${percent(item.tracking_yield, 1)}</td></tr>`);
    const exclusionRows = periods.map(([label, item = {}]) => `<tr><td>${label}</td><td>${integer(item.out_of_span?.below_detection_floor)}</td><td>${integer(item.out_of_span?.above_delay_ceiling)}</td><td>${integer(item.out_of_span?.outside_doppler_span)}</td><td>${integer(item.out_of_span?.stale_truth)}</td><td>${integer(item.out_of_span?.missing_exact_timing)}</td><td>${integer(item.out_of_span?.beyond_clutter_cancellation)}</td></tr>`);
    const activeRows = array(statistics.active_passes).map(item => `<tr><td>${escapeHtml(item.flight || item.hex)}</td><td>${escapeHtml(humanize(item.status))}</td><td>${integer(item.eligible_cpis)}</td><td>${integer(item.detected_cpis)}</td><td>${integer(item.tracked_cpis)}</td><td>${number(item.last_truth?.delay_km, 2)}</td><td>${number(item.last_truth?.doppler_hz, 1)}</td></tr>`);
    const finalizedRows = array(statistics.recent_finalized_passes).map(item => `<tr><td>${escapeHtml(item.flight || item.hex)}</td><td>${escapeHtml(humanize(item.outcome))}</td><td>${integer(item.eligible_cpis)}</td><td>${integer(item.detected_cpis)}</td><td>${integer(item.tracked_cpis)}</td><td>${timeLabel(item.finalized_at)}</td></tr>`);
    const quality = statistics.data_quality || {};
    const mix = statistics.mix_adjusted_previous_hour_comparison || {};
    const mixRows = [['Current hour', mix.current], ['Previous hour', mix.baseline]]
      .map(([label, item = {}]) => `<tr><td>${label}</td><td>${percent(item.detection_yield, 1)}</td><td>${percent(item.tracking_yield, 1)}</td></tr>`);
    const stratumRows = (definition, bands) => Object.entries(bands || {}).map(([label, item]) =>
      `<tr><td>${escapeHtml(definition)}</td><td>${escapeHtml(label)}</td><td>${integer(item.eligible_passes)}</td><td>${integer(item.detected_passes)}</td><td>${integer(item.tracked_passes)}</td><td>${percent(item.tracking_yield, 1)}</td></tr>`);
    const geometryRows = [
      ...stratumRows('Excess delay km', hour.delay_strata?.bands),
      ...stratumRows('Receiver slant km', hour.receiver_slant_range_strata?.bands)];
    renderHtml(root, `${decisionGrade ? '' : `<div class="evaluation-note"><strong>Provisional timing:</strong> ${escapeHtml(statistics.warning || 'Receiver time is not hardware-UTC-exact.')} Authoritative scorecard remains fail-closed (${integer(authoritative?.data_quality?.event_time_rejections)} rejected timing frames).</div>`}<div class="insight-stats insight-stats-four">
      ${card(integer(lifetime.eligible_passes), 'Eligible aircraft passes')}
      ${card(percent(lifetime.tracking_yield, 1), decisionGrade ? 'Lifetime tracking yield' : 'Provisional tracking yield', decisionGrade ? undefined : 'warning')}
      ${card(integer(statistics.active_passes?.length), 'Aircraft live now')}
      ${card(`${integer(quality.exact_timed_cpi_frames)}/${integer(quality.unique_cpi_frames)}`, 'Exact-timed CPI frames', quality.missing_timing_cpis ? 'warning' : 'good')}
    </div><div class="overview-grid">
      <section class="info-card"><h2>Tracking score</h2>${table(['Window', 'Out of config', 'Eligible', 'No radar return', 'Detected, not tracked', 'Tracked', 'Raw tracking yield'], trackingRows)}<p class="view-note">Raw yield describes the traffic that happened to fly through each window; it is not a regression verdict. Out-of-config passes never enter the performance denominator. “No radar return” isolates eligible aircraft with neither strict detections nor tracks. “Detected, not tracked” isolates tracker/initiation loss. Detection and tracking credit each require at least three unambiguous strict delay–Doppler matches.</p></section>
      <section class="info-card"><h2>Traffic-mix controlled comparison</h2>${table(['Period', 'Standardized detection', 'Standardized tracking'], mixRows)}<dl><div><dt>Common-support coverage</dt><dd>${percent(mix.common_support_coverage, 1)}</dd></div><div><dt>Common geometry strata</dt><dd>${integer(mix.common_strata?.length)}</dd></div><div><dt>Comparison grade</dt><dd>${mix.decision_grade ? 'Decision-capable' : 'Descriptive only'}</dd></div></dl><p class="view-note">Current and previous hours are directly standardized to the same pooled excess-delay × 3-D receiver-slant distribution. A surge of 45 km aircraft therefore cannot by itself look like a detector regression. Coverage below 80%, sparse strata, or inexact receiver timing blocks a regression verdict.</p></section>
      <section class="info-card"><h2>Current-hour geometry strata</h2>${geometryRows.length ? table(['Geometry', 'Band', 'Eligible', 'Detected', 'Tracked', 'Raw tracking yield'], geometryRows) : empty('Geometry strata collecting', 'A fresh scoring epoch needs at least three detector-searchable CPIs in a stratum.')}</section>
      <section class="info-card"><h2>Eligibility context</h2>${table(['Window', 'Below range floor', 'Above range ceiling', 'Outside Doppler', 'Stale truth', 'Missing timing', 'Beyond clutter canceller'], exclusionRows)}<dl><div><dt>Physical delay span</dt><dd>${number(limits.delayMinKm, 3)}–${number(limits.delayMaxKm, 3)} km</dd></div><div><dt>Doppler span</dt><dd>${number(limits.dopplerMinHz, 0)} to ${number(limits.dopplerMaxHz, 0)} Hz</dd></div><div><dt>Clutter cancellation</dt><dd>Through ${number(limits.clutterMaxKm, 3)} km</dd></div><div><dt>Strict match gate</dt><dd>±${number(limits.matchDelayKm, 2)} km / ±${number(limits.matchDopplerHz, 1)} Hz</dd></div></dl><p class="view-note">Missing-timing and invalid-truth opportunities are excluded, never counted as misses. Beyond-clutter-canceller opportunities remain physically searchable but are reported separately.</p></section>
      <section class="info-card"><h2>Aircraft live now</h2>${activeRows.length ? table(['Aircraft', 'Status', 'Eligible CPIs', 'Detected', 'Stable tracked', 'Delay km', 'Doppler Hz'], activeRows) : empty('No eligible aircraft live', 'Waiting for exact-timed ADS-B opportunities.')}</section>
      <section class="info-card"><h2>Recent finalized passes</h2>${finalizedRows.length ? table(['Aircraft', 'Outcome', 'Eligible CPIs', 'Detected', 'Stable tracked', 'Finalized'], finalizedRows) : empty('No finalized passes yet', 'Passes finalize after the aircraft leaves the eligible region.')}</section>
      <section class="info-card"><h2>Scoring integrity</h2><dl><div><dt>Decision grade</dt><dd>${decisionGrade ? 'Yes' : 'No — provisional operational lane'}</dd></div><div><dt>Timing policy</dt><dd>${escapeHtml(humanize(statistics.timing_policy))}</dd></div><div><dt>Association delay</dt><dd>${integer(statistics.association_lag_cpis)} CPIs · ${integer(statistics.association_lag_ms)} ms</dd></div><div><dt>UTC nuisance radius</dt><dd>±${number(statistics.provisional_event_time_uncertainty_s, 2)} s (scoring only)</dd></div><div><dt>Exact CPI frames</dt><dd>${integer(quality.exact_timed_cpi_frames)} / ${integer(quality.unique_cpi_frames)}</dd></div><div><dt>Aircraft-CPI rows</dt><dd>${integer(quality.exact_timed_aircraft_cpis)} / ${integer(quality.audited_aircraft_cpis)}</dd></div><div><dt>Pending delayed CPIs</dt><dd>${integer(quality.pending_delayed_cpis)}</dd></div><div><dt>Contract</dt><dd>${escapeHtml(String(statistics.evaluation_contract_hash || '').slice(0, 12))}</dd></div><div><dt>Nuisance accounting</dt><dd>${escapeHtml(humanize(quality.nuisance_accounting))}</dd></div></dl><p class="view-note">Live states are provisional until a pass finalizes. Tracking credit requires at least three ACTIVE, measurement-supported associations. Truth held, stale, or disagreeing beyond the contract is excluded rather than scored as a miss. The UTC nuisance radius expands validation gates using measured trajectory rates; it never steers radar processing. A provisional timing lane is never valid for acceptance decisions.</p></section>
    </div>`);
    setState(statistics.error ? `Collection warning · ${statistics.error}` :
      !decisionGrade ? 'Provisional receiver timing · authoritative scoring blocked' :
      statistics.updated_at ? `Updated ${timeLabel(statistics.updated_at)}` : 'Waiting for eligible ADS-B evidence',
    statistics.error || !decisionGrade ? 'warning' : statistics.updated_at ? 'good' : 'warning');
  }

  async function renderCalibration() {
    setupPlotScaffold('insight-content-wide');
    const campaign = await apiJson('/api/array-calibration/status');
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
        <div><dt>Native shadow evidence</dt><dd>${detectionAutomation.native_shadow_corpus?.error ? escapeHtml(detectionAutomation.native_shadow_corpus.error) : detectionAutomation.native_shadow_corpus ? `${integer(detectionAutomation.native_shadow_corpus.added)} new authenticated rows · shadow only` : 'Awaiting signed native exports'}</dd></div>
        <div><dt>Native 2-D shadow fit</dt><dd>${detectionAutomation.native_shadow_fit ? `${integer(detectionAutomation.native_shadow_fit.accepted_samples)} current-epoch rows · ${integer(detectionAutomation.native_shadow_fit.training_passes)} training / ${integer(detectionAutomation.native_shadow_fit.holdout_passes)} held-out passes · ${integer(detectionAutomation.native_shadow_fit.supported_cells)} supported cells` : 'Awaiting authenticated native training evidence'}</dd></div>
        <div><dt>Native A/B scope</dt><dd>Pass-disjoint complex projection only; nearby controls are not certified target-free, and no CFAR or detection improvement is claimed</dd></div>
        <div><dt>Rejected native receipts</dt><dd>${integer(detectionAutomation.native_shadow_corpus?.rejected_receipts)}</dd></div>
        <div><dt>Production full-bank receipts</dt><dd>${integer(detectionProgress.production_replay?.imported_passes)} imported · physical paired A/B qualification still required</dd></div>
        <div><dt>Supported sectors / bins</dt><dd>${integer(detectionProgress.covered_sectors)} / ${integer(detectionProgress.fit_bins)}</dd></div>
        <div><dt>2-D candidate</dt><dd>${detectionAutomation.candidate_version ? `v${escapeHtml(String(detectionAutomation.candidate_version).slice(0, 12))} · azimuth + elevation` : 'Waiting for supported 2-D cells'}</dd></div>
        <div><dt>Support / uncertainty</dt><dd>Unsupported or sparse cells are not extrapolated; supported cells report independent-pass confidence</dd></div>
        <div><dt>Validation state</dt><dd>${detectionProgress.validation?.ready ? 'Independent replay coverage ready — still shadow only' : 'Independent replay coverage incomplete'}</dd></div>
        <div><dt>Last shadow update</dt><dd>${detectionAutomation.last_evaluation_at ? escapeHtml(timeLabel(detectionAutomation.last_evaluation_at)) : 'Not evaluated yet'}</dd></div>
        <div><dt>Active detection model</dt><dd>${detectionCampaign.active_model ? escapeHtml(detectionCampaign.active_model) : 'Ideal/live configuration unchanged'}</dd></div>
        <div><dt>Shadow A/B receipt</dt><dd>${detectionAutomation.candidate_version && detectionAutomation.shadow_ab_receipt_path ? 'Append-only receipt ledger' : 'Waiting for a candidate evaluation'}</dd></div>
        <div><dt>Promotion</dt><dd>Hard-disabled — never automatic</dd></div>
        <div><dt>Errors</dt><dd>${detectionAutomation.error ? escapeHtml(detectionAutomation.error) : 'None'}</dd></div>
      </dl><p class="view-note">${escapeHtml(detectionProgress.limitation || 'Detection-manifold fitting is an isolated, diagnostic-only shadow path.')}</p>${detectionFailureText.length ? `<p class="view-note evaluation-note">Qualification blocked: ${escapeHtml(detectionFailureText.join(' '))}</p>` : ''}</section>
    </div><section class="info-card"><h2>Qualification gates</h2>${table(['Requirement', 'Target', 'Current', 'State'], requirementRows)}</section>
    <section class="info-card"><h2>Azimuth-sector corpus</h2>${table(['Local azimuth', 'Samples', 'Passes', 'Aircraft', 'Coverage'], sectorRows)}</section>`);
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
    if (refreshing || !renderers[view]) return;
    refreshing = true;
    try {
      await renderers[view]();
      window.blah2ViewLastError = null;
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
