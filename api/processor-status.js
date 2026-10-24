'use strict';

const RECEIVERS = new Set(['Kraken', 'RspDuo', 'Usrp', 'HackRF']);
const STATES = new Set(['loading', 'playing', 'draining', 'complete', 'stopped', 'error', 'live']);
const MAX_TEXT = 4096;
const MAX_ERROR_TEXT = 512;
const MAX_SAFE = Number.MAX_SAFE_INTEGER;
const object = value => value !== null && typeof value === 'object' && !Array.isArray(value);

function text(value, name, errors, required = true) {
  if (value === undefined && !required) return '';
  if (typeof value !== 'string' || value.length > MAX_TEXT || /[\u0000-\u001f]/.test(value)) {
    errors.push(`${name} must be a short printable string`);
    return '';
  }
  return value;
}
function count(value, name, errors) {
  if (!Number.isSafeInteger(value) || value < 0 || value > MAX_SAFE)
    errors.push(`${name} must be a non-negative safe integer`);
  return Number.isSafeInteger(value) && value >= 0 ? value : 0;
}
function errorText(value, name, errors) {
  if (typeof value !== 'string') {
    errors.push(`${name} must be a string`);
    return '';
  }
  // Exception text can contain newlines. Preserve its meaning without allowing
  // unbounded/control text into status labels or logs.
  return value.replace(/[\u0000-\u001f\u007f]+/g, ' ').replace(/\s+/g, ' ')
    .trim().slice(0, MAX_ERROR_TEXT);
}
function positive32(value, name, errors, failed = false) {
  // A failed native startup may be reporting the invalid zero from a hand-edited
  // config. Preserve its error instead of rejecting the entire status update.
  const minimum = failed ? 0 : 1;
  if (!Number.isSafeInteger(value) || value < minimum || value > 4294967295)
    errors.push(`${name} must be a ${failed ? 'non-negative' : 'positive'} 32-bit integer`);
  return Number.isSafeInteger(value) && value >= minimum && value <= 4294967295 ? value : 0;
}
const RSP_STAGES = ['open', 'apiVersion', 'lock', 'enumerate', 'select',
  'unlock', 'debugEnable', 'getDeviceParams', 'init', 'gainUpdateA', 'gainUpdateB'];
