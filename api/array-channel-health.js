'use strict';

// Truth-blind diagnostics only. This module never changes gain, channel
// weights, quarantine state, or detector behavior.
const finite = value => typeof value === 'number' && Number.isFinite(value);

function median(values) {
  const sorted = [...values].sort((a, b) => a - b);
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 ? sorted[middle] :
    (sorted[middle - 1] + sorted[middle]) / 2;
}

function relativeDb(value, reference) { return 20 * Math.log10(value / reference); }

function measurement(channel, field, {minimum = -Infinity, maximum = Infinity} = {}) {
  const value = channel[field];
  if (value === undefined || value === null) return {value: null, flag: `missing_${field}`};
  if (!finite(value) || value < minimum || value > maximum)
    return {value: null, flag: `invalid_${field}`};
  return {value, flag: null};
}

function metricMedian(channels, field, {positive = false} = {}) {
  const values = channels.map(channel => channel[field]).filter(value =>
    value !== null && (!positive || value > 0));
  return values.length ? median(values) : null;
}

// Accepts any bounded array cardinality. Missing or malformed telemetry is
// reported as diagnostic state rather than being silently substituted with a
// nominal value. This keeps the output useful when the Suite offers only ADC
// telemetry, and prevents an absent manifold sample from looking healthy.
function diagnoseArrayChannels(channels, options = {}) {
  if (!Array.isArray(channels) || channels.length < 1 || channels.length > 64)
    throw new TypeError('channels must contain one to 64 elements');
  const directLimitDb = options.directLimitDb ?? -6;
  const gainLimitDb = options.gainLimitDb ?? 6;
  const clippingFraction = options.clippingFraction ?? .9;
  const phaseStabilityLimitDeg = options.phaseStabilityLimitDeg ?? 15;
  [directLimitDb, gainLimitDb, clippingFraction, phaseStabilityLimitDeg].forEach((value, index) => {
    if (!finite(value)) throw new TypeError(`${['directLimitDb', 'gainLimitDb',
      'clippingFraction', 'phaseStabilityLimitDeg'][index]} must be finite`);
  });
  if (!(directLimitDb < 0) || !(gainLimitDb > 0) ||
      !(clippingFraction > 0 && clippingFraction <= 1) || !(phaseStabilityLimitDeg > 0))
    throw new RangeError('channel-health thresholds are invalid');

  const seen = new Set();
  const normalized = channels.map((channel, index) => {
    if (!channel || typeof channel !== 'object') throw new TypeError(`channel ${index} is invalid`);
    const id = String(channel.id ?? index);
    if (seen.has(id)) throw new TypeError(`channel ${index} has a duplicate id`);
    seen.add(id);
    const gain = measurement(channel, 'gainDb', {minimum: -1, maximum: 100});
    const rms = measurement(channel, 'rmsFraction', {minimum: 0, maximum: 1});
    const p999 = measurement(channel, 'p999Fraction', {minimum: 0, maximum: 1});
    const direct = measurement(channel, 'directAnchorMagnitude', {minimum: 0});
    // Phase stability is optional: this adapter currently has no time-series
    // phase measurement, so absence must not masquerade as an observation.
    const phase = Object.hasOwn(channel, 'phaseStabilityDeg') ?
      measurement(channel, 'phaseStabilityDeg', {minimum: 0}) : {value: null, flag: null};
    return {id, gainDb: gain.value, rmsFraction: rms.value,
      p999Fraction: p999.value, directAnchorMagnitude: direct.value,
      ...(Array.isArray(channel.directAnchorResponse) && channel.directAnchorResponse.length === 2 &&
          channel.directAnchorResponse.every(finite) ?
        {directAnchorResponse: [...channel.directAnchorResponse]} : {}),
      ...(finite(channel.directAnchorPhaseDeg) ? {directAnchorPhaseDeg: channel.directAnchorPhaseDeg} : {}),
      ...(Array.isArray(channel.complexResponse) && channel.complexResponse.length === 2 &&
          channel.complexResponse.every(finite) ? {complexResponse: [...channel.complexResponse]} : {}),
      ...(phase.value === null ? {} : {phaseStabilityDeg: phase.value}),
      measurementFlags: [gain.flag, rms.flag, p999.flag, direct.flag, phase.flag].filter(Boolean)};
  });
  const medians = {gainDb: metricMedian(normalized, 'gainDb'),
    rmsFraction: metricMedian(normalized, 'rmsFraction', {positive: true}),
    directAnchorMagnitude: metricMedian(normalized, 'directAnchorMagnitude', {positive: true})};
  const results = normalized.map(channel => {
    const flags = [...channel.measurementFlags];
    const allZeroAdc = channel.rmsFraction === 0 && channel.p999Fraction === 0;
    if (allZeroAdc) flags.push('absolute_zero_or_dropout');
    if (channel.directAnchorMagnitude === 0) flags.push('direct_anchor_dropout');
    const directRelativeDb = channel.directAnchorMagnitude !== null &&
      channel.directAnchorMagnitude > 0 && medians.directAnchorMagnitude !== null ?
      relativeDb(channel.directAnchorMagnitude, medians.directAnchorMagnitude) : null;
    const digitizerRelativeDb = channel.rmsFraction !== null && channel.rmsFraction > 0 &&
      medians.rmsFraction !== null ? relativeDb(channel.rmsFraction, medians.rmsFraction) : null;
    const gainOffsetDb = channel.gainDb !== null && medians.gainDb !== null ?
      channel.gainDb - medians.gainDb : null;
    if (directRelativeDb !== null && directRelativeDb < directLimitDb) flags.push('direct_path_attenuation');
    if (gainOffsetDb !== null && gainOffsetDb > gainLimitDb) flags.push('gain_compensation');
    if (digitizerRelativeDb !== null && digitizerRelativeDb < directLimitDb) flags.push('low_digitizer_level');
    if (channel.p999Fraction !== null && channel.p999Fraction >= clippingFraction) flags.push('clipping_risk');
    if (channel.phaseStabilityDeg !== undefined && channel.phaseStabilityDeg > phaseStabilityLimitDeg)
      flags.push('phase_instability');
    return {...channel, directRelativeDb, digitizerRelativeDb, gainOffsetDb,
      flags: [...new Set(flags)], recommendPhysicalSwapTest:
        flags.includes('direct_path_attenuation') && flags.includes('gain_compensation')};
  });
  const usableChannels = results.filter(channel => !channel.flags.includes('absolute_zero_or_dropout') &&
    !channel.flags.includes('missing_rmsFraction') && !channel.flags.includes('invalid_rmsFraction')).length;
  const allZeroOrDropped = results.length > 0 && results.every(channel =>
    channel.flags.includes('absolute_zero_or_dropout'));
  const arrayUsable = !allZeroOrDropped && usableChannels >= Math.min(2, results.length);
  return {schema: 'vectorwarp-array-channel-health-v2', action: 'diagnostic-only', medians,
    channels: results, arrayUsable, arrayFlags: [
      ...(allZeroOrDropped ? ['all_channels_zero_or_dropout', 'array_unusable'] : []),
      ...(!arrayUsable && !allZeroOrDropped ? ['array_unusable'] : [])],
    automaticWeightingAllowed: false, automaticQuarantineAllowed: false};
}

