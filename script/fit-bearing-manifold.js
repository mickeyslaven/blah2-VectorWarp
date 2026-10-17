#!/usr/bin/env node
'use strict';

const fs = require('fs');
const path = require('path');
const {fitManifold} = require('../api/manifold-fitter');

function usage() {
  return 'Usage: fit-bearing-manifold --input samples.jsonl [--input rotated.jsonl.1] --output manifold.json [--beams 60] [--min-samples 20] [--min-coverage 0.5] [--max-gap-deg 90]';
}

function argumentsFrom(argv) {
  const options = {input: [], beams: 60, minSamples: 20, minCoverage: .5, maxGapDeg: 90};
  for (let index = 0; index < argv.length; ++index) {
    const name = argv[index], value = argv[++index];
    if (value === undefined) throw new Error(usage());
    if (name === '--input') options.input.push(value);
    else if (name === '--output') options.output = value;
    else if (name === '--beams') options.beams = Number(value);
    else if (name === '--min-samples') options.minSamples = Number(value);
    else if (name === '--min-coverage') options.minCoverage = Number(value);
    else if (name === '--max-gap-deg') options.maxGapDeg = Number(value);
    else throw new Error(`Unknown option ${name}\n${usage()}`);
  }
  if (!options.input.length || !options.output || !path.isAbsolute(options.output))
    throw new Error(`${usage()}\nThe output path must be absolute.`);
  return options;
}

async function main() {
  const options = argumentsFrom(process.argv.slice(2));
  const samples = [];
  for (const filename of options.input) {
    const lines = (await fs.promises.readFile(filename, 'utf8')).split(/\r?\n/).filter(Boolean);
    lines.forEach((line, index) => {
      try { samples.push(JSON.parse(line)); }
      catch (error) { throw new Error(`${filename}:${index + 1}: ${error.message}`); }
    });
  }
  const result = fitManifold(samples, options);
  await fs.promises.mkdir(path.dirname(options.output), {recursive: true});
  const temporary = `${options.output}.tmp-${process.pid}`;
  await fs.promises.writeFile(temporary, JSON.stringify(result, null, 2) + '\n', {mode: 0o600});
  await fs.promises.rename(temporary, options.output);
  process.stdout.write(`Wrote ${options.output}: ${result.calibration.input_samples} samples, ` +
    `${result.calibration.observed_bins}/${result.calibration.beams} observed bins, ` +
    `${result.calibration.interpolated_bins} interpolated.\n`);
}

main().catch(error => { process.stderr.write(`fit-bearing-manifold: ${error.message}\n`); process.exitCode = 1; });
