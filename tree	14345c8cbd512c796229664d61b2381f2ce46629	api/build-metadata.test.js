'use strict';
const assert = require('node:assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {buildMetadata, readReceipt} = require('./build-metadata');

const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-build-receipt-'));
try {
  const apiDirectory = path.join(directory, 'api');
  fs.mkdirSync(apiDirectory);
  const receipt = path.join(directory, 'BUILD-RECEIPT.json');
  fs.writeFileSync(receipt, JSON.stringify({schema: 1, version: '1.2.3',
    sourceCommit: 'a'.repeat(40), package: 'deb'}));
  assert.deepEqual(readReceipt(receipt), {version: '1.2.3', commit: 'a'.repeat(40), package: 'deb'});
  assert.deepEqual(buildMetadata({}, apiDirectory),
    {version: '1.2.3', commit: 'a'.repeat(40), package: 'deb'});
  fs.writeFileSync(receipt, JSON.stringify({schema: 1, version: '@owner',
    sourceCommit: 'short', package: 'deb'}));
  assert.equal(readReceipt(receipt), null);
  assert.deepEqual(buildMetadata({BLAH2_PREVIEW: 'true'}, apiDirectory),
    {version: 'development', commit: '0000000', package: 'preview'});
  console.log('build metadata receipt validation passed');
} finally { fs.rmSync(directory, {recursive: true, force: true}); }
