'use strict';

const assert = require('assert');
const fs = require('fs');
const crypto = require('crypto');
const os = require('os');
const path = require('path');
const {validateDetectionSample, validateDetectionReplay, validateProductionReplay,
  productionCfarAlpha,
  buildDetectionCorpus} = require('./detection-manifold-corpus');
const {BANK_HASH_CONTRACT, detectionBankHash, fitDetectionManifold,
  denseRuntimeCandidate, cfarThreshold} = require('./detection-manifold-fitter');
const {evaluateDetectionManifoldCandidate, summarizeDetectionManifoldProgress,
  compareProductionReplay} = require('../script/detection-manifold-evaluator');
const {compatibilityFingerprint} = require('./manifold-identity');

const polar = phase => [Math.cos(phase), Math.sin(phase)];
const canonical = value => Array.isArray(value) ? `[${value.map(canonical).join(',')}]` :
  value && typeof value === 'object' ? `{${Object.keys(value).sort().map(key =>
    `${JSON.stringify(key)}:${canonical(value[key])}`).join(',')}}` : JSON.stringify(value);
const canonicalHash = value => crypto.createHash('sha256').update(canonical(value)).digest('hex');
const covariance = Array.from({length: 3}, (_, row) => Array.from({length: 3}, (_, column) =>
  [row === column ? .05 : 0, 0]));
const samples = [];
const manifoldIdentity = {frequency_hz: 539000000, sample_rate_hz: 2400000,
  channel_mapping: [0, 1, 2], requested_gain_db: [12, 12, 12],
  actual_gain_db: [12, 12, 12], positions_m: [[0, 0, 0], [.1, 0, 0], [0, .1, 0]],
  receiver_location: [40, -80, 300], transmitter_location: [40.1, -79.9, 400],
  x_axis_bearing_deg_true: 80};
manifoldIdentity.compatibility_fingerprint = compatibilityFingerprint(manifoldIdentity);
for (let sector = 0; sector < 8; ++sector) for (let pass = 0; pass < 20; ++pass) {
  const phase = sector * Math.PI / 4, common = pass * .11;
  const response = [polar(common), polar(common + .8 * Math.cos(phase)),
    polar(common + .8 * Math.sin(phase))];
  const nuisance = Array.from({length: 5}, () => [0, 1, 2].map(() => [0, 0]));
  samples.push({schema: 'vectorwarp-manifold-sample-v2',
    precision_contract: 'binary64-json-roundtrip-v1', frequency_hz: 539000000,
    sample_rate_hz: 2400000, channels: 3, config_fingerprint: 'detection-site',
    manifold_identity: manifoldIdentity,
    receiver_gain_v2: {requested_common_gain: 12, requested_channel_gains: null,
      actual_channel_gains_db: [12, 12, 12]},
    icao: `d${sector.toString(16)}${pass.toString(16).padStart(2, '0')}`,
    radar_timestamp_ms: (sector * 100 + pass) * 1000000,
    direct_path_local_azimuth_deg: 0,
    truth: {local_azimuth_deg: sector * 45 + 1, elevation_deg: 5,
      position_timestamp_s: (sector * 100 + pass) * 1000},
    match: {delay_error_km: 0, doppler_error_hz: 0},
    detection: {snr_db: 12}, response, reference_weights: response,
    configuration: {frequency_hz: 539000000, sample_rate_hz: 2400000,
      config_fingerprint: 'detection-site', receiver_gain_v2: {
        requested_common_gain: 12, requested_channel_gains: null,
        actual_channel_gains_db: [12, 12, 12]}},
    local_context: {covariance, covariance_samples: 32, detection: {same_iq: true,
      target: {baseline_statistic: 1, baseline_detected: false},
      nuisance: nuisance.map(item => ({response: item,
        baseline_statistic: .1, baseline_detected: false})),
      cfar: {pfa: .1, training_cells: 16, effective_trials: 1,
        baseline_threshold: cfarThreshold({pfa: .1, training_cells: 16, effective_trials: 1})}}},
    support: {adsb_label_only: true, covariance_samples: 32,
      local_nuisance_cells: nuisance.length},
    uncertainty: {truth_age_s: 0, delay_error_km: 0, doppler_error_hz: 0,
      normalized_match_error: 0}});
}

