'use strict';

const finite = value => typeof value === 'number' && Number.isFinite(value);
const count = value => Number.isSafeInteger(value) && value >= 0;
const identifier = value => typeof value === 'string' && value.trim().length > 0;
const mean = values => values.reduce((sum, value) => sum + value, 0) / values.length;

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
    bearing_evidence_evaluation: spatial,
    automatic_fast_proxy_evaluation: coherent,
    automatic_survey_execution: false,
    automatic_bearing_safe_selection: false,
    limitation: spatial ? null : coherent && channels === 2 ?
      'Two coherent channels can rank reference and radar performance, but cannot independently certify a reflection-safe multi-element bearing manifold.' :
      'A coherent receiver is required for automated illuminator assessment.'};
}

const DEFAULT_LIMITS = Object.freeze({minimum_cnr_db: 8, maximum_clipping_fraction: 1e-4,
  maximum_near_rail_fraction: 1e-3, maximum_peak_fraction: .95,
  minimum_occupied_bandwidth_fraction: .5, minimum_ambiguity_suppression_db: 8,
  minimum_ambiguity_islr_suppression_db: 4, minimum_cancellation_db: 20,
  minimum_clutter_cancellation_db: 15, minimum_usable_dynamic_range_db: 35,
  minimum_synthetic_echo_suppression_db: 35, minimum_synthetic_echo_recovery_fraction: .9,
  maximum_channel_power_span_db: 12, maximum_residual_floor_excess_db: 8,
  maximum_off_target_detection_density: .01, maximum_stationary_track_density: .01,
  maximum_cpi_deadline_miss_fraction: .01, maximum_dropped_frame_fraction: 0,
  minimum_repeat_correlation: .98, minimum_ideal_correlation: .4,
  maximum_amplitude_span_db: 6, maximum_phase_curvature_deg: 60,
  minimum_adsb_opportunities: 5, minimum_sector_opportunities: 5,
  minimum_range_bin_opportunities: 2, minimum_matched_detections: 30,
  minimum_persistent_tracks: 2, minimum_persistent_passes: 2,
  minimum_matched_aircraft: 3, minimum_sectors: 2,
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
    if (['minimum_adsb_opportunities', 'minimum_sector_opportunities', 'minimum_range_bin_opportunities',
      'minimum_matched_detections', 'minimum_persistent_tracks', 'minimum_persistent_passes', 'minimum_matched_aircraft',
      'minimum_sectors', 'minimum_held_out_observations'].includes(key) && !count(setting))
      throw new Error(`Illuminator survey limit ${key} must be a non-negative integer`);
    limits[key] = setting;
  }
  return limits;
}

const COMPARISON_FIELDS = ['site_id', 'protocol_id', 'sector_definition_id',
  'common_support_id', 'baseline_id'];

// These IDs identify frozen site/protocol/support definitions; frequency and
// transmitter are the experiment variables and deliberately are not cohort keys.
function comparisonKey(candidate) {
  const comparison = candidate?.comparison;
  if (!comparison || COMPARISON_FIELDS.some(field => !identifier(comparison[field])) ||
      !Array.isArray(comparison.sector_ids) || comparison.sector_ids.length < 2 ||
      comparison.sector_ids.some(sector => !identifier(sector)) ||
      new Set(comparison.sector_ids).size !== comparison.sector_ids.length ||
      !Array.isArray(comparison.range_bin_ids) || comparison.range_bin_ids.length < 2 ||
      comparison.range_bin_ids.some(bin => !identifier(bin)) ||
      new Set(comparison.range_bin_ids).size !== comparison.range_bin_ids.length ||
      typeof candidate.bearing_required !== 'boolean') return null;
  return JSON.stringify([...COMPARISON_FIELDS.map(field => comparison[field]),
    [...comparison.sector_ids].sort(), [...comparison.range_bin_ids].sort(),
    candidate.bearing_required]);
}

const FAST_COMPARISON_FIELDS = ['site_id', 'receiver_id', 'protocol_id',
  'processing_fingerprint', 'injection_grid_id'];

function fastComparisonKey(candidate) {
  const comparison = candidate?.comparison;
  if (!comparison || FAST_COMPARISON_FIELDS.some(field => !identifier(comparison[field])) ||
      !finite(comparison.sample_rate_hz) || comparison.sample_rate_hz <= 0) return null;
  return JSON.stringify([...FAST_COMPARISON_FIELDS.map(field => comparison[field]),
    comparison.sample_rate_hz]);
}

