var nRows = null;
var range_x = [];
var range_y = [];

// setup API
var urlMap = liveApiUrl('/stash/iqdata');

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
      text: 'Frequency (MHz)',
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
      text: 'Timestamp',
      font: {
        size: 18
      }
    },
    ticks: '',
    ticksuffix: ' ',
    categoryorder: "total descending"
  }
};
var config = {
  responsive: true,
  displayModeBar: false
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

Plotly.newPlot('data', data, layout, config);

// callback function
var radarUpdates = startRadarPlot(urlMap, async function (data) {

  // case draw new plot
  if (data.frequency?.[0]?.length !== nRows) {
    // timestamp posix to js
    for (let i = 0; i < data.timestamp.length; i++)
    {
      data.timestamp[i] = new Date(data.timestamp[i]);
    }
    var trace1 = {
        x: data.frequency[0].map(khz => khz / 1000),
        y: data.timestamp,
        z: data.spectrum,
        colorscale: 'Jet',
        zauto: false,
        type: 'heatmap'
    };

    var data_trace = [trace1];
    await Plotly.newPlot('data', data_trace, layout, config);
    nRows = data.frequency?.[0]?.length;
  }
  // case update plot
  else {
    // timestamp posix to js
    for (let i = 0; i < data.timestamp.length; i++)
    {
      data.timestamp[i] = new Date(data.timestamp[i]);
    }
    var trace_update = {
      x: [data.frequency[0].map(khz => khz / 1000)],
      y: [data.timestamp],
      z: [data.spectrum]
    };
    await Plotly.update('data', trace_update);
  }
});
