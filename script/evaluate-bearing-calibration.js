#!/usr/bin/env node
'use strict';

const fs = require('fs');
const yaml = require('../api/node_modules/js-yaml');
const {evaluateBearing, readSamples} = require('../api/bearing-diagnostic');

function option(name) {
  const at = process.argv.indexOf(name);
  if (at < 0 || !process.argv[at + 1]) throw new Error(`Missing ${name}`);
  return process.argv[at + 1];
}

try {
  const config = yaml.load(fs.readFileSync(option('--config'), 'utf8'), {schema: yaml.JSON_SCHEMA});
  const result = evaluateBearing(readSamples(option('--input')), config);
  process.stdout.write(`${JSON.stringify(result, null, 2)}\n`);
  if (!result.available) process.exitCode = 2;
} catch (error) {
  process.stderr.write(`evaluate-bearing-calibration: ${error.message}\n`);
  process.exitCode = 1;
}
