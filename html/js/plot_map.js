var gridSignature = '';
var range_x = [];
var range_y = [];

// setup API
var urlDetection = liveApiUrl('/api/detection');
urlMap = liveApiUrl(urlMap);
var urlAdsbData = liveApiUrl('/api/adsb/delay-doppler');

// setup plotly
var layout = {
  font: {color: '#f3eee9'},
  autosize: true,
  margin: {
    l: 92,
    r: 132,
    b: 50,
    t: 36,
    pad: 0
  },
  hoverlabel: {
    namelength: 0
  },
  plot_bgcolor: "rgba(0,0,0,0)",
  paper_bgcolor: "rgba(0,0,0,0)",
  annotations: [],
  displayModeBar: false,
  xaxis: {
    gridcolor: '#47362e', zerolinecolor: '#47362e',
    title: {
      text: 'Bistatic Range (km)',
      font: {
        size: 18
      }
    },
    ticks: '',
    side: 'bottom'
  },
  yaxis: {
    gridcolor: '#47362e', zerolinecolor: '#47362e',
    title: {
      text: 'Bistatic Doppler (Hz)',
      font: {
        size: 18
      }
    },
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
    type: 'heatmap'
  }
];
var detection = [];
var adsb = {};
var adsbCache = null;
var adsbReadAt = -Infinity;
var adsbPending = false;
var pendingDetectionTimestamp = null;

function currentAdsbOverlay(enabled) {
  if (!enabled) return null;
  if (!adsbPending && Date.now() - adsbReadAt >= 1000) {
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

Plotly.newPlot('data', data, layout, config);

// callback function
var radarUpdates = startRadarPlot(urlMap, async function (data) {
  const runningConfig = await getRadarRuntimeConfig();
  const detected = runningConfig.process?.detection?.enable !== false ?
    await fetchRadarJson(urlDetection).catch(() => null) : null;
  // The map was already painted on the first attempt. While its detection
  // stream catches up, avoid uploading the same heatmap on every retry.
  if (pendingDetectionTimestamp === data.timestamp) {
    if (runningConfig.process?.detection?.enable === false) {
      pendingDetectionTimestamp = null;
      return true;
    }
    if (detected?.timestamp !== data.timestamp) return false;
    await Plotly.restyle('data', {
      x: [detected.delay], y: [detected.doppler]
    }, [1]);
    pendingDetectionTimestamp = null;
    return true;
  }
  // Separate TCP streams may arrive at different times; never put old
  // detections onto a newly arrived map.
  detection = detected?.timestamp === data.timestamp ? detected : {delay: [], doppler: []};
  adsb = {delay: [], doppler: [], flight: []};
  const data_adsb = currentAdsbOverlay(runningConfig.truth?.adsb?.enabled === true);
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
        colorbar: {
          title: {text: 'Power (dB)', side: 'right', font: {color: '#f3eee9', size: 13}},
          tickfont: {color: '#f3eee9', size: 11},
          outlinecolor: '#47362e',
          thickness: 14
        },
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
    await Plotly.newPlot('data', data_trace, layout, config);
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
  const complete = runningConfig.process?.detection?.enable === false ||
    detected?.timestamp === data.timestamp;
  pendingDetectionTimestamp = complete ? null : data.timestamp;
  return complete;
});
