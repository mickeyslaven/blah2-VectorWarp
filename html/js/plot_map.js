var gridSignature = '';
var range_x = [];
var range_y = [];
var radarSemantics = radarVisualSemantics;
var selectedDelayDopplerTarget = null;
var delayDopplerSelectionTraces = [];

function delayDopplerSelectionStyle(trace) {
  const selected = (trace.ids || []).map(id => id === selectedDelayDopplerTarget);
  return {
    'marker.size': selected.map(value => value ? trace.size + 4 : trace.size),
    'marker.line.color': selected.map(value => value ? radarSemantics.colors.selected : trace.outline),
    'marker.line.width': selected.map(value => value ? 3 : trace.width)
  };
}

function refreshDelayDopplerSelection() {
  return Promise.all(delayDopplerSelectionTraces.map(trace => {
    const style = delayDopplerSelectionStyle(trace);
    return Plotly.restyle('data', Object.fromEntries(Object.entries(style)
      .map(([key, value]) => [key, [value]])), [trace.index]);
  }));
}

function bindDelayDopplerSelection() {
  const plot = typeof document === 'undefined' ? null : document.getElementById('data');
  if (!plot?.on || plot._vectorwarpSelectionBound) return;
  plot._vectorwarpSelectionBound = true;
  plot.tabIndex = 0;
  plot.setAttribute?.('aria-description', 'Bistatic range and Doppler coordinates. Click a marker to select; Escape clears selection.');
  plot.on('plotly_click', event => {
    const point = event?.points?.[0];
    const trace = delayDopplerSelectionTraces.find(item => item.index === point?.curveNumber);
    const id = trace?.ids?.[point?.pointNumber];
    if (!id) return;
    selectedDelayDopplerTarget = selectedDelayDopplerTarget === id ? null : id;
    void refreshDelayDopplerSelection();
  });
  plot.addEventListener?.('keydown', event => {
    if (event.key !== 'Escape' || !selectedDelayDopplerTarget) return;
    selectedDelayDopplerTarget = null;
    void refreshDelayDopplerSelection();
  });
}

function delayDopplerPresentation(width, height) {
  // A landscape phone has a wide CSS viewport, but very little vertical room.
  // Treat either constrained dimension as compact so fullscreen does not waste
  // the display on desktop margins, long axis labels, or a color scale.
  const compact = (Number.isFinite(width) && width <= 600) ||
    (Number.isFinite(height) && height <= 600);
  return compact ? {
    margin: {l: 38, r: 6, b: 38, t: 8, pad: 0},
    fontSize: 11,
    axisTitleSize: 12,
    xTitle: 'Range (km)',
    yTitle: 'Hz',
    tickSize: 10,
    tickCount: 4,
    showScale: false,
    colorbar: {
      title: {text: 'dB', side: 'right', font: {color: '#f3eee9', size: 11}},
      tickfont: {color: '#f3eee9', size: 9},
      outlinecolor: '#47362e', thickness: 8, len: .72, xpad: 2
    }
  } : {
    margin: {l: 92, r: 132, b: 50, t: 36, pad: 0},
    fontSize: 14,
    axisTitleSize: 20,
    xTitle: 'Bistatic Range (km)',
    yTitle: 'Bistatic Doppler (Hz)',
    tickSize: 14,
    tickCount: 0,
    showScale: true,
    colorbar: {
      // Native maps deliberately preserve the historical 10*log10(|CAF|)
      // display scale.  Calling that quantity power is dimensionally wrong
      // and invites threshold comparisons against the detector's separate
      // power statistic.
      title: {text: 'Map level (dB)', side: 'right', font: {color: '#f3eee9', size: 15}},
      tickfont: {color: '#f3eee9', size: 13},
      outlinecolor: '#47362e', thickness: 14
    }
  };
}

function currentDelayDopplerPresentation() {
  return delayDopplerPresentation(typeof window === 'undefined' ? 1024 : window.innerWidth,
    typeof window === 'undefined' ? 768 : window.innerHeight);
}

var plotPresentation = currentDelayDopplerPresentation();

