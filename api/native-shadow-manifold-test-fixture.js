'use strict';
const crypto = require('crypto');
const {canonical, canonicalHash, deriveNativeShadowManifoldRow} = require('./native-shadow-manifold-evidence');
const key = Buffer.alloc(32, 75), keyId = crypto.createHash('sha256').update(key).digest('hex');
const options = {resolveKey: id => id === keyId ? key : null, signingKey: key, keyId};
function sign(source) {
  delete source.export_sha256; delete source.authentication;
  source.export_sha256 = canonicalHash(source);
  const unsigned = {...source}; delete unsigned.export_sha256;
  source.authentication = {algorithm: 'hmac-sha256', key_id: keyId,
    mac: crypto.createHmac('sha256', key).update(canonical(unsigned)).digest('hex')};
  return source;
}
function source({pass = 'pass-1', time = 1000000, phase = 0, elevation = 5, epoch = 'epoch-1',
  gain = 18, label = 'subthreshold', response = [[1, 0], [0, .6]]} = {}) {
  const covariance = [[[2, 0], [0, 0]], [[0, 0], [1, 0]]];
  return sign({schema: 'vectorwarp-native-truth-export-v1', shadow_only: true,
    hardware_qualified: false, production_promotion_allowed: false, epoch_id: epoch,
    collection_epoch: {config_epoch: 'config-1', receiver_epoch: epoch},
    pass: {id: pass, icao: 'abc123', label},
    truth: {local_azimuth_deg: 10, elevation_deg: elevation, position_timestamp_ms: time, provenance: 'adsb'},
    configuration: {config_fingerprint: 'exact-config', cpi_seconds: .0005,
      fusion_mode: 'coherent_beam_bank', frequency_hz: 539000000,
      sample_rate_hz: 128000, channels: 2, actual_channel_gains_db: [gain, gain], channel_mapping: [0, 1]},
    sample_clock: {admitted: true, epoch_id: epoch, source_epoch: 1, first_epoch_sample: time * 128,
      end_epoch_sample: time * 128 + 64, source_sample_midpoint: time * 128 + 32,
      timestamp_ms: time, time_mapping: 'admitted-source-sample-midpoints'},
    source_snapshots: [{sha256: canonicalHash({pass, time}), epoch_id: epoch,
      raw_iq_sha256: '1'.repeat(64), source_sha256: '2'.repeat(64), rf_sha256: '3'.repeat(64),
      native_config_sha256: '4'.repeat(64), native_module_sha256: '5'.repeat(64), native_device_sha256: '6'.repeat(64)}],
    covariance: {matrix: covariance, samples: 64, sha256: canonicalHash(covariance), epoch_id: epoch,
      state: 'native-resident-reference-covariance', estimated_from_cut: false},
    target_cut: {id: `${time}:1:1`, role: 'target', raw_iq_sha256: '1'.repeat(64), epoch_id: epoch,
      representation: 'native-physical-channel-complex-caf',
      physical_channels: response.map(([r, i]) => [r * Math.cos(phase) - i * Math.sin(phase), r * Math.sin(phase) + i * Math.cos(phase)])},
    reference_weights: [[1, 0], [1, 0]], actual_fusion_weights: [[1, 0], [0, -.2]],
    weight_provenance: 'native-coherent-fusion-weights',
    local_control_cells: [{id: `${time}:1:2`, physical_channels: [[1, 0], [0, -.6]]}],
    local_control_scope: 'nearby physical CUTs; not certified target-free CFAR training',
    detector: {scope: 'native-cfar-before-centroid', statistics: [[1, 2, 3, 4, label === 'detected' ? 3 : 1]],
      production_cfar_recomputed: false}});
}
function ledger(sources) {
  let previousRowSha256 = null;
  return sources.map(item => { const row = deriveNativeShadowManifoldRow(JSON.parse(JSON.stringify(item)), {...options, previousRowSha256});
    previousRowSha256 = row.row_sha256; return row; });
}
function queuedFixture(sources) {
  const native_identity = {frequency_hz: 539000000, sample_rate_hz: 128000,
    channels: 2, actual_channel_gains_db: [18, 18]};
  const identity = {recording_sha256: 'a'.repeat(64), native_sidecar_sha256: 'b'.repeat(64),
    admission_sha256: 'c'.repeat(64), config_sha256: 'd'.repeat(64), native_identity,
    native_identity_sha256: canonicalHash(native_identity)};
  const job = {schema: 'vectorwarp-integration-shadow-job-v1', identity,
    job_id: canonicalHash(identity), timing_policy: 'exact', decision_grade: true,
    captures: sources.map((_, index) => ({capture_id: `capture-${index}`, processor_cpis: 3})),
    experiment: {dwell_seconds: [.05]}};
  job.job_binding_sha256 = canonicalHash(job);
  const truth_selected_exports = sources.map((item, index) => {
    const frames = [0, 1, 2].map(frame => {
      const evidence = JSON.parse(JSON.stringify(item));
      evidence.job_binding_sha256 = job.job_binding_sha256;
      evidence.sample_clock.timestamp_ms += frame * .5;
      evidence.truth.position_timestamp_ms += frame * .5;
      evidence.sample_clock.first_epoch_sample += frame * 64;
      evidence.sample_clock.end_epoch_sample += frame * 64;
      evidence.sample_clock.source_sample_midpoint += frame * 64;
      sign(evidence);
      return {manifold_evidence: evidence, native_snapshot: {native_contract_verified: true,
        sha256: evidence.source_snapshots[0].sha256, path: `/synthetic/frame-${index}-${frame}.rps`},
        target_cut: {physical_channels: evidence.target_cut.physical_channels, beam_powers: [1]},
        covariance_whitening: {state: 'native-resident-settled-snapshot'}};
    });
    const value = {schema: 'vectorwarp-truth-selected-shadow-export-v2', shadow_only: true, decision_grade: false,
      production_promotion_allowed: false, production_unchanged: true,
      selection: {basis: 'geometry-eligible-truth-cut-only', cfar_or_beam_selection_used: false,
        capture_id: `capture-${index}`, arm: '0.05s-production'},
      provenance: {job_binding_sha256: job.job_binding_sha256, native_recorder: 'ResidentReplaySnapshot-v1',
        native_contract_verified: true, hardware_qualified: false, time_mapping: 'admitted-source-sample-midpoints'},
      covariance_whitening: {state: 'native-resident-settled-snapshot', estimated_from_cut: false}, multi_cpi_history: frames};
    value.export_sha256 = canonicalHash(value); return value;
  });
  const result = {schema: 'vectorwarp-integration-shadow-result-v1', job_id: job.job_id,
    state: 'complete', shadow_only: true, production_unchanged: true, decision_grade: false,
    timing_policy: 'exact', timing_decision_grade: true, hardware_qualified: false,
    production_promotion_allowed: false, job_binding_sha256: job.job_binding_sha256,
    input: identity, truth_selected_exports, arms: {}, summary: {selection_performed: false}};
  result.result_binding_sha256 = canonicalHash(result);
  result.result_authentication = {algorithm: 'hmac-sha256', key_id: keyId,
    tag: crypto.createHmac('sha256', key).update(canonical(result)).digest('hex')};
  return {job, result};
}
module.exports = {key, keyId, options, sign, source, ledger, queuedFixture};
