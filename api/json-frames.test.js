'use strict';

const assert = require('assert/strict');
const jsonFrames = require('./json-frames.js');

const objects = [
  {text: 'braces } {, quote \", slash \\, emoji 🙂', nested: [{a: 2}]},
  {second: true, values: [1, 2, 3]}
];
const expected = objects.map(JSON.stringify);
const wire = Buffer.from(` \n${expected.join(' \t')}\n`);

for (let width = 1; width <= wire.length; ++width) {
  const received = [];
  const feed = jsonFrames(frame => received.push(frame));
  for (let offset = 0; offset < wire.length; offset += width)
    feed(wire.subarray(offset, offset + width));
  assert.deepEqual(received, expected, `fragment width ${width}`);
}

const reconnect = [];
jsonFrames(frame => reconnect.push(frame))(Buffer.from('{"unfinished":'));
jsonFrames(frame => reconnect.push(frame))(Buffer.from(expected[1]));
assert.deepEqual(reconnect, [expected[1]], 'one socket must not leak into the next');

assert.throws(() => jsonFrames(() => {})('x'), /Expected radar JSON object/);
assert.throws(() => jsonFrames(() => {})('{"missing":]'), /Invalid radar JSON object/);
assert.throws(() => jsonFrames(() => {}, {maxBytes: 16})(Buffer.from('{"payload":"' + 'x'.repeat(16))),
  /exceeds limit/);

let accepts = false;
const delivered = [];
const paused = jsonFrames(frame => {
  if (!accepts) return false;
  delivered.push(frame);
});
assert.equal(paused(Buffer.from(expected.join(''))), false);
accepts = true;
assert.equal(paused(Buffer.alloc(0)), true);
assert.deepEqual(delivered, expected, 'a held frame resumes before later coalesced frames');

// One validation per complete frame, even if a consumer applies backpressure.
// Raw decimal spelling and exact uint64 strings must remain independently intact.
const raw = '{ "timestamp":1, "n":1.2300000000000000, "id":"18446744073709551615" }';
const originalParse = JSON.parse;
let parses = 0, resume = false, firstParsed;
JSON.parse = (...args) => { parses++; return originalParse(...args); };
try {
  const feed = jsonFrames((text, parsed) => {
    assert.equal(text, raw);
    assert.equal(parsed.id, '18446744073709551615');
    assert.equal(parsed.n, 1.23);
    if (firstParsed) assert.equal(parsed, firstParsed, 'Resume reuses validation');
    firstParsed = parsed;
    return resume;
  });
  assert.equal(feed(raw), false);
  assert.equal(feed(''), false);
  resume = true;
  assert.equal(feed(''), true);
  assert.equal(parses, 1);
  for (const malformed of ['{"x":01}', '{"x":NaN}', '{"x":1,}', '{"x":"\\uZZZZ"}']) {
    let called = false;
    assert.throws(() => jsonFrames(() => { called = true; })(malformed), /Invalid radar JSON object/);
    assert.equal(called, false, 'Invalid JSON cannot reach any product consumer');
  }
} finally { JSON.parse = originalParse; }

console.log(JSON.stringify({pass: true, fragmentedAndUnicode: true, coalesced: true,
  reconnectIsolation: true, malformedAndBounded: true}));
