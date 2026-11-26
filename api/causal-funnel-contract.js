'use strict';

// Native health/evidence rejection labels.
const birthRejectionLabels = Object.freeze({
  birth_snr_rejected: 'SNR/support', birth_ridge_rejected: 'ridge',
  birth_doppler_comb_rejected: 'Doppler comb', birth_range_comb_rejected: 'range comb',
  birth_lineage_rejected: 'lineage', birth_capacity_rejected: 'capacity',
  stationary_birth_rejected: 'stationary',
  stationary_birth_capacity_rejected: 'stationary capacity',
  birth_exclusive_rejected: 'exclusive neighborhood'
});
const trackerDiagnosticKeys = Object.freeze(`no_op detections tracks_before tracks_after
candidate_track_pairs pair_already_claimed pair_delay_rejected pair_doppler_rejected
pair_innovation_rejected pair_spatial_rejected pair_in_gate pair_lineage_rejected
associations misses promotions tentative_expired inactivity_deleted birth_candidates initiated
lineage_resets independent_credits credited_source_cpis stationary_quarantined stationary_cells
stationary_cell_capacity_rejected`.split(/\s+/).concat(Object.keys(birthRejectionLabels)));

function exactUint64(value) {
  return typeof value === 'string' && /^(0|[1-9][0-9]{0,19})$/.test(value) &&
    (value.length < 20 || value <= '18446744073709551615');
}
function encodeUint64(value) {
  if (typeof value === 'bigint') value = value.toString();
  else if (Number.isSafeInteger(value) && value >= 0) value = String(value);
  if (!exactUint64(value)) throw new Error('Exact unsigned decimal uint64 required');
  return value;
}
module.exports = {exactUint64, encodeUint64, birthRejectionLabels, trackerDiagnosticKeys};
