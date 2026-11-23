var plottedKeys = '';

function timingTraceKeys(data) {
  const singleStreamDurations = new Set(['extract_buffer', 'spectrum', 'clutter_filter',
    'ambiguity_processing', 'detector', 'tracker', 'output_radar_data']);
  return Object.keys(data).filter(key => {
    if (!Array.isArray(data[key])) return false;
    if (!(key === 'cpi' || key.endsWith('_ms') || singleStreamDurations.has(key) ||
        /^multi_(?:channelizer_wall|spectrum_output|illuminator_radar|serialize|output|pipeline_advance)$/.test(key))) return false;
    return data[key].some(value => Number.isFinite(value));
  });
}

function timingTraceLabel(key) {
  return key.replace(/^multi_/, '').replace(/_ms$/, '').replaceAll('_', ' ')
    .replace(/\b\w/g, value => value.toUpperCase()) + ' (ms)';
}

async function updatePresentationTiming() {
  const target = typeof document === 'undefined' ? null : document.getElementById('presentation-timing');
  if (!target) return;
  try {
    const status = await readRadarDisplay('network', '/api/presentation/status', {awaitFresh: true});
    const cadence = status.cadence || {};
    const percent = Number.isFinite(cadence.within_100ms_fraction) ?
      `${Math.round(cadence.within_100ms_fraction * 100)}%` : 'Measuring';
    const source = status.event_time?.exact ? 'Exact receiver event time.' :
      status.event_time?.sample_contiguous === true ?
        'Receiver event time is sample-contiguous.' :
        status.event_time?.present === true ?
          `Host-estimated receiver time${status.event_time.reason ? `: ${status.event_time.reason}` : '.'}` :
          'Receiver event-time metadata unavailable.';
    target.innerHTML = `<div><strong>${status.presentation_delay_ms ?? '—'} ms</strong><span>Fixed presentation hold</span></div>` +
      `<div><strong>${status.expected_frame_interval_ms ?? '—'} ms</strong><span>Configured CPI cadence</span></div>` +
      `<div><strong>${percent}</strong><span>Frames within ±100 ms</span></div>` +
      `<div><strong>${status.relative_alignment_ms ?? '—'} ms</strong><span>Layer alignment</span></div>` +
      `<p>${source}</p>`;
    const state = document.getElementById('view-state');
    if (state) {
      state.textContent = `${status.expected_frame_interval_ms ?? '—'} ms cadence`;
      state.className = 'view-state good';
    }
  } catch (_) {
    target.innerHTML = '<div><strong>Unavailable</strong><span>Presentation timing</span></div>';
    const state = document.getElementById('view-state');
    if (state) {
      state.textContent = 'Timing unavailable';
      state.className = 'view-state bad';
    }
  }
}

// setup plotly
var layout = VectorWarpPlotTheme.cartesianLayout('vectorwarp-timing', xTitle, yTitle, {legend: true});
var config = VectorWarpPlotTheme.config(true);

// Keep the waiting canvas empty until measured timing arrives.
var data = [];

Plotly.react('data', data, layout, config);

// callback function
var vectorwarpTimingReady = startRadarDisplay('timing', async function (data) {
  void updatePresentationTiming();
  const keys = timingTraceKeys(data);
  const signature = JSON.stringify(keys);
  data.timestamp = data.timestamp.map(value => new Date(value));

  // case draw new plot
  if (signature !== plottedKeys) {

    var data_trace = [];
    for (let i = 0; i < keys.length; i++) {
      var trace = {
        x: data["timestamp"],
        y: data[keys[i]],
        mode: 'lines+markers',
        type: 'scatter',
        name: timingTraceLabel(keys[i]),
        line: {
          width: 5
        },
        marker: {
          size: 12
        },
      };
      data_trace.push(trace);
    }

    await Plotly.react('data', data_trace, layout, config);
    plottedKeys = signature;
  }
  // case update plot
  else {
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
