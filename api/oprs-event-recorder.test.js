'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {OprsEventRecorder} = require('./oprs-event-recorder');

const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'oprs-events-'));
const filename = path.join(directory, 'events.ndjson');
const recorder = new OprsEventRecorder({capture: {fs: 2_000_000}},
  {filename, runId: 'run-test', now: () => 1234});
recorder.acceptDetection({timestamp: 1000, delay: [12, NaN, 14], doppler: [-80, 0, -70]});
recorder.acceptTracker({timestamp: 1000, data: [
  {id: 'a', state: 'ACTIVE', n: 3, delay: 80, doppler: -80},
  {id: 'b', state: 'COASTING', n: 3, delay: 90, doppler: -70},
]});
recorder.acceptTracker({timestamp: 2000, data: [
  {id: 'a', state: 'ACTIVE', n: 4, delay: 81, doppler: -78},
]});
recorder.acceptTracker({timestamp: 3000, data: [
  {id: 'a', state: 'ACTIVE', n: 1, delay: 82, doppler: -76},
]});
recorder.acceptTiming({timestamp: 3000, cpi: 200, capture_backlog_ms: 5});
recorder.close();

const rows = fs.readFileSync(filename, 'utf8').trim().split('\n').map(JSON.parse);
assert.equal(rows[0].kind, 'run_start');
assert.equal(rows.filter(row => row.kind === 'detection').length, 2,
  'non-finite detections must not enter evidence');
assert.equal(rows.filter(row => row.kind === 'track_measurement').length, 3,
  'only ACTIVE measurement-supported updates are captured');
const starts = rows.filter(row => row.kind === 'track_initiation');
assert.equal(starts.length, 2, 'an association-count reset starts a new lifecycle');
assert.notEqual(starts[0].track_id, starts[1].track_id);
assert.equal(rows.find(row => row.kind === 'timing_frame').cpi_duration_ms, null);
assert(rows.find(row => row.kind === 'track_measurement').delay_km > 0);
assert.equal(rows.at(-1).kind, 'run_end');
assert.deepEqual(rows.map(row => row.sequence), rows.map((_row, index) => index));

console.log('oprs-event-recorder tests passed');
