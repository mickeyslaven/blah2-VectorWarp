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
console.log('Event-driven frame history tests passed.');
