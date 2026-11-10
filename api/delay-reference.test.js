'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const yaml = require('js-yaml');
const {scaleDelayBin, effectiveDelayConfig, legacyDelayReferenceRate} = require('../html/js/config_profiles');
const {laneConfig} = require('./multi-illuminator-evidence');
const {limits, siteKey, associationGates} = require('./evidence-statistics');
const {readConfig, migrateKnownDefaults, setupDefaults, configurationForReceiver} = require('./config-store');
const {validateConfig} = require('./config-manager');
const {selectedIlluminatorConfig} = require('../html/js/common');
const {delaySampleDescription} = require('../html/js/config_ui');
const clone = value => JSON.parse(JSON.stringify(value));
const km = (bin, rate) => bin * 299792.458 / rate;
const config = yaml.load(fs.readFileSync(path.join(__dirname, '../config/config-usrp.yml'), 'utf8'));
config.capture.fs = 61440000;
config.capture.fc = 539000000;
config.process.delay_reference_sample_rate_hz = 2400000;
config.process.ambiguity = {delayMin: -200, delayMax: 400, dopplerMin: -2000, dopplerMax: 2000};
config.process.clutter = {enable: true, followAmbiguity: false, delayMin: -200, delayMax: 400};
config.process.detection.minDelay = 80;
config.process.multi_illuminator = {enabled: true, illuminators: []};

for (const rate of [2400000, 6144000, 300000, 4800000]) {
  const lane = {id: `rate-${rate}`, enabled: true, standard: rate === 300000 ? 'FM' : 'ATSC',
    frequency_hz: 527000000, occupied_bandwidth_hz: rate - 100000,
    output_sample_rate_hz: rate, transmitter: config.location.tx};
  const projected = laneConfig(config, lane), p = projected.process;
  assert.ok(Math.abs(km(p.ambiguity.delayMin, rate) - km(-200, 2400000)) < 1e-12);
  assert.ok(Math.abs(km(p.ambiguity.delayMax, rate) - km(400, 2400000)) < 1e-12,
    `physical search endpoint must remain invariant at ${rate}`);
  assert.ok(Math.abs(km(p.clutter.delayMax, rate) - km(400, 2400000)) < 1e-12);
  const threshold = km(p.detection.minDelay, rate), desired = km(80, 2400000);
  assert.ok(threshold >= desired - 1e-12 && threshold - desired < km(1, rate),
    'minimum detection delay rounds up by less than one lane sample');
  assert.equal(p.ambiguity.dopplerMin, -2000);
  assert.equal(p.ambiguity.dopplerMax, 2000);
  for (const field of ['nGuard', 'nTrain', 'nCentroid']) {
    const configured = config.process.detection[field];
    const effective = Math.ceil(configured * rate / 2400000);
    assert.equal(p.detection[field], effective);
    assert.ok(effective / rate >= configured / 2400000,
      `${field} must not narrow its physical delay radius`);
    assert.ok(effective / rate - configured / 2400000 < 1 / rate,
      `${field} rounding must add less than one lane sample`);
  }
  assert.equal(p.detection.minDoppler, config.process.detection.minDoppler);
  assert.ok(Math.abs(limits(projected).delayMaxKm - km(400, 2400000)) < 1e-12);
  assert.deepEqual(effectiveDelayConfig(projected, rate), projected,
    'a projected config cannot be scaled twice');
  const displayed = selectedIlluminatorConfig(config, {item: lane});
  assert.equal(displayed.process.ambiguity.delayMax, p.ambiguity.delayMax);
  const follow = clone(config); follow.process.clutter.followAmbiguity = true;
  const followed = laneConfig(follow, lane).process;
  assert.equal(followed.clutter.delayMin, followed.ambiguity.delayMin);
  assert.equal(followed.clutter.delayMax, followed.ambiguity.delayMax + 1);
}
assert.equal(scaleDelayBin(-10, 6144000, 2400000, false), -26);
assert.equal(scaleDelayBin(5, 6144000, 2400000, true), 13);
const atsc = laneConfig(config, {id: 'atsc-neighborhood', enabled: true, standard: 'ATSC',
  frequency_hz: 527000000, occupied_bandwidth_hz: 5380000,
  output_sample_rate_hz: 6144000, transmitter: config.location.tx});
