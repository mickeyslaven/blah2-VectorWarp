'use strict';
const assert = require('assert');
const {PresentationHistory, frameTimestamp} = require('./presentation-history');

assert.equal(frameTimestamp({timestamp: 1000}), 1000);
assert.equal(frameTimestamp(JSON.stringify({frameTimestamp: 1500})), 1500);
assert.equal(frameTimestamp({timestamp: [1000, 2000]}), 2000);
assert.equal(frameTimestamp('invalid'), null);

const history = new PresentationHistory({maxAgeMs: 10000, maxBytes: 10000});
history.add(JSON.stringify({timestamp: 1000, value: 'a'}), 1000);
history.add(JSON.stringify({timestamp: 1500, value: 'b'}), 1500);
history.add(JSON.stringify({timestamp: 2000, value: 'c'}), 2000);
assert.equal(JSON.parse(history.at(1750)).value, 'b');
assert.equal(history.at(999), null);
assert.equal(JSON.parse(history.at(9999)).value, 'c');
assert.equal(JSON.parse(history.exact(1500)).value, 'b');
assert.equal(history.exact(1750), null, 'Exact-frame presentation never substitutes an adjacent CPI');
assert.deepEqual(history.diagnostics(500), {
  samples: 3, latest_timestamp: 2000, interval_median_ms: 500,
  interval_p95_ms: 500, interval_max_ms: 500, expected_interval_ms: 500,
  within_100ms_fraction: 1
});
history.clear();
assert.equal(history.at(9999), null);

console.log('Aligned presentation history selection PASS.');
