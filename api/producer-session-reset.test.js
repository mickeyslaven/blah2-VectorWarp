'use strict';

const assert = require('node:assert/strict');
const {CrossFrequencySupportTracker, MultiIlluminatorTrackFuser} =
  require('./multi-illuminator-track-fusion');
const {MultiSiteMapSupportTracker} = require('./multi-site-map-support');

const config = {capture: {fs: 2400000}, location: {rx: {
  latitude: 40, longitude: -80, altitude: 300}}, process: {
  data: {cpi: .5}, tracker: {initiate: {M: 3, N: 6, maxAcc: 10}, delete: 3},
  multi_illuminator: {illuminators: []}}};

const cross = new CrossFrequencySupportTracker(config);
cross.tracks.set('old', {}); cross.lastEpoch = 0; cross.lastCaptureEnd = 9000;
cross.seenCpis.add('0:1:2'); cross.lastResult = {available: true};
cross.reset();
assert.equal(cross.tracks.size, 0); assert.equal(cross.lastEpoch, null);
assert.equal(cross.lastCaptureEnd, null); assert.equal(cross.seenCpis.size, 0);
assert.equal(cross.lastResult, null);

const fusion = new MultiIlluminatorTrackFuser(config);
fusion.tracks.set('old', {}); fusion.lastKey = 'old'; fusion.lastCaptureEnd = 9000;
fusion.seenEpochs.add(0); fusion.seenCaptureIntervals.add('1:2');
fusion.crossFrequencySupport.lastEpoch = 0; fusion.reset();
assert.equal(fusion.tracks.size, 0); assert.equal(fusion.lastKey, null);
assert.equal(fusion.lastCaptureEnd, null); assert.equal(fusion.seenEpochs.size, 0);
assert.equal(fusion.seenCaptureIntervals.size, 0);
assert.equal(fusion.crossFrequencySupport.lastEpoch, null);

const maps = new MultiSiteMapSupportTracker(config);
maps.tracks.set('old', {}); maps.lastKey = 'old';
maps.lastCohort = {capture_epoch: 0, capture_end_input_sample: 9000};
maps.lastResult = {available: false}; maps.reset();
assert.equal(maps.tracks.size, 0); assert.equal(maps.lastKey, null);
assert.equal(maps.lastCohort, null); assert.equal(maps.lastResult, null);
maps.tracks.set('scientific', {});
const unavailable = maps.unavailable('Presentation-only map transport.');
assert.equal(unavailable.available, false);
assert.equal(unavailable.reason, 'Presentation-only map transport.');
assert.equal(unavailable.diagnostic_only, true);
assert.equal(maps.tracks.size, 0);

console.log('Producer session reset clears every capture-interval watermark.');