function deriveSyntheticEchoMetrics(trials, minimumRecovery = .9) {
  if (!Array.isArray(trials) || trials.length === 0 || trials.length > 65536 ||
      !finite(minimumRecovery) || minimumRecovery <= 0 || minimumRecovery > 1)
    throw new Error('Synthetic echo trials and recovery target are invalid');
  const ids = new Set(), levels = new Map();
  for (const trial of trials) {
    if (!trial || !identifier(trial.trial_id) || ids.has(trial.trial_id) ||
        !finite(trial.echo_ratio_db) || trial.echo_ratio_db > 0 ||
        !Number.isSafeInteger(trial.delay_bin) || !Number.isSafeInteger(trial.doppler_bin) ||
        typeof trial.detected !== 'boolean')
      throw new Error('Synthetic echo trial provenance is invalid');
    ids.add(trial.trial_id);
    const level = levels.get(trial.echo_ratio_db) || new Map();
    const cellKey = JSON.stringify([trial.delay_bin, trial.doppler_bin]);
    const cell = level.get(cellKey) || {trials: 0, detected: 0};
    cell.trials += 1;
    if (trial.detected) cell.detected += 1;
    level.set(cellKey, cell);
    levels.set(trial.echo_ratio_db, level);
  }
  const cellKeys = cells => [...cells.keys()].sort();
  const expectedCells = JSON.stringify(cellKeys([...levels.values()][0]));
  const rows = [...levels.entries()].sort((left, right) => left[0] - right[0]).map(([ratio, cells]) => {
    if (JSON.stringify(cellKeys(cells)) !== expectedCells)
      throw new Error('Synthetic echo grid differs between amplitude levels');
    const rates = [...cells.values()].map(cell => cell.detected / cell.trials);
    return {echo_ratio_db: ratio, recovery_fraction: mean(rates),
      worst_cell_recovery_fraction: Math.min(...rates)};
  });
  // A usable threshold must remain usable at every stronger injected level;
  // this rejects noisy, non-monotonic one-level wins.
  let threshold = null;
  for (let index = 0; index < rows.length; ++index) {
    if (rows.slice(index).every(row => row.worst_cell_recovery_fraction >= minimumRecovery)) {
      threshold = rows[index];
      break;
    }
  }
  return {synthetic_echo_trials: trials.length,
    synthetic_echo_threshold_db: threshold?.echo_ratio_db ?? null,
    synthetic_echo_recovery_fraction: threshold?.worst_cell_recovery_fraction ?? 0,
    levels: rows};
}

