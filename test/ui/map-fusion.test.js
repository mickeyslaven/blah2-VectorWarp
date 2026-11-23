'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const model = require('../../html/js/fusion_aircraft.js');
const receiver={latitude:40,longitude:-80};
const plain={hex:'abc123',flight:'PLAIN',lat:40.1,lon:-80.1,alt_baro:21000,radar_track_id:9};
const matched={hex:'def456',flight:'MATCHED',lat:40.2,lon:-80.2,alt_baro:33000,
  gs:500,track:180,geom_rate:3000,delay:99,doppler:99,t:'A320'};
const track={id:'F1',state:'FUSED_ACTIVE',latitude:40.3,longitude:-80.3,altitude:2000,
  velocity_east_km_s:.1,velocity_north_km_s:0,contributors:[{illuminator:'a'},{illuminator:'b'}]};
const envelope={id:'E1',state:'ENVELOPE_SUPPORTED',operational_position:false,
  representative_is_fix:false,latitude:40.1,longitude:-80.1,
  position_envelope:{horizontal_radius_km:4}};
const result={available:true,tracks:[track],position_candidates:[envelope],
  validation:{held_out_truth:true,adsb_used_by_solver:false,target_kind:'operational-fused-tracks',
    matches:[{candidate_id:'F1',adsb_hex:'def456'}]}};
const merged=model.mapAircraft([plain,matched],result,receiver);
assert.equal(merged.length,2,'explicitly associated aircraft has exactly one geographic record');
assert.equal(merged[0],plain,'unmatched ADS-B object is preserved without rewriting its icon inputs');
const radar=merged[1];
assert.equal(radar.lat,40.3);assert.equal(radar.lon,-80.3);
assert.equal(radar.alt_geom,2000/.3048);assert.equal(radar.track,90);
assert.equal(radar.t,'A320','identity metadata can retain the canonical aircraft class');
assert.equal(radar.flight,'MATCHED','identity merge retains the existing callsign');
assert.equal(radar.alt_baro,undefined);assert.equal(radar.geom_rate,null);
assert.equal(radar.delay,undefined);assert.equal(radar.doppler,undefined,'ADS-B geometry never becomes radar geometry');
assert.equal(radar._radar_position.state,'tracked');assert.deepEqual(radar._radar_position.channels,['a','b']);
assert.deepEqual(model.mapAircraft([plain,matched],{...result,available:false},receiver),[plain,matched]);
assert.equal(model.envelopeGeoJson(result,receiver).features.length,0,'diagnostic regions default off');
const region=model.envelopeGeoJson(result,receiver,true).features[0];
assert.equal(region.geometry.type,'Polygon');assert.equal(region.geometry.coordinates[0].length,65);
assert.deepEqual(region.geometry.coordinates[0][0],region.geometry.coordinates[0].at(-1));
assert.equal(region.properties.operational_position,false);
const narrow=model.envelopeGeoJson({...result,position_candidates:[{...envelope,
  position_envelope:{horizontal_radius_km:1}}]},receiver,true).features[0];
const width=feature=>Math.max(...feature.geometry.coordinates[0].map(p=>p[0]))-Math.min(...feature.geometry.coordinates[0].map(p=>p[0]));
assert.ok(width(narrow)<width(region)/3,'additional constraints shrink the displayed bound');
assert.equal(model.envelopeGeoJson({...result,available:false},receiver,true).features.length,0);
assert.equal(model.envelopeGeoJson({...result,position_candidates:[{...envelope,state:'ENVELOPE_STALE'}]},receiver,true).features.length,0);
const qualified={...envelope,operational_position:true,representative_is_fix:true,resolved:true,
  qualification:{qualified:true}};
assert.equal(model.mapAircraft([],{available:true,tracks:[],position_candidates:[qualified]},receiver).length,1,
  'only an explicit operational fix can become an aircraft glyph');