function mapPresentationUrl(path, width) {
  const separator = path.includes('?') ? '&' : '?';
  // The native 601x511 JSON map is about 1.8 MB per frame.  A peak-preserving
  // server presentation keeps every web client bounded without changing
  // detector processing or the underlying scientific grid. Desktop gets more
  // visual detail; tablets and phones avoid unnecessary transfer and paint.
  const compact = Number.isFinite(width) && width <= 1024;
  return `${path}${separator}max_rows=${compact ? 128 : 192}&max_cols=${compact ? 128 : 256}`;
}

// setup API
var selectedIlluminator = '';
var selectedIlluminatorSynthetic = false;
var selectedIlluminatorSampleRate = null;
var mapProductPath = urlMap;
var urlDetection = null;
var urlTracker = null;
var urlAdsbData = null;
var urlRemoteIdData = liveApiUrl(radarPresentationUrl('/api/remote-id/delay-doppler?overlay=1'));
var maxHoldState = null;

function finiteGridMaximum(grid, fallback = 0) {
  let maximum = Number.NEGATIVE_INFINITY;
  for (const row of Array.isArray(grid) ? grid : []) {
    if (!Array.isArray(row)) continue;
    for (const value of row) {
      const numeric = Number(value);
      if (Number.isFinite(numeric) && numeric > maximum) maximum = numeric;
    }
  }
  return Number.isFinite(maximum) ? maximum : fallback;
}

function applyClientMaxHold(frame, display, now = performance.now()) {
  if (typeof window === 'undefined' || window.vectorwarpMapMode !== 'maxhold' ||
      !Array.isArray(frame?.data)) { maxHoldState = null; return frame; }
  const axes = JSON.stringify([frame.delay, frame.doppler]);
  const previous = maxHoldState;
  const timestamp = Number(frame.timestamp);
  const continuing = previous && previous.axes === axes && now >= previous.at &&
    !(timestamp < previous.timestamp) && previous.grid.length === frame.data.length &&
    previous.grid.every((row, index) => row.length === frame.data[index]?.length);
  const rate = display?.max_hold_decay_db_per_second;
  const decay = continuing ? (typeof rate === 'number' && Number.isFinite(rate) &&
    rate >= 0 && rate <= 20 ? rate : 1) * (now - previous.at) / 1000 : 0;
  // Published map cells are dB: subtract a wall-time rate, including below zero.
  // A monotonic clock keeps the fade independent of frame cadence and CPI.
  const grid = frame.data.map((row, r) => row.map((value, c) => continuing ?
    Math.max(Number(value), previous.grid[r][c] - decay) : Number(value)));
  maxHoldState = {grid, axes, at: now, timestamp};
  // A full desktop map contains more than 300k cells. Spreading that array
  // into Math.max exceeds JavaScript's argument limit even though the exact
  // same selected-lane frame works after mobile downsampling.
  return {...frame, data: grid,
    maxPower: finiteGridMaximum(grid, Number(frame.maxPower) || 0)};
}

// setup plotly
var layout = {
  font: {color: '#f3eee9', size: plotPresentation.fontSize},
  autosize: true,
  margin: plotPresentation.margin,
  hoverlabel: {
    namelength: 0
  },
  plot_bgcolor: "rgba(0,0,0,0)",
  paper_bgcolor: "rgba(0,0,0,0)",
  annotations: [],
  // Preserve zoom/pan while live frames update through Plotly.update.
  uirevision: 'vectorwarp-delay-doppler',
  displayModeBar: false,
  xaxis: {
    gridcolor: '#47362e', zerolinecolor: '#47362e',
    title: {
      text: plotPresentation.xTitle,
      font: {
        size: plotPresentation.axisTitleSize
      }
    },
    tickfont: {size: plotPresentation.tickSize},
    nticks: plotPresentation.tickCount || undefined,
    automargin: true,
    ticks: '',
    side: 'bottom'
  },
  yaxis: {
    gridcolor: '#47362e', zerolinecolor: '#47362e',
    title: {
      text: plotPresentation.yTitle,
      font: {
        size: plotPresentation.axisTitleSize
      }
    },
    tickfont: {size: plotPresentation.tickSize},
    nticks: plotPresentation.tickCount || undefined,
    automargin: true,
    ticks: '',
    ticksuffix: ' ',
    categoryorder: "total descending"
  },
  showlegend: false
};
var config = {
  responsive: true,
  displayModeBar: false
  //scrollZoom: true
}

