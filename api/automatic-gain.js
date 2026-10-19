'use strict';

const clone = value => JSON.parse(JSON.stringify(value));
const liveKraken = config => config?.capture?.device?.type === 'Kraken' &&
  config?.capture?.replay?.state !== true;
const enabled = config => liveKraken(config) &&
  config.capture.device.heimdall?.gain_control === 'measured';
const fixedGain = gain => typeof gain === 'number' && Number.isFinite(gain) &&
  gain >= 0 && gain <= 50 && Math.round(gain * 10) === gain * 10;
const MAX_AUTOMATIC_GAIN_MS = 1235000;
const LIMITS = Object.freeze({maxTrialSettings: 16, maxProbeCaptures: 32,
  gainMinDb: 0, gainMaxDb: 50, commonRequestedGainStepDb: 1,
  railFraction: 0, peakFraction: .95, p999Fraction: .70,
  increaseBelowPeakFraction: .80, increaseBelowP999Fraction: .60,
  finalPassingProbes: 2, probeSeconds: .25, probeTimeoutMs: 15000,
  synchronizationTimeoutMs: 75000, settlingTimeoutMs: 15000, recalibrationTimeoutMs: 150000,
  selectionTimeoutMs: 900000, restoreTimeoutMs: 315000,
  maxDurationMs: MAX_AUTOMATIC_GAIN_MS});
const CLAIMS = Object.freeze({
  measured: 'Headroom of delivered calibrated wire samples at the selected common Suite gain.',
  rawAdcClippingProven: false, radarQualificationProven: false,
  hardwareGainStepsVerified: false,
  controlCalibration: 'Acknowledged full reset and observed receiver recovery; optional status flags are checked when reported.',
  wireCalibration: 'Every measured MCHQ packet reports phase state 4, noise off and no retuning.',
  limitation: 'Corrected wire samples do not prove raw ADC headroom, receiver sensitivity, or radar performance.'
});

function failure(code, message, status = 409) {
  const error = new Error(message); error.code = code; error.status = status; return error;
}

function assertTuple(receipt, config, {requireIdle = true} = {}) {
  const after = receipt?.after;
  const wanted = config.capture;
  const gain = wanted.device.heimdall.gain;
  if (!after || after.centerFrequency !== wanted.fc || after.sampleRate !== wanted.fs ||
      after.channelCount !== wanted.device.channel_count || after.operatingMode !== 'coherent' ||
      after.reconfiguring !== false || (requireIdle && (after.recovering !== false ||
      after.cooldownActive !== false)) || !fixedGain(after.gain) ||
      (gain !== 'keep' && gain !== undefined && after.gain !== gain))
    throw failure('AUTOMATIC_GAIN_RECEIVER_MISMATCH',
      'Measured gain requires a matching idle coherent receiver with a reported fixed gain between 0 and 50 dB.');
  return after;
}

function assertCalibration(receipt, config) {
  assertTuple(receipt, config);
  if (receipt.status !== 'recalibrated' || !receipt.operations?.some(operation => {
    const full = operation.fullRecalibration;
    return operation.commandSent === true && operation.acknowledged === true &&
      operation.readbackMatched === true && full?.commandSent === true &&
      full.acknowledged === true && full.recoveryObserved === true && full.completed === true &&
      (full.phaseState == null || full.phaseState === 4) &&
      (full.calibrationState == null || full.calibrationState === 'CONVERGED') &&
      (full.noiseSource == null || full.noiseSource === false);
  })) throw failure('AUTOMATIC_GAIN_CALIBRATION_UNVERIFIED',
    'Measured gain requires acknowledged full lag and phase recalibration with observed recovery and valid reported completion flags.');
}

