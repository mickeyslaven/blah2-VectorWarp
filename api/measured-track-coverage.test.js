'use strict';
// Exact arithmetic oracle for the shared API/browser aircraft-time metric.
const assert = require('node:assert/strict');
const test = require('node:test');
const coverage = require('../html/js/measured_track_coverage');
const {union, aggregate, kioskAssociationChannel} = coverage;
const {MultiIlluminatorEvidence} = require('./multi-illuminator-evidence');
const {EvidenceStatistics, measurementSupport, coverageBasis, observeMeasuredCoverage, sourceCpiInterval, boundMeasuredCoverage} = require('./evidence-statistics');
const basis = (eligible, measured, scored = eligible, hex = 'abc123') => ({version: 1,
  complete: true, aircraft: [{hex, eligible, scored, measured}]});
const full = [[0, 20000]], hits = [[0, 1000], [3000, 4000], [8000, 9000], [18000, 20000]];
assert.deepEqual(sourceCpiInterval({timestamp: 1000, eventTime: {firstSample: '9007199254741000',
  endSample: '9007199254742200', sampleRateHz: 2400}}, 1), [750, 1250],
  'Actual captured sample duration overrides the requested/configured CPI duration');
assert.equal(sourceCpiInterval({timestamp: 1000, eventTime: {firstSample: '0', endSample: '0', sampleRateHz: 2400}}, .5), null,
  'Invalid declared sample bounds must not silently fall back to a guessed interval');
assert.equal(sourceCpiInterval({timestamp: 1000}, NaN), null);
assert.equal(sourceCpiInterval({timestamp: 1000}, .5), null,
  'Configured CPI duration cannot manufacture absent captured source bounds');
assert.equal(sourceCpiInterval({timestamp: 1000, eventTime: {firstSample: '18446744073709551615',
  endSample: '18446744073709552815', sampleRateHz: 2400}}), null,
  'Captured sample bounds must fit the native uint64 identity contract');
assert.deepEqual(union([[2000, 4000], [0, 3000], [4000, 5000]]), [[0, 5000]]);
const incremental = [], inputs = Array.from({length: 300}, (_, index) => {
  const start = ((index * 7919) % 1000) / 4;
  return [start, start + index % 11 + .125];
});
for (let index = 0; index < inputs.length; ++index)
  assert.deepEqual(coverage.extendUnion(incremental, [inputs[index]]), union(inputs.slice(0,index+1)),
    'Incremental insertion equals the independent sorted-union oracle for unordered overlapping intervals');
assert.equal(coverage.extendUnion(incremental, [[NaN,1],[3,2]]),incremental);
const gaps = aggregate([basis(full, hits)]);
assert.equal(gaps.eligible_seconds, 20);
assert.equal(gaps.measured_seconds, 5);
assert.equal(gaps.fraction, .25);
assert.equal(gaps.gap_count, 3);
assert.equal(gaps.gap_p50_seconds, 4);
assert.equal(gaps.gap_p95_seconds, 9);
assert.equal(gaps.gap_max_seconds, 9);
assert.equal(aggregate([basis(full, [[1000, 19000]])]).gap_count, 0,
  'Unmeasured left/right pass edges are censored, not internal gaps');
const interrupted = aggregate([basis(full, hits, [[0, 4000], [8000, 20000]])]);
assert.equal(interrupted.unscored_seconds, 4);
assert.equal(interrupted.fraction, null, 'Unscored eligible presence must not silently shrink the denominator');
assert.equal(interrupted.gap_count, null, 'Incomplete exposure cannot claim a complete gap distribution');
assert.equal(interrupted.observed_internal_gap_count, 2, 'Unknown time cannot join the two scored runs');
const excluded = aggregate([basis([[0, 4000], [8000, 20000]], hits)]);
assert.equal(excluded.gap_count, 2, 'Eligibility breaks cannot be counted as track gaps');
assert.equal(aggregate([basis(full, [])]).fraction, 0, 'Known all-miss evidence is zero, not unavailable');
assert.equal(aggregate([]).fraction, null);
assert.equal(aggregate([basis([], [])]).fraction, null);
assert.equal(aggregate([undefined, basis(full, hits)]).fraction, null);
assert.equal(aggregate([{...basis(full, hits), complete: false}]).gap_count, null);
assert.equal(aggregate([{version: 1, complete: true, aircraft: [{hex: 'abc123', eligible: 'bad'}]}]).complete, false);