assert.equal(6144000 / 2400000, 2.56, 'native tracker delay gate scale');
assert.deepEqual({guard: atsc.process.detection.nGuard,
  train: atsc.process.detection.nTrain, centroid: atsc.process.detection.nCentroid},
{guard: 6, train: 16, centroid: 16});

// The live 6.144 MS/s values are the rounded projection of the historical
// 2.4 MS/s geometry. Re-basing the saved endpoints preserves every effective
// lane boundary, while intentionally restoring the wider physical detector
// neighborhoods and tracker gate defined at the 2.4 MS/s reference.
const liveBase = clone(config);
liveBase.process.delay_reference_sample_rate_hz = 6144000;
liveBase.process.ambiguity.delayMin = -26;
liveBase.process.ambiguity.delayMax = 1280;
liveBase.process.clutter.followAmbiguity = true;
liveBase.process.detection.minDelay = 13;
const migratedBase = clone(liveBase);
migratedBase.process.delay_reference_sample_rate_hz = 2400000;
migratedBase.process.ambiguity.delayMin = -10;
migratedBase.process.ambiguity.delayMax = 500;
migratedBase.process.detection.minDelay = 5;
const liveLane = laneConfig(liveBase, {...atsc.process.multi_illuminator?.illuminators?.[0],
  id: 'live', enabled: true, standard: 'ATSC', frequency_hz: 527000000,
  occupied_bandwidth_hz: 5380000, output_sample_rate_hz: 6144000,
  transmitter: config.location.tx});
const migratedLane = laneConfig(migratedBase, {...liveLane.process.multi_illuminator?.illuminators?.[0],
  id: 'migrated', enabled: true, standard: 'ATSC', frequency_hz: 527000000,
  occupied_bandwidth_hz: 5380000, output_sample_rate_hz: 6144000,
  transmitter: config.location.tx});
for (const field of ['delayMin', 'delayMax'])
  assert.equal(migratedLane.process.ambiguity[field], liveLane.process.ambiguity[field]);
assert.equal(migratedLane.process.detection.minDelay, liveLane.process.detection.minDelay);
assert.deepEqual({guard: liveLane.process.detection.nGuard,
  train: liveLane.process.detection.nTrain, centroid: liveLane.process.detection.nCentroid},
{guard: 2, train: 6, centroid: 6});
assert.deepEqual({guard: migratedLane.process.detection.nGuard,
  train: migratedLane.process.detection.nTrain, centroid: migratedLane.process.detection.nCentroid},
{guard: 6, train: 16, centroid: 16});
const liveLimits = limits(liveLane), migratedLimits = limits(migratedLane);
for (const field of ['delayMinKm', 'delayMaxKm', 'detectionMinDelayKm', 'kmPerBin'])
  assert.equal(migratedLimits[field], liveLimits[field]);
assert.deepEqual(associationGates(migratedLimits, {timestamp: 100, delay_rate_km_s: .1,
  doppler_rate_hz_s: 2}, 100000, 'tracker'),
associationGates(liveLimits, {timestamp: 100, delay_rate_km_s: .1,
  doppler_rate_hz_s: 2}, 100000, 'tracker'),
'ADS-B association remains in lane kilometres/Hz and is unchanged by the reference re-base');
assert.throws(() => scaleDelayBin(2147483647, 4294967295, 1, true), /exceeds/);
assert.throws(() => scaleDelayBin(1, 1, 0, true), /valid integers/);
assert.equal(config.process.ambiguity.delayMax, 400, 'projection leaves saved settings unchanged');
const singleHighRate = clone(config);
singleHighRate.process.multi_illuminator.enabled = false;
singleHighRate.capture.fs = 6144000;
singleHighRate.process.delay_reference_sample_rate_hz = 2000000;
singleHighRate.process.detection.minDelay = 80;
singleHighRate.process.detection.nTrain = 1;
assert.equal(validateConfig(singleHighRate).valid, true,
  validateConfig(singleHighRate).errors.join('; '));
