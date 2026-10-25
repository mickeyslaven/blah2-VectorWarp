#!/usr/bin/env node
'use strict';

// Prepare, but never install, a corrected Arrow/Kraken hole-9 configuration.
// The input and output must be distinct so this cannot mutate a locked node.
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const yaml = require('../api/node_modules/js-yaml');

const OLD_RADIUS_M = 0.193548;
const CORRECT_RADIUS_M = 0.0762;
const TOLERANCE_M = 1e-6;

function digest(value) {
  return crypto.createHash('sha256').update(value).digest('hex');
}

function radius(position) {
  return Math.hypot(position[0], position[1]);
}

function finitePosition(position) {
  return Array.isArray(position) && position.length === 3 &&
    position.every(Number.isFinite);
}

function points(radiusM) {
  return Array.from({length: 5}, (_, channel) => {
    const angle = -2 * Math.PI * channel / 5;
    return [radiusM * Math.cos(angle), radiusM * Math.sin(angle), 0];
  });
}

function corrected(source) {
  const value = yaml.load(source);
  const geometry = value?.capture?.device?.array_geometry;
  if (!geometry || geometry.shape !== 'regular_polygon' || geometry.units !== 'm' ||
      geometry.entry_template !== 'pentagon' || !Array.isArray(geometry.elements) ||
      geometry.elements.length !== 5)
    throw new Error('refusing: expected a five-element metre-based pentagon');
  const ordered = [...geometry.elements].sort((a, b) => a.daq_channel - b.daq_channel);
  if (new Set(geometry.elements.map(element => element?.id)).size !== 5 ||
      new Set(geometry.elements.map(element => element?.daq_channel)).size !== 5 ||
      ordered.some((element, index) => element.daq_channel !== index ||
      typeof element.id !== 'string' || !element.id || !finitePosition(element.position) ||
      Math.abs(radius(element.position) - OLD_RADIUS_M) > TOLERANCE_M ||
      Math.abs(element.position[2]) > TOLERANCE_M))
    throw new Error('refusing: source does not match the exact invalidated 0.193548 m geometry');
  const positions = ordered.map(element => {
    const sourceRadius = radius(element.position);
    return [element.position[0] * CORRECT_RADIUS_M / sourceRadius,
      element.position[1] * CORRECT_RADIUS_M / sourceRadius, 0];
  });
  ordered.forEach((element, channel) => { element.position = positions[channel]; });
  const note = String(geometry.operator_notes || '');
  if (!/7\.62 inches\s*\(0\.193548 m\)/i.test(note))
    throw new Error('refusing: source does not contain the audited unit-error note');
  geometry.operator_notes = note.replace(
    /7\.62 inches\s*\(0\.193548 m\)/i,
    '7.62 cm (0.0762 m), per Arrow hole-9 table');
  return {value, positions};
}

function main(argv) {
  if (argv.length !== 2)
    throw new Error('usage: prepare-corrected-arrow-geometry.js INPUT.yml OUTPUT.yml');
  const [input, output] = argv.map(value => path.resolve(value));
  if (input === output) throw new Error('refusing in-place configuration mutation');
  const source = fs.readFileSync(input, 'utf8');
  const result = corrected(source);
  const body = yaml.dump(result.value, {lineWidth: 100, noRefs: true});
  fs.writeFileSync(output, body, {flag: 'wx', mode: 0o600});
  const receipt = {
    schema: 'vectorwarp.prepared-array-geometry-correction.v1',
    installed: false,
    live_radar_modified: false,
    source_path: input,
    output_path: output,
    source_sha256: digest(source),
    output_sha256: digest(body),
    old_radius_m: OLD_RADIUS_M,
    corrected_radius_m: CORRECT_RADIUS_M,
    positions_m: result.positions,
    required_followup: 'managed apply/readback, new config/manifold identity, new evidence epoch',
  };
  fs.writeFileSync(`${output}.receipt.json`, `${JSON.stringify(receipt, null, 2)}\n`,
    {flag: 'wx', mode: 0o600});
  process.stdout.write(`${JSON.stringify(receipt, null, 2)}\n`);
}

if (require.main === module) {
  try { main(process.argv.slice(2)); }
  catch (error) { console.error(error.message); process.exitCode = 1; }
}

module.exports = {corrected, points, OLD_RADIUS_M, CORRECT_RADIUS_M};
