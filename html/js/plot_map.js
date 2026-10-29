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
    fontSize: 10,
    axisTitleSize: 11,
    xTitle: 'Range (km)',
    yTitle: 'Hz',
    tickSize: 9,
    tickCount: 4,
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
  return delayDopplerPresentation(typeof window === 'undefined' ? 1024 : window.innerWidth,
    typeof window === 'undefined' ? 768 : window.innerHeight);
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

Plotly.react('data', data, layout, config);

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
  const detectionEnabled = runningConfig.process?.detection?.enable !== false;
  const visible = overlayVisibility();
  // Bind overlays to the heatmap's exact processor frame. Waiting for these two
  // compact responses avoids clearing markers between independently selected
  // presentation snapshots and prevents adjacent-CPI data from being mixed.
  const adsbEnabled = runningConfig.truth?.adsb?.enabled === true;
  const [detected, trackerFrame, data_adsb] = await Promise.all([
    detectionEnabled && visible.detections ? fetchRadarJson(exactFrameUrl(urlDetection, data.timestamp)).catch(() => null) : null,
    fetchRadarJson(exactFrameUrl(urlTracker, data.timestamp)).catch(() => null),
    adsbEnabled ? fetchRadarJson(exactFrameUrl(urlAdsbData, data.timestamp)).catch(() => null) : null
  ]);
  detection = detected || {delay: [], doppler: []};
  const tracked = trackerOverlay(trackerFrame, runningConfig);
  if (!visible.tracks) {
    tracked.updated = {delay: [], doppler: [], hover: []};
    tracked.coasting = {delay: [], doppler: [], hover: []};
  }
  adsb = {delay: [], doppler: [], flight: [], customdata: [], hover: []};
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
        size: 16,
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

    var data_trace = [trace1, trace2, trace3, trace4, trace5];
    await Plotly.react('data', data_trace, layout, config);
    gridSignature = gridKey;
  }
  // case update plot
  else {
    var trace_update = {
      x: [data.delay, detection.delay, adsb.delay, tracked.updated.delay, tracked.coasting.delay],
      y: [data.doppler, detection.doppler, adsb.doppler, tracked.updated.doppler, tracked.coasting.doppler],
      z: [data.data, [], [], [], []],
      zmax: [Math.max(13, data.maxPower), [], [], [], []],
      text: [[], [], adsb.hover, tracked.updated.hover, tracked.coasting.hover],
      customdata: [[], [], adsb.customdata, [], []]
    };
    await Plotly.update('data', trace_update);
  }
  return true;
});
