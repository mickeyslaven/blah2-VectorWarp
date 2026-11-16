'use strict';
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const yaml = require('js-yaml');

const profiles = ['config-kraken.yml', 'config-usrp.yml',
  'config-hackrf.yml', 'config-pi4-rspduo.yml'];
const load = name => yaml.load(fs.readFileSync(
  path.join(__dirname, '..', 'config', name), 'utf8'));
const select = value => {
  const detection = value.process.detection;
  const tracker = value.process.tracker;
  return {
    detector: Object.fromEntries(['pfa', 'nGuard', 'nTrain', 'minDelay',
      'minDoppler', 'nCentroid', 'integration_cpis',
      'integration_motion_bins'].map(key => [key, detection[key]])),
    track_before_detect: detection.track_before_detect,
    doppler_ridge_scrubber: detection.doppler_ridge_scrubber,
    live_temporal_shadow: detection.live_temporal_shadow,
    tracker: {enable: tracker.enable, profile: tracker.profile,
      initiate: tracker.initiate, delete: tracker.delete}
  };
};

const baseline = select(load(profiles[0]));
for (const profile of profiles.slice(1))
  assert.deepStrictEqual(select(load(profile)), baseline,
    `${profile} must not fork receiver-independent radar behavior`);

assert.deepStrictEqual(
  baseline.live_temporal_shadow.association_sustain_cpis, 4);
assert.deepStrictEqual(
  Object.keys(baseline.live_temporal_shadow).sort(),
  Object.keys(select(load(profiles.at(-1))).live_temporal_shadow).sort());
console.log('Unified radar contract: receiver profiles share detector, temporal, and tracker semantics.');