const positivePayload = samples.map(JSON.stringify).join('\n') + '\n';
const corpusSha256 = crypto.createHash('sha256').update(positivePayload).digest('hex');
const replay = samples.map((row, index) => ({...row,
  schema: 'vectorwarp-detection-manifold-replay-v1',
  validation: {pass_id: `independent-replay-${index}`, baseline_outcome: 'miss'},
  provenance: {source: 'independent-iq-replay', corpus_id: 'frozen-replay-corpus',
    independent_from_positive_corpus: true, replay_id: `replay-${index}`,
    corpus_sha256: corpusSha256,
    iq_sha256: index.toString(16).padStart(64, '0')}}));
const evidence = [...samples, ...replay];

assert.doesNotThrow(() => validateDetectionSample(samples[0]));
const wrongActualGain = JSON.parse(JSON.stringify(samples[0]));
wrongActualGain.configuration.receiver_gain_v2.actual_channel_gains_db[0] += 1;
assert.throws(() => validateDetectionSample(wrongActualGain), /gain|sample/i,
  'the recorded hardware-gain vector must match the installed manifold identity');
assert.doesNotThrow(() => validateDetectionReplay(replay[0]));
assert.throws(() => validateDetectionReplay({...replay[0], provenance: {...replay[0].provenance,
  iq_sha256: 'not-a-sha'}}), /provenance/);
const fitOnly = {...samples[0], local_context: {},
  support: {...samples[0].support, local_nuisance_cells: 0}};
assert.doesNotThrow(() => validateDetectionSample(fitOnly),
  'positive-control fitting must accept the existing live v2 producer without replay context');
assert.throws(() => validateDetectionReplay({...replay[0], local_context: {}}),
  /same-IQ|Detection replay/);
const missingCovariance = JSON.parse(JSON.stringify(replay[0]));
delete missingCovariance.local_context.covariance;
delete missingCovariance.local_context.covariance_samples;
delete missingCovariance.support.covariance_samples;
assert.throws(() => validateDetectionReplay(missingCovariance), /covariance-backed/,
  'a detection context must not pass with mutually absent covariance metadata');
const corpus = buildDetectionCorpus(evidence, {corpusSha256});
assert.equal(corpus.accepted.length, samples.length);
assert.equal(corpus.validation.length, replay.length);
assert.equal(corpus.miss_passes, replay.length);
assert.equal(corpus.labels, 'adsb-collection-and-validation-only');
assert.throws(() => buildDetectionCorpus(evidence), /exact positive corpus SHA-256/);
const wrongCorpus = evidence.map(row => row.schema === 'vectorwarp-detection-manifold-replay-v1' ?
  {...row, provenance: {...row.provenance, corpus_sha256: 'f'.repeat(64)}} : row);
assert.throws(() => buildDetectionCorpus(wrongCorpus, {corpusSha256}), /exact positive corpus snapshot/);
const mixedCorpus = [...evidence, {...replay[0], provenance: {...replay[0].provenance,
  replay_id: 'mixed-hash', corpus_sha256: 'f'.repeat(64)}}];
assert.throws(() => buildDetectionCorpus(mixedCorpus, {corpusSha256}), /exact positive corpus snapshot/);
const overlappingPass = {...replay[0], validation: {...replay[0].validation,
  pass_id: `${samples[0].icao}:1`}};
assert.throws(() => buildDetectionCorpus([...samples, overlappingPass], {corpusSha256}),
  /pass IDs must be disjoint/);
const conflictingOutcome = {...replay[0],
  validation: {...replay[0].validation, baseline_outcome: 'detected'},
  local_context: {...replay[0].local_context, detection: {...replay[0].local_context.detection,
    target: {baseline_statistic: 20, baseline_detected: true}}}};
assert.throws(() => buildDetectionCorpus([...evidence, conflictingOutcome], {corpusSha256}),
  /conflicting baseline outcomes/);