// Different channel/node pass starts are immaterial: identity and actual time
// form the union, with partial overlaps and different aircraft tested separately.
const a = basis([[0, 10000]], [[0, 3000], [6000, 10000]]);
const b = basis([[5000, 15000]], [[5000, 12000]], [[5000, 15000]], 'ABC123');
const combined = aggregate([a, b, a]);
assert.equal(combined.eligible_seconds, 15);
assert.equal(combined.measured_seconds, 10);
assert.equal(combined.fraction, 2 / 3);
assert.deepEqual(aggregate([b, a]), aggregate([a, b]), 'Channel and node order cannot change totals');
assert.equal(aggregate([a, basis([[0, 10000]], [], undefined, 'def456')]).eligible_seconds, 20);
assert.equal(aggregate([a, b], {from: 7000, to: 11000}).fraction, 1,
  'Window totals must clip intervals, not include whole passes crossing the cutoff');
assert.equal(aggregate([basis(full, [[-5000, 50000]])]).measured_seconds, 20,
  'Support outside eligible presence never counts');

const history = new Map();
const firstSequence = 9007199254741000n;
const eventTime = {firstSample: '0', endSample: '1200', sampleRateHz: 2400};
for (let index = 0; index < 10; ++index) history.set(index * 500, {
  timestamp: index * 500, eventTime, evidence: [{epoch_exact: '42', newest_sequence_exact: String(firstSequence + BigInt(index))}]});
const association = (endpoint, depth) => ({frame_timestamp_ms: endpoint * 500,
  tracker: {state: 'ACTIVE', measured: true, source: 'detector', measurement_evidence: {
    epoch_exact: '42', newest_sequence_exact: String(firstSequence + BigInt(endpoint)), cpis: depth,
    support_sequences_exact: Array.from({length: depth}, (_, index) => String(firstSequence + BigInt(endpoint - index)))}}});
for (const depth of [1, 2, 4, 8]) assert.deepEqual(measurementSupport(association(9, depth), history, .5),
  [[(9 - depth) * 500 + 250, 4750]], `${depth}-CPI output receives its exact midpoint-anchored source support`);
const midpointWindow = aggregate([basis(measurementSupport(association(9, 2), history, .5),
  measurementSupport(association(9, 1), history, .5))], {from: 4000, to: 4500});
assert.equal(midpointWindow.eligible_seconds, .5);
assert.equal(midpointWindow.measured_seconds, .25);
assert.equal(midpointWindow.fraction, .5, 'Window clipping must use midpoint support, not a guessed CPI endpoint');
const support1 = measurementSupport(association(7, 8), history, .5);
const support2 = measurementSupport(association(9, 8), history, .5);
assert.equal(aggregate([basis([[-250, 4750]], [...support1, ...support2])]).measured_seconds, 5,
  'Overlapping temporal lineages are credited once');