// This is deliberately a proxy prescreen: weak delayed/Doppler-shifted copies
// of the measured reference are injected before the production canceller and
// detector. It rapidly measures the complete waveform/processing floor without
// claiming that a digitally injected echo proves RF aircraft performance.
function evaluateFastCandidate(candidate, capabilities, suppliedLimits = {}) {
  const limits = limitsFrom(suppliedLimits);
  const failures = [], missing = [];
  const reference = candidate?.reference || {};
  const sensitivity = candidate?.sensitivity || {};
  const dynamic = candidate?.dynamic_range || {};
  const operations = candidate?.operations || {};
  const channels = capabilities?.coherent_channels || 0;
  if (!finite(candidate?.frequency_hz) || candidate.frequency_hz <= 0)
    failures.push('invalid-frequency');
  if (!fastComparisonKey(candidate)) missing.push('fast-comparison-cohort');
  if (capabilities?.rf_prescreen !== true || channels < 2)
    failures.push('unsupported-receiver');

  const finiteGate = (object, field, boundary, direction, reason) => {
    if (!finite(object[field])) missing.push(field);
    else if (direction === 'minimum' ? object[field] < boundary : object[field] > boundary)
      failures.push(reason);
  };
  finiteGate(reference, 'cnr_db', limits.minimum_cnr_db, 'minimum', 'reference-too-weak');
  finiteGate(reference, 'occupied_bandwidth_fraction', limits.minimum_occupied_bandwidth_fraction,
    'minimum', 'insufficient-occupied-bandwidth');
  finiteGate(reference, 'ambiguity_pslr_db', -limits.minimum_ambiguity_suppression_db,
    'maximum', 'poor-ambiguity-sidelobes');
  finiteGate(reference, 'ambiguity_islr_db', -limits.minimum_ambiguity_islr_suppression_db,
    'maximum', 'poor-integrated-ambiguity-sidelobes');
  finiteGate(reference, 'cancellation_db', limits.minimum_cancellation_db,
    'minimum', 'poor-cancellation');
  finiteGate(reference, 'clutter_cancellation_db', limits.minimum_clutter_cancellation_db,
    'minimum', 'poor-clutter-cancellation');
  if (!['raw-adc', 'receiver-calibrated-adc'].includes(reference.adc_level_source))
    missing.push('adc_level_source');
  for (const [field, boundary, reason] of [
    ['channel_clipping_fractions', limits.maximum_clipping_fraction, 'receiver-channel-clipping'],
    ['channel_near_rail_fractions', limits.maximum_near_rail_fraction, 'receiver-near-rail']
  ]) {
    const values = reference[field];
    if (!Array.isArray(values) || values.length !== channels) missing.push(field);
    else if (values.some(value => !finite(value) || value < 0 || value > 1))
      failures.push(`invalid-${field.replaceAll('_', '-')}`);
    else if (values.some(value => value > boundary)) failures.push(reason);
  }

  if (sensitivity.gain_strategy !== 'measured-highest-safe-common')
    missing.push('gain_strategy');
  if (!finite(sensitivity.selected_gain_db)) missing.push('selected_gain_db');
  finiteGate(sensitivity, 'peak_fraction', limits.maximum_peak_fraction,
    'maximum', 'insufficient-receiver-headroom');
  if (finite(sensitivity.peak_fraction) && sensitivity.peak_fraction < 0)
    failures.push('invalid-peak-fraction');
  if (!finite(sensitivity.input_referred_noise_db)) missing.push('input_referred_noise_db');

  if (dynamic.injection_stage !== 'pre-cancellation-digital')
    missing.push('injection_stage');
  let derivedEcho = null;
  try {
    derivedEcho = deriveSyntheticEchoMetrics(dynamic.synthetic_echo_trials_data,
      limits.minimum_synthetic_echo_recovery_fraction);
    if (dynamic.synthetic_echo_trials !== derivedEcho.synthetic_echo_trials ||
        dynamic.synthetic_echo_threshold_db !== derivedEcho.synthetic_echo_threshold_db ||
        dynamic.synthetic_echo_recovery_fraction !== derivedEcho.synthetic_echo_recovery_fraction)
      failures.push('inconsistent-synthetic-echo-summary');
  } catch (error) {
    missing.push('synthetic_echo_trials_data');
  }
  finiteGate(dynamic, 'synthetic_echo_threshold_db',
    -limits.minimum_synthetic_echo_suppression_db, 'maximum', 'poor-synthetic-echo-sensitivity');
  if (finite(dynamic.synthetic_echo_threshold_db) && dynamic.synthetic_echo_threshold_db > 0)
    failures.push('invalid-synthetic-echo-threshold');
  finiteGate(dynamic, 'synthetic_echo_recovery_fraction',
    limits.minimum_synthetic_echo_recovery_fraction, 'minimum', 'unreliable-synthetic-echo-recovery');
  if (finite(dynamic.synthetic_echo_recovery_fraction) &&
      (dynamic.synthetic_echo_recovery_fraction < 0 || dynamic.synthetic_echo_recovery_fraction > 1))
    failures.push('invalid-synthetic-echo-recovery-fraction');
  finiteGate(dynamic, 'usable_dynamic_range_db', limits.minimum_usable_dynamic_range_db,
    'minimum', 'insufficient-usable-dynamic-range');
  finiteGate(dynamic, 'channel_power_span_db', limits.maximum_channel_power_span_db,
    'maximum', 'excessive-channel-power-span');
  finiteGate(dynamic, 'residual_floor_excess_db', limits.maximum_residual_floor_excess_db,
    'maximum', 'elevated-post-cancellation-floor');
  for (const field of ['usable_dynamic_range_db', 'channel_power_span_db', 'residual_floor_excess_db'])
    if (finite(dynamic[field]) && dynamic[field] < 0) failures.push(`invalid-${field.replaceAll('_', '-')}`);

  for (const [field, boundary, reason] of [
    ['off_target_detection_density', limits.maximum_off_target_detection_density, 'excessive-off-target-detections'],
    ['stationary_track_density', limits.maximum_stationary_track_density, 'excessive-stationary-tracks'],
    ['cpi_deadline_miss_fraction', limits.maximum_cpi_deadline_miss_fraction, 'cpi-deadline-regression'],
    ['dropped_frame_fraction', limits.maximum_dropped_frame_fraction, 'dropped-frames']
  ]) {
    if (!finite(operations[field])) missing.push(field);
    else if (operations[field] < 0 || operations[field] > 1)
      failures.push(`invalid-${field.replaceAll('_', '-')}`);
    else if (operations[field] > boundary) failures.push(reason);
  }

  return {frequency_hz: candidate?.frequency_hz, name: candidate?.name || null,
    status: failures.length ? 'rejected' : missing.length ? 'incomplete' : 'proxy-qualified',
    failures: [...new Set(failures)], missing: [...new Set(missing)], scores: {
      synthetic_echo_threshold_db: finite(dynamic.synthetic_echo_threshold_db) ?
        dynamic.synthetic_echo_threshold_db : null,
      synthetic_echo_recovery_fraction: finite(dynamic.synthetic_echo_recovery_fraction) ?
        dynamic.synthetic_echo_recovery_fraction : null,
      usable_dynamic_range_db: finite(dynamic.usable_dynamic_range_db) ? dynamic.usable_dynamic_range_db : null,
      residual_floor_excess_db: finite(dynamic.residual_floor_excess_db) ? dynamic.residual_floor_excess_db : null,
      selected_gain_db: finite(sensitivity.selected_gain_db) ? sensitivity.selected_gain_db : null,
      input_referred_noise_db: finite(sensitivity.input_referred_noise_db) ?
        sensitivity.input_referred_noise_db : null,
      ambiguity_pslr_db: finite(reference.ambiguity_pslr_db) ? reference.ambiguity_pslr_db : null
    }};
}

