'use strict';
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {EventEmitter} = require('events');
const {fitNativeShadow, nativeRows, projectionPower, split} = require('./native-shadow-manifold-fitter');
const {DetectionManifoldAutomation} = require('./detection-manifold-automation');
const {validateArtifactDestinations} = require('./detection-manifold-artifacts');
const {key, options, source, sign, ledger, queuedFixture} = require('./native-shadow-manifold-test-fixture');
const passes = Array.from({length: 100}, (_, index) => `flight-${index}`);
const training = passes.filter(pass => !split(pass)), holdout = passes.filter(split);
const inputs = [source({pass: training[0], time: 1000, label: 'detected'}), source({pass: training[1], time: 2000}),
  source({pass: holdout[0], time: 3000, label: 'miss'})];
const fitted = fitNativeShadow(ledger(inputs), options);
assert.equal(projectionPower([[1, 0], [1, 0]], [[1, 0], [-1, 0]]), 0, 'coherent cancellation must cancel');
assert.equal(projectionPower([[1, 0], [0, 1]], [[1, 0], [0, 1]]), 4, 'Hermitian complex convention');
assert.equal(fitted.model.cells.length, 48);
assert.equal(fitted.supported_cells, 1);
assert.equal(fitted.model.train_passes.length, 2);
assert.equal(fitted.evaluation.holdout_passes.length, 1);
assert.equal(fitted.evaluation.diagnostics[0].supported, true);
assert.equal(fitted.evaluation.promotion_allowed, false);
assert.equal(fitted.model.runtime_compatible, false);
assert.ok(fitted.evaluation.diagnostics[0].target_projection_change > 0);
const physical = nativeRows(ledger(inputs), options)[0];
assert.ok(Math.abs(projectionPower(physical.baseline, physical.response) - 1.12 ** 2 / (1.04 * 1.36)) < 1e-12,
  'native fusion coefficients are already conjugated, not direct-path reference or beam-power-derived responses');
const phases = inputs.map((row, index) => source({pass: row.pass.id, label: row.pass.label,
  time: row.sample_clock.timestamp_ms, phase: (index + 1) * 1.2}));
const changedPhase = fitNativeShadow(ledger(phases), options);
assert.ok(projectionPower(fitted.model.cells.find(cell => cell.response).response,
  changedPhase.model.cells.find(cell => cell.response).response) > 1 - 1e-12);
assert.ok(Math.abs(fitted.evaluation.diagnostics[0].target_projection_change -
  changedPhase.evaluation.diagnostics[0].target_projection_change) < 1e-12);
const changedHoldout = inputs.map(row => JSON.parse(JSON.stringify(row)));
changedHoldout[2].target_cut.physical_channels = [[.1, .2], [.9, -.8]]; sign(changedHoldout[2]);
assert.equal(fitNativeShadow(ledger(changedHoldout), options).model.candidate_version, fitted.model.candidate_version,
  'held-out response cannot influence a fitted candidate version');
const elevatedHoldout = [...inputs.slice(0, 2), source({pass: holdout[0], time: 3000, elevation: 6})];
assert.equal(fitNativeShadow(ledger(elevatedHoldout), options).evaluation.diagnostics[0].supported, false,
  'fixed elevation axes may not silently extrapolate observed support');
const orthogonal = [source({pass: training[0], time: 1000, response: [[0, -.2], [-1, 0]]}),
  source({pass: training[1], time: 2000, response: [[0, -.2], [-1, 0]]})];
assert.equal(fitNativeShadow(ledger(orthogonal), options).supported_cells, 0, 'orthogonal shrinkage phase is unidentifiable');
assert.equal(fitNativeShadow(ledger([inputs[0], source({pass: training[0], time: 2000})]), options).supported_cells, 0,
  'repeated rows from one pass cannot manufacture support');
