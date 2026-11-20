'use strict';

// Read-only presentation of bounded evidence collection and configured static
// models. Fitting and evaluation belong to explicitly invoked offline tools.
function buildArrayCalibrationStatus({config, recorderStatus, generatedAt = Date.now()}) {
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
    schema: 'vectorwarp-array-calibration-status-v3',
    generated_at: generatedAt,
    frequency_hz: config.capture?.fc || null,
    corpus: {
      role: 'installed-array-aircraft-response-corpus',
      schema: 'vectorwarp-manifold-sample-v2',
      path: recorder.path || null,
      cohort_root: recorder.cohortRoot || null,
      physical_site_fingerprint: recorder.physicalSiteFingerprint || null,
      acquisition_fingerprint: recorder.acquisitionFingerprint || null,
      accumulated_cohorts: recorder.cohorts || {files: 0, sampleRows: 0,
        referenceRows: 0, physicalSites: 0, acquisitionIdentities: 0},
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
      error: recorder.error || null,
      storage_limits: recorder.storageLimits || null
    },
    reference,
    bearing: {
      purpose: 'direction-of-arrival estimation after a delay-Doppler return exists',
      enabled: config.process?.bearing?.enable === true,
      active_model: config.process?.bearing?.manifold_file || null
    },
    detection: {
      purpose: 'surveillance beam-bank weighting before detection and CFAR',
      enabled: config.process?.detection?.enable === true,
      active_model: config.process?.surveillance_fusion?.manifold_file || null
    }
  };
}

module.exports = {buildArrayCalibrationStatus};
