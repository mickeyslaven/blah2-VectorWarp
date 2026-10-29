var gridSignature = '';
var range_x = [];
var range_y = [];

function delayDopplerPresentation(width) {
  const compact = Number.isFinite(width) && width <= 600;
  return compact ? {
    margin: {l: 42, r: 10, b: 42, t: 10, pad: 0},
    fontSize: 10,
    axisTitleSize: 11,
    xTitle: 'Range (km)',
    yTitle: 'Hz',
    tickSize: 9,
    tickCount: 5,
    showScale: false,
    colorbar: {
      title: {text: 'dB', side: 'right', font: {color: '#f3eee9', size: 10}},
      tickfont: {color: '#f3eee9', size: 8},
      outlinecolor: '#47362e', thickness: 8, len: .72, xpad: 2
    }
  } : {
    margin: {l: 92, r: 132, b: 50, t: 36, pad: 0},
    fontSize: 12,
    axisTitleSize: 18,
    xTitle: 'Bistatic Range (km)',
    yTitle: 'Bistatic Doppler (Hz)',
    tickSize: 12,
    tickCount: 0,
    showScale: true,
    colorbar: {
      title: {text: 'Power (dB)', side: 'right', font: {color: '#f3eee9', size: 13}},
      tickfont: {color: '#f3eee9', size: 11},
      outlinecolor: '#47362e', thickness: 14
    }
  };
}

function currentDelayDopplerPresentation() {
  return delayDopplerPresentation(typeof window === 'undefined' ? 1024 : window.innerWidth);
}

var plotPresentation = currentDelayDopplerPresentation();

function mapPresentationUrl(path, width) {
  if (!Number.isFinite(width) || width > 1024) return path;
  const separator = path.includes('?') ? '&' : '?';
  // The native 601x511 JSON map is about 1.8 MB per frame.  A peak-preserving
  // server presentation keeps tablet/phone transfer and Plotly work bounded
  // without changing detector processing or the underlying radar grid.
  return `${path}${separator}max_rows=128&max_cols=128`;
}

// setup API
var urlDetection = liveApiUrl(radarPresentationUrl('/api/detection'));
var urlTracker = liveApiUrl(radarPresentationUrl('/api/tracker'));
urlMap = liveApiUrl(radarPresentationUrl(mapPresentationUrl(urlMap,
  typeof window === 'undefined' ? 1280 : window.innerWidth)));
var urlAdsbData = liveApiUrl(radarPresentationUrl('/api/adsb/delay-doppler?overlay=1'));

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
var data = [
  {
    z: [[0, 0, 0], [0, 0, 0], [0, 0, 0]],
    colorscale: 'Jet',
    showscale: plotPresentation.showScale,
    type: 'heatmap'
  }
];
var detection = [];
var adsb = {};
var adsbCache = null;
var adsbReadAt = -Infinity;
var adsbPending = false;
var displayedMapTimestamp = null;
var detectionCache = null;
var detectionPending = false;
var trackerCache = null;
var trackerPending = false;
var overlayToleranceMs = 1;

function overlayFrameMatches(frame, timestamp, toleranceMs = 1) {
  const frameTimestamp = Number(frame?.frameTimestamp ?? frame?.timestamp);
  const targetTimestamp = Number(timestamp);
  return Number.isFinite(frameTimestamp) && Number.isFinite(targetTimestamp) &&
    Math.abs(frameTimestamp - targetTimestamp) <= toleranceMs;
}

function trackerOverlay(frame, runningConfig) {
  const sampleRate = Number(runningConfig?.capture?.fs);
  const kilometresPerBin = Number.isFinite(sampleRate) && sampleRate > 0 ?
    299792458 / sampleRate / 1000 : null;
  const visibleStates = new Set(['ACTIVE', 'ASSOCIATED', 'CONFIRMED']);
  const result = {delay: [], doppler: [], hover: []};
  if (kilometresPerBin === null) return result;
  for (const track of Array.isArray(frame?.data) ? frame.data : []) {
    const delay = Number(track?.delay), doppler = Number(track?.doppler);
    const state = String(track?.state || '').toUpperCase();
    if (!Number.isFinite(delay) || !Number.isFinite(doppler) ||
        !visibleStates.has(state)) continue;
    const range = delay * kilometresPerBin;
    result.delay.push(range);
    result.doppler.push(doppler);
    result.hover.push(`Radar track ${track.id ?? '—'}<br>${state}<br>${range.toFixed(2)} km · ${doppler.toFixed(1)} Hz`);
  }
  return result;
}

function currentAdsbOverlay(enabled, cpiSeconds) {
  if (!enabled) return null;
  const interval = Number.isFinite(cpiSeconds) && cpiSeconds > 0 ?
    Math.max(16, cpiSeconds * 1000) : 1000;
  if (!adsbPending && Date.now() - adsbReadAt >= interval) {
    adsbPending = true;
    adsbReadAt = Date.now();
    // An optional, slower truth service must not hold up radar frames.
    fetchRadarJson(urlAdsbData)
      .then(data => { adsbCache = data; })
      .catch(() => { adsbCache = null; })
      .finally(() => { adsbPending = false; });
  }
  return adsbCache;
}

Plotly.react('data', data, layout, config);

