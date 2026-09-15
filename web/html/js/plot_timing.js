var timestamp = -1;
// Signature of the trace set currently plotted. The stash has no nRows, so the
// old `data.nRows != nRows` test compared undefined against undefined and was
// false after the first fetch: traces were built once and a field that
// appeared later, or stopped appearing, was never picked up.
var plottedKeys = '';
var keys = [];
var host = window.location.hostname;
var isLocalHost = is_localhost(host);

// setup API
var urlTimestamp;
var urlTiming;
if (isLocalHost) {
  urlTimestamp = '//' + host + ':3000/api/timestamp';
} else {
  urlTimestamp = '/api/timestamp';
}
if (isLocalHost) {
  urlTiming = '//' + host + ':3000/stash/timing';
} else {
  urlTiming = '/stash/timing';
}

// setup plotly
var layout = {
  autosize: false,
  margin: {
    l: 50,
    r: 50,
    b: 50,
    t: 10,
    pad: 0
  },
  hoverlabel: {
    namelength: 0
  },
  width: document.getElementById('data').offsetWidth,
  height: document.getElementById('data').offsetHeight,
  plot_bgcolor: "#ffffff",
  paper_bgcolor: "#ffffff",
  font: {
    family: "-apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif",
    color: "#1a1a18"
  },
  annotations: [],
  displayModeBar: false,
  xaxis: {
    title: {
      text: xTitle,
      font: {
        size: 16
      }
    },
    showgrid: false,
    ticks: '',
    side: 'bottom'
  },
  yaxis: {
    title: {
      text: yTitle,
      font: {
        size: 16
      }
    },
    showgrid: false,
    ticks: '',
    ticksuffix: ' ',
    autosize: false,
    categoryorder: "total descending"
  },
  // Duty cycle is a percentage, so it cannot share a millisecond axis. 0-100
  // is fixed rather than autoscaled: the whole point is where the value sits
  // against "keeping up with the receiver", and an autoscaled axis hides that.
  yaxis2: {
    title: {
      text: 'duty cycle (%)',
      font: {
        size: 16
      }
    },
    overlaying: 'y',
    side: 'right',
    range: [0, 100],
    showgrid: false,
    ticks: '',
    ticksuffix: '%'
  },
  legend: {
    orientation: "h",
    bgcolor: "#ffffff",
    bordercolor: "#e8e8e3",
    borderwidth: 1
  }
};
var config = {
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
var intervalId = window.setInterval(function () {

  // check if timestamp is updated
  $.get(urlTimestamp, function () { })

    .done(function (data) {
      if (timestamp != data) {
        timestamp = data;

        // get new data
        $.getJSON(urlTiming, function () { })
          .done(function (data) {

            // rebuild whenever the set of reported stages changes
            var incoming = Object.keys(data)
              .filter(item => item !== "timestamp" && item !== "uptime_s" && item !== "uptime_days")
              .sort()
              .join(',');

            if (incoming != plottedKeys) {
              plottedKeys = incoming;

              // timestamp posix to js
              for (i = 0; i < data["timestamp"].length; i++)
              {
                data["timestamp"][i] = new Date(data["timestamp"][i]);
              }

              data_trace = [];
              keys = Object.keys(data);
              keys = keys.filter(item => item !== "timestamp" && item !== "uptime_s" && item !== "uptime_days");
              for (i = 0; i < keys.length; i++) {
                var isDuty = keys[i] === "duty_cycle";
                var trace = {
                  x: data["timestamp"],
                  y: data[keys[i]],
                  mode: 'lines+markers',
                  type: 'scatter',
                  name: isDuty ? 'duty cycle' : keys[i],
                  yaxis: isDuty ? 'y2' : 'y',
                  line: {
                    width: 5,
                    dash: isDuty ? 'dot' : 'solid'
                  },
                  marker: {
                    size: 12
                  },
                };
                data_trace.push(trace);
              }

              Plotly.newPlot('data', data_trace, layout, config);
            }
            // case update plot
            else {
              // timestamp posix to js
              for (i = 0; i < data["timestamp"].length; i++)
              {
                data["timestamp"][i] = new Date(data["timestamp"][i]);
              }
              var xVec = [];
              var yVec = [];
              for (i = 0; i < keys.length; i++) {
                xVec.push(data["timestamp"]);
                yVec.push(data[keys[i]]);
              }
              var trace_update = {
                x: xVec,
                y: yVec
              };
              Plotly.update('data', trace_update);
            }

          })
          .fail(function () {
          })
          .always(function () {
          });
      }
    })
    .fail(function () {
    })
    .always(function () {
    });
}, 100);
