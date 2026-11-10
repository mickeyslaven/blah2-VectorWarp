'use strict';

const clone = value => JSON.parse(JSON.stringify(value));

const LIMITS = Object.freeze({railFraction: .00001, p999Fraction: .70,
  minimumSampledComponents: 65536, minimumGainDb: 0, maximumBackoffDb: 6});

function enabled(config) {
  const capture = config?.capture;
  const gains = capture?.device?.gain;
  return capture?.replay?.state !== true && Array.isArray(gains) && gains.length > 0 &&
    gains.every(value => typeof value === 'number' && Number.isFinite(value) && value >= 0);
}

function unsafe(channel) {
  return channel.railFraction > LIMITS.railFraction ||
    channel.p999Fraction > LIMITS.p999Fraction;
}

function validateInputQuality(value, config) {
  const gains = config?.capture?.device?.gain;
  if (!enabled(config) || !value || !Array.isArray(value.channels) ||
      value.channels.length !== gains.length)
    throw Object.assign(new Error('Direct-receiver gain telemetry does not match the configured inputs.'),
      {code: 'DIRECT_GAIN_TELEMETRY_MISMATCH'});
  const seen = new Set();
  const channels = value.channels.map(channel => {
    const sampled = Number(channel?.sampledComponents);
    if (!Number.isInteger(channel?.channel) || channel.channel < 0 ||
        channel.channel >= gains.length || seen.has(channel.channel) ||
        !Number.isSafeInteger(sampled) || sampled < LIMITS.minimumSampledComponents ||
        typeof channel.gainDb !== 'number' || !Number.isFinite(channel.gainDb) ||
        Math.abs(channel.gainDb - gains[channel.channel]) > .11 ||
        !['railFraction', 'nearRailFraction', 'p999Fraction', 'rmsFraction'].every(key =>
          typeof channel[key] === 'number' && Number.isFinite(channel[key]) &&
          channel[key] >= 0 && channel[key] <= 1))
      throw Object.assign(new Error('Direct-receiver gain telemetry is incomplete, stale, or malformed.'),
        {code: 'DIRECT_GAIN_TELEMETRY_INVALID'});
    seen.add(channel.channel);
    return {channel: channel.channel, gainDb: channel.gainDb,
      sampledComponents: sampled, railFraction: channel.railFraction,
      nearRailFraction: channel.nearRailFraction, p999Fraction: channel.p999Fraction,
      rmsFraction: channel.rmsFraction};
  }).sort((left, right) => left.channel - right.channel);
  if (channels.some((channel, index) => channel.channel !== index))
    throw Object.assign(new Error('Direct-receiver telemetry does not cover each input exactly once.'),
      {code: 'DIRECT_GAIN_TELEMETRY_INVALID'});
  const unsafeChannels = channels.filter(unsafe).map(channel => channel.channel);
  return {source: value.source || 'receiver-input-quality', channels,
    safe: unsafeChannels.length === 0, unsafeChannels,
    worstP999Fraction: Math.max(...channels.map(channel => channel.p999Fraction)),
    railsObserved: channels.some(channel => channel.railFraction > LIMITS.railFraction)};
}

function backoff(config, probes) {
  if (!enabled(config) || !Array.isArray(probes) || probes.length < 2)
    throw Object.assign(new Error('Automatic direct gain backoff requires two matching unsafe measurements.'),
      {code: 'DIRECT_GAIN_BACKOFF_UNQUALIFIED'});
  const candidate = clone(config), gains = candidate.capture.device.gain;
  const common = probes.map(probe => new Set(probe.unsafeChannels || []))
    .reduce((left, right) => new Set([...left].filter(channel => right.has(channel))));
  const adjustments = [];
  for (const channel of [...common].sort((a, b) => a - b)) {
    const peak = Math.max(...probes.map(probe => probe.channels[channel].p999Fraction));
    const ratioDb = peak > LIMITS.p999Fraction ?
      Math.ceil(20 * Math.log10(peak / LIMITS.p999Fraction)) : 1;
    const amount = Math.max(1, Math.min(LIMITS.maximumBackoffDb, ratioDb));
    const before = gains[channel], after = Math.max(LIMITS.minimumGainDb, before - amount);
    if (after === before)
      throw Object.assign(new Error(`Input ${channel} is still clipping at minimum gain.`),
        {code: 'DIRECT_GAIN_UNSAFE_AT_MINIMUM'});
    gains[channel] = after;
    adjustments.push({channel, beforeDb: before, afterDb: after,
      reason: probes.some(probe => probe.channels[channel].railFraction > LIMITS.railFraction) ?
        'rail-clipping' : 'p999-headroom'});
  }
  if (!adjustments.length)
    throw Object.assign(new Error('No input was unsafe in both consecutive measurements.'),
      {code: 'DIRECT_GAIN_BACKOFF_UNQUALIFIED'});
  return {config: candidate, adjustments};
}

const signature = probe => (probe.unsafeChannels || []).join(',');

module.exports = {enabled, validateInputQuality, backoff, signature, LIMITS};