const coast = association(9, 8); coast.tracker.measured = false;
assert.equal(measurementSupport(coast, history, .5), null);
const prediction = association(9, 8); prediction.tracker.source = 'prediction';
assert.equal(measurementSupport(prediction, history, .5), null);
const tentative = association(9, 8); tentative.tracker.state = 'ASSOCIATED';
assert.equal(measurementSupport(tentative, history, .5), null);
const legacy = association(9, 8); delete legacy.tracker.measurement_evidence;
assert.equal(measurementSupport(legacy, history, .5), null);
const duplicate = association(9, 2); duplicate.tracker.measurement_evidence.support_sequences_exact[1] = duplicate.tracker.measurement_evidence.newest_sequence_exact;
assert.equal(measurementSupport(duplicate, history, .5), null);
for (const support of [[9, 7], [9, 10], [9, 8, 6, 7]]) {
  const invalid = association(9, support.length);
  invalid.tracker.measurement_evidence.support_sequences_exact = support.map(index => String(firstSequence + BigInt(index)));
  assert.equal(measurementSupport(invalid, history), null,
    'Measured support must obey native newest-first consecutive CPI lineage');
}
for (const invalid of [
  {epoch: '18446744073709551616', sequence: '1'},
  {epoch: '42', sequence: '18446744073709551616'},
  {epoch: '42', sequence: '0'}]) {
  const malformed = association(9, 1);
  Object.assign(malformed.tracker.measurement_evidence, {epoch_exact: invalid.epoch,
    newest_sequence_exact: invalid.sequence, support_sequences_exact: [invalid.sequence]});
  const matching = new Map([[4500, {timestamp: 4500, eventTime,
    evidence_identity: {epoch_exact: invalid.epoch, newest_sequence_exact: invalid.sequence}}]]);
  assert.equal(measurementSupport(malformed, matching), null,
    'Matching malformed source and track identities cannot manufacture valid lineage');
}
const wrongEpoch = association(9, 8); wrongEpoch.tracker.measurement_evidence.epoch_exact = '43';
assert.equal(measurementSupport(wrongEpoch, history, .5), null);
const missing = new Map(history); missing.delete(1000);
assert.equal(measurementSupport(association(9, 8), missing, .5), null,
  'Depth and endpoint must never manufacture missing source-frame times');
missing.set(1000, {timestamp: 1000, eventTime, delay: [], doppler: [], evidence_identity: {
  epoch_exact: '42', newest_sequence_exact: String(firstSequence + 2n)}});
assert.deepEqual(measurementSupport(association(9, 8), missing, .5), [[750, 4750]],
  'Identity-known empty detection frames establish source time without claiming a detection');
const conflicting = new Map(history);
conflicting.set('duplicate-source', {...history.get(1000),
  eventTime: {firstSample: '0', endSample: '2400', sampleRateHz: 2400}});
assert.equal(measurementSupport(association(9, 8), conflicting, .5), null,
  'One source identity with conflicting support durations is not authoritative');

const pass = {hex: 'abc123', eligibleFrames: 0};
for (let index = 0; index < 10; ++index) {
  pass.eligibleFrames++;
  observeMeasuredCoverage(pass, index * 500, .5, true, index === 7 ? support1 : index === 9 ? support2 : null);
}
assert.equal(aggregate([coverageBasis([pass])]).measured_seconds, 5);
const legacyPass = {hex: 'def456', eligibleFrames: 10};
assert.equal(coverageBasis([legacyPass]).excluded_passes, 1);
assert.equal(aggregate([coverageBasis([legacyPass, pass])]).fraction, null);
for (let index = 10; index < 1040; ++index) {
  pass.eligibleFrames++;
  observeMeasuredCoverage(pass, index * 500, .5, true, index % 2 ? [[index * 500 - 250, index * 500 + 250]] : null);
}
assert.equal(pass.measuredCoverage.complete, true,
  'A normal pass with more than 512 separated measurements retains exact coverage');
assert.ok(pass.measuredCoverage.measured.length > 512);
const boundedPass = {hex: 'abc123', eligibleFrames: 8192, measuredCoverage: {complete: true,
  eligible: Array.from({length: 8192}, (_, index) => [index * 1000, index * 1000 + 500]),
  scored: Array.from({length: 8192}, (_, index) => [index * 1000, index * 1000 + 500]), measured: []}};
observeMeasuredCoverage(boundedPass, 8192250, .5, true, [[8192000, 8192500]]);
assert.equal(boundedPass.measuredCoverage.complete, false,
  'Bounded retention fails closed, never silently drops gaps');
assert.equal(boundedPass.measuredCoverage.eligible.length, 8192);
observeMeasuredCoverage(boundedPass, 8193250, .5, true, [[8193000, 8193500]]);
assert.equal(boundedPass.measuredCoverage.complete, false,
  'New observations cannot certify a previously discarded prefix');
assert.deepEqual(boundedPass.measuredCoverage.eligible.at(-1), [8193000, 8193500]);
assert.deepEqual(boundedPass.measuredCoverage.measured.at(-1), [8193000, 8193500],
  'Hitting the bound must not permanently freeze subsequent measurements');
