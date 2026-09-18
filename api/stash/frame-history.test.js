'use strict';
const assert = require('assert');
const histories = require('./frame-history');
const map = histories.maxhold();
const spectrum = histories.iqdata();
const timing = histories.timing();
const detections = histories.detection();

assert.equal(map.get_data(), null);
for (let i = 1; i <= 30; i++) {
  const timestamp = i * 20;
  const frame = {timestamp, nRows: 1, delay: [1, 2], doppler: [3], data: [[i, -i]]};
  map.update_data(JSON.stringify(frame));
  map.update_data(frame); // Duplicate same-frame delivery is harmless.
  assert.deepEqual(frame.data, [[i, -i]], 'Max hold must not mutate live map data');
  spectrum.update_data({timestamp, frequency: [100, 101], spectrum: [i, i + 1]});
  timing.update_data({timestamp, cpi: 5, nCpi: i, ...(i > 20 ? {tracker: 1} : {})});
  detections.update_data({timestamp, delay: [i], doppler: [2], snr: [10]});
}
assert.deepEqual(map.get_data().data, [[30, -11]]);
assert.equal(spectrum.get_data().timestamp.length, 20);
assert.equal(spectrum.get_data().frameTimestamp, 600);
assert.equal(timing.get_data().timestamp.length, 20);
assert.equal(timing.get_data().tracker.length, 20);
assert.equal(timing.get_data().tracker[0], null, 'Late stages stay aligned with timestamps');
assert.equal(detections.get_data().delay.length, 30, 'Fast frames must not be limited by a 100 ms poller');
map.update_data('malformed');
map.update_data({timestamp: 610, data: []});
assert.equal(map.get_data().timestamp, 600);
map.update_data({timestamp: 620, delay: [1], doppler: [1, 2], data: [[-2], [-3]]});
assert.deepEqual(map.get_data().data, [[-2], [-3]], 'Changed axes reset max hold');
spectrum.update_data({timestamp: 620, frequency: [102], spectrum: [5]});
assert.deepEqual(spectrum.get_data().timestamp, [620], 'Changed tuning resets spectrum history');
detections.update_data({timestamp: 301000, delay: [], doppler: [], snr: []});
assert.deepEqual(detections.get_data().timestamp, []);
assert.equal(detections.get_data().frameTimestamp, 301000, 'Empty frames still clear expired detection history');
const sparse = histories.detection();
for (let i = 1; i <= 120000; i++) sparse.update_data({timestamp: i, delay: [], doppler: [], snr: []});
const sparseOutput = sparse.get_data();
assert.deepEqual(sparseOutput.delay, [], 'High-rate empty frames do not retain empty history records');
assert.equal(sparseOutput.frameTimestamp, 120000);
sparse.update_data({timestamp: 120001, delay: [7], doppler: [8], snr: [9]});
const immutable = sparse.get_data(); assert.throws(() => { immutable.delay[0] = 99; }, /read only/, 'Completed detection output is immutable between message updates.');
assert.equal(sparse.get_data().delay[0], 7);
sparse.update_data({timestamp: 420002, delay: [], doppler: [], snr: []});
assert.deepEqual(sparse.get_data().delay, [], 'Five-minute expiry remains exact after sparse high-rate input.');
const copied = histories.detection(); const source = {timestamp: 1, delay: [1], doppler: [2], snr: [3]};
copied.update_data(source); source.delay[0] = 99;
assert.deepEqual(copied.get_data().delay, [1], 'Detection history does not retain caller-mutable arrays.');
// Independent eager five-minute reference checks deferred reads, exact expiry,
// sparse/dense frames, malformed input and ignored duplicate/older messages.
const deferred = histories.detection();
let expectedFrames = [], latest = null;
for (let i = 0; i < 5000; i++) {
  const timestamp = i * 997;
  const count = i % 7 === 0 ? 3 : i % 5 === 0 ? 1 : 0;
  const frame = {timestamp, delay: Array.from({length: count}, (_, j) => i + j),
    doppler: Array.from({length: count}, (_, j) => -i - j), snr: Array.from({length: count}, (_, j) => j + 10)};
  deferred.update_data(i % 2 ? JSON.stringify(frame) : frame);
  latest = timestamp; expectedFrames = [...expectedFrames, frame].filter(item => timestamp - item.timestamp <= 300000);
  deferred.update_data({timestamp, delay: [999], doppler: [999], snr: [999]});
  deferred.update_data({timestamp: timestamp - 1, delay: [], doppler: [], snr: []});
  deferred.update_data({timestamp: timestamp + 1, delay: [999], doppler: [], snr: []});
  deferred.update_data('{');
  if (i % 137 === 0 || i === 4999) {
    const expected = {frameTimestamp: latest, timestamp: expectedFrames.flatMap(item => item.delay.map(() => item.timestamp)),
      delay: expectedFrames.flatMap(item => item.delay), doppler: expectedFrames.flatMap(item => item.doppler), snr: expectedFrames.flatMap(item => item.snr)};
    assert.deepEqual(deferred.get_data(), expected, 'Lazy detection output must match the complete eager five-minute history');
    assert.strictEqual(deferred.get_data(), deferred.get_data(), 'An unchanged history reuses its completed output');
  }
}
const boundary = histories.detection();
boundary.update_data({timestamp: 1, delay: [4], doppler: [5], snr: [6]});
boundary.update_data({timestamp: 300001, delay: [], doppler: [], snr: []});
assert.deepEqual(boundary.get_data().delay, [4], 'Exactly five minutes remains included');
boundary.update_data({timestamp: 300002, delay: [], doppler: [], snr: []});
assert.deepEqual(boundary.get_data().delay, [], 'The next millisecond expires the last retained detection');
console.log('Event-driven frame history tests passed.');