assert.equal(model.envelopeGeoJson({available:true,position_candidates:[qualified]},receiver,true).features.length,0);
const source=fs.readFileSync(require.resolve('../../html/js/insights.js'),'utf8');
const mapLayers=fs.readFileSync(require.resolve('../../html/js/locations-map-layers.js'),'utf8');
assert.doesNotMatch(source,/vectorwarp-fused-track-hypotheses|vectorwarp-fusion-ambiguous-hypotheses/,'no duplicate fused point layers');
assert.match(source,/syncBearingOverlayLayers\(map, bearingRows\)/);
assert.match(mapLayers,/function syncBearingOverlayLayers\(map, rows\)/);
assert.match(source,/selectedIsRadar \? radarAircraftTrackHistories : aircraftTrackHistories/,'position-source trails stay separate');
assert.match(source,/item\._radar_position \? radarVisualSemantics.colors.independent : aircraftAltitudeColor\(item\)/);
const page=fs.readFileSync(require.resolve('../../html/display/locations/index.html'),'utf8');
assert.match(page,/fusion_aircraft\.js\?v=[\w.-]+.*insights\.js\?v=[\w.-]+/);
const mapLayerWindow = {};
vm.runInNewContext(mapLayers, {window: mapLayerWindow});
const layers = mapLayerWindow.VectorWarpLocationMapLayers.createMapLayers({
  finite: value => Number.isFinite(Number(value)) ? Number(value) : null,
  array: value => Array.isArray(value) ? value : []
});
function fakeMap({throwOnLayer = false} = {}) {
  const sources = new Map(), mapLayers = new Map();
  return {sources, mapLayers,
    getSource: id => sources.get(id), getLayer: id => mapLayers.get(id),
    addSource(id, definition) {
      const source = {definition, updates: [], setData(data) { this.updates.push(data); }};
      sources.set(id, source);
    },
    addLayer(layer) {
      if (throwOnLayer) throw new Error('style mutation unavailable');
      mapLayers.set(layer.id, layer);
    }};
}
const coverage = {bins: 72, points: [
  {bin: 71, longitude: -80.1, latitude: 40}, {bin: 0, longitude: -80, latitude: 40.1},
  {bin: 1, longitude: -79.9, latitude: 40}, {bin: 4, longitude: -80, latitude: 39.9}
]};
assert.deepEqual(JSON.parse(JSON.stringify(layers.contiguousCoverageRuns(coverage,
  point => point.bin, Number.isFinite).map(run => run.map(row => row.bin)))), [[71, 0, 1], [4]],
  'coverage bins wrap at north and leave unsupported gaps separate');
const layerMap = fakeMap();
assert.equal(layers.syncRadarCoverageLayer(layerMap, coverage, receiver), true);
const coverageSource = layerMap.getSource('vectorwarp-radar-coverage');
assert.equal(layerMap.mapLayers.size, 2, 'coverage creates each MapLibre layer once');
assert.equal(layers.syncRadarCoverageLayer(layerMap, coverage, receiver), true);
assert.equal(layerMap.getSource('vectorwarp-radar-coverage'), coverageSource,
  'refresh retains the geographic coverage source identity');
assert.equal(coverageSource.updates.length, 1, 'refresh updates the existing coverage source');
assert.equal(layerMap.mapLayers.size, 2, 'refresh does not duplicate coverage layers');
layers.syncBearingOverlayLayers(layerMap, [{id: 'track', uncertainty_corridor: [
  {longitude: -80, latitude: 40}, {longitude: -79.9, latitude: 40.1}
]}]);
const bearingSource = layerMap.getSource('vectorwarp-bearing-overlays');
const bearingLayerCount = layerMap.mapLayers.size;
layers.syncBearingOverlayLayers(layerMap, []);
assert.equal(layerMap.getSource('vectorwarp-bearing-overlays'), bearingSource,
  'bearing refresh retains its source identity');
assert.equal(bearingSource.updates.length, 1, 'bearing refresh uses setData');
assert.equal(layerMap.mapLayers.size, bearingLayerCount, 'bearing layers remain idempotent');
assert.equal(layers.syncRadarCoverageLayer(fakeMap({throwOnLayer: true}), coverage, receiver), false,
  'renderer layer mutation failures retain the SVG compatibility fallback');
const malformedReceiver = {get longitude() { throw new Error('bad coverage data'); }, latitude: 40};
assert.throws(() => layers.syncRadarCoverageLayer(fakeMap(), coverage, malformedReceiver),
  /bad coverage data/, 'coverage construction failures are not swallowed as renderer failures');
console.log('Map fusion: canonical ADS-B preservation, one identity/marker, radar-only geometry, bounded opt-in regions, tightened bounds and explicit operational fixes pass.');
