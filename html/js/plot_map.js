var gridSignature = '';
var range_x = [];
var range_y = [];

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
var maxHoldGrid = null;

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

function applyClientMaxHold(frame) {
  if (typeof window === 'undefined' || window.vectorwarpMapMode !== 'maxhold' ||
      !Array.isArray(frame?.data)) return frame;
  const sameShape = Array.isArray(maxHoldGrid) && maxHoldGrid.length === frame.data.length &&
    maxHoldGrid.every((row, index) => Array.isArray(row) && row.length === frame.data[index]?.length);
  if (!sameShape) maxHoldGrid = frame.data.map(row => row.slice());
  else maxHoldGrid = frame.data.map((row, rowIndex) => row.map((value, columnIndex) =>
    Math.max(Number(value), Number(maxHoldGrid[rowIndex][columnIndex]) * .98)));
  // A full desktop map contains more than 300k cells. Spreading that array
  // into Math.max exceeds JavaScript's argument limit even though the exact
  // same selected-lane frame works after mobile downsampling.
  return {...frame, data: maxHoldGrid,
    maxPower: finiteGridMaximum(maxHoldGrid, Number(frame.maxPower) || 0)};
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
function exactFrameUrl(path, timestamp) {
  const url = new URL(path, typeof window === 'undefined' ?
    'http://vectorwarp.invalid' : window.location.href);
  url.searchParams.set('frame_timestamp', String(timestamp));
  return path.startsWith('http://') || path.startsWith('https://') ? url.href :
    `${url.pathname}${url.search}`;
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
    updated: {delay: [], doppler: [], hover: []},
    coasting: {delay: [], doppler: [], hover: []}
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
    group.hover.push(`Radar track ${track.id ?? '—'}<br>${state}<br>${range.toFixed(2)} km · ${doppler.toFixed(1)} Hz`);
  }
  return result;
}

Plotly.react('data', data, {...layout, annotations: [{
  text: 'Waiting for the first complete radar frame…', showarrow: false,
  font: {color: '#ac9e94', size: 16}
}]}, config);

if (typeof window !== 'undefined' && window.addEventListener) {
  let resizeTimer;
  window.addEventListener('resize', function () {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(function () {
      plotPresentation = currentDelayDopplerPresentation();
      Plotly.relayout('data', {
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
      });
      Plotly.restyle('data', {colorbar: plotPresentation.colorbar}, [0]);
      Plotly.restyle('data', {showscale: plotPresentation.showScale}, [0]);
    }, 100);
  });
}