const duplicatedIq = {...replay[1], provenance: {...replay[1].provenance,
  iq_sha256: replay[0].provenance.iq_sha256}};
assert.throws(() => buildDetectionCorpus([...samples, replay[0], duplicatedIq], {corpusSha256}),
  /One IQ capture/);
const model = fitDetectionManifold(evidence, {azimuthBins: 8, elevationBins: 1, corpusSha256});
assert.equal(model.schema, 'vectorwarp-detection-manifold-candidate-v1');
assert.equal(model.runtime_compatible, false);
assert.equal(model.intended_use, 'offline-shadow-validation-only');
assert.equal(model.calibration.positive_corpus_sha256, corpusSha256);
assert.equal(model.responses.length, 8);
assert.equal(model.calibration.adsb_role, 'adsb-collection-and-validation-only');
const runtimeCandidate = denseRuntimeCandidate(model);
assert.equal(runtimeCandidate.runtime_compatible, true);
assert.equal(runtimeCandidate.responses.length, 8);
assert.equal(runtimeCandidate.bank_hash_contract, BANK_HASH_CONTRACT);
assert.equal(runtimeCandidate.bank_sha256, detectionBankHash(runtimeCandidate.responses,
  runtimeCandidate.direct_anchor.response));
assert.throws(() => denseRuntimeCandidate({...model, elevation_bins: 2}),
  /not exactly convertible/);
assert.throws(() => denseRuntimeCandidate({...model, responses: model.responses.slice(1)}),
  /every azimuth bin/);
const candidateModelSha256 = crypto.createHash('sha256').update(
  JSON.stringify(runtimeCandidate)).digest('hex');
const productionAlpha = productionCfarAlpha(.05, 16, 1);
const productionGain = {requested_common_gain: 12, requested_channel_gains: null,
  actual_channel_gains_db: [12, 12, 12]};
const baselineDescriptor = {frequency_hz: 539000000, beams: 8,
  positions_m: [[0, 0, 0], [.1, 0, 0], [0, .1, 0]],
  direct_path_azimuth_rad: 0, direct_path_null_db: 0};
const baselineBankSha256 = canonicalHash(baselineDescriptor);
const productionReceipt = {schema: 'vectorwarp-detection-manifold-production-replay-v1',
  pass: {id: 'production-pass-1', label: 'positive', truth: {
    selection: 'target-cut-only-no-beam-selection', target_cut_id: 'target',
    local_azimuth_deg: 45, elevation_deg: 5}},
  provenance: {source: 'vectorwarp-native-raw-iq-full-bank-replay', raw_iq_consumed: true,
    independent_from_positive_corpus: true, corpus_sha256: corpusSha256,
    iq_sha256: '1'.repeat(64), replay_id: '2'.repeat(64), input_grid_sha256: '3'.repeat(64),
    generator_sha256: '4'.repeat(64), config_sha256: '7'.repeat(64),
    generator_trust_policy_sha256: canonicalHash([]),
    native_export_sha256: '3'.repeat(64), receipt_generator_sha256: '9'.repeat(64),
    frequency_sha256: canonicalHash(539000000), sample_rate_sha256: canonicalHash(2400000),
    channel_layout_sha256: canonicalHash([0, 1, 2]),
    gain_sha256: canonicalHash(productionGain), candidate_model_sha256: candidateModelSha256},
  configuration: {config_fingerprint: 'detection-site', frequency_hz: 539000000,
    sample_rate_hz: 2400000, channels: 3, channel_layout: [0, 1, 2],
    fusion_mode: 'coherent_beam_bank',
    beam_count: 8, x_axis_bearing_deg_true: 0,
    receiver_location: [40, -80, 300], transmitter_location: [40.1, -80, 400],
    positions_m: baselineDescriptor.positions_m, direct_path_azimuth_rad: 0,
    direct_path_null_db: 0,
    receiver_gain_v2: productionGain},
  detector: {contract: 'vectorwarp-production-ca-cfar-full-bank-v1',
    configured_pfa: .1, detector_pfa: .05, effective_beam_trials: 2,
    motion_trials: 1, integration_cpis: 1, guard_cells: 1,
    training_cells_per_side: 8},
  banks: {baseline: {sha256: baselineBankSha256, beams: 8, descriptor: baselineDescriptor},
    candidate: {sha256: '6'.repeat(64), beams: 8, runtime_compatible: true,
      elevation_range_deg: [0, 10]}},
  replay: {same_iq: true, complete_grid: true, truth_selected_beam: false,
    support_complete: true, cuts: [
      {id: 'target', role: 'target', input_grid_sha256: '3'.repeat(64), training_cells: 16,
        baseline: {bank_sha256: baselineBankSha256, beams_evaluated: 8,
          cut_power: productionAlpha * 2, training_mean_power: 1, alpha: productionAlpha,
          threshold: productionAlpha, detected: true},
        candidate: {bank_sha256: '6'.repeat(64), beams_evaluated: 8,
          cut_power: productionAlpha * 3, training_mean_power: 1, alpha: productionAlpha,
          threshold: productionAlpha, detected: true}},
      {id: 'nuisance', role: 'nuisance', input_grid_sha256: '3'.repeat(64), training_cells: 16,
        baseline: {bank_sha256: baselineBankSha256, beams_evaluated: 8,
          cut_power: productionAlpha / 2, training_mean_power: 1, alpha: productionAlpha,
          threshold: productionAlpha, detected: false},
        candidate: {bank_sha256: '6'.repeat(64), beams_evaluated: 8,
          cut_power: productionAlpha / 2, training_mean_power: 1, alpha: productionAlpha,
          threshold: productionAlpha, detected: false}}]}};