// Platform-neutral optional telemetry. A complex snapshot is not a phase
// stability observation: producers must provide the measured stability and an
// ADC-generation match for every channel.
function complexChannelMeasurements(telemetry, count, frequencyHz, sampleRateHz, adc) {
  const unavailable = reason => ({available: false, reason, measurements: new Map()});
  if (telemetry === undefined || telemetry === null) return unavailable('complex_channel_telemetry_unavailable');
  if (!telemetry || telemetry.schema !== 'vectorwarp-complex-channel-telemetry-v1' ||
      telemetry.frequency_hz !== frequencyHz || telemetry.sample_rate_hz !== sampleRateHz ||
      !Array.isArray(telemetry.channels) || telemetry.channels.length !== count)
    return unavailable('complex_channel_telemetry_invalid');
  const adcByChannel = new Map(adc.channels.map(channel => [channel.channel, channel]));
  const measurements = new Map();
  for (const item of telemetry.channels) {
    if (!Number.isInteger(item?.channel) || !adcByChannel.has(item.channel) || measurements.has(item.channel) ||
        item.generation !== adcByChannel.get(item.channel).generation || !Array.isArray(item.response) ||
        item.response.length !== 2 || !item.response.every(finite) ||
        !finite(item.phase_stability_deg) || item.phase_stability_deg < 0)
      return unavailable('complex_channel_telemetry_invalid');
    measurements.set(item.channel, {response: [...item.response],
      phaseStabilityDeg: item.phase_stability_deg});
  }
  return {available: true, reason: null, measurements};
}

