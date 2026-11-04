var plotted = false;
var range_x = [];
var range_y = [];
var detectionHistory = [];

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

var vectorwarpDetectionReady = getIlluminatorContext().then(context => {
  urlDetection = liveApiUrl(radarPresentationUrl(
    selectedIlluminatorUrl('/api/detection', context)));
  radarUpdates = startRadarPlot(urlDetection, renderDetection);
  return radarUpdates;
});
