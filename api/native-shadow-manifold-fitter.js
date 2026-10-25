'use strict';

const crypto = require('crypto');
const {canonicalHash} = require('./native-shadow-manifold-evidence');
const finite = value => typeof value === 'number' && Number.isFinite(value);
const unit = vector => { const n = Math.hypot(...vector.flat()); return Number.isFinite(n) && n > 1e-12 ? vector.map(([r, i]) => [r / n, i / n]) : null; };
const dotPower = (left, right) => left.reduce((sum, [r, i], n) =>
  sum + (r * right[n][0] + i * right[n][1]) ** 2 + (r * right[n][1] - i * right[n][0]) ** 2, 0);
const split = pass => parseInt(crypto.createHash('sha256').update(pass).digest('hex').slice(0, 8), 16) % 5 === 0;

function nativeRows(rows) {
  return rows.filter(row => row?.schema === 'vectorwarp-native-shadow-manifold-row-v1').map(row => {
    const controls = row.local_control_cells || [];
    if (!Array.isArray(controls) || controls.length > 24 ||
        new Set(controls.map(cell => cell?.id)).size !== controls.length ||
        controls.some(cell => !cell?.id || cell.id === row.target_cut_id || !Array.isArray(cell.physical_channels) ||
          cell.physical_channels.length !== row.physical_iq_cut.length)) throw new Error('Invalid native local control evidence');
    const response = unit(row.physical_iq_cut); if (!response) throw new Error('Native physical CUT has zero or nonfinite norm');
    return {...row, response, _pass_id: row.pass.id,
      _epoch_pass_id: `${row.epoch_id}:${row.pass.id}`, _holdout: split(row.pass.id), controls};
  });
}

function fitNativeShadow(rows, {azimuthBins = 12} = {}) {
  const all = nativeRows(rows), train = all.filter(row => !row._holdout), holdout = all.filter(row => row._holdout);
  const bins = Array.from({length: azimuthBins}, () => []);
  for (const row of train) bins[Math.floor((((row.truth.local_azimuth_deg % 360) + 360) % 360) / 360 * azimuthBins)].push(row);
  const steering = bins.map(values => values.length ? unit(values[0].response.map((_, c) => values.reduce((sum, row) =>
    [sum[0] + row.response[c][0], sum[1] + row.response[c][1]], [0, 0]))) : null);
  const diagnostics = holdout.map(row => {
    const index = Math.floor((((row.truth.local_azimuth_deg % 360) + 360) % 360) / 360 * azimuthBins);
    const measured = steering[index];
    if (!measured) return {pass_id: row._pass_id, epoch_id: row.epoch_id, supported: false,
      target_projection_gain: null, nuisance_change: null, confidence: 0};
    const ideal = unit(row.reference_weights), targetProjection = dotPower(measured, row.response) - dotPower(ideal, row.response);
    const nuisance = row.controls.map(cell => { const vector = unit(cell.physical_channels); return dotPower(measured, vector) - dotPower(ideal, vector); });
    return {pass_id: row._pass_id, epoch_id: row.epoch_id, supported: true,
      target_projection_gain: targetProjection, nuisance_change: nuisance.length ? nuisance.reduce((a, b) => a + b, 0) / nuisance.length : null,
      confidence: bins[index].length / (bins[index].length + 1)};
  });
  return {schema: 'vectorwarp-native-shadow-fit-v1', representation: 'complex_caf_target_cut_physical_channels_v1',
    evidence_sha256: canonicalHash(all.map(row => row.row_sha256)), train_passes: [...new Set(train.map(r => r._pass_id))],
    holdout_passes: [...new Set(holdout.map(r => r._pass_id))], steering, diagnostics,
    promotion_allowed: false, production_qualification: 'disabled-shadow-projection-only'};
}
module.exports = {nativeRows, fitNativeShadow};
