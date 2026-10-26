'use strict';

const crypto = require('crypto');
const {labelPasses, validateSample} = require('./manifold-fitter');
const {validateNativeShadowManifoldRow} = require('./native-shadow-manifold-evidence');

const finite = value => typeof value === 'number' && Number.isFinite(value);
const sha256 = value => typeof value === 'string' && /^[a-f0-9]{64}$/.test(value);
const canonical = value => Array.isArray(value) ? `[${value.map(canonical).join(',')}]` :
  value && typeof value === 'object' ? `{${Object.keys(value).sort().map(key =>
    `${JSON.stringify(key)}:${canonical(value[key])}`).join(',')}}` : JSON.stringify(value);
const canonicalHash = value => crypto.createHash('sha256').update(canonical(value)).digest('hex');
const complexVector = (value, channels) => Array.isArray(value) && value.length === channels &&
  value.every(item => Array.isArray(item) && item.length === 2 && item.every(finite));
const complexMatrix = (value, channels) => Array.isArray(value) && value.length === channels &&
  value.every(row => complexVector(row, channels));
const finiteVector = (value, length) => Array.isArray(value) && value.length === length &&
  value.every(finite);

function configuredDirectAzimuth(configuration) {
  const rx = configuration.receiver_location, tx = configuration.transmitter_location;
  if (!finiteVector(rx, 3) || !finiteVector(tx, 3) ||
      !finite(configuration.x_axis_bearing_deg_true) ||
      (rx[0] === tx[0] && rx[1] === tx[1])) return NaN;
  const radians = Math.PI / 180, rxLat = rx[0] * radians, txLat = tx[0] * radians;
  const deltaLon = (tx[1] - rx[1]) * radians;
  const bearing = Math.atan2(Math.sin(deltaLon) * Math.cos(txLat),
    Math.cos(rxLat) * Math.sin(txLat) - Math.sin(rxLat) * Math.cos(txLat) * Math.cos(deltaLon));
  return Math.atan2(Math.sin(configuration.x_axis_bearing_deg_true * radians - bearing),
    Math.cos(configuration.x_axis_bearing_deg_true * radians - bearing));
}

