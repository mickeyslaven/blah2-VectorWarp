#!/usr/bin/env node
'use strict';

const fs = require('fs');
const path = require('path');
const yaml = require('../api/node_modules/js-yaml');
const {evaluateReferenceSurvey, readReferenceUpdates} = require('../api/bearing-diagnostic');

function option(name) {
  const at = process.argv.indexOf(name);
  if (at < 0 || !process.argv[at + 1]) throw new Error(`Missing ${name}`);
  return process.argv[at + 1];
}

try {
  const config = yaml.load(fs.readFileSync(option('--config'), 'utf8'), {schema: yaml.JSON_SCHEMA});
  const manifestFilename = path.resolve(option('--manifest'));
  const manifest = JSON.parse(fs.readFileSync(manifestFilename, 'utf8'));
  const base = path.dirname(manifestFilename);
  const sources = (manifest.sources || []).map(source => ({...source,
    reference_updates: readReferenceUpdates(path.resolve(base, source.reference), {maximumRows: 4096})}));
  const result = evaluateReferenceSurvey(sources, config);
  process.stdout.write(`${JSON.stringify(result, null, 2)}\n`);
  if (!result.available) process.exitCode = 2;
} catch (error) {
  process.stderr.write(`evaluate-reference-survey: ${error.message}\n`);
  process.exitCode = 1;
}