assert.doesNotThrow(() => validateProductionReplay(productionReceipt));
assert.throws(() => validateProductionReplay({...productionReceipt, replay: {
  ...productionReceipt.replay, truth_selected_beam: true}}), /production full-bank replay contract/);
assert.throws(() => validateProductionReplay({...productionReceipt, replay: {
  ...productionReceipt.replay, cuts: productionReceipt.replay.cuts.map((cut, index) => index ? cut :
    {...cut, candidate: {...cut.candidate, beams_evaluated: 1}})}}), /full beam bank/);
assert.equal(buildDetectionCorpus([...samples, productionReceipt], {corpusSha256})
  .production_validation.length, 1);
assert.ok(cfarThreshold({pfa: .1, training_cells: 16, effective_trials: 4}) >
  cfarThreshold({pfa: .1, training_cells: 16, effective_trials: 1}),
  'effective trial accounting must tighten the candidate threshold');

const config = {capture: {fc: 539000000}, process: {bearing: {manifold_recorder: {}},
  surveillance_fusion: {beams: 8, effective_beam_trials: 2}}};
const relaxed = {azimuthBins: 8, elevationBins: 1, gates: {minIndependentPasses: 20,
  minValidationPasses: 8, minMissPasses: 6, minCoveredSectors: 6,
  minValidationSectors: 4, minFitBins: 6,
  maxUnsupportedGapDeg: 90, minTargetMarginImprovementDb: .5,
  maximumNuisanceRatio: 1.2, minValidationSupportFraction: .8,
  minCfarTrainingCells: 8, minHoldoutNuisanceCells: 8},
  corpusSha256};
const positivesOnly = summarizeDetectionManifoldProgress(samples, config, relaxed);
assert.equal(positivesOnly.positive_control.fit_ready, true);
assert.equal(positivesOnly.validation.ready, false);
assert.equal(positivesOnly.collection_ready, false);
assert.equal(positivesOnly.validation_passes, 0);
assert.equal(positivesOnly.miss_passes, 0);
assert.equal(positivesOnly.holdout_nuisance_cells, 0,
  'detected training rows cannot manufacture independent validation or false-alarm controls');
assert.equal(positivesOnly.fit_bins, 8);
assert.equal(positivesOnly.runtime_support.complete, true);
assert.deepEqual(positivesOnly.runtime_support.elevation_range_deg, [5, 5]);
const positivesOnlyEvaluation = evaluateDetectionManifoldCandidate(samples, config, relaxed);
assert.equal(positivesOnlyEvaluation.evaluation.qualified, false,
  'detected positive controls alone must never qualify a candidate');