// setup plotly data
// Do not draw a synthetic zero-valued heatmap before the first complete radar
// frame. Plotly maps zero to the middle of its automatic colour range, which
// produced a large green rectangle that looked like real radar data.
var data = [];
var detection = [];
var radarOverlayMode = 'all';
try {
  const saved = localStorage.getItem('vectorwarp-radar-overlay-mode');
  if (['all', 'tracks', 'detections'].includes(saved)) radarOverlayMode = saved;
} catch (_) { /* Persistence is optional. */ }
function overlayVisibility() {
  return {detections: radarOverlayMode !== 'tracks', tracks: radarOverlayMode !== 'detections'};
}
function updateOverlayControl() {
  const control = typeof document === 'undefined' ? null : document.getElementById('radar-overlay-mode');
  if (control) control.value = radarOverlayMode;
  if (typeof document !== 'undefined') document.querySelectorAll('[data-overlay-kind]').forEach(item => {
    item.hidden = !overlayVisibility()[item.dataset.overlayKind];
  });
}
if (typeof document !== 'undefined') {
  const control = document.getElementById('radar-overlay-mode');
  updateOverlayControl();
  control?.addEventListener('change', () => {
    radarOverlayMode = control.value;
    try { localStorage.setItem('vectorwarp-radar-overlay-mode', radarOverlayMode); } catch (_) {}
    updateOverlayControl();
  });
}
var adsb = {};
var remoteId = {};
function adsbOverlay(data, channel, graceMs) {
  const overlay = {delay: [], doppler: [], flight: [], customdata: [], hover: [], colors: [], ids: []};
  const stable = stabilizeAircraftSnapshots(Object.entries(data || {})
    .map(([hex, record]) => record?.hex ? record : {...record, hex}),
  {scope: `delay-doppler:${channel || 'single'}`, graceMs});
  for (const aircraft of stable) {
    if (!aircraft || !('doppler' in aircraft)) continue;
    overlay.delay.push(aircraft.delay);
    overlay.doppler.push(aircraft.doppler);
    overlay.flight.push(aircraftLabel(aircraft));
    overlay.customdata.push(aircraftPlotData(aircraft));
    overlay.colors.push(radarSemantics.aircraftColor(aircraft));
    overlay.ids.push(`aircraft:${aircraftIdentity(aircraft)}`);
    overlay.hover.push(aircraftHoverText(aircraft,
      {delay: aircraft.delay, doppler: aircraft.doppler}));
  }
  return overlay;
}

function trackerOverlay(frame, runningConfig) {
  const sampleRate = Number(runningConfig?.capture?.fs);
  const kilometresPerBin = Number.isFinite(sampleRate) && sampleRate > 0 ?
    299792458 / sampleRate / 1000 : null;
  // ASSOCIATED is a pre-confirmation M-of-N candidate, not an operational
  // aircraft track. ACTIVE/CONFIRMED are established; COASTING is retained
  // separately because it was previously established.
  const updatedStates = new Set(['ACTIVE', 'CONFIRMED']);
  const result = {
    updated: {delay: [], doppler: [], hover: [], ids: []},
    coasting: {delay: [], doppler: [], hover: [], ids: []}
  };
  if (kilometresPerBin === null) return result;
  for (const track of Array.isArray(frame?.data) ? frame.data : []) {
    const delay = Number(track?.delay), doppler = Number(track?.doppler);
    const state = String(track?.state || '').toUpperCase();
    if (!Number.isFinite(delay) || !Number.isFinite(doppler) ||
        (!updatedStates.has(state) && state !== 'COASTING')) continue;
    const range = delay * kilometresPerBin;
    const group = state === 'COASTING' ? result.coasting : result.updated;
    group.delay.push(range);
    group.doppler.push(doppler);
    group.ids.push(`track:${track.id ?? `${delay}:${doppler}`}`);
    group.hover.push(`Radar track ${track.id ?? '—'}<br>${state === 'COASTING' ? 'Coasting' : 'Radar tracked'}<br>Bistatic range ${range.toFixed(2)} km · Doppler ${doppler.toFixed(1)} Hz`);
  }
  return result;
}

Plotly.react('data', data, {...layout, annotations: [{
  text: 'Waiting for the first<br>complete radar frame…', showarrow: false,
  xref: 'paper', yref: 'paper', x: .5, y: .5, xanchor: 'center',
  font: {color: '#ac9e94', size: 14}
}]}, config);