function validateDetectionSample(sample, {requireDetectionContext = false} = {}) {
  validateSample(sample);
  if (typeof sample.icao !== 'string' || !sample.icao.trim() ||
      !finite(sample.radar_timestamp_ms) || sample.radar_timestamp_ms < 0 ||
      sample.truth.elevation_deg < -90 || sample.truth.elevation_deg > 90 ||
      !(Number.isFinite(Math.hypot(...sample.response.flat())) && Math.hypot(...sample.response.flat()) > 0))
    throw new Error('Detection evidence requires a physical direction, nonzero response, aircraft and sample timestamp');
  const local = sample.local_context || {};
  const covariance = local.covariance || sample.covariance;
  const covarianceSamples = local.covariance_samples ?? sample.covariance_samples;
  const context = local.detection;
  const gain = sample.configuration?.receiver_gain_v2;
  const captureGain = gain?.requested_capture_channel_gains;
  const actualGain = gain?.actual_channel_gains_db;
  const identity = sample.manifold_identity;
  const requestedGain = Array.isArray(gain?.requested_channel_gains) ?
    gain.requested_channel_gains : finite(gain?.requested_common_gain) ?
      Array(sample.channels).fill(gain.requested_common_gain) : null;
  const validGain = gain && (gain.requested_common_gain === 'keep' ||
    finite(gain.requested_common_gain)) && (gain.requested_channel_gains === null ||
    (Array.isArray(gain.requested_channel_gains) &&
     gain.requested_channel_gains.length === sample.channels &&
     gain.requested_channel_gains.every(finite))) &&
    (captureGain === undefined || (Array.isArray(captureGain) &&
      captureGain.length >= sample.channels && captureGain.every(finite))) &&
    Array.isArray(actualGain) && actualGain.length === sample.channels &&
    actualGain.every(finite) && Array.isArray(requestedGain) &&
    canonical(requestedGain) === canonical(identity?.requested_gain_db) &&
    canonical(actualGain) === canonical(identity?.actual_gain_db);
  const validCovariance = covariance === undefined ||
    (complexMatrix(covariance, sample.channels) && Number.isInteger(covarianceSamples) &&
     covarianceSamples >= 1);
  const validContext = context && covariance !== undefined &&
    complexMatrix(covariance, sample.channels) && Number.isInteger(covarianceSamples) &&
    covarianceSamples >= 1 && context.same_iq === true &&
    finite(context.target?.baseline_statistic) && context.target.baseline_statistic >= 0 &&
    typeof context.target.baseline_detected === 'boolean' &&
    context.target.baseline_detected === (context.target.baseline_statistic >= context.cfar?.baseline_threshold) &&
    Array.isArray(context.nuisance) && context.nuisance.length >= 1 &&
    context.nuisance.every(row => complexVector(row?.response, sample.channels) &&
      finite(row.baseline_statistic) && row.baseline_statistic >= 0 &&
      typeof row.baseline_detected === 'boolean' &&
      row.baseline_detected === (row.baseline_statistic >= context.cfar?.baseline_threshold)) &&
    finite(context.cfar?.pfa) && context.cfar.pfa > 0 && context.cfar.pfa < 1 &&
    Number.isInteger(context.cfar.training_cells) && context.cfar.training_cells >= 1 &&
    finite(context.cfar.effective_trials) && context.cfar.effective_trials >= 1 &&
    finite(context.cfar.baseline_threshold) &&
    sample.support?.covariance_samples === covarianceSamples &&
    sample.support?.local_nuisance_cells === context.nuisance.length;
  if (sample.schema !== 'vectorwarp-manifold-sample-v2' ||
      sample.configuration?.frequency_hz !== sample.frequency_hz ||
      sample.configuration?.sample_rate_hz !== sample.sample_rate_hz ||
      !finite(sample.sample_rate_hz) || sample.sample_rate_hz <= 0 ||
      sample.configuration?.config_fingerprint !== sample.config_fingerprint ||
      !validGain || sample.support?.adsb_label_only !== true ||
      !finite(sample.uncertainty?.truth_age_s) || !finite(sample.uncertainty?.delay_error_km) ||
      !finite(sample.uncertainty?.doppler_error_hz) ||
      !finite(sample.uncertainty?.normalized_match_error) || !validCovariance ||
      (covariance !== undefined && sample.support?.covariance_samples !== covarianceSamples) ||
      ((context || requireDetectionContext) && !validContext) ||
      (!context && sample.support?.local_nuisance_cells !== 0))
    throw new Error(requireDetectionContext ?
      'Detection replay requires covariance-backed same-IQ target, nuisance, and CFAR evidence' :
      'Detection fitting requires exact configuration, gain, support, and uncertainty evidence');
  return {...sample, _detection_covariance: covariance,
    _detection_covariance_samples: covarianceSamples, _detection_context: context};
}

function validateDetectionReplay(record) {
  if (!record || record.schema !== 'vectorwarp-detection-manifold-replay-v1' ||
      !['miss', 'subthreshold', 'detected'].includes(record.validation?.baseline_outcome) ||
      typeof record.validation?.pass_id !== 'string' || !record.validation.pass_id.trim() ||
      record.provenance?.source !== 'independent-iq-replay' ||
      record.provenance?.independent_from_positive_corpus !== true ||
      typeof record.provenance?.corpus_id !== 'string' || !record.provenance.corpus_id.trim() ||
      typeof record.provenance?.replay_id !== 'string' || !record.provenance.replay_id.trim() ||
      !/^[a-f0-9]{64}$/.test(record.provenance?.corpus_sha256 || '') ||
      !/^[a-f0-9]{64}$/.test(record.provenance?.iq_sha256 || ''))
    throw new Error('Invalid independent detection replay provenance');
  const sample = validateDetectionSample({...record, schema: 'vectorwarp-manifold-sample-v2'},
    {requireDetectionContext: true});
  if ((record.validation.baseline_outcome === 'detected') !==
      record.local_context.detection.target.baseline_detected)
    throw new Error('Replay outcome disagrees with the same-IQ baseline decision');
  return {...sample, schema: record.schema, _pass_id: record.validation.pass_id.trim(),
    _validation_replay: true, _baseline_outcome: record.validation.baseline_outcome};
}

function close(left, right) {
  return Math.abs(left - right) <= Math.max(1e-9, Math.abs(right) * 1e-6);
}

function cfarSurvival(alpha, trainingCells, integrationCpis) {
  const x = trainingCells / (trainingCells + alpha);
  if (!(x > 0) || x > 1) return 0;
  const total = trainingCells * integrationCpis + integrationCpis - 1;
  let term = x ** total, result = term;
  const ratio = (1 - x) / x;
  for (let index = 1; index < integrationCpis; ++index) {
    term *= (total - index + 1) / index * ratio;
    result += term;
  }
  return result;
}

