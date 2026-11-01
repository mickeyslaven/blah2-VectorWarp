(function(root, factory) {
  if (typeof module !== 'undefined' && module.exports) module.exports = factory();
  else root.VectorWarpConfigProfiles = factory();
})(typeof globalThis !== 'undefined' ? globalThis : this, function() {
  'use strict';

  const object = value => value !== null && typeof value === 'object' && !Array.isArray(value);
  const ensure = (parent, key) => object(parent[key]) ? parent[key] : (parent[key] = {});

  const detectionProfiles = Object.freeze({
    conservative: 0.000001,
    balanced: 0.00001,
    sensitive: 0.0001,
    research: 0.005
  });

  // A tracking profile is a detector-to-track policy. Weak-target recovery
  // remains an independent explicit choice. Balanced is
  // keeps the OWL/Retina detector defaults (Pfa 1e-5 and a 15 Hz
  // zero-Doppler blank), with 4-of-6 confirmation and VectorWarp's shorter
  // false-track lifetime.
  const trackingProfiles = Object.freeze({
    permissive: Object.freeze({pfa: 0.0001, minDoppler: 5, M: 2, N: 5,
      minSnrDb: 4, deleteAfter: 5, accelerationModel: 'hypothesis_bank'}),
    balanced: Object.freeze({pfa: 0.00001, minDoppler: 15, M: 4, N: 6,
      minSnrDb: 5, deleteAfter: 3, accelerationModel: 'automatic'}),
    conservative: Object.freeze({pfa: 0.000001, minDoppler: 20, M: 4, N: 8,
      minSnrDb: 7, deleteAfter: 3, accelerationModel: 'automatic'})
  });

  function gainMode(config) {
    const value = config?.capture?.device?.heimdall?.gain_control;
    return value === 'fixed' ? 'manual' :
      ['measured', 'measured_per_channel'].includes(value) ? 'automatic' : 'custom';
  }

  function applyGainMode(config, mode) {
    const heimdall = ensure(ensure(ensure(config, 'capture'), 'device'), 'heimdall');
    if (mode === 'automatic') {
      heimdall.gain_control = 'measured_per_channel';
      if (heimdall.gain === -1) heimdall.gain = 'keep';
      if (Array.isArray(heimdall.channel_gains) && heimdall.channel_gains.some(value =>
        !(typeof value === 'number' && Number.isFinite(value) && value >= 0 && value <= 50 &&
          Math.round(value * 10) === value * 10)))
        delete heimdall.channel_gains;
    } else if (mode === 'manual') {
      heimdall.gain_control = 'fixed';
      if (Array.isArray(heimdall.channel_gains) && heimdall.channel_gains.length)
        heimdall.gain = heimdall.channel_gains[0];
      else if (typeof heimdall.gain !== 'number') heimdall.gain = 0;
    }
    return config;
  }

  function clutterMode(config) {
    const clutter = config?.process?.clutter || {};
    if (clutter.enable !== true) return 'off';
    if (clutter.followAmbiguity === true) return 'automatic';
    const ambiguity = config?.process?.ambiguity || {};
    return clutter.delayMin === ambiguity.delayMin && clutter.delayMax === ambiguity.delayMax ?
      'full_window' : 'custom';
  }

  function applyClutterMode(config, mode) {
    const clutter = ensure(ensure(config, 'process'), 'clutter');
    clutter.enable = mode !== 'off';
    if (mode === 'automatic') clutter.followAmbiguity = true;
    if (mode === 'full_window') {
      const ambiguity = config.process.ambiguity || {};
      clutter.followAmbiguity = false;
      clutter.delayMin = ambiguity.delayMin ?? 0;
      clutter.delayMax = ambiguity.delayMax ?? 0;
    }
    if (mode === 'custom') {
      clutter.followAmbiguity = false;
      const ambiguity = config.process.ambiguity || {};
      if (!Number.isInteger(clutter.delayMin)) clutter.delayMin = ambiguity.delayMin ?? 0;
      if (!Number.isInteger(clutter.delayMax)) clutter.delayMax = (ambiguity.delayMax ?? 0) + 1;
    }
    return config;
  }

  function detectionSensitivity(config) {
    const pfa = config?.process?.detection?.pfa;
    return Object.entries(detectionProfiles).find(([, value]) => value === pfa)?.[0] || 'custom';
  }

  function applyDetectionSensitivity(config, profile) {
    const detection = ensure(ensure(config, 'process'), 'detection');
    if (detectionProfiles[profile] !== undefined) detection.pfa = detectionProfiles[profile];
    return config;
  }

  function applyTrackingProfile(config, profile) {
    const values = trackingProfiles[profile];
    if (!values) return config;
    const process = ensure(config, 'process');
    const detection = ensure(process, 'detection');
    const tracker = ensure(process, 'tracker');
    const initiate = ensure(tracker, 'initiate');
    tracker.profile = profile;
    detection.pfa = values.pfa;
    detection.minDoppler = values.minDoppler;
    initiate.M = values.M;
    initiate.N = values.N;
    initiate.min_snr_db = values.minSnrDb;
    initiate.acceleration_model = values.accelerationModel === 'automatic' &&
      process.performance?.acceleration === 'gpu-resident' ? 'adaptive_gate' : 'hypothesis_bank';
    tracker.delete = values.deleteAfter;
    return config;
  }

  function weakTargetMode(config) {
    const detection = config?.process?.detection || {};
    const tbd = detection.track_before_detect || {};
    if (tbd.enable === true) return tbd.max_cpis === 4 ? 'four' : tbd.max_cpis === 8 ? 'eight' : 'custom';
    if ((detection.integration_cpis ?? 1) === 2)
      return (detection.integration_motion_bins ?? 0) === 1 ? 'two_motion' : 'two';
    return (detection.integration_cpis ?? 1) === 1 && (detection.integration_motion_bins ?? 0) === 0 ? 'off' : 'custom';
  }

  function prepareResidentArray(config) {
    const process = ensure(config, 'process');
    const performance = ensure(process, 'performance');
    performance.acceleration = 'gpu-resident';
    const fusion = ensure(process, 'surveillance_fusion');
    if (!fusion.mode || fusion.mode === 'noncoherent') fusion.mode = 'coherent_beam_bank';
    const reference = ensure(process, 'reference_synthesis');
    const channels = [...(config.capture?.device?.surveillance_channels || [])];
    reference.mode = 'array_eigenbeam';
    reference.channels = channels;
    return config;
  }

  function applyWeakTargetMode(config, mode) {
    const detection = ensure(ensure(config, 'process'), 'detection');
    const tbd = ensure(detection, 'track_before_detect');
    detection.integration_cpis = mode === 'off' ? 1 : 2;
    detection.integration_motion_bins = mode === 'two_motion' ? 1 : 0;
    tbd.enable = ['four', 'eight'].includes(mode);
    if (mode === 'four') tbd.max_cpis = 4;
    if (mode === 'eight') tbd.max_cpis = 8;
    if (['four', 'eight'].includes(mode)) prepareResidentArray(config);
    else if (mode !== 'off') {
      const process = ensure(config, 'process');
      ensure(process, 'performance').acceleration = 'gpu-resident';
      const reference = ensure(process, 'reference_synthesis');
      reference.mode = 'array_eigenbeam';
      reference.channels = [...(config.capture?.device?.surveillance_channels || [])];
    }
    return config;
  }

  function processingMode(config) {
    return config?.process?.performance?.acceleration === 'cpu' ? 'cpu' : 'automatic';
  }

  function applyProcessingMode(config, mode) {
    const process = ensure(config, 'process');
    const performance = ensure(process, 'performance');
    if (mode === 'automatic') {
      const needsResident = weakTargetMode(config) !== 'off' || process.bearing?.enable === true ||
        process.tracker?.initiate?.stationary_clutter?.enable === true ||
        process.tracker?.track_guided_shadow?.enable === true ||
        process.surveillance_fusion?.mode === 'noise_whitened_beam_bank';
      performance.acceleration = needsResident ? 'gpu-resident' : 'auto';
      return config;
    }
    performance.acceleration = 'cpu';
    const detection = ensure(process, 'detection');
    detection.integration_cpis = 1;
    detection.integration_motion_bins = 0;
    if (object(detection.track_before_detect)) detection.track_before_detect.enable = false;
    if (object(process.bearing)) process.bearing.enable = false;
    const tracker = ensure(process, 'tracker');
    if (object(tracker.track_guided_shadow)) {
      tracker.track_guided_shadow.enable = false;
      tracker.track_guided_shadow.promote = false;
    }
    if (object(tracker.initiate?.stationary_clutter)) tracker.initiate.stationary_clutter.enable = false;
    if (object(tracker.initiate) && tracker.initiate.acceleration_model === 'adaptive_gate')
      tracker.initiate.acceleration_model = 'hypothesis_bank';
    if (process.surveillance_fusion?.mode === 'noise_whitened_beam_bank')
      process.surveillance_fusion.mode = 'coherent_beam_bank';
    return config;
  }

  return {detectionProfiles, trackingProfiles, gainMode, applyGainMode, clutterMode, applyClutterMode,
    detectionSensitivity, applyDetectionSensitivity, weakTargetMode, applyWeakTargetMode,
    applyTrackingProfile, processingMode, applyProcessingMode};
});
