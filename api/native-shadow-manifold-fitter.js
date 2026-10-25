'use strict';

const crypto = require('crypto');
const {canonicalHash, validateNativeShadowManifoldRow} = require('./native-shadow-manifold-evidence');
const unit = vector => { const n = Math.hypot(...vector.flat()); return Number.isFinite(n) && n > 1e-12 ? vector.map(([r, i]) => [r / n, i / n]) : null; };
const add = (a, b) => [a[0] + b[0], a[1] + b[1]];
const conjugate = vector => vector.map(([r, i]) => [r, -i]);
const inner = (left, right) => left.reduce((sum, [r, i], n) =>
  add(sum, [r * right[n][0] + i * right[n][1], r * right[n][1] - i * right[n][0]]), [0, 0]);
// Coherent power is the squared magnitude of the SUM, not a sum of channel powers.
const projectionPower = (response, vector) => { const [r, i] = inner(response, vector); return r * r + i * i; };
const split = pass => parseInt(crypto.createHash('sha256').update(pass).digest('hex').slice(0, 8), 16) % 5 === 0;
const azimuth = angle => ((angle % 360) + 360) % 360;
function align(vector, reference) {
  const [r, i] = inner(reference, vector), n = Math.hypot(r, i);
  return n > 1e-10 ? vector.map(([a, b]) => [(a * r + b * i) / n, (b * r - a * i) / n]) : null;
}
function canonicalPhase(vector) {
  const pivot = vector.reduce((best, value, index) => Math.hypot(...value) > Math.hypot(...vector[best]) ? index : best, 0);
  const [r, i] = vector[pivot], n = Math.hypot(r, i);
  return vector.map(([a, b]) => [(a * r + b * i) / n, (b * r - a * i) / n]);
}
function balancedMean(passes, field) {
  const reference = passes[0][0][field], means = [];
  for (const rows of passes) {
    let sum = reference.map(() => [0, 0]);
    for (const row of rows) {
      const aligned = align(row[field], reference);
      if (!aligned) return null;
      sum = sum.map((value, i) => add(value, aligned[i]));
    }
    const mean = unit(sum); if (!mean) return null; means.push(mean);
  }
  const mean = unit(means.reduce((sum, row) => sum.map((value, i) => add(value, row[i])), reference.map(() => [0, 0])));
  return mean && {mean: canonicalPhase(mean), coherence: means.reduce((sum, row) => sum + projectionPower(mean, row), 0) / means.length};
}
function identity(row) {
  return canonicalHash({epoch_id: row.epoch_id, configuration: row.configuration,
    source_epoch: row.sample_clock.source_epoch,
    native: row.source_snapshots.map(snapshot => [snapshot.rf_sha256, snapshot.native_module_sha256,
      snapshot.native_device_sha256])});
}
function hardwareEpoch(row) {
  return canonicalHash({collection_epoch: row.collection_epoch || row.epoch_id,
    source_epoch: row.sample_clock.source_epoch, frequency_hz: row.configuration.frequency_hz,
    sample_rate_hz: row.configuration.sample_rate_hz, channels: row.configuration.channels,
    channel_mapping: row.configuration.channel_mapping, gains: row.configuration.actual_channel_gains_db,
    native: row.source_snapshots.map(snapshot => [snapshot.rf_sha256, snapshot.native_module_sha256,
      snapshot.native_device_sha256])});
}
function nativeRows(rows, {resolveKey} = {}) {
  if (!Array.isArray(rows) || rows.length > 20000) throw new Error('Native corpus exceeds its row bound');
  let previous = null;
  const seen = new Set();
  return rows.map(row => {
    validateNativeShadowManifoldRow(row, {resolveKey});
    if (row.previous_row_sha256 !== previous) throw new Error('Native shadow ledger chain is discontinuous');
    previous = row.row_sha256;
    if (seen.has(row.native_export_sha256)) throw new Error('Duplicate native export in shadow ledger');
    seen.add(row.native_export_sha256);
    const response = unit(row.physical_iq_cut);
    if (!response) throw new Error('Native physical CUT has zero or nonfinite norm');
    const channels = row.configuration.channels, beams = row.actual_fusion_weights.length / channels;
    // Native bank entries are combining coefficients (already conjugated).
    // Pick only by signed geometry, never the maximum observed beam power.
    const beam = Math.round(azimuth(row.truth.local_azimuth_deg) / 360 * beams) % beams;
    const baseline = unit(conjugate(row.actual_fusion_weights.slice(beam * channels, (beam + 1) * channels)));
    if (!baseline) throw new Error('Native truth-selected fusion weights have zero or nonfinite norm');
    return {...row, response, baseline, controls: row.local_control_cells || [],
      _identity: identity(row), _hardware_epoch: hardwareEpoch(row), _holdout: split(row.pass.id)};
  });
}
function bin(row, azimuthBins, elevationBins) {
  return `${Math.floor(azimuth(row.truth.local_azimuth_deg) / 360 * azimuthBins)}:${Math.min(elevationBins - 1,
    Math.floor((row.truth.elevation_deg + 90) / 180 * elevationBins))}`;
}