assert.equal(boundedPass.measuredCoverage.eligible.length, 8192);
const older = {hex:'abc123', eligibleFrames:10, lastAt:10000,
  measuredCoverage:{complete:true, ...basis(full,hits).aircraft[0]}};
const newer = {...structuredClone(older), hex:'def456', lastAt:20000};
const newCoverage = structuredClone(newer.measuredCoverage);
assert.equal(boundMeasuredCoverage([newer, older], 8), true);
const retainedIntervals = [newer, older].reduce((total, item) => total +
  ['eligible','scored','measured'].reduce((sum, field) => sum + item.measuredCoverage[field].length, 0), 0);
assert.ok(retainedIntervals <= 8, 'The ledger-wide interval budget remains bounded');
assert.deepEqual(newer.measuredCoverage, newCoverage, 'Recent passes retain their exact evidence');
assert.equal(older.measuredCoverage.complete, false);
assert.equal(aggregate([coverageBasis([older,newer])]).fraction, null,
  'Budget pressure cannot turn partial evidence into a certified percentage');
assert.equal(boundMeasuredCoverage([newer,older],8),false, 'A bounded ledger does not repeatedly discard data');
console.log('PASS measured track coverage: source intervals, coast exclusion, overlap, gaps, missing data and aircraft/node union');

// Exercise the actual join, persistence and API summary rather than only the
// interval helpers. Empty, coasted and absent radar frames are distinct cases.
(async () => {
  const fs = require('node:fs'), os = require('node:os'), path = require('node:path');
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'measured-track-coverage-'));
  let now = 1800000000000;
  const config = {capture: {fs: 2400000, fc: 563000000},
    location: {rx: {latitude: 1, longitude: 2}, tx: {latitude: 3, longitude: 4}},
    truth: {adsb: {enabled: true}}, process: {
      ambiguity: {delayMin: 0, delayMax: 500, dopplerMin: -600, dopplerMax: 600},
      detection: {minDelay: 5}}};
  const options = {filename: path.join(directory, 'evidence.json'), now: () => now,
    setIntervalFn() { return {unref() {}}; }, clearIntervalFn() {}};
  const source = {async get() { return {abc123: {delay: 10, doppler: 100,
    timestamp: now / 1000, provenance: {observation_time_offset_s: 0, projection_model: 'identity'}}}; }};
  const statistics = new EvidenceStatistics(config, source, options);
  try {
    for (let index = 0; index < 5; ++index) {
      const record = association(index, 1).tracker;
      const detection = {timestamp: now, eventTime, delay: [10], doppler: [100], evidence: [record.measurement_evidence]};
      // Store directly to control the asynchronous join order deterministically.
      statistics.detectionHistory.set(now, detection);
      if (index !== 3) statistics.acceptTracker({timestamp: now, eventTime, data: index === 1 ? [] : [{...record,
        id: 'T1', n: 5, delay: 10 / statistics.limits.kmPerBin, doppler: 100,
        ...(index === 2 ? {state: 'COASTING', measured: false, source: 'prediction'} : {}),
        ...(index === 4 ? {measurement_evidence: null} : {})}]});
      await statistics.sample(now);
      assert.equal(statistics.state.error, null);
      now += 500;
    }
    const result = statistics.summarize(Infinity, now).measured_track_coverage;
    assert.equal(result.eligible_seconds, 2.5);
    assert.equal(result.measured_seconds, .5, 'Only the genuine operational measurement counts');
    assert.equal(result.scored_seconds, 1.5, 'Empty and coasted frames are scored misses');
    assert.equal(result.unscored_seconds, 1, 'Absent frames and absent lineage remain unknown');
    assert.equal(result.fraction, null);
    assert.equal(statistics.state.active.abc123.trackedFrames, 2, 'Retained pass counters keep their existing contract');
    statistics.close();
    const restored = new EvidenceStatistics(config, source, options);
    assert.deepEqual(restored.measuredTrackBasis(), statistics.measuredTrackBasis());
    restored.close();
    console.log('PASS measured coverage ingestion, missing-frame semantics, retained pass counters and restart persistence');
  } finally {
    statistics.close(); fs.rmSync(directory, {recursive: true, force: true});
  }
})().catch(error => { console.error(error); process.exitCode = 1; });

