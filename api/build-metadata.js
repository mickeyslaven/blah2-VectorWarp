'use strict';

const fs = require('fs');
const path = require('path');
const {RECEIVER_TYPES} = require('./receiver-manager');

const commitPattern = /^[a-f0-9]{40}$/i;
const identifierPattern = /^[A-Za-z0-9][A-Za-z0-9_.+-]{0,63}$/;

function valid(value) {
  return value && value.schema === 1 && identifierPattern.test(value.version || '') &&
    commitPattern.test(value.sourceCommit || '') && identifierPattern.test(value.package || '');
}

function readReceipt(filename) {
  try {
    const descriptor = fs.openSync(filename, fs.constants.O_RDONLY | (fs.constants.O_NOFOLLOW || 0));
    try {
      const stat = fs.fstatSync(descriptor);
      if (!stat.isFile() || stat.size < 2 || stat.size > 4096) return null;
      const value = JSON.parse(fs.readFileSync(descriptor, 'utf8'));
      return valid(value) ? {version: value.version, commit: value.sourceCommit.toLowerCase(),
        package: value.package} : null;
    } finally { fs.closeSync(descriptor); }
  } catch (_) { return null; }
}

function buildMetadata(env = process.env, apiDirectory = __dirname) {
  const receipt = readReceipt(path.resolve(apiDirectory, '../BUILD-RECEIPT.json'));
  if (receipt) return receipt;
  if (identifierPattern.test(env.VECTORWARP_VERSION || '') &&
      commitPattern.test(env.VECTORWARP_SOURCE_COMMIT || '') &&
      identifierPattern.test(env.VECTORWARP_PACKAGE || ''))
    return {version: env.VECTORWARP_VERSION, commit: env.VECTORWARP_SOURCE_COMMIT.toLowerCase(),
      package: env.VECTORWARP_PACKAGE};
  return {version: 'development', commit: '0000000',
    package: env.BLAH2_PREVIEW === 'true' ? 'preview' : 'source'};
}

function compiledReceiverTypes(env = process.env, apiDirectory = __dirname) {
  const filename = path.resolve(apiDirectory, '../.vectorwarp-build');
  let descriptor;
  try {
    descriptor = fs.openSync(filename, fs.constants.O_RDONLY |
      (fs.constants.O_NOFOLLOW || 0) | (fs.constants.O_NONBLOCK || 0));
    const stat = fs.fstatSync(descriptor);
    if (!stat.isFile() || stat.size < 1 || stat.size > 65536) return [];
    const fields = new Map();
    for (const line of fs.readFileSync(descriptor, 'utf8').split('\n').filter(Boolean)) {
      const match = /^([a-z][a-z0-9_]*)=([^\r\n]*)$/.exec(line);
      if (!match || fields.has(match[1])) return [];
      fields.set(match[1], match[2]);
    }
    const types = fields.get('compiled_receivers')?.split(',') || [];
    if (!identifierPattern.test(fields.get('build_id') || '') ||
        !commitPattern.test(fields.get('source_commit') || '') || !types.length ||
        new Set(types).size !== types.length ||
        types.some(type => !RECEIVER_TYPES.includes(type))) return [];
    return types;
  } catch (error) {
    // Installed artifact declarations outrank stale service environments.
    // Only absent manifests use the source/macOS lifecycle environment.
    return error.code === 'ENOENT' ?
      env.BLAH2_RECEIVER_TYPES?.split(',').map(item => item.trim()).filter(Boolean) || null : [];
  } finally { if (descriptor !== undefined) fs.closeSync(descriptor); }
}

module.exports = {buildMetadata, readReceipt, compiledReceiverTypes};
