#!/usr/bin/env node
'use strict';

const fs = require('fs');
const yaml = require('../api/node_modules/js-yaml');
const {fitManifold, labelPasses} = require('../api/manifold-fitter');
const {bestBearing, bestMeasuredBearing, circularError} = require('../api/bearing-diagnostic');

const finite = value => typeof value === 'number' && Number.isFinite(value);
const median = values => {
  const sorted = [...values].sort((a, b) => a - b), middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
};
const angleDistance = (left, right) => Math.abs((((left - right) % 360) + 540) % 360 - 180);
const option = name => {
  const at = process.argv.indexOf(name);
  if (at < 0 || !process.argv[at + 1]) throw new Error(`Missing ${name}`);
  return process.argv[at + 1];
};
const summarize = rows => ({
  passes: rows.length,
  median_error_deg: rows.length ? median(rows.map(row => row.error_deg)) : null,
  within_15_fraction: rows.length ? rows.filter(row => row.error_deg <= 15).length / rows.length : null,
  within_30_fraction: rows.length ? rows.filter(row => row.error_deg <= 30).length / rows.length : null,
  within_45_fraction: rows.length ? rows.filter(row => row.error_deg <= 45).length / rows.length : null
});

function main() {
  const samples = fs.readFileSync(option('--input'), 'utf8').trim().split(/\r?\n/).filter(Boolean).map(JSON.parse);
  const config = yaml.load(fs.readFileSync(option('--config'), 'utf8'), {schema: yaml.JSON_SCHEMA});
  const output = option('--output'), beams = Number(option('--beams'));
  const labelled = labelPasses(samples), passIds = [...new Set(labelled.map(row => row._pass_id))];
  const mapping = config.process?.bearing?.channel_mapping ||
    Array.from({length: samples[0]?.channels || 0}, (_, index) => index);
  const folds = [];
  for (const passId of passIds) {
    const training = labelled.filter(row => row._pass_id !== passId);
    const holdout = labelled.filter(row => row._pass_id === passId);
    try {
      const manifold = fitManifold(training, {beams, minSamples: 2, minPasses: 1,
        minCoverage: .2, maxGapDeg: 240,
        maxElevationDeg: config.process?.bearing?.manifold_recorder?.max_elevation_deg ?? 15,
        maxDelayErrorKm: config.process?.bearing?.manifold_recorder?.max_delay_error_km ?? .25,
        maxDopplerErrorHz: config.process?.bearing?.manifold_recorder?.max_doppler_error_hz ?? 5,
        maxTruthAgeS: config.process?.bearing?.manifold_recorder?.max_truth_age_s ?? 3,
        holdoutPredicate: () => false});
      const truthAzimuth = median(holdout.map(row => row.truth.local_azimuth_deg));
      const supportDistance = Math.min(...training.map(row => angleDistance(truthAzimuth,
        row.truth.local_azimuth_deg)));
      const idealErrors = [], measuredErrors = [];
      for (const row of holdout) {
        if (!finite(row.truth?.bearing_deg_true)) continue;
        idealErrors.push(circularError(bestBearing(row, config, {mapping}), row.truth.bearing_deg_true));
        measuredErrors.push(circularError(bestMeasuredBearing(row, config, manifold), row.truth.bearing_deg_true));
      }
      folds.push({pass_id: passId, icao: holdout[0]?.icao || null, samples: holdout.length,
        local_azimuth_deg: truthAzimuth, support_distance_deg: supportDistance,
        ideal_error_deg: median(idealErrors), measured_error_deg: median(measuredErrors),
        observed_bins: manifold.calibration.observed_bins,
        largest_gap_deg: manifold.calibration.largest_observed_gap_deg});
    } catch (error) { folds.push({pass_id: passId, error: error.message}); }
  }
  const valid = folds.filter(row => finite(row.ideal_error_deg) && finite(row.measured_error_deg));
  const support = {};
  for (const maximum of [6, 15, 30, 45, 90, 180]) {
    const rows = valid.filter(row => row.support_distance_deg <= maximum);
    support[maximum] = {ideal: summarize(rows.map(row => ({error_deg: row.ideal_error_deg}))),
      measured: summarize(rows.map(row => ({error_deg: row.measured_error_deg})))};
  }
  const result = {schema: 'vectorwarp-manifold-lopo-evaluation-v1',
    generated_at: new Date().toISOString(), beams, samples: samples.length,
    passes: passIds.length, successful_folds: valid.length, failed_folds: folds.length - valid.length,
    ideal: summarize(valid.map(row => ({error_deg: row.ideal_error_deg}))),
    measured: summarize(valid.map(row => ({error_deg: row.measured_error_deg}))),
    by_support_distance_deg: support, folds};
  fs.writeFileSync(output, JSON.stringify(result, null, 2) + '\n');
}

try { main(); }
catch (error) { process.stderr.write(`evaluate-manifold-lopo: ${error.message}\n`); process.exitCode = 1; }