function evaluateFastSweep(candidates, config, limits) {
  if (!Array.isArray(candidates) || candidates.length === 0 || candidates.length > 4096)
    throw new Error('Fast illuminator sweep requires candidate measurements');
  const capabilities = receiverCapabilities(config);
  const results = candidates.map(candidate => evaluateFastCandidate(candidate, capabilities, limits));
  const keys = candidates.map(fastComparisonKey);
  const comparable = keys.every(key => key !== null && key === keys[0]);
  const eligible = comparable ? results.filter(result => result.status === 'proxy-qualified') : [];
  const compare = (left, right) =>
    left.scores.synthetic_echo_threshold_db - right.scores.synthetic_echo_threshold_db ||
    right.scores.synthetic_echo_recovery_fraction - left.scores.synthetic_echo_recovery_fraction ||
    right.scores.usable_dynamic_range_db - left.scores.usable_dynamic_range_db ||
    left.scores.residual_floor_excess_db - right.scores.residual_floor_excess_db ||
    right.scores.selected_gain_db - left.scores.selected_gain_db ||
    left.scores.input_referred_noise_db - right.scores.input_referred_noise_db ||
    left.scores.ambiguity_pslr_db - right.scores.ambiguity_pslr_db;
  eligible.sort(compare);
  const tied = eligible.length > 1 && compare(eligible[0], eligible[1]) === 0;
  return {schema: 'vectorwarp-illuminator-fast-sweep-v1', capabilities, results,
    selected: tied ? null : eligible[0] || null, provisional: true,
    requires_aircraft_validation: true,
    comparison_failures: comparable ? [] : ['incomparable-fast-sweep-cohorts'],
    selection_reason: tied ? 'Proxy-qualified candidates tie; collect another bounded block.' :
      eligible.length ? 'Lowest recoverable injected-echo/direct-path ratio, then recovery reliability, usable dynamic range, residual floor, safe gain, input-referred noise and ambiguity sidelobes.' :
        'No candidate passed the rapid RF, waveform, cancellation, synthetic-echo and processing-health gates.'};
}

