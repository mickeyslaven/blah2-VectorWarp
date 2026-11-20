'use strict';

const positive = value => typeof value === 'number' && Number.isFinite(value) && value > 0;

// Native product coordinates override configuration coordinates. Replay never
// inherits the currently selected live receiver's rate or carrier. A malformed
// or contradictory declared value is an error, not permission to fall back.
function observationCoordinates(config, frames = [], {requireRate = true} = {}) {
  const replay = config?.capture?.replay?.state === true;
  const values = (Array.isArray(frames) ? frames : [frames]).flatMap(frame =>
    frame && typeof frame === 'object' ? [frame, frame.tracks].filter(Boolean) : []);
  const resolve = (field, configured) => {
    const declared = values.filter(frame => frame[field] !== undefined).map(frame => frame[field]);
    if (declared.some(value => !positive(value)) || new Set(declared).size > 1) return null;
    if (declared.length) return {value: declared[0], source: 'native-product'};
    return positive(configured) ? {value: configured,
      source: replay ? 'explicit-replay-coordinate' : 'validated-live-config-fallback'} : null;
  };
  const capture = config?.capture || {}, fallback = replay ? capture.replay : capture;
  const rate = resolve('sample_rate_hz', replay ? fallback?.sample_rate_hz : fallback?.fs);
  const frequency = resolve('frequency_hz', replay ? fallback?.frequency_hz : fallback?.fc);
  if (!frequency || requireRate && !rate ||
      values.some(frame => frame.sample_rate_hz !== undefined) && !rate) return null;
  return {sampleRate: rate?.value ?? null, frequency: frequency.value,
    sampleRateSource: rate?.source ?? null, frequencySource: frequency.source, replay};
}

module.exports = {observationCoordinates};