assert.equal(positivesOnlyEvaluation.evaluation.diagnostic_only, true);
assert.ok(positivesOnlyEvaluation.evaluation.diagnostic_failures.some(value =>
  /diagnostic replay passes/.test(value)));
const progress = summarizeDetectionManifoldProgress(evidence, config, relaxed);
assert.equal(progress.collection_ready, true, JSON.stringify(progress.next_actions));
assert.equal(progress.positive_control.fit_ready, true);
assert.equal(progress.validation.ready, true);
const evaluated = evaluateDetectionManifoldCandidate(evidence, config, relaxed);
assert.equal(evaluated.evaluation.qualified, false);
assert.equal(evaluated.evaluation.qualification_basis,
  'none-awaiting-native-full-bank-grid-export');
assert.ok(evaluated.evaluation.failures.some(value => /dense bank is replay-only/.test(value)));
assert.equal(evaluated.evaluation.promotion_allowed, false);
assert.ok(evaluated.evaluation.comparison.candidate_recall >=
  evaluated.evaluation.comparison.baseline_recall);
assert.ok(evaluated.evaluation.comparison.target_margin_improvement_db >= .5);
assert.ok(evaluated.evaluation.comparison.nuisance_ratio <= 1.2);
assert.ok(evaluated.evaluation.comparison.validation_support_fraction >= .8);
assert.equal(evaluated.evaluation.comparison.cfar_accounting.method,
  'ca-cfar-familywise-pfa-effective-trials-v1');
const strictImported = evaluateDetectionManifoldCandidate([...evidence, productionReceipt],
  config, relaxed);
assert.equal(strictImported.evaluation.production_replay.matching_candidate_passes, 1);
assert.equal(strictImported.evaluation.production_replay.generator_trusted, false);
assert.ok(strictImported.evaluation.production_failures.some(value => /per-sector pass support/.test(value)));
assert.equal(strictImported.evaluation.qualified, false,
  'strict receipts remain non-qualifying until their native producer is implemented and trusted');
const trustedConfig = JSON.parse(JSON.stringify(config));
trustedConfig.process.surveillance_fusion.detection_manifold_replay = {
  trusted_native_generator_sha256: [productionReceipt.provenance.generator_sha256]};
const trustedReceipt = {...productionReceipt, provenance: {...productionReceipt.provenance,
  generator_trust_policy_sha256: canonicalHash(
    trustedConfig.process.surveillance_fusion.detection_manifold_replay
      .trusted_native_generator_sha256)}};
const trustedImported = evaluateDetectionManifoldCandidate([...evidence, trustedReceipt],
  trustedConfig, relaxed);
assert.equal(trustedImported.evaluation.production_replay.generator_trusted, false,
  'receipts remain untrusted until the authenticated raw-IQ exporter exists');
assert.equal(trustedImported.evaluation.qualified, false,
  'a trusted receipt cannot make the sparse offline fitter runtime-compatible');
assert.throws(() => validateProductionReplay({...productionReceipt,
  pass: {...productionReceipt.pass, label: 'miss'}}), /label disagrees/,
  'a receipt must not relabel a baseline detection as a miss');

const regressing = evidence.map(row => ({...row, local_context: {...row.local_context,
  covariance: covariance.map(line => line.map(value => [value[0] * 1000, value[1]])),
  detection: {...row.local_context.detection,
    target: {baseline_statistic: 20, baseline_detected: true}}},
  ...(row.schema === 'vectorwarp-detection-manifold-replay-v1' ?
    {validation: {...row.validation, baseline_outcome: 'detected'}} : {})}));
const rejected = evaluateDetectionManifoldCandidate(regressing, config, relaxed);
assert.equal(rejected.evaluation.qualified, false);
assert.ok(rejected.evaluation.diagnostic_failures.some(value => /recall regressed/.test(value)));

const noisy = evidence.map(row => ({...row, local_context: {...row.local_context,
  detection: {...row.local_context.detection, nuisance: row.local_context.detection.nuisance.map(item =>
    ({...item, response: row.response}))}}}));
