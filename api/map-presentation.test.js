'use strict';

const assert = require('assert');
const zlib = require('zlib');
const {encodeMapResponse, presentMap, presentationQuery, requestedLimit} = require('./map-presentation');

const frame = {timestamp: 123, nRows: 6, nCols: 6,
  doppler: [-3, -2, -1, 0, 1, 2], delay: [0, 1, 2, 3, 4, 5],
  data: Array.from({length: 6}, (_, row) =>
    Array.from({length: 6}, (_, column) => row * 10 + column)), maxPower: 55};

assert.strictEqual(presentMap(frame), frame, 'full map is the default API contract');
assert.strictEqual(presentMap(frame, {max_rows: 'garbage', max_cols: '2'}), frame,
  'invalid presentation parameters fail safely to the full map');
assert.equal(requestedLimit('31'), null);
assert.equal(requestedLimit('240'), 240);
assert.deepEqual(presentationQuery({originalUrl: '/api/map?max_rows=240&max_cols=200',
  query: {}}), {max_rows: '240', max_cols: '200'});

const compact = presentMap(frame, {max_rows: '2', max_cols: '2'});
// Limits below 32 deliberately fail closed, so use a realistic native grid.
const large = {timestamp: 124, nRows: 96, nCols: 96,
  doppler: Array.from({length: 96}, (_, i) => i),
  delay: Array.from({length: 96}, (_, i) => i),
  data: Array.from({length: 96}, (_, row) =>
    Array.from({length: 96}, (_, column) => row * 1000 + column))};
const reduced = presentMap(large, {max_rows: '32', max_cols: '32'});
assert.equal(reduced.data.length, 32);
assert.equal(reduced.data[0].length, 32);
assert.equal(reduced.data[0][0], 2002, 'each display cell preserves its source-block peak');
assert.deepEqual(reduced.presentation, {peak_preserving: true, source_rows: 96,
  source_cols: 96, row_step: 3, column_step: 3});
assert.equal(reduced.timestamp, large.timestamp);
assert.ok(JSON.stringify(reduced).length < JSON.stringify(large).length / 5,
  'compact display transfer must be materially smaller than the processor grid');
assert.strictEqual(presentMap(large, {max_rows: '32', max_cols: '32'}), reduced,
  'repeated clients reuse the compact representation for one source frame');
const serialized = JSON.stringify({...large, timestamp: 125});
const serializedReduced = presentMap(serialized, {max_rows: '32', max_cols: '32'});
assert.equal(serializedReduced.data.length, 32,
  'live socket JSON strings must cross the presentation boundary before compaction');
assert.strictEqual(presentMap(serialized), serialized,
  'the default full-resolution endpoint preserves its existing wire representation');
const encoded = encodeMapResponse(serialized, {max_rows: '32', max_cols: '32'}, 'br, gzip');
assert.equal(encoded.contentEncoding, 'gzip');
assert.equal(JSON.parse(zlib.gunzipSync(encoded.body)).data.length, 32);
assert.ok(encoded.body.length < Buffer.byteLength(serialized) / 5,
  'HTTP content negotiation keeps remote live-map transfers bounded');
assert.strictEqual(compact, frame);

console.log('Peak-preserving mobile map presentation passed.');
