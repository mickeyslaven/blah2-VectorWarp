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

// setup plotly
var layout = {
  ...VectorWarpPlotTheme.layout('vectorwarp-spectrum'),
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
var config = VectorWarpPlotTheme.config();

// Keep the waiting canvas empty until measured spectrum arrives.
var data = [];
var detection = [];

Plotly.react('data', data, layout, config);

// callback function
async function renderSpectrum(data) {
  data.timestamp = data.timestamp.map(value => new Date(value));
  const frequency = data.frequency[0].map(khz => khz / 1000);
  const colorRange = spectrumColorRange(data.spectrum);

  // case draw new plot
  if (data.frequency?.[0]?.length !== nRows) {
    var trace1 = {
        x: frequency,
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
    var trace_update = {
      x: [frequency],
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

var vectorwarpSpectrumReady = startRadarDisplay('spectrum', renderSpectrum,
  {unavailable: showSpectrumUnavailable});
