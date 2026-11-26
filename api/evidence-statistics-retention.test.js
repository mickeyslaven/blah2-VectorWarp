'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const {EvidenceStatistics, compactAssociationPass, federationAssociationPassTransport,
  expandFederationAssociationPassTransport} = require('./evidence-statistics');

const DAY = 86400000;
const config = {capture: {fs: 2400000, fc: 563000000},
  location: {rx: {latitude: 1, longitude: 2}, tx: {latitude: 3, longitude: 4}},
  truth: {adsb: {enabled: false}}, process: {data: {cpi: .5},
    ambiguity: {delayMin: 0, delayMax: 500, dopplerMin: -600, dopplerMax: 600},
    detection: {minDelay: 5}, tracker: {initiate: {M: 3, N: 6}}}};

function fixture() {
  let clock = 1800000000000;
  const files = new Map();
  const fileSystem = {statSync(name) {
    if (!files.has(name)) throw Object.assign(new Error('missing'), {code: 'ENOENT'});
    return {isFile: () => true, size: Buffer.byteLength(files.get(name))};
  }, readFileSync: name => files.get(name), mkdirSync() {},
  writeFileSync: (name, data) => files.set(name, data),
  renameSync: (from, to) => {files.set(to, files.get(from)); files.delete(from);}};
  const create = () => new EvidenceStatistics(config, {}, {filename: '/fixture/stats.json',
    now: () => clock, fileSystem, setIntervalFn: () => ({unref() {}}), clearIntervalFn() {}});
  return {create, files, now: () => clock, advance: ms => {clock += ms;}};
}

function finish(stats, index, at, eligible = true) {
  const hex = index.toString(16).padStart(6, '0');
  stats.state.active[hex] = {id: `pass-${index}`, hex, siteKey: stats.key,
    evaluationContractHash: stats.scoreContractHash,
    startedAt: at - 2000, lastAt: at, frames: 6,
    physicalEligibleFrames: eligible ? 6 : 0, detectorEligibleFrames: eligible ? 6 : 0,
    eligibleFrames: eligible ? 6 : 0, associationEligibleFrames: eligible ? 6 : 0,
    detectedFrames: eligible ? 3 : 0, trackedFrames: eligible ? 3 : 0,
    missingTimingFrames: 0, sensitivityEpochId: 'run-1',
    measuredCoverage: {complete: true, eligible: [[at - 2000, at]],
      scored: [[at - 2000, at]], measured: [[at - 1000, at]]},
    cpiAudit: [{timestamp_ms: at}], detectionAssociations: [{large: 'payload'}],
    slantRangeBands: {'0-10': {minimum: 0, maximum: 10,
      eligibleFrames: eligible ? 6 : 0, detectedFrames: eligible ? 3 : 0, trackedFrames: eligible ? 3 : 0}}};
  stats.finish(hex);
}

test('a 24-hour pass window survives 500 ineligible detail evictions and API reload', () => {
  const f = fixture(); const stats = f.create();
  f.advance(1000); finish(stats, 0, f.now());
  f.advance(23 * 3600000);
  for (let i = 1; i <= 800; ++i) finish(stats, i, f.now(), false);
  assert.equal(stats.state.passes.length, 500);
  assert.equal(stats.state.passHistory.length, 301);
  assert.equal(stats.state.passHistory[0].cpiAudit, undefined);
  assert.equal(stats.state.passHistory[0].detectionAssociations, undefined);
  assert.equal(stats.associationPasses({durationMs: DAY}).length, 1);
  const day = stats.summarize(DAY, f.now());
  assert.equal(day.eligible_passes, 1);
  assert.equal(day.cpi_exposure.eligible_cpis, 6);
  assert.equal(day.receiver_slant_range_strata.bands['0-10'].detected_passes, 1);
  assert.equal(stats.summarize(Infinity, f.now()).eligible_passes, 1, 'rollup must not double count summaries');
  stats.persist(); const restored = f.create();
  assert.equal(restored.associationPasses({durationMs: DAY})[0].pass_id, 'pass-0');
  assert.equal(restored.summarize(DAY, f.now()).eligible_passes, 1);
  f.advance(2 * 3600000); restored.persist();
  assert.equal(restored.associationPasses({durationMs: DAY}).length, 0);
  assert.equal(restored.summarize(DAY, f.now()).eligible_passes, 0);
  assert.equal(restored.summarize(Infinity, f.now()).eligible_passes, 1);
  assert.equal(restored.associationWindow().complete, true);
});

test('legacy migration reports missing day coverage instead of inventing rolled-up passes', () => {
  const f = fixture(); const stats = f.create();
  finish(stats, 0, f.now()); stats.persist();
  const old = JSON.parse(f.files.get('/fixture/stats.json'));
  delete old.passHistory; delete old.passHistoryStartedAt;
  old.rollup.passes = 1000; old.rollup.eligible = 700;
  f.files.set('/fixture/stats.json', JSON.stringify(old));
  f.advance(3600000); const restored = f.create();
  assert.equal(restored.associationWindow().complete, false);
  assert.equal(restored.associationPasses({durationMs: DAY}).length, 1);
  assert.equal(restored.summarize(DAY, f.now()).eligible_passes, 1);
  assert.equal(restored.summarize(Infinity, f.now()).eligible_passes, 701);
});