function rspStartup(payload, outer, errors) {
  const name = 'receiverStartup';
  if (outer.input !== 'live' || outer.receiver !== 'RspDuo' ||
      !payload || typeof payload !== 'object' || Array.isArray(payload)) {
    errors.push(`${name} requires a live RspDuo object`); return null;
  }
  const requested = payload.requested;
  const selected = payload.selected;
  const sdk = payload.sdk;
  const stages = sdk?.stages;
  if (payload.schema !== 1 || payload.receiver !== 'RspDuo' ||
      !['pending', 'accepted'].includes(payload.status) ||
      payload.hardwareVerified !== false || payload.readbackAvailable !== false ||
      !requested || typeof requested !== 'object' || Array.isArray(requested) ||
      !sdk || typeof sdk !== 'object' || Array.isArray(sdk) ||
      !stages || typeof stages !== 'object' || Array.isArray(stages)) {
    errors.push(`${name} has an invalid schema`); return null;
  }
  const serial = text(requested.serial, `${name}.requested.serial`, errors);
  const frequency = positive32(requested.frequency, `${name}.requested.frequency`, errors);
  const sampleRate = positive32(requested.sampleRate, `${name}.requested.sampleRate`, errors);
  const integer = (value, field, min, max) => {
    if (!Number.isInteger(value) || value < min || value > max)
      errors.push(`${name}.${field} is invalid`);
    return value;
  };
  const agcSetPoint = integer(requested.agcSetPoint, 'requested.agcSetPoint', -72, 0);
  const bandwidthNumber = integer(requested.bandwidthNumber, 'requested.bandwidthNumber', 0, 100);
  if (![0, 5, 50, 100].includes(bandwidthNumber)) errors.push(`${name}.requested.bandwidthNumber is unsupported`);
  const gainReduction = requested.gainReduction;
  if (!Array.isArray(gainReduction) || gainReduction.length !== 2) errors.push(`${name}.requested.gainReduction requires two values`);
  else gainReduction.forEach((gain, index) => integer(gain, `requested.gainReduction[${index}]`, 20, 59));
  const lnaState = integer(requested.lnaState, 'requested.lnaState', 0, 9);
  const maxLna = frequency < 60000000 ? 6 : frequency < 1000000000 ? 9 : 8;
  if (lnaState > maxLna) errors.push(`${name}.requested.lnaState exceeds the band limit`);
  for (const field of ['dabNotch', 'rfNotch'])
    if (typeof requested[field] !== 'boolean') errors.push(`${name}.requested.${field} must be boolean`);
  const ifBandwidthKhz = integer(requested.ifBandwidthKhz, 'requested.ifBandwidthKhz', 1, 10000);
  const ifFrequencyKhz = integer(requested.ifFrequencyKhz, 'requested.ifFrequencyKhz', 0, 10000);
  const decimation = integer(requested.decimation, 'requested.decimation', 1, 32);
  if (![1, 2, 4, 8, 16, 32].includes(decimation)) errors.push(`${name}.requested.decimation is unsupported`);
  const outputModes = new Map([[2000000, [1, 1536]], [1000000, [2, 600]],
    [500000, [4, 300]], [250000, [8, 200]], [125000, [16, 200]],
    [62500, [32, 200]]]);
  const mode = outputModes.get(sampleRate);
  if (!mode || mode[0] !== decimation || mode[1] !== ifBandwidthKhz || ifFrequencyKhz !== 1620)
    errors.push(`${name}.requested IF and decimation do not match output rate`);
  if (frequency !== outer.frequency || sampleRate !== outer.sampleRate)
    errors.push(`${name}.requested frequency and sample rate must match processor status`);
  if (selected !== null && (!selected || typeof selected !== 'object' || Array.isArray(selected)))
    errors.push(`${name}.selected must be an object or null`);
  if (selected && typeof selected === 'object' && !Array.isArray(selected)) {
    const selectedSerial = text(selected.serial, `${name}.selected.serial`, errors);
    if (!selectedSerial || (serial && serial !== selectedSerial)) errors.push(`${name}.selected.serial does not match request`);
    count(selected.deviceIndex, `${name}.selected.deviceIndex`, errors);
    count(selected.hardwareVersion, `${name}.selected.hardwareVersion`, errors);
    if (selected.tuner !== 'Both' || selected.mode !== 'Dual_Tuner')
      errors.push(`${name}.selected has an unsupported mode`);
  }
  for (const stage of RSP_STAGES)
    if (typeof stages[stage] !== 'boolean') errors.push(`${name}.sdk.stages.${stage} must be boolean`);
  if (stages.select !== (selected !== null)) errors.push(`${name}.selected and select stage disagree`);
  if (sdk.version !== null && (typeof sdk.version !== 'number' ||
      !Number.isFinite(sdk.version) || sdk.version <= 0 || sdk.version > 100))
    errors.push(`${name}.sdk.version is invalid`);
  if (stages.apiVersion && sdk.version === null)
    errors.push(`${name}.sdk.version is required after the version check`);
  const allAccepted = RSP_STAGES.every(stage => stages[stage] === true);
  if ((payload.status === 'accepted') !== allAccepted)
    errors.push(`${name}.status disagrees with SDK stages`);
  if (payload.status === 'accepted' && !selected)
    errors.push(`${name}.accepted requires a selected receiver`);
  return {schema: 1, receiver: 'RspDuo', status: payload.status,
    hardwareVerified: false, readbackAvailable: false,
    requested: {serial, frequency, sampleRate, agcSetPoint, bandwidthNumber,
      gainReduction, lnaState, dabNotch: requested.dabNotch, rfNotch: requested.rfNotch,
      ifBandwidthKhz, ifFrequencyKhz, decimation},
    selected: selected ? {serial: selected.serial, deviceIndex: selected.deviceIndex,
      hardwareVersion: selected.hardwareVersion, tuner: 'Both', mode: 'Dual_Tuner'} : null,
    sdk: {version: sdk.version, stages: Object.fromEntries(RSP_STAGES.map(stage => [stage, stages[stage]]))}};
}
function rspHealth(payload, outer, errors) {
  const name = 'receiverHealth';
  if (outer.input !== 'live' || outer.receiver !== 'RspDuo' || !object(payload)) {
    errors.push(`${name} requires a live RspDuo object`); return null;
  }
  if (payload.schema !== 1 || payload.receiver !== 'RspDuo' ||
      typeof payload.monitoring !== 'boolean' || !object(payload.overload)) {
    errors.push(`${name} has an invalid schema`); return null;
  }
  if (outer.state === 'stopped' && payload.monitoring)
    errors.push(`${name}.monitoring must be false after stop`);
  const overload = {};
  for (const tuner of ['A', 'B']) {
    const source = payload.overload[tuner];
    const field = `${name}.overload.${tuner}`;
    if (!object(source)) {
      errors.push(`${field} must be an object`); continue;
    }
    if (typeof source.active !== 'boolean') errors.push(`${field}.active must be boolean`);
    const detectedCount = count(source.detectedCount, `${field}.detectedCount`, errors);
    const correctedCount = count(source.correctedCount, `${field}.correctedCount`, errors);
    const lastDetectedAt = count(source.lastDetectedAt, `${field}.lastDetectedAt`, errors);
    const lastCorrectedAt = count(source.lastCorrectedAt, `${field}.lastCorrectedAt`, errors);
    if (correctedCount > detectedCount) errors.push(`${field}.correctedCount exceeds detectedCount`);
    if (detectedCount - correctedCount !== (source.active ? 1 : 0))
      errors.push(`${field}.active disagrees with overload counts`);
    if ((detectedCount === 0) !== (lastDetectedAt === 0))
      errors.push(`${field}.lastDetectedAt disagrees with detectedCount`);
    if ((correctedCount === 0) !== (lastCorrectedAt === 0))
      errors.push(`${field}.lastCorrectedAt disagrees with correctedCount`);
    overload[tuner] = {active: source.active, detectedCount, correctedCount,
      lastDetectedAt, lastCorrectedAt};
  }
  let gain;
  if (payload.gain !== undefined) {
    const source = payload.gain;
    if (!object(source) || source.policy !== 'epoch-on-vendor-change' ||
        source.sampleAligned !== false || !object(source.tuners)) {
      errors.push(`${name}.gain has an invalid schema`);
    } else {
      const epoch = count(source.epoch, `${name}.gain.epoch`, errors);
      const tuners = {};
      for (const [index, tuner] of ['A', 'B'].entries()) {
        const event = source.tuners[tuner];
        const field = `${name}.gain.tuners.${tuner}`;
        if (!object(event)) {
          errors.push(`${field} must be an object`); continue;
        }
        const unsigned32 = (value, suffix) => {
          if (!Number.isSafeInteger(value) || value < 0 || value > 4294967295)
            errors.push(`${field}.${suffix} must be a non-negative 32-bit integer`);
          return Number.isSafeInteger(value) && value >= 0 && value <= 4294967295 ? value : 0;
        };
        const requestedGainReductionDb = unsigned32(event.requestedGainReductionDb,
          'requestedGainReductionDb');
        const requestedLnaState = unsigned32(event.requestedLnaState,
          'requestedLnaState');
        const eventCount = count(event.eventCount, `${field}.eventCount`, errors);
        const lastEventAt = count(event.lastEventAt, `${field}.lastEventAt`, errors);
        const eventEpoch = count(event.epoch, `${field}.epoch`, errors);
        if (eventEpoch > epoch) errors.push(`${field}.epoch exceeds capture gain epoch`);
        if ((eventCount === 0) !== (lastEventAt === 0 && eventEpoch === 0))
          errors.push(`${field} event count and provenance disagree`);
        if (typeof event.metadataAvailable !== 'boolean')
          errors.push(`${field}.metadataAvailable must be boolean`);
        let reported = null;
        if (event.metadataAvailable) {
          if (!object(event.reported)) errors.push(`${field}.reported must be an object`);
          else {
            const gRdB = unsigned32(event.reported.gRdB, 'reported.gRdB');
            const lnaGRdB = unsigned32(event.reported.lnaGRdB, 'reported.lnaGRdB');
            const currGain = event.reported.currGain;
            if (typeof currGain !== 'number' || !Number.isFinite(currGain))
              errors.push(`${field}.reported.currGain must be finite`);
            reported = {gRdB, lnaGRdB,
              currGain: typeof currGain === 'number' && Number.isFinite(currGain) ? currGain : 0};
          }
        } else if (event.reported !== null) {
          errors.push(`${field}.reported must be null when metadata is unavailable`);
        }
        const requested = outer.receiverStartup?.requested;
        if (requested && (requested.gainReduction[index] !== requestedGainReductionDb ||
            requested.lnaState !== requestedLnaState))
          errors.push(`${field} requested gain does not match startup receipt`);
        tuners[tuner] = {requestedGainReductionDb, requestedLnaState,
          eventCount, lastEventAt, epoch: eventEpoch,
          metadataAvailable: event.metadataAvailable, reported};
      }
      gain = {policy: 'epoch-on-vendor-change', sampleAligned: false, epoch, tuners};
    }
  }
  const result = {schema: 1, receiver: 'RspDuo', monitoring: payload.monitoring,
    overload, ackFailures: count(payload.ackFailures, `${name}.ackFailures`, errors)};
  if (gain !== undefined) result.gain = gain;
  return result;
}
function usrpStartup(payload, outer, errors) {
  const name = 'receiverStartup';
  if (outer.input !== 'live' || outer.receiver !== 'Usrp' || !object(payload)) {
    errors.push(`${name} requires a live Usrp object`); return null;
  }
  const requested = payload.requested;
  if (payload.schema !== 1 || payload.receiver !== 'Usrp' ||
      !['pending', 'accepted'].includes(payload.status) || !object(requested)) {
    errors.push(`${name} has an invalid schema`); return null;
  }
  const address = text(requested.address, `${name}.requested.address`, errors);
  const subdevices = text(requested.subdevices, `${name}.requested.subdevices`, errors);
  const frequency = positive32(requested.frequency, `${name}.requested.frequency`, errors);
  const sampleRate = positive32(requested.sampleRate, `${name}.requested.sampleRate`, errors);
  const strings = (source, field) => {
    if (!Array.isArray(source) || source.length !== 2) {
      errors.push(`${name}.requested.${field} requires two values`); return [];
    }
    return source.map((value, index) => text(value,
      `${name}.requested.${field}[${index}]`, errors));
  };
  const antennas = strings(requested.antennas, 'antennas');
  let gains = [];
  if (!Array.isArray(requested.gains) || requested.gains.length !== 2)
    errors.push(`${name}.requested.gains requires two values`);
  else gains = requested.gains.map((gain, index) => {
    if (typeof gain !== 'number' || !Number.isFinite(gain))
      errors.push(`${name}.requested.gains[${index}] must be finite`);
    return typeof gain === 'number' && Number.isFinite(gain) ? gain : 0;
  });
  if (!subdevices) errors.push(`${name}.requested.subdevices must not be empty`);
  if (frequency !== outer.frequency || sampleRate !== outer.sampleRate)
    errors.push(`${name}.requested frequency and sample rate must match processor status`);
  let coherence = null;
  if (payload.status === 'pending') {
    if (payload.coherence !== null) errors.push(`${name}.coherence must be null while pending`);
  } else if (!object(payload.coherence)) {
    errors.push(`${name}.accepted requires coherence attestation`);
  } else {
    const source = payload.coherence;
    const motherboards = count(source.motherboards, `${name}.coherence.motherboards`, errors);
    const channels = count(source.channels, `${name}.coherence.channels`, errors);
    const motherboard = text(source.motherboard, `${name}.coherence.motherboard`, errors);
    const clockSource = text(source.clockSource, `${name}.coherence.clockSource`, errors);
    const timeSource = text(source.timeSource, `${name}.coherence.timeSource`, errors);
    const appliedSubdevices = text(source.subdevices, `${name}.coherence.subdevices`, errors);
    const policy = text(source.policy, `${name}.coherence.policy`, errors);
    if (motherboards !== 1 || channels < 2 || source.singleMotherboard !== true ||
        motherboard !== 'B210' || policy !== 'single-motherboard-b210-v1' ||
        !clockSource || !timeSource || !appliedSubdevices)
      errors.push(`${name}.coherence does not attest one complete coherent motherboard`);
    if (appliedSubdevices !== subdevices)
      errors.push(`${name}.coherence subdevices differ from request`);
    coherence = {motherboards, channels, motherboard, clockSource, timeSource,
      subdevices: appliedSubdevices, policy, singleMotherboard: true};
  }
  return {schema: 1, receiver: 'Usrp', status: payload.status,
    requested: {address, subdevices, frequency, sampleRate, antennas, gains}, coherence};
}
function usrpHealth(payload, outer, errors) {
  const name = 'receiverHealth';
  if (outer.input !== 'live' || outer.receiver !== 'Usrp' || !object(payload) ||
      payload.schema !== 1 || payload.receiver !== 'Usrp' ||
      typeof payload.monitoring !== 'boolean' || !object(payload.deviceTime)) {
    errors.push(`${name} has an invalid live Usrp schema`); return null;
  }
  if (outer.state === 'stopped' && payload.monitoring)
    errors.push(`${name}.monitoring must be false after stop`);
  const timing = payload.deviceTime;
  if (timing.source !== 'uhd-device-time' || timing.utcMapped !== false ||
      typeof timing.continuous !== 'boolean')
    errors.push(`${name}.deviceTime has invalid provenance`);
  const sampleRateHz = positive32(timing.sampleRateHz,
    `${name}.deviceTime.sampleRateHz`, errors);
  if (sampleRateHz !== outer.sampleRate)
    errors.push(`${name}.deviceTime.sampleRateHz must match processor status`);
  const decimal = (value, field) => {
    if (typeof value !== 'string' || !/^(0|[1-9][0-9]*)$/.test(value)) {
      errors.push(`${name}.deviceTime.${field} must be a decimal integer string`); return '0';
    }
    return value;
  };
  const samples = decimal(timing.samples, 'samples');
  const blocks = decimal(timing.blocks, 'blocks');
  const epoch = timing.epoch === undefined ? null : decimal(timing.epoch, 'epoch');
  let epochTick = null, nextTick = null;
  if (timing.continuous) {
    epochTick = decimal(timing.epochTick, 'epochTick');
    nextTick = decimal(timing.nextTick, 'nextTick');
    if (BigInt(samples) === 0n || BigInt(blocks) === 0n ||
        BigInt(nextTick) !== BigInt(epochTick) + BigInt(samples))
      errors.push(`${name}.deviceTime continuous range is inconsistent`);
  } else if (timing.epochTick !== null || timing.nextTick !== null ||
      samples !== '0' || blocks !== '0') {
    errors.push(`${name}.deviceTime unavailable state is inconsistent`);
  }
  let recovery;
  if (payload.recovery !== undefined) {
    const source = payload.recovery;
    if (!object(source) || !['unavailable', 'requalifying', 'qualified'].includes(source.state)) {
      errors.push(`${name}.recovery has an invalid state`);
    } else {
      const required = count(source.qualificationRequiredBlocks,
        `${name}.recovery.qualificationRequiredBlocks`, errors);
      const observed = count(source.qualificationObservedBlocks,
        `${name}.recovery.qualificationObservedBlocks`, errors);
      const counters = Object.fromEntries(['discontinuities', 'requalifications',
        'consecutiveDiscontinuities', 'discardedBlocks', 'discardedSamplesPerChannel']
        .map(field => [field, decimal(source[field], field)]));
      const lastDiscontinuity = errorText(source.lastDiscontinuity,
        `${name}.recovery.lastDiscontinuity`, errors);
      if (required !== 2 || observed > required || epoch === null)
        errors.push(`${name}.recovery qualification bounds are invalid`);
      if ((source.state === 'qualified') !== timing.continuous)
        errors.push(`${name}.recovery state disagrees with device-time continuity`);
      if (source.state === 'qualified' && observed !== required)
        errors.push(`${name}.recovery qualified without the required blocks`);
      if (BigInt(counters.requalifications) > BigInt(counters.discontinuities))
        errors.push(`${name}.recovery has more requalifications than discontinuities`);
      recovery = {state: source.state, qualificationRequiredBlocks: required,
        qualificationObservedBlocks: observed, ...counters, lastDiscontinuity};
    }
  }
  const result = {schema: 1, receiver: 'Usrp', monitoring: payload.monitoring,
    deviceTime: {source: 'uhd-device-time', utcMapped: false,
      continuous: timing.continuous, sampleRateHz, epochTick, nextTick, samples, blocks}};
  if (epoch !== null) result.deviceTime.epoch = epoch;
  if (recovery !== undefined) result.recovery = recovery;
  return result;
}
function hackrfStartup(payload, outer, errors) {
  const name = 'receiverStartup';
  if (outer.input !== 'live' || outer.receiver !== 'HackRF' || !object(payload) ||
      payload.schema !== 1 || payload.receiver !== 'HackRF' ||
      !['pending', 'accepted'].includes(payload.status) || !object(payload.requested)) {
    errors.push(`${name} has an invalid live HackRF schema`); return null;
  }
  const requested = payload.requested;
  const frequency = positive32(requested.frequency, `${name}.requested.frequency`, errors);
  const sampleRate = positive32(requested.sampleRate, `${name}.requested.sampleRate`, errors);
  if (frequency !== outer.frequency || sampleRate !== outer.sampleRate)
    errors.push(`${name}.requested frequency and sample rate must match processor status`);
  const serials = Array.isArray(requested.serials) && requested.serials.length === 2 ?
    requested.serials.map((value, index) => text(value,
      `${name}.requested.serials[${index}]`, errors)) : [];
  if (serials.length !== 2 || !serials[0] || !serials[1] || serials[0] === serials[1])
    errors.push(`${name}.requested.serials requires two distinct values`);
  const gains = (field, step, maximum) => {
    const source = requested[field];
    if (!Array.isArray(source) || source.length !== 2) {
      errors.push(`${name}.requested.${field} requires two values`); return [];
    }
    return source.map((value, index) => {
      if (!Number.isInteger(value) || value < 0 || value > maximum || value % step)
        errors.push(`${name}.requested.${field}[${index}] is invalid`);
      return Number.isInteger(value) && value >= 0 ? value : 0;
    });
  };
  const lnaGainDb = gains('lnaGainDb', 8, 40);
  const vgaGainDb = gains('vgaGainDb', 2, 62);
  const ampEnabled = requested.ampEnabled;
  if (!Array.isArray(ampEnabled) || ampEnabled.length !== 2 ||
      ampEnabled.some(value => typeof value !== 'boolean'))
    errors.push(`${name}.requested.ampEnabled requires two booleans`);
  let synchronization = null;
  if (payload.status === 'pending') {
    if (payload.synchronization !== null)
      errors.push(`${name}.synchronization must be null while pending`);
  } else if (!object(payload.synchronization)) {
    errors.push(`${name}.accepted requires synchronization provenance`);
  } else {
    const source = payload.synchronization;
    if (source.policy !== 'shared-clock-hardware-trigger-v1' ||
        source.clockOutputRole !== 'surveillance' ||
        source.hardwareTriggerRole !== 'surveillance' ||
        source.startOrder !== 'surveillance-then-reference' ||
        source.physicalWiringVerified !== false ||
        source.hardwareTimestampAvailable !== false)
      errors.push(`${name}.synchronization has invalid provenance`);
    synchronization = {policy: 'shared-clock-hardware-trigger-v1',
      clockOutputRole: 'surveillance', hardwareTriggerRole: 'surveillance',
      startOrder: 'surveillance-then-reference', physicalWiringVerified: false,
      hardwareTimestampAvailable: false};
  }
  return {schema: 1, receiver: 'HackRF', status: payload.status,
    requested: {frequency, sampleRate, serials, lnaGainDb, vgaGainDb,
      ampEnabled: Array.isArray(ampEnabled) ? ampEnabled.slice(0, 2) : []},
    synchronization};
}
function hackrfHealth(payload, outer, errors) {
  const name = 'receiverHealth';
  if (outer.input !== 'live' || outer.receiver !== 'HackRF' || !object(payload) ||
      payload.schema !== 1 || payload.receiver !== 'HackRF' ||
      typeof payload.monitoring !== 'boolean' || !object(payload.pairing)) {
    errors.push(`${name} has an invalid live HackRF schema`); return null;
  }
  if (outer.state === 'stopped' && payload.monitoring)
    errors.push(`${name}.monitoring must be false after stop`);
  const pairing = payload.pairing;
  if (pairing.policy !== 'paired-callback-fail-closed-v1' ||
      typeof pairing.qualified !== 'boolean')
    errors.push(`${name}.pairing has invalid provenance`);
  const decimal = (value, field) => {
    if (typeof value !== 'string' || !/^(0|[1-9][0-9]*)$/.test(value)) {
      errors.push(`${name}.pairing.${field} must be a decimal integer string`); return '0';
    }
    return value;
  };
  const pair = (value, field) => {
    if (!Array.isArray(value) || value.length !== 2) {
      errors.push(`${name}.pairing.${field} requires two values`); return ['0', '0'];
    }
    return value.map((item, index) => decimal(item, `${field}[${index}]`));
  };
  const epoch = decimal(pairing.epoch, 'epoch');
  const callbacks = pair(pairing.callbacks, 'callbacks');
  const samples = pair(pairing.samples, 'samples');
  const pairedCallbacks = decimal(pairing.pairedCallbacks, 'pairedCallbacks');
  const pairedSamples = decimal(pairing.pairedSamples, 'pairedSamples');
  const discontinuities = decimal(pairing.discontinuities, 'discontinuities');
  const discardedSamples = pair(pairing.discardedSamples, 'discardedSamples');
  const reason = text(pairing.reason, `${name}.pairing.reason`, errors);
  if (pairing.qualified) {
    const pairedCallbackCount = BigInt(pairedCallbacks);
    const pairedSampleCount = BigInt(pairedSamples);
    const callbackCounts = callbacks.map(BigInt);
    const sampleCounts = samples.map(BigInt);
    const pending = callbackCounts.filter(value => value === pairedCallbackCount + 1n).length;
    if (BigInt(epoch) === 0n || pairedCallbackCount === 0n || pairedSampleCount === 0n ||
        pending > 1 || callbackCounts.some(value => value < pairedCallbackCount ||
          value > pairedCallbackCount + 1n) ||
        sampleCounts.some((value, index) => callbackCounts[index] === pairedCallbackCount ?
          value !== pairedSampleCount : value <= pairedSampleCount))
      errors.push(`${name}.pairing qualified counters disagree`);
  }
  if (payload.monitoring && BigInt(discontinuities) > 0n)
    errors.push(`${name}.monitoring cannot continue after a pair discontinuity`);
  return {schema: 1, receiver: 'HackRF', monitoring: payload.monitoring,
    pairing: {policy: 'paired-callback-fail-closed-v1', qualified: pairing.qualified,
      epoch, callbacks, samples, pairedCallbacks, pairedSamples, discontinuities,
      discardedSamples, reason}};
}
function status(payload) {
  const errors = [];
  if (!payload || typeof payload !== 'object' || Array.isArray(payload))
    return {valid: false, errors: ['status must be an object']};
  const input = text(payload.input, 'input', errors);
  const receiver = text(payload.receiver, 'receiver', errors);
  const state = text(payload.state, 'state', errors);
  if (!['live', 'replay'].includes(input)) errors.push('input is invalid');
  if (!RECEIVERS.has(receiver)) errors.push('receiver is invalid');
  if (!STATES.has(state)) errors.push('state is invalid');
  const value = {
    input, receiver, state,
    error: errorText(payload.error, 'error', errors),
    file: text(payload.file, 'file', errors),
    sampleRate: positive32(payload.sampleRate, 'sampleRate', errors, state === 'error'),
    frequency: positive32(payload.frequency, 'frequency', errors, state === 'error'),
    positionSamples: count(payload.positionSamples, 'positionSamples', errors),
    totalSamples: count(payload.totalSamples, 'totalSamples', errors),
    loops: count(payload.loops, 'loops', errors),
    trailingSamples: count(payload.trailingSamples, 'trailingSamples', errors),
    recording: payload.recording === true,
    recordingFile: text(payload.recordingFile, 'recordingFile', errors),
    recordingError: errorText(payload.recordingError, 'recordingError', errors),
    recordedSamples: count(payload.recordedSamples, 'recordedSamples', errors),
    recordingRequestId: payload.recordingRequestId === undefined ? null : count(payload.recordingRequestId, 'recordingRequestId', errors),
    clipRevision: payload.clipRevision === undefined ? null : count(payload.clipRevision, 'clipRevision', errors),
    clipError: payload.clipError === undefined ? '' : errorText(payload.clipError, 'clipError', errors),
    clipFile: payload.clipFile === undefined ? '' : text(payload.clipFile, 'clipFile', errors),
    clipSamples: payload.clipSamples === undefined ? 0 : count(payload.clipSamples, 'clipSamples', errors)
  };
  if (typeof payload.recording !== 'boolean') errors.push('recording must be a boolean');
  if (value.input === 'live' && value.state !== 'live' && value.state !== 'error' && value.state !== 'stopped')
    errors.push('live input has an invalid state');
  if (value.input === 'replay' && value.recording) errors.push('recording is unavailable during replay');
  if (payload.clipRevision !== undefined && value.clipRevision === 0)
    errors.push('clipRevision must be positive when supplied');
  if (value.input === 'replay' && value.clipRevision !== null)
    errors.push('clip acknowledgement is unavailable during replay');
  if (payload.clipError !== undefined && value.clipRevision === null)
    errors.push('clipError requires clipRevision');
  if ((payload.clipFile !== undefined || payload.clipSamples !== undefined) &&
      value.clipRevision === null)
    errors.push('clipFile and clipSamples require clipRevision');
  if (value.state === 'error' && !value.error) errors.push('error state requires error text');
  if (value.positionSamples > value.totalSamples && value.totalSamples > 0)
    errors.push('positionSamples exceeds totalSamples');
  if (payload.receiverStartup !== undefined)
    value.receiverStartup = value.receiver === 'Usrp' ?
      usrpStartup(payload.receiverStartup, value, errors) :
      value.receiver === 'HackRF' ?
        hackrfStartup(payload.receiverStartup, value, errors) :
      rspStartup(payload.receiverStartup, value, errors);
  if (payload.receiverHealth !== undefined)
    value.receiverHealth = value.receiver === 'Usrp' ?
      usrpHealth(payload.receiverHealth, value, errors) :
      value.receiver === 'HackRF' ?
        hackrfHealth(payload.receiverHealth, value, errors) :
      rspHealth(payload.receiverHealth, value, errors);
  return errors.length ? {valid: false, errors} : {valid: true, value};
}

function fresh(entry, now = Date.now(), maxAgeMs = 5000) {
  return !!entry && Number.isFinite(entry.receivedAt) && now - entry.receivedAt <= maxAgeMs;
}
module.exports = {status, fresh, MAX_TEXT};