test('kiosk projection keeps every pass and exact measured interval without diagnostic payloads', () => {
  const channel = {selected_channel:'lane', updated_at:123, association_statistics:{evaluation_policy_hash:'a'.repeat(64)},
    measured_track_basis:{version:1,complete:true,aircraft:[{hex:'abc123',
      eligible:[[0,10.25]],scored:[[0,10.25]],measured:[[.25,5.75],[7,10.25]]}]},
    passes:Array.from({length:4000}, (_,index) => ({hex:`${index}`,started_at:index,
      finalized_at:index+1,eligible:true,detected:index%2===0,tracked:index%3===0,
      cpi_exposure:{eligible_cpis:10,association_eligible_cpis:8,detected_cpis:4},
      cpi_audit:Array(100).fill({large:'diagnostic'}), azimuth_bands:{unused:1}})),
    track_population:{diagnostic:'not used on distance display'}};
  const result = kioskAssociationChannel(channel);
  assert.equal(result.passes.length, 4000);
  assert.deepEqual(result.passes, channel.passes.map(({cpi_audit,azimuth_bands,...pass}) => pass));
  assert.deepEqual(coverage.aggregate([result.measured_track_basis]), coverage.aggregate([channel.measured_track_basis]));
  assert.deepEqual(coverage.expandBasis(result.measured_track_basis), channel.measured_track_basis);
  assert.equal(result.association_statistics, channel.association_statistics);
  assert.ok(JSON.stringify(result).length < JSON.stringify(channel).length / 5);
  assert.ok(!('track_population' in result));
});

test('compact interval transport retains sparse breaks, fractional times and incomplete evidence', () => {
  const original = {version:1,complete:false,excluded_passes:2,aircraft:[{hex:'ABC123',
    eligible:[[1795359350682,1795359352682],[1795359354682.25,1795359356682.75]],
    scored:[[1795359350682,1795359352682],[1795359354682.25,1795359356682.75]],
    measured:[[1795359350682.5,1795359350684.25]]}]};
  const compact = coverage.compactBasis(original);
  assert.equal(compact.aircraft[0].scored, 'eligible');
  assert.deepEqual(coverage.expandBasis(compact), original);
  assert.deepEqual(coverage.aggregate([compact,original]), coverage.aggregate([original,original]));
  const invalid = structuredClone(compact); invalid.aircraft[0].measured=[0,-1];
  assert.equal(coverage.aggregate([invalid]).complete,false, 'Corrupt compact data never becomes known zero');
  assert.equal(coverage.compactBasis({version:1,aircraft:[{eligible:null}]}).version,1,
    'Invalid legacy evidence retains its existing unavailable semantics');
});

test('multi-channel route uses the same projection and leaves normal and compact responses unchanged', () => {
  const statistics = {liveStatus:()=>({updated_at:1,current_setup_lifetime:{eligible_passes:3}}),
    measuredTrackBasis:()=>({version:1,complete:true,aircraft:[]}),
    associationPasses:()=>[{hex:'abc',started_at:0,finalized_at:1,cpi_audit:['large']}]};
  const multi = Object.create(MultiIlluminatorEvidence.prototype);
  multi.lanes = new Map([['a',{illuminator:{id:'a'},exact:statistics,operational:statistics}]]);
  const full = multi.associationBasis();
  const kiosk = multi.associationBasis({kiosk:true});
  assert.deepEqual(kiosk.channels, full.channels.map(kioskAssociationChannel));
  assert.ok(full.channels[0].passes[0].cpi_audit);
  assert.equal(multi.associationBasis({compact:true}).channels[0].passes, undefined);
  const source = require('node:fs').readFileSync(require.resolve('./server'),'utf8');
  assert.match(source,/channels: \[kiosk \? kioskAssociationChannel\(channel\) : channel\]/,
    'Single-channel receivers use the identical projection');
});
