(function(root, factory) {
  if (typeof module !== 'undefined' && module.exports) module.exports = factory();
  else root.VectorWarpConfigProfiles = factory();
})(typeof globalThis !== 'undefined' ? globalThis : this, function() {
  'use strict';

  const object = value => value !== null && typeof value === 'object' && !Array.isArray(value);
  const finite = value => typeof value === 'number' && Number.isFinite(value);
  const ensure = (parent, key) => object(parent[key]) ? parent[key] : (parent[key] = {});

  function scaleDelayBin(bin, sampleRate, referenceRate, roundUp) {
    if (!Number.isInteger(bin) || bin < -2147483648 || bin > 2147483647 ||
        !Number.isInteger(sampleRate) || sampleRate < 1 || sampleRate > 4294967295 ||
        !Number.isInteger(referenceRate) || referenceRate < 1 || referenceRate > 4294967295)
      throw new Error('Delay bins and sample rates must be valid integers');
    const product = BigInt(bin) * BigInt(sampleRate), divisor = BigInt(referenceRate);
    let result = product / divisor;
    const remainder = product % divisor;
    if (roundUp && remainder > 0n) result += 1n;
    if (!roundUp && remainder < 0n) result -= 1n;
    if (result < -2147483648n || result > 2147483647n)
      throw new Error('Scaled delay exceeds the processor limit');
    return Number(result);
  }

  function effectiveDelayConfig(config, sampleRate = config.capture?.fs,
    {scaleNeighborhoods = false} = {}) {
    const reference = config.process?.delay_reference_sample_rate_hz;
    if (reference === undefined) return config; // Legacy lane-local bins.
    const process = config.process || {}, ambiguity = {...process.ambiguity};
    const clutter = {...process.clutter}, detection = {...process.detection};
    const scaled = (value, up) => value === undefined ? value :
      scaleDelayBin(value, sampleRate, reference, up);
    const scaleRadius = (value, maximum, name) => {
      if (value === undefined) return value;
      const result = Math.ceil(value * sampleRate / reference);
      if (!Number.isSafeInteger(result) || result < 0 || result > maximum)
        throw new Error(`Effective ${name} exceeds the supported lane limit`);
      return result;
    };
    ambiguity.delayMin = scaled(ambiguity.delayMin, false);
    ambiguity.delayMax = scaled(ambiguity.delayMax, true);
    if (clutter.followAmbiguity === true) {
      clutter.delayMin = ambiguity.delayMin;
      clutter.delayMax = ambiguity.delayMax === undefined ? undefined : ambiguity.delayMax + 1;
      if (clutter.delayMax > 2147483647) throw new Error('Clutter followAmbiguity endpoint overflows');
    } else {
      clutter.delayMin = scaled(clutter.delayMin, false);
      clutter.delayMax = scaled(clutter.delayMax, true);
    }
    detection.minDelay = scaled(detection.minDelay, true);
    if (scaleNeighborhoods) {
      detection.nGuard = scaleRadius(detection.nGuard, 127, 'CFAR guard radius');
      detection.nTrain = scaleRadius(detection.nTrain, 127, 'CFAR training radius');
      detection.nCentroid = scaleRadius(detection.nCentroid, 65535, 'centroid radius');
    }
    return {...config, process: {...process, delay_reference_sample_rate_hz: sampleRate,
      ambiguity, clutter, detection}};
  }

  function legacyDelayReferenceRate(config) {
    const multi = config.process?.multi_illuminator;
    const rates = multi?.enabled === true ? (multi.illuminators || [])
      .filter(lane => lane?.enabled === true).map(lane => lane.output_sample_rate_hz) :
      [config.capture?.replay?.state === true ?
        (config.capture.replay.sample_rate_hz ?? config.capture.fs) : config.capture?.fs];
    return rates.length && rates.every(rate => Number.isInteger(rate) && rate > 0 &&
      rate <= 4294967295 && rate === rates[0]) ? rates[0] : null;
  }

  const detectionProfiles = Object.freeze({
    conservative: 0.000001,
    balanced: 0.00001,
    sensitive: 0.0001,
    research: 0.005
  });

  // A tracking profile is a detector-to-track policy. Weak-target recovery
  // remains an independent explicit choice. Balanced keeps the OWL/Retina
  // detector and retention defaults: Pfa 1e-5, a 15 Hz zero-Doppler blank,
  // 3-of-6 confirmation, adaptive gating, and ten missed frames to delete.
  const trackingProfiles = Object.freeze({
    permissive: Object.freeze({pfa: 0.0001, minDoppler: 5, M: 2, N: 5,
      minSnrDb: 4, deleteAfter: 5, accelerationModel: 'hypothesis_bank'}),
    balanced: Object.freeze({pfa: 0.00001, minDoppler: 15, M: 3, N: 6,
      minSnrDb: 5, deleteAfter: 10, accelerationModel: 'adaptive_gate'}),
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
    initiate.acceleration_model = values.accelerationModel === 'automatic' ?
      (process.performance?.acceleration === 'gpu-resident' ? 'adaptive_gate' :
        'hypothesis_bank') : values.accelerationModel;
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

  function associationGates(span, prediction, timestamp, kind = 'detection',
    eventTimeUncertaintyS = 0) {
    const age = finite(prediction?.timestamp) ?
      Math.min(span.maxTruthAgeS, Math.max(0, timestamp / 1000 - prediction.timestamp)) :
      span.maxTruthAgeS;
    const boundedTimeUncertainty = finite(eventTimeUncertaintyS) ?
      Math.max(0, eventTimeUncertaintyS) : 0;
    const horizon = age + span.cpiSeconds * (kind === 'tracker' ? 3 : 1) +
      boundedTimeUncertainty;
    const delayMotion = finite(prediction?.delay_rate_km_s) ?
      Math.abs(prediction.delay_rate_km_s) * horizon : 0;
    const dopplerMotion = finite(prediction?.doppler_rate_hz_s) ?
      Math.abs(prediction.doppler_rate_hz_s) * horizon : 0;
    const delayFloor = span.kmPerBin * (kind === 'tracker' ? 3 : 2);
    const dopplerFloor = span.dopplerResolutionHz * (kind === 'tracker' ? 4 : 3);
    return {
      delayKm: Math.min(Math.max(span.matchDelayKm, delayFloor + delayMotion),
        Math.max(1, span.kmPerBin * 8)),
      dopplerHz: Math.min(Math.max(span.matchDopplerHz, dopplerFloor + dopplerMotion),
        Math.max(25, span.dopplerResolutionHz * 12)),
      eventTimeUncertaintyS: boundedTimeUncertainty
    };
  }

  function assignments(points, predictions, gatesForPrediction, kind, frameTimestamp,
    diagnostics = null) {
    const result = new Map(), predictionById = new Map(predictions);
    for (const point of points) {
      if (diagnostics) diagnostics.points = (diagnostics.points || 0) + 1;
      const candidates = predictions.map(([id, prediction]) => {
        const gates = gatesForPrediction(prediction);
        const delayError = Math.abs(point.delay - prediction.delay);
        const dopplerError = Math.abs(point.doppler - prediction.doppler);
        return {id, delayError, dopplerError, gates,
          score: (delayError / gates.delayKm) ** 2 +
            (dopplerError / gates.dopplerHz) ** 2};
      }).filter(item => item.delayError <= item.gates.delayKm &&
        item.dopplerError <= item.gates.dopplerHz)
        .sort((left, right) => left.score - right.score);
      if (diagnostics) diagnostics.candidate_matches =
        (diagnostics.candidate_matches || 0) + candidates.length;
      if (!candidates.length) {
        if (diagnostics) diagnostics.unmatched_points = (diagnostics.unmatched_points || 0) + 1;
        continue;
      }
      if (candidates.length > 1 && candidates[1].score <= candidates[0].score * 1.15 + .05) {
        if (diagnostics) diagnostics.ambiguous_points = (diagnostics.ambiguous_points || 0) + 1;
        continue;
      }
      const selected = candidates[0], prediction = predictionById.get(selected.id);
      const prior = result.get(selected.id);
      if (prior && prior.normalized_gate_score <= selected.score) continue;
      result.set(selected.id, {kind, frame_timestamp_ms: frameTimestamp,
        cell_index: point.index, measured_delay_raw: point.rawDelay,
        measured_delay_km: point.delay, measured_doppler_hz: point.doppler,
        measured_snr_db: point.snr ?? null, tracker: point.tracker ?? null,
        predicted_delay_km: prediction?.delay ?? null,
        predicted_doppler_hz: prediction?.doppler ?? null,
        delay_residual_km: point.delay - (prediction?.delay ?? point.delay),
        doppler_residual_hz: point.doppler - (prediction?.doppler ?? point.doppler),
        gate_delay_km: selected.gates.delayKm, gate_doppler_hz: selected.gates.dopplerHz,
        event_time_uncertainty_s: selected.gates.eventTimeUncertaintyS || 0,
        normalized_gate_score: selected.score});
      if (diagnostics) diagnostics.selected_points = (diagnostics.selected_points || 0) + 1;
    }
    return result;
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
    if (process.surveillance_fusion?.mode === 'noise_whitened_beam_bank')
      process.surveillance_fusion.mode = 'coherent_beam_bank';
    return config;
  }

  return {scaleDelayBin, effectiveDelayConfig, legacyDelayReferenceRate,
    detectionProfiles, trackingProfiles, gainMode, applyGainMode, clutterMode, applyClutterMode,
    detectionSensitivity, applyDetectionSensitivity, weakTargetMode, applyWeakTargetMode,
    applyTrackingProfile, processingMode, applyProcessingMode, associationGates, assignments};
});
