'use strict';

const assert = require('node:assert/strict');
const {test} = require('node:test');
const {normalizeAdcHeadroom, percentileFraction} = require('./kraken-adc-headroom');

function raw(channels = 2) {
  return {schema: 1, source: 'rtl2832_uint8_precalibration', sample_stride: 64,
    center_code: 127.5, full_scale_code: 127.5,
    channels: Array.from({length: channels}, (_, channel) => {
      const histogram = Array(256).fill(0); histogram[128] = 999; histogram[255] = 1;
      return {channel, generation: 2, actual_gain_db: 19.7, components: 64000, rail_components: 1,
        near_rail_components: 2, sampled_components: 1000, rms_fraction: .1, histogram};
    })};
}

test('normalizes exact raw counters and derives the absolute 99.9th percentile', () => {
  const value = normalizeAdcHeadroom(raw(), 2);
  assert.equal(value.complete, true);
  assert.equal(value.channels[0].railFraction, 1 / 64000);
  assert.equal(value.channels[0].nearRailFraction, 2 / 64000);
  assert.equal(value.channels[0].actualGainDb, 19.7);
  assert.equal(value.channels[0].p999Fraction, .5 / 127.5);
  assert.equal(percentileFraction(Array(256).fill(0), .999), null);
});

test('rejects malformed, duplicate, inconsistent and unavailable telemetry', () => {
  const mutations = [
    value => { value.source = 'mchq'; },
    value => { value.channels.pop(); },
    value => { value.channels[1].channel = 0; },
    value => { value.channels[0].near_rail_components = 0; },
    value => { value.channels[0].histogram[128]--; },
    value => { value.channels[0].unavailable = true; }
  ];
  for (const mutate of mutations) {
    const value = raw(); mutate(value);
    assert.throws(() => normalizeAdcHeadroom(value, 2), /malformed/);
  }
  assert.equal(normalizeAdcHeadroom(undefined, 2), null);
});
