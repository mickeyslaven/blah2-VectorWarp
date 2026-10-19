'use strict';

const finite = value => typeof value === 'number' && Number.isFinite(value);
const clamp = value => Math.max(0, Math.min(1, value));

const DAB_BLOCKS = Object.freeze([
  ['5A', 174928000], ['5B', 176640000], ['5C', 178352000], ['5D', 180064000],
  ['6A', 181936000], ['6B', 183648000], ['6C', 185360000], ['6D', 187072000],
  ['7A', 188928000], ['7B', 190640000], ['7C', 192352000], ['7D', 194064000],
  ['8A', 195936000], ['8B', 197648000], ['8C', 199360000], ['8D', 201072000],
  ['9A', 202928000], ['9B', 204640000], ['9C', 206352000], ['9D', 208064000],
  ['10A', 209936000], ['10N', 210096000], ['10B', 211648000], ['10C', 213360000],
  ['10D', 215072000], ['11A', 216928000], ['11N', 217088000], ['11B', 218640000],
  ['11C', 220352000], ['11D', 222064000], ['12A', 223936000], ['12N', 224096000],
  ['12B', 225648000], ['12C', 227360000], ['12D', 229072000], ['13A', 230784000],
  ['13B', 232496000], ['13C', 234208000], ['13D', 235776000], ['13E', 237488000],
  ['13F', 239200000]
]);

function sequence(first, last, step, label) {
  const result = [];
  for (let value = first; value <= last; value += step)
    result.push({id: label(value), frequency_hz: value});
  return result;
}

const BAND_PLANS = Object.freeze({
  'fm-us': Object.freeze({standard: 'FM', region: 'US', occupied_bandwidth_hz: 200000,
    candidates: Object.freeze(sequence(88100000, 107900000, 200000,
      value => (value / 1e6).toFixed(1)))}),
  'fm-eu': Object.freeze({standard: 'FM', region: 'EU', occupied_bandwidth_hz: 200000,
    candidates: Object.freeze(sequence(87500000, 108000000, 100000,
      value => (value / 1e6).toFixed(1)))}),
  'dab-band-iii': Object.freeze({standard: 'DAB', region: 'Band III',
    occupied_bandwidth_hz: 1536000,
    candidates: Object.freeze(DAB_BLOCKS.map(([id, frequency_hz]) => ({id, frequency_hz})))}),
  'atsc-us-uhf': Object.freeze({standard: 'ATSC', region: 'US', occupied_bandwidth_hz: 6000000,
    candidates: Object.freeze(sequence(473000000, 605000000, 6000000,
      value => `RF${14 + (value - 473000000) / 6000000}`))}),
  'dvb-t-uhf-8mhz': Object.freeze({standard: 'DVB-T/T2', region: 'generic 8 MHz UHF',
    occupied_bandwidth_hz: 8000000,
    candidates: Object.freeze(sequence(474000000, 690000000, 8000000,
      value => `CH${21 + (value - 474000000) / 8000000}`))})
});

function bandPlans() {
  return Object.fromEntries(Object.entries(BAND_PLANS).map(([name, plan]) => [name, {
    standard: plan.standard, region: plan.region,
    occupied_bandwidth_hz: plan.occupied_bandwidth_hz,
    candidates: plan.candidates.map(candidate => ({...candidate}))
  }]));
}

function receiverChannels(config) {
  const device = config?.capture?.device || {};
  if (device.type === 'Kraken') return device.channel_count;
  if (device.type === 'RspDuo') return 2;
  if (device.type === 'Usrp') return Array.isArray(device.antenna) ? device.antenna.length : 2;
  if (device.type === 'HackRF') return Array.isArray(device.serial) ? device.serial.length : 2;
  return 0;
}

function receiverCapabilities(config) {
  const type = config?.capture?.device?.type || 'Unknown';
  const channels = receiverChannels(config);
  const kraken = type === 'Kraken';
  const coherent = kraken || ['RspDuo', 'Usrp', 'HackRF'].includes(type);
  const geometry = config?.capture?.device?.array_geometry;
  const positioned = Array.isArray(geometry?.elements) && geometry.elements.length >= channels &&
    geometry.elements.every(element => Array.isArray(element.position) &&
      element.position.length === 3 && element.position.every(finite));
  const spatial = coherent && channels >= 3 && positioned &&
    finite(geometry?.x_axis_bearing_deg_true);
  return {receiver_type: type, coherent_channels: coherent ? channels : 0,
    standards: ['FM', 'DAB', 'ATSC', 'DVB-T/T2'],
    managed_retune: kraken ? 'suite-v2-readback-and-calibration' :
      coherent ? 'processor-owned-restart' : 'unsupported',
    rf_prescreen: coherent,
    spatial_reflection_assessment: spatial ? 'multi-element' :
      coherent && channels === 2 ? 'two-channel-limited' : 'unavailable',
    automatic_bearing_safe_selection: spatial,
    limitation: spatial ? null : coherent && channels === 2 ?
      'Two coherent channels can rank reference and radar performance, but cannot independently certify a reflection-safe multi-element bearing manifold.' :
      'A coherent receiver is required for automated illuminator assessment.'};
}

