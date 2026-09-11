var plottedKeys = '';

// setup API
var urlTiming = liveApiUrl('/stash/timing');

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
  },
  legend: {
    orientation: "h",
    bgcolor: "#29201c",
    bordercolor: "#47362e",
    borderwidth: 1
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

Plotly.newPlot('data', data, layout, config);

// callback function
var radarUpdates = startRadarPlot(urlTiming, async function (data) {
  const keys = Object.keys(data).filter(key =>
    Array.isArray(data[key]) && !['timestamp', 'uptime_s', 'uptime_days'].includes(key));
  const signature = JSON.stringify(keys);

  // case draw new plot
  if (signature !== plottedKeys) {

    // timestamp posix to js
    for (let i = 0; i < data["timestamp"].length; i++)
    {
      data["timestamp"][i] = new Date(data["timestamp"][i]);
    }

    var data_trace = [];
    for (let i = 0; i < keys.length; i++) {
      var trace = {
        x: data["timestamp"],
        y: data[keys[i]],
        mode: 'lines+markers',
        type: 'scatter',
        name: keys[i],
        line: {
          width: 5
        },
        marker: {
          size: 12
        },
      };
      data_trace.push(trace);
    }

    await Plotly.newPlot('data', data_trace, layout, config);
    plottedKeys = signature;
  }
  // case update plot
  else {
    // timestamp posix to js
    for (let i = 0; i < data["timestamp"].length; i++)
    {
      data["timestamp"][i] = new Date(data["timestamp"][i]);
    }
    var xVec = [];
    var yVec = [];
    for (let i = 0; i < keys.length; i++) {
      xVec.push(data["timestamp"]);
      yVec.push(data[keys[i]]);
    }
    var trace_update = {
      x: xVec,
      y: yVec
    };
    await Plotly.update('data', trace_update);
  }
});
