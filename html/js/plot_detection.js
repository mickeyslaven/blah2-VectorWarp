var plotted = false;
var range_x = [];
var range_y = [];
var detectionHistory = [];

function detectionSearchWindow(config) {
  const ambiguity = config?.process?.ambiguity || {};
  const sampleRate = Number(config?.capture?.fs);
  const delayMin = Number(ambiguity.delayMin);
  const delayMax = Number(ambiguity.delayMax);
  const dopplerMin = Number(ambiguity.dopplerMin);
  const dopplerMax = Number(ambiguity.dopplerMax);
  // Native delay is excess-path samples, so c/fs is kilometres per bin.
  const kmPerBin = Number.isFinite(sampleRate) && sampleRate > 0 ? 299792.458 / sampleRate : null;
  const x = kmPerBin && Number.isFinite(delayMin) && Number.isFinite(delayMax) && delayMax > delayMin ?
    [Math.max(0, delayMin * kmPerBin), delayMax * kmPerBin] : null;
  const y = Number.isFinite(dopplerMin) && Number.isFinite(dopplerMax) && dopplerMax > dopplerMin ?
    [dopplerMin, dopplerMax] : null;
  return {x, y};
}

// setup API
var urlDetection = null;

function accumulateDetectionHistory(frame) {
  if (typeof window === 'undefined' || window.vectorwarpDetectionHistory !== true) return frame;
  const stamp = Array.isArray(frame?.timestamp) ? frame.timestamp.at(-1) : frame?.timestamp;
  const delays = Array.isArray(frame?.delay) ? frame.delay : [];
  const dopplers = Array.isArray(frame?.doppler) ? frame.doppler : [];
  delays.forEach((delay, index) => detectionHistory.push({timestamp: stamp,
    delay, doppler: dopplers[index]}));
  detectionHistory = detectionHistory.slice(-2000);
  return {timestamp: detectionHistory.map(item => item.timestamp),
    delay: detectionHistory.map(item => item.delay),
    doppler: detectionHistory.map(item => item.doppler)};
}

// setup plotly
var layout = {
  font: {color: '#f3eee9'},
  autosize: true,
  margin: {
    l: 92,
    r: 50,
    b: 50,
    t: 10,
    pad: 0
  },
  hoverlabel: {
    namelength: 0
  },
  plot_bgcolor: "rgba(0,0,0,0)",
  paper_bgcolor: "rgba(0,0,0,0)",
  annotations: [],
  uirevision: 'vectorwarp-detections',
  displayModeBar: false,
  xaxis: {
    gridcolor: '#47362e', zerolinecolor: '#47362e',
    title: {
      text: xTitle,
      font: {
        size: 18
      }
    },
    showgrid: false,
    ticks: '',
    side: 'bottom'
  },
  yaxis: {
    gridcolor: '#47362e', zerolinecolor: '#47362e',
    title: {
      text: yTitle,
      font: {
        size: 18
      }
    },
    showgrid: false,
    ticks: '',
    ticksuffix: ' ',
    categoryorder: "total descending"
  }
};
var config = {
  responsive: true,
  displayModeBar: false,
  scrollZoom: true
}

// setup plotly data
var data = [
  {
    z: [[0, 0, 0], [0, 0, 0], [0, 0, 0]],
    colorscale: 'Jet',
    type: 'heatmap'
  }
];

Plotly.react('data', data, layout, config);

// callback function
var radarUpdates = null;
async function renderDetection(data) {
  data = accumulateDetectionHistory(data);

  // case draw new plot
  if (!plotted) {

    // timestamp posix to js
    if (xVariable === "timestamp")
    {
      for (let i = 0; i < data[xVariable].length; i++)
      {
        data[xVariable][i] = new Date(data[xVariable][i]);
      }
    }

    var trace1 = {
        x: data[xVariable],
        y: data[yVariable],
        mode: 'markers',
        type: 'scatter'
    };

    var data_trace = [trace1];
    await Plotly.react('data', data_trace, layout, config);
    plotted = true;
  }
  // case update plot
  else {
    // timestamp posix to js
    if (xVariable === "timestamp")
    {
      for (let i = 0; i < data[xVariable].length; i++)
      {
        data[xVariable][i] = new Date(data[xVariable][i]);
      }
    }
    var trace_update = {
      x: [data[xVariable]],
      y: [data[yVariable]]
    };
    await Plotly.update('data', trace_update);
  }
}

async function configureDetectionChannel(context) {
  detectionHistory = [];
  plotted = false;
  urlDetection = liveApiUrl(radarPresentationUrl(
    selectedIlluminatorUrl('/api/detection', context)));
  if (typeof window !== 'undefined' && window.vectorwarpDetectionHistory !== true &&
      typeof getRadarRuntimeConfig === 'function') {
    try {
      const runtime = await getRadarRuntimeConfig();
      const selected = typeof selectedIlluminatorConfig === 'function' ?
        selectedIlluminatorConfig(runtime, context) : runtime;
      const search = detectionSearchWindow(selected);
      if (search.x) {
        layout.xaxis.autorange = false;
        layout.xaxis.range = search.x;
      }
      if (search.y) {
        layout.yaxis.autorange = false;
        layout.yaxis.range = search.y;
      }
      if (search.x || search.y) await Plotly.relayout('data', {
        ...(search.x ? {'xaxis.autorange': false, 'xaxis.range': search.x} : {}),
        ...(search.y ? {'yaxis.autorange': false, 'yaxis.range': search.y} : {})
      });
    } catch (_) { /* Live detections still render if runtime metadata is delayed. */ }
  }
}
window.addEventListener('vectorwarp:illuminator-change', event => { void configureDetectionChannel(event.detail); });
var vectorwarpDetectionReady = getIlluminatorContext().then(async context => {
  await configureDetectionChannel(context);
  radarUpdates = startRadarPlot(() => urlDetection, renderDetection);
  return radarUpdates;
});
