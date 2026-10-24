'use strict';

const assert = require('assert');
const {analyze} = require('../../script/analyze-tracker-publication-ablation');

const detections = [
  {timestamp: 1000, delay: [1], doppler: [10], snr: [8]},
  {timestamp: 1500, delay: [1], doppler: [10], snr: [8]}
];
const tracks = [
  {timestamp: 1000, n: 2, nTentative: 1, nAssociated: 1, nActive: 0, nCoasting: 0,
    data: [{id: 'a', state: 'ASSOCIATED', delay: 8, doppler: 10, n: 2}]},
  {timestamp: 1500, n: 2, nTentative: 0, nAssociated: 1, nActive: 1, nCoasting: 0,
    data: [{id: 'a', state: 'ACTIVE', delay: 8, doppler: 10, n: 5},
      {id: 'false', state: 'ASSOCIATED', delay: 80, doppler: 100, n: 2}]}
];
const truth = [{observed_at: 1.5, truth: {target: {timestamp: 1.5, delay: 1, doppler: 10}}}];
const result = analyze({arms: {coherent_beam_bank: {detections, tracks}}}, truth,
  {signal_start_epoch: .5, cpi_seconds: .5});
assert.equal(result.raw_detector.detections, 2);
assert.equal(result.stationary_clutter.admission_suppression_reachable, false);
assert.equal(result.stationary_clutter.on_shadow_native.expected_equal_to_off, true);
assert.equal(result.publication.native.observable_formed_track_ids, 2);
assert.equal(result.publication.hybrid_no_external_events.observable_formed_track_ids, 1);
assert.equal(result.publication.native.target_near_track_ids, 1);
assert.equal(result.publication.hybrid_no_external_events.target_near_track_ids, 1);
assert.equal(result.publication.native.ids_never_near_eligible_adsb, 1);
assert.equal(result.publication.hybrid_no_external_events.ids_never_near_eligible_adsb, 0);
console.log('TRACKER_PUBLICATION_ABLATION_PASS');
