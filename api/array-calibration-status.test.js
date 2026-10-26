'use strict';

const assert = require('assert');
const {buildArrayCalibrationStatus} = require('./array-calibration-status');

const value = buildArrayCalibrationStatus({
  config: {capture: {fc: 527000000}, process: {
    bearing: {manifold_file: '/bearing.json', manifold_automation: {auto_promote: true}},
    surveillance_fusion: {manifold_file: '/detection.json'}
  }},
  recorderStatus: {enabled: true, automatic: false, active: true,
    path: '/corpus.jsonl', samples: 15, referencePath: '/corpus.reference.jsonl',
    referenceSamples: 699},
  bearingStatus: {enabled: true, state: 'collecting', qualified: true,
    candidatePath: '/bearing.candidate.json',
    qualifiedPath: '/bearing.qualified.json', progress: {independent_passes: 3}},
  detectionStatus: {enabled: true, state: 'collecting', candidatePath: '/detection.candidate.json',
    runtimeCandidatePath: '/detection.runtime.json', validationPath: '/validation.jsonl',
    nativeShadow: {path: '/native.jsonl', added: 3, ignored: 1, error: 'rejected native signature'},
    nativeFit: {accepted_samples: 9, supported_cells: 2, training_passes: 4, holdout_passes: 1},
    promotion_allowed: true, progress: {independent_passes: 3}},
  frequencyResolvedStatus: {enabled: true}, generatedAt: 123
});

assert.equal(value.schema, 'vectorwarp-array-calibration-status-v2');
assert.equal(value.relationship.shared_evidence_corpus, true);
assert.equal(value.relationship.shared_fitted_model, false);
assert.equal(value.corpus.path, '/corpus.jsonl');
assert.equal(value.corpus.accepted_samples_this_process, 15);
assert.equal(value.reference.samples, 699);
assert.equal(value.reference.mixed_with_aircraft_responses, false);
assert.equal(value.bearing.active_model, '/bearing.json');
assert.equal(value.bearing.automation.auto_promote_enabled, true);
assert.equal(value.bearing.automation.promotion_allowed, true);
assert.equal(value.detection.active_model, '/detection.json');
assert.equal(value.detection.automation.runtime_candidate_path, '/detection.runtime.json');
assert.equal(value.detection.automation.promotion_allowed, false,
  'detection-manifold production promotion must remain hard-disabled');
assert.equal(value.detection.automation.native_shadow_corpus.error, 'rejected native signature');
assert.equal(value.detection.automation.native_shadow_fit.supported_cells, 2);
assert.deepEqual(value.bearing.progress, {independent_passes: 3});
assert.deepEqual(value.detection.progress, {independent_passes: 3});

console.log('array calibration status tests passed');