function trackingEvidence(candidate, limits, failures, missing) {
  const tracking = candidate?.tracking || {};
  const sectors = comparisonKey(candidate) ? candidate.comparison.sector_ids : [];
  const rangeBins = comparisonKey(candidate) ? candidate.comparison.range_bin_ids : [];
  const metrics = ['adsb_opportunities', 'matched_passes', 'persistent_passes',
    'matched_detections', 'persistent_tracks', 'matched_aircraft', 'sectors',
    'held_out_observations'];
  for (const field of metrics) {
    if (tracking[field] === undefined) missing.push(`tracking.${field}`);
    else if (!count(tracking[field])) failures.push(`invalid-${field.replaceAll('_', '-')}`);
  }
  for (const [numerator, denominator] of [['matched_passes', 'adsb_opportunities'],
    ['persistent_passes', 'matched_passes'], ['matched_aircraft', 'matched_passes'],
    ['persistent_tracks', 'matched_detections']]) {
    if (count(tracking[numerator]) && count(tracking[denominator]) &&
        tracking[numerator] > tracking[denominator]) failures.push(`invalid-${numerator.replaceAll('_', '-')}`);
  }
  for (const [field, threshold] of [['adsb_opportunities', limits.minimum_adsb_opportunities],
    ['matched_detections', limits.minimum_matched_detections],
    ['persistent_tracks', limits.minimum_persistent_tracks],
    ['persistent_passes', limits.minimum_persistent_passes],
    ['matched_aircraft', Math.max(2, limits.minimum_matched_aircraft)],
    ['sectors', Math.max(2, limits.minimum_sectors)],
    ['held_out_observations', limits.minimum_held_out_observations]]) {
    if (count(tracking[field]) && tracking[field] < threshold)
      failures.push(`insufficient-${field.replaceAll('_', '-')}`);
  }

  const passes = new Map(), bySector = new Map(sectors.map(sector => [sector, []]));
  const bySupport = new Map(sectors.flatMap(sector => rangeBins.map(rangeBin =>
    [JSON.stringify([sector, rangeBin]), []])));
  if (!Array.isArray(tracking.eligible_passes)) missing.push('tracking.eligible_passes');
  else {
    for (const pass of tracking.eligible_passes) {
      if (!pass || !identifier(pass.pass_id) || !identifier(pass.aircraft_id) ||
          !identifier(pass.sector_id) || typeof pass.matched !== 'boolean' ||
          !identifier(pass.range_bin_id) || !rangeBins.includes(pass.range_bin_id) ||
          typeof pass.persistent !== 'boolean' || (pass.persistent && !pass.matched) ||
          passes.has(pass.pass_id) || !bySector.has(pass.sector_id)) {
        failures.push('invalid-eligible-pass-provenance');
        continue;
      }
      passes.set(pass.pass_id, pass);
      bySector.get(pass.sector_id).push(pass);
      bySupport.get(JSON.stringify([pass.sector_id, pass.range_bin_id])).push(pass);
    }
    const matched = [...passes.values()].filter(pass => pass.matched);
    const derived = {adsb_opportunities: passes.size, matched_passes: matched.length,
      persistent_passes: matched.filter(pass => pass.persistent).length,
      matched_aircraft: new Set(matched.map(pass => pass.aircraft_id)).size,
      sectors: new Set(matched.map(pass => pass.sector_id)).size};
    for (const [field, value] of Object.entries(derived)) {
      if (count(tracking[field]) && tracking[field] !== value)
        failures.push(`inconsistent-${field.replaceAll('_', '-')}`);
    }
    if (count(tracking.matched_detections) && tracking.matched_detections < matched.length)
      failures.push('invalid-matched-detections');
    if (count(tracking.persistent_tracks) && tracking.persistent_tracks < derived.persistent_passes)
      failures.push('invalid-persistent-tracks');
  }
  const sectorRates = sectors.map(sector_id => {
    const eligible = bySector.get(sector_id);
    if (eligible.length < Math.max(1, limits.minimum_sector_opportunities))
      failures.push('insufficient-sector-opportunities');
    return {sector_id, adsb_opportunities: eligible.length,
      matched_passes: eligible.filter(pass => pass.matched).length,
      persistent_passes: eligible.filter(pass => pass.persistent).length};
  });
  const supportRates = [...bySupport.entries()].map(([key, eligible]) => {
    const [sector_id, range_bin_id] = JSON.parse(key);
    if (eligible.length < Math.max(1, limits.minimum_range_bin_opportunities))
      failures.push('insufficient-range-support');
    return {sector_id, range_bin_id, adsb_opportunities: eligible.length,
      matched_passes: eligible.filter(pass => pass.matched).length,
      persistent_passes: eligible.filter(pass => pass.persistent).length};
  });

  const heldOut = tracking.held_out || {}, fitPasses = new Set(), fitAircraft = new Set();
  if (!Array.isArray(heldOut.fit_passes)) missing.push('tracking.held_out.fit_passes');
  else for (const pass of heldOut.fit_passes) {
    if (!pass || !identifier(pass.pass_id) || !identifier(pass.aircraft_id) ||
        fitPasses.has(pass.pass_id)) failures.push('invalid-fit-pass-provenance');
    else { fitPasses.add(pass.pass_id); fitAircraft.add(pass.aircraft_id); }
  }
  // All eligible passes belong to the evaluation split, including unmatched
  // opportunities. Training aircraft must not inflate the yield ranking.
  if ([...passes.values()].some(pass => fitPasses.has(pass.pass_id) || fitAircraft.has(pass.aircraft_id)))
    failures.push('evaluation-data-leakage');
  const observations = [], observationIds = new Set();
  if (!Array.isArray(heldOut.observations)) missing.push('tracking.held_out.observations');
  else {
    for (const observation of heldOut.observations) {
      const pass = passes.get(observation?.pass_id);
      if (!observation || !identifier(observation.observation_id) ||
          observationIds.has(observation.observation_id) || !pass || !pass.matched ||
          pass.aircraft_id !== observation.aircraft_id || pass.sector_id !== observation.sector_id) {
        failures.push('invalid-held-out-provenance');
        continue;
      }
      observationIds.add(observation.observation_id);
      if (fitPasses.has(observation.pass_id) || fitAircraft.has(observation.aircraft_id))
        failures.push('held-out-data-leakage');
      observations.push(observation);
    }
    if (count(tracking.held_out_observations) && tracking.held_out_observations !== observations.length)
      failures.push('inconsistent-held-out-observations');
    if (new Set(observations.map(item => item.aircraft_id)).size < Math.max(2, limits.minimum_matched_aircraft))
      failures.push('insufficient-held-out-aircraft');
    if (new Set(observations.map(item => item.sector_id)).size < Math.max(2, limits.minimum_sectors))
      failures.push('insufficient-held-out-sectors');
  }

  const sectorErrors = [];
  for (const sector_id of sectors) {
    const observed = observations.filter(item => item.sector_id === sector_id);
    if (!observed.length) missing.push(`tracking.held_out.sector:${sector_id}`);
    if (candidate?.bearing_required !== true || !observed.length) continue;
    if (observed.some(item => !finite(item.bearing_error_deg) || item.bearing_error_deg < 0 ||
        item.bearing_error_deg > 180 || !finite(item.baseline_bearing_error_deg) ||
        item.baseline_bearing_error_deg < 0 || item.baseline_bearing_error_deg > 180)) {
      failures.push('invalid-held-out-bearing-error');
      continue;
    }
    // Equal pass weight inside each sector prevents a long dwell from hiding
    // a bad pass; sectors also receive equal weight in the final ranking.
    const passIds = [...new Set(observed.map(item => item.pass_id))];
    const averageError = field => mean(passIds.map(pass_id =>
      mean(observed.filter(item => item.pass_id === pass_id).map(item => item[field]))));
    const error = averageError('bearing_error_deg');
    const baseline = averageError('baseline_bearing_error_deg');
    sectorErrors.push({sector_id, bearing_error_deg: error, baseline_bearing_error_deg: baseline});
    if (error > limits.maximum_held_out_bearing_error_deg) failures.push('held-out-bearing-error');
    if (error > baseline) failures.push('held-out-sector-regression');
  }
  const heldOutError = sectorErrors.length === sectors.length && sectors.length ?
    mean(sectorErrors.map(item => item.bearing_error_deg)) : null;
  if (candidate?.bearing_required === true) {
    if (tracking.held_out_bearing_error_deg === undefined) missing.push('tracking.held_out_bearing_error_deg');
    else if (!finite(tracking.held_out_bearing_error_deg) || tracking.held_out_bearing_error_deg < 0 ||
        tracking.held_out_bearing_error_deg > 180) failures.push('invalid-held-out-bearing-error');
    else if (heldOutError !== null && Math.abs(tracking.held_out_bearing_error_deg - heldOutError) > 1e-6)
      failures.push('inconsistent-held-out-bearing-error');
  }
  const rate = field => supportRates.length && supportRates.every(item => item.adsb_opportunities > 0) ?
    mean(supportRates.map(item => item[field] / item.adsb_opportunities)) : null;
  return {persistent_pass_rate: rate('persistent_passes'), matched_pass_rate: rate('matched_passes'),
    held_out_bearing_error_deg: heldOutError, sector_rates: sectorRates, support_rates: supportRates,
    held_out_sector_errors: sectorErrors};
}