// callback function
var radarUpdates = null;
async function renderRadarMap(data) {
  const frameChannel = selectedIlluminator;
  const atomicFrame = data?.schema === 'vectorwarp-radar-display-frame-v1' ? data : null;
  if (atomicFrame) data = atomicFrame.map;
  data = applyClientMaxHold(data);
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
    fetchRadarJson(exactFrameUrl(urlAdsbData, data.timestamp)).catch(() => null) :
    Promise.resolve(null);
  const [detected, trackerFrame, data_remote_id] = await Promise.all([
    atomicFrame ? Promise.resolve(atomicFrame.detections) :
      detectionEnabled && visible.detections ? fetchRadarJson(exactFrameUrl(urlDetection, data.timestamp)).catch(() => null) : null,
    atomicFrame ? Promise.resolve(atomicFrame.tracks) :
      fetchRadarJson(exactFrameUrl(urlTracker, data.timestamp)).catch(() => null),
    remoteIdEnabled ? fetchRadarJson(exactFrameUrl(urlRemoteIdData, data.timestamp)).catch(() => null) : null
  ]);
  if (frameChannel !== selectedIlluminator) return false;
  // Never advance the heatmap while replacing a failed exact-frame radar
  // overlay request with an empty set. An empty, successfully returned frame
  // is authoritative; null here means transport/history failure. Keeping the
  // last complete visualization avoids making established tracks blink out
  // during a transient request and lets startRadarPlot retry this timestamp.
  if (trackerFrame === null ||
      (detectionEnabled && visible.detections && detected === null)) return false;
  detection = detected || {delay: [], doppler: []};
  const tracked = trackerOverlay(trackerFrame, runningConfig);
  if (!visible.tracks) {
    tracked.updated = {delay: [], doppler: [], hover: []};
    tracked.coasting = {delay: [], doppler: [], hover: []};
  }
  // A complete atomic radar frame must paint without waiting on a network-fed
  // truth overlay. Preserve the last confirmed visualization and refresh trace
  // 2 independently when ADS-B arrives; scoring/association remains server-side
  // and still uses exact event-time data.
  const data_adsb = atomicFrame ? null : await adsbRequest;
  if (!atomicFrame || !Array.isArray(adsb.delay))
    adsb = {delay: [], doppler: [], flight: [], customdata: [], hover: []};
  const stableAircraft = atomicFrame ? [] : stabilizeAircraftSnapshots(Object.entries(data_adsb || {})
      .map(([hex, record]) => record?.hex ? record : {...record, hex}),
    {scope: `delay-doppler:${selectedIlluminator || 'single'}`,
      graceMs: adsbEnabled ? 10000 : 0});
  for (const aircraft of stableAircraft) {
      if (aircraft && 'doppler' in aircraft) {
        adsb.delay.push(aircraft.delay);
        adsb.doppler.push(aircraft.doppler);
        adsb.flight.push(aircraftLabel(aircraft));
        adsb.customdata.push(aircraftPlotData(aircraft));
        adsb.hover.push(aircraftHoverText(aircraft,
          {delay: aircraft.delay, doppler: aircraft.doppler}));
      }
  }
  remoteId = {delay: [], doppler: [], customdata: [], hover: []};
  const stableDrones = stabilizeAircraftSnapshots(Object.entries(data_remote_id || {})
      .map(([id, record]) => record?.hex ? record : {...record, hex: id}),
    {scope: 'delay-doppler-remote-id', graceMs: remoteIdEnabled ? 10000 : 0});
  for (const drone of stableDrones) {
    if (!drone || !Number.isFinite(Number(drone.delay)) || !Number.isFinite(Number(drone.doppler))) continue;
    remoteId.delay.push(drone.delay);
    remoteId.doppler.push(drone.doppler);
    remoteId.customdata.push(aircraftPlotData(drone));
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
        name: 'Raw detection',
        x: detection.delay,
        y: detection.doppler,
        mode: 'markers',
        type: 'scatter',
        marker: {
          size: 8,
          opacity: 0.68,
          color: '#f4b183',
          symbol: 'circle'
        }
    };
    var trace3 = {
      name: 'ADS-B aircraft',
      x: adsb.delay,
      y: adsb.doppler,
      mode: 'markers',
      type: 'scatter',
      marker: {
        size: 10,
        opacity: 0.78,
        color: '#4dd4b0',
        // All externally sourced aircraft truth remains a green dot. MLAT is
        // identified in hover/details rather than changing the radar grammar.
        symbol: 'circle',
        line: {color: '#10251f', width: 1.2}
      },
      customdata: adsb.customdata,
      text: adsb.hover,
      hovertemplate: '%{text}<extra></extra>'
    };
    var trace4 = {
      name: 'Updated radar track',
      x: tracked.updated.delay,
      y: tracked.updated.doppler,
      mode: 'markers',
      type: 'scatter',
      marker: {
        size: 11,
        opacity: 1,
        color: '#ff7a45',
        symbol: 'circle',
        line: {color: '#542012', width: 1.4}
      },
      text: tracked.updated.hover,
      hovertemplate: '%{text}<extra></extra>'
    };
    var trace5 = {
      name: 'Coasting radar track',
      x: tracked.coasting.delay,
      y: tracked.coasting.doppler,
      mode: 'markers',
      type: 'scatter',
      marker: {
        size: 13,
        opacity: .72,
        color: '#ff7a45',
        symbol: 'circle-open',
        line: {color: '#ff7a45', width: 2}
      },
      text: tracked.coasting.hover,
      hovertemplate: '%{text}<extra></extra>'
    };
    var trace6 = {
      name: 'Remote ID drone',
      x: remoteId.delay,
      y: remoteId.doppler,
      mode: 'markers',
      type: 'scatter',
      marker: {size: 14, opacity: .9, color: '#20dddc', symbol: 'diamond',
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
  }
  if (atomicFrame && adsbEnabled) void adsbRequest.then(dataAdsb => {
    if (frameChannel !== selectedIlluminator || dataAdsb === null) return;
    const next = {delay: [], doppler: [], flight: [], customdata: [], hover: []};
    const stable = stabilizeAircraftSnapshots(Object.entries(dataAdsb || {})
        .map(([hex, record]) => record?.hex ? record : {...record, hex}),
      {scope: `delay-doppler:${frameChannel || 'single'}`, graceMs: 10000});
    for (const aircraft of stable) {
      if (!aircraft || !('doppler' in aircraft)) continue;
      next.delay.push(aircraft.delay);
      next.doppler.push(aircraft.doppler);
      next.flight.push(aircraftLabel(aircraft));
      next.customdata.push(aircraftPlotData(aircraft));
      next.hover.push(aircraftHoverText(aircraft,
        {delay: aircraft.delay, doppler: aircraft.doppler}));
    }
    adsb = next;
    return Plotly.restyle('data', {x: [next.delay], y: [next.doppler],
      text: [next.hover], customdata: [next.customdata]}, [2]);
  }).catch(() => {});
  return true;
}

function configureMapChannel(context) {
  selectedIlluminator = context?.id || '';
  selectedIlluminatorSynthetic = context?.item?.api_illuminator === false;
  selectedIlluminatorSampleRate = Number(context?.item?.output_sample_rate_hz);
  const product = path => selectedIlluminatorUrl(path, context);
  urlDetection = liveApiUrl(radarPresentationUrl(product('/api/detection')));
  urlTracker = liveApiUrl(radarPresentationUrl(product('/api/tracker')));
  urlAdsbData = liveApiUrl(radarPresentationUrl(product('/api/adsb/delay-doppler?overlay=1')));
  const liveMapPath = context?.enabled && context.id && context.item?.api_illuminator !== false ?
    '/api/radar/frame' : mapProductPath;
  urlMap = liveApiUrl(radarPresentationUrl(mapPresentationUrl(product(liveMapPath),
    typeof window === 'undefined' ? 1280 : window.innerWidth)));
  maxHoldGrid = null;
  gridSignature = '';
  adsb = {delay: [], doppler: [], flight: [], customdata: [], hover: []};
}
window.addEventListener('vectorwarp:illuminator-change', event => configureMapChannel(event.detail));
var vectorwarpMapReady = getIlluminatorContext().then(context => {
  configureMapChannel(context);
  window.startVectorWarpChannelRadar = () => {
    if (!radarUpdates) radarUpdates = startRadarPlot(() => urlMap, renderRadarMap);
    return radarUpdates;
  };
  window.stopVectorWarpChannelRadar = () => {
    radarUpdates?.stop(); radarUpdates = null;
  };
  radarUpdates = window.startVectorWarpChannelRadar();
  return radarUpdates;
});
