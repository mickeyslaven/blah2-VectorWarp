'use strict';
// Forced GC belongs only to this disposable retention test process.
const assert = require('assert/strict');
const {execFileSync} = require('child_process');
if (typeof global.gc !== 'function') {
  execFileSync(process.execPath, ['--expose-gc', __filename], {stdio: 'inherit', timeout: 30000});
} else {
  const history = require('./frame-history').detection();
  let timestamp = -5;
  const fillThrough = end => {
    for (timestamp += 5; timestamp <= end; timestamp += 5) {
      history.update_data({timestamp, delay: [timestamp, 2, 3], doppler: [4, 5, 6], snr: [7, 8, 9]});
    }
    timestamp -= 5;
  };
  const sample = () => {
    // Every sample includes the same complete five-minute window and its
    // rendered output, covering consumers as well as incoming data retention.
    assert.equal(history.get_data().delay.length, 60001 * 3);
    global.gc(); global.gc();
    return process.memoryUsage().heapUsed;
  };
  fillThrough(300000);
  const baseline = sample(), samples = [];
  // Two further complete window turnovers cross prefix-compaction boundaries.
  for (let end = 330000; end <= 900000; end += 30000) {
    fillThrough(end); samples.push(sample());
  }
  const growth = Math.max(...samples) - baseline;
  assert(growth < 8 * 1024 * 1024, `Expired detection payloads retained: ${growth} bytes above a full-window baseline`);
  history.update_data({timestamp: 1200001, delay: [], doppler: [], snr: []});
  assert.equal(history.get_data().delay.length, 0);
  global.gc(); global.gc();
  const cleared = process.memoryUsage().heapUsed;
  assert(cleared < baseline / 2, 'A completely expired detection window must release its payloads and cached output');
  console.log(JSON.stringify({test: 'detection retained heap', retainedWindowMs: 300000,
    additionalWindowTurnovers: 2, baselineBytes: baseline, peakGrowthBytes: growth, clearedBytes: cleared}));
}