const nuisanceRejected = evaluateDetectionManifoldCandidate(noisy, config, relaxed);
assert.equal(nuisanceRejected.evaluation.qualified, false);
assert.ok(nuisanceRejected.evaluation.diagnostic_failures.some(value => /nuisance detections/.test(value)));

const wrongTrials = evidence.map(row => ({...row, local_context: {...row.local_context,
  detection: {...row.local_context.detection, cfar: {...row.local_context.detection.cfar,
    baseline_threshold: row.local_context.detection.cfar.baseline_threshold * 2}}}}));
const trialRejected = evaluateDetectionManifoldCandidate(wrongTrials, config, relaxed);
assert.equal(trialRejected.evaluation.qualified, false);
assert.ok(trialRejected.evaluation.diagnostic_failures.some(value => /effective trial accounting/.test(value)));

const sparseProgress = summarizeDetectionManifoldProgress(evidence.filter(row =>
  row.truth.local_azimuth_deg < 90), config, relaxed);
assert.equal(sparseProgress.collection_ready, false);
assert.ok(sparseProgress.largest_unsupported_gap_deg > relaxed.gates.maxUnsupportedGapDeg);
const unsupportedReplay = replay.map((row, index) => index < replay.length / 2 ? row :
  {...row, truth: {...row.truth, local_azimuth_deg: row.truth.local_azimuth_deg + 22.5}});
const unsupportedEvaluation = evaluateDetectionManifoldCandidate([...samples, ...unsupportedReplay],
  {...config, process: {...config.process, surveillance_fusion: {
    ...config.process.surveillance_fusion, beams: 16}}}, {...relaxed, azimuthBins: 16});
assert.equal(unsupportedEvaluation.evaluation.qualified, false);
assert.ok(unsupportedEvaluation.evaluation.diagnostic_failures.some(value => /support covers/.test(value)));

const twelveBeamConfig = JSON.parse(JSON.stringify(config));
twelveBeamConfig.process.surveillance_fusion.beams = 12;
const twelveBeamOptions = {...relaxed}; delete twelveBeamOptions.azimuthBins;
const twelveBeam = evaluateDetectionManifoldCandidate(samples, twelveBeamConfig, twelveBeamOptions);
assert.equal(twelveBeam.manifold.azimuth_bins, 12,
  'automation must fit the configured production bank without an independent eight-beam default');
assert.equal(twelveBeam.runtimeCandidate, null, 'eight supported directions cannot fill a twelve-beam bank');
assert.equal(twelveBeam.progress.runtime_support.complete, false);
assert.equal(twelveBeam.progress.fit_bins, twelveBeam.manifold.responses.length);
const twelveBeamSamples = samples.map((row, index) => ({...row,
  truth: {...row.truth, local_azimuth_deg: index % 12 * 30 + 1}}));
const twelveDense = evaluateDetectionManifoldCandidate(twelveBeamSamples,
  twelveBeamConfig, twelveBeamOptions);
assert.equal(twelveDense.runtimeCandidate.beams, 12);
assert.equal(twelveDense.runtimeCandidate.responses.length, 12);
const defaultBeamConfig = JSON.parse(JSON.stringify(twelveBeamConfig));
delete defaultBeamConfig.process.surveillance_fusion.beams;
assert.equal(evaluateDetectionManifoldCandidate(twelveBeamSamples, defaultBeamConfig,
  twelveBeamOptions).runtimeCandidate.beams, 12, 'the optional setting uses the production default');
assert.throws(() => evaluateDetectionManifoldCandidate(samples, twelveBeamConfig, relaxed),
  /configured production beam count/, 'legacy conflicting fit options require regeneration');
const wrongTopology = compareProductionReplay([validateProductionReplay(productionReceipt)],
  candidateModelSha256, twelveBeamConfig);
assert.equal(wrongTopology.matching_candidate_passes, 0);
assert.equal(wrongTopology.beam_topology_mismatches, 1);
assert.equal(wrongTopology.candidate_hash_mismatches, 0);
const reducedTrials = compareProductionReplay([validateProductionReplay(productionReceipt)],
  candidateModelSha256, {...config, process: {...config.process, surveillance_fusion: {
    ...config.process.surveillance_fusion, effective_beam_trials: 8}}});
