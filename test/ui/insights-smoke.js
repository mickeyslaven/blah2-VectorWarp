'use strict';

const fs = require('fs');
const vm = require('vm');

const view = process.argv[2];
const mode = process.argv[3] || 'portable';
if (!['portable', 'rich'].includes(mode)) throw new Error(`Unknown fixture mode: ${mode}`);
const validViews = ['overview', 'detections', 'health', 'activity', 'site', 'locations', 'evaluation', 'tracks'];
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
  '/api/timing': {timestamp: Date.now(), nCpi: 10, uptime_s: 5, cpi_data_ms: 500, cpi_hop_ms: 500, capture_backlog_ms: 0, ambiguity_processing: 20, detector: 1, cpi: 30}
};
const rich = {
  '/api/config': {capture: {fs: 2400000, fc: 527000000, device: {type: 'Kraken', gain: [10, 10, 10, 10, 10, 10]}}, location: {rx: {name: 'Test receiver', latitude: 40.0, longitude: -80.0, altitude: 250}, tx: {name: 'Test illuminator', latitude: 40.1, longitude: -80.2, altitude: 500}}, process: {data: {cpi: .2, overlap: .1}, ambiguity: {delayMin: -10, delayMax: 400, dopplerMin: -800, dopplerMax: 800}, detection: {enable: true, pfa: .0001}, tracker: {enable: true, initiate: {M: 3, N: 5}}, spectrum: {enable: true}}, truth: {adsb: {enabled: true, display_range_km: 30}, ais: {enabled: false}}},
  '/api/detection': {timestamp: Date.now(), delay: [1.25], doppler: [42], snr: [18]},
  '/api/tracker': {timestamp: Date.now(), n: 1, nTentative: 0, nAssociated: 1, nActive: 0, nCoasting: 0, data: [{id: 'T1', state: 'ASSOCIATED', delay: 10, doppler: 42, n: 3, associated_delay: [8, 9, 10], associated_doppler: [38, 40, 42], associated_state: ['TENTATIVE', 'ASSOCIATED', 'ASSOCIATED']}]},
  '/api/timing': {timestamp: Date.now(), nCpi: 10, uptime_s: 50, cpi_data_ms: 200, cpi_hop_ms: 100, capture_backlog_ms: 4, reference_synthesis: 4, ambiguity_processing: 20, detector: 1, tracker: .1, cpi: 35},
  '/api/adsb': {now: Date.now() / 1000, aircraft: [{hex: 'abc123', flight: 'TEST1', lat: 40.05, lon: -80.1, alt_geom: 5000, seen_pos: 1}]},
  '/api/dd': {abc123: {flight: 'TEST1', delay: 1.3, doppler: 40, timestamp: Date.now() / 1000}}
};

global.fetchStatusResource = async (url, options = {}) => {
  url = new URL(url, window.location.origin).href;
  const fixtures = mode === 'rich' ? rich : portable;
  const path = new URL(url).pathname.replace('/api/runtime/config', '/api/config').replace('/api/adsb/delay-doppler', '/api/dd');
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

vm.runInThisContext(fs.readFileSync('html/js/insights.js', 'utf8'), {filename: 'insights.js'});

setTimeout(() => {
  const plotViews = new Set(['activity', 'site', 'locations', 'evaluation', 'tracks']);
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
  if (view === 'locations' && JSON.stringify(lastPlot?.traces || []).includes('Radar return'))
    errors.push('unconfirmed radar returns were plotted as ellipses');
  if (/\b(?:undefined|NaN)\b/.test(rendered)) errors.push('view rendered an invalid value');
  if (rendered.length < 100) errors.push('view rendered too little content');
  if (errors.length) {
    console.error(JSON.stringify({view, mode, state: state.textContent, plotCalls, renderError: window.blah2ViewLastError, errors}));
    process.exit(1);
  }
  console.log(JSON.stringify({view, mode, state: state.textContent, plotCalls, htmlBytes: rendered.length}));
}, 2500);
