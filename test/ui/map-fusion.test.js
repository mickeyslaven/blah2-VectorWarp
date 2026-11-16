'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
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
assert.doesNotMatch(source,/vectorwarp-fused-track-hypotheses|vectorwarp-fusion-ambiguous-hypotheses/,'no duplicate fused point layers');
assert.match(source,/syncBearingOverlayLayers\(map, bearingRows\)/);
assert.match(source,/selectedIsRadar \? radarAircraftTrackHistories : aircraftTrackHistories/,'position-source trails stay separate');
assert.match(source,/item\._radar_position \? radarVisualSemantics.colors.independent : aircraftAltitudeColor\(item\)/);
const page=fs.readFileSync(require.resolve('../../html/display/locations/index.html'),'utf8');
assert.match(page,/fusion_aircraft\.js\?v=[\w.-]+.*insights\.js\?v=ui-consistency-29/);
console.log('Map fusion: canonical ADS-B preservation, one identity/marker, radar-only geometry, bounded opt-in regions, tightened bounds and explicit operational fixes pass.');