assert.equal(reducedTrials.matching_candidate_passes, 0);
assert.equal(reducedTrials.beam_trial_mismatches, 1,
  'matching steering weights cannot qualify receipts scored with a smaller trial penalty');
const variedElevation = samples.map((row, index) => ({...row,
  truth: {...row.truth, elevation_deg: 1 + index % 3 * 6}}));
const variedProgress = summarizeDetectionManifoldProgress(variedElevation, config, relaxed);
assert.equal(variedProgress.fit_bins, 8,
  'progress counts the actual fitter bins, not unrelated five-degree elevation bins');
assert.deepEqual(variedProgress.runtime_support.elevation_range_deg, [1, 13]);

const {spawnSync} = require('child_process');
const cli = path.join(__dirname, '../script/fit-detection-manifold.js');
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'detection-manifold-offline-'));
try {
  const samplePath = path.join(directory, 'samples.jsonl');
  const replayPath = path.join(directory, 'independent-replay.jsonl');
  const configPath = path.join(directory, 'config.json');
  const candidatePath = path.join(directory, 'candidate.json');
  const runtimePath = path.join(directory, 'dense-bank.json');
  fs.writeFileSync(samplePath, samples.map(JSON.stringify).join('\n') + '\n');
  fs.writeFileSync(replayPath, replay.map(JSON.stringify).join('\n') + '\n');
  const configBytes = JSON.stringify(config);
  fs.writeFileSync(configPath, configBytes);
  const args = ['--input', samplePath, '--validation', replayPath, '--config', configPath,
    '--output', candidatePath, '--runtime-output', runtimePath];
  const refused = spawnSync(process.execPath, [cli, ...args], {encoding: 'utf8'});
  assert.notEqual(refused.status, 0, 'offline execution requires explicit opt-in');
  assert.equal(fs.existsSync(candidatePath), false);
  const fitted = spawnSync(process.execPath, [cli, '--enable-offline-experiment', ...args],
    {encoding: 'utf8', timeout: 35000});
  assert.equal(fitted.status, 0, fitted.stderr);
  const status = JSON.parse(fitted.stdout);
  assert.equal(status.qualified, false);
  assert.equal(JSON.parse(fs.readFileSync(runtimePath)).beams, 8);
  assert.equal(JSON.parse(fs.readFileSync(candidatePath)).schema, 'vectorwarp-detection-manifold-candidate-v1');
  assert.equal(fs.readFileSync(configPath, 'utf8'), configBytes, 'offline fitting cannot edit configuration');
  const original = fs.readFileSync(runtimePath, 'utf8');
  const repeated = spawnSync(process.execPath, [cli, '--enable-offline-experiment', ...args],
    {encoding: 'utf8', timeout: 35000});
  assert.notEqual(repeated.status, 0);
  assert.match(repeated.stderr, /already exists/);
  assert.equal(fs.readFileSync(runtimePath, 'utf8'), original, 'existing bank bytes must be preserved');
  const sparseConfig = {...config, process: {...config.process,
    surveillance_fusion: {...config.process.surveillance_fusion, beams: 12}}};
  fs.writeFileSync(configPath, JSON.stringify(sparseConfig));
  const sparseArgs = ['--enable-offline-experiment', '--input', samplePath, '--config', configPath,
    '--output', path.join(directory, 'sparse.json'), '--runtime-output', path.join(directory, 'sparse-bank.json')];
  const sparse = spawnSync(process.execPath, [cli, ...sparseArgs], {encoding: 'utf8', timeout: 35000});
  assert.notEqual(sparse.status, 0);
  assert.match(sparse.stderr, /Incomplete beam\/elevation support/);
  assert.equal(fs.existsSync(path.join(directory, 'sparse.json')), false,
    'a rejected dense fit publishes no candidate');
  assert.equal(fs.existsSync(path.join(directory, 'sparse-bank.json')), false);
  console.log('Detection manifold: offline bounded fitting, full-bank import, identity and refusal checks passed.');
} finally { fs.rmSync(directory, {recursive: true}); }