function evaluateCandidate(candidate, capabilities, suppliedLimits = {}) {
  const limits = limitsFrom(suppliedLimits);
  const reference = candidate?.reference || {}, spatial = candidate?.spatial || {};
  const failures = [], missing = [];
  if (!finite(candidate?.frequency_hz) || candidate.frequency_hz <= 0) failures.push('invalid-frequency');
  if (typeof candidate?.bearing_required !== 'boolean') missing.push('bearing_required');
  if (!comparisonKey(candidate)) missing.push('comparison-cohort-and-sector-support');
  if (capabilities?.rf_prescreen !== true || !count(capabilities?.coherent_channels) ||
      capabilities.coherent_channels < 2) failures.push('unsupported-receiver');
  if (!finite(reference.cnr_db)) missing.push('reference.cnr_db');
  else if (reference.cnr_db < limits.minimum_cnr_db) failures.push('reference-too-weak');
  if (!finite(reference.clipping_fraction)) missing.push('reference.clipping_fraction');
  else if (reference.clipping_fraction < 0 || reference.clipping_fraction > 1)
    failures.push('invalid-clipping-fraction');
  else if (reference.clipping_fraction > limits.maximum_clipping_fraction) failures.push('receiver-clipping');
  if (!finite(reference.occupied_bandwidth_fraction)) missing.push('reference.occupied_bandwidth_fraction');
  else if (reference.occupied_bandwidth_fraction < 0 || reference.occupied_bandwidth_fraction > 1)
    failures.push('invalid-occupied-bandwidth-fraction');
  else if (reference.occupied_bandwidth_fraction < limits.minimum_occupied_bandwidth_fraction)
    failures.push('insufficient-occupied-bandwidth');
  if (!finite(reference.ambiguity_pslr_db)) missing.push('reference.ambiguity_pslr_db');
  else if (reference.ambiguity_pslr_db > -limits.minimum_ambiguity_suppression_db) failures.push('poor-ambiguity-sidelobes');
  if (!finite(reference.cancellation_db)) missing.push('reference.cancellation_db');
  else if (reference.cancellation_db < limits.minimum_cancellation_db) failures.push('poor-cancellation');

  // A common fixed gain can reward an overpowering direct path while hiding a
  // weaker illuminator's usable surveillance sensitivity. Compare candidates
  // at the highest safe gain selected by the same measured protocol, then
  // freeze that gain for every accepted block belonging to the candidate.
  const sensitivity = candidate?.sensitivity || {};
  if (sensitivity.gain_strategy !== 'measured-highest-safe-common')
    missing.push('sensitivity.gain_strategy');
  if (!finite(sensitivity.selected_gain_db)) missing.push('sensitivity.selected_gain_db');
  if (!finite(sensitivity.peak_fraction)) missing.push('sensitivity.peak_fraction');
  else if (sensitivity.peak_fraction < 0 || sensitivity.peak_fraction > 1)
    failures.push('invalid-peak-fraction');
  else if (sensitivity.peak_fraction > limits.maximum_peak_fraction)
    failures.push('insufficient-receiver-headroom');
  if (!finite(sensitivity.input_referred_noise_db))
    missing.push('sensitivity.input_referred_noise_db');

  let bearingStatus = candidate?.bearing_required === false ? 'not-requested' : 'unproven';
  if (candidate?.bearing_required === true) {
    if (capabilities?.spatial_reflection_assessment === 'multi-element') {
      const spatialFailures = failures.length;
      const spatialChecks = [
        ['repeat_principal_correlation', limits.minimum_repeat_correlation, 'minimum', 'spatial-response-unstable'],
        ['ideal_correlation', limits.minimum_ideal_correlation, 'minimum', 'direct-path-spatial-mismatch'],
        ['coherent_component_amplitude_span_db', limits.maximum_amplitude_span_db, 'maximum', 'severe-channel-imbalance'],
        ['phase_curvature_peak_to_peak_deg', limits.maximum_phase_curvature_deg, 'maximum', 'severe-frequency-dependent-multipath']
      ];
      for (const [field, boundary, direction, reason] of spatialChecks) {
        if (!finite(spatial[field])) missing.push(`spatial.${field}`);
        else if (spatial[field] < 0 || (field.includes('correlation') && spatial[field] > 1))
          failures.push(`invalid-spatial-${field.replaceAll('_', '-')}`);
        else if (direction === 'minimum' ? spatial[field] < boundary : spatial[field] > boundary) failures.push(reason);
      }
      if (spatial.transmitter_coordinates_verified !== true) missing.push('spatial.transmitter_coordinates_verified');
      bearingStatus = failures.length > spatialFailures ? 'unsafe' :
        missing.some(field => field.startsWith('spatial.')) ? 'unproven' : 'eligible';
    } else {
      bearingStatus = 'externally-unverified';
      missing.push('multi-element-spatial-reflection-assessment');
    }
  }
  const scores = trackingEvidence(candidate, limits, failures, missing);
  if (candidate?.bearing_required === true && bearingStatus === 'eligible') {
    if (failures.some(reason => reason.startsWith('held-out-') || reason.startsWith('invalid-held-out')))
      bearingStatus = 'unsafe';
    else if (failures.length || missing.length) bearingStatus = 'unproven';
  }
  const hardFailure = failures.some(reason => !reason.startsWith('insufficient-'));
  const status = hardFailure ? 'rejected' : failures.length || missing.length ? 'rf-prescreen-only' : 'qualified';
  scores.selected_gain_db = finite(sensitivity.selected_gain_db) ? sensitivity.selected_gain_db : null;
  scores.input_referred_noise_db = finite(sensitivity.input_referred_noise_db) ?
    sensitivity.input_referred_noise_db : null;
  return {frequency_hz: candidate?.frequency_hz, name: candidate?.name || null,
    status, bearing_status: bearingStatus, failures: [...new Set(failures)],
    missing: [...new Set(missing)], scores};
}

