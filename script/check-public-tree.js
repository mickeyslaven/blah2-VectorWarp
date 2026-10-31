#!/usr/bin/env node
'use strict';

const {execFileSync} = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(process.argv[2] || path.join(__dirname, '..'));
const tracked = execFileSync('git', ['ls-files', '-z'], {cwd: root})
  .toString().split('\0').filter(Boolean);
const privatePrefixes = ['results/', 'docs/research/', 'docs/validation/'];
const forbidden = [
  ['private home path', new RegExp('/Users/' + 'michaelslaven/', 'i')],
  ['private live host', new RegExp('\\b(?:fedora' + '3|100\\.126\\.186\\.65|192\\.168\\.1\\.209)\\b', 'i')],
  ['private site label', new RegExp('\\b(?:Ford ' + 'Street|WP' + 'XI|WP' + 'GH|WT' + 'AE)\\b', 'i')],
  ['private transport', new RegExp('\\bTail' + 'drop\\b|integration[-_]' + 'shadow', 'i')],
  ['private site coordinates', new RegExp('40\\.442' + '0304|-80\\.056' + '6972', 'i')]
];
const failures = [];

for (const relative of tracked) {
  if (privatePrefixes.some(prefix => relative.startsWith(prefix))) {
    failures.push(`${relative}: private evidence directory is tracked`);
    continue;
  }
  const filename = path.join(root, relative);
  let bytes;
  try { bytes = fs.readFileSync(filename); }
  catch { continue; }
  if (bytes.includes(0)) continue;
  const text = bytes.toString('utf8');
  for (const [label, pattern] of forbidden) {
    if (pattern.test(text)) failures.push(`${relative}: contains ${label}`);
  }
}

if (failures.length) {
  console.error(`Public-tree boundary failed:\n${failures.join('\n')}`);
  process.exit(1);
}
console.log(`Public-tree boundary passed for ${tracked.length} tracked paths.`);
