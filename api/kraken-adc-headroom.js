'use strict';

const BINS = 256;
const CENTER = 127.5;

function finite(value) {
  return typeof value === 'number' && Number.isFinite(value);
}

function percentileFraction(histogram, percentile) {
  const observations = histogram.reduce((sum, count) => sum + count, 0);
  if (observations < 1) return null;
  const ranked = histogram.map((count, code) => ({
    count, fraction: Math.abs(code - CENTER) / CENTER
  })).sort((a, b) => a.fraction - b.fraction);
  const threshold = Math.ceil(observations * percentile);
  let cumulative = 0;
  for (const item of ranked) {
    cumulative += item.count;
    if (cumulative >= threshold) return item.fraction;
  }
  return 1;
}

function normalizeAdcHeadroom(value, channelCount) {
  if (value === undefined) return null;
  if (!value || value.schema !== 1 || value.source !== 'rtl2832_uint8_precalibration' ||
      value.sample_stride !== 64 || value.center_code !== CENTER ||
      value.full_scale_code !== CENTER || !Array.isArray(value.channels) ||
      value.channels.length !== channelCount)
    throw new TypeError('Suite supplied malformed raw ADC headroom metadata.');
  const seen = new Set();
  const channels = value.channels.map(channel => {
    if (!channel || channel.unavailable === true || !Number.isInteger(channel.channel) ||
        channel.channel < 0 || channel.channel >= channelCount || seen.has(channel.channel) ||
        ![channel.generation, channel.components, channel.rail_components,
          channel.near_rail_components, channel.sampled_components].every(Number.isSafeInteger) ||
        channel.generation < 0 || channel.components < 0 || channel.rail_components < 0 ||
        channel.near_rail_components < channel.rail_components ||
        channel.near_rail_components > channel.components || channel.sampled_components < 0 ||
        channel.sampled_components > channel.components || !finite(channel.rms_fraction) ||
        channel.rms_fraction < 0 || channel.rms_fraction > 1 ||
        !finite(channel.actual_gain_db) || channel.actual_gain_db < -1 || channel.actual_gain_db > 50 ||
        !Array.isArray(channel.histogram) || channel.histogram.length !== BINS ||
        !channel.histogram.every(count => Number.isSafeInteger(count) && count >= 0) ||
        channel.histogram.reduce((sum, count) => sum + count, 0) !== channel.sampled_components)
      throw new TypeError('Suite supplied malformed per-channel raw ADC headroom counters.');
    seen.add(channel.channel);
    return {
      channel: channel.channel,
      generation: channel.generation,
      components: channel.components,
      railComponents: channel.rail_components,
      nearRailComponents: channel.near_rail_components,
      sampledComponents: channel.sampled_components,
      actualGainDb: channel.actual_gain_db,
      railFraction: channel.components ? channel.rail_components / channel.components : null,
      nearRailFraction: channel.components ? channel.near_rail_components / channel.components : null,
      rmsFraction: channel.rms_fraction,
      p999Fraction: percentileFraction(channel.histogram, .999),
      histogram: channel.histogram.slice()
    };
  }).sort((a, b) => a.channel - b.channel);
  return {schemaVersion: 1, source: value.source, sampleStride: value.sample_stride,
    channels, complete: channels.every(channel => channel.components > 0 &&
      channel.sampledComponents > 0 && channel.p999Fraction !== null)};
}

module.exports = {normalizeAdcHeadroom, percentileFraction, BINS};
