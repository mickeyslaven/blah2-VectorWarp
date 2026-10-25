'use strict';

// The recorder intentionally produces one immutable installed-array evidence
// corpus. Bearing and detection fit different models from that corpus and must
// retain independent qualification and activation state.
function compactAutomation(status = {}, {autoPromoteEnabled = false,
  forcePromotionDisabled = false} = {}) {
  return {
    enabled: Boolean(status.enabled),
    state: status.state || 'disabled',
    running: Boolean(status.running),
    qualified: Boolean(status.qualified),
    auto_promote_enabled: Boolean(autoPromoteEnabled),
    promotion_allowed: forcePromotionDisabled ? false :
      Boolean(status.qualified && autoPromoteEnabled),
    failures: Array.isArray(status.failures) ? status.failures : [],
    error: status.error || null,
    last_evaluation_at: status.lastEvaluationAt || null,
    candidate_path: status.candidatePath || null,
    qualified_path: status.qualifiedPath || null,
    validation_path: status.validationPath || null,
    runtime_candidate_path: status.runtimeCandidatePath || null,
    candidate_version: status.candidateVersion || null,
    versioned_candidate_path: status.versionedCandidatePath || null,
    shadow_ab_receipt_path: status.receiptPath || null,
    native_shadow_corpus: status.nativeShadow ? {path: status.nativeShadow.path || null,
      added: status.nativeShadow.added || 0, ignored: status.nativeShadow.ignored || 0,
      error: status.nativeShadow.error || null} : null,
    promotion: status.promotion || null
  };
}

function buildArrayCalibrationStatus({config, recorderStatus, bearingStatus,
  detectionStatus, frequencyResolvedStatus, generatedAt = Date.now()}) {
  const recorder = recorderStatus || {};
  const reference = {
    role: 'fixed-transmitter-direct-path-anchor',
    mixed_with_aircraft_responses: false,
    path: recorder.referencePath || null,
    samples: recorder.referenceSamples || 0,
    updates: recorder.referenceUpdates || 0,
    last_sample_at: recorder.lastReferenceAt || null,
    maximum_phase_step_deg: recorder.maxReferencePhaseStepDeg ?? null,
    maximum_amplitude_step_db: recorder.maxReferenceAmplitudeStepDb ?? null
  };
  return {
    schema: 'vectorwarp-array-calibration-status-v2',
    generated_at: generatedAt,
    frequency_hz: config.capture?.fc || null,
    relationship: {
      shared_evidence_corpus: true,
      shared_fitted_model: false,
      explanation: 'Bearing and detection use the same accepted complex array-response rows, but fit, validate, activate, and version separate models.'
    },
    corpus: {
      role: 'installed-array-aircraft-response-corpus',
      schema: 'vectorwarp-manifold-sample-v2',
      path: recorder.path || null,
      enabled: Boolean(recorder.enabled),
      automatic: Boolean(recorder.automatic),
      active: Boolean(recorder.active),
      queued: Boolean(recorder.queued),
      frames_seen: recorder.frames || 0,
      accepted_samples_this_process: recorder.samples || 0,
      unmatched_cells: recorder.unmatched || 0,
      stale_truth_rejections: recorder.staleTruth || 0,
      elevation_rejections: recorder.elevationRejected || 0,
      ambiguous_matches: recorder.ambiguous || 0,
      last_write_at: recorder.lastWriteAt || null,
      error: recorder.error || null
    },
    reference,
    bearing: {
      purpose: 'direction-of-arrival estimation after a delay-Doppler return exists',
      active_model: config.process?.bearing?.manifold_file || null,
      automation: compactAutomation(bearingStatus, {
        autoPromoteEnabled: config.process?.bearing?.manifold_automation?.auto_promote !== false
      }),
      progress: bearingStatus?.progress || null
    },
    detection: {
      purpose: 'surveillance beam-bank weighting before detection and CFAR',
      active_model: config.process?.surveillance_fusion?.manifold_file || null,
      active_model_policy: 'unchanged; shadow candidates cannot be promoted automatically',
      automation: compactAutomation(detectionStatus, {forcePromotionDisabled: true}),
      progress: detectionStatus?.progress || null
    },
    frequency_resolved: frequencyResolvedStatus || null
  };
}

module.exports = {compactAutomation, buildArrayCalibrationStatus};
