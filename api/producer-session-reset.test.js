'use strict';

const assert = require('node:assert/strict');
const {CrossFrequencySupportTracker, MultiIlluminatorTrackFuser} =
  require('./multi-illuminator-track-fusion');

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
fusion.seenEpochs.add(0); fusion.seenCaptureIntervals.set('1:2', 100000);
fusion.crossFrequencySupport.lastEpoch = 0; fusion.reset();
assert.equal(fusion.tracks.size, 0); assert.equal(fusion.lastKey, null);
assert.equal(fusion.lastCaptureEnd, null); assert.equal(fusion.seenEpochs.size, 0);
assert.equal(fusion.seenCaptureIntervals.size, 0);
assert.equal(fusion.crossFrequencySupport.lastEpoch, null);

// The fusion owners retain unsuccessful outcomes too. HTTP adapters can read
// their snapshots without maintaining a second cache or a separate reset lane.
for (const owner of [fusion]) {
  owner.reset();
  assert.equal(owner.snapshot(), null);
  const rejected = owner.unavailable('Incomplete capture cohort.');
  assert.equal(rejected.available, false);
  assert.equal(owner.lastResult, rejected);
  assert.equal(owner.snapshot().available, false);
  owner.reset();
  assert.equal(owner.snapshot(), null, 'Reset cannot resurrect a retained failure');
}

console.log('Producer session reset clears every capture-interval watermark.');
