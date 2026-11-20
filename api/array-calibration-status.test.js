'use strict';

const assert = require('assert');
const {buildArrayCalibrationStatus} = require('./array-calibration-status');

const value = buildArrayCalibrationStatus({
  config: {capture: {fc: 527000000}, process: {
    bearing: {enable: true, manifold_file: '/bearing.json'},
    detection: {enable: true},
    surveillance_fusion: {manifold_file: '/detection.json'}
  }},
  recorderStatus: {enabled: true, automatic: false, active: true,
    path: '/corpus.jsonl', samples: 15, referencePath: '/corpus.reference.jsonl',
    referenceSamples: 699, cohortRoot: '/corpus.jsonl.cohorts',
    physicalSiteFingerprint: 'physical', acquisitionFingerprint: 'acquisition',
    cohorts: {files: 4, sampleRows: 125, referenceRows: 200,
      physicalSites: 1, acquisitionIdentities: 2}},
  bearingStatus: {enabled: true, state: 'collecting', qualified: true,
    candidatePath: '/bearing.candidate.json',
    qualifiedPath: '/bearing.qualified.json', progress: {independent_passes: 3}},
  detectionStatus: {enabled: true, state: 'collecting', candidatePath: '/detection.candidate.json',
    runtimeCandidatePath: '/detection.runtime.json', validationPath: '/validation.jsonl',
    promotion_allowed: true, progress: {independent_passes: 3}},
  frequencyResolvedStatus: {enabled: true}, generatedAt: 123
});

assert.equal(value.schema, 'vectorwarp-array-calibration-status-v3');
assert.equal(value.corpus.path, '/corpus.jsonl');
assert.equal(value.corpus.accepted_samples_this_process, 15);
assert.equal(value.corpus.accumulated_cohorts.sampleRows, 125);
assert.equal(value.corpus.physical_site_fingerprint, 'physical');
assert.equal(value.reference.samples, 699);
assert.equal(value.reference.mixed_with_aircraft_responses, false);
assert.equal(value.bearing.active_model, '/bearing.json');
assert.equal(value.bearing.enabled, true);
assert.equal(value.detection.active_model, '/detection.json');
assert.equal(value.detection.enabled, true);
assert.deepEqual(Object.keys(value.bearing).sort(), ['active_model', 'enabled', 'purpose']);
assert.deepEqual(Object.keys(value.detection).sort(), ['active_model', 'enabled', 'purpose']);
assert.doesNotMatch(JSON.stringify(value), /automation|candidate|promotion|progress/,
  'status must not expose stale caller-supplied campaign state');
const disabled = buildArrayCalibrationStatus({config: {}, recorderStatus: {}, generatedAt: 1});
assert.equal(disabled.bearing.enabled, false);
assert.equal(disabled.bearing.active_model, null);
assert.equal(disabled.corpus.accepted_samples_this_process, 0);

console.log('array calibration status tests passed');