const DEFAULT_LIMITS = Object.freeze({minimum_cnr_db: 8, maximum_clipping_fraction: 1e-4,
  minimum_occupied_bandwidth_fraction: .5, minimum_ambiguity_suppression_db: 8,
  minimum_cancellation_db: 20,
  minimum_repeat_correlation: .98, minimum_ideal_correlation: .4,
  maximum_amplitude_span_db: 6, maximum_phase_curvature_deg: 60,
  minimum_adsb_opportunities: 5, minimum_matched_detections: 30,
  minimum_persistent_tracks: 2, minimum_matched_aircraft: 3, minimum_sectors: 2,
  minimum_held_out_observations: 30,
  maximum_held_out_bearing_error_deg: 20});

function limitsFrom(value) {
  if (value === undefined) return {...DEFAULT_LIMITS};
  if (!value || typeof value !== 'object' || Array.isArray(value))
    throw new Error('Illuminator survey limits must be an object');
  const limits = {...DEFAULT_LIMITS};
  for (const [key, setting] of Object.entries(value)) {
    if (!Object.hasOwn(DEFAULT_LIMITS, key))
      throw new Error(`Unknown illuminator survey limit ${key}`);
    if (!finite(setting) || setting < 0)
      throw new Error(`Illuminator survey limit ${key} must be a non-negative number`);
    if ((key.includes('correlation') || key.includes('fraction')) && setting > 1)
      throw new Error(`Illuminator survey limit ${key} must not exceed 1`);
    limits[key] = setting;
  }
  return limits;
}