function evaluateSurvey(candidates, config, limits) {
  if (!Array.isArray(candidates) || candidates.length === 0 || candidates.length > 4096)
    throw new Error('Illuminator survey requires candidate measurements');
  const capabilities = receiverCapabilities(config);
  const results = candidates.map(candidate => evaluateCandidate(candidate, capabilities, limits));
  const keys = candidates.map(comparisonKey);
  const comparable = keys.every(key => key !== null && key === keys[0]);
  const eligible = comparable ? results.filter(result => result.status === 'qualified') : [];
  const compareYield = (left, right) =>
    right.scores.persistent_pass_rate - left.scores.persistent_pass_rate ||
    right.scores.matched_pass_rate - left.scores.matched_pass_rate ||
    right.scores.selected_gain_db - left.scores.selected_gain_db ||
    left.scores.input_referred_noise_db - right.scores.input_referred_noise_db ||
    (left.scores.held_out_bearing_error_deg ?? 0) - (right.scores.held_out_bearing_error_deg ?? 0);
  eligible.sort(compareYield);
  const tied = eligible.length > 1 && compareYield(eligible[0], eligible[1]) === 0;
  return {schema: 'vectorwarp-illuminator-survey-evaluation-v3', capabilities,
    results, selected: tied ? null : eligible[0] || null, recommendation_only: true,
    comparison_failures: comparable ? [] : ['incomparable-survey-cohorts'],
    selection_reason: tied ? 'Qualified candidates tie on pass yield, surveillance sensitivity and held-out error; more evidence is required.' : eligible.length ?
      'Highest equal-sector-by-range persistent-pass yield, then matched-pass yield, safely available receiver gain, lowest input-referred noise and held-out bearing error; recommendation only.' :
      comparable ? 'No candidate passed radar-performance evidence and requested bearing-safety gates.' :
        'Selection requires the same site, protocol, common support, sectors, baseline and explicit bearing requirement.'};
}

module.exports = {DEFAULT_LIMITS, bandPlans, receiverCapabilities, deriveSyntheticEchoMetrics,
  evaluateFastCandidate, evaluateFastSweep, evaluateCandidate, evaluateSurvey};
