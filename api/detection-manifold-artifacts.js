'use strict';
const fs = require('fs');
const crypto = require('crypto');
const path = require('path');

function resolved(filename) {
  const absolute = path.resolve(filename);
  try { return fs.realpathSync(absolute); }
  catch (error) {
    if (error.code !== 'ENOENT') throw error;
    const parent = path.dirname(absolute);
    return parent === absolute ? absolute : path.join(resolved(parent), path.basename(absolute));
  }
}
function validateArtifactDestinations(config, {samplePath, validationPath, candidatePath,
  receiptPath, nativeShadowPath, versionedCandidatePath} = {}) {
  const protectedPaths = [samplePath, validationPath,
    config.process?.bearing?.manifold_file, config.process?.surveillance_fusion?.manifold_file,
    config.process?.bearing?.manifold_recorder?.path,
    config.process?.bearing?.manifold_recorder?.reference_path,
    config.process?.detection?.integration_shadow?.receipt_key_path].filter(Boolean);
  const outputs = [candidatePath, receiptPath, nativeShadowPath, versionedCandidatePath].filter(Boolean);
  const same = (a, b) => {
    if (resolved(a) === resolved(b)) return true;
    try { const left = fs.statSync(a), right = fs.statSync(b); return left.dev === right.dev && left.ino === right.ino; }
    catch (error) { if (error.code === 'ENOENT') return false; throw error; }
  };
  for (let index = 0; index < outputs.length; ++index) {
    const output = outputs[index];
    if (typeof output !== 'string' || !path.isAbsolute(output)) throw new Error('Shadow artifact destinations must be absolute paths');
    if (protectedPaths.some(input => same(input, output)) || outputs.slice(0, index).some(other => same(other, output)))
      throw new Error('Shadow artifact destination overlaps protected production/input data or another output');
    try { const stat = fs.lstatSync(output);
      if (!stat.isFile() || stat.isSymbolicLink() || stat.nlink !== 1) throw new Error('Shadow output must be an unaliased regular file');
    } catch (error) { if (error.code !== 'ENOENT') throw error; }
  }
}

function readBounded(filename, maximum = 64 * 1024 * 1024, fileSystem = fs) {
  const fd = fileSystem.openSync(filename, fs.constants.O_RDONLY | fs.constants.O_NOFOLLOW);
  try {
    const stat = fileSystem.fstatSync(fd);
    if (!stat.isFile() || stat.size > maximum) throw new Error('Detection artifact exceeds the read limit or is not a regular file');
    const bytes = Buffer.alloc(stat.size + 1);
    let read = 0, count;
    while (read < bytes.length && (count = fileSystem.readSync(fd, bytes, read, bytes.length - read, null))) read += count;
    const after = fileSystem.fstatSync(fd);
    if (read !== stat.size || after.size !== stat.size || after.mtimeMs !== stat.mtimeMs)
      throw new Error('Detection artifact changed while reading');
    return bytes.subarray(0, read);
  } finally { fileSystem.closeSync(fd); }
}

function persistCandidate(filename, manifold) {
  const model = JSON.parse(JSON.stringify(manifold));
  // Evaluations are separate immutable ledger entries, never part of a model.
  if (model.calibration) {
    delete model.calibration.evaluation;
    delete model.calibration.independent_validation_samples;
    delete model.calibration.independent_validation_passes;
    delete model.calibration.independent_miss_passes;
  }
  const bytes = Buffer.from(JSON.stringify(model, null, 2) + '\n');
  // Publish only a complete file. Exclusive linking never overwrites an
  // existing version, including when two workers finish simultaneously.
  const temporary = `${filename}.${process.pid}.${crypto.randomBytes(8).toString('hex')}.tmp`;
  fs.writeFileSync(temporary, bytes, {mode: 0o600, flag: 'wx'});
  try {
    try { fs.linkSync(temporary, filename); }
    catch (error) {
      if (error.code !== 'EEXIST') throw error;
      if (!readBounded(filename).equals(bytes)) throw new Error('Immutable detection candidate content differs for this version');
    }
  } finally { fs.unlinkSync(temporary); }
  return crypto.createHash('sha256').update(bytes).digest('hex');
}

function appendReceipt(filename, receipt) {
  const bytes = Buffer.from(JSON.stringify(receipt) + '\n');
  const lock = `${filename}.lock`;
  const lockFd = fs.openSync(lock, 'wx', 0o600);
  try {
    const fd = fs.openSync(filename, fs.constants.O_WRONLY | fs.constants.O_APPEND | fs.constants.O_CREAT | fs.constants.O_NOFOLLOW, 0o600);
    try {
      const stat = fs.fstatSync(fd);
      if (!stat.isFile() || stat.nlink !== 1 || stat.size + bytes.length > 64 * 1024 * 1024)
        throw new Error('Shadow A/B ledger exceeds its regular-file byte bound');
      fs.writeFileSync(fd, bytes);
      fs.fsyncSync(fd);
    } finally { fs.closeSync(fd); }
  } finally { fs.closeSync(lockFd); fs.unlinkSync(lock); }
}
function publishShadow(config, paths, manifold, evaluation, baseline, candidate) {
  const version = manifold.candidate_version;
  const versionedCandidatePath = `${paths.candidatePath}.${version}.json`;
  validateArtifactDestinations(config, {...paths, versionedCandidatePath});
  for (const filename of [paths.candidatePath, paths.receiptPath]) fs.mkdirSync(path.dirname(filename), {recursive: true, mode: 0o700});
  const candidateSha256 = persistCandidate(versionedCandidatePath, manifold);
  const manifest = {schema: 'vectorwarp-detection-manifold-shadow-manifest-v1', candidate_version: version,
    candidate_path: versionedCandidatePath, candidate_sha256: candidateSha256, evidence_key: manifold.evidence_key,
    ...(paths.nativeLedgerAnchor ? {native_ledger: paths.nativeLedgerAnchor} : {}),
    updated_at: new Date().toISOString(), production_promotion: 'hard-disabled'};
  appendReceipt(paths.receiptPath, {schema: 'vectorwarp-detection-manifold-shadow-ab-receipt-v1',
    at: manifest.updated_at, candidate_version: version, candidate_path: versionedCandidatePath,
    candidate_sha256: candidateSha256, evidence_key: manifold.evidence_key, baseline, candidate,
    ...(paths.nativeLedgerAnchor ? {native_ledger: paths.nativeLedgerAnchor} : {}),
    evaluation, production_promotion: 'hard-disabled'});
  const temporary = `${paths.candidatePath}.${process.pid}.${crypto.randomBytes(8).toString('hex')}.tmp`;
  fs.writeFileSync(temporary, JSON.stringify(manifest, null, 2) + '\n', {mode: 0o600, flag: 'wx'});
  try { fs.renameSync(temporary, paths.candidatePath); }
  finally { if (fs.existsSync(temporary)) fs.unlinkSync(temporary); }
  return {candidateVersion: version, versionedCandidatePath, candidateSha256, receiptPath: paths.receiptPath};
}
module.exports = {readBounded, persistCandidate, validateArtifactDestinations, publishShadow};
