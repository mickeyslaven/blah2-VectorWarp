'use strict';
const assert = require('assert');
const crypto = require('crypto');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {validateTruthWindow, verifyCaptureIntegrity, writeAdmissionSidecar,
  captureArtifactState, admissionsNeedTruthSettlement} = require('./capture-admission');

(async () => {
  const provenance = offset => ({projection_model: 'constant_velocity_ground_track',
    observation_time_offset_s: offset, maximum_pairwise_projected_disagreement_km: .1});
  const samples = Array.from({length: 22}, (_, index) =>
    ({timestamp_ms: index * 500, provenance: provenance(.4)}));
  const accepted = validateTruthWindow(samples, 0, 10000);
  assert.equal(accepted.valid, true); assert.equal(accepted.cpis.length, 20);
  assert.equal(accepted.maximum_bracket_span_ms, 500);
  assert.equal(validateTruthWindow(samples.filter((_, i) => i < 4 || i > 10), 0, 10000).valid, false);
  const stale = structuredClone(samples); stale[1].provenance.observation_time_offset_s = 1.01;
  assert.deepEqual(validateTruthWindow(stale, 0, 10000).reasons, ['truth-raw-offset']);
  const held = structuredClone(samples); held[1].provenance.projection_model = 'nearest';
  assert.deepEqual(validateTruthWindow(held, 0, 10000).reasons, ['truth-hold-or-fallback']);
  assert.equal(captureArtifactState({valid: true}, {bad: {valid: false}, good: {valid: true}}),
    'admitted', 'one valid target must retain a shared clip without admitting invalid targets');
  assert.equal(captureArtifactState({valid: true}, {bad: {valid: false}}), 'quarantined');
  assert.equal(captureArtifactState({valid: false}, {good: {valid: true}}), 'quarantined');
  assert.equal(admissionsNeedTruthSettlement({one: {valid: false,
    reasons: ['truth-bracket-missing']}}), true);
  assert.equal(admissionsNeedTruthSettlement({one: {valid: false,
    reasons: ['truth-raw-offset']}, two: {valid: true, reasons: []}}), false);

  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-admission-'));
  const file = path.join(directory, 'clip.mchq'), body = Buffer.from('bounded-iq');
  fs.writeFileSync(file, body);
  fs.writeFileSync(`${file}.json`, JSON.stringify({schema: 'vectorwarp-kraken-iq-clip-v1',
    clipRevision: 7, windowStart: 0, windowEnd: 10000, samplesPerChannel: 10,
    dataBytes: body.length, dataSha256: crypto.createHash('sha256').update(body).digest('hex')}));
  const integrity = await verifyCaptureIntegrity(file,
    {revision: 7, windowStart: 0, windowEnd: 10000, samples: 10});
  assert.equal(integrity.valid, true); assert.equal(integrity.data_bytes, body.length);
  assert.equal(integrity.data_sha256_verified_by, 'independent-read-sha256');
  const admission = await writeAdmissionSidecar(file, {state: 'admitted'});
  assert.equal(JSON.parse(fs.readFileSync(admission)).state, 'admitted');
  fs.appendFileSync(file, 'corrupt');
  assert.equal((await verifyCaptureIntegrity(file, {revision: 7})).valid, false);
  fs.rmSync(directory, {recursive: true, force: true});
  console.log('PASS strict CPI truth and capture SHA admission');
})().catch(error => { console.error(error); process.exitCode = 1; });
