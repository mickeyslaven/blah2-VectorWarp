var plotted = false;
var range_x = [];
var range_y = [];

// setup API
var urlDetection = liveApiUrl('/stash/detection');

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
var radarUpdates = startRadarPlot(urlDetection, async function (data) {

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
    await Plotly.newPlot('data', data_trace, layout, config);
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
});
