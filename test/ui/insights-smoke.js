'use strict';

const fs = require('fs');
const vm = require('vm');

const view = process.argv[2];
const mode = process.argv[3] || 'portable';
if (!['portable', 'rich'].includes(mode)) throw new Error(`Unknown fixture mode: ${mode}`);
const validViews = ['overview', 'detections', 'health', 'activity', 'site', 'locations', 'evaluation', 'tracks', 'statistics', 'calibration'];
if (!validViews.includes(view)) throw new Error(`Unknown view: ${view}`);

class Element {
  constructor(id = '') {
    this.id = id;
    this.dataset = {};
    this.className = '';
    this.textContent = '';
    this.children = [];
    this._innerHTML = '';
  }
  set innerHTML(value) {
    this._innerHTML = String(value);
    this.children = this._innerHTML ? [{}] : [];
    for (const match of this._innerHTML.matchAll(/id="([^"]+)"/g)) {
      if (!elements.has(match[1])) elements.set(match[1], new Element(match[1]));
    }
  }
  get innerHTML() { return this._innerHTML; }
}

const elements = new Map();
const root = new Element('view-root');
const state = new Element('view-state');
elements.set(root.id, root);
elements.set(state.id, state);

global.document = {
  body: {dataset: {insightView: view}},
  getElementById: id => elements.get(id) || null
};
global.window = {
  location: new URL('http://radar.example.invalid:8080/'),
  setInterval: () => 0
};
global.sessionStorage = {getItem: () => null, setItem: () => {}};
global.addFullscreenControl = () => {};
global.addRecordingControl = () => {};
global.liveApiUrl = path => path;

let plotCalls = 0;
let lastPlot = null;
global.Plotly = {react: (_target, traces, layout) => {
  plotCalls += 1;
  lastPlot = {traces, layout};
}};