for (const patch of [{epoch: 'epoch-2'}, {gain: 19}]) {
  const epochs = ledger([inputs[0], source({pass: training[1], time: 2000, ...patch}),
    source({pass: training[2], time: 3000})]);
  const result = fitNativeShadow(epochs, options);
  assert.equal(result.accepted_samples, 1); assert.equal(result.excluded_prior_epoch_samples, 2);
}
const corrupted = ledger(inputs); corrupted[0].physical_iq_cut[0][0] += 1;
const dwells = inputs.flatMap(item => {
  const other = JSON.parse(JSON.stringify(item));
  other.configuration.config_fingerprint = 'different-cpi'; other.configuration.cpi_seconds = .001;
  other.sample_clock.end_epoch_sample += 64; other.sample_clock.source_sample_midpoint += 32;
  other.epoch_id = 'different-dwell-epoch'; other.sample_clock.epoch_id = other.epoch_id;
  other.covariance.epoch_id = other.epoch_id; other.target_cut.epoch_id = other.epoch_id;
  other.source_snapshots.forEach(snapshot => { snapshot.epoch_id = other.epoch_id; });
  return [item, sign(other)];
});
const dwellFit = fitNativeShadow(ledger(dwells), {...options, cpiSeconds: .0005});
assert.equal(dwellFit.accepted_samples, 3, 'interleaved dwell variants cannot destroy same-epoch support');
assert.equal(dwellFit.excluded_other_configuration_samples, 3);
assert.equal(dwellFit.model.candidate_version, fitted.model.candidate_version);
assert.throws(() => fitNativeShadow(corrupted, options), /hash/);
assert.throws(() => fitNativeShadow(ledger(inputs).slice(1), options), /chain/);
assert.throws(() => fitNativeShadow(ledger(inputs)), /authentication/);
for (const patch of [{azimuthBins: 1000000}, {elevationBins: 1}, {minTrainingPassesPerBin: 1}])
  assert.throws(() => fitNativeShadow([], {...options, ...patch}), /bounded/);

