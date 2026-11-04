var nRows = null;
var range_x = [];
var range_y = [];
var SPECTRUM_MAX_DYNAMIC_RANGE_DB = 80;
var SPECTRUM_MIN_COLOR_SPAN_DB = 10;

function spectrumColorRange(spectrum) {
  var lowest = Infinity;
  var highest = -Infinity;
  for (var row of spectrum) {
    if (!Array.isArray(row)) continue;
    for (var value of row) {
      if (!Number.isFinite(value)) continue;
      lowest = Math.min(lowest, value);
      highest = Math.max(highest, value);
    }
  }
  if (!Number.isFinite(highest)) return {zmin: -120, zmax: 0};
  lowest = Math.max(lowest, highest - SPECTRUM_MAX_DYNAMIC_RANGE_DB);
  if (highest - lowest < SPECTRUM_MIN_COLOR_SPAN_DB) {
    var middle = (highest + lowest) / 2;
    return {zmin: middle - SPECTRUM_MIN_COLOR_SPAN_DB / 2,
      zmax: middle + SPECTRUM_MIN_COLOR_SPAN_DB / 2};
  }
  return {zmin: lowest, zmax: highest};
}

// setup API
var urlMap = liveApiUrl(radarPresentationUrl('/stash/iqdata'));

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
  // Preserve the operator's viewport across spectrum-history refreshes.
  uirevision: 'vectorwarp-spectrum',
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
    colorbar: {title: {text: 'dB'}},
    zauto: false,
    zmin: -120,
    zmax: 0,
    type: 'heatmap'
  }
];
var detection = [];

Plotly.react('data', data, layout, config);

// callback function
var radarUpdates = null;
async function renderSpectrum(data) {

  // case draw new plot
  if (data.frequency?.[0]?.length !== nRows) {
    // timestamp posix to js
    for (let i = 0; i < data.timestamp.length; i++)
    {
      data.timestamp[i] = new Date(data.timestamp[i]);
    }
    var colorRange = spectrumColorRange(data.spectrum);
    var trace1 = {
        x: data.frequency[0].map(khz => khz / 1000),
        y: data.timestamp,
        z: data.spectrum,
        colorscale: 'Jet',
        colorbar: {title: {text: 'dB'}},
        zauto: false,
        zmin: colorRange.zmin,
        zmax: colorRange.zmax,
        type: 'heatmap'
    };

    var data_trace = [trace1];
    await Plotly.react('data', data_trace, layout, config);
    nRows = data.frequency?.[0]?.length;
  }
  // case update plot
  else {
    // timestamp posix to js
    for (let i = 0; i < data.timestamp.length; i++)
    {
      data.timestamp[i] = new Date(data.timestamp[i]);
    }
    var colorRange = spectrumColorRange(data.spectrum);
    var trace_update = {
      x: [data.frequency[0].map(khz => khz / 1000)],
      y: [data.timestamp],
      z: [data.spectrum],
      zauto: [false],
      zmin: [colorRange.zmin],
      zmax: [colorRange.zmax]
    };
    await Plotly.update('data', trace_update);
  }
}

function showSpectrumUnavailable() {
  const message = 'Global wideband spectrum is unavailable: this multi-illuminator processor is not publishing wideband IQ spectrum frames.';
  Plotly.react('data', [], {...layout, annotations: [{text: message, showarrow: false,
    font: {color: '#f7c75f', size: 16}}]}, config);
  const target = typeof document === 'undefined' ? null : document.getElementById('data');
  target?.setAttribute?.('aria-label', message);
  target?.setAttribute?.('aria-busy', 'false');
}

var vectorwarpSpectrumReady = getRadarRuntimeConfig().then(async runtime => {
  if (runtime?.process?.multi_illuminator?.enabled === true) {
    try {
      const first = await fetchRadarJson(urlMap);
      const latestTimestamp = Array.isArray(first?.timestamp) ?
        Number(first.timestamp.at(-1)) : Number(first?.timestamp);
      const timestampMs = latestTimestamp < 1e11 ? latestTimestamp * 1000 : latestTimestamp;
      const freshnessMs = Math.max(10000, Number(runtime?.process?.data?.cpi || 1) * 3000);
      if (!first?.frequency?.[0]?.length || !Array.isArray(first?.spectrum) ||
          !Number.isFinite(timestampMs) || Math.abs(Date.now() - timestampMs) > freshnessMs) {
        showSpectrumUnavailable();
        return null;
      }
      await renderSpectrum(first);
    } catch (_) {
      showSpectrumUnavailable();
      return null;
    }
  }
  radarUpdates = startRadarPlot(urlMap, renderSpectrum);
  return radarUpdates;
});
