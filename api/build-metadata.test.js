'use strict';
const assert = require('node:assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {buildMetadata, readReceipt, compiledReceiverTypes} = require('./build-metadata');

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
  const environment = {BLAH2_RECEIVER_TYPES: 'Usrp,Kraken'};
  assert.deepEqual(compiledReceiverTypes(environment, apiDirectory), ['Usrp','Kraken'],
    'Source/macOS lifecycle environments still declare capabilities without a native manifest');
  assert.equal(compiledReceiverTypes({}, apiDirectory), null);
  const manifest = path.join(directory, '.vectorwarp-build');
  const fields = `build_id=candidate-123\nsource_commit=${'a'.repeat(40)}\n`;
  fs.writeFileSync(manifest, fields + 'compiled_receivers=Kraken,Usrp,HackRF\n');
  assert.deepEqual(compiledReceiverTypes(environment, apiDirectory), ['Kraken','Usrp','HackRF'],
    'Installed artifact capabilities override a stale service whitelist');
  for (const invalid of [fields + 'compiled_receivers=Kraken,Unknown\n',
    fields + 'compiled_receivers=Kraken,Kraken\n', fields + 'compiled_receivers=Kraken\ncompiled_receivers=Usrp\n',
    'compiled_receivers=Kraken\n', 'x'.repeat(65537)]) {
    fs.writeFileSync(manifest, invalid);
    assert.deepEqual(compiledReceiverTypes(environment, apiDirectory), [],
      'Malformed installed manifests cannot silently regain environment capabilities');
  }
  fs.unlinkSync(manifest); fs.symlinkSync(receipt, manifest);
  assert.deepEqual(compiledReceiverTypes(environment, apiDirectory), [],
    'A symlink cannot substitute an installed artifact declaration');
  console.log('build metadata receipt validation passed');
} finally { fs.rmSync(directory, {recursive: true, force: true}); }
