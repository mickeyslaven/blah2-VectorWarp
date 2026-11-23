(function(root, factory) {
  if (typeof module !== 'undefined' && module.exports) module.exports = factory();
  else root.VectorWarpConfigProfiles = factory();
})(typeof globalThis !== 'undefined' ? globalThis : this, function() {
  'use strict';

  const object = value => value !== null && typeof value === 'object' && !Array.isArray(value);
  const finite = value => typeof value === 'number' && Number.isFinite(value);
  const ensure = (parent, key) => object(parent[key]) ? parent[key] : (parent[key] = {});

  // Shift decimal units before parsing: multiplying an already rounded binary
  // float turns valid whole-Hz entries such as 2.000001 MS/s into fractions.
  function scaledInputNumber(value, scale = 1) {
    const exponent = Math.log10(scale);
    const decimal = String(value).trim().match(/^([+-]?(?:\d+(?:\.\d*)?|\.\d+))(?:e([+-]?\d+))?$/i);
    return decimal && Number.isInteger(exponent) ?
      Number(`${decimal[1]}e${Number(decimal[2] || 0) + exponent}`) : Number(value) * scale;
  }

  // Descriptors, not regional tuning defaults. Only ATSC reference conditioning
  // distinguishes versions; all other standards use the same raw-IQ extractor.
  const channelizerStandards = Object.freeze(['AM', 'FM', 'DAB', 'DRM', 'ATSC',
    'ATSC-1.0', 'ATSC-3.0', 'DVB-T', 'DVB-T2', 'DVB-T/T2', 'ISDB-T', 'DTMB', 'CUSTOM']);
  const channelizerPresets = Object.freeze({
    ATSC: {occupied_bandwidth_hz: 5380000, filter_taps_per_phase: 64},
    'ATSC-1.0': {occupied_bandwidth_hz: 5380000, filter_taps_per_phase: 64},
    'ATSC-3.0': {occupied_bandwidth_hz: 5832844, filter_taps_per_phase: 128}
  });
  function channelizerPreview(config, lane) {
    const capture = config.capture || {}, replay = capture.replay || {};
    const rate = replay.state === true ? replay.sample_rate_hz ?? capture.fs : capture.fs;
    const center = replay.state === true ? replay.frequency_hz ?? capture.fc : capture.fc;
    const {frequency_hz: frequency, occupied_bandwidth_hz: width,
      output_sample_rate_hz: output, filter_taps_per_phase: taps = 24} = lane;
    const errors = [];
    if (![rate, center, frequency, width, output, taps].every(Number.isSafeInteger) ||
        Math.min(rate, center, frequency, width, output) <= 0)
      errors.push('Frequency, occupied bandwidth and sample rates must be positive whole Hz; filter taps must be an integer');
    else {
      if (rate % output !== 0) errors.push(`output_sample_rate_hz must divide ${rate} Hz exactly`);
      if (width >= output) errors.push('occupied_bandwidth_hz must be smaller than output_sample_rate_hz');
      if (Math.abs(frequency - center) + width / 2 >= rate / 2)
        errors.push('frequency_hz and occupied bandwidth must fit inside the captured passband');
      if (taps < 4 || taps > 128) errors.push('filter_taps_per_phase must be between 4 and 128');
      if (rate % output === 0 && taps * (rate / output) + 1 > 131071)
        errors.push('full-quality channel filter exceeds 131,071 taps');
      for (const other of config.process?.multi_illuminator?.illuminators || []) {
        if (other?.enabled !== true || other.id === lane.id) continue;
        if (Math.abs(frequency - other.frequency_hz) < (width + other.occupied_bandwidth_hz) / 2)
          errors.push(`occupied passband overlaps enabled channel ${other.id}; choose non-overlapping waveform bands`);
      }
    }
    const requestedTaps = taps * (rate / output) + 1;
    return {errors, rate, center, lowerHz: frequency - width / 2,
      upperHz: frequency + width / 2, captureLowerHz: center - rate / 2,
      captureUpperHz: center + rate / 2, transitionHz: (output - width) / 2,
      decimation: rate / output, filterTaps: requestedTaps % 2 === 0 ? requestedTaps + 1 : requestedTaps};
  }

  function serializeConfig(value) {
    if (Array.isArray(value)) return `[${value.map(serializeConfig).join(',')}]`;
    if (object(value))
      return `{${Object.keys(value).sort().map(key => `${JSON.stringify(key)}:${serializeConfig(value[key])}`).join(',')}}`;
    return JSON.stringify(value);
  }

  function displayOnlyChange(before, after) {
    if (!before || !after) return false;
    const {display: previous, ...oldConfig} = before, {display: next, ...newConfig} = after;
    return serializeConfig(previous) !== serializeConfig(next) &&
      serializeConfig(oldConfig) === serializeConfig(newConfig);
  }

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
      minSnrDb: 7, deleteAfter: 3, accelerationModel: 'hypothesis_bank'})
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
    return 'custom';
  }

  function applyClutterMode(config, mode) {
    const clutter = ensure(ensure(config, 'process'), 'clutter');
    clutter.enable = mode !== 'off';
    if (mode === 'automatic') clutter.followAmbiguity = true;
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
    initiate.acceleration_model = values.accelerationModel;
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

  // Admission mirror of process/tracker/ConfirmationContract.h. N counts CPI
  // endpoints, not overlapping detections. Presets never widen these limits.
  function temporalConfirmationRequirements(config) {
    const tracker = config?.process?.tracker;
    if (tracker?.enable !== true) return null;
    const detection = config.process.detection || {}, tbd = detection.track_before_detect;
    const depth = tbd?.enable === true ? (tbd.max_cpis ?? 8) : (detection.integration_cpis ?? 1);
    const {M, N} = tracker.initiate || {}, misses = tracker.delete;
    if (![1, 2, 4, 8].includes(depth) || ![M, N, misses].every(Number.isInteger) || M < 1)
      return null; // Field validators report malformed values.
    const minimumWindow = 1 + depth * (M - 1), minimumMisses = depth - 1;
    return {depth, M, N, misses, minimumWindow, minimumMisses};
  }

  function temporalConfirmationErrors(config) {
    const required = temporalConfirmationRequirements(config);
    if (!required) return [];
    const {depth, M, N, misses, minimumWindow, minimumMisses} = required;
    return N >= minimumWindow && misses >= minimumMisses ? [] : [
      `process.tracker.initiate.N must be >= ${minimumWindow} and process.tracker.delete must be >= ${minimumMisses} ` +
      `for disjoint ${depth}-CPI confirmation (M=${M}; N >= 1 + depth*(M-1)). ` +
      'Qualify initiation latency, continuity and false tracks in replay before changing these limits.'];
  }

  function applyWeakTargetMode(config, mode) {
    const detection = ensure(ensure(config, 'process'), 'detection');
    const tbd = ensure(detection, 'track_before_detect');
    detection.integration_cpis = mode === 'off' ? 1 : 2;
    detection.integration_motion_bins = mode === 'two_motion' ? 1 : 0;
    tbd.enable = ['four', 'eight'].includes(mode);
    if (mode === 'four') tbd.max_cpis = 4;
    if (mode === 'eight') tbd.max_cpis = 8;
    if (!tbd.enable) {
      if (tbd.fractional_delay !== undefined) tbd.fractional_delay = false;
    }
    // Adjacent-CPI integration is supported by CPU and portable GPU paths too.
    // Two-frame presets must not silently switch an FP32 machine to resident.
    return config;
  }

  function processingMode(config) {
    return config?.process?.performance?.acceleration === 'cpu' ? 'cpu' : 'automatic';
  }

  function associationGates(span, prediction, timestamp, kind = 'detection',
    eventTimeUncertaintyS = 0, timingContext = null) {
    if (span.truthUncertaintyPolicy) {
      const model = prediction?.truth_uncertainty;
      const invalid = reason => ({shape: 'ellipse', valid: false, reason,
        delayKm: 0, dopplerHz: 0, evaluationOnly: true});
      if (!model?.valid || model.schema !== 'vectorwarp-truth-uncertainty-v1')
        return invalid(model?.reason || 'truth-uncertainty-unavailable');
      if (timingContext?.declared !== true || !finite(eventTimeUncertaintyS) || eventTimeUncertaintyS < 0)
        return invalid('frame-alignment-model-unavailable');
      const age = timestamp / 1000 - prediction.timestamp;
      if (!finite(age) || Math.abs(age) > .001) return invalid('truth-not-at-requested-frame');
      const offset = timingContext.frameOffsetS ?? 0;
      if (!finite(offset) || Math.abs(offset) > .1) return invalid('radar-frame-offset');
      const covariance = model.covariance;
      if (!Array.isArray(covariance) || covariance.length !== 4 || !covariance.every(finite))
        return invalid('invalid-covariance');
      const cpi = span.cpiSeconds, delayResolution = span.kmPerBin;
      const dopplerResolution = span.dopplerResolutionHz;
      if (![cpi, delayResolution, dopplerResolution].every(value => finite(value) && value > 0))
        return invalid('invalid-radar-resolution');
      // Resolution and the CPI integration aperture are not observation age.
      // Uniform alignment within the aperture supplies T^2/12. A declared
      // external alignment bound is conservatively treated as one sigma.
      const timeVariance = cpi * cpi / 12 + (eventTimeUncertaintyS + Math.abs(offset)) ** 2;
      const delayRate = model.delay_rate_km_s;
      const dopplerRate = model.geometric_doppler_rate_hz_s;
      const dopplerRateBound = model.doppler_rate_bound_hz_s;
      if (![delayRate, dopplerRate, dopplerRateBound].every(finite)) return invalid('motion-rate-unavailable');
      const a = covariance[0] + delayResolution ** 2 / 12 + timeVariance * delayRate ** 2;
      const b = .5 * (covariance[1] + covariance[2]) + timeVariance * delayRate * dopplerRate;
      const d = covariance[3] + dopplerResolution ** 2 / 12 + timeVariance * dopplerRateBound ** 2;
      const determinant = a * d - b * b, maximumNis = 9.210340371976184;
      if (!(a > 0 && d > 0 && determinant > 0) || !finite(determinant)) return invalid('invalid-covariance');
      const delayKm = Math.sqrt(maximumNis * a), dopplerHz = Math.sqrt(maximumNis * d);
      const area = Math.PI * maximumNis * Math.sqrt(determinant);
      // Do not clip an unbounded/too-wide ellipse into a deceptively valid one.
      if (delayKm > Math.max(1, delayResolution * 8) ||
          dopplerHz > Math.max(25, dopplerResolution * 12) ||
          area / (delayResolution * dopplerResolution) > span.truthUncertaintyPolicy.maxAreaBins)
        return invalid('uncertainty-exceeds-hard-cap');
      return {shape: 'ellipse', valid: true, delayKm, dopplerHz, covariance: [a, b, b, d],
        determinant, maximumNis, areaKmHz: area, evaluationOnly: true,
        nullCalibrated: false, eventTimeUncertaintyS, timingModel: timingContext.model};
    }
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
    const uncertaintyContenders = new Map();
    for (const point of points) {
      if (diagnostics) diagnostics.points = (diagnostics.points || 0) + 1;
      const candidates = predictions.map(([id, prediction]) => {
        const gates = gatesForPrediction(prediction);
        const delayError = Math.abs(point.delay - prediction.delay);
        const dopplerError = Math.abs(point.doppler - prediction.doppler);
        const dx = point.delay - prediction.delay, dy = point.doppler - prediction.doppler;
        const covariance = gates.covariance;
        return {id, delayError, dopplerError, gates,
          score: gates.shape === 'ellipse' ? (gates.valid ?
            (covariance[3] * dx * dx - 2 * covariance[1] * dx * dy + covariance[0] * dy * dy) /
              gates.determinant / gates.maximumNis : Infinity) :
            (delayError / gates.delayKm) ** 2 + (dopplerError / gates.dopplerHz) ** 2};
      }).filter(item => finite(item.score) && item.gates.valid !== false &&
        item.delayError <= item.gates.delayKm && item.dopplerError <= item.gates.dopplerHz &&
        (item.gates.shape !== 'ellipse' || item.score <= 1))
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
      if (selected.gates.shape === 'ellipse') {
        const contenders = uncertaintyContenders.get(selected.id) || [];
        contenders.push(selected.score); uncertaintyContenders.set(selected.id, contenders);
      }
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
        ...(selected.gates.shape === 'ellipse' ? {association_family: 'uncertainty-ellipse-v1',
          evaluation_only: true, null_calibrated: false,
          timing_model: selected.gates.timingModel,
          gate_covariance: selected.gates.covariance, gate_area_km_hz: selected.gates.areaKmHz} : {}),
        normalized_gate_score: selected.score});
      if (diagnostics) diagnostics.selected_points = (diagnostics.selected_points || 0) + 1;
    }
    // Mutual uniqueness: an uncertain aircraft cannot select arbitrarily among
    // equally plausible radar peaks. Each point was already assigned to at most
    // one aircraft above; each aircraft retains at most one unambiguous point.
    for (const [id, scores] of uncertaintyContenders) {
      scores.sort((a, b) => a - b);
      if (scores.length > 1 && scores[1] <= scores[0] * 1.15 + .05) {
        result.delete(id);
        if (diagnostics) diagnostics.ambiguous_truths = (diagnostics.ambiguous_truths || 0) + 1;
      }
    }
    return result;
  }

  function applyProcessingMode(config, mode) {
    const pipeline = ensure(config, 'process');
    const performance = ensure(pipeline, 'performance');
    // Execution policy never substitutes a statistic, topology or stage.
    // The shared capability validator refuses an unavailable implementation.
    const needsResident = pipeline.bearing?.enable === true ||
      pipeline.surveillance_fusion?.mode === 'noise_whitened_beam_bank' ||
      (pipeline.tracker?.beam_tolerance !== undefined &&
        ((pipeline.detection?.integration_cpis ?? 1) > 1 ||
          pipeline.detection?.track_before_detect?.enable === true));
    performance.acceleration = mode === 'cpu' ? 'cpu' : needsResident ? 'gpu-resident' : 'auto';
    return config;
  }

  function applyReceiverProfile(config, profile, initializeProcessing = false) {
    const capture = ensure(config, 'capture'), geometry = capture.device?.array_geometry;
    capture.device = JSON.parse(JSON.stringify(profile.device));
    if (geometry !== undefined) capture.device.array_geometry = geometry;
    if (capture.replay?.state === true) return config;
    capture.fs = profile.sampleRate;
    if (initializeProcessing) {
      const process = ensure(config, 'process');
      for (const [key, value] of Object.entries(profile.process || {}))
        process[key] = {...process[key], ...JSON.parse(JSON.stringify(value))};
    }
    return config;
  }

  return {serializeConfig, displayOnlyChange, scaledInputNumber, channelizerStandards, channelizerPresets,
    channelizerPreview, scaleDelayBin, effectiveDelayConfig, legacyDelayReferenceRate,
    detectionProfiles, trackingProfiles, gainMode, applyGainMode, clutterMode, applyClutterMode,
    detectionSensitivity, applyDetectionSensitivity, weakTargetMode, applyWeakTargetMode,
    temporalConfirmationErrors, temporalConfirmationRequirements,
    applyTrackingProfile, processingMode, applyProcessingMode, applyReceiverProfile, associationGates, assignments};
});