singleHighRate.process.detection.nTrain = 50;
assert.equal(validateConfig(singleHighRate).valid, false,
  'single-stream validation rejects a projected CFAR radius above the native limit');
const changedScale = clone(config); changedScale.process.delay_reference_sample_rate_hz = 6144000;
assert.notEqual(siteKey(changedScale), siteKey(config), 'physical search changes reset the statistics identity');

const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-delay-reference-'));
try {
  const filename = path.join(directory, 'config.yml');
  for (const rate of [2400000, 6144000]) {
    const legacy = clone(config); legacy.capture.fs = rate;
    legacy.process.multi_illuminator.enabled = false;
    delete legacy.process.delay_reference_sample_rate_hz;
    fs.writeFileSync(filename, yaml.dump(legacy));
    assert.equal(readConfig(filename).config.process.delay_reference_sample_rate_hz, undefined);
    assert.equal(effectiveDelayConfig(legacy, rate), legacy);
    assert.equal(migrateKnownDefaults(filename), true);
    const migrated = readConfig(filename).config;
    assert.equal(migrated.process.delay_reference_sample_rate_hz, rate);
    assert.equal(effectiveDelayConfig(migrated, rate).process.ambiguity.delayMax, 400);
    assert.equal(migrateKnownDefaults(filename), false);
    assert.equal(yaml.load(fs.readFileSync(`${filename}.bak`, 'utf8')).process.delay_reference_sample_rate_hz, undefined);
  }
  const legacy = clone(config); delete legacy.process.delay_reference_sample_rate_hz;
  legacy.process.multi_illuminator.illuminators = [
    {id: 'a', enabled: true, output_sample_rate_hz: 300000},
    {id: 'b', enabled: true, output_sample_rate_hz: 6144000}];
  assert.equal(legacyDelayReferenceRate(legacy), null,
    'unlike legacy lane distances cannot be preserved by one inferred reference');
  fs.writeFileSync(filename, yaml.dump(legacy));
  assert.equal(migrateKnownDefaults(filename), false);
  legacy.process.multi_illuminator.illuminators[1].output_sample_rate_hz = 300000;
  assert.equal(legacyDelayReferenceRate(legacy), 300000);
  assert.equal(setupDefaults().process.delay_reference_sample_rate_hz, 2000000);
  assert.equal(configurationForReceiver('Kraken').process.delay_reference_sample_rate_hz, 2400000);
  assert.equal(configurationForReceiver('Usrp').process.delay_reference_sample_rate_hz, 2000000);
  const switched = configurationForReceiver('Usrp', configurationForReceiver('Kraken'));
  assert.equal(switched.process.delay_reference_sample_rate_hz, 2400000,
    'switching receivers retains the existing physical delay scale');

  const mixed = clone(config); mixed.capture.fs = 153600000;
  mixed.process.data.cpi = .02;
  mixed.process.multi_illuminator.illuminators = [2400000, 6144000, 300000].map((rate, index) => ({
    id: `lane-${index}`, enabled: true, standard: index === 2 ? 'FM' : 'ATSC',
    frequency_hz: 527000000 + index * 10000000,
    occupied_bandwidth_hz: rate - 100000, output_sample_rate_hz: rate,
    filter_taps_per_phase: 24, transmitter: config.location.tx}));
  assert.equal(validateConfig(mixed).valid, true, validateConfig(mixed).errors.join('; '));
  const invalid = clone(mixed); invalid.process.ambiguity.delayMax = 1000;
  assert.equal(validateConfig(invalid).valid, false, 'reject invalid effective lane correlation geometry');
  const description = delaySampleDescription(mixed);
  assert.match(description, /limits scale automatically/);
  assert.match(description, /lane-1: -512–1024 bins/);
} finally { fs.rmSync(directory, {recursive: true, force: true}); }
console.log('Reference-rate delay scaling, physical windows, mixed lanes, UI, defaults and safe migration passed.');