function fitNativeShadow(rows, {resolveKey, cpiSeconds, azimuthBins = 12, elevationBins = 4, minTrainingPassesPerBin = 2} = {}) {
  if (!Number.isInteger(azimuthBins) || azimuthBins < 4 || azimuthBins > 360 ||
      !Number.isInteger(elevationBins) || elevationBins < 2 || elevationBins > 180 || azimuthBins * elevationBins > 4096 ||
      !Number.isInteger(minTrainingPassesPerBin) || minTrainingPassesPerBin < 2 || minTrainingPassesPerBin > 20000)
    throw new Error('Native fitter requires a bounded two-dimensional grid and independent pass support');
  const all = nativeRows(rows, {resolveKey}).sort((a, b) => a.sample_clock.timestamp_ms - b.sample_clock.timestamp_ms || a.row_sha256.localeCompare(b.row_sha256));
  if (!all.length) return null;
  // A/B/A is a new epoch, not permission to resurrect old A samples. Clock
  // source, receiver gain, RF, module and exact configuration all participate.
  let start = all.length - 1;
  while (start > 0 && all[start - 1]._hardware_epoch === all[all.length - 1]._hardware_epoch) --start;
  const epoch = all.slice(start);
  // Intentional dwell variants coexist within one hardware epoch. Select a
  // lane by configuration alone, never by support, SNR, or holdout outcome.
  const preferred = epoch.filter(row => row.configuration.cpi_seconds === cpiSeconds);
  const lane = [...new Set((preferred.length ? preferred : epoch).map(row => row._identity))].sort()[0];
  const current = epoch.filter(row => row._identity === lane);
  const train = current.filter(row => !row._holdout), holdout = current.filter(row => row._holdout);
  const groups = new Map();
  for (const row of train) {
    const key = bin(row, azimuthBins, elevationBins);
    if (!groups.has(key)) groups.set(key, new Map());
    const passes = groups.get(key);
    if (!passes.has(row.pass.id)) passes.set(row.pass.id, []);
    passes.get(row.pass.id).push(row);
  }
  const cells = [];
  for (let az = 0; az < azimuthBins; ++az) for (let el = 0; el < elevationBins; ++el) {
    const key = `${az}:${el}`, passes = [...(groups.get(key)?.values() || [])], values = passes.flat();
    const cell = {key, azimuth_bin: az, elevation_bin: el, independent_passes: passes.length,
      training_samples: values.length, support: passes.length ? 'sparse' : 'unsupported', response: null,
      elevation_range_deg: values.length ? [Math.min(...values.map(row => row.truth.elevation_deg)), Math.max(...values.map(row => row.truth.elevation_deg))] : null,
      confidence: 0, no_blind_extrapolation: true};
    if (passes.length >= minTrainingPassesPerBin) {
      const measured = balancedMean(passes, 'response'), prior = balancedMean(passes, 'baseline');
      const aligned = measured && prior && align(measured.mean, prior.mean);
      const weight = Math.min(.8, passes.length / (passes.length + 4));
      if (aligned) {
        cell.response = canonicalPhase(unit(aligned.map(([r, i], c) => [weight * r + (1 - weight) * prior.mean[c][0],
          weight * i + (1 - weight) * prior.mean[c][1]])));
        cell.support = 'supported'; cell.confidence = measured.coherence * passes.length / (passes.length + 4);
        cell.shrinkage_to_native_prior = 1 - weight;
        cell.projective_pass_coherence = measured.coherence;
      } else cell.support = 'incoherent';
    }
    cells.push(cell);
  }
  const diagnostics = holdout.map(row => {
    const cell = cells.find(value => value.key === bin(row, azimuthBins, elevationBins));
    const supported = cell.response && row.truth.elevation_deg >= cell.elevation_range_deg[0] && row.truth.elevation_deg <= cell.elevation_range_deg[1];
    const common = {pass_id: row.pass.id, epoch_id: row.epoch_id, native_export_sha256: row.native_export_sha256,
      detector_outcome: row.pass.label, supported: Boolean(supported), confidence: supported ? cell.confidence : 0};
    if (!supported) return {...common, target_projection_change: null, local_control_projection_change: null, local_control_count: 0};
    // Unit-normalized projection diagnostic only: not detection probability,
    // native CFAR, raw power improvement, or a certified target-free nuisance test.
    const differences = row.controls.map(control => unit(control.physical_channels)).filter(Boolean)
      .map(vector => projectionPower(cell.response, vector) - projectionPower(row.baseline, vector));
    return {...common, target_projection_change: projectionPower(cell.response, row.response) - projectionPower(row.baseline, row.response),
      local_control_projection_change: differences.length ? differences.reduce((a, b) => a + b, 0) / differences.length : null,
      local_control_count: differences.length};
  });
  const evidenceKey = canonicalHash({identity: current[0]._identity, epoch_started_at_ms: current[0].sample_clock.timestamp_ms});
  const model = {schema: 'vectorwarp-native-shadow-candidate-v1', representation: 'complex_caf_target_cut_physical_channels_v1',
    runtime_compatible: false, promotion_allowed: false, hardware_qualified: false,
    intended_use: 'shadow-projection-only', evidence_key: evidenceKey, epoch_id: current[0].epoch_id,
    configuration: current[0].configuration, azimuth_bins: azimuthBins, elevation_bins: elevationBins,
    elevation_range_deg: [-90, 90], min_training_passes_per_bin: minTrainingPassesPerBin,
    baseline: 'unit-normalized-native-truth-nearest-beam',
    prior: 'pass-balanced-native-truth-nearest-beam-response-not-ideal',
    method: 'pass-balanced-phase-aligned-2d-shrunk-to-recorded-native-prior-v1',
    training_evidence_sha256: canonicalHash(train.map(row => row.native_export_sha256).sort()),
    train_passes: [...new Set(train.map(row => row.pass.id))].sort(), cells};
  model.candidate_version = canonicalHash(model);
  const evaluation = {schema: 'vectorwarp-native-shadow-projection-ab-v1', candidate_version: model.candidate_version,
    qualified: false, promotion_allowed: false, hardware_qualified: false, decision_grade: false,
    holdout_evidence_sha256: canonicalHash(holdout.map(row => row.native_export_sha256).sort()),
    holdout_passes: [...new Set(holdout.map(row => row.pass.id))].sort(), diagnostics,
    local_control_scope: 'nearby physical CUTs; not certified target-free CFAR training',
    limitation: 'Pass-disjoint projection diagnostic only; no production CFAR/detection/qualification claim.'};
  return {model, evaluation, accepted_samples: current.length, excluded_prior_epoch_samples: start,
    excluded_other_configuration_samples: epoch.length - current.length,
    supported_cells: cells.filter(cell => cell.support === 'supported').length};
}
module.exports = {nativeRows, fitNativeShadow, projectionPower, split};
