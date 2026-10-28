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
var urlDetection = liveApiUrl('/api/detection');
urlMap = liveApiUrl(mapPresentationUrl(urlMap,
  typeof window === 'undefined' ? 1280 : window.innerWidth));
var urlAdsbData = liveApiUrl('/api/adsb/delay-doppler?presentation=overlay');

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
var detectionFollowupTimer = null;

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

function scheduleDetectionOverlay(timestamp) {
  if (typeof window === 'undefined' || typeof window.setTimeout !== 'function') return;
  if (detectionFollowupTimer) window.clearTimeout(detectionFollowupTimer);
  detectionFollowupTimer = window.setTimeout(async function () {
    detectionFollowupTimer = null;
    if (displayedMapTimestamp !== timestamp) return;
    const latest = await fetchRadarJson(urlDetection).catch(() => null);
    if (latest?.timestamp !== timestamp || displayedMapTimestamp !== timestamp) return;
    await Plotly.restyle('data', {x: [latest.delay], y: [latest.doppler]}, [1]);
  }, 150);
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
  const detected = runningConfig.process?.detection?.enable !== false ?
    await fetchRadarJson(urlDetection).catch(() => null) : null;
  // Separate TCP streams may arrive at different times; never put old
  // detections onto a newly arrived map.
  detection = detected?.timestamp === data.timestamp ? detected : {delay: [], doppler: []};
  adsb = {delay: [], doppler: [], flight: []};
  const data_adsb = currentAdsbOverlay(runningConfig.truth?.adsb?.enabled === true,
    runningConfig.process?.data?.cpi);
  if (data_adsb) {
    for (const aircraft of Object.values(data_adsb)) {
      if (aircraft && 'doppler' in aircraft) {
        adsb.delay.push(aircraft.delay);
        adsb.doppler.push(aircraft.doppler);
        adsb.flight.push(aircraft.flight);
      }
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
          opacity: 0.6
        }
    };
    var trace3 = {
      x: adsb.delay,
      y: adsb.doppler,
      mode: 'markers',
      type: 'scatter',
      marker: {
        size: 16,
        opacity: 0.6
      }
  };

    var data_trace = [trace1, trace2, trace3];
    await Plotly.react('data', data_trace, layout, config);
    gridSignature = gridKey;
  }
  // case update plot
  else {
    var trace_update = {
      x: [data.delay, detection.delay, adsb.delay],
      y: [data.doppler, detection.doppler, adsb.doppler],
      z: [data.data, [], []],
      zmax: [Math.max(13, data.maxPower), [], []],
      text: [[], [], adsb.flight]
    };
    await Plotly.update('data', trace_update);
  }
  displayedMapTimestamp = data.timestamp;
  if (runningConfig.process?.detection?.enable !== false && detected?.timestamp !== data.timestamp)
    scheduleDetectionOverlay(data.timestamp);
  return true;
});