if (typeof window !== 'undefined' && window.addEventListener) {
  let resizeTimer;
  window.addEventListener('resize', function () {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(function () {
      plotPresentation = currentDelayDopplerPresentation();
      const changes = {
        margin: plotPresentation.margin,
        'font.size': plotPresentation.fontSize,
        'xaxis.title.text': plotPresentation.xTitle,
        'xaxis.title.font.size': plotPresentation.axisTitleSize,
        'xaxis.tickfont.size': plotPresentation.tickSize,
        'xaxis.nticks': plotPresentation.tickCount || undefined,
        'yaxis.title.text': plotPresentation.yTitle,
        'yaxis.title.font.size': plotPresentation.axisTitleSize,
        'yaxis.tickfont.size': plotPresentation.tickSize,
        'yaxis.nticks': plotPresentation.tickCount || undefined
      };
      for (const [key, value] of Object.entries(changes)) {
        const parts = key.split('.'), leaf = parts.pop();
        parts.reduce((target, part) => target[part], layout)[leaf] = value;
      }
      Plotly.relayout('data', changes);
      Plotly.restyle('data', {colorbar: plotPresentation.colorbar}, [0]);
      Plotly.restyle('data', {showscale: plotPresentation.showScale}, [0]);
    }, 100);
  });
}