(async () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'native-fit-regression-'));
  try {
    const nativePath = path.join(directory, 'native.jsonl'), keyPath = path.join(directory, 'key');
    fs.writeFileSync(keyPath, key, {mode: 0o600});
    const config = {process: {detection: {integration_shadow: {enabled: true, receipt_key_path: keyPath,
      queue_path: path.join(directory, 'queue'), results_path: path.join(directory, 'results')}},
      surveillance_fusion: {detection_manifold_automation: {enabled: true, native_shadow_corpus_path: nativePath}}}};
    // First import must run even before either a live or derived corpus exists.
    let dispatched = 0;
    class FirstImport extends EventEmitter { constructor() { super(); ++dispatched;
      process.nextTick(() => this.emit('message', {ok: true, result: {qualified: false}})); } }
    const trigger = new DetectionManifoldAutomation(config, {worker: FirstImport});
    await trigger.evaluate(); assert.equal(dispatched, 1);
    await trigger.evaluate(); assert.equal(dispatched, 1);
    fs.mkdirSync(config.process.detection.integration_shadow.results_path);
    fs.writeFileSync(path.join(config.process.detection.integration_shadow.results_path, 'new.result.json'), '{}');
    await trigger.evaluate(); assert.equal(dispatched, 2, 'new signed receipt paths trigger evaluation independently of live corpus');
    let raced = 0;
    class ArrivalDuringFit extends EventEmitter { constructor() { super(); ++raced;
      process.nextTick(() => {
        if (raced === 1) fs.writeFileSync(path.join(config.process.detection.integration_shadow.results_path,
          'new.result.json'), '{"arrived_during_fit":true}');
        this.emit('message', {ok: true, result: {qualified: false}});
      }); } }
    const race = new DetectionManifoldAutomation(config, {worker: ArrivalDuringFit});
    await race.evaluate(); await race.evaluate();
    assert.equal(raced, 2, 'inputs arriving during a worker run must not be cached as already consumed');
    fs.writeFileSync(nativePath, ledger(inputs).map(JSON.stringify).join('\n') + '\n');
    const automation = new DetectionManifoldAutomation(config);
    const status = await automation.evaluate();
    assert.equal(status.error, null, JSON.stringify(status));
    assert.equal(status.state, 'native-shadow-projection-only');
    assert.equal(status.nativeFit.supported_cells, 1);
    assert.equal(status.nativeShadow.rejected, 1);
    assert.equal(status.promotion_allowed, false);
    assert.equal(status.qualified, false);
    const manifest = JSON.parse(fs.readFileSync(status.candidatePath));
    assert.equal(manifest.candidate_version, fitted.model.candidate_version);
    const modelBytes = fs.readFileSync(status.versionedCandidatePath);
    const receipt = JSON.parse(fs.readFileSync(status.receiptPath, 'utf8').trim());
    assert.equal(receipt.evaluation.schema, 'vectorwarp-native-shadow-projection-ab-v1');
    assert.equal(receipt.evaluation.diagnostics[0].supported, true);
    await automation.evaluate({force: true});
    assert.ok(fs.readFileSync(status.versionedCandidatePath).equals(modelBytes));
    const protectedConfig = JSON.parse(JSON.stringify(config));
    protectedConfig.process.surveillance_fusion.manifold_file = status.candidatePath;
    assert.throws(() => validateArtifactDestinations(protectedConfig, {candidatePath: status.candidatePath}), /overlaps/);
    assert.throws(() => validateArtifactDestinations(config, {candidatePath: keyPath}), /overlaps/);
    assert.throws(() => validateArtifactDestinations(config, {candidatePath: nativePath, nativeShadowPath: nativePath}), /overlaps/);
    const alias = path.join(directory, 'alias'); fs.linkSync(keyPath, alias);
    assert.throws(() => validateArtifactDestinations(config, {candidatePath: alias}), /overlaps/);
    fs.writeFileSync(nativePath, corrupted.map(JSON.stringify).join('\n') + '\n');
    const failed = await automation.evaluate({force: true});
    assert.equal(failed.state, 'error'); assert.match(failed.error, /hash/);
    assert.ok(fs.readFileSync(status.versionedCandidatePath).equals(modelBytes));
    assert.equal(fs.readFileSync(status.receiptPath, 'utf8').trim().split('\n').length, 2,
      'invalid ledger may not produce a third candidate/A-B receipt');
    fs.writeFileSync(nativePath, JSON.stringify(ledger(inputs)[0]) + '\n');
    const truncated = await automation.evaluate({force: true});
    assert.match(truncated.error, /published authenticated prefix/,
      'valid signatures on an old prefix must not conceal a truncated evidence ledger');
    const queued = queuedFixture(inputs);
    const freshConfig = JSON.parse(JSON.stringify(config));
    const setting = freshConfig.process.surveillance_fusion.detection_manifold_automation;
    setting.native_shadow_corpus_path = path.join(directory, 'new-corpus-directory', 'first-import.jsonl');
    const queue = freshConfig.process.detection.integration_shadow;
    fs.mkdirSync(queue.queue_path);
    fs.writeFileSync(path.join(queue.queue_path, `${queued.job.job_id}.job.json`), JSON.stringify(queued.job));
    fs.writeFileSync(path.join(queue.results_path, `${queued.job.job_id}.result.json`), JSON.stringify(queued.result));
    const firstImport = await new DetectionManifoldAutomation(freshConfig).evaluate();
    assert.equal(firstImport.error, null, JSON.stringify(firstImport));
    assert.equal(firstImport.nativeShadow.added, 9);
    assert.equal(firstImport.nativeFit.supported_cells, 1);
    assert.equal(firstImport.nativeFit.holdout_passes, 1);
    assert.equal(firstImport.state, 'native-shadow-projection-only');
    assert.ok(fs.existsSync(firstImport.versionedCandidatePath));
    const archiveJobs = path.join(queue.results_path, 'archive', 'jobs');
    const archiveResults = path.join(queue.results_path, 'archive', 'results');
    fs.mkdirSync(archiveJobs, {recursive: true}); fs.mkdirSync(archiveResults, {recursive: true});
    fs.renameSync(path.join(queue.queue_path, `${queued.job.job_id}.job.json`), path.join(archiveJobs, `${queued.job.job_id}.job.json`));
    fs.renameSync(path.join(queue.results_path, `${queued.job.job_id}.result.json`), path.join(archiveResults, `${queued.job.job_id}.result.json`));
    setting.native_shadow_corpus_path = path.join(directory, 'archive-import.jsonl');
    const archivedImport = await new DetectionManifoldAutomation(freshConfig).evaluate();
    assert.equal(archivedImport.error, null, JSON.stringify(archivedImport));
    assert.equal(archivedImport.nativeShadow.added, 9, 'compaction before first import cannot silently lose native evidence');
  } finally { fs.rmSync(directory, {recursive: true, force: true}); }
  console.log('Native shadow fitter: coherent complex projection, 2-D support, pass isolation, epochs, auth, immutable A/B, native-first trigger and production path guards passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
