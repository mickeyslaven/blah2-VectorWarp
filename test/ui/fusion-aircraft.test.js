'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const model = require('../../html/js/fusion_aircraft.js');
const {aircraftSvgMarkup,aircraftAltitudeColor,radarVisualSemantics} = require('../../html/js/common.js');
const receiver = {latitude:40,longitude:-80};
const adsb = (key, association) => ({key,item:{hex:key,flight:key,track:45},point:[1,2],trail:[],association});
const radar = {id:'F1',state:'FUSED_ACTIVE',local_east_km:9,local_north_km:8,
  contributors:[{illuminator:'ch1'},{illuminator:'ch2'}]};
const result = {available:true,tracks:[radar],validation:{held_out_truth:true,adsb_used_by_solver:false,
  target_kind:'operational-fused-tracks',matches:[{candidate_id:'F1',adsb_hex:'matched'}]}};
const values = model.aircraft([adsb('plain'),adsb('detected',{detected:true,channels:['one','one','two']}),
  adsb('tracked',{tracked:true}),adsb('matched')],result,receiver);
assert.equal(values.length,4,'one displayed record for an explicitly matched aircraft');
assert.equal(new Set(values.map(value=>value.id)).size,4);
assert.deepEqual(values.find(value=>value.key==='matched').point,[9,8],'radar coordinate is never replaced with ADS-B');
assert.equal(values.find(value=>value.key==='matched').source,'radar');
assert.equal(values.find(value=>value.key==='tracked').source,'adsb','tracking alone does not imply radar position');
assert.deepEqual(values.find(value=>value.key==='detected').channels,['one','two']);
assert.equal(model.state({tracked:true}), 'tracked', 'tracked does not need a redundant detected flag');
assert.equal(model.positionColor({...values[0],item:{alt_baro:30000}}),aircraftAltitudeColor({alt_baro:30000}),
  'Fusion and regular Map share the canonical altitude color');
assert.equal(model.positionColor(values[0]),radarVisualSemantics.colors.adsbFallback);
assert.equal(model.sourceLabel('adsb'),radarVisualSemantics.labels.adsb);
assert.equal(model.sourceLabel('radar'),radarVisualSemantics.labels.independent);
assert.equal(model.stateLabel('tracked'),radarVisualSemantics.labels.tracked);
assert.deepEqual(model.transmitterSiteLabels([
  {id:'TX-A',frequency_hz:509e6,transmitter_site:'lane-a',transmitter:{latitude:40.495278,longitude:-80.004444}},
  {id:'TX-B',frequency_hz:515e6,transmitter_site:'lane-b',transmitter:{latitude:40.495278,longitude:-80.004444}},
  {id:'TX-C',frequency_hz:527e6,transmitter_site:'lane-c',transmitter:{latitude:40.46325,longitude:-80.0043056}},
  {id:'primary',frequency_hz:509e6}
]), ['Site 1','Site 1','Site 2','Site 1'], 'co-sited relayed channels share one site label');
for (const value of values) {
  const color = model.positionColor(value);
  const canonical = aircraftSvgMarkup(value.item,color);
  const glyph = model.glyphMarkup(value,canonical);
  assert.equal((glyph.match(/<svg /g)||[]).length,1);
  assert.equal((glyph.match(/fusion-aircraft-planform/g)||[]).length,1);
  assert.doesNotMatch(glyph,/aircraft-3d-depth|aircraft-3d-highlight/,'no offset or highlighted duplicate shapes');
  assert.match(glyph,new RegExp(`stroke="${color}"`));
  assert.match(glyph,new RegExp(`fill="${color}"`));
  assert.match(glyph,/fill-opacity="1"/);
  if (value.source === 'radar') assert.match(glyph,/fusion-evidence fused/);
  else if (value.state === 'tracked') assert.match(glyph,/fusion-evidence tracked/);
  else if (value.state === 'detected') assert.match(glyph,/fusion-evidence detected/);
  else assert.doesNotMatch(glyph,/fusion-evidence/);
  const canonicalTop = canonical.match(/<g class="aircraft-3d-top"[^>]*>(.*?)<\/g>/)[1];
  assert.ok(glyph.includes(canonicalTop),'canonical map planform remains intact');
}
const guarded = [
  {...radar,state:'FUSED_CANDIDATE'}, {...radar,diagnostic_only:true},
  {...radar,qualification:{qualified:false}}, {...radar,local_north_km:null}, {...radar,operational_position:false}
];
assert.equal(model.aircraft([], {available:true,tracks:guarded}, receiver).length,0);
assert.equal(model.aircraft([], {...result,available:false}, receiver).length,0);
assert.equal(model.aircraft([], {...result,adsb_used_by_solver:true}, receiver).length,0);
assert.equal(model.aircraft([], {available:true,tracks:[],position_candidates:[{...radar,
  state:'ENVELOPE_SUPPORTED',operational_position:false}]}, receiver).length,0,
  'two-site diagnostic envelopes never become aircraft point markers');
assert.equal(model.aircraft([adsb('matched')], {...result,validation:{held_out_truth:false,matches:result.validation.matches}}, receiver).length,2,
  'no implicit client-side identity matching');
assert.equal(model.aircraft([], {...result,tracks:[radar,radar]}, receiver).length,1,'duplicate radar IDs do not duplicate glyphs');
const source = fs.readFileSync(path.join(__dirname,'../../html/js/fusion_scope.js'),'utf8');
assert.doesNotMatch(source,/Radar-fused|MULTI-SITE<br>FUSED|symbol:'star'|symbol:'diamond'|glow|fusion-evidence-note/);
assert.match(source,/mode:'text'/,'aircraft labels do not draw another marker');
assert.match(source,/Independent radar position/,'Fusion legend distinguishes independently solved positions');
assert.match(source,/hypothesesToggle\.checked \? \[/,'diagnostics require explicit opt-in');
assert.match(source,/transition:\{duration:0\}/);
assert.match(fs.readFileSync(path.join(__dirname,'../../html/lib/blah2.css'),'utf8'),/prefers-reduced-motion:reduce/);
const index = fs.readFileSync(path.join(__dirname,'../../html/index.html'),'utf8');
assert.match(index, /Detection<\/span>.*Radar track<\/span>.*Coasting<\/span>.*ADS-B position<\/span>.*Remote ID<\/span>/,
  'Delay-Doppler legend uses shared order and labels');
assert.match(index,/class="adsb aircraft-altitude-gradient"/,'Delay-Doppler legend reflects canonical altitude colors');
console.log('Fusion aircraft: source/evidence, canonical single glyph, identity deduplication, qualification and diagnostic contracts pass.');