function productionCfarAlpha(pfa, trainingCells, integrationCpis) {
  let low = 0, high = 1;
  while (cfarSurvival(high, trainingCells, integrationCpis) > pfa && high < 1e12)
    high *= 2;
  for (let iteration = 0; iteration < 96; ++iteration) {
    const middle = (low + high) / 2;
    if (cfarSurvival(middle, trainingCells, integrationCpis) > pfa) low = middle;
    else high = middle;
  }
  return high;
}

function validateProductionArm(arm, bank, cut, detector) {
  if (!arm || arm.bank_sha256 !== bank.sha256 || arm.beams_evaluated !== bank.beams ||
      !finite(arm.cut_power) || arm.cut_power < 0 ||
      !finite(arm.training_mean_power) || !(arm.training_mean_power > 0) ||
      !finite(arm.alpha) || arm.alpha < 0 || !finite(arm.threshold) || arm.threshold < 0 ||
      typeof arm.detected !== 'boolean')
    throw new Error('Production replay arm is incomplete or did not evaluate the full beam bank');
  const expectedAlpha = productionCfarAlpha(detector.detector_pfa,
    cut.training_cells, detector.integration_cpis);
  if (!close(arm.alpha, expectedAlpha) ||
      !close(arm.threshold, arm.alpha * arm.training_mean_power) ||
      arm.detected !== (arm.cut_power > arm.threshold))
    throw new Error('Production replay arm disagrees with raw CUT/training CFAR accounting');
}

