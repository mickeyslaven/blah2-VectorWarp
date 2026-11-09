(function () {
  'use strict';

  const root = document.getElementById('view-root');
  const state = document.getElementById('view-state');
  const view = document.body.dataset.insightView;
  const currentNode = () => typeof selectedNode === 'function' ? selectedNode() : 'local';
  const getLaneContext = typeof getIlluminatorContext === 'function' ?
    getIlluminatorContext : async () => ({enabled: false, id: null, item: null, illuminators: []});
  const laneScope = typeof illuminatorDisplayScope === 'function' ?
    illuminatorDisplayScope : () => ['health', 'calibration'].includes(view) ? 'aggregate' : 'selected';
  const laneUrl = typeof selectedIlluminatorUrl === 'function' ?
    selectedIlluminatorUrl : path => path;
  const laneConfig = typeof selectedIlluminatorConfig === 'function' ?
    selectedIlluminatorConfig : config => config;
  const atRadarFrame = typeof radarFrameUrl === 'function' ? radarFrameUrl :
    (path, timestamp) => {
      if (!Number.isSafeInteger(Number(timestamp)) || Number(timestamp) < 0) return path;
      const url = new URL(path, window.location.href);
      url.searchParams.set('frame_timestamp', String(Number(timestamp)));
      return `${url.pathname}${url.search}`;
    };
  const associationGates = window.VectorWarpConfigProfiles?.associationGates;
  const assignRadarPoints = window.VectorWarpConfigProfiles?.assignments;
  const colors = ['#ff8754', '#4dd4b0', '#70a7ff', '#f7c75f', '#d995ff', '#ff8298'];
  const plotConfig = {responsive: true, displayModeBar: false, scrollZoom: true};
  const plotBase = {
    autosize: true,
    paper_bgcolor: 'rgba(0,0,0,0)',
    plot_bgcolor: 'rgba(0,0,0,0)',
    font: {color: '#f3eee9', family: 'Inter, system-ui, sans-serif',
      size: document.body?.classList?.contains?.('distance-view') ? 18 :
        typeof window !== 'undefined' && window.innerWidth <= 600 ? 13 : 14},
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
  let statisticsDirectionSelection = {azimuth: '0-45', elevation: '0-2'};
  // The statistics page is a long-lived kiosk dashboard, not a CPI display.
  // Rebuilding its rich network-wide cards every five seconds needlessly
  // churned Chromium's renderer and could eventually leave the kiosk on an
  // "Aw, Snap!" page. Thirty seconds keeps operational totals current while
  // cutting the sustained render workload by 83%.
  const minimumRefreshIntervalMs = view === 'statistics' ? 30000 : 2000;
  const kioskRefreshIntervalMs = 4 * 60 * 60 * 1000;

  // A dashboard may remain open for weeks. Give Chromium a clean renderer at
  // a quiet, bounded interval instead of allowing long-lived graphics and font
  // allocations to accumulate indefinitely. Interactive/mobile statistics
  // pages are deliberately left alone.
  if (view === 'statistics' && document.body.classList?.contains('distance-view'))
    window.setTimeout?.(() => window.location.reload(), kioskRefreshIntervalMs);

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

  async function fetchJson(url, options = {}, cachePolicy = {}) {
    if (typeof fetchCachedJsonResource === 'function')
      return fetchCachedJsonResource(url, options, cachePolicy);
    const response = await fetchStatusResource(url, {cache: 'no-store', ...options});
    if (!response.ok) throw new Error(`${url} returned ${response.status}`);
    const text = await response.text();
    return text.trim() ? JSON.parse(text) : null;
  }

  async function apiJson(path, {awaitFresh = false} = {}) {
    const context = await getLaneContext();
    const selectedProduct = laneScope() === 'selected' &&
      /^\/api\/(?:detection|tracker|bearing|locations|adsb\/delay-doppler|live-scorecard(?:\/provisional)?|statistics|radar\/coverage)(?:[/?]|$)/.test(path);
    const productPath = selectedProduct ? laneUrl(path, context) : path;
    const aligned = /^\/api\/(?:adsb(?:\/delay-doppler)?|detection|tracker|bearing|locations|timing|iqdata)(?:[/?]|$)/.test(productPath) ?
      radarPresentationUrl(productPath) : productPath;
    const result = await fetchJson(liveApiUrl(aligned), {}, awaitFresh ?
      {freshMs: 0, awaitFresh: true} : {});
    if (selectedProduct && (await getLaneContext()).id !== context.id)
      throw new Error('Channel changed during refresh');
    return result;
  }

  async function allChannelJson(path) {
    return fetchJson(liveApiUrl(path));
  }

  async function federatedNodeJson(path, fleet) {
    const nodes = fleet?.nodes?.length ? fleet.nodes :
      [{id: 'local', label: 'Local', local: true}];
    const settled = await Promise.allSettled(nodes.map(async node => {
      const prefix = node.local ? '' :
        `/api/federation/nodes/${encodeURIComponent(node.id)}`;
      // A reachable API is not necessarily an observing radar.  Never merge a
      // stopped node's retained scorecard into a live network headline: doing
      // so gives old outages the same weight as the current common session.
      const [status, value] = await Promise.all([
        fetchJson(liveApiUrl(`${prefix}/api/system/status?summary=1`)),
        fetchJson(liveApiUrl(`${prefix}${path}`))
      ]);
      if (status?.radar !== 'receiving' || status?.processorFresh !== true)
        throw new Error(`${node.label || node.id} is not receiving fresh radar frames`);
      return {node, value};
    }));
    return settled.filter(result => result.status === 'fulfilled')
      .map(result => result.value);
  }

  function networkAssociationAggregate(rows, fleet) {
    const channels = [];
    for (const {node, value} of rows) for (const source of array(value?.channels)) {
      const id = source.identity?.illuminator || source.selected_channel || 'primary';
      const catalog = array(fleet?.channels).find(channel => channel.nodeId === node.id &&
        (channel.item?.id === id || id === 'primary'));
      const channelName = (catalog?.item?.id || id) === 'primary' ? 'Primary' :
        (catalog?.item?.id || id);
      const display = `${channelName} (${node.label})`;
      channels.push({...source, node_id: node.id, node_label: node.label,
        channel_key: `${node.id}:${channelName}`, display_channel: display,
        identity: {...(source.identity || {}), illuminator: display}});
    }
    const sum = getter => channels.reduce((total, channel) =>
      total + (Number(getter(channel)) || 0), 0);
    const eligible = sum(channel => channel.association_statistics?.eligible_adsb_passes);
    const detected = sum(channel => channel.association_statistics?.detection?.associated_passes);
    const tracked = sum(channel => channel.association_statistics?.tracking?.associated_passes);
    const audited = sum(channel => channel.association_statistics?.timing?.audited_aircraft_cpis);
    const exact = sum(channel => channel.association_statistics?.timing?.exact_timed_aircraft_cpis);
    const groups = [];
    const mergeBands = (target, source) => {
      for (const [name, band] of Object.entries(source || {})) {
        const merged = target[name] ||= {...band, eligible: false, detected: false,
          tracked: false};
        merged.eligible ||= band.eligible === true;
        merged.detected ||= band.detected === true;
        merged.tracked ||= band.tracked === true;
      }
    };
    for (const channel of channels) for (const pass of array(channel.passes)) {
      if (!pass?.hex || !Number.isFinite(Number(pass.started_at))) continue;
      const start = Number(pass.started_at);
      const end = Number(pass.finalized_at) || start;
      let group = groups.find(item => item.hex === String(pass.hex).toLowerCase() &&
        start <= item.end && end >= item.start);
      if (!group) {
        group = {hex: String(pass.hex).toLowerCase(), start, end, detected: false,
          tracked: false, channels: {}, azimuth_bands: {},
          azimuth_elevation_bands: {}};
        groups.push(group);
      }
      group.start = Math.min(group.start, start); group.end = Math.max(group.end, end);
      const channelResult = group.channels[channel.channel_key] ||= {eligible: true,
        detected: false, tracked: false, label: channel.display_channel};
      channelResult.detected ||= pass.detected === true;
      channelResult.tracked ||= pass.tracked === true;
      group.detected ||= pass.detected === true; group.tracked ||= pass.tracked === true;
      mergeBands(group.azimuth_bands, pass.azimuth_bands);
      mergeBands(group.azimuth_elevation_bands, pass.azimuth_elevation_bands);
    }
    const channelKeys = channels.map(channel => channel.channel_key);
    const labelByKey = Object.fromEntries(channels.map(channel =>
      [channel.channel_key, channel.display_channel]));
    const contribution = metric => {
      const only = Object.fromEntries(channels.map(channel => [channel.display_channel, 0]));
      let shared = 0, uncaught = 0;
      for (const pass of groups) {
        const caught = Object.entries(pass.channels).filter(([, value]) => value[metric])
          .map(([key]) => key);
        if (!caught.length) uncaught++;
        else if (caught.length > 1) shared++;
        else only[labelByKey[caught[0]]]++;
      }
      return {shared_by_multiple_channels: shared, only_by_channel: only,
        not_caught: uncaught};
    };
    const channelContribution = channels.map(channel => {
      const rowsForChannel = groups.map(pass => pass.channels[channel.channel_key]).filter(Boolean);
      const unique = metric => groups.filter(pass => pass.channels[channel.channel_key]?.[metric] &&
        Object.entries(pass.channels).every(([key, value]) =>
          key === channel.channel_key || !value[metric])).length;
      return {channel: channel.display_channel, eligible_passes: rowsForChannel.length,
        eligible_only_on_channel: groups.filter(pass => pass.channels[channel.channel_key] &&
          Object.keys(pass.channels).length === 1).length,
        detected_passes: rowsForChannel.filter(item => item.detected).length,
        tracked_passes: rowsForChannel.filter(item => item.tracked).length,
        unique_detection_contribution: unique('detected'),
        unique_tracking_contribution: unique('tracked'),
        leave_one_out_detection_loss: unique('detected'),
        leave_one_out_tracking_loss: unique('tracked')};
    });
    const summarizeBands = field => {
      const result = {};
      const cutoff = Date.now() - 24 * 60 * 60 * 1000;
      for (const pass of groups.filter(item => item.end >= cutoff))
        for (const [name, band] of Object.entries(pass[field] || {})) {
          if (!band.eligible) continue;
          const value = result[name] ||= {...band, eligible_passes: 0,
            detected_passes: 0, tracked_passes: 0};
          value.eligible_passes++;
          if (band.detected) value.detected_passes++;
          if (band.tracked) value.tracked_passes++;
        }
      for (const value of Object.values(result)) {
        value.detection_yield = value.eligible_passes ?
          value.detected_passes / value.eligible_passes : null;
        value.tracking_yield = value.eligible_passes ?
          value.tracked_passes / value.eligible_passes : null;
        delete value.eligible; delete value.detected; delete value.tracked;
      }
      return result;
    };
    const populations = channels.map(channel => channel.track_population || {});
    const populationBands = {};
    for (const population of populations)
      for (const [name, count] of Object.entries(population.similarity_bands || {}))
        populationBands[name] = (populationBands[name] || 0) + (Number(count) || 0);
    const population = {schema: 'vectorwarp-track-population-network-v1',
      scope: 'sum-of-node-channel-track-cohorts',
      retained_tracks: populations.reduce((total, item) => total +
        (Number(item.retained_tracks) || 0), 0),
      adsb_associated_tracks: populations.reduce((total, item) => total +
        (Number(item.adsb_associated_tracks) || 0), 0),
      unassociated_tracks: populations.reduce((total, item) => total +
        (Number(item.unassociated_tracks) || 0), 0),
      empirical_reference_tracks: populations.reduce((total, item) => total +
        (Number(item.empirical_reference_tracks) || 0), 0),
      similarity_bands: populationBands,
      aircraft_like_candidates: channels.flatMap(channel =>
        array(channel.track_population?.aircraft_like_candidates).map(candidate =>
          ({...candidate, channel: channel.display_channel, node: channel.node_label})))
        .sort((left, right) => (right.similarity?.score || 0) -
          (left.similarity?.score || 0)).slice(0, 50),
      channels: channels.map(channel => ({channel: channel.display_channel,
        node: channel.node_label, ...(channel.track_population || {})}))};
    const uniqueEligible = groups.length;
    const uniqueDetected = groups.filter(pass => pass.detected).length;
    const uniqueTracked = groups.filter(pass => pass.tracked).length;
    const unique = {scope: 'unique-aircraft-passes-caught-by-any-node-or-channel',
      aggregation: 'deduplicated-aircraft-pass-union',
      cross_channel_aircraft_deduplicated: true,
      eligible_adsb_passes: uniqueEligible,
      detection: {associated_passes: uniqueDetected,
        eligible_adsb_passes: uniqueEligible,
        yield: uniqueEligible ? uniqueDetected / uniqueEligible : null},
      tracking: {associated_passes: uniqueTracked,
        detected_passes: uniqueDetected,
        yield: uniqueDetected ? uniqueTracked / uniqueDetected : null,
        tracked_of_eligible_yield: uniqueEligible ? uniqueTracked / uniqueEligible : null},
      channel_contribution: {eligible_aircraft_passes: uniqueEligible,
        detection: contribution('detected'), tracking: contribution('tracked'),
        channels: channelContribution},
      directional_coverage: {azimuth_strata: {definition: 'receiver_true_azimuth_deg',
        bands: summarizeBands('azimuth_bands')}, azimuth_elevation_strata: {
        definition: 'receiver_true_azimuth_x_geometric_elevation',
        bands: summarizeBands('azimuth_elevation_bands')}}};
    return {schema: 'vectorwarp-network-scorecard-v1', aggregate: true,
      network_node_count: new Set(channels.map(channel => channel.node_id)).size,
      updated_at: Math.max(0, ...channels.map(channel => Number(channel.updated_at) || 0)),
      timing_policy: 'provisional', illuminators: channels,
      overall_association_statistics: {scope: 'all-node-channel-opportunities',
        eligible_adsb_passes: eligible,
        detection: {associated_passes: detected, eligible_adsb_passes: eligible,
          yield: eligible ? detected / eligible : null},
        tracking: {associated_passes: tracked, detected_passes: detected,
          yield: detected ? tracked / detected : null,
          tracked_of_eligible_yield: eligible ? tracked / eligible : null},
        timing: {exact_timed_aircraft_cpis: exact, audited_aircraft_cpis: audited,
          exact_fraction: audited ? exact / audited : null},
        track_population: population, unique_aircraft_passes: unique}};
  }

  async function getConfig() {
    const config = await getRadarRuntimeConfig();
    return laneScope() === 'selected' ? laneConfig(config) : config;
  }

  function selectedLaneLabel() {
    return window.vectorwarpIlluminatorContext?.id || 'radar';
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
    const surveillance = array(device.surveillance_channels);
    if (surveillance.length) return new Set([
      finite(device.reference_channel) ?? 0, ...surveillance.map(Number)]).size;
    return null;
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
    return `<div class="view-empty"><strong>${escapeHtml(title)}</strong>${detail ? `<span>${escapeHtml(detail)}</span>` : ''}${action}</div>`;
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
    root.innerHTML = `<section class="insight-content insight-content-wide location-map-layout"><div class="panel insight-plot-card location-map-panel"><div class="visualization insight-plot" id="data"></div><div class="location-map-drawers"><details class="location-map-sidebar location-map-controls"><summary aria-label="Map controls"><span class="map-sidebar-icon" aria-hidden="true"><svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="3"></circle><path d="M19.4 15a1.7 1.7 0 0 0 .34 1.88l.06.06-2.83 2.83-.06-.06A1.7 1.7 0 0 0 15 19.4a1.7 1.7 0 0 0-1 .6 1.7 1.7 0 0 0-.4 1.1V21h-4v-.1A1.7 1.7 0 0 0 8.6 19.4a1.7 1.7 0 0 0-1.88.34l-.06.06-2.83-2.83.06-.06A1.7 1.7 0 0 0 4.6 15a1.7 1.7 0 0 0-.6-1 1.7 1.7 0 0 0-1.1-.4H3v-4h.1A1.7 1.7 0 0 0 4.6 8.6a1.7 1.7 0 0 0-.34-1.88l-.06-.06 2.83-2.83.06.06A1.7 1.7 0 0 0 9 4.6a1.7 1.7 0 0 0 1-.6 1.7 1.7 0 0 0 .4-1.1V3h4v.1A1.7 1.7 0 0 0 15.4 4.6a1.7 1.7 0 0 0 1.88-.34l.06-.06 2.83 2.83-.06.06A1.7 1.7 0 0 0 19.4 9c.13.38.35.72.64 1 .3.27.68.41 1.08.4H21v4h-.1a1.7 1.7 0 0 0-1.5.6Z"></path></svg></span><span class="map-sidebar-title">Options</span><span class="map-sidebar-selected">Map layers</span></summary><div class="location-map-sidebar-body"><div class="map-controls-body"><label class="map-coverage-filter"><span>Background</span><select id="map-background-mode"><option value="aerial">Aerial imagery + roads</option><option value="street">Street map + terrain</option></select><small>Local aerial imagery is used where an offline pack is installed.</small></label><label class="map-coverage-filter"><span>Radar ellipses</span><select id="radar-map-overlay-mode"><option value="off">Hidden</option><option value="all">Show confirmed tracks</option></select><small>Hiding ellipses does not stop radar association.</small></label><label class="map-coverage-filter"><span>ADS-B aircraft</span><select id="adsb-map-filter-mode"><option value="all">Show all</option><option value="range">All within selected range</option><option value="radar">All within observed radar range</option><option value="tracked">Only radar-tracked</option></select><small>Map-only filter; ingestion and statistics are unchanged.</small></label><label class="map-coverage-filter map-adsb-range"><span>Maximum range</span><span class="map-range-input"><input id="adsb-map-range-km" type="number" min="1" max="1000" step="1" inputmode="decimal"><span>km</span></span><small>Receiver-to-aircraft ground range.</small></label><label class="map-auto-select"><input type="checkbox" id="show-radar-coverage-on-map"><span><strong>24 h observed radar reach</strong><small>Confirmed track sectors only; gaps mean no evidence.</small></span></label><label class="map-coverage-filter"><span>Coverage altitude</span><select id="radar-coverage-altitude"><option value="all">All altitudes</option><option value="low">0–10,000 ft</option><option value="medium">10–20,000 ft</option><option value="high">20–30,000 ft</option><option value="very_high">30–40,000 ft</option><option value="extreme">40,000+ ft</option></select></label><label class="map-auto-select"><input type="checkbox" id="auto-select-radar-aircraft"><span><strong>Auto-select radar matches</strong><small>Keep the current radar-associated aircraft selected.</small></span></label><div class="aircraft-altitude-legend" aria-label="Aircraft altitude colors"><strong>Altitude</strong><span class="aircraft-altitude-gradient"></span><span>Ground</span><span>0</span><span>10k</span><span>20k</span><span>30k</span><span>40k+ ft</span></div></div><div class="insight-stats" id="view-summary"></div><div id="view-detail"></div></div></details><details class="location-map-sidebar location-aircraft-sidebar"><summary aria-label="Aircraft details"><span class="map-sidebar-icon" aria-hidden="true"><svg viewBox="0 0 24 24"><path d="M2 14v-2l8-3V3c0-1.1.9-2 2-2s2 .9 2 2v6l8 3v2l-8-1v5l3 2v2l-5-1-5 1v-2l3-2v-5l-8 1Z"></path></svg></span><span class="map-sidebar-title">Aircraft</span><span class="map-sidebar-selected" id="map-sidebar-selected">Select a plane</span></summary><div class="location-map-sidebar-body"><div id="selected-aircraft-detail" aria-live="polite"></div></div></details></div></div></section>`;
    root.dataset.ready = 'true';
    const controlsDrawer = root.querySelector?.('.location-map-controls');
    const aircraftDrawer = root.querySelector?.('.location-aircraft-sidebar');
    if (controlsDrawer) controlsDrawer.open = !window.matchMedia?.('(max-width:760px)')?.matches;
    if (aircraftDrawer) aircraftDrawer.open = false;
    [controlsDrawer, aircraftDrawer].forEach(drawer => drawer?.addEventListener?.('toggle', () => {
      if (!drawer.open) return;
      const other = drawer === controlsDrawer ? aircraftDrawer : controlsDrawer;
      if (other) other.open = false;
    }));
    const radarOverlayMode = document.getElementById('radar-map-overlay-mode');
    const adsbFilterMode = document.getElementById('adsb-map-filter-mode');
    if (adsbFilterMode) adsbFilterMode.innerHTML = '<option value="all">All aircraft</option><option value="tracked">Radar tracked</option><option value="none">None</option>';
    root.querySelector?.('.map-adsb-range')?.remove();
    const backgroundMode = document.getElementById('map-background-mode');
    const showCoverage = document.getElementById('show-radar-coverage-on-map');
    const coverageAltitude = document.getElementById('radar-coverage-altitude');
    const autoSelect = document.getElementById('auto-select-radar-aircraft');
    const syncTrackControls = () => {
      if (radarOverlayMode) radarOverlayMode.value = showRadarTracksOnMap ? 'all' : 'off';
      if (adsbFilterMode) adsbFilterMode.value = adsbMapFilterMode;
      if (showCoverage) showCoverage.checked = showRadarCoverageOnMap;
      if (coverageAltitude) {
        coverageAltitude.value = radarCoverageAltitudeBand;
        coverageAltitude.disabled = !showRadarCoverageOnMap;
      }
      if (autoSelect) {
        autoSelect.checked = autoSelectRadarAircraft;
        autoSelect.disabled = false;
        autoSelect.closest?.('label')?.classList?.remove('disabled');
      }
    };
    syncTrackControls();
    if (backgroundMode) backgroundMode.value = mapBackgroundMode;
    backgroundMode?.addEventListener?.('change', () => {
      mapBackgroundMode = backgroundMode.value === 'street' ? 'street' : 'aerial';
      try { localStorage.setItem('vectorwarp-map-background-mode', mapBackgroundMode); }
      catch (_) { /* Persistence is optional. */ }
      syncMapBackground(document.getElementById('data')?._vectorwarpMap);
    });
    radarOverlayMode?.addEventListener?.('change', () => {
      showRadarTracksOnMap = radarOverlayMode.value === 'all';
      try {
        localStorage.setItem('vectorwarp-show-radar-tracks-on-map', String(showRadarTracksOnMap));
      }
      catch (_) { /* Persistence is optional. */ }
      syncTrackControls();
      renderLocations().catch(error => { window.blah2ViewLastError = error?.message || String(error); });
    });
    adsbFilterMode?.addEventListener?.('change', () => {
      adsbMapFilterMode = ['all', 'tracked', 'none'].includes(adsbFilterMode.value) ?
        adsbFilterMode.value : 'all';
      try { localStorage.setItem('vectorwarp-adsb-map-filter-mode', adsbMapFilterMode); }
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
      text: message, showarrow: false, font: {color: '#ac9e94', size: 16}
    }]}, plotConfig);
  }

  async function renderOverview() {
    const context = await getLaneContext();
    const aggregate = laneScope() === 'aggregate' && context.enabled;
    const fleet = typeof getFederatedChannels === 'function' ?
      await getFederatedChannels({fresh: true}).catch(() => null) : null;
    const channels = fleet?.channels?.length ? fleet.channels : context.illuminators.map(item =>
      ({nodeId: currentNode(), nodeLabel: 'Local', local: currentNode() === 'local', item}));
    const laneFrames = async path => {
      const settled = await Promise.allSettled(channels.map(channel =>
        fetchJson(typeof federatedChannelUrl === 'function' ?
          federatedChannelUrl(radarPresentationUrl(path), channel) :
          liveApiUrl(radarPresentationUrl(laneUrl(path,
            {...context, id: channel.item.id, item: channel.item}))), {},
        {freshMs: 0, awaitFresh: true})));
      return settled.filter(result => result.status === 'fulfilled').map(result => result.value);
    };
    const statusRows = await Promise.all((fleet?.nodes || [{id: 'local', label: 'Local', local: true}])
      .map(async node => {
        const prefix = node.local ? '' : `/api/federation/nodes/${encodeURIComponent(node.id)}`;
        try { return {node, status: await fetchJson(`${prefix}/api/system/status?summary=1`, {},
          {freshMs: 0, awaitFresh: true})}; }
        catch (_) { return {node, status: null}; }
      }));
    const [config, timing, detectionsByLane] = await Promise.all([
      getConfig(), apiJson('/api/timing'),
      aggregate ? laneFrames('/api/detection') : apiJson('/api/detection').then(value => [value])]);
    const trackersByLane = aggregate ? await laneFrames('/api/tracker') :
      await apiJson('/api/tracker').then(value => [value]);
    const detection = detectionsByLane[0] || {};
    const tracker = trackersByLane[0] || {};
    const adsb = await adsbHealth(config);
    const detections = detectionsByLane.flatMap(detectionRows);
    const tracks = trackersByLane.flatMap(value => array(value?.data));
    const strongest = [...detections].sort((left, right) => (right.snr ?? -Infinity) - (left.snr ?? -Infinity))[0];
    const hop = finite(config?.process?.data?.cpi) === null ? null : config.process.data.cpi * 1000;
    const load = hop && finite(timing?.cpi) !== null ? timing.cpi / hop * 100 : null;
    const health = healthSnapshot(config, timing, tracker, adsb);
    const nodeCards = statusRows.map(({node, status}) => {
      const nodeChannels = channels.filter(channel => channel.nodeId === node.id).length;
      const processor = status?.processor?.state || status?.processor?.input || 'unknown';
      return `<article class="health-row"><span class="health-indicator ${status?.radar === 'receiving' ? 'good' : status ? 'warning' : 'bad'}"></span><div><strong>${escapeHtml(node.label)}</strong><small>${nodeChannels} channel${nodeChannels === 1 ? '' : 's'} · ${escapeHtml(processor)}</small></div><b>${escapeHtml(status?.radar || 'Unavailable')}</b></article>`;
    }).join('');
    renderHtml(root, `<div class="insight-stats insight-stats-four">
      ${card(detections.length, aggregate ? `Detections across ${channels.length} channels` : `${selectedLaneLabel()} detections in latest CPI`)}
      ${card(tracks.length, aggregate ? `Persistent tracks across ${statusRows.length} nodes` : `${selectedLaneLabel()} persistent tracks`)}
      ${card(strongest ? `${number(strongest.snr, 1)} dB` : 'None', 'Strongest detection')}
      ${card(load === null ? '—' : `${number(load, 0)}%`, 'Load', load > 100 ? 'bad' : load > 80 ? 'warning' : 'good')}
    </div><div class="overview-grid controller-overview-grid">
      <section class="overview-health"><header><div><h2>All-lane pipeline health</h2></div><span class="state-pill ${health.overallTone === 'good' ? 'active' : health.overallTone === 'warning' ? 'coasting' : ''}">${escapeHtml(health.overall)}</span></header><div class="health-list">${health.rows}</div><a class="button-link" href="/display/diagnostics?view=health">Open diagnostics</a></section>
      <section class="overview-health"><header><div><h2>Local node fleet</h2></div><span class="state-pill active">${statusRows.filter(row => row.status).length}/${statusRows.length} online</span></header><div class="health-list">${nodeCards}</div></section>
    </div>`);
    setState(`Updated ${timeLabel(timing?.timestamp)}`, health.overallTone);
  }

  async function renderDetections() {
    const detection = await apiJson('/api/detection');
    const rows = detectionRows(detection).sort((left, right) => (right.snr ?? -Infinity) - (left.snr ?? -Infinity));
    const strongest = rows[0];
    const body = rows.map(item => `<tr><td>${item.index + 1}</td><td>${number(item.delay, 2)}</td><td>${number(item.doppler, 1)}</td><td>${number(item.snr, 1)}</td></tr>`);
    renderHtml(root, `<div class="insight-stats">${card(rows.length, 'Latest CPI')}${card(strongest ? `${number(strongest.snr, 1)} dB` : 'None', 'Strongest return')}</div>${rows.length ? table(['#', 'Bistatic range (km)', 'Doppler (Hz)', 'SNR (dB)'], body) : empty('No detections', '')}`);
    setState(`Frame ${timeLabel(detection?.timestamp)}`, 'good');
  }

  function healthRow(name, value, detail, tone) {
    return `<article class="health-row"><span class="health-indicator ${tone}"></span><div><strong>${escapeHtml(name)}</strong>${detail ? `<small>${escapeHtml(detail)}</small>` : ''}</div><b>${escapeHtml(value)}</b></article>`;
  }

  function healthSnapshot(config, timing, tracker, adsb) {
    const configuredCpi = finite(config?.process?.data?.cpi);
    const hop = configuredCpi === null ? null : configuredCpi * 1000;
    const processMs = finite(timing?.cpi);
    const load = processMs !== null && hop > 0 ? processMs / hop * 100 : null;
    const backlog = finite(timing?.capture_backlog_ms);
    const frameAge = finite(timing?.frame_age_ms);
    const presentationDelay = finite(timing?.presentation_delay_ms) ?? 0;
    const ageMs = finite(timing?.frame_excess_age_ms) ?? (frameAge !== null ?
      Math.max(0, frameAge - presentationDelay) :
      (finite(timing?.timestamp) === null ? null :
        Math.max(0, Date.now() - timing.timestamp - presentationDelay)));
    const freshLimit = Math.max(2500, (hop ?? 0) * 3);
    const freshnessTone = ageMs === null || ageMs > 10000 ? 'bad' : ageMs > freshLimit ? 'warning' : 'good';
    // Finishing inside the configured CPI is healthy. Backlog growth is the
    // stronger signal of sustained overload; do not alarm on normal 420–455 ms
    // GPU processing within a 500 ms cadence.
    const loadTone = load === null ? 'neutral' : load > 120 ? 'bad' : load > 100 ? 'warning' : 'good';
    const backlogTone = backlog === null || hop === null ? 'neutral' :
      backlog > hop * 2 ? 'bad' : backlog > hop * 1.25 ? 'warning' : 'good';
    const trackerEnabled = config?.process?.tracker?.enable === true;
    const trackerTone = trackerEnabled ? (tracker ? 'good' : 'bad') : 'neutral';
    const adsbTone = !adsb.enabled ? 'neutral' : adsb.online ? 'good' : 'bad';
    const tones = [freshnessTone, loadTone, backlogTone, trackerTone];
    if (adsb.enabled) tones.push(adsbTone);
    const overall = tones.includes('bad') ? 'Attention needed' : tones.includes('warning') ? 'Monitor' : 'Healthy';
    const overallTone = tones.includes('bad') ? 'bad' : tones.includes('warning') ? 'warning' : 'good';
    const frameDetail = [hop === null ? '' : `${number(hop, 0)} ms cadence`,
      presentationDelay > 0 ? `${number(presentationDelay / 1000, 1)} s aligned hold` : '']
      .filter(Boolean).join(' · ');
    const backlogDetail = backlog === null || !(hop > 0) ? '' : `${number(backlog / hop, 2)} CPI queued`;
    const rows = healthRow('Radar frames', ageMs === null ? 'Unavailable' : ageMs < 1000 ? 'Live' : `${number(ageMs / 1000, 1)} s late`, frameDetail, freshnessTone) +
      healthRow('Processing load', load === null ? 'Unavailable' : `${number(load, 0)}%`, processMs === null ? '' : `${number(processMs, 1)} / ${number(hop, 1)} ms`, loadTone) +
      healthRow('Capture backlog', backlog === null ? 'Unavailable' : `${number(backlog, 1)} ms`, backlogDetail, backlogTone) +
      healthRow('Tracking', trackerEnabled ? 'Enabled' : 'Disabled', trackerEnabled ? `${integer(tracker?.n ?? 0)} track candidates` : 'Optional processing is off', trackerTone) +
      healthRow('ADS-B', adsb.disabledForReplay ? 'Not replayed' : !adsb.enabled ? 'Disabled' : adsb.online ? 'Online' : 'Offline', adsb.enabled ? 'Comparison only' : '', adsbTone);
    return {hop, processMs, load, backlog, overall, overallTone, rows};
  }

  async function renderHealth() {
    const [config, timing, tracker] = await Promise.all([getConfig(), apiJson('/api/timing'), apiJson('/api/tracker')]);
    const adsb = await adsbHealth(config);
    const health = healthSnapshot(config, timing, tracker, adsb);
    const stages = Object.entries(timing || {}).filter(([key, value]) => typeof value === 'number' && finite(value) !== null && !['timestamp', 'nCpi', 'uptime_s', 'uptime_days', 'cpi_data_ms', 'cpi_hop_ms', 'cpi_overlap_fraction', 'capture_backlog_ms', 'cpi'].includes(key)).sort((left, right) => Number(right[1]) - Number(left[1]));
    renderHtml(root, `<p class="view-note"><strong>All-lane pipeline:</strong> health, timing, backlog, and load cover the complete multi-illuminator processor.</p><div class="insight-stats">${card(health.overall, 'Overall state', health.overallTone)}${card(health.load === null ? '—' : `${number(health.load, 0)}%`, 'Load', health.load > 120 ? 'bad' : health.load > 100 ? 'warning' : 'good')}${card(`${channelCount(config) ?? '—'}`, 'Channels')}${card(timing?.uptime_s === undefined ? '—' : `${number(timing.uptime_s / 3600, 1)} h`, 'Uptime')}</div><div class="health-grid"><section class="health-list">${health.rows}${healthRow('Detection', config?.process?.detection?.enable === false ? 'Disabled' : 'Enabled', '', config?.process?.detection?.enable === false ? 'neutral' : 'good')}</section><section class="info-card"><h2>Stage times</h2>${table(['Stage', 'Latest time (ms)'], stages.map(([name, value]) => `<tr><td>${escapeHtml(humanize(name))}</td><td>${number(value, 2)}</td></tr>`))}</section></div>`);
    setState(`Updated ${timeLabel(timing?.timestamp)}`, health.overallTone);
  }

  let activityHistory = [];
  let activityChannel = null;
  function loadActivityHistory(id) {
    activityChannel = id;
    try { activityHistory = JSON.parse(sessionStorage.getItem(
      `blah2-activity-history:${currentNode()}:${id}`) || '[]')
      .filter(item => Date.now() - item.time < 3600000); } catch (_) { activityHistory = []; }
  }

  async function renderActivity() {
    setupPlotScaffold();
    const [config, detection, tracker, timing] = await Promise.all([getConfig(), apiJson('/api/detection'), apiJson('/api/tracker'), apiJson('/api/timing')]);
    const rows = detectionRows(detection);
    const timestamp = finite(detection?.timestamp) ?? Date.now();
    const hop = finite(config?.process?.data?.cpi) === null ? null : config.process.data.cpi * 1000;
    const load = hop && finite(timing?.cpi) !== null ? timing.cpi / hop * 100 : null;
    const channel = selectedLaneLabel();
    if (activityChannel !== channel) loadActivityHistory(channel);
    if (!activityHistory.length || activityHistory[activityHistory.length - 1].time !== timestamp) {
      activityHistory.push({time: timestamp, detections: rows.length, tracks: array(tracker?.data).length, snr: rows.reduce((best, item) => Math.max(best, item.snr ?? -Infinity), -Infinity), load});
      activityHistory = activityHistory.slice(-300);
      try { sessionStorage.setItem(`blah2-activity-history:${currentNode()}:${channel}`,
        JSON.stringify(activityHistory)); } catch (_) { /* Storage is optional. */ }
    }
    const latest = activityHistory[activityHistory.length - 1] || {};
    renderHtml(document.getElementById('view-summary'), `${card(latest.detections ?? 0, 'Latest detections')}${card(latest.tracks ?? 0, 'Persistent tracks')}${card(Number.isFinite(latest.snr) ? `${number(latest.snr, 1)} dB` : 'None', 'Strongest return')}${card(activityHistory.length, 'Samples this browser session')}`);
    renderDetail(`<div class="view-note"><strong>Selected channel: ${escapeHtml(selectedLaneLabel())}.</strong> Detection and track history is collected in this browser tab. Processing load is the all-lane pipeline load.</div>`);
    const x = activityHistory.map(item => new Date(item.time));
    const traces = [
      {x, y: activityHistory.map(item => item.detections), name: 'Detections', type: 'scatter', mode: 'lines', line: {color: colors[0], width: 2}, yaxis: 'y'},
      {x, y: activityHistory.map(item => item.tracks), name: 'Persistent tracks', type: 'scatter', mode: 'lines', line: {color: colors[1], width: 2}, yaxis: 'y'},
      {x, y: activityHistory.map(item => Number.isFinite(item.snr) ? item.snr : null), name: 'Strongest SNR', type: 'scatter', mode: 'lines', line: {color: colors[2], width: 2}, yaxis: 'y2'},
      {x, y: activityHistory.map(item => item.load), name: 'Processing load', type: 'scatter', mode: 'lines', line: {color: colors[3], width: 2}, yaxis: 'y3'}
    ];
    const compact = typeof window !== 'undefined' && window.innerWidth <= 600;
    await Plotly.react('data', traces, {...plotBase,
      margin: compact ? {l: 55, r: 18, t: 42, b: 46} : {l: 72, r: 45, t: 34, b: 55},
      legend: {...plotBase.legend, font: {size: compact ? 10 : 14}},
      xaxis: {gridcolor: '#47362e', tickformat: compact ? '%H:%M:%S' : undefined,
        nticks: compact ? 4 : undefined, tickangle: 0, automargin: true},
      yaxis: {title: 'Count', domain: [.7, 1], rangemode: 'tozero', gridcolor: '#47362e'},
      yaxis2: {title: 'SNR (dB)', domain: [.35, .62], gridcolor: '#47362e'},
      yaxis3: {title: 'Load (%)', domain: [0, .27], rangemode: 'tozero', gridcolor: '#47362e'}}, plotConfig);
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

  let locationMapViewport = null;
  let selectedAircraftId = null;
  let selectedAircraftSnapshot = null;
  let autoSelectRadarAircraft = false;
  let showRadarTracksOnMap = true;
  let adsbMapFilterMode = 'all';
  let showRadarCoverageOnMap = true;
  let radarCoverageAltitudeBand = 'all';
  let radarCoverageCache = null;
  let mapBackgroundMode = 'aerial';
  try { autoSelectRadarAircraft = localStorage.getItem('vectorwarp-auto-select-radar-aircraft') === 'true'; }
  catch (_) { /* Persistence is optional. */ }
  try {
    const defaultsVersion = localStorage.getItem('vectorwarp-map-display-defaults');
    if (defaultsVersion === 'show-all-v1') {
      showRadarTracksOnMap = localStorage.getItem('vectorwarp-show-radar-tracks-on-map') !== 'false';
      const savedAircraftFilter = localStorage.getItem('vectorwarp-adsb-map-filter-mode');
      adsbMapFilterMode = ['all', 'tracked', 'none'].includes(savedAircraftFilter) ?
        savedAircraftFilter : 'all';
    } else {
      // Replace the earlier opt-in defaults once. Subsequent choices remain
      // personal browser preferences.
      showRadarTracksOnMap = true;
      adsbMapFilterMode = 'all';
      localStorage.setItem('vectorwarp-show-radar-tracks-on-map', 'true');
      localStorage.setItem('vectorwarp-adsb-map-filter-mode', 'all');
      localStorage.removeItem('vectorwarp-adsb-map-range-km');
      localStorage.setItem('vectorwarp-map-display-defaults', 'show-all-v1');
    }
  } catch (_) { /* Persistence is optional. */ }
  try { mapBackgroundMode = localStorage.getItem('vectorwarp-map-background-mode') || 'aerial'; }
  catch (_) { /* Persistence is optional. */ }
  try {
    const stored = localStorage.getItem('vectorwarp-show-radar-coverage-on-map');
    showRadarCoverageOnMap = stored === null ? true : stored === 'true';
    radarCoverageAltitudeBand = localStorage.getItem('vectorwarp-radar-coverage-altitude') || 'all';
  } catch (_) { /* Persistence is optional. */ }
  const aircraftTrackHistories = new Map();
  const aircraftTrailStorageKey = `vectorwarp-aircraft-trails-v1:${currentNode()}`;
  try {
    const saved = JSON.parse(localStorage.getItem(aircraftTrailStorageKey) || '{}');
    const cutoff = Date.now() - 15 * 60 * 1000;
    for (const [id, points] of Object.entries(saved)) {
      const recent = Array.isArray(points) ? points.filter(point => Number(point.at) >= cutoff &&
        Number.isFinite(Number(point.lat)) && Number.isFinite(Number(point.lon))).slice(-180) : [];
      if (recent.length) aircraftTrackHistories.set(id, recent);
    }
  } catch (_) { /* Trail continuity is an optional browser cache. */ }
  let aircraftTrailPersistAt = 0;
  const aircraftPhotoCache = new Map();
  try {
    const saved = JSON.parse(localStorage.getItem('vectorwarp-aircraft-photos-v1') || '{}');
    const cutoff = Date.now() - 30 * 86400000;
    Object.entries(saved).forEach(([hex, entry]) => {
      if (/^[0-9a-f]{6}$/.test(hex) && Number(entry?.at) >= cutoff && entry?.value)
        aircraftPhotoCache.set(hex, entry.value);
    });
  } catch (_) { /* Photo continuity is an optional browser cache. */ }
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

  function filterMapAircraft(aircraft, coverage) {
    if (adsbMapFilterMode === 'tracked')
      return aircraft.filter(item => item.radar_track_id !== undefined &&
        item.radar_track_id !== null && item.radar_track_id !== '');
    if (adsbMapFilterMode === 'none') return [];
    return aircraft;
  }

  function selectedAircraftMarkup(item, photo = null) {
    if (!item) return `<section class="info-card aircraft-detail-card aircraft-detail-empty"><h2>Aircraft details</h2><div class="aircraft-detail-prompt">${aircraftSvgMarkup({})}<p>Tap an aircraft or drone to inspect its live identity and radar geometry.</p></div></section>`;
    const info = aircraftInfo(item);
    const remoteId = item.remote_id && typeof item.remote_id === 'object' ? item.remote_id : null;
    const photoMarkup = !remoteId && photo?.available ?
      `<figure class="aircraft-photo"><img src="${escapeHtml(photo.thumbnail)}" alt="${escapeHtml(info.label)} aircraft" loading="lazy" referrerpolicy="no-referrer"><figcaption>Photo by ${escapeHtml(photo.photographer || 'contributor')} · <a href="${escapeHtml(photo.link)}" target="_blank" rel="noopener noreferrer">Planespotters.net</a></figcaption></figure>` :
      `<div class="aircraft-photo aircraft-photo-fallback">${aircraftSvgMarkup(item)}<span>${remoteId ? 'Broadcast Remote ID' : photo?.loading ? 'Looking for a photo…' : photo?.enabled === false ? 'Aircraft photos are off in ADS-B settings' : 'No photo available'}</span></div>`;
    const remoteIdMarkup = remoteId ? `<details class="aircraft-detail-section" open><summary>Remote ID</summary><dl>
      <div><dt>UAS identity</dt><dd>${aircraftValue(remoteId.id)} · ${aircraftValue(humanize(remoteId.id_type))}</dd></div><div><dt>Drone type</dt><dd>${aircraftValue(remoteId.ua_type)}</dd></div>
      <div><dt>Broadcast transport</dt><dd>${array(remoteId.sources).length ? array(remoteId.sources).map(humanize).join(', ') : aircraftValue(humanize(remoteId.source))}</dd></div><div><dt>Purpose</dt><dd>${aircraftValue(remoteId.self_id)}</dd></div>
      <div><dt>${remoteId.operator?.location_type === 'takeoff' ? 'Take-off location' : 'Operator location'}</dt><dd>${Number.isFinite(remoteId.operator?.latitude) && Number.isFinite(remoteId.operator?.longitude) ? `${number(remoteId.operator.latitude, 5)}, ${number(remoteId.operator.longitude, 5)}` : 'Not broadcast yet'}</dd></div>
    </dl></details>` : '';
    return `<section class="info-card aircraft-detail-card"><header><div><span class="aircraft-detail-kind">${escapeHtml(humanize(aircraftKind(item)))}</span><h2>${escapeHtml(info.label)}</h2><span class="aircraft-detail-subtitle">${aircraftValue(info.registration)} · ${aircraftValue(info.type)}</span></div><button type="button" class="button-secondary aircraft-detail-close" aria-label="Close aircraft details">Close</button></header><div class="aircraft-primary-metrics">
      <div><strong>${info.altitude === null ? '—' : integer(info.altitude)}</strong><span>ft altitude</span></div><div><strong>${info.speed === null ? '—' : integer(info.speed)}</strong><span>kt speed</span></div><div><strong>${info.heading === null ? '—' : `${integer(info.heading)}°`}</strong><span>heading</span></div><div><strong>${info.verticalRate === null ? '—' : integer(info.verticalRate)}</strong><span>ft/min</span></div>
    </div>${photoMarkup}${remoteIdMarkup}<details class="aircraft-detail-section"><summary>Aircraft and source</summary><dl>
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
      .then(value => {
        aircraftPhotoCache.set(hex, value);
        try {
          const entries = [...aircraftPhotoCache.entries()].slice(-200);
          localStorage.setItem('vectorwarp-aircraft-photos-v1', JSON.stringify(Object.fromEntries(
            entries.map(([id, photo]) => [id, {at: Date.now(), value: photo}]))));
        } catch (_) { /* A full/private browser cache must not affect photos. */ }
        return value;
      })
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
    target.closest?.('.location-aircraft-sidebar')?.classList.toggle('has-aircraft-selection', Boolean(item));
    const sidebarLabel = document.getElementById('map-sidebar-selected');
    if (sidebarLabel) sidebarLabel.textContent = item ? aircraftInfo(item).label : 'Select a plane';
    const hex = item ? aircraftInfo(item).hex.toLowerCase() : '';
    renderHtml(target, selectedAircraftMarkup(item, hex ? aircraftPhotoCache.get(hex) : null));
    target.querySelector?.('.aircraft-detail-close')?.addEventListener('click', () => {
      selectedAircraftId = null; selectedAircraftSnapshot = null;
      renderSelectedAircraftCard(aircraft);
      const aircraftDrawer = document.querySelector?.('.location-aircraft-sidebar');
      if (aircraftDrawer) aircraftDrawer.open = false;
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

  const radarCoverageSourceId = 'vectorwarp-radar-coverage';
  const radarCoverageFillLayerId = 'vectorwarp-radar-coverage-fill';
  const radarCoverageLineLayerId = 'vectorwarp-radar-coverage-line';
  const bearingOverlaySourceId = 'vectorwarp-bearing-overlays';

  function locationMapStyle(imageryManifest = {}) {
    const sources = {
      'vectorwarp-basemap': {
        type: 'raster', tileSize: 256,
        tiles: ['https://a.tile.openstreetmap.org/{z}/{x}/{y}.png',
          'https://b.tile.openstreetmap.org/{z}/{x}/{y}.png'],
        attribution: '&copy; OpenStreetMap contributors'
      },
      'vectorwarp-terrain': {
        type: 'raster-dem', url: 'https://tiles.mapterhorn.com/tilejson.json',
        // Mapterhorn's TileJSON declares 512 px Terrarium tiles. A 256 px
        // override builds the wrong terrain mesh and exposes seams/clipped
        // wedges while pitching or rotating the camera.
        tileSize: 512, attribution: 'Terrain &copy; Mapterhorn'
      },
      // MapLibre warns against sharing a DEM source between terrain and
      // hillshade because the two render paths need independent tile state.
      'vectorwarp-hillshade-dem': {
        type: 'raster-dem', url: 'https://tiles.mapterhorn.com/tilejson.json',
        tileSize: 512, attribution: 'Terrain &copy; Mapterhorn'
      }
    };
    const validBounds = bounds => Array.isArray(bounds) && bounds.length === 4 &&
      bounds.map(Number).every(Number.isFinite);
    const advertisedTiers = Array.isArray(imageryManifest.zoom_tiers) ?
      imageryManifest.zoom_tiers : [];
    const tiers = advertisedTiers.length ? advertisedTiers : (imageryManifest.available ? [{
      id: 'local', bounds: imageryManifest.bounds,
      minzoom: imageryManifest.minzoom, maxzoom: imageryManifest.maxzoom
    }] : []);
    // "Aerial imagery + roads" must still show roads when this installation
    // has no local aerial pack. Previously the basemap was made transparent
    // even though there was no replacement raster layer, yielding a black map.
    const aerial = mapBackgroundMode === 'aerial' && tiers.length > 0;
    const layers = [
      {id: 'vectorwarp-background', type: 'background', paint: {'background-color': '#15110f'}},
      {id: 'vectorwarp-basemap', type: 'raster', source: 'vectorwarp-basemap',
        paint: {'raster-opacity': aerial ? 0 : 1,
          'raster-opacity-transition': {duration: 120, delay: 0}}}
    ];
    tiers.sort((left, right) => Number(left.minzoom) - Number(right.minzoom));
    tiers.forEach((tier, index) => {
      if (!validBounds(tier.bounds)) return;
      const slug = String(tier.id || `tier-${index + 1}`).replace(/[^a-z0-9-]/gi, '-').toLowerCase();
      const id = `vectorwarp-local-imagery-${slug}-${index}`;
      sources[id] = {
        type: 'raster', tileSize: 256, bounds: tier.bounds.map(Number),
        minzoom: Number(tier.minzoom), maxzoom: Number(tier.maxzoom),
        tiles: ['/api/map/imagery/local/{z}/{x}/{y}'],
        attribution: imageryManifest.attribution || 'Aerial imagery &copy; USGS, USDA, The National Map'
      };
      // Do not set a layer maxzoom: MapLibre then keeps overzooming the last
      // real tile as a stable parent while a denser nested tier is loading.
      layers.push({id, type: 'raster', source: id, minzoom: Number(tier.minzoom),
        paint: {'raster-opacity': aerial ? 1 : 0,
          'raster-opacity-transition': {duration: 120, delay: 0}}});
    });
    layers.push({
      id: 'vectorwarp-hillshade', type: 'hillshade', source: 'vectorwarp-hillshade-dem',
      paint: {'hillshade-exaggeration': aerial ? 0 : .34,
        'hillshade-exaggeration-transition': {duration: 120, delay: 0},
        'hillshade-shadow-color': '#30251f',
        'hillshade-highlight-color': '#f4d4ad', 'hillshade-accent-color': '#7d614f'}
    });
    return {
      version: 8,
      sources,
      terrain: {source: 'vectorwarp-terrain', exaggeration: 1.12},
      layers
    };
  }

  function syncMapBackground(map) {
    if (!map?.getLayer || !map?.setPaintProperty || map._vectorwarpBackgroundSyncPending) return;
    map._vectorwarpBackgroundSyncPending = true;
    const schedule = window.requestAnimationFrame || (callback => callback());
    schedule(() => {
      map._vectorwarpBackgroundSyncPending = false;
      const imageryLayers = (map.getStyle?.()?.layers || []).map(layer => layer.id)
        .filter(id => id.startsWith('vectorwarp-local-imagery-'));
      const aerial = mapBackgroundMode === 'aerial' && imageryLayers.length > 0;
      if (map.getLayer('vectorwarp-basemap'))
        map.setPaintProperty('vectorwarp-basemap', 'raster-opacity', aerial ? 0 : 1);
      for (const id of imageryLayers)
        map.setPaintProperty(id, 'raster-opacity', aerial ? 1 : 0);
      if (map.getLayer('vectorwarp-hillshade'))
        map.setPaintProperty('vectorwarp-hillshade', 'hillshade-exaggeration', aerial ? 0 : .34);
    });
  }

  async function ensureLocationMap(viewport) {
    const graph = document.getElementById('data');
    if (graph?._vectorwarpMap) return graph._vectorwarpMap;
    const maplibre = await (window.vectorwarpMapLibreReady || Promise.resolve(window.maplibregl));
    if (!maplibre?.Map) throw new Error('Map renderer could not load');
    let imageryManifest = {};
    try { imageryManifest = await apiJson('/api/map/imagery'); } catch (_) {}
    let savedCamera = null;
    try {
      const candidate = JSON.parse(localStorage.getItem('vectorwarp-map-camera-v1') || 'null');
      if (candidate?.key === viewport.key && Array.isArray(candidate.center) &&
          candidate.center.length === 2 && candidate.center.every(Number.isFinite)) savedCamera = candidate;
    } catch (_) { /* Camera persistence is optional. */ }
    const map = new maplibre.Map({
      container: graph,
      style: locationMapStyle(imageryManifest),
      center: savedCamera?.center || [viewport.center.lon, viewport.center.lat],
      zoom: Number.isFinite(savedCamera?.zoom) ? savedCamera.zoom : viewport.zoom,
      pitch: Number.isFinite(savedCamera?.pitch) ? savedCamera.pitch : 58,
      bearing: Number.isFinite(savedCamera?.bearing) ? savedCamera.bearing : 0,
      minPitch: 0,
      maxPitch: 85,
      attributionControl: true,
      cooperativeGestures: false,
      renderWorldCopies: false,
      dragRotate: true,
      touchPitch: true,
      // Keep several neighboring zoom rings resident so turning the camera
      // reuses tiles instead of unloading and fetching the same view again.
      // The explicit cap remains suitable for mobile memory budgets.
      maxTileCacheZoomLevels: 8,
      maxTileCacheSize: 192,
      fadeDuration: 220
    });
    graph._vectorwarpMap = map;
    map.on('moveend', () => {
      try {
        const center = map.getCenter();
        localStorage.setItem('vectorwarp-map-camera-v1', JSON.stringify({key: viewport.key,
          center: [center.lng, center.lat], zoom: map.getZoom(), pitch: map.getPitch(),
          bearing: map.getBearing(), at: Date.now()}));
      } catch (_) { /* Camera persistence is optional. */ }
    });
    graph.dataset.mapRenderer = 'maplibre-terrain';
    if (maplibre.NavigationControl && map.addControl) {
      const compass = new maplibre.NavigationControl({
        showCompass: true, showZoom: false, visualizePitch: true
      });
      map.addControl(compass, 'top-left');
      graph.dataset.mapCompass = 'bearing-and-pitch';
    }
    await new Promise((resolve, reject) => {
      const timeout = window.setTimeout(() => reject(new Error('Map renderer timed out')), 15000);
      map.once('load', () => { window.clearTimeout(timeout); resolve(); });
      map.once('error', event => {
        // A missing terrain tile must not prevent the base map and live radar
        // overlays from loading. Only fail before the style itself exists.
        if (!map.isStyleLoaded?.() && !map.getStyle?.()) {
          window.clearTimeout(timeout);
          reject(event?.error || new Error('Map renderer failed'));
        }
      });
    });
    syncMapBackground(map);
    return map;
  }

  function bearingOverlayGeoJson(rows) {
    const features = [];
    for (const item of rows) {
      const properties = {track: String(item.id ?? ''),
        uncertainty: finite(item.uncertainty_deg), state: String(item.state || '')};
      if (item.fused === true) {
        const coordinates = [finite(item.estimate?.longitude), finite(item.estimate?.latitude)];
        if (coordinates.every(Number.isFinite)) features.push({type: 'Feature',
          properties: {...properties, kind: item.resolved === true ? 'fused' : 'fusion-hypothesis',
            support: Number(item.support) || 0},
          geometry: {type: 'Point', coordinates}});
        continue;
      }
      const corridor = array(item.uncertainty_corridor).map(point =>
        [finite(point.longitude), finite(point.latitude)]).filter(point => point.every(Number.isFinite));
      if (corridor.length > 1) features.push({type: 'Feature',
        properties: {...properties, kind: 'corridor'},
        geometry: {type: 'LineString', coordinates: corridor}});
      const resolved = item.location && [finite(item.location.longitude), finite(item.location.latitude)];
      if (resolved && resolved.every(Number.isFinite)) features.push({type: 'Feature',
        properties: {...properties, kind: 'resolved'}, geometry: {type: 'Point', coordinates: resolved}});
      const estimate = !item.location && item.estimate &&
        [finite(item.estimate.longitude), finite(item.estimate.latitude)];
      if (estimate && estimate.every(Number.isFinite)) features.push({type: 'Feature',
        properties: {...properties, kind: 'estimate'}, geometry: {type: 'Point', coordinates: estimate}});
      if (!item.location) for (const candidate of array(item.candidates)) {
        const coordinates = [finite(candidate.longitude), finite(candidate.latitude)];
        if (coordinates.every(Number.isFinite)) features.push({type: 'Feature',
          properties: {...properties, kind: 'candidate'}, geometry: {type: 'Point', coordinates}});
      }
    }
    return {type: 'FeatureCollection', features};
  }

  function syncBearingOverlayLayers(map, rows) {
    if (!map?.addSource || !map?.getSource || !map?.addLayer || !map?.getLayer) return;
    const data = bearingOverlayGeoJson(rows);
    const source = map.getSource(bearingOverlaySourceId);
    if (source?.setData) source.setData(data);
    else map.addSource(bearingOverlaySourceId, {type: 'geojson', data});
    const activeSource = map.getSource(bearingOverlaySourceId);
    if (activeSource) activeSource._vectorwarpData = data;
    const add = layer => { if (!map.getLayer(layer.id)) map.addLayer(layer); };
    add({id: 'vectorwarp-bearing-corridors', type: 'line', source: bearingOverlaySourceId,
      filter: ['==', ['get', 'kind'], 'corridor'],
      paint: {'line-color': '#f7c75f', 'line-opacity': .72, 'line-width': 6}});
    add({id: 'vectorwarp-bearing-resolved', type: 'circle', source: bearingOverlaySourceId,
      filter: ['==', ['get', 'kind'], 'resolved'],
      paint: {'circle-radius': 7, 'circle-color': '#f7c75f', 'circle-opacity': .95,
        'circle-stroke-color': '#211812', 'circle-stroke-width': 2}});
    add({id: 'vectorwarp-bearing-estimates', type: 'circle', source: bearingOverlaySourceId,
      filter: ['==', ['get', 'kind'], 'estimate'],
      paint: {'circle-radius': 6, 'circle-color': '#b99cff', 'circle-opacity': .58}});
    add({id: 'vectorwarp-bearing-candidates', type: 'circle', source: bearingOverlaySourceId,
      filter: ['==', ['get', 'kind'], 'candidate'],
      paint: {'circle-radius': 5, 'circle-color': '#f7c75f', 'circle-opacity': .55}});
    add({id: 'vectorwarp-fused-track-hypotheses', type: 'circle', source: bearingOverlaySourceId,
      filter: ['==', ['get', 'kind'], 'fused'],
      paint: {'circle-radius': 8, 'circle-color': '#20dddc', 'circle-opacity': .9,
        'circle-stroke-color': '#102c2b', 'circle-stroke-width': 3}});
    add({id: 'vectorwarp-fusion-ambiguous-hypotheses', type: 'circle', source: bearingOverlaySourceId,
      filter: ['==', ['get', 'kind'], 'fusion-hypothesis'],
      paint: {'circle-radius': 5, 'circle-color': '#20dddc', 'circle-opacity': .18,
        'circle-stroke-color': '#20dddc', 'circle-stroke-opacity': .7,
        'circle-stroke-width': 1}});
  }

  function radarCoverageGeoJson(coverage, receiver) {
    const rows = Array.isArray(coverage?.points) ? coverage.points.map(point => ({
      bin: Number(point.bin),
      coordinate: [Number(point.longitude), Number(point.latitude)]
    })).filter(row => Number.isInteger(row.bin) && row.coordinate.every(Number.isFinite))
      .sort((left, right) => left.bin - right.bin) : [];
    const runs = [];
    for (const row of rows) {
      const run = runs.at(-1);
      if (run && row.bin === run.at(-1).bin + 1) run.push(row);
      else runs.push([row]);
    }
    const binCount = Math.max(1, Number(coverage?.bins) || 72);
    if (runs.length > 1 && runs[0][0]?.bin === 0 &&
        runs.at(-1).at(-1)?.bin === binCount - 1) {
      const wrapped = runs.pop().concat(runs.shift());
      runs.unshift(wrapped);
    }
    const origin = [Number(receiver?.longitude), Number(receiver?.latitude)];
    if (!origin.every(Number.isFinite)) return {type: 'FeatureCollection', features: []};
    return {type: 'FeatureCollection', features: runs.map((run, index) => {
      const points = run.map(row => row.coordinate);
      return points.length > 1 ? {
        type: 'Feature', id: `sector-${index}`, properties: {},
        geometry: {type: 'Polygon', coordinates: [[origin, ...points, origin]]}
      } : {
        type: 'Feature', id: `radial-${index}`, properties: {},
        geometry: {type: 'LineString', coordinates: [origin, ...points]}
      };
    })};
  }

  function syncRadarCoverageLayer(map, coverage, receiver) {
    if (!map?.addSource || !map?.getSource || !map?.addLayer || !map?.getLayer) return false;
    const data = radarCoverageGeoJson(coverage, receiver);
    try {
      const source = map.getSource(radarCoverageSourceId);
      if (source?.setData) source.setData(data);
      else map.addSource(radarCoverageSourceId, {type: 'geojson', data});
      const activeSource = map.getSource(radarCoverageSourceId);
      if (activeSource) activeSource._vectorwarpData = data;
      if (!map.getLayer(radarCoverageFillLayerId)) map.addLayer({
        id: radarCoverageFillLayerId, type: 'fill', source: radarCoverageSourceId,
        filter: ['==', '$type', 'Polygon'],
        paint: {'fill-color': '#20dddc', 'fill-opacity': .09}
      });
      if (!map.getLayer(radarCoverageLineLayerId)) map.addLayer({
        id: radarCoverageLineLayerId, type: 'line', source: radarCoverageSourceId,
        paint: {'line-color': '#20dddc', 'line-opacity': .92, 'line-width': 3}
      });
      return true;
    } catch (_) {
      // Older geographic renderers may not expose style mutation. Retain the
      // screen-space SVG path below as a compatibility fallback.
      return false;
    }
  }

  function renderAircraftOverlays(aircraft, sites = [], radarTracks = [], coverage = null) {
    const schedule = window.requestAnimationFrame || (callback => callback());
    schedule(() => {
      const graph = document.getElementById('data');
      const map = graph?._vectorwarpMap || graph?._fullLayout?.mapbox?._subplot?.map;
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
        layer._schedulePositionUpdate = (settled = false) => {
          layer._settledUpdatePending = layer._settledUpdatePending || settled;
          if (layer._positionFrame) return;
          layer._positionFrame = schedule(() => {
            layer._positionFrame = 0;
            const finalPass = layer._settledUpdatePending;
            layer._settledUpdatePending = false;
            layer._updatePositions?.(finalPass);
          });
        };
        map.on('move', () => layer._schedulePositionUpdate(false));
        map.on('resize', () => layer._schedulePositionUpdate(true));
        const endMapTransform = () => layer._schedulePositionUpdate(true);
        ['moveend', 'zoomend', 'rotateend', 'pitchend'].forEach(name =>
          map.on(name, endMapTransform));
      }
      layer._aircraftMarkers ||= new Map();
      layer._siteMarkers ||= new Map();
      layer._radarTrackPaths ||= new Map();
      layer._aircraftSnapshot = aircraft;
      layer._radarCoverage = coverage;
      // Keep the 24-hour boundary in geographic coordinates. Mapbox can then
      // transform one stable GPU layer during pan, pitch and pinch zoom rather
      // than making Mobile Safari repeatedly composite a changing SVG path.
      layer._coverageUsesMapLayer = syncRadarCoverageLayer(map, coverage, sites[0]);
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
          // A delay-only track is one bistatic uncertainty ellipse, not a
          // measured 3D volume. Keep one mathematically honest outline. The
          // previous five-slice cage was visually noisy and clipped badly as
          // a pitched map rotated through the viewport.
          const path = makePath('location-radar-track-path');
          const associated = makePath('location-radar-track-associated');
          layer._radarTrackGroup?.appendChild(group);
          record = {group, path, associated, item: track};
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
            item, heading: finite(heading), signature: '', isAircraft: true};
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
            const controlsDrawer = document.querySelector?.('.location-map-controls');
            const aircraftDrawer = document.querySelector?.('.location-aircraft-sidebar');
            if (selectedAircraftId && aircraftDrawer) aircraftDrawer.open = true;
            if (selectedAircraftId && controlsDrawer) controlsDrawer.open = false;
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
        // Public feeds occasionally omit course for one snapshot while
        // retaining the position. Do not make a moving aircraft snap to
        // north until the next snapshot restores the same course.
        const nextHeading = finite(heading);
        if (nextHeading !== null) record.heading = ((nextHeading % 360) + 360) % 360;
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
      // The raster base map has no glyph source. Keep site labels in
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
      const altitudeLiftFor = (item, pitch = finite(map.getPitch?.()) ?? 0,
        zoom = finite(map.getZoom?.()) ?? 0) => {
        if (pitch <= 0) return 0;
        const altitudeFeet = Number.isFinite(item.altitude) ? item.altitude : aircraftInfo(item).altitude;
        const groundMeters = finite(sites[0]?.altitude) ?? 0;
        const heightMeters = altitudeFeet === null ? 0 : Math.max(0, altitudeFeet * .3048 - groundMeters);
        const metersPerPixel = Math.cos(Number(item.lat) * Math.PI / 180) * 2 * Math.PI * 6378137 /
          (512 * Math.pow(2, zoom));
        return Math.min(240, heightMeters / Math.max(.01, metersPerPixel) *
          Math.sin(pitch * Math.PI / 180));
      };
      layer._updatePositions = (settled = false) => {
        const updateNow = window.performance?.now?.() ?? Date.now();
        const refreshGeometry = settled || !layer._lastGeometryUpdate ||
          updateNow - layer._lastGeometryUpdate >= 90;
        const refreshLabels = settled || !layer._lastLabelUpdate ||
          updateNow - layer._lastLabelUpdate >= 120;
        const modelInterval = window.innerWidth <= 760 ? 40 : 28;
        const refreshModels = settled || !layer._lastModelUpdate ||
          updateNow - layer._lastModelUpdate >= modelInterval;
        const mapBearing = finite(map.getBearing?.()) ?? 0;
        const mapPitch = finite(map.getPitch?.()) ?? 0;
        const mapZoom = finite(map.getZoom?.()) ?? 0;
        const line = points => points.map((point, index) =>
          `${index ? 'L' : 'M'}${point.x.toFixed(1)} ${point.y.toFixed(1)}`).join(' ');
        // MapLibre's public project() samples terrain elevation. That is right
        // for aircraft and site markers, but it makes mathematical bistatic
        // ellipses visibly ripple over hills. Project radar cages against the
        // zero-elevation map plane so their geometry stays smooth and exact.
        const projectFlatRadarPoint = (longitude, latitude) => {
          const transform = map?._camera?.transform;
          const LngLat = window.maplibregl?.LngLat;
          if (transform?.locationToScreenPoint && LngLat?.convert) {
            try { return transform.locationToScreenPoint(LngLat.convert([longitude, latitude]), null); }
            catch (_) { /* Fall back for older MapLibre builds. */ }
          }
          return map.project([longitude, latitude]);
        };
        if (refreshGeometry) {
        layer._lastGeometryUpdate = updateNow;
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
        layer._coveragePath?.setAttribute('d', layer._coverageUsesMapLayer ? '' : coveragePath);
        for (const {item, path: radarPath, associated} of layer._radarTrackPaths.values()) {
          const points = item.latitude.map((latitude, index) =>
            projectFlatRadarPoint(item.longitude[index], latitude))
            .filter(point => Number.isFinite(point?.x) && Number.isFinite(point?.y));
          const outline = points.length > 2 ? line(points) : '';
          radarPath.setAttribute('d', outline);
          const hasAssociation = finite(item.associatedAltitude) !== null;
          associated.setAttribute('d', hasAssociation ? outline : '');
          if (hasAssociation) associated.setAttribute('stroke', item.associatedColor || '#5be7ff');
        }
        }
        const modelInstances = [];
        const aircraftLabelCandidates = [];
        const mapWidth = container.clientWidth || 0;
        const mapHeight = container.clientHeight || 0;
        [...layer._aircraftMarkers.values(), ...layer._siteMarkers.values()].forEach(
          ({item, marker, icon, heading, flightPathAngle, isAircraft}) => {
        const point = map.project([Number(item.lon), Number(item.lat)]);
        const altitudeLift = isAircraft ? altitudeLiftFor(item, mapPitch, mapZoom) : 0;
        marker.style.left = `${point.x}px`;
        marker.style.top = `${point.y - altitudeLift}px`;
        const onScreen = point.x >= -80 && point.x <= mapWidth + 80 &&
          point.y - altitudeLift >= -80 && point.y - altitudeLift <= mapHeight + 80;
        marker.style.visibility = onScreen ? 'visible' : 'hidden';
        if (isAircraft && icon) {
          const aircraftId = aircraftInfo(item).hex || aircraftInfo(item).label;
          if (onScreen) aircraftLabelCandidates.push({marker, item, x: point.x,
            y: point.y - altitudeLift, selected: selectedAircraftId === aircraftId,
            radarTracked: Boolean(item.radar_track_id)});
          const rotation = aircraftScreenRotation(heading, mapBearing);
          const yaw = rotation === null ? 0 : rotation;
          const pitch = Number.isFinite(flightPathAngle) ? flightPathAngle : 0;
          icon.dataset.flightPath = pitch > .5 ? 'climbing' : pitch < -.5 ? 'descending' : 'level';
          const modelSize = mapZoom < 6 ? 11 : mapZoom < 7.5 ? 13.5 : 16.5;
          if (onScreen) modelInstances.push({model: aircraftKind(item), x: point.x,
            y: point.y - altitudeLift, yaw, pitch, size: modelSize,
            color: aircraftAltitudeColor(item)});
        } else if (onScreen) {
          modelInstances.push({model: item.kind === 'transmitter' ? 'transmitter' : 'receiver',
            x: point.x, y: point.y - 23, yaw: -mapBearing, pitch: 0,
            size: item.kind === 'transmitter' ? 15 : 14, color: item.kind === 'transmitter' ? '#ff8754' : '#8fe7df'});
        }
        });
        // Aircraft labels are HTML overlays rather than native map symbols, so
        // the map cannot perform collision avoidance for them. Apply a stable
        // screen-space declutter pass on every camera update.  Selected and
        // radar-associated aircraft win, then fresh positions nearest the map
        // centre.  Icons remain visible; only overlapping labels are hidden.
        if (refreshLabels) {
        layer._lastLabelUpdate = updateNow;
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
          const provenanceWidth = candidate.marker.classList.contains('mlat-position') ? 34 : 0;
          const width = Math.min(112, Math.max(38, text.length * 7 + 12 + provenanceWidth));
          const box = {left: candidate.x - width / 2, right: candidate.x + width / 2,
            top: candidate.y + 8, bottom: candidate.y + 24};
          const onScreen = box.right >= 0 && box.left <= mapWidth && box.bottom >= 0 && box.top <= mapHeight;
          const visible = onScreen && visibleLabels < labelLimit && !overlaps(box);
          candidate.marker.classList.toggle('label-hidden', !visible);
          if (visible) { occupied.push(box); visibleLabels++; }
        }
        }
        if (refreshModels) {
          layer._lastModelUpdate = updateNow;
          layer._modelRenderer?.render(modelInstances, {fast: !settled});
        }
        if (refreshGeometry) {
        const history = selectedAircraftId ? aircraftTrackHistories.get(selectedAircraftId) || [] : [];
        const points = history.map(point => {
          const projected = map.project([point.lon, point.lat]);
          return {x: projected.x, groundY: projected.y,
            flightY: projected.y - altitudeLiftFor(point, mapPitch, mapZoom), altitude: point.altitude};
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
        }
      };
      layer._schedulePositionUpdate?.(true);
    });
  }

  async function renderLocations() {
    setupLocationMapScaffold();
    const [config, tracker, coverage, laneContext, fused] = await Promise.all([
      getConfig(), apiJson('/api/tracker', {awaitFresh: true}),
      radarCoverage().catch(() => radarCoverageCache?.value || null), getLaneContext(),
      apiJson('/api/tracker/fused', {awaitFresh: true}).catch(() => null)
    ]);
    // Anchor every selected-channel overlay to the tracker frame. Without an
    // exact lane timestamp, the geographic map can compare one channel's
    // tracks with ADS-B geometry from an adjacent/global presentation frame.
    const frameTimestamp = Number.isSafeInteger(Number(tracker?.timestamp)) ?
      Number(tracker.timestamp) : null;
    const detection = await apiJson(atRadarFrame('/api/detection', frameTimestamp),
      {awaitFresh: true});
    const receiver = config?.location?.rx || {};
    const transmitter = config?.location?.tx || {};
    if ([receiver.latitude, receiver.longitude, transmitter.latitude, transmitter.longitude]
      .some(value => finite(value) === null)) {
      renderHtml(document.getElementById('view-summary'), card('Unavailable', 'Site coordinates'));
      renderDetail(empty('Map is unavailable', 'Configure receiver and illuminator coordinates in Settings.'));
      renderHtml(document.getElementById('data'), empty('Map is unavailable',
        'Receiver and illuminator coordinates are required.'));
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
    let bearingLocations = null;
    if (config?.process?.bearing?.enable === true) {
      try { bearingLocations = await apiJson('/api/locations'); }
      catch (_) { bearingLocations = {enabled: true, available: false, data: []}; }
    }

    let aircraft = [];
    let aircraftError = null;
    const adsbEnabled = config?.truth?.adsb?.enabled === true;
    const remoteIdEnabled = config?.truth?.remote_id?.enabled === true;
    if (adsbEnabled || remoteIdEnabled) {
      try {
        const loadTruth = async (base, enabled) => {
          if (!enabled) return [];
          const laneGeometryAvailable = base === '/api/adsb' ||
            !window.vectorwarpIlluminatorContext?.enabled;
          const [feed, geometry] = await Promise.all([
            apiJson(atRadarFrame(base, frameTimestamp), {awaitFresh: true}),
            laneGeometryAvailable ?
              apiJson(atRadarFrame(`${base}/delay-doppler?overlay=1`, frameTimestamp),
                {awaitFresh: true}) : {}
          ]);
          const rows = feed?.disabledForReplay ? [] : array(feed?.aircraft).filter(item =>
            finite(item.lat) !== null && finite(item.lon) !== null && (finite(item.seen_pos) ?? 0) < 120);
          const byId = new Map(Object.entries(geometry || {}).map(([id, item]) =>
            [String(item?.hex || id || '').toUpperCase(), item]));
          return rows.map(item => {
            const geometryRow = byId.get(String(item.hex || '').toUpperCase()) || null;
            return {...item, ...(geometryRow || {}),
              _lane_geometry_current: finite(geometryRow?.delay) !== null &&
                finite(geometryRow?.doppler) !== null};
          });
        };
        const results = await Promise.allSettled([
          loadTruth('/api/adsb', adsbEnabled), loadTruth('/api/remote-id', remoteIdEnabled)
        ]);
        aircraft = results.flatMap(result => result.status === 'fulfilled' ? result.value : []);
        const failed = results.filter((result, index) =>
          [adsbEnabled, remoteIdEnabled][index] && result.status === 'rejected');
        if (!aircraft.length && failed.length) throw failed[0].reason;
        if (failed.length) aircraftError = 'Some aircraft sources are temporarily unavailable';
        aircraft = stabilizeAircraftSnapshots(aircraft, {
          scope: `map:${laneContext.enabled ? laneContext.id : 'single'}`, graceMs: 10000
        }).map(item => {
          // A fresh position may legitimately lack geometry. Never inherit a
          // prior channel's delay/Doppler or a prior frame's association.
          const currentGeometry = item._lane_geometry_current === true &&
            item._snapshot_stale !== true;
          const clean = {...item, radar_track_id: undefined, radar_track_state: undefined};
          if (!currentGeometry) { clean.delay = null; clean.doppler = null; }
          return clean;
        });
        // Association is radar evidence, not a presentation preference. Use
        // the same motion/uncertainty-expanded gates and ambiguity rejection
        // as the server scorecard instead of a second, tighter UI-only rule.
        const cpiSeconds = finite(config?.process?.data?.cpi) ?? .5;
        const span = {kmPerBin: resolution, cpiSeconds,
          dopplerResolutionHz: 1 / cpiSeconds,
          matchDelayKm: finite(config?.process?.bearing?.manifold_recorder?.max_delay_error_km) ?? .25,
          matchDopplerHz: finite(config?.process?.bearing?.manifold_recorder?.max_doppler_error_hz) ?? 5,
          maxTruthAgeS: finite(config?.process?.bearing?.manifold_recorder?.max_truth_age_s) ?? 3};
        const truthRows = aircraft.map((item, aircraftIndex) => [aircraftIndex, item])
          .filter(([, item]) => {
            if (item._lane_geometry_current !== true) return false;
            return item._snapshot_stale !== true && finite(item.delay) !== null &&
              finite(item.doppler) !== null;
          });
        // Association evidence must remain independent of whether the operator
        // has hidden the track cages on the geographic map.
        const associationTracks = tracks;
        const trackPoints = associationTracks.map((track, index) => ({index, rawDelay: track.delay,
          delay: track.delay, doppler: track.doppler,
          tracker: {id: track.id, state: track.state}}));
        const matched = associationGates && assignRadarPoints ? assignRadarPoints(
          trackPoints, truthRows, item => associationGates(span, item,
            frameTimestamp ?? Date.now(), 'tracker',
            finite(config?.truth?.adsb?.provisional_event_time_uncertainty_s) ?? 0),
          'tracker', tracker?.timestamp) : new Map();
        for (const [aircraftIndex, match] of matched) aircraft[aircraftIndex] = {
          ...aircraft[aircraftIndex], radar_track_id: match.tracker?.id,
          radar_track_state: match.tracker?.state
        };
        updateAircraftTrackHistories(aircraftTrackHistories, aircraft);
        if (Date.now() - aircraftTrailPersistAt > 5000) {
          aircraftTrailPersistAt = Date.now();
          try {
            localStorage.setItem(aircraftTrailStorageKey, JSON.stringify(
              Object.fromEntries([...aircraftTrackHistories].slice(-200))));
          } catch (_) { /* A full/private browser cache must not affect the map. */ }
        }
        prefetchAircraftPhotos(aircraft.filter(item => !item.remote_id));
        selectedAircraftId = selectRadarAssociatedAircraft(selectedAircraftId, aircraft,
          autoSelectRadarAircraft);
        selectedAircraftSnapshot = aircraft.find(item => aircraftIdentity(item) === selectedAircraftId) ||
          selectedAircraftSnapshot;
      } catch (_) {
        aircraft = stabilizeAircraftSnapshots([], {
          scope: `${currentNode()}:map:${laneContext.enabled ? laneContext.id : 'single'}`,
          graceMs: 10000
        });
        aircraftError = 'Aircraft positions temporarily unavailable';
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
    const eligibleMapTracks = showStationary ? tracks : tracks.filter(item => !stationaryIds.has(item.id));
    const visibleTracks = showRadarTracksOnMap ? eligibleMapTracks : [];
    const mapAircraft = filterMapAircraft(aircraft, coverage);
    if (selectedAircraftId && !mapAircraft.some(item => aircraftIdentity(item) === selectedAircraftId)) {
      selectedAircraftId = null;
      selectedAircraftSnapshot = null;
    }
    const radarTrackOverlays = visibleTracks.map(item => radarTrackOverlay(item));
    // The map is also a bearing diagnostic: show every tracker state and every
    // finite estimate. Resolution remains a separate strict signal used by
    // downstream consumers. MapLibre receives these as one stable GeoJSON
    // source below rather than rebuilding a Plotly scene every CPI.
    const allBearingRows = array(bearingLocations?.data).filter(item => item &&
      (item.location || item.estimate || array(item.candidates).length));
    const bearingRows = showStationary ? allBearingRows : allBearingRows.filter(item => item.publishable !== false);
    const fusedTracks = fused?.available === true ? array(fused?.tracks).filter(item =>
      String(item?.state || '').toUpperCase() === 'FUSED_ACTIVE') : [];
    const fusedHypotheses = fused?.map_support?.available === true ?
      array(fused.map_support.tracks).filter(item =>
        String(item?.state || '').toUpperCase() === 'PERSISTENT_AMBIGUOUS' && Number(item?.misses) === 0) : [];
    const fusedRows = [...fusedTracks, ...fusedHypotheses].map((item, index) => ({
      id: item.id || `fused-${index + 1}`,
      state: item.state || 'FUSED_CANDIDATE', fused: true,
      resolved: String(item?.state || '').toUpperCase() === 'FUSED_ACTIVE',
      support: item.support, contributors: item.contributors,
      estimate: {latitude: finite(item.latitude ?? item.position_hypothesis?.latitude),
        longitude: finite(item.longitude ?? item.position_hypothesis?.longitude)}
    })).filter(item => [item.estimate.latitude, item.estimate.longitude].every(Number.isFinite));
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
    const remoteIdCount = mapAircraft.filter(item => item.remote_id).length;
    renderHtml(document.getElementById('view-summary'), `${card(showRadarTracksOnMap ? visibleTracks.length : 'Off', 'Radar ellipses')}${card(fusedTracks.length, 'Confirmed fused tracks')}${card(fusedHypotheses.length, 'Diagnostic ambiguities')}${card(mapAircraft.length, 'Aircraft shown')}${remoteIdEnabled ? card(remoteIdCount, 'Remote ID drones') : ''}${card(rawDetections.length, 'Detections')}${showRadarCoverageOnMap ? card(coverage?.supporting_passes ?? 0, laneContext.enabled ? `${laneContext.id} coverage passes` : 'Coverage passes') : card(bearingRows.length, 'Bearing estimates')}`);
    renderDetail('');
    const map = await ensureLocationMap(locationMapViewport);
    syncBearingOverlayLayers(map, [...bearingRows, ...fusedRows]);
    renderSelectedAircraftCard(mapAircraft);
    renderAircraftOverlays(mapAircraft, [
      {...receiver, kind: 'receiver', name: receiver.name || 'Receiver'},
      {...transmitter, kind: 'transmitter', name: transmitter.name || 'Illuminator'}
    ], radarTrackOverlays, showRadarCoverageOnMap ? coverage : null);
    const trackStateLabel = tracks.length ?
      `${tracks.length} current radar track${tracks.length === 1 ? '' : 's'} · ${mapAircraft.length} aircraft shown${showRadarTracksOnMap ? ' · ellipses shown' : ''}` :
      'No current radar tracks';
    setState(aircraftError || trackStateLabel,
      aircraftError ? 'warning' : tracks.length && showRadarTracksOnMap ? 'good' : 'warning');
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
    const stateGuide = '<p class="view-note"><strong>States:</strong> Tentative/Associated are candidates. Active is confirmed. Coasting is a confirmed track with missed detections.</p>';
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
    const context = await getLaneContext();
    const parameters = new URL(window.location.href).searchParams;
    const localMultiChannel = context.enabled && context.illuminators.length > 1;
    const fleet = typeof getFederatedChannels === 'function' ?
      await getFederatedChannels({fresh: true}).catch(() => null) : null;
    const networkCombinedAvailable = (fleet?.channels?.length ||
      context.illuminators.length) > 1 || (fleet?.nodes?.length || 1) > 1;
    const combinedScope = networkCombinedAvailable &&
      (parameters.get('scope') === 'all' ||
        (!parameters.has('scope') && !parameters.has('illuminator')));
    let statistics, aggregate, timingLabel, authoritativeFrames = 0;
    if (combinedScope) {
      const rows = await federatedNodeJson(
        '/api/live-scorecard/association-basis?timing=provisional', fleet);
      aggregate = networkAssociationAggregate(rows, fleet);
      statistics = aggregate;
      timingLabel = 'Operational receiver-event alignment';
    } else {
      const [allAuthoritative, allProvisional] = localMultiChannel ? await Promise.all([
        allChannelJson('/api/live-scorecard'),
        allChannelJson('/api/live-scorecard/provisional')]) : [null, null];
      const [authoritative, provisional] = await Promise.all([
        apiJson('/api/live-scorecard'), apiJson('/api/live-scorecard/provisional')]);
      authoritativeFrames = authoritative?.data_quality?.exact_timed_cpi_frames || 0;
      statistics = authoritativeFrames > 0 ? authoritative : provisional;
      timingLabel = authoritativeFrames > 0 ? 'Exact receiver-event alignment' :
        'Operational receiver-event alignment';
      aggregate = localMultiChannel ?
        (authoritativeFrames > 0 ? allAuthoritative : allProvisional) : statistics;
    }
    const multiChannel = combinedScope || localMultiChannel;
    const aggregateAssociation = aggregate?.overall_association_statistics || {};
    const channelAssociationRows = array(aggregate?.illuminators).map(channel => {
      const value = channel.association_statistics || {};
      return `<tr><td>${escapeHtml(channel.identity?.illuminator || channel.selected_channel)}</td><td>${integer(value.detection?.associated_passes)} / ${integer(value.detection?.eligible_adsb_passes)}</td><td>${percent(value.detection?.yield, 1)}</td><td>${integer(value.tracking?.associated_passes)} / ${integer(value.tracking?.detected_passes)}</td><td>${percent(value.tracking?.yield, 1)}</td><td>${integer(value.timing?.audited_aircraft_cpis)}</td></tr>`;
    });
    const windows = {...(statistics?.windows || {})};
    if (statistics?.current_setup_lifetime)
      windows.lifetime = statistics.current_setup_lifetime;
    const periods = [['Past 24 hours', windows.day], ['All time', windows.lifetime]];
    const lifetime = windows.lifetime || {};
    const day = windows.day || {};
    const coverage = day;
    const limits = statistics?.limits || {};
    const scoreRows = periods.map(([label, item = {}]) => `<tr><td>${label}</td><td>${integer(item.eligible_passes)}</td><td>${percent(item.detection_yield, 1)}</td><td>${percent(item.tracking_yield, 1)}</td></tr>`);
    const outcomeRows = periods.map(([label, item = {}]) => `<tr><td>${label}</td><td>${integer(item.out_of_config_passes)}</td><td>${integer(item.eligible_no_radar_return)}</td><td>${integer(item.detected_not_tracked)}</td><td>${integer(item.tracked_passes)}</td></tr>`);
    const exclusionRows = periods.map(([label, item = {}]) => `<tr><td>${label}</td><td>${integer(item.out_of_span?.below_detection_floor)}</td><td>${integer(item.out_of_span?.above_delay_ceiling)}</td><td>${integer(item.out_of_span?.outside_doppler_span)}</td><td>${integer(item.out_of_span?.stale_truth)}</td><td>${integer(item.out_of_span?.missing_exact_timing)}</td><td>${integer(item.out_of_span?.beyond_clutter_cancellation)}</td></tr>`);
    const activePasses = array(statistics.active_passes);
    const activeEligible = activePasses.filter(item => (finite(item.eligible_cpis) ?? 0) >= 3)
      .sort((left, right) => (finite(right.tracked_cpis) ?? 0) - (finite(left.tracked_cpis) ?? 0) ||
        (finite(right.detected_cpis) ?? 0) - (finite(left.detected_cpis) ?? 0) ||
        (finite(right.eligible_cpis) ?? 0) - (finite(left.eligible_cpis) ?? 0));
    const activeRows = activeEligible.map(item => `<tr><td>${escapeHtml(item.flight || item.hex)}</td><td>${escapeHtml(humanize(item.status))}</td><td>${integer(item.eligible_cpis)}</td><td>${integer(item.detected_cpis)}</td><td>${integer(item.tracked_cpis)}</td><td>${number(item.last_truth?.delay_km, 2)}</td><td>${number(item.last_truth?.doppler_hz, 1)}</td></tr>`);
    const scoredFinalized = (array(statistics.recent_scored_passes).length ?
      array(statistics.recent_scored_passes) : array(statistics.recent_finalized_passes)
        .filter(item => (finite(item.eligible_cpis) ?? 0) >= 3))
      .sort((left, right) => (finite(right.finalized_at) ?? 0) -
        (finite(left.finalized_at) ?? 0));
    const finalizedRows = scoredFinalized.slice(0, 25).map(item => `<tr><td>${escapeHtml(item.flight || item.hex)}</td><td>${escapeHtml(humanize(item.outcome))}</td><td>${integer(item.eligible_cpis)}</td><td>${integer(item.detected_cpis)}</td><td>${integer(item.tracked_cpis)}</td><td>${timeLabel(item.finalized_at)}</td></tr>`);
    const quality = statistics.data_quality || {};
    const yieldFraction = value => Math.max(0, Math.min(1, finite(value) ?? 0));
    const yieldChart = (rows, label) => rows.length ? `<div class="statistics-yield-chart" role="img" aria-label="${escapeHtml(label)}">
      ${rows.map(([name, item = {}]) => `<div class="statistics-yield-row"><span>${escapeHtml(name)}</span><div class="statistics-yield-bars" title="Detection ${percent(item.detection_yield, 1)}; tracking ${percent(item.tracking_yield, 1)}"><i class="detected" style="width:${(yieldFraction(item.detection_yield) * 100).toFixed(1)}%"></i><i class="tracked" style="width:${(yieldFraction(item.tracking_yield) * 100).toFixed(1)}%"></i></div><b>${percent(item.detection_yield, 0)} / ${percent(item.tracking_yield, 0)}</b><small>n=${integer(item.eligible_passes)}</small></div>`).join('')}
      <div class="statistics-yield-legend"><span class="detected">Detected</span><span class="tracked">Tracked</span><small>Percentages · eligible sample count</small></div></div>` : '';
    const directionBand = value => {
      const numbers = String(value || '').match(/\d+(?:\.\d+)?/g)?.map(Number) || [];
      return {minimum: numbers[0], maximum: numbers[1]};
    };
    const polarPoint = (angleDeg, radius) => {
      const radians = (angleDeg - 90) * Math.PI / 180;
      return [180 + radius * Math.cos(radians), 180 + radius * Math.sin(radians)];
    };
    const annularSector = (minimum, maximum, inner, outer) => {
      const [a, b] = polarPoint(minimum, outer);
      const [c, d] = polarPoint(maximum, outer);
      const [e, f] = polarPoint(maximum, inner);
      const [g, h] = polarPoint(minimum, inner);
      const large = maximum - minimum > 180 ? 1 : 0;
      if (inner <= .1)
        return `M 180 180 L ${a.toFixed(2)} ${b.toFixed(2)} A ${outer} ${outer} 0 ${large} 1 ${c.toFixed(2)} ${d.toFixed(2)} Z`;
      return `M ${a.toFixed(2)} ${b.toFixed(2)} A ${outer} ${outer} 0 ${large} 1 ${c.toFixed(2)} ${d.toFixed(2)} L ${e.toFixed(2)} ${f.toFixed(2)} A ${inner} ${inner} 0 ${large} 0 ${g.toFixed(2)} ${h.toFixed(2)} Z`;
    };
    const directionColor = value => {
      const strength = yieldFraction(value);
      return `hsl(${(8 + strength * 158).toFixed(0)} 58% ${(34 + strength * 9).toFixed(0)}%)`;
    };
    const directionSky = (jointBands, metric, title, id) => {
      const observed = Object.entries(jointBands || {}).map(([name, item]) => {
        const [azimuthName, elevationName] = name.split('|');
        return {item, azimuthName: item.azimuth_band || azimuthName,
          elevationName: item.elevation_band || elevationName,
          azimuth: directionBand(item.azimuth_band || azimuthName),
          elevation: directionBand(item.elevation_band || elevationName)};
      }).filter(entry => Number.isFinite(entry.azimuth.minimum) &&
        Number.isFinite(entry.azimuth.maximum) && Number.isFinite(entry.elevation.minimum) &&
        Number.isFinite(entry.elevation.maximum));
      const azimuths = [[0, 45], [45, 90], [90, 135], [135, 180], [180, 225],
        [225, 270], [270, 315], [315, 360]];
      const elevations = [[0, 2], [2, 5], [5, 10], [10, 15], [15, 30], [30, 90]]
        .map(([minimum, maximum]) => [`${minimum}-${maximum}`, {minimum, maximum}]);
      const entries = azimuths.flatMap(([azimuthMinimum, azimuthMaximum]) =>
        elevations.map(([elevationName, elevation]) => {
          const match = observed.find(entry => entry.azimuth.minimum === azimuthMinimum &&
            entry.azimuth.maximum === azimuthMaximum &&
            entry.elevation.minimum === elevation.minimum &&
            entry.elevation.maximum === elevation.maximum);
          return match || {item: {}, azimuthName: `${azimuthMinimum}-${azimuthMaximum}`,
            elevationName, azimuth: {minimum: azimuthMinimum, maximum: azimuthMaximum},
            elevation};
        }));
      const elevationIndex = new Map(elevations.map(([name], index) => [name, index]));
      const radius = 137, ring = radius / elevations.length;
      const cells = entries.map(entry => {
        const index = elevationIndex.get(entry.elevationName);
        const outer = radius - index * ring;
        const inner = Math.max(0, radius - (index + 1) * ring);
        const eligible = finite(entry.item.eligible_passes) ?? 0;
        const countName = metric === 'tracking_yield' ? 'tracked_passes' : 'detected_passes';
        const successes = finite(entry.item[countName]) ?? 0;
        const value = finite(entry.item[metric]);
        const fill = eligible > 0 && value !== null ? directionColor(value) : '#292321';
        const path = annularSector(entry.azimuth.minimum, entry.azimuth.maximum, inner, outer);
        const label = eligible > 0 ? `${entry.azimuthName}° azimuth, ${entry.elevationName}° elevation: ${metric === 'tracking_yield' ? 'tracking' : 'detection'} ${successes}/${eligible} (${percent(value, 1)})` : `${entry.azimuthName}° azimuth, ${entry.elevationName}° elevation: no qualifying passes`;
        const selected = entry.azimuthName === statisticsDirectionSelection.azimuth &&
          entry.elevationName === statisticsDirectionSelection.elevation;
        return `<path class="direction-sky-cell${selected ? ' selected' : ''}" d="${path}" fill="${fill}" stroke="#120f0e" stroke-width="1.5" tabindex="-1" role="button" aria-label="${escapeHtml(label)}" data-direction-map="${id}" data-azimuth="${escapeHtml(entry.azimuthName)}" data-elevation="${escapeHtml(entry.elevationName)}" data-detail="${escapeHtml(label)}"><title>${escapeHtml(label)}</title></path>${eligible > 0 && eligible < 5 ? `<path d="${path}" fill="url(#${id}-hatch)" stroke="none" pointer-events="none"></path>` : ''}`;
      }).join('');
      const rings = elevations.map(([label], index) => {
        const value = radius - index * ring;
        return `<circle cx="180" cy="180" r="${value.toFixed(2)}"></circle><text x="${(184 + value * .72).toFixed(2)}" y="${(176 - value * .72).toFixed(2)}">${escapeHtml(label)}°</text>`;
      }).join('');
      const azimuthOptions = azimuths.map(([minimum, maximum]) => {
        const label = `${minimum}-${maximum}`;
        return `<option value="${label}"${label === statisticsDirectionSelection.azimuth ? ' selected' : ''}>${label}°</option>`;
      }).join('');
      const elevationOptions = elevations.map(([label]) => `<option value="${label}"${label === statisticsDirectionSelection.elevation ? ' selected' : ''}>${label}°</option>`).join('');
      const selectedDetail = entries.find(entry => entry.azimuthName === statisticsDirectionSelection.azimuth && entry.elevationName === statisticsDirectionSelection.elevation);
      const selectedEligible = finite(selectedDetail?.item?.eligible_passes) ?? 0;
      const selectedCount = finite(selectedDetail?.item?.[metric === 'tracking_yield' ? 'tracked_passes' : 'detected_passes']) ?? 0;
      const detail = selectedEligible > 0 ? `${statisticsDirectionSelection.azimuth}° azimuth, ${statisticsDirectionSelection.elevation}° elevation: ${metric === 'tracking_yield' ? 'tracking' : 'detection'} ${selectedCount}/${selectedEligible} (${percent(selectedDetail?.item?.[metric], 1)})` : `${statisticsDirectionSelection.azimuth}° azimuth, ${statisticsDirectionSelection.elevation}° elevation: no qualifying passes`;
      return `<div class="direction-sky"><strong>${escapeHtml(title)}</strong><svg viewBox="0 0 360 360" role="img" aria-label="${escapeHtml(title)}; north is up, horizon is outside, higher elevation is toward the center"><defs><pattern id="${id}-hatch" width="7" height="7" patternUnits="userSpaceOnUse" patternTransform="rotate(35)"><line x1="0" y1="0" x2="0" y2="7" stroke="rgb(255 255 255 / .34)" stroke-width="2"></line></pattern></defs><g class="direction-sky-cells">${cells}</g><g class="direction-sky-grid">${rings}<line x1="180" y1="43" x2="180" y2="317"></line><line x1="43" y1="180" x2="317" y2="180"></line></g><g class="direction-sky-cardinals"><text x="180" y="20">N</text><text x="340" y="185">E</text><text x="180" y="353">S</text><text x="20" y="185">W</text></g><text class="direction-sky-zenith" x="180" y="184">90°</text></svg><div class="direction-sky-controls" data-direction-map="${id}"><label>Sector<select data-direction-azimuth>${azimuthOptions}</select></label><label>Elevation<select data-direction-elevation>${elevationOptions}</select></label><output aria-live="polite">${escapeHtml(detail)}</output></div></div>`;
    };
    const directionalCoverage = (direction, scopeLabel, id) => {
      const joint = direction?.azimuth_elevation_strata?.bands || {};
      const evidenced = Object.entries(joint).filter(([, item]) =>
        (finite(item.eligible_passes) ?? 0) >= 5).sort((left, right) =>
        (finite(left[1].tracking_yield) ?? 0) - (finite(right[1].tracking_yield) ?? 0) ||
        (finite(right[1].eligible_passes) ?? 0) - (finite(left[1].eligible_passes) ?? 0));
      const selectedItem = Object.entries(joint).find(([name, item]) => {
        const [azimuth, elevation] = name.split('|');
        return (item.azimuth_band || azimuth) === statisticsDirectionSelection.azimuth &&
          (item.elevation_band || elevation) === statisticsDirectionSelection.elevation &&
          (finite(item.eligible_passes) ?? 0) > 0;
      });
      if (!selectedItem && evidenced.length) {
        const [azimuth, elevation] = evidenced[0][0].split('|');
        statisticsDirectionSelection = {
          azimuth: evidenced[0][1].azimuth_band || azimuth,
          elevation: evidenced[0][1].elevation_band || elevation};
      }
      let weakestText = 'No direction cell has five eligible passes yet; gaps are evidence gaps, not proven blind spots.';
      if (evidenced.length) {
        const [name, item] = evidenced[0];
        const [azimuth, elevation] = name.split('|');
        weakestText = `<strong>Lowest observed tracking rate (n ≥ 5):</strong> ${escapeHtml(item.azimuth_band || azimuth)}° azimuth, ${escapeHtml(item.elevation_band || elevation)}° elevation · tracked ${integer(item.tracked_passes)}/${integer(item.eligible_passes)} (${percent(item.tracking_yield, 1)}), detected ${integer(item.detected_passes)}/${integer(item.eligible_passes)} (${percent(item.detection_yield, 1)}).`;
      }
      return `<section class="info-card statistics-wide direction-coverage-card"><header><div><h2>Directional coverage</h2><p>${escapeHtml(scopeLabel)}</p></div></header><div class="direction-sky-pair">${directionSky(joint, 'tracking_yield', 'Tracking coverage', `${id}-tracking`)}${directionSky(joint, 'detection_yield', 'Detection coverage', `${id}-detection`)}</div><div class="direction-sky-legend"><span><i class="direction-gradient"></i>0% <b>yield</b> 100%</span><span class="direction-hatch-key">Striped: fewer than 5 qualifying passes</span><span class="direction-empty-key">Gray: no qualifying passes</span></div><p class="view-note direction-weakest">${weakestText}</p></section>`;
    };
    const bindDirectionCells = () => {
      if (typeof root?.querySelectorAll !== 'function') return;
      const select = (azimuth, elevation) => {
        statisticsDirectionSelection = {azimuth, elevation};
        for (const controls of root.querySelectorAll('.direction-sky-controls')) {
          controls.querySelector('[data-direction-azimuth]').value = azimuth;
          controls.querySelector('[data-direction-elevation]').value = elevation;
          const map = controls.dataset.directionMap;
          const cell = root.querySelector(`.direction-sky-cell[data-direction-map="${map}"][data-azimuth="${azimuth}"][data-elevation="${elevation}"]`);
          controls.querySelector('output').textContent = cell?.dataset.detail || `${azimuth}° azimuth, ${elevation}° elevation: no qualifying passes`;
        }
        for (const cell of root.querySelectorAll('.direction-sky-cell'))
          cell.classList.toggle('selected', cell.dataset.azimuth === azimuth &&
            cell.dataset.elevation === elevation);
      };
      for (const controls of root.querySelectorAll('.direction-sky-controls')) {
        if (controls.dataset.bound === 'true') continue;
        controls.dataset.bound = 'true';
        for (const input of controls.querySelectorAll('select')) input.addEventListener('change', () =>
          select(controls.querySelector('[data-direction-azimuth]').value,
            controls.querySelector('[data-direction-elevation]').value));
      }
      for (const cell of root.querySelectorAll('.direction-sky-cell')) {
        if (cell.dataset.bound === 'true') continue;
        cell.dataset.bound = 'true';
        const activate = () => select(cell.dataset.azimuth, cell.dataset.elevation);
        cell.addEventListener('click', activate); cell.addEventListener('focus', activate);
        cell.addEventListener('keydown', event => {
          if (event.key === 'Enter' || event.key === ' ') { event.preventDefault(); activate(); }
        });
      }
    };
    const associationFlow = (label, eligible, detection, tracking) => {
      const detected = finite(detection?.associated_passes) ?? 0;
      const tracked = finite(tracking?.associated_passes) ?? 0;
      const n = finite(eligible) ?? 0;
      const node = (name, value, denominator, yieldValue, tone) => `<div class="association-flow-node ${tone}"><span class="association-ring" style="--association-yield:${(yieldFraction(yieldValue) * 360).toFixed(1)}deg"><b>${percent(yieldValue, 1)}</b></span><strong>${escapeHtml(name)}</strong><small>${integer(value)} / ${integer(denominator)}</small></div>`;
      const trackingDenominator = finite(tracking?.detected_passes) ?? detected;
      return `<div class="association-flow" role="img" aria-label="${escapeHtml(label)}: ${integer(n)} eligible, ${integer(detected)} detected, ${integer(tracked)} tracked"><div class="association-flow-source"><b>${integer(n)}</b><span>${escapeHtml(label)}</span></div><div class="association-flow-branches" aria-hidden="true"><i></i><i></i></div><div class="association-flow-results">${node('Detected', detected, n, detection?.yield, 'detected')}${node('Tracked after detection', tracked, trackingDenominator, tracking?.yield, 'tracked')}</div></div>`;
    };
    const channelContributionVisual = contribution => {
      const n = finite(contribution?.eligible_aircraft_passes) ?? 0;
      if (!n) return '';
      const channels = array(contribution.channels);
      const metric = (value, label) => {
        const segments = [
          ['Shared', finite(value?.shared_by_multiple_channels) ?? 0, '#4dd4b0'],
          ...channels.map((channel, index) => [channel.channel,
            finite(value?.only_by_channel?.[channel.channel]) ?? 0,
            colors[(index + 2) % colors.length]]),
          ['Not caught', finite(value?.not_caught) ?? 0, '#554842']
        ];
        return `<div class="contribution-row"><strong>${escapeHtml(label)}</strong><div class="contribution-bar">${segments.map(([name, count, color]) => `<span style="width:${(count / n * 100).toFixed(3)}%;--segment-color:${color}" title="${escapeHtml(name)}: ${integer(count)}/${integer(n)}"></span>`).join('')}</div><div class="contribution-legend">${segments.map(([name, count, color]) => `<span style="--segment-color:${color}"><i></i>${escapeHtml(name)} <b>${integer(count)}</b></span>`).join('')}</div></div>`;
      };
      const channelCards = channels.map((channel, index) => `<article class="channel-contribution-card" style="--channel-color:${colors[(index + 2) % colors.length]}"><strong>${escapeHtml(channel.channel)}</strong><span><b>${integer(channel.unique_detection_contribution)}</b>unique detections</span><span><b>${integer(channel.unique_tracking_contribution)}</b>unique tracks</span><small>${integer(channel.eligible_only_on_channel)} passes eligible only here</small></article>`).join('');
      return `<section class="info-card statistics-wide contribution-card"><header><h2>Channel contribution</h2><span>Retained finalized passes · n=${integer(n)}</span></header>${metric(contribution.detection, 'Detection union')}${metric(contribution.tracking, 'Tracking union')}<div class="channel-contribution-grid">${channelCards}</div></section>`;
    };
    const trackPopulationVisual = (population, title = 'Track population') => {
      if (!population || !finite(population.retained_tracks)) return '';
      const total = finite(population.retained_tracks) || 0;
      const associated = finite(population.adsb_associated_tracks) || 0;
      const unassociated = finite(population.unassociated_tracks) || 0;
      const bands = population.similarity_bands || {};
      const high = finite(bands['high-similarity']) || 0;
      const moderate = finite(bands['moderate-similarity']) || 0;
      const low = finite(bands['low-similarity']) || 0;
      const unknown = (finite(bands['insufficient-reference']) || 0) +
        (finite(bands['insufficient-evidence']) || 0);
      const segment = (value, className, label) => value > 0 ?
        `<span class="${className}" style="width:${(value / Math.max(1, total) * 100).toFixed(3)}%" title="${escapeHtml(label)}: ${integer(value)}"></span>` : '';
      const featureLabels = {duration_s: 'Duration', observed_cpis: 'Observed CPIs',
        measured_fraction: 'Measured fraction', mean_absolute_doppler_hz: '|Doppler|',
        delay_motion_km: 'Range motion', doppler_motion_hz: 'Doppler motion',
        delay_step_km: 'Range motion / measured CPI',
        doppler_step_hz: 'Doppler motion / measured CPI'};
      const featureUnits = {duration_s: 's', observed_cpis: '', measured_fraction: '',
        mean_absolute_doppler_hz: 'Hz', delay_motion_km: 'km', doppler_motion_hz: 'Hz',
        delay_step_km: 'km', doppler_step_hz: 'Hz'};
      const comparisons = Object.entries(population.feature_comparison || {}).map(([name, values]) => {
        const matched = values?.associated || {}, unmatched = values?.unassociated || {};
        const maximum = Math.max(matched.p90 || 0, unmatched.p90 || 0,
          matched.median || 0, unmatched.median || 0, 1e-9);
        const left = Math.max(0, (matched.p10 || 0) / maximum * 100);
        const width = Math.max(1, ((matched.p90 || 0) - (matched.p10 || 0)) / maximum * 100);
        const matchedMedian = Math.max(0, Math.min(100, (matched.median || 0) / maximum * 100));
        const unmatchedMedian = Math.max(0, Math.min(100, (unmatched.median || 0) / maximum * 100));
        const unit = featureUnits[name] || '';
        return `<div class="track-feature"><strong>${escapeHtml(featureLabels[name] || humanize(name))}</strong><div class="track-feature-axis"><i class="associated-range" style="left:${left.toFixed(2)}%;width:${width.toFixed(2)}%"></i><i class="associated-median" style="left:${matchedMedian.toFixed(2)}%"></i><i class="unassociated-median" style="left:${unmatchedMedian.toFixed(2)}%"></i></div><small>Associated ${number(matched.median, name === 'measured_fraction' ? 2 : 1)}${unit} · unassociated ${number(unmatched.median, name === 'measured_fraction' ? 2 : 1)}${unit}</small></div>`;
      }).join('');
      const candidates = array(population.aircraft_like_candidates).slice(0, 8).map(track =>
        `<article class="track-candidate"><header><strong>${escapeHtml(track.channel ? `${track.channel} · ${track.track_id}` : `Track ${track.track_id}`)}</strong><b>${percent(track.similarity?.score, 0)}</b></header><span>${number(track.features?.duration_s, 1)} s · ${integer(track.observations)} CPIs · cell seen ${integer(track.similarity?.recurrence_count)}×</span><small>${number(track.delay_km?.minimum, 2)}–${number(track.delay_km?.maximum, 2)} km · ${number(track.doppler_hz?.minimum, 1)} to ${number(track.doppler_hz?.maximum, 1)} Hz</small></article>`).join('');
      const channelRows = array(population.channels).map(channel => `<tr><td>${escapeHtml(channel.channel)}</td><td>${integer(channel.adsb_associated_tracks)}</td><td>${integer(channel.unassociated_tracks)}</td><td>${integer(channel.similarity_bands?.['high-similarity'])}</td><td>${integer(channel.similarity_bands?.['moderate-similarity'])}</td><td>${integer(channel.similarity_bands?.['low-similarity'])}</td></tr>`);
      return `<section class="info-card statistics-wide track-population-card"><header><h2>${escapeHtml(title)}</h2><span>Operational track cohorts</span></header><div class="insight-stats insight-stats-four">${card(integer(total), 'Retained tracks')}${card(integer(associated), 'ADS-B associated')}${card(integer(unassociated), 'Unassociated with eligible ADS-B')}${card(integer(high), 'Aircraft-like motion')}</div><div class="track-population-bar">${segment(associated, 'associated', 'ADS-B associated')}${segment(high, 'high', 'Aircraft-like motion')}${segment(moderate, 'moderate', 'Moderate track-shape similarity')}${segment(low, 'low', 'Low track-shape similarity')}${segment(unknown, 'unknown', 'Insufficient evidence')}</div><div class="track-population-legend"><span class="associated">ADS-B associated ${integer(associated)}</span><span class="high">Aircraft-like ${integer(high)}</span><span class="moderate">Moderate ${integer(moderate)}</span><span class="low">Low ${integer(low)}</span>${unknown ? `<span class="unknown">Unscored ${integer(unknown)}</span>` : ''}</div>${comparisons ? `<div class="track-feature-grid"><div class="track-feature-key"><span class="associated">Associated central 80%</span><span class="unassociated">Unassociated median</span></div>${comparisons}</div>` : ''}${candidates ? `<h3>Most aircraft-like unassociated motion</h3><div class="track-candidate-grid">${candidates}</div>` : ''}${channelRows ? `<details class="statistics-table-details"><summary>Per-channel populations</summary>${table(['Channel', 'Associated', 'Unassociated', 'Aircraft-like', 'Moderate', 'Low'], channelRows)}</details>` : ''}</section>`;
    };
    const rangeSchematic = (bands, kind, title) => {
      const entries = Object.entries(bands || {}).sort((left, right) =>
        (finite(left[1]?.minimum) ?? directionBand(left[0]).minimum ?? 0) -
        (finite(right[1]?.minimum) ?? directionBand(right[0]).minimum ?? 0));
      if (!entries.length) return '';
      const shapes = entries.map(([name, item], index) => {
        const eligible = finite(item.eligible_passes) ?? 0;
        const color = eligible > 0 ? directionColor(item.tracking_yield) : '#292321';
        const support = eligible > 0 && eligible < 5 ? ' stroke-dasharray="5 5"' : '';
        if (kind === 'slant') {
          const radius = 25 + index * Math.min(15, 95 / Math.max(1, entries.length - 1));
          return `<circle cx="160" cy="130" r="${radius.toFixed(1)}" fill="none" stroke="${color}" stroke-width="9"${support}><title>${escapeHtml(name)} km: tracked ${integer(item.tracked_passes)}/${integer(item.eligible_passes)} (${percent(item.tracking_yield, 1)})</title></circle>`;
        }
        const focusOffset = 55;
        const rx = 62 + index * Math.min(13, 65 / Math.max(1, entries.length - 1));
        const ry = Math.sqrt(rx * rx - focusOffset * focusOffset);
        return `<ellipse cx="160" cy="130" rx="${rx.toFixed(1)}" ry="${ry.toFixed(1)}" fill="none" stroke="${color}" stroke-width="8"${support}><title>${escapeHtml(name)} km: tracked ${integer(item.tracked_passes)}/${integer(item.eligible_passes)} (${percent(item.tracking_yield, 1)})</title></ellipse>`;
      }).reverse().join('');
      const focus = kind === 'slant' ? `<circle cx="160" cy="130" r="7"></circle><text x="160" y="248">Receiver</text>` : `<circle cx="105" cy="130" r="7"></circle><circle cx="215" cy="130" r="7"></circle><text x="105" y="264">Receiver</text><text x="215" y="264">Transmitter</text>`;
      const chips = entries.map(([name, item]) => `<span style="--range-color:${directionColor(item.tracking_yield)}"><b>${escapeHtml(name)} km</b><small>T ${percent(item.tracking_yield, 0)} · n=${integer(item.eligible_passes)}</small></span>`).join('');
      return `<div class="range-schematic"><strong>${escapeHtml(title)}</strong><svg viewBox="0 0 320 280" role="img" aria-label="${escapeHtml(title)} tracking coverage schematic">${shapes}<g class="range-focus">${focus}</g></svg><div class="range-schematic-legend">${chips}</div></div>`;
    };
    const passCards = (items, recent = false) => array(items).map(item => {
      const eligible = finite(item.eligible_cpis) ?? 0;
      const detected = finite(item.detected_cpis) ?? 0;
      const tracked = finite(item.tracked_cpis) ?? 0;
      const tone = tracked >= 3 ? 'tracked' : detected >= 3 ? 'detected' : 'missed';
      const state = recent ? humanize(item.outcome) : (tracked ? 'Tracked so far' : detected ? 'Detected so far' : humanize(item.status));
      return `<article class="pass-visual-card ${tone}"><header><strong>${escapeHtml(item.flight || item.hex)}</strong><span>${escapeHtml(state)}</span></header><div class="pass-visual-tallies"><span><b>${integer(eligible)}</b>Eligible</span><span><b>${integer(detected)}</b>Detected</span><span><b>${integer(tracked)}</b>Tracked</span></div>${recent ? `<small>${timeLabel(item.finalized_at)}</small>` : `<small>${number(item.last_truth?.delay_km, 2)} km · ${number(item.last_truth?.doppler_hz, 1)} Hz</small>`}</article>`;
    }).join('');
    const outcomeVisual = item => `<div class="outcome-visual">${[
      ['Outside search', item?.out_of_config_passes, 'outside'],
      ['No return', item?.eligible_no_radar_return, 'missed'],
      ['Detected only', item?.detected_not_tracked, 'detected'],
      ['Tracked', item?.tracked_passes, 'tracked']
    ].map(([label, value, tone]) => `<span class="${tone}"><b>${integer(value)}</b>${escapeHtml(label)}</span>`).join('')}</div>`;
    const exclusionVisual = item => {
      const excluded = item?.out_of_span || {};
      return `<div class="exclusion-visual">${[
        ['Below range', excluded.below_detection_floor], ['Above range', excluded.above_delay_ceiling],
        ['Outside Doppler', excluded.outside_doppler_span], ['Stale truth', excluded.stale_truth],
        ['Unusable alignment', excluded.missing_exact_timing],
        ['Beyond clutter', excluded.beyond_clutter_cancellation]
      ].map(([label, value]) => `<span><i></i><b>${integer(value)}</b>${escapeHtml(label)}</span>`).join('')}</div>`;
    };
    const limitsVisual = () => {
      const delaySpan = Math.max(.001, (finite(limits.delayMaxKm) ?? 1) -
        (finite(limits.delayMinKm) ?? 0));
      const clutterWidth = Math.max(0, Math.min(100,
        ((finite(limits.clutterMaxKm) ?? 0) - (finite(limits.delayMinKm) ?? 0)) /
        delaySpan * 100));
      return `<div class="limits-visual" role="img" aria-label="Delay Doppler search and strict match limits"><div class="limits-search"><span class="limits-clutter" style="width:${clutterWidth.toFixed(1)}%">Clutter</span><span class="limits-gate">±${number(limits.matchDelayKm, 2)} km<br>±${number(limits.matchDopplerHz, 1)} Hz</span><i class="limits-zero"></i></div><div class="limits-axis"><span>${number(limits.delayMinKm, 2)} km</span><b>Doppler ${number(limits.dopplerMinHz, 0)}…${number(limits.dopplerMaxHz, 0)} Hz</b><span>${number(limits.delayMaxKm, 2)} km</span></div></div>`;
    };
    const scoringPipeline = () => `<div class="scoring-pipeline" role="img" aria-label="Scoring evidence pipeline"><span class="complete"><i>1</i><b>Capture</b><small>${integer(quality.unique_cpi_frames)} CPI frames</small></span><span class="complete"><i>2</i><b>Alignment</b><small>${integer(statistics.association_lag_cpis)} CPI hold</small></span><span class="complete"><i>3</i><b>Truth match</b><small>${escapeHtml(timingLabel)}</small></span><span class="complete"><i>4</i><b>Pass qualification</b><small>≥3 searchable CPIs</small></span></div>`;
    const associationYieldRows = multiChannel ? [
      ['Overall', {eligible_passes: aggregateAssociation.eligible_adsb_passes,
        detection_yield: aggregateAssociation.detection?.yield,
        tracking_yield: aggregateAssociation.tracking?.yield}],
      ...array(aggregate?.illuminators).map(channel => {
        const value=channel.association_statistics||{};
        return [channel.identity?.illuminator||channel.selected_channel,
          {eligible_passes:value.detection?.eligible_adsb_passes,
            detection_yield:value.detection?.yield,tracking_yield:value.tracking?.yield}];
      })] : [];
    if (combinedScope) {
      const unique = aggregateAssociation.unique_aircraft_passes || {};
      const combinedRows = [
        ['All channels combined', {eligible_passes: unique.eligible_adsb_passes,
          detection_yield: unique.detection?.yield, tracking_yield: unique.tracking?.yield}],
        ['Channel opportunities', {eligible_passes: aggregateAssociation.eligible_adsb_passes,
          detection_yield: aggregateAssociation.detection?.yield,
          tracking_yield: aggregateAssociation.tracking?.yield}],
        ...array(aggregate?.illuminators).map(channel => {
          const value = channel.association_statistics || {};
          return [channel.identity?.illuminator || channel.selected_channel,
            {eligible_passes: value.eligible_adsb_passes,
              detection_yield: value.detection?.yield,
              tracking_yield: value.tracking?.yield}];
        })
      ];
      const updatedAt = Math.max(0, ...array(aggregate?.illuminators)
        .map(channel => finite(channel.updated_at) ?? 0));
      const uniqueDetectionPercent = Math.max(0, Math.min(100,
        (finite(unique.detection?.yield) ?? 0) * 100)).toFixed(1);
      const uniqueTrackingPercent = Math.max(0, Math.min(100,
        (finite(unique.tracking?.yield) ?? 0) * 100)).toFixed(1);
      renderHtml(root, `<div class="statistics-page"><section class="statistics-section statistics-channel-overview"><header><h2>Combined aircraft-pass results · ${escapeHtml(timingLabel)}</h2></header><div class="insight-stats insight-stats-four statistics-distance-hero">
          <article class="metric-card statistics-hero-eligible"><strong>${integer(unique.eligible_adsb_passes)}</strong><span>Unique eligible aircraft passes</span></article>
          <article class="metric-card good statistics-hero-detected" style="--glance-yield:${uniqueDetectionPercent}%"><strong>${percent(unique.detection?.yield, 1)}</strong><span>Detected on any channel</span><small>${integer(unique.detection?.associated_passes)} / ${integer(unique.detection?.eligible_adsb_passes)} passes</small></article>
          <article class="metric-card good statistics-hero-tracked" style="--glance-yield:${uniqueTrackingPercent}%"><strong>${percent(unique.tracking?.yield, 1)}</strong><span>Tracked after detection</span><small>${integer(unique.tracking?.associated_passes)} / ${integer(unique.tracking?.detected_passes)} detected passes</small></article>
          <article class="metric-card statistics-network-context"><strong>${integer(array(aggregate?.illuminators).length)} channels · ${integer(aggregate?.network_node_count || 1)} nodes</strong><span>Network scope</span></article>
        </div>${yieldChart(combinedRows, 'Combined and per-channel detection and tracking yield')}</section>
        <section class="statistics-section"><div class="statistics-grid">
          <section class="info-card statistics-combined-catch-duplicate"><h2>Combined catch rate</h2>${associationFlow('Unique aircraft passes', unique.eligible_adsb_passes, unique.detection, unique.tracking)}</section>
          <section class="info-card"><h2>Channel opportunity totals</h2>${associationFlow('Eligible channel-passes', aggregateAssociation.eligible_adsb_passes, aggregateAssociation.detection, aggregateAssociation.tracking)}<dl>
            <div><dt>Eligible channel-passes</dt><dd>${integer(aggregateAssociation.eligible_adsb_passes)}</dd></div>
            <div><dt>Detection associations</dt><dd>${integer(aggregateAssociation.detection?.associated_passes)}</dd></div>
            <div><dt>Tracking associations</dt><dd>${integer(aggregateAssociation.tracking?.associated_passes)}</dd></div>
            <div><dt>Audited ADS-B CPIs</dt><dd>${integer(aggregateAssociation.timing?.audited_aircraft_cpis)}</dd></div>
          </dl></section>
        </div>${channelContributionVisual(unique.channel_contribution)}${trackPopulationVisual(aggregateAssociation.track_population, 'All-channel track population')}${directionalCoverage(unique.directional_coverage, 'Finalized 24 h · combined · deduplicated by aircraft pass', 'combined-direction')}${channelAssociationRows.length ? `<details class="statistics-table-details"><summary>Per-channel values</summary>${table(['Channel', 'Detected / eligible', 'Detection yield', 'Tracked / detected', 'Tracking yield', 'Audited CPIs'], channelAssociationRows)}</details>` : ''}</section></div>`);
      bindDirectionCells();
      setState(updatedAt ? `All channels · ${timeLabel(updatedAt)} · ${timingLabel.replace(/receiver-event\s*/i, '')}` :
        'All channels combined · Waiting for eligible ADS-B evidence', updatedAt ? 'good' : 'warning');
      return;
    }
    const bandOrder = value => finite(String(value).match(/-?\d+(?:\.\d+)?/)?.[0]) ?? Infinity;
    const bandEntries = bands => Object.entries(bands || {}).map(([name, item]) => [name, item])
      .sort((left, right) => (finite(left[1]?.minimum) ?? bandOrder(left[0])) -
        (finite(right[1]?.minimum) ?? bandOrder(right[0])));
    const stratumRows = (definition, bands) => bandEntries(bands).map(([label, item]) =>
      `<tr><td>${escapeHtml(definition)}</td><td>${escapeHtml(label)}</td><td>${integer(item.eligible_passes)}</td><td>${integer(item.detected_passes)}</td><td>${integer(item.tracked_passes)}</td><td>${percent(item.tracking_yield, 1)}</td></tr>`);
    const rangeRows = [
      ...stratumRows('Excess delay km', coverage.delay_strata?.bands),
      ...stratumRows('Receiver slant km', coverage.receiver_slant_range_strata?.bands)];
    const angleRows = [
      ...stratumRows('Elevation deg', coverage.elevation_strata?.bands),
      ...stratumRows('True azimuth deg', coverage.azimuth_strata?.bands)];
    const jointRows = Object.entries(coverage.azimuth_elevation_strata?.bands || {})
      .sort((left, right) => bandOrder(left[1]?.azimuth_band || left[0].split('|')[0]) -
        bandOrder(right[1]?.azimuth_band || right[0].split('|')[0]) ||
        bandOrder(left[1]?.elevation_band || left[0].split('|')[1]) -
        bandOrder(right[1]?.elevation_band || right[0].split('|')[1]))
      .map(([label, item]) =>
      `<tr><td>${escapeHtml(item.azimuth_band || label.split('|')[0])}</td><td>${escapeHtml(item.elevation_band || label.split('|')[1])}</td><td>${integer(item.eligible_passes)}</td><td>${percent(item.detection_yield, 1)}</td><td>${percent(item.tracking_yield, 1)}</td></tr>`);
    const rawAdsbWarning = String(statistics.adsb_quality?.warning || '');
    const adsbWarning = /exact|host-arrival|timing uncertainty/i.test(rawAdsbWarning) ?
      '' : rawAdsbWarning;
    renderHtml(root, `<div class="statistics-page">${multiChannel ? `<section class="statistics-section statistics-channel-overview"><header><h2>All-channel association overview · ${escapeHtml(timingLabel)}</h2></header><div class="insight-stats insight-stats-four">
        ${card(integer(aggregateAssociation.eligible_adsb_passes), 'Eligible channel-passes')}
        ${card(`${integer(aggregateAssociation.detection?.associated_passes)} / ${integer(aggregateAssociation.detection?.eligible_adsb_passes)}`, 'Detection associations / eligible')}
        ${card(`${integer(aggregateAssociation.tracking?.associated_passes)} / ${integer(aggregateAssociation.tracking?.detected_passes)}`, 'Tracking associations / detections')}
        ${card(integer(aggregateAssociation.timing?.audited_aircraft_cpis), 'Audited ADS-B CPIs')}
      </div>${associationYieldRows.length ? yieldChart(associationYieldRows, 'Overall and per-channel detection and tracking association yield') : ''}${channelAssociationRows.length ? `<details class="statistics-table-details"><summary>Values and timing quality</summary>${table(['Channel', 'Detected / eligible', 'Detection yield', 'Tracked / detected', 'Tracking yield', 'Audited CPIs'], channelAssociationRows)}</details>` : empty('No per-channel evidence yet', '')}</section>` : ''}${adsbWarning ? `<div class="evaluation-note"><strong>ADS-B data note:</strong> ${escapeHtml(adsbWarning)}</div>` : ''}<div class="insight-stats insight-stats-four">
      ${card(integer(lifetime.eligible_passes), 'All-time passes')}
      ${card(percent(lifetime.detection_yield, 1), 'All-time detection yield')}
      ${card(percent(lifetime.tracking_yield, 1), 'All-time tracking yield')}
      ${card(integer(activeEligible.length), 'Eligible aircraft now')}
    </div>
    <section class="statistics-section"><div class="statistics-grid">
      <section class="info-card statistics-score statistics-wide"><h2>Detection and tracking evidence</h2><div class="window-evidence-pair">${periods.map(([label, item = {}]) => associationFlow(label, item.eligible_passes, {associated_passes: item.detected_passes, yield: item.detection_yield}, {associated_passes: item.tracked_passes, yield: item.tracking_yield})).join('')}</div><details class="statistics-table-details"><summary>Values</summary>${table(['Window', 'Eligible passes', 'Detection yield', 'Tracking yield'], scoreRows)}</details></section>
      ${trackPopulationVisual(statistics.track_population, 'Radar-track population')}
    </div></section>
    <section class="statistics-section"><header><h2>Past 24 hours</h2></header><div class="statistics-grid">
      <section class="info-card statistics-wide"><h2>Range coverage</h2>${rangeRows.length ? `<div class="range-schematic-pair">${rangeSchematic(coverage.receiver_slant_range_strata?.bands, 'slant', 'Receiver slant range')}${rangeSchematic(coverage.delay_strata?.bands, 'delay', 'Excess-path contours')}</div><details class="statistics-table-details"><summary>Values</summary>${table(['Range measure', 'Band', 'Eligible', 'Detected', 'Tracked', 'Tracking yield'], rangeRows)}</details>` : empty('No range data', '')}</section>
    </div><div class="statistics-grid">${directionalCoverage(coverage, 'Finalized 24 h · selected channel', 'channel-direction')}</div><details class="statistics-details statistics-angle-details"><summary>Directional values</summary><div class="statistics-grid">
      <section class="info-card"><h2>Angle bands</h2>${angleRows.length ? table(['Angle', 'Band', 'Eligible', 'Detected', 'Tracked', 'Tracking yield'], angleRows) : empty('No angle data', '')}</section>
      <section class="info-card"><h2>Azimuth × elevation</h2>${jointRows.length ? table(['Azimuth', 'Elevation', 'Eligible', 'Detection yield', 'Tracking yield'], jointRows) : empty('No angle data', '')}</section>
    </div></details></section>
    <section class="statistics-section"><div class="statistics-grid">
      <section class="info-card"><h2>Eligible aircraft now</h2>${activeRows.length ? `<div class="pass-visual-list">${passCards(activeEligible)}</div><details class="statistics-table-details"><summary>Lookup table</summary><div class="statistics-evidence-list">${table(['Aircraft', 'Status', 'Eligible', 'Detected', 'Tracked', 'Delay km', 'Doppler Hz'], activeRows)}</div></details>` : empty('No eligible aircraft now', '')}</section>
      <section class="info-card"><h2>Recent scored passes</h2>${finalizedRows.length ? `<div class="pass-visual-list">${passCards(scoredFinalized.slice(0, 8), true)}</div><details class="statistics-table-details"><summary>Lookup table</summary><div class="statistics-evidence-list">${table(['Aircraft', 'Outcome', 'Eligible', 'Detected', 'Tracked', 'Finalized'], finalizedRows)}</div></details>` : empty('No scored passes yet', '')}</section>
    </div></section>
    <details class="statistics-details"><summary>Eligibility and scoring details</summary><div class="statistics-grid">
      <section class="info-card"><h2>Pass outcomes</h2><h3>Past 24 hours</h3>${outcomeVisual(day)}<h3>Setup lifetime</h3>${outcomeVisual(lifetime)}<details class="statistics-table-details"><summary>Values</summary>${table(['Window', 'Out of config', 'No radar return', 'Detected, not tracked', 'Tracked'], outcomeRows)}</details></section>
      <section class="info-card"><h2>Excluded opportunities</h2><h3>Past 24 hours · CPI counters</h3>${exclusionVisual(day)}<h3>Setup lifetime · CPI counters</h3>${exclusionVisual(lifetime)}<details class="statistics-table-details"><summary>Values</summary>${table(['Window', 'Below range', 'Above range', 'Outside Doppler', 'Stale truth', 'Unusable alignment', 'Beyond clutter'], exclusionRows)}</details></section>
      <section class="info-card"><h2>Search and match limits</h2>${limitsVisual()}<dl><div><dt>Physical delay span</dt><dd>${number(limits.delayMinKm, 3)}–${number(limits.delayMaxKm, 3)} km</dd></div><div><dt>Doppler span</dt><dd>${number(limits.dopplerMinHz, 0)} to ${number(limits.dopplerMaxHz, 0)} Hz</dd></div><div><dt>Clutter cancellation</dt><dd>Through ${number(limits.clutterMaxKm, 3)} km</dd></div><div><dt>Strict match gate</dt><dd>±${number(limits.matchDelayKm, 2)} km / ±${number(limits.matchDopplerHz, 1)} Hz</dd></div></dl></section>
      <section class="info-card"><h2>Scoring method</h2>${scoringPipeline()}<dl><div><dt>Timing</dt><dd>${escapeHtml(timingLabel)}</dd></div><div><dt>Frame hold</dt><dd>${integer(statistics.association_lag_cpis)} CPIs · ${integer(statistics.association_lag_ms)} ms</dd></div></dl><details class="statistics-table-details"><summary>Technical details</summary><dl><div><dt>Contract</dt><dd>${escapeHtml(String(statistics.evaluation_contract_hash || '').slice(0, 12))}</dd></div><div><dt>Nuisance accounting</dt><dd>${escapeHtml(humanize(quality.nuisance_accounting))}</dd></div></dl></details></section>
    </div></details></div>`);
    bindDirectionCells();
    const laneState = context.enabled ? `${context.id} · ` : '';
    setState(statistics.error ? `${laneState}Collection warning · ${statistics.error}` :
      statistics.updated_at ? `${laneState}Updated ${timeLabel(statistics.updated_at)} · ${timingLabel}` : `${laneState}Waiting for eligible ADS-B evidence`,
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
    const config = await getConfig();
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
    const bearingConfigured = config?.process?.bearing?.enable === true;
    const recorderConfigured = config?.process?.bearing?.manifold_recorder?.enabled === true;
    const retainedEvidence = (finite(progress.samples) ?? 0) > 0 ||
      (finite(progress.independent_passes) ?? 0) > 0 || Boolean(bearingCampaign.active_model) ||
      automation.qualified === true;
    const campaignAvailable = bearingConfigured || recorderConfigured || retainedEvidence;
    if (!campaignAvailable) {
      renderHtml(document.getElementById('view-summary'),
        `${card('Unavailable', 'Array calibration')}` +
        `${card('Disabled', 'Bearing output')}` +
        `${card('Disabled', 'Shared array recorder')}`);
    } else renderHtml(document.getElementById('view-summary'),
        `${card(percent(completion, 0), 'Collection gate progress', progress.collection_ready ? 'good' : 'warning')}` +
        `${card(integer(progress.independent_passes), 'Independent passes')}` +
        `${card(integer(progress.unique_aircraft), 'Unique aircraft')}` +
        `${card(`${integer(progress.covered_sectors)}/${integer(progress.required_covered_sectors)}`, 'Covered sectors')}`);
    const requirementRows = requirements.map(item => `<tr><td>${escapeHtml(item.label)}</td><td>${item.comparison === 'at_most' ? `≤ ${number(item.required, 1)}` : integer(item.required)}</td><td>${item.comparison === 'at_most' ? number(item.current, 1) : integer(item.current)}</td><td><span class="state-pill ${item.complete ? 'active' : 'coasting'}">${item.complete ? 'Ready' : 'Collecting'}</span></td></tr>`);
    const sectorRows = sectors.map(item => `<tr><td>${integer(item.from_deg)}–${integer(item.to_deg)}°</td><td>${integer(item.samples)}</td><td>${integer(item.passes)}</td><td>${integer(item.aircraft)}</td><td><span class="state-pill ${item.ready ? 'active' : 'coasting'}">${item.ready ? 'Covered' : `${integer(item.passes_needed)} passes / ${integer(item.aircraft_needed)} aircraft needed`}</span></td></tr>`);
    const blockers = campaignAvailable ? array(progress.next_actions) : [];
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
        <div><dt>Live bearing manifold</dt><dd>${active ? escapeHtml(bearingCampaign.active_model) : bearingConfigured ? 'Ideal steering model' : 'Bearing output disabled'}</dd></div>
        <div><dt>Last sample</dt><dd>${progress.last_sample_at ? escapeHtml(timeLabel(progress.last_sample_at)) : 'Waiting for accepted evidence'}</dd></div>
      </dl><p class="view-note">${escapeHtml(progress.limitation || 'Collection and production qualification are separate.')}</p></section>
      <section class="info-card"><h2>Next evidence needed</h2>${!campaignAvailable ? '<p class="view-note">No array-calibration campaign is active. Enable bearing or the shared array recorder to collect evidence.</p>' : blockers.length ? `<ul class="calibration-blockers">${blockers.map(item => `<li>${escapeHtml(item)}</li>`).join('')}</ul>` : '<p class="view-note">Collection gates are complete. Held-out performance gates determine whether the candidate can activate.</p>'}${failureText.length ? `<p class="view-note evaluation-note">Latest fit: ${escapeHtml(failureText.join(' '))}</p>` : ''}</section>
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
    setState(!campaignAvailable ? 'Unavailable · bearing and array recorder disabled' :
      recorder.error || automation.error || detectionAutomation.error ? `Calibration warning · ${recorder.error || automation.error || detectionAutomation.error}` :
      automation.qualified ? 'Qualified measured manifold' : recorder.enabled || automation.running ?
        `Collecting · ${percent(completion, 0)} of minimum evidence gates` :
        `Idle · retained evidence ${percent(completion, 0)}`, tone);
  }

  const renderers = {overview: renderOverview, detections: renderDetections, health: renderHealth, activity: renderActivity, locations: renderLocations, evaluation: renderEvaluation, tracks: renderTracks, statistics: renderStatistics, calibration: renderCalibration};

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

  window.addEventListener?.('vectorwarp:illuminator-change', () => {
    nextRefreshAt = 0;
    // A running refresh will complete first; the normal scheduler retries.
    if (!refreshing) void refresh();
  });
  window.addEventListener?.('vectorwarp:statistics-scope-change', () => {
    nextRefreshAt = 0;
    if (!refreshing) void refresh();
  });
  startRadarUpdates(refresh);
}());