function validateProbe(probe, config) {
  const capture = config.capture;
  const count = capture.device.channel_count;
  if (!probe || probe.frequencyHz !== capture.fc || probe.sampleRateHz !== capture.fs ||
      probe.channelCount !== count || probe.gainDb !== capture.device.heimdall.gain ||
      !Array.isArray(probe.channels) || probe.channels.length !== count)
    throw failure('AUTOMATIC_GAIN_PROBE_MISMATCH',
      'Gain measurement did not match the requested frequency, sample rate, channel count and fixed gain.');
  const calibration = probe.calibration;
  if (probe.source !== 'calibrated_mchq_uint8_wire' || !Number.isSafeInteger(probe.packets) ||
      probe.packets < 1 || calibration?.phaseState !== 4 || calibration.noiseSource !== false ||
      calibration.retuning !== false || calibration.verifiedPackets !== probe.packets)
    throw failure('AUTOMATIC_GAIN_UNCALIBRATED_PROBE',
      'Gain measurement requires calibrated MCHQ header evidence for every captured packet.');
  const seen = new Set();
  const channels = probe.channels.map(channel => {
    if (!channel || !Number.isInteger(channel.channel) || channel.channel < 0 ||
        channel.channel >= count || seen.has(channel.channel) ||
        !Number.isSafeInteger(channel.samples) || channel.samples < 1 ||
        channel.samples !== probe.channels[0].samples ||
        !['railFraction', 'peakFraction', 'p999Fraction', 'rmsFraction'].every(key =>
          typeof channel[key] === 'number' && Number.isFinite(channel[key]) && channel[key] >= 0) ||
        channel.railFraction > 1 || channel.p999Fraction > channel.peakFraction ||
        channel.rmsFraction > channel.peakFraction)
      throw failure('AUTOMATIC_GAIN_INVALID_PROBE',
        'Gain measurement must include valid finite headroom metrics for every active channel.');
    seen.add(channel.channel);
    return {channel: channel.channel, samples: channel.samples, railFraction: channel.railFraction,
      peakFraction: channel.peakFraction, p999Fraction: channel.p999Fraction,
      rmsFraction: channel.rmsFraction};
  });
  const safe = channels.every(channel => channel.railFraction === 0 &&
    channel.peakFraction <= LIMITS.peakFraction && channel.p999Fraction <= LIMITS.p999Fraction);
  return {frequencyHz: probe.frequencyHz, sampleRateHz: probe.sampleRateHz,
    channelCount: count, gainDb: probe.gainDb, channels, safe,
    source: probe.source, packets: probe.packets,
    calibration: {phaseState: 4, noiseSource: false, retuning: false, verifiedPackets: probe.packets},
    worstP999Fraction: Math.max(...channels.map(channel => channel.p999Fraction)),
    railsObserved: channels.some(channel => channel.railFraction > 0)};
}