const portable = {
  '/api/config': {capture: {fs: 2000000, fc: 100000000, device: {type: 'Usrp', reference_channel: 0}}, location: {rx: {name: 'Receiver', latitude: 51.5, longitude: -0.1, altitude: 20}, tx: {name: 'Illuminator', latitude: 51.6, longitude: -0.2, altitude: 100}}, process: {data: {cpi: .5, overlap: 0}, ambiguity: {delayMin: 0, delayMax: 100, dopplerMin: -200, dopplerMax: 200}, detection: {enable: true, pfa: .001}, tracker: {enable: false, initiate: {M: 3, N: 5}}, spectrum: {enable: false}}, truth: {adsb: {enabled: false}, ais: {enabled: false}}},
  '/api/detection': {timestamp: Date.now(), delay: [], doppler: [], snr: []},
  '/api/tracker': null,
  '/api/timing': {timestamp: Date.now(), nCpi: 10, uptime_s: 5, cpi_data_ms: 500, cpi_hop_ms: 500, capture_backlog_ms: 0, ambiguity_processing: 20, detector: 1, cpi: 30},
  '/api/statistics': {schema: 'vectorwarp-evidence-statistics-v1', collecting: false,
    limits: {detectionMinDelayKm: .75, delayMaxKm: 75, dopplerMinHz: -200, dopplerMaxHz: 200,
      clutterMaxKm: 75, matchDelayKm: .25, matchDopplerHz: 5},
    windows: Object.fromEntries(['hour','day','week','lifetime'].map(name => [name,
      {passes: 0, eligible_passes: 0, detected_passes: 0, tracked_passes: 0,
        missed_passes: 0, out_of_config_passes: 0, eligible_no_radar_return: 0,
        detected_not_tracked: 0, detection_yield: null, tracking_yield: null,
        out_of_span: {below_detection_floor: 0, above_delay_ceiling: 0,
          outside_doppler_span: 0, stale_truth: 0}}])),
    bearing: {enabled: false, calibrated: false, live: {tracks: 0, publishable: 0,
      resolved: 0, stationary: 0}, diagnostic: {available: false}, recorder: {enabled: false},
      automation: {state: 'disabled'}, frequency_resolved: {enabled: false}}},
  '/api/array-calibration/status': {frequency_hz: 100000000,
    corpus: {enabled: false}, reference: {samples: 0},
    bearing: {active_model: null, automation: {enabled: false, state: 'disabled', qualified: false},
    progress: {samples: 0, independent_passes: 0, unique_aircraft: 0, covered_sectors: 0,
      required_covered_sectors: 6, evidence_completion_fraction: 0, requirements: [], sectors: [],
      next_actions: ['Collect evidence.']}}, detection: {automation: {enabled: false,
      state: 'disabled', qualified: false, promotion_allowed: false}, progress: {accepted_samples: 0,
      independent_passes: 0, validation_passes: 0, miss_passes: 0, covered_sectors: 0,
      fit_bins: 0, positive_control: {fit_ready: false}, validation: {ready: false},
      production_replay: {imported_passes: 0}}}}
};
const rich = {
  '/api/config': {capture: {fs: 2400000, fc: 527000000, device: {type: 'Kraken', gain: [10, 10, 10, 10, 10, 10]}}, location: {rx: {name: 'Test receiver', latitude: 40.0, longitude: -80.0, altitude: 250}, tx: {name: 'Test illuminator', latitude: 40.1, longitude: -80.2, altitude: 500}}, process: {data: {cpi: .2, overlap: .1}, ambiguity: {delayMin: -10, delayMax: 400, dopplerMin: -800, dopplerMax: 800}, detection: {enable: true, pfa: .0001}, tracker: {enable: true, initiate: {M: 3, N: 5}}, bearing: {enable: true}, spectrum: {enable: true}}, truth: {adsb: {enabled: true, display_range_km: 30}, ais: {enabled: false}}},
  '/api/detection': {timestamp: Date.now(), delay: [1.25], doppler: [42], snr: [18]},
  '/api/tracker': {timestamp: Date.now(), n: 15, nTentative: 1, nAssociated: 13, nActive: 0, nCoasting: 1, data: [...Array.from({length: 13}, (_, index) => ({id: `T${index + 1}`, state: 'ASSOCIATED', delay: 10 + index, doppler: 42, n: 3, associated_delay: [8, 9, 10], associated_doppler: [38, 40, 42], associated_state: ['TENTATIVE', 'ASSOCIATED', 'ASSOCIATED']})), {id: 'tentative', state: 'TENTATIVE', delay: 8, doppler: 5, n: 1}, {id: 'coasting', state: 'COASTING', delay: 9, doppler: 6, n: 5}]},
  '/api/timing': {timestamp: Date.now(), nCpi: 10, uptime_s: 50, cpi_data_ms: 200, cpi_hop_ms: 100, capture_backlog_ms: 4, reference_synthesis: 4, ambiguity_processing: 20, detector: 1, tracker: .1, cpi: 35},
  '/api/statistics': {schema: 'vectorwarp-evidence-statistics-v1', collecting: true,
    updated_at: Date.now(), limits: {detectionMinDelayKm: .624, delayMaxKm: 62.456,
      dopplerMinHz: -600, dopplerMaxHz: 600, clutterMaxKm: 24.98,
      matchDelayKm: .25, matchDopplerHz: 5},
    windows: Object.fromEntries(['hour','day','week','lifetime'].map((name, index) => [name,
      {passes: 10 + index, eligible_passes: 8 + index, detected_passes: 5 + index,
        tracked_passes: 4 + index, missed_passes: 4, out_of_config_passes: 2,
        eligible_no_radar_return: 3, detected_not_tracked: 1, detection_yield: .625,
        tracking_yield: .5, out_of_span: {below_detection_floor: 2,
          above_delay_ceiling: 3, outside_doppler_span: 4, stale_truth: 1}}])),
    bearing: {enabled: true, calibrated: false, live: {tracks: 3, publishable: 1,
      resolved: 1, stationary: 1}, diagnostic: {available: true, samples: 120,
      aircraft: 8, sectors: 6, evidence_ready: true,
      active: {median_error_deg: 31, within_45_fraction: .7},
      best: {median_error_deg: 22, within_45_fraction: .8},
      global_heading_bias: {signed_correction_deg: -12, median_error_deg: 25}},
      recorder: {enabled: true, samples: 120, unmatched: 10, staleTruth: 2,
        elevationRejected: 3, ambiguous: 1, referenceSamples: 90,
        maxReferencePhaseStepDeg: .2, maxReferenceAmplitudeStepDb: .01},
      automation: {state: 'collecting', qualified: false},
      frequency_resolved: {enabled: true, state: 'idle'}}},
  '/api/array-calibration/status': {frequency_hz: 527000000,
    corpus: {enabled: true, automatic: false, accepted_samples_this_process: 120},
    reference: {samples: 90, maximum_phase_step_deg: .2, maximum_amplitude_step_db: .01},
    bearing: {active_model: null, automation: {enabled: true, state: 'collecting', qualified: false},
    progress: {samples: 120, independent_passes: 60, unique_aircraft: 20, covered_sectors: 6,
      required_covered_sectors: 6, evidence_completion_fraction: .8,
      requirements: [{label: 'Independent passes', required: 40, current: 60, complete: true}],
      sectors: [{from_deg: 0, to_deg: 45, samples: 20, passes: 10, aircraft: 4, ready: true,
        passes_needed: 0, aircraft_needed: 0}], next_actions: []}},
    detection: {automation: {enabled: false, state: 'disabled', qualified: false,
      promotion_allowed: false, runtime_candidate_path: '/tmp/replay-only.runtime.json'}, progress: {accepted_samples: 48, independent_passes: 24,
      validation_passes: 5, miss_passes: 3, covered_sectors: 4, fit_bins: 7,
      positive_control: {fit_ready: true}, validation: {ready: false},
      production_replay: {imported_passes: 0},
      limitation: 'Truth-selected statistics are diagnostic only; native full-bank replay is required.'}}},
  '/api/adsb': {now: Date.now() / 1000, aircraft: [{hex: 'abc123', flight: 'TEST1', lat: 40.05, lon: -80.1, alt_geom: 5000, seen_pos: 1}]},
  '/api/locations': {enabled: true, available: true, data: [
    {id: 'T1', state: 'ASSOCIATED', bearing_deg_true: 112, uncertainty_deg: 8,
      peak_separation_db: 5, temporal_consistency: .9, excess_path_km: 1.2,
      resolved: true, location: {latitude: 40.03, longitude: -80.04}, estimate: {latitude: 40.03, longitude: -80.04},
      uncertainty_corridor: [{latitude: 40.02, longitude: -80.03}, {latitude: 40.03, longitude: -80.04}, {latitude: 40.04, longitude: -80.05}], candidates: []},
    {id: 'T2', state: 'COASTING', bearing_deg_true: 177, uncertainty_deg: 46,
      peak_separation_db: 1.2, temporal_consistency: .35, excess_path_km: 2.1,
      resolved: false, location: null, estimate: {latitude: 39.98, longitude: -80.01}, uncertainty_corridor: [],
      candidates: [{latitude: 40.01, longitude: -80.06, bearing_deg_true: 130, support: .51}]}
  ]},
  '/api/dd': {abc123: {flight: 'TEST1', delay: 1.3, doppler: 40, timestamp: Date.now() / 1000}}
};

