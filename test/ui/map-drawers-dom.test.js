'use strict';

const assert = require('node:assert/strict');
const fs = require('fs');
const vm = require('vm');
const {JSDOM} = require('./jsdom');

const source = fs.readFileSync(require.resolve('../../html/js/insights.js'), 'utf8');
assert.match(source, /locationToScreenPoint\(LngLat\.convert\(\[longitude, latitude\]\), null\)/,
  'radar ellipses project on a flat reference plane instead of terrain');
assert.doesNotMatch(source, /location-radar-track-slice|location-radar-track-ribs|sliceAltitudes/,
  'radar tracks use one clean outline instead of a clipping 3D cage');
assert.equal((source.match(/type: 'raster-dem'/g) || []).length, 2,
  'terrain and hillshade use separate DEM state as required by MapLibre');
assert.match(source, /zoom_tiers[\s\S]*stable parent while a denser nested tier is loading/,
  'nested imagery coverage retains matching parent tiles during zoom and rotation');
assert.match(source, /vectorwarp-basemap', 'visibility', aerial \? 'none' : 'visible'/,
  'aerial mode never exposes mismatched street tiles beyond the local pack');
assert.match(source, /maxTileCacheZoomLevels: 8[\s\S]*maxTileCacheSize: 192/,
  'map retains a bounded neighboring tile cache while rotating');
assert.match(source, /if \(onScreen\) modelInstances\.push/,
  '3D renderer culls offscreen aircraft during map interaction');
assert.match(source, /NavigationControl[\s\S]*showCompass: true, showZoom: false, visualizePitch: true/,
  'map exposes a lightweight bearing and pitch compass');

const dom = new JSDOM(`<!doctype html><body data-insight-view="locations">
  <span id="view-state"></span><div id="view-root"></div></body>`,
{url: 'http://radar.example.invalid/display/locations'});
let refresh;
const context = vm.createContext({
  window: dom.window, document: dom.window.document, URL, URLSearchParams, console,
  Date, localStorage: dom.window.localStorage, sessionStorage: dom.window.sessionStorage,
  addFullscreenControl: () => {}, addRecordingControl: () => {},
  liveApiUrl: path => path, radarPresentationUrl: path => path,
  getRadarRuntimeConfig: async () => ({location: {rx: {}, tx: {}}}),
  startRadarUpdates: callback => { refresh = callback; return {stop() {}}; },
  fetchStatusResource: async url => ({ok: true, status: 200, text: async () =>
    JSON.stringify(url.includes('/tracker') ? {data: []} : {})})
});
dom.window.matchMedia = () => ({matches: true, addEventListener() {}, removeEventListener() {}});
vm.runInContext(source, context,
  {filename: 'insights.js'});

(async () => {
  await refresh();
  const controls = dom.window.document.querySelector('.location-map-controls');
  const aircraft = dom.window.document.querySelector('.location-aircraft-sidebar');
  assert.ok(controls && aircraft, 'map controls and aircraft details use separate drawers');
  assert.ok(controls.querySelector('.map-sidebar-icon svg'), 'map options use a settings icon');
  assert.ok(aircraft.querySelector('.map-sidebar-icon svg'), 'aircraft details use a plane icon');
  assert.equal(controls.open, false, 'mobile starts with map controls collapsed');
  assert.equal(aircraft.open, false, 'mobile starts with aircraft details collapsed');
  assert.equal(dom.window.document.querySelector('.location-map-notes'), null,
    'map does not render a redundant explanatory details box');
  controls.open = true;
  controls.dispatchEvent(new dom.window.Event('toggle'));
  aircraft.open = true;
  aircraft.dispatchEvent(new dom.window.Event('toggle'));
  assert.equal(controls.open, false, 'opening aircraft details closes map controls');

  const filter = dom.window.document.getElementById('adsb-map-filter-mode');
  assert.deepEqual([...filter.options].map(option => option.value),
    ['all', 'range', 'radar', 'tracked']);
  const range = dom.window.document.getElementById('adsb-map-range-km');
  filter.value = 'range';
  filter.dispatchEvent(new dom.window.Event('change'));
  assert.equal(range.closest('label').hidden, false, 'range input appears for distance filtering');
  range.value = '75';
  range.dispatchEvent(new dom.window.Event('change'));
  assert.equal(dom.window.localStorage.getItem('vectorwarp-adsb-map-range-km'), '75',
    'selected map range is retained');
  console.log('Map drawers are separate, mobile-safe, and expose all ADS-B filters.');
})().catch(error => { console.error(error); process.exitCode = 1; });