function completeAdcTelemetry(adc, count) {
  const channels = Array.isArray(adc?.channels) ? adc.channels : [];
  if (adc?.complete !== true || channels.length !== count) return false;
  const ids = new Set();
  for (const channel of channels) {
    if (!Number.isInteger(channel?.channel) || channel.channel < 0 || channel.channel >= count ||
        ids.has(channel.channel)) return false;
    ids.add(channel.channel);
  }
  return ids.size === count;
}

function coherentAdcGeneration(adc, telemetryComplete) {
  if (!telemetryComplete) return null;
  const generations = adc.channels.map(channel => channel.generation);
  if (!generations.every(generation => Number.isSafeInteger(generation) && generation >= 0)) return false;
  return new Set(generations).size === 1;
}

// Read-only Suite telemetry, bound to the loaded configuration and acquisition.
function diagnoseUpstreamArrayChannels({config, upstreamStatus,
  configRevision = null, loadedConfigRevision = null, configFingerprint = null} = {}) {
  const count = config?.capture?.device?.channel_count;
  if (!Number.isInteger(count) || count < 1 || count > 64)
    throw new TypeError('configured channel count must be one to 64');
  const adc = upstreamStatus?.available === true ? upstreamStatus.actual?.adcHeadroom : null;
  const byId = new Map(Array.isArray(adc?.channels) ? adc.channels.map(channel =>
    [channel?.channel, channel]) : []);
  const telemetryComplete = completeAdcTelemetry(adc, count);
  const acquisitionMatchesConfig = upstreamStatus?.available === true &&
    upstreamStatus.actual?.centerFrequency === config.capture?.fc &&
    upstreamStatus.actual?.sampleRate === config.capture?.fs;
  const adcGenerationCoherent = coherentAdcGeneration(adc, telemetryComplete);
  const calibrationReady = String(upstreamStatus?.actual?.calibrationState || '').toUpperCase() ===
    'CONVERGED';
  const revisionsMatch = !(configRevision && loadedConfigRevision) ||
    configRevision === loadedConfigRevision;
  const upstreamMatched = upstreamStatus?.matched !== false;
  const coherentEpochReady = telemetryComplete && acquisitionMatchesConfig &&
    adcGenerationCoherent === true && calibrationReady && revisionsMatch && upstreamMatched;
  const complex = coherentEpochReady ? complexChannelMeasurements(
    upstreamStatus?.actual?.complexChannelTelemetry, count, upstreamStatus.actual.centerFrequency,
    upstreamStatus.actual.sampleRate, adc) :
    {available: false, reason: 'upstream_epoch_not_ready', measurements: new Map()};
  // An otherwise valid manifold observation cannot be joined to ADC samples
  // from another tuning or generation. Do not claim an anchor in that epoch.
  const direct = !acquisitionMatchesConfig ?
    {available: false, reason: 'upstream_acquisition_mismatch', measurements: new Map()} :
    adcGenerationCoherent === false ?
      {available: false, reason: 'incoherent_adc_generation', measurements: new Map()} :
      !coherentEpochReady ?
        {available: false, reason: 'upstream_epoch_not_ready', measurements: new Map()} :
      {available: false, reason: 'direct_anchor_telemetry_unavailable', measurements: new Map()};
  const report = diagnoseArrayChannels(Array.from({length: count}, (_, id) => {
    const sample = byId.get(id);
    const anchor = direct.measurements.get(id), complexMeasurement = complex.measurements.get(id);
    return {id, gainDb: sample?.actualGainDb, rmsFraction: sample?.rmsFraction,
      p999Fraction: sample?.p999Fraction, directAnchorMagnitude: anchor?.magnitude,
      ...(anchor ? {directAnchorResponse: anchor.response, directAnchorPhaseDeg: anchor.phaseDeg} : {}),
      ...(complexMeasurement ? {complexResponse: complexMeasurement.response,
        phaseStabilityDeg: complexMeasurement.phaseStabilityDeg} : {})};
  }));
  const epochUnsafe = !coherentEpochReady;
  for (const channel of report.channels) {
    if (!telemetryComplete) {
      channel.flags.push('upstream_measurement_unavailable');
      channel.measurementFlags.push('upstream_measurement_unavailable');
    }
    if (!direct.available) {
      channel.flags.push('direct_anchor_evidence_unavailable');
      channel.measurementFlags.push('direct_anchor_evidence_unavailable');
    }
    if (!acquisitionMatchesConfig) {
      channel.flags.push('upstream_acquisition_mismatch');
      channel.measurementFlags.push('upstream_acquisition_mismatch');
    }
    if (adcGenerationCoherent === false) {
      channel.flags.push('incoherent_adc_generation');
      channel.measurementFlags.push('incoherent_adc_generation');
    }
    if (!coherentEpochReady) {
      channel.flags.push('upstream_epoch_not_ready');
      channel.measurementFlags.push('upstream_epoch_not_ready');
    }
    channel.flags = [...new Set(channel.flags)];
    channel.measurementFlags = [...new Set(channel.measurementFlags)];
  }
  if (epochUnsafe) report.arrayUsable = false;
  if (epochUnsafe) report.arrayFlags = [...new Set([...report.arrayFlags,
    ...(!acquisitionMatchesConfig ? ['upstream_acquisition_mismatch'] : []),
    ...(adcGenerationCoherent === false ? ['incoherent_adc_generation'] : []),
    ...(!telemetryComplete ? ['upstream_measurement_unavailable'] : []),
    ...(!calibrationReady ? ['calibration_not_converged'] : []),
    ...(!revisionsMatch ? ['config_revision_mismatch'] : []),
    ...(!upstreamMatched ? ['upstream_status_mismatch'] : []),
    'array_unusable'])];
  report.epoch = {configRevision: configRevision || null,
    loadedConfigRevision: loadedConfigRevision || null,
    configFingerprint: configFingerprint || null,
    upstream: {frequencyHz: upstreamStatus?.actual?.centerFrequency ?? null,
      sampleRateHz: upstreamStatus?.actual?.sampleRate ?? null,
      calibrationState: upstreamStatus?.actual?.calibrationState ?? null,
      acquisitionMatchesConfig,
      adcGenerationCoherent,
      calibrationReady,
      configRevisionsMatch: revisionsMatch,
      upstreamMatched,
      coherentEpochReady,
      adcGeneration: telemetryComplete ? adc.channels.map(channel => channel.generation) : null},
    directAnchor: {available: direct.available, reason: direct.reason},
    complexChannelTelemetry: {available: complex.available, reason: complex.reason}};
  return report;
}

module.exports = {diagnoseArrayChannels, diagnoseUpstreamArrayChannels};