test('compact association transport retains per-pass policy, timing and sensitivity identity', () => {
  const f = fixture(); const stats = f.create();
  finish(stats, 0, f.now());
  const pass = stats.state.passes[0];
  pass.sensitivityEpochId = null; pass.sensitivityEpochMixed = true;
  const value = compactAssociationPass(stats.associationPasses()[0]);
  assert.equal(value.pass_id, 'pass-0');
  assert.equal(value.site_epoch, stats.key);
  assert.equal(value.evaluation_contract_hash, stats.scoreContractHash);
  assert.equal(value.evaluation_policy_hash, stats.scorePolicyHash);
  assert.equal(value.timing_policy, 'exact');
  assert.equal(value.sensitivity_epoch_mixed, true);
  assert.equal(value.decision_grade, false);
  assert.equal(value.association_eligible_cpis, 6);
});

test('federation pass transport is bounded and preserves network scoring inputs', () => {
  const passes = Array.from({length: 4000}, (_, index) => ({
    pass_id: `pass-${index}`, hex: (index % 400).toString(16).padStart(6, '0'),
    flight: `FLT${index % 400}`, started_at: 1000 + index * 500,
    finalized_at: 1500 + index * 500,
    outcome: ['no_radar_return', 'detected_not_tracked', 'tracked'][index % 3],
    channel: ['first', 'second', 'third'][index % 3], eligible_cpis: 20,
    association_eligible_cpis: 18, timing_policy: index % 2 ? 'exact' : 'provisional',
    sensitivity_epoch_id: index % 17 ? 'd'.repeat(64) : null,
    decision_grade: index % 2 === 1 && index % 17 !== 0,
    detected_cpis: index % 3 ? 5 : 0, tracked_cpis: index % 3 === 2 ? 4 : 0,
    eligible: true, detected: index % 3 !== 0, tracked: index % 3 === 2,
    sensitivity_epoch_mixed: index % 17 === 0,
    evaluation_contract_hash: 'a'.repeat(64), evaluation_policy_hash: 'b'.repeat(64),
    site_epoch: 'c'.repeat(64), cpi_audit: Array(40).fill({unused: true})}));
  const transport = federationAssociationPassTransport(passes);
  const expanded = expandFederationAssociationPassTransport(transport);
  assert.equal(expanded.length, passes.length);
  assert.deepEqual(expanded, passes.map(compactAssociationPass));
  assert.equal(transport.dictionaries.provenance.length, 4);
  assert.ok(Buffer.byteLength(JSON.stringify(transport)) < 1024 * 1024);
});

test('legacy or malformed provenance remains unavailable, never borrowed from another pass', () => {
  const valid = {pass_id: 'pass-1', hex: 'a', flight: '', started_at: 1000, finalized_at: 2000,
    channel: 'primary', outcome: 'tracked', eligible_cpis: 6, association_eligible_cpis: 5,
    detected_cpis: 4, tracked_cpis: 3, site_epoch: 'c'.repeat(64),
    evaluation_contract_hash: 'd'.repeat(64), sensitivity_epoch_id: 'e'.repeat(64), timing_policy: 'exact'};
  const transport = federationAssociationPassTransport([
    {...valid, evaluation_policy_hash: 'a'.repeat(64), decision_grade: true},
    {...valid, hex: 'b', evaluation_policy_hash: 'b'.repeat(64), sensitivity_epoch_mixed: true}]);
  assert.equal(expandFederationAssociationPassTransport(transport)[1].evaluation_policy_hash, 'b'.repeat(64));
  assert.equal(expandFederationAssociationPassTransport(transport)[1].sensitivity_epoch_mixed, true);
  for (const index of [undefined, -1, 2, '0', 0.5]) {
    const invalid = structuredClone(transport); invalid.rows[0][11] = index;
    assert.throws(() => expandFederationAssociationPassTransport(invalid), /Invalid federation pass provenance batch/);
  }
  const legacy = structuredClone(transport);
  legacy.rows = legacy.rows.map(row => row.slice(0, 10)); delete legacy.dictionaries.provenance;
  legacy.fields = legacy.fields.slice(0, 10);
  assert.equal(expandFederationAssociationPassTransport(legacy)[0].evaluation_policy_hash, null);
  assert.equal(expandFederationAssociationPassTransport(legacy)[0].pass_id, null);
  transport.dictionaries.provenance[0].pop();
  assert.throws(() => expandFederationAssociationPassTransport(transport), /Invalid federation pass provenance batch/);
  transport.dictionaries.provenance = {0: transport.dictionaries.provenance[1]};
  assert.throws(() => expandFederationAssociationPassTransport(transport), /Invalid federation pass provenance batch/);
});

test('public statistics retain metrics without streaming durable capture and truth evidence', () => {
  const f = fixture(); const stats = f.create();
  finish(stats, 0, f.now());
  const windows = stats.status().windows;
  const truth = {timestamp_ms: f.now(), provenance: {evidence: 'x'.repeat(16384)}};
  stats.state.eligibilityEvents = Array.from({length: 250}, (_, index) => ({id: `event-${index}`,
    capture_state: 'not_integrated', truth_samples: [truth, truth, truth, truth],
    capture_windows: [{truth_samples: [truth, truth, truth, truth]}]}));
  const response = stats.status();
  assert.equal(response.eligibility_event_count, 250);
  assert.deepEqual(response.windows, windows, 'Every retained summary metric is unchanged');
  assert.equal(response.eligibility_events, undefined);
  assert.ok(Buffer.byteLength(JSON.stringify(response)) < 256 * 1024);
  stats.persist();
  const durable = JSON.parse(f.files.get('/fixture/stats.json'));
  assert.equal(durable.eligibilityEvents[0].capture_windows[0].truth_samples.length, 4);
  assert.equal(durable.eligibilityEvents[0].truth_samples[0].provenance.evidence.length, 16384);
});