global.fetchStatusResource = async (url, options = {}) => {
  url = new URL(url, window.location.origin).href;
  const fixtures = mode === 'rich' ? rich : portable;
  const path = new URL(url).pathname.replace('/api/runtime/config', '/api/config')
    .replace('/api/adsb/delay-doppler', '/api/dd')
    .replace('/api/live-scorecard/provisional', '/api/statistics')
    .replace('/api/live-scorecard', '/api/statistics');
  if (path === '/api/system/status') return {ok: true, text: async () => JSON.stringify({serverId: 'fixture', radar: 'receiving'})};
  if (path === '/api/adsb/status') return {ok: true, text: async () => JSON.stringify({enabled: mode === 'rich', online: mode === 'rich'})};
  if (!(path in fixtures)) return {ok: false, status: 404, text: async () => ''};
  return {ok: true, status: 200, text: async () => JSON.stringify(fixtures[path])};
};

// Exercise the real shared cadence/config helper, but suppress recurring timers
// in this one-render fixture check. Cadence itself has a fake-clock test.
global.window.setTimeout = () => 0;
global.window.clearTimeout = () => {};
global.fetch = global.fetchStatusResource;
const common = require('../../html/js/common.js');
global.getRadarRuntimeConfig = common.getRadarRuntimeConfig;
global.startRadarUpdates = common.startRadarUpdates;