// callback function
async function renderRadarMap(data) {
  const frameChannel = selectedIlluminator;
  const atomicFrame = data?.schema === 'vectorwarp-radar-display-frame-v1' ? data : null;
  if (atomicFrame) data = atomicFrame.map;
  const runningConfig = selectedIlluminatorConfig(await getRadarRuntimeConfig());
  const detectionEnabled = runningConfig.process?.detection?.enable !== false;
  const visible = overlayVisibility();
  // Bind overlays to the heatmap's exact processor frame. Waiting for these two
  // compact responses avoids clearing markers between independently selected
  // presentation snapshots and prevents adjacent-CPI data from being mixed.
  const adsbEnabled = runningConfig.truth?.adsb?.enabled === true;
  // Remote ID currently has no per-illuminator delay/Doppler geometry. Never
  // combine the selected FM lane with the legacy global transmitter model.
  const remoteIdEnabled = (!selectedIlluminator || selectedIlluminatorSynthetic) &&
    runningConfig.truth?.remote_id?.enabled === true;
  if (typeof document !== 'undefined') document.querySelectorAll('[data-overlay-kind="remote-id"]').forEach(marker => {
    marker.hidden = !remoteIdEnabled;
  });
  const adsbRequest = adsbEnabled ?
    fetchRadarJson(radarFrameUrl(urlAdsbData, data.timestamp)).catch(() => null) :
    Promise.resolve(null);
  const [detected, trackerFrame, data_remote_id] = await Promise.all([
    atomicFrame ? Promise.resolve(atomicFrame.detections) :
      detectionEnabled && visible.detections ? fetchRadarJson(radarFrameUrl(urlDetection, data.timestamp)).catch(() => null) : null,
    atomicFrame ? Promise.resolve(atomicFrame.tracks) :
      fetchRadarJson(radarFrameUrl(urlTracker, data.timestamp)).catch(() => null),
    remoteIdEnabled ? fetchRadarJson(radarFrameUrl(urlRemoteIdData, data.timestamp)).catch(() => null) : null
  ]);
  if (frameChannel !== selectedIlluminator) return false;
  // Never advance the heatmap while replacing a failed exact-frame radar
  // overlay request with an empty set. An empty, successfully returned frame
  // is authoritative; null here means transport/history failure. Keeping the
  // last complete visualization avoids making established tracks blink out
  // during a transient request and lets startRadarPlot retry this timestamp.
  if (trackerFrame === null ||
      (detectionEnabled && visible.detections && detected === null)) return false;
  data = applyClientMaxHold(data, runningConfig.display);
  detection = detected || {delay: [], doppler: []};
  const tracked = trackerOverlay(trackerFrame, runningConfig);
  if (!visible.tracks) {
    tracked.updated = {delay: [], doppler: [], hover: [], ids: []};
    tracked.coasting = {delay: [], doppler: [], hover: [], ids: []};
  }
  // A complete atomic radar frame must paint without waiting on a network-fed
  // truth overlay. Preserve the last confirmed visualization and refresh trace
  // 2 independently when ADS-B arrives; scoring/association remains server-side
  // and still uses exact event-time data.
  if (!atomicFrame) adsb = adsbOverlay(await adsbRequest, selectedIlluminator, adsbEnabled ? 10000 : 0);
  else if (!Array.isArray(adsb.delay))
    adsb = {delay: [], doppler: [], flight: [], customdata: [], hover: [], colors: [], ids: []};
  remoteId = {delay: [], doppler: [], customdata: [], hover: [], ids: []};
  const stableDrones = stabilizeAircraftSnapshots(Object.entries(data_remote_id || {})
      .map(([id, record]) => record?.hex ? record : {...record, hex: id}),
    {scope: 'delay-doppler-remote-id', graceMs: remoteIdEnabled ? 10000 : 0});
  for (const drone of stableDrones) {
    if (!drone || !Number.isFinite(Number(drone.delay)) || !Number.isFinite(Number(drone.doppler))) continue;
    remoteId.delay.push(drone.delay);
    remoteId.doppler.push(drone.doppler);
    remoteId.customdata.push(aircraftPlotData(drone));
    remoteId.ids.push(`remote-id:${aircraftIdentity(drone)}`);
    remoteId.hover.push(aircraftHoverText(drone, {delay: drone.delay, doppler: drone.doppler}));
  }

  // case draw new plot
  const gridKey = JSON.stringify([data.delay, data.doppler]);
  if (gridKey !== gridSignature) {

    // lock range before other trace
    var layout_update = {
      'xaxis.range': [data.delay[0], data.delay.slice(-1)[0]],
      'yaxis.range': [data.doppler[0], data.doppler.slice(-1)[0]]
    };
    await Plotly.relayout('data', layout_update);

    var trace1 = {
        z: data.data,
        x: data.delay,
        y: data.doppler,
        colorscale: 'Viridis',
        zauto: false,
        zmin: 0,
        zmax: Math.max(13, data.maxPower),
        showscale: plotPresentation.showScale,
        colorbar: plotPresentation.colorbar,
        type: 'heatmap'
    };
    var trace2 = {
        name: radarSemantics.labels.detection,
        legendrank: radarSemantics.legendRank.detection,
        x: detection.delay,
        y: detection.doppler,
        mode: 'markers',
        type: 'scatter',
        marker: {
          size: 8,
          opacity: 0.68,
          color: radarSemantics.colors.detection,
          symbol: 'circle'
        }
    };
    var trace3 = {
      name: radarSemantics.labels.adsb,
      legendrank: radarSemantics.legendRank.adsb,
      x: adsb.delay,
      y: adsb.doppler,
      mode: 'markers',
      type: 'scatter',
      marker: {
        size: 10,
        opacity: 0.78,
        color: adsb.colors,
        // Altitude colors match Map; circles indicate projected coordinates,
        // never a geographic aircraft pose or an independent radar position.
        symbol: 'circle',
        line: {color: '#10251f', width: 1.2}
      },
      customdata: adsb.customdata,
      text: adsb.hover,
      hovertemplate: '%{text}<extra></extra>'
    };
    var trace4 = {
      name: radarSemantics.labels.track,
      legendrank: radarSemantics.legendRank.track,
      x: tracked.updated.delay,
      y: tracked.updated.doppler,
      mode: 'markers',
      type: 'scatter',
      marker: {
        size: 11,
        opacity: 1,
        color: radarSemantics.colors.track,
        symbol: 'circle',
        line: {color: '#542012', width: 1.4}
      },
      text: tracked.updated.hover,
      hovertemplate: '%{text}<extra></extra>'
    };
    var trace5 = {
      name: radarSemantics.labels.coasting,
      legendrank: radarSemantics.legendRank.coasting,
      x: tracked.coasting.delay,
      y: tracked.coasting.doppler,
      mode: 'markers',
      type: 'scatter',
      marker: {
        size: 13,
        opacity: .72,
        color: radarSemantics.colors.coasting,
        symbol: 'circle-open',
        line: {color: radarSemantics.colors.coasting, width: 2}
      },
      text: tracked.coasting.hover,
      hovertemplate: '%{text}<extra></extra>'
    };
    var trace6 = {
      name: radarSemantics.labels.remoteId,
      legendrank: radarSemantics.legendRank.remoteId,
      x: remoteId.delay,
      y: remoteId.doppler,
      mode: 'markers',
      type: 'scatter',
      marker: {size: 14, opacity: .9, color: radarSemantics.colors.remoteId, symbol: 'diamond',
        line: {color: '#0d3535', width: 1.3}},
      customdata: remoteId.customdata,
      text: remoteId.hover,
      hovertemplate: '%{text}<extra></extra>'
    };

    var data_trace = [trace1, trace2, trace3, trace4, trace5, trace6];
    await Plotly.react('data', data_trace, {...layout, annotations: []}, config);
    gridSignature = gridKey;
  }
  // case update plot
  else {
    var trace_update = {
      x: [data.delay, detection.delay, adsb.delay, tracked.updated.delay, tracked.coasting.delay, remoteId.delay],
      y: [data.doppler, detection.doppler, adsb.doppler, tracked.updated.doppler, tracked.coasting.doppler, remoteId.doppler],
      z: [data.data, [], [], [], [], []],
      zmax: [Math.max(13, data.maxPower), [], [], [], [], []],
      text: [[], [], adsb.hover, tracked.updated.hover, tracked.coasting.hover, remoteId.hover],
      customdata: [[], [], adsb.customdata, [], [], remoteId.customdata]
    };
    await Plotly.update('data', trace_update);
    await Plotly.restyle('data', {'marker.color': [adsb.colors]}, [2]);
  }
  delayDopplerSelectionTraces = [
    {index: 2, ids: adsb.ids, size: 10, outline: '#10251f', width: 1.2},
    {index: 3, ids: tracked.updated.ids, size: 11, outline: '#542012', width: 1.4},
    {index: 4, ids: tracked.coasting.ids, size: 13, outline: radarSemantics.colors.coasting, width: 2},
    {index: 5, ids: remoteId.ids, size: 14, outline: '#0d3535', width: 1.3}
  ];
  bindDelayDopplerSelection();
  if (selectedDelayDopplerTarget) await refreshDelayDopplerSelection();
  if (atomicFrame && adsbEnabled) void adsbRequest.then(dataAdsb => {
    if (frameChannel !== selectedIlluminator || dataAdsb === null) return;
    const next = adsbOverlay(dataAdsb, frameChannel, 10000);
    adsb = next;
    delayDopplerSelectionTraces[0].ids = next.ids;
    const selection = delayDopplerSelectionStyle(delayDopplerSelectionTraces[0]);
    return Plotly.restyle('data', {x: [next.delay], y: [next.doppler],
      text: [next.hover], customdata: [next.customdata], 'marker.color': [next.colors],
      ...Object.fromEntries(Object.entries(selection).map(([key, value]) => [key, [value]]))}, [2]);
  }).catch(() => {});
  return true;
}