// This is an import boundary for a future native replay generator. It is
// intentionally stricter than the covariance-normalized diagnostic rows above:
// both arms must attest the same complete CUT/training grid and must expose the
// raw production-scale values from a full beam-bank search.
function validateProductionReplay(record) {
  const provenance = record?.provenance || {}, configuration = record?.configuration || {};
  const detector = record?.detector || {}, banks = record?.banks || {}, replay = record?.replay || {};
  const pass = record?.pass || {}, truth = pass.truth || {};
  const gain = configuration.receiver_gain_v2 || {};
  const baselineDescriptor = banks.baseline?.descriptor;
  if (record?.schema !== 'vectorwarp-detection-manifold-production-replay-v1' ||
      typeof pass.id !== 'string' || !pass.id.trim() ||
      !['positive', 'miss', 'subthreshold'].includes(pass.label) ||
      provenance.source !== 'vectorwarp-native-raw-iq-full-bank-replay' ||
      provenance.raw_iq_consumed !== true ||
      provenance.independent_from_positive_corpus !== true ||
      !sha256(provenance.corpus_sha256) || !sha256(provenance.iq_sha256) ||
      !sha256(provenance.replay_id) || !sha256(provenance.input_grid_sha256) ||
      !sha256(provenance.generator_sha256) || !sha256(provenance.config_sha256) ||
      !sha256(provenance.generator_trust_policy_sha256) ||
      !sha256(provenance.native_export_sha256) ||
      provenance.native_export_sha256 !== provenance.input_grid_sha256 ||
      !sha256(provenance.receipt_generator_sha256) ||
      !sha256(provenance.frequency_sha256) || !sha256(provenance.sample_rate_sha256) ||
      !sha256(provenance.channel_layout_sha256) ||
      !sha256(provenance.gain_sha256) || !sha256(provenance.candidate_model_sha256) ||
      typeof configuration.config_fingerprint !== 'string' || !configuration.config_fingerprint ||
      !finite(configuration.frequency_hz) || configuration.frequency_hz <= 0 ||
      !finite(configuration.sample_rate_hz) || configuration.sample_rate_hz <= 0 ||
      !Number.isInteger(configuration.channels) || configuration.channels < 2 ||
      configuration.channels > 8 ||
      !Array.isArray(configuration.channel_layout) ||
      configuration.channel_layout.length !== configuration.channels ||
      configuration.channel_layout.some(value => !Number.isInteger(value) || value < 0) ||
      new Set(configuration.channel_layout).size !== configuration.channels ||
      configuration.fusion_mode !== 'coherent_beam_bank' ||
      !Number.isInteger(configuration.beam_count) || configuration.beam_count < 1 ||
      !Array.isArray(configuration.positions_m) ||
      configuration.positions_m.length !== configuration.channels ||
      !configuration.positions_m.every(position => finiteVector(position, 3)) ||
      !finiteVector(configuration.receiver_location, 3) ||
      !finiteVector(configuration.transmitter_location, 3) ||
      !finite(configuration.x_axis_bearing_deg_true) ||
      !finite(configuration.direct_path_azimuth_rad) ||
      !close(configuration.direct_path_azimuth_rad, configuredDirectAzimuth(configuration)) ||
      !finite(configuration.direct_path_null_db) || configuration.direct_path_null_db < 0 ||
      configuration.direct_path_null_db > 40 ||
      provenance.frequency_sha256 !== canonicalHash(configuration.frequency_hz) ||
      provenance.sample_rate_sha256 !== canonicalHash(configuration.sample_rate_hz) ||
      provenance.channel_layout_sha256 !== canonicalHash(configuration.channel_layout) ||
      !finite(gain.requested_common_gain) ||
      !(gain.requested_channel_gains === null ||
        (Array.isArray(gain.requested_channel_gains) &&
         gain.requested_channel_gains.length === configuration.channels &&
         gain.requested_channel_gains.every(finite))) ||
      !(gain.requested_capture_channel_gains === undefined ||
        (Array.isArray(gain.requested_capture_channel_gains) &&
         gain.requested_capture_channel_gains.every(finite) &&
         configuration.channel_layout.every(channel =>
           channel < gain.requested_capture_channel_gains.length))) ||
      !Array.isArray(gain.actual_channel_gains_db) ||
      gain.actual_channel_gains_db.length !== configuration.channels ||
      !gain.actual_channel_gains_db.every(finite) ||
      provenance.gain_sha256 !== canonicalHash(gain) ||
      detector.contract !== 'vectorwarp-production-ca-cfar-full-bank-v1' ||
      !finite(detector.configured_pfa) || detector.configured_pfa <= 0 ||
      detector.configured_pfa >= 1 || !finite(detector.detector_pfa) ||
      detector.detector_pfa <= 0 || detector.detector_pfa >= 1 ||
      !finite(detector.effective_beam_trials) || detector.effective_beam_trials < 1 ||
      detector.motion_trials !== 1 || detector.integration_cpis !== 1 ||
      !Number.isInteger(detector.guard_cells) || detector.guard_cells < 0 ||
      !Number.isInteger(detector.training_cells_per_side) ||
      detector.training_cells_per_side < 1 ||
      !banks.baseline || !banks.candidate || !sha256(banks.baseline.sha256) ||
      !sha256(banks.candidate.sha256) || !Number.isInteger(banks.baseline.beams) ||
      banks.baseline.beams < 1 || banks.baseline.beams !== banks.candidate.beams ||
      banks.baseline.beams !== configuration.beam_count ||
      !baselineDescriptor || typeof baselineDescriptor !== 'object' ||
      baselineDescriptor.frequency_hz !== configuration.frequency_hz ||
      baselineDescriptor.beams !== banks.baseline.beams ||
      canonical(baselineDescriptor.positions_m) !== canonical(configuration.positions_m) ||
      baselineDescriptor.direct_path_azimuth_rad !== configuration.direct_path_azimuth_rad ||
      baselineDescriptor.direct_path_null_db !== configuration.direct_path_null_db ||
      canonicalHash(baselineDescriptor) !== banks.baseline.sha256 ||
      banks.candidate.runtime_compatible !== true ||
      !Array.isArray(banks.candidate.elevation_range_deg) ||
      banks.candidate.elevation_range_deg.length !== 2 ||
      !banks.candidate.elevation_range_deg.every(finite) ||
      banks.candidate.elevation_range_deg[0] > banks.candidate.elevation_range_deg[1] ||
      truth.selection !== 'target-cut-only-no-beam-selection' ||
      typeof truth.target_cut_id !== 'string' || !truth.target_cut_id ||
      !finite(truth.local_azimuth_deg) || !finite(truth.elevation_deg) ||
      truth.elevation_deg < banks.candidate.elevation_range_deg[0] ||
      truth.elevation_deg > banks.candidate.elevation_range_deg[1] ||
      detector.effective_beam_trials > banks.baseline.beams ||
      !close(detector.detector_pfa, detector.configured_pfa /
        (detector.effective_beam_trials * detector.motion_trials)) ||
      replay.same_iq !== true || replay.complete_grid !== true ||
      replay.truth_selected_beam !== false || replay.support_complete !== true ||
      !Array.isArray(replay.cuts) || replay.cuts.length < 2)
    throw new Error('Invalid production full-bank replay contract');
  let targets = 0, nuisances = 0;
  const cutIds = new Set();
  for (const cut of replay.cuts) {
    if (!cut || typeof cut.id !== 'string' || !cut.id || cutIds.has(cut.id) ||
        !['target', 'nuisance'].includes(cut.role) ||
        cut.input_grid_sha256 !== provenance.input_grid_sha256 ||
        !Number.isInteger(cut.training_cells) || cut.training_cells < 1)
      throw new Error('Production replay CUT/training grid is incomplete');
    cutIds.add(cut.id);
    if (cut.role === 'target') ++targets; else ++nuisances;
    validateProductionArm(cut.baseline, banks.baseline, cut, detector);
    validateProductionArm(cut.candidate, banks.candidate, cut, detector);
  }
  if (targets !== 1 || nuisances < 1)
    throw new Error('Production replay requires one target CUT and local nuisance CUTs');
  if (!replay.cuts.some(cut => cut.role === 'target' && cut.id === truth.target_cut_id))
    throw new Error('Production truth label does not identify the target CUT');
  const target = replay.cuts.find(cut => cut.role === 'target');
  if ((pass.label === 'positive') !== target.baseline.detected)
    throw new Error('Production pass label disagrees with the baseline target decision');
  return {...record, _pass_id: pass.id.trim(), _baseline_outcome: pass.label,
    _production_replay: true};
}