vm.runInThisContext(fs.readFileSync(require.resolve('../../html/js/insights.js'), 'utf8'), {filename: 'insights.js'});

setTimeout(() => {
  const plotViews = new Set(['activity', 'site', 'locations', 'evaluation', 'tracks', 'calibration']);
  const errors = [];
  const rendered = [...elements.values()].map(element => element.innerHTML).join('');
  if (!root.children.length) errors.push('view root remained empty');
  if (!state.textContent) errors.push('view state was not updated');
  if (state.textContent === 'Data unavailable') errors.push('renderer reported unavailable data');
  if (plotViews.has(view) && plotCalls < 1) errors.push('plot renderer was not called');
  if (view === 'locations' && (!lastPlot?.layout?.mapbox || !lastPlot.layout.uirevision))
    errors.push('location map viewport is not persistent');
  if (view === 'locations' && mode === 'rich' &&
      !lastPlot?.traces?.some(trace => trace.name === 'ADS-B' &&
        Number(trace.marker?.size) >= 24))
    errors.push('ADS-B plane symbols were not rendered');
  if (view === 'locations' && mode === 'rich' &&
      !lastPlot?.traces?.some(trace => trace.name === 'Unresolved estimate T2'))
    errors.push('unresolved coasting bearing estimate was not rendered');
  if (view === 'locations' && mode === 'rich' &&
      (!rendered.includes('Bearing estimates') || !rendered.includes('Unresolved shown')))
    errors.push('bearing diagnostic counts were not rendered');
  if (view === 'locations' && mode === 'rich' &&
      lastPlot?.traces?.filter(trace => String(trace.name).startsWith('Radar track ')).length !== 13)
    errors.push('current radar tracks were silently capped or hidden');
  if (view === 'locations' && JSON.stringify(lastPlot?.traces || []).includes('Radar return'))
    errors.push('unconfirmed radar returns were plotted as ellipses');
  if (view === 'calibration' && (!rendered.includes('Detection manifold shadow') ||
      !rendered.includes('Hard-disabled — never automatic') ||
      !rendered.includes('Support / uncertainty') ||
      !rendered.includes('Active detection model') ||
      !rendered.includes('2-D candidate') ||
      !rendered.includes('Native 2-D shadow fit') ||
      !rendered.includes('Pass-disjoint complex projection only') ||
      !rendered.includes('physical paired A/B qualification still required')))
    errors.push('detection manifold progress and promotion policy were not rendered');
  if (view === 'locations' && lastPlot?.layout?.mapbox?.style === 'open-street-map' &&
      lastPlot.traces.some(trace => trace.mode?.includes('text')))
    errors.push('raster OSM style cannot render text symbols without a glyph source');
  if (/\b(?:undefined|NaN)\b/.test(rendered)) errors.push('view rendered an invalid value');
  if (rendered.length < 100) errors.push('view rendered too little content');
  if (errors.length) {
    console.error(JSON.stringify({view, mode, state: state.textContent, plotCalls, renderError: window.blah2ViewLastError, errors}));
    process.exit(1);
  }
  console.log(JSON.stringify({view, mode, state: state.textContent, plotCalls, htmlBytes: rendered.length}));
}, 2500);