function configureMapChannel(context) {
  selectedIlluminator = context?.id || '';
  selectedIlluminatorSynthetic = context?.item?.api_illuminator === false;
  selectedIlluminatorSampleRate = Number(context?.item?.output_sample_rate_hz);
  urlDetection = radarDisplayUrl('detection', undefined, context);
  urlTracker = radarDisplayUrl('map', '/api/tracker', context);
  urlAdsbData = radarDisplayUrl('map', '/api/adsb/delay-doppler?overlay=1', context);
  const liveMapPath = context?.enabled && context.id && context.item?.api_illuminator !== false ?
    '/api/radar/frame' : mapProductPath;
  urlMap = radarDisplayUrl('map', mapPresentationUrl(liveMapPath,
    typeof window === 'undefined' ? 1280 : window.innerWidth), context);
  maxHoldState = null;
  gridSignature = '';
  selectedDelayDopplerTarget = null;
  delayDopplerSelectionTraces = [];
  adsb = {delay: [], doppler: [], flight: [], customdata: [], hover: [], colors: [], ids: []};
}
var vectorwarpMapReady = startRadarDisplay('map', renderRadarMap,
  {url: () => urlMap, configure: configureMapChannel}).then(controller => {
  window.startVectorWarpChannelRadar = controller.start;
  window.stopVectorWarpChannelRadar = controller.stop;
  return controller;
});