function requestDetectionOverlay(timestamp, enabled) {
  if (!enabled || detectionPending) return;
  detectionPending = true;
  // Detection markers are optional decoration. Fetch them beside the heatmap,
  // never in front of it, so a slow marker response cannot add latency to the
  // CPI-paced radar image. Independent API reads may select adjacent frames,
  // so accept at most one loaded CPI while rejecting genuinely stale markers.
  fetchRadarJson(urlDetection)
    .then(async latest => {
      detectionCache = latest;
      if (!overlayFrameMatches(latest, timestamp, overlayToleranceMs) ||
          displayedMapTimestamp !== timestamp) return;
      await Plotly.restyle('data', {x: [latest.delay], y: [latest.doppler]}, [1]);
    })
    .catch(() => {})
    .finally(() => { detectionPending = false; });
}

async function applyDetectionOverlay(timestamp) {
  if (!overlayFrameMatches(detectionCache, timestamp, overlayToleranceMs) ||
      displayedMapTimestamp !== timestamp) return false;
  await Plotly.restyle('data', {x: [detectionCache.delay], y: [detectionCache.doppler]}, [1]);
  return true;
}

function requestTrackerOverlay(timestamp, runningConfig) {
  if (trackerPending) return;
  trackerPending = true;
  fetchRadarJson(urlTracker)
    .then(async latest => {
      trackerCache = latest;
      if (!overlayFrameMatches(latest, timestamp, overlayToleranceMs) ||
          displayedMapTimestamp !== timestamp) return;
      const overlay = trackerOverlay(latest, runningConfig);
      await Plotly.restyle('data', {x: [overlay.delay], y: [overlay.doppler],
        text: [overlay.hover]}, [3]);
    })
    .catch(() => {})
    .finally(() => { trackerPending = false; });
}

async function applyTrackerOverlay(timestamp, runningConfig) {
  if (!overlayFrameMatches(trackerCache, timestamp, overlayToleranceMs) ||
      displayedMapTimestamp !== timestamp) return false;
  const overlay = trackerOverlay(trackerCache, runningConfig);
  await Plotly.restyle('data', {x: [overlay.delay], y: [overlay.doppler],
    text: [overlay.hover]}, [3]);
  return true;
}

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
var radarUpdates = startRadarPlot(urlMap, async function (data) {
  const runningConfig = await getRadarRuntimeConfig();
  overlayToleranceMs = Math.max(1,
    (Number(runningConfig.process?.data?.cpi) || 0) * 1000 + 5);
  const detectionEnabled = runningConfig.process?.detection?.enable !== false;
  const detected = detectionEnabled &&
    overlayFrameMatches(detectionCache, data.timestamp, overlayToleranceMs) ?
    detectionCache : null;
  requestDetectionOverlay(data.timestamp, detectionEnabled);
  const tracked = overlayFrameMatches(trackerCache, data.timestamp, overlayToleranceMs) ?
    trackerOverlay(trackerCache, runningConfig) : {delay: [], doppler: [], hover: []};
  requestTrackerOverlay(data.timestamp, runningConfig);
  // Separate TCP streams may arrive at different times; never put old
  // detections onto a newly arrived map.
  detection = detected || {delay: [], doppler: []};
  adsb = {delay: [], doppler: [], flight: [], customdata: [], hover: []};
  const adsbEnabled = runningConfig.truth?.adsb?.enabled === true;
  const data_adsb = currentAdsbOverlay(adsbEnabled,
    runningConfig.process?.data?.cpi);
  const stableAircraft = stabilizeAircraftSnapshots(Object.entries(data_adsb || {})
      .map(([hex, record]) => record?.hex ? record : {...record, hex}),
    {scope: 'delay-doppler', graceMs: adsbEnabled ? 10000 : 0});
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
        x: detection.delay,
        y: detection.doppler,
        mode: 'markers',
        type: 'scatter',
        marker: {
          size: 16,
          opacity: 0.6,
          color: '#ff9b65',
          symbol: 'circle'
        }
    };
    var trace3 = {
      x: adsb.delay,
      y: adsb.doppler,
      mode: 'markers',
      type: 'scatter',
      marker: {
        size: 16,
        opacity: 0.78,
        color: '#4dd4b0',
        symbol: 'circle',
        line: {color: '#10251f', width: 1.2}
      },
      customdata: adsb.customdata,
      text: adsb.hover,
      hovertemplate: '%{text}<extra></extra>'
    };
    var trace4 = {
      x: tracked.delay,
      y: tracked.doppler,
      mode: 'markers',
      type: 'scatter',
      marker: {
        size: 14,
        opacity: .95,
        color: '#ff6f3c',
        symbol: 'circle',
        line: {color: '#2b120a', width: 1.5}
      },
      text: tracked.hover,
      hovertemplate: '%{text}<extra></extra>'
    };

    var data_trace = [trace1, trace2, trace3, trace4];
    await Plotly.react('data', data_trace, layout, config);
    gridSignature = gridKey;
  }
  // case update plot
  else {
    var trace_update = {
      x: [data.delay, detection.delay, adsb.delay, tracked.delay],
      y: [data.doppler, detection.doppler, adsb.doppler, tracked.doppler],
      z: [data.data, [], [], []],
      zmax: [Math.max(13, data.maxPower), [], [], []],
      text: [[], [], adsb.hover, tracked.hover],
      customdata: [[], [], adsb.customdata, []]
    };
    await Plotly.update('data', trace_update);
  }
  displayedMapTimestamp = data.timestamp;
  // The marker request often wins the race against the heavier Plotly heatmap
  // render. Apply its cached exact-frame result after the marker trace exists;
  // otherwise every detection is discarded one frame early and never shown.
  await applyDetectionOverlay(data.timestamp);
  await applyTrackerOverlay(data.timestamp, runningConfig);
  return true;
});