function createAutomaticGain({synchronizer, measureGain, waitForGainSettled, now = Date.now} = {}) {
  if (!synchronizer || !['verify', 'synchronize', 'recalibrate'].every(key =>
    typeof synchronizer[key] === 'function') || typeof measureGain !== 'function' ||
      typeof waitForGainSettled !== 'function' || typeof now !== 'function')
    throw new TypeError('Measured gain requires receiver verification, synchronization, stream settling, recalibration and measurement.');

  async function apply(previous, candidate, {increase = true} = {}) {
    let restoreConfig, attemptedConfig, rollbackPromise, mutated = false;
    const selectionDeadline = now() + LIMITS.selectionTimeoutMs;
    let restoreDeadline;
    const config = clone(candidate);
    const receiverSync = {schemaVersion: 1, receiverType: 'Kraken', status: 'pending',
      operations: [], calibrationVerified: false, hardwareVerified: false,
      automaticGain: {status: 'measuring', probes: [], settling: [], trials: [], selectedGainDb: null,
        limits: {...LIMITS}, claims: {...CLAIMS}}};
    const automatic = receiverSync.automaticGain;
    // Dependencies own their bounded I/O and cancellation. Never race an active
    // write against a timer: rollback starts only after that operation settles.
    // Selection has a 15-minute budget and at most sixteen trial settings.
    // Restoration has a separate 315-second budget: sync75s + settle15s +
    // cal150s + verify75s. Each stage reserves its entire bound before it may start.
    function requireBudget(name, milliseconds, restoring = false) {
      if (now() + milliseconds > (restoring ? restoreDeadline : selectionDeadline))
        throw failure(restoring ? 'AUTOMATIC_GAIN_RESTORE_DEADLINE' : 'AUTOMATIC_GAIN_DEADLINE',
          `Insufficient remaining measured-gain time for ${name}.`, 504);
    }
    async function stage(name, action, trial = null) {
      function record(receipt) {
        if (!receipt) return;
        if (receipt.after) receiverSync.after = clone(receipt.after);
        for (const operation of receipt.operations || [])
          receiverSync.operations.push({...clone(operation), automaticGainStage: name, trial});
      }
      requireBudget(name, name.includes('recalibrate') ? LIMITS.recalibrationTimeoutMs :
        LIMITS.synchronizationTimeoutMs, name.startsWith('restore-'));
      try { const receipt = await action(); record(receipt); return receipt; }
      catch (error) { record(error.receiverSync); throw error; }
    }
    async function settle(name, candidate, synchronized, trial = null) {
      const requireTransition = synchronized.operations?.some(operation => operation.commandSent &&
        ['set_gain', 'set_frequency', 'set_num_elements'].includes(operation.operation)) === true;
      requireBudget(name, LIMITS.settlingTimeoutMs, name.startsWith('restore-'));
      try {
        // Gain changes initiate Suite phase calibration independently of its
        // TCP recovery flag. Wait for its coherent output before another reset.
        // These samples are readiness evidence, never a headroom acceptance.
        const result = await waitForGainSettled(clone(candidate), {
          captureSeconds: LIMITS.probeSeconds, timeoutMs: LIMITS.settlingTimeoutMs, requireTransition
        });
        const measured = validateProbe(result, candidate);
        if (requireTransition && result.settling?.transitionObserved !== true)
          throw failure('AUTOMATIC_GAIN_SETTLING_UNVERIFIED',
            'A changed receiver setting requires a fresh non-ready to coherent stream transition before recalibration.');
        automatic.settling.push({...measured, requireTransition,
          transitionObserved: result.settling?.transitionObserved === true, stage: name, trial, purpose: 'pre-reset-readiness'});
      } catch (error) {
        automatic.settling.push({stage: name, trial, status: 'failed',
          code: error.code || 'AUTOMATIC_GAIN_SETTLING_FAILED', message: error.message});
        throw error;
      }
    }
    async function rollback() {
      if (rollbackPromise) return rollbackPromise;
      rollbackPromise = (async () => {
        if (!mutated || !restoreConfig) {
          automatic.rollback = {status: 'not-required'};
          return receiverSync;
        }
        restoreDeadline = now() + LIMITS.restoreTimeoutMs;
        automatic.rollback = {status: 'restoring', gainDb: restoreConfig.capture.device.heimdall.gain};
        try {
          const synchronized = await stage('restore-synchronize', () =>
            synchronizer.synchronize(attemptedConfig, restoreConfig, {force: true}));
          assertTuple(synchronized, restoreConfig, {requireIdle: false});
          await settle('restore-settle', restoreConfig, synchronized);
          assertCalibration(await stage('restore-recalibrate', () =>
            synchronizer.recalibrate(restoreConfig)), restoreConfig);
          assertTuple(await stage('restore-verify', () => synchronizer.verify(restoreConfig)), restoreConfig);
          automatic.rollback.status = 'restored';
          automatic.status = 'rolled-back'; automatic.selectedGainDb = null;
          receiverSync.status = 'restored';
          return receiverSync;
        } catch (error) {
          automatic.rollback = {...automatic.rollback, status: 'failed',
            code: error.code || 'AUTOMATIC_GAIN_RESTORE_FAILED', message: error.message};
          automatic.status = 'restore-failed'; receiverSync.status = 'indeterminate';
          error.receiverSync = receiverSync;
          throw error;
        }
      })();
      return rollbackPromise;
    }
    async function probe(trial) {
      try {
        requireBudget('probe', LIMITS.probeTimeoutMs);
        const measurement = validateProbe(await measureGain(clone(config)), config);
        automatic.probes.push({...measurement, trial, capture: automatic.probes.length + 1});
        return measurement;
      } catch (error) {
        automatic.probes.push({trial, capture: automatic.probes.length + 1, status: 'failed',
          code: error.code || 'AUTOMATIC_GAIN_MEASUREMENT_FAILED', message: error.message});
        throw error;
      }
    }

    try {
      if (!enabled(candidate) || !liveKraken(previous))
        throw failure('AUTOMATIC_GAIN_UNAVAILABLE', 'Measured gain requires an existing live Kraken configuration.');
      const oldEndpoint = previous.capture.device.heimdall || {};
      const endpoint = config.capture.device.heimdall;
      if (oldEndpoint.host !== endpoint.host || (oldEndpoint.control_port ?? 8092) !==
          (endpoint.control_port ?? 8092) || oldEndpoint.port !== endpoint.port)
        throw failure('AUTOMATIC_GAIN_ENDPOINT_CHANGED',
          'Apply receiver endpoint changes before starting measured gain.');
      if (endpoint.gain !== undefined && endpoint.gain !== 'keep' && !fixedGain(endpoint.gain))
        throw failure('AUTOMATIC_GAIN_INVALID_GAIN', 'Measured gain accepts a fixed common gain or keep; independent AGC is unsupported.');
      const before = await stage('verify-before', () => synchronizer.verify(previous));
      const actual = assertTuple(before, previous);
      receiverSync.before = clone(actual);
      receiverSync.endpoint = clone(before.endpoint || {host: endpoint.host,
        controlPort: endpoint.control_port ?? 8092});
      if (before.acceptance) receiverSync.acceptance = clone(before.acceptance);
      restoreConfig = clone(previous);
      restoreConfig.capture.device.heimdall.gain = actual.gain;
      automatic.gainSteps = {source: 'Suite common gain requests',
        commonRequestedGainStepDb: LIMITS.commonRequestedGainStepDb,
        hardwareGainStepsVerified: false,
        limitation: 'Suite has not advertised a verified tuner gain table; a 1 dB request is not a guaranteed physical gain change.'};
      let trialGain = fixedGain(endpoint.gain) ? endpoint.gain : actual.gain;
      let lastSafeGain = null, seekingDown = false;
      for (let trial = 1; trial <= LIMITS.maxTrialSettings; trial++) {
        config.capture.device.heimdall.gain = trialGain;
        attemptedConfig = clone(config);
        automatic.trials.push({trial, gainDb: trialGain});
        const synchronized = await stage('trial-synchronize', () => {
          mutated = true; // A failed synchronization may still have changed hardware.
          return synchronizer.synchronize(restoreConfig, clone(config), {force: true});
        }, trial);
        assertTuple(synchronized, config, {requireIdle: false});
        await settle('trial-settle', config, synchronized, trial);
        assertCalibration(await stage('trial-recalibrate', () =>
          synchronizer.recalibrate(clone(config)), trial), config);
        let measurement = await probe(trial);
        if (measurement.safe) {
          const clearHeadroom = measurement.channels.every(channel =>
            channel.peakFraction < LIMITS.increaseBelowPeakFraction &&
            channel.p999Fraction < LIMITS.increaseBelowP999Fraction);
          if (increase && !seekingDown && clearHeadroom && trialGain < LIMITS.gainMaxDb) {
            // Keep room to return to this measured safe setting if the next
            // smallest request is unsafe. Budget exhaustion is not a peak claim.
            if (trial + 2 > LIMITS.maxTrialSettings) break;
            lastSafeGain = trialGain;
            trialGain = Math.min(LIMITS.gainMaxDb,
              Math.round((trialGain + LIMITS.commonRequestedGainStepDb) * 10) / 10);
            continue;
          }
          measurement = await probe(trial);
          if (measurement.safe) {
            assertTuple(await stage('verify-selected', () =>
              synchronizer.verify(clone(config)), trial), config);
            automatic.status = 'verified'; automatic.selectedGainDb = trialGain;
            automatic.selectionReason = seekingDown ? 'safe-below-measured-unsafe-setting' :
              !increase ? 'backoff-only' : trialGain === LIMITS.gainMaxDb ?
                'maximum-requested-gain' : 'headroom-target';
            automatic.finalPassingProbes = 2; receiverSync.status = 'synchronized';
            receiverSync.requested = {frequency: config.capture.fc, sampleRate: config.capture.fs,
              channelCount: config.capture.device.channel_count, gain: trialGain};
            return {config: clone(config), receiverSync, rollback};
          }
        }
        if (trialGain === 0)
          throw failure('AUTOMATIC_GAIN_UNSAFE_AT_MINIMUM', 'Delivered samples exceed headroom limits at zero common gain.');
        seekingDown = true;
        if (lastSafeGain !== null && lastSafeGain < trialGain) trialGain = lastSafeGain;
        else trialGain = Math.max(0,
          Math.round((trialGain - LIMITS.commonRequestedGainStepDb) * 10) / 10);
      }
      throw failure('AUTOMATIC_GAIN_NO_SAFE_SETTING',
        'No gain setting met the search stopping rule and two independent headroom checks within sixteen trials.');
    } catch (error) {
      automatic.failure = {code: error.code || 'AUTOMATIC_GAIN_FAILED', message: error.message};
      try {
        await rollback();
        automatic.status = automatic.rollback.status === 'restored' ? 'failed-restored' : 'failed';
        receiverSync.status = automatic.rollback.status === 'restored' ? 'restored' : 'failed';
      } catch (_) { /* Preserve the original failure and the explicit failed restore receipt. */ }
      error.receiverSync = receiverSync;
      throw error;
    }
  }
  return {apply};
}

module.exports = {createAutomaticGain, enabled, MAX_AUTOMATIC_GAIN_MS, LIMITS, validateProbe};