function evaluateCandidate(candidate, capabilities, suppliedLimits = {}) {
  const limits = limitsFrom(suppliedLimits);
  const reference = candidate?.reference || {}, spatial = candidate?.spatial || {};
  const tracking = candidate?.tracking || {}, failures = [], missing = [];
  if (!finite(candidate?.frequency_hz) || candidate.frequency_hz <= 0)
    failures.push('invalid-frequency');
  if (!finite(reference.cnr_db)) missing.push('reference.cnr_db');
  else if (reference.cnr_db < limits.minimum_cnr_db) failures.push('reference-too-weak');
  if (!finite(reference.clipping_fraction)) missing.push('reference.clipping_fraction');
  else if (reference.clipping_fraction > limits.maximum_clipping_fraction)
    failures.push('receiver-clipping');
  if (!finite(reference.occupied_bandwidth_fraction)) missing.push('reference.occupied_bandwidth_fraction');
  else if (reference.occupied_bandwidth_fraction < limits.minimum_occupied_bandwidth_fraction ||
      reference.occupied_bandwidth_fraction > 1) failures.push('insufficient-occupied-bandwidth');
  if (!finite(reference.ambiguity_pslr_db)) missing.push('reference.ambiguity_pslr_db');
  else if (reference.ambiguity_pslr_db > -limits.minimum_ambiguity_suppression_db)
    failures.push('poor-ambiguity-sidelobes');
  if (!finite(reference.cancellation_db)) missing.push('reference.cancellation_db');
  else if (reference.cancellation_db < limits.minimum_cancellation_db)
    failures.push('poor-cancellation');

  let bearingStatus = 'not-requested';
  if (candidate?.bearing_required === true) {
    if (capabilities?.spatial_reflection_assessment === 'multi-element') {
      bearingStatus = 'eligible';
      const spatialChecks = [
        ['repeat_principal_correlation', limits.minimum_repeat_correlation, 'minimum',
          'spatial-response-unstable'],
        ['ideal_correlation', limits.minimum_ideal_correlation, 'minimum',
          'direct-path-spatial-mismatch'],
        ['coherent_component_amplitude_span_db', limits.maximum_amplitude_span_db, 'maximum',
          'severe-channel-imbalance'],
        ['phase_curvature_peak_to_peak_deg', limits.maximum_phase_curvature_deg, 'maximum',
          'severe-frequency-dependent-multipath']
      ];
      for (const [field, boundary, direction, reason] of spatialChecks) {
        if (!finite(spatial[field])) missing.push(`spatial.${field}`);
        else if (direction === 'minimum' ? spatial[field] < boundary : spatial[field] > boundary)
          failures.push(reason);
      }
      if (spatial.transmitter_coordinates_verified !== true)
        missing.push('spatial.transmitter_coordinates_verified');
      if (failures.some(reason => reason.startsWith('spatial-') ||
          reason.includes('mismatch') || reason.includes('imbalance') || reason.includes('multipath')))
        bearingStatus = 'unsafe';
      else if (missing.some(field => field.startsWith('spatial.'))) bearingStatus = 'unproven';
    } else {
      bearingStatus = 'externally-unverified';
      missing.push('multi-element-spatial-reflection-assessment');
    }
  }

  const evidence = [
    ['matched_aircraft', limits.minimum_matched_aircraft],
    ['sectors', limits.minimum_sectors],
    ['held_out_observations', limits.minimum_held_out_observations]
  ];
  for (const [field, threshold] of evidence) {
    if (!finite(tracking[field])) missing.push(`tracking.${field}`);
    else if (tracking[field] < threshold) failures.push(`insufficient-${field.replaceAll('_', '-')}`);
  }
  if (candidate?.bearing_required === true) {
    if (!finite(tracking.held_out_bearing_error_deg)) missing.push('tracking.held_out_bearing_error_deg');
    else if (tracking.held_out_bearing_error_deg > limits.maximum_held_out_bearing_error_deg)
      failures.push('held-out-bearing-error');
  }
  for (const [field, threshold] of [['adsb_opportunities', limits.minimum_adsb_opportunities],
    ['matched_detections', limits.minimum_matched_detections],
    ['persistent_tracks', limits.minimum_persistent_tracks]]) {
    if (!finite(tracking[field])) missing.push(`tracking.${field}`);
    else if (tracking[field] < threshold) failures.push(`insufficient-${field.replaceAll('_', '-')}`);
  }

  const hardFailure = failures.some(reason => !reason.startsWith('insufficient-'));
  const status = hardFailure ? 'rejected' : failures.length || missing.length ?
    'rf-prescreen-only' : 'qualified';
  const signalScore = [
    finite(reference.cnr_db) ? clamp((reference.cnr_db - 8) / 32) : 0,
    finite(reference.occupied_bandwidth_fraction) ? clamp(reference.occupied_bandwidth_fraction) : 0,
    finite(reference.ambiguity_pslr_db) ? clamp((-reference.ambiguity_pslr_db - 3) / 27) : 0,
    finite(reference.cancellation_db) ? clamp((reference.cancellation_db - 10) / 40) : 0
  ].reduce((sum, value) => sum + value, 0) / 4;
  const spatialScore = capabilities?.spatial_reflection_assessment !== 'multi-element' ? null : [
    finite(spatial.ideal_correlation) ? clamp((spatial.ideal_correlation - .4) / .6) : 0,
    finite(spatial.coherent_component_amplitude_span_db) ?
      clamp(1 - spatial.coherent_component_amplitude_span_db / 6) : 0,
    finite(spatial.phase_curvature_peak_to_peak_deg) ?
      clamp(1 - spatial.phase_curvature_peak_to_peak_deg / 60) : 0,
    finite(spatial.repeat_principal_correlation) ?
      clamp((spatial.repeat_principal_correlation - .98) / .02) : 0
  ].reduce((sum, value) => sum + value, 0) / 4;
  return {frequency_hz: candidate?.frequency_hz, name: candidate?.name || null,
    status, bearing_status: bearingStatus, failures: [...new Set(failures)],
    missing: [...new Set(missing)], scores: {signal: signalScore, spatial: spatialScore}};
}

function evaluateSurvey(candidates, config, limits) {
  if (!Array.isArray(candidates) || candidates.length === 0 || candidates.length > 4096)
    throw new Error('Illuminator survey requires candidate measurements');
  const capabilities = receiverCapabilities(config);
  const results = candidates.map(candidate => evaluateCandidate(candidate, capabilities, limits));
  const eligible = results.filter(result => result.status === 'qualified');
  eligible.sort((left, right) =>
    ((right.scores.signal + (right.scores.spatial ?? 0)) -
     (left.scores.signal + (left.scores.spatial ?? 0))));
  return {schema: 'vectorwarp-illuminator-survey-evaluation-v1', capabilities,
    results, selected: eligible[0] || null,
    selection_reason: eligible.length ? 'Highest fully qualified radar and spatial score.' :
      'No candidate passed both radar-performance evidence and bearing-safety gates.'};
}

module.exports = {DEFAULT_LIMITS, bandPlans, receiverCapabilities,
  evaluateCandidate, evaluateSurvey};