function buildDetectionCorpus(rawSamples, {corpusSha256 = null, resolveNativeShadowKey = null} = {}) {
  const invalid = [], accepted = [], replay = [], productionReplay = [], shadowTraining = [];
  for (const sample of rawSamples) try {
    if (sample?.schema === 'vectorwarp-detection-manifold-production-replay-v1')
      productionReplay.push(validateProductionReplay(sample));
    else if (sample?.schema === 'vectorwarp-detection-manifold-replay-v1')
      replay.push(validateDetectionReplay(sample));
    else if (sample?.schema === 'vectorwarp-native-shadow-manifold-row-v1')
      shadowTraining.push(validateNativeShadowManifoldRow(sample, {resolveKey: resolveNativeShadowKey}));
    else accepted.push(validateDetectionSample(sample));
  }
  catch (error) { invalid.push(error.message); }
  // Pass labeling is chronological: shuffled or repeated frames may not invent
  // additional independent flights. A return to an earlier RF/gain identity
  // starts a new collection epoch rather than resurrecting its old samples.
  accepted.sort((a, b) => a.radar_timestamp_ms - b.radar_timestamp_ms);
  const latest = accepted.at(-1) || replay.at(-1);
  const fingerprint = latest?.config_fingerprint || null;
  const gain = canonical(latest?.configuration?.receiver_gain_v2);
  const sameEpoch = row => row.config_fingerprint === fingerprint &&
    row.channels === latest.channels && row.frequency_hz === latest.frequency_hz &&
    row.sample_rate_hz === latest.sample_rate_hz &&
    canonical(row.configuration.receiver_gain_v2) === gain &&
    canonical(row.manifold_identity) === canonical(latest.manifold_identity);
  let epochStart = accepted.length;
  while (epochStart > 0 && sameEpoch(accepted[epochStart - 1])) --epochStart;
  const epochTimestamp = accepted[epochStart]?.radar_timestamp_ms ?? latest?.radar_timestamp_ms;
  const evidenceKey = fingerprint ? canonical({config_fingerprint: fingerprint,
    collection_epoch_start_ms: epochTimestamp,
    frequency_hz: latest.frequency_hz, sample_rate_hz: latest.sample_rate_hz,
    channels: latest.channels, receiver_gain_v2: latest.configuration?.receiver_gain_v2,
    manifold_identity: latest.manifold_identity}) : null;
  const current = fingerprint ? accepted.slice(epochStart) : [];
  const validation = fingerprint ? replay.filter(row => row.config_fingerprint === fingerprint &&
    row.radar_timestamp_ms >= epochTimestamp &&
    row.channels === latest.channels && row.frequency_hz === latest.frequency_hz &&
    row.sample_rate_hz === latest.sample_rate_hz &&
    canonical(row.configuration.receiver_gain_v2) === gain &&
    canonical(row.manifold_identity) === canonical(latest.manifold_identity)) : [];
  const productionValidation = fingerprint ? productionReplay.filter(row =>
    row.configuration.config_fingerprint === fingerprint &&
    row.configuration.channels === latest.channels &&
    row.configuration.frequency_hz === latest.frequency_hz &&
    row.configuration.sample_rate_hz === latest.sample_rate_hz &&
    canonical({requested_common_gain: row.configuration.receiver_gain_v2.requested_common_gain,
      requested_channel_gains: row.configuration.receiver_gain_v2.requested_channel_gains,
      ...(row.configuration.receiver_gain_v2.requested_capture_channel_gains !== undefined ?
        {requested_capture_channel_gains:
          row.configuration.receiver_gain_v2.requested_capture_channel_gains} : {}),
      actual_channel_gains_db: row.configuration.receiver_gain_v2.actual_channel_gains_db}) ===
      gain) : [];
  const labelled = labelPasses(current);
  if (validation.length) {
    if (!/^[a-f0-9]{64}$/.test(corpusSha256 || ''))
      throw new Error('Independent replay requires the exact positive corpus SHA-256');
    const declaredHashes = new Set(validation.map(row => row.provenance.corpus_sha256));
    if (declaredHashes.size !== 1 || !declaredHashes.has(corpusSha256))
      throw new Error('Replay provenance does not match the exact positive corpus snapshot');
    const positivePasses = new Set(labelled.map(row => row._pass_id));
    if (validation.some(row => positivePasses.has(row._pass_id)))
      throw new Error('Training and validation pass IDs must be disjoint');
    const outcomes = new Map(), replayIds = new Map(), iqCaptures = new Map();
    for (const row of validation) {
      const prior = outcomes.get(row._pass_id);
      if (prior && prior !== row._baseline_outcome)
        throw new Error(`Replay pass ${row._pass_id} has conflicting baseline outcomes`);
      outcomes.set(row._pass_id, row._baseline_outcome);
      const priorReplay = replayIds.get(row.provenance.replay_id);
      if (priorReplay && priorReplay !== row._pass_id)
        throw new Error(`Replay ID ${row.provenance.replay_id} is shared by multiple passes`);
      replayIds.set(row.provenance.replay_id, row._pass_id);
      const priorIq = iqCaptures.get(row.provenance.iq_sha256);
      if (priorIq && priorIq !== row._pass_id)
        throw new Error('One IQ capture cannot count as multiple independent validation passes');
      iqCaptures.set(row.provenance.iq_sha256, row._pass_id);
    }
  }
  if (productionValidation.length) {
    if (!sha256(corpusSha256))
      throw new Error('Production replay requires the exact positive corpus SHA-256');
    const positivePasses = new Set(labelled.map(row => row._pass_id));
    const passIds = new Set(), replayIds = new Set(), iqCaptures = new Set(), grids = new Set();
    for (const row of productionValidation) {
      if (row.provenance.corpus_sha256 !== corpusSha256)
        throw new Error('Production replay provenance does not match the exact positive corpus snapshot');
      if (positivePasses.has(row._pass_id) || passIds.has(row._pass_id))
        throw new Error('Production training and validation passes must be unique and disjoint');
      if (replayIds.has(row.provenance.replay_id) || iqCaptures.has(row.provenance.iq_sha256) ||
          grids.has(row.provenance.input_grid_sha256))
        throw new Error('Production replay IDs, IQ captures, and input grids must be independent by pass');
      passIds.add(row._pass_id); replayIds.add(row.provenance.replay_id);
      iqCaptures.add(row.provenance.iq_sha256); grids.add(row.provenance.input_grid_sha256);
    }
  }
  return {schema: 'vectorwarp-detection-manifold-corpus-v2', fingerprint, evidence_key: evidenceKey,
    accepted: labelled, training: labelled, holdout: validation, validation,
    production_validation: productionValidation, shadow_training: shadowTraining,
    miss_passes: new Set(validation.filter(row => row._baseline_outcome !== 'detected')
      .map(row => row._pass_id)).size,
    invalid_samples: invalid.length,
    ignored_fingerprint_samples: accepted.length + replay.length + productionReplay.length -
      current.length - validation.length - productionValidation.length,
    labels: 'adsb-collection-and-validation-only'};
}

module.exports = {validateDetectionSample, validateDetectionReplay, validateProductionReplay,
  productionCfarAlpha, buildDetectionCorpus};
